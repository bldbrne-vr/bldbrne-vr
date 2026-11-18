// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "core/vr/hand_grip.h"

namespace VR::Augur {
inline constexpr int Goods = 2000;
inline constexpr float HeldScale = 1.02f;
inline constexpr float StoredRatio = .8f / HeldScale;
inline constexpr std::int16_t CrunchMs = 0;
inline constexpr std::int64_t EndNs = 1'250'000'000;
struct Use {
    std::int64_t ns{};
    std::int64_t held_since{};
    bool eaten{};
};
// The splatter out of the crushed Augur (0.32 s and near-black until 2026-11-04; then a small
// red one; the tester: "more exaggerated. Almost absurd! Cinematic too"): a burst out of the fist,
// heavy wet globs arcing far, long strands, a spray of drops, a fast fine mist and clouds of blood
// mist blooming, hanging a moment, then falling heavily. One draw: each particle a quad flown by
// the vertex shader (vr_inframe.vert AugurSprayVertex; its kind in the quad's z), no simulation.
inline constexpr float SpraySeconds = 1.2f;
enum DropKind : int { Mist, Blood, Strand, Glob, Puff };
struct Drop {
    std::array<float, 3> velocity; // in the fist's frame (y out of it); x 1.9 m/s in the shader
    float radius, stretch;         // metres (x the map's scale); a streak's length over its width
    DropKind kind;
};
inline constexpr std::size_t DropCount = 108;
inline const std::array<Drop, DropCount>& Drops() {
    static const std::array<Drop, DropCount> table = [] {
        std::array<Drop, DropCount> out{};
        const auto hash = [](std::uint32_t x) {
            x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
            return float(x & 0xFFFFFF) / float(0x1000000);
        };
        for (std::size_t i = 0; i < DropCount; ++i) {
            const std::uint32_t n = std::uint32_t(i) * 4u;
            // 10 globs, 18 strands, 40 drops, 34 mist, 6 puffs.
            const DropKind kind = i < 10 ? Glob : i < 28 ? Strand : i < 68 ? Blood : i < 102 ? Mist : Puff;
            // Out of the fist, mostly up and outward, every way round it.
            float x = 2.0f * hash(n + 1) - 1.0f, y = 0.25f + 0.95f * hash(n + 2), z = 2.0f * hash(n + 3) - 1.0f;
            const float len = std::sqrt(x * x + y * y + z * z);
            x /= len, y /= len, z /= len;
            const float h = hash(n + 4), h2 = hash(n + 5);
            float speed = 0.0f, radius = 0.0f, stretch = 1.0f;
            switch (kind) {
            case Glob: speed = 0.75f + 0.45f * h; radius = 0.007f + 0.005f * h2; stretch = 1.3f + 0.6f * h; break;
            case Strand: speed = 1.0f + 0.5f * h; radius = 0.0025f + 0.0015f * h2; stretch = 4.0f + 3.5f * h; break;
            case Blood: speed = 0.6f + 0.6f * h; radius = 0.003f + 0.003f * h2; stretch = 1.4f + 1.4f * h; break;
            case Mist: speed = 0.9f + 0.7f * h; radius = 0.0012f + 0.0009f * h2; stretch = 1.0f + 1.2f * h; break;
            case Puff: speed = 0.22f + 0.15f * h; radius = 0.02f + 0.012f * h2; stretch = 1.0f; break;
            }
            out[i] = {{x * speed, y * speed, z * speed}, radius, stretch, kind};
        }
        return out;
    }();
    return table;
}
inline float SprayAge(Use use,std::int64_t held_since,std::int64_t now) {
    if (use.eaten) return -1; // no screen-filling spray inside the headset
    if(held_since<=0 || use.held_since!=held_since || use.ns<held_since || now<use.ns)
        return -1;
    const auto elapsed=static_cast<std::uint64_t>(now)-static_cast<std::uint64_t>(use.ns);
    constexpr std::uint64_t start=std::uint64_t(CrunchMs)*1'000'000;
    if(elapsed<start || elapsed>=start+std::uint64_t(SpraySeconds*1e9f))return -1;
    return float(elapsed-start)*1e-9f;
}
// One immutable release frame, owned by the render-camera hook. No particle pool.
// Recentring, stale replies or resuming after the burst window cannot replay it.
struct Spray {
    Use use{};
    std::uint32_t space{};
    std::array<float,12> world{};
    bool captured{},aborted{};
    bool Sample(Use next,std::int64_t held_since,std::uint32_t serial,std::int64_t now,
                const std::array<float,12>& pose) {
        const float age=SprayAge(next,held_since,now);
        if(age<0)return false;
        if(next.ns!=use.ns || next.held_since!=use.held_since) {
            *this={};use=next;space=serial;
        }
        if(serial!=space)aborted=true;
        if(aborted)return false;
        if(!captured) {
            if(age>.060f){aborted=true;return false;}
            for(float value:pose)if(!std::isfinite(value)){aborted=true;return false;}
            world=pose;captured=true;
        }
        return true;
    }
};
// Request timestamps have millisecond precision. Capture the exact current take only
// if the request belongs to it; a late reply from a previous take cannot start a new cue.
inline Use Confirm(std::int64_t now, std::int64_t request_ms, std::int64_t held_since,
                   bool eaten = false) {
    if (held_since <= 0 || now < held_since || request_ms < held_since / 1'000'000 ||
        request_ms > now / 1'000'000)
        return {};
    return {now, held_since, eaten};
}
// Cosmetic only: draw the two-finger pinch into the palm, close the fist, then reopen.
// The game's cast, aim and parry timing do not wait for this animation.
inline float ConsumeProgress(Use use, std::int64_t held_since, std::int64_t now) {
    if (held_since <= 0 || use.held_since != held_since || use.ns < held_since || now < use.ns)
        return 0.0f;
    const auto elapsed = static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(use.ns);
    if (elapsed >= EndNs)
        return 0.0f;
    const float t = static_cast<float>(elapsed) * 1.0e-9f;
    const float x = t < .16f ? t / .16f : t < .85f ? 1.0f : 1.0f - (t - .85f) / .4f;
    return x * x * (3.0f - 2.0f * x);
}
inline float Squeeze(Use use, std::int64_t held_since, std::int64_t now) {
    return use.eaten ? 0.0f : ConsumeProgress(use, held_since, now);
}
inline std::array<float, 3> Stretch(float squeeze) {
    // Compress into the closed hand; do not generate a singular normal matrix.
    return {1.0f - .72f * squeeze, 1.0f - .80f * squeeze, 1.0f - .80f * squeeze};
}
inline bool Concealed(float squeeze) { return squeeze >= .985f; }
inline float Visibility(float squeeze) {
    const float t=std::clamp((squeeze-.72f)/(.985f-.72f),0.0f,1.0f);
    return 1.0f-t*t*(3.0f-2.0f*t);
}
// Palm-dummy coordinates (metres), independent of the model's held rotation.
inline Body::Vec3 DrawIn(float squeeze) {
    return Body::Vec3{-.012f, -.038f, -.040f} * squeeze;
}
inline void Tighten(Body::Grip::Shape& shape, float squeeze) {
    // Keep a rounded knuckle line on the hunter rig. A uniform 85-degree base curl
    // overfolds its low-poly finger skin; the outer fingers close progressively more.
    constexpr std::array<std::array<float, 3>, 5> fist{{
        {0, 65, 45}, {65, 85, 45}, {70, 90, 48}, {75, 85, 50}, {80, 80, 50}}};
    for (int f = 0; f < 5; ++f) {
        for (int j = 0; j < 3; ++j)
            shape.bend[f][j] += (fist[f][j] * Body::Grip::Deg - shape.bend[f][j]) * squeeze;
        shape.spread[f] *= 1.0f - squeeze;
    }
    shape.thumb_oppose += (25.0f * Body::Grip::Deg - shape.thumb_oppose) * squeeze;
}
} // namespace VR::Augur
