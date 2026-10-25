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

/// Shown by SHIFT + KIT in clip view: make a normal (sample) kit, a kit of the seven LXR voices, or a kit of the six
/// Cyc engines in the Cycles' usual layout. Every other drum engine is a SYNTH MODE choice on any row.
/// SELECT or KIT accepts, BACK cancels.
class NewKitType final : public ContextMenu {
public:
	NewKitType() = default;

	char const* getTitle() override;
	std::span<char const*> getOptions() override;
	bool canSeeViewUnderneath() override { return true; }
	bool acceptCurrentOption() override;
	deluge::hid::Button getAcceptButton() override { return deluge::hid::button::KIT; }
};

extern NewKitType newKitType;
} // namespace deluge::gui::context_menu
