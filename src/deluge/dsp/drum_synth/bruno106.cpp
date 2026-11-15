/*
 * BRUNO106: the DCO voice of Ultramaster KR-106 as a Deluge synth engine.
 *
 * Portions Copyright (C) 2026 Karl LaRocca (Ultramaster KR-106, kayrockscreenprinting, GPL-3.0).
 * Deluge integration Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * From KR106Oscillators.h (Oscillators::Process, PolyBLEP), KR106Voice.h (levels, sliders, DC blocker, variance) and
 * KR106Noise.h (noise band limits). Not affiliated with or endorsed by Roland.
 */

#include "dsp/drum_synth/bruno106.h"
#include <algorithm>
#include <cmath>

namespace deluge::dsp::drum_synth::bruno106 {

namespace {

// Juno-106 levels at the mix, a saw being 0.6 (KR106Oscillators.h). Everything is scaled so the saw lands at 1.
constexpr float kNorm = 1.f / 0.6f;
constexpr float kSawAmp = 0.6f * kNorm;
constexpr float kPulseAmp = 0.5f * kNorm;
constexpr float kSubAmp = 0.75f * kNorm;
constexpr float kNoiseAmp = 1.2f * kNorm;

// At 44.1 kHz: switch fade 1 - exp(-1 / (1.45 ms fs)), DC blocker tan(pi 1.59 / fs) as a TPT one pole (g / (1 + g)),
// noise low pass 5.3 kHz less KR-106's bilinear correction, noise high pass 34 Hz
constexpr float kSwitchRamp = 0.015518f;
constexpr float kDcG = 1.13265e-4f / (1.f + 1.13265e-4f);
constexpr float kNoiseLpA = 0.48118f;
constexpr float kNoiseHpA = 0.99516f;
constexpr float kGainEps = 1e-4f;

/// KR-106's PolyBLEP: a 4th order correction over two samples each side of a step. t is the phase, dt the step per
/// sample, invDt its inverse (once per buffer, so no divide here).
[[gnu::noinline]] float polyBlep(float t, float dt, float invDt) {
	const float dt2 = dt + dt;
	if (t < dt) {
		const float x = t * invDt;
		const float x2 = x * x;
		return 0.25f * x2 * x2 - 0.66666667f * x2 * x + 1.33333333f * x - 1.f;
	}
	if (t < dt2) {
		const float u = 1.f - (t - dt) * invDt;
		const float u2 = u * u;
		return -0.08333333f * u2 * u2;
	}
	if (t > 1.f - dt) {
		const float x = (t - 1.f) * invDt;
		const float x2 = x * x;
		return -0.25f * x2 * x2 - 0.66666667f * x2 * x + 1.33333333f * x + 1.f;
	}
	if (t > 1.f - dt2) {
		const float u = 1.f + (t - 1.f + dt) * invDt;
		const float u2 = u * u;
		return 0.08333333f * u2 * u2;
	}
	return 0.f;
}

/// -1 to 1 from the seed
inline float uniform(uint32_t& seed) {
	seed = seed * 196314165u + 907633515u;
	return static_cast<float>(static_cast<int32_t>(seed)) * (1.f / 2147483648.f);
}

/// A slider through an 11 point table, as KR-106's
float table11(const float (&t)[11], float x) {
	const float idx = std::clamp(x, 0.f, 1.f) * 10.f;
	const int32_t i = std::min<int32_t>(static_cast<int32_t>(idx), 9);
	return t[i] + (idx - static_cast<float>(i)) * (t[i + 1] - t[i]);
}

} // namespace

float subLevel(float slider) {
	// Juno-106 sub slider, measured (dcoSubLevel_j106); off below the first step, as the slider's bottom is silent
	static constexpr float kTable[11] = {0.f,      0.01251f, 0.10875f, 0.22125f, 0.33576f, 0.45206f,
	                                     0.55750f, 0.67178f, 0.78447f, 0.91132f, 1.f};
	return table11(kTable, slider) * kSubAmp;
}

float noiseLevel(float slider) {
	// Juno-106 noise slider (dcoNoiseLevel_j106): a soft knee turning on just above the bottom
	if (slider <= 0.f) {
		return 0.f;
	}
	constexpr float kA = 1.0632f;
	constexpr float kB = 0.0146f;
	constexpr float kC = 0.0594f;
	const float d = slider - kC;
	return kA * (sqrtf(d * d + kB * kB) + d) * 0.5f * kNoiseAmp;
}

void trigger(Voice& v, float slop, uint32_t seed) {
	auto rnd = [&]() { return uniform(seed); };
	slop = std::clamp(slop, 0.f, 1.f);
	// KR-106's human spread at full slop: tuning sigma 12 cents, the width's ends 2% each way, level 2.4%
	const float cents = (rnd() + rnd() + rnd()) * 12.f * slop;
	v.pitchMul = exp2f(cents * (1.f / 1200.f));
	v.pwLo = 0.5f + rnd() * 0.02f * slop;
	v.pwHi = 0.95f + rnd() * 0.02f * slop;
	v.level = 1.f + rnd() * 0.024f * slop;
	// The DCO free runs on the hardware: start each note somewhere in its cycle
	v.phase = static_cast<uint32_t>((rnd() + 1.f) * 2147483647.f);
	v.subLow = (rnd() < 0.f) ? 1u : 0u;
	v.seed = seed | 1u;
}

void render(Voice& v, const Settings& s, uint32_t baseIncrement, float* out, int32_t n) {
	if (n <= 0) {
		return;
	}
	// The phase in 32 bit integers, as the Deluge's own oscillators: the wrap, the pulse's level and whether a step is
	// near all come from integer compares, which don't hold up the float maths (a float compare waits for its flags)
	const float incF = std::clamp(static_cast<float>(baseIncrement) * v.pitchMul, 4295.f, 0.45f * 4294967296.f);
	const uint32_t inc = static_cast<uint32_t>(incF);
	const uint32_t inc2 = inc + inc; ///< PolyBLEP corrects within two samples of a step
	const uint32_t nearTop = ~inc2;  ///< ...on either side
	const float cps = static_cast<float>(inc) * (1.f / 4294967296.f);
	const float invDt = 1.f / cps;

	// The width as KR-106's Juno-106 path: the knob across this note's 50% to 95%, then inverted (its pulse is low for
	// the width): low for 1 - pw of the cycle
	const float pwTarget = std::clamp(1.f - (v.pwLo + std::clamp(s.pw, 0.f, 1.f) * (v.pwHi - v.pwLo)), 0.01f, 0.99f);
	const uint32_t pwPhaseTarget = static_cast<uint32_t>(pwTarget * 4294967296.f);
	const float subTarget = s.sub * v.level;
	const float noiseTarget = s.noise * v.level;
	if (!v.started) {
		v.pwPhase = pwPhaseTarget;
		v.sub = subTarget;
		v.noise = noiseTarget;
		v.sawGain = s.sawOn ? 1.f : 0.f;
		v.pulseGain = s.pulseOn ? 1.f : 0.f;
		v.started = true;
	}
	const float inv = 1.f / static_cast<float>(n);
	const int32_t dPw = static_cast<int32_t>((static_cast<int64_t>(pwPhaseTarget) - v.pwPhase) / n);
	const float dSub = (subTarget - v.sub) * inv;
	const float dNoise = (noiseTarget - v.noise) * inv;
	const float sawOn = s.sawOn ? 1.f : 0.f;
	const float pulseOn = s.pulseOn ? 1.f : 0.f;
	const bool pulse = s.pulseOn || v.pulseGain > kGainEps;
	const bool sub = subTarget > 0.f || v.sub > 0.f;
	const bool noise = noiseTarget > 0.f || v.noise > 0.f;
	const float sawLevel = kSawAmp * v.level;
	const float pulseLevel = kPulseAmp * v.level;
	static constexpr float kSign[2] = {1.f, -1.f};

	// Locals, so the loop keeps them in registers
	uint32_t phase = v.phase, subLow = v.subLow, pwPhase = v.pwPhase;
	float sawGain = v.sawGain, pulseGain = v.pulseGain;
	float subLvl = v.sub, noiseLvl = v.noise, dc = v.dc, nLp = v.noiseLp, nHp = v.noiseHp;
	uint32_t seed = v.seed;

	for (int32_t i = 0; i < n; i++) {
		phase += inc;
		subLow ^= static_cast<uint32_t>(phase < inc); // wrapped: the sub flips every cycle
		const float pos = static_cast<float>(phase) * (1.f / 4294967296.f);
		sawGain += (sawOn - sawGain) * kSwitchRamp;

		float blepReset = 0.f;
		if (phase < inc2 || phase > nearTop) [[unlikely]] {
			blepReset = polyBlep(pos, cps, invDt);
		}
		float x = (2.f * pos - 1.f - blepReset) * sawLevel * sawGain;
		if (pulse) {
			pulseGain += (pulseOn - pulseGain) * kSwitchRamp;
			float p = kSign[phase < pwPhase] - blepReset; // low for the first part of the cycle
			const uint32_t pw2 = phase - pwPhase;         // the phase since the rising edge, wrapped
			if (pw2 < inc2 || pw2 > nearTop) [[unlikely]] {
				p += polyBlep(static_cast<float>(pw2) * (1.f / 4294967296.f), cps, invDt);
			}
			x += p * pulseLevel * pulseGain;
			pwPhase += dPw;
		}
		if (sub) {
			subLvl += dSub;
			x += kSign[subLow] * (1.f - std::fabs(blepReset)) * subLvl;
		}
		if (noise) {
			noiseLvl += dNoise;
			const float white = (uniform(seed) + uniform(seed)) * 0.7f;
			nLp = kNoiseLpA * nLp + (1.f - kNoiseLpA) * white;
			nHp = kNoiseHpA * nHp + (1.f - kNoiseHpA) * nLp;
			x += (nLp - nHp) * noiseLvl;
		}
		// The coupling cap after the DCO: takes the pulse's DC off
		const float dv = (x - dc) * kDcG;
		const float lp = dc + dv;
		dc = lp + dv;
		out[i] = x - lp;
	}

	v.phase = phase;
	v.subLow = subLow;
	v.sawGain = sawGain;
	v.pulseGain = pulse ? pulseGain : pulseOn;
	v.pwPhase = pwPhaseTarget;
	v.sub = subTarget;
	v.noise = noiseTarget;
	v.dc = dc;
	v.noiseLp = nLp;
	v.noiseHp = nHp;
	v.seed = seed;
}

} // namespace deluge::dsp::drum_synth::bruno106
