#include "gui/l10n/l10n.h"
#include "gui/l10n/strings.h"
#include "util/misc.h"
#include <algorithm>

namespace deluge::l10n {
Language const* chosenLanguage = nullptr;

std::string_view getView(const deluge::l10n::Language& language, deluge::l10n::String string) {
	auto string_opt = language.get(string);
	if (string_opt.has_value()) {
		return string_opt.value();
	}

	if (language.hasFallback()) {
		return getView(language.fallback(), string);
	}

	return built_in::english.get(deluge::l10n::String::EMPTY_STRING).value(); // EMPTY_STRING
}

namespace {
// Ring of recent lookups in the chosen language: enough for a context menu's options and the screen around them
constexpr size_t kRecentLookups = 64;
struct RecentLookup {
	char const* data;
	uint16_t size;
	uint16_t string;
};
RecentLookup recentLookups[kRecentLookups]{};
size_t recentLookupNext = 0;
} // namespace

std::string_view getView(deluge::l10n::String string) {
	const std::string_view view = getView(*chosenLanguage, string);
	RecentLookup& slot = recentLookups[recentLookupNext];
	slot.data = view.data();
	slot.size = view.size();
	slot.string = util::to_underlying(string);
	recentLookupNext = (recentLookupNext + 1) % kRecentLookups;
	return view;
}

int32_t recentLookupOf(std::string_view text) {
	if (text.data() == nullptr) {
		return -1;
	}
	for (size_t n = 1; n <= kRecentLookups; n++) {
		const RecentLookup& slot = recentLookups[(recentLookupNext + kRecentLookups - n) % kRecentLookups];
		if (slot.data == text.data() && slot.size == text.size()) {
			return slot.string;
		}
	}
	return -1;
}

int32_t recentLookupByContent(std::string_view text, size_t depth) {
	if (text.empty()) {
		return -1;
	}
	depth = std::min(depth, kRecentLookups);
	int32_t found = -1;
	std::string_view foundEnglish;
	for (size_t n = 1; n <= depth; n++) {
		const RecentLookup& slot = recentLookups[(recentLookupNext + kRecentLookups - n) % kRecentLookups];
		if (slot.data == nullptr || std::string_view(slot.data, slot.size) != text) {
			continue;
		}
		const auto english = getView(built_in::english, static_cast<String>(slot.string));
		if (found < 0) {
			found = slot.string;
			foundEnglish = english;
		}
		else if (english != foundEnglish) {
			return -1; // Two recent strings with this text that mean different things: not certain
		}
	}
	return found;
}

const char* get(const Language& language, deluge::l10n::String string) {
	return getView(language, string).data();
}

const char* get(deluge::l10n::String string) {
	return getView(string).data();
}

} // namespace deluge::l10n

extern "C" {
#include "deluge.h"

const size_t l10n_STRING_FOR_USB_DEVICES_MAX = util::to_underlying(deluge::l10n::String::STRING_FOR_USB_DEVICES_MAX);
const size_t l10n_STRING_FOR_USB_DEVICE_DETACHED =
    util::to_underlying(deluge::l10n::String::STRING_FOR_USB_DEVICE_DETACHED);
const size_t l10n_STRING_FOR_USB_HUB_ATTACHED = util::to_underlying(deluge::l10n::String::STRING_FOR_USB_HUB_ATTACHED);
const size_t l10n_STRING_FOR_USB_DEVICE_NOT_RECOGNIZED =
    util::to_underlying(deluge::l10n::String::STRING_FOR_USB_DEVICE_NOT_RECOGNIZED);

char const* l10n_get(size_t string) {
	return deluge::l10n::get(static_cast<deluge::l10n::String>(string));
}
}
