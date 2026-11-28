// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <optional>

// The emulated pad's two layers (Input::GameController; header-only, tests/test_pad_layers.cpp):
// the gamepad's and the keyboard's own presses and sticks (physical), and the VR presenter's
// (injected, its PadMapper). The game sees a button held by either, and a stick or trigger the
// presenter drives off its rest (128; 0 for a trigger) over the physical one, which shows again
// when the presenter lets go. Before (one state, the last write won): a gamepad stick held through
// the ladder skip's climb was centred at its end until moved, and a gamepad event mid-climb took
// the climb's stick away.
namespace Input {

struct PadLayers {
    static constexpr int Axes = 6; // LeftX, LeftY, RightX, RightY, TriggerLeft, TriggerRight

    std::uint32_t phys_buttons{0};
    std::uint32_t inj_buttons{0};
    std::array<int, Axes> phys_axes{128, 128, 128, 128, 0, 0};
    std::array<bool, Axes> inj_axis{};
    // Left-handed play's gameplay (the VR presenter, SetMirrorX): the gamepad's and the keyboard's
    // own sticks reach the game mirrored left to right, as the world is; kept as pushed here.
    bool mirror_x{false};

    static constexpr int Rest(int axis) {
        return axis >= 4 ? 0 : 128;
    }
    // A stick's sideways value mirrored about its rest (128): 0..255 to 255..1.
    static constexpr int MirrorAxis(int value) {
        return value <= 0 ? 255 : value >= 256 ? 0 : (256 - value > 255 ? 255 : 256 - value);
    }
    // What the game gets of a physical axis now: LeftX and RightX mirrored in left-handed
    // gameplay, the rest as pushed.
    [[nodiscard]] int Shown(int axis, int value) const {
        return mirror_x && (axis == 0 || axis == 2) ? MirrorAxis(value) : value;
    }

    // The physical layer, in one read (Input::GameController::Physical).
    struct Physical {
        std::uint32_t buttons{0};
        std::array<int, Axes> axes{128, 128, 128, 128, 0, 0};
    };
    [[nodiscard]] Physical PhysicalNow() const {
        return {phys_buttons, phys_axes};
    }

    // Each returns what the game's state gets now (nullopt: it keeps what it has).
    bool PhysicalButton(std::uint32_t bit, bool pressed) {
        phys_buttons = pressed ? phys_buttons | bit : phys_buttons & ~bit;
        return pressed || (inj_buttons & bit) != 0;
    }
    std::optional<int> PhysicalAxis(int axis, int value) {
        phys_axes[axis] = value;
        if (inj_axis[axis]) {
            return std::nullopt; // the presenter drives it
        }
        return Shown(axis, value);
    }
    bool InjectButton(std::uint32_t bit, bool pressed) {
        inj_buttons = pressed ? inj_buttons | bit : inj_buttons & ~bit;
        return pressed || (phys_buttons & bit) != 0;
    }
    int InjectAxis(int axis, int value) {
        inj_axis[axis] = value != Rest(axis);
        return inj_axis[axis] ? value : Shown(axis, phys_axes[axis]);
    }
    // Left-handed gameplay begins or ends (`on`): whether it changed. The game then gets each
    // sideways stick the presenter is not driving again (Shown), so a stick held through the
    // change is mirrored (or not) at once.
    bool SetMirrorX(bool on) {
        if (mirror_x == on) {
            return false;
        }
        mirror_x = on;
        return true;
    }
};

} // namespace Input
