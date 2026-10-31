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

#include "gui/menu_item/drum_synth/editor.h"
#include "gui/colour/palette.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/enumeration.h"
#include "gui/menu_item/number.h"
#include "gui/menu_item/selection.h"
#include "gui/ui/sound_editor.h"
#include "gui/ui/ui.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "hid/led/pad_leds.h"
#include "processing/sound/drum_synth_params.h"
#include "processing/sound/sound.h"
#include "util/cfunctions.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace deluge::gui::menu_item::drum_synth {

using deluge::dsp::drum_synth::Engine;

namespace {

constexpr int32_t kRows = kDisplayHeight;
constexpr int32_t kTopRow = kRows - 1;

RGB groupColour(Column::Group group) {
	switch (group) {
	case Column::Group::OSC:
		return RGB(200, 200, 200);
	case Column::Group::PITCH:
		return RGB(255, 90, 0);
	case Column::Group::MOD:
		return RGB(0, 190, 255);
	case Column::Group::MOD2:
		return RGB(70, 50, 255);
	case Column::Group::NOISE:
		return RGB(170, 255, 40);
	case Column::Group::TRANSIENT:
		return RGB(255, 0, 170);
	case Column::Group::XIA_ENVELOPE:
		return RGB(255, 200, 0);
	default:
		return RGB(0, 255, 70);
	}
}

RGB scaled(RGB colour, int32_t shift) {
	return colour.transform([shift](RGB::channel_type c) { return static_cast<RGB::channel_type>(c >> shift); });
}

/// A choice with up to one option per row is drawn as a single lit pad; anything else as a level meter
bool isDot(const Column& column, int32_t optionCount) {
	return column.kind == Column::Kind::CHOICE && optionCount <= kRows;
}

} // namespace

// Ranges -------------------------------------------------------------------------------------------------------------

int32_t Editor::lowest(const Column& column) {
	if (column.kind == Column::Kind::CHOICE) {
		return 0;
	}
	return static_cast<Number*>(column.item)->getMinValue();
}

int32_t Editor::highest(const Column& column) {
	if (column.kind == Column::Kind::CHOICE) {
		return static_cast<int32_t>(static_cast<Enumeration*>(column.item)->size()) - 1;
	}
	return static_cast<Number*>(column.item)->getMaxValue();
}

int32_t Editor::padLow(const Column& column) {
	return (column.padMin != column.padMax) ? column.padMin : lowest(column);
}

int32_t Editor::padHigh(const Column& column) {
	return (column.padMin != column.padMax) ? column.padMax : highest(column);
}

int32_t Editor::readColumn(const Column& column) {
	column.item->readCurrentValue();
	return column.item->getValue();
}

void Editor::writeColumn(const Column& column, int32_t value) {
	column.item->setValue(std::clamp(value, lowest(column), highest(column)));
	column.item->writeCurrentValue();
}

// Session ------------------------------------------------------------------------------------------------------------

Editor::Layout Editor::layout() const {
	const Sound* sound = soundEditor.currentSound;
	if (sound == nullptr) {
		return {};
	}
	const Engine engine = sound->sources[0].drumPatch.engine();
	if (deluge::dsp::drum_synth::isUltrakik(engine)) {
		return layouts_[7];
	}
	if (deluge::dsp::drum_synth::isGin(engine)) {
		return layouts_[6];
	}
	if (deluge::dsp::drum_synth::isCyc(engine)) {
		return layouts_[5];
	}
	return layouts_[util::to_underlying(engine)];
}

void Editor::clampSelectedColumn() {
	const int32_t size = static_cast<int32_t>(layout().size());
	selectedColumn_ = std::clamp<int32_t>(selectedColumn_, 0, std::max<int32_t>(size - 1, 0));
}

bool Editor::isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const {
	// Sound menus are only ever opened on a Sound
	const auto* sound = static_cast<const Sound*>(modControllable);
	return sound != nullptr && sound->sources[0].oscType == OscType::DRUM_SYNTH;
}

std::string_view Editor::getTitle() const {
	using enum l10n::String;
	const Sound* sound = soundEditor.currentSound;
	if (sound == nullptr) {
		return l10n::getView(STRING_FOR_DRUM_SYNTH);
	}
	const Engine engine = sound->sources[0].drumPatch.engine();
	if (deluge::dsp::drum_synth::isCyc(engine)) {
		return l10n::getView(deluge::dsp::drum_synth::cycEngineName(engine));
	}
	switch (engine) {
	case Engine::ULTRAKIK:
		return l10n::getView(STRING_FOR_DRUM_ENGINE_ULTRAKIK);
	case Engine::GIN:
		return l10n::getView(STRING_FOR_DRUM_ENGINE_GIN);
	case Engine::SNARE:
		return l10n::getView(STRING_FOR_DRUM_ENGINE_SNARE);
	case Engine::METAL:
		return l10n::getView(STRING_FOR_DRUM_ENGINE_METAL);
	case Engine::TIA:
		return l10n::getView(STRING_FOR_DRUM_ENGINE_TIA);
	case Engine::IIA:
		return l10n::getView(STRING_FOR_DRUM_ENGINE_IIA);
	default:
		return l10n::getView(STRING_FOR_DRUM_ENGINE_DRUM);
	}
}

void Editor::beginSession(MenuItem* navigatedBackwardFrom) {
	clampSelectedColumn();
	PadLEDs::reassessGreyout();
	uiNeedsRendering(&soundEditor, 0xFFFFFFFF, 0);
	readValueAgain();
}

void Editor::endSession() {
	MenuItem::endSession();
	PadLEDs::reassessGreyout();
	uiNeedsRendering(&soundEditor, 0xFFFFFFFF, 0);
}

void Editor::readValueAgain() {
	if (display->haveOLED()) {
		renderUIsForOled();
	}
	else {
		drawValue();
	}
}

void Editor::changed() {
	soundEditor.markInstrumentAsEdited();
	uiNeedsRendering(&soundEditor, 0xFFFFFFFF, 0);
	readValueAgain();
}

// Controls ------------------------------------------------------------------------------------------------------------

void Editor::padAction(int32_t x, int32_t y) {
	const Layout columns = layout();
	if (x < 0 || x >= static_cast<int32_t>(columns.size()) || y < 0 || y >= kRows) {
		return;
	}
	selectedColumn_ = x;
	const Column& column = columns[x];

	const int32_t low = padLow(column);
	const int32_t high = padHigh(column);
	int32_t value;
	if (isDot(column, high - low + 1)) {
		if (y > high - low) {
			readValueAgain(); // Off the end of the options: just select the column
			uiNeedsRendering(&soundEditor, 0xFFFFFFFF, 0);
			return;
		}
		value = low + y;
	}
	else {
		value = low + static_cast<int32_t>(std::lround(static_cast<double>(y) * (high - low) / kTopRow));
	}
	writeColumn(column, value);
	changed();
}

void Editor::selectEncoderAction(int32_t offset) {
	const Layout columns = layout();
	if (columns.empty()) {
		return;
	}
	clampSelectedColumn();
	const Column& column = columns[selectedColumn_];
	writeColumn(column, readColumn(column) + offset * column.step);
	changed();
}

void Editor::horizontalEncoderAction(int32_t offset) {
	const int32_t size = static_cast<int32_t>(layout().size());
	if (size == 0) {
		return;
	}
	selectedColumn_ = std::clamp<int32_t>(selectedColumn_ + offset, 0, size - 1);
	if (display->have7SEG()) {
		display->displayPopup(std::string{layout()[selectedColumn_].item->getName()}.c_str());
	}
	uiNeedsRendering(&soundEditor, 0xFFFFFFFF, 0);
	readValueAgain();
}

// Drawing -------------------------------------------------------------------------------------------------------------

void Editor::renderPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
                        uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]) {
	const Layout columns = layout();
	clampSelectedColumn();

	for (int32_t x = 0; x < kDisplayWidth; x++) {
		const bool used = x < static_cast<int32_t>(columns.size());
		RGB lit = colours::black;
		RGB unlit = colours::black;
		int32_t level = -1;
		int32_t optionCount = 0;
		bool dot = false;

		if (used) {
			const Column& column = columns[x];
			const RGB base = groupColour(column.group);
			const bool selected = (x == selectedColumn_);
			lit = selected ? base : scaled(base, 1);
			unlit = scaled(base, selected ? 3 : 4);

			const int32_t low = padLow(column);
			const int32_t high = padHigh(column);
			const int32_t value = readColumn(column);
			optionCount = high - low + 1;
			dot = isDot(column, optionCount);
			if (dot) {
				level = value - low;
			}
			else if (high > low) {
				const double position = static_cast<double>(std::clamp(value, low, high) - low) / (high - low);
				level = static_cast<int32_t>(std::lround(position * kTopRow));
			}
		}

		for (int32_t y = 0; y < kRows; y++) {
			if (!(whichRows & (1 << y))) {
				continue;
			}
			RGB colour = colours::black;
			if (used) {
				if (dot) {
					colour = (y == level) ? lit : ((y < optionCount) ? unlit : colours::black);
				}
				else {
					colour = (y <= level) ? lit : unlit;
				}
			}
			image[y][x] = colour;
			if (occupancyMask != nullptr) {
				occupancyMask[y][x] = (colour == colours::black) ? 0 : 64;
			}
		}
	}
}

void Editor::drawPixelsForOled() {
	using deluge::hid::display::OLED;
	const Layout columns = layout();
	if (columns.empty()) {
		return;
	}
	clampSelectedColumn();
	const Column& column = columns[selectedColumn_];
	const int32_t value = readColumn(column);

	auto& canvas = OLED::main;
	const int32_t nameY = OLED_MAIN_TOPMOST_PIXEL + 15;
	canvas.drawString(column.item->getTitle(), 0, nameY, kTextSpacingX, kTextSpacingY, 0, OLED_MAIN_WIDTH_PIXELS - 30);

	// Column number, so you can find it on the grid
	char position[8];
	intToString(selectedColumn_ + 1, position, 1);
	strcat(position, "/");
	char total[4];
	intToString(static_cast<int32_t>(columns.size()), total, 1);
	strcat(position, total);
	canvas.drawString(position, OLED_MAIN_WIDTH_PIXELS - static_cast<int32_t>(strlen(position)) * kTextSpacingX, nameY,
	                  kTextSpacingX, kTextSpacingY);

	const int32_t valueY = nameY + kTextSpacingY + 3;
	if (column.kind == Column::Kind::CHOICE) {
		auto options = static_cast<Selection*>(column.item)->getOptions(Selection::OptType::FULL);
		const auto option = options[std::clamp<int32_t>(value, 0, static_cast<int32_t>(options.size()) - 1)];
		canvas.drawString(option, 0, valueY, kTextBigSpacingX, kTextBigSizeY);
	}
	else {
		char text[12];
		intToString(value / column.displayDivisor, text, 1);
		canvas.drawString(text, 0, valueY, kTextBigSpacingX, kTextBigSizeY);

		// Level bar along the bottom
		const int32_t low = lowest(column);
		const int32_t high = highest(column);
		const int32_t barY = OLED_MAIN_HEIGHT_PIXELS - 7;
		canvas.drawRectangle(0, barY, OLED_MAIN_WIDTH_PIXELS - 1, barY + 5);
		if (high > low) {
			const int32_t width = (OLED_MAIN_WIDTH_PIXELS - 4) * (std::clamp(value, low, high) - low) / (high - low);
			if (width > 0) {
				canvas.invertArea(2, width, barY + 2, barY + 3);
			}
		}
	}
}

void Editor::drawValue() {
	const Layout columns = layout();
	if (columns.empty()) {
		return;
	}
	clampSelectedColumn();
	const Column& column = columns[selectedColumn_];
	const int32_t value = readColumn(column);
	if (column.kind == Column::Kind::CHOICE) {
		auto options = static_cast<Selection*>(column.item)->getOptions(Selection::OptType::SHORT);
		const auto option = options[std::clamp<int32_t>(value, 0, static_cast<int32_t>(options.size()) - 1)];
		display->setText(option);
	}
	else {
		char text[12];
		intToString(value / column.displayDivisor, text, 1);
		display->setText(text, true);
	}
}

} // namespace deluge::gui::menu_item::drum_synth
