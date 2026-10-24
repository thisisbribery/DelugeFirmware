/*
 * The TIA and IIA engines from the XIA engine set for the Model:Cycles, ported to the Deluge. See xia.h.
 *
 * The arithmetic is the original's, step for step: 32-bit wrapping integer maths, and the ColdFire EMAC's signed
 * fractional multiply (mul()) wherever the original used it. Keep it that way: the tests compare every sample and every
 * word of state with the original.
 *
 * Not for distribution until the engines' author has agreed to it and to a GPL-3.0 compatible licence.
 */

#include "dsp/drum_synth/xia.h"
#include <algorithm>
#include <limits>

extern "C" {
extern const int32_t xiaRatioFm[8 * 129];
extern const int32_t xiaRatioAdditive[6 * 129];
extern const int32_t xiaLowPassCoef[640];
extern const int32_t xiaAttackStep[64];
extern const int16_t xiaDecayCurve[129];
extern const int32_t xiaDecayCoef[960];
extern const int32_t xiaPitchExp[768];
extern const int16_t xiaSine[1024];
}

namespace deluge::dsp::drum_synth::xia {

namespace {

constexpr int32_t kMax = std::numeric_limits<int32_t>::max();
constexpr int32_t kKnobMax = 0x7F00;
constexpr int32_t kPitchMax = 0x870000;

/// The EMAC's signed fractional multiply-accumulate into a clear accumulator, then read with saturation
int32_t mul(int32_t a, int32_t b) {
	const int64_t p = (static_cast<int64_t>(a) * b) >> 31;
	return static_cast<int32_t>(std::clamp<int64_t>(p, std::numeric_limits<int32_t>::min(), kMax));
}

int32_t sine(uint32_t phase) {
	return xiaSine[phase >> 22];
}

/// 32-bit wrapping multiply, as the ColdFire's muls.l
uint32_t wrap(int32_t a, int32_t b) {
	return static_cast<uint32_t>(a) * static_cast<uint32_t>(b);
}

/// 32-bit wrapping add and subtract
int32_t add(int32_t a, int32_t b) {
	return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}

int32_t sub(int32_t a, int32_t b) {
	return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

uint32_t increment(int32_t pitch);

} // namespace

uint32_t phaseIncrement(int32_t pitch) {
	return increment(pitch);
}

namespace {

/// 32-bit phase increment at 48 kHz for a pitch in 16.16 semitones
uint32_t increment(int32_t pitch) {
	if (pitch > kPitchMax) {
		pitch = kPitchMax;
	}
	uint32_t index = 0;
	uint32_t octave = 0;
	if (pitch >= 0) {
		index = static_cast<uint32_t>(pitch) >> 10;
		octave = index / 768;
		index %= 768;
	}
	const uint64_t p = static_cast<uint64_t>(static_cast<uint32_t>(xiaPitchExp[index])) * 0xB29A61Au;
	return static_cast<uint32_t>(p >> (38 - octave));
}

/// Index into xiaDecayCoef for a decay setting, interpolating xiaDecayCurve
int32_t decayIndex(int32_t decay) {
	if (decay < 0) {
		return xiaDecayCurve[0];
	}
	if (decay > 0x7FFF) {
		decay = 0x7FFF;
	}
	const int32_t i = decay >> 8;
	const int32_t a = xiaDecayCurve[i];
	return ((static_cast<int32_t>(decay & 0xFF) * 0x80 * (xiaDecayCurve[i + 1] - a)) >> 15) + a;
}

int32_t decayCoefficient(int32_t index) {
	return xiaDecayCoef[std::clamp<int32_t>(index, 0, 959)];
}

/// TIA's Freq knob: an offset in 16.16 semitones, gentle near the centre and +-24 at the ends
int32_t tiaFreqOffset(int32_t v) {
	if (v > kKnobMax) {
		return 0x180000;
	}
	if (v >= 0x4000) {
		const int32_t d = v - 0x4000;
		const int32_t s = d / 21;
		if (d <= 0x1514) {
			return s * s * 4;
		}
		if (d <= 0x2A14) {
			return (s - 0x100) * 0x800 + 0x40000;
		}
		return (s - 0x200) * 0xC00 + 0xC0000;
	}
	const int32_t s = (0x4000 - std::max<int32_t>(v, 0)) / 21;
	if (v < 0xEC) {
		return -0x180000;
	}
	if (v < 0x15EC) {
		return -((s - 0x200) * 0xC00 + 0xC0000);
	}
	if (v < 0x2AEC) {
		return -((s - 0x100) * 0x800 + 0x40000);
	}
	return -(s * s * 4);
}

/// The output stage both engines share: optional 4-pole low-pass, its fade-in gain, the drive folder and saturator
struct PostSettings {
	bool filterOff;
	int32_t coefficient;
	int32_t gain;
	int32_t drive; ///< The raw setting: the folder only runs above 0xFD
	int32_t foldGain;
	int32_t saturation; ///< Mix of the saturator, 0 = off
	int32_t offset;     ///< DC removed after saturating
};

/// semitonesAtZero / semitonesSpan: the cutoff range in semitones relative to the note, which differs per engine
PostSettings postSettings(int32_t lowPass, int32_t drive, int32_t pitch, int32_t span, int32_t bottom) {
	PostSettings p{};
	if (lowPass >= 0x7C00) {
		p.filterOff = true;
		p.gain = kMax;
	}
	else {
		int32_t cutoff;
		if (lowPass < 0) {
			cutoff = pitch - bottom * 0x10000;
			p.gain = 0;
		}
		else {
			cutoff = ((lowPass * span) / kKnobMax - bottom) * 0x10000 + pitch;
			p.gain = (lowPass < 0x100) ? 0 : (lowPass < 0x500) ? (lowPass - 0x100) * 0x1FFFFF : kMax;
		}
		p.coefficient =
		    (cutoff >= 0xA00000) ? xiaLowPassCoef[0x27F] : xiaLowPassCoef[std::max<int32_t>(cutoff >> 14, 0)];
	}

	p.drive = drive;
	const int32_t d = (drive < 0) ? 0 : std::min<int32_t>(drive, kKnobMax) / 0xFE;
	if (drive < 0x5F40) {
		p.foldGain = (static_cast<int16_t>(d * d) * 0x3C00) / 0x2400 + 0x400;
	}
	else {
		p.foldGain = 0x4000;
		p.saturation = (drive > 0x7EFF) ? kMax : (d - 0x60) * 0x4000000;
		p.offset = (p.saturation != 0) ? mul(0x30000000, p.saturation) : 0;
	}
	return p;
}

void post(const PostSettings& p, int32_t* buffer, int32_t* lowPass) {
	if (p.filterOff) {
		std::fill(lowPass, lowPass + 4, buffer[kBlockSize - 1]);
	}
	else {
		int32_t y0 = lowPass[0], y1 = lowPass[1], y2 = lowPass[2], y3 = lowPass[3];
		for (int32_t i = 0; i < kBlockSize; i++) {
			const int32_t x = std::clamp<int32_t>(buffer[i], -0x3FFFFFFF, 0x3FFFFFFF);
			y0 = add(y0, mul(sub(x, y0), p.coefficient));
			y1 = add(y1, mul(sub(y0, y1), p.coefficient));
			y2 = add(y2, mul(sub(y1, y2), p.coefficient));
			y3 = add(y3, mul(sub(y2, y3), p.coefficient));
			buffer[i] = y3;
		}
		lowPass[0] = y0;
		lowPass[1] = y1;
		lowPass[2] = y2;
		lowPass[3] = y3;
	}

	if (p.gain != kMax) {
		for (int32_t i = 0; i < kBlockSize; i++) {
			buffer[i] = mul(buffer[i], p.gain);
		}
	}

	if (p.drive > 0xFD) {
		for (int32_t i = 0; i < kBlockSize; i++) {
			// Triangle fold: wrap the scaled signal and mirror every other half
			uint32_t u = wrap(p.foldGain, buffer[i] >> 10) + 0x40000000u;
			if (u & 0x80000000u) {
				u = ~u;
			}
			int32_t f = static_cast<int32_t>(u + 0xC0000000u);
			if (p.saturation != 0) {
				f = add(f, mul(((f >= 0) ? 0x30000000 : -0x30000000) - f, p.saturation));
			}
			buffer[i] = sub(f, p.offset);
		}
	}
}

/// Apply the amp gain ramp, optionally through Punch's soft clipper
void output(const int32_t* buffer, int32_t gain, int32_t gainEnd, bool punch, int32_t* out) {
	int32_t step = gainEnd - gain;
	step = (step < 0) ? (step + 0x1F) >> 5 : step >> 5;
	for (int32_t i = 0; i < kBlockSize; i++) {
		int32_t v = mul(buffer[i], gain);
		if (punch) {
			v = std::clamp<int32_t>(v, -0x30000000, 0x30000000);
			const int32_t cube = static_cast<int32_t>(static_cast<uint32_t>(mul(mul(v, v), v)) << 4);
			v = (v - mul(cube, 0x12F684BD)) * 2;
		}
		out[i] = v;
		gain += step;
	}
}

} // namespace

// TIA -----------------------------------------------------------------------------------------------------------------

void reset(TiaState& s) {
	s = TiaState{};
	s.declick = kMax;
}

void render(TiaState& s, const TiaSettings& t, bool trigger, bool fadeOut, int32_t* out) {
	const int32_t variant = (static_cast<uint32_t>(t.variant) > 2) ? 0 : t.variant;

	if (trigger) {
		std::fill(s.phase, s.phase + 4, 0);
		s.feedback = 0;
		s.envelope = kMax;
		s.declick = kMax;
		s.active = 1;
		s.held = t.gate;
		if (t.gate && t.release && !fadeOut) {
			s.held = 0;
		}
	}
	else {
		if (!t.gate || (t.release && !fadeOut)) {
			s.held = 0;
		}
		if (!s.active) {
			std::fill(out, out + kBlockSize, 0);
			return;
		}
	}

	// Amp envelope: one exponential step per block
	int32_t index = decayIndex(t.decay);
	if (t.punch) {
		index += 0x20;
	}
	const int32_t envelope = s.envelope;
	int32_t newEnvelope = envelope;
	if (!s.held && t.decay < kKnobMax) {
		newEnvelope = mul(envelope, decayCoefficient(index));
	}
	s.envelope = newEnvelope;
	const int32_t declick = s.declick;
	const int32_t newDeclick = fadeOut ? 0 : kMax;
	s.declick = newDeclick;

	uint32_t inc[3];
	for (int32_t i = 0; i < 3; i++) {
		inc[i] = increment(t.pitch + tiaFreqOffset(t.freq[i]));
	}
	const uint32_t carrierInc = increment(t.pitch);

	int32_t level[3];
	int32_t depth[3];
	for (int32_t i = 0; i < 3; i++) {
		const int32_t x = std::clamp<int32_t>(t.blendex[i], 0, 0x7FFF);
		level[i] = 0x7FFF - x;
		depth[i] = static_cast<int32_t>((static_cast<int64_t>(x) * 0x26666) >> 15);
	}
	const int32_t sum = level[0] + level[1] + level[2];
	int16_t l[3];
	int16_t carrierLevel = 0;
	if (variant == 2) {
		const int32_t divisor = std::max<int32_t>(sum, 0x2000);
		for (int32_t i = 0; i < 3; i++) {
			l[i] = static_cast<int16_t>((level[i] * 0x7FFF) / divisor);
		}
	}
	else {
		const int32_t divisor = std::max<int32_t>(sum + 0x7FFF, 0x2000);
		carrierLevel = static_cast<int16_t>(0x3FFF0001 / divisor);
		for (int32_t i = 0; i < 3; i++) {
			l[i] = static_cast<int16_t>((level[i] * 0x7FFF) / divisor);
		}
	}

	const PostSettings p = postSettings(t.lowPass, t.drive, t.pitch, 0x78, 0x18);

	int32_t buffer[kBlockSize];
	uint32_t p0 = s.phase[0], p1 = s.phase[1], p2 = s.phase[2], pc = s.phase[3];
	switch (variant) {
	case 0:
		for (int32_t i = 0; i < kBlockSize; i++) {
			p0 += inc[0];
			p1 += inc[1];
			p2 += inc[2];
			pc += carrierInc;
			const int32_t a = sine(p0), b = sine(p1), c = sine(p2);
			const uint32_t pm = pc + wrap(depth[2], c) + wrap(depth[1], b) + wrap(depth[0], a);
			buffer[i] =
			    static_cast<int32_t>(wrap(c, l[2]) + wrap(a, l[0]) + wrap(b, l[1]) + wrap(sine(pm), carrierLevel));
		}
		break;
	case 1:
		for (int32_t i = 0; i < kBlockSize; i++) {
			p0 += inc[0];
			p1 += inc[1];
			p2 += inc[2];
			pc += carrierInc;
			const int32_t a = sine(p0);
			const int32_t b = sine(p1 + wrap(depth[0], a));
			const int32_t c = sine(p2 + wrap(depth[1], b));
			buffer[i] = static_cast<int32_t>(wrap(sine(pc + wrap(depth[2], c)), carrierLevel) + wrap(c, l[2])
			                                 + wrap(a, l[0]) + wrap(b, l[1]));
		}
		break;
	default: {
		int32_t feedback = s.feedback;
		for (int32_t i = 0; i < kBlockSize; i++) {
			p0 += inc[0];
			p1 += inc[1];
			p2 += inc[2];
			const int32_t a = sine(p0 + wrap(depth[2], feedback));
			const int32_t b = sine(p1 + wrap(depth[0], a));
			const int32_t c = sine(p2 + wrap(depth[1], b));
			feedback = c;
			buffer[i] = static_cast<int32_t>(wrap(l[2], c) + wrap(a, l[0]) + wrap(b, l[1]));
		}
		s.feedback = feedback;
		pc += carrierInc * static_cast<uint32_t>(kBlockSize);
		break;
	}
	}
	s.phase[0] = p0;
	s.phase[1] = p1;
	s.phase[2] = p2;
	s.phase[3] = pc;

	post(p, buffer, s.lowPass);
	output(buffer, mul(envelope, declick), mul(newEnvelope, newDeclick), t.punch != 0, out);

	if (t.decay < kKnobMax && s.envelope < 0x8000 && !trigger && !s.held) {
		s.active = 0;
	}
}

// IIA -----------------------------------------------------------------------------------------------------------------

namespace {

/// A partial's phase increment: its ratio (16.16, interpolated across Weight) times the note's increment
uint32_t partialIncrement(const int32_t* row, int32_t weightFraction, uint32_t baseInc) {
	const int32_t r0 = row[0];
	const int32_t ratio = static_cast<int32_t>((static_cast<int64_t>(row[1] - r0) * weightFraction) >> 15) + r0;
	return static_cast<uint32_t>((static_cast<uint64_t>(static_cast<uint32_t>(ratio)) * baseInc) >> 16);
}

/// Triangle from a phase that has been offset by a quarter cycle: -0x8000 to 0x7FFF
int32_t triangleOf(uint32_t u) {
	if (u & 0x80000000u) {
		u = ~u;
	}
	return static_cast<int32_t>(u >> 15) - 0x8000;
}

/// IIA's shape morph across its three regions: sine to triangle, triangle to square, square to saw. Values are q15.
/// The additive variants truncate the morph term to 16 bits on the way, like the original (Truncate = true).
template <bool Truncate>
int32_t addMorph(int32_t base, int32_t term) {
	if constexpr (Truncate) {
		return static_cast<int16_t>(static_cast<int16_t>(term) + static_cast<int16_t>(base));
	}
	return term + base;
}

template <bool Truncate>
int32_t morphSine(uint32_t phase, int32_t morph) {
	const int32_t s = sine(phase);
	const int32_t tri = triangleOf(phase + 0x40000000u);
	return addMorph<Truncate>(s, (morph * (tri - s)) >> 15);
}

template <bool Truncate>
int32_t morphSquare(uint32_t phase, int32_t morph) {
	const int32_t tri = triangleOf(phase + 0x40000000u);
	const int32_t square = (static_cast<int32_t>(phase) >= 0) ? 0x7FFF : -0x7FFF;
	return addMorph<Truncate>(tri, (morph * (square - tri)) >> 15);
}

template <bool Truncate>
int32_t morphSaw(uint32_t phase, int32_t morph) {
	const int32_t saw = static_cast<int16_t>(phase >> 16);
	if (static_cast<int32_t>(phase) >= 0) {
		return addMorph<Truncate>(0x7FFF, (morph * (saw - 0x7FFF)) >> 15);
	}
	return addMorph<Truncate>(-0x7FFF, (morph * (saw + 0x7FFF)) >> 15);
}

template <bool Truncate>
int32_t shapeAt(int32_t region, uint32_t phase, int32_t morph) {
	switch (region) {
	case 0:
		return morphSine<Truncate>(phase, morph);
	case 1:
		return morphSquare<Truncate>(phase, morph);
	default:
		return morphSaw<Truncate>(phase, morph);
	}
}

/// x - x^3 / 3-ish, IIA's saturator curve after its input scaling
int32_t iiaCurve(int32_t y) {
	int32_t c = mul(y, y);
	c = mul(static_cast<int32_t>(static_cast<uint32_t>(c) << 3), y);
	c = mul(static_cast<int32_t>(static_cast<uint32_t>(c) << 3), 0x12F684BD);
	return static_cast<int32_t>(static_cast<uint32_t>(sub(y, c)) << 2);
}

} // namespace

void reset(IiaState& s) {
	s = IiaState{};
	s.attack = kMax;
	s.noiseSeed = 0x3C6EF372;
	s.declick = kMax;
}

void render(IiaState& s, const IiaSettings& t, bool trigger, bool fadeOut, int32_t* out) {
	const int32_t variant = (static_cast<uint32_t>(t.variant) > 2) ? 0 : t.variant;

	// The attack setting does double duty: below 0x4000 it sets the strike level, above 0x4400 the attack time
	int32_t attackIndex = t.attack >> 8;
	int32_t strikeLevel = 0;
	bool slowAttack = false;
	if (t.attack < 0) {
		attackIndex = 0;
		strikeLevel = 0x7FFF;
	}
	else if (attackIndex < 0x40) {
		strikeLevel = (static_cast<int16_t>(0x40 - static_cast<int16_t>(t.attack >> 8)) * 0x7FFF) >> 6;
	}
	else if (attackIndex < 0x80) {
		slowAttack = attackIndex > 0x44;
	}
	else {
		attackIndex = 0x7F;
		slowAttack = true;
	}

	if (trigger) {
		std::fill(s.phase, s.phase + 8, 0);
		s.envelope = kMax;
		s.attack = slowAttack ? 0 : kMax;
		s.strike = (variant == 0) ? strikeLevel << 15 : 0;
		s.pitchDrop = kMax;
		s.declick = kMax;
		s.active = 1;
		s.held = t.gate;
		if (t.gate && t.release && !fadeOut) {
			s.held = 0;
		}
	}
	else {
		if (!t.gate || (t.release && !fadeOut)) {
			s.held = 0;
		}
		if (!s.active) {
			std::fill(out, out + kBlockSize, 0);
			return;
		}
	}

	// Envelopes, one step per block
	int32_t index = decayIndex(std::clamp<int32_t>(t.decay, 0, 0x7FFF));
	if (t.punch) {
		index += 0x20;
	}
	const int32_t envelope = s.envelope;
	int32_t newEnvelope = envelope;
	if (!s.held) {
		newEnvelope = mul(envelope, decayCoefficient(index));
	}
	s.envelope = newEnvelope;

	const int32_t attack = s.attack;
	int32_t newAttack = kMax;
	if (attackIndex >= 0x45) {
		// The ramp wraps past full scale, which counts as finished
		newAttack = add(xiaAttackStep[attackIndex - 0x40], attack);
		if (newAttack < 0) {
			newAttack = kMax;
		}
	}
	s.attack = newAttack;

	const int32_t declick = s.declick;
	const int32_t strike = s.strike;
	const int32_t newStrike = mul(strike, xiaDecayCoef[((static_cast<int16_t>(strikeLevel) * 0xC0) >> 15) + 0x155]);
	s.strike = newStrike;
	const int32_t newDeclick = fadeOut ? 0 : kMax;
	s.declick = newDeclick;

	int32_t pitch = t.pitch;
	if (variant == 1) {
		const int32_t drop = s.pitchDrop;
		const int64_t amount = static_cast<int64_t>(static_cast<int16_t>(strikeLevel) * 0x48 + 0xC0000) * drop;
		s.pitchDrop = mul(drop, xiaDecayCoef[374]);
		pitch = add(pitch, static_cast<int32_t>(amount >> 31));
	}
	const uint32_t baseInc = increment(pitch);

	const int32_t weight = std::clamp<int32_t>(t.weight, 0, kKnobMax);
	const int32_t alloy = std::clamp<int32_t>(t.alloy, 0, kKnobMax);
	const int32_t weightFraction = (weight & 0xFF) << 7;
	const int32_t weightStep = weight >> 8;

	// Shape: three morph regions across the knob
	int32_t morph;
	int32_t region;
	if (t.shape >= kKnobMax) {
		region = 3;
		morph = 0x7FFF;
	}
	else {
		const int32_t raw = (t.shape < 0) ? 0 : (t.shape * 0x180) / kKnobMax;
		region = raw >> 7;
		morph = (raw - std::min<int32_t>(region, 2) * 0x80) * 0x100;
	}
	int32_t buffer[kBlockSize]{};
	if (variant == 2) {
		// H: seven sines in a phase modulation chain, modulator > 6 > 5 > ... > 1, into the shaped carrier
		uint32_t inc[8];
		for (int32_t k = 0; k < 8; k++) {
			inc[k] = partialIncrement(&xiaRatioFm[k * 129 + weightStep], weightFraction, baseInc);
		}
		int32_t depth = (static_cast<int16_t>(alloy) * 0x50A) >> 8;
		depth += static_cast<int32_t>((static_cast<int64_t>(depth) * strikeLevel) >> 15);
		depth = mul(depth, std::max<int32_t>(envelope, 0x1FFFFFFF)); // follows the amp decay, but never below a quarter

		int32_t mod[kBlockSize]{};
		if (depth == 0) {
			for (int32_t k = 1; k < 8; k++) {
				s.phase[k] += inc[k] * kBlockSize;
			}
		}
		else {
			uint32_t phase = s.phase[7];
			for (int32_t i = 0; i < kBlockSize; i++) {
				phase += inc[7];
				mod[i] = sine(phase);
			}
			s.phase[7] += inc[7] * kBlockSize;
			for (int32_t k = 6; k >= 1; k--) {
				phase = s.phase[k];
				for (int32_t i = 0; i < kBlockSize; i++) {
					phase += inc[k];
					mod[i] = sine(phase + wrap(depth, mod[i]));
				}
				s.phase[k] += inc[k] * kBlockSize;
			}
		}
		uint32_t phase = s.phase[0];
		for (int32_t i = 0; i < kBlockSize; i++) {
			phase += inc[0];
			buffer[i] = shapeAt<false>(region, phase + wrap(depth, mod[i]), morph) * 0x8000;
		}
		s.phase[0] += inc[0] * kBlockSize;
	}
	else {
		// G and I: six partials added, their levels falling away more slowly as Alloy rises
		int32_t level[6];
		level[0] = 0x7FFF;
		int32_t a = ((((alloy * 0x1400) / kKnobMax) & 0xFFFF) << 5);
		int32_t ramp = 0x731B;
		for (int32_t k = 1; k < 6; k++) {
			level[k] = (static_cast<int16_t>(std::clamp<int32_t>(a, 0, 0x7FFF)) * static_cast<int16_t>(ramp)) >> 15;
			a -= 0x8000;
			ramp -= 0xCE4;
		}
		int32_t sum = 0;
		for (int32_t k = 0; k < 6; k++) {
			sum += level[k];
		}
		for (int32_t k = 0; k < 6; k++) {
			if (level[k] == 0) {
				continue; // a silent partial doesn't even advance its phase
			}
			const uint32_t inc = partialIncrement(&xiaRatioAdditive[k * 129 + weightStep], weightFraction, baseInc);
			if (inc <= 0x7D000000) {
				const int16_t gain = static_cast<int16_t>((level[k] * 0x7FFF) / sum);
				uint32_t phase = s.phase[k];
				for (int32_t i = 0; i < kBlockSize; i++) {
					phase += inc;
					buffer[i] += shapeAt<true>(region, phase, morph) * gain;
				}
			}
			s.phase[k] += inc * kBlockSize;
		}
	}

	// Gains for the block: amp decay x attack x declick, ramped across it
	const int32_t gain = mul(mul(envelope, attack), declick);
	const int32_t gainEnd = mul(mul(newEnvelope, newAttack), newDeclick);

	// The strike: a burst of noise on the attack
	uint32_t seed = s.noiseSeed;
	if (strike != 0) {
		int32_t step = newStrike - strike;
		step = (step < 0) ? (step + 0x1F) >> 5 : step >> 5;
		int32_t level = strike;
		for (int32_t i = 0; i < kBlockSize; i++) {
			if (level != 0) {
				seed = seed * 0x19660Du + 0x3C6EF35Fu;
				const int32_t noise = mul(static_cast<int32_t>(seed) >> 1, level);
				buffer[i] = std::clamp<int32_t>(add(buffer[i], noise), -0x3FFFFFFF, 0x3FFFFFFF);
				level += step;
			}
		}
	}

	// Saturator, driven harder as Weight rises and Alloy falls
	if (t.weight > 0) {
		const int32_t k = (static_cast<int16_t>(static_cast<int16_t>((alloy * alloy) / -0xFE00) + kKnobMax)
		                   * static_cast<int16_t>((weight * 0x700) / kKnobMax))
		                      / kKnobMax
		                  + 0x100;
		const int32_t limit = 0x60000000 / k;
		for (int32_t i = 0; i < kBlockSize; i++) {
			const int32_t x = buffer[i] >> 8;
			int32_t y;
			if (x > limit) {
				y = 0x18000000;
			}
			else if (x >= ~limit) {
				y = ((static_cast<int16_t>(buffer[i] & 0xFF) * static_cast<int16_t>(k)) >> 8) + x * k;
				y = (y < -0x60000000) ? -0x18000000 : (y > 0x60000000) ? 0x18000000 : y >> 2;
			}
			else {
				y = -0x18000000;
			}
			buffer[i] = iiaCurve(y);
		}
	}

	const PostSettings p = postSettings(t.lowPass, t.drive, t.pitch, 0xA8, 0x48);
	post(p, buffer, s.lowPass);
	output(buffer, gain, gainEnd, t.punch != 0, out);

	s.noiseSeed = seed;
	if (s.envelope < 0x8000 && s.strike < 0x8000 && !trigger && !s.held) {
		s.active = 0;
	}
}

} // namespace deluge::dsp::drum_synth::xia
