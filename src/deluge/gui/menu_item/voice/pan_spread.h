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

// VOICE > PAN SPREAD and SPREAD MODE: place each poly voice across the stereo field at note-on

#include "definitions_cxx.hpp"
#include "gui/menu_item/integer.h"
#include "gui/menu_item/selection.h"
#include "gui/ui/sound_editor.h"
#include "model/drum/drum.h"
#include "model/instrument/kit.h"
#include "model/song/song.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"

namespace deluge::gui::menu_item::voice {

namespace pan_spread_detail {
// Run fn on the current sound, or every sound row in the kit when AFFECT ENTIRE is held
template <typename Fn>
inline void forEachTargetSound(Fn fn) {
	if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
		Kit* kit = getCurrentKit();
		for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
			if (thisDrum->type == DrumType::SOUND) {
				fn(*static_cast<SoundDrum*>(thisDrum));
			}
		}
	}
	else {
		fn(*soundEditor.currentSound);
	}
}

inline bool relevantFor(ModControllableAudio* modControllable) {
	Sound* sound = static_cast<Sound*>(modControllable);
	return sound->polyphonic == PolyphonyMode::POLY && sound->synthMode != SynthMode::TB303;
}
} // namespace pan_spread_detail

class PanSpread final : public Integer {
public:
	using Integer::Integer;
	void readCurrentValue() override { this->setValue(soundEditor.currentSound->voiceStereoSpread); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		const auto value = static_cast<uint8_t>(this->getValue());
		pan_spread_detail::forEachTargetSound([value](Sound& sound) { sound.voiceStereoSpread = value; });
	}
	[[nodiscard]] int32_t getMaxValue() const override { return kMaxVoiceStereoSpread; }
	// SELECT goes on to SPREAD MODE (handy from the PAN pad's second layer)
	MenuItem* selectButtonPress() override;
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return pan_spread_detail::relevantFor(modControllable);
	}
	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(l10n::String::STRING_FOR_VOICE_SPREAD_SHORT));
	}
};

class PanSpreadMode final : public Selection {
public:
	using Selection::Selection;
	void readCurrentValue() override { this->setValue(soundEditor.currentSound->voiceSpreadMode); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		const auto mode = this->getValue<VoiceSpreadMode>();
		pan_spread_detail::forEachTargetSound([mode](Sound& sound) { sound.voiceSpreadMode = mode; });
	}
	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		return {
		    l10n::getView(l10n::String::STRING_FOR_ALTERNATE),
		    l10n::getView(l10n::String::STRING_FOR_VOICE_SPREAD_KEY),
		    l10n::getView(l10n::String::STRING_FOR_RANDOM),
		    l10n::getView(l10n::String::STRING_FOR_VOICE_SPREAD_PING_PONG),
		};
	}
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return pan_spread_detail::relevantFor(modControllable);
	}
	void getColumnLabel(StringBuf& label) override {
		label.append(deluge::l10n::get(l10n::String::STRING_FOR_VOICE_SPREAD_MODE_SHORT));
	}
};

extern PanSpread panSpreadMenu;
extern PanSpreadMode panSpreadModeMenu;

inline MenuItem* PanSpread::selectButtonPress() {
	return &panSpreadModeMenu;
}

} // namespace deluge::gui::menu_item::voice
