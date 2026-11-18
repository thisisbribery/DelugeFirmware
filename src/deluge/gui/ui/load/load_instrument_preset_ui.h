/*
 * Copyright © 2018-2023 Synthstrom Audible Limited
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
#include "gui/ui/load/load_ui.h"
#include "hid/button.h"
#include "model/instrument/kit.h"
#include "model/note/note_row.h"
#include "processing/sound/sound_drum.h"

class Instrument;
class InstrumentClip;
class Output;

class LoadInstrumentPresetUI final : public LoadUI {
public:
	LoadInstrumentPresetUI() = default;
	bool opened() override;
	// void selectEncoderAction(int8_t offset);
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	ActionResult verticalEncoderAction(int32_t offset, bool inCardRoutine) override;
	void instrumentEdited(Instrument* instrument);
	Error performLoad(bool doClone = false);
	Error performLoadSynthToKit();
	ActionResult timerCallback() override;
	bool getGreyoutColsAndRows(uint32_t* cols, uint32_t* rows) override;
	bool renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth] = nullptr,
	                    uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth] = nullptr, bool drawUndefinedArea = true,
	                    int32_t navSys = -1) {
		return true;
	}
	bool renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
	                   uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]) override;
	std::expected<FileItem*, Error>
	findAnUnlaunchedPresetIncludingWithinSubfolders(Song* song, OutputType outputType,
	                                                Availability availabilityRequirement);
	std::expected<FileItem*, Error> confirmPresetOrNextUnlaunchedOne(OutputType outputType, String* searchName,
	                                                                 Availability availabilityRequirement);
	/// With fromFolderStart, from the start of that folder instead of the old instrument's place in its own
	PresetNavigationResult doPresetNavigation(int32_t offset, Instrument* oldInstrument,
	                                          Availability availabilityRequirement, bool doBlink,
	                                          char const* fromFolderStart = nullptr);
	void setupLoadInstrument(OutputType newOutputType, Instrument* instrumentToReplace_,
	                         InstrumentClip* instrumentClipToLoadFor_) {
		Browser::outputTypeToLoad = newOutputType;
		instrumentToReplace = instrumentToReplace_;
		instrumentClipToLoadFor = instrumentClipToLoadFor_;
		loadingSynthToKitRow = false;
		soundDrumToReplace = nullptr;
		noteRowIndex = 255; // (not set value for note rows)
		noteRow = nullptr;
	}
	// ---- The SHIFT + SYNTH picker ----
	// One session runs from the type menu, through the category menu, to this browser on a category's folder. As the
	// menus move, the clip plays each type's init synth, so the audition pads always have something to play; the
	// browser keeps playing it until another preset is highlighted. BACK from the type menu (or leaving the browser any
	// way but BACK in the folder it opened on) puts the clip's old instrument back.

	/// What putting a type's init synth on the clip did
	enum class PickerInit : uint8_t { FAILED, ALREADY_ON, FROM_FILE, BUILT_IN };
	/// Start a session on clip's instrument, before the type menu opens
	void pickerBegin(InstrumentClip* clip);
	/// Put type's init synth on the clip: SYNTHS/INIT/<type>.XML if the card has one, else a blank synth (BUILT_IN:
	/// the caller sets its mode or engine, see context_menu::previewSynthInit)
	PickerInit pickerPreviewInit(size_t type);
	/// Put the old instrument back and end the session
	void pickerCancel();
	/// Put the old instrument back on the clip and carry the session on (its own type in the picker). False if it
	/// couldn't be put back.
	bool pickerRestoreOriginal();
	/// Keep the init synth on the clip (INIT, or a type with no presets yet): named as a new synth, saving into folder
	void pickerKeepInit(char const* folder);
	/// Open this browser on folder for type, the clip playing what it plays now until a preset is highlighted. BACK in
	/// that folder returns to type's categories (or the types, if it has none), playing its init.
	bool pickerOpenFolder(char const* folder, size_t type);
	bool pickerActive() const { return pickerSession_; }
	/// The init synth the session put on the clip, if it's still there
	Instrument* pickerInit() const { return (pickerInit_ == instrumentToReplace) ? pickerInit_ : nullptr; }

	void setupLoadSynthToKit(Instrument* kit, InstrumentClip* clip, SoundDrum* drum, NoteRow* row, int32_t rowIndex) {
		Browser::outputTypeToLoad = OutputType::SYNTH;
		instrumentToReplace = kit;
		instrumentClipToLoadFor = clip;
		loadingSynthToKitRow = true;
		soundDrumToReplace = drum;
		noteRowIndex = rowIndex; // (not set value for note rows)
		noteRow = row;
	}

	// ui
	UIType getUIType() override { return UIType::LOAD_INSTRUMENT_PRESET; }
	bool canDisplayFavourites() override { return true; }

protected:
	void enterKeyPress() override;
	void folderContentsReady(int32_t entryDirection) override;
	void currentFileChanged(int32_t movementDirection) override;

private:
	bool showingAuditionPads();
	Error setupForOutputType();
	void changeOutputType(OutputType newOutputType);
	void revertToInitialPreset();
	void exitAction() override;
	ActionResult backButtonAction() override;
	bool isInstrumentInList(Instrument* searchInstrument, Output* list);
	bool findUnusedSlotVariation(String* oldName, String* newName);

	InstrumentClip* instrumentClipToLoadFor; // Can be NULL - if called from Arranger.
	Instrument* instrumentToReplace; // The Instrument that's actually successfully loaded and assigned to the Clip.

	// these are all necessary to setup a sound drum
	bool loadingSynthToKitRow;
	SoundDrum* soundDrumToReplace;
	int32_t noteRowIndex;
	NoteRow* noteRow;
	Error currentInstrumentLoadError;

	int16_t initialChannel;
	int8_t initialChannelSuffix;
	OutputType initialOutputType;

	bool changedInstrumentForClip;
	bool replacedWholeInstrument;

	String initialName;
	String initialDirPath;

	// The SHIFT + SYNTH picker's folder: set before opening, then held in pickerStartDir while open
	char const* pickerFolder = nullptr;
	int32_t pickerBackToType = -1;
	String pickerStartDir;
	bool pickerSession_ = false;
	bool pickerOpening_ = false;       // opened() from pickerOpenFolder: the session's old instrument stays the one
	bool pickerHoldOnArrival_ = false; // arriving in the picker's folder: don't load the highlighted preset
	bool pickerNotLoaded_ = false;     // the highlighted preset isn't loaded: the clip plays the init (or the old one)
	Instrument* pickerInit_ = nullptr; // the last init synth the session put on the clip
	int32_t pickerInitType_ = -1;
	void captureInitialInstrument();
	void endPickerSession();
	// The preset last browsed from the picker and its folder, to pick up there next time
	String pickerLastDir;
	String pickerLastName;
	void rememberPickerPlace();
};

extern LoadInstrumentPresetUI loadInstrumentPresetUI;
