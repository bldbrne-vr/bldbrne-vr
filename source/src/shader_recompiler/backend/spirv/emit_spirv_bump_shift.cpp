// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Raised brickwork (ir/passes/parallax.h BumpGaps, BumpHeightPass): parallax occlusion on the
// bump maps whose blue is a height map, without a tangent frame. The pixel's world position
// changes across the pixel by dp1 = -d(view)/dx and dp2 = -d(view)/dy (`view` is the camera minus
// the pixel); its texture coordinates by duv1, duv2. On the surface's plane (n = dp1 x dp2) the
// gradient of a coordinate is g = (duv1 (dp2 x n) + duv2 (n x dp1)) / (n . n), so for the unit
// view V g . V = (duv1 a2 + duv2 a1) / (n . n), a2 = n . (V x dp2), a1 = n . (dp1 x V): three cross
// products, three dots, one divide. A point `delta` below the surface along the view shows
// delta (g . V) / cos away.
// Everything here runs only for a draw whose bump map is a height map: its pushed depth
// (VrPushData::bump_depth) is above 0. That test is the same for every pixel of a draw, so the
// screen derivatives and the plain reads behind it stay defined; any other draw skips the lot.

#include <array>

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/emit_spirv_light_common.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/passes/parallax.h"
#include "shader_recompiler/vr_push_data.h"
#include "video_core/texture_cache/bump_height_derive.h"

namespace Shader::Backend::SPIRV {

namespace {

namespace Tune = Shader::Optimization::BumpGaps;

Id PrivateVariable(EmitContext& ctx, Id type) {
    const Id v = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Private, type),
                                       spv::StorageClass::Private);
    ctx.interfaces.push_back(v);
    return v;
}

// The draw moves: its pushed depth is above 0 (a height map, with the setting on).
Id DrawMoves(EmitContext& ctx) {
    return ctx.OpFOrdGreaterThan(ctx.U1[1], ctx.PushF32(VrPushData::BumpDepthIndex),
                                 ctx.ConstF32(0.0f));
}

// A blue as depth: 0 at FaceTop and above (a face), 1 at FloorBlue and below.
Id DepthOf(EmitContext& ctx, Id height) {
    const Id f1 = ctx.F32[1];
    return ctx.OpFClamp(
        f1,
        ctx.OpFMul(f1, ctx.OpFSub(f1, ctx.ConstF32(Tune::FaceTop), height),
                   ctx.ConstF32(VideoCore::BumpHeight::DepthScale)),
        ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
}

// The bump map's image and sampler, loaded once; Sampled() pairs them where they are read (SPIR-V:
// an OpSampledImage must be in the block that uses it).
struct BumpMap {
    EmitContext& ctx;
    Id image;
    Id sampler;
    Id sampled_type;
    Id result_type;
    bool is_integer;

    Id Sampled() const {
        return ctx.NativeNonUniform(ctx.OpSampledImage(sampled_type, image, sampler));
    }
};

BumpMap LoadBumpMap(EmitContext& ctx, u32 handle) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.NativeNonUniform(
        ctx.OpLoad(texture.image_type, ctx.NativeImagePointer(texture.image_type, texture.id)));
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    return {ctx, image, sampler, texture.sampled_type, texture.data_types->Get(4),
            texture.is_integer};
}

} // namespace

// VrBumpSeed: {the bump map's blue at `at`, the fade there}: 1 up to the texture detail level
// FadeStart, 0 from FadeEnd on. {1, 0} (a face, faded out) for a draw that does not move, without
// a read.
Id EmitVrBumpSeed(EmitContext& ctx, u32 handle, Id at) {
    const Id f1 = ctx.F32[1], f2 = ctx.F32[2];
    const Id still = ctx.ConstF32(1.0f, 0.0f);
    if (!ctx.UsesVrPushData()) {
        return still;
    }
    const Id seed = PrivateVariable(ctx, f2);
    ctx.OpStore(seed, still);
    // (The test first: nothing may stand between the merge and its branch.)
    const Id moves = DrawMoves(ctx);
    const Id body = ctx.OpLabel(), merge = ctx.OpLabel();
    ctx.OpSelectionMerge(merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(moves, body, merge);
    ctx.AddLabel(body);
    {
        const BumpMap bump = LoadBumpMap(ctx, handle);
        const Id sampled = bump.Sampled();
        Id texel = ctx.OpImageSampleImplicitLod(bump.result_type, sampled, at);
        if (bump.is_integer) {
            texel = ctx.OpBitcast(ctx.F32[4], texel);
        }
        ctx.AddCapability(spv::Capability::ImageQuery);
        // (Finite's bit test: a driver may fold OpIsNan away. No number: faded out.)
        const Id lod =
            Finite(ctx, ctx.OpCompositeExtract(f1, ctx.OpImageQueryLod(f2, sampled, at), 1u),
                   ctx.ConstF32(Tune::FadeEnd));
        const Id near = ctx.OpFClamp(
            f1,
            ctx.OpFMul(f1, ctx.OpFSub(f1, ctx.ConstF32(Tune::FadeEnd), lod),
                       ctx.ConstF32(1.0f / (Tune::FadeEnd - Tune::FadeStart))),
            ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
        ctx.OpStore(seed, ctx.OpCompositeConstruct(
                              f2, std::array{ctx.OpCompositeExtract(f1, texel, 2u), near}));
    }
    ctx.OpBranch(merge);
    ctx.AddLabel(merge);
    return ctx.OpLoad(f2, seed);
}

// VrBumpDepth: a blue as depth, 0..1; 0 for a draw that does not move and without the push data.
Id EmitVrBumpDepth(EmitContext& ctx, Id height) {
    if (!ctx.UsesVrPushData()) {
        return ctx.ConstF32(0.0f);
    }
    return ctx.OpSelect(ctx.F32[1], DrawMoves(ctx), DepthOf(ctx, height), ctx.ConstF32(0.0f));
}

// VrBumpMarch: parallax occlusion on the bump map `handle`. `view` is the camera minus the pixel
// (world), `at` the bump map's coordinates, `base` the coordinates every read of the material is
// made from (`at` is them scaled and offset), `height` the bump map's blue at `at` and `near` the
// fade (VrBumpSeed's). Returns the move of `base` to the spot where the view ray meets the height
// map; 0 for a face, a faded pixel, a non-number and a draw that does not move.
Id EmitVrBumpMarch(EmitContext& ctx, u32 handle, Id view, Id at, Id base, Id height, Id near) {
    const Id f1 = ctx.F32[1], f2 = ctx.F32[2], f3 = ctx.F32[3];
    const Id zero2 = ctx.ConstF32(0.0f, 0.0f);
    if (!ctx.UsesVrPushData()) {
        return zero2;
    }
    ctx.AddCapability(spv::Capability::DerivativeControl);
    const auto F = [&](float v) { return ctx.ConstF32(v); };
    const auto c = [&](Id v, u32 i) { return ctx.OpCompositeExtract(f1, v, i); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(f1, a, b); };
    const auto sub = [&](Id a, Id b) { return ctx.OpFSub(f1, a, b); };
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(f1, a, b); };
    const auto cross = [&](Id a, Id b) {
        return ctx.OpCompositeConstruct(
            f3, std::array{sub(mul(c(a, 1), c(b, 2)), mul(c(a, 2), c(b, 1))),
                           sub(mul(c(a, 2), c(b, 0)), mul(c(a, 0), c(b, 2))),
                           sub(mul(c(a, 0), c(b, 1)), mul(c(a, 1), c(b, 0)))});
    };
    const Id move = PrivateVariable(ctx, f2);
    const Id frac = PrivateVariable(ctx, f1), t_var = PrivateVariable(ctx, f1),
             d_var = PrivateVariable(ctx, f1), tp_var = PrivateVariable(ctx, f1),
             dp_var = PrivateVariable(ctx, f1), i_var = PrivateVariable(ctx, ctx.U32[1]);
    ctx.OpStore(move, zero2);
    const Id moves = DrawMoves(ctx);
    const Id moving = ctx.OpLabel(), still = ctx.OpLabel();
    ctx.OpSelectionMerge(still, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(moves, moving, still);
    ctx.AddLabel(moving);
    {
        // Every derivative here, before the per-pixel branch: the pixel's quad is whole.
        const Id dp1 = ctx.OpDPdxFine(f3, view);
        const Id dp2 = ctx.OpDPdyFine(f3, view);
        const Id dat1 = ctx.OpDPdxFine(f2, at);
        const Id dat2 = ctx.OpDPdyFine(f2, at);
        const Id dbs1 = ctx.OpDPdxFine(f2, base);
        const Id dbs2 = ctx.OpDPdyFine(f2, base);
        // A face (no depth) is the surface itself and a faded pixel shows none: nothing more
        // for them (most pixels, and every pixel of a flat blue drawn on this path).
        const Id d0 = DepthOf(ctx, height);
        const Id joint = ctx.OpLogicalAnd(
            ctx.U1[1], ctx.OpFOrdGreaterThan(ctx.U1[1], d0, F(1e-3f)),
            ctx.OpFOrdGreaterThan(ctx.U1[1], near, F(0.0f)));
        const Id in_joint = ctx.OpLabel(), face = ctx.OpLabel();
        ctx.OpSelectionMerge(face, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(joint, in_joint, face);
        ctx.AddLabel(in_joint);
        const Id v_unit = ctx.OpVectorTimesScalar(
            f3, view,
            ctx.OpInverseSqrt(f1, ctx.OpFMax(f1, ctx.OpDot(f1, view, view), F(1e-12f))));
        const Id n = cross(dp1, dp2);
        const Id nn = ctx.OpFMax(f1, ctx.OpDot(f1, n, n), F(1e-30f));
        const Id a2 = ctx.OpDot(f1, n, cross(v_unit, dp2));
        const Id a1 = ctx.OpDot(f1, n, cross(dp1, v_unit));
        // The view's angle to the surface: the ray below it is 1 / cos longer (held at
        // PomMinCos). Its depth: the draw's, faded.
        const Id cosv = mul(ctx.OpFAbs(f1, ctx.OpDot(f1, v_unit, n)), ctx.OpInverseSqrt(f1, nn));
        const Id amount = mul(near, ctx.PushF32(VrPushData::BumpDepthIndex));
        const Id k = ctx.OpFDiv(f1, amount, mul(ctx.OpFMax(f1, cosv, F(Tune::PomMinCos)), nn));
        const auto along = [&](Id d1, Id d2) {
            return ctx.OpVectorTimesScalar(
                f2,
                ctx.OpFAdd(f2, ctx.OpVectorTimesScalar(f2, d1, a2),
                           ctx.OpVectorTimesScalar(f2, d2, a1)),
                k);
        };
        const Id p_at = along(dat1, dat2);
        const Id p_base = along(dbs1, dbs2);
        // The whole ray's length on screen (the bump coordinates' change across a pixel,
        // inverted), held softly under PomMaxPixels with its direction kept.
        const Id det = sub(mul(c(dat1, 0), c(dat2, 1)), mul(c(dat2, 0), c(dat1, 1)));
        const Id px = sub(mul(c(dat2, 1), c(p_at, 0)), mul(c(dat2, 0), c(p_at, 1)));
        const Id py = sub(mul(c(dat1, 0), c(p_at, 1)), mul(c(dat1, 1), c(p_at, 0)));
        const Id len2 = ctx.OpFDiv(f1, add(mul(px, px), mul(py, py)),
                                   ctx.OpFMax(f1, mul(det, det), F(1e-30f)));
        const Id held = ctx.OpInverseSqrt(
            f1, add(F(1.0f), mul(len2, F(1.0f / (Tune::PomMaxPixels * Tune::PomMaxPixels)))));
        // (Finite's bit test: a driver may fold OpIsNan away. No number: no march.)
        const Id length = Finite(ctx, mul(ctx.OpSqrt(f1, len2), held), F(0.0f));
        const Id steps =
            ctx.OpFClamp(f1, ctx.OpCeil(f1, mul(length, F(1.0f / Tune::PomPixelsPerStep))),
                         F(float(Tune::PomMinSteps)), F(float(Tune::PomMaxSteps)));
        const Id ray = ctx.OpVectorTimesScalar(f2, p_at, held);
        // A ray under PomMinPixels shows nothing.
        const Id go = ctx.OpFOrdGreaterThan(ctx.U1[1], length, F(Tune::PomMinPixels));
        ctx.OpStore(frac, F(0.0f));
        const Id run = ctx.OpLabel(), done = ctx.OpLabel();
        ctx.OpSelectionMerge(done, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(go, run, done);
        ctx.AddLabel(run);
        {
            const BumpMap bump = LoadBumpMap(ctx, handle);
            const Id count = ctx.OpConvertFToU(ctx.U32[1], steps);
            const Id step = ctx.OpFDiv(f1, F(1.0f), steps);
            ctx.OpStore(t_var, F(0.0f));
            ctx.OpStore(d_var, d0);
            ctx.OpStore(tp_var, F(0.0f));
            ctx.OpStore(dp_var, d0);
            ctx.OpStore(i_var, ctx.u32_zero_value);
            const Id header = ctx.OpLabel(), body = ctx.OpLabel(), cont = ctx.OpLabel(),
                     merge = ctx.OpLabel();
            ctx.OpBranch(header);
            ctx.AddLabel(header);
            const Id i = ctx.OpLoad(ctx.U32[1], i_var);
            // Above the height map while the ray's depth is less than the map's there.
            const Id more = ctx.OpLogicalAnd(
                ctx.U1[1], ctx.OpULessThan(ctx.U1[1], i, count),
                ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpLoad(f1, t_var), ctx.OpLoad(f1, d_var)));
            ctx.OpLoopMerge(merge, cont, spv::LoopControlMask::MaskNone);
            ctx.OpBranchConditional(more, body, merge);
            ctx.AddLabel(body);
            ctx.OpStore(tp_var, ctx.OpLoad(f1, t_var));
            ctx.OpStore(dp_var, ctx.OpLoad(f1, d_var));
            const Id next = ctx.OpIAdd(ctx.U32[1], i, ctx.ConstU32(1u));
            const Id t_next = mul(ctx.OpConvertUToF(f1, next), step);
            const Id uv = ctx.OpFAdd(f2, at, ctx.OpVectorTimesScalar(f2, ray, t_next));
            Id texel = ctx.OpImageSampleExplicitLod(bump.result_type, bump.Sampled(), uv,
                                                    spv::ImageOperandsMask::Grad, dat1, dat2);
            if (bump.is_integer) {
                texel = ctx.OpBitcast(ctx.F32[4], texel);
            }
            ctx.OpStore(t_var, t_next);
            ctx.OpStore(d_var, DepthOf(ctx, c(texel, 2)));
            ctx.OpStore(i_var, next);
            ctx.OpBranch(cont);
            ctx.AddLabel(cont);
            ctx.OpBranch(header);
            ctx.AddLabel(merge);
            // The crossing between the last two steps: t - d went from below 0 to 0 or above.
            const Id t = ctx.OpLoad(f1, t_var), d = ctx.OpLoad(f1, d_var);
            const Id tp = ctx.OpLoad(f1, tp_var), dp = ctx.OpLoad(f1, dp_var);
            const Id after = sub(t, d), before = sub(tp, dp);
            const Id hit = sub(t, ctx.OpFDiv(f1, mul(after, sub(t, tp)),
                                             ctx.OpFMax(f1, sub(after, before), F(1e-6f))));
            const Id crossed = ctx.OpFOrdGreaterThanEqual(ctx.U1[1], t, d);
            ctx.OpStore(frac,
                        ctx.OpFClamp(f1, ctx.OpSelect(f1, crossed, hit, t), F(0.0f), F(1.0f)));
        }
        ctx.OpBranch(done);
        ctx.AddLabel(done);
        ctx.OpStore(move, Finite(ctx,
                                 ctx.OpVectorTimesScalar(f2, p_base,
                                                         mul(held, ctx.OpLoad(f1, frac))),
                                 zero2, 2));
        ctx.OpBranch(face);
        ctx.AddLabel(face);
    }
    ctx.OpBranch(still);
    ctx.AddLabel(still);
    return ctx.OpLoad(f2, move);
}

} // namespace Shader::Backend::SPIRV
