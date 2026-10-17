// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdlib>
#include "common/types.h"
#include "video_core/fast_render.h"
#include "video_core/renderer_vulkan/vr_fsr4_bridge.h"

namespace Vulkan {
// FAST bit 45: both FSR 4 eyes in lockstep. FSR 4 is GPU-bound, and each eye is a separate
// single-view context whose frame is about fifteen compute dispatches, every one followed by a
// global barrier. At 1456x1576 per eye the small model passes (1/8-resolution encoder, SPD)
// cannot fill the GPU, and every barrier drains it. With the bit on, a bridge with the optional
// pair export (VR_FSR4_PAIR_EXPORT) records left job k and right job k back to back under one
// barrier: the eyes overlap on the GPU and 32 barrier groups per frame become 16. The
// dispatches, their inputs and descriptor contents are unchanged, the eyes share no written
// resource (argued in adapted/fsr4_bridge/vr_fsr4_bridge.cpp), so the output is bit-identical
// to the per-eye order.
// bbport never had this (one flat view). Any unmet precondition (different sizes, profiles,
// flags, resets or job lists, an eye without a free slot) records the per-eye order instead.
// Off, or with a bridge without that export (log: bridge=per-eye), the pair calls Evaluate per
// eye exactly as before. Read every frame; flipping it recreates nothing.
// FAST_FSR4_INTERLEAVE_VERIFY[=period] (default 120) adds a left-eye twin context with the
// eyes' own options, always evaluated alone after the pair, and compares the left eye with it
// every `period` resolved frames (the FAST_UPSCALER_VERIFY readback; replaces that twin when
// both are set). It costs one extra FSR eye per frame. FAST_FSR4_INTERLEAVE_VERIFY_RESET=1
// also resets every eye's history together at period/2, so each comparison covers frames since
// a common reset and a one-off SPD auto-exposure race (research_fsr4_bitexact.md) cannot leave
// a lasting difference.
static constexpr auto Fsr4Interleave = static_cast<FastRender::Bit>(45);

/// 0 = no verification; otherwise the sampling period in resolved frames.
inline u32 Fsr4InterleaveVerifyPeriod() {
    static const u32 period = [] {
        const char* value = std::getenv("FAST_FSR4_INTERLEAVE_VERIFY");
        if (!value || !*value || value[0] == '0')
            return u32{0};
        const unsigned long n = std::strtoul(value, nullptr, 0);
        return n > 1 ? u32(n) : u32{120};
    }();
    return period;
}

inline bool Fsr4InterleaveVerifyReset() {
    static const bool reset = [] {
        const char* value = std::getenv("FAST_FSR4_INTERLEAVE_VERIFY_RESET");
        return value && value[0] == '1';
    }();
    return reset;
}

/// "FAST fsr4 interleave" statistics. Written on the rendering thread, read by its periodic line.
struct Fsr4InterleaveStats {
    std::atomic<u64> paired{};      // frames through the bridge's lockstep entry
    std::atomic<u64> interleaved{}; // of those, recorded interleaved
    std::atomic<u64> captured{};    // of those, captured and replayed (barrier counts known)
    std::atomic<u64> barriers{}, sequential_barriers{}, dispatches{}; // over captured frames
    std::array<std::atomic<u64>, VR_FSR4_PAIR_REASONS> fallback{};    // by VR_FSR4_PAIR_*
    std::atomic<u32> bridge{}; // 0 no FSR session, 1 per-eye bridge only, 2 lockstep entry
    std::atomic<u64> verify_compared{}, verify_mismatched_frames{}, verify_mismatched_texels{};
    std::atomic<u32> verify_max_abs{}; // float bits of the largest absolute difference
    std::atomic<u64> verify_skipped{}; // twin unavailable or diverged (frames)
};
inline Fsr4InterleaveStats g_fsr4_interleave;

inline void Fsr4InterleaveNote(const VrFsr4PairResult& r) {
    auto& s = g_fsr4_interleave;
    constexpr auto relaxed = std::memory_order_relaxed;
    s.paired.fetch_add(1, relaxed);
    s.interleaved.fetch_add(r.interleaved, relaxed);
    if (!r.interleaved && r.fallback < s.fallback.size())
        s.fallback[r.fallback].fetch_add(1, relaxed);
    if (r.sequential_barriers) {
        s.captured.fetch_add(1, relaxed);
        s.barriers.fetch_add(r.barriers, relaxed);
        s.sequential_barriers.fetch_add(r.sequential_barriers, relaxed);
        s.dispatches.fetch_add(r.dispatches, relaxed);
    }
}
} // namespace Vulkan
