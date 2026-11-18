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

#include "gui/context_menu/synth_instrument.h"
#include "definitions_cxx.hpp"
#include "dsp/drum_synth/drum_synth.h"
#include "dsp/dx/dx7note.h"
#include "fatfs/ff.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/drum_synth/param.h"
#include "gui/menu_item/synth_mode.h"
#include "gui/ui/load/load_instrument_preset_ui.h"
#include "gui/ui/menus.h"
#include "gui/ui/sound_editor.h"
#include "gui/views/instrument_clip_view.h"
#include "gui/views/view.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/instrument.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "processing/sound/sound_instrument.h"
#include "storage/flash_storage.h"
#include <array>
#include <cstdio>
#include <cstring>
#include <strings.h>

extern deluge::gui::menu_item::SynthModeSelection synthModeMenu;

namespace deluge::gui::context_menu {

using synth_instruments::kSynthInstruments;
using synth_instruments::SynthInstrument;

SynthInstrumentMenu synthInstrumentMenu{};
SynthCategoryMenu synthCategoryMenu{};

namespace {

/// Whether a preset folder has any presets (XML files) directly in it
bool folderHasPresets(char const* folder) {
	DIR dir;
	if (f_opendir(&dir, folder) != FR_OK) {
		return false;
	}
	bool found = false;
	FILINFO fileInfo;
	while (!found && f_readdir(&dir, &fileInfo) == FR_OK && fileInfo.fname[0]) {
		const size_t length = strlen(fileInfo.fname);
		found = !(fileInfo.fattrib & AM_DIR) && fileInfo.fname[0] != '.' && length >= 5
		        && strcasecmp(fileInfo.fname + length - 4, ".XML") == 0;
	}
	f_closedir(&dir);
	return found;
}

/// The folder of a type's category, like SYNTHS/SUBT/BASS
void categoryFolder(size_t type, size_t category, char* out, size_t outSize) {
	snprintf(out, outSize, "%s/%s", kSynthInstruments[type].folder,
	         synth_instruments::categoriesFor(kSynthInstruments[type])[category].folder);
}

size_t numCategories(size_t type) {
	return synth_instruments::categoriesFor(kSynthInstruments[type]).size();
}

/// Whether path is folder or inside it
bool inFolder(char const* path, char const* folder) {
	const size_t length = strlen(folder);
	return !strncasecmp(path, folder, length) && (path[length] == '\0' || path[length] == '/');
}

bool typeAvailable(size_t type) {
	return !kSynthInstruments[type].dx || runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::EnableDX7Engine);
}

/// The type a synth plays: the one whose folder it came from, else the first that sounds like it
size_t typeOf(SoundInstrument* synth) {
	for (size_t i = 0; i < kSynthInstruments.size(); i++) {
		if (!kSynthInstruments[i].init && inFolder(synth->dirPath.get(), kSynthInstruments[i].folder)) {
			return i;
		}
	}
	const bool dx = synth->sources[0].oscType == OscType::DX7;
	const bool rings = synth->sources[0].oscType == OscType::DRUM_SYNTH
	                   && dsp::drum_synth::isRings(synth->sources[0].drumPatch.engine());
	const bool brophet = synth->sources[0].oscType == OscType::DRUM_SYNTH
	                     && dsp::drum_synth::isBrophet(synth->sources[0].drumPatch.engine());
	const bool bruno106 = synth->sources[0].oscType == OscType::DRUM_SYNTH
	                      && dsp::drum_synth::isBruno106(synth->sources[0].drumPatch.engine());
	const bool wavetable =
	    synth->getSynthMode() == SynthMode::SUBTRACTIVE
	    && (synth->sources[0].oscType == OscType::WAVETABLE || synth->sources[1].oscType == OscType::WAVETABLE);
	for (size_t i = 0; i < kSynthInstruments.size(); i++) {
		const SynthInstrument& type = kSynthInstruments[i];
		if (type.init) {
			continue;
		}
		if (rings      ? type.rings
		    : brophet  ? type.brophet
		    : bruno106 ? type.bruno106
		    : dx       ? type.dx
		         : (!type.dx && !type.rings && !type.brophet && !type.bruno106 && type.mode == synth->getSynthMode()
		            && type.wavetable == wavetable)) {
			return i;
		}
	}
	return 0;
}

/// The picker's pads: the audition pads play what the clip plays now (the type's init synth once the picker has moved),
/// as in the preset browser. Other pads do nothing, so a stray press doesn't throw the picker away.
ActionResult pickerPadAction(int32_t x, int32_t y, int32_t velocity) {
	if (x == kDisplayWidth + 1 && getRootUI() == &instrumentClipView) {
		if (sdRoutineLock) {
			return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
		}
		return instrumentClipView.padAction(x, y, velocity);
	}
	return ActionResult::DEALT_WITH;
}

/// Grey out everything but the audition pads
bool pickerGreyout(uint32_t* cols) {
	*cols = (getRootUI() == &instrumentClipView) ? 0xFFFFFFFE : 0xFFFFFFFF;
	return true;
}

} // namespace

/// The DX7 type's init: a plain two-operator FM voice (algorithm 1, operator 2 on operator 1 at the same ratio, its
/// envelope settling so the attack is brighter, velocity on both), so the pads play something DX7-like from the start.
/// The DX7's own blank voice is a bare sine.
void setDx7InitVoice(Source& source) {
	DxPatch* patch = source.ensureDxPatch();
	if (patch == nullptr) {
		return;
	}
	uint8_t* p = patch->params;
	// Each operator is 21 bytes, operator 6 first: rates 1-4, levels 1-4, scaling (break point, depths, curves), rate
	// scaling, amp mod sensitivity, velocity sensitivity, output level, mode, coarse, fine, detune
	auto setOp = [p](int32_t op, std::array<uint8_t, 4> rates, std::array<uint8_t, 4> levels, uint8_t velocity,
	                 uint8_t output) {
		uint8_t* o = p + (6 - op) * 21;
		for (int32_t i = 0; i < 4; i++) {
			o[i] = rates[i];
			o[4 + i] = levels[i];
		}
		o[13] = 2; // a little rate scaling: higher notes a touch quicker
		o[15] = velocity;
		o[16] = output;
		o[17] = 0; // ratio
		o[18] = 1; // coarse 1
		o[19] = 0;
		o[20] = 7; // detune centre
	};
	setOp(1, {99, 45, 30, 55}, {99, 90, 82, 0}, 2, 99);
	setOp(2, {99, 50, 28, 55}, {99, 72, 58, 0}, 3, 80);
	for (int32_t op = 3; op <= 6; op++) {
		p[(6 - op) * 21 + 16] = 0;
	}
	p[134] = 0; // algorithm 1
	p[135] = 0; // no feedback
	p[155] = 0x3F;
	patch->updateEngineMode();
	source.dxPatchChanged = true;
}

bool previewSynthInit(size_t type) {
	using PickerInit = LoadInstrumentPresetUI::PickerInit;
	const PickerInit result = loadInstrumentPresetUI.pickerPreviewInit(type);
	if (result != PickerInit::BUILT_IN) {
		return result != PickerInit::FAILED;
	}
	// A blank synth: set up as the type plays
	const SynthInstrument& synthType = kSynthInstruments[type];
	InstrumentClip* clip = getCurrentInstrumentClip();
	if (clip == nullptr || clip->output->type != OutputType::SYNTH) {
		return true;
	}
	auto* synth = static_cast<SoundInstrument*>(clip->output);
	if (synthType.dx && synth->sources[0].oscType == OscType::DX7) {
		setDx7InitVoice(synth->sources[0]);
	}
	if (synthType.mode == SynthMode::TB303) {
		synth->setSynthMode(SynthMode::TB303, currentSong);
		if (synth->isTb303()) {
			synth->setupTb303Defaults(&clip->paramManager);
			clip->ensureMonoScaleMode(currentSong);
		}
	}
	else if (synthType.mode != synth->getSynthMode()) {
		synth->setSynthMode(synthType.mode, currentSong);
	}
	if (synthType.rings || synthType.brophet || synthType.bruno106) {
		using dsp::drum_synth::Engine;
		const Engine engine = synthType.rings ? Engine::RINGS : synthType.brophet ? Engine::BROPHET : Engine::BRUNO106;
		// The engine is set up as SYNTH MODE sets it, which goes through the sound editor's sound
		if (soundEditor.setup(clip, &synthModeMenu, 0)) {
			gui::menu_item::drum_synth::setDrumEngine(engine, 0, true);
		}
	}
	// Still nobody's preset: it goes when something else takes its place
	synth->editedByUser = false;
	view.setKnobIndicatorLevels();
	return true;
}

namespace {
/// The type the track's own synth plays (-1: the track had no synth). The picker plays that synth on its type, and each
/// other type's init synth.
int32_t pickerHomeType = -1;

/// What the audition pads play on type: the track's own synth on its type, else the type's init synth
void showSynthType(size_t type) {
	if (static_cast<int32_t>(type) == pickerHomeType && loadInstrumentPresetUI.pickerRestoreOriginal()) {
		return;
	}
	previewSynthInit(type);
}
} // namespace

void returnToSynthPicker(size_t type) {
	showSynthType(type);
	if (synthCategoryMenu.setupFor(type)) {
		openUI(&synthCategoryMenu);
	}
	else {
		synthInstrumentMenu.setupFor(type);
		openUI(&synthInstrumentMenu);
	}
}

// ---- Types ----

char const* SynthInstrumentMenu::getTitle() {
	return l10n::get(l10n::String::STRING_FOR_SYNTH_INSTRUMENT);
}

std::span<char const*> SynthInstrumentMenu::getOptions() {
	static std::array<char const*, kSynthInstruments.size()> options;
	for (size_t i = 0; i < kSynthInstruments.size(); i++) {
		options[i] = l10n::get(kSynthInstruments[i].name);
	}
	return {options.data(), options.size()};
}

bool SynthInstrumentMenu::isCurrentOptionAvailable() {
	return typeAvailable(currentOption);
}

bool SynthInstrumentMenu::setupAndCheckAvailability() {
	InstrumentClip* clip = getCurrentInstrumentClip();
	if (clip == nullptr) {
		return false;
	}
	pickerHomeType = -1;
	// No synth on the track yet: start on the default synth type (Defaults > Synth instrument)
	const size_t defaultType = (FlashStorage::defaultSynthInstrument < synth_instruments::kNumDefaultSynthInstruments)
	                               ? FlashStorage::defaultSynthInstrument
	                               : 0;
	switch (getCurrentOutputType()) {
	case OutputType::SYNTH:
		setupFor(typeOf(static_cast<SoundInstrument*>(getCurrentOutput())));
		pickerHomeType = currentOption;
		break;
	case OutputType::KIT: {
		// As stock: a kit clip only turns into a synth clip while it has no notes or sounds
		if (!clip->isEmpty() || !clip->output->isEmpty()) {
			return false;
		}
		setupFor(defaultType);
		break;
	}
	case OutputType::MIDI_OUT:
	case OutputType::CV:
		setupFor(defaultType);
		break;
	default:
		return false;
	}
	// The picker's session: from here on, BACK puts this track's instrument back
	loadInstrumentPresetUI.pickerBegin(clip);
	// A track with its own synth plays it until the picker moves; any other starts on an init synth straight away,
	// so the audition pads have a synth to play
	if (pickerHomeType < 0) {
		previewSynthInit(currentOption);
	}
	return true;
}

void SynthInstrumentMenu::setupFor(size_t type) {
	currentOption = (type < kSynthInstruments.size() && typeAvailable(type)) ? static_cast<int32_t>(type) : 0;
	scrollPos = currentOption;
}

ActionResult SynthInstrumentMenu::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	// SHIFT + SYNTH again: the next type, round to the first after the last
	if (b == deluge::hid::button::SYNTH && Buttons::isShiftButtonPressed()) {
		if (on) {
			const int32_t numOptions = static_cast<int32_t>(kSynthInstruments.size());
			int32_t next = currentOption;
			do {
				next = (next + 1) % numOptions;
			} while (next != currentOption && !typeAvailable(next));
			if (inCardRoutine) {
				return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
			}
			const bool moved = next != currentOption;
			currentOption = next;
			scrollPos = currentOption;
			if (moved) {
				showSynthType(currentOption);
			}
			// After the swap: a loading message mustn't leave the type's name covered
			if (display->haveOLED()) {
				renderUIsForOled();
			}
			else {
				drawCurrentOption();
			}
		}
		return ActionResult::DEALT_WITH;
	}
	// BACK: the track's old instrument back, and out
	if (b == deluge::hid::button::BACK) {
		if (on) {
			if (inCardRoutine) {
				return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
			}
			loadInstrumentPresetUI.pickerCancel();
			display->setNextTransitionDirection(-1);
			close();
		}
		return ActionResult::DEALT_WITH;
	}
	return ContextMenu::buttonAction(b, on, inCardRoutine);
}

void SynthInstrumentMenu::selectEncoderAction(int8_t offset) {
	const int32_t before = currentOption;
	ContextMenu::selectEncoderAction(offset);
	if (currentOption != before) {
		showSynthType(currentOption);
		if (display->haveOLED()) {
			renderUIsForOled();
		}
		else {
			drawCurrentOption();
		}
	}
}

ActionResult SynthInstrumentMenu::padAction(int32_t x, int32_t y, int32_t velocity) {
	return pickerPadAction(x, y, velocity);
}

bool SynthInstrumentMenu::getGreyoutColsAndRows(uint32_t* cols, uint32_t* rows) {
	return pickerGreyout(cols);
}

bool SynthInstrumentMenu::acceptCurrentOption() {
	const size_t typeIndex = currentOption;
	const SynthInstrument& type = kSynthInstruments[typeIndex];
	display->setNextTransitionDirection(-1);
	close();

	// INIT: the stock blank synth (or the card's SYNTHS/INIT/INIT.XML), as the audition pads have been playing
	if (type.init) {
		previewSynthInit(typeIndex);
		loadInstrumentPresetUI.pickerKeepInit(getInstrumentFolder(OutputType::SYNTH));
		return true;
	}

	// Make the folders if they're missing, ready to save presets into, as the Deluge can't make folders when saving
	f_mkdir("SYNTHS");
	f_mkdir(type.folder);
	{
		char folder[48];
		for (size_t c = 0; c < numCategories(typeIndex); c++) {
			categoryFolder(typeIndex, c, folder, sizeof(folder));
			f_mkdir(folder);
		}
	}

	// From here on the audition pads play the type's init synth (the categories first look at where the track's synth
	// came from)
	const bool hasCategories = synthCategoryMenu.setupFor(typeIndex);
	showSynthType(typeIndex);
	if (hasCategories) {
		openUI(&synthCategoryMenu);
	}
	// No categories in use (DX7, or presets kept loose in the type's folder): its folder
	else if (folderHasPresets(type.folder)) {
		if (!loadInstrumentPresetUI.pickerOpenFolder(type.folder, typeIndex)) {
			loadInstrumentPresetUI.pickerCancel();
		}
	}
	// Nothing saved there yet: keep the init synth, which saves into the folder
	else {
		loadInstrumentPresetUI.pickerKeepInit(type.folder);
		if (type.dx && getCurrentOutputType() == OutputType::SYNTH
		    && soundEditor.setup(getCurrentInstrumentClip(), &dxMenu, 0)) {
			openUI(&soundEditor);
		}
	}
	return true;
}

// ---- Categories ----

char const* SynthCategoryMenu::getTitle() {
	return l10n::get(l10n::String::STRING_FOR_SYNTH_CATEGORY);
}

std::span<char const*> SynthCategoryMenu::getOptions() {
	static std::array<char const*, synth_instruments::kMaxSynthCategories> options;
	const auto categories = synth_instruments::categoriesFor(kSynthInstruments[type_]);
	for (size_t i = 0; i < categories.size(); i++) {
		options[i] = (categories[i].label != nullptr) ? categories[i].label : l10n::get(categories[i].name);
	}
	return {options.data(), categories.size()};
}

bool SynthCategoryMenu::setupFor(size_t type) {
	type_ = type;
	if (kSynthInstruments[type].init) {
		return false;
	}
	char folder[48];
	bool any = false;
	hasPresets_.fill(false);
	for (size_t c = 0; c < numCategories(type); c++) {
		categoryFolder(type, c, folder, sizeof(folder));
		hasPresets_[c] = folderHasPresets(folder);
		any = any || hasPresets_[c];
	}
	if (!any) {
		return false;
	}

	// On the category the current synth came from, else the first with presets
	int32_t current = -1;
	if (getCurrentOutputType() == OutputType::SYNTH) {
		char const* dirPath = static_cast<Instrument*>(getCurrentOutput())->dirPath.get();
		for (size_t c = 0; c < numCategories(type) && current < 0; c++) {
			categoryFolder(type, c, folder, sizeof(folder));
			if (hasPresets_[c] && inFolder(dirPath, folder)) {
				current = static_cast<int32_t>(c);
			}
		}
	}
	// Back from a category's presets: on that category
	if (current < 0 && lastType_ == static_cast<int32_t>(type) && lastCategory_ >= 0
	    && lastCategory_ < static_cast<int32_t>(numCategories(type)) && hasPresets_[lastCategory_]) {
		current = lastCategory_;
	}
	for (size_t c = 0; c < numCategories(type) && current < 0; c++) {
		if (hasPresets_[c]) {
			current = static_cast<int32_t>(c);
		}
	}
	currentOption = current;
	scrollPos = currentOption;
	return true;
}

ActionResult SynthCategoryMenu::buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) {
	// BACK: up to the types, on this one
	if (b == deluge::hid::button::BACK) {
		if (on) {
			if (inCardRoutine) {
				return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
			}
			display->setNextTransitionDirection(-1);
			close();
			synthInstrumentMenu.setupFor(type_);
			openUI(&synthInstrumentMenu);
		}
		return ActionResult::DEALT_WITH;
	}
	return ContextMenu::buttonAction(b, on, inCardRoutine);
}

ActionResult SynthCategoryMenu::padAction(int32_t x, int32_t y, int32_t velocity) {
	return pickerPadAction(x, y, velocity);
}

bool SynthCategoryMenu::getGreyoutColsAndRows(uint32_t* cols, uint32_t* rows) {
	return pickerGreyout(cols);
}

bool SynthCategoryMenu::acceptCurrentOption() {
	char folder[48];
	categoryFolder(type_, currentOption, folder, sizeof(folder));
	lastType_ = static_cast<int32_t>(type_);
	lastCategory_ = currentOption;
	display->setNextTransitionDirection(1);
	close();
	if (!loadInstrumentPresetUI.pickerOpenFolder(folder, type_)) {
		loadInstrumentPresetUI.pickerCancel();
	}
	return true;
}

} // namespace deluge::gui::context_menu
