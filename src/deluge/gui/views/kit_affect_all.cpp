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

#include "gui/views/kit_affect_all.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/menus.h"
#include "gui/ui/sound_editor.h"
#include "gui/ui/ui.h"
#include "gui/views/instrument_clip_view.h"
#include "gui/views/view.h"
#include "hid/display/display.h"
#include "hid/led/indicator_leds.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/song/song.h"
#include "modulation/automation/auto_param.h"
#include "modulation/params/param_collection.h"
#include "playback/playback_handler.h"
#include "processing/sound/drum_synth_params.h"
#include "processing/sound/sound_drum.h"
#include <algorithm>
#include <limits>

namespace params = deluge::modulation::params;

namespace deluge::kit_affect_all {

KitAffectAll kitAffectAll{};

namespace {

/// The rows affect all moves: drum synths, which all share one knob and pad layout
bool isDrumSynthRow(Drum* drum) {
	return drum != nullptr && drum->type == DrumType::SOUND
	       && static_cast<SoundDrum*>(drum)->sources[0].oscType == OscType::DRUM_SYNTH;
}

/// A kit SHIFT + KIT made (LXR, Cycles or GIN), whatever rows have been added to it since
bool isPresetKit(Kit* kit) {
	for (Drum* drum = kit->firstDrum; drum != nullptr; drum = drum->next) {
		if (drum->type == DrumType::SOUND
		    && static_cast<SoundDrum*>(drum)->presetKit != deluge::dsp::drum_synth::PresetKit::NONE) {
			return true;
		}
	}
	return false;
}

Drum* firstDrumSynthRow(Kit* kit) {
	for (Drum* drum = kit->firstDrum; drum != nullptr; drum = drum->next) {
		if (isDrumSynthRow(drum)) {
			return drum;
		}
	}
	return nullptr;
}

bool isRowParam(params::Kind kind) {
	return kind == params::Kind::PATCHED || kind == params::Kind::UNPATCHED_SOUND;
}

bool isKitParam(params::Kind kind) {
	return kind == params::Kind::UNPATCHED_GLOBAL || kind == params::Kind::UNPATCHED_SOUND;
}

/// The model stack for a drum row of this clip, or nullptr if the row isn't in the clip
ModelStackWithThreeMainThings* rowModelStack(void* memory, InstrumentClip* clip, SoundDrum* drum) {
	int32_t noteRowIndex;
	NoteRow* noteRow = clip->getNoteRowForDrum(drum, &noteRowIndex);
	if (noteRow == nullptr) {
		return nullptr;
	}
	return setupModelStackWithThreeMainThingsIncludingNoteRow(memory, currentSong, clip, noteRowIndex, noteRow, drum,
	                                                          &noteRow->paramManager);
}

/// With no clip in the stack, setting the param never records automation: the snapshot must be able to put it all back
ModelStackWithAutoParam* withoutAutomation(ModelStackWithAutoParam* modelStack) {
	if (modelStack == nullptr || modelStack->autoParam == nullptr) {
		return nullptr;
	}
	modelStack->setTimelineCounter(nullptr);
	return modelStack;
}

ModelStackWithAutoParam* paramOnRow(void* memory, InstrumentClip* clip, SoundDrum* drum, params::Kind kind,
                                    int32_t paramId) {
	ModelStackWithThreeMainThings* modelStack = rowModelStack(memory, clip, drum);
	if (modelStack == nullptr) {
		return nullptr;
	}
	return withoutAutomation((kind == params::Kind::PATCHED) ? modelStack->getPatchedAutoParamFromId(paramId)
	                                                         : modelStack->getUnpatchedAutoParamFromId(paramId));
}

/// Whatever this gold knob sets on the row, on the row's own engine, if it's a param affect all can move
ModelStackWithAutoParam* knobOnRow(void* memory, InstrumentClip* clip, SoundDrum* drum, int32_t whichModEncoder) {
	ModelStackWithThreeMainThings* modelStack = rowModelStack(memory, clip, drum);
	if (modelStack == nullptr) {
		return nullptr;
	}
	ModelStackWithAutoParam* withParam = drum->getParamFromModEncoder(whichModEncoder, modelStack, true);
	if (withParam == nullptr || withParam->autoParam == nullptr
	    || !isRowParam(withParam->paramCollection->getParamKind())) {
		return nullptr;
	}
	return withoutAutomation(withParam);
}

ModelStackWithAutoParam* paramOnKit(void* memory, InstrumentClip* clip, Kit* kit, int32_t paramId) {
	ModelStackWithThreeMainThings* modelStack =
	    setupModelStackWithThreeMainThingsButNoNoteRow(memory, currentSong, kit, clip, &clip->paramManager);
	return withoutAutomation(modelStack->getUnpatchedAutoParamFromId(paramId));
}

/// The param on another row doing the job a menu just edited on this one. Engines keep their controls on different
/// params, so a drum control is matched by the shortcut pad, then the gold knob, it sits on. Every other param means
/// the same on every row. kNoParamID where the other row's engine has nothing there.
uint32_t sameJobOn(SoundDrum* from, SoundDrum* to, params::Kind kind, int32_t paramId) {
	if (kind != params::Kind::PATCHED || deluge::dsp::drum_synth::drumSlotForParam(paramId) < 0
	    || from->sources[0].drumPatch.engine() == to->sources[0].drumPatch.engine()) {
		return paramId;
	}
	for (int32_t x = 0; x < kDisplayWidth; x++) {
		for (int32_t y = 0; y < kDisplayHeight; y++) {
			if (drumShortcutParam(from, x, y) == static_cast<uint32_t>(paramId)) {
				return drumShortcutParam(to, x, y);
			}
		}
	}
	for (int32_t mode = 0; mode < kNumModButtons; mode++) {
		for (int32_t knob = 0; knob < kNumPhysicalModKnobs; knob++) {
			if (from->modKnobs[mode][knob].paramDescriptor.isSetToParamWithNoSource(paramId)) {
				const ParamDescriptor& theirs = to->modKnobs[mode][knob].paramDescriptor;
				if (theirs.isJustAParam() && theirs.getJustTheParam() < params::UNPATCHED_START) {
					return theirs.getJustTheParam();
				}
				return params::kNoParamID;
			}
		}
	}
	return params::kNoParamID;
}

int32_t clampToParam(int64_t value) {
	return static_cast<int32_t>(
	    std::clamp<int64_t>(value, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()));
}

} // namespace

void KitAffectAll::shiftAffectEntirePressed(InstrumentClip* clip) {
	if (leaving_ && isActiveFor(clip)) {
		return; // Already going back at the next bar
	}
	if (isActiveFor(clip)) {
		leave();
		return;
	}
	if (clip_ != nullptr) {
		restore(); // Another clip was in the mode: put it back first
	}

	Kit* kit = static_cast<Kit*>(clip->output);
	Drum* firstRow = firstDrumSynthRow(kit);
	if (!isPresetKit(kit) || firstRow == nullptr) {
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_AFFECT_ALL_PRESET_KITS_ONLY));
		return;
	}

	clip_ = clip;
	kit_ = kit;
	song_ = currentSong;
	affectEntireBefore_ = clip->affectEntire;
	leaving_ = false;

	// Start on all voices: the gold knobs go to the drum rows instead of the kit's FX
	clip->affectEntire = false;
	if (!isDrumSynthRow(kit->selectedDrum)) {
		instrumentClipView.setSelectedDrum(firstRow);
	}
	view.setActiveModControllableTimelineCounter(clip);

	display->displayPopup(l10n::get(l10n::String::STRING_FOR_AFFECT_ALL));
	refreshLed();
}

bool KitAffectAll::affectEntirePressed(InstrumentClip* clip) {
	if (!isActiveFor(clip)) {
		return false;
	}
	clip->affectEntire = !clip->affectEntire;
	if (!clip->affectEntire && !isDrumSynthRow(kit_->selectedDrum)) {
		instrumentClipView.setSelectedDrum(firstDrumSynthRow(kit_));
	}
	view.setActiveModControllableTimelineCounter(clip);
	display->displayPopup(l10n::get(clip->affectEntire ? l10n::String::STRING_FOR_AFFECT_ALL_KIT_FX
	                                                   : l10n::String::STRING_FOR_AFFECT_ALL_VOICES));
	refreshLed();
	return true;
}

void KitAffectAll::leave() {
	if (playbackHandler.isEitherClockActive() && !playbackHandler.ticksLeftInCountIn) {
		leaving_ = true;
		// Make sure playback actions a tick on the next bar line, even if nothing else plays there
		int32_t barLength = currentSong->getBarLength();
		int64_t nextBar = (playbackHandler.getActualSwungTickCount() / barLength + 1) * barLength;
		int64_t ticksTilBar = nextBar - playbackHandler.lastSwungTickActioned;
		if (ticksTilBar > 0 && ticksTilBar < playbackHandler.swungTicksTilNextEvent) {
			playbackHandler.swungTicksTilNextEvent = static_cast<int32_t>(ticksTilBar);
			playbackHandler.scheduleSwungTick();
		}
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_AFFECT_ALL_LEAVING));
		refreshLed();
	}
	else {
		restore();
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_AFFECT_ALL_RESTORED));
	}
}

bool KitAffectAll::isActiveFor(Clip const* clip) const {
	return clip != nullptr && clip == clip_ && song_ == currentSong;
}

bool KitAffectAll::stillValid() const {
	if (clip_ == nullptr || song_ != currentSong) {
		return false;
	}
	if (currentSong->sessionClips.getIndexForClip(clip_) < 0
	    && currentSong->arrangementOnlyClips.getIndexForClip(clip_) < 0) {
		return false;
	}
	return clip_->output == kit_;
}

SoundDrum* KitAffectAll::drumRowFor(ModelStackWithAutoParam const* modelStack) const {
	if (!modelStack->timelineCounterIsSet() || modelStack->getTimelineCounter() != clip_) {
		return nullptr;
	}
	for (Drum* drum = kit_->firstDrum; drum != nullptr; drum = drum->next) {
		if (isDrumSynthRow(drum)
		    && static_cast<ModControllable*>(static_cast<SoundDrum*>(drum)) == modelStack->modControllable) {
			return static_cast<SoundDrum*>(drum);
		}
	}
	return nullptr;
}

bool KitAffectAll::isKitFx(ModelStackWithAutoParam const* modelStack) const {
	return modelStack->timelineCounterIsSet() && modelStack->getTimelineCounter() == clip_
	       && modelStack->modControllable == static_cast<ModControllable*>(kit_)
	       && isKitParam(modelStack->paramCollection->getParamKind());
}

KitAffectAll::Entry& KitAffectAll::entryFor(SoundDrum* drum, ModelStackWithAutoParam const* modelStack,
                                            int32_t valueBefore) {
	params::Kind kind = modelStack->paramCollection->getParamKind();
	for (Entry& entry : snapshot_) {
		if (entry.drum == drum && entry.kind == kind && entry.paramId == modelStack->paramId) {
			return entry;
		}
	}
	snapshot_.push_back({drum, kind, modelStack->paramId, valueBefore, 0});
	return snapshot_.back();
}

void KitAffectAll::moveBy(Entry& entry, ModelStackWithAutoParam* modelStack, int64_t amount) {
	// Each row can go anywhere from its snapshot
	constexpr int64_t kFullSweep = (int64_t)1 << 32;
	entry.moved = std::clamp<int64_t>(entry.moved + amount, -kFullSweep, kFullSweep);
	modelStack->autoParam->setCurrentValueInResponseToUserInput(clampToParam((int64_t)entry.value + entry.moved),
	                                                            modelStack, false);
}

bool KitAffectAll::modEncoderTurned(ModelStackWithAutoParam* modelStack, int32_t whichModEncoder, int32_t offset) {
	if (clip_ == nullptr || !stillValid()) {
		return false;
	}
	params::Kind kind = modelStack->paramCollection->getParamKind();

	// Kit FX: the normal knob move, remembered for the snapshot and never recorded
	if (isKitFx(modelStack)) {
		int32_t value = modelStack->autoParam->getCurrentValue();
		entryFor(nullptr, modelStack, value);
		int32_t knobPos = modelStack->paramCollection->paramValueToKnobPos(value, modelStack);
		int32_t newKnobPos = view.calculateKnobPosForModEncoderTurn(kind, knobPos, offset);
		view.displayModEncoderValuePopup(kind, modelStack->paramId, newKnobPos);
		if (newKnobPos != knobPos) {
			char modelStackMemory[MODEL_STACK_MAX_SIZE];
			copyModelStack(modelStackMemory, modelStack, sizeof(ModelStackWithAutoParam));
			auto* noRecording = withoutAutomation((ModelStackWithAutoParam*)modelStackMemory);
			noRecording->autoParam->setCurrentValueInResponseToUserInput(
			    noRecording->paramCollection->knobPosToParamValue(newKnobPos, noRecording), noRecording, false);
			view.sendMidiFollowFeedback(modelStack, newKnobPos);
		}
		return true;
	}

	// All voices: every drum synth row's own control on this knob moves the same number of knob steps
	if (!isRowParam(kind) || drumRowFor(modelStack) == nullptr) {
		return false;
	}
	for (Drum* drum = kit_->firstDrum; drum != nullptr; drum = drum->next) {
		if (!isDrumSynthRow(drum)) {
			continue;
		}
		auto* row = static_cast<SoundDrum*>(drum);
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithAutoParam* onRow = knobOnRow(modelStackMemory, clip_, row, whichModEncoder);
		if (onRow == nullptr) {
			continue;
		}
		// One knob step in param value units, as this param's knob maps them
		int64_t step = (int64_t)onRow->paramCollection->knobPosToParamValue(1, onRow)
		               - onRow->paramCollection->knobPosToParamValue(0, onRow);
		Entry& entry = entryFor(row, onRow, onRow->autoParam->getCurrentValue());
		moveBy(entry, onRow, step * offset);
	}

	int32_t knobPos =
	    modelStack->paramCollection->paramValueToKnobPos(modelStack->autoParam->getCurrentValue(), modelStack);
	view.displayModEncoderValuePopup(kind, modelStack->paramId, knobPos);
	view.sendMidiFollowFeedback(modelStack, knobPos);
	return true;
}

void KitAffectAll::menuValueChanged(ModelStackWithAutoParam const* modelStack, int32_t oldValue) {
	if (clip_ == nullptr || !stillValid()) {
		return;
	}
	// Kit FX: just remember where it was
	if (isKitFx(modelStack)) {
		entryFor(nullptr, modelStack, oldValue);
		return;
	}

	params::Kind kind = modelStack->paramCollection->getParamKind();
	SoundDrum* activeRow = drumRowFor(modelStack);
	if (!isRowParam(kind) || activeRow == nullptr) {
		return;
	}
	// The menu has already set the row it's on, from oldValue
	int64_t delta = (int64_t)modelStack->autoParam->getCurrentValue() - oldValue;
	Entry& own = entryFor(activeRow, modelStack, oldValue);
	own.moved += delta;

	for (Drum* drum = kit_->firstDrum; drum != nullptr; drum = drum->next) {
		if (!isDrumSynthRow(drum) || drum == activeRow) {
			continue;
		}
		auto* row = static_cast<SoundDrum*>(drum);
		uint32_t paramId = sameJobOn(activeRow, row, kind, modelStack->paramId);
		if (paramId == params::kNoParamID) {
			continue;
		}
		char modelStackMemory[MODEL_STACK_MAX_SIZE];
		ModelStackWithAutoParam* onRow = paramOnRow(modelStackMemory, clip_, row, kind, static_cast<int32_t>(paramId));
		if (onRow == nullptr) {
			continue;
		}
		Entry& entry = entryFor(row, onRow, onRow->autoParam->getCurrentValue());
		moveBy(entry, onRow, delta);
	}
}

void KitAffectAll::restore() {
	InstrumentClip* clip = clip_;
	bool valid = stillValid();
	if (valid) {
		for (Entry const& entry : snapshot_) {
			char modelStackMemory[MODEL_STACK_MAX_SIZE];
			ModelStackWithAutoParam* modelStack = nullptr;
			if (entry.drum == nullptr) {
				modelStack = paramOnKit(modelStackMemory, clip_, kit_, entry.paramId);
			}
			else {
				bool rowStillThere = false;
				for (Drum* drum = kit_->firstDrum; drum != nullptr; drum = drum->next) {
					rowStillThere |= (drum == entry.drum);
				}
				if (rowStillThere) {
					modelStack = paramOnRow(modelStackMemory, clip_, entry.drum, entry.kind, entry.paramId);
				}
			}
			if (modelStack != nullptr) {
				modelStack->autoParam->setCurrentValueInResponseToUserInput(entry.value, modelStack, false);
			}
		}
		clip_->affectEntire = affectEntireBefore_;
	}

	forget();

	indicator_leds::stopLedBlinking(IndicatorLED::AFFECT_ENTIRE);
	if (valid && rootUIIsClipMinderScreen() && getCurrentClip() == clip && getCurrentUI() != &soundEditor) {
		view.setActiveModControllableTimelineCounter(clip); // Knobs back to the kit's FX if they were there
	}
	else if (getCurrentUI() != &soundEditor) {
		view.setModLedStates();
	}
}

void KitAffectAll::forget() {
	clip_ = nullptr;
	kit_ = nullptr;
	song_ = nullptr;
	leaving_ = false;
	snapshot_.clear();
}

int32_t KitAffectAll::processTick(int64_t swungTick) {
	if (!leaving_) {
		return std::numeric_limits<int32_t>::max();
	}
	int32_t barLength = currentSong->getBarLength();
	int32_t intoBar = static_cast<int32_t>(swungTick % barLength);
	if (intoBar == 0) {
		restore(); // Before the clips play the bar's first hits
		return std::numeric_limits<int32_t>::max();
	}
	return barLength - intoBar;
}

void KitAffectAll::playbackStopped() {
	if (leaving_) {
		restore();
	}
}

void KitAffectAll::refreshLed() {
	if (rootUIIsClipMinderScreen() && isActiveFor(getCurrentClip())) {
		indicator_leds::blinkLed(IndicatorLED::AFFECT_ENTIRE, 255, leaving_ ? 1 : 0);
	}
}

} // namespace deluge::kit_affect_all
