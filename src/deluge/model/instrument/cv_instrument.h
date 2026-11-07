/*
 * Copyright © 2018-2023 Synthstrom Audible Limited
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

#include "model/instrument/cv_instrument.h"
#include "model/instrument/non_audio_instrument.h"

class ParamManagerForTimeline;
class ModelStackWithThreeMainThings;
class ModelStackWithSoundFlags;
/// lock: CV2 follows the gold knobs in semitones (1 V per octave, 0 V with the knobs at zero), so holding a step and
/// turning a knob locks CV2 for that step
enum class CVMode : uint8_t { off, pitch, mod, aftertouch, velocity, lock };
enum class GateMode : uint8_t { off, gate, trigger };
enum CVInstrumentMode { one, two, both };
constexpr int32_t kNumCVInstrumentChannels = both + 1;

class CVInstrument final : public NonAudioInstrument {
public:
	CVInstrument();
	/// Mono CV: the next note-on is shifted by octaves, or sends customMillivolts (if not -1) plus 1 V per octave
	void setMonoPitchForNextNote(int32_t octave, int32_t customMillivolts) {
		monoPitchPending = true;
		monoOctave = octave;
		monoCustomMillivolts = customMillivolts;
	}
	/// Mono CV CUSTOM: sets the pitch CV straight away, while its row is auditioned and its voltage turned
	void sendMonoCustomVoltage(int32_t millivolts);
	static int32_t millivoltsToCV(int32_t millivolts) { return millivolts * 6552 / 1000; }

	void noteOnPostArp(int32_t noteCodePostArp, ArpNote* arpNote, int32_t noteIndex) override;
	void noteOffPostArp(int32_t noteCode, int32_t oldMIDIChannel, int32_t velocity, int32_t noteIndex) override;
	void polyphonicExpressionEventPostArpeggiator(int32_t newValue, int32_t noteCodeAfterArpeggiation,
	                                              int32_t expressionDmiension, ArpNote* arpNote,
	                                              int32_t noteIndex) override;
	bool writeDataToFile(Serializer& writer, Clip* clipForSavingOutputOnly, Song* song) override;
	bool readTagFromFile(Deserializer& reader, const char* tagName) override;
	void monophonicExpressionEvent(int32_t newValue, int32_t expressionDmiension) override;
	bool setActiveClip(ModelStackWithTimelineCounter* modelStack, PgmChangeSend maySendMIDIPGMs) override;
	void setupWithoutActiveClip(ModelStack* modelStack) override;
	static int32_t navigateChannels(int oldChannel, int offset) {
		auto newChannel = (oldChannel + offset) % kNumCVInstrumentChannels;
		if (newChannel == -1) {
			newChannel = kNumCVInstrumentChannels - 1;
		}
		return newChannel;
	}
	bool matchesPreset(OutputType otherType, int32_t otherChannel, int32_t channelSuffix, char const* otherName,
	                   char const* otherPath) override {
		bool match{false};
		if (type == otherType) {
			auto ourChannel = getChannel();
			match = ourChannel == otherChannel || ourChannel == both || otherChannel == both; // 3 means both
		}
		return match;
	}

	// It's much easier to store local copies of the most recent of these, so we never have to go doing complex quizzing
	// of the arp, or MPE params, which we otherwise would have to do regularly.
	int32_t monophonicPitchBendValue;
	int32_t polyPitchBendValue;

	char const* getXMLTag() override { return "cvChannel"; }
	void setChannel(int channel) override {
		if (channel <= both) {
			NonAudioInstrument::setChannel(channel);
			setMode(static_cast<CVInstrumentMode>(channel));
		}
		else {
			NonAudioInstrument::setChannel(0);
			setMode(static_cast<CVInstrumentMode>(0));
		}
	}
	CVMode getCV2Mode() { return cvmode[1]; }

	/// CV2 LOCK: both gold knobs turn the Y expression param, which CV2 follows in semitones
	ModelStackWithAutoParam* getParamFromModEncoder(int32_t whichModEncoder, ModelStackWithThreeMainThings* modelStack,
	                                                bool allowCreation = true) override;

	void setCV2Mode(CVMode mode);

private:
	void updatePitchBendOutput(bool outputToo = true);
	// returns -1 if  no pitch
	int32_t getPitchChannel() {
		auto c = getChannel();
		if (c > 1) {
			return 0;
		}
		return c;
	}
	void setMode(CVInstrumentMode channel) {
		clearModes();
		switch (channel) {
		case CVInstrumentMode::one: {
			gateMode[0] = GateMode::gate;
			cvmode[0] = CVMode::pitch;
		}
		case CVInstrumentMode::two: {
			gateMode[1] = GateMode::gate;
			cvmode[1] = CVMode::pitch;
		}
		case CVInstrumentMode::both: {
			gateMode[0] = GateMode::gate;
			gateMode[1] = GateMode::trigger;
			cvmode[0] = CVMode::pitch;
			if (cvmode[1] == CVMode::off) {
				cvmode[1] = CVMode::aftertouch;
			}
		}
		}
	}
	void clearModes() {
		gateMode[0] = GateMode::off;
		gateMode[1] = GateMode::off;
		cvmode[0] = CVMode::off;
		cvmode[1] = CVMode::off;
	}

	GateMode gateMode[2]{GateMode::off, GateMode::off};
	CVMode cvmode[2]{CVMode::off, CVMode::off};
	void sendMonophonicExpressionEvent(int32_t dimension);

private:
	bool monoPitchPending = false;
	int32_t monoOctave = 0;
	int32_t monoCustomMillivolts = -1;
};
