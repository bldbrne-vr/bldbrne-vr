// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

// Knocking on a resident's door in Yharnam without the hunter's animation, in VR, while you keep
// moving (vr.ini vr_interactions, default 1, the one setting for chests, doors, lamps and knocks;
// the tester, 2026-11-08: "it should stay in headset, not animate the hunter - retain control, and
// just trigger the interaction"). Pure; unit-tested in tests/test_door_knock.cpp. The game side is
// core/vr/bloodborne_lamp_light.cpp (the event scripts below, served from memory at boot, with the
// lamps') and core/vr/bloodborne_cinematic.cpp (KeepInVr).
//
// A knock is not an object action and not a talk script's: the map's event script runs it, the
// same three events in Cathedral Ward (event/m24_00.emevd 12400840) and Central Yharnam
// (event/m24_01.emevd 12410750, 12410850), once the Knock button (ActionButtonParam 6030) is
// pressed at the resident's door:
//   2004[41] [10000, 0, door, 210]  a short warp: the player to the door's knock point, turned to it
//                                   (the door filled in by the event's parameter, at byte 8)
//   2003[18] [10000, 101320, 0]     the knock on the player (a000_101320, 7.7 s): flat on the screen
//   1001[01] [25], 1001[01] [20]    wait 45 frames
//   2003[02] [flag, 1]              the flag the resident's talk script answers
// Nothing waits on the animation: the talk starts from the flag. Its sound is the animation's own
// (the hunter's TAE, chr/c0000.anibnd a00.tae: a character sound, type 1 id 101320, at 0.53 and
// 0.80 s, on the hunter). So the warp becomes the game's own "play a sound at an entity"
// (2010[02] [10000, 1, 101320], as the scripts use it 31 times with type 1; the obsolete door
// parameter removed from the event), the knock a wait of 0 frames, and the 45
// frames stay: B knocks at once with its sound (once: no free instruction for the second), no
// warp, no turn, no animation, and the resident answers as before. A warp whose parameters are
// not the expected one is only pointed at no one (entity 1: no warp, no sound); a knock without
// the warp before it only becomes the wait.
namespace VR::DoorKnock {

inline constexpr std::int32_t Player = 10000; // the main player in the event scripts
inline constexpr std::int32_t Nobody = 1;     // no such entity: the instruction moves no one
inline constexpr std::int32_t Knock = 101320;
inline constexpr std::int32_t KnockSoundType = 1;   // a character sound (the TAE's type)
inline constexpr std::int32_t KnockSound = Knock;   // c101320, what the knock played
inline constexpr std::uint32_t WarpBank = 2004, WarpIndex = 41;
inline constexpr std::uint32_t AnimBank = 2003, AnimIndex = 18;
inline constexpr std::uint32_t SoundBank = 2010, SoundIndex = 2;
inline constexpr std::uint32_t WaitBank = 1001, WaitIndex = 1;
inline constexpr const char* Scripts[] = {"/app0/dvdroot_ps4/event/m24_00_00_00.emevd.dcx",
                                          "/app0/dvdroot_ps4/event/m24_01_00_00.emevd.dcx"};

// Third person with vr_interactions: the knock's events were not rewritten at boot (first person
// only, as lamps), so the hunter knocks; you see him from behind, in VR, never on the screen.
constexpr bool KeepInVr(bool setting, bool first_person, std::int32_t clip) {
    return setting && !first_person && clip >= 0 && clip % 1000000 == Knock;
}

struct Result {
    bool understood{false}; // a 64-bit EVD whose tables lie inside it
    int sounded{0};         // warps before a knock turned into the knock's sound on the hunter
    int unwarped{0};        // warps before a knock only pointed at no one (unexpected parameters)
    int unknocked{0};       // knocks turned into a wait of 0 frames
};

// Every knock of the player in `evd` (an event script as the game loads it: "EVD\0", 64-bit
// offsets) changed as above, in place. Anything it does not understand is left exactly as it is
// (understood false, nothing written).
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
    // empty one's offset is not read (an event with no parameters writes 0xFFFFFFFF there).
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
    // The header (core/vr/lamp_light.h): "EVD\0", then from 0x10 its counts and offsets (8 bytes
    // each): events at [0] / [1], instructions at [3], parameters at [9], the arguments at [13].
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
    for (std::uint64_t i = 0; i < events_n; ++i) {
        const std::uint64_t ev = events + i * EventSize;
        std::uint64_t start = 0;
        if (!table(instructions, u64_at(ev + 16), u64_at(ev + 8), Entry, start) ||
            !table(parameters, u64_at(ev + 32), u64_at(ev + 24), Entry, start)) {
            return result;
        }
    }
    result.understood = true;
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
        std::uint64_t params_n = u64_at(ev + 24);
        const std::uint64_t params = parameters + u64_at(ev + 32);
        // The event's parameters on instruction k: how many, and whether all are `bytes` bytes
        // into byte `target`.
        const auto params_on = [&](std::uint64_t k, std::uint64_t target, int& n) {
            bool expected = true;
            n = 0;
            for (std::uint64_t p = 0; p < params_n; ++p) {
                const std::uint64_t q = params + p * Entry;
                if (u64_at(q) == k) {
                    ++n;
                    expected = expected && u64_at(q + 8) == target && s32_at(q + 24) == 4;
                }
            }
            return expected;
        };
        for (std::uint64_t k = 0; k < count; ++k) {
            const std::uint64_t e = first + k * Entry;
            std::uint64_t a = 0;
            int on_knock = 0;
            if (u32_at(e) != AnimBank || u32_at(e + 4) != AnimIndex || !arguments(e, 8, a) ||
                s32_at(a) != Player || s32_at(a + 4) != Knock ||
                !params_on(k, ~std::uint64_t{0}, on_knock) || on_knock != 0) {
                continue; // not the player's knock, or one an event parameter fills in
            }
            // The knock: a wait of 0 frames (no animation, nothing to wait for).
            put32(e, static_cast<std::int32_t>(WaitBank));
            put32(e + 4, static_cast<std::int32_t>(WaitIndex));
            put64(e + 8, 4);
            put32(a, 0);
            ++result.unknocked;
            // The warp before it: the knock's sound on the hunter, or no one.
            const std::uint64_t w = e - Entry;
            std::uint64_t wa = 0;
            int on_warp = 0;
            if (k == 0 || u32_at(w) != WarpBank || u32_at(w + 4) != WarpIndex ||
                !arguments(w, 16, wa) || s32_at(wa) != Player) {
                continue;
            }
            if (!params_on(k - 1, 8, on_warp) || on_warp > 1) {
                put32(wa, Nobody); // parameters it does not know: no warp
                ++result.unwarped;
                continue;
            }
            put32(w, static_cast<std::int32_t>(SoundBank));
            put32(w + 4, static_cast<std::int32_t>(SoundIndex));
            put64(w + 8, 12);
            put32(wa, Player); // the hunter, as the animation played it
            put32(wa + 4, KnockSoundType);
            put32(wa + 8, KnockSound);
            put32(wa + 12, 0);
            // The sound has no door argument. Remove that substitution instead of pointing
            // it beyond the sound's 12 argument bytes. Compact only this event's records;
            // unused trailing storage remains, so every other table/event offset stays valid.
            std::uint64_t kept = 0;
            for (std::uint64_t p = 0; p < params_n; ++p) {
                if (u64_at(params + p * Entry) == k - 1) continue;
                if (kept != p) {
                    std::memmove(evd.data() + params + kept * Entry,
                                 evd.data() + params + p * Entry, Entry);
                }
                ++kept;
            }
            params_n = kept;
            put64(ev + 24, params_n);
            ++result.sounded;
        }
    }
    return result;
}

} // namespace VR::DoorKnock
