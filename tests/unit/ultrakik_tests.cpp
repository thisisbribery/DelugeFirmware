#include "CppUTest/TestHarness.h"
#include "dsp/drum_synth/ultrakik.h"
#include "dsp/drum_synth/ultrakik_presets.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

using namespace deluge::dsp::drum_synth;

namespace {

constexpr float kSampleRate = 44100.f;

uint32_t incrementForHz(double hz) {
	return static_cast<uint32_t>(hz / kSampleRate * 4294967296.0);
}

/// The engine's starting knobs: its default preset
ultrakik::Settings defaults() {
	return ultrakik::settingsFor(ultrakik::kPresets[ultrakik::kDefaultPreset]);
}

/// A preset's note on a kit row, which plays at middle C
double presetHz(const ultrakik::Preset& preset) {
	return 261.6256 * std::pow(2.0, preset.transpose / 12.0);
}

/// Every choice turned on at once, with a bend and the crusher
ultrakik::Settings everything(int32_t i) {
	ultrakik::Settings s = defaults();
	s.wave = static_cast<ultrakik::Wave>(i % static_cast<int32_t>(ultrakik::Wave::NUM_WAVES));
	s.driveType = static_cast<ultrakik::DriveType>(i % static_cast<int32_t>(ultrakik::DriveType::NUM_TYPES));
	s.clickType = static_cast<ultrakik::Click>(i % static_cast<int32_t>(ultrakik::Click::NUM_CLICKS));
	s.bend = (i % 2 == 0) ? -0.5f : 0.3f;
	s.bendTime = 0.3f;
	s.crush = 0.4f;
	s.fold = 0.8f;
	s.grit = 1.f;
	s.drive = 1.f;
	s.click = 1.f;
	return s;
}

std::vector<int32_t> renderNote(const ultrakik::Settings& s, double hz, int32_t length, int32_t block,
                                bool* finished = nullptr) {
	ultrakik::Voice voice;
	voice.reset();
	voice.trigger();
	std::vector<int32_t> out(length);
	for (int32_t i = 0; i < length; i += block) {
		voice.render(s, incrementForHz(hz), kSampleRate, out.data() + i, std::min(block, length - i));
	}
	if (finished != nullptr) {
		*finished = voice.finished();
	}
	return out;
}

float peak(const std::vector<int32_t>& x, int32_t from = 0, int32_t to = -1) {
	int64_t m = 0;
	for (int32_t i = from; i < ((to < 0) ? static_cast<int32_t>(x.size()) : to); i++) {
		m = std::max<int64_t>(m, std::abs(static_cast<int64_t>(x[i])));
	}
	return static_cast<float>(m) / 2147483648.f;
}

} // namespace

TEST_GROUP(Ultrakik){};

TEST(Ultrakik, sameEveryTime) {
	CHECK(renderNote(defaults(), 49, 22050, 128) == renderNote(defaults(), 49, 22050, 128));
}

TEST(Ultrakik, sameAtAnyHostBlockSize) {
	for (int32_t i = 0; i < 4; i++) {
		const auto reference = renderNote(everything(i), 49, 22050, 128);
		for (int32_t block : {1, 7, 32, 100}) {
			CHECK(renderNote(everything(i), 49, 22050, block) == reference);
		}
	}
	const auto reference = renderNote(defaults(), 49, 22050, 128);
	for (int32_t block : {1, 7, 32, 100}) {
		CHECK(renderNote(defaults(), 49, 22050, block) == reference);
	}
}

TEST(Ultrakik, defaultHitsHardWithoutClipping) {
	const auto x = renderNote(defaults(), 49, 44100, 128);
	const float p = peak(x);
	CHECK(p > 0.8f);
	CHECK(p < 1.f);
	// Still full 100 ms in: the thump
	CHECK(peak(x, 4410, 6615) > 0.5f);
}

TEST(Ultrakik, neverPastFullScale) {
	for (int32_t i = 0; i < 4; i++) {
		CHECK(peak(renderNote(everything(i), 49, 44100, 128)) < 1.f);
		CHECK(peak(renderNote(everything(i), 600, 44100, 128)) < 1.f);
	}
}

TEST(Ultrakik, defaultTailIsShort) {
	bool finished = false;
	const auto x = renderNote(defaults(), 49, 2 * 44100, 128, &finished);
	CHECK(finished);
	// 40 dB down by half a second
	CHECK(peak(x, 22050, 26460) < 0.01f);
}

TEST(Ultrakik, decayAtTheTopHolds) {
	ultrakik::Settings s = defaults();
	s.decay = 1.f;
	bool finished = true;
	const auto x = renderNote(s, 49, 3 * 44100, 128, &finished);
	CHECK(!finished);
	CHECK(peak(x, 2 * 44100, 3 * 44100) > 0.5f);
}

TEST(Ultrakik, settlesOnTheNote) {
	// Clean sine, no bend: the zero crossings 300 ms in are the note's
	ultrakik::Settings s = defaults();
	s.fold = s.grit = s.drive = s.click = 0.f;
	s.decay = 1.f;
	const auto x = renderNote(s, 55, 44100, 128);
	int32_t first = -1;
	int32_t last = -1;
	int32_t crossings = 0;
	for (int32_t i = 13230; i < 44100; i++) {
		if (x[i - 1] < 0 && x[i] >= 0) {
			if (first < 0) {
				first = i;
			}
			last = i;
			crossings++;
		}
	}
	const double hz = (crossings - 1) * kSampleRate / (last - first);
	DOUBLES_EQUAL(55.0, hz, 0.3);
}

TEST(Ultrakik, bendMovesThePitch) {
	ultrakik::Settings s = defaults();
	s.fold = s.grit = s.drive = s.click = s.sweep = s.punch = 0.f;
	s.decay = 1.f;
	s.bend = -0.5f;   // 12 semitones down
	s.bendTime = 0.f; // 40 ms
	const auto x = renderNote(s, 100, 44100, 128);
	int32_t first = -1;
	int32_t last = -1;
	int32_t crossings = 0;
	for (int32_t i = 22050; i < 44100; i++) {
		if (x[i - 1] < 0 && x[i] >= 0) {
			if (first < 0) {
				first = i;
			}
			last = i;
			crossings++;
		}
	}
	DOUBLES_EQUAL(50.0, (crossings - 1) * kSampleRate / (last - first), 0.5);
}

TEST(Ultrakik, defaultIsUltra) {
	const ultrakik::Settings s = defaults();
	CHECK_EQUAL(70, static_cast<int32_t>(lroundf(s.sweep * 127)));
	CHECK_EQUAL(60, static_cast<int32_t>(lroundf(s.drive * 127)));
	CHECK_EQUAL(52, static_cast<int32_t>(lroundf(s.decay * 127)));
	DOUBLES_EQUAL(0.0, s.bend, 1e-6);
	CHECK(s.wave == ultrakik::Wave::SINE);
}

TEST(Ultrakik, everyPresetHitsHardWithoutClipping) {
	for (const ultrakik::Preset& preset : ultrakik::kPresets) {
		const float p = peak(renderNote(ultrakik::settingsFor(preset), presetHz(preset), 44100, 128));
		CHECK(p > 0.8f);
		CHECK(p < 1.f);
	}
}

TEST(Ultrakik, everyPresetEnds) {
	for (const ultrakik::Preset& preset : ultrakik::kPresets) {
		bool finished = false;
		renderNote(ultrakik::settingsFor(preset), presetHz(preset), 5 * 44100, 128, &finished);
		CHECK(finished);
	}
}

TEST(Ultrakik, everyPresetSameAtAnyHostBlockSize) {
	for (const ultrakik::Preset& preset : ultrakik::kPresets) {
		const auto reference = renderNote(ultrakik::settingsFor(preset), presetHz(preset), 22050, 128);
		CHECK(renderNote(ultrakik::settingsFor(preset), presetHz(preset), 22050, 7) == reference);
	}
}
