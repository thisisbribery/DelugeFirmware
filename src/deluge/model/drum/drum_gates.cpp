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

#include "model/drum/drum_gates.h"
#include "definitions_cxx.hpp"
#include "processing/engines/audio_engine.h"
#include "processing/engines/cv_engine.h"

namespace drum_gates {
namespace {

struct Gate {
	void const* owner = nullptr; // nullptr = off
	uint32_t onTime = 0;
	bool oneShot = false;
	bool offPending = false;
};

Gate gates[NUM_GATE_CHANNELS];

void switchOff(int32_t g) {
	gates[g] = Gate{};
	cvEngine.sendNote(false, g, ALL_NOTES_OFF);
}

bool heldLongEnough(int32_t g) {
	return AudioEngine::audioSampleTimer - gates[g].onTime >= kTriggerSamples;
}

} // namespace

void noteOn(int32_t gate, void const* owner, bool oneShot) {
	if (gate < 0 || gate >= NUM_GATE_CHANNELS) {
		return;
	}
	// A new hit while the gate is still up: drop it first, so the module sees a fresh edge (the CV engine holds the
	// new gate back for its minimum off time)
	if (gates[gate].owner != nullptr) {
		cvEngine.sendNote(false, gate, ALL_NOTES_OFF);
	}
	gates[gate] = Gate{owner, AudioEngine::audioSampleTimer, oneShot, oneShot};
	cvEngine.sendNote(true, gate, ALL_NOTES_OFF);
}

void noteOff(void const* owner, bool force) {
	for (int32_t g = 0; g < NUM_GATE_CHANNELS; g++) {
		Gate& gate = gates[g];
		if (gate.owner != owner || owner == nullptr) {
			continue;
		}
		if (force) {
			switchOff(g);
		}
		else if (!gate.oneShot) {
			if (heldLongEnough(g)) {
				switchOff(g);
			}
			else {
				gate.offPending = true;
			}
		}
	}
}

void allOff() {
	for (int32_t g = 0; g < NUM_GATE_CHANNELS; g++) {
		if (gates[g].owner != nullptr) {
			switchOff(g);
		}
	}
}

void tick() {
	for (int32_t g = 0; g < NUM_GATE_CHANNELS; g++) {
		if (gates[g].offPending && heldLongEnough(g)) {
			switchOff(g);
		}
	}
}

} // namespace drum_gates
