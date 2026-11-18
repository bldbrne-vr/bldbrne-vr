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

    static constexpr int Rest(int axis) {
        return axis >= 4 ? 0 : 128;
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
        return value;
    }
    bool InjectButton(std::uint32_t bit, bool pressed) {
        inj_buttons = pressed ? inj_buttons | bit : inj_buttons & ~bit;
        return pressed || (phys_buttons & bit) != 0;
    }
    int InjectAxis(int axis, int value) {
        inj_axis[axis] = value != Rest(axis);
        return inj_axis[axis] ? value : phys_axes[axis];
    }
};

} // namespace Input
