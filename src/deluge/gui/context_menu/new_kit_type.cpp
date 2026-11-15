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
#include "gui/menu_item/drum_synth/param.h"
#include "gui/menu_item/synth_mode.h"
#include "gui/ui/sound_editor.h"
#include "gui/views/instrument_clip_view.h"
#include "hid/display/display.h"
#include "model/instrument/kit.h"
#include "model/instrument/preset_kit.h"
#include "model/song/song.h"

extern deluge::gui::menu_item::SynthModeSelection synthModeMenu;

namespace deluge::gui::context_menu {

NewKitType newKitType{};

char const* NewKitType::getTitle() {
	return l10n::get(l10n::String::STRING_FOR_NEW_KIT_TYPE);
}

namespace {
// The options, in order: sample kit, the three preset kits, then two engines that go on one row only
enum Option : int32_t { kSample, kCycles, kLxr, kGin, kUltrakik, kXia, kNumOptions };

/// ULTRAKIK or XIA on the kit's selected row only (made a sample kit first if this isn't a kit), opening SYNTH MODE
/// on it so the engine can be changed from there
void putEngineOnRow(dsp::drum_synth::Engine engine) {
	if (getCurrentOutputType() != OutputType::KIT) {
		instrumentClipView.createNewInstrument(OutputType::KIT);
		if (getCurrentOutputType() != OutputType::KIT) {
			return;
		}
	}
	Kit* kit = getCurrentKit();
	if (kit->selectedDrum == nullptr || kit->selectedDrum->type != DrumType::SOUND) {
		for (Drum* drum = kit->firstDrum; drum != nullptr; drum = drum->next) {
			if (drum->type == DrumType::SOUND) {
				instrumentClipView.setSelectedDrum(drum);
				break;
			}
		}
	}
	if (kit->selectedDrum == nullptr || kit->selectedDrum->type != DrumType::SOUND) {
		return;
	}
	if (soundEditor.setup(getCurrentClip(), &synthModeMenu, 0)) {
		openUI(&soundEditor);
		gui::menu_item::drum_synth::setDrumEngine(engine, 0);
		synthModeMenu.readValueAgain();
	}
}
} // namespace

std::span<char const*> NewKitType::getOptions() {
	using enum l10n::String;
	static char const* options[kNumOptions];
	options[kSample] = l10n::get(STRING_FOR_SAMPLE_KIT);
	options[kCycles] = l10n::get(STRING_FOR_CYCLES_KIT);
	options[kLxr] = l10n::get(STRING_FOR_LXR_KIT);
	options[kGin] = l10n::get(STRING_FOR_GIN_KIT);
	options[kUltrakik] = l10n::get(STRING_FOR_ULTRAKIK_KIT);
	options[kXia] = l10n::get(STRING_FOR_XIA_KIT);
	return {options, kNumOptions};
}

bool NewKitType::setupFor(dsp::drum_synth::PresetKit kit) {
	using dsp::drum_synth::PresetKit;
	currentOption = (kit == PresetKit::LXR)      ? kLxr
	                : (kit == PresetKit::CYCLES) ? kCycles
	                : (kit == PresetKit::GIN)    ? kGin
	                                             : kSample;
	scrollPos = currentOption;
	return true;
}

bool NewKitType::acceptCurrentOption() {
	using dsp::drum_synth::PresetKit;
	const int32_t option = currentOption;
	if (option == kUltrakik || option == kXia) {
		display->setNextTransitionDirection(-1);
		close();
		putEngineOnRow(option == kUltrakik ? dsp::drum_synth::Engine::ULTRAKIK : dsp::drum_synth::Engine::TIA);
		return true;
	}
	const PresetKit chosen = (option == kLxr)      ? PresetKit::LXR
	                         : (option == kCycles) ? PresetKit::CYCLES
	                         : (option == kGin)    ? PresetKit::GIN
	                                               : PresetKit::NONE;
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
	case PresetKit::GIN:
		// Then straight into its presets, playing the top one
		if (instrumentClipView.createNewDrumSynthKit(chosen) && kitPresets.setup(chosen)) {
			openUI(&kitPresets);
			kitPresets.startOnTopPreset();
		}
		break;
	default:
		instrumentClipView.createNewInstrument(OutputType::KIT);
		break;
	}
	return true;
}

} // namespace deluge::gui::context_menu
