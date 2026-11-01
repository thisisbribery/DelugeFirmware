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
#include "gui/menu_item/selection.h"
#include "gui/menu_item/toggle.h"
#include "gui/ui/sound_editor.h"
#include "model/drum/drum_gates.h"
#include "model/instrument/kit.h"
#include "model/song/song.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"

namespace deluge::gui::menu_item {

/// Kit gate out, in the kit's own menu: rows 1-4 of the current kit fire gate outs 1-4 (or each row's own pick).
/// Switching it on takes the gates from any other kit.
class KitGateOut final : public Toggle {
public:
	using Toggle::Toggle;
	void readCurrentValue() override {
		Kit* kit = getCurrentKit();
		this->setValue(kit != nullptr && kit->gateOut);
	}
	void writeCurrentValue() override {
		Kit* kit = getCurrentKit();
		if (kit != nullptr) {
			kit->setGateOut(this->getValue());
			// Every row had been switched off: start again from rows 1-4 on gates 1-4
			if (kit->gateOut && kit->gateOutMask() == 0) {
				kit->resetRowGates();
				drum_gates::takeGates(kit->gateOutMask());
			}
			soundEditor.markInstrumentAsEdited();
		}
	}
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return getCurrentKit() != nullptr;
	}
};

/// GATE, first in a kit row's menu: the gate out this row fires as it sounds, off or 1-4. Picking a gate on a kit
/// without gate out puts this row alone on the gates (taking them from any other kit).
class KitRowGate final : public Selection {
public:
	using Selection::Selection;
	void readCurrentValue() override {
		this->setValue(static_cast<SoundDrum*>(soundEditor.currentSound)->gateOutGate() + 1);
	}
	void writeCurrentValue() override {
		Kit* kit = getCurrentKit();
		if (kit != nullptr) {
			kit->setRowGate(static_cast<SoundDrum*>(soundEditor.currentSound), this->getValue() - 1);
			soundEditor.markInstrumentAsEdited();
		}
	}
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return getCurrentKit() != nullptr && soundEditor.editingKitRow();
	}
	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		return {l10n::getView(l10n::String::STRING_FOR_OFF), "1", "2", "3", "4"};
	}
};

} // namespace deluge::gui::menu_item
