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

#include "playback/autofill.h"
#include "hid/display/display.h"
#include "hid/led/indicator_leds.h"
#include "model/clip/instrument_clip.h"
#include "model/model_stack.h"
#include "model/note/note.h"
#include "model/note/note_row.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/clip_iterators.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include <algorithm>
#include <limits>

namespace deluge::autofill {

Autofill autofill{};

namespace {

constexpr int32_t kStepsPerBar = 16;
constexpr int32_t kNumTracks = 3; // Kick, Snare and Hihat 1: the bottom three rows of a kit
constexpr int32_t kKick = 0;

constexpr char const* kFillNames[kNumFills] = {"Ghost", "Drive", "Build", "Climb", "Stutter"};
constexpr char const* kFillShortNames[kNumFills] = {"GHST", "DRIV", "BILD", "CLMB", "STUT"};

int32_t velocityFor(int32_t permille) {
	return std::clamp<int32_t>(127 * permille / 1000, 1, 127);
}

} // namespace

bool Autofill::isChosenInSettings() {
	return runtimeFeatureSettings.get(RuntimeFeatureSettingType::TapTempoAction)
	       == RuntimeFeatureStateTapTempoAction::AutoFill;
}

uint32_t Autofill::random() {
	// xorshift32, as on the module
	rng_ ^= rng_ << 13;
	rng_ ^= rng_ >> 17;
	rng_ ^= rng_ << 5;
	return rng_;
}

void Autofill::tapPressed() {
	held_ = true;
	if (playbackHandler.isEitherClockActive() && !active_) {
		pending_ = true; // Starts on the next 16th
		playbackHandler.expectEvent();
	}
	displayFill();
}

void Autofill::tapReleased() {
	held_ = false;
}

void Autofill::changeFill(int32_t offset) {
	// Turning TEMPO while holding TAP is for picking a fill, not playing one
	pending_ = false;
	if (active_) {
		active_ = false;
		numRetriggers_ = 0;
		setLed();
	}
	fill_ = (fill_ + offset % kNumFills + kNumFills) % kNumFills;
	displayFill();
}

void Autofill::displayFill() {
	if (display->haveOLED()) {
		char text[32];
		snprintf(text, sizeof(text), "Fill %d: %s", (int)fill_ + 1, kFillNames[fill_]);
		display->popupTextTemporary(text);
	}
	else {
		display->displayPopup(kFillShortNames[fill_]);
	}
}

void Autofill::setLed() {
	indicator_leds::setLedState(IndicatorLED::TAP_TEMPO, active_ || playbackHandler.metronomeOn);
}

void Autofill::playbackStopped() {
	pending_ = false;
	active_ = false;
	numRetriggers_ = 0;
	numSilenced_ = 0;
	numSounding_ = 0;
	setLed();
}

bool Autofill::shouldSilence(InstrumentClip const* clip, int32_t noteRowId) const {
	for (int32_t i = 0; i < numSilenced_; i++) {
		if (silenced_[i].clip == clip && silenced_[i].noteRowId == noteRowId) {
			return true;
		}
	}
	return false;
}

int32_t Autofill::processTick(int64_t swungTick, int32_t swungTickIncrement) {
	// After a song swap, the clips in the lists are gone
	if (song_ != currentSong) {
		song_ = currentSong;
		numRetriggers_ = 0;
		numSounding_ = 0;
		numSilenced_ = 0;
	}
	if (!active_ && !pending_ && !numRetriggers_ && !numSounding_) {
		return std::numeric_limits<int32_t>::max();
	}
	int32_t sixteenth = currentSong->getSixteenthNoteLength();

	// Ratchet hits that are due
	for (int32_t i = 0; i < numRetriggers_; i++) {
		if (retriggers_[i].tick <= swungTick) {
			hit(retriggers_[i].clip, retriggers_[i].noteRowId, retriggers_[i].velocity, retriggers_[i].semitones);
			retriggers_[i--] = retriggers_[--numRetriggers_];
		}
	}

	if (swungTick % sixteenth == 0) {
		int32_t step = (swungTick / sixteenth) % kStepsPerBar;

		// A fill runs to the end of the bar, and on into the next while TAP is held
		if (active_ && step == 0 && !held_) {
			active_ = false;
			setLed();
		}
		if (pending_) {
			pending_ = false;
			active_ = true;
			elapsed_ = 0;
			startStep_ = step;
			rng_ ^= AudioEngine::audioSampleTimer;
			if (!rng_) {
				rng_ = 0x9E3779B9;
			}
			setLed();
		}

		stopSoundingHits();
		if (active_) {
			processStep(swungTick, swungTickIncrement, step);
			elapsed_++;
		}
	}

	if (!active_ && !pending_ && !numRetriggers_ && !numSounding_) {
		return std::numeric_limits<int32_t>::max();
	}
	int64_t nextTick = swungTick - swungTick % sixteenth + sixteenth;
	for (int32_t i = 0; i < numRetriggers_; i++) {
		nextTick = std::min(nextTick, retriggers_[i].tick);
	}
	return (int32_t)(nextTick - swungTick);
}

void Autofill::processStep(int64_t swungTick, int32_t swungTickIncrement, int32_t step) {
	for (Clip* clip : AllClips::everywhere(currentSong)) {
		if (clip->type == ClipType::INSTRUMENT && clip->output->type == OutputType::KIT
		    && currentSong->isClipActive(clip)) {
			processClip(static_cast<InstrumentClip*>(clip), swungTick, swungTickIncrement, step);
		}
	}
}

void Autofill::processClip(InstrumentClip* clip, int64_t stepTick, int32_t swungTickIncrement, int32_t s) {
	int32_t sixteenth = currentSong->getSixteenthNoteLength();
	int32_t barLength = sixteenth * kStepsPerBar;

	// What the pattern does on each track at this step
	struct Track {
		NoteRow* noteRow = nullptr;
		bool canSound = false;
		bool busy = false;      // The pattern plays this track here
		int32_t velocity = 127; // The pattern's velocity here, when busy
		int32_t pos = 0;        // This track's position in its own loop
		int32_t length = 1;
	};
	std::array<Track, kNumTracks> tracks{};
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);
	int32_t numTracks = std::min(kNumTracks, clip->noteRows.getNumElements());
	for (int32_t t = 0; t < numTracks; t++) {
		Track& track = tracks[t];
		track.noteRow = clip->noteRows.getElement(t);
		ModelStackWithNoteRow* modelStackWithNoteRow = modelStack->addNoteRow(t, track.noteRow);
		track.length = std::max(modelStackWithNoteRow->getLoopLength(), 1_i32);
		// The clips haven't moved on to this tick yet
		track.pos = (modelStackWithNoteRow->getLastProcessedPos() + swungTickIncrement) % track.length;
		if (track.pos < 0) {
			track.pos += track.length;
		}
		track.canSound = track.noteRow->drum && !track.noteRow->muted;
		int32_t i = track.noteRow->notes.search(track.pos, GREATER_OR_EQUAL);
		if (i < track.noteRow->notes.getNumElements()) {
			Note* note = track.noteRow->notes.getElement(i);
			if (note->pos < track.pos + sixteenth) {
				track.busy = true;
				track.velocity = note->velocity;
			}
		}
	}

	// Plays a fill hit, returning its velocity, or 0 if the track can't sound
	auto fillHit = [&](int32_t t, int32_t permille, int32_t semitones = 0) -> int32_t {
		if (t >= numTracks || !tracks[t].canSound) {
			return 0;
		}
		int32_t velocity = velocityFor(permille);
		hit(clip, t, velocity, semitones);
		return velocity;
	};
	auto busy = [&](int32_t t) { return t < numTracks && tracks[t].busy; };
	auto ratchet = [&](int32_t t, int32_t numHits, int32_t velocity, int32_t semitones = 0) {
		if (t < numTracks && tracks[t].canSound) {
			addRatchet(clip, t, stepTick, numHits, velocity, semitones);
		}
	};
	auto silence = [&](int32_t t) {
		if (busy(t) && numSilenced_ < kMaxSilenced) {
			silenced_[numSilenced_++] = {clip, t};
		}
	};

	switch (fill_) {
	case 0: { // Ghost: quiet snare and hat notes on the off 16ths
		for (int32_t t : {1, 2}) {
			if (!busy(t) && (s & 1) && chance(350)) {
				fillHit(t, 250 + random() % 201);
			}
		}
		break;
	}
	case 1: { // Drive: 16th hats, snare pickups and a kick at the end of the bar
		if (!busy(2)) {
			fillHit(2, (s & 1) ? 550 : 800);
		}
		if (!busy(1) && (s == 8 || s == 11 || s == 14) && chance(850)) {
			fillHit(1, 700);
		}
		if (!busy(kKick) && ((s == 15 && chance(900)) || (s == 14 && chance(400)))) {
			fillHit(kKick, 800);
		}
		break;
	}
	case 2: { // Build: random hits, more likely as the bar goes on, with some snare and hat ratchets
		int32_t base = 333 + 333 * s / 15;
		int32_t factor = (s % 4 == 0) ? 500 : ((s & 1) ? 1300 : 1000);
		int32_t chanceSnareHat = base * factor / 1000;
		int32_t chanceKick = base * factor / 2000;
		for (int32_t t = 0; t < kNumTracks; t++) {
			int32_t velocity = busy(t) ? tracks[t].velocity : 0;
			if (!velocity && chance(t == kKick ? chanceKick : chanceSnareHat)) {
				velocity = fillHit(t, 600 + random() % 301);
			}
			if (velocity && t != kKick && chance(250)) {
				ratchet(t, 2 + (random() & 1), velocity);
			}
		}
		break;
	}
	case 3: { // Climb: odds and velocity rise steeply, with more ratchets late in the bar
		int32_t odds = (1000 * s / 15) * 750 / 1000 + 200;
		int32_t permille = 500 + 1000 * s / 30;
		// The snare steps up in pitch each beat, on every snare hit in the step
		constexpr int32_t kSnarePitch[4] = {0, 3, 5, 7};
		int32_t snarePitch = kSnarePitch[s >> 2];
		for (int32_t t = 0; t < kNumTracks; t++) {
			int32_t semitones = (t == 1) ? snarePitch : 0;
			int32_t velocity = busy(t) ? tracks[t].velocity : 0;
			if (velocity && semitones && tracks[t].canSound) {
				// The pattern's own snare is played again by the fill, at its own velocity, so it can be pitched
				silence(t);
				hit(clip, t, velocity, semitones);
			}
			if (!velocity && chance(t == kKick ? odds / 2 : odds)) {
				velocity = fillHit(t, permille, semitones);
			}
			if (velocity && t != kKick) {
				if (s <= 5) {
					if (chance(300)) {
						ratchet(t, 2, velocity, semitones);
					}
				}
				else if (s <= 11) {
					if (chance(500)) {
						ratchet(t, 2 + (random() & 1), velocity, semitones);
					}
				}
				else if (chance(800)) {
					ratchet(t, 3 + (random() & 1), velocity, semitones);
				}
			}
		}
		break;
	}
	default: { // Stutter: repeats the live pattern in shrinking loops of 4, 2 and then 1 step
		int32_t e = elapsed_;
		int32_t loopLength = ((e & 12) == 0) ? 4 : ((e & 8) ? 1 : 2);
		int32_t source = (startStep_ + (e & 15) % loopLength) & 15;
		for (int32_t t = 0; t < kNumTracks; t++) {
			bool sourceHit = false;
			if (t < numTracks) {
				Track& track = tracks[t];
				int32_t sourcePos = (track.pos / barLength * barLength + source * sixteenth) % track.length;
				int32_t i = track.noteRow->notes.search(sourcePos, GREATER_OR_EQUAL);
				sourceHit = i < track.noteRow->notes.getNumElements()
				            && track.noteRow->notes.getElement(i)->pos < sourcePos + sixteenth;
			}
			int32_t chanceAdd = (t == kKick) ? 150 : 400;
			bool play = sourceHit ? (!chance(150) || chance(chanceAdd)) : chance(chanceAdd);
			// The fill decides this step completely, so the pattern's own hit goes either way
			silence(t);
			if (play) {
				// Drawn in the module's order, so the fills come out the same
				int32_t permille = 500 + random() % 501;
				int32_t numHits = chance(t == kKick ? 300 : 600) ? 2 + random() % 3 : 0;
				int32_t semitones = 0;
				if (chance(t == kKick ? 150 : 300)) {
					semitones = chance(500) ? 12 : -12;
				}
				int32_t velocity = fillHit(t, permille, semitones);
				if (numHits) {
					ratchet(t, numHits, velocity, semitones);
				}
			}
		}
		break;
	}
	}
}

void Autofill::addRatchet(InstrumentClip* clip, int32_t noteRowId, int64_t stepTick, int32_t numHits, int32_t velocity,
                          int32_t semitones) {
	// The extra hits split the step evenly at three quarters of the velocity. The step is in swung ticks, so they
	// follow the swing.
	int32_t sixteenth = currentSong->getSixteenthNoteLength();
	uint8_t retriggerVelocity = std::clamp<int32_t>(velocity * 3 / 4, 1, 127);
	for (int32_t k = 1; k < numHits && numRetriggers_ < kMaxRetriggers; k++) {
		retriggers_[numRetriggers_++] = {clip, noteRowId, stepTick + k * sixteenth / numHits, retriggerVelocity,
		                                 (int8_t)semitones};
	}
}

void Autofill::hit(InstrumentClip* clip, int32_t noteRowId, int32_t velocity, int32_t semitones) {
	if (velocity <= 0 || noteRowId >= clip->noteRows.getNumElements()) {
		return;
	}
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip);

	// Retriggering a row autofill is already sounding ends that note first
	for (int32_t i = 0; i < numSounding_; i++) {
		if (sounding_[i].clip == clip && sounding_[i].noteRowId == noteRowId) {
			clip->stopAutofillHit(modelStack, noteRowId);
			sounding_[i--] = sounding_[--numSounding_];
		}
	}
	clip->playAutofillHit(modelStack, noteRowId, velocity, semitones);
	if (numSounding_ < kMaxSounding) {
		sounding_[numSounding_++] = {clip, noteRowId};
	}
}

void Autofill::stopSoundingHits() {
	for (int32_t i = 0; i < numSounding_; i++) {
		InstrumentClip* clip = sounding_[i].clip;
		if (sounding_[i].noteRowId < clip->noteRows.getNumElements()) {
			char modelStackMemory[MODEL_STACK_MAX_SIZE];
			clip->stopAutofillHit(setupModelStackWithTimelineCounter(modelStackMemory, currentSong, clip),
			                      sounding_[i].noteRowId);
		}
	}
	numSounding_ = 0;
}

} // namespace deluge::autofill
