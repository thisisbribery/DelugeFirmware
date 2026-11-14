/*
 * Copyright (c) 2026 thisisbribery
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
#include "dsp/crusher.h"
#include "gui/menu_item/selection.h"
#include "gui/ui/sound_editor.h"
#include "model/instrument/kit.h"
#include "model/mod_controllable/mod_controllable_audio.h"
#include "model/song/song.h"
#include "processing/sound/sound_drum.h"
#include <cstdint>

namespace deluge::gui::menu_item::fx {

/// What the Bitcrush and Decimation knobs sound like. Sets the sound being edited, or every sound in the kit if
/// affect-entire is held.
class CrushType final : public Selection {
public:
	using Selection::Selection;

	void readCurrentValue() override { this->setValue<dsp::CrushType>(soundEditor.currentModControllable->crushType); }
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		auto type = this->getValue<dsp::CrushType>();
		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					static_cast<SoundDrum*>(thisDrum)->crushType = type;
				}
			}
		}
		else {
			soundEditor.currentModControllable->crushType = type;
		}
	}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		using enum l10n::String;
		return {l10n::getView(STRING_FOR_CRUSH_CLASSIC), l10n::getView(STRING_FOR_CRUSH_HARD),
		        l10n::getView(STRING_FOR_CRUSH_VINTAGE), l10n::getView(STRING_FOR_CRUSH_ULAW),
		        l10n::getView(STRING_FOR_CRUSH_JITTER)};
	}
};

} // namespace deluge::gui::menu_item::fx
