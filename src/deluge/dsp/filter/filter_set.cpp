/*
 * Copyright © 2015-2023 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#include "dsp/filter/filter_set.h"
#include "definitions_cxx.hpp"
#include "model/global_effectable/output_stage_gain.h"

namespace deluge::dsp::filter {

q31_t tempRenderBuffer[SSI_TX_BUFFER_NUM_SAMPLES * 2]; // * 2 to accomodate stereo samples

[[gnu::hot]] void FilterSet::renderHPFLong(q31_t* startSample, q31_t* endSample, int32_t sampleIncrement,
                                           q31_t feedback) {
	if (HPFOn) {
		if (hpfMode_ == FilterMode::HPLADDER) {
			if (feedback) {
				hpfilter.ladder.filterMonoFeedback(startSample, endSample, sampleIncrement, feedbackL_, feedback);
			}
			else {
				hpfilter.ladder.filterMono(startSample, endSample, sampleIncrement);
			}
		}
		else if ((hpfMode_ == FilterMode::SVF_BAND) || (hpfMode_ == FilterMode::SVF_NOTCH)) {
			if (feedback) {
				hpfilter.svf.filterMonoFeedback(startSample, endSample, sampleIncrement, feedbackL_, feedback);
			}
			else {
				hpfilter.svf.filterMono(startSample, endSample, sampleIncrement);
			}
		}
	}
}
[[gnu::hot]] void FilterSet::renderHPFLongStereo(q31_t* startSample, q31_t* endSample, q31_t feedback) {
	if (HPFOn) {
		if (hpfMode_ == FilterMode::HPLADDER) {
			if (feedback) {
				hpfilter.ladder.filterStereoFeedback(startSample, endSample, feedbackL_, feedbackR_, feedback);
			}
			else {
				hpfilter.ladder.filterStereo(startSample, endSample);
			}
		}
		else if ((hpfMode_ == FilterMode::SVF_BAND) || (hpfMode_ == FilterMode::SVF_NOTCH)) {
			if (feedback) {
				hpfilter.svf.filterStereoFeedback(startSample, endSample, feedbackL_, feedbackR_, feedback);
			}
			else {
				hpfilter.svf.filterStereo(startSample, endSample);
			}
		}
	}
}

[[gnu::hot]] void FilterSet::renderLPFLong(q31_t* startSample, q31_t* endSample, int32_t sampleIncrement,
                                           q31_t feedback) {
	if (LPFOn) {
		if ((lpfMode_ == FilterMode::SVF_BAND) || (lpfMode_ == FilterMode::SVF_NOTCH)) {
			if (feedback) {
				lpfilter.svf.filterMonoFeedback(startSample, endSample, sampleIncrement, feedbackL_, feedback);
			}
			else {
				lpfilter.svf.filterMono(startSample, endSample, sampleIncrement);
			}
		}
		else {
			if (feedback) {
				lpfilter.ladder.filterMonoFeedback(startSample, endSample, sampleIncrement, feedbackL_, feedback);
			}
			else {
				lpfilter.ladder.filterMono(startSample, endSample, sampleIncrement);
			}
		}
	}
}

[[gnu::hot]] void FilterSet::renderLPFLongStereo(q31_t* startSample, q31_t* endSample, q31_t feedback) {
	if (LPFOn) {
		if ((lpfMode_ == FilterMode::SVF_BAND) || (lpfMode_ == FilterMode::SVF_NOTCH)) {
			if (feedback) {
				lpfilter.svf.filterStereoFeedback(startSample, endSample, feedbackL_, feedbackR_, feedback);
			}
			else {
				lpfilter.svf.filterStereo(startSample, endSample);
			}
		}
		else {
			if (feedback) {
				lpfilter.ladder.filterStereoFeedback(startSample, endSample, feedbackL_, feedbackR_, feedback);
			}
			else {
				lpfilter.ladder.filterStereo(startSample, endSample);
			}
		}
	}
}
[[gnu::hot]] void FilterSet::renderLong(q31_t* startSample, q31_t* endSample, int32_t numSamples,
                                        int32_t sampleIncrememt, q31_t feedback) {
	// FEEDBACK: round the LPF, else the HPF, else round nothing
	q31_t lpfFeedback = 0;
	q31_t hpfFeedback = 0;
	if (feedback <= 0) {
		feedbackL_ = {};
		feedback = 0;
	}
	else if (LPFOn) {
		lpfFeedback = feedback;
	}
	else if (HPFOn) {
		hpfFeedback = feedback;
	}
	else {
		feedbackLoopMono(startSample, endSample, sampleIncrememt, feedbackL_, feedback, [](q31_t x) { return x; });
		return;
	}

	switch (routing_) {
	case FilterRoute::HIGH_TO_LOW:

		renderHPFLong(startSample, endSample, sampleIncrememt, hpfFeedback);
		renderLPFLong(startSample, endSample, sampleIncrememt, lpfFeedback);

		break;
	case FilterRoute::LOW_TO_HIGH:

		renderLPFLong(startSample, endSample, sampleIncrememt, lpfFeedback);
		renderHPFLong(startSample, endSample, sampleIncrememt, hpfFeedback);

		break;

	case FilterRoute::PARALLEL:
		// render one filter in the temp buffer so we can add
		// them together
		int32_t length = endSample - startSample;
		memcpy(tempRenderBuffer, startSample, length * sizeof(q31_t));

		renderHPFLong(tempRenderBuffer, tempRenderBuffer + length, sampleIncrememt, hpfFeedback);
		renderLPFLong(startSample, endSample, sampleIncrememt, lpfFeedback);

		for (int i = 0; i < length; i++) {
			startSample[i] += tempRenderBuffer[i];
		}
		break;
	}
}
// expects to receive an interleaved stereo stream
[[gnu::hot]] void FilterSet::renderLongStereo(q31_t* startSample, q31_t* endSample, q31_t feedback) {
	// FEEDBACK: round the LPF, else the HPF, else round nothing
	q31_t lpfFeedback = 0;
	q31_t hpfFeedback = 0;
	if (feedback <= 0) {
		feedbackL_ = {};
		feedbackR_ = {};
		feedback = 0;
	}
	else if (LPFOn) {
		lpfFeedback = feedback;
	}
	else if (HPFOn) {
		hpfFeedback = feedback;
	}
	else {
		feedbackLoopStereo(
		    startSample, endSample, feedbackL_, feedbackR_, feedback, [](q31_t x) { return x; },
		    [](q31_t x) { return x; });
		return;
	}

	switch (routing_) {
	case FilterRoute::HIGH_TO_LOW:

		renderHPFLongStereo(startSample, endSample, hpfFeedback);

		renderLPFLongStereo(startSample, endSample, lpfFeedback);

		break;
	case FilterRoute::LOW_TO_HIGH:

		renderLPFLongStereo(startSample, endSample, lpfFeedback);

		renderHPFLongStereo(startSample, endSample, hpfFeedback);

		break;
	case FilterRoute::PARALLEL:
		int32_t length = endSample - startSample;

		memcpy(tempRenderBuffer, startSample, length * sizeof(q31_t));

		renderHPFLongStereo(tempRenderBuffer, tempRenderBuffer + length, hpfFeedback);

		renderLPFLongStereo(startSample, endSample, lpfFeedback);

		for (int i = 0; i < length; i++) {
			startSample[i] += tempRenderBuffer[i];
		}
		break;
	}
}

int32_t FilterSet::setConfig(q31_t lpfFrequency, q31_t lpfResonance, FilterMode lpfmode, q31_t lpfMorph,
                             q31_t hpfFrequency, q31_t hpfResonance, FilterMode hpfmode, q31_t hpfMorph,
                             q31_t filterGain, FilterRoute routing, bool adjustVolumeForHPFResonance,
                             q31_t* overallOscAmplitude) {
	LPFOn = lpfmode != FilterMode::OFF;
	HPFOn = hpfmode != FilterMode::OFF;
	lpfMode_ = lpfmode;
	hpfMode_ = hpfmode;
	routing_ = routing;
	// Insanely, having changes happen in the small bytes too often causes rustling
	hpfResonance = (hpfResonance >> 21) << 21;

	if (LPFOn) {
		if ((lpfMode_ == FilterMode::SVF_BAND) || (lpfMode_ == FilterMode::SVF_NOTCH)) {
			if (SpecificFilter(lastLPFMode_).getFamily() != FilterFamily::SVF) {
				lpfilter.svf.reset(lastLPFMode_ == FilterMode::OFF);
			}
			filterGain = lpfilter.svf.configure(lpfFrequency, lpfResonance, lpfMode_, lpfMorph, filterGain);
		}
		else {
			if (SpecificFilter(lastLPFMode_).getFamily() != FilterFamily::LP_LADDER) {
				lpfilter.ladder.reset(lastLPFMode_ == FilterMode::OFF);
			}
			filterGain = lpfilter.ladder.configure(lpfFrequency, lpfResonance, lpfMode_, lpfMorph, filterGain);
		}
		lastLPFMode_ = lpfMode_;
	}
	else {
		lastLPFMode_ = FilterMode::OFF;
	}
	// This changes the overall amplitude so that, with resonance on 50%, the amplitude is the same as it was pre June
	// 2017
	filterGain = multiply_32x32_rshift32(filterGain, kFilterSetGainAdjust) << 1;

	// HPF
	if (HPFOn) {
		if (hpfMode_ == FilterMode::HPLADDER) {
			filterGain = hpfilter.ladder.configure(hpfFrequency, hpfResonance, hpfmode, hpfMorph, filterGain);
			if (lastHPFMode_ != hpfMode_) {
				hpfilter.ladder.reset(lastHPFMode_ == FilterMode::OFF);
			}
		}
		// otherwise it's an SVF ((lpfmode == FilterMode::SVF_BAND) || (lpfmode == FilterMode::SVF_NOTCH))
		else {
			// invert the morph for the HPF so it goes high-band/notch-low
			filterGain =
			    hpfilter.svf.configure(hpfFrequency, hpfResonance, hpfmode, ((1 << 29) - 1) - hpfMorph, filterGain);
			if (lastHPFMode_ != hpfMode_) {
				hpfilter.svf.reset(lastHPFMode_ == FilterMode::OFF);
			}
		}
		lastHPFMode_ = hpfMode_;
	}
	else {
		lastHPFMode_ = FilterMode::OFF;
	}

	return filterGain;
}

} // namespace deluge::dsp::filter
