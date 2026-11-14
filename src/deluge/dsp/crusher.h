/*
 * Copyright © 2026 thisisbribery
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
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>

namespace deluge::dsp {

/// What the Bitcrush and Decimation knobs sound like. CLASSIC is the original Deluge code, kept so old songs
/// sound the same. The others hold each sample flat (zero-order hold) instead of drawing a line to the next one,
/// so decimation aliases like a real low sample rate instead of acting like a lowpass filter, and their bit depth
/// sweeps smoothly instead of in 8 steps.
enum class CrushType : uint8_t {
	CLASSIC,
	HARD,    // pure digital: sample and hold, rounded levels, 12 bits down to 1
	VINTAGE, // old sampler: soft input, 12-bit floor, 26 kHz-ish clock, one soft pole after the hold
	ULAW,    // companded (mu-law style segments, after Mutable Instruments Clouds/Beads): quiet tails stay grainy
	JITTER,  // unstable clock and a dithered threshold: noisy, crackly, sputtering
	NUM_TYPES,
};

inline constexpr const char* kCrushTypeNames[] = {"classic", "hard", "vintage", "ulaw", "jitter"};
static_assert(std::size(kCrushTypeNames) == static_cast<size_t>(CrushType::NUM_TYPES));

inline const char* crushTypeToString(CrushType type) {
	return kCrushTypeNames[static_cast<size_t>(type)];
}
inline CrushType stringToCrushType(const char* string) {
	for (size_t i = 0; i < std::size(kCrushTypeNames); i++) {
		if (!strcmp(string, kCrushTypeNames[i])) {
			return static_cast<CrushType>(i);
		}
	}
	return CrushType::CLASSIC;
}

/// Bitcrush + decimation for every type except CLASSIC. Works on q31 stereo in place.
class Crusher {
public:
	/// unit is the q31 level normal signals peak at: 2^26 for a sound, 2^28 for a kit, audio clip or the song.
	static constexpr float kSoundUnit = 67108864.f; // 2^26
	static constexpr float kClipUnit = 268435456.f; // 2^28

	struct State {
		float heldL = 0.f;
		float heldR = 0.f;
		float countdown = 0.f; // samples until the next grab
		float lpL = 0.f, lpR = 0.f;
		uint32_t rng = 0x9E3779B9u;
		bool active = false;
	};

	struct Config {
		CrushType type = CrushType::HARD;
		bool crush = false;
		bool decimate = false;
		float step = 0.f; // quantiser step, in units of normal peak level
		float invStep = 0.f;
		float muStep = 0.f; // ULAW: step as a fraction of each segment
		float invMuStep = 0.f;
		float muBias = 33.f;
		float period = 1.f;   // samples per held sample
		float jitter = 0.f;   // +- fraction of the period
		float dither = 0.f;   // +- fraction of a step added before rounding
		float lpCoef = 1.f;   // VINTAGE output filter
		float inScale = 0.f;  // q31 -> 1 = normal peak
		float outScale = 0.f; // back to q31
	};

	/// crushKnob and rateKnob are 0..1 along the part of the knob where each is on.
	static Config configure(CrushType type, bool crush, float crushKnob, bool decimate, float rateKnob, float unit) {
		Config c;
		c.type = type;
		c.crush = crush;
		c.decimate = decimate;
		// Levels are measured in units of normal peak level; louder signals go above 1 and aren't clipped
		c.inScale = 1.f / unit;
		c.outScale = unit;
		crushKnob = std::clamp(crushKnob, 0.f, 1.f);
		rateKnob = std::clamp(rateKnob, 0.f, 1.f);

		// Bits, exponential along the knob so most of the travel is in the range that's audible
		float bits = 0.f;
		switch (type) {
		case CrushType::VINTAGE:
			// Always a 12-bit converter; the knob takes it down to 3
			bits = crush ? 12.f * std::pow(3.f / 12.f, crushKnob) : 12.f;
			c.crush = true;
			break;
		case CrushType::ULAW:
			// Bits in each segment: 5 (smooth), 4 (8-bit telephone), down to 0 (one level per octave). Towards the
			// end the bottom segments merge too, so quiet parts get coarse and gate
			c.muStep = std::exp2(-(5.f - 5.f * crushKnob));
			c.muBias = 33.f * std::exp2(6.f * crushKnob * crushKnob);
			c.invMuStep = 1.f / c.muStep;
			break;
		default:
			bits = 12.f * std::pow(1.f / 12.f, crushKnob);
			break;
		}
		c.step = 2.f * std::exp2(-bits);
		c.invStep = 1.f / c.step;

		// Clock: up to 256x down (172 Hz), shaped so the middle of the knob is around 5 kHz
		float octaves = 8.f * std::pow(rateKnob, 1.4f);
		switch (type) {
		case CrushType::VINTAGE:
			// Starts at about 26 kHz, ends around 580 Hz
			c.period = (44100.f / 26040.f) * std::exp2(5.5f * std::pow(rateKnob, 1.4f));
			// One soft pole at the held rate: rounds the edges of the steps a little, keeps the aliasing
			{
				float fc = std::min(44100.f / c.period, 16000.f);
				c.lpCoef = 1.f - std::exp(-6.2831853f * fc / 44100.f);
			}
			break;
		default:
			c.period = std::exp2(octaves);
			break;
		}
		if (type == CrushType::JITTER) {
			c.jitter = 0.5f;
			c.dither = 0.4f;
		}
		return c;
	}

	static void process(const Config& c, int32_t* interleaved, size_t numFrames, State& s) {
		if (!s.active) {
			s = State{};
			s.active = true;
		}
		switch (c.type) {
		case CrushType::VINTAGE:
			return run<CrushType::VINTAGE>(c, interleaved, numFrames, s);
		case CrushType::ULAW:
			return run<CrushType::ULAW>(c, interleaved, numFrames, s);
		case CrushType::JITTER:
			return run<CrushType::JITTER>(c, interleaved, numFrames, s);
		default:
			return run<CrushType::HARD>(c, interleaved, numFrames, s);
		}
	}

	/// xorshift32, -1..1
	static float noise(uint32_t& r) {
		r ^= r << 13;
		r ^= r >> 17;
		r ^= r << 5;
		return static_cast<float>(static_cast<int32_t>(r)) * (1.f / 2147483648.f);
	}

	/// No floor instruction on the A9 (std::floor is a library call), and the values here fit an int easily
	static float fastFloor(float v) {
		int32_t i = static_cast<int32_t>(v);
		return static_cast<float>(i - (v < static_cast<float>(i)));
	}

	static int32_t toQ31(float v) { return static_cast<int32_t>(std::clamp(v, -2147483520.f, 2147483520.f)); }

	template <CrushType T>
	static float quantise(const Config& c, float x, float d) {
		if constexpr (T == CrushType::ULAW) {
			// G.711-style segments on a 13-bit scale: each octave of level gets the same number of steps
			constexpr float kScale = 8159.f;
			const float kBias = c.muBias;
			if (x == 0.f) {
				return 0.f; // otherwise silence turns into a tiny DC offset
			}
			float a = std::min(std::fabs(x), 8.f) * kScale + kBias; // up to 3 extra segments of headroom
			uint32_t e = (std::bit_cast<uint32_t>(a) >> 23) & 0xFF; // floor(log2) + 127
			float segment = std::bit_cast<float>(e << 23);          // 2^floor(log2 a)
			float invSegment = std::bit_cast<float>((254u - e) << 23);
			float step = segment * c.muStep;
			float q = (fastFloor(a * invSegment * c.invMuStep) + 0.5f) * step;
			q = std::max(q - kBias, 0.f) * (1.f / kScale);
			return std::copysign(q, x);
		}
		else {
			// Mid-tread rounding: no DC, quiet signals gate to zero at low bit depths
			return fastFloor(std::clamp(x, -64.f, 64.f) * c.invStep + 0.5f + d) * c.step;
		}
	}

	template <CrushType T>
	static float shapeIn(float x) {
		if constexpr (T == CrushType::VINTAGE) {
			// Soft converter input: unity for small signals, rounds off above normal peak level, ceiling 1.33
			x = std::clamp(x * 0.5f, -1.f, 1.f);
			return 2.f * (x - x * x * x * (1.f / 3.f));
		}
		return x;
	}

	template <CrushType T>
	static void run(const Config& c, int32_t* buf, size_t numFrames, State& s) {
		for (size_t i = 0; i < numFrames; i++) {
			float l = shapeIn<T>(static_cast<float>(buf[2 * i]) * c.inScale);
			float r = shapeIn<T>(static_cast<float>(buf[2 * i + 1]) * c.inScale);

			bool grab = true;
			if (c.decimate) {
				s.countdown -= 1.f;
				grab = s.countdown <= 0.f;
				if (grab) {
					float period = c.period;
					if constexpr (T == CrushType::JITTER) {
						period *= 1.f + c.jitter * noise(s.rng);
					}
					s.countdown += period;
					if (s.countdown < 0.f) { // the period got shorter a lot all at once
						s.countdown = period;
					}
				}
			}

			if (grab) {
				if (c.crush) {
					float d = 0.f;
					if constexpr (T == CrushType::JITTER) {
						d = c.dither * noise(s.rng);
					}
					l = quantise<T>(c, l, d);
					r = quantise<T>(c, r, d);
				}
				s.heldL = l;
				s.heldR = r;
			}
			l = s.heldL;
			r = s.heldR;

			if constexpr (T == CrushType::VINTAGE) {
				if (c.decimate) {
					s.lpL += (l - s.lpL) * c.lpCoef;
					s.lpR += (r - s.lpR) * c.lpCoef;
					l = s.lpL;
					r = s.lpR;
				}
			}

			buf[2 * i] = toQ31(l * c.outScale);
			buf[2 * i + 1] = toQ31(r * c.outScale);
		}
	}
};

} // namespace deluge::dsp
