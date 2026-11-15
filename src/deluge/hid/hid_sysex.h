#include "definitions_cxx.hpp"
#include "io/midi/midi_device_manager.h"
#include <string_view>

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
/// (repeat about once a second; sending stops 2 s after the last one), mode 2 <n> = choose picker option n,
/// mode 3 <hi> <lo> = move the preset browser to that item, mode 4 = press SELECT, mode 5 = press BACK.
/// Replies: 0x42 STATE, 0x43 KIT_ROWS, 0x44 TEXT, 0x45 PICKER (any open context menu), 0x46 BROWSER, 0x47 BROWSER_SEL.
void requestState(MIDICable& cable, uint8_t* data, int32_t len);
void sendStateIfChanged(bool force = false);
/// Copy display text for the companion screen (up to 40 characters) and find which l10n string it is, by pointer
void companionCopyText(char* dest, std::string_view text, int16_t& stringIndex);
/// Remember the OLED's popup text, for the companion screen in OLED mode
void companionNoteOledPopup(char const* text);
/// Run what the tablet tapped (picker option or browser action), from the COMPANION_PICK UI timer
void runPendingPick();
} // namespace HIDSysex
