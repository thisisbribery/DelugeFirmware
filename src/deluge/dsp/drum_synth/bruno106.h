/*
 * BRUNO106: the DCO voice of Ultramaster KR-106 as a Deluge synth engine.
 *
 * Portions Copyright (C) 2026 Karl LaRocca (Ultramaster KR-106, kayrockscreenprinting, GPL-3.0).
 * Deluge integration Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This is a modified version: one voice's PolyBLEP DCO (saw, pulse with its width, the flip-flop sub) and noise, with
 * KR-106's Juno-106 levels and curves, and the Deluge's own envelopes, LFOs, glide, unison and filters around it (the
 * IR3109 filter is dsp/filter/bruno106.h, in the Deluge's filter list). Not affiliated with or endorsed by Roland.
 */

#pragma once

#include <cstdint>

namespace deluge::dsp::drum_synth::bruno106 {

/// Everything a render needs, worked out from the patch and controls once per buffer
struct Settings {
	bool sawOn;
	bool pulseOn;
	float pw;    ///< The width knob, 0 (square) to 1 (narrowest)
	float sub;   ///< Sub level, the slider's curve and the sub's mix level in
	float noise; ///< Noise level, likewise
};

/// One voice's running state. All zeros, then trigger(), is a valid start.
struct Voice {
	uint32_t phase;           ///< A cycle is 2^32, as the Deluge's oscillators
	uint32_t subLow;          ///< The sub's flip-flop: 0 high, 1 low
	float sawGain, pulseGain; ///< The switches fade in and out over 1.5 ms
	float pitchMul;           ///< This note's tuning, from Slop
	float pwLo, pwHi;         ///< This note's pulse width range, about 50% to 95%, from Slop
	float level;              ///< This note's level, from Slop
	uint32_t pwPhase;         ///< Where the pulse goes from low to high, in phase; where the last buffer ended
	float sub, noise;         ///< Where the last buffer ended, to ramp from
	float dc;                 ///< The DC blocker after the oscillator (1.6 Hz), as KR-106
	float noiseLp, noiseHp;   ///< The noise's band limiting
	uint32_t seed;
	bool started; ///< The ramps have somewhere to start from
};

/// A new note: slop (0 to 1) spreads its tuning, pulse width range and level, as the parts of a real one differ.
/// seed varies per note.
void trigger(Voice& v, float slop, uint32_t seed);

/// Render n samples into out (overwriting), a full saw being 1. baseIncrement is the Deluge's pitch, a cycle being
/// 2^32.
void render(Voice& v, const Settings& s, uint32_t baseIncrement, float* out, int32_t n);

/// KR-106's Juno-106 sub and noise sliders (0 to 1) to their levels in the mix
float subLevel(float slider);
float noiseLevel(float slider);

} // namespace deluge::dsp::drum_synth::bruno106
