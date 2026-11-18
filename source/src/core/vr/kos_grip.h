// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstring>
#include <span>
#include "core/vr/arm_ik.h"

namespace VR::Whip::Kos {

inline constexpr float RightGripScale = 1.06f;

// Uniform fit about the weapon attachment, after the native clip / lash solve.
// Havok does not scale child translations: scale every local translation, but
// only root scales, so every model-space bone grows exactly once. Keep rotations
// and padding lanes intact. No heap allocation; only the right Milkweed bundle
// uses this cache. Restore exact prior inputs if an evaluation reuses our output.
template <std::size_t Capacity>
class GripFit {
public:
    void Restore(std::span<Body::HkQsTransform> pose) {
        if (pose.size() == count_) {
            for (std::size_t i = 0; i < count_; ++i) {
                // A clip can refresh rotation while reusing translation/scale.
                // Restore only the channels we changed; never replace its rotation.
                if (std::memcmp(pose[i].t, after_[i].t, 3 * sizeof(float)) == 0)
                    std::memcpy(pose[i].t, before_[i].t, 3 * sizeof(float));
                if (std::memcmp(pose[i].s, after_[i].s, 3 * sizeof(float)) == 0)
                    std::memcpy(pose[i].s, before_[i].s, 3 * sizeof(float));
            }
        }
        count_ = 0;
    }

    template <class Index>
    bool Apply(std::span<Body::HkQsTransform> pose, std::span<const Index> parents) {
        if (pose.empty() || pose.size() > Capacity || parents.size() != pose.size())
            return false;
        // Validate the whole pose before writing any of it.
        for (std::size_t i = 0; i < pose.size(); ++i) {
            auto p = Body::FromHavok(pose[i]);
            if (parents[i] < -1 || parents[i] >= static_cast<int>(i)) return false;
            p.t = p.t * RightGripScale;
            if (parents[i] == -1) p.s = p.s * RightGripScale;
            if (!Body::IsFinite(p)) return false;
        }
        for (std::size_t i = 0; i < pose.size(); ++i) {
            before_[i] = pose[i];
            for (int c = 0; c < 3; ++c) {
                pose[i].t[c] *= RightGripScale;
                if (parents[i] == -1) pose[i].s[c] *= RightGripScale;
            }
            after_[i] = pose[i];
        }
        count_ = pose.size();
        return true;
    }

private:
    std::array<Body::HkQsTransform, Capacity> before_{};
    std::array<Body::HkQsTransform, Capacity> after_{};
    std::size_t count_{0};
};

} // namespace VR::Whip::Kos
