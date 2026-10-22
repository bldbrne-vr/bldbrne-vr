// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

// The pure logic of the GPU-pass optimizations (vr.ini perf_resync_memo / perf_depth_one_layout):
// no Vulkan, unit-tested in tests/test_vr_perf_logic.cpp. vr_perf.h holds the runtime switches.
#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace VideoCore::PerfLogic {

// --- What an image transition announces ---------------------------------------------------------
// VkAccessFlagBits2 values (fixed by the Vulkan spec; image.cpp static_asserts them): the accesses
// that only read. A transition to anything else may be followed by a write (a render target, a
// storage image, a copy, a clear, an upload), so it counts as one.
inline constexpr uint64_t ReadOnlyAccess =
    0x1ull |         // INDIRECT_COMMAND_READ
    0x2ull |         // INDEX_READ
    0x4ull |         // VERTEX_ATTRIBUTE_READ
    0x8ull |         // UNIFORM_READ
    0x10ull |        // INPUT_ATTACHMENT_READ
    0x20ull |        // SHADER_READ
    0x80ull |        // COLOR_ATTACHMENT_READ
    0x200ull |       // DEPTH_STENCIL_ATTACHMENT_READ
    0x800ull |       // TRANSFER_READ
    0x2000ull |      // HOST_READ
    0x8000ull |      // MEMORY_READ
    0x100000000ull | // SHADER_SAMPLED_READ
    0x200000000ull;  // SHADER_STORAGE_READ

// True when a transition to `access` may write the image. `from_undefined`: a transition out of
// VK_IMAGE_LAYOUT_UNDEFINED discards the contents, so it counts as a write too.
constexpr bool MayWrite(uint64_t access, bool from_undefined) {
    return from_undefined || (access & ~ReadOnlyAccess) != 0;
}

// --- The image -> buffer resync memo (point 3a) -------------------------------------------------
// A GPU-written tiled image read through a texel buffer is re-tiled into the buffer (an image
// download and a tiling dispatch) every time the buffer is bound. The memo remembers the last full
// resync: when the same image goes to the same buffer bytes again, and neither the image nor the
// buffer has been written since (their write counters unchanged, no global invalidation), the
// buffer still holds exactly those bytes and the resync is skipped.
struct ResyncKey {
    uint64_t image_uid = 0;     // the texture cache's unique id (never reused)
    uint64_t vk_image = 0;      // the image's backing (a new backing = new contents)
    uint64_t buffer_serial = 0; // the Buffer object's creation serial (never reused)
    uint64_t vk_buffer = 0;
    uint32_t buf_offset = 0;
    uint32_t copy_size = 0;
    uint32_t num_copies = 0;
    uint32_t tiled = 0;

    friend constexpr bool operator==(const ResyncKey&, const ResyncKey&) = default;
};

// The write counters a memo is checked against.
struct ResyncSeqs {
    uint64_t image = 0;  // the image's write counter (every transition that may write it)
    uint64_t buffer = 0; // the buffer's write counter (uploads, shader/copy/fill writes, resyncs)
    uint64_t global = 0; // toggles and raw-memory (DMA) shaders invalidate every memo

    friend constexpr bool operator==(const ResyncSeqs&, const ResyncSeqs&) = default;
};

struct ResyncMemo {
    ResyncKey key{};
    ResyncSeqs seqs{};
    bool valid = false;
};

// The resync may be skipped: the memo is for this exact target and nothing was written since.
constexpr bool CanSkip(const ResyncMemo& memo, const ResyncKey& key, const ResyncSeqs& now) {
    return memo.valid && memo.key == key && memo.seqs == now;
}

// After a full resync. `write_bound_this_draw`: the image or the buffer is bound for writing (or a
// raw-memory shader runs) in the draw whose bindings are being made: its shader writes come after
// this resync on the GPU although their counters moved before it, so no memo is kept then.
constexpr void Store(ResyncMemo& memo, const ResyncKey& key, const ResyncSeqs& now,
                     bool write_bound_this_draw) {
    memo.key = key;
    memo.seqs = now;
    memo.valid = !write_bound_this_draw;
}

// --- The depth target's view and layout (perf_depth_one_layout) --------------------------------
// The game turns depth writes on and off between draws into the same depth target (the hall: ~58
// times per frame). Today each state has its own view (the description's is_storage = "writes
// depth") and layout (attachment vs read-only), so every toggle ends the render pass and
// transitions the image. With the switch on, a draw with depth writes off that does not sample
// the depth, into a depth target that is in the writable layout now, keeps the write view and
// that layout; the writes stay off through the dynamic depth-write enable and the stencil write
// masks (a writable layout never adds writes). Everything else is today's choice: a sampled depth
// keeps today's read-only layout (its descriptor is written with it), and a depth already
// read-only stays read-only, so the rule never adds a view or layout change (test: every draw
// sequence up to length 8).
constexpr bool KeepDepthWritable(bool writes_depth, bool one_layout, bool sampled,
                                 bool writable_now) {
    return !writes_depth && one_layout && !sampled && writable_now;
}

enum class DepthLayout : uint32_t {
    DepthStencilAttachment,         // VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
    DepthAttachment,                // VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL
    DepthReadOnlyStencilAttachment, // VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_STENCIL_ATTACHMENT_OPTIMAL
    DepthStencilReadOnly,           // VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
    DepthReadOnly,                  // VK_IMAGE_LAYOUT_DEPTH_READ_ONLY_OPTIMAL
};

// The depth attachment's layout (Rasterizer::BeginRendering): `writable` = the draw writes depth
// or KeepDepthWritable; `stencil_enable` = the game's stencil test is on (stencil writes may
// happen while depth writes are off).
constexpr DepthLayout DepthAttachmentLayout(bool writable, bool has_stencil, bool stencil_enable) {
    if (writable) {
        return has_stencil ? DepthLayout::DepthStencilAttachment : DepthLayout::DepthAttachment;
    }
    if (has_stencil && stencil_enable) {
        return DepthLayout::DepthReadOnlyStencilAttachment;
    }
    return has_stencil ? DepthLayout::DepthStencilReadOnly : DepthLayout::DepthReadOnly;
}

// --- A live switch ------------------------------------------------------------------------------
// The value a switch has this frame; Update() says whether it changed (the caller then
// invalidates every memo).
struct Switch {
    bool value = false;
    bool known = false;

    bool Update(bool v) {
        const bool changed = known && v != value;
        value = v;
        known = true;
        return changed;
    }
};

// --- The shadow check's comparison --------------------------------------------------------------
struct Compare {
    std::size_t first = 0;      // the first differing byte (valid when bytes > 0)
    std::size_t bytes = 0;      // how many bytes differ
};

inline Compare CompareBytes(std::span<const uint8_t> a, std::span<const uint8_t> b) {
    Compare c{};
    const std::size_t n = std::min(a.size(), b.size());
    for (std::size_t i = 0; i < n; ++i) {
        if (a[i] != b[i]) {
            if (c.bytes == 0) {
                c.first = i;
            }
            ++c.bytes;
        }
    }
    if (a.size() != b.size()) {
        if (c.bytes == 0) {
            c.first = n;
        }
        c.bytes += std::max(a.size(), b.size()) - n;
    }
    return c;
}

} // namespace VideoCore::PerfLogic
