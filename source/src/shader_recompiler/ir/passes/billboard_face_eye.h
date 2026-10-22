// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>

// Camera-facing sprites (smoke puffs, sparks) are spread along the camera's right and up rows, so
// they lie in the view plane. On a monitor that is invisible; in a headset the camera is your head,
// so every sprite turns when you turn it. FaceEye turns that right/up pair by the smallest rotation
// that takes their plane's normal (right x up, the camera's forward) to the direction from the eye
// to the sprite centre: a sprite on the view axis is unchanged, any other one faces the eye.
// Generic over the scalar so the shader pass (IR values) and the tests (float) run the same math.
namespace Shader::Optimization::BillboardFaceEye {

// Startup policy shared by compilation and cache preloading. Desktop rendering stays unchanged.
inline bool Enabled(bool native_stereo, bool vr_enabled, const char* override_value) {
    return override_value ? override_value[0] != '0' : native_stereo || vr_enabled;
}

inline float RecipSqrt(float x) {
    return 1.0f / std::sqrt(x);
}
inline float Recip(float x) {
    return 1.0f / x;
}
inline float Max(float a, float b) {
    return std::max(a, b);
}
// 1 when x > 0, else 0: a branch-free choice (the shader pass emits a compare and a select).
inline float Step(float x) {
    return x > 0.0f ? 1.0f : 0.0f;
}

template <class T>
using Vec3 = std::array<T, 3>;

template <class T>
Vec3<T> Cross(const Vec3<T>& a, const Vec3<T>& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
template <class T>
T Dot(const Vec3<T>& a, const Vec3<T>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

// The eye position in world space from a 3x4 world-to-view matrix with rows right, up, forward
// (forward = right x up) and translation t: eye = -(right t0 + up t1 + forward t2).
template <class T>
Vec3<T> EyeFromView(const Vec3<T>& right, const Vec3<T>& up, const Vec3<T>& t) {
    const Vec3<T> forward = Cross(right, up);
    Vec3<T> eye;
    for (int i = 0; i < 3; ++i) {
        eye[i] = (right[i] * t[0] + up[i] * t[1] + forward[i] * t[2]) * T(-1.0f);
    }
    return eye;
}

// Rodrigues for unit n, d: v' = v cos + w x v + w (w.v) / (1 + cos), w = n x d. The clamp only
// matters for a sprite behind the eye (cos near -1), which is clipped anyway.
template <class T>
std::array<Vec3<T>, 2> FaceEye(const Vec3<T>& right, const Vec3<T>& up, const Vec3<T>& center,
                               const Vec3<T>& eye) {
    const Vec3<T> n = Cross(right, up);
    Vec3<T> d{center[0] - eye[0], center[1] - eye[1], center[2] - eye[2]};
    const T inv_length = RecipSqrt(Dot(d, d));
    for (auto& x : d) {
        x = x * inv_length;
    }
    const T cosine = Dot(n, d);
    const Vec3<T> w = Cross(n, d);
    const T k = Recip(Max(cosine + T(1.0f), T(1e-4f)));
    const auto rotate = [&](const Vec3<T>& v) {
        const Vec3<T> wv = Cross(w, v);
        const T along = Dot(w, v) * k;
        Vec3<T> out;
        for (int i = 0; i < 3; ++i) {
            out[i] = v[i] * cosine + wv[i] + w[i] * along;
        }
        return out;
    };
    return {rotate(right), rotate(up)};
}

// Sprites built from per-particle axes (the effect system's 3x4 particle matrix: its first two
// columns span the sprite) and projected with a view-projection VP = P * V of a symmetric
// projection (w = view depth). The camera comes from VP's rows alone: forward = row 3; right and up
// are rows 0 and 1 without any forward part (a jittered or off-centre P adds some), normalized; the
// view translations follow the same way.
template <class T>
struct ViewFrame {
    Vec3<T> right, up, forward, eye;
};
template <class T>
ViewFrame<T> ViewFromViewProjection(const Vec3<T>& row0, T w0, const Vec3<T>& row1, T w1,
                                    const Vec3<T>& row3, T w3) {
    ViewFrame<T> v;
    const T inv_f = RecipSqrt(Dot(row3, row3));
    for (int i = 0; i < 3; ++i) {
        v.forward[i] = row3[i] * inv_f;
    }
    const T t2 = w3 * inv_f;
    const auto axis = [&](const Vec3<T>& row, T w, Vec3<T>& out) {
        const T along = Dot(row, v.forward);
        Vec3<T> a;
        for (int i = 0; i < 3; ++i) {
            a[i] = row[i] - v.forward[i] * along;
        }
        const T inv = RecipSqrt(Dot(a, a));
        for (int i = 0; i < 3; ++i) {
            out[i] = a[i] * inv;
        }
        return (w - along * t2) * inv;
    };
    const T t0 = axis(row0, w0, v.right);
    const T t1 = axis(row1, w1, v.up);
    for (int i = 0; i < 3; ++i) {
        v.eye[i] = (v.right[i] * t0 + v.up[i] * t1 + v.forward[i] * t2) * T(-1.0f);
    }
    return v;
}

// Upright eye-facing for per-particle axes: each axis keeps its right, up and forward components
// (its spin and size on the screen), re-expressed in a frame whose forward runs from the eye to the
// sprite centre and whose right is horizontal (+Y is up; within ~12 degrees of straight up or down
// the camera's up is used instead). A sprite on the view axis of a level head keeps its axes, and
// head turns and tilts no longer turn any sprite. `weight` is 1 only for sprites lying in the view
// plane (their axes' normal within ~8 degrees of the forward axis): any other particle keeps its
// own orientation.
template <class T>
std::array<Vec3<T>, 2> FaceEyeUpright(const ViewFrame<T>& view, const Vec3<T>& a0,
                                      const Vec3<T>& a1, const Vec3<T>& center, T& weight) {
    Vec3<T> d{center[0] - view.eye[0], center[1] - view.eye[1], center[2] - view.eye[2]};
    const T inv_d = RecipSqrt(Dot(d, d));
    for (auto& x : d) {
        x = x * inv_d;
    }
    // Right = up x d with up = +Y (= (d.z, 0, -d.x)), or the camera's up near the poles.
    const Vec3<T> level{d[2], T(0.0f), d[0] * T(-1.0f)};
    const Vec3<T> tilted = Cross(view.up, d);
    const T use_level = Step(Dot(level, level) - T(0.04f));
    Vec3<T> right;
    for (int i = 0; i < 3; ++i) {
        right[i] = tilted[i] + (level[i] - tilted[i]) * use_level;
    }
    // The camera's handedness: +1 when right = up x forward.
    const T hand = Step(Dot(Cross(view.up, view.forward), view.right)) * T(2.0f) - T(1.0f);
    const T inv_r = RecipSqrt(Dot(right, right)) * hand;
    for (auto& x : right) {
        x = x * inv_r;
    }
    Vec3<T> up = Cross(d, right);
    for (auto& x : up) {
        x = x * hand;
    }
    const auto face = [&](const Vec3<T>& a) {
        const T r = Dot(a, view.right), u = Dot(a, view.up), f = Dot(a, view.forward);
        Vec3<T> out;
        for (int i = 0; i < 3; ++i) {
            out[i] = right[i] * r + up[i] * u + d[i] * f;
        }
        return out;
    };
    const Vec3<T> n = Cross(a0, a1);
    const T nf = Dot(n, view.forward);
    weight = Step(nf * nf - Dot(n, n) * T(0.98f));
    return {face(a0), face(a1)};
}

} // namespace Shader::Optimization::BillboardFaceEye
