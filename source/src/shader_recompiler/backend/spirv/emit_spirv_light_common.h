// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {

// The VR lighting's one-time setup in a shader (emit_spirv_light_shadow.cpp): the pixel's view
// position (ctx.vr_light_shadow.p), the ndc-to-coordinates line (a, b, ok), the pixel's size and
// a per-pixel jitter, from the matrix and depth that Info::vr_light_shadow names. Done once; a
// second call does nothing.
void VrLightSetup(EmitContext& ctx, Id ndc, Id uv, Id depth);

// The lights in the air (ir/passes/fog_light.h) in two parts: `air_part`, their glow along the
// pixel's ray; `surface_part`, the flames' fill and the muzzle flash on the pixel's surface (the
// light composite's inputs). The air pass (air_light.h) gives the glow in the scene's copy, after
// the particles; `vote`: its source check, called per light in the glow's loop.
struct VrFogVote {
    Id on;       // bool: this fragment votes
    Id lum_code; // u32: the scene's brightness here
    Id base;     // u32: the first light's dword in the probe's buffer
    Id length;   // the surface's distance along the ray
    u32 probe;   // ctx.buffers index
};
Id VrFogLightParts(EmitContext& ctx, Id ndc, Id uv, Id depth, bool air_part, bool surface_part,
                   const VrFogVote* vote);
// emit_spirv_air_light.cpp: light `light` (u32) passes `off2` (squared) from this pixel's ray,
// `along` it.
void VrAirSourceVote(EmitContext& ctx, const VrFogVote& vote, Id light, Id along, Id off2);

// x (n = 1 to 4 components) where it is a number, else `fallback` (2026-11-11: no NaN or Inf
// may reach the upscaler, whose history spreads one into a lasting black blot). The test is on
// the exponent's bits as integers: the shaders declare no SignedZeroInfNanPreserve, so a driver
// may fold OpIsNan, an ordered compare or an FMax/FClamp as if there were no NaN; integer bits
// it cannot.
inline Id Finite(EmitContext& ctx, Id x, Id fallback, u32 n = 1) {
    const u32 e = 0x7f800000u;
    const Id mask = n == 1   ? ctx.ConstU32(e)
                    : n == 2 ? ctx.ConstU32(e, e)
                    : n == 3 ? ctx.ConstU32(e, e, e)
                             : ctx.ConstU32(e, e, e, e);
    const Id bits = ctx.OpBitcast(ctx.U32[n], x);
    const Id ok = ctx.OpINotEqual(ctx.U1[n], ctx.OpBitwiseAnd(ctx.U32[n], bits, mask), mask);
    return ctx.OpSelect(ctx.F32[n], ok, x, fallback);
}

// Finite (else 0), then within [lo, hi] (FClamp is exact on a number).
inline Id FiniteClamp(EmitContext& ctx, Id x, float lo, float hi, u32 n = 1) {
    const auto splat = [&](float v) {
        return n == 1   ? ctx.ConstF32(v)
               : n == 2 ? ctx.ConstF32(v, v)
               : n == 3 ? ctx.ConstF32(v, v, v)
                        : ctx.ConstF32(v, v, v, v);
    };
    return ctx.OpFClamp(ctx.F32[n], Finite(ctx, x, splat(0.0f), n), splat(lo), splat(hi));
}

// v / |v| (3 components), or `fallback` (a unit vector) when v is too short or not a number.
inline Id SafeNormalize(EmitContext& ctx, Id v, Id fallback) {
    const Id len2 = ctx.OpDot(ctx.F32[1], v, v);
    const Id number = ctx.OpINotEqual(
        ctx.U1[1],
        ctx.OpBitwiseAnd(ctx.U32[1], ctx.OpBitcast(ctx.U32[1], len2), ctx.ConstU32(0x7f800000u)),
        ctx.ConstU32(0x7f800000u));
    const Id ok = ctx.OpLogicalAnd(ctx.U1[1], number,
                                   ctx.OpFOrdGreaterThan(ctx.U1[1], len2, ctx.ConstF32(1e-20f)));
    const Id safe = ctx.OpSelect(ctx.F32[1], ok, len2, ctx.ConstF32(1.0f));
    return ctx.OpSelect(ctx.F32[3], ok,
                        ctx.OpVectorTimesScalar(ctx.F32[3], v, ctx.OpInverseSqrt(ctx.F32[1], safe)),
                        fallback);
}

} // namespace Shader::Backend::SPIRV
