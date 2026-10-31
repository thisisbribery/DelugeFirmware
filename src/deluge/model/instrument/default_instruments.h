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
#include "dsp/drum_synth/drum_synth.h"
#include "model/instrument/synth_instruments.h"

class Instrument;
class Song;

/// The instruments new tracks start as, set in SETTINGS > DEFAULTS > CLIP TYPE: KIT INSTRUMENT (LXR, Cycles, GIN) and
/// SYNTH INSTRUMENT (one of the SHIFT + SYNTH instruments). Off on either gives that type of track as stock.
namespace deluge::default_instruments {

/// The preset kit a new kit track starts as, or NONE for a stock kit
dsp::drum_synth::PresetKit kit();

/// The instrument a new synth track starts as, or nullptr for stock (also for DX7 with the DX7 engine off)
const synth_instruments::SynthInstrument* synth();

/// For a new synth or kit track, when a default instrument is set for its type: the first preset in that instrument's
/// folder not on another track, loaded and out of hibernation, or if there's none a blank one (the kit as SHIFT + KIT
/// makes it, or a blank synth of that kind). A GIN kit is always its first built in kit, the top of its list. Its
/// audio files aren't loaded yet. nullptr if no default is set or it couldn't be made: the caller then goes on as
/// stock.
Instrument* forNewTrack(Song* song, OutputType type);

} // namespace deluge::default_instruments
