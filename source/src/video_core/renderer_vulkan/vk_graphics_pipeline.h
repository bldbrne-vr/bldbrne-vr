// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <boost/container/static_vector.hpp>
#include <span>
#include <xxhash.h>

#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_depth.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vr_eye_spec_variants.h"

namespace VideoCore {
class BufferCache;
class TextureCache;
} // namespace VideoCore

namespace Vulkan {

static constexpr u32 MaxShaderStages = static_cast<u32>(Shader::LogicalStage::NumLogicalStages);
static constexpr u32 MaxVertexBufferCount = 32;

class Instance;
class Scheduler;
class DescriptorHeap;

template <typename T>
using VertexInputs = boost::container::static_vector<T, MaxVertexBufferCount>;

// Owned per-draw values; resource resolution and barriers remain on the recording thread.
struct VertexPreparation {
    VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
    VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
    VertexInputs<AmdGpu::Buffer> guest_buffers;
    struct Range {
        VAddr base_address{}, end_address{};
        size_t GetSize() const { return end_address - base_address; }
        bool operator==(const Range&) const = default;
    };
    VertexInputs<Range> ranges;
    u64 geometry{};
};

// What BufferCache::BindVertexBuffers reads of a VertexPreparation. FAST_CPU_SLIM_PACKET: stage B
// borrows the arrays from the packet it consumes (like the packet's other arrays) instead of
// copying them into its own VertexPreparation; otherwise they name a VertexPreparation's own.
struct VertexView {
    std::span<const vk::VertexInputAttributeDescription2EXT> attributes;
    std::span<const vk::VertexInputBindingDescription2EXT> bindings;
    std::span<const AmdGpu::Buffer> guest_buffers;
    std::span<const VertexPreparation::Range> ranges;
    u64 geometry{};
    VertexView() = default;
    explicit VertexView(const VertexPreparation& prepared)
        : attributes(prepared.attributes.data(), prepared.attributes.size()),
          bindings(prepared.bindings.data(), prepared.bindings.size()),
          guest_buffers(prepared.guest_buffers.data(), prepared.guest_buffers.size()),
          ranges(prepared.ranges.data(), prepared.ranges.size()), geometry(prepared.geometry) {}
};

// FAST_CPU_STAGEB_SPLIT (stage 2): the vertex input and
// the vertex and index binds a draw encodes, as BufferCache::BindVertexBuffers / BindIndexBuffer
// resolved them (buffers obtained, barriers added): owned values, encoded by EncodeVertexBinds.
struct VertexBinds {
    bool vertex_input{}; // setVertexInputEXT (dynamic vertex input)
    VertexInputs<vk::VertexInputBindingDescription2EXT> bindings;
    VertexInputs<vk::VertexInputAttributeDescription2EXT> attributes;
    bool two{}; // bindVertexBuffers2 (sizes and strides) instead of bindVertexBuffers
    VertexInputs<vk::Buffer> buffers;
    VertexInputs<vk::DeviceSize> offsets, sizes, strides;
    bool index{};
    vk::Buffer index_buffer{};
    vk::DeviceSize index_offset{};
    vk::IndexType index_type{};
    void Clear() {
        vertex_input = two = index = false;
        bindings.clear();
        attributes.clear();
        buffers.clear();
        offsets.clear();
        sizes.clear();
        strides.clear();
    }
};

struct GraphicsPipelineKey {
    std::array<size_t, MaxShaderStages> stage_hashes;
    std::array<vk::Format, MaxVertexBufferCount> vertex_buffer_formats;
    u32 probe_view_mask;
    u32 patch_control_points;
    u32 num_color_attachments;
    std::array<Shader::PsColorBuffer, AmdGpu::NUM_COLOR_BUFFERS> color_buffers;
    std::array<AmdGpu::BlendControl, AmdGpu::NUM_COLOR_BUFFERS> blend_controls;
    std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS> write_masks;
    AmdGpu::ColorBufferMask cb_shader_mask;
    AmdGpu::ColorControl::LogicOp logic_op;
    u8 num_samples;
    u8 depth_samples;
    std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples;
    u32 mrt_mask;
    struct {
        AmdGpu::DepthBuffer::ZFormat z_format : 2;
        AmdGpu::DepthBuffer::StencilFormat stencil_format : 1;
        u32 depth_clamp_enable : 1;
    };
    struct {
        AmdGpu::PrimitiveType prim_type : 5;
        AmdGpu::PolygonMode polygon_mode : 2;
        AmdGpu::ClipSpace clip_space : 1;
        AmdGpu::ProvokingVtxLast provoking_vtx_last : 1;
        u32 depth_clip_enable : 1;
    };

    GraphicsPipelineKey() {
        std::memset(this, 0, sizeof(*this));
    }

    bool operator==(const GraphicsPipelineKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }

    void Serialize(Serialization::Archive& ar) const;
    bool Deserialize(Serialization::Archive& ar);
};

class GraphicsPipeline : public Pipeline {
public:
    struct SerializationSupport {
        VertexInputs<vk::VertexInputAttributeDescription> vertex_attributes{};
        VertexInputs<vk::VertexInputBindingDescription> vertex_bindings{};
        VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT> divisors{};
        vk::PipelineMultisampleStateCreateInfo multisampling{};
        std::vector<u32> tcs{};
        std::vector<u32> tes{};
        std::vector<u32> fragment{};

        void Serialize(Serialization::Archive& ar) const;
        bool Deserialize(Serialization::Archive& ar);
    };

    GraphicsPipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
                     const Shader::Profile& profile, const GraphicsPipelineKey& key,
                     vk::PipelineCache pipeline_cache,
                     std::span<const Shader::Info*, MaxShaderStages> stages,
                     std::span<const Shader::RuntimeInfo, MaxShaderStages> runtime_infos,
                     std::optional<const Shader::Gcn::FetchShaderData> fetch_shader,
                     std::span<const vk::ShaderModule> modules, SerializationSupport& sdata,
                     bool preloading, u64 vertex_output_locations = 0);
    ~GraphicsPipeline();

    const std::optional<const Shader::Gcn::FetchShaderData>& GetFetchShader() const noexcept {
        return fetch_shader;
    }

    const GraphicsPipelineKey& GetGraphicsKey() const {
        return key;
    }

    bool HasVrMotion() const { return vr_motion; }
    // A body-map capture variant (Shader::VrMotion::Capture, vr_body_maps.h).
    bool HasVrCapture() const { return vr_capture; }

    /// VR_EYE_SPEC (vr_eye_spec_variants.h), from Pipeline::BindResources after the draw's
    /// pairing and before its descriptors are recorded: sets drawn.handle to the eye-shared
    /// variant when it is built and the draw shares every binding it shares (equal descriptors,
    /// or the same stride and size), widening each strided binding's left descriptor to cover
    /// both copies; else leaves it null (the base pipeline). Learns over the pipeline's first
    /// draws and asks for the variant. Recording owner only.
    void EyeSelect(EyeSpecVariants::DrawMask& drawn) const;

    /// VR_EYE_SPEC: forgets the variant's recipe when it builds from one of `gone` (modules the
    /// warm-up destroyed with a cached pipeline the driver rejected; the recipe holds their
    /// handles). This pipeline draws on as it is, without a variant. Before its first draw only.
    bool DropEyeSpecRecipe(std::span<const vk::ShaderModule> gone);

    /// Gets the attributes and bindings for vertex inputs.
    template <typename Attribute, typename Binding>
    void GetVertexInputs(VertexInputs<Attribute>& attributes, VertexInputs<Binding>& bindings,
                         VertexInputs<vk::VertexInputBindingDivisorDescriptionEXT>& divisors,
                         VertexInputs<AmdGpu::Buffer>& guest_buffers, u32 step_rate_0,
                         u32 step_rate_1) const;

private:
    void BuildDescSetLayout(bool preloading);

private:
    GraphicsPipelineKey key;
    bool vr_motion{};
    bool vr_capture{};
    std::optional<const Shader::Gcn::FetchShaderData> fetch_shader{};
    // VR_EYE_SPEC: the layout's storage buffer bindings (below EyeSpec::MaxBindings), the copy of
    // the create info variants are built from (null: no variants), what the first draws shared,
    // and the variant.
    u64 eye_spec_buffers{};
    // VR_EYE_SPEC=2: the bindings whose modules get a stride and a size (read-only game buffers,
    // as EmitContext::DefineBuffers makes them); only these may share a strided pair. Sharing one
    // without them would read the left eye's copy in both eyes.
    u64 eye_spec_strides{};
    std::shared_ptr<const EyeSpecVariants::Recipe> eye_spec_recipe;
    mutable u32 eye_spec_draws{};
    mutable std::unique_ptr<EyeSpecVariants::Learn> eye_spec_learn;
    mutable std::shared_ptr<EyeSpecVariants::Slot> eye_spec_slot;
};

struct ClipDistanceShaderKey {
    std::array<std::tuple<u8, u8>, 8> clip_locations;

    bool operator==(const ClipDistanceShaderKey& key) const noexcept {
        return std::memcmp(this, &key, sizeof(key)) == 0;
    }
};

} // namespace Vulkan

template <>
struct std::hash<Vulkan::GraphicsPipelineKey> {
    std::size_t operator()(const Vulkan::GraphicsPipelineKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};

template <>
struct std::hash<Vulkan::ClipDistanceShaderKey> {
    std::size_t operator()(const Vulkan::ClipDistanceShaderKey& key) const noexcept {
        return XXH3_64bits(&key, sizeof(key));
    }
};
