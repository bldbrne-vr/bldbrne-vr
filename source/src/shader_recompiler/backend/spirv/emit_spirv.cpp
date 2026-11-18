// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdlib>
#include <optional>
#include <span>
#include <tuple>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>
#include <magic_enum/magic_enum.hpp>

#include "common/assert.h"
#include "common/func_traits.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/eye_spec.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Backend::SPIRV {
namespace {

static constexpr spv::ExecutionMode GetInputPrimitiveType(AmdGpu::PrimitiveType type) {
    switch (type) {
    case AmdGpu::PrimitiveType::PointList:
        return spv::ExecutionMode::InputPoints;
    case AmdGpu::PrimitiveType::LineList:
    case AmdGpu::PrimitiveType::LineStrip:
        return spv::ExecutionMode::InputLines;
    case AmdGpu::PrimitiveType::TriangleList:
    case AmdGpu::PrimitiveType::TriangleStrip:
    case AmdGpu::PrimitiveType::RectList:
        return spv::ExecutionMode::Triangles;
    case AmdGpu::PrimitiveType::AdjTriangleList:
        return spv::ExecutionMode::InputTrianglesAdjacency;
    case AmdGpu::PrimitiveType::AdjLineList:
        return spv::ExecutionMode::InputLinesAdjacency;
    default:
        UNREACHABLE_MSG("Unknown input primitive type {}", u32(type));
    }
}

static constexpr spv::ExecutionMode GetOutputPrimitiveType(AmdGpu::GsOutputPrimitiveType type) {
    switch (type) {
    case AmdGpu::GsOutputPrimitiveType::PointList:
        return spv::ExecutionMode::OutputVertices;
    case AmdGpu::GsOutputPrimitiveType::LineStrip:
        return spv::ExecutionMode::OutputLineStrip;
    case AmdGpu::GsOutputPrimitiveType::TriangleStrip:
        return spv::ExecutionMode::OutputTriangleStrip;
    default:
        UNREACHABLE_MSG("Unknown output primitive type {}", u32(type));
    }
}

template <auto func, typename... Args>
void SetDefinition(EmitContext& ctx, IR::Inst* inst, Args... args) {
    inst->SetDefinition<Id>(func(ctx, std::forward<Args>(args)...));
}

template <typename ArgType>
ArgType Arg(EmitContext& ctx, const IR::Value& arg) {
    if constexpr (std::is_same_v<ArgType, Id>) {
        return ctx.Def(arg);
    } else if constexpr (std::is_same_v<ArgType, const IR::Value&>) {
        return arg;
    } else if constexpr (std::is_same_v<ArgType, u32>) {
        return arg.U32();
    } else if constexpr (std::is_same_v<ArgType, u64>) {
        return arg.U64();
    } else if constexpr (std::is_same_v<ArgType, bool>) {
        return arg.U1();
    } else if constexpr (std::is_same_v<ArgType, IR::Attribute>) {
        return arg.Attribute();
    } else if constexpr (std::is_same_v<ArgType, IR::ScalarReg>) {
        return arg.ScalarReg();
    } else if constexpr (std::is_same_v<ArgType, IR::VectorReg>) {
        return arg.VectorReg();
    } else if constexpr (std::is_same_v<ArgType, const char*>) {
        return arg.StringLiteral();
    } else if constexpr (std::is_same_v<ArgType, IR::Patch>) {
        return arg.Patch();
    }
    UNREACHABLE();
}

template <auto func, bool is_first_arg_inst, size_t... I>
void Invoke(EmitContext& ctx, IR::Inst* inst, std::index_sequence<I...>) {
    using Traits = Common::FuncTraits<decltype(func)>;
    if constexpr (std::is_same_v<typename Traits::ReturnType, Id>) {
        if constexpr (is_first_arg_inst) {
            SetDefinition<func>(
                ctx, inst, inst,
                Arg<typename Traits::template ArgType<I + 2>>(ctx, inst->Arg(I))...);
        } else {
            SetDefinition<func>(
                ctx, inst, Arg<typename Traits::template ArgType<I + 1>>(ctx, inst->Arg(I))...);
        }
    } else {
        if constexpr (is_first_arg_inst) {
            func(ctx, inst, Arg<typename Traits::template ArgType<I + 2>>(ctx, inst->Arg(I))...);
        } else {
            func(ctx, Arg<typename Traits::template ArgType<I + 1>>(ctx, inst->Arg(I))...);
        }
    }
}

template <auto func>
void Invoke(EmitContext& ctx, IR::Inst* inst) {
    using Traits = Common::FuncTraits<decltype(func)>;
    static_assert(Traits::NUM_ARGS >= 1, "Insufficient arguments");
    if constexpr (Traits::NUM_ARGS == 1) {
        Invoke<func, false>(ctx, inst, std::make_index_sequence<0>{});
    } else {
        using FirstArgType = typename Traits::template ArgType<1>;
        static constexpr bool is_first_arg_inst = std::is_same_v<FirstArgType, IR::Inst*>;
        using Indices = std::make_index_sequence<Traits::NUM_ARGS - (is_first_arg_inst ? 2 : 1)>;
        Invoke<func, is_first_arg_inst>(ctx, inst, Indices{});
    }
}

void EmitInst(EmitContext& ctx, IR::Inst* inst) {
    switch (inst->GetOpcode()) {
#define OPCODE(name, result_type, ...)                                                             \
    case IR::Opcode::name:                                                                         \
        return Invoke<&Emit##name>(ctx, inst);
#include "shader_recompiler/ir/opcodes.inc"
#undef OPCODE
    }
    UNREACHABLE_MSG("Invalid opcode {}", inst->GetOpcode());
}

Id TypeId(const EmitContext& ctx, IR::Type type) {
    switch (type) {
    case IR::Type::U1:
        return ctx.U1[1];
    case IR::Type::U32:
        return ctx.U32[1];
    default:
        UNREACHABLE_MSG("Phi node type {}", type);
    }
}

void Traverse(EmitContext& ctx, const IR::Program& program) {
    IR::Block* current_block{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case IR::AbstractSyntaxNode::Type::Block: {
            const Id label{node.data.block->Definition<Id>()};
            if (current_block) {
                ctx.OpBranch(label);
            }
            current_block = node.data.block;
            ctx.AddLabel(label);
            for (IR::Inst& inst : node.data.block->Instructions()) {
                EmitInst(ctx, &inst);
            }
            ctx.first_to_last_label_map[label.value] = ctx.last_label;
            break;
        }
        case IR::AbstractSyntaxNode::Type::If: {
            const Id if_label{node.data.if_node.body->Definition<Id>()};
            const Id endif_label{node.data.if_node.merge->Definition<Id>()};
            ctx.OpSelectionMerge(endif_label, spv::SelectionControlMask::MaskNone);
            ctx.OpBranchConditional(ctx.Def(node.data.if_node.cond), if_label, endif_label);
            break;
        }
        case IR::AbstractSyntaxNode::Type::Loop: {
            const Id body_label{node.data.loop.body->Definition<Id>()};
            const Id continue_label{node.data.loop.continue_block->Definition<Id>()};
            const Id endloop_label{node.data.loop.merge->Definition<Id>()};

            ctx.OpLoopMerge(endloop_label, continue_label, spv::LoopControlMask::MaskNone);
            ctx.OpBranch(body_label);
            break;
        }
        case IR::AbstractSyntaxNode::Type::Break: {
            const Id break_label{node.data.break_node.merge->Definition<Id>()};
            const Id skip_label{node.data.break_node.skip->Definition<Id>()};
            ctx.OpBranchConditional(ctx.Def(node.data.break_node.cond), break_label, skip_label);
            break;
        }
        case IR::AbstractSyntaxNode::Type::EndIf:
            if (current_block) {
                ctx.OpBranch(node.data.end_if.merge->Definition<Id>());
            }
            break;
        case IR::AbstractSyntaxNode::Type::Repeat: {
            Id cond{ctx.Def(node.data.repeat.cond)};
            const Id loop_header_label{node.data.repeat.loop_header->Definition<Id>()};
            const Id merge_label{node.data.repeat.merge->Definition<Id>()};
            ctx.OpBranchConditional(cond, loop_header_label, merge_label);
            break;
        }
        case IR::AbstractSyntaxNode::Type::Return:
            ctx.OpReturn();
            break;
        case IR::AbstractSyntaxNode::Type::Unreachable:
            ctx.OpUnreachable();
            break;
        }
        if (node.type != IR::AbstractSyntaxNode::Type::Block) {
            current_block = nullptr;
        }
    }
}

Id DefineMain(EmitContext& ctx, const IR::Program& program) {
    const Id void_function{ctx.TypeFunction(ctx.void_id)};
    const Id main{ctx.OpFunction(ctx.void_id, spv::FunctionControlMask::MaskNone, void_function)};
    for (IR::Block* const block : program.blocks) {
        block->SetDefinition(ctx.OpLabel());
    }
    Traverse(ctx, program);
    ctx.OpFunctionEnd();
    return main;
}

spv::ExecutionMode ExecutionMode(AmdGpu::TessellationType primitive) {
    switch (primitive) {
    case AmdGpu::TessellationType::Isoline:
        return spv::ExecutionMode::Isolines;
    case AmdGpu::TessellationType::Triangle:
        return spv::ExecutionMode::Triangles;
    case AmdGpu::TessellationType::Quad:
        return spv::ExecutionMode::Quads;
    }
    UNREACHABLE_MSG("Tessellation primitive {}", magic_enum::enum_name(primitive));
}

spv::ExecutionMode ExecutionMode(AmdGpu::TessellationPartitioning spacing) {
    switch (spacing) {
    case AmdGpu::TessellationPartitioning::Integer:
        return spv::ExecutionMode::SpacingEqual;
    case AmdGpu::TessellationPartitioning::FracOdd:
        return spv::ExecutionMode::SpacingFractionalOdd;
    case AmdGpu::TessellationPartitioning::FracEven:
        return spv::ExecutionMode::SpacingFractionalEven;
    case AmdGpu::TessellationPartitioning::Pow2:
        // Pow2 rounds tessellation factors to the nearest power of 2, which has no
        // direct Vulkan equivalent. SpacingEqual (integer) is the closest match.
        LOG_WARNING(Render_Vulkan, "Tessellation partitioning Pow2 has no Vulkan equivalent, "
                                   "falling back to SpacingEqual");
        return spv::ExecutionMode::SpacingEqual;
    default:
        break;
    }
    UNREACHABLE_MSG("Tessellation spacing {}", magic_enum::enum_name(spacing));
}

void SetupCapabilities(const Info& info, const Profile& profile, const RuntimeInfo& runtime_info,
                       EmitContext& ctx) {
    ctx.AddCapability(spv::Capability::Image1D);
    ctx.AddCapability(spv::Capability::Sampled1D);
    ctx.AddCapability(spv::Capability::ImageQuery);
    ctx.AddCapability(spv::Capability::Int8);
    ctx.AddCapability(spv::Capability::Int16);
    ctx.AddCapability(spv::Capability::Int64);
    ctx.AddCapability(spv::Capability::StorageBuffer8BitAccess);
    ctx.AddCapability(spv::Capability::StorageBuffer16BitAccess);
    if (info.uses_fp16) {
        ctx.AddCapability(spv::Capability::Float16);
    }
    if (info.uses_fp64) {
        ctx.AddCapability(spv::Capability::Float64);
    }
    if (info.has_storage_images) {
        ctx.AddCapability(spv::Capability::StorageImageExtendedFormats);
        ctx.AddCapability(spv::Capability::StorageImageReadWithoutFormat);
        ctx.AddCapability(spv::Capability::StorageImageWriteWithoutFormat);
        if (profile.supports_image_load_store_lod) {
            ctx.AddExtension("SPV_AMD_shader_image_load_store_lod");
            ctx.AddCapability(spv::Capability::ImageReadWriteLodAMD);
        }
    }
    if (info.has_image_gather) {
        ctx.AddCapability(spv::Capability::ImageGatherExtended);
    }
    if (info.has_image_query) {
        ctx.AddCapability(spv::Capability::ImageQuery);
    }
    if ((info.uses_image_atomic_float_min_max && profile.supports_image_fp32_atomic_min_max) ||
        (info.uses_buffer_atomic_float_min_max && profile.supports_buffer_fp32_atomic_min_max)) {
        ctx.AddExtension("SPV_EXT_shader_atomic_float_min_max");
        ctx.AddCapability(spv::Capability::AtomicFloat32MinMaxEXT);
    }
    if (info.uses_lane_id) {
        ctx.AddCapability(spv::Capability::GroupNonUniform);
    }
    if (info.uses_group_quad) {
        ctx.AddCapability(spv::Capability::GroupNonUniformQuad);
    }
    if (info.uses_group_ballot) {
        ctx.AddCapability(spv::Capability::GroupNonUniformBallot);
    }
    const auto stage = info.l_stage;
    if (stage == LogicalStage::Vertex) {
        ctx.AddExtension("SPV_KHR_shader_draw_parameters");
        ctx.AddCapability(spv::Capability::DrawParameters);
    }
    if (stage == LogicalStage::Geometry) {
        ctx.AddCapability(spv::Capability::Geometry);
    }
    if (info.stage == Stage::Fragment) {
        if (profile.supports_amd_shader_explicit_vertex_parameter) {
            ctx.AddExtension("SPV_AMD_shader_explicit_vertex_parameter");
        } else if (profile.supports_fragment_shader_barycentric) {
            ctx.AddExtension("SPV_KHR_fragment_shader_barycentric");
            ctx.AddCapability(spv::Capability::FragmentBarycentricKHR);
            ctx.AddCapability(spv::Capability::InterpolationFunction);
        }
        if (info.loads.Get(IR::Attribute::SampleIndex) ||
            runtime_info.fs_info.addr_flags.linear_sample_ena ||
            runtime_info.fs_info.addr_flags.persp_sample_ena ||
            (!profile.supports_amd_shader_explicit_vertex_parameter &&
             profile.supports_fragment_shader_barycentric &&
             info.loads.Get(IR::Attribute::BaryCoordSmoothSample))) {
            ctx.AddCapability(spv::Capability::SampleRateShading);
        }
        if (info.loads.GetAny(IR::Attribute::RenderTargetIndex)) {
            ctx.AddCapability(spv::Capability::Geometry);
        }
        if (info.stores.Get(IR::Attribute::StencilRef) && profile.supports_shader_stencil_export) {
            ctx.AddExtension("SPV_EXT_shader_stencil_export");
            ctx.AddCapability(spv::Capability::StencilExportEXT);
        }
    }
    if (stage == LogicalStage::TessellationControl || stage == LogicalStage::TessellationEval) {
        ctx.AddCapability(spv::Capability::Tessellation);
    }
    if (stage == LogicalStage::Vertex || stage == LogicalStage::TessellationControl ||
        stage == LogicalStage::TessellationEval) {
        if (info.stores.GetAny(IR::Attribute::RenderTargetIndex) || ctx.NativeLayerOutput()) {
            ctx.AddCapability(spv::Capability::ShaderLayer);
        }
        if (info.stores.GetAny(IR::Attribute::ViewportIndex)) {
            ctx.AddCapability(spv::Capability::ShaderViewportIndex);
        }
    } else if (stage == LogicalStage::Geometry &&
               info.stores.GetAny(IR::Attribute::ViewportIndex)) {
        ctx.AddCapability(spv::Capability::MultiViewport);
    }
    if (info.uses_dma || ctx.VertexMotion()) {
        ctx.AddCapability(spv::Capability::PhysicalStorageBufferAddresses);
        ctx.AddExtension("SPV_KHR_physical_storage_buffer");
    }
    const auto shared_type_count = std::popcount(static_cast<u32>(info.shared_types));
    if (shared_type_count > 1 && profile.supports_workgroup_explicit_memory_layout) {
        ctx.AddExtension("SPV_KHR_workgroup_memory_explicit_layout");
        ctx.AddCapability(spv::Capability::WorkgroupMemoryExplicitLayoutKHR);
        ctx.AddCapability(spv::Capability::WorkgroupMemoryExplicitLayout16BitAccessKHR);
    }
    if (info.uses_buffer_int64_atomics || info.uses_shared_int64_atomics) {
        if (info.uses_buffer_int64_atomics) {
            ASSERT_MSG(ctx.profile.supports_buffer_int64_atomics,
                       "Shader requires support for atomic Int64 buffer operations that your "
                       "Vulkan instance does not advertise");
        }
        if (info.uses_shared_int64_atomics) {
            ASSERT_MSG(ctx.profile.supports_shared_int64_atomics,
                       "Shader requires support for atomic Int64 shared memory operations that "
                       "your Vulkan instance does not advertise");
        }
        ctx.AddCapability(spv::Capability::Int64Atomics);
    }
}

void DefineEntryPoint(const Info& info, EmitContext& ctx, Id main) {
    const std::span interfaces(ctx.interfaces.data(), ctx.interfaces.size());
    spv::ExecutionModel execution_model{};
    switch (info.l_stage) {
    case LogicalStage::Compute: {
        const std::array<u32, 3> workgroup_size{ctx.runtime_info.cs_info.workgroup_size};
        execution_model = spv::ExecutionModel::GLCompute;
        ctx.AddExecutionMode(main, spv::ExecutionMode::LocalSize, workgroup_size[0],
                             workgroup_size[1], workgroup_size[2]);
        break;
    }
    case LogicalStage::Vertex:
        execution_model = spv::ExecutionModel::Vertex;
        break;
    case LogicalStage::TessellationControl:
        execution_model = spv::ExecutionModel::TessellationControl;
        ctx.AddCapability(spv::Capability::Tessellation);
        ctx.AddExecutionMode(main, spv::ExecutionMode::OutputVertices,
                             ctx.runtime_info.hs_info.NumOutputControlPoints());
        break;
    case LogicalStage::TessellationEval: {
        execution_model = spv::ExecutionModel::TessellationEvaluation;
        ctx.AddExecutionMode(main, ExecutionMode(ctx.runtime_info.es_vs_info.tess_type));
        ctx.AddExecutionMode(main, ExecutionMode(ctx.runtime_info.es_vs_info.tess_partitioning));
        ctx.AddExecutionMode(main, ctx.runtime_info.es_vs_info.tess_topology ==
                                           AmdGpu::TessellationTopology::TriangleCcw
                                       ? spv::ExecutionMode::VertexOrderCcw
                                       : spv::ExecutionMode::VertexOrderCw);
        break;
    }
    case LogicalStage::Fragment:
        execution_model = spv::ExecutionModel::Fragment;
        if (ctx.profile.lower_left_origin_mode) {
            ctx.AddExecutionMode(main, spv::ExecutionMode::OriginLowerLeft);
        } else {
            ctx.AddExecutionMode(main, spv::ExecutionMode::OriginUpperLeft);
        }
        if (info.has_discard || ctx.runtime_info.fs_info.vr_cut != 0 || ctx.runtime_info.native_light_scissor) {
            ctx.AddCapability(spv::Capability::DemoteToHelperInvocation);
        }
        if (info.stores.GetAny(IR::Attribute::Depth)) {
            ctx.AddExecutionMode(main, spv::ExecutionMode::DepthReplacing);
        }
        break;
    case LogicalStage::Geometry:
        execution_model = spv::ExecutionModel::Geometry;
        ctx.AddExecutionMode(main, GetInputPrimitiveType(ctx.runtime_info.gs_info.in_primitive));
        ctx.AddExecutionMode(main,
                             GetOutputPrimitiveType(ctx.runtime_info.gs_info.out_primitive[0]));
        ctx.AddExecutionMode(main, spv::ExecutionMode::OutputVertices,
                             ctx.runtime_info.gs_info.output_vertices);
        ctx.AddExecutionMode(main, spv::ExecutionMode::Invocations,
                             ctx.runtime_info.gs_info.num_invocations);
        break;
    default:
        UNREACHABLE_MSG("Stage {}", u32(info.stage));
    }
    ctx.AddEntryPoint(execution_model, main, "main", interfaces);
}

void SetupDenormFlushMode(EmitContext& ctx, const Profile& profile, const RuntimeInfo& runtime_info,
                          Id main_func) {
    const auto fp32_denorm_mode = runtime_info.fp_denorm_mode32;
    if (fp32_denorm_mode == AmdGpu::FpDenormMode::InOutFlush) {
        if (profile.support_fp32_denorm_flush) {
            ctx.AddCapability(spv::Capability::DenormFlushToZero);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormFlushToZero, 32U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float32 denorm flushing is not supported by the GPU");
            });
        }
    } else {
        LOG_WARNING(Render_Vulkan, "Unknown FP denorm mode {}", u32(fp32_denorm_mode));
    }

    if (!ctx.info.uses_fp16 && !ctx.info.uses_fp64) {
        return;
    }

    const auto fp16_64_denorm_mode = runtime_info.fp_denorm_mode16_64;
    const auto same_denorm_mode = fp16_64_denorm_mode == fp32_denorm_mode;
    if (!same_denorm_mode && !profile.supports_denorm_behavior_independence) {
        static std::once_flag logged;
        std::call_once(logged, [] {
            LOG_WARNING(Render_Vulkan, "Float32 and Float16/64 denorm modes are different but the "
                                       "GPU does not support that");
        });
        return;
    }

    if (fp16_64_denorm_mode == AmdGpu::FpDenormMode::InOutFlush) {
        if (profile.support_fp16_denorm_flush) {
            ctx.AddCapability(spv::Capability::DenormFlushToZero);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormFlushToZero, 16U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float16 denorm flushing is not supported by the GPU");
            });
        }
        if (profile.support_fp64_denorm_flush) {
            ctx.AddCapability(spv::Capability::DenormFlushToZero);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormFlushToZero, 64U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float64 denorm flushing is not supported by the GPU");
            });
        }
    } else if (fp16_64_denorm_mode == AmdGpu::FpDenormMode::InOutAllow) {
        if (profile.support_fp16_denorm_preserve) {
            ctx.AddCapability(spv::Capability::DenormPreserve);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormPreserve, 16U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float16 denorm preserving is not supported by the GPU");
            });
        }
        if (profile.support_fp64_denorm_preserve) {
            ctx.AddCapability(spv::Capability::DenormPreserve);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::DenormPreserve, 64U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float64 denorm preserving is not supported by the GPU");
            });
        }
    } else {
        LOG_WARNING(Render_Vulkan, "Unknown Float16/64 denorm mode {}", u32(fp16_64_denorm_mode));
    }
}

void SetupRoundingMode(EmitContext& ctx, const Profile& profile, const RuntimeInfo& runtime_info,
                       Id main_func) {
    const auto fp_round_mode = runtime_info.fp_round_mode32;
    if (fp_round_mode == AmdGpu::FpRoundMode::ToZero) {
        if (profile.support_fp32_round_to_zero) {
            ctx.AddCapability(spv::Capability::RoundingModeRTZ);
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::RoundingModeRTZ, 32U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float32 rounding to zero is not supported by the GPU");
            });
        }
    } else if (fp_round_mode != AmdGpu::FpRoundMode::NearestEven) {
        LOG_WARNING(Render_Vulkan, "Unknown FP rounding mode {}", u32(fp_round_mode));
    }

    if (ctx.info.uses_fp16 || ctx.info.uses_fp64) {
        const auto fp16_64_round_mode = runtime_info.fp_round_mode16_64;
        const auto same_round_mode = fp16_64_round_mode == fp_round_mode;
        if (!same_round_mode && !profile.supports_rounding_mode_independence) {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(Render_Vulkan, "Float32 and Float16/64 rounding modes are different "
                                           "but the GPU does not support that");
            });
            return;
        }

        if (fp16_64_round_mode == AmdGpu::FpRoundMode::ToZero) {
            if (ctx.info.uses_fp16) {
                if (profile.support_fp16_round_to_zero) {
                    ctx.AddCapability(spv::Capability::RoundingModeRTZ);
                    ctx.AddExecutionMode(main_func, spv::ExecutionMode::RoundingModeRTZ, 16U);
                } else {
                    static std::once_flag logged;
                    std::call_once(logged, [] {
                        LOG_WARNING(Render_Vulkan,
                                    "Float16 rounding to zero is not supported by the GPU");
                    });
                }
            }
            if (ctx.info.uses_fp64) {
                if (profile.support_fp64_round_to_zero) {
                    ctx.AddCapability(spv::Capability::RoundingModeRTZ);
                    ctx.AddExecutionMode(main_func, spv::ExecutionMode::RoundingModeRTZ, 64U);
                } else {
                    static std::once_flag logged;
                    std::call_once(logged, [] {
                        LOG_WARNING(Render_Vulkan,
                                    "Float64 rounding to zero is not supported by the GPU");
                    });
                }
            }
        } else if (fp16_64_round_mode != AmdGpu::FpRoundMode::NearestEven) {
            LOG_WARNING(Render_Vulkan, "Unknown Float16/64 rounding mode {}",
                        u32(fp16_64_round_mode));
        }
    }
}

void SetupInfNanPreserveMode(EmitContext& ctx, const Profile& profile,
                             const RuntimeInfo& runtime_info, Id main_func) {
    if (profile.support_fp16_signed_zero_inf_nan_preserve ||
        profile.support_fp32_signed_zero_inf_nan_preserve ||
        profile.support_fp64_signed_zero_inf_nan_preserve) {
        ctx.AddCapability(spv::Capability::SignedZeroInfNanPreserve);
    }

    if (profile.support_fp32_signed_zero_inf_nan_preserve) {
        ctx.AddExecutionMode(main_func, spv::ExecutionMode::SignedZeroInfNanPreserve, 32U);
    } else {
        static std::once_flag logged;
        std::call_once(logged, [] {
            LOG_WARNING(Render_Vulkan,
                        "Float32 signed zero/inf/nan preserve mode is not supported by the GPU");
        });
    }
    if (ctx.info.uses_fp16) {
        if (profile.support_fp16_signed_zero_inf_nan_preserve) {
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::SignedZeroInfNanPreserve, 16U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(
                    Render_Vulkan,
                    "Float16 signed zero/inf/nan preserve mode is not supported by the GPU");
            });
        }
    }
    if (ctx.info.uses_fp64) {
        if (profile.support_fp64_signed_zero_inf_nan_preserve) {
            ctx.AddExecutionMode(main_func, spv::ExecutionMode::SignedZeroInfNanPreserve, 64U);
        } else {
            static std::once_flag logged;
            std::call_once(logged, [] {
                LOG_WARNING(
                    Render_Vulkan,
                    "Float64 signed zero/inf/nan preserve mode is not supported by the GPU");
            });
        }
    }
}

void SetupFloatMode(EmitContext& ctx, const Profile& profile, const RuntimeInfo& runtime_info,
                    Id main_func) {
    ctx.AddExtension("SPV_KHR_float_controls");
    SetupDenormFlushMode(ctx, profile, runtime_info, main_func);
    SetupRoundingMode(ctx, profile, runtime_info, main_func);
    SetupInfNanPreserveMode(ctx, profile, runtime_info, main_func);
}

void PatchPhiNodes(const IR::Program& program, EmitContext& ctx) {
    auto inst{program.blocks.front()->begin()};
    size_t block_index{0};
    ctx.PatchDeferredPhi([&](u32 phi_arg, Id first_parent) {
        if (phi_arg == 0) {
            ++inst;
            if (inst == program.blocks[block_index]->end() ||
                inst->GetOpcode() != IR::Opcode::Phi) {
                do {
                    ++block_index;
                    inst = program.blocks[block_index]->begin();
                } while (inst->GetOpcode() != IR::Opcode::Phi);
            }
        }
        const Id arg = ctx.Def(inst->Arg(phi_arg));
        const Id parent = ctx.first_to_last_label_map[first_parent.value];
        return std::make_pair(arg, parent);
    });
}
#include "native_screen_exports.inc"

// VR_EYE_SPEC (shader_recompiler/eye_spec.h): each buffer binding's marker, an OpConstant of
// EyeSpec::Magic + binding read only by its own OpIEqual compares, becomes an OpSpecConstant of
// the same type and default value with SpecId EyeSpec::SpecBase + binding (decorated before the
// first type declaration, after every other annotation). A marker whose id appears anywhere else
// in the module (a guest constant of the same value shares it, or a literal word happens to equal
// the id) stays a plain constant: its default is not 1, so that binding keeps its dynamic eye
// index. Returns how many markers were made specialization constants.
// The words of an instruction (after its first) that may hold an id: every one, except literal
// operands of the instructions the emitter writes most (decorations, names, constants, scalar and
// vector types, composite extracts and inserts, shuffles, extended instructions). A literal that
// happens to equal a marker's id, counted, only keeps that marker plain.
bool MayHoldId(u32 opcode, u32 word) {
    switch (opcode) {
    case 3:  // OpSource: literals and a string
    case 7:  // OpString
    case 10: // OpExtension
    case 14: // OpMemoryModel
    case 15: // OpEntryPoint: a constant is never an interface
    case 17: // OpCapability
        return false;
    case 5:  // OpName: target, string
    case 6:  // OpMemberName: type, member, string
    case 11: // OpExtInstImport: result, string
    case 16: // OpExecutionMode: entry point, literals
    case 21: // OpTypeInt: result, width, signedness
    case 22: // OpTypeFloat: result, width
    case 71: // OpDecorate: target, decoration, literals
    case 72: // OpMemberDecorate: type, member, decoration, literals
        return word == 1;
    case 23: // OpTypeVector: result, component type, count
    case 24: // OpTypeMatrix: result, column type, count
    case 43: // OpConstant: type, result, literal value
    case 50: // OpSpecConstant
        return word <= 2;
    case 81: // OpCompositeExtract: type, result, composite, literal indexes
        return word <= 3;
    case 79: // OpVectorShuffle: type, result, vector 1, vector 2, literal components
    case 82: // OpCompositeInsert: type, result, object, composite, literal indexes
        return word <= 4;
    case 12: // OpExtInst: type, result, set, literal instruction, operands
        return word != 4;
    default:
        return true;
    }
}

u32 PatchEyeSpec(std::vector<u32>& words, std::span<const EmitContext::EyeSpecMarker> markers) {
    if (markers.empty() || words.size() < 5) {
        return 0;
    }
    constexpr u32 OpConstant = 43, OpSpecConstant = 50, OpDecorate = 71, OpTypeVoid = 19,
                  OpTypeForwardPointer = 39, DecorationSpecId = 1;
    // A binding's markers (its flag, and a read-only binding's stride and size): all made
    // specialization constants, or none.
    struct Item {
        u32 marker;   // index in `markers`
        u32 id;       // the constant's id
        u32 value;    // its literal
        u32 uses;     // the references the emitter made
        u32 spec_id;
        size_t definition = 0;
        u32 references = 0;
    };
    std::vector<Item> items;
    for (u32 m = 0; m < markers.size(); ++m) {
        const auto& k = markers[m];
        items.push_back({m, k.marker.value, Shader::EyeSpec::Magic + k.binding, k.uses,
                         Shader::EyeSpec::SpecBase + k.binding});
        if (Sirit::ValidId(k.delta)) {
            items.push_back({m, k.delta.value, Shader::EyeSpec::MagicDelta + k.binding,
                             k.delta_uses, Shader::EyeSpec::SpecBaseDelta + k.binding});
            items.push_back({m, k.size.value, Shader::EyeSpec::MagicSize + k.binding,
                             k.size_uses, Shader::EyeSpec::SpecBaseSize + k.binding});
        }
    }
    size_t first_type = 0;
    for (size_t i = 5; i < words.size();) {
        const u32 count = words[i] >> 16;
        const u32 opcode = words[i] & 0xFFFF;
        if (count == 0 || i + count > words.size()) {
            return 0; // malformed: leave the module as it is
        }
        if (first_type == 0 && opcode >= OpTypeVoid && opcode <= OpTypeForwardPointer) {
            first_type = i;
        }
        for (auto& item : items) {
            if (opcode == OpConstant && count == 4 && words[i + 2] == item.id &&
                words[i + 3] == item.value) {
                item.definition = i;
                continue;
            }
            for (u32 w = 1; w < count; ++w) {
                item.references += words[i + w] == item.id && MayHoldId(opcode, w) ? 1 : 0;
            }
        }
        i += count;
    }
    if (first_type == 0) {
        return 0;
    }
    std::vector<u8> proven(markers.size(), 1);
    for (const auto& item : items) {
        if (item.definition == 0 || item.references != item.uses) {
            proven[item.marker] = 0;
        }
    }
    std::vector<u32> decorations;
    u32 patched = 0;
    for (const auto& item : items) {
        if (!proven[item.marker]) {
            continue;
        }
        words[item.definition] = (4u << 16) | OpSpecConstant;
        decorations.insert(decorations.end(),
                           {(4u << 16) | OpDecorate, item.id, DecorationSpecId, item.spec_id});
    }
    for (const u8 p : proven) {
        patched += p;
    }
    words.insert(words.begin() + static_cast<std::ptrdiff_t>(first_type), decorations.begin(),
                 decorations.end());
    return patched;
}

// VR_EYE_DEP_AUDIT=1 (diagnostic, off by default): which outputs of a native stereo vertex shader
// depend on an eye's own data. In a vertex stage only the game's 864-byte camera buffers are paired
// per eye (Rasterizer::BindBuffers), so an output whose value never reads one (directly or through
// anything it is computed from) is the same for both eyes. A shader whose only eye-dependent output
// is the position could run once for both views (VK_NVX_multiview_per_view_attributes). One line a
// shader, and every 64 shaders the totals.
void AuditEyeDependence(const IR::Program& program) {
    static const bool on = [] {
        const char* v = std::getenv("VR_EYE_DEP_AUDIT");
        return v && v[0] == '1';
    }();
    if (!on || program.info.stage != Stage::Vertex) {
        return;
    }
    const auto camera_handle = [&](const IR::Inst* inst) {
        const auto op = inst->GetOpcode();
        const bool load = op == IR::Opcode::ReadConstBuffer ||
                          (op >= IR::Opcode::LoadBufferU8 && op <= IR::Opcode::LoadBufferFormatF32);
        if (!load || !inst->Arg(0).IsImmediate()) {
            return false;
        }
        const u32 handle = inst->Arg(0).U32();
        return handle < program.info.buffers.size() &&
               !program.info.buffers[handle].IsSpecial() &&
               program.info.buffers[handle].GetSharp(program.info).GetSize() == 864;
    };
    // Memo: an instruction's eye dependence (0 unknown, 1 eye-free, 2 eye-dependent).
    std::unordered_map<const IR::Inst*, u8> memo;
    const auto depends = [&](IR::Value root) {
        if (root.IsEmpty() || root.IsImmediate()) {
            return false;
        }
        std::vector<const IR::Inst*> todo{root.InstRecursive()}, order;
        std::unordered_map<const IR::Inst*, u8> local;
        bool found = false;
        while (!todo.empty() && !found) {
            const IR::Inst* inst = todo.back();
            todo.pop_back();
            if (const auto it = memo.find(inst); it != memo.end()) {
                found = it->second == 2;
                continue;
            }
            if (!local.emplace(inst, 1).second) {
                continue;
            }
            order.push_back(inst);
            if (camera_handle(inst)) {
                found = true;
                break;
            }
            for (size_t i = 0; i < inst->NumArgs(); ++i) {
                const auto arg = inst->Arg(i);
                if (!arg.IsEmpty() && !arg.IsImmediate()) {
                    todo.push_back(arg.InstRecursive());
                }
            }
        }
        if (!found) {
            for (const auto* inst : order) {
                memo[inst] = 1; // its whole slice was walked without a camera read
            }
        }
        return found;
    };
    u32 outputs = 0, eye_outputs = 0;
    bool position_eye = false;
    std::string eye_list;
    for (const auto* block : program.blocks) {
        for (const auto& inst : *block) {
            if (inst.GetOpcode() != IR::Opcode::SetAttribute) {
                continue;
            }
            const auto attr = inst.Arg(0).Attribute();
            ++outputs;
            if (!depends(inst.Arg(1))) {
                continue;
            }
            if (attr == IR::Attribute::Position0) {
                position_eye = true;
                continue;
            }
            ++eye_outputs;
            if (eye_list.size() < 200) {
                eye_list += fmt::format(" {}.{}", IR::NameOf(attr), inst.Arg(2).U32());
            }
        }
    }
    static std::atomic<u32> shaders{}, camera_position{}, position_only{}, other_eye{};
    const u32 n = shaders.fetch_add(1) + 1;
    camera_position += position_eye ? 1 : 0;
    position_only += position_eye && eye_outputs == 0 ? 1 : 0;
    other_eye += eye_outputs != 0 ? 1 : 0;
    LOG_INFO(Render_Recompiler,
             "VR eye-dep audit: vs {:#x}: {} output writes, position {}, {} other writes eye-dependent:{}",
             program.info.pgm_hash, outputs, position_eye ? "eye-dependent" : "eye-free",
             eye_outputs, eye_list);
    if (n % 64 == 0) {
        LOG_INFO(Render_Recompiler,
                 "VR eye-dep audit: {} vertex shaders, {} with an eye-dependent position, {} of them "
                 "with no other eye-dependent output, {} with other eye-dependent outputs",
                 n, camera_position.load(), position_only.load(), other_eye.load());
    }
}

} // Anonymous namespace

std::vector<u32> EmitSPIRV(const Profile& profile, const RuntimeInfo& runtime_info,
                           const IR::Program& program, Bindings& binding, bool merged_eyes) {
    EmitContext ctx{profile, runtime_info, program.info, binding, merged_eyes};
    if (runtime_info.native_stereo) {
        ctx.info.native_camera_buffers = 0;
        ctx.info.native_projection_buffers = 0;
        std::vector<IR::Inst*> todo, seen;
        for (auto* block : program.blocks) for (auto& inst : *block) {
            if (inst.GetOpcode()==IR::Opcode::SetAttribute && inst.Arg(0).Attribute()==IR::Attribute::Position0 && !inst.Arg(1).IsImmediate())
                todo.push_back(inst.Arg(1).InstRecursive());
        }
        while (!todo.empty()) {
            auto* inst=todo.back();todo.pop_back();
            if (!inst || std::ranges::find(seen,inst)!=seen.end()) continue;
            seen.push_back(inst);
            if (inst->GetOpcode()==IR::Opcode::ReadConstBuffer && inst->Arg(0).IsImmediate() && inst->Arg(1).IsImmediate()) {
                const u32 handle=inst->Arg(0).U32(), index=inst->Arg(1).U32();
                if (handle<program.info.buffers.size() && program.info.buffers[handle].GetSharp(program.info).GetSize()==864 &&
                    (index==11 || index==183 || index==187 || index==191)) {
                    ctx.native_camera_position=true; ctx.info.native_camera_buffers |= u64{1} << handle;
                }
                if (handle<program.info.buffers.size() && program.info.buffers[handle].GetSharp(program.info).GetSize()==864 &&
                    ((index>=52 && index<68) || (index>=200 && index<216))) {
                    ctx.native_projection_position=true; ctx.info.native_projection_buffers |= u64{1} << handle;
                }
            }
            for (size_t i=0;i<inst->NumArgs();++i) {
                const auto arg=inst->Arg(i);
                if (!arg.IsEmpty() && !arg.IsImmediate()) todo.push_back(arg.InstRecursive());
            }
        }
        AuditEyeDependence(program);
    }
    FindNativeScreenExports(ctx, program);
    const Id main{DefineMain(ctx, program)};
    DefineEntryPoint(program.info, ctx, main);
    SetupCapabilities(program.info, profile, runtime_info, ctx);
    SetupFloatMode(ctx, profile, runtime_info, main);
    PatchPhiNodes(program, ctx);
    binding.user_data += program.info.ud_mask.NumRegs();
    auto words = ctx.Assemble();
    if (!ctx.eye_spec_markers.empty()) {
        // VR_EYE_SPEC: the markers made specialization constants; every 256 stages the totals.
        const u32 patched = PatchEyeSpec(
            words, {ctx.eye_spec_markers.data(), ctx.eye_spec_markers.size()});
        static std::atomic<u32> stages{}, markers{}, made{};
        const u32 n = stages.fetch_add(1, std::memory_order_relaxed) + 1;
        markers.fetch_add(static_cast<u32>(ctx.eye_spec_markers.size()), std::memory_order_relaxed);
        made.fetch_add(patched, std::memory_order_relaxed);
        if (n % 256 == 0 || patched != ctx.eye_spec_markers.size()) {
            LOG_INFO(Render_Recompiler,
                     "VR eye spec: {} stages, {} buffer markers, {} made specialization constants{}",
                     n, markers.load(std::memory_order_relaxed),
                     made.load(std::memory_order_relaxed),
                     patched != ctx.eye_spec_markers.size()
                         ? fmt::format(" ({:#x}: {} of {} kept plain)", program.info.pgm_hash,
                                       ctx.eye_spec_markers.size() - patched,
                                       ctx.eye_spec_markers.size())
                         : std::string{});
        }
    }
    return words;
}

Id EmitPhi(EmitContext& ctx, IR::Inst* inst) {
    const size_t num_args{inst->NumArgs()};
    boost::container::small_vector<Id, 32> blocks;
    blocks.reserve(num_args);
    for (size_t index = 0; index < num_args; ++index) {
        blocks.push_back(inst->PhiBlock(index)->Definition<Id>());
    }
    // The type of a phi instruction is stored in its flags
    const Id result_type{TypeId(ctx, inst->Flags<IR::Type>())};
    return ctx.DeferredOpPhi(result_type, std::span(blocks.data(), blocks.size()));
}

void EmitVoid(EmitContext&) {}

Id EmitIdentity(EmitContext& ctx, const IR::Value& value) {
    UNREACHABLE_MSG("Forward identity declaration");
}

Id EmitConditionRef(EmitContext& ctx, const IR::Value& value) {
    const Id id{ctx.Def(value)};
    ASSERT_MSG(Sirit::ValidId(id), "Forward identity declaration");
    return id;
}

void EmitReference(EmitContext&) {}

void EmitPhiMove(EmitContext&) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetScc(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetExec(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetVcc(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetSccLo(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetVccLo(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetVccHi(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitGetM0(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetScc(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetExec(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetVcc(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetSccLo(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetVccLo(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetVccHi(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

void EmitSetM0(EmitContext& ctx) {
    UNREACHABLE_MSG("Unreachable instruction");
}

} // namespace Shader::Backend::SPIRV
