/*
 * Drum synth voices for the Deluge, ported from the drum, snare, cymbal and hi-hat voices of the LXR-02 firmware.
 *
 * Portions Copyright (C) 2012-2026 Erica Synths (LXR firmware, GPL-3.0-only).
 * Deluge integration Copyright © 2026 the Deluge community.
 * SPDX-License-Identifier: GPL-3.0-only
 *
 * This is a modified version of the LXR firmware drum voices. It is licensed under the GNU General Public License
 * version 3, with the additional terms in the LXR firmware's NOTICE.md: modified versions must be marked as
 * modified, and no rights are granted to the "Erica Synths", "LXR" or "LXR-02" trademarks.
 */

#pragma once

#include "dsp/drum_synth/cyc.h"
#include "dsp/drum_synth/gin.h"
#include "dsp/drum_synth/gin_kits.h"
#include "dsp/drum_synth/xia.h"
#include <algorithm>
#include <array>
#include <climits>
#include <cstdint>

namespace deluge::dsp::drum_synth {

/// The three LXR voice structures, then two engines from the XIA set for the Model:Cycles (see xia.h), then the Cyc
/// engines (see cyc.h), then GIN (see gin.h). New engines go on the end: the number is saved with the sound.
enum class Engine : uint8_t {
	DRUM,  ///< Kick and toms: main osc, FM or mixed mod osc, pitch envelope
	SNARE, ///< Pitched osc with pitch envelope, plus filtered sample-and-hold noise
	METAL, ///< Cymbal and hi-hat: main osc frequency modulated by two more oscs, one usually noise
	TIA,   ///< "DXIA" on the Deluge (XIA TIA): three sines phase modulating a carrier
	IIA,   ///< "DIIA" on the Deluge (XIA IIA): inharmonic partials with a noise strike
	CYC_KIK,
	CYC_SNR,
	CYC_METAL,
	CYC_PERC,
	CYC_TONE,
	CYC_CHORD,
	VTG_SNR,
	CLAP_ENG,
	GIN, ///< The PO-32 style drum voice from SquareWave Studio's GIN for the OP-1
	NUM_ENGINES,
};

/// Whether an engine is one of the XIA engines
constexpr bool isXia(Engine engine) {
	return engine == Engine::TIA || engine == Engine::IIA;
}

/// Whether an engine is one of the Cyc engines
constexpr bool isCyc(Engine engine) {
	return engine >= Engine::CYC_KIK && engine <= Engine::CLAP_ENG;
}

/// Whether an engine is GIN
constexpr bool isGin(Engine engine) {
	return engine == Engine::GIN;
}

/// Whether an engine is one of the three LXR voices
constexpr bool isLxr(Engine engine) {
	return engine <= Engine::METAL;
}

/// The Cyc engine an engine runs on (only meaningful when isCyc())
constexpr cyc::Kind cycKind(Engine engine) {
	return static_cast<cyc::Kind>(static_cast<uint8_t>(engine) - static_cast<uint8_t>(Engine::CYC_KIK));
}

/// Every engine's continuous settings. They live on Deluge patched params that a drum synth sound doesn't otherwise
/// use, so they can be automated, parameter locked and put on gold knobs. Each slot means something per engine: the XIA
/// engines' meanings are below, and the LXR engines' come from lxrControlSlot().
struct DrumControls {
	enum Slot : uint8_t {
		TIA_BLENDEX1 = 0, ///< Operator 1 from heard (0) to modulating (127)
		TIA_BLENDEX2 = 1,
		TIA_BLENDEX3 = 2,
		DRIVE = 3,        ///< Both engines: wavefolder, then a saturator at the top of the range
		ENGINE_DECAY = 4, ///< Both engines: their own decay on top of the Deluge envelope. 127 holds
		TIA_FREQ1 = 5,    ///< Operator 1 tuning, 64 = unison, 0 / 127 = 24 semitones down / up
		TIA_FREQ2 = 6,
		TIA_FREQ3 = 7,
		IIA_WEIGHT = 0, ///< Partial ratios
		IIA_ALLOY = 1,  ///< Partial levels, or modulation depth on H
		IIA_SHAPE = 2,  ///< Partial waveform, sine > triangle > square > saw
		IIA_STRIKE = 5, ///< Level of the noise burst at the start of each hit
		// The Cyc engines: their four knobs, as on the Model:Cycles (Color, Shape, Sweep, Contour), then drive and
		// decay
		CYC_MACRO1 = 0,
		CYC_MACRO2 = 1,
		CYC_MACRO3 = 2,
		CYC_MACRO4 = 3,
		CYC_DRIVE = 4,
		// Drive and decay for the engines without their own (the LXR voices), and the Cyc engines' decay. Added after
		// the LXR settings, on params a drum synth sound has no other use for.
		COMMON_DECAY = 9, ///< 127 holds. On GIN, moves both its decays: 64 leaves them as they are.
		LXR_DRIVE = 10,
		// GIN's continuous settings. Its choices (waves, modes, filter type), EQ and level stay in the Patch.
		GIN_MIX = 0,          ///< Tone against noise: Custom 2
		GIN_NOISE_FREQ = 1,   ///< Noise filter cutoff: Custom 3
		GIN_MOD_RATE = 2,     ///< Pitch modulation speed: mod rate knob
		GIN_MOD_AMOUNT = 3,   ///< Pitch modulation depth, 64 = none: mod depth knob
		GIN_DRIVE = 4,        ///< Shares the Cyc engines' drive slot
		GIN_NOISE_DECAY = 5,  ///< Also spaces the bursts of the modulated noise envelope
		GIN_NOISE_RES = 6,    ///< Noise filter resonance
		GIN_NOISE_ATTACK = 7, ///< Noise envelope attack
		GIN_TONE_ATTACK = 8,
		GIN_TONE_DECAY = 10,
		GIN_EQ_FREQ = 11, ///< Peak EQ frequency. Added after GIN, on a param only a sample oscillator reads.
		NUM_SLOTS = 12,
	};
	/// Raw Deluge param values: the whole int32 range, centre 0
	std::array<int32_t, NUM_SLOTS> values{};

	/// Raw param value to an 8.8 knob value as the Cycles uses: 0 to 0x7F00, with the centre at exactly 0x4000 (64)
	static constexpr int32_t toKnob(int32_t raw) {
		const int64_t scale = (raw < 0) ? 0x4000 : 0x3F00;
		const int64_t offset = (static_cast<int64_t>(raw) * scale + (int64_t{1} << 30)) >> 31;
		return static_cast<int32_t>(0x4000 + offset);
	}
	/// The inverse, for setting a param to a knob value
	static constexpr int32_t fromKnob(int32_t knob) {
		const int64_t d = knob - 0x4000;
		const int64_t raw = (d << 31) / ((d < 0) ? 0x4000 : 0x3F00);
		return static_cast<int32_t>(std::clamp<int64_t>(raw, INT32_MIN, INT32_MAX));
	}
	/// A whole number knob value, 0 to 127
	static constexpr int32_t fromValue(int32_t value) { return fromKnob(value << 8); }
	/// The whole number knob value of a slot, 0 to 127
	[[nodiscard]] uint8_t value(int32_t slot) const {
		return static_cast<uint8_t>(std::clamp<int32_t>((toKnob(values[slot]) + 0x80) >> 8, 0, 127));
	}

	/// The Cycles' own defaults for an engine, as raw param values
	static DrumControls defaultsFor(Engine engine);
};

/// Every engine has a drive and a decay, on the same pads and gold knobs, though they live on different slots: the
/// XIA engines kept theirs where they were first saved
constexpr int32_t driveSlot(Engine engine) {
	return isXia(engine)                      ? DrumControls::DRIVE
	       : (isCyc(engine) || isGin(engine)) ? DrumControls::CYC_DRIVE
	                                          : DrumControls::LXR_DRIVE;
}
constexpr int32_t decaySlot(Engine engine) {
	return isXia(engine) ? DrumControls::ENGINE_DECAY : DrumControls::COMMON_DECAY;
}

/// The standard drum synth layout every engine follows, as far as it can: the shared slots, the same job on every
/// engine, then each engine's own extras. The shortcut pads, the gold knobs and the shift glow all read it. It only
/// arranges the controls: where each one is saved (DrumControls) doesn't change.
enum class StandardSlot : uint8_t {
	DECAY,
	DRIVE,
	PITCH,            ///< The Deluge's own tuning (osc 1 transpose), on every engine
	PITCH_ENV_AMOUNT, ///< The Cyc engines' Sweep
	PITCH_ENV_SPEED,  ///< The Cyc engines' Contour
	CHARACTER_A,      ///< The Cyc engines' Color
	CHARACTER_B,      ///< The Cyc engines' Shape
	NUM_SHARED,
	EXTRA_1 = NUM_SHARED, ///< Extras 1 to 9: each engine's own settings
	NUM_SLOTS = 16,
};

enum class Wave : uint8_t { SINE, TRIANGLE, SAW, SQUARE, NOISE, NUM_WAVES };

enum class NoiseFilter : uint8_t { LOW_PASS, HIGH_PASS, BAND_PASS, OFF, NUM_FILTERS };

/// Starting points for a drum synth row, in the order a new drum synth kit lays them out
enum class Preset : uint8_t { KICK, SNARE, CLOSED_HAT, OPEN_HAT, LOW_TOM, HIGH_TOM, CYMBAL, NUM_PRESETS };

/// The kits SHIFT + KIT makes. Each row of one remembers which it came from (saved with the row), so its audition pad
/// can be tinted and the kit can't be swapped for another once rows have been added to it.
enum class PresetKit : uint8_t { NONE, LXR, CYCLES, GIN, NUM_KITS };

/// How a drum synth row sits in a kit, besides the engine's own settings: the same whether it came from a preset kit
/// or from SYNTH MODE, so an engine picked on any row sounds as it does in its kit. Every engine shapes its own hits
/// with its own decay, inside an open Deluge envelope (attack 0, sustain full) whose release is long enough for that
/// decay to ring out after the note ends: one tap plays the whole hit, gated sounds follow the note.
struct RowSetup {
	int16_t transpose; ///< Kit rows play at middle C, so the kicks and toms drop and the hats rise to their usual range
	int8_t decay;      ///< Envelope 1 decay, as a user value
	int8_t hpf;        ///< High pass cutoff as a user value, -1 = off
	bool choke;        ///< Hats choke each other
	int8_t env2Decay = 5;  ///< Envelope 2 decay, as a user value
	int8_t pitchCable = 0; ///< Envelope 2 to pitch, as a patch cable user value (about 1.9 semitones each). 0 = none
	int8_t sustain = 50;   ///< Envelope 1 sustain and release, as user values
	int8_t release = 40;
	int8_t engineDecay = -1; ///< The LXR voices' own decay (DrumControls::COMMON_DECAY, 0-127), or -1 to leave it
	int8_t cents = 0;        ///< Fine tune, for GIN rows tuned to their sound
};

/// Envelope 1 release (a user value) that lets a sound falling 60 dB in t60 seconds ring out after its note ends: five
/// times that, about 4 dB off untouched by the time the sound is 30 dB down, and never under the presets' 40. Held
/// sounds (t60 of 1000 s or more) are gated by the note and get 40.
int8_t envelopeReleaseFor(float t60);

/// The LXR voices' own decay at a knob of 0-127: 30 ms to 12 s to fall 60 dB, 127 holds (1000 s)
float lxrDecaySeconds(int32_t knob);

/// The LXR kit's rows
RowSetup rowSetupFor(Preset preset);

/// The preset an LXR voice starts from when picked on its own
constexpr Preset presetForLxrEngine(Engine engine) {
	return (engine == Engine::SNARE) ? Preset::SNARE : (engine == Engine::METAL) ? Preset::CYMBAL : Preset::KICK;
}

/// What DXIA and DIIA start as when picked: DXIA's three variants as kicks, DIIA's as noisy percussion. Their settings
/// (slots 0 to 7, 0-127, see DrumControls), Punch, and the row around them. Tuned by measuring renders against Cyc Kik
/// and Cyc Snr: level, pitch drop, how bright the attack and tail are, how long they ring.
struct XiaPreset {
	std::array<uint8_t, 8> slots;
	bool punch;
	RowSetup row;
};
XiaPreset xiaPresetFor(Engine engine, uint8_t variant);

/// Envelope 1 release (a user value, 0 to 50) for a Cyc engine at its own decay knob (see envelopeReleaseFor)
int8_t cycEnvelopeRelease(Engine engine, int32_t decayKnob, bool punch);

/// A GIN kit sound's row: tuned to the sound's pitch, with the release fitted to its longer layer's decay
RowSetup rowSetupForGinSound(const gin::KitSound& sound, bool choke);

/// Any engine, as picked in SYNTH MODE (or a preset kit row), with its default settings. The LXR voices as their kit
/// rows, DXIA and DIIA as their presets. The Cyc engines play at the note, as the Cycles does at its default tune.
RowSetup rowSetupForEngine(Engine engine, uint8_t variant = 0);

/// Whether a row on this engine starts with Snappy decay on. None do: each engine's own decay shapes the hit, and the
/// Deluge envelope only has to stay out of its way.
constexpr bool snappyByDefault(Engine engine) {
	(void)engine;
	return false;
}

/// Transient modes. Values from kTransientSampleFirst up pick one of the LXR transient samples.
constexpr uint8_t kTransientSnap = 0;
constexpr uint8_t kTransientOffset = 1;
constexpr uint8_t kTransientSampleFirst = 2;
constexpr uint8_t kNumTransientSamples = 12;
constexpr uint8_t kNumTransientModes = kTransientSampleFirst + kNumTransientSamples;

/// The editable settings of one drum synth voice. Everything is stored as a small integer, mostly 0-127 like the
/// LXR stores its kits, so a whole patch saves as a short run of hex bytes. Settings an engine doesn't use are kept,
/// so switching engine and back loses nothing.
class Patch {
public:
	enum Param : uint8_t {
		ENGINE,            ///< Engine
		OSC_WAVE,          ///< Wave of the main osc
		PITCH_AMOUNT,      ///< Pitch envelope depth, up to 32x the base frequency (drum, snare)
		PITCH_DECAY,       ///< Pitch envelope decay time (drum, snare)
		PITCH_SLOPE,       ///< Pitch envelope curve, 64 = linear, lower = falls away faster (drum, snare)
		MIX_MODE,          ///< 0 = mod osc frequency modulates the main osc, 1 = the two are mixed (drum)
		FM_AMOUNT,         ///< FM depth, or mod osc level in mix mode (drum, metal)
		MOD_WAVE,          ///< Wave of the mod osc (drum, metal)
		MOD_COARSE,        ///< Mod osc semitones above the main osc, 64 = unison (drum, metal)
		MOD_FINE,          ///< Mod osc detune, 64 = none, 0 / 127 = a semitone down / up (drum, metal)
		FM2_AMOUNT,        ///< Second mod osc FM depth (metal)
		MOD2_WAVE,         ///< Wave of the second mod osc (metal)
		MOD2_COARSE,       ///< Second mod osc semitones above the main osc (metal)
		MOD2_FINE,         ///< Second mod osc detune (metal)
		NOISE_MIX,         ///< Noise level against the osc (snare)
		NOISE_PITCH,       ///< Noise sample-and-hold rate, 127 = 22 kHz (snare)
		NOISE_FILTER_TYPE, ///< NoiseFilter (snare)
		NOISE_CUTOFF,      ///< Noise filter cutoff, 30 Hz to 16 kHz (snare)
		NOISE_RESONANCE,   ///< Noise filter resonance (snare)
		TRANS_MODE,        ///< kTransientSnap, kTransientOffset, or a transient sample
		TRANS_VOLUME,      ///< Transient level
		TRANS_PITCH,       ///< Transient speed
		// XIA engines. Their continuous settings are on Deluge params: see DrumControls.
		XIA_VARIANT,      ///< The Cycles variant: TIA L, B, C or IIA G, I, H
		XIA_PUNCH,        ///< 1 = a soft clipper on the output, and a shorter engine decay. Also the Cyc engines'
		                  ///< Punch
		SNAPPY_DECAY,     ///< 1 = the Deluge amp envelope's decay and release fall away faster (any engine). On by
		                  ///< default.
		CONTROLS_VERSION, ///< 1 once the drive and decay controls exist. Sounds saved before have no byte for it.
		// GIN. Its continuous settings are on Deluge params: see DrumControls. The noise filter is NOISE_FILTER_TYPE.
		// The EQ frequency has moved to DrumControls::GIN_EQ_FREQ: this byte is only read from sounds saved before.
		GIN_WAVE,       ///< gin::Wave
		GIN_MOD_MODE,   ///< gin::ModMode
		GIN_NOISE_ENV,  ///< gin::NoiseEnv
		GIN_EQ_FREQ,    ///< Peak EQ frequency, 20 Hz to 20 kHz
		GIN_EQ_GAIN,    ///< Peak EQ gain, 64 = flat, 0 / 127 = 40 dB cut / boost
		GIN_LEVEL,      ///< GIN's level before its clipper. 102 is about -2 dB.
		GIN_EQ_CONTROL, ///< 1 once GIN's EQ frequency is a control. Sounds saved before have no byte for it.
		NUM_PARAMS,
	};

	/// A new drum synth starts on a kick, with Snappy decay on. Loading a preset afterwards keeps the Snappy setting.
	Patch() {
		setPreset(Preset::KICK);
		params_[SNAPPY_DECAY] = 1;
	}

	void setPreset(Preset preset);
	/// Switch to an XIA engine and variant. Its continuous settings are on Deluge params: see DrumControls.
	void setXiaPreset(Engine engine, uint8_t variant);
	/// Switch to a Cyc engine. Its continuous settings are on Deluge params: see DrumControls.
	void setCycPreset(Engine engine);
	/// Switch to GIN, with a kit sound's choices, EQ and level. Its continuous settings are on Deluge params: see
	/// ginControlsFor().
	void setGinSound(const gin::KitSound& sound);

	[[nodiscard]] uint8_t get(Param p) const { return params_[p]; }
	void set(Param p, uint8_t value);
	[[nodiscard]] Engine engine() const { return static_cast<Engine>(params_[ENGINE]); }
	[[nodiscard]] static uint8_t maxValue(Param p);
	/// Whether an engine reads this setting, for hiding the ones that do nothing
	[[nodiscard]] static bool isUsedBy(Param p, Engine engine);

	/// Raw bytes, for saving and loading. Out of range values are clamped when read.
	std::array<uint8_t, NUM_PARAMS>& bytes() { return params_; }
	void clampAll();

	/// Load the 12 bytes saved by the first, kick-and-tom-only version of the drum synth
	void readLegacyDrumBytes(const uint8_t* bytes, int32_t length);

	/// Called after reading a saved patch of length bytes. A sound saved before the drive and decay controls existed
	/// has whatever its params held on their slots, and one saved before GIN's EQ frequency became a control has it in
	/// the patch: needsControlDefaults() says to sort them out (savedBeforeControls(), savedBeforeGinEqControl()).
	void loadedBytes(int32_t length) {
		savedBeforeControls_ = length <= CONTROLS_VERSION;
		savedBeforeGinEqControl_ = length <= GIN_EQ_CONTROL;
		params_[CONTROLS_VERSION] = 1;
		params_[GIN_EQ_CONTROL] = 1;
	}
	[[nodiscard]] bool needsControlDefaults() const { return savedBeforeControls_ || savedBeforeGinEqControl_; }
	[[nodiscard]] bool savedBeforeControls() const { return savedBeforeControls_; }
	[[nodiscard]] bool savedBeforeGinEqControl() const { return savedBeforeGinEqControl_; }
	void clearNeedsControlDefaults() { savedBeforeControls_ = savedBeforeGinEqControl_ = false; }

private:
	std::array<uint8_t, NUM_PARAMS> params_{};
	bool savedBeforeControls_ = false;     ///< Not saved
	bool savedBeforeGinEqControl_ = false; ///< Not saved
};

/// The control slot an LXR setting lives on, or -1 for the choices (waves, modes, filter type), which stay in the
/// Patch. The slots are shared between the LXR engines: each engine uses its own settings' slots.
int32_t lxrControlSlot(Patch::Param param);

/// The control (DrumControls slot) an engine has on a shared standard slot, or -1 where it has none (and always for
/// PITCH, which is the Deluge's tuning). LXR Metal, DXIA and DIIA have no pitch envelope.
///
/// The Cyc engines put Sweep and Contour on the pitch envelope's slots on every machine, though they don't sweep pitch
/// on all of them. Sweep / Contour per machine: Kik pitch sweep / sweep length (and the knock), Snr pitch sweep /
/// rattle noise decay, Metal pitch sweep / top end decay, Perc pitch sweep / overtone decay and sweep length, Tone
/// index burst / index fade, Chord wave shape / wave decay, Vtg Snr pitch sweep / body FM depth, Clap spacing crunch /
/// tail decay.
int32_t sharedControlSlot(Engine engine, StandardSlot slot);

/// The controls a patch starts from: its LXR settings, or an XIA engine's defaults
DrumControls controlsFromPatch(const Patch& patch);

/// A GIN kit sound's continuous settings, as controls
DrumControls ginControlsFor(const gin::KitSound& sound);

/// Copy the LXR settings back from the controls into the patch, so switching engine and back keeps them
void storeControlsInPatch(const DrumControls& controls, Patch& patch);

/// The running state of one drum synth voice. Small and allocation-free, so every Deluge voice can carry one.
class Voice {
public:
	/// Starts a note. It begins on the next render, which brings the controls.
	void trigger(const Patch& patch);

	/// Adds numSamples of output into buffer. basePhaseIncrement is the Deluge's pitch for this note, before the
	/// drum's pitch envelope. amplitude / amplitudeIncrement are the source's gain ramp, as for the other oscillators.
	void render(const Patch& patch, const DrumControls& controls, int32_t* buffer, int32_t numSamples,
	            uint32_t basePhaseIncrement, int32_t amplitude, int32_t amplitudeIncrement);

	/// True once an XIA or Cyc engine's own envelope has finished, so the Deluge voice can be freed
	[[nodiscard]] bool finished() const { return xiaFinished_; }

private:
	/// Patch values after knob offsets, turned into the numbers the render loop wants
	struct Settings {
		Engine engine;
		Wave oscWave, modWave, mod2Wave;
		bool mixMode;
		float fmAmount, fm2Amount;
		float modRatio, mod2Ratio;
		float pitchDecayPerTick, pitchSlope, pitchAmount;
		float noiseMix;
		uint32_t noisePhaseIncrement;
		NoiseFilter noiseFilter;
		float filterG, filterK;
		uint8_t transientMode;
		float transientVolume, transientPitch;
		uint32_t transientPhaseIncrement;
	};

	struct Osc {
		uint32_t phase = 0;
		uint32_t phaseIncrement = 0;
		int32_t held = 0; ///< Current sample-and-hold value, for noise
	};

	void renderXia(const std::array<uint8_t, Patch::NUM_PARAMS>& p, const DrumControls& controls, int32_t* buffer,
	               int32_t numSamples, uint32_t basePhaseIncrement, int32_t amplitude, int32_t amplitudeIncrement);
	void startXia(Engine engine);
	void renderLxrDriven(int32_t* buffer, int32_t numSamples, uint32_t basePhaseIncrement, int32_t amplitude,
	                     int32_t amplitudeIncrement, float driveKnob, int32_t decayKnob);
	void renderCyc(Engine engine, const std::array<uint8_t, Patch::NUM_PARAMS>& p, const DrumControls& controls,
	               int32_t* buffer, int32_t numSamples, uint32_t basePhaseIncrement, int32_t amplitude,
	               int32_t amplitudeIncrement);
	void renderGin(const std::array<uint8_t, Patch::NUM_PARAMS>& p, const DrumControls& controls, int32_t* buffer,
	               int32_t numSamples, uint32_t basePhaseIncrement, int32_t amplitude, int32_t amplitudeIncrement);

	void updateSettings(const Patch& patch, const DrumControls& controls);
	void startLxr();
	void controlTick(uint32_t basePhaseIncrement);
	int32_t step(Wave w, Osc& osc, uint32_t phaseOffset = 0);
	int32_t random();
	int32_t filterNoise(int32_t input);

	void renderDrum(int32_t* out, int32_t n, int32_t& amplitude, int32_t amplitudeIncrement);
	void renderSnare(int32_t* out, int32_t n, int32_t& amplitude, int32_t amplitudeIncrement);
	void renderMetal(int32_t* out, int32_t n, int32_t& amplitude, int32_t amplitudeIncrement);
	int32_t transientSample();

	Settings settings_{};
	std::array<uint8_t, Patch::NUM_PARAMS> settingsFrom_{}; ///< Effective bytes settings_ was built from
	bool settingsValid_ = false;

	Osc main_, mod_, mod2_, noise_;
	uint32_t transientPhase_ = 0;
	int32_t fmDepth_ = 0;        ///< q31, FM index / 8
	int32_t mainLevel_ = 0;      ///< q31
	int32_t modLevel_ = 0;       ///< q31
	int32_t mod2Level_ = 0;      ///< q31
	int32_t transientLevel_ = 0; ///< q31
	float pitchEnvelope_ = 0;
	float snapEnvelope_ = 0;
	float filterState1_ = 0;
	float filterState2_ = 0;
	uint32_t random_ = 0x12345678;
	int32_t samplesUntilTick_ = 0;

	// XIA engines render whole blocks; samples left over from the last one wait here. The Cyc engines' and GIN's state
	// share the memory, as only one engine plays at a time.
	union {
		xia::TiaState tia;
		xia::IiaState iia;
		cyc::Voice cyc;
		gin::Voice gin;
	} xia_{};
	std::array<int32_t, xia::kBlockSize> xiaBlock_{};
	int32_t xiaBlockPos_ = xia::kBlockSize;
	Engine xiaEngine_ = Engine::DRUM; ///< The XIA engine xia_ holds, or DRUM for none
	bool cycReady_ = false;           ///< xia_ holds a Cyc engine's state
	bool ginReady_ = false;           ///< xia_ holds GIN's state
	float lxrLevel_ = 1.f;            ///< The LXR voices' decay
	bool xiaTrigger_ = false;
	bool xiaFinished_ = false;
	bool pendingTrigger_ = false;
};

} // namespace deluge::dsp::drum_synth
