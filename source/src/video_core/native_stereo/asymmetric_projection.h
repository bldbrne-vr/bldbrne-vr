// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <cstring>
#include <array>
#include <cmath>

namespace NativeProjection {
// Clip-space remap of a symmetric camera into one located eye frustum.
// fov is OpenXR left/right/up/down, radians. No change to depth or world units.
struct Transform {
    float x_scale{1}, y_scale{1}, x_offset{}, y_offset{};
    bool valid{};
    bool operator==(const Transform&) const = default;
};
inline Transform Make(const std::array<float, 4>& fov, float tan_x, float tan_y, bool require_coverage = true) {
    for (const float a : fov) if (!std::isfinite(a) || std::abs(a) >= 1.5f) return {};
    if (!(tan_x > 0 && tan_y > 0) || !std::isfinite(tan_x + tan_y)) return {};
    // The FOV changes per camera update, not per draw: memoize the tangents by exact bits.
    struct TanMemo { std::array<float, 4> fov{}, tan{}; bool valid{}; };
    thread_local std::array<TanMemo, 2> memo{};
    thread_local unsigned victim{};
    const TanMemo* hit = nullptr;
    for (const auto& e : memo)
        if (e.valid && std::memcmp(e.fov.data(), fov.data(), sizeof(fov)) == 0) { hit = &e; break; }
    if (!hit) {
        auto& e = memo[victim ^= 1u];
        e = {fov, {std::tan(fov[0]), std::tan(fov[1]), std::tan(fov[2]), std::tan(fov[3])}, true};
        hit = &e;
    }
    const float l = hit->tan[0], r = hit->tan[1];
    const float u = hit->tan[2], d = hit->tan[3];
    // Never silently clip a headset edge outside the guest's visibility camera.
    if (!(l < 0 && r > 0 && d < 0 && u > 0)) return {};
    if (require_coverage && (-l > tan_x * 1.001f || r > tan_x * 1.001f ||
        -d > tan_y * 1.001f || u > tan_y * 1.001f)) return {};
    return {2 * tan_x / (r-l), 2 * tan_y / (u-d),
            -(r+l)/(r-l), -(u+d)/(u-d), true};
}
// Row-major matrices. Forward: T*M. Reconstruction/shadow transforms: M*inverse(T).
inline void Forward(float* m, const Transform& t) {
    for (int c=0; c<4; ++c) {
        m[c] = t.x_scale*m[c] + t.x_offset*m[12+c];
        m[4+c] = t.y_scale*m[4+c] + t.y_offset*m[12+c];
    }
}
inline void Inverse(float* m, const Transform& t) {
    for (int r=0; r<4; ++r) {
        m[4*r] /= t.x_scale;
        m[4*r+1] /= t.y_scale;
        m[4*r+3] -= t.x_offset*m[4*r] + t.y_offset*m[4*r+1];
    }
}
// Screen-to-projector/world matrices need the eye translation before inverse(T).
// inverse_projection is the ORIGINAL symmetric camera's inverse, kept separate
// because the destination may itself be a composed screen-to-decal transform.
inline void EyeReconstruction(float* m, const float* inverse_projection,
                              float eye, const Transform& t) {
    const auto original = std::array<float,16>{m[0],m[1],m[2],m[3],m[4],m[5],m[6],m[7],
                                               m[8],m[9],m[10],m[11],m[12],m[13],m[14],m[15]};
    for (int r=0; r<4; ++r) for (int c=0; c<4; ++c)
        m[4*r+c] += original[4*r] * eye / inverse_projection[0] * inverse_projection[12+c];
    Inverse(m,t);
}
inline void Scene(float* f, const Transform& t) {
    Forward(f+52,t);
    Forward(f+200,t);
    for (int block : {20,36,112,128,144,160}) Inverse(f+block,t);
}
// FAST_GPU_MIRROR_FLIP. A planar reflection (Bloodborne's water) is drawn in a pass of its own
// from the frame camera mirrored below the water, its view kept right-handed: its image is the
// mirror image upside down, and the water reads it at its own screen position flipped,
// (u, 1 - v). That read is exact only when the reflection's frustum is the eye's turned upside
// down: with the same eye frustum the flip reverses the eye's vertical offset and every eye reads
// 2 * y_offset of NDC off (Quest 3: about 19% of the eye's height), a band that slides over the
// water as the head pitches. In the scene block (Scene's dwords): the world-to-view rows 8..19,
// the camera position 183, 187, 191 (inverse view column 3), the clip plane 196..199 (clip
// space: the shader dots it with its clip position; 0, 0, 0, 1 when the camera clips nothing, as
// the frame's camera does) and the camera-relative view-projection 200..215. A mirrored camera
// clips by its mirror plane and sits behind it, the plane is level in the world (its normal
// within 15 degrees of world up or down: water, at any head roll or pitch; a wall mirror's read
// would flip sideways) and the view keeps its handedness (an inverted view would show the mirror
// image itself, read without a flip). `render` (null: not known), the frame's render camera
// position (VR::CameraRecord::render_position): it is the camera's mirror image, nearer to it
// than half the camera's distance from the plane (a record a little behind still passes). That
// refuses a camera at the eye that clips away all above the water (its image lies twice its
// height away). `either_side` (FAST_GPU_MIRROR_ABOVE; only with `render`): the camera may also sit
// on the kept side of its plane. At the Yharnam sewer gate the game mirrors the eye about a plane
// 1.65 m above it, and that camera keeps what is above the plane; its image is just as upside
// down, so its read needs the same flip. The render camera then still has to be its mirror image
// (the tolerance, half the camera's distance from the plane, refuses the eye's own camera with
// that plane: its image lies twice that distance away). No hash or game address takes part.
inline bool MirroredCamera(const float* f, const float* render = nullptr,
                           bool either_side = false) {
    // The clip plane in camera-relative world space, M^T * plane: q.xyz its normal, q.w its value
    // at the camera.
    std::array<float,4> q{};
    for (int r=0; r<4; ++r) for (int c=0; c<4; ++c) q[c] += f[200+4*r+c]*f[196+r];
    const float square = q[0]*q[0] + q[1]*q[1] + q[2]*q[2];
    if (!std::isfinite(square) || !std::isfinite(q[3]) || !(square > 0)) return false;
    const float normal = std::sqrt(square);
    const bool behind = q[3] < -1e-3f*normal;
    const bool kept = either_side && render && q[3] > 1e-3f*normal;
    if (!(behind || kept) || !(std::abs(q[1]) > .966f*normal)) return false;
    const float* v = f+8;
    const float handedness = v[0]*(v[5]*v[10]-v[6]*v[9]) - v[1]*(v[4]*v[10]-v[6]*v[8]) +
                             v[2]*(v[4]*v[9]-v[5]*v[8]);
    if (!(handedness > .5f)) return false;
    if (!render) return true;
    // The camera mirrored about the plane: position - 2 q.w q.xyz / |q.xyz|^2.
    float miss = 0;
    for (int i=0; i<3; ++i) {
        const float d = f[183+4*i] - 2*q[3]*q[i]/square - render[i];
        miss += d*d;
    }
    return std::sqrt(miss) < .5f*std::abs(q[3])/normal;
}
// The eye frustum upside down: Make with up and down swapped ({left, right, -down, -up}). Its read
// (u, 1 - v) then lands where the eye sees the reflection. x is untouched: the mirror is level.
// A vertically symmetric eye (y_offset 0) keeps its bits.
inline float FlippedOffset(float y_offset) { return y_offset != 0 ? -y_offset : y_offset; }
inline Transform Flipped(Transform t) {
    t.y_offset = FlippedOffset(t.y_offset);
    return t;
}
// The mirrored camera's clip plane (dwords 196..199) for one eye of `f`: the camera moved by `eye`
// along its right axis (inverse view column 0: 180, 184, 188), then remapped by `t` (a row:
// plane * inverse(T), Inverse's row rule). `center` is the original block: M = 200..215, the
// camera-relative view-projection, and the last row of inverse P (48..51), which dots every clip
// position to 1: the constant term the eye's move adds to the plane, (M^T plane).xyz . right * eye.
inline void EyeClipPlane(float* f, const float* center, float eye, const Transform& t) {
    float along = 0;
    for (int c=0; c<3; ++c) {
        float normal = 0;
        for (int r=0; r<4; ++r) normal += center[200+4*r+c]*center[196+r];
        along += normal*center[180+4*c];
    }
    std::array<float,4> plane{};
    for (int i=0; i<4; ++i) plane[i] = center[196+i] + eye*along*center[48+i];
    plane[0] /= t.x_scale;
    plane[1] /= t.y_scale;
    plane[3] -= t.x_offset*plane[0] + t.y_offset*plane[1];
    std::memcpy(f+196, plane.data(), sizeof(plane));
}
// The reflection's pass in recording order: from a graphics draw that pairs a mirrored camera until
// one pairs any other scene camera, the multiview draws of its screen size. Its lights, sky and
// composite read no camera of their own; they pair their lighting and push their projection and
// light tiles upside down too, so the pass stays one image.
struct MirrorPass {
    std::array<unsigned,2> screen{}; // {0, 0}: no pass
    // A graphics draw's scene camera, once per draw (`mirrored`: MirroredCamera); true when a pass
    // starts.
    bool Camera(bool mirrored, const std::array<unsigned,2>& draw_screen) {
        const bool start = mirrored && screen != draw_screen;
        screen = mirrored ? draw_screen : std::array<unsigned,2>{};
        return start;
    }
    bool Contains(const std::array<unsigned,2>& draw_screen, bool multiview) const {
        return multiview && screen[0] != 0 && screen == draw_screen;
    }
};
} // namespace NativeProjection
