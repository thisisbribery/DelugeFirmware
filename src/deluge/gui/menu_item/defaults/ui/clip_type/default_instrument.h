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
#include "gui/l10n/l10n.h"
#include "gui/menu_item/selection.h"
#include "model/instrument/synth_instruments.h"
#include "storage/flash_storage.h"

namespace deluge::gui::menu_item::defaults::ui::clip_type {

/// Which preset kit a new kit track starts as: Off (a stock kit), LXR, Cycles or GIN. Option n is PresetKit n.
class DefaultKitInstrument final : public Selection {
public:
	using Selection::Selection;
	void readCurrentValue() override { this->setValue(FlashStorage::defaultKitInstrument); }
	void writeCurrentValue() override { FlashStorage::defaultKitInstrument = this->getValue(); }
	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {l10n::getView(STRING_FOR_OFF), l10n::getView(STRING_FOR_LXR_KIT), l10n::getView(STRING_FOR_CYCLES_KIT),
		        l10n::getView(STRING_FOR_GIN_KIT)};
	}
};

/// Which of the SHIFT + SYNTH instruments a new synth track starts as, or Off (stock). Option n is instrument n - 1.
class DefaultSynthInstrument final : public Selection {
public:
	using Selection::Selection;
	void readCurrentValue() override { this->setValue(FlashStorage::defaultSynthInstrument); }
	void writeCurrentValue() override { FlashStorage::defaultSynthInstrument = this->getValue(); }
	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		deluge::vector<std::string_view> options{l10n::getView(l10n::String::STRING_FOR_OFF)};
		for (size_t i = 0; i < synth_instruments::kNumDefaultSynthInstruments; i++) {
			options.push_back(l10n::getView(synth_instruments::kSynthInstruments[i].name));
		}
		return options;
	}
};

} // namespace deluge::gui::menu_item::defaults::ui::clip_type
