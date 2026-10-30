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

/// How long a Cyc engine's amp decay takes to fall 60 dB, in seconds, at a decay knob of 0 to 127. Punch stretches it
/// by a quarter. Very long (1000 s) where the knob holds.
float ampDecaySeconds(Kind kind, float knob, bool punch);

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
	/// The voice's overall level, 0 to 1: the amp envelope (and, on Vtg Snr, the snares' own), through the fade at the
	/// end. The voice ends once this falls below -72 dB.
	[[nodiscard]] float level() const { return finished_ ? 0.f : common_.level; }

	// Per-engine running state. Public for the engine functions in cyc.cpp; nothing else should touch them.
	struct Common {
		float amp;        ///< Amp envelope
		float ampCoef;    ///< Per sample amp decay multiplier
		float punchEnv;   ///< Punch's short boost
		float level;      ///< What level() reports, as of the end of the last render
		uint32_t rng;     ///< Noise generator state
		uint32_t ampHold; ///< Samples before the amp decay starts
		uint32_t pos;     ///< Samples since the note started. Work is done in chunks of 64 counted from here
		bool ending;      ///< Fading out over the current chunk, then finished
	};
	struct Osc {
		uint32_t phase;
		float last; ///< Last output, for feedback
	};
	struct Svf {
		float ic1, ic2;
	};
	struct SvfCoefs {
		float a1, a2, a3, k;
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
		float sqrtAmp, invAmp; ///< sqrt(amp) and 1 / amp, followed sample by sample from the start of each chunk
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

	// What each engine's settings make, worked out again only when the settings or the note change. "rest" phase
	// increments are the ones once the pitch sweep has died away.
	struct KikCoefs {
		float fEnd, depth, pitchCoef, incPerHz, maxHz;
		float spikeAmount, spikeNorm, spikeNoise, drive, driveNorm, dry, knock, clickCoef;
		uint32_t restInc;
		bool spikes;  ///< Shape adds spikes (or noise on them) above -110 dB
		bool colored; ///< Color is up at all
	};
	struct SnrCoefs {
		float depth, pitchCoef, incPerHz, maxHz, index, noiseAmount, rattleCoef, bodyCoef, attackStep;
		uint32_t modInc, carInc, restInc;
		SvfCoefs hp;
	};
	struct MetalCoefs {
		std::array<float, 6> inc;
		std::array<uint32_t, 6> restInc;
		float index, depth, pitchCoef, brightCoef, cutLo, cutHi;
		SvfCoefs hp, ring;
	};
	struct PercCoefs {
		float depth, pitchCoef, incPerHz, maxHz, index, levelB, ratioC, levelC, overtoneCoef;
		std::array<uint32_t, 4> restInc; ///< car, mod, mod2, sub
	};
	struct ToneCoefs {
		uint32_t carInc, modInc;
		float index, indexCoef, burst, burstCoef;
	};
	struct ChordCoefs {
		std::array<uint32_t, 4> inc, inc2;
		float odd, fb, sweepCoef;
	};
	struct VsnrCoefs {
		float depth, pitchCoef, incPerHz, maxHz, ratio, modDepth, modCoef, clusterCoef, clusterLevel, roughness;
		float sqrtAmpCoef, invAmpCoef, ampCoef64;
		std::array<uint32_t, 4> modeInc;
		uint32_t restInc, restModInc;
		SvfCoefs bp;
	};
	struct ClapCoefs {
		int32_t spacing, holdSamples;
		float burstCoef, bodyGain, darkCoef, burstLevel, tailLevel, burstBright, tailFastCoef;
		SvfCoefs bp, bp2, body;
	};
	struct Prepared {
		// What these were made from
		std::array<float, kNumMacros> macro;
		float decay, drive, sampleRate;
		uint32_t phaseIncrement;
		Kind kind;
		bool punch;
		// For every engine
		float hz; ///< The note's frequency
		float ampCoef, driveGain, punchCoef;
		union {
			KikCoefs kik;
			SnrCoefs snr;
			MetalCoefs metal;
			PercCoefs perc;
			ToneCoefs tone;
			ChordCoefs chord;
			VsnrCoefs vsnr;
			ClapCoefs clap;
		} e;
	};
	Prepared p_;

private:
	void start(Kind kind, const Settings& settings, uint32_t phaseIncrement, float sampleRate);
	void prepare(Kind kind, const Settings& settings, uint32_t phaseIncrement, float sampleRate);
	[[nodiscard]] bool preparedFor(Kind kind, const Settings& settings, uint32_t phaseIncrement,
	                               float sampleRate) const;

	Kind kind_; ///< The engine s_ was started for
	bool pendingTrigger_;
	bool finished_;
	bool prepared_; ///< p_ holds something
};

} // namespace deluge::dsp::drum_synth::cyc
