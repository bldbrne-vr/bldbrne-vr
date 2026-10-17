// SPDX-License-Identifier: GPL-2.0-or-later
//
// Fast bit 40: async graphics pipeline creation, ported from bbport's BB_ASYNC_PIPELINES
// (BBV = bbport gpu/shadps4/video_core: renderer_vulkan/vk_pipeline_cache.cpp:370-418 compiler
// pool, :540-598 pending jobs and waits, :626-641 FinishPipeline, :643-693 AsyncSkippable).
//
// Only the GraphicsPipeline constructor (descriptor/pipeline layouts and the driver's
// vkCreateGraphicsPipelines) runs on the compile threads. Shader translation, compute
// pipelines, every map and the disk-cache registration stay with the selection owner (whoever
// holds selection_mutex: Stage A, or Stage B for its view-mask reselects and VR variants),
// which installs a finished pipeline when a draw needs it.
//
// Where jobs come from:
//  - the bit-31 preparation workers (vk_pipeline_prepare.cpp): a prepared draw whose key has
//    no pipeline yet is offered through lock-free slots, so it compiles while Stage A is still
//    behind it. The workers run at idle priority and Windows does not boost a preempted idle
//    thread holding a lock, so they never take a lock the owner takes;
//  - the VR body cutaway: the other cut levels of a body pipeline, and with VrPushData its one
//    cut variant ahead of the cutaway, from the body's colour draws (PrebuildVrCutPipeline);
//  - the VR variants that never wait (GetVrCutPipeline and GetVrMotionPipeline with `wait`
//    false): the draw goes uncut, or with camera motion only, until its variant is ready. Their
//    jobs go to the front of the compile queue (a draw is already going without them), and a
//    later draw asks the job's state alone until it is done;
//  - bbport's skip-the-draw for passes drawn every frame (below).
//
// Otherwise a draw whose pipeline is still compiling waits for it, so the output is the same as
// without the bit; a job no thread has started yet is taken back and compiled by the owner, so a
// wait is never longer than compiling here. bbport's skip rule (the draw is left out until its
// pipeline is ready) is on by default in native-stereo VR, where a stall freezes the whole world
// against the head: FAST_ASYNC_PIPELINE_SKIP=0 turns it off, =1 turns it on in flat mode too
// (bbport's rule alone, as before). In VR a missing draw shows in both eyes, so a draw the rule
// would leave out is drawn after all when a veto holds (vk_pipeline_skip.h: it may write memory
// or stencil, it is the hunter's own, it is UI, its targets are not eye pairs, or the frame is
// not immersive). The age cap, FAST_ASYNC_PIPELINE_SKIP_MS (640; 0: none), bounds how long a key
// is left out without turning a burst into one long freeze: a key left out that long goes to the
// front of the compile queue (never compiled here for it), and one whose compile alone has run
// that long is waited for, at most one such wait a frame. These rules run only for a draw that
// would be left out.
//
// Unlike bbport (BBV vk_graphics_pipeline.cpp:506-520 builds the descriptor layout from the
// shared Info, whose user data the GPU thread rewrites meanwhile, BBV vk_pipeline_cache.cpp:
// 583-589), a job owns copies of every stage's user and flattened data and pins them with
// ScratchDraw::Scope, so the constructor's sharp reads never touch live Info data. Vertex input
// must be dynamic state (as bbport requires): otherwise the constructor reads guest V#s.
//
// Statistics: a 'FAST async_pipelines:' line about every 240 frames after any activity (the
// vetoes, the age cap, the deferred VR variants and the owner's stalls by reason at its end).
// With the VR vetoes on, any single owner stall of 30 ms or more also gets a 'FAST
// async_pipelines stall:' line with its reason (the first 64). After it, when anything compiled
// since, a 'FAST compile reasons:' line counts why, and the owner's stall ms by why
// (vk_compile_reason.h).
// FAST_ASYNC_PIPELINES_VERIFY=1 compares a job's inputs with the owner's own selection of the
// same key before installing it (a mismatch is logged and compiled here instead).
// FAST_ASYNC_PIPELINE_THREADS=N (1..8) sets the compile threads (bbport: hardware/4, 1..4).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "../../../scratchpad/dual-eye-perf/draw_inputs.h"
#include "../../../scratchpad/dual-eye-perf/draw_registers.h"
#include "../../../scratchpad/dual-eye-perf/native_stereo.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/fast_render.h"
#include "video_core/renderer_vulkan/vk_compile_trace.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"

namespace Vulkan {
namespace AsyncDetail {

using Clock = std::chrono::steady_clock;

u64 NowNs() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch())
            .count());
}

bool EnvOn(const char* name) {
    const char* value = std::getenv(name);
    return value && value[0] == '1';
}

// Skip-the-draw: FAST_ASYNC_PIPELINE_SKIP=0|1 when set, else on in native stereo.
bool SkipMode() {
    static const bool enabled =
        PipelineSkip::SkipOn(std::getenv("FAST_ASYNC_PIPELINE_SKIP"), NativeStereo::Enabled());
    return enabled;
}

// Native stereo: the age cap (FAST_ASYNC_PIPELINE_SKIP_MS, 0..10000, 0: none; PipelineSkip::Aged).
u64 AgeCapNs() {
    static const u64 cap = PipelineSkip::AgeCapNs(std::getenv("FAST_ASYNC_PIPELINE_SKIP_MS"));
    return cap;
}

bool VerifyMode() {
    static const bool enabled = EnvOn("FAST_ASYNC_PIPELINES_VERIFY");
    return enabled;
}

u32 ThreadCount() {
    static const u32 count = [] {
        if (const char* value = std::getenv("FAST_ASYNC_PIPELINE_THREADS"); value && *value) {
            return std::clamp<u32>(static_cast<u32>(std::strtoul(value, nullptr, 10)), 1u, 8u);
        }
        // BBV vk_pipeline_cache.cpp:555-557.
        return std::clamp<u32>(std::thread::hardware_concurrency() / 4, 1u, 4u);
    }();
    return count;
}

// A compile thread (Run), not the owner taking a job back: the compile reasons' counters.
thread_local bool t_compile_thread = false;

// FAST_CPU_VIEW_PREDICT (native stereo, on unless '0'; read once at start-up, a plain load per
// draw): Stage A and the preparation workers select a draw's pipeline with the view mask the
// recording will want (PipelineCache::PredictViewMask), so a draw into a target that is not an
// eye pair no longer builds a multiview twin that the recording's reselect never binds. Off with
// the draw inspector (VR_DRAW_CTL) or the foreground probe (VR_FOREGROUND_PROBE): both read Stage
// A's pipeline before that reselect.
const bool g_view_predict = PipelineSkip::ViewPredictOn(
    std::getenv("FAST_CPU_VIEW_PREDICT"), true, [] {
        const char* inspector = std::getenv("VR_DRAW_CTL");
        const char* probe = std::getenv("VR_FOREGROUND_PROBE");
        return (inspector != nullptr && *inspector != '\0') ||
               (probe != nullptr && std::atoi(probe) > 0);
    }());

/// One graphics pipeline to create off the owner thread (bbport PendingPipeline, BBV
/// vk_pipeline_cache.cpp:420-427), with everything the constructor reads owned by the job.
struct Job {
    enum : u32 { Queued = 0, Running = 1, Done = 2 };
    GraphicsPipelineKey key{};
    u64 hash{};
    std::array<const Shader::Info*, MaxShaderStages> infos{}; // the programs' shared Info
    std::array<Shader::RuntimeInfo, MaxShaderStages> runtime_infos{};
    std::optional<Shader::Gcn::FetchShaderData> fetch_shader{};
    std::array<vk::ShaderModule, MaxShaderStages> modules{};
    // Pinned while the constructor runs: its sharp reads see these words only.
    std::array<ScratchDraw::OwnedInputs, MaxShaderStages> inputs{};
    u64 generation{};      // programs_generation the modules belong to
    bool speculative{};    // offered by a preparation worker
    bool register_data{};  // written to the disk cache when installed (not probe/baked motion)
    CompileReason::Variant variant{}; // the VR variant it is (its log line's 'why:')
    // The owner only: the skip rule first left its draw out (the age cap, skip_max_ms), and the
    // job went to the front of the compile queue (once).
    u64 first_skip_ns{};
    bool promoted{};
    u64 done_frame{};
    std::atomic<u32> state{Queued};
    // When a thread started compiling it (0: not yet): the age cap's wait (the owner reads it).
    std::atomic<u64> start_ns{};
    // Written by the thread that ran it, before `state` becomes Done (release).
    std::unique_ptr<GraphicsPipeline> pipeline;
    GraphicsPipeline::SerializationSupport sdata{};
    u64 compile_ns{};
    bool stale{}, rejected{};
    CompileReason::Tags reason{}; // why it compiled: the owner's wait for it by reason
};

/// bbport NoteDrawTarget (BBV vk_pipeline_cache.cpp:487-525): frames in a row a target was
/// drawn to.
struct TargetStreak {
    u64 last_frame = ~u64{0};
    u32 frames = 0;
};

/// The draw's render target: its first colour buffer written, else its depth buffer, else 0
/// (BBV vk_pipeline_cache.cpp:493-503).
u64 DrawTarget(const AmdGpu::Regs& regs) {
    if (regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Disable) {
        for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
            if (regs.color_buffers[cb] && regs.color_target_mask.GetMask(cb)) {
                return regs.color_buffers[cb].Address();
            }
        }
    }
    return regs.depth_buffer.DepthValid() ? regs.depth_buffer.DepthAddress() : 0;
}

/// The draw changes stencil, or it is a DB clear (PipelineSkip::WritesStencil).
bool WritesStencil(const AmdGpu::Regs& regs) {
    const auto& ops = regs.stencil_control;
    const PipelineSkip::StencilFace front{
        static_cast<u32>(ops.stencil_fail_front), static_cast<u32>(ops.stencil_zpass_front),
        static_cast<u32>(ops.stencil_zfail_front), regs.stencil_ref_front.stencil_write_mask};
    const PipelineSkip::StencilFace back{
        static_cast<u32>(ops.stencil_fail_back), static_cast<u32>(ops.stencil_zpass_back),
        static_cast<u32>(ops.stencil_zfail_back), regs.stencil_ref_back.stencil_write_mask};
    const bool stencil = regs.depth_buffer.StencilValid();
    const bool db_clear =
        (regs.depth_render_control.depth_clear_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_render_control.stencil_clear_enable && stencil);
    return PipelineSkip::WritesStencil(regs.depth_control.stencil_enable && stencil, front,
                                       regs.depth_control.backface_enable, back, db_clear);
}

/// The attachments the recording binds for a pipeline writing `mrt_mask` (Rasterizer::
/// PrepareRenderState: the colour targets this pipeline writes, the depth target when its test
/// is on): their key (PipelineSkip::AddTarget, as the recording keys them) and whether one is a
/// layered view. `colours` (optional): how many colour targets there are.
PipelineSkip::ViewFacts TargetFacts(const AmdGpu::Regs& regs, u32 mrt_mask,
                                    u32* colours = nullptr) {
    PipelineSkip::ViewFacts facts{};
    u32 count = 0;
    if (regs.color_control.mode != AmdGpu::ColorControl::OperationMode::Disable) {
        for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb) {
            const auto& col_buf = regs.color_buffers[cb];
            if (!col_buf || !regs.color_target_mask.GetMask(cb) || (mrt_mask & (1u << cb)) == 0) {
                continue;
            }
            facts.layered |=
                PipelineSkip::LayeredView(col_buf.view.slice_start, col_buf.view.slice_max);
            facts.targets = PipelineSkip::AddTarget(facts.targets, col_buf.Address(), cb);
            ++count;
        }
    }
    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        facts.layered |=
            PipelineSkip::LayeredView(regs.depth_view.slice_start, regs.depth_view.slice_max);
        facts.targets = PipelineSkip::AddTarget(facts.targets, regs.depth_buffer.DepthAddress(), 8);
    }
    if (colours != nullptr) {
        *colours = count;
    }
    return facts;
}

/// The draw's targets are not all eye pairs: a layered colour or depth view, or a set of targets
/// the recording last drew with view mask 0 or never drew (`views`, the recording's `desired`).
/// The attachments are the ones the recording binds, keyed alike (TargetFacts).
bool MonoTarget(const AmdGpu::Regs& regs, u32 mrt_mask, const PipelineSkip::TargetViews* views) {
    if (views == nullptr) {
        return true;
    }
    const auto facts = TargetFacts(regs, mrt_mask);
    return facts.layered || views->Find(facts.targets) != 3;
}

/// Why the owner stalled on a pipeline: 0 not a skip candidate, a PipelineSkip::Veto, or
/// StallAged (the age cap's wait).
constexpr u32 StallAged = PipelineSkip::NumVetoes;

const char* StallName(u32 reason) {
    switch (reason) {
    case PipelineSkip::Writer:
        return "veto writer";
    case PipelineSkip::Player:
        return "veto player";
    case PipelineSkip::Ui:
        return "veto ui";
    case PipelineSkip::Mono:
        return "veto mono";
    case PipelineSkip::Stencil:
        return "veto stencil";
    case PipelineSkip::Frame:
        return "veto frame";
    case StallAged:
        return "aged";
    default:
        return "not a skip candidate";
    }
}

} // namespace AsyncDetail

namespace PipelineSkip {

// As fast bit 54's no-op draws had it (vk_rasterizer.cpp). A program from the disk cache keeps
// its buffers, images and uses_dma (InfoPersistent; has_storage_images reads false there, but a
// written storage image is a written image too).
bool StageMayWrite(const Shader::Info& info) {
    if (info.uses_dma || info.has_storage_images) {
        return true;
    }
    for (const auto& buffer : info.buffers) {
        if (buffer.is_written || buffer.buffer_type == Shader::BufferType::GdsBuffer ||
            buffer.buffer_type == Shader::BufferType::FaultBuffer ||
            buffer.buffer_type == Shader::BufferType::BdaPagetable) {
            return true;
        }
    }
    for (const auto& image : info.images) {
        if (image.is_written || image.is_atomic) {
            return true;
        }
    }
    return false;
}

} // namespace PipelineSkip

struct PipelineCache::AsyncPipelines {
    using Job = AsyncDetail::Job;
    static constexpr size_t OfferSlots = 64;
    static constexpr size_t SeenSlots = size_t{1} << 16;
    static constexpr size_t MaxPending = 256;
    static constexpr u64 ExpireFrames = 600; // a finished offer no draw asked for, ~5 s

    explicit AsyncPipelines(PipelineCache& cache_) : cache{cache_} {}

    ~AsyncPipelines() {
        stop.store(true, std::memory_order_release);
        wake.fetch_add(1, std::memory_order_release);
        wake.notify_all();
        threads.clear(); // joins; a thread finishes the pipeline it is creating first
        for (auto& slot : offers) {
            delete slot.exchange(nullptr, std::memory_order_acq_rel);
        }
    }

    AsyncPipelines(const AsyncPipelines&) = delete;
    AsyncPipelines& operator=(const AsyncPipelines&) = delete;

    // ---- Owner (selection lock held) ----

    void Start() {
        if (started.load(std::memory_order_relaxed)) {
            return;
        }
        // Pipelines installed before the bit was first on are never offered again.
        {
            std::scoped_lock lock{mutex};
            for (const auto& [key, pipeline] : cache.graphics_pipelines) {
                if (pipeline) {
                    const u64 hash = std::hash<GraphicsPipelineKey>{}(key);
                    known.insert(hash);
                    MarkSeen(hash);
                }
            }
        }
        thread_count = AsyncDetail::ThreadCount();
        for (u32 i = 0; i < thread_count; ++i) {
            threads.emplace_back([this, i] { Run(i); });
        }
        started.store(true, std::memory_order_release);
        const u64 cap_ms = AsyncDetail::AgeCapNs() / 1'000'000;
        const std::string skip = !AsyncDetail::SkipMode() ? std::string{"off"}
                                 : !cache.vr_skip_vetoes  ? std::string{"on"}
                                 : cap_ms != 0 ? fmt::format("on, VR vetoes, {} ms cap", cap_ms)
                                               : std::string{"on, VR vetoes, no cap"};
        LOG_INFO(Render_Vulkan,
                 "FAST async_pipelines: {} compile threads (skip-the-draw {}, verify {}); "
                 "BBV vk_pipeline_cache.cpp:370-418",
                 thread_count, skip, AsyncDetail::VerifyMode() ? "on" : "off");
    }

    std::shared_ptr<Job> Find(const GraphicsPipelineKey& key) {
        std::scoped_lock lock{mutex};
        const auto it = pending.find(key);
        return it == pending.end() ? nullptr : it->second;
    }

    void Forget(const GraphicsPipelineKey& key) {
        std::scoped_lock lock{mutex};
        pending.erase(key);
    }

    /// The owner installed `key` (compiled here or by a job): offers for it are duplicates.
    void NoteInstalled(const GraphicsPipelineKey& key, u64 hash) {
        {
            std::scoped_lock lock{mutex};
            known.insert(hash);
            pending.erase(key); // a racing offer for the same key: its result is dropped
        }
        MarkSeen(hash);
    }

    std::shared_ptr<Job> MakeJob(const PipelineSelection& selection) const {
        auto job = std::make_shared<Job>();
        job->key = selection.graphics_key;
        job->hash = std::hash<GraphicsPipelineKey>{}(job->key);
        job->infos = selection.infos;
        job->runtime_infos = selection.runtime_infos;
        job->fetch_shader = selection.fetch_shader;
        job->modules = selection.modules;
        for (u32 stage = 0; stage < MaxShaderStages; ++stage) {
            if (const auto* info = selection.infos[stage]) {
                // The owner's view of this draw (its packet scope on Stage B).
                job->inputs[stage].Capture(info, info->UserData(), info->FlatUserData(),
                                           info->ProgramBase());
            }
        }
        job->generation = cache.programs_generation.load(std::memory_order_relaxed);
        // As the synchronous path: not the probe's, nor a motion variant with baked addresses.
        job->register_data = selection.vr_probe_mode == 0 && !selection.vr_motion.Baked();
        job->variant = CompileReason::VariantOf(selection);
        return job;
    }

    /// `first`: to the front (a VR variant a draw already goes without, a cut needed next).
    void Submit(const std::shared_ptr<Job>& job, bool first = false) {
        job->promoted = first;
        {
            std::scoped_lock lock{mutex};
            pending.insert_or_assign(job->key, job);
            known.insert(job->hash);
        }
        MarkSeen(job->hash);
        {
            std::scoped_lock lock{queue_mutex};
            (first ? urgent : queue).push_back(job);
        }
        Wake();
    }

    /// A queued job a draw is going without: the compile threads take it next (once; its old
    /// place in the queue is passed over when they get there, TakeQueued).
    void Promote(const std::shared_ptr<Job>& job) {
        if (job->promoted || job->state.load(std::memory_order_acquire) != Job::Queued) {
            return;
        }
        job->promoted = true;
        ++promoted;
        dirty.store(true, std::memory_order_relaxed);
        {
            std::scoped_lock lock{queue_mutex};
            urgent.push_back(job);
        }
        Wake();
    }

    /// A queued job no compile thread has started: the owner compiles it itself.
    static bool TakeBack(Job& job) {
        u32 expected = Job::Queued;
        return job.state.compare_exchange_strong(expected, Job::Running,
                                                 std::memory_order_acq_rel);
    }

    static void Await(Job& job) {
        for (u32 state = job.state.load(std::memory_order_acquire); state != Job::Done;
             state = job.state.load(std::memory_order_acquire)) {
            job.state.wait(state, std::memory_order_acquire);
        }
    }

    /// bbport FinishPipeline (BBV vk_pipeline_cache.cpp:626-641).
    const GraphicsPipeline* Install(Job& job) {
        const auto it = cache.graphics_pipelines.try_emplace(job.key).first;
        if (!it->second) {
            it.value() = std::move(job.pipeline);
            if (job.register_data) {
                RegisterPipelineData(job.key, job.hash, job.sdata);
            }
            ++cache.num_new_pipelines;
            if (EmulatorSettings.IsShaderCollect()) {
                for (u32 stage = 0; stage < MaxShaderStages; ++stage) {
                    if (job.infos[stage]) {
                        cache.module_related_pipelines[job.modules[stage]].emplace_back(job.key);
                    }
                }
            }
        }
        NoteInstalled(job.key, job.hash);
        return it->second.get();
    }

    /// FAST_ASYNC_PIPELINES_VERIFY: the job was built from the inputs the owner's own selection
    /// of the same key has (the pipeline is a function of these and the key).
    static bool SameInputs(const Job& job, const PipelineSelection& selection) {
        if (!(job.infos == selection.infos) || !(job.modules == selection.modules) ||
            !(job.fetch_shader == selection.fetch_shader)) {
            return false;
        }
        for (u32 stage = 0; stage < MaxShaderStages; ++stage) {
            if (job.infos[stage] && !(job.runtime_infos[stage] == selection.runtime_infos[stage])) {
                return false;
            }
        }
        return true;
    }

    /// bbport AsyncSkippable (BBV vk_pipeline_cache.cpp:643-693): only a pass drawn every frame
    /// (its target drawn to in each of the last 8 frames), never a full-screen pass (12 indices
    /// or fewer: going without one greys whole frames), an indirect draw (only Stage A's direct
    /// draw asks), a loading-screen frame (fewer than 300 draws) or a VR variant.
    bool Skippable(const PipelineSelection& selection) const {
        if (selection.vr_probe_mode || selection.vr_fs_cut || selection.vr_motion.enabled ||
            !selection.regs || last_frame_draws < 300 || selection.regs->num_indices <= 12) {
            return false;
        }
        const u64 target = AsyncDetail::DrawTarget(*selection.regs);
        const auto streak = target_streaks.find(target);
        return target != 0 && streak != target_streaks.end() && streak->second.frames >= 8;
    }

    /// Native stereo, a draw the skip rule would leave out (its key has no finished job): its
    /// VR vetoes, then the age cap. True: leave it out; false: drawn after all, compiled here or
    /// waited for as without the rule (`reason`: the veto, or AsyncDetail::StallAged).
    bool VrSkip(const PipelineSelection& selection, const std::shared_ptr<Job>& job,
                u32& reason) {
        if (const auto veto = VrVeto(selection); veto != PipelineSkip::None) {
            ++vetoes[veto];
            reason = veto;
            dirty.store(true, std::memory_order_relaxed);
            return false;
        }
        if (job == nullptr || job->first_skip_ns == 0) {
            return true; // left out from now on (first_skip_ns, AsyncGraphicsPipeline)
        }
        const u64 now = AsyncDetail::NowNs();
        const u64 cap = AsyncDetail::AgeCapNs();
        if (job->state.load(std::memory_order_acquire) == Job::Queued) {
            // Left out past the cap behind other jobs: compiled next by a compile thread. Never
            // taken back here for it: in a burst that would add up to one long freeze.
            if (PipelineSkip::Aged(job->first_skip_ns, now, cap)) {
                Promote(job);
            }
            return true;
        }
        // Its compile alone has run past the cap: waited for now, one such wait a frame (other
        // keys this frame stay left out until a later one).
        if (!PipelineSkip::Aged(job->start_ns.load(std::memory_order_relaxed), now, cap)) {
            return true;
        }
        const u64 frame = FastRender::g_counters.frames.load(std::memory_order_relaxed);
        if (aged_wait_frame == frame) {
            if (aged_held_frame != frame) {
                aged_held_frame = frame;
                ++aged_held;
                dirty.store(true, std::memory_order_relaxed);
            }
            return true;
        }
        aged_wait_frame = frame;
        ++aged;
        reason = AsyncDetail::StallAged;
        dirty.store(true, std::memory_order_relaxed);
        return false;
    }

    /// Why a draw the skip rule would leave out is drawn after all (vk_pipeline_skip.h): the
    /// rasterizer's VR state first (the frame, the UI phase, the hunter's own draws), then the
    /// draw's registers and programs.
    PipelineSkip::Veto VrVeto(const PipelineSelection& selection) const {
        using PipelineSkip::Veto;
        if (!cache.vr_skip_veto) {
            return Veto::Frame; // no rasterizer to tell the frame
        }
        const auto* vs = selection.infos[static_cast<u32>(Shader::LogicalStage::Vertex)];
        if (const Veto veto = cache.vr_skip_veto(vs); veto != Veto::None) {
            return veto;
        }
        const auto& regs = *selection.regs;
        if (AsyncDetail::WritesStencil(regs)) {
            return Veto::Stencil;
        }
        if (regs.vgt_strmout_config.raw != 0) {
            return Veto::Writer;
        }
        for (const auto* info : selection.infos) {
            if (info != nullptr && PipelineSkip::StageMayWrite(*info)) {
                return Veto::Writer;
            }
        }
        return AsyncDetail::MonoTarget(regs, selection.graphics_key.mrt_mask,
                                       cache.skip_target_views.get())
                   ? Veto::Mono
                   : Veto::None;
    }

    /// A draw left out by the skip rule, or a VR variant's draw that goes on without it.
    void NoteLeftOut(const PipelineSelection& selection, bool skip) {
        ++(skip                          ? skipped
           : selection.vr_motion.enabled ? motion_deferred
                                         : cut_deferred);
        dirty.store(true, std::memory_order_relaxed);
    }

    /// The owner stalled `ns` on one pipeline (compiled here or waited for), for `reason`
    /// (AsyncDetail::StallName): aged_ms / veto_ms, stall_max_ms, and with the VR vetoes on a
    /// line of its own from 30 ms (the first 64).
    void NoteStall(u64 ns, u32 reason, u64 hash) {
        if (reason == AsyncDetail::StallAged) {
            aged_ns.fetch_add(ns, std::memory_order_relaxed);
        } else if (reason != PipelineSkip::None) {
            veto_ns.fetch_add(ns, std::memory_order_relaxed);
        }
        if (ns > stall_max_ns.load(std::memory_order_relaxed)) {
            stall_max_ns.store(ns, std::memory_order_relaxed); // owners only (selection lock)
        }
        if (cache.vr_skip_vetoes && ns >= 30'000'000 && stall_log_budget != 0) {
            --stall_log_budget;
            LOG_INFO(Render_Vulkan,
                     "FAST async_pipelines stall: Stage {}, {:.1f} ms on pipeline {:#x} ({})",
                     FastRender::IsStageB() ? 'B' : 'A', static_cast<double>(ns) / 1e6, hash,
                     AsyncDetail::StallName(reason));
        }
    }

    /// A key the skip rule left out is drawn (installed, or compiled here): how long it was out.
    void NoteBackIn(const Job& job) {
        if (job.first_skip_ns == 0) {
            return;
        }
        const u64 out = AsyncDetail::NowNs() - job.first_skip_ns;
        if (out > skip_max_ns.load(std::memory_order_relaxed)) {
            skip_max_ns.store(out, std::memory_order_relaxed);
        }
    }

    /// Per direct draw, skip mode only (BBV vk_pipeline_cache.cpp:505-525 and :660-675).
    void NoteDraw(const AmdGpu::Regs& regs) {
        const u64 frame = FastRender::g_counters.frames.load(std::memory_order_relaxed);
        if (frame != frame_seen) {
            const u64 draws = FastRender::g_counters.draws.load(std::memory_order_relaxed);
            last_frame_draws = draws - draws_at_start;
            draws_at_start = draws;
            frame_seen = frame;
        }
        const u64 target = AsyncDetail::DrawTarget(regs);
        if (target == 0 || (target == last_target && frame == last_target_frame)) {
            return;
        }
        last_target = target;
        last_target_frame = frame;
        auto& streak = target_streaks[target];
        if (streak.last_frame != frame) {
            streak.frames = streak.last_frame + 1 == frame ? streak.frames + 1 : 1;
            streak.last_frame = frame;
        }
    }

    void MaybeReport() {
        if (!dirty.load(std::memory_order_relaxed)) {
            return;
        }
        const u64 frames = FastRender::g_counters.frames.load(std::memory_order_relaxed);
        if (frames - report_frame < 240) {
            return;
        }
        report_frame = frames;
        dirty.store(false, std::memory_order_relaxed);
        size_t pending_now{};
        {
            std::scoped_lock lock{mutex};
            pending_now = pending.size();
        }
        const auto ms = [](const std::atomic<u64>& ns) {
            return static_cast<double>(ns.load(std::memory_order_relaxed)) / 1e6;
        };
        const auto n = [](const std::atomic<u64>& value) {
            return value.load(std::memory_order_relaxed);
        };
        // avoided: compiles the render threads did not do; saved_ms: their compile time minus
        // the time the owner still waited for them. sync_*: compiles still done on Stage A/B.
        // owner_jobs: jobs the skip rule started. Appended after skip= (cumulative unless said):
        // veto_*: draws the skip rule would have left out, drawn after all (vk_pipeline_skip.h);
        // aged: waits for a compile that ran past the age cap; aged_held: frames that left
        // another such key out (one wait a frame); promoted: jobs moved to the front of the
        // queue (left out past the cap, or a VR variant going without); cut_/motion_deferred:
        // draws of a VR variant still compiling, drawn with the base pipeline meanwhile;
        // variant_jobs: the jobs those started; aged_ms / veto_ms: the owner's stalls after an
        // aged or vetoed decision (the rest of sync_ms / wait_ms had no candidate); skip_keys:
        // keys left out. Since the last line: stall_max_ms, the longest single owner stall, and
        // skip_max_ms, the longest a key stayed left out (pop-in).
        using PipelineSkip::Veto;
        const auto max_ms = [](std::atomic<u64>& ns) {
            return static_cast<double>(ns.exchange(0, std::memory_order_relaxed)) / 1e6;
        };
        LOG_INFO(Render_Vulkan,
                 "FAST async_pipelines: avoided={} ready={} waited={} wait_ms={:.2f} "
                 "saved_ms={:.2f} sync_a={} sync_b={} sync_ms={:.2f} taken_back={} skipped={} "
                 "owner_jobs={} prebuilds={} offered={} offer_full={} accepted={} duplicates={} "
                 "expired={} compiled={} compile_ms={:.2f} stale={} rejected={} fallbacks={} "
                 "verified={} mismatches={} pending={} threads={} skip={} veto_writer={} "
                 "veto_player={} veto_ui={} veto_mono={} veto_stencil={} veto_frame={} aged={} "
                 "aged_held={} promoted={} cut_deferred={} motion_deferred={} variant_jobs={} "
                 "aged_ms={:.2f} veto_ms={:.2f} stall_max_ms={:.2f} skip_keys={} "
                 "skip_max_ms={:.2f}",
                 n(ready) + n(waited), n(ready), n(waited), ms(wait_ns), ms(saved_ns), n(sync_a),
                 n(sync_b), ms(sync_ns), n(taken_back), n(skipped), n(owner_jobs), n(prebuilds),
                 n(offered), n(offer_full), n(accepted), n(duplicates), n(expired), n(compiled),
                 ms(compile_ns), n(stale), n(rejected), n(fallbacks), n(verified), n(mismatches),
                 pending_now, thread_count, AsyncDetail::SkipMode() ? 1 : 0,
                 n(vetoes[Veto::Writer]), n(vetoes[Veto::Player]), n(vetoes[Veto::Ui]),
                 n(vetoes[Veto::Mono]), n(vetoes[Veto::Stencil]), n(vetoes[Veto::Frame]), n(aged),
                 n(aged_held), n(promoted), n(cut_deferred), n(motion_deferred), n(variant_jobs),
                 ms(aged_ns), ms(veto_ns), max_ms(stall_max_ns), n(skip_keys),
                 max_ms(skip_max_ns));
        // Why the shaders and pipelines compiled (vk_compile_reason.h), when any did since.
        if constexpr (Common::Log::Enabled) {
            if (const auto reasons = cache.compile_reasons.Report(); !reasons.empty()) {
                LOG_INFO(Render_Vulkan, "FAST compile reasons: {}", reasons);
            }
        }
    }

    // ---- Preparation workers (lock-free) ----

    bool Seen(u64 hash) const {
        return seen[hash & (SeenSlots - 1)].load(std::memory_order_relaxed) == hash;
    }

    void MarkSeen(u64 hash) {
        seen[hash & (SeenSlots - 1)].store(hash, std::memory_order_relaxed);
    }

    bool Offer(std::unique_ptr<Job>& job) {
        for (auto& slot : offers) {
            Job* empty = nullptr;
            if (slot.load(std::memory_order_relaxed) == nullptr &&
                slot.compare_exchange_strong(empty, job.get(), std::memory_order_acq_rel)) {
                (void)job.release();
                Wake();
                return true;
            }
        }
        return false;
    }

    void Wake() {
        wake.fetch_add(1, std::memory_order_release);
        wake.notify_one();
    }

    // ---- Compile threads ----

    void Run(u32 index) {
        Common::SetCurrentThreadName(fmt::format("shadPS4:PipeAsync{}", index).c_str());
        AsyncDetail::t_compile_thread = true;
        for (;;) {
            const u32 observed = wake.load(std::memory_order_acquire);
            if (stop.load(std::memory_order_acquire)) {
                return;
            }
            if (auto job = TakeQueued()) {
                Compile(*job);
                continue;
            }
            if (auto job = TakeOffer()) {
                Compile(*job);
                continue;
            }
            wake.wait(observed, std::memory_order_acquire);
        }
    }

    std::shared_ptr<Job> TakeQueued() {
        std::scoped_lock lock{queue_mutex};
        for (auto* jobs : {&urgent, &queue}) {
            while (!jobs->empty()) {
                auto job = std::move(jobs->front());
                jobs->pop_front();
                if (TakeBack(*job)) {
                    return job;
                }
                // Taken back by the owner meanwhile, or started from its other place.
            }
        }
        return {};
    }

    std::shared_ptr<Job> TakeOffer() {
        for (auto& slot : offers) {
            if (slot.load(std::memory_order_relaxed) == nullptr) {
                continue;
            }
            std::unique_ptr<Job> owned{slot.exchange(nullptr, std::memory_order_acq_rel)};
            if (!owned) {
                continue;
            }
            std::shared_ptr<Job> job{std::move(owned)};
            std::vector<std::shared_ptr<Job>> evicted; // their pipelines die outside the lock
            {
                std::scoped_lock lock{mutex};
                if (known.contains(job->hash) || pending.contains(job->key)) {
                    ++duplicates;
                    continue;
                }
                if (pending.size() >= MaxPending) {
                    ExpireLocked(evicted);
                }
                if (pending.size() >= MaxPending) {
                    ++offer_full;
                    continue;
                }
                // Accepted and started at once: the owner only ever waits for it.
                job->state.store(Job::Running, std::memory_order_relaxed);
                pending.emplace(job->key, job);
                known.insert(job->hash);
            }
            ++accepted;
            return job;
        }
        return {};
    }

    /// Finished jobs no draw asked for (a stale worker state, a draw that moved on, a cut level
    /// never used): a later draw that needs one compiles it on the owner, as without the bit.
    void ExpireLocked(std::vector<std::shared_ptr<Job>>& evicted) {
        const u64 frame = FastRender::g_counters.frames.load(std::memory_order_relaxed);
        for (auto it = pending.begin(); it != pending.end();) {
            const auto& job = *it->second;
            if (job.state.load(std::memory_order_acquire) == Job::Done &&
                job.done_frame + ExpireFrames < frame) {
                known.erase(job.hash);
                evicted.push_back(std::move(it->second));
                it = pending.erase(it);
                ++expired;
            } else {
                ++it;
            }
        }
    }

    /// Any thread: the compile threads, or the owner for a job it took back.
    void Compile(Job& job) {
        job.start_ns.store(AsyncDetail::NowNs(), std::memory_order_relaxed);
        running.fetch_add(1, std::memory_order_seq_cst);
        // Pairs with ReplaceShader: it bumps the generation, then waits for `running` to drain.
        if (job.generation != cache.programs_generation.load(std::memory_order_seq_cst)) {
            job.stale = true;
            ++stale;
            if constexpr (Common::Log::Enabled) {
                cache.compile_reasons.StaleJob(job.hash); // the owner's compile of it says so
            }
        } else {
            Build(job);
        }
        running.fetch_sub(1, std::memory_order_seq_cst);
        job.done_frame = FastRender::g_counters.frames.load(std::memory_order_relaxed);
        job.state.store(Job::Done, std::memory_order_release);
        job.state.notify_all();
        dirty.store(true, std::memory_order_relaxed);
    }

    void Build(Job& job) {
        std::array<ScratchDraw::Inputs, MaxShaderStages> views{};
        size_t count = 0;
        for (u32 stage = 0; stage < MaxShaderStages; ++stage) {
            if (job.infos[stage]) {
                views[count++] = job.inputs[stage].View();
            }
        }
        ScratchDraw::Scope pinned{{views.data(), count}};
        CompileTrace::Scope compile_trace(job.speculative ? "graphics_ahead" : "graphics_async",
                                          job.hash);
        LOG_INFO(Render_Vulkan, "Compiling graphics pipeline {:#x} ({}) why: {}", job.hash,
                 job.speculative ? "ahead of the draw" : "background",
                 cache.compile_reasons.Pipeline(job.key, job.hash, job.variant,
                                                !AsyncDetail::t_compile_thread, job.reason));
        // A rejection leaves the pipeline without a handle: the owner then compiles it itself
        // (and asserts there, as before, if the driver rejects it again).
        const bool tolerate = std::exchange(g_tolerate_pipeline_rejection, true);
        const u64 start = AsyncDetail::NowNs();
        auto pipeline = std::make_unique<GraphicsPipeline>(
            cache.instance, cache.scheduler, cache.desc_heap, cache.profile, job.key,
            *cache.pipeline_cache, job.infos, job.runtime_infos, job.fetch_shader, job.modules,
            job.sdata, false);
        job.compile_ns = AsyncDetail::NowNs() - start;
        g_tolerate_pipeline_rejection = tolerate;
        compile_ns.fetch_add(job.compile_ns, std::memory_order_relaxed);
        ++compiled;
        if (!pipeline->Handle()) {
            job.rejected = true;
            ++rejected;
            return;
        }
        job.pipeline = std::move(pipeline);
    }

    PipelineCache& cache;
    u32 thread_count{};
    std::vector<std::jthread> threads;
    std::atomic<bool> started{false};
    std::atomic<bool> stop{false};
    std::atomic<u32> wake{0};
    std::atomic<u32> running{0};
    // Owner jobs: the owner and the compile threads only; `urgent` is taken first (Submit's
    // `first`, Promote).
    std::mutex queue_mutex;
    std::deque<std::shared_ptr<Job>> queue;
    std::deque<std::shared_ptr<Job>> urgent;
    // Preparation-worker offers, handed over without a lock.
    std::array<std::atomic<Job*>, OfferSlots> offers{};
    // Worker hint: key hashes that have, or are getting, a pipeline (direct-mapped, lossy).
    std::array<std::atomic<u64>, SeenSlots> seen{};
    // The owner and the compile threads, never the idle-priority workers.
    std::mutex mutex;
    std::unordered_map<GraphicsPipelineKey, std::shared_ptr<Job>> pending;
    std::unordered_set<u64> known; // installed or pending key hashes
    // Owner only: bbport's skip rule.
    std::unordered_map<u64, AsyncDetail::TargetStreak> target_streaks;
    u64 last_target{};
    u64 last_target_frame = ~u64{0};
    u64 frame_seen = ~u64{0};
    u64 draws_at_start{};
    u64 last_frame_draws{};
    u64 report_frame{};
    // Stage A only: the age cap's one wait a frame (VrSkip).
    u64 aged_wait_frame = ~u64{0};
    u64 aged_held_frame = ~u64{0};
    u32 stall_log_budget = 64;
    // Statistics, cumulative (stall_max_ns, skip_max_ns: since the last line).
    std::atomic<bool> dirty{false};
    std::atomic<u64> ready{}, waited{}, wait_ns{}, saved_ns{}, sync_a{}, sync_b{}, sync_ns{},
        taken_back{}, skipped{}, owner_jobs{}, prebuilds{}, offered{}, offer_full{}, accepted{},
        duplicates{}, expired{}, compiled{}, compile_ns{}, stale{}, rejected{}, fallbacks{},
        verified{}, mismatches{}, aged{}, aged_held{}, promoted{}, cut_deferred{},
        motion_deferred{}, variant_jobs{}, aged_ns{}, veto_ns{}, stall_max_ns{}, skip_keys{},
        skip_max_ns{};
    std::array<std::atomic<u64>, PipelineSkip::NumVetoes> vetoes{};
};

void PipelineCache::InitAsyncPipelines() {
    async_pipelines = std::make_shared<AsyncPipelines>(*this);
    vr_skip_vetoes = NativeStereo::Enabled() && AsyncDetail::SkipMode();
    view_predict = NativeStereo::Enabled() && AsyncDetail::g_view_predict;
    if (vr_skip_vetoes || view_predict) {
        skip_target_views = std::make_unique<PipelineSkip::TargetViews>();
    }
    if (view_predict) {
        LOG_INFO(Render_Vulkan, "FAST_CPU_VIEW_PREDICT: on (Stage A and the preparation workers "
                                "select a draw into targets that are not eye pairs with view "
                                "mask 0)");
    }
}

u32 PipelineCache::PredictViewMask(const AmdGpu::Regs& regs, u32 mrt_mask, bool mrt_known) const {
    u32 colours = 0;
    auto facts = AsyncDetail::TargetFacts(regs, mrt_known ? mrt_mask : 0xFFu, &colours);
    facts.colours_unknown = !mrt_known && colours != 0;
    facts.noted = skip_target_views->Find(facts.targets);
    return PipelineSkip::PredictedViewMask(facts);
}

const GraphicsPipeline* PipelineCache::GetDrawPipeline(u32 view_mask, bool may_skip) {
    if (!view_predict || view_mask != 3) {
        return GetGraphicsPipeline(view_mask, may_skip);
    }
    // The fragment program's written colour targets (the key's mrt_mask, Info::mrt_mask) when it
    // was translated before: found the way RefreshGraphicsStages finds it, by its header hash.
    const auto& regs = ScratchDraw::Registers(liverpool);
    u32 mrt_mask = 0;
    bool mrt_known = true;
    const auto fs = static_cast<u32>(Shader::Stage::Fragment);
    if (regs.stage_enable.IsStageEnabled(fs)) {
        const auto* pgm = regs.ProgramForStage(fs);
        if (pgm && pgm->Address<u32*>()) {
            const auto it =
                program_cache.find(AmdGpu::SearchBinaryInfo(pgm->Address<u32*>()).shader_hash);
            mrt_known = it != program_cache.end();
            mrt_mask = mrt_known ? it->second->info.mrt_mask : 0u;
        }
    }
    if (PredictViewMask(regs, mrt_mask, mrt_known) != 0) {
        return GetGraphicsPipeline(3, may_skip);
    }
    CountViewPredict(ViewSelected);
    // A pipeline built or queued for it now is one whose multiview twin the old order built here
    // (ViewBuilt; SelectGraphicsPipeline, AsyncGraphicsPipeline).
    view_predicting = true;
    struct Reset {
        bool& flag;
        ~Reset() {
            flag = false;
        }
    } reset{view_predicting};
    return GetGraphicsPipeline(0, may_skip);
}

bool PipelineCache::AsyncPending(const AsyncInFlight& job) {
    return job && job->load(std::memory_order_acquire) != AsyncDetail::Job::Done;
}

void PipelineCache::NoteVrVariantDeferred(bool motion) {
    if (async_pipelines) {
        ++(motion ? async_pipelines->motion_deferred : async_pipelines->cut_deferred);
        async_pipelines->dirty.store(true, std::memory_order_relaxed);
    }
}

u64 PipelineCache::AsyncNowNs() {
    return AsyncDetail::NowNs();
}

void PipelineCache::NoteAsyncDrawSlow() {
    if (!async_pipelines || !instance.IsVertexInputDynamicState()) {
        return;
    }
    auto& async = *async_pipelines;
    async.Start();
    if (AsyncDetail::SkipMode()) {
        async.NoteDraw(ScratchDraw::Registers(liverpool));
    }
    async.MaybeReport();
}

bool PipelineCache::AsyncGraphicsPipeline(PipelineSelection& selected, AsyncWait wait,
                                          const GraphicsPipeline*& out, AsyncInFlight* in_flight) {
    using Job = AsyncDetail::Job;
    async_stall_reason = PipelineSkip::None; // not a skip candidate unless decided below
    if (!async_pipelines || !instance.IsVertexInputDynamicState()) {
        return false;
    }
    auto& async = *async_pipelines;
    async.Start();
    bool skip = wait == AsyncWait::MaySkip && AsyncDetail::SkipMode() && async.Skippable(selected);
    // A VR variant never waits: its draw goes on with the base pipeline until it is ready.
    const bool defer = wait == AsyncWait::Never;
    const auto job = async.Find(selected.graphics_key);
    if (skip && vr_skip_vetoes &&
        !(job && job->state.load(std::memory_order_acquire) == Job::Done)) {
        // Native stereo, only a draw that would be left out: its vetoes and the age cap.
        skip = async.VrSkip(selected, job, async_stall_reason);
    }
    if (!job) {
        if (!skip && !defer) {
            return false; // compiled here, as before
        }
        // bbport (BBV vk_pipeline_cache.cpp:567-596): start it, draw from the frame it is ready.
        const auto made = async.MakeJob(selected);
        if (skip) {
            made->first_skip_ns = AsyncNowNs();
            ++async.skip_keys;
            ++async.owner_jobs;
        } else {
            ++async.variant_jobs;
        }
        if (view_predicting) {
            CountViewPredict(ViewBuilt);
        }
        async.Submit(made, defer); // a VR variant first: a draw already goes without it
        async.NoteLeftOut(selected, skip);
        if (defer && in_flight != nullptr) {
            *in_flight = AsyncInFlight{made, &made->state};
        }
        selected.fetch_shader.reset();
        out = nullptr;
        return true;
    }
    u64 waited_ns = 0;
    bool here = false;
    if (job->state.load(std::memory_order_acquire) != Job::Done) {
        if (skip || defer) {
            if (skip && job->first_skip_ns == 0) {
                job->first_skip_ns = AsyncNowNs(); // an offer or a prebuild left out
                ++async.skip_keys;
            }
            if (defer) {
                async.Promote(job); // a prebuild or an offer a VR draw now goes without
                if (in_flight != nullptr) {
                    *in_flight = AsyncInFlight{job, &job->state};
                }
            }
            async.NoteLeftOut(selected, skip); // BBV vk_pipeline_cache.cpp:551-556
            out = nullptr;
            return true;
        }
        if (async.TakeBack(*job)) {
            // Nobody started it: compiling it here is never slower than waiting behind others.
            here = true;
            ++async.taken_back;
            async.Compile(*job);
        } else {
            // A pass that needs it now: the wait is compile time (BBV :557-563).
            const u64 start = AsyncNowNs();
            async.Await(*job);
            waited_ns = AsyncNowNs() - start;
            ++async.waited;
            async.wait_ns.fetch_add(waited_ns, std::memory_order_relaxed);
        }
        async.NoteStall(here ? job->compile_ns : waited_ns, async_stall_reason, job->hash);
        // Its reason was set before Done (none for a stale job: the compile here says why).
        compile_reasons.Stall(job->reason, here ? job->compile_ns : waited_ns);
    } else {
        ++async.ready;
    }
    async_stall_reason = PipelineSkip::None;
    async.Forget(selected.graphics_key);
    async.NoteBackIn(*job);
    async.dirty.store(true, std::memory_order_relaxed);
    if (here) {
        ++(FastRender::IsStageB() ? async.sync_b : async.sync_a);
        async.sync_ns.fetch_add(job->compile_ns, std::memory_order_relaxed);
    }
    if (!job->pipeline) {
        ++async.fallbacks; // stale generation or a driver rejection: compile here
        return false;
    }
    if (AsyncDetail::VerifyMode()) {
        ++async.verified;
        if (!AsyncPipelines::SameInputs(*job, selected)) {
            ++async.mismatches;
            LOG_ERROR(Render_Vulkan,
                      "FAST async_pipelines mismatch #{}: pipeline {:#x} ({}) was built from "
                      "other inputs than this draw's selection; compiled here instead",
                      async.mismatches.load(), job->hash,
                      job->speculative ? "preparation worker" : "owner job");
            return false;
        }
    }
    if (!here) {
        const u64 saved = job->compile_ns > waited_ns ? job->compile_ns - waited_ns : 0;
        async.saved_ns.fetch_add(saved, std::memory_order_relaxed);
    }
    out = async.Install(*job);
    selected.fetch_shader.reset(); // as after a compile here
    return true;
}

void PipelineCache::NoteSyncGraphicsPipeline(const GraphicsPipelineKey& key, u64 hash,
                                             u64 start_ns, const CompileReason::Tags& why) {
    // Why this compile here was not left out (AsyncGraphicsPipeline just decided it).
    const u32 reason = std::exchange(async_stall_reason, PipelineSkip::None);
    if (!async_pipelines || !async_pipelines->started.load(std::memory_order_relaxed)) {
        return;
    }
    auto& async = *async_pipelines;
    async.NoteInstalled(key, hash);
    if (start_ns != 0) {
        const u64 ns = AsyncNowNs() - start_ns;
        ++(FastRender::IsStageB() ? async.sync_b : async.sync_a);
        async.sync_ns.fetch_add(ns, std::memory_order_relaxed);
        async.NoteStall(ns, reason, hash);
        compile_reasons.Stall(why, ns);
        async.dirty.store(true, std::memory_order_relaxed);
    }
}

void PipelineCache::DrainAsyncPipelines() {
    if (!async_pipelines) {
        return;
    }
    auto& async = *async_pipelines;
    while (async.running.load(std::memory_order_seq_cst) != 0) {
        std::this_thread::yield();
    }
    std::scoped_lock lock{async.mutex};
    async.pending.clear();
}

void PipelineCache::PrebuildVrCutPipeline(const GraphicsPipeline* base, u32 cut, bool ahead) {
    const auto selection_lock = LockSelection();
    if (base == nullptr || cut == 0) {
        return;
    }
    // VrPushData: there is only the one variant (the rasterizer prebuilds it ahead of the
    // cutaway, and no other levels).
    cut = VrCutVariant(cut);
    const VrCutKey key{base, cut};
    if (vr_cut_pipelines.find(key) != vr_cut_pipelines.end()) {
        return;
    }
    if (!async_pipelines || !FastRender::On(AsyncPipelinesBit) ||
        !instance.IsVertexInputDynamicState()) {
        if (!ahead) {
            GetVrCutPipeline(base, cut); // as before
        }
        return;
    }
    if (!vr_cut_prebuilt.try_emplace(key, true).second) {
        return; // once per base pipeline and level; GetVrCutPipeline selects it when needed
    }
    // GetVrCutPipeline's conditions: a fragment program that does not write the sample mask.
    const Shader::Info* fs = nullptr;
    for (const auto* stage : base->GetStages()) {
        if (stage != nullptr && stage->l_stage == Shader::LogicalStage::Fragment) {
            fs = stage;
        }
    }
    if (fs == nullptr || fs->stores.Get(Shader::IR::Attribute::SampleMask)) {
        return;
    }
    auto& async = *async_pipelines;
    async.Start();
    // The shader permutation with the cutaway is still translated here; only the pipeline
    // creation goes to the background.
    selection.regs = &ScratchDraw::Registers(liverpool);
    selection.vr_fs_cut = cut;
    const bool selected = RefreshGraphicsKey(selection, base->GetGraphicsKey().probe_view_mask);
    selection.vr_fs_cut = 0;
    if (!selected) {
        return;
    }
    if (const auto found = graphics_pipelines.find(selection.graphics_key);
        found != graphics_pipelines.end() || async.Find(selection.graphics_key)) {
        return;
    }
    const auto job = async.MakeJob(selection);
    job->variant = CompileReason::Variant::CutPrebuild; // vr_fs_cut is back to 0 by now
    async.Submit(job, ahead); // ahead: the body's next colour draws need it
    ++async.prebuilds;
    async.dirty.store(true, std::memory_order_relaxed);
    selection.fetch_shader.reset();
}

void PipelineCache::KickPreparedPipeline(const PrepGraphicsContext& context, u64 key_hash) {
    auto* async = async_pipelines.get();
    if (!async || !async->started.load(std::memory_order_acquire) || async->Seen(key_hash)) {
        return;
    }
    const auto& selected = context.selection;
    // Without a fragment program the constructor also reads the fragment runtime info left by
    // the selection before (the clip-distance discard shader): only the owner builds those.
    if (!selected.graphics_key.stage_hashes[static_cast<u32>(Shader::LogicalStage::Fragment)]) {
        return;
    }
    auto job = std::make_unique<AsyncDetail::Job>();
    for (u32 stage = 0; stage < MaxShaderStages; ++stage) {
        const auto* identity = context.identity[stage];
        if (identity == nullptr) {
            continue;
        }
        const auto* input = context.inputs[stage];
        if (input == nullptr || !selected.modules[stage]) {
            return; // a module the warm-up dropped, or no owned inputs
        }
        job->infos[stage] = identity; // the pipeline keeps the shared Info, as the owner's does
        job->modules[stage] = selected.modules[stage];
        job->inputs[stage].Capture(identity, input->user, input->info.flattened_ud_buf,
                                   input->info.pgm_base);
    }
    if (job->infos[static_cast<u32>(Shader::LogicalStage::Fragment)] == nullptr) {
        return;
    }
    job->key = selected.graphics_key;
    job->hash = key_hash;
    job->runtime_infos = selected.runtime_infos;
    job->fetch_shader = selected.fetch_shader;
    job->generation = context.generation;
    job->speculative = true;
    job->register_data = true; // never a probe or motion variant
    if (!async->Offer(job)) {
        ++async->offer_full;
        return;
    }
    async->MarkSeen(key_hash);
    ++async->offered;
    // A worker selects mask 0 in native stereo only by FAST_CPU_VIEW_PREDICT: its twin with 3 is
    // the one the old order offered here.
    if (view_predict && selected.graphics_key.probe_view_mask == 0) {
        CountViewPredict(ViewQueued);
    }
}

} // namespace Vulkan
