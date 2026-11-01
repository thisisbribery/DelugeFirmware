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
#include "model/instrument/kit.h"
#include "model/song/song.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"

namespace deluge::gui::menu_item {

/// Kit gate out: rows 1-4 of the current kit also fire gate outs 1-4. For the whole kit, so it shows on any drum synth
/// row and in the kit's own menu. Switching it on takes the gates from any other kit.
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
			soundEditor.markInstrumentAsEdited();
		}
	}
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		if (getCurrentKit() == nullptr) {
			return false;
		}
		if (soundEditor.editingKitRow()) {
			const auto* sound = static_cast<Sound*>(modControllable);
			return sound->sources[0].oscType == OscType::DRUM_SYNTH;
		}
		return true;
	}
};

/// Which gate this kit row fires while Gate out is on: Auto (rows 1-4 = gates 1-4), Off, or gate 1-4
class KitRowGate final : public Selection {
public:
	using Selection::Selection;
	void readCurrentValue() override {
		this->setValue(static_cast<SoundDrum*>(soundEditor.currentSound)->gateOutChoice);
	}
	void writeCurrentValue() override {
		static_cast<SoundDrum*>(soundEditor.currentSound)->gateOutChoice = this->getValue();
		soundEditor.markInstrumentAsEdited();
	}
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		const auto* sound = static_cast<Sound*>(modControllable);
		return getCurrentKit() != nullptr && soundEditor.editingKitRow()
		       && sound->sources[0].oscType == OscType::DRUM_SYNTH;
	}
	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {l10n::getView(STRING_FOR_AUTO), l10n::getView(STRING_FOR_OFF), "1", "2", "3", "4"};
	}
};

} // namespace deluge::gui::menu_item
