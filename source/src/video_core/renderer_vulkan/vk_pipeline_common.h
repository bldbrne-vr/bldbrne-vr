// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_push_template.h"

#include <memory>
#include <span>
#include <vector>
#include <boost/container/small_vector.hpp>

namespace Shader {
struct Info;
struct PushData;
} // namespace Shader

namespace NativeStereo {
struct Handles;
struct Descriptors;
}

namespace Vulkan {
namespace EyeOverlap {
struct Check;
}
namespace StereoSetCodec {
struct Rights;
}

static constexpr auto AllGraphicsStageBits =
    vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl |
    vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry |
    vk::ShaderStageFlagBits::eFragment;

class Instance;
class Scheduler;
class DescriptorHeap;

// Fast bit 40 (vk_pipeline_async.cpp, the warm-up in vk_pipeline_serialization.cpp): set on a
// thread creating a cached or background pipeline. A driver rejection of the pipeline itself
// then leaves Handle() null for the caller to prune or rebuild, instead of asserting here.
inline thread_local bool g_tolerate_pipeline_rejection{};

class Pipeline {
public:
    Pipeline(const Instance& instance, Scheduler& scheduler, DescriptorHeap& desc_heap,
             const Shader::Profile& profile, vk::PipelineCache pipeline_cache,
             bool is_compute = false);
    virtual ~Pipeline();

    vk::Pipeline Handle() const noexcept {
        return *pipeline;
    }

    vk::PipelineLayout GetLayout() const noexcept {
        return *pipeline_layout;
    }

    auto GetStages() const {
        static_assert(static_cast<u32>(Shader::LogicalStage::Compute) == Shader::MaxStageTypes - 1);
        if (is_compute) {
            return std::span{stages.cend() - 1, stages.cend()};
        } else {
            return std::span{stages.cbegin(), stages.cend() - 1};
        }
    }

    const Shader::Info& GetStage(Shader::LogicalStage stage) const noexcept {
        return *stages[u32(stage)];
    }

    // VR pass markers refer to guest programs, not the order in which variants compile.
    // Keep the historic variant-zero encoding used by vr.ini markers.
    u64 GuestShaderKey(Shader::LogicalStage stage) const noexcept;

    std::span<const u8> NativeWritableBindings() const;

    /// What the stages may write: raw memory (DMA), a buffer, a storage image. Fixed after
    /// construction, so worked out once (recording thread) instead of at every draw.
    struct StageWrites {
        bool dma, buffers, images;
    };
    const StageWrites& Writes() const;

    /// The VR buffers and effect passes the fragment stage uses, worked out once: the
    /// rasterizer's per-draw VR hooks test a bit instead of scanning its buffer list at every
    /// draw. ShadowMaskRead: its first VrShadowMask buffer is read, not written.
    enum VrFsBuffer : u32 {
        VrFsBodyMaps = 1u << 0,
        VrFsSceneMaps = 1u << 1,
        VrFsShadowMaskRead = 1u << 2,
        VrFsDepthPyramid = 1u << 3,
        VrFsShadowEdges = 1u << 4,
        VrFsAirOrMoonPass = 1u << 5,
        VrFsShadowGrid = 1u << 6,
    };
    u32 VrFsBuffers() const;

    bool IsCompute() const {
        return is_compute;
    }

    using DescriptorWrites = std::vector<vk::WriteDescriptorSet>;
    using BufferBarriers = boost::container::small_vector<vk::BufferMemoryBarrier2, 16>;

    /// `vr_push`: native stereo graphics only, the values pushed after `push_data` (all of
    /// Shader::VrPushData when the device has room for it, else its scene_inverse alone).
    /// `native_handles` (fast bit 25): right-eye handles carried from the rasterizer's binding.
    /// `eye_check` (fast bit 53, vk_dispatch_eye_overlap.h): classify the paired descriptors.
    void BindResources(DescriptorWrites& set_writes, const BufferBarriers& buffer_barriers,
                       const Shader::PushData& push_data, const Shader::VrPushData& vr_push,
                       const NativeStereo::Handles* native_handles = nullptr,
                       EyeOverlap::Check* eye_check = nullptr,
                       std::span<const vk::DescriptorBufferInfo> flat_buffers = {},
                       std::span<const vk::DescriptorImageInfo> flat_images = {}) const;

    /// FAST_CPU_STAGEB_SPLIT (stage 2): BindResources
    /// in its two halves. PairResources: native stereo's pairing of the writes and barriers (the
    /// registry; resolve side) into `out`, which owns copies of every descriptor info the writes
    /// point at. EncodeResources: the barriers, push constants and descriptors from it (the
    /// recorder and the scheduler's push memo; encode side). Together they record what
    /// BindResources records.
    struct Prepared {
        DescriptorWrites writes;
        BufferBarriers barriers;
        std::vector<vk::DescriptorBufferInfo> buffers;
        std::vector<vk::DescriptorImageInfo> images;
        // Native stereo: the pairing writes into this one's own arrays, which the writes then
        // point into (no copy; made at first use). The bulk arrays: these, or buffers/images.
        std::shared_ptr<NativeStereo::Descriptors> native;
        std::span<const vk::DescriptorBufferInfo> bulk_buffers;
        std::span<const vk::DescriptorImageInfo> bulk_images;
        bool bulk{};
    };
    void PairResources(DescriptorWrites& set_writes, const BufferBarriers& buffer_barriers,
                       const NativeStereo::Handles* native_handles, EyeOverlap::Check* eye_check,
                       Prepared& out) const;
    void EncodeResources(Prepared& in, const Shader::PushData& push_data,
                         const Shader::VrPushData& vr_push) const;
    /// EncodeResources' barriers alone (they end the render pass first): FAST_CPU_STAGEB_SPLIT=2
    /// records them on stage B, which owns the pass, and leaves the rest to fast:StageB2.
    void EncodeBarriers(const BufferBarriers& barriers) const;

protected:
    // BindResources' halves: the pairing (set_writes then points into `native`, barriers into
    // `paired_barriers` or `buffer_barriers`), and the encode from writes, barriers and infos.
    void PairInto(DescriptorWrites& set_writes, const BufferBarriers& buffer_barriers,
                  const NativeStereo::Handles* native_handles, EyeOverlap::Check* eye_check,
                  NativeStereo::Descriptors& native, bool reuse_native, bool bulk,
                  BufferBarriers& paired_barriers, const BufferBarriers*& barriers) const;
    void EncodeBound(DescriptorWrites& set_writes, const BufferBarriers& barriers,
                     std::span<const vk::DescriptorBufferInfo> bulk_buffers,
                     std::span<const vk::DescriptorImageInfo> bulk_images, bool bulk,
                     const Shader::PushData& push_data, const Shader::VrPushData& vr_push,
                     const StereoSetCodec::Rights* compact = nullptr,
                     u64 compact_check = 0) const;
    // VR_COMPACT_SETS (vk_stereo_set_codec.h): BindResources for a native stereo draw whose set
    // pairs without side effects, its set written unpaired with the right eye's handles. False,
    // having recorded nothing: BindResources pairs and writes as before.
    bool BindCompactSet(DescriptorWrites& set_writes, const BufferBarriers& buffer_barriers,
                        const Shader::PushData& push_data, const Shader::VrPushData& vr_push,
                        const NativeStereo::Handles* native_handles,
                        EyeOverlap::Check* eye_check) const;
    [[nodiscard]] std::string GetDebugString() const;

    // Scratch: pipeline-owned push template and host payload; used only on the
    // existing recording thread. Destroy the template before its pipeline layout.
    // A descriptor has exactly one payload kind. Raw Vulkan C structs are trivial,
    // so assigning a union member starts its lifetime without placement construction.
    // Template offsets/strides below follow this compact layout automatically.
    union PushTemplateSlot {
        VkDescriptorBufferInfo buffer;
        VkDescriptorImageInfo image;
        VkBufferView texel;
    };
    static_assert(sizeof(PushTemplateSlot) == sizeof(VkDescriptorBufferInfo));
    bool TryPushTemplate(vk::CommandBuffer, const DescriptorWrites&) const;

    /// FAST_CPU_PUSH_TEMPLATE (vk_push_template.h): the template that pushes `writes` from FAST
    /// bit 28's two info arrays, made at the pipeline's first bulk push; null when this push
    /// takes the bulk packet (another shape, a write no entry takes, no template).
    vk::DescriptorUpdateTemplate BulkTemplateFor(vk::PipelineBindPoint bind_point,
                                                 const DescriptorWrites& writes,
                                                 std::span<const vk::DescriptorBufferInfo> buffers,
                                                 std::span<const vk::DescriptorImageInfo> images) const;
    bool MakeBulkTemplate(vk::PipelineBindPoint bind_point) const;
    /// FAST_CPU_PUSH_TEMPLATE_VERIFY: `recorded`, the payload of the template push of `writes`
    /// as the recorder encoded it from `buffers` and `images`, read through the template's slots
    /// against the writes (BindResources then pushes the bulk packet over it).
    [[gnu::noinline]] void VerifyTemplatePush(const DescriptorWrites& writes,
                                              const std::byte* recorded,
                                              std::span<const vk::DescriptorBufferInfo> buffers,
                                              std::span<const vk::DescriptorImageInfo> images) const;


    const Instance& instance;
    Scheduler& scheduler;
    DescriptorHeap& desc_heap;
    const Shader::Profile& profile;
    vk::UniquePipeline pipeline;
    vk::UniquePipelineLayout pipeline_layout;
    vk::UniqueDescriptorSetLayout desc_layout;
    std::array<const Shader::Info*, Shader::MaxStageTypes> stages{};
    mutable std::vector<u8> native_writable_bindings;
    mutable bool native_bindings_ready{};
    mutable StageWrites stage_writes{};
    mutable bool stage_writes_ready{};
    mutable u32 vr_fs_buffers{};
    mutable bool vr_fs_buffers_ready{};
    bool uses_push_descriptors{};
    bool is_compute;
    // Graphics: the layout's one push constant range is 0..push_range for AllGraphicsStageBits
    // (FAST_CPU_PUSH_SPLIT relies on it). 0 for compute.
    u32 push_range{};
    mutable vk::UniqueDescriptorUpdateTemplate push_template;
    mutable std::vector<vk::DescriptorUpdateTemplateEntry> push_template_entries;
    mutable std::vector<PushTemplateSlot> push_template_payload;
    mutable bool push_template_failed{};
    // FAST_CPU_PUSH_TEMPLATE (BulkTemplateFor; recording owner only, as BindResources): the
    // template and the slots it was made from. Declared after pipeline_layout, so destroyed
    // before it; packets in the recorder ring name both, as they name the layout alone today.
    mutable vk::UniqueDescriptorUpdateTemplate bulk_template;
    mutable std::vector<PushTemplate::Slot> bulk_template_slots;
    mutable bool bulk_template_failed{};
};

} // namespace Vulkan
