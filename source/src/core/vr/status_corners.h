// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Status effects at the corners of the view (the tester's 2026-11-04 "corner renderings for status
// effect. They should be visible only at the corners (top left, top right, bottom left, bottom
// right). Green for poison, etc. It should not become messed up when we get hit or have low
// health"): veins in the status's own colour come in from the four corners of what the headset
// shows as it builds up (the resist module's build-ups, read every main-player update; Veins
// below), flare as it takes hold, and breathe while it lasts (the game's own status meter shows
// its row). Drawn after the hit flash and the low-HP edge, in its own corners only, so neither
// tints nor hides it.
// Pure logic, no allocation; the channel's writer is the main player's update (Site A).
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include "core/vr/seqlock.h"

namespace VR::StatusCorners {

// The resist module's order (core/vr/bloodborne_status.h).
enum Type : std::uint8_t { SlowPoison, RapidPoison, Bleed, Frenzy, Beasthood, Count };
constexpr const char* TypeName(int t) {
    constexpr const char* names[Count] = {"slow poison", "rapid poison", "bleed", "frenzy",
                                          "beasthood"};
    return t >= 0 && t < Count ? names[t] : "?";
}
// Linear rgb: a bilious, sickly green, a venomous violet, arterial crimson, the frenzy's burning
// yellow-orange, the beast's amber (muted: tinted edges of the view, never neon; 2026-11-04 mock).
inline constexpr std::array<std::array<float, 3>, Count> Colors{{
    {0.17f, 0.42f, 0.03f},
    {0.30f, 0.05f, 0.50f},
    {0.55f, 0.01f, 0.02f},
    {0.90f, 0.38f, 0.03f},
    {0.85f, 0.50f, 0.10f},
}};

// What Site A read this update.
struct Reading {
    std::int64_t steady_ns{0};
    std::array<std::int64_t, Beasthood> proc_ns{}; // beasthood has a continuous gauge
    std::array<float, Count> buildup{};
    std::uint8_t active_poisons{}; // bits for the two active-duration HUD icons
    bool valid{false};
};

static_assert(sizeof(Reading) <= 64); // keep the existing game-to-XR snapshot budget

constexpr float Buildup(int current, int maximum) {
    if (maximum <= 0 || current >= maximum) return 0.0f;
    if (current <= 0) return 1.0f;
    return float(maximum - current) / float(maximum);
}

// In the 1.09 HUD's two verified tables, slow poison is 6002 -> icon 1, rapid is
// 6001 -> icon 4. Only an active-duration row confirms poison. Buildup uses the same icon.
constexpr std::uint8_t PoisonCountdown(std::int32_t icon, std::uint8_t countdown) {
    if (countdown != 1) return 0;
    return icon == 1 ? 1u << SlowPoison : icon == 4 ? 1u << RapidPoison : 0;
}

inline constexpr std::int64_t FlareNs = 1'200'000'000;

// Carry short activations across XR samples. A bit held for multiple game updates
// represents one event. A delayed HUD countdown corroborates the same poisoning.
struct Activations {
    std::array<std::int64_t, Beasthood> times{};
    unsigned previous_native{}, previous_poisons{}, older_poisons{};

    void Observe(bool same_hunter, unsigned native, unsigned poisons, std::int64_t now,
                 unsigned instant_resets = 0) {
        if (!same_hunter) times = {};
        const unsigned started = native & ~previous_native & 0xfu;
        // Bleed and frenzy have no countdown. Retain their existing sudden-reset
        // indication when the game's pending bit was consumed between samples.
        // The HUD can briefly drop a row during a refresh.
        const unsigned new_rows = poisons & ~(previous_poisons | older_poisons);
        const unsigned corroboration = new_rows | instant_resets;
        if (same_hunter && (started | corroboration)) {
            for (unsigned t = 0; t < times.size(); ++t) {
                const unsigned bit = 1u << t;
                if ((started & bit) ||
                    ((corroboration & bit) && (times[t] == 0 || now - times[t] >= FlareNs))) {
                    times[t] = now;
                }
            }
        }
        older_poisons = same_hunter ? previous_poisons : 0;
        previous_native = native;
        previous_poisons = poisons;
    }
};
// Save the gauge's capacity only when it is at least half built. Changing resistance
// capacity (for example by changing attire) cannot count as an instant-damage event.
struct InstantGauge {
    int armed_capacity{};

    bool Sample(float buildup, int capacity) {
        const bool reset = armed_capacity > 0 && armed_capacity == capacity && buildup <= 0.02f;
        armed_capacity = buildup >= 0.5f ? capacity : 0;
        return reset;
    }
};

inline SeqLock<Reading>& Channel() {
    static SeqLock<Reading> channel;
    return channel;
}
inline constexpr std::int64_t StaleNs = 500'000'000;

inline constexpr float StartBuild = 0.04f;   // below this much build-up: nothing
inline constexpr float BuildAlpha = 0.42f;    // the glow at a full build-up
// The glow's rise with the build-up (BuildAlpha x its fraction to this power): the lower, the
// sooner it darkens (2026-11-27: "make them appear sooner", darker faster, then halfway
// back from 0.75; 1.25 before, when a fifth of a build-up showed at about a sixth of the
// strength). It also drives the reach.
inline constexpr float BuildCurve = 0.95f;
inline constexpr float FlareAlpha = 0.80f;    // as it takes hold
inline constexpr float LastAlpha = 0.30f;     // breathing while it lasts
inline constexpr float LastPeriodS = 1.6f;
inline constexpr std::int64_t FadeNs = 250'000'000; // the colour's and the level's easing

// The strongest status (its own veins' shape, Veins' `type`) and the next one, drawn beside it in
// its own shape and colour (the tester's 2026-11-04 "different shapes/tendrils ... so they do not
// fully overlap"); second = -1 with one or none.
struct Glow {
    float alpha{0.0f};
    std::array<float, 3> color{1.0f, 1.0f, 1.0f};
    int type{-1};
    float second_alpha{0.0f};
    std::array<float, 3> second_color{1.0f, 1.0f, 1.0f};
    int second{-1};
};

class Tracker {
public:
    Glow Update(std::int64_t now, const Reading& r) {
        const bool fresh = r.valid && now >= r.steady_ns && now - r.steady_ns < StaleNs;
        std::array<float, Count> want{};
        for (int t = 0; t < Count; ++t) {
            const float build = fresh ? r.buildup[t] : 0.0f;
            float a = build > StartBuild
                          ? BuildAlpha * std::pow((build - StartBuild) / (1.0f - StartBuild),
                                                  BuildCurve)
                          : 0.0f;
            if (fresh && (r.active_poisons & (1u << t))) {
                const float s = float(now % 60'000'000'000LL) * 1e-9f;
                a = std::max(a, LastAlpha * (0.65f + 0.35f * std::sin(s * 6.2831853f / LastPeriodS)));
            }
            if (fresh && t < Beasthood && r.proc_ns[t] > 0 && now >= r.proc_ns[t] &&
                now - r.proc_ns[t] < FlareNs) {
                const float f = float(now - r.proc_ns[t]) / float(FlareNs);
                a = std::max(a, FlareAlpha * (1.0f - f) * (1.0f - f));
            }
            want[t] = a;
        }
        // Each level eased (up three times as fast as down); the two strongest shown, each in its
        // own colour and shape (one fading as another rises is a cross-fade of the two).
        const float step = last_now_ > 0 && now > last_now_
                               ? std::min(1.0f, float(now - last_now_) / float(FadeNs)) : 1.0f;
        last_now_ = now;
        int top = -1, second = -1;
        for (int t = 0; t < Count; ++t) {
            level_[t] += (want[t] - level_[t]) * (want[t] > level_[t] ? std::min(1.0f, 3.0f * step) : step);
            if (level_[t] < 1e-3f && want[t] <= 0.0f) level_[t] = 0.0f;
            if (top < 0 || level_[t] > level_[top]) {
                second = top;
                top = t;
            } else if (second < 0 || level_[t] > level_[second]) {
                second = t;
            }
        }
        Glow g{};
        if (top < 0 || level_[top] <= 1e-3f) return g;
        g.alpha = level_[top];
        g.color = Colors[top];
        g.type = top;
        if (second >= 0 && level_[second] > 1e-3f) {
            g.second_alpha = level_[second];
            g.second_color = Colors[second];
            g.second = second;
        }
        return g;
    }
    void Reset() { *this = {}; }

private:
    std::array<float, Count> level_{};
    std::int64_t last_now_{0};
};

// The corners' looks (the tester's 2026-11-04 "more visceral ... not gradients (but be VERY
// lightweight)", then "different shapes", "this belongs in Bloodborne"): no bloom, but each status
// its own thing growing in from the corners of what the headset shows (StatusCorner below): slow
// poison's veins, rapid poison's web, bleed's running blood, frenzy's fractures, beasthood's gashes.
// Analytic and per pixel (no texture, no pass; past the reach one check, inside it one look):
// host_shaders/vr_status_veins.glsl mirrors this line by line for vr_overlay.frag and the late
// warp's vr_eye_upscale.comp. The presenter grows the reach with the build-up (they creep in).
inline constexpr float VeinReach = 0.62f;  // at a full build-up (fractions of the half-field)
inline constexpr float VeinWidth = 0.021f; // a trunk's width at the corner (the same fractions)
// Opacity of markings already inside the lens; reach and full-strength alpha are capped separately.
inline constexpr float VeinGain = 3.8f;
// The heartbeat (beats per second): calm at the first build-up, racing as it takes hold.
inline constexpr float VeinBeatHz = 0.85f, VeinBeatHzFull = 1.6f;
inline constexpr float VeinSwayBeats = 6.0f; // the slow sway's period, in beats
inline constexpr double VeinBeatWrap = 240.0; // the beat clock wraps here (a whole number of sways)

// The beat clock advanced `dt_s` at `grow` (0..1: how far the build-up is), wrapped.
inline double AdvanceBeats(double beats, float dt_s, float grow) {
    const float hz = VeinBeatHz + (VeinBeatHzFull - VeinBeatHz) * std::clamp(grow, 0.0f, 1.0f);
    beats += static_cast<double>(std::clamp(dt_s, 0.0f, 0.1f) * hz);
    return beats >= VeinBeatWrap ? beats - VeinBeatWrap : beats;
}

struct Vein {
    float alpha{0.0f}; // 0..1 (times the item's opacity)
    float shade{1.0f}; // the colour's multiplier: dark at a vessel's core
};

inline float VeinSmooth(float e0, float e1, float x) {
    const float t = std::clamp((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
// Multiplies only (Hoskins' hash11): cheap on the GPU, and alike there and here.
inline float VeinHash(float x) {
    float p = x * 0.1031f;
    p -= std::floor(p);
    p *= p + 33.33f;
    p *= p + p;
    return p - std::floor(p);
}
inline float VeinFract(float x) {
    return x - std::floor(x);
}
// One heartbeat's swell at `x` beats after it (0..1): a quick rise, a slower fall.
inline float VeinBeat(float x) {
    return VeinSmooth(0.0f, 0.03f, x) * (1.0f - VeinSmooth(0.03f, 0.22f, x));
}

// Slow poison's vessels (Veins; the status's look, StatusPoison, puts its colour and residue on
// them). At q, the fitted field's coordinates (per side, 1 at the displayed edge; any sign: the
// corner is the quadrant's), `reach` how far in they have crept, `beats` the beat clock
// (AdvanceBeats) and `width` a trunk's width.
inline Vein Veins(float qx, float qy, float reach, float beats, float width) {
    constexpr float Pi = 3.14159265f;
    constexpr float Origin = 1.12f; // they come in from beyond the corner: no starburst in view
    constexpr float K = 4.0f;       // trunks per corner
    const float cx = Origin - std::min(std::abs(qx), Origin);
    const float cy = Origin - std::min(std::abs(qy), Origin);
    const float r = std::sqrt(cx * cx + cy * cy);
    const float total = reach + (Origin - 1.0f) * 1.41f;
    if (!(r < total) || !(width > 0.0f)) return {};
    const float seed = (qx > 0.0f ? 1.0f : 0.0f) + (qy > 0.0f ? 2.0f : 0.0f);
    // Across the corner 0..1, meandering with the distance in (continuous: no seams), swaying
    // slowly, the tips most (tendrils in water).
    const float sway = std::sin(6.2831853f * VeinFract(beats * (1.0f / VeinSwayBeats)) +
                                seed * 1.7f + r * 2.0f);
    float u = std::atan2(cy, std::max(cx, 1e-6f)) * (2.0f / Pi);
    u += 1.3f * (0.10f * r * std::sin(seed * 1.9f + u * 3.0f) +
                 0.028f * std::sin(r * 8.0f + u * 7.0f + seed * 2.1f) +
                 0.009f * std::sin(r * 31.0f + u * 5.0f + seed * 5.3f)) +
         0.03f * r * r * sway;
    const float v = u * K;
    const float cell = std::floor(v);
    const float f = v - cell - 0.5f; // across this trunk's lane, -0.5..0.5
    const float h = VeinHash(cell + 7.0f * seed);
    const float h2 = VeinHash(cell + 7.0f * seed + 3.7f);
    const float arc = r * (Pi * 0.5f) / K; // a lane's width here
    const float t = r / total;
    // The trunk forks once (a side branch), then the main one sheds a twig; all in its lane.
    const float sd = h > 0.5f ? 1.0f : -1.0f;
    const float jit = (h2 - 0.5f) * 0.16f;
    const float r1 = total * (0.22f + 0.18f * h);
    const float s1 = VeinSmooth(r1, r1 + 0.12f * total, r);
    const float r2 = total * (0.52f + 0.16f * h2);
    const float s2 = VeinSmooth(r2, r2 + 0.10f * total, r);
    const float side_end = std::min(total, r1 + total * (0.40f + 0.25f * h2));
    const float twig_end = std::min(total, r2 + total * (0.22f + 0.15f * h));
    const float b_main = jit - sd * (0.04f + 0.08f * h2) * s1;
    const float at[3] = {b_main, jit + sd * (0.17f + 0.08f * h) * s1,
                         b_main + sd * (0.09f + 0.04f * h) * s2};
    const float wide[3] = {width * (1.0f - 0.82f * t),
                           width * 0.62f * (1.0f - std::pow(VeinSmooth(r1, side_end, r), 0.8f)),
                           width * 0.45f * (1.0f - std::pow(VeinSmooth(r2, twig_end, r), 0.8f))};
    // The heartbeat runs out from the corner (lub, then a softer dub): they swell and brighten
    // as it passes.
    const float x = VeinFract(beats - r * 1.4f);
    const float pulse = std::max(VeinBeat(x), 0.6f * VeinBeat(x - 0.3f));
    // Never thinner than about two pixels of a headset's eye: a thinner one fades instead (a hair
    // of full opacity would crawl and shimmer as the heartbeat swells it).
    constexpr float MinWidth = 0.0025f;
    Vein out{};
    for (int i = 0; i < 3; ++i) {
        const float thin = wide[i] * (0.92f + 0.3f * pulse);
        if (!(thin > 1e-4f)) continue;
        const float w = std::max(thin, MinWidth);
        const float d = std::abs(f - at[i]) * arc;
        const float a = std::min(thin / MinWidth, 1.0f) *
                        std::max(1.0f - VeinSmooth(0.25f * w, w, d), 0.16f * (1.0f - VeinSmooth(w, 3.0f * w, d)));
        if (a > out.alpha) {
            out.alpha = a;
            out.shade = 0.95f - 0.67f * (1.0f - VeinSmooth(0.0f, 0.5f * w, d));
        }
    }
    out.alpha *= (1.0f - VeinSmooth(0.8f * total, total, r)) * (0.72f + 0.28f * pulse);
    out.shade *= 1.0f + 0.5f * pulse;
    // The bruise: a faint darkening at the displayed corner itself, under the vessels.
    const float vx = 1.0f - std::min(std::abs(qx), 1.0f);
    const float vy = 1.0f - std::min(std::abs(qy), 1.0f);
    const float b = 1.0f - VeinSmooth(0.0f, 0.42f * std::max(reach, 0.05f), std::sqrt(vx * vx + vy * vy));
    if (0.30f * b * b > out.alpha) out = {0.30f * b * b, 0.25f};
    return out;
}

// Each status has its own shape and colour. Broken edges and dark cores keep the effect
// readable against the scene without bright highlights.

// A colour (linear rgb) and its opacity (straight alpha).
struct Corner {
    std::array<float, 3> rgb{};
    float alpha{0.0f};
};

inline float VeinMix(float a, float b, float k) {
    return a + (b - a) * k;
}
inline std::array<float, 3> VeinMix(const std::array<float, 3>& a, const std::array<float, 3>& b,
                                    float k) {
    return {VeinMix(a[0], b[0], k), VeinMix(a[1], b[1], k), VeinMix(a[2], b[2], k)};
}
// Smooth value noise (four hashes).
inline float VeinNoise(float x, float y) {
    const float ix = std::floor(x), iy = std::floor(y);
    const float fx = x - ix, fy = y - iy;
    const float ux = fx * fx * (3.0f - 2.0f * fx), uy = fy * fy * (3.0f - 2.0f * fy);
    const float a = VeinHash(ix * 127.1f + iy * 311.7f);
    const float b = VeinHash((ix + 1.0f) * 127.1f + iy * 311.7f);
    const float c = VeinHash(ix * 127.1f + (iy + 1.0f) * 311.7f);
    const float d = VeinHash((ix + 1.0f) * 127.1f + (iy + 1.0f) * 311.7f);
    return VeinMix(VeinMix(a, b, ux), VeinMix(c, d, ux), uy);
}
// The grime that breaks every edge (two scales of the noise), 0..1.
inline float VeinGrime(float cx, float cy, float seed) {
    return 0.65f * VeinNoise(cx * 38.0f + seed * 3.1f, cy * 38.0f) +
           0.35f * VeinNoise(cx * 110.0f + 7.0f, cy * 110.0f + seed);
}
// A stroke of half-width w at distance d: its opacity (a faint halo round it) and
// shade (dark at its core).
inline Vein VeinLine(float d, float w, float halo) {
    w = std::max(w, 0.0025f);
    const float core = 1.0f - VeinSmooth(0.25f * w, w, d);
    return {std::max(core, halo * (1.0f - VeinSmooth(w, 3.0f * w, d))),
            0.95f - 0.67f * (1.0f - VeinSmooth(0.0f, 0.5f * w, d))};
}
// `rgb` at opacity `a` over `under`.
inline Corner VeinOver(const Corner& under, const std::array<float, 3>& rgb, float a) {
    a = std::clamp(a, 0.0f, 1.0f);
    const float out_a = a + under.alpha * (1.0f - a);
    Corner out{{}, out_a};
    for (int i = 0; i < 3; ++i)
        out.rgb[i] = (rgb[i] * a + under.rgb[i] * under.alpha * (1.0f - a)) / std::max(out_a, 1e-6f);
    return out;
}
inline float VeinCore(float shade) {
    return std::clamp((0.95f - shade) / 0.67f, 0.0f, 1.0f);
}

// Slow poison: clotted olive vessels with patchy residue along the rims.
inline Corner StatusPoison(float qx, float qy, float reach, float beats, float width, float g) {
    const Vein v = Veins(qx, qy, reach, beats, width);
    const float core = VeinCore(v.shade);
    const float residue = (1.0f - core) * VeinSmooth(0.28f, 0.72f, g);
    const auto rgb = VeinMix({0.043f, 0.055f, 0.025f}, {0.21f, 0.29f, 0.09f}, residue);
    return {rgb, v.alpha * (0.70f + 0.30f * g)};
}

// Rapid poison: fine bruised capillaries, with broken branches over a dark stain.
inline Corner StatusWeb(float qx, float qy, float reach, float beats, float g) {
    constexpr float Pi = 3.14159265f, Origin = 1.12f;
    const float ox = Origin - std::min(std::abs(qx), Origin);
    const float oy = Origin - std::min(std::abs(qy), Origin);
    const float r = std::sqrt(ox * ox + oy * oy);
    const float total = reach + 0.17f;
    if (!(r < total)) return {};
    const float seed = (qx > 0.0f ? 1.0f : 0.0f) + (qy > 0.0f ? 2.0f : 0.0f) + 4.0f;
    const float u = std::atan2(oy, std::max(ox, 1e-6f)) * (2.0f / Pi);
    constexpr float K = 9.0f;
    const float v = (u + 0.05f * std::sin(r * 9.0f + seed * 2.3f) +
                     0.012f * std::sin(r * 37.0f + u * 9.0f + seed)) * K;
    const float cell = std::floor(v);
    const float f = v - cell - 0.5f;
    const float arc = r * (Pi * 0.5f) / K;
    const float t = r / total;
    const float dt = std::abs(f - (VeinHash(cell + 7.0f * seed) - 0.5f) * 0.3f) * arc;
    Vein web = VeinLine(dt, 0.0055f - 0.0040f * t, 0.16f);
    const float rr = std::sqrt(std::max(r + 0.048f * std::sin(u * 14.0f + seed * 3.1f) +
                                            0.016f * std::sin(u * 43.0f + seed), 0.0f));
    const float ring = std::floor(rr / 0.06f);
    const float gd = (VeinFract(rr / 0.06f) - 0.5f) * 0.06f * 2.0f * rr; // d r = 2 sqrt(r) d sqrt(r)
    const float pulse = 0.85f + 0.15f * std::sin(6.2831853f * (beats - r * 1.2f));
    if (VeinHash(ring * 13.1f + cell * 3.7f + seed) > 0.35f) {
        const Vein ring_line = VeinLine(std::abs(gd), 0.004f * (1.0f - t), 0.16f);
        if (ring_line.alpha > web.alpha) web = ring_line;
    }
    const float a = std::clamp(web.alpha * (1.0f - VeinSmooth(0.75f * total, total, r)) * pulse * 1.15f,
                               0.0f, 1.0f);
    const auto rgb = VeinMix({0.23f, 0.14f, 0.31f}, {0.055f, 0.030f, 0.081f}, std::pow(VeinCore(web.shade), 0.7f));
    const float stain = 0.35f * (1.0f - VeinSmooth(0.17f, reach * 0.92f + 0.17f, r)) * g;
    return VeinOver({{0.036f, 0.018f, 0.048f}, stain}, rgb, a * (0.6f + 0.4f * g));
}

// Bleed: uneven smears at the top and clotted blood at the bottom.
inline Corner StatusBleed(float qx, float qy, float cx, float cy, float reach, float beats, float g) {
    const bool top = qy > 0.0f;
    const float seed = (qx > 0.0f ? 1.0f : 0.0f) + (top ? 2.0f : 0.0f) + 8.0f;
    Vein blood{};
    if (top) {
        constexpr float lane_w = 0.032f;
        const float lane = std::floor(cx / lane_w);
        const float h = VeinHash(lane + seed * 7.0f);
        const float h2 = VeinHash(lane + seed * 7.0f + 5.3f);
        const float by_corner = std::pow(std::clamp(1.0f - cx / (reach * 1.15f), 0.0f, 1.0f), 0.7f);
        if (h > 0.18f && by_corner > 0.0f) {
            const float len = (0.03f + reach * 0.97f * h * h + 0.018f * VeinBeat(VeinFract(beats - h))) * by_corner;
            const float xc = (lane + 0.5f + (h2 - 0.5f) * 0.45f) * lane_w + 0.003f * std::sin(cy * 41.0f + h * 11.0f);
            const float taper = std::clamp(cy / std::max(len, 1e-3f), 0.0f, 1.0f);
            const float w = (0.005f + 0.007f * h2) * (1.0f - 0.33f * taper) +
                            0.006f * (1.0f - VeinSmooth(0.0f, 0.03f, cy));
            const float dx = std::abs(cx - xc);
            blood = VeinLine(dx, w * (0.60f + 0.65f * g), 0.12f);
            blood.alpha *= 1.0f - VeinSmooth(0.76f * len, len, cy);
        }
    } else {
        float level = reach * 0.34f * std::pow(std::clamp(1.0f - cx / (reach * 0.9f), 0.0f, 1.0f), 1.8f);
        level *= 1.0f + 0.06f * std::sin(cx * 23.0f + beats * 1.4f + seed) +
                 0.025f * std::sin(cx * 61.0f - beats * 2.2f);
        level *= 0.78f + 0.44f * g;
        if (level > 0.002f) {
            const float under = level - cy;
            const float meniscus = 1.0f - VeinSmooth(0.0f, 0.005f, std::abs(under));
            blood = {std::max(VeinSmooth(-0.004f, 0.006f, under) * 0.6f, meniscus * (0.42f + 0.33f * g)),
                     meniscus > 0.5f ? 1.5f : 0.45f - 0.2f * VeinSmooth(0.0f, 0.08f, under)};
        }
    }
    auto rgb = VeinMix({0.23f, 0.037f, 0.030f}, {0.055f, 0.007f, 0.010f}, VeinCore(blood.shade) * 0.8f);
    rgb = VeinMix(rgb, {0.34f, 0.078f, 0.059f}, std::clamp(blood.shade - 1.2f, 0.0f, 1.0f) * 0.7f); // wet glint
    Corner o{rgb, blood.alpha * (0.75f + 0.25f * g)};
    if (top) {
        constexpr float cs = 0.03f;
        const float gx = std::floor(cx / cs), gy = std::floor(cy / cs);
        const float h = VeinHash(gx * 7.3f + gy * 13.7f + 41.0f);
        const float h2 = VeinHash(gx * 3.1f + gy * 5.9f + 43.0f);
        const float ox = (gx + 0.2f + 0.6f * h) * cs, oy = (gy + 0.2f + 0.6f * h2) * cs;
        const float dy = (cy - oy) * (1.0f + 0.6f * h);
        const float d = std::sqrt((cx - ox) * (cx - ox) + dy * dy);
        const float rad = cs * (0.06f + 0.16f * h2 * h2);
        if (VeinHash(gx * 1.7f + gy * 9.1f + 47.0f) <
            0.9f - 2.2f * std::sqrt(ox * ox + oy * oy) / std::max(reach, 1e-3f))
            o = VeinOver(o, {0.16f, 0.025f, 0.021f}, (1.0f - VeinSmooth(rad * 0.6f, rad, d)) * 0.9f);
    }
    return o;
}

// Frenzy: branching fractures with a tarnished gold edge. The shape stays fixed;
// a soft pulse travels down the cracks as the effect builds.
inline Corner StatusFrenzy(float qx, float qy, float cx, float cy, float reach, float beats,
                           float g) {
    const float seed = (qx > 0.0f ? 1.0f : 0.0f) + (qy > 0.0f ? 2.0f : 0.0f) + 12.0f;
    const float along = 0.72f * cx + 0.69f * cy;
    const float across = -0.69f * cx + 0.72f * cy;
    if (along >= 1.02f * reach) return {};
    const float spread = 0.045f + 0.17f * along;
    const float band = across / spread;
    const float lane = std::clamp(std::floor(band + 0.5f), -2.0f, 2.0f);
    const float key = seed * 9.0f + lane * 11.0f;
    const float lane_reach = reach * (0.80f + 0.22f * VeinHash(key + 5.0f));
    const float taper = 1.0f - VeinSmooth(0.65f * lane_reach, lane_reach, along);
    const float bend = (VeinNoise(along * 19.0f, key) - 0.5f) * 0.38f +
                       (VeinNoise(along * 57.0f, key + 1.0f) - 0.5f) * 0.14f;
    const float u = along / std::max(lane_reach, 1e-3f);
    const float split = VeinSmooth(0.14f, 0.82f, u) * (VeinHash(key) - 0.5f) * 0.35f;
    const float local = band - lane - bend;
    const float d = std::min(std::abs(local), std::abs(local - split) + 0.018f) * spread;
    const float w = (0.0045f + 0.0038f * VeinHash(key + 2.0f)) * (0.6f + 0.4f * taper);
    const float crack = (1.0f - VeinSmooth(0.18f * w, w, d)) * taper;
    const float halo = 0.20f * (1.0f - VeinSmooth(w, 3.1f * w, d)) * taper;
    const float pulse = 0.86f + 0.14f * std::cos(6.2831853f * (VeinFract(beats) - along * 0.8f));
    const float edge = VeinSmooth(0.11f * w, 0.72f * w, d) * (0.46f + 0.54f * g);
    return {VeinMix({0.34f, 0.23f, 0.115f}, {0.54f, 0.40f, 0.23f}, edge),
            (crack + halo * (1.0f - crack)) * pulse * (0.79f + 0.21f * g)};
}

// Beasthood: coarse dark fur combed out of the corner, three claw gashes torn through it (dark
// wet wounds, raw red edges), throbbing.
inline Corner StatusBeast(float qx, float qy, float cx, float cy, float reach, float beats, float g) {
    constexpr float Pi = 3.14159265f, Origin = 1.12f;
    const float ox = Origin - std::min(std::abs(qx), Origin);
    const float oy = Origin - std::min(std::abs(qy), Origin);
    const float r = std::sqrt(ox * ox + oy * oy);
    const float total = reach + 0.17f;
    const float seed = (qx > 0.0f ? 1.0f : 0.0f) + (qy > 0.0f ? 2.0f : 0.0f) + 16.0f;
    Corner o{};
    if (r < total) {
        const float u = std::atan2(oy, std::max(ox, 1e-6f)) * (2.0f / Pi);
        constexpr float K = 46.0f;
        const float v = (u + 0.02f * std::sin(r * 30.0f + seed)) * K;
        const float lane = std::floor(v);
        const float f = v - lane - 0.5f;
        const float shift = VeinHash(lane + seed);
        const float row = std::floor(r / 0.035f + shift);
        const float hr = VeinHash(lane * 3.7f + row * 11.3f + seed);
        const float tt = VeinFract(r / 0.035f + shift);
        const float d = std::abs(f - (hr - 0.5f) * 0.8f * tt) * r * (Pi * 0.5f) / K;
        const float strand = (1.0f - VeinSmooth(0.0012f, 0.0028f, d)) *
                             VeinSmooth(0.0f, 0.15f, tt) *
                             (1.0f - VeinSmooth(0.7f, 1.0f, tt));
        const float dens = 1.0f - VeinSmooth(total * 0.54f, total * 0.94f, r + 0.1f * (g - 0.5f));
        const float fur = (hr > 0.2f ? strand : 0.0f) * dens +
                          0.45f * dens * g * (1.0f - VeinSmooth(total * 0.3f, total * 0.8f, r));
        o = {VeinMix({0.041f, 0.030f, 0.019f}, {0.22f, 0.16f, 0.093f}, std::clamp(strand * hr, 0.0f, 1.0f)),
             std::min(fur, 0.9f)};
    }
    const float th = 38.0f * Pi / 180.0f + (VeinHash(seed) - 0.5f) * 0.6f;
    const float dx = std::cos(th), dy = std::sin(th);
    const float along = cx * dx + cy * dy;
    const float across = -cx * dy + cy * dx - 0.35f * (along - 0.15f) * (along - 0.15f);
    const float k = std::clamp(std::floor(across / 0.07f + 0.5f), -1.0f, 1.0f);
    const float hk = VeinHash(k * 5.1f + seed);
    const float s0 = 0.05f * hk + 0.03f * std::abs(k);
    const float s1 = s0 + reach * (0.75f + 0.25f * VeinHash(k * 2.3f + seed + 1.1f)) * (1.0f - 0.15f * std::abs(k));
    if (along > s0 && along < s1) {
        const float tt = std::clamp((along - s0) / std::max(s1 - s0, 1e-3f), 0.0f, 1.0f);
        const float rag = 1.0f + 0.35f * std::sin(along * 90.0f + hk * 13.0f) * std::sin(along * 37.0f + hk * 5.0f);
        const float w = std::max(0.02f * std::pow(std::max(0.0f, std::sin(Pi * tt)), 0.7f) * rag, 1e-4f);
        const float d = std::abs(across - k * 0.07f + (g - 0.5f) * 0.014f);
        const float wound = 1.0f - VeinSmooth(0.3f * w, 0.8f * w, d);
        const float torn = 1.0f - VeinSmooth(0.8f * w, 1.25f * w, d);
        const float a = std::max(torn, 0.2f * (1.0f - VeinSmooth(w, 2.5f * w, d))) *
                        (0.88f + 0.12f * std::sin(6.2831853f * beats - along * 10.0f));
        const auto flesh = VeinMix({0.36f, 0.048f, 0.036f}, {0.080f, 0.010f, 0.012f}, wound);
        o = VeinOver(o, flesh, a * (0.77f + 0.23f * g));
    }
    return o;
}

// The status `type` at q (Veins' coordinates): its look's colour (linear) and opacity. `width`:
// slow poison's trunk width. host_shaders/vr_status_veins.glsl StatusCorner mirrors this.
inline Corner StatusCorner(float qx, float qy, float reach, float beats, float width, int type) {
    const float cx = 1.0f - std::min(std::abs(qx), 1.0f); // in from the displayed corner
    const float cy = 1.0f - std::min(std::abs(qy), 1.0f);
    const float reach_max = 1.2f * reach + 0.2f;
    if (cx * cx + cy * cy > reach_max * reach_max) return {};
    const float g = VeinGrime(cx, cy, float(type + 1));
    if (type == Frenzy) return StatusFrenzy(qx, qy, cx, cy, reach, beats, g);
    if (type == RapidPoison) return StatusWeb(qx, qy, reach, beats, g);
    if (type == Bleed) return StatusBleed(qx, qy, cx, cy, reach, beats, g);
    if (type == Beasthood) return StatusBeast(qx, qy, cx, cy, reach, beats, g);
    return StatusPoison(qx, qy, reach, beats, width, g);
}

// ---- Into what the lenses show (2026-11-10: in the headset the red hit flash showed, the
// veins never). The corners of the rendered field lie outside what a headset's lenses show (on a
// Quest 3 about the outer quarter of each diagonal), and the looks, growing in from those corners
// with their substance near them, showed only faint tips. Drawn in a smaller field instead they
// floated ("a little weird appearing near the middle of the vision only"); the tester: "why not
// make them how we had them in the beginning just make them expand until they are visible?". So
// they are as first drawn, from the field's very corners, zoomed about each corner (Zoom: every
// size, stroke and reach that many times larger) by how much of the corner the lenses hide
// (HiddenBand): enough of each look reaches past it. The reach grows on screen past the hidden
// band, to VisibleSpan at a full build-up (ReachFor), so they reach no further in than they did.
// The lenses' edge on the corners' diagonals without the runtime's visibility mask (a Quest 3's,
// estimated; its mask measures it: LensDiagonal).
inline constexpr float LensDiagonalDefault = 0.75f;
inline constexpr float LensDiagonalMin = 0.45f;
inline constexpr float ZoomHidden = 0.2f; // of a look's own reach the lenses hide, at most
inline constexpr float ZoomMax = 2.2f;
// On screen past the hidden band at a full build-up (2026-11-10: "a tiny bit" further
// than 0.45).
inline constexpr float VisibleSpan = 0.48f;
// The band each corner's diagonal the lenses hide, in the looks' reach ((1 - diagonal) x sqrt 2).
inline float HiddenBand(float diagonal) {
    return (1.0f - std::clamp(diagonal, LensDiagonalMin, 1.0f)) * 1.41421356f;
}
// The zoom for a hidden band: at most ZoomHidden of a look hidden (1 when the lenses hide little).
inline float ZoomFor(float hidden) {
    return std::clamp(hidden / ZoomHidden, 1.0f, ZoomMax);
}
// A look's own reach at a build-up `grow` (0..1): on screen past the hidden band ReachStart of
// `span` at once (0.35 until the tester's 2026-11-27 "make them appear sooner", then halfway back
// from 0.5), creeping in to all of it; over the zoom.
inline constexpr float ReachStart = 0.425f;
inline float ReachFor(float hidden, float zoom, float grow, float span = VisibleSpan) {
    return (hidden + span * (ReachStart + (1.0f - ReachStart) *
                                              std::sqrt(std::clamp(grow, 0.0f, 1.0f)))) /
           std::clamp(zoom, 1.0f, ZoomMax);
}

// StatusCorner zoomed `zoom` times about each corner of the field (1: as drawn).
// host_shaders/vr_status_veins.glsl StatusCornerZoomed mirrors it.
inline Corner StatusCornerZoomed(float qx, float qy, float zoom, float reach, float beats,
                                 float width, int type) {
    const float z = std::clamp(zoom, 1.0f, ZoomMax);
    const float x = std::copysign(1.0f - (1.0f - std::min(std::abs(qx), 1.0f)) / z, qx);
    const float y = std::copysign(1.0f - (1.0f - std::min(std::abs(qy), 1.0f)) / z, qy);
    return StatusCorner(x, y, reach, beats, width, type);
}

// One eye's visible area (XR_KHR_visibility_mask's visible triangle mesh): on the z = -1 plane
// of its view (x right, y up: the tangents of the angles).
struct MaskPoint {
    float x{0.0f}, y{0.0f};
};
inline bool InsideMask(std::span<const MaskPoint> v, std::span<const std::uint32_t> tri, float x,
                       float y) {
    for (std::size_t i = 0; i + 2 < tri.size(); i += 3) {
        if (tri[i] >= v.size() || tri[i + 1] >= v.size() || tri[i + 2] >= v.size()) continue;
        const MaskPoint a = v[tri[i]], b = v[tri[i + 1]], c = v[tri[i + 2]];
        if (std::abs((b.x - a.x) * (c.y - a.y) - (c.x - a.x) * (b.y - a.y)) < 1e-12f) {
            continue; // degenerate: holds nothing
        }
        const float d1 = (x - b.x) * (a.y - b.y) - (a.x - b.x) * (y - b.y);
        const float d2 = (x - c.x) * (b.y - c.y) - (b.x - c.x) * (y - c.y);
        const float d3 = (x - a.x) * (c.y - a.y) - (c.x - a.x) * (y - a.y);
        const bool neg = d1 < 0.0f || d2 < 0.0f || d3 < 0.0f;
        const bool pos = d1 > 0.0f || d2 > 0.0f || d3 > 0.0f;
        if (!(neg && pos)) return true;
    }
    return false;
}
struct EyeMask {
    std::span<const MaskPoint> vertices;
    std::span<const std::uint32_t> indices;
};
// For the displayed field (degrees left, right, up, down; the union of the eyes'): how far out
// from the middle either eye still shows the directions (sx, sy) (each -1, 0 or 1; the same
// fraction s of both half-angles), the smallest of them. -1 (unknown) without a field or with
// either eye's area empty. Once per mask or field, not per frame.
inline float LensShown(const std::array<float, 4>& field, const std::array<EyeMask, 2>& eyes,
                       std::span<const std::array<float, 2>> directions) {
    if (!(field[0] > 1.0f) || eyes[0].indices.size() < 3 || eyes[1].indices.size() < 3) {
        return -1.0f;
    }
    constexpr float Rad = 0.0174532925f;
    float fit = 1.0f;
    for (const auto& [sx, sy] : directions) {
        const float hx = sx < 0.0f ? field[0] : field[1];
        const float hy = sy > 0.0f ? field[2] : field[3];
        float shown = 0.0f; // the last step shown, out from the middle
        for (int i = 1; i <= 200; ++i) {
            const float s = static_cast<float>(i) / 200.0f;
            const float x = sx * std::tan(s * hx * Rad), y = sy * std::tan(s * hy * Rad);
            if (!InsideMask(eyes[0].vertices, eyes[0].indices, x, y) &&
                !InsideMask(eyes[1].vertices, eyes[1].indices, x, y)) {
                break;
            }
            shown = s;
        }
        fit = std::min(fit, shown);
    }
    return fit;
}
// Along the four corners' diagonals, in [LensDiagonalMin, 1]; 0 unknown.
inline float LensDiagonal(const std::array<float, 4>& field, const std::array<EyeMask, 2>& eyes) {
    constexpr std::array<std::array<float, 2>, 4> Corners{
        {{-1.0f, 1.0f}, {1.0f, 1.0f}, {-1.0f, -1.0f}, {1.0f, -1.0f}}};
    const float shown = LensShown(field, eyes, Corners);
    return shown < 0.0f ? 0.0f : std::clamp(shown, LensDiagonalMin, 1.0f);
}

} // namespace VR::StatusCorners
