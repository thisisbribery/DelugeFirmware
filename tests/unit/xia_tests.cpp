#include "CppUTest/TestHarness.h"
#include "dsp/drum_synth/drum_synth.h"
#include "dsp/drum_synth/xia.h"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>

using namespace deluge::dsp::drum_synth;

namespace {

// Hashes of every output sample and every state word, block by block, for scenarios rendered by the XIA engines'
// original code in the Model:Cycles firmware, run in an emulator. The scenario generator below must stay identical
// to the one that made them (golden_hash.py, with the emulator, kept outside this repository with the firmware).
constexpr std::array<uint64_t, 24> kTiaHashes{
    0x2efa41b4d010c362ull, 0xaac43efc0601c9e7ull, 0xd848c1b0eac48507ull, 0xc577a8f7ed71c373ull, 0x064ae3491ea23762ull,
    0xcdf659e260ddb8a0ull, 0x8cf66777621e120aull, 0x5fc39b0409106bc7ull, 0x8f68f1570522272aull, 0x3b5813b238d88f07ull,
    0xc1148369aafee069ull, 0x440f857f14d7a9f8ull, 0xa63ead43ca2ff06full, 0xdf40296203fc8262ull, 0x12e3c68d9341c82full,
    0x2264d7ecf42ac9d0ull, 0xec66f5862b7b5975ull, 0xe2184242b5981b88ull, 0x29be585fa07d9340ull, 0x151289c080587590ull,
    0x6b6c3a9099fa6e0aull, 0xdee25304c9bd3a0eull, 0x2bd0c1cc80e896e6ull, 0x2d95f31a5a9418d4ull,
};
constexpr std::array<uint64_t, 24> kIiaHashes{
    0xc9c7c68e4a2c3130ull, 0x46988d8958d32a8full, 0x0d6ebe1a96b34d47ull, 0x95abb440da79308aull, 0x4d3a5da8effc1408ull,
    0x025ae5d3bc811547ull, 0x11295a5f21b063d2ull, 0xe5f1e2349a8767a9ull, 0xbaa862eed0c6b0baull, 0x01d046e8752d2631ull,
    0x337f54b541a1fef6ull, 0xf53238a1e544cb4eull, 0xe1a112ed060d976cull, 0xdc34da8d96b0fda7ull, 0x4a706381e0de7992ull,
    0xcaa6dd5b005c153eull, 0xc0706335981146faull, 0x84f45f813c66b4acull, 0x081bad005f88909dull, 0x8788a14b3a72210cull,
    0x41c70529644fe5beull, 0x603252a04724c41eull, 0xfad17f471f47036aull, 0xf720e06245e3bb36ull,
};

constexpr int32_t kBlocks = 200;

class Rng {
public:
	explicit Rng(uint32_t seed) : x_(seed) {}
	uint32_t next() {
		x_ ^= x_ << 13;
		x_ ^= x_ >> 17;
		x_ ^= x_ << 5;
		return x_;
	}
	int32_t range(int32_t lo, int32_t hi) {
		return lo + static_cast<int32_t>(next() % static_cast<uint32_t>(hi - lo + 1));
	}
	int32_t knob() {
		switch (next() % 4) {
		case 0:
			return range(0, 0x7F00);
		case 1:
			return static_cast<int32_t>(next() % 128) * 256;
		case 2:
			return 0x4000;
		default:
			return 0x7F00;
		}
	}

private:
	uint32_t x_;
};

struct Common {
	int32_t lowPass, drive, decay;
};

Common common(Rng& r) {
	Common c{};
	c.lowPass = (r.next() % 3 == 0) ? 0x7F00 : r.range(0x1800, 0x7F00);
	c.drive = (r.next() % 2) ? 0 : r.knob();
	c.decay = (r.next() % 3 == 0) ? 0x7F00 : r.range(0x4800, 0x7EFF);
	return c;
}

xia::TiaSettings tiaScenario(Rng& r, int32_t variant) {
	xia::TiaSettings t{};
	t.variant = variant;
	for (int32_t& f : t.freq) {
		f = r.knob();
	}
	for (int32_t& b : t.blendex) {
		b = (r.next() % 3 == 0) ? 0 : r.range(0, 0x7FFF);
	}
	const Common c = common(r);
	t.lowPass = c.lowPass;
	t.drive = c.drive;
	t.decay = c.decay;
	t.pitch = r.range(0x180000, 0x6C0000);
	t.punch = (r.next() % 4 == 0) ? 1 : 0;
	t.gate = (r.next() % 6 == 0) ? 1 : 0;
	return t;
}

xia::IiaSettings iiaScenario(Rng& r, int32_t variant) {
	xia::IiaSettings t{};
	t.variant = variant;
	t.weight = r.knob();
	t.alloy = r.knob();
	const Common c = common(r);
	t.lowPass = c.lowPass;
	t.drive = c.drive;
	t.decay = c.decay;
	t.shape = r.knob();
	t.attack = (r.next() % 2) ? r.range(0, 0x7F00) : 0x4000;
	t.pitch = r.range(0x180000, 0x6C0000);
	t.punch = (r.next() % 4 == 0) ? 1 : 0;
	t.gate = (r.next() % 6 == 0) ? 1 : 0;
	return t;
}

uint64_t fnv(uint64_t h, const void* data, size_t size) {
	const auto* bytes = static_cast<const uint8_t*>(data);
	for (size_t i = 0; i < size; i++) {
		h ^= bytes[i];
		h *= 0x100000001B3ull;
	}
	return h;
}

template <class State, class Settings, class Make>
uint64_t scenarioHash(int32_t scenario, uint32_t engineSalt, Make make) {
	Rng r(0x9E3779B9u ^ (static_cast<uint32_t>(scenario) * 0x85EBCA6Bu) ^ engineSalt);
	const int32_t variant = scenario % 3;
	Settings settings = make(r, variant);
	State state;
	xia::reset(state);
	uint64_t h = 0xCBF29CE484222325ull;
	for (int32_t b = 0; b < kBlocks; b++) {
		if (scenario % 2 == 1 && b > 0 && b % 25 == 0) {
			const int32_t pitch = settings.pitch;
			settings = make(r, variant);
			settings.pitch = pitch;
		}
		const bool trigger = b == 0 || (scenario % 4 == 2 && b == 120);
		const bool fadeOut = scenario % 4 == 2 && b == 119;
		Settings block = settings;
		if (block.gate && b >= 80) {
			block.release = 1;
		}
		int32_t out[xia::kBlockSize];
		xia::render(state, block, trigger, fadeOut, out);
		h = fnv(h, out, sizeof(out)); // Little-endian, as the generator writes them
		h = fnv(h, &state, sizeof(state));
	}
	return h;
}

constexpr int32_t kSampleRate = 44100;

uint32_t phaseIncrementFor(double hz) {
	return static_cast<uint32_t>(hz / kSampleRate * 4294967296.0);
}

/// Render a triggered voice, returning how many samples it ran before finishing (or the whole length)
int32_t renderVoice(Voice& voice, const Patch& patch, int32_t* out, int32_t length, double hz,
                    const KnobOffsets& knobs = {}) {
	voice.trigger(patch, knobs);
	std::fill(out, out + length, 0);
	for (int32_t pos = 0; pos < length; pos += 100) { // Deluge-style chunks that don't line up with XIA blocks
		const int32_t n = std::min(100, length - pos);
		voice.render(patch, knobs, out + pos, n, phaseIncrementFor(hz), 1 << 27, 0);
		if (voice.finished()) {
			return pos + n;
		}
	}
	return length;
}

double zeroCrossingHz(const int32_t* out, int32_t start, int32_t length) {
	int32_t crossings = 0;
	for (int32_t i = start + 1; i < start + length; i++) {
		if ((out[i - 1] < 0) && (out[i] >= 0)) {
			crossings++;
		}
	}
	return crossings * static_cast<double>(kSampleRate) / length;
}

} // namespace

TEST_GROUP(XiaTests){};

TEST(XiaTests, tiaMatchesTheOriginalSampleForSample) {
	for (int32_t s = 0; s < 24; s++) {
		const uint64_t h = scenarioHash<xia::TiaState, xia::TiaSettings>(s, 0, tiaScenario);
		if (h != kTiaHashes[s]) {
			FAIL(("TIA scenario " + std::to_string(s) + " differs from the original").c_str());
		}
	}
}

TEST(XiaTests, iiaMatchesTheOriginalSampleForSample) {
	for (int32_t s = 0; s < 24; s++) {
		const uint64_t h = scenarioHash<xia::IiaState, xia::IiaSettings>(s, 1, iiaScenario);
		if (h != kIiaHashes[s]) {
			FAIL(("IIA scenario " + std::to_string(s) + " differs from the original").c_str());
		}
	}
}

TEST(XiaTests, tiaPlaysTheDelugesPitch) {
	// TIA's defaults are a plain sine, so its pitch is easy to count
	Patch patch;
	patch.setXiaPreset(Engine::TIA, 0);
	patch.set(Patch::XIA_DECAY, 127); // hold, so the level doesn't fall away while counting
	static std::array<int32_t, kSampleRate> out;
	Voice voice;
	for (double hz : {55.0, 261.63, 1000.0}) {
		renderVoice(voice, patch, out.data(), kSampleRate, hz);
		DOUBLES_EQUAL(hz, zeroCrossingHz(out.data(), 0, kSampleRate), hz * 0.01 + 1);
	}
}

TEST(XiaTests, voiceFinishesWhenTheEngineDecays) {
	static std::array<int32_t, kSampleRate * 4> out;
	for (Engine engine : {Engine::TIA, Engine::IIA}) {
		for (uint8_t variant = 0; variant < 3; variant++) {
			Patch patch;
			patch.setXiaPreset(engine, variant);
			Voice voice;
			const int32_t length = renderVoice(voice, patch, out.data(), out.size(), 130.8);
			CHECK(voice.finished());
			CHECK(length < static_cast<int32_t>(out.size()));
			int32_t peak = 0;
			for (int32_t i = 0; i < length; i++) {
				peak = std::max(peak, std::abs(out[i]));
			}
			CHECK(peak > (1 << 20));
		}
	}
}

TEST(XiaTests, lxrEnginesNeverReportFinished) {
	Patch patch;
	patch.setPreset(Preset::KICK);
	static std::array<int32_t, kSampleRate> out;
	Voice voice;
	renderVoice(voice, patch, out.data(), out.size(), 49.0);
	CHECK_FALSE(voice.finished());
}

TEST(XiaTests, decayKnobLengthensTheSound) {
	Patch patch;
	patch.setXiaPreset(Engine::TIA, 0);
	patch.set(Patch::XIA_DECAY, 40);
	static std::array<int32_t, kSampleRate * 4> out;
	Voice voice;
	const int32_t shortLength = renderVoice(voice, patch, out.data(), out.size(), 130.8);
	KnobOffsets knobs;
	knobs.values[KnobOffsets::TRANSIENT] = 1 << 28; // the decay knob's slot on XIA rows: +32
	const int32_t longLength = renderVoice(voice, patch, out.data(), out.size(), 130.8, knobs);
	CHECK(longLength > shortLength * 2);
}

TEST(XiaTests, xiaSettingsAreHiddenFromLxrEngines) {
	CHECK_FALSE(Patch::isUsedBy(Patch::TIA_FREQ1, Engine::DRUM));
	CHECK_FALSE(Patch::isUsedBy(Patch::XIA_DECAY, Engine::METAL));
	CHECK(Patch::isUsedBy(Patch::XIA_DECAY, Engine::IIA));
	CHECK_FALSE(Patch::isUsedBy(Patch::IIA_WEIGHT, Engine::TIA));
	CHECK_FALSE(Patch::isUsedBy(Patch::OSC_WAVE, Engine::TIA));
	CHECK_FALSE(Patch::isUsedBy(Patch::PITCH_AMOUNT, Engine::IIA));
	CHECK_FALSE(Patch::isUsedBy(Patch::FM_AMOUNT, Engine::TIA));
	CHECK(Patch::isUsedBy(Patch::ENGINE, Engine::TIA));
}

TEST(XiaTests, olderSavesLoadWithXiaDefaults) {
	// A kit saved before the XIA engines has only the LXR settings: the rest keep their defaults
	Patch saved;
	saved.setPreset(Preset::SNARE);
	Patch loaded;
	std::memcpy(loaded.bytes().data(), saved.bytes().data(), Patch::XIA_VARIANT);
	loaded.clampAll();
	CHECK(loaded.engine() == Engine::SNARE);
	CHECK_EQUAL(64, loaded.get(Patch::TIA_FREQ1));
	CHECK_EQUAL(127, loaded.get(Patch::XIA_LOW_PASS));
	CHECK_EQUAL(0, loaded.get(Patch::XIA_PUNCH));
}
