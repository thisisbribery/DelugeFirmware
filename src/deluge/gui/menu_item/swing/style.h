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
#pragma once
#include "gui/menu_item/selection.h"
#include "model/song/song.h"
#include "model/swing/swing_style.h"

namespace deluge::gui::menu_item::swing {

/// Picks the Stumble style. Only shown while Stumble is on.
class Style final : public Selection {
public:
	using Selection::Selection;

	// Option 0 is the first style after Classic
	void readCurrentValue() override { this->setValue(util::to_underlying(currentSong->stumbleStyle) - 1); }
	void writeCurrentValue() override {
		currentSong->changeStumbleStyle(static_cast<deluge::swing_style::Style>(this->getValue() + 1));
	}
	deluge::vector<std::string_view> getOptions(OptType optType) override {
		deluge::vector<std::string_view> options;
		for (int32_t i = 1; i < deluge::swing_style::kNumStyles; i++) {
			options.push_back(deluge::swing_style::getName(static_cast<deluge::swing_style::Style>(i)));
		}
		return options;
	}
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return currentSong->stumble;
	}
};

} // namespace deluge::gui::menu_item::swing
