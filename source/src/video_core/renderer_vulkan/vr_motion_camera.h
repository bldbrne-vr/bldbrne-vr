// SPDX-License-Identifier: GPL-2.0-or-later
// bbport camera reprojection, adapted for exact native per-eye/asymmetric cameras.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace Vulkan::VrMotionCamera {
using Affine = std::array<float, 12>;
struct Camera {
    Affine view{}, inverse{};
    std::array<float, 4> projection{};
    std::array<float, 2> offset{};
};
struct alignas(16) Constants {
    Affine reproject{};
    std::array<float, 4> projection{}, previous_projection{}, offsets{};
    std::array<float, 2> size{}, jitter{}, previous_jitter{};
    uint32_t mode{};
};
static_assert(offsetof(Constants, offsets) == 80);
static_assert(offsetof(Constants, size) == 96);
static_assert(offsetof(Constants, mode) == 120);
static_assert(sizeof(Constants) == 128);

inline Affine Multiply(const Affine& a, const Affine& b) {
    Affine out{};
    for (unsigned r = 0; r < 3; ++r)
        for (unsigned c = 0; c < 4; ++c) {
            float v = c == 3 ? a[4 * r + 3] : 0.f;
            for (unsigned k = 0; k < 3; ++k)
                v += a[4 * r + k] * b[4 * k + c];
            out[4 * r + c] = v;
        }
    return out;
}
inline std::optional<Affine> Inverse(const Affine& m) {
    for (float x : m)
        if (!std::isfinite(x))
            return {};
    const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9],
                 i = m[10];
    const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
    if (!std::isfinite(det) || std::abs(det) < 1e-12)
        return {};
    const double s = 1.0 / det;
    const std::array<double, 9> v{(e * i - f * h) * s, (c * h - b * i) * s, (b * f - c * e) * s,
                                  (f * g - d * i) * s, (a * i - c * g) * s, (c * d - a * f) * s,
                                  (d * h - e * g) * s, (b * g - a * h) * s, (a * e - b * d) * s};
    Affine out{};
    for (unsigned r = 0; r < 3; ++r) {
        for (unsigned k = 0; k < 3; ++k)
            out[4 * r + k] = float(v[3 * r + k]);
        out[4 * r + 3] = float(-v[3 * r] * m[3] - v[3 * r + 1] * m[7] - v[3 * r + 2] * m[11]);
    }
    for (float x : out)
        if (!std::isfinite(x))
            return {};
    return out;
}
// Input is the FINAL 864-byte camera uploaded for this eye, after IPD and asymmetric
// remapping. Do not reconstruct it from a later headset pose. Viewport signs apply to
// both the projection scale and offset. No depth/scene guessing occurs here.
inline std::optional<Camera> Read(std::span<const float, 216> f, float x_sign, float y_sign) {
    if ((x_sign != -1.f && x_sign != 1.f) || (y_sign != -1.f && y_sign != 1.f) ||
        !(f[4] >= 64 && f[5] >= 64) || !std::isfinite(f[4] + f[5]) ||
        !(f[52] > 0 && f[57] > 0 && f[62] > 0 && f[63] < 0) || f[64] != 0 || f[65] != 0 ||
        f[66] != 1 || f[67] != 0 || f[53] != 0 || f[55] != 0 || f[56] != 0 || f[59] != 0 ||
        f[60] != 0 || f[61] != 0)
        return {};
    Camera camera;
    std::copy_n(f.begin() + 8, 12, camera.view.begin());
    const auto inverse = Inverse(camera.view);
    if (!inverse)
        return {};
    camera.inverse = *inverse;
    camera.projection = {f[52] * x_sign, f[57] * y_sign, f[62], f[63]};
    camera.offset = {f[54] * x_sign, f[58] * y_sign};
    for (float x : camera.projection)
        if (!std::isfinite(x))
            return {};
    for (float x : camera.offset)
        if (!std::isfinite(x))
            return {};
    return camera;
}
inline Constants Build(const Camera& current, const Camera& previous, uint32_t width,
                       uint32_t height, std::array<float, 2> jitter,
                       std::array<float, 2> previous_jitter, bool object_vectors) {
    return {Multiply(previous.view, current.inverse),
            current.projection,
            previous.projection,
            {current.offset[0], current.offset[1], previous.offset[0], previous.offset[1]},
            {float(width), float(height)},
            jitter,
            previous_jitter,
            object_vectors ? 1u : 0u};
}
} // namespace Vulkan::VrMotionCamera
