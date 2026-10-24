/*
 * Drum synth voices for the Deluge, ported from the drum, snare, cymbal and hi-hat voices of the LXR-02 firmware.
 *
 * Portions Copyright (C) 2012-2026 Erica Synths (LXR firmware, GPL-3.0-only).
 * Deluge integration Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This is a modified version of the LXR firmware drum voices. It is licensed under the GNU General Public License
 * version 3, with the additional terms in the LXR firmware's NOTICE.md: modified versions must be marked as
 * modified, and no rights are granted to the "Erica Synths", "LXR" or "LXR-02" trademarks.
 */

#pragma once

#include "dsp/drum_synth/xia.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>

namespace deluge::dsp::drum_synth {

/// The three LXR voice structures, then two engines from the XIA set for the Model:Cycles (see xia.h)
enum class Engine : uint8_t {
	DRUM,  ///< Kick and toms: main osc, FM or mixed mod osc, pitch envelope
	SNARE, ///< Pitched osc with pitch envelope, plus filtered sample-and-hold noise
	METAL, ///< Cymbal and hi-hat: main osc frequency modulated by two more oscs, one usually noise
	TIA,   ///< XIA TIA: three sines phase modulating a carrier
	IIA,   ///< XIA IIA: inharmonic partials with a noise strike
	NUM_ENGINES,
};

/// Whether an engine is one of the XIA engines, whose settings live on Deluge params (see XiaControls)
constexpr bool isXia(Engine engine) {
	return engine == Engine::TIA || engine == Engine::IIA;
}

/// The XIA engines' continuous settings. They live on Deluge patched params that a non-FM sound doesn't use, so they
/// can be automated, parameter locked and put on gold knobs. Each slot means something per engine.
struct XiaControls {
	enum Slot : uint8_t {
		TIA_BLENDEX1 = 0, ///< Operator 1 from heard (0) to modulating (127)
		TIA_BLENDEX2 = 1,
		TIA_BLENDEX3 = 2,
		DRIVE = 3,        ///< Both engines: wavefolder, then a saturator at the top of the range
		ENGINE_DECAY = 4, ///< Both engines: their own decay on top of the Deluge envelope. 127 holds
		TIA_FREQ1 = 5,    ///< Operator 1 tuning, 64 = unison, 0 / 127 = 24 semitones down / up
		TIA_FREQ2 = 6,
		TIA_FREQ3 = 7,
		IIA_WEIGHT = 0, ///< Partial ratios
		IIA_ALLOY = 1,  ///< Partial levels, or modulation depth on H
		IIA_SHAPE = 2,  ///< Partial waveform, sine > triangle > square > saw
		IIA_STRIKE = 5, ///< Level of the noise burst at the start of each hit
		NUM_SLOTS = 8,
	};
	/// Raw Deluge param values: the whole int32 range, centre 0
	std::array<int32_t, NUM_SLOTS> values{};

	/// Raw param value to an 8.8 knob value as the Cycles uses: 0 to 0x7F00, with the centre at exactly 0x4000 (64)
	static constexpr int32_t toKnob(int32_t raw) {
		const int64_t scale = (raw < 0) ? 0x4000 : 0x3F00;
		const int64_t offset = (static_cast<int64_t>(raw) * scale + (int64_t{1} << 30)) >> 31;
		return static_cast<int32_t>(0x4000 + offset);
	}
	/// The inverse, for setting a param to a knob value
	static constexpr int32_t fromKnob(int32_t knob) {
		const int64_t d = knob - 0x4000;
		const int64_t raw = (d << 31) / ((d < 0) ? 0x4000 : 0x3F00);
		return static_cast<int32_t>(std::clamp<int64_t>(raw, INT32_MIN, INT32_MAX));
	}
	/// A whole number knob value, 0 to 127
	static constexpr int32_t fromValue(int32_t value) { return fromKnob(value << 8); }

	/// The Cycles' own defaults for an engine, as raw param values
	static XiaControls defaultsFor(Engine engine);
};

enum class Wave : uint8_t { SINE, TRIANGLE, SAW, SQUARE, NOISE, NUM_WAVES };

enum class NoiseFilter : uint8_t { LOW_PASS, HIGH_PASS, BAND_PASS, OFF, NUM_FILTERS };

/// Starting points for a drum synth row, in the order a new drum synth kit lays them out
enum class Preset : uint8_t { KICK, SNARE, CLOSED_HAT, OPEN_HAT, LOW_TOM, HIGH_TOM, CYMBAL, NUM_PRESETS };

/// Transient modes. Values from kTransientSampleFirst up pick one of the LXR transient samples.
constexpr uint8_t kTransientSnap = 0;
constexpr uint8_t kTransientOffset = 1;
constexpr uint8_t kTransientSampleFirst = 2;
constexpr uint8_t kNumTransientSamples = 12;
constexpr uint8_t kNumTransientModes = kTransientSampleFirst + kNumTransientSamples;

/// The editable settings of one drum synth voice. Everything is stored as a small integer, mostly 0-127 like the
/// LXR stores its kits, so a whole patch saves as a short run of hex bytes. Settings an engine doesn't use are kept,
/// so switching engine and back loses nothing.
class Patch {
public:
	enum Param : uint8_t {
		ENGINE,            ///< Engine
		OSC_WAVE,          ///< Wave of the main osc
		PITCH_AMOUNT,      ///< Pitch envelope depth, up to 32x the base frequency (drum, snare)
		PITCH_DECAY,       ///< Pitch envelope decay time (drum, snare)
		PITCH_SLOPE,       ///< Pitch envelope curve, 64 = linear, lower = falls away faster (drum, snare)
		MIX_MODE,          ///< 0 = mod osc frequency modulates the main osc, 1 = the two are mixed (drum)
		FM_AMOUNT,         ///< FM depth, or mod osc level in mix mode (drum, metal)
		MOD_WAVE,          ///< Wave of the mod osc (drum, metal)
		MOD_COARSE,        ///< Mod osc semitones above the main osc, 64 = unison (drum, metal)
		MOD_FINE,          ///< Mod osc detune, 64 = none, 0 / 127 = a semitone down / up (drum, metal)
		FM2_AMOUNT,        ///< Second mod osc FM depth (metal)
		MOD2_WAVE,         ///< Wave of the second mod osc (metal)
		MOD2_COARSE,       ///< Second mod osc semitones above the main osc (metal)
		MOD2_FINE,         ///< Second mod osc detune (metal)
		NOISE_MIX,         ///< Noise level against the osc (snare)
		NOISE_PITCH,       ///< Noise sample-and-hold rate, 127 = 22 kHz (snare)
		NOISE_FILTER_TYPE, ///< NoiseFilter (snare)
		NOISE_CUTOFF,      ///< Noise filter cutoff, 30 Hz to 16 kHz (snare)
		NOISE_RESONANCE,   ///< Noise filter resonance (snare)
		TRANS_MODE,        ///< kTransientSnap, kTransientOffset, or a transient sample
		TRANS_VOLUME,      ///< Transient level
		TRANS_PITCH,       ///< Transient speed
		// XIA engines. Their continuous settings are on Deluge params: see XiaControls.
		XIA_VARIANT, ///< The Cycles variant: TIA L, B, C or IIA G, I, H
		XIA_PUNCH,   ///< 1 = a soft clipper on the output, and a shorter engine decay
		NUM_PARAMS,
	};

	Patch() { setPreset(Preset::KICK); }

	void setPreset(Preset preset);
	/// Switch to an XIA engine and variant. Its continuous settings are on Deluge params: see XiaControls.
	void setXiaPreset(Engine engine, uint8_t variant);

	[[nodiscard]] uint8_t get(Param p) const { return params_[p]; }
	void set(Param p, uint8_t value);
	[[nodiscard]] Engine engine() const { return static_cast<Engine>(params_[ENGINE]); }
	[[nodiscard]] static uint8_t maxValue(Param p);
	/// Whether an engine reads this setting, for hiding the ones that do nothing
	[[nodiscard]] static bool isUsedBy(Param p, Engine engine);

	/// Raw bytes, for saving and loading. Out of range values are clamped when read.
	std::array<uint8_t, NUM_PARAMS>& bytes() { return params_; }
	void clampAll();

	/// Load the 12 bytes saved by the first, kick-and-tom-only version of the drum synth
	void readLegacyDrumBytes(const uint8_t* bytes, int32_t length);

private:
	std::array<uint8_t, NUM_PARAMS> params_{};
};

/// The four drum settings that sit on gold knobs. Each is a bipolar offset from the Deluge's patched param system:
/// 0 leaves the patch value alone and a full knob turn (+- 2^30) moves it across its whole range.
/// Which setting each one moves depends on the engine; see targetFor().
struct KnobOffsets {
	enum Slot : uint8_t { TONE, PITCH_AMOUNT, PITCH_DECAY, TRANSIENT, NUM_SLOTS };
	std::array<int32_t, NUM_SLOTS> values{};

	/// The patch setting a knob moves for an engine
	static Patch::Param targetFor(Slot slot, Engine engine);
};

/// The running state of one drum synth voice. Small and allocation-free, so every Deluge voice can carry one.
class Voice {
public:
	void trigger(const Patch& patch, const KnobOffsets& knobs);

	/// Adds numSamples of output into buffer. basePhaseIncrement is the Deluge's pitch for this note, before the
	/// drum's pitch envelope. amplitude / amplitudeIncrement are the source's gain ramp, as for the other oscillators.
	/// xiaControls are read by the XIA engines only.
	void render(const Patch& patch, const KnobOffsets& knobs, int32_t* buffer, int32_t numSamples,
	            uint32_t basePhaseIncrement, int32_t amplitude, int32_t amplitudeIncrement,
	            const XiaControls& xiaControls = {});

	/// True once an XIA engine's own envelope has finished, so the Deluge voice can be freed
	[[nodiscard]] bool finished() const { return xiaFinished_; }

private:
	/// Patch values after knob offsets, turned into the numbers the render loop wants
	struct Settings {
		Engine engine;
		Wave oscWave, modWave, mod2Wave;
		bool mixMode;
		float fmAmount, fm2Amount;
		float modRatio, mod2Ratio;
		float pitchDecayPerTick, pitchSlope, pitchAmount;
		float noiseMix;
		uint32_t noisePhaseIncrement;
		NoiseFilter noiseFilter;
		float filterG, filterK;
		uint8_t transientMode;
		float transientVolume, transientPitch;
		uint32_t transientPhaseIncrement;
	};

	struct Osc {
		uint32_t phase = 0;
		uint32_t phaseIncrement = 0;
		int32_t held = 0; ///< Current sample-and-hold value, for noise
	};

	void renderXia(const std::array<uint8_t, Patch::NUM_PARAMS>& p, const XiaControls& controls, int32_t* buffer,
	               int32_t numSamples, uint32_t basePhaseIncrement, int32_t amplitude, int32_t amplitudeIncrement);
	void startXia(Engine engine);

	void updateSettings(const Patch& patch, const KnobOffsets& knobs);
	void controlTick(uint32_t basePhaseIncrement);
	int32_t step(Wave w, Osc& osc, uint32_t phaseOffset = 0);
	int32_t random();
	int32_t filterNoise(int32_t input);

	void renderDrum(int32_t* out, int32_t n, int32_t& amplitude, int32_t amplitudeIncrement);
	void renderSnare(int32_t* out, int32_t n, int32_t& amplitude, int32_t amplitudeIncrement);
	void renderMetal(int32_t* out, int32_t n, int32_t& amplitude, int32_t amplitudeIncrement);
	int32_t transientSample();

	Settings settings_{};
	std::array<uint8_t, Patch::NUM_PARAMS> settingsFrom_{}; ///< Effective bytes settings_ was built from
	bool settingsValid_ = false;

	Osc main_, mod_, mod2_, noise_;
	uint32_t transientPhase_ = 0;
	int32_t fmDepth_ = 0;        ///< q31, FM index / 8
	int32_t mainLevel_ = 0;      ///< q31
	int32_t modLevel_ = 0;       ///< q31
	int32_t mod2Level_ = 0;      ///< q31
	int32_t transientLevel_ = 0; ///< q31
	float pitchEnvelope_ = 0;
	float snapEnvelope_ = 0;
	float filterState1_ = 0;
	float filterState2_ = 0;
	uint32_t random_ = 0x12345678;
	int32_t samplesUntilTick_ = 0;

	// XIA engines render whole blocks; samples left over from the last one wait here
	union {
		xia::TiaState tia;
		xia::IiaState iia;
	} xia_{};
	std::array<int32_t, xia::kBlockSize> xiaBlock_{};
	int32_t xiaBlockPos_ = xia::kBlockSize;
	Engine xiaEngine_ = Engine::DRUM; ///< The XIA engine xia_ holds, or DRUM for none
	bool xiaTrigger_ = false;
	bool xiaFinished_ = false;
};

} // namespace deluge::dsp::drum_synth
