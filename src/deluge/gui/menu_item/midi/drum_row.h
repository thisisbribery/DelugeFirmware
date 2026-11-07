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

#pragma once
#include "definitions_cxx.hpp"
#include "gui/menu_item/integer.h"
#include "gui/ui/sound_editor.h"
#include "model/drum/midi_drum.h"
#include "model/instrument/kit.h"
#include "model/song/song.h"

/// Channel and note of a kit's MIDI row, from the row's menus (hold its audition pad and press SELECT), as the gold
/// knobs set them while the row is auditioned
namespace deluge::gui::menu_item::midi::drum_row {

inline MIDIDrum* selectedMIDIDrum() {
	if (getCurrentOutputType() != OutputType::KIT) {
		return nullptr;
	}
	Drum* drum = getCurrentKit()->selectedDrum;
	return (drum != nullptr && drum->type == DrumType::MIDI) ? static_cast<MIDIDrum*>(drum) : nullptr;
}

class Channel final : public Integer {
public:
	using Integer::Integer;
	[[nodiscard]] int32_t getMinValue() const override { return 1; }
	[[nodiscard]] int32_t getMaxValue() const override { return 16; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return selectedMIDIDrum() != nullptr;
	}
	void readCurrentValue() override {
		if (MIDIDrum* drum = selectedMIDIDrum()) {
			this->setValue(drum->channel + 1);
		}
	}
	void writeCurrentValue() override {
		if (MIDIDrum* drum = selectedMIDIDrum()) {
			// A note still sounding has to end on the channel it started on
			if (drum->hasActiveVoices()) {
				drum->noteOff(nullptr);
			}
			drum->channel = this->getValue() - 1;
		}
	}
};

class Note final : public Integer {
public:
	using Integer::Integer;
	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxMIDIValue; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return selectedMIDIDrum() != nullptr;
	}
	void readCurrentValue() override {
		if (MIDIDrum* drum = selectedMIDIDrum()) {
			this->setValue(drum->note);
		}
	}
	void writeCurrentValue() override {
		if (MIDIDrum* drum = selectedMIDIDrum()) {
			if (drum->hasActiveVoices()) {
				drum->noteOff(nullptr);
			}
			drum->note = this->getValue();
		}
	}
};

} // namespace deluge::gui::menu_item::midi::drum_row
