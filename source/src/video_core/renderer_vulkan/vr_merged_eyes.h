// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// VR_MERGED_EYES (on by default): a native
// stereo direct dispatch that FAST bit 53 takes (the eyes never touch the same memory, so no
// barrier between them) runs both eyes in one launch: dispatch(x, y, 2), the eye being the
// group's z (EmitContext merged_eyes). Saves the second launch and its push per pair.
//   VR_MERGED_EYES=0  off (two launches, as before);
//   VR_MERGED_EYES=v  on, and the first merged dispatch of every shader, then every 64th, also
//                     runs the two-launch way from the same inputs (bit 53's verify,
//                     EyeOverlapVerify): written buffer ranges and both image layers compared.
// Per compute pipeline a variant: stage B asks for it (Wanted) the first time a dispatch could
// merge; the owner copies what translating the program again reads (GetComputePipeline ->
// BuildMergedEyes: that dispatch's code, user data and flattened user data, and the walker that
// flattened them); a compile thread translates from that copy with merged_eyes (reading no guest
// memory: Shader::SrtSnapshot), checks the descriptor layout equals the pipeline's and builds the
// Vulkan pipeline. Until it is Ready the two launches run. The variant is never written to the
// disk cache, and defines its own layout, identical to the pipeline's (identically defined
// layouts are compatible): nothing of it depends on the base pipeline's lifetime. Excluded:
// dispatches with z > 1, screen-rect shaders, emulated shared memory (addressed by the workgroup
// index, which both eyes' groups share), raw memory and special buffers (bit 53 does not take
// those either).

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>
#include "common/logging/log.h"
#include "common/thread.h"
#include "common/types.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vk_platform.h"

namespace Vulkan::MergedEyes {

inline char Mode() {
    static const char mode = [] {
        const char* v = std::getenv("VR_MERGED_EYES");
        if (!v || !*v) {
            return '1';
        }
        return v[0] == '0' ? '0' : v[0] == 'v' ? 'v' : '1';
    }();
    return mode;
}

/// 0: no verify.
inline u32 VerifyPeriod() {
    return Mode() == 'v' ? 64u : 0u;
}

enum class State : u32 { None, Wanted, Compiling, Ready, Failed };

struct Variant {
    std::atomic<State> state{State::None};
    // The base pipeline's layout, defined again (set at construction, then read only).
    std::vector<vk::DescriptorSetLayoutBinding> bindings;
    vk::DescriptorSetLayoutCreateFlags flags{};
    bool subgroup64{};
    u64 hash{};
    // Written by the compile thread before `state` becomes Ready (release); read after acquire.
    vk::UniqueDescriptorSetLayout set_layout;
    vk::UniquePipelineLayout layout;
    vk::UniquePipeline pipeline;
};

struct Stats {
    std::atomic<u64> merged{}, not_ready{}, built{}, failed{};
    std::atomic<u64> verify_ok{}, verify_mismatch{}, verify_skipped{};
    std::atomic<s64> last_log{};
};
inline Stats g_stats;

/// A dispatch that could merge: merged, or its variant not ready yet. Logs every 5 s.
inline void NoteDispatch(bool merged) {
    auto& s = g_stats;
    constexpr auto relaxed = std::memory_order_relaxed;
    (merged ? s.merged : s.not_ready).fetch_add(1, relaxed);
    const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 last = s.last_log.load(relaxed);
    if (now - last < 5000 || !s.last_log.compare_exchange_strong(last, now, relaxed)) {
        return;
    }
    LOG_INFO(Render_Vulkan,
             "VR merged eyes: merged={} not_ready={} variants built={} failed={} verify ok={} "
             "mismatch={} skipped={}",
             s.merged.load(relaxed), s.not_ready.load(relaxed), s.built.load(relaxed),
             s.failed.load(relaxed), s.verify_ok.load(relaxed), s.verify_mismatch.load(relaxed),
             s.verify_skipped.load(relaxed));
}

/// One thread translating the variants and building their Vulkan pipelines. Destroyed before the
/// device (the pipeline cache's destructor); jobs still queued then are dropped as Failed.
class Compiler {
public:
    /// A variant's SPIR-V, or why it stays two launches. Runs on the compile thread with its
    /// pools, from inputs the owner copied (BuildMergedEyes): it must not read guest memory.
    using Translate =
        std::function<const char*(const Variant&, Shader::Pools&, std::vector<u32>& spv)>;

    explicit Compiler(vk::Device device_) : device{device_}, thread{[this] { Run(); }} {}
    ~Compiler() {
        {
            std::scoped_lock lock{mutex};
            stop = true;
        }
        cv.notify_one();
        thread.join();
        for (auto& job : jobs) {
            job.variant->state.store(State::Failed, std::memory_order_release);
        }
    }
    Compiler(const Compiler&) = delete;
    Compiler& operator=(const Compiler&) = delete;

    void Submit(std::shared_ptr<Variant> variant, Translate translate) {
        {
            std::scoped_lock lock{mutex};
            jobs.push_back({std::move(variant), std::move(translate)});
        }
        cv.notify_one();
    }

private:
    struct Job {
        std::shared_ptr<Variant> variant;
        Translate translate;
    };

    void Run() {
        Common::SetCurrentThreadName("VrMergedEyes");
        Shader::Pools pools; // this thread's translations, emptied after each
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
            }
            Build(job, pools);
        }
    }

    void Build(Job& job, Shader::Pools& pools) {
        auto& v = *job.variant;
        std::vector<u32> spv;
        const char* const rejected = job.translate(v, pools, spv);
        pools.ReleaseContents();
        if (rejected) {
            LOG_INFO(Render_Vulkan, "VR merged eyes: cs {:08x} stays two launches: {}", v.hash,
                     rejected);
            g_stats.failed.fetch_add(1, std::memory_order_relaxed);
            v.state.store(State::Failed, std::memory_order_release);
            return;
        }
        LOG_INFO(Render_Vulkan, "VR merged eyes: cs {:08x}: variant compiling", v.hash);
        const auto fail = [&](const char* what, vk::Result result) {
            LOG_WARNING(Render_Vulkan, "VR merged eyes: cs {:08x}: {} failed: {}", v.hash, what,
                        vk::to_string(result));
            g_stats.failed.fetch_add(1, std::memory_order_relaxed);
            v.state.store(State::Failed, std::memory_order_release);
        };
        auto [set_result, set_layout] = device.createDescriptorSetLayoutUnique({
            .flags = v.flags,
            .bindingCount = static_cast<u32>(v.bindings.size()),
            .pBindings = v.bindings.data(),
        });
        if (set_result != vk::Result::eSuccess) {
            return fail("descriptor set layout", set_result);
        }
        const vk::DescriptorSetLayout set = *set_layout;
        const vk::PushConstantRange push_constants = {
            .stageFlags = vk::ShaderStageFlagBits::eCompute,
            .offset = 0,
            .size = sizeof(Shader::PushData),
        };
        auto [layout_result, layout] = device.createPipelineLayoutUnique({
            .setLayoutCount = 1U,
            .pSetLayouts = &set,
            .pushConstantRangeCount = 1U,
            .pPushConstantRanges = &push_constants,
        });
        if (layout_result != vk::Result::eSuccess) {
            return fail("pipeline layout", layout_result);
        }
        auto [module_result, module] = device.createShaderModuleUnique({
            .codeSize = spv.size() * sizeof(u32),
            .pCode = spv.data(),
        });
        if (module_result != vk::Result::eSuccess) {
            return fail("shader module", module_result);
        }
        const vk::PipelineShaderStageRequiredSubgroupSizeCreateInfo subgroup_size_ci = {
            .requiredSubgroupSize = 64,
        };
        const vk::ComputePipelineCreateInfo ci = {
            .stage =
                {
                    .pNext = v.subgroup64 ? &subgroup_size_ci : nullptr,
                    .stage = vk::ShaderStageFlagBits::eCompute,
                    .module = *module,
                    .pName = "main",
                },
            .layout = *layout,
        };
        auto [pipeline_result, pipeline] = device.createComputePipelineUnique({}, ci);
        if (pipeline_result != vk::Result::eSuccess) {
            return fail("pipeline", pipeline_result);
        }
        SetObjectName(device, *pipeline, "Compute Pipeline {:08x} merged eyes", v.hash);
        v.set_layout = std::move(set_layout);
        v.layout = std::move(layout);
        v.pipeline = std::move(pipeline);
        g_stats.built.fetch_add(1, std::memory_order_relaxed);
        v.state.store(State::Ready, std::memory_order_release);
    }

    vk::Device device;
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<Job> jobs;
    bool stop{};
    std::thread thread;
};

} // namespace Vulkan::MergedEyes
