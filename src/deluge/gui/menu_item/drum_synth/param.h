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

using deluge::dsp::drum_synth::DrumControls;

using deluge::dsp::drum_synth::cycEngineName;
using deluge::dsp::drum_synth::cycKnobName;

/// Set a patched param of the sound being edited, as turning its knob would (so it can be undone and automated)
inline void setSoundParam(int32_t p, int32_t value) {
	char memory[MODEL_STACK_MAX_SIZE];
	ModelStackWithAutoParam* modelStack = soundEditor.getCurrentModelStack(memory)->getPatchedAutoParamFromId(p);
	modelStack->autoParam->setCurrentValueInResponseToUserInput(value, modelStack);
}

/// Custom 2 / Custom 3 (the last gold knob pair) carry the engine's two key settings. NUM_ENGINES puts the Deluge's
/// defaults back.
inline void setDrumKnobs(Sound& sound, Engine engine) {
	sound.setDrumSynthKnobs(engine);
	view.setKnobIndicatorLevels();
}

inline DrumControls currentControls() {
	return deluge::dsp::drum_synth::readDrumControls(*soundEditor.currentParamManager->getPatchedParamSet());
}

inline void setControls(const DrumControls& controls) {
	for (int32_t i = 0; i < DrumControls::NUM_SLOTS; i++) {
		setSoundParam(deluge::dsp::drum_synth::kDrumControlParams[i], controls.values[i]);
	}
}

/// Envelope 2 to pitch on the sound being edited, as a patch cable user value. 0 takes the cable away.
inline void setPitchCable(int32_t userValue) {
	namespace params = deluge::modulation::params;
	ParamDescriptor descriptor;
	descriptor.setToHaveParamAndSource(params::LOCAL_PITCH_ADJUST, PatchSource::ENVELOPE_1);
	char memory[MODEL_STACK_MAX_SIZE];
	ModelStackWithThreeMainThings* modelStack = soundEditor.getCurrentModelStack(memory);
	ParamCollectionSummary* summary = modelStack->paramManager->getPatchCableSetSummary();
	ModelStackWithParamId* modelStackWithParamId =
	    modelStack->addParamCollectionSummary(summary)->addParamId(descriptor.data);
	ModelStackWithAutoParam* modelStackWithParam =
	    summary->paramCollection->getAutoParamFromId(modelStackWithParamId, userValue != 0);
	if (modelStackWithParam->autoParam != nullptr) {
		modelStackWithParam->autoParam->setCurrentValueInResponseToUserInput(
		    getParamFromUserValue(params::PATCH_CABLE, userValue), modelStackWithParam);
	}
}

/// Set the sound being edited up as a drum synth row sits in its kit: transpose, both envelopes as percussive
/// one-shots, envelope 2 on pitch where the engine wants a drop, polyphony, filters open (the high pass on for hats and
/// cymbals), osc 2 and noise off, tune centred. Whatever the row was before (a sample, an LXR kick tuned down 29
/// semitones, a hat behind a high pass), the engine then sounds as it does in its kit.
inline void applyRowSetup(const deluge::dsp::drum_synth::RowSetup& row) {
	namespace params = deluge::modulation::params;
	Sound& sound = *soundEditor.currentSound;
	sound.killAllVoices();
	sound.polyphonic = row.choke ? PolyphonyMode::CHOKE : PolyphonyMode::AUTO;
	sound.sources[0].transpose = row.transpose;
	sound.sources[0].setCents(row.cents);

	auto user = [](int32_t p, int32_t value) { setSoundParam(p, getParamFromUserValue(p, value)); };
	user(params::LOCAL_ENV_0_ATTACK, 0);
	user(params::LOCAL_ENV_0_DECAY, row.decay);
	user(params::LOCAL_ENV_0_SUSTAIN, row.sustain);
	user(params::LOCAL_ENV_0_RELEASE, row.release);
	user(params::LOCAL_ENV_1_ATTACK, 0);
	user(params::LOCAL_ENV_1_DECAY, row.env2Decay);
	user(params::LOCAL_ENV_1_SUSTAIN, 0);
	user(params::LOCAL_ENV_1_RELEASE, 0);
	setPitchCable(row.pitchCable);
	setSoundParam(params::LOCAL_LPF_FREQ, 2147483647);
	setSoundParam(params::LOCAL_LPF_RESONANCE, -2147483648);
	if (row.hpf >= 0) {
		user(params::LOCAL_HPF_FREQ, row.hpf);
	}
	else {
		setSoundParam(params::LOCAL_HPF_FREQ, -2147483648);
	}
	setSoundParam(params::LOCAL_HPF_RESONANCE, -2147483648);
	setSoundParam(params::LOCAL_OSC_A_VOLUME, 2147483647);
	setSoundParam(params::LOCAL_OSC_B_VOLUME, -2147483648);
	setSoundParam(params::LOCAL_NOISE_VOLUME, -2147483648);
	setSoundParam(params::LOCAL_OSC_A_PITCH_ADJUST, 0);
	setSoundParam(params::LOCAL_PITCH_ADJUST, 0);
}

/// The LXR voices' own decay, which a row setup sets after their controls
inline void setEngineDecay(const deluge::dsp::drum_synth::RowSetup& row) {
	if (row.engineDecay >= 0) {
		setSoundParam(deluge::dsp::drum_synth::kDrumControlParams[DrumControls::COMMON_DECAY],
		              DrumControls::fromValue(row.engineDecay));
	}
}

/// Put the sound being edited onto a drum synth engine, or move it between engines. Each new engine starts from its
/// defaults, set up as its kit row is (applyRowSetup), so what SYNTH MODE previews is what the kit plays: the Cyc
/// engines as in the Cycles kit, the LXR voices as their kick, snare and cymbal rows. A sound that wasn't a drum synth
/// also gets envelope 2 as a short blip to patch to pitch for a snap. Moving between variants of DXIA or DIIA keeps
/// their settings. fresh: start the engine from its defaults even if it's already playing.
inline void setDrumEngine(Engine engine, uint8_t variant = 0, bool fresh = false) {
	using deluge::dsp::drum_synth::isCyc;
	using deluge::dsp::drum_synth::isGin;
	using deluge::dsp::drum_synth::isXia;
	Sound& sound = *soundEditor.currentSound;
	Source& source = sound.sources[0];
	Patch& patch = source.drumPatch;
	const bool wasDrumSynth = source.oscType == OscType::DRUM_SYNTH;
	const Engine from = patch.engine();

	if (!wasDrumSynth) {
		sound.killAllVoices();
		if (sound.getSynthMode() != SynthMode::SUBTRACTIVE) {
			sound.setSynthMode(SynthMode::SUBTRACTIVE, currentSong);
		}
		source.setOscType(OscType::DRUM_SYNTH);
		for (const auto& [p, userValue] : deluge::dsp::drum_synth::kDrumSynthEnvelope2) {
			setSoundParam(p, getParamFromUserValue(p, userValue));
		}
	}
	// Moving to another variant of DXIA or DIIA starts that variant's own preset too
	const bool sameVariant = !isXia(engine) || variant == patch.get(Patch::XIA_VARIANT);
	if (wasDrumSynth && engine == from && sameVariant && !fresh) {
		return;
	}

	const deluge::dsp::drum_synth::RowSetup row = deluge::dsp::drum_synth::rowSetupForEngine(engine, variant);
	applyRowSetup(row);
	if (isXia(engine)) {
		patch.setXiaPreset(engine, variant);
	}
	else if (isCyc(engine)) {
		patch.setCycPreset(engine);
	}
	else if (isGin(engine)) {
		patch.setGinSound(deluge::dsp::drum_synth::gin::defaultSound());
	}
	else {
		patch.setPreset(deluge::dsp::drum_synth::presetForLxrEngine(engine));
	}
	patch.set(Patch::SNAPPY_DECAY, deluge::dsp::drum_synth::snappyByDefault(engine) ? 1 : 0);
	setControls(deluge::dsp::drum_synth::controlsFromPatch(patch));
	setEngineDecay(row);
	setDrumKnobs(sound, engine);
	soundEditor.markInstrumentAsEdited();
}

/// Back from a drum synth to the Deluge's own oscillators: a sample on a kit row, a saw on a synth
inline void leaveDrumSynth() {
	Sound& sound = *soundEditor.currentSound;
	Source& source = sound.sources[0];
	if (source.oscType != OscType::DRUM_SYNTH) {
		return;
	}
	deluge::dsp::drum_synth::storeControlsInPatch(currentControls(), source.drumPatch);
	sound.killAllVoices();
	source.setOscType(soundEditor.editingKitRow() ? OscType::SAMPLE : OscType::SAW);
	setDrumKnobs(sound, Engine::NUM_ENGINES);
	soundEditor.markInstrumentAsEdited();
}

/// One drum synth control, edited 0-127 like the LXR's and the Cycles' knobs. It is a real patched param underneath, so
/// it can be parameter locked, automated, MIDI learned and put on a gold knob.
class Control : public patched_param::Integer {
public:
	Control(l10n::String name, int32_t slot)
	    : patched_param::Integer(name, deluge::dsp::drum_synth::kDrumControlParams[slot]) {}

	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] int32_t getMinValue() const override { return 0; }

protected:
	void readCurrentValue() override {
		const int32_t raw = soundEditor.currentParamManager->getPatchedParamSet()->getValue(getP());
		setValue(std::clamp<int32_t>((DrumControls::toKnob(raw) + 0x80) >> 8, 0, 127));
	}
	int32_t getFinalValue() override { return DrumControls::fromValue(getValue()); }
};

/// A control of the XIA engines. engine NUM_ENGINES: shown for both.
class XiaControl final : public Control {
public:
	XiaControl(l10n::String name, Engine engine, DrumControls::Slot slot) : Control(name, slot), engine_(engine) {}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		const auto* sound = static_cast<Sound*>(modControllable);
		const Engine engine = sound->sources[0].drumPatch.engine();
		return sound->sources[0].oscType == OscType::DRUM_SYNTH
		       && ((engine_ == Engine::NUM_ENGINES) ? deluge::dsp::drum_synth::isXia(engine) : engine == engine_);
	}

private:
	Engine engine_;
};

/// A knob of the Cyc engines: one of the four macros, or the amp decay. Named the way the current engine names it.
class CycControl final : public Control {
public:
	CycControl(l10n::String name, DrumControls::Slot slot) : Control(name, slot), slot_(slot) {}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		const auto* sound = static_cast<Sound*>(modControllable);
		return sound->sources[0].oscType == OscType::DRUM_SYNTH
		       && deluge::dsp::drum_synth::isCyc(sound->sources[0].drumPatch.engine());
	}
	[[nodiscard]] std::string_view getName() const override { return l10n::getView(currentName()); }
	[[nodiscard]] std::string_view getTitle() const override { return l10n::getView(currentName()); }

private:
	[[nodiscard]] l10n::String currentName() const {
		const Sound* sound = soundEditor.currentSound;
		return cycKnobName((sound != nullptr) ? sound->sources[0].drumPatch.engine() : Engine::CYC_KIK, slot_);
	}
	DrumControls::Slot slot_;
};

/// A continuous setting of GIN
class GinControl final : public Control {
public:
	using Control::Control;

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		const auto* sound = static_cast<Sound*>(modControllable);
		return sound->sources[0].oscType == OscType::DRUM_SYNTH
		       && deluge::dsp::drum_synth::isGin(sound->sources[0].drumPatch.engine());
	}
};

/// A GIN setting kept in the patch as a number, 0-127 (EQ, level). Menu edits only: not a patched param.
class GinNumber final : public Integer {
public:
	GinNumber(l10n::String name, Patch::Param param) : Integer(name), param_(param) {}

	[[nodiscard]] int32_t getMaxValue() const override { return 127; }
	[[nodiscard]] int32_t getMinValue() const override { return 0; }
	void readCurrentValue() override { setValue(currentPatch().get(param_)); }
	void writeCurrentValue() override { currentPatch().set(param_, getValue()); }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return usedBySound(modControllable, param_);
	}

private:
	Patch::Param param_;
};

/// The LXR voices' drive and decay, which they share with every other engine's on the same pads and gold knobs
class LxrCommonControl final : public Control {
public:
	using Control::Control;

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		const auto* sound = static_cast<Sound*>(modControllable);
		return sound->sources[0].oscType == OscType::DRUM_SYNTH
		       && deluge::dsp::drum_synth::isLxr(sound->sources[0].drumPatch.engine());
	}
};

/// A continuous LXR setting, shown for the LXR engines that use it
class LxrControl final : public Control {
public:
	LxrControl(l10n::String name, Patch::Param param)
	    : Control(name, deluge::dsp::drum_synth::lxrControlSlot(param)), param_(param) {}

	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		const auto* sound = static_cast<Sound*>(modControllable);
		const Engine engine = sound->sources[0].drumPatch.engine();
		return sound->sources[0].oscType == OscType::DRUM_SYNTH && !deluge::dsp::drum_synth::isXia(engine)
		       && Patch::isUsedBy(param_, engine);
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

/// GIN's oscillator wave
class GinWave final : public Choice {
public:
	explicit GinWave(l10n::String name) : Choice(name, Patch::GIN_WAVE) {}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {l10n::getView(STRING_FOR_SINE), l10n::getView(STRING_FOR_TRIANGLE), l10n::getView(STRING_FOR_SAW)};
	}
};

/// What bends GIN's pitch: a decaying sweep, a sine LFO (FM at audio rates), or filtered noise
class GinModMode final : public Choice {
public:
	explicit GinModMode(l10n::String name) : Choice(name, Patch::GIN_MOD_MODE) {}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {l10n::getView(STRING_FOR_GIN_DECAY), l10n::getView(STRING_FOR_SINE), l10n::getView(STRING_FOR_NOISE)};
	}
};

/// GIN's noise envelope: exponential, linear, or modulated (bursts, for claps)
class GinNoiseEnv final : public Choice {
public:
	explicit GinNoiseEnv(l10n::String name) : Choice(name, Patch::GIN_NOISE_ENV) {}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {l10n::getView(STRING_FOR_GIN_ENV_EXP), l10n::getView(STRING_FOR_LINEAR),
		        l10n::getView(STRING_FOR_GIN_ENV_MOD)};
	}
};

/// Puts any sound of the GIN kits on this row, tuned and set up as in its kit
class GinSoundSelect final : public Selection {
public:
	using Selection::Selection;

	void readCurrentValue() override { setValue(0); }
	void writeCurrentValue() override {}
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		const auto* sound = static_cast<Sound*>(modControllable);
		return sound->sources[0].oscType == OscType::DRUM_SYNTH
		       && deluge::dsp::drum_synth::isGin(sound->sources[0].drumPatch.engine());
	}
	MenuItem* selectButtonPress() override {
		namespace gin = deluge::dsp::drum_synth::gin;
		const int32_t choice = std::clamp<int32_t>(getValue(), 0, gin::kNumKits * gin::kNumKitSounds - 1);
		const int32_t row = choice % gin::kNumKitSounds;
		const gin::KitSound& sound = gin::kKits[choice / gin::kNumKitSounds].sounds[row];
		applyRowSetup(deluge::dsp::drum_synth::rowSetupForGinSound(sound, row == gin::HAT1 || row == gin::HAT2));
		currentPatch().setGinSound(sound);
		setControls(deluge::dsp::drum_synth::ginControlsFor(sound));
		setDrumKnobs(*soundEditor.currentSound, Engine::GIN);
		soundEditor.markInstrumentAsEdited();
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_GIN_SOUND_LOADED));
		return nullptr; // Back up to the drum synth menu
	}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		namespace gin = deluge::dsp::drum_synth::gin;
		// "808 KICK" and so on: kit, then row
		static constexpr std::array<char const*, gin::kNumKitSounds> kRows{"KICK",  "SNR",   "HAT1", "HAT2",
		                                                                   "PERC1", "PERC2", "CLAP", "OTHER"};
		static std::array<std::array<char, 12>, gin::kNumKits * gin::kNumKitSounds> names{};
		deluge::vector<std::string_view> options;
		for (int32_t i = 0; i < gin::kNumKits * gin::kNumKitSounds; i++) {
			if (names[i][0] == 0) {
				snprintf(names[i].data(), names[i].size(), "%s %s", gin::kKits[i / gin::kNumKitSounds].name,
				         kRows[i % gin::kNumKitSounds]);
			}
			options.emplace_back(names[i].data());
		}
		return options;
	}
};

/// The steeper amp envelope, on any drum synth sound
class SnappyDecay final : public Choice {
public:
	explicit SnappyDecay(l10n::String name) : Choice(name, Patch::SNAPPY_DECAY) {}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		(void)optType;
		using enum l10n::String;
		return {l10n::getView(STRING_FOR_OFF), l10n::getView(STRING_FOR_ON)};
	}
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		const auto* sound = static_cast<Sound*>(modControllable);
		return sound->sources[0].oscType == OscType::DRUM_SYNTH;
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

/// Replaces this row's drum synth settings with one of the starting points, set up as its kit row is (transpose,
/// envelope, filters). Leaves the FX alone.
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
			const deluge::dsp::drum_synth::RowSetup row =
			    deluge::dsp::drum_synth::rowSetupFor(static_cast<Preset>(choice));
			applyRowSetup(row);
			currentPatch().setPreset(static_cast<Preset>(choice));
			setControls(deluge::dsp::drum_synth::controlsFromPatch(currentPatch()));
			setEngineDecay(row);
			setDrumKnobs(*soundEditor.currentSound, currentPatch().engine());
			soundEditor.markInstrumentAsEdited();
		}
		else {
			// After the kit presets come the XIA engines' variants, with the Cycles' own defaults
			const int32_t xia = choice - kKitPresets;
			setDrumEngine((xia < 3) ? Engine::TIA : Engine::IIA, xia % 3, true);
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
