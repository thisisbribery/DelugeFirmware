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
#include "gui/context_menu/kit_presets.h"
#include "gui/l10n/l10n.h"
#include "gui/views/instrument_clip_view.h"
#include "hid/display/display.h"
#include "model/instrument/kit.h"
#include "model/instrument/preset_kit.h"
#include "model/song/song.h"

namespace deluge::gui::context_menu {

NewKitType newKitType{};

char const* NewKitType::getTitle() {
	return l10n::get(l10n::String::STRING_FOR_NEW_KIT_TYPE);
}

std::span<char const*> NewKitType::getOptions() {
	using enum l10n::String;
	static char const* options[] = {l10n::get(STRING_FOR_SAMPLE_KIT), l10n::get(STRING_FOR_LXR_KIT),
	                                l10n::get(STRING_FOR_CYCLES_KIT)};
	return {options, 3};
}

bool NewKitType::setupFor(dsp::drum_synth::PresetKit kit) {
	using dsp::drum_synth::PresetKit;
	currentOption = (kit == PresetKit::LXR) ? 1 : (kit == PresetKit::CYCLES) ? 2 : 0;
	scrollPos = currentOption;
	return true;
}

bool NewKitType::acceptCurrentOption() {
	using dsp::drum_synth::PresetKit;
	const int32_t option = currentOption;
	const PresetKit chosen = (option == 1) ? PresetKit::LXR : (option == 2) ? PresetKit::CYCLES : PresetKit::NONE;
	const PresetKit current =
	    (getCurrentOutputType() == OutputType::KIT) ? preset_kit::typeOf(getCurrentKit()) : PresetKit::NONE;

	display->setNextTransitionDirection(-1);
	close();

	// The kit's own type: back into its presets
	if (chosen != PresetKit::NONE && chosen == current) {
		if (kitPresets.setup(current)) {
			openUI(&kitPresets);
		}
		return true;
	}

	// A preset kit with rows added to it can't be swapped for another kit: the added rows would lose their sounds
	if (current != PresetKit::NONE && InstrumentClipView::presetKitHasAddedRows(getCurrentKit())) {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_PRESET_KIT_HAS_ADDED_ROWS));
		return true;
	}

	switch (chosen) {
	case PresetKit::LXR:
	case PresetKit::CYCLES:
		instrumentClipView.createNewDrumSynthKit(chosen);
		break;
	default:
		instrumentClipView.createNewInstrument(OutputType::KIT);
		break;
	}
	return true;
}

} // namespace deluge::gui::context_menu
