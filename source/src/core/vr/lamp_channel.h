// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <atomic>
#include <chrono>
#include <mutex>
#include "core/vr/lamp_travel.h"

namespace VR::Lamps {
inline std::atomic<bool> enabled{true}; // On by default (2026-10-13); vr.ini lamp_travel=0 turns it off.
inline bool Enabled() { return enabled.load(std::memory_order_acquire); }
inline void SetEnabled(bool value) { enabled.store(value, std::memory_order_release); }
inline std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
// Site A owns the native state; the presenter owns the page. This mailbox never contains pointers.
class Channel {
    std::mutex mutex;
    std::optional<Offer> offered;
    std::optional<Request> requested;
    std::uint64_t active{};
    bool taken{}, submitted{}, presented{};
    Pad pad{Pad::Neutral};
    std::int64_t heartbeat{};
    std::optional<PadReceipt> receipt;

  public:
    void Publish(Offer value) {
        std::scoped_lock lock{mutex};
        active = value.serial;
        offered = std::move(value);
        requested.reset();
        taken = submitted = presented = false;
        pad = Pad::Neutral;
        heartbeat = NowNs();
        receipt.reset();
    }
    std::optional<Offer> TakeOffer() {
        std::scoped_lock lock{mutex};
        if (!active || taken || NowNs() - heartbeat > 1'000'000'000)
            return {};
        taken = true;
        return offered;
    }
    void Present(std::uint64_t serial) {
        std::scoped_lock lock{mutex};
        if (serial == active && taken)
            presented = true;
    }
    bool Presented(std::uint64_t serial) {
        std::scoped_lock lock{mutex};
        return serial == active && presented;
    }
    bool Submit(Request request) {
        std::scoped_lock lock{mutex};
        if (!Enabled() || !offered || !presented || submitted || active != request.serial ||
            !ValidRequest(*offered, request) || NowNs() - heartbeat > 1'000'000'000)
            return false;
        submitted = true;
        requested = request;
        return true;
    }
    std::optional<Request> TakeRequest() {
        std::scoped_lock lock{mutex};
        auto result = requested;
        requested.reset();
        return result;
    }
    void Cancel(std::uint64_t serial = 0) {
        std::scoped_lock lock{mutex};
        if (serial && serial != active)
            return;
        receipt.reset();
        active = 0;
        offered.reset();
        requested.reset();
        pad = Pad::Neutral;
    }
    bool Active(std::uint64_t serial) {
        std::scoped_lock lock{mutex};
        return serial && serial == active && Enabled() && NowNs() - heartbeat < 1'000'000'000;
    }
    void Output(Pad value, bool new_command = false) {
        std::scoped_lock lock{mutex};
        if (pad != value || new_command)
            receipt.reset();
        pad = value;
        heartbeat = NowNs();
    }
    std::optional<PadReceipt> Delivery() {
        std::scoped_lock lock{mutex};
        return receipt;
    }
    // Application and acknowledgement are one operation: Cancel/Output cannot race a stale
    // button onto the controller. The callback only applies a pad; it must not call this mailbox.
    template <class Apply> bool ApplyInput(Apply&& apply) {
        std::scoped_lock lock{mutex};
        if (!active || !Enabled() || NowNs() - heartbeat > 1'000'000'000)
            return false;
        apply(pad);
        if (!receipt)
            receipt = PadReceipt{pad, NowNs()};
        return true;
    }
    // No heartbeat (load, lost player, crash): input automatically returns to normal.
    std::optional<Pad> Input() {
        std::scoped_lock lock{mutex};
        if (!active || !Enabled() || NowNs() - heartbeat > 1'000'000'000)
            return {};
        return pad;
    }
};
inline Channel& Mailbox() {
    static Channel channel;
    return channel;
}
} // namespace VR::Lamps
