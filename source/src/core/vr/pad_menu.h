// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

// The gamepad's Options button as the VR controllers' Menu (2026-11-08: "If it is
// pressed once, it is passed as a simple press. If held - we do recenter, no menu passed"): the
// presenter takes the gamepad's Options from the game and decides on it, as it does for Menu.
//  - Let go before HoldS: one Options press to the game (on the release: only then is it a tap).
//  - Held HoldS (0.7 s, Menu's MenuHoldS): the recenter, and the game gets nothing.
//  - Held EscapeS (2.5 s, MenuEscapeS) on a flat screen: back to first-person VR (Tab's escape).
// While the presenter is not deciding (no headset view, the session unfocused, the free camera),
// the gamepad's Options goes to the game as before: Taken() lapses after StaleNs.
namespace VR::PadMenu {

inline constexpr float HoldS = 0.7f, EscapeS = 2.5f;

struct Out {
    bool tap{false};      // the release of a short press: one Options press to the game
    bool recenter{false}; // HoldS reached
    bool escape{false};   // EscapeS reached
};

struct Hold {
    bool down{false};
    float held{0.0f};
    bool recentered{false}, escaped{false};

    Out Step(bool pressed, float dt) {
        Out out{};
        if (pressed) {
            held = down ? held + dt : 0.0f;
            down = true;
            if (!recentered && held >= HoldS) {
                recentered = out.recenter = true;
            }
            if (!escaped && held >= EscapeS) {
                escaped = out.escape = true;
            }
        } else if (down) {
            out.tap = !recentered;
            *this = {};
        }
        return out;
    }

    // The presenter decides again (Taken() had lapsed: the free camera, the headset off, a reset):
    // a press already down went to the game or began outside our hold, so it is spent: its release
    // taps nothing and it never recenters.
    void Resume(bool pressed) {
        *this = {};
        if (pressed) {
            down = true;
            recentered = escaped = true;
        }
    }
};

// The presenter decides on the gamepad's Options (its frame thread, every input update); the
// game's pad reads (pad.cpp, any thread) hide it from the game only while that is recent.
inline constexpr std::int64_t StaleNs = 250'000'000;
inline std::atomic<std::int64_t> g_taken_ns{0};
inline std::int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}
inline void Take(std::int64_t now_ns = NowNs()) {
    g_taken_ns.store(now_ns, std::memory_order_release);
}
[[nodiscard]] inline bool Taken(std::int64_t now_ns = NowNs()) {
    const std::int64_t t = g_taken_ns.load(std::memory_order_acquire);
    return t != 0 && now_ns - t < StaleNs;
}

} // namespace VR::PadMenu
