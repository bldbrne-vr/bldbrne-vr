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

// The easter egg (2026-11-25: "if we shake our hand while holding the blacksky eye, it
// gets dizzy ... a little comical ... extremely lightweight"): a shake (the hand's acceleration
// past ShakeAccel, kept up for about a second; a quick flick fades before it shows) makes it
// dizzy, 0 .. 1. Motion then rolls the eye round in lazy circles, sagging, its pupil swimming,
// slowing as it wears off (DizzyDecay a second).
inline constexpr float ShakeAccel = 12.0f; // m/s^2: a hand's ordinary moves stay under it
inline constexpr float DizzyGain = .15f;   // a second, per m/s^2 past it
inline constexpr float DizzyDecay = .40f;  // a second
inline constexpr float DizzyShows = .20f;  // below this nothing shows (a flick, a bump)
using Vec = std::array<float, 3>;
class Dizzy {
public:
    // The held hand's position (tracking space, m) sampled at `ns` (its clock): its velocity from
    // the last three samples (a centred difference, quiet against tracking jitter; every runtime
    // has positions, not all report velocities: the mock's are zero), its acceleration from two
    // velocities. The same sample twice changes nothing. A new take (`session`) starts calm.
    // A tracking jump (faster than a hand moves: past MaxHandSpeed, or JumpM in one sample; a
    // recenter, a new `space`) restarts the history: never counted as a shake.
    static constexpr float MaxHandSpeed = 10.0f; // m/s
    static constexpr float JumpM = .25f;
    // `now`: the render clock. It wears off by that, held or untracked alike (held to your eye
    // the controller often loses its tracking: it still comes round); the hand's samples only add.
    float Update(std::int64_t session, std::int64_t now, std::int64_t ns, const Vec& p, bool valid,
                 std::uint32_t space = 0) {
        if (session != session_) *this = {}, session_ = session;
        if (worn_ns_ != 0 && now > worn_ns_)
            level_ = std::max(level_ - DizzyDecay * std::min(float(now - worn_ns_) * 1e-9f, .5f), 0.0f);
        worn_ns_ = std::max(worn_ns_, now);
        if (!valid || space != space_) {
            space_ = space;
            count_ = 0;
            has_v_ = false;
            if (!valid) return std::min(level_, 1.0f);
        }
        if (count_ > 0 && ns <= ns1_) return std::min(level_, 1.0f);
        if (count_ > 0) {
            const float dx = p[0] - p1_[0], dy = p[1] - p1_[1], dz = p[2] - p1_[2];
            const float step = std::sqrt(dx * dx + dy * dy + dz * dz);
            const float gap = float(ns - ns1_) * 1e-9f;
            if (!std::isfinite(step) || step > JumpM || step > MaxHandSpeed * std::max(gap, 1e-3f)) {
                count_ = 0; // from here afresh
                has_v_ = false;
            }
        }
        if (count_ > 0) {
            // The acceleration over the real time between the samples (a stall reads no larger);
            // what it adds over at most a tenth of a second of it.
            const float gap = std::max(float(ns - ns1_) * 1e-9f, 1e-3f);
            float grow = 0.0f;
            if (count_ >= 2) {
                const float span = std::max(float(ns - ns0_) * 1e-9f, 1e-3f);
                const Vec v{(p[0] - p0_[0]) / span, (p[1] - p0_[1]) / span, (p[2] - p0_[2]) / span};
                if (has_v_) {
                    const float dx = v[0] - v_[0], dy = v[1] - v_[1], dz = v[2] - v_[2];
                    const float accel = std::sqrt(dx * dx + dy * dy + dz * dz) / gap;
                    if (std::isfinite(accel)) grow = std::max(accel - ShakeAccel, 0.0f) * DizzyGain;
                }
                v_ = v;
                has_v_ = true;
            }
            level_ = std::clamp(level_ + std::min(grow, 6.0f) * std::min(gap, .1f), 0.0f, 1.3f);
        }
        p0_ = p1_, ns0_ = ns1_;
        p1_ = p, ns1_ = ns;
        count_ = std::min(count_ + 1, 2);
        return std::min(level_, 1.0f);
    }
    // How clear-headed it is for its aim's mark (blacksky_mark.h): 1 calm .. 0 dizzy; the mark
    // fades out as the dizziness shows and back as it wears off (2026-11-25: "if it is
    // dizzy it shouldn't have the aim help/dot until it is not dizzy anymore").
    float Calm() const {
        const float x = std::clamp((level_ - DizzyShows) / .30f, 0.0f, 1.0f);
        return 1.0f - x * x * (3.0f - 2.0f * x);
    }

private:
    std::int64_t session_{}, ns0_{}, ns1_{}, worn_ns_{};
    Vec p0_{}, p1_{}, v_{};
    float level_{};
    std::uint32_t space_{0};
    int count_{0};
    bool has_v_{false};
};

class Motion {
public:
    // `session`: the take (a new one starts it afresh); `fired_ns`: the game's yes to its last
    // shot (0: none); `dizzy`: Dizzy's level. The same `now` twice (the second eye of a frame)
    // gives the same look.
    Look Update(std::int64_t now, std::int64_t session, std::int64_t fired_ns, float dizzy = 0.0f) {
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
        // Dizzy: round and round, a little faster the dizzier, sagging, its pupil swimming.
        const float dt = now > spun_ns_ ? std::min(float(now - spun_ns_) * 1e-9f, .1f) : 0.0f;
        spun_ns_ = std::max(spun_ns_, now);
        const float d = Smooth((dizzy - DizzyShows) / (1.0f - DizzyShows));
        if (d > 0.0f) {
            spin_ = std::fmod(spin_ + dt * (2.5f + 6.5f * d), 6.2831853f);
            const float r = .42f * d; // ~24 degrees round its line
            look.yaw += (r * std::cos(spin_) - look.yaw) * d;
            look.pitch += (r * .75f * std::sin(spin_) - .12f * d - look.pitch) * d;
            // Swollen and swimming, nearly as wide as a shot's (2026-11-25: "We can
            // make the pupil swell more!"): .48 .. .76 of the iris, resting .30.
            look.pupil += (.62f + .14f * std::sin(spin_ * 1.7f) - look.pupil) * d;
        }
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
    std::int64_t session_{}, move_ns_{}, next_ns_{}, spun_ns_{};
    float from_yaw_{}, from_pitch_{}, to_yaw_{}, to_pitch_{}, spin_{};
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
