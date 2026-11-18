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

#include "gui/context_menu/context_menu.h"
#include "model/instrument/synth_instruments.h"
#include <array>

namespace deluge::gui::context_menu {

/// Shown by SHIFT + SYNTH on any instrument track, as SHIFT + KIT shows the kit types: the synth types (see
/// model/instrument/synth_instruments.h), opening on the one the track plays. SHIFT + SYNTH again or turning SELECT
/// moves on. The audition pads always have a synth to play: on a synth track, its own synth on its own type and each
/// other type's init synth; on any other track, the init synth of the type the cursor is on, from the moment the menu
/// opens. SELECT or SYNTH opens the type's categories (LEAD, BASS, PAD, MISC; DX7 adds KEYS and BELL); INIT keeps the
/// stock blank synth. BACK puts the track's old instrument back.
class SynthInstrumentMenu final : public ContextMenu {
public:
	SynthInstrumentMenu() = default;

	/// Put the cursor on the type the current synth plays. False on a kit track with notes or sounds (as stock).
	bool setupAndCheckAvailability() override;
	/// Open on type (from the category menu's BACK)
	void setupFor(size_t type);

	char const* getTitle() override;
	std::span<char const*> getOptions() override;
	bool isCurrentOptionAvailable() override;
	bool canSeeViewUnderneath() override { return true; }
	bool acceptCurrentOption() override;
	void selectEncoderAction(int8_t offset) override;
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	bool getGreyoutColsAndRows(uint32_t* cols, uint32_t* rows) override;
	deluge::hid::Button getAcceptButton() override { return deluge::hid::button::SYNTH; }
};

/// The categories inside a synth type's folder (BASS, LEAD, PAD, MISC; BROPHET's are Pro-53's files, BNK1 to BNK8),
/// each one offered only if it holds presets. The audition pads play the type's init synth. SELECT or SYNTH opens the
/// preset browser in that folder, still playing the init until another preset is highlighted (SELECT straight away
/// loads the one shown): turning SELECT previews the presets, SELECT keeps one, BACK there returns here playing the
/// init again. BACK here returns to the types.
class SynthCategoryMenu final : public ContextMenu {
public:
	SynthCategoryMenu() = default;

	/// For type: false if none of its categories has a preset
	bool setupFor(size_t type);

	char const* getTitle() override;
	std::span<char const*> getOptions() override;
	bool isCurrentOptionAvailable() override {
		return currentOption < static_cast<int32_t>(hasPresets_.size()) && hasPresets_[currentOption];
	}
	bool canSeeViewUnderneath() override { return true; }
	bool acceptCurrentOption() override;
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	bool getGreyoutColsAndRows(uint32_t* cols, uint32_t* rows) override;
	deluge::hid::Button getAcceptButton() override { return deluge::hid::button::SYNTH; }

	size_t type() const { return type_; }

private:
	size_t type_ = 0;
	// The category last opened, to come back onto it
	int32_t lastType_ = -1;
	int32_t lastCategory_ = -1;
	std::array<bool, synth_instruments::kMaxSynthCategories> hasPresets_{};
};

/// Put type's init synth on the current clip for the SHIFT + SYNTH picker (its mode or engine set as the type plays).
/// False if it couldn't.
bool previewSynthInit(size_t type);
/// Back from the picker's preset browser: type's init synth on the clip, its categories open (or the types, if it has
/// none)
void returnToSynthPicker(size_t type);

extern SynthInstrumentMenu synthInstrumentMenu;
extern SynthCategoryMenu synthCategoryMenu;
} // namespace deluge::gui::context_menu
