// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <atomic>
#include <chrono>

#include "core/vr/quick_channel.h"
#include "core/vr/seqlock.h"

namespace VR::Quick {

namespace {

struct Channel {
    SeqLock<VialContact::Sample> vial_contact;
    SeqLock<Snapshot> snapshot;
    SeqLock<Request> request;
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
    C().snapshot.Store(snapshot);
}
bool LatestSnapshot(Snapshot& out) {
    return C().snapshot.Load(out);
}

void PublishRequest(const Request& request) {
    C().request.Store(request);
}
bool LatestRequest(Request& out) {
    return C().request.Load(out);
}

void PublishResult(const Result& result) {
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
