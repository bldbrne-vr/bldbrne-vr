#include "video_core/native_stereo/draw_registers.h"
#include "video_core/native_stereo/owned_optional_reuse.h"
#include "video_core/native_stereo/profile_scopes.h"
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/native_stereo/native_stereo.h"
#include "video_core/native_stereo/lighting_reconstruction.h"
#include "core/vr/vr_session.h"
#include <ranges>

#include "common/hash.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/ir/passes/billboard_face_eye.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/cache_storage.h"
#include "video_core/fast_render.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_compile_trace.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {
namespace {
template<class Info, class Attribs>
void ProjectSpecializationInputs(const Info& info, const Attribs* attribs, PerfMemo::Projection& out) {
    static const bool prepared = [] {
        const char* setting = std::getenv("VR_PREFLIGHT_PROJECTION");
        return !setting || setting[0] != '0';
    }();
    if (!prepared) { PerfMemo::ProjectStage(info, attribs, out); return; }
    PerfMemo::ProjectStagePreflight(info, attribs, out);
    if (PerfMemo::CheckMode()) {
        PerfMemo::Projection reference;
        PerfMemo::ProjectStage(info, attribs, reference);
        ASSERT_MSG(out.count == reference.count && out.overflow == reference.overflow &&
            std::memcmp(out.words.data(), reference.words.data(), out.count * sizeof(u32)) == 0,
            "Preflight projection mismatch");
    }
}
}


using Shader::LogicalStage;
using Shader::Output;
using Shader::Stage;

constexpr static auto SpirvVersion1_6 = 0x00010600U;

constexpr static std::array DescriptorHeapSizes = {
    vk::DescriptorPoolSize{vk::DescriptorType::eUniformBuffer, 512},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageBuffer, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampledImage, 8192},
    vk::DescriptorPoolSize{vk::DescriptorType::eStorageImage, 1024},
    vk::DescriptorPoolSize{vk::DescriptorType::eSampler, 1024},
};

static u32 MapOutputs(std::span<Shader::OutputMap, 3> outputs, const AmdGpu::VsOutputControl& ctl) {
    u32 num_outputs = 0;

    if (ctl.vs_out_misc_enable) {
        auto& misc_vec = outputs[num_outputs++];
        misc_vec[0] = ctl.use_vtx_point_size ? Output::PointSize : Output::None;
        misc_vec[1] = ctl.use_vtx_edge_flag
                          ? Output::EdgeFlag
                          : (ctl.use_vtx_gs_cut_flag ? Output::GsCutFlag : Output::None);
        misc_vec[2] =
            ctl.use_vtx_kill_flag
                ? Output::KillFlag
                : (ctl.use_vtx_render_target_idx ? Output::RenderTargetIndex : Output::None);
        misc_vec[3] = ctl.use_vtx_viewport_idx ? Output::ViewportIndex : Output::None;
    }

    if (ctl.vs_out_ccdist0_enable) {
        auto& ccdist0 = outputs[num_outputs++];
        ccdist0[0] = ctl.IsClipDistEnabled(0)
                         ? Output::ClipDist0
                         : (ctl.IsCullDistEnabled(0) ? Output::CullDist0 : Output::None);
        ccdist0[1] = ctl.IsClipDistEnabled(1)
                         ? Output::ClipDist1
                         : (ctl.IsCullDistEnabled(1) ? Output::CullDist1 : Output::None);
        ccdist0[2] = ctl.IsClipDistEnabled(2)
                         ? Output::ClipDist2
                         : (ctl.IsCullDistEnabled(2) ? Output::CullDist2 : Output::None);
        ccdist0[3] = ctl.IsClipDistEnabled(3)
                         ? Output::ClipDist3
                         : (ctl.IsCullDistEnabled(3) ? Output::CullDist3 : Output::None);
    }

    if (ctl.vs_out_ccdist1_enable) {
        auto& ccdist1 = outputs[num_outputs++];
        ccdist1[0] = ctl.IsClipDistEnabled(4)
                         ? Output::ClipDist4
                         : (ctl.IsCullDistEnabled(4) ? Output::CullDist4 : Output::None);
        ccdist1[1] = ctl.IsClipDistEnabled(5)
                         ? Output::ClipDist5
                         : (ctl.IsCullDistEnabled(5) ? Output::CullDist5 : Output::None);
        ccdist1[2] = ctl.IsClipDistEnabled(6)
                         ? Output::ClipDist6
                         : (ctl.IsCullDistEnabled(6) ? Output::CullDist6 : Output::None);
        ccdist1[3] = ctl.IsClipDistEnabled(7)
                         ? Output::ClipDist7
                         : (ctl.IsCullDistEnabled(7) ? Output::CullDist7 : Output::None);
    }

    return num_outputs;
}

const Shader::RuntimeInfo& PipelineCache::BuildRuntimeInfo(PipelineSelection& selection, Stage stage, LogicalStage l_stage,
                                                           u32 view_mask) {
    ScratchProfile::Cpu scratch_runtime(32,"PipelineCache::BuildRuntimeInfo");
    auto& info = selection.runtime_infos[u32(l_stage)];
    const auto& regs = *selection.regs;
    const auto BuildCommon = [&](const auto& program) {
        info.num_user_data = program.settings.num_user_regs;
        info.num_input_vgprs = program.settings.vgpr_comp_cnt;
        info.num_allocated_vgprs = program.NumVgprs();
        info.fp_denorm_mode32 = program.settings.fp_denorm_mode32;
        info.fp_denorm_mode16_64 = program.settings.fp_denorm_mode64;
        info.fp_round_mode32 = program.settings.fp_round_mode32;
        info.fp_round_mode16_64 = program.settings.fp_round_mode64;
    };
    info.Initialize(stage);
    info.native_stereo = NativeStereo::Enabled();
    // This override is startup-only. getenv scans/locks the process environment;
    // do not repeat it for every shader selection on the draw workers.
    static const char* const billboard_override = std::getenv("VR_BILLBOARD_FACE_EYE");
    info.billboard_face_eye = Shader::Optimization::BillboardFaceEye::Enabled(
        info.native_stereo, VR::IsEnabled(), billboard_override);
    if (stage == Stage::Vertex || stage == Stage::Fragment) info.vr_motion = selection.vr_motion;
    info.native_multiview = info.native_stereo && stage != Stage::Compute && view_mask == 3;
    // VrPushData: each draw pushes its projection, light tiles and cut radii
    // (Rasterizer::VrNativeDrawValues); the runtime info only says that a permutation reads them,
    // so no camera is read here and one permutation serves every camera, tile and radius.
    const bool vr_push =
        info.native_stereo && stage != Stage::Compute && profile.SupportsVrPushData();
    // The geometry stage emits final positions too (ApplyStereoShift before each EmitVertex):
    // particles expanded there from their own camera block need the same per-eye remap.
    if (NativeStereo::AsymmetricProjection() && info.native_multiview &&
        (stage == Stage::Vertex || stage == Stage::Geometry)) {
        if (vr_push) {
            info.native_asymmetric = true;
        } else {
            // Preparation workers carry their camera: VR::DrawFrameRecord locks record_mutex.
            info.native_projection = NativeStereo::DrawProjection(
                selection.camera ? *selection.camera : VR::DrawFrameRecord());
        }
    }
    // A light rectangle with a tile of its own. With VrPushData the tile's registers decide (the
    // bounds are pushed; a draw whose camera has no projection pushes a test every pixel passes).
    const bool light_tile =
        stage == Stage::Fragment && info.native_multiview &&
        (vr_push ? NativeStereo::LightTile(regs)
                 : NativeStereo::LightScissors(
                       regs, selection.camera ? *selection.camera : VR::DrawFrameRecord(),
                       info.native_light_bounds));
    if (light_tile) {
        const auto* vs=regs.ProgramForStage(u32(Stage::Vertex));
        info.native_light_scissor=vs && vs->Address<u32*>() &&
            NativeProjection::LightRectangleShader(selection.prepared_vertex_hash
                ? *selection.prepared_vertex_hash : AmdGpu::GetParams(*vs).hash);
        if (!info.native_light_scissor) info.native_light_bounds={};
    } else info.native_light_bounds={};
    switch (stage) {
    case Stage::Local: {
        BuildCommon(regs.ls_program);
        Shader::TessellationDataConstantBuffer tess_constants{};
        const auto* hull_info = selection.infos[u32(Shader::LogicalStage::TessellationControl)];
        hull_info->ReadTessConstantBuffer(tess_constants);
        info.ls_info.ls_stride = tess_constants.ls_stride;
        break;
    }
    case Stage::Hull: {
        BuildCommon(regs.hs_program);
        info.hs_info.num_input_control_points = regs.ls_hs_config.hs_input_control_points;
        info.hs_info.num_threads = regs.ls_hs_config.hs_output_control_points;
        info.hs_info.tess_type = regs.tess_config.type;
        info.hs_info.offchip_lds_enable = regs.hs_program.settings.oc_lds_en;

        // We need to initialize most hs_info fields after finding the V# with tess constants
        break;
    }
    case Stage::Export: {
        BuildCommon(regs.es_program);
        info.es_info.vertex_data_size = regs.vgt_esgs_ring_itemsize;
        if (l_stage == LogicalStage::TessellationEval) {
            info.es_vs_info.tess_type = regs.tess_config.type;
            info.es_vs_info.tess_topology = regs.tess_config.topology;
            info.es_vs_info.tess_partitioning = regs.tess_config.partitioning;
        }
        break;
    }
    case Stage::Vertex: {
        BuildCommon(regs.vs_program);
        info.vs_info.user_clip_plane_mask = regs.clipper_control.user_clip_plane_enable;
        info.vs_info.step_rate_0 = regs.vgt_instance_step_rate_0;
        info.vs_info.step_rate_1 = regs.vgt_instance_step_rate_1;
        info.vs_info.num_outputs = MapOutputs(info.vs_info.outputs, regs.vs_output_control);
        info.vs_info.emulate_depth_negative_one_to_one =
            !instance.IsDepthClipControlSupported() &&
            regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW;
        info.vs_info.tess_emulated_primitive =
            regs.primitive_type == AmdGpu::PrimitiveType::RectList ||
            regs.primitive_type == AmdGpu::PrimitiveType::QuadList;
        info.vs_info.clip_disable = regs.IsClipDisabled();
        info.vs_info.foreground_probe = selection.vr_probe_mode;
        if (l_stage == LogicalStage::TessellationEval) {
            info.es_vs_info.tess_type = regs.tess_config.type;
            info.es_vs_info.tess_topology = regs.tess_config.topology;
            info.es_vs_info.tess_partitioning = regs.tess_config.partitioning;
        }
        break;
    }
    case Stage::Geometry: {
        BuildCommon(regs.gs_program);
        auto& gs_info = info.gs_info;
        gs_info.num_outputs = MapOutputs(gs_info.outputs, regs.vs_output_control);
        gs_info.output_vertices = regs.vgt_gs_max_vert_out;
        gs_info.num_invocations =
            regs.vgt_gs_instance_cnt.IsEnabled() ? regs.vgt_gs_instance_cnt.count : 1;
        if (regs.stage_enable.raw == AmdGpu::ShaderStageEnable::LsHsEsGs) {
            gs_info.in_primitive = [&]() {
                switch (regs.tess_config.topology) {
                case AmdGpu::TessellationTopology::Point:
                    return AmdGpu::PrimitiveType::PointList;
                case AmdGpu::TessellationTopology::Line:
                    return AmdGpu::PrimitiveType::LineList;
                case AmdGpu::TessellationTopology::TriangleCw:
                case AmdGpu::TessellationTopology::TriangleCcw:
                    return AmdGpu::PrimitiveType::TriangleList;
                default:
                    UNREACHABLE();
                }
            }();
        } else {
            gs_info.in_primitive = regs.primitive_type;
        }
        for (u32 stream_id = 0; stream_id < Shader::GsMaxOutputStreams; ++stream_id) {
            gs_info.out_primitive[stream_id] =
                regs.vgt_gs_out_prim_type.GetPrimitiveType(stream_id);
        }
        gs_info.in_vertex_data_size = regs.vgt_esgs_ring_itemsize;
        gs_info.out_vertex_data_size = regs.vgt_gs_vert_itemsize[0];
        gs_info.mode = regs.vgt_gs_mode.mode;
        const auto params_vc = AmdGpu::GetParams(regs.vs_program);
        gs_info.vs_copy = params_vc.code;
        gs_info.vs_copy_hash = params_vc.hash;
        DumpShader(gs_info.vs_copy, gs_info.vs_copy_hash, Shader::Stage::Vertex, 0, "copy.bin");
        break;
    }
    case Stage::Fragment: {
        BuildCommon(regs.ps_program);
        info.fs_info.en_flags = regs.ps_input_ena;
        info.fs_info.addr_flags = regs.ps_input_addr;
        info.fs_info.num_inputs = regs.num_interp;
        info.fs_info.z_export_format = regs.z_export_format;
        u8 stencil_ref_export_enable = regs.depth_shader_control.stencil_op_val_export_enable |
                                       regs.depth_shader_control.stencil_test_val_export_enable;
        info.fs_info.mrtz_mask = regs.depth_shader_control.z_export_enable |
                                 (stencil_ref_export_enable << 1) |
                                 (regs.depth_shader_control.mask_export_enable << 2) |
                                 (regs.depth_shader_control.coverage_to_mask_enable << 3);
        const auto& cb0_blend = regs.blend_control[0];
        if (cb0_blend.enable) {
            info.fs_info.dual_source_blending =
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_dst_factor) ||
                LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.color_src_factor);
            if (cb0_blend.separate_alpha_blend) {
                info.fs_info.dual_source_blending |=
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_dst_factor) ||
                    LiverpoolToVK::IsDualSourceBlendFactor(cb0_blend.alpha_src_factor);
            }
        } else {
            info.fs_info.dual_source_blending = false;
        }
        const auto& ps_inputs = regs.ps_inputs;
        for (u32 i = 0; i < regs.num_interp; i++) {
            info.fs_info.inputs[i] = {
                .param_index = u8(ps_inputs[i].input_offset),
                .is_default = bool(ps_inputs[i].use_default),
                .is_flat = bool(ps_inputs[i].flat_shade),
                .default_value = u8(ps_inputs[i].default_value),
            };
        }
        for (u32 i = 0; i < Shader::MaxColorBuffers; i++) {
            info.fs_info.color_buffers[i] = selection.graphics_key.color_buffers[i];
        }
        // Lowered user clip planes ride the same emulation path as guest-exported distances, so
        // the fragment side arms whenever the hardware vertex stage lowers them, keeping its input
        // locations in sync with the shifted vertex outputs.
        const bool lowers_user_clip_planes =
            regs.clipper_control.user_clip_plane_enable &&
            !regs.stage_enable.IsStageEnabled(static_cast<u32>(Stage::Geometry));
        info.fs_info.clip_distance_emulation =
            ((regs.vs_output_control.clip_distance_enable &&
              !regs.stage_enable.IsStageEnabled(static_cast<u32>(Stage::Local))) ||
             lowers_user_clip_planes) &&
            profile.needs_clip_distance_emulation;
        // 0 except while GetVrCutPipeline builds a permutation; VrPushData: only its flag (the
        // draw pushes the radii).
        info.fs_info.vr_cut = vr_push && selection.vr_fs_cut != 0 ? 1u : selection.vr_fs_cut;
        break;
    }
    case Stage::Compute: {
        const auto& cs_pgm = *selection.compute;
        info.num_user_data = cs_pgm.settings.num_user_regs;
        info.num_allocated_vgprs = cs_pgm.settings.num_vgprs * 4;
        info.cs_info.workgroup_size = {cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
                                       cs_pgm.num_thread_z.full};
        info.cs_info.tgid_enable = {cs_pgm.IsTgidEnabled(0), cs_pgm.IsTgidEnabled(1),
                                    cs_pgm.IsTgidEnabled(2)};
        info.cs_info.shared_memory_size = cs_pgm.SharedMemSize();
        break;
    }
    default:
        break;
    }
    return info;
}

PipelineCache::PipelineCache(const Instance& instance_, Scheduler& scheduler_,
                             AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, liverpool{liverpool_},
      desc_heap{instance, scheduler.GetMasterSemaphore(), DescriptorHeapSizes} {
    const auto& vk12_props = instance.GetVk12Properties();
    profile = Shader::Profile{
        .max_viewport_width = instance.GetMaxViewportWidth(),
        .max_viewport_height = instance.GetMaxViewportHeight(),
        .max_shared_memory_size = instance.MaxComputeSharedMemorySize(),
        .max_push_constant_size = instance.GetPhysicalDevice().getProperties().limits.maxPushConstantsSize,
        .supported_spirv = SpirvVersion1_6,
        .subgroup_size = instance.SubgroupSize(),
        .support_int8 = instance.IsShaderInt8Supported(),
        .support_int16 = instance.IsShaderInt16Supported(),
        .support_int64 = instance.IsShaderInt64Supported(),
        .support_float16 = instance.IsShaderFloat16Supported(),
        .support_float64 = instance.IsShaderFloat64Supported(),
        .supports_denorm_behavior_independence =
            vk12_props.denormBehaviorIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .supports_rounding_mode_independence =
            vk12_props.roundingModeIndependence != vk::ShaderFloatControlsIndependence::eNone,
        .support_fp16_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat16),
        .support_fp16_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat16),
        .support_fp16_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat16),
        .support_fp32_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat32),
        .support_fp32_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat32),
        .support_fp32_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat32),
        .support_fp64_denorm_preserve = bool(vk12_props.shaderDenormPreserveFloat64),
        .support_fp64_denorm_flush = bool(vk12_props.shaderDenormFlushToZeroFloat64),
        .support_fp64_round_to_zero = bool(vk12_props.shaderRoundingModeRTZFloat64),
        .support_fp16_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat16),
        .support_fp32_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat32),
        .support_fp64_signed_zero_inf_nan_preserve =
            bool(vk12_props.shaderSignedZeroInfNanPreserveFloat64),
        .supports_image_load_store_lod = instance_.IsImageLoadStoreLodSupported(),
        .supports_native_cube_calc = instance_.IsAmdGcnShaderSupported(),
        .supports_trinary_minmax = instance_.IsAmdShaderTrinaryMinMaxSupported(),
        .supports_buffer_fp32_atomic_min_max =
            instance_.IsShaderAtomicFloatBuffer32MinMaxSupported(),
        .supports_image_fp32_atomic_min_max = instance_.IsShaderAtomicFloatImage32MinMaxSupported(),
        .supports_buffer_int64_atomics = instance_.IsBufferInt64AtomicsSupported(),
        .supports_shared_int64_atomics = instance_.IsSharedInt64AtomicsSupported(),
        .supports_workgroup_explicit_memory_layout =
            instance_.IsWorkgroupMemoryExplicitLayoutSupported(),
        .supports_amd_shader_explicit_vertex_parameter =
            instance_.IsAmdShaderExplicitVertexParameterSupported(),
        .supports_fragment_shader_barycentric = instance_.IsFragmentShaderBarycentricSupported(),
        .needs_manual_interpolation = instance.IsFragmentShaderBarycentricSupported() &&
                                      instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .needs_lds_barriers = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary ||
                              instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_buffer_offsets = instance.StorageMinAlignment() > 4,
        .needs_unorm_fixup = instance.GetDriverID() == vk::DriverId::eMesaKosmickrisp,
        .needs_clip_distance_emulation = instance.GetDriverID() == vk::DriverId::eNvidiaProprietary,
        .supports_shader_stencil_export = instance_.IsShaderStencilExportSupported(),
    };
    if (NativeStereo::Enabled()) {
        if (UsesVrPushData()) {
            LOG_INFO(Render_Vulkan,
                     "VR push data: on ({} of {} push-constant bytes): each draw pushes its "
                     "projection, light tiles, body cutaway and object-motion buffers; the shaders "
                     "bake none of them (motion variants are cached on disk)",
                     Shader::VrPushData::End, profile.max_push_constant_size);
        } else {
            LOG_INFO(Render_Vulkan,
                     "VR push data: off ({} push-constant bytes, {} needed): the projection, light "
                     "tiles, body cutaway and object-motion buffers are baked into the shader "
                     "permutations (motion variants are never cached on disk)",
                     profile.max_push_constant_size, Shader::VrPushData::End);
        }
    }
    WarmUp();

    auto [cache_result, cache] = instance.GetDevice().createPipelineCacheUnique({});
    ASSERT_MSG(cache_result == vk::Result::eSuccess, "Failed to create pipeline cache: {}",
               vk::to_string(cache_result));
    pipeline_cache = std::move(cache);
    InitAsyncPipelines(); // bit 40: state only; the compile threads start on first use
    // The warm-up's pipelines: a later compile names what differs from them. And every module it
    // loaded, also one whose pipelines it did not load (a stale stage beside it, the other
    // mode's, a motion variant without an upscaler): a pipeline using it translated nothing, so
    // it is not a new permutation (logs only).
    if constexpr (Common::Log::Enabled) {
        for (const auto& [key, pipeline] : graphics_pipelines) {
            if (pipeline) {
                compile_reasons.Add(key, std::hash<GraphicsPipelineKey>{}(key));
            }
        }
        for (const auto& [pgm_hash, program] : program_cache) {
            for (const auto& permutation : program->modules) {
                if (permutation.module && permutation.spec.Valid()) {
                    compile_reasons.AddModule(permutation.id);
                }
            }
        }
    }
}

// The preparation implementation stops its workers before destroying program metadata.

const GraphicsPipeline* PipelineCache::SelectGraphicsPipeline(u32 view_mask, AsyncWait wait,
                                                              AsyncInFlight* in_flight) {
    const auto selection_lock = LockSelection();
    selection.regs = &ScratchDraw::Registers(liverpool);
    ScratchProfile::Cpu scratch_cpu(20,"PipelineCache::GetGraphicsPipeline");
    if (!RefreshGraphicsKey(selection, view_mask)) {
        return nullptr;
    }
    static const u64 verify_every = [] {
        const char* value = std::getenv("FAST_SELECTION_VERIFY");
        return value ? std::strtoull(value, nullptr, 0) : 0ULL;
    }();
    if (verify_every && ++selection_sequence % verify_every == 0) {
        // Exercise an independent register source. Shader inputs stay pinned to this
        // draw: the check must not refresh shared Info pointers into its temporary regs.
        std::array<ScratchDraw::Inputs, MaxShaderStages> snapshots{};
        size_t count = 0;
        for (const auto* info : selection.infos) {
            if (!info) continue;
            if (const auto* current = ScratchDraw::Find(info)) snapshots[count] = *current;
            else snapshots[count] = {info, info->UserData(), info->FlatUserData(), info->ProgramBase()};
            ++count;
        }
        ScratchDraw::Scope pinned{{snapshots.data(), count}};
        const auto original = selection.graphics_key;
        auto regs = std::make_unique<AmdGpu::Regs>(*selection.regs);
        regs->polygon_control.provoking_vtx_last = static_cast<AmdGpu::ProvokingVtxLast>(u32(regs->polygon_control.provoking_vtx_last) ^ 1u);
        PipelineSelection independent{};
        independent.regs = regs.get();
        independent.vr_probe_mode = selection.vr_probe_mode;
        independent.vr_motion = selection.vr_motion;
        independent.vr_fs_cut = selection.vr_fs_cut;
        auto expected = original;
        expected.provoking_vtx_last = static_cast<AmdGpu::ProvokingVtxLast>(u32(expected.provoking_vtx_last) ^ 1u);
        ASSERT_MSG(RefreshGraphicsKey(independent, view_mask) &&
                       independent.graphics_key == expected && selection.graphics_key == original,
                   "Independent pipeline selection changed shared state or ignored its registers");
        ++selection_verified;
        if ((selection_verified & 127) == 0)
            LOG_INFO(Render_Vulkan, "FAST selection state: verified={} mismatches=0", selection_verified);
    }
    // Bit 40 (vk_pipeline_async.cpp): a pipeline compiled, or compiling, on a compile thread is
    // installed (waited for unless this draw may be skipped or deferred) instead of compiling it
    // here.
    if (FastRender::On(AsyncPipelinesBit)) {
        if (const auto found = graphics_pipelines.find(selection.graphics_key);
            found != graphics_pipelines.end() && found->second) {
            return found->second.get();
        }
        const GraphicsPipeline* async_result = nullptr;
        if (AsyncGraphicsPipeline(selection, wait, async_result, in_flight)) {
            return async_result;
        }
    }
    const auto [it, is_new] = graphics_pipelines.try_emplace(selection.graphics_key);
    if (is_new) {
        const u64 async_start = FastRender::On(AsyncPipelinesBit) ? AsyncNowNs() : 0;
        const auto pipeline_hash = std::hash<GraphicsPipelineKey>{}(selection.graphics_key);
        CompileTrace::Scope compile_trace("graphics", pipeline_hash);
        CompileReason::Tags reason{}; // for the stall time
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x} why: {}", pipeline_hash,
                 compile_reasons.Pipeline(selection.graphics_key, pipeline_hash,
                                          CompileReason::VariantOf(selection), true, reason));

        GraphicsPipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, selection.graphics_key, *pipeline_cache, selection.infos,
            selection.runtime_infos, selection.fetch_shader, selection.modules, sdata, false,
            selection.vertex_output_locations);

        // Not the probe's, nor a motion variant with its buffers' addresses baked in (without
        // VrPushData: this process's allocations).
        if (selection.vr_probe_mode == 0 && !selection.vr_motion.Baked()) RegisterPipelineData(selection.graphics_key, pipeline_hash, sdata);
        ++num_new_pipelines;
        if (view_predicting) // FAST_CPU_VIEW_PREDICT: its multiview twin is not built
            CountViewPredict(ViewBuilt);
        NoteSyncGraphicsPipeline(selection.graphics_key, pipeline_hash, async_start, reason);

        if (EmulatorSettings.IsShaderCollect()) {
            for (auto stage = 0; stage < MaxShaderStages; ++stage) {
                if (selection.infos[stage]) {
                    auto& m = selection.modules[stage];
                    module_related_pipelines[m].emplace_back(selection.graphics_key);
                }
            }
        }
        selection.fetch_shader.reset();
    }
    return it->second.get();
}

bool PipelineCache::UsesVrPushData() const {
    return NativeStereo::Enabled() && profile.SupportsVrPushData();
}

const GraphicsPipeline* PipelineCache::GetVrCutPipeline(const GraphicsPipeline* base, u32 cut,
                                                        bool wait) {
    const auto selection_lock = LockSelection();
    if (base == nullptr || cut == 0) {
        return base;
    }
    cut = VrCutVariant(cut); // VrPushData: one variant per base, the draw pushes the radii
    const VrCutKey key{base, cut};
    if (const auto it = vr_cut_pipelines.find(key); it != vr_cut_pipelines.end()) {
        return it->second;
    }
    // Bit 40: still compiling for an earlier draw that did not wait (its job's state alone).
    if (const auto it = vr_cut_pending.find(key); it != vr_cut_pending.end()) {
        if (!wait && AsyncPending(it->second)) {
            NoteVrVariantDeferred(false);
            return nullptr;
        }
        vr_cut_pending.erase(it); // ready (installed below), or waited for now
    }
    const Shader::Info* fs = nullptr;
    for (const auto* stage : base->GetStages()) {
        if (stage != nullptr && stage->l_stage == Shader::LogicalStage::Fragment) {
            fs = stage;
        }
    }
    const GraphicsPipeline* variant = nullptr;
    if (fs != nullptr && !fs->stores.Get(Shader::IR::Attribute::SampleMask)) {
        // The same registers as `base` (this draw's), with the cutaway in the fragment program's
        // runtime info: a permutation of that program, so a pipeline of its own.
        selection.vr_fs_cut = cut;
        AsyncInFlight in_flight;
        variant = SelectGraphicsPipeline(base->GetGraphicsKey().probe_view_mask,
                                         wait ? AsyncWait::Wait : AsyncWait::Never, &in_flight);
        selection.vr_fs_cut = 0;
        if (in_flight) {
            // Still compiling: not remembered; later draws ask its job, then install it.
            vr_cut_pending.insert_or_assign(key, std::move(in_flight));
            return nullptr;
        }
        if (variant == base) {
            variant = nullptr;
        }
    }
    const char* const outcome = variant != nullptr
                                    ? "built"
                                    : "not possible (no fragment shader or it writes the sample "
                                      "mask)";
    if (UsesVrPushData()) {
        LOG_INFO(Render_Vulkan,
                 "VR cut: pipeline {:#x} with the body cutaway (every cut, its radii pushed per "
                 "draw): {}",
                 reinterpret_cast<uintptr_t>(base), outcome);
    } else {
        LOG_INFO(Render_Vulkan, "VR cut: pipeline {:#x} with the body cutaway ({} .. {} mm): {}",
                 reinterpret_cast<uintptr_t>(base), cut & 0xFFFFu, (cut >> 16) & 0x7FFFu, outcome);
    }
    if (variant) vr_cut_values.insert_or_assign(variant, cut);
    vr_cut_pipelines.emplace(key, variant);
    return variant;
}

const GraphicsPipeline* PipelineCache::GetVrMotionPipeline(const GraphicsPipeline* base,
                                                          const Shader::VrMotion& motion,
                                                          bool wait, bool* deferred) {
    const auto selection_lock = LockSelection();
    if (!base || !motion.enabled || !motion.params_address || !motion.positions_address ||
        base->GetGraphicsKey().probe_view_mask != 3 || !NativeStereo::Enabled())
        return nullptr;
    const auto& key = base->GetGraphicsKey();
    const auto& regs = ScratchDraw::Registers(liverpool);
    // The spare varyings and MRT are available only on the ordinary VS/PS G-buffer path.
    if (key.probe_view_mask != 3 || key.num_samples != 1 || key.num_color_attachments > 7 ||
        std::popcount(key.mrt_mask) < 5 || (key.mrt_mask & (1u << 7)) ||
        regs.clipper_control.clip_disable ||
        key.prim_type == AmdGpu::PrimitiveType::RectList ||
        key.prim_type == AmdGpu::PrimitiveType::QuadList) return nullptr;
    const Shader::Info* vs{};
    const Shader::Info* fs{};
    for (const auto* stage : base->GetStages()) {
        if (!stage) continue;
        if (stage->stage == Stage::Vertex && stage->l_stage == LogicalStage::Vertex) vs = stage;
        else if (stage->stage == Stage::Fragment && stage->l_stage == LogicalStage::Fragment) fs = stage;
        else return nullptr;
    }
    if (!vs || !fs || vs->uses_reserved_motion_varyings || (fs->mrt_mask & (1u << 7)))
        return nullptr;
    // VrPushData: only the flag (the draw pushes the addresses), so new buffers find it too.
    const Shader::VrMotion variant_motion = VrMotionVariant(motion);
    if (const auto it = vr_motion_pipelines.find(base);
        it != vr_motion_pipelines.end() && it->second.motion == variant_motion) return it->second.variant;
    // Bit 40: still compiling for an earlier draw that did not wait (its job's state alone).
    if (const auto it = vr_motion_pending.find(base); it != vr_motion_pending.end()) {
        if (!wait && it->second.motion == variant_motion && AsyncPending(it->second.job)) {
            NoteVrVariantDeferred(true);
            if (deferred != nullptr) {
                *deferred = true;
            }
            return nullptr;
        }
        vr_motion_pending.erase(it); // ready (installed below), another variant, or waited for
    }
    selection.regs = &regs;
    // Check this shader's actual clip emulation below. The driver capability alone
    // is true for every NVIDIA pipeline and must not reject ordinary Gbuffer draws.
    // Validate fragment interpolation/blend requirements before entering the shader emitter.
    const auto& fragment =
        BuildRuntimeInfo(selection, Stage::Fragment, LogicalStage::Fragment, key.probe_view_mask).fs_info;
    if (fragment.dual_source_blending) return nullptr;
    const u32 clip_shift = fragment.clip_distance_emulation ? 1 : 0;
    for (u32 i = 0; i < fragment.num_inputs - clip_shift; ++i)
        if (!fragment.inputs[i].IsDefault() &&
            fragment.inputs[i].param_index + clip_shift >= Shader::VrMotion::CurrentLocation)
            return nullptr;
    const auto saved_cut = selection.vr_fs_cut;
    const auto cut = vr_cut_values.find(base);
    selection.vr_fs_cut = cut == vr_cut_values.end() ? 0 : cut->second;
    // Its permutations and key as any pipeline's (the motion slot added): with VrPushData the
    // warm-up preloaded them, and GetGraphicsPipeline finds that pipeline without a compile.
    selection.vr_motion = variant_motion;
    AsyncInFlight in_flight;
    const auto* variant = SelectGraphicsPipeline(
        key.probe_view_mask, wait ? AsyncWait::Wait : AsyncWait::Never, &in_flight);
    selection.vr_motion = {};
    selection.vr_fs_cut = saved_cut;
    if (in_flight) {
        // Still compiling: not remembered; later draws ask its job, then install it.
        vr_motion_pending.insert_or_assign(base,
                                           VrMotionPending{variant_motion, std::move(in_flight)});
        if (deferred != nullptr) {
            *deferred = true;
        }
        return nullptr;
    }
    vr_motion_pipelines.insert_or_assign(base, VrMotionEntry{variant_motion, variant});
    return variant;
}

const ComputePipeline* PipelineCache::GetComputePipeline() {
    const auto selection_lock = LockSelection();
    selection.regs = &ScratchDraw::Registers(liverpool);
    selection.compute = &ScratchDraw::ComputeRegisters(liverpool);
    if (!RefreshComputeKey(selection)) {
        return nullptr;
    }
    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);
    if (is_new) {
        const auto pipeline_hash = std::hash<ComputePipelineKey>{}(compute_key);
        CompileTrace::Scope compile_trace("compute", pipeline_hash);
        LOG_INFO(Render_Vulkan, "Compiling compute pipeline {:#x}", pipeline_hash);

        ComputePipeline::SerializationSupport sdata{};
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key, *selection.infos[0],
                                                       selection.modules[0], sdata, false);
        RegisterPipelineData(compute_key, sdata);
        ++num_new_pipelines;

        if (EmulatorSettings.IsShaderCollect()) {
            auto& m = selection.modules[0];
            module_related_pipelines[m].emplace_back(compute_key);
        }
    }
    return it->second.get();
}

bool PipelineCache::RefreshGraphicsKey(PipelineSelection& selection, u32 view_mask, PrepGraphicsContext* prep) {
    std::memset(&selection.graphics_key, 0, sizeof(GraphicsPipelineKey));
    const auto& regs = *selection.regs;
    auto& key = selection.graphics_key;
    key.probe_view_mask = view_mask;

    const bool db_enabled = regs.depth_buffer.DepthValid() || regs.depth_buffer.StencilValid();

    key.z_format = regs.depth_buffer.DepthValid() ? regs.depth_buffer.z_info.format
                                                  : AmdGpu::DepthBuffer::ZFormat::Invalid;
    key.stencil_format = regs.depth_buffer.StencilValid()
                             ? regs.depth_buffer.stencil_info.format
                             : AmdGpu::DepthBuffer::StencilFormat::Invalid;
    key.depth_clamp_enable = !regs.depth_render_override.disable_viewport_clamp;
    key.depth_clip_enable = regs.clipper_control.ZclipEnable();
    key.clip_space = regs.clipper_control.clip_space;
    key.provoking_vtx_last = regs.polygon_control.provoking_vtx_last;
    key.prim_type = regs.primitive_type;
    key.polygon_mode = regs.polygon_control.PolyMode();
    key.patch_control_points =
        regs.stage_enable.hs_en ? regs.ls_hs_config.hs_input_control_points : 0;
    key.logic_op = regs.color_control.rop3;
    key.depth_samples = db_enabled ? regs.depth_buffer.NumSamples() : 1;
    key.num_samples = key.depth_samples;
    key.cb_shader_mask = regs.color_shader_mask;

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;

    // First pass to fill render target information needed by shader recompiler
    for (s32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        if (!col_buf || !regs.color_target_mask.GetMask(cb)) {
            // No attachment bound or writing to it is disabled.
            continue;
        }

        // Fill color target information
        auto& color_buffer = key.color_buffers[cb];
        color_buffer.data_format = col_buf.GetDataFmt();
        color_buffer.num_format = col_buf.GetNumberFmt();
        color_buffer.num_conversion = col_buf.GetNumberConversion();
        color_buffer.export_format = regs.color_export_format.GetFormat(cb);
        color_buffer.swizzle = col_buf.Swizzle();
    }

    // Compile and bind shader stages
    if (!RefreshGraphicsStages(selection, view_mask, prep)) {
        return false;
    }

    // Second pass to mask out render targets not written by shader and fill remaining info
    u8 color_samples = 0;
    bool all_color_samples_same = true;
    for (s32 cb = 0; cb < key.num_color_attachments && !skip_cb_binding; ++cb) {
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (!col_buf || !target_mask) {
            continue;
        }
        if ((key.mrt_mask & (1u << cb)) == 0) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
            continue;
        }

        // Fill color blending information
        if (regs.blend_control[cb].enable && !col_buf.info.blend_bypass) {
            key.blend_controls[cb] = regs.blend_control[cb];
        }

        // Apply swizzle to target mask
        key.write_masks[cb] =
            vk::ColorComponentFlags{key.color_buffers[cb].swizzle.ApplyMask(target_mask)};

        // Fill color samples
        const u8 prev_color_samples = std::exchange(color_samples, col_buf.NumSamples());
        all_color_samples_same &= color_samples == prev_color_samples || prev_color_samples == 0;
        key.color_samples[cb] = color_samples;
        key.num_samples = std::max(key.num_samples, color_samples);
    }
    // The bound targets past the last one the fragment program writes are no attachments either
    // (GraphicsPipeline reads color_buffers below num_color_attachments only): cleared too, so
    // draws that differ only there share one pipeline. Only with a fragment program: every cached
    // key with one is stale with its fragment metas (VrFragmentRevision), the others stay valid.
    if (selection.infos[u32(LogicalStage::Fragment)]) {
        for (u32 cb = key.num_color_attachments; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
            std::memset(&key.color_buffers[cb], 0, sizeof(Shader::PsColorBuffer));
        }
    }

    // Force all color samples to match depth samples to avoid unsupported MSAA configuration
    if (color_samples != 0) {
        const bool depth_mismatch = db_enabled && color_samples != key.depth_samples;
        if (!all_color_samples_same && !instance.IsMixedAnySamplesSupported() ||
            all_color_samples_same && depth_mismatch && !instance.IsMixedDepthSamplesSupported()) {
            key.color_samples.fill(key.depth_samples);
            key.num_samples = key.depth_samples;
        }
    }

    if (selection.vr_motion.enabled) {
        // The guest MRT mask stays unchanged: target 7 is host-owned, not a guest image.
        constexpr auto slot = Shader::VrMotion::Output;
        key.num_color_attachments = slot + 1;
        key.color_buffers[slot] = {};
        key.color_buffers[slot].data_format = AmdGpu::DataFormat::Format32_32_32_32;
        key.color_buffers[slot].num_format = AmdGpu::NumberFormat::Float;
        key.blend_controls[slot] = {};
        key.write_masks[slot] = vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                                vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA;
        key.color_samples[slot] = 1;
    }
    if (selection.vr_probe_mode) {
        key.probe_view_mask = selection.vr_probe_mode == 2 ? 3 : 0;
        key.num_color_attachments = 0;
        key.z_format = AmdGpu::DepthBuffer::ZFormat::Z32Float;
        key.stencil_format = AmdGpu::DepthBuffer::StencilFormat::Invalid;
        key.num_samples = key.depth_samples = 1;
    }
    return true;
}

const GraphicsPipeline* PipelineCache::GetForegroundProbePipeline(const GraphicsPipeline* base, u32 mode) {
    const auto selection_lock = LockSelection();
    const VrCutKey key{base, mode};
    if (auto it = probe_pipelines.find(key); it != probe_pipelines.end()) return it->second;
    selection.vr_probe_mode = mode;
    const auto* result = GetGraphicsPipeline(base->GetGraphicsKey().probe_view_mask);
    selection.vr_probe_mode = 0;
    probe_pipelines.emplace(key, result);
    return result;
}

bool PipelineCache::RefreshGraphicsStages(PipelineSelection& selection, u32 view_mask, PrepGraphicsContext* prep) {
    const auto& regs = *selection.regs;
    auto& key = selection.graphics_key;
    static const bool reuse_fetch = [] {
        const char* setting = std::getenv("VR_REUSE_FETCH_STORAGE");
        return !setting || setting[0] != '0';
    }();
    std::optional<Shader::Gcn::FetchShaderData> spare_fetch;
    if (reuse_fetch) spare_fetch = std::move(selection.fetch_shader);
    selection.fetch_shader = std::nullopt;

    Shader::Backend::Bindings binding{};
    const auto bind_stage = [&](Shader::Stage stage_in, Shader::LogicalStage stage_out) -> bool {
        const auto stage_in_idx = static_cast<u32>(stage_in);
        const auto stage_out_idx = static_cast<u32>(stage_out);
        if (!regs.stage_enable.IsStageEnabled(stage_in_idx)) {
            key.stage_hashes[stage_out_idx] = 0;
            selection.infos[stage_out_idx] = nullptr;
            return false;
        }

        const auto* pgm = regs.ProgramForStage(stage_in_idx);
        if (!pgm || !pgm->Address<u32*>()) {
            key.stage_hashes[stage_out_idx] = 0;
            selection.infos[stage_out_idx] = nullptr;
            return false;
        }

        if (prep) {
            if (!PrepareGraphicsStage(selection, stage_in, stage_out, binding, view_mask, *prep)) {
                prep->failed = true;
                return false;
            }
            return true;
        }
        const auto params = AmdGpu::GetParams(*pgm);
        std::optional<Shader::Gcn::FetchShaderData> fetch_shader_;
        std::tie(selection.infos[stage_out_idx], selection.modules[stage_out_idx], fetch_shader_,
                 key.stage_hashes[stage_out_idx]) =
            GetProgram(selection, stage_in, stage_out, params, binding, reuse_fetch ? &spare_fetch : nullptr,
                       view_mask);
        if (fetch_shader_) {
            // This local is consumed immediately and never read again. Preserve
            // the owned metadata while avoiding a second attribute-vector allocation.
            static const bool move_fetch = [] {
                const char* setting = std::getenv("VR_MOVE_FETCH_METADATA");
                return !setting || setting[0] != '0';
            }();
            if (move_fetch) selection.fetch_shader = std::move(fetch_shader_);
            else selection.fetch_shader = fetch_shader_;
        }
        return true;
    };

    selection.infos.fill(nullptr);
    selection.modules.fill(nullptr);

    bind_stage(Stage::Fragment, LogicalStage::Fragment);

    const auto* fs_info = selection.infos[static_cast<u32>(LogicalStage::Fragment)];
    key.mrt_mask = fs_info ? fs_info->mrt_mask : 0u;
    key.num_color_attachments = std::bit_width(key.mrt_mask);

    switch (regs.stage_enable.raw) {
    case AmdGpu::ShaderStageEnable::VgtStages::EsGs:
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(Stage::Export, LogicalStage::Vertex)) {
            return false;
        }
        if (!bind_stage(Stage::Geometry, LogicalStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!bind_stage(Stage::Hull, LogicalStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(Stage::Vertex, LogicalStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(Stage::Local, LogicalStage::Vertex)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::LsHsEsGs:
        if (!instance.IsTessellationSupported()) {
            return false;
        }
        if (!instance.IsGeometryStageSupported()) {
            LOG_WARNING(Render_Vulkan, "Geometry shader stage unsupported, skipping");
            return false;
        }
        if (regs.vgt_gs_mode.onchip || regs.vgt_strmout_config.raw) {
            LOG_WARNING(Render_Vulkan, "Geometry shader features unsupported, skipping");
            return false;
        }
        if (!bind_stage(Stage::Hull, LogicalStage::TessellationControl)) {
            return false;
        }
        if (!bind_stage(Stage::Export, LogicalStage::TessellationEval)) {
            return false;
        }
        if (!bind_stage(Stage::Local, LogicalStage::Vertex)) {
            return false;
        }
        if (!bind_stage(Stage::Geometry, LogicalStage::Geometry)) {
            return false;
        }
        break;
    case AmdGpu::ShaderStageEnable::VgtStages::Vs:
        bind_stage(Stage::Vertex, LogicalStage::Vertex);
        break;
    default:
        UNREACHABLE_MSG("unhandled stage_en: {}", (u32)regs.stage_enable.raw);
    }

    if (prep && prep->failed) return false;
    const auto* vs_info = selection.infos[static_cast<u32>(Shader::LogicalStage::Vertex)];
    std::optional<ScratchDraw::Inputs> vertex_input;
    std::optional<ScratchDraw::Scope> vertex_scope;
    if (prep && vs_info) {
        // The worker's private copy: user/flat words and V#s it read under its fault guard.
        const auto* owned = prep->inputs[u32(LogicalStage::Vertex)];
        if (!owned) return false;
        vertex_input = ScratchDraw::Inputs{vs_info, owned->user, owned->info.flattened_ud_buf,
                                           owned->info.pgm_base, owned->register_reads};
        vertex_scope.emplace(std::span<const ScratchDraw::Inputs>{&*vertex_input, 1});
    }
    if (vs_info && selection.fetch_shader && !instance.IsVertexInputDynamicState()) {
        // Without vertex input dynamic state, the pipeline needs to specialize on format.
        // Stride will still be handled outside the pipeline using dynamic state.
        u32 vertex_binding = 0;
        for (const auto& attrib : selection.fetch_shader->attributes) {
            const auto& buffer = attrib.GetSharp(*vs_info);
            ASSERT_MSG(vertex_binding < MaxVertexBufferCount,
                       "Vertex attribute binding count exceeded limit: {} >= {}", vertex_binding,
                       MaxVertexBufferCount);
            if (prep) {
                const auto format = LiverpoolToVK::TrySurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
                if (!format) return false;
                key.vertex_buffer_formats[vertex_binding++] = *format;
            } else {
                key.vertex_buffer_formats[vertex_binding++] =
                    LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt());
            }
        }
    }

    return true;
}

bool PipelineCache::RefreshComputeKey(PipelineSelection& selection) {
    Shader::Backend::Bindings binding{};
    const auto& cs_pgm = *selection.compute;
    const auto cs_params = AmdGpu::GetParams(cs_pgm);
    std::tie(selection.infos[0], selection.modules[0], selection.fetch_shader, compute_key.value) =
        GetProgram(selection, Shader::Stage::Compute, LogicalStage::Compute, cs_params, binding);
    return true;
}

// `runtime_info` is the translation's own copy: TranslateProgram may change it
// (InjectClipDistanceAttributes counts the emulated clip distances as one more input), while the
// caller's permutation keeps the selection's values, as every later selection builds them.
vk::ShaderModule PipelineCache::CompileModule(Shader::Info& info, Shader::RuntimeInfo runtime_info,
                                              const std::span<const u32>& code, size_t perm_idx,
                                              u64 reason_hash, Shader::Backend::Bindings& binding,
                                              std::vector<u32>& spv_out,
                                              const Program::ModuleList* existing,
                                              const Shader::StageSpecialization* wanted) {
    CompileTrace::Scope compile_trace("module", info.pgm_hash);
    if (VR::GameplaySeconds() >= 20.0)
        FastRender::g_counters.gameplay_compiles.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO(Render_Vulkan, "Compiling {} shader {:#x} {}why: {}", info.stage, info.pgm_hash,
             perm_idx != 0 ? "(permutation) " : "",
             compile_reasons.Module(existing, wanted, reason_hash));
    DumpShader(code, info.pgm_hash, info.stage, perm_idx, "bin");

    const auto ir_program = Shader::TranslateProgram(code, pools, info, runtime_info, profile);
    info.uses_reserved_motion_varyings = info.l_stage == LogicalStage::Vertex &&
        (info.stores.GetAny(Shader::IR::Attribute::Param0 + (Shader::VrMotion::CurrentLocation - 1)) ||
         info.stores.GetAny(Shader::IR::Attribute::Param0 + Shader::VrMotion::CurrentLocation) ||
         info.stores.GetAny(Shader::IR::Attribute::Param0 + Shader::VrMotion::PreviousLocation));
    auto spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, ir_program, binding);
    DumpShader(spv, info.pgm_hash, info.stage, perm_idx, "spv");

    vk::ShaderModule module;

    auto patch = GetShaderPatch(info.pgm_hash, info.stage, perm_idx, "spv");
    const bool is_patched = patch && EmulatorSettings.IsPatchShaders();
    if (is_patched) {
        LOG_INFO(Loader, "Loaded patch for {} shader {:#x}", info.stage, info.pgm_hash);
        module = CompileSPV(*patch, instance.GetDevice());
    } else {
        module = CompileSPV(spv, instance.GetDevice());
    }

    const auto name = GetShaderName(info.stage, info.pgm_hash, perm_idx);
    Vulkan::SetObjectName(instance.GetDevice(), module, name);
    if (EmulatorSettings.IsShaderCollect()) {
        DebugState.CollectShader(name, info.l_stage, module, spv, code,
                                 patch ? *patch : std::span<const u32>{}, is_patched);
    }
    // The caller writes it (a new program's id needs the specialization, built from the
    // translation's Info).
    spv_out = std::move(spv);
    return module;
}

// FAST_CPU_PERM_IDS: a permutation's id that no other permutation of `program` has (a 64-bit
// collision takes the next candidate: deterministic, but not its content's, so `cacheable` is
// false and nothing of it is written to disk).
static u64 UniquePermutationId(const Program& program, u64 id, bool& cacheable) {
    cacheable = true;
    while (program.HasId(id)) {
        LOG_WARNING(Render_Vulkan, "Permutation id {:#018x} of program {:#x} is another's: this "
                                   "permutation is not cached on disk",
                    id, program.info.pgm_hash);
        id = PipelineCachePolicy::NextId(id);
        cacheable = false;
    }
    return id;
}

PipelineCache::Result PipelineCache::GetProgram(PipelineSelection& selection,
    Stage stage, LogicalStage l_stage, const Shader::ShaderParams& params,
    Shader::Backend::Bindings& binding, std::optional<Shader::Gcn::FetchShaderData>* spare_fetch,
    u32 view_mask) {
    ScratchProfile::Cpu scratch_cpu(21,"PipelineCache::GetProgram");
    auto runtime_info = BuildRuntimeInfo(selection, stage, l_stage, view_mask);
    const auto selection_start = binding;
    const auto finish = [&](Program& program, size_t index) -> Result {
        if (PrepVerifyInterval() || FastRender::On(GraphicsPrepBit))
            OnProgramSelected(program, index, runtime_info, selection_start);
        const auto& module = program.modules[index];
        if (l_stage == LogicalStage::Vertex)
            selection.vertex_output_locations = module.vertex_output_locations;
        // The stage hash: the permutation's name (its id with FAST_CPU_PERM_IDS, else
        // HashCombine(program, index); pipeline_cache_policy.h), the disk cache's name.
        return {&program.info, module.module,
                ScratchOwned::CopyForReturn(module.spec.fetch_shader_data, spare_fetch), module.id};
    };
    auto it_pgm = program_cache.find(params.hash);
    if (it_pgm == program_cache.end()) {
        auto program = std::make_unique<Program>(stage, l_stage, params);
        std::vector<u32> spv;
        const auto module = CompileModule(program->info, runtime_info, params.code, 0,
                                          HashCombine(params.hash, u64{0}), binding, spv);
        // The translation found the colour targets the program writes: its first permutation
        // keeps only those, as every later selection of it does (the SPIR-V never read the rest).
        if (stage == Stage::Fragment) {
            const bool fixup = profile.needs_unorm_fixup;
            runtime_info.fs_info.KeepWrittenColorBuffers(program->info.mrt_mask, fixup);
            selection.runtime_infos[u32(l_stage)].fs_info.KeepWrittenColorBuffers(
                program->info.mrt_mask, fixup);
        }
        auto spec = Shader::StageSpecialization(program->info, runtime_info, profile, selection_start);
        // The program's first permutation: no other to collide with.
        const u64 perm_id = PermutationIds() ? PermutationId(params.hash, spec)
                                             : HashCombine(params.hash, u64{0});
        // Physical motion-buffer addresses are process/device allocations, never disk data: a
        // motion permutation is written only when the draw pushes them (VrPushData).
        if (!runtime_info.vr_motion.Baked()) {
            RegisterShaderBinary(std::move(spv), perm_id, params.hash, 0);
            RegisterShaderMeta(program->info, spec.fetch_shader_data, spec, perm_id, 0,
                               program->info.vertex_output_locations);
        }
        program->AddPermut(module, std::move(spec), perm_id, program->info.vertex_output_locations);
        auto* published = program.get();
        {
            std::unique_lock lock(programs_mutex);
            program_cache.emplace(params.hash, std::move(program));
        }
        return finish(*published, 0);
    }

    auto& program = it_pgm.value();
    auto& info = program->info;
    // Only the colour targets this program writes are part of its permutations (a draw that
    // binds other targets too, or other formats for them, selects the same one): the spec memo
    // and the comparison below see the masked bytes, a new permutation compiles from them.
    if (stage == Stage::Fragment) {
        const bool fixup = profile.needs_unorm_fixup;
        runtime_info.fs_info.KeepWrittenColorBuffers(info.mrt_mask, fixup);
        selection.runtime_infos[u32(l_stage)].fs_info.KeepWrittenColorBuffers(info.mrt_mask, fixup);
    }
    // A recording packet already owns this program's user data and flattened tables.
    // Rare view-mask/cutaway re-selection must not walk its guest pointers again.
    if (!ScratchDraw::Find(&info)) {
        info.pgm_base = params.Base(); // Inline cbuffer address fixup on the producer.
        info.user_data = params.user_data;
        ScratchProfile::Cpu scratch_flat(22, "Shader::RefreshFlatBuf");
        info.RefreshFlatBuf();
    }

    // perf_spec_memo (vk_perf_memo.h): the same inputs as a recent use of this program build an
    // equal specialization, so the permutation that use resolved to is this one too. Not for the
    // tessellation stages (their specialization also reads the tessellation constants in memory)
    // nor the geometry stage.
    const bool memo = PerfMemo::g_switches.Get().spec_memo &&
                      l_stage != LogicalStage::TessellationControl &&
                      l_stage != LogicalStage::TessellationEval && l_stage != LogicalStage::Geometry;
    PerfMemo::Projection proj;
    bool proj_valid = false;
    int memo_idx = -1;
    const Shader::Backend::Bindings start = binding;
    if (memo) {
        memo_idx = FindSpecMemo(selection, *program, stage, l_stage, start, proj, proj_valid);
        if (memo_idx >= 0) {
            ++spec_counters.hits;
            if (!PerfMemo::CheckMode()) {
                info.AddBindings(binding);
                return finish(*program, memo_idx);
            }
        } else {
            ++spec_counters.misses;
        }
    }
    auto spec = [&] {
        ScratchProfile::Cpu scratch_spec(35,"PipelineCache::BuildSpecialization");
        return Shader::StageSpecialization(info, runtime_info, profile, binding);
    }();

    size_t perm_idx = program->modules.size();

    vk::ShaderModule module{};

    const auto it = [&] {
        ScratchProfile::Cpu scratch_search(36,"PipelineCache::FindPermutation");
        return std::ranges::find(program->modules, spec, &Program::Module::spec);
    }();
    if (memo_idx >= 0) {
        // VR_PERF_CHECK=1: the memo's answer against the full comparison (which is used).
        ++spec_counters.checked;
        const s64 orig =
            it == program->modules.end() ? -1 : std::distance(program->modules.begin(), it);
        if (orig != memo_idx && spec_counters.NoteMismatch()) {
            LOG_WARNING(Render_Vulkan,
                        "VR perf check: spec memo mismatch #{}: program {:#x} stage {} (logical "
                        "{}) binding start {}/{}/{}: memo permutation {}, the full comparison {} "
                        "of {}",
                        spec_counters.total_mismatches, params.hash, stage, u32(l_stage), start.unified,
                        start.buffer, start.user_data, memo_idx, orig, program->modules.size());
        }
    }
    std::optional<Shader::Gcn::FetchShaderData> memo_fs;
    if (memo) {
        memo_fs = spec.fetch_shader_data;
    }
    if (it == program->modules.end()) {
        bool cacheable = true;
        const u64 perm_id =
            PermutationIds()
                ? UniquePermutationId(*program, PermutationId(params.hash, spec), cacheable)
                : HashCombine(params.hash, u64(perm_idx));
        auto new_info = Shader::Info(stage, l_stage, params);
        std::vector<u32> spv;
        module = CompileModule(new_info, runtime_info, params.code, perm_idx, perm_id, binding,
                               spv, &program->modules, &spec);

        if (cacheable && !runtime_info.vr_motion.Baked()) {
            RegisterShaderBinary(std::move(spv), perm_id, params.hash, perm_idx);
            RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_id, perm_idx,
                               new_info.vertex_output_locations);
        }
        std::unique_lock lock(programs_mutex);
        program->AddPermut(module, std::move(spec), perm_id, new_info.vertex_output_locations);
    } else if (!it->module) {
        // Bit 40 warm-up (vk_pipeline_serialization.cpp): the driver rejected a cached pipeline
        // built from this permutation, so its cached module was dropped. Compile it again at its
        // own index and under its own id, keeping the cache's names and the other pipelines' keys
        // valid.
        perm_idx = std::distance(program->modules.begin(), it);
        const u64 perm_id = it->id;
        auto new_info = Shader::Info(stage, l_stage, params);
        std::vector<u32> spv;
        module = CompileModule(new_info, runtime_info, params.code, perm_idx, perm_id, binding,
                               spv, &program->modules, &spec);
        if (!runtime_info.vr_motion.Baked()) {
            RegisterShaderBinary(std::move(spv), perm_id, params.hash, perm_idx);
            RegisterShaderMeta(info, spec.fetch_shader_data, spec, perm_id, perm_idx,
                               new_info.vertex_output_locations);
        }
        std::unique_lock lock(programs_mutex);
        ++programs_generation;
        program->InsertPermut(module, std::move(spec), perm_idx, perm_id,
                              new_info.vertex_output_locations);
    } else {
        info.AddBindings(binding);
        module = it->module;
        perm_idx = std::distance(program->modules.begin(), it);
    }
    if (memo) {
        StoreSpecMemo(selection, *program, stage, l_stage, start, memo_fs, proj, proj_valid, u32(perm_idx));
    }
    return finish(*program, perm_idx);
}

int PipelineCache::FindSpecMemo(PipelineSelection& selection, Program& program, Stage stage, LogicalStage l_stage,
                                const Shader::Backend::Bindings& start,
                                PerfMemo::Projection& proj, bool& proj_valid) {
    ScratchProfile::Cpu scratch_memo(33,"PipelineCache::FindSpecMemo");
    proj_valid = false;
    const u32 now = PerfMemo::Epoch();
    const auto& info = program.info;
    const Shader::RuntimeInfo& rt = selection.runtime_infos[u32(l_stage)];
    const u32* fs_code = info.has_fetch_shader
                             ? Shader::Gcn::GetFetchShaderCode(info, info.fetch_shader_sgpr_base)
                             : nullptr;
    const bool recent_first = Program::SpecMru();
    const auto count = program.spec_memo.size(); // Always two or eight (power of two).
    for (size_t probe = 0; probe < count; ++probe) {
        const size_t index = recent_first ? ((program.spec_memo_recent + probe) & (count - 1)) : probe;
        const auto& m = program.spec_memo[index];
        if (m.epoch != now || m.stage != stage || m.l_stage != l_stage || m.start != start ||
            m.perm_idx >= program.modules.size() || m.fs_code != fs_code) {
            continue;
        }
        // BuildRuntimeInfo starts from a memset, so equal bytes are equal register values.
        if (std::memcmp(m.runtime_info.data(), &rt, sizeof(rt)) != 0) {
            continue;
        }
        if (fs_code != nullptr &&
            (m.fs_words.empty() ||
             std::memcmp(fs_code, m.fs_words.data(), m.fs_words.size() * sizeof(u32)) != 0)) {
            continue;
        }
        if (!proj_valid) {
            // The fetch shader's words are the remembered ones, so its attributes are too.
        { ScratchProfile::Cpu scratch_project(34,"PipelineCache::ProjectStage");
            ProjectSpecializationInputs(info, m.fs_data ? &m.fs_data->attributes : nullptr, proj);
        }
            proj_valid = true;
        }
        if (proj.Equals(m.proj)) {
            if (recent_first) program.spec_memo_recent = static_cast<u32>(index);
            return static_cast<int>(m.perm_idx);
        }
    }
    return -1;
}

void PipelineCache::StoreSpecMemo(PipelineSelection& selection, Program& program, Stage stage, LogicalStage l_stage,
                                  const Shader::Backend::Bindings& start,
                                  const std::optional<Shader::Gcn::FetchShaderData>& fs_data,
                                  PerfMemo::Projection& proj, bool proj_valid, u32 perm_idx) {
    ScratchProfile::Cpu scratch_store(37,"PipelineCache::StoreSpecMemo");
    const auto& info = program.info;
    if (info.has_fetch_shader && (!fs_data || fs_data->size == 0)) {
        return;
    }
    if (!proj_valid) {
        { ScratchProfile::Cpu scratch_project(34,"PipelineCache::ProjectStage");
        ProjectSpecializationInputs(info, fs_data ? &fs_data->attributes : nullptr, proj);
        }
    }
    if (!proj.Usable()) {
        return;
    }
    const Shader::RuntimeInfo& rt = selection.runtime_infos[u32(l_stage)];
    const u32* fs_code = info.has_fetch_shader
                             ? Shader::Gcn::GetFetchShaderCode(info, info.fetch_shader_sgpr_base)
                             : nullptr;
    // The entry of the same stage, binding start, runtime info and fetch shader is refreshed;
    // otherwise the older entry is replaced.
    Program::SpecMemo* slot = nullptr;
    for (auto& m : program.spec_memo) {
        if (m.epoch != 0 && m.stage == stage && m.l_stage == l_stage && m.start == start &&
            m.fs_code == fs_code && std::memcmp(m.runtime_info.data(), &rt, sizeof(rt)) == 0 &&
            (!Program::SpecVariantHistory() ||
             (proj.Equals(m.proj) &&
              (fs_code == nullptr ||
               (m.fs_words.size() == (fs_data->size + 3) / 4 &&
                std::memcmp(fs_code, m.fs_words.data(), m.fs_words.size() * sizeof(u32)) == 0))))) {
            slot = &m;
            break;
        }
    }
    if (slot == nullptr) {
        slot = &program.spec_memo[program.spec_memo_next];
        program.spec_memo_next = (program.spec_memo_next + 1) % program.spec_memo.size();
    }
    if (Program::SpecMru())
        program.spec_memo_recent = static_cast<u32>(slot - program.spec_memo.data());
    auto& m = *slot;
    m.epoch = PerfMemo::Epoch();
    m.perm_idx = perm_idx;
    m.stage = stage;
    m.l_stage = l_stage;
    m.start = start;
    std::memcpy(m.runtime_info.data(), &rt, sizeof(rt));
    m.fs_code = fs_code;
    if (fs_code != nullptr) {
        m.fs_words.assign(fs_code, fs_code + (fs_data->size + 3) / 4);
        m.fs_data = fs_data;
    } else {
        m.fs_words.clear();
        m.fs_data.reset();
    }
    proj.StoreTo(m.proj);
}

std::optional<vk::ShaderModule> PipelineCache::ReplaceShader(vk::ShaderModule module,
                                                             std::span<const u32> spv_code) {
    scheduler.Finish(); // retire commands referencing the pipelines replaced below
    const auto selection_lock = LockSelection();
    std::unique_lock program_lock(programs_mutex);
    ++programs_generation;
    // Bit 40: no compile thread may still use a module destroyed below (the generation bump
    // above makes every job that has not started yet stale).
    DrainAsyncPipelines();
    vr_cut_pending.clear(); // their jobs are stale now, and their bases may go below
    vr_motion_pending.clear();
    std::optional<vk::ShaderModule> new_module{};
    PerfMemo::BumpEpoch(); // perf_spec_memo: the selection.modules change
    for (const auto& [_, program] : program_cache) {
        for (auto& m : program->modules) {
            if (m.module == module) {
                const auto& d = instance.GetDevice();
                d.destroyShaderModule(m.module);
                m.module = CompileSPV(spv_code, d);
                new_module = m.module;
            }
        }
    }
    if (module_related_pipelines.contains(module)) {
        auto& pipeline_keys = module_related_pipelines[module];
        for (auto& key : pipeline_keys) {
            if (std::holds_alternative<GraphicsPipelineKey>(key)) {
                auto& changed_key = std::get<GraphicsPipelineKey>(key);
                graphics_pipelines.erase(changed_key);
                vr_cut_pipelines.clear(); // may point at the erased pipeline
                vr_cut_prebuilt.clear();
            } else if (std::holds_alternative<ComputePipelineKey>(key)) {
                auto& compute_key = std::get<ComputePipelineKey>(key);
                compute_pipelines.erase(compute_key);
            }
        }
    }
    return new_module;
}

std::string PipelineCache::GetShaderName(Shader::Stage stage, u64 hash,
                                         std::optional<size_t> perm) {
    if (perm) {
        return fmt::format("{}_{:#018x}_{}", stage, hash, *perm);
    }
    return fmt::format("{}_{:#018x}", stage, hash);
}

void PipelineCache::DumpShader(std::span<const u32> code, u64 hash, Shader::Stage stage,
                               size_t perm_idx, std::string_view ext) {
    if (!EmulatorSettings.IsDumpShaders()) {
        return;
    }

    using namespace Common::FS;
    const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
    if (!std::filesystem::exists(dump_dir)) {
        std::filesystem::create_directories(dump_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
    file.WriteSpan(code);
}

std::optional<std::vector<u32>> PipelineCache::GetShaderPatch(u64 hash, Shader::Stage stage,
                                                              size_t perm_idx,
                                                              std::string_view ext) {

    using namespace Common::FS;
    const auto patch_dir = GetUserPath(PathType::ShaderDir) / "patch";
    if (!std::filesystem::exists(patch_dir)) {
        std::filesystem::create_directories(patch_dir);
    }
    const auto filename = fmt::format("{}.{}", GetShaderName(stage, hash, perm_idx), ext);
    const auto filepath = patch_dir / filename;
    if (!std::filesystem::exists(filepath)) {
        return {};
    }
    const auto file = IOFile{patch_dir / filename, FileAccessMode::Read};
    std::vector<u32> code(file.GetSize() / sizeof(u32));
    file.Read(code);
    return code;
}
} // namespace Vulkan
