// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include "core/vr/summon_bell.h"
#include "core/vr/lever_settings.h"
#include "core/vr/lamp_channel.h"
#include "core/vr/lamp_page.h"
#include "core/vr/soft_save_channel.h"
#include "core/vr/soft_save_page.h"
#include "core/vr/lamp_presentation.h"
#include "core/vr/gun_settings.h"
#include "core/vr/color_settings.h"
#include "core/vr/dlss_model.h"
#include "core/vr/graphics_status.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <condition_variable>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>

#include "common/logging/log.h"
#include "common/memory_patcher.h"
#include "common/stb.h"
#include "common/thread.h"
#include "core/vr/bloodborne_assets.h"
#include "core/vr/body_settings.h"
#include "core/vr/quick_settings.h"
#include "core/vr/melee_settings.h"
#include "core/vr/blade_sounds.h"
#include "core/vr/trick_weapons.h"
#include "core/vr/two_hand_settings.h"
#include "core/vr/controls_render.h"
#include "core/vr/controls_session.h"
#include "core/vr/vr_page.h"
#include "core/vr/vr_page_runtime.h"
#include "core/vr/vr_session.h"

namespace VR::Page {

namespace {

std::string Bool(bool b) {
    return b ? "1" : "0";
}

const char* LampShadowTierName(u8 tier) {
    return tier == 0 ? "lite" : tier == 2 ? "high" : "medium";
}

// An upscaler's files beside the runtime (its folder there, or the VR_*_DIR override): a release
// without FSR 4 or DLSS 5 greys their rows instead of offering a choice that cannot start.
bool ProviderInstalled(const char* env, const char* folder, const char* file);

// The accepted upscaler's files are installed (Off: true). Checked once each.
bool UpscalerFilesPresent(const Graphics::Config& c) {
    static const bool dlss = ProviderInstalled("VR_DLSS_DIR", "dlss", "vr_dlss_stereo.dll") &&
                             ProviderInstalled("VR_DLSS_DIR", "dlss", "nvngx_dlss.dll");
    // DLSS 5: the bridge's forwarder ships; the player's NVIDIA file is checked at start (not -> 0).
    static const bool neural = ProviderInstalled("VR_DLSS_DIR", "dlss", "nvngx.dll_dlssnr.dll");
    static const bool fsr4 = ProviderInstalled("VR_FSR4_DIR", "fsr4", "vr_fsr4_stereo.dll");
    static const bool fsr31 = ProviderInstalled("VR_FSR31_DIR", "fsr31", "vr_fsr31_stereo.dll");
    static const bool fsr41 = ProviderInstalled("VR_FSR41_DIR", "fsr41", "vr_fsr41_stereo.dll");
    switch (c.upscaler) {
    case Graphics::Upscaler::Dlss:
        return dlss && (!c.dlss5 || (neural && Graphics::neural_file.load() != 0));
    case Graphics::Upscaler::Fsr4:
        return fsr4;
    case Graphics::Upscaler::Fsr31:
        return fsr31;
    case Graphics::Upscaler::Fsr41:
        return fsr41;
    default:
        return true;
    }
}

bool ProviderInstalled(const char* env, const char* folder, const char* file) {
    std::filesystem::path dir;
    if (const char* p = std::getenv(env); p && *p) {
        dir = p;
    } else if (const char* base = SDL_GetBasePath(); base) {
        dir = std::filesystem::path(base) / folder;
    } else {
        return false;
    }
    std::error_code error;
    return std::filesystem::is_regular_file(dir / file, error);
}

// The value of `key` in vr.ini's terms, from a settings struct (the live one, or a default one
// for the code default).
std::string ValueIn(const Settings &s, const Body::BodySettings &b, const Quick::QuickSettings &q,
                    const Melee::MeleeSettings &m, const Tricks::Settings &t,
                    const std::string &key) {
    if (key == "play_style")
        return PlayStyle::Name(static_cast<PlayStyle::Style>(s.play_style));
    if (key == "left_handed")
        return Bool(s.left_handed);
    if (key == "tp_distance_pct")
        return std::to_string(std::lround(s.tp_distance_scale * 100.0f));
    if (key == "tp_view_follows_pitch")
        return FormatIni(s.tp_view_follows_pitch);
    if (key == "gun_wrist_reload")
        return Bool(Gun::GunSettings{}.wrist_reload);
    if (key == "color_contrast" || key == "color_saturation" || key == "color_gamma")
        return "0";
    if (key == "two_hand_guide")
        return Bool(TwoHand::Settings{}.guide);
    if (key == "two_hand_optimized_controls")
        return Bool(TwoHand::Settings{}.optimized_controls);
    if (key == "two_hand_trigger_hold")
        return Bool(TwoHand::Settings{}.trigger_hold);
    if (key == "two_hand_hold_b_reposition")
        return Bool(TwoHand::Settings{}.hold_b_reposition);
    if (key == "two_hand_swing_hum")
        return FormatIni(TwoHand::Settings{}.swing_hum);
    if (key == "lamp_travel")
        return "1";
    if (key == "physical_summon_bell")
        return "1";
    if (key == "physical_levers")
        return "1"; // code default; live Get handles the atomic setting separately
    if (key == "weighted_weapons")
        return s.weighted_weapons == 2 ? "2" : Bool(s.WeaponWeightStrength() > 0.0f);
    if (key == "weapon_wall")
        return !t.weapon_wall ? "0" : t.weapon_wall_map ? "1" : "2";
    if (key == "blade_sounds")
        return Bool(BladeSound::Settings{}.on);
    if (key == "melee_stamina_pct")
        return std::to_string(m.stamina_pct);
    if (key == "shoulder_switch")
        return Bool(s.shoulder_switch);
    if (key == "health_color")
        return Bool(s.health_color);
    if (key == "riposte_style")
        return Bool(s.riposte_style);
    if (key == "stamina_color")
        return Bool(s.stamina_color);
    if (key == "health_color_strength")
        return FormatIni(s.health_color_strength);
    if (key == "stamina_color_strength")
        return FormatIni(s.stamina_color_strength);
    if (key == "holster_mode")
        return FormatIni(q.holster_mode);
    if (key == "holster_haptics")
        return Bool(q.holster_haptics);
    if (key == "shoulder_vial")
        return Bool(q.shoulder_vial);
    if (key == "quick_arc_pick")
        return q.pick_fan ? "fan" : q.pick_sector ? "sector" : "hover";
    if (key == "quick_fan_style")
        return q.fan_style == 1 ? "brass" : "lancet";
    if (key == "body_comfort")
        return Bool(b.body_comfort);
    if (key == "body_align")
        return Bool(b.body_align);
    if (key == "body_posture")
        return b.body_posture_lean ? "lean" : "shift";
    if (key == "body_lean_side_deg")
        return FormatIni(b.body_lean_side_deg);
    if (key == "body_align_ms")
        return FormatIni(b.body_align_ms);
    if (key == "height_base") {
        return s.height_player ? "player" : "hunter";
    } else if (key == "height_adjust_cm") {
        return std::to_string(s.height_adjust_cm);
    } else if (key == "seated") {
        return Bool(s.seated);
    } else if (key == "swap_sticks") {
        return Bool(s.swap_sticks);
    } else if (key == "left_scheme") {
        return std::to_string(s.left_scheme); // read only: the rows' control names
    } else if (key == "snap_turn_deg") {
        return FormatIni(s.snap_turn_deg);
    } else if (key == "smooth_turn_deg_per_s") {
        return FormatIni(s.smooth_turn_deg_per_s);
    } else if (key == "lockon_follow") {
        return std::to_string(s.lockon_follow);
    } else if (key == "haptic_gain") {
        return FormatIni(s.haptic_gain);
    } else if (key == "controls") {
        return s.controls_classic ? "classic" : "motion";
    } else if (key == "inject") {
        return Bool(s.inject);
    } else if (key == "x_button") {
        return s.x_fan ? "fan" : "vial";
    } else if (key == "laser") {
        return Bool(s.laser);
    } else if (key == "grip_adjust") {
        return Bool(b.grip_adjust);
    } else if (key == "vr_grab_attacks") {
        return Bool(s.vr_grab_attacks);
    } else if (key == "vr_interactions") {
        return Bool(s.vr_interactions);
    } else if (key == "upscale_focus") {
        return std::to_string(s.upscale_focus);
    } else if (key == "cinematic") {
        return Bool(s.cinematic);
    } else if (key == "cinematic_ladder_skip") {
        return Bool(s.cinematic_ladder_skip); // (missing: the row read "Custom", the user 11-06)
    } else if (key == "doll_respec") {
        return Bool(s.doll_respec);
    } else if (key == "late_warp_motion") {
        return Bool(s.late_warp_motion);
    } else if (key == "body_cutaway") {
        return Bool(b.body_cutaway);
    } else if (key == "body_mode") {
        return b.mode == Body::BodyMode::ArmsOnly    ? "arms_only"
               : b.mode == Body::BodyMode::HandsOnly ? "hands_only"
               : b.mode == Body::BodyMode::Full      ? "full"
                                                  : "no_head";
    } else if (key == "ipd_mm") {
        return FormatIni(std::round(s.ipd_mm));
    } else if (key == "hud_vitals") {
        return s.hud_vitals == 1 ? "wrist" : "classic";
    } else if (key == "vitals_heartbeat") {
        return Bool(s.vitals_heartbeat);
    } else if (key == "vitals_edge") {
        return Bool(s.vitals_edge);
    } else if (key == "ui_counters") {
        return s.ui_counters == 1 ? "always" : s.ui_counters == 2 ? "hidden" : "look_down";
    } else if (key == "hit_vignette") {
        return Bool(s.hit_vignette);
    } else if (key == "ui_hud_scale_deg") {
        return FormatIni(s.ui_hud_scale_deg);
    } else if (key == "outgoing_damage_pct") {
        return std::to_string(s.outgoing_damage_pct);
    } else if (key == "player_hp_bonus_pct") {
        return std::to_string(s.player_hp_bonus_pct);
    } else if (key == "empty_vial_refill") {
        return std::to_string(s.empty_vial_refill);
    } else if (key == "dodge_iframes_extra") {
        return std::to_string(s.dodge_iframes_extra);
    } else if (key == "melee_kill_ragdoll") {
        return Bool(s.melee_kill_ragdoll);
    } else if (key == "melee_ragdoll_impulse") {
        return FormatIni(s.melee_ragdoll_impulse);
    } else if (key == "foveation") {
        return std::to_string(Graphics::foveation.load(std::memory_order_relaxed));
    } else if (key == "foveation_feedback") {
        return Graphics::FoveationFeedback(Graphics::foveation.load(std::memory_order_relaxed),
                                           Graphics::foveation_applied.load(std::memory_order_relaxed));
    } else if (key == "dlss_model") {
        // The built-in model is M in this runtime (dlss_model.h): the page shows it as Best.
        const auto model = Graphics::dlss_model.load(std::memory_order_relaxed);
        return model == Graphics::DlssModelBuiltIn ? std::string("M") : Graphics::DlssModelName(model);
    } else if (key == "aa_mode") {
        return Aa::Name(GetRequestedAaMode());
    } else if (key == "lod_bias") {
        return std::to_string(s.lod_bias);
    } else if (key == "lod_bias_live") {
        return MemoryPatcher::LodLiveAvailable() ? "1" : "0";
    } else if (key == "graphics_fsr4_available") {
        static const bool installed = ProviderInstalled("VR_FSR4_DIR", "fsr4", "vr_fsr4_stereo.dll");
        return installed ? "1" : "0";
    } else if (key == "graphics_fsr31_available") {
        static const bool installed = ProviderInstalled("VR_FSR31_DIR", "fsr31", "vr_fsr31_stereo.dll");
        return installed ? "1" : "0";
    } else if (key == "graphics_fsr41_available") {
        // AMD cards only (VR_FSR41_ANY_GPU=1: tests on others, through AMD's FSR 3.1.6).
        static const bool installed = ProviderInstalled("VR_FSR41_DIR", "fsr41", "vr_fsr41_stereo.dll");
        static const bool any_gpu = std::getenv("VR_FSR41_ANY_GPU") != nullptr;
        return installed && (any_gpu || Graphics::amd_gpu.load(std::memory_order_relaxed) == 1)
                   ? "1"
                   : "0";
    } else if (key == "graphics_neural_available") {
        // DLSS 5 needs the bridge's forwarder, an RTX 50 card and the player's own NVIDIA-signed
        // nvngx_dlssnr.dll (vr_dlss_runtime.cpp VerifiedNeuralFile; the launcher adds it).
        static const bool installed = ProviderInstalled("VR_DLSS_DIR", "dlss", "nvngx.dll_dlssnr.dll");
        return installed && Graphics::neural_gpu.load(std::memory_order_relaxed) == 1 &&
                       Graphics::neural_file.load(std::memory_order_relaxed) == 1
                   ? "1"
                   : "0";
    } else if (key == "graphics_neural_note") {
        // The DLSS 5 tab's status line: why it is greyed, else that it is an experiment.
        if (Graphics::neural_gpu.load(std::memory_order_relaxed) == 0)
            return "Needs an RTX 50 GPU";
        if (Graphics::neural_file.load(std::memory_order_relaxed) == 0)
            return "Add file in launcher";
        return "Experimental";
    } else if (key == "ui_enemy_hp_bars") {
        return Bool(s.ui_enemy_hp_bars);
    } else if (key == "ui_boss_bar") {
        return Bool(s.ui_boss_hp_bar);
    } else if (key == "shadows_far") {
        return s.shadows_far_reduced ? "reduced" : "full";
    } else if (key == "water_reflections") {
        return Bool(s.water_reflections);
    } else if (key == "frame_overlap") {
        return Bool(s.frame_overlap);
    } else if (key == "canted_eyes") {
        return Bool(s.canted_eyes);
    } else if (key == "shadows_busy") {
        return Bool(s.shadows_busy);
    } else if (key == "light_shadows") {
        return Bool(s.light_shadows);
    } else if (key == "light_shadow_detail") {
        return LampShadowTierName(s.light_shadow_tier);
    } else if (key == "moon_shadows") {
        return Bool(s.moon_shadows);
    } else if (key == "ambient_occlusion") {
        return Bool(s.ambient_occlusion);
    } else if (key == "bounce_light") {
        return Bool(s.bounce_light);
    } else if (key == "fog_light") {
        return Bool(s.fog_light);
    } else if (key == "moon_shafts") {
        return Bool(s.moon_shafts); // The page shows the Fog override separately.
    } else if (key == "fog_banks") {
        return Bool(s.fog_banks);
    } else if (key == "fog_banks_loaded") {
        // (The area was loaded with the fog banks: vr_session.h FogBanksLoaded. Not a setting.)
        return Bool(FogBanksLoaded().load(std::memory_order_relaxed));
    } else if (key == "ground_mist") {
        return Bool(s.ground_mist);
    } else if (key == "cinematic_look") {
        return std::to_string(s.cinematic_look);
    } else if (key == "filmic_highlights") {
        return Bool(s.filmic_highlights);
    } else if (key == "eye_adaptation") {
        return Bool(s.eye_adaptation);
    } else if (key == "wet_streets") {
        return Bool(s.wet_streets);
    } else if (key == "raised_bricks") {
        return Bool(s.raised_bricks);
    }
    return {};
}

// The emulator's store: the live settings, changed through the vr.ini parser (VR::ApplySetting)
// and saved by the safe writer (VR::SaveSetting / RemoveSetting: only the keys changed here).
class SettingsStore final : public Store {
public:
    std::vector<ControlsView::DrawnPart> controller_parts;

    std::string ControlName(Thumbs::Control physical) override {
        return ControlsView::NameOf(controller_parts, physical);
    }
    std::string Get(const std::string& key) override {
        // Left-handed play: this session's, fixed at its start (VR::MirrorWorld, which also needs
        // motion controls, first person and stereo eyes): the page's names and words follow it
        // (vr_page.h PageMirror).
        if (key == "left_handed")
            return Bool(VR::MirrorWorld());
        if (key == "upscale_focus_live_path") {
            const auto c = Graphics::Accepted();
            return Bool(std::getenv("VR_NATIVE_STEREO") && Graphics::S1Output().width &&
                        ((c.upscaler == Graphics::Upscaler::Dlss && !c.dlss5) || Graphics::IsFsr(c.upscaler)));
        }
        if (key == "gun_wrist_reload")
            return Bool(Gun::GetGunSettings().wrist_reload);
        const auto color = Color::Get();
        if (key == "color_contrast")
            return std::to_string(color.contrast);
        if (key == "color_saturation")
            return std::to_string(color.saturation);
        if (key == "color_gamma")
            return std::to_string(color.gamma);
        if (key == "blade_sounds")
            return Bool(BladeSound::GetSettings().on);
        if (key == "two_hand_guide")
            return Bool(TwoHand::GetTwoHandSettings().guide);
        if (key == "two_hand_optimized_controls")
            return Bool(TwoHand::GetTwoHandSettings().optimized_controls);
        if (key == "two_hand_trigger_hold")
            return Bool(TwoHand::GetTwoHandSettings().trigger_hold);
        if (key == "two_hand_hold_b_reposition")
            return Bool(TwoHand::GetTwoHandSettings().hold_b_reposition);
        if (key == "two_hand_swing_hum")
            return FormatIni(TwoHand::GetTwoHandSettings().swing_hum);
        if (key == "lamp_travel")
            return Bool(Lamps::Enabled());
        if (key == "physical_summon_bell")
            return Bool(SummonBell::enabled.load());
        if (key == "physical_levers")
            return Bool(Lever::Enabled());
        if (key == "empty_vial_refill")
            return std::to_string(EmptyVialRefillCount());
        if (key == "light_shadows")
            return Bool(GetLampShadowRequest().enabled);
        if (key == "light_shadow_detail")
            return LampShadowTierName(GetLampShadowRequest().tier);
        if (key == "graphics_config")
            return Graphics::Serialize(Graphics::Saved());
        if (key == "graphics_native_stereo")
            return std::getenv("VR_NATIVE_STEREO") ? "1" : "0";
        // DIBR reconstructs its centre with DLSS / FSR unless VR_DIBR_UPSCALE=0 (vr_dibr_upscale.h).
        if (key == "graphics_dibr_upscale")
            return !std::getenv("VR_NATIVE_STEREO") && Graphics::DibrUpscaleAllowed() ? "1" : "0";
        if (key == "graphics_status" && !std::getenv("VR_NATIVE_STEREO")) {
            if (!Graphics::DibrUpscaleAllowed())
                return "DIBR | Scale applies after restart. DLSS/FSR unavailable; saved settings kept.";
            const auto accepted = Graphics::Accepted();
            if (accepted.upscaler == Graphics::Upscaler::Dlss && accepted.dlss5)
                return "DIBR | DLSS 5 needs native stereo. Pick DLSS or FSR for DIBR.";
            return "DIBR | " + Graphics::RuntimeStatusText(UpscalerFilesPresent(accepted));
        }
        if (key == "graphics_status")
            return Graphics::RuntimeStatusText(UpscalerFilesPresent(Graphics::Accepted()));
        if (key == "graphics_resolution")
            return Graphics::ResolutionText();
        return ValueIn(GetSettings(), Body::GetBodySettings(), Quick::GetQuickSettings(),
                       Melee::GetMeleeSettings(), Tricks::GetTrickSettings(), key);
    }
    void Set(const std::string& key, const std::string& value) override {
        if (ApplySetting(key, value)) {
            SaveSetting(key, value);
        }
    }
    void SetChoice(const Row& row, const Option& option) override {
        if (std::string_view(row.id) == "new_shadows") {
            ApplyLampShadowChoice(option.values[0], option.values[1]);
            SaveSetting(row.keys[0], option.values[0]);
            SaveSetting(row.keys[1], option.values[1]);
        } else {
            Store::SetChoice(row, option);
        }
    }
    void ResetRow(const Row& row) override {
        if (std::string_view(row.id) != "new_shadows") {
            Store::ResetRow(row);
            return;
        }
        const auto& defaults = DefaultSettings();
        ApplyLampShadowChoice(defaults.light_shadows ? "1" : "0",
                              LampShadowTierName(defaults.light_shadow_tier));
        RemoveSetting(row.keys[0]);
        RemoveSetting(row.keys[1]);
    }
    void Remove(const std::string& key) override {
        if (key == "weighted_weapons") {
            Set(key, "1"); // the default: on
            return;
        }
        if (key == "foveation") {
            // Live (vr_foveation.h): Auto again at once, the key gone from vr.ini (ValueIn reads
            // the level in force, not its default).
            ApplySetting(key, "-1");
            RemoveSetting(key);
            return;
        }
        static const Body::BodySettings body_defaults{};
        const std::string value = ValueIn(DefaultSettings(), body_defaults, Quick::QuickSettings{},
                                          Melee::MeleeSettings{}, Tricks::Settings{}, key);
        if (!value.empty()) {
            ApplySetting(key, value);
        }
        // Reset must not revive a legacy melee_stamina=1 override on the next launch.
        if (key == "melee_stamina_pct")
            RemoveSetting("melee_stamina");
        RemoveSetting(key);
    }

private:
    static const Settings& DefaultSettings() {
        static const Settings defaults = [] {
            Settings settings{};
            ApplyCardDefaults(settings);
            return settings;
        }();
        return defaults;
    }
};

constexpr float FadeInS = 0.2f;  // the page movie's FadeIn: 6 frames at 30 fps
constexpr float FadeOutS = 0.2f; // FadeOut: 6 frames
// The game slides its pages up 10 stage px while they fade in. In VR the page appears in place
// (a panel moving at 1.8 m while it fades in read as a wobble on open; the play test of
// 2026-10-09): only the fade.
constexpr float SlidePx = 0.0f;
constexpr std::array<const char*, 4> TextureNames{"MENU_Common_00003", "MENU_Common_00001",
                                                  "KG_L_LR", "KG_L_UD"};

// VR_PAGE_TEST_DUMP (tests): the page's values read through, its edits kept here (nothing reaches
// vr.ini or the game; a removed key reads the live value again).
class DumpStore final : public Store {
public:
    explicit DumpStore(Store& source_) : source{source_} {}
    std::string ControlName(Thumbs::Control physical) override {
        return source.ControlName(physical);
    }
    std::string Get(const std::string& key) override {
        const auto it = edits.find(key);
        return it != edits.end() ? it->second : source.Get(key);
    }
    void Set(const std::string& key, const std::string& value) override {
        edits[key] = value;
    }
    void Remove(const std::string& key) override {
        edits.erase(key);
    }

private:
    Store& source;
    std::map<std::string, std::string> edits;
};

// One image of the dump: a page's view, or Show VR controls'.
struct DumpPage {
    std::string name;
    std::optional<View> page;
    std::optional<ControlsView::View> controls;
};

// A tab's name as a file name ("HUD & feedback" -> "hud_feedback").
std::string FileName(std::string_view name) {
    std::string out;
    for (const char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c))) {
            out += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        } else if (!out.empty() && out.back() != '_') {
            out += '_';
        }
    }
    while (!out.empty() && out.back() == '_') {
        out.pop_back();
    }
    return out;
}

// The dump's images drawn with the game's art and font, each over a dark world (the page's
// premultiplied colour over it, its glow added) into <dir>/<name>.png.
void DrawDump(const Assets& assets, const std::filesystem::path& dir,
              const std::vector<DumpPage>& pages) {
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    size_t written = 0;
    for (const DumpPage& d : pages) {
        Image page, glow;
        const bool ok = d.controls ? ControlsView::Render(assets, *d.controls, page, glow)
                                   : d.page && Render(assets, *d.page, page, glow);
        const size_t n = static_cast<size_t>(std::max(page.width, 0)) *
                         static_cast<size_t>(std::max(page.height, 0)) * 4;
        if (!ok || n == 0 || page.rgba.size() != n || glow.rgba.size() != n) {
            LOG_WARNING(Core, "VR page dump: {} not drawn", d.name);
            continue;
        }
        constexpr float World = 24.0f; // the dark world behind the page
        std::vector<u8> out(n);
        for (size_t i = 0; i < n; i += 4) {
            const float k = 1.0f - static_cast<float>(page.rgba[i + 3]) / 255.0f;
            for (size_t c = 0; c < 3; ++c) {
                const float x = static_cast<float>(page.rgba[i + c]) + World * k +
                                static_cast<float>(glow.rgba[i + c]);
                out[i + c] = static_cast<u8>(std::clamp(x, 0.0f, 255.0f) + 0.5f);
            }
            out[i + 3] = 255;
        }
        const std::string path = (dir / (d.name + ".png")).string();
        if (stbi_write_png(path.c_str(), page.width, page.height, 4, out.data(),
                           page.width * 4) != 0) {
            ++written;
        } else {
            LOG_WARNING(Core, "VR page dump: {} not written", path);
        }
    }
    LOG_INFO(Core, "VR page dump: {} of {} images written to {}", written, pages.size(),
             dir.string());
}

} // Anonymous namespace

struct Runtime::Impl {
    std::optional<Lamps::Model> lamp;
    u64 death{}, death_revision{}, death_presented{};
    s64 death_offer_ns{}; // the death choice's offer (steady clock): back escapes a stalled page
    bool live_presentation{}; // latched for the page session, including its closing fade
    int death_focus{};
    Hover death_hover{};
    Hover lamp_hover{};
    Lamps::Presentation lamp_presentation;
    SettingsStore store;
    Model model{store, [](const std::string& line) { LOG_INFO(Core, "{}", line); }};
    std::atomic<u32> requests{0};
    // When the last request came (steady clock ns; 0 = never): any thread reads it
    // (SecondsSinceRequest: the UI layer holds the game's menu-less frames after the A).
    std::atomic<s64> request_ns{0};
    u32 handled{0};
    std::atomic_bool open{false}; // set on the frame thread; the game's pad reads check it
    bool closing{false};
    float opacity{0.0f};
    float slide{0.0f};
    float pulse_t{0.0f};
    Glyphs glyphs{};
    View view{};
    std::vector<HitBox> boxes;
    bool trigger_down{false};

    // Show VR controls (core/vr/controls_view.h), over the page while it is up.
    ControlsView::Model controls;
    bool controls_open{false};
    ControlsView::View controls_view{};
    std::vector<ControlsView::HitBox> controls_boxes;
    int controls_hover_tab{-1};
    bool controls_b{false}; // B held (the physical B returns to the page)
    std::string profile;    // the interaction profile bound (SetController)
    bool dpad{false};
    ControlsView::Family family{ControlsView::Family::Touch}; // its family on this headset

    // The render worker.
    std::mutex mutex;
    std::condition_variable cv;
    std::optional<View> wanted;
    std::optional<ControlsView::View> wanted_controls; // drawn instead of the page when set
    std::shared_ptr<const Image> page_image;
    std::shared_ptr<const Image> glow_image;
    u64 serial{0};
    u64 wanted_lamp_revision{}, image_lamp_revision{};
    u64 opened_serial{0}; // the image serial when the page opened: it fades in on a newer one
    u32 changes_at_open{0}; // stats.changes when the page opened (the close logs this session's)
    float refresh_s{0.0f};  // since the last check for values changed outside the page
    std::thread worker;
    bool stop{false};
    Stats stats{};
    // VR_PAGE_TEST_DUMP: the images the worker draws once (with `mutex`); posted at the first open.
    std::vector<DumpPage> dump;
    std::filesystem::path dump_dir;
    bool dump_posted{false}; // the owner's thread

    Impl() = default; // the worker starts with the first page drawn (vr_menu=0: never)
    ~Impl() {
        {
            std::scoped_lock lk{mutex};
            stop = true;
        }
        cv.notify_all();
        if (worker.joinable()) {
            worker.join();
        }
    }

    View LampView() { return Lamps::MakePage(*lamp, glyphs, lamp_hover); }

    // VR_PAGE_TEST_DUMP=<dir> (tests, 2026-10-30): every tab's pages, a few states (the sub-tab
    // strip selected, the Defaults and restart questions, ACCEPT selected), Show VR controls' tabs
    // and the lamp and death pages, drawn once by the worker into <dir>; and every row selected and
    // stepped through all its values, its texts measured against their room (TextFitReport), the
    // ones wider at full size logged. On a store of its own: nothing reaches vr.ini or the game.
    void PostDump(const std::filesystem::path& dir) {
        dump_posted = true;
        DumpStore scratch{store};
        Model m{scratch};
        m.Reset();
        const GameText::GameFont* font = VR::Assets::GetPageFont();
        std::vector<DumpPage> pages;
        std::set<std::string> lines;
        size_t measured = 0;
        const auto measure = [&](const View& v, const std::string& where) {
            ++measured;
            for (const auto& l : TextFitReport(v, font)) {
                lines.insert(where + ": " + l);
            }
        };
        const auto add = [&](const std::string& name, View v) {
            measure(v, name);
            pages.push_back({name, std::move(v), std::nullopt});
        };
        const auto tab_index = [](std::string_view name) {
            for (size_t t = 0; t < Tabs().size(); ++t) {
                if (name == Tabs()[t].name) {
                    return static_cast<int>(t);
                }
            }
            return 0;
        };
        for (int t = 0; t < static_cast<int>(Tabs().size()); ++t) {
            const Tab& tab = Tabs()[static_cast<size_t>(t)];
            for (int p = 0; p < tab.Pages(); ++p) {
                m.SetTab(t);
                m.PageTo(p);
                const std::string name =
                    fmt::format("{:02}_{}_{}", t + 1, FileName(tab.name), p + 1);
                add(name, MakeView(m, glyphs));
                // Each row selected (its help) and stepped through every value both ways (a
                // Number's whole range, at most 60 steps): measured, not drawn.
                for (int s = 0; s < Slots; ++s) {
                    const Row* row = m.RowAt(s);
                    if (row == nullptr || !m.Focusable(s)) {
                        continue;
                    }
                    m.SetHover({Target::Row, s});
                    measure(MakeView(m, glyphs), name);
                    int steps = 0;
                    if (row->widget == Widget::Choice) {
                        steps = static_cast<int>(row->options.size());
                    } else if (row->widget == Widget::Number) {
                        steps = std::min(60, (row->max - row->min) / std::max(row->step, 1) + 1);
                    }
                    for (int k = 0; k < steps; ++k) {
                        m.Value(-1);
                    }
                    for (int k = 0; k < 2 * steps; ++k) {
                        m.Value(1);
                        measure(MakeView(m, glyphs), name);
                    }
                    m.SetHover({});
                }
            }
        }
        // The states.
        m.SetTab(tab_index("You")); // its sub-tab strip selected (a tab with pages)
        add("state_strip_selected", MakeView(m, glyphs));
        m.SetTab(tab_index("Combat"));
        m.SetHover({Target::ButtonRight, -1});
        m.Confirm();
        add("state_defaults_question", MakeView(m, glyphs));
        m.Back();
        m.SetTab(tab_index("Upscaling"));
        m.SetHover({Target::ButtonRight, -1});
        add("state_accept_selected", MakeView(m, glyphs));
        m.SetHover({Target::ButtonLeft, -1});
        m.Confirm();
        add("state_restart_question", MakeView(m, glyphs));
        m.Back();
        ControlsView::Model controls_model;
        for (int t = 0; t < ControlsView::TabCount; ++t) {
            controls_model.SetTab(t);
            pages.push_back({fmt::format("controls_{}", t + 1), std::nullopt,
                             ControlsView::MakeView(controls_model, glyphs)});
        }
        // The gamepad's view (a gamepad play style): an Xbox and a PlayStation pad, first and
        // third person, every tab; then the Xbox one with A and RT held (its lights).
        for (const bool xbox : {true, false}) {
            for (const bool third : {false, true}) {
                ControlsView::Config pc = ControlsView::CurrentConfig();
                pc.gamepad = true;
                pc.gamepad_xbox = xbox;
                pc.third_person = third;
                ControlsView::Model pad_model;
                pad_model.Open(pc, family, profile, dpad, ControlsView::Live{});
                for (int t = 0; t < ControlsView::TabCount; ++t) {
                    pad_model.SetTab(t);
                    pages.push_back({fmt::format("controls_pad_{}_{}_{}", xbox ? "xbox" : "ps",
                                                 third ? "third" : "first", t + 1),
                                     std::nullopt, ControlsView::MakeView(pad_model, glyphs)});
                }
                if (xbox && !third) {
                    pad_model.SetTab(0);
                    ControlsView::Live held;
                    held.pad = PagePad::Bit::Cross | PagePad::Bit::R2;
                    pad_model.Update(held);
                    pages.push_back({"controls_pad_xbox_first_lit", std::nullopt,
                                     ControlsView::MakeView(pad_model, glyphs)});
                }
            }
        }
        add("lamp", Lamps::MakePage(Lamps::Model{}, glyphs));
        add("death", SoftSave::DeathPage(0, glyphs));
        add("death_soft_save", SoftSave::DeathPage(1, glyphs, {}, "Sep 30 2025, 12:59 PM"));
        if (font == nullptr) {
            LOG_WARNING(Core, "VR page dump: no font yet: texts not measured");
        }
        LOG_INFO(Core, "VR page dump: {} views measured, {} texts wider than their room", measured,
                 lines.size());
        for (const auto& l : lines) {
            LOG_INFO(Core, "VR page dump: {}", l);
        }
        {
            std::scoped_lock lk{mutex};
            dump_dir = dir;
            dump = std::move(pages);
        }
        cv.notify_all();
    }

    void Redraw() {
        if (!worker.joinable()) {
            worker = std::thread([this] { Run(); });
        }
        if (controls_open) {
            controls_view = ControlsView::MakeView(controls, glyphs);
            controls_view.hover_tab = controls_hover_tab;
            controls_boxes = ControlsView::HitBoxes(controls_view, VR::Assets::GetPageFont());
            {
                std::scoped_lock lk{mutex};
                wanted_controls = controls_view;
                wanted_lamp_revision = 0;
                wanted.reset(); // never a stale page drawn after it
            }
            cv.notify_all();
            return;
        }
        view = death ? SoftSave::DeathPage(death_focus, glyphs, death_hover, SoftSave::Mailbox().When()) : lamp ? LampView() : MakeView(model, glyphs);
        boxes = HitBoxes(view, VR::Assets::GetPageFont());
        const u64 lamp_revision = death ? (++death_revision | (u64{1} << 63)) : lamp ? lamp_presentation.Update(*lamp) : 0;
        {
            std::scoped_lock lk{mutex};
            wanted = view;
            wanted_lamp_revision = lamp_revision;
            wanted_controls.reset();
        }
        cv.notify_all();
    }

    void Run() {
        Common::SetCurrentThreadName("VR:Page");
        bool waiting_logged = false;
        for (;;) {
            View v;
            u64 lamp_revision{};
            std::optional<ControlsView::View> cview; // Show VR controls instead of the page
            bool have_view = false;                  // else woken by the dump alone
            {
                std::unique_lock lk{mutex};
                cv.wait(lk, [this] {
                    return stop || wanted.has_value() || wanted_controls.has_value() ||
                           !dump.empty();
                });
                if (stop) {
                    return;
                }
                lamp_revision = wanted_lamp_revision;
                if (wanted_controls.has_value()) {
                    cview = *wanted_controls;
                } else if (wanted.has_value()) {
                    v = *wanted;
                    have_view = true;
                }
            }
            // The game's art and font (loaded on the assets thread; asked for at the tile's
            // install, so normally long ready).
            std::array<std::shared_ptr<const VR::Assets::MenuTexture>, 4> textures{};
            bool ready = true;
            for (size_t i = 0; i < TextureNames.size(); ++i) {
                textures[i] = VR::Assets::GetMenuTexture(TextureNames[i]);
                ready = ready && textures[i] != nullptr;
            }
            const GameText::GameFont* font = VR::Assets::GetPageFont();
            if (!ready || font == nullptr) {
                if (!waiting_logged) {
                    waiting_logged = true;
                    LOG_INFO(Core, "VR page: waiting for the game's menu art and font");
                }
                std::unique_lock lk{mutex};
                cv.wait_for(lk, std::chrono::milliseconds(50), [this] { return stop; });
                continue;
            }
            Page::Assets assets;
            const auto tex = [](const std::shared_ptr<const VR::Assets::MenuTexture>& t) {
                return Texture{static_cast<int>(t->width), static_cast<int>(t->height),
                               t->rgba.data()};
            };
            assets.common3 = tex(textures[0]);
            assets.common1 = tex(textures[1]);
            assets.dpad_lr = tex(textures[2]);
            assets.dpad_ud = tex(textures[3]);
            assets.font = font;
            {
                std::vector<DumpPage> dump_now;
                std::filesystem::path dir;
                {
                    std::scoped_lock lk{mutex};
                    dump_now.swap(dump);
                    dir = dump_dir;
                }
                if (!dump_now.empty()) {
                    DrawDump(assets, dir, dump_now);
                }
            }
            if (!cview && !have_view) {
                continue;
            }
            const auto start = std::chrono::steady_clock::now();
            auto page = std::make_shared<Image>();
            auto glow = std::make_shared<Image>();
            const bool ok = cview ? ControlsView::Render(assets, *cview, *page, *glow)
                                  : Render(assets, v, *page, *glow);
            if (ok) {
                // The compositor blends in linear light: the alpha the game's look needs there
                // (vr_page_render.h ToCompositorAlpha).
                ToCompositorAlpha(*page);
                ToCompositorAlpha(*glow);
            }
            const double ms = std::chrono::duration<double, std::milli>(
                                  std::chrono::steady_clock::now() - start)
                                  .count();
            std::scoped_lock lk{mutex};
            if (lamp_revision != wanted_lamp_revision)
                continue; // an older lamp page/session must not replace its newer image
            if (ok) {
                page_image = std::move(page);
                image_lamp_revision = lamp_revision;
                glow_image = std::move(glow);
                ++serial;
                ++stats.renders;
                stats.render_ms_sum += ms;
                stats.render_ms_max = std::max(stats.render_ms_max, ms);
                if (cview) {
                    // Show VR controls: every draw counted, the first ones and the slow ones
                    // logged (a light shows a draw later).
                    ++stats.controls_renders;
                    if (stats.controls_renders <= 3 || stats.controls_renders % 50 == 0 ||
                        ms > 80.0) {
                        LOG_INFO(Core, "VR controls: drawn {} ({:.1f} ms, tab {}, {} lit, focus {})",
                                 stats.controls_renders, ms,
                                 cview->tabs[static_cast<size_t>(cview->tab)],
                                 std::count_if(cview->lights.begin(), cview->lights.end(),
                                               [](ControlsView::Lit l) {
                                                   return l != ControlsView::Lit::Off;
                                               }),
                                 cview->focus >= 0 && cview->focus <
                                                          static_cast<int>(cview->callouts.size())
                                     ? cview->callouts[static_cast<size_t>(cview->focus)].title
                                     : std::string{"-"});
                    }
                } else if (stats.renders <= 3 || stats.renders % 50 == 0) {
                    LOG_INFO(Core, "VR page: drawn {} ({:.1f} ms, {}x{}, tab {})", stats.renders,
                             ms, page_image->width, page_image->height,
                             v.tabs.empty() ? "-" : v.tabs[static_cast<size_t>(v.tab)]);
                }
            }
            // A newer view asked for meanwhile is drawn next; this one is done.
            if (cview) {
                if (wanted_controls.has_value() && SameControls(*wanted_controls, *cview)) {
                    wanted_controls.reset();
                }
            } else if (wanted.has_value() && wanted->tab == v.tab && SameView(*wanted, v)) {
                wanted.reset();
            }
        }
    }

    // The model's changes since the last call into the log, read back from the live settings.
    void LogChanges() {
        const auto changes = model.TakeChanges();
        if (changes.empty()) {
            return;
        }
        {
            std::scoped_lock lk{mutex};
            stats.changes += static_cast<u32>(changes.size());
        }
        // Read back from the live settings: what the game and the presenter now use.
        std::string live;
        for (const Change& c : changes) {
            for (const auto& [key, value] : c.set) {
                if (key.starts_with("graphics_"))
                    continue; // Draft, not live renderer state.
                live += fmt::format("{}{}={}", live.empty() ? "" : ", ", key, store.Get(key));
            }
            for (const std::string& key : c.removed) {
                if (key.starts_with("graphics_"))
                    continue;
                live += fmt::format("{}{}={} (default)", live.empty() ? "" : ", ", key,
                                    store.Get(key));
            }
        }
        if (!live.empty()) {
            LOG_INFO(Core, "VR page: live now {}", live);
        }
    }

    static bool SameView(const View& a, const View& b) {
        if (a.tabs != b.tabs || a.headers != b.headers || a.tab != b.tab || a.help != b.help ||
            a.graphics_status != b.graphics_status || a.confirm != b.confirm ||
            a.hover != b.hover || a.page != b.page || a.pages != b.pages ||
            a.graphics != b.graphics || a.subtabs != b.subtabs ||
            a.strip_selected != b.strip_selected) {
            return false;
        }
        for (size_t i = 0; i < a.rows.size(); ++i) {
            const RowView& x = a.rows[i];
            const RowView& y = b.rows[i];
            if (x.present != y.present || x.value != y.value || x.selected != y.selected ||
                x.number != y.number || x.label != y.label || x.enabled != y.enabled) {
                return false;
            }
        }
        for (size_t i = 0; i < a.buttons.size(); ++i) {
            if (a.buttons[i].selected != b.buttons[i].selected ||
                a.buttons[i].label != b.buttons[i].label ||
                a.buttons[i].present != b.buttons[i].present) {
                return false;
            }
        }
        return a.glyphs == b.glyphs;
    }

    // ---- Show VR controls ----

    static bool SameControls(const ControlsView::View& a, const ControlsView::View& b) {
        return a.tab == b.tab && a.focus == b.focus && a.hover == b.hover &&
               a.hover_tab == b.hover_tab && a.lights == b.lights && a.help == b.help &&
               a.family == b.family && a.callouts.size() == b.callouts.size() &&
               a.glyphs == b.glyphs && a.pad_xbox == b.pad_xbox && a.gestures == b.gestures &&
               a.note == b.note;
    }

    // One frame of the controllers (and the gamepad) for the controls view.
    static ControlsView::Live LiveOf(const Input& in) {
        ControlsView::Live live;
        live.held = in.held;
        live.touched = in.touched;
        live.right_speed = in.right_speed;
        live.left_speed = in.left_speed;
        live.dt = in.dt;
        live.pad = in.pad_held;
        live.pad_stick_l = in.pad_stick_l;
        live.pad_stick_r = in.pad_stick_r;
        return live;
    }

    // The A on "Show VR controls": the view of the layout and the controllers in use now. The
    // controls held or touched as it opens are not news (the A that opened it lights, it does
    // not select).
    void OpenControls(const Input& in) {
        const ControlsView::Config config = ControlsView::CurrentConfig();
        const ControlsView::Live live = LiveOf(in);
        controls.Open(config, family, profile, dpad, live);
        controls.Update(live);
        controls_open = true;
        controls_hover_tab = -1;
        controls_b = in.held[static_cast<size_t>(Thumbs::Control::B)];
        {
            std::scoped_lock lk{mutex};
            ++stats.controls_opens;
        }
        LOG_INFO(Core, "VR controls: opened ({})",
                 ControlsView::Describe(config, family, profile, dpad));
        LogControlsTab();
    }

    // Every callout of the tab now, for the log (once per tab shown).
    void LogControlsTab() {
        std::string list;
        for (const ControlsView::Callout& c : controls.Callouts()) {
            list += fmt::format("{}{}{}", list.empty() ? "" : " | ", c.title,
                                c.sub.empty() ? std::string{} : " (" + c.sub + ")");
        }
        LOG_INFO(Core, "VR controls: tab {}: {}", controls.TabName(), list);
    }

    void CloseControls(const char* why) {
        controls_open = false;
        LOG_INFO(Core, "VR controls: closed ({}), back to the VR page", why);
    }

    // The controls view's input: the grips (or a click on a tab or a badge) switch tabs, the
    // stick walks the callouts, B returns to the page; everything pressed or touched lights up and
    // a press (or the touch of a face button or a trigger) selects its callout (the triggers only
    // light theirs here: there is no value to change); the pointer's halo.
    Event HandleControls(const Input& in) {
        Event event = Event::None;
        const ControlsView::Tab tab_before = controls.GetTab();
        const int focus_before = controls.Focus();
        // The pointer: what the ray is on.
        int hover = -1;
        int hover_tab = -1;
        if (in.ray_on_page) {
            if (const ControlsView::HitBox* b =
                    ControlsView::HitTest(controls_boxes, in.ray_x, in.ray_y)) {
                switch (b->kind) {
                case ControlsView::HitBox::Kind::Callout:
                    hover = b->index;
                    break;
                case ControlsView::HitBox::Kind::Tab:
                    hover_tab = b->index;
                    break;
                case ControlsView::HitBox::Kind::Prev:
                    hover_tab = -2;
                    break;
                case ControlsView::HitBox::Kind::Next:
                    hover_tab = -3;
                    break;
                }
            }
        }
        controls.SetHover(hover);
        bool dirty = false;
        if (hover_tab != controls_hover_tab) {
            controls_hover_tab = hover_tab;
            dirty = true;
        }
        if (in.ray_on_page && in.trigger_pressed) {
            if (hover_tab >= 0) {
                controls.SetTab(hover_tab);
            } else if (hover_tab == -2) {
                controls.TabStep(-1);
            } else if (hover_tab == -3) {
                controls.TabStep(1);
            }
            if (hover_tab != -1 || hover >= 0) {
                event = Event::Clicked;
            }
        }
        if (in.tab_prev) {
            controls.TabStep(-1);
        }
        if (in.tab_next) {
            controls.TabStep(1);
        }
        if (in.up) {
            controls.Move(Dir::Up);
        }
        if (in.down) {
            controls.Move(Dir::Down);
        }
        if (in.left) {
            controls.Move(Dir::Left);
        }
        if (in.right) {
            controls.Move(Dir::Right);
        }
        // The lights (after a tab step: the grip that stepped lights on the new tab).
        const ControlsView::Live live = LiveOf(in);
        controls.Update(live);
        if (controls.GetTab() != tab_before) {
            LogControlsTab();
        }
        if ((controls.Focus() != focus_before || controls.GetTab() != tab_before) &&
            controls.Focus() >= 0) {
            const auto& c = controls.Callouts()[static_cast<size_t>(controls.Focus())];
            LOG_INFO(Core, "VR controls: {} selected ({}{})", c.title, controls.TabName(),
                     c.help.empty() ? std::string{} : ": " + c.help);
        }
        // B (the physical B; without the live state: the page's B or Y), or the gamepad's
        // Circle, returns to the page.
        const bool b = in.held[static_cast<size_t>(Thumbs::Control::B)];
        const bool back = (in.live ? (b && !controls_b) : in.back) || in.pad_back;
        controls_b = b;
        if (back) {
            CloseControls("B");
            Redraw();
            return Event::Clicked;
        }
        if (controls.TakeDirty() || dirty) {
            Redraw();
            if (event == Event::None &&
                (in.up || in.down || in.left || in.right || in.tab_prev || in.tab_next)) {
                event = Event::Clicked;
            }
        }
        return event;
    }
};

Runtime& Runtime::Get() {
    static Runtime* runtime = new Runtime; // never destroyed (its worker may outlive statics)
    return *runtime;
}

Runtime::Runtime() : impl{std::make_unique<Impl>()} {}
Runtime::~Runtime() = default;

void Runtime::RequestOpen() {
    impl->request_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(
                               std::chrono::steady_clock::now().time_since_epoch())
                               .count(),
                           std::memory_order_release);
    impl->requests.fetch_add(1, std::memory_order_acq_rel);
}

double Runtime::SecondsSinceRequest() const {
    const s64 t = impl->request_ns.load(std::memory_order_acquire);
    if (t == 0) {
        return 1e9;
    }
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    return static_cast<double>(now - t) * 1e-9;
}

bool Runtime::PollOpen() {
    if (auto offer = SoftSave::Mailbox().TakeOffer()) {
        if (impl->open) Close("death choice");
        impl->death = *offer;
        impl->death_focus = 0;
        impl->death_hover = {};
        impl->death_presented = 0;
        impl->death_offer_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count();
        RequestOpen();
    }
    if (auto offer = Lamps::Mailbox().TakeOffer()) {
        if (impl->open) {
            Lamps::Mailbox().Cancel(offer->serial);
            return false;
        }
        impl->lamp_presentation.Reset();
        impl->lamp.emplace(std::move(*offer));
        impl->lamp_hover = {};
        RequestOpen();
    }
    const u32 r = impl->requests.load(std::memory_order_acquire);
    if (r == impl->handled) {
        return false;
    }
    impl->handled = r;
    if (impl->open) {
        return false;
    }
    impl->open = true;
    impl->closing = false;
    impl->live_presentation = impl->death != 0;
    impl->opacity = 0.0f;
    {
        std::scoped_lock lk{impl->mutex};
        impl->opened_serial = impl->serial; // the last session's image is never shown again
        impl->changes_at_open = impl->stats.changes;
    }
    impl->slide = SlidePx;
    impl->pulse_t = 0.0f;
    impl->trigger_down = false;
    impl->model.Reset();
    // VR_PAGE_TEST_OPEN=<tab>[,<page>] (tests: the mock's view of each tab): the page opens on that
    // tab (its name as in Tabs()) and page (0 = the first).
    if (const char* test = std::getenv("VR_PAGE_TEST_OPEN"); test != nullptr && test[0] != '\0') {
        const std::string spec{test};
        const size_t comma = spec.find(',');
        const std::string name = spec.substr(0, comma);
        const int page = comma == std::string::npos ? 0 : std::atoi(spec.c_str() + comma + 1);
        for (int t = 0; t < static_cast<int>(Tabs().size()); ++t) {
            if (name == Tabs()[static_cast<size_t>(t)].name) {
                impl->model.SetTab(t);
                impl->model.PageStep(page);
                LOG_INFO(Core, "VR page: VR_PAGE_TEST_OPEN: {} page {}", name, page + 1);
            }
        }
    }
    impl->Redraw();
    if (const char* dir = std::getenv("VR_PAGE_TEST_DUMP");
        dir != nullptr && dir[0] != '\0' && !impl->dump_posted) {
        impl->PostDump(dir);
    }
    {
        std::scoped_lock lk{impl->mutex};
        ++impl->stats.opens;
    }
    LOG_INFO(Core, "VR page: opened ({}; vr.ini {})", Tabs()[0].name, SettingsFilePath());
    return true;
}

bool Runtime::IsOpen() const {
    return impl->open;
}

bool Runtime::Visible() const {
    return impl->open || impl->opacity > 0.0f;
}

bool Runtime::RequiresLivePresentation() const {
    return impl->live_presentation && Visible();
}

void Runtime::Close(const char* why) {
    if (!impl->open) {
        return;
    }
    impl->open = false;
    impl->closing = true;
    if (impl->controls_open) {
        impl->CloseControls(why);
    }
    if (impl->lamp) {
        Lamps::Mailbox().Cancel(impl->lamp->offer.serial);
        impl->lamp.reset();
    }
    if (impl->death) {
        SoftSave::Mailbox().Choose(impl->death, false);
        impl->death = 0;
    }
    impl->lamp_presentation.Reset();
    impl->model.DiscardGraphics();
    impl->model.Release();
    impl->model.SetHover({});
    u32 changes = 0;
    {
        std::scoped_lock lk{impl->mutex};
        ++impl->stats.closes;
        changes = impl->stats.changes - impl->changes_at_open;
    }
    FlushSettings();
    LOG_INFO(Core, "VR page: closed ({}); {} changes this session", why, changes);
}

void Runtime::GameMenuClosed() {
    if (impl->open) {
        Close("the game's menu closed");
    }
}

Event Runtime::HandleInput(const Input& in) {
    if (!impl->open) {
        return Event::None;
    }
    if (impl->death) {
        if (!SoftSave::Mailbox().Active(impl->death)) {
            Close("soft save context ended"); return Event::Closed;
        }
        // Input only on a view already shown; back once the page was seen (DeathInputTaken).
        const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now().time_since_epoch())
                            .count();
        if (!DeathInputTaken(in.back,
                             impl->death_presented == (impl->death_revision | (u64{1} << 63)),
                             impl->death_presented != 0, now - impl->death_offer_ns))
            return Event::None;
        impl->death_hover = in.ray_on_page ? HitTest(impl->boxes, in.ray_x, in.ray_y) : Hover{};
        int choice = -1;
        if (in.back) choice = 0;
        else if (in.confirm) choice = impl->death_focus;
        else if (in.trigger_pressed && impl->death_hover.target == Target::Row && impl->death_hover.index >= 0 && impl->death_hover.index < 2)
            choice = impl->death_hover.index;
        // Up the lantern, down the soft save: a held D-pad's repeat never flips it back.
        else if (in.up) impl->death_focus = 0;
        else if (in.down) impl->death_focus = 1;
        if (choice >= 0) {
            SoftSave::Mailbox().Choose(impl->death, choice == 1);
            impl->death = 0;
            Close("death choice submitted"); return Event::Closed;
        }
        if (!Impl::SameView(SoftSave::DeathPage(impl->death_focus, impl->glyphs, impl->death_hover, SoftSave::Mailbox().When()), impl->view))
            impl->Redraw();
        return Event::None;
    }
    if (impl->lamp) {
        auto& m = *impl->lamp;
        if (!Lamps::Mailbox().Active(m.offer.serial)) {
            Close("lamp context ended");
            return Event::Closed;
        }
        // Back can escape a stalled redraw. Other input waits for this selection view.
        if (!in.back && !impl->lamp_presentation.Ready(m))
            return Event::None;
        std::optional<Lamps::Request> action;
        impl->lamp_hover = in.ray_on_page ? HitTest(impl->boxes, in.ray_x, in.ray_y) : Hover{};
        if (in.back)
            action = m.Back();
        else if (in.trigger_pressed && impl->lamp_hover.target == Target::Row) {
            action = impl->lamp_presentation.Confirm(m, impl->lamp_hover.index);
        } else if (in.confirm)
            action = impl->lamp_presentation.Confirm(m);
        else {
            if (in.up)
                m.Move(-1);
            else if (in.down)
                m.Move(1);
            if (in.tab_prev || in.left || in.value_prev ||
                (in.trigger_pressed && impl->lamp_hover.target == Target::TabPrev))
                m.Tab(-1);
            else if (in.tab_next || in.right || in.value_next ||
                     (in.trigger_pressed && impl->lamp_hover.target == Target::TabNext))
                m.Tab(1);
        }
        if (action) {
            const bool sent = Lamps::Mailbox().Submit(*action);
            if (sent)
                impl->lamp.reset(); // closing the view must not cancel the queued action
            Close(sent ? "lamp action queued" : "lamp request expired");
            return Event::Closed;
        }
        if (!Impl::SameView(impl->LampView(), impl->view))
            impl->Redraw();
        return Event::None;
    }
    if (impl->controls_open) {
        return impl->HandleControls(in);
    }
    Model& m = impl->model;
    Event event = Event::None;
    Action action = Action::None;
    // The pointer first: what the ray is on (the halo, the glow), then its trigger.
    Hover hover{};
    if (in.ray_on_page) {
        hover = HitTest(impl->boxes, in.ray_x, in.ray_y);
    }
    m.SetHover(hover);
    if (in.ray_on_page && in.trigger_pressed &&
        (hover.target == Target::None || hover.target == Target::Page)) {
        // On the page but on nothing that reacts: the trigger keeps its value step.
        m.Value(in.trigger_step);
        event = Event::Clicked;
    } else if (in.ray_on_page && in.trigger_pressed) {
        float fraction = 0.0f;
        if (hover.target == Target::SliderBar) {
            const auto bar = SliderBarSpan(hover.index);
            fraction = (in.ray_x - bar[0]) / std::max(bar[1] - bar[0], 1.0f);
        }
        action = m.Click(fraction);
        event = Event::Clicked;
    } else if (in.trigger_held && m.GetState().dragging >= 0 && in.ray_on_page) {
        const auto bar = SliderBarSpan(m.GetState().dragging);
        m.Drag((in.ray_x - bar[0]) / std::max(bar[1] - bar[0], 1.0f));
    }
    if (in.trigger_released) {
        m.Release();
    }
    if (in.up) {
        m.Move(Dir::Up);
    }
    if (in.down) {
        m.Move(Dir::Down);
    }
    if (in.left) {
        m.Move(Dir::Left);
    }
    if (in.right) {
        m.Move(Dir::Right);
    }
    if (in.value_prev) {
        m.Value(-1);
    }
    if (in.value_next) {
        m.Value(1);
    }
    if (in.tab_prev) {
        m.TabStep(-1);
    }
    if (in.tab_next) {
        m.TabStep(1);
    }
    if (in.confirm) {
        action = m.Confirm();
    }
    if (in.back) {
        const Action back = m.Back();
        if (back == Action::Close) {
            Close("B");
            return Event::Closed;
        }
    }
    switch (action) {
    case Action::SoftSave:
        if (SoftSave::Mailbox().Save()) m.SetNotice("Saving...");
        else m.SetNotice(SoftSave::Mailbox().Status());
        impl->Redraw();
        return Event::Clicked;
    case Action::FreeCamera:
        // On or off, then straight into the view: the page and the game's menu close (CloseAll).
        VR::SetFreeCamera(!VR::IsFreeCamera(), "the VR page");
        Close("free camera");
        return Event::CloseAll;
    case Action::ClearSoftSave:
        if (SoftSave::Mailbox().Clear()) m.SetNotice("Clearing soft save...");
        else m.SetNotice(SoftSave::Mailbox().Status());
        impl->Redraw();
        return Event::Clicked;
    case Action::RestartGraphics: {
        // Let the main thread flush settings and perform the normal save/VR teardown.
        Graphics::restart_requested.store(true, std::memory_order_release);
        SDL_Event wake{};
        wake.type = SDL_EVENT_USER;
        SDL_PushEvent(&wake);
        m.SetNotice("Saving settings and restarting...");
        impl->Redraw();
        return Event::Clicked;
    }
    case Action::ShowControls:
        // Show VR controls (core/vr/controls_view.h): over the page until B.
        impl->OpenControls(in);
        impl->Redraw();
        return Event::Clicked;
    default:
        break;
    }
    impl->LogChanges();
    // A value changed outside the page while it is up (the stick-click chord's control style,
    // Menu + stick's eye distance): the page shows it within half a second.
    impl->refresh_s += in.dt;
    if (impl->refresh_s >= 0.5f) {
        impl->refresh_s = 0.0f;
        if (!Impl::SameView(MakeView(m, impl->glyphs), impl->view)) {
            m.MarkDirty();
        }
    }
    if (m.TakeDirty()) {
        impl->Redraw();
        if (event == Event::None && (in.up || in.down || in.left || in.right || in.confirm ||
                                     in.tab_prev || in.tab_next)) {
            event = Event::Clicked;
        }
    }
    return event;
}

void Runtime::SetNotice(const std::string& text) {
    if (!impl->open) {
        return;
    }
    impl->model.SetNotice(text);
    if (impl->model.TakeDirty()) {
        impl->Redraw();
    }
}

void Runtime::Tick(float dt) {
    if (impl->death && !SoftSave::Mailbox().Active(impl->death)) Close("soft save context ended");
    if (impl->open && !impl->death && !impl->lamp && std::string_view(impl->model.CurrentTab().name) == "Save") {
        const auto status = SoftSave::Mailbox().Status();
        if (impl->model.GetState().notice != status) SetNotice(status);
    }
    if (impl->lamp) {
        if (!Lamps::Mailbox().Active(impl->lamp->offer.serial))
            Close("lamp context ended");
    }
    if (const char* failure = Graphics::restart_failure.exchange(nullptr, std::memory_order_acquire))
        SetNotice(failure);
    bool fresh = false;
    {
        std::scoped_lock lk{impl->mutex};
        fresh = impl->serial > impl->opened_serial;
    }
    if (impl->open && !fresh) {
        // Open, its first image still being drawn: nothing shown yet.
    } else if (impl->open) {
        impl->opacity = std::min(1.0f, impl->opacity + dt / FadeInS);
        impl->slide = SlidePx * (1.0f - impl->opacity);
    } else if (impl->opacity > 0.0f) {
        impl->opacity = std::max(0.0f, impl->opacity - dt / FadeOutS);
        impl->slide = SlidePx * (1.0f - impl->opacity);
        if (impl->opacity <= 0.0f) {
            impl->closing = false;
        }
    }
    impl->pulse_t += dt;

}

float Runtime::Opacity() const {
    return impl->opacity;
}

float Runtime::SlideStagePx() const {
    return impl->slide;
}

float Runtime::GlowPulse() const {
    // The game's cursor: 30 frames at 30 fps, alpha 1 -> 0.4 (frame 14) -> 1.
    const float phase = std::fmod(impl->pulse_t, 1.0f);
    const float tri = phase < 0.5f ? phase / 0.5f : (1.0f - phase) / 0.5f; // 0 -> 1 -> 0
    return 1.0f - 0.6f * tri;
}

void Runtime::SetGlyphs(const Glyphs& glyphs) {
    if (glyphs == impl->glyphs) {
        return;
    }
    impl->glyphs = glyphs;
    if (impl->open) {
        impl->Redraw();
    }
}

void Runtime::SetController(const std::string& profile, bool dpad, std::string_view system) {
    const ControlsView::Family family = ControlsView::FamilyOf(profile, system);
    if (profile == impl->profile && dpad == impl->dpad && family == impl->family) {
        return;
    }
    impl->profile = profile;
    impl->dpad = dpad;
    impl->family = family;
    impl->store.controller_parts = ControlsView::PartsOf(family, profile, dpad);
    if (impl->open) {
        impl->Redraw();
    }
}

bool Runtime::ControlsOpen() const {
    return impl->open && impl->controls_open;
}

bool Runtime::TakeImage(u64& serial, std::shared_ptr<const Image>& page,
                        std::shared_ptr<const Image>& glow, u64& lamp_revision) {
    std::scoped_lock lk{impl->mutex};
    if (impl->serial == serial || impl->page_image == nullptr) {
        return false;
    }
    serial = impl->serial;
    lamp_revision = impl->image_lamp_revision;
    page = impl->page_image;
    glow = impl->glow_image;
    return true;
}

void Runtime::ImagePresented(u64 lamp_revision) {
    if (impl->death && lamp_revision == (impl->death_revision | (u64{1} << 63)))
        impl->death_presented = lamp_revision;
    impl->lamp_presentation.Presented(lamp_revision);
    if (impl->lamp && impl->lamp_presentation.Ready(*impl->lamp))
        Lamps::Mailbox().Present(impl->lamp->offer.serial);
}

Runtime::Stats Runtime::GetStats() const {
    std::scoped_lock lk{impl->mutex};
    return impl->stats;
}

} // namespace VR::Page
