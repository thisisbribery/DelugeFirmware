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

#include "gui/context_menu/synth_instrument.h"
#include "definitions_cxx.hpp"
#include "fatfs/ff.h"
#include "gui/l10n/l10n.h"
#include "gui/views/instrument_clip_view.h"
#include "gui/views/view.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "model/instrument/synth_instruments.h"
#include "model/model_stack.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "processing/sound/sound_instrument.h"
#include <cstring>
#include <strings.h>

namespace deluge::gui::context_menu {

using synth_instruments::kSynthInstruments;

SynthInstrumentMenu synthInstrumentMenu{};

namespace {
/// Whether a preset folder has any presets in it. A missing folder is made, ready to save presets into, as the Deluge
/// can't make folders when saving.
bool folderHasPresets(char const* folder) {
	DIR dir;
	if (f_opendir(&dir, folder) != FR_OK) {
		f_mkdir("SYNTHS");
		f_mkdir(folder);
		return false;
	}
	bool found = false;
	FILINFO fileInfo;
	while (!found && f_readdir(&dir, &fileInfo) == FR_OK && fileInfo.fname[0]) {
		const size_t length = strlen(fileInfo.fname);
		found = !(fileInfo.fattrib & AM_DIR) && fileInfo.fname[0] != '.' && length >= 5
		        && strcasecmp(fileInfo.fname + length - 4, ".XML") == 0;
	}
	f_closedir(&dir);
	return found;
}
} // namespace

char const* SynthInstrumentMenu::getTitle() {
	return l10n::get(l10n::String::STRING_FOR_SYNTH_INSTRUMENT);
}

std::span<char const*> SynthInstrumentMenu::getOptions() {
	static std::array<char const*, kSynthInstruments.size()> options;
	for (size_t i = 0; i < kSynthInstruments.size(); i++) {
		options[i] = l10n::get(kSynthInstruments[i].name);
	}
	return {options.data(), options.size()};
}

bool SynthInstrumentMenu::isCurrentOptionAvailable() {
	return !kSynthInstruments[currentOption].dx
	       || runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::EnableDX7Engine);
}

bool SynthInstrumentMenu::setupAndCheckAvailability() {
	if (getCurrentOutputType() != OutputType::SYNTH) {
		return false;
	}
	auto* synth = static_cast<SoundInstrument*>(getCurrentOutput());
	const bool dx = synth->sources[0].oscType == OscType::DX7;
	// The instrument whose folder the synth came from, else the first that plays like it
	int32_t current = -1;
	for (size_t i = 0; i < kSynthInstruments.size() && current < 0; i++) {
		if (!strcasecmp(synth->dirPath.get(), kSynthInstruments[i].folder)) {
			current = static_cast<int32_t>(i);
		}
	}
	for (size_t i = 0; i < kSynthInstruments.size() && current < 0; i++) {
		if (kSynthInstruments[i].dx == dx && (dx || kSynthInstruments[i].mode == synth->getSynthMode())) {
			current = static_cast<int32_t>(i);
		}
	}
	currentOption = (current < 0) ? 0 : current;
	if (!isCurrentOptionAvailable()) {
		currentOption = 0;
	}
	scrollPos = currentOption;
	return true;
}

ActionResult SynthInstrumentMenu::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	// SHIFT + SYNTH again: the next instrument, round to the first after the last
	if (b == deluge::hid::button::SYNTH && Buttons::isShiftButtonPressed()) {
		if (on) {
			const int32_t numOptions = static_cast<int32_t>(kSynthInstruments.size());
			int32_t next = currentOption;
			do {
				next = (next + 1) % numOptions;
			} while (next != currentOption
			         && (kSynthInstruments[next].dx
			             && !runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::EnableDX7Engine)));
			selectEncoderAction(display->haveOLED() ? next - currentOption : 1);
		}
		return ActionResult::DEALT_WITH;
	}
	return ContextMenu::buttonAction(b, on, inCardRoutine);
}

bool SynthInstrumentMenu::acceptCurrentOption() {
	const synth_instruments::SynthInstrument& instrument = kSynthInstruments[currentOption];
	display->setNextTransitionDirection(-1);
	close();
	if (getCurrentOutputType() != OutputType::SYNTH) {
		return true;
	}

	// Its first preset, as SELECT would step to it, so SELECT carries on through that folder
	if (folderHasPresets(instrument.folder)) {
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithTimelineCounter* modelStack = currentSong->setupModelStackWithCurrentClip(modelStackMemory);
		Output* before = getCurrentOutput();
		view.navigateThroughPresetsForInstrumentClip(1, modelStack, true, instrument.folder);
		// Every preset there already plays on another track, or this one is the only one
		if (getCurrentOutput() == before) {
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_NO_FREE_PRESETS));
		}
		return true;
	}

	// Nothing saved there yet: a blank synth of that kind, which saves into the folder
	if (instrumentClipView.createNewInstrument(OutputType::SYNTH, instrument.dx)
	    && getCurrentOutputType() == OutputType::SYNTH) {
		auto* synth = static_cast<SoundInstrument*>(getCurrentOutput());
		synth->dirPath.set(instrument.folder);
		if (instrument.mode != synth->getSynthMode()) {
			synth->setSynthMode(instrument.mode, currentSong);
		}
	}
	return true;
}

} // namespace deluge::gui::context_menu
