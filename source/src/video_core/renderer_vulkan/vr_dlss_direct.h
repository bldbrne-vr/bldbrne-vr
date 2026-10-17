// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdlib>
#include "common/types.h"
#include "video_core/fast_render.h"

namespace Vulkan {
// FAST bit 46: DLSS reads the scene itself. bbport hands NGX the scene colour and a depth-aspect
// view of the scene depth (vk_temporal_upscaler.cpp:1145-1225, docs/upscaler.md); we copied both
// into canonical single-layer images first (VrTemporalImages::Prepare: 24 bytes moved per input
// pixel and eye, RGBA16F 8+8 and depth 4+4). Our scene is a two-layer native-stereo image
// (RGBA16F colour, D32S8 depth), so with the bit each eye's DLSS feature gets that eye's 2D layer
// view (VrTemporalCapture::Views natively, VrSceneProxy::Read views in reduced raster), in GENERAL
// like the copies, and the motion pass samples the same depth view. NGX takes any depth-stencil
// format and one instance per view of a shared target (DLSS Programming Guide 3.3, 3.17). The
// values are the ones the copy stored (texel for texel, f32 depth to f32; the copy could at most
// flush f16 denormals), so DLSS keeps its features and history and the bit may flip between any
// two frames. The merge takes the game's alpha from the scene: in
// place natively (the shader's imageLoad path, the same texel), from the reduced proxy otherwise.
// Not covered, so still copied: FSR 4 (its backend's barriers are COLOR aspect, layer 0), DLSS 5
// (its chain hands the depth input to a second feature), VR_TEMPORAL_CAPTURE_DIR (captures read
// the copies) and any depth format other than D32/D32S8. The canonical images stay allocated so
// a flip needs no reallocation (about 55 MB unused at 1456x1576 while the bit is on).
static constexpr auto DlssDirect = static_cast<FastRender::Bit>(46);

/// Whether a frame of this provider may read the scene directly when the bit is on (see above).
inline bool DlssDirectEligible(bool dlss, bool dlss5) {
    static const bool capture = std::getenv("VR_TEMPORAL_CAPTURE_DIR") != nullptr;
    return dlss && !dlss5 && !capture;
}

/// FAST_DLSS_DIRECT_VERIFY[=period] (default 120; 0 or unset = off): a twin DLSS feature (bridge
/// view 2) follows one eye with the old path's inputs: that eye's canonical copies and its own
/// motion pass over the copied depth, the same jitter, reset and frame time. Every `period`
/// resolved frames the eye's output and the twin's are read back and compared once the GPU has
/// finished ("FAST dlss_direct verify:"). With the bit off both sides read the copies, which
/// measures how far DLSS differs from itself on identical inputs. Costs one more DLSS eye, one
/// eye's copies and one motion pass per frame; diagnostic only. The twin needs a bridge built
/// with three views (scratchpad/temporal-staging/adapted/dlss_bridge); an older bridge refuses
/// view 2 and the counter line says twin=unavailable. Build it with the same options as the
/// runtime it stands in for (the deployed one came from local/build_dlss45_only.cmd:
/// VR_DLSS_SR_PRESET=13, VR_DLSSNR_CHAIN=OFF), or the verify runs use another DLSS model.
inline u32 DlssDirectVerifyPeriod() {
    static const u32 period = [] {
        const char* value = std::getenv("FAST_DLSS_DIRECT_VERIFY");
        if (!value || !*value || value[0] == '0')
            return u32{0};
        const unsigned long n = std::strtoul(value, nullptr, 0);
        return n > 1 ? u32(n) : u32{120};
    }();
    return period;
}

/// FAST_DLSS_DIRECT_VERIFY_EYE=1: the twin follows the right eye (array layer 1, the layer a
/// renderer that ignored baseArrayLayer would get wrong) instead of the left one.
inline u32 DlssDirectVerifyEye() {
    static const u32 eye = [] {
        const char* value = std::getenv("FAST_DLSS_DIRECT_VERIFY_EYE");
        return value && value[0] == '1' ? u32{1} : u32{0};
    }();
    return eye;
}

/// Inputs of a direct eye that are still copied (bit DlssDirectColor, bit DlssDirectDepth).
enum DlssDirectInput : u32 { DlssDirectColor = 1, DlssDirectDepth = 2 };

/// FAST_DLSS_DIRECT_LAYER_COPY=depth|color|both|none (default depth): which inputs of an eye at
/// array layer 1 (the right eye) keep their canonical copy under bit 46. The verify twin found
/// the right eye's direct output wrong (2.2M of 2.29M texels, max_abs 0.5-0.73, w5_dlss_verify_
/// right) while the left eye's was bit-exact and the right eye's picture was its own (colour from
/// layer 1): NGX reads one layer-1 input from layer 0, most likely the depth aspect of the D32S8
/// scene. Copying only that input keeps the right eye bit-exact with its copy-path twin and still
/// saves the left eye's 24 bytes and the right eye's colour (16 of its 24). `none` restores the
/// fully direct right eye (to reproduce the bug); `both` copies the right eye as with the bit off.
inline u32 DlssDirectLayerCopy() {
    static const u32 copy = [] {
        const char* value = std::getenv("FAST_DLSS_DIRECT_LAYER_COPY");
        if (!value || !*value)
            return u32{DlssDirectDepth};
        switch (value[0]) {
        case 'n': case 'N': case '0': return u32{0};
        case 'c': case 'C': return u32{DlssDirectColor};
        case 'b': case 'B': return u32{DlssDirectColor | DlssDirectDepth};
        default: return u32{DlssDirectDepth};
        }
    }();
    return copy;
}
inline const char* DlssDirectLayerCopyName() {
    constexpr const char* names[] = {"none", "color", "depth", "both"};
    return names[DlssDirectLayerCopy() & 3];
}

/// "FAST dlss_direct" statistics. Written on the rendering thread, read by its periodic line.
struct DlssDirectStats {
    std::atomic<u64> direct{};      // resolved DLSS frames whose colour/depth were the scene's
    std::atomic<u64> copied{};      // resolved DLSS frames that read canonical copies
    std::atomic<u64> fallback{};    // ... of which the bit was on (DLSS 5, capture, depth format)
    std::atomic<u64> saved_bytes{}; // copy traffic not issued (colour 16, depth 8 bytes per
                                    // input pixel and eye)
    std::atomic<u32> twin{};        // 0 none, 1 created, 2 refused (bridge without view 2),
                                    // 3 failed after creation (verification stopped)
    std::atomic<u64> verify_compared{}, verify_direct{}; // compared frames, of which direct
    std::atomic<u64> verify_mismatched_frames{}, verify_mismatched_texels{};
    std::atomic<u32> verify_max_abs{}; // float bits of the largest absolute difference
    std::atomic<u64> verify_skipped{}; // twin out of step with its eye (frames)
};
inline DlssDirectStats g_dlss_direct;

inline void DlssDirectNote(std::atomic<u64>& counter, u64 n = 1) {
    counter.fetch_add(n, std::memory_order_relaxed);
}
} // namespace Vulkan
