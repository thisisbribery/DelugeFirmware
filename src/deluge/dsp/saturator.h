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
	TAPE, // smooth, slightly lopsided (even harmonics), turns into a hard clip at the top of the knob
	WARM, // softest knee, more lopsided, still hardens at the top
	CLIP, // firmer knee, hardens sooner
	HARD, // hard clip all the way
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

/// User-facing settings, as shown in the menus.
struct SaturationSettings {
	SaturationType type = SaturationType::TAPE;
	uint8_t drive = 0; // 0 = off, 1..50

	static constexpr int32_t kMaxDrive = 50;

	bool operator==(const SaturationSettings&) const = default;

	/// The old 1..15 "clippingAmount" mapped onto the 1..50 drive scale, and back (round trip is exact).
	static constexpr uint8_t driveFromLegacy(int32_t legacy) { return (legacy * 10 + 1) / 3; }
	static constexpr uint8_t legacyFromDrive(int32_t drive) {
		return drive ? std::clamp<int32_t>((drive * 3 + 5) / 10, 1, 15) : 0;
	}
};

/// One-knob drive, after the Sealegs drive's behaviour: pure input gain into the curve (up to +36 dB, steepest at
/// the top) with no level compensation, and a fixed ceiling at about normal peak level. So it gets denser and
/// louder as it goes up. The soft curves morph into a hard clip over the top half of the knob.
/// First-order antiderivative antialiasing throughout. Processes q31 audio in place.
class Saturator {
public:
	struct Channel {
		float prev = 0.f;  // previous curve input, for antialiasing
		float prevF = 0.f; // its antiderivative
		float dcIn = 0.f;  // DC blocker
		float dcOut = 0.f;
		SaturationType prevType = SaturationType::CLASSIC;
		float prevHardness = -1.f;
	};

	/// unit is the q31 level where the curve's knee sits at drive 1: about where normal signals peak.
	void configure(const SaturationSettings& s, float unit) {
		type_ = s.type;
		float t = static_cast<float>(std::max<int32_t>(s.drive, 1) - 1) * (1.f / (SaturationSettings::kMaxDrive - 1));
		float drive = std::pow(10.f, kMaxDriveDb * std::pow(t, 1.7f) * (1.f / 20.f));
		inGain_ = drive / unit;
		outGain_ = unit;

		switch (type_) {
		case SaturationType::WARM:
			bias_ = 0.25f;
			hardness_ = smoothstep(t, 0.6f, 1.f);
			break;
		case SaturationType::CLIP:
			bias_ = 0.08f;
			hardness_ = smoothstep(t, 0.35f, 0.9f);
			break;
		case SaturationType::HARD:
		case SaturationType::FOLD:
			bias_ = 0.f;
			hardness_ = 0.f;
			break;
		default:
			bias_ = 0.12f;
			hardness_ = smoothstep(t, 0.5f, 1.f);
			break;
		}
		// Quantised so the per-channel antiderivative cache isn't invalidated by tiny changes
		hardness_ = std::round(hardness_ * 64.f) * (1.f / 64.f);
		biasOffset_ = shape(bias_);
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

	// Where the knee and ceiling sit for a synth voice and for a whole kit or audio clip. The old saturation put
	// its ceiling at 2^23 and 2^25, well under normal levels, which is why it got quieter as it was turned up.
	// These are 3 bits higher, so drive 1 is close to clean and more drive is always louder.
	static constexpr float kVoiceUnit = 67108864.f; // 2^26
	static constexpr float kClipUnit = 268435456.f; // 2^28
	static constexpr float kMaxDriveDb = 36.f;

private:
	static constexpr float kMaxInput = 48.f; // keeps the antiderivative maths well conditioned
	static constexpr float kAntialiasEpsilon = 2e-3f;

	static float smoothstep(float t, float lo, float hi) {
		float u = std::clamp((t - lo) / (hi - lo), 0.f, 1.f);
		return u * u * (3.f - 2.f * u);
	}

	// --- Curves and their antiderivatives (F' = f). All have unity slope at zero and flatten at +-1. ---

	// (15z - 10z^3 + 3z^5) / 8 on [-1, 1]: tanh-like, smooth (C2) into the flat top
	static constexpr float kQuinticScale = 8.f / 15.f;
	static float quintic(float z) {
		float z2 = z * z;
		return z * (1.875f + z2 * (-1.25f + 0.375f * z2));
	}
	static float quinticIntegral(float z) {
		float z2 = z * z;
		return z2 * (0.9375f + z2 * (-0.3125f + 0.0625f * z2));
	}

	// 1.5z - 0.5z^3 on [-1, 1]
	static float cubic(float z) { return z * (1.5f - 0.5f * z * z); }
	static float cubicIntegral(float z) {
		float z2 = z * z;
		return z2 * (0.75f - 0.125f * z2);
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

	static float hardCurve(float x) { return std::clamp(x, -1.f, 1.f); }
	static float hardIntegral(float x) {
		float a = std::fabs(x);
		return (a >= 1.f) ? a - 0.5f : 0.5f * a * a;
	}

	template <SaturationType T>
	static float softCurve(float x) {
		if constexpr (T == SaturationType::WARM) {
			return x / std::sqrt(1.f + x * x);
		}
		else if constexpr (T == SaturationType::CLIP) {
			float z = x * (2.f / 3.f);
			return (z >= 1.f) ? 1.f : (z <= -1.f) ? -1.f : cubic(z);
		}
		else if constexpr (T == SaturationType::HARD) {
			return hardCurve(x);
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
	static float softIntegral(float x) {
		if constexpr (T == SaturationType::WARM) {
			return std::sqrt(1.f + x * x) - 1.f;
		}
		else if constexpr (T == SaturationType::CLIP) {
			float z = std::fabs(x) * (2.f / 3.f);
			return 1.5f * ((z >= 1.f) ? z - 0.375f : cubicIntegral(z));
		}
		else if constexpr (T == SaturationType::HARD) {
			return hardIntegral(x);
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

	// The soft curve blended towards a hard clip. Both are linear in the blend, so the antiderivative is too.
	template <SaturationType T>
	static float curve(float x, float hardness) {
		float s = softCurve<T>(x);
		return s + hardness * (hardCurve(x) - s);
	}
	template <SaturationType T>
	static float antiderivative(float x, float hardness) {
		float s = softIntegral<T>(x);
		return s + hardness * (hardIntegral(x) - s);
	}

	float shape(float x) const {
		switch (type_) {
		case SaturationType::WARM:
			return curve<SaturationType::WARM>(x, hardness_);
		case SaturationType::CLIP:
			return curve<SaturationType::CLIP>(x, hardness_);
		case SaturationType::HARD:
			return curve<SaturationType::HARD>(x, hardness_);
		case SaturationType::FOLD:
			return curve<SaturationType::FOLD>(x, hardness_);
		default:
			return curve<SaturationType::TAPE>(x, hardness_);
		}
	}

	template <SaturationType T>
	void run(int32_t* buffer, size_t numSamples, Channel& ch, size_t stride) const {
		constexpr float kDcCoef = 0.9993f; // ~5 Hz, removes the DC the bias leaves behind
		const float hardness = hardness_;
		const bool dcBlock = bias_ != 0.f;

		Channel c = ch;
		if (c.prevType != T || c.prevHardness != hardness) { // the cached antiderivative belongs to another curve
			c.prevF = antiderivative<T>(c.prev, hardness);
			c.prevType = T;
			c.prevHardness = hardness;
		}
		int32_t* const end = buffer + numSamples * stride;
		for (int32_t* p = buffer; p != end; p += stride) {
			float x = std::clamp(static_cast<float>(*p) * inGain_ + bias_, -kMaxInput, kMaxInput);

			float f = antiderivative<T>(x, hardness);
			float d = x - c.prev;
			float y = (std::fabs(d) < kAntialiasEpsilon) ? curve<T>(0.5f * (x + c.prev), hardness) : (f - c.prevF) / d;
			c.prev = x;
			c.prevF = f;
			y -= biasOffset_;

			if (dcBlock) {
				float dc = y - c.dcIn + kDcCoef * c.dcOut;
				c.dcIn = y;
				c.dcOut = dc;
				y = dc;
			}

			*p = static_cast<int32_t>(std::clamp(y * outGain_, -2147483520.f, 2147483520.f));
		}
		ch = c;
	}

	SaturationType type_ = SaturationType::TAPE;
	float inGain_ = 0.f;
	float outGain_ = 0.f;
	float bias_ = 0.f;
	float biasOffset_ = 0.f;
	float hardness_ = 0.f;
};

} // namespace deluge::dsp
