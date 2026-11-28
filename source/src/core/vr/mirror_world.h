// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cerrno>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>
#include <string_view>
#include <utility>

// Left-handed play: the mirrored world (2026-11-22: "How about we do left handed mode?
// ... basically mirror the hunter fully?", then "Mirror the world", "it needs to be a setting in
// Launcher", "only for first person + motion controls", and "NO PERFORMANCE HIT, no performance
// regressions, and should have no impact for regular (right handed) mode. Also memory safety is
// important").
//
// The game and every VR system go on exactly as they are, right-handed, in a mirror image of your
// room. Each pose OpenXR hands the presenter is reflected across the tracking space's x = 0 plane;
// the two eyes and the two controllers trade places (the eyes in the views' order, the controllers
// in the suggested bindings: core/vr/xr_bindings.h Mirror); a stick's x is reversed. So the weapon
// hand the game knows (its right) is the controller in your left hand. On the way out every layer's
// pose is reflected back (the same functions: a reflection is its own inverse), a projection
// layer's two views trade places again, and a picture of the world (an eye, the late warp's, the
// overlay's) is flipped left to right where it is written. A picture meant to be read (the game's
// UI, the VR page, the menu panel, the virtual screen) keeps its pixels; only its pose changes.
// Sound trades left and right at the audio output.
//
// The rule that keeps a rigid pose rigid is the reflection on both sides, R' = M R M with
// M = diag(-1, 1, 1): a rotation's axis is reflected and its angle reversed, so a quaternion's y and
// z change sign (Rotation); a position, a linear velocity or any other ordinary vector changes the
// sign of its x (Vector); an angular velocity, an axial vector, changes the signs of y and z
// (Axial). The same holds for a pose relative to another pose when both are reflected, so one rule
// serves LOCAL, the VIEW space and an aim pose relative to its grip.
//
// Right-handed play never calls anything here (the presenter's mirror flag is false all session).
// Pure: no OpenXR, no Vulkan (tests/test_mirror_world.cpp).
namespace VR::Mirror {

// On only with left_handed=1, motion controls (play_style 0: a gamepad style never mirrors), first
// person (VR::IsFirstPerson: first_person not 0), stereo mode 2 (native or DIBR eyes, flipped
// where they are written; mono and AER frames take an unflipped copy) and the game's UI on its own
// layer (ui_layer=1: drawn into the eyes it would be flipped with them). Read once at the start:
// the controllers' bindings are made then.
constexpr bool Wanted(bool left_handed, int play_style, bool first_person, bool stereo = true,
                      bool ui_layer = true) {
    return left_handed && play_style == 0 && first_person && stereo && ui_layer;
}

// These keys' vr.ini words as the game reads them (vr_session.cpp ParseSettingsLine), for the
// words that follow the file (the VR page, the launcher): left_handed is on for "1" or "true" only.
constexpr bool OnWord(std::string_view value) {
    return value == "1" || value == "true";
}
// first_person: std::stof's number cut to a whole one, 0 = third person ("0.0" and "0.5" too); a
// value stof refuses (a word, nothing, out of range) leaves the game in first person (the line is
// refused), and so does a number that is not finite.
inline bool ThirdPersonWord(std::string_view value) {
    const std::string text{value};
    char* end = nullptr;
    errno = 0;
    const float number = std::strtof(text.c_str(), &end);
    if (end == text.c_str() || errno == ERANGE || !std::isfinite(number)) {
        return false;
    }
    return number > -1.0f && number < 1.0f;
}

// A position, a linear velocity, a direction: x reversed.
template <class V>
constexpr V Vector(V v) {
    v.x = -v.x;
    return v;
}

// A rotation (a unit quaternion with x, y, z, w): y and z reversed.
template <class Q>
constexpr Q Rotation(Q q) {
    q.y = -q.y;
    q.z = -q.z;
    return q;
}

// An angular velocity (an axial vector): y and z reversed.
template <class V>
constexpr V Axial(V w) {
    w.y = -w.y;
    w.z = -w.z;
    return w;
}

// A pose with `orientation` and `position` members (XrPosef, VR::Pose).
template <class P>
constexpr P Pose(P p) {
    p.orientation = Rotation(p.orientation);
    p.position = Vector(p.position);
    return p;
}

// A turn about the vertical and a move (VR::Space::Shift: yaw, x, y, z): the turn reversed, the move's
// x reversed. A recenter's shift is taken in the room for OpenXR and in the mirror for what the
// presenter keeps.
template <class S>
constexpr S YawShift(S s) {
    s.yaw = -s.yaw;
    s.x = -s.x;
    return s;
}

// A view's field of view: its left and right edges trade places, each reversed (radians; left is
// negative). Up and down stay.
struct Sides {
    float left;
    float right;
};
constexpr Sides Fov(float left, float right) {
    return {-right, -left};
}

// The other eye, hand or view of a pair (0 left, 1 right). Anything else is returned as it is:
// never an index past the pair.
constexpr std::uint32_t Other(std::uint32_t i) {
    return i < 2u ? (i ^ 1u) : i;
}

// The x of a rectangle `width` wide mirrored inside the rectangle it lies in (`outer_x`,
// `outer_width`): its distance from the outer's left edge becomes its distance from the right one.
// A rectangle outside the outer one, or wider, is clamped to stay inside it.
constexpr std::int32_t RectX(std::int32_t x, std::int32_t width, std::int32_t outer_x,
                             std::int32_t outer_width) {
    if (outer_width <= 0 || width <= 0) {
        return x;
    }
    if (width > outer_width) {
        width = outer_width;
    }
    std::int32_t from_left = x - outer_x;
    if (from_left < 0) {
        from_left = 0;
    }
    if (from_left > outer_width - width) {
        from_left = outer_width - width;
    }
    return outer_x + (outer_width - width - from_left);
}

// A stick's sideways value.
constexpr float StickX(float x) {
    return -x;
}

// A help text about your hands and body ("the left grip at your left hip") for left-handed play:
// every whole word left / right (Left / Right, LEFT / RIGHT) traded, nothing else; a word inside
// {...} (a placeholder the text expands later) is left as it is. Only for such texts: a gamepad's,
// the game's own (its touchpad's left half) or a controller's hardware keep their words, and the
// names of controls come mirrored already (the callers choose what goes through here).
inline std::string SideWords(std::string_view text) {
    constexpr std::array<std::pair<std::string_view, std::string_view>, 6> Pairs{{
        {"left", "right"},
        {"right", "left"},
        {"Left", "Right"},
        {"Right", "Left"},
        {"LEFT", "RIGHT"},
        {"RIGHT", "LEFT"},
    }};
    const auto letter = [](char ch) {
        return (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
    };
    std::string out;
    out.reserve(text.size() + 8);
    int braces = 0;
    std::size_t i = 0;
    while (i < text.size()) {
        const char ch = text[i];
        if (ch == '{') {
            ++braces;
        } else if (ch == '}' && braces > 0) {
            --braces;
        } else if (braces == 0 && letter(ch) && (i == 0 || !letter(text[i - 1]))) {
            bool traded = false;
            for (const auto& [from, to] : Pairs) {
                const std::size_t end = i + from.size();
                if (text.compare(i, from.size(), from) == 0 &&
                    (end == text.size() || !letter(text[end]))) {
                    out += to;
                    i = end;
                    traded = true;
                    break;
                }
            }
            if (traded) {
                continue;
            }
        }
        out += ch;
        ++i;
    }
    return out;
}

// Interleaved audio: each frame's left and right channels trade places, and so do the surround
// pairs. These orders put the left and right of a pair at 0/1, 4/5 and 6/7: the PS4's 8-channel
// L R C LFE Ls Rs Lb Rb, OpenAL's 5.1 FL FR FC LFE (S|B)L (S|B)R and 7.1 FL FR FC LFE BL BR SL SR.
// Quad is FL FR BL BR. Mono, or a buffer shorter than one frame, is left as it is; a partial last
// frame is never touched.
template <class T>
void SwapSides(std::span<T> samples, std::size_t channels) {
    if (channels < 2 || samples.size() < channels) {
        return;
    }
    const std::size_t frames = samples.size() / channels;
    const auto pair = [&](std::size_t a, std::size_t b) {
        for (std::size_t f = 0; f < frames; ++f) {
            T* frame = samples.data() + f * channels;
            std::swap(frame[a], frame[b]);
        }
    };
    pair(0, 1);
    if (channels == 4) {
        pair(2, 3);
    } else if (channels >= 6) {
        pair(4, 5);
        if (channels >= 8) {
            pair(6, 7);
        }
    }
}

} // namespace VR::Mirror
