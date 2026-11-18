// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// VrParticleAir (ir/passes/particle_air.h): the light the frame's strongest lights give a soft
// particle's fragment. The fragment's view position from the vector to the camera in world axes
// (the buffer's world-to-view rotation: p = -R e), then per light (a loop over the buffer's
// count, uniform: the condition before OpLoopMerge) the game's point-light falloff at it,
// 1 / (d^2 + CoreM^2) - reach (at least 0), times a mild forward phase, times the light's air
// colour; the sum times the smoke's albedo (its own hue and brightness) and the strength; 0 for
// smoke past MaxEyeM; finite and within 0..MaxAir. The buffer is read from its first descriptor
// (both eyes' are the same upload): no NonUniform; no noise, so both eyes see the same light.

#include <algorithm>
#include <array>

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/emit_spirv_light_common.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/passes/particle_air.h"

namespace Shader::Backend::SPIRV {
namespace {

namespace Air = Shader::Optimization::ParticleAir;

Id PrivateAir(EmitContext& ctx, Id type) {
    const Id var = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Private, type),
                                         spv::StorageClass::Private);
    ctx.interfaces.push_back(var);
    return var;
}

// A dword of the lights' buffer, from its first descriptor (as the fog lights' FogRead).
Id AirRead(EmitContext& ctx, u32 handle, Id index) {
    const auto& buffer = ctx.buffers[handle];
    if (const Id offset = buffer.Offset(EmitContext::PointerSize::B32); Sirit::ValidId(offset)) {
        index = ctx.OpIAdd(ctx.U32[1], index, offset);
    }
    const auto [id, pointer_type] = buffer.Alias(EmitContext::PointerType::U32);
    const Id ptr = ctx.runtime_info.native_stereo
                       ? ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value,
                                           ctx.u32_zero_value, index)
                       : ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, index);
    return ctx.OpLoad(ctx.U32[1], ptr);
}

Id AirF(EmitContext& ctx, u32 handle, Id index) {
    return ctx.OpBitcast(ctx.F32[1], AirRead(ctx, handle, index));
}

} // namespace

Id EmitVrParticleAir(EmitContext& ctx, Id e, Id rgb) {
    const auto& buffers = ctx.info.buffers;
    const auto found =
        std::ranges::find(buffers, BufferType::VrParticleLights, &BufferResource::buffer_type);
    if (found == buffers.end()) {
        return ctx.ConstF32(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const u32 buf = static_cast<u32>(std::distance(buffers.begin(), found));
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(ctx.F32[1], a, b); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(ctx.F32[1], a, b); };
    const auto sub = [&](Id a, Id b) { return ctx.OpFSub(ctx.F32[1], a, b); };
    const auto f = [&](u32 dword) { return AirF(ctx, buf, ctx.ConstU32(dword)); };
    const Id count =
        ctx.OpUMin(ctx.U32[1], AirRead(ctx, buf, ctx.u32_zero_value), ctx.ConstU32(Air::MaxLights));
    const Id strength = f(1);
    // The fragment from the camera in the flat camera's view axes: -R e.
    std::array<Id, 3> rows;
    for (u32 r = 0; r < 3; ++r) {
        const u32 at = Air::RowsDword + 4 * r;
        rows[r] = ctx.OpCompositeConstruct(ctx.F32[3], std::array{f(at), f(at + 1), f(at + 2)});
    }
    // Finite, else far away (no light).
    const Id e_safe = Finite(ctx, e, ctx.ConstF32(1e4f, 1e4f, 1e4f), 3);
    const Id er = ctx.OpCompositeConstruct(
        ctx.F32[3], std::array{ctx.OpDot(ctx.F32[1], rows[0], e_safe),
                               ctx.OpDot(ctx.F32[1], rows[1], e_safe),
                               ctx.OpDot(ctx.F32[1], rows[2], e_safe)});
    const Id e2 = ctx.OpDot(ctx.F32[1], e_safe, e_safe);
    const Id inv_e = ctx.OpInverseSqrt(ctx.F32[1], ctx.OpFMax(ctx.F32[1], e2, ctx.ConstF32(1e-8f)));
    const float g = Air::PhaseG;
    const Id sum = PrivateAir(ctx, ctx.F32[3]);
    const Id index = PrivateAir(ctx, ctx.U32[1]);
    ctx.OpStore(sum, ctx.ConstF32(0.0f, 0.0f, 0.0f));
    ctx.OpStore(index, ctx.u32_zero_value);
    const Id header = ctx.OpLabel(), body = ctx.OpLabel(), cont = ctx.OpLabel(),
             merge = ctx.OpLabel();
    ctx.OpBranch(header);
    ctx.AddLabel(header);
    const Id i = ctx.OpLoad(ctx.U32[1], index);
    // The condition before the merge: OpLoopMerge must be just before the block's branch.
    const Id go = ctx.OpULessThan(ctx.U1[1], i, count);
    ctx.OpLoopMerge(merge, cont, spv::LoopControlMask::MaskNone);
    ctx.OpBranchConditional(go, body, merge);
    ctx.AddLabel(body);
    {
        const Id base = ctx.OpIAdd(ctx.U32[1], ctx.ConstU32(Air::HeaderDwords),
                                   ctx.OpIMul(ctx.U32[1], i, ctx.ConstU32(Air::LightDwords)));
        const auto at = [&](u32 k) {
            return AirF(ctx, buf, ctx.OpIAdd(ctx.U32[1], base, ctx.ConstU32(k)));
        };
        const Id l = ctx.OpCompositeConstruct(ctx.F32[3], std::array{at(0), at(1), at(2)});
        // The fragment to the light: l - (-R e).
        const Id v = ctx.OpFAdd(ctx.F32[3], l, er);
        const Id d2 = ctx.OpDot(ctx.F32[1], v, v);
        const Id falloff = ctx.OpFMax(
            ctx.F32[1],
            sub(ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f),
                           add(d2, FiniteClamp(ctx, at(7), Air::CoreM * Air::CoreM, 64.0f))),
                at(3)),
            ctx.ConstF32(0.0f));
        // The light's way on (light to fragment, -v) against the way to the eye (R e): 1 when
        // the light is behind the smoke.
        const Id cosine = mul(mul(ctx.OpFNegate(ctx.F32[1], ctx.OpDot(ctx.F32[1], v, er)),
                                  ctx.OpInverseSqrt(ctx.F32[1],
                                                    ctx.OpFMax(ctx.F32[1], d2, ctx.ConstF32(1e-8f)))),
                              inv_e);
        // Henyey-Greenstein (1 - g^2) / k^1.5, k = 1 + g^2 - 2 g cos; half of it constant, capped.
        const Id k = ctx.OpFMax(ctx.F32[1],
                                sub(ctx.ConstF32(1.0f + g * g), mul(ctx.ConstF32(2.0f * g), cosine)),
                                ctx.ConstF32(1e-4f));
        const Id hg = ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f - g * g),
                                 mul(k, ctx.OpSqrt(ctx.F32[1], k)));
        const Id phase = ctx.OpFMin(ctx.F32[1], ctx.OpFma(ctx.F32[1], hg, ctx.ConstF32(0.5f), ctx.ConstF32(0.5f)),
                                    ctx.ConstF32(Air::PhaseMax));
        const Id colour = ctx.OpCompositeConstruct(ctx.F32[3], std::array{at(4), at(5), at(6)});
        ctx.OpStore(sum, ctx.OpFAdd(ctx.F32[3], ctx.OpLoad(ctx.F32[3], sum),
                                    ctx.OpVectorTimesScalar(ctx.F32[3], colour, mul(falloff, phase))));
    }
    ctx.OpStore(index, ctx.OpIAdd(ctx.U32[1], i, ctx.ConstU32(1u)));
    ctx.OpBranch(cont);
    ctx.AddLabel(cont);
    ctx.OpBranch(header);
    ctx.AddLabel(merge);
    // The smoke's albedo: its hue (grey stays grey) times its brightness within Floor..1.
    const Id c = FiniteClamp(ctx, rgb, 0.0f, 64.0f, 3);
    const Id lift = ctx.ConstF32(Air::Lift);
    const Id top = add(ctx.OpFMax(ctx.F32[1], ctx.OpCompositeExtract(ctx.F32[1], c, 0u),
                                  ctx.OpFMax(ctx.F32[1], ctx.OpCompositeExtract(ctx.F32[1], c, 1u),
                                             ctx.OpCompositeExtract(ctx.F32[1], c, 2u))),
                       lift);
    const Id hue = ctx.OpVectorTimesScalar(ctx.F32[3],
                                           ctx.OpFAdd(ctx.F32[3], c, ctx.ConstF32(Air::Lift, Air::Lift, Air::Lift)),
                                           ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), top));
    const Id luma = ctx.OpDot(ctx.F32[1], c, ctx.ConstF32(0.2126f, 0.7152f, 0.0722f));
    const Id bright = ctx.OpFClamp(ctx.F32[1], mul(luma, ctx.ConstF32(Air::Gain)),
                                   ctx.ConstF32(Air::Floor), ctx.ConstF32(1.0f));
    // Near enough to the eye, and the strength a number within 0..4.
    const Id near = ctx.OpFOrdLessThan(ctx.U1[1], e2, ctx.ConstF32(Air::MaxEyeM * Air::MaxEyeM));
    const Id scale = ctx.OpSelect(
        ctx.F32[1], near,
        mul(mul(FiniteClamp(ctx, strength, 0.0f, 4.0f), ctx.ConstF32(Air::Albedo)), bright),
        ctx.ConstF32(0.0f));
    const Id lit = ctx.OpFMul(ctx.F32[3], hue, ctx.OpLoad(ctx.F32[3], sum));
    const Id air = FiniteClamp(ctx, ctx.OpVectorTimesScalar(ctx.F32[3], lit, scale), 0.0f,
                               Air::MaxAir, 3);
    return ctx.OpCompositeConstruct(ctx.F32[4],
                                    std::array{ctx.OpCompositeExtract(ctx.F32[1], air, 0u),
                                               ctx.OpCompositeExtract(ctx.F32[1], air, 1u),
                                               ctx.OpCompositeExtract(ctx.F32[1], air, 2u),
                                               ctx.ConstF32(0.0f)});
}

} // namespace Shader::Backend::SPIRV
