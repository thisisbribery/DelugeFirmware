/*
 * BRUNO CHORUS menus: its mode (shown only with BRUNO CHORUS as the Mod FX type), and an on / off switch for a pad.
 *
 * Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "definitions_cxx.hpp"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/selection.h"
#include "gui/menu_item/toggle.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/display.h"
#include "model/drum/drum.h"
#include "model/instrument/kit.h"
#include "model/mod_controllable/mod_controllable_audio.h"
#include "model/song/song.h"
#include "processing/sound/sound_drum.h"

namespace deluge::gui::menu_item::mod_fx {

/// BRUNO CHORUS's mode: I, II or I+II, as the Juno's buttons. Only there while BRUNO CHORUS is the Mod FX type.
class BrunoChorusMode final : public Selection {
public:
	using Selection::Selection;

	void readCurrentValue() override {
		setValue(std::clamp<int32_t>(soundEditor.currentModControllable->modfx.brunoChorusMode, 1, 3) - 1);
	}
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		const auto mode = static_cast<uint8_t>(getValue() + 1);
		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			for (Drum* d = getCurrentKit()->firstDrum; d != nullptr; d = d->next) {
				if (d->type == DrumType::SOUND) {
					static_cast<SoundDrum*>(d)->modfx.brunoChorusMode = mode;
				}
			}
		}
		else {
			soundEditor.currentModControllable->modfx.brunoChorusMode = mode;
		}
	}
	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {l10n::getView(STRING_FOR_BRUNO_CHORUS_I), l10n::getView(STRING_FOR_BRUNO_CHORUS_II),
		        l10n::getView(STRING_FOR_BRUNO_CHORUS_I_II)};
	}
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return modControllable->getModFXType() == ModFXType::BRUNO_CHORUS;
	}
};

/// BRUNO CHORUS on or off: on makes it the Mod FX type, off takes the Mod FX off. On a pad, each press flips it (see
/// SoundEditor::potentialShortcutPadAction).
class BrunoChorusOnOff final : public Toggle {
public:
	using Toggle::Toggle;

	void readCurrentValue() override {
		setValue(soundEditor.currentModControllable->getModFXType() == ModFXType::BRUNO_CHORUS);
	}
	void writeCurrentValue() override {
		if (!soundEditor.currentModControllable->setModFXType(getValue() ? ModFXType::BRUNO_CHORUS : ModFXType::NONE)) {
			display->displayError(Error::INSUFFICIENT_RAM);
		}
	}
	/// The pad's press: flip it and show which way it went
	void flipFromPad() {
		toggleValue();
		drawValue();
	}
};

} // namespace deluge::gui::menu_item::mod_fx
