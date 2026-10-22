// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <array>
#include <atomic>
#include <mutex>

#include "core/vr/combat_channel.h"
#include "core/vr/gun_aim.h"
#include "core/vr/seqlock.h"

namespace VR::Combat {

namespace {

SeqLock<SawRequest> g_saw_request;
SeqLock<GunPull> g_pull;
SeqLock<GunStatus> g_status;
std::atomic<s64> g_last_reload_shot_ns{0};
SeqLock<MeleeWindow> g_melee_window;
SeqLock<MeleeWindow> g_melee_window_left;
SeqLock<MeleeStatus> g_melee_status;
SeqLock<MeleeTelegraph> g_melee_telegraph;
SeqLock<TransformPull> g_transform_pull;
SeqLock<TransformStatus> g_transform_status;
SeqLock<BladeShot> g_blade_shot;

template <typename T>
struct EventQueue {
    std::mutex mutex;
    std::array<T, 64> ring{};
    u32 head{0}; // the oldest
    u32 count{0};

    void Push(const T& event) {
        std::scoped_lock lock{mutex};
        if (count >= ring.size()) {
            return;
        }
        ring[(head + count) % ring.size()] = event;
        ++count;
    }

    u32 Poll(T* out, u32 max) {
        std::scoped_lock lock{mutex};
        u32 n = 0;
        while (n < max && count > 0) {
            out[n++] = ring[head];
            head = (head + 1) % ring.size();
            --count;
        }
        return n;
    }
};

EventQueue<GunEvent>& Events() {
    static EventQueue<GunEvent> queue;
    return queue;
}

EventQueue<MeleeEvent>& MeleeEvents() {
    static EventQueue<MeleeEvent> queue;
    return queue;
}

EventQueue<TransformEvent>& TransformEvents() {
    static EventQueue<TransformEvent> queue;
    return queue;
}

EventQueue<EvadeEvent>& EvadeEvents() {
    static EventQueue<EvadeEvent> queue;
    return queue;
}

} // Anonymous namespace

void PublishSawRequest(const SawRequest& request) {
    g_saw_request.Store(request);
}

bool LatestSawRequest(SawRequest& out) {
    return g_saw_request.Load(out);
}

void PublishBladeShot(const BladeShot& shot) {
    g_blade_shot.Store(shot);
}

bool LatestBladeShot(BladeShot& out) {
    return g_blade_shot.Load(out);
}

void PublishGunPull(const GunPull& pull) {
    g_pull.Store(pull);
}

bool LatestGunPull(GunPull& out) {
    return g_pull.Load(out);
}

void PublishGunStatus(const GunStatus& status) {
    g_status.Store(status);
}

bool LatestGunStatus(GunStatus& out) {
    return g_status.Load(out);
}

void PushGunEvent(const GunEvent& event) {
    // Keep continuous-fire emissions in the feedback queue, but never let one empty a
    // pistol chamber (including when observed late, after switching away from the sprayer).
    if (!Gun::ContinuousFire(static_cast<Gun::Family>(event.family)) &&
        ((event.kind == GunEventKind::Fired && event.a > 0.0f) ||
         event.kind == GunEventKind::GameShot)) {
        s64 previous = g_last_reload_shot_ns.load(std::memory_order_relaxed);
        while (event.ns > previous &&
               !g_last_reload_shot_ns.compare_exchange_weak(previous, event.ns,
                                                        std::memory_order_relaxed)) {}
    }
    Events().Push(event);
}

s64 LatestReloadShotNs() {
    return g_last_reload_shot_ns.load(std::memory_order_relaxed);
}

u32 PollGunEvents(GunEvent* out, u32 max) {
    return Events().Poll(out, max);
}

void PublishMeleeWindow(const MeleeWindow& window) {
    g_melee_window.Store(window);
}

bool LatestMeleeWindow(MeleeWindow& out) {
    return g_melee_window.Load(out);
}

void PublishMeleeWindowLeft(const MeleeWindow& window) {
    g_melee_window_left.Store(window);
}

bool LatestMeleeWindowLeft(MeleeWindow& out) {
    return g_melee_window_left.Load(out);
}

void PublishMeleeTelegraph(const MeleeTelegraph& telegraph) {
    g_melee_telegraph.Store(telegraph);
}

bool LatestMeleeTelegraph(MeleeTelegraph& out) {
    return g_melee_telegraph.Load(out);
}

void PublishMeleeStatus(const MeleeStatus& status) {
    g_melee_status.Store(status);
}

bool LatestMeleeStatus(MeleeStatus& out) {
    return g_melee_status.Load(out);
}

void PushMeleeEvent(const MeleeEvent& event) {
    MeleeEvents().Push(event);
}

u32 PollMeleeEvents(MeleeEvent* out, u32 max) {
    return MeleeEvents().Poll(out, max);
}

void PublishTransformPull(const TransformPull& pull) {
    g_transform_pull.Store(pull);
}

bool LatestTransformPull(TransformPull& out) {
    return g_transform_pull.Load(out);
}

void PublishTransformStatus(const TransformStatus& status) {
    g_transform_status.Store(status);
}

bool LatestTransformStatus(TransformStatus& out) {
    return g_transform_status.Load(out);
}

void PushTransformEvent(const TransformEvent& event) {
    TransformEvents().Push(event);
}

u32 PollTransformEvents(TransformEvent* out, u32 max) {
    return TransformEvents().Poll(out, max);
}

void PushEvadeEvent(const EvadeEvent& event) {
    EvadeEvents().Push(event);
}

u32 PollEvadeEvents(EvadeEvent* out, u32 max) {
    return EvadeEvents().Poll(out, max);
}

} // namespace VR::Combat
