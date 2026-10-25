/*
 * The GIN kits: sixteen eight-sound kits for the GIN engine (gin.h), made for the Deluge. These are our own sounds, not
 * GIN's factory kits.
 *
 * Copyright © 2026 thisisbribery and the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "dsp/drum_synth/gin.h"
#include <array>
#include <cstdint>

namespace deluge::dsp::drum_synth::gin {

/// One sound of a kit, in GIN's own units (0 to 1 unless noted), plus where it sits in the kit
struct KitSound {
	Wave wave = Wave::SINE;
	ModMode modMode = ModMode::DECAY;
	NoiseFilter noiseFilter = NoiseFilter::LOW_PASS;
	NoiseEnv noiseEnv = NoiseEnv::EXP;
	float hz = 261.6256f; ///< The tone's pitch: set by the row's transpose, as the kit rows play at middle C
	float toneAttack = 0.f;
	float toneDecay = 0.4f;
	float modRate = 0.5f;
	float modAmount = 0.f; ///< -1 to 1
	float noiseFreq = 0.7f;
	float noiseQ = 0.1f;
	float noiseAttack = 0.f;
	float noiseDecay = 0.3f;
	float mix = 0.f;
	float drive = 0.f;
	float eqFreq = 0.5f;
	float eqGain = 0.5f;
	float level = 0.8f; ///< GIN's Level, before its clipper: 0.8 is about -2 dB (see levelGain())
};

inline constexpr int32_t kNumKitSounds = 8;
inline constexpr int32_t kNumKits = 16;

/// Rows, bottom first: kick, snare, hat 1 (closed), hat 2 (open), perc 1, perc 2, clap, other. The hats choke each
/// other.
enum KitRow : uint8_t { KICK, SNARE, HAT1, HAT2, PERC1, PERC2, CLAP, OTHER };

struct Kit {
	char const* name;
	std::array<KitSound, kNumKitSounds> sounds;
};

extern const std::array<Kit, kNumKits> kKits;

/// The sound a GIN row starts on when picked in SYNTH MODE: the first kit's kick
inline const KitSound& defaultSound() {
	return kKits[0].sounds[KICK];
}

} // namespace deluge::dsp::drum_synth::gin
