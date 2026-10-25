/*
 * The TIA and IIA engines, shown on the Deluge as DXIA and DIIA. They come from the XIA engine set for the
 * Model:Cycles, whose author released them as open source; they are distributed here under the GPL-3.0 with the
 * rest of this firmware.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Ported from the compiled Model:Cycles firmware they were written for (model-cycles_OS1.13_XIA_2lfo.syx), not from
 * source. The render code reproduces the original output sample for sample at its own block size and settings, and
 * is checked against an emulation of that firmware (see tests/unit/xia_tests.cpp).
 *
 */

#pragma once

#include <array>
#include <cstdint>

namespace deluge::dsp::drum_synth::xia {

/// Both engines render in blocks of this many samples, with their settings read once per block
constexpr int32_t kBlockSize = 32;

/// Settings arrive in the formats the Model:Cycles hands its machines:
/// - knobs: 8.8 fixed point, 0 to 0x7F00 for knob values 0 to 127
/// - pitch: semitones in 16.16, 0 to 127 semitones, where 48 semitones plays MIDI note 48 (130.8 Hz at 48 kHz)

/// TIA: three sine oscillators and a fourth carrier, with three routings (the three Cycles variants)
struct TiaSettings {
	int32_t
	    variant; ///< 0: all three phase modulate the carrier. 1: a chain 1 > 2 > 3 > carrier. 2: a loop 1 > 2 > 3 > 1
	int32_t freq[3];    ///< Freq 1-3 (knobs), centre 0x4000 = unison, -24 to +24 semitones
	int32_t blendex[3]; ///< 0 to 0x7FFF: how much each oscillator is heard (0) or modulates the next (0x7FFF)
	int32_t lowPass;    ///< LPF (knob). At 0x7C00 and above the filter is off
	int32_t drive;      ///< Wavefold and saturation (knob)
	int32_t decay;      ///< Amp decay (knob). 0x7F00 holds forever
	int32_t pitch;      ///< 16.16 semitones
	int32_t punch;      ///< Non-zero: punchier decay and a soft clipper on the output
	int32_t gate;       ///< Non-zero: hold the envelope until release
	int32_t release;    ///< Non-zero once the note is released
};

/// TIA's running state. The layout follows the original's, which lets the tests compare it word for word.
struct TiaState {
	uint32_t phase[4]; ///< Oscillators 1-3, then the carrier
	int32_t feedback;  ///< Last output of oscillator 3, for variant 2
	int32_t envelope;  ///< Amp envelope, q31
	int32_t lowPass[4];
	int32_t declick; ///< Gain that fades a voice out over one block before it is retriggered
	int32_t active;
	int32_t held;
};

/// IIA: inharmonic partials (variants G and I add six of them, H chains eight phase modulated sines), a noise strike
/// on the attack, and variant I's falling pitch
struct IiaSettings {
	int32_t variant; ///< 0 = G, 1 = I, 2 = H
	int32_t weight;  ///< Weight (knob): picks the partial ratios
	int32_t alloy;   ///< Alloy (knob): partial levels (G, I) or modulation depth (H), and the strike's colour
	int32_t lowPass; ///< LPF (knob)
	int32_t drive;   ///< Wavefold and saturation (knob)
	int32_t shape;   ///< Shape (knob): partial waveform, sine to triangle to square to saw
	int32_t decay;   ///< Amp decay (knob)
	int32_t attack;  ///< Attack (knob). Below 0x4000 it sets the strike level instead
	int32_t pitch;   ///< 16.16 semitones
	int32_t punch;
	int32_t gate;
	int32_t release;
};

struct IiaState {
	uint32_t phase[8];  ///< Carrier, partials 1-6, then variant H's modulator
	int32_t envelope;   ///< Amp decay, q31
	int32_t attack;     ///< Attack ramp
	int32_t strike;     ///< Noise strike level
	int32_t pitchDrop;  ///< Variant I's pitch envelope
	uint32_t noiseSeed; ///< Strike noise generator
	int32_t lowPass[4];
	int32_t declick;
	int32_t active;
	int32_t held;
};

/// Highest pitch the engines accept, 127 semitones in 16.16
constexpr int32_t kMaxPitch = 0x7F0000;

/// The 32-bit phase increment the engines use at 48 kHz for a pitch in 16.16 semitones
uint32_t phaseIncrement(int32_t pitch);

/// Clear a voice, as the Cycles does when a track switches to the engine
void reset(TiaState& state);
void reset(IiaState& state);

/// Render one block. trigger starts a note. fadeOut ramps the block to silence, which the Cycles does to a sounding
/// voice the block before it retriggers it.
void render(TiaState& state, const TiaSettings& settings, bool trigger, bool fadeOut, int32_t* out);
void render(IiaState& state, const IiaSettings& settings, bool trigger, bool fadeOut, int32_t* out);

} // namespace deluge::dsp::drum_synth::xia
