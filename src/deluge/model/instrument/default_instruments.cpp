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

#include "model/instrument/default_instruments.h"
#include "gui/ui/browser/browser.h"
#include "gui/ui/load/load_instrument_preset_ui.h"
#include "hid/display/display.h"
#include "model/instrument/kit.h"
#include "model/instrument/preset_kit.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "modulation/params/param_manager.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"
#include "storage/flash_storage.h"
#include "storage/storage_manager.h"

namespace deluge::default_instruments {

using dsp::drum_synth::PresetKit;
using synth_instruments::kSynthInstruments;
using synth_instruments::SynthInstrument;

PresetKit kit() {
	return (FlashStorage::defaultKitInstrument < util::to_underlying(PresetKit::NUM_KITS))
	           ? static_cast<PresetKit>(FlashStorage::defaultKitInstrument)
	           : PresetKit::NONE;
}

const SynthInstrument* synth() {
	const uint8_t setting = FlashStorage::defaultSynthInstrument;
	if (setting == 0 || setting > kSynthInstruments.size()) {
		return nullptr;
	}
	const SynthInstrument& instrument = kSynthInstruments[setting - 1];
	if (instrument.dx && !runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::EnableDX7Engine)) {
		return nullptr;
	}
	return &instrument;
}

namespace {

/// The first preset in folder (or its subfolders) not on a track of the song, loaded. nullptr if there's none.
Instrument* firstFreePreset(Song* song, OutputType type, char const* folder) {
	if (Browser::currentDir.set(folder) != Error::NONE) {
		return nullptr;
	}
	auto result = loadInstrumentPresetUI.findAnUnlaunchedPresetIncludingWithinSubfolders(
	    song, type, Availability::INSTRUMENT_UNUSED);
	if (!result || result.value()->instrumentAlreadyInSong) {
		Browser::emptyFileItems();
		return nullptr;
	}

	FileItem* fileItem = result.value();
	Instrument* instrument = fileItem->instrument;
	const bool hibernating = instrument != nullptr;
	Error error = Error::NONE;
	if (instrument == nullptr) {
		String presetName;
		fileItem->getFilenameWithoutExtension(&presetName);
		error = StorageManager::loadInstrumentFromFile(song, nullptr, type, false, &instrument, &fileItem->filePointer,
		                                               &presetName, &Browser::currentDir);
	}
	Browser::emptyFileItems();

	if (error != Error::NONE) {
		display->displayError(error);
		return nullptr;
	}
	if (hibernating) {
		song->removeInstrumentFromHibernationList(instrument);
	}
	return instrument;
}

/// A new name in folder, like KIT004 or SYNT012, for a blank instrument saved there
bool unusedName(OutputType type, char const* folder, String* name) {
	return Browser::currentDir.set(folder) == Error::NONE
	       && loadInstrumentPresetUI.getUnusedSlot(type, name, (type == OutputType::KIT) ? "KIT" : "SYNT")
	              == Error::NONE
	       && !name->isEmpty();
}

/// A blank synth of the instrument's kind, which saves into its folder
Instrument* blankSynth(Song* song, const SynthInstrument& instrument) {
	String name;
	if (!unusedName(OutputType::SYNTH, instrument.folder, &name)) {
		return nullptr;
	}
	ParamManagerForTimeline paramManager;
	auto* synth = static_cast<SoundInstrument*>(StorageManager::createNewInstrument(OutputType::SYNTH, &paramManager));
	if (synth == nullptr) {
		return nullptr;
	}
	synth->name.set(&name);
	synth->dirPath.set(instrument.folder);
	synth->setupAsBlankSynth(&paramManager, instrument.dx);
	if (instrument.mode != synth->getSynthMode()) {
		synth->setSynthMode(instrument.mode, song);
	}
	synth->editedByUser = true;
	synth->mightExistOnCard = false;
	song->backUpParamManager(synth, nullptr, &paramManager, true);
	return synth;
}

/// A preset kit as SHIFT + KIT makes it: its rows bottom first, each with its sound's param manager backed up as a
/// kit loaded from a file has them
Instrument* blankKit(Song* song, PresetKit type) {
	char const* folder = getInstrumentFolder(OutputType::KIT);
	String name;
	if (!unusedName(OutputType::KIT, folder, &name)) {
		return nullptr;
	}
	ParamManagerForTimeline kitParamManager;
	auto* kit = static_cast<Kit*>(StorageManager::createNewInstrument(OutputType::KIT, &kitParamManager));
	if (kit == nullptr) {
		return nullptr;
	}
	kit->name.set(&name);
	kit->dirPath.set(folder);

	for (const preset_kit::Row& row : preset_kit::rowsFor(type)) {
		ParamManagerForTimeline paramManager;
		SoundDrum* drum = preset_kit::makeDrum(row, type, paramManager);
		if (drum == nullptr) {
			display->displayError(Error::INSUFFICIENT_RAM);
			break;
		}
		song->backUpParamManager(drum, nullptr, &paramManager, true);
		kit->addDrum(drum);
	}
	kit->editedByUser = true;
	kit->mightExistOnCard = false;
	song->backUpParamManager(static_cast<ModControllableAudio*>(kit->toModControllable()), nullptr, &kitParamManager,
	                         true);
	return kit;
}

} // namespace

Instrument* forNewTrack(Song* song, OutputType type) {
	if (song == nullptr) {
		return nullptr;
	}
	if (type == OutputType::KIT) {
		const PresetKit preset = kit();
		if (preset == PresetKit::NONE) {
			return nullptr;
		}
		Instrument* instrument =
		    (preset != PresetKit::GIN) ? firstFreePreset(song, type, preset_kit::folderFor(preset)) : nullptr;
		return (instrument != nullptr) ? instrument : blankKit(song, preset);
	}
	if (type == OutputType::SYNTH) {
		const SynthInstrument* instrument = synth();
		if (instrument == nullptr) {
			return nullptr;
		}
		Instrument* preset = firstFreePreset(song, type, instrument->folder);
		return (preset != nullptr) ? preset : blankSynth(song, *instrument);
	}
	return nullptr;
}

} // namespace deluge::default_instruments
