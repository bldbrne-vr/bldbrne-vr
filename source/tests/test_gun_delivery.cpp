// SPDX-License-Identifier: GPL-2.0-or-later
#include <thread>
#include "core/vr/interaction_native_delivery_module.h"
#include "core/vr/gun_fire_mailbox.h"
#include "core/vr/interaction_gun.h"
#include "core/vr/interaction_gun_delivery.h"
#include "interaction_test_support.h"
namespace C = VR::Combat;
namespace D = PhysicalInteractions::GunDelivery;
PiGunDeliveryRequest Request(uint64_t id = 1) {
    PiGunDeliveryRequest q{};
    q.session = 1;
    q.request_id = id;
    q.character_generation = 9;
    q.controller_serial = 77;
    q.press_ns = 1;
    q.family = 1;
    q.attach = 348;
    q.grip_valid = 1;
    q.grip_q[3] = 1;
    return q;
}
struct Rig {
    VR::Interactions::Module module;
    PiGunDeliveryInput p{};
    PiGunDeliveryOutput o{};
    uint64_t sequence{};
    Rig(const char* path) {
        Check(module.Load(std::filesystem::absolute(path)), "load gun delivery DLL");
    }
    uint32_t Call(uint32_t operation) {
        p.size = sizeof(p);
        p.operation = operation;
        p.session = module.Session();
        p.sequence = ++sequence;
        o = {};
        o.size = sizeof(o);
        return module.GunDelivery(p, o);
    }
};
void Ledger(const char* path) {
    Rig r(path);
    r.p.request = Request();
    r.p.request.session = r.module.Session();
    Check(r.Call(PI_GUN_DELIVERY_SUBMIT) == PI_OK && r.o.pending, "owned direct gun request");
    const auto q = r.p.request;
    r.p.result = D::Result(q, 3, 10, 0, "handed to L2");
    Check(r.Call(PI_GUN_DELIVERY_HANDOFF) == PI_OK && r.o.pending &&
              r.o.resolution == PI_ACK_CURRENT,
          "handoff is progress, not completion");
    Check(r.Call(PI_GUN_DELIVERY_HANDOFF) == PI_BAD_PACKET, "duplicate handoff cannot press twice");
    Check(r.Call(PI_GUN_DELIVERY_RESULT) == PI_BAD_PACKET, "queued L2 cannot settle action");
    for (int i = 0; i < 900; ++i)
        Check(r.Call(PI_GUN_DELIVERY_SNAPSHOT) == PI_OK && r.o.pending,
              "long handoff delay never proves nonexecution");
    r.p.request.request_id = 2;
    Check(r.Call(PI_GUN_DELIVERY_SUBMIT) == PI_PENDING, "unresolved gun cannot be replaced");
    r.module.Reset();
    r.sequence = 0;
    Check(r.Call(PI_GUN_DELIVERY_SNAPSHOT) == PI_OK && r.o.pending && r.o.cancel_requested,
          "reset retains accepted handoff");
    r.p.result = D::Result(q, 4, 30, 1, "late shot");
    Check(r.Call(PI_GUN_DELIVERY_RESULT) == PI_OK && r.o.resolution == PI_ACK_CANCELLED,
          "late shot suppresses feedback");
    Check(r.Call(PI_GUN_DELIVERY_RESULT) == PI_BAD_PACKET, "duplicate terminal ignored");
    r.p.request = Request(3);
    r.p.request.session = r.module.Session();
    Check(r.Call(PI_GUN_DELIVERY_SUBMIT) == PI_OK, "new identity");
    r.p.rejected_session = r.p.request.session;
    r.p.rejected_id = 4;
    Check(r.Call(PI_GUN_DELIVERY_NOT_EXECUTED) == PI_BAD_PACKET,
          "wrong queue rejection cannot clear request");
    r.p.rejected_id = 3;
    Check(r.Call(PI_GUN_DELIVERY_NOT_EXECUTED) == PI_OK && !r.o.pending,
          "exact nonexecution settles");
}
void Transport() {
    C::GunFireMailbox box;
    C::GunPull native{};
    PiGunDeliveryResult result{};
    for (uint32_t kind : {0u, 1u, 2u}) {
        const auto q = Request(kind + 1);
        const auto ticket = box.Submit(q);
        Check(C::IdentifiedGunSerial(ticket), "process native ticket namespace");
        Check(box.Claim(9, 1, 348, 0x1234, 42, 10, native) && native.serial == ticket,
              "native gun claim");
        Check(!box.Cancel(ticket) && !box.Submit(Request(99)),
              "claimed action cannot be replaced or cancelled");
        char text[32] = "owned reason";
        Check(box.Finish(ticket, kind, 20, kind == 0 ? 1.f : 0.f, text), "retained direct outcome");
        text[0] = 'X';
        Check(box.Poll(result) && D::Matches(q, result) && result.reason[0] == 'o',
              "native ticket mapped to owned controller identity");
        Check(!box.Poll(result), "terminal result once");
    }
    auto q = Request(4);
    auto ticket = box.Submit(q);
    Check(ticket, "handoff submit");
    Check(box.Claim(9, 1, 348, 0x1234, 42, 10, native), "handoff claim");
    Check(box.Handoff(ticket, 20, "arm busy") && !box.Handoff(ticket, 20, "duplicate"),
          "progress once");
    Check(box.PollHandoff(result) && result.kind == 3 && !box.PollHandoff(result),
          "progress retained independently");
    Check(box.BeginHandoff(ticket) && !box.BeginHandoff(ticket) && box.HandoffAck(ticket, true),
          "L2 is issued once");
    box.Advance(9, 1, 348, false, 30);
    Check(box.Busy() && !box.Poll(result), "accepting L2 is not a fired shot");
    Check(!box.NoteShot(ticket, 2, 0x1234, 42) && !box.NoteShot(ticket, 1, 0x1235, 42) &&
              !box.NoteShot(ticket, 1, 0x1234, 43),
          "wrong emitter ownership rejected");
    Check(box.NoteShot(ticket, 1, 0x1234, 42) && !box.NoteShot(ticket, 1, 0x1234, 42),
          "matching shot observation once");
    box.Advance(9, 1, 348, false, 40);
    Check(box.Poll(result) && result.kind == 4 && D::Matches(q, result),
          "observed shot settles accepted handoff");
    q = Request(5);
    ticket = box.Submit(q);
    Check(box.Claim(9, 1, 348, 0x1234, 42, 10, native) && box.Handoff(ticket, 20, "busy"),
          "refused L2 setup");
    Check(box.BeginHandoff(ticket) && box.HandoffAck(ticket, false), "Pad queue refusal recorded");
    box.Advance(9, 1, 348, false, 30);
    Check(box.Poll(result) && result.kind == 2, "definite unqueued handoff completes denial");
    q = Request(6);
    ticket = box.Submit(q);
    Check(!box.Claim(10, 1, 348, 0x1234, 42, 10, native) && box.Poll(result) && result.kind == 2,
          "wrong character cannot fire");
    q = Request(7);
    ticket = box.Submit(q);
    Check(box.Claim(9, 1, 348, 0x1234, 42, 10, native) && box.Handoff(ticket, 20, "busy"),
          "actor changed before issue setup");
    box.Advance(10, 1, 348, false, 30);
    Check(!box.BeginHandoff(ticket) && box.Poll(result) && result.kind == 2,
          "actor change wins before L2 issue");
    q = Request(8);
    ticket = box.Submit(q);
    Check(box.Claim(9, 1, 348, 0x1234, 42, 10, native) && box.Handoff(ticket, 20, "busy"),
          "accepted actor change setup");
    Check(box.BeginHandoff(ticket) && box.HandoffAck(ticket, true), "accepted L2 setup");
    box.Advance(10, 2, 350, true, 1'000'000'000);
    Check(box.Busy() && !box.Poll(result), "actor change cannot prove accepted L2 failed");
    Check(box.NoteShot(ticket, 1, 0x1234, 42), "old actor final observation retained");
    box.Advance(10, 2, 350, true, 1'000'000'001);
    Check(box.Poll(result) && result.kind == 4, "old actor final result reconciled");
}
void Races() {
    C::GunFireMailbox box;
    for (uint64_t i = 1; i <= 10000; ++i) {
        const auto q = Request(i);
        const auto ticket = box.Submit(q);
        Check(ticket, "race submit");
        std::atomic<bool> start{};
        bool claimed = false;
        std::thread consumer([&] {
            while (!start.load(std::memory_order_acquire)) {
            }
            C::GunPull p{};
            claimed = box.Claim(9, 1, 348, 0x1234, 42, 10, p);
            if (claimed)
                Check(box.Finish(p.serial, 0, 20, 1, "race"), "race completion");
        });
        start.store(true, std::memory_order_release);
        const bool cancelled = box.Cancel(ticket);
        consumer.join();
        Check(cancelled != claimed, "claim/cancel exactly one wins");
        PiGunDeliveryResult r{};
        Check(box.Poll(r) == claimed, "race retained result");
        if (claimed)
            Check(D::Matches(q, r), "race identity");
    }
    for (uint64_t i = 10001; i <= 11000; ++i) {
        const auto q = Request(i);
        const auto ticket = box.Submit(q);
        C::GunPull native{};
        Check(box.Claim(9, 1, 348, 0x1234, 42, 10, native) && box.Handoff(ticket, 20, "race"),
              "handoff race setup");
        std::atomic<bool> start{};
        bool rejected = false;
        std::thread consumer([&] {
            while (!start.load(std::memory_order_acquire)) {
            }
            rejected = box.RejectHandoff(ticket);
        });
        start.store(true, std::memory_order_release);
        const bool issuing = box.BeginHandoff(ticket);
        consumer.join();
        Check(rejected != issuing, "issue/reject race exactly one wins");
        if (issuing)
            Check(box.HandoffAck(ticket, false), "issued but refused acknowledgement");
        box.Advance(9, 1, 348, false, 30);
        PiGunDeliveryResult r{};
        Check(box.Poll(r) && r.kind == 2, "handoff race closes without a shot");
    }
}
void Runtime(const char* path) {
    Rig r(path);
    bool fault = false;
    VR::Interactions::GunDeliveryRuntime runtime;
    runtime.Bind(r.module, fault);
    VR::Quick::ObserveInteractionActor(0x1234, 42);
    VR::Quick::Snapshot s{};
    s.valid = true;
    VR::Quick::PublishSnapshot(s);
    Check(VR::Quick::LatestSnapshot(s), "actor facts");
    auto request = [&] {
        auto q = Request(VR::Quick::NextInteractionRequestId());
        q.session = r.module.Session();
        q.character_generation = s.character_generation;
        return q;
    };
    C::GunPull native{};
    PiGunDeliveryResult out{};
    auto q = request();
    Check(runtime.Submit(q), "production direct submit");
    Check(C::ClaimGunFire(s.character_generation, 1, 348, 0x1234, 42, 10, native),
          "production direct claim");
    for (int i = 0; i < 80; ++i)
        C::PushGunEvent({C::GunEventKind::ToolBurst, 0, 0, 0, 10, "overflow"});
    C::PushGunEvent({C::GunEventKind::Denied, native.serial, 1, 0, 20, "owned denial"});
    Check(runtime.Poll(out) && out.kind == 2 && D::Matches(q, out) && !runtime.Poll(out),
          "denial survives full cue ring exactly once");
    q = request();
    Check(runtime.Submit(q) &&
              C::ClaimGunFire(s.character_generation, 1, 348, 0x1234, 42, 10, native),
          "production handoff setup");
    C::PushGunEvent({C::GunEventKind::Vanilla, native.serial, 1, 0, 20, "arm busy"});
    Check(runtime.Poll(out) && out.kind == 3 && runtime.Pending(),
          "production handoff stays pending");
    Check(runtime.BeginHandoff(), "production issue once");
    runtime.AckHandoff(true);
    for (int i = 0; i < 900; ++i) {
        runtime.Service();
        Check(runtime.Pending() && !runtime.Submit(request()) && !fault,
              "accepted handoff never expires/replaces");
    }
    r.module.Reset();
    runtime.Service();
    Check(C::ObserveGunHandoffShot(native.serial, 1, 0x1234, 42),
          "native handoff shot observed after reset");
    C::AdvanceGunFire(s.character_generation, 1, 348, false, 30);
    bool reconcile_only = false;
    Check(runtime.Poll(out, &reconcile_only) && reconcile_only && !runtime.Pending() && !fault,
          "reset returns policy-only final outcome without feedback");
    q = request();
    Check(runtime.Submit(q), "post-reset unclaimed submit");
    runtime.Cancel();
    Check(!C::ClaimGunFire(s.character_generation, 1, 348, 0x1234, 42, 40, native) &&
              !runtime.Pending(),
          "unclaimed cancellation prevents shot");
}
void ReloadReconciliation(const char* path) {
    Rig r(path);
    bool fault = false;
    VR::Interactions::GunRuntime gun;
    VR::Interactions::GunDeliveryRuntime delivery;
    gun.Bind(r.module, fault);
    delivery.Bind(r.module, fault);
    VR::Quick::ObserveInteractionActor(0x1234, 42);
    VR::Quick::Snapshot actor{};
    actor.valid = true;
    VR::Quick::PublishSnapshot(actor);
    Check(VR::Quick::LatestSnapshot(actor), "reload actor facts");
    PiGunInput frame{};
    auto& f = frame.facts;
    f.now_ns = f.status_ns = f.player_since_ns = 1'000'000'000;
    f.fresh = f.in_hand = f.direct_ok = f.ready = f.can_fire = f.aim = 1;
    f.fire_direct = f.wrist_reload = f.haptics = f.reload_pose_valid = 1;
    f.player_epoch = 1;
    f.family = 1;
    f.cadence_s = .3f;
    f.space = 9;
    f.bow_hand = 1;
    f.grip.q.w = 1;
    gun.Update(frame);
    f.trigger = 1;
    ++f.now_ns;
    const auto sent = gun.Update(frame);
    Check((sent.flags & PI_GUN_SEND) && sent.reload_pending, "reload reserves direct request");
    auto q = Request(VR::Quick::NextInteractionRequestId());
    q.session = r.module.Session();
    q.character_generation = actor.character_generation;
    q.controller_serial = sent.request_serial;
    Check(delivery.Submit(q), "reload direct delivery submit");
    C::GunPull native{};
    Check(C::ClaimGunFire(actor.character_generation, 1, 348, 0x1234, 42, 10, native),
          "reload direct claim");
    r.module.Reset();
    delivery.Service();
    C::PushGunEvent({C::GunEventKind::Denied, native.serial, 1, 0, 20, "late refusal"});
    PiGunDeliveryResult outcome{};
    bool reconcile = false;
    Check(delivery.Poll(outcome, &reconcile) && reconcile,
          "cancelled denial still supplies state reconciliation");
    frame.event_count = 1;
    frame.events[0] = {};
    frame.events[0].kind = outcome.kind;
    frame.events[0].serial = outcome.controller_serial;
    frame.events[0].family = outcome.family;
    frame.events[0].ns = outcome.now_ns;
    frame.events[0].value = outcome.value;
    frame.events[0].flags = PI_GUN_RECONCILE_ONLY;
    ++f.now_ns;
    const auto settled = gun.Update(frame);
    Check(!settled.reload_pending && !settled.events[0].flags && !(settled.flags & PI_GUN_SEND),
          "late denial settles reload without feedback or held-button replay");
}
void DeliveryPolicyFaultRetention() {
    C::GunFireMailbox box;
    auto q = Request();
    auto ticket = box.Submit(q);
    C::GunPull native{};
    Check(box.Claim(9, 1, 348, 0x1234, 42, 10, native) &&
              box.Handoff(ticket, 20, "fault test") && box.BeginHandoff(ticket) &&
              box.HandoffAck(ticket, true), "queue before policy fault");
    using namespace VR::Interactions::NativeDelivery;
    const auto* previous = binding.load(std::memory_order_acquire);
    const Binding failed{nullptr, [](void*, const PiNativeDeliveryInput&,
                                     PiNativeDeliveryOutput&) -> uint32_t { return PI_BAD_PACKET; }};
    Bind(&failed);
    box.Advance(10, 2, 0, true, 1000000000000LL);
    PiGunDeliveryResult result{};
    Check(box.Busy() && !box.Poll(result), "fault and actor change retain queued command");
    Check(box.NoteShot(ticket, 1, 0x1234, 42), "native shot evidence survives policy fault");
    box.Advance(10, 2, 0, true, 1000000000001LL);
    Check(box.Poll(result) && result.kind == 4 && D::Matches(q, result),
          "confirmed native shot closes retained command exactly once");
    Bind(previous);
    q = Request(2); ticket = box.Submit(q);
    Check(box.Claim(9, 1, 348, 0x1234, 42, 10, native) && box.Handoff(ticket, 20, "unissued"),
          "unissued fault setup");
    Bind(&failed);
    box.Advance(9, 1, 348, false, 30);
    Check(!box.BeginHandoff(ticket) && box.Poll(result) && result.kind == 2,
          "fault can close only definitively unissued work");
    Bind(previous);
}
int main(int argc, char** argv) {
    Check(argc == 2, "DLL path");
    // Native hooks retain a separate immutable module binding in production. Keep this
    // test binding alive across all transport/race cases and exercise the actual DLL policy.
    VR::Interactions::Module delivery_module;
    Check(delivery_module.Load(std::filesystem::absolute(argv[1])), "load native delivery DLL");
    const auto delivery_binding = VR::Interactions::NativeDelivery::MakeBinding(delivery_module);
    VR::Interactions::NativeDelivery::Require();
    VR::Interactions::NativeDelivery::Bind(&delivery_binding);
    Ledger(argv[1]);
    Transport();
    DeliveryPolicyFaultRetention();
    Races();
    Runtime(argv[1]);
    ReloadReconciliation(argv[1]);
    std::cout << "PASS: retained gun results, nonterminal L2 handoff, actor/equipment validation, "
                 "reset/refusal, cue overflow, 10000 claim/cancel and 1000 issue/reject races\n";
}
