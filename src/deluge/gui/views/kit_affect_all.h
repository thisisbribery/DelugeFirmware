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

#include "modulation/params/param.h"
#include "util/containers.h"
#include <cstdint>

class Clip;
class InstrumentClip;
class Kit;
class ModelStackWithAutoParam;
class Song;
class SoundDrum;

// Affect all, a performance mode for the preset kits (LXR, Cycles, GIN). SHIFT + AFFECT ENTIRE in a kit clip takes a
// temp snapshot of the kit (AFFECT ENTIRE flashes) and goes to all voices: the gold knobs, and the sound menu (SHIFT +
// pad, turn SELECT), move every drum synth row by the same amount, each from where it was and each on its own engine's
// control for that knob or pad. Sample and synth rows are left alone. AFFECT ENTIRE on its own swaps between all voices
// and the kit's FX, so both can be pushed around. SHIFT + AFFECT ENTIRE again (AFFECT ENTIRE flashes twice as fast)
// puts the whole snapshot back on the next bar, or straight away when stopped. The snapshot lives in RAM only.
namespace deluge::kit_affect_all {

class KitAffectAll {
public:
	/// SHIFT + AFFECT ENTIRE in a kit clip: take the snapshot, or ask to put it back
	void shiftAffectEntirePressed(InstrumentClip* clip);
	/// AFFECT ENTIRE on its own in a kit clip. True if the snapshot is on, which then swaps all voices / kit FX
	bool affectEntirePressed(InstrumentClip* clip);
	[[nodiscard]] bool isActiveFor(Clip const* clip) const;

	/// A gold knob turned. True if affect all made the change, so the normal knob action must not happen
	bool modEncoderTurned(ModelStackWithAutoParam* modelStack, int32_t whichModEncoder, int32_t offset);
	/// The sound menu just set a param from oldValue: move the other rows the same way, and remember where it was
	void menuValueChanged(ModelStackWithAutoParam const* modelStack, int32_t oldValue);

	/// From playback, before the clips play a swung tick. Returns the ticks until it next needs one
	int32_t processTick(int64_t swungTick);
	void playbackStopped();

	/// Flash AFFECT ENTIRE while the current clip is in the mode. Called after the view sets the mod LEDs
	void refreshLed();

private:
	/// One param on one row (or the kit's FX, drum nullptr): where it was when the snapshot first needed it, so it can
	/// go back exactly, and how far affect all has moved it since, in param value units. Kept apart from the value, so
	/// a row pushed against an end keeps its place against the others when you turn back.
	struct Entry {
		SoundDrum* drum;
		modulation::params::Kind kind;
		int32_t paramId;
		int32_t value;
		int64_t moved;
	};

	[[nodiscard]] bool stillValid() const;
	[[nodiscard]] SoundDrum* drumRowFor(ModelStackWithAutoParam const* modelStack) const;
	[[nodiscard]] bool isKitFx(ModelStackWithAutoParam const* modelStack) const;
	Entry& entryFor(SoundDrum* drum, ModelStackWithAutoParam const* modelStack, int32_t valueBefore);
	void moveBy(Entry& entry, ModelStackWithAutoParam* modelStack, int64_t amount);
	void restore();
	void leave();
	void forget();

	InstrumentClip* clip_ = nullptr;
	Kit* kit_ = nullptr;
	Song* song_ = nullptr;
	bool affectEntireBefore_ = false;
	bool leaving_ = false;
	deluge::vector<Entry> snapshot_;
};

extern KitAffectAll kitAffectAll;

} // namespace deluge::kit_affect_all
