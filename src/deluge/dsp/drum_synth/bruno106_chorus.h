/*
 * BRUNO CHORUS: the Juno's BBD chorus from Ultramaster KR-106, as a Deluge Mod FX type.
 *
 * Portions Copyright (C) 2026 Karl LaRocca (Ultramaster KR-106, kayrockscreenprinting, GPL-3.0).
 * Deluge integration Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This is a modified version of KR106Chorus.h and BBDFilter.h: the two MN3009 delay lines swept in antiphase by one
 * LFO, their anti-aliasing and reconstruction filters, input saturation, Hermite reads and clock dependent gain, with
 * KR-106's measured rates and depths for modes I, II and I+II. Left out: the modelled noise floor, BBD clicks, clock
 * leakage and mains ripple. Each line takes its own channel (on the hardware both take the same mono signal), so a
 * stereo track stays stereo. Not affiliated with or endorsed by Roland.
 */

#pragma once

#include "dsp/stereo_sample.h"
#include <cstdint>
#include <span>

namespace deluge::dsp::drum_synth::bruno106 {

/// The chorus modes, as the Juno's buttons
enum class ChorusMode : uint8_t { I = 1, II = 2, I_II = 3 };

class Chorus {
public:
	/// Floats of delay memory it needs: two lines of 512 (the Mod FX buffer, 512 stereo samples, holds exactly that)
	static constexpr int32_t kMemoryFloats = 1024;

	/// Start clean: memory is the delay memory, cleared by the caller
	void reset();
	/// Chorus the buffer in place. mode 1 to 3 (anything else is I). Switching modes fades out and back in over 5 ms.
	/// rate and depth 0 to 127, 64 = the mode's own: rate two octaves each way, depth none to twice the mode's.
	void process(std::span<StereoSample> buffer, float* memory, uint8_t mode, uint8_t rateKnob = 64,
	             uint8_t depthKnob = 64);
	/// Whether it has been reset since it was last off
	bool started = false;

private:
	struct Line {
		float bq1, bq2; ///< Pre filter: Butterworth low pass at 9.66 kHz (SVF)
		float p;        ///< and a pole at 20 kHz
		float qb1, qb2; ///< Post filter, the same
		float q;
	};
	void configure(uint8_t mode);
	void scale(uint8_t rate, uint8_t depth);

	Line l0_{}, l1_{};
	int32_t write_ = 0;
	float phase_ = 0.f;       ///< LFO, 0 to 1
	float inc_ = 0.f;         ///< LFO step per sample
	float depth_ = 0.f;       ///< Delay sweep, ms, smoothed towards depthTarget_
	float depthTarget_ = 0.f; ///< The mode's sweep
	float fade_ = 0.f;        ///< Wet in, 0 to 1
	bool sine_ = false;       ///< I+II's LFO is a sine (a fast vibrato); I and II are triangles
	uint8_t mode_ = 0;        ///< Running
	uint8_t pending_ = 0;     ///< Asked for: the running mode fades out first
	float baseInc_ = 0.f;     ///< The mode's own LFO step and sweep, before rate and depth
	float baseDepth_ = 0.f;
	uint8_t rate_ = 64;    ///< The rate and depth inc_ and depthTarget_ were scaled by
	uint8_t depth_k_ = 64; ///< (depth_ is the smoothed sweep)
};

} // namespace deluge::dsp::drum_synth::bruno106
