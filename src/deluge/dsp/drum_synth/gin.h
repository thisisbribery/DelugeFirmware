/*
 * GIN: the drum synth from SquareWave Studio's GIN for the OP-1, on the Deluge's drum synth. GIN's synthesis core is
 * built on Eric Lewis's libpo32, a PO-32 style drum voice: one oscillator with a pitch modulator, and a filtered noise
 * layer with its own envelope, mixed, driven and EQ'd.
 *
 * Deluge port Copyright © 2026 thisisbribery and the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Derived from MIT licensed code, whose notices are kept here as that licence asks:
 *
 *   libpo32: Copyright (c) 2026 Eric Lewis
 *   GIN synthesis core: Copyright (c) 2026 SquareWave Studio
 *
 *   Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 *   documentation files (the "Software"), to deal in the Software without restriction, including without limitation
 *   the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 *   permit persons to whom the Software is furnished to do so, subject to the following conditions:
 *
 *   The above copyright notice and this permission notice shall be included in all copies or substantial portions of
 *   the Software.
 *
 *   THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO
 *   THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *   AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 *   TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *   SOFTWARE.
 *
 * Not affiliated with, authorised by or endorsed by SquareWave Studio, Teenage Engineering or Sonic Charge. No
 * factory kit data from GIN is used: the Deluge's GIN kits (gin_kits.cpp) are our own.
 *
 * What changed for the Deluge: the voice renders in float from libpo32's float renderer (GIN's own fixed point path is
 * for the OP-1's Blackfin), at the note's pitch rather than a pitch parameter, at full velocity (the Deluge's velocity
 * cable does dynamics), and it follows its settings while it plays
 * rather than freezing them at the hit, so knobs, automation and modulation move a sounding note. It ends once both
 * envelopes are 80 dB down, rather than after a fixed length.
 */

#pragma once

#include <cstdint>

namespace deluge::dsp::drum_synth::gin {

enum class Wave : uint8_t { SINE, TRIANGLE, SAW, NUM_WAVES };
/// What bends the oscillator's pitch: a decaying sweep, a sine LFO, or filtered noise
enum class ModMode : uint8_t { DECAY, SINE, NOISE, NUM_MODES };
/// The noise filter, in the order the Deluge's drum synth noise filter menu lists them
enum class NoiseFilter : uint8_t { LOW_PASS, HIGH_PASS, BAND_PASS, OFF, NUM_FILTERS };
/// The noise envelope: exponential, linear, or modulated (a decaying train of bursts, for claps)
enum class NoiseEnv : uint8_t { EXP, LINEAR, MOD, NUM_ENVS };

/// Every setting, as GIN's own controls: 0 to 1 unless noted
struct Settings {
	Wave wave;
	ModMode modMode;
	NoiseFilter noiseFilter;
	NoiseEnv noiseEnv;
	float toneAttack;
	float toneDecay; ///< 10 ms to 10 s to fall 60 dB
	float modRate;
	float modAmount; ///< -1 to 1: up to 8 octaves either way for the sweep, 4 for the LFO and noise
	float noiseFreq; ///< 20 Hz to 20 kHz
	float noiseQ;
	float noiseAttack;
	float noiseDecay;
	float burstSpacing; ///< The modulated noise envelope's burst spacing: GIN ties it to the noise decay setting,
	                    ///< before the Deluge's Decay knob moves that
	float mix;          ///< 0 = tone only, 0.5 = both full, 1 = noise only
	float drive;        ///< GIN's distortion
	float eqFreq;       ///< Peak EQ, 20 Hz to 20 kHz
	float eqGain;       ///< -40 to +40 dB, 0.5 = flat
	float level;        ///< Output gain before the clipper, linear: see levelGain()

	bool operator==(const Settings&) const = default;
};

/// GIN's Level control, 0 to 1, as a gain: 0.8 is about -2 dB, 1 is +10 dB
float levelGain(float level);
/// The Level setting that gives a gain in dB
float levelForDb(float db);

/// One GIN voice. Allocation free, and trivially constructible so it can share memory with the other engines' state:
/// call reset() before first use.
class Voice {
public:
	void reset();
	/// Start a hit on the next render
	void trigger() { pendingTrigger_ = true; }

	/// Render numSamples into out, replacing what is there, as q31, clipped at full scale. phaseIncrement is the note's
	/// pitch, 2^32 = one cycle per sample. The sample rate is the Deluge's 44.1 kHz.
	void render(const Settings& settings, uint32_t phaseIncrement, int32_t* out, int32_t numSamples);

	/// True once both envelopes have died away, so the Deluge voice can be freed
	[[nodiscard]] bool finished() const { return finished_; }
	/// True once every layer is 40 dB down
	[[nodiscard]] bool quiet() const;

	struct Biquad {
		float b0, b1, b2, a1, a2;
	};

private:
	void prepare(const Settings& s);
	void start();

	// What the settings make, worked out again only when they change
	Settings preparedFor_;
	bool prepared_;
	float oscAtk_, oscDcy_, nAtk_, nDcy_; ///< Seconds
	float oscAtkCoef_, oscDcyCoef_, nAtkCoef_, nDcyCoef_;
	float linAtk_, linDcy_, invLinAtk_, invLinDcy_;
	float modPitchScale_; ///< Octaves at full modulation
	float modDcyCoef_, rotS_, rotC_, modAlpha_;
	float ringPeriod_, invRingPeriod_, ringGain_;
	float oscGain_, noiseGain_, level_;
	float distDrive_, distGain_, distClip_, distBias_;
	bool useEq_;
	bool filterNoise_; ///< The noise filter is on (off, its biquad passes the noise straight through)
	float endTime_;    ///< Seconds after the hit when both envelopes are 72 dB down
	float quietTime_;  ///< And 40 dB down
	Biquad noiseFilter_, eq_;

	// Running state
	uint32_t n_; ///< Samples since the hit
	uint32_t phase_;
	uint32_t rand_;
	float oscEnv_, nEnv_, modEnv_, modS_, modC_, modState_;
	bool oscSynced_, nSynced_;
	float nz1_, nz2_, ez1_, ez2_;
	bool pendingTrigger_;
	bool finished_;
};

} // namespace deluge::dsp::drum_synth::gin
