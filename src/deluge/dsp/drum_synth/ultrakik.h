/*
 * ULTRAKIK: a kick drum engine for the Deluge's drum synth.
 *
 * Copyright © 2026 thisisbribery.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * A new engine, written from general kick synthesis principles (and what Plaits' bass drums, the 808 and 909 circuits,
 * Noise Engineering's BIA and Ohm Force's Bohm do with theirs): one oscillator swept down from above by a fast pitch
 * spike (the punch) and a slower sweep, with a slow bend over the hit for 808 slides, starting at a set phase (the
 * snap). Then, a stage a pass over each chunk: amp hold and decay, a sine wavefolder, the drive (warm, hard, tube or
 * fuzz), grit riding on the driven peaks, the click, a crusher, a DC blocker, and a clean sine of the body mixed under
 * it all so the low end survives anything above it.
 *
 * Starts as a thick, low thump with drive and grit already on and a short tail, and bends into long gliding 808s, hard
 * 909s and fully broken kicks.
 */

#pragma once

#include <array>
#include <cstdint>

namespace deluge::dsp::drum_synth::ultrakik {

/// The body oscillator's wave, before folding and drive
enum class Wave : uint8_t {
	SINE,  ///< Clean sine: 808s, subs
	ROUND, ///< A triangle rounded towards a sine, as the 909's VCO is shaped: a sine with a knock in it
	TRI,   ///< Triangle: hollow, woody, more knock
	SAW,   ///< Saw: buzzy, hardstyle and gabber bodies
	NUM_WAVES,
};

/// What the drive stage does
enum class DriveType : uint8_t {
	WARM, ///< Soft clip: rounds the body towards a square, adds odd harmonics
	HARD, ///< Hard clip: a square-ish body at high drive, the loudest
	TUBE, ///< Asymmetric soft clip: even harmonics, thicker and warmer
	FUZZ, ///< Full wave fuzz: the body's octave up mixed into a hard clip, ragged and broken
	NUM_TYPES,
};

/// The transient at the start of each hit
enum class Click : uint8_t {
	THUD,  ///< A low passed noise thump, the beater hitting the skin
	TICK,  ///< A short pulse, the 808 / 909 attack
	NOISE, ///< A bright noise burst
	ZAP,   ///< A very fast falling sine, the modern laser click
	NUM_CLICKS,
};

/// Continuous settings, 0 to 1 (knob / 127). Bend is -1 to 1 around its centre.
struct Settings {
	float sweep = 0.f;     ///< Pitch envelope depth: none to 5 octaves above the note
	float sweepTime = 0.f; ///< Pitch envelope time: 2 ms to 250 ms
	float punch = 0.f;     ///< A very fast pitch spike on top of the sweep: none to 4 octaves, over about 3 ms
	float fold = 0.f;      ///< Wavefolder: 0 clean, a little rounds the peaks, then folding into bright harmonics
	float grit = 0.f;      ///< Noise riding on the body's peaks, the gritty rattle of a driven speaker
	float drive = 0.f;     ///< Gain into the drive stage, 0 to 36 dB
	float click = 0.f;     ///< Transient level
	float clickTone = 0.f; ///< Transient colour: dark and long to bright and short
	float bend = 0.f;      ///< Slow pitch bend over the hit, -1 to 1: 24 semitones down or up
	float bendTime = 0.f;  ///< How long the bend takes: 40 ms to 4 s
	float decay = 0.f;     ///< Amp decay: 40 ms to 8 s to fall 60 dB. The top holds, for gated 808s
	float hold = 0.f;      ///< Amp hold at full level before the decay starts: 0 to 200 ms
	float sub = 0.f;       ///< Clean sine of the body, mixed in after the drive to keep the low end
	float crush = 0.f;     ///< Sample rate and bit depth reduction after the drive
	float snap = 0.f;      ///< Start phase, 0 to 90 degrees: the further round, the harder the first edge hits
	Wave wave = Wave::SINE;
	DriveType driveType = DriveType::WARM;
	Click clickType = Click::THUD;

	bool operator==(const Settings&) const = default;
};

/// How long the amp decay takes to fall 60 dB at a decay setting (0 to 1), in seconds. 1000 s where it holds.
float ampDecaySeconds(float decay);
/// The hold before it, in seconds
float holdSeconds(float hold);

/// One voice's state between renders. Trivially constructible, so it can share memory with the other engines' state:
/// call reset() before first use.
class Voice {
public:
	void reset();
	/// Start a note on the next render
	void trigger() { pendingTrigger_ = true; }

	/// Render numSamples into out, replacing what is there, q31, peaking a little under full scale at the defaults.
	/// phaseIncrement is the note's pitch, 2^32 = one cycle per sample.
	void render(const Settings& settings, uint32_t phaseIncrement, float sampleRate, int32_t* out, int32_t numSamples);

	[[nodiscard]] bool finished() const { return finished_; }
	/// The amp envelope, 0 to 1. The voice ends once this falls below -72 dB.
	[[nodiscard]] float level() const { return finished_ ? 0.f : level_; }

private:
	/// What the settings make, worked out again only when they or the note change
	struct Prepared {
		Settings settings;
		uint32_t phaseIncrement;
		float sampleRate;

		float incPerHz, maxInc;
		float baseInc;   ///< The note's phase increment, as a float
		float depth;     ///< Pitch envelope: the start's ratio over the note, minus 1
		float pitchCoef; ///< Per sample pitch envelope multiplier
		float punchDepth, punchCoef;
		uint32_t startPhase;
		float bendSemis;    ///< The bend's full depth, semitones
		float bendPerChunk; ///< How far through the bend a chunk moves, 0 to 1
		float ampCoef;      ///< Per sample amp decay multiplier
		uint32_t holdSamples;
		float foldGain; ///< Into the folder, in quarter cycles of the sine it reads
		float foldMix;
		float grit;
		float driveGain, driveNorm;
		float tubeBias, tubeOffset;
		float sub, bodyMix;
		float clickLevel, clickCoef, clickLp, clickHp;
		float zapDepth, zapCoef;
		float crushInc; ///< Crusher's sample and hold rate, 2^32 = every sample
		float crushStep, crushInvStep;
		bool crushing;
		float outGain; ///< Overall level, so every setting peaks near full scale
		float dcCoef;
	};

	void start(const Settings& settings, uint32_t phaseIncrement, float sampleRate);
	void prepare(const Settings& settings, uint32_t phaseIncrement, float sampleRate);

	Prepared p_;
	uint32_t phase_;
	uint32_t crushPhase_;
	uint32_t rng_;      ///< Grit's noise
	uint32_t clickRng_; ///< The click's, so the passes draw the same noise however a chunk is split
	uint32_t pos_;      ///< Samples since the note started
	float pitchEnv_;
	float punchEnv_;
	float bendPos_;  ///< Through the bend, 0 to 1
	float bendMul_;  ///< The bend's pitch multiplier, now
	float bendStep_; ///< How much it moves each sample of this chunk
	float amp_;
	uint32_t holdLeft_;
	float clickEnv_;
	float zapEnv_;
	uint32_t zapPhase_;
	float clickLpState_, clickHpState_;
	float gritLp_;
	float dcIn_, dcOut_;
	float crushHeld_;
	float level_;
	bool ending_;
	bool sweeping_, clicking_, gritty_; ///< As of the start of this chunk
	bool started_;
	bool pendingTrigger_;
	bool finished_;
	bool prepared_;
};

} // namespace deluge::dsp::drum_synth::ultrakik
