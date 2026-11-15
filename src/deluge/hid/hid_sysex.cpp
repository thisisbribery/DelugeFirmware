#include "hid/hid_sysex.h"
#include "dsp/drum_synth/drum_synth.h"
#include "extern.h"
#include "gui/context_menu/context_menu.h"
#include "gui/l10n/l10n.h"
#include "gui/l10n/language.h"
#include "gui/menu_item/menu_item.h"
#include "gui/ui/browser/browser.h"
#include "gui/ui/sound_editor.h"
#include "gui/ui/ui.h"
#include "gui/ui_timer_manager.h"
#include "hid/button.h"
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
#include "storage/file_item.h"
#include "util/d_stringbuf.h"
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
constexpr uint8_t kTextHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x44};
constexpr int32_t kTextMax = 40;
constexpr uint8_t kPickerHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x45};
constexpr int32_t kPickerMaxOptions = 32;
constexpr int32_t kPickerNameMax = 20;
constexpr uint8_t kBrowserHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x46};
constexpr uint8_t kBrowserSelHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x47};
constexpr int32_t kBrowserWindow = 48; ///< Items sent around the selected one
constexpr int32_t kBrowserNameMax = 24;

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
uint8_t lastText[5 * (kTextMax + 1) + 2];
int32_t lastTextLen = 0;
char oledPopupText[kTextMax + 1] = {}; ///< The OLED's popup, in OLED mode

using deluge::l10n::kNumStrings;
namespace l10n_built_in = deluge::l10n::built_in;

/// Which l10n string this text is, only when that's certain, else -1 (the text is then shown as it is, never
/// guessed). Certain means: a recent lookup returned exactly this text for that string (identical texts share
/// storage, so the lookup says which one was meant), or, with scanUnique, only one string uses this storage.
int16_t resolveString(std::string_view text, bool scanUnique) {
	if (text.empty()) {
		return -1;
	}
	const int32_t recent = deluge::l10n::recentLookupOf(text);
	if (recent >= 0) {
		return recent;
	}
	if (!scanUnique) {
		return -1;
	}
	int16_t found = -1;
	for (size_t i = 1; i < kNumStrings; i++) {
		const auto id = static_cast<deluge::l10n::String>(i);
		const auto seg = l10n_built_in::seven_segment.get(id);
		const auto eng = l10n_built_in::english.get(id);
		const bool same = (seg.has_value() && seg->data() == text.data() && seg->size() == text.size())
		                  || (eng.has_value() && eng->data() == text.data() && eng->size() == text.size());
		if (same) {
			if (found >= 0) {
				return -1; // Shared by two strings: can't be sure which
			}
			found = i;
		}
	}
	return found;
}

std::string_view englishFor(std::string_view text, int16_t stringIndex) {
	if (stringIndex <= 0) {
		return text;
	}
	return deluge::l10n::getView(l10n_built_in::english, static_cast<deluge::l10n::String>(stringIndex));
}

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

/// Append a length byte and up to kTextMax printable ASCII characters
int32_t putText(uint8_t* out, std::string_view text) {
	int32_t n = 0;
	for (; n < kTextMax && n < static_cast<int32_t>(text.size()) && text[n] != '\0'; n++) {
		const char c = text[n];
		out[1 + n] = (c >= 0x20 && c <= 0x7E) ? c : '?';
	}
	out[0] = n;
	return n + 1;
}

/// The TEXT body: version, flags (1 = popup showing), then length-prefixed main text as shown, its English, popup
/// text, its English, and the English name of the menu item being edited
int32_t buildText(uint8_t* out) {
	int16_t mainIndex = -1;
	int16_t popupIndex = -1;
	std::string_view mainText;
	std::string_view popupText;
	DEF_STACK_STRING_BUF(oledValue, kTextMax + 1);
	if (display->have7SEG()) {
		auto* seg = static_cast<deluge::hid::display::SevenSegment*>(display);
		mainText = seg->companionMainText(mainIndex);
		popupText = seg->companionPopupText(popupIndex);
	}
	else {
		// OLED mode: its text is English already. The main text is the value of the menu item being edited.
		if (getCurrentUI() == &soundEditor && soundEditor.getCurrentMenuItem() != nullptr) {
			soundEditor.getCurrentMenuItem()->getNotificationValue(oledValue);
			mainText = oledValue.c_str();
		}
		if (display->hasPopup()) {
			popupText = oledPopupText;
		}
	}

	std::string_view menuName;
	if (getCurrentUI() == &soundEditor) {
		MenuItem* item = soundEditor.getCurrentMenuItem();
		if (item != nullptr) {
			// Cached per item and name, as this runs on every redraw
			static MenuItem* cachedItem = nullptr;
			static char const* cachedName = nullptr;
			static int16_t cachedIndex = -1;
			menuName = item->getName();
			if (item != cachedItem || menuName.data() != cachedName) {
				cachedItem = item;
				cachedName = menuName.data();
				// The item's own string when that's the name it shows, else only if certain
				const auto own = deluge::l10n::getView(*deluge::l10n::chosenLanguage, item->name);
				cachedIndex =
				    (own.data() == menuName.data()) ? util::to_underlying(item->name) : resolveString(menuName, true);
			}
			menuName = englishFor(menuName, cachedIndex);
		}
	}

	int32_t i = 0;
	out[i++] = kStateVersion;
	out[i++] = popupText.empty() ? 0 : 1;
	i += putText(out + i, mainText);
	i += putText(out + i, englishFor(mainText, mainIndex));
	i += putText(out + i, popupText);
	i += putText(out + i, englishFor(popupText, popupIndex));
	i += putText(out + i, menuName);
	return i;
}

// Picker (any context menu): last one sent, to send only on change. Static so the big buffers stay off the stack.
void const* pickerKeyMenu = nullptr;
int32_t pickerKeyOption = -1;
char const* const* pickerKeyOptions = nullptr;
size_t pickerKeyCount = 0;
bool pickerWasOpen = false;
uint8_t pickerBody[4 + (kPickerNameMax + 1) * (kPickerMaxOptions + 1)];
int32_t pickerBodyLen = 0;
int32_t pendingPick = -1; ///< Option the tablet tapped, chosen from the UI timer

/// Append a length byte and the English of a context menu text, up to kPickerNameMax characters
int32_t putEnglish(uint8_t* out, char const* text, int32_t max) {
	if (text == nullptr) {
		out[0] = 0;
		return 1;
	}
	const std::string_view view(text);
	const std::string_view english = englishFor(view, resolveString(view, true));
	int32_t n = 0;
	for (; n < max && n < static_cast<int32_t>(english.size()); n++) {
		const char c = english[n];
		out[1 + n] = (c >= 0x20 && c <= 0x7E) ? c : '?';
	}
	out[0] = n;
	return n + 1;
}

/// The open context menu, or nullptr
deluge::gui::ContextMenu* openPicker() {
	UI* ui = getCurrentUI();
	if (ui == nullptr || ui->getUIType() != UIType::CONTEXT_MENU) {
		return nullptr;
	}
	return static_cast<deluge::gui::ContextMenu*>(ui);
}

/// Rebuild the PICKER body if the menu, its options or the highlighted one changed. Returns whether it changed.
/// Body: version, open (0/1), highlighted option, option count, title, then each option, all English.
bool updatePicker() {
	deluge::gui::ContextMenu* menu = openPicker();
	if (menu == nullptr) {
		if (!pickerWasOpen) {
			return false;
		}
		pickerWasOpen = false;
		pickerKeyMenu = nullptr;
		pickerBody[0] = kStateVersion;
		pickerBody[1] = 0;
		pickerBody[2] = 0;
		pickerBody[3] = 0;
		pickerBody[4] = 0;
		pickerBodyLen = 5;
		return true;
	}
	const auto options = menu->getOptions();
	const bool sameList = pickerWasOpen && menu == pickerKeyMenu && options.data() == pickerKeyOptions
	                      && options.size() == pickerKeyCount;
	if (sameList) {
		// Same list: only the highlight can have moved. Translating the list again would cost a few ms per turn.
		if (menu->currentOption == pickerKeyOption) {
			return false;
		}
		pickerKeyOption = menu->currentOption;
		pickerBody[2] = std::clamp<int32_t>(menu->currentOption, 0, 0x7F);
		return true;
	}
	pickerWasOpen = true;
	pickerKeyMenu = menu;
	pickerKeyOption = menu->currentOption;
	pickerKeyOptions = options.data();
	pickerKeyCount = options.size();

	const int32_t count = std::min<int32_t>(options.size(), kPickerMaxOptions);
	int32_t i = 0;
	pickerBody[i++] = kStateVersion;
	pickerBody[i++] = 1;
	pickerBody[i++] = std::clamp<int32_t>(menu->currentOption, 0, 0x7F);
	pickerBody[i++] = count;
	i += putEnglish(pickerBody + i, menu->getTitle(), kPickerNameMax);
	for (int32_t o = 0; o < count; o++) {
		i += putEnglish(pickerBody + i, options[o], kPickerNameMax);
	}
	pickerBodyLen = i;
	return true;
}

// Browser (preset, song, pattern and sample browsers): the window of names around the selected item
uint8_t browserBody[8 + (kBrowserNameMax + 1) + (kBrowserNameMax + 2) * kBrowserWindow];
int32_t browserBodyLen = 0;
bool browserWasOpen = false;
uint32_t browserKeyDir = 0;     ///< Hash of the folder path
int32_t browserKeyCount = -1;   ///< Items loaded
int32_t browserKeyDeleted = -1; ///< Items before them not loaded
int32_t browserKeyStart = -1;   ///< Absolute index of the first item sent
int32_t browserKeyCountSent = 0;
int32_t browserKeySelected = -2;

bool browserIsOpen() {
	UI* ui = getCurrentUI();
	if (ui == nullptr) {
		return false;
	}
	switch (ui->getUIType()) {
	case UIType::LOAD_INSTRUMENT_PRESET:
	case UIType::LOAD_SONG:
	case UIType::LOAD_PATTERN:
	case UIType::SAMPLE_BROWSER:
		return true;
	default:
		return false;
	}
}

uint32_t hashText(char const* text) {
	uint32_t h = 2166136261u;
	for (; text != nullptr && *text != '\0'; text++) {
		h = (h ^ static_cast<uint8_t>(*text)) * 16777619u;
	}
	return h;
}

/// What to send for the browser: 0 = nothing, 1 = the whole window (BROWSER), 2 = only the selection (BROWSER_SEL).
/// BROWSER body: version, open, base (2 bytes, absolute index of the first item), selected (2 bytes, absolute, 0x3FFF
/// if none), count, folder name, then per item a flags byte (1 = folder) and its name.
int32_t updateBrowser() {
	if (!browserIsOpen()) {
		if (!browserWasOpen) {
			return 0;
		}
		browserWasOpen = false;
		browserKeyCount = -1;
		int32_t i = 0;
		browserBody[i++] = kStateVersion;
		browserBody[i++] = 0;
		browserBody[i++] = 0;
		browserBody[i++] = 0;
		browserBody[i++] = 0x7F;
		browserBody[i++] = 0x7F;
		browserBody[i++] = 0;
		browserBody[i++] = 0;
		browserBodyLen = i;
		return 1;
	}
	const int32_t count = Browser::fileItems.getNumElements();
	const int32_t deleted = Browser::numFileItemsDeletedAtStart;
	const int32_t selectedLocal = Browser::getFileIndexSelected();
	const int32_t selected = (selectedLocal >= 0) ? deleted + selectedLocal : -1;
	const uint32_t dir = hashText(Browser::currentDir.get());

	const bool sameList =
	    browserWasOpen && dir == browserKeyDir && count == browserKeyCount && deleted == browserKeyDeleted;
	const bool inWindow =
	    selected < 0 || (selected >= browserKeyStart && selected < browserKeyStart + browserKeyCountSent);
	if (sameList && inWindow) {
		if (selected == browserKeySelected) {
			return 0;
		}
		browserKeySelected = selected;
		return 2;
	}

	browserWasOpen = true;
	browserKeyDir = dir;
	browserKeyCount = count;
	browserKeyDeleted = deleted;
	browserKeySelected = selected;
	int32_t startLocal = std::max<int32_t>(0, (selectedLocal >= 0 ? selectedLocal : 0) - kBrowserWindow / 2);
	startLocal = std::max<int32_t>(0, std::min<int32_t>(startLocal, count - kBrowserWindow));
	const int32_t sent = std::min<int32_t>(kBrowserWindow, count - startLocal);
	browserKeyStart = deleted + startLocal;
	browserKeyCountSent = sent;

	int32_t i = 0;
	browserBody[i++] = kStateVersion;
	browserBody[i++] = 1;
	browserBody[i++] = (browserKeyStart >> 7) & 0x7F;
	browserBody[i++] = browserKeyStart & 0x7F;
	const int32_t sel = selected < 0 ? 0x3FFF : std::min<int32_t>(selected, 0x3FFE);
	browserBody[i++] = (sel >> 7) & 0x7F;
	browserBody[i++] = sel & 0x7F;
	browserBody[i++] = sent;
	char const* path = Browser::currentDir.get();
	char const* folder = path;
	for (char const* c = path; c != nullptr && *c != '\0'; c++) {
		if (*c == '/') {
			folder = c + 1;
		}
	}
	i += putName(browserBody + i, folder); // Folder and file names are names, not strings: shown as they are
	for (int32_t n = 0; n < sent; n++) {
		auto* item = static_cast<FileItem*>(Browser::fileItems.getElementAddress(startLocal + n));
		browserBody[i++] = item->isFolder ? 1 : 0;
		i += putName(browserBody + i, item->displayName != nullptr ? item->displayName : item->filename.get());
	}
	browserBodyLen = i;
	return 1;
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
	static uint8_t msg[7 + 1 + std::max(sizeof(pickerBody), sizeof(browserBody)) + 1];
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
	if (data[2] == 2) {
		// Tapped a picker tile: choose that option from the UI timer, like a select press (MIDI arrives on a task
		// that can run inside the SD card routine)
		if (len >= 4) {
			pendingPick = data[3];
			uiTimerManager.setTimer(TimerName::COMPANION_PICK, 1);
		}
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
	// Room for a full burst (STATE, KIT_ROWS, TEXT and a PICKER are up to about 360 USB MIDI messages)
	if (midiStateCable->sendBufferSpace() < 400) {
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

	uint8_t text[sizeof(lastText)];
	const int32_t textLen = buildText(text);
	if (force || textLen != lastTextLen || memcmp(text, lastText, textLen) != 0) {
		sendFramed(*midiStateCable, kTextHeader, false, text, textLen);
		memcpy(lastText, text, textLen);
		lastTextLen = textLen;
	}

	if (force) {
		pickerKeyMenu = nullptr; // Rebuild and resend whatever is open
	}
	if (updatePicker() || (force && pickerBodyLen > 0)) {
		sendFramed(*midiStateCable, kPickerHeader, false, pickerBody, pickerBodyLen);
	}

	// The browser window is the biggest message (up to about 420 USB MIDI messages): wait for room
	if (midiStateCable->sendBufferSpace() < 450) {
		uiTimerManager.setTimer(TimerName::SYSEX_DISPLAY, 100);
		return;
	}
	if (force) {
		browserKeyCount = -1; // Resend the whole window
	}
	const int32_t browserSend = updateBrowser();
	if (browserSend == 1) {
		sendFramed(*midiStateCable, kBrowserHeader, false, browserBody, browserBodyLen);
	}
	else if (browserSend == 2) {
		uint8_t sel[3] = {kStateVersion,
		                  static_cast<uint8_t>((browserKeySelected < 0 ? 0x3FFF : browserKeySelected) >> 7 & 0x7F),
		                  static_cast<uint8_t>((browserKeySelected < 0 ? 0x3FFF : browserKeySelected) & 0x7F)};
		sendFramed(*midiStateCable, kBrowserSelHeader, false, sel, 3);
	}
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

void HIDSysex::companionCopyText(char* dest, std::string_view text, int16_t& stringIndex) {
	const size_t n = std::min<size_t>(text.size(), kTextMax);
	size_t i = 0;
	for (; i < n && text[i] != '\0'; i++) {
		dest[i] = text[i];
	}
	dest[i] = '\0';
	// Look it up now, while the recent lookups still say which string it was. The scan for storage only one string
	// uses costs more, so only while a companion screen is listening.
	const bool listening = midiStateCable != nullptr && AudioEngine::audioSampleTimer <= midiStateUntil;
	stringIndex = resolveString(text, listening);
}

void HIDSysex::companionNoteOledPopup(char const* text) {
	int16_t unused;
	companionCopyText(oledPopupText, text != nullptr ? text : "", unused);
}

void HIDSysex::runPendingPick() {
	const int32_t pick = pendingPick;
	if (pick < 0) {
		return;
	}
	deluge::gui::ContextMenu* menu = openPicker();
	if (menu == nullptr || pick >= static_cast<int32_t>(menu->getOptions().size())) {
		pendingPick = -1;
		return;
	}
	const int32_t oldOption = menu->currentOption;
	menu->currentOption = pick;
	if (!menu->isCurrentOptionAvailable()) {
		menu->currentOption = oldOption;
		pendingPick = -1;
		return;
	}
	const ActionResult result = menu->buttonAction(deluge::hid::button::SELECT_ENC, true, sdRoutineLock);
	if (result == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE) {
		uiTimerManager.setTimer(TimerName::COMPANION_PICK, 20); // Try again once the card routine is done
		return;
	}
	pendingPick = -1;
}
