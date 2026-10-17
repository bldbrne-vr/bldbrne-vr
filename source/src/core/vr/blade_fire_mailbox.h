// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/vr/combat_channel.h"
#include "core/vr/interaction_mailbox.h"
#include "core/vr/interaction_native_delivery.h"
#include "physical_interactions/fire_action_packets.h"
namespace VR::Combat {
class BladeFireMailbox {
    struct Delivery {
        uint32_t ticket{};
        PiFireRequest request{};
    };

public:
    uint32_t Submit(const PiFireRequest& p) {
        if (last_ticket_ == 0x7fffffffu || !PhysicalInteractions::FireActions::Valid(p) ||
            p.request_id <= last_id_ || box_.Busy())
            return 0;
        const auto next = last_ticket_ + 1;
        if (!box_.Submit(next, {next, p}))
            return 0;
        last_ticket_ = next;
        last_id_ = p.request_id;
        return next;
    }
    bool Cancel(uint32_t ticket) {
        return box_.Cancel(ticket);
    }
    bool Poll(PiFireResult& r) {
        return box_.Poll(r);
    }
    bool Busy() const {
        return box_.Busy();
    }
    bool Claim(uint64_t actor, int32_t variation, bool form, int32_t bow_hand, int64_t now,
               BladeShot& out) {
        if (claimed_.ticket || !box_.Claim(claimed_))
            return false;
        const auto& q = claimed_.request;
        PiNativeDeliveryInput p{};
        p.operation = PI_ND_BLADE_CLAIM;
        p.actor_generation = actor;
        p.expected_generation = q.character_generation;
        p.variation = variation;
        p.expected_variation = q.variation;
        p.form_b = form;
        p.expected_form_b = q.form_b;
        p.shot = q.shot;
        p.bow_hand = bow_hand;
        p.expected_bow_hand = q.bow_hand;
        PiNativeDeliveryOutput decision{};
        if (!Interactions::NativeDelivery::Call(p, decision) || !decision.approve) {
            Finish(claimed_.ticket, PI_FIRE_DENIED, now, "character or equipment changed");
            return false;
        }
        out = {};
        out.serial = claimed_.ticket;
        out.shot = static_cast<uint8_t>(q.shot);
        out.charged = q.charged;
        out.press_ns = q.press_ns;
        out.space_serial = q.space;
        out.window = q.window;
        out.dir_valid = q.dir_valid;
        out.dir = {q.direction[0], q.direction[1], q.direction[2]};
        return true;
    }
    bool Finish(uint32_t ticket, uint32_t outcome, int64_t now, const char* reason) {
        if (!claimed_.ticket || ticket != claimed_.ticket)
            return false;
        const auto r =
            PhysicalInteractions::FireActions::Result(claimed_.request, outcome, now, reason);
        if (!PhysicalInteractions::FireActions::Valid(r) || !box_.Finish(ticket, r))
            return false;
        claimed_ = {};
        return true;
    }

private:
    Interactions::Mailbox<Delivery, PiFireResult> box_;
    Delivery claimed_{};
    uint32_t last_ticket_{};
    uint64_t last_id_{};
};
} // namespace VR::Combat
