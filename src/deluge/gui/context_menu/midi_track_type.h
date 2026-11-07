/*
 * Copyright © 2026 thisisbribery
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
#include "model/instrument/midi_kit.h"

namespace deluge::gui::context_menu {

/// Shown by SHIFT + MIDI in clip view: a MIDI track (the default), or MKIT, a kit of sixteen MIDI rows that kit
/// patterns and Autofill play. Picking MKIT then asks for its layout (see model/instrument/midi_kit.h).
/// SHIFT + MIDI again or turning SELECT moves on, SELECT or MIDI accepts, BACK cancels.
class MIDITrackType final : public ContextMenu {
public:
	MIDITrackType() = default;

	bool setupAndCheckAvailability() override;

	char const* getTitle() override;
	std::span<char const*> getOptions() override;
	bool canSeeViewUnderneath() override { return true; }
	bool acceptCurrentOption() override;
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	deluge::hid::Button getAcceptButton() override { return deluge::hid::button::MIDI; }

private:
	bool choosingLayout_ = false;
};

extern MIDITrackType midiTrackType;
} // namespace deluge::gui::context_menu
