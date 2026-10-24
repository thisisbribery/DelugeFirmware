#include "CppUTest/TestHarness.h"
#include "dsp/drum_synth/cyc.h"
#include "dsp/drum_synth/drum_synth.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <vector>

using namespace deluge::dsp::drum_synth;

namespace {

constexpr float kSampleRate = 44100.f;
constexpr int32_t kNumKinds = static_cast<int32_t>(cyc::Kind::NUM_KINDS);

uint32_t incrementForNote(double note) {
	return static_cast<uint32_t>(440.0 * std::pow(2.0, (note - 69.0) / 12.0) / kSampleRate * 4294967296.0);
}

cyc::Settings middleSettings() {
	cyc::Settings s;
	s.macro = {0.4f, 0.4f, 0.4f, 0.4f};
	s.decay = 0.4f;
	return s;
}

std::vector<int32_t> renderNote(cyc::Kind kind, const cyc::Settings& s, double note, int32_t length, int32_t block) {
	cyc::Voice voice;
	voice.reset();
	voice.trigger();
	std::vector<int32_t> out(length);
	for (int32_t i = 0; i < length; i += block) {
		voice.render(kind, s, incrementForNote(note), kSampleRate, out.data() + i, std::min(block, length - i));
	}
	return out;
}

} // namespace

TEST_GROUP(CycEngines){};

TEST(CycEngines, sameEveryTime) {
	for (int32_t k = 0; k < kNumKinds; k++) {
		const auto kind = static_cast<cyc::Kind>(k);
		CHECK(renderNote(kind, middleSettings(), 60, 22050, 128) == renderNote(kind, middleSettings(), 60, 22050, 128));
	}
}

TEST(CycEngines, sameAtAnyHostBlockSize) {
	for (int32_t k = 0; k < kNumKinds; k++) {
		const auto kind = static_cast<cyc::Kind>(k);
		const auto reference = renderNote(kind, middleSettings(), 60, 22050, 128);
		for (int32_t block : {1, 7, 32, 100}) {
			CHECK(renderNote(kind, middleSettings(), 60, 22050, block) == reference);
		}
	}
}

TEST(CycEngines, voicesDontShareState) {
	for (int32_t k = 0; k < kNumKinds; k++) {
		const auto kindA = static_cast<cyc::Kind>(k);
		const auto kindB = static_cast<cyc::Kind>((k + 3) % kNumKinds);
		cyc::Settings b = middleSettings();
		b.macro = {0.9f, 0.1f, 0.7f, 0.2f};
		cyc::Voice voiceA, voiceB;
		voiceA.reset();
		voiceB.reset();
		voiceA.trigger();
		voiceB.trigger();
		constexpr int32_t kLength = 11025;
		std::vector<int32_t> outA(kLength), outB(kLength);
		for (int32_t i = 0; i < kLength; i += 128) {
			const int32_t n = std::min(128, kLength - i);
			voiceA.render(kindA, middleSettings(), incrementForNote(60), kSampleRate, outA.data() + i, n);
			voiceB.render(kindB, b, incrementForNote(48), kSampleRate, outB.data() + i, n);
		}
		CHECK(outA == renderNote(kindA, middleSettings(), 60, kLength, 128));
		CHECK(outB == renderNote(kindB, b, 48, kLength, 128));
	}
}

TEST(CycEngines, soundAndStayInRangeAtTheExtremes) {
	for (int32_t k = 0; k < kNumKinds; k++) {
		const auto kind = static_cast<cyc::Kind>(k);
		for (int32_t corner = 0; corner < 16; corner++) {
			for (double note : {12.0, 60.0, 120.0}) {
				cyc::Settings s;
				for (int32_t m = 0; m < cyc::kNumMacros; m++) {
					s.macro[m] = ((corner >> m) & 1) ? 1.f : 0.f;
				}
				s.decay = 1.f;
				s.punch = (corner & 1) != 0;
				const auto out = renderNote(kind, s, note, 4410, 128);
				int64_t peak = 0;
				for (int32_t x : out) {
					CHECK(x != INT32_MIN);
					peak = std::max<int64_t>(peak, std::llabs(x));
				}
				CHECK(peak > 0);
			}
		}
	}
}

TEST(CycEngines, shortDecaysFreeTheVoice) {
	for (int32_t k = 0; k < kNumKinds; k++) {
		cyc::Voice voice;
		voice.reset();
		voice.trigger();
		cyc::Settings s = middleSettings();
		s.decay = 0.1f;
		std::array<int32_t, 128> out{};
		int32_t blocks = 0;
		do {
			voice.render(static_cast<cyc::Kind>(k), s, incrementForNote(60), kSampleRate, out.data(), 128);
		} while (!voice.finished() && ++blocks < 44100 * 10 / 128);
		CHECK(voice.finished());
	}
}

TEST(CycEngines, defaultsAreLoudAndDriven) {
	for (Engine engine : {Engine::CYC_KIK, Engine::CYC_SNR, Engine::CYC_TONE, Engine::CLAP_ENG}) {
		const DrumControls c = DrumControls::defaultsFor(engine);
		CHECK_EQUAL(100, c.value(DrumControls::CYC_DRIVE));
		CHECK(c.value(DrumControls::COMMON_DECAY) >= 50);
		const auto kind = cycKind(engine);
		cyc::Settings s;
		for (int32_t m = 0; m < cyc::kNumMacros; m++) {
			s.macro[m] = c.value(m) / 127.f;
		}
		s.decay = c.value(DrumControls::COMMON_DECAY) / 127.f;
		s.drive = c.value(DrumControls::CYC_DRIVE) / 127.f;
		const auto out = renderNote(kind, s, 60, 11025, 128);
		int64_t peak = 0;
		for (int32_t x : out) {
			peak = std::max<int64_t>(peak, std::llabs(x));
		}
		CHECK(peak > (INT32_MAX / 2)); // within 6 dB of full scale
	}
}

TEST(CycEngines, everyEngineHasADriveAndDecay) {
	for (uint8_t e = 0; e < static_cast<uint8_t>(Engine::NUM_ENGINES); e++) {
		const auto engine = static_cast<Engine>(e);
		CHECK(driveSlot(engine) != decaySlot(engine));
		CHECK(driveSlot(engine) < DrumControls::NUM_SLOTS);
		// The LXR voices' drive and decay start neutral, so older kits sound the same
		if (isLxr(engine)) {
			const DrumControls c = DrumControls::defaultsFor(Engine::TIA);
			CHECK_EQUAL(0, c.value(driveSlot(engine)));
			CHECK_EQUAL(127, c.value(decaySlot(engine)));
		}
	}
}

TEST(CycEngines, lxrDriveAndDecayWork) {
	Patch patch;
	patch.setPreset(Preset::KICK);
	DrumControls controls = controlsFromPatch(patch);
	auto peakAndFinish = [&](int32_t drive, int32_t decay, bool& finished) {
		controls.values[DrumControls::LXR_DRIVE] = DrumControls::fromValue(drive);
		controls.values[DrumControls::COMMON_DECAY] = DrumControls::fromValue(decay);
		Voice voice;
		voice.trigger(patch);
		std::array<int32_t, 128> out{};
		int64_t peak = 0;
		for (int32_t b = 0; b < 44100 * 3 / 128; b++) {
			out.fill(0);
			voice.render(patch, controls, out.data(), 128, incrementForNote(48), 1 << 30, 0);
			for (int32_t x : out) {
				peak = std::max<int64_t>(peak, std::llabs(x));
			}
		}
		finished = voice.finished();
		return peak;
	};
	bool finished = false;
	const int64_t plain = peakAndFinish(0, 127, finished);
	CHECK(!finished);
	const int64_t driven = peakAndFinish(127, 127, finished);
	CHECK(driven > plain);
	peakAndFinish(0, 20, finished);
	CHECK(finished);
}

TEST(CycEngines, enginesAreNumberedAfterTheXiaOnes) {
	// The engine number is saved with the sound: existing engines must keep theirs
	CHECK_EQUAL(5, static_cast<int32_t>(Engine::CYC_KIK));
	CHECK_EQUAL(12, static_cast<int32_t>(Engine::CLAP_ENG));
	CHECK(isCyc(Engine::CLAP_ENG));
	CHECK(!isCyc(Engine::IIA));
	CHECK(isLxr(Engine::METAL));
	CHECK(cycKind(Engine::VTG_SNR) == cyc::Kind::VSNR);
	CHECK(Patch::isUsedBy(Patch::XIA_PUNCH, Engine::CYC_TONE));
	CHECK(!Patch::isUsedBy(Patch::XIA_VARIANT, Engine::CYC_TONE));
	CHECK(!Patch::isUsedBy(Patch::OSC_WAVE, Engine::CYC_KIK));
}
