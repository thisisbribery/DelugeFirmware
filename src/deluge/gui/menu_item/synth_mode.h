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
#include "gui/menu_item/drum_synth/param.h"
#include "gui/menu_item/selection.h"
#include "gui/ui/sound_editor.h"
#include "gui/views/view.h"
#include "model/instrument/kit.h"
#include "model/song/song.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"

namespace deluge::gui::menu_item {
/// The Deluge's synth modes, then the drum synth engines, then (on synths) the 303. Choosing a drum engine puts it on
/// osc 1 of this sound, which keeps every envelope, filter and effect, so any kit row can go from a sample to TIA and
/// back.
class SynthModeSelection final : public Selection {
public:
	using Selection::Selection;

	enum Choice : uint8_t { SUBTRACTIVE, FM, RINGMOD, TIA, IIA, DRUM_SYNTH, TB303 };

	void readCurrentValue() override {
		const Source& source = soundEditor.currentSound->sources[0];
		if (source.oscType == OscType::DRUM_SYNTH) {
			switch (source.drumPatch.engine()) {
			case dsp::drum_synth::Engine::TIA:
				this->setValue(TIA);
				return;
			case dsp::drum_synth::Engine::IIA:
				this->setValue(IIA);
				return;
			default:
				this->setValue(DRUM_SYNTH);
				return;
			}
		}
		if (soundEditor.currentSound->synthMode == ::SynthMode::TB303) {
			this->setValue(TB303);
			return;
		}
		this->setValue(soundEditor.currentSound->synthMode);
	}
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		using dsp::drum_synth::Engine;
		Sound& sound = *soundEditor.currentSound;
		switch (this->getValue()) {
		case TIA:
			drum_synth::setDrumEngine(Engine::TIA);
			break;
		case IIA:
			drum_synth::setDrumEngine(Engine::IIA);
			break;
		case DRUM_SYNTH:
			if (sound.sources[0].oscType != OscType::DRUM_SYNTH
			    || dsp::drum_synth::isXia(sound.sources[0].drumPatch.engine())) {
				drum_synth::setDrumEngine(Engine::DRUM);
			}
			break;
		case TB303:
			drum_synth::leaveDrumSynth();
			setSynthMode(::SynthMode::TB303);
			break;
		default: {
			auto current_value = this->getValue<::SynthMode>();
			drum_synth::leaveDrumSynth();
			// FM and ringmod are made of the Deluge's own oscillators
			if (current_value != ::SynthMode::SUBTRACTIVE) {
				for (Source& source : sound.sources) {
					if (source.oscType > kLastRingmoddableOscType) {
						sound.killAllVoices();
						source.setOscType(OscType::SAW);
					}
				}
			}
			setSynthMode(current_value);
			break;
		}
		}
		view.setKnobIndicatorLevels();
	}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		// On a kit row, plain subtractive is a sample
		deluge::vector<std::string_view> options{
		    l10n::getView(soundEditor.editingKitRow() ? STRING_FOR_SAMPLE : STRING_FOR_SUBTRACTIVE),
		    l10n::getView(STRING_FOR_FM),
		    l10n::getView(STRING_FOR_RINGMOD),
		    l10n::getView(STRING_FOR_DRUM_ENGINE_TIA),
		    l10n::getView(STRING_FOR_DRUM_ENGINE_IIA),
		    l10n::getView(STRING_FOR_DRUM_SYNTH),
		};
		// The 303 is a mono synth, played from a clip's mono sequencer, so not on kit rows
		if (!soundEditor.editingKitRow()) {
			options.push_back(l10n::getView(STRING_FOR_TB303));
		}
		return options;
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		// DX7 sounds have their own engine menu
		Sound* sound = static_cast<Sound*>(modControllable);
		return sound->sources[0].oscType != OscType::DX7;
	}

private:
	void setSynthMode(::SynthMode current_value) {
		// If affect-entire button held, do whole kit
		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {

			Kit* kit = getCurrentKit();

			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					auto* soundDrum = static_cast<SoundDrum*>(thisDrum);
					// Only rows made of the Deluge's own oscillators can change mode
					if (soundDrum->sources[0].oscType <= kLastRingmoddableOscType
					    && soundDrum->sources[1].oscType <= kLastRingmoddableOscType) {
						soundDrum->setSynthMode(current_value, currentSong);
					}
				}
			}
		}
		// Or, the normal case of just one sound
		else if (soundEditor.currentSound->synthMode != current_value) {
			Sound* sound = soundEditor.currentSound;
			bool becoming303 = current_value == ::SynthMode::TB303;
			sound->setSynthMode(current_value, currentSong);
			// A fresh 303 starts on an acid sound. Its clips play from the mono sequencer while it's a 303.
			if (becoming303 && sound->isTb303()) {
				sound->setupTb303Defaults(soundEditor.currentParamManager);
			}
		}
	}
};
} // namespace deluge::gui::menu_item
