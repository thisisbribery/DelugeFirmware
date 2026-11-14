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
/// moves on. SELECT or SYNTH opens the type's categories (BASS, LEAD, PAD, MISC); INIT makes the stock blank synth.
/// BACK cancels. DX7 is only offered with the DX7 engine on.
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
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	deluge::hid::Button getAcceptButton() override { return deluge::hid::button::SYNTH; }
};

/// The categories inside a synth type's folder (BASS, LEAD, PAD, MISC; BROPHET's are Pro-53's files, BNK1 to BNK8),
/// each one offered only if it holds presets. SELECT or SYNTH opens the preset browser in that folder playing its first
/// preset: turning SELECT previews the others, SELECT keeps one, BACK there puts the old synth back and returns here.
/// BACK here returns to the types.
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
	deluge::hid::Button getAcceptButton() override { return deluge::hid::button::SYNTH; }

	size_t type() const { return type_; }

private:
	size_t type_ = 0;
	std::array<bool, synth_instruments::kMaxSynthCategories> hasPresets_{};
};

/// Open the preset browser on folder for the current clip, playing its first preset. backTo is the category menu's
/// type to return to on BACK from that folder (-1: BACK leaves as usual). False if it couldn't open.
bool openSynthPresetFolder(char const* folder, int32_t backTo);

extern SynthInstrumentMenu synthInstrumentMenu;
extern SynthCategoryMenu synthCategoryMenu;
} // namespace deluge::gui::context_menu
