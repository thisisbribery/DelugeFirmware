/*
 * Copyright © 2026 Synthstrom Audible Limited
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
 *
 * Ported from Open303 by Robin Schmidt (https://github.com/RobinSchmidt/Open303), which is released under the MIT
 * license:
 *
 *   Copyright (c) 2009 Robin Schmidt (www.rs-met.com)
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 *   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and
 *   to permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all copies or substantial portions of
 *   the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 *   THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF
 *   CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 *   IN THE SOFTWARE.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <span>

// A mono TB-303 voice: Open303's oscillator, filter, envelopes, accent and slide, in single precision. Notes that
// overlap slide, as on Open303 and the 303 itself; a note with velocity 100 or more is accented.
namespace deluge::dsp::tb303 {

/// Band-limited 303 saw and square, one table per octave, shared by every 303 sound
namespace wavetables {
constexpr int32_t kLengthMagnitude = 11;
constexpr int32_t kLength = 1 << kLengthMagnitude;
constexpr int32_t kNumTables = 12;
enum Wave : uint8_t { SAW, SQUARE };
/// Builds the tables the first time (takes a few milliseconds). Call it from outside the audio routine.
void ensureBuilt();
bool built();
const float* get(Wave wave, int32_t table);
} // namespace wavetables

/// The filter models. DIRT is ours rather than Open303's: the TB-303 ladder with a saturating input stage.
enum class FilterMode : uint8_t { CLASSIC, LP18, DIRT };

/// The front panel, plus Devil Fish style extras. Times are in ms and amounts in percent, as on Open303.
struct Controls {
	float cutoff = 500.f;    // Hz, 314 to 2394 on the panel
	float resonance = 50.f;  // %
	float envMod = 25.f;     // %
	float decay = 400.f;     // ms, 200 to 2000 on the panel
	float accent = 50.f;     // %
	float waveform = 0.f;    // 0 saw to 1 square
	float slideTime = 60.f;  // ms, the time constant of the glide, as Open303 sets it up
	float ampDecay = 1230.f; // ms
	float ampSustain = 0.f;  // 0 to 1
	float drive = 0.f;       // 0 to 1. Into the filter on DIRT, else into the drum synth engines' soft clipper after it
	FilterMode filterMode = FilterMode::DIRT;
};

class OnePole {
public:
	enum Mode : uint8_t { HIGHPASS, ALLPASS };
	void setup(Mode mode, float cutoff, float sampleRate);
	[[gnu::always_inline]] inline float process(float in) {
		y1 = b0 * in + b1 * x1 + a1 * y1;
		x1 = in;
		return y1;
	}
	void reset() { x1 = y1 = 0.f; }

private:
	float b0 = 1.f, b1 = 0.f, a1 = 0.f;
	float x1 = 0.f, y1 = 0.f;
};

class Biquad {
public:
	void setupLowpass(float frequency, float q, float sampleRate);
	void setupNotch(float frequency, float bandwidthOctaves, float sampleRate);
	[[gnu::always_inline]] inline float process(float in) {
		float y = b0 * in + b1 * x1 + b2 * x2 + a1 * y1 + a2 * y2;
		x2 = x1;
		x1 = in;
		y2 = y1;
		y1 = y;
		return y;
	}
	void reset() { x1 = x2 = y1 = y2 = 0.f; }

private:
	float b0 = 1.f, b1 = 0.f, b2 = 0.f, a1 = 0.f, a2 = 0.f;
	float x1 = 0.f, x2 = 0.f, y1 = 0.f, y2 = 0.f;
};

/// Open303's 12th order elliptic quarter-band lowpass, split into biquads so it holds up in single precision
class AntiAliasFilter {
public:
	[[gnu::always_inline]] inline float process(float in) {
		float x = in;
#pragma GCC unroll 6
		for (int32_t s = 0; s < 6; s++) {
			// Transposed direct form II
			const float* c = kSections[s];
			std::array<float, 2>& z = state[s];
			float y = c[0] * x + z[0];
			z[0] = c[1] * x + c[3] * y + z[1];
			z[1] = c[2] * x + c[4] * y;
			x = y;
		}
		return x;
	}
	void reset() { state = {}; }

private:
	// b0, b1, b2, -a1, -a2 for each section. The product of these is Open303's filter to within 3e-7.
	static constexpr float kSections[6][5] = {
	    {0.016780659926151895f, 0.022947305659324117f, 0.016780659926151933f, 1.5863116558748012f,
	     -0.6428202813864291f},
	    {0.09459763035184697f, -0.035133895460929543f, 0.09459763035184561f, 1.5620957226926269f, -0.7161570879353899f},
	    {0.29069484193705036f, -0.29936460675402321f, 0.290694841937152f, 1.5310700890181974f, -0.81309516613837662f},
	    {0.52578571778729133f, -0.66650721431468896f, 0.52578571778628858f, 1.5080939966772393f, -0.89315821793613026f},
	    {0.70505069759938288f, -0.96089912665161337f, 0.70505069760221728f, 1.4983265489464017f, -0.9475288174963884f},
	    {0.79922266095877925f, -1.117333033909587f, 0.79922266095683048f, 1.5032624520096824f, -0.98437474001570513f},
	};
	std::array<std::array<float, 2>, 6> state{};
};

/// Open303's TeeBeeFilter: a ladder with a highpass in its feedback path. Open303 leaves it linear (its saturation is
/// commented out, as a todo), so DIRT adds one.
class TeeBeeFilter {
public:
	struct Coefficients {
		float b0, a1, k, g;
	};

	void setSampleRate(float sampleRate);
	void setResonance(float percent);
	void setMode(FilterMode mode) { mode_ = mode; }
	FilterMode mode() const { return mode_; }
	/// DIRT's drive knob, 0 to 1
	void setDrive(float drive);
	Coefficients coefficientsFor(float cutoff) const;

	/// Glides the coefficients from where they are to target over the next numSamples calls to step()
	[[gnu::always_inline]] inline void glideTo(Coefficients const& target, int32_t numSamples) {
		float scale = 1.f / (float)numSamples;
		step_ = {(target.b0 - now.b0) * scale, (target.a1 - now.a1) * scale, (target.k - now.k) * scale,
		         (target.g - now.g) * scale};
	}
	/// Like glideTo(), but the first step() lands on first rather than carrying on from where the coefficients are
	[[gnu::always_inline]] inline void jumpTo(Coefficients const& first, Coefficients const& target,
	                                          int32_t numSamples) {
		now = first;
		if (numSamples > 1) {
			glideTo(target, numSamples - 1);
		}
		else {
			step_ = {};
		}
		now = {now.b0 - step_.b0, now.a1 - step_.a1, now.k - step_.k, now.g - step_.g};
	}
	/// Once per output sample, before it's processed
	[[gnu::always_inline]] inline void step() {
		now = {now.b0 + step_.b0, now.a1 + step_.a1, now.k + step_.k, now.g + step_.g};
	}

	template <FilterMode mode>
	[[gnu::always_inline]] inline float process(float in) {
		if constexpr (mode == FilterMode::CLASSIC) {
			float y0 = in - feedbackHighpass.process(now.k * y4);
			y1 += 2.f * now.b0 * (y0 - y1 + y2);
			y2 += now.b0 * (y1 - 2.f * y2 + y3);
			y3 += now.b0 * (y2 - 2.f * y3 + y4);
			y4 += now.b0 * (y3 - 2.f * y4);
			return 2.f * now.g * y4;
		}
		else if constexpr (mode == FilterMode::DIRT) {
			// The classic ladder, with the input and the resonance meeting in a saturating stage, as at a diode
			// ladder's input pair. Driven harder, the resonance squashes and growls instead of ringing clean.
			float y0 = dirt(dirtIn * in - feedbackHighpass.process(now.k * y4));
			y1 += 2.f * now.b0 * (y0 - y1 + y2);
			y2 += now.b0 * (y1 - 2.f * y2 + y3);
			y3 += now.b0 * (y2 - 2.f * y3 + y4);
			y4 += now.b0 * (y3 - 2.f * y4);
			return dirtOut * now.g * y4;
		}
		else {
			float y0 = 0.125f * in - feedbackHighpass.process(now.k * y4);
			y1 = y0 + now.a1 * (y0 - y1);
			y2 = y1 + now.a1 * (y1 - y2);
			y3 = y2 + now.a1 * (y2 - y3);
			y4 = y3 + now.a1 * (y3 - y4);
			return 8.f * y3;
		}
	}
	void reset();

private:
	/// Cubic soft clip, flat at +-1 from +-1.5, with a little offset for even harmonics (the offset's DC is taken back
	/// out, so silence stays silent)
	[[gnu::always_inline]] static inline float dirt(float x) {
		constexpr float kBias = 0.2f;
		constexpr float kBiasOut = kBias - (4.f / 27.f) * kBias * kBias * kBias;
		x = std::clamp(x + kBias, -1.5f, 1.5f);
		return x - (4.f / 27.f) * x * x * x - kBiasOut;
	}

	void updateDirt();

	Coefficients now{0.f, 0.f, 0.f, 1.f};
	Coefficients step_{};
	float y1 = 0.f, y2 = 0.f, y3 = 0.f, y4 = 0.f;
	float resonanceSkewed = 0.f;
	float twoPiOverSampleRate = 0.f;
	FilterMode mode_ = FilterMode::DIRT;
	float dirtBoost = 1.f; // The drive's input gain over DIRT's at 0
	float dirtIn = 0.5f;
	float dirtOut = 4.f;
	OnePole feedbackHighpass;
};

class Open303 {
public:
	static constexpr float kSampleRate = 44100.f;
	static constexpr int32_t kOversampling = 4;
	/// Output samples between filter coefficient updates. They glide in between, so the envelope doesn't zipper.
	static constexpr int32_t kControlInterval = 4;

	Open303();

	/// Picks up the latest controls. Cutoff, envelope modulation, resonance, accent and waveform act at once;
	/// decay and slide act from the next note, as on Open303.
	void setControls(Controls const& controls);

	void noteOn(int32_t note, int32_t velocity);
	void noteOff(int32_t note);
	void allNotesOff();
	/// Stops dead and clears all state
	void reset();

	/// Semitones added to every note, for the sound's transpose
	void setPitchOffset(float semitones) { pitchOffset = semitones; }

	bool isSounding() const { return sounding; }
	bool isNoteHeld(int32_t note) const {
		for (int32_t i = 0; i < numNotes; i++) {
			if (noteList[i] == note) {
				return true;
			}
		}
		return false;
	}

	/// Adds the voice to a mono q31 buffer, with 1.0 at full scale scaled to gain
	void render(std::span<int32_t> buffer, float gain);

private:
	template <FilterMode mode>
	void renderVoice(std::span<int32_t> buffer, float gain);
	void triggerNote(int32_t note, bool accent);
	void slideToNote(int32_t note, bool accent);
	void releaseNote();
	void setAccentState(bool accent);
	void calculateEnvModScalerAndOffset();
	float noteToFrequency(int32_t note) const;

	Controls controls;

	// Notes held, most recent first, for slides back to an earlier note
	static constexpr int32_t kMaxNotes = 16;
	std::array<int8_t, kMaxNotes> noteList{};
	int32_t numNotes = 0;
	int32_t currentNote = -1;

	float pitchOffset = 0.f;
	float oscFreq = 440.f;
	float cutoffNow = 500.f; // The cutoff, glided across each render
	float envScaler = 1.f;
	float envOffset = 0.f;
	float accentGain = 0.f;
	bool sounding = false;
	bool filterJump = true; // The next coefficient update jumps rather than glides: a new note, accent or knob move

	// Oscillator: one phase for both waves, in fractions of a cycle
	uint32_t phase = 0;
	uint32_t phaseIncrement = 0;

	// Main envelope: a pure exponential decay
	float mainEnv = 0.f;
	float mainEnvCoeff = 1.f;

	// Amp envelope: Open303's AnalogEnvelope, which with no attack or hold jumps to 1, then decays to the sustain
	// level while the note is held
	float ampEnv = 0.f;
	float ampDecayCoeff = 0.f;
	float ampReleaseCoeff = 1.f;
	float lastAmp = 0.f; // After the declicker
	bool ampAttackPending = false;
	bool ampNoteOn = false;

	// Leaky integrators
	float pitchSlew = 440.f;
	float pitchSlewCoeff = 0.f;
	float rc2 = 0.f;
	float rc2Coeff = 0.f;

	TeeBeeFilter filter;
	Biquad ampDeClicker;
	OnePole highpass1, highpass2, allpass;
	Biquad notch;
	AntiAliasFilter antiAlias;
};

} // namespace deluge::dsp::tb303
