// SPDX-License-Identifier: GPL-2.0-or-later

// Temporal anti-aliasing in the game frame (vr.ini aa_mode=taa, 2026-10-08): the pure maths, the
// CPU mirror of host_shaders/vr_render_taa.comp and of the vertex epilogue's jitter
// (shader_recompiler/backend/spirv/emit_spirv_special.cpp ApplyStereoShift).
//
// Why: the flicker left after render_steady is the game render's own aliasing (thin edges,
// foliage, glints) changing with every slightly different viewpoint while the head moves 4-30
// deg/s. Per-frame AA (FXAA, SMAA) cannot fix that: each frame's edges are decided anew. TAA in
// the game frame does: every frame is rendered a sub-pixel step off (Halton 2,3), and a resolve at
// the game's AA pass gathers those samples over time through a history that is reprojected with
// the frame's depth and the exact render cameras, so an edge converges to its true coverage and
// stays there whether the head is still or moving.
//
//  - The jitter: a clip-space offset in the vertex epilogue, x += jx * w, y += jy * w for
//    perspective vertices (an exact off-centre projection; the same pixel step everywhere on the
//    screen, unlike a camera rotation, whose step grows as 1 + tan^2 toward the render's edges).
//    The game's matrices, the camera record, the DIBR and the late warp stay unjittered: the
//    resolve outputs the unjittered image. It travels in the push block's two VR floats:
//    vr_shift = jx and vr_hud = JitterTag + jy (HUD values are >= 0.55, plain scene draws 0).
//  - The resolve (per output pixel, in the unjittered grid): the current frame's 3x3 filtered at
//    the unjittered centre (a Blackman-Harris-like Gaussian), the history reprojected through the
//    closest depth of the 3x3 and both render cameras, clipped to the 3x3's YCoCg variance box,
//    rejected where the history's surface is nearer than the reprojected point (a disocclusion),
//    blended with alpha = max(1 / (n + 1), alpha_min) and luma weights (anti-flicker).
//  - A stable depth for the DIBR and the late warp: per pixel the 3x3 depth tap nearest the
//    previous stable depth (so an edge pixel does not flip between foreground and background
//    disparity as the jitter moves the edge by +-0.5 px).
//
// Header-only and dependency-free (unit tests: tests/test_render_taa.cpp).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

namespace VR::RenderTaa {

// ---- The jitter sequence ----

// The radical inverse of `index` in `base` (Halton): 0..1.
inline float RadicalInverse(std::uint32_t index, std::uint32_t base) {
    float f = 1.0f;
    float r = 0.0f;
    while (index > 0) {
        f /= static_cast<float>(base);
        r += f * static_cast<float>(index % base);
        index /= base;
    }
    return r;
}

struct Jitter {
    float x{0.0f}; // render pixels, -0.5 .. 0.5 (+ = the image moves right)
    float y{0.0f}; // render pixels, -0.5 .. 0.5 (+ = the image moves up in NDC)
};

// Phase `index` of a `phases`-long Halton (2, 3) cycle (8 or 16), centred: -0.5 .. 0.5 px. Index
// 1-based like the classic tables (index 0 would be the unjittered centre twice per cycle).
inline Jitter Halton(std::uint32_t index, std::uint32_t phases = 8) {
    phases = phases >= 16 ? 16u : 8u;
    const std::uint32_t i = (index % phases) + 1;
    return {RadicalInverse(i, 2) - 0.5f, RadicalInverse(i, 3) - 0.5f};
}

// The NDC offset of a pixel jitter on a W x H render (NDC spans 2 over the render).
inline Jitter JitterNdc(Jitter px, float width, float height) {
    if (!(width > 0.0f) || !(height > 0.0f)) {
        return {};
    }
    return {2.0f * px.x / width, 2.0f * px.y / height};
}

// ---- The push block encoding (vr_shift, vr_hud) and the epilogue ----

// vr_hud of a jittered scene draw: JitterTag + jy (|jy| < 0.01 always). HUD values are >= 0.55,
// plain draws 0: the epilogue tells them apart by hud < JitterTest.
inline constexpr float JitterTag = -2.0f;
inline constexpr float JitterTest = -1.0f;

struct Push {
    float shift{0.0f};
    float hud{0.0f};
};

inline Push EncodePush(Jitter ndc) {
    return {ndc.x, JitterTag + ndc.y};
}

struct Clip {
    float x, y;
};

// The epilogue's rule exactly (ApplyStereoShift): perspective vertices (|w - 1| > 1e-5) get the
// stereo shift (vr_shift, plain draws) or the jitter (x += jx * w, y += jy * w, jittered draws);
// HUD vertices (w == 1, hud > 0, inside the screen) are scaled toward the centre and offset; the
// rest is untouched.
inline Clip EpilogueCpu(float x, float y, float w, float shift, float hud) {
    const bool perspective = std::abs(w - 1.0f) > 1e-5f;
    const bool jit = hud < JitterTest;
    if (perspective) {
        if (jit) {
            return {x + shift * w, y + (hud - JitterTag) * w};
        }
        return {x + shift, y};
    }
    const bool inside = std::max(std::abs(x), std::abs(y)) < 0.999f;
    if (hud > 0.0f && inside) {
        const float steps = std::nearbyint(hud);
        const float scale = steps * (1.0f / 16.0f);
        const float offset = hud - steps;
        return {x * scale + offset, y * scale};
    }
    return {x, y};
}

// ---- Cameras and the reprojection ----

// The render camera's rows (right, up, forward), game convention, as the render-camera hook's
// WriteOrientation writes them: yaw + = right, pitch + = down, roll + = the right side up. From a
// camera record: yaw = base_yaw - head_yaw, pitch = -head_pitch, roll = head_roll.
inline std::array<float, 9> CameraRows(float yaw, float pitch, float roll) {
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    const float sp = std::sin(pitch), cp = std::cos(pitch);
    std::array<float, 9> r{cy, 0.0f, -sy, sy * sp, cp, cy * sp, sy * cp, -sp, cy * cp};
    if (roll != 0.0f) {
        const float sr = std::sin(roll), cr = std::cos(roll);
        for (int i = 0; i < 3; ++i) {
            const float right = r[i], up = r[3 + i];
            r[i] = right * cr + up * sr;
            r[3 + i] = up * cr - right * sr;
        }
    }
    return r;
}

struct Camera {
    std::array<float, 9> rows{1, 0, 0, 0, 1, 0, 0, 0, 1}; // right, up, forward (world)
    std::array<float, 3> pos{0, 0, 0};                    // world (game units)
    float fx{1.0f}; // focal lengths in render pixels
    float fy{1.0f};
};

// The focal lengths of a symmetric projection (vfov radians, aspect = P11 / P00) on W x H.
inline void Focal(float width, float height, float vfov, float aspect, float& fx, float& fy) {
    const float cot = 1.0f / std::tan(0.5f * vfov);
    fy = 0.5f * height * cot;
    fx = 0.5f * width * cot / (aspect > 0.0f ? aspect : width / height);
}

// current view -> previous view: v_prev = M v_cur + t (game units), M = Rp Rc^T, t = Rp (pc - pp).
struct Reprojector {
    std::array<float, 9> m{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<float, 3> t{0, 0, 0};
};

inline Reprojector MakeReprojector(const Camera& cur, const Camera& prev) {
    Reprojector r{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            float s = 0.0f;
            for (int k = 0; k < 3; ++k) {
                s += prev.rows[i * 3 + k] * cur.rows[j * 3 + k];
            }
            r.m[i * 3 + j] = s;
        }
        float s = 0.0f;
        for (int k = 0; k < 3; ++k) {
            s += prev.rows[i * 3 + k] * (cur.pos[k] - prev.pos[k]);
        }
        r.t[i] = s;
    }
    return r;
}

struct PixelPos {
    float u{0.0f}, v{0.0f};
    bool valid{false};
};

// Where pixel position (u, v) (window pixels of a W x H render, centre W/2, H/2) with inverse view
// depth inv_z (1/z, game units; 0 = the sky: rotation only) lands in the previous frame. ysign:
// +1 when window y grows with view y, -1 when it shrinks (the usual: y down on the screen).
inline PixelPos Reproject(const Reprojector& r, float u, float v, float inv_z, float width,
                          float height, float fx, float fy, float pfx, float pfy, float ysign) {
    const float cx = 0.5f * width, cy = 0.5f * height;
    // Homogeneous view point (x, y, 1) * z, scaled by 1/z: (dx, dy, 1, inv_z).
    const float dx = (u - cx) / fx;
    const float dy = ysign * (v - cy) / fy;
    const float px = r.m[0] * dx + r.m[1] * dy + r.m[2] + r.t[0] * inv_z;
    const float py = r.m[3] * dx + r.m[4] * dy + r.m[5] + r.t[1] * inv_z;
    const float pz = r.m[6] * dx + r.m[7] * dy + r.m[8] + r.t[2] * inv_z;
    if (!(pz > 1e-6f)) {
        return {};
    }
    return {cx + pfx * px / pz, cy + ysign * pfy * py / pz, true};
}

// The expected inverse depth of the same point in the previous view (for the disocclusion test).
inline float PrevInverseDepth(const Reprojector& r, float u, float v, float inv_z, float width,
                              float height, float fx, float fy, float ysign) {
    const float dx = (u - 0.5f * width) / fx;
    const float dy = ysign * (v - 0.5f * height) / fy;
    const float pz = r.m[6] * dx + r.m[7] * dy + r.m[8] + r.t[2] * inv_z;
    return pz > 1e-6f ? inv_z / pz : 0.0f;
}

// ---- Depth ----

// The stored depth's inverse view depth, affine in it (D3D projection, depth 0 at near, 1 at far,
// not reversed; vr_dibr_prep.comp's InverseDepth): 1/z = a - b * d, 0 at and beyond `sky`.
struct DepthAffine {
    float a{0.0f};
    float b{0.0f};
    float sky{1.0f};
};

inline DepthAffine MakeDepthAffine(float near_z, float far_z, float depth_min, float depth_max) {
    const float inv_near = 1.0f / near_z;
    const float inv_range = inv_near - 1.0f / far_z;
    const float span = 1.0f / (depth_max - depth_min);
    return {inv_near + depth_min * span * inv_range, span * inv_range,
            depth_min + (1.0f - 1e-7f) / span};
}

inline float InverseDepth(const DepthAffine& d, float depth) {
    if (!(depth < d.sky)) {
        return 0.0f;
    }
    return std::max(d.a - d.b * depth, 0.0f);
}

// The stable depth of a pixel from its 3x3 jittered depth taps: the tap nearest the previous
// stable value `prev` (any tap: the edge's side the pixel was on stays its side while the jitter
// moves the edge by +-0.5 px), or `centre` (the tap nearest the unjittered centre) without a
// history or where the taps hardly differ (flat surfaces).
inline float StableDepth(const float* taps, int count, float centre, bool has_prev, float prev,
                         float flat_eps) {
    float lo = taps[0], hi = taps[0];
    for (int i = 1; i < count; ++i) {
        lo = std::min(lo, taps[i]);
        hi = std::max(hi, taps[i]);
    }
    if (!has_prev || hi - lo <= flat_eps) {
        return centre;
    }
    float best = taps[0];
    for (int i = 1; i < count; ++i) {
        if (std::abs(taps[i] - prev) < std::abs(best - prev)) {
            best = taps[i];
        }
    }
    return best;
}

// ---- Colour ----

struct Rgb {
    float r{0.0f}, g{0.0f}, b{0.0f};
};

// YCoCg (Y = R/4 + G/2 + B/4: symmetric in R and B, so the game's red/blue swap does not matter).
inline Rgb RgbToYCoCg(Rgb c) {
    return {0.25f * c.r + 0.5f * c.g + 0.25f * c.b, 0.5f * c.r - 0.5f * c.b,
            -0.25f * c.r + 0.5f * c.g - 0.25f * c.b};
}

inline Rgb YCoCgToRgb(Rgb c) {
    const float y = c.r, co = c.g, cg = c.b;
    return {y + co - cg, y + cg, y - co - cg};
}

// Clips `h` toward the box centre `mean` onto the box mean +- extent (per channel); inside: as is.
// Returns the clipped value and how far outside it was (1 = on the surface, > 1 outside).
inline Rgb ClipToBox(Rgb h, Rgb mean, Rgb extent, float* outside = nullptr) {
    const Rgb d{h.r - mean.r, h.g - mean.g, h.b - mean.b};
    const auto ratio = [](float dv, float e) { return std::abs(dv) / std::max(e, 1e-5f); };
    const float m = std::max({ratio(d.r, extent.r), ratio(d.g, extent.g), ratio(d.b, extent.b)});
    if (outside != nullptr) {
        *outside = m;
    }
    if (m <= 1.0f) {
        return h;
    }
    return {mean.r + d.r / m, mean.g + d.g / m, mean.b + d.b / m};
}

// The current frame's share: 1 / (n + 1) (an average over the samples so far) down to alpha_min.
inline float BlendAlpha(float n, float alpha_min) {
    return std::max(1.0f / (n + 1.0f), alpha_min);
}

// The accumulation count's cap: 1 / alpha_min.
inline float CountCap(float alpha_min) {
    return 1.0f / std::max(alpha_min, 1e-3f);
}

// Luma-weighted blend (anti-flicker: a bright sample counts less): luma = YCoCg's Y, weights
// 1 / (1 + k luma). k = 1 is round 1's full weighting; it also dims a converged sub-pixel glint
// (bright in a few jitter phases), k = 0 is the plain blend, which keeps its energy (taa_luma_weight).
inline Rgb Blend(Rgb cur, Rgb hist, float alpha, float luma_k = 1.0f) {
    const float lc = RgbToYCoCg(cur).r, lh = RgbToYCoCg(hist).r;
    const float wc = alpha / (1.0f + luma_k * lc);
    const float wh = (1.0f - alpha) / (1.0f + luma_k * lh);
    const float s = 1.0f / std::max(wc + wh, 1e-6f);
    return {(cur.r * wc + hist.r * wh) * s, (cur.g * wc + hist.g * wh) * s,
            (cur.b * wc + hist.b * wh) * s};
}

// taa_luma_weight in push flags2 bits 0..7 (/255), and back as the shader reads it.
inline std::uint32_t LumaCode(float k) {
    return static_cast<std::uint32_t>(std::clamp(std::nearbyint(k * 255.0f), 0.0f, 255.0f));
}
inline float LumaWeight(std::uint32_t flags2) {
    return static_cast<float>(flags2 & 255u) / 255.0f;
}

// taa_sharpen_lift in push flags2 bits 16..23 (0 .. 0.5 in 1/510 steps): RCAS's limit treats
// black as -lift, so dark textures get sharpened too (RCAS alone keeps its output above black,
// which leaves a dark scene's textures nearly unsharpened).
inline std::uint32_t LiftCode(float lift) {
    return static_cast<std::uint32_t>(std::clamp(std::nearbyint(lift * 510.0f), 0.0f, 255.0f));
}
inline float Lift(std::uint32_t flags2) {
    return static_cast<float>((flags2 >> 16) & 255u) * (0.5f / 255.0f);
}

// The TAA_SHARPEN pass's lobe for one channel (RCAS, FSR 1): the cross's min / max `mn4` / `mx4`
// and centre `e` (0 .. 1), `amount` (taa_sharpen), `lift` (taa_sharpen_lift). -0.1875 x amount at
// most; the output (e + lobe x sum4) / (1 + 4 lobe) is then clamped to 0 .. 1.
inline float RcasLobe(float mn4, float mx4, float e, float amount, float lift) {
    const float hit_min = (std::min(mn4, e) + lift) / std::max(4.0f * (mx4 + lift), 1e-5f);
    const float hit_max = (1.0f - std::max(mx4, e)) / std::min(4.0f * mn4 - 4.0f, -1e-5f);
    const float lobe3 = std::max(-hit_min, hit_max);
    return std::max(-0.1875f, std::min(lobe3, 0.0f)) * amount;
}

// The occluder test (taa_occluder, flags2 bit 8): the history's 2x2, even its nearest tap
// (`hist_near_iz`, 1/z in the previous view), lies farther than every tap of this frame's 3x3
// (`cur_far_iz`: its farthest, seen from the previous view) by more than 4 depth_reject + 1%:
// something without motion vectors moved in front (an enemy's leading edge). 0 = sky / none.
inline bool OccluderArrived(float hist_near_iz, float cur_far_iz, float depth_reject) {
    return cur_far_iz > 0.0f && hist_near_iz * (1.0f + 4.0f * depth_reject + 0.01f) < cur_far_iz;
}

// The reconstruction filter (taa_filter_px): a Gaussian of sigma 0.2 .. 0.55 render px in 0.05
// steps, sent to the shader as a 3-bit code (push flags bits 5..7). UE4's Blackman-Harris fit,
// exp(-2.29 d^2), is sigma 0.47: softer than needed (the jitter's 1 px box is the anti-aliasing).
inline std::uint32_t FilterCode(float sigma_px) {
    const float c = std::nearbyint((std::clamp(sigma_px, 0.2f, 0.55f) - 0.2f) / 0.05f);
    return static_cast<std::uint32_t>(std::clamp(c, 0.0f, 7.0f));
}
inline float FilterSigma(std::uint32_t code) {
    return 0.2f + 0.05f * static_cast<float>(code & 7u);
}
// exp(-k d^2) with k = 1 / (2 sigma^2).
inline float FilterK(std::uint32_t code) {
    const float s = FilterSigma(code);
    return 0.5f / (s * s);
}

// The weight of a tap `dx`, `dy` render pixels from the output centre.
inline float FilterWeight(float dx, float dy, float k = FilterK(FilterCode(0.45f))) {
    return std::exp(-k * (dx * dx + dy * dy));
}

// The near field (taa_near_m / taa_near_alpha, push flags bits 24..31 = near_m in 0.05 m steps,
// bits 16..23 = the least alpha in 1/255): within near_m of the eyes the current frame keeps at
// least near_alpha (full at near_m, none at 1.5 near_m, smoothstep): the hunter's weapon and
// hands move without motion vectors.
inline std::uint32_t NearCode(float near_m, float near_alpha) {
    const auto m = static_cast<std::uint32_t>(std::clamp(std::nearbyint(near_m / 0.05f), 0.0f, 255.0f));
    const auto a = static_cast<std::uint32_t>(std::clamp(std::nearbyint(near_alpha * 255.0f), 0.0f, 255.0f));
    return (m << 24) | (a << 16);
}
// taa_near_m is in metres, but the shader compares it with the game's own view depth (1 / iz), which
// is in game units: vr.ini world_scale is game units per metre (1 for Bloodborne). The near field
// in game units, before NearCode.
inline float NearGameUnits(float near_m, float world_scale) {
    return near_m * (world_scale > 0.0f ? world_scale : 1.0f);
}
inline float NearAlpha(float alpha, float z, std::uint32_t flags) {
    const float near_m = static_cast<float>((flags >> 24) & 255u) * 0.05f;
    const float near_a = static_cast<float>((flags >> 16) & 255u) / 255.0f;
    if (near_m <= 0.0f || z <= 0.0f) {
        return alpha;
    }
    const float x = std::clamp((1.5f * near_m - z) / (0.5f * near_m), 0.0f, 1.0f);
    return std::max(alpha, near_a * x * x * (3.0f - 2.0f * x));
}

// The TAA_SHARPEN pass's near-field fade (taa_sharpen and taa_sharpen_lift are both scaled by
// it): 0 within near_m, 1 from 1.5 near_m (smoothstep), `z` the nearest depth of the pixel's 3x3
// and its cross 2 px out (0 = sky); 1 with the near field off. The weapon and hands are mostly
// this frame's jittered image (NearAlpha), and sharpening them brought the jitter's flicker back.
inline float SharpenFade(float z, std::uint32_t flags) {
    const float near_m = static_cast<float>((flags >> 24) & 255u) * 0.05f;
    if (near_m <= 0.0f || z <= 0.0f) {
        return 1.0f;
    }
    const float x = std::clamp((z - near_m) / (0.5f * near_m), 0.0f, 1.0f);
    return x * x * (3.0f - 2.0f * x);
}

// The near field's reconstruction filter: at least round 1's 0.45 px. There the result is mostly
// this frame's jittered image, and taa_filter_px 0.35 let the jitter through as flicker (halved by
// 0.45 in the skeptic's isolation run). The shader mixes the two filtered values by the near
// field's weight 1 - SharpenFade (the 3x3's nearest depth).
inline constexpr float NearFilterPx = 0.45f;
inline float NearFilterK(float filter_px) {
    const float s = std::max(FilterSigma(FilterCode(filter_px)), NearFilterPx);
    return 0.5f / (s * s);
}

// ---- The history's lifetime ----

enum class Reset : std::uint8_t {
    None = 0,
    First,  // no previous frame (or it was refused)
    Record, // a camera record is missing or unusable
    Turn,   // an artificial turn or camera takeover (turn_serial)
    Space,  // a recenter (space_serial) or a world scale change
    Gap,    // the previous resolve is too old (> 70 ms: a pause, a load)
    Jump,   // the camera jumped (> 0.35 rad or > 1 m)
    Size,   // the render size changed
    Count,
};

inline const char* ResetName(Reset r) {
    switch (r) {
    case Reset::None:
        return "none";
    case Reset::First:
        return "first";
    case Reset::Record:
        return "record";
    case Reset::Turn:
        return "turn";
    case Reset::Space:
        return "recenter";
    case Reset::Gap:
        return "gap";
    case Reset::Jump:
        return "jump";
    case Reset::Size:
        return "size";
    default:
        return "?";
    }
}

struct FrameFacts {
    bool valid{false};
    std::uint32_t width{0}, height{0};
    std::uint32_t turn_serial{0};
    std::uint32_t space_serial{0};
    float world_scale{1.0f}; // game units per metre
    double time_s{0.0};
    std::array<float, 9> rows{1, 0, 0, 0, 1, 0, 0, 0, 1};
    std::array<float, 3> pos{0, 0, 0};
};

inline Reset ResetReason(const FrameFacts& prev, const FrameFacts& cur) {
    if (!cur.valid) {
        return Reset::Record;
    }
    if (!prev.valid) {
        return Reset::First;
    }
    if (prev.width != cur.width || prev.height != cur.height) {
        return Reset::Size;
    }
    if (prev.turn_serial != cur.turn_serial) {
        return Reset::Turn;
    }
    if (prev.space_serial != cur.space_serial || prev.world_scale != cur.world_scale) {
        return Reset::Space;
    }
    const double dt = cur.time_s - prev.time_s;
    if (dt > 0.070 || dt < -0.001) {
        return Reset::Gap;
    }
    // The angle between the two cameras: trace(Rp Rc^T) = 1 + 2 cos(angle).
    float trace = 0.0f;
    for (int i = 0; i < 9; ++i) {
        trace += prev.rows[i] * cur.rows[i];
    }
    const float cos_angle = std::clamp(0.5f * (trace - 1.0f), -1.0f, 1.0f);
    const float dx = cur.pos[0] - prev.pos[0], dy = cur.pos[1] - prev.pos[1],
                dz = cur.pos[2] - prev.pos[2];
    const float dist_m = std::sqrt(dx * dx + dy * dy + dz * dz) / std::max(cur.world_scale, 1e-6f);
    if (std::acos(cos_angle) > 0.35f || dist_m > 1.0f) {
        return Reset::Jump;
    }
    return Reset::None;
}

// ---- One output pixel (the shader's main, minus the image access) ----

struct Params {
    float alpha_min{0.1f};
    float gamma{1.0f};        // the variance box's half-size in sigma
    float depth_reject{0.01f}; // relative 1/z
    float filter_px{0.45f};   // taa_filter_px
    float luma_k{1.0f};       // taa_luma_weight
};

struct PixelIn {
    std::array<Rgb, 9> taps{};    // the 3x3 of the jittered frame around the output pixel
    std::array<float, 9> dx{};    // each tap's offset from the output centre (render px)
    std::array<float, 9> dy{};
    bool has_history{false};
    Rgb history{};                // the reprojected history sample
    float n{0.0f};                // its accumulation count
    bool disoccluded{false};      // the history's surface is nearer than this point
    bool occluder{false};         // OccluderArrived: something moved in front of it
};

struct PixelOut {
    Rgb rgb{};
    float n{0.0f};
};

inline PixelOut ResolvePixel(const PixelIn& in, const Params& p) {
    Rgb sum{};
    float wsum = 0.0f;
    Rgb m1{}, m2{};
    for (int i = 0; i < 9; ++i) {
        const float w = FilterWeight(in.dx[i], in.dy[i], FilterK(FilterCode(p.filter_px)));
        sum.r += in.taps[i].r * w;
        sum.g += in.taps[i].g * w;
        sum.b += in.taps[i].b * w;
        wsum += w;
        const Rgb y = RgbToYCoCg(in.taps[i]);
        m1.r += y.r;
        m1.g += y.g;
        m1.b += y.b;
        m2.r += y.r * y.r;
        m2.g += y.g * y.g;
        m2.b += y.b * y.b;
    }
    const Rgb cur{sum.r / wsum, sum.g / wsum, sum.b / wsum};
    if (!in.has_history || in.disoccluded || in.occluder) {
        return {cur, 1.0f};
    }
    const Rgb mean{m1.r / 9.0f, m1.g / 9.0f, m1.b / 9.0f};
    const auto sigma = [](float a, float b) { return std::sqrt(std::max(b - a * a, 0.0f)); };
    const Rgb ext{p.gamma * sigma(mean.r, m2.r / 9.0f) + 1.0f / 512.0f,
                  p.gamma * sigma(mean.g, m2.g / 9.0f) + 1.0f / 512.0f,
                  p.gamma * sigma(mean.b, m2.b / 9.0f) + 1.0f / 512.0f};
    float outside = 0.0f;
    const Rgb hist = YCoCgToRgb(ClipToBox(RgbToYCoCg(in.history), mean, ext, &outside));
    float n = in.n;
    if (outside > 2.0f) {
        n *= 0.5f; // a history far off the neighbourhood: mostly a new surface
    }
    const float alpha = BlendAlpha(n, p.alpha_min);
    return {Blend(cur, hist, alpha, p.luma_k), std::min(n + 1.0f, CountCap(p.alpha_min))};
}

} // namespace VR::RenderTaa
