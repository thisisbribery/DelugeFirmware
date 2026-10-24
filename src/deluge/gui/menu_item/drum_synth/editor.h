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

#include "definitions_cxx.hpp"
#include "dsp/drum_synth/drum_synth.h"
#include "gui/colour/rgb.h"
#include "gui/menu_item/menu_item.h"
#include "gui/menu_item/value.h"
#include "util/misc.h"
#include <array>
#include <span>

namespace deluge::gui::menu_item::drum_synth {

/// One column of the drum synth pad editor: a menu item edited by tapping a pad for its level
struct Column {
	enum class Kind : uint8_t { NUMBER, CHOICE };
	enum class Group : uint8_t { OSC, PITCH, MOD, MOD2, NOISE, TRANSIENT, SOUND, XIA_ENVELOPE };

	Value<int32_t>* item;
	Kind kind;
	Group group;
	/// Value range the 8 pad rows cover. When padMin == padMax the item's own range is used.
	int32_t padMin = 0;
	int32_t padMax = 0;
	/// How far one click of SELECT moves the value
	int32_t step = 1;
	/// Shown value = value / displayDivisor (transpose is stored in cents)
	int32_t displayDivisor = 1;
};

/// The drum synth's fast editor. While it's open the 16 x 8 pad grid shows one setting per column as a level meter:
/// tap a pad to set that level, turn SELECT to fine tune the last column touched, and turn the horizontal encoder to
/// move between columns. The audition pads still play the row. SELECT press opens the full drum synth menu, BACK goes
/// up to the normal sound menu.
class Editor final : public MenuItem {
public:
	using Layout = std::span<const Column>;

	Editor(l10n::String name, Layout drumLayout, Layout snareLayout, Layout metalLayout, Layout tiaLayout,
	       Layout iiaLayout, Layout cycLayout, MenuItem* fullMenu)
	    : MenuItem(name), layouts_{drumLayout, snareLayout, metalLayout, tiaLayout, iiaLayout, cycLayout},
	      fullMenu_(fullMenu) {}

	void beginSession(MenuItem* navigatedBackwardFrom) override;
	void endSession() override;
	void readValueAgain() override;
	void selectEncoderAction(int32_t offset) override;
	void horizontalEncoderAction(int32_t offset) override;
	bool selectEncoderActionEditsInstrument() override { return true; }
	MenuItem* selectButtonPress() override { return fullMenu_; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override;
	[[nodiscard]] std::string_view getTitle() const override;

	void drawPixelsForOled() override;
	void drawValue();

	/// Called by the SoundEditor while this is the current menu item
	void padAction(int32_t x, int32_t y);
	void renderPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]);

private:
	[[nodiscard]] Layout layout() const;
	void clampSelectedColumn();
	int32_t readColumn(const Column& column);
	void writeColumn(const Column& column, int32_t value);
	static int32_t lowest(const Column& column);
	static int32_t highest(const Column& column);
	static int32_t padLow(const Column& column);
	static int32_t padHigh(const Column& column);
	void changed();

	/// LXR drum, snare and metal, TIA, IIA, then one shared by all the Cyc engines
	std::array<Layout, 6> layouts_;
	MenuItem* fullMenu_;
	int32_t selectedColumn_ = 0;
};

extern Editor drumSynthEditor;

} // namespace deluge::gui::menu_item::drum_synth
