// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// VrSpecularAa (ir/passes/specular_aa.h, SpecularAaPass): the surface's gloss g widened by the
// variance of the G-buffer normal across the pixel. The normal's fine derivatives (a light pass
// draws over the G-buffer one texel per pixel, so they are the neighbour texels' differences),
// each axis left out where the view depth jumps across the pixel (a silhouette: two surfaces),
// make a Beckmann roughness k added to the lobe's own; the result is the gloss whose specular
// power exp2(13 g') has that lobe. SpecularAaPass places it in the entry block, before any branch
// of the game's: the derivatives see the whole quad (the light tiles' per-eye test demotes its
// pixels to helpers, which still take part).

#include <array>
#include <cmath>
#include <cstdlib>

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/emit_spirv_light_common.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/passes/specular_aa.h"

namespace Shader::Backend::SPIRV {
namespace {

namespace Tune = Shader::Optimization::SpecularAa;

// A test knob (specular_aa.h): `name`'s number when it is one and not negative, else `fallback`.
float Knob(const char* name, float fallback) {
    const char* const env = std::getenv(name);
    if (env == nullptr) {
        return fallback;
    }
    char* end = nullptr;
    const float value = std::strtof(env, &end);
    return end != env && std::isfinite(value) && value >= 0.0f ? value : fallback;
}

} // namespace

Id EmitVrSpecularAa(EmitContext& ctx, Id gloss, Id normal, Id depth, u32 matrix) {
    static const float strength = Knob("VR_SPEC_AA_STRENGTH", Tune::Strength);
    static const float kappa = Knob("VR_SPEC_AA_KAPPA", Tune::Kappa);
    ctx.AddCapability(spv::Capability::DerivativeControl); // core Vulkan, fragment stages
    const Id f1 = ctx.F32[1];
    const auto F = [&](float v) { return ctx.ConstF32(v); };
    const auto read = [&](u32 dword) {
        return ctx.OpBitcast(f1, EmitReadConstBuffer(ctx, matrix, ctx.ConstU32(dword)));
    };
    const auto row = [&](u32 r) {
        return ctx.OpCompositeConstruct(
            ctx.F32[4], std::array{read(4 * r), read(4 * r + 1), read(4 * r + 2), read(4 * r + 3)});
    };
    // The pixel's view depth: rows 2 and 3 of the ndc-to-view matrix (dwords 8..15) at its depth
    // and ndc 0 (a perspective inverse's z and w rows take no x or y: the same on the whole ray;
    // the game computes its ndc after its first use of the gloss in the composite). Not a number
    // (the sky, w = 0): far away, so the sky's edge counts as a silhouette.
    const Id pos =
        ctx.OpCompositeConstruct(ctx.F32[4], std::array{F(0.0f), F(0.0f), depth, F(1.0f)});
    Id z = ctx.OpFDiv(f1, ctx.OpDot(f1, row(2), pos), ctx.OpDot(f1, row(3), pos));
    z = Finite(ctx, z, F(1e6f));
    // The normal (2 s - 1 of its UNORM sample: a number) and its differences across the pixel.
    const Id s3 = ctx.OpCompositeConstruct(
        ctx.F32[3], std::array{ctx.OpCompositeExtract(f1, normal, 0u),
                               ctx.OpCompositeExtract(f1, normal, 1u),
                               ctx.OpCompositeExtract(f1, normal, 2u)});
    const Id n = ctx.OpFSub(ctx.F32[3], ctx.OpVectorTimesScalar(ctx.F32[3], s3, F(2.0f)),
                            ctx.ConstF32(1.0f, 1.0f, 1.0f));
    const Id dnx = ctx.OpDPdxFine(ctx.F32[3], n);
    const Id dny = ctx.OpDPdyFine(ctx.F32[3], n);
    // An axis across a silhouette adds nothing (z is a number, so its differences are too).
    const Id limit = ctx.OpFMul(f1, ctx.OpFAbs(f1, z), F(Tune::EdgeRel));
    const auto axis = [&](Id dz, Id dn) {
        const Id same_surface = ctx.OpFOrdLessThanEqual(ctx.U1[1], ctx.OpFAbs(f1, dz), limit);
        return ctx.OpSelect(f1, same_surface, ctx.OpDot(f1, dn, dn), F(0.0f));
    };
    const Id variance = ctx.OpFAdd(f1, axis(ctx.OpDPdxFine(f1, z), dnx),
                                   axis(ctx.OpDPdyFine(f1, z), dny));
    // k = Strength * clamp(2 Sigma2 variance - Floor, 0, Kappa).
    Id k = ctx.OpFSub(f1, ctx.OpFMul(f1, variance, F(2.0f * Tune::Sigma2)), F(Tune::Floor));
    k = ctx.OpFMul(f1, FiniteClamp(ctx, k, 0.0f, kappa), F(strength));
    // The gloss (a UNORM G-buffer channel; kept within 0..1 for the arithmetic) as a power, as a
    // roughness, widened, and back: n0 <= 8192, so a2 > 2 / 8194 and n1 >= 1.
    const Id g = FiniteClamp(ctx, gloss, 0.0f, 1.0f);
    const Id n0 = ctx.OpExp2(f1, ctx.OpFMul(f1, g, F(13.0f)));
    const Id a2 = ctx.OpFAdd(f1, ctx.OpFDiv(f1, F(2.0f), ctx.OpFAdd(f1, n0, F(2.0f))), k);
    const Id n1 = ctx.OpFMax(f1, ctx.OpFSub(f1, ctx.OpFDiv(f1, F(2.0f), a2), F(2.0f)), F(1.0f));
    const Id widened = ctx.OpFMul(f1, ctx.OpLog2(f1, n1), F(1.0f / 13.0f));
    // Only ever rougher; on a gloss that is not a number, the game's own (both sides compared
    // are numbers).
    return ctx.OpSelect(f1, ctx.OpFOrdLessThan(ctx.U1[1], widened, g), widened, gloss);
}

} // namespace Shader::Backend::SPIRV
