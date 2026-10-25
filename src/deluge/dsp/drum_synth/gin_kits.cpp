/*
 * The GIN kits. See gin_kits.h.
 *
 * Copyright © 2026 thisisbribery and the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Quick reference for GIN's units, to read and tweak these by:
 * - toneDecay / noiseDecay: 60 dB in 0.01 x 1000^x s. 0.2 = 40 ms, 0.3 = 80 ms, 0.4 = 160 ms, 0.5 = 320 ms,
 *   0.6 = 630 ms, 0.7 = 1.3 s
 * - attacks: x * 0.04 * 10^(2.4x) s. 0.05 = 2.6 ms, 0.1 = 7 ms, 0.2 = 24 ms
 * - modRate, sweep: the pitch falls 60 dB (in octaves) in (1.03 - x) / 3x s. 0.5 = 350 ms, 0.7 = 160 ms, 0.9 = 50 ms
 * - modRate, sine LFO: 2000x * 10^(3.5(x - 1)) Hz. 0.5 = 18 Hz, 0.7 = 125 Hz, 0.83 = 420 Hz, 0.9 = 800 Hz. Audio rate
 *   LFOs are FM: bells, cowbells, metal
 * - modAmount: x|x| * 96 semitones for the sweep, 48 for the LFO and noise. 0.3 = 9, 0.4 = 15, 0.5 = 24, 0.7 = 47
 * - noiseFreq / eqFreq: 20 x 1000^x Hz. 0.5 = 630 Hz, 0.6 = 1.3 kHz, 0.7 = 2.5 kHz, 0.8 = 5 kHz, 0.9 = 10 kHz
 * - noiseQ: 0.1 x 100000^x. 0.17 = 0.7, 0.2 = 1, 0.3 = 3, 0.4 = 10
 * - mix: 0 = tone, 0.5 = both full, 1 = noise. Each side dips 25 dB as it goes
 * - drive: 200x^3 into GIN's shaper. Under 0.17 is warmth, 0.3 is crunch, 0.4 and up is fuzz
 * - modulated noise envelope (claps): bursts about 9 ms apart at noiseDecay 0.5, 5 ms at 0.45
 */

#include "dsp/drum_synth/gin_kits.h"

namespace deluge::dsp::drum_synth::gin {

namespace {
constexpr Wave SIN = Wave::SINE;
constexpr Wave TRI = Wave::TRIANGLE;
constexpr Wave SAW = Wave::SAW;
constexpr ModMode DEC = ModMode::DECAY;
constexpr ModMode LFO = ModMode::SINE;
constexpr ModMode NSE = ModMode::NOISE;
constexpr NoiseFilter LP = NoiseFilter::LOW_PASS;
constexpr NoiseFilter HP = NoiseFilter::HIGH_PASS;
constexpr NoiseFilter BP = NoiseFilter::BAND_PASS;
constexpr NoiseFilter OFF = NoiseFilter::OFF;
constexpr NoiseEnv EXP = NoiseEnv::EXP;
constexpr NoiseEnv LIN = NoiseEnv::LINEAR;
constexpr NoiseEnv MOD = NoiseEnv::MOD;
} // namespace

// clang-format off
const std::array<Kit, kNumKits> kKits{{
    {"808", {{
        // Kick: long round sine, a soft sweep and a breath of noise
        {.wave = SIN, .hz = 47.f, .toneDecay = 0.64f, .modRate = 0.62f, .modAmount = 0.38f, .noiseFreq = 0.45f,
         .noiseDecay = 0.1f, .mix = 0.1f, .drive = 0.06f, .level = 0.814f},
        {.wave = TRI, .noiseFilter = HP, .hz = 185.f, .toneDecay = 0.33f, .modRate = 0.8f, .modAmount = 0.28f,
         .noiseFreq = 0.74f, .noiseQ = 0.12f, .noiseDecay = 0.43f, .mix = 0.56f, .drive = 0.08f, .level = 0.739f},
        {.noiseFilter = HP, .noiseFreq = 0.87f, .noiseQ = 0.18f, .noiseDecay = 0.2f, .mix = 1.f, .eqFreq = 0.88f,
         .eqGain = 0.6f, .level = 0.722f},
        {.noiseFilter = HP, .noiseFreq = 0.87f, .noiseQ = 0.18f, .noiseDecay = 0.46f, .mix = 1.f, .eqFreq = 0.88f,
         .eqGain = 0.6f, .level = 0.655f},
        // Low tom, conga
        {.wave = SIN, .hz = 95.f, .toneDecay = 0.5f, .modRate = 0.55f, .modAmount = 0.22f, .noiseFreq = 0.5f,
         .noiseDecay = 0.15f, .mix = 0.15f, .level = 0.778f},
        {.wave = SIN, .hz = 330.f, .toneDecay = 0.36f, .modRate = 0.85f, .modAmount = 0.2f, .mix = 0.05f,
         .level = 0.760f},
        {.noiseFilter = BP, .noiseEnv = MOD, .noiseFreq = 0.62f, .noiseQ = 0.28f, .noiseDecay = 0.47f, .mix = 1.f,
         .level = 0.640f},
        // Cowbell: a triangle frequency modulated at 420 Hz, and a peak in the upper mids
        {.wave = TRI, .modMode = LFO, .hz = 560.f, .toneDecay = 0.38f, .modRate = 0.83f, .modAmount = 0.18f,
         .eqFreq = 0.72f, .eqGain = 0.6f, .level = 0.745f},
    }}},
    {"909", {{
        {.wave = SIN, .noiseFilter = HP, .hz = 52.f, .toneDecay = 0.52f, .modRate = 0.72f, .modAmount = 0.5f,
         .noiseFreq = 0.78f, .noiseDecay = 0.08f, .mix = 0.28f, .drive = 0.16f, .eqFreq = 0.3f, .eqGain = 0.56f,
         .level = 0.782f},
        {.wave = TRI, .hz = 205.f, .toneDecay = 0.28f, .modRate = 0.82f, .modAmount = 0.34f, .noiseFreq = 0.86f,
         .noiseQ = 0.15f, .noiseDecay = 0.42f, .mix = 0.62f, .drive = 0.14f, .level = 0.824f},
        {.noiseFilter = HP, .noiseFreq = 0.9f, .noiseQ = 0.25f, .noiseDecay = 0.17f, .mix = 1.f, .eqFreq = 0.9f,
         .eqGain = 0.58f, .level = 0.689f},
        {.noiseFilter = HP, .noiseFreq = 0.9f, .noiseQ = 0.25f, .noiseDecay = 0.5f, .mix = 1.f, .eqFreq = 0.9f,
         .eqGain = 0.58f, .level = 0.586f},
        // Low and high toms
        {.wave = SIN, .hz = 110.f, .toneDecay = 0.45f, .modRate = 0.6f, .modAmount = 0.26f, .noiseFreq = 0.55f,
         .noiseDecay = 0.2f, .mix = 0.22f, .level = 0.778f},
        {.wave = SIN, .hz = 180.f, .toneDecay = 0.4f, .modRate = 0.62f, .modAmount = 0.24f, .noiseFreq = 0.6f,
         .noiseDecay = 0.18f, .mix = 0.2f, .level = 0.762f},
        {.noiseFilter = BP, .noiseEnv = MOD, .noiseFreq = 0.66f, .noiseQ = 0.25f, .noiseDecay = 0.5f, .mix = 1.f,
         .level = 0.655f},
        // Rim shot
        {.wave = TRI, .noiseFilter = BP, .hz = 1700.f, .toneDecay = 0.1f, .modRate = 0.9f, .modAmount = 0.2f,
         .noiseFreq = 0.78f, .noiseQ = 0.5f, .noiseDecay = 0.08f, .mix = 0.35f, .level = 0.761f},
    }}},
    {"ELEC", {{
        // Electro: zapped kick, saw snare, FM hats, zap tom, blip, laser
        {.wave = SIN, .hz = 55.f, .toneDecay = 0.48f, .modRate = 0.82f, .modAmount = 0.62f, .drive = 0.2f,
         .level = 0.851f},
        {.wave = SAW, .noiseFilter = BP, .hz = 160.f, .toneDecay = 0.25f, .modRate = 0.85f, .modAmount = 0.45f,
         .noiseFreq = 0.7f, .noiseQ = 0.25f, .noiseDecay = 0.35f, .mix = 0.6f, .drive = 0.2f, .level = 0.852f},
        {.wave = SAW, .modMode = LFO, .noiseFilter = HP, .hz = 3200.f, .toneDecay = 0.18f, .modRate = 0.9f,
         .modAmount = 0.6f, .noiseFreq = 0.9f, .noiseDecay = 0.15f, .mix = 0.7f, .level = 0.786f},
        {.wave = SAW, .modMode = LFO, .noiseFilter = HP, .hz = 3200.f, .toneDecay = 0.4f, .modRate = 0.9f,
         .modAmount = 0.6f, .noiseFreq = 0.9f, .noiseDecay = 0.4f, .mix = 0.7f, .level = 0.710f},
        {.wave = SIN, .hz = 140.f, .toneDecay = 0.4f, .modRate = 0.75f, .modAmount = 0.7f, .level = 0.773f},
        {.wave = SIN, .hz = 900.f, .toneDecay = 0.2f, .modRate = 0.9f, .modAmount = -0.5f, .level = 0.746f},
        {.noiseFilter = BP, .noiseEnv = MOD, .noiseFreq = 0.7f, .noiseQ = 0.2f, .noiseDecay = 0.46f, .mix = 1.f,
         .drive = 0.15f, .level = 0.727f},
        {.wave = SAW, .hz = 400.f, .toneDecay = 0.45f, .modRate = 0.5f, .modAmount = 0.75f, .drive = 0.18f,
         .level = 0.732f},
    }}},
    {"MINI", {{
        // Minimal: short and clicky
        {.wave = SIN, .noiseFilter = HP, .hz = 60.f, .toneDecay = 0.35f, .modRate = 0.88f, .modAmount = 0.5f,
         .noiseFreq = 0.85f, .noiseDecay = 0.05f, .mix = 0.3f, .level = 0.813f},
        {.wave = TRI, .noiseFilter = HP, .hz = 240.f, .toneDecay = 0.2f, .modRate = 0.9f, .modAmount = 0.3f,
         .noiseFreq = 0.8f, .noiseQ = 0.3f, .noiseDecay = 0.3f, .mix = 0.55f, .level = 0.700f},
        {.noiseFilter = HP, .noiseFreq = 0.93f, .noiseQ = 0.3f, .noiseDecay = 0.12f, .mix = 1.f, .level = 0.671f},
        {.noiseFilter = BP, .noiseFreq = 0.88f, .noiseQ = 0.28f, .noiseDecay = 0.32f, .mix = 1.f, .level = 0.797f},
        // Woodblock, click
        {.wave = TRI, .noiseFilter = BP, .hz = 800.f, .toneDecay = 0.12f, .modRate = 0.95f, .modAmount = 0.25f,
         .noiseFreq = 0.8f, .noiseQ = 0.4f, .noiseDecay = 0.05f, .mix = 0.3f, .level = 0.788f},
        {.wave = SIN, .hz = 1200.f, .toneDecay = 0.08f, .level = 0.733f},
        {.noiseFilter = BP, .noiseEnv = MOD, .noiseFreq = 0.72f, .noiseQ = 0.35f, .noiseDecay = 0.42f, .mix = 1.f,
         .level = 0.673f},
        // Bleep
        {.wave = SIN, .modMode = LFO, .hz = 1600.f, .toneDecay = 0.22f, .modRate = 0.5f, .modAmount = 0.12f,
         .level = 0.694f},
    }}},
    {"INDU", {{
        // Industrial: everything driven
        {.wave = SIN, .hz = 50.f, .toneDecay = 0.55f, .modRate = 0.7f, .modAmount = 0.48f, .noiseFreq = 0.5f,
         .noiseDecay = 0.2f, .mix = 0.25f, .drive = 0.38f, .level = 0.885f},
        {.wave = SAW, .noiseFilter = BP, .hz = 150.f, .toneDecay = 0.3f, .modRate = 0.8f, .modAmount = 0.4f,
         .noiseFreq = 0.65f, .noiseQ = 0.4f, .noiseDecay = 0.45f, .mix = 0.6f, .drive = 0.4f, .level = 0.861f},
        {.noiseFilter = HP, .noiseFreq = 0.82f, .noiseQ = 0.35f, .noiseDecay = 0.18f, .mix = 1.f, .drive = 0.3f,
         .level = 0.758f},
        {.noiseFilter = BP, .noiseFreq = 0.8f, .noiseQ = 0.32f, .noiseDecay = 0.45f, .mix = 1.f, .drive = 0.35f,
         .level = 0.745f},
        // Metal hit, pipe
        {.wave = TRI, .modMode = LFO, .hz = 230.f, .toneDecay = 0.42f, .modRate = 0.88f, .modAmount = 0.55f,
         .drive = 0.3f, .level = 0.839f},
        {.wave = SIN, .modMode = LFO, .noiseFilter = BP, .hz = 420.f, .toneDecay = 0.5f, .modRate = 0.86f,
         .modAmount = 0.35f, .noiseFreq = 0.7f, .noiseQ = 0.45f, .noiseDecay = 0.2f, .mix = 0.3f, .level = 0.758f},
        {.noiseFilter = BP, .noiseEnv = MOD, .noiseFreq = 0.6f, .noiseQ = 0.4f, .noiseDecay = 0.5f, .mix = 1.f,
         .drive = 0.32f, .level = 0.841f},
        // Noise-bent saw
        {.wave = SAW, .modMode = NSE, .hz = 80.f, .toneDecay = 0.4f, .modRate = 0.75f, .modAmount = 0.6f,
         .drive = 0.35f, .level = 0.810f},
    }}},
    {"DUST", {{
        // Lo-fi: the tops rolled off with the EQ
        {.wave = SIN, .hz = 54.f, .toneDecay = 0.5f, .modRate = 0.6f, .modAmount = 0.35f, .noiseFreq = 0.4f,
         .noiseDecay = 0.25f, .mix = 0.3f, .drive = 0.12f, .eqFreq = 0.75f, .eqGain = 0.3f, .level = 0.811f},
        {.wave = TRI, .hz = 190.f, .toneDecay = 0.32f, .modRate = 0.75f, .modAmount = 0.25f, .noiseFreq = 0.66f,
         .noiseQ = 0.2f, .noiseDecay = 0.4f, .mix = 0.55f, .drive = 0.14f, .eqFreq = 0.85f, .eqGain = 0.32f,
         .level = 0.830f},
        {.noiseFilter = BP, .noiseFreq = 0.8f, .noiseQ = 0.35f, .noiseDecay = 0.2f, .mix = 1.f, .level = 0.875f},
        {.noiseFilter = BP, .noiseEnv = LIN, .noiseFreq = 0.78f, .noiseQ = 0.35f, .noiseDecay = 0.42f, .mix = 1.f,
         .level = 0.838f},
        // Dusty tom, shaker
        {.wave = SIN, .hz = 100.f, .toneDecay = 0.48f, .modRate = 0.5f, .modAmount = 0.2f, .noiseFreq = 0.45f,
         .mix = 0.3f, .level = 0.780f},
        {.noiseFilter = HP, .noiseEnv = LIN, .noiseFreq = 0.8f, .noiseQ = 0.2f, .noiseAttack = 0.12f,
         .noiseDecay = 0.3f, .mix = 1.f, .level = 0.719f},
        {.noiseEnv = MOD, .noiseFreq = 0.62f, .noiseQ = 0.3f, .noiseDecay = 0.5f, .mix = 1.f, .level = 0.562f},
        // Rim tick
        {.wave = TRI, .noiseFilter = HP, .hz = 1300.f, .toneDecay = 0.12f, .noiseFreq = 0.85f, .noiseDecay = 0.1f,
         .mix = 0.5f, .level = 0.717f},
    }}},
    {"RUMB", {{
        // Techno rumble: a kick with a long low noise tail
        {.wave = SIN, .hz = 45.f, .toneDecay = 0.7f, .modRate = 0.75f, .modAmount = 0.45f, .noiseFreq = 0.3f,
         .noiseQ = 0.3f, .noiseDecay = 0.6f, .mix = 0.3f, .drive = 0.24f, .level = 0.869f},
        {.wave = TRI, .noiseFilter = HP, .hz = 175.f, .toneDecay = 0.3f, .modRate = 0.8f, .modAmount = 0.3f,
         .noiseFreq = 0.76f, .noiseQ = 0.15f, .noiseDecay = 0.38f, .mix = 0.6f, .drive = 0.1f, .level = 0.752f},
        {.noiseFilter = HP, .noiseFreq = 0.9f, .noiseQ = 0.2f, .noiseDecay = 0.18f, .mix = 1.f, .level = 0.727f},
        // Ride: FM saw over long noise
        {.wave = SAW, .modMode = LFO, .noiseFilter = HP, .hz = 4000.f, .toneDecay = 0.6f, .modRate = 0.92f,
         .modAmount = 0.5f, .noiseFreq = 0.88f, .noiseDecay = 0.6f, .mix = 0.65f, .level = 0.646f},
        // Deep perc, tick
        {.wave = SIN, .hz = 75.f, .toneDecay = 0.55f, .modRate = 0.4f, .modAmount = 0.15f, .noiseFreq = 0.4f,
         .noiseDecay = 0.4f, .mix = 0.25f, .drive = 0.2f, .level = 0.811f},
        {.wave = TRI, .noiseFilter = HP, .hz = 2200.f, .toneDecay = 0.08f, .noiseFreq = 0.88f, .noiseDecay = 0.06f,
         .mix = 0.4f, .level = 0.748f},
        {.noiseFilter = BP, .noiseEnv = MOD, .noiseFreq = 0.58f, .noiseQ = 0.22f, .noiseDecay = 0.52f, .mix = 1.f,
         .level = 0.639f},
        // Sub boom
        {.wave = SIN, .hz = 38.f, .toneDecay = 0.78f, .modRate = 0.3f, .modAmount = 0.25f, .level = 0.790f},
    }}},
    {"ZAP", {{
        // Zaps: fast deep sweeps everywhere
        {.wave = SIN, .hz = 58.f, .toneDecay = 0.42f, .modRate = 0.9f, .modAmount = 0.75f, .drive = 0.1f,
         .level = 0.808f},
        {.wave = TRI, .noiseFilter = BP, .hz = 220.f, .toneDecay = 0.25f, .modRate = 0.92f, .modAmount = 0.55f,
         .noiseFreq = 0.74f, .noiseQ = 0.3f, .noiseDecay = 0.32f, .mix = 0.55f, .level = 0.841f},
        {.wave = SAW, .modMode = NSE, .noiseFilter = HP, .hz = 6000.f, .toneDecay = 0.15f, .modRate = 0.8f,
         .modAmount = 0.4f, .noiseFreq = 0.9f, .noiseDecay = 0.12f, .mix = 0.6f, .level = 0.736f},
        {.wave = SAW, .modMode = NSE, .noiseFilter = HP, .hz = 6000.f, .toneDecay = 0.38f, .modRate = 0.8f,
         .modAmount = 0.4f, .noiseFreq = 0.9f, .noiseDecay = 0.36f, .mix = 0.6f, .level = 0.672f},
        // Pew down, pew up
        {.wave = SIN, .hz = 600.f, .toneDecay = 0.3f, .modRate = 0.7f, .modAmount = 0.6f, .level = 0.745f},
        {.wave = SIN, .hz = 300.f, .toneDecay = 0.3f, .modRate = 0.75f, .modAmount = -0.5f, .level = 0.758f},
        {.noiseFilter = BP, .noiseEnv = MOD, .noiseFreq = 0.68f, .noiseQ = 0.4f, .noiseDecay = 0.46f, .mix = 1.f,
         .level = 0.793f},
        // Wobble blip
        {.wave = SIN, .modMode = LFO, .hz = 500.f, .toneDecay = 0.45f, .modRate = 0.45f, .modAmount = 0.35f,
         .level = 0.741f},
    }}},
    {"BOOM", {{
        // Trap: a long tuned 808 and crisp tops
        {.wave = SIN, .noiseFilter = HP, .hz = 41.f, .toneDecay = 0.82f, .modRate = 0.65f, .modAmount = 0.32f,
         .noiseFreq = 0.8f, .noiseDecay = 0.06f, .mix = 0.18f, .drive = 0.2f, .level = 0.851f},
        {.wave = TRI, .noiseFilter = HP, .hz = 210.f, .toneDecay = 0.24f, .modRate = 0.85f, .modAmount = 0.3f,
         .noiseFreq = 0.82f, .noiseQ = 0.12f, .noiseDecay = 0.38f, .mix = 0.68f, .drive = 0.08f, .eqFreq = 0.82f,
         .eqGain = 0.58f, .level = 0.829f},
        {.noiseFilter = HP, .noiseFreq = 0.92f, .noiseQ = 0.15f, .noiseDecay = 0.12f, .mix = 1.f, .level = 0.768f},
        {.noiseFilter = HP, .noiseFreq = 0.9f, .noiseQ = 0.15f, .noiseDecay = 0.38f, .mix = 1.f, .level = 0.699f},
        // Snap, low perc
        {.wave = TRI, .noiseFilter = BP, .hz = 1900.f, .toneDecay = 0.12f, .noiseFreq = 0.78f, .noiseQ = 0.4f,
         .noiseDecay = 0.15f, .mix = 0.55f, .level = 0.847f},
        {.wave = SIN, .hz = 160.f, .toneDecay = 0.38f, .modRate = 0.7f, .modAmount = 0.3f, .level = 0.762f},
        {.noiseFilter = BP, .noiseEnv = MOD, .noiseFreq = 0.64f, .noiseQ = 0.25f, .noiseDecay = 0.5f, .mix = 1.f,
         .level = 0.671f},
        // 808 tom
        {.wave = SIN, .hz = 82.f, .toneDecay = 0.62f, .modRate = 0.6f, .modAmount = 0.3f, .level = 0.774f},
    }}},
    {"TITE", {{
        // Tight and woody, nearer a real kit
        {.wave = SIN, .hz = 62.f, .toneDecay = 0.38f, .modRate = 0.82f, .modAmount = 0.38f, .noiseFreq = 0.62f,
         .noiseQ = 0.2f, .noiseDecay = 0.1f, .mix = 0.32f, .eqFreq = 0.55f, .eqGain = 0.56f, .level = 0.855f},
        {.wave = TRI, .hz = 230.f, .toneDecay = 0.26f, .modRate = 0.85f, .modAmount = 0.2f, .noiseFreq = 0.84f,
         .noiseQ = 0.12f, .noiseDecay = 0.38f, .mix = 0.6f, .drive = 0.06f, .level = 0.837f},
        {.noiseFilter = BP, .noiseFreq = 0.86f, .noiseQ = 0.35f, .noiseDecay = 0.15f, .mix = 1.f, .level = 0.900f},
        {.noiseFilter = HP, .noiseFreq = 0.84f, .noiseQ = 0.3f, .noiseDecay = 0.36f, .mix = 1.f, .level = 0.606f},
        // Low and high toms
        {.wave = SIN, .hz = 120.f, .toneDecay = 0.48f, .modRate = 0.7f, .modAmount = 0.15f, .noiseFreq = 0.55f,
         .noiseDecay = 0.2f, .mix = 0.3f, .level = 0.779f},
        {.wave = SIN, .hz = 190.f, .toneDecay = 0.44f, .modRate = 0.7f, .modAmount = 0.15f, .noiseFreq = 0.6f,
         .noiseDecay = 0.2f, .mix = 0.3f, .level = 0.763f},
        {.noiseFilter = BP, .noiseEnv = MOD, .noiseFreq = 0.64f, .noiseQ = 0.2f, .noiseDecay = 0.48f, .mix = 1.f,
         .level = 0.596f},
        // Side stick
        {.wave = TRI, .noiseFilter = BP, .hz = 1100.f, .toneDecay = 0.14f, .modRate = 0.9f, .modAmount = 0.15f,
         .noiseFreq = 0.72f, .noiseQ = 0.35f, .noiseDecay = 0.1f, .mix = 0.45f, .level = 0.749f},
    }}},
    {"METL", {{
        // Metallic: audio rate LFOs (FM) on most sounds
        {.wave = SIN, .noiseFilter = BP, .hz = 50.f, .toneDecay = 0.5f, .modRate = 0.7f, .modAmount = 0.45f,
         .noiseFreq = 0.6f, .noiseQ = 0.3f, .noiseDecay = 0.15f, .mix = 0.2f, .drive = 0.12f, .level = 0.809f},
        {.wave = TRI, .modMode = LFO, .noiseFilter = HP, .hz = 260.f, .toneDecay = 0.3f, .modRate = 0.86f,
         .modAmount = 0.45f, .noiseFreq = 0.78f, .noiseDecay = 0.4f, .mix = 0.55f, .level = 0.760f},
        {.wave = SAW, .modMode = LFO, .noiseFilter = HP, .hz = 3500.f, .toneDecay = 0.2f, .modRate = 0.95f,
         .modAmount = 0.75f, .noiseFreq = 0.9f, .noiseQ = 0.4f, .noiseDecay = 0.16f, .mix = 0.55f, .level = 0.586f},
        {.wave = SAW, .modMode = LFO, .noiseFilter = HP, .hz = 3500.f, .toneDecay = 0.48f, .modRate = 0.95f,
         .modAmount = 0.75f, .noiseFreq = 0.9f, .noiseQ = 0.4f, .noiseDecay = 0.45f, .mix = 0.55f, .level = 0.511f},
        // Bell, low gong
        {.wave = SIN, .modMode = LFO, .hz = 700.f, .toneDecay = 0.6f, .modRate = 0.89f, .modAmount = 0.6f,
         .level = 0.739f},
        {.wave = TRI, .modMode = LFO, .hz = 140.f, .toneDecay = 0.65f, .modRate = 0.82f, .modAmount = 0.5f,
         .level = 0.742f},
        {.noiseFilter = BP, .noiseEnv = MOD, .noiseFreq = 0.7f, .noiseQ = 0.34f, .noiseDecay = 0.48f, .mix = 1.f,
         .level = 0.705f},
        // Cowbell
        {.wave = TRI, .modMode = LFO, .hz = 600.f, .toneDecay = 0.4f, .modRate = 0.84f, .modAmount = 0.22f,
         .eqFreq = 0.7f, .eqGain = 0.6f, .level = 0.751f},
    }}},
    {"DUB", {{
        // Dub: deep and round, with a long syndrum
        {.wave = SIN, .hz = 44.f, .toneDecay = 0.66f, .modRate = 0.5f, .modAmount = 0.3f, .drive = 0.04f,
         .eqFreq = 0.2f, .eqGain = 0.56f, .level = 0.798f},
        {.wave = TRI, .noiseFilter = BP, .hz = 300.f, .toneDecay = 0.3f, .modRate = 0.9f, .modAmount = 0.2f,
         .noiseFreq = 0.68f, .noiseQ = 0.35f, .noiseDecay = 0.5f, .mix = 0.5f, .eqFreq = 0.9f, .eqGain = 0.35f,
         .level = 0.806f},
        {.noiseFilter = BP, .noiseFreq = 0.82f, .noiseQ = 0.4f, .noiseDecay = 0.15f, .mix = 1.f, .level = 0.937f},
        {.noiseFilter = BP, .noiseEnv = LIN, .noiseFreq = 0.8f, .noiseQ = 0.4f, .noiseDecay = 0.5f, .mix = 1.f,
         .level = 0.802f},
        // Syndrum, bongo
        {.wave = SIN, .hz = 150.f, .toneDecay = 0.55f, .modRate = 0.4f, .modAmount = 0.4f, .level = 0.773f},
        {.wave = SIN, .hz = 380.f, .toneDecay = 0.3f, .modRate = 0.85f, .modAmount = 0.18f, .level = 0.762f},
        {.noiseEnv = MOD, .noiseFreq = 0.6f, .noiseQ = 0.25f, .noiseDecay = 0.52f, .mix = 1.f, .level = 0.590f},
        // Long falling syndrum
        {.wave = SIN, .hz = 260.f, .toneDecay = 0.6f, .modRate = 0.3f, .modAmount = 0.5f, .level = 0.740f},
    }}},
    {"BLIP", {{
        // Glitch: bleeps and broken noise
        {.wave = SIN, .noiseFilter = HP, .hz = 65.f, .toneDecay = 0.3f, .modRate = 0.95f, .modAmount = 0.7f,
         .noiseFreq = 0.9f, .noiseDecay = 0.04f, .mix = 0.3f, .level = 0.812f},
        {.wave = SAW, .modMode = NSE, .noiseFilter = BP, .hz = 320.f, .toneDecay = 0.22f, .modRate = 0.6f,
         .modAmount = 0.5f, .noiseFreq = 0.75f, .noiseQ = 0.4f, .noiseDecay = 0.25f, .mix = 0.55f, .level = 0.837f},
        {.noiseFilter = HP, .noiseFreq = 0.95f, .noiseQ = 0.6f, .noiseDecay = 0.08f, .mix = 1.f, .level = 0.613f},
        {.wave = SAW, .modMode = LFO, .noiseFilter = HP, .hz = 5000.f, .toneDecay = 0.3f, .modRate = 0.97f,
         .modAmount = 0.8f, .noiseFreq = 0.9f, .noiseDecay = 0.3f, .mix = 0.3f, .level = 0.634f},
        {.wave = SIN, .modMode = LFO, .hz = 1046.5f, .toneDecay = 0.18f, .modRate = 0.62f, .modAmount = 0.3f,
         .level = 0.728f},
        {.wave = SIN, .hz = 2093.f, .toneDecay = 0.1f, .level = 0.712f},
        {.noiseFilter = HP, .noiseEnv = MOD, .noiseFreq = 0.76f, .noiseQ = 0.3f, .noiseDecay = 0.45f, .mix = 1.f,
         .drive = 0.2f, .level = 0.811f},
        {.wave = TRI, .modMode = NSE, .hz = 440.f, .toneDecay = 0.35f, .modRate = 0.9f, .modAmount = 0.5f,
         .level = 0.744f},
    }}},
    {"BRKS", {{
        // Breaks: crunchy, like a sampled break
        {.wave = SIN, .hz = 58.f, .toneDecay = 0.44f, .modRate = 0.78f, .modAmount = 0.4f, .noiseFreq = 0.65f,
         .noiseQ = 0.3f, .noiseDecay = 0.15f, .mix = 0.35f, .drive = 0.3f, .level = 0.880f},
        {.wave = TRI, .noiseFilter = BP, .hz = 195.f, .toneDecay = 0.3f, .modRate = 0.8f, .modAmount = 0.3f,
         .noiseFreq = 0.72f, .noiseQ = 0.25f, .noiseDecay = 0.42f, .mix = 0.62f, .drive = 0.3f, .level = 0.855f},
        {.noiseFilter = HP, .noiseFreq = 0.84f, .noiseQ = 0.3f, .noiseDecay = 0.18f, .mix = 1.f, .drive = 0.25f,
         .level = 0.750f},
        {.noiseFilter = HP, .noiseFreq = 0.82f, .noiseQ = 0.3f, .noiseDecay = 0.42f, .mix = 1.f, .drive = 0.25f,
         .level = 0.734f},
        // Tom, ghost snare
        {.wave = SIN, .hz = 130.f, .toneDecay = 0.42f, .modRate = 0.6f, .modAmount = 0.2f, .noiseFreq = 0.5f,
         .mix = 0.3f, .drive = 0.25f, .level = 0.831f},
        {.wave = TRI, .noiseFilter = HP, .hz = 240.f, .toneDecay = 0.18f, .noiseFreq = 0.8f, .noiseDecay = 0.25f,
         .mix = 0.6f, .drive = 0.2f, .level = 0.794f},
        {.noiseFilter = BP, .noiseEnv = MOD, .noiseFreq = 0.66f, .noiseQ = 0.3f, .noiseDecay = 0.5f, .mix = 1.f,
         .drive = 0.3f, .level = 0.839f},
        // Crash
        {.wave = SAW, .modMode = LFO, .noiseFilter = HP, .hz = 3800.f, .toneDecay = 0.7f, .modRate = 0.93f,
         .modAmount = 0.55f, .noiseFreq = 0.8f, .noiseQ = 0.2f, .noiseDecay = 0.75f, .mix = 0.7f, .drive = 0.15f,
         .level = 0.647f},
    }}},
    {"PERC", {{
        // Hand percussion: frame drum, slap, shakers, congas, claps, clave
        {.wave = SIN, .hz = 70.f, .toneDecay = 0.45f, .modRate = 0.75f, .modAmount = 0.2f, .noiseFreq = 0.5f,
         .noiseQ = 0.2f, .noiseDecay = 0.2f, .mix = 0.3f, .level = 0.826f},
        {.wave = TRI, .noiseFilter = BP, .hz = 330.f, .toneDecay = 0.22f, .modRate = 0.9f, .modAmount = 0.25f,
         .noiseFreq = 0.74f, .noiseQ = 0.4f, .noiseDecay = 0.25f, .mix = 0.5f, .level = 0.797f},
        {.noiseFilter = HP, .noiseEnv = LIN, .noiseFreq = 0.82f, .noiseQ = 0.25f, .noiseAttack = 0.12f,
         .noiseDecay = 0.26f, .mix = 1.f, .level = 0.635f},
        {.noiseFilter = HP, .noiseEnv = LIN, .noiseFreq = 0.85f, .noiseQ = 0.3f, .noiseAttack = 0.2f,
         .noiseDecay = 0.4f, .mix = 1.f, .level = 0.564f},
        {.wave = SIN, .hz = 220.f, .toneDecay = 0.4f, .modRate = 0.85f, .modAmount = 0.18f, .level = 0.777f},
        {.wave = SIN, .hz = 330.f, .toneDecay = 0.36f, .modRate = 0.85f, .modAmount = 0.2f, .level = 0.760f},
        {.noiseFilter = BP, .noiseEnv = MOD, .noiseFreq = 0.62f, .noiseQ = 0.2f, .noiseDecay = 0.5f, .mix = 1.f,
         .level = 0.632f},
        {.wave = SIN, .hz = 2400.f, .toneDecay = 0.15f, .level = 0.711f},
    }}},
    {"CHIP", {{
        // 8-bit: triangle kick, raw noise, saw blips
        {.wave = TRI, .hz = 55.f, .toneDecay = 0.4f, .modRate = 0.85f, .modAmount = 0.6f, .level = 0.817f},
        {.wave = TRI, .noiseFilter = OFF, .noiseEnv = LIN, .hz = 180.f, .toneDecay = 0.2f, .modRate = 0.9f,
         .modAmount = 0.5f, .noiseDecay = 0.38f, .mix = 0.6f, .level = 0.753f},
        {.noiseFilter = HP, .noiseEnv = LIN, .noiseFreq = 0.95f, .noiseDecay = 0.15f, .mix = 1.f, .level = 0.782f},
        {.noiseFilter = HP, .noiseEnv = LIN, .noiseFreq = 0.95f, .noiseDecay = 0.42f, .mix = 1.f, .level = 0.751f},
        {.wave = SAW, .hz = 220.f, .toneDecay = 0.3f, .modRate = 0.8f, .modAmount = 0.6f, .level = 0.734f},
        {.wave = SAW, .modMode = LFO, .hz = 880.f, .toneDecay = 0.3f, .modRate = 0.55f, .modAmount = 0.5f,
         .level = 0.714f},
        {.noiseFilter = HP, .noiseEnv = MOD, .noiseFreq = 0.7f, .noiseDecay = 0.46f, .mix = 1.f, .level = 0.463f},
        // Coin
        {.wave = SIN, .modMode = LFO, .hz = 1318.5f, .toneDecay = 0.35f, .modRate = 0.35f, .modAmount = 0.1f,
         .level = 0.692f},
    }}},
}};
// clang-format on

} // namespace deluge::dsp::drum_synth::gin
