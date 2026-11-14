/*
 * Copyright © 2026 the Deluge community.
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
 */

#pragma once

#include "definitions_cxx.hpp"
#include "gui/l10n/strings.h"
#include <array>

/// The synth types SHIFT + SYNTH picks between, on any track. Each has its own preset folder, sorted into the
/// category folders below: picking a type then a category opens the preset browser in that folder, playing its first
/// preset. INIT, last, is the stock blank synth. A new type is one entry here (and its name string) plus its folder.
namespace deluge::synth_instruments {

struct SynthInstrument {
	l10n::String name;
	char const* folder;
	/// What an empty folder gives: a blank synth in this mode (DX7 on osc 1 if dx)
	SynthMode mode;
	bool dx;
	/// Wavetable oscillators (a subtractive synth with one tells WAVE from SUBT)
	bool wavetable;
	/// BRIBERING MODE (the Rings engine in SYNTH MODE). Its presets are the way in: an empty folder gives a plain
	/// synth.
	bool rings;
	/// The stock blank synth, as SHIFT + SYNTH made before: no folder of its own, not a default instrument
	bool init;
};

inline constexpr std::array<SynthInstrument, 7> kSynthInstruments{{
    {l10n::String::STRING_FOR_SYNTH_TYPE_SUBT, "SYNTHS/SUBT", SynthMode::SUBTRACTIVE, false, false, false, false},
    {l10n::String::STRING_FOR_SYNTH_TYPE_FM, "SYNTHS/FM", SynthMode::FM, false, false, false, false},
    {l10n::String::STRING_FOR_SYNTH_TYPE_WAVE, "SYNTHS/WAVE", SynthMode::SUBTRACTIVE, false, true, false, false},
    {l10n::String::STRING_FOR_SYNTH_TYPE_RING, "SYNTHS/RING", SynthMode::RINGMOD, false, false, false, false},
    {l10n::String::STRING_FOR_SYNTH_TYPE_BRIB, "SYNTHS/BRIB", SynthMode::SUBTRACTIVE, false, false, true, false},
    {l10n::String::STRING_FOR_SYNTH_INSTRUMENT_DX7, "SYNTHS/DX7", SynthMode::SUBTRACTIVE, true, false, false, false},
    {l10n::String::STRING_FOR_SYNTH_TYPE_INIT, "SYNTHS", SynthMode::SUBTRACTIVE, false, false, false, true},
}};

/// The ones a new track can start as (Settings > Defaults): all but INIT, which is last
inline constexpr size_t kNumDefaultSynthInstruments = kSynthInstruments.size() - 1;

/// The category folders inside each type's folder, in the order SHIFT + SYNTH offers them
struct SynthCategory {
	l10n::String name;
	char const* folder;
};

inline constexpr std::array<SynthCategory, 4> kSynthCategories{{
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_BASS, "BASS"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_LEAD, "LEAD"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_PAD, "PAD"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_MISC, "MISC"},
}};

} // namespace deluge::synth_instruments
