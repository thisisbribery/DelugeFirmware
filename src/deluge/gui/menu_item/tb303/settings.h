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
#include "gui/l10n/l10n.h"
#include "gui/menu_item/patched_param/integer.h"
#include "gui/menu_item/selection.h"
#include "gui/menu_item/submenu.h"
#include "gui/ui/sound_editor.h"
#include "processing/sound/sound.h"

// The 303 synth mode's settings
namespace deluge::gui::menu_item::tb303 {

inline bool isTb303(ModControllableAudio* modControllable) {
	return modControllable != nullptr && modControllable->isTb303();
}

/// One of the 303's knobs
class Param final : public patched_param::Integer {
public:
	using Integer::Integer;
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return isTb303(modControllable);
	}
};

/// The filter choices, in FilterMode order: the TB-303 ladder model, Open303's plain 18 dB lowpass, the ladder with
/// dirt
inline deluge::vector<std::string_view> filterOptions() {
	return {l10n::getView(l10n::String::STRING_FOR_TB303), l10n::getView(l10n::String::STRING_FOR_TB303_FILTER_LP18),
	        l10n::getView(l10n::String::STRING_FOR_TB303_FILTER_DIRT)};
}

class Filter final : public Selection {
public:
	using Selection::Selection;
	void readCurrentValue() override { this->setValue((int32_t)soundEditor.currentSound->tb303FilterMode); }
	void writeCurrentValue() override {
		soundEditor.currentSound->tb303FilterMode = (deluge::dsp::tb303::FilterMode)this->getValue();
	}
	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		return filterOptions();
	}
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return isTb303(modControllable);
	}
};

class Menu final : public Submenu {
public:
	using Submenu::Submenu;
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return isTb303(modControllable);
	}
};

} // namespace deluge::gui::menu_item::tb303
