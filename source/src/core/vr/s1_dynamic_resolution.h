// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include "core/vr/eye_cant.h"

namespace S1Drs {
// The render owner publishes a requested level and the floor reported by both DLSS
// features. Stage A latches the actual percentage for a whole flip.
inline const bool enabled = [] {
    const char* p = std::getenv("VR_S1_DRS");
    return !p || *p != '0';
}();
inline const bool trace = [] {
    const char* value = std::getenv("VR_TEMPORAL_TRACE");
    return enabled && value && *value == '1';
}();
// VR_S1_DRS_LEVEL=1..3 (tests): that level whenever scaling is possible, whatever the GPU's load
// (a mock run's load rarely asks for one); unset or anything else: the governor decides.
inline const unsigned pinned_level = [] {
    const char* p = std::getenv("VR_S1_DRS_LEVEL");
    return enabled && p && p[0] >= '1' && p[0] <= '3' && p[1] == '\0' ? unsigned(p[0] - '0') : 0u;
}();
inline std::atomic<unsigned> requested{};
inline std::atomic<unsigned> floor_percent{100};
// Never reduce either eye dimension below this many input pixels. A user who
// already chose a smaller input keeps that exact size. 1160 since 2026-11-24 (the
// user, after a Quest 3 session with DLSS Quality's 1336x1440 input, which 1280
// let drop to 96% only: "we should make the floor for dynamic resolution be 1160
// or so"; now 87%).
inline constexpr unsigned MinShortSidePx = 1160;
inline constexpr unsigned QualityFloor(unsigned short_side) {
    return short_side <= MinShortSidePx ? 100u :
        std::max(70u, 1u + (100u * MinShortSidePx - 1u) / short_side);
}
inline std::atomic<bool> native_ready{};
template <class Q>
inline bool ParallelEyes(const std::array<Q, 2>& eyes) {
    return !VR::Cant::Canted(eyes[0]) && !VR::Cant::Canted(eyes[1]);
}
// The eyes' scene must reach the headset as the reconstructed or blitted pair in the UI layer.
// Foveated upscaling no longer stops it (2026-11-22): its cut-outs follow the scaled view
// (vr_dlss_focus.h DrsSpan) and its foveation map the view (vr_foveation.h Map scale).
inline constexpr bool ProjectionSettingsAllow(bool late_warp, bool xr_depth, bool ui_layer) {
    return ui_layer && !late_warp && !xr_depth;
}
inline unsigned Percent(unsigned step) {
    return std::max(100u - 10u * step, floor_percent.load(std::memory_order_relaxed));
}
inline constexpr unsigned Margin(unsigned n, unsigned drop) {
    return n * drop / 200u;
}
// The motion shader uses absolute pixel coordinates and 8x8 groups. Round both
// edges outwards so even odd-sized DLSS subrectangles get every requested vector.
struct GroupRange {
    unsigned first, count;
};
inline constexpr GroupRange MotionGroups(unsigned extent, unsigned drop) {
    const unsigned inset = Margin(extent, drop);
    const unsigned first = inset / 8;
    return {first, (extent - inset + 7) / 8 - first};
}
// Guest scissors are signed. Intersect each endpoint with the same closed
// interval; a fully offscreen or inverted scissor remains empty.
inline void ClipAxis(int16_t& lo, int16_t& hi, unsigned size, unsigned inset) {
    const int left = static_cast<int>(inset);
    const int right = static_cast<int>(size - inset);
    const int start = std::clamp<int>(lo, left, right);
    lo = static_cast<int16_t>(start);
    hi = static_cast<int16_t>(std::clamp<int>(hi, start, right));
}
// Widen about each asymmetric eye's own optical center. The original rays then
// cover exactly the active input pixels. DLSS maps those pixels to the full output.
template <class F>
F Expanded(F f, unsigned drop, unsigned eye) {
    struct Entry {
        F in{}, out{};
        unsigned pct{};
    };
    static thread_local std::array<Entry, 2> cache{};
    auto& e = cache[eye];
    const unsigned pct = 100u - drop;
    if (e.pct != pct || std::memcmp(&f, &e.in, sizeof(F))) {
        e.in = f;
        e.pct = pct;
        auto expand = [pct](float& a, float& b) {
            float lo = std::tan(a), hi = std::tan(b), center = (lo + hi) * .5f;
            float half = (hi - lo) * 50.f / float(pct);
            a = std::atan(center - half);
            b = std::atan(center + half);
        };
        expand(f.angle_left, f.angle_right);
        expand(f.angle_down, f.angle_up);
        e.out = f;
    }
    return e.out;
}
using SetRegion = unsigned (*)(unsigned);
inline std::atomic<SetRegion> dlss_region{};
inline std::atomic<bool> timing_wanted{};
// Only the player hook owns the low bit. A camera teleport can also advance the
// sequence, including when no stereo frame was reconstructed during the change.
inline std::atomic<unsigned> player_transition{};
inline void NotePlayerTransition(bool active) {
    const unsigned previous = player_transition.load(std::memory_order_relaxed);
    player_transition.fetch_add(2u + unsigned(active) - (previous & 1u),
                                std::memory_order_relaxed);
}
inline void NoteTeleport() {
    player_transition.fetch_add(2u, std::memory_order_relaxed);
}
// Part of the rasterizer's ordered draw state. The flip can run on the worker
// or on the producer after draining it. Both must observe the same frame.
struct FrameState {
    bool started{}, native{}, gameplay{}, parallel_eyes{}, clip_scene{}, sample_candidate{}, pair_captured{};
    unsigned width{}, height{}, drop{}, clipped{}, inset_x{}, inset_y{};
    uint64_t scissor_key{};

    void Target(unsigned target_width, unsigned target_height, unsigned mask) {
        scissor_key = clip_scene && mask == 3 && target_width == width && target_height == height
                          ? (uint64_t(width) << 32 | uint64_t(height) << 8 | drop)
                          : 0;
        if (trace && scissor_key)
            ++clipped;
    }
    void BeginFrame(unsigned w, unsigned h, unsigned reduction) {
        width = w;
        height = h;
        drop = reduction;
        clip_scene = drop != 0;
        if (drop) {
            const unsigned guard = std::min(w, h) / 24;
            const unsigned x = Margin(w, drop), y = Margin(h, drop);
            inset_x = x > guard ? x - guard : 0;
            inset_y = y > guard ? y - guard : 0;
        }
    }
    bool ShouldHold(bool used_layer_scene) const {
        return drop && !(pair_captured && used_layer_scene);
    }
};
// One sample per completed GPU frame. A recovery step must have enough headroom
// to pay the area increase: what the step saved when it was last measured, or,
// until it has been, as if all elapsed GPU work scaled with pixels.
struct Governor {
    unsigned last{}, count{}, over{}, quiet{}, level{};
    unsigned transition{}, settle{}, ready{};
    unsigned retry_frames = 8;
    bool retrying_reduction{};
    bool suspended{};
    float sum{};
    // relief[l]: the GPU time at level l over the time at l - 1, measured across the last
    // change between them (0: not measured): the larger saving of two readings, the last few
    // frames' time (`recent`) just before the change and at the end of the first window after
    // it, and the two windows' averages. A step taken at a sudden load mixes the window's
    // lighter frames from before the load into its average and would read as nothing saved; a
    // hitch in the last frames would do the same to `recent`. Kept between the pixels' share
    // (ratio^2: the whole frame scaling) and 1 (nothing saved). With foveated upscaling a step
    // saved far less than its pixels (2026-11-24 review, mock: 8.72 -> 8.38 -> 7.79 ms at
    // 100/90/80%), and the pixels' share held one heavy fight's level long after full scale fit
    // again.
    std::array<float, 4> relief{};
    float recent{};       // the GPU time of about the last 8 frames (an average, 1/8 per frame)
    float before{};       // `recent` at a change (0: none waiting to be measured)
    float before_avg{};   // and that window's average
    unsigned before_level{};
    // A climb is only a prediction: a level a climb reached goes back whenever one of its
    // windows runs over the budget, the first (a wrong prediction) or any later one (the scene
    // grew), where a level never left holds up to 1.07 of the budget (the pressure rule). Each
    // climb that goes back makes the next one wait twice as many quiet windows (3, 6, 12, up to
    // 24); a climbed level that holds 8 windows, or 32 windows without a change, eases it again.
    // (2026-11-24, closed-loop sessions against the old rule: frames over the budget 18.2% ->
    // 14.9% near the budget, time below full scale while it fit 10.0% -> 3.6%; loads swinging
    // around the budget every few seconds 3.95 -> 1.2 level changes a minute.)
    bool climbed{};
    unsigned climb_need = 3; // quiet windows a climb needs
    unsigned held{};         // windows the climbed level has held
    unsigned still{};        // windows without a change
    void DiscardWindow() {
        count = over = quiet = ready = 0;
        sum = 0;
        settle = 48;
        // (A rest, a death or a fallback between them: not one scene.)
        recent = before = before_avg = 0;
    }
    // A fallback requires an ordinary projection. Keep the last working level
    // privately, then restore it once a short streak of usable frames arrives.
    void Suspend(bool reduced = false) {
        if (!suspended) {
            if (reduced && retrying_reduction)
                retry_frames = std::min(1024u, retry_frames * 4);
            suspended = true;
            requested.store(0, std::memory_order_release);
        }
        DiscardWindow();
    }
    bool Sample(unsigned serial, float ms, float budget, unsigned event, bool playing) {
        if (transition != event || (event & 1u) || !playing) {
            transition = event;
            last = serial;
            DiscardWindow();
            return false;
        }
        if (serial == last || !(ms > 0 && ms < 100 && budget > 0))
            return false;
        last = serial;
        if (suspended) {
            if (++ready < retry_frames)
                return false;
            suspended = false;
            retrying_reduction = level != 0;
            requested.store(level, std::memory_order_release);
            return level != 0;
        }
        if (settle) {
            if (--settle == 0) {
                retry_frames = 8;
                retrying_reduction = false;
            }
            return false;
        }
        sum += ms;
        recent = recent > 0 ? recent + (ms - recent) * 0.125f : ms;
        // Require persistent GPU pressure. At 90 Hz a steady 89 FPS-equivalent
        // GPU time is inside the tolerance. Give full quality a longer first look.
        over += ms > budget * 1.07f;
        const unsigned window = level ? 32u : 64u;
        if (++count != window)
            return false;
        const float avg = sum / count;
        // The first window after a change: what the step between the two levels saves.
        const bool measured = before > 0 && before_avg > 0 &&
                              (before_level + 1 == level || level + 1 == before_level);
        if (measured) {
            const unsigned reduced = std::max(level, before_level);
            const float pixels = float(Percent(reduced)) / Percent(reduced - 1);
            const float by_frames = reduced == level ? recent / before : before / recent;
            const float by_windows = reduced == level ? avg / before_avg : before_avg / avg;
            relief[reduced] = std::clamp(std::min(by_frames, by_windows), pixels * pixels, 1.0f);
        }
        before = before_avg = 0;
        unsigned next = level;
        if (climbed && avg > budget && Percent(level + 1) < Percent(level)) {
            next++; // (back where it climbed from: see climbed)
            quiet = 0;
            climb_need = std::min(climb_need * 2, 24u);
        } else if (over >= window * 3 / 4 && level < 3 && Percent(level + 1) < Percent(level)) {
            next++;
            quiet = 0;
        } else if (level) {
            const float ratio = float(Percent(level)) / Percent(level - 1);
            // The time one step up: this level's over what the step saves, inside 0.90 of the
            // budget with a measured saving, which predicts the climb as it is (the scene's own
            // drift then needs the margin: closed-loop sessions, 2026-11-24, 0.94 let 16-43
            // climbs land over the budget in 40-60 h, 0.90 2-6, still cutting the old rule's
            // time held below full scale when it fit by half to two thirds). Unmeasured, the
            // pixels' share inside 0.94, as before: never more than a step's real saving.
            const float headroom =
                relief[level] > 0 ? .90f * relief[level] : .94f * ratio * ratio;
            quiet = avg < budget * headroom ? quiet + 1 : 0;
            if (quiet >= climb_need) {
                next--;
                quiet = 0;
            }
        }
        sum = 0;
        over = count = 0;
        if (next == level) {
            if (climbed && ++held >= 8)
                climb_need = 3;
            if (++still >= 32) {
                still = 0;
                climb_need = std::max(climb_need / 2, 3u);
            }
            return false;
        }
        held = still = 0;
        climbed = next < level;
        before = recent;
        before_avg = avg;
        before_level = level;
        level = next;
        requested.store(next, std::memory_order_release);
        return true;
    }
};
} // namespace S1Drs
