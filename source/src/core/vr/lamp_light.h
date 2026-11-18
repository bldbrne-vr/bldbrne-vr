// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

// Lighting a lamp at once, without the hunter's animation, while you keep moving (vr.ini
// vr_interactions, default 1; the tester, 2026-11-06: "the lamp lights, but we do not wait at all,
// we retain full control while it happens", "Always instant"). Pure; unit-tested in
// tests/test_lamp_light.cpp. The game side is core/vr/bloodborne_lamp_light.cpp: the event scripts
// below, served from memory at boot.
//
// A lamp is not an object action (no ObjActParam row plays a lamp animation; chests and doors
// are, core/vr/chest_open.h). The shared event script (event/common.emevd, event 7100; the
// Chalice dungeons' copy is event/m29.emevd 12907010) lights one:
//   2004[14] [10000, lamp, 101170, 0]  turn the player to the lamp, playing the lighting animation
//                                      (the lamp filled in by the event's parameter, at byte 4)
//   1001[01] [32]                      wait 32 frames
//   2003[52] [lamp]                    light it (its flags, its flame)
// Nothing waits on the animation (the flags 3466-3470 that common event 9500 sets while it plays
// are read by no event, talk script or the hunter's behaviour script), but its sound is the
// animation's own (the mock's sound trace: type 5, id 22821, as the lamp lights). So the first
// instruction becomes the game's own "play a sound at an entity" (2010[02] [lamp, 5, 22821], as
// the scripts use it 134 times; its parameter moved to byte 0, so the sound comes from the lamp)
// and the wait becomes 0 frames: B lights the lamp at once with its sound, no turn, no animation.
// An instruction whose parameters are not the expected one is only pointed at no one (entity 1:
// no turn, no animation, no sound) and its wait kept. The Hunter's Dream from a lamp (event 7200,
// the kneel 101160) is left to the game: it stays on the screen (the tester: "Rest/travel should
// always be 3rd person").
namespace VR::LampLight {

inline constexpr std::int32_t Player = 10000; // the main player in the event scripts
inline constexpr std::int32_t Nobody = 1;     // no such entity: the instruction moves no one
inline constexpr std::int32_t Lighting = 101170;
inline constexpr std::int32_t SoundType = 5;          // the scripts' "s" sounds (the trace's type)
inline constexpr std::int32_t LightingSound = 22821;  // what the lighting animation played
inline constexpr std::uint32_t TurnBank = 2004, TurnIndex = 14;
inline constexpr std::uint32_t SoundBank = 2010, SoundIndex = 2;
inline constexpr std::uint32_t WaitBank = 1001, WaitIndex = 1;
inline constexpr std::array<const char*, 2> Scripts{"/app0/dvdroot_ps4/event/common.emevd.dcx",
                                                    "/app0/dvdroot_ps4/event/m29.emevd.dcx"};

struct Result {
    bool understood{false}; // a 64-bit EVD whose tables lie inside it
    int sounded{0};         // lighting instructions turned into the lamp's sound
    int silenced{0};        // lighting instructions only pointed at no one
    int unwaited{0};        // the waits after them set to 0 frames
};

// Every lighting instruction of the player in `evd` (an event script as the game loads it:
// "EVD\0", 64-bit offsets) changed as above, in place. Anything it does not understand is left
// exactly as it is (understood false, nothing written).
inline Result Rewrite(std::span<std::uint8_t> evd) {
    Result result;
    const std::uint64_t size = evd.size();
    const auto u32_at = [&](std::uint64_t o) {
        std::uint32_t v;
        std::memcpy(&v, evd.data() + o, 4);
        return v;
    };
    const auto s32_at = [&](std::uint64_t o) { return static_cast<std::int32_t>(u32_at(o)); };
    const auto u64_at = [&](std::uint64_t o) {
        std::uint64_t v;
        std::memcpy(&v, evd.data() + o, 8);
        return v;
    };
    const auto put32 = [&](std::uint64_t o, std::int32_t v) { std::memcpy(evd.data() + o, &v, 4); };
    const auto put64 = [&](std::uint64_t o, std::uint64_t v) { std::memcpy(evd.data() + o, &v, 8); };
    // `count` entries of `entry` bytes from `base` + `offset`, inside the file: their start. An
    // empty one's offset is not read (the game's files write 0xFFFFFFFF there: an event with no
    // parameters).
    const auto table = [size](std::uint64_t base, std::uint64_t offset, std::uint64_t count,
                              std::uint64_t entry, std::uint64_t& start) {
        if (count == 0) {
            return true;
        }
        if (base > size || offset > size - base || count > (size - base - offset) / entry) {
            return false;
        }
        start = base + offset;
        return true;
    };
    // The header: "EVD\0", then from 0x10 its counts and offsets (8 bytes each): events at [0] /
    // [1], instructions at [3], parameters at [9], the arguments' block at [13].
    constexpr std::uint64_t Header = 0x10 + 14 * 8, EventSize = 0x30, Entry = 0x20;
    if (size < Header || std::memcmp(evd.data(), "EVD\0", 4) != 0) {
        return result;
    }
    const std::uint64_t events_n = u64_at(0x10), events_o = u64_at(0x18),
                        instructions = u64_at(0x10 + 3 * 8), parameters = u64_at(0x10 + 9 * 8),
                        args = u64_at(0x10 + 13 * 8);
    std::uint64_t events = 0;
    if (!table(events_o, 0, events_n, EventSize, events) || instructions > size ||
        parameters > size || args > size) {
        return result;
    }
    // Every event's instructions and parameters inside the file before anything is written.
    for (std::uint64_t i = 0; i < events_n; ++i) {
        const std::uint64_t ev = events + i * EventSize;
        std::uint64_t start = 0;
        if (!table(instructions, u64_at(ev + 16), u64_at(ev + 8), Entry, start) ||
            !table(parameters, u64_at(ev + 32), u64_at(ev + 24), Entry, start)) {
            return result;
        }
    }
    result.understood = true;
    // An instruction's arguments: their start when `length` bytes of them lie inside the file.
    const auto arguments = [&](std::uint64_t entry, std::uint64_t length, std::uint64_t& at) {
        const std::uint64_t offset = u64_at(entry + 16);
        if (u64_at(entry + 8) < length || offset > size - args || size - args - offset < length) {
            return false;
        }
        at = args + offset;
        return true;
    };
    for (std::uint64_t i = 0; i < events_n; ++i) {
        const std::uint64_t ev = events + i * EventSize;
        const std::uint64_t count = u64_at(ev + 8), first = instructions + u64_at(ev + 16);
        const std::uint64_t params_n = u64_at(ev + 24), params = parameters + u64_at(ev + 32);
        // The event's parameters on instruction k: all the expected one (the lamp, 4 bytes into
        // byte 4), and whether there is any.
        const auto params_on = [&](std::uint64_t k, bool& any) {
            bool expected = true;
            any = false;
            for (std::uint64_t p = 0; p < params_n; ++p) {
                const std::uint64_t q = params + p * Entry;
                if (u64_at(q) == k) {
                    any = true;
                    expected = expected && u64_at(q + 8) == 4 && s32_at(q + 24) == 4;
                }
            }
            return expected;
        };
        for (std::uint64_t k = 0; k < count; ++k) {
            const std::uint64_t e = first + k * Entry;
            std::uint64_t a = 0;
            if (u32_at(e) != TurnBank || u32_at(e + 4) != TurnIndex || !arguments(e, 16, a) ||
                s32_at(a) != Player || s32_at(a + 8) != Lighting) {
                continue;
            }
            bool any = false;
            if (!params_on(k, any)) {
                put32(a, Nobody); // parameters it does not know: no turn, no animation
                ++result.silenced;
                continue;
            }
            put32(e, static_cast<std::int32_t>(SoundBank));
            put32(e + 4, static_cast<std::int32_t>(SoundIndex));
            put64(e + 8, 12);
            put32(a, s32_at(a + 4)); // the lamp (or its parameter's placeholder)
            put32(a + 4, SoundType);
            put32(a + 8, LightingSound);
            put32(a + 12, 0);
            for (std::uint64_t p = 0; p < params_n; ++p) {
                if (u64_at(params + p * Entry) == k) {
                    put64(params + p * Entry + 8, 0); // the lamp now fills the sound's anchor
                }
            }
            ++result.sounded;
            // The wait that gave the animation its time: none.
            std::uint64_t w = 0;
            bool waited = false;
            if (k + 1 < count && u32_at(e + Entry) == WaitBank &&
                u32_at(e + Entry + 4) == WaitIndex && u64_at(e + Entry + 8) == 4 &&
                arguments(e + Entry, 4, w) && params_on(k + 1, waited) && !waited) {
                put32(w, 0);
                ++result.unwaited;
            }
        }
    }
    return result;
}

} // namespace VR::LampLight
