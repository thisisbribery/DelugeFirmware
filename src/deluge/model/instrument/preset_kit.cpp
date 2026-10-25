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

#include "model/instrument/preset_kit.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/song/clip_iterators.h"
#include "model/song/song.h"
#include "modulation/params/param_manager.h"
#include "processing/sound/sound_drum.h"
#include "storage/storage_manager.h"
#include "util/d_string.h"
#include <algorithm>
#include <array>

namespace deluge::preset_kit {

using dsp::drum_synth::Engine;
using dsp::drum_synth::Preset;

namespace {
constexpr std::array<Row, 7> kLxrRows{{
    {Preset::KICK, Engine::NUM_ENGINES, "KICK"},
    {Preset::SNARE, Engine::NUM_ENGINES, "SNARE"},
    {Preset::CLOSED_HAT, Engine::NUM_ENGINES, "CH"},
    {Preset::OPEN_HAT, Engine::NUM_ENGINES, "OH"},
    {Preset::LOW_TOM, Engine::NUM_ENGINES, "LTOM"},
    {Preset::HIGH_TOM, Engine::NUM_ENGINES, "HTOM"},
    {Preset::CYMBAL, Engine::NUM_ENGINES, "CYM"},
}};
// The Cycles' usual six tracks
constexpr std::array<Row, 6> kCyclesRows{{
    {Preset::KICK, Engine::CYC_KIK, "KIK"},
    {Preset::KICK, Engine::CYC_SNR, "SNR"},
    {Preset::KICK, Engine::CYC_METAL, "METAL"},
    {Preset::KICK, Engine::CYC_TONE, "TONE"},
    {Preset::KICK, Engine::CYC_PERC, "PERC"},
    {Preset::KICK, Engine::CYC_CHORD, "CHORD"},
}};
// GIN's eight: the hats choke each other
constexpr std::array<Row, dsp::drum_synth::gin::kNumKitSounds> kGinRows{{
    {Preset::KICK, Engine::GIN, "KICK", dsp::drum_synth::gin::KICK},
    {Preset::KICK, Engine::GIN, "SNR", dsp::drum_synth::gin::SNARE},
    {Preset::KICK, Engine::GIN, "HAT1", dsp::drum_synth::gin::HAT1},
    {Preset::KICK, Engine::GIN, "HAT2", dsp::drum_synth::gin::HAT2},
    {Preset::KICK, Engine::GIN, "PERC1", dsp::drum_synth::gin::PERC1},
    {Preset::KICK, Engine::GIN, "PERC2", dsp::drum_synth::gin::PERC2},
    {Preset::KICK, Engine::GIN, "CLAP", dsp::drum_synth::gin::CLAP},
    {Preset::KICK, Engine::GIN, "OTHER", dsp::drum_synth::gin::OTHER},
}};
} // namespace

std::span<const Row> rowsFor(PresetKit kit) {
	switch (kit) {
	case PresetKit::CYCLES:
		return kCyclesRows;
	case PresetKit::GIN:
		return kGinRows;
	default:
		return kLxrRows;
	}
}

char const* folderFor(PresetKit kit) {
	switch (kit) {
	case PresetKit::CYCLES:
		return "KITS/CYCLES";
	case PresetKit::GIN:
		return "KITS/GIN";
	default:
		return "KITS/LXR";
	}
}

int32_t numBuiltInKits(PresetKit kit) {
	return (kit == PresetKit::GIN) ? dsp::drum_synth::gin::kNumKits : 1;
}

char const* builtInKitName(PresetKit kit, int32_t index) {
	return dsp::drum_synth::gin::kKits[std::clamp<int32_t>(index, 0, dsp::drum_synth::gin::kNumKits - 1)].name;
}

SoundDrum* makeDrum(const Row& row, PresetKit kit, ParamManagerForTimeline& paramManager, int32_t builtInKit) {
	void* memory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(SoundDrum));
	if (memory == nullptr) {
		return nullptr;
	}
	if (paramManager.setupWithPatching() != Error::NONE) {
		delugeDealloc(memory);
		return nullptr;
	}
	Sound::initParams(&paramManager);
	auto* drum = new (memory) SoundDrum();
	if (row.ginRow >= 0) {
		const auto& kitSounds =
		    dsp::drum_synth::gin::kKits[std::clamp<int32_t>(builtInKit, 0, dsp::drum_synth::gin::kNumKits - 1)].sounds;
		const bool hat = row.ginRow == dsp::drum_synth::gin::HAT1 || row.ginRow == dsp::drum_synth::gin::HAT2;
		drum->setupAsDrumSynth(&paramManager, row.preset, row.cycEngine, &kitSounds[row.ginRow], hat);
		// The kit's balance is in each sound's GIN level; the row volume stays where the Deluge starts it
	}
	else {
		drum->setupAsDrumSynth(&paramManager, row.preset, row.cycEngine);
	}
	drum->presetKit = kit;
	drum->drumName = row.name;
	return drum;
}

PresetKit typeOf(Kit* kit) {
	for (Drum* drum = kit->firstDrum; drum != nullptr; drum = drum->next) {
		if (drum->type == DrumType::SOUND && static_cast<SoundDrum*>(drum)->presetKit != PresetKit::NONE) {
			return static_cast<SoundDrum*>(drum)->presetKit;
		}
	}
	return PresetKit::NONE;
}

int32_t loadDrums(PresetKit kit, char const* fileName, std::span<SoundDrum*> drums,
                  std::span<ParamManagerForTimeline> paramManagers, Error* error) {
	String dirPath;
	String path;
	String name;
	dirPath.set(folderFor(kit));
	path.set(folderFor(kit));
	path.concatenate("/");
	path.concatenate(fileName);
	name.set(fileName);

	FilePointer filePointer;
	if (!StorageManager::fileExists(path.get(), &filePointer)) {
		*error = Error::FILE_NOT_FOUND;
		return 0;
	}
	Instrument* instrument = nullptr;
	*error = StorageManager::loadInstrumentFromFile(currentSong, nullptr, OutputType::KIT, true, &instrument,
	                                                &filePointer, &name, &dirPath);
	if (*error != Error::NONE) {
		return 0;
	}
	auto* fileKit = static_cast<Kit*>(instrument);

	// The rows saved as preset rows, in the order they were saved (bottom first), or every sound row of a kit made by
	// hand
	const bool anyPresetRows = typeOf(fileKit) != PresetKit::NONE;
	const int32_t max = std::min(drums.size(), paramManagers.size());
	int32_t count = 0;
	for (Drum* drum = fileKit->firstDrum; drum != nullptr && count < max;) {
		Drum* next = drum->next;
		auto* soundDrum = static_cast<SoundDrum*>(drum);
		if (drum->type == DrumType::SOUND && (!anyPresetRows || soundDrum->presetKit != PresetKit::NONE)) {
			fileKit->removeDrum(soundDrum);
			soundDrum->next = nullptr;
			if (currentSong->getBackedUpParamManagerPreferablyWithClip(soundDrum, nullptr, &paramManagers[count])) {
				soundDrum->presetKit = kit;
				drums[count++] = soundDrum;
			}
			else {
				deleteDrum(soundDrum);
			}
		}
		drum = next;
	}

	// The rest of the file's kit goes
	fileKit->deleteBackedUpParamManagers(currentSong);
	void* toDealloc = dynamic_cast<void*>(instrument);
	instrument->~Instrument();
	delugeDealloc(toDealloc);

	if (count == 0) {
		*error = Error::FILE_CORRUPTED;
	}
	return count;
}

void swapDrum(Kit* kit, InstrumentClip* clip, int32_t noteRowIndex, SoundDrum* newDrum,
              ParamManagerForTimeline* paramManager, bool keepOld) {
	NoteRow* noteRow = clip->noteRows.getElement(noteRowIndex);
	auto* oldDrum = static_cast<SoundDrum*>(noteRow->drum);
	const bool wasSelected = kit->selectedDrum == oldDrum;

	// The old drum stays in the kit until every NoteRow has been moved off it, as when loading a synth onto a kit row
	kit->addDrum(newDrum);

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithNoteRow* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip)
	                                        ->addNoteRow(clip->getNoteRowId(noteRow, noteRowIndex), noteRow);
	noteRow->stopCurrentlyPlayingNote(modelStack);
	// Backs the old drum's param manager up for this Clip, for putting it back or deleting it with the drum
	noteRow->setDrum(newDrum, kit, modelStack, nullptr, paramManager, true);

	// Every other Clip of this Kit. A new sound copies this Clip's param manager; a sound coming back gets each Clip's
	// own back.
	for (InstrumentClip* otherClip : InstrumentClips::everywhere(currentSong)) {
		if (otherClip == clip || otherClip->output != kit) {
			continue;
		}
		for (int32_t i = 0; i < otherClip->noteRows.getNumElements(); i++) {
			NoteRow* otherNoteRow = otherClip->noteRows.getElement(i);
			if (oldDrum == nullptr || otherNoteRow->drum != oldDrum) {
				continue;
			}
			char otherModelStackMemory[MODEL_STACK_MAX_SIZE];
			ModelStackWithNoteRow* otherModelStack =
			    setupModelStackWithTimelineCounter(otherModelStackMemory, currentSong, otherClip)
			        ->addNoteRow(otherClip->getNoteRowId(otherNoteRow, i), otherNoteRow);
			otherNoteRow->stopCurrentlyPlayingNote(otherModelStack);
			otherNoteRow->setDrum(newDrum, kit, otherModelStack, (paramManager != nullptr) ? clip : nullptr, nullptr,
			                      true);
		}
	}

	if (oldDrum != nullptr) {
		oldDrum->wontBeRenderedForAWhile();
		kit->drumsWithRenderingActive.deleteAtKey((int32_t)(Drum*)oldDrum);
		kit->removeDrum(oldDrum);
		oldDrum->next = nullptr;
		if (!keepOld) {
			deleteDrum(oldDrum);
		}
	}
	if (wasSelected) {
		kit->selectedDrum = newDrum;
	}
	kit->beenEdited();
}

void deleteDrum(SoundDrum* drum) {
	currentSong->deleteBackedUpParamManagersForModControllable(drum);
	void* toDealloc = dynamic_cast<void*>(drum);
	drum->~SoundDrum();
	delugeDealloc(toDealloc);
}

} // namespace deluge::preset_kit
