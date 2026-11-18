// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>

#include "common/types.h"
#include "core/vr/arm_ik.h"

// Bloodborne (v1.09 eboot): bare hands on corpses (vr.ini corpse_hands; the pure rules are
// core/vr/corpse_hands.h). In unarmed mode a hand pushes a dead enemy's ragdoll around, and a
// squeeze while touching one holds that part of it: dragged, lifted, thrown, within the hand's
// strength, the rest of the corpse hanging heavier than the game's gravity.
//
// The corpse is the game's own ragdoll (RagdollIns state 2: its bodies dynamic, stepped by the
// game's hknpWorld). The body's pose hook hands over the bare hands (CorpseHands: the palm where the
// controller holds it, its squeeze) and a few times a second looks for dead enemies with a ragdoll
// near the hunter (WorldChrMan's chr sets); the nearest four are watched by the kill ragdoll's entry
// stub on the ChrCtrl ragdoll update (bloodborne_ragdoll.cpp), which calls CorpseStep on each one's
// own update, on its own thread, where the kill ragdoll's push already runs. There the parts are
// read (each body's capsule or sphere, its motion's mass, inertia and velocities) and the hands'
// impulses go in through the game's own hknpWorld::applyBodyImpulseAt (0xB69150) and
// applyBodyLinearImpulse (0xB68DB0). Live enemies, armed hands, corpses without a ragdoll of their
// own (bosses, dissolves) are never touched. Every site is signature-checked at install.
namespace VR::Bloodborne {

// The bare hands as the body's pose places them (world, game units); C0000::Left = 0, Right = 1.
struct CorpseHandsIn {
    s64 now{0};
    float scale{1.0f};     // the world scale (game units per metre of yours)
    const u8* player{nullptr}; // the hunter's ChrIns (never a corpse)
    VR::Body::Vec3 feet{}; // where he stands
    std::array<bool, 2> bare{}; // unarmed, tracked, yours (not a clip's, not on a lever)
    std::array<VR::Body::Vec3, 2> palm{};
    std::array<VR::Body::Quat, 2> q{};
    std::array<float, 2> squeeze{};
};

// OnEbootLoaded, after InstallRagdoll (whose entry stub calls CorpseStep). Logs one line.
void InstallCorpseHands(VAddr base, u64 size);

// The body's pose hook, every main-player pose (no hand bare and nothing held: a check, nothing
// more).
void CorpseHands(const CorpseHandsIn& in);

// The ChrCtrl ragdoll update of a watched corpse (the kill ragdoll's entry stub), on its own thread.
void CorpseStep(u8* ctrl);

// While a hand holds a corpse: where its palm holds it now (world, game units), from that corpse's
// last update. The body's pose draws the hand there instead of on its controller (the user,
// 2026-11-02: "physical (real player) hand can be further up, but game limits it to max lift
// point"): a body too heavy to follow keeps the drawn hand on it, while the pull still comes from
// where the controller is.
struct CorpseGrip {
    s64 ns{0}; // steady clock of that update
    bool on{false};
    VR::Body::Vec3 palm{};
};
// False before any hold (both hands then free).
bool LatestCorpseGrips(std::array<CorpseGrip, 2>& out);

// Fresh contact with a dynamic grabbable part, or an active hold. Atomic hint only:
// no guest-memory reads or corpse mutex on the input/render thread.
bool CorpseGrabAvailable(unsigned hand, s64 now);

} // namespace VR::Bloodborne
