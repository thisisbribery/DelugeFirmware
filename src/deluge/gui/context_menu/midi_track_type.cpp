/*
 * Copyright © 2026 thisisbribery
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

#include "gui/context_menu/midi_track_type.h"
#include "definitions_cxx.hpp"
#include "gui/l10n/l10n.h"
#include "gui/ui/ui.h"
#include "gui/views/instrument_clip_view.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "model/instrument/midi_instrument.h"
#include "model/song/song.h"

namespace deluge::gui::context_menu {

MIDITrackType midiTrackType{};

char const* MIDITrackType::getTitle() {
	using enum l10n::String;
	return l10n::get(choosingLayout_ ? STRING_FOR_MIDI_KIT_LAYOUT : STRING_FOR_MIDI_TRACK_TYPE);
}

std::span<char const*> MIDITrackType::getOptions() {
	using enum l10n::String;
	if (choosingLayout_) {
		static char const* layouts[] = {l10n::get(STRING_FOR_MIDI_KIT_ONE_CHANNEL),
		                                l10n::get(STRING_FOR_MIDI_KIT_SIXTEEN_CHANNELS),
		                                l10n::get(STRING_FOR_MIDI_KIT_BLANK)};
		return {layouts, midi_kit::kNumLayouts};
	}
	static char const* types[] = {l10n::get(STRING_FOR_MIDI), l10n::get(STRING_FOR_MIDI_KIT),
	                              l10n::get(STRING_FOR_MIDI_LOOP_TRACK)};
	return {types, 3};
}

bool MIDITrackType::setupAndCheckAvailability() {
	choosingLayout_ = false;
	currentOption = 0; // MIDI
	scrollPos = 0;
	return true;
}

ActionResult MIDITrackType::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	// SHIFT + MIDI again: the next option, round to the first after the last
	if (b == deluge::hid::button::MIDI && Buttons::isShiftButtonPressed()) {
		if (on) {
			const int32_t numOptions = static_cast<int32_t>(getOptions().size());
			const int32_t next = (currentOption + 1) % numOptions;
			selectEncoderAction(display->haveOLED() ? next - currentOption : 1);
		}
		return ActionResult::DEALT_WITH;
	}
	return ContextMenu::buttonAction(b, on, inCardRoutine);
}

bool MIDITrackType::acceptCurrentOption() {
	// MKIT: on to its layouts
	if (!choosingLayout_ && currentOption == 1) {
		choosingLayout_ = true;
		currentOption = 0;
		scrollPos = 0;
		if (display->haveOLED()) {
			renderUIsForOled();
		}
		else {
			drawCurrentOption();
		}
		return true;
	}

	const bool midiKit = choosingLayout_;
	const bool loopTrack = !choosingLayout_ && currentOption == 2;
	const auto layout = static_cast<midi_kit::Layout>(currentOption);
	display->setNextTransitionDirection(-1);
	close();

	if (midiKit) {
		instrumentClipView.createNewMIDIKit(layout);
	}
	else {
		instrumentClipView.changeOutputType(OutputType::MIDI_OUT);
		// LOOP: a MIDI track that takes its channel from the first note played into it and doesn't echo it back
		if (loopTrack && getCurrentOutputType() == OutputType::MIDI_OUT) {
			auto* midiInstrument = static_cast<MIDIInstrument*>(getCurrentOutput());
			midiInstrument->echo = MIDIEcho::OFF;
			midiInstrument->loopAutoBind = true;
			midiInstrument->midiInput.clear();
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_NEW_MIDI_LOOP_TRACK));
		}
	}
	return true;
}

} // namespace deluge::gui::context_menu
