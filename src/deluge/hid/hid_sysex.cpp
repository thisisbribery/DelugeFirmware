#include "hid/hid_sysex.h"
#include "dsp/drum_synth/drum_synth.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/ui.h"
#include "gui/ui_timer_manager.h"
#include "hid/display/oled.h"
#include "hid/display/seven_segment.h"
#include "io/midi/midi_device.h"
#include "io/midi/midi_engine.h"
#include "io/midi/sysex.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/clip.h"
#include "model/drum/drum.h"
#include "model/instrument/kit.h"
#include "model/song/song.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"
#include "util/pack.h"
#include <algorithm>
#include <cstring>

MIDICable* midiDisplayCable = nullptr;
int32_t midiDisplayUntil = 0;
uint8_t* oledDeltaImage = nullptr;
bool oledDeltaForce = true;

// Companion screen state (see hid_sysex.h). Kept apart from the display mirror so both can run at once.
namespace {
using deluge::dsp::drum_synth::Engine;

MIDICable* midiStateCable = nullptr;
uint32_t midiStateUntil = 0;
uint8_t stateSeq = 0;

constexpr int32_t kStateNameMax = 24;
constexpr int32_t kStateMaxRows = 64;
constexpr uint8_t kStateVersion = 0;
constexpr uint8_t kStateHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x42};
constexpr uint8_t kRowsHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x43};

// Engine codes: a drum_synth::Engine number (0 up, append-only, saved with sounds), or one of these
constexpr uint8_t kCodeTb303 = 0x7A;
constexpr uint8_t kCodeDelugeSynth = 0x7B; ///< Subtractive, FM or ringmod oscillators: synthMode says which
constexpr uint8_t kCodeDx7 = 0x7C;
constexpr uint8_t kCodeMidiOrGate = 0x7D;
constexpr uint8_t kCodeSample = 0x7E;
constexpr uint8_t kCodeNone = 0x7F;

// Last bytes sent, without the sequence number, to spot changes. Length 0 = nothing sent yet.
uint8_t lastState[96];
int32_t lastStateLen = 0;
uint8_t lastRows[kStateMaxRows + 2];
int32_t lastRowsLen = 0;

uint8_t engineCode(Sound* sound) {
	if (sound == nullptr) {
		return kCodeNone;
	}
	if (sound->getSynthMode() == SynthMode::TB303) {
		return kCodeTb303;
	}
	const Source& source = sound->sources[0];
	switch (source.oscType) {
	case OscType::DRUM_SYNTH:
		return static_cast<uint8_t>(source.drumPatch.engine()) & 0x7F;
	case OscType::DX7:
		return kCodeDx7;
	case OscType::SAMPLE:
		return kCodeSample;
	default:
		return kCodeDelugeSynth;
	}
}

uint8_t engineSubCode(Sound* sound) {
	if (sound == nullptr || sound->sources[0].oscType != OscType::DRUM_SYNTH) {
		return 0;
	}
	const auto& patch = sound->sources[0].drumPatch;
	if (patch.engine() == Engine::RINGS) {
		return patch.get(deluge::dsp::drum_synth::Patch::RINGS_MODEL) & 0x7F;
	}
	return 0;
}

uint8_t drumCode(Drum* drum) {
	if (drum == nullptr) {
		return kCodeNone;
	}
	if (drum->type != DrumType::SOUND) {
		return kCodeMidiOrGate;
	}
	return engineCode(static_cast<SoundDrum*>(drum));
}

/// Append a length byte and up to kStateNameMax printable ASCII characters
int32_t putName(uint8_t* out, char const* name) {
	int32_t n = 0;
	if (name != nullptr) {
		while (n < kStateNameMax && name[n] != '\0') {
			const char c = name[n];
			out[1 + n] = (c >= 0x20 && c <= 0x7E) ? c : '?';
			n++;
		}
	}
	out[0] = n;
	return n + 1;
}

/// The STATE body after the header and sequence byte. Returns its length.
int32_t buildState(uint8_t* out) {
	Clip* clip = (currentSong != nullptr) ? currentSong->getCurrentClip() : nullptr;
	Output* output = (clip != nullptr) ? clip->output : nullptr;

	Sound* sound = nullptr;
	Kit* kit = nullptr;
	Drum* drum = nullptr;
	if (output != nullptr && output->type == OutputType::SYNTH) {
		sound = static_cast<SoundInstrument*>(output);
	}
	else if (output != nullptr && output->type == OutputType::KIT) {
		kit = static_cast<Kit*>(output);
		drum = kit->selectedDrum;
		if (drum != nullptr && drum->type == DrumType::SOUND) {
			sound = static_cast<SoundDrum*>(drum);
		}
	}

	int32_t track = 0x3FFF;
	if (output != nullptr) {
		track = 0;
		for (Output* o = currentSong->firstOutput; o != nullptr && o != output; o = o->next) {
			track++;
		}
	}

	int32_t row = 0x7F;
	if (kit != nullptr && drum != nullptr) {
		row = 0;
		for (Drum* d = kit->firstDrum; d != nullptr && d != drum; d = d->next) {
			row++;
		}
		row = std::min<int32_t>(row, 0x7E);
	}

	int32_t i = 0;
	out[i++] = kStateVersion;
	out[i++] = (output != nullptr) ? (static_cast<uint8_t>(output->type) & 0x7F) : 0x7F;
	out[i++] = (sound != nullptr) ? (static_cast<uint8_t>(sound->getSynthMode()) & 0x7F) : 0x7F;
	out[i++] = (sound != nullptr) ? (static_cast<uint8_t>(sound->sources[0].oscType) & 0x7F) : 0x7F;
	out[i++] = (kit != nullptr) ? drumCode(drum) : engineCode(sound);
	out[i++] = engineSubCode(sound);
	out[i++] = (track >> 7) & 0x7F;
	out[i++] = track & 0x7F;
	out[i++] = row;
	i += putName(out + i, (output != nullptr) ? output->name.get() : nullptr);
	i += putName(out + i, (sound != nullptr && kit != nullptr) ? static_cast<SoundDrum*>(drum)->getName() : nullptr);
	return i;
}

/// The KIT_ROWS body: version, row count, one engine code per row. Returns 0 when not on a kit.
int32_t buildRows(uint8_t* out) {
	Clip* clip = (currentSong != nullptr) ? currentSong->getCurrentClip() : nullptr;
	Output* output = (clip != nullptr) ? clip->output : nullptr;
	if (output == nullptr || output->type != OutputType::KIT) {
		return 0;
	}
	int32_t n = 0;
	for (Drum* d = static_cast<Kit*>(output)->firstDrum; d != nullptr && n < kStateMaxRows; d = d->next) {
		out[2 + n++] = drumCode(d);
	}
	out[0] = kStateVersion;
	out[1] = n;
	return n + 2;
}

void sendFramed(MIDICable& cable, uint8_t const* header, bool withSeq, uint8_t const* body, int32_t bodyLen) {
	uint8_t msg[7 + 1 + 96 + 1];
	int32_t i = 0;
	memcpy(msg, header, 7);
	i += 7;
	if (withSeq) {
		msg[i++] = stateSeq;
		stateSeq = (stateSeq + 1) & 0x7F;
	}
	memcpy(msg + i, body, bodyLen);
	i += bodyLen;
	msg[i++] = 0xF7;
	cable.sendSysex(msg, i);
}
} // namespace

void HIDSysex::requestState(MIDICable& cable, uint8_t* data, int32_t len) {
	if (len < 3) {
		return;
	}
	const bool wasActive = midiStateCable == &cable && AudioEngine::audioSampleTimer <= midiStateUntil;
	midiStateCable = &cable;
	midiStateUntil = AudioEngine::audioSampleTimer + 2 * kSampleRate;
	// Mode 0, or a keepalive that starts a new subscription, sends everything
	sendStateIfChanged(data[2] == 0 || !wasActive);
}

void HIDSysex::sendStateIfChanged(bool force) {
	if (midiStateCable == nullptr || AudioEngine::audioSampleTimer > midiStateUntil) {
		return;
	}
	if (midiStateCable->sendBufferSpace() < 256) {
		uiTimerManager.setTimer(TimerName::SYSEX_DISPLAY, 100);
		return;
	}

	uint8_t body[96];
	const int32_t len = buildState(body);
	if (force || len != lastStateLen || memcmp(body, lastState, len) != 0) {
		sendFramed(*midiStateCable, kStateHeader, true, body, len);
		memcpy(lastState, body, len);
		lastStateLen = len;
	}

	uint8_t rows[kStateMaxRows + 2];
	const int32_t rowsLen = buildRows(rows);
	if (rowsLen > 0 && (force || rowsLen != lastRowsLen || memcmp(rows, lastRows, rowsLen) != 0)) {
		sendFramed(*midiStateCable, kRowsHeader, false, rows, rowsLen);
		memcpy(lastRows, rows, rowsLen);
	}
	lastRowsLen = rowsLen;
}

void HIDSysex::sysexReceived(MIDICable& cable, uint8_t* data, int32_t len) {
	if (len < 3) {
		return;
	}
	// first three bytes are already used, next is command
	switch (data[1]) {
	case 0:
		requestOLEDDisplay(cable, data, len);
		break;

	case 1:
		request7SegDisplay(cable, data, len);
		break;

	case 2:
		readBlock(cable);
		break;

	case 3:
		requestState(cable, data, len);
		break;

	default:
		break;
	}
}

void HIDSysex::requestOLEDDisplay(MIDICable& cable, uint8_t* data, int32_t len) {
	//	if (data[4] == 0 or data[4] == 1) {
	if (data[2] == 0 or data[2] == 1) {
		// sendOLEDData(device, (data[4] == 1)); // Adjustting by -2 to correct for payload offset.
		sendOLEDData(cable, (data[2] == 1));
	}
	// else if (data[4] == 2 || data[4] == 3) {
	else if (data[2] == 2 || data[2] == 3) {
		// bool force = (data[4] == 3);
		bool force = (data[2] == 3);
		midiDisplayCable = &cable;
		// two seconds
		midiDisplayUntil = AudioEngine::audioSampleTimer + 2 * kSampleRate;
		if (display->haveOLED()) {
			if (force) {
				oledDeltaForce = true;
			}

			if (oledDeltaImage == nullptr) {
				oledDeltaImage = (uint8_t*)GeneralMemoryAllocator::get().allocMaxSpeed(
				    sizeof(uint8_t[OLED_MAIN_HEIGHT_PIXELS >> 3][OLED_MAIN_WIDTH_PIXELS]));
			}
		}
		sendDisplayIfChanged();
		if (force && display->have7SEG()) {
			send7SegData(cable);
		}
	}
	// else if (data[4] == 4) { // SWAP
	else if (data[2] == 4) { // SWAP
		deluge::hid::display::swapDisplayType();
		oledDeltaForce = true;
	}
}

void HIDSysex::sendDisplayIfChanged() {
	// NB: timer is only used for throttling, under good conditions sending
	// is driven by the display subsystem only
	uiTimerManager.unsetTimer(TimerName::SYSEX_DISPLAY);
	// Every redraw passes through here, so this is where the companion screen hears about changes
	sendStateIfChanged();
	if (midiDisplayCable == nullptr || AudioEngine::audioSampleTimer > midiDisplayUntil) {
		return;
	}
	// not exact, but if more than half than the serial buffer is still full,
	// we need to slow down a little. (USB buffer is larger and should be consumed much quicker)
	if (midiDisplayCable->sendBufferSpace() < 512) {
		uiTimerManager.setTimer(TimerName::SYSEX_DISPLAY, 100);
		return;
	}

	if (display->haveOLED()) {
		sendOLEDDataDelta(*midiDisplayCable, false);
	}
	if (display->have7SEG()) {
		send7SegData(*midiDisplayCable);
	}
}

void HIDSysex::sendOLEDData(MIDICable& cable, bool rle) {
	if (display->haveOLED()) {
		const int32_t data_size = 768;
		const int32_t max_packed_size = 922;
		// 		uint8_t reply_hdr[5] = {0xf0, 0x7d, 0x02, 0x40, rle ? 0x01_u8 : 0x00_u8};
		uint8_t reply_hdr[8] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x40, rle ? 0x01_u8 : 0x00_u8};
		uint8_t* reply = midiEngine.sysex_fmt_buffer;
		// 		memcpy(reply, reply_hdr, 5);
		memcpy(reply, reply_hdr, 8); //
		                             //		reply[5] = 0; // nominally 32*data[5] is start pos for a delta
		reply[8] = 0;                // nominally 32*data[8] is start pos for a delta //

		int32_t packed;
		if (rle) {
			packed =
			    //				pack_8to7_rle(reply + 6, max_packed_size,
			    // deluge::hid::display::OLED::oledCurrentImage[0], data_size);
			    pack_8to7_rle(reply + 9, max_packed_size, deluge::hid::display::OLED::oledCurrentImage[0], data_size);
		}
		else {
			//			packed = pack_8bit_to_7bit(reply + 6, max_packed_size,
			// deluge::hid::display::OLED::oledCurrentImage[0], 			                           data_size);
			packed = pack_8bit_to_7bit(reply + 9, max_packed_size, deluge::hid::display::OLED::oledCurrentImage[0],
			                           data_size); //
		}
		if (packed < 0) {
			display->popupTextTemporary("error: fail");
		}
		//		reply[6 + packed] = 0xf7; // end of transmission
		reply[9 + packed] = 0xf7;            // end of transmission
		                                     // 		device->sendSysex(reply, packed + 7); //
		cable.sendSysex(reply, packed + 10); //
	}
}

void HIDSysex::request7SegDisplay(MIDICable& cable, uint8_t* data, int32_t len) {
	if (data[2] == 0) { // was 4
		send7SegData(cable);
	}
}

void HIDSysex::send7SegData(MIDICable& cable) {
	if (display->have7SEG()) {
		// aschually 8 segments if you count the dot
		auto data = display->getLast();
		const int32_t packed_data_size = 5;
		//		uint8_t reply[12] = {0xf0, 0x7d, 0x02, 0x41, 0x00, 0x00};
		uint8_t reply[15] = {0xf0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x41, 0x00, 0x00};
		//  	pack_8bit_to_7bit(reply + 6, packed_data_size, data.data(), data.size());
		// int32_t pack_8bit_to_7bit(uint8_t* dst, int32_t dst_size, uint8_t* src, int32_t src_len);
		pack_8bit_to_7bit(reply + 9, packed_data_size, data.data(), data.size());
		//		reply[6 + packed_data_size] = 0xf7; // end of transmission
		reply[9 + packed_data_size] = 0xf7; // end of transmission
		                                    //		device->sendSysex(reply, packed_data_size + 7);
		cable.sendSysex(reply, packed_data_size + 10);
	}
}

void HIDSysex::sendOLEDDataDelta(MIDICable& cable, bool force) {
	const int32_t data_size = 768;
	const int32_t max_packed_size = 922;

	uint8_t* current = deluge::hid::display::OLED::oledCurrentImage[0];

	int32_t first_change = 9000;
	int32_t last_change = 0;
	int32_t* blkdata_new = (int32_t*)current;
	int32_t* blkdata_old = (int32_t*)oledDeltaImage;

	const int32_t word_size = data_size >> 2;

	if (force || oledDeltaForce) {
		first_change = 0;
		last_change = word_size - 1;
	}
	else {
		for (int32_t blk = 0; blk < word_size; blk++) {
			if (blkdata_new[blk] != blkdata_old[blk]) {
				if (first_change > blk) {
					first_change = blk;
				}
				last_change = blk;
			}
		}
	}

	if (first_change > word_size) {
		return;
	}

	int start = first_change / 2;
	int len = (last_change / 2) - start + 1;
	//	int8_t reply_hdr[5] = {0xf0, 0x7d, 0x02, 0x40, 0x02};
	uint8_t reply_hdr[8] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x40, 0x02};
	uint8_t* reply = midiEngine.sysex_fmt_buffer;
	memcpy(reply, reply_hdr, sizeof(reply_hdr));
	//	reply[5] = start;
	reply[sizeof(reply_hdr) + 0] = start;
	//  reply[6] = len;
	reply[sizeof(reply_hdr) + 1] = len;
	// 	int32_t packed = pack_8to7_rle(reply + 7, max_packed_size, current + 8 * start, 8 * len);
	int32_t packed = pack_8to7_rle(reply + 10, max_packed_size, current + 8 * start, 8 * len);
	if (packed <= 0) {
		return;
	}
	memcpy(oledDeltaImage + (8 * start), current + (8 * start), 8 * len);
	oledDeltaForce = false;
	// reply[7 + packed] = 0xf7; // end of transmission
	reply[10 + packed] = 0xf7; // end of transmission //
	// device->sendSysex(reply, packed + 8);
	cable.sendSysex(reply, packed + 11);
}

void HIDSysex::readBlock(MIDICable& cable) {
	const int32_t data_size = 768;
	const int32_t max_packed_size = 922;
	uint8_t reply_hdr[8] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x40, 0x00};
	uint8_t* reply = midiEngine.sysex_fmt_buffer;
	// 		memcpy(reply, reply_hdr, 5);
	memcpy(reply, reply_hdr, 8); //
	                             //		reply[5] = 0; // nominally 32*data[5] is start pos for a delta
	reply[8] = 0;                // nominally 32*data[8] is start pos for a delta //

	int32_t packed;

	//			packed = pack_8bit_to_7bit(reply + 6, max_packed_size,
	// deluge::hid::display::OLED::oledCurrentImage[0],
	uint8_t* srcBlock = (uint8_t*)smDeserializer.fileClusterBuffer; //  deluge::hid::display::OLED::oledCurrentImage[0];

	packed = pack_8bit_to_7bit(reply + 9, max_packed_size, srcBlock,
	                           data_size); //
	if (packed < 0) {
		display->popupTextTemporary("error: fail");
	}
	//		reply[6 + packed] = 0xf7; // end of transmission
	reply[9 + packed] = 0xf7;            // end of transmission
	                                     // 		device->sendSysex(reply, packed + 7); //
	cable.sendSysex(reply, packed + 10); //
}
