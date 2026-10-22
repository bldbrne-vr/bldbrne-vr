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
#include "core/vr/lamp_presentation.h"
#include "core/vr/gun_settings.h"
#include "core/vr/color_settings.h"
#include "core/vr/graphics_status.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <optional>
#include <thread>
#include <SDL3/SDL_events.h>
#include <SDL3/SDL_filesystem.h>

#include "common/logging/log.h"
#include "common/memory_patcher.h"
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

// An upscaler's files beside the runtime (its folder there, or the VR_*_DIR override): a release
// without FSR 4 or DLSS 5 greys their rows instead of offering a choice that cannot start.
bool ProviderInstalled(const char* env, const char* folder, const char* file);

// The accepted upscaler's files are installed (Off: true). Checked once each.
bool UpscalerFilesPresent(const Graphics::Config& c) {
    static const bool dlss = ProviderInstalled("VR_DLSS_DIR", "dlss", "vr_dlss_stereo.dll") &&
                             ProviderInstalled("VR_DLSS_DIR", "dlss", "nvngx_dlss.dll");
    static const bool neural = ProviderInstalled("VR_DLSS_DIR", "dlss", "nvngx_dlssnr.dll");
    static const bool fsr4 = ProviderInstalled("VR_FSR4_DIR", "fsr4", "vr_fsr4_stereo.dll");
    static const bool fsr31 = ProviderInstalled("VR_FSR31_DIR", "fsr31", "vr_fsr31_stereo.dll");
    switch (c.upscaler) {
    case Graphics::Upscaler::Dlss:
        return dlss && (!c.dlss5 || neural);
    case Graphics::Upscaler::Fsr4:
        return fsr4;
    case Graphics::Upscaler::Fsr31:
        return fsr31;
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
    if (key == "gun_wrist_reload")
        return Bool(Gun::GunSettings{}.wrist_reload);
    if (key == "color_contrast" || key == "color_saturation" || key == "color_gamma")
        return "0";
    if (key == "two_hand_guide")
        return Bool(TwoHand::Settings{}.guide);
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
    } else if (key == "haptic_gain") {
        return FormatIni(s.haptic_gain);
    } else if (key == "controls") {
        return s.controls_classic ? "classic" : "motion";
    } else if (key == "inject") {
        return Bool(s.inject);
    } else if (key == "laser") {
        return Bool(s.laser);
    } else if (key == "grip_adjust") {
        return Bool(b.grip_adjust);
    } else if (key == "vr_grab_attacks") {
        return Bool(s.vr_grab_attacks);
    } else if (key == "cinematic") {
        return Bool(s.cinematic);
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
    } else if (key == "dodge_iframes_extra") {
        return std::to_string(s.dodge_iframes_extra);
    } else if (key == "melee_kill_ragdoll") {
        return Bool(s.melee_kill_ragdoll);
    } else if (key == "melee_ragdoll_impulse") {
        return FormatIni(s.melee_ragdoll_impulse);
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
    } else if (key == "graphics_neural_available") {
        // DLSS 5 needs NVIDIA's neural runtime beside the DLSS bridge.
        static const bool installed = ProviderInstalled("VR_DLSS_DIR", "dlss", "nvngx_dlssnr.dll");
        return installed ? "1" : "0";
    } else if (key == "ui_enemy_hp_bars") {
        return Bool(s.ui_enemy_hp_bars);
    } else if (key == "ui_boss_bar") {
        return Bool(s.ui_boss_hp_bar);
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
        if (key == "two_hand_swing_hum")
            return FormatIni(TwoHand::GetTwoHandSettings().swing_hum);
        if (key == "lamp_travel")
            return Bool(Lamps::Enabled());
        if (key == "physical_summon_bell")
            return Bool(SummonBell::enabled.load());
        if (key == "physical_levers")
            return Bool(Lever::Enabled());
        if (key == "graphics_config")
            return Graphics::Serialize(Graphics::Accepted());
        if (key == "graphics_native_stereo")
            return std::getenv("VR_NATIVE_STEREO") ? "1" : "0";
        if (key == "graphics_status" && !std::getenv("VR_NATIVE_STEREO"))
            return "DIBR | Scale applies after restart. DLSS/FSR unavailable; saved settings kept.";
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
    void Remove(const std::string& key) override {
        if (key == "weighted_weapons") {
            Set(key, "1"); // the default: on
            return;
        }
        static const Settings defaults{};
        static const Body::BodySettings body_defaults{};
        const std::string value = ValueIn(defaults, body_defaults, Quick::QuickSettings{},
                                          Melee::MeleeSettings{}, Tricks::Settings{}, key);
        if (!value.empty()) {
            ApplySetting(key, value);
        }
        // Reset must not revive a legacy melee_stamina=1 override on the next launch.
        if (key == "melee_stamina_pct")
            RemoveSetting("melee_stamina");
        RemoveSetting(key);
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

} // Anonymous namespace

struct Runtime::Impl {
    std::optional<Lamps::Model> lamp;
    Hover lamp_hover{};
    Lamps::Presentation lamp_presentation;
    SettingsStore store;
    Model model{store, [](const std::string& line) { LOG_INFO(Core, "{}", line); }};
    std::atomic<u32> requests{0};
    // When the last request came (steady clock ns; 0 = never): any thread reads it
    // (SecondsSinceRequest: the UI layer holds the game's menu-less frames after the A).
    std::atomic<s64> request_ns{0};
    u32 handled{0};
    bool open{false};
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
        view = lamp ? LampView() : MakeView(model, glyphs);
        boxes = HitBoxes(view, VR::Assets::GetPageFont());
        const u64 lamp_revision = lamp ? lamp_presentation.Update(*lamp) : 0;
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
            {
                std::unique_lock lk{mutex};
                cv.wait(lk, [this] {
                    return stop || wanted.has_value() || wanted_controls.has_value();
                });
                if (stop) {
                    return;
                }
                lamp_revision = wanted_lamp_revision;
                if (wanted_controls.has_value()) {
                    cview = *wanted_controls;
                } else {
                    v = *wanted;
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
            a.hover != b.hover) {
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
               a.glyphs == b.glyphs;
    }

    // The A on "Show VR controls": the view of the layout and the controllers in use now. The
    // controls held or touched as it opens are not news (the A that opened it lights, it does
    // not select).
    void OpenControls(const Input& in) {
        const ControlsView::Config config = ControlsView::CurrentConfig();
        const ControlsView::Family family = ControlsView::FamilyOf(profile);
        ControlsView::Live live;
        live.held = in.held;
        live.touched = in.touched;
        live.right_speed = in.right_speed;
        live.left_speed = in.left_speed;
        live.dt = in.dt;
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
        LOG_INFO(Core, "VR controls: tab {}: {}",
                 ControlsView::TabNames[static_cast<size_t>(controls.GetTab())], list);
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
        ControlsView::Live live;
        live.held = in.held;
        live.touched = in.touched;
        live.right_speed = in.right_speed;
        live.left_speed = in.left_speed;
        live.dt = in.dt;
        controls.Update(live);
        if (controls.GetTab() != tab_before) {
            LogControlsTab();
        }
        if ((controls.Focus() != focus_before || controls.GetTab() != tab_before) &&
            controls.Focus() >= 0) {
            const auto& c = controls.Callouts()[static_cast<size_t>(controls.Focus())];
            LOG_INFO(Core, "VR controls: {} selected ({}{})", c.title,
                     ControlsView::TabNames[static_cast<size_t>(controls.GetTab())],
                     c.help.empty() ? std::string{} : ": " + c.help);
        }
        // B (the physical B; without the live state: the page's B or Y) returns to the page.
        const bool b = in.held[static_cast<size_t>(Thumbs::Control::B)];
        const bool back = in.live ? (b && !controls_b) : in.back;
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
    impl->Redraw();
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

void Runtime::SetController(const std::string& profile, bool dpad) {
    if (profile == impl->profile && dpad == impl->dpad) {
        return;
    }
    impl->profile = profile;
    impl->dpad = dpad;
    impl->store.controller_parts = ControlsView::PartsOf(ControlsView::FamilyOf(profile), profile, dpad);
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
    impl->lamp_presentation.Presented(lamp_revision);
    if (impl->lamp && impl->lamp_presentation.Ready(*impl->lamp))
        Lamps::Mailbox().Present(impl->lamp->offer.serial);
}

Runtime::Stats Runtime::GetStats() const {
    std::scoped_lock lk{impl->mutex};
    return impl->stats;
}

} // namespace VR::Page
