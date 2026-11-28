// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string_view>

namespace VR::CutsceneFrameLimit {

// Verified against 1.09 events 13201803 and 13404825. The latter is Ludwig's
// phase change, not his entrance (34000020) or Laurence's scene (34000010).
constexpr std::uint32_t Rom = 32000000, Ludwig = 34000030;
constexpr std::uint32_t PlayingFlag = 9180;
constexpr std::uint64_t FlagManagerRva = 0x553B100;
constexpr std::int64_t MaxHoldNs = 120'000'000'000;
constexpr std::int64_t RomWarpTimeoutNs = 45'000'000'000;
constexpr std::uint32_t RomDestinationMap = 28u << 24; // m28_00, respawn point 2802958

constexpr std::uint32_t SceneOf(std::string_view path) {
    if (path == "/app0/dvdroot_ps4/remo/s32_00_0000.remobnd.dcx") return Rom;
    if (path == "/app0/dvdroot_ps4/remo/s34_00_0030.remobnd.dcx") return Ludwig;
    return 0;
}

struct Hold {
    std::uint32_t scene = 0;
    std::int64_t began_ns = 0;
    bool awaiting_rom_arrival = false;
    bool saw_other_map = false;

    void Open(std::uint32_t id, bool playing, std::int64_t now, std::uint32_t hunter_map = 0) {
        if (id != Rom && id != Ludwig) return;
        // A preload or repeat open cannot cancel an armed Rom warp or extend its deadline.
        if (scene == Rom && (!playing || id == Rom)) return;
        if (!playing) {
            scene = 0;
        } else if (scene != id) {
            scene = id;
            began_ns = now;
            awaiting_rom_arrival = false;
            saw_other_map = id == Rom && hunter_map != RomDestinationMap;
        }
    }

    // Returns a reason only when releasing an active cap.
    const char* Tick(std::optional<bool> playing, std::int64_t now, std::uint32_t hunter_map = 0) {
        if (!scene) return nullptr;
        const char* reason = nullptr;
        const auto timeout = awaiting_rom_arrival ? RomWarpTimeoutNs : MaxHoldNs;
        if (now < began_ns || now - began_ns >= timeout) {
            reason = awaiting_rom_arrival ? "Rom warp timeout" : "safety timeout";
        } else if (scene == Rom) {
            // The published id comes from the hunter. Observing it change rules out
            // a stale destination id left over from an earlier visit.
            saw_other_map |= hunter_map != RomDestinationMap;
            if (!playing.value_or(false) && saw_other_map && hunter_map == RomDestinationMap) {
                reason = "hunter arrived in Yahar'gul";
            } else if (!awaiting_rom_arrival && playing == false) {
                // Event 13201803 clears 9180 before requesting the warp. From here
                // only the hunter's map and a separate wall-clock deadline matter.
                awaiting_rom_arrival = true;
                began_ns = now;
            }
            // An unreadable flag can be transient. Keep the current deadline.
        } else if (!playing.value_or(false)) {
            reason = playing.has_value() ? "event ended/skipped" : "event state unavailable";
        }
        if (reason) {
            scene = 0;
            awaiting_rom_arrival = false;
        }
        return reason;
    }
    [[nodiscard]] int Rate(int requested) const { return scene ? std::min(requested, 30) : requested; }
};

// A bounded, read-only equivalent of SprjEventFlagMan::GetEventFlag for one
// flag. Reader must perform fault-safe copies; never retain guest pointers.
// nullopt means unavailable/corrupt, not an unset flag. No allocations or calls
// into the guest, and at most 32 tree nodes even if a bad tree cycles.
template <class Reader>
std::optional<bool> ReadFlag(std::uint64_t image, std::uint32_t flag, Reader&& copy) {
    constexpr std::uint64_t Limit = 0x0000'8000'0000'0000ULL;
    const auto read = [&](std::uint64_t at, auto& value) {
        return at >= 0x10000 && at < Limit && sizeof(value) <= Limit - at &&
               copy(at, &value, sizeof(value));
    };
    const auto pointer = [](std::uint64_t p) {
        return p >= 0x10000 && p < Limit - 0x40 && (p & 7) == 0;
    };
    if (!pointer(image) || image >= Limit - FlagManagerRva) return std::nullopt;
    std::uint64_t manager{};
    if (!read(image + FlagManagerRva, manager) || !pointer(manager)) return std::nullopt;
    std::uint32_t block_size{}, stride{};
    std::uint64_t base{}, root{}, node{};
    if (!read(manager + 0x1c, block_size) || block_size == 0 ||
        !read(manager + 0x38, root) || !pointer(root) || !read(root + 8, node))
        return std::nullopt;
    const auto block = flag / block_size;
    for (unsigned depth = 0; depth < 32; ++depth) {
        if (!pointer(node)) return std::nullopt;
        std::uint8_t nil{};
        std::uint32_t key{}, kind{};
        if (!read(node + 0x19, nil)) return std::nullopt;
        if (nil) return false;
        if (!read(node + 0x20, key)) return std::nullopt;
        if (key != block) {
            if (!read(node + (key < block ? 0x10 : 0), node)) return std::nullopt;
            continue;
        }
        if (!read(node + 0x28, kind)) return std::nullopt;
        std::uint64_t data{};
        if (kind == 2) {
            if (!read(node + 0x30, data)) return std::nullopt;
        } else if (kind == 1) {
            std::uint32_t index{};
            if (!read(node + 0x30, index) || !read(manager + 0x20, stride) ||
                !read(manager + 0x28, base)) return std::nullopt;
            const std::uint64_t offset = std::uint64_t(stride) * index;
            // The game's multiply is 32-bit. Reject overflow rather than wrap it.
            if (offset > UINT32_MAX || base < 0x10000 || base >= Limit ||
                offset >= Limit - base)
                return std::nullopt;
            data = base + offset;
        } else {
            return std::nullopt;
        }
        const auto bit = flag % block_size;
        if (data < 0x10000 || data >= Limit || (bit >> 3) >= Limit - data)
            return std::nullopt;
        std::uint8_t byte{};
        if (!read(data + (bit >> 3), byte)) return std::nullopt;
        return (byte & (1u << (7 - (bit & 7)))) != 0;
    }
    return std::nullopt;
}
// Flag 9180: a watched scene is playing.
template <class Reader>
std::optional<bool> ReadPlaying(std::uint64_t image, Reader&& copy) {
    return ReadFlag(image, PlayingFlag, copy);
}
} // namespace VR::CutsceneFrameLimit
