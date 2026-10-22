// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <span>
#include <boost/container/small_vector.hpp>
#include "common/logging/log.h"
#include "common/types.h"
#include "video_core/fast_render.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

// FAST bit 53 (gpu_plan.md #2, "D1"): no barrier between the two eyes of a game dispatch.
// Native stereo records each guest dispatch twice: the left eye, a global ALL->ALL barrier, the
// right eye (vr_hud = 1 selects descriptor 1 of every pair), and a closing global barrier
// (DispatchDirectRecord / DispatchIndirectRecord). The pre-dispatch buffer barriers already name
// the right twins (PairBarriers) and image transitions cover both layers, so the middle barrier
// orders only the left dispatch against the right one. With the bit, it is left out when the two
// eyes cannot touch the same memory:
//  - every right-eye storage buffer descriptor, read or written, differs from every buffer the
//    left eye writes, and every left descriptor from every buffer the right eye writes (this also
//    covers a read binding paired before the written binding that created right(B), which keeps
//    the left B, and a written buffer without a twin); distinct VkBuffers never share memory here;
//  - every storage image descriptor (all of them count as written) has a right view other than
//    its left one: a registered native-stereo view, so the left eye touches layer 0 of that image
//    only and the right eye layer 1 only (every view of such an image is layer 0 + registered);
//  - only storage buffer, sampled image, storage image and sampler descriptors;
//  - no raw memory (uses_dma: BDA writes are not in the descriptors), no GDS, BDA page-table or
//    fault-buffer binding;
//  - indirect: the right eye's argument buffer is no buffer the left eye writes (and back);
//  - no draw since the last dispatch's closing barrier wrote buffers, raw memory or storage
//    images: a buffer write leaves Buffer::GetBarrier's state at ShaderWrite, so a dispatch that
//    writes the same buffer gets no pre-barrier, and an image bound both sampled and written takes
//    TransitTexture's force_general path, whose tracked access has no write flag, so a dispatch
//    binding it the same way gets no transition; today only the middle barrier orders the draw's
//    right-eye accesses before the right dispatch. Every other writer of a right twin or layer 1
//    (twin creation, NativeStereo::Copy, Image upload/download/copy/clear/resolve, scene proxy
//    fills, tile manager, Buffer::Fill, stream mirrors, HLE copies, dispatches) ends with a global
//    barrier or a tracked image transition whose state has a write flag. A new writer of a right
//    twin or layer 1 that ends with a barrier naming only the left resource (gpu_plan.md #11,
//    N1) would break this argument: audit it against this list.
// The closing barrier stays: it is the only publication of the right eye's writes. Anything not
// proven falls back to the middle barrier, counted by reason. The decision is made per dispatch
// from that dispatch's own bindings, so the bit may flip between any two dispatches (frames).
// Bit off: exactly the old command stream (only a CPU flag is kept).
static constexpr auto DispatchEyeOverlap = static_cast<FastRender::Bit>(53);

namespace EyeOverlap {

enum class Reason : u32 {
    Taken,                 // no middle barrier
    DrawWrite,             // a draw wrote buffers/raw memory/storage images since the last
                           // closing barrier
    RawMemory,             // the shader uses DMA/BDA
    SpecialBuffer,         // GDS, BDA page table or fault buffer bound
    MonoImage,             // a storage image without a right-eye layer
    RightAliasLeftWrite,   // a right-eye buffer descriptor is a buffer the left eye writes
    LeftAliasRightWrite,   // a left-eye buffer descriptor is a buffer the right eye writes
    Indirect,              // an argument buffer is written by the other eye
    OtherDescriptor,       // a descriptor type the pairing does not double
    Count,
};

/// Filled by Pipeline::BindResources from the paired descriptors of one dispatch.
struct Check {
    Reason reason{};
    bool classified{}; // Classify ran on this dispatch's paired descriptors
    bool collect{};    // FAST_DISPATCH_EYE_OVERLAP_VERIFY: keep the written ranges
    boost::container::small_vector<vk::Buffer, 8> left_written, right_written;
    // Written storage buffer ranges, left then right per descriptor (collect only).
    boost::container::small_vector<vk::DescriptorBufferInfo, 16> written;
};

/// `paired` are native stereo's writes ([left, right] per storage buffer and image descriptor);
/// `writable` is Pipeline::NativeWritableBindings (indexed by dstBinding). A no-op unless the
/// rasterizer-side checks left `check.reason` at Taken.
inline void Classify(std::span<const u8> writable, std::span<const vk::WriteDescriptorSet> paired,
                     Check& check) {
    check.classified = true;
    check.left_written.clear();
    check.right_written.clear();
    check.written.clear();
    if (check.reason != Reason::Taken) {
        return;
    }
    for (const auto& write : paired) {
        switch (write.descriptorType) {
        case vk::DescriptorType::eStorageBuffer:
            if (write.dstBinding < writable.size() && writable[write.dstBinding]) {
                for (u32 i = 0; i + 1 < write.descriptorCount; i += 2) {
                    const auto& left = write.pBufferInfo[i];
                    const auto& right = write.pBufferInfo[i + 1];
                    if (left.buffer) {
                        check.left_written.push_back(left.buffer);
                    }
                    if (right.buffer) {
                        check.right_written.push_back(right.buffer);
                    }
                    if (check.collect) {
                        check.written.push_back(left);
                        check.written.push_back(right);
                    }
                }
            }
            break;
        case vk::DescriptorType::eStorageImage:
            for (u32 i = 0; i + 1 < write.descriptorCount; i += 2) {
                const vk::ImageView left = write.pImageInfo[i].imageView;
                if (left && write.pImageInfo[i + 1].imageView == left) {
                    check.reason = Reason::MonoImage;
                    return;
                }
            }
            break;
        case vk::DescriptorType::eSampledImage:
        case vk::DescriptorType::eSampler:
            break;
        default:
            check.reason = Reason::OtherDescriptor;
            return;
        }
    }
    const auto contains = [](const auto& list, vk::Buffer buffer) {
        return std::find(list.begin(), list.end(), buffer) != list.end();
    };
    for (const auto& write : paired) {
        if (write.descriptorType != vk::DescriptorType::eStorageBuffer) {
            continue;
        }
        for (u32 i = 0; i + 1 < write.descriptorCount; i += 2) {
            const vk::Buffer left = write.pBufferInfo[i].buffer;
            const vk::Buffer right = write.pBufferInfo[i + 1].buffer;
            if (right && contains(check.left_written, right)) {
                check.reason = Reason::RightAliasLeftWrite;
                return;
            }
            if (left && contains(check.right_written, left)) {
                check.reason = Reason::LeftAliasRightWrite;
                return;
            }
        }
    }
}

/// FAST_DISPATCH_EYE_OVERLAP_VERIFY[=period] (default 64; 0 or unset = off): the first taken
/// dispatch of every shader and every `period`-th taken dispatch after that runs twice from the
/// same inputs: the written buffer ranges of both eyes and the written storage image levels (both
/// layers) are saved, the eyes run in the old order (left, barrier, right), the results are read
/// back, the inputs restored, and the eyes run swapped (right, barrier, left). Independent eyes
/// give identical bytes; a right eye that read or wrote what the left one writes (or back) gives
/// different ones in the swapped order deterministically, where the overlapped stream would only
/// race. Compared once the GPU has finished ("verify ok/mismatch" on the counter line, details
/// in a "FAST dispatch_eye_overlap verify:" error line). One sample in flight; samples over 64 MiB
/// (both layers of a 2000x2160 RGBA16F storage image already are: verify at 1456x1576), with a
/// written range of unknown size, or with a storage image that is not native stereo, single
/// sample, in GENERAL as a whole (a per-mip storage transition leaves per-subresource states) are
/// skipped. A shader whose own result depends on atomic order would differ between any two runs.
/// What it checks is the descriptor gate (Classify, EyeOverlapTake): both runs are fenced by full
/// barriers, so it cannot see an earlier writer that only the middle barrier ordered before the
/// right eye (the DrawWrite reason's class); that part needs Khronos synchronization validation.
/// `=1` means on with the default period; hexadecimal (`0x40`) is accepted. Diagnostic only.
inline u32 VerifyPeriod() {
    static const u32 period = [] {
        const char* value = std::getenv("FAST_DISPATCH_EYE_OVERLAP_VERIFY");
        if (!value || !*value) {
            return u32{0};
        }
        char* end = nullptr;
        const unsigned long n = std::strtoul(value, &end, 0);
        if (end != value && n == 0) {
            return u32{0}; // "0", "0x0"
        }
        return n > 1 ? u32(std::min<unsigned long>(n, 0xFFFF'FFFFul)) : u32{64};
    }();
    return period;
}

/// "FAST dispatch_eye_overlap" statistics: written on the recording thread, the verify ones by the
/// deferred comparison.
struct Stats {
    std::atomic<u64> evaluated{};
    std::array<std::atomic<u64>, static_cast<size_t>(Reason::Count)> reasons{};
    std::atomic<u64> verify_ok{}, verify_mismatch{}, verify_skipped{};
    std::atomic<s64> last_log{};
};
inline Stats g_stats;

/// Counts one evaluated dispatch and logs the line every 5 s.
inline void Note(Reason reason) {
    auto& s = g_stats;
    constexpr auto relaxed = std::memory_order_relaxed;
    const u64 evaluated = s.evaluated.fetch_add(1, relaxed) + 1;
    s.reasons[static_cast<size_t>(reason)].fetch_add(1, relaxed);
    if ((evaluated & 255) != 0) {
        return;
    }
    const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 last = s.last_log.load(relaxed);
    if (now - last < 5000 || !s.last_log.compare_exchange_strong(last, now, relaxed)) {
        return;
    }
    const auto r = [&](Reason which) { return s.reasons[static_cast<size_t>(which)].load(relaxed); };
    LOG_INFO(Render_Vulkan,
             "FAST dispatch_eye_overlap: evaluated={} taken={} fallback draw_write={} "
             "raw_memory={} special_buffer={} mono_image={} right_alias={} left_alias={} "
             "indirect={} other_descriptor={} verify ok={} mismatch={} skipped={}",
             evaluated, r(Reason::Taken), r(Reason::DrawWrite), r(Reason::RawMemory),
             r(Reason::SpecialBuffer), r(Reason::MonoImage), r(Reason::RightAliasLeftWrite),
             r(Reason::LeftAliasRightWrite), r(Reason::Indirect), r(Reason::OtherDescriptor),
             s.verify_ok.load(relaxed), s.verify_mismatch.load(relaxed),
             s.verify_skipped.load(relaxed));
}

} // namespace EyeOverlap
} // namespace Vulkan
