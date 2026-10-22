// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include "core/vr/body_frame.h"
#include "core/vr/physical_lever.h"
#include <optional>

namespace VR::Lever {
// Keep the hinge in game-world orientation, but metres for grab/slip distances. Mapping
// the hinge axis as an ordinary tracking vector would reverse its handedness: the body's
// tracking bridge reflects Z. Map hand points instead, using that same existing bridge.
class Space {
public:
    static std::optional<Space> From(const Body::Anchor &anchor, std::int64_t now_ns) {
        if (!anchor.valid || anchor.steady_ns <= 0 || now_ns < anchor.steady_ns ||
            now_ns - anchor.steady_ns > 100'000'000 || !Body::IsFinite(anchor.eye_world) ||
            !Body::IsFinite(anchor.head_k) || !std::isfinite(anchor.base_yaw) ||
            !std::isfinite(anchor.world_scale) || anchor.world_scale < 0.01f ||
            anchor.world_scale > 100.0f) {
            return std::nullopt;
        }
        return Space(anchor);
    }

    Vec3 HandInMetres(Vec3 tracking_position) const {
        return anchor_.eye_world * (1.0f / anchor_.world_scale) +
               Body::TrackingToWorld(tracking_position - anchor_.head_k, anchor_.base_yaw, 1.0f);
    }

    Vec3 PinInTracking(Vec3 world_metres) const {
        return anchor_.head_k +
               Body::WorldToTracking(world_metres * anchor_.world_scale - anchor_.eye_world,
                                     anchor_.base_yaw, anchor_.world_scale);
    }

    std::optional<Rig> RigInMetres(const Rig &world_units) const {
        const Rig rig = Scaled(world_units, 1.0f / anchor_.world_scale);
        return Valid(rig) ? std::optional<Rig>{rig} : std::nullopt;
    }

private:
    explicit Space(const Body::Anchor &anchor) : anchor_(anchor) {}
    Body::Anchor anchor_;
};
} // namespace VR::Lever
