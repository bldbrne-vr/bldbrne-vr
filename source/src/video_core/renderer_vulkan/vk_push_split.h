// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstring>

#include "common/types.h"

namespace Vulkan {

/// FAST_CPU_PUSH_SPLIT (CommandRecorder::PushSplit): the push constant block the command buffer
/// holds from PushSplit's packets, and what of the next block it still needs. Every native
/// graphics layout has the one range 0..size for the same stages, so a block pushed for one
/// draw is still there for the next whatever pipelines are bound between them, until anything
/// else pushes, records raw, or a new command buffer starts (Forget). Producer-owned.
struct HeldPush {
    enum class Encode : u8 {
        Whole, // the whole block; the command buffer holds it from this packet on
        Head,  // bytes 0..split only: it holds the same tail
        None,  // nothing: it holds all of it
    };
    static constexpr u32 MaxBytes = 256;

    u32 stages{}, split{}, size{};
    bool valid{};
    std::array<std::byte, MaxBytes> bytes{};

    /// A block this can hold: split inside it, and it fits.
    static constexpr bool Fits(u32 split, u32 size) noexcept {
        return split != 0 && split < size && size <= MaxBytes;
    }
    /// What the command buffer needs encoded for `values` (Fits), without changing anything.
    Encode Peek(u32 stages_, u32 split_, u32 size_, const std::byte* values) const noexcept {
        if (!valid || stages != stages_ || split != split_ || size != size_ ||
            std::memcmp(bytes.data() + split, values + split, size - split) != 0) {
            return Encode::Whole;
        }
        return std::memcmp(bytes.data(), values, split) == 0 ? Encode::None : Encode::Head;
    }
    /// Peek, then holds `values`: the caller encodes what this returns.
    Encode Next(u32 stages_, u32 split_, u32 size_, const std::byte* values) noexcept {
        const Encode encode = Peek(stages_, split_, size_, values);
        if (encode == Encode::Head) {
            std::memcpy(bytes.data(), values, split);
        } else if (encode == Encode::Whole) {
            stages = stages_;
            split = split_;
            size = size_;
            std::memcpy(bytes.data(), values, size);
            valid = true;
        }
        return encode;
    }
    /// Anything else that may change what the command buffer holds.
    void Forget() noexcept {
        valid = false;
    }
};

} // namespace Vulkan
