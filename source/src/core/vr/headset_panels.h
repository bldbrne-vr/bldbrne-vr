// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Headsets' own panel pixels per eye, which no OpenXR call reports: the runtime's recommended
// size includes its supersampling (PS VR2 on SteamVR at 100% asks 3400x3468 of a 2000x2040
// panel). Header-only and dependency-free: vr.ini eye_size=panel (vr_eye_scale.h) and the
// launcher's suggested scaling read it.

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace VR::Headsets {

struct Headset {
    std::string_view id; // the launcher profile's "headset"
    std::string_view model;
    uint32_t width{};
    uint32_t height{};
};

// Manufacturers' per-eye panel resolutions.
inline constexpr std::array<Headset, 21> All{{
    {"quest2", "Quest 2", 1832, 1920},
    {"quest3", "Quest 3", 2064, 2208},
    {"quest3s", "Quest 3S", 1832, 1920},
    {"questpro", "Quest Pro", 1800, 1920},
    {"quest", "Quest", 1440, 1600},
    {"psvr2", "PS VR2", 2000, 2040},
    {"index", "Valve Index", 1440, 1600},
    {"steamframe", "Steam Frame", 2160, 2160},
    {"pico4", "Pico 4 / 4 Ultra", 2160, 2160},
    {"vivepro2", "Vive Pro 2", 2448, 2448},
    {"vivefocus", "Vive Focus 3 / Vision", 2448, 2448},
    {"vivexrelite", "Vive XR Elite", 1920, 1920},
    {"vivepro", "Vive Pro", 1440, 1600},
    {"vive", "Vive", 1080, 1200},
    {"reverb", "HP Reverb G2", 2160, 2160},
    {"beyond", "Bigscreen Beyond", 2560, 2560},
    {"crystal", "Pimax Crystal", 2880, 2880},
    {"aero", "Varjo Aero", 2880, 2720},
    {"galaxyxr", "Galaxy XR", 3552, 3840},
    {"rifts", "Rift S", 1280, 1440},
    {"rift", "Rift", 1080, 1200},
}};

inline const Headset* ById(std::string_view id) {
    for (const auto& h : All) {
        if (h.id == id) {
            return &h;
        }
    }
    return nullptr;
}

// The name as words: split at anything but letters and digits and between letters and digits
// ("Quest3S" = quest 3 s), lower case.
inline std::vector<std::string> NameWords(std::string_view name) {
    std::vector<std::string> words;
    std::string word;
    int kind = 0; // 1 letter, 2 digit
    for (const char ch : name) {
        const unsigned char u = static_cast<unsigned char>(ch);
        const int k = (u >= '0' && u <= '9') ? 2 : ((u | 0x20) >= 'a' && (u | 0x20) <= 'z') ? 1 : 0;
        if (k == 0 || (k != kind && !word.empty())) {
            if (!word.empty()) {
                words.push_back(word);
                word.clear();
            }
        }
        if (k != 0) {
            word += static_cast<char>(k == 1 ? (u | 0x20) : u);
        }
        kind = k;
    }
    if (!word.empty()) {
        words.push_back(word);
    }
    return words;
}

// The headset from the runtime's system name (xrGetSystemProperties::systemName: "Meta Quest 3",
// "Oculus Quest2", "SteamVR/OpenXR : playstation_vr2"). Whole words only. nullptr: a name that
// does not say the model, e.g. SteamVR's "lighthouse" for Index, Vive and Beyond alike.
inline const Headset* FromSystemName(std::string_view system_name) {
    const std::vector<std::string> w = NameWords(system_name);
    const auto next = [&w](size_t i, size_t n) -> std::string_view {
        return i + n < w.size() ? std::string_view{w[i + n]} : std::string_view{};
    };
    for (size_t i = 0; i < w.size(); ++i) {
        const std::string_view a = next(i, 1);
        if (w[i] == "quest") {
            if (a == "3") {
                return ById(next(i, 2) == "s" ? "quest3s" : "quest3");
            }
            if (a == "2") {
                return ById("quest2");
            }
            if (a == "pro") {
                return ById("questpro");
            }
            if (a.empty() || a == "1") {
                return ById("quest");
            }
            return nullptr; // a later Quest: not known yet
        }
        if (w[i] == "rift") {
            return ById(a == "s" ? "rifts" : "rift");
        }
        if (((w[i] == "playstation" || w[i] == "ps") && a == "vr" && next(i, 2) == "2") ||
            (w[i] == "psvr" && a == "2")) {
            return ById("psvr2");
        }
        if (w[i] == "index") {
            return ById("index");
        }
        if (w[i] == "frame") {
            return ById("steamframe");
        }
        if (w[i] == "pico" && a == "4") {
            return ById("pico4");
        }
        if (w[i] == "vive") {
            if (a == "pro") {
                return ById(next(i, 2) == "2" ? "vivepro2" : "vivepro");
            }
            if (a == "focus") {
                return ById("vivefocus");
            }
            if (a == "xr") {
                return ById("vivexrelite");
            }
            return a.empty() ? ById("vive") : nullptr;
        }
        if (w[i] == "reverb") {
            return ById("reverb");
        }
        if (w[i] == "beyond") {
            return ById("beyond");
        }
        if (w[i] == "crystal") {
            return ById("crystal");
        }
        if (w[i] == "aero") {
            return ById("aero");
        }
        if (w[i] == "galaxy" && a == "xr") {
            return ById("galaxyxr");
        }
    }
    return nullptr;
}

} // namespace VR::Headsets
