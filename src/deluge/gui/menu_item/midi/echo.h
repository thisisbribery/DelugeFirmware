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
#include "gui/l10n/l10n.h"
#include "gui/menu_item/selection.h"
#include "model/instrument/midi_instrument.h"
#include "model/song/song.h"

namespace deluge::gui::menu_item::midi {

/// MIDI loop tracks: ON sends live input back out (normal), OFF never does, REC stops only while recording. With OFF
/// a synth with local control on can play into its own MIDI track without hearing every note twice.
class Echo final : public Selection {
public:
	using Selection::Selection;
	// this is safe since it's only shown in midi clips
	void readCurrentValue() override {
		this->setValue(static_cast<int32_t>(((MIDIInstrument*)getCurrentOutput())->echo));
	}
	void writeCurrentValue() override {
		((MIDIInstrument*)getCurrentOutput())->echo = static_cast<MIDIEcho>(this->getValue());
	}
	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {l10n::getView(STRING_FOR_ON), l10n::getView(STRING_FOR_OFF), l10n::getView(STRING_FOR_MIDI_ECHO_REC)};
	}
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return getCurrentOutputType() == OutputType::MIDI_OUT;
	}
};

} // namespace deluge::gui::menu_item::midi
