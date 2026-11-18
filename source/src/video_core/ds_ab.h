// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// VR_DS_AB=a or c (measurement only, off by default): an A/B inside one run of the stage B
// data-structure fixes: RangeSet::AddIfNew, RefreshImage's timer
// after its clean return, and the page tracker's IsMapped check skipped (as logs-off builds do).
// Every Window frames they switch on or off together; stage B's busy cycles (the FAST stats'
// handler ticks) and the draws of each finished window are kept, and adjacent on/off windows are
// compared as pairs (heat, scene and load move both sides of a pair alike). Logged every 8 pairs:
// stage B cycles a frame and a draw, on and off, and the mean pair difference with its standard
// error. The first Warmup frames are not counted. FrameEnd: stage B's frame end only.

#include <atomic>
#include <cmath>
#include <cstdlib>
#include "common/logging/log.h"
#include "common/types.h"
#include "video_core/fast_render.h"

namespace DsAb {

/// What alternates: 'a' the data-structure fixes, 'c' VR_COMPACT_SETS (vk_stereo_set_codec.h);
/// 0: nothing (VR_DS_AB unset).
inline char Mode() {
    static const char mode = [] {
        const char* v = std::getenv("VR_DS_AB");
        return v && (v[0] == 'a' || v[0] == 'c') ? v[0] : '\0';
    }();
    return mode;
}
inline bool Alternating() {
    return Mode() != '\0';
}

inline std::atomic<bool> g_off{false};

/// The fixes are on (always, unless VR_DS_AB=a and this is an off window).
inline bool FixesOn() {
    return Mode() != 'a' || !g_off.load(std::memory_order_relaxed);
}

/// VR_COMPACT_SETS may be used (always, unless VR_DS_AB=c and this is an off window).
inline bool CompactOn() {
    return Mode() != 'c' || !g_off.load(std::memory_order_relaxed);
}

class Ab {
public:
    void FrameEnd() {
        constexpr u64 Window = 300, Warmup = 1800;
        if (++frames < Warmup || (frames - Warmup) % Window != 0) {
            return;
        }
        const u64 ticks = FastRender::g_counters.handler_ticks.load(std::memory_order_relaxed);
        const u64 draws = FastRender::g_counters.draws.load(std::memory_order_relaxed);
        if (started) {
            const double per_frame = double(ticks - last_ticks) / double(Window);
            const double per_draw = double(ticks - last_ticks) / double(std::max<u64>(draws - last_draws, 1));
            const bool was_off = g_off.load(std::memory_order_relaxed);
            Side& side = was_off ? off : on;
            side.frame += per_frame;
            side.draw += per_draw;
            ++side.windows;
            if (was_off && have_on) {
                // A pair: the on window just before this off window. Relative difference of the
                // cycles a draw, off - on, over off (> 0: the fixes save that share).
                const double diff = (per_draw - pair_on_draw) / per_draw;
                sum += diff;
                sum2 += diff * diff;
                ++pairs;
                have_on = false;
                if (pairs % 8 == 0) {
                    const double mean = sum / double(pairs);
                    const double var = std::max(0.0, sum2 / double(pairs) - mean * mean);
                    const double se = std::sqrt(var / double(pairs));
                    LOG_INFO(Render_Vulkan,
                             "VR DS A/B: {} pairs; stage B cycles a frame on {:.0f} off {:.0f}; "
                             "a draw on {:.0f} off {:.0f}; fixes save {:.2f}% +- {:.2f}% of a "
                             "draw's stage B (pairs)",
                             pairs, on.frame / on.windows, off.frame / off.windows,
                             on.draw / on.windows, off.draw / off.windows, 100.0 * mean,
                             100.0 * se);
                }
            } else if (!was_off) {
                pair_on_draw = per_draw;
                have_on = true;
            }
            g_off.store(!was_off, std::memory_order_relaxed);
        }
        started = true;
        last_ticks = ticks;
        last_draws = draws;
    }

private:
    struct Side {
        double frame{}, draw{};
        u64 windows{};
    };
    Side on, off;
    u64 frames{}, last_ticks{}, last_draws{}, pairs{};
    double pair_on_draw{}, sum{}, sum2{};
    bool started{}, have_on{};
};

inline Ab& Get() {
    static Ab ab;
    return ab;
}

} // namespace DsAb
