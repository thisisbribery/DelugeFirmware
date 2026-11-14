#pragma once
#include "gui/menu_item/audio_clip/audio_source_selector.h"
#include "gui/menu_item/audio_clip/sample_marker_editor.h"
#include "gui/menu_item/edit_name.h"
#include "gui/menu_item/filter/param.h"
#include "gui/menu_item/firmware/version.h"
#include "gui/menu_item/note/fill.h"
#include "gui/menu_item/note/iterance_divisor.h"
#include "gui/menu_item/note/iterance_preset.h"
#include "gui/menu_item/note/iterance_step_toggle.h"
#include "gui/menu_item/note/probability.h"
#include "gui/menu_item/note/velocity.h"
#include "gui/menu_item/note_row/fill.h"
#include "gui/menu_item/note_row/iterance_divisor.h"
#include "gui/menu_item/note_row/iterance_preset.h"
#include "gui/menu_item/note_row/iterance_step_toggle.h"
#include "gui/menu_item/note_row/probability.h"
#include "gui/menu_item/osc/audio_recorder.h"
#include "gui/menu_item/osc/source/wave_index.h"
#include "gui/menu_item/osc/sync.h"
#include "gui/menu_item/patch_cables.h"
#include "gui/menu_item/patched_param/integer_non_fm.h"
#include "gui/menu_item/randomizer/midi_cv/note_probability.h"
#include "gui/menu_item/randomizer/midi_cv/spread_velocity.h"
#include "gui/menu_item/randomizer/randomizer_lock.h"
#include "gui/menu_item/sample/end.h"
#include "gui/menu_item/sample/start.h"
#include "gui/menu_item/sample/start_offset.h"
#include "gui/menu_item/sequence/direction.h"
#include "gui/menu_item/source/patched_param/modulator_level.h"
#include "gui/menu_item/stem_export/start.h"
#include "gui/menu_item/submenu.h"
#include "gui/menu_item/submenu/mod_fx.h"

namespace deluge::gui::menu_item {
class HorizontalMenu;
class HorizontalMenuGroup;
class HorizontalMenuContainer;
} // namespace deluge::gui::menu_item

extern gui::menu_item::patched_param::IntegerNonFM noiseMenu;
extern gui::menu_item::osc::Sync oscSyncMenu;
extern gui::menu_item::osc::source::WaveIndex source0WaveIndexMenu;
extern gui::menu_item::osc::source::WaveIndex source1WaveIndexMenu;

extern gui::menu_item::sample::Start sample0StartMenu;
extern gui::menu_item::sample::Start sample1StartMenu;
extern gui::menu_item::sample::End sample0EndMenu;
extern gui::menu_item::sample::End sample1EndMenu;
extern gui::menu_item::sample::StartOffset sample0StartOffsetMenu;
extern gui::menu_item::sample::StartOffset sample1StartOffsetMenu;
extern gui::menu_item::osc::AudioRecorder sample0RecorderMenu;
extern gui::menu_item::osc::AudioRecorder sample1RecorderMenu;
extern gui::menu_item::audio_clip::SampleMarkerEditor audioClipSampleMarkerEditorMenuStart;
extern gui::menu_item::audio_clip::SampleMarkerEditor audioClipSampleMarkerEditorMenuEnd;
extern gui::menu_item::EditName nameEditMenu;
extern gui::menu_item::EditName drumNameEditMenu;
extern gui::menu_item::Submenu editNameMenu;
extern gui::menu_item::Submenu dxMenu;
extern gui::menu_item::Submenu stemExportMenu;
extern gui::menu_item::stem_export::Start startStemExportMenu;

extern gui::menu_item::firmware::Version firmwareVersionMenu;
extern gui::menu_item::sequence::Direction sequenceDirectionMenu;
extern gui::menu_item::Submenu soundEditorRootMenuMIDIOrCV;
extern gui::menu_item::Submenu soundEditorRootMenuMidiDrum;
extern gui::menu_item::Submenu soundEditorRootMenuGateDrum;
extern gui::menu_item::Submenu soundEditorRootMenuAudioClip;
extern gui::menu_item::Submenu soundEditorRootMenuPerformanceView;
extern gui::menu_item::Submenu soundEditorRootMenuSongView;
extern gui::menu_item::Submenu soundEditorRootMenuKitGlobalFX;
extern gui::menu_item::Submenu soundEditorRootMenuDrum;
extern gui::menu_item::Submenu soundEditorRootMenu;
extern gui::menu_item::Submenu settingsRootMenu;
extern gui::menu_item::submenu::ModFxHorizontalMenu globalModFXMenu;
extern gui::menu_item::submenu::ModFxHorizontalMenu modFXMenu;

extern gui::menu_item::randomizer::RandomizerLock randomizerLockMenu;
extern gui::menu_item::randomizer::midi_cv::SpreadVelocity spreadVelocityMenuMIDIOrCV;
extern gui::menu_item::randomizer::midi_cv::NoteProbability randomizerNoteProbabilityMenuMIDIOrCV;

// note editor menu's
extern gui::menu_item::HorizontalMenu noteEditorRootMenu;
extern gui::menu_item::note::Velocity noteVelocityMenu;
extern gui::menu_item::note::Probability noteProbabilityMenu;
extern gui::menu_item::note::IterancePreset noteIteranceMenu;
extern gui::menu_item::note::IteranceDivisor noteCustomIteranceDivisor;
extern gui::menu_item::note::IteranceStepToggle noteCustomIteranceStep1;
extern gui::menu_item::note::IteranceStepToggle noteCustomIteranceStep2;
extern gui::menu_item::note::IteranceStepToggle noteCustomIteranceStep3;
extern gui::menu_item::note::IteranceStepToggle noteCustomIteranceStep4;
extern gui::menu_item::note::IteranceStepToggle noteCustomIteranceStep5;
extern gui::menu_item::note::IteranceStepToggle noteCustomIteranceStep6;
extern gui::menu_item::note::IteranceStepToggle noteCustomIteranceStep7;
extern gui::menu_item::note::IteranceStepToggle noteCustomIteranceStep8;
extern gui::menu_item::note::Fill noteFillMenu;
// note row editor menu's
extern gui::menu_item::HorizontalMenu noteRowEditorRootMenu;
extern gui::menu_item::note_row::Probability noteRowProbabilityMenu;
extern gui::menu_item::note_row::IterancePreset noteRowIteranceMenu;
extern gui::menu_item::note_row::IteranceDivisor noteRowCustomIteranceDivisor;
extern gui::menu_item::note_row::IteranceStepToggle noteRowCustomIteranceStep1;
extern gui::menu_item::note_row::IteranceStepToggle noteRowCustomIteranceStep2;
extern gui::menu_item::note_row::IteranceStepToggle noteRowCustomIteranceStep3;
extern gui::menu_item::note_row::IteranceStepToggle noteRowCustomIteranceStep4;
extern gui::menu_item::note_row::IteranceStepToggle noteRowCustomIteranceStep5;
extern gui::menu_item::note_row::IteranceStepToggle noteRowCustomIteranceStep6;
extern gui::menu_item::note_row::IteranceStepToggle noteRowCustomIteranceStep7;
extern gui::menu_item::note_row::IteranceStepToggle noteRowCustomIteranceStep8;
extern gui::menu_item::note_row::Fill noteRowFillMenu;

extern gui::menu_item::PatchCables patchCablesMenu;
extern gui::menu_item::source::patched_param::ModulatorLevel modulator0Volume;
extern gui::menu_item::source::patched_param::ModulatorLevel modulator1Volume;

extern MenuItem* midiOrCVParamShortcuts[kDisplayHeight];
extern MenuItem* gateDrumParamShortcuts[kDisplayHeight];
extern MenuItem* paramShortcutsForSounds[kDisplayWidth][kDisplayHeight];
/// In 303 mode, the menu item that takes over a shortcut pad, or nullptr
MenuItem* tb303ShortcutAt(int32_t x, int32_t y);
/// Whether a shortcut pad item is the vibrato pad (LFO 1 to pitch)
bool isVibratoShortcut(MenuItem const* item);
/// The shortcut pad of a 303 mode menu item, if it has one
bool tb303ShortcutFor(MenuItem* item, int32_t* x, int32_t* y);
extern MenuItem* paramShortcutsForSoundsSecondLayer[kDisplayWidth][kDisplayHeight];
/// Delay feedback / tone for a kit, audio clip or song delay amount / rate pad pressed twice, or nullptr
MenuItem* globalDelaySecondLayer(MenuItem const* item);
/// The delay amount / rate pad item that delay feedback / tone sit under, or nullptr
MenuItem* globalDelayFirstLayer(MenuItem const* item);
class Sound;
namespace deluge::dsp::drum_synth {
enum class Engine : uint8_t;
}
/// Whether a sound's osc 1 is a drum synth
bool isDrumSynth(const Sound* sound);
/// The menu item a drum synth engine's layout puts on a pad, or nullptr: on a drum engine the MOD1 and MOD2 columns
/// (and the OSC1 extras), on a synth engine (RINGS, BROPHET) the OSC1, OSC2 and FM MOD columns
MenuItem* drumLayoutItemAt(deluge::dsp::drum_synth::Engine engine, int32_t x, int32_t y);
/// Whether a column is part of an engine's layout (kept lit for Pad Guides, its pads without a setting greyed)
bool drumLayoutColumn(deluge::dsp::drum_synth::Engine engine, int32_t x);
/// Whether an engine lays out as a synth (its basics on the OSC pads) rather than a drum
bool isSynthLayout(deluge::dsp::drum_synth::Engine engine);
/// Whether a pad is one of a drum synth sound's layout pads (on a drum engine every MOD1 and MOD2 pad)
bool isDrumLayoutPad(const Sound* sound, int32_t x, int32_t y);
/// A sound's shortcut pad item. On a drum synth sound, the engine's layout pads, and on a drum engine OSC1 TYPE opens
/// SYNTH MODE (see menus.cpp). nullptr = nothing.
MenuItem* soundShortcutFor(const Sound* sound, int32_t x, int32_t y);
/// The patched param on a drum synth sound's shortcut pad, for the automation view and patch cable blinks: the
/// control on a drum layout pad (kNoParamID for a choice or the tuning), elsewhere the Deluge's own param, or
/// kNoParamID where that is one of the drum controls.
uint32_t drumShortcutParam(const Sound* sound, int32_t x, int32_t y);
extern MenuItem* paramShortcutsForAudioClips[kDisplayWidth][kDisplayHeight];
extern MenuItem* paramShortcutsForSongView[kDisplayWidth][kDisplayHeight];
extern MenuItem* paramShortcutsForKitGlobalFX[kDisplayWidth][kDisplayHeight];

extern deluge::vector<gui::menu_item::HorizontalMenu*> horizontalMenusChainForSound;
extern deluge::vector<gui::menu_item::HorizontalMenu*> horizontalMenusChainForKit;
extern deluge::vector<gui::menu_item::HorizontalMenu*> horizontalMenusChainForSong;
extern deluge::vector<gui::menu_item::HorizontalMenu*> horizontalMenusChainForAudioClip;
extern deluge::vector<gui::menu_item::HorizontalMenu*> horizontalMenusChainForMidiOrCv;
extern deluge::vector<gui::menu_item::HorizontalMenuContainer*> horizontalMenuContainers;

extern gui::menu_item::HorizontalMenuGroup sourceMenuGroup;
extern gui::menu_item::HorizontalMenu audioClipSampleMenu;
