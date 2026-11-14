/*
 * RINGS: a resonator engine for the Deluge, ported from Mutable Instruments Rings.
 *
 * Portions Copyright 2015 Emilie Gillet (Mutable Instruments Rings, MIT licence, below).
 * Deluge port Copyright © 2026 thisisbribery.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software and associated
 * documentation files (the "Software"), to deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to the following conditions: The above copyright
 * notice and this permission notice shall be included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE
 * WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR
 * COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR
 * OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 *
 * The Deluge combination is distributed under the GNU General Public License v3 with the rest of the firmware.
 *
 * One Deluge voice is one Rings voice: each note strikes its own resonator, the Deluge allocates and steals voices, and
 * its envelopes, filters and effects follow. Rings' internal exciter strikes on note on; Bow adds a continuous noise
 * excitation for sustained, bowed sounds.
 */

#pragma once

#include <cstdint>

namespace deluge::dsp::drum_synth::rings {

/// Rings' resonator models, as on the module (its "Disastrous Peace" easter egg and the reverb model are left out: the
/// Deluge has its own reverb)
enum class Model : uint8_t {
	MODAL,       ///< Modal resonator: a bank of tuned band passes
	SYMPATHETIC, ///< Sympathetic strings: a struck string and strings tuned around it
	STRING,      ///< Inharmonic string: Karplus-Strong with dispersion
	FM,          ///< 2-op FM voice
	CHORD,       ///< Sympathetic strings tuned to a chord, picked by Structure
	NUM_MODELS,
};

/// Continuous settings, 0 to 1, as Rings' knobs
struct Settings {
	Model model = Model::MODAL;
	float structure = 0.25f;
	float brightness = 0.5f;
	float damping = 0.5f;
	float position = 0.5f;
	float bow = 0.f;   ///< Continuous noise excitation while the voice plays: none to strong
	float drive = 0.f; ///< Soft clip on the output: clean to driven
	/// Strings a voice on the sympathetic and chord models: 2 as Rings at 4-voice polyphony, 4 as at 2-voice. Rings
	/// shares 8 strings between its voices; so should the Deluge, through Max voices.
	uint8_t strings = 2;
};

/// The running state of one voice. Holds the strings' delay lines, so it is big (tens of KB): the Deluge allocates
/// these separately and only for voices on this engine.
class Voice;

/// Bytes a Voice needs. The caller provides the memory, aligned to 32: no constructor runs, and none needs to (reset()
/// sets up every member), and nothing needs destroying.
[[nodiscard]] uint32_t voiceSize();

/// Sets the voice up from whatever its memory holds, silent, ready for a fresh note. Cheap: the strings' delay lines
/// are cleared only as far as they are read, as they come to be read.
void reset(Voice& voice);
/// Strikes at the next render. The resonator keeps ringing from whatever it holds, as Rings does on a new strum.
void trigger(Voice& voice);
/// Writes n samples (n <= 64) of mono output, q31 (full scale = 1.0, soft clipped), over out. basePhaseIncrement is the
/// note's pitch: cycles per sample * 2^32, at 44.1 kHz.
void render(Voice& voice, const Settings& settings, uint32_t basePhaseIncrement, int32_t* out, int32_t n);
/// Output level, 1 = full scale, tracked over the last renders, for culling and ending the voice
[[nodiscard]] float level(const Voice& voice);

} // namespace deluge::dsp::drum_synth::rings
