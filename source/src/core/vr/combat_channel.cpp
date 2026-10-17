// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <atomic>
#include <limits>
#include <mutex>
#include "core/vr/interaction_mailbox.h"
#include "core/vr/blade_fire_mailbox.h"
#include "core/vr/gun_fire_mailbox.h"

#include "core/vr/combat_channel.h"
#include "core/vr/seqlock.h"

namespace VR::Combat {

namespace {

SeqLock<SawRequest> g_saw_request;
SeqLock<GunPull> g_pull;
GunFireMailbox g_gun_fire;
std::atomic<bool> g_identified_gun_fire{};
SeqLock<GunStatus> g_status;
std::atomic<s64> g_last_gun_shot_ns{0};
SeqLock<MeleeWindow> g_melee_window;
SeqLock<MeleeWindow> g_melee_window_left;
SeqLock<MeleeStatus> g_melee_status;
SeqLock<MeleeTelegraph> g_melee_telegraph;
SeqLock<TransformPull> g_transform_pull;
Interactions::Mailbox<TransformPull, TransformEvent> g_transform_requests;
std::atomic<u32> g_transform_serial{};
SeqLock<TransformStatus> g_transform_status;
SeqLock<BladeShot> g_blade_shot;
BladeFireMailbox g_blade_fire;
std::atomic<bool> g_identified_blade_fire{};

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

void EnableBladeFireTransport(){Interactions::NativeDelivery::Require();g_identified_blade_fire.store(true,std::memory_order_release);}
bool BladeFireTransportEnabled(){return g_identified_blade_fire.load(std::memory_order_acquire);}
uint32_t SubmitBladeFire(const PiFireRequest& request){return g_blade_fire.Submit(request);}
bool CancelBladeFire(uint32_t ticket){return g_blade_fire.Cancel(ticket);}
bool PollBladeFire(PiFireResult& result){return g_blade_fire.Poll(result);}
bool ClaimBladeFire(uint64_t actor,int32_t variation,bool form,int32_t bow_hand,int64_t now,BladeShot& out){
    return g_blade_fire.Claim(actor,variation,form,bow_hand,now,out);
}
bool FinishBladeFire(uint32_t ticket,uint32_t outcome,int64_t now,const char* reason){
    return g_blade_fire.Finish(ticket,outcome,now,reason);
}
void PublishBladeShot(const BladeShot& shot) {
    g_blade_shot.Store(shot);
}

bool LatestBladeShot(BladeShot& out) {
    return g_blade_shot.Load(out);
}

void EnableGunFireTransport(){Interactions::NativeDelivery::Require();g_identified_gun_fire.store(true,std::memory_order_release);}
bool GunFireTransportEnabled(){return g_identified_gun_fire.load(std::memory_order_acquire);}
bool GunFireBusy(){return g_gun_fire.Busy();}
uint32_t SubmitGunFire(const PiGunDeliveryRequest& q){return g_gun_fire.Submit(q);}
bool CancelGunFire(uint32_t ticket){return g_gun_fire.Cancel(ticket);}
bool PollGunFire(PiGunDeliveryResult& out){return g_gun_fire.Poll(out);}
bool PollGunHandoff(PiGunDeliveryResult& out){return g_gun_fire.PollHandoff(out);}
bool BeginGunHandoff(uint32_t ticket){return g_gun_fire.BeginHandoff(ticket);}
bool AckGunHandoff(uint32_t ticket,bool accepted){return g_gun_fire.HandoffAck(ticket,accepted);}
bool RejectGunHandoff(uint32_t ticket){return g_gun_fire.RejectHandoff(ticket);}
bool GunFireOwns(uint32_t ticket){return g_gun_fire.Owns(ticket);}
bool ClaimGunFire(uint64_t actor,uint32_t family,int32_t attach,uintptr_t pointer,uint32_t handle,int64_t now,GunPull& out){return g_gun_fire.Claim(actor,family,attach,pointer,handle,now,out);}
void AdvanceGunFire(uint64_t actor,uint32_t family,int32_t attach,bool two_handed,int64_t now){g_gun_fire.Advance(actor,family,attach,two_handed,now);}
bool ObserveGunHandoffShot(uint32_t ticket,uint32_t family,uintptr_t actor,uint32_t handle){return g_gun_fire.NoteShot(ticket,family,actor,handle);}
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
    if ((event.kind == GunEventKind::Fired && event.a > 0.0f) ||
        event.kind == GunEventKind::GameShot) {
        s64 previous = g_last_gun_shot_ns.load(std::memory_order_relaxed);
        while (event.ns > previous &&
               !g_last_gun_shot_ns.compare_exchange_weak(previous, event.ns,
                                                        std::memory_order_relaxed)) {}
    }
    if (GunFireTransportEnabled() && IdentifiedGunSerial(event.serial)) {
        if(event.kind==GunEventKind::Vanilla) g_gun_fire.Handoff(event.serial,event.ns,event.why);
        else if(event.kind!=GunEventKind::GameShot && event.kind!=GunEventKind::ToolBurst) {
            const auto kind=event.kind==GunEventKind::Fired && event.a<=0?uint32_t(GunEventKind::Denied):uint32_t(event.kind);
            g_gun_fire.Finish(event.serial,kind,event.ns,event.a,event.why);
        }
        return; // retained outcomes own DLL-mode feedback, independent of cue-ring capacity
    }
    Events().Push(event);
}

s64 LatestGunShotNs() {
    return g_last_gun_shot_ns.load(std::memory_order_relaxed);
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

u32 NextTransformSerial() {
    auto serial = g_transform_serial.load(std::memory_order_relaxed);
    while (serial != std::numeric_limits<u32>::max()) {
        if (g_transform_serial.compare_exchange_weak(serial, serial + 1, std::memory_order_relaxed))
            return serial + 1;
    }
    return 0; // never wrap and alias an old action
}

bool SubmitTransformPull(const TransformPull& pull) {
    return g_transform_requests.Submit(pull.serial, pull);
}
bool CancelTransformPull(u32 serial) {
    return g_transform_requests.Cancel(serial);
}
bool ClaimTransformPull(TransformPull& out) {
    return g_transform_requests.Claim(out);
}
bool PollTransformOutcome(TransformEvent& out) {
    return g_transform_requests.Poll(out);
}
bool TransformPullBusy() {
    return g_transform_requests.Busy();
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
    // Locked is definitive success for the non-idempotent form change. Retiring the
    // request here permits a later explicit squeeze to interrupt the settled animation
    // tail as before. Late tail Done/Aborted events cannot complete a different serial.
    if (event.kind == TransformEventKind::Locked || event.kind == TransformEventKind::Done ||
        event.kind == TransformEventKind::Denied || event.kind == TransformEventKind::Aborted)
        g_transform_requests.Finish(event.serial, event);
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
