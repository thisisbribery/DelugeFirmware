/*
 * Cyc engines. See cyc.h for what these are.
 *
 * Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dsp/drum_synth/cyc.h"
#include <algorithm>
#include <cmath>

namespace deluge::dsp::drum_synth::cyc {

namespace cyc_detail {

constexpr float kPhaseToCycles = 1.f / 4294967296.f;
constexpr float kFullScale = 2147483520.f; ///< The largest float below 2^31, so clipping can't wrap

/// sin(2 pi phase / 2^32), to about -100 dB, without a table
inline float sine(uint32_t phase) {
	// Centre on zero: x in [-0.5, 0.5) cycles, then fold into [-0.25, 0.25]
	float x = static_cast<float>(static_cast<int32_t>(phase)) * kPhaseToCycles;
	if (x > 0.25f) {
		x = 0.5f - x;
	}
	else if (x < -0.25f) {
		x = -0.5f - x;
	}
	const float x2 = x * x;
	// Taylor series of sin(2 pi x), coefficients trimmed for the [-0.25, 0.25] range
	return x * (6.283185307f + x2 * (-41.341702f + x2 * (81.605249f + x2 * (-76.705859f + x2 * 42.058693f))));
}

/// Phase offset in cycles to a phase. Wraps first: converting a float beyond the int32 range is undefined, and deep
/// modulation goes well past half a cycle.
inline uint32_t cycles(float c) {
	float x =
	    c - static_cast<float>(static_cast<int32_t>(c)); // -1 to 1 (no rintf: the Cortex-A9 has no instruction for it)
	if (x >= 0.5f) {
		x -= 1.f;
	}
	else if (x < -0.5f) {
		x += 1.f;
	}
	return static_cast<uint32_t>(static_cast<int32_t>(x * 4294967040.f));
}

/// Per sample multiplier that falls by 60 dB in t60 seconds
inline float decayFor(float t60, float sampleRate) {
	if (t60 <= 0.f) {
		return 0.f;
	}
	return expf(-6.907755f / (t60 * sampleRate));
}

/// Uniform noise, -1 to 1
inline float noise(uint32_t& state) {
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return static_cast<float>(static_cast<int32_t>(state)) * (1.f / 2147483648.f);
}

/// A TPT state variable filter, after Andrew Simper's
struct SvfCoefs {
	float a1, a2, a3, k;
};

inline SvfCoefs svfCoefs(float hz, float q, float sampleRate) {
	const float g = tanf(3.14159265f * std::clamp(hz, 10.f, 0.45f * sampleRate) / sampleRate);
	const float k = 1.f / q;
	SvfCoefs c;
	c.a1 = 1.f / (1.f + g * (g + k));
	c.a2 = g * c.a1;
	c.a3 = g * c.a2;
	c.k = k;
	return c;
}

struct SvfOut {
	float lp, bp, hp;
};

inline SvfOut svf(Voice::Svf& s, const SvfCoefs& c, float x) {
	const float v3 = x - s.ic2;
	const float v1 = c.a1 * s.ic1 + c.a2 * v3;
	const float v2 = s.ic2 + c.a2 * s.ic1 + c.a3 * v3;
	s.ic1 = 2.f * v1 - s.ic1;
	s.ic2 = 2.f * v2 - s.ic2;
	return {v2, v1, x - c.k * v1 - v2};
}

/// Soft clip with unity slope at zero, approaching +-1
inline float softClip(float x) {
	x = std::clamp(x, -3.f, 3.f);
	return x * (27.f + x * x) / (27.f + 9.f * x * x);
}

constexpr float kHoldKnob = 0.995f;

} // namespace cyc_detail

using namespace cyc_detail;

// Engines -------------------------------------------------------------------------------------------------------------
// Each renders mono float samples, about -1 to 1, before the common amp envelope.

namespace {

struct Ctx {
	const Settings& st;
	float sr;
	float hz; ///< The note's frequency
};

/// Linear interpolation through (x, y) points, x ascending
template <size_t N>
float interp(const std::array<float, N>& xs, const std::array<float, N>& ys, float x) {
	if (x <= xs[0]) {
		return ys[0];
	}
	for (size_t i = 1; i < N; i++) {
		if (x <= xs[i]) {
			const float f = (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
			return ys[i - 1] + f * (ys[i] - ys[i - 1]);
		}
	}
	return ys[N - 1];
}

/// Amp decay: knob (0-127) to the time in seconds to fall 60 dB, through measured points, interpolated in log time.
/// On the engines whose originals hold at the top of the knob, so does this.
struct DecayCurve {
	std::array<float, 6> knob;
	std::array<float, 6> logT60;
	bool holdsAtTop = false;
};

constexpr DecayCurve kKikDecay{
    {0.f, 16.f, 40.f, 64.f, 100.f, 127.f}, {-3.0f, -0.80f, 0.35f, 1.10f, 2.07f, 2.71f}, true};
constexpr DecayCurve kMetalDecay{{0.f, 16.f, 40.f, 64.f, 100.f, 127.f}, {-4.61f, -2.30f, -0.19f, 0.97f, 2.07f, 2.63f}};
constexpr DecayCurve kPercDecay{{0.f, 16.f, 40.f, 64.f, 100.f, 127.f}, {-3.0f, -1.97f, -0.06f, 1.07f, 2.16f, 2.69f}};
constexpr DecayCurve kToneDecay{
    {0.f, 16.f, 42.f, 64.f, 100.f, 127.f}, {-4.61f, -1.5f, -0.03f, 0.99f, 2.09f, 2.68f}, true};
constexpr DecayCurve kChordDecay{{0.f, 16.f, 40.f, 64.f, 100.f, 127.f}, {-4.61f, -1.6f, 0.f, 0.99f, 2.09f, 2.67f}};
constexpr DecayCurve kVsnrDecay{{0.f, 16.f, 33.f, 64.f, 100.f, 127.f}, {-3.0f, -2.6f, -2.3f, -1.31f, -0.76f, -0.12f}};
constexpr DecayCurve kClapDecay{{0.f, 32.f, 64.f, 100.f, 120.f, 127.f}, {-3.9f, -0.6f, -0.1f, 0.45f, 0.65f, 0.72f}};
constexpr DecayCurve kSnrDecay{{0.f, 16.f, 40.f, 64.f, 100.f, 127.f}, {-4.61f, -3.91f, -2.04f, -0.94f, 0.13f, 0.72f}};

float ampDecayTime(const DecayCurve& curve, float knob) {
	return expf(interp(curve.knob, curve.logT60, knob));
}

/// Each engine's level, so they all start near full scale at their default settings, before drive: peaks around -3 dB
/// for the tonal ones, -1 dB for the ones whose peak is a short crack
constexpr std::array<float, static_cast<size_t>(Kind::NUM_KINDS)> kLevelTrim{
    2.92f, // Cyc Kik
    3.13f, // Cyc Snr
    4.52f, // Cyc Metal
    3.85f, // Cyc Perc
    5.69f, // Cyc Tone
    7.24f, // Cyc Chord
    2.26f, // Vtg Snr
    2.02f, // clap eng
};

const DecayCurve& decayCurveFor(Kind kind) {
	switch (kind) {
	case Kind::SNR:
		return kSnrDecay;
	case Kind::METAL:
		return kMetalDecay;
	case Kind::PERC:
		return kPercDecay;
	case Kind::TONE:
		return kToneDecay;
	case Kind::CHORD:
		return kChordDecay;
	case Kind::VSNR:
		return kVsnrDecay;
	case Kind::CLAP:
		return kClapDecay;
	default:
		return kKikDecay;
	}
}

// Cyc Kik -------------------------------------------------------------------------------------------------------------
// A sine body two octaves under the note, swept down from above by a pitch envelope, then shaped:
// - Color: drive into a soft clipper, adding odd harmonics that last as long as the body
// - Shape: below 16 it rounds the peaks off; above, it adds a sharp spike on each peak, a buzz of odd harmonics, then
//   noise riding on those spikes, strongest around 96
// - Sweep: depth and speed of the pitch sweep, from none to about 13 times the final pitch
// - Contour: the length of the pitch sweep, from a short blip (0) to a long, boomy drop (127), plus the knock at the
//   start, a short burst of filtered noise and a click. The original's Contour barely does anything; at its default
//   (24) the sweep is as voiced.

constexpr std::array<float, 6> kKikSweepKnob{0.f, 16.f, 32.f, 64.f, 96.f, 127.f};
constexpr std::array<float, 6> kKikSweepDepth{0.f, 1.46f, 1.70f, 1.90f, 2.30f, 3.09f}; ///< ln(1 + start ratio)
constexpr std::array<float, 6> kKikSweepTime{0.010f, 0.011f, 0.022f, 0.046f, 0.023f, 0.012f};
constexpr std::array<float, 6> kKikShapeKnob{0.f, 16.f, 32.f, 64.f, 96.f, 127.f};
constexpr std::array<float, 6> kKikSpike{-0.05f, 0.f, 0.1f, 0.45f, 0.5f, 0.2f};
constexpr std::array<float, 6> kKikSpikeNoise{0.f, 0.f, 0.f, 0.f, 2.5f, 0.5f};

/// sign(x) |x|^20: a narrow spike at each peak of a sine
inline float spike(float x) {
	const float x2 = x * x;
	const float x4 = x2 * x2;
	const float x8 = x4 * x4;
	const float x16 = x8 * x8;
	return x * x16 * x2;
}

/// How much Contour stretches a pitch sweep's time: 0.35 times at 0, as voiced at the engine's default, 5 times at 127
float sweepTimeScale(float contour, float atDefault) {
	if (contour <= atDefault) {
		return 0.35f * powf(1.f / 0.35f, contour / atDefault);
	}
	return powf(5.f, (contour - atDefault) / (1.f - atDefault));
}
constexpr float kKikContourDefault = 24.f / 127.f;

void startKik(Voice& v, const Ctx& c) {
	auto& k = v.s_.kik;
	k = {};
	k.pitchEnv = 1.f;
	k.clickEnv = 1.f;
	k.body.phase = 0;
}

void renderKik(Voice& v, const Ctx& c, float* out, int32_t n) {
	auto& k = v.s_.kik;
	const float color = c.st.macro[0];
	const float shape = c.st.macro[1];
	const float sweepKnob = c.st.macro[2] * 127.f;
	const float contour = c.st.macro[3];

	const float fEnd = c.hz * 0.25f;
	const float depth = expf(interp(kKikSweepKnob, kKikSweepDepth, sweepKnob)) - 1.f;
	const float pitchCoef = decayFor(interp(kKikSweepKnob, kKikSweepTime, sweepKnob) * 6.907755f
	                                     * sweepTimeScale(contour, kKikContourDefault),
	                                 c.sr);
	const float incPerHz = 4294967296.f / c.sr;

	// Shape: spikes, then feedback
	const float spikeAmount = interp(kKikShapeKnob, kKikSpike, shape * 127.f);
	const float spikeNorm = 1.f / (1.f + spikeAmount);
	const float spikeNoise = interp(kKikShapeKnob, kKikSpikeNoise, shape * 127.f);
	// Color: a blend from clean into a soft clipper driven harder as it comes in
	const float drive = 1.f + 5.f * color;
	const float driveNorm = color / softClip(drive);
	const float dry = 1.f - color;
	// Contour: knock level
	const float knock = 0.04f + 0.5f * contour * contour;
	const float clickCoef = decayFor(0.004f, c.sr);
	constexpr float kLevel = 0.23f;

	for (int32_t i = 0; i < n; i++) {
		const float hz = fEnd * (1.f + depth * k.pitchEnv);
		k.pitchEnv *= pitchCoef;
		k.body.phase += static_cast<uint32_t>(std::min(hz, 0.45f * c.sr) * incPerHz);
		float x = sine(k.body.phase);
		const float nz = noise(v.common_.rng);
		const float sp = spike(x);
		x = (x + (spikeAmount + spikeNoise * nz) * sp) * spikeNorm;
		x = dry * x + softClip(drive * x) * driveNorm;
		// The knock: noise through a one-pole high pass, dying in a few ms
		const float hp = nz - k.fold;
		k.fold = nz;
		const float click = knock * k.clickEnv * hp;
		k.clickEnv *= clickCoef;
		out[i] = kLevel * x + click;
	}
}

// Cyc Snr -------------------------------------------------------------------------------------------------------------
// A sine body on the note with a short pitch drop, and a rattle: an FM pair (carrier three times the note, modulator
// twice it) whose index runs from a few clean odd partials up into noise. A burst of noise at the start gives the
// crack.
// - Color: tunes the rattle up, from 3 to 6 times the note
// - Shape: rattle index and noise, from tonal to all noise
// - Sweep: depth and length of the body's pitch drop
// - Contour: how fast the rattle's noise dies away, leaving its tone ringing

void startSnr(Voice& v, const Ctx& c) {
	auto& d = v.s_.snr;
	d = {};
	d.pitchEnv = 1.f;
	d.rattleEnv = 1.f;
	d.noiseEnv = 1.f;
	d.bodyEnv = 1.f;
}

void renderSnr(Voice& v, const Ctx& c, float* out, int32_t n) {
	auto& d = v.s_.snr;
	const float color = c.st.macro[0];
	const float shape = c.st.macro[1];
	const float sweep = c.st.macro[2];
	const float contour = c.st.macro[3];
	const float incPerHz = 4294967296.f / c.sr;

	const float depth = 2.2f * powf(sweep, 0.8f);
	const float pitchCoef = decayFor(0.004f + 0.05f * sweep, c.sr);
	const uint32_t modInc = static_cast<uint32_t>(std::min(2.f * c.hz, 0.45f * c.sr) * incPerHz);
	const float ratio = 3.f * powf(2.f, powf(color, 1.4f));
	const uint32_t carInc = static_cast<uint32_t>(std::min(ratio * c.hz, 0.45f * c.sr) * incPerHz);
	const float index = 0.1f + 1.1f * shape * shape;
	const float noiseAmount = powf(shape, 1.5f);
	// Contour: the rattle's index and noise fall away, from lasting the whole hit (0) to 70 ms (127)
	const float rattleCoef = (contour < 0.01f) ? 1.f : decayFor(0.07f / (contour * contour * 0.99f + 0.01f), c.sr);
	// The body cracks and dies quickly; the rattle carries the tail
	const float bodyCoef = decayFor(0.045f + 0.05f * sweep, c.sr);
	const float attackStep = 1.f / (0.0015f * c.sr);
	const SvfCoefs hp = svfCoefs(500.f, 0.7f, c.sr);
	constexpr float kBody = 0.3f;
	constexpr float kRattle = 0.035f;
	constexpr float kNoise = 0.05f;
	constexpr float kBurst = 0.045f;

	for (int32_t i = 0; i < n; i++) {
		const float hz = c.hz * (1.f + depth * d.pitchEnv);
		d.pitchEnv *= pitchCoef;
		d.body.phase += static_cast<uint32_t>(std::min(hz, 0.45f * c.sr) * incPerHz);
		d.attack = std::min(d.attack + attackStep, 1.f);
		const float body = sine(d.body.phase) * d.bodyEnv * d.attack;
		d.bodyEnv *= bodyCoef;

		d.mod.phase += modInc;
		d.rattle[0].phase += carInc;
		const float rattle = sine(d.rattle[0].phase + cycles(index * d.rattleEnv * sine(d.mod.phase)));
		const float nz = svf(d.noiseBp, hp, noise(v.common_.rng)).hp;
		const float noisy = (kNoise * noiseAmount * d.rattleEnv + kBurst * d.noiseEnv) * nz;
		d.rattleEnv *= rattleCoef;
		d.noiseEnv *= bodyCoef;
		out[i] = kBody * body + d.attack * (kRattle * rattle + noisy);
	}
}

// Cyc Metal -----------------------------------------------------------------------------------------------------------
// Three phase modulation pairs at inharmonic ratios around five times the note, cross-coupled, driven hard enough to
// spread into a dense forest of partials, then high passed. Hats at short decays, cymbals and bells at long ones.
// - Color: how far the ratios are pulled apart, from clustered to wide and bright
// - Shape: modulation depth, from a few metallic tones to dense noise
// - Sweep: a pitch drop on all partials at the start
// - Contour: how fast the top end dies away against the rest

constexpr std::array<float, 6> kMetalRatioLo{1.f, 1.3420f, 1.2001f, 1.6830f, 1.4513f, 2.1260f};
constexpr std::array<float, 6> kMetalRatioHi{1.f, 1.8137f, 1.5873f, 2.7461f, 2.2389f, 3.4317f};

void startMetal(Voice& v, const Ctx& c) {
	auto& m = v.s_.metal;
	m = {};
	m.pitchEnv = 1.f;
	m.brightEnv = 1.f;
	for (size_t i = 0; i < m.osc.size(); i++) {
		m.osc[i].phase = static_cast<uint32_t>(i) * 0x2545F491u; // spread the start phases
	}
}

void renderMetal(Voice& v, const Ctx& c, float* out, int32_t n) {
	auto& m = v.s_.metal;
	const float color = c.st.macro[0];
	const float shape = c.st.macro[1];
	const float sweep = c.st.macro[2];
	const float contour = c.st.macro[3];
	const float incPerHz = 4294967296.f / c.sr;

	const float base = c.hz * 5.03f;
	std::array<float, 6> inc;
	for (size_t i = 0; i < inc.size(); i++) {
		const float ratio = kMetalRatioLo[i] + color * (kMetalRatioHi[i] - kMetalRatioLo[i]);
		inc[i] = std::min(base * ratio, 0.49f * c.sr) * incPerHz;
	}
	const float index = 0.25f + 1.1f * shape * shape;
	const float depth = 0.9f * sweep * sweep;
	const float pitchCoef = decayFor(0.02f + 0.06f * sweep, c.sr);
	const float brightCoef = (contour < 0.01f) ? 1.f : decayFor(0.03f / (contour * contour * 0.99f + 0.01f), c.sr);
	const SvfCoefs hp = svfCoefs(std::clamp(base * 1.5f, 300.f, 6000.f), 0.6f, c.sr);
	const SvfCoefs ring = svfCoefs(std::clamp(base * 2.5f, 500.f, 12000.f), 4.f, c.sr);
	constexpr float kLevel = 0.14f;
	// Contour's low pass closes from open to about 5 kHz
	const float cutLo = 1.f - expf(-6.2831853f * 4000.f / c.sr);
	const float cutHi = 1.f - expf(-6.2831853f * std::min(15000.f, 0.4f * c.sr) / c.sr);

	for (int32_t i = 0; i < n; i++) {
		const float bend = 1.f + depth * m.pitchEnv;
		m.pitchEnv *= pitchCoef;
		for (size_t k = 0; k < 6; k++) {
			m.osc[k].phase += static_cast<uint32_t>(inc[k] * bend);
		}
		// Pairs: 1 > 0, 3 > 2, 5 > 4, and each pair's output also rocks the next pair's modulator
		const float a = sine(m.osc[1].phase + cycles(0.5f * index * m.osc[4].last));
		const float x0 = sine(m.osc[0].phase + cycles(index * a));
		const float b = sine(m.osc[3].phase + cycles(0.5f * index * x0));
		const float x1 = sine(m.osc[2].phase + cycles(index * b));
		const float d = sine(m.osc[5].phase + cycles(0.5f * index * x1));
		const float x2 = sine(m.osc[4].phase + cycles(index * d));
		m.osc[4].last = x2;
		float x = (x0 + x1 + x2) * 0.4f;
		x = svf(m.hp, hp, x).hp;
		x += 0.35f * svf(m.ring, ring, x).bp; // the cymbal's ringing band
		// Contour: a one-pole low pass closing from wide open, so the top dies first
		const float cut = cutLo + (cutHi - cutLo) * m.brightEnv;
		m.brightEnv *= brightCoef;
		m.lp += cut * (x - m.lp);
		out[i] = kLevel * m.lp;
	}
}

// Cyc Perc ------------------------------------------------------------------------------------------------------------
// Two sines a major third apart (the note and 1.26 times it), the first phase modulated from just over half its pitch,
// plus a bright overtone riding on it. Toms, congas, blocks and bells.
// - Color: the overtone, coming in and climbing from 3 to 7 times the note
// - Shape: the modulation, growing sidebands around the note as the third fades: hollow to gnarly
// - Sweep: the pitch drop, slow and shallow in the middle of the knob, fast and deep at the top
// - Contour: how fast the overtone dies against the body, and the length of the pitch drop (as voiced at the default,
//   64), so it still does something with no overtone

constexpr float kPercContourDefault = 64.f / 127.f;
constexpr std::array<float, 5> kPercSweepKnob{0.f, 32.f, 64.f, 100.f, 127.f};
constexpr std::array<float, 5> kPercSweepDepth{0.f, 0.4f, 0.75f, 0.35f, 1.2f};
constexpr std::array<float, 5> kPercSweepTime{0.05f, 0.2f, 0.2f, 0.025f, 0.012f};

void startPerc(Voice& v, const Ctx& c) {
	auto& d = v.s_.perc;
	d = {};
	d.pitchEnv = 1.f;
	d.indexEnv = 1.f;
}

void renderPerc(Voice& v, const Ctx& c, float* out, int32_t n) {
	auto& d = v.s_.perc;
	const float color = c.st.macro[0];
	const float shape = c.st.macro[1];
	const float sweepKnob = c.st.macro[2] * 127.f;
	const float contour = c.st.macro[3];
	const float incPerHz = 4294967296.f / c.sr;

	const float depth = interp(kPercSweepKnob, kPercSweepDepth, sweepKnob);
	const float pitchCoef = decayFor(interp(kPercSweepKnob, kPercSweepTime, sweepKnob) * 6.907755f
	                                     * sweepTimeScale(contour, kPercContourDefault),
	                                 c.sr);
	const float index = 0.24f * shape * shape;
	const float levelB = 0.9f * (1.f - 0.75f * shape);
	const float ratioC = 1.f + 6.f * powf(color, 1.3f);
	const float levelC = 1.2f * powf(color, 0.8f);
	const float overtoneCoef = decayFor(0.08f + 6.f * (1.f - contour) * (1.f - contour), c.sr);
	const float maxHz = 0.45f * c.sr;
	constexpr float kLevel = 0.1f;

	for (int32_t i = 0; i < n; i++) {
		const float hz = c.hz * (1.f + depth * d.pitchEnv);
		d.pitchEnv *= pitchCoef;
		d.car.phase += static_cast<uint32_t>(std::min(hz, maxHz) * incPerHz);
		d.mod.phase += static_cast<uint32_t>(std::min(hz * 1.26f, maxHz) * incPerHz);
		d.mod2.phase += static_cast<uint32_t>(std::min(hz * ratioC, maxHz) * incPerHz);
		d.sub.phase += static_cast<uint32_t>(std::min(hz * 0.52f, maxHz) * incPerHz);
		const float a = sine(d.car.phase + cycles(index * sine(d.sub.phase)));
		const float b = sine(d.mod.phase);
		const float ov = sine(d.mod2.phase + cycles(0.08f * a)) * d.indexEnv;
		d.indexEnv *= overtoneCoef;
		out[i] = kLevel * (a + levelB * b + levelC * ov);
	}
}

// Cyc Tone ------------------------------------------------------------------------------------------------------------
// Two-operator FM on the note: a sine carrier phase modulated by a sine with a little self feedback, which spreads the
// sidebands into a smooth, slowly falling series.
// - Color: the modulator's ratio, knob / 12, so every 12 steps is a whole number ratio (24 = 2, odd harmonics) and the
//   steps between are inharmonic
// - Shape: modulation index, from a pure sine to a hollow, then folded, spectrum
// - Sweep: an extra burst of index at the start that falls away
// - Contour: how fast the index fades, from never (0) to a pluck that settles on a sine (127)

constexpr std::array<float, 5> kToneShapeKnob{0.f, 32.f, 64.f, 96.f, 127.f};
constexpr std::array<float, 5> kToneIndex{0.f, 0.07f, 0.17f, 0.5f, 1.2f}; ///< cycles
constexpr std::array<float, 4> kToneSweepKnob{0.f, 64.f, 96.f, 127.f};
constexpr std::array<float, 4> kToneBurst{0.f, 0.1f, 1.2f, 0.12f};
constexpr std::array<float, 4> kToneBurstTime{0.05f, 0.1f, 0.05f, 2.f};
constexpr float kToneFeedback = 0.127f;
constexpr std::array<float, 5> kToneContourKnob{0.f, 48.f, 64.f, 96.f, 127.f};
constexpr std::array<float, 5> kToneIndexTime{4.6f, 3.f, -1.2f, -3.5f, -4.6f}; ///< ln(seconds to fall 60 dB)

void startTone(Voice& v, const Ctx& c) {
	auto& d = v.s_.tone;
	d = {};
	d.indexEnv = 1.f;
	d.pitchEnv = 1.f; // the sweep's burst
}

void renderTone(Voice& v, const Ctx& c, float* out, int32_t n) {
	auto& d = v.s_.tone;
	const float colorKnob = c.st.macro[0] * 127.f;
	const float shapeKnob = c.st.macro[1] * 127.f;
	const float sweep = c.st.macro[2];
	const float contourKnob = c.st.macro[3] * 127.f;
	const float incPerHz = 4294967296.f / c.sr;
	const float maxHz = 0.45f * c.sr;

	const uint32_t carInc = static_cast<uint32_t>(std::min(c.hz, maxHz) * incPerHz);
	const uint32_t modInc = static_cast<uint32_t>(std::min(c.hz * colorKnob / 12.f, maxHz) * incPerHz);
	const float index = interp(kToneShapeKnob, kToneIndex, shapeKnob);
	const float indexCoef =
	    (contourKnob < 1.f) ? 1.f : decayFor(expf(interp(kToneContourKnob, kToneIndexTime, contourKnob)), c.sr);
	const float burst = interp(kToneSweepKnob, kToneBurst, sweep * 127.f);
	const float burstCoef = decayFor(interp(kToneSweepKnob, kToneBurstTime, sweep * 127.f), c.sr);
	constexpr float kLevel = 0.125f;

	for (int32_t i = 0; i < n; i++) {
		d.car.phase += carInc;
		d.mod.phase += modInc;
		const float idx = index * d.indexEnv + burst * d.pitchEnv;
		d.indexEnv *= indexCoef;
		d.pitchEnv *= burstCoef;
		const float m = sine(d.mod.phase + cycles(kToneFeedback * d.mod.last));
		d.mod.last = m;
		out[i] = kLevel * sine(d.car.phase + cycles(idx * m));
	}
}

// Cyc Chord -----------------------------------------------------------------------------------------------------------
// Four voices playing a chord from one note. Each voice is a sine bent towards a hollow, then a buzzy wave.
// - Color: the voicing, in 8 steps: unison, close, close, first inversion, dropped, second inversion, close with the
//   octave, spread
// - Shape: the chord, one of kNumChordShapes
// - Sweep: the wave, from sine through hollow (odd harmonics) to buzzy (all harmonics)
// - Contour: how fast the wave mellows back towards a sine

/// Semitones above the root for each voice. The same chords in the same order as the Cycles' CHORD machine (measured
/// from its output): three-note chords double the root.
constexpr std::array<std::array<int8_t, 4>, kNumChordShapes> kChords{{
    {0, 0, 12, 19}, // root, octave and fifth
    {0, 0, 0, 0},   // unison
    {0, 0, 0, 0},   // unison
    {0, 3, 7, 0},   // minor
    {0, 4, 7, 0},   // major
    {0, 2, 7, 0},   // sus2
    {0, 5, 7, 0},   // sus4
    {0, 3, 7, 10},  // m7
    {0, 4, 7, 10},  // 7
    {0, 3, 7, 11},  // m(maj7)
    {0, 4, 7, 11},  // maj7
    {0, 5, 7, 10},  // 7sus4
    {0, 3, 6, 9},   // dim7
    {0, 3, 7, 14},  // m(add9)
    {0, 4, 7, 14},  // add9
    {0, 3, 7, 9},   // m6
    {0, 4, 7, 9},   // 6
    {0, 3, 6, 0},   // dim
    {0, 4, 6, 0},   // (b5)
    {0, 3, 6, 10},  // m7b5
    {0, 4, 6, 10},  // 7b5
    {0, 4, 8, 0},   // aug
    {0, 3, 8, 10},  // m7#5
    {0, 4, 8, 10},  // 7#5
    {0, 3, 7, 8},   // m(b6)
    {0, 3, 10, 14}, // m9 (no 5th)
    {0, 4, 10, 14}, // 9 (no 5th)
    {0, 4, 6, 14},  // add9 b5
    {0, 4, 6, 11},  // maj7b5
    {0, 4, 10, 13}, // 7b9 (no 5th)
    {1, 5, 8, 1},   // a half step up, major
    {0, 5, 7, 8},   // sus4 b6
    {0, 4, 6, 7},   // major add b5
    {0, 4, 5, 9},   // 6 add 4
    {0, 4, 9, 11},  // 6 maj7
    {0, 4, 11, 14}, // maj9 (no 5th)
    {0, 5, 12, 17}, // fourths
    {0, 7, 12, 19}, // fifths
}};

/// Octaves added to each voice for each voicing
constexpr std::array<std::array<int8_t, 4>, 8> kVoicings{{
    {0, 0, 0, 0},   // unison: everything on the root
    {0, 0, 0, 0},   // close
    {0, 0, 0, 0},   // close
    {1, 0, 0, 0},   // first inversion: root up
    {0, -1, -1, 0}, // dropped: middle voices down
    {1, 1, 0, 0},   // second inversion
    {0, 0, 0, 1},   // close, top voice up an octave
    {0, 1, 1, 1},   // spread
}};

void startChord(Voice& v, const Ctx& c) {
	auto& d = v.s_.chord;
	d = {};
	d.sweepEnv = 1.f;
	for (size_t i = 0; i < d.osc.size(); i++) {
		d.osc[i].phase = static_cast<uint32_t>(i) * 0x2545F491u;
	}
}

void renderChord(Voice& v, const Ctx& c, float* out, int32_t n) {
	auto& d = v.s_.chord;
	const float colorKnob = c.st.macro[0] * 127.f;
	const int32_t shape =
	    std::clamp<int32_t>(static_cast<int32_t>(c.st.macro[1] * kNumChordShapes), 0, kNumChordShapes - 1);
	const float sweep = c.st.macro[2];
	const float contour = c.st.macro[3];
	const float incPerHz = 4294967296.f / c.sr;
	const float maxHz = 0.45f * c.sr;

	const int32_t voicing = std::clamp<int32_t>(static_cast<int32_t>(colorKnob / 16.f), 0, 7);
	std::array<uint32_t, 4> inc;
	std::array<uint32_t, 4> inc2;
	for (size_t i = 0; i < 4; i++) {
		const int32_t semis = (voicing == 0) ? 0 : kChords[shape][i] + 12 * kVoicings[voicing][i];
		// Unison spreads its voices by a few cents, so it's thick rather than just loud
		const float detune = (voicing == 0) ? (static_cast<float>(i) - 1.5f) * 0.004f : 0.f;
		const float hz = std::min(c.hz * exp2f(static_cast<float>(semis) / 12.f + detune), maxHz);
		inc[i] = static_cast<uint32_t>(hz * incPerHz);
		inc2[i] = static_cast<uint32_t>(std::min(2.f * hz, maxHz) * incPerHz);
	}
	// Sweep: odd harmonics from a modulator at twice the pitch, then all of them from feedback
	const float odd = 0.22f * std::min(sweep * 2.f, 1.f) * std::min(sweep * 2.f, 1.f);
	const float fb = 0.12f * std::max(sweep - 0.3f, 0.f) / 0.7f;
	const float sweepCoef =
	    (contour < 0.2f) ? 1.f : decayFor(0.15f / ((contour - 0.2f) * (contour - 0.2f) + 0.002f), c.sr);
	constexpr float kLevel = 0.045f;

	for (int32_t i = 0; i < n; i++) {
		const float bright = d.sweepEnv;
		d.sweepEnv *= sweepCoef;
		float sum = 0.f;
		for (size_t k = 0; k < 4; k++) {
			d.osc[k].phase += inc[k];
			d.mod[k].phase += inc2[k];
			const float x = sine(d.osc[k].phase + cycles(bright * (odd * sine(d.mod[k].phase) + fb * d.osc[k].last)));
			d.osc[k].last = x;
			sum += x;
		}
		out[i] = kLevel * sum;
	}
}

// Vtg Snr -------------------------------------------------------------------------------------------------------------
// A vintage-style snare: a two-operator FM body on the note with a pitch sweep, and a cluster of high inharmonic modes
// (fixed in pitch, around 3 to 6 kHz) roughened by noise, the snares.
// - Inharm: the body's modulator ratio, from harmonic to clangy
// - Freq Complex: how strong and dense the high cluster is against the body
// - Pitch Sweep: depth and length of the body's pitch drop
// - Mod Envelope: depth and length of the body's modulation, the knock in the attack

constexpr std::array<float, 4> kVsnrModes{2870.f, 3730.f, 4610.f, 5330.f};

void startVsnr(Voice& v, const Ctx& c) {
	auto& d = v.s_.vsnr;
	d = {};
	d.pitchEnv = 1.f;
	d.modEnv = 1.f;
	d.clusterEnv = 1.f;
	for (size_t i = 0; i < d.cluster.size(); i++) {
		d.cluster[i].phase = static_cast<uint32_t>(i) * 0x2545F491u;
	}
}

void renderVsnr(Voice& v, const Ctx& c, float* out, int32_t n) {
	auto& d = v.s_.vsnr;
	const float inharm = c.st.macro[0];
	const float complex = c.st.macro[1];
	const float sweep = c.st.macro[2];
	const float menv = c.st.macro[3];
	const float incPerHz = 4294967296.f / c.sr;
	const float maxHz = 0.45f * c.sr;

	const float depth = 0.75f * sweep * sweep;
	const float pitchCoef = decayFor(0.02f + 0.12f * sweep * sweep, c.sr);
	const float ratio = 1.f + 1.41f * inharm;
	const float modDepth = 0.25f * menv;
	const float modCoef = decayFor(0.01f + 0.15f * menv * menv, c.sr);
	// The body follows the amp decay. The snares have their own 0.25 s decay, and only feel the amp decay half as much,
	// so at short decays they ring past the body and at long ones the body rings past them. The common amp envelope is
	// applied after, so it's divided back out here.
	const float clusterCoef = decayFor(0.25f, c.sr);
	float amp = v.common_.amp;
	const float ampCoef = v.common_.ampCoef;
	std::array<uint32_t, 4> modeInc;
	for (size_t i = 0; i < 4; i++) {
		modeInc[i] = static_cast<uint32_t>(std::min(kVsnrModes[i], maxHz) * incPerHz);
	}
	const float clusterLevel = 0.15f * (0.25f + 0.75f * complex);
	const float roughness = 0.05f + 0.25f * complex;
	const SvfCoefs bp = svfCoefs(3500.f, 0.9f, c.sr);
	constexpr float kBody = 0.3f;

	for (int32_t i = 0; i < n; i++) {
		const float hz = c.hz * (1.f + depth * d.pitchEnv);
		d.pitchEnv *= pitchCoef;
		d.body.phase += static_cast<uint32_t>(std::min(hz, maxHz) * incPerHz);
		d.bodyMod.phase += static_cast<uint32_t>(std::min(hz * ratio, maxHz) * incPerHz);
		const float body = sine(d.body.phase + cycles(modDepth * d.modEnv * sine(d.bodyMod.phase)));
		d.modEnv *= modCoef;

		// The snares: four modes, each phase-jittered by band passed noise
		const float nz = svf(d.noiseBp, bp, noise(v.common_.rng)).bp;
		float cl = 0.f;
		for (size_t k = 0; k < 4; k++) {
			d.cluster[k].phase += modeInc[k];
			cl += sine(d.cluster[k].phase + cycles(roughness * nz * static_cast<float>(k + 1)));
		}
		const float snares = std::min(d.clusterEnv, sqrtf(amp)) / std::max(amp, 1e-5f);
		out[i] = kBody * body + clusterLevel * snares * (0.25f * cl + 1.5f * nz);
		d.clusterEnv *= clusterCoef;
		amp *= ampCoef;
	}
}

// clap eng ------------------------------------------------------------------------------------------------------------
// Three quick claps of band passed noise, then a tail through a resonant body that drops fast, then rings on. The amp
// decay shapes only the tail, so the claps stay the same at any decay. The note moves the body and filters.
// - Body Char: brightness of the claps and the body's resonance, from a dull thud to a ringing box
// - Balance: the tail against the claps, from all tail to claps alone
// - Spacing Crunch: the gap between claps (6 to 24 ms), and grit in the noise as it goes up
// - Body Envelope: how fast the tail darkens

constexpr int32_t kClapBursts = 3;

void startClap(Voice& v, const Ctx& c) {
	auto& d = v.s_.clap;
	d = {};
	d.burstEnv = 1.f;
	d.tailEnv = 1.f;
	d.bodyEnv = 1.f;
	d.burstIndex = 0;
	d.samplesToBurst = 0;
	const float spacing = 0.006f + 0.018f * c.st.macro[2];
	v.common_.ampHold = static_cast<uint32_t>(spacing * kClapBursts * c.sr);
}

void renderClap(Voice& v, const Ctx& c, float* out, int32_t n) {
	auto& d = v.s_.clap;
	const float bodyChar = c.st.macro[0];
	const float balance = c.st.macro[1];
	const float crunch = c.st.macro[2];
	const float bodyEnvKnob = c.st.macro[3];
	const float pitch = c.hz / 261.63f; // the note moves the filters, C4 = as measured

	const int32_t spacing = static_cast<int32_t>((0.006f + 0.018f * crunch) * c.sr);
	const float burstCoef = decayFor(0.022f, c.sr);
	const SvfCoefs bp = svfCoefs(1000.f * pitch, 1.1f, c.sr);
	const SvfCoefs bp2 = svfCoefs(2400.f * pitch, 1.5f, c.sr);
	const SvfCoefs body = svfCoefs(900.f * pitch, 1.f + 9.f * bodyChar, c.sr);
	const float bodyGain = 1.f / (1.f + 4.f * bodyChar); // a band pass peaks at its Q
	const float darkCoef = decayFor(0.05f + 1.5f * (1.f - bodyEnvKnob) * (1.f - bodyEnvKnob), c.sr);
	const float burstLevel = 0.4f * (0.8f + 0.4f * balance);
	const float tailLevel = 0.4f * (1.f - balance * balance);
	const float burstBright = 0.3f + 1.5f * bodyChar * bodyChar;
	const float tailFastCoef = decayFor(0.2f, c.sr);        // the tail's first drop, before the amp decay takes over
	const float grit = std::max(crunch - 0.4f, 0.f) * 1.6f; // sample-and-hold roughness
	const int32_t holdSamples = 1 + static_cast<int32_t>(grit * 6.f);

	for (int32_t i = 0; i < n; i++) {
		if (d.burstIndex < kClapBursts && --d.samplesToBurst <= 0) {
			d.burstEnv = 1.f;
			d.burstIndex++;
			d.samplesToBurst = spacing;
		}
		if (d.body.phase-- == 0) {
			d.body.last = noise(v.common_.rng);
			d.body.phase = static_cast<uint32_t>(holdSamples - 1);
		}
		const float nz = d.body.last;
		const auto b1 = svf(d.bp1, bp, nz);
		const auto b2 = svf(d.bp2, bp2, nz);
		const float bright = b1.bp + 0.5f * b2.bp * d.bodyEnv;
		const float bursts = (d.burstIndex > 0) ? burstLevel * d.burstEnv * (b1.bp + burstBright * b2.bp) : 0.f;
		d.burstEnv *= burstCoef;
		const bool tailOn = d.burstIndex >= kClapBursts;
		const float tail =
		    tailOn ? tailLevel * (0.2f + 0.8f * d.tailEnv) * (bright + bodyGain * svf(d.hp, body, bright).bp) : 0.f;
		if (tailOn) {
			d.bodyEnv *= darkCoef;
			d.tailEnv *= tailFastCoef;
		}
		out[i] = bursts + tail;
	}
}

} // namespace

// Voice ---------------------------------------------------------------------------------------------------------------

void Voice::reset() {
	*this = Voice{};
	kind_ = Kind::NUM_KINDS;
	pendingTrigger_ = false;
	finished_ = true;
}

void Voice::start(Kind kind, const Settings& settings, uint32_t phaseIncrement, float sampleRate) {
	kind_ = kind;
	finished_ = false;
	const uint32_t seed = common_.rng;
	common_ = Common{};
	common_.rng = (seed != 0) ? seed : 0x9E3779B9u;
	common_.amp = 1.f;
	common_.punchEnv = 1.f;
	const Ctx c{settings, sampleRate, static_cast<float>(phaseIncrement) * kPhaseToCycles * sampleRate};
	switch (kind) {
	case Kind::KIK:
		startKik(*this, c);
		break;
	case Kind::SNR:
		startSnr(*this, c);
		break;
	case Kind::METAL:
		startMetal(*this, c);
		break;
	case Kind::PERC:
		startPerc(*this, c);
		break;
	case Kind::TONE:
		startTone(*this, c);
		break;
	case Kind::CHORD:
		startChord(*this, c);
		break;
	case Kind::VSNR:
		startVsnr(*this, c);
		break;
	case Kind::CLAP:
		startClap(*this, c);
		break;
	case Kind::NUM_KINDS:
		break;
	}
}

void Voice::render(Kind kind, const Settings& settings, uint32_t phaseIncrement, float sampleRate, int32_t* out,
                   int32_t numSamples) {
	if (pendingTrigger_ || kind != kind_) {
		const bool trigger = pendingTrigger_;
		pendingTrigger_ = false;
		if (!trigger && kind_ == Kind::NUM_KINDS) {
			// Never triggered: nothing to play
			std::fill(out, out + numSamples, 0);
			return;
		}
		start(kind, settings, phaseIncrement, sampleRate);
	}
	if (finished_) {
		std::fill(out, out + numSamples, 0);
		return;
	}

	const Ctx c{settings, sampleRate, static_cast<float>(phaseIncrement) * kPhaseToCycles * sampleRate};
	const DecayCurve& curve = decayCurveFor(kind);
	const bool hold = curve.holdsAtTop && settings.decay >= kHoldKnob;
	const float punchStretch = settings.punch ? 1.25f : 1.f;
	common_.ampCoef = hold ? 1.f : decayFor(ampDecayTime(curve, settings.decay * 127.f) * punchStretch, sampleRate);

	constexpr int32_t kChunk = 64;
	float buf[kChunk];
	while (numSamples > 0) {
		const int32_t n = std::min(numSamples, kChunk);
		switch (kind) {
		case Kind::KIK:
			renderKik(*this, c, buf, n);
			break;
		case Kind::SNR:
			renderSnr(*this, c, buf, n);
			break;
		case Kind::METAL:
			renderMetal(*this, c, buf, n);
			break;
		case Kind::PERC:
			renderPerc(*this, c, buf, n);
			break;
		case Kind::TONE:
			renderTone(*this, c, buf, n);
			break;
		case Kind::CHORD:
			renderChord(*this, c, buf, n);
			break;
		case Kind::VSNR:
			renderVsnr(*this, c, buf, n);
			break;
		case Kind::CLAP:
			renderClap(*this, c, buf, n);
			break;
		case Kind::NUM_KINDS:
			std::fill(buf, buf + n, 0.f);
			break;
		}

		// Punch: a soft clipper driven hard at the start and easing off over
		// about 60 ms, which squares up and lifts the attack like a compressor slamming. The amp decay also runs a
		// quarter longer.
		if (settings.punch) {
			const float punchCoef = decayFor(0.18f, sampleRate);
			float p = common_.punchEnv;
			for (int32_t i = 0; i < n; i++) {
				const float drive = 2.f + 2.f * p;
				buf[i] = softClip(buf[i] * drive) * (1.f + 0.5f * p) / drive;
				p *= punchCoef;
			}
			common_.punchEnv = p;
		}

		// Level trim, then drive after the amp envelope, so the tail cleans up as it dies away
		float amp = common_.amp;
		const float coef = common_.ampCoef;
		const float trim = kLevelTrim[static_cast<size_t>(kind)];
		const bool driven = settings.drive > 0.f;
		const float gain = driveGainFor(settings.drive);
		for (int32_t i = 0; i < n; i++) {
			// Below -100 dB the voice is done, at the same sample whatever the host's block size
			if (amp < 1e-5f) {
				std::fill(out + i, out + numSamples, 0);
				finished_ = true;
				common_.amp = 0.f;
				return;
			}
			float x = buf[i] * amp * trim;
			if (driven) {
				x = driveCurve(x * gain);
			}
			out[i] = static_cast<int32_t>(std::clamp(x, -1.f, 1.f) * kFullScale);
			if (common_.ampHold > 0) {
				common_.ampHold--;
			}
			else {
				amp *= coef;
			}
		}
		common_.amp = amp;
		out += n;
		numSamples -= n;
	}
}

float ampDecaySeconds(Kind kind, float knob, bool punch) {
	const DecayCurve& curve = decayCurveFor(kind);
	if (curve.holdsAtTop && knob / 127.f >= kHoldKnob) {
		return 1000.f;
	}
	return ampDecayTime(curve, knob) * (punch ? 1.25f : 1.f);
}

} // namespace deluge::dsp::drum_synth::cyc
