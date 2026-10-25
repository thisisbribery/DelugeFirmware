/*
 * Copyright © 2026 the Deluge community.
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

#include "gui/context_menu/context_menu.h"
#include "model/instrument/preset_kit.h"
#include "modulation/params/param_manager.h"
#include <array>

class SoundDrum;

namespace deluge::gui::context_menu {

/// Shown by SHIFT + KIT on a preset kit (LXR or Cycles): its presets, the kit files in KITS/LXR or KITS/CYCLES. It
/// opens on the preset the kit has now. Turning SELECT plays each one on the kit's preset rows straight away, pattern
/// running; the other rows and the pattern stay. SELECT or KIT keeps the one showing. BACK puts the kit back as it was
/// and goes up to the Kit / LXR / Cycles menu. The list: Default (as SHIFT + KIT makes it), then the presets in name
/// order, with Current first if the kit isn't on any of them.
class KitPresets final : public ContextMenu {
public:
	KitPresets() = default;

	/// Read the preset list and find the kit's preset rows. false if there's nothing to browse.
	bool setup(preset_kit::PresetKit kit);

	char const* getTitle() override;
	std::span<char const*> getOptions() override;
	bool canSeeViewUnderneath() override { return true; }
	void selectEncoderAction(int8_t offset) override;
	bool acceptCurrentOption() override;
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	deluge::hid::Button getAcceptButton() override { return deluge::hid::button::KIT; }

private:
	static constexpr int32_t kMaxPresets = 48;
	static constexpr int32_t kMaxNameLength = 64;
	static constexpr int32_t kMaxRows = 16;

	[[nodiscard]] int32_t defaultOption() const { return hasCurrent_ ? 1 : 0; }
	[[nodiscard]] int32_t firstPresetOption() const { return defaultOption() + 1; }
	[[nodiscard]] int32_t numOptions() const { return firstPresetOption() + numPresets_; }
	void readPresetList();
	/// The option the kit is on now: its last kept preset, or a preset with the kit's name, or Default for a kit as
	/// SHIFT + KIT made it. -1 if none.
	int32_t findHome();
	/// Play option on the preset rows. The home option puts back what was there.
	void preview(int32_t option);
	void restoreRow(int32_t row);
	void restoreAll();
	void keep();
	void refresh();

	preset_kit::PresetKit kit_ = preset_kit::PresetKit::NONE;
	std::array<std::array<char, kMaxNameLength>, kMaxPresets> fileNames_{}; ///< With the .XML
	std::array<std::array<char, kMaxNameLength>, kMaxPresets> labels_{};    ///< Without
	int32_t numPresets_ = 0;
	bool hasCurrent_ = false;
	int32_t home_ = 0;
	std::array<char const*, kMaxPresets + 2> options_{};

	/// The clip's NoteRows that are preset rows, bottom first
	std::array<int32_t, kMaxRows> rows_{};
	int32_t numRows_ = 0;
	/// The sound each row had when the menu opened, taken off while a preset plays there; nullptr while it's on
	std::array<SoundDrum*, kMaxRows> originals_{};
	int32_t previewing_ = 0;

	std::array<SoundDrum*, kMaxRows> loaded_{};
	std::array<ParamManagerForTimeline, kMaxRows> loadedParamManagers_{};
};

extern KitPresets kitPresets;
} // namespace deluge::gui::context_menu
