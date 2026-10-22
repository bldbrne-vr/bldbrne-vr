// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

// Render-pass breaks: what ends the game's dynamic-rendering passes, and toggle bit 43, which keeps
// a pass open where ending it and beginning the next one could not change any attachment.
//
// Bit 43 (PassContinue), parts (FAST_PASS_CONTINUE_PARTS, a mask, default 3):
//  1 Consumed clears (Scheduler::BeginRendering). A pass begun with a clear (a fast-cleared CMASK
//    or HTILE, a depth-clear draw) is followed by draws whose state no longer asks for the clear
//    (TouchMeta consumed it): memcmp sees a different RenderState and ends the pass, and the next
//    one begins with LOAD on the same views, layouts, area, layers and view mask. vkCmdEndRendering
//    stores every attachment (storeOp STORE) and nothing is recorded between that end and the
//    next begin, so the load reads exactly what the store wrote: continuing the open pass gives
//    the same memory, bit for bit. A clear that is asked for again, or with another value, still
//    ends the pass. Clear values of attachments that load are ignored (LOAD never reads them).
//  2 Subset passes (needs VK_EXT_dynamic_rendering_unused_attachments, enabled when the device
//    has it). A draw whose colour attachments are a prefix of the open pass's (same views,
//    layouts, depth, area, layers, view mask) continues it: with dynamicRenderingUnusedAttachments
//    a pipeline may declare fewer colour attachments than the render pass instance (VUID-vkCmdDraw-
//    colorAttachmentCount-06179 applies only with the feature off) and an attachment the pipeline
//    does not declare is not written (the extension's "unused attachment" rule). The split pass
//    would not contain those attachments at all, so they keep the same bytes either way. In the
//    temporal modes this keeps the object-motion slot 7 (RGBA32F) bound once the first motion draw
//    of a G-buffer pass added it: the 6-target draws between characters no longer end the pass,
//    and the next motion draw continues it again (its slot-7 clear is consumed, part 1).
//  4 Retired from this mask: read-only buffer barriers are FAST_GPU_READ_BARRIERS below (the mask
//    bit does nothing).
// Off: the scheduler compares RenderStates with memcmp.
//
// FAST_GPU_READ_BARRIERS=1 (off by default; GPU synchronization only, read once at start-up): the
// old part 4, reconciled with bit 44. After anything else, the first read of a buffer gets one
// barrier from every earlier command and write to every read access at every stage, and later
// reads of any kind need none until the next write request: the old code issued a barrier, and so
// ended the open pass (Pipeline::BindResources), each time the read kind changed (vertex input,
// index, storage, indirect). Never more barriers than before on the same requests, except for a
// read excluded from it after that state (a buffer that came to hold image data, a copy's source).
// The logic, the argument and the exclusions: buffer_cache/read_barrier_logic.h; the step:
// Buffer::GetBarrierReads (buffer.cpp); bits 33/44 (BufferCache::ObtainSmallDirect) bind a small
// shader read in place in that state too. Off: Buffer::GetBarrier is the old code. Check it with
// the Khronos synchronization validation before making it a default.
// FAST_GPU_READ_BARRIERS_VERIFY=1 (turns the switch on): the old step runs too, on its own copy of
// each buffer's state; the barrier recorded does what both do (merged), so the GPU gets at least
// the old synchronization; every request is compared (ReadBarrierLogic::Check): no barrier where
// the old one publishes a write, a weaker barrier, or a diverged state logs an error. A
// 'FAST read_barriers:' line every 65536 requests either way.
//
// FAST_PASS_CONTINUE_VERIFY=N: every Nth continued pass is re-derived independently (both states
// normalized: the consumed clears and the subset's extra attachments zeroed, then memcmp); a
// mismatch logs an error and the pass is ended and begun the old way.
//
// FAST_PASS_BREAK_STATS=1 (any mask): every ~2 s a 'FAST pass_breaks:' line on the game's
// scheduler: passes begun and ended per game frame, state-change breaks by first difference,
// explicit ends by call site (EndRendering's std::source_location) with how many of them were
// followed by a pass with the same state (a "restart": the end was only for that operation, the
// candidates for hoisting), passes ended with 4+ colour targets (the G-buffer), BindResources
// barrier breaks whose barriers were all read-after-read (FAST_GPU_READ_BARRIERS' opportunity),
// and bit 43's continues. Off: one predictable branch per pass end.
#pragma once

#include <atomic>
#include <cstdlib>
#include "common/types.h"
#include "video_core/fast_render.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

static constexpr auto PassContinue = static_cast<FastRender::Bit>(43);

namespace PassBreaks {

enum Part : u32 { ConsumedClears = 1, SubsetPasses = 2, ReadBarriers = 4 };

inline u32 Parts() {
    static const u32 parts = [] {
        const char* value = std::getenv("FAST_PASS_CONTINUE_PARTS");
        return value ? static_cast<u32>(std::strtoul(value, nullptr, 0)) : u32{3};
    }();
    return parts;
}

/// Read once per decision: the bit may change between two passes (or two barriers).
inline bool PartOn(Part part) {
    return (Parts() & part) != 0 && FastRender::On(PassContinue);
}

// Read once at start-up (namespace scope: Buffer::GetBarrier tests it per barrier).
inline const bool g_stats_enabled = [] {
    const char* value = std::getenv("FAST_PASS_BREAK_STATS");
    return value != nullptr && value[0] == '1';
}();

inline bool StatsEnabled() {
    return g_stats_enabled;
}

// FAST_GPU_READ_BARRIERS / FAST_GPU_READ_BARRIERS_VERIFY (above), read once at start-up.
inline const bool g_read_barriers_verify = [] {
    const char* value = std::getenv("FAST_GPU_READ_BARRIERS_VERIFY");
    return value != nullptr && value[0] == '1';
}();
inline const bool g_read_barriers = [] {
    const char* value = std::getenv("FAST_GPU_READ_BARRIERS");
    const char* verify = std::getenv("FAST_GPU_READ_BARRIERS_VERIFY");
    return (value != nullptr && value[0] == '1') || (verify != nullptr && verify[0] == '1');
}();

inline u64 VerifyEvery() {
    static const u64 every = [] {
        const char* value = std::getenv("FAST_PASS_CONTINUE_VERIFY");
        return value ? static_cast<u64>(std::strtoull(value, nullptr, 0)) : u64{0};
    }();
    return every;
}

/// Every read access a buffer is bound for (FAST_GPU_READ_BARRIERS' "visible to all reads" state,
/// ReadBarrierLogic::AnyRead).
inline constexpr vk::AccessFlags2 AnyRead =
    vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eShaderRead |
    vk::AccessFlagBits2::eUniformRead | vk::AccessFlagBits2::eVertexAttributeRead |
    vk::AccessFlagBits2::eIndexRead | vk::AccessFlagBits2::eIndirectCommandRead |
    vk::AccessFlagBits2::eTransferRead;

/// A non-empty access mask with read bits only.
inline bool IsPureRead(vk::AccessFlags2 access) {
    return static_cast<bool>(access) && (access | AnyRead) == AnyRead;
}

/// Buffer::GetBarrier counters (any thread that records; relaxed, statistics only).
struct BarrierCounters {
    std::atomic<u64> elided{};    // FAST_GPU_READ_BARRIERS: reads that needed no barrier
    std::atomic<u64> broadened{}; // FAST_GPU_READ_BARRIERS: barriers to every read access
    // Read -> read barriers: recorded with FAST_GPU_READ_BARRIERS off and FAST_PASS_BREAK_STATS=1
    // (the opportunity); with FAST_GPU_READ_BARRIERS_VERIFY, the old step's where the new one has
    // no barrier (what the switch saves).
    std::atomic<u64> read_after_read{};
};
inline BarrierCounters g_barriers;

inline void Count(std::atomic<u64>& counter) {
    counter.fetch_add(1, std::memory_order_relaxed);
}

} // namespace PassBreaks
} // namespace Vulkan
