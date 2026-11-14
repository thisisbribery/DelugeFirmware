/*
 * BROPHET: the voice of Pro-54, Cmajor's port of Native Instruments' Pro-53, as a Deluge synth engine.
 *
 * Portions Copyright (C) 2024 Cmajor Software Ltd (Pro-54, GPL-3.0), originally Native Instruments' Pro-53.
 * Deluge integration Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * The oscillators below are Pro-54's "old" oscillators (the ones it plays: its newer ones are switched off), with its
 * anti-aliasing, osc A's sync to osc B, and poly mod, translated from Cmajor line for line. Modified: the wheel mod,
 * LFO, envelopes, glide, unison, filter and delay are gone (the Deluge has its own); osc B's DC is taken out of the
 * mix.
 */

#include "dsp/drum_synth/brophet.h"
#include <algorithm>
#include <arm_neon.h>
#include <cmath>

namespace deluge::dsp::drum_synth::brophet {

namespace {

/// min / max through NEON: no compare, so no wait for the compare's flags (VFP has no scalar min or max)
[[gnu::always_inline]] inline float fastClamp(float x, float lo, float hi) {
	return vget_lane_f32(vmin_f32(vmax_f32(vdup_n_f32(x), vdup_n_f32(lo)), vdup_n_f32(hi)), 0);
}
[[gnu::always_inline]] inline float fastMax(float x, float lo) {
	return vget_lane_f32(vmax_f32(vdup_n_f32(x), vdup_n_f32(lo)), 0);
}

[[gnu::always_inline]] inline float sgn(float x) {
	return x >= 0 ? 1.0f : -1.0f;
}

[[gnu::always_inline]] inline float getPhase(float ph2) {
	if (ph2 >= 1.0f) {
		return ph2 - 2.0f;
	}
	if (ph2 <= -1.0f) {
		return ph2 + 2.0f;
	}
	return ph2;
}

/// The 32-bit noise shift register Pro-54 uses
[[gnu::always_inline]] inline uint32_t nextNoise(uint32_t w) {
	const uint32_t feedback = ((w ^ (w >> 5) ^ (w >> 13)) & 1u) << 31;
	return (w >> 1) | feedback;
}

float unitRandom(uint32_t& seed) {
	seed = seed * 1664525u + 1013904223u;
	return static_cast<float>(seed >> 8) * (1.f / 8388608.f) - 1.f; // -1 to 1
}

/// Middle C as a phase increment, for osc B off the keys
constexpr float kMiddleCIncrement = 261.625565f / 44100.f * 4294967296.f;

/// The names Pro-54's oscillator code reads, per sample
struct Local {
	float mixerOscillatorA;
	float oscillatorA_SawA;
	bool oscillatorA_PulseActive;
	float oscillatorA_FM_PM;
	float oscillatorA_PW_PM;
	float oscillatorB_SawA;
	bool oscillatorB_TriA;
	float oscillatorB_PulseA;
	bool oscillatorSyncActive;
};

} // namespace

void trigger(Voice& v, float analog, uint32_t seed) {
	// Pro-53's Analog knob: up to about a quarter tone of drift on each oscillator and 10x as much on the poly mod
	// depth (it also drifts the filter, which here is the Deluge's)
	const float tune = (analog > 0.f) ? expf(5.f * analog) * 0.19205f : 0.f;
	v.driftA = 0.01f * tune * unitRandom(seed);
	v.driftB = 0.01f * tune * unitRandom(seed);
	v.pmEnvRnd = 1.f + 0.1f * tune * unitRandom(seed);
	// Free running oscillators: each note catches them somewhere
	v.oscillatorA_Phase = unitRandom(seed) * 0.999f;
	v.oscillatorB_Phase = unitRandom(seed) * 0.999f;
	v.oscillatorA_Saw_AA2 = v.oscillatorA_Pulse_AA2 = 0.f;
	v.oscillatorB_Saw_AA2 = v.oscillatorB_Pulse_AA2 = 0.f;
	v.eqLast = 0.f;
	v.noise = seed | 1u;
	v.started = false;
}

float mixDc(const Settings& s) {
	// Over a cycle osc A's pulse averages -pw (its saw 0); osc B's saw 0.5, its pulse 0.5 + 0.5 pw, its triangle 0, and
	// its pulse and triangle are subtracted. Then the EQ, whose gain at DC is 1 - 0.171.
	const float pulseA = s.oscillatorA_PulseActive ? -s.mixerOscillatorA * std::clamp(s.pwA, -1.f, 1.f) : 0.f;
	const float pulseB = (s.oscillatorB_PulseA != 0.f) ? 0.5f + 0.5f * std::clamp(s.pwB, -1.f, 1.f) : 0.f;
	return (pulseA + s.mixerOscillatorB * (s.oscillatorB_SawA - pulseB)) * (1.f - 0.171f);
}

void render(Voice& voice, const Settings& settings, uint32_t baseIncrement, float env, float* out, int32_t n) {
	// Work on a copy: out is floats too, so writing it could (as far as the compiler knows) change the voice, which
	// would make it reload and store every phase and anti-aliasing term each sample. A local stays in registers.
	Voice v = voice;
	// Osc A at the Deluge's pitch, osc B against it (or against middle C off the keys); a cycle is 2 here
	const float baseA = static_cast<float>(baseIncrement) * (1.f / 2147483648.f);
	const float baseB = (settings.keysB ? static_cast<float>(baseIncrement) : kMiddleCIncrement) * (1.f / 2147483648.f);
	v.oscillatorA_F = baseA * exp2f(v.driftA * (1.f / 12.f));
	v.oscillatorB_F = baseB * exp2f((settings.semitonesB + v.driftB) * (1.f / 12.f));
	// Osc B's frequency holds for the buffer: one divide here, not one per pulse edge
	const float oscillatorB_Rcp = (v.oscillatorB_F > 0.f) ? 1.f / v.oscillatorB_F : 0.f;

	// The mixer levels and pulse widths ramp across the buffer, as Pro-54 smooths its knobs
	const float sawA = settings.mixerOscillatorA * settings.sawActiveA;
	if (!v.started) {
		v.mixA = settings.mixerOscillatorA;
		v.sawA = sawA;
		v.mixB = settings.mixerOscillatorB;
		v.pwA = settings.pwA;
		v.pwB = settings.pwB;
		v.started = true;
	}
	const float step = (n > 0) ? 1.f / static_cast<float>(n) : 0.f;
	const float dMixA = (settings.mixerOscillatorA - v.mixA) * step;
	const float dSawA = (sawA - v.sawA) * step;
	const float dMixB = (settings.mixerOscillatorB - v.mixB) * step;
	const float dPwA = (settings.pwA - v.pwA) * step;
	const float dPwB = (settings.pwB - v.pwB) * step;
	// The saws' anti-aliasing lands each wrap a sample late, which leaves a DC of their phase step: taken out too
	const float dc =
	    mixDc(settings)
	    + (1.f - 0.171f)
	          * (sawA * v.oscillatorA_F + settings.mixerOscillatorB * settings.oscillatorB_SawA * v.oscillatorB_F);

	Local s{v.mixA,
	        v.sawA,
	        settings.oscillatorA_PulseActive,
	        settings.oscillatorA_FM_PM,
	        settings.oscillatorA_PW_PM,
	        settings.oscillatorB_SawA,
	        settings.oscillatorB_TriA,
	        settings.oscillatorB_PulseA,
	        settings.oscillatorSyncActive};
	float mixerB = v.mixB;
	float pwA = v.pwA;
	float pwB = v.pwB;
	const float polymodB = settings.polymodOscillatorB;
	const float polymodEnv = settings.polymodEnvelopeF * env * v.pmEnvRnd;
	const float mixerNoise = settings.mixerNoise;

	for (int32_t i = 0; i < n; i++) {
		s.mixerOscillatorA += dMixA;
		s.oscillatorA_SawA += dSawA;
		mixerB += dMixB;
		pwA += dPwA;
		pwB += dPwB;
		const float PulseWidthA = pwA; // clipped after poly mod is added
		const float PulseWidthB = fastClamp(pwB, -1.0f, 1.0f);

		float OscB_Saw, OscB_Pulse, OscB_Tri, OscA_Saw, OscA_Pulse;
		bool oscillatorSyncNow = false;
		float Osc_Sync_x = 0.f;

		// Oscillator B
		{
			float f = v.oscillatorB_F;
			float ph = v.oscillatorB_Phase;

			f = std::max(f, 0.0f);

			if (f >= 1.0f) {
				ph = 0.0f;
				OscB_Saw = 0.0f;
				OscB_Pulse = 0.0f;
				OscB_Tri = 0.0f;
				oscillatorSyncNow = false;
			}
			else {
				ph = (ph + f);

				if (ph >= 1.0f) //  ph_mask_p=1, dn_mask=1
				{
					oscillatorSyncNow = s.oscillatorSyncActive; // Sync OSC-A now if sync is on

					ph = ph - 2.0f;

					float d = 1.0f + ph; // float d = 1 - ph_abs;
					const float f_rcp = oscillatorB_Rcp;
					float x = d * f_rcp;
					float x_1 = x - 1.0f;
					Osc_Sync_x = x;

					const float aa_dn_1 = (2.0f - x * x); // first part of anti-aliasing
					const float y_saw = ph + v.oscillatorB_Saw_AA2 + aa_dn_1;
					const float aa_dn_2 = x_1 * x_1;
					v.oscillatorB_Saw_AA2 = aa_dn_2;                // second part of anti-aliasing, use once then reset
					OscB_Saw = s.oscillatorB_SawA * (y_saw + 1.0f); // add one to give correct modulation DC offset

					if (s.oscillatorB_PulseA != 0) {
						const float ph_pwm = PulseWidthB;
						const float ph2 = ph - ph_pwm;
						const float ph2_sign = sgn(ph2); // (ph2 >= 0 ? 1 : -1);

						const float ph3 = getPhase(ph2);
						float pulse_aa_2 = v.oscillatorB_Pulse_AA2;

						if (ph3 * (ph3 - f) < 0.0f) // up_mask=1
						{
							const float ph3_abs = std::fabs(ph3);
							const float x3 = ph3_abs * f_rcp;
							const float x3_1 = x3 - 1.0f;
							const float aa_up_1 = (2.0f - x3 * x3); // first part of anti-aliasing
							const float aa_up_2 = x3_1 * x3_1;
							const float y_pulse = ph2_sign + pulse_aa_2 + aa_dn_1 - aa_up_1;
							pulse_aa_2 = aa_dn_2 - aa_up_2;     // second part of anti-aliasing, use once then reset
							OscB_Pulse = 0.5f - 0.5f * y_pulse; // add one to give correct modulation DC offset
						}
						else {
							const float y_pulse = ph2_sign + pulse_aa_2 + aa_dn_1;
							pulse_aa_2 = aa_dn_2;               // second part of anti-aliasing, reset
							OscB_Pulse = 0.5f - 0.5f * y_pulse; // add one to give correct modulation DC offset
						}
						v.oscillatorB_Pulse_AA2 = pulse_aa_2;
					}
					else {
						OscB_Pulse = 0.0f;
					}

					if (s.oscillatorB_TriA) {
						if (ph * (ph - f) < 0.0f) // btm_mask=1
						{
							const float aa_top = (x * (2.0f - x) - 1.0f);
							const float aa_btm = (1.0f - f_rcp * (2.0f + f_rcp));
							OscB_Tri = 0.5f - (0.5f * f - ph) - f * (aa_top - aa_btm);
						}
						else // btm_mask=0
						{
							const float aa_top = (x * (2.0f - x) - 1.0f);
							OscB_Tri = 0.5f - (0.5f * f - ph) - f * (aa_top);
						}
					}
					else {
						OscB_Tri = 0.0f;
					}
				}
				else {
					oscillatorSyncNow = false; // Sync now if sync is on

					const float y_saw = ph + v.oscillatorB_Saw_AA2;
					v.oscillatorB_Saw_AA2 = 0;                      // second part of anti-aliasing, reset
					OscB_Saw = s.oscillatorB_SawA * (y_saw + 1.0f); // add one to give correct modulation DC offset

					if (s.oscillatorB_PulseA != 0) {
						const float ph_pwm = PulseWidthB;
						const float ph2 = ph - ph_pwm;
						const float ph2_sign = sgn(ph2);

						const float ph3 = getPhase(ph2);
						float pulse_aa_2 = v.oscillatorB_Pulse_AA2;

						if (ph3 * (ph3 - f) < 0.0f) // up_mask=1
						{
							const float ph3_abs = std::fabs(ph3);
							const float x3 = ph3_abs * oscillatorB_Rcp;
							const float x3_1 = x3 - 1.0f;
							const float aa_up_1 = (2.0f - x3 * x3); // first part of anti-aliasing
							const float aa_up_2 = x3_1 * x3_1;      // second part of anti-aliasing, use once then reset
							const float y_pulse = ph2_sign + pulse_aa_2 - aa_up_1;
							pulse_aa_2 = -aa_up_2; // second part of anti-aliasing, use once then reset
							OscB_Pulse = 0.5f - 0.5f * y_pulse;
						}
						else {
							const float y_pulse = ph2_sign + pulse_aa_2;
							pulse_aa_2 = 0; // second part of anti-aliasing, reset
							OscB_Pulse = 0.5f - 0.5f * y_pulse;
						}

						v.oscillatorB_Pulse_AA2 = pulse_aa_2;
					}
					else {
						OscB_Pulse = 0.0f;
					}

					if (s.oscillatorB_TriA) {
						const float ph_sign = sgn(ph); // for tri

						if (ph * (ph - f) < 0) // btm_mask=1
						{
							const float ph_abs = std::fabs(ph);
							const float x5 = ph_abs * oscillatorB_Rcp;
							const float aa_btm = (x5 * (2.0f - x5) - 1.0f);
							OscB_Tri = 0.5f + ph_sign * (0.5f * f - ph) + f * aa_btm;
						}
						else // btm_mask=0
						{
							OscB_Tri = 0.5f + ph_sign * (0.5f * f - ph);
						}
					}
					else {
						OscB_Tri = 0.0f;
					}
				}
			}

			v.oscillatorB_Phase = ph;
		}

		// Poly Mod
		const float OscB_PulseTri = OscB_Pulse + OscB_Tri;
		const float PolyMod = polymodB * (OscB_Saw + OscB_PulseTri) + polymodEnv;

		// Oscillator A

		{
			float f = fastMax(v.oscillatorA_F * (1.0f + s.oscillatorA_FM_PM * PolyMod), 0.0f);

			float ph = v.oscillatorA_Phase;

			if (f >= 1.0f) // f_mask=0
			{
				ph = 0.0f;
				OscA_Saw = 0.0f;
				OscA_Pulse = 0.0f;
			}
			else {
				ph = (ph + f);

				if (oscillatorSyncNow) {
					const float x_snc = Osc_Sync_x;
					const float x_snc_1 = x_snc - 1.0f;
					const float d_snc = x_snc * f;
					const float snc_ph = d_snc - 1.0f;
					const float ph_adj = ph - d_snc;

					if (ph_adj >= 1.0f) //  ph_mask_p=1, dn_mask=1
					{
						ph = ph - 2.0f;

						const float ph_abs = std::fabs(ph);
						const float d = 1.0f - ph_abs;
						const float f_rcp = 1.0f / f; // reciprocal estimate operator
						const float x = d * f_rcp;
						const float x_1 = x - 1.0f;

						float ph2 = ph;
						const float ph_snc_change = (snc_ph - ph);
						ph = ph + ph_snc_change;

						const float snc_aa_amp = (-0.5f) * ph_snc_change;

						const float aa_dn_1 = (2.0f - x * x);          // first part of anti-aliasing
						const float aa_snc_1 = (2.0f - x_snc * x_snc); // first part of anti-aliasing
						const float aa_snc_1_saw = aa_snc_1 * snc_aa_amp;

						const float aa_dn_2 = x_1 * x_1;          // second part of anti-aliasing, use once then reset
						const float aa_snc_2 = x_snc_1 * x_snc_1; // second part of anti-aliasing, use once then reset
						const float aa_snc_2_saw =
						    aa_snc_2 * snc_aa_amp; // second part of anti-aliasing, use once then reset

						const float y_saw = ph + v.oscillatorA_Saw_AA2 + aa_dn_1 + aa_snc_1_saw;
						v.oscillatorA_Saw_AA2 = aa_dn_2 + aa_snc_2_saw;
						OscA_Saw = s.oscillatorA_SawA * y_saw;

						if (s.oscillatorA_PulseActive) {
							float ph_pwm = PulseWidthA + s.oscillatorA_PW_PM * PolyMod;
							ph_pwm = fastClamp(ph_pwm, -1.0f, 1.0f); // clip to range -1...1
							ph2 = ph2 - ph_pwm;

							const float ph3 = getPhase(ph2);
							float ph4 = ph2 + ph_snc_change;

							float aa_up_snc_1 = 0.0f, aa_up_snc_2 = 0.0f; // Cmajor starts a variable at 0
							if (ph4 * (ph4 + ph_snc_change - f) < 0.0f)   // up_snc_mask=1
							{
								float x4 = std::fabs(ph4) * f_rcp;
								float x4_1 = x4 - 1.0f;
								aa_up_snc_1 = (2.0f - x4 * x4); // first part of anti-aliasing
								aa_up_snc_2 = x4_1 * x4_1;      // second part of anti-aliasing, use once then reset
							}
							else {
								aa_up_snc_1 = 0.0f; // first part of anti-aliasing
								aa_up_snc_2 = 0.0f; // second part of anti-aliasing, use once then reset
							}

							float aa_snc_1_pulse = 0.0f, aa_snc_2_pulse = 0.0f;

							if ((ph2 - d_snc) > 0.0f) // snc_pulse_mask=1
							{
								aa_snc_1_pulse = aa_snc_1;
								aa_snc_2_pulse = aa_snc_2;
							}
							else {
								aa_snc_1_pulse = 0.0f;
								aa_snc_2_pulse = 0.0f;
							}

							const float ph2_sign = sgn(ph - ph_pwm);
							float pulse_aa_2 = v.oscillatorA_Pulse_AA2;

							if ((ph3 - d_snc) * (ph3 - f) < 0.0f) // up_mask=1
							{
								const float ph3_abs = std::fabs(ph3);
								const float x3 = ph3_abs * f_rcp;
								const float x3_1 = x3 - 1.0f;
								const float aa_up_1 = (2.0f - x3 * x3); // first part of anti-aliasing
								const float aa_up_2 = x3_1 * x3_1; // second part of anti-aliasing, use once then reset
								const float y_pulse =
								    ph2_sign + pulse_aa_2 + aa_dn_1 - aa_up_1 - aa_up_snc_1 + aa_snc_1_pulse;
								pulse_aa_2 = aa_dn_2 - aa_up_2 - aa_up_snc_2
								             + aa_snc_2_pulse; // second part of anti-aliasing, use once then reset
								OscA_Pulse = s.mixerOscillatorA * y_pulse;
							}
							else {
								const float y_pulse = ph2_sign + pulse_aa_2 + aa_dn_1 - aa_up_snc_1 + aa_snc_1_pulse;
								pulse_aa_2 = aa_dn_2 - aa_up_snc_2
								             + aa_snc_2_pulse; // second part of anti-aliasing, use once then reset
								OscA_Pulse = s.mixerOscillatorA * y_pulse;
							}
							v.oscillatorA_Pulse_AA2 = pulse_aa_2;
						}
						else {
							OscA_Pulse = 0.0f;
						}
					}
					else // dn_mask=0
					{
						float ph2 = ph;
						const float ph_snc_change = (snc_ph - ph);
						ph = ph + ph_snc_change;

						const float snc_aa_amp = (-0.5f) * ph_snc_change;

						const float aa_snc_1 = (2.0f - x_snc * x_snc); // first part of anti-aliasing
						const float aa_snc_1_saw = aa_snc_1 * snc_aa_amp;

						const float aa_snc_2 = x_snc_1 * x_snc_1; // second part of anti-aliasing, use once then reset
						const float aa_snc_2_saw =
						    aa_snc_2 * snc_aa_amp; // second part of anti-aliasing, use once then reset

						const float y_saw = ph + v.oscillatorA_Saw_AA2 + aa_snc_1_saw;
						v.oscillatorA_Saw_AA2 = aa_snc_2_saw;
						OscA_Saw = s.oscillatorA_SawA * y_saw;

						if (s.oscillatorA_PulseActive) {
							const float ph_pwm = fastClamp(PulseWidthA + s.oscillatorA_PW_PM * PolyMod, -1.0f, 1.0f);
							ph2 = ph2 - ph_pwm;

							const float ph3 = getPhase(ph2);
							const float ph4 = ph2 + ph_snc_change;
							float aa_up_snc_1 = 0.0f, aa_up_snc_2 = 0.0f; // Cmajor starts a variable at 0

							if (ph4 * (ph4 + ph_snc_change - f) < 0.0f) // up_snc_mask=1
							{
								const float x4 = std::fabs(ph4) / f;
								const float x4_1 = x4 - 1.0f;
								aa_up_snc_1 = (2.0f - x4 * x4); // first part of anti-aliasing
								aa_up_snc_2 = x4_1 * x4_1;      // second part of anti-aliasing, use once then reset
							}

							float aa_snc_1_pulse = 0.0f, aa_snc_2_pulse = 0.0f;

							if ((ph2 - d_snc) > 0.0f) // snc_pulse_mask=1
							{
								aa_snc_1_pulse = aa_snc_1;
								aa_snc_2_pulse = aa_snc_2;
							}
							else {
								aa_snc_1_pulse = 0.0f;
								aa_snc_2_pulse = 0.0f;
							}

							const float ph2_sign = sgn(ph - ph_pwm);
							float pulse_aa_2 = v.oscillatorA_Pulse_AA2;

							if ((ph3 - d_snc) * (ph3 - f) < 0.0f) // up_mask=1
							{
								const float ph3_abs = std::fabs(ph3);
								const float x3 = ph3_abs / f;
								const float x3_1 = x3 - 1.0f;
								const float aa_up_1 = (2.0f - x3 * x3); // first part of anti-aliasing
								const float aa_up_2 = x3_1 * x3_1; // second part of anti-aliasing, use once then reset
								const float y_pulse = ph2_sign + pulse_aa_2 - aa_up_1 - aa_up_snc_1 + aa_snc_1_pulse;
								pulse_aa_2 = -aa_up_2 - aa_up_snc_2
								             + aa_snc_2_pulse; // second part of anti-aliasing, use once then reset
								OscA_Pulse = s.mixerOscillatorA * y_pulse;
							}
							else {
								const float y_pulse = ph2_sign + pulse_aa_2 - aa_up_snc_1 + aa_snc_1_pulse;
								pulse_aa_2 =
								    -aa_up_snc_2 + aa_snc_2_pulse; // second part of anti-aliasing, use once then reset
								OscA_Pulse = s.mixerOscillatorA * y_pulse;
							}

							v.oscillatorA_Pulse_AA2 = pulse_aa_2;
						}
						else {
							OscA_Pulse = 0.0f;
						}
					}
				}
				else // no sync
				{
					if (ph >= 1.0f) //  ph_mask_p=1, dn_mask=1
					{
						ph = ph - 2.0f;

						const float d = 1.0f + ph;    // float d = 1 - ph_abs;
						const float f_rcp = 1.0f / f; // reciprocal estimate operator
						const float x = d * f_rcp;
						const float x_1 = x - 1.0f;
						const float aa_dn_1 = (2.0f - x * x); // first part of anti-aliasing
						const float aa_dn_2 = x_1 * x_1;      // second part of anti-aliasing, use once then reset

						const float y_saw = ph + v.oscillatorA_Saw_AA2 + aa_dn_1;
						v.oscillatorA_Saw_AA2 = aa_dn_2;
						OscA_Saw = s.oscillatorA_SawA * y_saw;

						if (s.oscillatorA_PulseActive) {
							const float ph_pwm = fastClamp(PulseWidthA + s.oscillatorA_PW_PM * PolyMod, -1.0f, 1.0f);
							const float ph2 = ph - ph_pwm;
							const float ph2_sign = sgn(ph2);

							const float ph3 = getPhase(ph2);
							float pulse_aa_2 = v.oscillatorA_Pulse_AA2;

							if (ph3 * (ph3 - f) < 0.0f) // up_mask=1
							{
								const float ph3_abs = std::fabs(ph3);
								const float x3 = ph3_abs * f_rcp;
								const float x3_1 = x3 - 1.0f;
								const float aa_up_1 = (2.0f - x3 * x3); // first part of anti-aliasing
								const float aa_up_2 = x3_1 * x3_1; // second part of anti-aliasing, use once then reset
								const float y_pulse = ph2_sign + pulse_aa_2 + aa_dn_1 - aa_up_1;
								pulse_aa_2 = aa_dn_2 - aa_up_2; // second part of anti-aliasing, use once then reset
								OscA_Pulse = s.mixerOscillatorA * y_pulse;
							}
							else {
								const float y_pulse = ph2_sign + pulse_aa_2 + aa_dn_1;
								pulse_aa_2 = aa_dn_2; // second part of anti-aliasing, reset
								OscA_Pulse = s.mixerOscillatorA * y_pulse;
							}
							v.oscillatorA_Pulse_AA2 = pulse_aa_2;
						}
						else {
							OscA_Pulse = 0.0f;
						}
					}
					else // dn_mask=0
					{
						const float y_saw = ph + v.oscillatorA_Saw_AA2;
						v.oscillatorA_Saw_AA2 = 0;
						OscA_Saw = s.oscillatorA_SawA * y_saw;

						if (s.oscillatorA_PulseActive) {
							const float ph_pwm = fastClamp(PulseWidthA + s.oscillatorA_PW_PM * PolyMod, -1.0f, 1.0f);
							const float ph2 = ph - ph_pwm;
							const float ph2_sign = sgn(ph2);

							const float ph3 = getPhase(ph2);
							float pulse_aa_2 = v.oscillatorA_Pulse_AA2;

							if (ph3 * (ph3 - f) < 0.0f) // up_mask=1
							{
								const float ph3_abs = std::fabs(ph3);
								const float x3 = ph3_abs / f;
								const float x3_1 = x3 - 1.0f;
								const float aa_up_1 = (2.0f - x3 * x3); // first part of anti-aliasing
								const float aa_up_2 = x3_1 * x3_1; // second part of anti-aliasing, use once then reset
								const float y_pulse = ph2_sign + pulse_aa_2 - aa_up_1;
								pulse_aa_2 = -aa_up_2; // second part of anti-aliasing, use once then reset
								OscA_Pulse = s.mixerOscillatorA * y_pulse;
							}
							else {
								const float y_pulse = ph2_sign + pulse_aa_2;
								pulse_aa_2 = 0; // second part of anti-aliasing, use once then reset
								OscA_Pulse = s.mixerOscillatorA * y_pulse;
							}

							v.oscillatorA_Pulse_AA2 = pulse_aa_2;
						}
						else {
							OscA_Pulse = 0.0f;
						}
					}
				}
			}

			v.oscillatorA_Phase = ph;
		}

		// EQ: boosts the top (up to 3.1 dB at Nyquist) to make up for the anti-aliasing
		const float EQ_In = OscA_Pulse + OscA_Saw + mixerB * (OscB_Saw - OscB_PulseTri);
		const float EQ_Out = EQ_In + -0.171f * v.eqLast;
		v.eqLast = EQ_In;

		// Noise, then the mixer
		v.noise = nextNoise(v.noise);
		const float Noise_White = -0.5f + static_cast<float>(v.noise & 1u);
		out[i] = EQ_Out + mixerNoise * Noise_White - dc;
	}

	v.mixA = settings.mixerOscillatorA;
	v.sawA = sawA;
	v.mixB = settings.mixerOscillatorB;
	v.pwA = settings.pwA;
	v.pwB = settings.pwB;
	voice = v;
}

} // namespace deluge::dsp::drum_synth::brophet
