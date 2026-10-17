// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <chrono>
#include <limits>

#include "core/vr/quick_channel.h"
#include "core/vr/seqlock.h"
#include "core/vr/quick_interaction_mailbox.h"

namespace VR::Quick {

namespace {

struct Channel {
    SeqLock<VialContact::Sample> vial_contact;
    SeqLock<Snapshot> snapshot;
    SeqLock<Request> request;
    InteractionMailbox interactions;
    std::atomic<bool> identified{false};
    std::atomic<uint64_t> request_id{};
    uintptr_t actor{}; // game thread only
    uint32_t actor_handle{};
    uint64_t generation{};
    SeqLock<Result> result;
    SeqLock<Scene> scene;
    SeqLock<Holsters::Targets> holsters; // render-camera hook is the only writer
    SeqLock<Hover> hover;
    SeqLock<Launch> launch;
    SeqLock<WipeContact> wipe;
    SeqLock<CannedUse> canned;
    std::atomic<std::uint32_t> canned_serial{0}; // the presenter's own count (single writer)
    std::atomic<std::uint64_t> held{~std::uint64_t{0}};
    std::atomic<bool> native{false};
};

Channel& C() {
    static Channel channel;
    return channel;
}

} // Anonymous namespace

std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

void PublishVialContact(const VialContact::Sample& s) { C().vial_contact.Store(s); }
bool LatestVialContact(VialContact::Sample& s) { return C().vial_contact.Load(s); }

void PublishSnapshot(const Snapshot& snapshot) {
    auto copied = snapshot;
    copied.character_generation = C().actor ? C().generation : 0;
    C().snapshot.Store(copied);
}
bool LatestSnapshot(Snapshot& out) {
    return C().snapshot.Load(out);
}

void EnableInteractionTransport() { C().identified.store(true, std::memory_order_release); }
void ObserveInteractionActor(uintptr_t actor, uint32_t handle) {
    auto& c = C();
    if (actor != c.actor || handle != c.actor_handle) {
        c.actor = actor; c.actor_handle = handle;
        if (c.generation != std::numeric_limits<uint64_t>::max()) ++c.generation;
        else c.actor = 0; // exhausted identity cannot authorize guest execution
    }
}
uint64_t InteractionActorGeneration(uintptr_t actor, uint32_t handle) {
    const auto& c=C();return actor && actor==c.actor && handle==c.actor_handle ? c.generation : 0;
}
uint64_t NextInteractionRequestId() {
    auto& counter = C().request_id;
    auto old = counter.load(std::memory_order_relaxed);
    while (old != std::numeric_limits<uint64_t>::max()) {
        if (counter.compare_exchange_weak(old,old+1,std::memory_order_relaxed)) return old+1;
    }
    return 0;
}
uint32_t SubmitInteractionRequest(const PiQuickRequest& request) {
    const auto ticket = C().interactions.Submit(request);
    if (ticket) {
        auto& counter = C().request_id;
        auto old = counter.load(std::memory_order_relaxed);
        while (old < request.request_id &&
               !counter.compare_exchange_weak(old, request.request_id, std::memory_order_relaxed)) {}
    }
    return ticket;
}
bool CancelInteractionRequest(uint32_t ticket) {
    return C().interactions.Cancel(ticket);
}
bool ClaimInteractionRequest(uint64_t character_generation, Request& out) {
    return C().interactions.Claim(character_generation, out);
}
bool FinishInteractionRequest(const Result& result) {
    return C().interactions.Finish(result);
}
bool PollInteractionResult(PiQuickResult& out) {
    return C().interactions.Poll(out);
}
bool InteractionRequestBusy() {
    return C().interactions.Busy();
}

void PublishRequest(const Request& request) {
    C().request.Store(request);
}
bool LatestRequest(Request& out) {
    if (C().identified.load(std::memory_order_acquire))
        return C().interactions.Claim(C().actor ? C().generation : 0,out);
    return C().request.Load(out);
}

void PublishResult(const Result& result) {
    if (C().identified.load(std::memory_order_acquire)) {
        C().interactions.Finish(result);
        return;
    }
    C().result.Store(result);
}
bool LatestResult(Result& out) {
    return C().result.Load(out);
}

void PublishScene(const Scene& scene) {
    C().scene.Store(scene);
}
bool LatestScene(Scene& out) {
    return C().scene.Load(out);
}

void PublishHolsterTargets(const Holsters::Targets& targets) {
    C().holsters.Store(targets);
}
bool LatestHolsterTargets(Holsters::Targets& out) {
    return C().holsters.Load(out);
}

void PublishHover(const Hover& hover) {
    C().hover.Store(hover);
}
bool LatestHover(Hover& out) {
    return C().hover.Load(out);
}

void PublishWipe(const WipeContact& contact) {
    C().wipe.Store(contact);
}
bool LatestWipe(WipeContact& out) {
    return C().wipe.Load(out);
}

void PublishLaunch(const Launch& launch) {
    C().launch.Store(launch);
}
bool LatestLaunch(Launch& out) {
    return C().launch.Load(out);
}

void PublishHeldItem(std::int32_t goods, std::int32_t ffx) {
    const std::uint64_t word = (static_cast<std::uint64_t>(static_cast<std::uint32_t>(goods))
                                << 32) |
                               static_cast<std::uint32_t>(ffx);
    C().held.store(word, std::memory_order_release);
}

void LatestHeldItem(std::int32_t& goods, std::int32_t& ffx) {
    const std::uint64_t word = C().held.load(std::memory_order_acquire);
    goods = static_cast<std::int32_t>(static_cast<std::uint32_t>(word >> 32));
    ffx = static_cast<std::int32_t>(static_cast<std::uint32_t>(word));
}

void SetHeldItemNative(bool shown) {
    C().native.store(shown, std::memory_order_relaxed);
}

bool HeldItemNative() {
    return C().native.load(std::memory_order_relaxed);
}

void NoteCannedUse(std::int32_t goods) {
    CannedUse use{};
    use.serial = C().canned_serial.fetch_add(1, std::memory_order_relaxed) + 1;
    use.goods = goods;
    use.clip = CannedClipOf(goods);
    use.steady_ns = NowNs();
    C().canned.Store(use);
}

bool LatestCannedUse(CannedUse& out) {
    return C().canned.Load(out);
}

} // namespace VR::Quick
