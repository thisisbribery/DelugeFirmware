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

#include <cstdint>

/// Running totals of time spent in the audio render, for the CPU load menu. Times are getSystemTicks() counts
/// (33.33 MHz) and are left to wrap: readers only ever look at the difference between two snapshots.
/// Everything here is written by the audio routine and read by the UI, which run on the same thread.
namespace deluge::cpu_meter {

struct Counter {
	uint32_t ticks = 0;
	uint32_t samples = 0;

	void add(uint32_t startTicks, uint32_t endTicks, uint32_t numSamples) {
		ticks += endTicks - startTicks;
		samples += numSamples;
	}
};

/// Whole audio render. samples = output samples.
inline Counter audio;
/// Every synth voice, including its filters and envelopes. samples = voice-samples.
inline Counter voices;
/// The drum synth oscillator alone. samples = voice-samples.
inline Counter drumSynth;

constexpr uint32_t kTicksPerSecond = 33330000;
constexpr uint32_t kSampleRate = 44100;

} // namespace deluge::cpu_meter
