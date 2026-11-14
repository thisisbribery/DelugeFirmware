/*
 * BROPHET: the voice of Pro-54, Cmajor's port of Native Instruments' Pro-53, as a Deluge synth engine.
 *
 * Portions Copyright (C) 2024 Cmajor Software Ltd (Pro-54, GPL-3.0), originally Native Instruments' Pro-53.
 * Deluge integration Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This is a modified version: one voice's oscillators, poly mod and mixer, with the Deluge's own envelopes, LFOs,
 * glide, unison and filters around it (the filter itself is dsp/filter/brophet.h, in the Deluge's filter list). Not
 * affiliated with or endorsed by Native Instruments, Cmajor Software or Sequential.
 */

#pragma once

#include <cstdint>

namespace deluge::dsp::drum_synth::brophet {

/// Osc A's waves
enum class WaveA : uint8_t { SAW, PULSE, BOTH, NUM };
/// Osc B's waves, as its three buttons combine
enum class WaveB : uint8_t { SAW, TRIANGLE, PULSE, SAW_TRIANGLE, SAW_PULSE, TRIANGLE_PULSE, ALL, NUM };
/// Where poly mod goes: osc A's frequency, its pulse width, or both (the filter is the Deluge's, reached by cables)
enum class PolyModDest : uint8_t { FREQ_A, PW_A, BOTH, NUM };

/// Everything a render needs, worked out from the patch and controls once per buffer. Names follow Pro-54's.
struct Settings {
	float mixerOscillatorA; ///< 0 to 1
	float sawActiveA;       ///< 0 or 1
	bool oscillatorA_PulseActive;
	float oscillatorA_FM_PM; ///< 0.4 with poly mod to osc A's frequency
	float oscillatorA_PW_PM; ///< -1 with poly mod to osc A's pulse width
	float oscillatorB_SawA;  ///< 0.5 when on
	bool oscillatorB_TriA;
	float oscillatorB_PulseA;  ///< 1 when on
	bool oscillatorSyncActive; ///< Osc A synced to osc B
	float mixerOscillatorB;    ///< 0 to 2
	float mixerNoise;          ///< 0 to 1.75
	float polymodOscillatorB;  ///< 0 to 3.09
	float polymodEnvelopeF;    ///< 0 to 26
	float pwA, pwB;            ///< -1 to 1
	float semitonesB;          ///< Osc B against osc A (or middle C when not on the keys), its low range included
	bool keysB;                ///< Osc B follows the keys
};

/// One voice's running state. All zeros, then trigger(), is a valid start.
struct Voice {
	float oscillatorA_Phase, oscillatorB_Phase; ///< -1 to 1: a cycle is 2
	float oscillatorA_Saw_AA2, oscillatorA_Pulse_AA2;
	float oscillatorB_Saw_AA2, oscillatorB_Pulse_AA2;
	float oscillatorA_F, oscillatorB_F; ///< Phase per sample, before poly mod
	float eqLast;
	float driftA, driftB;             ///< This note's analog detune, semitones
	float pmEnvRnd;                   ///< This note's poly mod envelope depth, about 1
	float mixA, mixB, pwA, pwB, sawA; ///< Where the last buffer ended, to ramp from
	uint32_t noise;
	bool started; ///< The ramps have somewhere to start from
};

/// A new note: fresh drift (analog, 0 to 1, as Pro-53's knob), random phases. seed varies per note.
void trigger(Voice& v, float analog, uint32_t seed);

/// Render n samples into out (overwriting), in Pro-54's units (a full saw is 1). baseIncrement is the Deluge's pitch
/// for osc A, a cycle being 2^32. env is the poly mod envelope (the Deluge's envelope 2), 0 to 1.
void render(Voice& v, const Settings& s, uint32_t baseIncrement, float env, float* out, int32_t n);

/// The DC the mix carries at these settings: osc A's pulse, and osc B's waves (which sit above zero for poly mod)
float mixDc(const Settings& s);

} // namespace deluge::dsp::drum_synth::brophet
