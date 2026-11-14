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
#include "model/clip/instrument_clip.h"
#include "model/instrument/kit.h"
#include "model/song/song.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"

namespace deluge::gui::menu_item {
/// Every sound engine in one list: the Deluge's synth modes, the 303 (on synths), RINGS' models, then each drum synth
/// engine and variant. Choosing a drum engine puts it on osc 1 of this sound, which keeps every envelope, filter and
/// effect, so any kit row can go from a sample to a Cyc Kik and back. This is the only place engines are chosen.
class SynthModeSelection final : public Selection {
public:
	using Selection::Selection;

	struct Entry {
		enum class Kind : uint8_t { MODE, TB303, DRUM };
		Kind kind;
		::SynthMode mode;
		dsp::drum_synth::Engine engine;
		uint8_t variant; ///< DXIA / DIIA variant
		l10n::String name;
	};

	void readCurrentValue() override {
		const Sound& sound = *soundEditor.currentSound;
		const Source& source = sound.sources[0];
		const auto list = entries();
		for (size_t i = 0; i < list.size(); i++) {
			const Entry& e = list[i];
			const bool match = (source.oscType == OscType::DRUM_SYNTH)
			                       ? (e.kind == Entry::Kind::DRUM && e.engine == source.drumPatch.engine()
			                          && (!dsp::drum_synth::isXia(e.engine)
			                              || e.variant == source.drumPatch.get(dsp::drum_synth::Patch::XIA_VARIANT))
			                          && (!dsp::drum_synth::isRings(e.engine)
			                              || e.variant == source.drumPatch.get(dsp::drum_synth::Patch::RINGS_MODEL)))
			                   : (sound.synthMode == ::SynthMode::TB303)
			                       ? e.kind == Entry::Kind::TB303
			                       : (e.kind == Entry::Kind::MODE && e.mode == sound.synthMode);
			if (match) {
				this->setValue(static_cast<int32_t>(i));
				return;
			}
		}
		this->setValue(0);
	}
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		Sound& sound = *soundEditor.currentSound;
		const auto list = entries();
		const Entry& e = list[std::clamp<int32_t>(this->getValue(), 0, static_cast<int32_t>(list.size()) - 1)];
		switch (e.kind) {
		case Entry::Kind::DRUM:
			drum_synth::setDrumEngine(e.engine, e.variant);
			break;
		case Entry::Kind::TB303:
			drum_synth::leaveDrumSynth();
			setSynthMode(::SynthMode::TB303);
			break;
		case Entry::Kind::MODE: {
			drum_synth::leaveDrumSynth();
			// FM and ringmod are made of the Deluge's own oscillators
			if (e.mode != ::SynthMode::SUBTRACTIVE) {
				for (Source& source : sound.sources) {
					if (source.oscType > kLastRingmoddableOscType) {
						sound.killAllVoices();
						source.setOscType(OscType::SAW);
					}
				}
			}
			setSynthMode(e.mode);
			break;
		}
		}
		view.setKnobIndicatorLevels();
	}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		deluge::vector<std::string_view> options;
		for (const Entry& e : entries()) {
			options.push_back(l10n::getView(e.name));
		}
		return options;
	}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		// DX7 sounds have their own engine menu
		Sound* sound = static_cast<Sound*>(modControllable);
		return sound->sources[0].oscType != OscType::DX7;
	}

	/// The list as it stands for the sound being edited: on a kit row plain subtractive reads as Sample, and there is
	/// no 303 (it's a mono synth, played from a clip's mono sequencer)
	static deluge::vector<Entry> entries() {
		using enum l10n::String;
		using dsp::drum_synth::Engine;
		using K = Entry::Kind;
		const bool kitRow = soundEditor.editingKitRow();
		deluge::vector<Entry> list{
		    {K::MODE, ::SynthMode::SUBTRACTIVE, Engine::DRUM, 0, kitRow ? STRING_FOR_SAMPLE : STRING_FOR_SUBTRACTIVE},
		    {K::MODE, ::SynthMode::FM, Engine::DRUM, 0, STRING_FOR_FM},
		    {K::MODE, ::SynthMode::RINGMOD, Engine::DRUM, 0, STRING_FOR_RINGMOD},
		};
		if (!kitRow) {
			list.push_back({K::TB303, ::SynthMode::TB303, Engine::DRUM, 0, STRING_FOR_TB303});
		}
		// RINGS, one entry per model. Moving between them keeps the knobs, as Rings' model button does.
		static constexpr std::array<Entry, 5> kRings{{
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::RINGS, 0, STRING_FOR_RINGS_MODAL_ENGINE},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::RINGS, 1, STRING_FOR_RINGS_SYMPATHETIC_ENGINE},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::RINGS, 2, STRING_FOR_RINGS_STRING_ENGINE},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::RINGS, 3, STRING_FOR_RINGS_FM_ENGINE},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::RINGS, 4, STRING_FOR_RINGS_CHORD_ENGINE},
		}};
		for (const Entry& e : kRings) {
			list.push_back(e);
		}
		static constexpr std::array<Entry, 19> kDrumEngines{{
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::DRUM, 0, STRING_FOR_DRUM_ENGINE_DRUM},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::SNARE, 0, STRING_FOR_DRUM_ENGINE_SNARE},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::METAL, 0, STRING_FOR_DRUM_ENGINE_METAL},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::TIA, 0, STRING_FOR_DRUM_PRESET_TIA_L},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::TIA, 1, STRING_FOR_DRUM_PRESET_TIA_B},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::TIA, 2, STRING_FOR_DRUM_PRESET_TIA_C},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::IIA, 0, STRING_FOR_DRUM_PRESET_IIA_G},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::IIA, 1, STRING_FOR_DRUM_PRESET_IIA_I},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::IIA, 2, STRING_FOR_DRUM_PRESET_IIA_H},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::CYC_KIK, 0, STRING_FOR_DRUM_ENGINE_CYC_KIK},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::CYC_SNR, 0, STRING_FOR_DRUM_ENGINE_CYC_SNR},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::CYC_METAL, 0, STRING_FOR_DRUM_ENGINE_CYC_METAL},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::CYC_PERC, 0, STRING_FOR_DRUM_ENGINE_CYC_PERC},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::CYC_TONE, 0, STRING_FOR_DRUM_ENGINE_CYC_TONE},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::CYC_CHORD, 0, STRING_FOR_DRUM_ENGINE_CYC_CHORD},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::VTG_SNR, 0, STRING_FOR_DRUM_ENGINE_VTG_SNR},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::CLAP_ENG, 0, STRING_FOR_DRUM_ENGINE_CLAP_ENG},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::GIN, 0, STRING_FOR_DRUM_ENGINE_GIN},
		    {K::DRUM, ::SynthMode::SUBTRACTIVE, Engine::ULTRAKIK, 0, STRING_FOR_DRUM_ENGINE_ULTRAKIK},
		}};
		for (const Entry& e : kDrumEngines) {
			list.push_back(e);
		}
		return list;
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
				InstrumentClip* clip = getCurrentInstrumentClip();
				if (clip && clip->output->type == OutputType::SYNTH
				    && static_cast<Sound*>(static_cast<SoundInstrument*>(clip->output)) == sound) {
					clip->ensureMonoScaleMode(currentSong);
				}
			}
		}
	}
};
} // namespace deluge::gui::menu_item
