/*
 * BRUNO106 filter: the IR3109 four pole OTA filter of Ultramaster KR-106, in the Deluge's LPF list.
 *
 * Portions Copyright (C) 2026 Karl LaRocca (Ultramaster KR-106, kayrockscreenprinting, GPL-3.0).
 * Deluge integration Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This is a modified version: lowpass only, at 1x, with the Deluge's cutoff, resonance and drive (see
 * bruno106_core.h for the filter itself). Not affiliated with or endorsed by Roland.
 */

#pragma once

#include "dsp/filter/bruno106_core.h"
#include "dsp/filter/filter.h"
#include "util/fixedpoint.h"

namespace deluge::dsp::filter {

/// Four OTA-C stages with the BA662 feedback around them, each stage soft clipping as the IR3109's do. Self
/// oscillates near the top of the resonance. Runs in float, in KR-106's units (a full saw 0.6, a loud Deluge oscillator
/// being 2^27). Cutoff ramps across each buffer.
class Bruno106Filter : public Filter<Bruno106Filter> {
public:
	Bruno106Filter() = default;
	q31_t setConfig(q31_t frequency, q31_t resonance, FilterMode mode, q31_t morph, q31_t filterGain);
	void doFilter(q31_t* startSample, q31_t* endSample, int32_t sampleIncrement);
	void doFilterStereo(q31_t* startSample, q31_t* endSample);
	void doFilterFeedback(q31_t* startSample, q31_t* endSample, int32_t sampleIncrement, FeedbackChannel& fb,
	                      q31_t gain);
	void doFilterStereoFeedback(q31_t* startSample, q31_t* endSample, FeedbackChannel& fbL, FeedbackChannel& fbR,
	                            q31_t gain);
	void resetFilter() {
		l = {};
		r = {};
		l.seed = 123456789u;
		r.seed = 987654321u;
		primed_ = false;
		cached_ = false;
	}

private:
	/// g1 and invDen step across a buffer, from where the last ended to this one's target
	struct Ramp {
		float g1, dg1, invDen, dInvDen;
	};
	[[nodiscard]] Ramp rampFor(int32_t numSamples) const;

	bruno106::State l;
	bruno106::State r;
	bruno106::Fixed fixed_;
	float g1_, invDen_;             ///< Where the last buffer ended
	float g1Target_, invDenTarget_; ///< Where this one ends
	float inGain_;                  ///< q31 to KR-106 units, with the drive
	float outGain_;                 ///< Back to q31 (as q31_from_float takes it), the passband brought back to unity
	bool primed_;                   ///< g1_ holds a cutoff (false after a reset: the first buffer starts at its target)
	bool cached_;                   ///< The coefficients below are for lastFrequency_, lastResonance_ and lastMorph_
	q31_t lastFrequency_, lastResonance_, lastMorph_;
	bruno106::ResCache res_; ///< For lastResonance_
};

} // namespace deluge::dsp::filter
