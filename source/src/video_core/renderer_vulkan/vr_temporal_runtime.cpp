// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "video_core/renderer_vulkan/vr_temporal_profile.h"
#include "common/assert.h"
#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vr_dlss_runtime.h"
#include "video_core/renderer_vulkan/vr_fsr4_interleave.h"
#include "video_core/renderer_vulkan/vr_fsr4_runtime.h"
#include "video_core/renderer_vulkan/vr_temporal_runtime.h"
#include "video_core/renderer_vulkan/vr_dlss_direct.h"
#include "video_core/renderer_vulkan/vr_upscaler_fast.h"
namespace Vulkan {
VrTemporalRuntime::VrTemporalRuntime(const Instance& i, Scheduler& s) : instance(i), scheduler(s) {}
VrTemporalRuntime::~VrTemporalRuntime() {
    // The worker owns pair/session state until its initialization fence completes.
    if (initialization.valid()) {
        try { initialization.get(); } catch (const std::exception&) {}
    }
    if (!touched) return;
    // Teardown follows the last command buffer; no new device-wide wait, and no module
    // unload while its callbacks or FSR contexts can still be referenced by the GPU.
    const auto ticket = scheduler.CurrentTick();
    pair.Retire(scheduler.GetMasterSemaphore()->KnownGpuTick()); // disables new work immediately
    scheduler.DeferOperation([old_pair = std::move(pair), old_fsr = std::move(fsr), ticket]() mutable {
        const bool retired = old_pair.Retire(ticket);
        ASSERT(retired);
        if (old_fsr) {
            const bool shut_down = old_fsr->Shutdown(ticket);
            ASSERT(shut_down);
            old_fsr.reset();
        }
    });
}
VrTemporalRuntime::Result VrTemporalRuntime::BeginPair(const VR::Graphics::Config& accepted,
        const VrTemporalPair::Plan& plan, VkCommandBuffer command, uint64_t ticket,
        uint64_t completed) {
    return pair.Begin(accepted, plan, {instance.GetDevice(), instance.GetPhysicalDevice()},
        command, ticket, completed,
        [&]() { return VrDlssRuntime::Get().Activate(accepted, plan.dlss_preset); },
        [&]() -> VrTemporalPair::FsrActivation {
            // This activator runs only after the old pair retires. Never unload a
            // provider while its contexts or submitted commands can still be used.
            if (fsr && fsr->Provider() != accepted.upscaler) {
                if (!fsr->Shutdown(completed)) return {};
                fsr.reset();
            }
            if (!fsr) fsr = std::make_unique<VrFsr4Runtime>(accepted.upscaler);
            auto* session = fsr->Activate(accepted, instance);
            if (session) fsr_assets = fsr->Assets().wstring();
            return {session, fsr_assets.c_str(), session && fsr->OptimizedPostInstalled(),
                    session && fsr->FencedSpdInstalled()};
        });
}
VrTemporalRuntime::InitializationResult VrTemporalRuntime::Initialize(
        const VR::Graphics::Config& accepted, const VrTemporalPair::Plan& plan,
        uint64_t ticket, uint64_t completed) {
    // No scheduler command-buffer access from this worker. Creation can record uploads,
    // so even a failed feature must finish its private command buffer before retirement.
    const auto device = instance.GetDevice();
    const auto fail = [](const char* why) { return InitializationResult{Result::Failed, why}; };
    auto [pool_result, pool] = device.createCommandPoolUnique({
        .flags = vk::CommandPoolCreateFlagBits::eTransient,
        .queueFamilyIndex = instance.GetGraphicsQueueFamilyIndex()});
    if (pool_result != vk::Result::eSuccess) return fail("Provider initialization command pool failed");
    auto [alloc_result, commands] = device.allocateCommandBuffersUnique({
        .commandPool = *pool, .level = vk::CommandBufferLevel::ePrimary, .commandBufferCount = 1});
    if (alloc_result != vk::Result::eSuccess) return fail("Provider initialization command buffer failed");
    auto [fence_result, fence] = device.createFenceUnique({});
    if (fence_result != vk::Result::eSuccess) return fail("Provider initialization fence failed");
    const auto command = *commands.front();
    if (command.begin({.flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit}) != vk::Result::eSuccess)
        return fail("Provider initialization recording failed");
    const auto result = BeginPair(accepted, plan, command, ticket, completed);
    // Initialization uploads become visible to subsequent submissions on this queue.
    const vk::MemoryBarrier2 visibility{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite};
    command.pipelineBarrier2({.memoryBarrierCount = 1, .pMemoryBarriers = &visibility});
    if (command.end() != vk::Result::eSuccess) return fail("Provider initialization recording end failed");
    vk::Result submitted;
    {
        // External synchronization for the shared graphics queue, held only for submit.
        std::scoped_lock lock{Scheduler::submit_mutex};
        submitted = instance.GetGraphicsQueue().submit(
            vk::SubmitInfo{.commandBufferCount = 1, .pCommandBuffers = &command}, *fence);
    }
    if (submitted != vk::Result::eSuccess) return fail("Provider initialization submission failed");
    if (device.waitForFences(*fence, true, UINT64_MAX) != vk::Result::eSuccess)
        return fail("Provider initialization GPU completion failed");
    std::string why;
    if (result == Result::Unavailable || result == Result::Failed || result == Result::Invalid) {
        why = accepted.upscaler == VR::Graphics::Upscaler::Dlss
            ? VrDlssRuntime::Get().Problem() : fsr ? fsr->Problem() : "FSR runtime unavailable";
        if (why.empty()) why = "Stereo provider initialization failed";
    }
    return {result, std::move(why)};
}
VrTemporalPair::Result VrTemporalRuntime::Begin(const VR::Graphics::Config& accepted,
                                                const VrTemporalPair::Plan& plan) {
    if (initialization.valid()) {
        if (initialization.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
            return accepted.upscaler == VR::Graphics::Upscaler::Off ? Result::Disabled : Result::Pending;
        try {
            auto done = initialization.get();
            problem = std::move(done.problem);
            initialization_failed = done.result == Result::Failed || done.result == Result::Unavailable ||
                                    done.result == Result::Invalid;
        } catch (const std::exception& e) {
            initialization_failed = true;
            problem = std::string("Provider initialization failed: ") + e.what();
        }
        epoch = pair.Epoch();
        g_upscaler_fast.fsr4_post.store(pair.FsrPostState(), std::memory_order_relaxed);
        g_upscaler_fast.fsr4_spd.store(pair.FsrSpdState(), std::memory_order_relaxed);
        g_fsr4_interleave.bridge.store(pair.FsrPairSupport(), std::memory_order_relaxed);
        g_dlss_direct.twin.store(pair.DlssTwinState(), std::memory_order_relaxed);
    }
    if (accepted.upscaler == VR::Graphics::Upscaler::Off) {
        initialization_failed = false;
        problem.clear();
        if (!touched) return Result::Disabled;
    }
    touched = true;
    scheduler.GetMasterSemaphore()->Refresh();
    const auto completed = scheduler.GetMasterSemaphore()->KnownGpuTick();
    const bool same_attempt = VR::Graphics::Pack(accepted) == VR::Graphics::Pack(initialization_config) &&
                              plan == initialization_plan;
    if (initialization_failed && same_attempt) return Result::Failed;
    if (accepted.upscaler != VR::Graphics::Upscaler::Off && pair.NeedsConfiguration(accepted, plan)) {
        if (!pair.Retire(completed)) return Result::Pending;
        epoch = pair.Epoch();
        initialization_config = accepted;
        initialization_plan = plan;
        initialization_failed = false;
        problem.clear();
        const auto ticket = scheduler.CurrentTick();
        try {
            initialization = std::async(std::launch::async, [this, accepted, plan, ticket, completed] {
                return Initialize(accepted, plan, ticket, completed);
            });
        } catch (const std::exception& e) {
            initialization_failed = true;
            problem = std::string("Cannot start provider preparation: ") + e.what();
            return Result::Failed;
        }
        return Result::Pending; // Native rendering continues; no jitter or motion generation.
    }
    // Steady state: the pair is configured, so Begin records nothing. Borrowing the raw command
    // buffer anyway (bit 37 off) drained the recorder at the frame's first draw and kept the
    // rest of that command buffer on this thread. Configuration always goes through the
    // asynchronous path above; OFF only retires.
    VkCommandBuffer command = VK_NULL_HANDLE;
    if (!UpscalerFastOn(UpscalerFastPart::Recording) ||
        (accepted.upscaler != VR::Graphics::Upscaler::Off &&
         pair.NeedsConfiguration(accepted, plan)))
        command = scheduler.CommandBuffer();
    else
        UpscalerFastNote(g_upscaler_fast.raw_avoided);
    const auto result = BeginPair(accepted, plan, command, scheduler.CurrentTick(), completed);
    epoch = pair.Epoch();
    if (result == Result::Disabled) touched = false;
    return result;
}
VrTemporalPair::Result VrTemporalRuntime::Resolve(uint64_t frame_id,
                                                  const VrTemporalPair::Frame& frame) {
    if (!CanProduceMotion()) return VrTemporalPair::Result::Pending;
    scheduler.EndRendering();
    // Completion at Begin can be stale by this point. Retire actual finished
    // dispatches, then apply backpressure only if all three FSR slots are busy.
    auto* timeline = scheduler.GetMasterSemaphore();
    timeline->Refresh();
    pair.RefreshCompletion(timeline->KnownGpuTick());
    if (const auto ticket = pair.ReuseTicket()) {
        ScratchProfile::Cpu wait_profile(TemporalProfile::SlotWait, "Temporal FSR slot wait");
        scheduler.Wait(ticket);
        timeline->Refresh();
        pair.RefreshCompletion(timeline->KnownGpuTick());
    }
    // The providers record into a raw command buffer (bbport keeps FSR 4/DLSS direct too).
    const auto cmd = [&] {
        ScratchProfile::Cpu borrow_profile(TemporalProfile::CommandBorrow, "Temporal command borrow");
        return scheduler.CommandBuffer();
    }();
    // Bit 37: the borrow ends with this function (no raw handle outlives it), so the merge and
    // the rest of this command buffer return to the recording thread.
    const bool release = UpscalerFastOn(UpscalerFastPart::Recording);
    const auto release_borrow = [&] {
        if (!release)
            return;
        scheduler.ReleaseCommandBuffer();
        UpscalerFastNote(g_upscaler_fast.raw_released);
    };
    const auto result = [&] {
        TemporalProfile::Scope provider_profile(scheduler, TemporalProfile::Provider,
                                                "Temporal provider evaluation (both eyes)");
        return pair.Evaluate(frame_id, cmd, scheduler.CurrentTick(), frame);
    }();
    if (const auto* paired = pair.FsrPairResult()) // FAST bit 45
        Fsr4InterleaveNote(*paired);
    ScratchPush::Invalidate();
    if (pair.DlssTwinState() == 3) // FAST_DLSS_DIRECT_VERIFY twin failed after creation
        g_dlss_direct.twin.store(3, std::memory_order_relaxed);
    if (result == VrTemporalPair::Result::Invalid || result == VrTemporalPair::Result::Disabled) {
        release_borrow();
        return result;
    }
    // A failed/pending second eye may still leave writes from the first. Order those too.
    // Both outputs become readable together. The scene owner handles subsequent image copies
    // or sampling layouts, and keeps all four input/output resources per eye alive.
    std::array<vk::ImageMemoryBarrier2, 3> barriers{};
    uint32_t count = 0;
    const auto readable = [&](VkImage image) {
        // NGX may clear its output with transfer commands as well as write it
        // through compute/CUDA; the provider's whole output write set must finish.
        barriers[count++] = {.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
                        .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                        .dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite,
                        .oldLayout = vk::ImageLayout::eGeneral,
                        .newLayout = vk::ImageLayout::eGeneral,
                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                        .image = image,
                        .subresourceRange = {vk::ImageAspectFlagBits::eColor,0,1,0,1}};
    };
    for (uint32_t eye = 0; eye < 2; ++eye)
        if (frame.eyes[eye].output.image) readable(frame.eyes[eye].output.image);
    if (frame.verify_output.image) // FAST_UPSCALER_VERIFY twin (bit 37), read back later
        readable(frame.verify_output.image);
    cmd.pipelineBarrier2({.imageMemoryBarrierCount = count, .pImageMemoryBarriers = barriers.data()});
    release_borrow();
    return result;
}
} // namespace Vulkan
