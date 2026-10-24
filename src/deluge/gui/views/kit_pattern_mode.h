/*
 * Copyright © 2026 Synthstrom Audible Limited
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

#include <array>
#include <cstdint>

class Action;

// Kit pattern mode: a pattern store for the first six rows of a kit, after the Stolperbeats module. With the Sync
// Scaling button set to "Kit pattern mode" in community features, pressing it latches the mode on (its LED flashes).
// Then, in a kit clip:
// - Hold a track's audition pad and turn SELECT to step that track through its 24 saved patterns.
// - With no pad held, turn SELECT to pick a bank, and press SELECT to load it into all six tracks.
// - Hold a track's audition pad and press SAVE to save that track's pattern over its slot.
// Patterns are ordinary pattern files at PATTERNS/RHYTHMIC/STUMBLE/<bank>/<track>/PATTERN001.XML to PATTERN024.XML,
// where a bank is any folder, such as TECHNO or HOUSE.
namespace deluge::gui::kit_pattern {

constexpr int32_t kNumTracks = 6;
constexpr int32_t kNumSlots = 24;
constexpr int32_t kMaxBanks = 64;
constexpr int32_t kMaxBankNameLength = 32;

class KitPatternMode {
public:
	/// True when the Sync Scaling button is set to kit pattern mode in community features
	static bool isChosenInSettings();
	/// True when the mode is latched on and the current view is a kit clip
	bool isActiveHere();
	bool isOn() const { return on_; }
	void toggle();

	/// Turning SELECT with a track's audition pad held: steps that track to its next or previous pattern
	void stepTrackPattern(int32_t noteRowIndex, int32_t offset);
	/// Turning SELECT with no pad held: picks the bank that pressing SELECT will load
	void browseBanks(int32_t offset);
	/// Pressing SELECT with no pad held: loads the picked bank into all six tracks, each on its own pattern number
	void loadBank();
	/// Holding a track's audition pad and pressing SAVE: sets up the pattern save screen for that track's slot.
	/// Returns false if the row isn't one of the six tracks or there's no bank, after saying why.
	bool setUpTrackSave(int32_t noteRowIndex);

private:
	/// Returns false if the track's slot has no file, in which case the track is cleared
	bool loadTrack(int32_t track, Action* action);
	int32_t readBanks();
	void showTrack(int32_t track, bool empty);
	void show(char const* oledText, char const* sevenSegText);

	bool on_ = false;
	char bank_[kMaxBankNameLength] = "";
	int32_t browsedBank_ = -1;               // Index into banks_ while browsing, or -1
	std::array<int32_t, kNumTracks> slot_{}; // 0 to kNumSlots - 1 for each track
	char banks_[kMaxBanks][kMaxBankNameLength];
	int32_t numBanks_ = 0;
};

extern KitPatternMode kitPatternMode;

} // namespace deluge::gui::kit_pattern
