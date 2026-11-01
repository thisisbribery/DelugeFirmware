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
 * Ported from Open303 by Robin Schmidt (MIT license, see open303.h).
 */
#include "dsp/tb303/open303.h"
#include "definitions.h"
#include "dsp/drum_synth/cyc.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <complex>

namespace deluge::dsp::tb303 {

namespace {
constexpr double kPi = 3.1415926535897932384626433832795;
constexpr float kPiF = 3.14159265358979f;

/// Open303's LeakyIntegrator coefficient for a time constant in ms
float leakyCoeff(float tauMs, float sampleRate) {
	return (tauMs > 0.f) ? std::exp(-1.f / (sampleRate * 0.001f * tauMs)) : 0.f;
}

/// Open303's AnalogEnvelope coefficient for a stage time in ms
float envelopeCoeff(float timeMs, float sampleRate) {
	return (timeMs > 0.f) ? 1.f - std::exp(-1.f / (sampleRate * 0.001f * timeMs)) : 1.f;
}

/// 2 to the x, to within 2e-7: the whole part straight into the exponent, and a polynomial for the fraction
[[gnu::always_inline]] inline float fastExp2(float x) {
	x = std::clamp(x, -126.f, 126.f);
	int32_t whole = (int32_t)x;
	whole -= (x < (float)whole) ? 1 : 0; // Round down rather than towards zero
	float f = x - (float)whole;
	float p = 1.8775767e-3f;
	p = p * f + 8.9893406e-3f;
	p = p * f + 5.5826318e-2f;
	p = p * f + 2.4015361e-1f;
	p = p * f + 6.9315308e-1f;
	p = p * f + 9.9999994e-1f;
	return p * std::bit_cast<float>((whole + 127) << 23);
}

/// Linear interpolation into a wavetable
[[gnu::always_inline]] inline float readTable(const float* table, int32_t index, float frac) {
	return table[index] + frac * (table[index + 1] - table[index]);
}
} // namespace

// Wavetables

namespace wavetables {
namespace {
constexpr int32_t kStride = kLength + 4; // 4 extra samples so interpolation never wraps

PLACE_SDRAM_BSS float tables[2][kNumTables][kStride];
PLACE_SDRAM_BSS std::complex<double> spectrum[kLength];
PLACE_SDRAM_BSS std::complex<double> work[kLength];
PLACE_SDRAM_BSS double prototype[kLength];
bool isBuilt = false;

// In-place radix-2 FFT. inverse leaves out the 1/N.
void fft(std::complex<double>* x, bool inverse) {
	for (int32_t i = 1, j = 0; i < kLength; i++) {
		int32_t bit = kLength >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			std::swap(x[i], x[j]);
		}
	}
	for (int32_t len = 2; len <= kLength; len <<= 1) {
		double angle = 2 * kPi / len * (inverse ? 1 : -1);
		std::complex<double> step(std::cos(angle), std::sin(angle));
		for (int32_t i = 0; i < kLength; i += len) {
			std::complex<double> w(1.0, 0.0);
			for (int32_t j = 0; j < len / 2; j++) {
				std::complex<double> u = x[i + j];
				std::complex<double> v = x[i + j + len / 2] * w;
				x[i + j] = u + v;
				x[i + j + len / 2] = u - v;
				w *= step;
			}
		}
	}
}

// Open303's 303 saw: rises from 0 to 1 over the first half, jumps to -1 and rises back to 0
void fillSaw() {
	constexpr int32_t n1 = 1024; // roundToInt(0.5 * (kLength - 1))
	constexpr int32_t n2 = kLength - n1;
	for (int32_t n = 0; n < n1; n++) {
		prototype[n] = (double)n / (n1 - 1);
	}
	for (int32_t n = n1; n < kLength; n++) {
		prototype[n] = -1.0 + (double)(n - n1) / n2;
	}
}

// Open303's 303 square: the saw, through a tanh with an offset, half a cycle out
void fillSquare() {
	const double drive = std::exp(36.9 * 0.11512925464970228);
	constexpr double offset = 4.37;
	fillSaw();
	for (double& sample : prototype) {
		sample = -std::tanh(drive * sample + offset);
	}
	std::rotate(prototype, prototype + kLength / 2, prototype + kLength);
}

void addGuardSamples(float* table) {
	for (int32_t i = 0; i < 4; i++) {
		table[kLength + i] = table[i];
	}
}

// Like Open303: table 0 is the waveform as drawn, and each table after it keeps half the harmonics of the one before
void buildMipMap(float (*mipMap)[kStride]) {
	for (int32_t n = 0; n < kLength; n++) {
		mipMap[0][n] = (float)prototype[n];
		spectrum[n] = prototype[n];
	}
	addGuardSamples(mipMap[0]);
	fft(spectrum, false);

	for (int32_t t = 1; t < kNumTables; t++) {
		int32_t numHarmonics = kLength >> (t + 1);
		std::fill(work, work + kLength, std::complex<double>(0.0, 0.0));
		for (int32_t h = 1; h < numHarmonics; h++) {
			work[h] = spectrum[h];
			work[kLength - h] = spectrum[kLength - h];
		}
		fft(work, true);
		for (int32_t n = 0; n < kLength; n++) {
			mipMap[t][n] = (float)(work[n].real() / kLength);
		}
		addGuardSamples(mipMap[t]);
	}
}
} // namespace

void ensureBuilt() {
	if (isBuilt) {
		return;
	}
	fillSaw();
	buildMipMap(tables[SAW]);
	fillSquare();
	buildMipMap(tables[SQUARE]);
	isBuilt = true;
}

bool built() {
	return isBuilt;
}

const float* get(Wave wave, int32_t table) {
	return tables[wave][table];
}
} // namespace wavetables

// Filters

void OnePole::setup(Mode mode, float cutoff, float sampleRate) {
	if (mode == HIGHPASS) {
		float x = std::exp(-2.f * kPiF * cutoff / sampleRate);
		b0 = 0.5f * (1.f + x);
		b1 = -b0;
		a1 = x;
	}
	else {
		float t = std::tan(kPiF * cutoff / sampleRate);
		float x = (t - 1.f) / (t + 1.f);
		b0 = x;
		b1 = 1.f;
		a1 = -x;
	}
}

void Biquad::setupLowpass(float frequency, float q, float sampleRate) {
	float w = 2.f * kPiF * frequency / sampleRate;
	float s = std::sin(w);
	float c = std::cos(w);
	float alpha = s / (2.f * q);
	float scale = 1.f / (1.f + alpha);
	a1 = 2.f * c * scale;
	a2 = (alpha - 1.f) * scale;
	b1 = (1.f - c) * scale;
	b0 = 0.5f * b1;
	b2 = b0;
}

void Biquad::setupNotch(float frequency, float bandwidthOctaves, float sampleRate) {
	// In double: the notch sits a few Hz up, where single precision cos() is too coarse
	double w = 2 * kPi * frequency / sampleRate;
	double s = std::sin(w);
	double c = std::cos(w);
	double alpha = s * std::sinh(0.5 * std::log(2.0) * bandwidthOctaves * w / s);
	double scale = 1.0 / (1.0 + alpha);
	a1 = (float)(2.0 * c * scale);
	a2 = (float)((alpha - 1.0) * scale);
	b0 = (float)scale;
	b1 = (float)(-2.0 * c * scale);
	b2 = (float)scale;
}

void TeeBeeFilter::setSampleRate(float sampleRate) {
	twoPiOverSampleRate = 2.f * kPiF / sampleRate;
	feedbackHighpass.setup(OnePole::HIGHPASS, 150.f, sampleRate);
}

void TeeBeeFilter::setResonance(float percent) {
	float r = 0.01f * percent;
	resonanceSkewed = (1.f - std::exp(-3.f * r)) / (1.f - std::exp(-3.f));
	updateDirt();
}

TeeBeeFilter::Coefficients TeeBeeFilter::coefficientsFor(float cutoff) const {
	float wc = twoPiOverSampleRate * std::clamp(cutoff, 200.f, 20000.f);
	float r = resonanceSkewed;

	if (mode_ != FilterMode::LP18) {
		// Ala mystran and kunn, from the KVR thread
		float fx = wc * (0.70710678f / (2.f * kPiF));
		float b0 = (0.00045522346f + 6.1922189f * fx) / (1.f + 12.358354f * fx + 4.4156345f * (fx * fx));
		float k = fx * (fx * (fx * (fx * (fx * (fx + 7198.6997f) - 5837.7917f) - 476.47308f) + 614.95611f) + 213.87126f)
		          + 16.998792f;
		float g = k * (1.f / 17.f);
		g = (g - 1.f) * r + 1.f;
		g = g * (1.f + r);
		return {b0, 0.f, k * r, g};
	}

	// 12th order polynomial for the one-pole coefficient, and 8th order for the resonance scale
	float wc2 = wc * wc;
	float tmp = wc2 * -1.341281325101042e-02f + 8.168739417977708e-02f * wc + -2.365036766021623e-01f;
	tmp = wc2 * tmp + 4.439739664918068e-01f * wc + -6.297350825423579e-01f;
	tmp = wc2 * tmp + 7.529691648678890e-01f * wc + -8.249882473764324e-01f;
	tmp = wc2 * tmp + 8.736418933533319e-01f * wc + -9.164580250284832e-01f;
	tmp = wc2 * tmp + 9.583192455599817e-01f * wc + -9.999994950291231e-01f;
	float a1 = wc2 * tmp + 9.999999927726119e-01f * wc + -9.999999999857464e-01f;

	tmp = wc2 * -4.554677015609929e-05f + -2.022131730719448e-05f * wc + 2.784706718370008e-03f;
	tmp = wc2 * tmp + 2.079921151733780e-03f * wc + -8.333236384240325e-02f;
	tmp = wc2 * tmp + -1.666668203490468e-01f * wc + 1.000000012124230e+00f;
	tmp = wc2 * tmp + 3.999999999650040e+00f * wc + 4.000000000000113e+00f;
	return {1.f + a1, a1, r * tmp, 1.f};
}

void TeeBeeFilter::setDrive(float drive) {
	dirtBoost = std::pow(10.f, 1.2f * drive * std::sqrt(drive));
	updateDirt();
}

void TeeBeeFilter::updateDirt() {
	// At 0 a plain note sits in the soft part of the curve, so it stays close to CLASSIC until the resonance gets loud.
	// Up to 24 dB more input, held back at the bottom so the first half of the knob stays usable. Once the clipper
	// takes over, the resonance stops pulling the passband down and the level shoots up with the input (about 23 dB
	// over the knob, from half resonance up), so most of that comes back off, more the more resonance there is. It
	// gets 8 to 12 dB louder in all.
	dirtIn = 0.5f * dirtBoost;
	dirtOut = 4.f * std::pow(dirtBoost, -0.6f * std::sqrt(resonanceSkewed));
}

void TeeBeeFilter::reset() {
	feedbackHighpass.reset();
	y1 = y2 = y3 = y4 = 0.f;
}

// The voice

Open303::Open303() {
	constexpr float oversampledRate = kSampleRate * kOversampling;
	filter.setSampleRate(oversampledRate);
	highpass1.setup(OnePole::HIGHPASS, 44.486f, oversampledRate);
	highpass2.setup(OnePole::HIGHPASS, 24.167f, kSampleRate);
	allpass.setup(OnePole::ALLPASS, 14.008f, kSampleRate);
	notch.setupNotch(7.5164f, 4.7f, kSampleRate);
	ampDeClicker.setupLowpass(200.f, 0.70710678f, kSampleRate);
	rc2Coeff = leakyCoeff(15.f, kSampleRate);
	setControls(controls);
	setAccentState(false);
	reset();
}

void Open303::setControls(Controls const& newControls) {
	bool resonanceChanged = newControls.resonance != controls.resonance;
	// Open303 applies these at once, so the filter jumps to them rather than gliding. The cutoff knob counts too, as it
	// moves the envelope's scaling and offset.
	if (resonanceChanged || newControls.cutoff != controls.cutoff || newControls.envMod != controls.envMod
	    || newControls.filterMode != controls.filterMode) {
		filterJump = true;
	}
	controls = newControls;
	if (resonanceChanged || !sounding) {
		filter.setResonance(controls.resonance);
	}
	filter.setMode(controls.filterMode);
	filter.setDrive(controls.drive);
	pitchSlewCoeff = leakyCoeff(controls.slideTime, kSampleRate);
	ampDecayCoeff = envelopeCoeff(controls.ampDecay, kSampleRate);
	calculateEnvModScalerAndOffset();
}

void Open303::calculateEnvModScalerAndOffset() {
	// Open303's mapping, measured from a real 303
	constexpr float c0 = 3.138152786059267e+002f; // lowest nominal cutoff
	constexpr float c1 = 2.394411986817546e+003f; // highest nominal cutoff
	constexpr float oF = 0.048292930943553f;
	constexpr float oC = 0.294391201442418f;
	constexpr float sLoF = 3.773996325111173f;
	constexpr float sLoC = 0.736965594166206f;
	constexpr float sHiF = 4.194548788411135f;
	constexpr float sHiC = 0.864344900642434f;

	float e = 0.01f * controls.envMod;
	float c = std::log(controls.cutoff / c0) / std::log(c1 / c0);
	float sLo = sLoF * e + sLoC;
	float sHi = sHiF * e + sHiC;
	envScaler = (1.f - c) * sLo + c * sHi;
	envOffset = oF * c + oC;
}

float Open303::noteToFrequency(int32_t note) const {
	return 8.1757989156437073f * std::exp(0.057762265046662109f * ((float)note + pitchOffset));
}

void Open303::setAccentState(bool accent) {
	accentGain = accent ? 0.01f * controls.accent : 0.f;
	// Accented notes have a fixed 200 ms filter decay
	mainEnvCoeff = std::exp(-1.f / (0.001f * (accent ? 200.f : controls.decay) * kSampleRate));
	ampReleaseCoeff = envelopeCoeff(accent ? 50.f : 1.f, kSampleRate);
	// The accent's jump in envelope, or a new note's, lands at once, as on Open303
	filterJump = true;
}

void Open303::triggerNote(int32_t note, bool accent) {
	// Only start the filters afresh from silence, to avoid clicks. The oscillator runs free, as on a 303.
	if (!sounding) {
		filter.reset();
		highpass1.reset();
		highpass2.reset();
		allpass.reset();
		notch.reset();
		antiAlias.reset();
		ampDeClicker.reset();
	}
	setAccentState(accent);
	oscFreq = noteToFrequency(note);
	pitchSlew = oscFreq;
	mainEnv = 1.f / mainEnvCoeff;
	ampAttackPending = true;
	ampNoteOn = true;
	sounding = true;
}

void Open303::slideToNote(int32_t note, bool accent) {
	oscFreq = noteToFrequency(note);
	setAccentState(accent);
	sounding = true;
}

void Open303::releaseNote() {
	if (numNotes == 0) {
		ampNoteOn = false;
	}
	else {
		// Slide back to the latest note still held
		oscFreq = noteToFrequency(currentNote);
	}
}

void Open303::noteOn(int32_t note, int32_t velocity) {
	if (velocity <= 0) {
		noteOff(note);
		return;
	}
	bool accent = velocity >= 100;
	if (numNotes == 0) {
		triggerNote(note, accent);
	}
	else {
		slideToNote(note, accent);
	}
	currentNote = note;

	// Newest first. When full, the oldest falls off the end.
	numNotes = std::min(numNotes + 1, kMaxNotes);
	for (int32_t i = numNotes - 1; i > 0; i--) {
		noteList[i] = noteList[i - 1];
	}
	noteList[0] = (int8_t)note;
}

void Open303::noteOff(int32_t note) {
	int32_t kept = 0;
	for (int32_t i = 0; i < numNotes; i++) {
		if (noteList[i] != note) {
			noteList[kept++] = noteList[i];
		}
	}
	if (kept == numNotes) {
		return; // Wasn't playing
	}
	numNotes = kept;
	currentNote = numNotes ? noteList[0] : -1;
	releaseNote();
}

void Open303::allNotesOff() {
	numNotes = 0;
	currentNote = -1;
	ampNoteOn = false;
}

void Open303::reset() {
	allNotesOff();
	sounding = false;
	ampEnv = 0.f;
	lastAmp = 0.f;
	mainEnv = 0.f;
	rc2 = 0.f;
	phase = 0;
	filter.reset();
	highpass1.reset();
	highpass2.reset();
	allpass.reset();
	notch.reset();
	antiAlias.reset();
	ampDeClicker.reset();
}

template <FilterMode mode>
void Open303::renderVoice(std::span<int32_t> buffer, float gain) {
	// Glide the cutoff across the buffer so turning the knob doesn't zipper
	const int32_t size = (int32_t)buffer.size();
	const float cutoffStep = (controls.cutoff - cutoffNow) / (float)size;

	// Drive, the drum synth engines' curve. A plain note peaks near 1 and an accent near 3, so a quarter of that into
	// the clipper keeps plain notes clean at low settings and lets accents bite first. Half the drive gain is taken
	// back off, so it gets dirtier much faster than it gets louder. DIRT takes the drive into the filter instead.
	constexpr float kDriveIn = 0.25f;
	const bool driven = mode != FilterMode::DIRT && controls.drive > 0.f;
	const float driveGain = drum_synth::cyc::driveGainFor(controls.drive);
	const float driveIn = kDriveIn * driveGain;
	const float driveOut = 1.f / (kDriveIn * std::sqrt(driveGain));

	// Fully saw or fully square only needs the one wavetable
	const float blend = controls.waveform;
	const bool sawOnly = blend == 0.f;
	const bool squareOnly = blend == 1.f;
	const float sawLevel = 1.f - blend;
	const float squareLevel = 0.5f * blend;

	// The state the oversampled loop works on, in locals so it can stay in registers. It goes back at the end.
	uint32_t phaseNow = phase;
	uint32_t increment = phaseIncrement;
	OnePole inputHighpass = highpass1;
	TeeBeeFilter ladder = filter;
	AntiAliasFilter decimator = antiAlias;
	float amp = lastAmp;

	constexpr int32_t fracBits = 32 - wavetables::kLengthMagnitude;
	int32_t* sample = buffer.data();
	for (int32_t done = 0; done < size;) {
		const int32_t n = std::min(kControlInterval, size - done);

		// The cutoff, from the main envelope and, on accents, its smoothed copy, through the block. It moves slowly, so
		// the filter coefficients are worked out for the block's last sample only, and glide there from the last
		// block's.
		std::array<float, kControlInterval> envelope;
		float firstCutoff = 0.f;
		for (int32_t i = 0; i < n; i++) {
			cutoffNow += cutoffStep;
			mainEnv *= mainEnvCoeff;
			rc2 = ((accentGain > 0.f) ? mainEnv : 0.f) * (1.f - rc2Coeff) + rc2Coeff * rc2;
			envelope[i] = mainEnv;
			if (i == 0 && filterJump) {
				firstCutoff = cutoffNow * fastExp2(envScaler * (mainEnv - envOffset) + accentGain * rc2);
			}
		}
		done += n;
		const float lastCutoff = cutoffNow * fastExp2(envScaler * (mainEnv - envOffset) + accentGain * rc2);

		// Asked of filter rather than ladder, which has the same settings, so ladder never has its address taken
		TeeBeeFilter::Coefficients target = filter.coefficientsFor(lastCutoff);
		if (filterJump) {
			ladder.jumpTo(filter.coefficientsFor(firstCutoff), target, n);
			filterJump = false;
		}
		else {
			ladder.glideTo(target, n);
		}

		for (int32_t i = 0; i < n; i++) {
			// Oscillator pitch, through the slide
			pitchSlew = oscFreq + pitchSlewCoeff * (pitchSlew - oscFreq);
			if (pitchSlew > 0.f && pitchSlew < 20000.f) {
				increment = (uint32_t)(pitchSlew * (4294967296.f / (kSampleRate * kOversampling)));
			}

			// Amplitude
			if (ampAttackPending) {
				ampEnv = 1.f;
				ampAttackPending = false;
			}
			else if (ampNoteOn) {
				ampEnv += ampDecayCoeff * (controls.ampSustain - ampEnv);
			}
			else {
				ampEnv -= ampReleaseCoeff * ampEnv;
			}
			amp = ampEnv;
			if (ampNoteOn) {
				amp += (0.45f + 4.f * accentGain) * envelope[i];
			}
			amp = ampDeClicker.process(amp);

			// Like Open303: choose the table from the increment, two octaves of headroom
			int32_t octave = (31 - __builtin_clz(increment | 1)) - (32 - wavetables::kLengthMagnitude);
			int32_t table = std::clamp<int32_t>(octave + 2, 0, wavetables::kNumTables - 1);
			const float* saw = wavetables::tables[wavetables::SAW][table];
			const float* square = wavetables::tables[wavetables::SQUARE][table];

			ladder.step();
			float out = 0.f;
			for (int32_t j = 0; j < kOversampling; j++) {
				int32_t index = phaseNow >> fracBits;
				float frac = (float)(phaseNow & ((1u << fracBits) - 1)) * (1.f / (1u << fracBits));
				phaseNow += increment;
				if (sawOnly) {
					out = readTable(saw, index, frac);
				}
				else if (squareOnly) {
					out = 0.5f * readTable(square, index, frac);
				}
				else {
					out = sawLevel * readTable(saw, index, frac) + squareLevel * readTable(square, index, frac);
				}
				out = inputHighpass.process(-out);
				out = ladder.process<mode>(out);
				out = decimator.process(out);
			}
			out = allpass.process(out);
			out = highpass2.process(out);
			out = notch.process(out);
			out *= amp;

			if (driven) {
				out = drum_synth::cyc::driveCurve(out * driveIn) * driveOut;
			}
			out *= gain;
			*sample++ += (int32_t)std::clamp(out, -2147483520.f, 2147483520.f);
		}
	}

	phase = phaseNow;
	phaseIncrement = increment;
	highpass1 = inputHighpass;
	filter = ladder;
	antiAlias = decimator;
	lastAmp = amp;
	cutoffNow = controls.cutoff;
}

void Open303::render(std::span<int32_t> buffer, float gain) {
	if (!sounding || !wavetables::built()) {
		return;
	}

	switch (filter.mode()) {
	case FilterMode::CLASSIC:
		renderVoice<FilterMode::CLASSIC>(buffer, gain);
		break;
	case FilterMode::LP18:
		renderVoice<FilterMode::LP18>(buffer, gain);
		break;
	case FilterMode::DIRT:
		renderVoice<FilterMode::DIRT>(buffer, gain);
		break;
	}

	// Gone quiet, so stop rendering until the next note
	if (!ampNoteOn && ampEnv < 1e-5f && std::abs(lastAmp) < 1e-5f) {
		sounding = false;
	}
}

} // namespace deluge::dsp::tb303
