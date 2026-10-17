// SPDX-License-Identifier: GPL-2.0-or-later
//
// FastRender bit 63 (gpu_plan.md #11, N1): the native-stereo transfer helpers record both eyes'
// transfers under one barrier pair. Today a paired helper records its caller's pre-barrier (left
// resource only), the left transfer, a global ALL->ALL barrier, the right transfer, another global
// barrier, and the caller's post-barrier (left only). The two global barriers were the right
// twin's only memory dependency. With the bit on, the caller's pre- and post-barriers name the
// right twin too (a second BufferMemoryBarrier2 with the same masks, in the same
// vkCmdPipelineBarrier2) and the two global barriers between the eyes are gone.
//
// Sites: BufferCache::SynchronizeBuffer (NativeStereo::Copy of an upload), Image::Upload,
// Image::Download (the existing VR_PAIR_DOWNLOAD_BARRIERS shape), Buffer::Fill. Not
// CopyImageWithBuffer (one staging buffer shared by both eyes), not DetileImage/TileImage.
//
// Gate (no aliasing): the right destination differs from the left destination and from every
// source; the right source (if any) differs from the left destination. Images: the destination is
// a 2-layer native stereo image (layers 0 and 1 are disjoint subresources, and Image::GetBarriers
// widens every transition to both layers). A buffer copy whose source has a right twin (a
// GPU-written source) or whose destination twin would be created lazily falls back to the old
// stream. Bit off: exactly the old command stream.
//
// FAST_TRANSFER_EYE_BARRIERS_VERIFY[=period] (default 64; 0/unset = off): on every period-th taken
// SynchronizeBuffer upload (one in flight, at most 4 MiB), after the post-barrier the left and
// right destination ranges are copied to two host buffers and compared once the GPU has finished.
// Both eyes copy the same staging bytes, so they must be equal; a right copy reordered against an
// earlier writer of the right twin (the hazard the dropped barriers covered) shows up as a
// difference. Diagnostic only; verify adds a global barrier after its readback.

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include "common/logging/log.h"
#include "common/types.h"
#include "video_core/fast_render.h"

namespace Vulkan::TransferEyeBarriers {

static constexpr auto TransferEyeBarriers = static_cast<FastRender::Bit>(63);

enum class Site : u32 { Sync, Upload, Download, Fill, Count };
enum class Fallback : u32 { RightSource, LazyTwin, Alias, MonoImage, EnvOverride, Count };

inline bool On() noexcept {
    return FastRender::On(TransferEyeBarriers);
}

inline u32 VerifyPeriod() {
    static const u32 period = [] {
        const char* value = std::getenv("FAST_TRANSFER_EYE_BARRIERS_VERIFY");
        if (!value || !*value) {
            return u32{0};
        }
        char* end = nullptr;
        const unsigned long n = std::strtoul(value, &end, 0);
        if (end != value && n == 0) {
            return u32{0};
        }
        return n >= 1 ? u32(std::min<unsigned long>(n, 0xFFFF'FFFFul)) : u32{64};
    }();
    return period;
}

struct Stats {
    std::atomic<u64> eligible{};
    std::array<std::atomic<u64>, static_cast<size_t>(Site::Count)> taken{};
    std::array<std::atomic<u64>, static_cast<size_t>(Fallback::Count)> fallback{};
    std::atomic<u64> verify_ok{}, verify_mismatch{}, verify_skipped{};
    std::atomic<u64> verify_counter{};
    std::atomic<bool> verify_in_flight{};
    std::atomic<s64> last_log{};
};
inline Stats g_stats;

inline void MaybeLog() {
    auto& s = g_stats;
    constexpr auto relaxed = std::memory_order_relaxed;
    const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 last = s.last_log.load(relaxed);
    if (now - last < 5000 || !s.last_log.compare_exchange_strong(last, now, relaxed)) {
        return;
    }
    const auto t = [&](Site w) { return s.taken[static_cast<size_t>(w)].load(relaxed); };
    const auto f = [&](Fallback w) { return s.fallback[static_cast<size_t>(w)].load(relaxed); };
    LOG_INFO(Render_Vulkan,
             "FAST transfer_eye_barriers: eligible={} taken sync={} upload={} download={} fill={} "
             "fallback right_source={} lazy_twin={} alias={} mono_image={} env_override={} "
             "verify ok={} mismatch={} skipped={}",
             s.eligible.load(relaxed), t(Site::Sync), t(Site::Upload), t(Site::Download),
             t(Site::Fill), f(Fallback::RightSource), f(Fallback::LazyTwin), f(Fallback::Alias),
             f(Fallback::MonoImage), f(Fallback::EnvOverride), s.verify_ok.load(relaxed),
             s.verify_mismatch.load(relaxed), s.verify_skipped.load(relaxed));
}

/// One paired transfer seen with the bit on: taken at `site`.
inline void NoteTaken(Site site) {
    g_stats.eligible.fetch_add(1, std::memory_order_relaxed);
    g_stats.taken[static_cast<size_t>(site)].fetch_add(1, std::memory_order_relaxed);
    MaybeLog();
}

/// One paired transfer seen with the bit on that kept the old stream.
inline void NoteFallback(Fallback reason) {
    g_stats.eligible.fetch_add(1, std::memory_order_relaxed);
    g_stats.fallback[static_cast<size_t>(reason)].fetch_add(1, std::memory_order_relaxed);
    MaybeLog();
}

/// True when this taken SynchronizeBuffer upload should be verified (claims the one slot).
inline bool ClaimVerify() {
    const u32 period = VerifyPeriod();
    if (period == 0) {
        return false;
    }
    const u64 n = g_stats.verify_counter.fetch_add(1, std::memory_order_relaxed);
    if (n % period != 0) {
        return false;
    }
    bool expected = false;
    if (!g_stats.verify_in_flight.compare_exchange_strong(expected, true,
                                                          std::memory_order_acq_rel)) {
        g_stats.verify_skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    return true;
}

} // namespace Vulkan::TransferEyeBarriers
