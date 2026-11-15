#include "definitions_cxx.hpp"
#include "io/midi/midi_device_manager.h"

namespace HIDSysex {
void requestOLEDDisplay(MIDICable& cable, uint8_t* data, int32_t len);
void request7SegDisplay(MIDICable& cable, uint8_t* data, int32_t len);
void sysexReceived(MIDICable& cable, uint8_t* data, int32_t len);
void sendOLEDData(MIDICable& cable, bool rle);
void sendOLEDDataDelta(MIDICable& cable, bool force);
void send7SegData(MIDICable& cable);
void sendDisplayIfChanged();
void readBlock(MIDICable& cable);
/// Companion screen: what's selected (track, engine, preset, kit row), sent as small messages when it changes.
/// Host sends F0 00 21 7B 01 02 03 <mode> F7: mode 0 = subscribe and send everything now, mode 1 = keep alive
/// (repeat about once a second; sending stops 2 s after the last one). Replies are 0x42 STATE and 0x43 KIT_ROWS.
void requestState(MIDICable& cable, uint8_t* data, int32_t len);
void sendStateIfChanged(bool force = false);
} // namespace HIDSysex
