// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// VrMoonShadow (ir/passes/moon_shadow.h, MoonShadowPass): the moon's share of this pixel, in
// place of the game's 4 atlas taps. In the entry block (uniform, derivatives defined): the
// pixel's view position P and cascade c as the game picks them, its atlas coordinates and depth
// through cascade c's screen-to-shadow matrix S, S's view-space part L = S M^-1 (M: the
// ndc-to-view matrix) for the moon's direction and the atlas's scale (texels and depth per
// metre), the receiver plane's depth slope across the atlas, per-pixel and per-frame noise.
// Then, only where it can matter:
//  1. blocker search: the pixel's 4 texels and BlockerRing more gathers on a rotated spiral out
//     to the widest penumbra; none in front of the pixel: lit; all of them: in shadow;
//  2. otherwise the penumbra: the blockers' mean height above the pixel times the moon's size,
//     filtered with FilterNear / FilterMid / FilterFar hardware compares (by the penumbra's
//     width) on another rotated spiral (contact hardening);
//  3. contact shadows where the moon still reaches the pixel: ContactSteps points towards it,
//     each against the eye's depth.
// The tuning is LanternEngine's (moon_shadow.h).
// Loop-free (every tap unrolled); every selection's condition comes before its merge.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <utility>

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/emit_spirv_light_common.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/passes/moon_shadow.h"

namespace Shader::Backend::SPIRV {
namespace {

namespace Tune = Shader::Optimization::MoonShadow;

using Mat4 = std::array<std::array<Id, 4>, 4>;

struct MoonMath {
    EmitContext& ctx;

    Id F(float v) const {
        return ctx.ConstF32(v);
    }
    Id Mul(Id a, Id b) const {
        return ctx.OpFMul(ctx.F32[1], a, b);
    }
    Id Add(Id a, Id b) const {
        return ctx.OpFAdd(ctx.F32[1], a, b);
    }
    Id Sub(Id a, Id b) const {
        return ctx.OpFSub(ctx.F32[1], a, b);
    }
    Id Div(Id a, Id b) const {
        return ctx.OpFDiv(ctx.F32[1], a, b);
    }
    Id Max(Id a, Id b) const {
        return ctx.OpFMax(ctx.F32[1], a, b);
    }
    Id Min(Id a, Id b) const {
        return ctx.OpFMin(ctx.F32[1], a, b);
    }
    Id Clamp(Id x, float lo, float hi) const {
        return ctx.OpFClamp(ctx.F32[1], x, F(lo), F(hi));
    }
    Id X(Id v, u32 i) const {
        return ctx.OpCompositeExtract(ctx.F32[1], v, i);
    }
    Id Vec2(Id a, Id b) const {
        return ctx.OpCompositeConstruct(ctx.F32[2], std::array{a, b});
    }
    Id Vec3(Id a, Id b, Id c) const {
        return ctx.OpCompositeConstruct(ctx.F32[3], std::array{a, b, c});
    }
    Id Vec4(Id a, Id b, Id c, Id d) const {
        return ctx.OpCompositeConstruct(ctx.F32[4], std::array{a, b, c, d});
    }
    Id Dot(Id a, Id b) const {
        return ctx.OpDot(ctx.F32[1], a, b);
    }
    Id Gt(Id a, Id b) const {
        return ctx.OpFOrdGreaterThan(ctx.U1[1], a, b);
    }
    Id Lt(Id a, Id b) const {
        return ctx.OpFOrdLessThan(ctx.U1[1], a, b);
    }
    Id And(Id a, Id b) const {
        return ctx.OpLogicalAnd(ctx.U1[1], a, b);
    }
    // Hermite smoothstep(e0, e1, x).
    Id SmoothStep(float e0, float e1, Id x) const {
        const Id t = Clamp(Mul(Sub(x, F(e0)), F(1.0f / (e1 - e0))), 0.0f, 1.0f);
        return Mul(Mul(t, t), Sub(F(3.0f), Mul(F(2.0f), t)));
    }
};

Id MoonRead(EmitContext& ctx, u32 handle, Id index) {
    return ctx.OpBitcast(ctx.F32[1], EmitReadConstBuffer(ctx, handle, index));
}

Id MoonPrivate(EmitContext& ctx, Id type) {
    const Id var = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Private, type),
                                         spv::StorageClass::Private);
    ctx.interfaces.push_back(var);
    return var;
}

Id MoonImage(EmitContext& ctx, u32 handle) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    return ctx.NativeNonUniform(
        ctx.OpLoad(texture.image_type, ctx.NativeImagePointer(texture.image_type, texture.id)));
}

// The 4 raw texels round `coords` (an image | sampler handle with a plain sampler).
Id MoonGather(EmitContext& ctx, u32 handle, Id coords) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = MoonImage(ctx, handle);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled =
        ctx.NativeNonUniform(ctx.OpSampledImage(texture.sampled_type, image, sampler));
    const Id texels = ctx.OpImageGather(texture.data_types->Get(4), sampled, coords,
                                        ctx.ConstU32(0u));
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texels) : texels;
}

// The inverse of m (the adjugate's transpose over the determinant).
Mat4 Inverse(const MoonMath& o, const Mat4& m) {
    EmitContext& ctx = o.ctx;
    Mat4 cof;
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            std::array<u32, 3> rr{}, cc{};
            for (u32 i = 0, n = 0; i < 4; ++i) {
                if (i != r) {
                    rr[n++] = i;
                }
            }
            for (u32 i = 0, n = 0; i < 4; ++i) {
                if (i != c) {
                    cc[n++] = i;
                }
            }
            const auto e = [&](u32 i, u32 j) { return m[rr[i]][cc[j]]; };
            const Id d3 = o.Add(
                o.Sub(o.Mul(e(0, 0), o.Sub(o.Mul(e(1, 1), e(2, 2)), o.Mul(e(1, 2), e(2, 1)))),
                      o.Mul(e(0, 1), o.Sub(o.Mul(e(1, 0), e(2, 2)), o.Mul(e(1, 2), e(2, 0))))),
                o.Mul(e(0, 2), o.Sub(o.Mul(e(1, 0), e(2, 1)), o.Mul(e(1, 1), e(2, 0)))));
            cof[r][c] = ((r + c) & 1) ? ctx.OpFNegate(ctx.F32[1], d3) : d3;
        }
    }
    Id det = o.F(0.0f);
    for (u32 c = 0; c < 4; ++c) {
        det = o.Add(det, o.Mul(m[0][c], cof[0][c]));
    }
    const Id inv_det = o.Div(o.F(1.0f), det);
    Mat4 inv;
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            inv[r][c] = o.Mul(cof[c][r], inv_det);
        }
    }
    return inv;
}

// Point i of an n-point spiral in the unit disc (the golden angle apart, radius by its share of
// the area), as constants.
std::array<float, 2> Spiral(u32 i, u32 n, float turn) {
    const float radius = std::sqrt((float(i) + 0.5f) / float(n));
    const float angle = float(i) * 2.39996323f + turn;
    return {radius * std::cos(angle), radius * std::sin(angle)};
}

} // namespace

Id EmitVrMoonShadow(EmitContext& ctx, Id ndc, Id uv, Id depth) {
    const auto& mi = ctx.info.vr_moon_shadow;
    if (!mi.on) {
        return ctx.ConstF32(1.0f);
    }
    const MoonMath o{ctx};
    const Id zero = o.F(0.0f), one = o.F(1.0f);
    const float sigma = mi.reversed ? -1.0f : 1.0f;
    // The moon's apparent radius (moon_shadow.h LightTan; VR_MOON_LIGHT_TAN overrides it, tests).
    static const float light_tan = [] {
        const char* const env = std::getenv("VR_MOON_LIGHT_TAN");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 0.05f)
                              : Tune::LightTan;
    }();
    const auto dword = [&](u32 d) { return MoonRead(ctx, mi.cascades_buffer, ctx.ConstU32(d)); };

    // ---- Entry: uniform control flow ----
    // M: the eye's ndc (x, y, depth, 1) to view (x, y, z) w; its inverse projects back.
    Mat4 m;
    std::array<Id, 4> rows;
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            m[r][c] = dword(4 * r + c);
        }
        rows[r] = o.Vec4(m[r][0], m[r][1], m[r][2], m[r][3]);
    }
    const Id nx = o.X(ndc, 0), ny = o.X(ndc, 1);
    const Id v = o.Vec4(nx, ny, depth, one);
    const Id w = o.Dot(rows[3], v);
    const Id inv_w = o.Div(one, w);
    const Id pz = o.Mul(o.Dot(rows[2], v), inv_w);
    const Id p = o.Vec3(o.Mul(o.Dot(rows[0], v), inv_w), o.Mul(o.Dot(rows[1], v), inv_w), pz);
    const Id dist = ctx.OpLength(ctx.F32[1], p);
    // Every screen derivative, before the first branch: the view position's change across the
    // pixel (the receiver plane below) and the eye's depth coordinates from ndc, uv = a ndc + b
    // (each axis its own; the contact march).
    const Id dpx = ctx.OpDPdx(ctx.F32[3], p), dpy = ctx.OpDPdy(ctx.F32[3], p);
    const Id ux = o.X(uv, 0), uy = o.X(uv, 1);
    const Id dnx = ctx.OpDPdx(ctx.F32[1], nx), dny = ctx.OpDPdy(ctx.F32[1], ny);
    const Id ax = o.Div(ctx.OpDPdx(ctx.F32[1], ux), dnx);
    const Id ay = o.Div(ctx.OpDPdy(ctx.F32[1], uy), dny);
    const Id bx = o.Sub(ux, o.Mul(ax, nx)), by = o.Sub(uy, o.Mul(ay, ny));
    const Id screen_ok = o.And(o.Gt(ctx.OpFAbs(ctx.F32[1], dnx), o.F(1e-9f)),
                               o.Gt(ctx.OpFAbs(ctx.F32[1], dny), o.F(1e-9f)));
    // if (cond) { body }: the condition is computed before the merge (Sirit emits in call order).
    const auto when = [&](Id cond, auto&& body) {
        const Id then_label = ctx.OpLabel(), endif = ctx.OpLabel();
        ctx.OpSelectionMerge(endif, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(cond, then_label, endif);
        ctx.AddLabel(then_label);
        body();
        ctx.OpBranch(endif);
        ctx.AddLabel(endif);
    };
    // M^-1. The game's M and every eye's (EyeReconstruction keeps the shape) are a perspective
    // inverse, (a 0 0 b)(0 c 0 d)(0 0 0 k)(0 0 e f), whose inverse is a dozen operations; the full
    // cofactor inverse only for a matrix of any other shape (the same way for the whole draw).
    std::array<Id, 4> inv_var;
    for (Id& var : inv_var) {
        var = MoonPrivate(ctx, ctx.F32[4]);
    }
    const auto is_zero = [&](Id x) { // +0 or -0, by the bits
        return ctx.OpIEqual(ctx.U1[1],
                            ctx.OpBitwiseAnd(ctx.U32[1], ctx.OpBitcast(ctx.U32[1], x),
                                             ctx.ConstU32(0x7fffffffu)),
                            ctx.u32_zero_value);
    };
    Id sparse = is_zero(m[0][1]);
    for (const auto& [r, c] : {std::pair{0u, 2u}, std::pair{1u, 0u}, std::pair{1u, 2u},
                               std::pair{2u, 0u}, std::pair{2u, 1u}, std::pair{2u, 2u},
                               std::pair{3u, 0u}, std::pair{3u, 1u}}) {
        sparse = o.And(sparse, is_zero(m[r][c]));
    }
    when(sparse, [&] {
        const Id ia = o.Div(one, m[0][0]), ic = o.Div(one, m[1][1]);
        const Id ik = o.Div(one, m[2][3]), ie = o.Div(one, m[3][2]);
        const auto neg = [&](Id x) { return ctx.OpFNegate(ctx.F32[1], x); };
        ctx.OpStore(inv_var[0], o.Vec4(ia, zero, neg(o.Mul(o.Mul(m[0][3], ia), ik)), zero));
        ctx.OpStore(inv_var[1], o.Vec4(zero, ic, neg(o.Mul(o.Mul(m[1][3], ic), ik)), zero));
        ctx.OpStore(inv_var[2], o.Vec4(zero, zero, neg(o.Mul(o.Mul(m[3][3], ie), ik)), ie));
        ctx.OpStore(inv_var[3], o.Vec4(zero, zero, ik, zero));
    });
    when(ctx.OpLogicalNot(ctx.U1[1], sparse), [&] {
        const Mat4 full = Inverse(o, m);
        for (u32 r = 0; r < 4; ++r) {
            ctx.OpStore(inv_var[r], o.Vec4(full[r][0], full[r][1], full[r][2], full[r][3]));
        }
    });
    std::array<Id, 4> inv_rows;
    Mat4 inv;
    for (u32 r = 0; r < 4; ++r) {
        inv_rows[r] = ctx.OpLoad(ctx.F32[4], inv_var[r]);
        for (u32 c = 0; c < 4; ++c) {
            inv[r][c] = o.X(inv_rows[r], c);
        }
    }
    // Noise per pixel and per frame: the pixel's position hashed with its depth's bits (they move
    // every frame with the upscaler's sub-pixel jitter; an upscaler is required).
    const Id fx = EmitGetAttribute(ctx, IR::Attribute::FragCoord, 0, 0);
    const Id fy = EmitGetAttribute(ctx, IR::Attribute::FragCoord, 1, 0);
    const auto u32c = [&](u32 x) { return ctx.ConstU32(x); };
    const auto hash = [&](Id h) { // lowbias32
        h = ctx.OpBitwiseXor(ctx.U32[1], h, ctx.OpShiftRightLogical(ctx.U32[1], h, u32c(16)));
        h = ctx.OpIMul(ctx.U32[1], h, u32c(0x7feb352du));
        h = ctx.OpBitwiseXor(ctx.U32[1], h, ctx.OpShiftRightLogical(ctx.U32[1], h, u32c(15)));
        h = ctx.OpIMul(ctx.U32[1], h, u32c(0x846ca68bu));
        return ctx.OpBitwiseXor(ctx.U32[1], h, ctx.OpShiftRightLogical(ctx.U32[1], h, u32c(16)));
    };
    const auto unit = [&](Id h) { // [0, 1)
        return o.Mul(ctx.OpConvertUToF(ctx.F32[1], ctx.OpShiftRightLogical(ctx.U32[1], h, u32c(8))),
                     o.F(1.0f / 16777216.0f));
    };
    Id h = ctx.OpBitwiseXor(
        ctx.U32[1], ctx.OpBitcast(ctx.U32[1], depth),
        ctx.OpBitwiseXor(ctx.U32[1],
                         ctx.OpIMul(ctx.U32[1], ctx.OpConvertFToU(ctx.U32[1], fx), u32c(1973u)),
                         ctx.OpIMul(ctx.U32[1], ctx.OpConvertFToU(ctx.U32[1], fy), u32c(9277u))));
    h = hash(ctx.OpBitwiseXor(ctx.U32[1], h, u32c(0x6d2b79f5u)));
    // An atlas texel.
    const Id size = ctx.OpImageQuerySizeLod(ctx.U32[2], MoonImage(ctx, mi.shadow_compare),
                                            ctx.u32_zero_value);
    const Id texel = o.Div(one, o.Max(ctx.OpConvertUToF(ctx.F32[1],
                                                        ctx.OpCompositeExtract(ctx.U32[1], size, 0u)),
                                      one));
    // The cascade: one more past each split (the game's own choice), and its matrix S.
    const Id split80 = dword(80), split81 = dword(81), split82 = dword(82);
    const auto past = [&](Id split) {
        return ctx.OpSelect(ctx.U32[1], o.Gt(pz, split), ctx.ConstU32(1u), ctx.u32_zero_value);
    };
    const Id game_cascade = ctx.OpIAdd(
        ctx.U32[1], ctx.OpIAdd(ctx.U32[1], past(split80), past(split81)), past(split82));
    const auto cascade_rows = [&](Id k) {
        const Id base = ctx.OpIAdd(ctx.U32[1], ctx.ConstU32(16u),
                                   ctx.OpIMul(ctx.U32[1], k, ctx.ConstU32(16u)));
        Mat4 sm;
        for (u32 r = 0; r < 4; ++r) {
            for (u32 c = 0; c < 4; ++c) {
                sm[r][c] = MoonRead(ctx, mi.cascades_buffer,
                                    ctx.OpIAdd(ctx.U32[1], base, ctx.ConstU32(4 * r + c)));
            }
        }
        return sm;
    };
    Id cascade = game_cascade;
    // Seamless cascades (moon_shadow.h CascadeBlendShare): near a split, the next cascade by
    // chance, where it holds the pixel.
    static const float blend_share = [] {
        const char* const env = std::getenv("VR_MOON_BLEND");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 0.5f)
                              : Tune::CascadeBlendShare;
    }();
    if (blend_share > 0.0f) {
        const auto is = [&](u32 k) {
            return ctx.OpIEqual(ctx.U1[1], game_cascade, ctx.ConstU32(k));
        };
        const Id split = ctx.OpSelect(ctx.F32[1], is(0), split80,
                                      ctx.OpSelect(ctx.F32[1], is(1), split81, split82));
        const Id previous = ctx.OpSelect(ctx.F32[1], is(0), zero,
                                         ctx.OpSelect(ctx.F32[1], is(1), split80, split81));
        const Id band =
            FiniteClamp(ctx, o.Mul(o.F(blend_share), o.Sub(split, previous)), 0.0f, 1.0e4f);
        const Id band_ok = o.Gt(band, o.F(1e-4f));
        const Id t = FiniteClamp(ctx,
                                 o.Div(o.Sub(pz, o.Sub(split, band)),
                                       ctx.OpSelect(ctx.F32[1], band_ok, band, one)),
                                 0.0f, 1.0f);
        const Id chance = o.Mul(o.Mul(t, t), o.Sub(o.F(3.0f), o.Mul(o.F(2.0f), t)));
        const Id xi = unit(hash(ctx.OpBitwiseXor(ctx.U32[1], h, u32c(0x632be5abu))));
        const Id next = ctx.OpUMin(ctx.U32[1], ctx.OpIAdd(ctx.U32[1], game_cascade, u32c(1u)),
                                   u32c(3u));
        const Mat4 sn = cascade_rows(next);
        std::array<Id, 4> nrows;
        for (u32 r = 0; r < 4; ++r) {
            nrows[r] = o.Vec4(sn[r][0], sn[r][1], sn[r][2], sn[r][3]);
        }
        const Id inv_wn = o.Div(one, o.Dot(nrows[3], v));
        // Not a number or out of reach: 0, on a quarter's edge or the near plane, refused below.
        const Id un = FiniteClamp(ctx, o.Mul(o.Dot(nrows[0], v), inv_wn), -1.0f, 2.0f);
        const Id vn = FiniteClamp(ctx, o.Mul(o.Dot(nrows[1], v), inv_wn), -1.0f, 2.0f);
        const Id rn = FiniteClamp(ctx, o.Mul(o.Dot(nrows[2], v), inv_wn), -1.0f, 2.0f);
        const Id qx = o.Mul(ctx.OpConvertUToF(ctx.F32[1],
                                              ctx.OpBitwiseAnd(ctx.U32[1], next, u32c(1u))),
                            o.F(0.5f));
        const Id qy = o.Mul(ctx.OpConvertUToF(ctx.F32[1],
                                              ctx.OpShiftRightLogical(ctx.U32[1], next, u32c(1u))),
                            o.F(0.5f));
        const Id margin = o.Mul(o.F(Tune::CascadeMarginTexels), texel);
        const Id holds = o.And(
            o.And(o.And(o.Gt(un, o.Add(qx, margin)), o.Lt(un, o.Sub(o.Add(qx, o.F(0.5f)), margin))),
                  o.And(o.Gt(vn, o.Add(qy, margin)), o.Lt(vn, o.Sub(o.Add(qy, o.F(0.5f)), margin)))),
            o.And(o.Gt(rn, zero), o.Lt(rn, one)));
        const Id take =
            o.And(o.And(ctx.OpULessThan(ctx.U1[1], game_cascade, u32c(3u)), band_ok),
                  o.And(o.Lt(xi, chance), holds));
        cascade = ctx.OpSelect(ctx.U32[1], take, next, game_cascade);
    }
    const Mat4 s = cascade_rows(cascade);
    std::array<Id, 4> srows;
    for (u32 r = 0; r < 4; ++r) {
        srows[r] = o.Vec4(s[r][0], s[r][1], s[r][2], s[r][3]);
    }
    const Id inv_ws = o.Div(one, o.Dot(srows[3], v));
    const Id su = o.Mul(o.Dot(srows[0], v), inv_ws);
    const Id sv = o.Mul(o.Dot(srows[1], v), inv_ws);
    const Id sref = o.Mul(o.Dot(srows[2], v), inv_ws);
    const Id here = o.Vec2(su, sv);
    // L = S M^-1 (view to atlas u, v, depth; its first three columns).
    std::array<Id, 3> l;
    for (u32 i = 0; i < 3; ++i) {
        std::array<Id, 3> e;
        for (u32 j = 0; j < 3; ++j) {
            Id sum = o.Mul(s[i][0], inv[0][j]);
            for (u32 k = 1; k < 4; ++k) {
                sum = o.Add(sum, o.Mul(s[i][k], inv[k][j]));
            }
            e[j] = sum;
        }
        l[i] = o.Vec3(e[0], e[1], e[2]);
    }
    // The moon: along neither u nor v, towards a depth nearer it.
    const Id axis = ctx.OpNormalize(ctx.F32[3], ctx.OpCross(ctx.F32[3], l[0], l[1]));
    const Id along = o.Dot(axis, l[2]);
    const Id away = mi.reversed ? o.Lt(along, zero) : o.Gt(along, zero);
    const Id moon = ctx.OpSelect(ctx.F32[3], away, ctx.OpFNegate(ctx.F32[3], axis), axis);
    const Id uv_per_m = o.Max(ctx.OpLength(ctx.F32[1], l[0]), o.F(1e-7f));
    const Id depth_per_m = o.Max(ctx.OpLength(ctx.F32[1], l[2]), o.F(1e-12f));
    // The cascade's quarter of the atlas (2 x 2): every tap stays on its own cascade's map.
    const Id half2 = ctx.ConstF32(0.5f, 0.5f);
    const Id quarter = ctx.OpFMul(
        ctx.F32[2], ctx.OpFloor(ctx.F32[2], ctx.OpFMul(ctx.F32[2], here, ctx.ConstF32(2.0f, 2.0f))),
        half2);
    const Id texel2 = o.Vec2(texel, texel);
    const Id quarter_lo = ctx.OpFAdd(ctx.F32[2], quarter, texel2);
    const Id quarter_hi = ctx.OpFSub(ctx.F32[2], ctx.OpFAdd(ctx.F32[2], quarter, half2), texel2);
    const auto keep = [&](Id at) {
        return ctx.OpFClamp(ctx.F32[2], ctx.OpFAdd(ctx.F32[2], here, at), quarter_lo, quarter_hi);
    };
    // The receiver plane: depth change per atlas u and v (the view position's change across the
    // pixel through this pixel's own L, also where the quad's other pixels took another cascade;
    // the steepest MaxSlope, none where they are undefined).
    const Id dux = o.Dot(l[0], dpx), duy = o.Dot(l[0], dpy);
    const Id dvx = o.Dot(l[1], dpx), dvy = o.Dot(l[1], dpy);
    const Id drx = o.Dot(l[2], dpx), dry = o.Dot(l[2], dpy);
    const Id det = o.Sub(o.Mul(dux, dvy), o.Mul(dvx, duy));
    const Id det_ok = o.Gt(ctx.OpFAbs(ctx.F32[1], det), o.F(1e-30f));
    const Id inv_det = o.Div(one, ctx.OpSelect(ctx.F32[1], det_ok, det, one));
    const Id gu = o.Mul(o.Sub(o.Mul(drx, dvy), o.Mul(dvx, dry)), inv_det);
    const Id gv = o.Mul(o.Sub(o.Mul(dux, dry), o.Mul(drx, duy)), inv_det);
    const Id g_len = ctx.OpLength(ctx.F32[1], o.Vec2(gu, gv));
    const Id g_max = o.Mul(o.F(Tune::MaxSlope), o.Div(depth_per_m, uv_per_m));
    const Id g_keep = ctx.OpSelect(ctx.F32[1], det_ok,
                                   o.Min(one, o.Div(g_max, o.Max(g_len, o.F(1e-30f)))), zero);
    const Id slope = ctx.OpVectorTimesScalar(ctx.F32[2], o.Vec2(gu, gv), g_keep);
    const Id turn = o.Mul(unit(h), o.F(6.28318531f));
    h = hash(ctx.OpIAdd(ctx.U32[1], h, u32c(0x9e3779b9u)));
    const Id march_seed = unit(h);
    const Id cos_t = ctx.OpCos(ctx.F32[1], turn), sin_t = ctx.OpSin(ctx.F32[1], turn);
    // A spiral point (constants) turned by the noise and scaled by `radius`.
    const auto offset = [&](std::array<float, 2> d, Id radius) {
        const Id dx = o.F(d[0]), dy = o.F(d[1]);
        return ctx.OpVectorTimesScalar(
            ctx.F32[2],
            o.Vec2(o.Sub(o.Mul(cos_t, dx), o.Mul(sin_t, dy)), o.Add(o.Mul(sin_t, dx), o.Mul(cos_t, dy))),
            radius);
    };
    // Where it can matter: a surface in front of the eye, on the atlas, inside the game's fade.
    Id active = o.And(o.Gt(w, zero), o.Gt(dist, o.F(0.01f)));
    active = o.And(active, ctx.OpAll(ctx.U1[1], ctx.OpFOrdGreaterThanEqual(ctx.U1[2], here,
                                                                          ctx.ConstF32(0.0f, 0.0f))));
    active = o.And(active, ctx.OpAll(ctx.U1[1], ctx.OpFOrdLessThanEqual(ctx.U1[2], here,
                                                                       ctx.ConstF32(1.0f, 1.0f))));
    if (mi.params_buffer != 0xFFFFFFFFu) {
        active = o.And(active,
                       o.Lt(dist, MoonRead(ctx, mi.params_buffer, ctx.ConstU32(72u))));
    }
    const Id vis = MoonPrivate(ctx, ctx.F32[1]);
    ctx.OpStore(vis, one);
    const Id search_uv =
        o.Mul(o.Clamp(o.Div(o.Mul(o.F(Tune::MaxPenumbraM), uv_per_m), texel),
                      Tune::MinSearchTexels, Tune::MaxSearchTexels),
              texel);
    // A texel counts as a blocker when it is nearer the moon than the receiver plane there by
    // more than the bias (and the plane's own change across a gather's texels).
    // (BiasM, but at most half an atlas texel's width in metres.)
    const Id bias_m = o.Min(o.F(Tune::BiasM), o.Mul(o.F(0.5f), o.Div(texel, uv_per_m)));
    const Id bias = o.Add(o.Mul(bias_m, depth_per_m), o.Mul(g_len, o.Mul(g_keep, texel)));
    when(active, [&] {
        // ---- 1. Blocker search ----
        const Id one4 = ctx.ConstF32(1.0f, 1.0f, 1.0f, 1.0f);
        const Id zero4 = ctx.ConstF32(0.0f, 0.0f, 0.0f, 0.0f);
        constexpr u32 Gathers = Tune::BlockerRing + 1;
        Id found = zero4, found_depth = zero4;
        for (u32 i = 0; i < Gathers; ++i) {
            const Id at = i == 0 ? ctx.ConstF32(0.0f, 0.0f)
                                 : offset(Spiral(i - 1, Tune::BlockerRing, 0.0f), search_uv);
            const Id texels = MoonGather(ctx, mi.shadow_raw, keep(at));
            const Id expected = o.Add(o.Add(sref, o.Dot(slope, at)), o.Mul(o.F(-sigma), bias));
            const Id expected4 = o.Vec4(expected, expected, expected, expected);
            const Id blocks = mi.reversed ? ctx.OpFOrdGreaterThan(ctx.U1[4], texels, expected4)
                                          : ctx.OpFOrdLessThan(ctx.U1[4], texels, expected4);
            found = ctx.OpFAdd(ctx.F32[4], found, ctx.OpSelect(ctx.F32[4], blocks, one4, zero4));
            found_depth = ctx.OpFAdd(ctx.F32[4], found_depth,
                                     ctx.OpSelect(ctx.F32[4], blocks, texels, zero4));
        }
        const Id blockers = o.Dot(found, one4);
        const Id mean = o.Div(o.Dot(found_depth, one4), o.Max(blockers, one));
        // The penumbra (atlas units): the blockers' mean height above the pixel (metres) times
        // the moon's size, at least the sharpest filter, at most the search.
        const Id height = o.Div(o.Max(o.Mul(o.F(sigma), o.Sub(sref, mean)), zero), depth_per_m);
        const Id radius =
            ctx.OpFClamp(ctx.F32[1], o.Mul(o.Mul(height, o.F(light_tan)), uv_per_m),
                         o.Mul(o.F(Tune::MinFilterTexels), texel), search_uv);
        const Id any = o.Gt(blockers, o.F(0.5f));
        const Id partial = o.Lt(blockers, o.F(float(4 * Gathers) - 0.5f));
        when(any, [&] {
            // Every texel round it in front: deep in the shadow.
            ctx.OpStore(vis, zero);
            // ---- 2. The penumbra's filter: more taps the wider it is (LanternEngine's) ----
            const Id tap_bias = o.Mul(o.F(-sigma * 0.5f), o.Mul(bias_m, depth_per_m));
            const Id radius_texels = o.Div(radius, texel);
            const auto filter = [&](u32 taps) {
                Id lit = zero;
                for (u32 i = 0; i < taps; ++i) {
                    const Id at = offset(Spiral(i, taps, 1.3f), radius);
                    const Id dref = o.Add(o.Add(sref, o.Dot(slope, at)), tap_bias);
                    const Id tap = EmitImageSampleDrefExplicitLod(
                        ctx, nullptr, mi.shadow_compare, keep(at), dref, zero, IR::Value{});
                    lit = o.Add(lit, o.X(tap, 0));
                }
                ctx.OpStore(vis, o.Mul(lit, o.F(1.0f / float(taps))));
            };
            const Id near_filter =
                o.And(partial, ctx.OpFOrdLessThanEqual(ctx.U1[1], radius_texels,
                                                       o.F(Tune::FilterNearTexels)));
            const Id mid_filter = o.And(
                partial, o.And(o.Gt(radius_texels, o.F(Tune::FilterNearTexels)),
                               ctx.OpFOrdLessThanEqual(ctx.U1[1], radius_texels,
                                                       o.F(Tune::FilterMidTexels))));
            const Id far_filter = o.And(partial, o.Gt(radius_texels, o.F(Tune::FilterMidTexels)));
            when(near_filter, [&] { filter(Tune::FilterNear); });
            when(mid_filter, [&] { filter(Tune::FilterMid); });
            when(far_filter, [&] { filter(Tune::FilterFar); });
        });
    });

    // ---- 3. Contact shadows ----
    const Id moon_lit = ctx.OpLoad(ctx.F32[1], vis);
    const Id contact = o.And(o.And(active, screen_ok),
                             o.And(o.Gt(moon_lit, o.F(0.02f)), o.Lt(dist, o.F(Tune::ContactFarM))));
    when(contact, [&] {
        Id occlusion = zero;
        const Id zero2 = ctx.ConstF32(0.0f, 0.0f), one2 = ctx.ConstF32(1.0f, 1.0f);
        const Id bias_base =
            o.Add(o.F(Tune::ContactBiasM), o.Mul(o.F(Tune::ContactBiasPerM), dist));
        // The march is a line: the point's clip x, y, w at share 0 (the pixel) and their change
        // to share 1 (ContactM towards the moon), its distance from the eye by |p + t moon|^2 =
        // p.p + 2 t p.moon + t^2 (the lamp march's way: a third of the work a step).
        const Id p4 = o.Vec4(o.X(p, 0), o.X(p, 1), pz, one);
        const Id tip = ctx.OpFAdd(ctx.F32[3], p,
                                  ctx.OpVectorTimesScalar(ctx.F32[3], moon, o.F(Tune::ContactM)));
        const Id tip4 = o.Vec4(o.X(tip, 0), o.X(tip, 1), o.X(tip, 2), one);
        std::array<Id, 3> clip0, clip_d;
        for (u32 k = 0; const u32 r : {0u, 1u, 3u}) {
            clip0[k] = o.Dot(inv_rows[r], p4);
            clip_d[k] = o.Sub(o.Dot(inv_rows[r], tip4), clip0[k]);
            ++k;
        }
        const Id pp = o.Dot(p, p);
        const Id pd2 = o.Mul(o.F(2.0f * Tune::ContactM), o.Dot(p, moon));
        const Id dz = o.Mul(o.X(moon, 2), o.F(Tune::ContactM));
        for (u32 j = 0; j < Tune::ContactSteps; ++j) {
            const Id share = o.Mul(o.Add(o.F(float(j) + 0.5f), march_seed),
                                   o.F(1.0f / float(Tune::ContactSteps)));
            const Id t = o.Mul(o.F(Tune::ContactM), share);
            const auto along_line = [&](u32 k) {
                return ctx.OpFma(ctx.F32[1], clip_d[k], share, clip0[k]);
            };
            const Id cw = along_line(2);
            const Id in_front = o.Gt(cw, o.F(1e-6f));
            const Id inv_cw = o.Div(one, o.Max(cw, o.F(1e-6f)));
            const Id qnx = o.Mul(along_line(0), inv_cw);
            const Id qny = o.Mul(along_line(1), inv_cw);
            const Id quv = o.Vec2(o.Add(o.Mul(ax, qnx), bx), o.Add(o.Mul(ay, qny), by));
            const Id inside = o.And(
                in_front,
                o.And(ctx.OpAll(ctx.U1[1], ctx.OpFOrdGreaterThanEqual(ctx.U1[2], quv, zero2)),
                      ctx.OpAll(ctx.U1[1], ctx.OpFOrdLessThanEqual(ctx.U1[2], quv, one2))));
            const Id ds = o.X(EmitImageSampleExplicitLod(ctx, nullptr, mi.depth_image,
                                                         ctx.OpFClamp(ctx.F32[2], quv, zero2, one2),
                                                         zero, IR::Value{}),
                              0);
            // The surface there lies on the same eye ray as the point (M's perspective shape: the
            // eye at the origin), so its distance is the point's times their depths' ratio.
            const Id s4 = o.Vec4(qnx, qny, ds, one);
            const Id sz = o.Div(o.Dot(rows[2], s4), o.Dot(rows[3], s4));
            const Id qz = ctx.OpFma(ctx.F32[1], dz, share, pz);
            const Id qz_safe = ctx.OpSelect(
                ctx.F32[1], o.Lt(ctx.OpFAbs(ctx.F32[1], qz), o.F(1e-6f)), o.F(1e-6f), qz);
            const Id q_len = ctx.OpSqrt(
                ctx.F32[1],
                o.Max(o.Add(pp, o.Mul(share, o.Add(pd2, o.Mul(share, o.F(Tune::ContactM *
                                                                         Tune::ContactM))))),
                      zero));
            // In front of the point by more than the bias (growing with the pixel's distance and
            // along the march), less than an occluder's thickness; a farther hit counts less.
            const Id gap = o.Mul(q_len, o.Sub(one, o.Div(sz, qz_safe)));
            const Id bias_here = o.Add(bias_base, o.Mul(o.F(Tune::ContactBiasAlong), t));
            const Id thin = o.Clamp(o.Mul(o.Sub(o.F(Tune::ContactThickM), gap),
                                          o.F(1.0f / (0.4f * Tune::ContactThickM))),
                                    0.0f, 1.0f);
            const Id past_bias = o.Clamp(o.Div(o.Sub(gap, bias_here), bias_here), 0.0f, 1.0f);
            const Id along = o.Sub(one, o.Mul(o.F(0.5f), share));
            const Id hit = o.Mul(o.Mul(past_bias, thin), along);
            occlusion = o.Max(occlusion, ctx.OpSelect(ctx.F32[1], inside, hit, zero));
        }
        const Id fade = o.Clamp(o.Mul(o.Sub(o.F(Tune::ContactFarM), dist),
                                      o.F(1.0f / (Tune::ContactFarM - Tune::ContactFadeM))),
                                0.0f, 1.0f);
        ctx.OpStore(vis, o.Mul(moon_lit, o.Sub(one, o.Mul(occlusion,
                                                         o.Mul(o.F(Tune::ContactStrength), fade)))));
    });
    // (Not a number: lit, as the game would have it.)
    const Id ours = ctx.OpFClamp(ctx.F32[1], Finite(ctx, ctx.OpLoad(ctx.F32[1], vis), one),
                                 zero, one);
    // VR_MOON_SHADOW_EYES=1 (tests): the left eye keeps the game's own 4 taps (its offsets at the
    // fade buffer's dwords 74, 75), the right eye ours: one frame, both looks. =2: the right eye
    // without the moon at all (where it lights anything). =3: ours without contact shadows on
    // the left (their share alone); =4: ours without them on the right (contact hardening alone).
    static const int eyes = [] {
        const char* const env = std::getenv("VR_MOON_SHADOW_EYES");
        return env != nullptr ? std::atoi(env) : 0;
    }();
    if (eyes != 0 && ctx.runtime_info.native_stereo && ctx.runtime_info.native_multiview &&
        mi.params_buffer != 0xFFFFFFFFu) {
        const Id a = MoonRead(ctx, mi.params_buffer, ctx.ConstU32(74u));
        const Id b = MoonRead(ctx, mi.params_buffer, ctx.ConstU32(75u));
        const Id na = ctx.OpFNegate(ctx.F32[1], a), nb = ctx.OpFNegate(ctx.F32[1], b);
        Id game = zero;
        for (const auto& [du, dv] : {std::pair{b, na}, std::pair{a, b}, std::pair{na, nb},
                                     std::pair{nb, a}}) {
            const Id tap = EmitImageSampleDrefExplicitLod(
                ctx, nullptr, mi.shadow_compare, ctx.OpFAdd(ctx.F32[2], here, o.Vec2(du, dv)), sref,
                zero, IR::Value{});
            game = o.Add(game, o.Mul(o.X(tap, 0), o.F(0.25f)));
        }
        return ctx.OpSelect(ctx.F32[1],
                            ctx.OpIEqual(ctx.U1[1], ctx.NativeEyeIndex(), ctx.u32_zero_value),
                            eyes == 3 ? moon_lit : game,
                            eyes == 2 ? zero : (eyes == 4 ? moon_lit : ours));
    }
    return ours;
}

} // namespace Shader::Backend::SPIRV
