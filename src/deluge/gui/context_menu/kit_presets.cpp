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

#include "gui/context_menu/kit_presets.h"
#include "definitions_cxx.hpp"
#include "gui/context_menu/new_kit_type.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/ui.h"
#include "gui/views/instrument_clip_view.h"
#include "gui/views/view.h"
#include "hid/display/display.h"
#include "model/action/action_logger.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/kit.h"
#include "model/song/song.h"
#include "processing/sound/sound_drum.h"
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <strings.h>

extern "C" {
#include "fatfs/ff.h"
}

namespace deluge::gui::context_menu {

KitPresets kitPresets{};

bool KitPresets::setup(preset_kit::PresetKit kit) {
	kit_ = kit;
	readPresetList();

	InstrumentClip* clip = getCurrentInstrumentClip();
	numRows_ = 0;
	for (int32_t i = 0; i < clip->noteRows.getNumElements() && numRows_ < kMaxRows; i++) {
		Drum* drum = clip->noteRows.getElement(i)->drum;
		if (drum != nullptr && drum->type == DrumType::SOUND && static_cast<SoundDrum*>(drum)->presetKit == kit) {
			rows_[numRows_++] = i;
		}
	}
	if (numRows_ == 0) {
		return false;
	}
	originals_.fill(nullptr);

	// Open on the preset the kit is on now, or on Current if it's on none of them
	numBuiltIn_ = std::min(preset_kit::numBuiltInKits(kit), kMaxBuiltIn);
	hasCurrent_ = false;
	home_ = findHome();
	if (home_ < 0) {
		hasCurrent_ = true;
		home_ = 0;
	}
	using enum l10n::String;
	int32_t o = 0;
	if (hasCurrent_) {
		options_[o++] = l10n::get(STRING_FOR_KIT_PRESET_CURRENT);
	}
	for (int32_t i = 0; i < numBuiltIn_; i++) {
		options_[o++] = (kit == preset_kit::PresetKit::GIN) ? preset_kit::builtInKitName(kit, i)
		                                                    : l10n::get(STRING_FOR_KIT_PRESET_DEFAULT);
	}
	for (int32_t i = 0; i < numPresets_; i++) {
		options_[o++] = labels_[i].data();
	}
	previewing_ = home_;
	currentOption = home_;
	scrollPos = home_;
	return true;
}

void KitPresets::startOnTopPreset() {
	const int32_t top = (numPresets_ > 0 && kit_ != preset_kit::PresetKit::GIN) ? firstPresetOption() : defaultOption();
	currentOption = top;
	scrollPos = top;
	preview(top);
}

bool KitPresets::step(preset_kit::PresetKit kit, int32_t offset) {
	if (offset == 0 || !setup(kit)) {
		return false;
	}
	// Current isn't a preset to step to: from there, the first or the last
	const int32_t first = defaultOption();
	const int32_t count = numOptions() - first;
	int32_t target;
	if (hasCurrent_) {
		target = (offset > 0) ? first : numOptions() - 1;
	}
	else {
		target = first + (home_ - first + ((offset > 0) ? 1 : count - 1)) % count;
	}
	if (target == home_) {
		return true;
	}
	currentOption = target;
	preview(target);
	if (previewing_ == target) {
		remember(target);
		keep();
		getCurrentKit()->beenEdited();
	}
	return true;
}

int32_t KitPresets::findHome() {
	Kit* kit = getCurrentKit();
	char const* name = kit->presetName.get();
	// A built in kit: "*" for the first (Default), "*2" on for GIN's others
	if (name[0] == '*') {
		const int32_t index = (name[1] != 0) ? atoi(name + 1) - 1 : 0;
		return defaultOption() + std::clamp<int32_t>(index, 0, numBuiltIn_ - 1);
	}
	// A kit loaded straight from a preset file is named after it
	if (!*name) {
		name = kit->name.get();
	}
	for (int32_t i = 0; i < numPresets_; i++) {
		if (!strcasecmp(labels_[i].data(), name)) {
			return firstPresetOption() + i;
		}
	}
	// As SHIFT + KIT made it
	return kit->presetName.isEmpty() ? defaultOption() : -1;
}

void KitPresets::readPresetList() {
	numPresets_ = 0;
	DIR dir;
	if (f_opendir(&dir, preset_kit::folderFor(kit_)) != FR_OK) {
		// The Deluge can't make folders when saving, so make this one ready to save presets into
		f_mkdir(preset_kit::folderFor(kit_));
		return;
	}
	FILINFO fileInfo;
	while (numPresets_ < kMaxPresets && f_readdir(&dir, &fileInfo) == FR_OK && fileInfo.fname[0]) {
		const size_t length = strlen(fileInfo.fname);
		if ((fileInfo.fattrib & AM_DIR) || fileInfo.fname[0] == '.' || length < 5 || length >= kMaxNameLength
		    || strcasecmp(fileInfo.fname + length - 4, ".XML") != 0) {
			continue;
		}
		strcpy(fileNames_[numPresets_].data(), fileInfo.fname);
		strcpy(labels_[numPresets_].data(), fileInfo.fname);
		labels_[numPresets_][length - 4] = 0;
		numPresets_++;
	}
	f_closedir(&dir);

	// In name order, so they come round the same way every time
	for (int32_t i = 1; i < numPresets_; i++) {
		for (int32_t j = i; j > 0 && strcasecmp(labels_[j - 1].data(), labels_[j].data()) > 0; j--) {
			std::swap(fileNames_[j - 1], fileNames_[j]);
			std::swap(labels_[j - 1], labels_[j]);
		}
	}
}

char const* KitPresets::getTitle() {
	return l10n::get(l10n::String::STRING_FOR_KIT_PRESETS);
}

std::span<char const*> KitPresets::getOptions() {
	return {options_.data(), static_cast<size_t>(numOptions())};
}

void KitPresets::selectEncoderAction(int8_t offset) {
	ContextMenu::selectEncoderAction(offset);
	preview(currentOption);
}

void KitPresets::preview(int32_t option) {
	if (option == previewing_) {
		return;
	}
	if (option == home_) {
		restoreAll();
		refresh();
		return;
	}

	// Read the whole preset before touching the kit, so a file that won't load leaves it as it was
	int32_t count = 0;
	Error error = Error::NONE;
	if (option >= defaultOption() && option < firstPresetOption()) {
		const auto rows = preset_kit::rowsFor(kit_);
		for (; count < static_cast<int32_t>(rows.size()) && count < kMaxRows; count++) {
			loaded_[count] =
			    preset_kit::makeDrum(rows[count], kit_, loadedParamManagers_[count], option - defaultOption());
			if (loaded_[count] == nullptr) {
				error = Error::INSUFFICIENT_RAM;
				break;
			}
		}
	}
	else {
		count = preset_kit::loadDrums(kit_, fileNames_[option - firstPresetOption()].data(), loaded_,
		                              loadedParamManagers_, &error);
	}
	if (count == 0) {
		display->displayError(error);
		return;
	}

	// The undo history can't follow sounds being swapped under it
	actionLogger.deleteAllLogs();
	InstrumentClip* clip = getCurrentInstrumentClip();
	Kit* kit = getCurrentKit();
	for (int32_t r = 0; r < numRows_; r++) {
		if (r >= count) {
			restoreRow(r);
			continue;
		}
		auto* onRow = static_cast<SoundDrum*>(clip->noteRows.getElement(rows_[r])->drum);
		const bool original = originals_[r] == nullptr;
		preset_kit::swapDrum(kit, clip, rows_[r], loaded_[r], &loadedParamManagers_[r], original);
		if (original) {
			originals_[r] = onRow;
		}
	}
	// More sounds in the file than preset rows here: the rest aren't used
	for (int32_t r = numRows_; r < count; r++) {
		preset_kit::deleteDrum(loaded_[r]);
		loadedParamManagers_[r].destructAndForgetParamCollections();
	}
	previewing_ = option;
	refresh();
}

void KitPresets::restoreRow(int32_t row) {
	if (originals_[row] == nullptr) {
		return;
	}
	preset_kit::swapDrum(getCurrentKit(), getCurrentInstrumentClip(), rows_[row], originals_[row], nullptr, false);
	originals_[row] = nullptr;
}

void KitPresets::restoreAll() {
	for (int32_t r = 0; r < numRows_; r++) {
		restoreRow(r);
	}
	previewing_ = home_;
}

void KitPresets::keep() {
	for (int32_t r = 0; r < numRows_; r++) {
		if (originals_[r] != nullptr) {
			preset_kit::deleteDrum(originals_[r]);
			originals_[r] = nullptr;
		}
	}
	home_ = previewing_;
}

void KitPresets::refresh() {
	view.setActiveModControllableTimelineCounter(getCurrentInstrumentClip());
	view.setKnobIndicatorLevels();
	uiNeedsRendering(&instrumentClipView);
	// Stepping with SELECT, the menu closed: the preset's name over the view
	if (getCurrentUI() != this) {
		if (display->haveOLED()) {
			display->displayPopup(options_[previewing_]);
		}
		else {
			display->setScrollingText(options_[previewing_]);
		}
		return;
	}
	if (display->haveOLED()) {
		renderUIsForOled();
	}
	else {
		drawCurrentOption();
	}
}

void KitPresets::remember(int32_t option) {
	Kit* kit = getCurrentKit();
	if (option == defaultOption()) {
		kit->presetName.set("*");
	}
	else if (option > defaultOption() && option < firstPresetOption()) {
		char builtIn[8];
		snprintf(builtIn, sizeof(builtIn), "*%d", static_cast<int>(option - defaultOption() + 1));
		kit->presetName.set(builtIn);
	}
	else if (option >= firstPresetOption()) {
		kit->presetName.set(labels_[option - firstPresetOption()].data());
	}
}

bool KitPresets::acceptCurrentOption() {
	preview(currentOption);
	remember(currentOption);
	keep();
	getCurrentKit()->beenEdited();
	display->setNextTransitionDirection(-1);
	close();
	return true;
}

ActionResult KitPresets::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	// BACK puts the kit back as it was and goes up to the Kit / LXR / Cycles / GIN menu
	if (b == deluge::hid::button::BACK) {
		if (on) {
			if (inCardRoutine) {
				return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
			}
			restoreAll();
			refresh();
			display->setNextTransitionDirection(-1);
			close();
			if (newKitType.setupFor(kit_)) {
				openUI(&newKitType);
			}
		}
		return ActionResult::DEALT_WITH;
	}
	return ContextMenu::buttonAction(b, on, inCardRoutine);
}

ActionResult KitPresets::padAction(int32_t x, int32_t y, int32_t velocity) {
	// Audition pads play the rows, so presets can be heard without the pattern running. Other pads do nothing.
	if (x == kDisplayWidth + 1) {
		if (sdRoutineLock) {
			return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
		}
		NoteRow* noteRow = getCurrentInstrumentClip()->getNoteRowOnScreen(y, currentSong);
		if (velocity == 0 || (noteRow != nullptr && noteRow->drum != nullptr)) {
			return instrumentClipView.padAction(x, y, velocity);
		}
	}
	return ActionResult::DEALT_WITH;
}

} // namespace deluge::gui::context_menu
