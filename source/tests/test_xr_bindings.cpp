// SPDX-License-Identifier: GPL-2.0-or-later

// The OpenXR suggested bindings per controller (core/vr/xr_bindings.h): every path is one the
// OpenXR specification (1.1, the core profiles and XR_META_touch_controller_plus,
// XR_EXT_hp_mixed_reality_controller, XR_BD_controller_interaction, XR_EXT_dpad_binding) lists
// for that profile and hand, with a type the action can take; every map holds both poses and both
// vibration outputs; the Touch map is exactly the one the Quest has always used; nothing is bound
// to a system button; only the d-pad maps use d-pad paths.

#include <algorithm>
#include <initializer_list>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/xr_bindings.h"

namespace {

namespace XB = VR::XrBindings;

// The specification's component paths per profile: {hand ("both", "left", "right"), path}.
struct SpecProfile {
    std::vector<std::pair<std::string, std::string>> components;
};

const std::map<std::string, SpecProfile>& Spec() {
    static const std::map<std::string, SpecProfile> spec = [] {
        std::map<std::string, SpecProfile> s;
        const auto add = [&s](const char* profile, const char* hand,
                              std::initializer_list<const char*> paths) {
            for (const char* p : paths) {
                s[profile].components.emplace_back(hand, p);
            }
        };
        const char* touch_both[] = {"/input/squeeze/value",    "/input/trigger/value",
                                    "/input/trigger/touch",    "/input/thumbstick/x",
                                    "/input/thumbstick/y",     "/input/thumbstick/click",
                                    "/input/thumbstick/touch", "/input/thumbrest/touch",
                                    "/input/grip/pose",        "/input/aim/pose",
                                    "/output/haptic"};
        for (const char* profile : {"/interaction_profiles/oculus/touch_controller",
                                    "/interaction_profiles/meta/touch_controller_plus"}) {
            add(profile, "left",
                {"/input/x/click", "/input/x/touch", "/input/y/click", "/input/y/touch",
                 "/input/menu/click"});
            add(profile, "right",
                {"/input/a/click", "/input/a/touch", "/input/b/click", "/input/b/touch",
                 "/input/system/click"});
            for (const char* p : touch_both) {
                add(profile, "both", {p});
            }
        }
        add("/interaction_profiles/meta/touch_controller_plus", "both",
            {"/input/thumb_meta/proximity_meta", "/input/trigger/proximity_meta",
             "/input/trigger/curl_meta", "/input/trigger/slide_meta", "/input/trigger/force"});
        add("/interaction_profiles/valve/index_controller", "both",
            {"/input/system/click", "/input/system/touch", "/input/a/click", "/input/a/touch",
             "/input/b/click", "/input/b/touch", "/input/squeeze/value", "/input/squeeze/force",
             "/input/trigger/click", "/input/trigger/value", "/input/trigger/touch",
             "/input/thumbstick/x", "/input/thumbstick/y", "/input/thumbstick/click",
             "/input/thumbstick/touch", "/input/trackpad/x", "/input/trackpad/y",
             "/input/trackpad/force", "/input/trackpad/touch", "/input/grip/pose",
             "/input/aim/pose", "/output/haptic"});
        add(XB::FrameProfile, "both",
            {"/input/system/click", "/input/system/touch", "/input/bumper/click",
             "/input/bumper/touch", "/input/squeeze/value", "/input/squeeze/click",
             "/input/squeeze/touch", "/input/trigger/click", "/input/trigger/value",
             "/input/trigger/touch", "/input/thumbstick/x", "/input/thumbstick/y",
             "/input/thumbstick/click", "/input/thumbstick/touch", "/input/grip/pose",
             "/input/aim/pose", "/output/haptic"});
        add(XB::FrameProfile, "left",
            {"/input/dpad_up/click", "/input/dpad_up/touch", "/input/dpad_down/click",
             "/input/dpad_down/touch", "/input/dpad_left/click", "/input/dpad_left/touch",
             "/input/dpad_right/click", "/input/dpad_right/touch", "/input/view/click",
             "/input/view/touch"});
        add(XB::FrameProfile, "right",
            {"/input/a/click", "/input/a/touch", "/input/b/click", "/input/b/touch",
             "/input/x/click", "/input/x/touch", "/input/y/click", "/input/y/touch",
             "/input/menu/click", "/input/menu/touch"});
        add("/interaction_profiles/htc/vive_controller", "both",
            {"/input/system/click", "/input/squeeze/click", "/input/menu/click",
             "/input/trigger/click", "/input/trigger/value", "/input/trackpad/x",
             "/input/trackpad/y", "/input/trackpad/click", "/input/trackpad/touch",
             "/input/grip/pose", "/input/aim/pose", "/output/haptic"});
        add("/interaction_profiles/microsoft/motion_controller", "both",
            {"/input/menu/click", "/input/squeeze/click", "/input/trigger/value",
             "/input/thumbstick/x", "/input/thumbstick/y", "/input/thumbstick/click",
             "/input/trackpad/x", "/input/trackpad/y", "/input/trackpad/click",
             "/input/trackpad/touch", "/input/grip/pose", "/input/aim/pose", "/output/haptic"});
        add("/interaction_profiles/hp/mixed_reality_controller", "left",
            {"/input/x/click", "/input/y/click"});
        add("/interaction_profiles/hp/mixed_reality_controller", "right",
            {"/input/a/click", "/input/b/click"});
        add("/interaction_profiles/hp/mixed_reality_controller", "both",
            {"/input/menu/click", "/input/squeeze/value", "/input/trigger/value",
             "/input/thumbstick/x", "/input/thumbstick/y", "/input/thumbstick/click",
             "/input/grip/pose", "/input/aim/pose", "/output/haptic"});
        const char* pico_both[] = {
            "/input/system/click",  "/input/trigger/click",    "/input/trigger/value",
            "/input/trigger/touch", "/input/thumbstick/y",     "/input/thumbstick/x",
            "/input/thumbstick/click", "/input/thumbstick/touch", "/input/squeeze/click",
            "/input/squeeze/value", "/input/grip/pose",        "/input/aim/pose",
            "/output/haptic"};
        add("/interaction_profiles/bytedance/pico4_controller", "left",
            {"/input/x/click", "/input/x/touch", "/input/y/click", "/input/y/touch",
             "/input/menu/click"});
        add("/interaction_profiles/bytedance/pico4_controller", "right",
            {"/input/a/click", "/input/a/touch", "/input/b/click", "/input/b/touch"});
        add("/interaction_profiles/bytedance/pico_neo3_controller", "left",
            {"/input/x/click", "/input/x/touch", "/input/y/click", "/input/y/touch"});
        add("/interaction_profiles/bytedance/pico_neo3_controller", "right",
            {"/input/a/click", "/input/a/touch", "/input/b/click", "/input/b/touch"});
        add("/interaction_profiles/bytedance/pico_neo3_controller", "both", {"/input/menu/click"});
        for (const char* p : pico_both) {
            add("/interaction_profiles/bytedance/pico4_controller", "both", {p});
            add("/interaction_profiles/bytedance/pico_neo3_controller", "both", {p});
        }
        add("/interaction_profiles/khr/simple_controller", "both",
            {"/input/select/click", "/input/menu/click", "/input/grip/pose", "/input/aim/pose",
             "/output/haptic"});
        return s;
    }();
    return spec;
}

bool HasComponent(const std::string& profile, const std::string& hand, const std::string& comp) {
    const auto it = Spec().find(profile);
    if (it == Spec().end()) {
        return false;
    }
    return std::ranges::any_of(it->second.components, [&](const auto& c) {
        return c.second == comp && (c.first == "both" || c.first == hand);
    });
}

enum class Kind { Vector2, Pose, Haptic, Scalar };

Kind KindOf(XB::Act act) {
    switch (act) {
    case XB::Act::Move:
    case XB::Act::Turn:
        return Kind::Vector2;
    case XB::Act::AimLeft:
    case XB::Act::AimRight:
    case XB::Act::GripPoseLeft:
    case XB::Act::GripPoseRight:
        return Kind::Pose;
    case XB::Act::HapticLeft:
    case XB::Act::HapticRight:
        return Kind::Haptic;
    default:
        return Kind::Scalar;
    }
}

// Why a binding is not valid for the profile ("" = valid). `dpad`: d-pad paths allowed.
std::string Problem(const std::string& profile, const XB::Binding& b, bool dpad) {
    const std::string path = b.path;
    std::string hand;
    std::string rest;
    for (const char* h : {"left", "right"}) {
        const std::string prefix = std::string("/user/hand/") + h;
        if (path.rfind(prefix + "/", 0) == 0) {
            hand = h;
            rest = path.substr(prefix.size());
        }
    }
    if (hand.empty()) {
        return "not a hand path";
    }
    const Kind kind = KindOf(b.act);
    const bool left_act = b.act == XB::Act::AimLeft || b.act == XB::Act::GripPoseLeft ||
                          b.act == XB::Act::HapticLeft;
    const bool right_act = b.act == XB::Act::AimRight || b.act == XB::Act::GripPoseRight ||
                           b.act == XB::Act::HapticRight;
    if ((left_act && hand != "left") || (right_act && hand != "right")) {
        return "a hand's pose or vibration on the other hand";
    }
    if (rest.find("/system/") != std::string::npos) {
        return "the system button is reserved";
    }
    // A d-pad component: .../input/<trackpad|thumbstick>/dpad_<dir>.
    const auto dpad_at = rest.find("/dpad_");
    if (rest.starts_with("/input/trackpad/dpad_") ||
        rest.starts_with("/input/thumbstick/dpad_")) {
        if (!dpad) {
            return "a d-pad path in a map without the d-pad extension";
        }
        const std::string parent = rest.substr(0, dpad_at);
        const std::string dir = rest.substr(dpad_at + 6);
        if (kind != Kind::Scalar) {
            return "a d-pad path on a non-button action";
        }
        if (parent == "/input/trackpad") {
            if (dir != "up" && dir != "down" && dir != "left" && dir != "right" &&
                dir != "center") {
                return "unknown d-pad direction";
            }
        } else if (parent == "/input/thumbstick") {
            if (dir != "up" && dir != "down" && dir != "left" && dir != "right") {
                return "unknown d-pad direction";
            }
        } else {
            return "a d-pad on neither a trackpad nor a thumbstick";
        }
        return HasComponent(profile, hand, parent + "/x") ? "" : "no such trackpad / thumbstick";
    }
    switch (kind) {
    case Kind::Vector2:
        // The identifier itself (x and y together).
        if (!HasComponent(profile, hand, rest + "/x") || !HasComponent(profile, hand, rest + "/y")) {
            return "a 2D action on a path without x and y";
        }
        return "";
    case Kind::Pose:
        if (rest.size() < 5 || rest.substr(rest.size() - 5) != "/pose") {
            return "a pose action on a non-pose path";
        }
        break;
    case Kind::Haptic:
        if (rest != "/output/haptic") {
            return "a vibration action on a non-haptic path";
        }
        break;
    case Kind::Scalar:
        if (rest.find("/pose") != std::string::npos || rest.find("/output/") == 0) {
            return "a button / value action on a pose or output";
        }
        break;
    }
    return HasComponent(profile, hand, rest) ? "" : "not in the profile";
}

std::string Name(XB::Act act) {
    return std::to_string(static_cast<int>(act));
}

} // namespace

TEST(XrBindings, EveryPathIsInTheSpecificationForItsProfile) {
    int checked = 0;
    for (const XB::Profile& p : XB::Profiles) {
        ASSERT_TRUE(Spec().contains(p.path)) << p.path;
        const bool dpad = (p.needs & XB::NeedsDpad) != 0;
        for (const XB::Binding& b : p.bindings) {
            const std::string why = Problem(p.path, b, dpad);
            EXPECT_TRUE(why.empty()) << p.path << " " << b.path << " (act " << Name(b.act)
                                     << "): " << why;
            ++checked;
        }
        for (const XB::Binding& b : p.fallback) {
            const std::string why = Problem(p.path, b, false);
            EXPECT_TRUE(why.empty()) << p.path << " fallback " << b.path << " (act "
                                     << Name(b.act) << "): " << why;
            ++checked;
        }
    }
    EXPECT_GT(checked, 150);
}

TEST(XrBindings, EveryMapHasBothPosesAndBothVibrationOutputs) {
    for (const XB::Profile& p : XB::Profiles) {
        for (const auto map : {p.bindings, p.fallback}) {
            if (map.empty()) {
                continue;
            }
            for (const XB::Act act :
                 {XB::Act::AimLeft, XB::Act::AimRight, XB::Act::GripPoseLeft,
                  XB::Act::GripPoseRight, XB::Act::HapticLeft, XB::Act::HapticRight}) {
                const auto count =
                    std::ranges::count_if(map, [act](const XB::Binding& b) { return b.act == act; });
                EXPECT_EQ(count, 1) << p.path << " act " << Name(act);
            }
            const auto haptic = [&](XB::Act act) {
                return std::ranges::find_if(
                    map, [act](const XB::Binding& b) { return b.act == act; });
            };
            EXPECT_STREQ(haptic(XB::Act::HapticLeft)->path, "/user/hand/left/output/haptic");
            EXPECT_STREQ(haptic(XB::Act::HapticRight)->path, "/user/hand/right/output/haptic");
        }
    }
}

TEST(XrBindings, TouchMapIsUnchanged) {
    // The 19 bindings the Quest has always had (openxr_presenter.cpp before the table).
    const std::vector<std::pair<XB::Act, std::string>> legacy{
        {XB::Act::Move, "/user/hand/left/input/thumbstick"},
        {XB::Act::Turn, "/user/hand/right/input/thumbstick"},
        {XB::Act::TriggerLeft, "/user/hand/left/input/trigger/value"},
        {XB::Act::TriggerRight, "/user/hand/right/input/trigger/value"},
        {XB::Act::GripLeft, "/user/hand/left/input/squeeze/value"},
        {XB::Act::GripRight, "/user/hand/right/input/squeeze/value"},
        {XB::Act::ButtonA, "/user/hand/right/input/a/click"},
        {XB::Act::ButtonB, "/user/hand/right/input/b/click"},
        {XB::Act::ButtonX, "/user/hand/left/input/x/click"},
        {XB::Act::ButtonY, "/user/hand/left/input/y/click"},
        {XB::Act::StickLeft, "/user/hand/left/input/thumbstick/click"},
        {XB::Act::StickRight, "/user/hand/right/input/thumbstick/click"},
        {XB::Act::Menu, "/user/hand/left/input/menu/click"},
        {XB::Act::AimLeft, "/user/hand/left/input/aim/pose"},
        {XB::Act::AimRight, "/user/hand/right/input/aim/pose"},
        {XB::Act::GripPoseLeft, "/user/hand/left/input/grip/pose"},
        {XB::Act::GripPoseRight, "/user/hand/right/input/grip/pose"},
        {XB::Act::HapticLeft, "/user/hand/left/output/haptic"},
        {XB::Act::HapticRight, "/user/hand/right/output/haptic"},
    };
    int touch_profiles = 0;
    for (const XB::Profile& p : XB::Profiles) {
        const std::string_view path = p.path;
        if (path != "/interaction_profiles/oculus/touch_controller" &&
            path != "/interaction_profiles/meta/touch_controller_plus") {
            continue;
        }
        ++touch_profiles;
        EXPECT_TRUE(p.fallback.empty());
        ASSERT_EQ(p.bindings.size(), legacy.size());
        for (size_t i = 0; i < legacy.size(); ++i) {
            EXPECT_EQ(p.bindings[i].act, legacy[i].first) << p.path << " #" << i;
            EXPECT_EQ(std::string(p.bindings[i].path), legacy[i].second) << p.path << " #" << i;
        }
    }
    EXPECT_EQ(touch_profiles, 2);
    // The Touch profile needs nothing; Touch Plus only its own extension.
    EXPECT_EQ(static_cast<int>(XB::Profiles[0].needs), static_cast<int>(XB::NeedsNothing));
    EXPECT_EQ(static_cast<int>(XB::Profiles[1].needs), static_cast<int>(XB::NeedsTouchPlus));
}

TEST(XrBindings, GameplayActionsReachableOnEveryFullController) {
    // Walk, turn, both triggers, both grips, dodge, interact, the vial and Menu on every
    // controller but the simple one (select / menu only).
    for (const XB::Profile& p : XB::Profiles) {
        if (std::string_view{p.path} == "/interaction_profiles/khr/simple_controller") {
            continue;
        }
        for (const auto map : {p.bindings, p.fallback}) {
            if (map.empty()) {
                continue;
            }
            for (const XB::Act act :
                 {XB::Act::Move, XB::Act::Turn, XB::Act::TriggerLeft, XB::Act::TriggerRight,
                  XB::Act::GripLeft, XB::Act::GripRight, XB::Act::ButtonA, XB::Act::ButtonB,
                  XB::Act::ButtonX, XB::Act::Menu}) {
                EXPECT_TRUE(std::ranges::any_of(
                    map, [act](const XB::Binding& b) { return b.act == act; }))
                    << p.path << " lacks act " << Name(act);
            }
        }
    }
}

TEST(XrBindings, NoDuplicatesAndOneEntryPerProfile) {
    std::set<std::string> profiles;
    for (const XB::Profile& p : XB::Profiles) {
        EXPECT_TRUE(profiles.insert(p.path).second) << p.path;
        for (const auto map : {p.bindings, p.fallback}) {
            std::set<std::pair<int, std::string>> seen;
            for (const XB::Binding& b : map) {
                EXPECT_TRUE(seen.insert({static_cast<int>(b.act), b.path}).second)
                    << p.path << " " << b.path;
            }
        }
        // Only d-pad entries have a fallback, and it needs nothing more than the profile.
        EXPECT_EQ(!p.fallback.empty(), (p.needs & XB::NeedsDpad) != 0) << p.path;
        EXPECT_EQ(p.fallback_layout != nullptr, !p.fallback.empty()) << p.path;
    }
    EXPECT_STREQ(XB::Profiles.back().path, "/interaction_profiles/khr/simple_controller");
}

// The touch sensors (the VR page's controls view lights what you touch): a map of their own per
// profile, every path in the specification for that profile and hand, only touch acts in it and
// no touch act in a gameplay map, at most one path per act, and none where the controller has
// no sensor (HP Reverb G2, WMR, the simple controller).
TEST(XrBindings, TouchSensorsAreTheirOwnValidMap) {
    const auto is_touch = [](XB::Act a) {
        return a >= XB::Act::TouchTriggerLeft && a < XB::Act::Count;
    };
    int checked = 0;
    for (const XB::Profile& p : XB::Profiles) {
        for (const XB::Binding& b : p.touches) {
            EXPECT_TRUE(is_touch(b.act)) << p.path << " " << b.path;
            const std::string why = Problem(p.path, b, false);
            EXPECT_TRUE(why.empty()) << p.path << " touch " << b.path << ": " << why;
            EXPECT_NE(std::string(b.path).find("/touch"), std::string::npos) << b.path;
            EXPECT_EQ(std::ranges::count_if(p.touches,
                                            [&](const XB::Binding& o) { return o.act == b.act; }),
                      1)
                << p.path << " " << b.path;
            ++checked;
        }
        for (const auto map : {p.bindings, p.fallback}) {
            for (const XB::Binding& b : map) {
                EXPECT_FALSE(is_touch(b.act)) << p.path << " " << b.path;
            }
        }
        const std::string_view path = p.path;
        if (path.find("hp/mixed_reality") != std::string_view::npos ||
            path.find("microsoft/motion") != std::string_view::npos ||
            path.find("khr/simple") != std::string_view::npos) {
            EXPECT_TRUE(p.touches.empty()) << p.path;
        }
    }
    EXPECT_EQ(checked, 50);
    // Touch: both triggers, A B X Y and both sticks.
    EXPECT_EQ(XB::Profiles[0].touches.size(), 8u);
}

TEST(XrBindings, LayoutNames) {
    EXPECT_STREQ(XB::LayoutOf("/interaction_profiles/meta/touch_controller_plus", false), "Touch");
    EXPECT_STREQ(XB::LayoutOf("/interaction_profiles/valve/index_controller", false), "Index");
    EXPECT_STREQ(XB::LayoutOf("/interaction_profiles/htc/vive_controller", true),
                 "Vive, trackpad d-pad");
    EXPECT_STREQ(XB::LayoutOf("/interaction_profiles/htc/vive_controller", false),
                 "Vive, trackpad clicks");
    EXPECT_STREQ(XB::LayoutOf("/interaction_profiles/microsoft/motion_controller", false),
                 "WMR, trackpad clicks");
    EXPECT_STREQ(XB::LayoutOf("/interaction_profiles/khr/simple_controller", false), "simple");
    EXPECT_STREQ(XB::LayoutOf("/interaction_profiles/unknown/thing", false), "");
}

// Y on the map in use (left_scheme=2's modifier falls back to the left stick click without it).
TEST(XrBindings, WhichMapsHaveY) {
    EXPECT_TRUE(XB::Binds("/interaction_profiles/oculus/touch_controller", false, XB::Act::ButtonY));
    EXPECT_TRUE(XB::Binds("/interaction_profiles/valve/index_controller", false, XB::Act::ButtonY));
    EXPECT_TRUE(XB::Binds("/interaction_profiles/microsoft/motion_controller", true,
                          XB::Act::ButtonY));
    EXPECT_FALSE(XB::Binds("/interaction_profiles/microsoft/motion_controller", false,
                           XB::Act::ButtonY));
    EXPECT_TRUE(XB::Binds("/interaction_profiles/htc/vive_controller", true, XB::Act::ButtonY));
    EXPECT_FALSE(XB::Binds("/interaction_profiles/htc/vive_controller", false, XB::Act::ButtonY));
    EXPECT_TRUE(XB::Binds("/interaction_profiles/microsoft/motion_controller", false,
                          XB::Act::StickLeft));
    EXPECT_FALSE(XB::Binds("/interaction_profiles/khr/simple_controller", false, XB::Act::ButtonY));
    EXPECT_TRUE(XB::Binds("/interaction_profiles/unknown/thing", false, XB::Act::ButtonY));
}

TEST(XrBindings, FrameUsesNativeButtonsWithoutTheDpadExtension) {
    const auto frame = std::ranges::find_if(XB::Profiles, [](const XB::Profile& p) {
        return std::string_view{p.path} == XB::FrameProfile;
    });
    ASSERT_NE(frame, XB::Profiles.end());
    EXPECT_EQ(frame->needs, XB::NeedsFrame);
    for (const auto act : {XB::Act::ButtonY, XB::Act::StickLeft, XB::Act::StickRight}) {
        EXPECT_TRUE(XB::Binds(frame->path, false, act));
    }
    EXPECT_TRUE(Problem(frame->path,
                        {XB::Act::ButtonY, "/user/hand/left/input/dpad_up/click"}, false).empty());
    EXPECT_FALSE(Problem(frame->path,
                         {XB::Act::ButtonX, "/user/hand/left/input/x/click"}, false).empty());
    EXPECT_FALSE(Problem(frame->path,
                         {XB::Act::Menu, "/user/hand/left/input/menu/click"}, false).empty());
    EXPECT_FALSE(Problem(frame->path,
                         {XB::Act::Menu, "/user/hand/left/input/system/click"}, false).empty());
    EXPECT_STREQ(XB::LayoutOf(frame->path, false), "Steam Frame");
}
