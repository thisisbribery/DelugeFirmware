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
#include "gui/menu_item/selection.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/display.h"
#include "model/model_stack.h"
#include "modulation/automation/auto_param.h"
#include "modulation/params/param.h"
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

/// The XIA engines bring their own amp envelope. Moving a row onto one opens the Deluge's envelope up, so that a short
/// note doesn't cut the XIA decay, and puts the XIA decay on the amp decay gold knob. Moving back undoes both.
inline void applyEngineChange(Engine from, Engine to) {
	using deluge::dsp::drum_synth::isXia;
	namespace params = deluge::modulation::params;
	if (isXia(from) == isXia(to) || soundEditor.currentSound == nullptr) {
		return;
	}

	auto setParam = [](int32_t p, int32_t userValue) {
		char memory[MODEL_STACK_MAX_SIZE];
		ModelStackWithAutoParam* modelStack = soundEditor.getCurrentModelStack(memory)->getPatchedAutoParamFromId(p);
		modelStack->autoParam->setCurrentValueInResponseToUserInput(getParamFromUserValue(p, userValue), modelStack);
	};

	Sound& sound = *soundEditor.currentSound;
	if (isXia(to)) {
		setParam(params::LOCAL_ENV_0_ATTACK, 0);
		setParam(params::LOCAL_ENV_0_SUSTAIN, 50);
		setParam(params::LOCAL_ENV_0_RELEASE, 40);
		sound.modKnobs[2][1].paramDescriptor.setToHaveParamOnly(params::LOCAL_OSC_B_WAVE_INDEX);
	}
	else {
		setParam(params::LOCAL_ENV_0_DECAY, 25);
		setParam(params::LOCAL_ENV_0_SUSTAIN, 0);
		setParam(params::LOCAL_ENV_0_RELEASE, 0);
		sound.modKnobs[2][1].paramDescriptor.setToHaveParamOnly(params::LOCAL_ENV_0_DECAY);
	}
}

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

	void writeCurrentValue() override {
		const Engine from = currentPatch().engine();
		Choice::writeCurrentValue();
		applyEngineChange(from, currentPatch().engine());
	}
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
		const Engine from = currentPatch().engine();
		const int32_t choice = getValue();
		if (choice < kKitPresets) {
			currentPatch().setPreset(static_cast<Preset>(choice));
		}
		else {
			// After the kit presets come the XIA engines' variants, with the Cycles' own defaults
			const int32_t xia = choice - kKitPresets;
			currentPatch().setXiaPreset((xia < 3) ? Engine::TIA : Engine::IIA, xia % 3);
		}
		applyEngineChange(from, currentPatch().engine());
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
