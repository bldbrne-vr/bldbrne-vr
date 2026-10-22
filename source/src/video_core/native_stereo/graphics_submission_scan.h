// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Diagnostic bridge toward validated speculative draw preparation.
#pragma once
#include "graphics_packet_replay.h"
#include "register_delta.h"
#include <xxhash.h>
#include <atomic>
#include <memory>
#include <vector>

namespace ScratchDraw {
inline uint64_t HashRegisterWords(std::span<const uint32_t> words) {
    return XXH3_64bits(words.data(), words.size_bytes());
}
struct GraphicsSubmissionScan {
    // Immutable after Build; no worker reads the guest command buffer.
    std::vector<u32> commands;
    std::vector<size_t> offsets;
    std::vector<uint64_t> checksums;
    // One writer publishes a contiguous prefix. Published entries never change.
    std::atomic<size_t> published{};
    std::atomic<bool> finished{};
    std::atomic<bool> cancelled{};
    static std::shared_ptr<GraphicsSubmissionScan> Build(std::span<const u32> source) {
        auto result = std::make_shared<GraphicsSubmissionScan>();
        result->commands.assign(source.begin(), source.end());
        for (size_t offset = 0; offset < source.size();) {
            const auto* header = reinterpret_cast<const AmdGpu::PM4Header*>(result->commands.data() + offset);
            if (header->type == 2) { ++offset; continue; }
            if (header->type != 3) break;
            const size_t words = header->type3.NumWords() + 1;
            if (words < 2) break;
            if (words > source.size() - offset) break;
            const auto opcode = header->type3.opcode;
            if (opcode == AmdGpu::PM4ItOpcode::DrawIndex2 ||
                opcode == AmdGpu::PM4ItOpcode::DrawIndexOffset2 ||
                opcode == AmdGpu::PM4ItOpcode::DrawIndexAuto) result->offsets.push_back(offset);
            offset += words;
        }
        result->checksums.resize(result->offsets.size());
        return result;
    }
    void Run(AmdGpu::Regs& regs) {
        RegisterDeltaTracker<AmdGpu::Regs::NumRegs> tracker;
        size_t draw = 0;
        for (size_t offset = 0; offset < commands.size() && !cancelled.load(std::memory_order_relaxed);) {
            size_t words;
            if (!ReplayGraphicsPacket(regs, std::span<const u32>(commands).subspan(offset), words,
                                       [&](size_t first, size_t count) { tracker.Mark(first, count); })) break;
            if (draw < offsets.size() && offsets[draw] == offset) {
                checksums[draw] = tracker.Checksum(regs.reg_array, HashRegisterWords);
                published.store(++draw, std::memory_order_release);
            }
            offset += words;
        }
        finished.store(true, std::memory_order_release);
    }
    enum class Result { Ready, Pending, Unavailable, Stale };
    Result Compare(size_t offset, uint64_t actual) const {
        const auto it = std::lower_bound(offsets.begin(), offsets.end(), offset);
        if (it == offsets.end() || *it != offset) return Result::Unavailable;
        const size_t index = it - offsets.begin();
        // Read completion first: a finished scan may have published its final entry
        // between an earlier published load and the completion observation.
        const bool complete = finished.load(std::memory_order_acquire);
        if (index >= published.load(std::memory_order_acquire))
            return complete ? Result::Unavailable : Result::Pending;
        return checksums[index] == actual ? Result::Ready : Result::Stale;
    }
};
}
