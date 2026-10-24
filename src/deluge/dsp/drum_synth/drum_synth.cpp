/*
 * Drum synth voice for the Deluge, ported from the drum voice of the LXR-02 firmware.
 *
 * Portions Copyright (C) 2012-2026 Erica Synths (LXR firmware, GPL-3.0-only).
 * Deluge integration Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This is a modified version of the LXR firmware drum voice (DrumVoice.c, Oscillator.c, Decay.c, snapEg.c and
 * transientGenerator.c). It is licensed under the GNU General Public License version 3, with the additional terms in
 * the LXR firmware's NOTICE.md.
 *
 * What was changed for the Deluge:
 * - Oscillators run on the Deluge's 32-bit phase and q31 sample format instead of int16.
 * - The amp envelope, filter, distortion and decimation are left to the Deluge's own Sound, so only the parts that
 *   make the LXR sound like itself are kept: the mod osc and FM, the pitch envelope, the snap envelope and the
 *   transient generator.
 * - Envelopes tick every 16 samples rather than every 64, with their rates scaled to match.
 * - Saw and square are naive rather than band-limited, and noise is plain white noise.
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

namespace deluge::dsp::drum_synth {

namespace {

constexpr int32_t kTickSamples = 16;
/// The LXR runs its envelopes once per 64-sample block. We tick four times as often, so rates are scaled down.
constexpr float kTickScale = kTickSamples / 64.f;
constexpr uint32_t kTransientEnd = 2205u << 20;
constexpr uint32_t kQuarterCycle = 1u << 30;
constexpr float kMaxPhaseIncrement = 2147483647.f; // Nyquist

constexpr float kSnapMax = 24.f;
constexpr float kSnapReduction = 0.2f;
constexpr float kPitchAmountFactor = 32.f;

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

/// LXR DecayEg_calcTime(), including its x2 for the 64-sample block size.
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

} // namespace

void Patch::setPreset(Preset preset) {
	params_ = {};
	params_[OSC_WAVE] = static_cast<uint8_t>(Wave::SINE);
	params_[MOD_WAVE] = static_cast<uint8_t>(Wave::SINE);
	params_[MOD_FINE] = 64;
	params_[MIX_MODE] = 0;

	switch (preset) {
	case Preset::KICK:
		params_[MOD_COARSE] = 64 + 12;
		params_[FM_AMOUNT] = 30;
		params_[PITCH_DECAY] = 55;
		params_[PITCH_SLOPE] = 20;
		params_[PITCH_AMOUNT] = 45;
		params_[TRANS_MODE] = kTransientSnap;
		params_[TRANS_VOLUME] = 60;
		params_[TRANS_PITCH] = 64;
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
	}
	recalculate();
}

uint8_t Patch::maxValue(Param p) {
	switch (p) {
	case OSC_WAVE:
	case MOD_WAVE:
		return static_cast<uint8_t>(Wave::NUM_WAVES) - 1;
	case MIX_MODE:
		return 1;
	case TRANS_MODE:
		return kNumTransientModes - 1;
	default:
		return 127;
	}
}

void Patch::set(Param p, uint8_t value) {
	params_[p] = std::min(value, maxValue(p));
	recalculate();
}

void Patch::recalculate() {
	for (uint8_t p = 0; p < NUM_PARAMS; p++) {
		params_[p] = std::min(params_[p], maxValue(static_cast<Param>(p)));
	}

	oscWave = static_cast<Wave>(params_[OSC_WAVE]);
	modWave = static_cast<Wave>(params_[MOD_WAVE]);
	mixMode = params_[MIX_MODE] != 0;
	fmAmount = params_[FM_AMOUNT] / 127.f;

	const float semitones = (params_[MOD_COARSE] - 64) + (params_[MOD_FINE] - 64) / 64.f;
	modRatio = exp2f(semitones / 12.f);

	pitchDecayPerTick = pitchDecayFromParam(params_[PITCH_DECAY]);
	pitchSlope = pitchSlopeFromParam(params_[PITCH_SLOPE]);
	const float amount = params_[PITCH_AMOUNT] / 127.f;
	pitchAmount = amount * amount * kPitchAmountFactor;

	transientMode = params_[TRANS_MODE];
	transientVolume = params_[TRANS_VOLUME] / 127.f;
	// LXR: "range about 0.25 to 4"
	transientPitch = 1.f + ((params_[TRANS_PITCH] / 33.9f) - 0.75f);
	transientPhaseIncrement = static_cast<uint32_t>(transientPitch * (1 << 20));
}

void Voice::trigger(const Patch& patch) {
	// In offset mode the transient volume moves the start phase towards zero, softening the click a sine gets from
	// starting at its peak.
	const float offset = (patch.transientMode == kTransientOffset) ? 1.f - patch.transientVolume : 1.f;
	mainPhase_ = (patch.oscWave == Wave::NOISE) ? 0 : static_cast<uint32_t>(kQuarterCycle * offset);
	modPhase_ = (patch.modWave == Wave::NOISE) ? 0 : kQuarterCycle;
	transientPhase_ = 0;
	pitchEnvelope_ = 1.f;
	snapEnvelope_ = 1.f;
	samplesUntilTick_ = 0;
}

int32_t Voice::wave(Wave w, uint32_t phase) {
	switch (w) {
	case Wave::SINE:
		return sine(phase);
	case Wave::TRIANGLE:
		return triangle(phase);
	case Wave::SAW:
		return static_cast<int32_t>(phase);
	case Wave::SQUARE:
		return (phase < 0x80000000u) ? std::numeric_limits<int32_t>::max() : std::numeric_limits<int32_t>::min();
	default:
		noise_ ^= noise_ << 13;
		noise_ ^= noise_ >> 17;
		noise_ ^= noise_ << 5;
		return static_cast<int32_t>(noise_);
	}
}

void Voice::controlTick(const Patch& patch, uint32_t basePhaseIncrement) {
	// Pitch envelope: a linear decay bent by the slope curve (LXR DecayEg_calc)
	pitchEnvelope_ = std::max(pitchEnvelope_ - patch.pitchDecayPerTick, 0.f);
	const float s = patch.pitchSlope;
	// At slope 0 the LXR curve is 0 / 0 on the first tick, so keep the divisor off zero
	const float envelope = (1 + s) * pitchEnvelope_ / std::max(1 + s * pitchEnvelope_, 1e-6f);

	float pitchMod = 1 + envelope * patch.pitchAmount;

	// Snap: a very fast pitch spike instead of a transient sample (LXR SnapEg_calc)
	if (patch.transientMode == kTransientSnap && snapEnvelope_ > 0) {
		pitchMod += snapEnvelope_ * snapEnvelope_ * kSnapMax * patch.transientVolume;
		snapEnvelope_ -= kSnapReduction * patch.transientPitch * kTickScale;
	}

	const float base = static_cast<float>(basePhaseIncrement);
	mainPhaseIncrement_ = toPhaseIncrement(base * pitchMod);
	modPhaseIncrement_ = toPhaseIncrement(base * patch.modRatio);

	if (patch.mixMode) {
		mainLevel_ = toQ31(1.f - patch.fmAmount);
		modLevel_ = toQ31(patch.fmAmount);
	}
	else {
		// The LXR scales the mod osc by the FM amount and then the FM depth by it again, and its wavetable carriers
		// get four times the depth of its sine. depth is stored divided by 8 to leave room for that.
		float index = patch.fmAmount * patch.fmAmount * envelope;
		if (patch.oscWave != Wave::SINE) {
			index *= 4;
		}
		fmDepth_ = toQ31(index / 8);
	}
	transientLevel_ = toQ31(patch.transientVolume);
}

void Voice::render(const Patch& patch, int32_t* buffer, int32_t numSamples, uint32_t basePhaseIncrement,
                   int32_t amplitude, int32_t amplitudeIncrement) {
	const bool playTransient = patch.transientMode >= kTransientSampleFirst;
	const int8_t* transient =
	    playTransient ? drumSynthTransientData[patch.transientMode - kTransientSampleFirst] : nullptr;

	int32_t* out = buffer;
	int32_t remaining = numSamples;
	while (remaining > 0) {
		if (samplesUntilTick_ <= 0) {
			controlTick(patch, basePhaseIncrement);
			samplesUntilTick_ = kTickSamples;
		}
		const int32_t n = std::min(remaining, samplesUntilTick_);
		samplesUntilTick_ -= n;
		remaining -= n;

		for (int32_t i = 0; i < n; i++) {
			const int32_t mod = wave(patch.modWave, modPhase_);
			modPhase_ += modPhaseIncrement_;

			int32_t sample; // q30, leaving headroom to add the transient
			if (patch.mixMode) {
				sample = multiply_32x32_rshift32(wave(patch.oscWave, mainPhase_), mainLevel_)
				         + multiply_32x32_rshift32(mod, modLevel_);
			}
			else {
				// fmDepth_ is index / 8, and a full-scale mod should move the phase by index cycles: x 2^5 here
				const uint32_t phaseOffset = static_cast<uint32_t>(multiply_32x32_rshift32(mod, fmDepth_)) << 5;
				sample = wave(patch.oscWave, mainPhase_ + phaseOffset) >> 1;
			}
			mainPhase_ += mainPhaseIncrement_;

			if (playTransient && transientPhase_ < kTransientEnd) {
				const int32_t t = static_cast<int32_t>(transient[transientPhase_ >> 20]) << 24;
				sample = add_saturate(sample, multiply_32x32_rshift32(t, transientLevel_));
				transientPhase_ += patch.transientPhaseIncrement;
			}

			amplitude += amplitudeIncrement;
			*out++ += multiply_32x32_rshift32(sample, amplitude) << 2;
		}
	}
}

} // namespace deluge::dsp::drum_synth
