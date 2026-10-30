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

#include "gui/menu_item/horizontal_menu.h"
#include "gui/menu_item/integer.h"
#include "gui/ui_timer_manager.h"
#include "processing/engines/cpu_meter.h"
#include <algorithm>
#include <cstdint>
#include <limits>

namespace deluge::gui::menu_item::cpu {

/// Live render cost, refreshed twice a second:
/// - Load: audio render time as a % of real time, averaged over the last half second
/// - Peak: the highest Load seen since the menu was opened
/// - Voice: average nanoseconds per sample for one synth voice, filters and envelopes included
/// - Drum: average nanoseconds per sample for one drum synth oscillator alone
/// One sample lasts 22,676 ns, so Voice / 226.76 is one voice's share of the CPU in %.
class Load final : public HorizontalMenu {
public:
	class Metric final : public Integer {
	public:
		enum class Kind { LOAD, PEAK, VOICE_NS, DRUM_NS };

		Metric(Load& load, Kind kind, l10n::String name, l10n::String columnLabel)
		    : Integer(name), load_(load), kind_(kind), column_label_(columnLabel) {}

		void beginSession(MenuItem*) override {
			Integer::beginSession(nullptr);
			load_.startTimer();
		}

		ActionResult timerCallback() override { return load_.timerCallback(); }
		void selectEncoderAction(int32_t) override {}
		bool selectEncoderActionIsPermitted() override { return false; }

		void getColumnLabel(StringBuf& label) override { label.append(l10n::getView(column_label_)); }

		void readCurrentValue() override { setValue(metricValue()); }

	private:
		int32_t metricValue() const {
			switch (kind_) {
			case Kind::LOAD:
				return load_.load_percent_;
			case Kind::PEAK:
				return load_.peak_percent_;
			case Kind::VOICE_NS:
				return load_.voice_ns_;
			case Kind::DRUM_NS:
				return load_.drum_ns_;
			}
			return 0;
		}

		[[nodiscard]] int32_t getMaxValue() const override { return std::numeric_limits<int32_t>::max(); }
		[[nodiscard]] deluge::gui::menu_item::RenderingStyle getRenderingStyle() const override {
			return deluge::gui::menu_item::RenderingStyle::NUMBER;
		}

		Load& load_;
		Kind kind_;
		l10n::String column_label_;
	};

	Load(l10n::String name, l10n::String title)
	    : HorizontalMenu(name, title, {&load_, &peak_, &voice_, &drum_}, HorizontalMenu::Layout::FIXED),
	      load_(*this, Metric::Kind::LOAD, l10n::String::STRING_FOR_CPU_LOAD_PERCENT,
	            l10n::String::STRING_FOR_LOAD_SHORT),
	      peak_(*this, Metric::Kind::PEAK, l10n::String::STRING_FOR_CPU_PEAK_PERCENT,
	            l10n::String::STRING_FOR_PEAK_SHORT),
	      voice_(*this, Metric::Kind::VOICE_NS, l10n::String::STRING_FOR_CPU_VOICE_NS,
	             l10n::String::STRING_FOR_VOICE_NS_SHORT),
	      drum_(*this, Metric::Kind::DRUM_NS, l10n::String::STRING_FOR_CPU_DRUM_NS,
	            l10n::String::STRING_FOR_DRUM_NS_SHORT) {}

	void beginSession(MenuItem* navigatedBackwardFrom) override {
		peak_percent_ = 0;
		takeSnapshot();
		HorizontalMenu::beginSession(navigatedBackwardFrom);
		startTimer();
	}

	ActionResult timerCallback() override {
		sample();
		if (renderingStyle() == RenderingStyle::HORIZONTAL) {
			renderUIsForOled();
		}
		else {
			getCurrentItem()->readValueAgain();
		}
		startTimer();
		return ActionResult::DEALT_WITH;
	}

private:
	static constexpr uint32_t kRefreshIntervalMs = 500;

	void startTimer() { uiTimerManager.setTimer(TimerName::UI_SPECIFIC, kRefreshIntervalMs); }

	void takeSnapshot() {
		cpu_meter::rendersToTimeVoices = cpu_meter::kRendersPerRefresh;
		last_audio_ = cpu_meter::audio;
		last_voices_ = cpu_meter::voices;
		last_drum_ = cpu_meter::drumSynth;
	}

	static int32_t nsPerSample(const cpu_meter::Counter& now, const cpu_meter::Counter& then) {
		uint32_t samples = now.samples - then.samples;
		if (samples == 0) {
			return 0;
		}
		uint64_t ticks = now.ticks - then.ticks;
		return static_cast<int32_t>(ticks * 1000000000ull / (uint64_t{cpu_meter::kTicksPerSecond} * samples));
	}

	void sample() {
		uint32_t samples = cpu_meter::audio.samples - last_audio_.samples;
		if (samples != 0) {
			uint64_t ticks = cpu_meter::audio.ticks - last_audio_.ticks;
			load_percent_ = static_cast<int32_t>(ticks * 100 * cpu_meter::kSampleRate
			                                     / (uint64_t{cpu_meter::kTicksPerSecond} * samples));
			peak_percent_ = std::max(peak_percent_, load_percent_);
		}
		voice_ns_ = nsPerSample(cpu_meter::voices, last_voices_);
		drum_ns_ = nsPerSample(cpu_meter::drumSynth, last_drum_);
		takeSnapshot();
	}

	cpu_meter::Counter last_audio_;
	cpu_meter::Counter last_voices_;
	cpu_meter::Counter last_drum_;
	int32_t load_percent_{0};
	int32_t peak_percent_{0};
	int32_t voice_ns_{0};
	int32_t drum_ns_{0};

	Metric load_;
	Metric peak_;
	Metric voice_;
	Metric drum_;
};

} // namespace deluge::gui::menu_item::cpu
