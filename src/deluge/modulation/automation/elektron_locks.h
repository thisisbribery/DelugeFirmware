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

#pragma once

#include <cstdint>

class Action;
class ModelStackWithNoteRow;

/// Elektron P-locks (Community Features > ELEK): a lock made by holding a step and turning a knob belongs to that
/// step. It's still automation, so songs stay readable by any firmware, but kept to a shape that tells locks and the
/// unlocked value apart:
///  - a lock runs from its note to one tick before the next note (on the row for kits, in the clip otherwise), so the
///    unlocked value is always what plays in the tick before a note, even with every note locked, or only one note
///  - a new note inside a lock plays the unlocked value, and the lock before it ends a tick before the new note
///  - deleting a locked note hands its time to the note before it, as that note now sounds until the next one
///  - letting go of a held step with the transport stopped puts the unlocked value back on the knobs and the sound
namespace deluge::plock {

bool active();

/// Length of the lock region for a held note, given the stock one (to the next note)
int32_t lockLength(int32_t distanceToNextNote);

/// A note has just been put at pos on this row
void noteAdded(ModelStackWithNoteRow* modelStack, int32_t pos, Action* action);

/// The note that was at pos on this row has just been deleted
void noteDeleted(ModelStackWithNoteRow* modelStack, int32_t pos, Action* action);

/// Every held step was let go: with the transport stopped, back to the unlocked value from before the step at pos
void stepReleased(int32_t pos);

} // namespace deluge::plock
