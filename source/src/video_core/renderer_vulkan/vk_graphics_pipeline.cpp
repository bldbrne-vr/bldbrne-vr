// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/native_stereo/native_stereo.h"
#include <algorithm>
#include <bit>
#include <utility>
#include <boost/container/small_vector.hpp>

#include "common/assert.h"
#include "video_core/fast_render.h"
#include "shader_recompiler/backend/spirv/emit_spirv_discard_frag.h"
#include "shader_recompiler/backend/spirv/emit_spirv_quad_rect.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

using Shader::Backend::SPIRV::AuxShaderType;

static constexpr std::array LogicalStageToStageBit = {
    vk::ShaderStageFlagBits::eFragment,
    vk::ShaderStageFlagBits::eTessellationControl,
    vk::ShaderStageFlagBits::eTessellationEvaluation,
    vk::ShaderStageFlagBits::eVertex,
    vk::ShaderStageFlagBits::eGeometry,
    vk::ShaderStageFlagBits::eCompute,
};

GraphicsPipeline::GraphicsPipeline(
    const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
    const Shader::Profile& profile, const GraphicsPipelineKey& key_,
    vk::PipelineCache pipeline_cache, std::span<const Shader::Info*, MaxShaderStages> infos,
    std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader_,
    std::span<const vk::ShaderModule> modules, SerializationSupport& sdata, bool preloading,
    u64 vertex_output_locations)
    : Pipeline{instance, scheduler, desc_heap, profile, pipeline_cache}, key{key_},
      fetch_shader{std::move(fetch_shader_)} {
    // A motion variant: its fragment permutation's flag, compiled or preloaded (the warm-up
    // passes the loaded permutations' own runtime infos). Without a fragment program that slot
    // may hold another selection's: no motion then.
    vr_motion = infos[u32(Shader::LogicalStage::Fragment)] != nullptr &&
                runtime_infos[u32(Shader::LogicalStage::Fragment)].vr_motion.enabled &&
                !runtime_infos[u32(Shader::LogicalStage::Fragment)].vr_motion.Capture();
    // A body-map capture (vr_body_maps.h): its vertex stage stores, nothing else changes.
    vr_capture = infos[u32(Shader::LogicalStage::Vertex)] != nullptr &&
                 runtime_infos[u32(Shader::LogicalStage::Vertex)].vr_motion.Capture();
    const vk::Device device = instance.GetDevice();
    std::ranges::copy(infos, stages.begin());
    BuildDescSetLayout(preloading);
    const auto debug_str = GetDebugString();

    // One range for every graphics pipeline, cached, background and VR variants alike: push
    // constants then survive the binds between them (VrDrawCapeSplit's cut pipeline is bound in
    // the middle of a draw, with no push of its own). Native stereo: scene_inverse after
    // PushData, and with VrPushData the draw's projection, light tiles, cut and motion buffers.
    const bool scene_constants = profile.max_push_constant_size >= 136 && NativeStereo::Enabled();
    const bool vr_push = scene_constants && profile.SupportsVrPushData();
    const u32 push_size = vr_push           ? Shader::VrPushData::End
                          : scene_constants ? 136u
                                            : u32(sizeof(Shader::PushData));
    const vk::PushConstantRange push_constants{AllGraphicsStageBits, 0, push_size};

    const vk::DescriptorSetLayout set_layout = *desc_layout;
    const vk::PipelineLayoutCreateInfo layout_info = {
        .setLayoutCount = 1U,
        .pSetLayouts = &set_layout,
        .pushConstantRangeCount = 1u,
        .pPushConstantRanges = &push_constants,
    };
    auto [layout_result, layout] = instance.GetDevice().createPipelineLayoutUnique(layout_info);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics pipeline layout: {}", vk::to_string(layout_result));
    pipeline_layout = std::move(layout);
    push_range = push_size; // FAST_CPU_PUSH_SPLIT: the one range, the same for every layout
    SetObjectName(device, *pipeline_layout, "Graphics PipelineLayout {}", debug_str);

    if (!preloading) {
        VertexInputs<AmdGpu::Buffer> guest_buffers;
        if (!instance.IsVertexInputDynamicState()) {
            const auto& vs_info = runtime_infos[u32(Shader::LogicalStage::Vertex)].vs_info;
            GetVertexInputs(sdata.vertex_attributes, sdata.vertex_bindings, sdata.divisors,
                            guest_buffers, vs_info.step_rate_0, vs_info.step_rate_1);
        }
    }

    const vk::PipelineVertexInputDivisorStateCreateInfo divisor_state = {
        .vertexBindingDivisorCount = static_cast<u32>(sdata.divisors.size()),
        .pVertexBindingDivisors = sdata.divisors.data(),
    };

    const vk::PipelineVertexInputStateCreateInfo vertex_input_info = {
        .pNext = sdata.divisors.empty() ? nullptr : &divisor_state,
        .vertexBindingDescriptionCount = static_cast<u32>(sdata.vertex_bindings.size()),
        .pVertexBindingDescriptions = sdata.vertex_bindings.data(),
        .vertexAttributeDescriptionCount = static_cast<u32>(sdata.vertex_attributes.size()),
        .pVertexAttributeDescriptions = sdata.vertex_attributes.data(),
    };

    const auto topology = LiverpoolToVK::PrimitiveType(key.prim_type);
    const vk::PipelineInputAssemblyStateCreateInfo input_assembly = {
        .topology = topology,
    };

    const bool is_rect_list = key.prim_type == AmdGpu::PrimitiveType::RectList;
    const bool is_quad_list = key.prim_type == AmdGpu::PrimitiveType::QuadList;
    const vk::PipelineTessellationStateCreateInfo tessellation_state = {
        .patchControlPoints = is_rect_list ? 3U : (is_quad_list ? 4U : key.patch_control_points),
    };

    vk::StructureChain raster_chain = {
        vk::PipelineRasterizationStateCreateInfo{
            .depthClampEnable = key.depth_clamp_enable &&
                                (!key.depth_clip_enable || instance.IsDepthClipEnableSupported()),
            .rasterizerDiscardEnable = false,
            .polygonMode = LiverpoolToVK::PolygonMode(key.polygon_mode),
            .lineWidth = 1.0f,
        },
        vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT{
            .provokingVertexMode = key.provoking_vtx_last == AmdGpu::ProvokingVtxLast::First
                                       ? vk::ProvokingVertexModeEXT::eFirstVertex
                                       : vk::ProvokingVertexModeEXT::eLastVertex,
        },
        vk::PipelineRasterizationDepthClipStateCreateInfoEXT{
            .depthClipEnable = key.depth_clip_enable,
        },
    };

    if (!instance.IsProvokingVertexSupported()) {
        raster_chain.unlink<vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT>();
    }
    if (!instance.IsDepthClipEnableSupported()) {
        raster_chain.unlink<vk::PipelineRasterizationDepthClipStateCreateInfoEXT>();
    }

    if (!preloading) {
        const auto& fs_info = runtime_infos[u32(Shader::LogicalStage::Fragment)].fs_info;
        sdata.multisampling = {
            .rasterizationSamples = LiverpoolToVK::NumSamples(
                key.num_samples, instance.GetColorSampleCounts() & instance.GetDepthSampleCounts()),
            .sampleShadingEnable =
                fs_info.addr_flags.persp_sample_ena || fs_info.addr_flags.linear_sample_ena,
        };
    }

    const vk::PipelineViewportDepthClipControlCreateInfoEXT clip_control = {
        .negativeOneToOne = key.clip_space == AmdGpu::ClipSpace::MinusWToW,
    };

    const vk::PipelineViewportStateCreateInfo viewport_info = {
        .pNext = instance.IsDepthClipControlSupported() ? &clip_control : nullptr,
    };

    boost::container::static_vector<vk::DynamicState, 32> dynamic_states = {
        vk::DynamicState::eViewportWithCount,  vk::DynamicState::eScissorWithCount,
        vk::DynamicState::eBlendConstants,     vk::DynamicState::eDepthTestEnable,
        vk::DynamicState::eDepthWriteEnable,   vk::DynamicState::eDepthCompareOp,
        vk::DynamicState::eDepthBiasEnable,    vk::DynamicState::eDepthBias,
        vk::DynamicState::eStencilTestEnable,  vk::DynamicState::eStencilReference,
        vk::DynamicState::eStencilCompareMask, vk::DynamicState::eStencilWriteMask,
        vk::DynamicState::eStencilOp,          vk::DynamicState::eCullMode,
        vk::DynamicState::eFrontFace,          vk::DynamicState::eRasterizerDiscardEnable,
        vk::DynamicState::eLineWidth,          vk::DynamicState::ePrimitiveRestartEnable,
    };

    if (instance.IsDepthBoundsSupported()) {
        dynamic_states.push_back(vk::DynamicState::eDepthBoundsTestEnable);
        dynamic_states.push_back(vk::DynamicState::eDepthBounds);
    }
    if (instance.IsDynamicColorWriteMaskSupported()) {
        dynamic_states.push_back(vk::DynamicState::eColorWriteMaskEXT);
    }
    if (instance.IsVertexInputDynamicState()) {
        dynamic_states.push_back(vk::DynamicState::eVertexInputEXT);
    } else if (!sdata.vertex_bindings.empty()) {
        dynamic_states.push_back(vk::DynamicState::eVertexInputBindingStride);
    }

    const vk::PipelineDynamicStateCreateInfo dynamic_info = {
        .dynamicStateCount = static_cast<u32>(dynamic_states.size()),
        .pDynamicStates = dynamic_states.data(),
    };

    boost::container::static_vector<vk::PipelineShaderStageCreateInfo, MaxShaderStages>
        shader_stages;
    auto stage = u32(Shader::LogicalStage::Vertex);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eVertex,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(Shader::LogicalStage::Geometry);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eGeometry,
            .module = modules[stage],
            .pName = "main",
        });
    }
    stage = u32(Shader::LogicalStage::TessellationControl);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (is_rect_list || is_quad_list) {
        const auto type = is_quad_list ? AuxShaderType::QuadListTCS : AuxShaderType::RectListTCS;
        if (!preloading) {
            sdata.tcs = Shader::Backend::SPIRV::EmitAuxilaryTessShader(type, vertex_output_locations);
        }
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationControl,
            .module = CompileSPV(sdata.tcs, instance.GetDevice()),
            .pName = "main",
        });
    }
    stage = u32(Shader::LogicalStage::TessellationEval);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (is_rect_list || is_quad_list) {
        if (!preloading) {
            sdata.tes = Shader::Backend::SPIRV::EmitAuxilaryTessShader(
                AuxShaderType::PassthroughTES, vertex_output_locations,
                NativeStereo::LayerDraws() && key.probe_view_mask == 3);
        }
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eTessellationEvaluation,
            .module = CompileSPV(sdata.tes, instance.GetDevice()),
            .pName = "main",
        });
    }
    stage = u32(Shader::LogicalStage::Fragment);
    if (infos[stage]) {
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = modules[stage],
            .pName = "main",
        });
    } else if (preloading
                   ? !sdata.fragment.empty()
                   : runtime_infos[u32(Shader::LogicalStage::Fragment)]
                         .fs_info.clip_distance_emulation) {
        if (!preloading) {
            const auto vs_runtime_info =
                runtime_infos[static_cast<u32>(Shader::LogicalStage::Vertex)].vs_info;

            sdata.fragment =
                Shader::Backend::SPIRV::EmitDiscardFragmentShader(vs_runtime_info.outputs);
        }
        shader_stages.emplace_back(vk::PipelineShaderStageCreateInfo{
            .stage = vk::ShaderStageFlagBits::eFragment,
            .module = CompileSPV(sdata.fragment, instance.GetDevice()),
            .pName = "main",
        });
    }

    const auto depth_format =
        instance.GetSupportedFormat(LiverpoolToVK::DepthFormat(key.z_format, key.stencil_format),
                                    vk::FormatFeatureFlagBits2::eDepthStencilAttachment);
    std::array<vk::Format, Shader::IR::NumRenderTargets> color_formats;
    for (s32 i = 0; i < key.num_color_attachments; ++i) {
        // Motion extends the render target array to slot 7. Intermediate guest
        // slots with no attachment must remain undefined in dynamic rendering.
        if (vr_motion && i != Shader::VrMotion::Output && !(key.mrt_mask & (1u << i))) {
            color_formats[i] = vk::Format::eUndefined;
            continue;
        }
        const auto& col_buf = key.color_buffers[i];
        const auto format = LiverpoolToVK::SurfaceFormat(col_buf.data_format, col_buf.num_format);
        const auto color_format =
            instance.GetSupportedFormat(format, vk::FormatFeatureFlagBits2::eColorAttachment);
        if (!instance.IsFormatSupported(color_format,
                                        vk::FormatFeatureFlagBits2::eColorAttachment)) {
            LOG_WARNING(Render_Vulkan,
                        "color buffer format {} does not support COLOR_ATTACHMENT_BIT",
                        vk::to_string(color_format));
        }
        color_formats[i] = color_format;
    }

    std::array<vk::SampleCountFlagBits, AmdGpu::NUM_COLOR_BUFFERS> color_samples;
    std::ranges::transform(key.color_samples, color_samples.begin(), [&instance](u8 num_samples) {
        return num_samples ? LiverpoolToVK::NumSamples(num_samples, instance.GetColorSampleCounts())
                           : vk::SampleCountFlagBits::e1;
    });
    const vk::AttachmentSampleCountInfoAMD mixed_samples = {
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentSamples = color_samples.data(),
        .depthStencilAttachmentSamples =
            LiverpoolToVK::NumSamples(key.depth_samples, instance.GetDepthSampleCounts()),
    };

    const vk::PipelineRenderingCreateInfo pipeline_rendering_ci = {
        .pNext = instance.IsMixedDepthSamplesSupported() ? &mixed_samples : nullptr,
        // Layer draws: a both-eye pipeline renders without multiview (NativeStereo::LayerDraws).
        .viewMask = NativeStereo::LayerDraws() && key.probe_view_mask == 3 ? 0u
                                                                           : key.probe_view_mask,
        .colorAttachmentCount = key.num_color_attachments,
        .pColorAttachmentFormats = color_formats.data(),
        .depthAttachmentFormat = key.z_format != AmdGpu::DepthBuffer::ZFormat::Invalid
                                     ? depth_format
                                     : vk::Format::eUndefined,
        .stencilAttachmentFormat = key.stencil_format != AmdGpu::DepthBuffer::StencilFormat::Invalid
                                       ? depth_format
                                       : vk::Format::eUndefined,
    };

    std::array<vk::PipelineColorBlendAttachmentState, AmdGpu::NUM_COLOR_BUFFERS> attachments;
    for (u32 i = 0; i < key.num_color_attachments; i++) {
        const auto& control = key.blend_controls[i];

        const auto src_color = LiverpoolToVK::BlendFactor(control.color_src_factor);
        const auto dst_color = LiverpoolToVK::BlendFactor(control.color_dst_factor);
        const auto color_blend = LiverpoolToVK::BlendOp(control.color_func);

        const auto src_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_src_factor)
                                   : src_color;
        const auto dst_alpha = control.separate_alpha_blend
                                   ? LiverpoolToVK::BlendFactor(control.alpha_dst_factor)
                                   : dst_color;
        const auto alpha_blend =
            control.separate_alpha_blend ? LiverpoolToVK::BlendOp(control.alpha_func) : color_blend;

        const auto color_scaled_min_max =
            (color_blend == vk::BlendOp::eMin || color_blend == vk::BlendOp::eMax) &&
            (src_color != vk::BlendFactor::eOne || dst_color != vk::BlendFactor::eOne);
        const auto alpha_scaled_min_max =
            (alpha_blend == vk::BlendOp::eMin || alpha_blend == vk::BlendOp::eMax) &&
            (src_alpha != vk::BlendFactor::eOne || dst_alpha != vk::BlendFactor::eOne);
        if (color_scaled_min_max || alpha_scaled_min_max) {
            LOG_WARNING(
                Render_Vulkan,
                "Unimplemented use of min/max blend op with blend factor not equal to one.");
        }

        attachments[i] = vk::PipelineColorBlendAttachmentState{
            .blendEnable = control.enable,
            .srcColorBlendFactor = src_color,
            .dstColorBlendFactor = dst_color,
            .colorBlendOp = color_blend,
            .srcAlphaBlendFactor = src_alpha,
            .dstAlphaBlendFactor = dst_alpha,
            .alphaBlendOp = alpha_blend,
            .colorWriteMask =
                instance.IsDynamicColorWriteMaskSupported()
                    ? vk::ColorComponentFlagBits::eR | vk::ColorComponentFlagBits::eG |
                          vk::ColorComponentFlagBits::eB | vk::ColorComponentFlagBits::eA
                    : key.write_masks[i],
        };

        // On GCN GPU there is an additional mask which allows to control color components exported
        // from a pixel shader. A situation possible, when the game may mask out the alpha channel,
        // while it is still need to be used in blending ops. For such cases, HW will default alpha
        // to 1 and perform the blending, while shader normally outputs 0 in the last component.
        // Unfortunatelly, Vulkan doesn't provide any control on blend inputs, so below we detecting
        // such cases and override alpha value in order to emulate HW behaviour.
        const auto has_alpha_masked_out =
            (key.cb_shader_mask.GetMask(i) & AmdGpu::ColorBufferMask::ComponentA) == 0;
        const auto has_src_alpha_in_src_blend = src_color == vk::BlendFactor::eSrcAlpha ||
                                                src_color == vk::BlendFactor::eOneMinusSrcAlpha;
        const auto has_src_alpha_in_dst_blend = dst_color == vk::BlendFactor::eSrcAlpha ||
                                                dst_color == vk::BlendFactor::eOneMinusSrcAlpha;
        if (has_alpha_masked_out && has_src_alpha_in_src_blend) {
            attachments[i].srcColorBlendFactor = src_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
        if (has_alpha_masked_out && has_src_alpha_in_dst_blend) {
            attachments[i].dstColorBlendFactor = dst_color == vk::BlendFactor::eSrcAlpha
                                                     ? vk::BlendFactor::eOne
                                                     : vk::BlendFactor::eZero; // 1-A
        }
    }

    const vk::PipelineColorBlendStateCreateInfo color_blending = {
        .logicOpEnable =
            instance.IsLogicOpSupported() && key.logic_op != AmdGpu::ColorControl::LogicOp::Copy,
        .logicOp = LiverpoolToVK::LogicOp(key.logic_op),
        .attachmentCount = key.num_color_attachments,
        .pAttachments = attachments.data(),
        .blendConstants = std::array{1.0f, 1.0f, 1.0f, 1.0f},
    };

    // Required by spec unless VK_EXT_extended_dynamic_state3 is supported.
    // In practice, we use dynamic state for all of it.
    constexpr vk::PipelineDepthStencilStateCreateInfo depth_stencil_info = {};

    // Fixed foveated rendering (vr_foveation.h): every pipeline may run in a pass with a shading
    // rate attachment; the pipeline's own rate stays 1x1 and the attachment's replaces it (a pass
    // without one shades at full rate, as before). A fragment shader writing depth/stencil or the
    // sample mask on a card that restricts coarse shading with it keeps the attachment out
    // (always 1x1), as the extension requires.
    const bool foveated = instance.IsFoveationEnabled();
    bool full_rate_only = false;
    if (foveated) {
        if (const auto* fs = infos[u32(Shader::LogicalStage::Fragment)]) {
            const bool depth = fs->stores.Get(Shader::IR::Attribute::Depth) ||
                               fs->stores.Get(Shader::IR::Attribute::StencilRef);
            const bool mask = fs->stores.Get(Shader::IR::Attribute::SampleMask) ||
                              runtime_infos[u32(Shader::LogicalStage::Fragment)].fs_info.vr_cut != 0;
            // A shader writing the lamp shadows' cubes or mask (light_shadow.h SceneMaps: the AO
            // pass) writes for its own pixel only: at a coarse rate the pixels it skips keep no
            // shadow, so it runs at full rate.
            const bool writes_shadows = std::ranges::any_of(fs->buffers, [](const auto& buffer) {
                return buffer.is_written && (buffer.buffer_type == Shader::BufferType::VrShadowMask ||
                                             buffer.buffer_type == Shader::BufferType::VrSceneMaps);
            });
            // (No longer: the shadow work covers a coarse fragment's block itself, and the AO the
            // pass makes stays at the eye's rate, as the game had it.)
            (void)writes_shadows;
            full_rate_only = (depth && !instance.FoveationWithShaderDepth()) ||
                             (mask && !instance.FoveationWithShaderSampleMask());
        }
    }
    const vk::PipelineFragmentShadingRateStateCreateInfoKHR foveation_rate = {
        .pNext = &pipeline_rendering_ci,
        .fragmentSize = vk::Extent2D{1, 1},
        .combinerOps = std::array{vk::FragmentShadingRateCombinerOpKHR::eKeep,
                                  full_rate_only ? vk::FragmentShadingRateCombinerOpKHR::eKeep
                                                 : vk::FragmentShadingRateCombinerOpKHR::eReplace},
    };
    const vk::GraphicsPipelineCreateInfo pipeline_info = {
        .pNext = foveated ? static_cast<const void*>(&foveation_rate)
                          : static_cast<const void*>(&pipeline_rendering_ci),
        .flags = foveated ? vk::PipelineCreateFlagBits::eRenderingFragmentShadingRateAttachmentKHR
                          : vk::PipelineCreateFlags{},
        .stageCount = static_cast<u32>(shader_stages.size()),
        .pStages = shader_stages.data(),
        .pVertexInputState = !instance.IsVertexInputDynamicState() ? &vertex_input_info : nullptr,
        .pInputAssemblyState = &input_assembly,
        .pTessellationState = &tessellation_state,
        .pViewportState = &viewport_info,
        .pRasterizationState = &raster_chain.get(),
        .pMultisampleState = &sdata.multisampling,
        .pDepthStencilState =
            !instance.IsExtendedDynamicState3Supported() ? &depth_stencil_info : nullptr,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_info,
        .layout = *pipeline_layout,
    };

    // VR_EYE_SPEC: what a variant is built from, the create info as it stands (copied).
    if (Shader::EyeSpec::Enabled() && NativeStereo::Enabled() && eye_spec_buffers != 0) {
        eye_spec_recipe = EyeSpecVariants::CopyRecipe(pipeline_info, device, pipeline_cache);
        if (!eye_spec_recipe) {
            EyeSpecVariants::g_stats.no_recipe.fetch_add(1, std::memory_order_relaxed);
        }
    }
    auto [pipeline_result, pipe] =
        device.createGraphicsPipelineUnique(pipeline_cache, pipeline_info);
    if (pipeline_result != vk::Result::eSuccess && g_tolerate_pipeline_rejection) {
        // Fast bit 40: the caller prunes the cache entry (warm-up) or compiles on its own thread.
        LOG_ERROR(Render_Vulkan, "Driver rejected graphics pipeline {}: {}", debug_str,
                  vk::to_string(pipeline_result));
        return;
    }
    ASSERT_MSG(pipeline_result == vk::Result::eSuccess, "Failed to create graphics pipeline: {}",
               vk::to_string(pipeline_result));
    pipeline = std::move(pipe);
    SetObjectName(device, *pipeline, "Graphics Pipeline {}", debug_str);
}

GraphicsPipeline::~GraphicsPipeline() = default;

bool GraphicsPipeline::DropEyeSpecRecipe(std::span<const vk::ShaderModule> gone) {
    if (!eye_spec_recipe) {
        return false;
    }
    for (const auto& stage : eye_spec_recipe->stages) {
        if (std::ranges::find(gone, stage.module) != gone.end()) {
            eye_spec_recipe.reset();
            return true;
        }
    }
    return false;
}

void GraphicsPipeline::EyeSelect(EyeSpecVariants::DrawMask& drawn) const {
    drawn.handle = vk::Pipeline{};
    if (!eye_spec_recipe) {
        return;
    }
    using namespace EyeSpecVariants;
    auto& s = g_stats;
    constexpr auto relaxed = std::memory_order_relaxed;
    static thread_local u32 log_tick = 0;
    if ((++log_tick & 1023) == 0) {
        MaybeLog();
    }
    const u64 shared = drawn.shared & eye_spec_buffers;
    // VR_EYE_SPEC=2 only: strided pairs (=1 leaves them on their dynamic index).
    const u64 strided = Shader::EyeSpec::Strides() ? drawn.strided & eye_spec_strides : 0;
    if (eye_spec_slot) {
        const Slot& slot = *eye_spec_slot;
        switch (slot.state.load(std::memory_order_acquire)) {
        case State::Ready: {
            if (!FastRender::AbOn(UseBit)) {
                s.none.fetch_add(1, relaxed); // in-run A/B: the base pipeline this phase
                return;
            }
            bool fits = (shared & (slot.mask & ~slot.strided)) == (slot.mask & ~slot.strided) &&
                        (strided & slot.strided) == slot.strided;
            for (u32 i = 0; fits && i < slot.strides.size(); ++i) {
                const Stride* const mine = drawn.StrideOf(slot.strides[i].binding);
                fits = mine && mine->delta == slot.strides[i].delta &&
                       mine->size == slot.strides[i].size;
            }
            if (!fits) {
                s.mismatch.fetch_add(1, relaxed);
                return;
            }
            // Descriptor 0 of each strided binding covers both eyes' copies.
            for (const auto& want : slot.strides) {
                Stride* const mine = const_cast<Stride*>(drawn.StrideOf(want.binding));
                mine->left->range = u64{want.delta} + want.size;
            }
            drawn.handle = *slot.pipeline;
            s.variant.fetch_add(1, relaxed);
            if (!slot.strides.empty()) {
                s.variant_strided.fetch_add(1, relaxed);
            }
            return;
        }
        case State::Compiling:
            s.compiling.fetch_add(1, relaxed);
            return;
        case State::Failed:
            s.none.fetch_add(1, relaxed);
            return;
        }
        return;
    }
    // The bindings every one of the first draws shared, with the same stride and size if strided.
    if (!eye_spec_learn) {
        eye_spec_learn = std::make_unique<Learn>();
    }
    Learn& learn = *eye_spec_learn;
    learn.shared &= shared;
    if (learn.first) {
        learn.first = false;
        learn.strided = strided;
        for (u32 i = 0; i < drawn.stride_count; ++i) {
            learn.strides[drawn.strides[i].binding] = drawn.strides[i];
        }
    } else {
        learn.strided &= strided;
        for (u64 bits = learn.strided; bits != 0; bits &= bits - 1) {
            const u32 b = static_cast<u32>(std::countr_zero(bits));
            const Stride* const mine = drawn.StrideOf(b);
            if (!mine || mine->delta != learn.strides[b].delta ||
                mine->size != learn.strides[b].size) {
                learn.strided &= ~(u64{1} << b);
            }
        }
    }
    s.learning.fetch_add(1, relaxed);
    if (++eye_spec_draws < LearnDraws) {
        return;
    }
    auto slot = std::make_shared<Slot>();
    slot->strided = learn.strided & ~learn.shared;
    slot->mask = learn.shared | slot->strided;
    for (u64 bits = slot->strided; bits != 0; bits &= bits - 1) {
        const u32 b = static_cast<u32>(std::countr_zero(bits));
        slot->strides.push_back({b, learn.strides[b].delta, learn.strides[b].size, nullptr});
    }
    eye_spec_learn.reset();
    Compiler* const compiler = g_compiler.load(std::memory_order_acquire);
    if (slot->mask == 0 || !compiler) {
        slot->state.store(State::Failed, relaxed);
    } else {
        s.requested.fetch_add(1, relaxed);
        compiler->Submit(slot, eye_spec_recipe);
    }
    eye_spec_slot = std::move(slot);
}

template <typename Attribute, typename Binding>
void GraphicsPipeline::GetVertexInputs(
    VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const {
    using InstanceIdType = Shader::Gcn::VertexAttribute::InstanceIdType;
    if (!fetch_shader || fetch_shader->attributes.empty()) {
        return;
    }
    const auto& vs_info = GetStage(Shader::LogicalStage::Vertex);
    for (const auto& attrib : fetch_shader->attributes) {
        const auto step_rate = attrib.GetStepRate();
        const auto buffer = attrib.GetSharp(vs_info);
        attributes.push_back(Attribute{
            .location = attrib.semantic,
            .binding = attrib.semantic,
            .format = LiverpoolToVK::SurfaceFormat(buffer.GetDataFmt(), buffer.GetNumberFmt()),
            .offset = 0,
        });
        bindings.push_back(Binding{
            .binding = attrib.semantic,
            .stride = buffer.GetStride(),
            .inputRate = step_rate == InstanceIdType::None ? vk::VertexInputRate::eVertex
                                                           : vk::VertexInputRate::eInstance,
        });
        const u32 divisor = step_rate == InstanceIdType::OverStepRate0
                                ? step_rate_0
                                : (step_rate == InstanceIdType::OverStepRate1 ? step_rate_1 : 1);
        if constexpr (std::is_same_v<Binding, vk::VertexInputBindingDescription2EXT>) {
            bindings.back().divisor = divisor;
        } else if (step_rate != InstanceIdType::None) {
            divisors.push_back(vk::VertexInputBindingDivisorDescriptionEXT{
                .binding = attrib.semantic,
                .divisor = divisor,
            });
        }
        guest_buffers.emplace_back(buffer);
    }
}

// Declare templated GetVertexInputs for necessary types.
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription>& attributes,
    VertexInputs<vk::VertexInputBindingDescription>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const;
template void GraphicsPipeline::GetVertexInputs(
    VertexInputs<vk::VertexInputAttributeDescription2EXT>& attributes,
    VertexInputs<vk::VertexInputBindingDescription2EXT>& bindings,
    VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
    VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0, u32 step_rate_1) const;

void GraphicsPipeline::BuildDescSetLayout(bool preloading) {
    boost::container::small_vector<vk::DescriptorSetLayoutBinding, 32> bindings;
    u32 binding{};

    for (const auto* stage : stages) {
        if (!stage) {
            continue;
        }
        const auto stage_bit = LogicalStageToStageBit[u32(stage->l_stage)];
        for (const auto& buffer : stage->buffers) {
            const auto sharp =
                preloading ? AmdGpu::Buffer{}
                           : buffer.GetSharp(*stage); // See for the comment in compute PL creation
            if (binding < Shader::EyeSpec::MaxBindings) {
                eye_spec_buffers |= u64{1} << binding;
                if (buffer.buffer_type == Shader::BufferType::Guest && !buffer.is_written) {
                    eye_spec_strides |= u64{1} << binding;
                }
            }
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eStorageBuffer,
                .descriptorCount = NativeStereo::Enabled() ? 2u : 1u,
                .stageFlags = stage_bit,
            });
        }
        for (const auto& image : stage->images) {
            const u32 num_bindings = image.NumBindings(*stage);
            bindings.push_back({
                .binding = binding,
                .descriptorType = image.is_written ? vk::DescriptorType::eStorageImage
                                                   : vk::DescriptorType::eSampledImage,
                .descriptorCount = num_bindings * (NativeStereo::Enabled() ? 2 : 1),
                .stageFlags = stage_bit,
            });
            binding += num_bindings;
        }
        for (const auto& sampler : stage->samplers) {
            bindings.push_back({
                .binding = binding++,
                .descriptorType = vk::DescriptorType::eSampler,
                .descriptorCount = 1,
                .stageFlags = stage_bit,
            });
        }
    }
    uses_push_descriptors = !NativeStereo::Enabled() && binding < instance.MaxPushDescriptors();
    static const bool native_push = [] {
        const char* setting = std::getenv("VR_NATIVE_PUSH_DESCRIPTORS");
        return !setting || setting[0] != '0';
    }();
    if (NativeStereo::Enabled() && native_push) {
        u64 descriptor_count = 0;
        for (const auto& entry : bindings) descriptor_count += entry.descriptorCount;
        // The limit counts array elements, not binding numbers. A zero limit means
        // push descriptors are unavailable; retain the ordinary descriptor-set path.
        uses_push_descriptors = instance.MaxPushDescriptors() != 0 &&
                               descriptor_count <= instance.MaxPushDescriptors();
        LOG_INFO(Render_Vulkan, "VR native descriptor layout: {} elements, limit {}, push {}",
                 descriptor_count, instance.MaxPushDescriptors(), uses_push_descriptors);
    }
    const auto flags = uses_push_descriptors
                           ? vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR
                           : vk::DescriptorSetLayoutCreateFlagBits{};
    const vk::DescriptorSetLayoutCreateInfo desc_layout_ci = {
        .flags = flags,
        .bindingCount = static_cast<u32>(bindings.size()),
        .pBindings = bindings.data(),
    };
    auto [layout_result, layout] =
        instance.GetDevice().createDescriptorSetLayoutUnique(desc_layout_ci);
    ASSERT_MSG(layout_result == vk::Result::eSuccess,
               "Failed to create graphics descriptor set layout: {}", vk::to_string(layout_result));
    desc_layout = std::move(layout);
}

} // namespace Vulkan
