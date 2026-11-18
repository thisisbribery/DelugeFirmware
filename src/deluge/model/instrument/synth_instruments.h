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
#include <span>

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
	/// BROPHET (the Pro-53 voice in SYNTH MODE). As BRINGS: its presets are the way in.
	bool brophet;
	/// The stock blank synth, as SHIFT + SYNTH made before: no folder of its own, not a default instrument
	bool init;
	/// BRUNO106 (the KR-106 DCO voice in SYNTH MODE). As BROPHET: its presets are the way in.
	bool bruno106 = false;
};

// BROPHET goes after DX7, and BRUNO106 after BROPHET, so the types saved before them (Defaults > Synth instrument) keep
// their numbers. DX7 only shows
// with its setting on, so it usually comes straight after BRINGS.
inline constexpr std::array<SynthInstrument, 10> kSynthInstruments{{
    {l10n::String::STRING_FOR_SYNTH_TYPE_SUBT, "SYNTHS/SUBT", SynthMode::SUBTRACTIVE, false, false, false, false,
     false},
    {l10n::String::STRING_FOR_SYNTH_TYPE_FM, "SYNTHS/FM", SynthMode::FM, false, false, false, false, false},
    {l10n::String::STRING_FOR_SYNTH_TYPE_WAVE, "SYNTHS/WAVE", SynthMode::SUBTRACTIVE, false, true, false, false, false},
    {l10n::String::STRING_FOR_SYNTH_TYPE_RING, "SYNTHS/RING", SynthMode::RINGMOD, false, false, false, false, false},
    {l10n::String::STRING_FOR_SYNTH_TYPE_BRIB, "SYNTHS/BRINGS", SynthMode::SUBTRACTIVE, false, false, true, false,
     false},
    {l10n::String::STRING_FOR_SYNTH_INSTRUMENT_DX7, "SYNTHS/DX7", SynthMode::SUBTRACTIVE, true, false, false, false,
     false},
    {l10n::String::STRING_FOR_SYNTH_TYPE_BROPHET, "SYNTHS/BROPHET", SynthMode::SUBTRACTIVE, false, false, false, true,
     false},
    {l10n::String::STRING_FOR_SYNTH_TYPE_BRUNO106, "SYNTHS/BRUNO106", SynthMode::SUBTRACTIVE, false, false, false,
     false, false, true},
    // The 303 (SYNTH MODE 303): after BRUNO106, so the types saved before it keep their numbers
    {l10n::String::STRING_FOR_SYNTH_TYPE_303, "SYNTHS/303", SynthMode::TB303, false, false, false, false, false},
    {l10n::String::STRING_FOR_SYNTH_TYPE_INIT, "SYNTHS", SynthMode::SUBTRACTIVE, false, false, false, false, true},
}};

/// The ones a new track can start as (Settings > Defaults): all but INIT, which is last
inline constexpr size_t kNumDefaultSynthInstruments = kSynthInstruments.size() - 1;

/// The category folders inside each type's folder, in the order SHIFT + SYNTH offers them (LEAD first). label, if set,
/// is shown instead of the name string (the same on OLED and 7SEG, which scrolls it).
struct SynthCategory {
	l10n::String name;
	char const* folder;
	char const* label = nullptr;
};

inline constexpr std::array<SynthCategory, 4> kSynthCategories{{
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_LEAD, "LEAD"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_BASS, "BASS"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_PAD, "PAD"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_MISC, "MISC"},
}};

/// BROPHET's categories follow Pro-53's own programs: BNK1 to BNK8 are its eight files of 64 (8 banks of 8 programs,
/// the presets named by program number, as 411 for file 4, bank 1, program 1), then BRIB, the Bribes presets
inline constexpr std::array<SynthCategory, 9> kBrophetCategories{{
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_MISC, "BNK1", "BNK1"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_MISC, "BNK2", "BNK2"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_MISC, "BNK3", "BNK3"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_MISC, "BNK4", "BNK4"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_MISC, "BNK5", "BNK5"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_MISC, "BNK6", "BNK6"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_MISC, "BNK7", "BNK7"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_MISC, "BNK8", "BNK8"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_MISC, "BRIB", "BRIB"},
}};

/// DX7's categories: the usual four, with its keys (pianos, organs, clavs) and bells (mallets, chimes) in their own
inline constexpr std::array<SynthCategory, 6> kDx7Categories{{
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_LEAD, "LEAD"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_BASS, "BASS"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_PAD, "PAD"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_MISC, "KEYS", "KEYS"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_MISC, "BELL", "BELL"},
    {l10n::String::STRING_FOR_SYNTH_CATEGORY_MISC, "MISC"},
}};

/// The most categories any type has
inline constexpr size_t kMaxSynthCategories = kBrophetCategories.size();
static_assert(kDx7Categories.size() <= kMaxSynthCategories);

/// A type's categories
inline std::span<const SynthCategory> categoriesFor(const SynthInstrument& type) {
	if (type.brophet) {
		return kBrophetCategories;
	}
	if (type.dx) {
		return kDx7Categories;
	}
	return kSynthCategories;
}

} // namespace deluge::synth_instruments
