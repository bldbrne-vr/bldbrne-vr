// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/vr/whip_chain.h"

namespace VR::Whip::TestMotion {
// The same hand inputs used by the before/after mock. A 140-degree arc through the front,
// with a 0.5-second smooth stroke after 0.3 seconds of wind-up. Both the hand and blade turn.
// kind: 0 right-to-left, 1 left-to-right, 2 top-to-bottom, 3 bottom-to-top.
inline Frame Slice(float t, int kind) {
    const float u = std::clamp((t - 0.30f) / 0.50f, 0.0f, 1.0f);
    const float eased = u * u * u * (10.0f + u * (-15.0f + 6.0f * u));
    const float angle = (70.0f - 140.0f * eased) * B::Deg * (kind % 2 == 0 ? 1.0f : -1.0f);
    const B::Vec3 blade = kind < 2 ? B::Vec3{std::sin(angle), 0, -std::cos(angle)}
                                 : B::Vec3{0, std::sin(angle), -std::cos(angle)};
    return {B::Vec3{0.22f, 1.5f, 0.15f} + blade * 0.55f,
            B::FromTo(B::Normalize(B::Vec3{0.381f, -0.883f, 0}), blade)};
}
inline float Ease(float u) {
    u = std::clamp(u, 0.0f, 1.0f);
    return u * u * u * (10.0f + u * (-15.0f + 6.0f * u));
}
// Forward throw, optionally changing direction while the chain is still paying out.
inline Frame Throw(float t, bool pull = false, bool swipe = false) {
    const float u = Ease((t - .25f) / .45f);
    const B::Vec3 a{.45f, 1.4f, .25f}, b{.5f, 1.9f, -.2f}, c{.2f, 1.65f, -.7f};
    B::Vec3 hand = a * ((1-u)*(1-u)) + b * (2*u*(1-u)) + c * (u*u);
    if (pull) hand.z += .65f * Ease((t - .72f) / .16f);
    const float turn = swipe ? Ease((t - .43f) / .25f) : 0.0f;
    hand.x -= .75f * turn;
    const auto direction = B::RotateAbout(
        B::Normalize(B::Lerp(B::Vec3{.3f, .3f, -1}, B::Vec3{0, 0, -1}, u)),
        {0, 1, 0}, 35.0f * B::Deg * turn);
    return {hand, B::FromTo(B::Normalize(B::Vec3{.381f, -.883f, 0}), direction)};
}
} // namespace VR::Whip::TestMotion
