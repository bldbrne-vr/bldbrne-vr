// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <atomic>
#include <cstdlib>
#include <string_view>
#include "common/types.h"
#include "video_core/fast_render.h"

namespace Vulkan {
// FAST bit 37: bbport's temporal-upscaler optimizations (docs/upscaler.md, parallel_gpu.md,
// vk_temporal_upscaler.cpp), adapted to the stereo DLSS/FSR path. Off = the previous paths.
// Every use reads the bit again, so it may flip between frames (FAST_TOGGLE_FILE):
//  - Recording: the motion, preparation and merge passes, object-motion barriers and the
//    reduced-raster proxy copies go through the recorder instead of borrowing the raw command
//    buffer, which drained the recording thread and kept the rest of that command buffer on
//    the stage-B thread. Only the provider evaluation stays raw, and the borrow ends after it.
//    Identical commands in identical order; output unchanged.
//  - Fsr4Post: FSR 4 uses bbport's shared-memory post pass (opt/*_post.spv,
//    prepare_fsr4_post_lds.py) when its manifest is installed. Changing it recreates both FSR
//    eyes (history restarts). FAST_UPSCALER_VERIFY[=period] runs a twin left-eye context with
//    the original post pass on identical inputs and compares the outputs every `period`
//    resolved frames (default 120; costs one extra FSR eye per frame, diagnostic only).
//    The twin keeps its own history, so one stray difference persists and spreads:
//    FAST_UPSCALER_VERIFY_TWIN=same gives the twin the eyes' own post pass (A/A control: any
//    mismatch then comes from nondeterminism both paths share), and FAST_UPSCALER_VERIFY_RESET=1
//    restarts both eyes and the twin together half a period before each comparison, so every
//    comparison covers only that window (per-frame equality, not a lifetime's drift).
// FAST_UPSCALER_PARTS isolates parts for A/B: 1 = Recording, 2 = Fsr4Post (default both). The
// optimized post pass measured faster (GPU 9.46 -> 9.35 ms/frame, RTX 5080, 2026-10-13). Its
// twin check on that GPU matched bit for bit for 40 comparisons (frames 2520-4860), then drifted
// for good (max_abs 0.05-1.2): most likely a one-off seed carried by both histories, not a
// per-frame arithmetic difference (research_fsr4_bitexact.md: inferred from FSR 4's racing SPD
// auto-exposure, not yet seen on the GPU; FAST_UPSCALER_VERIFY_RESET=1 checks it).
// With the bit on, every FSR context (eyes and twin) also runs the fenced SPD auto-exposure pass
// (opt/spd_auto_exposure.spv, same arithmetic) when packaged and the bridge takes its flag;
// FAST_UPSCALER_SPD_FENCE=0 keeps FSR 4's original for A/B (FAST_*: run_probe.py drops VR_*).
static constexpr auto UpscalerFast = static_cast<FastRender::Bit>(37);

namespace UpscalerFastPart {
enum : u32 { Recording = 1, Fsr4Post = 2 };
}

inline u32 UpscalerFastParts() {
    static const u32 parts = [] {
        const char* value = std::getenv("FAST_UPSCALER_PARTS");
        return value && *value ? u32(std::strtoul(value, nullptr, 0)) : ~u32{0};
    }();
    return parts;
}

[[nodiscard]] inline bool UpscalerFastOn(u32 part) {
    return FastRender::On(UpscalerFast) && (UpscalerFastParts() & part) != 0;
}

/// 0 = no verification; otherwise the sampling period in resolved frames.
inline u32 UpscalerFastVerifyPeriod() {
    static const u32 period = [] {
        const char* value = std::getenv("FAST_UPSCALER_VERIFY");
        if (!value || !*value || value[0] == '0')
            return u32{0};
        const unsigned long n = std::strtoul(value, nullptr, 0);
        return n > 1 ? u32(n) : u32{120};
    }();
    return period;
}

/// FAST_UPSCALER_VERIFY_TWIN=same: the twin runs the eyes' own post pass (A/A control).
inline bool UpscalerFastVerifySame() {
    static const bool same = [] {
        const char* value = std::getenv("FAST_UPSCALER_VERIFY_TWIN");
        return value && std::string_view(value) == "same";
    }();
    return same;
}

/// FAST_UPSCALER_VERIFY_RESET=1: both eyes and the twin restart history together at frame
/// period/2 of every period, so each comparison (frame % period == 0) covers period/2 frames.
inline bool UpscalerFastVerifyLockstep() {
    static const bool lockstep = [] {
        const char* value = std::getenv("FAST_UPSCALER_VERIFY_RESET");
        return value && *value && value[0] != '0';
    }();
    return lockstep;
}

/// FAST_UPSCALER_SPD_FENCE=0: bit 37 does not request the fenced SPD auto-exposure pass, so
/// FSR keeps FSR 4's original one (A/B and A/A control runs).
inline bool UpscalerFastSpdFence() {
    static const bool fence = [] {
        const char* value = std::getenv("FAST_UPSCALER_SPD_FENCE");
        return !(value && value[0] == '0');
    }();
    return fence;
}

/// "FAST upscaler" statistics. Written on the rendering thread, read by its periodic line.
struct UpscalerFastStats {
    std::atomic<u64> raw_avoided{};  // raw command-buffer borrows replaced by recorded commands
    std::atomic<u64> raw_released{}; // provider borrows handed back to the recorder
    std::atomic<u64> resolved{};     // successful both-eye resolves
    std::atomic<u32> fsr4_post{};    // 0 original, 1 optimized, 2 requested but not installed
    std::atomic<u32> fsr4_spd{};     // 0 no FSR, 1 original SPD, 2 fenced, 3 requested but absent
    std::atomic<u64> verify_compared{}, verify_mismatched_frames{}, verify_mismatched_texels{};
    std::atomic<u32> verify_max_abs{}; // float bits of the largest absolute difference
    std::atomic<u64> verify_skipped{}; // twin unavailable or diverged (frames)
};
inline UpscalerFastStats g_upscaler_fast;

inline void UpscalerFastNote(std::atomic<u64>& counter, u64 n = 1) {
    counter.fetch_add(n, std::memory_order_relaxed);
}
} // namespace Vulkan
