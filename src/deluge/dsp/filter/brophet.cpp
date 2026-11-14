/*
 * BROPHET filter: the "old" filter of Pro-54, Cmajor's port of Native Instruments' Pro-53.
 *
 * Portions Copyright (C) 2024 Cmajor Software Ltd (Pro-54, GPL-3.0), originally Native Instruments' Pro-53.
 * Deluge integration Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dsp/filter/brophet.h"
#include "util/functions.h"
#include <algorithm>
#include <cmath>

namespace deluge::dsp::filter {

namespace {
/// A loud oscillator, in the voice buffer, is about this: Pro-54's 1
constexpr float kUnit = 134217728.f; // 2^27

/// atan for 0 to 8 (the Deluge's tanned cutoff), within about 0.002 rad: plenty for a cutoff
float fastAtan(float x) {
	// atan(x) = pi/2 - atan(1/x) above 1
	const bool big = x > 1.f;
	const float t = big ? 1.f / x : x;
	const float t2 = t * t;
	const float a = t * (0.9998660f + t2 * (-0.3302995f + t2 * (0.1801410f + t2 * (-0.0851330f + t2 * 0.0208351f))));
	return big ? 1.5707963f - a : a;
}
} // namespace

q31_t BrophetFilter::setConfig(q31_t frequency, q31_t resonance, FilterMode mode, q31_t morph, q31_t filterGain) {
	(void)mode;
	// The Deluge's cutoff as tan(pi fc / fs), 1 = 2^28, as its ladders take it. Pro-54 works in 2 pi fc / fs.
	const float g = static_cast<float>(instantTan(lshiftAndSaturate<5>(frequency))) * (1.f / 268435456.f);
	float w = 2.f * fastAtan(std::max(g, 0.f));

	// The resonance knob, 0 to 1 (the ladders' 0 to 2^29), through Pro-53's curve
	const float knob = std::clamp(static_cast<float>(resonance) * (1.f / 536870912.f), 0.f, 1.f);
	float res;
	if (knob < 0.25f) {
		res = 2.32f * knob;
	}
	else if (knob < 0.5f) {
		res = 0.52f * knob + 0.45f;
	}
	else if (knob < 0.75f) {
		res = 0.92f * knob + 0.25f;
	}
	else {
		res = 0.036f * knob + 0.913f;
	}
	d_ = dn_ = 2.f - 2.f * res;
	fmax_ = fnmax_ = 0.7352f + 0.2930f * res * (1.3075f + res);
	// Pro-53 raises the cutoff 4.5 semitones with the resonance knob
	w *= exp2f(0.375f * knob);
	wTarget_ = w;
	if (!primed_) {
		w_ = w;
		primed_ = true;
	}

	// Drive (the morph knob, 0 to 1): up to 30 dB into a soft clipper before the filter. The clipper's ceiling rises
	// from a loud oscillator to four times that, so at the top the filter's own fed back clipper is pushed hard too.
	// The output comes down by the ceiling and a bit more: a saw driven to full lands about 4 dB above a clean one.
	// With no resonance and no drive the passband gain is 2 / (1 + 2 x 0.13335): brought back to 1.
	const float drive = std::clamp(static_cast<float>(morph) * (1.f / 2147483648.f), 0.f, 1.f);
	const float driveGain = exp2f(5.f * drive);
	ceil_ = 1.f + 3.f * drive;
	invCeil_ = 1.f / ceil_;
	wet_ = std::min(drive * 8.f, 1.f);
	inGain_ = driveGain / kUnit;
	outGain_ = kUnit * (1.f / 2147483648.f) * (1.f + 2.f * 0.13335f) * 0.5f * (1.f - 0.3f * drive) / ceil_;
	return filterGain;
}

template <typename Loop>
void BrophetFilter::ramp(int32_t numSamples, Loop&& loop) {
	// The stage frequencies ramp between their clamped ends: no clamping per sample
	const Coefs from = coefsFor(w_);
	const Coefs to = coefsFor(wTarget_);
	const float inv = (numSamples > 0) ? 1.f / static_cast<float>(numSamples) : 0.f;
	const float stepF = (to.f - from.f) * inv;
	const float stepFn = (to.fn - from.fn) * inv;
	Coefs c = from;
	loop([&]() {
		c.f += stepF;
		c.fn += stepFn;
		return c;
	});
	w_ = wTarget_;
}

namespace {
/// Back to q31, saturating, in one instruction (outGain_ already scales to its 1 = full scale)
[[gnu::always_inline]] inline q31_t toQ31(float x) {
	return q31_from_float(x);
}
} // namespace

// Each loop works on local copies of the channel state (kept in registers, not written back every sample), and comes in
// two kinds: with drive, through the drive clipper, and without, which skips it (and its divide) altogether.

template <bool kDrive>
void BrophetFilter::runMono(q31_t* startSample, q31_t* endSample, int32_t sampleIncrement) {
	const int32_t n = static_cast<int32_t>((endSample - startSample + sampleIncrement - 1) / sampleIncrement);
	ramp(n, [&](auto&& next) {
		const float d = d_, dn = dn_, inGain = inGain_, outGain = outGain_, ceil = ceil_, invCeil = invCeil_,
		            wet = wet_;
		State st = l;
		for (q31_t* s = startSample; s < endSample; s += sampleIncrement) {
			float x = static_cast<float>(*s) * inGain;
			if constexpr (kDrive) {
				x = driveIn(x, ceil, invCeil, wet);
			}
			*s = toQ31(tick(x, st, next(), d, dn) * outGain);
		}
		l = st;
	});
}

template <bool kDrive>
void BrophetFilter::runStereo(q31_t* startSample, q31_t* endSample) {
	const int32_t n = static_cast<int32_t>((endSample - startSample) >> 1);
	ramp(n, [&](auto&& next) {
		const float d = d_, dn = dn_, inGain = inGain_, outGain = outGain_, ceil = ceil_, invCeil = invCeil_,
		            wet = wet_;
		State sl = l;
		State sr = r;
		for (q31_t* s = startSample; s < endSample; s += 2) {
			const Coefs c = next();
			float xl = static_cast<float>(s[0]) * inGain;
			float xr = static_cast<float>(s[1]) * inGain;
			if constexpr (kDrive) {
				xl = driveIn(xl, ceil, invCeil, wet);
				xr = driveIn(xr, ceil, invCeil, wet);
			}
			s[0] = toQ31(tick(xl, sl, c, d, dn) * outGain);
			s[1] = toQ31(tick(xr, sr, c, d, dn) * outGain);
		}
		l = sl;
		r = sr;
	});
}

template <bool kDrive>
void BrophetFilter::runMonoFeedback(q31_t* startSample, q31_t* endSample, int32_t sampleIncrement, FeedbackChannel& fb,
                                    q31_t gain) {
	const int32_t n = static_cast<int32_t>((endSample - startSample + sampleIncrement - 1) / sampleIncrement);
	ramp(n, [&](auto&& next) {
		const float d = d_, dn = dn_, inGain = inGain_, outGain = outGain_, ceil = ceil_, invCeil = invCeil_,
		            wet = wet_;
		State st = l;
		feedbackLoopMono(startSample, endSample, sampleIncrement, fb, gain, [&](q31_t in) {
			float x = static_cast<float>(in) * inGain;
			if constexpr (kDrive) {
				x = driveIn(x, ceil, invCeil, wet);
			}
			return toQ31(tick(x, st, next(), d, dn) * outGain);
		});
		l = st;
	});
}

template <bool kDrive>
void BrophetFilter::runStereoFeedback(q31_t* startSample, q31_t* endSample, FeedbackChannel& fbL, FeedbackChannel& fbR,
                                      q31_t gain) {
	const int32_t n = static_cast<int32_t>((endSample - startSample) >> 1);
	ramp(n, [&](auto&& next) {
		const float d = d_, dn = dn_, inGain = inGain_, outGain = outGain_, ceil = ceil_, invCeil = invCeil_,
		            wet = wet_;
		State sl = l;
		State sr = r;
		Coefs c{};
		feedbackLoopStereo(
		    startSample, endSample, fbL, fbR, gain,
		    [&](q31_t in) {
			    c = next(); // left runs first on each sample, then right uses the same coefficients
			    float x = static_cast<float>(in) * inGain;
			    if constexpr (kDrive) {
				    x = driveIn(x, ceil, invCeil, wet);
			    }
			    return toQ31(tick(x, sl, c, d, dn) * outGain);
		    },
		    [&](q31_t in) {
			    float x = static_cast<float>(in) * inGain;
			    if constexpr (kDrive) {
				    x = driveIn(x, ceil, invCeil, wet);
			    }
			    return toQ31(tick(x, sr, c, d, dn) * outGain);
		    });
		l = sl;
		r = sr;
	});
}

void BrophetFilter::doFilter(q31_t* startSample, q31_t* endSample, int32_t sampleIncrement) {
	if (wet_ > 0.f) {
		runMono<true>(startSample, endSample, sampleIncrement);
	}
	else {
		runMono<false>(startSample, endSample, sampleIncrement);
	}
}

void BrophetFilter::doFilterStereo(q31_t* startSample, q31_t* endSample) {
	if (wet_ > 0.f) {
		runStereo<true>(startSample, endSample);
	}
	else {
		runStereo<false>(startSample, endSample);
	}
}

void BrophetFilter::doFilterFeedback(q31_t* startSample, q31_t* endSample, int32_t sampleIncrement, FeedbackChannel& fb,
                                     q31_t gain) {
	if (wet_ > 0.f) {
		runMonoFeedback<true>(startSample, endSample, sampleIncrement, fb, gain);
	}
	else {
		runMonoFeedback<false>(startSample, endSample, sampleIncrement, fb, gain);
	}
}

void BrophetFilter::doFilterStereoFeedback(q31_t* startSample, q31_t* endSample, FeedbackChannel& fbL,
                                           FeedbackChannel& fbR, q31_t gain) {
	if (wet_ > 0.f) {
		runStereoFeedback<true>(startSample, endSample, fbL, fbR, gain);
	}
	else {
		runStereoFeedback<false>(startSample, endSample, fbL, fbR, gain);
	}
}

} // namespace deluge::dsp::filter
