// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

// The left stick click in the motion layout with left_scheme=1 (the presenter's UpdateGameplay;
// left_scheme=2 uses this class for Y instead, never sprinting): one button with three jobs.
//  - Pressed while sprinting (A held, the stick past half way): the sprint jump at once (Out::jump;
//    the presenter plays the game's jump, a Circle release and tap: core/vr/left_locomotion.h. The
//    game has no L3 jump: L3 while sprinting never jumped, from the VR layer or the keyboard).
//  - Held: the modifier for the combos (+ right stick = the D-pad, + B = the gestures, + Y = the
//    game's Square; Menu + it = MARK). The caller reports every combo that used it (NoteCombo).
//  - A tap (released with no combo, not a jump): with quick_arc_l3 (Config::arc) and a release
//    within tap_max_s it toggles the quick-item arc exactly like Y (the thumb stays on the stick,
//    so walking and dodging go on); the caller sends L3 to the game instead when the arc is not
//    available there (a cinematic, the classic layout, third person, no quick-item state). A
//    longer hold, or quick_arc_l3=0, sends L3 to the game on the release (as before 2026-10-05:
//    the game does nothing with L3).
// Header-only and pure (no emulator dependencies): tests/test_stick_click.cpp.

namespace VR::StickClick {

struct Config {
    bool arc{true};        // quick_arc_l3: a tap toggles the quick-item arc (else L3)
    float tap_max_s{0.5f}; // quick_arc_l3_tap_ms: a release later than this is no tap
};

struct Frame {
    bool pressed{false};   // the click (gated: one held through a context switch reads false)
    bool sprinting{false}; // A held with the left stick past half way (at the press: a jump)
    float dt{0.0f};        // seconds since the last frame
};

struct Out {
    bool l3{false};       // one L3 press for the game this frame (the jump, or a release)
    bool jump{false};     // ... it is the sprint jump (on the press)
    bool arc{false};      // a tap: toggle the quick-item arc, as a Y press (L3 if it is refused)
    bool modifier{false}; // the click is held as the modifier this frame (not a jump)
    float held_s{0.0f};   // on the release frame: how long it was held (s)
};

class LeftClick {
public:
    Out Update(const Config& c, const Frame& f) {
        Out o{};
        if (down_) {
            held_s_ += f.dt;
        }
        if (f.pressed && !down_) {
            used_ = false;
            held_s_ = 0.0f;
            jump_ = f.sprinting;
            if (jump_) {
                o.l3 = o.jump = true; // the jump fires on the press
                used_ = true;
            }
        }
        if (!f.pressed && down_) {
            o.held_s = held_s_;
            if (!used_) {
                if (c.arc && held_s_ <= c.tap_max_s) {
                    o.arc = true;
                } else {
                    o.l3 = true;
                }
            }
            jump_ = false;
        }
        down_ = f.pressed;
        o.modifier = f.pressed && !jump_;
        return o;
    }

    // A combo used the held modifier (or the click went to Menu's MARK): its release is nothing.
    void NoteCombo() {
        used_ = true;
    }

    // A context switch: whatever was held is forgotten (the gate holds the click back until it is
    // released, so that release is never seen).
    void Reset() {
        down_ = used_ = jump_ = false;
        held_s_ = 0.0f;
    }

    [[nodiscard]] bool Down() const {
        return down_;
    }
    [[nodiscard]] bool Used() const {
        return used_;
    }
    [[nodiscard]] float HeldS() const {
        return held_s_;
    }

private:
    bool down_{false};
    bool used_{false};
    bool jump_{false};
    float held_s_{0.0f};
};

} // namespace VR::StickClick
