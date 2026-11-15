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
} // namespace deluge::l10n
