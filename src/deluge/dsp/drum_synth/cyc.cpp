/*
 * Cyc engines. See cyc.h for what these are.
 *
 * Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dsp/drum_synth/cyc.h"
#include <algorithm>
#include <cmath>
#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace deluge::dsp::drum_synth::cyc {

namespace cyc_detail {

constexpr float kPhaseToCycles = 1.f / 4294967296.f;
constexpr float kFullScale = 2147483520.f; ///< The largest float below 2^31, so clipping can't wrap

// The oscillators are integer: a phase is a uint32, 2^32 to the cycle, and a sine comes out q31. Phase modulation adds
// a fixed point offset to the phase, so the 32-bit wrap takes it into the cycle for nothing.

constexpr int32_t kSineBits = 10; ///< 1024 steps to the cycle: linear interpolation between them is good to -106 dB
constexpr int32_t kSineSize = 1 << kSineBits;

/// sin(x) for x in [-pi, pi], as a Taylor series in double, to build the table at compile time
constexpr double taylorSine(double x) {
	double term = x;
	double sum = x;
	for (int32_t n = 1; n < 30; n++) {
		term *= -x * x / static_cast<double>((2 * n) * (2 * n + 1));
		sum += term;
	}
	return sum;
}

/// One cycle of sine, q31, with the first entry again at the end so interpolation can read one past
constexpr std::array<int32_t, kSineSize + 1> makeSineTable() {
	std::array<int32_t, kSineSize + 1> table{};
	constexpr double kPi = 3.14159265358979323846;
	for (int32_t i = 0; i <= kSineSize; i++) {
		const int32_t folded = (i <= kSineSize / 2) ? i : i - kSineSize; // into [-pi, pi]
		const double y = taylorSine(2. * kPi * folded / kSineSize) * 2147483647.;
		table[i] = static_cast<int32_t>(y + ((y >= 0.) ? 0.5 : -0.5));
	}
	return table;
}
constexpr std::array<int32_t, kSineSize + 1> kSineTable = makeSineTable();

/// sin(2 pi phase / 2^32), q31, from the table with linear interpolation
inline int32_t sine(uint32_t phase) {
	const uint32_t i = phase >> (32 - kSineBits);
	const int32_t a = kSineTable[i];
	const int32_t b = kSineTable[i + 1];
	const int32_t f = static_cast<int32_t>((phase << kSineBits) >> 1); // the way from a to b, q31
	return a + 2 * static_cast<int32_t>((static_cast<int64_t>(b - a) * f) >> 32);
}

/// q31 to a float, -1 to 1
inline float toFloat(int32_t x) {
	return static_cast<float>(x) * (1.f / 2147483648.f);
}

/// A phase modulation depth in cycles, under 4 either way, as q29 for pmOffset()
constexpr int32_t depthQ29(float cycles) {
	return static_cast<int32_t>(cycles * 536870912.f);
}

/// The phase offset of a modulator x (q31) at a depth (q29 cycles): x * depth cycles, wrapped into the cycle
inline uint32_t pmOffset(int32_t x, int32_t depth) {
	return static_cast<uint32_t>(static_cast<int32_t>((static_cast<int64_t>(x) * depth) >> 32)) << 4;
}

/// A phase offset in cycles, under 8 either way, wrapped into the cycle
inline uint32_t cyclesOffset(float c) {
	return static_cast<uint32_t>(static_cast<int32_t>(c * 268435456.f)) << 4;
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

/// Step the noise on n samples without using them, so the noise after is as if they had been
inline void skipNoise(uint32_t& state, int32_t n) {
	for (int32_t i = 0; i < n; i++) {
		state ^= state << 13;
		state ^= state >> 17;
		state ^= state << 5;
	}
}

/// A TPT state variable filter, after Andrew Simper's
using SvfCoefs = Voice::SvfCoefs;

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

/// Samples per chunk. Which layers still sound is looked at once a chunk, the chunks counted from the start of the
/// note, so it sounds the same at any host block size.
constexpr int32_t kChunk = 64;
/// A voice ends once its level is below -72 dB, fading out over a chunk so it doesn't click
constexpr float kEndLevel = 2.5118864e-4f;
/// An envelope times the deepest sweep below this leaves 1 + depth * envelope exactly 1: the sweep is over to the bit
constexpr float kSweepOver = 0x1p-24f;
/// A layer whose part of the output has fallen below this, at full scale, is left out: -110 dB, so a few left out
/// together still come to less than -100 dB
constexpr float kInaudible = 3e-6f;
/// The most the output stage can raise an engine's output: full drive, and punch at its start
constexpr float kMaxOutputGain = 5.6234133f * 1.5f;

} // namespace cyc_detail

using namespace cyc_detail;

// Engines -------------------------------------------------------------------------------------------------------------
// Each renders mono float samples, about -1 to 1, before the common amp envelope. prepare*() works out what the
// settings make, only when they change; render*() runs a chunk or part of one, chunkStart at the start of a chunk.

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

/// A part of an engine's output below this, before the level trim, is inaudible (see kInaudible)
constexpr float goneFor(Kind kind) {
	return kInaudible / (kLevelTrim[static_cast<size_t>(kind)] * kMaxOutputGain);
}

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
constexpr float kKikLevel = 0.23f;
constexpr float kKikMaxDepth = 22.f; ///< Deeper than the deepest sweep, e^3.09 - 1
constexpr float kKikMaxKnock = 0.54f;

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

void prepareKik(Voice& v, const Ctx& c) {
	auto& p = v.p_.e.kik;
	const float color = c.st.macro[0];
	const float shape = c.st.macro[1];
	const float sweepKnob = c.st.macro[2] * 127.f;
	const float contour = c.st.macro[3];

	p.fEnd = c.hz * 0.25f;
	p.depth = expf(interp(kKikSweepKnob, kKikSweepDepth, sweepKnob)) - 1.f;
	p.pitchCoef = decayFor(interp(kKikSweepKnob, kKikSweepTime, sweepKnob) * 6.907755f
	                           * sweepTimeScale(contour, kKikContourDefault),
	                       c.sr);
	p.incPerHz = 4294967296.f / c.sr;
	p.maxHz = 0.45f * c.sr;
	p.restInc = static_cast<uint32_t>(std::min(p.fEnd, p.maxHz) * p.incPerHz);

	// Shape: spikes, then feedback
	p.spikeAmount = interp(kKikShapeKnob, kKikSpike, shape * 127.f);
	p.spikeNorm = 1.f / (1.f + p.spikeAmount);
	p.spikeNoise = interp(kKikShapeKnob, kKikSpikeNoise, shape * 127.f);
	// The spikes reach the output through Color's drive (up to 6 times) and the level
	p.spikes = (std::abs(p.spikeAmount) + std::abs(p.spikeNoise)) * 1.05f * 6.f * kKikLevel >= goneFor(Kind::KIK);
	// Color: a blend from clean into a soft clipper driven harder as it comes in
	p.drive = 1.f + 5.f * color;
	p.driveNorm = color / softClip(p.drive);
	p.dry = 1.f - color;
	p.colored = color != 0.f;
	// Contour: knock level
	p.knock = 0.04f + 0.5f * contour * contour;
	p.clickCoef = decayFor(0.004f, c.sr);
}

void renderKik(Voice& v, float* out, int32_t n, bool chunkStart) {
	auto& k = v.s_.kik;
	const auto& p = v.p_.e.kik;
	if (k.pitchEnv * kKikMaxDepth < kSweepOver) {
		k.pitchEnv = 0.f; // Over, at any Sweep
	}
	if (chunkStart && kKikMaxKnock * 2.f * k.clickEnv < goneFor(Kind::KIK)) {
		k.clickEnv = 0.f; // The knock is gone
	}
	const bool sweeping = k.pitchEnv != 0.f;
	const bool full = p.spikes || k.clickEnv != 0.f; // Spikes or knock, both riding on the noise

	for (int32_t i = 0; i < n; i++) {
		if (sweeping) {
			const float hz = p.fEnd * (1.f + p.depth * k.pitchEnv);
			k.pitchEnv *= p.pitchCoef;
			k.body.phase += static_cast<uint32_t>(std::min(hz, p.maxHz) * p.incPerHz);
		}
		else {
			k.body.phase += p.restInc;
		}
		float x = toFloat(sine(k.body.phase));
		float click = 0.f;
		if (full) {
			const float nz = noise(v.common_.rng);
			const float sp = spike(x);
			x = (x + (p.spikeAmount + p.spikeNoise * nz) * sp) * p.spikeNorm;
			// The knock: noise through a one-pole high pass, dying in a few ms
			const float hp = nz - k.fold;
			k.fold = nz;
			click = p.knock * k.clickEnv * hp;
			k.clickEnv *= p.clickCoef;
		}
		else {
			x *= p.spikeNorm;
		}
		if (p.colored) {
			x = p.dry * x + softClip(p.drive * x) * p.driveNorm;
		}
		out[i] = kKikLevel * x + click;
	}
	if (!full) {
		skipNoise(v.common_.rng, n);
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

constexpr float kSnrBody = 0.3f;
constexpr float kSnrRattle = 0.035f;
constexpr float kSnrNoise = 0.05f;
constexpr float kSnrBurst = 0.045f;
constexpr float kSnrMaxDepth = 2.5f;
constexpr float kSnrMaxIndex = 1.2f;
constexpr float kSnrMaxNoise = 3.f; ///< The most the high passed noise reaches

void startSnr(Voice& v, const Ctx& c) {
	auto& d = v.s_.snr;
	d = {};
	d.pitchEnv = 1.f;
	d.rattleEnv = 1.f;
	d.noiseEnv = 1.f;
	d.bodyEnv = 1.f;
}

void prepareSnr(Voice& v, const Ctx& c) {
	auto& p = v.p_.e.snr;
	const float color = c.st.macro[0];
	const float shape = c.st.macro[1];
	const float sweep = c.st.macro[2];
	const float contour = c.st.macro[3];
	p.incPerHz = 4294967296.f / c.sr;
	p.maxHz = 0.45f * c.sr;

	p.depth = 2.2f * powf(sweep, 0.8f);
	p.pitchCoef = decayFor(0.004f + 0.05f * sweep, c.sr);
	p.restInc = static_cast<uint32_t>(std::min(c.hz, p.maxHz) * p.incPerHz);
	p.modInc = static_cast<uint32_t>(std::min(2.f * c.hz, p.maxHz) * p.incPerHz);
	const float ratio = 3.f * powf(2.f, powf(color, 1.4f));
	p.carInc = static_cast<uint32_t>(std::min(ratio * c.hz, p.maxHz) * p.incPerHz);
	p.index = 0.1f + 1.1f * shape * shape;
	p.noiseAmount = powf(shape, 1.5f);
	// Contour: the rattle's index and noise fall away, from lasting the whole hit (0) to 70 ms (127)
	p.rattleCoef = (contour < 0.01f) ? 1.f : decayFor(0.07f / (contour * contour * 0.99f + 0.01f), c.sr);
	// The body cracks and dies quickly; the rattle carries the tail
	p.bodyCoef = decayFor(0.045f + 0.05f * sweep, c.sr);
	p.attackStep = 1.f / (0.0015f * c.sr);
	p.hp = svfCoefs(500.f, 0.7f, c.sr);
}

void renderSnr(Voice& v, float* out, int32_t n, bool chunkStart) {
	auto& d = v.s_.snr;
	const auto& p = v.p_.e.snr;
	const float noteHz = v.p_.hz;
	if (d.pitchEnv * kSnrMaxDepth < kSweepOver) {
		d.pitchEnv = 0.f;
	}
	if (chunkStart) {
		const float gone = goneFor(Kind::SNR);
		if (kSnrBody * d.bodyEnv < gone) {
			d.bodyEnv = 0.f; // The body is gone
		}
		// The rattle's modulation moves its phase by up to index * rattleEnv cycles; the noise on it is quieter still
		if (kSnrRattle * 6.2831853f * kSnrMaxIndex * d.rattleEnv < gone) {
			d.rattleEnv = 0.f;
		}
		if (kSnrBurst * kSnrMaxNoise * d.noiseEnv < gone) {
			d.noiseEnv = 0.f;
		}
	}
	const bool bodyOn = d.bodyEnv != 0.f;
	const bool sweeping = d.pitchEnv != 0.f;
	const bool rattleMod = d.rattleEnv != 0.f;
	const bool noiseOn = (rattleMod && p.noiseAmount != 0.f) || d.noiseEnv != 0.f;

	for (int32_t i = 0; i < n; i++) {
		d.attack = std::min(d.attack + p.attackStep, 1.f);
		float body = 0.f;
		if (bodyOn) {
			if (sweeping) {
				const float hz = noteHz * (1.f + p.depth * d.pitchEnv);
				d.pitchEnv *= p.pitchCoef;
				d.body.phase += static_cast<uint32_t>(std::min(hz, p.maxHz) * p.incPerHz);
			}
			else {
				d.body.phase += p.restInc;
			}
			body = toFloat(sine(d.body.phase)) * d.bodyEnv * d.attack;
			d.bodyEnv *= p.bodyCoef;
		}

		d.mod.phase += p.modInc;
		d.rattle[0].phase += p.carInc;
		const float rattle =
		    toFloat(rattleMod ? sine(d.rattle[0].phase + pmOffset(sine(d.mod.phase), depthQ29(p.index * d.rattleEnv)))
		                      : sine(d.rattle[0].phase));
		float noisy = 0.f;
		if (noiseOn) {
			const float nz = svf(d.noiseBp, p.hp, noise(v.common_.rng)).hp;
			noisy = (kSnrNoise * p.noiseAmount * d.rattleEnv + kSnrBurst * d.noiseEnv) * nz;
			d.noiseEnv *= p.bodyCoef;
		}
		d.rattleEnv *= p.rattleCoef;
		out[i] = kSnrBody * body + d.attack * (kSnrRattle * rattle + noisy);
	}
	if (!noiseOn) {
		skipNoise(v.common_.rng, n);
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
constexpr float kMetalMaxDepth = 1.f;

void startMetal(Voice& v, const Ctx& c) {
	auto& m = v.s_.metal;
	m = {};
	m.pitchEnv = 1.f;
	m.brightEnv = 1.f;
	for (size_t i = 0; i < m.osc.size(); i++) {
		m.osc[i].phase = static_cast<uint32_t>(i) * 0x2545F491u; // spread the start phases
	}
}

void prepareMetal(Voice& v, const Ctx& c) {
	auto& p = v.p_.e.metal;
	const float color = c.st.macro[0];
	const float shape = c.st.macro[1];
	const float sweep = c.st.macro[2];
	const float contour = c.st.macro[3];
	const float incPerHz = 4294967296.f / c.sr;

	const float base = c.hz * 5.03f;
	for (size_t i = 0; i < p.inc.size(); i++) {
		const float ratio = kMetalRatioLo[i] + color * (kMetalRatioHi[i] - kMetalRatioLo[i]);
		p.inc[i] = std::min(base * ratio, 0.49f * c.sr) * incPerHz;
		p.restInc[i] = static_cast<uint32_t>(p.inc[i]);
	}
	const float index = 0.25f + 1.1f * shape * shape;
	p.index = depthQ29(index);
	p.halfIndex = depthQ29(0.5f * index);
	p.depth = 0.9f * sweep * sweep;
	p.pitchCoef = decayFor(0.02f + 0.06f * sweep, c.sr);
	p.brightCoef = (contour < 0.01f) ? 1.f : decayFor(0.03f / (contour * contour * 0.99f + 0.01f), c.sr);
	p.hp = svfCoefs(std::clamp(base * 1.5f, 300.f, 6000.f), 0.6f, c.sr);
	p.ring = svfCoefs(std::clamp(base * 2.5f, 500.f, 12000.f), 4.f, c.sr);
	// Contour's low pass closes from open to about 5 kHz
	p.cutLo = 1.f - expf(-6.2831853f * 4000.f / c.sr);
	p.cutHi = 1.f - expf(-6.2831853f * std::min(15000.f, 0.4f * c.sr) / c.sr);
}

void renderMetal(Voice& v, float* out, int32_t n, bool chunkStart) {
	auto& m = v.s_.metal;
	const auto& p = v.p_.e.metal;
	constexpr float kLevel = 0.14f;
	if (m.pitchEnv * kMetalMaxDepth < kSweepOver) {
		m.pitchEnv = 0.f;
	}
	const bool sweeping = m.pitchEnv != 0.f;

	for (int32_t i = 0; i < n; i++) {
		if (sweeping) {
			const float bend = 1.f + p.depth * m.pitchEnv;
			m.pitchEnv *= p.pitchCoef;
			for (size_t k = 0; k < 6; k++) {
				m.osc[k].phase += static_cast<uint32_t>(p.inc[k] * bend);
			}
		}
		else {
			for (size_t k = 0; k < 6; k++) {
				m.osc[k].phase += p.restInc[k];
			}
		}
		// Pairs: 1 > 0, 3 > 2, 5 > 4, and each pair's output also rocks the next pair's modulator
		const int32_t a = sine(m.osc[1].phase + pmOffset(m.osc[4].last, p.halfIndex));
		const int32_t x0 = sine(m.osc[0].phase + pmOffset(a, p.index));
		const int32_t b = sine(m.osc[3].phase + pmOffset(x0, p.halfIndex));
		const int32_t x1 = sine(m.osc[2].phase + pmOffset(b, p.index));
		const int32_t d = sine(m.osc[5].phase + pmOffset(x1, p.halfIndex));
		const int32_t x2 = sine(m.osc[4].phase + pmOffset(d, p.index));
		m.osc[4].last = x2;
		float x = toFloat((x0 >> 2) + (x1 >> 2) + (x2 >> 2)) * (4.f * 0.4f);
		x = svf(m.hp, p.hp, x).hp;
		x += 0.35f * svf(m.ring, p.ring, x).bp; // the cymbal's ringing band
		// Contour: a one-pole low pass closing from wide open, so the top dies first
		const float cut = p.cutLo + (p.cutHi - p.cutLo) * m.brightEnv;
		m.brightEnv *= p.brightCoef;
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
constexpr float kPercLevel = 0.1f;
constexpr float kPercMaxDepth = 1.25f;
constexpr float kPercMaxOvertone = 1.2f;

void startPerc(Voice& v, const Ctx& c) {
	auto& d = v.s_.perc;
	d = {};
	d.pitchEnv = 1.f;
	d.indexEnv = 1.f;
}

void preparePerc(Voice& v, const Ctx& c) {
	auto& p = v.p_.e.perc;
	const float color = c.st.macro[0];
	const float shape = c.st.macro[1];
	const float sweepKnob = c.st.macro[2] * 127.f;
	const float contour = c.st.macro[3];
	p.incPerHz = 4294967296.f / c.sr;
	p.maxHz = 0.45f * c.sr;

	p.depth = interp(kPercSweepKnob, kPercSweepDepth, sweepKnob);
	p.pitchCoef = decayFor(interp(kPercSweepKnob, kPercSweepTime, sweepKnob) * 6.907755f
	                           * sweepTimeScale(contour, kPercContourDefault),
	                       c.sr);
	p.index = depthQ29(0.24f * shape * shape);
	p.levelB = 0.9f * (1.f - 0.75f * shape);
	p.ratioC = 1.f + 6.f * powf(color, 1.3f);
	p.levelC = 1.2f * powf(color, 0.8f);
	p.overtoneCoef = decayFor(0.08f + 6.f * (1.f - contour) * (1.f - contour), c.sr);
	p.restInc[0] = static_cast<uint32_t>(std::min(c.hz, p.maxHz) * p.incPerHz);
	p.restInc[1] = static_cast<uint32_t>(std::min(c.hz * 1.26f, p.maxHz) * p.incPerHz);
	p.restInc[2] = static_cast<uint32_t>(std::min(c.hz * p.ratioC, p.maxHz) * p.incPerHz);
	p.restInc[3] = static_cast<uint32_t>(std::min(c.hz * 0.52f, p.maxHz) * p.incPerHz);
}

void renderPerc(Voice& v, float* out, int32_t n, bool chunkStart) {
	auto& d = v.s_.perc;
	const auto& p = v.p_.e.perc;
	const float noteHz = v.p_.hz;
	if (d.pitchEnv * kPercMaxDepth < kSweepOver) {
		d.pitchEnv = 0.f;
	}
	if (chunkStart && kPercLevel * kPercMaxOvertone * d.indexEnv < goneFor(Kind::PERC)) {
		d.indexEnv = 0.f; // The overtone is gone
	}
	const bool sweeping = d.pitchEnv != 0.f;
	const bool modulated = p.index != 0;
	const bool overtone = p.levelC != 0.f && d.indexEnv != 0.f;

	for (int32_t i = 0; i < n; i++) {
		if (sweeping) {
			const float hz = noteHz * (1.f + p.depth * d.pitchEnv);
			d.pitchEnv *= p.pitchCoef;
			d.car.phase += static_cast<uint32_t>(std::min(hz, p.maxHz) * p.incPerHz);
			d.mod.phase += static_cast<uint32_t>(std::min(hz * 1.26f, p.maxHz) * p.incPerHz);
			d.mod2.phase += static_cast<uint32_t>(std::min(hz * p.ratioC, p.maxHz) * p.incPerHz);
			d.sub.phase += static_cast<uint32_t>(std::min(hz * 0.52f, p.maxHz) * p.incPerHz);
		}
		else {
			d.car.phase += p.restInc[0];
			d.mod.phase += p.restInc[1];
			d.mod2.phase += p.restInc[2];
			d.sub.phase += p.restInc[3];
		}
		const int32_t a = modulated ? sine(d.car.phase + pmOffset(sine(d.sub.phase), p.index)) : sine(d.car.phase);
		const float b = toFloat(sine(d.mod.phase));
		const float ov = overtone ? toFloat(sine(d.mod2.phase + pmOffset(a, depthQ29(0.08f)))) * d.indexEnv : 0.f;
		d.indexEnv *= p.overtoneCoef;
		out[i] = kPercLevel * (toFloat(a) + p.levelB * b + p.levelC * ov);
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
constexpr float kToneLevel = 0.125f;
constexpr float kToneMaxIndex = 1.2f; ///< The most of the index and of the burst

void startTone(Voice& v, const Ctx& c) {
	auto& d = v.s_.tone;
	d = {};
	d.indexEnv = 1.f;
	d.pitchEnv = 1.f; // the sweep's burst
}

void prepareTone(Voice& v, const Ctx& c) {
	auto& p = v.p_.e.tone;
	const float colorKnob = c.st.macro[0] * 127.f;
	const float shapeKnob = c.st.macro[1] * 127.f;
	const float sweep = c.st.macro[2];
	const float contourKnob = c.st.macro[3] * 127.f;
	const float incPerHz = 4294967296.f / c.sr;
	const float maxHz = 0.45f * c.sr;

	p.carInc = static_cast<uint32_t>(std::min(c.hz, maxHz) * incPerHz);
	p.modInc = static_cast<uint32_t>(std::min(c.hz * colorKnob / 12.f, maxHz) * incPerHz);
	p.index = interp(kToneShapeKnob, kToneIndex, shapeKnob);
	p.indexCoef =
	    (contourKnob < 1.f) ? 1.f : decayFor(expf(interp(kToneContourKnob, kToneIndexTime, contourKnob)), c.sr);
	p.burst = interp(kToneSweepKnob, kToneBurst, sweep * 127.f);
	p.burstCoef = decayFor(interp(kToneSweepKnob, kToneBurstTime, sweep * 127.f), c.sr);
}

void renderTone(Voice& v, float* out, int32_t n, bool chunkStart) {
	auto& d = v.s_.tone;
	const auto& p = v.p_.e.tone;
	if (chunkStart) {
		// The modulation moves the carrier's phase by up to idx cycles
		const float gone = goneFor(Kind::TONE) / (kToneLevel * 6.2831853f * kToneMaxIndex);
		if (d.indexEnv < gone) {
			d.indexEnv = 0.f;
		}
		if (d.pitchEnv < gone) {
			d.pitchEnv = 0.f;
		}
	}
	const bool modulated = (p.index != 0.f && d.indexEnv != 0.f) || (p.burst != 0.f && d.pitchEnv != 0.f);

	if (modulated) {
		for (int32_t i = 0; i < n; i++) {
			d.car.phase += p.carInc;
			d.mod.phase += p.modInc;
			const float idx = p.index * d.indexEnv + p.burst * d.pitchEnv;
			d.indexEnv *= p.indexCoef;
			d.pitchEnv *= p.burstCoef;
			const int32_t m = sine(d.mod.phase + pmOffset(d.mod.last, depthQ29(kToneFeedback)));
			d.mod.last = m;
			out[i] = kToneLevel * toFloat(sine(d.car.phase + pmOffset(m, depthQ29(idx))));
		}
	}
	else {
		// No index: just the carrier
		for (int32_t i = 0; i < n; i++) {
			d.car.phase += p.carInc;
			d.indexEnv *= p.indexCoef;
			d.pitchEnv *= p.burstCoef;
			out[i] = kToneLevel * toFloat(sine(d.car.phase));
		}
		d.mod.phase += p.modInc * static_cast<uint32_t>(n);
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

constexpr float kChordLevel = 0.045f;
constexpr float kChordMaxWave = 0.22f + 0.12f; ///< The most of odd and fb together

void startChord(Voice& v, const Ctx& c) {
	auto& d = v.s_.chord;
	d = {};
	d.sweepEnv = 1.f;
	for (size_t i = 0; i < d.osc.size(); i++) {
		d.osc[i].phase = static_cast<uint32_t>(i) * 0x2545F491u;
	}
}

void prepareChord(Voice& v, const Ctx& c) {
	auto& p = v.p_.e.chord;
	const float colorKnob = c.st.macro[0] * 127.f;
	const int32_t shape =
	    std::clamp<int32_t>(static_cast<int32_t>(c.st.macro[1] * kNumChordShapes), 0, kNumChordShapes - 1);
	const float sweep = c.st.macro[2];
	const float contour = c.st.macro[3];
	const float incPerHz = 4294967296.f / c.sr;
	const float maxHz = 0.45f * c.sr;

	const int32_t voicing = std::clamp<int32_t>(static_cast<int32_t>(colorKnob / 16.f), 0, 7);
	for (size_t i = 0; i < 4; i++) {
		const int32_t semis = (voicing == 0) ? 0 : kChords[shape][i] + 12 * kVoicings[voicing][i];
		// Unison spreads its voices by a few cents, so it's thick rather than just loud
		const float detune = (voicing == 0) ? (static_cast<float>(i) - 1.5f) * 0.004f : 0.f;
		const float hz = std::min(c.hz * exp2f(static_cast<float>(semis) / 12.f + detune), maxHz);
		p.inc[i] = static_cast<uint32_t>(hz * incPerHz);
		p.inc2[i] = static_cast<uint32_t>(std::min(2.f * hz, maxHz) * incPerHz);
	}
	// Sweep: odd harmonics from a modulator at twice the pitch, then all of them from feedback
	p.odd = 0.22f * std::min(sweep * 2.f, 1.f) * std::min(sweep * 2.f, 1.f);
	p.fb = 0.12f * std::max(sweep - 0.3f, 0.f) / 0.7f;
	p.sweepCoef = (contour < 0.2f) ? 1.f : decayFor(0.15f / ((contour - 0.2f) * (contour - 0.2f) + 0.002f), c.sr);
}

void renderChord(Voice& v, float* out, int32_t n, bool chunkStart) {
	auto& d = v.s_.chord;
	const auto& p = v.p_.e.chord;
	// The wave moves each voice's phase by up to sweepEnv * (odd + fb) cycles
	if (chunkStart && 4.f * kChordLevel * 6.2831853f * kChordMaxWave * d.sweepEnv < goneFor(Kind::CHORD)) {
		d.sweepEnv = 0.f;
	}
	const bool shaped = d.sweepEnv != 0.f && (p.odd != 0.f || p.fb != 0.f);

	for (int32_t i = 0; i < n; i++) {
		const float bright = d.sweepEnv;
		d.sweepEnv *= p.sweepCoef;
		const int32_t odd = shaped ? depthQ29(bright * p.odd) : 0;
		const int32_t fb = shaped ? depthQ29(bright * p.fb) : 0;
		int32_t sum = 0; // a quarter of each voice, so four can't overflow
		for (size_t k = 0; k < 4; k++) {
			d.osc[k].phase += p.inc[k];
			d.mod[k].phase += p.inc2[k];
			const int32_t x =
			    shaped ? sine(d.osc[k].phase + pmOffset(sine(d.mod[k].phase), odd) + pmOffset(d.osc[k].last, fb))
			           : sine(d.osc[k].phase);
			d.osc[k].last = x;
			sum += x >> 2;
		}
		out[i] = (4.f * kChordLevel) * toFloat(sum);
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
constexpr float kVsnrBody = 0.3f;
constexpr float kVsnrMaxDepth = 1.f;
constexpr float kVsnrMaxModDepth = 0.25f;
/// The most the snares come to for each unit of their envelope: the top cluster level, four modes, and the noise
constexpr float kVsnrMaxSnares = 0.15f * (0.25f * 4.f + 1.5f * 3.f);

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

void prepareVsnr(Voice& v, const Ctx& c) {
	auto& p = v.p_.e.vsnr;
	const float inharm = c.st.macro[0];
	const float complex = c.st.macro[1];
	const float sweep = c.st.macro[2];
	const float menv = c.st.macro[3];
	p.incPerHz = 4294967296.f / c.sr;
	p.maxHz = 0.45f * c.sr;

	p.depth = 0.75f * sweep * sweep;
	p.pitchCoef = decayFor(0.02f + 0.12f * sweep * sweep, c.sr);
	p.ratio = 1.f + 1.41f * inharm;
	p.modDepth = 0.25f * menv;
	p.modCoef = decayFor(0.01f + 0.15f * menv * menv, c.sr);
	p.restInc = static_cast<uint32_t>(std::min(c.hz, p.maxHz) * p.incPerHz);
	p.restModInc = static_cast<uint32_t>(std::min(c.hz * p.ratio, p.maxHz) * p.incPerHz);
	// The body follows the amp decay. The snares have their own 0.25 s decay, and only feel the amp decay half as much,
	// so at short decays they ring past the body and at long ones the body rings past them. The common amp envelope is
	// applied after, so it's divided back out here: sqrt(amp) and 1 / amp are followed as envelopes of their own.
	p.clusterCoef = decayFor(0.25f, c.sr);
	const float ampCoef = v.p_.ampCoef;
	p.sqrtAmpCoef = sqrtf(ampCoef);
	p.invAmpCoef = 1.f / ampCoef;
	float coef64 = ampCoef;
	for (int32_t i = 0; i < 6; i++) {
		coef64 *= coef64;
	}
	p.ampCoef64 = coef64;
	for (size_t i = 0; i < 4; i++) {
		p.modeInc[i] = static_cast<uint32_t>(std::min(kVsnrModes[i], p.maxHz) * p.incPerHz);
	}
	p.clusterLevel = 0.15f * (0.25f + 0.75f * complex);
	p.roughness = 0.05f + 0.25f * complex;
	p.bp = svfCoefs(3500.f, 0.9f, c.sr);
}

void renderVsnr(Voice& v, float* out, int32_t n, bool chunkStart) {
	auto& d = v.s_.vsnr;
	const auto& p = v.p_.e.vsnr;
	const float noteHz = v.p_.hz;
	if (d.pitchEnv * kVsnrMaxDepth < kSweepOver) {
		d.pitchEnv = 0.f;
	}
	if (chunkStart) {
		const float gone = goneFor(Kind::VSNR);
		// The body's modulation moves its phase by up to modDepth * modEnv cycles
		if (kVsnrBody * 6.2831853f * kVsnrMaxModDepth * d.modEnv < gone) {
			d.modEnv = 0.f;
		}
		const float amp = v.common_.amp;
		d.sqrtAmp = sqrtf(amp);
		d.invAmp = 1.f / std::max(amp, 1e-5f);
		// Once the amp envelope is on them, the snares come out at min(clusterEnv, sqrt(amp)) at most
		if (kVsnrMaxSnares * std::min(d.clusterEnv, d.sqrtAmp) < gone) {
			d.clusterEnv = 0.f;
		}
	}
	const bool sweeping = d.pitchEnv != 0.f;
	const bool modulated = d.modEnv != 0.f;
	const bool snaresOn = d.clusterEnv != 0.f;

	for (int32_t i = 0; i < n; i++) {
		if (sweeping) {
			const float hz = noteHz * (1.f + p.depth * d.pitchEnv);
			d.pitchEnv *= p.pitchCoef;
			d.body.phase += static_cast<uint32_t>(std::min(hz, p.maxHz) * p.incPerHz);
			if (modulated) {
				d.bodyMod.phase += static_cast<uint32_t>(std::min(hz * p.ratio, p.maxHz) * p.incPerHz);
			}
		}
		else {
			d.body.phase += p.restInc;
			d.bodyMod.phase += p.restModInc;
		}
		float body;
		if (modulated) {
			body = toFloat(sine(d.body.phase + pmOffset(sine(d.bodyMod.phase), depthQ29(p.modDepth * d.modEnv))));
			d.modEnv *= p.modCoef;
		}
		else {
			body = toFloat(sine(d.body.phase));
		}

		if (snaresOn) {
			// The snares: four modes, each phase-jittered by band passed noise
			const float nz = svf(d.noiseBp, p.bp, noise(v.common_.rng)).bp;
			// Mode k is jittered by k times the noise: in phase, multiplying the offset wraps it as the phase does
			const uint32_t jitter = cyclesOffset(p.roughness * nz);
			int32_t cl = 0; // a quarter of each mode
			for (size_t k = 0; k < 4; k++) {
				d.cluster[k].phase += p.modeInc[k];
				cl += sine(d.cluster[k].phase + jitter * static_cast<uint32_t>(k + 1)) >> 2;
			}
			const float snares = std::min(d.clusterEnv, d.sqrtAmp) * d.invAmp;
			out[i] = kVsnrBody * body + p.clusterLevel * snares * (toFloat(cl) + 1.5f * nz);
			d.clusterEnv *= p.clusterCoef;
			d.sqrtAmp *= p.sqrtAmpCoef;
			d.invAmp = std::min(d.invAmp * p.invAmpCoef, 1e5f);
		}
		else {
			out[i] = kVsnrBody * body;
		}
	}
	if (!snaresOn) {
		skipNoise(v.common_.rng, n);
	}
}

/// Vtg Snr's level: its snares outlast the amp envelope at short decays. It ends no later than at the amp envelope's
/// -100 dB, where every engine used to end.
float vsnrLevel(const Voice& v) {
	const float amp = v.common_.amp;
	if (amp * v.p_.e.vsnr.ampCoef64 < 1e-5f) {
		return amp;
	}
	return std::max(amp, std::min(v.s_.vsnr.clusterEnv, sqrtf(amp)));
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

void prepareClap(Voice& v, const Ctx& c) {
	auto& p = v.p_.e.clap;
	const float bodyChar = c.st.macro[0];
	const float balance = c.st.macro[1];
	const float crunch = c.st.macro[2];
	const float bodyEnvKnob = c.st.macro[3];
	const float pitch = c.hz / 261.63f; // the note moves the filters, C4 = as measured

	p.spacing = static_cast<int32_t>((0.006f + 0.018f * crunch) * c.sr);
	p.burstCoef = decayFor(0.022f, c.sr);
	p.bp = svfCoefs(1000.f * pitch, 1.1f, c.sr);
	p.bp2 = svfCoefs(2400.f * pitch, 1.5f, c.sr);
	p.body = svfCoefs(900.f * pitch, 1.f + 9.f * bodyChar, c.sr);
	p.bodyGain = 1.f / (1.f + 4.f * bodyChar); // a band pass peaks at its Q
	p.darkCoef = decayFor(0.05f + 1.5f * (1.f - bodyEnvKnob) * (1.f - bodyEnvKnob), c.sr);
	p.burstLevel = 0.4f * (0.8f + 0.4f * balance);
	p.tailLevel = 0.4f * (1.f - balance * balance);
	p.burstBright = 0.3f + 1.5f * bodyChar * bodyChar;
	p.tailFastCoef = decayFor(0.2f, c.sr);                  // the tail's first drop, before the amp decay takes over
	const float grit = std::max(crunch - 0.4f, 0.f) * 1.6f; // sample-and-hold roughness
	p.holdSamples = 1 + static_cast<int32_t>(grit * 6.f);
}

void renderClap(Voice& v, float* out, int32_t n, bool chunkStart) {
	auto& d = v.s_.clap;
	const auto& p = v.p_.e.clap;

	for (int32_t i = 0; i < n; i++) {
		if (d.burstIndex < kClapBursts && --d.samplesToBurst <= 0) {
			d.burstEnv = 1.f;
			d.burstIndex++;
			d.samplesToBurst = p.spacing;
		}
		if (d.holdLeft-- == 0) {
			d.held = noise(v.common_.rng);
			d.holdLeft = static_cast<uint32_t>(p.holdSamples - 1);
		}
		const float nz = d.held;
		const auto b1 = svf(d.bp1, p.bp, nz);
		const auto b2 = svf(d.bp2, p.bp2, nz);
		const float bright = b1.bp + 0.5f * b2.bp * d.bodyEnv;
		const float bursts = (d.burstIndex > 0) ? p.burstLevel * d.burstEnv * (b1.bp + p.burstBright * b2.bp) : 0.f;
		d.burstEnv *= p.burstCoef;
		const bool tailOn = d.burstIndex >= kClapBursts;
		const float tail =
		    tailOn ? p.tailLevel * (0.2f + 0.8f * d.tailEnv) * (bright + p.bodyGain * svf(d.hp, p.body, bright).bp)
		           : 0.f;
		if (tailOn) {
			d.bodyEnv *= p.darkCoef;
			d.tailEnv *= p.tailFastCoef;
		}
		out[i] = bursts + tail;
	}
}

/// Level trim, then drive after the amp envelope (amp, sample by sample), so the tail cleans up as it dies away
#if defined(__ARM_NEON)
/// Four samples. The drive's divide is a reciprocal estimate and two Newton steps, to a couple of bits of float
inline void output4(const float* in, const float* amp, int32_t* out, float trim, float gain, bool driven) {
	float32x4_t x = vmulq_n_f32(vmulq_f32(vld1q_f32(in), vld1q_f32(amp)), trim);
	if (driven) {
		x = vmulq_n_f32(x, gain);
		x = vminq_f32(vmaxq_f32(x, vdupq_n_f32(-3.f)), vdupq_n_f32(3.f));
		const float32x4_t x2 = vmulq_f32(x, x);
		const float32x4_t num = vmulq_f32(x, vaddq_f32(vdupq_n_f32(27.f), x2));
		const float32x4_t den = vmlaq_n_f32(vdupq_n_f32(27.f), x2, 9.f);
		float32x4_t r = vrecpeq_f32(den);
		r = vmulq_f32(r, vrecpsq_f32(den, r));
		r = vmulq_f32(r, vrecpsq_f32(den, r));
		x = vmulq_f32(num, r);
	}
	x = vminq_f32(vmaxq_f32(x, vdupq_n_f32(-1.f)), vdupq_n_f32(1.f));
	vst1q_s32(out, vcvtq_s32_f32(vmulq_n_f32(x, kFullScale)));
}

void output(const float* in, const float* amp, int32_t* out, int32_t n, float trim, float gain, bool driven) {
	const int32_t whole = n & ~3;
	for (int32_t i = 0; i < whole; i += 4) {
		output4(in + i, amp + i, out + i, trim, gain, driven);
	}
	if (whole < n) {
		// The last one to three, the same way, so they come out the same at any host block size
		std::array<float, 4> inRest{}, ampRest{};
		std::array<int32_t, 4> outRest;
		std::copy(in + whole, in + n, inRest.begin());
		std::copy(amp + whole, amp + n, ampRest.begin());
		output4(inRest.data(), ampRest.data(), outRest.data(), trim, gain, driven);
		std::copy(outRest.begin(), outRest.begin() + (n - whole), out + whole);
	}
}
#else
void output(const float* in, const float* amp, int32_t* out, int32_t n, float trim, float gain, bool driven) {
	for (int32_t i = 0; i < n; i++) {
		float x = in[i] * amp[i] * trim;
		if (driven) {
			x = driveCurve(x * gain);
		}
		out[i] = static_cast<int32_t>(std::clamp(x, -1.f, 1.f) * kFullScale);
	}
}
#endif

} // namespace

// Voice ---------------------------------------------------------------------------------------------------------------

void Voice::reset() {
	*this = Voice{};
	kind_ = Kind::NUM_KINDS;
	pendingTrigger_ = false;
	finished_ = true;
	prepared_ = false;
}

void Voice::start(Kind kind, const Settings& settings, uint32_t phaseIncrement, float sampleRate) {
	kind_ = kind;
	finished_ = false;
	const uint32_t seed = common_.rng;
	common_ = Common{};
	common_.rng = (seed != 0) ? seed : 0x9E3779B9u;
	common_.amp = 1.f;
	common_.punchEnv = 1.f;
	common_.level = 1.f;
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

bool Voice::preparedFor(Kind kind, const Settings& settings, uint32_t phaseIncrement, float sampleRate) const {
	return prepared_ && p_.kind == kind && p_.phaseIncrement == phaseIncrement && p_.sampleRate == sampleRate
	       && p_.macro == settings.macro && p_.decay == settings.decay && p_.drive == settings.drive
	       && p_.punch == settings.punch;
}

void Voice::prepare(Kind kind, const Settings& settings, uint32_t phaseIncrement, float sampleRate) {
	p_.macro = settings.macro;
	p_.decay = settings.decay;
	p_.drive = settings.drive;
	p_.sampleRate = sampleRate;
	p_.phaseIncrement = phaseIncrement;
	p_.kind = kind;
	p_.punch = settings.punch;
	prepared_ = true;

	p_.hz = static_cast<float>(phaseIncrement) * kPhaseToCycles * sampleRate;
	const DecayCurve& curve = decayCurveFor(kind);
	const bool hold = curve.holdsAtTop && settings.decay >= kHoldKnob;
	const float punchStretch = settings.punch ? 1.25f : 1.f;
	p_.ampCoef = hold ? 1.f : decayFor(ampDecayTime(curve, settings.decay * 127.f) * punchStretch, sampleRate);
	p_.driveGain = driveGainFor(settings.drive);
	p_.punchCoef = decayFor(0.18f, sampleRate);

	const Ctx c{settings, sampleRate, p_.hz};
	switch (kind) {
	case Kind::KIK:
		prepareKik(*this, c);
		break;
	case Kind::SNR:
		prepareSnr(*this, c);
		break;
	case Kind::METAL:
		prepareMetal(*this, c);
		break;
	case Kind::PERC:
		preparePerc(*this, c);
		break;
	case Kind::TONE:
		prepareTone(*this, c);
		break;
	case Kind::CHORD:
		prepareChord(*this, c);
		break;
	case Kind::VSNR:
		prepareVsnr(*this, c);
		break;
	case Kind::CLAP:
		prepareClap(*this, c);
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
	if (!preparedFor(kind, settings, phaseIncrement, sampleRate)) {
		prepare(kind, settings, phaseIncrement, sampleRate);
	}
	common_.ampCoef = p_.ampCoef;

	const float trim = kLevelTrim[static_cast<size_t>(kind)];
	const bool driven = settings.drive > 0.f;
	float buf[kChunk];
	while (numSamples > 0) {
		// Chunks count from the start of the note, so a host block can start or end part way through one
		const int32_t offset = static_cast<int32_t>(common_.pos % kChunk);
		const int32_t n = std::min(numSamples, kChunk - offset);
		const bool chunkStart = offset == 0;
		// Below -72 dB the voice fades out over the next chunk, then it's done
		if (chunkStart && common_.level < kEndLevel) {
			common_.ending = true;
		}

		switch (kind) {
		case Kind::KIK:
			renderKik(*this, buf, n, chunkStart);
			break;
		case Kind::SNR:
			renderSnr(*this, buf, n, chunkStart);
			break;
		case Kind::METAL:
			renderMetal(*this, buf, n, chunkStart);
			break;
		case Kind::PERC:
			renderPerc(*this, buf, n, chunkStart);
			break;
		case Kind::TONE:
			renderTone(*this, buf, n, chunkStart);
			break;
		case Kind::CHORD:
			renderChord(*this, buf, n, chunkStart);
			break;
		case Kind::VSNR:
			renderVsnr(*this, buf, n, chunkStart);
			break;
		case Kind::CLAP:
			renderClap(*this, buf, n, chunkStart);
			break;
		case Kind::NUM_KINDS:
			std::fill(buf, buf + n, 0.f);
			break;
		}

		// Punch: a soft clipper driven hard at the start and easing off over
		// about 60 ms, which squares up and lifts the attack like a compressor slamming. The amp decay also runs a
		// quarter longer.
		if (settings.punch) {
			float p = common_.punchEnv;
			for (int32_t i = 0; i < n; i++) {
				const float drive = 2.f + 2.f * p;
				buf[i] = softClip(buf[i] * drive) * (1.f + 0.5f * p) / drive;
				p *= p_.punchCoef;
			}
			common_.punchEnv = p;
		}

		// The amp envelope, which waits out any hold
		float amps[kChunk];
		const int32_t held = static_cast<int32_t>(std::min(common_.ampHold, static_cast<uint32_t>(n)));
		common_.ampHold -= held;
		float amp = common_.amp;
		std::fill(amps, amps + held, amp);
		for (int32_t i = held; i < n; i++) {
			amps[i] = amp;
			amp *= p_.ampCoef;
		}
		common_.amp = amp;
		if (common_.ending) {
			for (int32_t i = 0; i < n; i++) {
				amps[i] *= static_cast<float>(kChunk - offset - i) * (1.f / kChunk);
			}
		}
		output(buf, amps, out, n, trim, p_.driveGain, driven);
		common_.pos += n;
		out += n;
		numSamples -= n;

		const float level = (kind == Kind::VSNR) ? vsnrLevel(*this) : amp;
		if (!common_.ending) {
			common_.level = level;
		}
		else if (common_.pos % kChunk != 0) {
			common_.level = level * static_cast<float>(kChunk - offset - n) * (1.f / kChunk);
		}
		else {
			std::fill(out, out + numSamples, 0);
			finished_ = true;
			common_.amp = 0.f;
			common_.level = 0.f;
			return;
		}
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
