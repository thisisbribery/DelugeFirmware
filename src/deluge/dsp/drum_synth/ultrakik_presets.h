/*
 * ULTRAKIK's presets: sixteen big kicks, clean and deep first, then crunchier and more broken down the list. All tuned
 * to C on a kit row, and the two that bend dive a whole octave, so they land on C too.
 *
 * Copyright © 2026 thisisbribery.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "dsp/drum_synth/ultrakik.h"
#include <array>
#include <cstdint>

namespace deluge::dsp::drum_synth::ultrakik {

/// The knobs, 0-127, in the order of their DrumControls slots (UK_SWEEP to UK_SUB)
enum Knob : uint8_t {
	SWEEP,
	SWEEP_TIME,
	FOLD,
	GRIT,
	DRIVE,
	CLICK,
	CLICK_TONE,
	BEND,
	PUNCH,
	DECAY,
	HOLD,
	SUB,
	NUM_KNOBS
};

struct Preset {
	char const* name;
	std::array<uint8_t, NUM_KNOBS> knobs;
	Wave wave;
	DriveType driveType;
	Click clickType;
	uint8_t crush;    ///< 0-127
	uint8_t bendTime; ///< 0-127
	uint8_t snap;     ///< 0-127
	int8_t transpose; ///< On a kit row, semitones from the kit's middle C: -24 is C2 (65 Hz), -36 is C1 (33 Hz)
};

inline constexpr int32_t kNumPresets = 16;
/// ULTRA, what the engine starts on: a thick, low thump with colour and grit on. A 2.9 octave sweep over about 30 ms on
/// top of a 2.5 octave punch, a little fold, grit and 24 dB of warm drive, a dark thud of a click, about 30 ms of hold,
/// then a decay that's down 20 dB by about 280 ms and 40 dB by 400 ms.
inline constexpr int32_t kDefaultPreset = 4;

/// Clean and deep first, then crunchier and more broken down the list
extern const std::array<Preset, kNumPresets> kPresets;

/// A preset's settings, as the Deluge reads them from its controls (bend centred on 64)
Settings settingsFor(const Preset& preset);

} // namespace deluge::dsp::drum_synth::ultrakik
