/*
 * BRUNO CHORUS: the Juno's BBD chorus from Ultramaster KR-106, as a Deluge Mod FX type.
 *
 * Portions Copyright (C) 2026 Karl LaRocca (Ultramaster KR-106, kayrockscreenprinting, GPL-3.0).
 * Deluge integration Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dsp/drum_synth/bruno106_chorus.h"
#include "util/fixedpoint.h"
#include <algorithm>
#include <cmath>

namespace deluge::dsp::drum_synth::bruno106 {

namespace {
constexpr float kFs = 44100.f;
constexpr int32_t kLineLen = 512;
constexpr int32_t kMask = kLineLen - 1;

// KR-106's chorus, measured on hardware (KR106Chorus.h)
constexpr float kCenterMs = 3.30f;
constexpr float kRateI = 0.514f, kRateII = 0.842f, kRateI_II = 7.85f;   // Hz
constexpr float kDepthI = 2.13f, kDepthII = 1.71f, kDepthI_II = 0.236f; // ms each way
constexpr float kClockTrim = 0.015f;                                    // the two lines' clocks 3% apart
constexpr float kGainTrim = 0.04f;                                      // and their gains 8%
constexpr float kDryGain = 0.863f, kWetGain = 1.257f;                   // the output summers
constexpr float kCteLoss = 4468.f, kInvClockCenter = 1.f / 40000.f;     // charge transfer loss with clock
constexpr float kStages = 256.f;                                        // MN3009
constexpr float kFadeInc = 1.f / (0.005f * kFs);                        // 5 ms
constexpr float kSatDrive = 0.12f;                                      // the BBD's headroom, KR-106's default

// BBDFilter at 44.1 kHz: SVF g = tan(pi 9661 / fs), a1 = 1 / (1 + g / Q + g^2), and the pole's g / (1 + g), its
// 20 kHz held to 0.45 fs as KR-106 does
constexpr float kBqG = 0.82236413f;
constexpr float kBqA1 = 1.f / (1.f + kBqG / 0.7071f + kBqG * kBqG);
constexpr float kPoleG = 0.86327126f;

// The Deluge's buffer to KR-106's units: a loud voice (2^27) is a full saw, 0.6
constexpr float kIn = 0.6f / 134217728.f;
constexpr float kOut = 134217728.f / 0.6f / 2147483648.f;

[[gnu::always_inline]] inline float svf(float x, float& s1, float& s2) {
	const float v3 = x - s2;
	const float v1 = kBqA1 * (s1 + kBqG * v3);
	const float v2 = s2 + kBqG * v1;
	s1 = 2.f * v1 - s1;
	s2 = 2.f * v2 - s2;
	return v2;
}

[[gnu::always_inline]] inline float pole(float x, float& s) {
	const float v = (x - s) * kPoleG;
	const float lp = s + v;
	s = lp + v;
	return lp;
}

/// tanh(x k) / k as the filters' Padé curve: the MN3009 running out of headroom on hot signals only
[[gnu::always_inline]] inline float sat(float x) {
	const float u = std::clamp(x * kSatDrive, -3.f, 3.f);
	const float u2 = u * u;
	return u * (27.f + u2) / (27.f + 9.f * u2) * (1.f / kSatDrive);
}

[[gnu::always_inline]] inline float hermite(const float* line, float pos) {
	const int32_t i = static_cast<int32_t>(pos);
	const float f = pos - static_cast<float>(i);
	const float y0 = line[(i - 1) & kMask], y1 = line[i & kMask], y2 = line[(i + 1) & kMask],
	            y3 = line[(i + 2) & kMask];
	const float c1 = 0.5f * (y2 - y0);
	const float c2 = y0 - 2.5f * y1 + 2.f * y2 - 0.5f * y3;
	const float c3 = 0.5f * (y3 - y0) + 1.5f * (y1 - y2);
	return ((c3 * f + c2) * f + c1) * f + y1;
}

/// One line: pre filter, saturate, write, read delayMs back, post filter, the clock's gain
[[gnu::always_inline]] inline float line(float x, float* buf, int32_t write, float delayMs, float trimGain, float& bq1,
                                         float& bq2, float& p, float& qb1, float& qb2, float& q) {
	buf[write] = sat(pole(svf(x, bq1, bq2), p));
	float pos = static_cast<float>(write) - delayMs * (kFs * 0.001f);
	if (pos < 0.f) {
		pos += kLineLen;
	}
	const float wet = pole(svf(hermite(buf, pos), qb1, qb2), q);
	// The BBD loses a little more charge at slower clocks: its clock is stages / (2 delay)
	const float invClock = delayMs * (0.002f / kStages);
	return wet * trimGain * (1.f - kCteLoss * (invClock - kInvClockCenter));
}

/// sin(2 pi phase) for phase 0 to 1, within 0.1%: enough for a vibrato
inline float fastSine(float phase) {
	const float x = phase * 2.f - 1.f; // -1 to 1 for -pi to pi
	const float y = 4.f * x * (1.f - std::fabs(x));
	return -(y * (0.775f + 0.225f * std::fabs(y)));
}
} // namespace

void Chorus::reset() {
	l0_ = {};
	l1_ = {};
	write_ = 0;
	phase_ = 0.f;
	fade_ = 0.f;
	mode_ = pending_ = 0;
	started = true;
}

void Chorus::configure(uint8_t mode) {
	switch (mode) {
	case 2:
		inc_ = kRateII / kFs;
		depthTarget_ = kDepthII;
		sine_ = false;
		break;
	case 3:
		inc_ = kRateI_II / kFs;
		depthTarget_ = kDepthI_II;
		sine_ = true;
		break;
	default:
		inc_ = kRateI / kFs;
		depthTarget_ = kDepthI;
		sine_ = false;
		break;
	}
}

void Chorus::process(std::span<StereoSample> buffer, float* memory, uint8_t mode) {
	mode = (mode >= 1 && mode <= 3) ? mode : 1;
	if (mode_ == 0) { // Just switched on: straight to this mode, fading in
		mode_ = pending_ = mode;
		configure(mode);
		depth_ = depthTarget_;
	}
	else if (mode != pending_) {
		pending_ = mode; // fades out, then switches
	}

	float* buf0 = memory;
	float* buf1 = memory + kLineLen;
	Line a = l0_, b = l1_;
	int32_t write = write_;
	float phase = phase_, depth = depth_, fade = fade_;

	for (StereoSample& s : buffer) {
		// Fade in, or out to change mode
		const float fadeTarget = (pending_ == mode_) ? 1.f : 0.f;
		fade = std::clamp(fade + ((fadeTarget > fade) ? kFadeInc : -kFadeInc), 0.f, 1.f);
		if (fade <= 0.f && pending_ != mode_) {
			mode_ = pending_;
			configure(mode_);
			depth = depthTarget_;
		}
		depth += (depthTarget_ - depth) * kFadeInc;

		phase += inc_;
		if (phase >= 1.f) {
			phase -= 1.f;
		}
		const float lfo = sine_ ? fastSine(phase) : 1.f - 4.f * std::fabs(phase - 0.5f);

		// One LFO, the two lines in antiphase, their clocks a little apart
		const float d0 = std::max((kCenterMs + depth * lfo) * (1.f - kClockTrim), 0.1f);
		const float d1 = std::max((kCenterMs - depth * lfo) * (1.f + kClockTrim), 0.1f);

		const float inL = static_cast<float>(s.l) * kIn;
		const float inR = static_cast<float>(s.r) * kIn;
		const float w0 = line(inL, buf0, write, d0, 1.f + kGainTrim, a.bq1, a.bq2, a.p, a.qb1, a.qb2, a.q);
		const float w1 = line(inR, buf1, write, d1, 1.f - kGainTrim, b.bq1, b.bq2, b.p, b.qb1, b.qb2, b.q);
		write = (write + 1) & kMask;

		// Each output an inverting summer: dry plus its line
		const float dry = 1.f - fade * (1.f - kDryGain);
		const float wet = fade * kWetGain;
		s.l = q31_from_float((dry * inL + wet * w0) * kOut);
		s.r = q31_from_float((dry * inR + wet * w1) * kOut);
	}

	l0_ = a;
	l1_ = b;
	write_ = write;
	phase_ = phase;
	depth_ = depth;
	fade_ = fade;
}

} // namespace deluge::dsp::drum_synth::bruno106
