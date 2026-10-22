// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <array>
#include <memory>
#include <string>

#include "common/types.h"
#include "core/vr/thumb_layout.h"
#include "core/vr/vr_page_render.h"

// The VR settings page at runtime (vr.ini vr_menu=1): opened by A on the Options grid's VR tile
// (core/vr/bloodborne_vrtile.cpp), owned by the OpenXR presenter while it is up (it gets the
// controllers, the game gets a neutral pad), drawn on a worker thread (vr_page_render.h) and shown
// on its own layer at the menu canvas (renderer_vulkan/vr_page_layer.h). Values: the live settings
// (applied at once) and vr.ini (VR::SaveSetting / RemoveSetting, the safe writer).
namespace VR::Page {

// One input frame of the page (frame thread): presses as edges, the D-pad with its repeat.
struct Input {
    bool up{false}, down{false}, left{false}, right{false}; // a step (first press or a repeat)
    bool confirm{false};                                    // A (or X)
    bool back{false};                                       // B (or Y)
    bool tab_prev{false};                                   // the left grip
    bool tab_next{false};                                   // the right grip
    // The triggers: the highlighted row's previous / next value, as the stick pushed left / right
    // (the press, then held a repeat at the stick's pace; the pointing trigger only while its ray
    // is not on the page).
    bool value_prev{false}; // the left trigger
    bool value_next{false}; // the right trigger
    // The right controller's ray on the page (stage px of the page), and its trigger.
    bool ray_on_page{false};
    float ray_x{0.0f};
    float ray_y{0.0f};
    bool trigger_pressed{false}; // the right trigger's press edge with the ray on the page
    bool trigger_held{false};
    bool trigger_released{false};
    // The pointing trigger on the page but on nothing that reacts (the panel, the help line):
    // the value step it is then (+1: the right trigger's next value).
    int trigger_step{1};
    // Show VR controls (core/vr/controls_view.h): the controllers in your hands this frame,
    // physical (Quest Touch names, as bound, before any stick swap): pressed, squeezed or pushed,
    // and touched (the capacitive sensors), and the hands' speeds (m/s). `live`: filled.
    bool live{false};
    std::array<bool, Thumbs::ControlCount> held{};
    std::array<bool, Thumbs::ControlCount> touched{};
    float right_speed{0.0f};
    float left_speed{0.0f};
    float dt{0.0f}; // seconds since the last input frame
};

enum class Event : unsigned char {
    None,
    Closed,      // B on the page: back to the Options grid
    CloseAll,    // (Menu): the page closed, the presenter presses Options to close the grid too
    Clicked,     // a pointer click acted (a tick on the right controller)
};

class Runtime {
public:
    static Runtime& Get();

    // The VR tile's handler (the game's menu thread): one request per A press.
    void RequestOpen();
    // Any thread: seconds since the last RequestOpen (huge when none yet). The game draws 1-4
    // frames of its menu without the menu right after the A; the UI layer holds them.
    [[nodiscard]] double SecondsSinceRequest() const;
    // Frame thread, every XR frame: takes a pending request (opens the page; true then).
    bool PollOpen();
    [[nodiscard]] bool IsOpen() const;
    // The page's layer is up (open, or fading out after a close).
    [[nodiscard]] bool Visible() const;
    void Close(const char* why);
    // Frame thread: the page's input (only while open). Returns what the presenter must do.
    Event HandleInput(const Input& in);
    // Frame thread: fades and the cursor pulse. `dt` seconds.
    void Tick(float dt);
    // 0..1: the page layer's opacity (the game's page fade-in / fade-out, 0.2 s).
    [[nodiscard]] float Opacity() const;
    // The page's slide while it fades in (stage px down; 0 in VR: it appears in place).
    [[nodiscard]] float SlideStagePx() const;
    // The selection glow's pulse (the game's cursor: 1 -> 0.4 -> 1 over a second).
    [[nodiscard]] float GlowPulse() const;
    // The controller's own names for the buttons (the presenter, from the interaction profile).
    void SetGlyphs(const Glyphs& glyphs);
    // The interaction profile bound for the controllers in your hands (and whether its trackpad
    // d-pad map was taken): the controls view draws that family and its buttons.
    void SetController(const std::string& profile, bool dpad);
    // Show VR controls is up (over the page; B returns to the page).
    [[nodiscard]] bool ControlsOpen() const;
    // A newer finished image than `serial` (the presenter uploads it).
    bool TakeImage(u64& serial, std::shared_ptr<const Image>& page,
                   std::shared_ptr<const Image>& glow, u64& lamp_revision);
    // Frame thread: revision of the visible page in an accepted XR submission, or zero if absent.
    void ImagePresented(u64 lamp_revision);
    // The game's grid closed under the page (or the game left the menu): the page goes too.
    void GameMenuClosed();
    // A help line until the next move or change.
    void SetNotice(const std::string& text);

    struct Stats {
        u32 opens{0};
        u32 closes{0};
        u32 renders{0};
        u32 changes{0};
        double render_ms_max{0.0};
        double render_ms_sum{0.0};
        u32 controls_opens{0};   // Show VR controls
        u32 controls_renders{0};
    };
    Stats GetStats() const;

private:
    Runtime();
    ~Runtime();
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace VR::Page
