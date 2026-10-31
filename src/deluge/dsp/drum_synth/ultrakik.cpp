/*
 * Copyright © 2026 thisisbribery.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dsp/drum_synth/ultrakik.h"
#include <algorithm>
#include <cmath>

namespace deluge::dsp::drum_synth::ultrakik {

namespace {

// The body is an integer phase, 2^32 to the cycle, read from a q31 sine table like the Cyc engines'. Everything after
// it is float, a sample at a time, with the work that doesn't change worked out once per chunk or per settings change.

constexpr int32_t kSineBits = 10; ///< 1024 steps to the cycle, linear interpolation: good to about -106 dB
constexpr int32_t kSineSize = 1 << kSineBits;

constexpr double taylorSine(double x) {
	double term = x;
	double sum = x;
	for (int32_t n = 1; n < 30; n++) {
		term *= -x * x / static_cast<double>((2 * n) * (2 * n + 1));
		sum += term;
	}
	return sum;
}

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

/// sin(2 pi phase / 2^32), -1 to 1
[[gnu::always_inline]] inline float sine(uint32_t phase) {
	const uint32_t i = phase >> (32 - kSineBits);
	const int32_t a = kSineTable[i];
	const int32_t b = kSineTable[i + 1];
	const int32_t f = static_cast<int32_t>((phase << kSineBits) >> 1);
	const int32_t s = a + 2 * static_cast<int32_t>((static_cast<int64_t>(b - a) * f) >> 32);
	return static_cast<float>(s) * (1.f / 2147483648.f);
}

/// A phase offset in cycles, under 8 either way, wrapped into the cycle
[[gnu::always_inline]] inline uint32_t cyclesOffset(float c) {
	return static_cast<uint32_t>(static_cast<int32_t>(c * 268435456.f)) << 4;
}

[[gnu::always_inline]] inline float noise(uint32_t& state) {
	state ^= state << 13;
	state ^= state >> 17;
	state ^= state << 5;
	return static_cast<float>(static_cast<int32_t>(state)) * (1.f / 2147483648.f);
}

/// Per sample multiplier that falls by 60 dB in t60 seconds
inline float decayFor(float t60, float sampleRate) {
	return (t60 <= 0.f) ? 0.f : expf(-6.907755f / (t60 * sampleRate));
}

/// Per sample multiplier for a time constant (falls to 1/e) in seconds
inline float timeConstant(float tau, float sampleRate) {
	return expf(-1.f / (tau * sampleRate));
}

/// One pole low pass coefficient for a cutoff
inline float onePole(float hz, float sampleRate) {
	return 1.f - expf(-6.2831853f * hz / sampleRate);
}

/// Cubic soft clip: unity slope at zero, flat at +-1 from +-1 on
[[gnu::always_inline]] inline float cubicClip(float x) {
	x = std::clamp(x, -1.f, 1.f);
	return x * (1.5f - 0.5f * x * x);
}

constexpr int32_t kChunk = 64;
constexpr float kEndLevel = 2.5118864e-4f; ///< -72 dB
constexpr float kHoldDecay = 0.995f;       ///< Decay at or above this holds
constexpr float kGone = 1e-5f;             ///< An envelope below this is off: -100 dB
constexpr float kLevel = 0.891f;           ///< -1 dB: the loudest any setting peaks

} // namespace

float ampDecaySeconds(float decay) {
	return (decay >= kHoldDecay) ? 1000.f : 0.04f * powf(200.f, decay);
}

float holdSeconds(float hold) {
	return 0.2f * hold * hold;
}

void Voice::reset() {
	*this = Voice{};
	finished_ = true;
	level_ = 0.f;
}

void Voice::start(const Settings& settings, uint32_t phaseIncrement, float sampleRate) {
	if (!prepared_ || !(p_.settings == settings) || p_.sampleRate != sampleRate) {
		prepare(settings, phaseIncrement, sampleRate);
	}
	phase_ = p_.startPhase;
	crushPhase_ = 0;
	if (rng_ == 0) {
		rng_ = 0x2545F491u;
		clickRng_ = 0x9E3779B9u;
	}
	pos_ = 0;
	pitchEnv_ = 1.f;
	punchEnv_ = 1.f;
	bendPos_ = 0.f;
	bendMul_ = 1.f;
	bendStep_ = 0.f;
	amp_ = 1.f;
	holdLeft_ = p_.holdSamples;
	clickEnv_ = 1.f;
	zapEnv_ = 1.f;
	zapPhase_ = 0;
	clickLpState_ = clickHpState_ = gritLp_ = 0.f;
	dcIn_ = dcOut_ = 0.f;
	crushHeld_ = 0.f;
	level_ = 1.f;
	ending_ = false;
	finished_ = false;
	started_ = true;
}

void Voice::prepare(const Settings& s, uint32_t phaseIncrement, float sr) {
	Prepared& p = p_;
	p.settings = s;
	p.phaseIncrement = phaseIncrement;
	p.sampleRate = sr;
	prepared_ = true;

	p.incPerHz = 4294967296.f / sr;
	p.maxInc = 0.45f * 4294967296.f;
	p.baseInc = static_cast<float>(phaseIncrement);

	// Sweep: up to 5 octaves above the note, falling with a time constant of 2 to 250 ms
	p.depth = exp2f(5.f * s.sweep) - 1.f;
	p.pitchCoef = timeConstant(0.002f * powf(125.f, s.sweepTime), sr);
	// Punch: the first few ms jump far above the sweep, the hit of the beater rather than a sweep you hear
	p.punchDepth = exp2f(4.f * s.punch) - 1.f;
	p.punchCoef = timeConstant(0.0012f + 0.0025f * s.punch, sr);
	p.startPhase = static_cast<uint32_t>(s.snap * 1073741824.f);

	// Bend: up to 24 semitones either way, over 40 ms to 4 s
	p.bendSemis = 24.f * s.bend;
	p.bendPerChunk = static_cast<float>(kChunk) / (0.04f * powf(100.f, s.bendTime) * sr);

	p.ampCoef = (s.decay >= kHoldDecay) ? 1.f : decayFor(ampDecaySeconds(s.decay), sr);
	p.holdSamples = static_cast<uint32_t>(holdSeconds(s.hold) * sr);

	// Fold: in from nothing, a quarter cycle of sine (rounding the peaks) up to two cycles (folding over many times)
	p.foldGain = 0.25f + 1.75f * s.fold * std::sqrt(s.fold);
	p.foldMix = std::min(1.f, s.fold * 6.f);

	p.grit = 0.7f * s.grit;

	// Drive: 0 to 36 dB, levelled so a full scale input peaks at full scale. Crossfaded in from clean at the bottom.
	p.driveGain = exp2f(6.f * s.drive);
	p.tubeBias = 0.2f;
	p.tubeOffset = cubicClip(p.tubeBias);
	{
		const float g = p.driveGain;
		float peak;
		switch (s.driveType) {
		case DriveType::TUBE:
			peak = std::max(std::abs(cubicClip(g + p.tubeBias) - p.tubeOffset),
			                std::abs(cubicClip(-g + p.tubeBias) - p.tubeOffset));
			break;
		case DriveType::WARM:
			peak = cubicClip(g);
			break;
		default:
			peak = 1.f;
			break;
		}
		p.driveNorm = 1.f / std::max(peak, 0.05f);
	}

	// Sub: a clean sine of the body under the driven body
	p.sub = s.sub;
	p.bodyMix = 1.f - 0.6f * s.sub;

	// Click
	const float tone = s.clickTone;
	switch (s.clickType) {
	case Click::THUD:
		p.clickLevel = 5.f * s.click;
		p.clickCoef = timeConstant(0.012f - 0.008f * tone, sr);
		p.clickLp = onePole(700.f * powf(8.f, tone), sr);
		break;
	case Click::TICK:
		p.clickLevel = 2.4f * s.click;
		p.clickCoef = timeConstant(0.0015f - 0.00125f * tone, sr);
		break;
	case Click::NOISE:
		p.clickLevel = 1.8f * s.click;
		p.clickCoef = timeConstant(0.02f - 0.015f * tone, sr);
		p.clickHp = onePole(800.f * powf(8.f, tone), sr);
		break;
	default: // ZAP
		p.clickLevel = 1.2f * s.click;
		p.clickCoef = timeConstant(0.004f, sr);
		p.zapDepth = (1500.f * powf(6.f, tone)) * p.incPerHz;
		p.zapCoef = timeConstant(0.0012f, sr);
		break;
	}

	// Crush: sample and hold from 44.1 kHz down to about 700 Hz, and 16 bits down to 3
	p.crushing = s.crush > 0.f;
	p.crushInc = exp2f(-6.f * s.crush) * 4294967295.f;
	p.crushStep = exp2f(-(15.f - 12.f * s.crush));
	p.crushInvStep = 1.f / p.crushStep;

	p.outGain = kLevel / (p.bodyMix + p.sub);
	p.dcCoef = 1.f - 6.2831853f * 6.f / sr;
}

void Voice::render(const Settings& s, uint32_t phaseIncrement, float sr, int32_t* out, int32_t numSamples) {
	if (pendingTrigger_) {
		pendingTrigger_ = false;
		start(s, phaseIncrement, sr);
	}
	if (finished_ || !started_) {
		std::fill(out, out + numSamples, 0);
		return;
	}
	if (!(p_.settings == s) || p_.sampleRate != sr) {
		prepare(s, phaseIncrement, sr);
	}
	else if (p_.phaseIncrement != phaseIncrement) {
		// Only the note moved (a glide, a bend, envelope 2 on pitch): nothing else to work out
		p_.phaseIncrement = phaseIncrement;
		p_.baseInc = static_cast<float>(phaseIncrement);
	}
	const Prepared& p = p_;

	while (numSamples > 0) {
		const int32_t offset = static_cast<int32_t>(pos_ % kChunk);
		const int32_t n = std::min(numSamples, kChunk - offset);
		const bool driven = p.settings.drive > 0.f;
		// What changes as the hit goes on is looked at once a chunk, the chunks counted from the start of the note, so
		// a hit sounds the same whatever size the host's buffers are
		if (offset == 0) {
			if (level_ < kEndLevel) {
				ending_ = true;
			}
			sweeping_ = pitchEnv_ * p.depth > 1e-5f || punchEnv_ * p.punchDepth > 1e-5f;
			if (!sweeping_) {
				pitchEnv_ = punchEnv_ = 0.f;
			}
			clicking_ = clickEnv_ > kGone && p.clickLevel > 0.f;
			if (!clicking_) {
				clickEnv_ = 0.f;
			}
			// Grit rides on the body squared: once the driven body is under -26 dB it's under -55 dB, so it stops
			gritty_ = p.grit > 0.f && amp_ * (driven ? p.driveGain * p.driveNorm : 1.f) > 0.05f;
			// The bend, followed in straight lines from one chunk boundary to the next
			if (p.bendSemis == 0.f) {
				bendMul_ = 1.f;
				bendStep_ = 0.f;
			}
			else if (bendPos_ < 1.f) {
				bendPos_ = std::min(1.f, bendPos_ + p.bendPerChunk);
				const float nextMul = exp2f(p.bendSemis * bendPos_ * bendPos_ * (1.f / 12.f));
				bendStep_ = (nextMul - bendMul_) * (1.f / kChunk);
			}
			else {
				bendStep_ = 0.f;
			}
		}
		const bool sweeping = sweeping_;
		const bool clicking = clicking_;
		const bool gritty = gritty_;
		const float bendStep = bendStep_;
		float bendMul = bendMul_;
		const bool folding = p.foldMix > 0.f;
		const float driveMix = std::min(1.f, p.settings.drive * 10.f);
		const Wave wave = p.settings.wave;
		const DriveType driveType = p.settings.driveType;
		const Click clickType = p.settings.clickType;

		// Each stage runs over the chunk in its own pass: the samples of a pass don't depend on each other, so an
		// in-order FPU can overlap them (and the drive can go four at a time), where one long chain per sample would
		// stall on every step. State the passes carry lives in locals.
		float body[kChunk];
		float sub[kChunk];
		float amp = amp_;
		uint32_t rng = rng_;

		// Oscillator, pitch and amp envelope
		{
			float pitchEnv = pitchEnv_;
			float punchEnv = punchEnv_;
			uint32_t holdLeft = holdLeft_;
			uint32_t phase = phase_;
			const bool moving = sweeping || bendStep != 0.f;
			const uint32_t restInc = static_cast<uint32_t>(std::min(p.baseInc * bendMul, p.maxInc));
			for (int32_t i = 0; i < n; i++) {
				uint32_t inc = restInc;
				if (moving) {
					// The note, bent, swept down from above
					const float incF =
					    p.baseInc * bendMul * (1.f + p.depth * pitchEnv) * (1.f + p.punchDepth * punchEnv);
					inc = static_cast<uint32_t>(std::min(incF, p.maxInc));
					pitchEnv *= p.pitchCoef;
					punchEnv *= p.punchCoef;
					bendMul += bendStep;
				}
				phase += inc;
				// Hold, then the decay. It sits before the folder and the drive, as a VCA into a pedal: the more drive,
				// the longer the body stays full
				if (holdLeft != 0) {
					holdLeft--;
				}
				else {
					amp *= p.ampCoef;
				}
				const float s = sine(phase) * amp;
				sub[i] = s;
				float x;
				switch (wave) {
				case Wave::ROUND: {
					const float t = 1.f
					                - std::abs(static_cast<float>(static_cast<int32_t>(phase - 0x40000000u)))
					                      * (1.f / 1073741824.f);
					x = t * (1.35f - 0.35f * t * t) * amp;
					break;
				}
				case Wave::TRI:
					x = (1.f
					     - std::abs(static_cast<float>(static_cast<int32_t>(phase - 0x40000000u)))
					           * (1.f / 1073741824.f))
					    * amp;
					break;
				case Wave::SAW: {
					// Rising through zero at the start of the cycle, the jump half way, smoothed by a polyBLEP
					const uint32_t h = phase + 0x80000000u; // 0 at the jump
					float y = static_cast<float>(static_cast<int32_t>(phase)) * (1.f / 2147483648.f);
					const float dt = static_cast<float>(inc) * (1.f / 4294967296.f);
					const float t = static_cast<float>(h) * (1.f / 4294967296.f);
					if (t < dt) {
						const float u = t / dt;
						y -= u + u - u * u - 1.f;
					}
					else if (t > 1.f - dt) {
						const float u = (t - 1.f) / dt;
						y -= u * u + u + u + 1.f;
					}
					x = y * amp;
					break;
				}
				default:
					x = s;
					break;
				}
				body[i] = x;
			}
			pitchEnv_ = pitchEnv;
			punchEnv_ = punchEnv;
			holdLeft_ = holdLeft;
			phase_ = phase;
			bendMul_ = bendMul;
		}

		if (folding) {
			for (int32_t i = 0; i < n; i++) {
				const float x = body[i];
				body[i] = x + p.foldMix * (sine(cyclesOffset(x * p.foldGain)) - x);
			}
		}

		if (driven) {
			const float g = p.driveGain;
			const float norm = p.driveNorm;
			switch (driveType) {
			case DriveType::HARD:
				for (int32_t i = 0; i < n; i++) {
					const float x = body[i];
					body[i] = x + driveMix * (std::clamp(x * g, -1.f, 1.f) * norm - x);
				}
				break;
			case DriveType::TUBE:
				for (int32_t i = 0; i < n; i++) {
					const float x = body[i];
					body[i] = x + driveMix * ((cubicClip(x * g + p.tubeBias) - p.tubeOffset) * norm - x);
				}
				break;
			case DriveType::FUZZ:
				for (int32_t i = 0; i < n; i++) {
					const float x = body[i];
					const float u = x * g;
					body[i] = x + driveMix * (std::clamp(u + 0.8f * (std::abs(u) - 0.5f), -1.f, 1.f) * norm - x);
				}
				break;
			default:
				for (int32_t i = 0; i < n; i++) {
					const float x = body[i];
					body[i] = x + driveMix * (cubicClip(x * g) * norm - x);
				}
				break;
			}
		}

		if (gritty) {
			// Darkened noise riding on the driven body's peaks: the louder the body, the more it rattles, like a
			// speaker pushed past what it can take
			float gritLp = gritLp_;
			for (int32_t i = 0; i < n; i++) {
				gritLp += 0.6f * (noise(rng) - gritLp);
				const float x = body[i];
				body[i] = x + p.grit * gritLp * x * x;
			}
			gritLp_ = gritLp;
		}

		// The click goes in after the drive, so it stays sharp however hard the body is driven
		if (clicking) {
			float clickEnv = clickEnv_;
			uint32_t clickRng = clickRng_;
			switch (clickType) {
			case Click::THUD: {
				float lp = clickLpState_;
				for (int32_t i = 0; i < n; i++) {
					lp += p.clickLp * (noise(clickRng) - lp);
					body[i] += p.clickLevel * clickEnv * lp;
					clickEnv *= p.clickCoef;
				}
				clickLpState_ = lp;
				break;
			}
			case Click::TICK:
				for (int32_t i = 0; i < n; i++) {
					body[i] += p.clickLevel * clickEnv;
					clickEnv *= p.clickCoef;
				}
				break;
			case Click::NOISE: {
				float hp = clickHpState_;
				for (int32_t i = 0; i < n; i++) {
					const float nz = noise(clickRng);
					hp += p.clickHp * (nz - hp);
					body[i] += p.clickLevel * clickEnv * (nz - hp);
					clickEnv *= p.clickCoef;
				}
				clickHpState_ = hp;
				break;
			}
			default: { // ZAP: a sine of its own, falling from kHz to the body's pitch in a few ms
				float zapEnv = zapEnv_;
				uint32_t zapPhase = zapPhase_;
				const uint32_t restInc = static_cast<uint32_t>(std::min(p.baseInc, p.maxInc));
				for (int32_t i = 0; i < n; i++) {
					zapPhase += static_cast<uint32_t>(p.zapDepth * zapEnv) + restInc;
					zapEnv *= p.zapCoef;
					body[i] += p.clickLevel * clickEnv * sine(zapPhase);
					clickEnv *= p.clickCoef;
				}
				zapEnv_ = zapEnv;
				zapPhase_ = zapPhase;
				break;
			}
			}
			clickEnv_ = clickEnv;
			clickRng_ = clickRng;
		}
		rng_ = rng;

		if (p.crushing) {
			uint32_t crushPhase = crushPhase_;
			float crushHeld = crushHeld_;
			const uint32_t crushInc = static_cast<uint32_t>(p.crushInc);
			const bool every = p.crushInc >= 4294967040.f;
			for (int32_t i = 0; i < n; i++) {
				const uint32_t before = crushPhase;
				crushPhase += crushInc;
				if (crushPhase < before || every) {
					const float q = body[i] * p.crushInvStep;
					crushHeld = static_cast<float>(static_cast<int32_t>(q + std::copysign(0.5f, q))) * p.crushStep;
				}
				body[i] = crushHeld;
			}
			crushPhase_ = crushPhase;
			crushHeld_ = crushHeld;
		}

		// DC blocker on the body (the tube, fuzz, saw and click all leave some), the clean sine under it, then a soft
		// knee above -1 dB towards full scale to catch the click and grit on top of a full body
		{
			float dcIn = dcIn_;
			float dcOut = dcOut_;
			const float bodyGain = p.bodyMix * p.outGain;
			const float subGain = p.sub * p.outGain;
			for (int32_t i = 0; i < n; i++) {
				const float x = body[i];
				dcOut = x - dcIn + p.dcCoef * dcOut;
				dcIn = x;
				float y = bodyGain * dcOut + subGain * sub[i];
				const float a = std::abs(y);
				if (a > kLevel) {
					const float z = (a - kLevel) * (1.f / (1.f - kLevel));
					y = std::copysign(kLevel + (1.f - kLevel) * z / (1.f + z), y);
				}
				out[i] = static_cast<int32_t>(std::min(y, 0.99999994f) * 2147483648.f);
			}
			dcIn_ = dcIn;
			dcOut_ = dcOut;
		}
		amp_ = amp;

		if (ending_) {
			for (int32_t i = 0; i < n; i++) {
				const float f = static_cast<float>(kChunk - offset - i) * (1.f / kChunk);
				out[i] = static_cast<int32_t>(static_cast<float>(out[i]) * f);
			}
		}
		pos_ += n;
		out += n;
		numSamples -= n;

		// The level counts the drive's gain, so a driven tail isn't cut while it can still be heard
		const float gain = driven ? std::max(1.f, p.driveGain * p.driveNorm * driveMix) : 1.f;
		const float level = amp * gain;
		if (!ending_) {
			level_ = level;
		}
		else if (pos_ % kChunk == 0) {
			std::fill(out, out + numSamples, 0);
			finished_ = true;
			level_ = 0.f;
			return;
		}
	}
}

} // namespace deluge::dsp::drum_synth::ultrakik
