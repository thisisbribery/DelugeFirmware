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

/// The synth instruments SHIFT + SYNTH picks between on a synth track. Each has its own preset folder: picking one
/// loads the first preset there, and SELECT then steps through that folder's presets as usual. A new instrument is one
/// entry here (and its name string) plus its preset folder on the card.
namespace deluge::synth_instruments {

struct SynthInstrument {
	l10n::String name;
	char const* folder;
	/// What an empty folder gives: a blank synth in this mode (DX7 on osc 1 if dx)
	SynthMode mode;
	bool dx;
};

inline constexpr std::array<SynthInstrument, 3> kSynthInstruments{{
    {l10n::String::STRING_FOR_SYNTH_INSTRUMENT_DELUGE, "SYNTHS", SynthMode::SUBTRACTIVE, false},
    {l10n::String::STRING_FOR_SYNTH_INSTRUMENT_DLFM, "SYNTHS/DLFM", SynthMode::FM, false},
    {l10n::String::STRING_FOR_SYNTH_INSTRUMENT_DX7, "SYNTHS/DX7", SynthMode::SUBTRACTIVE, true},
}};

} // namespace deluge::synth_instruments
