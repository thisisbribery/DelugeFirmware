/*
 * Copyright © 2026 Synthstrom Audible Limited
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
#include "gui/menu_item/integer.h"
#include "gui/menu_item/selection.h"
#include "gui/menu_item/toggle.h"
#include "model/clip/instrument_clip.h"
#include "model/song/song.h"

// Mono sequencer settings, on synth, MIDI and CV clips' menus
namespace deluge::gui::menu_item::cv {

/// True when the sound editor is on a synth, MIDI or CV clip, which can be mono
inline bool canBeMono() {
	Clip* clip = getCurrentClip();
	OutputType type = getCurrentOutputType();
	return clip && clip->type == ClipType::INSTRUMENT
	       && (type == OutputType::CV || type == OutputType::SYNTH || type == OutputType::MIDI_OUT);
}

/// Classic (off) or the mono sequencer (on) for this clip
class Mono final : public Toggle {
public:
	using Toggle::Toggle;
	void readCurrentValue() override { this->setValue(getCurrentInstrumentClip()->monoMode); }
	void writeCurrentValue() override {
		InstrumentClip* clip = getCurrentInstrumentClip();
		clip->monoMode = this->getValue();
		if (clip->monoMode) {
			// The 8 rows start at the scale's root
			clip->yScroll = clip->getMonoYScroll(currentSong);
		}
	}
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override { return canBeMono(); }
};

/// CV only: the song's scale, or a voltage set for each row
class MonoScale final : public Selection {
public:
	using Selection::Selection;
	void readCurrentValue() override { this->setValue(getCurrentInstrumentClip()->monoCustom); }
	void writeCurrentValue() override { getCurrentInstrumentClip()->monoCustom = this->getValue(); }
	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		return {"SCALE", "CUSTOM"};
	}
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return canBeMono() && getCurrentOutputType() == OutputType::CV && getCurrentInstrumentClip()->monoMode;
	}
};

/// How much of each note's length its gate stays open for
class MonoGate final : public Integer {
public:
	using Integer::Integer;
	void readCurrentValue() override { this->setValue(getCurrentInstrumentClip()->monoGatePercent); }
	void writeCurrentValue() override { getCurrentInstrumentClip()->monoGatePercent = this->getValue(); }
	[[nodiscard]] int32_t getMinValue() const override { return 1; }
	[[nodiscard]] int32_t getMaxValue() const override { return 100; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return canBeMono() && getCurrentInstrumentClip()->monoMode;
	}
};

} // namespace deluge::gui::menu_item::cv
