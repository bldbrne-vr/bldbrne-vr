// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// VrLightVisibility (ir/passes/light_shadow.h, LightShadowPass): light k's visibility from this
// pixel. Once per shader, in the entry block: the pixel's view position P from the matrix rows
// (M * (ndc, depth, 1)), M's inverse (view to ndc), the ndc-to-coordinates line (from the
// derivatives, where they are defined) and a per-pixel, per-frame jitter (the matrix moves with
// the upscaler's sub-pixel jitter). Per light, only when it is near the eye and reaches this
// pixel: Steps points from the surface towards it, each projected into the eye's depth; a point
// with the depth there in front of it (by more than BiasM, less than ThickM) is in shadow.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <tuple>
#include <type_traits>
#include <utility>

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/emit_spirv_light_common.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/passes/ambient_occlusion.h"
#include "shader_recompiler/ir/passes/bounce_light.h"
#include "shader_recompiler/ir/passes/cinematic_look.h"
#include "shader_recompiler/ir/passes/body_shadow.h"
#include "shader_recompiler/ir/passes/air_light.h"
#include "shader_recompiler/ir/passes/fog_light.h"
#include "shader_recompiler/ir/passes/light_shadow.h"

namespace Shader::Backend::SPIRV {

// (Below, outside the unnamed namespace: the body buffer's dword.)
Id BodyRead(EmitContext& ctx, u32 handle, u32 dword);
Id BodyReadAt(EmitContext& ctx, u32 handle, Id dword);
Id BodyVisibility(EmitContext& ctx, u32 buf, u32 maps, Id p, Id l, bool his_known = false);

namespace {

namespace Tune = Shader::Optimization::LightShadow;

Id ReadF(EmitContext& ctx, u32 handle, u32 dword) {
    return ctx.OpBitcast(ctx.F32[1], EmitReadConstBuffer(ctx, handle, ctx.ConstU32(dword)));
}

// Hermite smoothstep(e0, e1, x).
Id SmoothStep(EmitContext& ctx, float e0, float e1, Id x) {
    const Id t = ctx.OpFClamp(ctx.F32[1],
                              ctx.OpFMul(ctx.F32[1], ctx.OpFSub(ctx.F32[1], x, ctx.ConstF32(e0)),
                                         ctx.ConstF32(1.0f / (e1 - e0))),
                              ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
    const Id k = ctx.OpFSub(ctx.F32[1], ctx.ConstF32(3.0f), ctx.OpFMul(ctx.F32[1], ctx.ConstF32(2.0f), t));
    return ctx.OpFMul(ctx.F32[1], ctx.OpFMul(ctx.F32[1], t, t), k);
}

// rows . (x, y, z, 1): a 4-vector.
std::array<Id, 4> Transform(EmitContext& ctx, const std::array<Id, 4>& rows, Id x, Id y, Id z) {
    const Id v = ctx.OpCompositeConstruct(ctx.F32[4], std::array{x, y, z, ctx.ConstF32(1.0f)});
    std::array<Id, 4> out;
    for (u32 i = 0; i < 4; ++i) {
        out[i] = ctx.OpDot(ctx.F32[1], rows[i], v);
    }
    return out;
}

Id Private(EmitContext& ctx, Id type) {
    const Id var = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Private, type),
                                         spv::StorageClass::Private);
    ctx.interfaces.push_back(var);
    return var;
}

// The inverse of M (its elements m[r][c]) as rows: the adjugate's transpose over the
// determinant (cofactors of 3x3 minors).
std::array<Id, 4> InverseRows(EmitContext& ctx, const std::array<std::array<Id, 4>, 4>& m) {
    std::array<Id, 4> inv;
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(ctx.F32[1], a, b); };
    const auto sub = [&](Id a, Id b) { return ctx.OpFSub(ctx.F32[1], a, b); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(ctx.F32[1], a, b); };
    std::array<std::array<Id, 4>, 4> cof;
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
            const Id d3 = add(sub(mul(e(0, 0), sub(mul(e(1, 1), e(2, 2)), mul(e(1, 2), e(2, 1)))),
                                  mul(e(0, 1), sub(mul(e(1, 0), e(2, 2)), mul(e(1, 2), e(2, 0))))),
                              mul(e(0, 2), sub(mul(e(1, 0), e(2, 1)), mul(e(1, 1), e(2, 0)))));
            cof[r][c] = ((r + c) & 1) ? ctx.OpFNegate(ctx.F32[1], d3) : d3;
        }
    }
    Id det = ctx.ConstF32(0.0f);
    for (u32 c = 0; c < 4; ++c) {
        det = add(det, mul(m[0][c], cof[0][c]));
    }
    const Id inv_det = ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), det);
    for (u32 r = 0; r < 4; ++r) {
        std::array<Id, 4> row;
        for (u32 c = 0; c < 4; ++c) {
            row[c] = mul(cof[c][r], inv_det); // inverse[r][c] = cofactor[c][r] / det
        }
        inv[r] = ctx.OpCompositeConstruct(ctx.F32[4], row);
    }
    return inv;
}

// ... as s.inv.
void Invert(EmitContext& ctx, const std::array<std::array<Id, 4>, 4>& m) {
    auto& s = ctx.vr_light_shadow;
    s.inv = InverseRows(ctx, m);
    s.has_inv = true;
}

// The lights the game keeps on the camera (light_shadow.h AttachedFromM), once, where Setup is.
void AttachedSetup(EmitContext& ctx) {
    namespace Body = Shader::Optimization::BodyShadow;
    auto& s = ctx.vr_light_shadow;
    if (s.attached_ready) {
        return;
    }
    s.attached_ready = true;
    if (!Body::Enabled()) {
        return;
    }
    const auto& buffers = ctx.info.buffers;
    const auto it = std::ranges::find(buffers, BufferType::VrBodyCapsules, &BufferResource::buffer_type);
    if (it == buffers.end()) {
        return;
    }
    const u32 body = static_cast<u32>(std::distance(buffers.begin(), it));
    const auto rf = [&](u32 d) { return ctx.OpBitcast(ctx.F32[1], BodyRead(ctx, body, Body::AttachedAt + d)); };
    s.attached_count = BodyRead(ctx, body, Body::AttachedAt);
    Id shift = ctx.ConstF32(0.0f);
    if (ctx.runtime_info.native_stereo && ctx.runtime_info.native_multiview) {
        const Id half = rf(1);
        shift = ctx.OpSelect(ctx.F32[1], ctx.OpIEqual(ctx.U1[1], ctx.NativeEyeIndex(), ctx.u32_zero_value),
                             ctx.OpFNegate(ctx.F32[1], half), half);
    }
    s.attached_shift = ctx.OpCompositeConstruct(ctx.F32[3], std::array{shift, ctx.ConstF32(0.0f), ctx.ConstF32(0.0f)});
    s.attached_remove = ctx.OpINotEqual(ctx.U1[1], BodyRead(ctx, body, Body::AttachedAt + 2),
                                        ctx.u32_zero_value);
    for (u32 k = 0; k < 4; ++k) {
        s.attached[k] = ctx.OpCompositeConstruct(
            ctx.F32[4], std::array{rf(4 + 4 * k), rf(5 + 4 * k), rf(6 + 4 * k), rf(7 + 4 * k)});
    }
    // Tests (body_shadow.h ProbeAt; light_shadow.h Probe).
    if (Tune::Probe()) {
        const auto pf = [&](u32 d) { return ctx.OpBitcast(ctx.F32[1], BodyRead(ctx, body, Body::ProbeAt + d)); };
        s.probe_mask = BodyRead(ctx, body, Body::ProbeAt);
        s.probe_nudge = ctx.OpCompositeConstruct(ctx.F32[3], std::array{pf(1), pf(2), pf(3)});
        s.probe_sel = BodyRead(ctx, body, Body::ProbeAt + 4);
    }
}

// The scene maps (light_shadow.h SceneMaps), once: the slots from the body buffer's header and
// this eye's shift to the flat view (the attached lights' convention, header AttachedAt + 1).
void SceneSetup(EmitContext& ctx) {
    namespace Body = Shader::Optimization::BodyShadow;
    namespace Scene = Tune::SceneMaps;
    static_assert(Body::SlotsDwords == Scene::SlotHeader + Scene::SlotDwords * Scene::Slots);
    auto& s = ctx.vr_light_shadow;
    if (s.scene_ready) {
        return;
    }
    s.scene_ready = true;
    if (!Scene::Enabled() || !Body::Enabled()) {
        return;
    }
    const auto& buffers = ctx.info.buffers;
    const auto body_it = std::ranges::find(buffers, BufferType::VrBodyCapsules, &BufferResource::buffer_type);
    const auto maps_it = std::ranges::find(buffers, BufferType::VrSceneMaps, &BufferResource::buffer_type);
    if (body_it == buffers.end() || (maps_it == buffers.end() && !Scene::Mask())) {
        return;
    }
    const u32 body = static_cast<u32>(std::distance(buffers.begin(), body_it));
    s.scene_buffer = maps_it == buffers.end() ? 0u : static_cast<u32>(std::distance(buffers.begin(), maps_it));
    s.scene_body = body;
    s.scene_count = ctx.OpUMin(ctx.U32[1], BodyRead(ctx, body, Body::SlotsAt), ctx.ConstU32(Scene::Slots));
    const Id half_and_edges = BodyRead(ctx, body, Body::SlotsAt + 2);
    s.scene_half = ctx.OpBitwiseAnd(ctx.U32[1], half_and_edges, ctx.ConstU32(1u));
    if (ctx.runtime_info.vr_light_shadows && Scene::Edges()) {
        s.edge_slots = ctx.OpBitwiseAnd(ctx.U32[1], ctx.OpShiftRightLogical(ctx.U32[1], half_and_edges, ctx.ConstU32(Scene::EdgeSlotShift)),
                                        ctx.ConstU32((1u << Scene::Slots) - 1u));
    }
    // (The edge lamps for the mask's march: the others march PlainStepShare, light_shadow.h.)
    if (Scene::Edges() && Scene::PlainStepShare() > Scene::MarchStepShare()) {
        s.edge_set = Sirit::ValidId(s.edge_slots)
                         ? s.edge_slots
                         : ctx.OpBitwiseAnd(ctx.U32[1], ctx.OpShiftRightLogical(ctx.U32[1], half_and_edges, ctx.ConstU32(Scene::EdgeSlotShift)),
                                            ctx.ConstU32((1u << Scene::Slots) - 1u));
    }
    s.scene_capacity = BodyRead(ctx, body, Body::SlotsAt + 3);
    for (u32 r = 0; r < 3; ++r) {
        const auto rf = [&](u32 d) { return ctx.OpBitcast(ctx.F32[1], BodyRead(ctx, body, Body::SlotsAt + d)); };
        s.scene_rot[r] = ctx.OpCompositeConstruct(ctx.F32[3], std::array{rf(4 + 3 * r), rf(5 + 3 * r), rf(6 + 3 * r)});
    }
    Id shift = ctx.ConstF32(0.0f);
    if (ctx.runtime_info.native_stereo && ctx.runtime_info.native_multiview) {
        const Id half = ctx.OpBitcast(ctx.F32[1], BodyRead(ctx, body, Body::AttachedAt + 1));
        shift = ctx.OpSelect(ctx.F32[1], ctx.OpIEqual(ctx.U1[1], ctx.NativeEyeIndex(), ctx.u32_zero_value),
                             ctx.OpFNegate(ctx.F32[1], half), half);
    }
    s.scene_shift = ctx.OpCompositeConstruct(ctx.F32[3], std::array{shift, ctx.ConstF32(0.0f), ctx.ConstF32(0.0f)});
    s.scene_voxel = ctx.OpBitcast(ctx.F32[1], BodyRead(ctx, body, Body::GridAt + 2));
}

// A word of the scene maps (the buffer's first descriptor: both eyes' are the same buffer).
Id ScenePtr(EmitContext& ctx, Id index) {
    const auto& s = ctx.vr_light_shadow;
    const auto& buffer = ctx.buffers[s.scene_buffer];
    if (const Id offset = buffer.Offset(EmitContext::PointerSize::B32); Sirit::ValidId(offset)) {
        index = ctx.OpIAdd(ctx.U32[1], index, offset);
    }
    const auto [id, pointer_type] = buffer.Alias(EmitContext::PointerType::U32);
    return ctx.runtime_info.native_stereo
               ? ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, ctx.u32_zero_value, index)
               : ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, index);
}

Id FixtureClear(EmitContext& ctx, Id radius, bool air);

// A flat-view vector in the world's axes (the cubes'), and back.
Id ToWorld(EmitContext& ctx, Id v) {
    const auto& s = ctx.vr_light_shadow;
    return ctx.OpCompositeConstruct(ctx.F32[3], std::array{ctx.OpDot(ctx.F32[1], s.scene_rot[0], v),
                                                           ctx.OpDot(ctx.F32[1], s.scene_rot[1], v),
                                                           ctx.OpDot(ctx.F32[1], s.scene_rot[2], v)});
}
Id ToFlat(EmitContext& ctx, Id w) {
    const auto& s = ctx.vr_light_shadow;
    const auto c = [&](u32 i) { return ctx.OpCompositeExtract(ctx.F32[1], w, i); };
    return ctx.OpFAdd(ctx.F32[3],
                      ctx.OpFAdd(ctx.F32[3], ctx.OpVectorTimesScalar(ctx.F32[3], s.scene_rot[0], c(0)),
                                 ctx.OpVectorTimesScalar(ctx.F32[3], s.scene_rot[1], c(1))),
                      ctx.OpVectorTimesScalar(ctx.F32[3], s.scene_rot[2], c(2)));
}

// Slot `k` (a u32), read from the body buffer: its lamp (the world's axes, from this frame's
// camera), reach dword, shadow strength and flags (light_shadow.h SlotUsed, SlotFresh).
struct SlotRead {
    Id lamp;
    Id reach_k;
    Id strength;
    Id flags;
    Id clear; // its fixture's clearance (the CPU's: the march's rule)
    Id prev;  // where its lamp was when last frame's cube was made
};
SlotRead SceneSlot(EmitContext& ctx, Id k) {
    namespace Body = Shader::Optimization::BodyShadow;
    namespace Scene = Tune::SceneMaps;
    const auto& s = ctx.vr_light_shadow;
    const Id at = ctx.OpIAdd(ctx.U32[1], ctx.ConstU32(Body::SlotsAt + Scene::SlotHeader),
                             ctx.OpIMul(ctx.U32[1], k, ctx.ConstU32(Scene::SlotDwords)));
    const auto ru = [&](u32 c) { return BodyReadAt(ctx, s.scene_body, ctx.OpIAdd(ctx.U32[1], at, ctx.ConstU32(c))); };
    const auto rf = [&](u32 c) { return ctx.OpBitcast(ctx.F32[1], ru(c)); };
    return {ctx.OpCompositeConstruct(ctx.F32[3], std::array{rf(0), rf(1), rf(2)}), rf(3), rf(4), ru(5), rf(6),
            ctx.OpCompositeConstruct(ctx.F32[3], std::array{rf(8), rf(9), rf(10)})};
}
Id SlotHas(EmitContext& ctx, Id flags, u32 bit) {
    return ctx.OpINotEqual(ctx.U1[1], ctx.OpBitwiseAnd(ctx.U32[1], flags, ctx.ConstU32(bit)), ctx.u32_zero_value);
}
// A slot's trace reach from the eye (light_shadow.h TraceFarLevels: slot dword 7; 0 traces nothing).
Id SlotTraceFar(EmitContext& ctx, Id k) {
    namespace Body = Shader::Optimization::BodyShadow;
    namespace Scene = Tune::SceneMaps;
    const Id at = ctx.OpIAdd(ctx.U32[1], ctx.ConstU32(Body::SlotsAt + Scene::SlotHeader + 7),
                             ctx.OpIMul(ctx.U32[1], k, ctx.ConstU32(Scene::SlotDwords)));
    return ctx.OpBitcast(ctx.F32[1], BodyReadAt(ctx, ctx.vr_light_shadow.scene_body, at));
}

// A slot's march step scale (light_shadow.h StepDial: slot dword 11, the same for every slot).
Id SlotStepScale(EmitContext& ctx, Id k) {
    namespace Body = Shader::Optimization::BodyShadow;
    namespace Scene = Tune::SceneMaps;
    const Id at = ctx.OpIAdd(ctx.U32[1], ctx.ConstU32(Body::SlotsAt + Scene::SlotHeader + 11),
                             ctx.OpIMul(ctx.U32[1], k, ctx.ConstU32(Scene::SlotDwords)));
    return ctx.OpBitcast(ctx.F32[1], BodyReadAt(ctx, ctx.vr_light_shadow.scene_body, at));
}

// A loop for k in [0, count) (u32 ids): `body(k)` emits an iteration (its own constructs closed);
// `more()`, when given, is asked in the header too (a bool id: false ends the loop early).
template <typename F, typename M = std::nullptr_t>
void LoopCount(EmitContext& ctx, Id count, F&& body, M&& more = nullptr,
               spv::LoopControlMask control = spv::LoopControlMask::MaskNone) {
    const Id var = Private(ctx, ctx.U32[1]);
    ctx.OpStore(var, ctx.u32_zero_value);
    const Id header = ctx.OpLabel(), body_l = ctx.OpLabel(), cont = ctx.OpLabel(), merge = ctx.OpLabel();
    ctx.OpBranch(header);
    ctx.AddLabel(header);
    const Id k = ctx.OpLoad(ctx.U32[1], var);
    // (The condition before the merge: OpLoopMerge must be just before the block's branch.)
    Id go = ctx.OpULessThan(ctx.U1[1], k, count);
    if constexpr (!std::is_same_v<std::decay_t<M>, std::nullptr_t>) {
        go = ctx.OpLogicalAnd(ctx.U1[1], go, more());
    }
    ctx.OpLoopMerge(merge, cont, control);
    ctx.OpBranchConditional(go, body_l, merge);
    ctx.AddLabel(body_l);
    body(k);
    ctx.OpBranch(cont);
    ctx.AddLabel(cont);
    ctx.OpStore(var, ctx.OpIAdd(ctx.U32[1], k, ctx.ConstU32(1u)));
    ctx.OpBranch(header);
    ctx.AddLabel(merge);
}

// Direction `d` (from the lamp) on its cube: the face's first texel within a slot (u32) and the
// texel's (u, v) on it (floats, 0..Side): the largest axis picks the face (+x -x +y -y +z -z), the
// other two over it give the place.
struct CubePlace {
    Id face_base;
    Id u;
    Id v;
};
CubePlace CubeOf(EmitContext& ctx, Id d) {
    namespace Scene = Tune::SceneMaps;
    const auto c = [&](u32 i) { return ctx.OpCompositeExtract(ctx.F32[1], d, i); };
    const Id x = c(0), y = c(1), z = c(2);
    const Id ax = ctx.OpFAbs(ctx.F32[1], x), ay = ctx.OpFAbs(ctx.F32[1], y), az = ctx.OpFAbs(ctx.F32[1], z);
    const Id is_x = ctx.OpLogicalAnd(ctx.U1[1], ctx.OpFOrdGreaterThanEqual(ctx.U1[1], ax, ay),
                                     ctx.OpFOrdGreaterThanEqual(ctx.U1[1], ax, az));
    const Id is_y = ctx.OpLogicalAnd(ctx.U1[1], ctx.OpLogicalNot(ctx.U1[1], is_x),
                                     ctx.OpFOrdGreaterThanEqual(ctx.U1[1], ay, az));
    const auto pick = [&](Id a, Id b, Id cc) {
        return ctx.OpSelect(ctx.F32[1], is_x, a, ctx.OpSelect(ctx.F32[1], is_y, b, cc));
    };
    const Id major = ctx.OpFMax(ctx.F32[1], pick(ax, ay, az), ctx.ConstF32(1e-6f));
    const Id comp = pick(x, y, z);
    const Id sc = pick(y, x, x);
    const Id tc = pick(z, z, y);
    const Id axis = ctx.OpSelect(ctx.U32[1], is_x, ctx.ConstU32(0u),
                                 ctx.OpSelect(ctx.U32[1], is_y, ctx.ConstU32(1u), ctx.ConstU32(2u)));
    const Id face = ctx.OpIAdd(ctx.U32[1], ctx.OpIMul(ctx.U32[1], axis, ctx.ConstU32(2u)),
                               ctx.OpSelect(ctx.U32[1], ctx.OpFOrdLessThan(ctx.U1[1], comp, ctx.ConstF32(0.0f)),
                                            ctx.ConstU32(1u), ctx.ConstU32(0u)));
    const auto to_texel = [&](Id t) {
        // (t / major + 1) / 2 * Side, kept inside the face.
        const Id f = ctx.OpFMul(ctx.F32[1], ctx.OpFAdd(ctx.F32[1], ctx.OpFDiv(ctx.F32[1], t, major), ctx.ConstF32(1.0f)),
                                ctx.ConstF32(0.5f * float(Scene::Side)));
        return ctx.OpFClamp(ctx.F32[1], f, ctx.ConstF32(0.0f), ctx.ConstF32(float(Scene::Side) - 0.001f));
    };
    return {ctx.OpIMul(ctx.U32[1], face, ctx.ConstU32(Scene::FaceTexels)), to_texel(sc), to_texel(tc)};
}

// This fragment's block (the AO pass at a coarse rate covers bw x bh pixels; 1 x 1 without
// attachment rates): its size and its first pixel.
struct FragmentBlock {
    Id bw, bh, x0, y0;
};
FragmentBlock Block(EmitContext& ctx) {
    const Id u1 = ctx.U32[1];
    const Id fx = ctx.OpConvertFToU(u1, EmitGetAttribute(ctx, IR::Attribute::FragCoord, 0, 0));
    const Id fy = ctx.OpConvertFToU(u1, EmitGetAttribute(ctx, IR::Attribute::FragCoord, 1, 0));
    if (!Sirit::ValidId(ctx.shading_rate)) {
        return {ctx.ConstU32(1u), ctx.ConstU32(1u), fx, fy};
    }
    // Horizontal2Pixels 4, Horizontal4Pixels 8, Vertical2Pixels 1, Vertical4Pixels 2.
    const Id rate = ctx.OpLoad(u1, ctx.shading_rate);
    const auto has = [&](u32 bit) {
        return ctx.OpINotEqual(ctx.U1[1], ctx.OpBitwiseAnd(u1, rate, ctx.ConstU32(bit)), ctx.u32_zero_value);
    };
    const Id bw = ctx.OpSelect(u1, has(8u), ctx.ConstU32(4u), ctx.OpSelect(u1, has(4u), ctx.ConstU32(2u), ctx.ConstU32(1u)));
    const Id bh = ctx.OpSelect(u1, has(2u), ctx.ConstU32(4u), ctx.OpSelect(u1, has(1u), ctx.ConstU32(2u), ctx.ConstU32(1u)));
    return {bw, bh, ctx.OpIMul(u1, ctx.OpUDiv(u1, fx, bw), bw), ctx.OpIMul(u1, ctx.OpUDiv(u1, fy, bh), bh)};
}

// The texel direction `d` falls in, in the slot whose first texel is `slot_base` (u32).
Id CubeTexel(EmitContext& ctx, Id slot_base, Id d) {
    namespace Scene = Tune::SceneMaps;
    const CubePlace at = CubeOf(ctx, d);
    return ctx.OpIAdd(ctx.U32[1], ctx.OpIAdd(ctx.U32[1], slot_base, at.face_base),
                      ctx.OpIAdd(ctx.U32[1], ctx.OpIMul(ctx.U32[1], ctx.OpConvertFToU(ctx.U32[1], at.v), ctx.ConstU32(Scene::Side)),
                                 ctx.OpConvertFToU(ctx.U32[1], at.u)));
}

// Two unit directions square to `d` and to each other: taps round a direction on any face.
std::pair<Id, Id> Tangents(EmitContext& ctx, Id d) {
    const Id f1 = ctx.F32[1], f3 = ctx.F32[3];
    const Id n = ctx.OpNormalize(f3, d);
    const Id flat_up = ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpFAbs(f1, ctx.OpCompositeExtract(f1, n, 1u)), ctx.ConstF32(0.9f));
    const Id up = ctx.OpSelect(f3, flat_up, ctx.ConstF32(0.0f, 1.0f, 0.0f), ctx.ConstF32(1.0f, 0.0f, 0.0f));
    const Id e1 = ctx.OpNormalize(f3, ctx.OpCross(f3, up, n));
    return {e1, ctx.OpCross(f3, n, e1)};
}

// The game's AO pass: this pixel (this eye's view point, s.p) into the cube of every slot whose
// lamp reaches it, its distance from the lamp kept where it is the nearest.
void SceneScatter(EmitContext& ctx) {
    namespace Scene = Tune::SceneMaps;
    auto& s = ctx.vr_light_shadow;
    SceneSetup(ctx);
    if (!Sirit::ValidId(s.scene_count) || !Scene::ScreenSplat()) {
        return;
    }
    const Id flat = ToWorld(ctx, ctx.OpFAdd(ctx.F32[3], s.p, s.scene_shift));
    const Id scope = ctx.ConstU32(static_cast<u32>(spv::Scope::Device));
    const Id half_base = ctx.OpIMul(ctx.U32[1], s.scene_half, ctx.ConstU32(Scene::Texels));
    // The pixel's footprint: its view place's step across a pixel (at most 15 cm: a depth edge's
    // step is no surface); a sample covers that much on each cube, not one texel.
    const Id foot = ctx.OpFMin(
        ctx.F32[1],
        ctx.OpFMax(ctx.F32[1], ctx.OpLength(ctx.F32[1], ctx.OpDPdx(ctx.F32[3], s.p)),
                   ctx.OpLength(ctx.F32[1], ctx.OpDPdy(ctx.F32[3], s.p))),
        ctx.ConstF32(0.15f));
    const Id foot_i = Private(ctx, ctx.U32[1]);
    LoopCount(ctx, ctx.OpSelect(ctx.U32[1], s.ok, s.scene_count, ctx.u32_zero_value), [&](Id k) {
        const SlotRead slot = SceneSlot(ctx, k);
        const Id lamp = slot.lamp;
        const Id reach_k = slot.reach_k;
        const Id d = ctx.OpFSub(ctx.F32[3], flat, lamp);
        const Id d2 = ctx.OpDot(ctx.F32[1], d, d);
        // (Not the lamp's own fixture, nor the hand holding it: its clearance, the march's.)
        const Id clear = slot.clear;
        const Id inside = ctx.OpLogicalAnd(
            ctx.U1[1], SlotHas(ctx, slot.flags, Scene::SlotUsed),
            ctx.OpLogicalAnd(
                ctx.U1[1], ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpFMul(ctx.F32[1], d2, reach_k), ctx.ConstF32(1.0f)),
                ctx.OpFOrdGreaterThan(ctx.U1[1], d2, ctx.OpFMul(ctx.F32[1], clear, clear))));
        const Id then_label = ctx.OpLabel(), done = ctx.OpLabel();
        ctx.OpSelectionMerge(done, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(inside, then_label, done);
        ctx.AddLabel(then_label);
        {
            const Id f1 = ctx.F32[1], f3 = ctx.F32[3], u1 = ctx.U32[1], s1 = ctx.S32[1], b1 = ctx.U1[1];
            const Id dist = ctx.OpSqrt(f1, d2);
            const Id value = ctx.OpBitwiseAnd(u1, ctx.OpBitcast(u1, dist), ctx.ConstU32(~1u));
            // A texel's size at this distance (a face's centre), and the texels the footprint spans,
            // half of them each way (the centre's own counts once).
            const Id texel_m = ctx.OpFMul(f1, dist, ctx.ConstF32(2.0f / float(Scene::Side)));
            const Id spacing = ctx.OpFDiv(f1, foot, ctx.OpFMax(f1, texel_m, ctx.ConstF32(1e-5f)));
            const Id reach_t = ctx.OpUMin(
                u1, ctx.OpConvertFToU(u1, ctx.OpFMax(f1, ctx.OpFAdd(f1, ctx.OpFMul(f1, ctx.OpFSub(f1, spacing, ctx.ConstF32(1.0f)), ctx.ConstF32(0.5f)), ctx.ConstF32(0.5f)),
                                                     ctx.ConstF32(0.0f))),
                ctx.ConstU32(Scene::MaxPixelFoot));
            const Id width = ctx.OpIAdd(u1, ctx.OpIMul(u1, reach_t, ctx.ConstU32(2u)), ctx.ConstU32(1u));
            const Id slot_base = ctx.OpIAdd(u1, half_base, ctx.OpIMul(u1, k, ctx.ConstU32(Scene::SlotTexels)));
            const auto [e1, e2] = Tangents(ctx, d);
            const Id rs = ctx.OpConvertSToF(f1, ctx.OpBitcast(s1, reach_t));
            ctx.OpStore(foot_i, ctx.u32_zero_value);
            LoopCount(ctx, ctx.OpIMul(u1, width, width), [&](Id i) {
                const Id du = ctx.OpFSub(f1, ctx.OpConvertUToF(f1, ctx.OpUMod(u1, i, width)), rs);
                const Id dv = ctx.OpFSub(f1, ctx.OpConvertUToF(f1, ctx.OpUDiv(u1, i, width)), rs);
                const Id tap = ctx.OpFAdd(f3, d, ctx.OpFAdd(f3, ctx.OpVectorTimesScalar(f3, e1, ctx.OpFMul(f1, du, texel_m)),
                                                            ctx.OpVectorTimesScalar(f3, e2, ctx.OpFMul(f1, dv, texel_m))));
                ctx.OpAtomicUMin(u1, ScenePtr(ctx, CubeTexel(ctx, slot_base, tap)), scope, ctx.u32_zero_value, value);
            });
            (void)b1;
        }
        ctx.OpBranch(done);
        ctx.AddLabel(done);
    });
}

// Stage 2 (light_shadow.h SceneMaps::GridSplat; the tester: "I want NPCs and enemies and misc
// geometry etc (from the grid) to be added"): the blocker grid's voxels into the slots' cubes, so
// what the view saw lately casts too, behind and beside it. The AO pass's pixels share the voxels
// out (pixel i of N, both eyes counted: voxels i, i + N, ...); a stamped, live voxel whose tag is
// the cell nearest the camera with its index goes into each slot cube whose lamp reaches it: its
// centre's distance with the low bit set, over enough texels to close the gaps to its neighbours.
Id GridPtr(EmitContext& ctx, Id index);
Id GridAge(EmitContext& ctx, Id word);
void SceneGridSplat(EmitContext& ctx) {
    namespace Body = Shader::Optimization::BodyShadow;
    namespace Scene = Tune::SceneMaps;
    namespace Grid = Tune::Grid;
    auto& s = ctx.vr_light_shadow;
    if (!Scene::GridSplat() || !Grid::Enabled() || !Sirit::ValidId(s.scene_count)) {
        return;
    }
    const auto& buffers = ctx.info.buffers;
    const auto body_it = std::ranges::find(buffers, BufferType::VrBodyCapsules, &BufferResource::buffer_type);
    const auto grid_it = std::ranges::find(buffers, BufferType::VrShadowGrid, &BufferResource::buffer_type);
    if (body_it == buffers.end() || grid_it == buffers.end()) {
        return;
    }
    const u32 body = static_cast<u32>(std::distance(buffers.begin(), body_it));
    s.grid_buffer = static_cast<u32>(std::distance(buffers.begin(), grid_it));
    const auto casters_it = std::ranges::find(buffers, BufferType::VrShadowCasters, &BufferResource::buffer_type);
    const u32 casters = casters_it == buffers.end() ? ~0u : static_cast<u32>(std::distance(buffers.begin(), casters_it));
    const Id f1 = ctx.F32[1], f3 = ctx.F32[3], u1 = ctx.U32[1], s1 = ctx.S32[1], b1 = ctx.U1[1];
    const auto f = [&](float v) { return ctx.ConstF32(v); };
    const auto u = [&](u32 v) { return ctx.ConstU32(v); };
    const auto ru = [&](u32 d) { return BodyRead(ctx, body, Body::GridAt + d); };
    const auto rf = [&](u32 d) { return ctx.OpBitcast(f1, ru(d)); };
    s.grid_stamp = ru(1);
    s.grid_life = ru(3);
    const Id voxel = ctx.OpFMax(f1, rf(2), f(1e-4f));
    const Id inv_voxel = ctx.OpFDiv(f1, f(1.0f), voxel);
    // The grid's rows (flat view -> world): the camera's place, and their columns (world -> flat).
    std::array<std::array<Id, 4>, 3> m;
    for (u32 r = 0; r < 3; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            m[r][c] = rf(8 + 4 * r + c);
        }
    }
    const Id cam = ctx.OpCompositeConstruct(f3, std::array{m[0][3], m[1][3], m[2][3]});
    std::array<Id, 3> col;
    for (u32 c = 0; c < 3; ++c) {
        col[c] = ctx.OpCompositeConstruct(f3, std::array{m[0][c], m[1][c], m[2][c]});
    }
    const Id cam_cell = ctx.OpConvertFToS(ctx.S32[3], ctx.OpFloor(f3, ctx.OpVectorTimesScalar(f3, cam, inv_voxel)));
    // This pixel's number in the pass and the pass's count (its size from the uv's step a pixel:
    // rounded down, so a doubt makes two pixels share voxels, never leaves voxels out).
    const auto size_of = [&](u32 k) {
        const Id step = ctx.OpFMax(f1, ctx.OpCompositeExtract(f1, s.pixel, k), f(1e-6f));
        return ctx.OpConvertFToU(u1, ctx.OpFClamp(f1, ctx.OpFAdd(f1, ctx.OpFDiv(f1, f(1.0f), step), f(0.01f)),
                                                  f(1.0f), f(16384.0f)));
    };
    const FragmentBlock blk = Block(ctx);
    const Id w = ctx.OpIMul(u1, size_of(0), blk.bw), h = ctx.OpIMul(u1, size_of(1), blk.bh);
    Id eye = u(0u), eyes = u(1u);
    if (ctx.runtime_info.native_stereo && ctx.runtime_info.native_multiview) {
        eye = ctx.NativeEyeIndex();
        eyes = u(2u);
    }
    const Id stride = ctx.OpIMul(u1, ctx.OpIMul(u1, w, h), eyes);
    const Id scope = u(static_cast<u32>(spv::Scope::Device));
    const Id index = Private(ctx, u1);
    const Id foot_i = Private(ctx, u1);
    // (Every pixel the fragment covers: each pixel's share of the voxels.)
    LoopCount(ctx, ctx.OpIMul(u1, blk.bw, blk.bh), [&](Id cell) {
    const Id fx = ctx.OpIAdd(u1, blk.x0, ctx.OpUMod(u1, cell, blk.bw));
    const Id fy = ctx.OpIAdd(u1, blk.y0, ctx.OpUDiv(u1, cell, blk.bw));
    const Id first = ctx.OpIAdd(u1, ctx.OpIMul(u1, ctx.OpIAdd(u1, ctx.OpIMul(u1, eye, h), fy), w), fx);
    const Id go = ctx.OpLogicalAnd(
        b1, ctx.OpLogicalAnd(b1, ctx.OpINotEqual(b1, ru(0), ctx.u32_zero_value),
                             ctx.OpINotEqual(b1, BodyRead(ctx, body, Body::SlotsAt + 1), ctx.u32_zero_value)),
        ctx.OpLogicalAnd(b1, s.ok, ctx.OpLogicalAnd(b1, ctx.OpULessThan(b1, fx, w), ctx.OpULessThan(b1, fy, h))));
    ctx.OpStore(index, first);
    const Id header = ctx.OpLabel(), body_l = ctx.OpLabel(), cont = ctx.OpLabel(), merge = ctx.OpLabel();
    ctx.OpBranch(header);
    ctx.AddLabel(header);
    const Id v = ctx.OpLoad(u1, index);
    const Id more = ctx.OpLogicalAnd(b1, go, ctx.OpULessThan(b1, v, u(Grid::Voxels)));
    ctx.OpLoopMerge(merge, cont, spv::LoopControlMask::MaskNone);
    ctx.OpBranchConditional(more, body_l, merge);
    ctx.AddLabel(body_l);
    {
        const Id word = ctx.OpLoad(u1, GridPtr(ctx, v));
        const Id age = GridAge(ctx, word);
        // (This frame's stamps count too: pixels stamp the grid in this same pass, so skipping them
        // made a cell re-stamped this frame count or not by which pixel ran first. A code review,
        // 2026-11-13 evening.)
        const Id live = ctx.OpLogicalAnd(
            b1, ctx.OpINotEqual(b1, ctx.OpBitwiseAnd(u1, word, u(Grid::StampMask)), ctx.u32_zero_value),
            ctx.OpULessThan(b1, age, s.grid_life));
        const Id live_l = ctx.OpLabel(), live_done = ctx.OpLabel();
        ctx.OpSelectionMerge(live_done, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(live, live_l, live_done);
        ctx.AddLabel(live_l);
        {
            // Its cell: the index's low bits nearest the camera's cell, its tag the word's.
            const Id tags = ctx.OpShiftRightLogical(u1, word, u(Grid::StampBits));
            const auto axis = [&](Id low, u32 k, u32 side, u32 shift, u32 tag_at) {
                const Id cc = ctx.OpCompositeExtract(s1, cam_cell, k);
                Id diff = ctx.OpBitcast(s1, ctx.OpBitwiseAnd(u1, ctx.OpISub(u1, low, ctx.OpBitcast(u1, cc)), u(side - 1u)));
                diff = ctx.OpSelect(s1, ctx.OpSGreaterThanEqual(b1, diff, ctx.ConstS32(s32(side / 2))),
                                    ctx.OpISub(s1, diff, ctx.ConstS32(s32(side))), diff);
                const Id c = ctx.OpIAdd(s1, cc, diff);
                const Id tag = ctx.OpBitwiseAnd(u1, ctx.OpBitcast(u1, ctx.OpShiftRightArithmetic(s1, c, u(shift))), u(15u));
                const Id want = ctx.OpBitwiseAnd(u1, ctx.OpShiftRightLogical(u1, tags, u(tag_at)), u(15u));
                return std::pair{ctx.OpConvertSToF(f1, c), ctx.OpIEqual(b1, tag, want)};
            };
            const Id lx = ctx.OpBitwiseAnd(u1, v, u(Grid::SideX - 1u));
            const Id lz = ctx.OpBitwiseAnd(u1, ctx.OpShiftRightLogical(u1, v, u(Grid::ShiftX)), u(Grid::SideZ - 1u));
            const Id ly = ctx.OpShiftRightLogical(u1, v, u(Grid::ShiftX + Grid::ShiftZ));
            const auto [cx, okx] = axis(lx, 0, Grid::SideX, Grid::ShiftX, 0u);
            const auto [cy, oky] = axis(ly, 1, Grid::SideY, Grid::ShiftY, 4u);
            const auto [cz, okz] = axis(lz, 2, Grid::SideZ, Grid::ShiftZ, 8u);
            const Id match = ctx.OpLogicalAnd(b1, okx, ctx.OpLogicalAnd(b1, oky, okz));
            const Id match_l = ctx.OpLabel(), match_done = ctx.OpLabel();
            ctx.OpSelectionMerge(match_done, spv::SelectionControlMask::MaskNone);
            ctx.OpBranchConditional(match, match_l, match_done);
            ctx.AddLabel(match_l);
            {
                const Id world = ctx.OpVectorTimesScalar(
                    f3, ctx.OpFAdd(f3, ctx.OpCompositeConstruct(f3, std::array{cx, cy, cz}), ctx.ConstF32(0.5f, 0.5f, 0.5f)),
                    voxel);
                const Id flat = ctx.OpFSub(f3, world, cam); // (the world's axes: the cubes')
                const Id half_base = ctx.OpIMul(u1, s.scene_half, u(Scene::Texels));
                // Seen by this eye now (projected on to its picture, the depth there): on the
                // surface the view sees (the screen casts it, finer) or in front of it in empty air
                // (a voxel the air has not yet cleared): left out. Behind what the view sees, or off
                // the picture: it casts.
                Id in_view = ctx.false_value;
                if (Scene::Mask() && s.has_inv) {
                    const Id v = ctx.OpFSub(f3, ToFlat(ctx, flat), s.scene_shift);
                    const auto clip = Transform(ctx, s.inv, ctx.OpCompositeExtract(f1, v, 0u), ctx.OpCompositeExtract(f1, v, 1u),
                                                ctx.OpCompositeExtract(f1, v, 2u));
                    const Id cw = ctx.OpFMax(f1, clip[3], f(1e-6f));
                    const Id ndc = ctx.OpCompositeConstruct(ctx.F32[2], std::array{ctx.OpFDiv(f1, clip[0], cw), ctx.OpFDiv(f1, clip[1], cw)});
                    const Id uvp = ctx.OpFAdd(ctx.F32[2], ctx.OpFMul(ctx.F32[2], s.a, ndc), s.b);
                    const Id ux = ctx.OpCompositeExtract(f1, uvp, 0u), uy = ctx.OpCompositeExtract(f1, uvp, 1u);
                    const Id on_picture = ctx.OpLogicalAnd(
                        b1, ctx.OpFOrdGreaterThan(b1, clip[3], f(1e-4f)),
                        ctx.OpLogicalAnd(b1, ctx.OpLogicalAnd(b1, ctx.OpFOrdGreaterThan(b1, ux, f(0.0f)), ctx.OpFOrdLessThan(b1, ux, f(1.0f))),
                                         ctx.OpLogicalAnd(b1, ctx.OpFOrdGreaterThan(b1, uy, f(0.0f)), ctx.OpFOrdLessThan(b1, uy, f(1.0f)))));
                    const Id uvc = ctx.OpFClamp(ctx.F32[2], uvp, ctx.ConstF32(0.0f, 0.0f), ctx.ConstF32(1.0f, 1.0f));
                    const Id sampled = ctx.OpCompositeExtract(
                        f1, EmitImageSampleExplicitLod(ctx, nullptr, ctx.info.vr_light_shadow.depth_image, uvc, f(0.0f), IR::Value{}), 0u);
                    const auto q = Transform(ctx, s.rows, ctx.OpCompositeExtract(f1, ndc, 0u), ctx.OpCompositeExtract(f1, ndc, 1u), sampled);
                    const Id qw = ctx.OpFDiv(f1, f(1.0f), q[3]);
                    const Id seen_dist = ctx.OpLength(f1, ctx.OpCompositeConstruct(f3, std::array{ctx.OpFMul(f1, q[0], qw), ctx.OpFMul(f1, q[1], qw),
                                                                                                  ctx.OpFMul(f1, q[2], qw)}));
                    const Id voxel_dist = ctx.OpLength(f1, v);
                    in_view = ctx.OpLogicalAnd(
                        b1, on_picture,
                        ctx.OpFOrdLessThan(b1, voxel_dist, ctx.OpFAdd(f1, seen_dist, ctx.OpFMul(f1, voxel, f(1.5f)))));
                }
                const Id min_d2 = ctx.OpFMul(f1, ctx.OpFMul(f1, voxel, voxel), f(2.25f));
                LoopCount(ctx, s.scene_count, [&](Id k) {
                    const SlotRead slot = SceneSlot(ctx, k);
                    const Id lamp = slot.lamp;
                    const Id reach_k = slot.reach_k;
                    const Id d = ctx.OpFSub(f3, flat, lamp);
                    const Id d2 = ctx.OpDot(f1, d, d);
                    const Id inside = ctx.OpLogicalAnd(
                        b1, ctx.OpLogicalAnd(b1, SlotHas(ctx, slot.flags, Scene::SlotUsed), ctx.OpLogicalNot(b1, in_view)),
                        ctx.OpLogicalAnd(b1, ctx.OpFOrdLessThan(b1, ctx.OpFMul(f1, d2, reach_k), f(1.0f)),
                                         ctx.OpFOrdGreaterThan(b1, d2, ctx.OpFMax(f1, min_d2, ctx.OpFMul(f1, slot.clear, slot.clear)))));
                    const Id in_l = ctx.OpLabel(), in_done = ctx.OpLabel();
                    ctx.OpSelectionMerge(in_done, spv::SelectionControlMask::MaskNone);
                    ctx.OpBranchConditional(inside, in_l, in_done);
                    ctx.AddLabel(in_l);
                    {
                        const Id value = ctx.OpBitwiseOr(u1, ctx.OpBitcast(u1, ctx.OpSqrt(f1, d2)), u(1u));
                        const CubePlace at = CubeOf(ctx, d);
                        const Id major = ctx.OpFMax(
                            f1, ctx.OpFMax(f1, ctx.OpFAbs(f1, ctx.OpCompositeExtract(f1, d, 0u)),
                                           ctx.OpFAbs(f1, ctx.OpCompositeExtract(f1, d, 1u))),
                            ctx.OpFMax(f1, ctx.OpFAbs(f1, ctx.OpCompositeExtract(f1, d, 2u)), f(1e-4f)));
                        // Neighbouring voxels lie `spacing` texels apart here: cover half of it each way.
                        const Id spacing = ctx.OpFDiv(f1, ctx.OpFMul(f1, voxel, f(0.5f * float(Scene::Side))), major);
                        const Id reach_t = ctx.OpUMin(
                            u1, ctx.OpConvertFToU(u1, ctx.OpFMax(f1, ctx.OpFAdd(f1, ctx.OpFMul(f1, ctx.OpFSub(f1, spacing, f(1.0f)), f(0.5f)), f(0.5f)), f(0.0f))),
                            u(Scene::MaxFoot));
                        const Id width = ctx.OpIAdd(u1, ctx.OpIMul(u1, reach_t, u(2u)), u(1u));
                        const Id taps = ctx.OpIMul(u1, width, width);
                        const Id base = ctx.OpIAdd(u1, ctx.OpIAdd(u1, half_base, ctx.OpIMul(u1, k, u(Scene::SlotTexels))), at.face_base);
                        const Id ui = ctx.OpConvertFToS(s1, at.u), vi = ctx.OpConvertFToS(s1, at.v);
                        const Id rs = ctx.OpBitcast(s1, reach_t);
                        ctx.OpStore(foot_i, ctx.u32_zero_value);
                        const Id fh = ctx.OpLabel(), fb = ctx.OpLabel(), fc = ctx.OpLabel(), fm = ctx.OpLabel();
                        ctx.OpBranch(fh);
                        ctx.AddLabel(fh);
                        const Id i = ctx.OpLoad(u1, foot_i);
                        const Id more = ctx.OpULessThan(b1, i, taps);
                        ctx.OpLoopMerge(fm, fc, spv::LoopControlMask::MaskNone);
                        ctx.OpBranchConditional(more, fb, fm);
                        ctx.AddLabel(fb);
                        {
                            const Id du = ctx.OpISub(s1, ctx.OpBitcast(s1, ctx.OpUMod(u1, i, width)), rs);
                            const Id dv = ctx.OpISub(s1, ctx.OpBitcast(s1, ctx.OpUDiv(u1, i, width)), rs);
                            const auto clampi = [&](Id a, Id o) {
                                return ctx.OpBitcast(u1, ctx.OpSClamp(s1, ctx.OpIAdd(s1, a, o), ctx.ConstS32(0),
                                                                     ctx.ConstS32(s32(Scene::Side) - 1)));
                            };
                            const Id texel = ctx.OpIAdd(
                                u1, base, ctx.OpIAdd(u1, ctx.OpIMul(u1, clampi(vi, dv), u(Scene::Side)), clampi(ui, du)));
                            ctx.OpAtomicUMin(u1, ScenePtr(ctx, texel), scope, ctx.u32_zero_value, value);
                            ctx.OpStore(foot_i, ctx.OpIAdd(u1, i, u(1u)));
                        }
                        ctx.OpBranch(fc);
                        ctx.AddLabel(fc);
                        ctx.OpBranch(fh);
                        ctx.AddLabel(fm);
                    }
                    ctx.OpBranch(in_done);
                    ctx.AddLabel(in_done);
                });
            }
            ctx.OpBranch(match_done);
            ctx.AddLabel(match_done);
        }
        ctx.OpBranch(live_done);
        ctx.AddLabel(live_done);
        ctx.OpStore(index, ctx.OpIAdd(u1, v, ctx.OpUMax(u1, stride, u(1u))));
    }
    ctx.OpBranch(cont);
    ctx.AddLabel(cont);
    ctx.OpBranch(header);
    ctx.AddLabel(merge);
    });
}

// A lamp's slot `slot` (u32): the share of 3x3 cube texels round direction `d` (the receiver less
// the lamp, flat view) whose surface lies short of the receiver's own: 0 lit, 1 in shadow. With
// `blocker`: the nearest such surface's distance from the lamp too (a large one where none blocks).
Id SceneOcclusion(EmitContext& ctx, Id slot, Id d, Id half_base, Id normal, Id* blocker = nullptr) {
    namespace Scene = Tune::SceneMaps;
    const auto& s = ctx.vr_light_shadow;
    // The normal offset: off its own surface, towards the light's side, by NormalTexels of a
    // texel's size at its distance (a trusted normal only).
    {
        const Id f1 = ctx.F32[1], f3 = ctx.F32[3];
        const Id dist0 = ctx.OpLength(f1, d);
        const Id facing = ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpDot(f1, normal, d), ctx.ConstF32(0.0f));
        const Id n = ctx.OpSelect(f3, facing, normal, ctx.OpFNegate(f3, normal));
        const Id lift = ctx.OpFMax(f1, ctx.ConstF32(Scene::NormalMinM),
                                   ctx.OpFMul(f1, dist0, ctx.ConstF32(Scene::NormalTexels * 2.0f / float(Scene::Side))));
        d = ctx.OpSelect(f3, s.normal_ok, ctx.OpFAdd(f3, d, ctx.OpVectorTimesScalar(f3, n, lift)), d);
    }
    const Id dist = ctx.OpLength(ctx.F32[1], d);
    const Id bias = ctx.OpFAdd(ctx.F32[1], ctx.ConstF32(Scene::BiasM),
                               ctx.OpFMul(ctx.F32[1], dist, ctx.ConstF32(Scene::BiasTexels * 2.0f / float(Scene::Side))));
    // The surface's slope to the light (tan of the angle off its normal; unknown: the most).
    const Id to_light = ctx.OpFNegate(ctx.F32[3], ctx.OpVectorTimesScalar(
                                                      ctx.F32[3], d, ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f),
                                                                                ctx.OpFMax(ctx.F32[1], dist, ctx.ConstF32(1e-4f)))));
    const Id cosine = ctx.OpFClamp(ctx.F32[1], ctx.OpDot(ctx.F32[1], normal, to_light), ctx.ConstF32(0.05f), ctx.ConstF32(1.0f));
    const Id tangent = ctx.OpFDiv(ctx.F32[1], ctx.OpSqrt(ctx.F32[1], ctx.OpFSub(ctx.F32[1], ctx.ConstF32(1.0f), ctx.OpFMul(ctx.F32[1], cosine, cosine))), cosine);
    const Id slope = ctx.OpSelect(ctx.F32[1], s.normal_ok, ctx.OpFMin(ctx.F32[1], tangent, ctx.ConstF32(Scene::SlopeMax())),
                                  ctx.ConstF32(Scene::SlopeMax()));
    // A surface the view saw: SlopeTexels of a texel's size more per unit of slope.
    const Id ref = ctx.OpFSub(ctx.F32[1], ctx.OpFSub(ctx.F32[1], dist, bias),
                              ctx.OpFMul(ctx.F32[1], ctx.OpFMul(ctx.F32[1], dist, ctx.ConstF32(Scene::SlopeTexels() * 2.0f / float(Scene::Side))),
                                         slope));
    // A grid entry (stage 2: low bit set) passes a voxel nearer, up to GridSlopeMax more on a slope
    // to the light (the surface's own voxels lie along a grazing ray).
    Id grid_ref = ref;
    if (Scene::GridSplat() && Sirit::ValidId(s.scene_voxel)) {
        grid_ref = ctx.OpFSub(ctx.F32[1], ref,
                              ctx.OpFMul(ctx.F32[1], s.scene_voxel,
                                         ctx.OpFAdd(ctx.F32[1], ctx.ConstF32(1.0f), ctx.OpFMin(ctx.F32[1], slope, ctx.ConstF32(Scene::GridSlopeMax)))));
    }
    const Id slot_base = ctx.OpIAdd(ctx.U32[1], half_base, ctx.OpIMul(ctx.U32[1], slot, ctx.ConstU32(Scene::SlotTexels)));
    // A 3x3 tent of taps a texel apart round the direction (weights 1 2 1 each way, over 16), each on
    // whatever face it falls: smooth edges, no seams where faces meet.
    const auto [e1, e2] = Tangents(ctx, d);
    const Id texel_m = ctx.OpFMul(ctx.F32[1], dist, ctx.ConstF32(2.0f / float(Scene::Side)));
    Id blocked = ctx.ConstF32(0.0f);
    Id nearest = ctx.ConstF32(1e30f);
    for (s32 dv = -1; dv <= 1; ++dv) {
        for (s32 du = -1; du <= 1; ++du) {
            const Id tap = ctx.OpFAdd(ctx.F32[3], d,
                                      ctx.OpFAdd(ctx.F32[3], ctx.OpVectorTimesScalar(ctx.F32[3], e1, ctx.OpFMul(ctx.F32[1], ctx.ConstF32(float(du)), texel_m)),
                                                 ctx.OpVectorTimesScalar(ctx.F32[3], e2, ctx.OpFMul(ctx.F32[1], ctx.ConstF32(float(dv)), texel_m))));
            const float w = float((du == 0 ? 2 : 1) * (dv == 0 ? 2 : 1)) / 16.0f;
            const Id bits = ctx.OpLoad(ctx.U32[1], ScenePtr(ctx, CubeTexel(ctx, slot_base, tap)));
            const Id stored = ctx.OpBitcast(ctx.F32[1], bits);
            const Id from_grid = ctx.OpINotEqual(ctx.U1[1], ctx.OpBitwiseAnd(ctx.U32[1], bits, ctx.ConstU32(1u)),
                                                 ctx.u32_zero_value);
            const Id blocks = ctx.OpFOrdLessThan(ctx.U1[1], stored, ctx.OpSelect(ctx.F32[1], from_grid, grid_ref, ref));
            blocked = ctx.OpFAdd(ctx.F32[1], blocked, ctx.OpSelect(ctx.F32[1], blocks, ctx.ConstF32(w), ctx.ConstF32(0.0f)));
            if (blocker != nullptr) {
                nearest = ctx.OpSelect(ctx.F32[1], blocks, ctx.OpFMin(ctx.F32[1], nearest, stored), nearest);
            }
        }
    }
    if (blocker != nullptr) {
        *blocker = nearest;
    }
    return blocked;
}

// (`upkeep`: this pixel's share of the grid's fill too; false: the header alone, for lookups: High's
// hero lamps, whose programs read the grid the AO pass fills.)
void GridSetup(EmitContext& ctx, bool upkeep = true);
Id CasterVisibility(EmitContext& ctx, u32 buf, Id p, Id lamp, Id clear);

// The mask's last word (light_shadow.h MaskNormals): a view distance and a unit facing (view space)
// packed (the distance a half float in the low 16 bits, the facing octahedral, 6 bits a component,
// in bits 16..27), and back. Bits 28..31: the size of the block the pass shaded as one (log2 of
// its width, then of its height, in mask pixels: 0..2), for the lamp read's blend between blocks.
Id MaskDepthPack(EmitContext& ctx, Id dist, Id n, Id block_bits) {
    const Id f1 = ctx.F32[1], u1 = ctx.U32[1], b1 = ctx.U1[1];
    const auto f = [&](float v) { return ctx.ConstF32(v); };
    const auto x = [&](Id v, u32 i) { return ctx.OpCompositeExtract(f1, v, i); };
    const Id half = ctx.OpBitwiseAnd(u1, ctx.OpPackHalf2x16(u1, ctx.OpCompositeConstruct(ctx.F32[2], std::array{dist, f(0.0f)})),
                                     ctx.ConstU32(0xFFFFu));
    const Id l1 = ctx.OpFMax(f1, ctx.OpFAdd(f1, ctx.OpFAdd(f1, ctx.OpFAbs(f1, x(n, 0)), ctx.OpFAbs(f1, x(n, 1))), ctx.OpFAbs(f1, x(n, 2))),
                             f(1e-6f));
    const Id vx = ctx.OpFDiv(f1, x(n, 0), l1), vy = ctx.OpFDiv(f1, x(n, 1), l1);
    // (The far half folded over: (1 - |v.yx|) with v's signs.)
    const auto sgn = [&](Id v) { return ctx.OpSelect(f1, ctx.OpFOrdGreaterThanEqual(b1, v, f(0.0f)), f(1.0f), f(-1.0f)); };
    const Id lower = ctx.OpFOrdLessThan(b1, x(n, 2), f(0.0f));
    const Id ox = ctx.OpSelect(f1, lower, ctx.OpFMul(f1, ctx.OpFSub(f1, f(1.0f), ctx.OpFAbs(f1, vy)), sgn(vx)), vx);
    const Id oy = ctx.OpSelect(f1, lower, ctx.OpFMul(f1, ctx.OpFSub(f1, f(1.0f), ctx.OpFAbs(f1, vx)), sgn(vy)), vy);
    const auto q = [&](Id v) {
        return ctx.OpConvertFToU(u1, ctx.OpFAdd(f1, ctx.OpFMul(f1, ctx.OpFClamp(f1, ctx.OpFAdd(f1, ctx.OpFMul(f1, v, f(0.5f)), f(0.5f)), f(0.0f), f(1.0f)),
                                                         f(63.0f)),
                                                f(0.5f)));
    };
    return ctx.OpBitwiseOr(u1, ctx.OpBitwiseOr(u1, half, ctx.OpShiftLeftLogical(u1, block_bits, ctx.ConstU32(28u))),
                           ctx.OpBitwiseOr(u1, ctx.OpShiftLeftLogical(u1, q(ox), ctx.ConstU32(16u)),
                                           ctx.OpShiftLeftLogical(u1, q(oy), ctx.ConstU32(22u))));
}
Id MaskDepthOf(EmitContext& ctx, Id word) {
    return ctx.OpCompositeExtract(ctx.F32[1], ctx.OpUnpackHalf2x16(ctx.F32[2], ctx.OpBitwiseAnd(ctx.U32[1], word, ctx.ConstU32(0xFFFFu))), 0u);
}
Id MaskFacingOf(EmitContext& ctx, Id word) {
    const Id f1 = ctx.F32[1], u1 = ctx.U32[1], b1 = ctx.U1[1];
    const auto f = [&](float v) { return ctx.ConstF32(v); };
    const auto unq = [&](u32 shift) {
        const Id byte = ctx.OpBitwiseAnd(u1, ctx.OpShiftRightLogical(u1, word, ctx.ConstU32(shift)), ctx.ConstU32(63u));
        return ctx.OpFSub(f1, ctx.OpFMul(f1, ctx.OpConvertUToF(f1, byte), f(2.0f / 63.0f)), f(1.0f));
    };
    Id vx = unq(16), vy = unq(22);
    const Id vz = ctx.OpFSub(f1, ctx.OpFSub(f1, f(1.0f), ctx.OpFAbs(f1, vx)), ctx.OpFAbs(f1, vy));
    const Id t = ctx.OpFMax(f1, ctx.OpFNegate(f1, vz), f(0.0f));
    vx = ctx.OpFAdd(f1, vx, ctx.OpSelect(f1, ctx.OpFOrdGreaterThanEqual(b1, vx, f(0.0f)), ctx.OpFNegate(f1, t), t));
    vy = ctx.OpFAdd(f1, vy, ctx.OpSelect(f1, ctx.OpFOrdGreaterThanEqual(b1, vy, f(0.0f)), ctx.OpFNegate(f1, t), t));
    return ctx.OpNormalize(ctx.F32[3], ctx.OpCompositeConstruct(ctx.F32[3], std::array{vx, vy, vz}));
}

// A word of the shadow mask.
Id MaskPtr(EmitContext& ctx, Id index) {
    const auto& s = ctx.vr_light_shadow;
    const auto& buffer = ctx.buffers[s.mask_buffer];
    if (const Id offset = buffer.Offset(EmitContext::PointerSize::B32); Sirit::ValidId(offset)) {
        index = ctx.OpIAdd(ctx.U32[1], index, offset);
    }
    const auto [id, pointer_type] = buffer.Alias(EmitContext::PointerType::U32);
    return ctx.runtime_info.native_stereo
               ? ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, ctx.u32_zero_value, index)
               : ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, index);
}

// A word of the edge pass's buffer (light_shadow.h SceneMaps::EdgePass).
Id EdgesPtr(EmitContext& ctx, u32 handle, Id index) {
    const auto& buffer = ctx.buffers[handle];
    if (const Id offset = buffer.Offset(EmitContext::PointerSize::B32); Sirit::ValidId(offset)) {
        index = ctx.OpIAdd(ctx.U32[1], index, offset);
    }
    const auto [id, pointer_type] = buffer.Alias(EmitContext::PointerType::U32);
    return ctx.runtime_info.native_stereo
               ? ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, ctx.u32_zero_value, index)
               : ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, index);
}

// The old march (below), its step rule and the other eye (light_shadow.h SceneMaps::MarchMode).
Id March(EmitContext& ctx, Id l, Id dist, Id end, Id count, Id delta, bool adaptive);
Id MarchSteps(EmitContext& ctx, Id l, Id w, Id here, float share, Id coarse = Id{}, Id boost = Id{});
void SetupOtherEye(EmitContext& ctx);

// The AO pass (light_shadow.h SceneMaps::Mask): this half-res pixel's shadow from every slot lamp
// whose cube of last frame holds its lamp (not fresh): the cube's 3x3 test, his body (his three
// lamps), times the lamp's strength; 4 bits a slot (15: lit) into the mask.
void SceneMaskWrite(EmitContext& ctx) {
    namespace Body = Shader::Optimization::BodyShadow;
    namespace Scene = Tune::SceneMaps;
    auto& s = ctx.vr_light_shadow;
    if (!Scene::Mask() || !Sirit::ValidId(s.scene_count)) {
        return;
    }
    const auto& buffers = ctx.info.buffers;
    const auto mask_it = std::ranges::find(buffers, BufferType::VrShadowMask, &BufferResource::buffer_type);
    if (mask_it == buffers.end()) {
        return;
    }
    s.mask_buffer = static_cast<u32>(std::distance(buffers.begin(), mask_it));
    // The depth pyramid (light_shadow.h SceneMaps::Pyramid): its steps' scale where it was built
    // (the header's size not 0; the stand-in's is), 1 elsewhere.
    s.pyr_buffer = ~0u;
    s.pyr_scale = Id{};
    if (const auto pyr_it = std::ranges::find(buffers, BufferType::VrDepthPyramid, &BufferResource::buffer_type);
        pyr_it != buffers.end() && Tune::SceneMaps::Pyramid()) {
        s.pyr_buffer = static_cast<u32>(std::distance(buffers.begin(), pyr_it));
        s.pyr_scale = ctx.OpSelect(ctx.F32[1],
                                   ctx.OpINotEqual(ctx.U1[1], BodyRead(ctx, s.pyr_buffer, 0u), ctx.u32_zero_value),
                                   ctx.ConstF32(Tune::SceneMaps::PyramidStepScale()), ctx.ConstF32(1.0f));
    }
    const auto maps_it = std::ranges::find(buffers, BufferType::VrBodyMaps, &BufferResource::buffer_type);
    const u32 body_maps = maps_it == buffers.end() ? ~0u : static_cast<u32>(std::distance(buffers.begin(), maps_it));
    const auto casters_it = std::ranges::find(buffers, BufferType::VrShadowCasters, &BufferResource::buffer_type);
    const u32 casters = casters_it == buffers.end() ? ~0u : static_cast<u32>(std::distance(buffers.begin(), casters_it));
    const Id f1 = ctx.F32[1], f3 = ctx.F32[3], u1 = ctx.U32[1], b1 = ctx.U1[1];
    const auto u = [&](u32 v) { return ctx.ConstU32(v); };
    const Id flat_p = ctx.OpFAdd(f3, s.p, s.scene_shift);
    const Id wp = ToWorld(ctx, flat_p);
    const Id nw = ToWorld(ctx, s.normal);
    const Id prev_base = ctx.OpIMul(u1, ctx.OpISub(u1, u(1u), s.scene_half), u(Scene::Texels));
    const Id words_type = ctx.TypeArray(u1, u(Scene::MaskWords));
    const Id words = Private(ctx, words_type);
    const Id word_ptr = ctx.TypePointer(spv::StorageClass::Private, u1);
    if (Scene::MaskShow()) {
        s.mask_min = Private(ctx, f1);
        ctx.OpStore(s.mask_min, ctx.ConstF32(1.0f));
    }
    for (u32 j = 0; j < Scene::MaskSlotWords; ++j) {
        ctx.OpStore(ctx.OpAccessChain(word_ptr, words, u(j)), u(~0u));
    }
    // Slot k's 4 bits in the words: q, or the lower of q and what they hold (`keep_min`). (VR_MARCH_OPT
    // 32: every word by a constant index, the right one picked by a select: a private array indexed
    // at run time lives in local memory, not registers.)
    const auto put_bits = [&](Id k, Id q, bool keep_min) {
        const Id sh = ctx.OpShiftLeftLogical(u1, ctx.OpBitwiseAnd(u1, k, u(7u)), u(2u));
        const Id keep = ctx.OpNot(u1, ctx.OpShiftLeftLogical(u1, u(15u), sh));
        const auto merged = [&](Id old) {
            const Id v = keep_min ? ctx.OpUMin(u1, ctx.OpBitwiseAnd(u1, ctx.OpShiftRightLogical(u1, old, sh), u(15u)), q) : q;
            return ctx.OpBitwiseOr(u1, ctx.OpBitwiseAnd(u1, old, keep), ctx.OpShiftLeftLogical(u1, v, sh));
        };
        if (Scene::Opt(32)) {
            const Id word = ctx.OpShiftRightLogical(u1, k, u(3u));
            for (u32 j = 0; j < Scene::MaskSlotWords; ++j) {
                const Id ptr = ctx.OpAccessChain(word_ptr, words, u(j));
                const Id old = ctx.OpLoad(u1, ptr);
                ctx.OpStore(ptr, ctx.OpSelect(u1, ctx.OpIEqual(b1, word, u(j)), merged(old), old));
            }
        } else {
            const Id ptr = ctx.OpAccessChain(word_ptr, words, ctx.OpShiftRightLogical(u1, k, u(3u)));
            ctx.OpStore(ptr, merged(ctx.OpLoad(u1, ptr)));
        }
    };
    // (The last word: the view distance and the surface's facing, light_shadow.h MaskNormals; a
    // normal that cannot be trusted: facing the eye.)
    // (... and the block this fragment stands for: 1, 2 or 4 mask pixels a side, log2 = side >> 1.)
    const FragmentBlock own_block = Block(ctx);
    const Id block_bits = ctx.OpBitwiseOr(u1, ctx.OpShiftRightLogical(u1, own_block.bw, u(1u)),
                                          ctx.OpShiftLeftLogical(u1, ctx.OpShiftRightLogical(u1, own_block.bh, u(1u)), u(2u)));
    ctx.OpStore(ctx.OpAccessChain(word_ptr, words, u(Scene::MaskSlotWords)),
                MaskDepthPack(ctx, ctx.OpLength(f1, s.p),
                              ctx.OpSelect(f3, s.normal_ok, s.normal, ctx.OpNormalize(f3, ctx.OpFNegate(f3, s.p))), block_bits));
    // Contact shadows (light_shadow.h Contact*): the lamp lighting this pixel the most, its place
    // in this eye's view, its shadow's share (strength, edge-on and rim fades) and clearance.
    const bool trace_all = Scene::TraceAll() && s.has_inv;
    // (The strongest lamp's contact trace only when not every lamp is traced: its tracking was dead
    // work beside the full trace. A code review, 2026-11-13 evening.)
    const bool contact = Scene::Contact() && s.has_inv && !trace_all;
    // The trace's own per-pixel values, outside any branch (derivatives are undefined where other
    // pixels skip): the pixel's footprint (its view place's step across a pixel), a fixed per-pixel
    // jitter (interleaved gradient noise of its place, the same every frame), and where it runs (a
    // pixel the pass shades on its own, its coarse blocks would show squares, within ContactFarM).
    Id trace_ok{}, trace_jitter{}, trace_foot{};
    if (contact || trace_all) {
        const auto f = [&](float v) { return ctx.ConstF32(v); };
        trace_foot = ctx.OpFMax(f1, ctx.OpLength(f1, ctx.OpDPdx(f3, s.p)), ctx.OpLength(f1, ctx.OpDPdy(f3, s.p)));
        const Id gx = EmitGetAttribute(ctx, IR::Attribute::FragCoord, 0, 0);
        const Id gy = EmitGetAttribute(ctx, IR::Attribute::FragCoord, 1, 0);
        // The steps' offset (light_shadow.h Jitter): none, the screen's (interleaved gradient noise of
        // the pixel's place), or the world's (a hash of the pixel's world cell, its size the power of
        // two at or above its footprint: the pattern stays on the floor as the head turns).
        const u32 mode = Scene::Jitter();
        if (mode == 0) {
            trace_jitter = f(0.5f);
        } else if (mode == 1) {
            trace_jitter = ctx.OpFract(
                f1, ctx.OpFMul(f1, ctx.OpFract(f1, ctx.OpFAdd(f1, ctx.OpFMul(f1, gx, f(0.06711056f)), ctx.OpFMul(f1, gy, f(0.00583715f)))),
                               f(52.9829189f)));
        } else {
            // (The pixel's true world place: the grid's transform holds the camera's world position;
            // where there is no grid, the world's axes about the camera. A fixed cell, about a mask
            // pixel within the trace's 12 m: no size that switches as the view turns. A code review.)
            Id world = wp;
            if (Sirit::ValidId(s.grid_rows[0])) {
                const Id flat = ctx.OpFAdd(f3, s.p, s.grid_shift);
                const Id f4 = ctx.OpCompositeConstruct(ctx.F32[4], std::array{ctx.OpCompositeExtract(f1, flat, 0u), ctx.OpCompositeExtract(f1, flat, 1u),
                                                                              ctx.OpCompositeExtract(f1, flat, 2u), f(1.0f)});
                world = ctx.OpCompositeConstruct(f3, std::array{ctx.OpDot(f1, s.grid_rows[0], f4), ctx.OpDot(f1, s.grid_rows[1], f4),
                                                                ctx.OpDot(f1, s.grid_rows[2], f4)});
            }
            const Id c = ctx.OpFloor(f3, ctx.OpVectorTimesScalar(f3, world, f(1.0f / Scene::JitterCellM)));
            Id p3 = ctx.OpFract(f3, ctx.OpVectorTimesScalar(f3, c, f(0.1031f)));
            const Id d = ctx.OpDot(f1, p3, ctx.OpFAdd(f3, ctx.OpVectorShuffle(f3, p3, p3, 2, 1, 0), ctx.ConstF32(31.32f, 31.32f, 31.32f)));
            p3 = ctx.OpFAdd(f3, p3, ctx.OpCompositeConstruct(f3, std::array{d, d, d}));
            trace_jitter = ctx.OpFract(f1, ctx.OpFMul(f1, ctx.OpFAdd(f1, ctx.OpCompositeExtract(f1, p3, 0u), ctx.OpCompositeExtract(f1, p3, 1u)),
                                                      ctx.OpCompositeExtract(f1, p3, 2u)));
        }
        const FragmentBlock fine = Block(ctx);
        // (Every lamp traced: each slot's own reach from the eye, below; the contact's ContactFarM.)
        trace_ok = ctx.OpLogicalAnd(b1,
                                    trace_all && Scene::TraceCoarse() ? ctx.true_value
                                                                      : ctx.OpIEqual(b1, ctx.OpIMul(u1, fine.bw, fine.bh), u(1u)),
                                    trace_all ? ctx.true_value
                                              : ctx.OpFOrdLessThan(b1, ctx.OpLength(f1, s.p), ctx.ConstF32(Scene::ContactFarM)));
    }
    const Id p_dist = trace_all ? ctx.OpLength(f1, s.p) : Id{};
    // A point `q` (this eye's view) against the eye's picture there: its distance behind the surface
    // the eye sees (`gap`; < 0 in front of it), and whether the picture holds it at all.
    struct Probe {
        Id gap, on_picture, q_dist;
    };
    const auto probe_at = [&](Id q) {
        const auto f = [&](float v) { return ctx.ConstF32(v); };
        const auto clip = Transform(ctx, s.inv, ctx.OpCompositeExtract(f1, q, 0u), ctx.OpCompositeExtract(f1, q, 1u),
                                    ctx.OpCompositeExtract(f1, q, 2u));
        const Id cw = ctx.OpFMax(f1, clip[3], f(1e-6f));
        const Id ndc = ctx.OpCompositeConstruct(ctx.F32[2], std::array{ctx.OpFDiv(f1, clip[0], cw), ctx.OpFDiv(f1, clip[1], cw)});
        const Id uvp = ctx.OpFAdd(ctx.F32[2], ctx.OpFMul(ctx.F32[2], s.a, ndc), s.b);
        const Id ux = ctx.OpCompositeExtract(f1, uvp, 0u), uy = ctx.OpCompositeExtract(f1, uvp, 1u);
        const Id on_picture = ctx.OpLogicalAnd(
            b1, ctx.OpFOrdGreaterThan(b1, clip[3], f(1e-4f)),
            ctx.OpLogicalAnd(b1, ctx.OpLogicalAnd(b1, ctx.OpFOrdGreaterThan(b1, ux, f(0.0f)), ctx.OpFOrdLessThan(b1, ux, f(1.0f))),
                             ctx.OpLogicalAnd(b1, ctx.OpFOrdGreaterThan(b1, uy, f(0.0f)), ctx.OpFOrdLessThan(b1, uy, f(1.0f)))));
        const Id uvc = ctx.OpFClamp(ctx.F32[2], uvp, ctx.ConstF32(0.0f, 0.0f), ctx.ConstF32(1.0f, 1.0f));
        const Id sampled = ctx.OpCompositeExtract(
            f1, EmitImageSampleExplicitLod(ctx, nullptr, ctx.info.vr_light_shadow.depth_image, uvc, f(0.0f), IR::Value{}), 0u);
        const auto e = Transform(ctx, s.rows, ctx.OpCompositeExtract(f1, ndc, 0u), ctx.OpCompositeExtract(f1, ndc, 1u), sampled);
        const Id ew = ctx.OpFDiv(f1, f(1.0f), e[3]);
        const Id seen = ctx.OpLength(f1, ctx.OpCompositeConstruct(f3, std::array{ctx.OpFMul(f1, e[0], ew), ctx.OpFMul(f1, e[1], ew),
                                                                                 ctx.OpFMul(f1, e[2], ew)}));
        const Id q_dist = ctx.OpLength(f1, q);
        return Probe{ctx.OpFSub(f1, q_dist, seen), on_picture, q_dist};
    };
    // The share of the light from `l_eye` (this eye's view) reaching this pixel past the screen's
    // surfaces, towards it to `clear` short of it. The smart trace (light_shadow.h Smart): steps that
    // grow out to SmartM, a crossing refined, a confirmed one the light all gone. Before (and the
    // contact): TraceSteps even steps over TraceM, a surface just in front of the ray half the light.
    const auto trace = [&](Id l_eye, Id clear) {
        const auto f = [&](float v) { return ctx.ConstF32(v); };
        const Id to = ctx.OpFSub(f3, l_eye, s.p);
        const Id dist = ctx.OpLength(f1, to);
        const Id dir = ctx.OpVectorTimesScalar(f3, to, ctx.OpFDiv(f1, f(1.0f), ctx.OpFMax(f1, dist, f(1e-4f))));
        const Id foot = trace_foot;
        const Id t0 = ctx.OpFMax(f1, f(0.02f), ctx.OpFMul(f1, foot, f(2.0f)));
        const Id lit = Private(ctx, f1);
        ctx.OpStore(lit, f(1.0f));
        const auto probe = [&](Id t) { return probe_at(ctx.OpFAdd(f3, s.p, ctx.OpVectorTimesScalar(f3, dir, t))); };
        // Behind the surface by more than the pixel's own footprint (its own surface never counts).
        const Id behind_by = ctx.OpFAdd(f1, foot, f(0.01f));
        if (trace_all && Scene::Smart()) {
            const Id t1 = ctx.OpFMin(f1, f(Scene::SmartM()), ctx.OpFSub(f1, dist, clear));
            const Id steps = ctx.OpSelect(u1, ctx.OpFOrdGreaterThan(b1, t1, t0), u(Scene::TraceSteps()), ctx.u32_zero_value);
            const Id grow = ctx.OpLog2(f1, ctx.OpFDiv(f1, ctx.OpFMax(f1, t1, t0), t0));
            // The last step on the picture: where along the ray, and its gap (the ray starts in front).
            const Id prev_t = Private(ctx, f1), prev_gap = Private(ctx, f1);
            ctx.OpStore(prev_t, f(0.0f));
            ctx.OpStore(prev_gap, f(-1.0f));
            const auto step = [&](Id i) {
                const Id share = ctx.OpFMul(f1, ctx.OpFAdd(f1, ctx.OpConvertUToF(f1, i), trace_jitter), f(1.0f / float(Scene::TraceSteps())));
                const Id t = ctx.OpFMul(f1, t0, ctx.OpExp2(f1, ctx.OpFMul(f1, grow, share)));
                const Probe here = probe(t);
                const Id behind = ctx.OpLogicalAnd(b1, here.on_picture, ctx.OpFOrdGreaterThan(b1, here.gap, behind_by));
                // (A crossing: in front at the last step on the picture, behind now; not once blocked.)
                const Id cross = ctx.OpLogicalAnd(
                    b1, behind,
                    ctx.OpLogicalAnd(b1, ctx.OpFOrdLessThanEqual(b1, ctx.OpLoad(f1, prev_gap), behind_by),
                                     ctx.OpFOrdGreaterThan(b1, ctx.OpLoad(f1, lit), f(0.0f))));
                const Id ref_l = ctx.OpLabel(), ref_done = ctx.OpLabel();
                ctx.OpSelectionMerge(ref_done, spv::SelectionControlMask::MaskNone);
                ctx.OpBranchConditional(cross, ref_l, ref_done);
                ctx.AddLabel(ref_l);
                {
                    // Halve between the two steps to where it goes behind: a real blocker is entered
                    // there (a small gap); something far in front of the ray keeps a large one.
                    Id lo = ctx.OpLoad(f1, prev_t), hi = t, gap_hi = here.gap;
                    for (u32 r = 0; r < Scene::SmartRefine; ++r) {
                        const Id mid = ctx.OpFMul(f1, ctx.OpFAdd(f1, lo, hi), f(0.5f));
                        const Probe m = probe(mid);
                        const Id in = ctx.OpLogicalAnd(b1, m.on_picture, ctx.OpFOrdGreaterThan(b1, m.gap, behind_by));
                        hi = ctx.OpSelect(f1, in, mid, hi);
                        gap_hi = ctx.OpSelect(f1, in, m.gap, gap_hi);
                        lo = ctx.OpSelect(f1, in, lo, mid);
                    }
                    const Id thick = ctx.OpFAdd(f1, ctx.OpFAdd(f1, f(Scene::TraceThickM), ctx.OpFMul(f1, p_dist, f(Scene::ContactThickPerM))),
                                                ctx.OpFMul(f1, ctx.OpFSub(f1, hi, lo), f(2.0f)));
                    ctx.OpStore(lit, ctx.OpSelect(f1, ctx.OpFOrdLessThan(b1, gap_hi, thick), f(0.0f), ctx.OpLoad(f1, lit)));
                }
                ctx.OpBranch(ref_done);
                ctx.AddLabel(ref_done);
                ctx.OpStore(prev_gap, ctx.OpSelect(f1, here.on_picture, here.gap, ctx.OpLoad(f1, prev_gap)));
                ctx.OpStore(prev_t, ctx.OpSelect(f1, here.on_picture, t, ctx.OpLoad(f1, prev_t)));
            };
            if (Scene::EarlyOut()) {
                LoopCount(ctx, steps, step, [&] { return ctx.OpFOrdGreaterThan(b1, ctx.OpLoad(f1, lit), f(0.0f)); });
            } else {
                LoopCount(ctx, steps, step);
            }
            return ctx.OpLoad(f1, lit);
        }
        const Id t1 = ctx.OpFMin(f1, f(Scene::TraceM()), ctx.OpFSub(f1, dist, clear));
        const Id steps = ctx.OpSelect(u1, ctx.OpFOrdGreaterThan(b1, t1, t0), u(Scene::TraceSteps()), ctx.u32_zero_value);
        const auto step = [&](Id i) {
            const Id even = ctx.OpFMul(f1, ctx.OpFAdd(f1, ctx.OpConvertUToF(f1, i), trace_jitter), f(1.0f / float(Scene::TraceSteps())));
            const Id share = Scene::TraceM() > 0.5f ? ctx.OpFMul(f1, even, even) : even;
            const Id t = ctx.OpFAdd(f1, t0, ctx.OpFMul(f1, ctx.OpFSub(f1, t1, t0), share));
            const Probe here = probe(t);
            const Id thick = ctx.OpFAdd(f1, ctx.OpFAdd(f1, f(trace_all ? Scene::TraceThickM : Scene::ContactThickM),
                                                       ctx.OpFMul(f1, here.q_dist, f(Scene::ContactThickPerM))),
                                        ctx.OpFMul(f1, t, f(Scene::TraceThickPerM)));
            const Id hit = ctx.OpLogicalAnd(
                b1, here.on_picture,
                ctx.OpLogicalAnd(b1, ctx.OpFOrdGreaterThan(b1, here.gap, behind_by), ctx.OpFOrdLessThan(b1, here.gap, thick)));
            ctx.OpStore(lit, ctx.OpSelect(f1, hit, ctx.OpFMax(f1, ctx.OpFSub(f1, ctx.OpLoad(f1, lit), f(0.5f)), f(0.0f)),
                                          ctx.OpLoad(f1, lit)));
        };
        if (Scene::EarlyOut()) {
            // (Tests: the ray ends once fully blocked.)
            LoopCount(ctx, steps, step, [&] { return ctx.OpFOrdGreaterThan(b1, ctx.OpLoad(f1, lit), f(0.0f)); });
        } else {
            LoopCount(ctx, steps, step);
        }
        return ctx.OpLoad(f1, lit);
    };
    // The old march (light_shadow.h MarchMode): the other eye's picture (its matrices, once), this
    // pixel's picture coordinates (the step rule's ray length on the picture), and this eye's view to
    // the other's (each eye half the eyes' gap off the flat view: twice this eye's shift).
    const bool march = trace_all && Scene::MarchMode();
    Id march_here{}, march_delta{};
    if (march) {
        SetupOtherEye(ctx);
        const auto pc = Transform(ctx, s.inv, ctx.OpCompositeExtract(f1, s.p, 0u), ctx.OpCompositeExtract(f1, s.p, 1u),
                                  ctx.OpCompositeExtract(f1, s.p, 2u));
        const Id pw = ctx.OpFMax(f1, pc[3], ctx.ConstF32(1e-6f));
        march_here = ctx.OpFAdd(ctx.F32[2],
                                ctx.OpFMul(ctx.F32[2], s.a,
                                           ctx.OpCompositeConstruct(ctx.F32[2], std::array{ctx.OpFDiv(f1, pc[0], pw), ctx.OpFDiv(f1, pc[1], pw)})),
                                s.b);
        if (s.other_ready) {
            march_delta = ctx.OpVectorTimesScalar(f3, s.scene_shift, ctx.ConstF32(2.0f));
        }
    }
    // (light_shadow.h FoveaSteps: the steps' scale by this pixel's foveation block, once.)
    Id march_fovea{};
    if (march && Scene::FoveaSteps()) {
        const FragmentBlock blk = Block(ctx);
        const Id area = ctx.OpIMul(ctx.U32[1], blk.bw, blk.bh);
        const auto pick = [&](u32 most, Id below, float scale) {
            return ctx.OpSelect(f1, ctx.OpULessThanEqual(ctx.U1[1], area, ctx.ConstU32(most)), ctx.ConstF32(scale), below);
        };
        Id scale = ctx.ConstF32(Scene::FoveaStepScale[4]);
        scale = pick(8u, scale, Scene::FoveaStepScale[3]);
        scale = pick(4u, scale, Scene::FoveaStepScale[2]);
        scale = pick(2u, scale, Scene::FoveaStepScale[1]);
        march_fovea = pick(1u, scale, Scene::FoveaCenterScale());
    }
    // (light_shadow.h MarchNoise 1: the march's offsets from the world cell's hash, turned by the
    // frame; the pass's own seed back after the lamps, for what follows them.)
    const Id saved_seed = s.seed;
    if (march && Scene::MarchNoise() == 1 && Scene::Jitter() == 2 && Sirit::ValidId(trace_jitter) && Sirit::ValidId(s.frame_turn)) {
        s.seed = ctx.OpFract(f1, ctx.OpFAdd(f1, trace_jitter, s.frame_turn));
    }
    // (MarchNoise 2: interleaved. The pixel's quarter by its place in its 2 x 2 group, the ordered
    // dither 0 2 / 3 1: (x ^ y) * 2 + y; the lamp shaders' noise within the quarter.)
    if (march && Scene::MarchNoise() == 2) {
        const Id px = ctx.OpBitwiseAnd(u1, ctx.OpConvertFToU(u1, EmitGetAttribute(ctx, IR::Attribute::FragCoord, 0, 0)), u(1u));
        const Id py = ctx.OpBitwiseAnd(u1, ctx.OpConvertFToU(u1, EmitGetAttribute(ctx, IR::Attribute::FragCoord, 1, 0)), u(1u));
        const Id quarter = ctx.OpIAdd(u1, ctx.OpShiftLeftLogical(u1, ctx.OpBitwiseXor(u1, px, py), u(1u)), py);
        s.seed = ctx.OpFMul(f1, ctx.OpFAdd(f1, ctx.OpConvertUToF(f1, quarter), s.seed), ctx.ConstF32(0.25f));
    }
    Id top_k{}, top_w{}, top_l{}, top_f{}, top_c{};
    if (contact) {
        top_k = Private(ctx, u1);
        top_w = Private(ctx, f1);
        top_l = Private(ctx, f3);
        top_f = Private(ctx, f1);
        top_c = Private(ctx, f1);
        ctx.OpStore(top_k, u(~0u));
        ctx.OpStore(top_w, ctx.ConstF32(Scene::ContactMinWeight));
        ctx.OpStore(top_l, ctx.ConstF32(0.0f, 0.0f, 0.0f));
        ctx.OpStore(top_f, ctx.ConstF32(0.0f));
        ctx.OpStore(top_c, ctx.ConstF32(0.0f));
    }
    LoopCount(ctx, ctx.OpSelect(u1, s.ok, s.scene_count, ctx.u32_zero_value), [&](Id k) {
        const SlotRead slot = SceneSlot(ctx, k);
        const Id d = ctx.OpFSub(f3, wp, slot.lamp);
        const Id d2 = ctx.OpDot(f1, d, d);
        const Id clear = slot.clear;
        // (A fresh slot, a lamp just taken: no cube of its own yet, but traced, this frame's depth.)
        const Id fresh = SlotHas(ctx, slot.flags, Scene::SlotFresh);
        const Id go = ctx.OpLogicalAnd(
            b1, ctx.OpLogicalAnd(b1, SlotHas(ctx, slot.flags, Scene::SlotUsed),
                                 trace_all ? ctx.true_value : ctx.OpLogicalNot(b1, fresh)),
            ctx.OpLogicalAnd(b1, ctx.OpFOrdLessThan(b1, ctx.OpFMul(f1, d2, slot.reach_k), ctx.ConstF32(1.0f)),
                             ctx.OpFOrdGreaterThan(b1, d2, ctx.OpFMul(f1, clear, clear))));
        const Id go_l = ctx.OpLabel(), go_done = ctx.OpLabel();
        ctx.OpSelectionMerge(go_done, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(go, go_l, go_done);
        ctx.AddLabel(go_l);
        {
            // Its share at this pixel first (strength, edge-on and rim fades; light_shadow.h GrazeIn,
            // DimBand). Under 1/32 its 4 bits round to lit whatever the shadow (1 - share > 29/30):
            // with VR_MARCH_OPT 1 nothing more is done for it (the words hold 15 already).
            const Id to_lamp = ctx.OpFNegate(f3, ctx.OpVectorTimesScalar(f3, d, ctx.OpInverseSqrt(f1, ctx.OpFMax(f1, d2, ctx.ConstF32(1e-8f)))));
            const Id graze = ctx.OpSelect(
                f1, s.normal_ok,
                SmoothStep(ctx, Scene::Graze().first, Scene::Graze().second, ctx.OpDot(f1, nw, to_lamp)), ctx.ConstF32(1.0f));
            const Id rim = ctx.OpFClamp(f1, ctx.OpFMul(f1, ctx.OpFSub(f1, ctx.ConstF32(1.0f), ctx.OpFMul(f1, d2, slot.reach_k)),
                                                       ctx.ConstF32(1.0f / Tune::DimBand)),
                                        ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
            const Id weight_k = ctx.OpFMul(f1, ctx.OpFClamp(f1, slot.strength, ctx.ConstF32(0.0f), ctx.ConstF32(1.0f)),
                                           ctx.OpFMul(f1, graze, rim));
            // The lamp in this eye's view, once (his body, the march, the trace and the contact read it).
            const Id lamp_eye = ctx.OpFSub(f3, ToFlat(ctx, slot.lamp), s.scene_shift);
            const bool skip_weak = Scene::Opt(1) && trace_all && march && !contact && !Scene::MaskShow();
            Id weak_done{};
            if (skip_weak) {
                // (The condition before the merge: SPIR-V wants the merge right before the branch.)
                const Id heavy = ctx.OpFOrdGreaterThanEqual(b1, weight_k, ctx.ConstF32(1.0f / 32.0f));
                const Id heavy_l = ctx.OpLabel();
                weak_done = ctx.OpLabel();
                ctx.OpSelectionMerge(weak_done, spv::SelectionControlMask::MaskNone);
                ctx.OpBranchConditional(heavy, heavy_l, weak_done);
                ctx.AddLabel(heavy_l);
            }
            // The cube's shadow (with its nearest blocker's distance from the lamp, for Confirm).
            // (The old march: no cubes.)
            const bool confirm = trace_all && Scene::Smart() && Scene::Confirm() && !march;
            const Id d_prev = ctx.OpFSub(f3, wp, slot.prev);
            Id cube_near{};
            Id vis = march ? ctx.ConstF32(1.0f)
                           : ctx.OpSelect(f1, fresh, ctx.ConstF32(1.0f),
                                          ctx.OpFSub(f1, ctx.ConstF32(1.0f), SceneOcclusion(ctx, k, d_prev, prev_base, nw, confirm ? &cube_near : nullptr)));
            // His body and the casters out of view (never checked against the view: they are not on it).
            Id others = ctx.ConstF32(1.0f);
            if (Body::Enabled()) {
                // His body (his three lamps: BodyVisibility's own gate), the lamp in this eye's view.
                // (VR_MARCH_OPT 16: only a slot the CPU flagged his; the gate's own test then skipped.)
                if (Scene::Opt(16)) {
                    const Id body_vis = Private(ctx, f1);
                    ctx.OpStore(body_vis, ctx.ConstF32(1.0f));
                    const Id his = SlotHas(ctx, slot.flags, Scene::SlotHis);
                    const Id his_l = ctx.OpLabel(), his_done = ctx.OpLabel();
                    ctx.OpSelectionMerge(his_done, spv::SelectionControlMask::MaskNone);
                    ctx.OpBranchConditional(his, his_l, his_done);
                    ctx.AddLabel(his_l);
                    ctx.OpStore(body_vis, BodyVisibility(ctx, s.scene_body, body_maps, s.p, lamp_eye, true));
                    ctx.OpBranch(his_done);
                    ctx.AddLabel(his_done);
                    others = ctx.OpFMin(f1, others, ctx.OpLoad(f1, body_vis));
                } else {
                    others = ctx.OpFMin(f1, others, BodyVisibility(ctx, s.scene_body, body_maps, s.p, lamp_eye));
                }
            }
            if (casters != ~0u) {
                others = ctx.OpFMin(f1, others, CasterVisibility(ctx, casters, wp, slot.lamp, slot.clear));
            }
            if (trace_all) {
                // (Every slot lamp traced where the trace runs, within its reach from the eye: the
                // darker of it and the cube, the cube's checked against the view first.)
                const Id traced = Private(ctx, f1);
                ctx.OpStore(traced, ctx.ConstF32(1.0f));
                const Id cube = Private(ctx, f1);
                ctx.OpStore(cube, vis);
                Id tr_go = ctx.OpLogicalAnd(b1, trace_ok, ctx.OpFOrdLessThan(b1, p_dist, SlotTraceFar(ctx, k)));
                // The old march's own terms (LightVisibilityAt's): the lamp in this eye's view, the
                // march's end short of its fixture (MaxM at most), its steps, room to march, and a
                // surface that faces it (within FacingCull) or whose normal cannot be trusted.
                Id m_l{}, m_dist{}, m_end{}, m_steps{};
                if (march) {
                    m_l = lamp_eye;
                    const Id dl = ctx.OpFSub(f3, m_l, s.p);
                    const Id d2l = ctx.OpDot(f1, dl, dl);
                    m_dist = ctx.OpSqrt(f1, d2l);
                    m_end = ctx.OpFMin(f1, ctx.OpFSub(f1, m_dist, slot.clear), ctx.ConstF32(Tune::MaxM));
                    // (A close lamp, the held torch, and a near pixel: more steps, light_shadow.h
                    // SceneMaps::CloseStepShare, NearStepShare, the larger share at most; then the
                    // GPU's dial, StepDial.)
                    Id boost{};
                    const float close_ratio = Scene::CloseStepShare() / Scene::MarchStepShare();
                    const float near_ratio = Scene::NearStepShare() / Scene::MarchStepShare();
                    const auto ramp = [&](Id dist, float in_m, float out_m, float ratio) {
                        const Id t = ctx.OpFClamp(
                            f1, ctx.OpFMul(f1, ctx.OpFSub(f1, ctx.ConstF32(out_m), dist), ctx.ConstF32(1.0f / (out_m - in_m))),
                            ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
                        return ctx.OpFma(f1, t, ctx.ConstF32(ratio - 1.0f), ctx.ConstF32(1.0f));
                    };
                    if (close_ratio > 1.0f) {
                        boost = ramp(ctx.OpLength(f1, m_l), Scene::CloseInM, Scene::CloseOutM, close_ratio);
                    }
                    if (near_ratio > 1.0f) {
                        const Id near = ramp(ctx.OpLength(f1, s.p), Scene::NearStepInM, Scene::NearStepOutM, near_ratio);
                        boost = Sirit::ValidId(boost)
                                    ? ctx.OpFMin(f1, ctx.OpFMul(f1, boost, near), ctx.ConstF32(std::max(close_ratio, near_ratio)))
                                    : near;
                    }
                    // (A lamp that is not one of the frame's edge lamps: PlainStepShare's steps,
                    // light_shadow.h; its close and near boosts with it.)
                    if (Sirit::ValidId(s.edge_set)) {
                        const Id bit = ctx.OpShiftLeftLogical(u1, u(1u), ctx.OpUMin(u1, k, u(Scene::Slots - 1)));
                        const Id edge_lamp = ctx.OpINotEqual(b1, ctx.OpBitwiseAnd(u1, s.edge_set, bit), u(0u));
                        const Id plain = ctx.OpSelect(f1, edge_lamp, ctx.ConstF32(1.0f),
                                                      ctx.ConstF32(Scene::PlainStepShare() / Scene::MarchStepShare()));
                        boost = Sirit::ValidId(boost) ? ctx.OpFMul(f1, boost, plain) : plain;
                    }
                    if (Scene::StepDial()) {
                        const Id dial = SlotStepScale(ctx, k);
                        boost = Sirit::ValidId(boost) ? ctx.OpFMul(f1, boost, dial) : dial;
                    }
                    if (Sirit::ValidId(march_fovea)) {
                        boost = Sirit::ValidId(boost) ? ctx.OpFMul(f1, boost, march_fovea) : march_fovea;
                    }
                    m_steps = MarchSteps(ctx, m_l, ctx.OpFSub(f1, ctx.ConstF32(1.0f), ctx.OpFMul(f1, d2l, slot.reach_k)), march_here,
                                         Scene::MarchStepShare(), s.pyr_scale, boost);
                    const Id facing = ctx.OpLogicalOr(
                        b1, ctx.OpLogicalNot(b1, s.normal_ok),
                        ctx.OpFOrdGreaterThan(b1,
                                              ctx.OpDot(f1, s.normal, ctx.OpVectorTimesScalar(f3, dl, ctx.OpFDiv(f1, ctx.ConstF32(1.0f), ctx.OpFMax(f1, m_dist, ctx.ConstF32(1e-4f))))),
                                              ctx.ConstF32(Tune::FacingCull)));
                    tr_go = ctx.OpLogicalAnd(b1, ctx.OpLogicalAnd(b1, trace_ok, facing),
                                             ctx.OpFOrdGreaterThan(b1, m_end, ctx.ConstF32(Tune::StartM)));
                }
                if (Scene::EarlyOut() && !confirm && !march) {
                    // (Tests: not where it is already dark below the mask's half step.)
                    tr_go = ctx.OpLogicalAnd(b1, tr_go, ctx.OpFOrdGreaterThan(b1, ctx.OpFMin(f1, vis, others), ctx.ConstF32(0.5f / 15.0f)));
                }
                const Id tr_l = ctx.OpLabel(), tr_done = ctx.OpLabel();
                ctx.OpSelectionMerge(tr_done, spv::SelectionControlMask::MaskNone);
                ctx.OpBranchConditional(tr_go, tr_l, tr_done);
                ctx.AddLabel(tr_l);
                ctx.OpStore(traced, march ? March(ctx, m_l, m_dist, m_end, m_steps, march_delta, Scene::MarchSmart())
                                          : trace(lamp_eye, slot.clear));
                if (confirm) {
                    // Where the cube's blocker is (the lamp's place of its cube, towards this pixel, at
                    // that distance), in this eye's view: a point the eye sees past is empty air.
                    const Id toward = ctx.OpVectorTimesScalar(f3, d_prev, ctx.OpInverseSqrt(f1, ctx.OpFMax(f1, ctx.OpDot(f1, d_prev, d_prev), ctx.ConstF32(1e-8f))));
                    const Id at = ctx.OpFAdd(f3, slot.prev, ctx.OpVectorTimesScalar(f3, toward, cube_near));
                    const Probe there = probe_at(ctx.OpFSub(f3, ToFlat(ctx, at), s.scene_shift));
                    const Id room = ctx.OpFAdd(f1, ctx.OpFAdd(f1, ctx.ConstF32(Scene::ConfirmM),
                                                              ctx.OpFMul(f1, cube_near, ctx.ConstF32(2.0f * 2.0f / float(Scene::Side)))),
                                               ctx.OpFMul(f1, trace_foot, ctx.ConstF32(2.0f)));
                    const Id ghost = ctx.OpLogicalAnd(
                        b1, ctx.OpFOrdLessThan(b1, cube_near, ctx.ConstF32(1e29f)),
                        ctx.OpLogicalAnd(b1, there.on_picture, ctx.OpFOrdLessThan(b1, there.gap, ctx.OpFNegate(f1, room))));
                    ctx.OpStore(cube, ctx.OpSelect(f1, ghost, ctx.ConstF32(1.0f), vis));
                }
                ctx.OpBranch(tr_done);
                ctx.AddLabel(tr_done);
                vis = ctx.OpFMin(f1, ctx.OpLoad(f1, cube), ctx.OpLoad(f1, traced));
            }
            vis = ctx.OpFMin(f1, vis, others);
            // Edge-on light and the light's rim: the shadow fades out (to_lamp, graze, rim above).
            if (contact) {
                const Id share = ctx.OpFMul(f1, ctx.OpFClamp(f1, slot.strength, ctx.ConstF32(0.0f), ctx.ConstF32(1.0f)),
                                            ctx.OpFMul(f1, graze, rim));
                const Id fall = ctx.OpFSub(f1, ctx.ConstF32(1.0f), ctx.OpFMul(f1, d2, slot.reach_k));
                const Id facing = ctx.OpSelect(f1, s.normal_ok,
                                               ctx.OpFClamp(f1, ctx.OpDot(f1, nw, to_lamp), ctx.ConstF32(0.0f), ctx.ConstF32(1.0f)),
                                               ctx.ConstF32(1.0f));
                const Id weight = ctx.OpFMul(f1, share, ctx.OpFMul(f1, ctx.OpFMul(f1, fall, fall), facing));
                const Id take = ctx.OpFOrdGreaterThan(b1, weight, ctx.OpLoad(f1, top_w));
                ctx.OpStore(top_w, ctx.OpSelect(f1, take, weight, ctx.OpLoad(f1, top_w)));
                ctx.OpStore(top_k, ctx.OpSelect(u1, take, k, ctx.OpLoad(u1, top_k)));
                ctx.OpStore(top_l, ctx.OpSelect(f3, take, lamp_eye, ctx.OpLoad(f3, top_l)));
                ctx.OpStore(top_f, ctx.OpSelect(f1, take, share, ctx.OpLoad(f1, top_f)));
                ctx.OpStore(top_c, ctx.OpSelect(f1, take, slot.clear, ctx.OpLoad(f1, top_c)));
            }
            vis = ctx.OpFAdd(f1, ctx.ConstF32(1.0f), ctx.OpFMul(f1, ctx.OpFSub(f1, vis, ctx.ConstF32(1.0f)), weight_k));
            if (Scene::MaskShow()) {
                ctx.OpStore(s.mask_min, ctx.OpFMin(f1, ctx.OpLoad(f1, s.mask_min), vis));
            }
            const Id q = ctx.OpConvertFToU(
                u1, ctx.OpFAdd(f1, ctx.OpFMul(f1, ctx.OpFClamp(f1, vis, ctx.ConstF32(0.0f), ctx.ConstF32(1.0f)),
                                               ctx.ConstF32(15.0f)),
                               ctx.ConstF32(0.5f)));
            put_bits(k, q, false);
            if (skip_weak) {
                ctx.OpBranch(weak_done);
                ctx.AddLabel(weak_done);
            }
        }
        ctx.OpBranch(go_done);
        ctx.AddLabel(go_done);
    });
    s.seed = saved_seed;
    if (contact && !trace_all) {
        const Id top = ctx.OpLoad(u1, top_k);
        const Id has = ctx.OpLogicalAnd(b1, ctx.OpULessThan(b1, top, u(Scene::Slots)), trace_ok);
        const Id c_l = ctx.OpLabel(), c_done = ctx.OpLabel();
        ctx.OpSelectionMerge(c_done, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(has, c_l, c_done);
        ctx.AddLabel(c_l);
        {
            const auto f = [&](float v) { return ctx.ConstF32(v); };
            const Id lit_now = trace(ctx.OpLoad(f3, top_l), ctx.OpLoad(f1, top_c));
            const Id vis = ctx.OpFSub(f1, f(1.0f), ctx.OpFMul(f1, ctx.OpFSub(f1, f(1.0f), lit_now), ctx.OpLoad(f1, top_f)));
            if (Scene::MaskShow()) {
                ctx.OpStore(s.mask_min, ctx.OpFMin(f1, ctx.OpLoad(f1, s.mask_min), vis));
            }
            const Id q = ctx.OpConvertFToU(u1, ctx.OpFAdd(f1, ctx.OpFMul(f1, ctx.OpFClamp(f1, vis, f(0.0f), f(1.0f)), f(15.0f)), f(0.5f)));
            put_bits(top, q, true);
        }
        ctx.OpBranch(c_done);
        ctx.AddLabel(c_done);
    }
    // This pixel's place (the pass's size from the uv's step a pixel; both eyes), within the
    // mask's capacity (the CPU's, header SlotsAt + 3): else nothing is written.
    const auto size_of = [&](u32 c) {
        const Id step = ctx.OpFMax(f1, ctx.OpCompositeExtract(f1, s.pixel, c), ctx.ConstF32(1e-6f));
        return ctx.OpConvertFToU(u1, ctx.OpFClamp(f1, ctx.OpFAdd(f1, ctx.OpFDiv(f1, ctx.ConstF32(1.0f), step), ctx.ConstF32(0.5f)),
                                                  ctx.ConstF32(1.0f), ctx.ConstF32(8192.0f)));
    };
    const FragmentBlock blk = Block(ctx);
    const Id w = ctx.OpIMul(u1, size_of(0), blk.bw), h = ctx.OpIMul(u1, size_of(1), blk.bh);
    const Id fx = blk.x0, fy = blk.y0;
    Id eye = u(0u), eyes = u(1u);
    if (ctx.runtime_info.native_stereo && ctx.runtime_info.native_multiview) {
        eye = ctx.NativeEyeIndex();
        eyes = u(2u);
    }
    const Id fits_all = ctx.OpLogicalAnd(
        b1, s.ok, ctx.OpULessThanEqual(b1, ctx.OpIMul(u1, ctx.OpIMul(u1, w, h), eyes), s.scene_capacity));
    // Every pixel the fragment covers (at a coarse rate: its block).
    LoopCount(ctx, ctx.OpSelect(u1, fits_all, ctx.OpIMul(u1, blk.bw, blk.bh), ctx.u32_zero_value), [&](Id cell) {
        const Id px = ctx.OpIAdd(u1, blk.x0, ctx.OpUMod(u1, cell, blk.bw));
        const Id py = ctx.OpIAdd(u1, blk.y0, ctx.OpUDiv(u1, cell, blk.bw));
        const Id inside = ctx.OpLogicalAnd(b1, ctx.OpULessThan(b1, px, w), ctx.OpULessThan(b1, py, h));
        const Id in_l = ctx.OpLabel(), in_done = ctx.OpLabel();
        ctx.OpSelectionMerge(in_done, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(inside, in_l, in_done);
        ctx.AddLabel(in_l);
        {
            const Id pixel = ctx.OpIAdd(u1, ctx.OpIMul(u1, ctx.OpIAdd(u1, ctx.OpIMul(u1, eye, h), py), w), px);
            const Id first = ctx.OpIAdd(u1, u(Scene::MaskHeader), ctx.OpIMul(u1, pixel, u(Scene::MaskWords)));
            for (u32 j = 0; j < Scene::MaskWords; ++j) {
                ctx.OpStore(MaskPtr(ctx, ctx.OpIAdd(u1, first, u(j))), ctx.OpLoad(u1, ctx.OpAccessChain(word_ptr, words, u(j))));
            }
        }
        ctx.OpBranch(in_done);
        ctx.AddLabel(in_done);
    });
    // The first pixel: the pass's size, for the lamp shaders, whenever a mask exists (capacity > 0;
    // never into the stand-in): a pass grown past the capacity then reads as lit, not stale.
    {
        const Id corner = ctx.OpLogicalAnd(
            b1, ctx.OpLogicalAnd(b1, ctx.OpIEqual(b1, fx, ctx.u32_zero_value), ctx.OpIEqual(b1, fy, ctx.u32_zero_value)),
            ctx.OpLogicalAnd(b1, ctx.OpIEqual(b1, eye, ctx.u32_zero_value),
                             ctx.OpUGreaterThan(b1, s.scene_capacity, ctx.u32_zero_value)));
        const Id c_l = ctx.OpLabel(), c_done = ctx.OpLabel();
        ctx.OpSelectionMerge(c_done, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(corner, c_l, c_done);
        ctx.AddLabel(c_l);
        ctx.OpStore(MaskPtr(ctx, u(0u)), w);
        ctx.OpStore(MaskPtr(ctx, u(1u)), h);
        ctx.OpStore(MaskPtr(ctx, u(2u)), eyes);
        ctx.OpBranch(c_done);
        ctx.AddLabel(c_done);
    }
}

// The other casters (light_shadow.h SceneMaps::Caster*: enemies and NPCs out of view): how much of
// the light from `lamp` reaches `p` past their capsules (both the world's axes from the camera),
// from StartM off the surface to `clear` short of the lamp; a body's sphere first.
Id CasterVisibility(EmitContext& ctx, u32 buf, Id p, Id lamp, Id clear) {
    namespace Scene = Tune::SceneMaps;
    const Id f1 = ctx.F32[1], f3 = ctx.F32[3], u1 = ctx.U32[1], b1 = ctx.U1[1];
    const auto u = [&](u32 v) { return ctx.ConstU32(v); };
    const auto F = [&](float v) { return ctx.ConstF32(v); };
    const auto ru = [&](Id d) { return BodyReadAt(ctx, buf, d); };
    const auto rf = [&](Id d) { return ctx.OpBitcast(f1, ru(d)); };
    const auto at = [&](Id base, u32 c) { return ctx.OpIAdd(u1, base, u(c)); };
    const auto vec = [&](Id base, u32 c) {
        return ctx.OpCompositeConstruct(f3, std::array{rf(at(base, c)), rf(at(base, c + 1)), rf(at(base, c + 2))});
    };
    const Id seg = ctx.OpFSub(f3, lamp, p);
    const Id len = ctx.OpLength(f1, seg);
    const Id dir = ctx.OpVectorTimesScalar(f3, seg, ctx.OpFDiv(f1, F(1.0f), ctx.OpFMax(f1, len, F(1e-4f))));
    const Id t0 = F(Tune::StartM);
    const Id t1 = ctx.OpFMax(f1, ctx.OpFSub(f1, len, clear), t0);
    const Id p1 = ctx.OpFAdd(f3, p, ctx.OpVectorTimesScalar(f3, dir, t0));
    const Id d1 = ctx.OpVectorTimesScalar(f3, dir, ctx.OpFSub(f1, t1, t0));
    const Id vis = Private(ctx, f1);
    ctx.OpStore(vis, F(1.0f));
    const Id bodies = ctx.OpUMin(u1, ru(u(1u)), u(Scene::CasterBodies));
    LoopCount(ctx, bodies, [&](Id k) {
        const Id body = ctx.OpIAdd(u1, u(Scene::CasterBodiesAt), ctx.OpIMul(u1, k, u(8u)));
        const Id c = vec(body, 0);
        const Id r = rf(at(body, 3));
        const Id t = ctx.OpFClamp(f1, ctx.OpDot(f1, ctx.OpFSub(f3, c, p), dir), t0, t1);
        const Id gap = ctx.OpLength(f1, ctx.OpFSub(f3, c, ctx.OpFAdd(f3, p, ctx.OpVectorTimesScalar(f3, dir, t))));
        const Id near_l = ctx.OpLabel(), near_done = ctx.OpLabel();
        const Id branch_cond_0 = ctx.OpFOrdLessThan(b1, gap, ctx.OpFMul(f1, r, F(Scene::CasterEdge)));
        ctx.OpSelectionMerge(near_done, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(branch_cond_0, near_l, near_done);
        ctx.AddLabel(near_l);
        {
            const Id first = ctx.OpUMin(u1, ru(at(body, 4)), u(Scene::CasterCapsules));
            const Id count = ctx.OpUMin(u1, ru(at(body, 5)), ctx.OpISub(u1, u(Scene::CasterCapsules), first));
            LoopCount(ctx, count, [&](Id j) {
                const Id cap = ctx.OpIAdd(u1, u(Scene::CasterCapsulesAt), ctx.OpIMul(u1, ctx.OpIAdd(u1, first, j), u(8u)));
                const Id a = vec(cap, 0);
                const Id rad = rf(at(cap, 3));
                const Id d2v = ctx.OpFSub(f3, vec(cap, 4), a);
                // The closest points of the ray's segment and the capsule's axis (Ericson 5.1.9).
                const Id rr = ctx.OpFSub(f3, p1, a);
                const Id aa = ctx.OpFMax(f1, ctx.OpDot(f1, d1, d1), F(1e-8f));
                const Id ee = ctx.OpFMax(f1, ctx.OpDot(f1, d2v, d2v), F(1e-8f));
                const Id ff = ctx.OpDot(f1, d2v, rr);
                const Id cc = ctx.OpDot(f1, d1, rr);
                const Id bb = ctx.OpDot(f1, d1, d2v);
                const Id denom = ctx.OpFSub(f1, ctx.OpFMul(f1, aa, ee), ctx.OpFMul(f1, bb, bb));
                Id s = ctx.OpSelect(f1, ctx.OpFOrdGreaterThan(b1, denom, F(1e-8f)),
                                    ctx.OpFClamp(f1, ctx.OpFDiv(f1, ctx.OpFSub(f1, ctx.OpFMul(f1, bb, ff), ctx.OpFMul(f1, cc, ee)),
                                                                ctx.OpFMax(f1, denom, F(1e-8f))),
                                                 F(0.0f), F(1.0f)),
                                    F(0.0f));
                Id tt = ctx.OpFDiv(f1, ctx.OpFAdd(f1, ctx.OpFMul(f1, bb, s), ff), ee);
                const Id lo = ctx.OpFOrdLessThan(b1, tt, F(0.0f)), hi = ctx.OpFOrdGreaterThan(b1, tt, F(1.0f));
                s = ctx.OpSelect(f1, lo, ctx.OpFClamp(f1, ctx.OpFDiv(f1, ctx.OpFNegate(f1, cc), aa), F(0.0f), F(1.0f)),
                                 ctx.OpSelect(f1, hi, ctx.OpFClamp(f1, ctx.OpFDiv(f1, ctx.OpFSub(f1, bb, cc), aa), F(0.0f), F(1.0f)), s));
                tt = ctx.OpFClamp(f1, tt, F(0.0f), F(1.0f));
                const Id dist = ctx.OpLength(f1, ctx.OpFSub(f3, ctx.OpFAdd(f3, p1, ctx.OpVectorTimesScalar(f3, d1, s)),
                                                            ctx.OpFAdd(f3, a, ctx.OpVectorTimesScalar(f3, d2v, tt))));
                const Id lit = SmoothStep(ctx, Scene::CasterCore, Scene::CasterEdge,
                                          ctx.OpFDiv(f1, dist, ctx.OpFMax(f1, rad, F(1e-3f))));
                ctx.OpStore(vis, ctx.OpFMin(f1, ctx.OpLoad(f1, vis), lit));
            });
        }
        ctx.OpBranch(near_done);
        ctx.AddLabel(near_done);
    });
    return ctx.OpLoad(f1, vis);
}

// The mask's edge at this lamp pixel, marched again at the lamp draw's own resolution (light_shadow.h
// SceneMaps::Edges): `blend` (the four neighbours' blend for slot `k`, 0..15; `got`: the slot + 1, 0
// none) moved to the neighbours' lowest (`lo`) where the pixel's own ray toward the light hits
// something on this eye's depth picture, toward their highest (`hi`) where it is clear all the way,
// kept elsewhere. All behind one branch: a pixel whose neighbours agree does none of it.
//
// The ray without the matrix's inverse: the view position is M (ndc, depth, 1) / w, and its x, y, z
// rows do not hold the depth (a pixel's ray keeps its direction whatever its depth), so with H their
// ndc x, ndc y and constant columns (3 x 3), h = H^-1 q of a view point q is (ndc x, ndc y, 1) times
// a scale h.z, and the surface the eye sees at that ndc lies at q / (h.z w), w the matrix's w row on
// (ndc, its depth, 1). h is linear along the ray: one H^-1 L by cross products for the light, then a
// step is a few multiply-adds, one divide for the picture's coordinates and one for the gap.
Id MaskEdge(EmitContext& ctx, Id light_base, Id k, Id got, Id blend, Id lo, Id hi) {
    namespace Scene = Tune::SceneMaps;
    auto& s = ctx.vr_light_shadow;
    const auto& info = ctx.info.vr_light_shadow;
    const Id f1 = ctx.F32[1], f2 = ctx.F32[2], f3 = ctx.F32[3], u1 = ctx.U32[1], b1 = ctx.U1[1];
    const auto f = [&](float v) { return ctx.ConstF32(v); };
    const auto u = [&](u32 v) { return ctx.ConstU32(v); };
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(f1, a, b); };
    const auto x = [&](Id v, u32 i) { return ctx.OpCompositeExtract(f1, v, i); };
    const auto [far_in, far_out] = Scene::EdgeFar();
    const Id nearness = ctx.OpFClamp(f1, mul(ctx.OpFSub(f1, f(far_out), s.edge_own), f(1.0f / (far_out - far_in))), f(0.0f), f(1.0f));
    const Id spread = ctx.OpFSub(f1, hi, lo);
    // k is zero-based and s.scene_count is clamped to the allocated slot table.
    const Id valid_slot = ctx.OpLogicalAnd(b1, ctx.OpINotEqual(b1, got, ctx.u32_zero_value),
                                           ctx.OpULessThan(b1, k, s.scene_count));
    Id go = ctx.OpLogicalAnd(
        b1, ctx.OpLogicalAnd(b1, valid_slot, s.edge_ok),
        ctx.OpLogicalAnd(b1, ctx.OpFOrdGreaterThanEqual(b1, spread, f(float(Scene::EdgeRange()) - 0.5f)),
                         ctx.OpFOrdGreaterThan(b1, nearness, f(0.0f))));
    // Tests (VR_SHADOW_EDGES_SHOW 3..7): black where one gate of the march holds: 3 the neighbours'
    // spread, 4 a slot and the picture's coordinates, 5 near enough, 6 the budget, 7 all of them.
    if (Scene::EdgeShow() >= 3 && Scene::EdgeShow() <= 7) {
        Id gate = go;
        switch (Scene::EdgeShow()) {
        case 3:
            gate = ctx.OpFOrdGreaterThanEqual(b1, spread, f(float(Scene::EdgeRange()) - 0.5f));
            break;
        case 4:
            gate = ctx.OpLogicalAnd(b1, ctx.OpINotEqual(b1, got, ctx.u32_zero_value), s.edge_ok);
            break;
        case 5:
            gate = ctx.OpFOrdGreaterThan(b1, nearness, f(0.0f));
            break;
        case 6:
            gate = ctx.true_value; // Slot and per-pixel budget already passed at the call site.
            break;
        default:
            break;
        }
        return ctx.OpSelect(f1, gate, f(0.0f), blend);
    }
    const Id out = Private(ctx, f1);
    ctx.OpStore(out, blend);
    const Id go_l = ctx.OpLabel(), go_done = ctx.OpLabel();
    ctx.OpSelectionMerge(go_done, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(go, go_l, go_done);
    ctx.AddLabel(go_l);
    {
        const auto rd = [&](u32 c) { return ctx.OpBitcast(f1, EmitReadConstBuffer(ctx, info.light_buffer, ctx.OpIAdd(u1, light_base, u(c)))); };
        const Id pv = s.edge_pv;
        const Id l = ctx.OpCompositeConstruct(f3, std::array{rd(0), rd(1), rd(2)});
        const Id to = ctx.OpFSub(f3, l, pv);
        const Id d2 = ctx.OpDot(f1, to, to);
        const Id dist = ctx.OpSqrt(f1, d2);
        const Id inv_dist = ctx.OpFDiv(f1, f(1.0f), ctx.OpFMax(f1, dist, f(1e-4f)));
        // (The lamp's falloff here: 1 at the lamp, 0 at its reach.)
        const Id fall = ctx.OpFClamp(f1, ctx.OpFSub(f1, f(1.0f), mul(d2, rd(3))), f(0.0f), f(1.0f));
        const SlotRead slot = SceneSlot(ctx, k);
        const Id end = ctx.OpFMin(f1, ctx.OpFSub(f1, dist, slot.clear), f(Tune::MaxM));
        const Id span = ctx.OpFSub(f1, end, f(Tune::StartM));
        const Id worth = ctx.OpLogicalAnd(b1, ctx.OpFOrdGreaterThanEqual(b1, mul(fall, spread), f(Scene::EdgeMinContrast)),
                                          ctx.OpFOrdGreaterThan(b1, span, f(0.0f)));
        if (Scene::EdgeShow() == 8) {
            ctx.OpStore(out, ctx.OpSelect(f1, worth, f(0.0f), f(15.0f)));
        }
        const Id trace = ctx.OpLabel(), trace_done = ctx.OpLabel();
        ctx.OpSelectionMerge(trace_done, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(worth, trace, trace_done);
        ctx.AddLabel(trace);
        if (Sirit::ValidId(s.edge_seen)) {
            ctx.OpStore(s.edge_seen, ctx.OpBitwiseOr(u1, ctx.OpLoad(u1, s.edge_seen), ctx.OpShiftLeftLogical(u1, u(1), k)));
        }
        // H's columns and the light's h (Cramer's rule: the columns' cross products).
        const auto col = [&](u32 c) {
            return ctx.OpCompositeConstruct(f3, std::array{x(s.edge_rows[0], c), x(s.edge_rows[1], c), x(s.edge_rows[2], c)});
        };
        const Id c0 = col(0), c1 = col(1), c3 = col(3);
        const Id x13 = ctx.OpCross(f3, c1, c3), x30 = ctx.OpCross(f3, c3, c0), x01 = ctx.OpCross(f3, c0, c1);
        const Id inv_det = ctx.OpFDiv(f1, f(1.0f), ctx.OpDot(f1, c0, x13));
        const Id hl = ctx.OpVectorTimesScalar(
            f3, ctx.OpCompositeConstruct(f3, std::array{ctx.OpDot(f1, l, x13), ctx.OpDot(f1, l, x30), ctx.OpDot(f1, l, x01)}), inv_det);
        // The pixel's own: (ndc, 1) / w. The ray's h a metre: (hl - hp) / dist.
        const Id iw = ctx.OpFDiv(f1, f(1.0f), s.edge_w);
        const Id hp = ctx.OpCompositeConstruct(f3, std::array{mul(x(s.edge_ndc, 0), iw), mul(x(s.edge_ndc, 1), iw), iw});
        const Id dh = ctx.OpVectorTimesScalar(f3, ctx.OpFSub(f3, hl, hp), inv_dist);
        // The picture's coordinates times h.z (a h.xy + b h.z), the w row's part without the depth,
        // h.z, each at the pixel and a metre along; the w row's depth term.
        const auto coords = [&](Id h) {
            return ctx.OpFAdd(f2, ctx.OpFMul(f2, s.edge_a, ctx.OpVectorShuffle(f2, h, h, 0, 1)), ctx.OpVectorTimesScalar(f2, s.edge_b, x(h, 2)));
        };
        const Id row3 = s.edge_rows[3];
        const Id w_row = ctx.OpCompositeConstruct(f3, std::array{x(row3, 0), x(row3, 1), x(row3, 3)});
        const Id m32 = x(row3, 2);
        const Id uv0 = coords(hp), duv = coords(dh);
        const Id k0 = ctx.OpDot(f1, w_row, hp), dk = ctx.OpDot(f1, w_row, dh);
        const Id hz0 = iw, dhz = x(dh, 2);
        const Id pp = ctx.OpDot(f1, pv, pv);
        const Id pd2 = mul(mul(f(2.0f), ctx.OpDot(f1, pv, to)), inv_dist);
        // In front of the eye: h.z keeps the pixel's own sign (h.z w is the pixel's depth over the
        // point's).
        const auto in_front = [&](Id hz) { return ctx.OpFOrdGreaterThan(b1, mul(hz, s.edge_w), f(1e-4f)); };
        // A fixed step count needs no projected endpoint or screen-space ray length.
        const u32 max_steps = Scene::EdgeSteps();
        Id ray_steps = u(max_steps);
        Id inv_count = f(1.0f / float(max_steps));
        if (max_steps > Scene::EdgeMinSteps) {
            const Id hz_end = ctx.OpFma(f1, dhz, end, hz0);
            const Id uv_end = ctx.OpFDiv(f2, ctx.OpFAdd(f2, uv0, ctx.OpVectorTimesScalar(f2, duv, end)),
                                         ctx.OpCompositeConstruct(f2, std::array{hz_end, hz_end}));
            const Id uv_here = ctx.OpVectorTimesScalar(f2, uv0, s.edge_w);
            const Id pixels = ctx.OpLength(f1, ctx.OpFDiv(f2, ctx.OpFSub(f2, uv_end, uv_here), ctx.OpFMax(f2, s.edge_pixel, ctx.ConstF32(1e-7f, 1e-7f))));
            const Id most = f(float(max_steps));
            const Id steps_f = ctx.OpSelect(
                f1, in_front(hz_end),
                ctx.OpFClamp(f1, mul(pixels, f(1.0f / Scene::EdgePixels())), f(float(Scene::EdgeMinSteps)), most), most);
            ray_steps = ctx.OpConvertFToU(u1, ctx.OpFAdd(f1, steps_f, f(0.5f)));
            inv_count = ctx.OpFDiv(f1, f(1.0f), ctx.OpConvertUToF(f1, ray_steps));
        }
        Id seed = f(0.5f);
        if (Scene::EdgeNoise() == 1) {
            const Id gx = EmitGetAttribute(ctx, IR::Attribute::FragCoord, 0, 0);
            const Id gy = EmitGetAttribute(ctx, IR::Attribute::FragCoord, 1, 0);
            seed = ctx.OpFract(f1, mul(ctx.OpFract(f1, ctx.OpFAdd(f1, mul(gx, f(0.06711056f)), mul(gy, f(0.00583715f)))), f(52.9829189f)));
        }
        // (The march's own bias and thickness, light_shadow.h BiasM, ThickM; a hit from the middle
        // of its soft rise.)
        const Id bias0 = mul(ctx.OpFAdd(f1, f(Tune::BiasM), mul(s.edge_own, f(Tune::BiasPerEyeM))), f(1.5f));
        const Id v_hit = Private(ctx, u1), v_off = Private(ctx, u1), v_unsure = Private(ctx, u1);
        ctx.OpStore(v_hit, ctx.u32_zero_value);
        ctx.OpStore(v_off, ctx.u32_zero_value);
        ctx.OpStore(v_unsure, ctx.u32_zero_value);
        LoopCount(
            ctx, ray_steps,
            [&](Id i) {
                const Id share = mul(ctx.OpFAdd(f1, ctx.OpConvertUToF(f1, i), seed), inv_count);
                const Id t = ctx.OpFma(f1, span, mul(share, share), f(Tune::StartM));
                const Id hz = ctx.OpFma(f1, dhz, t, hz0);
                const Id at = ctx.OpFDiv(f2, ctx.OpFAdd(f2, uv0, ctx.OpVectorTimesScalar(f2, duv, t)), ctx.OpCompositeConstruct(f2, std::array{hz, hz}));
                const Id inside = ctx.OpLogicalAnd(
                    b1, in_front(hz),
                    ctx.OpLogicalAnd(b1, ctx.OpAll(b1, ctx.OpFOrdGreaterThanEqual(ctx.U1[2], at, ctx.ConstF32(0.0f, 0.0f))),
                                     ctx.OpAll(b1, ctx.OpFOrdLessThanEqual(ctx.U1[2], at, ctx.ConstF32(1.0f, 1.0f)))));
                const Id raw = ctx.OpCompositeExtract(
                    f1,
                    EmitImageSampleExplicitLod(ctx, nullptr, info.depth_image, ctx.OpFClamp(f2, at, ctx.ConstF32(0.0f, 0.0f), ctx.ConstF32(1.0f, 1.0f)),
                                               f(0.0f), IR::Value{}),
                    0u);
                // The point's distance behind the surface seen there: its own from the eye times
                // (1 - 1 / (h.z w)).
                const Id scale = ctx.OpFma(f1, mul(m32, raw), hz, ctx.OpFma(f1, dk, t, k0));
                const Id q_len = ctx.OpSqrt(f1, ctx.OpFMax(f1, ctx.OpFma(f1, t, ctx.OpFAdd(f1, pd2, t), pp), f(0.0f)));
                const Id gap = mul(q_len, ctx.OpFSub(f1, f(1.0f), ctx.OpFDiv(f1, f(1.0f), scale)));
                const Id bias = ctx.OpFma(f1, t, f(Tune::BiasPerM * 1.5f), bias0);
                const Id thick = ctx.OpFma(f1, t, f(Tune::ThickPerM), f(Tune::ThickM));
                const Id behind = ctx.OpLogicalAnd(b1, inside, ctx.OpFOrdGreaterThan(b1, gap, bias));
                const Id thin = ctx.OpFOrdLessThan(b1, gap, thick);
                ctx.OpStore(v_hit, ctx.OpSelect(u1, ctx.OpLogicalAnd(b1, behind, thin), u(1u), ctx.u32_zero_value));
                ctx.OpStore(v_off, ctx.OpSelect(u1, inside, ctx.u32_zero_value, u(1u)));
                // (Off the picture, or behind something too thick to be the caster: the picture
                // cannot say the ray is clear.)
                const Id unsure = ctx.OpLogicalOr(b1, ctx.OpLogicalNot(b1, inside), ctx.OpLogicalAnd(b1, behind, ctx.OpLogicalNot(b1, thin)));
                ctx.OpStore(v_unsure, ctx.OpSelect(u1, unsure, u(1u), ctx.OpLoad(u1, v_unsure)));
            },
            [&] {
                return ctx.OpIEqual(b1, ctx.OpBitwiseOr(u1, ctx.OpLoad(u1, v_hit), ctx.OpLoad(u1, v_off)), ctx.u32_zero_value);
            });
        const Id hit = ctx.OpINotEqual(b1, ctx.OpLoad(u1, v_hit), ctx.u32_zero_value);
        // (His body is not on the picture where the head is left out: a lamp that casts his shadow
        // never takes a clear ray's word.)
        const Id clear = ctx.OpLogicalAnd(
            b1, ctx.OpLogicalNot(b1, hit),
            ctx.OpLogicalAnd(b1, ctx.OpIEqual(b1, ctx.OpLoad(u1, v_unsure), ctx.u32_zero_value),
                             ctx.OpLogicalNot(b1, SlotHas(ctx, slot.flags, Scene::SlotHis))));
        const Id lit = ctx.OpFma(f1, ctx.OpFSub(f1, hi, blend), f(Scene::EdgeClear()), blend);
        const Id sharp = ctx.OpSelect(f1, hit, lo, ctx.OpSelect(f1, clear, lit, blend));
        Id value = ctx.OpFma(f1, ctx.OpFSub(f1, sharp, blend), nearness, blend);
        if (Scene::EdgeShow() == 8) {
            value = f(0.0f);
        } else if (Scene::EdgeShow() == 1) {
            value = f(0.0f);
        } else if (Scene::EdgeShow() == 2) {
            value = ctx.OpSelect(f1, hit, f(0.0f), ctx.OpSelect(f1, clear, f(15.0f), blend));
        }
        ctx.OpStore(out, value);
        ctx.OpBranch(trace_done);
        ctx.AddLabel(trace_done);
    }
    ctx.OpBranch(go_done);
    ctx.AddLabel(go_done);
    return ctx.OpLoad(f1, out);
}

// Read one light from the mask. The multi-light edge shader calls this inside a single
// generated loop; ordinary mask shaders keep their constant buffer offsets.
Id MaskLight(EmitContext& ctx, Id index, Id light_base, u32 tint) {
    namespace Scene = Tune::SceneMaps;
    auto& s = ctx.vr_light_shadow;
    const auto& info = ctx.info.vr_light_shadow;
    const Id f1 = ctx.F32[1], f3 = ctx.F32[3], u1 = ctx.U32[1], b1 = ctx.U1[1];
    const auto u = [&](u32 v) { return ctx.ConstU32(v); };
    // Its slot: the CPU's match for this draw (light_shadow.h SceneMaps::DrawSlotsAt: a byte each).
    namespace Body = Shader::Optimization::BodyShadow;
    const Id byte = ctx.OpBitwiseAnd(
        u1, ctx.OpShiftRightLogical(u1, BodyReadAt(ctx, s.scene_body,
            ctx.OpIAdd(u1, u(Body::SlotsAt + Scene::DrawSlotsAt), ctx.OpShiftRightLogical(u1, index, u(2u)))),
            ctx.OpIMul(u1, ctx.OpBitwiseAnd(u1, index, u(3u)), u(8u))), u(255u));
    // (0: the CPU did not see this draw's list: the nearest slot within SlotKeepM of the light, here;
    // 255: it did, no slot.)
    const Id found = Private(ctx, u1);
    ctx.OpStore(found, ctx.OpSelect(u1, ctx.OpIEqual(b1, byte, u(255u)), ctx.u32_zero_value, byte));
    const Id gpu_l = ctx.OpLabel(), gpu_done = ctx.OpLabel();
    const Id branch_cond_1 = ctx.OpIEqual(b1, byte, ctx.u32_zero_value);
    ctx.OpSelectionMerge(gpu_done, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(branch_cond_1, gpu_l, gpu_done);
    ctx.AddLabel(gpu_l);
    {
        const auto rd = [&](u32 c) {
            return ctx.OpBitcast(f1, EmitReadConstBuffer(ctx, info.light_buffer, ctx.OpIAdd(u1, light_base, u(c))));
        };
        const Id wl = ToWorld(ctx, ctx.OpFAdd(f3, ctx.OpCompositeConstruct(f3, std::array{rd(0), rd(1), rd(2)}), s.scene_shift));
        const Id best = Private(ctx, f1);
        ctx.OpStore(best, ctx.ConstF32(Scene::SlotKeepM * Scene::SlotKeepM));
        LoopCount(ctx, s.scene_count, [&](Id k) {
            const SlotRead slot = SceneSlot(ctx, k);
            const Id dv = ctx.OpFSub(f3, wl, slot.lamp);
            const Id d2 = ctx.OpDot(f1, dv, dv);
            const Id take = ctx.OpLogicalAnd(b1, SlotHas(ctx, slot.flags, Scene::SlotUsed),
                                             ctx.OpFOrdLessThan(b1, d2, ctx.OpLoad(f1, best)));
            ctx.OpStore(best, ctx.OpSelect(f1, take, d2, ctx.OpLoad(f1, best)));
            ctx.OpStore(found, ctx.OpSelect(u1, take, ctx.OpIAdd(u1, k, u(1u)), ctx.OpLoad(u1, found)));
        });
    }
    ctx.OpBranch(gpu_done);
    ctx.AddLabel(gpu_done);
    const Id got = ctx.OpLoad(u1, found);
    // After either slot lookup, zero means fully lit. In multi-light programs the
    // whole draw can skip interpolation for that lamp, including its edge verdict.
    const bool skip_unassigned = info.count >= 4 && tint == 0 && Scene::EdgePass();
    Id mask_result{}, mask_done{};
    if (skip_unassigned) {
        mask_result = Private(ctx, f1);
        mask_done = ctx.OpLabel();
        const Id assigned = ctx.OpLabel();
        ctx.OpStore(mask_result, ctx.ConstF32(1.0f));
        const Id has_slot = ctx.OpINotEqual(b1, got, u(0));
        ctx.OpSelectionMerge(mask_done, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(has_slot, assigned, mask_done);
        ctx.AddLabel(assigned);
    }
    const Id k = ctx.OpISub(u1, ctx.OpUMax(u1, got, u(1u)), u(1u));
    const Id wi = ctx.OpShiftRightLogical(u1, k, u(3u));
    const Id shift = ctx.OpShiftLeftLogical(u1, ctx.OpBitwiseAnd(u1, k, u(7u)), u(2u));
    Id weighted = ctx.ConstF32(0.0f);
    // (The edges' march: the lowest and highest of the neighbours that count here.)
    const bool edges = Sirit::ValidId(s.edge_corner[0]);
    std::array<Id, 4> values;
    for (u32 c = 0; c < 4; ++c) {
        Id word = s.mask_corner[c][Scene::MaskSlotWords - 1];
        for (u32 j = Scene::MaskSlotWords - 1; j-- > 0;) {
            word = ctx.OpSelect(u1, ctx.OpIEqual(b1, wi, u(j)), s.mask_corner[c][j], word);
        }
        const Id v = ctx.OpConvertUToF(f1, ctx.OpBitwiseAnd(u1, ctx.OpShiftRightLogical(u1, word, shift), u(15u)));
        weighted = ctx.OpFAdd(f1, weighted, ctx.OpFMul(f1, v, s.mask_w[c]));
        values[c] = v;
    }
    if (edges && Sirit::ValidId(s.edge_packed)) {
        // The edge pass's verdict for this light's slot (light_shadow.h EdgePass): its rank among
        // the frame's edge lamps names its two bits; a hit takes the lowest neighbour, a clear ray
        // EdgeClear() of the way to the highest, by the pixel's nearness (EdgeFar, as the march).
        // A pixel without a verdict for it (nearly all) skips the rest behind a branch.
        const Id safe_k = ctx.OpUMin(u1, k, u(Scene::Slots - 1));
        const Id slot_bit = ctx.OpShiftLeftLogical(u1, u(1), safe_k);
        const Id is_edge = ctx.OpLogicalAnd(
            b1, ctx.OpLogicalAnd(b1, ctx.OpULessThanEqual(b1, got, s.scene_count), ctx.OpINotEqual(b1, got, u(0))),
            ctx.OpINotEqual(b1, ctx.OpBitwiseAnd(u1, s.edge_slots, slot_bit), u(0)));
        const Id rank = ctx.OpBitCount(u1, ctx.OpBitwiseAnd(u1, s.edge_slots, ctx.OpISub(u1, slot_bit, u(1))));
        const Id capacity = u(Scene::EdgePassLampCapacity(Scene::EdgeBits()));
        const Id code = ctx.OpSelect(
            u1, ctx.OpLogicalAnd(b1, is_edge, ctx.OpULessThan(b1, rank, capacity)),
            ctx.OpBitwiseAnd(u1, ctx.OpShiftRightLogical(u1, s.edge_packed, ctx.OpIAdd(u1, ctx.OpIMul(u1, rank, u(2u)), u(4u))), u(3u)),
            u(0u));
        const Id result = Private(ctx, f1);
        ctx.OpStore(result, weighted);
        const Id act = ctx.OpINotEqual(b1, code, u(0u));
        const Id refine = ctx.OpLabel(), done = ctx.OpLabel();
        ctx.OpSelectionMerge(done, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(act, refine, done);
        ctx.AddLabel(refine);
        {
            Id lowest = ctx.ConstF32(15.0f), highest = ctx.ConstF32(0.0f);
            for (u32 c = 0; c < values.size(); ++c) {
                const Id counts = ctx.OpINotEqual(b1, ctx.OpBitwiseAnd(u1, s.edge_packed, u(1u << c)), u(0u));
                lowest = ctx.OpFMin(f1, lowest, ctx.OpSelect(f1, counts, values[c], ctx.ConstF32(15.0f)));
                highest = ctx.OpFMax(f1, highest, ctx.OpSelect(f1, counts, values[c], ctx.ConstF32(0.0f)));
            }
            const Id lit = ctx.OpFma(f1, ctx.OpFSub(f1, highest, weighted), ctx.ConstF32(Scene::EdgeClear()), weighted);
            const Id sharp = ctx.OpSelect(f1, ctx.OpIEqual(b1, code, u(Scene::EdgeHit)), lowest,
                                          ctx.OpSelect(f1, ctx.OpIEqual(b1, code, u(Scene::EdgeClearCode)), lit, weighted));
            Id value = ctx.OpFma(f1, ctx.OpFSub(f1, sharp, weighted), s.edge_near, weighted);
            if (Scene::EdgeShow() == 2) {
                value = ctx.OpSelect(f1, ctx.OpIEqual(b1, code, u(Scene::EdgeHit)), ctx.ConstF32(0.0f),
                                     ctx.OpSelect(f1, ctx.OpIEqual(b1, code, u(Scene::EdgeClearCode)),
                                                  ctx.ConstF32(15.0f), weighted));
            }
            ctx.OpStore(result, value);
        }
        ctx.OpBranch(done);
        ctx.AddLabel(done);
        weighted = ctx.OpLoad(f1, result);
    } else if (edges) {
        // Guard the optional work with the frame's stable slot flag. Unknown slots keep the blend.
        const Id safe_k = ctx.OpUMin(u1, k, u(Scene::Slots - 1));
        const Id slot_bit = ctx.OpShiftLeftLogical(u1, u(1), safe_k);
        Id selected = ctx.OpLogicalAnd(b1, ctx.OpULessThanEqual(b1, got, s.scene_count),
            ctx.OpLogicalAnd(b1, ctx.OpINotEqual(b1, got, u(0)),
                ctx.OpINotEqual(b1, ctx.OpBitwiseAnd(u1, s.edge_slots, slot_bit), u(0))));
        if (Sirit::ValidId(s.edge_seen)) {
            const Id seen = ctx.OpLoad(u1, s.edge_seen);
            selected = ctx.OpLogicalAnd(b1, selected, ctx.OpLogicalAnd(b1,
                ctx.OpIEqual(b1, ctx.OpBitwiseAnd(u1, seen, slot_bit), u(0)),
                ctx.OpULessThan(b1, ctx.OpBitCount(u1, seen), u(Scene::EdgeLights))));
        }
        const Id result = Private(ctx, f1);
        ctx.OpStore(result, weighted);
        const Id refine = ctx.OpLabel(), done = ctx.OpLabel();
        ctx.OpSelectionMerge(done, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(selected, refine, done);
        ctx.AddLabel(refine);
        Id lowest = ctx.ConstF32(15.0f), highest = ctx.ConstF32(0.0f);
        for (u32 c = 0; c < values.size(); ++c) {
            lowest = ctx.OpFMin(f1, lowest, ctx.OpSelect(f1, s.edge_corner[c], values[c], ctx.ConstF32(15.0f)));
            highest = ctx.OpFMax(f1, highest, ctx.OpSelect(f1, s.edge_corner[c], values[c], ctx.ConstF32(0.0f)));
        }
        ctx.OpStore(result, MaskEdge(ctx, light_base, k, got, weighted, lowest, highest));
        ctx.OpBranch(done);
        ctx.AddLabel(done);
        weighted = ctx.OpLoad(f1, result);
    }
    const Id vis = ctx.OpFMul(f1, weighted, ctx.ConstF32(1.0f / 15.0f));
    if (skip_unassigned) {
        ctx.OpStore(mask_result, vis);
        ctx.OpBranch(mask_done);
        ctx.AddLabel(mask_done);
        return ctx.OpLoad(f1, mask_result);
    }
    const Id out = ctx.OpSelect(f1, ctx.OpINotEqual(b1, got, ctx.u32_zero_value), vis, ctx.ConstF32(1.0f));
    if (tint == 0) {
        return out;
    }
    // Tests (Scene::Tint): the blocked share of the light keeps its slot's colour in this channel.
    Id colour = ctx.ConstF32(1.0f);
    for (u32 j = 0; j < Scene::Slots && j < 16; ++j) {
        colour = ctx.OpSelect(f1, ctx.OpIEqual(b1, k, u(j)), ctx.ConstF32(Scene::TintColours[j][tint - 1]), colour);
    }
    return ctx.OpFAdd(f1, out, ctx.OpFMul(f1, ctx.OpFSub(f1, ctx.ConstF32(1.0f), out), colour));
}

// A lamp shader's light with the mask (light_shadow.h SceneMaps::Mask): its slot's 4 bits at this
// pixel's half-res place (15: lit); a light in no slot casts none. Nothing else: no setup, no march.
// (`tint`: 0, or the channel 1..3 asked: light_shadow.h SceneMaps::Tint.)
Id MaskVisibility(EmitContext& ctx, u32 light, Id ndc, Id uv, Id depth, u32 tint = 0) {
    namespace Scene = Tune::SceneMaps;
    auto& s = ctx.vr_light_shadow;
    const auto& info = ctx.info.vr_light_shadow;
    const Id f1 = ctx.F32[1], f3 = ctx.F32[3], u1 = ctx.U32[1], b1 = ctx.U1[1];
    const auto u = [&](u32 v) { return ctx.ConstU32(v); };
    SceneSetup(ctx);
    if (!Sirit::ValidId(s.scene_count)) {
        return ctx.ConstF32(1.0f);
    }
    if (!s.mask_ready) {
        s.mask_ready = true;
        const auto& buffers = ctx.info.buffers;
        const auto mask_it = std::ranges::find(buffers, BufferType::VrShadowMask, &BufferResource::buffer_type);
        if (mask_it == buffers.end()) {
            return ctx.ConstF32(1.0f);
        }
        s.mask_buffer = static_cast<u32>(std::distance(buffers.begin(), mask_it));
        const Id w = ctx.OpLoad(u1, MaskPtr(ctx, u(0u)));
        const Id h = ctx.OpLoad(u1, MaskPtr(ctx, u(1u)));
        const Id eyes = ctx.OpLoad(u1, MaskPtr(ctx, u(2u)));
        Id eye = u(0u);
        if (ctx.runtime_info.native_stereo && ctx.runtime_info.native_multiview) {
            eye = ctx.NativeEyeIndex();
        }
        const Id fits = ctx.OpLogicalAnd(
            b1, ctx.OpLogicalAnd(b1, ctx.OpUGreaterThan(b1, w, ctx.u32_zero_value), ctx.OpUGreaterThan(b1, h, ctx.u32_zero_value)),
            ctx.OpLogicalAnd(b1, ctx.OpULessThan(b1, eye, eyes),
                             ctx.OpULessThanEqual(b1, ctx.OpIMul(u1, ctx.OpIMul(u1, w, h), eyes), s.scene_capacity)));
        // (The blend between blocks, 2026-11-15 morning, is OFF: in the headset, where the foveation's
        // blocks are larger than in the test window, it turned the lamps' shadows into blobs that
        // drift as the head turns (the user, the same day: "it's like blobs", "lamp shadows drift
        // when I turn my head"). VR_MASK_BLOCK_BLEND=1: tests.)
        if (!Scene::MaskBlockBlend()) {
            // The 4 half-res pixels round this one's centre, bilinear (no 2x2 steps at shadow edges).
            const Id gx = ctx.OpFSub(f1, ctx.OpFMul(f1, EmitGetAttribute(ctx, IR::Attribute::FragCoord, 0, 0), ctx.ConstF32(0.5f)), ctx.ConstF32(0.5f));
            const Id gy = ctx.OpFSub(f1, ctx.OpFMul(f1, EmitGetAttribute(ctx, IR::Attribute::FragCoord, 1, 0), ctx.ConstF32(0.5f)), ctx.ConstF32(0.5f));
            s.mask_fx = ctx.OpFract(f1, gx);
            s.mask_fy = ctx.OpFract(f1, gy);
            const Id wm = ctx.OpISub(u1, ctx.OpUMax(u1, w, u(1u)), u(1u)), hm = ctx.OpISub(u1, ctx.OpUMax(u1, h, u(1u)), u(1u));
            const auto clamp_to = [&](Id v, Id hi) {
                return ctx.OpBitcast(u1, ctx.OpSClamp(ctx.S32[1], v, ctx.ConstS32(0), ctx.OpBitcast(ctx.S32[1], hi)));
            };
            const Id x0s = ctx.OpConvertFToS(ctx.S32[1], ctx.OpFloor(f1, gx)), y0s = ctx.OpConvertFToS(ctx.S32[1], ctx.OpFloor(f1, gy));
            const std::array<Id, 2> xs{clamp_to(x0s, wm), clamp_to(ctx.OpIAdd(ctx.S32[1], x0s, ctx.ConstS32(1)), wm)};
            const std::array<Id, 2> ys{clamp_to(y0s, hm), clamp_to(ctx.OpIAdd(ctx.S32[1], y0s, ctx.ConstS32(1)), hm)};
            for (u32 c = 0; c < 4; ++c) {
                const Id pixel = ctx.OpIAdd(u1, ctx.OpIMul(u1, ctx.OpIAdd(u1, ctx.OpIMul(u1, eye, h), ys[c >> 1]), w), xs[c & 1]);
                const Id first = ctx.OpSelect(u1, fits, ctx.OpIAdd(u1, u(Scene::MaskHeader), ctx.OpIMul(u1, pixel, u(Scene::MaskWords))),
                                              u(Scene::MaskHeader));
                for (u32 j = 0; j < Scene::MaskWords; ++j) {
                    s.mask_corner[c][j] = ctx.OpSelect(u1, fits, ctx.OpLoad(u1, MaskPtr(ctx, ctx.OpIAdd(u1, first, u(j)))), u(~0u));
                }
            }
        } else {
            // This pixel's own mask pixel first: its last word names the block the pass shaded as one
            // (1, 2 or 4 mask pixels a side: the foveation's). Then the blend is between BLOCKS: this
            // one's value at its middle, the next block's (on the side this pixel lies) from its
            // border on. (Until 2026-11-15 the blend was between neighbouring mask pixels only: inside
            // a block all are the same, and every block showed as a square step. The user: "shadow
            // edges look kinda blocky".) A block of one pixel: the plain bilinear, as before.
            const Id s1 = ctx.S32[1];
            const Id px = ctx.OpFMul(f1, EmitGetAttribute(ctx, IR::Attribute::FragCoord, 0, 0), ctx.ConstF32(0.5f));
            const Id py = ctx.OpFMul(f1, EmitGetAttribute(ctx, IR::Attribute::FragCoord, 1, 0), ctx.ConstF32(0.5f));
            const Id wm = ctx.OpISub(u1, ctx.OpUMax(u1, w, u(1u)), u(1u)), hm = ctx.OpISub(u1, ctx.OpUMax(u1, h, u(1u)), u(1u));
            const auto clamp_to = [&](Id v, Id hi) {
                return ctx.OpBitcast(u1, ctx.OpSClamp(s1, v, ctx.ConstS32(0), ctx.OpBitcast(s1, hi)));
            };
            const auto read_at = [&](Id x, Id y, u32 c) {
                const Id pixel = ctx.OpIAdd(u1, ctx.OpIMul(u1, ctx.OpIAdd(u1, ctx.OpIMul(u1, eye, h), y), w), x);
                const Id first = ctx.OpSelect(u1, fits, ctx.OpIAdd(u1, u(Scene::MaskHeader), ctx.OpIMul(u1, pixel, u(Scene::MaskWords))),
                                              u(Scene::MaskHeader));
                for (u32 j = 0; j < Scene::MaskWords; ++j) {
                    s.mask_corner[c][j] = ctx.OpSelect(u1, fits, ctx.OpLoad(u1, MaskPtr(ctx, ctx.OpIAdd(u1, first, u(j)))), u(~0u));
                }
            };
            const Id cx = clamp_to(ctx.OpConvertFToS(s1, ctx.OpFloor(f1, px)), wm);
            const Id cy = clamp_to(ctx.OpConvertFToS(s1, ctx.OpFloor(f1, py)), hm);
            read_at(cx, cy, 0);
            const Id own_word = s.mask_corner[0][Scene::MaskSlotWords];
            // (No mask, a stand-in: all bits set; a block is 4 a side at most.)
            const Id lbw = ctx.OpUMin(u1, ctx.OpBitwiseAnd(u1, ctx.OpShiftRightLogical(u1, own_word, u(28u)), u(3u)), u(2u));
            const Id lbh = ctx.OpUMin(u1, ctx.OpBitwiseAnd(u1, ctx.OpShiftRightLogical(u1, own_word, u(30u)), u(3u)), u(2u));
            // Along one axis: the next block's nearest pixel and its share (0 at this block's middle,
            // a half at its border).
            const auto axis = [&](Id at, Id c, Id lb, Id last, Id& next, Id& share) {
                const Id side = ctx.OpShiftLeftLogical(u1, u(1u), lb);
                const Id first = ctx.OpShiftLeftLogical(u1, ctx.OpShiftRightLogical(u1, c, lb), lb);
                const Id side_f = ctx.OpConvertUToF(f1, side);
                const Id rel = ctx.OpFDiv(
                    f1, ctx.OpFSub(f1, at, ctx.OpFAdd(f1, ctx.OpConvertUToF(f1, first), ctx.OpFMul(f1, side_f, ctx.ConstF32(0.5f)))), side_f);
                const Id before = ctx.OpFOrdLessThan(b1, rel, ctx.ConstF32(0.0f));
                const Id first_s = ctx.OpBitcast(s1, first);
                next = clamp_to(ctx.OpSelect(s1, before, ctx.OpISub(s1, first_s, ctx.ConstS32(1)), ctx.OpIAdd(s1, first_s, ctx.OpBitcast(s1, side))),
                                last);
                // (A block of more than one pixel: the blend only over the last MaskBlockRamp of the way
                // to its border, the block's own value before it. Over the whole way the soft ramp read
                // as a fatter shadow: the tester, 2026-11-15. A half at the border either way.)
                const float ramp = Scene::MaskBlockRamp();
                const Id whole = ctx.OpFClamp(f1, ctx.OpFAbs(f1, rel), ctx.ConstF32(0.0f), ctx.ConstF32(0.5f));
                const Id late = ctx.OpFClamp(f1, ctx.OpFMul(f1, ctx.OpFSub(f1, ctx.OpFAbs(f1, rel), ctx.ConstF32(0.5f - ramp)), ctx.ConstF32(0.5f / ramp)),
                                             ctx.ConstF32(0.0f), ctx.ConstF32(0.5f));
                share = ctx.OpSelect(f1, ctx.OpIEqual(b1, lb, ctx.u32_zero_value), whole, late);
            };
            Id nx{}, ny{};
            axis(px, cx, lbw, wm, nx, s.mask_fx);
            axis(py, cy, lbh, hm, ny, s.mask_fy);
            read_at(nx, cy, 1);
            read_at(cx, ny, 2);
            read_at(nx, ny, 3);
        }
        s.mask_word[0] = s.mask_corner[0][0];
        // Their weights: bilinear, times how near each one's distance is to this pixel's own (none
        // past MaskDepthShare of it); all refused (or no mask): the plain bilinear.
        // (This pixel's own distance: the lamp shader's matrix on its ndc and depth.)
        Id own{}, own_facing{};
        if (Sirit::ValidId(ndc) && Sirit::ValidId(depth)) {
            std::array<Id, 4> rows;
            for (u32 r = 0; r < 4; ++r) {
                rows[r] = ctx.OpCompositeConstruct(ctx.F32[4], std::array{ReadF(ctx, info.matrix_buffer, 4 * r), ReadF(ctx, info.matrix_buffer, 4 * r + 1),
                                                                          ReadF(ctx, info.matrix_buffer, 4 * r + 2), ReadF(ctx, info.matrix_buffer, 4 * r + 3)});
            }
            const auto p4 = Transform(ctx, rows, ctx.OpCompositeExtract(f1, ndc, 0u), ctx.OpCompositeExtract(f1, ndc, 1u), depth);
            const Id iw = ctx.OpFDiv(f1, ctx.ConstF32(1.0f), p4[3]);
            const Id pv = ctx.OpCompositeConstruct(f3, std::array{ctx.OpFMul(f1, p4[0], iw), ctx.OpFMul(f1, p4[1], iw), ctx.OpFMul(f1, p4[2], iw)});
            own = ctx.OpLength(f1, pv);
            if (Scene::MaskNormals()) {
                // (This pixel's facing, as the mask pass takes it: its view place's neighbours,
                // turned to face the eye; here in the entry block, where derivatives hold.)
                const Id n = ctx.OpNormalize(f3, ctx.OpCross(f3, ctx.OpDPdy(f3, pv), ctx.OpDPdx(f3, pv)));
                own_facing = ctx.OpSelect(f3, ctx.OpFOrdLessThan(b1, ctx.OpDot(f1, n, ctx.OpFNegate(f3, pv)), ctx.ConstF32(0.0f)),
                                          ctx.OpFNegate(f3, n), n);
            }
            const auto edges_it = std::ranges::find(ctx.info.buffers, BufferType::VrShadowEdges,
                                                    &BufferResource::buffer_type);
            if (Sirit::ValidId(s.edge_slots) && Scene::EdgePass() && edges_it != ctx.info.buffers.end()) {
                // The edge pass (light_shadow.h EdgePass): this pixel's byte, two bits a frame's
                // edge lamp, when the pass ran this frame (its serial in the buffer and the slot
                // header agree) and the pixel is in its picture; else 0 (every lamp's blend).
                namespace Body = Shader::Optimization::BodyShadow;
                const u32 eb = static_cast<u32>(std::distance(ctx.info.buffers.begin(), edges_it));
                s.edge_pv = pv;
                s.edge_own = own;
                const Id x = ctx.OpConvertFToU(u1, EmitGetAttribute(ctx, IR::Attribute::FragCoord, 0, 0));
                const Id y = ctx.OpConvertFToU(u1, EmitGetAttribute(ctx, IR::Attribute::FragCoord, 1, 0));
                Id eye = u(0u);
                if (ctx.runtime_info.native_stereo && ctx.runtime_info.native_multiview) {
                    eye = ctx.NativeEyeIndex();
                }
                const Id serial = ctx.OpLoad(u1, EdgesPtr(ctx, eb, u(Scene::EdgeHeaderAt)));
                const Id ew = ctx.OpLoad(u1, EdgesPtr(ctx, eb, u(Scene::EdgeHeaderAt + 1)));
                const Id eh = ctx.OpLoad(u1, EdgesPtr(ctx, eb, u(Scene::EdgeHeaderAt + 2)));
                const Id wq = ctx.OpLoad(u1, EdgesPtr(ctx, eb, u(Scene::EdgeHeaderAt + 3)));
                const Id want = ctx.OpBitwiseAnd(
                    u1, ctx.OpShiftRightLogical(u1, BodyRead(ctx, s.scene_body, Body::SlotsAt + 2), u(Scene::EdgeSerialShift)),
                    u(Scene::EdgeSerialMask));
                const Id valid = ctx.OpLogicalAnd(
                    b1, ctx.OpLogicalAnd(b1, ctx.OpIEqual(b1, serial, want), ctx.OpINotEqual(b1, want, u(0u))),
                    ctx.OpLogicalAnd(b1, ctx.OpULessThan(b1, x, ew), ctx.OpULessThan(b1, y, eh)));
                // (A pixel's bits, light_shadow.h EdgeBits: a byte, four pixels a word; 16 bits for
                // High's five lamps, two a word.)
                const bool wide = Scene::EdgeBits() == 16;
                const Id at = ctx.OpIAdd(u1, u(Scene::EdgeDataAt),
                                         ctx.OpIAdd(u1, ctx.OpIMul(u1, ctx.OpIAdd(u1, ctx.OpIMul(u1, eye, eh), y), wq),
                                                    ctx.OpShiftRightLogical(u1, x, u(wide ? 1u : 2u))));
                const Id word = ctx.OpLoad(u1, EdgesPtr(ctx, eb, ctx.OpSelect(u1, valid, at, u(Scene::EdgeHeaderAt))));
                const Id byte = ctx.OpBitwiseAnd(
                    u1, ctx.OpShiftRightLogical(u1, word, ctx.OpShiftLeftLogical(u1, ctx.OpBitwiseAnd(u1, x, u(wide ? 1u : 3u)),
                                                                                u(wide ? 4u : 3u))),
                    u(wide ? 0xFFFFu : 255u));
                s.edge_code = ctx.OpSelect(u1, valid, byte, u(0u));
            } else if (Sirit::ValidId(s.edge_slots) && Sirit::ValidId(uv) && !Scene::EdgePass()) {
                // The edges' march (light_shadow.h SceneMaps::Edges): what each light's needs of
                // this one-time part, and the picture's coordinates from ndc (uv = a ndc + b, each
                // axis its own; Setup's way: derivatives, here in the entry block).
                s.edge_pv = pv;
                s.edge_own = own;
                s.edge_w = p4[3];
                s.edge_ndc = ndc;
                s.edge_rows = rows;
                const Id nx = ctx.OpCompositeExtract(f1, ndc, 0u), ny = ctx.OpCompositeExtract(f1, ndc, 1u);
                const Id ux = ctx.OpCompositeExtract(f1, uv, 0u), uy = ctx.OpCompositeExtract(f1, uv, 1u);
                const Id dnx = ctx.OpDPdx(f1, nx), dny = ctx.OpDPdy(f1, ny);
                const Id dux = ctx.OpDPdx(f1, ux), duy = ctx.OpDPdy(f1, uy);
                const Id ax = ctx.OpFDiv(f1, dux, dnx), ay = ctx.OpFDiv(f1, duy, dny);
                s.edge_a = ctx.OpCompositeConstruct(ctx.F32[2], std::array{ax, ay});
                s.edge_b = ctx.OpCompositeConstruct(
                    ctx.F32[2], std::array{ctx.OpFSub(f1, ux, ctx.OpFMul(f1, ax, nx)), ctx.OpFSub(f1, uy, ctx.OpFMul(f1, ay, ny))});
                s.edge_pixel = ctx.OpCompositeConstruct(ctx.F32[2], std::array{ctx.OpFAbs(f1, dux), ctx.OpFAbs(f1, duy)});
                s.edge_ok = ctx.OpLogicalAnd(b1, ctx.OpFOrdGreaterThan(b1, ctx.OpFAbs(f1, dnx), ctx.ConstF32(1e-9f)),
                                             ctx.OpFOrdGreaterThan(b1, ctx.OpFAbs(f1, dny), ctx.ConstF32(1e-9f)));
                if (info.count > 1) {
                    s.edge_seen = Private(ctx, u1);
                    ctx.OpStore(s.edge_seen, u(0));
                }
            }
        }
        const std::array<Id, 4> bil{ctx.OpFMul(f1, ctx.OpFSub(f1, ctx.ConstF32(1.0f), s.mask_fx), ctx.OpFSub(f1, ctx.ConstF32(1.0f), s.mask_fy)),
                                    ctx.OpFMul(f1, s.mask_fx, ctx.OpFSub(f1, ctx.ConstF32(1.0f), s.mask_fy)),
                                    ctx.OpFMul(f1, ctx.OpFSub(f1, ctx.ConstF32(1.0f), s.mask_fx), s.mask_fy),
                                    ctx.OpFMul(f1, s.mask_fx, s.mask_fy)};
        std::array<Id, 4> wt = bil;
        if (Sirit::ValidId(own)) {
            Id sum = ctx.ConstF32(0.0f);
            Id best_rel = ctx.ConstF32(1e30f);
            Id best_c = u(0u);
            for (u32 c = 0; c < 4; ++c) {
                const Id dc = MaskDepthOf(ctx, s.mask_corner[c][Scene::MaskSlotWords]);
                const Id rel = ctx.OpFDiv(f1, ctx.OpFAbs(f1, ctx.OpFSub(f1, dc, own)), ctx.OpFMax(f1, own, ctx.ConstF32(0.05f)));
                // (Not a number, a stand-in's corner: refused.)
                Id near_w = ctx.OpFClamp(f1, ctx.OpFSub(f1, ctx.ConstF32(1.0f), ctx.OpFMul(f1, rel, ctx.ConstF32(1.0f / Scene::MaskDepthShare))),
                                         ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
                if (Sirit::ValidId(own_facing)) {
                    // ... and by how near its facing is to this pixel's (light_shadow.h MaskNormals).
                    const Id cosine = ctx.OpDot(f1, MaskFacingOf(ctx, s.mask_corner[c][Scene::MaskSlotWords]), own_facing);
                    near_w = ctx.OpFMul(f1, near_w,
                                        ctx.OpFClamp(f1, ctx.OpFMul(f1, ctx.OpFSub(f1, cosine, ctx.ConstF32(Scene::MaskNormalFrom)),
                                                                    ctx.ConstF32(1.0f / (Scene::MaskNormalTo - Scene::MaskNormalFrom))),
                                                     ctx.ConstF32(0.0f), ctx.ConstF32(1.0f)));
                }
                wt[c] = ctx.OpSelect(f1, ctx.OpFOrdEqual(b1, rel, rel), ctx.OpFMul(f1, bil[c], near_w), ctx.ConstF32(0.0f));
                sum = ctx.OpFAdd(f1, sum, wt[c]);
                // (The corner nearest this pixel's depth; a NaN never compares less.)
                const Id nearer = ctx.OpFOrdLessThan(b1, rel, best_rel);
                best_rel = ctx.OpSelect(f1, nearer, rel, best_rel);
                best_c = ctx.OpSelect(u1, nearer, u(c), best_c);
            }
            // Every corner at another depth (a thin thing in front of what the mask saw beside it):
            // the one nearest its depth alone. The blend of the refused corners bled the wall's
            // shadow onto the thing in front (a code review, 2026-11-13 evening); the plain blend
            // stays only where every corner is a stand-in's.
            const Id ok = ctx.OpFOrdGreaterThan(b1, sum, ctx.ConstF32(1e-3f));
            // (Little to go by: under half the blend's weight survived the depth and facing tests.
            // The edges' march then takes all four neighbours as its bounds: MaskVisibility below.)
            const bool edges = Sirit::ValidId(s.edge_pv);
            const Id unsure = edges ? ctx.OpFOrdLessThan(b1, sum, ctx.ConstF32(0.5f)) : Id{};
            const Id any = ctx.OpFOrdLessThan(b1, best_rel, ctx.ConstF32(1e29f));
            for (u32 c = 0; c < 4; ++c) {
                const Id only = ctx.OpSelect(f1, ctx.OpIEqual(b1, best_c, u(c)), ctx.ConstF32(1.0f), ctx.ConstF32(0.0f));
                wt[c] = ctx.OpSelect(f1, ok, ctx.OpFDiv(f1, wt[c], ctx.OpFMax(f1, sum, ctx.ConstF32(1e-3f))),
                                     ctx.OpSelect(f1, any, only, bil[c]));
                if (edges) {
                    // A weak blend admits all corners. Every lamp uses this same set.
                    s.edge_corner[c] = ctx.OpLogicalOr(b1, unsure, ctx.OpFOrdGreaterThan(b1, wt[c], ctx.ConstF32(0.0f)));
                }
            }
            if (edges && Sirit::ValidId(s.edge_code)) {
                // The edge pass (light_shadow.h EdgePass): what the lights need of this pixel kept in
                // two values through the lamp shader's light loop: the corner set (bits 0..3) with the
                // pass's byte (bits 4..11), and the nearness. Four flags, the byte and the distance
                // held apart cost every lamp pixel (their registers through the game's light loop).
                Id packed = ctx.OpShiftLeftLogical(u1, s.edge_code, u(4u));
                for (u32 c = 0; c < 4; ++c) {
                    packed = ctx.OpBitwiseOr(u1, packed, ctx.OpSelect(u1, s.edge_corner[c], u(1u << c), u(0u)));
                }
                s.edge_packed = packed;
                const auto [far_in, far_out] = Scene::EdgeFar();
                s.edge_near = ctx.OpFClamp(
                    f1, ctx.OpFMul(f1, ctx.OpFSub(f1, ctx.ConstF32(far_out), s.edge_own), ctx.ConstF32(1.0f / (far_out - far_in))),
                    ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
            }
        }
        s.mask_w = wt;
    }
    if (!Sirit::ValidId(s.mask_word[0])) {
        return ctx.ConstF32(1.0f);
    }
    const u32 index = info.stride != 0 ? (8 * light) / info.stride : 0;
    if (index >= Scene::DrawLights) {
        return ctx.ConstF32(1.0f);
    }
    const u32 count = std::min(info.count, Scene::DrawLights);
    if (count < 2 || !info.stride || !Sirit::ValidId(s.edge_corner[0]) || Scene::Tint() || Scene::EdgePass()) {
        return MaskLight(ctx, u(index), u(8 * light), tint);
    }
    if (index >= count) {
        return ctx.ConstF32(1.0f);
    }
    // One copy of the edge march even in the twelve-light Woods shader. The array
    // belongs to this fragment invocation; neither eye writes to a shared buffer.
    const Id ptr = ctx.TypePointer(spv::StorageClass::Private, f1);
    if (!Sirit::ValidId(s.edge_visibility)) {
        s.edge_visibility = Private(ctx, ctx.TypeArray(f1, u(count)));
        LoopCount(ctx, u(count), [&](Id i) {
            const Id vis = MaskLight(ctx, i, ctx.OpIMul(u1, i, u(info.stride)), 0);
            ctx.OpStore(ctx.OpAccessChain(ptr, s.edge_visibility, i), vis);
        }, nullptr, spv::LoopControlMask::DontUnroll);
    }
    return ctx.OpLoad(f1, ctx.OpAccessChain(ptr, s.edge_visibility, u(index)));
}

// Tests: the light's class bit (body_shadow.h ProbeAt) from its dword 7.
Id ProbeClass(EmitContext& ctx, Id t) {
    const auto is = [&](float v) {
        return ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpFAbs(ctx.F32[1], ctx.OpFSub(ctx.F32[1], t, ctx.ConstF32(v))),
                                  ctx.ConstF32(0.01f));
    };
    return ctx.OpSelect(ctx.U32[1], is(1.0f), ctx.ConstU32(1u),
                        ctx.OpSelect(ctx.U32[1], is(0.5f), ctx.ConstU32(2u),
                                     ctx.OpSelect(ctx.U32[1], is(20.0f), ctx.ConstU32(4u), ctx.ConstU32(8u))));
}

// The light at `centre` (this eye's view) of reach dword `reach_k` is in a slot (its place within
// 2 cm, its reach within 2%): one the game keeps on the camera (`camera`: the first
// light_shadow.h AttachedCameraSlots) or a NoCastLights one. Either casts nothing.
struct Attached {
    Id any;
    Id camera;
};
Attached AttachedLight(EmitContext& ctx, Id centre, Id reach_k) {
    const auto& s = ctx.vr_light_shadow;
    if (!Sirit::ValidId(s.attached_count)) {
        return {ctx.false_value, ctx.false_value};
    }
    // Its place in the flat view: this eye's moved by the half shift where each eye has its own
    // list (the asymmetric eyes), or the same where both share the flat one: either matches.
    const Id flat = ctx.OpFAdd(ctx.F32[3], centre, s.attached_shift);
    Id any = ctx.false_value, camera = ctx.false_value;
    for (u32 k = 0; k < 4; ++k) {
        const Id place = ctx.OpVectorShuffle(ctx.F32[3], s.attached[k], s.attached[k], 0, 1, 2);
        const Id k_at = ctx.OpCompositeExtract(ctx.F32[1], s.attached[k], 3u);
        const auto near_to = [&](Id at) {
            return ctx.OpFOrdLessThan(ctx.U1[1],
                                      ctx.OpLength(ctx.F32[1], ctx.OpFSub(ctx.F32[3], at, place)),
                                      ctx.ConstF32(0.02f));
        };
        const Id same_reach = ctx.OpFOrdLessThanEqual(
            ctx.U1[1], ctx.OpFAbs(ctx.F32[1], ctx.OpFSub(ctx.F32[1], reach_k, k_at)),
            ctx.OpFMul(ctx.F32[1], ctx.ConstF32(0.02f), ctx.OpFAbs(ctx.F32[1], k_at)));
        const Id here = ctx.OpLogicalAnd(
            ctx.U1[1], ctx.OpLogicalAnd(ctx.U1[1], ctx.OpULessThan(ctx.U1[1], ctx.ConstU32(k), s.attached_count), same_reach),
            ctx.OpLogicalOr(ctx.U1[1], near_to(flat), near_to(centre)));
        Id& into = k < Tune::AttachedCameraSlots ? camera : any;
        into = ctx.OpLogicalOr(ctx.U1[1], into, here);
    }
    return {ctx.OpLogicalOr(ctx.U1[1], any, camera), camera};
}

// The other eye's matrices (light_shadow.h CrossEye), once, where Setup is: both eyes in one draw.
void SetupOtherEye(EmitContext& ctx) {
    auto& s = ctx.vr_light_shadow;
    if (s.other_ready || !Tune::CrossEye() || !ctx.runtime_info.native_stereo ||
        !ctx.runtime_info.native_multiview) {
        return;
    }
    const auto& info = ctx.info.vr_light_shadow;
    s.this_eye = ctx.NativeEyeIndex();
    s.other_eye = ctx.OpBitwiseXor(ctx.U32[1], s.this_eye, ctx.ConstU32(1u));
    std::array<std::array<Id, 4>, 4> m;
    ctx.eye_override = s.other_eye;
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            m[r][c] = ReadF(ctx, info.matrix_buffer, 4 * r + c);
        }
    }
    ctx.eye_override = Id{};
    s.other_inv = InverseRows(ctx, m);
    s.other_row2 = ctx.OpCompositeConstruct(ctx.F32[4], m[2]);
    s.other_row3 = ctx.OpCompositeConstruct(ctx.F32[4], m[3]);
    // A matrix both eyes share would be this eye's view again: no other eye then.
    Id differs = ctx.false_value;
    for (u32 r = 0; r < 4; ++r) {
        differs = ctx.OpLogicalOr(
            ctx.U1[1], differs,
            ctx.OpAny(ctx.U1[1], ctx.OpFUnordNotEqual(ctx.U1[4], s.rows[r],
                                                      ctx.OpCompositeConstruct(ctx.F32[4], m[r]))));
    }
    s.other_differs = differs;
    s.other_ready = true;
}

void Setup(EmitContext& ctx, Id ndc, Id uv, Id depth, bool inverse) {
    auto& s = ctx.vr_light_shadow;
    const auto& info = ctx.info.vr_light_shadow;
    // M's elements m[r][c] = dword 4r + c; its rows as vectors.
    std::array<std::array<Id, 4>, 4> m;
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            m[r][c] = s.rows_given ? ctx.OpCompositeExtract(ctx.F32[1], s.rows[r], c)
                                   : ReadF(ctx, info.matrix_buffer, 4 * r + c);
        }
        if (!s.rows_given) {
            s.rows[r] = ctx.OpCompositeConstruct(ctx.F32[4], m[r]);
        }
    }
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(ctx.F32[1], a, b); };
    const auto sub = [&](Id a, Id b) { return ctx.OpFSub(ctx.F32[1], a, b); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(ctx.F32[1], a, b); };
    // The inverse (the light shadows' projection; the AO needs none).
    if (inverse) {
        Invert(ctx, m);
    }
    // P = (M (ndc, depth, 1)).xyz / w.
    const Id nx = ctx.OpCompositeExtract(ctx.F32[1], ndc, 0u);
    const Id ny = ctx.OpCompositeExtract(ctx.F32[1], ndc, 1u);
    const auto p4 = Transform(ctx, s.rows, nx, ny, depth);
    const Id inv_w = ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), p4[3]);
    s.p = ctx.OpCompositeConstruct(
        ctx.F32[3], std::array{mul(p4[0], inv_w), mul(p4[1], inv_w), mul(p4[2], inv_w)});
    // The surface's normal from the view position's neighbours (facing the eye), and whether it
    // can be trusted: no depth edge across the pixel (each neighbour within FacingEdgeShare of
    // the pixel's distance). The light shadows skip lights the surface faces away from.
    {
        const Id px = ctx.OpDPdx(ctx.F32[3], s.p), py = ctx.OpDPdy(ctx.F32[3], s.p);
        Id normal = ctx.OpNormalize(ctx.F32[3], ctx.OpCross(ctx.F32[3], py, px));
        const Id to_eye = ctx.OpFNegate(ctx.F32[3], s.p);
        normal = ctx.OpSelect(ctx.F32[3],
                              ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpDot(ctx.F32[1], normal, to_eye),
                                                 ctx.ConstF32(0.0f)),
                              ctx.OpFNegate(ctx.F32[3], normal), normal);
        s.normal = normal;
        const Id limit =
            mul(ctx.ConstF32(Tune::FacingEdgeShare), ctx.OpLength(ctx.F32[1], s.p));
        s.normal_ok = ctx.OpLogicalAnd(
            ctx.U1[1],
            ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpLength(ctx.F32[1], px), limit),
            ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpLength(ctx.F32[1], py), limit));
    }
    // Coordinates from ndc: uv = a ndc + b (each axis follows its own screen axis).
    const Id ux = ctx.OpCompositeExtract(ctx.F32[1], uv, 0u);
    const Id uy = ctx.OpCompositeExtract(ctx.F32[1], uv, 1u);
    const Id dnx = ctx.OpDPdx(ctx.F32[1], nx), dny = ctx.OpDPdy(ctx.F32[1], ny);
    const Id ax = ctx.OpFDiv(ctx.F32[1], ctx.OpDPdx(ctx.F32[1], ux), dnx);
    const Id ay = ctx.OpFDiv(ctx.F32[1], ctx.OpDPdy(ctx.F32[1], uy), dny);
    s.a = ctx.OpCompositeConstruct(ctx.F32[2], std::array{ax, ay});
    s.b = ctx.OpCompositeConstruct(ctx.F32[2], std::array{sub(ux, mul(ax, nx)), sub(uy, mul(ay, ny))});
    const auto defined = [&](Id d) {
        return ctx.OpFOrdGreaterThan(ctx.U1[1], ctx.OpFAbs(ctx.F32[1], d), ctx.ConstF32(1e-9f));
    };
    s.ok = ctx.OpLogicalAnd(ctx.U1[1], defined(dnx), defined(dny));
    s.pixel = ctx.OpCompositeConstruct(
        ctx.F32[2], std::array{ctx.OpFAbs(ctx.F32[1], ctx.OpDPdx(ctx.F32[1], ux)),
                               ctx.OpFAbs(ctx.F32[1], ctx.OpDPdy(ctx.F32[1], uy))});
    // White noise per pixel and per frame: the pixel's position hashed with its depth's bits,
    // which move every frame with the upscaler's sub-pixel jitter (an upscaler is required).
    const Id fx = EmitGetAttribute(ctx, IR::Attribute::FragCoord, 0, 0);
    const Id fy = EmitGetAttribute(ctx, IR::Attribute::FragCoord, 1, 0);
    const auto u32c = [&](u32 v) { return ctx.ConstU32(v); };
    const auto hash = [&](Id h) { // lowbias32
        h = ctx.OpBitwiseXor(ctx.U32[1], h, ctx.OpShiftRightLogical(ctx.U32[1], h, u32c(16)));
        h = ctx.OpIMul(ctx.U32[1], h, u32c(0x7feb352du));
        h = ctx.OpBitwiseXor(ctx.U32[1], h, ctx.OpShiftRightLogical(ctx.U32[1], h, u32c(15)));
        h = ctx.OpIMul(ctx.U32[1], h, u32c(0x846ca68bu));
        return ctx.OpBitwiseXor(ctx.U32[1], h, ctx.OpShiftRightLogical(ctx.U32[1], h, u32c(16)));
    };
    const auto unit = [&](Id h) { // [0, 1)
        return mul(ctx.OpConvertUToF(ctx.F32[1], ctx.OpShiftRightLogical(ctx.U32[1], h, u32c(8))),
                   ctx.ConstF32(1.0f / 16777216.0f));
    };
    Id h = ctx.OpBitwiseXor(
        ctx.U32[1], ctx.OpBitcast(ctx.U32[1], depth),
        ctx.OpBitwiseXor(ctx.U32[1],
                         ctx.OpIMul(ctx.U32[1], ctx.OpConvertFToU(ctx.U32[1], fx), u32c(1973u)),
                         ctx.OpIMul(ctx.U32[1], ctx.OpConvertFToU(ctx.U32[1], fy), u32c(9277u))));
    h = hash(h);
    s.seed = unit(h);
    // The steps' jitter as interleaved gradient noise (Jimenez 2014) instead (2026-11-11, the
    // user: "more detailed and more lightweight"): neighbouring pixels take evenly spread values,
    // so the upscaler's accumulation settles in fewer frames and with less crawl than white noise
    // gives. Its pattern moves every frame by the frame's own number: the matrix's x and y rows'
    // depth and constant terms, which carry the upscaler's sub-pixel jitter, hashed (the same for
    // every pixel of a frame, another each frame). VR_LIGHT_NOISE=0 (tests): white noise.
    static const bool gradient_noise = [] {
        const char* const env = std::getenv("VR_LIGHT_NOISE");
        return env == nullptr || env[0] != '0';
    }();
    if (gradient_noise) {
        Id frame_bits = ctx.OpBitcast(ctx.U32[1], m[0][2]);
        for (const Id v : {m[0][3], m[1][2], m[1][3]}) {
            frame_bits = ctx.OpBitwiseXor(
                ctx.U32[1], ctx.OpIMul(ctx.U32[1], frame_bits, u32c(16777619u)),
                ctx.OpBitcast(ctx.U32[1], v));
        }
        const Id frame_index = ctx.OpConvertUToF(ctx.F32[1], ctx.OpBitwiseAnd(ctx.U32[1], hash(frame_bits), u32c(63u)));
        const Id frame = mul(frame_index, ctx.ConstF32(5.588238f));
        // (The frame alone, for a noise tied to the world that still turns every frame.)
        s.frame_turn = ctx.OpFract(ctx.F32[1], mul(frame_index, ctx.ConstF32(0.618034f)));
        const Id inner = ctx.OpFract(
            ctx.F32[1], add(mul(add(fx, frame), ctx.ConstF32(0.06711056f)),
                            mul(add(fy, frame), ctx.ConstF32(0.00583715f))));
        s.seed = ctx.OpFract(ctx.F32[1], mul(inner, ctx.ConstF32(52.9829189f)));
        // (The same without the frame: ambient_occlusion.h FrameNoise.)
        s.seed_still = ctx.OpFract(
            ctx.F32[1], mul(ctx.OpFract(ctx.F32[1], add(mul(fx, ctx.ConstF32(0.06711056f)),
                                                         mul(fy, ctx.ConstF32(0.00583715f)))),
                            ctx.ConstF32(52.9829189f)));
    }
    std::array<Id, 3> ball;
    for (u32 k = 0; k < 3; ++k) {
        h = hash(ctx.OpIAdd(ctx.U32[1], h, u32c(0x9e3779b9u)));
        ball[k] = sub(mul(unit(h), ctx.ConstF32(2.0f)), ctx.ConstF32(1.0f));
    }
    s.ball = ctx.OpCompositeConstruct(ctx.F32[3], ball);
    s.vis = Private(ctx, ctx.F32[1]);
    s.gi = Private(ctx, ctx.F32[3]);
    s.occl = Private(ctx, ctx.F32[1]);
    s.step = Private(ctx, ctx.U32[1]);
    s.mask = Private(ctx, ctx.U32[1]);
    s.ready = true;
}

// The clearance round a light's fixture (its lamp, candle holder or pyre), from its reach's radius
// (light_shadow.h ClearSmallM: ClearPerReachM of it, ClearSmallM to ClearCapM; ReachOld: at least
// ClearFixtureM, ClearPerRadius of it; `air`: always that, light_shadow.h ClearAirOld).
Id FixtureClear(EmitContext& ctx, Id radius, bool air = false) {
    if (air || Tune::ReachOld()) {
        return ctx.OpFMax(ctx.F32[1], ctx.ConstF32(Tune::ClearFixtureM),
                          ctx.OpFMul(ctx.F32[1], radius, ctx.ConstF32(Tune::ClearPerRadius)));
    }
    // (light_shadow.h FixtureClearM: by the reach, or the rise to the cap at a fire's reach.)
    const Id by_reach = ctx.OpFMul(ctx.F32[1], radius, ctx.ConstF32(Tune::ClearPerReach()));
    const Id rise = ctx.OpFAdd(ctx.F32[1], ctx.ConstF32(Tune::ClearRiseAt0()),
                               ctx.OpFMul(ctx.F32[1], radius, ctx.ConstF32(Tune::ClearRisePerM())));
    return ctx.OpFClamp(ctx.F32[1], ctx.OpFMax(ctx.F32[1], by_reach, rise),
                        ctx.ConstF32(Tune::ClearSmall()), ctx.ConstF32(Tune::ClearCap()));
}

// The blocker grid (light_shadow.h Grid). A word's pointer (the buffer's first descriptor: both
// eyes' are the same buffer).
Id GridPtr(EmitContext& ctx, Id index) {
    const auto& s = ctx.vr_light_shadow;
    const auto& buffer = ctx.buffers[s.grid_buffer];
    if (const Id offset = buffer.Offset(EmitContext::PointerSize::B32); Sirit::ValidId(offset)) {
        index = ctx.OpIAdd(ctx.U32[1], index, offset);
    }
    const auto [id, pointer_type] = buffer.Alias(EmitContext::PointerType::U32);
    return ctx.runtime_info.native_stereo
               ? ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, ctx.u32_zero_value, index)
               : ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, index);
}

// A point of this eye's view in the grid's space (the world's, from the camera's place).
Id GridWorld(EmitContext& ctx, Id view_point) {
    const auto& s = ctx.vr_light_shadow;
    const auto x = [&](Id v, u32 k) { return ctx.OpCompositeExtract(ctx.F32[1], v, k); };
    const Id flat = ctx.OpFAdd(ctx.F32[3], view_point, s.grid_shift);
    const Id f4 = ctx.OpCompositeConstruct(ctx.F32[4],
                                           std::array{x(flat, 0), x(flat, 1), x(flat, 2), ctx.ConstF32(1.0f)});
    return ctx.OpCompositeConstruct(
        ctx.F32[3], std::array{ctx.OpDot(ctx.F32[1], s.grid_rows[0], f4),
                               ctx.OpDot(ctx.F32[1], s.grid_rows[1], f4),
                               ctx.OpDot(ctx.F32[1], s.grid_rows[2], f4)});
}

// A point of the grid's space: its voxel's word and its tag, in place (above the stamp's bits).
std::pair<Id, Id> GridCellAt(EmitContext& ctx, Id world) {
    namespace Grid = Tune::Grid;
    const auto& s = ctx.vr_light_shadow;
    const auto u = [&](u32 v) { return ctx.ConstU32(v); };
    // Its voxel's coordinates (bounded: nothing that is not a number turns into an index).
    const Id cell = ctx.OpFClamp(
        ctx.F32[3],
        ctx.OpFloor(ctx.F32[3], ctx.OpVectorTimesScalar(ctx.F32[3], world, s.grid_inv_voxel)),
        ctx.ConstF32(-1.0e6f, -1.0e6f, -1.0e6f), ctx.ConstF32(1.0e6f, 1.0e6f, 1.0e6f));
    const Id vi = ctx.OpConvertFToS(ctx.S32[3], cell);
    const auto axis = [&](u32 k, u32 side, u32 shift) {
        const Id c = ctx.OpCompositeExtract(ctx.S32[1], vi, k);
        const Id low = ctx.OpBitwiseAnd(ctx.U32[1], ctx.OpBitcast(ctx.U32[1], c), u(side - 1u));
        const Id tag = ctx.OpBitwiseAnd(
            ctx.U32[1],
            ctx.OpBitcast(ctx.U32[1], ctx.OpShiftRightArithmetic(ctx.S32[1], c, u(shift))), u(15u));
        return std::pair{low, tag};
    };
    const auto [cx, tx] = axis(0, Grid::SideX, Grid::ShiftX);
    const auto [cy, ty] = axis(1, Grid::SideY, Grid::ShiftY);
    const auto [cz, tz] = axis(2, Grid::SideZ, Grid::ShiftZ);
    const Id index = ctx.OpIAdd(
        ctx.U32[1],
        ctx.OpIMul(ctx.U32[1], ctx.OpIAdd(ctx.U32[1], ctx.OpIMul(ctx.U32[1], cy, u(Grid::SideZ)), cz),
                   u(Grid::SideX)),
        cx);
    const Id tags = ctx.OpBitwiseOr(
        ctx.U32[1], tx,
        ctx.OpBitwiseOr(ctx.U32[1], ctx.OpShiftLeftLogical(ctx.U32[1], ty, u(4u)),
                        ctx.OpShiftLeftLogical(ctx.U32[1], tz, u(8u))));
    return {index, ctx.OpShiftLeftLogical(ctx.U32[1], tags, u(Grid::StampBits))};
}

// The word of a cell's 4 x 4 x 4 block (light_shadow.h Grid::Coarse*): after the cells.
Id GridBlock(EmitContext& ctx, Id index) {
    namespace Grid = Tune::Grid;
    const auto u = [&](u32 v) { return ctx.ConstU32(v); };
    const Id u1 = ctx.U32[1];
    const Id cx = ctx.OpShiftRightLogical(u1, ctx.OpBitwiseAnd(u1, index, u(Grid::SideX - 1u)), u(Grid::CoarseShift));
    const Id cz = ctx.OpShiftRightLogical(u1, ctx.OpBitwiseAnd(u1, ctx.OpShiftRightLogical(u1, index, u(Grid::ShiftX)), u(Grid::SideZ - 1u)),
                                          u(Grid::CoarseShift));
    const Id cy = ctx.OpShiftRightLogical(u1, index, u(Grid::ShiftX + Grid::ShiftZ + Grid::CoarseShift));
    const Id block = ctx.OpIAdd(u1, ctx.OpIMul(u1, ctx.OpIAdd(u1, ctx.OpIMul(u1, cy, u(Grid::CoarseSideZ)), cz), u(Grid::CoarseSideX)), cx);
    return ctx.OpIAdd(u1, u(Grid::Voxels), block);
}

// A point of this eye's view: its voxel's word and its tag, in place (above the stamp's bits).
std::pair<Id, Id> GridCell(EmitContext& ctx, Id view_point) {
    return GridCellAt(ctx, GridWorld(ctx, view_point));
}

// A word's age in frames (its stamp's, modulo the stamps).
Id GridAge(EmitContext& ctx, Id word) {
    namespace Grid = Tune::Grid;
    const auto& s = ctx.vr_light_shadow;
    return ctx.OpBitwiseAnd(
        ctx.U32[1],
        ctx.OpISub(ctx.U32[1], s.grid_stamp,
                   ctx.OpBitwiseAnd(ctx.U32[1], word, ctx.ConstU32(Grid::StampMask))),
        ctx.ConstU32(Grid::StampMask));
}

// The word holds a blocker there: its tag `key`'s, a stamp, within the life.
Id GridSolid(EmitContext& ctx, Id word, Id key) {
    namespace Grid = Tune::Grid;
    const auto& s = ctx.vr_light_shadow;
    const Id tag_ok = ctx.OpIEqual(
        ctx.U1[1], ctx.OpBitwiseAnd(ctx.U32[1], word, ctx.ConstU32(~Grid::StampMask)), key);
    const Id stamped = ctx.OpINotEqual(
        ctx.U1[1], ctx.OpBitwiseAnd(ctx.U32[1], word, ctx.ConstU32(Grid::StampMask)),
        ctx.u32_zero_value);
    return ctx.OpLogicalAnd(ctx.U1[1], ctx.OpLogicalAnd(ctx.U1[1], tag_ok, stamped),
                            ctx.OpULessThan(ctx.U1[1], GridAge(ctx, word), s.grid_life));
}

// The blocker grid (light_shadow.h Grid), once, where Setup is: its header, then this pixel's
// share of the fill (its surface's voxel stamped, the air between it and the eye cleared).
void GridSetup(EmitContext& ctx, bool upkeep) {
    namespace Body = Shader::Optimization::BodyShadow;
    namespace Grid = Tune::Grid;
    auto& s = ctx.vr_light_shadow;
    if (s.grid_ready) {
        return;
    }
    s.grid_ready = true;
    if (!Grid::Enabled() || !Body::Enabled()) {
        return;
    }
    const auto& buffers = ctx.info.buffers;
    const auto body_it = std::ranges::find(buffers, BufferType::VrBodyCapsules, &BufferResource::buffer_type);
    const auto grid_it = std::ranges::find(buffers, BufferType::VrShadowGrid, &BufferResource::buffer_type);
    if (body_it == buffers.end() || grid_it == buffers.end()) {
        return;
    }
    const u32 body = static_cast<u32>(std::distance(buffers.begin(), body_it));
    s.grid_buffer = static_cast<u32>(std::distance(buffers.begin(), grid_it));
    const auto f = [&](float v) { return ctx.ConstF32(v); };
    const auto u = [&](u32 v) { return ctx.ConstU32(v); };
    const auto ru = [&](u32 d) { return BodyRead(ctx, body, Body::GridAt + d); };
    const auto rf = [&](u32 d) { return ctx.OpBitcast(ctx.F32[1], ru(d)); };
    const auto bf = [&](u32 d) { return ctx.OpBitcast(ctx.F32[1], BodyRead(ctx, body, d)); };
    s.grid_on = ctx.OpLogicalAnd(ctx.U1[1], ctx.OpINotEqual(ctx.U1[1], ru(0), ctx.u32_zero_value), s.ok);
    s.grid_stamp = ru(1);
    const Id voxel = ctx.OpFMax(ctx.F32[1], rf(2), f(1e-4f));
    s.grid_inv_voxel = ctx.OpFDiv(ctx.F32[1], f(1.0f), voxel);
    s.grid_life = ru(3);
    Id shift = f(0.0f);
    if (ctx.runtime_info.native_stereo && ctx.runtime_info.native_multiview) {
        const Id half = rf(4);
        shift = ctx.OpSelect(ctx.F32[1], ctx.OpIEqual(ctx.U1[1], ctx.NativeEyeIndex(), ctx.u32_zero_value),
                             ctx.OpFNegate(ctx.F32[1], half), half);
    }
    s.grid_shift = ctx.OpCompositeConstruct(ctx.F32[3], std::array{shift, f(0.0f), f(0.0f)});
    for (u32 r = 0; r < 3; ++r) {
        s.grid_rows[r] = ctx.OpCompositeConstruct(
            ctx.F32[4], std::array{rf(8 + 4 * r), rf(9 + 4 * r), rf(10 + 4 * r), rf(11 + 4 * r)});
    }
    s.grid_hit = Private(ctx, ctx.F32[1]);
    s.grid_thick = rf(7);
    // This pixel fills: one in FillEvery (by its place and the frame), from NearSkipM of the eye
    // to ReachM, not on his body (the capsules' sphere, header 4..7, while valid).
    const Id near_skip = rf(5);
    const Id reach = ctx.OpFMul(ctx.F32[1], voxel, f(Grid::ReachM / Grid::VoxelM));
    s.grid_near = near_skip;
    s.grid_reach = reach;
    if (!upkeep) {
        return; // (lookups only: the buffer is read-only here, light_shadow_pass.cpp)
    }
    const Id dist = ctx.OpLength(ctx.F32[1], s.p);
    const Id fx = ctx.OpConvertFToU(ctx.U32[1], EmitGetAttribute(ctx, IR::Attribute::FragCoord, 0, 0));
    const Id fy = ctx.OpConvertFToU(ctx.U32[1], EmitGetAttribute(ctx, IR::Attribute::FragCoord, 1, 0));
    const Id slot = ctx.OpBitwiseOr(ctx.U32[1], ctx.OpBitwiseAnd(ctx.U32[1], fx, u(1u)),
                                    ctx.OpShiftLeftLogical(ctx.U32[1], ctx.OpBitwiseAnd(ctx.U32[1], fy, u(1u)), u(1u)));
    const Id pick = ctx.OpIEqual(ctx.U1[1], slot,
                                 ctx.OpBitwiseAnd(ctx.U32[1], s.grid_stamp, u(Grid::FillEvery - 1u)));
    const Id centre = ctx.OpCompositeConstruct(
        ctx.F32[3], std::array{ctx.OpFSub(ctx.F32[1], bf(4), shift), bf(5), bf(6)});
    const Id on_body = ctx.OpLogicalAnd(
        ctx.U1[1], ctx.OpINotEqual(ctx.U1[1], BodyRead(ctx, body, 2u), ctx.u32_zero_value),
        ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpLength(ctx.F32[1], ctx.OpFSub(ctx.F32[3], s.p, centre)), bf(7)));
    // Nor an enemy's or NPC's (the other casters' spheres, the AO pass with the mask): they move,
    // so the grid would keep their ghosts; their shadows come from their capsules and the screen.
    Id on_caster = ctx.false_value;
    if (const auto casters_it = std::ranges::find(buffers, BufferType::VrShadowCasters, &BufferResource::buffer_type);
        casters_it != buffers.end() && Sirit::ValidId(s.scene_count)) {
        namespace Scene = Tune::SceneMaps;
        const u32 cbuf = static_cast<u32>(std::distance(buffers.begin(), casters_it));
        const Id wp = ToWorld(ctx, ctx.OpFAdd(ctx.F32[3], s.p, s.scene_shift));
        const Id inside = Private(ctx, ctx.U1[1]);
        ctx.OpStore(inside, ctx.false_value);
        LoopCount(ctx, ctx.OpUMin(ctx.U32[1], BodyRead(ctx, cbuf, 1u), u(Scene::CasterBodies)), [&](Id k) {
            const Id at = ctx.OpIAdd(ctx.U32[1], u(Scene::CasterBodiesAt), ctx.OpIMul(ctx.U32[1], k, u(8u)));
            const auto rf2 = [&](u32 c) { return ctx.OpBitcast(ctx.F32[1], BodyReadAt(ctx, cbuf, ctx.OpIAdd(ctx.U32[1], at, u(c)))); };
            const Id c = ctx.OpCompositeConstruct(ctx.F32[3], std::array{rf2(0), rf2(1), rf2(2)});
            const Id hit = ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpLength(ctx.F32[1], ctx.OpFSub(ctx.F32[3], wp, c)),
                                              ctx.OpFMul(ctx.F32[1], rf2(3), f(1.1f)));
            ctx.OpStore(inside, ctx.OpLogicalOr(ctx.U1[1], ctx.OpLoad(ctx.U1[1], inside), hit));
        });
        on_caster = ctx.OpLoad(ctx.U1[1], inside);
    }
    const Id fill = ctx.OpLogicalAnd(
        ctx.U1[1], ctx.OpLogicalAnd(ctx.U1[1], Grid::FillOn() ? s.grid_on : ctx.false_value, pick),
        ctx.OpLogicalAnd(ctx.U1[1], ctx.OpLogicalNot(ctx.U1[1], ctx.OpLogicalOr(ctx.U1[1], on_body, on_caster)),
                         ctx.OpLogicalAnd(ctx.U1[1], ctx.OpFOrdGreaterThan(ctx.U1[1], dist, near_skip),
                                          ctx.OpFOrdLessThan(ctx.U1[1], dist, reach))));
    const Id fill_label = ctx.OpLabel(), fill_done = ctx.OpLabel();
    ctx.OpSelectionMerge(fill_done, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(fill, fill_label, fill_done);
    ctx.AddLabel(fill_label);
    {
        const auto [index, key] = GridCell(ctx, s.p);
        ctx.OpStore(GridPtr(ctx, index), ctx.OpBitwiseOr(ctx.U32[1], key, s.grid_stamp));
        if (Tune::SceneMaps::Opt(128)) {
            // Its block's stamp (light_shadow.h Grid::Coarse*: every cell's fill stamps it).
            ctx.OpStore(GridPtr(ctx, GridBlock(ctx, index)), s.grid_stamp);
        }
        // The air between the eye and it: CarvePoints points from NearSkipM to a voxel and a
        // half short of the surface, each cleared unless stamped within CarveAfter frames (a thin
        // thing beside the ray, stamped by another pixel, stays).
        const Id span = ctx.OpFSub(ctx.F32[1], ctx.OpFSub(ctx.F32[1], dist, ctx.OpFMul(ctx.F32[1], voxel, f(1.5f))),
                                   near_skip);
        const Id scope = u(static_cast<u32>(spv::Scope::Device));
        for (u32 k = 0; k < Grid::CarvePoints; ++k) {
            const Id frac = ctx.OpFMul(ctx.F32[1], ctx.OpFAdd(ctx.F32[1], f(float(k)), s.seed),
                                       f(1.0f / float(Grid::CarvePoints)));
            const Id t = ctx.OpFAdd(ctx.F32[1], near_skip, ctx.OpFMul(ctx.F32[1], span, frac));
            const Id point = ctx.OpVectorTimesScalar(ctx.F32[3], s.p,
                                                     ctx.OpFDiv(ctx.F32[1], t, ctx.OpFMax(ctx.F32[1], dist, f(1e-4f))));
            const auto [ci, ck] = GridCell(ctx, point);
            const Id ptr = GridPtr(ctx, ci);
            const Id old = ctx.OpLoad(ctx.U32[1], ptr);
            const Id clear = ctx.OpLogicalAnd(
                ctx.U1[1], ctx.OpFOrdGreaterThan(ctx.U1[1], span, f(0.0f)),
                ctx.OpLogicalAnd(ctx.U1[1], ctx.OpINotEqual(ctx.U1[1], old, ctx.u32_zero_value),
                                 ctx.OpUGreaterThan(ctx.U1[1], GridAge(ctx, old), u(Grid::CarveAfter))));
            const Id carve_label = ctx.OpLabel(), carve_done = ctx.OpLabel();
            ctx.OpSelectionMerge(carve_done, spv::SelectionControlMask::MaskNone);
            ctx.OpBranchConditional(clear, carve_label, carve_done);
            ctx.AddLabel(carve_label);
            ctx.OpAtomicCompareExchange(ctx.U32[1], ptr, scope, ctx.u32_zero_value, ctx.u32_zero_value,
                                        ctx.u32_zero_value, old);
            ctx.OpBranch(carve_done);
            ctx.AddLabel(carve_done);
        }
    }
    ctx.OpBranch(fill_done);
    ctx.AddLabel(fill_done);
}

// The march from P towards light (view space) `l`, `dist` away, over [StartM, end]. `delta`:
// this eye's view to the other eye's (light_shadow.h CrossEye; invalid: this eye only).
// `adaptive` (the mask, light_shadow.h SceneMaps::MarchSmart): the smart steps.
Id March(EmitContext& ctx, Id l, Id dist, Id end, Id count, Id delta = Id{}, bool adaptive = false) {
    const auto& s = ctx.vr_light_shadow;
    const auto& info = ctx.info.vr_light_shadow;
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(ctx.F32[1], a, b); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(ctx.F32[1], a, b); };
    // (VR_MARCH_OPT, light_shadow.h SceneMaps::Opt: 2 the step's divides, 4 the other eye only near
    // this picture's edge, 8 the grid's lookups in its own space.)
    const bool opt_step = Tune::SceneMaps::Opt(2);
    const bool opt_other = Tune::SceneMaps::Opt(4);
    const bool opt_grid = Tune::SceneMaps::Opt(8);
    // The nearest-depth pyramid (light_shadow.h SceneMaps::Pyramid): the mask pass's march only
    // (its buffer), the even steps (not the smart ones).
    const bool use_pyramid = Tune::SceneMaps::Pyramid() && s.pyr_buffer != ~0u && !adaptive;
    const Id dir = ctx.OpVectorTimesScalar(ctx.F32[3], ctx.OpFSub(ctx.F32[3], l, s.p),
                                           ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), dist));
    const Id span = ctx.OpFSub(ctx.F32[1], end, ctx.ConstF32(Tune::StartM));
    // The ray is a line: its clip coordinates are cp + t cd (x, y, w; the inverse's rows on
    // P and on P + dir, once), and its length from the eye sqrt(pp + 2 t pd + t^2). (2026-11-10:
    // the same result as projecting each point, with a third of the work per step.)
    const auto px = ctx.OpCompositeExtract(ctx.F32[1], s.p, 0u);
    const auto py = ctx.OpCompositeExtract(ctx.F32[1], s.p, 1u);
    const auto pz = ctx.OpCompositeExtract(ctx.F32[1], s.p, 2u);
    const Id tip = ctx.OpFAdd(ctx.F32[3], s.p, dir);
    const auto cp = Transform(ctx, s.inv, px, py, pz);
    const auto ct = Transform(ctx, s.inv, ctx.OpCompositeExtract(ctx.F32[1], tip, 0u),
                              ctx.OpCompositeExtract(ctx.F32[1], tip, 1u),
                              ctx.OpCompositeExtract(ctx.F32[1], tip, 2u));
    std::array<Id, 4> cd;
    for (u32 k = 0; k < 4; ++k) {
        cd[k] = ctx.OpFSub(ctx.F32[1], ct[k], cp[k]);
    }
    const Id pp = ctx.OpDot(ctx.F32[1], s.p, s.p);
    // The bias's share of the pixel's own distance (Tune::BiasPerEyeM).
    const Id eye_bias = Tune::OldShadows()
                            ? ctx.ConstF32(0.0f)
                            : mul(ctx.OpSqrt(ctx.F32[1], pp), ctx.ConstF32(Tune::BiasPerEyeM));
    const Id pd2 = mul(ctx.ConstF32(2.0f), ctx.OpDot(ctx.F32[1], s.p, dir));
    const Id dz = ctx.OpCompositeExtract(ctx.F32[1], dir, 2u);
    // The same ray in the other eye's view (P + delta): its clip line and its lengths.
    const bool cross = s.other_ready && Sirit::ValidId(delta);
    std::array<Id, 4> cp2{}, cd2{};
    Id pp2{}, pdd2{}, pz2{};
    if (cross) {
        const Id p2 = ctx.OpFAdd(ctx.F32[3], s.p, delta);
        const Id tip2 = ctx.OpFAdd(ctx.F32[3], p2, dir);
        const auto x = [&](Id v, u32 k) { return ctx.OpCompositeExtract(ctx.F32[1], v, k); };
        cp2 = Transform(ctx, s.other_inv, x(p2, 0), x(p2, 1), x(p2, 2));
        const auto ct2 = Transform(ctx, s.other_inv, x(tip2, 0), x(tip2, 1), x(tip2, 2));
        for (u32 k = 0; k < 4; ++k) {
            cd2[k] = ctx.OpFSub(ctx.F32[1], ct2[k], cp2[k]);
        }
        pp2 = ctx.OpDot(ctx.F32[1], p2, p2);
        pdd2 = mul(ctx.ConstF32(2.0f), ctx.OpDot(ctx.F32[1], p2, dir));
        pz2 = x(p2, 2);
    }
    // The smart steps (light_shadow.h SceneMaps::MarchSmart): the step's place carried from one
    // to the next, the first jittered within the finest step (count's quarter of the span, at
    // least SmartMinStepM); count is then the most steps the ray may take.
    Id t_var{}, t_prev_var{}, min_step{};
    if (adaptive) {
        min_step = ctx.OpFMax(
            ctx.F32[1], ctx.ConstF32(Tune::SceneMaps::SmartMinStepM),
            ctx.OpFDiv(ctx.F32[1], span,
                       mul(ctx.OpConvertUToF(ctx.F32[1], ctx.OpUMax(ctx.U32[1], count, ctx.ConstU32(1u))), ctx.ConstF32(4.0f))));
        t_var = Private(ctx, ctx.F32[1]);
        t_prev_var = Private(ctx, ctx.F32[1]);
        ctx.OpStore(t_var, add(ctx.ConstF32(Tune::StartM), mul(s.seed, min_step)));
        ctx.OpStore(t_prev_var, ctx.ConstF32(0.0f));
    }
    ctx.OpStore(s.occl, ctx.ConstF32(0.0f));
    ctx.OpStore(s.step, ctx.u32_zero_value);
    // What every step shares, once before the loop: the steps' spacing (Opt 2), the blockers' base
    // thickness, the grid's voxel and the ray's slope off the surface, the ray in the grid's space
    // (Opt 8: a step's place there is w0 + wd t).
    const Id inv_count = opt_step ? ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), ctx.OpConvertUToF(ctx.F32[1], count))
                                  : Id{};
    // Taken to be ThickM + ThickPerM t thick: gap in [Bias, thick] is behind something.
    // (The header's thickness when set: the live key lighting.shadow_thick_cm, body_shadow.h.)
    Id thick_base = ctx.ConstF32(Tune::OldShadows() ? Tune::ThickMOld : Tune::ThickM);
    if (Sirit::ValidId(s.grid_thick)) {
        thick_base = ctx.OpSelect(ctx.F32[1],
                                  ctx.OpFOrdGreaterThan(ctx.U1[1], s.grid_thick, ctx.ConstF32(0.0f)),
                                  s.grid_thick, thick_base);
    }
    // VR_LIGHT_THICK_PER=<m a m> (tests): the thickness's growth along the ray.
    static const float thick_per = [] {
        const char* const env = std::getenv("VR_LIGHT_THICK_PER");
        return env != nullptr ? std::clamp(float(std::atof(env)), 0.0f, 1.0f) : Tune::ThickPerM;
    }();
    const bool grid_look = Sirit::ValidId(s.grid_on) && Tune::Grid::LookOn();
    Id voxel{}, rise_per_m{}, grid_w0{}, grid_wd{};
    if (grid_look) {
        voxel = ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), s.grid_inv_voxel);
        rise_per_m = ctx.OpFAbs(ctx.F32[1], ctx.OpDot(ctx.F32[1], dir, s.normal));
        if (opt_grid) {
            grid_w0 = GridWorld(ctx, s.p);
            const auto dx = [&](u32 k) { return ctx.OpCompositeExtract(ctx.F32[1], dir, k); };
            const Id d4 = ctx.OpCompositeConstruct(ctx.F32[4], std::array{dx(0), dx(1), dx(2), ctx.ConstF32(0.0f)});
            grid_wd = ctx.OpCompositeConstruct(ctx.F32[3], std::array{ctx.OpDot(ctx.F32[1], s.grid_rows[0], d4),
                                                                      ctx.OpDot(ctx.F32[1], s.grid_rows[1], d4),
                                                                      ctx.OpDot(ctx.F32[1], s.grid_rows[2], d4)});
        }
    }
    // The pyramid's header (its level-0 size: 0 for the stand-in, no pyramid this frame), this
    // eye's planes, which raw depth is the nearer (the projection's direction, from this pixel's
    // ndc: the view z of 0 and of 1), and the first stretch's start (the march's first point).
    Id pyr_ok{}, pyr_w0{}, pyr_h0{}, size0{}, pyr_eye{}, near_min{};
    Id v_uv_prev{}, v_qz_prev{}, v_in_prev{}, v_t_prev{};
    const auto view_z = [&](Id nd, Id d) {
        const Id v4 = ctx.OpCompositeConstruct(ctx.F32[4], std::array{ctx.OpCompositeExtract(ctx.F32[1], nd, 0u),
                                                                      ctx.OpCompositeExtract(ctx.F32[1], nd, 1u), d,
                                                                      ctx.ConstF32(1.0f)});
        return ctx.OpFAbs(ctx.F32[1], ctx.OpFDiv(ctx.F32[1], ctx.OpDot(ctx.F32[1], s.rows[2], v4),
                                                 ctx.OpDot(ctx.F32[1], s.rows[3], v4)));
    };
    if (use_pyramid) {
        pyr_w0 = BodyRead(ctx, s.pyr_buffer, 0u);
        pyr_h0 = BodyRead(ctx, s.pyr_buffer, 1u);
        pyr_ok = ctx.OpLogicalAnd(ctx.U1[1], ctx.OpINotEqual(ctx.U1[1], pyr_w0, ctx.u32_zero_value),
                                  ctx.OpINotEqual(ctx.U1[1], pyr_h0, ctx.u32_zero_value));
        size0 = ctx.OpCompositeConstruct(ctx.F32[2], std::array{ctx.OpConvertUToF(ctx.F32[1], pyr_w0),
                                                                ctx.OpConvertUToF(ctx.F32[1], pyr_h0)});
        pyr_eye = ctx.runtime_info.native_stereo && ctx.runtime_info.native_multiview ? ctx.NativeEyeIndex()
                                                                                       : ctx.u32_zero_value;
        const Id cw_here = ctx.OpFMax(ctx.F32[1], cp[3], ctx.ConstF32(1e-6f));
        const Id n_here = ctx.OpCompositeConstruct(ctx.F32[2], std::array{ctx.OpFDiv(ctx.F32[1], cp[0], cw_here),
                                                                          ctx.OpFDiv(ctx.F32[1], cp[1], cw_here)});
        near_min = ctx.OpFOrdLessThan(ctx.U1[1], view_z(n_here, ctx.ConstF32(0.0f)), view_z(n_here, ctx.ConstF32(1.0f)));
        v_uv_prev = Private(ctx, ctx.F32[2]);
        v_qz_prev = Private(ctx, ctx.F32[1]);
        v_in_prev = Private(ctx, ctx.U1[1]);
        v_t_prev = Private(ctx, ctx.F32[1]);
        const Id t0 = ctx.ConstF32(Tune::StartM);
        const Id cw0 = ctx.OpFma(ctx.F32[1], cd[3], t0, cp[3]);
        const Id inv0 = ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), ctx.OpFMax(ctx.F32[1], cw0, ctx.ConstF32(1e-6f)));
        const Id n0 = ctx.OpCompositeConstruct(ctx.F32[2], std::array{mul(ctx.OpFma(ctx.F32[1], cd[0], t0, cp[0]), inv0),
                                                                      mul(ctx.OpFma(ctx.F32[1], cd[1], t0, cp[1]), inv0)});
        const Id uv0 = ctx.OpFAdd(ctx.F32[2], ctx.OpFMul(ctx.F32[2], s.a, n0), s.b);
        const Id in0 = ctx.OpLogicalAnd(
            ctx.U1[1], ctx.OpFOrdGreaterThan(ctx.U1[1], cw0, ctx.ConstF32(1e-6f)),
            ctx.OpLogicalAnd(ctx.U1[1], ctx.OpAll(ctx.U1[1], ctx.OpFOrdGreaterThanEqual(ctx.U1[2], uv0, ctx.ConstF32(0.0f, 0.0f))),
                             ctx.OpAll(ctx.U1[1], ctx.OpFOrdLessThanEqual(ctx.U1[2], uv0, ctx.ConstF32(1.0f, 1.0f)))));
        ctx.OpStore(v_uv_prev, uv0);
        ctx.OpStore(v_qz_prev, ctx.OpFma(ctx.F32[1], dz, t0, pz));
        ctx.OpStore(v_in_prev, in0);
        ctx.OpStore(v_t_prev, t0);
    }
    const Id header = ctx.OpLabel(), body = ctx.OpLabel(), cont = ctx.OpLabel(),
             merge = ctx.OpLabel();
    ctx.OpBranch(header);
    ctx.AddLabel(header);
    const Id i = ctx.OpLoad(ctx.U32[1], s.step);
    const Id o = ctx.OpLoad(ctx.F32[1], s.occl);
    Id go = ctx.OpLogicalAnd(ctx.U1[1], ctx.OpULessThan(ctx.U1[1], i, count),
                             ctx.OpFOrdLessThan(ctx.U1[1], o, ctx.ConstF32(0.99f)));
    if (adaptive) {
        // (... and short of the march's end.)
        go = ctx.OpLogicalAnd(ctx.U1[1], go, ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpLoad(ctx.F32[1], t_var), end));
    }
    ctx.OpLoopMerge(merge, cont, spv::LoopControlMask::MaskNone);
    ctx.OpBranchConditional(go, body, merge);
    ctx.AddLabel(body);
    // t = StartM + span * ((i + jitter) / n)^2: denser near the surface.
    const Id i_f = add(ctx.OpConvertUToF(ctx.F32[1], i), s.seed);
    const Id u = opt_step ? mul(i_f, inv_count) : ctx.OpFDiv(ctx.F32[1], i_f, ctx.OpConvertUToF(ctx.F32[1], count));
    const Id t = adaptive ? ctx.OpLoad(ctx.F32[1], t_var) : add(ctx.ConstF32(Tune::StartM), mul(span, mul(u, u)));
    const auto at = [&](u32 k) { return ctx.OpFma(ctx.F32[1], cd[k], t, cp[k]); };
    const Id cw = at(3);
    const Id valid = ctx.OpFOrdGreaterThan(ctx.U1[1], cw, ctx.ConstF32(1e-6f));
    const Id inv_cw = ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f),
                                 ctx.OpFMax(ctx.F32[1], cw, ctx.ConstF32(1e-6f)));
    const Id n = ctx.OpCompositeConstruct(ctx.F32[2], std::array{mul(at(0), inv_cw), mul(at(1), inv_cw)});
    // The point's distance from the eye and its view z.
    const Id q_len = ctx.OpSqrt(ctx.F32[1],
                                ctx.OpFMax(ctx.F32[1], add(pp, mul(t, add(pd2, t))),
                                           ctx.ConstF32(0.0f)));
    const Id qz = ctx.OpFma(ctx.F32[1], dz, t, pz);
    const Id uv = ctx.OpFAdd(ctx.F32[2], ctx.OpFMul(ctx.F32[2], s.a, n), s.b);
    const Id zero2 = ctx.ConstF32(0.0f, 0.0f), one2 = ctx.ConstF32(1.0f, 1.0f);
    const Id inside = ctx.OpLogicalAnd(
        ctx.U1[1], valid,
        ctx.OpLogicalAnd(ctx.U1[1], ctx.OpAll(ctx.U1[1], ctx.OpFOrdGreaterThanEqual(ctx.U1[2], uv, zero2)),
                         ctx.OpAll(ctx.U1[1], ctx.OpFOrdLessThanEqual(ctx.U1[2], uv, one2))));
    // The step's distance from the picture's nearest edge (0 off it).
    const auto margin_of = [&](Id c) {
        const Id m2 = ctx.OpFMin(ctx.F32[2], c, ctx.OpFSub(ctx.F32[2], one2, c));
        return ctx.OpFMin(ctx.F32[1], ctx.OpCompositeExtract(ctx.F32[1], m2, 0u),
                          ctx.OpCompositeExtract(ctx.F32[1], m2, 1u));
    };
    Id uvc = ctx.OpFClamp(ctx.F32[2], uv, zero2, one2);
    Id margin = ctx.OpSelect(ctx.F32[1], inside, margin_of(uvc), ctx.ConstF32(0.0f));
    // Off this eye's picture, on the other's: the other eye's depth there (CrossEye).
    Id seen = inside, n_at = n, q_at = q_len, qz_at = qz, row2 = s.rows[2], row3 = s.rows[3];
    if (cross) {
        Id eye_at = s.this_eye;
        const auto other_eye = [&] {
            const auto at2 = [&](u32 k) { return ctx.OpFma(ctx.F32[1], cd2[k], t, cp2[k]); };
            const Id cw2 = at2(3);
            const Id inv_cw2 = ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f),
                                          ctx.OpFMax(ctx.F32[1], cw2, ctx.ConstF32(1e-6f)));
            const Id n2 = ctx.OpCompositeConstruct(
                ctx.F32[2], std::array{mul(at2(0), inv_cw2), mul(at2(1), inv_cw2)});
            const Id uv2 = ctx.OpFAdd(ctx.F32[2], ctx.OpFMul(ctx.F32[2], s.a, n2), s.b);
            const Id inside2 = ctx.OpLogicalAnd(
                ctx.U1[1],
                ctx.OpLogicalAnd(ctx.U1[1], s.other_differs,
                                 ctx.OpFOrdGreaterThan(ctx.U1[1], cw2, ctx.ConstF32(1e-6f))),
                ctx.OpLogicalAnd(ctx.U1[1],
                                 ctx.OpAll(ctx.U1[1], ctx.OpFOrdGreaterThanEqual(ctx.U1[2], uv2, zero2)),
                                 ctx.OpAll(ctx.U1[1], ctx.OpFOrdLessThanEqual(ctx.U1[2], uv2, one2))));
            const Id uvc2 = ctx.OpFClamp(ctx.F32[2], uv2, zero2, one2);
            const Id use_other = ctx.OpLogicalAnd(ctx.U1[1], ctx.OpLogicalNot(ctx.U1[1], inside), inside2);
            seen = ctx.OpLogicalOr(ctx.U1[1], inside, inside2);
            margin = ctx.OpFMax(ctx.F32[1], margin,
                                ctx.OpSelect(ctx.F32[1], inside2, margin_of(uvc2), ctx.ConstF32(0.0f)));
            uvc = ctx.OpSelect(ctx.F32[2], use_other, uvc2, uvc);
            n_at = ctx.OpSelect(ctx.F32[2], use_other, n2, n);
            q_at = ctx.OpSelect(ctx.F32[1], use_other,
                                ctx.OpSqrt(ctx.F32[1], ctx.OpFMax(ctx.F32[1], add(pp2, mul(t, add(pdd2, t))),
                                                                  ctx.ConstF32(0.0f))),
                                q_len);
            qz_at = ctx.OpSelect(ctx.F32[1], use_other, ctx.OpFma(ctx.F32[1], dz, t, pz2), qz);
            row2 = ctx.OpSelect(ctx.F32[4], use_other, s.other_row2, s.rows[2]);
            row3 = ctx.OpSelect(ctx.F32[4], use_other, s.other_row3, s.rows[3]);
            eye_at = ctx.OpSelect(ctx.U32[1], use_other, s.other_eye, s.this_eye);
        };
        if (opt_other) {
            // (Opt 4: only where the step is off this eye's picture or within its edge band. Well
            // inside it the other eye changes nothing: the step is seen, the picture this eye's,
            // and the edge fade is full whatever the other's margin.)
            const Id f2 = ctx.F32[2], f4 = ctx.F32[4];
            const Id v_seen = Private(ctx, ctx.U1[1]), v_margin = Private(ctx, ctx.F32[1]),
                     v_uvc = Private(ctx, f2), v_n = Private(ctx, f2), v_q = Private(ctx, ctx.F32[1]),
                     v_qz = Private(ctx, ctx.F32[1]), v_row2 = Private(ctx, f4), v_row3 = Private(ctx, f4),
                     v_eye = Private(ctx, ctx.U32[1]);
            const auto put = [&] {
                ctx.OpStore(v_seen, seen);
                ctx.OpStore(v_margin, margin);
                ctx.OpStore(v_uvc, uvc);
                ctx.OpStore(v_n, n_at);
                ctx.OpStore(v_q, q_at);
                ctx.OpStore(v_qz, qz_at);
                ctx.OpStore(v_row2, row2);
                ctx.OpStore(v_row3, row3);
                ctx.OpStore(v_eye, eye_at);
            };
            put();
            const Id need = Tune::OldShadows()
                                ? ctx.OpLogicalNot(ctx.U1[1], inside)
                                : ctx.OpLogicalOr(ctx.U1[1], ctx.OpLogicalNot(ctx.U1[1], inside),
                                                  ctx.OpFOrdLessThan(ctx.U1[1], margin, ctx.ConstF32(Tune::EdgeBand)));
            const Id other_l = ctx.OpLabel(), other_done = ctx.OpLabel();
            ctx.OpSelectionMerge(other_done, spv::SelectionControlMask::MaskNone);
            ctx.OpBranchConditional(need, other_l, other_done);
            ctx.AddLabel(other_l);
            other_eye();
            put();
            ctx.OpBranch(other_done);
            ctx.AddLabel(other_done);
            seen = ctx.OpLoad(ctx.U1[1], v_seen);
            margin = ctx.OpLoad(ctx.F32[1], v_margin);
            uvc = ctx.OpLoad(f2, v_uvc);
            n_at = ctx.OpLoad(f2, v_n);
            q_at = ctx.OpLoad(ctx.F32[1], v_q);
            qz_at = ctx.OpLoad(ctx.F32[1], v_qz);
            row2 = ctx.OpLoad(f4, v_row2);
            row3 = ctx.OpLoad(f4, v_row3);
            eye_at = ctx.OpLoad(ctx.U32[1], v_eye);
        } else {
            other_eye();
        }
        ctx.eye_override = eye_at;
    }
    // The point test at `t_at` (its picture coordinates, view place and rows: this eye's, or the
    // other's with CrossEye; the sample's eye is ctx.eye_override): the hit, behind the surface
    // there from the bias to the blocker's thickness (soft at both ends), and the gap behind it.
    const auto bias_at = [&](Id t_at) {
        return add(add(ctx.ConstF32(Tune::BiasM), eye_bias), mul(t_at, ctx.ConstF32(Tune::BiasPerM)));
    };
    const auto point_test = [&](Id t_at, Id uvc_at, Id n_p, Id q_p, Id qz_p, Id row2_p, Id row3_p,
                                bool thin = false, Id raw = Id{}) {
        // (`raw`: a depth from the pyramid in place of the picture's sample here.)
        const Id ds = Sirit::ValidId(raw)
                          ? raw
                          : ctx.OpCompositeExtract(ctx.F32[1], EmitImageSampleExplicitLod(ctx, nullptr, info.depth_image, uvc_at,
                                                                                          ctx.ConstF32(0.0f), IR::Value{}),
                                                   0u);
        // The surface there lies on the same ray from the eye as the point: its distance is the
        // point's scaled by their view z (only the M rows giving z and w).
        const Id s4v = ctx.OpCompositeConstruct(
            ctx.F32[4], std::array{ctx.OpCompositeExtract(ctx.F32[1], n_p, 0u),
                                   ctx.OpCompositeExtract(ctx.F32[1], n_p, 1u), ds, ctx.ConstF32(1.0f)});
        const Id qz_safe = ctx.OpSelect(ctx.F32[1],
                                        ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpFAbs(ctx.F32[1], qz_p), ctx.ConstF32(1e-6f)),
                                        ctx.ConstF32(1e-6f), qz_p);
        // (Opt 2: one divide, z over w and over the point's z at once.)
        const Id ratio = opt_step
                             ? ctx.OpFDiv(ctx.F32[1], ctx.OpDot(ctx.F32[1], row2_p, s4v),
                                          mul(ctx.OpDot(ctx.F32[1], row3_p, s4v), qz_safe))
                             : ctx.OpFDiv(ctx.F32[1],
                                          ctx.OpFDiv(ctx.F32[1], ctx.OpDot(ctx.F32[1], row2_p, s4v),
                                                     ctx.OpDot(ctx.F32[1], row3_p, s4v)),
                                          qz_safe);
        const Id gap_p = mul(q_p, ctx.OpFSub(ctx.F32[1], ctx.ConstF32(1.0f), ratio));
        // (`thin`: the pyramid's dense reads, light_shadow.h SceneMaps::PyramidThick.)
        const Id thick = thin ? add(ctx.ConstF32(Tune::SceneMaps::PyramidThickM()),
                                    mul(t_at, ctx.ConstF32(Tune::SceneMaps::PyramidThickPerM)))
                              : add(thick_base, mul(t_at, ctx.ConstF32(thick_per)));
        // A hit from the bias (BiasM, growing BiasPerM a metre along the ray and BiasPerEyeM a
        // metre of the pixel's distance) to twice it.
        const Id bias_p = bias_at(t_at);
        const Id band = mul(thick, ctx.ConstF32(Tune::OldShadows() ? Tune::HitBandOld : Tune::HitBand));
        Id fade{}, rise{};
        if (opt_step) {
            // (Opt 2: one reciprocal for both fades: band and bias are never 0.)
            const Id inv_both = ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), mul(band, bias_p));
            fade = ctx.OpFClamp(ctx.F32[1], mul(mul(ctx.OpFSub(ctx.F32[1], thick, gap_p), bias_p), inv_both),
                                ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
            rise = ctx.OpFClamp(ctx.F32[1], mul(mul(ctx.OpFSub(ctx.F32[1], gap_p, bias_p), band), inv_both),
                                ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
        } else {
            fade = ctx.OpFClamp(ctx.F32[1], ctx.OpFDiv(ctx.F32[1], ctx.OpFSub(ctx.F32[1], thick, gap_p), band),
                                ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
            rise = ctx.OpFClamp(ctx.F32[1], ctx.OpFDiv(ctx.F32[1], ctx.OpFSub(ctx.F32[1], gap_p, bias_p), bias_p),
                                ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
        }
        const Id hit_p = mul(mul(mul(rise, rise), ctx.OpFSub(ctx.F32[1], ctx.ConstF32(3.0f),
                                                             mul(ctx.ConstF32(2.0f), rise))),
                             fade);
        return std::pair{hit_p, gap_p};
    };
    // ... and less towards the picture's edge (light_shadow.h EdgeBand; with CrossEye the edge
    // of both eyes' pictures: the margin is the larger of the two).
    const Id edge = Tune::OldShadows()
                        ? ctx.ConstF32(1.0f)
                        : ctx.OpFClamp(ctx.F32[1], mul(margin, ctx.ConstF32(1.0f / Tune::EdgeBand)),
                                       ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
    const Id bias = bias_at(t);
    Id hit{}, gap{};
    if (use_pyramid) {
        namespace Scene = Tune::SceneMaps;
        const Id f1 = ctx.F32[1], f2 = ctx.F32[2], u1 = ctx.U32[1], b1 = ctx.U1[1];
        const auto uconst = [&](u32 v) { return ctx.ConstU32(v); };
        const Id v_hit = Private(ctx, f1), v_gap = Private(ctx, f1);
        const Id uv_prev = ctx.OpLoad(f2, v_uv_prev);
        // The stretch from the last step to this one on this eye's picture, in level-0 texels.
        const Id stretch = ctx.OpLength(f1, ctx.OpFMul(f2, ctx.OpFSub(f2, uv, uv_prev), size0));
        // On this eye's picture at both ends (the pyramid this frame's), longer than a texel and past
        // PyramidStartM of the receiver: the nearest depth of the blocks covering the stretch; else
        // the point test as before (the other eye's picture, the grid).
        const Id whole = ctx.OpLogicalAnd(
            b1, ctx.OpLogicalAnd(b1, ctx.OpLogicalAnd(b1, pyr_ok, inside), ctx.OpLoad(b1, v_in_prev)),
            ctx.OpLogicalAnd(b1, ctx.OpFOrdGreaterThan(b1, stretch, ctx.ConstF32(1.0f)),
                             ctx.OpFOrdGreaterThan(b1, t, ctx.ConstF32(Scene::PyramidStartM))));
        const Id saved_override = ctx.eye_override;
        const Id whole_l = ctx.OpLabel(), point_l = ctx.OpLabel(), screen_done = ctx.OpLabel();
        ctx.OpSelectionMerge(screen_done, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(whole, whole_l, point_l);
        ctx.AddLabel(whole_l);
        {
            ctx.eye_override = Id{}; // this eye's picture (no sample: the pyramid)
            // The level whose blocks are at least the stretch long; the blocks at its two ends
            // (they hold the stretch but where it cuts a block's corner).
            const Id kf = ctx.OpFClamp(f1, ctx.OpCeil(f1, ctx.OpLog2(f1, stretch)), ctx.ConstF32(1.0f),
                                       ctx.ConstF32(float(Scene::PyramidLevels)));
            const Id k = ctx.OpConvertFToU(u1, kf);
            const Id round = ctx.OpISub(u1, ctx.OpShiftLeftLogical(u1, uconst(1u), k), uconst(1u));
            const Id wk = ctx.OpUMax(u1, ctx.OpShiftRightLogical(u1, ctx.OpIAdd(u1, pyr_w0, round), k), uconst(1u));
            const Id hk = ctx.OpUMax(u1, ctx.OpShiftRightLogical(u1, ctx.OpIAdd(u1, pyr_h0, round), k), uconst(1u));
            const Id per_k = ctx.OpFMul(f2, size0, ctx.OpExp2(f2, ctx.OpCompositeConstruct(f2, std::array{ctx.OpFNegate(f1, kf), ctx.OpFNegate(f1, kf)})));
            const auto texel_at = [&](Id c, u32 axis, Id last) {
                const Id v = ctx.OpConvertFToS(ctx.S32[1], ctx.OpFloor(f1, ctx.OpCompositeExtract(f1, ctx.OpFMul(f2, c, per_k), axis)));
                return ctx.OpBitcast(u1, ctx.OpSClamp(ctx.S32[1], v, ctx.ConstS32(0), ctx.OpBitcast(ctx.S32[1], ctx.OpISub(u1, last, uconst(1u)))));
            };
            const Id level_first = BodyReadAt(ctx, s.pyr_buffer, ctx.OpIAdd(u1, uconst(3u), k));
            const Id plane_first = ctx.OpIAdd(u1, level_first, ctx.OpIMul(u1, pyr_eye, ctx.OpIMul(u1, wk, hk)));
            const Id word_of = ctx.OpSelect(u1, near_min, uconst(0u), uconst(1u));
            const auto nearer_at = [&](Id c) {
                const Id texel = ctx.OpIAdd(u1, plane_first, ctx.OpIAdd(u1, ctx.OpIMul(u1, texel_at(c, 1u, hk), wk), texel_at(c, 0u, wk)));
                return ctx.OpBitcast(f1, BodyReadAt(ctx, s.pyr_buffer,
                                                    ctx.OpIAdd(u1, uconst(Scene::PyramidHeaderWords),
                                                               ctx.OpIAdd(u1, ctx.OpIMul(u1, texel, uconst(2u)), word_of))));
            };
            const Id raw_a = nearer_at(uv_prev), raw_b = nearer_at(uvc);
            const Id nearest = ctx.OpSelect(f1, near_min, ctx.OpFMin(f1, raw_a, raw_b), ctx.OpFMax(f1, raw_a, raw_b));
            const auto [hit_f, gap_f] = point_test(t, uvc, n, q_len, qz, s.rows[2], s.rows[3], false, nearest);
            ctx.OpStore(v_hit, hit_f);
            ctx.OpStore(v_gap, gap_f);
        }
        ctx.OpBranch(screen_done);
        ctx.AddLabel(point_l);
        {
            ctx.eye_override = saved_override;
            const auto [hit_p, gap_p] = point_test(t, uvc, n_at, q_at, qz_at, row2, row3);
            ctx.OpStore(v_hit, hit_p);
            ctx.OpStore(v_gap, gap_p);
        }
        ctx.OpBranch(screen_done);
        ctx.AddLabel(screen_done);
        ctx.eye_override = Id{};
        hit = ctx.OpLoad(f1, v_hit);
        gap = ctx.OpLoad(f1, v_gap);
        // The next stretch starts here.
        ctx.OpStore(v_uv_prev, uv);
        ctx.OpStore(v_qz_prev, qz);
        ctx.OpStore(v_in_prev, inside);
        ctx.OpStore(v_t_prev, t);
    } else {
        std::tie(hit, gap) = point_test(t, uvc, n_at, q_at, qz_at, row2, row3);
        ctx.eye_override = Id{};
    }
    Id counted = ctx.OpSelect(ctx.F32[1], seen, mul(hit, edge), ctx.ConstF32(0.0f));
    // Neither eye sees it: the blocker grid there, and halfway back to the last step (the steps
    // are far apart up the ray; light_shadow.h Grid). Only where the grid holds anything (from
    // NearSkipM to ReachM of the eye); beyond its reach, off both pictures, the march ends.
    Id go_on = seen;
    if (grid_look) {
        // (light_shadow.h Grid::Soft: off the receiver's own surface; within the edge band too,
        // where the picture holds the step behind something; two cells for a full hit.)
        const bool soft = Tune::Grid::Soft();
        const auto off_surface = [&](Id t_at) {
            return ctx.OpSelect(
                ctx.U1[1], s.normal_ok,
                ctx.OpFOrdGreaterThan(ctx.U1[1], mul(t_at, rise_per_m), mul(voxel, ctx.ConstF32(Tune::Grid::SurfaceVoxels))),
                ctx.OpFOrdGreaterThan(ctx.U1[1], t_at, mul(voxel, ctx.ConstF32(Tune::Grid::SurfaceVoxels + 1.0f))));
        };
        ctx.OpStore(s.grid_hit, ctx.ConstF32(0.0f));
        const Id within = ctx.OpFOrdLessThan(ctx.U1[1], q_len, s.grid_reach);
        Id want = ctx.OpLogicalNot(ctx.U1[1], seen);
        if (soft) {
            const Id banded = ctx.OpLogicalAnd(
                ctx.U1[1], seen,
                ctx.OpLogicalAnd(ctx.U1[1], ctx.OpFOrdLessThan(ctx.U1[1], edge, ctx.ConstF32(1.0f)),
                                 ctx.OpFOrdGreaterThan(ctx.U1[1], gap, bias)));
            want = ctx.OpLogicalAnd(ctx.U1[1], ctx.OpLogicalOr(ctx.U1[1], want, banded), off_surface(t));
        }
        const Id look = ctx.OpLogicalAnd(
            ctx.U1[1], ctx.OpLogicalAnd(ctx.U1[1], s.grid_on, want),
            ctx.OpLogicalAnd(ctx.U1[1], within, ctx.OpFOrdGreaterThan(ctx.U1[1], q_len, s.grid_near)));
        const Id grid_label = ctx.OpLabel(), grid_done = ctx.OpLabel();
        ctx.OpSelectionMerge(grid_done, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(look, grid_label, grid_done);
        ctx.AddLabel(grid_label);
        {
            const auto solid_at = [&](Id t_at) {
                // (Opt 8: the step's place in the grid's space from the ray's there.)
                const auto [index, key] =
                    opt_grid ? GridCellAt(ctx, ctx.OpFAdd(ctx.F32[3], grid_w0, ctx.OpVectorTimesScalar(ctx.F32[3], grid_wd, t_at)))
                             : GridCell(ctx, ctx.OpFAdd(ctx.F32[3], s.p, ctx.OpVectorTimesScalar(ctx.F32[3], dir, t_at)));
                if (!Tune::SceneMaps::Opt(128)) {
                    return GridSolid(ctx, ctx.OpLoad(ctx.U32[1], GridPtr(ctx, index)), key);
                }
                // (Opt 128: the cell read only where its block was stamped within the life.)
                const Id stamp = ctx.OpLoad(ctx.U32[1], GridPtr(ctx, GridBlock(ctx, index)));
                const Id live = ctx.OpLogicalAnd(ctx.U1[1], ctx.OpINotEqual(ctx.U1[1], stamp, ctx.u32_zero_value),
                                                 ctx.OpULessThan(ctx.U1[1], GridAge(ctx, stamp), s.grid_life));
                const Id solid = Private(ctx, ctx.U1[1]);
                ctx.OpStore(solid, ctx.false_value);
                const Id cell_l = ctx.OpLabel(), cell_done = ctx.OpLabel();
                ctx.OpSelectionMerge(cell_done, spv::SelectionControlMask::MaskNone);
                ctx.OpBranchConditional(live, cell_l, cell_done);
                ctx.AddLabel(cell_l);
                ctx.OpStore(solid, GridSolid(ctx, ctx.OpLoad(ctx.U32[1], GridPtr(ctx, index)), key));
                ctx.OpBranch(cell_done);
                ctx.AddLabel(cell_done);
                return ctx.OpLoad(ctx.U1[1], solid);
            };
            const Id i_mid = ctx.OpFSub(ctx.F32[1], i_f, ctx.ConstF32(0.5f));
            const Id u_mid = ctx.OpFMax(
                ctx.F32[1],
                opt_step ? mul(i_mid, inv_count)
                         : ctx.OpFDiv(ctx.F32[1], i_mid, ctx.OpConvertUToF(ctx.F32[1], count)),
                ctx.ConstF32(0.0f));
            const Id t_mid = adaptive ? mul(add(ctx.OpLoad(ctx.F32[1], t_prev_var), t), ctx.ConstF32(0.5f))
                                      : add(ctx.ConstF32(Tune::StartM), mul(span, mul(u_mid, u_mid)));
            if (soft) {
                // (The cell halfway back read only where the step's own is solid: most cells are
                // empty, so about half the grid's reads; v132's lookups cost 0.15 ms at Oedon.)
                const Id at_step = solid_at(t);
                ctx.OpStore(s.grid_hit, ctx.OpSelect(ctx.F32[1], at_step, ctx.ConstF32(0.5f), ctx.ConstF32(0.0f)));
                const Id mid_l = ctx.OpLabel(), mid_done = ctx.OpLabel();
                ctx.OpSelectionMerge(mid_done, spv::SelectionControlMask::MaskNone);
                ctx.OpBranchConditional(at_step, mid_l, mid_done);
                ctx.AddLabel(mid_l);
                {
                    const Id at_mid = ctx.OpLogicalAnd(ctx.U1[1], solid_at(t_mid), off_surface(t_mid));
                    ctx.OpStore(s.grid_hit, ctx.OpSelect(ctx.F32[1], at_mid, ctx.ConstF32(1.0f), ctx.ConstF32(0.5f)));
                }
                ctx.OpBranch(mid_done);
                ctx.AddLabel(mid_done);
            } else {
                const Id solid = ctx.OpLogicalOr(ctx.U1[1], solid_at(t), solid_at(t_mid));
                ctx.OpStore(s.grid_hit, ctx.OpSelect(ctx.F32[1], solid, ctx.ConstF32(1.0f),
                                                     ctx.ConstF32(0.0f)));
            }
        }
        ctx.OpBranch(grid_done);
        ctx.AddLabel(grid_done);
        // (Soft: the grid's weight rises as the screen's falls across the edge band; off both
        // pictures, edge is 0 and it counts in full.)
        counted = ctx.OpFMax(ctx.F32[1], counted,
                             soft ? mul(ctx.OpLoad(ctx.F32[1], s.grid_hit), ctx.OpFSub(ctx.F32[1], ctx.ConstF32(1.0f), edge))
                                  : ctx.OpLoad(ctx.F32[1], s.grid_hit));
        go_on = ctx.OpLogicalOr(ctx.U1[1], seen, ctx.OpLogicalAnd(ctx.U1[1], s.grid_on, within));
    }
    ctx.OpStore(s.occl, ctx.OpFMax(ctx.F32[1], o, counted));
    if (adaptive) {
        // The next step: SmartFreeShare of the open air in front of the surface the eye sees here
        // (none where the step is at or behind it; off the pictures, the cap), at least min_step,
        // and no longer on the picture than SmartPixels pixels (the ray's speed across it here).
        const Id inv2 = mul(inv_cw, inv_cw);
        const Id dnx = mul(ctx.OpFSub(ctx.F32[1], mul(cd[0], cw), mul(at(0), cd[3])), inv2);
        const Id dny = mul(ctx.OpFSub(ctx.F32[1], mul(cd[1], cw), mul(at(1), cd[3])), inv2);
        const Id duv = ctx.OpFMul(ctx.F32[2], s.a, ctx.OpCompositeConstruct(ctx.F32[2], std::array{dnx, dny}));
        const Id px_per_m = ctx.OpLength(ctx.F32[1], ctx.OpFDiv(ctx.F32[2], duv, ctx.OpFMax(ctx.F32[2], s.pixel, ctx.ConstF32(1e-7f, 1e-7f))));
        const Id cap = ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(Tune::SceneMaps::SmartPixels), ctx.OpFMax(ctx.F32[1], px_per_m, ctx.ConstF32(1e-4f)));
        const Id in_front = ctx.OpLogicalAnd(ctx.U1[1], seen, ctx.OpFOrdLessThan(ctx.U1[1], gap, ctx.ConstF32(0.0f)));
        const Id free = ctx.OpSelect(ctx.F32[1], in_front, mul(ctx.ConstF32(Tune::SceneMaps::SmartFreeShare), ctx.OpFNegate(ctx.F32[1], gap)),
                                     ctx.OpSelect(ctx.F32[1], seen, ctx.ConstF32(0.0f), cap));
        const Id step_len = ctx.OpFMax(ctx.F32[1], ctx.OpFMin(ctx.F32[1], free, cap), min_step);
        ctx.OpStore(t_prev_var, t);
        ctx.OpStore(t_var, add(t, step_len));
    }
    // Off the eyes' images (and no grid): nothing more to see along this ray.
    ctx.OpStore(s.step, ctx.OpSelect(ctx.U32[1], go_on, ctx.OpIAdd(ctx.U32[1], i, ctx.ConstU32(1u)),
                                     count));
    ctx.OpBranch(cont);
    ctx.AddLabel(cont);
    ctx.OpBranch(header);
    ctx.AddLabel(merge);
    return ctx.OpFSub(ctx.F32[1], ctx.ConstF32(1.0f), ctx.OpLoad(ctx.F32[1], s.occl));
}

// A light's march steps at this pixel (LightVisibilityAt's rule; the mask's march too): by the light's
// strength here (`w`: 1 less its falloff; MinSteps at its rim, the near or far steps beside it) and
// no more than the ray's length on the picture needs (`here`: the pixel's picture coordinates; a
// light behind the eye: as many as its strength gives); the detail by the pixel's distance.
Id MarchSteps(EmitContext& ctx, Id l, Id w, Id here, float share, Id coarse, Id boost) {
    const auto& s = ctx.vr_light_shadow;
    const auto mix = [&](float far_value, float near_value, Id f) {
        return ctx.OpFAdd(ctx.F32[1], ctx.ConstF32(far_value),
                          ctx.OpFMul(ctx.F32[1], ctx.ConstF32(near_value - far_value), f));
    };
    const Id detail = ctx.OpFClamp(
        ctx.F32[1],
        ctx.OpFMul(ctx.F32[1],
                   ctx.OpFSub(ctx.F32[1], ctx.ConstF32(Tune::DetailFarM), ctx.OpLength(ctx.F32[1], s.p)),
                   ctx.ConstF32(1.0f / (Tune::DetailFarM - Tune::DetailNearM))),
        ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
    const Id farness = ctx.OpFClamp(
        ctx.F32[1],
        ctx.OpFMul(ctx.F32[1],
                   ctx.OpFSub(ctx.F32[1], ctx.ConstF32(Tune::DetailFarthestM),
                              ctx.OpLength(ctx.F32[1], s.p)),
                   ctx.ConstF32(1.0f / (Tune::DetailFarthestM - Tune::DetailFarM))),
        ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
    const auto toward = [&](float farthest, Id value) {
        return ctx.OpFAdd(ctx.F32[1], ctx.ConstF32(farthest),
                          ctx.OpFMul(ctx.F32[1], ctx.OpFSub(ctx.F32[1], value,
                                                            ctx.ConstF32(farthest)),
                                     farness));
    };
    // Steps by the light's strength here (MinSteps at the rim of its reach, Steps beside it),
    // and no more than the ray's length on screen needs (PixelsPerStep; a light behind the eye:
    // as many as its strength gives).
    // VR_LIGHT_STEPS=<share> (tests): the near and far step counts times that share.
    static const float steps_share = [] {
        const char* const env = std::getenv("VR_LIGHT_STEPS");
        return env != nullptr ? float(std::atof(env)) : 1.0f;
    }();
    // VR_LIGHT_STEPS_FAR / VR_LIGHT_PIXELS_FAR (tests): the medium-to-far step count and spacing.
    static const float steps_far = [] {
        const char* const env = std::getenv("VR_LIGHT_STEPS_FAR");
        return env != nullptr ? float(std::atof(env)) : float(Tune::StepsFar);
    }();
    static const float pixels_far = [] {
        const char* const env = std::getenv("VR_LIGHT_PIXELS_FAR");
        return env != nullptr ? float(std::atof(env)) : Tune::PixelsFar;
    }();
    Id most = toward(float(Tune::StepsFarthest),
                     mix(steps_far * steps_share * share,
                         float(Tune::StepsNear) * steps_share * share, detail));
    Id least = toward(float(Tune::MinStepsFarthest), ctx.ConstF32(float(Tune::MinSteps)));
    // (`boost`: a close lamp's (light_shadow.h SceneMaps::CloseStepShare), the most steps times it.)
    if (Sirit::ValidId(boost)) {
        most = ctx.OpFMul(ctx.F32[1], most, boost);
    }
    // (`coarse`: the depth pyramid's scale, its steps that many times fewer and longer: each
    // stretch is checked whole, read finely only where something can block it.)
    if (Sirit::ValidId(coarse)) {
        most = ctx.OpFDiv(ctx.F32[1], most, coarse);
        least = ctx.OpFMax(ctx.F32[1], ctx.OpFDiv(ctx.F32[1], least, coarse), ctx.ConstF32(2.0f));
    }
    // (By the square root of the falloff: a shadow cast from near the light lands out at its rim;
    // light_shadow.h ReachOld.)
    const Id w01 = ctx.OpFClamp(ctx.F32[1], w, ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
    const Id by_weight = ctx.OpFAdd(
        ctx.F32[1], least,
        ctx.OpFMul(ctx.F32[1], ctx.OpFSub(ctx.F32[1], most, least),
                   Tune::ReachOld() ? w01 : ctx.OpSqrt(ctx.F32[1], w01)));
    const auto lc = Transform(ctx, s.inv, ctx.OpCompositeExtract(ctx.F32[1], l, 0u),
                              ctx.OpCompositeExtract(ctx.F32[1], l, 1u),
                              ctx.OpCompositeExtract(ctx.F32[1], l, 2u));
    const Id lw = ctx.OpFMax(ctx.F32[1], lc[3], ctx.ConstF32(1e-6f));
    const Id luv = ctx.OpFAdd(
        ctx.F32[2],
        ctx.OpFMul(ctx.F32[2], s.a,
                   ctx.OpCompositeConstruct(ctx.F32[2],
                                            std::array{ctx.OpFDiv(ctx.F32[1], lc[0], lw),
                                                       ctx.OpFDiv(ctx.F32[1], lc[1], lw)})),
        s.b);
    const Id pixels = ctx.OpLength(
        ctx.F32[1], ctx.OpFDiv(ctx.F32[2], ctx.OpFSub(ctx.F32[2], luv, here),
                               ctx.OpFMax(ctx.F32[2], s.pixel, ctx.ConstF32(1e-7f, 1e-7f))));
    const Id by_screen = ctx.OpSelect(
        ctx.F32[1], ctx.OpFOrdGreaterThan(ctx.U1[1], lc[3], ctx.ConstF32(1e-6f)),
        ctx.OpFMax(ctx.F32[1], least,
                   ctx.OpFDiv(ctx.F32[1], pixels,
                              Sirit::ValidId(coarse)
                                  ? ctx.OpFMul(ctx.F32[1], toward(Tune::PixelsFarthest, mix(pixels_far, Tune::PixelsNear, detail)), coarse)
                                  : toward(Tune::PixelsFarthest, mix(pixels_far, Tune::PixelsNear, detail)))),
        by_weight);
    return ctx.OpConvertFToU(
        ctx.U32[1], ctx.OpFAdd(ctx.F32[1], ctx.OpFMin(ctx.F32[1], by_weight, by_screen),
                               ctx.ConstF32(0.5f)));
}

} // namespace

// The one-time setup for the other VR lighting files (emit_spirv_light_common.h).
void VrLightSetup(EmitContext& ctx, Id ndc, Id uv, Id depth) {
    if (!ctx.vr_light_shadow.ready) {
        Setup(ctx, ndc, uv, depth, false);
    }
}

// ---- The hunter's own shadow (ir/passes/body_shadow.h) ----

Id BodyRead(EmitContext& ctx, u32 handle, u32 dword) {
    const auto& buffer = ctx.buffers[handle];
    Id index = ctx.ConstU32(dword);
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

// BodyRead at a computed dword.
Id BodyReadAt(EmitContext& ctx, u32 handle, Id dword) {
    const auto& buffer = ctx.buffers[handle];
    Id index = dword;
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

// The light at `l` seen from the surface at `p` (this eye's view) past the hunter: his body map
// when the light is a mapped lamp's (`maps`, body_shadow.h MaxMaps; ~0u: none), else his
// capsules. 1 lit, 0 behind him, soft at the edges. Only where the ray passes his bounding sphere.
Id BodyVisibility(EmitContext& ctx, u32 buf, u32 maps, Id p, Id l, bool his_known) {
    namespace Body = Shader::Optimization::BodyShadow;
    const Id f1 = ctx.F32[1], f3 = ctx.F32[3];
    const auto F = [&](float v) { return ctx.ConstF32(v); };
    const auto rd = [&](u32 d) { return ctx.OpBitcast(f1, BodyRead(ctx, buf, d)); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(f1, a, b); };
    const auto sub = [&](Id a, Id b) { return ctx.OpFSub(f1, a, b); };
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(f1, a, b); };
    const auto div = [&](Id a, Id b) { return ctx.OpFDiv(f1, a, b); };
    const auto dot = [&](Id a, Id b) { return ctx.OpDot(f1, a, b); };
    const auto clamp01 = [&](Id v) { return ctx.OpFClamp(f1, v, F(0.0f), F(1.0f)); };
    // The flat camera's view to this eye's (the fog's convention: eye x = flat x - shift).
    Id shift = F(0.0f);
    if (ctx.runtime_info.native_stereo && ctx.runtime_info.native_multiview) {
        const Id half = rd(1);
        shift = ctx.OpSelect(f1, ctx.OpIEqual(ctx.U1[1], ctx.NativeEyeIndex(), ctx.u32_zero_value),
                             ctx.OpFNegate(f1, half), half);
    }
    const auto point = [&](u32 d) {
        return ctx.OpCompositeConstruct(f3, std::array{sub(rd(d), shift), rd(d + 1), rd(d + 2)});
    };
    const Id units = ctx.OpFMax(f1, rd(3), F(1e-3f)); // game units a metre
    const Id to_light = ctx.OpFSub(f3, l, p);
    const Id length = ctx.OpLength(f1, to_light);
    const Id dir = ctx.OpVectorTimesScalar(f3, to_light, div(F(1.0f), ctx.OpFMax(f1, length, F(1e-4f))));
    const Id start = mul(F(Body::StartM), units);
    const Id p0 = ctx.OpFAdd(f3, p, ctx.OpVectorTimesScalar(f3, dir, start));
    const Id d1 = ctx.OpFSub(f3, l, p0);
    const Id a = ctx.OpFMax(f1, dot(d1, d1), F(1e-8f));
    const Id span = ctx.OpSqrt(f1, a);
    // His bounding sphere (header 4..7) against the ray: nothing to test when it passes clear.
    const Id centre = point(4);
    const Id along = ctx.OpFClamp(f1, dot(ctx.OpFSub(f3, centre, p0), dir), F(0.0f), span);
    const Id nearest = ctx.OpFAdd(f3, p0, ctx.OpVectorTimesScalar(f3, dir, along));
    const Id gap = ctx.OpLength(f1, ctx.OpFSub(f3, centre, nearest));
    // Only a lamp his shadow comes from (body_shadow.h BodyLampsAt: up to three; none listed: any).
    Id from_his_lamp = ctx.true_value;
    if (!his_known) {
        const Id light_flat = ctx.OpCompositeConstruct(
            f3, std::array{add(ctx.OpCompositeExtract(f1, l, 0u), shift), ctx.OpCompositeExtract(f1, l, 1u),
                           ctx.OpCompositeExtract(f1, l, 2u)});
        const Id listed = BodyRead(ctx, buf, Body::BodyLampsAt);
        const Id tolerance = mul(F(Body::MapMatchM), units);
        from_his_lamp = ctx.OpIEqual(ctx.U1[1], listed, ctx.u32_zero_value);
        for (u32 j = 0; j < Body::BodyLamps; ++j) {
            const u32 h = Body::BodyLampsAt + 4 + 4 * j;
            const Id lamp = ctx.OpCompositeConstruct(f3, std::array{rd(h), rd(h + 1), rd(h + 2)});
            from_his_lamp = ctx.OpLogicalOr(
                ctx.U1[1], from_his_lamp,
                ctx.OpLogicalAnd(ctx.U1[1], ctx.OpULessThan(ctx.U1[1], ctx.ConstU32(j), listed),
                                 ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpLength(f1, ctx.OpFSub(f3, light_flat, lamp)),
                                                    tolerance)));
        }
    }
    const Id go = ctx.OpLogicalAnd(
        ctx.U1[1],
        ctx.OpLogicalAnd(ctx.U1[1], ctx.OpINotEqual(ctx.U1[1], BodyRead(ctx, buf, 2u), ctx.u32_zero_value),
                         from_his_lamp),
        ctx.OpLogicalAnd(ctx.U1[1], ctx.OpFOrdLessThan(ctx.U1[1], gap, rd(7)),
                         ctx.OpFOrdGreaterThan(ctx.U1[1], length, start)));
    const Id vis = ctx.AddGlobalVariable(ctx.TypePointer(spv::StorageClass::Private, f1),
                                         spv::StorageClass::Private);
    ctx.interfaces.push_back(vis);
    ctx.OpStore(vis, F(1.0f));
    const Id body = ctx.OpLabel(), merge = ctx.OpLabel();
    ctx.OpSelectionMerge(merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(go, body, merge);
    ctx.AddLabel(body);
    // His body map: the light within MapMatchM of a map's lamp (both in the flat view).
    const Id l_flat = ctx.OpCompositeConstruct(
        f3, std::array{add(ctx.OpCompositeExtract(f1, l, 0u), shift), ctx.OpCompositeExtract(f1, l, 1u),
                       ctx.OpCompositeExtract(f1, l, 2u)});
    Id matched = ctx.false_value;
    Id header = ctx.ConstU32(Body::MapsAt);
    if (maps != ~0u && Body::MapsEnabled()) {
        const Id tolerance = mul(F(Body::MapMatchM), units);
        for (u32 k = Body::MaxMaps; k-- > 0;) {
            const u32 h = Body::MapsAt + k * Body::MapDwords;
            const Id lamp = ctx.OpCompositeConstruct(f3, std::array{rd(h + 4), rd(h + 5), rd(h + 6)});
            const Id here = ctx.OpLogicalAnd(
                ctx.U1[1], ctx.OpINotEqual(ctx.U1[1], BodyRead(ctx, buf, h), ctx.u32_zero_value),
                ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpLength(f1, ctx.OpFSub(f3, l_flat, lamp)),
                                   tolerance));
            header = ctx.OpSelect(ctx.U32[1], here, ctx.ConstU32(h), header);
            matched = ctx.OpLogicalOr(ctx.U1[1], matched, here);
        }
        if (Body::MapsRightEyeOnly()) {
            matched = ctx.OpLogicalAnd(
                ctx.U1[1], matched,
                ctx.OpINotEqual(ctx.U1[1], ctx.NativeEyeIndex(), ctx.u32_zero_value));
        }
    }
    const Id map_label = ctx.OpLabel(), caps_label = ctx.OpLabel(), chosen = ctx.OpLabel();
    ctx.OpSelectionMerge(chosen, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(matched, map_label, caps_label);
    ctx.AddLabel(map_label);
    if (maps != ~0u) {
        const auto hd = [&](u32 d) {
            return ctx.OpBitcast(f1, BodyReadAt(ctx, buf, ctx.OpIAdd(ctx.U32[1], header, ctx.ConstU32(d))));
        };
        const auto hu = [&](u32 d) {
            return BodyReadAt(ctx, buf, ctx.OpIAdd(ctx.U32[1], header, ctx.ConstU32(d)));
        };
        const Id p_flat = ctx.OpCompositeConstruct(
            f3, std::array{add(ctx.OpCompositeExtract(f1, p, 0u), shift),
                           ctx.OpCompositeExtract(f1, p, 1u), ctx.OpCompositeExtract(f1, p, 2u)});
        const auto row = [&](u32 r) {
            return add(dot(p_flat, ctx.OpCompositeConstruct(
                                       f3, std::array{hd(8 + 4 * r), hd(9 + 4 * r), hd(10 + 4 * r)})),
                       hd(11 + 4 * r));
        };
        const Id qx = row(0), qy = row(1), qz = row(2);
        const Id focal = hd(2);
        const Id near_z = hd(3);
        const Id base = hu(1);
        const Id size_u = hu(7);
        const Id size = ctx.OpConvertUToF(f1, size_u);
        const Id inv_z = div(F(1.0f), ctx.OpFMax(f1, qz, F(1e-6f)));
        const Id sx = mul(mul(qx, inv_z), focal);
        const Id sy = mul(mul(qy, inv_z), focal);
        const Id in_front = ctx.OpFOrdGreaterThan(ctx.U1[1], qz, near_z);
        const Id inside = ctx.OpLogicalAnd(
            ctx.U1[1], ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpFAbs(f1, sx), F(1.0f)),
            ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpFAbs(f1, sy), F(1.0f)));
        // Texel coordinates, then a 2x2 filter of the compares (soft over a texel).
        const Id tx = sub(mul(add(mul(sx, F(0.5f)), F(0.5f)), size), F(0.5f));
        const Id ty = sub(mul(add(mul(sy, F(0.5f)), F(0.5f)), size), F(0.5f));
        const Id fx = ctx.OpFloor(f1, tx), fy = ctx.OpFloor(f1, ty);
        const Id wx = sub(tx, fx), wy = sub(ty, fy);
        const Id last = sub(size, F(1.0f));
        // The texel's width at this depth: the bias grows with it (its own limb is not its shadow).
        const Id texel = div(mul(F(2.0f), qz), ctx.OpFMax(f1, mul(focal, size), F(1e-3f)));
        const Id bias = add(mul(F(Body::MapBiasM), units), mul(F(Body::MapBiasTexels), texel));
        const auto occluded = [&](Id x, Id y) {
            const Id cx = ctx.OpConvertFToU(ctx.U32[1], ctx.OpFClamp(f1, x, F(0.0f), last));
            const Id cy = ctx.OpConvertFToU(ctx.U32[1], ctx.OpFClamp(f1, y, F(0.0f), last));
            const Id at = ctx.OpIAdd(ctx.U32[1], base,
                                     ctx.OpIAdd(ctx.U32[1], ctx.OpIMul(ctx.U32[1], cy, size_u), cx));
            const Id depth = ctx.OpBitcast(f1, BodyReadAt(ctx, maps, at));
            const Id hit = Body::MapsDebug()
                               ? ctx.OpFOrdLessThan(ctx.U1[1], depth, F(1e30f))
                               : ctx.OpFOrdGreaterThan(ctx.U1[1], qz, add(depth, bias));
            return ctx.OpSelect(f1, hit, F(1.0f), F(0.0f));
        };
        const Id x1 = add(fx, F(1.0f)), y1 = add(fy, F(1.0f));
        const Id top = ctx.OpFMix(f1, occluded(fx, fy), occluded(x1, fy), wx);
        const Id bottom = ctx.OpFMix(f1, occluded(fx, y1), occluded(x1, y1), wx);
        // Doubled and clamped: a limb two texels wide (a leg from a torch at his hip) is solid,
        // the outer half texel of an edge stays soft.
        const Id occ = ctx.OpFClamp(f1, mul(F(2.0f), ctx.OpFMix(f1, top, bottom, wy)), F(0.0f),
                                    F(1.0f));
        Id map_vis = ctx.OpSelect(f1, ctx.OpLogicalAnd(ctx.U1[1], in_front, inside),
                                  sub(F(1.0f), mul(F(Body::Strength), occ)), F(1.0f));
        if (Body::MapsDebugMode() == 2) {
            map_vis = F(0.0f);
        } else if (Body::MapsDebugMode() == 3) {
            map_vis = ctx.OpSelect(f1, ctx.OpLogicalAnd(ctx.U1[1], in_front, inside), F(0.0f),
                                   F(1.0f));
        }
        ctx.OpStore(vis, map_vis);
    }
    ctx.OpBranch(chosen);
    ctx.AddLabel(caps_label);
    Id result = F(1.0f);
    for (u32 k = 0; k < Body::MaxCapsules; ++k) {
        const u32 base = Body::HeaderDwords + k * Body::CapsuleDwords;
        const Id A = point(base);
        const Id r = rd(base + 3);
        const Id B = point(base + 4);
        const Id d2 = ctx.OpFSub(f3, B, A);
        const Id r0 = ctx.OpFSub(f3, p0, A);
        const Id e = ctx.OpFMax(f1, dot(d2, d2), F(1e-8f));
        const Id f = dot(d2, r0);
        const Id c = dot(d1, r0);
        const Id b = dot(d1, d2);
        const Id den = sub(mul(a, e), mul(b, b));
        // The closest points of the ray's segment (s) and the limb's axis (t).
        const Id s_free = ctx.OpSelect(f1, ctx.OpFOrdGreaterThan(ctx.U1[1], den, F(1e-8f)),
                                       clamp01(div(sub(mul(b, f), mul(c, e)), ctx.OpFMax(f1, den, F(1e-8f)))),
                                       F(0.0f));
        const Id t_raw = div(add(mul(b, s_free), f), e);
        const Id s = ctx.OpSelect(
            f1, ctx.OpFOrdLessThan(ctx.U1[1], t_raw, F(0.0f)), clamp01(div(ctx.OpFNegate(f1, c), a)),
            ctx.OpSelect(f1, ctx.OpFOrdGreaterThan(ctx.U1[1], t_raw, F(1.0f)), clamp01(div(sub(b, c), a)),
                         s_free));
        const Id t = clamp01(t_raw);
        const Id q1 = ctx.OpFAdd(f3, p0, ctx.OpVectorTimesScalar(f3, d1, s));
        const Id q2 = ctx.OpFAdd(f3, A, ctx.OpVectorTimesScalar(f3, d2, t));
        const Id dist = ctx.OpLength(f1, ctx.OpFSub(f3, q1, q2));
        // The receiver on this limb itself: not its own shadow.
        const Id tp = clamp01(div(dot(ctx.OpFSub(f3, p, A), d2), e));
        const Id own = ctx.OpFOrdLessThan(
            ctx.U1[1], ctx.OpLength(f1, ctx.OpFSub(f3, p, ctx.OpFAdd(f3, A, ctx.OpVectorTimesScalar(f3, d2, tp)))),
            add(r, mul(F(Body::OwnM), units)));
        // The light on this limb (the hand's torch, the hip's lantern): it does not hide it.
        const Id tl = clamp01(div(dot(ctx.OpFSub(f3, l, A), d2), e));
        const Id carrier = ctx.OpFOrdLessThan(
            ctx.U1[1], ctx.OpLength(f1, ctx.OpFSub(f3, l, ctx.OpFAdd(f3, A, ctx.OpVectorTimesScalar(f3, d2, tl)))),
            add(r, mul(F(Body::CarryM), units)));
        // The edge: wider the farther the receiver is behind the limb (a small flame's penumbra).
        const Id behind = mul(s, span);
        const Id edge = add(mul(F(Body::MinEdgeM), units),
                            div(mul(mul(F(Body::LightM), units), behind),
                                ctx.OpFMax(f1, sub(span, behind), mul(F(0.2f), units))));
        const Id x = clamp01(div(sub(dist, sub(r, edge)), ctx.OpFMax(f1, mul(F(2.0f), edge), F(1e-5f))));
        const Id occ = sub(F(1.0f), mul(mul(x, x), sub(F(3.0f), mul(F(2.0f), x))));
        const Id counted = ctx.OpSelect(
            f1, ctx.OpLogicalOr(ctx.U1[1], ctx.OpLogicalOr(ctx.U1[1], own, carrier),
                                ctx.OpFOrdLessThanEqual(ctx.U1[1], r, F(0.0f))),
            F(0.0f), occ);
        result = ctx.OpFMin(f1, result, sub(F(1.0f), mul(F(Body::Strength), counted)));
    }
    ctx.OpStore(vis, result);
    ctx.OpBranch(chosen);
    ctx.AddLabel(chosen);
    ctx.OpBranch(merge);
    ctx.AddLabel(merge);
    return ctx.OpLoad(f1, vis);
}

// (`hero`: High's hero lamp, light_shadow.h High: the march itself always, no cube test, no group.)
Id LightVisibilityAt(EmitContext& ctx, Id base, Id ndc, Id uv, Id depth, bool hero = false);

// High lamp shadows (light_shadow.h High; RuntimeInfo::vr_light_shadows 2): the old march's own
// setups (the view place, normal and noise; the other eye; the grid's header, read only; the
// lights kept on the camera), once, at the first light: here in the entry block, where their
// derivatives hold, before the mask's read branches.
void HeroSetup(EmitContext& ctx, Id ndc, Id uv, Id depth) {
    auto& s = ctx.vr_light_shadow;
    if (s.hero_ready) {
        return;
    }
    s.hero_ready = true;
    if (!s.ready) {
        Setup(ctx, ndc, uv, depth, true);
    }
    SetupOtherEye(ctx);
    GridSetup(ctx, false);
    AttachedSetup(ctx);
}

// High: this shader's lights whose slots the CPU made the frame's heroes (SlotHero: the torch in
// your hand, the lamps lighting the most of the view) march at the lamp pass's own resolution, the
// old method's march (its steps, the other eye's picture, the grid, his body), all in ONE loop over
// the lights: one copy of the march however many lights the shader has. (A copy a light, the first
// build of 2026-11-17, made lamp programs of 850 KB whose compiles stalled the game for seconds.)
// Each light's entry: its march's visibility, or -1 for a light that is no hero. A light's slot is
// the CPU's byte for this draw (light_shadow.h SceneMaps::DrawSlotsAt: slot + 1; 0, the CPU saw no
// list: no hero, the mask's own match stands). Once, after the first light's mask read (its setup
// and HeroSetup's are in the entry block before it).
void HeroLoop(EmitContext& ctx, Id ndc, Id uv, Id depth) {
    namespace Scene = Tune::SceneMaps;
    namespace Body = Shader::Optimization::BodyShadow;
    auto& s = ctx.vr_light_shadow;
    const auto& info = ctx.info.vr_light_shadow;
    if (s.hero_loop_ready) {
        return;
    }
    s.hero_loop_ready = true;
    const u32 count = std::min(info.count, Scene::DrawLights);
    if (!s.ready || count == 0 || info.stride == 0 || !Sirit::ValidId(s.scene_count)) {
        return;
    }
    const Id u1 = ctx.U32[1], b1 = ctx.U1[1], f1 = ctx.F32[1];
    const auto u = [&](u32 v) { return ctx.ConstU32(v); };
    s.hero_vis = Private(ctx, ctx.TypeArray(f1, u(count)));
    s.hero_vis_ptr = ctx.TypePointer(spv::StorageClass::Private, f1);
    s.hero_count = count;
    const Id index = Private(ctx, u1);
    ctx.OpStore(index, ctx.u32_zero_value);
    // The hero slots already marching in this draw: one light a slot (the CPU's match gives a slot
    // to every light of the draw within SlotKeepM of its lamp: a spare candle beside a hero lamp
    // reads the mask, as at Low, so the march stays HeroLamps at most).
    const Id claimed = Private(ctx, u1);
    ctx.OpStore(claimed, ctx.u32_zero_value);
    const Id header = ctx.OpLabel(), body = ctx.OpLabel(), cont = ctx.OpLabel(), merge = ctx.OpLabel();
    ctx.OpBranch(header);
    ctx.AddLabel(header);
    const Id i = ctx.OpLoad(u1, index);
    const Id more = ctx.OpULessThan(b1, i, u(count)); // (the condition before the merge)
    ctx.OpLoopMerge(merge, cont, spv::LoopControlMask::MaskNone);
    ctx.OpBranchConditional(more, body, merge);
    ctx.AddLabel(body);
    {
        const Id word = BodyReadAt(ctx, s.scene_body,
                                   ctx.OpIAdd(u1, u(Body::SlotsAt + Scene::DrawSlotsAt), ctx.OpShiftRightLogical(u1, i, u(2u))));
        const Id byte = ctx.OpBitwiseAnd(
            u1, ctx.OpShiftRightLogical(u1, word, ctx.OpShiftLeftLogical(u1, ctx.OpBitwiseAnd(u1, i, u(3u)), u(3u))), u(255u));
        // (A slot the table holds: slot + 1 up to Slots; 255, none, and 0 fail.)
        const Id in_table = ctx.OpLogicalAnd(b1, ctx.OpINotEqual(b1, byte, ctx.u32_zero_value),
                                             ctx.OpULessThanEqual(b1, byte, u(Scene::Slots)));
        const Id k = ctx.OpUMin(u1, ctx.OpISub(u1, ctx.OpUMax(u1, byte, u(1u)), u(1u)), u(Scene::Slots - 1u));
        const SlotRead slot = SceneSlot(ctx, k);
        const Id taken = ctx.OpLoad(u1, claimed);
        const Id bit = ctx.OpShiftLeftLogical(u1, u(1u), k);
        const Id unclaimed = ctx.OpIEqual(b1, ctx.OpBitwiseAnd(u1, taken, bit), ctx.u32_zero_value);
        const Id hero = ctx.OpLogicalAnd(
            b1, ctx.OpLogicalAnd(b1, in_table, SlotHas(ctx, slot.flags, Scene::SlotHero)), unclaimed);
        ctx.OpStore(claimed, ctx.OpSelect(u1, hero, ctx.OpBitwiseOr(u1, taken, bit), taken));
        const Id entry = ctx.OpAccessChain(s.hero_vis_ptr, s.hero_vis, i);
        ctx.OpStore(entry, ctx.ConstF32(-1.0f));
        const Id march_label = ctx.OpLabel(), done = ctx.OpLabel();
        ctx.OpSelectionMerge(done, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(hero, march_label, done);
        ctx.AddLabel(march_label);
        // (Its first dword: stride a light, as the non-mask loop's; LightVisibilityAt's result is in
        // 0..1, never not a number, so -1 stays the no-hero mark.)
        ctx.OpStore(entry, LightVisibilityAt(ctx, ctx.OpIMul(u1, i, u(info.stride)), ndc, uv, depth, true));
        ctx.OpBranch(done);
        ctx.AddLabel(done);
        ctx.OpStore(index, ctx.OpIAdd(u1, i, u(1u)));
    }
    ctx.OpBranch(cont);
    ctx.AddLabel(cont);
    ctx.OpBranch(header);
    ctx.AddLabel(merge);
}

// High: the light's visibility, its hero march's (HeroLoop) where it is a hero, else the mask's
// read `mask_vis`.
Id HeroVisibility(EmitContext& ctx, u32 light, Id mask_vis) {
    auto& s = ctx.vr_light_shadow;
    const auto& info = ctx.info.vr_light_shadow;
    if (!Sirit::ValidId(s.hero_vis) || info.stride == 0) {
        return mask_vis;
    }
    // (The op's light: its first dword is 8 times it, so its index is that over the stride.)
    const u32 index = (8 * light) / info.stride;
    if (index >= s.hero_count) {
        return mask_vis;
    }
    const Id marched = ctx.OpLoad(ctx.F32[1], ctx.OpAccessChain(s.hero_vis_ptr, s.hero_vis, ctx.ConstU32(index)));
    return ctx.OpSelect(ctx.F32[1], ctx.OpFOrdGreaterThanEqual(ctx.U1[1], marched, ctx.ConstF32(0.0f)), marched,
                        mask_vis);
}

// Every light of a lamp shader at once (light_shadow.h LoopLights): one copy of the code in a
// loop, the results in a private array; the light's op reads its entry. A one-light shader, or
// VR_LIGHT_LOOP=0: the light's own copy.
Id EmitVrLightVisibility(EmitContext& ctx, u32 light, Id ndc, Id uv, Id depth) {
    if (!ctx.info.vr_light_shadow.on) {
        return ctx.ConstF32(1.0f);
    }
    if (Tune::SceneMaps::Mask()) {
        // (Tests: the channel asked rides in the immediate's high bits, SceneMaps::Tint.)
        const u32 tint = light >> Tune::SceneMaps::TintChannelShift;
        const u32 own = light & ((1u << Tune::SceneMaps::TintChannelShift) - 1u);
        // High (light_shadow.h High): the hero lamps' own march: its setups first (HeroSetup, in the
        // entry block), then the mask's read, then the heroes' loop (once) and this light's choice.
        const bool high = ctx.runtime_info.vr_light_shadows == 2 && tint == 0;
        if (high) {
            HeroSetup(ctx, ndc, uv, depth);
        }
        const Id vis = MaskVisibility(ctx, own, ndc, uv, depth, tint);
        if (!high) {
            return vis;
        }
        HeroLoop(ctx, ndc, uv, depth);
        return HeroVisibility(ctx, own, vis);
    }
    auto& s = ctx.vr_light_shadow;
    const auto& info = ctx.info.vr_light_shadow;
    if (!s.ready) {
        Setup(ctx, ndc, uv, depth, true);
    }
    SetupOtherEye(ctx);
    GridSetup(ctx);
    AttachedSetup(ctx);
    SceneSetup(ctx);
    const u32 count = info.count, stride = info.stride;
    if (!Tune::LoopLights() || count < 2 || stride == 0) {
        return LightVisibilityAt(ctx, ctx.ConstU32(8 * light), ndc, uv, depth);
    }
    if (!s.loop_ready) {
        s.loop_ready = true;
        s.loop_stride = stride;
        const Id array_type = ctx.TypeArray(ctx.F32[1], ctx.ConstU32(count));
        s.loop_vis = Private(ctx, array_type);
        s.loop_vis_ptr = ctx.TypePointer(spv::StorageClass::Private, ctx.F32[1]);
        const Id index = Private(ctx, ctx.U32[1]);
        ctx.OpStore(index, ctx.u32_zero_value);
        const Id header = ctx.OpLabel(), body = ctx.OpLabel(), cont = ctx.OpLabel(), merge = ctx.OpLabel();
        ctx.OpBranch(header);
        ctx.AddLabel(header);
        const Id i = ctx.OpLoad(ctx.U32[1], index);
        const Id more = ctx.OpULessThan(ctx.U1[1], i, ctx.ConstU32(count));
        ctx.OpLoopMerge(merge, cont, spv::LoopControlMask::MaskNone);
        ctx.OpBranchConditional(more, body, merge);
        ctx.AddLabel(body);
        {
            const Id vis = LightVisibilityAt(ctx, ctx.OpIMul(ctx.U32[1], i, ctx.ConstU32(stride)), ndc, uv, depth);
            ctx.OpStore(ctx.OpAccessChain(s.loop_vis_ptr, s.loop_vis, i), vis);
            ctx.OpStore(index, ctx.OpIAdd(ctx.U32[1], i, ctx.ConstU32(1u)));
        }
        ctx.OpBranch(cont);
        ctx.AddLabel(cont);
        ctx.OpBranch(header);
        ctx.AddLabel(merge);
    }
    // (The op's light: its first dword is 8 times it, so its index is that over the stride.)
    return ctx.OpLoad(ctx.F32[1], ctx.OpAccessChain(s.loop_vis_ptr, s.loop_vis, ctx.ConstU32(8 * light / s.loop_stride)));
}

// A light's dword `d` (its first dword `base`).
Id ReadFAt(EmitContext& ctx, u32 handle, Id base, u32 d) {
    return ctx.OpBitcast(ctx.F32[1],
                         EmitReadConstBuffer(ctx, handle, ctx.OpIAdd(ctx.U32[1], base, ctx.ConstU32(d))));
}

// One light's visibility (its first dword in the list `base`): the setups are done.
Id LightVisibilityAt(EmitContext& ctx, Id base, Id ndc, Id uv, Id depth, bool hero) {
    auto& s = ctx.vr_light_shadow;
    const auto& info = ctx.info.vr_light_shadow;
    const Id centre_raw = ctx.OpCompositeConstruct(
        ctx.F32[3], std::array{ReadFAt(ctx, info.light_buffer, base, 0),
                               ReadFAt(ctx, info.light_buffer, base, 1),
                               ReadFAt(ctx, info.light_buffer, base, 2)});
    // Tests (body_shadow.h ProbeAt): its class, muted or moved for its shadows.
    Id probe_class{}, probe_muted = ctx.false_value;
    Id centre = centre_raw;
    if (Sirit::ValidId(s.probe_mask)) {
        probe_class = ProbeClass(ctx, ReadFAt(ctx, info.light_buffer, base, 7));
        probe_muted = ctx.OpINotEqual(ctx.U1[1], ctx.OpBitwiseAnd(ctx.U32[1], s.probe_mask, probe_class),
                                      ctx.u32_zero_value);
        const Id moved = ctx.OpINotEqual(ctx.U1[1], ctx.OpBitwiseAnd(ctx.U32[1], s.probe_sel, probe_class),
                                         ctx.u32_zero_value);
        centre = ctx.OpFAdd(ctx.F32[3], centre_raw,
                            ctx.OpVectorTimesScalar(ctx.F32[3], s.probe_nudge,
                                                    ctx.OpSelect(ctx.F32[1], moved, ctx.ConstF32(1.0f),
                                                                 ctx.ConstF32(0.0f))));
    }
    // This eye's view to the other's: the light's position in the other eye's list less this
    // eye's (light_shadow.h CrossEye).
    Id delta{};
    if (s.other_ready) {
        ctx.eye_override = s.other_eye;
        const Id other = ctx.OpCompositeConstruct(
            ctx.F32[3], std::array{ReadFAt(ctx, info.light_buffer, base, 0),
                                   ReadFAt(ctx, info.light_buffer, base, 1),
                                   ReadFAt(ctx, info.light_buffer, base, 2)});
        ctx.eye_override = Id{};
        delta = ctx.OpFSub(ctx.F32[3], other, centre_raw);
    }
    // The detail by this pixel's distance: 1 within DetailNearM, 0 from DetailFarM.
    const auto mix = [&](float far_value, float near_value, Id f) {
        return ctx.OpFAdd(ctx.F32[1], ctx.ConstF32(far_value),
                          ctx.OpFMul(ctx.F32[1], ctx.ConstF32(near_value - far_value), f));
    };
    const Id detail = ctx.OpFClamp(
        ctx.F32[1],
        ctx.OpFMul(ctx.F32[1],
                   ctx.OpFSub(ctx.F32[1], ctx.ConstF32(Tune::DetailFarM), ctx.OpLength(ctx.F32[1], s.p)),
                   ctx.ConstF32(1.0f / (Tune::DetailFarM - Tune::DetailNearM))),
        ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
    // The far mode (2026-11-10, the user: "extend their range ... and vary their fidelity"):
    // 1 within DetailFarM, 0 from DetailFarthestM; `toward(v, x, far)` goes from x to v across it.
    const Id farness = ctx.OpFClamp(
        ctx.F32[1],
        ctx.OpFMul(ctx.F32[1],
                   ctx.OpFSub(ctx.F32[1], ctx.ConstF32(Tune::DetailFarthestM),
                              ctx.OpLength(ctx.F32[1], s.p)),
                   ctx.ConstF32(1.0f / (Tune::DetailFarthestM - Tune::DetailFarM))),
        ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
    const auto toward = [&](float farthest, Id value) {
        return ctx.OpFAdd(ctx.F32[1], ctx.ConstF32(farthest),
                          ctx.OpFMul(ctx.F32[1], ctx.OpFSub(ctx.F32[1], value,
                                                            ctx.ConstF32(farthest)),
                                     farness));
    };
    // This pixel aims at its own point of the light's ball (soft edges, averaged by the upscaler)
    // times BallShare (0 by default: the light itself, crisp edges).
    static const float ball_share = [] {
        const char* const env = std::getenv("VR_LIGHT_BALL");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 1.0f)
                              : Tune::BallShare;
    }();
    const Id l =
        ball_share > 0.0f
            ? ctx.OpFAdd(ctx.F32[3], centre,
                         ctx.OpVectorTimesScalar(
                             ctx.F32[3], s.ball,
                             ctx.OpFMul(ctx.F32[1], ctx.ConstF32(ball_share),
                                        toward(Tune::LightFarthestM,
                                               mix(Tune::LightFarM, Tune::LightNearM, detail)))))
            : centre;
    const Id reach_k = ReadFAt(ctx, info.light_buffer, base, 3);
    const Id d = ctx.OpFSub(ctx.F32[3], l, s.p);
    const Id d2 = ctx.OpDot(ctx.F32[1], d, d);
    const Id dist = ctx.OpSqrt(ctx.F32[1], d2);
    // Clear of the hand that holds it, or of the lamp, candle holder or pyre it sits in (the
    // wider of ClearFixtureM and its reach's share), blending between them at arm's reach.
    const Id eye_distance = ctx.OpLength(ctx.F32[1], centre);
    // (By its reach: light_shadow.h ClearSmallM.)
    const Id fixture = FixtureClear(
        ctx, ctx.OpInverseSqrt(ctx.F32[1], ctx.OpFMax(ctx.F32[1], reach_k, ctx.ConstF32(1e-6f))));
    const Id held_out = SmoothStep(ctx, Tune::HeldInM, Tune::HeldOutM, eye_distance);
    const Id clear = ctx.OpFAdd(ctx.F32[1], ctx.ConstF32(Tune::ClearM),
                                ctx.OpFMul(ctx.F32[1],
                                           ctx.OpFSub(ctx.F32[1], fixture, ctx.ConstF32(Tune::ClearM)),
                                           held_out));
    const Id end = ctx.OpFMin(ctx.F32[1], ctx.OpFSub(ctx.F32[1], dist, clear),
                        ctx.ConstF32(Tune::MaxM));
    // Near the eye, lighting this pixel enough (the game's own falloff ends at reach_k * d2 = 1)
    // and far enough from it to march.
    // VR_LIGHT_SHADOW_NEAR=<m> (tests): lights farther away march too (the room's candles).
    static const float near_m = [] {
        const char* const env = std::getenv("VR_LIGHT_SHADOW_NEAR");
        return env != nullptr ? float(std::atof(env)) : Tune::NearLightM;
    }();
    // 1 within near_m - NearFadeM, 0 from near_m: the shadow fades in as you walk up to it.
    const Id fade = ctx.OpFClamp(
        ctx.F32[1],
        ctx.OpFMul(ctx.F32[1], ctx.OpFSub(ctx.F32[1], ctx.ConstF32(near_m), eye_distance),
                   ctx.ConstF32(1.0f / Tune::NearFadeM)),
        ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
    const Id near = ctx.OpFOrdGreaterThan(ctx.U1[1], fade, ctx.ConstF32(0.0f));
    const Id w = ctx.OpFSub(ctx.F32[1], ctx.ConstF32(1.0f), ctx.OpFMul(ctx.F32[1], d2, reach_k));
    const Id reaches = ctx.OpFOrdGreaterThan(ctx.U1[1], w, ctx.ConstF32(Tune::MinWeight));
    const Id here = ctx.OpFAdd(ctx.F32[2], ctx.OpFMul(ctx.F32[2], s.a,
                                                      ctx.OpCompositeConstruct(ctx.F32[2], std::array{
                                                          ctx.OpCompositeExtract(ctx.F32[1], ndc, 0u),
                                                          ctx.OpCompositeExtract(ctx.F32[1], ndc, 1u)})),
                               s.b);
    const Id count = MarchSteps(ctx, l, w, here, 1.0f);
    const Id room = ctx.OpFOrdGreaterThan(ctx.U1[1], end, ctx.ConstF32(Tune::StartM));
    // The surface faces the light (within FacingCull), or its normal cannot be trusted.
    const Id facing = ctx.OpLogicalOr(
        ctx.U1[1], ctx.OpLogicalNot(ctx.U1[1], s.normal_ok),
        ctx.OpFOrdGreaterThan(
            ctx.U1[1],
            ctx.OpDot(ctx.F32[1], s.normal,
                      ctx.OpVectorTimesScalar(ctx.F32[3], d,
                                              ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f),
                                                         ctx.OpFMax(ctx.F32[1], dist,
                                                                    ctx.ConstF32(1e-4f))))),
            ctx.ConstF32(Tune::FacingCull)));
    // Not a fill light (dimmer than fog_light.h FillLuminance or wider than light_shadow.h
    // ShadowFillReachM: the moonlight's fills, the grey light the game keeps just in front of the
    // camera). They have no source to cast from: shadowed, they threw shadows from
    // nowhere, the camera's away from the eye, against the moon's (2026-11-12, the user at the
    // first lamp: "things near me get their shadow ... opposite"). The fog skips them too.
    namespace Fills = Shader::Optimization::FogLight;
    const Id lum = ctx.OpFAdd(
        ctx.F32[1],
        ctx.OpFAdd(ctx.F32[1], ctx.OpFMul(ctx.F32[1], ctx.ConstF32(0.3f), ReadFAt(ctx, info.light_buffer, base, 4)),
                   ctx.OpFMul(ctx.F32[1], ctx.ConstF32(0.6f), ReadFAt(ctx, info.light_buffer, base, 5))),
        ctx.OpFMul(ctx.F32[1], ctx.ConstF32(0.1f), ReadFAt(ctx, info.light_buffer, base, 6)));
    const Id bright = ctx.OpFOrdGreaterThanEqual(ctx.U1[1], lum, ctx.ConstF32(Fills::FillLuminance));
    const Id narrow = ctx.OpFOrdGreaterThanEqual(
        ctx.U1[1], reach_k, ctx.ConstF32(1.0f / (Tune::ShadowFillReachM * Tune::ShadowFillReachM)));
    // A warm light casts at any brightness (a dim one only near itself); grey and blue ones when
    // bright and narrow; never the game's light on the camera (light_shadow.h WarmShare).
    const Id warm = ctx.OpFOrdGreaterThan(
        ctx.U1[1], ReadFAt(ctx, info.light_buffer, base, 4),
        ctx.OpFMul(ctx.F32[1], ctx.ConstF32(Tune::WarmShare), ReadFAt(ctx, info.light_buffer, base, 6)));
    const Id warm_casts = ctx.OpLogicalAnd(
        ctx.U1[1], warm,
        ctx.OpLogicalAnd(
            ctx.U1[1], ctx.OpFOrdGreaterThanEqual(ctx.U1[1], lum, ctx.ConstF32(Tune::DimMinLuminance)),
            ctx.OpFOrdGreaterThanEqual(ctx.U1[1], reach_k,
                                       ctx.ConstF32(1.0f / (Tune::WarmReachM * Tune::WarmReachM)))));
    const Id off_camera = ctx.OpFOrdGreaterThan(ctx.U1[1], eye_distance, ctx.ConstF32(Tune::HeldInM));
    const Id old_rule = ctx.OpLogicalAnd(ctx.U1[1], narrow, bright);
    // The strength of its shadows (light_shadow.h DimFadeLuminance: nothing switches).
    const auto ramp = [&](Id x, float from, float to) {
        return ctx.OpFClamp(ctx.F32[1],
                            ctx.OpFMul(ctx.F32[1], ctx.OpFSub(ctx.F32[1], x, ctx.ConstF32(from)),
                                       ctx.ConstF32(1.0f / (to - from))),
                            ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
    };
    // A warm light: in over DimFadeLuminance; the share of its reach growing with its brightness,
    // fading out over DimBand of the falloff before the share's end.
    const Id share = ctx.OpFAdd(
        ctx.F32[1], ctx.ConstF32(Tune::DimNearShare),
        ctx.OpFMul(ctx.F32[1], ctx.ConstF32(1.0f - Tune::DimNearShare),
                   ramp(lum, Tune::DimShareLuminance, Fills::FillLuminance)));
    const Id w_min = ctx.OpFSub(ctx.F32[1], ctx.ConstF32(1.0f), ctx.OpFMul(ctx.F32[1], share, share));
    const Id warm_strength = ctx.OpFMul(
        ctx.F32[1],
        ramp(lum, Tune::DimMinLuminance, Tune::DimMinLuminance + Tune::DimFadeLuminance),
        ctx.OpFClamp(ctx.F32[1],
                     ctx.OpFMul(ctx.F32[1], ctx.OpFSub(ctx.F32[1], w, w_min),
                                ctx.ConstF32(1.0f / Tune::DimBand)),
                     ctx.ConstF32(0.0f), ctx.ConstF32(1.0f)));
    // A grey or blue one: narrow, off the camera, in over BrightFadeLuminance; one the game spawned
    // (its dword 7 under FogLight::MapLightWord: a carried lantern) in full from
    // BrightKeepLuminance (light_shadow.h SpawnedFadeLuminance), so its flicker does not swing it.
    const Id spawned = ctx.OpFOrdLessThan(ctx.U1[1], ReadFAt(ctx, info.light_buffer, base, 7),
                                          ctx.ConstF32(Fills::MapLightWord));
    const Id grey_from =
        ctx.OpSelect(ctx.F32[1], spawned,
                     ctx.ConstF32(Fills::BrightKeepLuminance - Tune::SpawnedFadeLuminance),
                     ctx.ConstF32(Fills::FillLuminance - Tune::BrightFadeLuminance));
    const Id grey_scale = ctx.OpSelect(ctx.F32[1], spawned, ctx.ConstF32(1.0f / Tune::SpawnedFadeLuminance),
                                       ctx.ConstF32(1.0f / Tune::BrightFadeLuminance));
    const Id grey_ramp =
        ctx.OpFClamp(ctx.F32[1], ctx.OpFMul(ctx.F32[1], ctx.OpFSub(ctx.F32[1], lum, grey_from), grey_scale),
                     ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
    const Id grey_strength = ctx.OpSelect(
        ctx.F32[1], ctx.OpLogicalAnd(ctx.U1[1], narrow, off_camera), grey_ramp, ctx.ConstF32(0.0f));
    const Id strength =
        Tune::OldShadows() ? ctx.ConstF32(1.0f)
        : Tune::OldRule()
            ? ctx.OpSelect(ctx.F32[1], old_rule, ctx.ConstF32(1.0f), ctx.ConstF32(0.0f))
            : ctx.OpSelect(ctx.F32[1], warm_casts, warm_strength,
                           ctx.OpSelect(ctx.F32[1], warm, ctx.ConstF32(0.0f), grey_strength));
    // The game's shadowed lamps (Info::VrLightShadow::game_shadowed): each casts in full, whatever
    // its colour or brightness (the game marked it shadowed; its own map is gone).
    const Id cast_strength = ctx.info.vr_light_shadow.game_shadowed ? ctx.ConstF32(1.0f) : strength;
    // ... and never a light the game keeps on the camera (light_shadow.h AttachedFromM) or a
    // NoCastLights one (the same slots' test).
    const Attached slot = AttachedLight(ctx, centre_raw, reach_k);
    const Id on_camera_light = slot.any;
    Id casts = ctx.OpLogicalAnd(
        ctx.U1[1], ctx.OpFOrdGreaterThan(ctx.U1[1], cast_strength, ctx.ConstF32(0.0f)),
        ctx.OpLogicalNot(ctx.U1[1], on_camera_light));
    if (Sirit::ValidId(s.probe_mask)) {
        casts = ctx.OpLogicalAnd(ctx.U1[1], casts, ctx.OpLogicalNot(ctx.U1[1], probe_muted));
    }
    // (All of them: light_shadow.h ReachOld; this evening a small one took half.)
    const Id steps = !Tune::ReachOld() ? count : ctx.OpSelect(
        ctx.U32[1],
        ctx.OpFOrdLessThanEqual(ctx.U1[1], reach_k,
                                ctx.ConstF32(1.0f / (Tune::SmallReachM * Tune::SmallReachM))),
        count,
        ctx.OpIAdd(ctx.U32[1], ctx.OpShiftRightLogical(ctx.U32[1], count, ctx.ConstU32(1u)),
                   ctx.ConstU32(2u)));
    const Id march = ctx.OpLogicalAnd(
        ctx.U1[1], ctx.OpLogicalAnd(ctx.U1[1], ctx.OpLogicalAnd(ctx.U1[1], s.ok, facing), casts),
        ctx.OpLogicalAnd(ctx.U1[1], near, ctx.OpLogicalAnd(ctx.U1[1], reaches, room)));
    // Grouped (light_shadow.h GroupM): a small light this close to an earlier small one of this
    // shader that marched takes its visibility instead of a march of its own.
    const Id small = ctx.OpFOrdGreaterThanEqual(
        ctx.U1[1], reach_k, ctx.ConstF32(1.0f / (Tune::SmallReachM * Tune::SmallReachM)));
    Id grouped = ctx.false_value;
    Id group_vis = ctx.ConstF32(1.0f);
    if (Tune::GroupM() > 0.0f && !s.loop_ready && !hero) {
        const Id mine = ctx.OpLogicalAnd(ctx.U1[1], march, small);
        for (const auto& g : s.grouped) {
            const Id close = ctx.OpFOrdLessThan(
                ctx.U1[1], ctx.OpLength(ctx.F32[1], ctx.OpFSub(ctx.F32[3], centre_raw, g.centre)),
                ctx.ConstF32(Tune::GroupM()));
            const Id take = ctx.OpLogicalAnd(
                ctx.U1[1], ctx.OpLogicalAnd(ctx.U1[1], mine, ctx.OpLogicalNot(ctx.U1[1], grouped)),
                ctx.OpLogicalAnd(ctx.U1[1], ctx.OpLogicalAnd(ctx.U1[1], g.small, g.marched), close));
            group_vis = ctx.OpSelect(ctx.F32[1], take, g.vis, group_vis);
            grouped = ctx.OpLogicalOr(ctx.U1[1], grouped, take);
        }
    }
    // The scene maps (light_shadow.h SceneMaps): a lamp that is a slot's (its place within MatchM
    // in the flat view, its reach within 2%) takes its cube's test instead of a march. (Not High's
    // hero: it marches; its programs read no cubes.)
    Id mapped = ctx.false_value;
    Id mapped_slot = ctx.u32_zero_value;
    if (Tune::SceneMaps::Enabled() && !hero) {
        SceneSetup(ctx);
        if (Sirit::ValidId(s.scene_count)) {
            const Id flat_centre = ToWorld(ctx, ctx.OpFAdd(ctx.F32[3], centre_raw, s.scene_shift));
            const Id found = Private(ctx, ctx.U32[1]); // the slot + 1 (0: none)
            ctx.OpStore(found, ctx.u32_zero_value);
            LoopCount(ctx, s.scene_count, [&](Id k) {
                const SlotRead slot = SceneSlot(ctx, k);
                const Id lamp = slot.lamp;
                const Id k_at = slot.reach_k;
                const Id same = ctx.OpLogicalAnd(
                    ctx.U1[1],
                    ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpLength(ctx.F32[1], ctx.OpFSub(ctx.F32[3], flat_centre, lamp)),
                                       ctx.ConstF32(Tune::SceneMaps::MatchM)),
                    ctx.OpFOrdLessThanEqual(ctx.U1[1], ctx.OpFAbs(ctx.F32[1], ctx.OpFSub(ctx.F32[1], reach_k, k_at)),
                                            ctx.OpFMul(ctx.F32[1], ctx.ConstF32(0.02f), ctx.OpFAbs(ctx.F32[1], k_at))));
                const Id cur = ctx.OpLoad(ctx.U32[1], found);
                ctx.OpStore(found, ctx.OpSelect(ctx.U32[1],
                                                ctx.OpLogicalAnd(ctx.U1[1], same, ctx.OpIEqual(ctx.U1[1], cur, ctx.u32_zero_value)),
                                                ctx.OpIAdd(ctx.U32[1], k, ctx.ConstU32(1u)), cur));
            });
            const Id got = ctx.OpLoad(ctx.U32[1], found);
            mapped = ctx.OpLogicalAnd(ctx.U1[1], ctx.OpINotEqual(ctx.U1[1], got, ctx.u32_zero_value), march);
            mapped_slot = ctx.OpISub(ctx.U32[1], ctx.OpUMax(ctx.U32[1], got, ctx.ConstU32(1u)), ctx.ConstU32(1u));
        }
    }
    const Id march_own = ctx.OpLogicalAnd(
        ctx.U1[1], ctx.OpLogicalAnd(ctx.U1[1], march, ctx.OpLogicalNot(ctx.U1[1], grouped)),
        ctx.OpLogicalNot(ctx.U1[1], mapped));
    ctx.OpStore(s.vis, group_vis);
    if (Tune::SceneMaps::Enabled() && Sirit::ValidId(s.scene_count) && !hero) {
        const Id map_label = ctx.OpLabel(), map_done = ctx.OpLabel();
        ctx.OpSelectionMerge(map_done, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(mapped, map_label, map_done);
        ctx.AddLabel(map_label);
        {
            const Id flat_p = ctx.OpFAdd(ctx.F32[3], s.p, s.scene_shift);
            const Id flat_l = ctx.OpFAdd(ctx.F32[3], centre_raw, s.scene_shift);
            ctx.OpStore(s.vis, ctx.OpFSub(ctx.F32[1], ctx.ConstF32(1.0f),
                                          SceneOcclusion(ctx, mapped_slot, ToWorld(ctx, ctx.OpFSub(ctx.F32[3], flat_p, flat_l)),
                                                         ctx.OpIMul(ctx.U32[1], s.scene_half, ctx.ConstU32(Tune::SceneMaps::Texels)),
                                                         ToWorld(ctx, s.normal))));
        }
        ctx.OpBranch(map_done);
        ctx.AddLabel(map_done);
    }
    // (Maps only: no march code at all; a lamp outside the slots casts none.)
    if (hero || !(Tune::SceneMaps::Enabled() && Tune::SceneMaps::Only() && Sirit::ValidId(s.scene_count))) {
        const Id then_label = ctx.OpLabel(), endif = ctx.OpLabel();
        ctx.OpSelectionMerge(endif, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(march_own, then_label, endif);
        ctx.AddLabel(then_label);
        ctx.OpStore(s.vis, March(ctx, l, dist, end, steps, delta));
        ctx.OpBranch(endif);
        ctx.AddLabel(endif);
    }
    // The hunter's own shadow (body_shadow.h): his capsules on the way to a lamp near him.
    if (Shader::Optimization::BodyShadow::Enabled()) {
        const auto& buffers = ctx.info.buffers;
        const auto found = std::ranges::find(buffers, BufferType::VrBodyCapsules,
                                             &BufferResource::buffer_type);
        if (found != buffers.end()) {
            const u32 buf = static_cast<u32>(std::distance(buffers.begin(), found));
            const auto maps_found =
                std::ranges::find(buffers, BufferType::VrBodyMaps, &BufferResource::buffer_type);
            const u32 maps = maps_found == buffers.end()
                                 ? ~0u
                                 : static_cast<u32>(std::distance(buffers.begin(), maps_found));
            // (Grouped: the earlier light's visibility has his body in it already.)
            const Id lit_by = ctx.OpLogicalAnd(
                ctx.U1[1],
                ctx.OpLogicalAnd(ctx.U1[1], ctx.OpLogicalAnd(ctx.U1[1], s.ok, casts),
                                 ctx.OpLogicalNot(ctx.U1[1], grouped)),
                ctx.OpLogicalAnd(ctx.U1[1], near, reaches));
            const Id body_label = ctx.OpLabel(), body_done = ctx.OpLabel();
            ctx.OpSelectionMerge(body_done, spv::SelectionControlMask::MaskNone);
            ctx.OpBranchConditional(lit_by, body_label, body_done);
            ctx.AddLabel(body_label);
            ctx.OpStore(s.vis, ctx.OpFMin(ctx.F32[1], ctx.OpLoad(ctx.F32[1], s.vis),
                                          BodyVisibility(ctx, buf, maps, s.p, l)));
            ctx.OpBranch(body_done);
            ctx.AddLabel(body_done);
        }
    }
    // For the lights after it (grouping): what this one found.
    if (Tune::GroupM() > 0.0f && !s.loop_ready && !hero) {
        s.grouped.push_back({centre_raw, small, ctx.OpLogicalOr(ctx.U1[1], march_own, grouped),
                             ctx.OpLoad(ctx.F32[1], s.vis)});
    }
    // (Not a number: lit.)
    const Id result = ctx.OpFClamp(
        ctx.F32[1],
        Finite(ctx,
               ctx.OpFAdd(ctx.F32[1], ctx.ConstF32(1.0f),
                                  ctx.OpFMul(ctx.F32[1],
                                             ctx.OpFSub(ctx.F32[1], ctx.OpLoad(ctx.F32[1], s.vis),
                                                        ctx.ConstF32(1.0f)),
                                             ctx.OpFMul(ctx.F32[1], fade, cast_strength))),
               ctx.ConstF32(1.0f)),
        ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
    // The game's fill light ahead of the camera, removed (vr.ini camera_fill_light off).
    if (Sirit::ValidId(s.attached_remove)) {
        return ctx.OpSelect(ctx.F32[1], ctx.OpLogicalAnd(ctx.U1[1], slot.camera, s.attached_remove),
                            ctx.ConstF32(0.0f), result);
    }
    return result;
}

} // namespace Shader::Backend::SPIRV

// ---- Ambient occlusion (ir/passes/ambient_occlusion.h) ----


namespace Shader::Backend::SPIRV {
namespace {

namespace AoTune = Shader::Optimization::AmbientOcclusion;

// The sectors of the slice's half-circle (Sectors bits from the normal's -90 degrees, lo, to its
// +90) that the points along dir_uv (a pixel's step in coordinates; side +1 or -1, the slice's
// signed angle from the eye direction v) hide, from the pixel here (coordinates) at view
// position s.p out to pixels: each point from its front to its back, `thick` behind it (thinner
// towards the radius' edge), within radius (metres) and beyond BiasShare of it.
Id SideMask(EmitContext& ctx, Id here, Id dir_uv, Id pixels, Id radius, Id v, float side, Id lo,
            Id thick) {
    const auto& s = ctx.vr_light_shadow;
    const auto& info = ctx.info.vr_light_shadow;
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(ctx.F32[1], a, b); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(ctx.F32[1], a, b); };
    const auto sub = [&](Id a, Id b) { return ctx.OpFSub(ctx.F32[1], a, b); };
    constexpr float Pi = 3.14159265f;
    ctx.OpStore(s.mask, ctx.u32_zero_value);
    ctx.OpStore(s.step, ctx.u32_zero_value);
    const Id header = ctx.OpLabel(), body = ctx.OpLabel(), cont = ctx.OpLabel(),
             merge = ctx.OpLabel();
    ctx.OpBranch(header);
    ctx.AddLabel(header);
    const Id j = ctx.OpLoad(ctx.U32[1], s.step);
    const Id bits = ctx.OpLoad(ctx.U32[1], s.mask);
    // The condition before the merge: OpLoopMerge must be just before the block's branch.
    const Id go = ctx.OpULessThan(ctx.U1[1], j, ctx.ConstU32(AoTune::Steps));
    ctx.OpLoopMerge(merge, cont, spv::LoopControlMask::MaskNone);
    ctx.OpBranchConditional(go, body, merge);
    ctx.AddLabel(body);
    // t = pixels ((j + jitter) / Steps)^2, at least a pixel and a half.
    const Id u = mul(add(ctx.OpConvertUToF(ctx.F32[1], j), s.seed),
                     ctx.ConstF32(1.0f / float(AoTune::Steps)));
    const Id t = ctx.OpFMax(ctx.F32[1], mul(pixels, mul(u, u)), ctx.ConstF32(1.5f));
    const Id uv = ctx.OpFAdd(ctx.F32[2], here, ctx.OpVectorTimesScalar(ctx.F32[2], dir_uv, t));
    const Id zero2 = ctx.ConstF32(0.0f, 0.0f), one2 = ctx.ConstF32(1.0f, 1.0f);
    const Id inside = ctx.OpLogicalAnd(
        ctx.U1[1], ctx.OpAll(ctx.U1[1], ctx.OpFOrdGreaterThanEqual(ctx.U1[2], uv, zero2)),
        ctx.OpAll(ctx.U1[1], ctx.OpFOrdLessThanEqual(ctx.U1[2], uv, one2)));
    const Id uvc = ctx.OpFClamp(ctx.F32[2], uv, zero2, one2);
    const Id sample = EmitImageSampleExplicitLod(ctx, nullptr, info.depth_image, uvc,
                                                 ctx.ConstF32(0.0f), IR::Value{});
    const Id ds = ctx.OpCompositeExtract(ctx.F32[1], sample, 0u);
    const Id n = ctx.OpFDiv(ctx.F32[2], ctx.OpFSub(ctx.F32[2], uvc, s.b), s.a);
    const auto p4 = Transform(ctx, s.rows, ctx.OpCompositeExtract(ctx.F32[1], n, 0u),
                              ctx.OpCompositeExtract(ctx.F32[1], n, 1u), ds);
    const Id inv_w = ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), p4[3]);
    const Id pj = ctx.OpCompositeConstruct(
        ctx.F32[3], std::array{mul(p4[0], inv_w), mul(p4[1], inv_w), mul(p4[2], inv_w)});
    const Id d = ctx.OpFSub(ctx.F32[3], pj, s.p);
    const Id l = ctx.OpSqrt(ctx.F32[1],
                            ctx.OpFMax(ctx.F32[1], ctx.OpDot(ctx.F32[1], d, d), ctx.ConstF32(1e-8f)));
    const Id near_edge = ctx.OpFOrdLessThanEqual(ctx.U1[1], l, radius);
    const Id beyond_bias =
        ctx.OpFOrdGreaterThanEqual(ctx.U1[1], l, mul(radius, ctx.ConstF32(AoTune::BiasShare)));
    // Thinner towards the radius' edge: smoothstep(0.3, 1, l / radius) of it gone.
    const Id x = ctx.OpFClamp(ctx.F32[1],
                              mul(sub(ctx.OpFDiv(ctx.F32[1], l, radius), ctx.ConstF32(0.3f)),
                                  ctx.ConstF32(1.0f / 0.7f)),
                              ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
    const Id smooth = mul(mul(x, x), sub(ctx.ConstF32(3.0f), mul(ctx.ConstF32(2.0f), x)));
    const Id depth_m = mul(thick, sub(ctx.ConstF32(1.0f), smooth));
    const Id back = ctx.OpFSub(ctx.F32[3], d, ctx.OpVectorTimesScalar(ctx.F32[3], v, depth_m));
    const auto angle = [&](Id w, Id length) {
        const Id c = ctx.OpFClamp(
            ctx.F32[1],
            ctx.OpFDiv(ctx.F32[1], ctx.OpDot(ctx.F32[1], w, v),
                       ctx.OpFMax(ctx.F32[1], length, ctx.ConstF32(1e-4f))),
            ctx.ConstF32(-1.0f), ctx.ConstF32(1.0f));
        return mul(ctx.ConstF32(side), ctx.OpAcos(ctx.F32[1], c));
    };
    const Id front_a = angle(d, l);
    const Id back_a = angle(back, ctx.OpLength(ctx.F32[1], back));
    const auto sector = [&](Id a) {
        return mul(ctx.OpFClamp(ctx.F32[1], mul(sub(a, lo), ctx.ConstF32(1.0f / Pi)),
                                ctx.ConstF32(0.0f), ctx.ConstF32(1.0f)),
                   ctx.ConstF32(float(AoTune::Sectors)));
    };
    const Id s0 = sector(ctx.OpFMin(ctx.F32[1], front_a, back_a));
    const Id s1 = sector(ctx.OpFMax(ctx.F32[1], front_a, back_a));
    const Id first = ctx.OpConvertFToU(ctx.U32[1], add(s0, ctx.ConstF32(0.5f)));
    const Id last = ctx.OpConvertFToU(ctx.U32[1], add(s1, ctx.ConstF32(0.5f)));
    const Id count = ctx.OpISub(ctx.U32[1], last, first);
    // count bits from `first` (all 32 when count is 32; shifts kept under 32).
    const Id ones = ctx.OpSelect(
        ctx.U32[1], ctx.OpUGreaterThanEqual(ctx.U1[1], count, ctx.ConstU32(32u)),
        ctx.ConstU32(0xFFFFFFFFu),
        ctx.OpISub(ctx.U32[1],
                   ctx.OpShiftLeftLogical(ctx.U32[1], ctx.ConstU32(1u),
                                          ctx.OpUMin(ctx.U32[1], count, ctx.ConstU32(31u))),
                   ctx.ConstU32(1u)));
    const Id hidden = ctx.OpShiftLeftLogical(ctx.U32[1], ones,
                                             ctx.OpUMin(ctx.U32[1], first, ctx.ConstU32(31u)));
    const Id counts = ctx.OpLogicalAnd(
        ctx.U1[1], ctx.OpLogicalAnd(ctx.U1[1], inside, ctx.OpLogicalAnd(ctx.U1[1], near_edge,
                                                                        beyond_bias)),
        ctx.OpLogicalAnd(ctx.U1[1], ctx.OpUGreaterThan(ctx.U1[1], count, ctx.u32_zero_value),
                         ctx.OpULessThan(ctx.U1[1], first, ctx.ConstU32(32u))));
    ctx.OpStore(s.mask, ctx.OpBitwiseOr(ctx.U32[1], bits,
                                        ctx.OpSelect(ctx.U32[1], counts, hidden,
                                                     ctx.u32_zero_value)));
    ctx.OpStore(s.step, ctx.OpIAdd(ctx.U32[1], j, ctx.ConstU32(1u)));
    ctx.OpBranch(cont);
    ctx.AddLabel(cont);
    ctx.OpBranch(header);
    ctx.AddLabel(merge);
    return ctx.OpLoad(ctx.U32[1], s.mask);
}

} // namespace

// GTAO: per slice, the two horizons around the pixel and the cosine-weighted visible arc between
// them about the normal (rebuilt from the view position's derivatives), averaged over Slices.
Id EmitVrAmbientOcclusion(EmitContext& ctx, Id ndc, Id uv, Id depth) {
    if (!ctx.info.vr_light_shadow.on) {
        return ctx.ConstF32(1.0f);
    }
    auto& s = ctx.vr_light_shadow;
    // The lamp shadows' work here only with them on (RuntimeInfo::VrAoLampShadows: the scene maps
    // and the mask are on by default since 2026-11-17, a session without lamp shadows skips it).
    const bool lamps = Tune::SceneMaps::Enabled() &&
                       (ctx.runtime_info.vr_ambient_occlusion & RuntimeInfo::VrAoLampShadows) != 0;
    if (!s.ready) {
        Setup(ctx, ndc, uv, depth, lamps && Tune::SceneMaps::Mask()); // (the mask: the grid's in-view test projects)
    }
    // The scene maps (light_shadow.h SceneMaps): this pixel into the slots' cubes first.
    if (lamps) {
        if (Tune::SceneMaps::Mask()) {
            SceneSetup(ctx);
            GridSetup(ctx); // (the grid's upkeep: here, once a frame, not in every lamp draw)
        }
        // (The old march in the mask reads no cubes: nothing splatted into them.)
        if (!(Tune::SceneMaps::Mask() && Tune::SceneMaps::MarchMode() && Tune::SceneMaps::TraceAll())) {
            SceneScatter(ctx);
            SceneGridSplat(ctx);
        }
        SceneMaskWrite(ctx);
    }
    // Our AO off (RuntimeInfo::VrAoMaskOnly): the mask only; 1 here keeps the game's AO.
    if ((ctx.runtime_info.vr_ambient_occlusion & RuntimeInfo::VrAoMaskOnly) != 0) {
        if (Tune::SceneMaps::MaskShow() && Sirit::ValidId(s.mask_min)) {
            return ctx.OpLoad(ctx.F32[1], s.mask_min);
        }
        return ctx.ConstF32(1.0f);
    }
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(ctx.F32[1], a, b); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(ctx.F32[1], a, b); };
    const auto sub = [&](Id a, Id b) { return ctx.OpFSub(ctx.F32[1], a, b); };
    const Id v = ctx.OpNormalize(ctx.F32[3], ctx.OpFNegate(ctx.F32[3], s.p));
    Id normal = ctx.OpNormalize(ctx.F32[3], ctx.OpCross(ctx.F32[3], ctx.OpDPdy(ctx.F32[3], s.p),
                                                         ctx.OpDPdx(ctx.F32[3], s.p)));
    normal = ctx.OpSelect(ctx.F32[3],
                          ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpDot(ctx.F32[1], normal, v),
                                             ctx.ConstF32(0.0f)),
                          ctx.OpFNegate(ctx.F32[3], normal), normal);
    // The radius on screen: RadiusM over the metres a pixel covers here (the view position's
    // change across a pixel).
    const Id here = ctx.OpFAdd(ctx.F32[2], ctx.OpFMul(ctx.F32[2], s.a, ndc), s.b);
    // The game's own normal buffer where the pass found it (ambient_occlusion.h BufferNormals):
    // world space in 0..1, turned into the view by the matrix buffer's rotation rows. A texel
    // that holds no direction keeps the derivatives' normal.
    if (ctx.info.vr_light_shadow.normal_image != ~0u) {
        const Id texel4 = EmitImageSampleExplicitLod(ctx, nullptr, ctx.info.vr_light_shadow.normal_image, here,
                                                     ctx.ConstF32(0.0f), IR::Value{});
        const Id texel = ctx.OpCompositeConstruct(
            ctx.F32[3], std::array{ctx.OpCompositeExtract(ctx.F32[1], texel4, 0u),
                                   ctx.OpCompositeExtract(ctx.F32[1], texel4, 1u),
                                   ctx.OpCompositeExtract(ctx.F32[1], texel4, 2u)});
        const Id world = ctx.OpFSub(ctx.F32[3], ctx.OpVectorTimesScalar(ctx.F32[3], texel, ctx.ConstF32(2.0f)),
                                    ctx.ConstF32(1.0f, 1.0f, 1.0f));
        std::array<Id, 3> turned;
        for (u32 r = 0; r < 3; ++r) {
            const u32 d = AoTune::RotationDwords[r];
            const Id row = ctx.OpCompositeConstruct(
                ctx.F32[3], std::array{ReadF(ctx, ctx.info.vr_light_shadow.matrix_buffer, d),
                                       ReadF(ctx, ctx.info.vr_light_shadow.matrix_buffer, d + 1),
                                       ReadF(ctx, ctx.info.vr_light_shadow.matrix_buffer, d + 2)});
            turned[r] = ctx.OpDot(ctx.F32[1], row, world);
        }
        const Id view_n = ctx.OpCompositeConstruct(ctx.F32[3], turned);
        const Id length = ctx.OpLength(ctx.F32[1], view_n);
        const Id usable = ctx.OpLogicalAnd(
            ctx.U1[1], ctx.OpFOrdGreaterThan(ctx.U1[1], length, ctx.ConstF32(0.5f)),
            ctx.OpFOrdLessThan(ctx.U1[1], length, ctx.ConstF32(1.5f)));
        normal = ctx.OpSelect(
            ctx.F32[3], usable,
            ctx.OpVectorTimesScalar(ctx.F32[3], view_n,
                                    ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f),
                                               ctx.OpFMax(ctx.F32[1], length, ctx.ConstF32(1e-4f)))),
            normal);
    }
    // The search's noise: the pixel's own, the same every frame (ambient_occlusion.h FrameNoise).
    const Id frame_seed = s.seed;
    if (!AoTune::FrameNoise() && Sirit::ValidId(s.seed_still)) {
        s.seed = s.seed_still;
    }
    const Id footprint = ctx.OpFMax(
        ctx.F32[1],
        ctx.OpFMax(ctx.F32[1], ctx.OpLength(ctx.F32[1], ctx.OpDPdx(ctx.F32[3], s.p)),
                   ctx.OpLength(ctx.F32[1], ctx.OpDPdy(ctx.F32[3], s.p))),
        ctx.ConstF32(1e-5f));
    constexpr float radius_m = AoTune::RadiusM;
    constexpr float thick_m = AoTune::ThicknessM;
    constexpr float power = AoTune::Power;
    const Id raw_px = ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(radius_m), footprint);
    const Id pixels = ctx.OpFClamp(ctx.F32[1], raw_px, ctx.ConstF32(AoTune::MinPixels),
                                   ctx.ConstF32(AoTune::MaxPixels));
    // The radius in metres that many pixels cover here (a clamped search shrinks its falloff too).
    const Id radius =
        mul(ctx.ConstF32(radius_m),
            ctx.OpFDiv(ctx.F32[1], pixels, ctx.OpFMax(ctx.F32[1], raw_px, ctx.ConstF32(1e-3f))));
    // How thick a surface counts (the bitmask's), more for farther pixels.
    const Id thick = add(ctx.ConstF32(thick_m),
                         mul(ctx.ConstF32(AoTune::ThicknessPerM), ctx.OpLength(ctx.F32[1], s.p)));
    constexpr float Pi = 3.14159265f;
    Id total = ctx.ConstF32(0.0f);
    for (u32 k = 0; k < AoTune::Slices; ++k) {
        const Id phi =
            mul(add(ctx.ConstF32(float(k)), s.seed), ctx.ConstF32(Pi / float(AoTune::Slices)));
        const Id dir_uv = ctx.OpFMul(
            ctx.F32[2],
            ctx.OpCompositeConstruct(ctx.F32[2], std::array{ctx.OpCos(ctx.F32[1], phi),
                                                            ctx.OpSin(ctx.F32[1], phi)}),
            s.pixel);
        // The slice's direction in view space: a point a few pixels along it at this depth.
        const Id e = ctx.OpFAdd(ctx.F32[2], here,
                                ctx.OpVectorTimesScalar(ctx.F32[2], dir_uv, ctx.ConstF32(4.0f)));
        const Id en = ctx.OpFDiv(ctx.F32[2], ctx.OpFSub(ctx.F32[2], e, s.b), s.a);
        const auto e4 = Transform(ctx, s.rows, ctx.OpCompositeExtract(ctx.F32[1], en, 0u),
                                  ctx.OpCompositeExtract(ctx.F32[1], en, 1u), depth);
        const Id e_inv = ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), e4[3]);
        const Id pe = ctx.OpCompositeConstruct(
            ctx.F32[3], std::array{mul(e4[0], e_inv), mul(e4[1], e_inv), mul(e4[2], e_inv)});
        const Id omega = ctx.OpFSub(ctx.F32[3], pe, s.p);
        const Id ortho = ctx.OpNormalize(
            ctx.F32[3],
            ctx.OpFSub(ctx.F32[3], omega,
                       ctx.OpVectorTimesScalar(ctx.F32[3], v, ctx.OpDot(ctx.F32[1], omega, v))));
        const Id axis = ctx.OpNormalize(ctx.F32[3], ctx.OpCross(ctx.F32[3], ortho, v));
        const Id proj_n = ctx.OpFSub(
            ctx.F32[3], normal,
            ctx.OpVectorTimesScalar(ctx.F32[3], axis, ctx.OpDot(ctx.F32[1], normal, axis)));
        const Id proj_len =
            ctx.OpFMax(ctx.F32[1], ctx.OpLength(ctx.F32[1], proj_n), ctx.ConstF32(1e-4f));
        const Id cos_n = ctx.OpFClamp(
            ctx.F32[1], ctx.OpFDiv(ctx.F32[1], ctx.OpDot(ctx.F32[1], proj_n, v), proj_len),
            ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
        const Id sign_n = ctx.OpFSign(ctx.F32[1], ctx.OpDot(ctx.F32[1], ortho, proj_n));
        const Id n_ang = mul(sign_n, ctx.OpAcos(ctx.F32[1], cos_n));
        // The visibility bitmask (ambient_occlusion.h): both sides' hidden sectors, the open
        // share of the half-circle about the normal, cosine-weighted.
        const Id lo = sub(n_ang, ctx.ConstF32(0.5f * Pi));
        const Id mask = ctx.OpBitwiseOr(
            ctx.U32[1],
            SideMask(ctx, here, ctx.OpFNegate(ctx.F32[2], dir_uv), pixels, radius, v, -1.0f, lo,
                     thick),
            SideMask(ctx, here, dir_uv, pixels, radius, v, 1.0f, lo, thick));
        Id open = ctx.ConstF32(1.0f);
        for (u32 b = 0; b < AoTune::Sectors; ++b) {
            const float a0 = -0.5f * Pi + Pi * float(b) / float(AoTune::Sectors);
            const float a1 = -0.5f * Pi + Pi * float(b + 1) / float(AoTune::Sectors);
            const float weight = 0.5f * (std::sin(a1) - std::sin(a0));
            const Id bit = ctx.OpBitFieldUExtract(ctx.U32[1], mask, ctx.ConstU32(b), ctx.ConstU32(1u));
            open = sub(open, mul(ctx.OpConvertUToF(ctx.F32[1], bit), ctx.ConstF32(weight)));
        }
        total = add(total, ctx.OpFClamp(ctx.F32[1], open, ctx.ConstF32(0.0f), ctx.ConstF32(1.0f)));
    }
    s.seed = frame_seed;
    const Id visible = ctx.OpFClamp(ctx.F32[1], mul(total, ctx.ConstF32(1.0f / float(AoTune::Slices))),
                                    ctx.ConstF32(1e-4f), ctx.ConstF32(1.0f));
    if (Tune::SceneMaps::MaskShow() && Sirit::ValidId(s.mask_min)) {
        return ctx.OpLoad(ctx.F32[1], s.mask_min); // (tests: the shadow mask as the AO)
    }
    return ctx.OpPow(ctx.F32[1], visible, ctx.ConstF32(power));
}



} // namespace Shader::Backend::SPIRV

// ---- Bounce light (ir/passes/bounce_light.h) ----

namespace Shader::Backend::SPIRV {
namespace {

namespace BounceTune = Shader::Optimization::BounceLight;

Id SampleRgb(EmitContext& ctx, u32 image, Id uv) {
    const Id sample = EmitImageSampleExplicitLod(ctx, nullptr, image, uv, ctx.ConstF32(0.0f),
                                                 IR::Value{});
    return ctx.OpCompositeConstruct(ctx.F32[3],
                                    std::array{ctx.OpCompositeExtract(ctx.F32[1], sample, 0u),
                                               ctx.OpCompositeExtract(ctx.F32[1], sample, 1u),
                                               ctx.OpCompositeExtract(ctx.F32[1], sample, 2u)});
}

// (Below, with the light in the air: the surface's normal on this pixel's side of an edge.)
Id SurfaceNormal(EmitContext& ctx, Id ndc, Id dir);

} // namespace

// One bounce: Samples points on a spiral round the pixel, each one's lit diffuse light (it leaves
// that surface) weighted by the cosine to this pixel's normal and fading with distance, times
// this pixel's surface colour.
Id EmitVrBounceLight(EmitContext& ctx, Id ndc, Id uv, Id depth) {
    if (!ctx.info.vr_light_shadow.on) {
        return ctx.ConstF32(0.0f, 0.0f, 0.0f, 0.0f);
    }
    auto& s = ctx.vr_light_shadow;
    if (!s.ready) {
        Setup(ctx, ndc, uv, depth, false);
    }
    const auto& info = ctx.info.vr_light_shadow;
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(ctx.F32[1], a, b); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(ctx.F32[1], a, b); };
    const Id v = ctx.OpNormalize(ctx.F32[3], ctx.OpFNegate(ctx.F32[3], s.p));
    Id normal = ctx.OpNormalize(ctx.F32[3], ctx.OpCross(ctx.F32[3], ctx.OpDPdy(ctx.F32[3], s.p),
                                                         ctx.OpDPdx(ctx.F32[3], s.p)));
    normal = ctx.OpSelect(ctx.F32[3],
                          ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpDot(ctx.F32[1], normal, v),
                                             ctx.ConstF32(0.0f)),
                          ctx.OpFNegate(ctx.F32[3], normal), normal);
    const Id here = ctx.OpFAdd(ctx.F32[2], ctx.OpFMul(ctx.F32[2], s.a, ndc), s.b);
    const Id footprint = ctx.OpFMax(
        ctx.F32[1],
        ctx.OpFMax(ctx.F32[1], ctx.OpLength(ctx.F32[1], ctx.OpDPdx(ctx.F32[3], s.p)),
                   ctx.OpLength(ctx.F32[1], ctx.OpDPdy(ctx.F32[3], s.p))),
        ctx.ConstF32(1e-5f));
    const Id raw_px = ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(BounceTune::RadiusM), footprint);
    const Id pixels = ctx.OpFClamp(ctx.F32[1], raw_px, ctx.ConstF32(BounceTune::MinPixels),
                                   ctx.ConstF32(BounceTune::MaxPixels));
    const Id radius = mul(ctx.ConstF32(BounceTune::RadiusM),
                          ctx.OpFDiv(ctx.F32[1], pixels,
                                     ctx.OpFMax(ctx.F32[1], raw_px, ctx.ConstF32(1e-3f))));
    // if (cond) { body }: the condition comes before the merge (Sirit emits in call order).
    const auto when = [&](Id cond, auto&& body) {
        const Id then_label = ctx.OpLabel(), endif = ctx.OpLabel();
        ctx.OpSelectionMerge(endif, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(cond, then_label, endif);
        ctx.AddLabel(then_label);
        body();
        ctx.OpBranch(endif);
        ctx.AddLabel(endif);
    };
    // The bounce is this pixel's surface colour times the light round it: a black surface (or
    // none) gets none, and gathers nothing (2026-11-10: the same picture, less work).
    const Id albedo = SampleRgb(ctx, BounceTune::AlbedoImage, here);
    const Id coloured = ctx.OpFOrdGreaterThan(
        ctx.U1[1],
        ctx.OpFMax(ctx.F32[1], ctx.OpCompositeExtract(ctx.F32[1], albedo, 0u),
                   ctx.OpFMax(ctx.F32[1], ctx.OpCompositeExtract(ctx.F32[1], albedo, 1u),
                              ctx.OpCompositeExtract(ctx.F32[1], albedo, 2u))),
        ctx.ConstF32(1.0f / 1024.0f));
    ctx.OpStore(s.gi, ctx.ConstF32(0.0f, 0.0f, 0.0f));
    // The weights' sum: how much of the view round the pixel is surface (the further bounces).
    const Id cover_var = Private(ctx, ctx.F32[1]);
    ctx.OpStore(cover_var, ctx.ConstF32(0.0f));
    // The spiral's rotation by the jitter, once: point k is the golden angle k on from it.
    const Id spin = mul(s.seed, ctx.ConstF32(2.3999632f));
    const Id cos_s = ctx.OpCos(ctx.F32[1], spin), sin_s = ctx.OpSin(ctx.F32[1], spin);
    static const bool horizon_gather = [] {
        const char* const env = std::getenv("VR_BOUNCE_HORIZON");
        return env == nullptr || env[0] != '0';
    }();
    // One sample at screen offset `offset_px` (pixels, a 2-vector): its view position, its
    // elevation over this pixel's surface (cosine to the normal) and its falloff; `weight_of`
    // turns (elevation, falloff) into its weight; its light is gathered with that weight.
    const auto sample_at = [&](Id offset_px, auto&& weight_of) {
        const Id offset = ctx.OpFMul(ctx.F32[2], offset_px, s.pixel);
        const Id uvj = ctx.OpFAdd(ctx.F32[2], here, offset);
        const Id zero2 = ctx.ConstF32(0.0f, 0.0f), one2 = ctx.ConstF32(1.0f, 1.0f);
        const Id inside = ctx.OpLogicalAnd(
            ctx.U1[1], ctx.OpAll(ctx.U1[1], ctx.OpFOrdGreaterThanEqual(ctx.U1[2], uvj, zero2)),
            ctx.OpAll(ctx.U1[1], ctx.OpFOrdLessThanEqual(ctx.U1[2], uvj, one2)));
        const Id uvc = ctx.OpFClamp(ctx.F32[2], uvj, zero2, one2);
        const Id ds = ctx.OpCompositeExtract(
            ctx.F32[1],
            EmitImageSampleExplicitLod(ctx, nullptr, info.depth_image, uvc, ctx.ConstF32(0.0f),
                                       IR::Value{}),
            0u);
        const Id n = ctx.OpFDiv(ctx.F32[2], ctx.OpFSub(ctx.F32[2], uvc, s.b), s.a);
        const auto p4 = Transform(ctx, s.rows, ctx.OpCompositeExtract(ctx.F32[1], n, 0u),
                                  ctx.OpCompositeExtract(ctx.F32[1], n, 1u), ds);
        const Id inv_w = ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), p4[3]);
        const Id pj = ctx.OpCompositeConstruct(
            ctx.F32[3], std::array{mul(p4[0], inv_w), mul(p4[1], inv_w), mul(p4[2], inv_w)});
        const Id w = ctx.OpFSub(ctx.F32[3], pj, s.p);
        const Id d2 = ctx.OpFMax(ctx.F32[1], ctx.OpDot(ctx.F32[1], w, w), ctx.ConstF32(1e-6f));
        // (A number within -1..1: a sample that is not one counts as below every horizon.)
        const Id elevation = FiniteClamp(
            ctx, mul(ctx.OpDot(ctx.F32[1], normal, w), ctx.OpInverseSqrt(ctx.F32[1], d2)), -1.0f,
            1.0f);
        const Id fall = ctx.OpFClamp(
            ctx.F32[1],
            ctx.OpFSub(ctx.F32[1], ctx.ConstF32(1.0f),
                       ctx.OpFDiv(ctx.F32[1], d2, mul(radius, radius))),
            ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
        const Id weight = ctx.OpSelect(ctx.F32[1], inside, weight_of(elevation, fall),
                                       ctx.ConstF32(0.0f));
        ctx.OpStore(cover_var, ctx.OpFAdd(ctx.F32[1], ctx.OpLoad(ctx.F32[1], cover_var), weight));
        // Its light only when it counts (in front of the surface, within the radius).
        when(ctx.OpFOrdGreaterThan(ctx.U1[1], weight, ctx.ConstF32(0.0f)), [&] {
            // (A texel that is not a number, or negative, would spread over every pixel that
            // gathers it.)
            const Id raw_light = FiniteClamp(
                ctx,
                ctx.OpFAdd(ctx.F32[3], SampleRgb(ctx, BounceTune::DiffuseImages[0], uvc),
                           SampleRgb(ctx, BounceTune::DiffuseImages[1], uvc)),
                0.0f, BounceTune::MaxLight, 3);
            // At most SampleMaxLuminance, its colour kept (bounce_light.h: a lamp's own glow).
            static const float sample_max = [] {
                const char* const env = std::getenv("VR_BOUNCE_SAMPLE_MAX");
                return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.01f, 256.0f)
                                      : BounceTune::SampleMaxLuminance;
            }();
            const Id lum = ctx.OpDot(ctx.F32[1], raw_light, ctx.ConstF32(0.3f, 0.6f, 0.1f));
            const Id light = ctx.OpVectorTimesScalar(
                ctx.F32[3], raw_light,
                ctx.OpFMin(ctx.F32[1], ctx.ConstF32(1.0f),
                           ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(sample_max),
                                      ctx.OpFMax(ctx.F32[1], lum, ctx.ConstF32(1e-4f)))));
            ctx.OpStore(s.gi, ctx.OpFAdd(ctx.F32[3], ctx.OpLoad(ctx.F32[3], s.gi),
                                         ctx.OpVectorTimesScalar(ctx.F32[3], light, weight)));
        });
        return elevation;
    };
    // The 2x2 split (bounce_light.h QuadGather): each pixel of a 2x2 block walks one of the four
    // rays (Directions 2 x 2 sides; its place in the block picks which), so the block makes the
    // samples one pixel made alone, and the four rays' light is shared across the block: a fine
    // derivative carries a value to the horizontal, the vertical and (twice) the diagonal
    // neighbour. A neighbour's rays count by how surely it lies on this surface (soft weights,
    // QuadPlaneShare and QuadNormalPower); the rays left out are made up by the ones counted. A
    // quarter of the fetches, nothing stored, no frame history. Every pixel walks its ray, whatever
    // its own colour: its neighbours need it.
    const bool quad_mode = horizon_gather && BounceTune::QuadGather();
    if (quad_mode) {
        ctx.AddCapability(spv::Capability::DerivativeControl);
        const Id f1 = ctx.F32[1], f3 = ctx.F32[3];
        // The pixel's own normal (four depth reads round it): the derivative one above is the
        // block's, made across an edge at the block's pixels on both sides of it, so neither the
        // rays' elevations (sample_at reads `normal`) nor the same-surface weights could tell a
        // crate's corner or a railing's edge from a flat wall.
        const Id dir = ctx.OpVectorTimesScalar(
            f3, s.p,
            ctx.OpFDiv(f1, ctx.ConstF32(1.0f), ctx.OpFMax(f1, ctx.OpLength(f1, s.p), ctx.ConstF32(1e-4f))));
        normal = SurfaceNormal(ctx, ndc, dir);
        // The pixel's own window coordinate, not the game's (EmitGetAttribute scales FragCoord by
        // the native-stereo size ratio: in that product and its finite difference the rounding grew
        // with the column, and on some eye widths whole columns of blocks took their parity
        // swapped, each pixel then mixing its own values in for its neighbour's: thin streaks).
        // Unscaled it holds x + 0.5 (a coarse fragment's centre), exact, and its fine difference
        // is exactly the fragment's size.
        const auto window = [&](u32 comp) {
            return ctx.OpLoad(f1, ctx.OpAccessChain(ctx.input_f32, ctx.frag_coord, ctx.ConstU32(comp)));
        };
        const Id fx = window(0);
        const Id fy = window(1);
        // Even (0) or odd (1) along an axis in the fragments' own size (a foveated fragment
        // covers several pixels; its coordinate is its centre).
        const auto parity = [&](Id f, Id df) {
            const Id size = ctx.OpFMax(f1, ctx.OpFAbs(f1, df), ctx.ConstF32(1.0f));
            const Id col = ctx.OpFloor(f1, ctx.OpFDiv(f1, f, size));
            return ctx.OpFSub(f1, col, mul(ctx.OpFloor(f1, mul(col, ctx.ConstF32(0.5f))), ctx.ConstF32(2.0f)));
        };
        const Id px = parity(fx, ctx.OpDPdxFine(f1, fx));
        const Id py = parity(fy, ctx.OpDPdyFine(f1, fy));
        // This pixel's ray: direction py, side +1 (px 0) or -1 (px 1). And the sign that carries a
        // value from the neighbour across x (y): its fine difference is (right - left), so the
        // left pixel adds it and the right one takes it away.
        const Id sx = ctx.OpFSub(f1, ctx.ConstF32(1.0f), mul(px, ctx.ConstF32(2.0f)));
        const Id sy = ctx.OpFSub(f1, ctx.ConstF32(1.0f), mul(py, ctx.ConstF32(2.0f)));
        const Id angle = add(mul(s.seed, ctx.ConstF32(3.14159265f)),
                             mul(py, ctx.ConstF32(3.14159265f / float(BounceTune::Directions))));
        const Id line = ctx.OpCompositeConstruct(
            ctx.F32[2], std::array{ctx.OpCos(f1, angle), ctx.OpSin(f1, angle)});
        Id horizon = ctx.ConstF32(0.0f); // the surface's own plane
        for (u32 k = 0; k < BounceTune::StepsPerSide; ++k) {
            const Id share = mul(mul(add(ctx.ConstF32(float(k)), s.seed), sx),
                                 ctx.ConstF32(1.0f / float(BounceTune::StepsPerSide)));
            const Id offset_px = ctx.OpVectorTimesScalar(ctx.F32[2], line, mul(share, pixels));
            const Id seen = horizon;
            const Id e = sample_at(offset_px, [&](Id elevation, Id fall) {
                return mul(ctx.OpFMax(f1, ctx.OpFSub(f1, elevation, seen), ctx.ConstF32(0.0f)), fall);
            });
            horizon = ctx.OpFMax(f1, horizon, e);
        }
        // The neighbours' values (outside every branch: derivatives need the whole block).
        const auto across1 = [&](Id v, bool vertical) { // a scalar
            const Id d = vertical ? ctx.OpDPdyFine(f1, v) : ctx.OpDPdxFine(f1, v);
            return ctx.OpFAdd(f1, v, mul(d, vertical ? sy : sx));
        };
        const auto across3 = [&](Id v, bool vertical) { // a 3-vector
            const Id d = vertical ? ctx.OpDPdyFine(f3, v) : ctx.OpDPdxFine(f3, v);
            return ctx.OpFAdd(f3, v, ctx.OpVectorTimesScalar(f3, d, vertical ? sy : sx));
        };
        const Id gi = ctx.OpLoad(f3, s.gi);
        const Id cover = ctx.OpLoad(f1, cover_var);
        // 1 / (QuadPlaneShare of the view depth): a neighbour that far off the plane counts 1/e.
        const Id per_gap = ctx.OpFDiv(
            f1, ctx.ConstF32(1.0f),
            mul(ctx.OpFMax(f1, ctx.OpFAbs(f1, ctx.OpCompositeExtract(f1, s.p, 2u)), ctx.ConstF32(0.05f)),
                ctx.ConstF32(BounceTune::QuadPlaneShare)));
        Id gi_sum = gi, cover_sum = cover, counted = ctx.ConstF32(1.0f);
        const auto count = [&](Id n_gi, Id n_cover, Id n_p, Id n_normal) {
            const Id step = ctx.OpFSub(f3, n_p, s.p);
            // Its distance from this pixel's plane or this pixel's from its (the larger: the two
            // weigh each other the same).
            const Id gap = ctx.OpFMax(f1, ctx.OpFAbs(f1, ctx.OpDot(f1, normal, step)),
                                      ctx.OpFAbs(f1, ctx.OpDot(f1, n_normal, step)));
            Id facing = ctx.OpFClamp(f1, ctx.OpDot(f1, normal, n_normal), ctx.ConstF32(0.0f),
                                     ctx.ConstF32(1.0f));
            for (u32 k = 1; k < BounceTune::QuadNormalPower; k *= 2) {
                facing = mul(facing, facing);
            }
            // (Not a number, as beside a pixel with no surface: no weight; and its values are left
            // out by a select, since 0 times a value that is not a number is not 0.)
            const Id w = FiniteClamp(
                ctx, mul(ctx.OpExp(f1, ctx.OpFNegate(f1, mul(gap, per_gap))), facing), 0.0f, 1.0f);
            const Id some = ctx.OpFOrdGreaterThan(ctx.U1[1], w, ctx.ConstF32(0.0f));
            gi_sum = ctx.OpSelect(f3, some, ctx.OpFAdd(f3, gi_sum, ctx.OpVectorTimesScalar(f3, n_gi, w)),
                                  gi_sum);
            cover_sum = ctx.OpSelect(f1, some, add(cover_sum, mul(n_cover, w)), cover_sum);
            counted = add(counted, w);
        };
        const Id h_gi = across3(gi, false), h_cover = across1(cover, false);
        const Id h_p = across3(s.p, false), h_normal = across3(normal, false);
        count(h_gi, h_cover, h_p, h_normal);
        count(across3(gi, true), across1(cover, true), across3(s.p, true), across3(normal, true));
        count(across3(h_gi, true), across1(h_cover, true), across3(h_p, true), across3(h_normal, true));
        const Id scale = ctx.OpFDiv(f1, ctx.ConstF32(float(2 * BounceTune::Directions)), counted);
        ctx.OpStore(s.gi, FiniteClamp(ctx, ctx.OpVectorTimesScalar(f3, gi_sum, scale), 0.0f,
                                      BounceTune::MaxLight * float(2 * BounceTune::Directions), 3));
        ctx.OpStore(cover_var, FiniteClamp(ctx, mul(cover_sum, scale), 0.0f,
                                           float(2 * BounceTune::Directions * BounceTune::StepsPerSide), 1));
    }
    if (!quad_mode) when(coloured, [&] {
        if (horizon_gather) {
            // Directions lines through the pixel, turned by the jitter; on each side the samples
            // out to the radius (spaced (step + seed) / StepsPerSide), each counting only the
            // elevation it adds over the side's horizon so far.
            for (u32 d = 0; d < BounceTune::Directions; ++d) {
                const float base = 3.14159265f * float(d) / float(BounceTune::Directions);
                const Id angle = add(mul(s.seed, ctx.ConstF32(3.14159265f)), ctx.ConstF32(base));
                const Id line = ctx.OpCompositeConstruct(
                    ctx.F32[2], std::array{ctx.OpCos(ctx.F32[1], angle), ctx.OpSin(ctx.F32[1], angle)});
                for (const float side : {1.0f, -1.0f}) {
                    Id horizon = ctx.ConstF32(0.0f); // the surface's own plane
                    for (u32 k = 0; k < BounceTune::StepsPerSide; ++k) {
                        const Id share = mul(add(ctx.ConstF32(float(k)), s.seed),
                                             ctx.ConstF32(side / float(BounceTune::StepsPerSide)));
                        const Id offset_px =
                            ctx.OpVectorTimesScalar(ctx.F32[2], line, mul(share, pixels));
                        const Id seen = horizon;
                        const Id e = sample_at(offset_px, [&](Id elevation, Id fall) {
                            return mul(ctx.OpFMax(ctx.F32[1], ctx.OpFSub(ctx.F32[1], elevation, seen),
                                                  ctx.ConstF32(0.0f)),
                                       fall);
                        });
                        horizon = ctx.OpFMax(ctx.F32[1], horizon, e);
                    }
                }
            }
            return;
        }
        for (u32 k = 0; k < BounceTune::Samples; ++k) {
            // A spiral: the golden angle per point, rotated by the jitter; radius by its share.
            const float a = 2.3999632f * float(k);
            const float share = std::sqrt((float(k) + 0.5f) / float(BounceTune::Samples));
            const Id cx = ctx.ConstF32(share * std::cos(a)), cy = ctx.ConstF32(share * std::sin(a));
            const Id dir = ctx.OpCompositeConstruct(
                ctx.F32[2], std::array{ctx.OpFSub(ctx.F32[1], mul(cos_s, cx), mul(sin_s, cy)),
                                       ctx.OpFAdd(ctx.F32[1], mul(sin_s, cx), mul(cos_s, cy))});
            sample_at(ctx.OpVectorTimesScalar(ctx.F32[2], dir, pixels), [&](Id elevation, Id fall) {
                return mul(ctx.OpFMax(ctx.F32[1], elevation, ctx.ConstF32(0.0f)), fall);
            });
        }
    });
    const float norm = horizon_gather
                           ? 2.0f * BounceTune::Strength / float(2 * BounceTune::Directions)
                           : 2.0f * BounceTune::Strength / float(BounceTune::Samples);
    const Id irradiance = ctx.OpVectorTimesScalar(ctx.F32[3], ctx.OpLoad(ctx.F32[3], s.gi),
                                                  ctx.ConstF32(norm));
    Id first = ctx.OpFMul(ctx.F32[3], irradiance, albedo);
    // The further bounces (bounce_light.h Multi): the first over (1 - gain) per colour.
    static const float multi = [] {
        const char* const env = std::getenv("VR_BOUNCE_MULTI");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 8.0f)
                              : BounceTune::Multi;
    }();
    if (multi > 0.0f) {
        const Id cover = mul(ctx.OpLoad(ctx.F32[1], cover_var), ctx.ConstF32(multi * norm));
        const Id gain = FiniteClamp(ctx, ctx.OpVectorTimesScalar(ctx.F32[3], albedo, cover), 0.0f,
                                    BounceTune::MaxGain, 3);
        const Id one3 = ctx.ConstF32(1.0f, 1.0f, 1.0f);
        first = ctx.OpFMul(ctx.F32[3], first,
                           ctx.OpFDiv(ctx.F32[3], one3, ctx.OpFSub(ctx.F32[3], one3, gain)));
    }
    // Crevices stay dark (bounce_light.h GuardTimes): at most that many times the pixel's own lit
    // diffuse light, its colour kept.
    static const float guard = [] {
        const char* const env = std::getenv("VR_BOUNCE_GUARD");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 64.0f) : BounceTune::GuardTimes;
    }();
    if (guard > 0.0f) {
        const Id own = FiniteClamp(ctx,
                                   ctx.OpFMul(ctx.F32[3],
                                              ctx.OpFAdd(ctx.F32[3], SampleRgb(ctx, BounceTune::DiffuseImages[0], here),
                                                         SampleRgb(ctx, BounceTune::DiffuseImages[1], here)),
                                              albedo),
                                   0.0f, BounceTune::MaxLight, 3);
        const Id weights = ctx.ConstF32(0.3f, 0.6f, 0.1f);
        const Id most = mul(ctx.OpDot(ctx.F32[1], own, weights), ctx.ConstF32(guard));
        const Id has = ctx.OpFMax(ctx.F32[1], ctx.OpDot(ctx.F32[1], first, weights), ctx.ConstF32(1e-6f));
        first = ctx.OpVectorTimesScalar(ctx.F32[3], first,
                                        ctx.OpFMin(ctx.F32[1], ctx.ConstF32(1.0f), ctx.OpFDiv(ctx.F32[1], most, has)));
    }
    const Id gi = FiniteClamp(ctx, first, 0.0f, BounceTune::MaxLight, 3);
    return ctx.OpCompositeConstruct(ctx.F32[4],
                                    std::array{ctx.OpCompositeExtract(ctx.F32[1], gi, 0u),
                                               ctx.OpCompositeExtract(ctx.F32[1], gi, 1u),
                                               ctx.OpCompositeExtract(ctx.F32[1], gi, 2u),
                                               ctx.ConstF32(0.0f)});
}

} // namespace Shader::Backend::SPIRV

// ---- Light in the air (ir/passes/fog_light.h) ----

namespace Shader::Backend::SPIRV {
namespace {

namespace FogTune = Shader::Optimization::FogLight;

// atan(x), error below 1e-5 rad: an odd polynomial on [0, 1], pi/2 - atan(1/x) above.
Id Atan(EmitContext& ctx, Id x) {
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(ctx.F32[1], a, b); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(ctx.F32[1], a, b); };
    const Id a = ctx.OpFAbs(ctx.F32[1], x);
    const Id big = ctx.OpFOrdGreaterThan(ctx.U1[1], a, ctx.ConstF32(1.0f));
    const Id z = ctx.OpSelect(ctx.F32[1], big,
                              ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f),
                                         ctx.OpFMax(ctx.F32[1], a, ctx.ConstF32(1.0f))),
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

// A dword of the lights' buffer. Both eyes' descriptors are the same upload (the eye's shift is
// applied here), so the first one: no per-eye descriptor index, no NonUniform, and every lane
// of a wave reads the same dword at the same time (one scalar load, not a loop per lane).
Id FogRead(EmitContext& ctx, u32 handle, Id index) {
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

// The view position of the surface at depth-image coordinates uv.
Id PositionAt(EmitContext& ctx, Id uv) {
    const auto& s = ctx.vr_light_shadow;
    const Id d = ctx.OpCompositeExtract(
        ctx.F32[1],
        EmitImageSampleExplicitLod(ctx, nullptr, ctx.info.vr_light_shadow.depth_image, uv,
                                   ctx.ConstF32(0.0f), IR::Value{}),
        0u);
    const Id n = ctx.OpFDiv(ctx.F32[2], ctx.OpFSub(ctx.F32[2], uv, s.b), s.a);
    const auto p4 = Transform(ctx, s.rows, ctx.OpCompositeExtract(ctx.F32[1], n, 0u),
                              ctx.OpCompositeExtract(ctx.F32[1], n, 1u), d);
    return ctx.OpVectorTimesScalar(ctx.F32[3],
                                   ctx.OpCompositeConstruct(ctx.F32[3], std::array{p4[0], p4[1], p4[2]}),
                                   ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), p4[3]));
}

// The surface's normal towards the eye: per screen axis the step to the neighbour nearer in
// distance (the one on this pixel's side of an edge), not the quad's derivative (which spans an
// edge on half the edge pixels).
Id SurfaceNormal(EmitContext& ctx, Id ndc, Id dir) {
    const auto& s = ctx.vr_light_shadow;
    const Id here = ctx.OpFAdd(ctx.F32[2], ctx.OpFMul(ctx.F32[2], s.a, ndc), s.b);
    const Id far_p = ctx.OpLength(ctx.F32[1], s.p);
    const auto step = [&](Id offset) {
        const Id ahead = PositionAt(ctx, ctx.OpFAdd(ctx.F32[2], here, offset));
        const Id behind = PositionAt(ctx, ctx.OpFSub(ctx.F32[2], here, offset));
        const Id gap_ahead = ctx.OpFAbs(ctx.F32[1], ctx.OpFSub(ctx.F32[1], ctx.OpLength(ctx.F32[1], ahead), far_p));
        const Id gap_behind = ctx.OpFAbs(ctx.F32[1], ctx.OpFSub(ctx.F32[1], ctx.OpLength(ctx.F32[1], behind), far_p));
        return ctx.OpSelect(ctx.F32[3], ctx.OpFOrdLessThan(ctx.U1[1], gap_ahead, gap_behind),
                            ctx.OpFSub(ctx.F32[3], ahead, s.p), ctx.OpFSub(ctx.F32[3], s.p, behind));
    };
    const Id px = ctx.OpCompositeExtract(ctx.F32[1], s.pixel, 0u);
    const Id py = ctx.OpCompositeExtract(ctx.F32[1], s.pixel, 1u);
    const Id ex = step(ctx.OpCompositeConstruct(ctx.F32[2], std::array{px, ctx.ConstF32(0.0f)}));
    const Id ey = step(ctx.OpCompositeConstruct(ctx.F32[2], std::array{ctx.ConstF32(0.0f), py}));
    // (Neighbours on one line, or not numbers: facing the eye.)
    const Id normal = SafeNormalize(ctx, ctx.OpCross(ctx.F32[3], ey, ex),
                                    ctx.OpFNegate(ctx.F32[3], dir));
    return ctx.OpSelect(ctx.F32[3],
                        ctx.OpFOrdGreaterThan(ctx.U1[1], ctx.OpDot(ctx.F32[1], normal, dir),
                                              ctx.ConstF32(0.0f)),
                        ctx.OpFNegate(ctx.F32[3], normal), normal);
}

// s.inv from s.rows, when the setup made none (the composite's: its other lighting needs none).
void EnsureInverse(EmitContext& ctx) {
    auto& s = ctx.vr_light_shadow;
    if (s.has_inv) {
        return;
    }
    std::array<std::array<Id, 4>, 4> m;
    for (u32 r = 0; r < 4; ++r) {
        for (u32 c = 0; c < 4; ++c) {
            m[r][c] = ctx.OpCompositeExtract(ctx.F32[1], s.rows[r], c);
        }
    }
    Invert(ctx, m);
}

// The share of a lamp's air along this pixel's ray that sees the lamp (FogLight's shadowed air):
// the epipolar bitmask march. The ray from the eye (view origin) along `dir`, the lamp at `l`;
// b = dir.l, `inv_h` and `a0`/`a1` the air integral's own (its slices of equal light run from
// atan((t0 - b) / h) to atan((t1 - b) / h)); `clear` the lamp fixture's radius.
Id AirShadow(EmitContext& ctx, Id l, Id dir, Id b, Id inv_h, Id a0, Id a1, Id clear) {
    const auto& s = ctx.vr_light_shadow;
    const auto& info = ctx.info.vr_light_shadow;
    const auto mul = [&](Id x, Id y) { return ctx.OpFMul(ctx.F32[1], x, y); };
    const auto add = [&](Id x, Id y) { return ctx.OpFAdd(ctx.F32[1], x, y); };
    const auto sub = [&](Id x, Id y) { return ctx.OpFSub(ctx.F32[1], x, y); };
    const auto f = [&](float v) { return ctx.ConstF32(v); };
    const auto u = [&](u32 v) { return ctx.ConstU32(v); };
    // The plane: e the unit way from the ray to the lamp, hp the lamp's distance from the ray.
    const Id perp = ctx.OpFSub(ctx.F32[3], l, ctx.OpVectorTimesScalar(ctx.F32[3], dir, b));
    const Id hp = ctx.OpFMax(ctx.F32[1], ctx.OpLength(ctx.F32[1], perp), f(1e-4f));
    const Id e = ctx.OpVectorTimesScalar(ctx.F32[3], perp, ctx.OpFDiv(ctx.F32[1], f(1.0f), hp));
    // The lamp's angle from the ray, atan2(hp, b) (hp > 0): pi/2 - atan(b / hp).
    const Id theta_l = sub(f(1.5707963f), Atan(ctx, ctx.OpFDiv(ctx.F32[1], b, hp)));
    const Id step = mul(theta_l, f(1.0f / float(FogTune::AirSamples)));
    // The sample rays at angles (k + seed) step from the ray, by rotating (c, sn) by step.
    Id c = ctx.OpCos(ctx.F32[1], mul(s.seed, step));
    Id sn = ctx.OpSin(ctx.F32[1], mul(s.seed, step));
    const Id cs = ctx.OpCos(ctx.F32[1], step);
    const Id ss = ctx.OpSin(ctx.F32[1], step);
    // Clip coordinates are linear in the ray: inv (c dir + sn e, 1) = c Cd + sn Ce + C0.
    const auto clip_of = [&](Id v, float w) {
        const Id v4 = ctx.OpCompositeConstruct(
            ctx.F32[4], std::array{ctx.OpCompositeExtract(ctx.F32[1], v, 0u),
                                   ctx.OpCompositeExtract(ctx.F32[1], v, 1u),
                                   ctx.OpCompositeExtract(ctx.F32[1], v, 2u), f(w)});
        return ctx.OpCompositeConstruct(
            ctx.F32[4],
            std::array{ctx.OpDot(ctx.F32[1], s.inv[0], v4), ctx.OpDot(ctx.F32[1], s.inv[1], v4),
                       ctx.OpDot(ctx.F32[1], s.inv[2], v4), ctx.OpDot(ctx.F32[1], s.inv[3], v4)});
    };
    const Id cd = clip_of(dir, 0.0f);
    const Id ce = clip_of(e, 0.0f);
    const Id c0 = clip_of(ctx.ConstF32(0.0f, 0.0f, 0.0f), 1.0f);
    const Id dz = ctx.OpCompositeExtract(ctx.F32[1], dir, 2u);
    const Id ez = ctx.OpCompositeExtract(ctx.F32[1], e, 2u);
    const Id scale =
        ctx.OpFDiv(ctx.F32[1], f(32.0f), ctx.OpFMax(ctx.F32[1], sub(a1, a0), f(1e-6f)));
    const Id clear2 = mul(clear, clear);
    const Id zero2 = ctx.ConstF32(0.0f, 0.0f), one2 = ctx.ConstF32(1.0f, 1.0f);
    // Bits below k (k in 0..32, a number: no shift by 32).
    const auto below = [&](Id k_f) {
        const Id k = ctx.OpConvertFToU(ctx.U32[1], FiniteClamp(ctx, k_f, 0.0f, 32.0f));
        const Id safe = ctx.OpUMin(ctx.U32[1], k, u(31u));
        const Id bits =
            ctx.OpISub(ctx.U32[1], ctx.OpShiftLeftLogical(ctx.U32[1], u(1u), safe), u(1u));
        return ctx.OpSelect(ctx.U32[1], ctx.OpUGreaterThanEqual(ctx.U1[1], k, u(32u)),
                            u(0xffffffffu), bits);
    };
    // The slice of a point t on the ray (0..32 once `below` clamps it).
    const auto slice = [&](Id t) { return mul(sub(Atan(ctx, mul(sub(t, b), inv_h)), a0), scale); };
    Id mask = u(0u);
    for (u32 k = 0; k < FogTune::AirSamples; ++k) {
        if (k > 0) {
            const Id nc = sub(mul(c, cs), mul(sn, ss));
            sn = add(mul(sn, cs), mul(c, ss));
            c = nc;
        }
        const Id clip = ctx.OpFAdd(
            ctx.F32[4],
            ctx.OpFAdd(ctx.F32[4], ctx.OpVectorTimesScalar(ctx.F32[4], cd, c),
                       ctx.OpVectorTimesScalar(ctx.F32[4], ce, sn)),
            c0);
        const Id w = ctx.OpCompositeExtract(ctx.F32[1], clip, 3u);
        const Id inv_w = ctx.OpFDiv(ctx.F32[1], f(1.0f), ctx.OpFMax(ctx.F32[1], w, f(1e-6f)));
        const Id n = ctx.OpCompositeConstruct(
            ctx.F32[2], std::array{mul(ctx.OpCompositeExtract(ctx.F32[1], clip, 0u), inv_w),
                                   mul(ctx.OpCompositeExtract(ctx.F32[1], clip, 1u), inv_w)});
        const Id uv = ctx.OpFAdd(ctx.F32[2], ctx.OpFMul(ctx.F32[2], s.a, n), s.b);
        const Id on = ctx.OpLogicalAnd(
            ctx.U1[1], ctx.OpFOrdGreaterThan(ctx.U1[1], w, f(1e-6f)),
            ctx.OpLogicalAnd(
                ctx.U1[1], ctx.OpAll(ctx.U1[1], ctx.OpFOrdGreaterThanEqual(ctx.U1[2], uv, zero2)),
                ctx.OpAll(ctx.U1[1], ctx.OpFOrdLessThanEqual(ctx.U1[2], uv, one2))));
        const Id ds = ctx.OpCompositeExtract(
            ctx.F32[1],
            EmitImageSampleExplicitLod(ctx, nullptr, info.depth_image,
                                       ctx.OpFClamp(ctx.F32[2], uv, zero2, one2), f(0.0f),
                                       IR::Value{}),
            0u);
        // The surface there: its view z, then its distance d along this sample's (unit) ray.
        const Id s4v = ctx.OpCompositeConstruct(
            ctx.F32[4], std::array{ctx.OpCompositeExtract(ctx.F32[1], n, 0u),
                                   ctx.OpCompositeExtract(ctx.F32[1], n, 1u), ds, f(1.0f)});
        const Id sz = ctx.OpFDiv(ctx.F32[1], ctx.OpDot(ctx.F32[1], s.rows[2], s4v),
                                 ctx.OpDot(ctx.F32[1], s.rows[3], s4v));
        const Id rz = add(mul(c, dz), mul(sn, ez));
        const Id rz_safe = ctx.OpSelect(
            ctx.F32[1], ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpFAbs(ctx.F32[1], rz), f(1e-6f)),
            f(1e-6f), rz);
        const Id d = ctx.OpFDiv(ctx.F32[1], sz, rz_safe);
        // Not the lamp's own fixture.
        const Id r = ctx.OpFAdd(ctx.F32[3], ctx.OpVectorTimesScalar(ctx.F32[3], dir, c),
                                ctx.OpVectorTimesScalar(ctx.F32[3], e, sn));
        const Id q = ctx.OpFSub(ctx.F32[3], ctx.OpVectorTimesScalar(ctx.F32[3], r, d), l);
        const Id housing = ctx.OpFOrdLessThan(ctx.U1[1], ctx.OpDot(ctx.F32[1], q, q), clear2);
        // A = |l| sin(theta_l - theta) > 0 before the lamp's own angle.
        const Id a = sub(mul(hp, c), mul(b, sn));
        // The segment from the ray's point t to the lamp crosses this sample's ray at
        // rho(t) = t hp / (A + t sn), so t(rho) = rho A / (hp - rho sn): the surface, from
        // AirBiasM to its thickness behind it, blocks t from t(d + bias) to t(d + thick).
        const Id rho_lo = add(d, f(FogTune::AirBiasM));
        const Id rho_hi = add(d, add(f(FogTune::AirThickM), mul(d, f(FogTune::AirThickPerM))));
        const Id den_lo = sub(hp, mul(rho_lo, sn));
        const Id den_hi = sub(hp, mul(rho_hi, sn));
        const Id t_lo = ctx.OpFDiv(ctx.F32[1], mul(rho_lo, a),
                                   ctx.OpFMax(ctx.F32[1], den_lo, f(1e-6f)));
        const Id t_hi = ctx.OpSelect(
            ctx.F32[1], ctx.OpFOrdGreaterThan(ctx.U1[1], den_hi, f(1e-6f)),
            ctx.OpFDiv(ctx.F32[1], mul(rho_hi, a), ctx.OpFMax(ctx.F32[1], den_hi, f(1e-6f))),
            f(1e9f));
        const Id ok = ctx.OpLogicalAnd(
            ctx.U1[1], ctx.OpLogicalAnd(ctx.U1[1], on, ctx.OpLogicalNot(ctx.U1[1], housing)),
            ctx.OpLogicalAnd(
                ctx.U1[1],
                ctx.OpLogicalAnd(ctx.U1[1], ctx.OpFOrdGreaterThan(ctx.U1[1], d, f(0.05f)),
                                 ctx.OpFOrdGreaterThan(ctx.U1[1], a, f(1e-6f))),
                ctx.OpFOrdGreaterThan(ctx.U1[1], den_lo, f(1e-6f))));
        const Id bits =
            ctx.OpBitwiseAnd(ctx.U32[1], below(ctx.OpCeil(ctx.F32[1], slice(t_hi))),
                             ctx.OpNot(ctx.U32[1], below(ctx.OpFloor(ctx.F32[1], slice(t_lo)))));
        mask = ctx.OpBitwiseOr(ctx.U32[1], mask, ctx.OpSelect(ctx.U32[1], ok, bits, u(0u)));
    }
    return sub(f(1.0f), mul(ctx.OpConvertUToF(ctx.F32[1], ctx.OpBitCount(ctx.U32[1], mask)),
                            f(1.0f / 32.0f)));
}

} // namespace

// Every light of the buffer (the frame's strongest, Rasterizer::VrFogUpload): the integral of its
// falloff along this pixel's ray from the eye to the surface, inside its reach, while it is in
// front of the surface's plane; the sum times the density.
Id EmitVrFogLight(EmitContext& ctx, Id ndc, Id uv, Id depth) {
    // With the air pass (air_light.h) the glow in the air is the scene copy's: here the surface's.
    return VrFogLightParts(ctx, ndc, uv, depth, !Shader::Optimization::AirLight::Halos(), true, nullptr);
}

Id VrFogLightParts(EmitContext& ctx, Id ndc, Id uv, Id depth, bool air_part, bool surface_part,
                   const VrFogVote* vote) {
    const auto& buffers = ctx.info.buffers;
    const auto found =
        std::ranges::find(buffers, BufferType::VrFogLights, &BufferResource::buffer_type);
    if (!ctx.info.vr_light_shadow.on || found == buffers.end()) {
        return ctx.ConstF32(0.0f, 0.0f, 0.0f, 0.0f);
    }
    const u32 fog = static_cast<u32>(std::distance(buffers.begin(), found));
    auto& s = ctx.vr_light_shadow;
    if (!s.ready) {
        Setup(ctx, ndc, uv, depth, false);
    }
    // Shadowed air (FogLight's AirLamps): compiled unless VR_AIR_SHADOWS=0.
    static const bool air_shadows = [] {
        const char* const env = std::getenv("VR_AIR_SHADOWS");
        return env == nullptr || env[0] != '0';
    }();
    // The scene copy's call (the glow in the air alone): the surface's normal (four depth reads)
    // and the matrix's inverse are made only at a pixel a light's reach touches, the inverse only
    // for a lamp that marches (2026-11-15: both were made at every pixel of the picture, and most
    // lie in no light's reach). The same numbers. VR_OPT_GLOWLIT=0: as before.
    static const bool lazy_opt = [] {
        const char* const env = std::getenv("VR_OPT_GLOWLIT");
        return env != nullptr && env[0] == '1'; // (off unless 1: measured 2026-11-15, no gain in lit scenes)
    }();
    const bool lazy = lazy_opt && air_part && !surface_part;
    Id air_vis{};
    if (air_shadows) {
        if (!lazy) {
            EnsureInverse(ctx);
        }
        air_vis = Private(ctx, ctx.F32[1]);
    }
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(ctx.F32[1], a, b); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(ctx.F32[1], a, b); };
    const auto sub = [&](Id a, Id b) { return ctx.OpFSub(ctx.F32[1], a, b); };
    const Id length = ctx.OpLength(ctx.F32[1], s.p);
    const Id dir = ctx.OpVectorTimesScalar(
        ctx.F32[3], s.p,
        ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), ctx.OpFMax(ctx.F32[1], length, ctx.ConstF32(1e-4f))));
    const Id normal = lazy ? Id{} : SurfaceNormal(ctx, ndc, dir);
    Id normal_var{}, normal_have{};
    if (lazy) {
        normal_var = Private(ctx, ctx.F32[3]);
        normal_have = Private(ctx, ctx.U32[1]);
        ctx.OpStore(normal_have, ctx.u32_zero_value);
    }
    // The header's first word: the lights (its low half), of them the first so many with a glow
    // of their own (its high half; the rest only light smoke and are voted on, air_light.h
    // Sources): the loop ends after those, except at a pixel that votes.
    const Id count_word = FogRead(ctx, fog, ctx.ConstU32(0u));
    const Id count_all = ctx.OpUMin(ctx.U32[1], ctx.OpBitwiseAnd(ctx.U32[1], count_word, ctx.ConstU32(0xffffu)),
                                    ctx.ConstU32(FogTune::MaxLights));
    const Id count_glow = ctx.OpUMin(ctx.U32[1], ctx.OpShiftRightLogical(ctx.U32[1], count_word, ctx.ConstU32(16u)),
                                     count_all);
    const Id count = vote != nullptr ? ctx.OpSelect(ctx.U32[1], vote->on, count_all, count_glow) : count_glow;
    // Native stereo: each eye's view is the flat camera's moved along its x, the lists' lights
    // the flat camera's (NativeProjection::RemapLightList's shift).
    Id shift = ctx.ConstF32(0.0f);
    if (ctx.runtime_info.native_stereo && ctx.runtime_info.native_multiview) {
        const Id half = ctx.OpBitcast(ctx.F32[1], FogRead(ctx, fog, ctx.ConstU32(1u)));
        shift = ctx.OpSelect(ctx.F32[1], ctx.OpIEqual(ctx.U1[1], ctx.NativeEyeIndex(), ctx.u32_zero_value),
                             ctx.OpFNegate(ctx.F32[1], half), half);
    }
    const Id density = ctx.OpBitcast(ctx.F32[1], FogRead(ctx, fog, ctx.ConstU32(2u)));
    // Flames fill the room (FogLight's FillLights): header dword 3, the fill strength.
    const Id fill_strength = FiniteClamp(ctx, ctx.OpBitcast(ctx.F32[1], FogRead(ctx, fog, ctx.ConstU32(3u))),
                                         0.0f, 1.0f);
    const Id fill_sum = Private(ctx, ctx.F32[3]);
    ctx.OpStore(fill_sum, ctx.ConstF32(0.0f, 0.0f, 0.0f));
    const Id sum = Private(ctx, ctx.F32[3]);
    const Id index = Private(ctx, ctx.U32[1]);
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
    const Id base = ctx.OpIAdd(ctx.U32[1], ctx.ConstU32(FogTune::HeaderDwords),
                               ctx.OpIMul(ctx.U32[1], i, ctx.ConstU32(FogTune::LightDwords)));
    const auto at = [&](u32 k) {
        return ctx.OpBitcast(ctx.F32[1],
                             FogRead(ctx, fog, ctx.OpIAdd(ctx.U32[1], base, ctx.ConstU32(k))));
    };
    const Id l = ctx.OpCompositeConstruct(ctx.F32[3], std::array{sub(at(0), shift), at(1), at(2)});
    const Id reach_k = ctx.OpFMax(ctx.F32[1], at(3), ctx.ConstF32(1e-6f));
    if (surface_part) {
        // The flame's light round the room: (1 - d^2 / R^2)^2 out to R = FillRadiusScale of its
        // radius (R^2 = FillRadiusScale^2 / reach), in front of the surface's plane; the first
        // FillLights only (a select, not a branch: the loop is uniform).
        const Id to_light = ctx.OpFSub(ctx.F32[3], l, s.p);
        const Id d2 = ctx.OpDot(ctx.F32[1], to_light, to_light);
        const Id window = ctx.OpFMax(
            ctx.F32[1],
            sub(ctx.ConstF32(1.0f),
                mul(mul(d2, reach_k), ctx.ConstF32(1.0f / (FogTune::FillRadiusScale *
                                                           FogTune::FillRadiusScale)))),
            ctx.ConstF32(0.0f));
        // (By the angle: fog_light.h FillFaceFrom.)
        const Id facing = SmoothStep(
            ctx, FogTune::FillFaceFrom, FogTune::FillFaceTo,
            mul(ctx.OpDot(ctx.F32[1], normal, to_light),
                ctx.OpInverseSqrt(ctx.F32[1], ctx.OpFMax(ctx.F32[1], d2, ctx.ConstF32(1e-6f)))));
        const Id weight = ctx.OpSelect(
            ctx.F32[1], ctx.OpULessThan(ctx.U1[1], i, ctx.ConstU32(FogTune::FillLights)),
            mul(mul(window, window), facing), ctx.ConstF32(0.0f));
        const Id fill_colour = ctx.OpCompositeConstruct(ctx.F32[3], std::array{at(4), at(5), at(6)});
        ctx.OpStore(fill_sum, ctx.OpFAdd(ctx.F32[3], ctx.OpLoad(ctx.F32[3], fill_sum),
                                         ctx.OpVectorTimesScalar(ctx.F32[3], fill_colour, weight)));
    }
    if (air_part) {
    // The ray's closest approach: at b along it, h from the light; inside the reach (radius^2 =
    // 1 / reach_k) from b - half to b + half, on [0, length].
    const Id b = ctx.OpDot(ctx.F32[1], dir, l);
    const Id off2 = ctx.OpFMax(ctx.F32[1], sub(ctx.OpDot(ctx.F32[1], l, l), mul(b, b)), ctx.ConstF32(0.0f));
    if (vote != nullptr) {
        VrAirSourceVote(ctx, *vote, i, b, off2); // (air_light.h: a flame at the light's place?)
    }
    const Id h2 = add(off2, ctx.ConstF32(FogTune::GlowCoreM() * FogTune::GlowCoreM()));
    const Id half = ctx.OpSqrt(
        ctx.F32[1], ctx.OpFMax(ctx.F32[1], sub(ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), reach_k), h2),
                               ctx.ConstF32(0.0f)));
    const Id t0 = ctx.OpFMax(ctx.F32[1], sub(b, half), ctx.ConstF32(0.0f));
    const Id t1 = ctx.OpFMin(ctx.F32[1], add(b, half), length);
    const Id inside = ctx.OpFOrdGreaterThan(ctx.U1[1], t1, t0);
    const Id then_label = ctx.OpLabel(), endif = ctx.OpLabel();
    ctx.OpSelectionMerge(endif, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(inside, then_label, endif);
    ctx.AddLabel(then_label);
    {
        const Id inv_h = ctx.OpInverseSqrt(ctx.F32[1], h2);
        const Id a0 = Atan(ctx, mul(sub(t0, b), inv_h));
        const Id a1 = Atan(ctx, mul(sub(t1, b), inv_h));
        Id integral = ctx.OpFMax(ctx.F32[1], sub(mul(sub(a1, a0), inv_h), mul(sub(t1, t0), reach_k)),
                                 ctx.ConstF32(0.0f));
        if (air_shadows) {
            // Light dword 7: this lamp's shadow share (0: not one of the shadowed lamps).
            const Id share = FiniteClamp(ctx, at(7), 0.0f, 1.0f);
            ctx.OpStore(air_vis, ctx.ConstF32(1.0f));
            const Id march = ctx.OpLabel(), marched = ctx.OpLabel();
            const Id want = ctx.OpFOrdGreaterThan(ctx.U1[1], share, ctx.ConstF32(0.0f));
            ctx.OpSelectionMerge(marched, spv::SelectionControlMask::MaskNone);
            ctx.OpBranchConditional(want, march, marched);
            ctx.AddLabel(march);
            {
                // Its fixture: the old, wider clearance (light_shadow.h ClearAirOld).
                const Id clear = FixtureClear(ctx, ctx.OpInverseSqrt(ctx.F32[1], reach_k), true);
                // (Lazy: the inverse lives in this block only; what follows makes its own.)
                const bool had_inverse = s.has_inv;
                EnsureInverse(ctx);
                ctx.OpStore(air_vis, AirShadow(ctx, l, dir, b, inv_h, a0, a1, clear));
                if (lazy && !had_inverse) {
                    s.has_inv = false;
                }
                ctx.OpBranch(marched);
            }
            ctx.AddLabel(marched);
            const Id vis = ctx.OpLoad(ctx.F32[1], air_vis);
            integral = mul(integral, sub(ctx.ConstF32(1.0f),
                                         mul(share, sub(ctx.ConstF32(1.0f), vis))));
        }
        // In front of the surface's plane (by the angle: fog_light.h AirFaceFrom).
        const Id to_lamp = ctx.OpFSub(ctx.F32[3], l, s.p);
        Id normal_here = normal;
        if (lazy) {
            // (Once a pixel: the first light whose reach it lies in.)
            const Id make = ctx.OpLabel(), made = ctx.OpLabel();
            const Id first_need = ctx.OpIEqual(ctx.U1[1], ctx.OpLoad(ctx.U32[1], normal_have), ctx.u32_zero_value);
            ctx.OpSelectionMerge(made, spv::SelectionControlMask::MaskNone);
            ctx.OpBranchConditional(first_need, make, made);
            ctx.AddLabel(make);
            ctx.OpStore(normal_var, SurfaceNormal(ctx, ndc, dir));
            ctx.OpStore(normal_have, ctx.ConstU32(1u));
            ctx.OpBranch(made);
            ctx.AddLabel(made);
            normal_here = ctx.OpLoad(ctx.F32[3], normal_var);
        }
        const Id front = SmoothStep(
            ctx, FogTune::AirFaceFrom, FogTune::AirFaceTo,
            mul(ctx.OpDot(ctx.F32[1], normal_here, to_lamp),
                ctx.OpInverseSqrt(ctx.F32[1], ctx.OpFMax(ctx.F32[1], ctx.OpDot(ctx.F32[1], to_lamp, to_lamp),
                                                         ctx.ConstF32(1e-6f)))));
        // A glow round the light, not a veil over the room.
        const Id halo = ctx.OpExp(ctx.F32[1], mul(ctx.OpSqrt(ctx.F32[1], h2),
                                                  ctx.ConstF32(-1.0f / FogTune::GlowHaloM())));
        const Id colour = ctx.OpCompositeConstruct(ctx.F32[3], std::array{at(4), at(5), at(6)});
        ctx.OpStore(sum, ctx.OpFAdd(ctx.F32[3], ctx.OpLoad(ctx.F32[3], sum),
                                    ctx.OpVectorTimesScalar(ctx.F32[3], colour,
                                                            mul(mul(integral, front), halo))));
        ctx.OpBranch(endif);
    }
    ctx.AddLabel(endif);
    }
    ctx.OpStore(index, ctx.OpIAdd(ctx.U32[1], i, ctx.ConstU32(1u)));
    ctx.OpBranch(cont);
    ctx.AddLabel(cont);
    ctx.OpBranch(header);
    ctx.AddLabel(merge);
    // The fill on this surface: its own colour (the composite's albedo input) times the sum.
    Id surface{};
    Id fill = ctx.ConstF32(0.0f, 0.0f, 0.0f);
    if (surface_part) {
        const Id here = ctx.OpFAdd(ctx.F32[2], ctx.OpFMul(ctx.F32[2], s.a, ndc), s.b);
        surface = FiniteClamp(ctx, SampleRgb(ctx, BounceTune::AlbedoImage, here), 0.0f, 1.0f, 3);
        fill = ctx.OpVectorTimesScalar(
            ctx.F32[3], ctx.OpFMul(ctx.F32[3], surface, ctx.OpLoad(ctx.F32[3], fill_sum)), fill_strength);
    }
    // The shared surface light uses its colour, falloff and surface normal. Gun and torch also
    // use the shadow march; the atmospheric Tonitrus light uses full visibility.
    Id muzzle = ctx.ConstF32(0.0f, 0.0f, 0.0f);
    if (surface_part) {
        const auto mzf = [&](u32 k) {
            return ctx.OpBitcast(ctx.F32[1], FogRead(ctx, fog, ctx.ConstU32(FogTune::MuzzleDword + k)));
        };
        const Id mz_var = Private(ctx, ctx.F32[3]);
        ctx.OpStore(mz_var, ctx.ConstF32(0.0f, 0.0f, 0.0f));
        const Id signed_core = mzf(7);
        const Id core = ctx.OpFAbs(ctx.F32[1], signed_core);
        // With shadowed air disabled, only the unshadowed Tonitrus owns a visible slot.
        const Id shines = air_shadows ? ctx.OpFOrdGreaterThan(ctx.U1[1], core, ctx.ConstF32(0.5f))
                                      : ctx.OpFOrdLessThan(ctx.U1[1], signed_core, ctx.ConstF32(-0.5f));
        const Id lit_label = ctx.OpLabel(), lit_done = ctx.OpLabel();
        ctx.OpSelectionMerge(lit_done, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(shines, lit_label, lit_done);
        ctx.AddLabel(lit_label);
        {
            const Id l = ctx.OpCompositeConstruct(ctx.F32[3], std::array{sub(mzf(0), shift), mzf(1), mzf(2)});
            const Id to_light = ctx.OpFSub(ctx.F32[3], l, s.p);
            const Id d2 = ctx.OpFMax(ctx.F32[1], ctx.OpDot(ctx.F32[1], to_light, to_light), ctx.ConstF32(1e-6f));
            const Id dist = ctx.OpSqrt(ctx.F32[1], d2);
            const Id ndl = ctx.OpFMax(ctx.F32[1],
                                      ctx.OpFDiv(ctx.F32[1], ctx.OpDot(ctx.F32[1], normal, to_light), dist),
                                      ctx.ConstF32(0.0f));
            const Id falloff = ctx.OpFMax(
                ctx.F32[1],
                sub(ctx.OpFDiv(ctx.F32[1], ctx.ConstF32(1.0f), add(d2, FiniteClamp(ctx, core, 0.5f, 50.0f))),
                    FiniteClamp(ctx, mzf(3), 0.0f, 100.0f)),
                ctx.ConstF32(0.0f));
            Id vis = ctx.ConstF32(1.0f);
            if (air_shadows) {
                // The slot is uniform across the draw. Only a gun flash or raised torch traces.
                const Id visibility = Private(ctx, ctx.F32[1]);
                ctx.OpStore(visibility, vis);
                const Id trace = ctx.OpLabel(), done = ctx.OpLabel();
                const Id casts = ctx.OpFOrdGreaterThan(ctx.U1[1], signed_core, ctx.ConstF32(0.0f));
                ctx.OpSelectionMerge(done, spv::SelectionControlMask::MaskNone);
                ctx.OpBranchConditional(casts, trace, done);
                ctx.AddLabel(trace);
                const Id end = ctx.OpFClamp(ctx.F32[1], sub(dist, ctx.ConstF32(Tune::ClearM)),
                                            ctx.ConstF32(Tune::StartM), ctx.ConstF32(Tune::MaxM));
                ctx.OpStore(visibility, March(ctx, l, dist, end, ctx.ConstU32(FogTune::MuzzleSteps)));
                ctx.OpBranch(done);
                ctx.AddLabel(done);
                vis = ctx.OpLoad(ctx.F32[1], visibility);
            }
            const Id colour = FiniteClamp(
                ctx, ctx.OpCompositeConstruct(ctx.F32[3], std::array{mzf(4), mzf(5), mzf(6)}), 0.0f,
                64.0f, 3);
            const Id light = ctx.OpVectorTimesScalar(ctx.F32[3], ctx.OpFMul(ctx.F32[3], surface, colour),
                                                     mul(mul(ndl, falloff), vis));
            ctx.OpStore(mz_var, FiniteClamp(ctx, light, 0.0f, FogTune::MaxAir, 3));
            ctx.OpBranch(lit_done);
        }
        ctx.AddLabel(lit_done);
        muzzle = ctx.OpLoad(ctx.F32[3], mz_var);
    }
    const Id air = FiniteClamp(
        ctx,
        ctx.OpFAdd(ctx.F32[3],
                   ctx.OpFAdd(ctx.F32[3],
                              ctx.OpVectorTimesScalar(ctx.F32[3], ctx.OpLoad(ctx.F32[3], sum), density),
                              fill),
                   muzzle),
        0.0f, FogTune::MaxAir, 3);
    return ctx.OpCompositeConstruct(ctx.F32[4],
                                    std::array{ctx.OpCompositeExtract(ctx.F32[1], air, 0u),
                                               ctx.OpCompositeExtract(ctx.F32[1], air, 1u),
                                               ctx.OpCompositeExtract(ctx.F32[1], air, 2u),
                                               ctx.ConstF32(0.0f)});
}

} // namespace Shader::Backend::SPIRV

// ---- Cinematic look (ir/passes/cinematic_look.h) ----

namespace Shader::Backend::SPIRV {

// The grade (cinematic_look.h Params): deeper blacks, a filmic curve round the pivot, saturation
// by brightness, a faint split tint; mixed in by the strength (0: the colour as it was).
Id EmitVrCinematic(EmitContext& ctx, Id colour) {
    const auto& buffers = ctx.info.buffers;
    const auto found = std::ranges::find(buffers, BufferType::VrGrade, &BufferResource::buffer_type);
    const auto out4 = [&](Id rgb) {
        return ctx.OpCompositeConstruct(ctx.F32[4],
                                        std::array{ctx.OpCompositeExtract(ctx.F32[1], rgb, 0u),
                                                   ctx.OpCompositeExtract(ctx.F32[1], rgb, 1u),
                                                   ctx.OpCompositeExtract(ctx.F32[1], rgb, 2u),
                                                   ctx.ConstF32(0.0f)});
    };
    if (found == buffers.end()) {
        return out4(colour);
    }
    const u32 grade = static_cast<u32>(std::distance(buffers.begin(), found));
    const auto P = [&](u32 k) {
        return ctx.OpBitcast(ctx.F32[1], FogRead(ctx, grade, ctx.ConstU32(k)));
    };
    const auto V = [&](u32 k) { return ctx.OpCompositeConstruct(ctx.F32[3], std::array{P(k), P(k + 1), P(k + 2)}); };
    const auto splat = [&](Id x) { return ctx.OpCompositeConstruct(ctx.F32[3], std::array{x, x, x}); };
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(ctx.F32[1], a, b); };
    const auto sub = [&](Id a, Id b) { return ctx.OpFSub(ctx.F32[1], a, b); };
    const Id one = ctx.ConstF32(1.0f);
    const Id strength = P(0);
    const Id exponent = ctx.OpFAdd(ctx.F32[1], one, P(1));
    const Id black = P(2);
    const Id pivot = P(3);
    const Id zero3 = ctx.ConstF32(0.0f, 0.0f, 0.0f), one3 = ctx.ConstF32(1.0f, 1.0f, 1.0f);
    const Id c = FiniteClamp(ctx, colour, 0.0f, 1.0f, 3);
    // Deeper blacks: the black level crushed to 0, white kept (within [0, 1] again: a reciprocal
    // one ulp high puts white at 1.0000001, and the curve's pow of 1 - x would be NaN).
    const Id c1 = ctx.OpFClamp(
        ctx.F32[3],
        ctx.OpVectorTimesScalar(
            ctx.F32[3], ctx.OpFMax(ctx.F32[3], ctx.OpFSub(ctx.F32[3], c, splat(black)), zero3),
            ctx.OpFDiv(ctx.F32[1], one, sub(one, black))),
        zero3, one3);
    // The filmic curve round the pivot: steeper midtones, a toe and a shoulder.
    std::array<Id, 3> y;
    for (u32 i = 0; i < 3; ++i) {
        const Id x = ctx.OpCompositeExtract(ctx.F32[1], c1, i);
        const Id lo = mul(pivot, ctx.OpPow(ctx.F32[1], ctx.OpFDiv(ctx.F32[1], x, pivot), exponent));
        const Id hi = sub(one, mul(sub(one, pivot),
                                   ctx.OpPow(ctx.F32[1],
                                             ctx.OpFDiv(ctx.F32[1], sub(one, x), sub(one, pivot)),
                                             exponent)));
        y[i] = ctx.OpSelect(ctx.F32[1], ctx.OpFOrdLessThan(ctx.U1[1], x, pivot), lo, hi);
    }
    const Id c2 = ctx.OpCompositeConstruct(ctx.F32[3], y);
    // Saturation by brightness: quieter shadows, a little more in the light.
    const Id luma = ctx.OpDot(ctx.F32[1], c2, V(12));
    const Id lit = SmoothStep(ctx, 0.08f, 0.6f, luma);
    const Id sat = ctx.OpFAdd(ctx.F32[1], P(4), mul(sub(P(5), P(4)), lit));
    const Id c3 = ctx.OpFAdd(ctx.F32[3], splat(luma),
                             ctx.OpVectorTimesScalar(ctx.F32[3],
                                                     ctx.OpFSub(ctx.F32[3], c2, splat(luma)), sat));
    // The split tint: cooler shadows, warmer highlights.
    const Id dark = sub(one, luma);
    const Id c4 = ctx.OpFAdd(
        ctx.F32[3], c3,
        ctx.OpFAdd(ctx.F32[3], ctx.OpVectorTimesScalar(ctx.F32[3], V(6), mul(dark, dark)),
                   ctx.OpVectorTimesScalar(ctx.F32[3], V(9), mul(luma, luma))));
    const Id graded = ctx.OpFClamp(ctx.F32[3], c4, zero3, one3);
    return out4(Finite(ctx,
                       ctx.OpFAdd(ctx.F32[3], c,
                                  ctx.OpVectorTimesScalar(ctx.F32[3],
                                                          ctx.OpFSub(ctx.F32[3], graded, c),
                                                          strength)),
                       c, 3));
}

// Filmic highlights (cinematic_look.h): e = exp(-k x) per channel, the game's tone-mapped channel
// 1 - e. The brightest channel's t = 1 - min(e) shared out by each channel's part of the
// brightest (x / max x) keeps the light's hue; the buffer's float 15 moves e that far towards it.
// Eye adaptation (cinematic_look.h): the exposure from the state buffer's slot for this frame's
// parity; every SampleEvery-th pixel (both directions) adds its log2 luminance to this frame's
// sum; the frame's first pixel (the first eye's) turns the last frame's sum into the next frame's
// state and clears the next frame's sum. The state's slots alternate, so no pixel reads a value
// another pixel of the same frame writes. All values bounded, nothing that is not a number kept.
Id EmitVrExposure(EmitContext& ctx, Id x) {
    namespace Look = Shader::Optimization::CinematicLook;
    const auto& buffers = ctx.info.buffers;
    const auto grade_it = std::ranges::find(buffers, BufferType::VrGrade, &BufferResource::buffer_type);
    const auto state_it =
        std::ranges::find(buffers, BufferType::VrAdaptation, &BufferResource::buffer_type);
    if (grade_it == buffers.end() || state_it == buffers.end()) {
        return ctx.ConstF32(1.0f);
    }
    const u32 grade = static_cast<u32>(std::distance(buffers.begin(), grade_it));
    const u32 state = static_cast<u32>(std::distance(buffers.begin(), state_it));
    const auto f = [&](float v) { return ctx.ConstF32(v); };
    const auto u = [&](u32 v) { return ctx.ConstU32(v); };
    const auto mul = [&](Id a, Id b) { return ctx.OpFMul(ctx.F32[1], a, b); };
    const auto add = [&](Id a, Id b) { return ctx.OpFAdd(ctx.F32[1], a, b); };
    const auto sub = [&](Id a, Id b) { return ctx.OpFSub(ctx.F32[1], a, b); };
    const auto gradef = [&](u32 dword) { return ctx.OpBitcast(ctx.F32[1], FogRead(ctx, grade, u(dword))); };
    // The state buffer's dword `index` (its first descriptor: both eyes' are the same buffer).
    const auto& sbuf = ctx.buffers[state];
    const auto [sid, sptr_type] = sbuf.Alias(EmitContext::PointerType::U32);
    const auto ptr = [&](Id index) {
        if (const Id offset = sbuf.Offset(EmitContext::PointerSize::B32); Sirit::ValidId(offset)) {
            index = ctx.OpIAdd(ctx.U32[1], index, offset);
        }
        return ctx.runtime_info.native_stereo
                   ? ctx.OpAccessChain(sptr_type, sid, ctx.u32_zero_value, ctx.u32_zero_value, index)
                   : ctx.OpAccessChain(sptr_type, sid, ctx.u32_zero_value, index);
    };
    const auto load = [&](Id index) { return ctx.OpLoad(ctx.U32[1], ptr(index)); };
    const auto loadf = [&](Id index) { return ctx.OpBitcast(ctx.F32[1], load(index)); };
    const auto store = [&](Id index, Id value_u32) { ctx.OpStore(ptr(index), value_u32); };
    const Id strength = FiniteClamp(ctx, gradef(16), 0.0f, 3.0f);
    const Id fast_rate = FiniteClamp(ctx, gradef(17), 0.0f, 1.0f);
    const Id slow_rate = FiniteClamp(ctx, gradef(18), 0.0f, 1.0f);       // into the light
    const Id stops = FiniteClamp(ctx, gradef(19), 0.0f, 4.0f);
    const Id slow_down_rate = FiniteClamp(ctx, gradef(21), 0.0f, 1.0f);  // into the dark
    const Id threshold = FiniteClamp(ctx, gradef(22), 0.0f, 4.0f);
    const Id frame = FogRead(ctx, grade, u(20));
    const Id parity = ctx.OpBitwiseAnd(ctx.U32[1], frame, u(1u));
    const Id read_base = ctx.OpIMul(ctx.U32[1], parity, u(2u));          // 0 or 2
    const Id write_base = ctx.OpISub(ctx.U32[1], u(2u), read_base);      // 2 or 0
    const Id current = FiniteClamp(ctx, loadf(read_base), -24.0f, 16.0f);
    const Id adapted = FiniteClamp(ctx, loadf(ctx.OpIAdd(ctx.U32[1], read_base, u(1u))), -24.0f, 16.0f);
    const Id valid = ctx.OpINotEqual(ctx.U1[1], load(u(4u)), ctx.u32_zero_value);
    // The gap past the dead zone, with its sign: small changes do nothing.
    const Id gap = sub(current, adapted);
    const Id past = ctx.OpFMax(ctx.F32[1], sub(ctx.OpFAbs(ctx.F32[1], gap), threshold), f(0.0f));
    const Id signed_past = ctx.OpSelect(ctx.F32[1], ctx.OpFOrdLessThan(ctx.U1[1], gap, f(0.0f)),
                                        ctx.OpFNegate(ctx.F32[1], past), past);
    const Id exposure = ctx.OpSelect(
        ctx.F32[1], valid,
        ctx.OpExp2(ctx.F32[1],
                   ctx.OpFClamp(ctx.F32[1], mul(strength, signed_past),
                                ctx.OpFNegate(ctx.F32[1], stops), stops)),
        f(1.0f));
    // This pixel's share of the frame's brightness.
    const Id fx = ctx.OpConvertFToU(ctx.U32[1], ctx.OpFMax(ctx.F32[1], EmitGetAttribute(ctx, IR::Attribute::FragCoord, 0, 0), f(0.0f)));
    const Id fy = ctx.OpConvertFToU(ctx.U32[1], ctx.OpFMax(ctx.F32[1], EmitGetAttribute(ctx, IR::Attribute::FragCoord, 1, 0), f(0.0f)));
    // Every SampleEvery-th pair of columns and rows: a 2x2 coarse shading rate's pixel (its centre
    // at an odd coordinate) is counted too (the foveated edges' 4x4 ones are not).
    const Id grid = u(Look::SampleEvery / 2 - 1);
    const auto on_line = [&](Id c) {
        return ctx.OpIEqual(ctx.U1[1],
                            ctx.OpBitwiseAnd(ctx.U32[1], ctx.OpShiftRightLogical(ctx.U32[1], c, u(1u)), grid),
                            ctx.u32_zero_value);
    };
    const Id on_grid = ctx.OpLogicalAnd(ctx.U1[1], on_line(fx), on_line(fy));
    const Id slot = ctx.OpUMod(ctx.U32[1], frame, u(3u));
    const Id sum_index = ctx.OpIAdd(ctx.U32[1], u(8u), ctx.OpIMul(ctx.U32[1], slot, u(2u)));
    {
        const Id lum = ctx.OpDot(ctx.F32[1], FiniteClamp(ctx, x, 0.0f, 65504.0f, 3), ctx.ConstF32(0.2126f, 0.7152f, 0.0722f));
        const Id level = ctx.OpFClamp(ctx.F32[1], ctx.OpLog2(ctx.F32[1], ctx.OpFMax(ctx.F32[1], lum, f(1e-7f))),
                                      f(-24.0f), f(16.0f));
        const Id fixed = ctx.OpBitcast(ctx.U32[1], ctx.OpConvertFToS(ctx.S32[1], mul(level, f(Look::SumScale))));
        const Id then_label = ctx.OpLabel(), merge = ctx.OpLabel();
        ctx.OpSelectionMerge(merge, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(on_grid, then_label, merge);
        ctx.AddLabel(then_label);
        const Id scope = u(static_cast<u32>(spv::Scope::Device));
        ctx.OpAtomicIAdd(ctx.U32[1], ptr(sum_index), scope, ctx.u32_zero_value, fixed);
        ctx.OpAtomicIAdd(ctx.U32[1], ptr(ctx.OpIAdd(ctx.U32[1], sum_index, u(1u))), scope,
                         ctx.u32_zero_value, u(1u));
        ctx.OpBranch(merge);
        ctx.AddLabel(merge);
    }
    // The frame's updater (the first eye's): the first pixel to claim the frame (an atomic swap of
    // dword 5) among those of a 16x16 block at (512, 512), well inside the view at any size (the
    // corner pixel lies in the foveated edge, shaded at a coarser rate: it never ran). It turns
    // the last frame's sum into the next frame's state and clears the next frame's sum.
    Id candidate = ctx.OpLogicalAnd(
        ctx.U1[1], ctx.OpIEqual(ctx.U1[1], ctx.OpShiftRightLogical(ctx.U32[1], fx, u(4u)), u(32u)),
        ctx.OpIEqual(ctx.U1[1], ctx.OpShiftRightLogical(ctx.U32[1], fy, u(4u)), u(32u)));
    if (ctx.runtime_info.native_stereo && ctx.runtime_info.native_multiview) {
        candidate = ctx.OpLogicalAnd(ctx.U1[1], candidate,
                                     ctx.OpIEqual(ctx.U1[1], ctx.NativeEyeIndex(), ctx.u32_zero_value));
    }
    const Id claim_var = Private(ctx, ctx.U1[1]);
    ctx.OpStore(claim_var, ctx.false_value);
    {
        const Id try_label = ctx.OpLabel(), tried = ctx.OpLabel();
        ctx.OpSelectionMerge(tried, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(candidate, try_label, tried);
        ctx.AddLabel(try_label);
        const Id tag = ctx.OpIAdd(ctx.U32[1], frame, u(1u)); // never 0, the buffer's start
        const Id was = ctx.OpAtomicExchange(ctx.U32[1], ptr(u(5u)),
                                            u(static_cast<u32>(spv::Scope::Device)),
                                            ctx.u32_zero_value, tag);
        ctx.OpStore(claim_var, ctx.OpINotEqual(ctx.U1[1], was, tag));
        ctx.OpBranch(tried);
        ctx.AddLabel(tried);
    }
    const Id first = ctx.OpLoad(ctx.U1[1], claim_var);
    {
        const Id then_label = ctx.OpLabel(), merge = ctx.OpLabel();
        ctx.OpSelectionMerge(merge, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(first, then_label, merge);
        ctx.AddLabel(then_label);
        const Id prev = ctx.OpUMod(ctx.U32[1], ctx.OpIAdd(ctx.U32[1], frame, u(2u)), u(3u));
        const Id prev_index = ctx.OpIAdd(ctx.U32[1], u(8u), ctx.OpIMul(ctx.U32[1], prev, u(2u)));
        const Id total = ctx.OpConvertSToF(ctx.F32[1], ctx.OpBitcast(ctx.S32[1], load(prev_index)));
        const Id count = ctx.OpConvertUToF(ctx.F32[1], load(ctx.OpIAdd(ctx.U32[1], prev_index, u(1u))));
        const Id have = ctx.OpFOrdGreaterThan(ctx.U1[1], count, f(0.5f));
        const Id average = ctx.OpSelect(
            ctx.F32[1], have,
            ctx.OpFClamp(ctx.F32[1],
                         ctx.OpFDiv(ctx.F32[1], total, mul(ctx.OpFMax(ctx.F32[1], count, f(1.0f)), f(Look::SumScale))),
                         f(-24.0f), f(16.0f)),
            current);
        const Id next_current = ctx.OpSelect(ctx.F32[1], valid, add(current, mul(fast_rate, sub(average, current))), average);
        // Into the light quickly, into the dark slowly (an eye's two adaptations).
        const Id rate = ctx.OpSelect(ctx.F32[1], ctx.OpFOrdGreaterThan(ctx.U1[1], next_current, adapted),
                                     slow_rate, slow_down_rate);
        const Id next_adapted = ctx.OpSelect(ctx.F32[1], valid, add(adapted, mul(rate, sub(next_current, adapted))), average);
        store(write_base, ctx.OpBitcast(ctx.U32[1], FiniteClamp(ctx, next_current, -24.0f, 16.0f)));
        store(ctx.OpIAdd(ctx.U32[1], write_base, u(1u)),
              ctx.OpBitcast(ctx.U32[1], FiniteClamp(ctx, next_adapted, -24.0f, 16.0f)));
        store(u(4u), ctx.OpSelect(ctx.U32[1], have, u(1u), load(u(4u))));
        const Id next = ctx.OpUMod(ctx.U32[1], ctx.OpIAdd(ctx.U32[1], frame, u(1u)), u(3u));
        const Id next_index = ctx.OpIAdd(ctx.U32[1], u(8u), ctx.OpIMul(ctx.U32[1], next, u(2u)));
        store(next_index, ctx.u32_zero_value);
        store(ctx.OpIAdd(ctx.U32[1], next_index, u(1u)), ctx.u32_zero_value);
        ctx.OpBranch(merge);
        ctx.AddLabel(merge);
    }
    return exposure;
}

Id EmitVrFilmic(EmitContext& ctx, Id x, Id e) {
    namespace Look = Shader::Optimization::CinematicLook;
    const auto& buffers = ctx.info.buffers;
    const auto found = std::ranges::find(buffers, BufferType::VrGrade, &BufferResource::buffer_type);
    const auto out4 = [&](Id rgb) {
        return ctx.OpCompositeConstruct(ctx.F32[4],
                                        std::array{ctx.OpCompositeExtract(ctx.F32[1], rgb, 0u),
                                                   ctx.OpCompositeExtract(ctx.F32[1], rgb, 1u),
                                                   ctx.OpCompositeExtract(ctx.F32[1], rgb, 2u),
                                                   ctx.ConstF32(0.0f)});
    };
    if (found == buffers.end()) {
        return out4(e);
    }
    const u32 grade = static_cast<u32>(std::distance(buffers.begin(), found));
    const Id share = ctx.OpBitcast(ctx.F32[1], FogRead(ctx, grade, ctx.ConstU32(15u)));
    const auto X = [&](Id v, u32 i) { return ctx.OpCompositeExtract(ctx.F32[1], v, i); };
    const Id m = ctx.OpFMax(ctx.F32[1], X(x, 0), ctx.OpFMax(ctx.F32[1], X(x, 1), X(x, 2)));
    const Id e_min = ctx.OpFMin(ctx.F32[1], X(e, 0), ctx.OpFMin(ctx.F32[1], X(e, 1), X(e, 2)));
    // t of the brightest channel per unit of light (x >= the floor 2^-14, so m > 0).
    const Id t_max = ctx.OpFSub(ctx.F32[1], ctx.ConstF32(1.0f), e_min);
    const Id per = ctx.OpFDiv(ctx.F32[1], t_max, m);
    const Id one3 = ctx.ConstF32(1.0f, 1.0f, 1.0f);
    const Id hue_e = ctx.OpFSub(ctx.F32[3], one3, ctx.OpVectorTimesScalar(ctx.F32[3], x, per));
    // Only the bright end (the brightest channel past FilmicFrom of white, all of it from
    // FilmicFull): the hue-keeping curve is darker in the other channels, which would also deepen
    // midtones and haze; they keep the game's curve.
    const Id ramp = ctx.OpFClamp(
        ctx.F32[1],
        ctx.OpFMul(ctx.F32[1], ctx.OpFSub(ctx.F32[1], t_max, ctx.ConstF32(Look::FilmicFrom)),
                   ctx.ConstF32(1.0f / (Look::FilmicFull - Look::FilmicFrom))),
        ctx.ConstF32(0.0f), ctx.ConstF32(1.0f));
    const Id weight = ctx.OpFMul(
        ctx.F32[1], share,
        ctx.OpFMul(ctx.F32[1], ctx.OpFMul(ctx.F32[1], ramp, ramp),
                   ctx.OpFSub(ctx.F32[1], ctx.ConstF32(3.0f),
                              ctx.OpFMul(ctx.F32[1], ctx.ConstF32(2.0f), ramp))));
    const Id moved = ctx.OpFAdd(
        ctx.F32[3], e,
        ctx.OpVectorTimesScalar(ctx.F32[3], ctx.OpFSub(ctx.F32[3], hue_e, e), weight));
    // An infinite or NaN channel (x / m of an infinite scene) keeps the game's own e.
    return out4(Finite(ctx, moved, e, 3));
}

} // namespace Shader::Backend::SPIRV
