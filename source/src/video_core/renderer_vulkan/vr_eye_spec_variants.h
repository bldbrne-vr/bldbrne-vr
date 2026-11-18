// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// VR_EYE_SPEC's host side (the design: shader_recompiler/eye_spec.h).
//  - DrawMask: after a graphics draw's descriptors are paired (Pipeline::BindResources), the
//    storage buffer bindings whose two eye descriptors are equal, for the pipeline that paired
//    them. GraphicsPipeline::EyeHandle reads it right before the draw binds its pipeline.
//  - Recipe: the base pipeline's create info, deep-copied at construction (only with VR_EYE_SPEC
//    on) with every array and chained structure it points at, so a variant is built later from
//    nothing but this copy (no guest memory, no shader info). A create info holding a structure
//    the copy does not know gives no recipe: that pipeline has no variants.
//  - Slot: a variant's state and Vulkan pipeline (the base pipeline's own layout: the same
//    VkPipelineLayout, so descriptors and push constants bound for the base stay valid).
//  - Compiler: one thread building variants from (slot, recipe). Owned by the pipeline cache and
//    destroyed before the device and before any pipeline (jobs still queued then fail).

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "common/logging/log.h"
#include "common/thread.h"
#include "common/types.h"
#include "shader_recompiler/eye_spec.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::EyeSpecVariants {

/// A buffer binding whose right descriptor is its left one moved by `delta` bytes in the same
/// buffer, both `size` bytes (a camera or light pair made in one allocation).
struct Stride {
    u32 binding{};
    u32 delta{};
    u32 size{};
    vk::DescriptorBufferInfo* left{}; // the left info, widened when a variant reads both copies
};
constexpr u32 MaxStrides = 8;

struct DrawMask {
    const void* pipeline{}; // the pipeline whose pairing made the rest
    u64 shared{};           // bindings whose two descriptors are equal
    u64 strided{};          // bindings in `strides`
    std::array<Stride, MaxStrides> strides{};
    u32 stride_count{};
    vk::Pipeline handle{}; // what GraphicsPipeline::EyeSelect chose (null: the base pipeline)

    void AddStride(u32 binding, const vk::DescriptorBufferInfo& left,
                   const vk::DescriptorBufferInfo& right) {
        if (stride_count == MaxStrides || binding >= Shader::EyeSpec::MaxBindings ||
            left.buffer != right.buffer || left.range != right.range ||
            right.offset <= left.offset || right.offset - left.offset > 0xFFFF'FFFFull ||
            left.range == 0 || left.range > 0xFFFF'FFFFull || ((right.offset - left.offset) & 15)) {
            return;
        }
        strides[stride_count++] = {binding, u32(right.offset - left.offset), u32(left.range),
                                   const_cast<vk::DescriptorBufferInfo*>(&left)};
        strided |= u64{1} << binding;
    }
    const Stride* StrideOf(u32 binding) const {
        for (u32 i = 0; i < stride_count; ++i) {
            if (strides[i].binding == binding) {
                return &strides[i];
            }
        }
        return nullptr;
    }
};
inline thread_local DrawMask g_draw_mask;

/// The pipeline a draw binds after its own descriptors were bound (Pipeline::BindResources): the
/// variant EyeSelect chose for this pipeline, else `base`. Every bind of the same pipeline over
/// those descriptors asks here (a strided binding's widened descriptor is the variant's).
template <class P>
inline vk::Pipeline Bound(const P* pipeline, const void* as_base) {
    const DrawMask& drawn = g_draw_mask;
    return drawn.pipeline == as_base && drawn.handle ? drawn.handle : pipeline->Handle();
}

/// Draws a pipeline learns its shared bindings over before its variant is asked for.
constexpr u32 LearnDraws = 16;
/// FAST_TOGGLE_FILE's A/B bit (FastRender::AbOn) for using built variants: cleared, every draw
/// binds its base pipeline (learning and building go on), for an in-run A/B.
constexpr unsigned UseBit = 16;

enum class State : u32 { Compiling, Ready, Failed };

struct Slot {
    std::atomic<State> state{State::Compiling};
    u64 mask{};    // every binding it reads through descriptor 0
    u64 strided{}; // those of them read with a stride (`strides`; the rest: equal descriptors)
    std::vector<Stride> strides;
    vk::UniquePipeline pipeline;
};

/// What a pipeline's first draws had in common (EyeSelect's learning).
struct Learn {
    u64 shared{~u64{0}};
    u64 strided{~u64{0}};
    std::array<Stride, Shader::EyeSpec::MaxBindings> strides{}; // by binding, from the first draw
    bool first{true};
};

struct Stats {
    std::atomic<u64> variant{}, variant_strided{}, learning{}, compiling{}, mismatch{}, no_mask{},
        none{};
    std::atomic<u64> requested{}, built{}, failed{}, no_recipe{};
    std::atomic<s64> last_log{};
};
inline Stats g_stats;

/// A copy of a graphics pipeline's create info that owns everything it points at.
struct Recipe {
    vk::Device device;
    vk::PipelineCache cache;
    vk::GraphicsPipelineCreateInfo info;
    std::vector<vk::PipelineShaderStageCreateInfo> stages;
    vk::PipelineVertexInputStateCreateInfo vertex_input;
    vk::PipelineVertexInputDivisorStateCreateInfo divisor;
    std::vector<vk::VertexInputBindingDescription> vertex_bindings;
    std::vector<vk::VertexInputAttributeDescription> vertex_attributes;
    std::vector<vk::VertexInputBindingDivisorDescriptionEXT> divisors;
    vk::PipelineInputAssemblyStateCreateInfo input_assembly;
    vk::PipelineTessellationStateCreateInfo tessellation;
    vk::PipelineViewportStateCreateInfo viewport;
    vk::PipelineViewportDepthClipControlCreateInfoEXT clip_control;
    vk::PipelineRasterizationStateCreateInfo raster;
    vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT provoking;
    vk::PipelineRasterizationDepthClipStateCreateInfoEXT depth_clip;
    vk::PipelineMultisampleStateCreateInfo multisample;
    vk::PipelineDepthStencilStateCreateInfo depth_stencil;
    vk::PipelineColorBlendStateCreateInfo blend;
    std::vector<vk::PipelineColorBlendAttachmentState> blend_attachments;
    vk::PipelineDynamicStateCreateInfo dynamic;
    std::vector<vk::DynamicState> dynamic_states;
    vk::PipelineFragmentShadingRateStateCreateInfoKHR shading_rate;
    vk::PipelineRenderingCreateInfo rendering;
    std::vector<vk::Format> color_formats;
    vk::AttachmentSampleCountInfoAMD samples;
    std::vector<vk::SampleCountFlagBits> color_samples;

    Recipe() = default;
    Recipe(const Recipe&) = delete;
    Recipe& operator=(const Recipe&) = delete;
};

/// The recipe of `ci`, or null when it holds something this copy does not know.
inline std::shared_ptr<const Recipe> CopyRecipe(const vk::GraphicsPipelineCreateInfo& ci,
                                                vk::Device device, vk::PipelineCache cache) {
    auto r = std::make_shared<Recipe>();
    r->device = device;
    r->cache = cache;
    r->info = ci;
    for (u32 i = 0; i < ci.stageCount; ++i) {
        if (ci.pStages[i].pNext || ci.pStages[i].pSpecializationInfo) {
            return nullptr;
        }
        r->stages.push_back(ci.pStages[i]);
    }
    r->info.pStages = r->stages.data();
    if (ci.pVertexInputState) {
        const auto& v = *ci.pVertexInputState;
        r->vertex_input = v;
        r->vertex_bindings.assign(v.pVertexBindingDescriptions,
                                  v.pVertexBindingDescriptions + v.vertexBindingDescriptionCount);
        r->vertex_attributes.assign(v.pVertexAttributeDescriptions,
                                    v.pVertexAttributeDescriptions +
                                        v.vertexAttributeDescriptionCount);
        r->vertex_input.pVertexBindingDescriptions = r->vertex_bindings.data();
        r->vertex_input.pVertexAttributeDescriptions = r->vertex_attributes.data();
        r->vertex_input.pNext = nullptr;
        if (v.pNext) {
            const auto* d = static_cast<const vk::PipelineVertexInputDivisorStateCreateInfo*>(v.pNext);
            if (d->sType != vk::StructureType::ePipelineVertexInputDivisorStateCreateInfo ||
                d->pNext) {
                return nullptr;
            }
            r->divisor = *d;
            r->divisors.assign(d->pVertexBindingDivisors,
                               d->pVertexBindingDivisors + d->vertexBindingDivisorCount);
            r->divisor.pVertexBindingDivisors = r->divisors.data();
            r->vertex_input.pNext = &r->divisor;
        }
        r->info.pVertexInputState = &r->vertex_input;
    }
    if (!ci.pInputAssemblyState || ci.pInputAssemblyState->pNext || !ci.pTessellationState ||
        ci.pTessellationState->pNext || !ci.pViewportState || !ci.pRasterizationState ||
        !ci.pMultisampleState || ci.pMultisampleState->pNext ||
        ci.pMultisampleState->pSampleMask || !ci.pColorBlendState ||
        ci.pColorBlendState->pNext || !ci.pDynamicState || ci.pDynamicState->pNext ||
        (ci.pDepthStencilState && ci.pDepthStencilState->pNext)) {
        return nullptr;
    }
    r->input_assembly = *ci.pInputAssemblyState;
    r->info.pInputAssemblyState = &r->input_assembly;
    r->tessellation = *ci.pTessellationState;
    r->info.pTessellationState = &r->tessellation;
    r->viewport = *ci.pViewportState;
    if (r->viewport.pViewports || r->viewport.pScissors) {
        return nullptr; // dynamic with count: none
    }
    if (ci.pViewportState->pNext) {
        const auto* c = static_cast<const vk::PipelineViewportDepthClipControlCreateInfoEXT*>(
            ci.pViewportState->pNext);
        if (c->sType != vk::StructureType::ePipelineViewportDepthClipControlCreateInfoEXT ||
            c->pNext) {
            return nullptr;
        }
        r->clip_control = *c;
        r->viewport.pNext = &r->clip_control;
    }
    r->info.pViewportState = &r->viewport;
    r->raster = *ci.pRasterizationState;
    r->raster.pNext = nullptr;
    const void** tail = const_cast<const void**>(&r->raster.pNext);
    for (const auto* p = static_cast<const vk::BaseInStructure*>(ci.pRasterizationState->pNext); p;
         p = p->pNext) {
        if (p->sType == vk::StructureType::ePipelineRasterizationProvokingVertexStateCreateInfoEXT) {
            r->provoking =
                *reinterpret_cast<const vk::PipelineRasterizationProvokingVertexStateCreateInfoEXT*>(p);
            r->provoking.pNext = nullptr;
            *tail = &r->provoking;
            tail = const_cast<const void**>(&r->provoking.pNext);
        } else if (p->sType ==
                   vk::StructureType::ePipelineRasterizationDepthClipStateCreateInfoEXT) {
            r->depth_clip =
                *reinterpret_cast<const vk::PipelineRasterizationDepthClipStateCreateInfoEXT*>(p);
            r->depth_clip.pNext = nullptr;
            *tail = &r->depth_clip;
            tail = const_cast<const void**>(&r->depth_clip.pNext);
        } else {
            return nullptr;
        }
    }
    r->info.pRasterizationState = &r->raster;
    r->multisample = *ci.pMultisampleState;
    r->info.pMultisampleState = &r->multisample;
    if (ci.pDepthStencilState) {
        r->depth_stencil = *ci.pDepthStencilState;
        r->info.pDepthStencilState = &r->depth_stencil;
    }
    r->blend = *ci.pColorBlendState;
    r->blend_attachments.assign(ci.pColorBlendState->pAttachments,
                                ci.pColorBlendState->pAttachments +
                                    ci.pColorBlendState->attachmentCount);
    r->blend.pAttachments = r->blend_attachments.data();
    r->info.pColorBlendState = &r->blend;
    r->dynamic = *ci.pDynamicState;
    r->dynamic_states.assign(ci.pDynamicState->pDynamicStates,
                             ci.pDynamicState->pDynamicStates +
                                 ci.pDynamicState->dynamicStateCount);
    r->dynamic.pDynamicStates = r->dynamic_states.data();
    r->info.pDynamicState = &r->dynamic;
    // The create info's own chain: the shading rate state, the rendering info, its samples.
    r->info.pNext = nullptr;
    tail = const_cast<const void**>(&r->info.pNext);
    for (const auto* p = static_cast<const vk::BaseInStructure*>(ci.pNext); p; p = p->pNext) {
        if (p->sType == vk::StructureType::ePipelineFragmentShadingRateStateCreateInfoKHR) {
            r->shading_rate =
                *reinterpret_cast<const vk::PipelineFragmentShadingRateStateCreateInfoKHR*>(p);
            r->shading_rate.pNext = nullptr;
            *tail = &r->shading_rate;
            tail = const_cast<const void**>(&r->shading_rate.pNext);
        } else if (p->sType == vk::StructureType::ePipelineRenderingCreateInfo) {
            const auto& src = *reinterpret_cast<const vk::PipelineRenderingCreateInfo*>(p);
            r->rendering = src;
            r->rendering.pNext = nullptr;
            r->color_formats.assign(src.pColorAttachmentFormats,
                                    src.pColorAttachmentFormats + src.colorAttachmentCount);
            r->rendering.pColorAttachmentFormats = r->color_formats.data();
            *tail = &r->rendering;
            tail = const_cast<const void**>(&r->rendering.pNext);
        } else if (p->sType == vk::StructureType::eAttachmentSampleCountInfoAMD) {
            const auto& src = *reinterpret_cast<const vk::AttachmentSampleCountInfoAMD*>(p);
            r->samples = src;
            r->samples.pNext = nullptr;
            r->color_samples.assign(src.pColorAttachmentSamples,
                                    src.pColorAttachmentSamples + src.colorAttachmentCount);
            r->samples.pColorAttachmentSamples = r->color_samples.data();
            *tail = &r->samples;
            tail = const_cast<const void**>(&r->samples.pNext);
        } else {
            return nullptr;
        }
    }
    return r;
}

/// Logs every 5 s.
inline void MaybeLog() {
    auto& s = g_stats;
    constexpr auto relaxed = std::memory_order_relaxed;
    const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 last = s.last_log.load(relaxed);
    if (now - last < 5000 || !s.last_log.compare_exchange_strong(last, now, relaxed)) {
        return;
    }
    LOG_INFO(Render_Vulkan,
             "VR eye spec: draws variant={} (with strides {}) learning={} compiling={} "
             "mask_mismatch={} no_mask={} nothing_shared={} | variants requested={} built={} "
             "failed={} no_recipe={}",
             s.variant.load(relaxed), s.variant_strided.load(relaxed), s.learning.load(relaxed),
             s.compiling.load(relaxed),
             s.mismatch.load(relaxed), s.no_mask.load(relaxed), s.none.load(relaxed),
             s.requested.load(relaxed), s.built.load(relaxed), s.failed.load(relaxed),
             s.no_recipe.load(relaxed));
}

class Compiler {
public:
    Compiler() : thread{[this] { Run(); }} {}
    ~Compiler() {
        {
            std::scoped_lock lock{mutex};
            stop = true;
        }
        cv.notify_one();
        thread.join();
        for (auto& job : jobs) {
            job.slot->state.store(State::Failed, std::memory_order_release);
        }
    }
    Compiler(const Compiler&) = delete;
    Compiler& operator=(const Compiler&) = delete;

    void Submit(std::shared_ptr<Slot> slot, std::shared_ptr<const Recipe> recipe) {
        {
            std::scoped_lock lock{mutex};
            jobs.push_back({std::move(slot), std::move(recipe)});
        }
        cv.notify_one();
    }

    /// Waits until no job is queued or running (before shader modules are replaced).
    void Drain() {
        std::unique_lock lock{mutex};
        idle_cv.wait(lock, [this] { return jobs.empty() && !busy; });
    }

private:
    struct Job {
        std::shared_ptr<Slot> slot;
        std::shared_ptr<const Recipe> recipe;
    };

    void Run() {
        Common::SetCurrentThreadName("VrEyeSpec");
        for (;;) {
            Job job;
            {
                std::unique_lock lock{mutex};
                cv.wait(lock, [this] { return stop || !jobs.empty(); });
                if (stop) {
                    return;
                }
                job = std::move(jobs.front());
                jobs.pop_front();
                busy = true;
            }
            Build(job);
            {
                std::scoped_lock lock{mutex};
                busy = false;
            }
            idle_cv.notify_all();
        }
    }

    static void Build(const Job& job) {
        const Recipe& r = *job.recipe;
        // Per binding it shares: the flag 1, and a stride and size (0 and 0 for equal
        // descriptors; ids a module does not have are ignored).
        std::array<u32, 3 * Shader::EyeSpec::MaxBindings> values{};
        std::array<vk::SpecializationMapEntry, 3 * Shader::EyeSpec::MaxBindings> entries{};
        u32 count = 0;
        const auto add = [&](u32 id, u32 value) {
            values[count] = value;
            entries[count] = {id, count * u32(sizeof(u32)), sizeof(u32)};
            ++count;
        };
        for (u32 b = 0; b < Shader::EyeSpec::MaxBindings; ++b) {
            if (((job.slot->mask >> b) & 1) == 0) {
                continue;
            }
            u32 delta = 0, size = 0;
            for (const auto& s : job.slot->strides) {
                if (s.binding == b) {
                    delta = s.delta;
                    size = s.size;
                }
            }
            add(Shader::EyeSpec::SpecBase + b, 1);
            add(Shader::EyeSpec::SpecBaseDelta + b, delta);
            add(Shader::EyeSpec::SpecBaseSize + b, size);
        }
        const vk::SpecializationInfo spec = {
            .mapEntryCount = count,
            .pMapEntries = entries.data(),
            .dataSize = count * sizeof(u32),
            .pData = values.data(),
        };
        std::vector<vk::PipelineShaderStageCreateInfo> stages = r.stages;
        for (auto& stage : stages) {
            stage.pSpecializationInfo = &spec;
        }
        vk::GraphicsPipelineCreateInfo ci = r.info;
        ci.pStages = stages.data();
        auto [result, pipeline] = r.device.createGraphicsPipelineUnique(r.cache, ci);
        if (result != vk::Result::eSuccess) {
            LOG_WARNING(Render_Vulkan, "VR eye spec: variant pipeline failed: {}",
                        vk::to_string(result));
            g_stats.failed.fetch_add(1, std::memory_order_relaxed);
            job.slot->state.store(State::Failed, std::memory_order_release);
            return;
        }
        job.slot->pipeline = std::move(pipeline);
        g_stats.built.fetch_add(1, std::memory_order_relaxed);
        job.slot->state.store(State::Ready, std::memory_order_release);
    }

    std::mutex mutex;
    std::condition_variable cv, idle_cv;
    std::deque<Job> jobs;
    bool stop{}, busy{};
    std::thread thread;
};

/// The pipeline cache's compiler (null when VR_EYE_SPEC is off or the cache is gone).
inline std::atomic<Compiler*> g_compiler{nullptr};

} // namespace Vulkan::EyeSpecVariants
