/*
 * Copyright © 2026 Synthstrom Audible Limited
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

#include "gui/views/kit_pattern_mode.h"
#include "fatfs/ff.h"
#include "gui/ui/save/save_pattern_ui.h"
#include "gui/views/instrument_clip_view.h"
#include "gui/views/view.h"
#include "hid/display/display.h"
#include "model/action/action_logger.h"
#include "model/clip/instrument_clip.h"
#include "model/note/note_row.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "storage/storage_manager.h"
#include <cstdio>
#include <cstring>

namespace deluge::gui::kit_pattern {

KitPatternMode kitPatternMode{};

namespace {

constexpr char const* kRootFolder = "PATTERNS/RHYTHMIC/STUMBLE";

// In the module's order, bottom row first. These are also the track folder names.
constexpr char const* kTrackNames[kNumTracks] = {"KICK", "SNARE", "HIHAT1", "HIHAT2", "PERC1", "PERC2"};
constexpr char const* kTrackShortNames[kNumTracks] = {"BD", "SD", "H1", "H2", "P1", "P2"};

int32_t wrap(int32_t value, int32_t size) {
	return ((value % size) + size) % size;
}

} // namespace

bool KitPatternMode::isChosenInSettings() {
	return runtimeFeatureSettings.get(RuntimeFeatureSettingType::SyncScalingAction)
	       == RuntimeFeatureStateSyncScalingAction::Pattern;
}

bool KitPatternMode::isActiveHere() {
	return on_ && isChosenInSettings() && getCurrentUI() == &instrumentClipView
	       && getCurrentOutputType() == OutputType::KIT;
}

void KitPatternMode::toggle() {
	on_ = !on_;
	browsedBank_ = -1;
	if (on_) {
		if (!bank_[0] && readBanks()) {
			strcpy(bank_, banks_[0]);
		}
		char text[64];
		snprintf(text, sizeof(text), "Kit pattern mode\n%s", bank_[0] ? bank_ : "No pattern banks");
		show(text, "PATT");
	}
	else {
		show("Kit pattern mode off", "OFF");
	}
	view.setTimeBaseScaleLedState();
}

int32_t KitPatternMode::readBanks() {
	numBanks_ = 0;
	DIR dir;
	if (f_opendir(&dir, kRootFolder) != FR_OK) {
		return 0;
	}
	FILINFO fileInfo;
	while (numBanks_ < kMaxBanks && f_readdir(&dir, &fileInfo) == FR_OK && fileInfo.fname[0]) {
		if (!(fileInfo.fattrib & AM_DIR) || fileInfo.fname[0] == '.') {
			continue;
		}
		strncpy(banks_[numBanks_], fileInfo.fname, kMaxBankNameLength - 1);
		banks_[numBanks_][kMaxBankNameLength - 1] = 0;
		numBanks_++;
	}
	f_closedir(&dir);

	// Alphabetical, so banks come round in the same order every time
	for (int32_t i = 1; i < numBanks_; i++) {
		for (int32_t j = i; j > 0 && strcasecmp(banks_[j - 1], banks_[j]) > 0; j--) {
			char swap[kMaxBankNameLength];
			strcpy(swap, banks_[j]);
			strcpy(banks_[j], banks_[j - 1]);
			strcpy(banks_[j - 1], swap);
		}
	}
	return numBanks_;
}

void KitPatternMode::stepTrackPattern(int32_t noteRowIndex, int32_t offset) {
	if (noteRowIndex < 0 || noteRowIndex >= kNumTracks) {
		show("Patterns are on the bottom 6 rows", "NONE");
		return;
	}
	if (!bank_[0]) {
		show("No pattern banks", "NOBK");
		return;
	}
	slot_[noteRowIndex] = wrap(slot_[noteRowIndex] + offset, kNumSlots);
	Action* action = actionLogger.getNewAction(ActionType::PATTERN_PASTE, ActionAddition::NOT_ALLOWED);
	bool found = loadTrack(noteRowIndex, action);
	actionLogger.closeAction(ActionType::PATTERN_PASTE);
	showTrack(noteRowIndex, !found);
}

void KitPatternMode::browseBanks(int32_t offset) {
	if (!readBanks()) {
		show("No pattern banks", "NOBK");
		return;
	}
	if (browsedBank_ < 0 || browsedBank_ >= numBanks_) {
		browsedBank_ = 0;
		for (int32_t i = 0; i < numBanks_; i++) {
			if (!strcasecmp(banks_[i], bank_)) {
				browsedBank_ = i;
			}
		}
	}
	browsedBank_ = wrap(browsedBank_ + offset, numBanks_);
	char text[64];
	snprintf(text, sizeof(text), "Bank: %s\nPress SELECT to load", banks_[browsedBank_]);
	show(text, banks_[browsedBank_]);
}

void KitPatternMode::loadBank() {
	if (browsedBank_ >= 0 && browsedBank_ < numBanks_) {
		strcpy(bank_, banks_[browsedBank_]);
	}
	browsedBank_ = -1;
	if (!bank_[0]) {
		show("No pattern banks", "NOBK");
		return;
	}
	InstrumentClip* clip = getCurrentInstrumentClip();
	Action* action = actionLogger.getNewAction(ActionType::PATTERN_PASTE, ActionAddition::NOT_ALLOWED);
	for (int32_t track = 0; track < kNumTracks && track < clip->noteRows.getNumElements(); track++) {
		loadTrack(track, action);
	}
	actionLogger.closeAction(ActionType::PATTERN_PASTE);
	char text[64];
	snprintf(text, sizeof(text), "%s loaded", bank_);
	show(text, bank_);
}

bool KitPatternMode::loadTrack(int32_t track, Action* action) {
	char path[160];
	snprintf(path, sizeof(path), "%s/%s/%s/PATTERN%03d.XML", kRootFolder, bank_, kTrackNames[track],
	         (int)slot_[track] + 1);
	FilePointer filePointer;
	if (StorageManager::fileExists(path, &filePointer)) {
		Error error = StorageManager::loadRowPatternFile(&filePointer, track, action);
		if (error != Error::NONE) {
			display->displayError(error);
		}
		return true;
	}
	// An empty slot is an empty pattern
	instrumentClipView.applyRowPattern(track, nullptr, 0, action);
	return false;
}

bool KitPatternMode::setUpTrackSave(int32_t noteRowIndex) {
	if (noteRowIndex < 0 || noteRowIndex >= kNumTracks) {
		show("Patterns are on the bottom 6 rows", "NONE");
		return false;
	}
	if (!bank_[0]) {
		show("Make a bank folder in PATTERNS/RHYTHMIC/STUMBLE first", "NOBK");
		return false;
	}
	InstrumentClip* clip = getCurrentInstrumentClip();
	NoteRow* noteRow = clip->noteRows.getElement(noteRowIndex);
	int32_t rowLength = noteRow->loopLengthIfIndependent ? noteRow->loopLengthIfIndependent : clip->loopLength;

	char dir[128];
	snprintf(dir, sizeof(dir), "%s/%s/%s", kRootFolder, bank_, kTrackNames[noteRowIndex]);
	char fileName[16];
	snprintf(fileName, sizeof(fileName), "PATTERN%03d", (int)slot_[noteRowIndex] + 1);

	// The save writes the selected drum's row
	instrumentClipView.setSelectedDrum(noteRow->drum, true);
	savePatternUI.setupRowPattern(dir, fileName, rowLength);
	return true;
}

void KitPatternMode::showTrack(int32_t track, bool empty) {
	char text[64];
	snprintf(text, sizeof(text), "%s\n%s %d%s", bank_, kTrackNames[track], (int)slot_[track] + 1,
	         empty ? " (empty)" : "");
	char shortText[8];
	snprintf(shortText, sizeof(shortText), "%s%02d", kTrackShortNames[track], (int)slot_[track] + 1);
	show(text, shortText);
}

void KitPatternMode::show(char const* oledText, char const* sevenSegText) {
	if (display->haveOLED()) {
		display->popupTextTemporary(oledText);
	}
	else {
		display->displayPopup(sevenSegText);
	}
}

} // namespace deluge::gui::kit_pattern
