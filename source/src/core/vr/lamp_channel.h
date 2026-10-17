// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "core/vr/interaction_lamp_channel.h"
#include "core/vr/lamp_travel.h"

namespace VR::Lamps {
inline std::atomic<bool> enabled{
    true}; // On by default (the user, 2026-10-13); vr.ini lamp_travel=0 turns it off.
inline bool Enabled() {
    return enabled.load(std::memory_order_acquire);
}
inline void SetEnabled(bool value) {
    enabled.store(value, std::memory_order_release);
}
inline std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
// The host owns copied UI payloads, the mutex and pad application. The DLL owns
// mailbox timing, request validity/deduplication, presentation and receipt state.
class Channel {
    std::mutex mutex;
    std::optional<Offer> offered;
    VR::Interactions::LampChannelRuntime policy;
    VR::Interactions::Module* module{};
    bool initialized{}, fault{};

    // Every call is serialized by the original mailbox mutex, including the
    // application/ack pair. Retain the loaded module while guest hooks can call us.
    void Initialize() {
        if (initialized)
            return;
        initialized = true;
        if (!VR::Interactions::DllRequested())
            return;
        module = new VR::Interactions::Module();
        module->EnableFaultBroadcast();
        policy.Bind(*module, fault);
        if (!module->LoadBesideExecutable()) {
            fault = true;
            module->ReportFault();
            std::fprintf(stderr, "VR lamp mailbox: %s\n", module->Diagnostic().c_str());
        }
    }
    PiLampChannelOutput Call(PiLampChannelInput p) {
        Initialize();
        p.now_ns = NowNs();
        p.enabled = Enabled();
        return policy.Call(p);
    }
    PiLampChannelOutput Call(uint32_t command, uint64_t serial = 0) {
        PiLampChannelInput p{};
        p.command = command;
        p.serial = serial;
        return Call(p);
    }

public:
    void Publish(Offer value) {
        std::scoped_lock lock{mutex};
        Initialize();
        PiLampChannelInput p{};
        p.command = PI_LCH_PUBLISH;
        p.serial = value.serial;
        p.current = value.current;
        for (const auto& page : value.pages)
            for (int id : page.destinations) {
                if (p.destination_count == PI_LCH_MAX_DESTINATIONS) {
                    policy.ReportFault(); // Never silently drop an offered destination.
                    offered.reset();
                    return;
                }
                p.destinations[p.destination_count++] = id;
            }
        if (Call(p).accepted)
            offered = std::move(value);
    }
    std::optional<Offer> TakeOffer() {
        std::scoped_lock lock{mutex};
        if (!Call(PI_LCH_TAKE_OFFER).accepted)
            return {};
        return offered;
    }
    void Present(std::uint64_t serial) {
        std::scoped_lock lock{mutex};
        Call(PI_LCH_PRESENT, serial);
    }
    bool Presented(std::uint64_t serial) {
        std::scoped_lock lock{mutex};
        return Call(PI_LCH_PRESENTED, serial).accepted;
    }
    bool Submit(Request request) {
        std::scoped_lock lock{mutex};
        PiLampChannelInput p{};
        p.command = PI_LCH_SUBMIT;
        p.serial = request.serial;
        p.action = static_cast<int32_t>(request.action);
        p.destination = request.destination;
        return Call(p).accepted;
    }
    std::optional<Request> TakeRequest() {
        std::scoped_lock lock{mutex};
        const auto out = Call(PI_LCH_TAKE_REQUEST);
        if (!out.accepted)
            return {};
        return Request{out.request_serial, static_cast<Action>(out.action), out.destination};
    }
    void Cancel(std::uint64_t serial = 0) {
        std::scoped_lock lock{mutex};
        if (Call(PI_LCH_CANCEL, serial).accepted || policy.Faulted())
            offered.reset();
    }
    bool Active(std::uint64_t serial) {
        std::scoped_lock lock{mutex};
        return Call(PI_LCH_ACTIVE, serial).accepted;
    }
    void Output(Pad value, bool new_command = false) {
        std::scoped_lock lock{mutex};
        PiLampChannelInput p{};
        p.command = PI_LCH_OUTPUT;
        p.pad = static_cast<uint32_t>(value);
        p.new_command = new_command;
        Call(p);
    }
    std::optional<PadReceipt> Delivery() {
        std::scoped_lock lock{mutex};
        const auto out = Call(PI_LCH_DELIVERY);
        if (!out.accepted || !out.has_receipt)
            return {};
        return PadReceipt{static_cast<Pad>(out.pad), out.receipt_ns};
    }
    // Cancel/Output cannot race a stale button onto the controller. The callback
    // only applies the approved pad; it must not call this mailbox.
    template <class Apply>
    bool ApplyInput(Apply&& apply) {
        std::scoped_lock lock{mutex};
        const auto out = Call(PI_LCH_INPUT);
        if (!out.accepted)
            return false;
        apply(static_cast<Pad>(out.pad));
        if (!out.has_receipt) {
            PiLampChannelInput p{};
            p.command = PI_LCH_ACK;
            p.pad = out.pad;
            Call(p); // Timestamp after actual application, preserving the first receipt.
        }
        return true; // The pad was applied even if the receipt transport then failed.
    }
    std::optional<Pad> Input() {
        std::scoped_lock lock{mutex};
        const auto out = Call(PI_LCH_INPUT);
        if (!out.accepted)
            return {};
        return static_cast<Pad>(out.pad);
    }
};
inline Channel& Mailbox() {
    static Channel channel;
    return channel;
}
} // namespace VR::Lamps
