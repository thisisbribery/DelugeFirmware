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

/// The front panel, plus Devil Fish style extras. Times are in ms and amounts in percent, as on Open303.
struct Controls {
	float cutoff = 500.f;      // Hz, 314 to 2394 on the panel
	float resonance = 50.f;    // %
	float envMod = 25.f;       // %
	float decay = 400.f;       // ms, 200 to 2000 on the panel
	float accent = 50.f;       // %
	float waveform = 0.f;      // 0 saw to 1 square
	float slideTime = 60.f;    // ms, the time constant of the glide, as Open303 sets it up
	float ampDecay = 1230.f;   // ms
	float ampSustain = 0.f;    // 0 to 1
	float drive = 0.f;         // 0 to 1, into the drum synth engines' soft clipper. 0 is clean
	bool classicFilter = true; // The TB-303 ladder model, or Open303's plain 18 dB lowpass
};

class OnePole {
public:
	enum Mode : uint8_t { HIGHPASS, ALLPASS };
	void setup(Mode mode, float cutoff, float sampleRate);
	inline float process(float in) {
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
	inline float process(float in) {
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
	float process(float in);
	void reset() { state = {}; }

private:
	std::array<std::array<float, 2>, 6> state{};
};

/// Open303's TeeBeeFilter: a ladder with a highpass in its feedback path
class TeeBeeFilter {
public:
	void setSampleRate(float sampleRate);
	void setResonance(float percent);
	void setClassic(bool classic) { classic_ = classic; }
	void setCutoff(float cutoff);
	inline float process(float in) {
		if (classic_) {
			float y0 = in - feedbackHighpass.process(k * y4);
			y1 += 2.f * b0 * (y0 - y1 + y2);
			y2 += b0 * (y1 - 2.f * y2 + y3);
			y3 += b0 * (y2 - 2.f * y3 + y4);
			y4 += b0 * (y3 - 2.f * y4);
			return 2.f * g * y4;
		}
		float y0 = 0.125f * in - feedbackHighpass.process(k * y4);
		y1 = y0 + a1 * (y0 - y1);
		y2 = y1 + a1 * (y1 - y2);
		y3 = y2 + a1 * (y2 - y3);
		y4 = y3 + a1 * (y3 - y4);
		return 8.f * y3;
	}
	void reset();

private:
	void calculateCoefficients();
	float b0 = 0.f, a1 = 0.f, k = 0.f, g = 1.f;
	float y1 = 0.f, y2 = 0.f, y3 = 0.f, y4 = 0.f;
	float cutoff_ = 1000.f;
	float resonanceSkewed = 0.f;
	float twoPiOverSampleRate = 0.f;
	bool classic_ = true;
	OnePole feedbackHighpass;
};

class Open303 {
public:
	static constexpr float kSampleRate = 44100.f;
	static constexpr int32_t kOversampling = 4;

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
	float renderSample();
	void triggerNote(int32_t note, bool accent);
	void slideToNote(int32_t note, bool accent);
	void releaseNote();
	void setAccentState(bool accent);
	void calculateEnvModScalerAndOffset();
	float noteToFrequency(int32_t note) const;
	inline float oscillator();

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
