/*
 * Copyright (c) 2014-2023 Synthstrom Audible Limited
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
// Sample start offset ported from the Owlet Deluge firmware (github.com/owlet-labs/DelugeFirmware, GPLv3)
#pragma once
#include "gui/menu_item/sample/utils.h"
#include "gui/menu_item/source/patched_param.h"
#include "gui/menu_item/toggle.h"
#include "gui/ui/sound_editor.h"
#include "modulation/params/param.h"
#include "modulation/params/param_set.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound.h"

namespace deluge::gui::menu_item::sample {

/// Bipolar, p-lockable start offset. Full scale (+/-512) moves the start by one whole sample length.
class StartOffset final : public source::PatchedParam {
public:
	static constexpr int32_t kResolution = 512; // Fits the 7-segment display ("-512")
	static constexpr int32_t kShift = 22;       // 31 - log2(512)

	StartOffset(l10n::String name, uint8_t source_id)
	    : source::PatchedParam(name, modulation::params::LOCAL_OSC_A_START_OFFSET, source_id) {}

	bool isRelevant(ModControllableAudio* modControllable, int32_t) const override {
		return isSampleModeSample(modControllable, source_id_);
	}

	[[nodiscard]] int32_t getMaxValue() const override { return kResolution; }
	[[nodiscard]] int32_t getMinValue() const override { return -kResolution; }

	void readCurrentValue() override {
		int32_t q31 = soundEditor.currentParamManager->getPatchedParamSet()->getValue(getP());
		this->setValue(q31 >> kShift);
	}

	int32_t getFinalValue() override {
		int32_t v = this->getValue();
		if (v >= kResolution) {
			return 2147483647;
		}
		if (v <= -kResolution) {
			return -2147483648;
		}
		return v << kShift;
	}

	// Faster encoder turns take bigger steps, slow turns keep single-step control
	void selectEncoderAction(int32_t offset) override {
		uint32_t now = AudioEngine::audioSampleTimer;
		uint32_t elapsed = now - lastTime_;
		lastTime_ = now;

		// Reset on direction change or a pause of more than ~180ms
		if (((offset > 0) != (lastDirection_ > 0)) || elapsed > 8000) {
			velocity_ = 1;
		}
		lastDirection_ = offset;

		int32_t mult = (elapsed < 1500) ? 12 : (elapsed < 3500) ? 6 : (elapsed < 6500) ? 3 : 1;
		velocity_ = (velocity_ + mult + 1) >> 1;

		source::PatchedParam::selectEncoderAction(offset * velocity_);
	}

	[[nodiscard]] RenderingStyle getRenderingStyle() const override { return KNOB; }

private:
	uint32_t lastTime_{0};
	int32_t lastDirection_{0};
	int32_t velocity_{1};
};

/// Whether the start offset wraps the playhead around the whole sample (on) or clamps at the sample edge (off)
class OffsetWrap final : public Toggle {
public:
	OffsetWrap(l10n::String name, uint8_t source_id) : Toggle(name), source_id_{source_id} {}

	bool isRelevant(ModControllableAudio* modControllable, int32_t) const override {
		return isSampleModeSample(modControllable, source_id_);
	}

	void readCurrentValue() override { setValue(soundEditor.currentSound->sources[source_id_].offsetWraps); }

	void writeCurrentValue() override {
		soundEditor.currentSound->killAllVoices();
		soundEditor.currentSound->sources[source_id_].offsetWraps = getValue();
	}

private:
	uint8_t source_id_;
};
} // namespace deluge::gui::menu_item::sample
