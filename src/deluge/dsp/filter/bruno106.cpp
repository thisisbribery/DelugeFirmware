/*
 * BRUNO106 filter: the IR3109 four pole OTA filter of Ultramaster KR-106, in the Deluge's LPF list.
 *
 * Portions Copyright (C) 2026 Karl LaRocca (Ultramaster KR-106, kayrockscreenprinting, GPL-3.0).
 * Deluge integration Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dsp/filter/bruno106.h"
#include "util/functions.h"
#include <algorithm>
#include <cmath>

namespace deluge::dsp::filter {

namespace {
/// A loud oscillator in the voice buffer: KR-106's full saw, 0.6
constexpr float kBrunoUnit = 134217728.f; // 2^27
constexpr float kBrunoSaw = 0.6f;

/// atan for 0 to 8 (the Deluge's tanned cutoff), within about 0.002 rad: plenty for a cutoff
float brunoAtan(float x) {
	const bool big = x > 1.f;
	const float t = big ? 1.f / x : x;
	const float t2 = t * t;
	const float a = t * (0.9998660f + t2 * (-0.3302995f + t2 * (0.1801410f + t2 * (-0.0851330f + t2 * 0.0208351f))));
	return big ? 1.5707963f - a : a;
}

[[gnu::always_inline]] inline q31_t brunoToQ31(float x) {
	return q31_from_float(x);
}
} // namespace

q31_t Bruno106Filter::setConfig(q31_t frequency, q31_t resonance, FilterMode mode, q31_t morph, q31_t filterGain) {
	(void)mode;
	// The thermal noise follows the state every buffer. The rest is worked out only for what moved: an envelope
	// sweeping the cutoff skips the resonance and drive maths, and a held note skips it all.
	fixed_.noise = bruno106::noiseFor(l); // the left channel stands for both
	const bool resMoved = !cached_ || resonance != lastResonance_;
	const bool morphMoved = !cached_ || morph != lastMorph_;
	if (!resMoved && !morphMoved && frequency == lastFrequency_) {
		g1_ = g1Target_;
		invDen_ = invDenTarget_;
		return filterGain;
	}
	cached_ = true;
	lastFrequency_ = frequency;
	lastResonance_ = resonance;
	lastMorph_ = morph;

	if (resMoved) {
		// The resonance knob, 0 to 1 (the ladders' 0 to 2^29), as KR-106's slider
		res_ = bruno106::resCache(std::clamp(static_cast<float>(resonance) * (1.f / 536870912.f), 0.f, 1.f));
	}
	// The Deluge's cutoff as tan(pi fc / fs), 1 = 2^28, as its ladders take it: KR-106's g. Its frq is fc over Nyquist.
	const float g =
	    std::max(static_cast<float>(instantTan(lshiftAndSaturate<5>(frequency))) * (1.f / 268435456.f), 0.f);
	const float frq = brunoAtan(g) * (2.f / 3.14159265f);
	const float noise = fixed_.noise;
	g1Target_ = bruno106::freqCoefs(frq, g, res_, fixed_, invDenTarget_);
	fixed_.noise = noise;
	if (!primed_) {
		g1_ = g1Target_;
		invDen_ = invDenTarget_;
		primed_ = true;
	}

	if (morphMoved) {
		// Drive (the morph knob, 0 to 1): up to 18 dB into the filter; 12 dB of it taken back off after
		const float drive = std::clamp(static_cast<float>(morph) * (1.f / 2147483648.f), 0.f, 1.f);
		inGain_ = exp2f(3.f * drive) * kBrunoSaw / kBrunoUnit;
		// At no resonance the passband is kDirectGain x kOutGain = 1.22 of the input: brought back to 1
		outGain_ = kBrunoUnit / kBrunoSaw / (bruno106::kDirectGain * bruno106::kOutGain) * (1.f / 2147483648.f)
		           * exp2f(-2.f * drive);
	}
	return filterGain;
}

Bruno106Filter::Ramp Bruno106Filter::rampFor(int32_t numSamples) const {
	const float inv = (numSamples > 0) ? 1.f / static_cast<float>(numSamples) : 0.f;
	return {g1_, (g1Target_ - g1_) * inv, invDen_, (invDenTarget_ - invDen_) * inv};
}

// Each loop works on local copies of the state and coefficients, kept in registers

void Bruno106Filter::doFilter(q31_t* startSample, q31_t* endSample, int32_t sampleIncrement) {
	const int32_t n = static_cast<int32_t>((endSample - startSample + sampleIncrement - 1) / sampleIncrement);
	Ramp rp = rampFor(n);
	const bruno106::Fixed f = fixed_;
	const float inGain = inGain_, outGain = outGain_;
	bruno106::State st = l;
	for (q31_t* s = startSample; s < endSample; s += sampleIncrement) {
		rp.g1 += rp.dg1;
		rp.invDen += rp.dInvDen;
		*s = brunoToQ31(bruno106::tick(static_cast<float>(*s) * inGain, st, rp.g1, rp.invDen, f) * outGain);
	}
	l = st;
	g1_ = g1Target_;
	invDen_ = invDenTarget_;
}

void Bruno106Filter::doFilterStereo(q31_t* startSample, q31_t* endSample) {
	const int32_t n = static_cast<int32_t>((endSample - startSample) >> 1);
	Ramp rp = rampFor(n);
	const bruno106::Fixed f = fixed_;
	const float inGain = inGain_, outGain = outGain_;
	bruno106::State sl = l;
	bruno106::State sr = r;
	for (q31_t* s = startSample; s < endSample; s += 2) {
		rp.g1 += rp.dg1;
		rp.invDen += rp.dInvDen;
		s[0] = brunoToQ31(bruno106::tick(static_cast<float>(s[0]) * inGain, sl, rp.g1, rp.invDen, f) * outGain);
		s[1] = brunoToQ31(bruno106::tick(static_cast<float>(s[1]) * inGain, sr, rp.g1, rp.invDen, f) * outGain);
	}
	l = sl;
	r = sr;
	g1_ = g1Target_;
	invDen_ = invDenTarget_;
}

void Bruno106Filter::doFilterFeedback(q31_t* startSample, q31_t* endSample, int32_t sampleIncrement,
                                      FeedbackChannel& fb, q31_t gain) {
	const int32_t n = static_cast<int32_t>((endSample - startSample + sampleIncrement - 1) / sampleIncrement);
	Ramp rp = rampFor(n);
	const bruno106::Fixed f = fixed_;
	const float inGain = inGain_, outGain = outGain_;
	bruno106::State st = l;
	feedbackLoopMono(startSample, endSample, sampleIncrement, fb, gain, [&](q31_t in) {
		rp.g1 += rp.dg1;
		rp.invDen += rp.dInvDen;
		return brunoToQ31(bruno106::tick(static_cast<float>(in) * inGain, st, rp.g1, rp.invDen, f) * outGain);
	});
	l = st;
	g1_ = g1Target_;
	invDen_ = invDenTarget_;
}

void Bruno106Filter::doFilterStereoFeedback(q31_t* startSample, q31_t* endSample, FeedbackChannel& fbL,
                                            FeedbackChannel& fbR, q31_t gain) {
	const int32_t n = static_cast<int32_t>((endSample - startSample) >> 1);
	Ramp rp = rampFor(n);
	const bruno106::Fixed f = fixed_;
	const float inGain = inGain_, outGain = outGain_;
	bruno106::State sl = l;
	bruno106::State sr = r;
	feedbackLoopStereo(
	    startSample, endSample, fbL, fbR, gain,
	    [&](q31_t in) {
		    rp.g1 += rp.dg1; // left runs first on each sample, then right uses the same coefficients
		    rp.invDen += rp.dInvDen;
		    return brunoToQ31(bruno106::tick(static_cast<float>(in) * inGain, sl, rp.g1, rp.invDen, f) * outGain);
	    },
	    [&](q31_t in) {
		    return brunoToQ31(bruno106::tick(static_cast<float>(in) * inGain, sr, rp.g1, rp.invDen, f) * outGain);
	    });
	l = sl;
	r = sr;
	g1_ = g1Target_;
	invDen_ = invDenTarget_;
}

} // namespace deluge::dsp::filter
