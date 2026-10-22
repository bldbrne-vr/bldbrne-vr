// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// bbport: object motion vectors (runtime_info.h, MotionVectors). The vertex shader stores its
// clip position for this frame and loads the one of the previous frame (same draw, same
// vertex), both by buffer device address. Disabled accesses are branched around: a shared
// scratch element would race between all the inactive vertex invocations. The two buffers'
// addresses are the draw's (VrPushData) or baked into this permutation (RuntimeInfo::vr_motion).
static void EmitVertexMotion(EmitContext& ctx) {
    const Id u32_type = ctx.U32[1];
    const Id position = ctx.OpLoad(ctx.F32[4], ctx.output_position);
    // Preserve native stereo flags in vr_hud's low three bits. The integer is
    // encoded numerically as float (not bitcast), matching ApplyStereoShift.
    const Id param_ptr =
        ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, ctx.F32[1]),
                          ctx.push_data_block, ctx.ConstU32(PushData::VrHudIndex));
    const Id pair_index = ctx.OpShiftRightLogical(
        u32_type, ctx.OpConvertFToU(u32_type, ctx.OpLoad(ctx.F32[1], param_ptr)),
        ctx.ConstU32(VrMotion::ParameterShift));
    const Id param_index = ctx.OpIAdd(u32_type, pair_index, ctx.NativeEyeIndex());
    // VrPushData: each address is a uvec2 {low, high} of the push block, bitcast to a u64.
    const bool pushed = ctx.UsesVrPushData();
    const auto pushed_address = [&](u32 member) {
        const Id ptr =
            ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, ctx.U32[2]),
                              ctx.push_data_block, ctx.ConstU32(member));
        return ctx.OpBitcast(ctx.U64, ctx.OpLoad(ctx.U32[2], ptr));
    };
    const Id params_buffer{pushed ? pushed_address(VrPushData::MotionIndex) : Id{}};
    const Id positions_buffer{pushed ? pushed_address(VrPushData::MotionIndex + 1) : Id{}};
    // `offset` bytes into a buffer: the pushed address, or the baked one (a constant, as before).
    const auto base = [&](Id buffer, u64 baked, u64 offset) {
        if (!pushed) {
            return ctx.Constant(ctx.U64, baked + offset);
        }
        return offset == 0 ? buffer : ctx.OpIAdd(ctx.U64, buffer, ctx.Constant(ctx.U64, offset));
    };
    const auto address = [&](Id buffer, u64 baked, u64 offset, Id index, u32 stride) {
        return ctx.OpIAdd(ctx.U64, base(buffer, baked, offset),
                          ctx.OpIMul(ctx.U64, ctx.OpUConvert(ctx.U64, index),
                                     ctx.Constant(ctx.U64, u64(stride))));
    };
    const auto& motion = ctx.runtime_info.vr_motion;
    const Id u32x4_ptr = ctx.TypePointer(spv::StorageClass::PhysicalStorageBuffer, ctx.U32[4]);
    const Id f32x4_ptr = ctx.TypePointer(spv::StorageClass::PhysicalStorageBuffer, ctx.F32[4]);
    const Id params = ctx.OpLoad(
        ctx.U32[4],
        ctx.OpConvertUToPtr(u32x4_ptr,
                            address(params_buffer, motion.params_address, 0, param_index, 32)),
        spv::MemoryAccessMask::Aligned, 16u);
    const Id store_base = ctx.OpCompositeExtract(u32_type, params, 0u);
    const Id load_base = ctx.OpCompositeExtract(u32_type, params, 1u);
    const Id vertices = ctx.OpCompositeExtract(u32_type, params, 2u);
    const Id flags = ctx.OpCompositeExtract(u32_type, params, 3u);
    const Id offsets = ctx.OpLoad(
        ctx.U32[4],
        ctx.OpConvertUToPtr(u32x4_ptr,
                            address(params_buffer, motion.params_address, 16, param_index, 32)),
        spv::MemoryAccessMask::Aligned, 16u);
    const Id first_vertex = ctx.OpCompositeExtract(u32_type, offsets, 0u);
    const Id first_instance = ctx.OpCompositeExtract(u32_type, offsets, 1u);
    const Id instances = ctx.OpCompositeExtract(u32_type, offsets, 2u);
    const Id vertex = ctx.OpISub(u32_type, ctx.OpLoad(u32_type, ctx.vertex_index), first_vertex);
    const Id instance = ctx.OpISub(u32_type, ctx.OpLoad(u32_type, ctx.instance_id), first_instance);
    const Id bool_type = ctx.U1[1];
    const Id in_range = ctx.OpLogicalAnd(bool_type, ctx.OpULessThan(bool_type, vertex, vertices),
                                         ctx.OpULessThan(bool_type, instance, instances));
    const Id slot = ctx.OpIAdd(u32_type, vertex, ctx.OpIMul(u32_type, instance, vertices));
    const auto flag = [&](u32 bit) {
        return ctx.OpLogicalAnd(
            bool_type, in_range,
            ctx.OpINotEqual(bool_type, ctx.OpBitwiseAnd(u32_type, flags, ctx.ConstU32(bit)),
                            ctx.u32_zero_value));
    };
    const Id do_store = flag(VrMotion::FlagStore);
    const Id do_load = flag(VrMotion::FlagLoad);
    const Id store_label = ctx.OpLabel();
    const Id store_merge = ctx.OpLabel();
    ctx.OpSelectionMerge(store_merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(do_store, store_label, store_merge);
    ctx.AddLabel(store_label);
    const Id store_address = address(positions_buffer, motion.positions_address, 0,
                                     ctx.OpIAdd(u32_type, store_base, slot), 16);
    // Indexed draws may invoke the same vertex more than once. Atomic component stores
    // avoid write/write races; all these invocations produce the same clip position.
    const Id scalar_ptr = ctx.TypePointer(spv::StorageClass::PhysicalStorageBuffer, u32_type);
    const Id scope = ctx.ConstU32(static_cast<u32>(spv::Scope::Device));
    for (u32 i = 0; i < 4; ++i) {
        const Id ptr = ctx.OpConvertUToPtr(
            scalar_ptr, ctx.OpIAdd(ctx.U64, store_address, ctx.Constant(ctx.U64, u64(i * 4))));
        const Id bits = ctx.OpBitcast(u32_type, ctx.OpCompositeExtract(ctx.F32[1], position, i));
        ctx.OpAtomicExchange(u32_type, ptr, scope, ctx.u32_zero_value, bits);
    }
    ctx.OpBranch(store_merge);
    ctx.AddLabel(store_merge);

    const Id load_label = ctx.OpLabel();
    const Id load_merge = ctx.OpLabel();
    ctx.OpSelectionMerge(load_merge, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(do_load, load_label, load_merge);
    ctx.AddLabel(load_label);
    const Id loaded = ctx.OpLoad(
        ctx.F32[4],
        ctx.OpConvertUToPtr(f32x4_ptr, address(positions_buffer, motion.positions_address, 0,
                                               ctx.OpIAdd(u32_type, load_base, slot), 16)),
        spv::MemoryAccessMask::Aligned, 16u);
    ctx.OpBranch(load_merge);
    ctx.AddLabel(load_merge);
    Id previous = ctx.OpPhi(ctx.F32[4], position, store_merge, loaded, load_label);
    const Id valid = ctx.OpSelect(ctx.F32[1], do_load, ctx.Constant(ctx.F32[1], 1.0f),
                                  ctx.Constant(ctx.F32[1], 0.0f));
    previous = ctx.OpCompositeInsert(ctx.F32[4], valid, previous, 2u);
    ctx.OpStore(ctx.motion_out_cur, position);
    ctx.OpStore(ctx.motion_out_prev, previous);
}

// Screen-space motion (previous minus current, pixels) through the viewport scale; z = valid.
static void EmitFragmentMotion(EmitContext& ctx) {
    const Id f32_type = ctx.F32[1];
    const Id current = ctx.OpLoad(ctx.F32[4], ctx.motion_in_cur);
    const Id previous = ctx.OpLoad(ctx.F32[4], ctx.motion_in_prev);
    const auto push = [&](u32 index) {
        return ctx.OpLoad(
            f32_type, ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, f32_type),
                                        ctx.push_data_block, ctx.ConstU32(index)));
    };
    Id xscale = push(PushData::XScaleIndex);
    Id yscale = push(PushData::YScaleIndex);
    // Guest viewport scales remain in output pixels for guest shader semantics.
    // Temporal vectors must use the actual raster/input pixels, like camera motion.
    if (ctx.runtime_info.native_stereo && ctx.profile.max_push_constant_size >= 136) {
        xscale = ctx.OpFDiv(f32_type, xscale, push(13u));
        yscale = ctx.OpFDiv(f32_type, yscale, push(14u));
    }
    const auto ndc = [&](Id clip, u32 component) {
        return ctx.OpFDiv(f32_type, ctx.OpCompositeExtract(f32_type, clip, component),
                          ctx.OpCompositeExtract(f32_type, clip, 3u));
    };
    const Id dx =
        ctx.OpFMul(f32_type, ctx.OpFSub(f32_type, ndc(previous, 0u), ndc(current, 0u)), xscale);
    const Id dy =
        ctx.OpFMul(f32_type, ctx.OpFSub(f32_type, ndc(previous, 1u), ndc(current, 1u)), yscale);
    const Id zero = ctx.Constant(f32_type, 0.0f);
    const Id epsilon = ctx.Constant(f32_type, 1e-5f);
    const Id front = ctx.OpLogicalAnd(
        ctx.U1[1],
        ctx.OpFOrdGreaterThan(ctx.U1[1], ctx.OpCompositeExtract(f32_type, previous, 3u), epsilon),
        ctx.OpFOrdGreaterThan(ctx.U1[1], ctx.OpCompositeExtract(f32_type, current, 3u), epsilon));
    const Id valid =
        ctx.OpSelect(f32_type, front, ctx.OpCompositeExtract(f32_type, previous, 2u), zero);
    // Uninstrumented draws can overwrite the final scene depth after this pixel was
    // written. Preserve depth so the compose pass can reject an occluded vector.
    const Id depth = ctx.OpCompositeExtract(f32_type, ctx.OpLoad(ctx.F32[4], ctx.frag_coord), 2u);
    ctx.OpStore(ctx.motion_frag_out,
                ctx.OpCompositeConstruct(ctx.F32[4],
                                         std::array<Id, 4>{ctx.OpSelect(f32_type, front, dx, zero),
                                                           ctx.OpSelect(f32_type, front, dy, zero),
                                                           valid, depth}));
}
