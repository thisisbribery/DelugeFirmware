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

namespace deluge::gui::context_menu {

/// Shown by SHIFT + SYNTH on a synth track: the synth instruments (see model/instrument/synth_instruments.h), opening
/// on the one the track plays. SHIFT + SYNTH again or turning SELECT moves on. SELECT or SYNTH loads the instrument's
/// first preset (a blank one if its folder is empty), BACK cancels. DX7 is only offered with the DX7 engine on.
class SynthInstrumentMenu final : public ContextMenu {
public:
	SynthInstrumentMenu() = default;

	/// Put the cursor on the instrument the current synth plays
	bool setupAndCheckAvailability() override;

	char const* getTitle() override;
	std::span<char const*> getOptions() override;
	bool isCurrentOptionAvailable() override;
	bool canSeeViewUnderneath() override { return true; }
	bool acceptCurrentOption() override;
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	deluge::hid::Button getAcceptButton() override { return deluge::hid::button::SYNTH; }
};

extern SynthInstrumentMenu synthInstrumentMenu;
} // namespace deluge::gui::context_menu
