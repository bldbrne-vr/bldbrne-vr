// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <cmath>

// First-person eye height: the hunter's root height under the camera (bloodborne_camera.cpp).
// The camera follows the height of his model (W.t.y, BodyRootHeight), which follows stairs and
// slopes smoothly where the physics root steps. But W.t.y also depends on which way he faces: the
// game fits his model to the ground under his feet and eases it there over about half a second,
// so a turn in place (the body facing's idle turn past its deadzone, an attack or a lock-on
// turning him) raised or lowered the whole view by 2-3.4 cm while his physics root did not move
// at all (mock, 2026-10-15: every in-place turn moved W.t.y and the eye by the same amount; with
// the idle turn off W.t.y stayed within 0.6 mm). In the headset that read as the floor bobbing
// after a look around.
//
// RootHeightHold: the root height is the model's plus a residual. From the moment he turns in place
// (his yaw changes while the physics root stays put) the root height stays where it was: the
// residual takes up the model's easing. When the physics root moves again (more than StillM:
// walking, stairs, a dodge, a lift) the residual goes, on top of the model's own smooth height:
// first the floor pays it off (the model rising under a root held above it, or falling under one
// held below: the eye stays put while it does), and where the floor does not, it fades over FadeM
// of ground walked. So the eye never moves against the floor's own rise or drop and never follows
// the physics root's stair steps (mock, 2026-10-15: letting go in one tick dipped the eye 2.1 cm
// on a rising floor; a plain 0.4 m fade still 1.3 cm where the rise paused). A residual over
// JumpM (a teleport) is dropped at once. While the residual is not 0 the camera sits a few
// centimetres off the model's height; the body takes that up as camera_dip, so the hands stay
// with the camera.
//
// Pure (no emulator state); tests/test_root_height_hold.cpp drives it.
namespace VR {

class RootHeightHold {
public:
    // A turn in place: the model's yaw moved this much (radians) since the last step...
    static constexpr float TurnRad = 0.1f * 3.14159265f / 180.0f;
    // ... while the physics root stayed within this of where it was (metres).
    static constexpr float StillM = 0.002f;
    // Where the floor does not pay it off, the residual is gone after this much ground walked
    // (metres) once he moves again.
    static constexpr float FadeM = 0.75f;
    // A residual larger than this (metres) is dropped at once.
    static constexpr float JumpM = 0.3f;

    void Reset() {
        *this = {};
    }

    [[nodiscard]] bool Held() const {
        return held;
    }

    // `phys`: the physics root (x, y, z), `model_y`: the model's height (W.t.y), `yaw`: the model's
    // heading (radians), `scale`: game units per metre. Returns the root height for the camera.
    float Step(const float phys[3], float model_y, float yaw, float scale) {
        if (!(scale > 0.0f) || !std::isfinite(model_y) || !std::isfinite(yaw) ||
            !std::isfinite(phys[0]) || !std::isfinite(phys[1]) || !std::isfinite(phys[2])) {
            Reset();
            return model_y;
        }
        if (!valid) {
            valid = true;
            last_yaw = yaw;
            Copy(phys, last_phys);
            Copy(phys, anchor);
            last_root = model_y;
            last_model = model_y;
            return model_y;
        }
        const float step = Distance(phys, last_phys);
        // The fade counts ground covered: the physics root's own stair steps (up to ~18 cm at
        // once) are not distance walked.
        const float walked = std::hypot(phys[0] - last_phys[0], phys[2] - last_phys[2]);
        const float turn = std::remainder(yaw - last_yaw, 2.0f * 3.14159265f);
        const float still = StillM * scale;
        if (held) {
            if (Distance(phys, anchor) > still) {
                held = false; // he moves again: the residual goes from here
                fade_start = residual;
            } else {
                residual = last_root - model_y; // the root height stays
            }
        } else if (step <= still && std::abs(turn) > TurnRad) {
            held = true;
            Copy(phys, anchor);
            residual = last_root - model_y;
        }
        if (!held && residual != 0.0f) {
            const float floor = model_y - last_model;
            if ((residual > 0.0f) == (floor > 0.0f) && floor != 0.0f) {
                // The floor rises to a root held above it (or drops to one held below): the eye
                // stays put while it pays the residual off.
                const float pay = std::min(std::abs(floor), std::abs(residual));
                residual -= std::copysign(pay, residual);
            } else {
                const float fade = std::abs(fade_start) * walked / (FadeM * scale);
                residual -= std::copysign(std::min(fade, std::abs(residual)), residual);
            }
        }
        if (std::abs(residual) > JumpM * scale) {
            held = false;
            residual = 0.0f;
        }
        last_yaw = yaw;
        last_model = model_y;
        Copy(phys, last_phys);
        last_root = model_y + residual;
        return last_root;
    }

private:
    static void Copy(const float from[3], float to[3]) {
        to[0] = from[0];
        to[1] = from[1];
        to[2] = from[2];
    }
    static float Distance(const float a[3], const float b[3]) {
        const float dx = a[0] - b[0];
        const float dy = a[1] - b[1];
        const float dz = a[2] - b[2];
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    }

    bool valid{false};
    bool held{false};
    float residual{0.0f};   // the root height over the model's
    float fade_start{0.0f}; // the residual when he moved again (the fade's rate)
    float last_yaw{0.0f};
    float last_model{0.0f};
    float last_root{0.0f};
    float last_phys[3]{};
    float anchor[3]{}; // the physics root where the hold began
};

} // namespace VR
