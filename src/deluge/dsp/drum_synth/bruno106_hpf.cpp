/*
 * BRUNO106 HPF: the Juno-106's four position high pass slider, from Ultramaster KR-106.
 *
 * Portions Copyright (C) 2026 Karl LaRocca (Ultramaster KR-106, kayrockscreenprinting, GPL-3.0).
 * Deluge integration Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dsp/drum_synth/bruno106_hpf.h"
#include "util/fixedpoint.h"

namespace deluge::dsp::drum_synth::bruno106 {

namespace {
// The bass boost: KR-106's circuit model (two stages and the summer, +10.5 dB at DC, +1.4 dB up top, a shelf between
// about 60 and 170 Hz), through the bilinear transform at 44.1 kHz
constexpr double kB0 = 1.1853541539;
constexpr double kB1 = -2.3302446589;
constexpr double kB2 = 1.1451800430;
constexpr double kA1 = -1.9813171098;
constexpr double kA2 = 0.98140353912;
// The cuts: one pole TPT, g / (1 + g) for g = tan(pi fc / 44100)
constexpr float kG236 = 0.01653571f;
constexpr float kG754 = 0.05102192f;
constexpr int32_t kFadeSamples = 64;
constexpr float kHpfIn = 1.f / 2147483648.f;
} // namespace

float HpfState::run(float x, Hpf position, Channel& c) {
	switch (position) {
	case Hpf::BOOST: {
		const double y = kB0 * x + c.z1;
		c.z1 = kB1 * x - kA1 * y + c.z2;
		c.z2 = kB2 * x - kA2 * y;
		return static_cast<float>(y);
	}
	case Hpf::CUT_236:
	case Hpf::CUT_754: {
		const float g = (position == Hpf::CUT_236) ? kG236 : kG754;
		const float v = (x - c.hp) * g;
		const float lp = c.hp + v;
		c.hp = lp + v;
		return x - lp;
	}
	default:
		return x;
	}
}

void HpfState::process(std::span<StereoSample> buffer, Hpf position) {
	if (!active_) {
		cur_ = Filter{position, {}, {}};
		fade_ = 0;
		active_ = true;
	}
	else if (position != cur_.position) {
		prev_ = cur_;
		cur_ = Filter{position, {}, {}};
		fade_ = kFadeSamples;
	}

	Filter cur = cur_;
	for (StereoSample& s : buffer) {
		const float l = static_cast<float>(s.l) * kHpfIn;
		const float r = static_cast<float>(s.r) * kHpfIn;
		float outL = run(l, cur.position, cur.l);
		float outR = run(r, cur.position, cur.r);
		if (fade_ > 0) {
			const float t = static_cast<float>(fade_) * (1.f / kFadeSamples);
			outL += (run(l, prev_.position, prev_.l) - outL) * t;
			outR += (run(r, prev_.position, prev_.r) - outR) * t;
			fade_--;
		}
		// The boost is +10.5 dB at its lowest: q31_from_float saturates rather than wraps
		s.l = q31_from_float(outL);
		s.r = q31_from_float(outR);
	}
	cur_ = cur;
}

} // namespace deluge::dsp::drum_synth::bruno106
