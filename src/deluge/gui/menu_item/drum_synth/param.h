/*
 * Copyright © 2026 the Deluge community.
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

#include "dsp/drum_synth/drum_synth.h"
#include "gui/menu_item/integer.h"
#include "gui/menu_item/patched_param/integer.h"
#include "gui/menu_item/selection.h"
#include "gui/ui/sound_editor.h"
#include "gui/views/view.h"
#include "hid/display/display.h"
#include "model/model_stack.h"
#include "model/song/song.h"
#include "modulation/automation/auto_param.h"
#include "modulation/params/param.h"
#include "processing/sound/drum_synth_params.h"
#include "processing/sound/sound.h"
#include "util/functions.h"

namespace deluge::gui::menu_item::drum_synth {

using deluge::dsp::drum_synth::Engine;
using deluge::dsp::drum_synth::Patch;

/// The drum synth always lives on osc 1
inline Patch& currentPatch() {
	return soundEditor.currentSound->sources[0].drumPatch;
}

/// Shown only when the sound's drum synth engine reads this setting
inline bool usedBySound(ModControllableAudio* modControllable, Patch::Param param) {
	// Patched sound menus are only ever opened on a Sound
	const auto* sound = static_cast<Sound*>(modControllable);
	return Patch::isUsedBy(param, sound->sources[0].drumPatch.engine());
}

using deluge::dsp::drum_synth::XiaControls;

/// Set a patched param of the sound being edited, as turning its knob would (so it can be undone and automated)
inline void setSoundParam(int32_t p, int32_t value) {
	char memory[MODEL_STACK_MAX_SIZE];
	ModelStackWithAutoParam* modelStack = soundEditor.getCurrentModelStack(memory)->getPatchedAutoParamFromId(p);
	modelStack->autoParam->setCurrentValueInResponseToUserInput(value, modelStack);
}

/// Custom 2 / Custom 3 (the last gold knob pair) on XIA rows carry the engine's two key settings
inline void setXiaKnobs(Sound& sound, Engine engine) {
	using deluge::dsp::drum_synth::kXiaControlParams;
	namespace params = deluge::modulation::params;
	if (engine == Engine::TIA) {
		sound.modKnobs[7][1].paramDescriptor.setToHaveParamOnly(kXiaControlParams[XiaControls::TIA_BLENDEX1]);
		sound.modKnobs[7][0].paramDescriptor.setToHaveParamOnly(kXiaControlParams[XiaControls::TIA_FREQ1]);
	}
	else if (engine == Engine::IIA) {
		sound.modKnobs[7][1].paramDescriptor.setToHaveParamOnly(kXiaControlParams[XiaControls::IIA_WEIGHT]);
		sound.modKnobs[7][0].paramDescriptor.setToHaveParamOnly(kXiaControlParams[XiaControls::IIA_ALLOY]);
	}
	else {
		// The Deluge's defaults for that pair
		sound.modKnobs[7][1].paramDescriptor.setToHaveParamOnly(params::UNPATCHED_START
		                                                        + params::UNPATCHED_SAMPLE_RATE_REDUCTION);
		sound.modKnobs[7][0].paramDescriptor.setToHaveParamOnly(params::UNPATCHED_START
		                                                        + params::UNPATCHED_BITCRUSHING);
	}
	view.setKnobIndicatorLevels();
}

/// Put the sound being edited onto a drum synth engine, or move it between engines. A sound that wasn't a drum synth
/// gets a percussive Deluge envelope, which decays both while held and after letting go. An XIA engine starts from the
/// Cycles' defaults, on its control params.
inline void setDrumEngine(Engine engine, uint8_t variant = 0) {
	using deluge::dsp::drum_synth::isXia;
	using deluge::dsp::drum_synth::kXiaControlParams;
	namespace params = deluge::modulation::params;
	Sound& sound = *soundEditor.currentSound;
	Source& source = sound.sources[0];
	const bool wasDrumSynth = source.oscType == OscType::DRUM_SYNTH;
	const Engine from = source.drumPatch.engine();

	if (!wasDrumSynth) {
		sound.killAllVoices();
		if (sound.getSynthMode() != SynthMode::SUBTRACTIVE) {
			sound.setSynthMode(SynthMode::SUBTRACTIVE, currentSong);
		}
		source.setOscType(OscType::DRUM_SYNTH);
		setSoundParam(params::LOCAL_ENV_0_ATTACK, getParamFromUserValue(params::LOCAL_ENV_0_ATTACK, 0));
		setSoundParam(params::LOCAL_ENV_0_DECAY, getParamFromUserValue(params::LOCAL_ENV_0_DECAY, 30));
		setSoundParam(params::LOCAL_ENV_0_SUSTAIN, getParamFromUserValue(params::LOCAL_ENV_0_SUSTAIN, 0));
		setSoundParam(params::LOCAL_ENV_0_RELEASE, getParamFromUserValue(params::LOCAL_ENV_0_RELEASE, 30));
	}

	const bool changed = !wasDrumSynth || engine != from;
	if (isXia(engine)) {
		if (changed) {
			source.drumPatch.setXiaPreset(engine, variant);
			const XiaControls defaults = XiaControls::defaultsFor(engine);
			for (int32_t i = 0; i < XiaControls::NUM_SLOTS; i++) {
				setSoundParam(kXiaControlParams[i], defaults.values[i]);
			}
		}
	}
	else if (changed && (!wasDrumSynth || isXia(from))) {
		source.drumPatch.setPreset(static_cast<deluge::dsp::drum_synth::Preset>(engine == Engine::SNARE   ? 1
		                                                                        : engine == Engine::METAL ? 6
		                                                                                                  : 0));
	}
	else if (changed) {
		source.drumPatch.set(Patch::ENGINE, static_cast<uint8_t>(engine));
	}

	if (isXia(engine) || isXia(from)) {
		setXiaKnobs(sound, engine);
	}
	soundEditor.markInstrumentAsEdited();
}

/// Back from a drum synth to the Deluge's own oscillators: a sample on a kit row, a saw on a synth
inline void leaveDrumSynth() {
	using deluge::dsp::drum_synth::isXia;
	Sound& sound = *soundEditor.currentSound;
	Source& source = sound.sources[0];
	if (source.oscType != OscType::DRUM_SYNTH) {
		return;
	}
	const bool wasXia = isXia(source.drumPatch.engine());
	sound.killAllVoices();
	source.setOscType(soundEditor.editingKitRow() ? OscType::SAMPLE : OscType::SAW);
	if (wasXia) {
		setXiaKnobs(sound, Engine::DRUM);
	}
	soundEditor.markInstrumentAsEdited();
}

/// One XIA control, edited 0-127 like the Cycles knob. It is a real patched param underneath, so it can be parameter
/// locked, automated, MIDI learned and put on a gold knob.
class XiaControl final : public patched_param::Integer {
public:
	/// engine NUM_ENGINES: shown for both XIA engines
	XiaControl(l10n::String name, Engine engine, XiaControls::Slot slot)
	    : patched_param::Integer(name, deluge::dsp::drum_synth::kXiaControlParams[slot]), engine_(engine) {}

	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		const auto* sound = static_cast<Sound*>(modControllable);
		const Engine engine = sound->sources[0].drumPatch.engine();
		return sound->sources[0].oscType == OscType::DRUM_SYNTH
		       && ((engine_ == Engine::NUM_ENGINES) ? deluge::dsp::drum_synth::isXia(engine) : engine == engine_);
	}

protected:
	void readCurrentValue() override {
		const int32_t raw = soundEditor.currentParamManager->getPatchedParamSet()->getValue(getP());
		setValue(std::clamp<int32_t>((XiaControls::toKnob(raw) + 0x80) >> 8, 0, 127));
	}
	int32_t getFinalValue() override { return XiaControls::fromValue(getValue()); }

private:
	Engine engine_;
};

/// A drum synth setting edited as its raw 0-127 value, the way the LXR shows it
class Value final : public Integer {
public:
	Value(l10n::String name, Patch::Param param) : Integer(name), param_(param) {}

	void readCurrentValue() override { setValue(currentPatch().get(param_)); }
	void writeCurrentValue() override { currentPatch().set(param_, getValue()); }
	[[nodiscard]] int32_t getMaxValue() const override { return Patch::maxValue(param_); }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return usedBySound(modControllable, param_);
	}

private:
	Patch::Param param_;
};

/// A drum synth setting picked from named options
class Choice : public Selection {
public:
	Choice(l10n::String name, Patch::Param param) : Selection(name), param_(param) {}

	void readCurrentValue() override { setValue(currentPatch().get(param_)); }
	void writeCurrentValue() override { currentPatch().set(param_, getValue()); }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return usedBySound(modControllable, param_);
	}

protected:
	Patch::Param param_;
};

class Wave final : public Choice {
public:
	using Choice::Choice;

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {l10n::getView(STRING_FOR_SINE), l10n::getView(STRING_FOR_TRIANGLE), l10n::getView(STRING_FOR_SAW),
		        l10n::getView(STRING_FOR_SQUARE), l10n::getView(STRING_FOR_NOISE)};
	}
};

class EngineSelect final : public Choice {
public:
	explicit EngineSelect(l10n::String name) : Choice(name, Patch::ENGINE) {}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {l10n::getView(STRING_FOR_DRUM_ENGINE_DRUM), l10n::getView(STRING_FOR_DRUM_ENGINE_SNARE),
		        l10n::getView(STRING_FOR_DRUM_ENGINE_METAL), l10n::getView(STRING_FOR_DRUM_ENGINE_TIA),
		        l10n::getView(STRING_FOR_DRUM_ENGINE_IIA)};
	}

	void writeCurrentValue() override { setDrumEngine(static_cast<Engine>(getValue())); }
};

/// The Model:Cycles variant of the current XIA engine, under its Cycles name
class XiaVariant final : public Choice {
public:
	explicit XiaVariant(l10n::String name) : Choice(name, Patch::XIA_VARIANT) {}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		if (currentPatch().engine() == Engine::IIA) {
			return {"G", "I", "H"};
		}
		return {"L", "B", "C"};
	}
};

class OffOn final : public Choice {
public:
	using Choice::Choice;

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {l10n::getView(STRING_FOR_OFF), l10n::getView(STRING_FOR_ON)};
	}
};

class MixMode final : public Choice {
public:
	explicit MixMode(l10n::String name) : Choice(name, Patch::MIX_MODE) {}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {l10n::getView(STRING_FOR_FM), l10n::getView(STRING_FOR_DRUM_MIX)};
	}
};

class NoiseFilterType final : public Choice {
public:
	explicit NoiseFilterType(l10n::String name) : Choice(name, Patch::NOISE_FILTER_TYPE) {}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {l10n::getView(STRING_FOR_LPF), l10n::getView(STRING_FOR_HPF),
		        l10n::getView(STRING_FOR_DRUM_FILTER_BAND), l10n::getView(STRING_FOR_OFF)};
	}
};

/// Snap, offset, or one of the twelve LXR transient samples
class TransientMode final : public Choice {
public:
	explicit TransientMode(l10n::String name) : Choice(name, Patch::TRANS_MODE) {}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		static constexpr std::array<char const*, deluge::dsp::drum_synth::kNumTransientModes> kNames{
		    "Snap",    "Offset",  "Click 1", "Click 2", "Click 3",  "Click 4",  "Click 5",
		    "Click 6", "Click 7", "Click 8", "Click 9", "Click 10", "Click 11", "Click 12",
		};
		static constexpr std::array<char const*, deluge::dsp::drum_synth::kNumTransientModes> kShortNames{
		    "SNAP", "OFFS", "CL1", "CL2", "CL3", "CL4", "CL5", "CL6", "CL7", "CL8", "CL9", "CL10", "CL11", "CL12",
		};
		const auto& names = display->haveOLED() ? kNames : kShortNames;
		deluge::vector<std::string_view> options;
		for (char const* name : names) {
			options.emplace_back(name);
		}
		return options;
	}
};

/// Replaces this row's drum synth settings with one of the starting points. Leaves the Deluge side of the sound
/// (envelopes, filters, FX) alone.
class PresetSelect final : public Selection {
public:
	using Selection::Selection;

	void readCurrentValue() override { setValue(0); }
	void writeCurrentValue() override {}
	MenuItem* selectButtonPress() override {
		using deluge::dsp::drum_synth::Preset;
		constexpr int32_t kKitPresets = static_cast<int32_t>(Preset::NUM_PRESETS);
		const int32_t choice = getValue();
		if (choice < kKitPresets) {
			const bool wasXia = deluge::dsp::drum_synth::isXia(currentPatch().engine());
			currentPatch().setPreset(static_cast<Preset>(choice));
			if (wasXia) {
				setXiaKnobs(*soundEditor.currentSound, currentPatch().engine());
			}
		}
		else {
			// After the kit presets come the XIA engines' variants, with the Cycles' own defaults
			const int32_t xia = choice - kKitPresets;
			const Engine engine = (xia < 3) ? Engine::TIA : Engine::IIA;
			// Force a fresh start, even on the same engine
			currentPatch().set(Patch::ENGINE, static_cast<uint8_t>(Engine::DRUM));
			setDrumEngine(engine, xia % 3);
		}
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_DRUM_PRESET_LOADED));
		return nullptr; // Back up to the drum synth menu
	}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {
		    l10n::getView(STRING_FOR_DRUM_PRESET_KICK),       l10n::getView(STRING_FOR_DRUM_PRESET_SNARE),
		    l10n::getView(STRING_FOR_DRUM_PRESET_CLOSED_HAT), l10n::getView(STRING_FOR_DRUM_PRESET_OPEN_HAT),
		    l10n::getView(STRING_FOR_DRUM_PRESET_LOW_TOM),    l10n::getView(STRING_FOR_DRUM_PRESET_HIGH_TOM),
		    l10n::getView(STRING_FOR_DRUM_PRESET_CYMBAL),     l10n::getView(STRING_FOR_DRUM_PRESET_TIA_L),
		    l10n::getView(STRING_FOR_DRUM_PRESET_TIA_B),      l10n::getView(STRING_FOR_DRUM_PRESET_TIA_C),
		    l10n::getView(STRING_FOR_DRUM_PRESET_IIA_G),      l10n::getView(STRING_FOR_DRUM_PRESET_IIA_I),
		    l10n::getView(STRING_FOR_DRUM_PRESET_IIA_H),
		};
	}
};

} // namespace deluge::gui::menu_item::drum_synth
