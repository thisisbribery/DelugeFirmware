/*
 * Copyright (c) 2014-2023 Synthstrom Audible Limited
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
#include "gui/menu_item/drum_synth/editor.h"
#include "gui/menu_item/formatted_title.h"
#include "gui/menu_item/selection.h"
#include "gui/menu_item/submenu.h"
#include "gui/ui/sound_editor.h"
#include "model/song/song.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound.h"
#include "processing/source.h"
#include "util/comparison.h"

#include <hid/display/oled.h>

extern gui::menu_item::Submenu dxMenu;

namespace deluge::gui::menu_item::osc {
class Type final : public Selection, public FormattedTitle {
public:
	Type(l10n::String name, l10n::String title_format_str, uint8_t source_id)
	    : Selection(name), FormattedTitle(title_format_str, source_id + 1), sourceId_{source_id} {};
	void beginSession(MenuItem* navigatedBackwardFrom) override { Selection::beginSession(navigatedBackwardFrom); }

	bool mayUseDx() const { return !soundEditor.editingKit() && sourceId_ == 0; }
	bool mayUseDrumSynth() const { return sourceId_ == 0; }

	/// On a drum synth row, OSC1 TYPE picks the engine's variant instead: TIA L / B / C, IIA G / I / H, or the LXR
	/// voice. SYNTH MODE switches between drum synth engines and back to samples and oscillators.
	bool pickingDrumVariant() const {
		return sourceId_ == 0 && soundEditor.currentSound->sources[0].oscType == OscType::DRUM_SYNTH;
	}

	/// Option index -> OscType for the menu value. The optional types sit between SAMPLE and the inputs, so values
	/// after a missing one shift down.
	OscType typeForValue(int32_t value) const {
		int32_t raw = value;
		if (!mayUseDx() && raw >= static_cast<int32_t>(OscType::DX7)) {
			raw += 1;
		}
		if (!mayUseDrumSynth() && raw >= static_cast<int32_t>(OscType::DRUM_SYNTH)) {
			raw += 1;
		}
		return static_cast<OscType>(raw);
	}

	void readCurrentValue() override {
		if (pickingDrumVariant()) {
			const auto& patch = soundEditor.currentSound->sources[0].drumPatch;
			setValue(dsp::drum_synth::isXia(patch.engine()) ? patch.get(dsp::drum_synth::Patch::XIA_VARIANT)
			                                                : static_cast<int32_t>(patch.engine()));
			return;
		}
		OscType type = soundEditor.currentSound->sources[sourceId_].oscType;
		int32_t value = static_cast<int32_t>(type);
		if (!mayUseDrumSynth() && type > OscType::DRUM_SYNTH) {
			value -= 1;
		}
		if (!mayUseDx() && type > OscType::DX7) {
			value -= 1;
		}
		setValue(value);
	}
	void writeCurrentValue() override {
		if (pickingDrumVariant()) {
			auto& patch = soundEditor.currentSound->sources[0].drumPatch;
			const auto value = static_cast<uint8_t>(getValue());
			if (dsp::drum_synth::isXia(patch.engine())) {
				patch.set(dsp::drum_synth::Patch::XIA_VARIANT, value);
			}
			else {
				patch.set(dsp::drum_synth::Patch::ENGINE, value);
			}
			return;
		}
		OscType oldValue = soundEditor.currentSound->sources[sourceId_].oscType;
		OscType newValue = typeForValue(getValue());

		const auto needs_unassignment = {
		    OscType::INPUT_L,
		    OscType::INPUT_R,
		    OscType::INPUT_STEREO,
		    OscType::SAMPLE,
		    OscType::DX7,
		    OscType::DRUM_SYNTH,

		    // Haven't actually really determined if this needs to be here - maybe not?
		    OscType::WAVETABLE,
		};

		if (util::one_of(oldValue, needs_unassignment) || util::one_of(newValue, needs_unassignment)) {
			soundEditor.currentSound->killAllVoices();
		}

		soundEditor.currentSound->sources[sourceId_].setOscType(newValue);

		if (oldValue == OscType::SQUARE || newValue == OscType::SQUARE) {
			soundEditor.currentSound->setupPatchingForAllParamManagers(currentSong);
		}
	}

	[[nodiscard]] std::string_view getTitle() const override { return FormattedTitle::title(); }

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		if (pickingDrumVariant()) {
			switch (soundEditor.currentSound->sources[0].drumPatch.engine()) {
			case dsp::drum_synth::Engine::TIA:
				return {l10n::getView(STRING_FOR_DRUM_PRESET_TIA_L), l10n::getView(STRING_FOR_DRUM_PRESET_TIA_B),
				        l10n::getView(STRING_FOR_DRUM_PRESET_TIA_C)};
			case dsp::drum_synth::Engine::IIA:
				return {l10n::getView(STRING_FOR_DRUM_PRESET_IIA_G), l10n::getView(STRING_FOR_DRUM_PRESET_IIA_I),
				        l10n::getView(STRING_FOR_DRUM_PRESET_IIA_H)};
			default:
				return {l10n::getView(STRING_FOR_DRUM_ENGINE_DRUM), l10n::getView(STRING_FOR_DRUM_ENGINE_SNARE),
				        l10n::getView(STRING_FOR_DRUM_ENGINE_METAL)};
			}
		}
		deluge::vector options = {
		    l10n::getView(STRING_FOR_SINE),          //<
		    l10n::getView(STRING_FOR_TRIANGLE),      //<
		    l10n::getView(STRING_FOR_SQUARE),        //<
		    l10n::getView(STRING_FOR_ANALOG_SQUARE), //<
		    l10n::getView(STRING_FOR_SAW),           //<
		    l10n::getView(STRING_FOR_ANALOG_SAW),    //<
		    l10n::getView(STRING_FOR_WAVETABLE),     //<
		};

		if (soundEditor.currentSound->getSynthMode() == SynthMode::RINGMOD) {
			return options;
		}

		options.emplace_back(l10n::getView(STRING_FOR_SAMPLE));

		if (mayUseDx()) {
			options.emplace_back(l10n::getView(STRING_FOR_DX7));
		}

		if (mayUseDrumSynth()) {
			options.emplace_back(l10n::getView(STRING_FOR_DRUM_SYNTH));
		}

		if (AudioEngine::micPluggedIn || AudioEngine::lineInPluggedIn) {
			options.emplace_back(l10n::getView(STRING_FOR_INPUT_LEFT));
			options.emplace_back(l10n::getView(STRING_FOR_INPUT_RIGHT));
			options.emplace_back(l10n::getView(STRING_FOR_INPUT_STEREO));
		}
		else {
			options.emplace_back(l10n::getView(STRING_FOR_INPUT));
		}

		return options;
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		Sound* sound = static_cast<Sound*>(modControllable);
		return (sound->getSynthMode() != SynthMode::FM);
	}

	MenuItem* selectButtonPress() override {
		switch (soundEditor.currentSound->sources[sourceId_].oscType) {
		case OscType::DX7:
			return &dxMenu;
		case OscType::DRUM_SYNTH:
			return &drum_synth::drumSynthEditor;
		default:
			return nullptr;
		}
	}

	[[nodiscard]] bool showColumnLabel() const override { return false; }

	void renderInHorizontalMenu(const SlotPosition& slot) override {
		oled_canvas::Canvas& image = OLED::main;

		const OscType osc_type = soundEditor.currentSound->sources[sourceId_].oscType;
		if (osc_type == OscType::DX7 || osc_type == OscType::DRUM_SYNTH) {
			const auto option = getOptions(OptType::FULL)[getValue()].data();
			return image.drawStringCentered(option, slot.start_x, slot.start_y + kHorizontalMenuSlotYOffset + 5,
			                                kTextTitleSpacingX, kTextTitleSizeY, slot.width);
		}

		const Icon& icon = [&] {
			switch (osc_type) {
			case OscType::SINE:
				return OLED::sineIcon;
			case OscType::TRIANGLE:
				return OLED::triangleIcon;
			case OscType::SQUARE:
			case OscType::ANALOG_SQUARE:
				return OLED::squareIcon;
			case OscType::SAW:
			case OscType::ANALOG_SAW_2:
				return OLED::sawIcon;
			case OscType::SAMPLE:
				return OLED::sampleIcon;
			case OscType::INPUT_STEREO:
			case OscType::INPUT_L:
			case OscType::INPUT_R:
				return AudioEngine::lineInPluggedIn ? OLED::inputIcon : OLED::micIcon;
			case OscType::WAVETABLE:
				return OLED::wavetableIcon;
			default:
				return OLED::sineIcon;
			}
		}();

		image.drawIconCentered(icon, slot.start_x, slot.width, slot.start_y + kHorizontalMenuSlotYOffset + 2);

		if (osc_type == OscType::ANALOG_SQUARE || osc_type == OscType::ANALOG_SAW_2) {
			const int32_t x = slot.start_x + 4;
			constexpr int32_t y = OLED_MAIN_HEIGHT_PIXELS - kTextSpacingY - 8;
			image.clearAreaExact(x - 1, y - 1, x + kTextSpacingX + 1, y + kTextSpacingY + 1);
			image.drawChar('A', x, y, kTextSpacingX, kTextSpacingY);
		}
	}

	bool wrapAround() override {
		return parent != nullptr && parent->renderingStyle() == Submenu::RenderingStyle::HORIZONTAL;
	}

private:
	uint8_t sourceId_;
};

} // namespace deluge::gui::menu_item::osc
