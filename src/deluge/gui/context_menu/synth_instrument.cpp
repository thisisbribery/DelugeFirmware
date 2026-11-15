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

#include "gui/context_menu/synth_instrument.h"
#include "definitions_cxx.hpp"
#include "dsp/drum_synth/drum_synth.h"
#include "fatfs/ff.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/load/load_instrument_preset_ui.h"
#include "gui/views/instrument_clip_view.h"
#include "gui/views/view.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/instrument.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "processing/sound/sound_instrument.h"
#include <cstdio>
#include <cstring>
#include <strings.h>

namespace deluge::gui::context_menu {

using synth_instruments::kSynthInstruments;
using synth_instruments::SynthInstrument;

SynthInstrumentMenu synthInstrumentMenu{};
SynthCategoryMenu synthCategoryMenu{};

namespace {

/// Whether a preset folder has any presets (XML files) directly in it
bool folderHasPresets(char const* folder) {
	DIR dir;
	if (f_opendir(&dir, folder) != FR_OK) {
		return false;
	}
	bool found = false;
	FILINFO fileInfo;
	while (!found && f_readdir(&dir, &fileInfo) == FR_OK && fileInfo.fname[0]) {
		const size_t length = strlen(fileInfo.fname);
		found = !(fileInfo.fattrib & AM_DIR) && fileInfo.fname[0] != '.' && length >= 5
		        && strcasecmp(fileInfo.fname + length - 4, ".XML") == 0;
	}
	f_closedir(&dir);
	return found;
}

/// The folder of a type's category, like SYNTHS/SUBT/BASS
void categoryFolder(size_t type, size_t category, char* out, size_t outSize) {
	snprintf(out, outSize, "%s/%s", kSynthInstruments[type].folder,
	         synth_instruments::categoriesFor(kSynthInstruments[type])[category].folder);
}

size_t numCategories(size_t type) {
	return synth_instruments::categoriesFor(kSynthInstruments[type]).size();
}

/// Whether path is folder or inside it
bool inFolder(char const* path, char const* folder) {
	const size_t length = strlen(folder);
	return !strncasecmp(path, folder, length) && (path[length] == '\0' || path[length] == '/');
}

bool typeAvailable(size_t type) {
	return !kSynthInstruments[type].dx || runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::EnableDX7Engine);
}

/// The type a synth plays: the one whose folder it came from, else the first that sounds like it
size_t typeOf(SoundInstrument* synth) {
	for (size_t i = 0; i < kSynthInstruments.size(); i++) {
		if (!kSynthInstruments[i].init && inFolder(synth->dirPath.get(), kSynthInstruments[i].folder)) {
			return i;
		}
	}
	const bool dx = synth->sources[0].oscType == OscType::DX7;
	const bool rings = synth->sources[0].oscType == OscType::DRUM_SYNTH
	                   && dsp::drum_synth::isRings(synth->sources[0].drumPatch.engine());
	const bool brophet = synth->sources[0].oscType == OscType::DRUM_SYNTH
	                     && dsp::drum_synth::isBrophet(synth->sources[0].drumPatch.engine());
	const bool bruno106 = synth->sources[0].oscType == OscType::DRUM_SYNTH
	                      && dsp::drum_synth::isBruno106(synth->sources[0].drumPatch.engine());
	const bool wavetable =
	    synth->getSynthMode() == SynthMode::SUBTRACTIVE
	    && (synth->sources[0].oscType == OscType::WAVETABLE || synth->sources[1].oscType == OscType::WAVETABLE);
	for (size_t i = 0; i < kSynthInstruments.size(); i++) {
		const SynthInstrument& type = kSynthInstruments[i];
		if (type.init) {
			continue;
		}
		if (rings      ? type.rings
		    : brophet  ? type.brophet
		    : bruno106 ? type.bruno106
		    : dx       ? type.dx
		         : (!type.dx && !type.rings && !type.brophet && !type.bruno106 && type.mode == synth->getSynthMode()
		            && type.wavetable == wavetable)) {
			return i;
		}
	}
	return 0;
}

/// A blank synth of the type, saving into its folder (the type has no presets yet)
void makeBlankSynth(const SynthInstrument& type) {
	if (instrumentClipView.createNewInstrument(OutputType::SYNTH, type.dx)
	    && getCurrentOutputType() == OutputType::SYNTH) {
		auto* synth = static_cast<SoundInstrument*>(getCurrentOutput());
		synth->dirPath.set(type.folder);
		if (type.mode != synth->getSynthMode()) {
			synth->setSynthMode(type.mode, currentSong);
		}
	}
}

} // namespace

bool openSynthPresetFolder(char const* folder, int32_t backTo) {
	InstrumentClip* clip = getCurrentInstrumentClip();
	if (clip == nullptr) {
		return false;
	}
	loadInstrumentPresetUI.setupLoadInstrument(OutputType::SYNTH, static_cast<Instrument*>(clip->output), clip);
	loadInstrumentPresetUI.setPickerFolder(folder, backTo);
	return openUI(&loadInstrumentPresetUI);
}

// ---- Types ----

char const* SynthInstrumentMenu::getTitle() {
	return l10n::get(l10n::String::STRING_FOR_SYNTH_INSTRUMENT);
}

std::span<char const*> SynthInstrumentMenu::getOptions() {
	static std::array<char const*, kSynthInstruments.size()> options;
	for (size_t i = 0; i < kSynthInstruments.size(); i++) {
		options[i] = l10n::get(kSynthInstruments[i].name);
	}
	return {options.data(), options.size()};
}

bool SynthInstrumentMenu::isCurrentOptionAvailable() {
	return typeAvailable(currentOption);
}

bool SynthInstrumentMenu::setupAndCheckAvailability() {
	switch (getCurrentOutputType()) {
	case OutputType::SYNTH:
		setupFor(typeOf(static_cast<SoundInstrument*>(getCurrentOutput())));
		return true;
	case OutputType::KIT: {
		// As stock: a kit clip only turns into a synth clip while it has no notes or sounds
		InstrumentClip* clip = getCurrentInstrumentClip();
		if (clip == nullptr || !clip->isEmpty() || !clip->output->isEmpty()) {
			return false;
		}
		setupFor(0);
		return true;
	}
	case OutputType::MIDI_OUT:
	case OutputType::CV:
		setupFor(0);
		return true;
	default:
		return false;
	}
}

void SynthInstrumentMenu::setupFor(size_t type) {
	currentOption = (type < kSynthInstruments.size() && typeAvailable(type)) ? static_cast<int32_t>(type) : 0;
	scrollPos = currentOption;
}

ActionResult SynthInstrumentMenu::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	// SHIFT + SYNTH again: the next type, round to the first after the last
	if (b == deluge::hid::button::SYNTH && Buttons::isShiftButtonPressed()) {
		if (on) {
			const int32_t numOptions = static_cast<int32_t>(kSynthInstruments.size());
			int32_t next = currentOption;
			do {
				next = (next + 1) % numOptions;
			} while (next != currentOption && !typeAvailable(next));
			currentOption = next;
			scrollPos = currentOption;
			if (display->haveOLED()) {
				renderUIsForOled();
			}
			else {
				drawCurrentOption();
			}
		}
		return ActionResult::DEALT_WITH;
	}
	return ContextMenu::buttonAction(b, on, inCardRoutine);
}

bool SynthInstrumentMenu::acceptCurrentOption() {
	const size_t typeIndex = currentOption;
	const SynthInstrument& type = kSynthInstruments[typeIndex];
	display->setNextTransitionDirection(-1);
	close();

	// INIT: the stock blank synth
	if (type.init) {
		instrumentClipView.createNewInstrument(OutputType::SYNTH);
		return true;
	}

	// Make the folders if they're missing, ready to save presets into, as the Deluge can't make folders when saving
	f_mkdir("SYNTHS");
	f_mkdir(type.folder);
	if (!type.dx) {
		char folder[48];
		for (size_t c = 0; c < numCategories(typeIndex); c++) {
			categoryFolder(typeIndex, c, folder, sizeof(folder));
			f_mkdir(folder);
		}
	}

	if (synthCategoryMenu.setupFor(typeIndex)) {
		openUI(&synthCategoryMenu);
	}
	// No categories in use (DX7, or presets kept loose in the type's folder): its folder
	else if (folderHasPresets(type.folder)) {
		openSynthPresetFolder(type.folder, -1);
	}
	// Nothing saved there yet: a blank synth of that type, which saves into the folder
	else {
		makeBlankSynth(type);
	}
	return true;
}

// ---- Categories ----

char const* SynthCategoryMenu::getTitle() {
	return l10n::get(l10n::String::STRING_FOR_SYNTH_CATEGORY);
}

std::span<char const*> SynthCategoryMenu::getOptions() {
	static std::array<char const*, synth_instruments::kMaxSynthCategories> options;
	const auto categories = synth_instruments::categoriesFor(kSynthInstruments[type_]);
	for (size_t i = 0; i < categories.size(); i++) {
		options[i] = (categories[i].label != nullptr) ? categories[i].label : l10n::get(categories[i].name);
	}
	return {options.data(), categories.size()};
}

bool SynthCategoryMenu::setupFor(size_t type) {
	type_ = type;
	if (kSynthInstruments[type].dx || kSynthInstruments[type].init) {
		return false;
	}
	char folder[48];
	bool any = false;
	hasPresets_.fill(false);
	for (size_t c = 0; c < numCategories(type); c++) {
		categoryFolder(type, c, folder, sizeof(folder));
		hasPresets_[c] = folderHasPresets(folder);
		any = any || hasPresets_[c];
	}
	if (!any) {
		return false;
	}

	// On the category the current synth came from, else the first with presets
	int32_t current = -1;
	if (getCurrentOutputType() == OutputType::SYNTH) {
		char const* dirPath = static_cast<Instrument*>(getCurrentOutput())->dirPath.get();
		for (size_t c = 0; c < numCategories(type) && current < 0; c++) {
			categoryFolder(type, c, folder, sizeof(folder));
			if (hasPresets_[c] && inFolder(dirPath, folder)) {
				current = static_cast<int32_t>(c);
			}
		}
	}
	for (size_t c = 0; c < numCategories(type) && current < 0; c++) {
		if (hasPresets_[c]) {
			current = static_cast<int32_t>(c);
		}
	}
	currentOption = current;
	scrollPos = currentOption;
	return true;
}

ActionResult SynthCategoryMenu::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	// BACK: up to the types, on this one
	if (b == deluge::hid::button::BACK) {
		if (on) {
			if (inCardRoutine) {
				return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
			}
			display->setNextTransitionDirection(-1);
			close();
			synthInstrumentMenu.setupFor(type_);
			openUI(&synthInstrumentMenu);
		}
		return ActionResult::DEALT_WITH;
	}
	return ContextMenu::buttonAction(b, on, inCardRoutine);
}

bool SynthCategoryMenu::acceptCurrentOption() {
	char folder[48];
	categoryFolder(type_, currentOption, folder, sizeof(folder));
	display->setNextTransitionDirection(1);
	close();
	openSynthPresetFolder(folder, static_cast<int32_t>(type_));
	return true;
}

} // namespace deluge::gui::context_menu
