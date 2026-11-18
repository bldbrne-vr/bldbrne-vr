// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

namespace VR::CallBeyond {
inline constexpr int Goods = 2010, Model = 11014;
inline constexpr unsigned Bones = 16, Frames = 255;
inline constexpr unsigned TextureSize = 256, TextureLevels = 7, TextureBytes = 87376;
inline constexpr unsigned MaxVertices = 4096, MaxIndices = 18000, MaxDecodedBytes = 1024 * 1024;
inline constexpr float IdleStart = 1, WiggleStart = 97, ChargeStart = 145, ReadyFrame = 180;
inline constexpr float RecoverStart = 181, WiggleOtherStart = 211;
inline constexpr std::int64_t ChargeNs = 1'167'000'000, RecoveryNs = 966'666'667;
inline constexpr std::int64_t GrayTrimNs = 80'000'000;
inline constexpr float HeldScale = .48f;

// Presenter-owned state, published with the existing quick-item scene. No render-thread
// mutation or pointers into game memory. Session time rejects replies from a previous take.
struct State {
    std::int64_t published_ns{}, session_ns{}, charge_ns{};
    std::uint32_t serial{};
    bool held{}, pending{};
};
// Site A is the only producer; success is the game's confirmed UseGoods outcome.
struct Answer {
    std::int64_t ns{};
    std::uint32_t serial{};
    bool used{};
};
struct Visual {
    float frame{}, squeeze{}, glow{}, invocation{}, world_gray{};
};
inline float Smooth(float x) {
    x = std::clamp(x, 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}
inline bool Fresh(std::int64_t now, std::int64_t then) {
    return then > 0 && now >= then &&
           static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(then) < 250'000'000;
}
inline std::uint32_t Hash(std::uint32_t x) {
    x ^= 0x6d2b79f5u;
    x = (x ^ (x >> 16)) * 0x45d9f3bu;
    return x ^ (x >> 16);
}
// Every baked idle clip begins and ends in the same rest pose. Irregular burst slots
// therefore join without interpolating across unrelated palette frames.
inline float IdleFrame(std::int64_t now) {
    constexpr std::int64_t SlotNs = 3'200'000'000;
    now = std::max<std::int64_t>(now, 0);
    const auto slot = static_cast<std::uint64_t>(now / SlotNs);
    const float t = static_cast<float>(now % SlotNs) * 1e-9f;
    const auto h = Hash(static_cast<std::uint32_t>(slot));
    if ((slot & 1) && (h & 3)) {
        constexpr float BurstS = 1.6f;
        if (t < BurstS)
            return (h & 4 ? WiggleStart : WiggleOtherStart) +
                   (h & 4 ? 47.0f : 43.0f) * (t / BurstS);
        return IdleStart + 95.0f * ((t - BurstS) / (3.2f - BurstS));
    }
    return IdleStart + 95.0f * (t / 3.2f);
}
inline bool CurrentAnswer(const State& s, const Answer& a, std::int64_t now) {
    return a.ns > 0 && a.ns >= s.session_ns && now >= a.ns && a.serial == s.serial;
}
// The gathered world held until the stars come (2026-11-06: "keep it on until the first
// meteor/stars get cast/appear"): the newest launch of a Call Beyond star (bloodborne_gun.cpp:
// the game's own launch of this goods' bullets; steady ns, any thread). A cast holds the world
// full until a star launched after it (or StarLeadNs before: the same frame's race) shows,
// StarShowNs after its launch (a direct use launches at the cast; the stars are seen ~0.3 s
// later), at most StarWaitNs (no star: refused, interrupted, or tool_aim off), then lets it go.
inline std::atomic<std::int64_t> star_ns{0};
inline constexpr std::int64_t StarWaitNs = 2'000'000'000, StarLeadNs = 300'000'000;
inline constexpr std::int64_t StarShowNs = 450'000'000;
inline float HeldGray(float evaluated, bool cast, std::int64_t cast_ns, std::int64_t star,
                      std::int64_t now) {
    const bool shown = star + StarLeadNs >= cast_ns && now >= star + StarShowNs;
    const bool waiting = cast && cast_ns > 0 && now >= cast_ns && now - cast_ns < StarWaitNs &&
                         !shown;
    return waiting ? 1.0f : evaluated;
}
// The lift (2026-11-06: "make it brighter the moment I lift my hand up (like make it
// become much brighter with a color transition)"): from the cast the gathered gray flares to the
// stars' pale light over FlashRiseNs (vr.ini call_beyond_flash), and fades out with the world.
inline constexpr std::int64_t FlashRiseNs = 250'000'000;
inline float Flash(bool cast, std::int64_t cast_ns, std::int64_t now) {
    return cast && cast_ns > 0 && now >= cast_ns
               ? Smooth(static_cast<float>(now - cast_ns) / static_cast<float>(FlashRiseNs))
               : 0.0f;
}
inline Visual Evaluate(const State& s, const Answer& a, std::int64_t now) {
    if (!s.held || !Fresh(now, s.published_ns))
        return {};
    Visual v{IdleFrame(now)};
    if (CurrentAnswer(s, a, now)) {
        if (!a.used)
            return v;
        const auto elapsed = static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(a.ns);
        if (elapsed < RecoveryNs) {
            const float t = static_cast<float>(elapsed) / static_cast<float>(RecoveryNs);
            v.frame = RecoverStart + 29.0f * t;
            v.squeeze = 1.0f - Smooth((t - .55f) / .45f);
            v.invocation = 1.0f - Smooth(static_cast<float>(elapsed) / 380'000'000.0f);
            v.glow = .10f * v.squeeze + .35f * v.invocation;
            return v;
        }
        // Recovery ends at rest. Begin a new idle loop there, rather than jumping to
        // an unrelated point in the global wiggle schedule (up to 7 cm at the tail).
        v.frame = IdleFrame(static_cast<std::int64_t>(elapsed - RecoveryNs));
    }
    if (s.pending) {
        v.world_gray = 1.0f;
        v.frame = ReadyFrame;
        v.squeeze = 1.0f;
        v.glow = .10f;
    } else if (s.charge_ns > 0 && now >= s.charge_ns) {
        const auto elapsed =
            static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(s.charge_ns);
        const float p = std::min(1.0f, static_cast<float>(elapsed) / static_cast<float>(ChargeNs));
        // Tighter (2026-11-06: "starts maybe 80ms late and finish 80ms early"): the gray
        // comes on GrayTrimNs into the gathering and is full GrayTrimNs before it is gathered.
        v.world_gray = Smooth((static_cast<float>(elapsed) - static_cast<float>(GrayTrimNs)) /
                              static_cast<float>(ChargeNs - 2 * GrayTrimNs));
        v.frame = ChargeStart + 35.0f * p;
        v.squeeze = Smooth(p / .18f);
        v.glow = .10f * Smooth((p - .70f) / .30f);
    }
    return v;
}
// The gray world let go (the tester's 2026-11-05 "the shader transition when call to beyond is cast
// is wayyy too abrupt": Evaluate drops it to none on the cast's frame). It follows Evaluate up;
// once that falls (the cast landed, or the gathering let go) the colour comes back from wherever
// the gray was over `fade_ns`, eased (slow at first, so the cast's moment stays gray), unless
// Evaluate rises over it again. The same `now` twice (the second eye) gives the same value; a new
// take or a gap over a second starts afresh.
class GrayFade {
public:
    float Update(std::int64_t now, std::int64_t session, float target, std::int64_t fade_ns) {
        target = std::clamp(target, 0.0f, 1.0f);
        if (now == last_ns_ && session == session_) {
            return shown_;
        }
        if (session != session_ || now < last_ns_ || now - last_ns_ > 1'000'000'000LL) {
            session_ = session;
            from_ns_ = 0;
            shown_ = target;
        }
        last_ns_ = now;
        if (from_ns_ == 0 && target < shown_ && fade_ns > 0) {
            from_ns_ = now;
            from_ = shown_;
            fade_ns_ = fade_ns;
        }
        float fading = 0.0f;
        if (from_ns_ != 0) {
            fading = from_ * (1.0f - Smooth(static_cast<float>(now - from_ns_) /
                                            static_cast<float>(fade_ns_)));
            if (target >= fading || fading <= 0.0f) {
                from_ns_ = 0;
            }
        }
        shown_ = std::max(target, fading);
        return shown_;
    }

private:
    std::int64_t last_ns_{}, session_{}, from_ns_{}, fade_ns_{1};
    float from_{}, shown_{};
};
inline float StoredRatio(int slot) {
    return (slot == 2 ? .26f : .32f) / HeldScale;
}
// Same approved subtle squeeze as the real-hunter preview. Call on a local Shape copy;
// the existing 15-joint loop applies it, without a second skeleton or per-frame allocation.
template <class Shape>
inline void Tighten(Shape& shape, float weight) {
    const float k = std::clamp(weight, 0.0f, 1.0f) * .01745329252f;
    constexpr float delta[5][3]{
        {-1, 3, 2}, {2.5f, 4, 2.5f}, {2.5f, 4, 2.5f}, {2.5f, 4, 2.5f}, {2.5f, 4, 2.5f}};
    for (unsigned f = 0; f < 5; ++f)
        for (unsigned j = 0; j < 3; ++j)
            shape.bend[f][j] += delta[f][j] * k;
}
} // namespace VR::CallBeyond
