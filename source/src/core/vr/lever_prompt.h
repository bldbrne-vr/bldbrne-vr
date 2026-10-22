// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>

// Which lever instance has its normal prompt replaced by the hand pull, decided per instance at
// runtime (header-only, pure; tests/test_physical_lever.cpp). The game's selection detour
// (bloodborne_levers.cpp) keeps one verdict for the action row it last saw selected: the native
// gate compares the row and its object handle, hides the prompt only when `bound` is set, and
// asks the host to classify again on a different row/handle, a legacy serial change, a prompt
// change (a refused pull, a hold, the XR side's readiness), or when `recheck` selection passes run
// out. A row id is never enough: generic rows (100: the game's sample lever) are placed with
// models that have no rig.
namespace VR::Lever {

// Read and written by the native selection gate: keep the layout exactly as the offsets say.
struct PromptVerdict {
    std::uint64_t row{};      // +0x00 the ObjAct row (guest address) this verdict is for
    std::uint32_t handle{};   // +0x08 its object handle (row +0x58) when classified
    std::uint16_t recheck{};  // +0x0C selection passes left before the host classifies again
    std::uint8_t bound{};     // +0x0E 1: a measured rig binds this exact instance (prompt hidden)
    std::uint8_t reserved{};  // +0x0F
    std::uint64_t fallback{}; // +0x10 legacy serial; retained for the native gate layout
    std::uint64_t serial{};   // +0x18 the prompt-change serial this verdict has seen
};
static_assert(offsetof(PromptVerdict, row) == 0x00 && offsetof(PromptVerdict, handle) == 0x08 &&
              offsetof(PromptVerdict, recheck) == 0x0C && offsetof(PromptVerdict, bound) == 0x0E &&
              offsetof(PromptVerdict, fallback) == 0x10 && offsetof(PromptVerdict, serial) == 0x18 &&
              sizeof(PromptVerdict) == 0x20);

// About one second (bound) / half a second (not bound) of selection passes at 90 game frames
// per second: a lever that comes to rest after an animation gets its rig soon, and a bound one
// is re-validated so a changed object restores its prompt.
inline constexpr std::uint16_t BoundRecheck = 90;
inline constexpr std::uint16_t UnboundRecheck = 45;
// A physical attempt the game refused (permission, pose, identity): the prompt comes back.
inline constexpr std::int64_t RejectedNs = 5'000'000'000;
// The XR side's word that a hand can pull a lever now (immersive first person, the body driven
// by fresh tracking, a hand tracked), refreshed every XR frame: a hand-pulled lever hides its
// prompt only while this word is younger than this. It is cleared at once when the display mode
// cannot pull (the virtual screen, third person); a tracking gap or a stopped XR loop (headset
// off) lets it age, and the next recheck brings the prompt back.
inline constexpr std::int64_t ReadyNs = 1'000'000'000;
// Arming. The zone where the game offers the prompt (ActionButtonParam 7100: a cylinder in front
// of the lever) does not contain the handle's reach: the o240001 handle leans 0.7 m beyond the
// hinge, ~1.9 m from where the game stands the hunter. So a lever arms by proximity alone: while
// a hand can pull and no armed lever is near, the XR side asks the game about every ScanNs to look
// for a bound lever instance (its rig binds at rest on its own row, unused, not given back to its
// button) whose hinge the head or a hand is within its handle's reach plus this margin of (about
// 3 m), and the nearest is armed, as one the game selects is. It stays armed, with no timer, while
// the head or a hand stays that near, and while the game still finds it in place, selectable and
// at rest (re-checked twice a second while near). A squeeze is sent for it only from a hand already
// reaching for it (lever_acquisition.h NearMiss), and the game re-checks identity, state,
// interaction and its own permission before any grab.
inline constexpr float ArmMargin = 1.75f; // metres beyond the handle's distance from the hinge
inline constexpr std::int64_t ArmCheckNs = 500'000'000;
inline constexpr std::int64_t ScanNs = 333'000'000; // about 3 scans a second, only while needed
// The game's word on the armed lever counts this long (its checks answer within a frame or two).
inline constexpr std::int64_t ArmConfirmNs = 3 * ArmCheckNs;

inline bool Recent(std::int64_t stamp, std::int64_t now, std::int64_t window) {
    return stamp > 0 && now >= stamp && now - stamp <= window;
}

// Host-side state behind the native verdict (game thread only, under the adapter's busy guard):
// the lever instances whose normal prompt and button are back for a while, one entry each, so a
// refused squeeze at one lever never ends another's demotion, and a short demotion never
// shortens a longer one of the same lever.
struct PromptState {
    struct Demotion {
        std::uint64_t row{};
        std::uint32_t handle{};
        std::int64_t until{};
    };
    std::array<Demotion, 4> demotions{};
    void Demote(std::uint64_t row, std::uint32_t handle, std::int64_t until) {
        if (row == 0)
            return;
        Demotion *slot = &demotions[0];
        for (auto &d : demotions) {
            if (d.row == row && d.handle == handle) {
                d.until = d.until > until ? d.until : until;
                return;
            }
            if (d.until < slot->until)
                slot = &d; // a full table replaces the entry that ends first (or has ended)
        }
        *slot = {row, handle, until};
    }
    bool Demoted(std::uint64_t row, std::uint32_t handle, std::int64_t now) const {
        for (const auto &d : demotions)
            if (row != 0 && row == d.row && handle == d.handle && now < d.until)
                return true;
        return false;
    }
};

// The host classifies again on a different row/handle, a legacy serial change, a prompt change
// or a due recheck. The native gate decrements `recheck` before it asks: 1 -> 0 is due, and so is any
// count above the largest the host sets (a wrapped counter), so the gate can never be left
// skipping the host for a whole 16-bit wrap.
inline bool NeedsClassify(const PromptVerdict &v, std::uint64_t row, std::uint32_t handle,
                          std::uint64_t fallback, std::uint64_t serial) {
    return v.row != row || v.handle != handle || v.recheck == 0 || v.recheck > BoundRecheck ||
           v.fallback != fallback || v.serial != serial;
}

// The next verdict for the selected `row`/`handle`. `binds`: its rig binds at rest right now,
// a hand can pull it and the game's tables hold it. `held`: a physical hold or settle owns this
// very lever (its pose is off rest by design). B never demotes a supported lever: it can be
// intended for nearby loot. The legacy serial stays in the native gate layout, but cannot
// restore a prompt. Explicit demotions after an unavailable hand pull still keep the button.
inline PromptVerdict Decide(const PromptVerdict &previous, PromptState &state, std::uint64_t row,
                            std::uint32_t handle, bool enabled, bool binds, bool held,
                            std::uint64_t fallback, std::uint64_t serial, std::int64_t now) {
    (void)previous; // retained in the classifier interface; B no longer changes prompt policy
    PromptVerdict next{};
    next.row = row;
    next.handle = handle;
    next.fallback = fallback;
    next.serial = serial;
    next.bound = enabled && (held || (binds && !state.Demoted(row, handle, now))) ? 1 : 0;
    next.recheck = next.bound ? BoundRecheck : UnboundRecheck;
    return next;
}

} // namespace VR::Lever
