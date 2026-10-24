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

#include "model/swing/swing_style.h"
#include "util/misc.h"
#include <algorithm>
#include <array>

namespace deluge::swing_style {

namespace {

// Generated from the Stolperbeats visualiser's Deluge formulas, at a 16th swing interval. Each row is one level: the
// time each step of the block plays at, in 1/kStepOne of a swing interval, then the end of the block.
// clang-format off
constexpr uint32_t kHopp[kMaxLevel][3] = {
    {0, 70997, 131072},
    {0, 73728, 131072},
    {0, 76459, 131072},
    {0, 81920, 131072},
    {0, 87381, 131072},
    {0, 92843, 131072},
};

constexpr uint32_t kDeep[kMaxLevel][5] = {
    {0, 70884, 136315, 201746, 262144},
    {0, 76415, 144179, 206701, 262144},
    {0, 82104, 152044, 211498, 262144},
    {0, 89548, 159908, 217160, 262144},
    {0, 97308, 167772, 222508, 262144},
    {0, 104910, 174850, 227226, 262144},
};

constexpr uint32_t kSqez[kMaxLevel][5] = {
    {0, 87381, 145636, 203890, 262144},
    {0, 104858, 157286, 209715, 262144},
    {0, 94663, 174763, 203890, 262144},
    {0, 147456, 172032, 229376, 262144},
    {0, 84261, 215333, 243419, 262144},
    {0, 152917, 225735, 247580, 262144},
};

constexpr uint32_t kFold[kMaxLevel][5] = {
    {0, 72090, 131072, 190054, 262144},
    {0, 78643, 131072, 183501, 262144},
    {0, 85197, 131072, 176947, 262144},
    {0, 91750, 131072, 170394, 262144},
    {0, 98304, 131072, 163840, 262144},
    {0, 104858, 131072, 157286, 262144},
};

// clang-format on

struct StyleInfo {
	std::string_view name;
	int32_t numSteps;
	const uint32_t* table; // kMaxLevel rows of numSteps + 1 entries
};

// In Style order
constexpr std::array<StyleInfo, kNumStyles> kStyles = {{
    {"CLASSIC", 2, nullptr},
    {"HOPP", 2, &kHopp[0][0]},
    {"DEEP", 4, &kDeep[0][0]},
    {"SQEZ", 4, &kSqez[0][0]},
    {"FOLD", 4, &kFold[0][0]},
}};

const uint32_t* getRow(Style style, int32_t level) {
	const StyleInfo& info = kStyles[util::to_underlying(style)];
	level = std::clamp<int32_t>(level, 1, kMaxLevel);
	return info.table + (level - 1) * (info.numSteps + 1);
}

} // namespace

std::string_view getName(Style style) {
	return kStyles[util::to_underlying(style)].name;
}

Style fromName(std::string_view name) {
	for (int32_t i = 0; i < kNumStyles; i++) {
		if (kStyles[i].name == name) {
			return static_cast<Style>(i);
		}
	}
	return Style::CLASSIC;
}

int32_t getNumSteps(Style style) {
	return kStyles[util::to_underlying(style)].numSteps;
}

uint64_t swungToUnswung(Style style, int32_t level, uint32_t swingIntervalTicks, uint32_t swungTicksIntoBlock) {
	int32_t numSteps = getNumSteps(style);
	uint32_t step = swungTicksIntoBlock / swingIntervalTicks;
	if (style == Style::CLASSIC || step >= numSteps) {
		return (uint64_t)swungTicksIntoBlock << 16;
	}
	const uint32_t* row = getRow(style, level);
	uint32_t ticksIntoStep = swungTicksIntoBlock - step * swingIntervalTicks;

	// row[] is in fractions of a swing interval, so multiplying by the interval gives 1/65536ths of a tick
	uint64_t stepStart = (uint64_t)row[step] * swingIntervalTicks;
	uint64_t stepLength = row[step + 1] - row[step];
	return stepStart + stepLength * ticksIntoStep;
}

uint32_t unswungToSwung(Style style, int32_t level, uint32_t swingIntervalTicks, uint64_t unswungIntoBlock) {
	int32_t numSteps = getNumSteps(style);
	if (style != Style::CLASSIC) {
		const uint32_t* row = getRow(style, level);
		for (int32_t step = 0; step < numSteps; step++) {
			uint64_t stepEnd = (uint64_t)row[step + 1] * swingIntervalTicks;
			if (unswungIntoBlock < stepEnd) {
				uint64_t intoStep = unswungIntoBlock - (uint64_t)row[step] * swingIntervalTicks;
				return step * swingIntervalTicks + (uint32_t)(intoStep / (row[step + 1] - row[step]));
			}
		}
	}
	return (uint32_t)(unswungIntoBlock >> 16);
}

} // namespace deluge::swing_style
