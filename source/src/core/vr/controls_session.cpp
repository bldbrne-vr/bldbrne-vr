// SPDX-License-Identifier: GPL-2.0-or-later

#include <fmt/format.h>

#include "core/vr/body_settings.h"
#include "core/vr/controls_session.h"
#include "core/vr/melee_settings.h"
#include "core/vr/quick_settings.h"
#include "core/vr/vr_session.h"

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
    c.right_grip_draw = s.right_grip_draw;
    c.swings = s.gestures;
    c.melee_physical = m.physical;
    c.menu_hold_screen = s.menu_hold_screen;
    c.physical_evasion = s.physical_evasion;
    c.lean_dodge = s.lean_dodge && !s.physical_evasion;
    c.grip_adjust = b.body && b.grip_adjust;
    c.snap_turn_deg = s.snap_turn_deg;
    c.seated = s.seated;
    return c;
}

std::string Describe(const Config& c, Family family, std::string_view profile, bool dpad) {
    return fmt::format("{} layout{}{}, inject={}, quick_arc={}, quick_arc_l3={}, belt_vial={}, "
                       "melee={}{}{}; {} controllers ({}{})",
                       c.layout.classic ? "classic" : "motion",
                       c.layout.classic ? "" : fmt::format(", left_scheme={}", c.layout.left_scheme),
                       c.layout.swap ? ", move with the right stick" : "", c.layout.inject ? 1 : 0,
                       c.quick_arc ? 1 : 0, c.arc_l3 ? 1 : 0, c.belt_vial ? 1 : 0,
                       c.melee_physical ? "physical" : "vanilla",
                       c.right_grip_draw ? ", right_grip=draw" : "",
                       c.layout.y_menu ? ", Y = the menu (y_menu)" : "", FamilyName(family),
                       profile.empty() ? std::string_view{"no profile bound yet"} : profile,
                       dpad ? ", trackpad d-pad" : "");
}

} // namespace VR::ControlsView
