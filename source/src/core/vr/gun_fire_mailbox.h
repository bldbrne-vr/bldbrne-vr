// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/vr/combat_channel.h"
#include "core/vr/interaction_mailbox.h"
#include "core/vr/interaction_native_delivery.h"
#include "core/vr/seqlock.h"
#include "physical_interactions/gun_delivery_packets.h"
namespace VR::Combat {
constexpr bool IdentifiedGunSerial(uint32_t serial) {
    return (serial & 0xc0000000u) == 0x40000000u;
}
class GunFireMailbox {
    struct Delivery {
        uint32_t ticket{};
        PiGunDeliveryRequest request{};
    };
    struct Gate {
        uint32_t ticket{}, family{}, handle{};
        uintptr_t actor{};
    };
    static uint64_t Key(uint32_t ticket, uint32_t phase) {
        return (uint64_t(ticket) << 3) | phase;
    }
    static constexpr uint32_t Await = 1, Issuing = 2, Queued = 3, Rejected = 4;

public:
    uint32_t Submit(const PiGunDeliveryRequest& q) {
        if (counter_ == 0x3fffffffu || !PhysicalInteractions::GunDelivery::Valid(q) ||
            q.request_id <= last_id_ || box_.Busy())
            return 0;
        const auto ticket = 0x40000000u | ++counter_;
        shot_.store(Key(ticket, 0), std::memory_order_relaxed);
        handoff_.store(0, std::memory_order_relaxed);
        progress_ticket_.store(0, std::memory_order_relaxed);
        if (!box_.Submit(ticket, {ticket, q}))
            return 0;
        last_id_ = q.request_id;
        return ticket;
    }
    bool Cancel(uint32_t ticket) {
        return box_.Cancel(ticket);
    }
    bool Poll(PiGunDeliveryResult& out) {
        return box_.Poll(out);
    }
    bool Busy() const {
        return box_.Busy();
    }
    bool Owns(uint32_t ticket) const {
        return ticket && claimed_.ticket == ticket;
    } // consumer only
    bool Claim(uint64_t actor, uint32_t family, int32_t attach, uintptr_t pointer, uint32_t handle,
               int64_t now, GunPull& out) {
        if (claimed_.ticket || !box_.Claim(claimed_))
            return false;
        const auto& q = claimed_.request;
        PiNativeDeliveryInput p{};
        p.operation = PI_ND_GUN_CLAIM;
        p.actor_generation = actor;
        p.expected_generation = q.character_generation;
        p.family = family;
        p.expected_family = q.family;
        p.attach = attach;
        p.expected_attach = q.attach;
        PiNativeDeliveryOutput decision{};
        if (!Interactions::NativeDelivery::Call(p, decision) || !decision.approve) {
            Finish(claimed_.ticket, 2, now, 0, "character or gun changed");
            return false;
        }
        out = {};
        out.serial = claimed_.ticket;
        out.direct = true;
        out.press_ns = q.press_ns;
        out.space_serial = q.space;
        out.grip_valid = q.grip_valid;
        out.grip_q = {q.grip_q[0], q.grip_q[1], q.grip_q[2], q.grip_q[3]};
        out.grip_p = {q.grip_p[0], q.grip_p[1], q.grip_p[2]};
        gate_.Store({claimed_.ticket, q.family, handle, pointer});
        return true;
    }
    bool Finish(uint32_t ticket, uint32_t kind, int64_t now, float value, const char* why) {
        if (!Owns(ticket) || kind == 3)
            return false;
        const auto result =
            PhysicalInteractions::GunDelivery::Result(claimed_.request, kind, now, value, why);
        if (!PhysicalInteractions::GunDelivery::Valid(result) || !box_.Finish(ticket, result))
            return false;
        claimed_ = {};
        handoff_.store(0, std::memory_order_release);
        progress_ticket_.store(0, std::memory_order_release);
        return true;
    }
    bool Handoff(uint32_t ticket, int64_t now, const char* why) {
        if (!Owns(ticket) || handoff_.load(std::memory_order_acquire))
            return false;
        progress_.Store(
            PhysicalInteractions::GunDelivery::Result(claimed_.request, 3, now, 0, why));
        handoff_.store(Key(ticket, Await), std::memory_order_release);
        progress_ticket_.store(ticket, std::memory_order_release);
        return true;
    }
    bool PollHandoff(PiGunDeliveryResult& out) {
        auto ticket = progress_ticket_.load(std::memory_order_acquire);
        if (!ticket || !progress_.Load(out))
            return false;
        return progress_ticket_.compare_exchange_strong(ticket, 0, std::memory_order_acq_rel);
    }
    bool BeginHandoff(uint32_t ticket) {
        auto expected = Key(ticket, Await);
        return handoff_.compare_exchange_strong(expected, Key(ticket, Issuing),
                                                std::memory_order_acq_rel);
    }
    // A successful pad queue is not native shot acceptance.
    bool HandoffAck(uint32_t ticket, bool accepted) {
        auto expected = Key(ticket, Issuing);
        return handoff_.compare_exchange_strong(expected, Key(ticket, accepted ? Queued : Rejected),
                                                std::memory_order_acq_rel);
    }
    bool RejectHandoff(uint32_t ticket) {
        auto expected = Key(ticket, Await);
        return handoff_.compare_exchange_strong(expected, Key(ticket, Rejected),
                                                std::memory_order_acq_rel);
    }
    // Emitter callbacks read copied host keys; they never mutate consumer-owned request/result
    // storage.
    bool NoteShot(uint32_t ticket, uint32_t family, uintptr_t actor, uint32_t handle) {
        const auto phase = handoff_.load(std::memory_order_acquire);
        if (phase != Key(ticket, Issuing) && phase != Key(ticket, Queued))
            return false;
        Gate gate{};
        if (!gate_.Load(gate) || gate.ticket != ticket || gate.family != family ||
            gate.actor != actor || gate.handle != handle)
            return false;
        uint64_t expected = Key(ticket, 0);
        return shot_.compare_exchange_strong(expected, Key(ticket, 1), std::memory_order_release,
                                             std::memory_order_relaxed);
    }
    void Advance(uint64_t actor, uint32_t family, int32_t attach, bool two_handed, int64_t now) {
        if (!claimed_.ticket)
            return;
        const auto ticket = claimed_.ticket;
        if (shot_.load(std::memory_order_acquire) == Key(ticket, 1)) {
            Finish(ticket, 4, now, 1, "handoff shot observed");
            return;
        }
        const auto phase = handoff_.load(std::memory_order_acquire);
        const auto& q = claimed_.request;
        PiNativeDeliveryInput p{};
        p.operation = PI_ND_GUN_ADVANCE;
        p.actor_generation = actor;
        p.expected_generation = q.character_generation;
        p.family = family;
        p.expected_family = q.family;
        p.attach = attach;
        p.expected_attach = q.attach;
        p.two_handed = two_handed;
        p.phase = !phase                           ? PI_ND_NONE
                  : phase == Key(ticket, Await)    ? PI_ND_AWAIT
                  : phase == Key(ticket, Issuing)  ? PI_ND_ISSUING
                  : phase == Key(ticket, Queued)   ? PI_ND_QUEUED
                  : phase == Key(ticket, Rejected) ? PI_ND_REJECTED
                                                   : PI_ND_OTHER;
        PiNativeDeliveryOutput decision{};
        if (!Interactions::NativeDelivery::Call(p, decision)) {
            // Fault cleanup closes only work definitively not issued. Observed native shots
            // above still win; issuing/queued commands retain their slot until resolved.
            if (!phase || phase == Key(ticket, Rejected) ||
                (phase == Key(ticket, Await) && RejectHandoff(ticket)))
                Finish(ticket, 2, now, 0, "delivery module unavailable");
            return;
        }
        if (!decision.finish_kind || (decision.reject_first && !RejectHandoff(ticket)))
            return;
        const char* reason = decision.reason == PI_ND_NOT_QUEUED ? "L2 was not queued"
                             : decision.reason == PI_ND_CHANGED_BEFORE_L2
                                 ? "character or gun changed before L2"
                                 : "character or gun changed";
        Finish(ticket, decision.finish_kind, now, 0, reason);
    }

private:
    Interactions::Mailbox<Delivery, PiGunDeliveryResult> box_;
    Delivery claimed_{};
    uint32_t counter_{};
    uint64_t last_id_{};
    SeqLock<Gate> gate_;
    SeqLock<PiGunDeliveryResult> progress_;
    std::atomic<uint64_t> handoff_{}, shot_{};
    std::atomic<uint32_t> progress_ticket_{};
};
} // namespace VR::Combat
