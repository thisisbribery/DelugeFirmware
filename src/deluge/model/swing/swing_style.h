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

#include <cstdint>
#include <string_view>

// Swing styles, after the Stolperbeats shuffle visualiser. Classic is the Deluge's own swing. Every other style is a
// time warp over a block of swing intervals: step k of the block plays at a fixed point in time, and positions between
// steps are warped linearly. The first step of each block never moves, so blocks start on the beat.
namespace deluge::swing_style {

enum class Style : uint8_t {
	CLASSIC,
	HOPP, // MPC / Linn 16th swing in whole ticks
	DEEP, // the 8ths swing, then the 16ths inside them
	SQEZ, // the & and a squeezed up against the next downbeat
	FOLD, // the e and a fold in toward the &
	NUM_STYLES,
};

constexpr int32_t kNumStyles = static_cast<int32_t>(Style::NUM_STYLES);

/// Styles other than Classic have levels 1 to kMaxLevel (0 is straight). They use the song's swingAmount as the level.
constexpr int32_t kMaxLevel = 6;

/// Step times are in 1/kStepOne of a swing interval.
constexpr uint32_t kStepOne = 65536;

std::string_view getName(Style style);
/// Returns Style::CLASSIC for unknown names.
Style fromName(std::string_view name);

/// Number of swing intervals in one block of a style (not used for Classic).
int32_t getNumSteps(Style style);

/// Unswung position, in 1/65536ths of an internal tick, of a swung position within a block. Level 1 to kMaxLevel.
uint64_t swungToUnswung(Style style, int32_t level, uint32_t swingIntervalTicks, uint32_t swungTicksIntoBlock);

/// The inverse: the swung tick (rounded down) that an unswung position within a block falls in. The position is in
/// 1/65536ths of an internal tick.
uint32_t unswungToSwung(Style style, int32_t level, uint32_t swingIntervalTicks, uint64_t unswungIntoBlock);

} // namespace deluge::swing_style
