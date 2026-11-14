/*
 * Copyright © 2026 the Deluge community.
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

#pragma once

#include "util/fixedpoint.h"
#include "util/functions.h"

namespace deluge::dsp::filter {

/// One channel of a voice's FEEDBACK loop (params::LOCAL_VOICE_FEEDBACK): the filter's output from the sample before,
/// with its DC taken out and soft clipped, added back to the filter's input, like patching a mono synth's output back
/// into its own mixer. It fattens and drives the low end; past the knob's range (patch cables) it can self-oscillate.
/// All zeroes is the reset state.
struct FeedbackChannel {
	q31_t last; ///< The filter's last output, DC removed
	q31_t dc;   ///< Running DC of the filter's output

	/// The filter's input x with the feedback added. gain: 1 = 2^29
	[[gnu::always_inline]] inline q31_t in(q31_t x, q31_t gain) {
		q31_t fed = lshiftAndSaturate<3>(multiply_32x32_rshift32(last, gain));
		// The fed back signal is soft clipped at about 2^27, a bit under a loud oscillator, so it can't run away
		return add_saturate(x, getTanHUnknown(fed, 2));
	}

	/// Note the filter's output y for the next sample, and return it
	[[gnu::always_inline]] inline q31_t out(q31_t y) {
		dc += (y >> 7) - (dc >> 7); // About 55 Hz
		last = y - dc;
		return y;
	}
};

/// Run filter (q31_t -> q31_t, one sample) over a mono buffer inside the feedback loop
template <typename F>
[[gnu::always_inline]] inline void feedbackLoopMono(q31_t* sample, q31_t* end, int32_t increment, FeedbackChannel& fb,
                                                    q31_t gain, F&& filter) {
	do {
		*sample = fb.out(filter(fb.in(*sample, gain)));
		sample += increment;
	} while (sample < end);
}

/// Run filterL and filterR over an interleaved stereo buffer, each inside its own channel's feedback loop
template <typename FL, typename FR>
[[gnu::always_inline]] inline void feedbackLoopStereo(q31_t* sample, q31_t* end, FeedbackChannel& fbL,
                                                      FeedbackChannel& fbR, q31_t gain, FL&& filterL, FR&& filterR) {
	do {
		sample[0] = fbL.out(filterL(fbL.in(sample[0], gain)));
		sample[1] = fbR.out(filterR(fbR.in(sample[1], gain)));
		sample += 2;
	} while (sample < end);
}

} // namespace deluge::dsp::filter
