// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstdint>
#include <optional>

namespace VideoCore::UploadStaging {

// A block may append while its earlier uploads are in flight, but may rewind only
// after its last GPU submission completes. Completion also covers host-copy workers:
// the scheduler waits for their prefix before submitting the corresponding GPU work.
struct BlockState {
    std::uint64_t capacity{};
    std::uint64_t end{};
    std::uint64_t tick{};
    std::uint64_t last_frame{};
    std::uint64_t reservations{};

    std::optional<std::uint64_t> Append(std::uint64_t size, std::uint64_t alignment,
                                        std::uint64_t current_tick, std::uint64_t frame) {
        if (size == 0 || alignment == 0 || (alignment & (alignment - 1)) != 0 || end > capacity) {
            return {};
        }
        const auto padding = (alignment - (end & (alignment - 1))) & (alignment - 1);
        if (padding > capacity - end || size > capacity - end - padding) {
            return {};
        }
        const auto offset = end + padding;
        end = offset + size;
        tick = current_tick;
        last_frame = frame;
        ++reservations;
        return offset;
    }

    void Commit(std::uint64_t current_tick) {
        // A synchronous source read can submit work (e.g. a GPU-dirty readback).
        // Retire against the tick that will consume the bytes, after that read.
        if (current_tick > tick) {
            tick = current_tick;
        }
        --reservations;
    }

    bool Rewind(bool completed) {
        if (!completed || reservations != 0) {
            return false;
        }
        end = 0;
        return true;
    }

    bool CanTrim(std::uint64_t frame, bool completed) const {
        return reservations == 0 && completed && frame >= last_frame && frame - last_frame >= 3000;
    }
};

} // namespace VideoCore::UploadStaging
