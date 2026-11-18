// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// The Blacksky Eye in the hand (the tester's 2026-11-04 "retextured to be like a real human eye,
// with its eyeball moving around looking all jittery, then when we fire its pupil should dilate
// and move to where it shoots from"): our own eyeball (vr_hunter_relic_meshes.h BlackskyEye, a
// 24 mm human eye with its cornea, iris and a stub of optic nerve) turned in the fingers about
// its centre. Between shots it watches you (its line toward your eyes: pointed away like a gun,
// it would only ever show you its back), darting about your face with a tremor between saccades;
// fired, it whips onto the shot's line with its pupil blown wide, holds, then calms back to
// watching you. No allocation; one small state per held eye.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace VR::BlackskyEye {
inline constexpr int Goods = 2120;
inline constexpr float RadiusM = .012f; // the eyeball (mesh metres)
// The pupil in the shader's units (vr_inframe.frag ShadeEye: its radius in the iris' 0..1):
// resting a little narrow, blown nearly to the iris' edge at the shot.
inline constexpr float PupilRest = .30f, PupilWide = .78f;
inline constexpr std::int64_t SnapNs = 60'000'000;     // onto the shot's line (a whip)
inline constexpr std::int64_t DilateNs = 130'000'000;  // the pupil blown wide
inline constexpr std::int64_t HoldNs = 650'000'000;    // held on the line, wide
inline constexpr std::int64_t CalmNs = 1'100'000'000;  // back to its darting and rest size
inline constexpr float MaxGlanceRad = .38f;            // ~22 degrees off the line
inline constexpr float DegToRad = 3.14159265f / 180.0f;

struct Look {
    float yaw{}, pitch{};     // radians off its line (its own right, up)
    float fix{};              // its line: 0 toward you .. 1 the shot's
    float pupil{PupilRest};   // ShadeEye's pupil radius
    float flash{};            // the shot's light deep in the pupil (1 at the shot .. 0)
};

inline std::uint32_t Hash(std::uint32_t x) {
    x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16;
    return x;
}
inline float Smooth(float s) { s = std::clamp(s, 0.0f, 1.0f); return s * s * (3.0f - 2.0f * s); }

class Motion {
public:
    // `session`: the take (a new one starts it afresh); `fired_ns`: the game's yes to its last
    // shot (0: none). The same `now` twice (the second eye of a frame) gives the same look.
    Look Update(std::int64_t now, std::int64_t session, std::int64_t fired_ns) {
        if (session != session_ || now < move_ns_ - 2'000'000'000LL || now - next_ns_ > 2'000'000'000LL) {
            session_ = session;
            seed_ = Hash(static_cast<std::uint32_t>(session) ^ 0x9e3779b9U);
            Target(from_yaw_, from_pitch_);
            to_yaw_ = from_yaw_, to_pitch_ = from_pitch_;
            move_ns_ = now - 1'000'000'000LL;
            next_ns_ = now + Dwell();
        }
        for (int i = 0; i < 8 && now >= next_ns_; ++i) {
            from_yaw_ = to_yaw_, from_pitch_ = to_pitch_; // settled: a dwell outlasts a saccade
            Target(to_yaw_, to_pitch_);
            move_ns_ = next_ns_;
            next_ns_ += Dwell();
        }
        if (now >= next_ns_) next_ns_ = now + Dwell();
        Look look{};
        // The saccade: its length by the main sequence (~21 ms + 2.2 ms a degree), eased, with
        // a small settling wobble after it (the eye's glissade).
        const float amp = std::hypot(to_yaw_ - from_yaw_, to_pitch_ - from_pitch_) / DegToRad;
        const float span = (21.0f + 2.2f * amp) * 1e-3f;
        const float t = float(now - move_ns_) * 1e-9f;
        const float e = Smooth(t / span);
        const float wobble = t > span ? .06f * std::sin(6.2831853f * (t - span) / .06f) *
                                            std::exp(-(t - span) / .035f) : 0.0f;
        look.yaw = from_yaw_ + (to_yaw_ - from_yaw_) * (e + wobble);
        look.pitch = from_pitch_ + (to_pitch_ - from_pitch_) * (e + wobble);
        // The tremor between them: a fraction of a degree, never still.
        const float s = float(now % 60'000'000'000LL) * 1e-9f;
        look.yaw += .30f * DegToRad * (std::sin(s * 6.2831853f * 71.0f) * .6f + std::sin(s * 6.2831853f * 43.0f + 1.3f) * .4f);
        look.pitch += .30f * DegToRad * (std::sin(s * 6.2831853f * 59.0f + .7f) * .6f + std::sin(s * 6.2831853f * 37.0f + 2.1f) * .4f);
        // Its pupil breathes a little (hippus).
        look.pupil = PupilRest + .025f * std::sin(s * 6.2831853f * .43f) + .012f * std::sin(s * 6.2831853f * 1.1f);
        if (fired_ns > 0 && now >= fired_ns && now - fired_ns < HoldNs + CalmNs) {
            const std::int64_t age = now - fired_ns;
            const float on = age < HoldNs ? Smooth(float(age) / float(SnapNs))
                                          : 1.0f - Smooth(float(age - HoldNs) / float(CalmNs));
            const float wide = age < HoldNs ? 1.0f - std::pow(1.0f - std::clamp(float(age) / float(DilateNs), 0.0f, 1.0f), 3.0f)
                                            : 1.0f - Smooth(float(age - HoldNs) / float(CalmNs));
            look.yaw *= 1.0f - on;
            look.pitch *= 1.0f - on;
            look.fix = on;
            look.pupil += (PupilWide - look.pupil) * wide;
            look.flash = std::exp(-float(age) * 1e-9f / .18f);
        }
        return look;
    }
    void Reset() { *this = {}; }

private:
    float Random() { seed_ = seed_ * 1664525U + 1013904223U; return float(Hash(seed_) >> 8) * (1.0f / 16777216.0f); }
    // Mostly short, nervous dwells; now and then a longer stare (70 .. 520 ms).
    std::int64_t Dwell() { const float u = Random(); return static_cast<std::int64_t>((70.0f + 450.0f * u * u) * 1e6f); }
    // Mostly small darts round the line; one in four a wider glance.
    void Target(float& yaw, float& pitch) {
        const float reach = Random() < .25f ? MaxGlanceRad : .13f;
        yaw = (Random() * 2.0f - 1.0f) * reach;
        pitch = (Random() * 2.0f - 1.0f) * reach * .7f;
    }
    std::int64_t session_{}, move_ns_{}, next_ns_{};
    float from_yaw_{}, from_pitch_{}, to_yaw_{}, to_pitch_{};
    std::uint32_t seed_{1};
};

using Vec = std::array<float, 3>;
inline Vec Cross(const Vec& a, const Vec& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline float Dot(const Vec& a, const Vec& b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
inline bool Unit(Vec& a) {
    const float l = std::sqrt(Dot(a, a));
    if (!(l > 1e-6f) || !std::isfinite(l)) return false;
    for (auto& x : a) x /= l;
    return true;
}
// `from` turned toward `to` by `t` (0..1) along the great circle; opposite lines turn over
// `side` (their circle through it).
inline Vec Toward(Vec from, Vec to, Vec side, float t) {
    if (!Unit(from)) return to;
    if (!Unit(to) || t <= 0.0f) return from;
    if (t >= 1.0f) return to;
    Vec axis = Cross(from, to);
    float angle = std::atan2(std::sqrt(Dot(axis, axis)), Dot(from, to));
    if (!Unit(axis)) {
        if (Dot(from, to) > 0.0f) return to;
        axis = Cross(from, side);
        if (!Unit(axis)) return to;
        angle = 3.14159265f;
    }
    const float a = angle * t, c = std::cos(a), s = std::sin(a);
    const Vec x = Cross(axis, from);
    Vec out{};
    for (int i = 0; i < 3; ++i) out[i] = from[i] * c + x[i] * s; // axis is perpendicular to from
    return out;
}
// The eye's turn in its own mesh axes (columns: where its x, y, z go), its front (-Z) along its
// line (toward you, `watch`, turning onto the shot's `line` by the look's fix) turned by the
// look, kept upright (its y toward `up`): all in mesh axes. Without a line it keeps its pose.
inline std::array<Vec, 3> Turn(Vec watch, Vec line, Vec up, const Look& look) {
    std::array<Vec, 3> out{Vec{1, 0, 0}, Vec{0, 1, 0}, Vec{0, 0, 1}};
    if (!Unit(line)) line = watch;
    if (!Unit(watch)) watch = line;
    line = Toward(watch, line, up, look.fix);
    if (!Unit(line)) return out;
    if (!Unit(up) || std::abs(Dot(up, line)) > .98f) up = std::abs(line[1]) < .9f ? Vec{0, 1, 0} : Vec{1, 0, 0};
    Vec right = Cross(line, up);
    if (!Unit(right)) return out;
    const Vec top = Cross(right, line);
    const float cy = std::cos(look.yaw), sy = std::sin(look.yaw), cp = std::cos(look.pitch), sp = std::sin(look.pitch);
    Vec g{};
    for (int i = 0; i < 3; ++i) g[i] = line[i] * cy * cp + right[i] * sy * cp + top[i] * sp;
    if (!Unit(g)) return out;
    Vec y{};
    const float d = Dot(up, g);
    for (int i = 0; i < 3; ++i) y[i] = up[i] - d * g[i];
    if (!Unit(y)) return out;
    const Vec z{-g[0], -g[1], -g[2]};
    out = {Cross(y, z), y, z};
    return out;
}
} // namespace VR::BlackskyEye
