// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <span>

namespace Libraries::VideoOut {
// Stored after the PatchedFlip marker, inside its reserved NOP payload. No pointers
// or separately queued callbacks: copying/submitting the packet carries its identity.
struct FlipPacket {
    std::uint32_t buffer;
    std::uint32_t handle;
    std::int64_t argument;
    std::uint64_t ticket;
    bool operator==(const FlipPacket&) const = default;
};
inline constexpr std::uint32_t FlipPacketVersion = 0x46504b31; // FPK1
inline constexpr std::size_t FlipPacketWords = 7;

inline std::array<std::uint32_t, FlipPacketWords> EncodeFlip(const FlipPacket& flip) {
    const auto argument = std::bit_cast<std::uint64_t>(flip.argument);
    return {flip.buffer,
            FlipPacketVersion,
            flip.handle,
            static_cast<std::uint32_t>(argument),
            static_cast<std::uint32_t>(argument >> 32),
            static_cast<std::uint32_t>(flip.ticket),
            static_cast<std::uint32_t>(flip.ticket >> 32)};
}
inline std::optional<FlipPacket> DecodeFlip(std::span<const std::uint32_t> words) {
    if (words.size() < FlipPacketWords || words[1] != FlipPacketVersion)
        return std::nullopt;
    const auto join = [](std::uint32_t lo, std::uint32_t hi) {
        return std::uint64_t{lo} | (std::uint64_t{hi} << 32);
    };
    return FlipPacket{words[0], words[2], std::bit_cast<std::int64_t>(join(words[3], words[4])),
                      join(words[5], words[6])};
}
} // namespace Libraries::VideoOut
