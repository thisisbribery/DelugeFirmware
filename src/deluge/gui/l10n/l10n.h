#pragma once
#include "gui/l10n/language.h"
#include "util/misc.h"
#include <array>
#include <cstddef>
#include <initializer_list>
#include <optional>

namespace deluge::l10n {
std::string_view getView(const deluge::l10n::Language& language, deluge::l10n::String string);
std::string_view getView(deluge::l10n::String string);
const char* get(const deluge::l10n::Language& language, deluge::l10n::String string);
const char* get(deluge::l10n::String string);

/// Which string a recent lookup in the chosen language returned this exact text for (same pointer and length), most
/// recent first, or -1. Identical texts share storage (BASS is "Bass" and "Bass Probability"), so the pointer alone
/// can't say which string the display was given: this remembers which one was asked for. Used by the companion screen.
int32_t recentLookupOf(std::string_view text);

/// As recentLookupOf, for a copy of the text: which of the last depth lookups had this exact text, or -1 if none, or if
/// two of them did and their English differs. For a value just copied out of a menu item (getNotificationValue).
int32_t recentLookupByContent(std::string_view text, size_t depth = 16);
} // namespace deluge::l10n
