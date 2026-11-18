// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// The air pass (ir/passes/air_light.h): VrAirCompute, in the shadow mask's first blur, marches the
// moonlight in the air (emit_spirv_moon_shafts.cpp's core) for one fragment in four and stores
// it in the half-size cells; VrAirApply, in the copy of the finished scene, gives every pixel the
// cells round it (and a sky pixel the lamps' halos). Both make their own view position: the
// pixel's ndc from its coordinates, the depth the moon's shadow pass read (a host image), this
// eye's ndc-to-view matrix from the parameters.

#include <algorithm>
#include <array>

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/emit_spirv_light_common.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/passes/air_light.h"
#include "shader_recompiler/ir/passes/fog_grid.h"
#include "shader_recompiler/ir/passes/fog_light.h"

namespace Shader::Backend::SPIRV {

// emit_spirv_moon_shafts.cpp: the march, with a given jitter.
Id VrMoonShaftsCore(EmitContext& ctx, Id ndc, Id uv, Id depth, u32 sets_image, Id jitter);

namespace {

namespace Air = Shader::Optimization::AirLight;
namespace FogTune = Shader::Optimization::FogLight;

// A dword of a buffer from its first descriptor (both eyes' are the same upload), and the
// arctangent the lights in the air use: emit_spirv_light_shadow.cpp's FogRead and Atan, the same.
Id FogRead(EmitContext& ctx, u32 handle, Id index) {
    const auto& buffer = ctx.buffers[handle];
    if (const Id offset = buffer.Offset(EmitContext::PointerSize::B32); Sirit::ValidId(offset)) {
        index = ctx.OpIAdd(ctx.U32[1], index, offset);
    }
    const auto [id, pointer_type] = buffer.Alias(EmitContext::PointerType::U32);
    const Id ptr = ctx.runtime_info.native_stereo
                       ? ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, ctx.u32_zero_value, index)
                       : ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, index);
    return ctx.OpLoad(ctx.U32[1], ptr);
}

Id Atan(EmitContext& ctx, Id x) {
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(ctx.F32[1], a, b); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(ctx.F32[1], a, b); };
    const Id a = ctx.OpFAbs(ctx.F32[1], x);
    const Id big = ctx.OpFOrdGreaterThan(ctx.U1[1], a, ctx.ConstF32(1.0f));
    const Id z = ctx.OpSelect(ctx.F32[1], big,
                              ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), ctx.OpFMax(ctx.F32[1], a, ctx.ConstF32(1.0f))),
                              a);
    const Id z2 = mul(z, z);
    Id p = ctx.ConstF32(0.0208351f);
    for (const float c : {-0.0851330f, 0.1801410f, -0.3302995f, 0.9998660f}) {
        p = add(mul(p, z2), ctx.ConstF32(c));
    }
    p = mul(p, z);
    const Id r = ctx.OpSelect(ctx.F32[1], big, ctx.OpFSub(ctx.F32[1], ctx.ConstF32(1.5707963f), p), p);
    return mul(ctx.OpFSign(ctx.F32[1], x), r);
}

Id PrivateVar(EmitContext& ctx, Id type) {
    const Id var = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Private, type),
                                         spv::StorageClass::Private);
    ctx.interfaces.push_back(var);
    return var;
}

struct Found {
    bool ok{};
    u32 params{}, cells{}, depth{};
    u32 fog{~0u};
    u32 grid{~0u}; // the fog grid's buffer (fog_grid.h), in the permutations made for it
};

Found Find(EmitContext& ctx) {
    const auto& info = ctx.info;
    Found f;
    const auto params = std::ranges::find(info.buffers, BufferType::VrAir, &BufferResource::buffer_type);
    const auto cells = std::ranges::find(info.buffers, BufferType::VrAirCells, &BufferResource::buffer_type);
    const auto depth = std::ranges::find_if(info.images, [](const ImageResource& desc) {
        return desc.sharp_idx == VrHostImageBase + static_cast<u32>(VrHostImage::AirDepth);
    });
    if (params == info.buffers.end() || cells == info.buffers.end() || depth == info.images.end()) {
        return f;
    }
    f.ok = true;
    f.params = static_cast<u32>(std::distance(info.buffers.begin(), params));
    f.cells = static_cast<u32>(std::distance(info.buffers.begin(), cells));
    f.depth = static_cast<u32>(std::distance(info.images.begin(), depth));
    if (const auto fog = std::ranges::find(info.buffers, BufferType::VrFogLights, &BufferResource::buffer_type);
        fog != info.buffers.end()) {
        f.fog = static_cast<u32>(std::distance(info.buffers.begin(), fog));
    }
    if (const auto grid = std::ranges::find(info.buffers, BufferType::VrFogGrid, &BufferResource::buffer_type);
        grid != info.buffers.end()) {
        f.grid = static_cast<u32>(std::distance(info.buffers.begin(), grid));
    }
    return f;
}

// A cell's word in the cells buffer (its first descriptor: both eyes' are the same buffer).
Id CellPointer(EmitContext& ctx, u32 handle, Id index) {
    const auto& buffer = ctx.buffers[handle];
    if (const Id offset = buffer.Offset(EmitContext::PointerSize::B32); Sirit::ValidId(offset)) {
        index = ctx.OpIAdd(ctx.U32[1], index, offset);
    }
    const auto [id, pointer_type] = buffer.Alias(EmitContext::PointerType::U32);
    return ctx.runtime_info.native_stereo
               ? ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, ctx.u32_zero_value, index)
               : ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, index);
}

struct View {
    Id fx, fy; // the fragment's coordinates (float)
    Id ix, iy; // ... as u32
    Id eye;    // u32
    Id ndc;    // vec2
    Id depth;  // the raw depth
    Id p;      // the view position (a sky pixel's: far along its ray)
    Id dir;    // unit
    Id dist;   // |p|, at most the distances' cap
    Id sky;    // bool
    std::array<Id, 4> rows{}; // this eye's ndc-to-view matrix
};

// The fragment's coordinates and eye (cheap: every fragment of the blur needs only these).
View Coordinates(EmitContext& ctx) {
    View v;
    v.fx = EmitGetAttribute(ctx, IR::Attribute::FragCoord, 0, 0);
    v.fy = EmitGetAttribute(ctx, IR::Attribute::FragCoord, 1, 0);
    v.ix = ctx.OpConvertFToU(ctx.U32[1], v.fx);
    v.iy = ctx.OpConvertFToU(ctx.U32[1], v.fy);
    v.eye = ctx.NativeEyeIndex();
    return v;
}

// ... and its view position (the matrix, the depth fetch): where it is needed only.
void Position(EmitContext& ctx, const Found& f, View& v, u32 depth_scale = 1) {
    const Id f1 = ctx.F32[1], u1 = ctx.U32[1];
    const auto F = [&](float v) { return ctx.ConstF32(v); };
    const auto U = [&](u32 v) { return ctx.ConstU32(v); };
    const auto rd = [&](Id index) { return ctx.OpBitcast(f1, FogRead(ctx, f.params, index)); };
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(f1, a, b); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(f1, a, b); };
    const Id nx = add(mul(v.fx, rd(U(8u))), rd(U(10u)));
    const Id ny = add(mul(v.fy, rd(U(9u))), rd(U(11u)));
    v.ndc = ctx.OpCompositeConstruct(ctx.F32[2], std::array{nx, ny});
    // This eye's matrix.
    const Id base = ctx.OpIAdd(u1, U(Air::MatrixDword), ctx.OpIMul(u1, v.eye, U(16u)));
    std::array<Id, 4> rows;
    for (u32 r = 0; r < 4; ++r) {
        std::array<Id, 4> m;
        for (u32 c = 0; c < 4; ++c) {
            m[c] = rd(ctx.OpIAdd(u1, base, U(4 * r + c)));
        }
        rows[r] = ctx.OpCompositeConstruct(ctx.F32[4], m);
    }
    v.rows = rows;
    // The depth the shadow pass read, at this fragment's own texel (this eye's image).
    const auto& texture = ctx.images[f.depth];
    const Id image_ptr =
        ctx.runtime_info.native_stereo
            ? ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::UniformConstant, texture.image_type),
                                texture.id, v.eye)
            : texture.id;
    const Id loaded = ctx.OpLoad(texture.image_type, image_ptr);
    const Id texel = ctx.OpCompositeConstruct(
        ctx.S32[2], std::array{ctx.OpBitcast(ctx.S32[1], ctx.OpIMul(u1, v.ix, U(depth_scale))),
                               ctx.OpBitcast(ctx.S32[1], ctx.OpIMul(u1, v.iy, U(depth_scale)))});
    const std::array<Id, 1> lod{ctx.ConstS32(0)};
    v.depth = ctx.OpCompositeExtract(
        f1, ctx.OpImageFetch(texture.data_types->Get(4), loaded, texel, spv::ImageOperandsMask::Lod, lod), 0u);
    const auto at = [&](Id depth) {
        const Id h = ctx.OpCompositeConstruct(ctx.F32[4], std::array{nx, ny, depth, F(1.0f)});
        const Id w = ctx.OpDot(f1, rows[3], h);
        return ctx.OpVectorTimesScalar(
            ctx.F32[3],
            ctx.OpCompositeConstruct(ctx.F32[3], std::array{ctx.OpDot(f1, rows[0], h), ctx.OpDot(f1, rows[1], h),
                                                            ctx.OpDot(f1, rows[2], h)}),
            ctx.OpFDiv(f1, F(1.0f), w));
    };
    const Id raw = at(v.depth);
    // A number, and nearer than the sky's distance: a surface. (The exponent's bits as integers:
    // emit_spirv_light_common.h Finite.)
    const u32 e = 0x7f800000u;
    const Id bits = ctx.OpBitcast(ctx.U32[3], raw);
    const Id number = ctx.OpAll(
        ctx.U1[1], ctx.OpINotEqual(ctx.U1[3], ctx.OpBitwiseAnd(ctx.U32[3], bits, ctx.ConstU32(e, e, e)),
                                   ctx.ConstU32(e, e, e)));
    const Id safe = ctx.OpSelect(ctx.F32[3], ctx.OpCompositeConstruct(ctx.U1[3], std::array{number, number, number}),
                                 raw, ctx.ConstF32(0.0f, 0.0f, 0.0f));
    const Id sky_at = rd(U(12u));
    const Id len2 = ctx.OpDot(f1, safe, safe);
    const Id surface = ctx.OpLogicalAnd(
        ctx.U1[1], number,
        ctx.OpLogicalAnd(ctx.U1[1], ctx.OpFOrdGreaterThan(ctx.U1[1], len2, F(1e-12f)),
                         ctx.OpFOrdLessThan(ctx.U1[1], len2, mul(sky_at, sky_at))));
    v.sky = ctx.OpLogicalNot(ctx.U1[1], surface);
    // A sky pixel's ray: through its own far point when that is a number (the game's sky dome has
    // a depth), else through the point a mid depth gives; ahead of the eye either way (view z is
    // ahead: a depth past the projection's range comes back mirrored through the eye).
    const Id has_point = ctx.OpLogicalAnd(ctx.U1[1], number, ctx.OpFOrdGreaterThan(ctx.U1[1], len2, F(1e-12f)));
    const Id through = ctx.OpSelect(ctx.F32[3], ctx.OpCompositeConstruct(ctx.U1[3], std::array{has_point, has_point, has_point}),
                                    safe, at(F(0.5f)));
    const Id unit = SafeNormalize(ctx, through, ctx.ConstF32(0.0f, 0.0f, 1.0f));
    const Id behind = ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpCompositeExtract(f1, unit, 2u), F(0.0f));
    const Id mid = ctx.OpSelect(ctx.F32[3], ctx.OpCompositeConstruct(ctx.U1[3], std::array{behind, behind, behind}),
                                ctx.OpFNegate(ctx.F32[3], unit), unit);
    const Id far_point = ctx.OpVectorTimesScalar(ctx.F32[3], mid, sky_at);
    v.p = ctx.OpSelect(ctx.F32[3], ctx.OpCompositeConstruct(ctx.U1[3], std::array{surface, surface, surface}),
                       safe, far_point);
    const Id length = ctx.OpLength(f1, v.p);
    v.dir = ctx.OpVectorTimesScalar(ctx.F32[3], v.p, ctx.OpFDiv(f1, F(1.0f), ctx.OpFMax(f1, length, F(1e-4f))));
    v.dist = ctx.OpFMin(f1, length, ctx.OpFMax(f1, rd(U(7u)), F(1.0f)));
}

// A distance as the cell keeps it: 0..255 over DistanceOctaves octaves from DistanceLoM.
Id DistanceCode(EmitContext& ctx, Id dist) {
    const Id f1 = ctx.F32[1];
    const Id octaves = ctx.OpFSub(f1, ctx.OpLog2(f1, ctx.OpFMax(f1, dist, ctx.ConstF32(Air::DistanceLoM))),
                                  ctx.ConstF32(-3.0f)); // log2(DistanceLoM)
    return ctx.OpFClamp(f1, ctx.OpFMul(f1, octaves, ctx.ConstF32(255.0f / Air::DistanceOctaves)),
                        ctx.ConstF32(0.0f), ctx.ConstF32(255.0f));
}
static_assert(Air::DistanceLoM == 0.125f);

// This fragment's block (a coarse fragment covers bw x bh pixels) and its first pixel.
struct FragmentBlock {
    Id bw, bh, x0, y0;
};
FragmentBlock Block(EmitContext& ctx, const View& v) {
    const Id u1 = ctx.U32[1];
    if (!Sirit::ValidId(ctx.shading_rate)) {
        return {ctx.ConstU32(1u), ctx.ConstU32(1u), v.ix, v.iy};
    }
    // Horizontal2Pixels 4, Horizontal4Pixels 8, Vertical2Pixels 1, Vertical4Pixels 2.
    const Id rate = ctx.OpLoad(u1, ctx.shading_rate);
    const auto has = [&](u32 bit) {
        return ctx.OpINotEqual(ctx.U1[1], ctx.OpBitwiseAnd(u1, rate, ctx.ConstU32(bit)), ctx.u32_zero_value);
    };
    const Id bw = ctx.OpSelect(u1, has(8u), ctx.ConstU32(4u), ctx.OpSelect(u1, has(4u), ctx.ConstU32(2u), ctx.ConstU32(1u)));
    const Id bh = ctx.OpSelect(u1, has(2u), ctx.ConstU32(4u), ctx.OpSelect(u1, has(1u), ctx.ConstU32(2u), ctx.ConstU32(1u)));
    return {bw, bh, ctx.OpIMul(u1, ctx.OpUDiv(u1, v.ix, bw), bw), ctx.OpIMul(u1, ctx.OpUDiv(u1, v.iy, bh), bh)};
}

// The mist along this cell's ray (air_light.h Mist): what it scatters towards the eye (a scalar,
// times the moon's colour at the apply) and its transmittance. `moon`: the moon's buffer (its
// phase's direction and g), `jitter` the cell's.
struct MistLight {
    Id scatter, transmit;
};
MistLight MistAlong(EmitContext& ctx, const Found& f, const View& v, u32 moon, Id jitter, Id beam) {
    namespace Mist = Air::Mist;
    const Id f1 = ctx.F32[1], f3 = ctx.F32[3];
    const auto F = [&](float x) { return ctx.ConstF32(x); };
    const auto U = [&](u32 x) { return ctx.ConstU32(x); };
    const auto rd = [&](u32 d) { return ctx.OpBitcast(f1, FogRead(ctx, f.params, U(d))); };
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(f1, a, b); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(f1, a, b); };
    const auto sub = [&](Id a, Id b) { return ctx.OpFSub(f1, a, b); };
    const auto smooth = [&](Id e0, Id e1, Id x) {
        const Id t = ctx.OpFClamp(f1, ctx.OpFDiv(f1, sub(x, e0), ctx.OpFMax(f1, sub(e1, e0), F(1e-3f))), F(0.0f), F(1.0f));
        return mul(mul(t, t), sub(F(3.0f), mul(F(2.0f), t)));
    };
    const u32 m = Air::MistDword;
    const Id time = rd(m + 0), density = FiniteClamp(ctx, rd(m + 1), 0.0f, 4.0f), inv_height = rd(m + 2);
    const Id eye_height = rd(m + 3), far = ctx.OpFMax(f1, rd(m + 4), F(2.0f)), ambient = rd(m + 5), forward = rd(m + 6);
    const Id scale = rd(m + 7), extinction = rd(m + 11), near_start = rd(m + 12), near_full = rd(m + 13);
    const Id patchy = rd(m + 14), strength = rd(m + 15);
    const Id high = FiniteClamp(ctx, rd(Mist::HighDword), 0.0f, 1.0f); // the thin haze above the layer
    const Id haze_inv = FiniteClamp(ctx, rd(Mist::HazeDword), 1e-3f, 10.0f); // 1 / its height
    const Id wind = ctx.OpVectorTimesScalar(f3, ctx.OpCompositeConstruct(f3, std::array{rd(m + 8), rd(m + 9), rd(m + 10)}), time);
    // This frame's view to world: the rotation's rows and the camera's place.
    std::array<Id, 3> rows;
    std::array<Id, 3> place;
    for (u32 r = 0; r < 3; ++r) {
        const u32 d = Air::ViewDword + 4 * r;
        rows[r] = ctx.OpCompositeConstruct(f3, std::array{rd(d), rd(d + 1), rd(d + 2)});
        place[r] = rd(d + 3);
    }
    const Id origin = ctx.OpCompositeConstruct(f3, place);
    const Id length = ctx.OpFMin(f1, ctx.OpLength(f1, v.p), far);
    const Id span = ctx.OpFMax(f1, sub(length, near_start), F(0.0f));
    // A gyroid: smooth, cheap, never the same twice across a few metres.
    const auto gyroid = [&](Id q) {
        const Id s = ctx.OpSin(f3, q);
        const Id c = ctx.OpCos(f3, ctx.OpVectorShuffle(f3, q, q, 1u, 2u, 0u));
        return ctx.OpDot(f1, s, c);
    };
    Id depth = F(0.0f);   // the optical depth so far
    Id scatter = F(0.0f); // the light sent towards the eye
    for (u32 i = 0; i < Mist::Steps; ++i) {
        const Id u = mul(add(F(static_cast<float>(i)), jitter), F(1.0f / Mist::Steps));
        const Id t = add(near_start, mul(span, mul(u, u)));
        const Id dt = mul(mul(F(2.0f / Mist::Steps), span), u);
        const Id p = ctx.OpVectorTimesScalar(f3, v.dir, t);
        const Id world = ctx.OpFAdd(
            f3, origin,
            ctx.OpCompositeConstruct(f3, std::array{ctx.OpDot(f1, rows[0], p), ctx.OpDot(f1, rows[1], p),
                                                    ctx.OpDot(f1, rows[2], p)}));
        // Over the ground: the eye's height plus the point's own along the world's up.
        const Id height = ctx.OpFMax(f1, add(eye_height, ctx.OpDot(f1, rows[1], p)), F(0.0f));
        const Id layer = add(ctx.OpExp(f1, ctx.OpFNegate(f1, mul(height, inv_height))),
                             mul(high, ctx.OpExp(f1, ctx.OpFNegate(f1, mul(height, haze_inv)))));
        const Id q = ctx.OpFAdd(f3, ctx.OpVectorTimesScalar(f3, world, scale), wind);
        const Id q2 = ctx.OpFAdd(f3, ctx.OpVectorTimesScalar(f3, q, F(2.17f)), ctx.ConstF32(1.7f, 4.1f, 2.9f));
        const Id noise = ctx.OpFClamp(f1, add(F(0.5f), add(mul(F(0.30f), gyroid(q)), mul(F(0.15f), gyroid(q2)))), F(0.0f), F(1.0f));
        const Id banks = mul(smooth(F(0.25f), F(0.85f), noise), F(1.6f));
        const Id patch = add(F(1.0f), mul(patchy, sub(banks, F(1.0f))));
        const Id here = mul(mul(mul(density, layer), patch), smooth(near_start, near_full, t));
        scatter = add(scatter, mul(mul(here, ctx.OpExp(f1, ctx.OpFNegate(f1, mul(extinction, depth)))), dt));
        depth = add(depth, mul(here, dt));
    }
    // The moon's phase (its buffer's direction, dwords 8..10, and g, dword 6), a share of it.
    const auto moonf = [&](u32 d) { return ctx.OpBitcast(f1, FogRead(ctx, moon, U(d))); };
    const Id towards = ctx.OpCompositeConstruct(f3, std::array{moonf(8), moonf(9), moonf(10)});
    const Id g = F(Mist::PhaseG);
    const Id g2 = mul(g, g);
    const Id denom = ctx.OpFMax(f1, sub(add(F(1.0f), g2), mul(mul(F(2.0f), g), ctx.OpDot(f1, v.dir, towards))), F(1e-4f));
    const Id hg = ctx.OpFDiv(f1, sub(F(1.0f), g2), mul(denom, ctx.OpSqrt(f1, denom)));
    Id phase = add(ambient, mul(forward, hg));
    if (Sirit::ValidId(beam)) {
        phase = add(phase, beam); // the moon's rays (air_light.h Rays)
    }
    MistLight out;
    out.scatter = FiniteClamp(ctx, mul(mul(scatter, phase), strength), 0.0f, 16.0f);
    out.transmit = FiniteClamp(ctx, ctx.OpExp(f1, ctx.OpFNegate(f1, mul(extinction, depth))), 0.0f, 1.0f);
    return out;
}

// The moon's rays (air_light.h Rays): how open the way from this cell to the moon is on the
// screen, and what that adds to the mist's phase.
struct RayLight {
    Id open, weight, beam;
};
RayLight MoonRays(EmitContext& ctx, const Found& f, const View& v, u32 moon, Id jitter, u32 depth_scale) {
    namespace Rays = Air::Rays;
    const Id f1 = ctx.F32[1], f2 = ctx.F32[2], u1 = ctx.U32[1], b1 = ctx.U1[1];
    const auto F = [&](float x) { return ctx.ConstF32(x); };
    const auto U = [&](u32 x) { return ctx.ConstU32(x); };
    const auto rd = [&](Id index) { return ctx.OpBitcast(f1, FogRead(ctx, f.params, index)); };
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(f1, a, b); };
    const auto sub = [&](Id a, Id b) { return ctx.OpFSub(f1, a, b); };
    const Id at = ctx.OpIAdd(u1, U(Air::RaysDword), ctx.OpIMul(u1, v.eye, U(4u)));
    const Id moon_ndc = ctx.OpCompositeConstruct(f2, std::array{FiniteClamp(ctx, rd(at), -64.0f, 64.0f),
                                                                 FiniteClamp(ctx, rd(ctx.OpIAdd(u1, at, U(1u))), -64.0f, 64.0f)});
    RayLight out;
    out.weight = FiniteClamp(ctx, rd(ctx.OpIAdd(u1, at, U(2u))), 0.0f, 1.0f);
    const Id strength = FiniteClamp(ctx, rd(U(Air::RaysDword + 8)), 0.0f, 8.0f);
    const Id reach_max = FiniteClamp(ctx, rd(U(Air::RaysDword + 10)), 0.05f, 4.0f);
    const Id near_m = FiniteClamp(ctx, rd(U(Air::RaysDword + 11)), 0.0f, 64.0f);
    const Id sky_at = rd(U(12u));
    const Id delta = ctx.OpFSub(f2, moon_ndc, v.ndc);
    const Id len = ctx.OpLength(f1, delta);
    const Id reach = ctx.OpFMin(f1, len, reach_max);
    const Id step = ctx.OpVectorTimesScalar(
        f2, delta, ctx.OpFDiv(f1, reach, ctx.OpFMax(f1, mul(len, F(static_cast<float>(Rays::Steps))), F(1e-4f))));
    // A texel of the depth from an ndc: the parameters' line backwards, times the depth's scale.
    const float scale = static_cast<float>(depth_scale);
    const Id line_k = ctx.OpCompositeConstruct(f2, std::array{rd(U(8u)), rd(U(9u))});
    const Id line_c = ctx.OpCompositeConstruct(f2, std::array{rd(U(10u)), rd(U(11u))});
    const Id to_texel = ctx.OpFDiv(f2, ctx.ConstF32(scale, scale), line_k);
    const Id cw = ctx.OpUMax(u1, FogRead(ctx, f.params, U(1u)), U(2u));
    const Id ch = ctx.OpUMax(u1, FogRead(ctx, f.params, U(2u)), U(2u));
    const Id last = ctx.OpCompositeConstruct(
        f2, std::array{ctx.OpConvertUToF(f1, ctx.OpISub(u1, ctx.OpIMul(u1, cw, U(2u)), U(2u))),
                       ctx.OpConvertUToF(f1, ctx.OpISub(u1, ctx.OpIMul(u1, ch, U(2u)), U(2u)))});
    const auto& texture = ctx.images[f.depth];
    const Id image_ptr =
        ctx.runtime_info.native_stereo
            ? ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::UniformConstant, texture.image_type),
                                texture.id, v.eye)
            : texture.id;
    const Id loaded = ctx.OpLoad(texture.image_type, image_ptr);
    const std::array<Id, 1> lod{ctx.ConstS32(0)};
    Id blocked = F(0.0f);
    for (u32 i = 0; i < Rays::Steps; ++i) {
        const Id s = ctx.OpFClamp(
            f2, ctx.OpFAdd(f2, v.ndc, ctx.OpVectorTimesScalar(f2, step, ctx.OpFAdd(f1, F(static_cast<float>(i)), jitter))),
            ctx.ConstF32(-1.0f, -1.0f), ctx.ConstF32(1.0f, 1.0f));
        const Id t = ctx.OpFClamp(f2, ctx.OpFMul(f2, ctx.OpFSub(f2, s, line_c), to_texel), ctx.ConstF32(0.0f, 0.0f), last);
        const Id texel = ctx.OpConvertFToS(ctx.S32[2], t);
        const Id depth = ctx.OpCompositeExtract(
            f1, ctx.OpImageFetch(texture.data_types->Get(4), loaded, texel, spv::ImageOperandsMask::Lod, lod), 0u);
        const Id h = ctx.OpCompositeConstruct(
            ctx.F32[4], std::array{ctx.OpCompositeExtract(f1, s, 0u), ctx.OpCompositeExtract(f1, s, 1u), depth, F(1.0f)});
        const Id z = ctx.OpFDiv(f1, ctx.OpDot(f1, v.rows[2], h), ctx.OpDot(f1, v.rows[3], h));
        // Something in front of the sky: nearer than it, farther than the near bound (soft). A
        // depth that gives no number, or one behind the eye, is the sky (the comparisons fail).
        const Id solid = ctx.OpLogicalAnd(b1, ctx.OpFOrdGreaterThan(b1, z, near_m), ctx.OpFOrdLessThan(b1, z, sky_at));
        const Id share = ctx.OpFClamp(f1, mul(sub(z, near_m), F(1.0f / Rays::NearSoftM)), F(0.0f), F(1.0f));
        blocked = ctx.OpFAdd(f1, blocked, ctx.OpSelect(f1, solid, share, F(0.0f)));
    }
    out.open = ctx.OpFClamp(f1, sub(F(1.0f), mul(blocked, F(1.0f / Rays::Steps))), F(0.0f), F(1.0f));
    // The sharp phase towards the moon (the moon's buffer: its direction, dwords 8..10).
    const auto moonf = [&](u32 d) { return ctx.OpBitcast(f1, FogRead(ctx, moon, U(d))); };
    const Id towards = ctx.OpCompositeConstruct(ctx.F32[3], std::array{moonf(8), moonf(9), moonf(10)});
    const float g = Rays::PhaseG;
    const Id denom = ctx.OpFMax(f1, sub(F(1.0f + g * g), mul(F(2.0f * g), ctx.OpDot(f1, v.dir, towards))), F(1e-4f));
    const Id hg = ctx.OpFMin(f1, ctx.OpFDiv(f1, F(1.0f - g * g), mul(denom, ctx.OpSqrt(f1, denom))), F(Rays::Peak));
    out.beam = FiniteClamp(ctx, mul(mul(strength, hg), mul(out.open, out.weight)), 0.0f, 64.0f);
    return out;
}

// The fog grid's air at this pixel (fog_grid.h: the mist and the moonlit thin air, the moon's
// shadow in both): its view position moved to the head's (this eye's shift along x), then the
// eight cells round it, blended. What the air between the eye and the pixel sends towards the eye
// (rgb) and how much of the pixel gets through (w); no air
// (0 0 0 1) while `on` is false or the buffer holds nothing built.
// The buffer's first descriptor in both eyes: the grid is the head's, one for the two of them.
Id GridLookup(EmitContext& ctx, u32 grid, const View& v, Id on) {
    namespace Grid = Shader::Optimization::FogGrid;
    const Id f1 = ctx.F32[1], f4 = ctx.F32[4], u1 = ctx.U32[1], b1 = ctx.U1[1];
    const auto F = [&](float x) { return ctx.ConstF32(x); };
    const auto U = [&](u32 x) { return ctx.ConstU32(x); };
    const auto rdu = [&](Id index) { return FogRead(ctx, grid, index); };
    const auto rdf = [&](u32 d) { return ctx.OpBitcast(f1, rdu(U(d))); };
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(f1, a, b); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(f1, a, b); };
    const auto sub = [&](Id a, Id b) { return ctx.OpFSub(f1, a, b); };
    // (Sizes kept sane whatever the header holds: the stand-in is two cells a side.)
    const Id gx = ctx.OpUClamp(u1, rdu(U(0u)), U(2u), U(1024u)), gy = ctx.OpUClamp(u1, rdu(U(1u)), U(2u), U(1024u));
    const Id gz = ctx.OpUClamp(u1, rdu(U(2u)), U(2u), U(1024u));
    const Id built = ctx.OpLogicalAnd(b1, on, ctx.OpINotEqual(b1, rdu(U(3u)), ctx.u32_zero_value));
    // The head's view: eye 0 sits at minus half the eye distance (the fog lights' rule).
    const Id half = rdf(12);
    const Id shift = ctx.OpSelect(f1, ctx.OpIEqual(b1, v.eye, ctx.u32_zero_value), ctx.OpFNegate(f1, half), half);
    const Id pz = ctx.OpFMax(f1, ctx.OpCompositeExtract(f1, v.p, 2u), F(1e-3f));
    const Id inv_z = ctx.OpFDiv(f1, F(1.0f), pz);
    const Id tx = mul(add(ctx.OpCompositeExtract(f1, v.p, 0u), shift), inv_z);
    const Id ty = mul(ctx.OpCompositeExtract(f1, v.p, 1u), inv_z);
    // Cell coordinates: across and up in cells (a cell's middle at + 0.5), the depth in slices.
    const Id cx = sub(mul(ctx.OpFDiv(f1, sub(tx, rdf(4)), rdf(6)), ctx.OpConvertUToF(f1, gx)), F(0.5f));
    const Id cy = sub(mul(ctx.OpFDiv(f1, sub(ty, rdf(5)), rdf(7)), ctx.OpConvertUToF(f1, gy)), F(0.5f));
    const Id s = mul(mul(ctx.OpLog2(f1, ctx.OpFMax(f1, ctx.OpFDiv(f1, pz, rdf(8)), F(1e-6f))), rdf(11)),
                     ctx.OpConvertUToF(f1, gz));
    // A cell and the next along one axis, and the share of the next (never off the grid; a
    // coordinate that is no number: the first cell).
    const auto split = [&](Id c, Id size, Id& i0, Id& i1, Id& t) {
        const Id last = ctx.OpISub(u1, size, U(1u));
        const Id cc = ctx.OpFClamp(f1, Finite(ctx, c, F(0.0f)), F(0.0f), ctx.OpConvertUToF(f1, last));
        const Id fl = ctx.OpFloor(f1, cc);
        i0 = ctx.OpUMin(u1, ctx.OpConvertFToU(u1, fl), last);
        i1 = ctx.OpUMin(u1, ctx.OpIAdd(u1, i0, U(1u)), last);
        t = sub(cc, fl);
    };
    // Along the depth the value AT slice coordinate k + 1 is cell k's (all the air up to its far
    // face): between cells floor(s - 1) and the next; nearer than the first far face, from no air
    // up to cell 0.
    Id x0, x1, wx, y0, y1, wy, z0, z1, wz;
    split(cx, gx, x0, x1, wx);
    split(cy, gy, y0, y1, wy);
    split(sub(s, F(1.0f)), gz, z0, z1, wz);
    const Id near_fade = ctx.OpFClamp(f1, Finite(ctx, s, F(0.0f)), F(0.0f), F(1.0f));
    Id sum = ctx.ConstF32(0.0f, 0.0f, 0.0f, 0.0f);
    for (u32 corner = 0; corner < 8; ++corner) {
        const Id x = corner & 1 ? x1 : x0, y = corner & 2 ? y1 : y0, z = corner & 4 ? z1 : z0;
        const Id weight = mul(mul(corner & 1 ? wx : sub(F(1.0f), wx), corner & 2 ? wy : sub(F(1.0f), wy)),
                              corner & 4 ? wz : sub(F(1.0f), wz));
        // (By row, column, slice.)
        const Id at = ctx.OpIAdd(
            u1, U(Grid::HeaderDwords),
            ctx.OpIMul(u1, U(Grid::CellDwords),
                       ctx.OpIAdd(u1, ctx.OpIMul(u1, ctx.OpIAdd(u1, ctx.OpIMul(u1, y, gx), x), gz), z)));
        const Id w0 = rdu(at), w1 = rdu(ctx.OpIAdd(u1, at, U(1u)));
        const auto code = [&](Id word, u32 bits) {
            return mul(ctx.OpConvertUToF(f1, ctx.OpBitwiseAnd(u1, ctx.OpShiftRightLogical(u1, word, U(bits)), U(0xffffu))),
                       F(1.0f / 65535.0f));
        };
        // (The colours were stored as sqrt(light / MaxLight).)
        const Id r = code(w0, 0), g = code(w0, 16), b = code(w1, 0);
        const Id cell = ctx.OpCompositeConstruct(
            f4, std::array{mul(mul(r, r), F(Grid::MaxLight)), mul(mul(g, g), F(Grid::MaxLight)),
                           mul(mul(b, b), F(Grid::MaxLight)), code(w1, 16)});
        sum = ctx.OpFAdd(f4, sum, ctx.OpVectorTimesScalar(f4, cell, weight));
    }
    const Id none = ctx.ConstF32(0.0f, 0.0f, 0.0f, 1.0f);
    const Id share = ctx.OpSelect(f1, built, near_fade, F(0.0f));
    return ctx.OpFAdd(f4, none, ctx.OpVectorTimesScalar(f4, ctx.OpFSub(f4, sum, none), share));
}

} // namespace

Id EmitVrAirCompute(EmitContext& ctx) {
    const Found f = Find(ctx);
    const auto moon = std::ranges::find(ctx.info.buffers, BufferType::VrMoonShafts, &BufferResource::buffer_type);
    if (!f.ok || moon == ctx.info.buffers.end()) {
        return ctx.ConstF32(0.0f);
    }
    const Id f1 = ctx.F32[1], u1 = ctx.U32[1];
    const auto F = [&](float x) { return ctx.ConstF32(x); };
    const auto U = [&](u32 x) { return ctx.ConstU32(x); };
    const auto rdu = [&](u32 d) { return FogRead(ctx, f.params, U(d)); };
    const Id flags = rdu(0u);
    // A half-size host (air_light.h IsHalfProgram): its fragments are the cells.
    const bool half = Air::IsHalfProgram(ctx.info.pgm_hash);
    View v = Coordinates(ctx);
    const FragmentBlock block = Block(ctx, v);
    // The full-size host: one fragment in four, the even pixels, and every coarse fragment along
    // an axis it spans.
    const auto even = [&](Id size, Id first) {
        return ctx.OpLogicalOr(ctx.U1[1], ctx.OpUGreaterThanEqual(ctx.U1[1], size, U(2u)),
                               ctx.OpIEqual(ctx.U1[1], ctx.OpBitwiseAnd(u1, first, U(1u)), ctx.u32_zero_value));
    };
    const Id flagged = ctx.OpINotEqual(ctx.U1[1], ctx.OpBitwiseAnd(u1, flags, U(Air::FlagCompute)), ctx.u32_zero_value);
    const Id go = half ? flagged
                       : ctx.OpLogicalAnd(ctx.U1[1], flagged,
                                          ctx.OpLogicalAnd(ctx.U1[1], even(block.bw, block.x0), even(block.bh, block.y0)));
    const Id then_label = ctx.OpLabel(), merge = ctx.OpLabel();
    ctx.OpSelectionMerge(merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(go, then_label, merge);
    ctx.AddLabel(then_label);
    {
        // The march reads the lighting setup's view position: ours, made for marching fragments
        // only (three fragments in four leave at the branch).
        Position(ctx, f, v, half ? 2u : 1u);
        ctx.vr_light_shadow.p = v.p;
        ctx.vr_light_shadow.ready = true;
        const Id cx = half ? block.x0 : ctx.OpShiftRightLogical(u1, block.x0, U(1u));
        const Id cy = half ? block.y0 : ctx.OpShiftRightLogical(u1, block.y0, U(1u));
        // The cell's jitter, by its place in the 2x2 pattern.
        const Id place = ctx.OpIAdd(u1, ctx.OpBitwiseAnd(u1, cx, U(1u)),
                                    ctx.OpIMul(u1, ctx.OpBitwiseAnd(u1, cy, U(1u)), U(2u)));
        Id jitter = F(Air::Jitter[0]);
        for (u32 k = 1; k < 4; ++k) {
            jitter = ctx.OpSelect(f1, ctx.OpIEqual(ctx.U1[1], place, U(k)), F(Air::Jitter[k]), jitter);
        }
        // A coarse fragment gives all its cells its one jitter: the mean of the four instead (a
        // region of them was brighter or darker than its neighbours: rectangles at the
        // foveated edge).
        const u32 coarse_from = half ? 2u : 4u;
        const Id coarse = ctx.OpLogicalOr(ctx.U1[1], ctx.OpUGreaterThanEqual(ctx.U1[1], block.bw, U(coarse_from)),
                                          ctx.OpUGreaterThanEqual(ctx.U1[1], block.bh, U(coarse_from)));
        jitter = ctx.OpSelect(f1, coarse, F(0.5f), jitter);
        const Id shafts = VrMoonShaftsCore(ctx, v.ndc, v.ndc, v.depth, ~0u, jitter);
        // (The buffer's colour is white here: x is the amount; w the transmittance minus one.)
        Id amount = ctx.OpFClamp(f1, ctx.OpCompositeExtract(f1, shafts, 0u), F(0.0f), F(64.0f));
        Id transmit = ctx.OpFClamp(f1, ctx.OpFAdd(f1, ctx.OpCompositeExtract(f1, shafts, 3u), F(1.0f)),
                                   F(0.0f), F(1.0f));
        // The mist over the ground (air_light.h Mist): its light joins the amount (what is nearer
        // than it is lit first: the moonlight's own air is thin), its transmittance the cell's.
        // (With the fog grid, fog_grid.h, this op is not in the program at all: AirLightPass.)
        if (Air::Mist::Enabled()) {
            const u32 moon_index = static_cast<u32>(std::distance(ctx.info.buffers.begin(), moon));
            Id beam{};
            if (Air::Rays::Enabled()) {
                // The moon's rays (air_light.h Rays): the thin air's light follows the open share
                // (CoreShare of it), the mist's phase gets the beam.
                const RayLight rays = MoonRays(ctx, f, v, moon_index, jitter, half ? 2u : 1u);
                const Id follow = ctx.OpFMul(f1, rays.weight, F(Air::Rays::CoreShare));
                amount = ctx.OpFMul(
                    f1, amount,
                    ctx.OpFAdd(f1, ctx.OpFSub(f1, F(1.0f), follow),
                               ctx.OpFMul(f1, follow, ctx.OpFMul(f1, rays.open, F(2.0f)))));
                beam = rays.beam;
            }
            const MistLight mist = MistAlong(ctx, f, v, moon_index, jitter, beam);
            amount = ctx.OpFClamp(f1, ctx.OpFAdd(f1, amount, mist.scatter), F(0.0f), F(64.0f));
            transmit = ctx.OpFMul(f1, transmit, mist.transmit);
        }
        const Id amount_code = ctx.OpConvertFToU(
            u1, ctx.OpFAdd(f1, ctx.OpFMul(f1, ctx.OpSqrt(f1, ctx.OpFMul(f1, amount, F(1.0f / 64.0f))), F(65535.0f)),
                           F(0.5f)));
        const Id transmit_code =
            ctx.OpConvertFToU(u1, ctx.OpFAdd(f1, ctx.OpFMul(f1, transmit, F(255.0f)), F(0.5f)));
        const Id distance_code = ctx.OpConvertFToU(u1, ctx.OpFAdd(f1, DistanceCode(ctx, v.dist), F(0.5f)));
        const Id word = ctx.OpBitwiseOr(
            u1, ctx.OpUMin(u1, amount_code, U(65535u)),
            ctx.OpBitwiseOr(u1, ctx.OpShiftLeftLogical(u1, ctx.OpUMin(u1, transmit_code, U(255u)), U(16u)),
                            ctx.OpShiftLeftLogical(u1, ctx.OpUMin(u1, distance_code, U(255u)), U(24u))));
        const Id cw = ctx.OpUMax(u1, rdu(1u), U(1u));
        const Id ch = ctx.OpUMax(u1, rdu(2u), U(1u));
        const Id base = ctx.OpIMul(u1, v.eye, rdu(3u));
        // A 4-pixel block covers two cells along that axis.
        const Id dx = ctx.OpSelect(u1, ctx.OpIEqual(ctx.U1[1], block.bw, U(4u)), U(1u), ctx.u32_zero_value);
        const Id dy = ctx.OpSelect(u1, ctx.OpIEqual(ctx.U1[1], block.bh, U(4u)), U(1u), ctx.u32_zero_value);
        const Id last_x = ctx.OpISub(u1, cw, U(1u)), last_y = ctx.OpISub(u1, ch, U(1u));
        if (half) {
            // Every cell the fragment's block covers (one, or more at a coarse rate): a loop.
            const Id count = ctx.OpIMul(u1, block.bw, block.bh);
            const Id k_var = PrivateVar(ctx, u1);
            ctx.OpStore(k_var, ctx.u32_zero_value);
            const Id header = ctx.OpLabel(), body = ctx.OpLabel(), cont = ctx.OpLabel(), done = ctx.OpLabel();
            ctx.OpBranch(header);
            ctx.AddLabel(header);
            const Id k = ctx.OpLoad(u1, k_var);
            // (The condition before the merge: OpLoopMerge must be just before the block's branch.)
            const Id more = ctx.OpULessThan(ctx.U1[1], k, count);
            ctx.OpLoopMerge(done, cont, spv::LoopControlMask::MaskNone);
            ctx.OpBranchConditional(more, body, done);
            ctx.AddLabel(body);
            const Id x = ctx.OpUMin(u1, ctx.OpIAdd(u1, cx, ctx.OpUMod(u1, k, block.bw)), last_x);
            const Id y = ctx.OpUMin(u1, ctx.OpIAdd(u1, cy, ctx.OpUDiv(u1, k, block.bw)), last_y);
            const Id at = ctx.OpIAdd(u1, base, ctx.OpIAdd(u1, ctx.OpIMul(u1, y, cw), x));
            ctx.OpStore(CellPointer(ctx, f.cells, at), word);
            ctx.OpStore(k_var, ctx.OpIAdd(u1, k, U(1u)));
            ctx.OpBranch(cont);
            ctx.AddLabel(cont);
            ctx.OpBranch(header);
            ctx.AddLabel(done);
        } else {
            const std::array<Id, 2> xs{ctx.OpUMin(u1, cx, last_x), ctx.OpUMin(u1, ctx.OpIAdd(u1, cx, dx), last_x)};
            const std::array<Id, 2> ys{ctx.OpUMin(u1, cy, last_y), ctx.OpUMin(u1, ctx.OpIAdd(u1, cy, dy), last_y)};
            for (const Id y : ys) {
                for (const Id x : xs) {
                    const Id at = ctx.OpIAdd(u1, base, ctx.OpIAdd(u1, ctx.OpIMul(u1, y, cw), x));
                    ctx.OpStore(CellPointer(ctx, f.cells, at), word);
                }
            }
        }
        ctx.OpBranch(merge);
    }
    ctx.AddLabel(merge);
    return F(0.0f);
}

// A light's source vote (air_light.h Sources; called per light from the glow's loop): this
// fragment's ray passes `off2` (squared) from the light, `along` the ray; when that is within the
// outer ring and the light is not behind the fragment's surface, the scene's brightness here goes
// to the light's inner or ring maximum in the probe's buffer, and its count up.
void VrAirSourceVote(EmitContext& ctx, const VrFogVote& vote, Id light, Id along, Id off2) {
    namespace Air = Shader::Optimization::AirLight;
    const Id f1 = ctx.F32[1], u1 = ctx.U32[1], b1 = ctx.U1[1];
    const auto F = [&](float x) { return ctx.ConstF32(x); };
    const auto U = [&](u32 x) { return ctx.ConstU32(x); };
    const Id inner = ctx.OpFOrdLessThan(b1, off2, F(Air::SourceInnerM * Air::SourceInnerM));
    const Id near_it = ctx.OpFOrdLessThan(b1, off2, F(Air::SourceOuterM * Air::SourceOuterM));
    const Id ahead = ctx.OpFOrdGreaterThan(b1, along, F(Air::SourceNearM));
    const Id seen = ctx.OpFOrdGreaterThan(b1, vote.length, ctx.OpFSub(f1, along, F(Air::SourceBehindM)));
    const Id go = ctx.OpLogicalAnd(b1, ctx.OpLogicalAnd(b1, vote.on, near_it), ctx.OpLogicalAnd(b1, ahead, seen));
    const Id then_label = ctx.OpLabel(), merge = ctx.OpLabel();
    ctx.OpSelectionMerge(merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(go, then_label, merge);
    ctx.AddLabel(then_label);
    {
        const auto& buffer = ctx.buffers[vote.probe];
        const auto [id, pointer_type] = buffer.Alias(EmitContext::PointerType::U32);
        const Id first = ctx.OpIAdd(
            u1, ctx.OpIAdd(u1, vote.base, ctx.OpIMul(u1, ctx.OpUMin(u1, light, U(Air::SourceLights - 1)), U(Air::SourceDwords))),
            ctx.OpSelect(u1, inner, ctx.u32_zero_value, U(2u)));
        const auto ptr = [&](Id index) {
            if (const Id offset = buffer.Offset(EmitContext::PointerSize::B32); Sirit::ValidId(offset)) {
                index = ctx.OpIAdd(u1, index, offset);
            }
            return ctx.runtime_info.native_stereo
                       ? ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, ctx.u32_zero_value, index)
                       : ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, index);
        };
        const Id scope = U(static_cast<u32>(spv::Scope::Device));
        ctx.OpAtomicUMax(u1, ptr(first), scope, ctx.u32_zero_value, vote.lum_code);
        ctx.OpAtomicIAdd(u1, ptr(ctx.OpIAdd(u1, first, U(1u))), scope, ctx.u32_zero_value, U(1u));
        ctx.OpBranch(merge);
    }
    ctx.AddLabel(merge);
}

Id EmitVrAirApply(EmitContext& ctx, u32 scene_image) {
    const Found f = Find(ctx);
    if (!f.ok) {
        return ctx.ConstF32(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const Id f1 = ctx.F32[1], f3 = ctx.F32[3], u1 = ctx.U32[1];
    const auto F = [&](float x) { return ctx.ConstF32(x); };
    const auto U = [&](u32 x) { return ctx.ConstU32(x); };
    const auto rdu = [&](u32 d) { return FogRead(ctx, f.params, U(d)); };
    const auto rdf = [&](u32 d) { return ctx.OpBitcast(f1, rdu(d)); };
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(f1, a, b); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(f1, a, b); };
    const auto sub = [&](Id a, Id b) { return ctx.OpFSub(f1, a, b); };
    const Id flags = rdu(0u);
    const auto has = [&](u32 bit) {
        return ctx.OpINotEqual(ctx.U1[1], ctx.OpBitwiseAnd(u1, flags, U(bit)), ctx.u32_zero_value);
    };
    View v = Coordinates(ctx);
    // (2026-11-15) VR_OPT_AIRBRANCH (on unless 0): the view position, the scene's sample and the
    // lighting's setup are made inside the `apply` branch below, not before it: a draw of this
    // program with nothing to apply (its flags word 0: not the scene's copy, or the air idle) no
    // longer pays for them. The branch's condition is one word of the parameters, the same for
    // every pixel of the draw (uniform control flow: the setup's derivatives stay defined).
    // 0: before the branch, the shader as it was.
    static const bool opt_branch = [] {
        const char* const env = std::getenv("VR_OPT_AIRBRANCH");
        return env != nullptr && env[0] == '1'; // (off unless 1: measured 2026-11-15, no gain in lit scenes)
    }();
    // The lights in the air's glow runs on the VR lighting's setup (emit_spirv_light_shadow.cpp):
    // made here from this eye's rows and the depth just fetched, at the top (it takes screen
    // derivatives); its view position ours (a sky pixel's is far along its ray).
    const bool glow = f.fog != ~0u && ctx.info.vr_light_shadow.on;
    // The scene's colour here (the copy's own picture, its own sampler), for the source votes and
    // the moon's place; and the probe's slot they go to.
    const auto probe_it = std::ranges::find(ctx.info.buffers, BufferType::VrAirProbe, &BufferResource::buffer_type);
    const bool probes = probe_it != ctx.info.buffers.end() && scene_image != ~0u &&
                        (Air::Sources() || Air::MoonAim());
    const u32 probe_index = static_cast<u32>(std::distance(ctx.info.buffers.begin(), probe_it));
    Id uv{}, scene{}, scene_lum{}, slot_base{};
    const auto prepare = [&] {
        Position(ctx, f, v);
        uv = ctx.OpCompositeConstruct(ctx.F32[2], std::array{mul(v.fx, rdf(Air::SlotDword + 1)),
                                                              mul(v.fy, rdf(Air::SlotDword + 2))});
        if (probes) {
            const Id scene4 = EmitImageSampleExplicitLod(ctx, nullptr, scene_image, uv, ctx.ConstF32(0.0f), IR::Value{});
            scene = FiniteClamp(ctx, ctx.OpVectorShuffle(f3, scene4, scene4, 0u, 1u, 2u), 0.0f, 4096.0f, 3);
            scene_lum = ctx.OpDot(f1, scene, ctx.ConstF32(0.2126f, 0.7152f, 0.0722f));
            slot_base = ctx.OpIMul(u1, ctx.OpUMin(u1, rdu(Air::SlotDword), U(Air::ProbeSlots - 1)), U(Air::ProbeSlotDwords));
        }
        if (glow) {
            ctx.vr_light_shadow.rows = v.rows;
            ctx.vr_light_shadow.rows_given = true;
            VrLightSetup(ctx, v.ndc, uv, v.depth);
            ctx.vr_light_shadow.p = v.p;
        }
    };
    if (!opt_branch) {
        prepare();
    }
    const Id result = PrivateVar(ctx, ctx.F32[4]);
    ctx.OpStore(result, ctx.ConstF32(0.0f, 0.0f, 0.0f, 0.0f));
    // (The condition before the merge: OpSelectionMerge must be just before the block's branch.)
    const Id apply = has(Air::FlagApply);
    const Id then_label = ctx.OpLabel(), merge = ctx.OpLabel();
    ctx.OpSelectionMerge(merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(apply, then_label, merge);
    ctx.AddLabel(then_label);
    if (opt_branch) {
        prepare(); // (2026-11-15, VR_OPT_AIRBRANCH)
    }
    if (probes && Air::MoonAim()) {
        // The moon's place (air_light.h MoonAim): a sky fragment of the first eye on the grid,
        // cool or white, reports its brightness and place (an atomic maximum).
        const Id grid = U(Air::MoonAimEvery - 1);
        const Id on_grid = ctx.OpIEqual(
            ctx.U1[1], ctx.OpBitwiseAnd(u1, ctx.OpBitwiseOr(u1, v.ix, v.iy), grid), ctx.u32_zero_value);
        const Id cool = ctx.OpFOrdGreaterThanEqual(
            ctx.U1[1], ctx.OpCompositeExtract(f1, scene, 2u),
            mul(ctx.OpCompositeExtract(f1, scene, 0u), F(Air::MoonAimCool)));
        const Id report = ctx.OpLogicalAnd(
            ctx.U1[1], ctx.OpLogicalAnd(ctx.U1[1], has(Air::FlagMoonAim), v.sky),
            ctx.OpLogicalAnd(ctx.U1[1], ctx.OpLogicalAnd(ctx.U1[1], on_grid, cool),
                             ctx.OpIEqual(ctx.U1[1], v.eye, ctx.u32_zero_value)));
        const Id aim_label = ctx.OpLabel(), aim_done = ctx.OpLabel();
        ctx.OpSelectionMerge(aim_done, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(report, aim_label, aim_done);
        ctx.AddLabel(aim_label);
        {
            const Id bright = ctx.OpUMin(u1, ctx.OpConvertFToU(u1, mul(scene_lum, F(Air::MoonAimLumScale))), U(4095u));
            const Id word = ctx.OpBitwiseOr(
                u1, ctx.OpShiftLeftLogical(u1, bright, U(20u)),
                ctx.OpBitwiseOr(u1,
                                ctx.OpShiftLeftLogical(u1, ctx.OpBitwiseAnd(u1, ctx.OpShiftRightLogical(u1, v.ix, U(1u)), U(1023u)), U(10u)),
                                ctx.OpBitwiseAnd(u1, ctx.OpShiftRightLogical(u1, v.iy, U(1u)), U(1023u))));
            const auto& buffer = ctx.buffers[probe_index];
            const auto [id, pointer_type] = buffer.Alias(EmitContext::PointerType::U32);
            Id index = ctx.OpIAdd(u1, slot_base, U(Air::MoonAimDword));
            if (const Id offset = buffer.Offset(EmitContext::PointerSize::B32); Sirit::ValidId(offset)) {
                index = ctx.OpIAdd(u1, index, offset);
            }
            const Id ptr = ctx.runtime_info.native_stereo
                               ? ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, ctx.u32_zero_value, index)
                               : ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, index);
            ctx.OpAtomicUMax(u1, ptr, U(static_cast<u32>(spv::Scope::Device)), ctx.u32_zero_value, word);
            ctx.OpBranch(aim_done);
        }
        ctx.AddLabel(aim_done);
    }
    {
        // With the fog grid (fog_grid.h) the mist and the moonlit air are the grid's: its light
        // (already the moon's colour) and its transmittance for this pixel, and no cell is read.
        const bool grid = f.grid != ~0u;
        // (2026-11-15) VR_OPT_AIRCELLS (on unless 0; not with the fog grid): the cells and the
        // distance haze only in a frame that has cells (Air::FlagCells, one parameter word: the
        // same for every pixel). Without them the selects below gave amount 0 and transmit 1
        // whatever was worked out: the two variables start there. 0: the shader as it was.
        static const bool opt_cells = [] {
            const char* const env = std::getenv("VR_OPT_AIRCELLS");
            return env != nullptr && env[0] == '1'; // (off unless 1: measured 2026-11-15, no gain in lit scenes)
        }();
        const bool cells_branch = opt_cells && !grid;
        Id amount_var{}, transmit_var{}, cells_done{};
        if (cells_branch) {
            amount_var = PrivateVar(ctx, f1);
            transmit_var = PrivateVar(ctx, f1);
            ctx.OpStore(amount_var, F(0.0f));
            ctx.OpStore(transmit_var, F(1.0f));
            // (The condition before the merge: OpSelectionMerge must be just before the branch.)
            const Id with_cells = has(Air::FlagCells);
            const Id cells_label = ctx.OpLabel();
            cells_done = ctx.OpLabel();
            ctx.OpSelectionMerge(cells_done, spv::SelectionControlMask::MaskNone);
            ctx.OpBranchConditional(with_cells, cells_label, cells_done);
            ctx.AddLabel(cells_label);
        }
        Id mist_far{}, share{}, cells_amount{}, cells_transmit{}, cells_on{};
        if (!grid) {
            const Id here = DistanceCode(ctx, v.dist);
            const Id cw = ctx.OpUMax(u1, rdu(1u), U(1u));
            const Id ch = ctx.OpUMax(u1, rdu(2u), U(1u));
            const Id base = ctx.OpIMul(u1, v.eye, rdu(3u));
            const Id cx = ctx.OpBitcast(ctx.S32[1], ctx.OpShiftRightLogical(u1, v.ix, U(1u)));
            const Id cy = ctx.OpBitcast(ctx.S32[1], ctx.OpShiftRightLogical(u1, v.iy, U(1u)));
            const Id last_x = ctx.OpBitcast(ctx.S32[1], ctx.OpISub(u1, cw, U(1u)));
            const Id last_y = ctx.OpBitcast(ctx.S32[1], ctx.OpISub(u1, ch, U(1u)));
            Id sum_amount = F(0.0f), sum_transmit = F(0.0f), sum_weight = F(0.0f);
            Id centre_amount{}, centre_transmit{};
            // The mist of a cell, brought to this pixel's own distance (air_light.h CellStretch): the
            // same ray, so its optical depth in the share of the marched way.
            const bool to_pixel = Air::Mist::Enabled();
            mist_far = ctx.OpFMax(f1, rdf(Air::MistDword + 4), F(2.0f));
            const Id mist_near = rdf(Air::MistDword + 12);
            const Id way_here = ctx.OpFMax(f1, sub(ctx.OpFMin(f1, v.dist, mist_far), mist_near), F(0.0f));
            // In the cells' codes: EdgeNear and EdgeFar octaves.
            const float code = 255.0f / Air::DistanceOctaves;
            for (s32 dy = -1; dy <= 1; ++dy) {
                for (s32 dx = -1; dx <= 1; ++dx) {
                    const Id x = ctx.OpSClamp(ctx.S32[1], ctx.OpIAdd(ctx.S32[1], cx, ctx.ConstS32(dx)), ctx.ConstS32(0), last_x);
                    const Id y = ctx.OpSClamp(ctx.S32[1], ctx.OpIAdd(ctx.S32[1], cy, ctx.ConstS32(dy)), ctx.ConstS32(0), last_y);
                    const Id at = ctx.OpIAdd(u1, base,
                                             ctx.OpIAdd(u1, ctx.OpIMul(u1, ctx.OpBitcast(u1, y), cw), ctx.OpBitcast(u1, x)));
                    const Id word = ctx.OpLoad(u1, CellPointer(ctx, f.cells, at));
                    const Id root = mul(ctx.OpConvertUToF(f1, ctx.OpBitwiseAnd(u1, word, U(0xffffu))), F(1.0f / 65535.0f));
                    const Id amount = mul(mul(root, root), F(64.0f));
                    const Id transmit = mul(
                        ctx.OpConvertUToF(f1, ctx.OpBitwiseAnd(u1, ctx.OpShiftRightLogical(u1, word, U(16u)), U(0xffu))),
                        F(1.0f / 255.0f));
                    Id cell_amount = amount, cell_transmit = transmit;
                    const Id there = ctx.OpConvertUToF(f1, ctx.OpShiftRightLogical(u1, word, U(24u)));
                    if (to_pixel) {
                        const Id there_m = mul(F(Air::DistanceLoM), ctx.OpExp2(f1, mul(there, F(Air::DistanceOctaves / 255.0f))));
                        const Id way_there = ctx.OpFMax(f1, sub(ctx.OpFMin(f1, there_m, mist_far), mist_near), F(0.25f));
                        const Id stretch = ctx.OpFClamp(f1, ctx.OpFDiv(f1, way_here, way_there), F(0.0f), F(Air::Mist::CellStretch));
                        const Id kept = ctx.OpExp2(
                            f1, mul(ctx.OpLog2(f1, ctx.OpFMax(f1, transmit, F(1.0f / 510.0f))), stretch));
                        const Id lost = sub(F(1.0f), transmit);
                        const Id gain = ctx.OpSelect(f1, ctx.OpFOrdGreaterThan(ctx.U1[1], lost, F(1.5f / 255.0f)),
                                                     ctx.OpFDiv(f1, sub(F(1.0f), kept), ctx.OpFMax(f1, lost, F(1.0f / 255.0f))),
                                                     stretch);
                        cell_amount = mul(amount, ctx.OpFMin(f1, gain, F(Air::Mist::CellStretch)));
                        cell_transmit = kept;
                    }
                    const Id apart = ctx.OpFAbs(f1, sub(there, here));
                    const Id t = ctx.OpFClamp(f1, mul(sub(apart, F(Air::EdgeNear * code)),
                                                      F(1.0f / ((Air::EdgeFar - Air::EdgeNear) * code))),
                                              F(0.0f), F(1.0f));
                    const Id close = sub(F(1.0f), mul(mul(t, t), sub(F(3.0f), mul(F(2.0f), t))));
                    const float tent = (dx == 0 ? 2.0f : 1.0f) * (dy == 0 ? 2.0f : 1.0f);
                    const Id weight = mul(close, F(tent));
                    sum_amount = add(sum_amount, mul(cell_amount, weight));
                    sum_transmit = add(sum_transmit, mul(cell_transmit, weight));
                    sum_weight = add(sum_weight, weight);
                    if (dx == 0 && dy == 0) {
                        centre_amount = cell_amount;
                        centre_transmit = cell_transmit;
                    }
                }
            }
            // No cell at the pixel's distance (a thin thing in front of a far one): its own cell's.
            const Id any = ctx.OpFOrdGreaterThan(ctx.U1[1], sum_weight, F(0.05f));
            const Id inv = ctx.OpFDiv(f1, F(1.0f), ctx.OpFMax(f1, sum_weight, F(0.05f)));
            // (No cells this frame: no moonlight, nothing dimmed.)
            // A sky pixel takes the parameters' share of it (dword 13: 1, the same air as in front
            // of everything else; less keeps the sky and what stands against it clearer).
            share = ctx.OpSelect(f1, v.sky, FiniteClamp(ctx, rdf(13u), 0.0f, 1.0f), F(1.0f));
            cells_on = has(Air::FlagCells);
            cells_amount = ctx.OpSelect(f1, any, mul(sum_amount, inv), centre_amount);
            cells_transmit = ctx.OpSelect(f1, any, mul(sum_transmit, inv), centre_transmit);
        }
        Id grid_on{}, grid_light{}, grid_transmit{};
        if (grid) {
            mist_far = ctx.OpFMax(f1, rdf(Air::MistDword + 4), F(2.0f));
            // A sky pixel takes the parameters' share of the air (dword 13, as above).
            share = ctx.OpSelect(f1, v.sky, FiniteClamp(ctx, rdf(13u), 0.0f, 1.0f), F(1.0f));
            cells_amount = F(0.0f);
            cells_transmit = F(1.0f);
            grid_on = has(Air::FlagGrid);
            const Id air = GridLookup(ctx, f.grid, v, grid_on);
            grid_light = FiniteClamp(ctx, ctx.OpVectorShuffle(f3, air, air, 0u, 1u, 2u), 0.0f, 64.0f, 3);
            grid_transmit = FiniteClamp(ctx, ctx.OpCompositeExtract(f1, air, 3u), 0.0f, 1.0f);
        }
        if (Air::Mist::Enabled()) {
            // The distance haze (air_light.h Mist::HazeHeightM), for this pixel's own distance: past
            // the march's reach the thin haze goes on to the surface (the sky: its far point), an
            // exponential height fog's own integral along the ray: density0 * exp(-h0 / H) * L *
            // (1 - exp(-k)) / k, k = up * L / H. Seen through the mist in front of it.
            namespace Mist = Air::Mist;
            const u32 m = Air::MistDword;
            const Id density = FiniteClamp(ctx, rdf(m + 1), 0.0f, 4.0f);
            const Id eye_height = rdf(m + 3);
            const Id extinction = rdf(m + 11);
            const Id high = FiniteClamp(ctx, rdf(Mist::HighDword), 0.0f, 1.0f);
            const Id haze_inv = FiniteClamp(ctx, rdf(Mist::HazeDword), 1e-3f, 10.0f);
            const Id haze_far = FiniteClamp(ctx, rdf(Mist::HazeDword + 1), 0.0f, 4096.0f);
            const Id haze_glow = FiniteClamp(ctx, rdf(Mist::HazeDword + 2), 0.0f, 16.0f);
            const Id up_row = ctx.OpCompositeConstruct(
                f3, std::array{rdf(Air::ViewDword + 4), rdf(Air::ViewDword + 5), rdf(Air::ViewDword + 6)});
            const Id whole_m = ctx.OpLength(f1, v.p);
            const Id marched = ctx.OpFMin(f1, whole_m, mist_far);
            const Id beyond = ctx.OpFMax(f1, sub(ctx.OpFMin(f1, whole_m, haze_far), marched), F(0.0f));
            const Id up = ctx.OpDot(f1, up_row, v.dir);
            const Id h0 = ctx.OpFMax(f1, add(eye_height, mul(up, marched)), F(0.0f));
            const Id k = ctx.OpFClamp(f1, mul(mul(up, beyond), haze_inv), F(-3.0f), F(60.0f));
            const Id tiny = ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpFAbs(f1, k), F(1e-3f));
            const Id safe_k = ctx.OpSelect(f1, tiny, F(1.0f), k);
            const Id fk = ctx.OpSelect(f1, tiny, F(1.0f),
                                       ctx.OpFDiv(f1, sub(F(1.0f), ctx.OpExp(f1, ctx.OpFNegate(f1, safe_k))), safe_k));
            const Id tau = FiniteClamp(
                ctx, mul(mul(mul(density, high), ctx.OpExp(f1, ctx.OpFNegate(f1, mul(h0, haze_inv)))), mul(beyond, fk)),
                0.0f, 64.0f);
            const Id ext = ctx.OpFMax(f1, extinction, F(1e-3f));
            const Id through = ctx.OpExp(f1, ctx.OpFNegate(f1, mul(extinction, tau)));
            const Id light = mul(mul(haze_glow, ctx.OpFDiv(f1, sub(F(1.0f), ctx.OpExp(f1, ctx.OpFNegate(f1, mul(ext, tau)))), ext)),
                                 mul(add(rdf(m + 5), mul(rdf(m + 6), F(Mist::HazePhase))), rdf(m + 15)));
            if (grid) {
                // The haze belongs to the mist: behind the grid's, and none without the grid.
                cells_amount = add(cells_amount, ctx.OpSelect(f1, grid_on,
                                                              mul(mul(cells_transmit, grid_transmit),
                                                                  FiniteClamp(ctx, light, 0.0f, 16.0f)),
                                                              F(0.0f)));
                grid_transmit = mul(grid_transmit, ctx.OpSelect(f1, grid_on, FiniteClamp(ctx, through, 0.0f, 1.0f), F(1.0f)));
            } else {
                cells_amount = add(cells_amount, mul(cells_transmit, FiniteClamp(ctx, light, 0.0f, 16.0f)));
                cells_transmit = mul(cells_transmit, FiniteClamp(ctx, through, 0.0f, 1.0f));
            }
        }
        Id amount = grid ? mul(cells_amount, share)
                         : ctx.OpSelect(f1, cells_on, mul(cells_amount, share), F(0.0f));
        Id transmit =
            grid ? sub(F(1.0f), mul(sub(F(1.0f), mul(cells_transmit, grid_transmit)), share))
                 : ctx.OpSelect(f1, cells_on, sub(F(1.0f), mul(sub(F(1.0f), cells_transmit), share)), F(1.0f));
        if (cells_branch) {
            // (2026-11-15, VR_OPT_AIRCELLS) Out of the branch through the two variables.
            ctx.OpStore(amount_var, amount);
            ctx.OpStore(transmit_var, transmit);
            ctx.OpBranch(cells_done);
            ctx.AddLabel(cells_done);
            amount = ctx.OpLoad(f1, amount_var);
            transmit = ctx.OpLoad(f1, transmit_var);
        }
        const Id colour = FiniteClamp(ctx, ctx.OpCompositeConstruct(f3, std::array{rdf(4u), rdf(5u), rdf(6u)}),
                                      0.0f, 1024.0f, 3);
        Id rgb = FiniteClamp(ctx, ctx.OpVectorTimesScalar(f3, colour, amount), 0.0f, 64.0f, 3);
        if (grid) {
            rgb = ctx.OpFAdd(f3, rgb, ctx.OpVectorTimesScalar(f3, grid_light, share));
        }
        // The lamps' glow in the air: every pixel's (air_light.h Halos: after the particles), or
        // with the glow left in the composite a sky pixel's only (the composite never drew it).
        if (glow) {
            const Id halos = PrivateVar(ctx, f3);
            ctx.OpStore(halos, ctx.ConstF32(0.0f, 0.0f, 0.0f));
            Id want = Air::Halos() ? has(Air::FlagHalos)
                                   : ctx.OpLogicalAnd(ctx.U1[1], has(Air::FlagHalos), v.sky);
            // The source votes (air_light.h Sources): a quarter of the first eye's pixels, in
            // 4x4 blocks (whole quads: the other pixels end the lights' loop after the ones with a
            // glow, and a quad with one voting pixel would run all four to the end), the blocks'
            // place by the probe's slot (every pixel votes once in four frames).
            VrFogVote vote{};
            const bool votes = Air::Sources() && probes;
            if (votes) {
                const Id slot = rdu(Air::SlotDword);
                const auto in_block = [&](Id c, Id phase) {
                    return ctx.OpIEqual(ctx.U1[1], ctx.OpBitwiseAnd(u1, ctx.OpShiftRightLogical(u1, c, U(2u)), U(1u)),
                                        ctx.OpBitwiseAnd(u1, phase, U(1u)));
                };
                const Id block = ctx.OpLogicalAnd(ctx.U1[1], in_block(v.ix, slot),
                                                  in_block(v.iy, ctx.OpShiftRightLogical(u1, slot, U(1u))));
                vote.on = ctx.OpLogicalAnd(
                    ctx.U1[1], ctx.OpLogicalAnd(ctx.U1[1], has(Air::FlagSources), block),
                    ctx.OpIEqual(ctx.U1[1], v.eye, ctx.u32_zero_value));
                vote.lum_code = ctx.OpConvertFToU(u1, mul(scene_lum, F(Air::SourceLumScale)));
                vote.base = ctx.OpIAdd(u1, slot_base, U(Air::ProbeDwords));
                vote.length = ctx.OpLength(f1, v.p);
                vote.probe = probe_index;
            }
            // (2026-11-15) VR_OPT_GLOWNONE (on unless 0): no light for this pixel's loop (none
            // with a glow of its own in the list, and the pixel does not vote; or no light at
            // all): VrFogLightParts would invert the eye's matrix and take four more depth
            // samples for the surface's normal, run no light and give 0, which `halos` holds
            // already. The count as VrFogLightParts works it out (the lights' header word 0).
            // 0: the shader as it was.
            static const bool opt_glow_none = [] {
                const char* const env = std::getenv("VR_OPT_GLOWNONE");
                return env != nullptr && env[0] == '1'; // (off unless 1: measured 2026-11-15: 0.015 ms less with no glowing lamp in view, 0.005 more with one)
            }();
            if (opt_glow_none) {
                const Id count_word = FogRead(ctx, f.fog, U(0u));
                const Id count_all = ctx.OpUMin(u1, ctx.OpBitwiseAnd(u1, count_word, U(0xffffu)), U(FogTune::MaxLights));
                const Id count_glow = ctx.OpUMin(u1, ctx.OpShiftRightLogical(u1, count_word, U(16u)), count_all);
                const Id count = votes ? ctx.OpSelect(u1, vote.on, count_all, count_glow) : count_glow;
                want = ctx.OpLogicalAnd(ctx.U1[1], want, ctx.OpINotEqual(ctx.U1[1], count, ctx.u32_zero_value));
            }
            const Id glow_label = ctx.OpLabel(), glow_done = ctx.OpLabel();
            ctx.OpSelectionMerge(glow_done, spv::SelectionControlMask::MaskNone);
            ctx.OpBranchConditional(want, glow_label, glow_done);
            ctx.AddLabel(glow_label);
            const Id air = VrFogLightParts(ctx, v.ndc, uv, v.depth, true, false, votes ? &vote : nullptr);
            ctx.OpStore(halos, ctx.OpVectorShuffle(f3, air, air, 0u, 1u, 2u));
            ctx.OpBranch(glow_done);
            ctx.AddLabel(glow_done);
            // A glow never outshines what it sits on (air_light.h GlowKnee).
            Id glow_rgb = ctx.OpLoad(f3, halos);
            if (probes) {
                const Id knee = FiniteClamp(ctx, rdf(Air::GlowKneeDword), 0.0f, 64.0f);
                const Id on = ctx.OpFOrdGreaterThan(ctx.U1[1], knee, F(1e-4f));
                const Id share = ctx.OpSelect(
                    f1, on, ctx.OpFDiv(f1, knee, ctx.OpFAdd(f1, knee, ctx.OpFMax(f1, scene_lum, F(0.0f)))), F(1.0f));
                glow_rgb = ctx.OpVectorTimesScalar(f3, glow_rgb, share);
            }
            rgb = ctx.OpFAdd(f3, rgb, glow_rgb);
        }
        Id dim = FiniteClamp(ctx, sub(transmit, F(1.0f)), -1.0f, 0.0f);
        if (probes) {
            // ... and the air dims a bright pixel less (GlowDimKnees).
            const Id knee = mul(FiniteClamp(ctx, rdf(Air::GlowKneeDword), 0.0f, 64.0f), F(Air::GlowDimKnees));
            const Id on = ctx.OpFOrdGreaterThan(ctx.U1[1], knee, F(1e-4f));
            dim = mul(dim, ctx.OpSelect(
                               f1, on, ctx.OpFDiv(f1, knee, ctx.OpFAdd(f1, knee, ctx.OpFMax(f1, scene_lum, F(0.0f)))),
                               F(1.0f)));
        }
        ctx.OpStore(result, ctx.OpCompositeConstruct(ctx.F32[4],
                                                     std::array{ctx.OpCompositeExtract(f1, rgb, 0u),
                                                                ctx.OpCompositeExtract(f1, rgb, 1u),
                                                                ctx.OpCompositeExtract(f1, rgb, 2u), dim}));
        ctx.OpBranch(merge);
    }
    ctx.AddLabel(merge);
    return ctx.OpLoad(ctx.F32[4], result);
}

} // namespace Shader::Backend::SPIRV
