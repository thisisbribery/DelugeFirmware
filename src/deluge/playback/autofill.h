/*
 * Copyright © 2026 Synthstrom Audible Limited
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

#include <array>
#include <cstdint>

class Clip;
class InstrumentClip;
class Song;

// Autofill, after the fills on the experimental Stolperbeats module firmware. With Tap Tempo Action set to Autofill
// in community features, pressing TAP plays a fill on the bottom three rows (Kick, Snare, Hihat 1) of every playing
// kit clip: from the next 16th to the end of the bar, carrying on while TAP is held. Hold TAP and turn TEMPO to pick
// one of five fills. Fill hits land on the swung 16ths, so they follow the song's swing.
namespace deluge::autofill {

constexpr int32_t kNumFills = 5;

class Autofill {
public:
	/// True when TAP is set to play fills in community features
	static bool isChosenInSettings();

	void tapPressed();
	void tapReleased();
	/// Holding TAP and turning TEMPO: picks the fill (and cancels the one the press started)
	void changeFill(int32_t offset);
	void displayFill();

	/// Called for every swung tick before the clips play it. Returns swung ticks until autofill next needs a tick.
	int32_t processTick(int64_t swungTick, int32_t swungTickIncrement);
	/// Called after the clips have played the tick
	void afterTick() { numSilenced_ = 0; }
	/// True if autofill has taken this row's hit away on the current tick
	bool shouldSilence(InstrumentClip const* clip, int32_t noteRowId) const;
	void playbackStopped();

	bool isPlayingFill() const { return active_; }

private:
	struct Retrigger {
		InstrumentClip* clip;
		int32_t noteRowId;
		int64_t tick;
		uint8_t velocity;
		int8_t semitones;
	};
	struct Sounding {
		InstrumentClip* clip;
		int32_t noteRowId;
	};

	void processStep(int64_t swungTick, int32_t swungTickIncrement, int32_t step);
	void processClip(InstrumentClip* clip, int64_t stepTick, int32_t swungTickIncrement, int32_t step);
	void hit(InstrumentClip* clip, int32_t noteRowId, int32_t velocity, int32_t semitones);
	void addRatchet(InstrumentClip* clip, int32_t noteRowId, int64_t stepTick, int32_t numHits, int32_t velocity,
	                int32_t semitones);
	void stopSoundingHits();
	void setLed();
	uint32_t random();
	bool chance(int32_t permille) { return (int32_t)(random() % 1000) < permille; }

	int32_t fill_ = 2; // 0 to kNumFills - 1. Build, the one the module falls back to.
	bool held_ = false;
	bool pending_ = false;
	bool active_ = false;
	int32_t elapsed_ = 0;   // Fill steps played so far
	int32_t startStep_ = 0; // Step in the bar the fill started on
	uint32_t rng_ = 0x9E3779B9;
	Song* song_ = nullptr; // The song the lists below belong to

	static constexpr int32_t kMaxRetriggers = 48;
	std::array<Retrigger, kMaxRetriggers> retriggers_{};
	int32_t numRetriggers_ = 0;

	static constexpr int32_t kMaxSilenced = 32;
	std::array<Sounding, kMaxSilenced> silenced_{};
	int32_t numSilenced_ = 0;

	static constexpr int32_t kMaxSounding = 32;
	std::array<Sounding, kMaxSounding> sounding_{};
	int32_t numSounding_ = 0;
};

extern Autofill autofill;

} // namespace deluge::autofill
