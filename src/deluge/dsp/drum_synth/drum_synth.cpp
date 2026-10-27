/*
 * Drum synth voices for the Deluge, ported from the drum, snare, cymbal and hi-hat voices of the LXR-02 firmware.
 *
 * Portions Copyright (C) 2012-2026 Erica Synths (LXR firmware, GPL-3.0-only).
 * Deluge integration Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This is a modified version of the LXR firmware voices (DrumVoice.c, Snare.c, CymbalVoice.c, HiHat.c,
 * Oscillator.c, Decay.c, snapEg.c and transientGenerator.c). It is licensed under the GNU General Public License
 * version 3, with the additional terms in the LXR firmware's NOTICE.md.
 *
 * What was changed for the Deluge:
 * - Oscillators run on the Deluge's 32-bit phase and q31 sample format instead of int16.
 * - The amp envelope, main filter, distortion and decimation are left to the Deluge's own Sound. Only the parts that
 *   make each LXR voice sound like itself are kept: the mod oscs and FM, the pitch envelope, the snap envelope, the
 *   transient generator, and the snare's own noise filter.
 * - The cymbal and hi-hat share one engine. Open and closed hats are two kit rows in the Deluge's choke mode, rather
 *   than one voice with two decays.
 * - Envelopes tick every 16 samples rather than every 64, with their rates scaled to match.
 * - Saw and square are naive rather than band-limited.
 * - The snare noise filter is a plain TPT state variable filter with low, high and band pass outputs.
 *
 * The TIA and IIA engines are not LXR code: they are in xia.cpp, and only hooked in here. Nor are the Cyc engines,
 * which are in cyc.cpp, or GIN, in gin.cpp.
 */

#include "dsp/drum_synth/drum_synth.h"
#include "util/fixedpoint.h"
#include <algorithm>
#include <cmath>
#include <limits>

extern "C" const int8_t drumSynthTransientData[deluge::dsp::drum_synth::kNumTransientSamples][2205];
// The Deluge's 256-point sine, from util/lookuptables. Declared here rather than through util/waves.h so this file
// stays free of the GUI headers that waves.h drags in, and can be unit tested on its own.
extern const int16_t sineWaveSmall[];
// The envelope rates, also from util/lookuptables
extern const int32_t releaseRateTable64[65];

namespace deluge::dsp::drum_synth {

namespace {

constexpr float kSampleRate = 44100.f;
constexpr int32_t kTickSamples = 16;
/// The LXR runs its envelopes once per 64-sample block. We tick four times as often, so rates are scaled down.
constexpr float kTickScale = kTickSamples / 64.f;
constexpr uint32_t kTransientEnd = 2205u << 20;
constexpr uint32_t kQuarterCycle = 1u << 30;
constexpr float kMaxPhaseIncrement = 2147483647.f; // Nyquist

constexpr float kSnapMax = 24.f;
constexpr float kSnapReduction = 0.2f;
constexpr float kPitchAmountFactor = 32.f;
constexpr float kNoiseMaxRate = 22000.f;
constexpr float kNoiseGain = 0.9f;
constexpr float kCutoffMin = 30.f;
constexpr float kCutoffMax = 16000.f;

int32_t toQ31(float f) {
	return static_cast<int32_t>(std::clamp(f, -1.f, 1.f) * 2147483647.f);
}

uint32_t toPhaseIncrement(float f) {
	return static_cast<uint32_t>(std::clamp(f, 0.f, kMaxPhaseIncrement));
}

/// Same as getSine() in util/waves.h: linear interpolation of the 256-point table, giving a full-range q31
int32_t sine(uint32_t phase) {
	const uint32_t index = phase >> 24;
	const int32_t strength2 = (phase >> 8) & 65535;
	const int32_t strength1 = 65536 - strength2;
	return sineWaveSmall[index] * strength1 + sineWaveSmall[index + 1] * strength2;
}

/// Full-range triangle, starting at its lowest point
int32_t triangle(uint32_t phase) {
	const uint32_t folded = std::min<uint32_t>((phase >= 0x80000000u) ? -phase : phase, 0x7FFFFFFFu);
	return static_cast<int32_t>((folded << 1) - 0x80000000u);
}

/// LXR DecayEg_calcTime(), including its x2 for the 64-sample block size
float pitchDecayFromParam(uint8_t value) {
	constexpr float k = 2 * 0.99f / (1.f - 0.99f);
	const float v = value / 127.f;
	const float perBlock = std::min((1.f - ((1.f + k) * v / (1.f + k * v))) * 2.f, 1.f);
	return perBlock * kTickScale;
}

/// LXR DecayEg_setSlope(). Clamped so 127 doesn't divide by zero.
float pitchSlopeFromParam(uint8_t value) {
	const float amount = std::min(((value / 127.f) - 0.5f) * 2, 0.99f);
	return 2 * amount / (1 - amount);
}

float ratioFromParams(uint8_t coarse, uint8_t fine) {
	const float semitones = (coarse - 64) + (fine - 64) / 64.f;
	return exp2f(semitones / 12.f);
}

} // namespace

// Patch ---------------------------------------------------------------------------------------------------------------

void Patch::setPreset(Preset preset) {
	params_[ENGINE] = static_cast<uint8_t>(Engine::DRUM);
	params_[OSC_WAVE] = static_cast<uint8_t>(Wave::SINE);
	params_[PITCH_AMOUNT] = 0;
	params_[PITCH_DECAY] = 64;
	params_[PITCH_SLOPE] = 64;
	params_[MIX_MODE] = 0;
	params_[FM_AMOUNT] = 0;
	params_[MOD_WAVE] = static_cast<uint8_t>(Wave::SINE);
	params_[MOD_COARSE] = 64;
	params_[MOD_FINE] = 64;
	params_[FM2_AMOUNT] = 0;
	params_[MOD2_WAVE] = static_cast<uint8_t>(Wave::NOISE);
	params_[MOD2_COARSE] = 64;
	params_[MOD2_FINE] = 64;
	params_[NOISE_MIX] = 0;
	params_[NOISE_PITCH] = 127;
	params_[NOISE_FILTER_TYPE] = static_cast<uint8_t>(NoiseFilter::LOW_PASS);
	params_[NOISE_CUTOFF] = 127;
	params_[NOISE_RESONANCE] = 0;
	params_[TRANS_MODE] = kTransientSnap;
	params_[TRANS_VOLUME] = 0;
	params_[TRANS_PITCH] = 64;
	setXiaPreset(Engine::TIA, 0);
	params_[ENGINE] = static_cast<uint8_t>(Engine::DRUM);
	params_[CONTROLS_VERSION] = 1;
	params_[GIN_WAVE] = static_cast<uint8_t>(gin::Wave::SINE);
	params_[GIN_MOD_MODE] = static_cast<uint8_t>(gin::ModMode::DECAY);
	params_[GIN_NOISE_ENV] = static_cast<uint8_t>(gin::NoiseEnv::EXP);
	params_[GIN_EQ_FREQ] = 64;
	params_[GIN_EQ_GAIN] = 64;
	params_[GIN_LEVEL] = 102;

	switch (preset) {
	case Preset::KICK:
		params_[MOD_COARSE] = 64 + 12;
		params_[FM_AMOUNT] = 30;
		params_[PITCH_DECAY] = 55;
		params_[PITCH_SLOPE] = 20;
		params_[PITCH_AMOUNT] = 45;
		params_[TRANS_MODE] = kTransientSnap;
		params_[TRANS_VOLUME] = 60;
		break;

	case Preset::LOW_TOM:
		params_[MOD_COARSE] = 64 + 7;
		params_[FM_AMOUNT] = 40;
		params_[PITCH_DECAY] = 70;
		params_[PITCH_SLOPE] = 40;
		params_[PITCH_AMOUNT] = 28;
		params_[TRANS_MODE] = kTransientSampleFirst;
		params_[TRANS_VOLUME] = 40;
		params_[TRANS_PITCH] = 40;
		break;

	case Preset::HIGH_TOM:
		params_[MOD_COARSE] = 64 + 19;
		params_[FM_AMOUNT] = 36;
		params_[PITCH_DECAY] = 68;
		params_[PITCH_SLOPE] = 40;
		params_[PITCH_AMOUNT] = 24;
		params_[TRANS_MODE] = kTransientSampleFirst + 1;
		params_[TRANS_VOLUME] = 40;
		params_[TRANS_PITCH] = 60;
		break;

	case Preset::SNARE:
		params_[ENGINE] = static_cast<uint8_t>(Engine::SNARE);
		params_[OSC_WAVE] = static_cast<uint8_t>(Wave::TRIANGLE);
		params_[PITCH_AMOUNT] = 34;
		params_[PITCH_DECAY] = 58;
		params_[PITCH_SLOPE] = 30;
		params_[NOISE_MIX] = 72;
		params_[NOISE_PITCH] = 127;
		params_[NOISE_FILTER_TYPE] = static_cast<uint8_t>(NoiseFilter::HIGH_PASS);
		params_[NOISE_CUTOFF] = 85; // about 2 kHz
		params_[NOISE_RESONANCE] = 25;
		params_[TRANS_MODE] = kTransientSampleFirst + 1;
		params_[TRANS_VOLUME] = 55;
		break;

	case Preset::CLOSED_HAT:
	case Preset::OPEN_HAT:
		// Two detuned squares and a fast sample-and-hold noise, after the 808's bank of square oscs
		params_[ENGINE] = static_cast<uint8_t>(Engine::METAL);
		params_[OSC_WAVE] = static_cast<uint8_t>(Wave::SQUARE);
		params_[MOD_WAVE] = static_cast<uint8_t>(Wave::SQUARE);
		params_[MOD_COARSE] = 64 + 7;
		params_[MOD_FINE] = 115;
		params_[FM_AMOUNT] = 90;
		params_[MOD2_WAVE] = static_cast<uint8_t>(Wave::NOISE);
		params_[MOD2_COARSE] = 64 + 40;
		params_[FM2_AMOUNT] = 70;
		params_[TRANS_MODE] = kTransientOffset;
		params_[TRANS_VOLUME] = 20;
		break;

	case Preset::CYMBAL:
		params_[ENGINE] = static_cast<uint8_t>(Engine::METAL);
		params_[OSC_WAVE] = static_cast<uint8_t>(Wave::SQUARE);
		params_[MOD_WAVE] = static_cast<uint8_t>(Wave::SQUARE);
		params_[MOD_COARSE] = 64 + 5;
		params_[MOD_FINE] = 90;
		params_[FM_AMOUNT] = 100;
		params_[MOD2_WAVE] = static_cast<uint8_t>(Wave::NOISE);
		params_[MOD2_COARSE] = 64 + 40;
		params_[FM2_AMOUNT] = 70;
		params_[TRANS_MODE] = kTransientOffset;
		params_[TRANS_VOLUME] = 10;
		break;

	default:
		break;
	}
}

void Patch::setXiaPreset(Engine engine, uint8_t variant) {
	params_[ENGINE] = static_cast<uint8_t>(engine);
	params_[XIA_VARIANT] = std::min<uint8_t>(variant, 2);
	params_[XIA_PUNCH] = xiaPresetFor(engine, params_[XIA_VARIANT]).punch ? 1 : 0;
}

void Patch::setCycPreset(Engine engine) {
	params_[ENGINE] = static_cast<uint8_t>(engine);
	params_[XIA_PUNCH] = 0;
}

namespace {
/// A GIN setting, 0 to 1, as a knob value with the Cycles' 8.8 resolution
int32_t ginKnob(float x) {
	return static_cast<int32_t>(std::clamp(x, 0.f, 1.f) * 0x7F00 + 0.5f);
}
/// A byte setting, 0 to 127
uint8_t ginByte(float x) {
	return static_cast<uint8_t>(std::clamp(x, 0.f, 1.f) * 127.f + 0.5f);
}
} // namespace

void Patch::setGinSound(const gin::KitSound& sound) {
	params_[ENGINE] = static_cast<uint8_t>(Engine::GIN);
	params_[GIN_WAVE] = static_cast<uint8_t>(sound.wave);
	params_[GIN_MOD_MODE] = static_cast<uint8_t>(sound.modMode);
	params_[NOISE_FILTER_TYPE] = static_cast<uint8_t>(sound.noiseFilter);
	params_[GIN_NOISE_ENV] = static_cast<uint8_t>(sound.noiseEnv);
	params_[GIN_EQ_FREQ] = ginByte(sound.eqFreq);
	// 64 is exactly flat
	params_[GIN_EQ_GAIN] = (sound.eqGain <= 0.5f) ? static_cast<uint8_t>(sound.eqGain * 128.f + 0.5f)
	                                              : static_cast<uint8_t>(64 + (sound.eqGain - 0.5f) * 126.f + 0.5f);
	params_[GIN_LEVEL] = ginByte(sound.level);
}

DrumControls ginControlsFor(const gin::KitSound& sound) {
	using S = DrumControls;
	DrumControls c;
	c.values[S::GIN_MIX] = S::fromKnob(ginKnob(sound.mix));
	c.values[S::GIN_NOISE_FREQ] = S::fromKnob(ginKnob(sound.noiseFreq));
	c.values[S::GIN_MOD_RATE] = S::fromKnob(ginKnob(sound.modRate));
	const float amount = std::clamp(sound.modAmount, -1.f, 1.f);
	c.values[S::GIN_MOD_AMOUNT] =
	    S::fromKnob(0x4000 + static_cast<int32_t>(amount * ((amount < 0.f) ? 0x4000 : 0x3F00)));
	c.values[S::GIN_DRIVE] = S::fromKnob(ginKnob(sound.drive));
	c.values[S::GIN_NOISE_DECAY] = S::fromKnob(ginKnob(sound.noiseDecay));
	c.values[S::GIN_NOISE_RES] = S::fromKnob(ginKnob(sound.noiseQ));
	c.values[S::GIN_NOISE_ATTACK] = S::fromKnob(ginKnob(sound.noiseAttack));
	c.values[S::GIN_TONE_ATTACK] = S::fromKnob(ginKnob(sound.toneAttack));
	c.values[S::COMMON_DECAY] = S::fromValue(64);
	c.values[S::GIN_TONE_DECAY] = S::fromKnob(ginKnob(sound.toneDecay));
	c.values[S::GIN_EQ_FREQ] = S::fromKnob(ginKnob(sound.eqFreq));
	return c;
}

RowSetup rowSetupForGinSound(const gin::KitSound& sound, bool choke) {
	// Kit rows play at middle C
	const float semitones = 12.f * log2f(sound.hz / 261.6256f);
	const int32_t whole = static_cast<int32_t>(std::floor(semitones + 0.5f));
	const int32_t cents = static_cast<int32_t>(std::floor((semitones - whole) * 100.f + 0.5f));
	// The envelope opens for the engine's own envelopes, its release fitted to the longer layer heard, as for the
	// other engines (envelopeReleaseFor())
	auto attack = [](float x) { return (x <= 0.f) ? 0.f : x * 0.03981072f * powf(10.f, 2.4f * x); };
	auto decay = [](float x) { return 0.01f * powf(1000.f, x); };
	float t60 = 0.f;
	if (sound.mix < 1.f) {
		t60 = attack(sound.toneAttack) + decay(sound.toneDecay);
	}
	if (sound.mix > 0.f) {
		const float noise = attack(sound.noiseAttack) + decay(sound.noiseDecay);
		t60 = std::max(t60, (sound.noiseEnv == gin::NoiseEnv::LINEAR) ? noise * (2.f / 3.f) : noise);
	}
	RowSetup row{static_cast<int16_t>(whole), 30, -1, choke, 5, 0, 50, envelopeReleaseFor(t60)};
	row.cents = static_cast<int8_t>(cents);
	return row;
}

namespace {
/// Knob value that puts Cyc Chord's Shape on a chord shape
constexpr int32_t chordShapeKnob(int32_t shape) {
	return (shape * 128 + cyc::kNumChordShapes - 1) / cyc::kNumChordShapes;
}

/// The Cyc engines' starting knobs: Color, Shape, Sweep and Contour as the original machines start, then a longer amp
/// decay than theirs
constexpr std::array<std::array<uint8_t, 5>, static_cast<size_t>(cyc::Kind::NUM_KINDS)> kCycDefaults{{
    {10, 16, 16, 24, 50},                // Cyc Kik
    {0, 127, 8, 0, 60},                  // Cyc Snr
    {16, 46, 48, 0, 50},                 // Cyc Metal
    {15, 38, 100, 64, 52},               // Cyc Perc
    {24, 40, 38, 52, 64},                // Cyc Tone
    {32, chordShapeKnob(3), 43, 24, 80}, // Cyc Chord
    {0, 110, 74, 80, 60},                // Vtg Snr
    {24, 25, 46, 37, 56},                // clap eng
}};
constexpr int32_t kCycDefaultDrive = 100;
} // namespace

DrumControls DrumControls::defaultsFor(Engine engine) {
	// The Model:Cycles' own defaults, except that the engine's decay holds: the Deluge envelope does the shaping
	if (isGin(engine)) {
		return ginControlsFor(gin::defaultSound());
	}
	DrumControls c;
	if (isCyc(engine)) {
		// The Cyc engines start where the originals do, their own amp decay included
		const auto& d = kCycDefaults[static_cast<size_t>(cycKind(engine))];
		for (int32_t i = 0; i < cyc::kNumMacros; i++) {
			c.values[CYC_MACRO1 + i] = fromValue(d[i]);
		}
		for (int32_t i = CYC_DRIVE + 1; i < NUM_SLOTS; i++) {
			c.values[i] = fromValue(64);
		}
		c.values[CYC_DRIVE] = fromValue(kCycDefaultDrive);
		c.values[COMMON_DECAY] = fromValue(d[4]);
		c.values[LXR_DRIVE] = fromValue(0);
		return c;
	}
	c.values[DRIVE] = fromValue(0);
	c.values[ENGINE_DECAY] = fromValue(127);
	// The LXR voices' drive and decay start neutral: no drive, decay holding
	c.values[COMMON_DECAY] = fromValue(127);
	c.values[LXR_DRIVE] = fromValue(0);
	if (engine == Engine::IIA) {
		c.values[IIA_WEIGHT] = fromValue(30);
		c.values[IIA_ALLOY] = fromValue(50);
		c.values[IIA_SHAPE] = fromValue(0);
		c.values[IIA_STRIKE] = fromValue(0);
		c.values[6] = fromValue(64);
		c.values[7] = fromValue(64);
	}
	else {
		c.values[TIA_BLENDEX1] = fromValue(0);
		c.values[TIA_BLENDEX2] = fromValue(0);
		c.values[TIA_BLENDEX3] = fromValue(0);
		c.values[TIA_FREQ1] = fromValue(64);
		c.values[TIA_FREQ2] = fromValue(64);
		c.values[TIA_FREQ3] = fromValue(64);
	}
	return c;
}

uint8_t Patch::maxValue(Param p) {
	switch (p) {
	case ENGINE:
		return static_cast<uint8_t>(Engine::NUM_ENGINES) - 1;
	case OSC_WAVE:
	case MOD_WAVE:
	case MOD2_WAVE:
		return static_cast<uint8_t>(Wave::NUM_WAVES) - 1;
	case MIX_MODE:
		return 1;
	case NOISE_FILTER_TYPE:
		return static_cast<uint8_t>(NoiseFilter::NUM_FILTERS) - 1;
	case TRANS_MODE:
		return kNumTransientModes - 1;
	case XIA_VARIANT:
		return 2;
	case GIN_WAVE:
		return static_cast<uint8_t>(gin::Wave::NUM_WAVES) - 1;
	case GIN_MOD_MODE:
		return static_cast<uint8_t>(gin::ModMode::NUM_MODES) - 1;
	case GIN_NOISE_ENV:
		return static_cast<uint8_t>(gin::NoiseEnv::NUM_ENVS) - 1;
	case XIA_PUNCH:
	case SNAPPY_DECAY:
	case CONTROLS_VERSION:
	case GIN_EQ_CONTROL:
		return 1;
	default:
		return 127;
	}
}

bool Patch::isUsedBy(Param p, Engine engine) {
	switch (p) {
	case ENGINE:
	case SNAPPY_DECAY:
		return true;
	case CONTROLS_VERSION:
	case GIN_EQ_CONTROL:
		return false;
	case OSC_WAVE:
	case TRANS_MODE:
	case TRANS_VOLUME:
	case TRANS_PITCH:
		return isLxr(engine);
	case XIA_PUNCH:
		return isXia(engine) || isCyc(engine);
	case PITCH_AMOUNT:
	case PITCH_DECAY:
	case PITCH_SLOPE:
		return engine == Engine::DRUM || engine == Engine::SNARE;
	case MIX_MODE:
		return engine == Engine::DRUM;
	case FM_AMOUNT:
	case MOD_WAVE:
	case MOD_COARSE:
	case MOD_FINE:
		return engine == Engine::DRUM || engine == Engine::METAL;
	case FM2_AMOUNT:
	case MOD2_WAVE:
	case MOD2_COARSE:
	case MOD2_FINE:
		return engine == Engine::METAL;
	case NOISE_FILTER_TYPE:
		return engine == Engine::SNARE || isGin(engine);
	case NOISE_MIX:
	case NOISE_PITCH:
	case NOISE_CUTOFF:
	case NOISE_RESONANCE:
		return engine == Engine::SNARE;
	case GIN_WAVE:
	case GIN_MOD_MODE:
	case GIN_NOISE_ENV:
	case GIN_EQ_GAIN:
	case GIN_LEVEL:
		return isGin(engine);
	case GIN_EQ_FREQ: // Now a control
		return false;
	default:
		return isXia(engine);
	}
}

void Patch::set(Param p, uint8_t value) {
	params_[p] = std::min(value, maxValue(p));
}

void Patch::clampAll() {
	for (uint8_t p = 0; p < NUM_PARAMS; p++) {
		params_[p] = std::min(params_[p], maxValue(static_cast<Param>(p)));
	}
}

void Patch::readLegacyDrumBytes(const uint8_t* bytes, int32_t length) {
	// The order the first version stored its settings in
	constexpr std::array<Param, 12> kLegacyOrder{
	    OSC_WAVE,    MOD_WAVE,    MOD_COARSE,   MOD_FINE,   FM_AMOUNT,    MIX_MODE,
	    PITCH_DECAY, PITCH_SLOPE, PITCH_AMOUNT, TRANS_MODE, TRANS_VOLUME, TRANS_PITCH,
	};
	setPreset(Preset::KICK);
	for (int32_t i = 0; i < std::min<int32_t>(length, kLegacyOrder.size()); i++) {
		params_[kLegacyOrder[i]] = bytes[i];
	}
	clampAll();
}

int32_t lxrControlSlot(Patch::Param param) {
	// Settings that no engine uses together share a slot
	switch (param) {
	case Patch::PITCH_AMOUNT: // kick, snare
	case Patch::FM2_AMOUNT:   // metal
		return 0;
	case Patch::PITCH_DECAY:
	case Patch::MOD2_COARSE:
		return 1;
	case Patch::PITCH_SLOPE:
	case Patch::MOD2_FINE:
		return 2;
	case Patch::FM_AMOUNT: // kick, metal
	case Patch::NOISE_MIX: // snare
		return 3;
	case Patch::MOD_COARSE:
	case Patch::NOISE_PITCH:
		return 4;
	case Patch::MOD_FINE:
	case Patch::NOISE_CUTOFF:
		return 5;
	case Patch::TRANS_VOLUME:
		return 6;
	case Patch::TRANS_PITCH:
		return 7;
	case Patch::NOISE_RESONANCE:
		return 8;
	default:
		return -1;
	}
}

int32_t sharedControlSlot(Engine engine, StandardSlot slot) {
	struct Shared {
		int32_t pitchEnvAmount, pitchEnvSpeed, characterA, characterB;
	};
	Shared shared;
	switch (engine) {
	case Engine::DRUM:
		shared = {lxrControlSlot(Patch::PITCH_AMOUNT), lxrControlSlot(Patch::PITCH_DECAY),
		          lxrControlSlot(Patch::FM_AMOUNT), lxrControlSlot(Patch::MOD_COARSE)};
		break;
	case Engine::SNARE:
		shared = {lxrControlSlot(Patch::PITCH_AMOUNT), lxrControlSlot(Patch::PITCH_DECAY),
		          lxrControlSlot(Patch::NOISE_MIX), lxrControlSlot(Patch::NOISE_CUTOFF)};
		break;
	case Engine::METAL:
		shared = {-1, -1, lxrControlSlot(Patch::FM_AMOUNT), lxrControlSlot(Patch::FM2_AMOUNT)};
		break;
	case Engine::TIA:
		shared = {-1, -1, DrumControls::TIA_BLENDEX1, DrumControls::TIA_FREQ1};
		break;
	case Engine::IIA:
		shared = {-1, -1, DrumControls::IIA_WEIGHT, DrumControls::IIA_ALLOY};
		break;
	case Engine::GIN:
		shared = {DrumControls::GIN_MOD_AMOUNT, DrumControls::GIN_MOD_RATE, DrumControls::GIN_MIX,
		          DrumControls::GIN_NOISE_FREQ};
		break;
	default: // The Cyc engines: Sweep, Contour, Color, Shape
		shared = {DrumControls::CYC_MACRO3, DrumControls::CYC_MACRO4, DrumControls::CYC_MACRO1,
		          DrumControls::CYC_MACRO2};
		break;
	}
	switch (slot) {
	case StandardSlot::DECAY:
		return decaySlot(engine);
	case StandardSlot::DRIVE:
		return driveSlot(engine);
	case StandardSlot::PITCH_ENV_AMOUNT:
		return shared.pitchEnvAmount;
	case StandardSlot::PITCH_ENV_SPEED:
		return shared.pitchEnvSpeed;
	case StandardSlot::CHARACTER_A:
		return shared.characterA;
	case StandardSlot::CHARACTER_B:
		return shared.characterB;
	default:
		return -1;
	}
}

namespace {
/// How long an envelope release takes at a user value, in seconds, as the Deluge works it out: the preset value through
/// the release rate table (lookupReleaseRate in util/functions), scaled by the release's neutral rate, then stepped
/// through 2^23 positions a sample
float releaseSeconds(int32_t userValue) {
	const int64_t preset = static_cast<int64_t>(userValue) * 85899345 - 2147483648LL;
	const int32_t patched = static_cast<int32_t>(preset >> 2);
	int32_t index = (patched >> 24) + 32;
	const int32_t further = static_cast<int32_t>((static_cast<uint32_t>(patched) << 7) & 2147483647u);
	int32_t rate;
	if (index < 0) {
		rate = releaseRateTable64[0];
	}
	else if (index >= 64) {
		rate = releaseRateTable64[64];
	}
	else {
		rate = (multiply_32x32_rshift32(releaseRateTable64[index + 1], further)
		        + multiply_32x32_rshift32(releaseRateTable64[index], 2147483647 - further))
		       << 1;
	}
	const int32_t perSample = multiply_32x32_rshift32(140 << 9, rate);
	return 8388608.f / static_cast<float>(std::max<int32_t>(perSample, 1)) / kSampleRate;
}
} // namespace

int8_t envelopeReleaseFor(float t60) {
	constexpr int8_t kPresetRelease = 40;
	if (t60 >= 1000.f) {
		return kPresetRelease;
	}
	// The release falls about 48 dB, steadily in dB
	const float wanted = 5.f * t60;
	for (int8_t r = kPresetRelease; r < 50; r++) {
		if (releaseSeconds(r) >= wanted) {
			return r;
		}
	}
	return 50;
}

float lxrDecaySeconds(int32_t knob) {
	return (knob >= 127) ? 1000.f : 0.03f * powf(400.f, static_cast<float>(knob) / 126.f);
}

int8_t cycEnvelopeRelease(Engine engine, int32_t decayKnob, bool punch) {
	return envelopeReleaseFor(cyc::ampDecaySeconds(cycKind(engine), static_cast<float>(decayKnob), punch));
}

namespace {
/// DXIA and DIIA's own decay at a knob of 0-127, measured: the time to fall 60 dB
float xiaDecaySeconds(int32_t knob) {
	static constexpr std::array<float, 7> kKnob{60.f, 70.f, 80.f, 90.f, 100.f, 110.f, 120.f};
	static constexpr std::array<float, 7> kSeconds{0.39f, 0.64f, 1.05f, 1.7f, 2.7f, 4.1f, 5.9f};
	if (knob >= 127) {
		return 1000.f;
	}
	const float k = std::clamp(static_cast<float>(knob), kKnob.front(), kKnob.back());
	size_t i = 0;
	while (i + 2 < kKnob.size() && k > kKnob[i + 1]) {
		i++;
	}
	const float f = (k - kKnob[i]) / (kKnob[i + 1] - kKnob[i]);
	return expf(logf(kSeconds[i]) + f * (logf(kSeconds[i + 1]) - logf(kSeconds[i])));
}

/// An LXR kit row: its own decay knob sets the length (chosen to match how long the row rang in earlier versions), the
/// release lets it ring out
RowSetup lxrRow(int16_t transpose, int8_t decayKnob, int8_t hpf, bool choke) {
	return {transpose, 30, hpf, choke, 5, 0, 50, envelopeReleaseFor(lxrDecaySeconds(decayKnob)), decayKnob};
}

/// A DXIA / DIIA preset row
XiaPreset xiaPreset(std::array<uint8_t, 8> slots, int16_t transpose, int8_t env2Decay, int8_t pitchCable) {
	return {slots,
	        true,
	        {transpose, 30, -1, false, env2Decay, pitchCable, 50, envelopeReleaseFor(xiaDecaySeconds(slots[4]))}};
}
} // namespace

RowSetup rowSetupFor(Preset preset) {
	switch (preset) {
	case Preset::KICK:
		return lxrRow(-29, 100, -1, false); // G1, about 49 Hz
	case Preset::SNARE:
		return lxrRow(-6, 90, -1, false); // F#3, about 185 Hz
	case Preset::CLOSED_HAT:
		return lxrRow(13, 67, 30, true); // C#5, about 554 Hz
	case Preset::OPEN_HAT:
		return lxrRow(13, 94, 30, true);
	case Preset::LOW_TOM:
		return lxrRow(-17, 95, -1, false); // G2
	case Preset::HIGH_TOM:
		return lxrRow(-10, 92, -1, false); // D3
	default:
		return lxrRow(8, 109, 26, false); // Cymbal, G#4
	}
}

XiaPreset xiaPresetFor(Engine engine, uint8_t variant) {
	// DXIA slots: Blendex 1-3, drive, engine decay, Freq 1-3. All three operators heard at unison is a clean sine:
	// the kick's body, dropped by envelope 2 on pitch.
	if (engine == Engine::TIA) {
		switch (variant) {
		case 1: // B: a knock, operator 1 an octave and a bit up feeding the chain
			return xiaPreset({20, 0, 0, 8, 87, 76, 64, 64}, -30, 1, 20);
		case 2: // C: long and deep, a touch more drive
			return xiaPreset({0, 0, 0, 12, 104, 64, 64, 64}, -32, 2, 16);
		default: // L: tight, a fast drop from about 200 Hz to 44 Hz
			return xiaPreset({0, 0, 0, 8, 90, 64, 64, 64}, -31, 1, 20);
		}
	}
	// DIIA slots: Weight, Alloy, Shape, drive, engine decay, Strike. A loud noise strike over the partials.
	switch (variant) {
	case 1: // I: partials falling in pitch under the strike, a zap
		return xiaPreset({53, 71, 10, 40, 75, 79, 64, 64}, 0, 5, 0);
	case 2: // H: chained sines, metallic and bright
		return xiaPreset({70, 90, 19, 20, 70, 111, 64, 64}, 0, 5, 0);
	default: // G: noise with body, a snare-ish perc
		return xiaPreset({35, 24, 65, 40, 72, 105, 64, 64}, 0, 5, 0);
	}
}

RowSetup rowSetupForEngine(Engine engine, uint8_t variant) {
	if (isLxr(engine)) {
		return rowSetupFor(presetForLxrEngine(engine));
	}
	if (isCyc(engine)) {
		const auto& d = kCycDefaults[static_cast<size_t>(cycKind(engine))];
		return {0, 30, -1, false, 5, 0, 50, cycEnvelopeRelease(engine, d[4], false)};
	}
	if (isGin(engine)) {
		return rowSetupForGinSound(gin::defaultSound(), false);
	}
	return xiaPresetFor(engine, variant).row;
}

DrumControls controlsFromPatch(const Patch& patch) {
	const Engine engine = patch.engine();
	if (isXia(engine)) {
		DrumControls controls = DrumControls::defaultsFor(engine);
		const XiaPreset preset = xiaPresetFor(engine, patch.get(Patch::XIA_VARIANT));
		for (int32_t i = 0; i < static_cast<int32_t>(preset.slots.size()); i++) {
			controls.values[i] = DrumControls::fromValue(preset.slots[i]);
		}
		return controls;
	}
	if (!isLxr(engine)) {
		return DrumControls::defaultsFor(engine);
	}
	DrumControls controls = DrumControls::defaultsFor(Engine::TIA); // neutral values for the unused slots
	for (uint8_t p = 0; p < Patch::NUM_PARAMS; p++) {
		const auto param = static_cast<Patch::Param>(p);
		const int32_t slot = lxrControlSlot(param);
		if (slot >= 0 && Patch::isUsedBy(param, engine)) {
			controls.values[slot] = DrumControls::fromValue(patch.get(param));
		}
	}
	return controls;
}

void storeControlsInPatch(const DrumControls& controls, Patch& patch) {
	const Engine engine = patch.engine();
	if (!isLxr(engine)) {
		return;
	}
	for (uint8_t p = 0; p < Patch::NUM_PARAMS; p++) {
		const auto param = static_cast<Patch::Param>(p);
		const int32_t slot = lxrControlSlot(param);
		if (slot >= 0 && Patch::isUsedBy(param, engine)) {
			patch.set(param, controls.value(slot));
		}
	}
}

// Voice ---------------------------------------------------------------------------------------------------------------

void Voice::updateSettings(const Patch& patch, const DrumControls& controls) {
	std::array<uint8_t, Patch::NUM_PARAMS> effective;
	for (uint8_t p = 0; p < Patch::NUM_PARAMS; p++) {
		const auto param = static_cast<Patch::Param>(p);
		effective[p] = std::min(patch.get(param), Patch::maxValue(param));
	}

	const Engine engine = static_cast<Engine>(effective[Patch::ENGINE]);
	// The LXR engines' continuous settings come from the controls; the patch keeps the choices
	if (isLxr(engine)) {
		for (uint8_t p = 0; p < Patch::NUM_PARAMS; p++) {
			const auto param = static_cast<Patch::Param>(p);
			const int32_t slot = lxrControlSlot(param);
			if (slot >= 0 && Patch::isUsedBy(param, engine)) {
				effective[p] = std::min(controls.value(slot), Patch::maxValue(param));
			}
		}
	}

	if (settingsValid_ && effective == settingsFrom_) {
		return;
	}
	settingsFrom_ = effective;
	settingsValid_ = true;

	Settings& s = settings_;
	s.engine = engine;
	s.oscWave = static_cast<Wave>(effective[Patch::OSC_WAVE]);
	s.modWave = static_cast<Wave>(effective[Patch::MOD_WAVE]);
	s.mod2Wave = static_cast<Wave>(effective[Patch::MOD2_WAVE]);
	s.mixMode = effective[Patch::MIX_MODE] != 0;
	s.fmAmount = effective[Patch::FM_AMOUNT] / 127.f;
	s.fm2Amount = effective[Patch::FM2_AMOUNT] / 127.f;
	s.modRatio = ratioFromParams(effective[Patch::MOD_COARSE], effective[Patch::MOD_FINE]);
	s.mod2Ratio = ratioFromParams(effective[Patch::MOD2_COARSE], effective[Patch::MOD2_FINE]);

	s.pitchDecayPerTick = pitchDecayFromParam(effective[Patch::PITCH_DECAY]);
	s.pitchSlope = pitchSlopeFromParam(effective[Patch::PITCH_SLOPE]);
	const float amount = effective[Patch::PITCH_AMOUNT] / 127.f;
	s.pitchAmount = amount * amount * kPitchAmountFactor;

	s.noiseMix = effective[Patch::NOISE_MIX] / 127.f;
	s.noisePhaseIncrement =
	    static_cast<uint32_t>(effective[Patch::NOISE_PITCH] / 127.f * kNoiseMaxRate / kSampleRate * 4294967296.f);
	s.noiseFilter = static_cast<NoiseFilter>(effective[Patch::NOISE_FILTER_TYPE]);
	const float cutoff = kCutoffMin * powf(kCutoffMax / kCutoffMin, effective[Patch::NOISE_CUTOFF] / 127.f);
	s.filterG = tanf(3.14159265f * std::min(cutoff, 0.45f * kSampleRate) / kSampleRate);
	s.filterK = 2.f - 1.95f * (effective[Patch::NOISE_RESONANCE] / 127.f);

	s.transientMode = effective[Patch::TRANS_MODE];
	s.transientVolume = effective[Patch::TRANS_VOLUME] / 127.f;
	// LXR: "range about 0.25 to 4"
	s.transientPitch = 1.f + ((effective[Patch::TRANS_PITCH] / 33.9f) - 0.75f);
	s.transientPhaseIncrement = static_cast<uint32_t>(s.transientPitch * (1 << 20));
}

void Voice::trigger(const Patch& patch) {
	(void)patch;
	pendingTrigger_ = true;
	xiaFinished_ = false;
}

void Voice::startLxr() {
	const Settings& s = settings_;
	xiaEngine_ = Engine::DRUM;

	// In offset mode the transient volume moves the start phase towards zero, softening the click an osc gets from
	// starting at its peak
	const float offset = (s.transientMode == kTransientOffset) ? 1.f - s.transientVolume : 1.f;
	main_.phase = (s.oscWave == Wave::NOISE) ? 0 : static_cast<uint32_t>(kQuarterCycle * offset);
	if (s.engine == Engine::DRUM) {
		mod_.phase = (s.modWave == Wave::NOISE) ? 0 : kQuarterCycle;
	}
	else {
		mod_.phase = 0;
	}
	mod2_.phase = 0;
	noise_.phase = 0;
	transientPhase_ = 0;
	pitchEnvelope_ = 1.f;
	snapEnvelope_ = 1.f;
	filterState1_ = 0;
	filterState2_ = 0;
	samplesUntilTick_ = 0;
}

int32_t Voice::random() {
	random_ ^= random_ << 13;
	random_ ^= random_ >> 17;
	random_ ^= random_ << 5;
	return static_cast<int32_t>(random_);
}

int32_t Voice::step(Wave w, Osc& osc, uint32_t phaseOffset) {
	const uint32_t phase = osc.phase + phaseOffset;
	const uint32_t next = osc.phase + osc.phaseIncrement;
	int32_t value;
	switch (w) {
	case Wave::SINE:
		value = sine(phase);
		break;
	case Wave::TRIANGLE:
		value = triangle(phase);
		break;
	case Wave::SAW:
		value = static_cast<int32_t>(phase);
		break;
	case Wave::SQUARE:
		value = (phase < 0x80000000u) ? std::numeric_limits<int32_t>::max() : std::numeric_limits<int32_t>::min();
		break;
	default:
		// Sample-and-hold noise, a new value each cycle, so it can be tuned like the LXR's
		if (next < osc.phase) {
			osc.held = random();
		}
		value = osc.held;
		break;
	}
	osc.phase = next;
	return value;
}

void Voice::controlTick(uint32_t basePhaseIncrement) {
	const Settings& s = settings_;

	// Pitch envelope: a linear decay bent by the slope curve (LXR DecayEg_calc). The metal engine has none.
	float envelope = 0;
	if (s.engine != Engine::METAL) {
		pitchEnvelope_ = std::max(pitchEnvelope_ - s.pitchDecayPerTick, 0.f);
		// At slope 0 the LXR curve is 0 / 0 on the first tick, so keep the divisor off zero
		envelope = (1 + s.pitchSlope) * pitchEnvelope_ / std::max(1 + s.pitchSlope * pitchEnvelope_, 1e-6f);
	}
	float pitchMod = 1 + envelope * s.pitchAmount;

	// Snap: a very fast pitch spike instead of a transient sample (LXR SnapEg_calc)
	if (s.transientMode == kTransientSnap && snapEnvelope_ > 0) {
		pitchMod += snapEnvelope_ * snapEnvelope_ * kSnapMax * s.transientVolume;
		snapEnvelope_ -= kSnapReduction * s.transientPitch * kTickScale;
	}

	const float base = static_cast<float>(basePhaseIncrement);
	main_.phaseIncrement = toPhaseIncrement(base * pitchMod);
	// The LXR's wavetable carriers get four times the FM depth of its sine
	const float carrierDepth = (s.oscWave == Wave::SINE) ? 1.f : 4.f;

	switch (s.engine) {
	case Engine::DRUM:
		mod_.phaseIncrement = toPhaseIncrement(base * s.modRatio);
		if (s.mixMode) {
			mainLevel_ = toQ31(1.f - s.fmAmount);
			modLevel_ = toQ31(s.fmAmount);
		}
		else {
			// The LXR scales the mod osc by the FM amount and then the FM depth by it again. Depth is stored divided
			// by 8 to leave room for the x4.
			fmDepth_ = toQ31(s.fmAmount * s.fmAmount * envelope * carrierDepth / 8);
		}
		break;

	case Engine::SNARE:
		noise_.phaseIncrement = s.noisePhaseIncrement;
		mainLevel_ = toQ31(1.f - s.noiseMix);
		modLevel_ = toQ31(s.noiseMix * kNoiseGain);
		// Let the filter state settle to true zero rather than drifting into denormals
		if (std::abs(filterState1_) < 1e-15f) {
			filterState1_ = 0;
		}
		if (std::abs(filterState2_) < 1e-15f) {
			filterState2_ = 0;
		}
		break;

	case Engine::METAL:
	default:
		// Like the LXR, the mod oscs follow the note but not the snap
		mod_.phaseIncrement = toPhaseIncrement(base * s.modRatio);
		mod2_.phaseIncrement = toPhaseIncrement(base * s.mod2Ratio);
		modLevel_ = toQ31(s.fmAmount);
		mod2Level_ = toQ31(s.fm2Amount);
		fmDepth_ = toQ31(carrierDepth / 8);
		break;
	}

	transientLevel_ = toQ31(s.transientVolume);
}

int32_t Voice::transientSample() {
	const uint8_t mode = settings_.transientMode;
	if (mode < kTransientSampleFirst || transientPhase_ >= kTransientEnd) {
		return 0;
	}
	const int8_t* table = drumSynthTransientData[mode - kTransientSampleFirst];
	const int32_t t = static_cast<int32_t>(table[transientPhase_ >> 20]) << 24;
	transientPhase_ += settings_.transientPhaseIncrement;
	return multiply_32x32_rshift32(t, transientLevel_); // q30
}

int32_t Voice::filterNoise(int32_t input) {
	const Settings& s = settings_;
	if (s.noiseFilter == NoiseFilter::OFF) {
		return input;
	}

	// TPT state variable filter (Zavalishin, "The Art of VA Filter Design")
	const float g = s.filterG;
	const float k = s.filterK;
	const float a1 = 1.f / (1.f + g * (g + k));
	const float a2 = g * a1;
	const float a3 = g * a2;

	const float x = input * (1.f / 2147483648.f);
	const float v3 = x - filterState2_;
	const float v1 = a1 * filterState1_ + a2 * v3;
	const float v2 = filterState2_ + a2 * filterState1_ + a3 * v3;
	filterState1_ = 2 * v1 - filterState1_;
	filterState2_ = 2 * v2 - filterState2_;

	float y;
	switch (s.noiseFilter) {
	case NoiseFilter::LOW_PASS:
		y = v2;
		break;
	case NoiseFilter::HIGH_PASS:
		y = x - k * v1 - v2;
		break;
	default:
		y = v1;
		break;
	}
	return toQ31(y);
}

void Voice::renderDrum(int32_t* out, int32_t n, int32_t& amplitude, int32_t amplitudeIncrement) {
	const Settings& s = settings_;
	for (int32_t i = 0; i < n; i++) {
		const int32_t mod = step(s.modWave, mod_);

		int32_t sample; // q30, leaving headroom to add the transient
		if (s.mixMode) {
			sample =
			    multiply_32x32_rshift32(step(s.oscWave, main_), mainLevel_) + multiply_32x32_rshift32(mod, modLevel_);
		}
		else {
			// fmDepth_ is index / 8, and a full-scale mod should move the phase by index cycles: x 2^5 here
			const uint32_t phaseOffset = static_cast<uint32_t>(multiply_32x32_rshift32(mod, fmDepth_)) << 5;
			sample = step(s.oscWave, main_, phaseOffset) >> 1;
		}

		sample = add_saturate(sample, transientSample());
		amplitude += amplitudeIncrement;
		out[i] += multiply_32x32_rshift32(sample, amplitude) << 2;
	}
}

void Voice::renderSnare(int32_t* out, int32_t n, int32_t& amplitude, int32_t amplitudeIncrement) {
	const Settings& s = settings_;
	for (int32_t i = 0; i < n; i++) {
		const int32_t noise = filterNoise(step(Wave::NOISE, noise_));
		const int32_t osc = step(s.oscWave, main_);

		int32_t sample = multiply_32x32_rshift32(osc, mainLevel_) + multiply_32x32_rshift32(noise, modLevel_);
		sample = add_saturate(sample, transientSample());
		amplitude += amplitudeIncrement;
		out[i] += multiply_32x32_rshift32(sample, amplitude) << 2;
	}
}

void Voice::renderMetal(int32_t* out, int32_t n, int32_t& amplitude, int32_t amplitudeIncrement) {
	const Settings& s = settings_;
	constexpr int32_t kOne = 1 << 30; // q30
	for (int32_t i = 0; i < n; i++) {
		// The two mods sum and clip like the LXR's saturating int16 add
		int32_t mod = multiply_32x32_rshift32(step(s.modWave, mod_), modLevel_)
		              + multiply_32x32_rshift32(step(s.mod2Wave, mod2_), mod2Level_);
		mod = std::clamp(mod, -kOne, kOne);

		// mod is q30 and fmDepth_ is index / 8: x 2^6 makes a full-scale mod move the phase by index cycles
		const uint32_t phaseOffset = static_cast<uint32_t>(multiply_32x32_rshift32(mod, fmDepth_)) << 6;
		int32_t sample = step(s.oscWave, main_, phaseOffset) >> 1;

		sample = add_saturate(sample, transientSample());
		amplitude += amplitudeIncrement;
		out[i] += multiply_32x32_rshift32(sample, amplitude) << 2;
	}
}

void Voice::render(const Patch& patch, const DrumControls& controls, int32_t* buffer, int32_t numSamples,
                   uint32_t basePhaseIncrement, int32_t amplitude, int32_t amplitudeIncrement) {
	updateSettings(patch, controls);
	if (isGin(settings_.engine)) {
		if (!ginReady_) {
			xia_.gin.reset();
			ginReady_ = true;
			cycReady_ = false;
			xiaEngine_ = Engine::DRUM;
		}
		if (pendingTrigger_) {
			pendingTrigger_ = false;
			xia_.gin.trigger();
		}
		renderGin(settingsFrom_, controls, buffer, numSamples, basePhaseIncrement, amplitude, amplitudeIncrement);
		return;
	}
	if (isCyc(settings_.engine)) {
		if (!cycReady_) {
			xia_.cyc.reset();
			cycReady_ = true;
			ginReady_ = false;
			xiaEngine_ = Engine::DRUM;
		}
		if (pendingTrigger_) {
			pendingTrigger_ = false;
			xia_.cyc.trigger();
		}
		renderCyc(settings_.engine, settingsFrom_, controls, buffer, numSamples, basePhaseIncrement, amplitude,
		          amplitudeIncrement);
		return;
	}
	if (isXia(settings_.engine)) {
		if (pendingTrigger_) {
			pendingTrigger_ = false;
			startXia(settings_.engine);
		}
		renderXia(settingsFrom_, controls, buffer, numSamples, basePhaseIncrement, amplitude, amplitudeIncrement);
		return;
	}
	if (pendingTrigger_) {
		pendingTrigger_ = false;
		startLxr();
		lxrLevel_ = 1.f;
	}
	xiaFinished_ = false;

	// The LXR voices' drive and decay. Left at neutral (no drive, decay holding) they're skipped entirely.
	const float driveKnob = static_cast<float>(controls.value(DrumControls::LXR_DRIVE)) / 127.f;
	const int32_t decayKnob = controls.value(DrumControls::COMMON_DECAY);
	if (driveKnob > 0.f || decayKnob < 127) {
		renderLxrDriven(buffer, numSamples, basePhaseIncrement, amplitude, amplitudeIncrement, driveKnob, decayKnob);
		return;
	}

	int32_t* out = buffer;
	int32_t remaining = numSamples;
	while (remaining > 0) {
		if (samplesUntilTick_ <= 0) {
			controlTick(basePhaseIncrement);
			samplesUntilTick_ = kTickSamples;
		}
		const int32_t n = std::min(remaining, samplesUntilTick_);
		samplesUntilTick_ -= n;
		remaining -= n;

		switch (settings_.engine) {
		case Engine::DRUM:
			renderDrum(out, n, amplitude, amplitudeIncrement);
			break;
		case Engine::SNARE:
			renderSnare(out, n, amplitude, amplitudeIncrement);
			break;
		default:
			renderMetal(out, n, amplitude, amplitudeIncrement);
			break;
		}
		out += n;
	}
}

void Voice::renderLxrDriven(int32_t* buffer, int32_t numSamples, uint32_t basePhaseIncrement, int32_t amplitude,
                            int32_t amplitudeIncrement, float driveKnob, int32_t decayKnob) {
	// The voice renders at unity into a scratch buffer, then gets the decay and drive, then the Deluge's gain ramp.
	// At unity amplitude the engines write their q30 samples unchanged.
	constexpr int32_t kUnity = 1 << 30;
	constexpr float kQ30 = 1073741824.f;
	const float gain = cyc::driveGainFor(driveKnob);
	// Decay: 30 ms to 12 s to fall 60 dB across the knob, 127 holds
	const float coef =
	    (decayKnob >= 127) ? 1.f : expf(-6.907755f / (0.03f * powf(400.f, decayKnob / 126.f) * kSampleRate));
	std::array<int32_t, 64> scratch;
	int32_t* out = buffer;
	int32_t remaining = numSamples;
	while (remaining > 0) {
		if (samplesUntilTick_ <= 0) {
			controlTick(basePhaseIncrement);
			samplesUntilTick_ = kTickSamples;
		}
		const int32_t n = std::min({remaining, samplesUntilTick_, static_cast<int32_t>(scratch.size())});
		samplesUntilTick_ -= n;
		remaining -= n;
		std::fill(scratch.begin(), scratch.begin() + n, 0);
		int32_t unity = kUnity;
		switch (settings_.engine) {
		case Engine::DRUM:
			renderDrum(scratch.data(), n, unity, 0);
			break;
		case Engine::SNARE:
			renderSnare(scratch.data(), n, unity, 0);
			break;
		default:
			renderMetal(scratch.data(), n, unity, 0);
			break;
		}
		for (int32_t i = 0; i < n; i++) {
			float x = static_cast<float>(scratch[i]) * (1.f / kQ30) * lxrLevel_;
			lxrLevel_ *= coef;
			if (driveKnob > 0.f) {
				x = cyc::driveCurve(x * gain);
			}
			amplitude += amplitudeIncrement;
			const int32_t sample = static_cast<int32_t>(std::clamp(x, -1.99f, 1.99f) * kQ30);
			out[i] += multiply_32x32_rshift32(sample, amplitude) << 2;
		}
		out += n;
	}
	// Once the decay has died away, so has the voice
	xiaFinished_ = lxrLevel_ < 1e-5f;
}

// XIA engines ---------------------------------------------------------------------------------------------------------

namespace {

/// The XIA engines take their pitch in 16.16 semitones, as their own phase increment tables see it. Converting the
/// Deluge's increment through those tables plays the note at the Deluge's pitch, even though the engines were made for
/// 48 kHz and the Deluge runs at 44.1 kHz. Their envelopes and other per-block steps run about 9% slower here.
int32_t xiaPitch(uint32_t basePhaseIncrement) {
	static const float kLowest = static_cast<float>(xia::phaseIncrement(0));
	if (basePhaseIncrement == 0) {
		return 0;
	}
	const float semitones = 12.f * log2f(static_cast<float>(basePhaseIncrement) / kLowest);
	return static_cast<int32_t>(std::clamp(semitones * 65536.f, 0.f, static_cast<float>(xia::kMaxPitch)));
}

/// The Cycles scales its extra knob slots to 0-0x7FFF
int32_t fullKnob(int32_t knob) {
	return (knob * 0x7FFF) / 0x7F00;
}

/// The engines' own low pass stays open: the Deluge's filters do that job
constexpr int32_t kLowPassOff = 0x7F00;

/// IIA's attack setting below 64 sets its noise strike. Strike 0-127 runs that from none to full. (Its fade-in, the
/// setting above 64, is left to the Deluge envelope.) The Cycles never sends less than 0x100.
int32_t iiaAttackForStrike(int32_t strike) {
	return 0x4000 - (strike * 0x3EA2) / 0x7F00;
}

} // namespace

void Voice::startXia(Engine engine) {
	xiaEngine_ = engine;
	cycReady_ = false;
	ginReady_ = false;
	if (engine == Engine::TIA) {
		xia::reset(xia_.tia);
	}
	else {
		xia::reset(xia_.iia);
	}
	xiaBlockPos_ = xia::kBlockSize;
	xiaTrigger_ = true;
	xiaFinished_ = false;
}

void Voice::renderXia(const std::array<uint8_t, Patch::NUM_PARAMS>& p, const DrumControls& controls, int32_t* buffer,
                      int32_t numSamples, uint32_t basePhaseIncrement, int32_t amplitude, int32_t amplitudeIncrement) {
	auto knob = [&](uint8_t slot) {
		return std::clamp<int32_t>(DrumControls::toKnob(controls.values[slot]), 0, 0x7F00);
	};
	const auto engine = static_cast<Engine>(p[Patch::ENGINE]);
	if (engine != xiaEngine_) {
		// Switched engine while the note was sounding: start the new one fresh
		startXia(engine);
	}

	int32_t* out = buffer;
	int32_t remaining = numSamples;
	while (remaining > 0) {
		if (xiaBlockPos_ >= xia::kBlockSize) {
			const int32_t pitch = xiaPitch(basePhaseIncrement);
			if (engine == Engine::TIA) {
				const xia::TiaSettings t{
				    .variant = p[Patch::XIA_VARIANT],
				    .freq = {knob(DrumControls::TIA_FREQ1), knob(DrumControls::TIA_FREQ2),
				             knob(DrumControls::TIA_FREQ3)},
				    .blendex = {fullKnob(knob(DrumControls::TIA_BLENDEX1)), fullKnob(knob(DrumControls::TIA_BLENDEX2)),
				                fullKnob(knob(DrumControls::TIA_BLENDEX3))},
				    .lowPass = kLowPassOff,
				    .drive = knob(DrumControls::DRIVE),
				    .decay = knob(DrumControls::ENGINE_DECAY),
				    .pitch = pitch,
				    .punch = p[Patch::XIA_PUNCH],
				    .gate = 0,
				    .release = 0,
				};
				xia::render(xia_.tia, t, xiaTrigger_, false, xiaBlock_.data());
				xiaFinished_ = !xia_.tia.active;
			}
			else {
				const xia::IiaSettings t{
				    .variant = p[Patch::XIA_VARIANT],
				    .weight = knob(DrumControls::IIA_WEIGHT),
				    .alloy = knob(DrumControls::IIA_ALLOY),
				    .lowPass = kLowPassOff,
				    .drive = knob(DrumControls::DRIVE),
				    .shape = knob(DrumControls::IIA_SHAPE),
				    .decay = knob(DrumControls::ENGINE_DECAY),
				    .attack = iiaAttackForStrike(knob(DrumControls::IIA_STRIKE)),
				    .pitch = pitch,
				    .punch = p[Patch::XIA_PUNCH],
				    .gate = 0,
				    .release = 0,
				};
				xia::render(xia_.iia, t, xiaTrigger_, false, xiaBlock_.data());
				xiaFinished_ = !xia_.iia.active;
			}
			xiaTrigger_ = false;
			xiaBlockPos_ = 0;
		}

		const int32_t n = std::min(remaining, xia::kBlockSize - xiaBlockPos_);
		const int32_t* in = &xiaBlock_[xiaBlockPos_];
		for (int32_t i = 0; i < n; i++) {
			amplitude += amplitudeIncrement;
			// Twice the Cycles' own voice level (it halves each voice), which puts DXIA and DIIA level with the LXR and
			// Cyc engines. Saturates rather than wrapping when driven and punched to full scale.
			const int64_t sample = (static_cast<int64_t>(in[i]) * amplitude) >> 30;
			out[i] += static_cast<int32_t>(std::clamp<int64_t>(sample, INT32_MIN, INT32_MAX));
		}
		out += n;
		xiaBlockPos_ += n;
		remaining -= n;
	}
}

// Cyc engines ---------------------------------------------------------------------------------------------------------

void Voice::renderCyc(Engine engine, const std::array<uint8_t, Patch::NUM_PARAMS>& p, const DrumControls& controls,
                      int32_t* buffer, int32_t numSamples, uint32_t basePhaseIncrement, int32_t amplitude,
                      int32_t amplitudeIncrement) {
	cyc::Settings settings;
	auto knob = [&](int32_t slot) {
		return static_cast<float>(std::clamp<int32_t>(DrumControls::toKnob(controls.values[slot]), 0, 0x7F00)) / 0x7F00;
	};
	for (int32_t i = 0; i < cyc::kNumMacros; i++) {
		settings.macro[i] = knob(DrumControls::CYC_MACRO1 + i);
	}
	settings.decay = knob(DrumControls::COMMON_DECAY);
	settings.drive = knob(DrumControls::CYC_DRIVE);
	settings.punch = p[Patch::XIA_PUNCH] != 0;

	constexpr int32_t kChunk = 64;
	std::array<int32_t, kChunk> chunk;
	int32_t* out = buffer;
	int32_t remaining = numSamples;
	while (remaining > 0) {
		const int32_t n = std::min(remaining, kChunk);
		xia_.cyc.render(cycKind(engine), settings, basePhaseIncrement, static_cast<float>(kSampleRate), chunk.data(),
		                n);
		for (int32_t i = 0; i < n; i++) {
			amplitude += amplitudeIncrement;
			// Like the XIA engines: the Cycles' level halved, which leaves it q30 like the LXR engines'
			out[i] += multiply_32x32_rshift32(chunk[i] >> 1, amplitude) << 2;
		}
		out += n;
		remaining -= n;
	}
	xiaFinished_ = xia_.cyc.finished();
}

// GIN -----------------------------------------------------------------------------------------------------------------

void Voice::renderGin(const std::array<uint8_t, Patch::NUM_PARAMS>& p, const DrumControls& controls, int32_t* buffer,
                      int32_t numSamples, uint32_t basePhaseIncrement, int32_t amplitude, int32_t amplitudeIncrement) {
	auto knob = [&](int32_t slot) {
		return static_cast<float>(std::clamp<int32_t>(DrumControls::toKnob(controls.values[slot]), 0, 0x7F00)) / 0x7F00;
	};
	/// A knob as -1 to 1 around its centre
	auto bipolar = [&](int32_t slot) {
		const int32_t d = std::clamp<int32_t>(DrumControls::toKnob(controls.values[slot]), 0, 0x7F00) - 0x4000;
		return static_cast<float>(d) / ((d < 0) ? 0x4000 : 0x3F00);
	};
	// The Decay knob moves both decays together, up to 0.6 either way: 63 times longer or shorter
	const float decayOffset = bipolar(DrumControls::COMMON_DECAY) * 0.6f;
	const uint8_t eqGain = p[Patch::GIN_EQ_GAIN];

	gin::Settings s;
	s.wave = static_cast<gin::Wave>(p[Patch::GIN_WAVE]);
	s.modMode = static_cast<gin::ModMode>(p[Patch::GIN_MOD_MODE]);
	s.noiseFilter = static_cast<gin::NoiseFilter>(p[Patch::NOISE_FILTER_TYPE]);
	s.noiseEnv = static_cast<gin::NoiseEnv>(p[Patch::GIN_NOISE_ENV]);
	s.toneAttack = knob(DrumControls::GIN_TONE_ATTACK);
	s.toneDecay = std::clamp(knob(DrumControls::GIN_TONE_DECAY) + decayOffset, 0.f, 1.f);
	s.modRate = knob(DrumControls::GIN_MOD_RATE);
	s.modAmount = bipolar(DrumControls::GIN_MOD_AMOUNT);
	s.noiseFreq = knob(DrumControls::GIN_NOISE_FREQ);
	s.noiseQ = knob(DrumControls::GIN_NOISE_RES);
	s.noiseAttack = knob(DrumControls::GIN_NOISE_ATTACK);
	s.burstSpacing = knob(DrumControls::GIN_NOISE_DECAY);
	s.noiseDecay = std::clamp(s.burstSpacing + decayOffset, 0.f, 1.f);
	s.mix = knob(DrumControls::GIN_MIX);
	s.drive = knob(DrumControls::GIN_DRIVE);
	s.eqFreq = knob(DrumControls::GIN_EQ_FREQ);
	s.eqGain = (eqGain <= 64) ? eqGain / 128.f : 0.5f + (eqGain - 64) / 126.f;
	s.level = gin::levelGain(p[Patch::GIN_LEVEL] / 127.f);

	constexpr int32_t kChunk = 64;
	std::array<int32_t, kChunk> chunk;
	int32_t* out = buffer;
	int32_t remaining = numSamples;
	while (remaining > 0) {
		const int32_t n = std::min(remaining, kChunk);
		xia_.gin.render(s, basePhaseIncrement, chunk.data(), n);
		for (int32_t i = 0; i < n; i++) {
			amplitude += amplitudeIncrement;
			// Halved like the Cyc engines, which leaves it q30 like the LXR engines'
			out[i] += multiply_32x32_rshift32(chunk[i] >> 1, amplitude) << 2;
		}
		out += n;
		remaining -= n;
	}
	xiaFinished_ = xia_.gin.finished();
}

} // namespace deluge::dsp::drum_synth
