// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "../../../../scratchpad/dual-eye-perf/lighting_reconstruction.h"
#include <algorithm>

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/ir/debug_print.h"
#include "shader_recompiler/vr_push_data.h"

namespace Shader::Backend::SPIRV {

// VR body cutaway (FragmentRuntimeInfo::vr_cut): a fragment of the hunter's own body nearer to the
// first-person camera than `near` is discarded; between `near` and `far` it is kept with the
// probability smoothstep(near, far, depth) against interleaved gradient noise on the pixel grid
// (a stable dither, no hard edge). The view depth is 1 / gl_FragCoord.w (w_clip of a perspective
// projection); an orthographic pass (w = 1, shadow maps) reads as 1 unit and so is never cut.
// A cut fragment gets SampleMask 0 (no colour, no depth written), so no control flow is added to
// the entry block; the rest of the shader runs as before. Bit 31 of the code: back-facing
// fragments are cut too, at any depth (the inside of the body never shows through the opening).
// VrPushData: near and the fade's scale are the draw's (vr_cut is only this permutation's flag),
// a negative scale cuts the back faces; the same arithmetic as with the code's baked constants.
void EmitVrCutaway(EmitContext& ctx) {
    const bool pushed = ctx.UsesVrPushData();
    const VrCut baked = VrCut::Decode(ctx.runtime_info.fs_info.vr_cut);
    const bool cull_back = (pushed || baked.back) && Sirit::ValidId(ctx.front_facing);
    const Id type{ctx.F32[1]};
    const Id coord{ctx.OpLoad(ctx.F32[4], ctx.frag_coord)};
    const Id x{ctx.OpCompositeExtract(type, coord, 0u)};
    const Id y{ctx.OpCompositeExtract(type, coord, 1u)};
    const Id w{ctx.OpCompositeExtract(type, coord, 3u)};
    const Id depth{ctx.OpFDiv(type, ctx.Constant(type, 1.0f), w)};
    const Id signed_scale{pushed ? ctx.PushF32(VrPushData::CutScaleIndex) : Id{}};
    const Id t{ctx.OpFClamp(type,
                            ctx.OpFMul(type,
                                       ctx.OpFSub(type, depth,
                                                  pushed ? ctx.PushF32(VrPushData::CutNearIndex)
                                                         : ctx.Constant(type, baked.near_units)),
                                       pushed ? ctx.OpFAbs(type, signed_scale)
                                              : ctx.Constant(type, baked.scale)),
                            ctx.Constant(type, 0.0f), ctx.Constant(type, 1.0f))};
    const Id keep{ctx.OpFMul(type, ctx.OpFMul(type, t, t),
                             ctx.OpFSub(type, ctx.Constant(type, 3.0f),
                                        ctx.OpFMul(type, ctx.Constant(type, 2.0f), t)))};
    // Interleaved gradient noise (Jimenez 2014): fract(52.98 * fract(0.0671 x + 0.00584 y)).
    const Id dot{ctx.OpFAdd(type, ctx.OpFMul(type, x, ctx.Constant(type, 0.06711056f)),
                            ctx.OpFMul(type, y, ctx.Constant(type, 0.00583715f)))};
    const Id noise{ctx.OpFract(type, ctx.OpFMul(type, ctx.Constant(type, 52.9829189f),
                                                ctx.OpFract(type, dot)))};
    Id cut{ctx.OpFOrdLessThanEqual(ctx.U1[1], keep, noise)};
    if (cull_back) {
        // The inside of the body (back faces: the coat's lining, the far side of the torso shell)
        // is never drawn, so the cut opening shows what is beyond the body, not a hollow.
        Id back_face{
            ctx.OpLogicalNot(ctx.U1[1], ctx.OpLoad(ctx.U1[1], ctx.front_facing))};
        if (pushed) {
            back_face = ctx.OpLogicalAnd(
                ctx.U1[1], ctx.OpFOrdLessThan(ctx.U1[1], signed_scale, ctx.Constant(type, 0.0f)),
                back_face);
        }
        cut = ctx.OpLogicalOr(ctx.U1[1], cut, back_face);
    }
    const Id mask{ctx.OpSelect(ctx.U32[1], cut, ctx.u32_zero_value, ctx.ConstU32(0xFFFFFFFFu))};
    ctx.OpStore(ctx.OpAccessChain(ctx.output_u32, ctx.sample_mask, ctx.u32_zero_value), mask);
}

void EmitPrologue(EmitContext& ctx) {
    if (ctx.stage == Stage::Fragment) {
        ctx.DefineAmdPerVertexAttribs();
        if (ctx.runtime_info.native_light_scissor) {
            // Hardware multiview shares one scissor. Its union is conservative;
            // this half-open per-eye test preserves the guest's disjoint light tiles.
            // VrPushData: the draw's tiles (all pixels pass for a draw without its own tile).
            const bool pushed=ctx.UsesVrPushData();
            const Id left=ctx.OpIEqual(ctx.U1[1],ctx.NativeEyeIndex(),ctx.ConstU32(0u));
            Id outside=ctx.false_value;
            for (u32 axis=0;axis<2;++axis) {
                const Id pixel=EmitGetAttribute(ctx,IR::Attribute::FragCoord,axis,0);
                const auto bound=[&](u32 i) {
                    if (pushed) return ctx.PushEyeF32(left,VrPushData::LightIndex,i);
                    return ctx.OpSelect(ctx.F32[1],left,ctx.Constant(ctx.F32[1],ctx.runtime_info.native_light_bounds[0][i]),
                                        ctx.Constant(ctx.F32[1],ctx.runtime_info.native_light_bounds[1][i]));
                };
                const Id low=ctx.OpFOrdLessThan(ctx.U1[1],pixel,bound(axis));
                const Id high=ctx.OpFOrdGreaterThanEqual(ctx.U1[1],pixel,bound(axis+2));
                outside=ctx.OpLogicalOr(ctx.U1[1],outside,ctx.OpLogicalOr(ctx.U1[1],low,high));
            }
            EmitDiscardCond(ctx,outside);
        }
        if (ctx.runtime_info.fs_info.vr_cut != 0 && Sirit::ValidId(ctx.frag_coord) &&
            Sirit::ValidId(ctx.sample_mask) && !ctx.info.stores.Get(IR::Attribute::SampleMask)) {
            EmitVrCutaway(ctx);
        }
    }
    if (ctx.info.loads.Get(IR::Attribute::WorkgroupIndex)) {
        ctx.DefineWorkgroupIndex();
    }
    ctx.DefineBufferProperties();
}

void ConvertDepthMode(EmitContext& ctx) {
    const Id type{ctx.F32[1]};
    const Id position{ctx.OpLoad(ctx.F32[4], ctx.output_position)};
    const Id z{ctx.OpCompositeExtract(type, position, 2u)};
    const Id w{ctx.OpCompositeExtract(type, position, 3u)};
    const Id screen_depth{ctx.OpFMul(type, ctx.OpFAdd(type, z, w), ctx.Constant(type, 0.5f))};
    const Id vector{ctx.OpCompositeInsert(ctx.F32[4], screen_depth, position, 2u)};
    ctx.OpStore(ctx.output_position, vector);
}

void ConvertPositionToClipSpace(EmitContext& ctx) {
    ASSERT_MSG(!ctx.info.stores.GetAny(IR::Attribute::ViewportIndex),
               "Multi-viewport with shader clip space conversion not yet implemented.");

    const Id type{ctx.F32[1]};
    Id position{ctx.OpLoad(ctx.F32[4], ctx.output_position)};
    const Id x{ctx.OpCompositeExtract(type, position, 0u)};
    const Id y{ctx.OpCompositeExtract(type, position, 1u)};
    const Id z{ctx.OpCompositeExtract(type, position, 2u)};
    const Id w{ctx.OpCompositeExtract(type, position, 3u)};
    const Id xoffset_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                           ctx.push_data_block,
                                           ctx.ConstU32(PushData::XOffsetIndex))};
    const Id xoffset{ctx.OpLoad(type, xoffset_ptr)};
    const Id yoffset_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                           ctx.push_data_block,
                                           ctx.ConstU32(PushData::YOffsetIndex))};
    const Id yoffset{ctx.OpLoad(type, yoffset_ptr)};
    const Id xscale_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                          ctx.push_data_block,
                                          ctx.ConstU32(PushData::XScaleIndex))};
    const Id xscale{ctx.OpLoad(type, xscale_ptr)};
    const Id yscale_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                          ctx.push_data_block,
                                          ctx.ConstU32(PushData::YScaleIndex))};
    const Id yscale{ctx.OpLoad(type, yscale_ptr)};
    const Id vport_w =
        ctx.Constant(type, float(std::min<u32>(ctx.profile.max_viewport_width / 2, 8_KB)));
    const Id wnd_x = ctx.OpFAdd(type, ctx.OpFMul(type, x, xscale), xoffset);
    const Id ndc_x = ctx.OpFSub(type, ctx.OpFDiv(type, wnd_x, vport_w), ctx.Constant(type, 1.f));
    const Id vport_h =
        ctx.Constant(type, float(std::min<u32>(ctx.profile.max_viewport_height / 2, 8_KB)));
    const Id wnd_y = ctx.OpFAdd(type, ctx.OpFMul(type, y, yscale), yoffset);
    const Id ndc_y = ctx.OpFSub(type, ctx.OpFDiv(type, wnd_y, vport_h), ctx.Constant(type, 1.f));
    const Id vector{ctx.OpCompositeConstruct(ctx.F32[4], std::array<Id, 4>({ndc_x, ndc_y, z, w}))};
    ctx.OpStore(ctx.output_position, vector);
}

// VR stereo, applied to the final position of every vertex shader:
//  - perspective vertices (w != 1): x_clip += vr_shift. For a projection without x/z
//    coupling this equals translating the camera along its right axis, so each eye gets
//    geometrically correct parallax.
//  - aa_mode=taa jittered scene draws (vr_hud < -1): perspective vertices move by
//    (vr_shift, vr_hud + 2) * w in clip space, a sub-pixel off-centre projection.
//  - HUD/menu vertices (w == 1, flagged per draw through vr_hud > 0): scaled toward the
//    centre so the HUD sits inside the headset FOV, plus a small offset that places it
//    at a comfortable stereo depth. Full-screen passes (vertices on the screen edge) and
//    other screen-space work are left untouched.
void ApplyStereoShift(EmitContext& ctx) {
    const Id type{ctx.F32[1]};
    const Id bool_type{ctx.U1[1]};
    const auto push_float = [&](u32 index) {
        const Id ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                       ctx.push_data_block, ctx.ConstU32(index))};
        return ctx.OpLoad(type, ptr);
    };
    const Id zero{ctx.Constant(type, 0.0f)};
    const Id position{ctx.OpLoad(ctx.F32[4], ctx.output_position)};
    const Id x{ctx.OpCompositeExtract(type, position, 0u)};
    const Id y{ctx.OpCompositeExtract(type, position, 1u)};
    const Id w{ctx.OpCompositeExtract(type, position, 3u)};
    const Id shift{push_float(PushData::VrShiftIndex)};
    const Id hud{push_float(PushData::VrHudIndex)};
    if (ctx.runtime_info.native_stereo) {
        if (!ctx.runtime_info.native_multiview) return;
        const Id flags=ctx.OpConvertFToU(ctx.U32[1],hud);
        const auto flag=[&](u32 mask) {
            return ctx.OpINotEqual(bool_type,ctx.OpBitwiseAnd(ctx.U32[1],flags,ctx.ConstU32(mask)),ctx.ConstU32(0u));
        };
        const Id perspective=ctx.OpFOrdGreaterThan(bool_type,
            ctx.OpFAbs(type,ctx.OpFSub(type,w,ctx.Constant(type,1.f))),ctx.Constant(type,1e-5f));
        const int uv_param = NativeProjection::ScreenQuadUvParam(ctx.info.pgm_hash);
        const bool light_rectangle = uv_param >= 0;
        const Id world=ctx.OpLogicalAnd(bool_type,flag(PushData::NativeWorld),
            light_rectangle ? ctx.true_value : perspective);
        const Id eye=ctx.NativeEyeIndex();
        const Id left=ctx.OpIEqual(bool_type,eye,ctx.ConstU32(0u));
        // The asymmetric remap: baked into this permutation, or (VrPushData) the draw's own, used
        // only while both eyes have one (x_scale > 0). Without one, every value below is the
        // unremapped one (selected, not an identity transform): what a permutation without a
        // projection computes.
        const auto& baked=ctx.runtime_info.native_projection;
        const bool pushed=ctx.runtime_info.native_asymmetric && ctx.UsesVrPushData();
        const bool remap=pushed || (baked[0][0]>0 && baked[1][0]>0);
        const auto component=[&](u32 i) {
            if (pushed) return ctx.PushEyeF32(left,VrPushData::ProjectionIndex,i);
            return ctx.OpSelect(type,left,ctx.Constant(type,baked[0][i]),ctx.Constant(type,baked[1][i]));
        };
        Id valid{};
        if (pushed) {
            valid=ctx.OpLogicalAnd(bool_type,
                ctx.OpFOrdGreaterThan(bool_type,ctx.PushF32(VrPushData::ProjectionIndex,0u),zero),
                ctx.OpFOrdGreaterThan(bool_type,ctx.PushF32(VrPushData::ProjectionIndex+1,0u),zero));
        }
        const auto remapped=[&](Id condition) {
            return pushed ? ctx.OpLogicalAnd(bool_type,condition,valid) : condition;
        };
        Id offset=ctx.OpSelect(type,left,shift,ctx.OpFNegate(type,shift));
        if (remap) {
            const Id scale=component(0);
            offset=ctx.OpSelect(type,remapped(flag(PushData::NativeProjectionPaired)),ctx.OpFMul(type,offset,scale),offset);
        }
        const Id translate=ctx.OpLogicalAnd(bool_type,ctx.OpLogicalAnd(bool_type,world,perspective),
            ctx.OpLogicalNot(bool_type,flag(PushData::NativeCameraPaired)));
        const Id px=ctx.OpFAdd(type,x,ctx.OpSelect(type,translate,offset,zero));
        Id result=ctx.OpCompositeInsert(ctx.F32[4],px,position,0u);
        if (remap) {
            const Id active=remapped(ctx.OpLogicalAnd(bool_type,world,ctx.OpLogicalNot(bool_type,flag(PushData::NativeProjectionPaired))));
            const Id tx=ctx.OpFAdd(type,ctx.OpFMul(type,px,component(0)),ctx.OpFMul(type,w,component(2)));
            const Id ty=ctx.OpFAdd(type,ctx.OpFMul(type,y,component(1)),ctx.OpFMul(type,w,component(3)));
            result=ctx.OpCompositeInsert(ctx.F32[4],ctx.OpSelect(type,active,tx,px),result,0u);
            result=ctx.OpCompositeInsert(ctx.F32[4],ctx.OpSelect(type,active,ty,y),result,1u);
        }
        // The light rectangle and glow quads carry separately fetched UVs: the affine-export
        // matcher cannot infer their relationship to Position. Move those UVs with
        // the rectangle so depth/G-buffer samples still address the rasterized pixel.
        if (light_rectangle && remap) {
            const auto& uv=ctx.output_params[uv_param];
            const Id moved=remapped(world);
            for (u32 axis=0;axis<2;++axis) {
                const Id ptr=ctx.OpAccessChain(uv.pointer_type,uv.id,ctx.ConstU32(axis));
                const Id before=ctx.OpLoad(type,ptr);
                const Id half=ctx.Constant(type,.5f);
                const Id centered=ctx.OpFMul(type,ctx.OpFSub(type,before,half),component(axis));
                const Id bias=ctx.OpFMul(type,ctx.Constant(type,axis==0?.5f:-.5f),component(axis+2));
                const Id after=ctx.OpFAdd(type,ctx.OpFAdd(type,centered,half),bias);
                ctx.OpStore(ptr,ctx.OpSelect(type,moved,after,before));
            }
        }
        ctx.OpStore(ctx.output_position,result);
        // Apply precisely the same position correction to recognized screen exports.
        // Deltas preserve each expression's scale/bias and leave unrelated UVs alone.
        for (const auto& e:ctx.native_screen_exports) {
            const auto& out=ctx.output_params[e.param];
            const Id ptr=ctx.OpAccessChain(out.pointer_type,out.id,ctx.ConstU32(e.component));
            const Id before=ctx.OpCompositeExtract(type,position,e.axis);
            const Id after=ctx.OpCompositeExtract(type,result,e.axis);
            Id delta=ctx.OpFSub(type,after,before);
            if (e.normalized) delta=ctx.OpFDiv(type,delta,w);
            if (!e.dynamic_scale.IsEmpty()) delta=ctx.OpFMul(type,delta,ctx.Def(e.dynamic_scale));
            const Id value=ctx.OpFAdd(type,ctx.OpLoad(type,ptr),ctx.OpFMul(type,ctx.Constant(type,e.slope),delta));
            ctx.OpStore(ptr,ctx.OpSelect(type,world,value,ctx.OpLoad(type,ptr)));
        }
        return;
    }
    if (ctx.runtime_info.vs_info.foreground_probe != 0) {
        Id offset = shift;
        if (ctx.runtime_info.vs_info.foreground_probe == 2) {
            const Id eye = ctx.OpLoad(ctx.U32[1], ctx.probe_view_index);
            offset = ctx.OpSelect(type, ctx.OpIEqual(bool_type, eye, ctx.ConstU32(0u)),
                                  shift, ctx.OpFNegate(type, shift));
        }
        // Parallel eye cameras, same vertical projection as the source. No DIBR
        // near compression. Target aspect 1568/1696; source aspect 2720/1808.
        const Id sx = ctx.Constant(type, (2720.f / 1808.f) / (1568.f / 1696.f));
        const Id eye_x = ctx.OpFMul(type, ctx.OpFAdd(type, x, offset), sx);
        ctx.OpStore(ctx.output_position, ctx.OpCompositeInsert(ctx.F32[4], eye_x, position, 0u));
        return;
    }

    const Id distance{ctx.OpFAbs(type, ctx.OpFSub(type, w, ctx.Constant(type, 1.0f)))};
    const Id perspective{ctx.OpFOrdGreaterThan(bool_type, distance, ctx.Constant(type, 1e-5f))};
    // aa_mode=taa (core/vr/render_taa.h): a jittered scene draw carries vr_hud = -2 + jy and
    // vr_shift = jx (NDC); its perspective vertices move by (jx, jy) * w, an exact sub-pixel
    // off-centre projection (the same step everywhere on the screen).
    const Id jit{ctx.OpFOrdLessThan(bool_type, hud, ctx.Constant(type, -1.0f))};
    const Id jit_y{ctx.OpFAdd(type, hud, ctx.Constant(type, 2.0f))};
    const Id step_x{ctx.OpSelect(type, jit, ctx.OpFMul(type, shift, w), shift)};
    const Id step_y{ctx.OpSelect(type, jit, ctx.OpFMul(type, jit_y, w), zero)};
    const Id geometry_x{ctx.OpFAdd(type, x, ctx.OpSelect(type, perspective, step_x, zero))};
    const Id geometry_y{ctx.OpFAdd(type, y, ctx.OpSelect(type, perspective, step_y, zero))};

    const Id hud_steps{ctx.OpRoundEven(type, hud)};
    const Id hud_scale{ctx.OpFMul(type, hud_steps, ctx.Constant(type, 1.0f / 16.0f))};
    const Id hud_offset{ctx.OpFSub(type, hud, hud_steps)};
    const Id extent{ctx.OpFMax(type, ctx.OpFAbs(type, x), ctx.OpFAbs(type, y))};
    const Id inside{ctx.OpFOrdLessThan(bool_type, extent, ctx.Constant(type, 0.999f))};
    const Id is_hud{ctx.OpLogicalAnd(
        bool_type,
        ctx.OpLogicalAnd(bool_type, ctx.OpFOrdGreaterThan(bool_type, hud, zero), inside),
        ctx.OpLogicalNot(bool_type, perspective))};
    const Id hud_x{ctx.OpFAdd(type, ctx.OpFMul(type, x, hud_scale), hud_offset)};
    const Id hud_y{ctx.OpFMul(type, y, hud_scale)};

    const Id final_x{ctx.OpSelect(type, is_hud, hud_x, geometry_x)};
    const Id final_y{ctx.OpSelect(type, is_hud, hud_y, geometry_y)};
    Id result{ctx.OpCompositeInsert(ctx.F32[4], final_x, position, 0u)};
    result = ctx.OpCompositeInsert(ctx.F32[4], final_y, result, 1u);
    ctx.OpStore(ctx.output_position, result);
}

static_assert(((PushData::NativeWorld | PushData::NativeCameraPaired |
                PushData::NativeProjectionPaired) & ~VrMotion::NativeFlagsMask) == 0);
#include "shader_recompiler/backend/spirv/emit_spirv_vr_motion.inc"

void EmitEpilogue(EmitContext& ctx) {
    if (ctx.stage == Stage::Vertex && ctx.runtime_info.vs_info.emulate_depth_negative_one_to_one) {
        ConvertDepthMode(ctx);
    }
    if (ctx.stage == Stage::Vertex && ctx.runtime_info.vs_info.clip_disable) {
        ConvertPositionToClipSpace(ctx);
    }
    // Screen-space (clip_disable) shaders use a virtual viewport, so they are left alone.
    if (ctx.stage == Stage::Vertex && !ctx.runtime_info.vs_info.clip_disable) {
        ApplyStereoShift(ctx);
    }
    // Store the final per-eye position, after the native stereo/asymmetric remap.
    if (Sirit::ValidId(ctx.motion_out_cur)) {
        EmitVertexMotion(ctx);
    }
    if (Sirit::ValidId(ctx.motion_frag_out)) {
        EmitFragmentMotion(ctx);
    }
}

void EmitDiscard(EmitContext& ctx) {
    ctx.OpDemoteToHelperInvocationEXT();
}

void EmitDiscardCond(EmitContext& ctx, Id condition) {
    const Id kill_label{ctx.OpLabel()};
    const Id merge_label{ctx.OpLabel()};
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(condition, kill_label, merge_label);
    ctx.AddLabel(kill_label);
    ctx.OpDemoteToHelperInvocationEXT();
    ctx.OpBranch(merge_label);
    ctx.AddLabel(merge_label);
}

void EmitEmitVertex(EmitContext& ctx) {
    if (ctx.runtime_info.native_multiview) {
        // The stereo correction is per emitted vertex: the position and its recognized screen
        // copies are restored after the emit, as the guest program wrote them.
        const Id original=ctx.OpLoad(ctx.F32[4],ctx.output_position);
        std::vector<std::pair<Id,Id>> exports;
        for (const auto& e:ctx.native_screen_exports) {
            const auto& out=ctx.output_params[e.param];
            const Id ptr=ctx.OpAccessChain(out.pointer_type,out.id,ctx.ConstU32(e.component));
            exports.emplace_back(ptr,ctx.OpLoad(ctx.F32[1],ptr));
        }
        ApplyStereoShift(ctx);
        ctx.OpEmitVertex();
        ctx.OpStore(ctx.output_position,original);
        for (const auto& [ptr,value]:exports) ctx.OpStore(ptr,value);
    } else {
        ctx.OpEmitVertex();
    }
}

void EmitEmitPrimitive(EmitContext& ctx) {
    ctx.OpEndPrimitive();
}

void EmitEmitVertex(EmitContext& ctx, const IR::Value& stream) {
    UNREACHABLE_MSG("Geometry streams");
}

void EmitEndPrimitive(EmitContext& ctx, const IR::Value& stream) {
    UNREACHABLE_MSG("Geometry streams");
}

void EmitDebugPrint(EmitContext& ctx, IR::Inst* inst, Id fmt, Id arg0, Id arg1, Id arg2, Id arg3) {
    IR::DebugPrintFlags flags = inst->Flags<IR::DebugPrintFlags>();
    std::array<Id, IR::DEBUGPRINT_NUM_FORMAT_ARGS> fmt_args = {arg0, arg1, arg2, arg3};
    auto fmt_args_span = std::span<Id>(fmt_args.begin(), fmt_args.begin() + flags.num_args);
    ctx.OpDebugPrintf(fmt, fmt_args_span);
}

} // namespace Shader::Backend::SPIRV
