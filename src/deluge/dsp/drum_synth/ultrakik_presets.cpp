/*
 * ULTRAKIK's presets.
 *
 * Copyright © 2026 thisisbribery.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "dsp/drum_synth/ultrakik_presets.h"

namespace deluge::dsp::drum_synth::ultrakik {

// clang-format off
const std::array<Preset, kNumPresets> kPresets{{
    //                Sweep Time  Fold  Grit Drive Click  Tone  Bend Punch  Decay  Hold  Sub  Wave         Drive            Click         Cr  BT  Snap Note
    {"DEEP 808",     {35,   55,   8,    0,   38,   20,    50,   64,  40,    92,    55,   75}, Wave::SINE,  DriveType::TUBE, Click::TICK,  0,  90,  24,  -36},
    {"GRIME SLIDE",  {30,   60,   10,   8,   45,   15,    45,   32,  35,    100,   40,   70}, Wave::SINE,  DriveType::TUBE, Click::TICK,  0,  95,  20,  -24},
    {"BOOM ROOM",    {80,   75,   15,   15,  50,   30,    40,   64,  50,    70,    60,   55}, Wave::SINE,  DriveType::WARM, Click::THUD,  0,  64,  64,  -36},
    {"909 THUMP",    {60,   30,   15,   15,  55,   55,    85,   64,  75,    50,    30,   40}, Wave::ROUND, DriveType::WARM, Click::TICK,  0,  64,  110, -24},
    {"ULTRA",        {70,   40,   20,   25,  60,   45,    60,   64,  60,    52,    50,   45}, Wave::SINE,  DriveType::WARM, Click::THUD,  0,  64,  64,  -24},
    {"CLUB 909",     {60,   30,   20,   25,  75,   70,    90,   64,  85,    50,    25,   35}, Wave::ROUND, DriveType::HARD, Click::TICK,  0,  64,  127, -24},
    {"TUBE FAT",     {65,   45,   35,   40,  80,   50,    55,   64,  60,    58,    55,   50}, Wave::TRI,   DriveType::TUBE, Click::THUD,  0,  64,  64,  -24},
    {"TRAP KNOCK",   {45,   45,   25,   35,  85,   40,    70,   64,  55,    85,    45,   60}, Wave::SINE,  DriveType::TUBE, Click::TICK,  0,  70,  64,  -36},
    {"HARDSTYLE",    {55,   70,   45,   30,  100,  50,    60,   64,  60,    75,    55,   45}, Wave::SAW,   DriveType::WARM, Click::THUD,  0,  64,  64,  -24},
    {"DISTORTO",     {70,   30,   45,   60,  105,  75,    100,  64,  90,    55,    35,   35}, Wave::ROUND, DriveType::HARD, Click::NOISE, 0,  64,  127, -24},
    {"RUMBLE",       {70,   50,   40,   80,  85,   40,    40,   64,  60,    75,    60,   50}, Wave::SINE,  DriveType::WARM, Click::THUD,  0,  64,  64,  -24},
    {"GABBER",       {75,   35,   65,   70,  127,  70,    95,   64,  95,    60,    70,   30}, Wave::ROUND, DriveType::HARD, Click::TICK,  0,  64,  127, -24},
    {"BITCRUSHER",   {85,   40,   60,   95,  110,  70,    100,  64,  100,   60,    50,   40}, Wave::SAW,   DriveType::HARD, Click::ZAP,   80, 64,  64,  -24},
    {"FUZZ BOMB",    {80,   40,   70,   110, 120,  60,    70,   64,  100,   60,    50,   35}, Wave::TRI,   DriveType::FUZZ, Click::NOISE, 20, 64,  64,  -24},
    {"FOLD STORM",   {85,   45,   115,  90,  110,  60,    80,   64,  90,    58,    45,   40}, Wave::SINE,  DriveType::WARM, Click::ZAP,   0,  64,  64,  -24},
    {"ANNIHILATE",   {110,  50,   127,  127, 127,  110,   110,  32,  110,   65,    60,   40}, Wave::SAW,   DriveType::FUZZ, Click::ZAP,   60, 50,  127, -24},
}};
// clang-format on

Settings settingsFor(const Preset& preset) {
	auto knob = [&](Knob k) { return static_cast<float>(preset.knobs[k]) / 127.f; };
	Settings s;
	s.sweep = knob(SWEEP);
	s.sweepTime = knob(SWEEP_TIME);
	s.fold = knob(FOLD);
	s.grit = knob(GRIT);
	s.drive = knob(DRIVE);
	s.click = knob(CLICK);
	s.clickTone = knob(CLICK_TONE);
	const int32_t bend = preset.knobs[BEND];
	s.bend = static_cast<float>(bend - 64) / ((bend < 64) ? 64.f : 63.f);
	s.punch = knob(PUNCH);
	s.decay = knob(DECAY);
	s.hold = knob(HOLD);
	s.sub = knob(SUB);
	s.crush = static_cast<float>(preset.crush) / 127.f;
	s.bendTime = static_cast<float>(preset.bendTime) / 127.f;
	s.snap = static_cast<float>(preset.snap) / 127.f;
	s.wave = preset.wave;
	s.driveType = preset.driveType;
	s.clickType = preset.clickType;
	return s;
}

} // namespace deluge::dsp::drum_synth::ultrakik
