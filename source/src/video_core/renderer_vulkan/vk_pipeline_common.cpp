#include "video_core/native_stereo/profile_scopes.h"
#include "video_core/native_stereo/push_state_memo.h"
#include "video_core/fast/b_handler_profile.h"
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/native_stereo/native_stereo.h"
#include <boost/container/static_vector.hpp>
#include <cstring>
#include <bit>

#include "common/hash.h"
#include "shader_recompiler/resource.h"
#include "video_core/fast_render.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_dispatch_eye_overlap.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pass_breaks.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {
// BindResources' switches, read once at start-up: a plain load per draw where the function-local
// statics paid a guard (a thread-local read) on every call.
const bool g_reuse_native_descriptors = [] {
    const char* setting = std::getenv("VR_NATIVE_REUSE_DESCRIPTORS");
    return !setting || setting[0] != '0';
}();
const bool g_swap_native_writes = [] {
    const char* setting = std::getenv("VR_NATIVE_SWAP_WRITES");
    return !setting || setting[0] != '0';
}();
const bool g_submission_audit = std::getenv("VR_SUBMISSION_AUDIT") != nullptr;
const bool g_push_template = std::getenv("VR_PUSH_TEMPLATE") != nullptr;
// FAST_CPU_PUSH_TEMPLATE (on unless '0'): a native stereo bulk push (FAST bit 28) goes through the
// pipeline's descriptor update template, its packet the two info arrays without the writes
// (vk_push_template.h, BulkTemplateFor). The descriptors set are the same.
const bool g_push_template_bulk = [] {
    const char* value = std::getenv("FAST_CPU_PUSH_TEMPLATE");
    return !value || value[0] != '0';
}();
// FAST_CPU_PUSH_TEMPLATE_VERIFY=1: every push the template takes is recorded through it (the
// template's entries, its packet and the replay's vkCmdPushDescriptorSetWithTemplateKHR all run,
// so VK_LAYER_KHRONOS_validation checks them), the payload as the recorder encoded it is read
// back through the template's slots, every descriptor compared with its write's (LOG_ERROR on a
// mismatch), and then the bulk packet is pushed as before over it: its descriptors are the ones
// drawn with (the old answer). A driver that reads a valid template wrongly shows only in the
// pixel A/B (FAST_CPU_PUSH_TEMPLATE=0 against the default), the check this switch needs.
const bool g_push_template_verify = [] {
    const char* value = std::getenv("FAST_CPU_PUSH_TEMPLATE_VERIFY");
    return value && value[0] == '1';
}();
// FAST_CPU_PUSH_TEMPLATE's line (the recording owner's, as BindResources): bulk pushes offered,
// pushed through a template, templates made, pipelines left on the bulk packet (a write no entry
// takes, or no template), pushes of another shape, pushes recorded inline, verified, mismatches.
struct PushTemplateStats {
    u64 calls, pushes, made, unsupported, other_shape, inline_pushes, verified, mismatches;
};
PushTemplateStats g_push_template_stats{};
static_assert(sizeof(vk::DescriptorBufferInfo) == PushTemplate::InfoBytes &&
              sizeof(vk::DescriptorImageInfo) == PushTemplate::InfoBytes);
// The info array a bulk write points into (the recorder's BulkInfoOf, without the dynamic and
// input-attachment types no push layout here has; texel views stay with the bulk packet).
struct BulkKindOf {
    PushTemplate::Kind operator()(vk::DescriptorType type) const noexcept {
        switch (type) {
        case vk::DescriptorType::eUniformBuffer:
        case vk::DescriptorType::eStorageBuffer:
            return PushTemplate::Kind::Buffer;
        case vk::DescriptorType::eSampler:
        case vk::DescriptorType::eCombinedImageSampler:
        case vk::DescriptorType::eSampledImage:
        case vk::DescriptorType::eStorageImage:
            return PushTemplate::Kind::Image;
        default:
            return PushTemplate::Kind::Other;
        }
    }
};
// FAST_CPU_DRIVER_CALLS (on unless '0'): a layout without push descriptors (native stereo doubles
// the image descriptors: 190 of 737 layouts are past NVIDIA's 32, 36-66 elements) has its set
// committed, written and bound by the recorder's replay (RecorderProxy::writeDescriptorSet). Its
// vkUpdateDescriptorSets (3.9%) and vkAllocateDescriptorSets (1.9%) were nearly all of Stage B's
// 6.1% inside the NVIDIA driver (2026-10-14 sampled profile, callers by return address). The
// set receives the same descriptors and is bound at the same place in the command stream.
// FAST_CPU_DRIVER_CALLS_VERIFY=1: vk_recorder.cpp.
const bool g_driver_calls = [] {
    const char* value = std::getenv("FAST_CPU_DRIVER_CALLS");
    return !value || value[0] != '0';
}();
} // namespace

u64 Pipeline::GuestShaderKey(Shader::LogicalStage stage) const noexcept {
    const auto* info = stages[u32(stage)];
    return info ? HashCombine(info->pgm_hash, u64{0}) : 0;
}

Pipeline::Pipeline(const Instance& instance_, Scheduler& scheduler_, DescriptorHeap& desc_heap_,
                   const Shader::Profile& profile_, vk::PipelineCache pipeline_cache,
                   bool is_compute_ /*= false*/)
    : instance{instance_}, scheduler{scheduler_}, desc_heap{desc_heap_}, profile{profile_},
      is_compute{is_compute_} {}

Pipeline::~Pipeline() { ScratchPush::Invalidate(); }

std::span<const u8> Pipeline::NativeWritableBindings() const {
    // Pipeline resource layouts are fixed after construction. Keep metadata owned by the
    // pipeline so destruction/recreation cannot reuse stale pointer-keyed entries.
    if (!native_bindings_ready) {
        for (const auto* stage : GetStages()) {
            if (!stage) continue;
            for (const auto& buffer : stage->buffers)
                native_writable_bindings.push_back(buffer.is_written);
            for (const auto& image : stage->images)
                native_writable_bindings.resize(native_writable_bindings.size() + image.NumBindings(*stage));
            native_writable_bindings.resize(native_writable_bindings.size() + stage->samplers.size());
        }
        native_bindings_ready = true;
    }
    return native_writable_bindings;
}

void Pipeline::BindResources(DescriptorWrites& set_writes, const BufferBarriers& buffer_barriers,
                             const Shader::PushData& push_data, const Shader::VrPushData& vr_push,
                             const NativeStereo::Handles* native_handles,
                             EyeOverlap::Check* eye_check) const {
    FastRender::BProfile::Scope b_profile{FastRender::BProfile::Recording};
    ScratchProfile::Cpu scratch_cpu(12,"Pipeline::BindResources");
    const bool reuse_native = g_reuse_native_descriptors;
    NativeStereo::Descriptors temporary_native;
    // Vulkan consumes descriptor host arrays during these calls. No array pointer is
    // retained across BindResources calls; each recording thread has its own storage.
    thread_local NativeStereo::Descriptors reusable_native;
    auto& native = reuse_native ? reusable_native : temporary_native;
    BufferBarriers paired_barriers;
    const BufferBarriers* barriers = &buffer_barriers;
    // FAST bit 28: pairing leaves every write inside native.buffers/images, and the recorder
    // copies the writes and both arrays in three blocks (bbport vk_pipeline_common.cpp:49-90).
    // Read once so pairing and encoding agree when the toggle flips.
    static constexpr auto PushDescBulk = static_cast<FastRender::Bit>(28);
    const bool bulk =
        NativeStereo::Enabled() && uses_push_descriptors && FastRender::On(PushDescBulk);
    if (NativeStereo::Enabled()) {
        // FAST_CPU_FUSED_PAIR: from what the binding carried, the registry lock only for what it
        // did not; the registry pass when the registry changed since binding.
        if (!native_handles || !native_handles->fused ||
            !NativeStereo::PairCarried(scheduler, *this, set_writes, native, reuse_native,
                                       *native_handles, bulk)) {
            NativeStereo::PairDescriptors(scheduler, *this, set_writes, native, reuse_native,
                                          native_handles, bulk);
        }
        if (g_swap_native_writes) {
            // Keep the caller's paired writes (including dstSet) observable. Only the
            // write-vector allocation moves; pointed-to image/buffer arrays stay owned
            // by native until the next call, exactly as in the assign path.
            set_writes.swap(native.writes);
        } else {
            set_writes.assign(native.writes.begin(), native.writes.end());
        }
        // FAST bit 53: the paired writes point into `native`, alive only during this call.
        if (eye_check) {
            EyeOverlap::Classify(NativeWritableBindings(), set_writes, *eye_check);
        }
        paired_barriers = buffer_barriers;
        if (native_handles) {
            // Bit 25: one registry lock for all barriers, entries carried from BindBuffers.
            if (!buffer_barriers.empty()) {
                boost::container::small_vector<vk::Buffer, 16> rights(buffer_barriers.size());
                NativeStereo::PairBarriers({buffer_barriers.data(), buffer_barriers.size()},
                                           *native_handles, {rights.data(), rights.size()});
                for (size_t i = 0; i < rights.size(); ++i) if (rights[i]) {
                    auto b = buffer_barriers[i]; b.buffer = rights[i]; paired_barriers.push_back(b);
                }
            }
        } else {
            for (auto b : buffer_barriers) if (auto right = NativeStereo::ExistingRight(b.buffer)) {
                b.buffer=right; paired_barriers.push_back(b);
            }
        }
        barriers=&paired_barriers;
    }
    const auto cmdbuf = scheduler.Commands();
    const auto bind_point =
        IsCompute() ? vk::PipelineBindPoint::eCompute : vk::PipelineBindPoint::eGraphics;

    if (!barriers->empty()) {
        const auto dependencies = vk::DependencyInfo{
            .dependencyFlags = vk::DependencyFlagBits::eByRegion,
            .bufferMemoryBarrierCount = u32(barriers->size()),
            .pBufferMemoryBarriers = barriers->data(),
        };
        if (PassBreaks::StatsEnabled() && scheduler.IsRendering()) [[unlikely]] {
            // FAST_PASS_BREAK_STATS: would bit 43 part 4 have kept this pass (only read -> read)?
            bool read_after_read = true;
            for (const auto& barrier : *barriers) {
                read_after_read = read_after_read && PassBreaks::IsPureRead(barrier.srcAccessMask) &&
                                  PassBreaks::IsPureRead(barrier.dstAccessMask);
            }
            scheduler.NoteBarrierBreak(read_after_read);
        }
        scheduler.EndRendering();
        cmdbuf.pipelineBarrier2(dependencies);
    }

    const auto stage_flags = IsCompute() ? vk::ShaderStageFlagBits::eCompute : AllGraphicsStageBits;
    if (g_submission_audit) {
        struct Audit {
            const Scheduler* owner{};
            vk::CommandBuffer command{};
            vk::PipelineLayout layout{};
            vk::ShaderStageFlags stages{};
            u64 tick{}, calls{}, identical{}, barrier_calls{}, barriers{}, writes{};
            Shader::PushData previous{};
            bool valid{};
        };
        static thread_local Audit audit;
        ++audit.calls;
        if (!barriers->empty()) ++audit.barrier_calls;
        audit.barriers += barriers->size();
        audit.writes += set_writes.size();
        if (audit.valid && audit.owner == &scheduler && audit.tick == scheduler.CurrentTick() &&
            audit.tick == scheduler.CurrentTick() && audit.layout == *pipeline_layout &&
            audit.stages == stage_flags &&
            std::memcmp(&audit.previous, &push_data, sizeof(push_data)) == 0) ++audit.identical;
        audit.owner = &scheduler; audit.command = vk::CommandBuffer{}; audit.tick = scheduler.CurrentTick();
        audit.layout = *pipeline_layout; audit.stages = stage_flags;
        audit.previous = push_data; audit.valid = true;
        if (audit.calls % 10000 == 0) {
            LOG_INFO(Render_Vulkan, "VR submission audit: calls {}, identical push candidates {}, barrier calls {}, barriers {}, writes {}",
                     audit.calls, audit.identical, audit.barrier_calls, audit.barriers, audit.writes);
        }
    }
    // One upload and one memo key for guest constants plus native raster scale (VrPushData: and
    // the draw's projection, light tiles, cut and motion buffers; the pipeline layout's whole
    // range, so the cut pipeline VrDrawCapeSplit binds later in the draw reads them too; the memo
    // compares every uploaded byte, a new motion-buffer address included). Keep the original
    // 128-byte path for compute and devices without the extension.
    static_assert(sizeof(ScratchPush::Memo::bytes) >= Shader::VrPushData::End);
    std::array<std::byte, Shader::VrPushData::End> scaled_push;
    const void* push_bytes = &push_data;
    u32 push_size = sizeof(push_data);
    if (!IsCompute() && NativeStereo::Enabled() && profile.max_push_constant_size >= 136) {
        static_assert(sizeof(push_data) == Shader::VrPushData::Begin);
        push_size = profile.SupportsVrPushData() ? Shader::VrPushData::End : 136u;
        std::memcpy(scaled_push.data(), &push_data, sizeof(push_data));
        std::memcpy(scaled_push.data() + sizeof(push_data), &vr_push,
                    push_size - sizeof(push_data)); // scene_inverse comes first
        push_bytes = scaled_push.data();
    }
    bool skip_push = false;
    if (ScratchPush::Enabled()) {
        static thread_local ScratchPush::Memo memo;
        const ScratchPush::Memo::Key key{
            reinterpret_cast<std::uintptr_t>(&scheduler),
            scheduler.CurrentTick(),
            std::bit_cast<u64>(VkPipelineLayout(*pipeline_layout)),
            scheduler.CurrentTick(), static_cast<u32>(stage_flags)};
        skip_push = ScratchPush::OwnedEnabled()
            ? scheduler.PushStateMemo().PrepareOwned(key, push_bytes, push_size)
            : memo.Prepare(key, push_bytes, push_size);
        static const bool stats = std::getenv("VR_PERF_CHECK") || std::getenv("VR_PERF_STATS");
        static thread_local u64 calls{}, skipped{};
        if (stats) {
            ++calls; skipped += skip_push;
            if (calls % 10000 == 0)
                LOG_INFO(Render_Vulkan, "VR push reuse: {} calls, {} skipped", calls, skipped);
        }
    }
    if (!skip_push) {
        // FAST_CPU_PUSH_SPLIT: every native graphics layout has this one range, so the recorder
        // leaves out the VR block (or the whole push) the command buffer already holds.
        if (push_size == Shader::VrPushData::End && push_range == push_size) {
            cmdbuf.pushConstantsSplit(*pipeline_layout, stage_flags, Shader::VrPushData::Begin,
                                      push_size, push_bytes);
        } else {
            cmdbuf.pushConstants(*pipeline_layout, stage_flags, 0u, push_size, push_bytes);
        }
    }


    // Bind descriptor set.
    if (set_writes.empty()) {
        return;
    }

    if (uses_push_descriptors) {
        if (g_push_template && TryPushTemplate(cmdbuf, set_writes)) return;
        if (bulk) {
            const std::span<const vk::DescriptorBufferInfo> buffers{native.buffers.data(),
                                                                    native.buffers_used};
            const std::span<const vk::DescriptorImageInfo> images{native.images.data(),
                                                                  native.images_used};
            // FAST_CPU_PUSH_TEMPLATE: the same descriptors from the two arrays alone.
            if (g_push_template_bulk) {
                if (const auto update_template =
                        BulkTemplateFor(bind_point, set_writes, buffers, images)) {
                    const std::byte* recorded = nullptr;
                    const bool pushed = cmdbuf.pushDescriptorSetTemplate(
                        bind_point, *pipeline_layout, 0, update_template, buffers, images,
                        g_push_template_verify ? &recorded : nullptr);
                    if constexpr (Common::Log::Enabled) {
                        ++(pushed ? g_push_template_stats.pushes
                                  : g_push_template_stats.inline_pushes);
                    }
                    if (pushed) {
                        if (!g_push_template_verify) [[likely]] {
                            return;
                        }
                        // FAST_CPU_PUSH_TEMPLATE_VERIFY: the recorded payload checked; the bulk
                        // packet below, pushed after it, decides the descriptors.
                        VerifyTemplatePush(set_writes, recorded, buffers, images);
                    }
                }
            }
            cmdbuf.pushDescriptorSetBulk(bind_point, *pipeline_layout, 0, set_writes, buffers,
                                         images);
            return;
        }
        cmdbuf.pushDescriptorSetKHR(bind_point, *pipeline_layout, 0, set_writes);
        return;
    }

    if (g_driver_calls) {
        cmdbuf.writeDescriptorSet(desc_heap, *desc_layout, bind_point, *pipeline_layout, 0,
                                  set_writes);
        return;
    }
    const auto desc_set = desc_heap.Commit(*desc_layout);
    for (auto& set_write : set_writes) {
        set_write.dstSet = desc_set;
    }
    instance.GetDevice().updateDescriptorSets(set_writes, {});
    cmdbuf.bindDescriptorSets(bind_point, *pipeline_layout, 0, desc_set, {});
}

// Keep this out of BindResources' hot body. Unsupported or changing shapes use
// the established push-write path; a template never outlives its owning pipeline.
[[gnu::noinline]] bool Pipeline::TryPushTemplate(vk::CommandBuffer cmdbuf,
                                               const DescriptorWrites& writes) const {
    if (!g_push_template || !NativeStereo::Enabled() || push_template_failed) return false;
    if (!push_template) {
        size_t slots = 0;
        for (const auto& w : writes) {
            if (w.pNext || !w.descriptorCount) { push_template_failed = true; return false; }
            size_t member;
            switch (w.descriptorType) {
            case vk::DescriptorType::eUniformBuffer:
            case vk::DescriptorType::eStorageBuffer:
                member = offsetof(PushTemplateSlot, buffer); break;
            case vk::DescriptorType::eSampler:
            case vk::DescriptorType::eCombinedImageSampler:
            case vk::DescriptorType::eSampledImage:
            case vk::DescriptorType::eStorageImage:
                member = offsetof(PushTemplateSlot, image); break;
            case vk::DescriptorType::eUniformTexelBuffer:
            case vk::DescriptorType::eStorageTexelBuffer:
                member = offsetof(PushTemplateSlot, texel); break;
            default: push_template_failed = true; return false;
            }
            push_template_entries.push_back({.dstBinding=w.dstBinding,
                .dstArrayElement=w.dstArrayElement, .descriptorCount=w.descriptorCount,
                .descriptorType=w.descriptorType,
                .offset=slots*sizeof(PushTemplateSlot)+member, .stride=sizeof(PushTemplateSlot)});
            slots += w.descriptorCount;
        }
        push_template_payload.resize(slots);
        const vk::DescriptorUpdateTemplateCreateInfo info{
            .descriptorUpdateEntryCount=u32(push_template_entries.size()),
            .pDescriptorUpdateEntries=push_template_entries.data(),
            .templateType=vk::DescriptorUpdateTemplateType::ePushDescriptors,
            .pipelineBindPoint=IsCompute()?vk::PipelineBindPoint::eCompute:vk::PipelineBindPoint::eGraphics,
            .pipelineLayout=*pipeline_layout, .set=0};
        auto result = instance.GetDevice().createDescriptorUpdateTemplateUnique(info);
        if (result.result != vk::Result::eSuccess) {
            push_template_failed = true;
            LOG_WARNING(Render_Vulkan, "Scratch push template creation failed: {}", vk::to_string(result.result));
            return false;
        }
        push_template = std::move(result.value);
        LOG_INFO(Render_Vulkan, "Scratch push template created: {} writes, {} descriptors",
                 writes.size(), slots);
    }
    if (writes.size() != push_template_entries.size()) return false;
    size_t index = 0;
    for (size_t i=0; i<writes.size(); ++i) {
        const auto& w = writes[i]; const auto& entry = push_template_entries[i];
        if (w.pNext || w.dstBinding != entry.dstBinding || w.dstArrayElement != entry.dstArrayElement ||
            w.descriptorCount != entry.descriptorCount || w.descriptorType != entry.descriptorType)
            return false;
        for (u32 j=0; j<w.descriptorCount; ++j) {
            auto& slot = push_template_payload[index++];
            switch (w.descriptorType) {
            case vk::DescriptorType::eUniformBuffer:
            case vk::DescriptorType::eStorageBuffer: slot.buffer=static_cast<const VkDescriptorBufferInfo&>(w.pBufferInfo[j]); break;
            case vk::DescriptorType::eUniformTexelBuffer:
            case vk::DescriptorType::eStorageTexelBuffer: slot.texel=VkBufferView(w.pTexelBufferView[j]); break;
            default: slot.image=static_cast<const VkDescriptorImageInfo&>(w.pImageInfo[j]); break;
            }
        }
    }
    // Use the raw dispatcher explicitly: the enhanced typed-pointer overload
    // takes &data, which would pass a pointer-to-pointer instead of this payload.
    VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPushDescriptorSetWithTemplateKHR(
        VkCommandBuffer(cmdbuf), VkDescriptorUpdateTemplate(*push_template),
        VkPipelineLayout(*pipeline_layout), 0, push_template_payload.data());
    return true;
}

vk::DescriptorUpdateTemplate Pipeline::BulkTemplateFor(
    vk::PipelineBindPoint bind_point, const DescriptorWrites& writes,
    std::span<const vk::DescriptorBufferInfo> buffers,
    std::span<const vk::DescriptorImageInfo> images) const {
    auto& stats = g_push_template_stats;
    if constexpr (Common::Log::Enabled) {
        if ((++stats.calls & 0xFFFF) == 0) {
            LOG_INFO(Render_Vulkan,
                     "FAST_CPU_PUSH_TEMPLATE: bulk pushes={} templated={} templates={} "
                     "unsupported={} other_shape={} inline={} verified={} mismatches={}",
                     stats.calls, stats.pushes, stats.made, stats.unsupported, stats.other_shape,
                     stats.inline_pushes, stats.verified, stats.mismatches);
        }
    }
    if (bulk_template_failed) {
        return {};
    }
    const std::span<const vk::WriteDescriptorSet> view{writes.data(), writes.size()};
    if (!bulk_template) {
        // The first bulk push of this pipeline: its slots, then the template made from them. A
        // pipeline's writes keep their shape (one write per resource of its stages), so the
        // first one serves the rest.
        bulk_template_slots.resize(writes.size());
        if (!PushTemplate::MakeSlots(view, buffers, images, BulkKindOf{},
                                     std::span<PushTemplate::Slot>{bulk_template_slots}) ||
            !MakeBulkTemplate(bind_point)) {
            bulk_template_failed = true; // the bulk packet, as before
            bulk_template_slots.clear();
            if constexpr (Common::Log::Enabled) {
                ++stats.unsupported;
            }
            return {};
        }
        if constexpr (Common::Log::Enabled) {
            ++stats.made;
        }
    }
    const std::span<const PushTemplate::Slot> slots{bulk_template_slots};
    if (!PushTemplate::Matches(view, buffers, images, BulkKindOf{}, slots)) {
        if constexpr (Common::Log::Enabled) {
            ++stats.other_shape;
        }
        return {};
    }
    return *bulk_template;
}

void Pipeline::VerifyTemplatePush(const DescriptorWrites& writes, const std::byte* recorded,
                                  std::span<const vk::DescriptorBufferInfo> buffers,
                                  std::span<const vk::DescriptorImageInfo> images) const {
    // The payload as the recorder encoded it (what the replay hands the driver), read through
    // the template's slots as the driver reads it: every descriptor must be the one its write
    // points at.
    auto& stats = g_push_template_stats;
    const std::span<const std::byte> payload{
        recorded, static_cast<size_t>(PushTemplate::PayloadBytes(buffers, images))};
    const std::span<const vk::WriteDescriptorSet> view{writes.data(), writes.size()};
    const auto same = [](const vk::WriteDescriptorSet& w, u32 j, const std::byte* bytes) {
        if (BulkKindOf{}(w.descriptorType) == PushTemplate::Kind::Buffer) {
            vk::DescriptorBufferInfo info;
            std::memcpy(&info, bytes, sizeof(info));
            return info.buffer == w.pBufferInfo[j].buffer &&
                   info.offset == w.pBufferInfo[j].offset && info.range == w.pBufferInfo[j].range;
        }
        vk::DescriptorImageInfo info;
        std::memcpy(&info, bytes, sizeof(info));
        return info.sampler == w.pImageInfo[j].sampler &&
               info.imageView == w.pImageInfo[j].imageView &&
               info.imageLayout == w.pImageInfo[j].imageLayout;
    };
    const size_t bad = PushTemplate::FirstDiffering(
        view, std::span<const PushTemplate::Slot>{bulk_template_slots}, payload, same);
    if constexpr (Common::Log::Enabled) {
        ++stats.verified;
        if (bad != writes.size() && ++stats.mismatches <= 64) {
            LOG_ERROR(Render_Vulkan,
                      "FAST_CPU_PUSH_TEMPLATE verify: write {} of {} (binding {}) reads other "
                      "descriptors through the template as recorded; the bulk packet decides, "
                      "mismatch #{}",
                      bad, writes.size(), bad < writes.size() ? writes[bad].dstBinding : 0u,
                      stats.mismatches);
        }
    }
}

bool Pipeline::MakeBulkTemplate(vk::PipelineBindPoint bind_point) const {
    // A push descriptor template reads the set's layout from the pipeline layout (set 0, as the
    // writes push it); every entry steps one info at a time through the payload.
    if (!VULKAN_HPP_DEFAULT_DISPATCHER.vkCmdPushDescriptorSetWithTemplateKHR ||
        !VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateDescriptorUpdateTemplate) {
        return false;
    }
    boost::container::small_vector<vk::DescriptorUpdateTemplateEntry, 32> entries;
    for (const auto& slot : bulk_template_slots) {
        entries.push_back({
            .dstBinding = slot.binding,
            .dstArrayElement = slot.element,
            .descriptorCount = slot.count,
            .descriptorType = static_cast<vk::DescriptorType>(slot.type),
            .offset = static_cast<size_t>(slot.offset),
            .stride = PushTemplate::InfoBytes,
        });
    }
    const vk::DescriptorUpdateTemplateCreateInfo info{
        .descriptorUpdateEntryCount = static_cast<u32>(entries.size()),
        .pDescriptorUpdateEntries = entries.data(),
        .templateType = vk::DescriptorUpdateTemplateType::ePushDescriptors,
        .pipelineBindPoint = bind_point,
        .pipelineLayout = *pipeline_layout,
        .set = 0,
    };
    auto result = instance.GetDevice().createDescriptorUpdateTemplateUnique(info);
    if (result.result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "FAST_CPU_PUSH_TEMPLATE: template creation failed ({}); "
                    "this pipeline keeps the bulk packet", vk::to_string(result.result));
        return false;
    }
    bulk_template = std::move(result.value);
    return true;
}

std::string Pipeline::GetDebugString() const {
    std::string stage_desc;
    for (const auto& stage : stages) {
        if (stage) {
            const auto shader_name = PipelineCache::GetShaderName(stage->stage, stage->pgm_hash);
            if (stage_desc.empty()) {
                stage_desc = shader_name;
            } else {
                stage_desc = fmt::format("{},{}", stage_desc, shader_name);
            }
        }
    }
    return stage_desc;
}

} // namespace Vulkan
