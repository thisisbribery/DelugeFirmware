#include "CppUTest/TestHarness.h"
#include "model/swing/swing_style.h"

using namespace deluge::swing_style;

namespace {
constexpr uint32_t kSixteenth = 24; // Swing interval in internal ticks at the default 96 ticks per quarter note

double unswungTick(Style style, int32_t level, uint32_t swungTick) {
	return swungToUnswung(style, level, kSixteenth, swungTick) / 65536.0;
}
} // namespace

TEST_GROUP(SwingStyleTests){};

TEST(SwingStyleTests, namesRoundTrip) {
	for (int32_t i = 0; i < kNumStyles; i++) {
		auto style = static_cast<Style>(i);
		CHECK(fromName(getName(style)) == style);
	}
	CHECK(fromName("NOT A STYLE") == Style::CLASSIC);
}

TEST(SwingStyleTests, stepsLandWhereTheVisualiserPutsThem) {
	// HOPP level 1: the even 16th lands on tick 26 of the pair
	DOUBLES_EQUAL(26.0, unswungTick(Style::HOPP, 1, 24), 0.001);
	// DEEP level 6: the 8ths split 66.7%, so the "&" drags to tick 64.032 of the beat
	DOUBLES_EQUAL(64.032, unswungTick(Style::DEEP, 6, 48), 0.001);
	// FOLD level 6: the e and a fold 14.4 ticks in toward the &, which stays put
	DOUBLES_EQUAL(38.4, unswungTick(Style::FOLD, 6, 24), 0.001);
	DOUBLES_EQUAL(48.0, unswungTick(Style::FOLD, 6, 48), 0.001);
	DOUBLES_EQUAL(57.6, unswungTick(Style::FOLD, 6, 72), 0.001);
	// SQEZ level 6: 36 ticks per beat on the module, the a on tick 34, so 34 * 96 / 36 at the Deluge's 96
	DOUBLES_EQUAL(90.667, unswungTick(Style::SQEZ, 6, 72), 0.001);
}

TEST(SwingStyleTests, blocksStartAndEndOnTheGrid) {
	for (int32_t i = 1; i < kNumStyles; i++) {
		auto style = static_cast<Style>(i);
		uint32_t block = getNumSteps(style) * kSixteenth;
		for (int32_t level = 1; level <= kMaxLevel; level++) {
			DOUBLES_EQUAL(0.0, unswungTick(style, level, 0), 0.0);
			DOUBLES_EQUAL((double)block, unswungTick(style, level, block), 0.0);
		}
	}
}

TEST(SwingStyleTests, unswungToSwungInvertsSwungToUnswung) {
	for (int32_t i = 1; i < kNumStyles; i++) {
		auto style = static_cast<Style>(i);
		for (uint32_t interval : {kSixteenth, 2 * kSixteenth}) {
			uint32_t block = getNumSteps(style) * interval;
			for (int32_t level = 1; level <= kMaxLevel; level++) {
				uint64_t previous = 0;
				for (uint32_t tick = 0; tick < block; tick++) {
					uint64_t unswung = swungToUnswung(style, level, interval, tick);
					if (tick > 0) {
						CHECK(unswung > previous);
					}
					previous = unswung;
					CHECK_EQUAL(tick, unswungToSwung(style, level, interval, unswung));
					// Just before the next tick is still this tick
					uint64_t nextUnswung = swungToUnswung(style, level, interval, tick + 1);
					CHECK_EQUAL(tick, unswungToSwung(style, level, interval, nextUnswung - 1));
				}
			}
		}
	}
}
