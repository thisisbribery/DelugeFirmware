/*
 * BRUNO106 filter core: the IR3109 four pole OTA filter of Ultramaster KR-106.
 *
 * Portions Copyright (C) 2026 Karl LaRocca (Ultramaster KR-106, kayrockscreenprinting, GPL-3.0).
 * Deluge integration Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This is a modified version of KR106VCF_OPTIMIZED.h: run at 1x only, the Juno-106 resonance curve only, and the
 * per sample maths rearranged to cut its divides from about 13 to 5 with the same result (the soft clipper and its
 * slope share one denominator, the Newton step's divide folds into one, the pitch compensation needs no divide).
 * Not affiliated with or endorsed by Roland.
 *
 * No Deluge headers: the host test builds this too.
 */

#pragma once

#include <algorithm>
#include <arm_neon.h>
#include <cmath>
#include <cstdint>

namespace deluge::dsp::filter::bruno106 {

constexpr float kDirectGain = 0.142f; ///< The osc mix straight to the filter input (10K)
constexpr float kInputRatio = 0.065f; ///< The osc mix into the BA662's + input, against the fed back output
constexpr float kOta = 0.35f;         ///< Each stage's OTA drive
constexpr float kInvOta = 1.f / kOta;
constexpr float kOutGain = 8.59f; ///< The four poles' passband makeup: kDirectGain x this = 1.22

/// min / max through NEON: no compare, so no wait for the compare's flags (VFP has no scalar min or max)
[[gnu::always_inline]] inline float clampF(float x, float lo, float hi) {
	return vget_lane_f32(vmin_f32(vmax_f32(vdup_n_f32(x), vdup_n_f32(lo)), vdup_n_f32(hi)), 0);
}

/// One channel's state. All zeros is a valid start (the noise seed is set by reset()).
struct State {
	float s0, s1, s2, s3; ///< The integrators
	uint32_t seed;        ///< Thermal noise, which seeds self oscillation from silence
};

/// What stays put across a buffer
struct Fixed {
	float k;           ///< Resonance feedback
	float kFb, invKFb; ///< The feedback OTA's drive and its inverse
	float noise;       ///< Thermal noise level
};

/// KR-106's resonance slider (0 to 1) to feedback, Juno-106 curve: k reaches 4 just past 0.9 to self oscillate
inline float resKJ106(float r) {
	float k = r * (4.6265f + r * (4.3866f + r * (-8.9869f + r * 3.3902f)));
	if (r > 0.7f) {
		const float knee = (r - 0.7f) * (1.f / 0.3f);
		k += std::min(knee * knee * 1.2f, 0.35f);
	}
	const float boost = std::max(0.f, (r - 0.898f) * (0.17f / 0.102f));
	return k * (1.f + boost);
}

/// What depends on the resonance alone: worked out when it moves, not when only the cutoff does
struct ResCache {
	float k0;          ///< Feedback before the fade near Nyquist
	float expM;        ///< The peak pull correction at k0
	float kFb, invKFb; ///< The feedback OTA's drive at k0
};

/// KR-106's peak pull correction, fitted on hardware: fades in to k = 1 and out from k = 3.7 to 4.4
inline float peakPull(float k) {
	const float kFit = std::clamp(k, 0.f, 5.f);
	float logM = 0.20646f + kFit * (-0.04140f + kFit * (0.00602f - kFit * 0.00127f));
	if (k < 1.f) {
		logM *= std::max(k, 0.f);
	}
	if (k > 3.7f) {
		logM *= std::min(1.f, std::max(0.f, (4.4f - k) * (1.f / 0.7f)));
	}
	return expf(logM);
}

inline ResCache resCache(float res) {
	ResCache c;
	c.k0 = resKJ106(res);
	c.expM = peakPull(c.k0);
	c.kFb = 4.2f * std::clamp(c.k0 - 2.5f, 0.3f, 1.f);
	c.invKFb = 1.f / c.kFb;
	return c;
}

/// The coefficients for a cutoff, given the resonance's. frq is the cutoff over Nyquist (0 to 1), g its
/// tan(frq pi / 2). Writes the fixed ones, returns g1 = g / (1 + g) after the peak pull correction; invDen from that.
inline float freqCoefs(float frq, float g, const ResCache& rc, Fixed& f, float& invDen) {
	float k = rc.k0, expM = rc.expM, kFb = rc.kFb, invKFb = rc.invKFb;
	// At 1x a resonant peak near Nyquist folds: fade the resonance out from 0.7 to 0.9 of Nyquist
	if (frq > 0.7f) {
		k *= std::max(1.f - (frq - 0.7f) * 5.f, 0.f);
		expM = peakPull(k);
		kFb = 4.2f * std::clamp(k - 2.5f, 0.3f, 1.f);
		invKFb = 1.f / kFb;
	}
	g = std::min(std::min(g, 4.1652998f) * expM, 10.f); // tan(0.85 pi / 2): where the integrators stay well behaved
	const float g1 = g / (1.f + g);
	const float g1sq = g1 * g1;
	invDen = 1.f / (1.f + k * g1sq * g1sq);
	f.k = k;
	f.kFb = kFb;
	f.invKFb = invKFb;
	return g1;
}

/// Both at once (the host tests use this)
inline float coefs(float frq, float g, float res, Fixed& f, float& invDen) {
	return freqCoefs(frq, g, resCache(res), f, invDen);
}

/// The thermal noise level for the filter's state now: bigger when quiet, so it seeds self oscillation, and too small
/// to hear once there's signal. (KR-106 also follows the input level; the state carries it a buffer later.)
inline float noiseFor(const State& s) {
	const float energy = std::fabs(s.s0) + std::fabs(s.s1) + std::fabs(s.s2) + std::fabs(s.s3);
	return 1e-2f / (1.f + energy * 1000.f);
}

/// One OTA-C stage: solves y = s + g tanh(x - y) by one Newton step from the linear estimate. h = 1 - g1.
/// KR-106's NLStage with the clipper, its slope and the Newton divide sharing one denominator.
[[gnu::always_inline]] inline float stage(float& s, float x, float g1, float h) {
	float y = s + g1 * (x - s);
	const float sd = clampF((x - y) * kOta, -3.f, 3.f); // the curve is flat at 1 beyond 3, and so is KR-106's
	const float x2 = sd * sd;
	const float d = 27.f + 9.f * x2;
	const float num = h * (y - s) * d * d - g1 * sd * (27.f + x2) * d * kInvOta;
	const float den = h * d * d + g1 * 27.f * (27.f - 3.f * x2);
	y -= num / den;
	s = 2.f * y - s;
	return y;
}

/// One sample as the Deluge runs it, in KR-106's units (a full saw 0.6). g1 and invDen ramp across a buffer; f holds
/// still. KR-106's BA662 feedback OTA as it is (the resonance, its compression and self oscillation), with the four
/// IR3109 stages linear: their saturation only shows when driven hot, and solving it per stage cost four times the
/// rest of the voice on the Deluge (see tickFull). With linear stages there is no saturation to pull the pitch, so no
/// pitch compensation either.
[[gnu::always_inline]] inline float tick(float in, State& st, float g1, float invDen, const Fixed& f) {
	st.seed = st.seed * 196314165u + 907633515u;
	union {
		uint32_t u;
		float fl;
	} n;
	n.u = (st.seed >> 9) | 0x40000000u;
	in += (n.fl - 3.f) * f.noise;

	st.s0 += 1e-20f;
	const float h = 1.f - g1;
	const float g1sq = g1 * g1;
	// The cascade's output with no input: what the fed back path sees before this sample's input arrives
	const float S = h * ((st.s0 * g1 + st.s1) * g1sq + st.s2 * g1 + st.s3);

	const float dd = clampF((in * kInputRatio - S) * f.kFb, -3.f, 3.f);
	const float dd2 = dd * dd;
	const float otaOut = dd * (27.f + dd2) / (27.f + 9.f * dd2) * f.invKFb;
	const float u = (in * kDirectGain + f.k * otaOut) * invDen;

	// Four trapezoidal one poles. Each output is the next one's input; the states update off that path.
	const float y1 = st.s0 + g1 * (u - st.s0);
	const float y2 = st.s1 + g1 * (y1 - st.s1);
	const float y3 = st.s2 + g1 * (y2 - st.s2);
	const float y4 = st.s3 + g1 * (y3 - st.s3);
	st.s0 = 2.f * y1 - st.s0;
	st.s1 = 2.f * y2 - st.s1;
	st.s2 = 2.f * y3 - st.s2;
	st.s3 = 2.f * y4 - st.s3;
	return y4 * kOutGain;
}

/// One sample with KR-106's nonlinear stages, each solved by a Newton step (kept for the host comparison: too heavy for
/// the Deluge, about 1 us a voice)
[[gnu::always_inline]] inline float tickFull(float in, State& st, float g1, float invDen, const Fixed& f) {
	st.seed = st.seed * 196314165u + 907633515u;
	union {
		uint32_t u;
		float fl;
	} n;
	n.u = (st.seed >> 9) | 0x40000000u;
	in += (n.fl - 3.f) * f.noise;

	st.s0 += 1e-20f;
	const float g1sq = g1 * g1;
	const float S = (1.f - g1) * (st.s0 * g1sq * g1 + st.s1 * g1sq + st.s2 * g1 + st.s3);

	// The BA662: input and fed back output share its differential pair
	const float dd = clampF((in * kInputRatio - S) * f.kFb, -3.f, 3.f);
	const float dd2 = dd * dd;
	const float otaOut = dd * (27.f + dd2) / (27.f + 9.f * dd2) * f.invKFb;
	const float u = (in * kDirectGain + f.k * otaOut) * invDen;

	// Pitch held as the stages saturate in self oscillation: g1 / max(1 / sqrt(1 + 0.6 a^2), 0.65), at most 0.98
	const float a = st.s3;
	const float m = std::min(sqrtf(1.f + 0.6f * a * a), 1.f / 0.65f);
	const float gN = std::min(g1 * m, 0.98f);
	const float h = 1.f - gN;

	const float y1 = stage(st.s0, u, gN, h);
	const float y2 = stage(st.s1, y1, gN, h);
	const float y3 = stage(st.s2, y2, gN, h);
	const float y4 = stage(st.s3, y3, gN, h);
	return y4 * kOutGain;
}

} // namespace deluge::dsp::filter::bruno106
