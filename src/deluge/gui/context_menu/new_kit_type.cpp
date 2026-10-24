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

#include "gui/context_menu/new_kit_type.h"
#include "definitions_cxx.hpp"
#include "gui/l10n/l10n.h"
#include "gui/views/instrument_clip_view.h"
#include "hid/display/display.h"

namespace deluge::gui::context_menu {

NewKitType newKitType{};

char const* NewKitType::getTitle() {
	return l10n::get(l10n::String::STRING_FOR_NEW_KIT_TYPE);
}

std::span<char const*> NewKitType::getOptions() {
	using enum l10n::String;
	static char const* options[] = {l10n::get(STRING_FOR_SAMPLE_KIT), l10n::get(STRING_FOR_DRUM_SYNTH)};
	return {options, 2};
}

bool NewKitType::acceptCurrentOption() {
	const bool drumSynth = (currentOption == 1);

	display->setNextTransitionDirection(-1);
	close();

	if (drumSynth) {
		instrumentClipView.createNewDrumSynthKit();
	}
	else {
		instrumentClipView.createNewInstrument(OutputType::KIT);
	}
	return true;
}

} // namespace deluge::gui::context_menu
