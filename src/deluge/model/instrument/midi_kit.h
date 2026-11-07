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

#include <array>
#include <cstdint>

/// MKIT: a kit whose sixteen rows are MIDI rows, so kit patterns, Autofill and everything else a kit does play other
/// gear. It's an ordinary kit once made: each row's channel and note can be changed from the row's menu or the gold
/// knobs, and it saves and loads as a kit.
namespace deluge::midi_kit {

constexpr int32_t kNumRows = 16;

enum class Layout : uint8_t {
	ONE_CHANNEL,      ///< Channel 10, General MIDI drum notes, the bottom six in the kit pattern order
	SIXTEEN_CHANNELS, ///< Row n on channel n, all on note 60 (C3)
	BLANK,            ///< Every row on channel 1, note 60, ready to set up
};
constexpr int32_t kNumLayouts = 3;

struct Row {
	uint8_t channel; ///< 0-15
	uint8_t note;
};

/// Bottom row first. Rows 1-6 are kick, snare, hihat 1, hihat 2, perc 1 and perc 2, as kit patterns, Autofill and
/// the Stolperbeats MIDI export use them.
constexpr std::array<uint8_t, kNumRows> kGMDrumNotes = {36, 38, 42, 46, 39, 63, 37, 45, 47, 50, 49, 51, 44, 56, 54, 75};
constexpr uint8_t kGMDrumChannel = 9;
constexpr uint8_t kDefaultNote = 60;

constexpr Row rowFor(Layout layout, int32_t row) {
	switch (layout) {
	case Layout::ONE_CHANNEL:
		return {kGMDrumChannel, kGMDrumNotes[row]};
	case Layout::SIXTEEN_CHANNELS:
		return {static_cast<uint8_t>(row), kDefaultNote};
	default:
		return {0, kDefaultNote};
	}
}

} // namespace deluge::midi_kit
