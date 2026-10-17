// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstdlib>
#include "video_core/fast_render.h"
namespace Vulkan {
inline bool TemporalOptimizations() {
    static const bool enabled = [] {
        const char* value = std::getenv("VR_TEMPORAL_OPT_OFF");
        return !value || value[0] != '1';
    }();
    return enabled;
}

// Toggle bit 36: the per-draw work of the temporal path (object motion vectors) in bbport's
// cheaper order, with results identical to the old path (vk_rasterizer_temporal.cpp): an index
// list is hashed only when its bytes may have changed, the scene camera is parsed only when its
// bytes changed, the player test reads the packet's own camera record without the session lock,
// and the motion pipeline variant is memoized off the pipeline selection lock. bbport measured
// these costs at ~0.28 us per packet on stage B (docs/parallel_gpu.md:511-513). Its bone-palette
// gate (vk_rasterizer.cpp:1691-1720) is not ported: our candidates are character skeletons, which
// it never gates either, and player-held rigid pieces, which move with their 416-byte model
// constants while the palette stays the same, so gating them would change the vectors.
static constexpr auto MotionFast = static_cast<FastRender::Bit>(36);

// Bit 36 counters. All temporal work runs on the thread that records draws (stage B, or the GPU
// command thread when serial): one writer, and the "FAST motion" line is printed there too.
struct MotionFastCounters {
    uint64_t frames{}, index_draws{}, index_proven{}, index_hashed{}, index_unprovable{};
    uint64_t hashed_bytes{}, skipped_bytes{};
    uint64_t camera_calls{}, camera_reused{};
    uint64_t player_calls{}, player_unlocked{};
    uint64_t variant_calls{}, variant_memo{};
    uint64_t verify_tick{}, verify_checks{}, verify_mismatches{}, verify_races{};
};
inline MotionFastCounters g_motion_fast{};

/// FAST_MOTION_VERIFY=N: every Nth reuse of bit 36 is recomputed the old way and compared; a
/// mismatch logs an error and the old way's result is used.
inline uint64_t MotionVerifyEvery() {
    static const uint64_t every = [] {
        const char* value = std::getenv("FAST_MOTION_VERIFY");
        return value ? uint64_t(std::strtoull(value, nullptr, 0)) : uint64_t{0};
    }();
    return every;
}
inline bool MotionVerifySample() {
    const uint64_t every = MotionVerifyEvery();
    if (!every || ++g_motion_fast.verify_tick % every != 0)
        return false;
    ++g_motion_fast.verify_checks;
    return true;
}
} // namespace Vulkan
