// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

// The OpenXR suggested bindings of the presenter's one action set, per controller, pure data (no
// OpenXR) so the unit tests (tests/test_xr_bindings.cpp) can check every path against the
// specification. The game reads the same actions (stick, triggers, grips, A/B/X/Y, stick clicks,
// Menu, the poses and the two vibration outputs) whatever the controller; only where each one sits
// on the controller differs. The presenter (openxr_presenter.cpp CreateActions) suggests every
// profile below independently: one the runtime refuses is logged and the others stay. The runtime
// then binds the profile of the controllers actually in use (logged per hand as `OpenXR
// /user/hand/<hand> controller profile: <path> (<layout>)`); the vibration always goes to that
// controller's own haptic output.
namespace VR::XrBindings {

inline constexpr char FrameExtension[] = "XR_VALVE_frame_controller_interaction";
inline constexpr char FrameProfile[] = "/interaction_profiles/valve/frame_controller_valve";

// The presenter's actions (openxr_presenter.cpp Actions), one entry each.
enum class Act : std::uint8_t {
    Move,          // vector2: walk (left stick)
    Turn,          // vector2: snap turn, flicks (right stick)
    TriggerLeft,   // float: the gun (L2)
    TriggerRight,  // float: arm a heavy / charge
    GripLeft,      // float: take a quick item
    GripRight,     // float: transform the weapon (L1)
    ButtonA,       // bool: dodge / sprint (left_scheme=2: sprinting, the leap or a dodge)
    ButtonB,       // bool: interact
    ButtonX,       // bool: Blood Vial
    ButtonY,       // bool: quick-item arc (left_scheme=2: a tap; held, the modifier)
    StickLeft,     // bool: L3 / modifier (left_scheme=2: the sprint latch, a second click leaps)
    StickRight,    // bool: lock on / weapon switch
    Menu,          // bool: Options / recenter
    AimLeft,       // pose
    AimRight,      // pose
    GripPoseLeft,  // pose
    GripPoseRight, // pose
    HapticLeft,    // vibration output
    HapticRight,   // vibration output
    // The capacitive sensors (bool): a finger or thumb resting on the control. Only the VR page's
    // controls view reads them (it lights what you touch); suggested with Profile::touches.
    TouchTriggerLeft,
    TouchTriggerRight,
    TouchA,
    TouchB,
    TouchX,
    TouchY,
    TouchStickLeft,
    TouchStickRight,
    Count,
};

struct Binding {
    Act act;
    const char* path;
};

// Which instance extensions an entry needs (all of them; the presenter skips the entry when the
// runtime does not offer one).
enum Needs : std::uint8_t {
    NeedsNothing = 0,
    NeedsTouchPlus = 1 << 0, // XR_META_touch_controller_plus
    NeedsHp = 1 << 1,        // XR_EXT_hp_mixed_reality_controller
    NeedsBd = 1 << 2,        // XR_BD_controller_interaction
    NeedsDpad = 1 << 3,      // XR_EXT_dpad_binding (+ XR_KHR_binding_modification)
    NeedsFrame = 1 << 4,
};

struct Profile {
    const char* path;   // the interaction profile
    std::uint8_t needs; // Needs bits
    const char* layout; // for the log and the docs
    std::span<const Binding> bindings;
    // A plainer map for the same profile, suggested instead when `needs` is not met or the runtime
    // refuses `bindings` (the trackpad d-pad of the Vive wands and WMR controllers); empty: none.
    std::span<const Binding> fallback{};
    const char* fallback_layout{nullptr};
    // The touch sensors of this controller, suggested together with the map above (a profile's
    // bindings are one suggestion); a runtime that refuses them gets the map without them.
    std::span<const Binding> touches{};
};

namespace Detail {

// Both hands' poses and vibration outputs: the same paths on every controller profile.
#define BBVR_XR_POSES_AND_HAPTICS                                                                  \
    Binding{Act::AimLeft, "/user/hand/left/input/aim/pose"},                                       \
        Binding{Act::AimRight, "/user/hand/right/input/aim/pose"},                                 \
        Binding{Act::GripPoseLeft, "/user/hand/left/input/grip/pose"},                             \
        Binding{Act::GripPoseRight, "/user/hand/right/input/grip/pose"},                           \
        Binding{Act::HapticLeft, "/user/hand/left/output/haptic"},                                 \
        Binding{Act::HapticRight, "/user/hand/right/output/haptic"}

// Quest Touch / Touch Plus (the layout the game was built for, unchanged), and the controllers
// with the same buttons in the same places: HP Reverb G2, PICO 4 and PICO Neo3.
inline constexpr std::array Touch{
    Binding{Act::Move, "/user/hand/left/input/thumbstick"},
    Binding{Act::Turn, "/user/hand/right/input/thumbstick"},
    Binding{Act::TriggerLeft, "/user/hand/left/input/trigger/value"},
    Binding{Act::TriggerRight, "/user/hand/right/input/trigger/value"},
    Binding{Act::GripLeft, "/user/hand/left/input/squeeze/value"},
    Binding{Act::GripRight, "/user/hand/right/input/squeeze/value"},
    Binding{Act::ButtonA, "/user/hand/right/input/a/click"},
    Binding{Act::ButtonB, "/user/hand/right/input/b/click"},
    Binding{Act::ButtonX, "/user/hand/left/input/x/click"},
    Binding{Act::ButtonY, "/user/hand/left/input/y/click"},
    Binding{Act::StickLeft, "/user/hand/left/input/thumbstick/click"},
    Binding{Act::StickRight, "/user/hand/right/input/thumbstick/click"},
    Binding{Act::Menu, "/user/hand/left/input/menu/click"},
    BBVR_XR_POSES_AND_HAPTICS,
};

inline constexpr std::array Frame{
    Binding{Act::Move, "/user/hand/left/input/thumbstick"},
    Binding{Act::Turn, "/user/hand/right/input/thumbstick"},
    Binding{Act::TriggerLeft, "/user/hand/left/input/trigger/value"},
    Binding{Act::TriggerRight, "/user/hand/right/input/trigger/value"},
    Binding{Act::GripLeft, "/user/hand/left/input/squeeze/value"},
    Binding{Act::GripRight, "/user/hand/right/input/squeeze/value"},
    Binding{Act::ButtonA, "/user/hand/right/input/a/click"},
    Binding{Act::ButtonB, "/user/hand/right/input/b/click"},
    Binding{Act::ButtonX, "/user/hand/left/input/dpad_down/click"},
    Binding{Act::ButtonY, "/user/hand/left/input/dpad_up/click"},
    Binding{Act::StickLeft, "/user/hand/left/input/thumbstick/click"},
    Binding{Act::StickRight, "/user/hand/right/input/thumbstick/click"},
    Binding{Act::Menu, "/user/hand/left/input/view/click"},
    BBVR_XR_POSES_AND_HAPTICS,
};

// Valve Index: A/B on each hand (the left hand's A/B are X/Y), the grips by squeeze FORCE (the
// value rises to 1 just from the fingers wrapped round the handle), the system button is the
// SteamVR dashboard: Menu is the left trackpad pressed firmly.
inline constexpr std::array Index{
    Binding{Act::Move, "/user/hand/left/input/thumbstick"},
    Binding{Act::Turn, "/user/hand/right/input/thumbstick"},
    Binding{Act::TriggerLeft, "/user/hand/left/input/trigger/value"},
    Binding{Act::TriggerRight, "/user/hand/right/input/trigger/value"},
    Binding{Act::GripLeft, "/user/hand/left/input/squeeze/force"},
    Binding{Act::GripRight, "/user/hand/right/input/squeeze/force"},
    Binding{Act::ButtonA, "/user/hand/right/input/a/click"},
    Binding{Act::ButtonB, "/user/hand/right/input/b/click"},
    Binding{Act::ButtonX, "/user/hand/left/input/a/click"},
    Binding{Act::ButtonY, "/user/hand/left/input/b/click"},
    Binding{Act::StickLeft, "/user/hand/left/input/thumbstick/click"},
    Binding{Act::StickRight, "/user/hand/right/input/thumbstick/click"},
    Binding{Act::Menu, "/user/hand/left/input/trackpad/force"},
    BBVR_XR_POSES_AND_HAPTICS,
};

// HTC Vive wands: the trackpads are the sticks. With the d-pad extension (SteamVR) the pads also
// click in regions: left up = Y, left down = X, left centre = L3, right centre = A; the menu
// buttons are Menu (left) and B (right). No lock-on (R3): every other right-pad region also
// deflects the turn stick.
inline constexpr std::array ViveDpad{
    Binding{Act::Move, "/user/hand/left/input/trackpad"},
    Binding{Act::Turn, "/user/hand/right/input/trackpad"},
    Binding{Act::TriggerLeft, "/user/hand/left/input/trigger/value"},
    Binding{Act::TriggerRight, "/user/hand/right/input/trigger/value"},
    Binding{Act::GripLeft, "/user/hand/left/input/squeeze/click"},
    Binding{Act::GripRight, "/user/hand/right/input/squeeze/click"},
    Binding{Act::ButtonA, "/user/hand/right/input/trackpad/dpad_center"},
    Binding{Act::ButtonB, "/user/hand/right/input/menu/click"},
    Binding{Act::ButtonX, "/user/hand/left/input/trackpad/dpad_down"},
    Binding{Act::ButtonY, "/user/hand/left/input/trackpad/dpad_up"},
    Binding{Act::StickLeft, "/user/hand/left/input/trackpad/dpad_center"},
    Binding{Act::Menu, "/user/hand/left/input/menu/click"},
    BBVR_XR_POSES_AND_HAPTICS,
};
// Without the d-pad extension: a click anywhere on the right pad = A, on the left pad = X.
inline constexpr std::array Vive{
    Binding{Act::Move, "/user/hand/left/input/trackpad"},
    Binding{Act::Turn, "/user/hand/right/input/trackpad"},
    Binding{Act::TriggerLeft, "/user/hand/left/input/trigger/value"},
    Binding{Act::TriggerRight, "/user/hand/right/input/trigger/value"},
    Binding{Act::GripLeft, "/user/hand/left/input/squeeze/click"},
    Binding{Act::GripRight, "/user/hand/right/input/squeeze/click"},
    Binding{Act::ButtonA, "/user/hand/right/input/trackpad/click"},
    Binding{Act::ButtonB, "/user/hand/right/input/menu/click"},
    Binding{Act::ButtonX, "/user/hand/left/input/trackpad/click"},
    Binding{Act::Menu, "/user/hand/left/input/menu/click"},
    BBVR_XR_POSES_AND_HAPTICS,
};

// Windows Mixed Reality: thumbsticks as on Touch (with their clicks), the trackpads are the face
// buttons. With the d-pad extension: right pad up = B, the rest of it = A; left pad up = Y, the
// rest = X. The menu buttons are Menu (left) and B (right).
inline constexpr std::array WmrDpad{
    Binding{Act::Move, "/user/hand/left/input/thumbstick"},
    Binding{Act::Turn, "/user/hand/right/input/thumbstick"},
    Binding{Act::TriggerLeft, "/user/hand/left/input/trigger/value"},
    Binding{Act::TriggerRight, "/user/hand/right/input/trigger/value"},
    Binding{Act::GripLeft, "/user/hand/left/input/squeeze/click"},
    Binding{Act::GripRight, "/user/hand/right/input/squeeze/click"},
    Binding{Act::ButtonA, "/user/hand/right/input/trackpad/dpad_center"},
    Binding{Act::ButtonA, "/user/hand/right/input/trackpad/dpad_down"},
    Binding{Act::ButtonA, "/user/hand/right/input/trackpad/dpad_left"},
    Binding{Act::ButtonA, "/user/hand/right/input/trackpad/dpad_right"},
    Binding{Act::ButtonB, "/user/hand/right/input/trackpad/dpad_up"},
    Binding{Act::ButtonB, "/user/hand/right/input/menu/click"},
    Binding{Act::ButtonX, "/user/hand/left/input/trackpad/dpad_center"},
    Binding{Act::ButtonX, "/user/hand/left/input/trackpad/dpad_down"},
    Binding{Act::ButtonX, "/user/hand/left/input/trackpad/dpad_left"},
    Binding{Act::ButtonX, "/user/hand/left/input/trackpad/dpad_right"},
    Binding{Act::ButtonY, "/user/hand/left/input/trackpad/dpad_up"},
    Binding{Act::StickLeft, "/user/hand/left/input/thumbstick/click"},
    Binding{Act::StickRight, "/user/hand/right/input/thumbstick/click"},
    Binding{Act::Menu, "/user/hand/left/input/menu/click"},
    BBVR_XR_POSES_AND_HAPTICS,
};
// Without the d-pad extension: the right pad clicked = A, the left pad clicked = X (no Y).
inline constexpr std::array Wmr{
    Binding{Act::Move, "/user/hand/left/input/thumbstick"},
    Binding{Act::Turn, "/user/hand/right/input/thumbstick"},
    Binding{Act::TriggerLeft, "/user/hand/left/input/trigger/value"},
    Binding{Act::TriggerRight, "/user/hand/right/input/trigger/value"},
    Binding{Act::GripLeft, "/user/hand/left/input/squeeze/click"},
    Binding{Act::GripRight, "/user/hand/right/input/squeeze/click"},
    Binding{Act::ButtonA, "/user/hand/right/input/trackpad/click"},
    Binding{Act::ButtonB, "/user/hand/right/input/menu/click"},
    Binding{Act::ButtonX, "/user/hand/left/input/trackpad/click"},
    Binding{Act::StickLeft, "/user/hand/left/input/thumbstick/click"},
    Binding{Act::StickRight, "/user/hand/right/input/thumbstick/click"},
    Binding{Act::Menu, "/user/hand/left/input/menu/click"},
    BBVR_XR_POSES_AND_HAPTICS,
};

// Khronos simple controller, the last fallback of any runtime: select = the triggers, menu =
// Menu (either hand), the poses and the vibration. Enough for the virtual screen and the hands.
inline constexpr std::array Simple{
    Binding{Act::TriggerLeft, "/user/hand/left/input/select/click"},
    Binding{Act::TriggerRight, "/user/hand/right/input/select/click"},
    Binding{Act::Menu, "/user/hand/left/input/menu/click"},
    Binding{Act::Menu, "/user/hand/right/input/menu/click"},
    BBVR_XR_POSES_AND_HAPTICS,
};

#undef BBVR_XR_POSES_AND_HAPTICS

// The touch sensors (the controls view: what a finger or thumb rests on lights up). Quest Touch /
// Touch Plus and PICO: the triggers, the face buttons and the sticks.
inline constexpr std::array TouchTouches{
    Binding{Act::TouchTriggerLeft, "/user/hand/left/input/trigger/touch"},
    Binding{Act::TouchTriggerRight, "/user/hand/right/input/trigger/touch"},
    Binding{Act::TouchA, "/user/hand/right/input/a/touch"},
    Binding{Act::TouchB, "/user/hand/right/input/b/touch"},
    Binding{Act::TouchX, "/user/hand/left/input/x/touch"},
    Binding{Act::TouchY, "/user/hand/left/input/y/touch"},
    Binding{Act::TouchStickLeft, "/user/hand/left/input/thumbstick/touch"},
    Binding{Act::TouchStickRight, "/user/hand/right/input/thumbstick/touch"},
};
inline constexpr std::array FrameTouches{
    Binding{Act::TouchTriggerLeft, "/user/hand/left/input/trigger/touch"},
    Binding{Act::TouchTriggerRight, "/user/hand/right/input/trigger/touch"},
    Binding{Act::TouchA, "/user/hand/right/input/a/touch"},
    Binding{Act::TouchB, "/user/hand/right/input/b/touch"},
    Binding{Act::TouchX, "/user/hand/left/input/dpad_down/touch"},
    Binding{Act::TouchY, "/user/hand/left/input/dpad_up/touch"},
    Binding{Act::TouchStickLeft, "/user/hand/left/input/thumbstick/touch"},
    Binding{Act::TouchStickRight, "/user/hand/right/input/thumbstick/touch"},
};
// Valve Index: the same, its left A / B carrying X / Y as in its map.
inline constexpr std::array IndexTouches{
    Binding{Act::TouchTriggerLeft, "/user/hand/left/input/trigger/touch"},
    Binding{Act::TouchTriggerRight, "/user/hand/right/input/trigger/touch"},
    Binding{Act::TouchA, "/user/hand/right/input/a/touch"},
    Binding{Act::TouchB, "/user/hand/right/input/b/touch"},
    Binding{Act::TouchX, "/user/hand/left/input/a/touch"},
    Binding{Act::TouchY, "/user/hand/left/input/b/touch"},
    Binding{Act::TouchStickLeft, "/user/hand/left/input/thumbstick/touch"},
    Binding{Act::TouchStickRight, "/user/hand/right/input/thumbstick/touch"},
};
// Vive wands: the trackpads are the sticks.
inline constexpr std::array ViveTouches{
    Binding{Act::TouchStickLeft, "/user/hand/left/input/trackpad/touch"},
    Binding{Act::TouchStickRight, "/user/hand/right/input/trackpad/touch"},
};

} // namespace Detail

// Every profile suggested, in this order (the order does not matter to a runtime).
inline constexpr std::array Profiles{
    Profile{"/interaction_profiles/oculus/touch_controller", NeedsNothing, "Touch", Detail::Touch,
            {}, nullptr, Detail::TouchTouches},
    Profile{"/interaction_profiles/meta/touch_controller_plus", NeedsTouchPlus, "Touch",
            Detail::Touch, {}, nullptr, Detail::TouchTouches},
    Profile{FrameProfile, NeedsFrame, "Steam Frame", Detail::Frame, {}, nullptr,
            Detail::FrameTouches},
    Profile{"/interaction_profiles/valve/index_controller", NeedsNothing, "Index", Detail::Index, {},
            nullptr, Detail::IndexTouches},
    Profile{"/interaction_profiles/htc/vive_controller", NeedsDpad, "Vive, trackpad d-pad",
            Detail::ViveDpad, Detail::Vive, "Vive, trackpad clicks", Detail::ViveTouches},
    Profile{"/interaction_profiles/microsoft/motion_controller", NeedsDpad, "WMR, trackpad d-pad",
            Detail::WmrDpad, Detail::Wmr, "WMR, trackpad clicks"},
    Profile{"/interaction_profiles/hp/mixed_reality_controller", NeedsHp, "Touch (HP Reverb G2)",
            Detail::Touch},
    Profile{"/interaction_profiles/bytedance/pico4_controller", NeedsBd, "Touch (PICO 4)",
            Detail::Touch, {}, nullptr, Detail::TouchTouches},
    Profile{"/interaction_profiles/bytedance/pico_neo3_controller", NeedsBd, "Touch (PICO Neo3)",
            Detail::Touch, {}, nullptr, Detail::TouchTouches},
    Profile{"/interaction_profiles/khr/simple_controller", NeedsNothing, "simple", Detail::Simple},
};

// The layout name for a bound interaction profile ("" for one not in the table). `dpad`: whether
// the d-pad map of that profile was accepted.
inline const char* LayoutOf(std::string_view profile, bool dpad) {
    for (const Profile& p : Profiles) {
        if (profile == p.path) {
            return (p.needs & NeedsDpad) != 0 && !dpad && p.fallback_layout != nullptr
                       ? p.fallback_layout
                       : p.layout;
        }
    }
    return "";
}

// Whether the map in use for a bound interaction profile binds `act` (`dpad`: its d-pad map was
// accepted; a profile not in the table: true, nothing known against it). The presenter: on a
// controller without Y (the WMR / Vive trackpad clicks) left_scheme=2's modifier falls back to the
// left stick click held with the stick centred.
inline bool Binds(std::string_view profile, bool dpad, Act act) {
    for (const Profile& p : Profiles) {
        if (profile != p.path) {
            continue;
        }
        const bool fallback = (p.needs & NeedsDpad) != 0 && !dpad && !p.fallback.empty();
        for (const Binding& b : fallback ? p.fallback : p.bindings) {
            if (b.act == act) {
                return true;
            }
        }
        return false;
    }
    return true;
}

} // namespace VR::XrBindings
