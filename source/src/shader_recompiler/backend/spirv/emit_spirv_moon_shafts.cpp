// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// VrMoonShafts (ir/passes/moon_shafts.h): the moonlight the air scatters towards the eye along
// this pixel's ray, through the gaps in the moon's shadow. Per pixel: the moon's colour (the
// light sets of the surfaces at a few fixed screen points, the same for every pixel), the ray
// (the view position of the VR lighting's setup), per cascade its view-to-shadow rows over the
// ray (A t + B: the eye's half shift folded into B), a jitter (interleaved gradient noise moved
// by the golden ratio every frame). Then `steps` points from StartM to the surface or the
// buffer's far distance, quadratic in t (denser near the eye); each in the cascade of its view
// z, inside that cascade's tile of the atlas, lit when its shadow depth passes the atlas' (one
// texel fetch, no filter: the upscaler's accumulation does the rest), weighted by the air's
// thickness (from NearStartM, thinning out towards far) and transmittance. The sum times the
// moon's colour, the phase and the density; w the transmittance of that air minus one (the
// composite dims the surface behind it). The buffers are read from their first descriptor (both
// eyes' are the same), every atlas fetch from the first image (both eyes' are the same atlas):
// no NonUniform.

#include <algorithm>
#include <array>

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/emit_spirv_light_common.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/passes/air_light.h"
#include "shader_recompiler/ir/passes/moon_shafts.h"

namespace Shader::Backend::SPIRV {
namespace {

namespace Moon = Shader::Optimization::MoonShafts;

Id PrivateVar(EmitContext& ctx, Id type) {
    const Id var = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Private, type),
                                         spv::StorageClass::Private);
    ctx.interfaces.push_back(var);
    return var;
}

// A dword of a buffer, from its first descriptor (as the fog lights' FogRead).
Id MoonReadAt(EmitContext& ctx, u32 handle, Id index) {
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

Id MoonRead(EmitContext& ctx, u32 handle, u32 dword) {
    return MoonReadAt(ctx, handle, ctx.ConstU32(dword));
}

Id MoonF(EmitContext& ctx, u32 handle, u32 dword) {
    return ctx.OpBitcast(ctx.F32[1], MoonRead(ctx, handle, dword));
}

struct Cascade {
    Id a, b;     // shadow coordinates (uv x, uv y, depth) = a t + b along the ray
    Id lo, size; // the tile (atlas uv) and its size
    Id bias;     // in the atlas' depth units
};

} // namespace

// `jitter` given (the air pass, air_light.h): that jitter for every step, the buffer's colour
// alone (no light-set table), and the caller's own view position (no lighting setup needed).
Id VrMoonShaftsCore(EmitContext& ctx, Id ndc, Id uv, Id depth, u32 sets_image, Id jitter_given) {
    const bool air_pass = Sirit::ValidId(jitter_given);
    const auto& info = ctx.info;
    const auto buffer = std::ranges::find(info.buffers, BufferType::VrMoonShafts,
                                          &BufferResource::buffer_type);
    const auto table = std::ranges::find_if(info.buffers, [](const BufferResource& desc) {
        return desc.IsVrHost() &&
               desc.sharp_idx == VrHostBufferBase + static_cast<u32>(VrHostBuffer::MoonSets);
    });
    const auto image = std::ranges::find_if(info.images, [](const ImageResource& desc) {
        return desc.sharp_idx == VrHostImageBase + static_cast<u32>(VrHostImage::MoonAtlas);
    });
    if (buffer == info.buffers.end() || image == info.images.end() ||
        (!air_pass && (!info.vr_light_shadow.on || table == info.buffers.end()))) {
        return ctx.ConstF32(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const u32 moon = static_cast<u32>(std::distance(info.buffers.begin(), buffer));
    const u32 atlas = static_cast<u32>(std::distance(info.images.begin(), image));
    const u32 sets = table == info.buffers.end()
                         ? ~0u
                         : static_cast<u32>(std::distance(info.buffers.begin(), table));
    VrLightSetup(ctx, ndc, uv, depth);
    const auto& s = ctx.vr_light_shadow;
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(ctx.F32[1], a, b); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(ctx.F32[1], a, b); };
    const auto sub = [&](Id a, Id b) { return ctx.OpFSub(ctx.F32[1], a, b); };
    const auto f = [&](u32 dword) { return MoonF(ctx, moon, dword); };

    const Id frame = MoonRead(ctx, moon, 1);
    const Id sign = f(2);
    const Id density = f(4);
    const Id far_m = f(5);
    const Id g = f(6);
    const Id forward = f(7);
    const Id towards = ctx.OpCompositeConstruct(ctx.F32[3], std::array{f(8), f(9), f(10)});
    const Id strength = f(11);
    const Id test_colour = ctx.OpCompositeConstruct(ctx.F32[3], std::array{f(12), f(13), f(14)});
    const std::array<Id, 4> ends{f(16), f(17), f(18), f(19)};
    const Id atlas_size = ctx.OpCompositeConstruct(ctx.F32[2], std::array{f(20), f(21)});

    // The moon's colour: light 0 of the light sets (the directional light's table, which the
    // GPU writes; light 0 is the one the moon's shadow darkens, never the sets' unshadowed second
    // light) of the surfaces at a few fixed points of the screen, weighted by 1 / distance^2 (the
    // world round you, not the hunter's own hands within NearIgnoreM, not a distant backdrop
    // with a set of its own): one colour for the whole air. Plus a test's colour. Without one,
    // no point is marched.
    const Id set_count = MoonRead(ctx, moon, 22);
    Id colour_sum = ctx.ConstF32(0.0f, 0.0f, 0.0f);
    Id weight_sum = ctx.ConstF32(0.0f);
    if (sets_image != ~0u && sets != ~0u) {
        const u32 col0 = 4 * Moon::SetColour;
        for (const auto& point : Moon::ColourPoints) {
            const Id n = ctx.ConstF32(point[0], point[1]);
            const Id at = ctx.OpFAdd(ctx.F32[2], ctx.OpFMul(ctx.F32[2], s.a, n), s.b);
            const Id d = ctx.OpCompositeExtract(
                ctx.F32[1],
                EmitImageSampleExplicitLod(ctx, nullptr, info.vr_light_shadow.depth_image, at,
                                           ctx.ConstF32(0.0f), IR::Value{}),
                0u);
            const Id v = ctx.OpCompositeConstruct(ctx.F32[4], std::array{ctx.ConstF32(point[0]),
                                                                        ctx.ConstF32(point[1]), d,
                                                                        ctx.ConstF32(1.0f)});
            std::array<Id, 4> q;
            for (u32 r = 0; r < 4; ++r) {
                q[r] = ctx.OpDot(ctx.F32[1], s.rows[r], v);
            }
            const Id inv_w = ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), q[3]);
            const Id pos = ctx.OpVectorTimesScalar(
                ctx.F32[3], ctx.OpCompositeConstruct(ctx.F32[3], std::array{q[0], q[1], q[2]}), inv_w);
            const Id dist2 = ctx.OpDot(ctx.F32[1], pos, pos);
            const Id w = ctx.OpCompositeExtract(
                ctx.F32[1],
                EmitImageSampleExplicitLod(ctx, nullptr, sets_image, at, ctx.ConstF32(0.0f),
                                           IR::Value{}),
                3u);
            const Id id_u = ctx.OpBitcast(
                ctx.U32[1], ctx.OpConvertFToS(ctx.S32[1], mul(w, ctx.ConstF32(255.0f))));
            const Id ok = ctx.OpLogicalAnd(
                ctx.U1[1], ctx.OpULessThan(ctx.U1[1], id_u, set_count),
                ctx.OpFOrdGreaterThan(ctx.U1[1], dist2,
                                      ctx.ConstF32(Moon::NearIgnoreM * Moon::NearIgnoreM)));
            const Id base = ctx.OpSelect(ctx.U32[1], ok,
                                         ctx.OpIMul(ctx.U32[1], id_u, ctx.ConstU32(Moon::SetDwords)),
                                         ctx.u32_zero_value);
            std::array<Id, 3> c;
            for (u32 k = 0; k < 3; ++k) {
                c[k] = ctx.OpBitcast(ctx.F32[1], MoonReadAt(ctx, sets, ctx.OpIAdd(ctx.U32[1], base,
                                                                                ctx.ConstU32(col0 + k))));
            }
            const Id weight = ctx.OpSelect(
                ctx.F32[1], ok,
                ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), ctx.OpFMax(ctx.F32[1], dist2, ctx.ConstF32(1.0f))),
                ctx.ConstF32(0.0f));
            colour_sum = ctx.OpFAdd(
                ctx.F32[3], colour_sum,
                ctx.OpVectorTimesScalar(ctx.F32[3],
                                        // (A table colour that is not a number counts 0,
                                        // also where its weight is 0: NaN * 0 is NaN.)
                                        FiniteClamp(ctx, ctx.OpCompositeConstruct(ctx.F32[3], c),
                                                    0.0f, 1024.0f, 3),
                                        weight));
            weight_sum = add(weight_sum, weight);
        }
    }
    const Id colour = ctx.OpFAdd(
        ctx.F32[3], test_colour,
        ctx.OpVectorTimesScalar(ctx.F32[3], colour_sum,
                                ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f),
                                           ctx.OpFMax(ctx.F32[1], weight_sum, ctx.ConstF32(1e-12f)))));
    const Id lit_air = ctx.OpFOrdGreaterThan(
        ctx.U1[1], ctx.OpDot(ctx.F32[1], colour, ctx.ConstF32(1.0f, 1.0f, 1.0f)), ctx.ConstF32(0.0f));
    const Id steps = ctx.OpSelect(ctx.U32[1], lit_air, MoonRead(ctx, moon, 0), ctx.u32_zero_value);

    // The ray: from the eye through the surface (its view position).
    const Id length = ctx.OpLength(ctx.F32[1], s.p);
    const Id dir = ctx.OpVectorTimesScalar(
        ctx.F32[3], s.p,
        ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), ctx.OpFMax(ctx.F32[1], length, ctx.ConstF32(1e-4f))));
    const Id t_end = ctx.OpFMin(ctx.F32[1], length, far_m);
    const Id inv_half_far = ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(2.0f),
                                       ctx.OpFMax(ctx.F32[1], far_m, ctx.ConstF32(1.0f)));
    const Id span = ctx.OpFMax(ctx.F32[1], sub(t_end, ctx.ConstF32(Moon::StartM)), ctx.ConstF32(0.0f));
    const Id zdir = ctx.OpCompositeExtract(ctx.F32[1], dir, 2u);
    // Native stereo: each eye's view is the flat camera's moved along its x (the fog lights' rule):
    // a point of the eye's view is at x + shift in the flat camera's, the cascades' view.
    Id shift = ctx.ConstF32(0.0f);
    if (ctx.runtime_info.native_stereo && ctx.runtime_info.native_multiview) {
        const Id half = f(3);
        shift = ctx.OpSelect(ctx.F32[1], ctx.OpIEqual(ctx.U1[1], ctx.NativeEyeIndex(), ctx.u32_zero_value),
                             ctx.OpFNegate(ctx.F32[1], half), half);
    }
    std::array<Cascade, Moon::Cascades> cascades;
    for (u32 k = 0; k < Moon::Cascades; ++k) {
        const u32 base = Moon::HeaderDwords + Moon::CascadeDwords * k;
        std::array<Id, 3> a, b;
        for (u32 r = 0; r < 3; ++r) {
            const Id row = ctx.OpCompositeConstruct(
                ctx.F32[3], std::array{f(base + 4 * r), f(base + 4 * r + 1), f(base + 4 * r + 2)});
            a[r] = ctx.OpDot(ctx.F32[1], row, dir);
            b[r] = add(f(base + 4 * r + 3), mul(f(base + 4 * r), shift));
        }
        cascades[k] = {ctx.OpCompositeConstruct(ctx.F32[3], a), ctx.OpCompositeConstruct(ctx.F32[3], b),
                       ctx.OpCompositeConstruct(ctx.F32[2], std::array{f(base + 12), f(base + 13)}),
                       f(base + 14), f(base + 15)};
    }
    // Interleaved gradient noise, moved by the golden ratio every frame.
    const Id fx = EmitGetAttribute(ctx, IR::Attribute::FragCoord, 0, 0);
    const Id fy = EmitGetAttribute(ctx, IR::Attribute::FragCoord, 1, 0);
    const Id ign = ctx.OpFract(
        ctx.F32[1],
        mul(ctx.ConstF32(52.9829189f),
            ctx.OpFract(ctx.F32[1], add(mul(fx, ctx.ConstF32(0.06711056f)),
                                        mul(fy, ctx.ConstF32(0.00583715f))))));
    const Id frame_f = ctx.OpConvertUToF(
        ctx.F32[1], ctx.OpBitwiseAnd(ctx.U32[1], frame, ctx.ConstU32(1023u)));
    const Id jitter = air_pass ? jitter_given
                               : ctx.OpFract(ctx.F32[1], add(ign, mul(frame_f, ctx.ConstF32(0.61803399f))));
    const Id steps_f = ctx.OpConvertUToF(ctx.F32[1], ctx.OpUMax(ctx.U32[1], steps, ctx.ConstU32(1u)));
    const Id inv_steps = ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), steps_f);
    const Id size_max = ctx.OpConvertFToS(
        ctx.S32[2], ctx.OpFSub(ctx.F32[2], atlas_size, ctx.ConstF32(1.0f, 1.0f)));
    const auto& texture = ctx.images[atlas];
    const Id image_ptr =
        ctx.runtime_info.native_stereo
            ? ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::UniformConstant, texture.image_type),
                                texture.id, ctx.u32_zero_value)
            : texture.id;
    const Id loaded = ctx.OpLoad(texture.image_type, image_ptr);

    const Id sum = PrivateVar(ctx, ctx.F32[1]);
    const Id path = PrivateVar(ctx, ctx.F32[1]);
    const Id index = PrivateVar(ctx, ctx.U32[1]);
    ctx.OpStore(sum, ctx.ConstF32(0.0f));
    ctx.OpStore(path, ctx.ConstF32(0.0f));
    ctx.OpStore(index, ctx.u32_zero_value);
    const Id header = ctx.OpLabel(), body = ctx.OpLabel(), cont = ctx.OpLabel(),
             merge = ctx.OpLabel();
    ctx.OpBranch(header);
    ctx.AddLabel(header);
    const Id i = ctx.OpLoad(ctx.U32[1], index);
    // The condition before the merge: OpLoopMerge must be just before the block's branch.
    const Id go = ctx.OpULessThan(ctx.U1[1], i, steps);
    ctx.OpLoopMerge(merge, cont, spv::LoopControlMask::MaskNone);
    ctx.OpBranchConditional(go, body, merge);
    ctx.AddLabel(body);
    {
        const Id u = mul(add(ctx.OpConvertUToF(ctx.F32[1], i), jitter), inv_steps);
        const Id t = add(ctx.ConstF32(Moon::StartM), mul(span, mul(u, u)));
        const Id dt = mul(mul(ctx.ConstF32(2.0f), mul(span, u)), inv_steps);
        const Id z = mul(t, zdir);
        const Id k1 = ctx.OpFOrdGreaterThan(ctx.U1[1], z, ends[0]);
        const Id k2 = ctx.OpFOrdGreaterThan(ctx.U1[1], z, ends[1]);
        const Id k3 = ctx.OpFOrdGreaterThan(ctx.U1[1], z, ends[2]);
        const auto pick = [&](Id type, auto field) {
            Id v = ctx.OpSelect(type, k1, field(cascades[1]), field(cascades[0]));
            v = ctx.OpSelect(type, k2, field(cascades[2]), v);
            return ctx.OpSelect(type, k3, field(cascades[3]), v);
        };
        const Id a = pick(ctx.F32[3], [](const Cascade& c) { return c.a; });
        const Id b = pick(ctx.F32[3], [](const Cascade& c) { return c.b; });
        const Id lo = pick(ctx.F32[2], [](const Cascade& c) { return c.lo; });
        const Id size = pick(ctx.F32[1], [](const Cascade& c) { return c.size; });
        const Id bias = pick(ctx.F32[1], [](const Cascade& c) { return c.bias; });
        const Id sc = ctx.OpFAdd(ctx.F32[3], ctx.OpVectorTimesScalar(ctx.F32[3], a, t), b);
        const Id suv = ctx.OpVectorShuffle(ctx.F32[2], sc, sc, 0u, 1u);
        const Id sd = ctx.OpCompositeExtract(ctx.F32[1], sc, 2u);
        const Id hi = ctx.OpFAdd(ctx.F32[2], lo, ctx.OpCompositeConstruct(ctx.F32[2], std::array{size, size}));
        const Id inside = ctx.OpLogicalAnd(
            ctx.U1[1],
            ctx.OpLogicalAnd(ctx.U1[1], ctx.OpAll(ctx.U1[1], ctx.OpFOrdGreaterThanEqual(ctx.U1[2], suv, lo)),
                             ctx.OpAll(ctx.U1[1], ctx.OpFOrdLessThan(ctx.U1[2], suv, hi))),
            ctx.OpFOrdLessThanEqual(ctx.U1[1], z, ends[3]));
        const Id texel = ctx.OpSClamp(
            ctx.S32[2],
            ctx.OpConvertFToS(ctx.S32[2], ctx.OpFloor(ctx.F32[2], ctx.OpFMul(ctx.F32[2], suv, atlas_size))),
            ctx.ConstS32(0, 0), size_max);
        const std::array<Id, 1> lod{ctx.ConstS32(0)};
        const Id fetched = ctx.OpImageFetch(texture.data_types->Get(4), loaded, texel,
                                            spv::ImageOperandsMask::Lod, lod);
        const Id stored = ctx.OpCompositeExtract(ctx.F32[1], fetched, 0u);
        // Lit: the point's depth passes the atlas' (the game's compare, by its sign) within the
        // bias; outside every cascade (beyond the shadows' reach) the air is lit.
        const Id passes = ctx.OpFOrdGreaterThanEqual(
            ctx.U1[1], mul(sign, sub(stored, sd)), ctx.OpFNegate(ctx.F32[1], bias));
        const Id lit = ctx.OpSelect(ctx.F32[1], ctx.OpLogicalOr(ctx.U1[1], ctx.OpLogicalNot(ctx.U1[1], inside), passes),
                                    ctx.ConstF32(1.0f), ctx.ConstF32(0.0f));
        const Id transmit = ctx.OpExp(ctx.F32[1], ctx.OpFNegate(ctx.F32[1], mul(density, t)));
        // The lit air reaches far_m, thinning out over its second half (a haze round you, not a
        // wall over the distant city: the painted sky behind it gets none).
        const Id h = ctx.OpFClamp(ctx.F32[1], mul(sub(far_m, t), inv_half_far), ctx.ConstF32(0.0f),
                                  ctx.ConstF32(1.0f));
        // And it thickens from NearStartM to NearFullM: what is at your hands stays clear.
        const Id n = ctx.OpFClamp(ctx.F32[1],
                                  mul(sub(t, ctx.ConstF32(Moon::NearStartM)),
                                      ctx.ConstF32(1.0f / (Moon::NearFullM - Moon::NearStartM))),
                                  ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
        const Id air = mul(mul(mul(h, h), sub(ctx.ConstF32(3.0f), mul(ctx.ConstF32(2.0f), h))),
                           mul(mul(n, n), sub(ctx.ConstF32(3.0f), mul(ctx.ConstF32(2.0f), n))));
        ctx.OpStore(sum, add(ctx.OpLoad(ctx.F32[1], sum), mul(mul(lit, air), mul(transmit, dt))));
        ctx.OpStore(path, add(ctx.OpLoad(ctx.F32[1], path), mul(air, dt)));
        ctx.OpStore(index, ctx.OpIAdd(ctx.U32[1], i, ctx.ConstU32(1u)));
        ctx.OpBranch(cont);
    }
    ctx.AddLabel(cont);
    ctx.OpBranch(header);
    ctx.AddLabel(merge);
    // The phase: Henyey-Greenstein g for the forward share (relative to an even scatter), the
    // rest even.
    const Id cosine = ctx.OpDot(ctx.F32[1], dir, towards);
    const Id g2 = mul(g, g);
    const Id denom = ctx.OpFMax(ctx.F32[1],
                                sub(add(ctx.ConstF32(1.0f), g2), mul(mul(ctx.ConstF32(2.0f), g), cosine)),
                                ctx.ConstF32(1e-4f));
    const Id hg = ctx.OpFDiv(ctx.F32[1], sub(ctx.ConstF32(1.0f), g2),
                             mul(denom, ctx.OpSqrt(ctx.F32[1], denom)));
    const Id phase = add(sub(ctx.ConstF32(1.0f), forward), mul(forward, hg));
    // Surfaces far beyond the lit air (the distant city against the painted sky, which gets none)
    // keep FarKeep of it.
    const Id beyond = ctx.OpFClamp(
        ctx.F32[1], ctx.OpFDiv(ctx.F32[1], sub(length, far_m), mul(far_m, ctx.ConstF32(Moon::FarFade - 1.0f))),
        ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
    const Id keep = sub(ctx.ConstF32(1.0f),
                        mul(ctx.ConstF32(1.0f - Moon::FarKeepKnob()),
                            mul(mul(beyond, beyond), sub(ctx.ConstF32(3.0f), mul(ctx.ConstF32(2.0f), beyond)))));
    const Id amount =
        mul(mul(mul(strength, density), keep), mul(phase, ctx.OpLoad(ctx.F32[1], sum)));
    const Id air = ctx.OpVectorTimesScalar(ctx.F32[3], colour, amount);
    // The air in front of the surface dims it (shadowed air: the dark pockets): w = its
    // transmittance - 1, ExtinctionShare of the density over the air's path.
    const Id dim = sub(ctx.OpExp(ctx.F32[1], ctx.OpFNegate(ctx.F32[1],
                                                           mul(mul(density, ctx.ConstF32(Moon::ExtinctionKnob())),
                                                               mul(keep, ctx.OpLoad(ctx.F32[1], path))))),
                       ctx.ConstF32(1.0f));
    // (Not a number: no air, no dimming.)
    const Id safe_air = FiniteClamp(ctx, air, 0.0f, 64.0f, 3);
    const Id safe_dim = FiniteClamp(ctx, dim, -1.0f, 0.0f);
    return ctx.OpCompositeConstruct(ctx.F32[4],
                                    std::array{ctx.OpCompositeExtract(ctx.F32[1], safe_air, 0u),
                                               ctx.OpCompositeExtract(ctx.F32[1], safe_air, 1u),
                                               ctx.OpCompositeExtract(ctx.F32[1], safe_air, 2u),
                                               safe_dim});
}

Id EmitVrMoonShafts(EmitContext& ctx, Id ndc, Id uv, Id depth, u32 sets_image) {
    return VrMoonShaftsCore(ctx, ndc, uv, depth, sets_image, Id{});
}

// The moon's colour for the air pass (air_light.h): one fragment in ProbeEvery^2 of the first eye
// adds its own surface's light set's colour (light 0 of the set its pixel names), weighted by
// 1 / distance^2 (not the hunter's own hands within NearIgnoreM), to this frame's slot of the
// probe's buffer. The rasterizer reads it a few frames later. Returns nothing to add.
Id EmitVrAirProbe(EmitContext& ctx, Id ndc, Id uv, Id depth, u32 sets_image) {
    namespace Air = Shader::Optimization::AirLight;
    const auto& info = ctx.info;
    const Id zero = ctx.ConstF32(0.0f, 0.0f, 0.0f, 0.0f);
    const auto buffer = std::ranges::find(info.buffers, BufferType::VrMoonShafts, &BufferResource::buffer_type);
    const auto probe = std::ranges::find(info.buffers, BufferType::VrAirProbe, &BufferResource::buffer_type);
    const auto table = std::ranges::find_if(info.buffers, [](const BufferResource& desc) {
        return desc.IsVrHost() &&
               desc.sharp_idx == VrHostBufferBase + static_cast<u32>(VrHostBuffer::MoonSets);
    });
    if (!info.vr_light_shadow.on || sets_image == ~0u || buffer == info.buffers.end() ||
        probe == info.buffers.end() || table == info.buffers.end()) {
        return zero;
    }
    const u32 moon = static_cast<u32>(std::distance(info.buffers.begin(), buffer));
    const u32 sets = static_cast<u32>(std::distance(info.buffers.begin(), table));
    const u32 out = static_cast<u32>(std::distance(info.buffers.begin(), probe));
    VrLightSetup(ctx, ndc, uv, depth);
    const auto& s = ctx.vr_light_shadow;
    const Id f1 = ctx.F32[1], u1 = ctx.U32[1];
    const auto U = [&](u32 v) { return ctx.ConstU32(v); };
    const Id fx = ctx.OpConvertFToU(u1, EmitGetAttribute(ctx, IR::Attribute::FragCoord, 0, 0));
    const Id fy = ctx.OpConvertFToU(u1, EmitGetAttribute(ctx, IR::Attribute::FragCoord, 1, 0));
    // Every ProbeEvery-th pair of columns and rows (a 2x2 coarse pixel's centre is odd).
    const Id grid = U(Air::ProbeEvery / 2 - 1);
    const auto on_line = [&](Id c) {
        return ctx.OpIEqual(ctx.U1[1], ctx.OpBitwiseAnd(u1, ctx.OpShiftRightLogical(u1, c, U(1u)), grid),
                            ctx.u32_zero_value);
    };
    Id sample = ctx.OpLogicalAnd(ctx.U1[1], on_line(fx), on_line(fy));
    sample = ctx.OpLogicalAnd(ctx.U1[1], sample,
                              ctx.OpINotEqual(ctx.U1[1], MoonRead(ctx, moon, 0), ctx.u32_zero_value));
    if (ctx.runtime_info.native_stereo && ctx.runtime_info.native_multiview) {
        sample = ctx.OpLogicalAnd(ctx.U1[1], sample,
                                  ctx.OpIEqual(ctx.U1[1], ctx.NativeEyeIndex(), ctx.u32_zero_value));
    }
    const Id then_label = ctx.OpLabel(), merge = ctx.OpLabel();
    ctx.OpSelectionMerge(merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(sample, then_label, merge);
    ctx.AddLabel(then_label);
    {
        const Id dist2 = ctx.OpDot(f1, s.p, s.p);
        const Id w = ctx.OpCompositeExtract(
            f1, EmitImageSampleExplicitLod(ctx, nullptr, sets_image, uv, ctx.ConstF32(0.0f), IR::Value{}), 3u);
        const Id id_u = ctx.OpBitcast(u1, ctx.OpConvertFToS(ctx.S32[1], ctx.OpFMul(f1, w, ctx.ConstF32(255.0f))));
        const Id ok = ctx.OpLogicalAnd(
            ctx.U1[1], ctx.OpULessThan(ctx.U1[1], id_u, MoonRead(ctx, moon, 22)),
            ctx.OpFOrdGreaterThan(ctx.U1[1], dist2, ctx.ConstF32(Moon::NearIgnoreM * Moon::NearIgnoreM)));
        const Id base = ctx.OpSelect(u1, ok, ctx.OpIMul(u1, id_u, U(Moon::SetDwords)), ctx.u32_zero_value);
        std::array<Id, 3> c;
        for (u32 k = 0; k < 3; ++k) {
            c[k] = ctx.OpBitcast(f1, MoonReadAt(ctx, sets, ctx.OpIAdd(u1, base, U(4 * Moon::SetColour + k))));
        }
        const Id colour = FiniteClamp(ctx, ctx.OpCompositeConstruct(ctx.F32[3], c), 0.0f, 1024.0f, 3);
        const Id weight = ctx.OpSelect(
            f1, ok, ctx.OpFDiv(f1, ctx.ConstF32(1.0f), ctx.OpFMax(f1, FiniteClamp(ctx, dist2, 0.0f, 1e12f), ctx.ConstF32(1.0f))),
            ctx.ConstF32(0.0f));
        // The probe's slot (the moon's buffer names it: dword 23), its first descriptor.
        const auto& pbuf = ctx.buffers[out];
        const auto [pid, pptr_type] = pbuf.Alias(EmitContext::PointerType::U32);
        const Id slot = ctx.OpUMin(u1, MoonRead(ctx, moon, 23), U(Air::ProbeSlots - 1));
        const auto ptr = [&](u32 k) {
            Id index = ctx.OpIAdd(u1, ctx.OpIMul(u1, slot, U(Air::ProbeSlotDwords)), U(k));
            if (const Id offset = pbuf.Offset(EmitContext::PointerSize::B32); Sirit::ValidId(offset)) {
                index = ctx.OpIAdd(u1, index, offset);
            }
            return ctx.runtime_info.native_stereo
                       ? ctx.OpAccessChain(pptr_type, pid, ctx.u32_zero_value, ctx.u32_zero_value, index)
                       : ctx.OpAccessChain(pptr_type, pid, ctx.u32_zero_value, index);
        };
        const Id scope = U(static_cast<u32>(spv::Scope::Device));
        for (u32 k = 0; k < 3; ++k) {
            const Id part = ctx.OpConvertFToU(
                u1, ctx.OpFMul(f1, ctx.OpFMul(f1, ctx.OpCompositeExtract(f1, colour, k), weight),
                               ctx.ConstF32(Air::ProbeColourScale)));
            ctx.OpAtomicIAdd(u1, ptr(k), scope, ctx.u32_zero_value, part);
        }
        ctx.OpAtomicIAdd(u1, ptr(3u), scope, ctx.u32_zero_value,
                         ctx.OpConvertFToU(u1, ctx.OpFMul(f1, weight, ctx.ConstF32(Air::ProbeWeightScale))));
        ctx.OpBranch(merge);
    }
    ctx.AddLabel(merge);
    return zero;
}

} // namespace Shader::Backend::SPIRV
