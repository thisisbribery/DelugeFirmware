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
#include "gui/l10n/strings.h"
#include "modulation/params/param.h"
#include "modulation/params/param_set.h"
#include <algorithm>
#include <array>
#include <utility>

namespace deluge::dsp::drum_synth {

/// The Deluge patched params the drum synth controls live on: the FM operators' params, which a drum synth sound (never
/// in FM mode) has no other use for, osc 1's pulse width, which the drum synth doesn't have, the two wavetable
/// positions, which only a wavetable oscillator reads, and osc 1's sample start offset. Being patched params, they save
/// with the sound and can be automated, parameter locked and put on gold knobs.
inline constexpr std::array<int32_t, DrumControls::NUM_SLOTS> kDrumControlParams{
    modulation::params::LOCAL_MODULATOR_0_VOLUME,       modulation::params::LOCAL_MODULATOR_1_VOLUME,
    modulation::params::LOCAL_MODULATOR_0_FEEDBACK,     modulation::params::LOCAL_MODULATOR_1_FEEDBACK,
    modulation::params::LOCAL_CARRIER_0_FEEDBACK,       modulation::params::LOCAL_CARRIER_1_FEEDBACK,
    modulation::params::LOCAL_MODULATOR_0_PITCH_ADJUST, modulation::params::LOCAL_MODULATOR_1_PITCH_ADJUST,
    modulation::params::LOCAL_OSC_A_PHASE_WIDTH,        modulation::params::LOCAL_OSC_A_WAVE_INDEX,
    modulation::params::LOCAL_OSC_B_WAVE_INDEX,         modulation::params::LOCAL_OSC_A_START_OFFSET,
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

/// The display name of a Cyc engine
inline deluge::l10n::String cycEngineName(Engine engine) {
	using enum deluge::l10n::String;
	switch (engine) {
	case Engine::CYC_SNR:
		return STRING_FOR_DRUM_ENGINE_CYC_SNR;
	case Engine::CYC_METAL:
		return STRING_FOR_DRUM_ENGINE_CYC_METAL;
	case Engine::CYC_PERC:
		return STRING_FOR_DRUM_ENGINE_CYC_PERC;
	case Engine::CYC_TONE:
		return STRING_FOR_DRUM_ENGINE_CYC_TONE;
	case Engine::CYC_CHORD:
		return STRING_FOR_DRUM_ENGINE_CYC_CHORD;
	case Engine::VTG_SNR:
		return STRING_FOR_DRUM_ENGINE_VTG_SNR;
	case Engine::CLAP_ENG:
		return STRING_FOR_DRUM_ENGINE_CLAP_ENG;
	default:
		return STRING_FOR_DRUM_ENGINE_CYC_KIK;
	}
}

/// What a Cyc engine calls the knob on a control slot: the four macros, then the amp decay
inline deluge::l10n::String cycKnobName(Engine engine, int32_t slot) {
	using enum deluge::l10n::String;
	if (slot == DrumControls::COMMON_DECAY) {
		return STRING_FOR_CYC_AMP_DECAY;
	}
	if (slot == DrumControls::CYC_DRIVE) {
		return STRING_FOR_XIA_DRIVE;
	}
	static constexpr std::array<deluge::l10n::String, 4> kCycles{STRING_FOR_CYC_COLOR, STRING_FOR_CYC_SHAPE,
	                                                             STRING_FOR_CYC_SWEEP, STRING_FOR_CYC_CONTOUR};
	static constexpr std::array<deluge::l10n::String, 4> kVtgSnr{
	    STRING_FOR_CYC_INHARM, STRING_FOR_CYC_FREQ_COMPLEX, STRING_FOR_CYC_PITCH_SWEEP, STRING_FOR_CYC_MOD_ENVELOPE};
	static constexpr std::array<deluge::l10n::String, 4> kClap{
	    STRING_FOR_CYC_BODY_CHAR, STRING_FOR_CYC_BALANCE, STRING_FOR_CYC_SPACING_CRUNCH, STRING_FOR_CYC_BODY_ENVELOPE};
	const int32_t i = std::clamp<int32_t>(slot - DrumControls::CYC_MACRO1, 0, 3);
	switch (engine) {
	case Engine::VTG_SNR:
		return kVtgSnr[i];
	case Engine::CLAP_ENG:
		return kClap[i];
	default:
		return kCycles[i];
	}
}

/// What ULTRAKIK calls the setting on a control slot
inline deluge::l10n::String ultrakikKnobName(int32_t slot) {
	using enum deluge::l10n::String;
	static constexpr std::array<deluge::l10n::String, DrumControls::NUM_SLOTS> kNames{
	    STRING_FOR_UK_SWEEP,  STRING_FOR_UK_SWEEP_TIME, STRING_FOR_UK_FOLD,       STRING_FOR_UK_GRIT,
	    STRING_FOR_XIA_DRIVE, STRING_FOR_UK_CLICK,      STRING_FOR_UK_CLICK_TONE, STRING_FOR_UK_BEND,
	    STRING_FOR_UK_PUNCH,  STRING_FOR_UK_DECAY,      STRING_FOR_UK_HOLD,       STRING_FOR_UK_SUB,
	};
	return kNames[std::clamp<int32_t>(slot, 0, DrumControls::NUM_SLOTS - 1)];
}

/// What RINGS calls the setting on a control slot, or EMPTY_STRING for a slot it doesn't use
inline deluge::l10n::String ringsKnobName(int32_t slot) {
	using enum deluge::l10n::String;
	switch (slot) {
	case DrumControls::RINGS_STRUCTURE:
		return STRING_FOR_RINGS_STRUCTURE;
	case DrumControls::RINGS_BRIGHTNESS:
		return STRING_FOR_RINGS_BRIGHTNESS;
	case DrumControls::RINGS_POSITION:
		return STRING_FOR_RINGS_POSITION;
	case DrumControls::RINGS_BOW:
		return STRING_FOR_RINGS_BOW;
	case DrumControls::RINGS_DRIVE:
		return STRING_FOR_XIA_DRIVE;
	case DrumControls::RINGS_DAMPING:
		return STRING_FOR_RINGS_DAMPING;
	default:
		return EMPTY_STRING;
	}
}

/// What BROPHET calls the setting on a control slot, or EMPTY_STRING for a slot it doesn't use
inline deluge::l10n::String brophetKnobName(int32_t slot) {
	using enum deluge::l10n::String;
	switch (slot) {
	case DrumControls::BROPHET_PW_A:
		return STRING_FOR_BROPHET_PW_A;
	case DrumControls::BROPHET_PW_B:
		return STRING_FOR_BROPHET_PW_B;
	case DrumControls::BROPHET_FREQ_B:
		return STRING_FOR_BROPHET_FREQ_B;
	case DrumControls::BROPHET_FINE_B:
		return STRING_FOR_BROPHET_FINE_B;
	case DrumControls::BROPHET_PM_ENV:
		return STRING_FOR_BROPHET_PM_ENV;
	case DrumControls::BROPHET_PM_OSC_B:
		return STRING_FOR_BROPHET_PM_OSC_B;
	case DrumControls::BROPHET_MIX_A:
		return STRING_FOR_BROPHET_MIX_A;
	case DrumControls::BROPHET_MIX_B:
		return STRING_FOR_BROPHET_MIX_B;
	case DrumControls::BROPHET_NOISE:
		return STRING_FOR_BROPHET_NOISE;
	case DrumControls::BROPHET_ANALOG:
		return STRING_FOR_BROPHET_ANALOG;
	default:
		return EMPTY_STRING;
	}
}

/// What GIN calls the setting on a control slot
inline deluge::l10n::String ginKnobName(int32_t slot) {
	using enum deluge::l10n::String;
	switch (slot) {
	case DrumControls::GIN_MIX:
		return STRING_FOR_GIN_MIX;
	case DrumControls::GIN_NOISE_FREQ:
		return STRING_FOR_GIN_NOISE_FREQ;
	case DrumControls::GIN_MOD_RATE:
		return STRING_FOR_GIN_MOD_RATE;
	case DrumControls::GIN_MOD_AMOUNT:
		return STRING_FOR_GIN_MOD_AMOUNT;
	case DrumControls::GIN_DRIVE:
		return STRING_FOR_XIA_DRIVE;
	case DrumControls::GIN_NOISE_DECAY:
		return STRING_FOR_GIN_NOISE_DECAY;
	case DrumControls::GIN_NOISE_RES:
		return STRING_FOR_GIN_NOISE_RES;
	case DrumControls::GIN_NOISE_ATTACK:
		return STRING_FOR_GIN_NOISE_ATTACK;
	case DrumControls::GIN_TONE_ATTACK:
		return STRING_FOR_GIN_TONE_ATTACK;
	case DrumControls::COMMON_DECAY:
		return STRING_FOR_GIN_DECAY;
	case DrumControls::GIN_EQ_FREQ:
		return STRING_FOR_GIN_EQ_FREQ;
	default:
		return STRING_FOR_GIN_TONE_DECAY;
	}
}

} // namespace deluge::dsp::drum_synth
