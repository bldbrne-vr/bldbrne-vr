// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <ostream>
#include <stdexcept>
#include <fmt/format.h>

#include "core/vr/gun_settings.h"

namespace VR::Gun {

namespace {

bool Flag(const std::string& value) {
    return value == "1" || value == "true";
}

const char* BusyName(Busy b) {
    switch (b) {
    case Busy::Buffer:
        return "buffer";
    case Busy::Vanilla:
        return "vanilla";
    case Busy::Drop:
        return "drop";
    }
    return "?";
}

} // Anonymous namespace

GunSettings& GetGunSettings() {
    static GunSettings settings;
    return settings;
}

bool ParseGunSetting(const std::string& key, const std::string& value) {
    auto& s = GetGunSettings();
    const auto f = [&value] { return std::stof(value); };
    if (key == "gun_wrist_reload") {
        s.wrist_reload = Flag(value);
    } else if (key == "gun_aim") {
        s.aim = Flag(value);
    } else if (key == "gun_fire") {
        if (value != "direct" && value != "vanilla") {
            throw std::invalid_argument{"direct or vanilla expected"};
        }
        s.fire_direct = value == "direct";
    } else if (key == "gun_assist") {
        s.assist = std::clamp(f(), 0.0f, 0.6f);
    } else if (key == "gun_assist_deg") {
        s.assist_deg = std::clamp(f(), 0.0f, 12.0f);
    } else if (key == "gun_assist_unlocked") {
        s.assist_unlocked = Flag(value);
    } else if (key == "gun_origin") {
        if (value != "barrel" && value != "palm") {
            throw std::invalid_argument{"barrel or palm expected"};
        }
        s.origin_barrel = value == "barrel";
    } else if (key == "gun_busy") {
        if (value == "buffer") {
            s.busy = Busy::Buffer;
        } else if (value == "vanilla") {
            s.busy = Busy::Vanilla;
        } else if (value == "drop") {
            s.busy = Busy::Drop;
        } else {
            throw std::invalid_argument{"buffer, vanilla or drop expected"};
        }
    } else if (key == "gun_buffer_ms") {
        s.buffer_ms = std::clamp(f(), 0.0f, 1000.0f);
    } else if (key == "gun_rate_scale") {
        s.rate_scale = std::clamp(f(), 0.6f, 1.0f);
    } else if (key == "gun_recoil_visual") {
        s.recoil_visual = std::clamp(f(), 0.0f, 1.5f);
    } else if (key == "gun_haptics") {
        s.haptics = Flag(value);
    } else if (key == "gun_recoil_gain") {
        s.recoil_gain = std::clamp(f(), 0.0f, 1.5f);
    } else if (key == "gun_regen_pause") {
        s.regen_pause = Flag(value);
    } else {
        return false;
    }
    return true;
}

void WriteGunDefaults(std::ostream& out) {
    const GunSettings d{};
    out << "# ---- The gun (left trigger) ----\n"
        << "# 1 = flick the left wrist after each shot to reload (VR Settings > Weapons)\n"
        << "gun_wrist_reload=" << (d.wrist_reload ? 1 : 0) << "\n"
        << "# 1 = shots leave along the gun's visible barrel, wherever the hunter faces; 0 = the "
           "game's aim\n"
        << "gun_aim=" << (d.aim ? 1 : 0) << "\n"
        << "# direct = the bang at the trigger pull (pistols, Blunderbuss, Ludwig's Rifle); "
           "vanilla = the game's\n# own L2 with its draw animation (0.3-0.5 s)\n"
        << "gun_fire=" << (d.fire_direct ? "direct" : "vanilla") << "\n"
        << "# locked on: share of the aiming error removed near the target (at most ~1 degree, "
           "0 = none),\n# within this many degrees of it; 1 = also without lock-on\n"
        << "gun_assist=" << d.assist << "\n"
        << "gun_assist_deg=" << d.assist_deg << "\n"
        << "gun_assist_unlocked=" << (d.assist_unlocked ? 1 : 0) << "\n"
        << "# barrel = the bullet starts on the barrel's line; palm = where the game starts it\n"
        << "gun_origin=" << (d.origin_barrel ? "barrel" : "palm") << "\n"
        << "# a pull during a dodge or a hit waits gun_buffer_ms for the arm, then: vanilla = the "
           "game's own\n# L2 (it fires as soon as its dodge allows), buffer / drop = nothing\n"
        << "gun_busy=" << BusyName(d.busy) << "\n"
        << "gun_buffer_ms=" << d.buffer_ms << "\n"
        << "# the gun's fire rate (1 = the game's own; 0.6 .. 1)\n"
        << "gun_rate_scale=" << d.rate_scale << "\n"
        << "# the hunter's hand kicks up at the bang (0 = off .. 1.5)\n"
        << "gun_recoil_visual=" << d.recoil_visual << "\n"
        << "# trigger break, recoil thump, empty click; recoil strength (0 .. 1.5)\n"
        << "gun_haptics=" << (d.haptics ? 1 : 0) << "\n"
        << "gun_recoil_gain=" << d.recoil_gain << "\n"
        << "# no stamina regen just after a shot, as the game's own shot\n"
        << "gun_regen_pause=" << (d.regen_pause ? 1 : 0) << "\n";
}

std::string GunSettingsText() {
    const auto& s = GetGunSettings();
    return fmt::format("gun_aim={} gun_fire={} gun_assist={:.2f} gun_assist_deg={:.1f} "
                       "gun_assist_unlocked={} gun_origin={} gun_busy={} gun_buffer_ms={:.0f} "
                       "gun_rate_scale={:.2f} gun_recoil_visual={:.2f} gun_haptics={} "
                       "gun_recoil_gain={:.2f} gun_regen_pause={} gun_wrist_reload={}",
                       s.aim ? 1 : 0, s.fire_direct ? "direct" : "vanilla", s.assist, s.assist_deg,
                       s.assist_unlocked ? 1 : 0, s.origin_barrel ? "barrel" : "palm",
                       BusyName(s.busy), s.buffer_ms, s.rate_scale, s.recoil_visual,
                       s.haptics ? 1 : 0, s.recoil_gain, s.regen_pause ? 1 : 0, s.wrist_reload ? 1 : 0);
}

} // namespace VR::Gun
