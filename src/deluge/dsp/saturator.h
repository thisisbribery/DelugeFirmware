/*
 * Copyright © 2026 Mitch Catterall
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace deluge::dsp {

/// Saturation curves. CLASSIC is the original Deluge tanh table, kept so old songs sound the same.
enum class SaturationType : uint8_t {
	CLASSIC,
	TAPE, // smooth tanh-like curve: rounded, the default
	WARM, // x / sqrt(1 + x^2): very soft knee, long tail
	CLIP, // cubic soft clip: firmer knee, flat top
	HARD, // hard clip
	FOLD, // soft wavefolder: goes bright and metallic as drive rises
	NUM_TYPES,
};

inline constexpr const char* kSaturationTypeNames[] = {"classic", "tape", "warm", "clip", "hard", "fold"};
static_assert(std::size(kSaturationTypeNames) == static_cast<size_t>(SaturationType::NUM_TYPES));

inline const char* saturationTypeToString(SaturationType type) {
	return kSaturationTypeNames[static_cast<size_t>(type)];
}
inline SaturationType stringToSaturationType(const char* string) {
	for (size_t i = 0; i < std::size(kSaturationTypeNames); i++) {
		if (!strcmp(string, kSaturationTypeNames[i])) {
			return static_cast<SaturationType>(i);
		}
	}
	return SaturationType::TAPE;
}

/// User-facing settings, all small integers as shown in the menus.
struct SaturationSettings {
	SaturationType type = SaturationType::TAPE;
	uint8_t drive = 0;  // 0 = off, 1..50
	int8_t bias = 0;    // -25..25, makes the curve asymmetric (even harmonics)
	uint8_t tone = 50;  // 0..50, post low-pass, 50 = open
	uint8_t lowCut = 0; // 0..50, pre high-pass, 0 = off
	uint8_t mix = 50;   // 0..50, dry/wet, 50 = fully wet

	static constexpr int32_t kMaxDrive = 50;
	static constexpr int32_t kMaxBias = 25;
	static constexpr int32_t kMaxTone = 50;
	static constexpr int32_t kMaxLowCut = 50;
	static constexpr int32_t kMaxMix = 50;

	bool operator==(const SaturationSettings&) const = default;

	/// The old 1..15 "clippingAmount" mapped onto the 1..50 drive scale, and back (round trip is exact).
	static constexpr uint8_t driveFromLegacy(int32_t legacy) { return (legacy * 10 + 1) / 3; }
	static constexpr uint8_t legacyFromDrive(int32_t drive) {
		return drive ? std::clamp<int32_t>((drive * 3 + 5) / 10, 1, 15) : 0;
	}
};

/// Cheap, clean-room saturator: pre high-pass, drive + bias, waveshaper with first-order antiderivative
/// antialiasing, DC block when biased, post low-pass, dry/wet mix.
/// Processes q31 audio in place, about 25 float ops per sample per channel.
class Saturator {
public:
	struct Channel {
		float low = 0.f;   // pre high-pass state
		float prev = 0.f;  // previous shaper input, for antialiasing
		float prevF = 0.f; // its antiderivative
		SaturationType prevType = SaturationType::CLASSIC;
		float dcIn = 0.f;  // DC blocker
		float dcOut = 0.f; // DC blocker
		float tone = 0.f;  // post low-pass state
	};

	void configure(const SaturationSettings& s) {
		type_ = s.type;
		// 1.2 dB per step: 1 = -18 dB (barely touching), 16 = 0 dB (the old setting 1), 50 = +41 dB.
		float driveDb = -19.2f + 1.2f * static_cast<float>(s.drive);
		drive_ = std::exp2(driveDb * (1.f / 6.0206f));
		makeup_ = (drive_ < 1.f) ? 1.f / drive_ : 1.f;
		bias_ = static_cast<float>(s.bias) * (0.6f / SaturationSettings::kMaxBias);
		biasOffset_ = shape(bias_);
		dcBlock_ = s.bias != 0;
		toneCoef_ = (s.tone >= SaturationSettings::kMaxTone) ? 1.f : onePoleCoef(hzFromSteps(s.tone, 600.f, 18000.f));
		lowCutCoef_ = (s.lowCut == 0) ? 0.f : onePoleCoef(hzFromSteps(s.lowCut - 1, 20.f, 1200.f));
		wet_ = static_cast<float>(s.mix) * (1.f / SaturationSettings::kMaxMix);
	}

	/// Mono buffer, or one channel of a buffer with the given stride.
	void process(int32_t* buffer, size_t numSamples, Channel& ch, size_t stride = 1) const {
		switch (type_) {
		case SaturationType::WARM:
			return run<SaturationType::WARM>(buffer, numSamples, ch, stride);
		case SaturationType::CLIP:
			return run<SaturationType::CLIP>(buffer, numSamples, ch, stride);
		case SaturationType::HARD:
			return run<SaturationType::HARD>(buffer, numSamples, ch, stride);
		case SaturationType::FOLD:
			return run<SaturationType::FOLD>(buffer, numSamples, ch, stride);
		default:
			return run<SaturationType::TAPE>(buffer, numSamples, ch, stride);
		}
	}

	/// Interleaved stereo.
	void processStereo(int32_t* buffer, size_t numFrames, Channel& l, Channel& r) const {
		process(buffer, numFrames, l, 2);
		process(buffer + 1, numFrames, r, 2);
	}

	// q31 level treated as 1.0 by the curves. Matches where the old saturation's tanh knee sat at setting 1.
	static constexpr float kUnit = 8388608.f; // 2^23

private:
	static constexpr float kSampleRate = 44100.f;
	static constexpr float kMaxInput = 32.f; // keeps the antiderivative maths well conditioned
	static constexpr float kAntialiasEpsilon = 2e-3f;

	static float hzFromSteps(int32_t step, float lowHz, float highHz) {
		float t = static_cast<float>(step) * (1.f / 49.f);
		return lowHz * std::pow(highHz / lowHz, t);
	}
	static float onePoleCoef(float hz) { return 1.f - std::exp(-6.2831853f * hz / kSampleRate); }

	// --- Curves and their antiderivatives (F' = f). All have unity slope at zero. ---

	// (15z - 10z^3 + 3z^5) / 8 on [-1, 1]: tanh-like, smooth (C2) into the flat top. Antiderivative is even.
	static constexpr float kQuinticScale = 8.f / 15.f;
	static float quintic(float z) {
		float z2 = z * z;
		return z * (1.875f + z2 * (-1.25f + 0.375f * z2));
	}
	static float quinticIntegral(float z) {
		float z2 = z * z;
		return z2 * (0.9375f + z2 * (-0.3125f + 0.0625f * z2));
	}

	// 1.5x - 0.5x^3 on [-1, 1], and its antiderivative for |x| <= 1 (even)
	static float cubic(float x) { return x * (1.5f - 0.5f * x * x); }
	static float cubicIntegral(float x) {
		float x2 = x * x;
		return x2 * (0.75f - 0.125f * x2);
	}

	// Triangle with period 4 and peaks at x = 1 + 4k. Sets rising when on an upward slope.
	static float triangle(float x, bool& rising) {
		float u = x + 1.f;
		float q = u * 0.25f;
		int32_t whole = static_cast<int32_t>(q);
		whole -= (static_cast<float>(whole) > q); // floor, without a libm call
		u -= 4.f * static_cast<float>(whole);
		rising = u < 2.f;
		return rising ? u - 1.f : 3.f - u;
	}

	template <SaturationType T>
	static float curve(float x) {
		if constexpr (T == SaturationType::WARM) {
			return x / std::sqrt(1.f + x * x);
		}
		else if constexpr (T == SaturationType::CLIP) {
			float z = x * (2.f / 3.f);
			return (z >= 1.f) ? 1.f : (z <= -1.f) ? -1.f : cubic(z);
		}
		else if constexpr (T == SaturationType::HARD) {
			return std::clamp(x, -1.f, 1.f);
		}
		else if constexpr (T == SaturationType::FOLD) {
			bool rising;
			return cubic(triangle(x * (2.f / 3.f), rising));
		}
		else {
			float z = x * kQuinticScale;
			return (z >= 1.f) ? 1.f : (z <= -1.f) ? -1.f : quintic(z);
		}
	}

	template <SaturationType T>
	static float antiderivative(float x) {
		if constexpr (T == SaturationType::WARM) {
			return std::sqrt(1.f + x * x) - 1.f;
		}
		else if constexpr (T == SaturationType::CLIP) {
			float z = std::fabs(x) * (2.f / 3.f);
			return 1.5f * ((z >= 1.f) ? z - 0.375f : cubicIntegral(z));
		}
		else if constexpr (T == SaturationType::HARD) {
			float a = std::fabs(x);
			return (a >= 1.f) ? a - 0.5f : 0.5f * a * a;
		}
		else if constexpr (T == SaturationType::FOLD) { // periodic, falling segments mirror the rising ones
			bool rising;
			float c = cubicIntegral(triangle(x * (2.f / 3.f), rising));
			return 1.5f * (rising ? c : 1.25f - c);
		}
		else {
			float z = std::fabs(x) * kQuinticScale;
			return (1.f / kQuinticScale) * ((z >= 1.f) ? z - 0.3125f : quinticIntegral(z));
		}
	}

	template <SaturationType T>
	static float shapeAntialiased(float x, Channel& c) {
		float f = antiderivative<T>(x);
		float d = x - c.prev;
		float y = (std::fabs(d) < kAntialiasEpsilon) ? curve<T>(0.5f * (x + c.prev)) : (f - c.prevF) / d;
		c.prev = x;
		c.prevF = f;
		return y;
	}

	float shape(float x) const {
		switch (type_) {
		case SaturationType::WARM:
			return curve<SaturationType::WARM>(x);
		case SaturationType::CLIP:
			return curve<SaturationType::CLIP>(x);
		case SaturationType::HARD:
			return curve<SaturationType::HARD>(x);
		case SaturationType::FOLD:
			return curve<SaturationType::FOLD>(x);
		default:
			return curve<SaturationType::TAPE>(x);
		}
	}

	template <SaturationType T>
	void run(int32_t* buffer, size_t numSamples, Channel& ch, size_t stride) const {
		constexpr float kToUnit = 1.f / kUnit;
		constexpr float kDcCoef = 0.9993f; // ~5 Hz
		const float outGain = makeup_ * wet_ * kUnit;
		const float dryGain = 1.f - wet_;

		Channel c = ch;
		if (c.prevType != T) { // the cached antiderivative belongs to another curve
			c.prevF = antiderivative<T>(c.prev);
			c.prevType = T;
		}
		int32_t* const end = buffer + numSamples * stride;
		for (int32_t* p = buffer; p != end; p += stride) {
			float dry = static_cast<float>(*p);
			float x = dry * kToUnit;

			// Pre high-pass, so the lows stay tight and don't smear the drive
			c.low += lowCutCoef_ * (x - c.low);
			x -= c.low;

			x = std::clamp(x * drive_ + bias_, -kMaxInput, kMaxInput);
			float y = shapeAntialiased<T>(x, c) - biasOffset_;

			// Bias leaves some DC that follows the signal level
			if (dcBlock_) {
				float dc = y - c.dcIn + kDcCoef * c.dcOut;
				c.dcIn = y;
				c.dcOut = dc;
				y = dc;
			}

			// Post low-pass to tame fizz
			c.tone += toneCoef_ * (y - c.tone);

			float out = c.tone * outGain + dry * dryGain;
			*p = static_cast<int32_t>(std::clamp(out, -2147483520.f, 2147483520.f));
		}
		ch = c;
	}

	SaturationType type_ = SaturationType::TAPE;
	float drive_ = 1.f;
	float makeup_ = 1.f;
	float bias_ = 0.f;
	float biasOffset_ = 0.f;
	float toneCoef_ = 1.f;
	float lowCutCoef_ = 0.f;
	float wet_ = 1.f;
	bool dcBlock_ = false;
};

} // namespace deluge::dsp
