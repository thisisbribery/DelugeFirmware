/*
 * Copyright © 2014-2023 Synthstrom Audible Limited
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

#include "definitions_cxx.hpp"
#include "dsp/tb303/open303.h"
#include "model/mod_controllable/mod_controllable_audio.h"
#include "model/sample/sample_recorder.h"
#include "model/voiced.h"
#include "modulation/arpeggiator.h"
#include "modulation/knob.h"
#include "modulation/lfo.h"
#include "modulation/params/param.h"
#include "modulation/params/param_manager.h"
#include "modulation/params/param_set.h"
#include "modulation/patch/patcher.h"
#include "modulation/sidechain/sidechain.h"
#include "processing/engines/audio_engine.h"
#include "processing/source.h"
#include "util/misc.h"
#include <bitset>
#include <memory>

struct CableGroup;
class StereoSample;
class Voice;
class Kit;
class ParamManagerForTimeline;
class TimelineCounter;
class Clip;
class GlobalEffectableForClip;
class ModelStackWithThreeMainThings;
class ModelStackWithSoundFlags;
class ModelStackWithModControllable;

#define PARAM_LPF_OFF (-1)

struct ParamLPF {
	int32_t p; // PARAM_LPF_OFF means none
	int32_t currentValue;
};

#define NUM_MOD_SOURCE_SELECTION_BUTTONS 2

/*
 * Sound can be either an Instrument or a Drum, in the form of SoundInstrument or SoundDrum respectively.
 * These classes are implemented using “multiple inheritance”, which is sacrilegious to many C++ programmers.
 * I (Rohan) consider it to be a more or less appropriate solution in this case and a few others in the Deluge codebase
 * where it’s used. It’s a little while though since I’ve sat and thought about what the alternatives could be and
 * whether anything else would be appropriate.
 *
 * Anyway, Sound (which may be named a bit too broadly) basically means a synth or sample, or any combination of the
 * two. And, to reiterate the above, it can exist as a “synth” as the melodic Output of one entire Clip(s), or as just a
 * Drum - one of the many items in a Kit, normally associated with a row of notes.
 */

/// The default volume every Sound gets from Sound::initParams(), as a user value (0-50).
constexpr int32_t kDefaultSoundVolumeUserValue = 40;
/// The depth of the VELOCITY -> LOCAL_VOLUME patch cable that Sound::setupAsSample() installs, as a user value.
constexpr int32_t kSampleDrumVelocityCableUserValue = 50;

// 303 mode's knobs. Cutoff and resonance are the LPF's; the rest sit on the FM operators' params, which 303 mode
// doesn't otherwise use, and take their grid shortcuts.
constexpr int32_t kTb303EnvModParam = deluge::modulation::params::LOCAL_MODULATOR_0_VOLUME;
constexpr int32_t kTb303DecayParam = deluge::modulation::params::LOCAL_MODULATOR_1_VOLUME;
constexpr int32_t kTb303AccentParam = deluge::modulation::params::LOCAL_CARRIER_0_FEEDBACK;
constexpr int32_t kTb303WaveformParam = deluge::modulation::params::LOCAL_CARRIER_1_FEEDBACK;
constexpr int32_t kTb303SlideParam = deluge::modulation::params::LOCAL_MODULATOR_0_FEEDBACK;
// Added in v1.11. Drive starts at 0 on the param's default (its minimum); amp decay at the knob's centre, Open303's own
// 1230 ms, which an osc 1 wavetable position left at its default gives older 303 sounds.
constexpr int32_t kTb303DriveParam = deluge::modulation::params::LOCAL_MODULATOR_1_FEEDBACK;
constexpr int32_t kTb303AmpDecayParam = deluge::modulation::params::LOCAL_OSC_A_WAVE_INDEX;
// Ranges, in Hz and ms. Cutoff and decay are the 303 panel's.
constexpr float kTb303CutoffMin = 314.f;
constexpr float kTb303CutoffMax = 2394.f;
constexpr float kTb303DecayMin = 200.f;
constexpr float kTb303DecayMax = 2000.f;
constexpr float kTb303SlideMin = 10.f;
constexpr float kTb303SlideMax = 300.f;
constexpr float kTb303AmpDecayMin = 60.f;
constexpr float kTb303AmpDecayMid = 1230.f;

class Sound : public ModControllableAudio, public virtual Voiced {
public:
	using ActiveVoice = AudioEngine::VoicePool::pointer_type;

	Sound();
	~Sound() override { std::erase(AudioEngine::sounds, this); }

	Patcher patcher;

	ParamLPF paramLPF{};

	Source sources[kNumSources];

	// This is for the *global* params only, and begins with FIRST_GLOBAL_PARAM, so subtract that from your p value
	// before accessing this array!
	std::array<int32_t, deluge::modulation::params::kNumParams - deluge::modulation::params::FIRST_GLOBAL>
	    paramFinalValues{};
	std::array<int32_t, util::to_underlying(kFirstLocalSource)> globalSourceValues{};

	uint32_t sourcesChanged{}; // Applies from first source up to FIRST_UNCHANGEABLE_SOURCE

	LFO globalLFO1;
	LFO globalLFO3;
	LFOConfig lfoConfig[LFO_COUNT];

	bool invertReversed{}; // Used by the arpeggiator to invert the reverse flag just for the current voice

	// December 3, 2024
	// @todo
	// Commit 6534979986d465a0acf01018e066e1d7ca1d170e
	// From PR #2004 (LFO: cleanups in preparation for LFO2 sync support)
	// (https://github.com/SynthstromAudible/DelugeFirmware/pull/2004)
	// Removed four uint8_t lfo variables from this section and replaced them with the LFOConfig
	// definition above. This unknowingly introduced a "release" bug which changed the sound of the
	// Deluge synth engine compared to 1.1 as reported in issue #2660:
	// (https://github.com/SynthstromAudible/DelugeFirmware/issues/2660)
	// As a solution modKnobs is aligned to 8 bytes. I don't know why this fixes the problem but it seems to work even
	// with other changes to the class. We think the issue relates to the use of "offsetof" in the
	// param and patcher system (related to the paramFinalValues / globalSourceValues definitions above)

	alignas(8) ModKnob modKnobs[kNumModButtons][kNumPhysicalModKnobs];

	int32_t sideChainSendLevel = 0;

	PolyphonyMode polyphonic = PolyphonyMode::POLY;
	uint8_t maxVoiceCount = 8;

	int16_t transpose = 0;
	int8_t cents = 0;
	PhaseIncrementFineTuner fineTuner;

	uint8_t numUnison = 1;
	int8_t unisonDetune = 8;
	uint8_t unisonStereoSpread = 0;

	// For sending MIDI notes for SoundDrums
	uint8_t outputMidiChannel = MIDI_CHANNEL_NONE;
	uint8_t outputMidiNoteForDrum = MIDI_NOTE_NONE;

	int16_t modulatorTranspose[kNumModulators] = {0, -12};
	int8_t modulatorCents[kNumModulators] = {0, 0};

	PhaseIncrementFineTuner modulatorTransposers[kNumModulators];

	PhaseIncrementFineTuner unisonDetuners[kMaxNumVoicesUnison];
	int32_t unisonPan[kMaxNumVoicesUnison]{};

	SynthMode synthMode = SynthMode::SUBTRACTIVE;
	bool modulator1ToModulator0 = false;

	int32_t volumeNeutralValueForUnison{0};

	int32_t lastNoteCode = std::numeric_limits<int32_t>::min();

	bool oscillatorSync = false;

	VoicePriority voicePriority = VoicePriority::MEDIUM;

	bool skippingRendering = true;

	std::bitset<kNumExpressionDimensions> expressionSourcesChangedAtSynthLevel{0};

	// I really didn't want to store these here, since they're stored in the ParamManager, but.... complications! Always
	// 0 for Drums - that was part of the problem - a Drum's main ParamManager's expression data has been sent to the
	// "polyphonic" bit, and we don't want it to get referred to twice. These get manually refreshed in setActiveClip().
	std::array<int32_t, kNumExpressionDimensions> monophonicExpressionValues{};

	std::array<uint32_t, kNumSources> oscRetriggerPhase{}; // 4294967295 means "off"
	std::array<uint32_t, kNumModulators> modulatorRetriggerPhase{};

	uint32_t timeStartedSkippingRenderingModFX{0};
	uint32_t timeStartedSkippingRenderingLFO{0};
	uint32_t timeStartedSkippingRenderingArp{0};
	uint32_t startSkippingRenderingAtTime = 0; // Valid when not 0. Allows a wait-time before render skipping starts,
	                                           // for if mod fx are on

	virtual ArpeggiatorSettings* getArpSettings(InstrumentClip* clip = nullptr) = 0;
	virtual void setSkippingRendering(bool newSkipping);

	ModFXType getModFXType() override;
	char const* getPatchedParamDisplayNameOverride(int32_t p) override;
	bool setModFXType(ModFXType newType) final;

	void patchedParamPresetValueChanged(uint8_t p, ModelStackWithSoundFlags* modelStack, int32_t oldValue,
	                                    int32_t newValue);
	void render(ModelStackWithThreeMainThings* modelStack, std::span<StereoSample> output, int32_t* reverbBuffer,
	            int32_t sideChainHitPending, int32_t reverbAmountAdjust = 134217728,
	            bool shouldLimitDelayFeedback = false, int32_t pitchAdjust = kMaxSampleValue,
	            SampleRecorder* recorder = nullptr);

	void ensureInaccessibleParamPresetValuesWithoutKnobsAreZero(Song* song) final; // Song may be NULL
	void ensureInaccessibleParamPresetValuesWithoutKnobsAreZero(ModelStackWithThreeMainThings* modelStack);
	void ensureInaccessibleParamPresetValuesWithoutKnobsAreZeroWithMinimalDetails(ParamManager* paramManager);
	void ensureParamPresetValueWithoutKnobIsZero(ModelStackWithAutoParam* modelStack);
	void ensureParamPresetValueWithoutKnobIsZeroWithMinimalDetails(ParamManager* paramManager, int32_t p);

	PatchCableAcceptance maySourcePatchToParam(PatchSource s, uint8_t p, ParamManager* paramManager);

	void resyncGlobalLFOs();

	int8_t getKnobPos(uint8_t p, ParamManagerForTimeline* paramManager, uint32_t timePos, TimelineCounter* counter);
	int32_t getKnobPosBig(int32_t p, ParamManagerForTimeline* paramManager, uint32_t timePos, TimelineCounter* counter);
	bool learnKnob(MIDICable* cable, ParamDescriptor paramDescriptor, uint8_t whichKnob, uint8_t modKnobMode,
	               uint8_t midiChannel, Song* song) final;

	bool hasFilters();

	void sampleZoneChanged(MarkerType markerType, int32_t s, ModelStackWithSoundFlags* modelStack);
	void setNumUnison(int32_t newNum, ModelStackWithSoundFlags* modelStack);
	void setUnisonDetune(int32_t newAmount, ModelStackWithSoundFlags* modelStack);
	void setUnisonStereoSpread(int32_t newAmount);
	void setModulatorTranspose(int32_t m, int32_t value, ModelStackWithSoundFlags* modelStack);
	void setModulatorCents(int32_t m, int32_t value, ModelStackWithSoundFlags* modelStack);
	Error readFromFile(Deserializer& reader, ModelStackWithModControllable* modelStack, int32_t readAutomationUpToPos,
	                   ArpeggiatorSettings* arpSettings);
	void writeToFile(Serializer& writer, bool savingSong, ParamManager* paramManager, ArpeggiatorSettings* arpSettings,
	                 const char* pathAttribute = NULL);

	void voiceUnassigned(ModelStackWithSoundFlags* modelStack);
	bool isSourceActiveCurrently(int32_t s, ParamManagerForTimeline* paramManager);
	bool isSourceActiveEverDisregardingMissingSample(int32_t s, ParamManager* paramManager);
	bool isSourceActiveEver(int32_t s, ParamManager* paramManager);
	bool isNoiseActiveEver(ParamManagerForTimeline* paramManager);
	void noteOn(ModelStackWithThreeMainThings* modelStack, ArpeggiatorBase* arpeggiator, int32_t noteCode,
	            int16_t const* mpeValues, uint32_t sampleSyncLength = 0, int32_t ticksLate = 0,
	            uint32_t samplesLate = 0, int32_t velocity = 64, int32_t fromMIDIChannel = 16);
	void noteOff(ModelStackWithThreeMainThings* modelStack, ArpeggiatorBase* arpeggiator, int32_t noteCode);
	void allNotesOff(ModelStackWithThreeMainThings* modelStack, ArpeggiatorBase* arpeggiator);

	void noteOffPostArpeggiator(ModelStackWithSoundFlags* modelStack, int32_t noteCode = -32768);
	void noteOnPostArpeggiator(ModelStackWithSoundFlags* modelStack, int32_t newNoteCodeBeforeArpeggiation,
	                           int32_t newNoteCodeAfterArpeggiation, int32_t velocity, int16_t const* mpeValues,
	                           uint32_t sampleSyncLength, int32_t ticksLate, uint32_t samplesLate,
	                           int32_t fromMIDIChannel = 16);
	void polyphonicExpressionEventOnChannelOrNote(int32_t newValue, int32_t expressionDimension,
	                                              int32_t channelOrNoteNumber,
	                                              MIDICharacteristic whichCharacteristic) override;

	int16_t getMaxOscTranspose(InstrumentClip* clip);
	int16_t getMinOscTranspose();
	void setSynthMode(SynthMode value, Song* song);
	[[nodiscard]] SynthMode getSynthMode() const { return synthMode; }

	virtual bool isDrum() { return false; }
	void setupAsSample(ParamManagerForTimeline* paramManager);
	/// A kit row on an LXR voice from preset, or, given a Cyc engine, on that engine at the note as the Cycles plays it
	void setupAsDrumSynth(ParamManagerForTimeline* paramManager, deluge::dsp::drum_synth::Preset preset,
	                      deluge::dsp::drum_synth::Engine cycEngine = deluge::dsp::drum_synth::Engine::NUM_ENGINES);
	/// The gold knobs every drum synth engine shares, laid out like the Cycles': attack / release = drive / decay, mod
	/// rate / depth = the sweep's speed and depth, Custom 2 / Custom 3 = two voicing settings. Any other engine value
	/// puts the Deluge's defaults back.
	void setDrumSynthKnobs(deluge::dsp::drum_synth::Engine engine);
	/// The 303's gold knobs, in the drum synth layout: attack / release = drive / amp decay, mod rate / depth = filter
	/// envelope decay / env mod, portamento = slide, Custom 2 / Custom 3 = accent / waveform. false puts back the
	/// Deluge's defaults wherever a 303 knob is.
	void setTb303Knobs(bool tb303);
	void recalculateAllVoicePhaseIncrements(ModelStackWithSoundFlags* modelStack);
	void set_cents(int8_t new_cents);
	Error loadAllAudioFiles(bool mayActuallyReadFiles);
	bool envelopeHasSustainCurrently(int32_t e, ParamManagerForTimeline* paramManager);
	bool envelopeHasSustainEver(int32_t e, ParamManagerForTimeline* paramManager);
	bool renderingOscillatorSyncCurrently(ParamManagerForTimeline* paramManager);
	bool renderingOscillatorSyncEver(ParamManager* paramManager);
	void setupAsBlankSynth(ParamManager* paramManager, bool is_dx = false);
	void setupAsDefaultSynth(ParamManager* paramManager);
	void modButtonAction(uint8_t whichModButton, bool on, ParamManagerForTimeline* paramManager) final;
	bool modEncoderButtonAction(uint8_t whichModEncoder, bool on, ModelStackWithThreeMainThings* modelStack) final;
	static void writeParamsToFile(Serializer& writer, ParamManager* paramManager, bool writeAutomation);
	static void readParamsFromFile(Deserializer& reader, ParamManagerForTimeline* paramManager,
	                               int32_t readAutomationUpToPos);
	static bool readParamTagFromFile(Deserializer& reader, char const* tagName, ParamManagerForTimeline* paramManager,
	                                 int32_t readAutomationUpToPos);
	static void initParams(ParamManager* paramManager);
	static Error createParamManagerForLoading(ParamManagerForTimeline* paramManager);
	int32_t hasAnyTimeStretchSyncing(ParamManagerForTimeline* paramManager, bool getSampleLength = false,
	                                 int32_t note = 0);
	int32_t hasCutOrLoopModeSamples(ParamManagerForTimeline* paramManager, int32_t note, bool* anyLooping = nullptr);
	bool hasCutModeSamples(ParamManagerForTimeline* paramManager);
	bool allowsVeryLateNoteStart(InstrumentClip* clip, ParamManagerForTimeline* paramManager);
	void fastReleaseAllVoices(ModelStackWithSoundFlags* modelStack);
	void recalculatePatchingToParam(uint8_t p, ParamManagerForTimeline* paramManager);
	void doneReadingFromFile();
	virtual void setupPatchingForAllParamManagers(Song* song) {}
	void compensateVolumeForResonance(ModelStackWithThreeMainThings* modelStack);
	// void channelAftertouchReceivedFromInputMIDIChannel(int32_t newValue);
	ModelStackWithAutoParam* getParamFromModEncoder(int32_t whichModEncoder, ModelStackWithThreeMainThings* modelStack,
	                                                bool allowCreation = true) final;
	void reassessRenderSkippingStatus(ModelStackWithSoundFlags* modelStack, bool shouldJustCutModFX = false);
	void getThingWithMostReverb(Sound** soundWithMostReverb, ParamManager** paramManagerWithMostReverb,
	                            GlobalEffectableForClip** globalEffectableWithMostReverb,
	                            int32_t* highestReverbAmountFound, ParamManagerForTimeline* paramManager);
	virtual bool readTagFromFile(Deserializer& reader, char const* tagName) = 0;
	void detachSourcesFromAudioFiles();

	// Yup, inlining this helped a tiny bit.
	[[gnu::always_inline]] int32_t getSmoothedPatchedParamValue(int32_t p, ParamManager& paramManager) const {
		if (paramLPF.p == p) {
			return paramLPF.currentValue;
		}
		return paramManager.getPatchedParamSet()->getValue(p);
	}

	void notifyValueChangeViaLPF(int32_t p, bool shouldDoParamLPF, ModelStackWithThreeMainThings const* modelStack,
	                             int32_t oldValue, int32_t newValue, bool fromAutomation);
	void deleteMultiRange(int32_t s, int32_t r);
	void wontBeRenderedForAWhile() override;
	ModelStackWithAutoParam* getParamFromMIDIKnob(MIDIKnob& knob, ModelStackWithThreeMainThings* modelStack) final;
	virtual ArpeggiatorBase* getArp() = 0;
	void possiblySetupDefaultExpressionPatching(ParamManager* paramManager);
	/// A drum synth sound saved before its drive and decay controls existed gets them at neutral
	void resetOldDrumSynthControls(ParamManager* paramManager);

	int32_t getShiftAmountForSaturation() const {
		int32_t legacy = getLegacyClippingAmount();
		return (legacy >= 2) ? (legacy - 2) : 0;
	}

	///	Classic saturation only. Drive must be greater than 0! Check before calling
	/// Shift amount is givben by getShiftAmountForSaturation
	[[gnu::always_inline]] void saturate(int32_t* data, uint32_t* workingValue, int32_t shiftAmount) {
		// Clipping
		//*data = getTanHUnknown(*data, 5 + clippingAmount) << (shiftAmount);
		*data = getTanHAntialiased(*data, workingValue, 5 + getLegacyClippingAmount()) << (shiftAmount);
	}
	uint32_t getSyncedLFOPhaseIncrement(const LFOConfig& config);

	/// @brief Does this sound have any active voices? A 303 counts while it sounds.
	[[nodiscard]] bool hasActiveVoices() const override { return !voices_.empty() || tb303Sounding(); }

	// 303 mode: one mono Open303 voice, played from this sound's knobs, in place of the Deluge's voices

	[[nodiscard]] bool isTb303() const override { return synthMode == SynthMode::TB303; }
	[[nodiscard]] bool tb303Sounding() const { return tb303_ && tb303_->isSounding(); }
	[[nodiscard]] bool tb303NoteHeld(int32_t noteCode) const { return tb303_ && tb303_->isNoteHeld(noteCode); }
	/// Makes the 303 voice and its wavetables. Call from outside the audio routine. False if out of memory.
	bool ensureTb303();
	/// Sets this sound's 303 knobs, and the gold knobs, to a starting acid sound
	void setupTb303Defaults(ParamManager* paramManager);
	bool tb303ClassicFilter = true; // The TB-303 ladder model, or Open303's plain 18 dB lowpass

	/// @brief Get the number of active voices
	[[nodiscard]] size_t numActiveVoices() const { return voices_.size(); }

	/// @brief Immediately ends all active voices
	void killAllVoices() override;

	/// @brief Get the voice with the lowest priority
	/// @return The voice with the lowest priority
	[[nodiscard]] const ActiveVoice& getLowestPriorityVoice() const;

	/// @brief Get the voices for this sound
	[[nodiscard]] const deluge::fast_vector<ActiveVoice>& voices() const { return voices_; }

	/// @brief Releases a given voice from the Sound
	/// @param voice The voice to release
	/// @param modelStack The model stack to use for the release
	/// @param erase Whether to erase the voice from the list of active voices (default: true)
	void freeActiveVoice(const ActiveVoice& voice, ModelStackWithSoundFlags* modelStack = nullptr, bool erase = true);

	// Voiced overrides
	bool anyNoteIsOn() override;
	bool allowNoteTails(ModelStackWithSoundFlags* modelStack, bool disregardSampleLoop = false) override;
	void prepareForHibernation() override;
	void process_postarp_notes(ModelStackWithSoundFlags* modelStackWithSoundFlags, ArpeggiatorSettings* arpSettings,
	                           ArpReturnInstruction instruction);

	virtual const char* getName() { return nullptr; }

private:
	uint32_t getGlobalLFOPhaseIncrement(LFO_ID lfoId, deluge::modulation::params::Global param);
	void recalculateFineTuner();
	void recalculateModulatorTransposer(uint8_t m, ModelStackWithSoundFlags* modelStack);
	void setupUnisonDetuners(ModelStackWithSoundFlags* modelStack);
	void setupUnisonStereoSpread();
	void calculateEffectiveVolume();
	void ensureKnobReferencesCorrectVolume(Knob& knob);
	Error readTagFromFileOrError(Deserializer& reader, char const* tagName, ParamManagerForTimeline* paramManager,
	                             int32_t readAutomationUpToPos, ArpeggiatorSettings* arpSettings, Song* song);

	void writeSourceToFile(Serializer& writer, int32_t s, char const* tagName);
	Error readSourceFromFile(Deserializer& reader, int32_t s, ParamManagerForTimeline* paramManager,
	                         int32_t readAutomationUpToPos);
	void stopSkippingRendering(ArpeggiatorSettings* arpSettings);
	void startSkippingRendering(ModelStackWithSoundFlags* modelStack);
	void getArpBackInTimeAfterSkippingRendering(ArpeggiatorSettings* arpSettings);
	void doParamLPF(int32_t numSamples, ModelStackWithSoundFlags* modelStack);
	void stopParamLPF(ModelStackWithSoundFlags* modelStack);

	bool renderingVoicesInStereo(ModelStackWithSoundFlags* modelStack);
	void setupDefaultExpressionPatching(ParamManager* paramManager);
	void pushSwitchActionOnEncoderForParam(int32_t p, bool on, ModelStackWithThreeMainThings* modelStack);
	ModelStackWithAutoParam* getParamFromModEncoderDeeper(int32_t whichModEncoder,
	                                                      ModelStackWithThreeMainThings* modelStack,
	                                                      bool allowCreation = true);

	/// @brief the list of active voices for this sound
	// O(n) lookup is fine when most of the time we're iterating over all voices anyways.
	// Went with a vector instead of a list so that we don't need to always allocate if
	// we're constantly cycling between a minimum and maximum number of voices.
	deluge::fast_vector<ActiveVoice> voices_;

	std::unique_ptr<deluge::dsp::tb303::Open303> tb303_;
	// The 303's saturation state, as each voice keeps its own
	deluge::dsp::Saturator::Channel tb303Saturation_{};
	uint32_t tb303ClassicSaturation_ = 2147483648u;
	void renderTb303(ParamManagerForTimeline* paramManager, std::span<q31_t> buffer);

	/// @brief Acquire a voice for use by a note
	/// Internally this will either acquire a new voice or steal an existing one
	/// @return The acquired voice
	const ActiveVoice& acquireVoice() noexcept(false); // throws an exception if no memory is available

	/// @brief Check if a voice exists in the list of active voices
	void checkVoiceExists(const ActiveVoice& voice, const char* error) const;

	/// @brief Steals a currently-in-use voice for use by another note
	/// @return The stolen voice
	const ActiveVoice& stealOneActiveVoice();

	/// @brief Force a voice to release very quickly - will be almost instant but click-free
	void terminateOneActiveVoice();

	/// @brief Force a voice to release, or speed up its release if it's already releasing
	void forceReleaseOneActiveVoice();
};
