// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>

#include "core/vr/quick_items.h"
#include "core/vr/vial_contact.h"
#include "physical_interactions/quick_action_api.h"

// Quick items in VR: the records the threads hand each other (core/vr/quick_items.h has the
// types). Every record is a single-writer seqlock (core/vr/seqlock.h): no thread ever waits for
// another.
//  - Snapshot: the main player's update (site A) -> the presenter and the render-camera hook.
//  - Request: the presenter -> site A (one at a time; site A answers each serial once).
//  - Result: site A -> the presenter.
//  - Scene: the presenter -> the render-camera hook (what the arc shows).
//  - Hover: the render-camera hook -> the presenter (the card the hunter's palm is on).
//  - Wipe: the render-camera hook -> the presenter (the paper against the blade).
//  - Launch: the projectile hook (the bullet manager's thread) -> the presenter (a thrown item
//    left with your velocity).
//  - Held item: the presenter -> the held-item tick (core/vr/bloodborne_held.cpp): the goods and
//    effect the hunter's left hand shows.
namespace VR::Quick {

void PublishVialContact(const VialContact::Sample& sample);
bool LatestVialContact(VialContact::Sample& out);

[[nodiscard]] std::int64_t NowNs(); // steady clock

void PublishSnapshot(const Snapshot& snapshot);
bool LatestSnapshot(Snapshot& out);

// Startup selection only; both LatestRequest and PublishResult switch together.
void EnableInteractionTransport();
// Game-thread only, immediately before publishing the quick-item snapshot. Actor keys
// remain in the host; the DLL receives only the monotonically increasing generation.
void ObserveInteractionActor(uintptr_t actor, uint32_t handle);
uint64_t NextInteractionRequestId();
// Game-thread only: verify the current observed actor without changing its generation.
uint64_t InteractionActorGeneration(uintptr_t actor, uint32_t handle);

// Identified consumable transport. Presenter and game hook select it together at startup
// in DLL mode. Ticket 0 means not submitted.
uint32_t SubmitInteractionRequest(const PiQuickRequest& request);
bool CancelInteractionRequest(uint32_t ticket);
bool ClaimInteractionRequest(uint64_t character_generation, Request& out);
bool FinishInteractionRequest(const Result& result);
bool PollInteractionResult(PiQuickResult& out);
bool InteractionRequestBusy();

void PublishRequest(const Request& request);
bool LatestRequest(Request& out);

void PublishResult(const Result& result);
bool LatestResult(Result& out);

void PublishScene(const Scene& scene);
bool LatestScene(Scene& out);
// Exact rendered body attachments -> left-hand pickup test.
void PublishHolsterTargets(const Holsters::Targets& targets);
bool LatestHolsterTargets(Holsters::Targets& out);

void PublishHover(const Hover& hover);
bool LatestHover(Hover& out);

// quick_use_wipe: the paper in the hunter's left hand against his weapon's blade (the
// render-camera hook -> the presenter, every rendered frame while a wipe item is held).
void PublishWipe(const WipeContact& contact);
bool LatestWipe(WipeContact& out);

struct Launch {
    std::uint32_t count{0};      // launches matched so far
    std::int64_t steady_ns{0};
    std::int32_t bullet{-1};
    std::int32_t goods{-1};
    std::array<float, 3> position{}; // world
    std::array<float, 3> velocity{}; // world units / s, as written
    float game_speed{0.0f};          // the game's own launch speed
    float issue_to_launch_ms{0.0f};
};
void PublishLaunch(const Launch& launch);
bool LatestLaunch(Launch& out);

// The item in the hunter's left hand (presenter, every input frame): goods -1 = none; ffx = the
// game's held effect (-1: none, our mesh is drawn instead).
void PublishHeldItem(std::int32_t goods, std::int32_t ffx);
void LatestHeldItem(std::int32_t& goods, std::int32_t& ffx);

// The game's held effect for the item failed (no live effect): the in-frame renderer draws our
// mesh. Written by the held-item tick.
void SetHeldItemNative(bool shown);
[[nodiscard]] bool HeldItemNative();

// Canned item uses (CannedOf: the Shaman Bone Blade's stab): our Square for one went out (the
// presenter, once pad.Press took it) -> the main player's update (core/vr/bloodborne_cinematic.cpp:
// the cinematic's ItemUseGate arms its flat view). serial 0: never.
struct CannedUse {
    std::uint32_t serial{0};
    std::int32_t goods{-1};
    std::int32_t clip{-1}; // CannedClipOf(goods)
    std::int64_t steady_ns{0};
};
void NoteCannedUse(std::int32_t goods);
bool LatestCannedUse(CannedUse& out);

} // namespace VR::Quick
