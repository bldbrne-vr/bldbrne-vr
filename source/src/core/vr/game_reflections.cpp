// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstring>
#include <span>

#include "common/logging/log.h"
#include "core/vr/game_reflections.h"
#include "core/vr/hook_util.h"
#include "core/vr/vr_session.h"

namespace VR {
namespace {
// Where the switch is, as LanternEngine's patch list found it ("Enable Screen Space Reflections",
// Lantern Engine by Awaken, github.com/Awakenchan/LanternEngine): image offsets of the 1.09
// eboot, the game's bytes and ours.
struct Site {
    const char* name;
    u64 rva;
    std::span<const u8> game;
    std::span<const u8> ours;
};
// The render settings' constructor stores 1 at +0x2977; it stores it at +0x2974 instead, the
// reflections' own flag.
constexpr std::array<u8, 8> FlagGame{0x41, 0xc6, 0x85, 0x77, 0x29, 0x00, 0x00, 0x01};
constexpr std::array<u8, 8> FlagOurs{0x41, 0xc6, 0x85, 0x74, 0x29, 0x00, 0x00, 0x01};
// The frame's check that skips the pass: a je, made a two-byte nop.
constexpr std::array<u8, 2> SkipGame{0x74, 0x53};
constexpr std::array<u8, 2> SkipOurs{0x66, 0x90};
// The pass's target at half the screen in each direction (two shr): full size.
constexpr std::array<u8, 4> HalfGame{0xd1, 0xee, 0xd1, 0xea};
constexpr std::array<u8, 4> HalfOurs{0x0f, 0x1f, 0x40, 0x00};
// Two of its constants (-1 and 0.01: -100 and 1), a register it reads (xmm1: xmm0), and a scale
// it loads (rip-relative) from a free dword set to 2.
constexpr std::array<u8, 8> RangeGame{0x00, 0x00, 0x80, 0xbf, 0x0a, 0xd7, 0x23, 0x3c};
constexpr std::array<u8, 8> RangeOurs{0x00, 0x00, 0xc8, 0xc2, 0x00, 0x00, 0x80, 0x3f};
constexpr std::array<u8, 4> MoveGame{0xc5, 0xf8, 0x28, 0xd9};
constexpr std::array<u8, 4> MoveOurs{0xc5, 0xf8, 0x28, 0xd8};
constexpr std::array<u8, 4> TwoGame{0x00, 0x00, 0x00, 0x00};
constexpr std::array<u8, 4> TwoOurs{0x00, 0x00, 0x00, 0x40};
constexpr std::array<u8, 4> LoadGame{0xbf, 0xf7, 0x9f, 0x03};
constexpr std::array<u8, 4> LoadOurs{0x23, 0x26, 0x81, 0x02};

constexpr std::array<Site, 7> Sites{{
    {"flag", 0x22c4fd0, FlagGame, FlagOurs},
    {"skip", 0x22c3d2f, SkipGame, SkipOurs},
    {"half size", 0x22c428d, HalfGame, HalfOurs},
    {"range", 0x4929c8c, RangeGame, RangeOurs},
    {"register", 0x22b8d80, MoveGame, MoveOurs},
    {"scale", 0x36bc008, TwoGame, TwoOurs},
    {"scale load", 0xea99e1, LoadGame, LoadOurs},
}};
} // namespace

void ReflectionsEbootLoaded(VAddr address, u64 size) {
    // vr.ini wet_streets (2026-11-11: "the wetness should be a flag in settings", off
    // by default): the game makes the pass's targets once, as its renderer starts, so they follow
    // the setting as it is at the start.
    if (!GetSettings().wet_streets) {
        return;
    }
    u8* const base = reinterpret_cast<u8*>(address);
    for (const Site& site : Sites) {
        if (site.rva + site.game.size() > size ||
            std::memcmp(base + site.rva, site.game.data(), site.game.size()) != 0) {
            LOG_WARNING(Core, "VR reflections: OFF: the game's bytes differ at {} ({:#x})",
                        site.name, site.rva);
            return;
        }
    }
    for (const Site& site : Sites) {
        if (!Hook::MakeWritable(base + site.rva, site.ours.size())) {
            LOG_WARNING(Core, "VR reflections: OFF: {} ({:#x}) is not writable", site.name,
                        site.rva);
            return; // nothing written yet
        }
    }
    for (const Site& site : Sites) {
        u8* const at = base + site.rva;
        std::memcpy(at, site.ours.data(), site.ours.size());
        Hook::FlushCode(at, site.ours.size());
    }
    LOG_INFO(Core, "VR reflections: the game's screen-space reflections on");
}
} // namespace VR
