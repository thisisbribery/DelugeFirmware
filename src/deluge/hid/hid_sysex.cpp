#include "hid/hid_sysex.h"
#include "dsp/drum_synth/drum_synth.h"
#include "extern.h"
#include "gui/colour/rgb.h"
#include "gui/context_menu/context_menu.h"
#include "gui/l10n/l10n.h"
#include "gui/l10n/language.h"
#include "gui/menu_item/menu_item.h"
#include "gui/menu_item/sample/start_offset.h"
#include "gui/menu_item/value_scaling.h"
#include "gui/ui/browser/browser.h"
#include "gui/ui/browser/sample_browser.h"
#include "gui/ui/menus.h"
#include "gui/ui/sample_marker_editor.h"
#include "gui/ui/sound_editor.h"
#include "gui/ui/ui.h"
#include "gui/ui_timer_manager.h"
#include "gui/views/arranger_view.h"
#include "gui/views/audio_clip_view.h"
#include "gui/views/instrument_clip_view.h"
#include "gui/waveform/waveform_basic_navigator.h"
#include "gui/waveform/waveform_render_data.h"
#include "gui/waveform/waveform_renderer.h"
#include "modulation/patch/patch_cable_set.h"
#include "gui/views/view.h"
#include "util/functions.h"
#include "hid/button.h"
#include "hid/buttons.h"
#include "hid/display/oled.h"
#include "hid/display/seven_segment.h"
#include "io/midi/midi_device.h"
#include "io/midi/midi_engine.h"
#include "io/midi/sysex.h"
#include "memory/general_memory_allocator.h"
#include "model/clip/audio_clip.h"
#include "model/clip/clip.h"
#include "model/clip/clip_instance.h"
#include "model/clip/instrument_clip.h"
#include "model/drum/drum.h"
#include "model/instrument/kit.h"
#include "model/note/note_row.h"
#include "model/output.h"
#include "model/sample/sample.h"
#include "model/sample/sample_cache.h"
#include "model/sample/sample_holder_for_voice.h"
#include "model/song/song.h"
#include "model/voice/voice.h"
#include "model/voice/voice_sample.h"
#include "playback/mode/arrangement.h"
#include "playback/mode/session.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/drum_synth_params.h"
#include "processing/sound/sound_drum.h"
#include "processing/sound/sound_instrument.h"
#include "storage/audio/audio_file.h"
#include "storage/audio/audio_file_holder.h"
#include "storage/file_item.h"
#include "storage/multi_range/multisample_range.h"
#include "util/comparison.h"
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
	DEF_STACK_STRING_BUF(itemValue, kTextMax + 1);
	if (display->have7SEG()) {
		auto* seg = static_cast<deluge::hid::display::SevenSegment*>(display);
		mainText = seg->companionMainText(mainIndex);
		popupText = seg->companionPopupText(popupIndex);
	}
	else if (display->hasPopup()) {
		popupText = oledPopupText; // OLED mode: its text is English already
	}
	// Editing a menu item: the main text is its value, whatever the display is showing (a 7-seg scrolls the name
	// first), so the tablet shows the name and the value together
	if (getCurrentUI() == &soundEditor && soundEditor.getCurrentMenuItem() != nullptr) {
		soundEditor.getCurrentMenuItem()->getNotificationValue(itemValue);
		if (itemValue.size() > 0) {
			mainText = itemValue.c_str();
			// The value was just looked up (an option's text) or is a number: only a copy here, so match it against
			// the latest lookups by text, which gives up when two of them differ in meaning
			mainIndex = display->have7SEG() ? deluge::l10n::recentLookupByContent(mainText) : -1;
		}
		else if (!display->have7SEG()) {
			mainText = {};
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
/// Browser action the tablet asked for, run from the UI timer: 0 none, 1 go to item pendingBrowserItem, 2 press SELECT,
/// 3 press BACK. pendingButtonDown: the button is held, so the next run lets it go.
int32_t pendingBrowser = 0;
int32_t pendingBrowserItem = -1;
bool pendingButtonDown = false;

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

/// Biggest body any companion message has (CLIPS is about 1350 bytes)
constexpr int32_t kMaxFramedBody = 2400;

/// Send one companion message, whole or not at all: false (nothing sent) if there isn't room in the cable's buffer
/// for all of it (a sysex cut off when the buffer fills is lost on the tablet) or the body is too big
bool sendFramed(MIDICable& cable, uint8_t const* header, bool withSeq, uint8_t const* body, int32_t bodyLen) {
	static uint8_t msg[7 + 1 + kMaxFramedBody + 1];
	if (bodyLen > kMaxFramedBody || static_cast<int32_t>(cable.sendBufferSpace()) < bodyLen + 24) {
		return false;
	}
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
	return true;
}

// ---- Waveform (audio clip view, sample marker editor, sample browser preview) ----
constexpr uint8_t kWaveHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x48};
constexpr uint8_t kPlayheadHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x49};
constexpr uint8_t kDiagHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x54};
char diagLastVoice[64] = {}; ///< The last note's start, cache and stretch, kept after it ends
char lastDiag[160] = {};
constexpr int32_t kWaveSub = 12;                        ///< Tablet columns per pad column
constexpr int32_t kWaveChunks = kWaveSub;               ///< Each chunk is kDisplayWidth tablet columns
constexpr int32_t kWaveCols = kDisplayWidth * kWaveSub; ///< 192
constexpr int32_t kWaveChunksPerCall = 4;               ///< Peak work per call, to keep frames short
constexpr int32_t kWaveMaxMarkers = 6;
constexpr int32_t kWaveBodyMax = 8 + 1 + kWaveMaxMarkers * 3 + 1 + kStateNameMax + 2 * kWaveCols;
constexpr int32_t kWaveNoCol = 0x3FFF;

WaveformRenderData waveChunks[kWaveChunks];
Sample* waveSample = nullptr;
uint8_t waveBody[kWaveBodyMax];
int32_t waveBodyLen = 0;
uint8_t lastWave[kWaveBodyMax];
int32_t lastWaveLen = 0;
bool waveWasOpen = false;
uint32_t waveLastSendTime = 0;
int32_t lastPlayhead = -2;

/// The tablet column of a sample position, clamped to a little past either edge
int32_t waveCol(int64_t pos, int64_t xScroll, int64_t subZoom) {
	const int64_t col = (pos - xScroll) / subZoom;
	return static_cast<int32_t>(std::clamp<int64_t>(col, -1, kWaveCols + 1));
}

/// 14-bit marker column, -1 kept as 0x3FFE so the tablet can tell "off the left" from "none"
void putCol(uint8_t* out, int32_t col) {
	const int32_t v = col < 0 ? 0x3FFE : std::min<int32_t>(col, 0x3FFD);
	out[0] = (v >> 7) & 0x7F;
	out[1] = v & 0x7F;
}

/// The file name without its folders
char const* fileNameOf(Sample* sample) {
	char const* path = sample->filePath.get();
	char const* slash = strrchr(path, '/');
	return slash != nullptr ? slash + 1 : path;
}

struct WaveView {
	uint8_t kind = 0; ///< 0 none, 1 audio clip, 2 sample marker editor, 3 sample browser, 4 start offset
	Sample* sample = nullptr;
	int64_t xScroll = 0;
	int64_t xZoom = 1;
	bool reversed = false;
	bool recording = false;
	int32_t numMarkers = 0;
	uint8_t markerType[kWaveMaxMarkers];
	int64_t markerPos[kWaveMaxMarkers];
	int64_t playhead = -1; ///< Sample position, or -1
	int32_t numVoiceHeads = 0;
	int64_t voiceHeads[4];  ///< Where the voices playing this sample are, for synths and kit rows
	Sound* sound = nullptr; ///< The sound playing it (marker editor and start offset), for the diagnostics line
	int32_t source = -1;
};

/// Where the sound's voices are in this sample (the newest few), for the waveform's playheads
void addVoiceHeads(WaveView& v, Sound* sound, int32_t source) {
	if (sound == nullptr || v.sample == nullptr || source < 0 || source >= kNumSources) {
		return;
	}
	for (const auto& voice : sound->voices()) {
		if (v.numVoiceHeads >= 4) {
			break;
		}
		auto& guide = voice->guides[source];
		VoiceSample* voiceSample = voice->unisonParts[0].sources[source].voiceSample;
		if (voiceSample == nullptr || guide.audioFileHolder == nullptr
		    || guide.audioFileHolder->audioFile != v.sample) {
			continue;
		}
		v.voiceHeads[v.numVoiceHeads++] = voiceSample->getPlaySample(v.sample, &guide);
	}
}

void addMarker(WaveView& v, uint8_t type, int64_t pos) {
	if (v.numMarkers < kWaveMaxMarkers) {
		v.markerType[v.numMarkers] = type;
		v.markerPos[v.numMarkers] = pos;
		v.numMarkers++;
	}
}

/// What waveform the Deluge's pads are showing, if any. Marker types: 0 start, 1 loop start, 2 loop end, 3 end (as
/// MarkerType), 4 clip start, 5 clip end; 0x10 set on the one being edited.
WaveView currentWaveView() {
	WaveView v;
	UI* ui = getCurrentUI();
	if (ui == nullptr) {
		return v;
	}
	switch (ui->getUIType()) {
	case UIType::AUDIO_CLIP: {
		AudioClip* clip = getCurrentAudioClip();
		if (clip == nullptr) {
			return v;
		}
		v.recording = clip->getCurrentlyRecordingLinearly();
		v.sample = v.recording ? clip->recorder->sample : static_cast<Sample*>(clip->sampleHolder.audioFile);
		if (v.sample == nullptr) {
			return v;
		}
		v.kind = 1;
		const int32_t scrollTicks = currentSong->xScroll[NAVIGATION_CLIP];
		const int32_t zoomTicks = currentSong->xZoom[NAVIGATION_CLIP];
		clip->getScrollAndZoomInSamples(scrollTicks, zoomTicks, &v.xScroll, &v.xZoom);
		v.reversed = clip->sampleControls.isCurrentlyReversed();
		// Ticks to samples with the same sum the pads use: the scroll position of that tick
		int64_t pos;
		int64_t unusedZoom;
		clip->getScrollAndZoomInSamples(0, zoomTicks, &pos, &unusedZoom);
		addMarker(v, 4, pos);
		clip->getScrollAndZoomInSamples(clip->loopLength, zoomTicks, &pos, &unusedZoom);
		addMarker(v, 5, pos);
		if (playbackHandler.isEitherClockActive() && currentSong->isClipActive(clip)) {
			clip->getScrollAndZoomInSamples(static_cast<int32_t>(clip->getLivePos()), zoomTicks, &pos, &unusedZoom);
			v.playhead = pos;
		}
		return v;
	}
	case UIType::SAMPLE_MARKER_EDITOR: {
		v.sample = waveformBasicNavigator.sample;
		if (v.sample == nullptr) {
			return v;
		}
		v.kind = 2;
		v.xScroll = waveformBasicNavigator.xScroll;
		v.xZoom = waveformBasicNavigator.xZoom;
		const int32_t editing = util::to_underlying(sampleMarkerEditor.markerType);
		auto mark = [&](int32_t type, int64_t pos) { addMarker(v, type | (type == editing ? 0x10 : 0), pos); };
		AudioClip* clip = (getCurrentClip() != nullptr && getCurrentClip()->type == ClipType::AUDIO)
		                      ? getCurrentAudioClip()
		                      : nullptr;
		if (clip != nullptr) {
			mark(0, clip->sampleHolder.startPos);
			mark(3, clip->sampleHolder.endPos);
			// Audio clip playing: its playhead is in the sample's own samples here
			if (playbackHandler.isEitherClockActive() && currentSong->isClipActive(clip)
			    && clip->voiceSample != nullptr) {
				v.voiceHeads[v.numVoiceHeads++] = clip->voiceSample->getPlaySample(v.sample, &clip->guide);
			}
		}
		else if (soundEditor.currentMultiRange != nullptr) {
			auto& holder = static_cast<MultisampleRange*>(soundEditor.currentMultiRange)->sampleHolder;
			mark(0, holder.startPos);
			if (holder.loopStartPos != 0) {
				mark(1, holder.loopStartPos);
			}
			if (holder.loopEndPos != 0) {
				mark(2, holder.loopEndPos);
			}
			mark(3, holder.endPos);
			addVoiceHeads(v, soundEditor.currentSound, soundEditor.currentSourceIndex);
			v.sound = soundEditor.currentSound;
			v.source = soundEditor.currentSourceIndex;
			// A start offset moves where playback really starts: show it (yellow) whenever it isn't zero, as drum
			// engines keep a control on the same param and a row turned into a sample can carry it over
			if (soundEditor.currentSound != nullptr && soundEditor.currentParamManager != nullptr
			    && soundEditor.currentSourceIndex >= 0 && soundEditor.currentSourceIndex < kNumSources
			    && v.sample->lengthInSamples > 1) {
				const int32_t param = soundEditor.currentParamManager->getPatchedParamSet()->getValue(
				    deluge::modulation::params::LOCAL_OSC_A_START_OFFSET + soundEditor.currentSourceIndex);
				if (param != 0) {
					const int64_t length = static_cast<int64_t>(v.sample->lengthInSamples);
					const int64_t start = holder.startPos;
					const int64_t shift = (static_cast<int64_t>(param) * length) >> 30;
					const bool wraps = soundEditor.currentSound->sources[soundEditor.currentSourceIndex].offsetWraps;
					addMarker(v, 6,
					          wraps ? (((start + shift) % length) + length) % length
					                : std::clamp<int64_t>(start + shift, 0, length - 1));
				}
			}
		}
		return v;
	}
	case UIType::SOUND_EDITOR: {
		// Start offset (synth oscillators and kit rows alike): the whole sample, its start and end, and where the
		// offset makes it start (the sum Voice::applyStartOffsetToGuide does, ignoring reverse)
		MenuItem* item = soundEditor.getCurrentMenuItem();
		const int32_t source = item == &sample0StartOffsetMenu ? 0 : item == &sample1StartOffsetMenu ? 1 : -1;
		if (source < 0 || soundEditor.currentSound == nullptr || soundEditor.currentParamManager == nullptr) {
			return v;
		}
		Source& src = soundEditor.currentSound->sources[source];
		if (src.ranges.getNumElements() < 1) {
			return v;
		}
		auto* range = static_cast<MultisampleRange*>(
		    soundEditor.currentMultiRange != nullptr ? soundEditor.currentMultiRange : src.ranges.getElement(0));
		v.sample = static_cast<Sample*>(range->sampleHolder.audioFile);
		if (v.sample == nullptr || v.sample->lengthInSamples < 2) {
			return v;
		}
		v.kind = 4;
		const int64_t length = static_cast<int64_t>(v.sample->lengthInSamples);
		v.xScroll = 0;
		v.xZoom = std::max<int64_t>(kWaveSub, (length + kDisplayWidth - 1) / kDisplayWidth);
		v.reversed = src.sampleControls.isCurrentlyReversed();
		const int64_t start = range->sampleHolder.startPos;
		const int64_t end = std::min<int64_t>(range->sampleHolder.getEndPos(), length);
		addMarker(v, 0, start);
		addMarker(v, 3, end);
		const int32_t param = soundEditor.currentParamManager->getPatchedParamSet()->getValue(
		    deluge::modulation::params::LOCAL_OSC_A_START_OFFSET + source);
		const int64_t shift = (static_cast<int64_t>(param) * length) >> 30;
		int64_t offsetPos = src.offsetWraps ? (((start + shift) % length) + length) % length
		                                    : std::clamp<int64_t>(start + shift, 0, length - 1);
		addMarker(v, 6 | 0x10, offsetPos);
		addVoiceHeads(v, soundEditor.currentSound, source);
		v.sound = soundEditor.currentSound;
		v.source = source;
		return v;
	}
	case UIType::SAMPLE_BROWSER:
		if (!sampleBrowser.isShowingSamplePreview() || waveformBasicNavigator.sample == nullptr) {
			return v;
		}
		v.kind = 3;
		v.sample = waveformBasicNavigator.sample;
		v.xScroll = waveformBasicNavigator.xScroll;
		v.xZoom = waveformBasicNavigator.xZoom;
		return v;
	default:
		return v;
	}
}

/// One line on how the sample is being played, for finding why a start marker isn't heard: the holder's start, the
/// last note's own start (from its playback guide), whether it read from a pitch cache (and that cache's start) or a
/// time stretcher, the repeat mode, and what the other oscillator plays.
void buildDiag(WaveView const& v, char* out, size_t size) {
	out[0] = 0;
	if (v.sound == nullptr || v.source < 0 || v.source >= kNumSources || v.sample == nullptr) {
		return;
	}
	Sound* sound = v.sound;
	Source& src = sound->sources[v.source];
	const int32_t bytesPerSample = std::max<int32_t>(v.sample->numChannels * v.sample->byteDepth, 1);
	for (const auto& voice : sound->voices()) {
		auto& guide = voice->guides[v.source];
		VoiceSample* voiceSample = voice->unisonParts[0].sources[v.source].voiceSample;
		if (voiceSample == nullptr || guide.audioFileHolder == nullptr
		    || guide.audioFileHolder->audioFile != v.sample) {
			continue;
		}
		const int32_t guideStart =
		    (static_cast<int32_t>(guide.startPlaybackAtByte) - static_cast<int32_t>(v.sample->audioDataStartPosBytes))
		    / bytesPerSample;
		const char cacheState = voiceSample->cache == nullptr ? '-' : voiceSample->writingToCache ? 'W' : 'R';
		snprintf(diagLastVoice, sizeof(diagLastVoice), "note start %ld, cache %c%s%ld, stretch %s, preroll %ld",
		         static_cast<long>(guideStart), cacheState, voiceSample->cache != nullptr ? " from " : "",
		         static_cast<long>(voiceSample->cache != nullptr ? voiceSample->cache->skipSamplesAtStart : 0),
		         voiceSample->timeStretcher != nullptr ? "yes" : "no", static_cast<long>(guide.preRollSamples));
		break;
	}
	static constexpr char const* kRepeat[] = {"CUT", "ONCE", "LOOP", "STRETCH"};
	const int32_t repeat = std::clamp<int32_t>(util::to_underlying(src.repeatMode), 0, 3);
	Source& other = sound->sources[1 - v.source];
	char otherText[40];
	if (other.oscType == OscType::SAMPLE && other.ranges.getNumElements() > 0) {
		AudioFileHolder* holder = other.ranges.getElement(0)->getAudioFileHolder();
		const bool same = holder != nullptr && holder->audioFile == v.sample;
		snprintf(otherText, sizeof(otherText), "osc %ld sample%s", static_cast<long>(2 - v.source),
		         same ? " (same file)" : "");
	}
	else {
		snprintf(otherText, sizeof(otherText), "osc %ld type %ld", static_cast<long>(2 - v.source),
		         static_cast<long>(util::to_underlying(other.oscType)));
	}
	int64_t holderStart = -1;
	if (src.ranges.getNumElements() > 0) {
		holderStart = static_cast<SampleHolder*>(src.ranges.getElement(0)->getAudioFileHolder())->startPos;
	}
	snprintf(out, size, "start %ld | %s | %s | %ld ranges | %s", static_cast<long>(holderStart),
	         diagLastVoice[0] ? diagLastVoice : "no note yet", kRepeat[repeat],
	         static_cast<long>(src.ranges.getNumElements()), otherText);
}

/// Work out the waveform body. Returns 0 = nothing to send, 1 = send WAVEFORM. WAVEFORM body: version, open, kind,
/// flags (1 reversed, 2 recording, 4 every column known), columns (2 bytes), marker count, per marker its type and
/// column (2 bytes, 0x3FFE = left of the window), file name, then per column min and max (0 to 127, 64 = silence;
/// 127 then 0 = not known yet). Columns are kDisplayWidth * kWaveSub across the window the pads show.
int32_t updateWave(WaveView const& v) {
	int32_t i = 0;
	if (v.kind == 0) {
		waveSample = nullptr;
		if (!waveWasOpen) {
			return 0;
		}
		waveWasOpen = false;
		waveBody[i++] = kStateVersion;
		waveBody[i++] = 0;
		waveBodyLen = i;
		return 1;
	}
	waveWasOpen = true;
	const int64_t subZoom = std::max<int64_t>(1, v.xZoom / kWaveSub);
	if (v.sample != waveSample) {
		waveSample = v.sample;
		for (auto& chunk : waveChunks) {
			chunk.xScroll = -1; // Work everything out again
		}
	}

	bool complete = true;
	if (!v.recording) {
		int32_t done = 0;
		for (int32_t c = 0; c < kWaveChunks; c++) {
			WaveformRenderData& chunk = waveChunks[c];
			const int64_t chunkScroll = v.xScroll + static_cast<int64_t>(c) * kDisplayWidth * subZoom;
			const bool fresh = chunk.xScroll == chunkScroll && chunk.xZoom == subZoom;
			bool chunkComplete = fresh;
			if (fresh) {
				for (auto status : chunk.colStatus) {
					if (status == ColumnStatus::NOT_INVESTIGATED || status == ColumnStatus::PLACEHOLDER) {
						chunkComplete = false;
						break;
					}
				}
			}
			if (chunkComplete) {
				continue;
			}
			if (done >= kWaveChunksPerCall || sdRoutineLock) {
				complete = false;
				continue;
			}
			done++;
			if (!waveformRenderer.findPeaksPerCol(v.sample, chunkScroll, subZoom, &chunk, nullptr, 0, kDisplayWidth,
			                                      false)) {
				complete = false;
			}
		}
	}
	else {
		complete = false;
	}

	waveBody[i++] = kStateVersion;
	waveBody[i++] = 1;
	waveBody[i++] = v.kind;
	waveBody[i++] = (v.reversed ? 1 : 0) | (v.recording ? 2 : 0) | (complete ? 4 : 0);
	waveBody[i++] = (kWaveCols >> 7) & 0x7F;
	waveBody[i++] = kWaveCols & 0x7F;
	waveBody[i++] = v.numMarkers;
	for (int32_t m = 0; m < v.numMarkers; m++) {
		waveBody[i++] = v.markerType[m];
		putCol(waveBody + i, waveCol(v.markerPos[m], v.xScroll, subZoom));
		i += 2;
	}
	i += putName(waveBody + i, fileNameOf(v.sample));
	for (int32_t c = 0; c < kWaveChunks; c++) {
		WaveformRenderData& chunk = waveChunks[c];
		const bool usable = !v.recording && chunk.xZoom == subZoom
		                    && chunk.xScroll == v.xScroll + static_cast<int64_t>(c) * kDisplayWidth * subZoom;
		for (int32_t col = 0; col < kDisplayWidth; col++) {
			const ColumnStatus status = chunk.colStatus[col];
			if (!usable || status == ColumnStatus::NOT_INVESTIGATED) {
				waveBody[i++] = 127;
				waveBody[i++] = 0;
			}
			else if (status == ColumnStatus::BEYOND_WAVEFORM) {
				waveBody[i++] = 64;
				waveBody[i++] = 64;
			}
			else {
				waveBody[i++] = static_cast<uint8_t>(std::clamp<int32_t>((chunk.minPerCol[col] >> 25) + 64, 0, 127));
				waveBody[i++] = static_cast<uint8_t>(std::clamp<int32_t>((chunk.maxPerCol[col] >> 25) + 64, 0, 127));
			}
		}
	}
	waveBodyLen = i;
	return (complete || v.recording) ? 1 : 2; // 2 = still filling in: send now and then
}

// ---- Knobs: the main settings of the synth or kit row being played, read from its params ----
constexpr uint8_t kParamsHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x4A};
constexpr uint8_t kTransportHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x4B};
constexpr uint8_t kOutputsHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x4C};
constexpr uint8_t kClipsHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x4D};
constexpr uint8_t kArrangerHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x4E};
constexpr uint8_t kArrangerPosHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x4F};
constexpr uint8_t kClipPosHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x50};
constexpr uint8_t kBeatHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x51};
constexpr uint8_t kKitSamplesHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x52};
constexpr uint8_t kKitActiveHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x53};

/// A setting shown as a knob: the shortcut pad that opens it (what a tap presses), the group the tablet puts it in,
/// the param it reads, and its English name. Read straight from the sound's params: no menu item is asked anything
/// here, as menu items read the sound editor's state, which isn't set up (or is stale) outside the editor.
/// When a knob's pad would say CANT (worked out here from the sound, as the menu items' own checks read the editor)
enum class KnobRule : uint8_t {
	ALWAYS,
	NOT_RINGMOD,  ///< Oscillator levels
	NOT_FM,       ///< Noise
	LPF,          ///< LPF on, not FM
	HPF,          ///< HPF on, not FM
	MOD_FX_RATE,  ///< Any mod FX but BRUNO CHORUS
	MOD_FX_DEPTH, ///< Chorus, stereo chorus, grain, phaser, warble, dimension
	MOD_FX_FB,    ///< Flanger, phaser, grain, warble
	OFFSET_A,     ///< Shown only on a sample oscillator (its pad is pulse width otherwise)
	OFFSET_B,
};
struct KnobDef {
	uint8_t x;
	uint8_t y;
	uint8_t group;  ///< 0 oscillators, 1 filters, 2 envelope 1, 3 envelope 2, 4 voice, 5 effects, 6 LFOs
	bool unpatched; ///< In the unpatched param set, else the patched one
	uint16_t param;
	char const* name;
	KnobRule rule = KnobRule::ALWAYS;
};
namespace P = deluge::modulation::params;
constexpr KnobDef kKnobs[] = {
    {2, 0, 0, false, P::LOCAL_OSC_A_VOLUME, "Osc1 level", KnobRule::NOT_RINGMOD},
    {3, 0, 0, false, P::LOCAL_OSC_B_VOLUME, "Osc2 level", KnobRule::NOT_RINGMOD},
    {2, 3, 0, false, P::LOCAL_OSC_A_START_OFFSET, "Start offset", KnobRule::OFFSET_A},
    {3, 3, 0, false, P::LOCAL_OSC_B_START_OFFSET, "Start offset", KnobRule::OFFSET_B},
    {2, 7, 0, false, P::LOCAL_NOISE_VOLUME, "Noise level", KnobRule::NOT_FM},
    {8, 7, 1, false, P::LOCAL_LPF_FREQ, "LPF frequency", KnobRule::LPF},
    {8, 6, 1, false, P::LOCAL_LPF_RESONANCE, "LPF resonance", KnobRule::LPF},
    {8, 4, 1, false, P::LOCAL_LPF_MORPH, "LPF morph", KnobRule::LPF},
    {9, 7, 1, false, P::LOCAL_HPF_FREQ, "HPF frequency", KnobRule::HPF},
    {9, 6, 1, false, P::LOCAL_HPF_RESONANCE, "HPF resonance", KnobRule::HPF},
    {9, 4, 1, false, P::LOCAL_HPF_MORPH, "HPF Morph", KnobRule::HPF},
    {8, 3, 2, false, P::LOCAL_ENV_0_ATTACK, "Env1 attack"},
    {8, 2, 2, false, P::LOCAL_ENV_0_DECAY, "Env1 decay"},
    {8, 1, 2, false, P::LOCAL_ENV_0_SUSTAIN, "Env1 sustain"},
    {8, 0, 2, false, P::LOCAL_ENV_0_RELEASE, "Env1 release"},
    {9, 3, 3, false, P::LOCAL_ENV_1_ATTACK, "Env2 attack"},
    {9, 2, 3, false, P::LOCAL_ENV_1_DECAY, "Env2 decay"},
    {9, 1, 3, false, P::LOCAL_ENV_1_SUSTAIN, "Env2 sustain"},
    {9, 0, 3, false, P::LOCAL_ENV_1_RELEASE, "Env2 release"},
    {6, 0, 4, false, P::GLOBAL_VOLUME_POST_FX, "Master level"},
    {6, 3, 4, false, P::LOCAL_PAN, "Pan"},
    {7, 0, 4, true, P::UNPATCHED_PORTAMENTO, "Portamento"},
    {12, 7, 5, false, P::GLOBAL_MOD_FX_RATE, "MOD-FX rate", KnobRule::MOD_FX_RATE},
    {12, 6, 5, false, P::GLOBAL_MOD_FX_DEPTH, "MOD-FX depth", KnobRule::MOD_FX_DEPTH},
    {12, 5, 5, true, P::UNPATCHED_MOD_FX_FEEDBACK, "MOD-FX feedback", KnobRule::MOD_FX_FB},
    {13, 3, 5, false, P::GLOBAL_REVERB_AMOUNT, "Reverb amount"},
    {14, 0, 5, false, P::GLOBAL_DELAY_RATE, "Delay rate"},
    {14, 3, 5, false, P::GLOBAL_DELAY_FEEDBACK, "Delay amount"},
    {6, 6, 5, true, P::UNPATCHED_BITCRUSHING, "Bitcrush"},
    {6, 5, 5, true, P::UNPATCHED_SAMPLE_RATE_REDUCTION, "Decimation"},
    {10, 6, 5, true, P::UNPATCHED_BASS, "Bass"},
    {11, 6, 5, true, P::UNPATCHED_TREBLE, "Treble"},
    {12, 0, 6, false, P::GLOBAL_LFO_FREQ_1, "LFO1 rate"},
    {13, 0, 6, false, P::LOCAL_LFO_LOCAL_FREQ_1, "LFO2 rate"},
};
constexpr int32_t kNumKnobs = sizeof(kKnobs) / sizeof(kKnobs[0]);
constexpr int32_t kKnobNameMax = 18;
constexpr int32_t kMaxEngineKnobs = 24;
constexpr int32_t kKnobTextMax = 10;
constexpr int32_t kParamsBodyMax = 4 + (kNumKnobs + kMaxEngineKnobs) * (8 + kKnobNameMax + kKnobTextMax);

uint8_t paramsBody[kParamsBodyMax];
struct EngineName {
	uint32_t param;
	char name[kKnobNameMax + 1];
};
EngineName engineNames[kMaxEngineKnobs]; ///< The engine controls sent in the last PARAMS, by param
int32_t numEngineNames = 0;
int32_t paramsBodyLen = 0;
uint8_t lastParams[kParamsBodyMax];
int32_t lastParamsLen = 0;

/// The sound being played and its params: the synth, or the kit's selected row. False if neither.
bool currentSoundAndParams(Sound*& sound, ParamManagerForTimeline*& params) {
	Clip* clip = getCurrentClip();
	if (clip == nullptr || clip->type != ClipType::INSTRUMENT || clip->output == nullptr) {
		return false;
	}
	auto* instrumentClip = static_cast<InstrumentClip*>(clip);
	if (clip->output->type == OutputType::SYNTH) {
		sound = static_cast<SoundInstrument*>(clip->output);
		params = &clip->paramManager;
	}
	else if (clip->output->type == OutputType::KIT) {
		Drum* drum = static_cast<Kit*>(clip->output)->selectedDrum;
		if (drum == nullptr || drum->type != DrumType::SOUND) {
			return false;
		}
		NoteRow* row = instrumentClip->getNoteRowForDrum(drum);
		if (row == nullptr) {
			return false;
		}
		sound = static_cast<SoundDrum*>(drum);
		params = &row->paramManager;
	}
	else {
		return false;
	}
	return params->containsAnyMainParamCollections();
}

/// One PARAMS entry. text: what a switch is set to (empty for a knob).
int32_t putKnob(uint8_t* out, int32_t x, int32_t y, int32_t group, int32_t flags, int32_t pos, int32_t shown,
                std::string_view name, std::string_view text) {
	int32_t i = 0;
	out[i++] = x;
	out[i++] = y;
	out[i++] = group;
	out[i++] = flags;
	out[i++] = static_cast<uint8_t>(std::clamp<int32_t>(pos, 0, 127));
	const int32_t v = std::clamp<int32_t>(shown + 8192, 0, 16383);
	out[i++] = (v >> 7) & 0x7F;
	out[i++] = v & 0x7F;
	char buffer[kKnobNameMax + 1];
	size_t n = std::min<size_t>(name.size(), kKnobNameMax);
	memcpy(buffer, name.data(), n);
	buffer[n] = 0;
	i += putName(out + i, buffer);
	char textBuffer[kKnobTextMax + 1];
	n = std::min<size_t>(text.size(), kKnobTextMax);
	memcpy(textBuffer, text.data(), n);
	textBuffer[n] = 0;
	i += putName(out + i, textBuffer);
	return i;
}

std::string_view englishOf(deluge::l10n::String string) {
	return deluge::l10n::getView(deluge::l10n::built_in::english, string);
}

/// Whether a knob's pad would open its setting (true) or say CANT. Mirrors the menu items' isRelevant(), from the
/// sound.
bool knobAvailable(KnobRule rule, Sound* sound) {
	const SynthMode mode = sound->getSynthMode();
	switch (rule) {
	case KnobRule::NOT_RINGMOD:
		return mode != SynthMode::RINGMOD;
	case KnobRule::NOT_FM:
		return mode != SynthMode::FM;
	case KnobRule::LPF:
		return mode != SynthMode::FM && sound->lpfMode != FilterMode::OFF;
	case KnobRule::HPF:
		return mode != SynthMode::FM && sound->hpfMode != FilterMode::OFF;
	case KnobRule::MOD_FX_RATE: {
		const ModFXType type = sound->getModFXType();
		return type != ModFXType::NONE && type != ModFXType::BRUNO_CHORUS;
	}
	case KnobRule::MOD_FX_DEPTH:
		return util::one_of(sound->getModFXType(), {ModFXType::CHORUS, ModFXType::CHORUS_STEREO, ModFXType::GRAIN,
		                                            ModFXType::PHASER, ModFXType::WARBLE, ModFXType::DIMENSION});
	case KnobRule::MOD_FX_FB:
		return util::one_of(sound->getModFXType(),
		                    {ModFXType::FLANGER, ModFXType::PHASER, ModFXType::GRAIN, ModFXType::WARBLE});
	case KnobRule::OFFSET_A:
	case KnobRule::OFFSET_B: {
		// As sample::isSampleModeSample()
		Source& source = sound->sources[rule == KnobRule::OFFSET_A ? 0 : 1];
		return mode == SynthMode::SUBTRACTIVE && source.oscType == OscType::SAMPLE
		       && source.hasAtLeastOneAudioFileLoaded();
	}
	default:
		return true;
	}
}

/// PARAMS body: version, open, count, then per knob its pad x and y, group, flags (1 bipolar, 2 open in the menu,
/// 4 its pad would say CANT),
/// position (0 to 127, 64 = middle), the value the Deluge shows (2 bytes, plus 8192), and its English name
int32_t buildParams(uint8_t* out) {
	int32_t i = 0;
	out[i++] = kStateVersion;
	Sound* sound = nullptr;
	ParamManagerForTimeline* params = nullptr;
	if (!currentSoundAndParams(sound, params)) {
		out[i++] = 0;
		return i;
	}
	out[i++] = 1;
	const int32_t countAt = i++;
	int32_t count = 0;
	const bool drumEngine = sound->sources[0].oscType == OscType::DRUM_SYNTH;
	const bool editing = getCurrentUI() == &soundEditor;
	PatchedParamSet* patched = params->getPatchedParamSet();
	UnpatchedParamSet* unpatched = params->getUnpatchedParamSet();
	for (int32_t k = 0; k < kNumKnobs; k++) {
		const KnobDef& knob = kKnobs[k];
		if (knob.group == 0 && drumEngine) {
			continue; // Drum engines put their own controls on the oscillator pads
		}
		if (knob.group <= 3 && sound->synthMode == SynthMode::TB303) {
			continue; // The 303's own knobs take these pads (group 7)
		}
		const bool offset = knob.rule == KnobRule::OFFSET_A || knob.rule == KnobRule::OFFSET_B;
		const bool available = knobAvailable(knob.rule, sound);
		if (offset && !available) {
			continue; // Pulse width's pad: not a start offset here
		}
		if (knob.unpatched ? unpatched == nullptr : patched == nullptr) {
			continue;
		}
		const int32_t value = knob.unpatched ? unpatched->getValue(knob.param) : patched->getValue(knob.param);
		const bool pan = !knob.unpatched && knob.param == P::LOCAL_PAN;
		// Start offset shows -512 to 512, as its menu (sample::StartOffset)
		const int32_t shown = offset ? (value >> 22)
		                      : pan  ? computeCurrentValueForPan(value)
		                             : computeCurrentValueForStandardMenuItem(value);
		const bool open =
		    editing && soundEditor.currentParamShortcutX == knob.x && soundEditor.currentParamShortcutY == knob.y;
		// The names are the Deluge's own English titles for these pads. MOD-FX depth and feedback are named by the
		// Mod FX type, as on the Deluge.
		std::string_view name = knob.name;
		if (knob.rule == KnobRule::MOD_FX_DEPTH || knob.rule == KnobRule::MOD_FX_FB) {
			const deluge::l10n::Language* language = deluge::l10n::chosenLanguage;
			deluge::l10n::chosenLanguage = &deluge::l10n::built_in::english;
			name = modfx::getParamName(sound->getModFXType(),
			                           knob.rule == KnobRule::MOD_FX_DEPTH ? ModFXParam::DEPTH : ModFXParam::FEEDBACK);
			deluge::l10n::chosenLanguage = language;
		}
		i += putKnob(out + i, knob.x, knob.y, knob.group,
		             ((pan || offset) ? 1 : 0) | (open ? 2 : 0) | (available ? 0 : 4), (value >> 25) + 64, shown, name,
		             {});
		count++;
	}

	// The engine's own controls (group 7) and switches (group 8, flag 16), read from the params and the patch bytes
	auto isOpen = [&](int32_t x, int32_t y) {
		return editing && soundEditor.currentParamShortcutX == x && soundEditor.currentParamShortcutY == y;
	};
	int32_t engineCount = 0;
	numEngineNames = 0;
	if (drumEngine && patched != nullptr) {
		// Drum and synth engines: every pad of the engine's layout that holds one of its controls
		const auto engine = sound->sources[0].drumPatch.engine();
		uint32_t seen = 0;
		for (int32_t x = 0; x < kDisplayWidth && engineCount < kMaxEngineKnobs; x++) {
			for (int32_t y = 0; y < kDisplayHeight && engineCount < kMaxEngineKnobs; y++) {
				const uint32_t param = drumShortcutParam(sound, x, y);
				if (param >= deluge::modulation::params::kNumParams) {
					continue;
				}
				const int32_t slot = deluge::dsp::drum_synth::drumSlotForParam(static_cast<int32_t>(param));
				if (slot < 0 || slot >= 32 || (seen & (1u << slot))) {
					continue;
				}
				seen |= 1u << slot;
				const int32_t raw = patched->getValue(param);
				const int32_t knob = std::clamp<int32_t>(
				    (deluge::dsp::drum_synth::DrumControls::toKnob(raw) + 0x80) >> 8, 0, 127); // 0-127, as its menu
				// Its name as the Deluge gives it: the English title of the control on that pad (what the 7-seg
				// scrolls when the pad is held). Only drum control items get here, whose titles read nothing but
				// the sound being edited, so the editor is pointed at this one meanwhile.
				char title[kKnobNameMax + 1];
				std::string_view name = englishOf(deluge::dsp::drum_synth::cycKnobName(engine, slot));
				if (isDrumLayoutPad(sound, x, y)) {
					if (MenuItem* item = soundShortcutFor(sound, x, y); item != nullptr) {
						const deluge::l10n::Language* language = deluge::l10n::chosenLanguage;
						Sound* editorSound = soundEditor.currentSound;
						deluge::l10n::chosenLanguage = &deluge::l10n::built_in::english;
						soundEditor.currentSound = sound;
						const std::string_view itemTitle = item->getTitle();
						const size_t n = std::min<size_t>(itemTitle.size(), kKnobNameMax);
						memcpy(title, itemTitle.data(), n);
						title[n] = 0;
						soundEditor.currentSound = editorSound;
						deluge::l10n::chosenLanguage = language;
						if (n > 0) {
							name = std::string_view(title, n);
						}
					}
				}
				i += putKnob(out + i, x, y, 7, (isOpen(x, y) ? 2 : 0) | 8, knob, knob, name, {});
				EngineName& kept = engineNames[numEngineNames++];
				kept.param = param;
				const size_t keptLen = std::min<size_t>(name.size(), kKnobNameMax);
				memcpy(kept.name, name.data(), keptLen);
				kept.name[keptLen] = 0;
				count++;
				engineCount++;
			}
		}
		// BRUNO106's switches
		if (engine == Engine::BRUNO106) {
			using deluge::dsp::drum_synth::Patch;
			const auto& patch = sound->sources[0].drumPatch;
			const bool saw = patch.get(Patch::BRUNO_SAW) != 0;
			const bool pulse = patch.get(Patch::BRUNO_PULSE) != 0;
			const int32_t hpf = patch.get(Patch::BRUNO_HPF);
			static constexpr deluge::l10n::String kHpfNames[] = {
			    deluge::l10n::String::STRING_FOR_BRUNO_HPF_FLAT, deluge::l10n::String::STRING_FOR_BRUNO_HPF_BOOST,
			    deluge::l10n::String::STRING_FOR_BRUNO_HPF_236, deluge::l10n::String::STRING_FOR_BRUNO_HPF_754};
			const bool chorus = sound->getModFXType() == ModFXType::BRUNO_CHORUS;
			const int32_t mode = std::clamp<int32_t>(sound->modfx.brunoChorusMode, 1, 3);
			static constexpr deluge::l10n::String kModeNames[] = {deluge::l10n::String::STRING_FOR_BRUNO_CHORUS_I,
			                                                      deluge::l10n::String::STRING_FOR_BRUNO_CHORUS_II,
			                                                      deluge::l10n::String::STRING_FOR_BRUNO_CHORUS_I_II};
			auto onOff = [](bool on) {
				return englishOf(on ? deluge::l10n::String::STRING_FOR_ON : deluge::l10n::String::STRING_FOR_OFF);
			};
			i += putKnob(out + i, 2, 2, 8, 16 | (isOpen(2, 2) ? 2 : 0), saw, saw,
			             englishOf(deluge::l10n::String::STRING_FOR_BRUNO_SAW), onOff(saw));
			i += putKnob(out + i, 3, 2, 8, 16 | (isOpen(3, 2) ? 2 : 0), pulse, pulse,
			             englishOf(deluge::l10n::String::STRING_FOR_BRUNO_PULSE), onOff(pulse));
			i += putKnob(out + i, 4, 0, 8, 16 | (isOpen(4, 0) ? 2 : 0), hpf, hpf,
			             englishOf(deluge::l10n::String::STRING_FOR_BRUNO_HPF),
			             englishOf(kHpfNames[std::clamp<int32_t>(hpf, 0, 3)]));
			i += putKnob(out + i, 4, 1, 8, 16 | (isOpen(4, 1) ? 2 : 0), chorus, chorus,
			             englishOf(deluge::l10n::String::STRING_FOR_BRUNO_CHORUS_ON), onOff(chorus));
			i +=
			    putKnob(out + i, 4, 2, 8, 16 | (isOpen(4, 2) ? 2 : 0) | (chorus ? 0 : 4), mode, mode,
			            englishOf(deluge::l10n::String::STRING_FOR_BRUNO_CHORUS_MODE), englishOf(kModeNames[mode - 1]));
			count += 5;
		}
	}
	else if (sound->synthMode == SynthMode::TB303 && patched != nullptr) {
		// The 303's knobs, on their pads, and its filter switch
		struct Tb303Knob {
			uint8_t x;
			uint8_t y;
			int32_t param;
			deluge::l10n::String name;
		};
		using S = deluge::l10n::String;
		static constexpr Tb303Knob kTb303Knobs[] = {
		    {8, 7, P::LOCAL_LPF_FREQ, S::STRING_FOR_TB303_CUTOFF},
		    {8, 6, P::LOCAL_LPF_RESONANCE, S::STRING_FOR_TB303_RESONANCE},
		    {4, 0, kTb303EnvModParam, S::STRING_FOR_TB303_ENV_MOD},
		    {5, 0, kTb303DecayParam, S::STRING_FOR_TB303_DECAY},
		    {2, 4, kTb303AccentParam, S::STRING_FOR_TB303_ACCENT},
		    {2, 3, kTb303WaveformParam, S::STRING_FOR_TB303_WAVEFORM},
		    {4, 5, kTb303SlideParam, S::STRING_FOR_TB303_SLIDE},
		    {2, 5, kTb303DriveParam, S::STRING_FOR_TB303_DRIVE},
		    {2, 6, kTb303AmpDecayParam, S::STRING_FOR_TB303_AMP_DECAY},
		};
		for (const Tb303Knob& k : kTb303Knobs) {
			const int32_t raw = patched->getValue(k.param);
			i += putKnob(out + i, k.x, k.y, 7, isOpen(k.x, k.y) ? 2 : 0, (raw >> 25) + 64,
			             computeCurrentValueForStandardMenuItem(raw), englishOf(k.name), {});
			count++;
		}
		static constexpr S kFilterNames[] = {S::STRING_FOR_TB303, S::STRING_FOR_TB303_FILTER_LP18,
		                                     S::STRING_FOR_TB303_FILTER_DIRT};
		const int32_t filter = std::clamp<int32_t>(static_cast<int32_t>(sound->tb303FilterMode), 0, 2);
		i += putKnob(out + i, 5, 5, 8, 16 | (isOpen(5, 5) ? 2 : 0), filter, filter,
		             englishOf(S::STRING_FOR_TB303_FILTER), englishOf(kFilterNames[filter]));
		count++;
	}
	out[countAt] = count;
	return i;
}

// ---- Gold knobs: what each of the 8 mod buttons puts on the two gold knobs ----
constexpr uint8_t kGoldHeader[7] = {0xF0, 0x00, 0x21, 0x7B, 0x01, 0x02, 0x55};
constexpr int32_t kGoldBodyMax = 4 + 16 * (6 + kStateNameMax);
uint8_t lastGold[kGoldBodyMax];
int32_t lastGoldLen = 0;

/// A gold knob's name as the Deluge's OLED popup gives it, in English. Engine controls take the name of their pad.
void goldKnobName(Sound* sound, ParamDescriptor descriptor, char* out, size_t size) {
	out[0] = 0;
	if (descriptor.isNull()) {
		strncpy(out, englishOf(deluge::l10n::String::STRING_FOR_NONE).data(), size - 1);
		out[size - 1] = 0;
		return;
	}
	const deluge::l10n::Language* language = deluge::l10n::chosenLanguage;
	deluge::l10n::chosenLanguage = &deluge::l10n::built_in::english;
	if (descriptor.isJustAParam()) {
		const int32_t p = descriptor.getJustTheParam();
		char const* name = nullptr;
		for (int32_t e = 0; e < numEngineNames; e++) {
			if (engineNames[e].param == static_cast<uint32_t>(p)) {
				name = engineNames[e].name;
				break;
			}
		}
		if (name == nullptr) {
			name = p >= P::UNPATCHED_START
			           ? deluge::modulation::params::getParamDisplayName(P::Kind::UNPATCHED_SOUND,
			                                                             p - P::UNPATCHED_START, sound)
			           : deluge::modulation::params::getParamDisplayName(P::Kind::PATCHED, p, sound);
		}
		snprintf(out, size, "%s", name != nullptr ? name : "");
	}
	else {
		// As the OLED: the source, an arrow, and the param's short name
		const PatchSource source1 = descriptor.getBottomLevelSource();
		const int32_t p = descriptor.getJustTheParam();
		char const* param = deluge::modulation::params::getPatchedParamShortName(p, sound);
		if (descriptor.hasJustOneSource()) {
			snprintf(out, size, "%s -> %s", getSourceDisplayNameForOLED(source1), param);
		}
		else {
			snprintf(out, size, "%s -> %s -> %s", sourceToStringShort(source1),
			         sourceToStringShort(descriptor.getTopLevelSource()), param);
		}
	}
	deluge::l10n::chosenLanguage = language;
}

/// GOLD body: version, open, the mod button lit (127 if none), then for each mod button 0-7 its upper knob and its
/// lower knob: flags (1 bipolar, 4 nothing assigned), position (0 to 127), the value the Deluge shows (2 bytes, plus
/// 8192) and the English name
int32_t buildGold(uint8_t* out) {
	int32_t i = 0;
	out[i++] = kStateVersion;
	Sound* sound = nullptr;
	ParamManagerForTimeline* params = nullptr;
	if (!currentSoundAndParams(sound, params)) {
		out[i++] = 0;
		return i;
	}
	out[i++] = 1;
	uint8_t* modeAt = sound->getModKnobMode();
	out[i++] = (modeAt != nullptr && *modeAt < kNumModButtons) ? *modeAt : 127;
	PatchedParamSet* patched = params->getPatchedParamSet();
	UnpatchedParamSet* unpatched = params->getUnpatchedParamSet();
	auto* cables = static_cast<PatchCableSet*>(params->getPatchCableSetSummary()->paramCollection);
	for (int32_t button = 0; button < kNumModButtons; button++) {
		for (int32_t w = 1; w >= 0; w--) {
			const ParamDescriptor descriptor = sound->modKnobs[button][w].paramDescriptor;
			int32_t knobPos = 0;
			bool found = false;
			P::Kind kind = P::Kind::NONE;
			int32_t paramId = 0;
			if (descriptor.isNull()) {}
			else if (descriptor.isJustAParam()) {
				const int32_t p = descriptor.getJustTheParam();
				if (p >= P::UNPATCHED_START) {
					kind = P::Kind::UNPATCHED_SOUND;
					paramId = p - P::UNPATCHED_START;
					if (unpatched != nullptr && paramId < P::kMaxNumUnpatchedParams) {
						const int32_t value = unpatched->getValue(paramId);
						knobPos = value >= static_cast<int32_t>(0x80000000u - (1u << 24)) ? 64 : (value + (1 << 24)) >> 25;
						found = true;
					}
				}
				else if (patched != nullptr && p < P::kNumParams) {
					kind = P::Kind::PATCHED;
					paramId = p;
					const int32_t value = patched->getValue(p);
					if (p == P::LOCAL_OSC_A_PHASE_WIDTH || p == P::LOCAL_OSC_B_PHASE_WIDTH) {
						knobPos = (value >> 24) - 64;
					}
					else {
						knobPos = value >= static_cast<int32_t>(0x80000000u - (1u << 24)) ? 64 : (value + (1 << 24)) >> 25;
					}
					found = true;
				}
			}
			else if (cables != nullptr) {
				kind = P::Kind::PATCH_CABLE;
				paramId = static_cast<int32_t>(descriptor.data);
				const uint8_t c =
				    cables->getPatchCableIndex(descriptor.getBottomLevelSource(), descriptor.getDestination());
				knobPos = c == 255 ? 0 : (cables->patchCables[c].param.getCurrentValue() >> 23) - 64;
				found = true;
			}
			knobPos = std::clamp<int32_t>(knobPos, -64, 64);
			const int32_t shown = found ? view.calculateKnobPosForDisplay(kind, paramId, knobPos + 64) : 0;
			const bool bipolar = found && view.calculateKnobPosForDisplay(kind, paramId, 0) < 0;
			out[i++] = (bipolar ? 1 : 0) | (descriptor.isNull() ? 4 : 0);
			out[i++] = static_cast<uint8_t>(std::clamp<int32_t>(knobPos + 64, 0, 127));
			const int32_t v = std::clamp<int32_t>(shown + 8192, 0, 16383);
			out[i++] = (v >> 7) & 0x7F;
			out[i++] = v & 0x7F;
			char name[kStateNameMax + 1];
			goldKnobName(sound, descriptor, name, sizeof(name));
			i += putName(out + i, name);
		}
	}
	return i;
}

// ---- Transport, song (session clips and outputs), arranger ----
uint8_t lastTransport[12];
int32_t lastTransportLen = 0;
constexpr int32_t kMaxOutputs = 48;
constexpr int32_t kMaxClips = 64;
constexpr int32_t kSongNameMax = 12;
constexpr int32_t kMaxInstances = 64;
uint8_t songBody[4 + kMaxClips * (12 + kSongNameMax)];
int32_t songBodyLen = 0;
uint8_t lastOutputs[4 + kMaxOutputs * (6 + kSongNameMax)];
int32_t lastOutputsLen = 0;
uint8_t lastClips[sizeof(songBody)];
int32_t lastClipsLen = 0;
uint8_t lastArranger[16 + kMaxOutputs + kMaxInstances * 13];
uint8_t lastClipPos[4 + kMaxClips * 2];
int32_t lastClipPosLen = 0;
bool refreshPanels = false; ///< Keepalive: resend the picker, and any closed panel, in case one was missed
int32_t lastArrangerLen = 0;
int32_t lastArrangerPos = -2;

/// What the Deluge is showing, for the tablet: 0 other, 1 song rows, 2 song grid, 3 arranger, 4 instrument clip,
/// 5 audio clip, 6 keyboard, 7 automation, 8 performance
uint8_t viewCode() {
	UI* root = getRootUI();
	if (root == nullptr) {
		return 0;
	}
	switch (root->getUIType()) {
	case UIType::SESSION:
		return currentSong->sessionLayout == SessionLayoutType::SessionLayoutTypeGrid ? 2 : 1;
	case UIType::ARRANGER:
		return 3;
	case UIType::INSTRUMENT_CLIP:
		return 4;
	case UIType::AUDIO_CLIP:
		return 5;
	case UIType::KEYBOARD_SCREEN:
		return 6;
	case UIType::AUTOMATION:
		return 7;
	case UIType::PERFORMANCE:
		return 8;
	default:
		return 0;
	}
}

/// TRANSPORT body: version, flags (1 playing, 2 recording, 4 arrangement playing, 8 metronome, 16 recording to the
/// arrangement), tempo times 100 (3 bytes), view code
int32_t buildTransport(uint8_t* out) {
	int32_t i = 0;
	const bool playing = playbackHandler.isEitherClockActive();
	out[i++] = kStateVersion;
	out[i++] = (playing ? 1 : 0) | (playbackHandler.isCurrentlyRecording() ? 2 : 0)
	           | (playing && currentPlaybackMode == &arrangement ? 4 : 0) | (playbackHandler.metronomeOn ? 8 : 0)
	           | (playbackHandler.recording == RecordingMode::ARRANGEMENT ? 16 : 0);
	const int32_t bpm = std::clamp<int32_t>(
	    static_cast<int32_t>(playbackHandler.calculateBPMForDisplay() * 100.0f + 0.5f), 0, (1 << 21) - 1);
	out[i++] = (bpm >> 14) & 0x7F;
	out[i++] = (bpm >> 7) & 0x7F;
	out[i++] = bpm & 0x7F;
	out[i++] = viewCode();
	return i;
}

/// OUTPUTS body: version, count, index of the current one (127 none), then per output (arranger order, top first):
/// type, flags (1 muted in the arranger, 2 soloing in the arranger, 4 has a clip active), colour r g b (7 bits each),
/// name
int32_t buildOutputs(uint8_t* out) {
	int32_t i = 0;
	out[i++] = kStateVersion;
	const int32_t countAt = i++;
	const int32_t currentAt = i++;
	out[currentAt] = 127;
	Output* current = getCurrentClip() != nullptr ? getCurrentClip()->output : nullptr;
	int32_t n = 0;
	for (Output* o = currentSong->firstOutput; o != nullptr && n < kMaxOutputs; o = o->next, n++) {
		if (o == current) {
			out[currentAt] = n;
		}
		out[i++] = static_cast<uint8_t>(o->type) & 0x7F;
		out[i++] = (o->mutedInArrangementMode ? 1 : 0) | (o->soloingInArrangementMode ? 2 : 0)
		           | (o->getActiveClip() != nullptr ? 4 : 0);
		const RGB rgb = RGB::fromHue(o->colour);
		out[i++] = rgb.r >> 1;
		out[i++] = rgb.g >> 1;
		out[i++] = rgb.b >> 1;
		char name[kSongNameMax + 1];
		strncpy(name, o->name.get(), kSongNameMax);
		name[kSongNameMax] = 0;
		i += putName(out + i, name);
	}
	out[countAt] = n;
	return i;
}

/// Where a clip has notes: 32 bits across its loop, bit n set if a note starts in that 32nd. Audio clips: all set.
uint32_t clipPattern(Clip* clip) {
	if (clip == nullptr) {
		return 0;
	}
	if (clip->type != ClipType::INSTRUMENT) {
		return 0xFFFFFFFF;
	}
	auto* instrumentClip = static_cast<InstrumentClip*>(clip);
	const int64_t length = std::max<int32_t>(clip->loopLength, 1);
	uint32_t bits = 0;
	int32_t looked = 0;
	for (int32_t r = 0; r < instrumentClip->noteRows.getNumElements() && bits != 0xFFFFFFFF; r++) {
		NoteRow* row = instrumentClip->noteRows.getElement(r);
		for (int32_t n = 0; n < row->notes.getNumElements() && looked < 2048; n++, looked++) {
			const int64_t bin = static_cast<int64_t>(row->notes.getElement(n)->pos) * 32 / length;
			if (bin >= 0 && bin < 32) {
				bits |= 1u << bin;
			}
		}
	}
	return bits;
}

/// 32 bits as 5 bytes of 7
int32_t putPattern(uint8_t* out, uint32_t bits) {
	for (int32_t b = 0; b < 5; b++) {
		out[b] = (bits >> (7 * b)) & 0x7F;
	}
	return 5;
}

/// CLIPS body: version, count, then per session clip (rows view order): output index, section (127 none), flags
/// (1 playing, 2 launch or stop pending, 4 audio, 8 soloing, 16 the current clip), note pattern (5 bytes, see
/// clipPattern), its section pad's colour (r g b, 7 bits each), name
int32_t buildClips(uint8_t* out) {
	int32_t i = 0;
	out[i++] = kStateVersion;
	const int32_t countAt = i++;
	const int32_t total = std::min<int32_t>(currentSong->sessionClips.getNumElements(), kMaxClips);
	Clip* current = getCurrentClip();
	for (int32_t c = 0; c < total; c++) {
		Clip* clip = currentSong->sessionClips.getClipAtIndex(c);
		const int32_t outputIndex = clip->output != nullptr ? currentSong->getOutputIndex(clip->output) : 127;
		out[i++] = static_cast<uint8_t>(std::clamp<int32_t>(outputIndex, 0, 127));
		out[i++] = clip->section < kMaxNumSections ? clip->section : 127;
		out[i++] = (currentSong->isClipActive(clip) ? 1 : 0) | (clip->armState != ArmState::OFF ? 2 : 0)
		           | (clip->type == ClipType::AUDIO ? 4 : 0) | (clip->soloingInSessionMode ? 8 : 0)
		           | (clip == current ? 16 : 0);
		i += putPattern(out + i, clipPattern(clip));
		const RGB section =
		    clip->section < kMaxNumSections ? RGB(defaultClipSectionColours[clip->section]) : RGB(0, 0, 0);
		out[i++] = section.r >> 1;
		out[i++] = section.g >> 1;
		out[i++] = section.b >> 1;
		char name[kSongNameMax + 1];
		strncpy(name, clip->name.get(), kSongNameMax);
		name[kSongNameMax] = 0;
		i += putName(out + i, name);
	}
	out[countAt] = total;
	return i;
}

/// CLIP_POS body: version, count, then per playing session clip its index and where it is in its loop (0 to 127)
int32_t buildClipPositions(uint8_t* out) {
	int32_t i = 0;
	out[i++] = kStateVersion;
	const int32_t countAt = i++;
	int32_t count = 0;
	if (playbackHandler.isEitherClockActive() && currentPlaybackMode != &arrangement) {
		const int32_t total = std::min<int32_t>(currentSong->sessionClips.getNumElements(), kMaxClips);
		for (int32_t c = 0; c < total; c++) {
			Clip* clip = currentSong->sessionClips.getClipAtIndex(c);
			if (!currentSong->isClipActive(clip) || clip->loopLength <= 0) {
				continue;
			}
			const int64_t pos = static_cast<int64_t>(clip->getLivePos()) % clip->loopLength;
			out[i++] = c;
			out[i++] = static_cast<uint8_t>(std::clamp<int64_t>(pos * 128 / clip->loopLength, 0, 127));
			count++;
		}
	}
	out[countAt] = count;
	return i;
}

/// The arranger window the pads show: its start and width in ticks
void arrangerWindow(int64_t& start, int64_t& width) {
	start = currentSong->xScroll[NAVIGATION_ARRANGEMENT];
	width = std::max<int64_t>(static_cast<int64_t>(currentSong->xZoom[NAVIGATION_ARRANGEMENT]) * kDisplayWidth, 1);
}

/// 14-bit fraction of the window, clamped to its edges
int32_t windowFraction(int64_t pos, int64_t start, int64_t width) {
	return static_cast<int32_t>(std::clamp<int64_t>((pos - start) * 16383 / width, 0, 16383));
}

void putTicks(uint8_t* out, int64_t ticks) {
	for (int32_t b = 3; b >= 0; b--) {
		*out++ = (std::max<int64_t>(ticks, 0) >> (7 * b)) & 0x7F;
	}
}

/// ARRANGER body: version, window start and width in ticks (4 bytes each, 7 bits a byte), arrangement length (4
/// bytes), then per output (as OUTPUTS) the number of its clip instances in the window and for each: start and end as
/// 14-bit fractions of the window, its clip's loop length as one too (at least 1), where the window cuts into its
/// loop (same units), and the clip's note pattern
int32_t buildArranger(uint8_t* out) {
	int32_t i = 0;
	out[i++] = kStateVersion;
	int64_t start;
	int64_t width;
	arrangerWindow(start, width);
	putTicks(out + i, start);
	i += 4;
	putTicks(out + i, width);
	i += 4;
	putTicks(out + i, arrangerView.getMaxLength());
	i += 4;
	int32_t instances = 0;
	int32_t n = 0;
	for (Output* o = currentSong->firstOutput; o != nullptr && n < kMaxOutputs; o = o->next, n++) {
		const int32_t countAt = i++;
		int32_t count = 0;
		for (int32_t k = 0; k < o->clipInstances.getNumElements() && instances < kMaxInstances; k++) {
			ClipInstance* instance = o->clipInstances.getElement(k);
			const int64_t a = instance->pos;
			const int64_t e = a + instance->length;
			if (e <= start || a >= start + width) {
				continue; // Not in the window
			}
			const int64_t loop =
			    std::max<int64_t>(instance->clip != nullptr ? instance->clip->loopLength : instance->length, 1);
			const int32_t values[4] = {windowFraction(a, start, width), windowFraction(e, start, width),
			                           static_cast<int32_t>(std::clamp<int64_t>(loop * 16383 / width, 1, 16383)),
			                           static_cast<int32_t>(std::clamp<int64_t>(
			                               std::max<int64_t>(start - a, 0) % loop * 16383 / width, 0, 16383))};
			for (int32_t v : values) {
				out[i++] = (v >> 7) & 0x7F;
				out[i++] = v & 0x7F;
			}
			i += putPattern(out + i, clipPattern(instance->clip));
			count++;
			instances++;
		}
		out[countAt] = count;
	}
	return i;
}

/// Where the beat is, 0-16383 over four quarter notes: from the playhead while playing, else at the song's tempo. The
/// same sum the section pads use (session_view.cpp beatPhase), so the tablet's glows run with them.
int32_t beatPhase4() {
	const uint64_t quarterNote = currentSong->getQuarterNoteLength();
	uint64_t position;
	uint64_t period;
	if (playbackHandler.isEitherClockActive()) {
		position = static_cast<uint64_t>(std::max<int64_t>(playbackHandler.getActualSwungTickCount(), 0));
		period = quarterNote * 4;
	}
	else {
		position = AudioEngine::audioSampleTimer;
		period = ((currentSong->timePerTimerTickBig * quarterNote) >> 32) * 4;
	}
	return (period == 0) ? 0 : static_cast<int32_t>((position % period) * 16384 / period);
}

constexpr int32_t kMaxKitSampleRows = 32;
constexpr int32_t kKitFileMax = 20;
uint8_t lastKitSamples[4 + kMaxKitSampleRows * (4 + kSongNameMax + kKitFileMax)];
int32_t lastKitSamplesLen = 0;
uint8_t lastKitActive[8];
int32_t lastKitActiveLen = 0;
int32_t lastBeat = -1000;
int32_t pendingKitRow = -1; ///< Kit row whose sample to open (index in the clip's note rows)

/// The kit clip being played, or nullptr
InstrumentClip* currentKitClip() {
	Clip* clip = getCurrentClip();
	if (clip == nullptr || clip->type != ClipType::INSTRUMENT || clip->output == nullptr
	    || clip->output->type != OutputType::KIT) {
		return nullptr;
	}
	return static_cast<InstrumentClip*>(clip);
}

/// The sample a kit row plays from its first oscillator, or nullptr
AudioFile* rowSample(NoteRow* row) {
	if (row == nullptr || row->drum == nullptr || row->drum->type != DrumType::SOUND) {
		return nullptr;
	}
	auto* sound = static_cast<SoundDrum*>(row->drum);
	Source& source = sound->sources[0];
	if (sound->getSynthMode() == SynthMode::FM || source.oscType != OscType::SAMPLE
	    || source.ranges.getNumElements() < 1) {
		return nullptr;
	}
	AudioFileHolder* holder = source.ranges.getElement(0)->getAudioFileHolder();
	return holder != nullptr ? holder->audioFile : nullptr;
}

/// KIT_SAMPLES body: version, count, then per kit row playing a sample (note row order, bottom row first): its row
/// index, flags (2 selected), its name, and the sample's file name. KIT_ACTIVE: version, then 5 bytes of 7 bits, bit
/// n set while the nth of those rows is sounding.
int32_t buildKitSamples(uint8_t* out, uint8_t* active, int32_t& activeLen) {
	int32_t i = 0;
	out[i++] = kStateVersion;
	const int32_t countAt = i++;
	int32_t count = 0;
	uint32_t sounding = 0;
	InstrumentClip* clip = currentKitClip();
	if (clip != nullptr) {
		Drum* selected = static_cast<Kit*>(clip->output)->selectedDrum;
		for (int32_t r = 0; r < clip->noteRows.getNumElements() && count < kMaxKitSampleRows; r++) {
			NoteRow* row = clip->noteRows.getElement(r);
			AudioFile* file = rowSample(row);
			if (file == nullptr) {
				continue;
			}
			auto* sound = static_cast<SoundDrum*>(row->drum);
			out[i++] = std::min<int32_t>(r, 127);
			out[i++] = row->drum == selected ? 2 : 0;
			char name[kSongNameMax + 1];
			strncpy(name, sound->drumName.c_str(), kSongNameMax);
			name[kSongNameMax] = 0;
			i += putName(out + i, name);
			char const* path = file->filePath.get();
			char const* slash = strrchr(path, '/');
			char fileName[kKitFileMax + 1];
			strncpy(fileName, slash != nullptr ? slash + 1 : path, kKitFileMax);
			fileName[kKitFileMax] = 0;
			i += putName(out + i, fileName);
			if (sound->hasActiveVoices()) {
				sounding |= 1u << count;
			}
			count++;
		}
	}
	out[countAt] = count;
	active[0] = kStateVersion;
	activeLen = 1 + putPattern(active + 1, sounding);
	return i;
}

/// Send body under header if it changed from last (and remember it). Returns false if there wasn't room.
bool sendIfChanged(uint8_t const* header, uint8_t const* body, int32_t len, uint8_t* last, int32_t& lastLen, bool force,
                   int32_t space) {
	if (!force && len == lastLen && memcmp(body, last, len) == 0) {
		return true;
	}
	if (!sendFramed(*midiStateCable, header, false, body, len)) {
		return false;
	}
	memcpy(last, body, len);
	lastLen = len;
	return true;
}

/// The rest of the companion screen, after the waveform: knobs, transport, song and arranger. Returns true if it
/// should be polled (playback running, or a song view open), as these change without the display redrawing.
bool sendSongAndKnobs(bool force) {
	// Never while the card routine runs or a browser is open: loading a preset or song swaps sounds, params and outputs
	// over several steps, and redraws during it would read them half-built
	if (currentSong == nullptr || sdRoutineLock || browserIsOpen()) {
		return false;
	}
	if (force) {
		lastParamsLen = lastTransportLen = lastOutputsLen = lastClipsLen = lastArrangerLen = lastClipPosLen = 0;
		lastArrangerPos = -2;
	}
	uint8_t transport[sizeof(lastTransport)];
	const int32_t transportLen = buildTransport(transport);
	sendIfChanged(kTransportHeader, transport, transportLen, lastTransport, lastTransportLen, force, 40);

	paramsBodyLen = buildParams(paramsBody);
	sendIfChanged(kParamsHeader, paramsBody, paramsBodyLen, lastParams, lastParamsLen, force, 320);
	if (force) {
		lastGoldLen = 0;
	}
	uint8_t gold[kGoldBodyMax];
	const int32_t goldLen = buildGold(gold);
	sendIfChanged(kGoldHeader, gold, goldLen, lastGold, lastGoldLen, force, 0);

	if (force) {
		lastKitSamplesLen = lastKitActiveLen = 0;
		lastBeat = -1000;
	}
	uint8_t kitSamples[sizeof(lastKitSamples)];
	uint8_t kitActive[sizeof(lastKitActive)];
	int32_t kitActiveLen = 0;
	const int32_t kitSamplesLen = buildKitSamples(kitSamples, kitActive, kitActiveLen);
	sendIfChanged(kKitSamplesHeader, kitSamples, kitSamplesLen, lastKitSamples, lastKitSamplesLen, force, 0);
	sendIfChanged(kKitActiveHeader, kitActive, kitActiveLen, lastKitActive, lastKitActiveLen, force, 0);

	uint8_t outputs[sizeof(lastOutputs)];
	const int32_t outputsLen = buildOutputs(outputs);
	sendIfChanged(kOutputsHeader, outputs, outputsLen, lastOutputs, lastOutputsLen, force, 320);

	const uint8_t view = viewCode();
	const bool songView = view == 1 || view == 2 || view == 3;
	if (songView) {
		// The beat, for the section glows: sent when it has moved on, the tablet runs it on at the tempo in between
		const int32_t beat = beatPhase4();
		const int32_t moved = (beat - lastBeat + 16384) % 16384;
		if (force || moved > 1024 || moved < 0) {
			const uint8_t body[3] = {kStateVersion, static_cast<uint8_t>((beat >> 7) & 0x7F),
			                         static_cast<uint8_t>(beat & 0x7F)};
			if (sendFramed(*midiStateCable, kBeatHeader, false, body, 3)) {
				lastBeat = beat;
			}
		}
	}
	if (view == 1 || view == 2 || force) {
		songBodyLen = buildClips(songBody);
		sendIfChanged(kClipsHeader, songBody, songBodyLen, lastClips, lastClipsLen, force, 400);
		uint8_t positions[sizeof(lastClipPos)];
		const int32_t positionsLen = buildClipPositions(positions);
		sendIfChanged(kClipPosHeader, positions, positionsLen, lastClipPos, lastClipPosLen, force, 60);
	}
	if (view == 3 || force) {
		uint8_t arranger[sizeof(lastArranger)];
		const int32_t arrangerLen = buildArranger(arranger);
		sendIfChanged(kArrangerHeader, arranger, arrangerLen, lastArranger, lastArrangerLen, force, 400);
		const bool arrangementPlaying = playbackHandler.isEitherClockActive() && currentPlaybackMode == &arrangement;
		int64_t start;
		int64_t width;
		arrangerWindow(start, width);
		const int64_t live = arrangementPlaying ? static_cast<int64_t>(arrangement.getLivePos()) : -1;
		// 0x3FFF = not playing, 0x3FFE = off the window
		const int32_t pos = live < 0                                  ? 0x3FFF
		                    : (live < start || live >= start + width) ? 0x3FFE
		                                                              : windowFraction(live, start, width);
		if (pos != lastArrangerPos) {
			const uint8_t body[3] = {kStateVersion, static_cast<uint8_t>((pos >> 7) & 0x7F),
			                         static_cast<uint8_t>(pos & 0x7F)};
			if (sendFramed(*midiStateCable, kArrangerPosHeader, false, body, 3)) {
				lastArrangerPos = pos;
			}
		}
	}
	return songView || playbackHandler.isEitherClockActive() || (paramsBodyLen > 2) || kitSamplesLen > 2;
}

// ---- Tablet actions, run from the COMPANION_PICK UI timer (MIDI can arrive inside the SD card routine) ----
int32_t pendingMarkerType = -1; ///< Waveform drag: marker type (0-3 MarkerType, 6 start offset)
int32_t pendingMarkerFraction = 0;
int32_t pendingKnobX = -1; ///< Knob tap: its shortcut pad
int32_t pendingKnobY = 0;
int32_t pendingCommand = -1; ///< Top bar: 0 play, 1 record, 2 select, 3 back, 4 select left, 5 select right
bool pendingCommandDown = false;
int32_t pendingClip = -1;    ///< Session clip to launch or stop
int32_t pendingSection = -1; ///< Section to launch
int32_t pendingModButton = -1; ///< Gold knob tap: the mod button to press
} // namespace

void HIDSysex::requestState(MIDICable& cable, uint8_t* data, int32_t len) {
	if (len < 3) {
		return;
	}
	if (data[2] >= 3 && data[2] <= 5) {
		// Preset browser: 3 <hi> <lo> = go to that item (absolute index, as in BROWSER), like turning the select knob
		// to it; 4 = press SELECT (load / open folder); 5 = press BACK. Run from the UI timer, like the picker.
		if (data[2] == 3) {
			if (len < 5) {
				return;
			}
			pendingBrowserItem = (data[3] << 7) | data[4];
			pendingBrowser = 1;
		}
		else {
			pendingBrowser = data[2] == 4 ? 2 : 3;
		}
		pendingButtonDown = false;
		uiTimerManager.setTimer(TimerName::COMPANION_PICK, 1);
		return;
	}
	if (data[2] == 12) {
		// 12 <button> = press that mod button, so the gold knobs turn its pair
		if (len >= 4 && data[3] < kNumModButtons) {
			pendingModButton = data[3];
			uiTimerManager.setTimer(TimerName::COMPANION_PICK, 1);
		}
		return;
	}
	if (data[2] == 11) {
		// 11 <row> = select that kit row and open its sample's waveform (SHIFT + the sample start pad)
		if (len >= 4) {
			pendingKitRow = data[3];
			uiTimerManager.setTimer(TimerName::COMPANION_PICK, 1);
		}
		return;
	}
	if (data[2] >= 6 && data[2] <= 10) {
		// 6 <marker> <hi> <lo> = drag a waveform marker to that 14-bit fraction of the window; 7 <x> <y> = open the
		// setting on that shortcut pad (a knob tap); 8 <command> = top bar button; 9 <hi> <lo> = launch or stop that
		// session clip; 10 <section> = launch that section
		switch (data[2]) {
		case 6:
			if (len < 6) {
				return;
			}
			pendingMarkerType = data[3];
			pendingMarkerFraction = (data[4] << 7) | data[5];
			break;
		case 7:
			if (len < 5) {
				return;
			}
			pendingKnobX = data[3];
			pendingKnobY = data[4];
			break;
		case 8:
			if (len < 4) {
				return;
			}
			pendingCommand = data[3];
			pendingCommandDown = false;
			break;
		case 9:
			if (len < 5) {
				return;
			}
			pendingClip = (data[3] << 7) | data[4];
			break;
		default:
			if (len < 4) {
				return;
			}
			pendingSection = data[3];
			break;
		}
		uiTimerManager.setTimer(TimerName::COMPANION_PICK, 1);
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
	refreshPanels = true;
	midiStateCable = &cable;
	midiStateUntil = AudioEngine::audioSampleTimer + 2 * kSampleRate;
	// Mode 0, or a keepalive that starts a new subscription, sends everything
	sendStateIfChanged(data[2] == 0 || !wasActive);
}

void HIDSysex::sendStateIfChanged(bool force) {
	if (midiStateCable == nullptr || AudioEngine::audioSampleTimer > midiStateUntil || currentSong == nullptr) {
		return;
	}
	// Room for a burst (sendBufferSpace() counts bytes; each message also checks it has room for all of itself)
	if (midiStateCable->sendBufferSpace() < 400) {
		uiTimerManager.setTimer(TimerName::SYSEX_DISPLAY, 100);
		return;
	}

	uint8_t body[96];
	const int32_t len = buildState(body);
	if (force || len != lastStateLen || memcmp(body, lastState, len) != 0) {
		if (sendFramed(*midiStateCable, kStateHeader, true, body, len)) {
			memcpy(lastState, body, len);
			lastStateLen = len;
		}
	}

	uint8_t rows[kStateMaxRows + 2];
	const int32_t rowsLen = buildRows(rows);
	if (rowsLen > 0 && (force || rowsLen != lastRowsLen || memcmp(rows, lastRows, rowsLen) != 0)) {
		if (sendFramed(*midiStateCable, kRowsHeader, false, rows, rowsLen)) {
			memcpy(lastRows, rows, rowsLen);
			lastRowsLen = rowsLen;
		}
	}
	else {
		lastRowsLen = rowsLen;
	}

	uint8_t text[sizeof(lastText)];
	const int32_t textLen = buildText(text);
	if (force || textLen != lastTextLen || memcmp(text, lastText, textLen) != 0) {
		if (sendFramed(*midiStateCable, kTextHeader, false, text, textLen)) {
			memcpy(lastText, text, textLen);
			lastTextLen = textLen;
		}
	}

	if (force) {
		pickerKeyMenu = nullptr; // Rebuild and resend whatever is open
	}
	// Once a second (each keepalive) the picker goes again, open or closed, so one that was missed (a full buffer,
	// a redraw that didn't come) shows up without reloading the page
	const bool refresh = refreshPanels;
	refreshPanels = false;
	if (updatePicker() || ((force || refresh) && pickerBodyLen > 0)) {
		if (!sendFramed(*midiStateCable, kPickerHeader, false, pickerBody, pickerBodyLen)) {
			refreshPanels = true; // Try again next time round
			uiTimerManager.setTimer(TimerName::COMPANION_WAVE, 40);
		}
	}

	// The browser window is up to about 1300 bytes: wait for room
	if (midiStateCable->sendBufferSpace() < 1350) {
		uiTimerManager.setTimer(TimerName::SYSEX_DISPLAY, 100);
		return;
	}
	if (force) {
		browserKeyCount = -1; // Resend the whole window
	}
	const int32_t browserSend = updateBrowser();
	if (browserSend == 1) {
		if (!sendFramed(*midiStateCable, kBrowserHeader, false, browserBody, browserBodyLen)) {
			browserKeyCount = -1; // Build and send it again next time round
			uiTimerManager.setTimer(TimerName::COMPANION_WAVE, 40);
		}
	}
	else if (browserSend == 2) {
		uint8_t sel[3] = {kStateVersion,
		                  static_cast<uint8_t>((browserKeySelected < 0 ? 0x3FFF : browserKeySelected) >> 7 & 0x7F),
		                  static_cast<uint8_t>((browserKeySelected < 0 ? 0x3FFF : browserKeySelected) & 0x7F)};
		if (!sendFramed(*midiStateCable, kBrowserSelHeader, false, sel, 3)) {
			browserKeySelected = -2;
			uiTimerManager.setTimer(TimerName::COMPANION_WAVE, 40);
		}
	}

	// Knobs, transport, song and arranger. Polled while playing or in a song view, as they change without redraws.
	const bool pollSong = sendSongAndKnobs(force);

	// Waveform: the biggest message (about 460 bytes). Polled from a timer while open, as scrolling and the playhead
	// don't always redraw the display.
	const WaveView view = currentWaveView();
	if (view.kind != 0 || pollSong) {
		uiTimerManager.setTimer(TimerName::COMPANION_WAVE, 40);
	}
	if (view.kind == 0 && !waveWasOpen) {
		return;
	}
	if (midiStateCable->sendBufferSpace() < 250) {
		uiTimerManager.setTimer(TimerName::COMPANION_WAVE, 60);
		return;
	}
	if (force) {
		lastWaveLen = 0;
		lastPlayhead = -2;
	}
	const int32_t waveSend = updateWave(view);
	const bool changed = waveBodyLen != lastWaveLen || memcmp(waveBody, lastWave, waveBodyLen) != 0;
	// While columns are still loading, send at most every 150 ms
	const bool due = waveSend == 1 || AudioEngine::audioSampleTimer - waveLastSendTime > kSampleRate * 3 / 20;
	if (waveSend != 0 && changed && due) {
		if (sendFramed(*midiStateCable, kWaveHeader, false, waveBody, waveBodyLen)) {
			memcpy(lastWave, waveBody, waveBodyLen);
			lastWaveLen = waveBodyLen;
			waveLastSendTime = AudioEngine::audioSampleTimer;
		}
	}
	if (view.kind != 0) {
		// DIAG body: version, then the line (up to 120 characters)
		char diag[sizeof(lastDiag)];
		buildDiag(view, diag, sizeof(diag));
		if (strcmp(diag, lastDiag) != 0) {
			uint8_t body[2 + 121];
			body[0] = kStateVersion;
			const int32_t n = std::min<int32_t>(strlen(diag), 120);
			body[1] = n;
			for (int32_t c = 0; c < n; c++) {
				body[2 + c] = diag[c] & 0x7F;
			}
			if (sendFramed(*midiStateCable, kDiagHeader, false, body, 2 + n)) {
				strncpy(lastDiag, diag, sizeof(lastDiag) - 1);
			}
		}
	}
	if (view.kind != 0) {
		// PLAYHEAD body: version, count, then each playhead's column (2 bytes): the clip's, or each voice's
		const int64_t subZoom = std::max<int64_t>(1, view.xZoom / kWaveSub);
		uint8_t body[2 + 2 * 5];
		int32_t n = 0;
		body[0] = kStateVersion;
		if (view.playhead >= 0) {
			putCol(body + 2 + 2 * n++, waveCol(view.playhead, view.xScroll, subZoom));
		}
		for (int32_t h = 0; h < view.numVoiceHeads && n < 5; h++) {
			putCol(body + 2 + 2 * n++, waveCol(view.voiceHeads[h], view.xScroll, subZoom));
		}
		body[1] = n;
		uint32_t key = n;
		for (int32_t b = 2; b < 2 + 2 * n; b++) {
			key = key * 131 + body[b];
		}
		const int32_t head = static_cast<int32_t>(key & 0x7FFFFFFF);
		if (head != lastPlayhead) {
			if (sendFramed(*midiStateCable, kPlayheadHeader, false, body, 2 + 2 * n)) {
				lastPlayhead = head;
			}
		}
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

namespace {
/// The tablet's browser action, from the UI timer. Returns true if it wants to run again.
bool runPendingBrowser() {
	if (pendingBrowser == 0) {
		return false;
	}
	if (sdRoutineLock) {
		return true; // Card routine running: try again after it
	}
	if (!browserIsOpen() || currentUIMode != UI_MODE_NONE) {
		pendingBrowser = 0;
		pendingButtonDown = false;
		return false;
	}
	if (pendingBrowser == 1) {
		// Step the select knob by the distance to the tapped item, only when it's in the loaded list and close
		const int32_t selectedLocal = Browser::getFileIndexSelected();
		const int32_t targetLocal = pendingBrowserItem - Browser::numFileItemsDeletedAtStart;
		pendingBrowser = 0;
		if (selectedLocal < 0 || targetLocal < 0 || targetLocal >= Browser::fileItems.getNumElements()) {
			return false;
		}
		const int32_t offset = targetLocal - selectedLocal;
		if (offset == 0 || offset > 127 || offset < -127) {
			return false;
		}
		getCurrentUI()->selectEncoderAction(static_cast<int8_t>(offset));
		return false;
	}
	// Press, then release on the next run, like a finger on the button
	const deluge::hid::Button button =
	    pendingBrowser == 2 ? deluge::hid::button::SELECT_ENC : deluge::hid::button::BACK;
	const ActionResult result = Buttons::buttonAction(button, !pendingButtonDown, sdRoutineLock);
	if (result == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE) {
		return true;
	}
	if (!pendingButtonDown) {
		pendingButtonDown = true;
		return true;
	}
	pendingButtonDown = false;
	pendingBrowser = 0;
	return false;
}
} // namespace

namespace {
/// The tablet's other actions. Returns true if one wants to run again.
bool runPendingActions() {
	if (sdRoutineLock) {
		return pendingMarkerType >= 0 || pendingKnobX >= 0 || pendingCommand >= 0 || pendingClip >= 0
		       || pendingSection >= 0 || pendingKitRow >= 0 || pendingModButton >= 0;
	}
	bool again = false;
	if (pendingModButton >= 0) {
		view.modButtonAction(pendingModButton, true);
		view.modButtonAction(pendingModButton, false);
		pendingModButton = -1;
	}
	if (pendingMarkerType >= 0) {
		const int32_t type = pendingMarkerType;
		pendingMarkerType = -1;
		const WaveView v = currentWaveView();
		const int64_t subZoom = std::max<int64_t>(1, v.xZoom / kWaveSub);
		const int64_t pos = v.xScroll + (pendingMarkerFraction * subZoom * kWaveCols) / 16384;
		if (v.kind == 2 && type <= 3) {
			sampleMarkerEditor.companionMoveMarker(static_cast<MarkerType>(type), pos);
		}
		else if (v.kind == 4 && type == 6 && v.sample != nullptr) {
			// Start offset: the setting that starts the sample there, through the menu item as a knob turn would
			auto* item = static_cast<deluge::gui::menu_item::sample::StartOffset*>(soundEditor.getCurrentMenuItem());
			const int64_t length = static_cast<int64_t>(v.sample->lengthInSamples);
			const int64_t start = v.markerPos[0];
			const int64_t param = ((pos - start) << 30) / std::max<int64_t>(length, 1);
			const int32_t value = static_cast<int32_t>(
			    std::clamp<int64_t>(param >> 22, -deluge::gui::menu_item::sample::StartOffset::kResolution,
			                        deluge::gui::menu_item::sample::StartOffset::kResolution));
			item->companionSet(value);
		}
	}
	if (pendingKitRow >= 0) {
		InstrumentClip* clip = currentKitClip();
		if (clip != nullptr && getRootUI() == &instrumentClipView && pendingKitRow < clip->noteRows.getNumElements()) {
			NoteRow* row = clip->noteRows.getElement(pendingKitRow);
			if (rowSample(row) != nullptr) {
				if (getCurrentUI() != getRootUI()) {
					// Leave whatever menu is open first, so the shortcut opens fresh for this row
					getCurrentUI()->close();
				}
				instrumentClipView.setSelectedDrum(row->drum, true);
				pendingKnobX = 0; // SAMPLE START pad, which opens the sample's markers
				pendingKnobY = 7;
			}
		}
		pendingKitRow = -1;
	}
	if (pendingKnobX >= 0) {
		// As holding SHIFT and pressing the shortcut pad
		const int32_t x = pendingKnobX;
		const int32_t y = pendingKnobY;
		Buttons::buttonAction(deluge::hid::button::SHIFT, true, false);
		const ActionResult result = soundEditor.potentialShortcutPadAction(x, y, true);
		soundEditor.potentialShortcutPadAction(x, y, false);
		Buttons::ignoreCurrentShiftForSticky();
		Buttons::buttonAction(deluge::hid::button::SHIFT, false, false);
		if (result == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE) {
			again = true;
		}
		else {
			pendingKnobX = -1;
		}
	}
	if (pendingCommand >= 0) {
		switch (pendingCommand) {
		case 0:
			playbackHandler.playButtonPressed(kInternalButtonPressLatency);
			pendingCommand = -1;
			break;
		case 1:
			playbackHandler.recordButtonPressed();
			pendingCommand = -1;
			break;
		case 2:
		case 3: {
			// Press, then let go on the next run, like a finger on the button
			const deluge::hid::Button button =
			    pendingCommand == 2 ? deluge::hid::button::SELECT_ENC : deluge::hid::button::BACK;
			const ActionResult result = Buttons::buttonAction(button, !pendingCommandDown, false);
			if (result == ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE) {
				again = true;
			}
			else if (!pendingCommandDown) {
				pendingCommandDown = true;
				again = true;
			}
			else {
				pendingCommandDown = false;
				pendingCommand = -1;
			}
			break;
		}
		case 4:
		case 5:
			if (getCurrentUI() != nullptr) {
				getCurrentUI()->selectEncoderAction(pendingCommand == 4 ? -1 : 1);
			}
			pendingCommand = -1;
			break;
		default:
			pendingCommand = -1;
			break;
		}
	}
	if (pendingClip >= 0) {
		if (pendingClip < currentSong->sessionClips.getNumElements()) {
			session.toggleClipStatus(currentSong->sessionClips.getClipAtIndex(pendingClip), nullptr, false,
			                         kInternalButtonPressLatency);
			uiNeedsRendering(getCurrentUI(), 0xFFFFFFFF, 0xFFFFFFFF);
		}
		pendingClip = -1;
	}
	if (pendingSection >= 0) {
		if (pendingSection < kMaxNumSections) {
			session.armSection(pendingSection, kInternalButtonPressLatency);
			uiNeedsRendering(getCurrentUI(), 0xFFFFFFFF, 0xFFFFFFFF);
		}
		pendingSection = -1;
	}
	return again;
}
} // namespace

void HIDSysex::runPendingPick() {
	const bool again = runPendingActions();
	if (runPendingBrowser() || again) {
		uiTimerManager.setTimer(TimerName::COMPANION_PICK, 20);
		return;
	}
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
