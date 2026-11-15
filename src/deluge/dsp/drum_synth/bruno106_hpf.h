/*
 * BRUNO106 HPF: the Juno-106's four position high pass slider, from Ultramaster KR-106.
 *
 * Portions Copyright (C) 2026 Karl LaRocca (Ultramaster KR-106, kayrockscreenprinting, GPL-3.0).
 * Deluge integration Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * This is a modified version of KR-106's HPF (KR106_DSP.h) and bass boost (KR106_HPF.h): Juno-106 positions only,
 * coefficients worked out for 44.1 kHz, the 0.35 Hz coupling cap left out. It runs once per track on the summed voices,
 * as on the hardware, and not at all at FLAT. Not affiliated with or endorsed by Roland.
 */

#pragma once

#include "dsp/stereo_sample.h"
#include <cstdint>
#include <span>

namespace deluge::dsp::drum_synth::bruno106 {

/// The slider's positions, in the order they're saved (FLAT first, so a sound saved without the byte has none). The
/// menu shows them in the slider's order, bottom up: BOOST, FLAT, 236 Hz, 754 Hz.
enum class Hpf : uint8_t { FLAT, BOOST, CUT_236, CUT_754, NUM };

/// The HPF's state for one track, both channels
class HpfState {
public:
	/// Filter the track's buffer at a position (not FLAT: the caller skips it then). Moving between positions
	/// crossfades over 64 samples, as KR-106, so the switch doesn't click.
	void process(std::span<StereoSample> buffer, Hpf position);
	/// The track went to FLAT, or isn't a BRUNO106: start clean next time
	void idle() { active_ = false; }

private:
	struct Channel {
		double z1, z2; ///< The bass boost (its poles sit near 60 Hz: double keeps them quiet)
		float hp;      ///< The cut's one pole
	};
	struct Filter {
		Hpf position;
		Channel l, r;
	};
	static float run(float x, Hpf position, Channel& c);

	Filter cur_{};
	Filter prev_{};
	int32_t fade_ = 0; ///< Samples of crossfade from prev_ left
	bool active_ = false;
};

} // namespace deluge::dsp::drum_synth::bruno106
