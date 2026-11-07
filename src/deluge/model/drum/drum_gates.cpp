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
#include "io/midi/midi_engine.h"
#include "model/instrument/kit.h"
#include "model/song/song.h"
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
	cvEngine.sendDrumGate(false, g);
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
		cvEngine.sendDrumGate(false, gate);
	}
	gates[gate] = Gate{owner, AudioEngine::audioSampleTimer, oneShot, oneShot};
	cvEngine.sendDrumGate(true, gate);
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

uint32_t reservedMask() {
	if (currentSong == nullptr) {
		return 0;
	}
	for (Output* output = currentSong->firstOutput; output != nullptr; output = output->next) {
		if (output->type == OutputType::KIT && static_cast<Kit*>(output)->gateOut) {
			return static_cast<Kit*>(output)->gateOutMask();
		}
	}
	return 0;
}

bool isReserved(int32_t gate) {
	return (reservedMask() >> gate) & 1;
}

bool blocksCVChannel(int32_t slot) {
	// CV tracks on channel 1 or 2 use that gate, on both (slot 2) gates 1 and 2
	const uint32_t needs = (slot >= NUM_PHYSICAL_CV_CHANNELS) ? 0b11 : 1u << slot;
	return (reservedMask() & needs) != 0;
}

void takeGates(uint32_t mask) {
	for (int32_t g = 0; g < NUM_GATE_CHANNELS; g++) {
		if (((mask >> g) & 1) && gates[g].owner == nullptr) {
			cvEngine.sendDrumGate(false, g); // Whatever else held it up
		}
	}
}

namespace {
struct MIDIOneShot {
	Sound const* sound;
	uint32_t onTime;
	uint8_t note;
	uint8_t channel;
};
constexpr int32_t kMaxMIDIOneShots = 16;
MIDIOneShot midiOneShots[kMaxMIDIOneShots];
int32_t numMIDIOneShots = 0;

void midiOneShotOff(MIDIOneShot const& oneShot) {
	midiEngine.sendNote(oneShot.sound, false, oneShot.note, kDefaultNoteOffVelocity, oneShot.channel, 0);
}
} // namespace

void midiOneShot(Sound const* sound, int32_t note, int32_t channel) {
	// Retriggering the same note ends the earlier one first
	for (int32_t i = 0; i < numMIDIOneShots; i++) {
		if (midiOneShots[i].note == note && midiOneShots[i].channel == channel) {
			midiOneShots[i] = midiOneShots[--numMIDIOneShots];
			break;
		}
	}
	if (numMIDIOneShots == kMaxMIDIOneShots) {
		midiOneShotOff(midiOneShots[0]);
		midiOneShots[0] = midiOneShots[--numMIDIOneShots];
	}
	midiOneShots[numMIDIOneShots++] = {sound, AudioEngine::audioSampleTimer, static_cast<uint8_t>(note),
	                                   static_cast<uint8_t>(channel)};
}

void tick() {
	for (int32_t g = 0; g < NUM_GATE_CHANNELS; g++) {
		if (gates[g].offPending && heldLongEnough(g)) {
			switchOff(g);
		}
	}
	for (int32_t i = 0; i < numMIDIOneShots; i++) {
		if (AudioEngine::audioSampleTimer - midiOneShots[i].onTime >= kTriggerSamples) {
			midiOneShotOff(midiOneShots[i]);
			midiOneShots[i--] = midiOneShots[--numMIDIOneShots];
		}
	}
}

} // namespace drum_gates
