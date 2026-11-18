// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace VR::MessengerGift {
inline constexpr int Goods = 2110;
inline constexpr std::int64_t CrumbleNs = 850'000'000;
inline constexpr float BreakSeconds = .17f;
struct Use { std::int64_t ns{}, held_since{}; };
struct Visual { float seconds{-1.0f}, squeeze{}; bool active{}; };
// Camera-hook owned, fixed-size release frame. Captures once per successful use;
// no physics state or guest pointers. A coordinate-space change cancels old debris.
struct ReleasePose {
    Use use{};
    std::uint64_t space{};
    bool ready{}, aborted{};
    std::array<float,12> world{};
    std::array<float,3> gravity{};
    bool Sample(Use current, std::uint64_t current_space, float seconds,
                const std::array<float,12>& pose, const std::array<float,3>& down) {
        if(current.ns!=use.ns || current.held_since!=use.held_since) {
            *this={}; use=current; space=current_space;
        }
        if(current.ns<=0 || current_space!=space || !std::isfinite(seconds)) aborted=true;
        if(aborted || seconds<BreakSeconds) return false;
        if(!ready) {
            for(float v:pose) if(!std::isfinite(v)) { aborted=true; return false; }
            for(float v:down) if(!std::isfinite(v)) { aborted=true; return false; }
            world=pose; gravity=down; ready=true;
        }
        return true;
    }
};
inline Use Confirm(std::int64_t now, std::int64_t request_ms, std::int64_t held_since) {
    if (held_since<=0 || now<held_since || request_ms<held_since/1'000'000 || request_ms>now/1'000'000)
        return {};
    return {now,held_since};
}
inline float Smooth(float x) { x=std::clamp(x,0.0f,1.0f); return x*x*(3-2*x); }
inline Visual Evaluate(Use use, std::int64_t held_since, std::int64_t now) {
    if (held_since<=0 || use.held_since!=held_since || use.ns<held_since || now<use.ns) return {};
    const auto age=static_cast<std::uint64_t>(now)-static_cast<std::uint64_t>(use.ns);
    if(age>=CrumbleNs) return {};
    const float t=static_cast<float>(age)*1e-9f;
    // Light pressure while solid; close fully only as the pieces leave the fingers.
    const float squeeze=.14f*Smooth(t/.17f)+.86f*Smooth((t-.17f)/.27f);
    return {t,squeeze*(1.0f-Smooth((t-.55f)/.30f)),true};
}
// Local game data: goods 2110 -> bullet 340 -> owner SpEffect 1500
// (indefinite disguise, special state 95). Child bullet 341's 1510 is movement only.
inline constexpr int DisguiseEffect = 1500;
inline constexpr float StoredRatio(int slot) { return slot == 2 ? .45f : .65f; }
inline constexpr std::int64_t FreshNs = 250'000'000;
inline std::atomic<std::int64_t> active_ns{};
inline bool Fresh(std::int64_t stamp, std::int64_t now) {
    return stamp > 0 && now >= stamp &&
           static_cast<std::uint64_t>(now) - static_cast<std::uint64_t>(stamp) < FreshNs;
}
// Main-player update only. Shares the existing heal/status effect scan; no new list walk.
inline void Publish(bool active, std::int64_t now) {
    active_ns.store(active ? now : 0, std::memory_order_relaxed);
}
inline bool Hidden(std::int64_t now) {
    return Fresh(active_ns.load(std::memory_order_relaxed), now);
}
} // namespace VR::MessengerGift
