// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>

namespace VR::SoftSave {
using Steady = std::chrono::steady_clock;
// A soft save's time as the page shows it, from local times ("Oct 31, 4:33 PM"; "Oct 31 2025,
// 4:33 PM" when it is not this year's).
inline std::string SavedAt(const std::tm& made, const std::tm& now) {
    static constexpr std::array<const char*, 12> Months{"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                                        "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    const char* const month = Months[static_cast<std::size_t>(std::clamp(made.tm_mon, 0, 11))];
    const int hour = made.tm_hour % 12 == 0 ? 12 : made.tm_hour % 12;
    const char* const half = made.tm_hour < 12 ? "AM" : "PM";
    char text[48];
    if (made.tm_year == now.tm_year)
        std::snprintf(text, sizeof(text), "%s %d, %d:%02d %s", month, made.tm_mday, hour,
                      made.tm_min, half);
    else
        std::snprintf(text, sizeof(text), "%s %d %d, %d:%02d %s", month, made.tm_mday,
                      made.tm_year + 1900, hour, made.tm_min, half);
    return text;
}
enum class Command { None, Save, Restore, Clear };
class Channel {
    mutable std::mutex mutex;
    bool supported{}, busy{}, snapshot{}, taken{};
    std::uint64_t serial{}, offer{};
    Command command{Command::None};
    std::string status{"Soft save is unavailable in this session."};
    std::string when; // the snapshot's time (SavedAt), empty without one
    bool owns_pad{}, confirm{}, allow_back{};
    Steady::time_point heartbeat{}, delivered{};

public:
    void Publish(bool usable, bool working, bool saved, const std::string& message,
                 const std::string& saved_at = {}) {
        std::scoped_lock lock(mutex);
        supported = usable;
        busy = working;
        snapshot = saved;
        status = message;
        when = saved ? saved_at : std::string{};
        if (!usable || !saved || working)
            offer = 0;
        // A queued click must not survive losing its originating game context.
        if (!usable || working || (!saved && command == Command::Restore))
            command = Command::None;
    }
    std::string Status() const {
        std::scoped_lock lock(mutex);
        // The UI may refresh before Game:Main consumes a click. Keep its acknowledgement
        // visible even if an idle Publish still contains the previous completion message.
        switch (command) {
        case Command::Save:
            return "Saving...";
        case Command::Clear:
            return "Clearing soft save...";
        case Command::Restore:
            return "Returning to soft save...";
        case Command::None:
            return status;
        }
        return status;
    }
    // The snapshot's time ("Oct 31, 4:33 PM"), empty without one or when it is unknown.
    std::string When() const {
        std::scoped_lock lock(mutex);
        return when;
    }
    bool Save() {
        std::scoped_lock lock(mutex);
        if (!supported || busy || offer || command != Command::None)
            return false;
        command = Command::Save;
        return true;
    }
    bool Clear() {
        std::scoped_lock lock(mutex);
        if (!supported || busy || !snapshot || offer || command != Command::None)
            return false;
        command = Command::Clear;
        return true;
    }
    Command TakeCommand() {
        std::scoped_lock lock(mutex);
        const auto out = command;
        command = Command::None;
        return out;
    }
    void Death() {
        std::scoped_lock lock(mutex);
        if (!supported || busy || !snapshot || offer || command != Command::None)
            return;
        offer = ++serial;
        taken = false;
    }
    std::optional<std::uint64_t> TakeOffer() {
        std::scoped_lock lock(mutex);
        if (!offer || taken)
            return {};
        taken = true;
        return offer;
    }
    bool Active(std::uint64_t value) const {
        std::scoped_lock lock(mutex);
        return value && value == offer;
    }
    bool Prompt() const {
        std::scoped_lock lock(mutex);
        return offer != 0;
    }
    bool Choose(std::uint64_t value, bool restore) {
        std::scoped_lock lock(mutex);
        if (!value || value != offer)
            return false;
        offer = 0;
        if (supported && snapshot && !busy && command == Command::None)
            command = restore ? Command::Restore : Command::Clear;
        return true;
    }
    void Cancel() {
        std::scoped_lock lock(mutex);
        offer = 0;
        command = Command::None;
        owns_pad = false;
        allow_back = false;
        delivered = {};
    }
    // Recovery passes only the user's Back button, so an error dialog can be dismissed
    // without allowing a second load/character selection to race the protected load.
    void Pad(bool active, bool pressed = false, bool back = false) {
        std::scoped_lock lock(mutex);
        const auto now = Steady::now();
        // Renewing after a stalled callback needs a fresh delivery acknowledgement,
        // even when the requested button state has not changed.
        if (owns_pad != active || confirm != pressed || allow_back != back ||
            now - heartbeat > std::chrono::seconds(1))
            delivered = {};
        owns_pad = active;
        confirm = pressed;
        allow_back = back;
        heartbeat = now;
    }
    Steady::time_point Delivered() const {
        std::scoped_lock lock(mutex);
        return delivered;
    }
    template <class Apply>
    bool ApplyInput(Apply&& apply) {
        std::scoped_lock lock(mutex);
        if (!owns_pad || Steady::now() - heartbeat > std::chrono::seconds(1)) {
            delivered = {};
            return false;
        }
        apply(confirm, allow_back);
        if (delivered == Steady::time_point{})
            delivered = Steady::now();
        return true;
    }
};
inline Channel& Mailbox() {
    static Channel channel;
    return channel;
}
} // namespace VR::SoftSave
