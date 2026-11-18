// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

// The new graphics a graphics card gets where the player has not chosen (2026-11-17,
// for 0.9.6): "12gb cards or more: shadows on (low), mist on (low), water reflections on. 16 gb
// cards: wet streets on, water reflections on, bounce on. and when MIST is on - then moon glow
// (godrays) is on. Moon shadows not on for anyone." Everyone has the filmic highlights, the tone
// filter at 30%, the adaptive eyes and the parallax: those are the settings' own defaults.
// The ambient occlusion and the lamps' glow in the air were everyone's at first (his pick of the
// open points); the same day he said: "we can explicitely disable all our new graphics on 8gb",
// and of where that line is: "UNDER 10GB!!". So a card under 10 GB has none of the graphics that
// need DLSS or FSR, and a 10 or 11 GB card has the ambient occlusion and the lamps' glow alone.
// A key in vr.ini is the player's choice and wins; a key not there is "by the card" (the in-game
// page's Defaults and the launcher's "Game default" remove the key).
namespace VR::CardTier {

inline constexpr char MoonGlowRequired[] = "On (required by Fog)";

enum class Tier : u8 { Base, Ten, Twelve, Sixteen };

// Allow half a GiB below the nominal 10 and 12 GB tiers. The full 16 GB preset starts at
// 13.8 GiB of reported dedicated memory, rounded up to the next whole byte.
inline constexpr u64 TenBytes = 19ull << 29;     // 9.5 GiB
inline constexpr u64 TwelveBytes = 23ull << 29;  // 11.5 GiB
inline constexpr u64 SixteenBytes = ((138ull << 30) + 9) / 10;

inline constexpr Tier Of(u64 dedicated_bytes) {
    return dedicated_bytes >= SixteenBytes  ? Tier::Sixteen
           : dedicated_bytes >= TwelveBytes ? Tier::Twelve
           : dedicated_bytes >= TenBytes    ? Tier::Ten
                                            : Tier::Base;
}

struct Defaults {
    bool lamp_shadows;      // light_shadows (at Low: light_shadow_detail stays low for everyone)
    bool fog_banks;         // fog_banks: the game's lingering fog taken over, the ground mist
    bool moon_glow;         // moon_shafts: the moon's glow, haze and rays (always on with the fog)
    int moon_glow_quality;  // moon_shafts_quality: 0 low (6 steps), 1 medium (10)
    bool water_reflections; // water_reflections
    bool wet_streets;       // wet_streets
    bool bounce_light;      // bounce_light
    bool ambient_occlusion; // ambient_occlusion
    bool lamp_glow;         // fog_light: lamps, candles and fires light the air round them
    constexpr bool operator==(const Defaults&) const = default;
};

inline constexpr Defaults For(Tier tier) {
    switch (tier) {
    case Tier::Sixteen:
        return {true, true, true, 1, true, true, true, true, true};
    case Tier::Twelve:
        return {true, true, true, 0, true, false, false, true, true};
    case Tier::Ten:
        return {false, false, false, 0, false, false, false, true, true};
    case Tier::Base:
        break;
    }
    return {false, false, false, 0, false, false, false, false, false};
}

} // namespace VR::CardTier
