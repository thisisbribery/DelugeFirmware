/*
 * GIN drum voice. See gin.h for what this is, where it comes from, and the MIT notices of the code it is derived from.
 *
 * Deluge port Copyright © 2026 thisisbribery and the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Portions derived from libpo32 (Copyright (c) 2026 Eric Lewis) and the GIN synthesis core (Copyright (c) 2026
 * SquareWave Studio), both MIT licensed: see gin.h.
 */

#include "dsp/drum_synth/gin.h"
#include <algorithm>
#include <cmath>

namespace deluge::dsp::drum_synth::gin {

namespace {

constexpr float kSampleRate = 44100.f;
constexpr float kInvSampleRate = 1.f / kSampleRate;
constexpr float kTwoPi = 6.283185307f;
constexpr float kPhaseToCycles = 1.f / 4294967296.f;

constexpr float kEnvFloor = 0.001f; ///< Envelopes run from here to 1 and back: 60 dB
constexpr float kEnvSpan = 1.f / kEnvFloor;
/// Envelope time for 72 dB rather than 60: where the voice is called finished
constexpr float kEndScale = 72.f / 60.f;
/// And for 40 dB, where culling treats it like a releasing voice
constexpr float kQuietScale = 40.f / 60.f;
constexpr float kOneThird = 1.f / 3.f;
constexpr float kTwoThirds = 2.f / 3.f;

// libpo32's control maps
// -----------------------------------------------------------------------------------------------

float attackSeconds(float x) {
	return (x <= 0.f) ? 0.f : x * 0.03981071706f * std::pow(10.f, 2.4f * x);
}

float decaySeconds(float x) {
	return 0.01f * std::pow(1000.f, x);
}

/// 20 Hz to 20 kHz
float paramToHz(float x) {
	return 20.f * std::pow(1000.f, x);
}

float modHz(float rate, bool sine) {
	return (sine ? 2000.f : 20000.f) * rate * std::pow(10.f, (sine ? 3.5f : 4.f) * (rate - 1.f));
}

float dbToGain(float db) {
	return std::pow(10.f, db * 0.05f);
}

/// The mix control as a pan between the two layers, each dipping 25 dB as it goes
float mixOscGain(float mix) {
	const float pan = mix * 2.f - 1.f;
	return (pan < 0.f) ? 1.f : (1.f - pan) * dbToGain(-25.f * pan);
}
float mixNoiseGain(float mix) {
	const float pan = mix * 2.f - 1.f;
	return (pan >= 0.f) ? 1.f : (1.f + pan) * dbToGain(25.f * pan);
}

// Filters: the RBJ cookbook biquads, as libpo32 designs them
// -----------------------------------------------------------

using Biquad = Voice::Biquad;

Biquad biquad(float b0, float b1, float b2, float a0, float a1, float a2) {
	return {b0 / a0, b1 / a0, b2 / a0, a1 / a0, a2 / a0};
}

Biquad noiseFilterFor(NoiseFilter type, float hz, float q) {
	const float w0 = kTwoPi * hz * kInvSampleRate;
	const float alpha = std::sin(w0) / (2.f * q);
	const float c = std::cos(w0);
	switch (type) {
	case NoiseFilter::LOW_PASS:
		return biquad((1.f - c) * 0.5f, 1.f - c, (1.f - c) * 0.5f, 1.f + alpha, -2.f * c, 1.f - alpha);
	case NoiseFilter::BAND_PASS:
		return biquad(alpha, 0.f, -alpha, 1.f + alpha, -2.f * c, 1.f - alpha);
	case NoiseFilter::HIGH_PASS:
		return biquad((1.f + c) * 0.5f, -(1.f + c), (1.f + c) * 0.5f, 1.f + alpha, -2.f * c, 1.f - alpha);
	default:
		return {1.f, 0.f, 0.f, 0.f, 0.f}; // Off: the noise as it is
	}
}

Biquad peakFor(float hz, float db) {
	const float a = std::pow(10.f, db / 40.f);
	const float w0 = kTwoPi * hz * kInvSampleRate;
	const float alpha = std::sin(w0) * 0.5f;
	const float c = std::cos(w0);
	return biquad(1.f + alpha * a, -2.f * c, 1.f - alpha * a, 1.f + alpha / a, -2.f * c, 1.f - alpha / a);
}

/// Transposed direct form II
inline float process(const Biquad& f, float& z1, float& z2, float x) {
	const float y = f.b0 * x + z1;
	z1 = f.b1 * x - f.a1 * y + z2;
	z2 = f.b2 * x - f.a2 * y;
	return y;
}

// Per sample helpers
// ---------------------------------------------------------------------------------------------------

/// sin(2 pi phase / 2^32), to about -100 dB, without a table
inline float sine(uint32_t phase) {
	float x = static_cast<float>(static_cast<int32_t>(phase)) * kPhaseToCycles;
	if (x > 0.25f) {
		x = 0.5f - x;
	}
	else if (x < -0.25f) {
		x = -0.5f - x;
	}
	const float x2 = x * x;
	return x * (6.283185307f + x2 * (-41.341702f + x2 * (81.605249f + x2 * (-76.705859f + x2 * 42.058693f))));
}

/// 2^x, to about 1e-7, for the pitch modulation
inline float exp2Fast(float x) {
	x = std::clamp(x, -120.f, 120.f);
	int32_t i = static_cast<int32_t>(x);
	if (x < static_cast<float>(i)) {
		i--;
	}
	const float f = x - static_cast<float>(i);
	const float p =
	    1.f + f * (0.69314718f + f * (0.24022650f + f * (0.05550411f + f * (0.00961813f + f * 0.00133336f))));
	union {
		uint32_t u;
		float f;
	} scale{static_cast<uint32_t>(i + 127) << 23};
	return p * scale.f;
}

/// libpo32's noise: a linear congruential generator, 0 to 1
inline float randomUnit(uint32_t& state) {
	state = state * 1103515245u + 12345u;
	return static_cast<float>(state & 0x7fffffffu) * (1.f / 2147483647.f);
}

inline float symLimit(float x) {
	return 0.5f * (std::abs(x + 1.f) - std::abs(x - 1.f));
}

} // namespace

float levelGain(float level) {
	if (level < 1.0e-4f) {
		return 0.f;
	}
	const float db = 60.f * level - 49.f - 1.f / level;
	return (db < -500.f) ? 0.f : dbToGain(db);
}

float levelForDb(float db) {
	// 60 L - 49 - 1 / L = db, solved for L
	const float b = 49.f + db;
	return std::clamp((b + std::sqrt(b * b + 240.f)) / 120.f, 0.f, 1.f);
}

void Voice::reset() {
	prepared_ = false;
	pendingTrigger_ = false;
	finished_ = true;
	start();
	finished_ = true;
}

void Voice::start() {
	n_ = 0;
	phase_ = 0;
	rand_ = 123456789u;
	oscEnv_ = kEnvFloor;
	oscSynced_ = false;
	nEnv_ = kEnvFloor;
	nSynced_ = false;
	modEnv_ = 1.f;
	modS_ = 1.f;
	modC_ = 0.f;
	modState_ = 0.f;
	if (prepared_ && preparedFor_.modMode == ModMode::NOISE) {
		modState_ = randomUnit(rand_) * 2.f - 1.f;
	}
	nz1_ = nz2_ = ez1_ = ez2_ = 0.f;
	finished_ = false;
}

void Voice::prepare(const Settings& s) {
	preparedFor_ = s;
	prepared_ = true;

	oscAtk_ = attackSeconds(s.toneAttack);
	oscDcy_ = decaySeconds(s.toneDecay);
	nAtk_ = attackSeconds(s.noiseAttack);
	nDcy_ = decaySeconds(s.noiseDecay);

	oscAtkCoef_ = (oscAtk_ > 0.f) ? std::pow(kEnvSpan, kInvSampleRate / oscAtk_) : 1.f;
	oscDcyCoef_ = std::pow(kEnvFloor, kInvSampleRate / oscDcy_);
	nAtkCoef_ = (nAtk_ > 0.f) ? std::pow(kEnvSpan, kInvSampleRate / nAtk_) : 1.f;
	nDcyCoef_ = std::pow(kEnvFloor, kInvSampleRate / nDcy_);

	linAtk_ = nAtk_ * kTwoThirds;
	linDcy_ = nDcy_ * kTwoThirds;
	invLinAtk_ = (linAtk_ > 0.f) ? 1.f / linAtk_ : 0.f;
	invLinDcy_ = 1.f / linDcy_;

	// Pitch modulation. The sweep reaches 8 octaves, the LFO and noise 4.
	const bool decayMod = s.modMode == ModMode::DECAY;
	const float amount = std::clamp(s.modAmount, -1.f, 1.f);
	modPitchScale_ = amount * std::abs(amount) * (decayMod ? 8.f : 4.f);
	const float rate = std::clamp(s.modRate, 0.f, 1.f);
	const float hz = modHz(rate, s.modMode != ModMode::NOISE);
	modAlpha_ = std::clamp(1.f - std::exp(-kTwoPi * hz * kInvSampleRate), 0.f, 1.f);
	// The sweep's rate zero holds its starting pitch
	modDcyCoef_ = (decayMod && rate > 0.f) ? std::pow(kEnvFloor, kInvSampleRate * 3.f * rate / (1.03f - rate)) : 1.f;
	rotS_ = std::sin(kTwoPi * hz * kInvSampleRate);
	rotC_ = std::cos(kTwoPi * hz * kInvSampleRate);

	// The modulated noise envelope's bursts
	ringPeriod_ = 0.f;
	ringGain_ = 1.f;
	if (s.noiseEnv == NoiseEnv::MOD && s.burstSpacing > 1.0e-4f) {
		ringPeriod_ = (s.burstSpacing * kInvSampleRate) * std::pow(10000.f, s.burstSpacing);
		if (ringPeriod_ < 1.5e-3f) {
			ringPeriod_ *= 8.f;
			ringGain_ = std::sqrt(0.5f / ringPeriod_);
		}
		else {
			ringPeriod_ *= 2.f;
		}
	}
	invRingPeriod_ = (ringPeriod_ > 0.f) ? 1.f / ringPeriod_ : 0.f;

	oscGain_ = mixOscGain(s.mix);
	noiseGain_ = mixNoiseGain(s.mix);

	const float maxHz = 0.45f * kSampleRate;
	noiseFilter_ =
	    noiseFilterFor(s.noiseFilter, std::min(paramToHz(s.noiseFreq), maxHz), 0.1f * std::pow(100000.f, s.noiseQ));
	filterNoise_ = s.noiseFilter != NoiseFilter::OFF;

	// A boost takes half its dB off the whole channel
	const float eqDb = (s.eqGain - 0.5f) * 80.f;
	level_ = s.level;
	if (eqDb > 0.f) {
		level_ *= dbToGain(-0.5f * eqDb);
	}
	useEq_ = std::abs(eqDb) > 0.5f;
	if (useEq_) {
		eq_ = peakFor(std::min(paramToHz(s.eqFreq), maxHz), eqDb);
	}

	// Distortion: gentle below a drive of 1, a folding clipper above
	distDrive_ = 200.f * s.drive * s.drive * s.drive;
	if (s.drive <= 0.01f || distDrive_ <= 0.f) {
		distDrive_ = 0.f;
	}
	else if (distDrive_ < 1.f) {
		distGain_ = 1.f - distDrive_ * (1.f / 30.f);
		distClip_ = (1.f + std::sqrt((distDrive_ + 3.f) / distDrive_)) * kOneThird;
		distBias_ = (-2.f / 27.f) * distDrive_ - 9.f / 27.f;
	}
	else {
		distGain_ = 0.2f / (distDrive_ - 0.4827586f) + 0.58f;
		distBias_ = -11.f / 27.f;
	}

	// Done once each layer that can be heard is 72 dB down, and quiet at 40 dB
	float end = 0.f;
	float quiet = 0.f;
	if (oscGain_ > 0.f) {
		end = oscAtk_ + oscDcy_ * kEndScale;
		quiet = oscAtk_ + oscDcy_ * kQuietScale;
	}
	if (noiseGain_ > 0.f) {
		const bool linear = s.noiseEnv == NoiseEnv::LINEAR;
		end = std::max(end, linear ? linAtk_ + linDcy_ : nAtk_ + nDcy_ * kEndScale);
		quiet = std::max(quiet, linear ? linAtk_ + linDcy_ * 0.99f : nAtk_ + nDcy_ * kQuietScale);
	}
	endTime_ = end;
	quietTime_ = quiet;
}

void Voice::render(const Settings& s, uint32_t phaseIncrement, int32_t* out, int32_t numSamples) {
	if (!prepared_ || !(s == preparedFor_)) {
		prepare(s);
	}
	if (pendingTrigger_) {
		pendingTrigger_ = false;
		start();
	}
	if (finished_) {
		std::fill(out, out + numSamples, 0);
		return;
	}

	// Keep the sine LFO's rotation on the unit circle: it drifts over a long note otherwise
	const float r = modS_ * modS_ + modC_ * modC_;
	if (r > 0.f) {
		const float norm = 1.f / std::sqrt(r);
		modS_ *= norm;
		modC_ *= norm;
	}

	const float baseStep = static_cast<float>(phaseIncrement);
	const bool modActive = modPitchScale_ != 0.f;
	const bool distort = distDrive_ > 0.f;
	// A layer the mix has turned all the way off isn't rendered: hats and claps are all noise, many kicks all tone.
	// Its envelope still runs, so it comes in right if the mix moves.
	const bool toneOn = oscGain_ > 0.f;
	const bool noiseOn = noiseGain_ > 0.f;

	for (int32_t i = 0; i < numSamples; i++) {
		const float t = static_cast<float>(n_ + i) * kInvSampleRate;

		// Tone envelope: exponential attack from -60 dB, then exponential decay
		float oscEnv;
		if (oscAtk_ > 0.f && t < oscAtk_) {
			oscEnv = oscEnv_;
			oscEnv_ *= oscAtkCoef_;
		}
		else {
			if (!oscSynced_) {
				oscEnv_ = std::pow(kEnvFloor, (t - oscAtk_) / oscDcy_);
				oscSynced_ = true;
			}
			oscEnv = oscEnv_;
			oscEnv_ *= oscDcyCoef_;
		}

		float osc = 0.f;
		if (toneOn) {
			float modSig;
			switch (preparedFor_.modMode) {
			case ModMode::DECAY:
				modSig = modEnv_;
				modEnv_ *= modDcyCoef_;
				break;
			case ModMode::SINE: {
				const float ms = modS_ * rotC_ + modC_ * rotS_;
				const float mc = modC_ * rotC_ - modS_ * rotS_;
				modSig = modS_;
				modS_ = ms;
				modC_ = mc;
				break;
			}
			default: {
				const float noiseIn = randomUnit(rand_) * 2.f - 1.f;
				modState_ = modState_ * (1.f - modAlpha_) + noiseIn * modAlpha_;
				modSig = modState_;
				break;
			}
			}

			const float mult = modActive ? exp2Fast(modSig * modPitchScale_) : 1.f;
			phase_ += static_cast<uint32_t>(std::min(baseStep * mult, 2147483520.f));

			switch (preparedFor_.wave) {
			case Wave::SINE:
				osc = sine(phase_);
				break;
			case Wave::TRIANGLE:
				osc = 2.f * std::abs(2.f * (static_cast<float>(phase_) * kPhaseToCycles) - 1.f) - 1.f;
				break;
			default:
				osc = 2.f * (static_cast<float>(phase_) * kPhaseToCycles) - 1.f;
				break;
			}
			osc *= oscEnv;
		}

		// Noise envelope
		float nEnv;
		switch (preparedFor_.noiseEnv) {
		case NoiseEnv::MOD:
			if (t < nAtk_) {
				nEnv = 1.f;
			}
			else {
				const float dt = t - nAtk_;
				if (!nSynced_) {
					nEnv_ = std::pow(kEnvFloor, dt / nDcy_);
					nSynced_ = true;
				}
				const float decay = nEnv_;
				nEnv_ *= nDcyCoef_;
				if (ringPeriod_ > 0.f) {
					// A triangle through each burst period, through cos(pi x): a burst at each end
					float saw = dt * invRingPeriod_;
					saw -= static_cast<float>(static_cast<int32_t>(saw));
					const float tri = 1.f - 2.f * std::abs(saw - 0.5f);
					const float cosine = sine(static_cast<uint32_t>((0.25f + 0.5f * tri) * 4294967040.f));
					nEnv = cosine * decay * ringGain_;
				}
				else {
					nEnv = decay;
				}
			}
			break;
		case NoiseEnv::LINEAR:
			if (t < linAtk_) {
				nEnv = t * invLinAtk_;
			}
			else if (t < linAtk_ + linDcy_) {
				nEnv = 1.f - (t - linAtk_) * invLinDcy_;
			}
			else {
				nEnv = 0.f;
			}
			break;
		default:
			if (nAtk_ > 0.f && t < nAtk_) {
				nEnv = nEnv_;
				nEnv_ *= nAtkCoef_;
			}
			else {
				if (!nSynced_) {
					nEnv_ = std::pow(kEnvFloor, (t - nAtk_) / nDcy_);
					nSynced_ = true;
				}
				nEnv = nEnv_;
				nEnv_ *= nDcyCoef_;
			}
			break;
		}

		float noise = 0.f;
		if (noiseOn) {
			noise = randomUnit(rand_) * 2.f - 1.f;
			// With the filter off its biquad passes the noise straight through, so skip it
			if (filterNoise_) {
				noise = process(noiseFilter_, nz1_, nz2_, noise);
			}
			noise *= nEnv;
		}
		float x = osc * oscGain_ + noise * noiseGain_;

		if (distort) {
			if (distDrive_ < 1.f) {
				float xc = std::clamp(x - kOneThird, -distClip_, distClip_);
				x = (xc * (distDrive_ * (std::abs(xc) - xc * xc) + 1.f) - distBias_) * distGain_;
			}
			else {
				float xc = symLimit(x * distDrive_ - kOneThird);
				const float pos = xc * (std::abs(xc) - xc * xc + 1.f) - distBias_;
				xc = symLimit(-x * distDrive_ - kOneThird);
				const float neg = xc * (std::abs(xc) - xc * xc + 1.f) - distBias_;
				x = 0.5f * (pos - neg) * distGain_;
			}
		}
		if (useEq_) {
			x = process(eq_, ez1_, ez2_, x);
		}
		x = std::clamp(x * level_, -1.f, 1.f);
		out[i] = static_cast<int32_t>(x * 2147483520.f);
	}
	n_ += static_cast<uint32_t>(numSamples);
	finished_ = static_cast<float>(n_) * kInvSampleRate > endTime_;
}

bool Voice::quiet() const {
	return finished_ || static_cast<float>(n_) * kInvSampleRate > quietTime_;
}

} // namespace deluge::dsp::drum_synth::gin
