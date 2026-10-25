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
#include <cstdint>
#include <span>

class InstrumentClip;
class Kit;
class ParamManagerForTimeline;
class SoundDrum;

/// The preset kits SHIFT + KIT makes (LXR, Cycles), and their presets: ordinary kit files saved into KITS/LXR or
/// KITS/CYCLES. A preset only replaces the sounds of a kit's preset rows, in order from the bottom, so the pattern and
/// any rows added to the kit stay as they are.
namespace deluge::preset_kit {

using dsp::drum_synth::PresetKit;

/// One row of a preset kit as SHIFT + KIT first makes it
struct Row {
	dsp::drum_synth::Preset preset;
	dsp::drum_synth::Engine cycEngine; ///< NUM_ENGINES for an LXR voice
	char const* name;
};

/// The rows a preset kit starts with, bottom row first
std::span<const Row> rowsFor(PresetKit kit);

/// Where a preset kit's presets are saved on the card
char const* folderFor(PresetKit kit);

/// A new sound for a preset kit row, with its param manager set up in paramManager. nullptr if out of RAM.
SoundDrum* makeDrum(const Row& row, PresetKit kit, ParamManagerForTimeline& paramManager);

/// The preset kit a kit was made as, from its rows, or NONE
PresetKit typeOf(Kit* kit);

/// Read the preset rows' sounds from a preset file in folderFor(kit): the rows saved as preset rows, or if there are
/// none (a kit made by hand), every sound row, bottom first. Fills drums and paramManagers and returns how many were
/// read, up to drums.size(). On failure returns 0 and sets error.
int32_t loadDrums(PresetKit kit, char const* fileName, std::span<SoundDrum*> drums,
                  std::span<ParamManagerForTimeline> paramManagers, Error* error);

/// Put newDrum on the clip's NoteRow at noteRowIndex in place of the drum it has, in every Clip of the Kit.
/// paramManager: newDrum's own (a new sound); nullptr when newDrum is a sound taken off the row earlier with keepOld,
/// which gets each Clip's own param manager back. keepOld: take the old drum out of the kit but keep it, with its param
/// managers backed up, to put back later; otherwise it is deleted.
void swapDrum(Kit* kit, InstrumentClip* clip, int32_t noteRowIndex, SoundDrum* newDrum,
              ParamManagerForTimeline* paramManager, bool keepOld);

/// Delete a drum that's in no kit, and its backed up param managers
void deleteDrum(SoundDrum* drum);

} // namespace deluge::preset_kit
