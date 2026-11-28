// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include <openxr/openxr.h>

#include "core/vr/mirror_world.h"

// Left-handed play (core/vr/mirror_world.h), the OpenXR side: what the runtime hands the presenter
// is reflected on the way in (a located pose with its velocity, a pair of located views), and the
// frame's layers on the way out (each pose reflected back, a projection's two views traded again).
// Called only while the session mirrors (OpenXRPresenter's mirror_world): right-handed play never
// reaches any of it.
namespace VrMirrorXr {

namespace M = VR::Mirror;

inline XrPosef Pose(const XrPosef& p) {
    return M::Pose(p);
}

inline XrFovf Fov(const XrFovf& f) {
    const M::Sides s = M::Fov(f.angleLeft, f.angleRight);
    return {s.left, s.right, f.angleUp, f.angleDown};
}

// A located space, and a velocity chained on it (XR_TYPE_SPACE_VELOCITY), in place.
inline void Located(XrSpaceLocation& location) {
    location.pose = Pose(location.pose);
    for (auto* next = static_cast<XrBaseOutStructure*>(location.next); next != nullptr;
         next = next->next) {
        if (next->type == XR_TYPE_SPACE_VELOCITY) {
            auto* velocity = reinterpret_cast<XrSpaceVelocity*>(next);
            velocity->linearVelocity = M::Vector(velocity->linearVelocity);
            velocity->angularVelocity = M::Axial(velocity->angularVelocity);
        }
    }
}

// A located stereo pair, in place: each view reflected, the two traded (the mirror's left eye is the
// reflection of your right one). Only the poses and fields of view move: each XrView keeps its own
// type and next. Anything but a pair is left as it is.
inline void Views(XrView* views, std::uint32_t count) {
    if (views == nullptr || count != 2) {
        return;
    }
    const XrPosef pose0 = views[0].pose;
    const XrFovf fov0 = views[0].fov;
    views[0].pose = Pose(views[1].pose);
    views[0].fov = Fov(views[1].fov);
    views[1].pose = Pose(pose0);
    views[1].fov = Fov(fov0);
}

// A layer seen by one eye only goes to the other.
inline XrEyeVisibility Eye(XrEyeVisibility eye) {
    return eye == XR_EYE_VISIBILITY_LEFT    ? XR_EYE_VISIBILITY_RIGHT
           : eye == XR_EYE_VISIBILITY_RIGHT ? XR_EYE_VISIBILITY_LEFT
                                            : eye;
}

// The frame's layers back into your room. Each layer is copied into this storage (nothing the
// presenter or a renderer keeps from frame to frame is changed) and its entry in the list points at
// the copy: a projection's two views reflected and traded (each view keeps its own sub-image and
// next chain, the depth among it: the pictures were flipped where they were written), a quad's and
// a cylinder's pose reflected and a one-eye layer given to the other eye; one fixed to the view
// (the VIEW space) is reflected the same way (a pose relative to the view: both reflected). A quad
// or cylinder in `keep_space` (XR_NULL_HANDLE: none) keeps its place. A layer of another type, one
// with other than two views, or one past this storage's room is left as it was (Unconverted counts
// them). One per frame thread; reused every frame.
class Layers {
public:
    static constexpr std::size_t Max = 16;

    void Convert(const XrCompositionLayerBaseHeader** layers, std::uint32_t count,
                 XrSpace keep_space) {
        projections_ = quads_ = cylinders_ = 0;
        unconverted_ = 0;
        if (layers == nullptr) {
            return;
        }
        for (std::uint32_t i = 0; i < count; ++i) {
            const XrCompositionLayerBaseHeader* layer = layers[i];
            if (layer == nullptr) {
                continue;
            }
            switch (layer->type) {
            case XR_TYPE_COMPOSITION_LAYER_PROJECTION: {
                const auto& src = *reinterpret_cast<const XrCompositionLayerProjection*>(layer);
                if (src.viewCount != 2 || src.views == nullptr || projections_ >= Max) {
                    ++unconverted_;
                    break;
                }
                auto& dst = projection_[projections_];
                auto& views = views_[projections_];
                ++projections_;
                dst = src;
                views[0] = src.views[1];
                views[1] = src.views[0];
                for (auto& view : views) {
                    view.pose = Pose(view.pose);
                    view.fov = Fov(view.fov);
                }
                dst.views = views.data();
                layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&dst);
                break;
            }
            case XR_TYPE_COMPOSITION_LAYER_QUAD: {
                if (keep_space != XR_NULL_HANDLE &&
                    reinterpret_cast<const XrCompositionLayerQuad*>(layer)->space == keep_space) {
                    break;
                }
                if (quads_ >= Max) {
                    ++unconverted_;
                    break;
                }
                auto& dst = quad_[quads_++];
                dst = *reinterpret_cast<const XrCompositionLayerQuad*>(layer);
                dst.pose = Pose(dst.pose);
                dst.eyeVisibility = Eye(dst.eyeVisibility);
                layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&dst);
                break;
            }
            case XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR: {
                if (keep_space != XR_NULL_HANDLE &&
                    reinterpret_cast<const XrCompositionLayerCylinderKHR*>(layer)->space ==
                        keep_space) {
                    break;
                }
                if (cylinders_ >= Max) {
                    ++unconverted_;
                    break;
                }
                auto& dst = cylinder_[cylinders_++];
                dst = *reinterpret_cast<const XrCompositionLayerCylinderKHR*>(layer);
                dst.pose = Pose(dst.pose);
                dst.eyeVisibility = Eye(dst.eyeVisibility);
                layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&dst);
                break;
            }
            default:
                ++unconverted_;
                break;
            }
        }
    }

    std::uint32_t Unconverted() const {
        return unconverted_;
    }

private:
    std::array<XrCompositionLayerProjection, Max> projection_{};
    std::array<std::array<XrCompositionLayerProjectionView, 2>, Max> views_{};
    std::array<XrCompositionLayerQuad, Max> quad_{};
    std::array<XrCompositionLayerCylinderKHR, Max> cylinder_{};
    std::size_t projections_{0}, quads_{0}, cylinders_{0};
    std::uint32_t unconverted_{0};
};

} // namespace VrMirrorXr
