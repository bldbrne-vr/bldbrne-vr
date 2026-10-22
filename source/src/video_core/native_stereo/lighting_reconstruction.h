// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "asymmetric_projection.h"
#include <algorithm>
#include <cstdint>
#include <span>

namespace NativeProjection {
enum class LightingLayout { None, Light, Occlusion, Cascades, Decal };
inline const char* Name(LightingLayout layout) {
    switch (layout) {
    case LightingLayout::Light: return "light208";
    case LightingLayout::Occlusion: return "ssao272";
    case LightingLayout::Cascades: return "cascades336";
    case LightingLayout::Decal: return "decal416";
    default: return "unrecognized";
    }
}
inline bool Finite(const float* f, unsigned count) {
    return std::all_of(f, f+count, [](float v) { return std::isfinite(v); });
}
// Matrix inversion can produce 1.000000119 at the homogeneous coefficient.
// Keep the structural zeros and complete layout checks; allow only float roundoff.
inline bool InversePerspective(const float* f) {
    if (!Finite(f,16) || !(f[0]>0 && f[5]>0 && std::abs(f[11]-1.f)<1e-5f && f[14]<0 && f[15]>0)) return false;
    for (unsigned i : {1u,2u,3u,4u,6u,7u,8u,9u,10u,12u,13u}) if (f[i]!=0) return false;
    return true;
}
inline bool Viewport(const float* f) {
    return Finite(f,4) && f[0]>100 && f[1]>100 &&
        std::abs(f[0]*f[2]-1)<.001f && std::abs(f[1]*f[3]-1)<.001f;
}
inline bool ViewRotation(const float* f) {
    if (!Finite(f,12)) return false;
    for (unsigned r=0;r<3;++r) for (unsigned s=r;s<3;++s) {
        float dot=0;
        for (unsigned c=0;c<3;++c) dot+=f[4*r+c]*f[4*s+c];
        if (std::abs(dot-(r==s?1.f:0.f))>.002f) return false;
    }
    return true;
}
inline bool AffineView(const float* f) {
    // Captured SSAO during a head turn has homogeneous w=0.99999994.
    // Treat ordinary matrix roundoff as affine, without accepting projective rows.
    return ViewRotation(f) && std::abs(f[12])<1e-5f && std::abs(f[13])<1e-5f &&
        std::abs(f[14])<1e-5f && std::abs(f[15]-1)<1e-5f;
}
inline LightingLayout RecognizeLighting(std::span<const float> f) {
    if (f.size()==104 && InversePerspective(f.data()+16) &&
        Finite(f.data(),16) && AffineView(f.data()+32) && Viewport(f.data()+48))
        return LightingLayout::Decal;
    if (f.size()!=52 && f.size()!=68 && f.size()!=84) return LightingLayout::None;
    if (!InversePerspective(f.data())) return LightingLayout::None;
    if (f.size()==52 && ViewRotation(f.data()+16) && Viewport(f.data()+28) &&
        AffineView(f.data()+32)) return LightingLayout::Light;
    // Captured SSAO: inverse P, half-resolution viewport, 4x4 view, parameters.
    if (f.size()==68 && Viewport(f.data()+16) && AffineView(f.data()+20))
        return LightingLayout::Occlusion;
    if (f.size()==84) {
        // Four directional screen-to-shadow matrices retain inverse P's homogeneous
        // row. Require all four, finite nonzero spatial rows, and ordered split depths.
        for (unsigned block=16;block<80;block+=16) {
            if (!Finite(f.data()+block,16)) return LightingLayout::None;
            for (unsigned c=0;c<4;++c)
                if (std::abs(f[block+12+c]-f[12+c])>.001f) return LightingLayout::None;
            for (unsigned r=0;r<3;++r)
                if (std::abs(f[block+4*r])+std::abs(f[block+4*r+1])==0)
                    return LightingLayout::None;
        }
        if (Finite(f.data()+80,4) && f[80]>0 && f[81]>f[80] && f[82]>f[81])
            return LightingLayout::Cascades;
    }
    return LightingLayout::None;
}
// Row-major 4x4 matrices in double: the temporal SSAO's history reprojection below.
using Matrix4d = std::array<double,16>;
inline Matrix4d MatrixProduct(const Matrix4d& a, const Matrix4d& b) {
    Matrix4d c{};
    for (int r=0; r<4; ++r) for (int k=0; k<4; ++k) for (int col=0; col<4; ++col)
        c[4*r+col] += a[4*r+k]*b[4*k+col];
    return c;
}
// The temporal SSAO (FS 0x83da8467, the Hunter's Dream) blends each pixel with its own eye's AO
// of the last frame (the history image is per eye). It finds that pixel through ssao272's dwords
// 44..59, rows M on (u, v, raw depth, 1) with u right and v down: the last frame's (u, v, raw
// depth) times w; that depth fades out history whose own differs, fully at 2%. The game builds
// M for the flat camera, M = T P R inverse(P) inverse(T): T takes clip x, y to u = (x + 1) / 2,
// v = (1 - y) / 2, P is the projection whose inverse is dwords 0..15 and R the camera's rigid
// move from this frame's view to the last frame's. Each eye got that M, which its own block 0
// (RemapLighting) contradicts, so during head turns and walking each eye fetched its history
// from the wrong place and the AO smeared or lost its filtering. The other programs bound to
// this layout (the plain SSAO 0xcbbca5dc, the AO upsample 0xc45c51cb) never read these dwords.
//
// True when M is such a move under dwords 0..15: block 0 an InversePerspective, M finite, and
// inverse(P) inverse(T) M T P (scaled to a homogeneous 1) a right-handed rotation plus a
// translation. That fails when the last frame had another projection (zoom, near plane) and for
// anything that is not a reprojection: those keep the flat M, as before.
inline bool OcclusionHistoryMove(const float* f) {
    if (!InversePerspective(f) || !Finite(f+44,16)) return false;
    // InversePerspective's rows: (a 0 0 0) (0 b 0 0) (0 0 0 k) (0 0 c w); P is its inverse.
    const double a=f[0], b=f[5], k=f[11], c=f[14], w=f[15];
    const Matrix4d inverse_p{a,0,0,0, 0,b,0,0, 0,0,0,k, 0,0,c,w};
    const Matrix4d p{1/a,0,0,0, 0,1/b,0,0, 0,0,-w/(c*k),1/c, 0,0,1/k,0};
    const Matrix4d to_uv{.5,0,0,.5, 0,-.5,0,.5, 0,0,1,0, 0,0,0,1};
    const Matrix4d to_clip{2,0,0,-1, 0,-2,0,1, 0,0,1,0, 0,0,0,1};
    Matrix4d history{};
    for (int i=0; i<16; ++i) history[i]=f[44+i];
    Matrix4d r=MatrixProduct(MatrixProduct(MatrixProduct(MatrixProduct(inverse_p,to_clip),history),
                                           to_uv),p);
    if (!std::all_of(r.begin(),r.end(),[](double v) { return std::isfinite(v); }) || !(r[15]>0))
        return false;
    const double scale=r[15];
    for (double& v : r) v/=scale;
    for (int i : {12,13,14}) if (std::abs(r[i])>1e-4) return false;
    for (int i=0; i<3; ++i) for (int j=i; j<3; ++j) {
        const double dot=r[4*i]*r[4*j]+r[4*i+1]*r[4*j+1]+r[4*i+2]*r[4*j+2];
        if (std::abs(dot-(i==j ? 1. : 0.))>.002) return false;
    }
    // Orthonormal rows leave a determinant of +-1: a reflection is no camera move.
    return r[0]*(r[5]*r[10]-r[6]*r[9]) - r[1]*(r[4]*r[10]-r[6]*r[8]) +
           r[2]*(r[4]*r[9]-r[5]*r[8]) > 0;
}
// One eye's history reprojection (`m`: dwords 44..59, after OcclusionHistoryMove held), from the
// flat camera's `inverse_projection` (dwords 0..15): E M inverse(E). E takes a flat (u, v, raw
// depth, 1) to the same point's in this eye. The eye sits at `eye` along view x (dword 23's
// shift): with block 0's rows above, view x - eye moves clip x by -eye / a times W = c depth + w.
// Then the eye's frustum `t` (Forward); depth stays. The eye drew its history with the same E
// last frame (same frustum and IPD), so E M inverse(E) takes the eye's pixel to its own history
// for any move R. A still camera's identity stays the identity (to float rounding).
inline void EyeOcclusionHistory(float* m, const float* inverse_projection, float eye,
                                const Transform& t) {
    const double a=inverse_projection[0], c=inverse_projection[14], w=inverse_projection[15];
    const double sx=t.x_scale, sy=t.y_scale, s=eye/a;
    const double u_depth=-.5*sx*s*c, u_one=.5-.5*sx+.5*t.x_offset-.5*sx*s*w;
    const double v_one=.5-.5*sy-.5*t.y_offset;
    const Matrix4d e{sx,0,u_depth,u_one, 0,sy,0,v_one, 0,0,1,0, 0,0,0,1};
    const Matrix4d e_inverse{1/sx,0,-u_depth/sx,-u_one/sx, 0,1/sy,0,-v_one/sy, 0,0,1,0, 0,0,0,1};
    Matrix4d history{};
    for (int i=0; i<16; ++i) history[i]=m[i];
    const Matrix4d result=MatrixProduct(MatrixProduct(e,history),e_inverse);
    for (int i=0; i<16; ++i) m[i]=float(result[i]);
}
// `history` (FAST_GPU_SSAO_HISTORY_EYE; only for Occlusion): the caller found
// OcclusionHistoryMove(source), and dwords 44..59 follow the eye too (EyeOcclusionHistory).
inline void RemapLighting(float* values, const float* source, LightingLayout layout,
                          float eye, const Transform& transform, bool history = false) {
    if (layout==LightingLayout::Decal) {
        EyeReconstruction(values,source+16,eye,transform);
        Inverse(values+16,transform);
        for (unsigned r=0;r<3;++r) values[35+4*r]+=source[32+4*r]*eye;
        return;
    }
    if (layout==LightingLayout::Light) {
        values[19]-=eye;
        for (unsigned r=0;r<3;++r) values[35+4*r]+=source[32+4*r]*eye;
    } else if (layout==LightingLayout::Occlusion) {
        values[23]-=eye;
        if (history) EyeOcclusionHistory(values+44,source,eye,transform);
    } else if (layout==LightingLayout::Cascades) {
        for (unsigned block=16;block<80;block+=16)
            EyeReconstruction(values+block,source,eye,transform);
    } else return;
    Inverse(values,transform);
}
// Remap a guest pixel rectangle with the same viewport and clip transform as geometry.
inline std::array<float,4> RemapScissor(const std::array<float,4>& rect,
                                       const std::array<float,4>& viewport, const Transform& t) {
    const auto x=[&](float v) { return t.x_scale*(v-viewport[0])+viewport[0]+viewport[2]*t.x_offset; };
    const auto y=[&](float v) { return t.y_scale*(v-viewport[1])+viewport[1]+viewport[3]*t.y_offset; };
    return {x(rect[0]),y(rect[1]),x(rect[2]),y(rect[3])};
}
// Captured preprojected light-rectangle VS: Position/Param0 = fetched clip XYW,
// Param1 = separately fetched screen UV. Unlike world meshes these have w == 1.
inline bool LightRectangleShader(std::uint64_t hash) { return hash==0x8747a367; }
// Quads the game places in guest screen coordinates (w == 1) over something in the world, with a
// separately fetched screen UV: the light rectangles (UV in param 1) and the light-glow pass
// (VS 0x637500e0: the glow's depth-tested quad, its blur and its composite; UV in param 0). Their
// position, screen UV and hardware scissor follow each eye like geometry. -1: not such a quad.
inline int ScreenQuadUvParam(std::uint64_t hash) {
    return hash==0x8747a367 ? 1 : hash==0x637500e0 ? 0 : -1;
}
inline bool ScreenQuadShader(std::uint64_t hash) { return ScreenQuadUvParam(hash) >= 0; }
// Radial blurs whose samples step toward the light's position on the flat game's screen, a screen
// UV in two constant dwords of their buffer 0. Both eyes got that one point, so the streaks
// radiated from the wrong place in each eye and swung as the head turned. Each eye now gets the
// light where that eye sees it.
//  - FS 0xd506a1, the light shafts (a full-screen pass): 16 samples per pixel, dwords 28..29.
//  - FS 0xe55e45eb, the light glow's god rays (VS 0x637500e0's quad, whose UV in param 0 follows
//    each eye like ScreenQuadUvParam): from that UV, uv -= dword 62 / dword 66 * (uv - C) per step,
//    C = dwords 68..69 (all four variants).
// The first of the two dwords; -1: not such a shader.
inline int ScreenPointDword(std::uint64_t hash) {
    return hash==0xd506a1 ? 28 : hash==0xe55e45eb ? 68 : -1;
}
inline bool ScreenPointShader(std::uint64_t hash) { return ScreenPointDword(hash) >= 0; }
// A point on the flat game's screen (UV: u right, v down; a direction, the light at infinity: no
// eye shift) where one eye sees it: NDC x' = sx x + ox, y' = sy y + oy.
inline std::array<float,2> RemapScreenPoint(float u, float v, const Transform& t) {
    const float x = 2.f*u - 1.f, y = 1.f - 2.f*v;
    return {.5f*(t.x_scale*x + t.x_offset + 1.f), .5f*(1.f - (t.y_scale*y + t.y_offset))};
}
// A pixel rectangle [x0, x1) x [y0, y1) of a width x height image on the flat game's screen, where
// one eye sees it: the corners as RemapScreenPoint, widened by `parallax` pixels on the side the
// eye's shift moves near things to (the left eye: right). Rounded outward, clamped to the image.
inline std::array<std::uint32_t,4> RemapScreenRect(const std::array<std::uint32_t,4>& r,
                                                  std::uint32_t width, std::uint32_t height,
                                                  const Transform& t, bool left_eye, float parallax) {
    const float w = float(width), h = float(height);
    const auto a = RemapScreenPoint(float(r[0])/w, float(r[1])/h, t);
    const auto b = RemapScreenPoint(float(r[2])/w, float(r[3])/h, t);
    const auto edge = [](float v, float hi) { return std::uint32_t(std::clamp(v, 0.f, hi)); };
    return {edge(std::floor(a[0]*w - (left_eye ? 0.f : parallax)), w), edge(std::floor(a[1]*h), h),
            edge(std::ceil(b[0]*w + (left_eye ? parallax : 0.f)), w), edge(std::ceil(b[1]*h), h)};
}
// FAST_GPU_LIGHT_EYE. The deferred point and spot lights: fragment programs over the light
// rectangles (LightRectangleShader) with two buffers, the light208 camera (RecognizeLighting's
// Light) and a light list of 144 bytes per light. A program rebuilds the pixel's view-space
// position P through light208 (once paired, each eye's own: that eye's depth and inverse
// projection) and lights it from the list, whose lights are in the flat camera's view space:
// L = list - P. Each eye's camera is the flat one moved e along its view x (CameraBuffer:
// f[11] -= e, light208: dword 19), so its P is the flat P minus (e, 0, 0); with the flat list every
// light moved with each eye, and its pool and highlights had no stereo depth. The list moved by the
// same -e gives each eye the flat game's light vector. A list's layout, by program (what the
// programs read):
//  - Point: 16 dwords per light, the position at 0..2.
//  - Spot: 32 dwords per light, the position at 0..2 and the rows of the view-to-spot projection
//    at 16..31 (each applied to (P, 1)).
//  - CubeShadow: a point light with a cube shadow map, 24 dwords per light, the position at 0..2
//    and the cube's origin at 20..22 (its faces along the view axes; the lookup is origin - P).
// Nothing else in a record is in view space (colours, ranges, depth parameters, flags), and no
// projection takes part: the asymmetric remap and a reflection pass's flip leave the shift as is.
enum class LightKind : std::uint8_t { None, Point, Spot, CubeShadow };
inline const char* Name(LightKind kind) {
    switch (kind) {
    case LightKind::Point: return "point";
    case LightKind::Spot: return "spot";
    case LightKind::CubeShadow: return "cube-shadowed point";
    default: return "none";
    }
}
struct LightList {
    static constexpr unsigned RecordBytes = 144; // the list's bytes per light, any kind
    static constexpr unsigned MaxCount = 12;
    static constexpr unsigned MaxBytes = RecordBytes * MaxCount;
    LightKind kind{};
    std::uint8_t count{};
    constexpr unsigned Stride() const {
        return kind == LightKind::Point ? 16 : kind == LightKind::Spot ? 32 :
               kind == LightKind::CubeShadow ? 24 : 0;
    }
    constexpr unsigned Dwords() const { return Stride() * count; }
    constexpr unsigned Bytes() const { return kind == LightKind::None ? 0 : RecordBytes * count; }
    constexpr bool operator==(const LightList&) const = default;
};
// The deferred-light programs (every variant of the captured scenes; VS 0x8747a367) and their
// lists. Any other program keeps the flat list.
inline LightList LightListLayout(std::uint64_t hash) {
    using K = LightKind;
    switch (hash) {
    case 0x370174f7: case 0x7cc2c044: return {K::Point, 1};
    case 0x186d69d5: case 0x600f86b0: return {K::Point, 2};
    case 0x89f9ee3e: case 0xd22cbfd1: return {K::Point, 3};
    case 0x149432ff: case 0x3105494c: return {K::Point, 4};
    case 0x5de04266: case 0xd75fe2ff: return {K::Point, 5};
    case 0x5d7150da: case 0x07d07cf3: return {K::Point, 6};
    case 0x8d5387d9: case 0xf2531727: return {K::Point, 7};
    case 0xc6edb19a: case 0xf81596cd: return {K::Point, 8};
    case 0x0b2ace4c: case 0xb59b4c4e: return {K::Point, 9};
    case 0x201cb43d: case 0x505ebdbe: return {K::Point, 10};
    case 0x22b357f3: case 0x5f76eb3d: return {K::Point, 11};
    case 0x799bfa32: case 0x9f9ef267: return {K::Point, 12};
    case 0xbae5454e: return {K::Spot, 1};
    case 0x896e7242: return {K::Spot, 2};
    case 0x02cd8f9b: return {K::Spot, 3};
    case 0x8ea5be99: return {K::Spot, 4};
    case 0xe96447c3: case 0xf40da260: return {K::CubeShadow, 1};
    case 0xa22c7f71: case 0xb5b52d63: return {K::CubeShadow, 2};
    case 0x6b42fc89: case 0xbb2ac1cb: return {K::CubeShadow, 3};
    case 0x0ef50047: return {K::CubeShadow, 4};
    default: return {};
    }
}
// LightListLayout for a buffer of exactly its list's size (as every captured draw binds); a buffer
// of any other size is not taken for it.
inline LightList LightListFor(std::uint64_t hash, std::uint64_t bytes) {
    const LightList list = LightListLayout(hash);
    return list.kind != LightKind::None && bytes == list.Bytes() ? list : LightList{};
}
// `values`: a copy of `source` (the list's Bytes()). Each light's view-space quantities are moved
// into the eye `eye` (CameraBuffer's e) along view x: a position x -= eye; a spot row applied to
// the flat P = the eye's P + (eye, 0, 0): row[3] += row[0] * eye. Only those dwords change. False,
// with `values` partly written, when `eye` or one of those dwords comes out not finite (a NaN or
// infinity in a dword they are made from does).
inline bool RemapLightList(float* values, const float* source, LightList list, float eye) {
    const unsigned stride = list.Stride();
    if (!stride || list.count > LightList::MaxCount || !std::isfinite(eye)) return false;
    for (unsigned i = 0; i < list.count; ++i) {
        const float* s = source + stride * i;
        float* v = values + stride * i;
        v[0] = s[0] - eye;
        if (!std::isfinite(v[0])) return false;
        if (list.kind == LightKind::Spot) {
            for (unsigned r = 19; r < 32; r += 4) {
                v[r] = s[r] + s[r - 3] * eye;
                if (!std::isfinite(v[r])) return false;
            }
        } else if (list.kind == LightKind::CubeShadow) {
            v[20] = s[20] - eye;
            if (!std::isfinite(v[20])) return false;
        }
    }
    return true;
}
} // namespace NativeProjection
