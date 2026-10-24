#include "CppUTest/TestHarness.h"
#include "dsp/drum_synth/drum_synth.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>

using namespace deluge::dsp::drum_synth;

namespace {

constexpr int32_t kSampleRate = 44100;
constexpr int32_t kAmplitude = 1 << 27;

constexpr uint32_t phaseIncrementFor(double hz) {
	return static_cast<uint32_t>(hz / kSampleRate * 4294967296.0);
}

/// Render in Deluge-sized chunks (the audio engine renders up to 128 samples at a time)
/// controls: the Deluge params the engine's continuous settings live on. By default, the patch's own settings.
template <size_t N>
void renderInto(Voice& voice, const Patch& patch, std::array<int32_t, N>& out, double hz = 49.0,
                const DrumControls* controls = nullptr) {
	const DrumControls c = controls ? *controls : controlsFromPatch(patch);
	out.fill(0);
	for (size_t pos = 0; pos < N; pos += 128) {
		int32_t n = static_cast<int32_t>(std::min<size_t>(128, N - pos));
		voice.render(patch, c, out.data() + pos, n, phaseIncrementFor(hz), kAmplitude, 0);
	}
}

int32_t countZeroCrossings(const int32_t* start, int32_t length) {
	int32_t count = 0;
	for (int32_t i = 1; i < length; i++) {
		if ((start[i - 1] < 0) != (start[i] < 0)) {
			count++;
		}
	}
	return count;
}

int32_t peak(const int32_t* start, int32_t length) {
	int32_t p = 0;
	for (int32_t i = 0; i < length; i++) {
		p = std::max(p, std::abs(start[i]));
	}
	return p;
}

/// Mean absolute difference between neighbouring samples, relative to level: a rough brightness measure
double brightness(const int32_t* start, int32_t length) {
	double diff = 0;
	double level = 0;
	for (int32_t i = 1; i < length; i++) {
		diff += std::abs(static_cast<double>(start[i]) - start[i - 1]);
		level += std::abs(static_cast<double>(start[i]));
	}
	return level > 0 ? diff / level : 0;
}

Patch presetPatch(Preset preset) {
	Patch patch;
	patch.setPreset(preset);
	return patch;
}

} // namespace

TEST_GROUP(DrumSynthTest){};

TEST(DrumSynthTest, everyPresetMakesSound) {
	for (uint8_t p = 0; p < static_cast<uint8_t>(Preset::NUM_PRESETS); p++) {
		Patch patch = presetPatch(static_cast<Preset>(p));
		Voice voice;
		voice.trigger(patch);
		std::array<int32_t, 4410> out{};
		renderInto(voice, patch, out, 200.0);
		CHECK(peak(out.data(), out.size()) > kAmplitude / 8);
	}
}

TEST(DrumSynthTest, presetsPickTheirEngines) {
	CHECK(presetPatch(Preset::KICK).engine() == Engine::DRUM);
	CHECK(presetPatch(Preset::LOW_TOM).engine() == Engine::DRUM);
	CHECK(presetPatch(Preset::SNARE).engine() == Engine::SNARE);
	CHECK(presetPatch(Preset::CLOSED_HAT).engine() == Engine::METAL);
	CHECK(presetPatch(Preset::CYMBAL).engine() == Engine::METAL);
}

TEST(DrumSynthTest, kickPitchFalls) {
	Patch patch = presetPatch(Preset::KICK);
	Voice voice;
	voice.trigger(patch);

	// 500 ms
	std::array<int32_t, 22050> out{};
	renderInto(voice, patch, out);

	// 20 ms windows: the start of the hit sweeps from several times the base pitch down to it
	constexpr int32_t window = 882;
	int32_t early = countZeroCrossings(out.data(), window);
	int32_t late = countZeroCrossings(out.data() + 22050 - window, window);
	CHECK(early > late * 2);

	// Once the pitch envelope has finished the kick sits at its base pitch: 49 Hz is ~2 crossings per 20 ms
	CHECK(late >= 1);
	CHECK(late <= 3);
}

TEST(DrumSynthTest, snareNoiseFilterShapesTheNoise) {
	// Noise only, so the filter is all that differs
	Patch patch = presetPatch(Preset::SNARE);
	patch.set(Patch::NOISE_MIX, 127);
	patch.set(Patch::TRANS_VOLUME, 0);
	patch.set(Patch::NOISE_RESONANCE, 0);
	patch.set(Patch::NOISE_CUTOFF, 64);

	patch.set(Patch::NOISE_FILTER_TYPE, static_cast<uint8_t>(NoiseFilter::LOW_PASS));
	Voice low;
	low.trigger(patch);
	std::array<int32_t, 8192> lowOut{};
	renderInto(low, patch, lowOut, 185.0);

	patch.set(Patch::NOISE_FILTER_TYPE, static_cast<uint8_t>(NoiseFilter::HIGH_PASS));
	Voice high;
	high.trigger(patch);
	std::array<int32_t, 8192> highOut{};
	renderInto(high, patch, highOut, 185.0);

	CHECK(peak(lowOut.data(), lowOut.size()) > 0);
	CHECK(peak(highOut.data(), highOut.size()) > 0);
	CHECK(brightness(highOut.data(), highOut.size()) > brightness(lowOut.data(), lowOut.size()) * 2);
}

TEST(DrumSynthTest, metalFmMakesItBrighter) {
	Patch patch = presetPatch(Preset::CYMBAL);
	patch.set(Patch::OSC_WAVE, static_cast<uint8_t>(Wave::SINE));
	patch.set(Patch::TRANS_VOLUME, 0);

	patch.set(Patch::FM_AMOUNT, 0);
	patch.set(Patch::FM2_AMOUNT, 0);
	Voice plain;
	plain.trigger(patch);
	std::array<int32_t, 4096> plainOut{};
	renderInto(plain, patch, plainOut, 400.0);

	patch.set(Patch::FM_AMOUNT, 127);
	patch.set(Patch::FM2_AMOUNT, 127);
	Voice fm;
	fm.trigger(patch);
	std::array<int32_t, 4096> fmOut{};
	renderInto(fm, patch, fmOut, 400.0);

	CHECK(brightness(fmOut.data(), fmOut.size()) > brightness(plainOut.data(), plainOut.size()) * 2);
}

TEST(DrumSynthTest, controlsCarryEveryLxrSetting) {
	// Each engine's continuous settings go onto the controls and come back unchanged
	for (Preset preset : {Preset::KICK, Preset::SNARE, Preset::CYMBAL}) {
		Patch patch = presetPatch(preset);
		patch.set(Patch::PITCH_DECAY, 17);
		patch.set(Patch::NOISE_RESONANCE, 99);
		patch.set(Patch::FM2_AMOUNT, 3);
		Patch copy = presetPatch(preset);
		storeControlsInPatch(controlsFromPatch(patch), copy);
		for (uint8_t p = 0; p < Patch::NUM_PARAMS; p++) {
			const auto param = static_cast<Patch::Param>(p);
			if (Patch::isUsedBy(param, patch.engine())) {
				LONGS_EQUAL(patch.get(param), copy.get(param));
			}
		}
	}
}

TEST(DrumSynthTest, eachEngineUsesDistinctControls) {
	for (Engine engine : {Engine::DRUM, Engine::SNARE, Engine::METAL}) {
		std::array<bool, DrumControls::NUM_SLOTS> taken{};
		for (uint8_t p = 0; p < Patch::NUM_PARAMS; p++) {
			const auto param = static_cast<Patch::Param>(p);
			const int32_t slot = lxrControlSlot(param);
			if (slot < 0 || !Patch::isUsedBy(param, engine)) {
				continue;
			}
			CHECK_FALSE(taken[slot]);
			taken[slot] = true;
		}
	}
}

TEST(DrumSynthTest, pitchAmountKnobDeepensTheSweep) {
	Patch patch = presetPatch(Preset::KICK);
	patch.set(Patch::TRANS_VOLUME, 0);
	constexpr int32_t window = 441; // the first 10 ms

	Voice centred;
	centred.trigger(patch);
	std::array<int32_t, 441> centredOut{};
	renderInto(centred, patch, centredOut);

	DrumControls up = controlsFromPatch(patch);
	up.values[lxrControlSlot(Patch::PITCH_AMOUNT)] = INT32_MAX; // knob fully clockwise
	Voice turned;
	turned.trigger(patch);
	std::array<int32_t, 441> turnedOut{};
	renderInto(turned, patch, turnedOut, 49.0, &up);

	CHECK(countZeroCrossings(turnedOut.data(), window) > countZeroCrossings(centredOut.data(), window));
}

TEST(DrumSynthTest, retriggerIsRepeatable) {
	Patch patch = presetPatch(Preset::LOW_TOM);

	Voice a;
	a.trigger(patch);
	std::array<int32_t, 2048> first{};
	renderInto(a, patch, first);

	a.trigger(patch);
	std::array<int32_t, 2048> second{};
	renderInto(a, patch, second);

	for (size_t i = 0; i < first.size(); i++) {
		LONGS_EQUAL(first[i], second[i]);
	}
}

TEST(DrumSynthTest, chunkSizeDoesNotChangeOutput) {
	for (Preset preset : {Preset::HIGH_TOM, Preset::SNARE, Preset::OPEN_HAT}) {
		Patch patch = presetPatch(preset);

		Voice whole;
		whole.trigger(patch);
		std::array<int32_t, 1024> a{};
		renderInto(whole, patch, a, 300.0);

		// Odd chunk sizes cross the 16-sample control ticks at different points
		Voice pieces;
		pieces.trigger(patch);
		std::array<int32_t, 1024> b{};
		size_t pos = 0;
		int32_t sizes[] = {4, 36, 100, 12, 128, 60};
		for (int32_t i = 0; pos < b.size(); i++) {
			int32_t n = std::min<int32_t>(sizes[i % 6], b.size() - pos);
			pieces.render(patch, controlsFromPatch(patch), b.data() + pos, n, phaseIncrementFor(300.0), kAmplitude, 0);
			pos += n;
		}

		for (size_t i = 0; i < a.size(); i++) {
			LONGS_EQUAL(a[i], b[i]);
		}
	}
}

TEST(DrumSynthTest, everyEngineWaveAndTransientRenders) {
	for (uint8_t engine = 0; engine < static_cast<uint8_t>(Engine::NUM_ENGINES); engine++) {
		for (uint8_t mode = 0; mode < kNumTransientModes; mode++) {
			for (uint8_t w = 0; w < static_cast<uint8_t>(Wave::NUM_WAVES); w++) {
				Patch patch;
				patch.set(Patch::ENGINE, engine);
				patch.set(Patch::TRANS_MODE, mode);
				patch.set(Patch::TRANS_VOLUME, 127);
				patch.set(Patch::OSC_WAVE, w);
				patch.set(Patch::MOD_WAVE, w);
				patch.set(Patch::MOD2_WAVE, w);
				patch.set(Patch::FM_AMOUNT, 127);
				patch.set(Patch::FM2_AMOUNT, 127);
				patch.set(Patch::PITCH_AMOUNT, 127);
				patch.set(Patch::NOISE_MIX, 64);
				patch.set(Patch::NOISE_RESONANCE, 127);
				Voice voice;
				voice.trigger(patch);
				std::array<int32_t, 2205> out{};
				renderInto(voice, patch, out, 300.0);
				CHECK(peak(out.data(), out.size()) > 0);
			}
		}
	}
}

TEST(DrumSynthTest, extremeSettingsStayInRange) {
	// Slope 0 is 0 / 0 in the LXR curve, slope 127 divides by zero, and full resonance can ring hard
	for (uint8_t engine = 0; engine < static_cast<uint8_t>(Engine::NUM_ENGINES); engine++) {
		for (uint8_t extreme : {uint8_t{0}, uint8_t{127}}) {
			Patch patch;
			patch.set(Patch::ENGINE, engine);
			patch.set(Patch::PITCH_SLOPE, extreme);
			patch.set(Patch::PITCH_DECAY, extreme);
			patch.set(Patch::NOISE_CUTOFF, extreme);
			patch.set(Patch::NOISE_RESONANCE, 127);
			patch.set(Patch::NOISE_MIX, 127);
			DrumControls controls = controlsFromPatch(patch);
			controls.values.fill(extreme ? INT32_MAX : INT32_MIN);
			Voice voice;
			voice.trigger(patch);
			std::array<int32_t, 4096> out{};
			renderInto(voice, patch, out, 49.0, &controls);
			CHECK(peak(out.data(), out.size()) <= (kAmplitude << 1));
		}
	}
}

TEST(DrumSynthTest, loadingClampsBadBytes) {
	Patch patch;
	patch.bytes().fill(255);
	patch.clampAll();
	LONGS_EQUAL(static_cast<int>(Engine::NUM_ENGINES) - 1, patch.get(Patch::ENGINE));
	LONGS_EQUAL(static_cast<int>(Wave::NUM_WAVES) - 1, patch.get(Patch::OSC_WAVE));
	LONGS_EQUAL(1, patch.get(Patch::MIX_MODE));
	LONGS_EQUAL(static_cast<int>(NoiseFilter::NUM_FILTERS) - 1, patch.get(Patch::NOISE_FILTER_TYPE));
	LONGS_EQUAL(kNumTransientModes - 1, patch.get(Patch::TRANS_MODE));
	LONGS_EQUAL(127, patch.get(Patch::FM_AMOUNT));
}

TEST(DrumSynthTest, legacyKitsLoadAsDrums) {
	// The first test build saved 12 bytes in this order: osc wave, mod wave, mod coarse, mod fine, FM, mix mode,
	// pitch decay, pitch slope, pitch amount, transient mode, transient volume, transient pitch
	const uint8_t legacy[12] = {1, 2, 70, 60, 33, 1, 44, 55, 66, 5, 77, 88};
	Patch patch;
	patch.setPreset(Preset::CYMBAL);
	patch.readLegacyDrumBytes(legacy, 12);
	CHECK(patch.engine() == Engine::DRUM);
	LONGS_EQUAL(1, patch.get(Patch::OSC_WAVE));
	LONGS_EQUAL(2, patch.get(Patch::MOD_WAVE));
	LONGS_EQUAL(70, patch.get(Patch::MOD_COARSE));
	LONGS_EQUAL(33, patch.get(Patch::FM_AMOUNT));
	LONGS_EQUAL(1, patch.get(Patch::MIX_MODE));
	LONGS_EQUAL(66, patch.get(Patch::PITCH_AMOUNT));
	LONGS_EQUAL(5, patch.get(Patch::TRANS_MODE));
	LONGS_EQUAL(88, patch.get(Patch::TRANS_PITCH));
}

TEST(DrumSynthTest, enginesOnlyShowTheirSettings) {
	CHECK(Patch::isUsedBy(Patch::PITCH_AMOUNT, Engine::DRUM));
	CHECK_FALSE(Patch::isUsedBy(Patch::PITCH_AMOUNT, Engine::METAL));
	CHECK(Patch::isUsedBy(Patch::NOISE_CUTOFF, Engine::SNARE));
	CHECK_FALSE(Patch::isUsedBy(Patch::NOISE_CUTOFF, Engine::DRUM));
	CHECK(Patch::isUsedBy(Patch::FM2_AMOUNT, Engine::METAL));
	CHECK_FALSE(Patch::isUsedBy(Patch::MOD_WAVE, Engine::SNARE));
	CHECK(Patch::isUsedBy(Patch::TRANS_MODE, Engine::METAL));
}
