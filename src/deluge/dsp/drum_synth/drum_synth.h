/*
 * Drum synth voice for the Deluge, ported from the drum voice of the LXR-02 firmware.
 *
 * Portions Copyright (C) 2012-2026 Erica Synths (LXR firmware, GPL-3.0-only).
 * Deluge integration Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This is a modified version of the LXR firmware drum voice. It is licensed under the GNU General Public License
 * version 3, with the additional terms in the LXR firmware's NOTICE.md: modified versions must be marked as
 * modified, and no rights are granted to the "Erica Synths", "LXR" or "LXR-02" trademarks.
 */

#pragma once

#include <array>
#include <cstdint>

namespace deluge::dsp::drum_synth {

enum class Wave : uint8_t { SINE, TRIANGLE, SAW, SQUARE, NOISE, NUM_WAVES };

/// Starting points for a new drum synth row.
enum class Preset : uint8_t { KICK, LOW_TOM, HIGH_TOM };

/// Transient modes. Values from SAMPLE_FIRST up pick one of the LXR transient samples.
constexpr uint8_t kTransientSnap = 0;
constexpr uint8_t kTransientOffset = 1;
constexpr uint8_t kTransientSampleFirst = 2;
constexpr uint8_t kNumTransientSamples = 12;
constexpr uint8_t kNumTransientModes = kTransientSampleFirst + kNumTransientSamples;

/// The editable settings of one drum synth voice. Everything is stored as a 0-127 value, like the LXR stores its
/// kits, so a whole patch saves as a short run of hex bytes.
class Patch {
public:
	enum Param : uint8_t {
		OSC_WAVE,     ///< Wave
		MOD_WAVE,     ///< Wave
		MOD_COARSE,   ///< Mod osc semitones above the main osc, 64 = unison
		MOD_FINE,     ///< Mod osc detune, 64 = none, 0 / 127 = a semitone down / up
		FM_AMOUNT,    ///< FM depth, or mod osc level in mix mode
		MIX_MODE,     ///< 0 = mod osc frequency modulates the main osc, 1 = the two oscs are mixed
		PITCH_DECAY,  ///< Pitch envelope decay time
		PITCH_SLOPE,  ///< Pitch envelope curve, 64 = linear, lower = falls away faster
		PITCH_AMOUNT, ///< Pitch envelope depth, up to 32x the base frequency
		TRANS_MODE,   ///< kTransientSnap, kTransientOffset, or a transient sample
		TRANS_VOLUME, ///< Transient level
		TRANS_PITCH,  ///< Transient speed
		NUM_PARAMS,
	};

	Patch() { setPreset(Preset::KICK); }

	void setPreset(Preset preset);

	[[nodiscard]] uint8_t get(Param p) const { return params_[p]; }
	void set(Param p, uint8_t value);
	[[nodiscard]] static uint8_t maxValue(Param p);

	/// Raw bytes, for saving and loading. Call recalculate() after writing to these directly.
	std::array<uint8_t, NUM_PARAMS>& bytes() { return params_; }
	void recalculate();

	// Values derived from params_, read by Voice on every control tick
	Wave oscWave;
	Wave modWave;
	bool mixMode;
	float fmAmount;
	float modRatio;
	float pitchDecayPerTick;
	float pitchSlope;
	float pitchAmount;
	uint8_t transientMode;
	float transientVolume;
	float transientPitch;
	uint32_t transientPhaseIncrement;

private:
	std::array<uint8_t, NUM_PARAMS> params_{};
};

/// The running state of one drum synth voice. Small and allocation-free, so every Deluge voice can carry one.
class Voice {
public:
	void trigger(const Patch& patch);

	/// Adds numSamples of output into buffer. basePhaseIncrement is the Deluge's pitch for this note, before the
	/// drum's pitch envelope. amplitude / amplitudeIncrement are the source's gain ramp, as for the other oscillators.
	void render(const Patch& patch, int32_t* buffer, int32_t numSamples, uint32_t basePhaseIncrement, int32_t amplitude,
	            int32_t amplitudeIncrement);

private:
	void controlTick(const Patch& patch, uint32_t basePhaseIncrement);
	int32_t wave(Wave w, uint32_t phase);

	uint32_t mainPhase_ = 0;
	uint32_t modPhase_ = 0;
	uint32_t transientPhase_ = 0;
	uint32_t mainPhaseIncrement_ = 0;
	uint32_t modPhaseIncrement_ = 0;
	int32_t fmDepth_ = 0;        ///< q31, phase offset per unit of mod osc
	int32_t mainLevel_ = 0;      ///< q31
	int32_t modLevel_ = 0;       ///< q31
	int32_t transientLevel_ = 0; ///< q31
	float pitchEnvelope_ = 0;
	float snapEnvelope_ = 0;
	uint32_t noise_ = 0x12345678;
	int32_t samplesUntilTick_ = 0;
};

} // namespace deluge::dsp::drum_synth
