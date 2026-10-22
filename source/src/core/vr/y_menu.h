// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <cstdint>
#include <string_view>

// Y's own gesture (the presenter's UpdateInput, next to Menu's block: one machine for every
// context), on top of the click machine (core/vr/stick_click.h) that keeps Y held the modifier of
// the gestures (+ B) in left_scheme=2's gameplay.
//
// y_menu (SteamVR by default: its dashboard owns the left Menu button and Meta's the right
// Oculus button, so none of Menu's jobs reach the game): Y does Menu's jobs wherever Menu does
// them on Meta Link (Place(): gameplay, the game's menus, the VR page, and the virtual screen
// while the game shows its title, a loading screen or a cutscene).
//  - A tap (let go within hold_s, nothing else pressed meanwhile) = Options, the game's menu (a
//    cutscene's skip prompt, the game's menus open and closed).
//  - Held hold_s (0.7 s, Menu's) with nothing else pressed and both sticks left alone = the
//    recenter; the release after it is nothing. Y stays held as the gestures' modifier through
//    the recenter's context switch (KeepThroughRecenter: + B is then the gestures, never B's own
//    press).
//  - Held escape_s (2.5 s, Menu's MenuEscapeS) with nothing else pressed = the escape, once; the
//    release after it is nothing. The caller brings first-person VR back during a cinematic's
//    flat view and on the virtual screen chosen with Tab (there only while a title, loading or
//    cutscene shows on it: during gameplay Y is Triangle there). The virtual screen that comes up
//    by itself has no first-person view to go back to (it ends with the gameplay camera).
//  - Another button pressed during the hold (B: the gestures, X: the vial, A, a trigger, a grip,
//    a stick click) makes it a combo: none of them. A stick pushed from the centre during the
//    hold cancels only the recenter (a tap while walking or turning still opens the menu),
//    exactly as on Menu (menu_hold_cancelled).
//  - The grip adjust (A + B held: the weapon frozen) owns the hands: none of them.
// In the game's menus B alone is Back there (Y no longer doubles as it). The virtual screen the
// player chose with Tab during gameplay (the DS4 layout, a full pad) keeps Y as Triangle.
//
// Without y_menu (Meta Link: Menu is ours) the machine runs in left_scheme=2's gameplay only and
// Y's tap stays the quick-item arc (the click machine's). Hold Y + right stick (the old D-pad) and
// hold Y + X (the game's Square) are gone in both: the right stick pushed from the centre during
// the hold does its own job (turn, the item flick, blood bullets) and only makes the hold a combo
// (its release opens no arc), as the D-pad did. With y_menu, where the arc cannot open (third
// person, body=0, quick_arc=0, no quick-item state), X pressed while Y is held stays the game's
// Square (the presenter's y_x_combo: Y's tap, Square there before, is the menu).
// Header-only and pure: tests/test_y_menu.cpp.

namespace VR::YMenu {

// vr.ini y_menu and the env VR_Y_MENU, one parser: 0 = auto, 1 = on, 2 = off, -1 = not a y_menu
// word (auto / on, 1, true, yes / off, 0, false, no; any case).
constexpr int ParseSetting(std::string_view v) {
    while (!v.empty() && (v.front() == ' ' || v.front() == '\t')) {
        v.remove_prefix(1);
    }
    while (!v.empty() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) {
        v.remove_suffix(1);
    }
    const auto is = [v](std::string_view word) {
        if (v.size() != word.size()) {
            return false;
        }
        for (std::size_t i = 0; i < v.size(); ++i) {
            const char c = v[i] >= 'A' && v[i] <= 'Z' ? static_cast<char>(v[i] + 32) : v[i];
            if (c != word[i]) {
                return false;
            }
        }
        return true;
    };
    if (is("auto")) {
        return 0;
    }
    if (is("on") || is("1") || is("true") || is("yes")) {
        return 1;
    }
    if (is("off") || is("0") || is("false") || is("no")) {
        return 2;
    }
    return -1;
}

constexpr const char* SettingName(int setting) {
    return setting == 1 ? "on" : setting == 2 ? "off" : "auto";
}

// Where the machine runs this frame.
enum class Where : std::uint8_t {
    None,     // not at all (the DS4-layout virtual screen; menus and screens without y_menu)
    Gameplay, // gameplay: y_menu, or left_scheme=2's arc combos without it
    Menus,    // y_menu in the menu layout: the game's menus, the VR page, the virtual screen while
              // the game shows no gameplay camera (title, loading, a cutscene)
};

// `gameplay`: the gameplay context (no virtual screen, no game menu); `menu_layout`: the menu
// layout's context (a game menu or the VR page open, or the virtual screen with no gameplay
// camera). The virtual screen with a gameplay camera (chosen with Tab) is neither.
constexpr Where Place(bool y_menu, bool scheme2, bool gameplay, bool menu_layout) {
    if (gameplay) {
        return y_menu || scheme2 ? Where::Gameplay : Where::None;
    }
    return y_menu && menu_layout ? Where::Menus : Where::None;
}

// The right stick click pressed while the modifier is held: does it go to the game (lock-on on
// its tap, + the right stick: the weapon switch / the D-pad)? left_scheme=1 and classic swallow it
// (the modifier is the left stick click: L3 + R3 is a chord, the layout switch); left_scheme=2's Y
// has no right stick combo since 2026-10-15, so the click does its own job there and only spends
// Y's tap / hold (the caller's NoteCombo).
constexpr bool RightClickToGame(bool modifier_held, bool scheme2) {
    return !modifier_held || scheme2;
}

// How long the recenter's context switch holds new presses back (the pad's suppression). Normally
// `normal_s` (0.4 s: a button pressed in the fade does not land on the new view). Y's own
// recenter with Y kept (Gesture::KeepThroughRecenter) holds them back only until the game has seen
// the switch's releases (`min_release_s`, 1.5 game frames): Y + B tapped right after is the
// gestures, and a held-back touchpad press would drop it without a word.
constexpr float RecenterSuppressS(bool keep_y, float normal_s, float min_release_s) {
    return keep_y && min_release_s < normal_s ? min_release_s : normal_s;
}

struct Config {
    bool menu{false};     // y_menu: tap = Options, hold = the recenter, long hold = the escape
    float hold_s{0.7f};   // Menu's MenuHoldS
    float escape_s{2.5f}; // Menu's MenuEscapeS (0: none)
};

struct Frame {
    bool y{false};           // Y (gated, the shoulder switch's press already taken out)
    // Another button's press edge this frame: y_menu any (A, B, X, the stick clicks, triggers,
    // grips, Menu); without it the caller passes X's only (B and the right stick click note
    // their own combos there).
    bool other_press{false};
    float stick_l{0.0f};     // max(|x|, |y|) of each stick, logical
    float stick_r{0.0f};
    bool frozen{false};      // the grip adjust holds the weapon
    float dt{0.0f};
    // Y still held but taken away (`y` false: held back by a game menu's closing tail, a press
    // the shoulder switch took): no release, so never a tap; the hold is forgotten.
    bool taken{false};
};

struct Out {
    bool menu{false};          // y_menu: Options now (the tap's release)
    bool recenter{false};      // y_menu: the hold's recenter now
    bool escape{false};        // y_menu: the long hold now (the flat screen's way back to VR)
    bool combo{false};         // the hold became a combo this frame (the caller's NoteCombo)
    bool hold_cancelled{false}; // y_menu: a stick cancelled the hold's recenter this frame
    bool late_release{false};  // y_menu: let go after hold_s with the recenter cancelled: nothing
    float held_s{0.0f};        // on a release frame: how long Y was held
};

class Gesture {
public:
    Out Update(const Config& c, const Frame& f) {
        Out o{};
        if (f.y && !down_) {
            down_ = true;
            held_s_ = 0.0f;
            used_ = fired_ = escaped_ = cancelled_ = false;
            l_centred_ = r_centred_ = false;
        }
        if (!down_) {
            return o;
        }
        if (!f.y && f.taken) {
            Reset();
            return o;
        }
        if (f.y) {
            held_s_ += f.dt;
            l_centred_ = l_centred_ || f.stick_l < CentredBelow;
            r_centred_ = r_centred_ || f.stick_r < CentredBelow;
            if (c.menu) {
                if ((f.other_press || f.frozen) && !used_) {
                    used_ = true;
                    o.combo = true;
                }
                if (!cancelled_ && ((l_centred_ && f.stick_l > PushedAbove) ||
                                    (r_centred_ && f.stick_r > PushedAbove))) {
                    cancelled_ = true;
                    o.hold_cancelled = !fired_ && !used_;
                }
                if (held_s_ > c.hold_s && !used_ && !cancelled_ && !fired_) {
                    fired_ = true;
                    o.recenter = true;
                    o.combo = true; // the release is nothing (the click machine's too)
                }
                if (c.escape_s > 0.0f && held_s_ > c.escape_s && !used_ && !escaped_) {
                    escaped_ = true;
                    o.escape = true;
                }
            } else if (!used_ && (f.other_press || (r_centred_ && f.stick_r > DpadAbove))) {
                // X (the old Square: now the vial's own press) or the old D-pad's push (now the
                // stick's own job): the hold is a combo, its release opens no arc.
                used_ = true;
                o.combo = true;
            }
            return o;
        }
        // The release.
        o.held_s = held_s_;
        if (c.menu && !used_ && !fired_ && !escaped_) {
            if (held_s_ <= c.hold_s) {
                o.menu = true;
            } else {
                o.late_release = true;
            }
        }
        Reset();
        return o;
    }

    // A combo used the held Y (seen by the caller, e.g. Y + B): its release is nothing.
    void NoteCombo() {
        if (down_) {
            used_ = true;
        }
    }

    // A context switch: the held Y is forgotten (the gate holds it back until it is let go, so
    // that release is never seen).
    void Reset() {
        down_ = used_ = fired_ = escaped_ = cancelled_ = l_centred_ = r_centred_ = false;
        held_s_ = 0.0f;
    }

    [[nodiscard]] bool Down() const {
        return down_;
    }
    [[nodiscard]] float HeldS() const {
        return held_s_;
    }
    // The held Y did something (a combo, the recenter, the escape): the click machine's release
    // is no arc tap either.
    [[nodiscard]] bool Spent() const {
        return down_ && (used_ || fired_ || escaped_);
    }
    // This hold fired the recenter and Y is still down: the recenter's context switch keeps Y
    // (not held back, this machine as it is), so Y stays the gestures' modifier and the escape
    // still comes at escape_s.
    [[nodiscard]] bool KeepThroughRecenter() const {
        return down_ && fired_;
    }

    static constexpr float CentredBelow = 0.35f; // Menu's: a stick counts as centred
    static constexpr float PushedAbove = 0.5f;   // ... and as pushed (the hold's cancel)
    static constexpr float DpadAbove = 0.7f;     // the old Y + right stick D-pad's push

private:
    bool down_{false};
    bool used_{false};
    bool fired_{false};
    bool escaped_{false};
    bool cancelled_{false};
    bool l_centred_{false};
    bool r_centred_{false};
    float held_s_{0.0f};
};

} // namespace VR::YMenu
