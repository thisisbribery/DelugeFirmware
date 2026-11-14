/*
 * BROPHET filter: the "old" filter of Pro-54, Cmajor's port of Native Instruments' Pro-53.
 *
 * Portions Copyright (C) 2024 Cmajor Software Ltd (Pro-54, GPL-3.0), originally Native Instruments' Pro-53.
 * Deluge integration Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This is a modified version: the filter alone, lowpass only, with the Deluge's cutoff, resonance and drive. Not
 * affiliated with or endorsed by Native Instruments, Cmajor Software or Sequential.
 */

#pragma once

#include "dsp/filter/filter.h"
#include "util/fixedpoint.h"
#include <arm_neon.h>

namespace deluge::dsp::filter {

/// Two cascaded state variable stages (24 dB), the resonance on both, the output fed back to the input through a soft
/// clipper, and a sub-audio DC blocker on the output. Its passband drops as the resonance rises, as Pro-53's does.
/// Runs in float, with the signal in units of 2^27 (a loud oscillator). Cutoff ramps across each buffer.
class BrophetFilter : public Filter<BrophetFilter> {
public:
	BrophetFilter() = default;
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
		primed_ = false;
	}

	/// One channel's state. All zeros is a valid start.
	struct State {
		float bpf, lpf;   ///< First stage
		float nbpf, nlpf; ///< Second stage
		float sat;        ///< The fed back output, after the soft clipper
		float dc;         ///< The DC blocker's running low pass
	};

	/// Coefficients for one sample: the stages' frequencies (2 pi fc / fs, clamped for stability)
	struct Coefs {
		float f, fn;
	};

	/// min / max through NEON: no compare, so no wait for the compare's flags (VFP has no scalar min or max)
	[[gnu::always_inline]] static inline float fastClamp(float x, float lo, float hi) {
		return vget_lane_f32(vmin_f32(vmax_f32(vdup_n_f32(x), vdup_n_f32(lo)), vdup_n_f32(hi)), 0);
	}

	/// The filter on one sample, in Pro-54's units (a loud oscillator about 1)
	[[gnu::always_inline]] static inline float tick(float in, State& s, Coefs c, float d, float dn) {
		const float x = in - s.sat;
		float hpf = x - (s.bpf * d + s.lpf);
		s.bpf += hpf * c.f;
		s.lpf += s.bpf * c.f + 1.0e-7f;
		const float notch = s.lpf - s.nbpf;
		hpf = notch * dn - s.nlpf;
		s.nbpf += hpf * c.fn;
		s.nlpf += s.nbpf * c.fn;
		const float y = s.nlpf;
		// Soft clipper on the fed back output: a parabola up to 4, flat beyond (Pro-54's two branches as one)
		const float yc = fastClamp(y, -4.f, 4.f);
		s.sat = 0.13335f * yc * (1.f - 0.125f * std::fabs(yc));
		const float out = y - s.dc;
		s.dc += out * kDcCoef;
		return out;
	}

	/// Drive: the input pushed into a soft clipper (tanh-like) whose ceiling rises with the drive, so a driven filter
	/// also runs hot into its own fed back clipper. wet fades the clipper in over the knob's first eighth.
	[[gnu::always_inline]] static inline float driveIn(float x, float ceil, float invCeil, float wet) {
		const float u = fastClamp(x * invCeil, -3.f, 3.f); // the curve reaches 1 at 3
		const float u2 = u * u;
		const float sat = u * (27.f + u2) / (27.f + 9.f * u2);
		return x + wet * (sat * ceil - x);
	}

	/// The stage frequencies for a cutoff (2 pi fc / fs) at this filter's resonance
	[[nodiscard]] Coefs coefsFor(float w) const {
		Coefs c;
		c.f = std::clamp(w, 0.f, fmax_);
		c.fn = std::clamp(w, 0.f, fnmax_);
		return c;
	}

	/// The DC blocker: about 0.8 Hz, as Pro-54 (pitchToFreq(-40) * 2 pi / 44100)
	static constexpr float kDcCoef = 1.1586e-4f;

private:
	template <typename Loop>
	[[gnu::always_inline]] inline void ramp(int32_t numSamples, Loop&& loop);
	template <bool kDrive>
	void runMono(q31_t* startSample, q31_t* endSample, int32_t sampleIncrement);
	template <bool kDrive>
	void runStereo(q31_t* startSample, q31_t* endSample);
	template <bool kDrive>
	void runMonoFeedback(q31_t* startSample, q31_t* endSample, int32_t sampleIncrement, FeedbackChannel& fb,
	                     q31_t gain);
	template <bool kDrive>
	void runStereoFeedback(q31_t* startSample, q31_t* endSample, FeedbackChannel& fbL, FeedbackChannel& fbR,
	                       q31_t gain);

	State l;
	State r;
	float w_;       ///< Cutoff the last buffer ended on
	float wTarget_; ///< Cutoff this buffer ramps to
	float d_, dn_;  ///< Damping of each stage
	float fmax_, fnmax_;
	float inGain_;               ///< Drive, times q31 to Pro-54 units
	float ceil_, invCeil_, wet_; ///< The drive clipper's ceiling (Pro-54 units) and how much of it is heard
	float outGain_; ///< Back to q31 (as q31_from_float takes it, 1 = full scale), less the drive, and the passband at
	                ///< no resonance brought back to unity
	bool primed_;   ///< w_ holds a cutoff (false after a reset: the first buffer starts at its target)
};

} // namespace deluge::dsp::filter
