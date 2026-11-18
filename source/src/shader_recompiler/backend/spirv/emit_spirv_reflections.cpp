// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// VrReflection (ir/passes/reflections.h, ReflectionsPass): this pixel's reflection, weighted
// (rgb * a, a), in place of the game's trace, from the game's own buffer (each eye's, as
// RemapLighting's Reflection case makes it: rows 0..15 the eye's ndc x, y and raw depth to view,
// 16..27 view to the eye's clip with w = view z, 32..42 the G-buffer normals' rotation, 48..63
// this frame's ndc and depth to last frame's clip, 78 the game's reach in view metres).
//  1. The pixel's view position P and a normal: the G-buffer's, steadied towards the plane its
//     four depth neighbours make where they are continuous and agree with it.
//  2. The reflected ray R from P, projected: a line in (u, v, raw depth) (raw depth is affine in
//     1 / z, so the line is exact), cut where it leaves the screen or the depth range, or
//     MaxRayUv.
//  3. MaxSteps points along it, (i + jitter)^2 spaced, until one is behind the depth while the
//     one before was in front; RefineSteps halvings; kept when ray and surface are within the
//     thickness in view metres and the hit lies far enough along R; otherwise on.
//  4. Last frame's colour at the hit through the reprojection, only from inside last frame's
//     screen; finite, clamped; faded at the edges (the hit's, now and last frame), for rays
//     turned towards the eye and near the game's reach.
// Every division is guarded and every result checked finite: a reflection is (0, 0, 0, 0) or a
// finite colour, never a NaN (the game's mip chain and the upscaler would spread it).
// The conditions of selections and of the loop come before their merges (Sirit emits in call
// order; OpLoopMerge directly before the header's branch).

#include <array>
#include <cstdlib>

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/passes/reflections.h"

namespace Shader::Backend::SPIRV {
namespace {

namespace Tune = Shader::Optimization::Reflections;

struct ReflMath {
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
    Id Abs(Id a) const {
        return ctx.OpFAbs(ctx.F32[1], a);
    }
    Id Clamp(Id x, float lo, float hi) const {
        return ctx.OpFClamp(ctx.F32[1], x, F(lo), F(hi));
    }
    Id Sat(Id x) const {
        return Clamp(x, 0.0f, 1.0f);
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
    Id Dot3(Id a, Id b) const {
        return ctx.OpDot(ctx.F32[1], a, b);
    }
    Id Dot4(Id a, Id b) const {
        return ctx.OpDot(ctx.F32[1], a, b);
    }
    Id Scale3(Id v, Id s) const {
        return ctx.OpVectorTimesScalar(ctx.F32[3], v, s);
    }
    Id Add3(Id a, Id b) const {
        return ctx.OpFAdd(ctx.F32[3], a, b);
    }
    Id Sub3(Id a, Id b) const {
        return ctx.OpFSub(ctx.F32[3], a, b);
    }
    Id Gt(Id a, Id b) const {
        return ctx.OpFOrdGreaterThan(ctx.U1[1], a, b);
    }
    Id Ge(Id a, Id b) const {
        return ctx.OpFOrdGreaterThanEqual(ctx.U1[1], a, b);
    }
    Id Lt(Id a, Id b) const {
        return ctx.OpFOrdLessThan(ctx.U1[1], a, b);
    }
    Id Le(Id a, Id b) const {
        return ctx.OpFOrdLessThanEqual(ctx.U1[1], a, b);
    }
    Id And(Id a, Id b) const {
        return ctx.OpLogicalAnd(ctx.U1[1], a, b);
    }
    Id Sel(Id c, Id a, Id b) const {
        return ctx.OpSelect(ctx.F32[1], c, a, b);
    }
    // a / b with b kept at least `tiny` away from 0 (its sign kept).
    Id SafeDiv(Id a, Id b, float tiny) const {
        const Id far_enough = Gt(Abs(b), F(tiny));
        const Id signed_tiny = Sel(Lt(b, F(0.0f)), F(-tiny), F(tiny));
        return Div(a, Sel(far_enough, b, signed_tiny));
    }
    // normalize(v), or `fallback` where |v| is (near) 0.
    Id SafeNormalize(Id v, Id fallback) const {
        const Id len2 = Dot3(v, v);
        const Id ok = Gt(len2, F(1e-12f));
        const Id inv = ctx.OpInverseSqrt(ctx.F32[1], Max(len2, F(1e-12f)));
        return ctx.OpSelect(ctx.F32[3], ok, Scale3(v, inv), fallback);
    }
    // Hermite smoothstep(e0, e1, x).
    Id SmoothStep(float e0, float e1, Id x) const {
        const Id t = Sat(Mul(Sub(x, F(e0)), F(1.0f / (e1 - e0))));
        return Mul(Mul(t, t), Sub(F(3.0f), Mul(F(2.0f), t)));
    }
    // True where x is neither NaN nor infinite.
    // (The exponent's bits as integers: without SignedZeroInfNanPreserve a driver may fold a
    // float NaN or Inf test away.)
    Id Finite(Id x) const {
        const Id e = ctx.ConstU32(0x7f800000u);
        return ctx.OpINotEqual(ctx.U1[1],
                               ctx.OpBitwiseAnd(ctx.U32[1], ctx.OpBitcast(ctx.U32[1], x), e), e);
    }
};

Id ReflRead(EmitContext& ctx, u32 handle, u32 dword) {
    return ctx.OpBitcast(ctx.F32[1], EmitReadConstBuffer(ctx, handle, ctx.ConstU32(dword)));
}

Id ReflPrivate(EmitContext& ctx, Id type) {
    const Id var = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Private, type),
                                         spv::StorageClass::Private);
    ctx.interfaces.push_back(var);
    return var;
}

// An image's size at mip 0 (image | sampler handle).
Id ReflSize(EmitContext& ctx, u32 handle) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.NativeNonUniform(
        ctx.OpLoad(texture.image_type, ctx.NativeImagePointer(texture.image_type, texture.id)));
    const Id size = ctx.OpImageQuerySizeLod(ctx.U32[2], image, ctx.u32_zero_value);
    return ctx.OpConvertUToF(ctx.F32[2], size);
}

// The sample at `uv` (mip 0), all four channels.
Id ReflSample(EmitContext& ctx, u32 handle, Id uv) {
    return EmitImageSampleExplicitLod(ctx, nullptr, handle, uv, ctx.ConstF32(0.0f), IR::Value{});
}

} // namespace

Id EmitVrReflection(EmitContext& ctx, Id ndc, Id uv, Id depth) {
    const auto& ri = ctx.info.vr_reflection;
    const Id none = ctx.ConstF32(0.0f, 0.0f, 0.0f, 0.0f);
    if (!ri.on) {
        return none;
    }
    const ReflMath o{ctx};
    const Id zero = o.F(0.0f), one = o.F(1.0f);
    const auto cb = [&](u32 d) { return ReflRead(ctx, ri.buffer, d); };
    const auto row4 = [&](u32 d) { return o.Vec4(cb(d), cb(d + 1), cb(d + 2), cb(d + 3)); };
    const auto row3 = [&](u32 d) { return o.Vec3(cb(d), cb(d + 1), cb(d + 2)); };
    // ndc x, y and raw depth to view (rows 0..15), view to clip (16..27; w = view z), the
    // normals' rotation (32..42), this frame to last frame's clip (48..63).
    const std::array<Id, 4> inv{row4(0), row4(4), row4(8), row4(12)};
    const std::array<Id, 3> proj{row4(16), row4(20), row4(24)};
    const std::array<Id, 3> rot{row3(32), row3(36), row3(40)};
    const std::array<Id, 3> rep{row4(48), row4(52), row4(60)};
    const Id reach = cb(78);
    // View position and view z at ndc (x, y) and raw depth d.
    const auto view_at = [&](Id x, Id y, Id d) {
        const Id v = o.Vec4(x, y, d, one);
        const Id iw = o.SafeDiv(one, o.Dot4(inv[3], v), 1e-20f);
        return o.Vec3(o.Mul(o.Dot4(inv[0], v), iw), o.Mul(o.Dot4(inv[1], v), iw),
                      o.Mul(o.Dot4(inv[2], v), iw));
    };
    const auto view_z = [&](Id x, Id y, Id d) {
        const Id v = o.Vec4(x, y, d, one);
        return o.SafeDiv(o.Dot4(inv[2], v), o.Dot4(inv[3], v), 1e-20f);
    };

    // ---- 1. The pixel: position and normal ----
    const Id nx = o.X(ndc, 0), ny = o.X(ndc, 1);
    const Id p = view_at(nx, ny, depth);
    const Id pz = o.X(p, 2);
    const Id size = ReflSize(ctx, ri.depth_image);
    const Id tx = o.Div(one, o.Max(o.X(size, 0), one));
    const Id ty = o.Div(one, o.Max(o.X(size, 1), one));
    const Id u0 = o.X(uv, 0), v0 = o.X(uv, 1);
    // The depth's four neighbours (u right and v down: ndc x right, y up).
    const auto neighbour = [&](float du, float dv) {
        const Id u = o.Add(u0, o.Mul(o.F(du), tx)), v = o.Add(v0, o.Mul(o.F(dv), ty));
        const Id d = o.X(ReflSample(ctx, ri.depth_image, o.Vec2(u, v)), 0);
        return view_at(o.Add(nx, o.Mul(o.F(2.0f * du), tx)), o.Sub(ny, o.Mul(o.F(2.0f * dv), ty)), d);
    };
    const Id px = neighbour(1.0f, 0.0f), nxp = neighbour(-1.0f, 0.0f);
    const Id py = neighbour(0.0f, 1.0f), nyp = neighbour(0.0f, -1.0f);
    // Each axis's nearer side (a silhouette on one side does not bend the plane).
    const auto side = [&](Id plus, Id minus) {
        const Id fwd = o.Sub3(plus, p), back = o.Sub3(p, minus);
        return ctx.OpSelect(ctx.F32[3], o.Lt(o.Abs(o.X(fwd, 2)), o.Abs(o.X(back, 2))), fwd, back);
    };
    const Id dx = side(px, nxp), dy = side(py, nyp);
    Id g = o.SafeNormalize(ctx.OpCross(ctx.F32[3], dx, dy), o.Vec3(zero, zero, zero));
    // Towards the eye.
    g = ctx.OpSelect(ctx.F32[3], o.Gt(o.Dot3(g, p), zero), ctx.OpFNegate(ctx.F32[3], g), g);
    // The G-buffer's normal (encoded n * 0.5 + 0.5) in view space.
    const Id gb = ReflSample(ctx, ri.normal_image, uv);
    const Id raw = o.Vec3(o.Sub(o.Mul(o.X(gb, 0), o.F(2.0f)), one),
                          o.Sub(o.Mul(o.X(gb, 1), o.F(2.0f)), one),
                          o.Sub(o.Mul(o.X(gb, 2), o.F(2.0f)), one));
    const Id n_world = o.SafeNormalize(raw, g);
    const Id nv = o.SafeNormalize(
        o.Vec3(o.Dot3(rot[0], n_world), o.Dot3(rot[1], n_world), o.Dot3(rot[2], n_world)), g);
    // Steadied: towards the plane where the depth is continuous and agrees, the ripple across
    // the plane kept to RippleMax.
    const Id continuity =
        o.Div(o.Max(o.Abs(o.X(dx, 2)), o.Abs(o.X(dy, 2))), o.Max(o.Abs(pz), o.F(1e-3f)));
    const Id plane_w =
        o.Mul(o.Sub(one, o.SmoothStep(Tune::ContinuityLo, Tune::ContinuityHi, continuity)),
              o.SmoothStep(Tune::AgreeLo, Tune::AgreeHi, o.Dot3(g, nv)));
    const Id ripple = o.Sub3(nv, o.Scale3(g, o.Dot3(nv, g)));
    const Id ripple_len = ctx.OpLength(ctx.F32[1], ripple);
    const Id ripple_kept =
        o.Scale3(ripple, o.Min(one, o.Div(o.F(Tune::RippleMax), o.Max(ripple_len, o.F(1e-4f)))));
    const Id flat = o.SafeNormalize(o.Add3(g, ripple_kept), nv);
    const Id n = o.SafeNormalize(
        ctx.OpFMix(ctx.F32[3], nv, flat,
                   o.Vec3(o.Mul(plane_w, o.F(Tune::PlaneBlend)), o.Mul(plane_w, o.F(Tune::PlaneBlend)),
                          o.Mul(plane_w, o.F(Tune::PlaneBlend)))),
        nv);

    // ---- 2. The ray on the screen ----
    const Id view_dir = o.SafeNormalize(p, o.Vec3(zero, zero, one));
    const Id r = o.SafeNormalize(
        o.Sub3(view_dir, o.Scale3(n, o.Mul(o.F(2.0f), o.Dot3(n, view_dir)))), o.Vec3(zero, zero, one));
    const Id rz = o.X(r, 2);
    // A point on R kept in front of the eye (view z at least 5 cm).
    const Id toward = o.Lt(rz, zero);
    const Id len_q = o.Sel(toward,
                           o.Clamp(o.Div(o.Sub(pz, o.F(0.05f)), o.Max(o.Abs(rz), o.F(1e-4f))),
                                   1e-3f, 1.0f),
                           one);
    const Id q = o.Add3(p, o.Scale3(r, len_q));
    const Id q4 = o.Vec4(o.X(q, 0), o.X(q, 1), o.X(q, 2), one);
    const Id iqz = o.Div(one, o.Max(o.X(q, 2), o.F(1e-6f)));
    const Id ex = o.Mul(o.Dot4(proj[0], q4), iqz), ey = o.Mul(o.Dot4(proj[1], q4), iqz);
    const Id ed = o.Mul(o.Dot4(proj[2], q4), iqz);
    // (u, v, d) per unit of screen distance.
    const Id du = o.Mul(o.F(0.5f), o.Sub(ex, nx)), dv = o.Mul(o.F(-0.5f), o.Sub(ey, ny));
    const Id dd = o.Sub(ed, depth);
    const Id uv_len = ctx.OpLength(ctx.F32[1], o.Vec2(du, dv));
    const Id inv_len = o.Div(one, o.Max(uv_len, o.F(1e-7f)));
    const Id dir = o.Vec3(o.Mul(du, inv_len), o.Mul(dv, inv_len), o.Mul(dd, inv_len));
    // The game's start: the pixel's (u, v) from its ndc, its depth.
    const Id start = o.Vec3(o.Add(o.Mul(nx, o.F(0.5f)), o.F(0.5f)),
                            o.Sub(o.F(0.5f), o.Mul(ny, o.F(0.5f))), depth);
    // Where it leaves the screen or the depth range.
    const auto leave = [&](Id from, Id d) {
        const Id up = o.Div(o.Sub(one, from), o.Max(d, o.F(1e-8f)));
        const Id down = o.Div(from, o.Max(ctx.OpFNegate(ctx.F32[1], d), o.F(1e-8f)));
        return o.Sel(o.Gt(d, o.F(1e-8f)), up, o.Sel(o.Lt(d, o.F(-1e-8f)), down, o.F(1e9f)));
    };
    const Id ray_uv = o.Min(o.Min(leave(o.X(start, 0), o.X(dir, 0)), leave(o.X(start, 1), o.X(dir, 1))),
                            o.Min(leave(depth, o.X(dir, 2)), o.F(Tune::MaxRayUv)));
    const Id t_first = o.Mul(o.F(Tune::StartTexels), o.Max(tx, ty));
    const Id span = o.Sub(ray_uv, t_first);
    // Rays the game's would never trace: sky, a degenerate ray, one shorter than a texel or two,
    // one turned fully towards the eye.
    Id active = o.And(o.Lt(depth, one), o.Gt(uv_len, o.F(1e-6f)));
    active = o.And(active, o.Gt(span, zero));
    active = o.And(active, o.Gt(rz, o.F(Tune::FacingMin)));
    active = o.And(active, o.And(o.Finite(o.X(p, 0)), o.Finite(pz)));

    // Jitter (0..1): interleaved gradient noise on the pixel grid; with an upscaler moved every
    // frame by a hash of the reprojection's translation (it changes with every head move), the
    // same shift for every pixel (2026-11-11, the user: "little dotted artifacts or shimmer
    // appearing in the ground": the pixel's depth bits in the hash scattered the pattern into
    // white noise that crawled on wet ground; a shift of the whole pattern the upscaler resolves,
    // and with a still head the reflection stands still).
    const Id fx = EmitGetAttribute(ctx, IR::Attribute::FragCoord, 0, 0);
    const Id fy = EmitGetAttribute(ctx, IR::Attribute::FragCoord, 1, 0);
    Id frame = zero;
    if (ri.jitter) {
        const auto u32c = [&](u32 x) { return ctx.ConstU32(x); };
        Id h = ctx.OpBitcast(ctx.U32[1], cb(51));
        for (const Id w : {cb(55), cb(63)}) {
            h = ctx.OpBitwiseXor(ctx.U32[1], ctx.OpIMul(ctx.U32[1], h, u32c(16777619u)),
                                 ctx.OpBitcast(ctx.U32[1], w));
        }
        h = ctx.OpBitwiseXor(ctx.U32[1], h, ctx.OpShiftRightLogical(ctx.U32[1], h, u32c(16)));
        h = ctx.OpIMul(ctx.U32[1], h, u32c(0x7feb352du));
        h = ctx.OpBitwiseXor(ctx.U32[1], h, ctx.OpShiftRightLogical(ctx.U32[1], h, u32c(15)));
        frame = o.Mul(ctx.OpConvertUToF(ctx.F32[1], ctx.OpBitwiseAnd(ctx.U32[1], h, u32c(63u))),
                      o.F(5.588238f));
    }
    const Id inner = ctx.OpFract(ctx.F32[1], o.Add(o.Mul(o.Add(fx, frame), o.F(0.06711056f)),
                                                   o.Mul(o.Add(fy, frame), o.F(0.00583715f))));
    const Id jitter = ctx.OpFract(ctx.F32[1], o.Mul(inner, o.F(52.9829189f)));

    const Id result = ReflPrivate(ctx, ctx.F32[4]);
    ctx.OpStore(result, none);
    // if (cond) { body }: the condition is computed before the merge.
    const auto when = [&](Id cond, auto&& body) {
        const Id then_label = ctx.OpLabel(), endif = ctx.OpLabel();
        ctx.OpSelectionMerge(endif, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(cond, then_label, endif);
        ctx.AddLabel(then_label);
        body();
        ctx.OpBranch(endif);
        ctx.AddLabel(endif);
    };
    const auto at = [&](Id t) { return o.Add3(start, o.Scale3(dir, t)); };
    const auto depth_at = [&](Id point) {
        const Id c = ctx.OpFClamp(ctx.F32[2], o.Vec2(o.X(point, 0), o.X(point, 1)),
                                  ctx.ConstF32(0.0f, 0.0f), ctx.ConstF32(1.0f, 1.0f));
        return o.X(ReflSample(ctx, ri.depth_image, c), 0);
    };

    when(active, [&] {
        // ---- 3. The march ----
        const Id step = ReflPrivate(ctx, ctx.U32[1]);
        const Id prev_t = ReflPrivate(ctx, ctx.F32[1]);
        const Id prev_diff = ReflPrivate(ctx, ctx.F32[1]);
        const Id hit_t = ReflPrivate(ctx, ctx.F32[1]);
        const Id hit_depth = ReflPrivate(ctx, ctx.F32[1]);
        ctx.OpStore(step, ctx.u32_zero_value);
        ctx.OpStore(prev_t, zero);
        ctx.OpStore(prev_diff, o.F(-1.0f)); // the pixel itself: in front
        ctx.OpStore(hit_t, o.F(-1.0f));
        ctx.OpStore(hit_depth, one);
        const Id header = ctx.OpLabel(), body = ctx.OpLabel(), cont = ctx.OpLabel(),
                 merge = ctx.OpLabel();
        ctx.OpBranch(header);
        ctx.AddLabel(header);
        const Id i = ctx.OpLoad(ctx.U32[1], step);
        const Id go = ctx.OpLogicalAnd(ctx.U1[1], ctx.OpULessThan(ctx.U1[1], i, ctx.ConstU32(Tune::MaxSteps)),
                                       o.Lt(ctx.OpLoad(ctx.F32[1], hit_t), zero));
        ctx.OpLoopMerge(merge, cont, spv::LoopControlMask::MaskNone);
        ctx.OpBranchConditional(go, body, merge);
        ctx.AddLabel(body);
        {
            const Id share = o.Mul(o.Add(ctx.OpConvertUToF(ctx.F32[1], i), jitter),
                                   o.F(1.0f / float(Tune::MaxSteps)));
            const Id t = o.Add(t_first, o.Mul(span, o.Mul(share, share)));
            const Id point = at(t);
            const Id s = depth_at(point);
            const Id diff = o.Sub(o.X(point, 2), s);
            const Id crossing = o.And(o.And(o.Ge(diff, zero), o.Lt(ctx.OpLoad(ctx.F32[1], prev_diff), zero)),
                                      o.Lt(s, one));
            const Id t_before = ctx.OpLoad(ctx.F32[1], prev_t);
            when(crossing, [&] {
                // Halvings: lo in front of the depth, hi behind it.
                Id lo = t_before, hi = t;
                for (u32 k = 0; k < Tune::RefineSteps; ++k) {
                    const Id mid = o.Mul(o.Add(lo, hi), o.F(0.5f));
                    const Id mp = at(mid);
                    const Id behind = o.Ge(o.X(mp, 2), depth_at(mp));
                    hi = o.Sel(behind, mid, hi);
                    lo = o.Sel(behind, lo, mid);
                }
                const Id hp = at(hi);
                const Id hs = depth_at(hp);
                const Id hx = o.Sub(o.Mul(o.X(hp, 0), o.F(2.0f)), one);
                const Id hy = o.Sub(one, o.Mul(o.X(hp, 1), o.F(2.0f)));
                const Id z_ray = view_z(hx, hy, o.X(hp, 2));
                const Id z_surface = view_z(hx, hy, hs);
                const Id thick = o.Clamp(o.Mul(o.Abs(z_surface), o.F(Tune::ThickRel)), Tune::ThickMinM,
                                         Tune::ThickMaxM);
                const Id close = o.Le(o.Abs(o.Sub(z_ray, z_surface)), thick);
                const Id travel = o.Dot3(o.Sub3(view_at(hx, hy, hs), p), r);
                const Id far_enough =
                    o.Gt(travel, o.Max(o.F(Tune::MinTravelM), o.Mul(o.Abs(pz), o.F(Tune::MinTravelRel))));
                // Within the game's reach (dword 78, view metres; none set: any).
                const Id in_reach = ctx.OpLogicalOr(ctx.U1[1], o.Le(reach, zero), o.Lt(z_surface, reach));
                const Id accept = o.And(o.And(o.And(close, far_enough), o.And(in_reach, o.Lt(hs, one))),
                                        o.And(o.Finite(z_ray), o.Finite(z_surface)));
                when(accept, [&] {
                    ctx.OpStore(hit_t, hi);
                    ctx.OpStore(hit_depth, hs);
                });
            });
            ctx.OpStore(prev_diff, diff);
            ctx.OpStore(prev_t, t);
            ctx.OpBranch(cont);
        }
        ctx.AddLabel(cont);
        ctx.OpStore(step, ctx.OpIAdd(ctx.U32[1], i, ctx.ConstU32(1u)));
        ctx.OpBranch(header);
        ctx.AddLabel(merge);

        // ---- 4. Last frame's colour at the hit ----
        const Id t_hit = ctx.OpLoad(ctx.F32[1], hit_t);
        const Id hs = ctx.OpLoad(ctx.F32[1], hit_depth);
        const Id hp = at(o.Max(t_hit, zero));
        const Id hx = o.Sub(o.Mul(o.X(hp, 0), o.F(2.0f)), one);
        const Id hy = o.Sub(one, o.Mul(o.X(hp, 1), o.F(2.0f)));
        const Id h4 = o.Vec4(hx, hy, hs, one);
        const Id rw = o.Dot4(rep[2], h4);
        const Id irw = o.Div(one, o.Max(rw, o.F(1e-6f)));
        const Id pu = o.Add(o.F(0.5f), o.Mul(o.F(0.5f), o.Mul(o.Dot4(rep[0], h4), irw)));
        const Id pv = o.Sub(o.F(0.5f), o.Mul(o.F(0.5f), o.Mul(o.Dot4(rep[1], h4), irw)));
        const Id on_screen =
            o.And(o.And(o.Ge(pu, zero), o.Le(pu, one)), o.And(o.Ge(pv, zero), o.Le(pv, one)));
        const Id found = o.And(o.And(o.Ge(t_hit, zero), o.Gt(rw, o.F(1e-6f))), on_screen);
        when(found, [&] {
            const Id c = ReflSample(ctx, ri.colour_image, o.Vec2(pu, pv));
            const Id cr = o.X(c, 0), cg = o.X(c, 1), cbl = o.X(c, 2);
            const Id finite = o.And(o.And(o.Finite(cr), o.Finite(cg)), o.Finite(cbl));
            const auto keep = [&](Id x) { return o.Clamp(x, 0.0f, Tune::MaxRadiance); };
            // Edges: the hit's place now and last frame.
            const auto edge = [&](Id a) {
                return o.Sat(o.Div(o.Min(a, o.Sub(one, a)), o.F(Tune::EdgeFadeUv)));
            };
            const Id edges = o.Min(o.Min(edge(pu), edge(pv)), o.Min(edge(o.X(hp, 0)), edge(o.X(hp, 1))));
            const Id facing = o.Sat(o.Div(o.Sub(rz, o.F(Tune::FacingMin)), o.F(Tune::FacingFade)));
            const Id z_hit = view_z(hx, hy, hs);
            const Id far_fade = o.Sel(o.Gt(reach, zero),
                                      o.Sub(one, o.Sat(o.Div(o.Sub(z_hit, o.Mul(reach, o.F(Tune::FarFadeStart))),
                                                             o.Max(o.Mul(reach, o.F(1.0f - Tune::FarFadeStart)),
                                                                   o.F(1e-3f))))),
                                      one);
            const Id weight = o.Mul(o.Mul(o.Mul(edges, edges), facing), far_fade);
            const Id w = o.Sel(finite, o.Sat(weight), zero);
            ctx.OpStore(result, o.Vec4(o.Sel(finite, o.Mul(keep(cr), w), zero),
                                       o.Sel(finite, o.Mul(keep(cg), w), zero),
                                       o.Sel(finite, o.Mul(keep(cbl), w), zero), w));
        });
    });
    // Never a NaN or an infinity out (each channel checked once more).
    const Id out = ctx.OpLoad(ctx.F32[4], result);
    Id all_finite = ctx.ConstantTrue(ctx.U1[1]);
    for (u32 k = 0; k < 4; ++k) {
        all_finite = o.And(all_finite, o.Finite(o.X(out, k)));
    }
    return ctx.OpSelect(ctx.F32[4], all_finite, out, none);
}

namespace {

// Channel 0 of the 2 x 2 texels round `uv` (an image | sampler handle).
Id ReflGather(EmitContext& ctx, u32 handle, Id uv) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.NativeNonUniform(
        ctx.OpLoad(texture.image_type, ctx.NativeImagePointer(texture.image_type, texture.id)));
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled =
        ctx.NativeNonUniform(ctx.OpSampledImage(texture.sampled_type, image, sampler));
    const Id texels =
        ctx.OpImageGather(texture.data_types->Get(4), sampled, uv, ctx.ConstU32(0u));
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texels) : texels;
}

// if (cond) { body }: the condition computed before the merge (Sirit emits in call order).
template <typename Body>
void ReflWhen(EmitContext& ctx, Id cond, Body&& body) {
    const Id then_label = ctx.OpLabel(), endif = ctx.OpLabel();
    ctx.OpSelectionMerge(endif, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(cond, then_label, endif);
    ctx.AddLabel(then_label);
    body();
    ctx.OpBranch(endif);
    ctx.AddLabel(endif);
}

} // namespace

// Sharper wet streets (reflections.h UpsampleLodMax): mip 0's bilinear share of the composite's
// trilinear sample swapped for a depth-aware one where the 4 texels' depths differ.
Id EmitVrReflectionUpsample(EmitContext& ctx, Id bilinear, Id uv, Id depth, Id lod) {
    const auto& ru = ctx.info.vr_reflection_upsample;
    static const bool off = [] {
        const char* const env = std::getenv("VR_REFLECTION_UPSAMPLE");
        return env != nullptr && env[0] == '0';
    }();
    if (!ru.on || off) {
        return bilinear;
    }
    const ReflMath o{ctx};
    const Id one = o.F(1.0f);
    const Id r10 = ReflRead(ctx, ru.buffer, 10), r11 = ReflRead(ctx, ru.buffer, 11);
    const Id r14 = ReflRead(ctx, ru.buffer, 14), r15 = ReflRead(ctx, ru.buffer, 15);
    // The view depth of a raw depth d (ndc x = y = 0: the matrix's perspective shape makes it
    // the same on the whole ray).
    const auto view_z = [&](Id d) {
        return o.Abs(o.SafeDiv(o.Add(o.Mul(r10, d), r11), o.Add(o.Mul(r14, d), r15), 1e-20f));
    };
    const Id size = ReflSize(ctx, ru.colour_image);
    const Id full = ReflSize(ctx, ru.depth_image);
    const Id zc = view_z(depth);
    const Id gate = o.And(
        o.And(o.Lt(lod, o.F(Tune::UpsampleLodMax)), o.Gt(o.X(bilinear, 3), o.F(Tune::MinWeight))),
        o.And(o.And(o.Finite(zc), o.Lt(zc, o.F(Tune::UpsampleMaxM))),
              o.Lt(o.X(size, 0), o.Mul(o.X(full, 0), o.F(0.75f)))));
    const Id result = ReflPrivate(ctx, ctx.F32[4]);
    ctx.OpStore(result, bilinear);
    ReflWhen(ctx, gate, [&] {
        const Id inv = ctx.OpFDiv(ctx.F32[2], ctx.ConstF32(1.0f, 1.0f), size);
        const Id st =
            ctx.OpFSub(ctx.F32[2], ctx.OpFMul(ctx.F32[2], uv, size), ctx.ConstF32(0.5f, 0.5f));
        const Id i = ctx.OpFloor(ctx.F32[2], st);
        const Id f = ctx.OpFSub(ctx.F32[2], st, i);
        // The full-size depth's 2 x 2 round the 4 texels' shared corner: one in each one's
        // footprint (x: (0, 1), y: (1, 1), z: (1, 0), w: (0, 0)).
        const Id g = ReflGather(
            ctx, ru.depth_image,
            ctx.OpFMul(ctx.F32[2], ctx.OpFAdd(ctx.F32[2], i, ctx.ConstF32(1.0f, 1.0f)), inv));
        const std::array<Id, 4> d{o.X(g, 3), o.X(g, 2), o.X(g, 0), o.X(g, 1)}; // 00 10 01 11
        std::array<Id, 4> wd;
        Id worst = o.F(0.0f);
        for (u32 k = 0; k < 4; ++k) {
            const Id rel = o.Div(o.Abs(o.Sub(view_z(d[k]), zc)), o.Max(zc, o.F(1e-3f)));
            const Id r = o.Sel(o.Finite(rel), rel, o.F(1e6f));
            worst = o.Max(worst, r);
            const Id q = o.Mul(r, o.F(1.0f / Tune::UpsampleTol));
            wd[k] = o.Div(one, o.Add(one, o.Mul(q, q)));
        }
        ReflWhen(ctx, o.Gt(worst, o.F(Tune::UpsampleEdgeRel)), [&] {
            const Id fx = o.X(f, 0), fy = o.X(f, 1);
            const Id gx = o.Sub(one, fx), gy = o.Sub(one, fy);
            const std::array<Id, 4> wb{o.Mul(gx, gy), o.Mul(fx, gy), o.Mul(gx, fy), o.Mul(fx, fy)};
            constexpr std::array<std::array<float, 2>, 4> Centres{
                {{0.5f, 0.5f}, {1.5f, 0.5f}, {0.5f, 1.5f}, {1.5f, 1.5f}}};
            Id joint = ctx.ConstF32(0.0f, 0.0f, 0.0f, 0.0f);
            Id plain = joint;
            Id sum = o.F(0.0f);
            for (u32 k = 0; k < 4; ++k) {
                const Id at = ctx.OpFMul(
                    ctx.F32[2],
                    ctx.OpFAdd(ctx.F32[2], i, ctx.ConstF32(Centres[k][0], Centres[k][1])), inv);
                const Id c = ReflSample(ctx, ru.colour_image, at);
                const Id w = o.Mul(wb[k], wd[k]);
                joint = ctx.OpFAdd(ctx.F32[4], joint, ctx.OpVectorTimesScalar(ctx.F32[4], c, w));
                plain = ctx.OpFAdd(ctx.F32[4], plain, ctx.OpVectorTimesScalar(ctx.F32[4], c, wb[k]));
                sum = o.Add(sum, w);
            }
            const Id sharp = ctx.OpVectorTimesScalar(
                ctx.F32[4], joint, o.Div(one, o.Max(sum, o.F(Tune::UpsampleMinSum))));
            // The mip-0 share of the game's sample (1 - lod) swapped: plain bilinear out, ours in.
            const Id out = ctx.OpFAdd(
                ctx.F32[4], bilinear,
                ctx.OpVectorTimesScalar(ctx.F32[4], ctx.OpFSub(ctx.F32[4], sharp, plain),
                                        o.Sat(o.Sub(one, lod))));
            Id all_finite = o.Gt(sum, o.F(Tune::UpsampleMinSum));
            for (u32 k = 0; k < 4; ++k) {
                all_finite = o.And(all_finite, o.Finite(o.X(out, k)));
            }
            ctx.OpStore(result, ctx.OpSelect(ctx.F32[4], all_finite, out, bilinear));
        });
    });
    return ctx.OpLoad(ctx.F32[4], result);
}

} // namespace Shader::Backend::SPIRV
