// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

// A gamepad on the VR page (after ID-Gamer's third-person-gamepad patch, 2026-11-07): the page
// reads the gamepad (the presenter's UpdatePage) and the game gets none of it while the page is
// open, nor after it closes until every gamepad control is let go (the Circle that closed it
// must not also back out of the game's menu, a stick held through the close must not walk him).
//
// The emulated pad is one state for the gamepad, the keyboard and the presenter's own presses
// (its PadMapper: the VR controllers, the lamp and death prompts' answers), so the gate keeps the
// presenter's: Injected() is what it holds now, and only those buttons pass while it is shut.
namespace VR::PagePad {

// OrbisPadButtonDataOffset's bits (core/libraries/pad/pad.h; pad.cpp checks they match).
namespace Bit {
inline constexpr std::uint32_t L3 = 0x2, R3 = 0x4, Options = 0x8, Up = 0x10, Right = 0x20,
                               Down = 0x40, Left = 0x80, L2 = 0x100, R2 = 0x200, L1 = 0x400,
                               R1 = 0x800, Triangle = 0x1000, Circle = 0x2000, Cross = 0x4000,
                               Square = 0x8000, TouchPad = 0x100000;
}

// Semantic VR menu actions -> the native game's configured confirm/cancel buttons.
// The VR page itself keeps A/X = Confirm and B = Back. Gameplay and a physical
// gamepad's raw button stream are not translated by this helper.
struct MenuButtons {
    std::uint32_t confirm, back;
};
[[nodiscard]] constexpr MenuButtons NativeMenuButtons(bool circle_enter) {
    return circle_enter ? MenuButtons{Bit::Circle, Bit::Cross}
                        : MenuButtons{Bit::Cross, Bit::Circle};
}

// The presenter's held mask (PadMapper::Apply, its frame thread); any thread reads it.
inline std::atomic<std::uint32_t> g_injected{0};
inline void SetInjected(std::uint32_t mask) {
    g_injected.store(mask, std::memory_order_release);
}
[[nodiscard]] inline std::uint32_t Injected() {
    return g_injected.load(std::memory_order_acquire);
}

// A stick or trigger counts as held past half way (a resting stick's drift never keeps the gate
// shut). Axes as the emulated pad's: sticks 0 .. 255 (128 centre), triggers 0 .. 255.
inline constexpr int HeldAxis = 64;

// What the gamepad (and keyboard) hold beyond the presenter's own presses.
[[nodiscard]] constexpr bool PhysicalHeld(std::uint32_t buttons, std::uint32_t injected,
                                          const std::array<int, 6>& axes) {
    if ((buttons & ~injected) != 0) {
        return true;
    }
    for (int i = 0; i < 4; ++i) {
        if (axes[i] - 128 > HeldAxis || 128 - axes[i] > HeldAxis) {
            return true;
        }
    }
    const bool l2_ours = (injected & Bit::L2) != 0, r2_ours = (injected & Bit::R2) != 0;
    return (!l2_ours && axes[4] > HeldAxis) || (!r2_ours && axes[5] > HeldAxis);
}

// The game's side (pad.cpp, the game's pad reads): shut while the page is open, and after it
// closes until nothing physical is held. pad.cpp counts the page open only while the presenter
// is deciding (PadMenu::Taken: the headset off, the page stays up and the gamepad is the game's).
struct Gate {
    bool shut{false};
    bool Update(bool page_open, bool physical_held) {
        shut = page_open || (shut && physical_held);
        return shut;
    }
};

// Whether the gate is shut now (pad.cpp publishes it with every read; vr_session.cpp's stick
// turn takes nothing from a gamepad that is the page's).
inline std::atomic<bool> g_gate_shut{false};
inline void SetGateShut(bool shut) {
    g_gate_shut.store(shut, std::memory_order_relaxed);
}
[[nodiscard]] inline bool GateShut() {
    return g_gate_shut.load(std::memory_order_relaxed);
}

// The page's side (the presenter's UpdatePage): the gamepad as the page's controls.
// Left stick or D-pad: the stick (y up +); Cross confirms, Circle goes back, L1 / R1 the tabs,
// L2 / R2 held step the value (at the stick's pace).
struct Nav {
    float x{0.0f}, y{0.0f};
    bool confirm{false}, back{false}, tab_prev{false}, tab_next{false};
    bool value_prev_held{false}, value_next_held{false};
    bool used{false}; // a new press or the stick past half way: the gamepad's names show
};

// `now` / `prev`: the physical buttons this frame and the last (Injected() taken out).
[[nodiscard]] inline Nav ReadNav(std::uint32_t now, std::uint32_t prev, int left_x, int left_y) {
    Nav n{};
    const std::uint32_t pressed = now & ~prev;
    n.x = static_cast<float>(left_x - 128) / 127.0f;
    n.y = -static_cast<float>(left_y - 128) / 127.0f;
    if (now & Bit::Left) n.x = -1.0f;
    if (now & Bit::Right) n.x = 1.0f;
    if (now & Bit::Up) n.y = 1.0f;
    if (now & Bit::Down) n.y = -1.0f;
    n.confirm = (pressed & Bit::Cross) != 0;
    n.back = (pressed & Bit::Circle) != 0;
    n.tab_prev = (pressed & Bit::L1) != 0;
    n.tab_next = (pressed & Bit::R1) != 0;
    n.value_prev_held = (now & Bit::L2) != 0;
    n.value_next_held = (now & Bit::R2) != 0;
    n.used = pressed != 0 || n.x * n.x + n.y * n.y > 0.25f;
    return n;
}

// The page's button names for the gamepad in your hands (SDL's gamepad type).
enum class Kind { PlayStation, Xbox };
struct Names {
    std::string confirm, back, prev, next, value_prev, value_next;
};
[[nodiscard]] inline Names NamesFor(Kind kind) {
    if (kind == Kind::Xbox) {
        return {"A", "B", "LB", "RB", "LT", "RT"};
    }
    return {"Cross", "Circle", "L1", "R1", "L2", "R2"};
}

} // namespace VR::PagePad
