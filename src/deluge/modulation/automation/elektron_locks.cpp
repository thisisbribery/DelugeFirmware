/*
 * Copyright © 2026 thisisbribery
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

#include "modulation/automation/elektron_locks.h"
#include "definitions_cxx.hpp"
#include "gui/views/view.h"
#include "model/clip/instrument_clip.h"
#include "model/drum/drum.h"
#include "model/instrument/instrument.h"
#include "model/model_stack.h"
#include "model/note/note.h"
#include "model/note/note_row.h"
#include "model/settings/runtime_feature_settings.h"
#include "modulation/automation/auto_param.h"
#include "modulation/params/param_collection.h"
#include "modulation/params/param_manager.h"
#include "playback/playback_handler.h"
#include <algorithm>
#include <limits>

namespace deluge::plock {

bool active() {
	return runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::ElektronLocks);
}

int32_t lockLength(int32_t distanceToNextNote) {
	return active() ? std::max<int32_t>(distanceToNextNote - 1, 1) : distanceToNextNote;
}

namespace {

/// Notes on this row, or every row: the same notes a held step's lock runs up to
/// (InstrumentClip::getDistanceToNextNote)
bool rowOnly(InstrumentClip const* clip) {
	return clip->output->type == OutputType::KIT && !clip->affectEntire;
}

/// Ticks back from pos to the start of the note before it, wrapping round the loop. 0 if the row has no other note.
int32_t distanceToPrevious(NoteRow* noteRow, int32_t pos, int32_t loopLength) {
	const int32_t numNotes = noteRow->notes.getNumElements();
	if (!numNotes) {
		return 0;
	}
	int32_t i = noteRow->notes.search(pos, LESS);
	if (i < 0) {
		i = numNotes - 1;
	}
	int32_t distance = pos - noteRow->notes.getElement(i)->pos;
	if (distance <= 0) {
		distance += loopLength;
	}
	return (distance >= loopLength) ? 0 : distance;
}

struct Neighbours {
	int32_t toNext;     ///< Ticks to the next note's start, the loop length if there's no other note
	int32_t toPrevious; ///< Ticks back to the previous note's start, 0 if there's no other note
};

Neighbours neighboursOf(ModelStackWithNoteRow* modelStack, int32_t pos) {
	auto* clip = static_cast<InstrumentClip*>(modelStack->getTimelineCounter());
	const int32_t loopLength = modelStack->getLoopLength();
	Neighbours result{loopLength, 0};
	auto consider = [&](NoteRow* noteRow) {
		if (noteRow->hasNoNotes()) {
			return;
		}
		result.toNext = std::min(result.toNext, noteRow->getDistanceToNextNote(pos, modelStack));
		const int32_t back = distanceToPrevious(noteRow, pos, loopLength);
		if (back && (!result.toPrevious || back < result.toPrevious)) {
			result.toPrevious = back;
		}
	};
	if (rowOnly(clip)) {
		consider(modelStack->getNoteRow());
	}
	else {
		for (int32_t i = 0; i < clip->noteRows.getNumElements(); i++) {
			consider(clip->noteRows.getElement(i));
		}
	}
	return result;
}

/// Calls visit for every automated param a lock on this row's steps would be in
void forEachLockableParam(ModelStackWithNoteRow* modelStack, ParamCollection::AutoParamVisitor visit, void* context) {
	auto* clip = static_cast<InstrumentClip*>(modelStack->getTimelineCounter());
	NoteRow* noteRow = modelStack->getNoteRow();
	ModControllable* modControllable;
	ParamManagerForTimeline* paramManager;
	if (rowOnly(clip)) {
		if (noteRow->drum == nullptr) {
			return;
		}
		modControllable = noteRow->drum->toModControllable();
		paramManager = &noteRow->paramManager;
	}
	else {
		modControllable = clip->output->toModControllable();
		paramManager = &clip->paramManager;
	}
	if (modControllable == nullptr || !paramManager->containsAnyParamCollectionsIncludingExpression()) {
		return;
	}
	ModelStackWithThreeMainThings* modelStackWithThreeMainThings =
	    modelStack->addOtherTwoThings(modControllable, paramManager);
	// Patch cables are left alone: changing one can delete it, mid-walk
	for (ParamCollectionSummary* summary = paramManager->summaries; summary->paramCollection; summary++) {
		summary->paramCollection->forEachAutomatedParam(
		    modelStackWithThreeMainThings->addParamCollection(summary->paramCollection, summary), visit, context);
	}
}

int32_t wrap(int32_t pos, int32_t loopLength) {
	pos %= loopLength;
	return (pos < 0) ? pos + loopLength : pos;
}

struct Edit {
	int32_t pos;
	Neighbours neighbours;
};

bool hasNodeAt(AutoParam* param, int32_t pos) {
	const int32_t i = param->nodes.search(pos, GREATER_OR_EQUAL);
	return i < param->nodes.getNumElements() && param->nodes.getElement(i)->pos == pos;
}

} // namespace

void noteAdded(ModelStackWithNoteRow* modelStack, int32_t pos, Action* action) {
	if (!active()) {
		return;
	}
	Edit edit{pos, neighboursOf(modelStack, pos)};
	forEachLockableParam(
	    modelStack,
	    [](ModelStackWithAutoParam* paramStack, void* context) {
		    const Edit& edit = *static_cast<Edit*>(context);
		    AutoParam* param = paramStack->autoParam;
		    const int32_t loopLength = paramStack->getLoopLength();
		    if (loopLength <= 1 || edit.pos >= loopLength) {
			    return;
		    }
		    // The unlocked value is what plays the tick before the next note
		    const int32_t next = edit.pos + edit.neighbours.toNext;
		    const int32_t base = param->getValueAtPos(wrap(next - 1, loopLength), paramStack);
		    // Inside a lock: the note before has one, and it's still going here. Drawn automation is left alone.
		    if (!edit.neighbours.toPrevious
		        || !hasNodeAt(param, wrap(edit.pos - edit.neighbours.toPrevious, loopLength))
		        || param->getValueAtPos(edit.pos, paramStack) == base) {
			    return;
		    }
		    // From a tick before the new note (ending the lock before it) up to the next note
		    const int32_t start = (edit.neighbours.toPrevious > 1) ? edit.pos - 1 : edit.pos;
		    const int32_t length = std::min(next - start, loopLength - 1);
		    param->setValueForRegion(wrap(start, loopLength), length, base, paramStack);
	    },
	    &edit);
}

void noteDeleted(ModelStackWithNoteRow* modelStack, int32_t pos, Action* action) {
	if (!active()) {
		return;
	}
	Edit edit{pos, neighboursOf(modelStack, pos)};
	forEachLockableParam(
	    modelStack,
	    [](ModelStackWithAutoParam* paramStack, void* context) {
		    const Edit& edit = *static_cast<Edit*>(context);
		    AutoParam* param = paramStack->autoParam;
		    const int32_t loopLength = paramStack->getLoopLength();
		    if (loopLength <= 1 || edit.pos >= loopLength || !hasNodeAt(param, edit.pos)) {
			    return; // Nothing starts at the deleted note: not a lock of its
		    }
		    // No notes left: the whole loop goes back to the unlocked value
		    if (!edit.neighbours.toPrevious) {
			    const int32_t base = param->getValueAtPos(wrap(edit.pos - 1, loopLength), paramStack);
			    param->setValueForRegion(0, loopLength, base, paramStack);
			    return;
		    }
		    // The note before now sounds up to the next one, so its value (locked or not) carries on until then
		    const int32_t previous = wrap(edit.pos - edit.neighbours.toPrevious, loopLength);
		    const int32_t value = param->getValueAtPos(previous, paramStack);
		    const int32_t start = (edit.neighbours.toPrevious > 1) ? edit.pos - 1 : edit.pos;
		    const int32_t end = edit.pos + edit.neighbours.toNext - 1; // The next note's unlocked tick stays
		    const int32_t length = std::min(end - start, loopLength - 1);
		    if (length > 0) {
			    param->setValueForRegion(wrap(start, loopLength), length, value, paramStack);
		    }
	    },
	    &edit);
}

void stepReleased(int32_t pos) {
	if (!active() || playbackHandler.isEitherClockActive()) {
		return;
	}
	ModelStackWithThreeMainThings& modelStack = view.activeModControllableModelStack;
	if (!modelStack.timelineCounterIsSet() || modelStack.modControllable == nullptr
	    || modelStack.paramManager == nullptr || !modelStack.paramManager->containsAnyMainParamCollections()) {
		return;
	}
	const int32_t loopLength = modelStack.getLoopLength();
	if (loopLength <= 1) {
		return;
	}
	modelStack.paramManager->toForTimeline()->grabValuesFromPos(wrap(static_cast<int32_t>(pos) - 1, loopLength),
	                                                            &modelStack);
	view.setKnobIndicatorLevels();
}

} // namespace deluge::plock
