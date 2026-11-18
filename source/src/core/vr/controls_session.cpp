// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <fmt/format.h>

#include "common/singleton.h"
#include "core/vr/body_settings.h"
#include "core/vr/controls_session.h"
#include "core/vr/melee_settings.h"
#include "core/vr/quick_settings.h"
#include "core/vr/two_hand_settings.h"
#include "core/vr/vr_session.h"
#include "input/controller.h"

namespace VR::ControlsView {

Config CurrentConfig() {
    const auto& s = VR::GetSettings();
    const auto& q = VR::Quick::GetQuickSettings();
    const auto& m = VR::Melee::GetMeleeSettings();
    const auto& b = VR::Body::GetBodySettings();
    Config c;
    c.layout.left_scheme = s.left_scheme;
    c.layout.classic = s.controls_classic;
    c.layout.swap = s.swap_sticks;
    c.layout.inject = s.inject;
    // The presenter resolves y_menu=auto by the runtime (SteamVR: its dashboard owns Menu).
    c.layout.y_menu = VR::YMenuActive();
    c.layout.menu_runtime = VR::MenuOwnedByRuntime();
    c.quick_arc = q.quick_items && q.quick_arc;
    c.arc_l3 = q.arc_l3;
    c.belt_vial = q.belt_vial;
    c.grip_hold = q.grip_hold;
    c.x_fan = s.x_fan;
    c.right_grip_draw = s.right_grip_draw;
    c.optimized_two_hand = VR::TwoHand::GetTwoHandSettings().optimized_controls;
    c.trigger_hold = VR::TwoHand::GetTwoHandSettings().trigger_hold;
    c.swings = s.gestures;
    c.melee_physical = m.physical;
    c.menu_hold_screen = s.menu_hold_screen;
    c.physical_evasion = s.physical_evasion;
    c.lean_dodge = s.lean_dodge && !s.physical_evasion;
    c.grip_adjust = b.body && b.grip_adjust;
    c.snap_turn_deg = s.snap_turn_deg;
    c.seated = s.seated;
    // A gamepad play style with a gamepad connected (its type known at connect: UNKNOWN = none):
    // the gamepad's page, and the VR controllers' as that style plays them (no grip adjust: the
    // arms are the game's there).
    if (VR::CurrentPlayStyle() != VR::PlayStyle::Style::Motion) {
        c.grip_adjust = false;
        const SDL_GamepadType type =
            (*Common::Singleton<Input::GameControllers>::Instance())[0]->GamepadType();
        c.gamepad = type != SDL_GAMEPAD_TYPE_UNKNOWN;
        c.gamepad_xbox = Input::UsesXboxNames(type);
        c.third_person = !VR::IsFirstPerson();
    }
    return c;
}

std::string Describe(const Config& c, Family family, std::string_view profile, bool dpad) {
    return fmt::format("{} layout{}{}, inject={}, quick_arc={}, quick_arc_l3={}, belt_vial={}, "
                       "melee={}{}{}; {} controllers ({}{}){}",
                       c.layout.classic ? "classic" : "motion",
                       c.layout.classic ? "" : fmt::format(", left_scheme={}", c.layout.left_scheme),
                       c.layout.swap ? ", move with the right stick" : "", c.layout.inject ? 1 : 0,
                       c.quick_arc ? 1 : 0, c.arc_l3 ? 1 : 0, c.belt_vial ? 1 : 0,
                       c.melee_physical ? "physical" : "vanilla",
                       c.right_grip_draw ? ", right_grip=draw" : "",
                       c.layout.y_menu ? ", Y = the menu (y_menu)" : "", FamilyName(family),
                       profile.empty() ? std::string_view{"no profile bound yet"} : profile,
                       dpad ? ", trackpad d-pad" : "",
                       c.gamepad ? fmt::format("; a gamepad ({} names, {} person)",
                                               c.gamepad_xbox ? "Xbox" : "PlayStation",
                                               c.third_person ? "third" : "first")
                                 : std::string{});
}

} // namespace VR::ControlsView
