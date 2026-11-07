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

class Sound;

/// Kit gate out: the bottom four rows of one kit (kick, snare, hat 1, hat 2 on the preset kits) fire gate outs 1-4 as
/// they sound. A gate follows the note when the sound has a note tail, else it's a short trigger, and is always held
/// long enough for a module to see it.
namespace drum_gates {

constexpr uint32_t kTriggerSamples = 529; // 12 ms

/// A row of the gate kit sounded. owner = the row's drum, so only it can end the gate.
void noteOn(int32_t gate, void const* owner, bool oneShot);
/// That drum's note ended. force = also end a one-shot trigger now (voices killed)
void noteOff(void const* owner, bool force = false);
/// Switch every gate this feature holds off, now
void allOff();
/// Once per audio routine: ends triggers and gates held up to their minimum length
void tick();
/// A sound with no note tail sent MIDI note on: send its note off once it has been held a trigger's length, rather
/// than straight after the note on, which some gear ignores
void midiOneShot(Sound const* sound, int32_t note, int32_t channel);
/// The gates the song's gate kit has taken, one bit each. CV tracks and GATE rows can't use them.
uint32_t reservedMask();
bool isReserved(int32_t gate);
/// Whether a CV track on this channel slot (0, 1, or 2 = both) would need a gate the gate kit has
bool blocksCVChannel(int32_t slot);
/// The gate kit has just taken these gates: drop any still held up by something else
void takeGates(uint32_t mask);

} // namespace drum_gates
