#include "CppUTest/TestHarness.h"
#include "dsp/drum_synth/drum_synth.h"
#include <array>
#include <cstdint>
#include <cstdlib>

using namespace deluge::dsp::drum_synth;

namespace {

constexpr int32_t kSampleRate = 44100;
// 49 Hz, the kick preset's pitch
constexpr uint32_t kBasePhaseIncrement = static_cast<uint32_t>(49.0 / kSampleRate * 4294967296.0);
constexpr int32_t kAmplitude = 1 << 27;

/// Render in Deluge-sized chunks (the audio engine renders up to 128 samples at a time)
template <size_t N>
void renderInto(Voice& voice, const Patch& patch, std::array<int32_t, N>& out) {
	out.fill(0);
	for (size_t pos = 0; pos < N; pos += 128) {
		int32_t n = static_cast<int32_t>(std::min<size_t>(128, N - pos));
		voice.render(patch, out.data() + pos, n, kBasePhaseIncrement, kAmplitude, 0);
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

} // namespace

TEST_GROUP(DrumSynthTest){};

TEST(DrumSynthTest, kickMakesSound) {
	Patch patch;
	patch.setPreset(Preset::KICK);
	Voice voice;
	voice.trigger(patch);

	std::array<int32_t, 4410> out{};
	renderInto(voice, patch, out);

	CHECK(peak(out.data(), out.size()) > kAmplitude / 4);
}

TEST(DrumSynthTest, kickPitchFalls) {
	Patch patch;
	patch.setPreset(Preset::KICK);
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

TEST(DrumSynthTest, retriggerIsRepeatable) {
	Patch patch;
	patch.setPreset(Preset::LOW_TOM);

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
	Patch patch;
	patch.setPreset(Preset::HIGH_TOM);

	Voice whole;
	whole.trigger(patch);
	std::array<int32_t, 1024> a{};
	renderInto(whole, patch, a);

	// Odd chunk sizes cross the 16-sample control ticks at different points
	Voice pieces;
	pieces.trigger(patch);
	std::array<int32_t, 1024> b{};
	size_t pos = 0;
	int32_t sizes[] = {4, 36, 100, 12, 128, 60};
	for (int32_t i = 0; pos < b.size(); i++) {
		int32_t n = std::min<int32_t>(sizes[i % 6], b.size() - pos);
		pieces.render(patch, b.data() + pos, n, kBasePhaseIncrement, kAmplitude, 0);
		pos += n;
	}

	for (size_t i = 0; i < a.size(); i++) {
		LONGS_EQUAL(a[i], b[i]);
	}
}

TEST(DrumSynthTest, everyTransientAndWaveRenders) {
	for (uint8_t mode = 0; mode < kNumTransientModes; mode++) {
		for (uint8_t w = 0; w < static_cast<uint8_t>(Wave::NUM_WAVES); w++) {
			for (uint8_t mix = 0; mix <= 1; mix++) {
				Patch patch;
				patch.set(Patch::TRANS_MODE, mode);
				patch.set(Patch::TRANS_VOLUME, 127);
				patch.set(Patch::OSC_WAVE, w);
				patch.set(Patch::MOD_WAVE, w);
				patch.set(Patch::MIX_MODE, mix);
				patch.set(Patch::FM_AMOUNT, 127);
				patch.set(Patch::PITCH_AMOUNT, 127);
				Voice voice;
				voice.trigger(patch);
				std::array<int32_t, 2205> out{};
				renderInto(voice, patch, out);
				CHECK(peak(out.data(), out.size()) > 0);
			}
		}
	}
}

TEST(DrumSynthTest, extremeSettingsStayFinite) {
	// Slope 0 is 0 / 0 in the LXR curve, and slope 127 divides by zero
	for (uint8_t slope : {uint8_t{0}, uint8_t{127}}) {
		Patch patch;
		patch.set(Patch::PITCH_SLOPE, slope);
		patch.set(Patch::PITCH_DECAY, 0);
		Voice voice;
		voice.trigger(patch);
		std::array<int32_t, 512> out{};
		renderInto(voice, patch, out);
		CHECK(peak(out.data(), out.size()) <= (kAmplitude << 1));
	}
}

TEST(DrumSynthTest, loadingClampsBadBytes) {
	Patch patch;
	patch.bytes().fill(255);
	patch.recalculate();
	LONGS_EQUAL(static_cast<int>(Wave::NUM_WAVES) - 1, patch.get(Patch::OSC_WAVE));
	LONGS_EQUAL(1, patch.get(Patch::MIX_MODE));
	LONGS_EQUAL(kNumTransientModes - 1, patch.get(Patch::TRANS_MODE));
	LONGS_EQUAL(127, patch.get(Patch::FM_AMOUNT));
}
