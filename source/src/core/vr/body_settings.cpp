// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <mutex>
#include <ostream>
#include <string_view>

#include "common/logging/log.h"
#include "core/vr/bb_weapons.h"
#include "core/vr/body_settings.h"
#include "core/vr/live_tune.h"
#include "core/vr/vr_session.h"

namespace VR::Body {

namespace {

bool Flag(const std::string& value) {
    return value == "1" || value == "true";
}

// Per-weapon grips (weapon_grip_<weapon> lines and the live adjust). The pose hook reads them on
// the loop thread when the weapon or the generation changes, the presenter sets them.
struct WeaponGripEntry {
    s32 family{-1};
    GripTrim trim{};
};

struct WeaponGrips {
    std::mutex mutex;
    std::array<WeaponGripEntry, 64> entries{};
    u32 count{0};
    std::atomic<u32> generation{0};
};

WeaponGrips& Grips() {
    static WeaponGrips grips;
    return grips;
}

// Stores (nullptr: removes) a family's grip.
void StoreGrip(s32 family, const GripTrim* trim) {
    auto& g = Grips();
    {
        std::scoped_lock lock{g.mutex};
        u32 index = 0;
        while (index < g.count && g.entries[index].family != family) {
            ++index;
        }
        if (trim == nullptr) {
            if (index < g.count) {
                g.entries[index] = g.entries[g.count - 1];
                --g.count;
            }
        } else if (index < g.count) {
            g.entries[index].trim = *trim;
        } else if (g.count < g.entries.size()) {
            g.entries[g.count++] = {family, *trim};
        } else {
            LOG_WARNING(Core, "VR body: more than {} weapon grips; {} not kept", g.entries.size(),
                        WeaponGripKey(family));
        }
    }
    g.generation.fetch_add(1, std::memory_order_release);
}

// A grip within the per-weapon limits; `clamped` says whether it had to be moved.
GripTrim LimitGrip(GripTrim t, bool& clamped) {
    const auto limit = [&clamped](float& value, float max) {
        const float v = std::isfinite(value) ? std::clamp(value, -max, max) : 0.0f;
        clamped |= v != value;
        value = v;
    };
    for (float& v : t.offset_m) {
        if (!std::isfinite(v)) {
            clamped = true;
            v = 0.0f;
        }
    }
    limit(t.pitch_deg, GripPitchLimitDeg);
    limit(t.yaw_deg, GripYawLimitDeg);
    limit(t.roll_deg, GripRollLimitDeg);
    const float length = std::sqrt(t.offset_m[0] * t.offset_m[0] + t.offset_m[1] * t.offset_m[1] +
                                   t.offset_m[2] * t.offset_m[2]);
    if (length > GripOffsetLimitM) {
        clamped = true;
        for (float& v : t.offset_m) {
            v *= GripOffsetLimitM / length;
        }
    }
    return t;
}

std::string GripText(const GripTrim& t) {
    return fmt::format("{:.1f},{:.1f},{:.1f},{:.1f},{:.1f},{:.1f}", t.pitch_deg, t.yaw_deg,
                       t.roll_deg, t.offset_m[0] * 100.0f, t.offset_m[1] * 100.0f,
                       t.offset_m[2] * 100.0f);
}

// weapon_grip_<weapon>=pitch,yaw,roll[,x_cm,y_cm,z_cm] or =default. <weapon> is a key of
// core/vr/bb_weapons.h or a family number.
void ParseWeaponGrip(const std::string& key, const std::string& value) {
    const std::string name = key.substr(std::string_view{"weapon_grip_"}.size());
    s32 family = -1;
    if (const auto* f = Weapons::FindKey(name); f != nullptr) {
        family = f->family;
    } else if (!name.empty() && name.size() <= 4 &&
               std::all_of(name.begin(), name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
        family = static_cast<s32>(std::strtol(name.c_str(), nullptr, 10));
    } else {
        LOG_WARNING(Core, "VR body: {} names no weapon; ignored", key);
        return;
    }
    if (value == "default") {
        StoreGrip(family, nullptr);
        return;
    }
    std::array<float, 6> v{};
    size_t count = 0;
    const char* p = value.c_str();
    while (count < v.size()) {
        char* end = nullptr;
        const float parsed = std::strtof(p, &end);
        if (end == p || !std::isfinite(parsed)) {
            break;
        }
        v[count++] = parsed;
        p = end;
        while (*p == ' ') {
            ++p;
        }
        if (*p != ',') {
            break;
        }
        ++p;
    }
    if (count != 3 && count != 6) {
        LOG_WARNING(Core,
                    "VR body: {}={} is not pitch,yaw,roll[,x_cm,y_cm,z_cm] or default; ignored",
                    key, value);
        return;
    }
    bool clamped = false;
    const GripTrim trim =
        LimitGrip({v[0], v[1], v[2], {v[3] * 0.01f, v[4] * 0.01f, v[5] * 0.01f}}, clamped);
    if (clamped) {
        LOG_WARNING(Core,
                    "VR body: {}={} is outside +-{}/{}/{} deg, {} cm; using {} (pitch,yaw,roll,"
                    "x,y,z cm)",
                    key, value, GripPitchLimitDeg, GripYawLimitDeg, GripRollLimitDeg,
                    GripOffsetLimitM * 100.0f, GripText(trim));
    }
    StoreGrip(family, &trim);
}

// A number limited to lo .. hi; anything outside is clamped and logged, a non-number keeps
// `current`.
float Ranged(const std::string& key, const std::string& value, float lo, float hi, float current) {
    const float parsed = std::stof(value);
    if (!std::isfinite(parsed)) {
        LOG_WARNING(Core, "VR body: {}={} is not a number; keeping {}", key, value, current);
        return current;
    }
    const float clamped = std::clamp(parsed, lo, hi);
    if (clamped != parsed) {
        LOG_WARNING(Core, "VR body: {}={} is outside {} .. {}; using {}", key, value, lo, hi,
                    clamped);
    }
    return clamped;
}

} // Anonymous namespace

BodySettings& GetBodySettings() {
    static BodySettings settings;
    return settings;
}

const char* BodyModeName(BodyMode mode) {
    switch (mode) {
    case BodyMode::NoHead:
        return "no_head";
    case BodyMode::ArmsOnly:
        return "arms_only";
    case BodyMode::Full:
        return "full";
    case BodyMode::HandsOnly:
        return "hands_only";
    }
    return "?";
}

bool HeadHiddenByRenderer() {
    static const int env = [] {
        const char* value = std::getenv("VR_HEAD_SHADOW");
        if (value == nullptr || *value == '\0') {
            return -1;
        }
        const int v = *value == '0' ? 0 : 1;
        LOG_INFO(Core, "VR body: VR_HEAD_SHADOW={} overrides vr.ini body_head_shadow", v);
        return v;
    }();
    const auto& s = GetBodySettings();
    const bool ini = env >= 0 ? env == 1 : s.body_head_shadow;
    const bool on = VR::Tune::Get("body.head_shadow", ini ? 1.0f : 0.0f) > 0.5f;
    return HeadHiddenByRendererFor(s.body, EffectiveBodyMode(), on);
}

BodyMode EffectiveBodyMode() {
    const auto& s = GetBodySettings();
    const float tuned = VR::Tune::Get("body.mode", static_cast<float>(s.mode));
    const int mode = static_cast<int>(tuned + 0.5f);
    return mode == 1   ? BodyMode::ArmsOnly
           : mode == 2 ? BodyMode::Full
           : mode == 3 ? BodyMode::HandsOnly
                       : BodyMode::NoHead;
}

DrawnArms DrawnArmsNow() {
    const bool by_draw = VR::Tune::Get("body.arms_draw", 1.0f) > 0.5f;
    return DrawnArmsFor(GetBodySettings().body, EffectiveBodyMode(), by_draw);
}

bool ParseBodySetting(const std::string& key, const std::string& value) {
    auto& s = GetBodySettings();
    if (key == "body") {
        s.body = Flag(value);
    } else if (key == "body_mode") {
        if (value == "no_head") {
            s.mode = BodyMode::NoHead;
        } else if (value == "arms_only") {
            s.mode = BodyMode::ArmsOnly;
        } else if (value == "full") {
            s.mode = BodyMode::Full;
        } else if (value == "hands_only") {
            s.mode = BodyMode::HandsOnly;
        } else {
            LOG_WARNING(Core,
                        "VR body: body_mode={} is not no_head, arms_only, hands_only or full; "
                        "using {}",
                        value, BodyModeName(s.mode));
        }
    } else if (key == "hide_cape") {
        s.hide_cape = Flag(value);
    } else if (key == "body_cutaway") {
        s.body_cutaway = Flag(value);
    } else if (key == "body_cutaway_near_m") {
        s.body_cutaway_near_m = Ranged(key, value, 0.0f, 0.6f, s.body_cutaway_near_m);
    } else if (key == "body_cutaway_far_m") {
        s.body_cutaway_far_m = Ranged(key, value, 0.0f, 0.8f, s.body_cutaway_far_m);
    } else if (key == "body_cutaway_pitch_deg") {
        s.body_cutaway_pitch_deg = Ranged(key, value, 0.0f, 85.0f, s.body_cutaway_pitch_deg);
    } else if (key == "body_cutaway_inside") {
        s.body_cutaway_inside = Flag(value);
    } else if (key == "body_cutaway_boost") {
        s.body_cutaway_boost = Flag(value);
    } else if (key == "body_cutaway_boost_near_m") {
        s.body_cutaway_boost_near_m =
            Ranged(key, value, 0.0f, 0.6f, s.body_cutaway_boost_near_m);
    } else if (key == "body_cutaway_boost_far_m") {
        s.body_cutaway_boost_far_m = Ranged(key, value, 0.0f, 0.8f, s.body_cutaway_boost_far_m);
    } else if (key == "body_cutaway_shoulders") {
        s.body_cutaway_shoulders = Flag(value);
    } else if (key == "body_head_shadow") {
        s.body_head_shadow = Flag(value);
    } else if (key == "arm_ik") {
        s.arm_ik = Flag(value);
    } else if (key == "arm_pair_sync") {
        s.arm_pair_sync = Flag(value);
    } else if (key == "torso_follow") {
        s.torso_follow = Flag(value);
    } else if (key == "torso_deadzone_deg") {
        s.torso_deadzone_deg = Ranged(key, value, 0.0f, 90.0f, s.torso_deadzone_deg);
    } else if (key == "torso_max_deg") {
        s.torso_max_deg = Ranged(key, value, 0.0f, 120.0f, s.torso_max_deg);
    } else if (key == "torso_follow_ms") {
        s.torso_follow_ms = Ranged(key, value, 0.0f, 2000.0f, s.torso_follow_ms);
    } else if (key == "body_facing") {
        s.body_facing = Flag(value);
    } else if (key == "body_facing_deadzone_deg") {
        s.body_facing_deadzone_deg = Ranged(key, value, 0.0f, 90.0f, s.body_facing_deadzone_deg);
    } else if (key == "body_facing_moving_deg") {
        s.body_facing_moving_deg = Ranged(key, value, 0.0f, 90.0f, s.body_facing_moving_deg);
    } else if (key == "body_facing_ms") {
        s.body_facing_ms = Ranged(key, value, 0.0f, 2000.0f, s.body_facing_ms);
    } else if (key == "body_align") {
        s.body_align = Flag(value);
    } else if (key == "body_lean_side_deg") {
        s.body_lean_side_deg = Ranged(key, value, 0.0f, 35.0f, s.body_lean_side_deg);
    } else if (key == "body_comfort") {
        s.body_comfort = Flag(value);
    } else if (key == "body_posture") {
        if (value == "lean") {
            s.body_posture_lean = true;
        } else if (value == "shift") {
            s.body_posture_lean = false;
        } else {
            LOG_WARNING(Core, "VR body: body_posture={} is not lean or shift; using {}", value,
                        s.body_posture_lean ? "lean" : "shift");
        }
    } else if (key == "body_height_from_head") {
        s.body_height_from_head = Flag(value);
    } else if (key == "body_vertical_deadband_m") {
        s.body_vertical_deadband_m = Ranged(key, value, 0.0f, 0.3f, s.body_vertical_deadband_m);
    } else if (key == "body_duck_lean_deg") {
        s.body_duck_lean_deg = Ranged(key, value, 35.0f, 85.0f, s.body_duck_lean_deg);
    } else if (key == "body_shoulder_reach_deg") {
        s.body_shoulder_reach_deg = Ranged(key, value, 0.0f, 45.0f, s.body_shoulder_reach_deg);
    } else if (key == "body_natural_arms") {
        s.body_natural_arms = Flag(value);
    } else if (key == "body_forearm_pronation_deg") {
        s.body_forearm_pronation_deg =
            Ranged(key, value, 0.0f, 150.0f, s.body_forearm_pronation_deg);
    } else if (key == "body_forearm_supination_deg") {
        s.body_forearm_supination_deg =
            Ranged(key, value, 0.0f, 150.0f, s.body_forearm_supination_deg);
    } else if (key == "body_wrist_twist_max_deg") {
        s.body_wrist_twist_max_deg = Ranged(key, value, 90.0f, 175.0f, s.body_wrist_twist_max_deg);
    } else if (key == "body_align_max_m") {
        s.body_align_max_m = Ranged(key, value, 0.0f, 0.35f, s.body_align_max_m);
    } else if (key == "body_back_m") {
        s.body_back_m = Ranged(key, value, 0.0f, 0.15f, s.body_back_m);
    } else if (key == "body_back_down_m") {
        s.body_back_down_m = Ranged(key, value, 0.0f, 0.20f, s.body_back_down_m);
    } else if (key == "body_align_ms") {
        s.body_align_ms = Ranged(key, value, 0.0f, 500.0f, s.body_align_ms);
    } else if (key == "arm_blend_in_ms") {
        s.arm_blend_in_ms = Ranged(key, value, 0.0f, 2000.0f, s.arm_blend_in_ms);
    } else if (key == "arm_blend_out_ms") {
        s.arm_blend_out_ms = Ranged(key, value, 0.0f, 1000.0f, s.arm_blend_out_ms);
    } else if (key == "arm_anim_gate") {
        s.arm_anim_gate = Flag(value);
    } else if (key == "arm_keep_on_hit") {
        s.arm_keep_on_hit = Flag(value);
    } else if (key == "arm_keep_on_dodge") {
        s.arm_keep_on_dodge = Flag(value);
    } else if (key == "ghost_hands") {
        s.ghost_hands = Flag(value);
    } else if (key == "ghost_alpha") {
        s.ghost_alpha = Ranged(key, value, 0.0f, 1.0f, s.ghost_alpha);
    } else if (key == "weapon_grip_pitch_deg") {
        s.weapon_grip_pitch_deg = Ranged(key, value, -180.0f, 180.0f, s.weapon_grip_pitch_deg);
    } else if (key == "weapon_grip_yaw_deg") {
        s.weapon_grip_yaw_deg = Ranged(key, value, -180.0f, 180.0f, s.weapon_grip_yaw_deg);
    } else if (key == "weapon_grip_roll_deg") {
        s.weapon_grip_roll_deg = Ranged(key, value, -180.0f, 180.0f, s.weapon_grip_roll_deg);
    } else if (key == "grip_adjust") {
        s.grip_adjust = Flag(value);
    } else if (key == "body_haptics") {
        s.body_haptics = Flag(value);
    } else if (key == "body_budget_us") {
        s.body_budget_us = Ranged(key, value, 0.0f, 20000.0f, s.body_budget_us);
        if (s.body_budget_us > 0.0f && s.body_budget_us < 100.0f) {
            LOG_WARNING(Core, "VR body: body_budget_us={} is below 100; using 100", value);
            s.body_budget_us = 100.0f;
        }
    } else if (key.rfind("weapon_grip_", 0) == 0) {
        ParseWeaponGrip(key, value);
    } else {
        return false;
    }
    return true;
}

GripTrim DefaultWeaponGrip() {
    const auto& s = GetBodySettings();
    GripTrim t{};
    t.pitch_deg = s.weapon_grip_pitch_deg;
    t.yaw_deg = s.weapon_grip_yaw_deg;
    t.roll_deg = s.weapon_grip_roll_deg;
    return t;
}

GripTrim WeaponGrip(s32 family, bool* custom) {
    auto& g = Grips();
    {
        std::scoped_lock lock{g.mutex};
        for (u32 i = 0; i < g.count; ++i) {
            if (g.entries[i].family == family) {
                if (custom != nullptr) {
                    *custom = true;
                }
                return g.entries[i].trim;
            }
        }
    }
    if (custom != nullptr) {
        *custom = false;
    }
    return DefaultWeaponGrip();
}

void SetWeaponGrip(s32 family, const GripTrim* trim) {
    if (family < 0) {
        return;
    }
    bool clamped = false;
    GripTrim limited{};
    if (trim != nullptr) {
        limited = LimitGrip(*trim, clamped);
    }
    StoreGrip(family, trim != nullptr ? &limited : nullptr);
    const std::string key = WeaponGripKey(family);
    const std::string value = trim != nullptr ? GripText(limited) : std::string{"default"};
    VR::SaveSetting(key, value);
    LOG_INFO(Core, "VR body: saved {}={} to vr.ini{}", key, value,
             clamped ? " (clamped to the per-weapon limits)" : "");
}

u32 WeaponGripGeneration() {
    return Grips().generation.load(std::memory_order_acquire);
}

std::string WeaponGripKey(s32 family) {
    if (const auto* f = Weapons::FindFamily(family); f != nullptr) {
        return fmt::format("weapon_grip_{}", f->key);
    }
    return fmt::format("weapon_grip_{}", family);
}

std::string WeaponName(s32 family) {
    if (const auto* f = Weapons::FindFamily(family); f != nullptr) {
        return std::string{f->name};
    }
    return fmt::format("weapon family {}", family);
}

void WriteBodyDefaults(std::ostream& out) {
    const BodySettings d{};
    out << "# ---- VR body: the hunter's arms follow your controllers (first person) ----\n"
        << "# 0 = no body hooks at all (the game exactly as without the body)\n"
        << "body=" << (d.body ? 1 : 0) << "\n"
        << "# no_head = your head hidden, torso, legs and arms shown; arms_only = only your hands\n"
        << "# and forearms; hands_only = only your hands (both with what they hold; his shadow\n"
        << "# stays whole); full = nothing hidden\n"
        << "body_mode=" << BodyModeName(d.mode) << "\n"
        << "# 1 = with no_head the head keeps its size so it casts its shadow, and is left out of "
           "the\n"
        << "# first-person view by the renderer instead (0 = shrunk away: no head shadow)\n"
        << "body_head_shadow=" << (d.body_head_shadow ? 1 : 0) << "\n"
        << "# 1 = the hunter's cape is hidden in first person (cinematics, the virtual screen and "
           "third\n"
        << "# person still show it)\n"
        << "hide_cape=" << (d.hide_cape ? 1 : 0) << "\n"
        << "# with no_head / full: 1 = through a hit, a stagger or a dodge (or your head behind "
           "or\n"
        << "# below his eye) the hunter's body near the camera fades out more, its inside never\n"
        << "# drawn (body_cutaway_boost_near_m / _far_m); 0 = the same fade all the time\n"
        << "body_cutaway_boost=" << (d.body_cutaway_boost ? 1 : 0) << "\n"
        << "# with no_head / full: 1 = his upper arms and shoulders fade near the camera like the "
           "rest of\n"
        << "# his body (the forearms and hands never); 0 = the arms whole up to the shoulder\n"
        << "body_cutaway_shoulders=" << (d.body_cutaway_shoulders ? 1 : 0) << "\n"
        << "# 1 = the hunter's arms follow your hands (0 = the game's arms)\n"
        << "arm_ik=" << (d.arm_ik ? 1 : 0) << "\n"
        << "# 1 = both eyes of a stereo pair show your hands at the same instant\n"
        << "arm_pair_sync=" << (d.arm_pair_sync ? 1 : 0) << "\n"
        << "# the torso turns with your head beyond the deadzone (following in torso_follow_ms), "
           "at most\n"
        << "# torso_max_deg from the legs\n"
        << "torso_follow=" << (d.torso_follow ? 1 : 0) << "\n"
        << "torso_deadzone_deg=" << d.torso_deadzone_deg << "\n"
        << "torso_max_deg=" << d.torso_max_deg << "\n"
        << "torso_follow_ms=" << d.torso_follow_ms << "\n"
        << "# 1 = only the turn stick and your real heading turn the hunter, never walking or "
           "rolling:\n"
        << "# he strafes, backpedals and steps in every direction (0 = he turns toward the stick "
           "as in\n"
        << "# the flat game). He turns to your view once you look more than the deadzone away\n"
        << "# (body_facing_moving_deg while you move), easing in body_facing_ms\n"
        << "body_facing=" << (d.body_facing ? 1 : 0) << "\n"
        << "body_facing_deadzone_deg=" << d.body_facing_deadzone_deg << "\n"
        << "body_facing_moving_deg=" << d.body_facing_moving_deg << "\n"
        << "body_facing_ms=" << d.body_facing_ms << "\n"
        << "# the upper body follows your eyes, body_back_m behind them. body_posture=lean: the "
           "trunk\n"
        << "# leans from the waist and keeps its length; height differences within\n"
        << "# body_vertical_deadband_m are left alone (the walk's bob). shift: the upper body "
           "slides\n"
        << "# against the hips (at most body_align_max_m sideways; the waist shears)\n"
        << "body_align=" << (d.body_align ? 1 : 0) << "\n"
        << "body_comfort=" << (d.body_comfort ? 1 : 0) << "\n"
        << "body_lean_side_deg=" << d.body_lean_side_deg << "\n"
        << "body_posture=" << (d.body_posture_lean ? "lean" : "shift") << "\n"
        << "body_vertical_deadband_m=" << d.body_vertical_deadband_m << "\n"
        << "# a duck bends at the hips and comes all the way down: as your eyes go 0.10 .. 0.35 m\n"
        << "# below his, his trunk leans forward up to this (degrees, 35 .. 85) and the height\n"
        << "# deadband fades out (35 = off: the standing limit and deadband everywhere)\n"
        << "body_duck_lean_deg=" << d.body_duck_lean_deg << "\n"
        << "# a controller out of the arm's reach turns his shoulder toward it, at most this much\n"
        << "# (degrees, 0 .. 45; 0 = off)\n"
        << "body_shoulder_reach_deg=" << d.body_shoulder_reach_deg << "\n"
        << "# 1 = natural arms: the forearm turns the hand up to body_forearm_pronation_deg palm\n"
        << "# down / body_forearm_supination_deg palm up before the elbow moves (0 .. 150), the\n"
        << "# forearm's twist follows the hand up to body_wrist_twist_max_deg (90 .. 175), arms\n"
        << "# folded across the chest hang their elbows lower and no elbow goes into his body\n"
        << "# (0 = the arms before 2026-10-08)\n"
        << "body_natural_arms=" << (d.body_natural_arms ? 1 : 0) << "\n"
        << "body_forearm_pronation_deg=" << d.body_forearm_pronation_deg << "\n"
        << "body_forearm_supination_deg=" << d.body_forearm_supination_deg << "\n"
        << "body_wrist_twist_max_deg=" << d.body_wrist_twist_max_deg << "\n"
        << "# 1 = the hunter's height follows your real head over his feet, not the camera's own\n"
        << "# vertical smoothing on stairs and slopes\n"
        << "body_height_from_head=" << (d.body_height_from_head ? 1 : 0) << "\n"
        << "body_align_max_m=" << d.body_align_max_m << "\n"
        << "body_back_m=" << d.body_back_m << "\n"
        << "# looking down the upper body moves further back, up to this much (from 25 deg below\n"
        << "# the horizon, all of it from 60), so the coat under your eyes does not fill the view\n"
        << "body_back_down_m=" << d.body_back_down_m << "\n"
        << "body_align_ms=" << d.body_align_ms << "\n"
        << "# hand-over between your arms and the game's animations (ms)\n"
        << "arm_blend_in_ms=" << d.arm_blend_in_ms << "\n"
        << "arm_blend_out_ms=" << d.arm_blend_out_ms << "\n"
        << "# 1 = the hunter's arms play the game's attacks, dodges, hits, items, doors and "
           "gestures\n"
        << "# (0 = only grabs, viscerals, ladders, event animations and death take them)\n"
        << "arm_anim_gate=" << (d.arm_anim_gate ? 1 : 0) << "\n"
        << "# 1 = your arms stay on the controllers through a hit stagger / a dodge, quickstep or\n"
        << "# backstep (swings and shots still wait for the game's cancel window; 0 = the game's\n"
        << "# animation takes both arms until then)\n"
        << "arm_keep_on_hit=" << (d.arm_keep_on_hit ? 1 : 0) << "\n"
        << "arm_keep_on_dodge=" << (d.arm_keep_on_dodge ? 1 : 0) << "\n"
        << "# faint ghost hands at your controllers while the game moves the hunter's arms (not "
           "drawn yet)\n"
        << "ghost_hands=" << (d.ghost_hands ? 1 : 0) << "\n"
        << "ghost_alpha=" << d.ghost_alpha << "\n"
        << "# weapon grip trim in degrees: pitch + = blade up, yaw + = blade outward, roll + = "
           "grip top inward\n"
        << "weapon_grip_pitch_deg=" << d.weapon_grip_pitch_deg << "\n"
        << "weapon_grip_yaw_deg=" << d.weapon_grip_yaw_deg << "\n"
        << "weapon_grip_roll_deg=" << d.weapon_grip_roll_deg << "\n"
        << "# 1 = hold A+B for 1 s in game: the weapon freezes, line your controller up with it "
           "and\n"
        << "# release (right trigger while holding = back to the default). Saved per weapon as\n"
        << "# weapon_grip_<weapon>=pitch,yaw,roll,x_cm,y_cm,z_cm (e.g. weapon_grip_saw_cleaver)\n"
        << "grip_adjust=" << (d.grip_adjust ? 1 : 0) << "\n"
        << "# 1 = a tick when your hand reaches past the hunter's arm, a double buzz if the body "
           "turns off\n"
        << "body_haptics=" << (d.body_haptics ? 1 : 0) << "\n"
        << "# the body's time budget per frame (microseconds, average over 5 s; 0 = no limit): "
           "twice\n"
        << "# over it in a row turns the body off for the session\n"
        << "body_budget_us=" << d.body_budget_us << "\n";
}

} // namespace VR::Body
