/*
 * Cyc engines: eight drum and tone engines for the Deluge's drum synth, named after the Model:Cycles machines and the
 * Syntakt machines that Modded-Cycles moved into it, which inspired them.
 *
 * Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * These are new implementations. They were written from general synthesis principles and tuned by ear and by
 * measurement against renders of the original machines (Elektron's KICK, SNARE, METAL, PERC, TONE and CHORD, and the
 * Syntakt's SD VINTAGE and CP VINTAGE), made in an emulator. No Elektron code or table is in them, and they are not a
 * recovery of the original algorithms: the knob names follow the originals, the sound inside each knob is our own.
 */

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace deluge::dsp::drum_synth::cyc {

enum class Kind : uint8_t {
	KIK,   ///< Cyc Kik: pitched kick with a swept body
	SNR,   ///< Cyc Snr: tuned body plus a rattle of dense partials and noise
	METAL, ///< Cyc Metal: inharmonic phase modulation network, hats and cymbals
	PERC,  ///< Cyc Perc: small FM percussion, toms to woodblocks
	TONE,  ///< Cyc Tone: two-operator FM tone
	CHORD, ///< Cyc Chord: four-voice chord from one note
	VSNR,  ///< Vtg Snr: vintage-style snare, body plus a high inharmonic cluster
	CLAP,  ///< clap eng: a train of filtered noise bursts and a tail
	NUM_KINDS,
};

inline constexpr int32_t kNumMacros = 4;

/// Number of chord shapes Cyc Chord's Shape knob steps through
inline constexpr int32_t kNumChordShapes = 38;

/// The drive every drum synth engine without its own shares: knob 0 to 1 to a gain of 0 to 15 dB, steepening towards
/// the top, into driveCurve(). Louder as it goes up, pushing into hard clipping at the end.
inline float driveGainFor(float knob) {
	return std::pow(10.f, 0.75f * knob * knob);
}
/// Soft clip with unity slope at zero, reaching +-1 at +-3 and flat beyond
inline float driveCurve(float x) {
	x = std::clamp(x, -3.f, 3.f);
	return x * (27.f + x * x) / (27.f + 9.f * x * x);
}

struct Settings {
	std::array<float, kNumMacros> macro{}; ///< 0 to 1, knob value / 127
	float decay = 1.f;                     ///< Amp decay knob, 0 to 1. 1 holds
	float drive = 0.f;                     ///< Drive knob, 0 to 1
	bool punch = false;                    ///< A harder, shorter attack
};

/// Everything a voice of any Cyc engine needs between renders. Allocation free, and trivially constructible so it can
/// share memory with the XIA engines' state: call reset() before first use.
class Voice {
public:
	/// Clear everything, as when the voice is (re)assigned
	void reset();
	/// Start a note on the next render
	void trigger() { pendingTrigger_ = true; }

	/// Render numSamples of the engine into out, replacing what is there. Samples are q31 at the level the Model:Cycles
	/// hands its mixer. phaseIncrement is the note's pitch, 2^32 = one cycle per sample.
	void render(Kind kind, const Settings& settings, uint32_t phaseIncrement, float sampleRate, int32_t* out,
	            int32_t numSamples);

	/// True once the amp envelope has died away, so the Deluge voice can be freed
	[[nodiscard]] bool finished() const { return finished_; }

	// Per-engine running state. Public for the engine functions in cyc.cpp; nothing else should touch them.
	struct Common {
		float amp;        ///< Amp envelope
		float ampCoef;    ///< Per sample amp decay multiplier
		float punchEnv;   ///< Punch's short boost
		uint32_t rng;     ///< Noise generator state
		uint32_t ampHold; ///< Samples before the amp decay starts
	};
	struct Osc {
		uint32_t phase;
		float last; ///< Last output, for feedback
	};
	struct Svf {
		float ic1, ic2;
	};
	struct KikState {
		Osc body, harm;
		float pitchEnv, clickEnv, fold;
		Svf lp;
	};
	struct SnrState {
		Osc body, mod, rattle[3];
		float pitchEnv, rattleEnv, noiseEnv, bodyEnv, attack;
		Svf noiseBp, outHp;
	};
	struct MetalState {
		std::array<Osc, 6> osc;
		float pitchEnv, brightEnv, lp;
		Svf hp, ring;
	};
	struct PercState {
		Osc car, mod, mod2, sub;
		float pitchEnv, indexEnv, clickEnv;
		Svf hp;
	};
	struct ToneState {
		Osc car, mod;
		float pitchEnv, indexEnv;
		Svf lp;
	};
	struct ChordState {
		std::array<Osc, 4> osc;
		std::array<Osc, 4> mod;
		float sweepEnv;
		Svf lp;
	};
	struct VsnrState {
		Osc body, bodyMod;
		std::array<Osc, 4> cluster;
		float pitchEnv, modEnv, clusterEnv;
		Svf noiseBp;
	};
	struct ClapState {
		float burstEnv, tailEnv, bodyEnv;
		int32_t burstIndex, samplesToBurst;
		Svf bp1, bp2, hp;
		Osc body; ///< phase: samples left on the held noise value, last: that value
	};

	Common common_;
	union {
		KikState kik;
		SnrState snr;
		MetalState metal;
		PercState perc;
		ToneState tone;
		ChordState chord;
		VsnrState vsnr;
		ClapState clap;
	} s_;

private:
	void start(Kind kind, const Settings& settings, uint32_t phaseIncrement, float sampleRate);

	Kind kind_; ///< The engine s_ was started for
	bool pendingTrigger_;
	bool finished_;
};

} // namespace deluge::dsp::drum_synth::cyc
