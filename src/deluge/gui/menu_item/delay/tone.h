/*
 * Copyright (c) 2026 thisisbribery
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

#include "gui/menu_item/unpatched_param.h"
#include "gui/menu_item/value_scaling.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/display.h"
#include "modulation/params/param_manager.h"
#include "modulation/params/param_set.h"

namespace deluge::gui::menu_item::delay {

/// Delay tone, -25 to +25: lowpass to the left, flat at 0, highpass to the right
class Tone final : public UnpatchedParam {
public:
	using UnpatchedParam::UnpatchedParam;

	void drawValue() override {
		DEF_STACK_STRING_BUF(text, 8);
		int32_t value = getValue();
		if (value == 0) {
			text.append("FLAT");
		}
		else {
			text.append(value < 0 ? "L" : "H");
			text.appendInt(std::abs(value));
		}
		display->setText(text.c_str(), true);
	}

	void getColumnLabel(StringBuf& label) override { label.append(l10n::get(l10n::String::STRING_FOR_TONE_SHORT)); }

protected:
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxMenuRelativeValue; }
	[[nodiscard]] int32_t getMinValue() const override { return kMinMenuRelativeValue; }
	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return PAN; }
	int32_t getFinalValue() override { return computeFinalValueForPan(getValue()); }
	void readCurrentValue() override {
		setValue(computeCurrentValueForPan(soundEditor.currentParamManager->getUnpatchedParamSet()->getValue(getP())));
	}
};
} // namespace deluge::gui::menu_item::delay
