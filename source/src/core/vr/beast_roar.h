// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace VR::BeastRoar {
inline constexpr int Goods = 2020, Model = 11013;
inline constexpr unsigned Bones = 16, Frames = 255;
inline constexpr unsigned AtlasSize = 512, AtlasLevels = 5, TextureBytes = 349184;
inline constexpr unsigned MaxVertices = 8192, MaxIndices = 36000, MaxDecodedBytes = 2 * 1024 * 1024;
inline constexpr float NormalStart = 1, VictoryStart = 47, IdleStart = 110;
// The baked normal clip, measured on the skinned palette (fingertip reach from the wrist): rest
// at 1, curled into a fist by 6 (6 and 7 the same pose), opened again by 12 (fingers straight,
// the claw at its longest), held open to 27, relaxed back to rest by 46.
inline constexpr float FistFrame = 6.5f, OpenFrom = 7, OpenFrame = 12, OpenHeld = 27, RelaxEnd = 46;
// The roar is charged (the tester's 2026-11-03: held, the claw curls into a fist; the ring full,
// it bursts open, "visceral and powerful"): the trigger held this long, then the game's use.
inline constexpr float WindupSeconds = .6f;
// A successful use: when the game took it (Site A's reply). Failed presses never reach this.
struct Use {
    std::int64_t ns{};
    std::uint32_t count{};
};
// The claw's look this frame: its palette frame (and the idle pose it blends from as a charge
// starts), and a motion of the whole claw about the wrist: a uniform scale, a push along the
// claw (metres, model +Z), and turns about the model's X (up and down) and Y (side to side).
struct Pose {
    float frame{IdleStart};
    float blend_frame{-1.0f}, blend_weight{0.0f};
    float scale{1.0f};
    float push_m{0.0f};
    float pitch{0.0f}, yaw{0.0f}; // radians
};
inline constexpr std::int64_t BurstNs = 1'250'000'000; // the burst, the open hold, the relax

inline float Smooth(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}
inline float EaseOut(float x) {
    x = 1.0f - std::clamp(x, 0.0f, 1.0f);
    return 1.0f - x * x * x;
}
// The held idle loop (staggered finger extensions and tremor), from its rest seam.
inline float IdleFrame(std::int64_t ns) {
    const auto idle = static_cast<std::uint64_t>(std::max<std::int64_t>(ns, 0));
    return IdleStart + static_cast<float>(idle % 4'800'000'000ULL) * (144.0f / 4.8e9f);
}
// A strained tremor (two sines, never in step: no fixed buzz), unit amplitude; `phase` turns
// one axis's apart from the other's.
inline float Tremor(float t, float hz, float phase) {
    constexpr float Tau = 6.2831853f;
    return .62f * std::sin(Tau * hz * t + phase) +
           .38f * std::sin(Tau * hz * 1.618f * t + 2.0f * phase);
}

// `charge_ns`: the trigger's hold began then (0: none); `use`: the last successful use. No state,
// no allocation: the same inputs give the same pose.
inline Pose Evaluate(std::int64_t now, std::int64_t charge_ns, const Use& use) {
    constexpr float Deg = 3.14159265f / 180.0f;
    Pose p{};
    const bool burst = use.count != 0 && use.ns > 0 && now >= use.ns && use.ns >= charge_ns &&
                       static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(use.ns) <
                           static_cast<std::uint64_t>(BurstNs);
    if (burst) {
        const float a = static_cast<float>(static_cast<std::uint64_t>(now) -
                                           static_cast<std::uint64_t>(use.ns)) * 1e-9f;
        // The fingers snap open in 70 ms (the clip took 140), hold rigid, then relax.
        p.frame = a < .07f   ? OpenFrom + (OpenFrame - OpenFrom) * EaseOut(a / .07f)
                  : a < .60f ? OpenFrame + (OpenHeld - OpenFrame) * (a - .07f) / .53f
                             : OpenHeld + (RelaxEnd - OpenHeld) * Smooth((a - .60f) / .60f);
        // The blast: the claw flung out and swelling at once (its peak about 35 ms in), swinging
        // back past its rest at 0.2 s and settling (a damped spring), with a shudder through the
        // open hold.
        const float kick =
            Smooth(a / .03f) * std::exp(-a / .16f) * std::cos(6.2831853f * a / .40f);
        const float shudder = std::exp(-a / .20f) * Smooth(a / .04f);
        p.scale = 1.0f + .16f * kick;
        p.push_m = .028f * kick;
        p.pitch = 8.0f * Deg * kick + 1.6f * Deg * shudder * Tremor(a, 21.0f, 0.0f);
        p.yaw = 1.6f * Deg * shudder * Tremor(a, 19.0f, 1.7f);
        return p;
    }
    p.frame = IdleFrame(now);
    // (A clench the last roar already answered is over, whatever the caller still reports.)
    if (charge_ns > 0 && now >= charge_ns && !(use.count != 0 && use.ns >= charge_ns)) {
        const float age = static_cast<float>(static_cast<std::uint64_t>(now) -
                                             static_cast<std::uint64_t>(charge_ns)) * 1e-9f;
        const float t = std::min(age / WindupSeconds, 1.0f);
        // Curled hard at once (most of the way in the first third), then ground tighter.
        p.frame = NormalStart + (FistFrame - NormalStart) * EaseOut(t / .55f);
        // From the idle pose it was in, over the first 0.12 s (none after: one pose to skin).
        p.blend_weight = 1.0f - Smooth(age / .12f);
        p.blend_frame = p.blend_weight > 0.0f ? IdleFrame(charge_ns) : -1.0f;
        // The strain rising: drawn back toward the wrist, squeezed smaller, cocked, shaking ever
        // harder as the ring fills (and held so while the game's use is on its way).
        const float s = Smooth((t - .25f) / .75f);
        p.scale = 1.0f - .05f * s;
        p.push_m = -.012f * s;
        p.pitch = -5.0f * Deg * s + 1.4f * Deg * s * s * Tremor(age, 17.0f, 0.0f);
        p.yaw = 1.2f * Deg * s * s * Tremor(age, 23.0f, 1.3f);
    }
    return p;
}
// The palette frame alone (stored copies, older callers).
inline float Frame(std::int64_t now, const Use& use) {
    return Evaluate(now, 0, use).frame;
}
} // namespace VR::BeastRoar
