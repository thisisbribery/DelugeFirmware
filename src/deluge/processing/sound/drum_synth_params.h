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

#include "dsp/drum_synth/drum_synth.h"
#include "modulation/params/param.h"
#include "modulation/params/param_set.h"
#include <array>
#include <utility>

namespace deluge::dsp::drum_synth {

/// The Deluge patched params the drum synth controls live on: the FM operators' params, which a drum synth sound (never
/// in FM mode) has no other use for, and osc 1's pulse width, which the drum synth doesn't have. Being patched params,
/// they save with the sound and can be automated, parameter locked and put on gold knobs.
inline constexpr std::array<int32_t, DrumControls::NUM_SLOTS> kDrumControlParams{
    modulation::params::LOCAL_MODULATOR_0_VOLUME,       modulation::params::LOCAL_MODULATOR_1_VOLUME,
    modulation::params::LOCAL_MODULATOR_0_FEEDBACK,     modulation::params::LOCAL_MODULATOR_1_FEEDBACK,
    modulation::params::LOCAL_CARRIER_0_FEEDBACK,       modulation::params::LOCAL_CARRIER_1_FEEDBACK,
    modulation::params::LOCAL_MODULATOR_0_PITCH_ADJUST, modulation::params::LOCAL_MODULATOR_1_PITCH_ADJUST,
    modulation::params::LOCAL_OSC_A_PHASE_WIDTH,
};

/// Envelope 2 on every drum synth sound: a short blip (attack 0, decay 5, sustain 0, release 0), ready to patch to
/// pitch for a transient snap
inline constexpr std::array<std::pair<int32_t, int32_t>, 4> kDrumSynthEnvelope2{{
    {modulation::params::LOCAL_ENV_1_ATTACK, 0},
    {modulation::params::LOCAL_ENV_1_DECAY, 5},
    {modulation::params::LOCAL_ENV_1_SUSTAIN, 0},
    {modulation::params::LOCAL_ENV_1_RELEASE, 0},
}};

inline DrumControls readDrumControls(PatchedParamSet& params) {
	DrumControls controls;
	for (int32_t i = 0; i < DrumControls::NUM_SLOTS; i++) {
		controls.values[i] = params.getValue(kDrumControlParams[i]);
	}
	return controls;
}

/// The slot a patched param carries, or -1
inline int32_t drumSlotForParam(int32_t p) {
	for (int32_t i = 0; i < DrumControls::NUM_SLOTS; i++) {
		if (kDrumControlParams[i] == p) {
			return i;
		}
	}
	return -1;
}

} // namespace deluge::dsp::drum_synth
