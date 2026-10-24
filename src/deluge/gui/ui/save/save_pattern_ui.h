/*
 * Copyright © 2014-2023 Synthstrom Audible Limited
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
#include "gui/ui/save/save_ui.h"

class Song;

class SavePatternUI final : public SaveUI {
public:
	SavePatternUI();
	bool opened() override;
	/// Kit pattern mode: the next time this opens, it saves one whole kit row of rowLength into dir, offering fileName
	void setupRowPattern(char const* dir, char const* fileName, int32_t rowLength);

	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;

	void verticalEncoderAction(int32_t offset, bool encoderButtonPressed, bool shiftButtonPressed) {};
	void endSession() {};
	bool performSave(bool mayOverwrite) override;

	bool renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth] = nullptr,
	                   uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth] = nullptr) override {
		return true;
	}

	// ui
	UIType getUIType() override { return UIType::SAVE_PATTERN; }

private:
	bool selectedDrumOnly;
	std::string defaultDir;
	std::string rowPatternDir;
	std::string rowPatternFileName;
	int32_t rowPatternLength = 0; // Not 0 while saving a kit row for kit pattern mode
	bool rowPatternPending = false;
};

extern SavePatternUI savePatternUI;
