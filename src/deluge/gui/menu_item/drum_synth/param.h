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

#include "dsp/drum_synth/drum_synth.h"
#include "gui/menu_item/integer.h"
#include "gui/menu_item/selection.h"
#include "gui/ui/sound_editor.h"
#include "processing/sound/sound.h"

namespace deluge::gui::menu_item::drum_synth {

using deluge::dsp::drum_synth::Patch;

/// The drum synth always lives on osc 1
inline Patch& currentPatch() {
	return soundEditor.currentSound->sources[0].drumPatch;
}

/// A drum synth setting edited as its raw 0-127 value, the way the LXR shows it
class Value final : public Integer {
public:
	Value(l10n::String name, Patch::Param param) : Integer(name), param_(param) {}

	void readCurrentValue() override { setValue(currentPatch().get(param_)); }
	void writeCurrentValue() override { currentPatch().set(param_, getValue()); }
	[[nodiscard]] int32_t getMaxValue() const override { return Patch::maxValue(param_); }

private:
	Patch::Param param_;
};

class Wave final : public Selection {
public:
	Wave(l10n::String name, Patch::Param param) : Selection(name), param_(param) {}

	void readCurrentValue() override { setValue(currentPatch().get(param_)); }
	void writeCurrentValue() override { currentPatch().set(param_, getValue()); }

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {l10n::getView(STRING_FOR_SINE), l10n::getView(STRING_FOR_TRIANGLE), l10n::getView(STRING_FOR_SAW),
		        l10n::getView(STRING_FOR_SQUARE), l10n::getView(STRING_FOR_NOISE)};
	}

private:
	Patch::Param param_;
};

class MixMode final : public Selection {
public:
	using Selection::Selection;

	void readCurrentValue() override { setValue(currentPatch().get(Patch::MIX_MODE)); }
	void writeCurrentValue() override { currentPatch().set(Patch::MIX_MODE, getValue()); }

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {l10n::getView(STRING_FOR_FM), l10n::getView(STRING_FOR_DRUM_MIX)};
	}
};

} // namespace deluge::gui::menu_item::drum_synth
