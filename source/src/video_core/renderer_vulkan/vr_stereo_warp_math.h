// SPDX-License-Identifier: GPL-2.0-or-later

// DIBR stereo (stereo_mode=2) math shared by VrStereoWarp (vr_stereo_warp.cpp), the two compute
// shaders (host_shaders/vr_dibr_prep.comp, vr_dibr_warp.comp) and the CPU reference / tests.
// Header-only and Vulkan-free on purpose so a test can include it without the renderer.
//
// Model. The game renders ONE centre-eye frame (camera at the head centre). Its projection
// (builder 0x1D166F0) is D3D-style, left-handed, depth 0..1 at near..far, NOT reversed-Z:
//     P00 = cot(vfov/2) / aspect,  P11 = cot(vfov/2),  P22 = f/(f-n),  P32 = -n*f/(f-n)
//     z_ndc = f/(f-n) - n*f/((f-n)*z)   <=>   1/z = 1/n - z_ndc * (1/n - 1/f)
// so 1/z is affine in the stored depth. An eye camera with the same orientation and intrinsics,
// translated by b along the camera's right axis, sees a point at view depth z shifted by
//     D = fx * b / z  pixels,       fx = (W/2) * P00   (horizontal focal length in pixels)
// The view depth is unchanged by a sideways translation, so the eye's depth equals the centre's.
// Left eye (camera at -b): source pixel x lands at x + D. Right eye (+b): at x - D.
// Hence D(d) = K * (1/n - z_ndc(d) * (1/n - 1/f)) with K = fx * b, an affine function of d.

#pragma once

#include <algorithm>
#include <cmath>

namespace Vulkan::VrStereo {

/// Camera that rendered the single centre frame, exactly as the game projected it.
struct CameraParams {
    float near_z{0.2f};          ///< camera +0x58, game units
    float far_z{3000.0f};        ///< camera +0x5C, game units
    float vfov{};                ///< camera +0x50, vertical FOV in RADIANS
    float aspect{};              ///< camera +0x54, projection aspect (P00 = cot(vfov/2) / aspect)
    float half_ipd{};            ///< b: one eye's offset from the centre camera, game units
                                 ///<    (EffectiveIpd / 2 * world_scale). 0 = mono control.
    float units_per_metre{1.0f}; ///< game units per real metre; converts the *_m settings
    float depth_min{0.0f};       ///< viewport minDepth of the scene pass (zoffset)
    float depth_max{1.0f};       ///< viewport maxDepth of the scene pass (zoffset + zscale)
};

/// Push-constant payload of vr_dibr_prep.comp (layout must match PrepConstants there).
struct DisparityParams {
    float k_px{};           ///< K = fx * b
    float inv_near{};       ///< 1/n
    float inv_range{};      ///< 1/n - 1/f
    float depth_min{};      ///< viewport minDepth
    float inv_depth_span{}; ///< 1 / (maxDepth - minDepth)
    float knee_px{};        ///< near-field compression starts here (K / knee distance)
    float floor_px{};       ///< ... and never exceeds this (K / min distance); <= knee_px = off
    float snap_px{};        ///< |D| below this is exactly 0 (bit-exact copy path)
    float max_px{};         ///< hard cap, keeps the warp's LDS window inside 256 columns
};

/// |D| below this snaps to 0. Today (K = 19.9 px*m) that is everything beyond ~1275 m.
constexpr float kSnapPx = 1.0f / 64.0f;
/// Hard disparity cap. The warp stages 64 output columns plus the search window
/// (ceil(max + 1) - floor(min - 1) <= 185) plus 4 guard columns in 256 LDS columns per row:
/// 64 + 185 + 4 = 253 <= 256, so the window is never clipped.
constexpr float kMaxDisparityPx = 184.0f;
/// Normalised depth at or beyond this is sky / cleared depth / far plane: D = 0.
constexpr float kSkyDepthNdc = 1.0f - 1e-7f;
/// Thread-group geometry of the warp (vr_dibr_warp.comp) and tile size of the prep pass.
constexpr int kWarpGroupWidth = 64;
constexpr int kWarpGroupRows = 4;
constexpr int kTileSize = 16;

/// +1 for the left eye (eye 0, content moves +x), -1 for the right eye (eye 1).
constexpr float EyeSign(int eye) {
    return eye == 0 ? 1.0f : -1.0f;
}

/// Horizontal focal length in pixels of a render `width_px` wide: (W/2) * cot(vfov/2) / aspect.
inline float FocalLengthPx(float width_px, float vfov, float aspect) {
    return 0.5f * width_px / (std::tan(0.5f * vfov) * aspect);
}

inline bool IsCameraValid(const CameraParams& cam) {
    return cam.vfov > 0.0f && cam.vfov < 3.1f && cam.aspect > 0.0f && cam.near_z > 0.0f &&
           cam.far_z > cam.near_z && cam.depth_max != cam.depth_min && cam.half_ipd >= 0.0f &&
           cam.units_per_metre > 0.0f && std::isfinite(cam.half_ipd);
}

/// Builds the prep-pass constants for a disparity image `width_px` wide (the scene colour width).
/// knee_m / min_depth_m: near-field compression in real metres (knee <= 0 or min >= knee: off).
/// An invalid camera yields K = 0, i.e. both eyes equal the centre image (mono).
inline DisparityParams MakeDisparityParams(const CameraParams& cam, float width_px, float knee_m,
                                           float min_depth_m) {
    DisparityParams p{};
    p.snap_px = kSnapPx;
    p.max_px = kMaxDisparityPx;
    p.inv_depth_span = 1.0f;
    if (!IsCameraValid(cam) || width_px <= 0.0f) {
        return p;
    }
    p.k_px = FocalLengthPx(width_px, cam.vfov, cam.aspect) * cam.half_ipd;
    p.inv_near = 1.0f / cam.near_z;
    p.inv_range = 1.0f / cam.near_z - 1.0f / cam.far_z;
    p.depth_min = cam.depth_min;
    p.inv_depth_span = 1.0f / (cam.depth_max - cam.depth_min);
    if (p.k_px > 0.0f && knee_m > 0.0f && min_depth_m > 0.0f && min_depth_m < knee_m) {
        p.knee_px = p.k_px / (knee_m * cam.units_per_metre);
        p.floor_px = p.k_px / (min_depth_m * cam.units_per_metre);
    }
    return p;
}

/// Exact uncompressed disparity of a point at view depth z (game units): K / z.
inline float DisparityAtViewDepth(float k_px, float z) {
    return k_px / z;
}

/// Near-field knee: identity below knee_px, knee + r*tanh((D-knee)/r) above (r = floor - knee).
/// Monotonic (keeps occlusion order), C1 at the knee, never above floor_px.
inline float CompressNear(float D, const DisparityParams& p) {
    if (p.floor_px > p.knee_px && D > p.knee_px) {
        const float r = p.floor_px - p.knee_px;
        D = p.knee_px + r * std::tanh(std::min((D - p.knee_px) / r, 10.0f));
    }
    return D;
}

/// Stored depth (viewport units) -> one eye's disparity magnitude in pixels.
/// Literal port of DepthToDisparity() in vr_dibr_prep.comp.
inline float DepthToDisparity(float d, const DisparityParams& p) {
    float zn = (d - p.depth_min) * p.inv_depth_span;
    if (!(zn < kSkyDepthNdc)) {
        return 0.0f; // sky, cleared depth, far plane (and NaN)
    }
    zn = std::max(zn, 0.0f);
    float D = p.k_px * std::max(p.inv_near - zn * p.inv_range, 0.0f);
    D = CompressNear(D, p);
    D = std::min(D, p.max_px);
    return D < p.snap_px ? 0.0f : D;
}

/// Largest disparity the prep pass can emit for these constants (the warp's reach).
inline float ReachPx(const DisparityParams& p) {
    const float D = CompressNear(p.k_px * p.inv_near, p);
    return std::min(D, p.max_px);
}

/// Whole-pixel HUD disparity of a panel at hud_depth (game units) for `eye` (0 left, 1 right).
inline int HudShiftPx(float k_px, float hud_depth_units, int eye) {
    if (!(hud_depth_units > 0.0f) || !(k_px > 0.0f)) {
        return 0;
    }
    return static_cast<int>(std::lround(EyeSign(eye) * k_px / hud_depth_units));
}

/// The eye offset (metres, along the head's right axis) that matches the warp's baseline.
/// Submit the left eye at head - right * EyeOffsetMetres and the right eye at head + right * it.
inline float EyeOffsetMetres(const CameraParams& cam) {
    return cam.units_per_metre > 0.0f ? cam.half_ipd / cam.units_per_metre : 0.0f;
}

/// View depth (game units) -> stored depth in viewport units. For tests and debugging.
inline double StoredDepthFromViewDepth(double z, const CameraParams& cam) {
    const double n = cam.near_z;
    const double f = cam.far_z;
    const double zn = f / (f - n) - n * f / ((f - n) * z);
    return cam.depth_min + zn * (static_cast<double>(cam.depth_max) - cam.depth_min);
}

/// Stored depth -> view depth (game units). Inverse of StoredDepthFromViewDepth.
inline double ViewDepthFromStoredDepth(double d, const CameraParams& cam) {
    const double n = cam.near_z;
    const double f = cam.far_z;
    const double zn = (d - cam.depth_min) / (static_cast<double>(cam.depth_max) - cam.depth_min);
    return 1.0 / (1.0 / n - zn * (1.0 / n - 1.0 / f));
}

/// A capsule stamped into the disparity: something the game draws without writing depth that must
/// still warp at its own depth (the game's vial in the hunter's hand: an alpha material, so its
/// pixels hold the depth of whatever is behind the glass). View space of the centre camera, game
/// units: x right, y up, z forward along the view.
struct DepthProxy {
    bool valid{false};
    float a[3]{};
    float b[3]{};
    float radius{}; ///< game units
};

/// Push-constant tail of vr_dibr_prep.comp (layout must match the proxy_* members there): the
/// capsule's ends in disparity-image pixels, their 1/z, and radius * fx, so its radius in pixels at
/// a point of view depth z is r_fx / z. `on` > 0 stamps it.
struct ProxyParams {
    float ax{}, ay{}, bx{}, by{};
    float inv_za{}, inv_zb{};
    float r_fx{};
    float on{};
};

/// Projects `proxy` with the camera that rendered the frame (the game's D3D projection: x right,
/// y up becomes pixel y down) into a `width_px` x `height_px` disparity image. A capsule end in
/// front of the near plane is moved along the capsule onto it; wholly in front of it: off.
inline ProxyParams MakeProxyParams(const DepthProxy& proxy, const CameraParams& cam, float width_px,
                                   float height_px) {
    ProxyParams p{};
    if (!proxy.valid || !IsCameraValid(cam) || width_px <= 0.0f || height_px <= 0.0f ||
        !(proxy.radius > 0.0f)) {
        return p;
    }
    float a[3] = {proxy.a[0], proxy.a[1], proxy.a[2]};
    float b[3] = {proxy.b[0], proxy.b[1], proxy.b[2]};
    for (const float v : {a[0], a[1], a[2], b[0], b[1], b[2]}) {
        if (!std::isfinite(v)) {
            return p;
        }
    }
    const float n = cam.near_z;
    if (a[2] < n && b[2] < n) {
        return p;
    }
    const auto clip = [n](float* behind, const float* front) {
        const float t = (n - behind[2]) / (front[2] - behind[2]);
        for (int i = 0; i < 3; ++i) {
            behind[i] += (front[i] - behind[i]) * t;
        }
        behind[2] = n;
    };
    if (a[2] < n) {
        clip(a, b);
    } else if (b[2] < n) {
        clip(b, a);
    }
    const float fx = FocalLengthPx(width_px, cam.vfov, cam.aspect);
    const float fy = 0.5f * height_px / std::tan(0.5f * cam.vfov);
    p.ax = 0.5f * width_px + fx * a[0] / a[2];
    p.ay = 0.5f * height_px - fy * a[1] / a[2];
    p.bx = 0.5f * width_px + fx * b[0] / b[2];
    p.by = 0.5f * height_px - fy * b[1] / b[2];
    p.inv_za = 1.0f / a[2];
    p.inv_zb = 1.0f / b[2];
    p.r_fx = proxy.radius * fx;
    p.on = 1.0f;
    return p;
}

/// The disparity the prep pass stamps at pixel centre (x, y) for the proxy (0 outside it): the
/// capsule's 1/z, interpolated along its projected axis (affine in the image), through the same
/// knee, cap and snap as the depth. Literal port of ProxyDisparity() in vr_dibr_prep.comp.
inline float ProxyDisparity(float x, float y, const ProxyParams& q, const DisparityParams& p) {
    if (!(q.on > 0.0f)) {
        return 0.0f;
    }
    const float abx = q.bx - q.ax;
    const float aby = q.by - q.ay;
    const float t = std::clamp(
        ((x - q.ax) * abx + (y - q.ay) * aby) / std::max(abx * abx + aby * aby, 1e-6f), 0.0f, 1.0f);
    const float inv_z = q.inv_za + (q.inv_zb - q.inv_za) * t;
    const float dx = x - (q.ax + abx * t);
    const float dy = y - (q.ay + aby * t);
    if (dx * dx + dy * dy > (q.r_fx * inv_z) * (q.r_fx * inv_z)) {
        return 0.0f;
    }
    float D = CompressNear(p.k_px * inv_z, p);
    D = std::min(D, p.max_px);
    return D < p.snap_px ? 0.0f : D;
}

} // namespace Vulkan::VrStereo
