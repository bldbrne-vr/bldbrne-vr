#include "video_core/native_stereo/push_state_memo.h"
// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include "video_core/fast_render.h"

#include <array>
#include <atomic>
#include <chrono>
#include <immintrin.h>
#include <condition_variable>
#include <memory>
#include <optional>
#include <mutex>
#include <source_location>
#include <thread>
#include <unordered_map>
#include <queue>

#include "common/unique_function.h"
#include "video_core/amdgpu/regs_color.h"
#include "video_core/amdgpu/regs_primitive.h"
#include "video_core/renderer_vulkan/vk_master_semaphore.h"
#include "video_core/renderer_vulkan/vk_resource_pool.h"
#include "video_core/renderer_vulkan/vk_recorder.h"
#include "video_core/renderer_vulkan/vk_signal_policy.h"

namespace tracy {
class VkCtxScope;
}

namespace Vulkan {

class Instance;
struct FoveationCache;
struct PassBreakStats; // FAST_PASS_BREAK_STATS (vk_scheduler.cpp)

struct RenderAttachment {
    vk::ImageView image_view;
    vk::ImageLayout image_layout;
    std::array<u32, 4> clear_value;
    union {
        u32 is_clear;
        struct {
            bool has_depth;
            bool depth_clear;
            bool has_stencil;
            bool stencil_clear;
        };
    };
};
static_assert(std::has_unique_object_representations_v<RenderAttachment>);

struct RenderState {
    std::array<RenderAttachment, 8> color_attachments;
    RenderAttachment depth_stencil_attachment;
    u16 width;
    u16 height;
    u16 num_layers;
    u16 num_color_attachments;
    u32 probe_view_mask{};
    u32 probe_reserved{};

    bool operator==(const RenderState& other) const noexcept {
        return std::memcmp(this, &other, sizeof(RenderState)) == 0;
    }
};
static_assert(std::has_unique_object_representations_v<RenderState>);

struct SubmitInfo {
    // 4: three the callers may add, one for the sparse binds (Scheduler::SetSparseBinds).
    std::array<vk::Semaphore, 4> wait_semas;
    std::array<u64, 4> wait_ticks;
    std::array<vk::Semaphore, 3> signal_semas;
    std::array<u64, 3> signal_ticks;
    vk::Fence fence;
    u32 num_wait_semas;
    u32 num_signal_semas;

    void AddWait(vk::Semaphore semaphore, u64 tick = 1) {
        wait_semas[num_wait_semas] = semaphore;
        wait_ticks[num_wait_semas++] = tick;
    }

    void AddSignal(vk::Semaphore semaphore, u64 tick = 1) {
        signal_semas[num_signal_semas] = semaphore;
        signal_ticks[num_signal_semas++] = tick;
    }

    void AddSignal(vk::Fence fence) {
        this->fence = fence;
    }
};

using Viewports = boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS>;
using Scissors = boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS>;
using ColorWriteMasks = std::array<vk::ColorComponentFlags, AmdGpu::NUM_COLOR_BUFFERS>;
struct StencilOps {
    vk::StencilOp fail_op{};
    vk::StencilOp pass_op{};
    vk::StencilOp depth_fail_op{};
    vk::CompareOp compare_op{};

    bool operator==(const StencilOps& other) const {
        return fail_op == other.fail_op && pass_op == other.pass_op &&
               depth_fail_op == other.depth_fail_op && compare_op == other.compare_op;
    }
};
/// FAST_CPU_STAGEB_SPLIT=2 asked for (the rasterizer starts fast:StageB2: StartSplitThread).
bool StageBSplitThreadRequested();

struct DynamicState {
    struct {
        bool viewports : 1;
        bool scissors : 1;

        bool depth_test_enabled : 1;
        bool depth_write_enabled : 1;
        bool depth_compare_op : 1;

        bool depth_bounds_test_enabled : 1;
        bool depth_bounds : 1;

        bool depth_bias_enabled : 1;
        bool depth_bias : 1;

        bool stencil_test_enabled : 1;
        bool stencil_front_ops : 1;
        bool stencil_front_reference : 1;
        bool stencil_front_write_mask : 1;
        bool stencil_front_compare_mask : 1;
        bool stencil_back_ops : 1;
        bool stencil_back_reference : 1;
        bool stencil_back_write_mask : 1;
        bool stencil_back_compare_mask : 1;

        bool primitive_restart_enable : 1;
        bool rasterizer_discard_enable : 1;
        bool cull_mode : 1;
        bool front_face : 1;

        bool blend_constants : 1;
        bool color_write_masks : 1;
        bool line_width : 1;
        bool feedback_loop_enabled : 1;
    } dirty_state{};

    Viewports viewports{};
    Scissors scissors{};

    bool depth_test_enabled{};
    bool depth_write_enabled{};
    vk::CompareOp depth_compare_op{};

    bool depth_bounds_test_enabled{};
    float depth_bounds_min{};
    float depth_bounds_max{};

    bool depth_bias_enabled{};
    float depth_bias_constant{};
    float depth_bias_clamp{};
    float depth_bias_slope{};

    bool stencil_test_enabled{};
    StencilOps stencil_front_ops{};
    u32 stencil_front_reference{};
    u32 stencil_front_write_mask{};
    u32 stencil_front_compare_mask{};
    StencilOps stencil_back_ops{};
    u32 stencil_back_reference{};
    u32 stencil_back_write_mask{};
    u32 stencil_back_compare_mask{};

    bool primitive_restart_enable{};
    bool rasterizer_discard_enable{};
    vk::CullModeFlags cull_mode{};
    vk::FrontFace front_face{};

    std::array<float, 4> blend_constants{};
    ColorWriteMasks color_write_masks{};
    float line_width{};
    bool feedback_loop_enabled{};

    /// Commits the dynamic state to the provided command buffer.
    void Commit(const Instance& instance, const RecorderProxy& cmdbuf);

    /// Invalidates all dynamic state to be flushed into the next command buffer.
    void Invalidate() {
        std::memset(&dirty_state, 0xFF, sizeof(dirty_state));
    }

    void SetViewports(const Viewports& viewports_) {
        if (!std::ranges::equal(viewports, viewports_)) {
            viewports = viewports_;
            dirty_state.viewports = true;
        }
    }

    void SetScissors(const Scissors& scissors_) {
        if (!std::ranges::equal(scissors, scissors_)) {
            scissors = scissors_;
            dirty_state.scissors = true;
        }
    }

    void SetDepthTestEnabled(const bool enabled) {
        if (depth_test_enabled != enabled) {
            depth_test_enabled = enabled;
            dirty_state.depth_test_enabled = true;
        }
    }

    void SetDepthWriteEnabled(const bool enabled) {
        if (depth_write_enabled != enabled) {
            depth_write_enabled = enabled;
            dirty_state.depth_write_enabled = true;
        }
    }

    void SetDepthCompareOp(const vk::CompareOp compare_op) {
        if (depth_compare_op != compare_op) {
            depth_compare_op = compare_op;
            dirty_state.depth_compare_op = true;
        }
    }

    void SetDepthBoundsTestEnabled(const bool enabled) {
        if (depth_bounds_test_enabled != enabled) {
            depth_bounds_test_enabled = enabled;
            dirty_state.depth_bounds_test_enabled = true;
        }
    }

    void SetDepthBounds(const float min, const float max) {
        if (depth_bounds_min != min || depth_bounds_max != max) {
            depth_bounds_min = min;
            depth_bounds_max = max;
            dirty_state.depth_bounds = true;
        }
    }

    void SetDepthBiasEnabled(const bool enabled) {
        if (depth_bias_enabled != enabled) {
            depth_bias_enabled = enabled;
            dirty_state.depth_bias_enabled = true;
        }
    }

    void SetDepthBias(const float constant, const float clamp, const float slope) {
        if (depth_bias_constant != constant || depth_bias_clamp != clamp ||
            depth_bias_slope != slope) {
            depth_bias_constant = constant;
            depth_bias_clamp = clamp;
            depth_bias_slope = slope;
            dirty_state.depth_bias = true;
        }
    }

    void SetStencilTestEnabled(const bool enabled) {
        if (stencil_test_enabled != enabled) {
            stencil_test_enabled = enabled;
            dirty_state.stencil_test_enabled = true;
        }
    }

    void SetStencilOps(const StencilOps& front_ops, const StencilOps& back_ops) {
        if (stencil_front_ops != front_ops) {
            stencil_front_ops = front_ops;
            dirty_state.stencil_front_ops = true;
        }
        if (stencil_back_ops != back_ops) {
            stencil_back_ops = back_ops;
            dirty_state.stencil_back_ops = true;
        }
    }

    void SetStencilReferences(const u32 front_reference, const u32 back_reference) {
        if (stencil_front_reference != front_reference) {
            stencil_front_reference = front_reference;
            dirty_state.stencil_front_reference = true;
        }
        if (stencil_back_reference != back_reference) {
            stencil_back_reference = back_reference;
            dirty_state.stencil_back_reference = true;
        }
    }

    void SetStencilWriteMasks(const u32 front_write_mask, const u32 back_write_mask) {
        if (stencil_front_write_mask != front_write_mask) {
            stencil_front_write_mask = front_write_mask;
            dirty_state.stencil_front_write_mask = true;
        }
        if (stencil_back_write_mask != back_write_mask) {
            stencil_back_write_mask = back_write_mask;
            dirty_state.stencil_back_write_mask = true;
        }
    }

    void SetStencilCompareMasks(const u32 front_compare_mask, const u32 back_compare_mask) {
        if (stencil_front_compare_mask != front_compare_mask) {
            stencil_front_compare_mask = front_compare_mask;
            dirty_state.stencil_front_compare_mask = true;
        }
        if (stencil_back_compare_mask != back_compare_mask) {
            stencil_back_compare_mask = back_compare_mask;
            dirty_state.stencil_back_compare_mask = true;
        }
    }

    void SetPrimitiveRestartEnabled(const bool enabled) {
        if (primitive_restart_enable != enabled) {
            primitive_restart_enable = enabled;
            dirty_state.primitive_restart_enable = true;
        }
    }

    void SetCullMode(const vk::CullModeFlags cull_mode_) {
        if (cull_mode != cull_mode_) {
            cull_mode = cull_mode_;
            dirty_state.cull_mode = true;
        }
    }

    void SetFrontFace(const vk::FrontFace front_face_) {
        if (front_face != front_face_) {
            front_face = front_face_;
            dirty_state.front_face = true;
        }
    }

    void SetBlendConstants(const std::array<float, 4> blend_constants_) {
        if (blend_constants != blend_constants_) {
            blend_constants = blend_constants_;
            dirty_state.blend_constants = true;
        }
    }

    void SetRasterizerDiscardEnabled(const bool enabled) {
        if (rasterizer_discard_enable != enabled) {
            rasterizer_discard_enable = enabled;
            dirty_state.rasterizer_discard_enable = true;
        }
    }

    void SetColorWriteMasks(const ColorWriteMasks& color_write_masks_) {
        if (!std::ranges::equal(color_write_masks, color_write_masks_)) {
            color_write_masks = color_write_masks_;
            dirty_state.color_write_masks = true;
        }
    }

    void SetLineWidth(const float width) {
        if (line_width != width) {
            line_width = width;
            dirty_state.line_width = true;
        }
    }

    void SetAttachmentFeedbackLoopEnabled(const bool enabled) {
        if (feedback_loop_enabled != enabled) {
            feedback_loop_enabled = enabled;
            dirty_state.feedback_loop_enabled = true;
        }
    }

    // FAST_CPU_STAGEB_SPLIT (stage 2): every value of `desired` through the
    // setters, so exactly what differs from this tracker (the command buffer's state) is dirty.
    void SetFrom(const DynamicState& desired) {
        SetViewports(desired.viewports);
        SetScissors(desired.scissors);
        SetDepthTestEnabled(desired.depth_test_enabled);
        SetDepthWriteEnabled(desired.depth_write_enabled);
        SetDepthCompareOp(desired.depth_compare_op);
        SetDepthBoundsTestEnabled(desired.depth_bounds_test_enabled);
        SetDepthBounds(desired.depth_bounds_min, desired.depth_bounds_max);
        SetDepthBiasEnabled(desired.depth_bias_enabled);
        SetDepthBias(desired.depth_bias_constant, desired.depth_bias_clamp,
                     desired.depth_bias_slope);
        SetStencilTestEnabled(desired.stencil_test_enabled);
        SetStencilOps(desired.stencil_front_ops, desired.stencil_back_ops);
        SetStencilReferences(desired.stencil_front_reference, desired.stencil_back_reference);
        SetStencilWriteMasks(desired.stencil_front_write_mask, desired.stencil_back_write_mask);
        SetStencilCompareMasks(desired.stencil_front_compare_mask,
                               desired.stencil_back_compare_mask);
        SetPrimitiveRestartEnabled(desired.primitive_restart_enable);
        SetRasterizerDiscardEnabled(desired.rasterizer_discard_enable);
        SetCullMode(desired.cull_mode);
        SetFrontFace(desired.front_face);
        SetBlendConstants(desired.blend_constants);
        SetColorWriteMasks(desired.color_write_masks);
        SetLineWidth(desired.line_width);
        SetAttachmentFeedbackLoopEnabled(desired.feedback_loop_enabled);
    }
};

enum class GuestSignalValue : u32 { Literal, GpuClock, PerfCounter };

// FAST_CPU_ASYNC_IDLE (on unless '0'; read once at start-up): Liverpool's ordered retirement of a
// guest submission and its GPU-idle task no longer make stage B wait for the recorder. B submits
// as before and queues the rest to the recorder, which decrements num_submits and raises the
// GpuIdle interrupt when its replay reaches them: after the submission and the guest signals it
// writes, as B's drain made sure before. Sync points then submit only for guest signals encoded
// since the last submission (SignalPolicy::Precise), so a submission still queued is not followed
// by a second one. The same commands and guest writes in the same order; only the thread and the
// moment change (B goes on with OnSubmit and the next submission meanwhile).
inline const bool g_async_idle_switch = [] {
    const char* value = std::getenv("FAST_CPU_ASYNC_IDLE");
    return !value || value[0] != '0';
}();
// FAST_CPU_ASYNC_IDLE_VERIFY=1: the old path (B drains), with the recorder checking at the points
// the async path would act that every signal encoded before is written, comparing its GPU-idle
// decision with B's, and checking SignalPolicy::SubmitOrderHolds where Precise would decide (not
// while FAST_CPU_SIGNAL_AT_REPLAY writes the signals: g_signal_order_check).
inline const bool g_async_idle_verify = [] {
    const char* value = std::getenv("FAST_CPU_ASYNC_IDLE_VERIFY");
    return value && value[0] == '1';
}();
inline const bool g_async_idle = g_async_idle_switch && !g_async_idle_verify;
// FAST_CPU_SIGNAL_AT_REPLAY (on by default, '0' off; read once at start-up; bbport's RecorderFences): a
// guest signal is written when the recorder's replay reaches it, after the host copies queued
// before it, instead of after the next submission. A guest-visible write on B (WriteData, a
// query, a CE dump) then waits only until the replay has written the signals before it, with no
// submission; the mid-frame cadence (FAST bit 62) may defer cuts that carry signals. The values
// and their order among themselves and B's writes are unchanged, but the guest sees a label
// before the work ahead of it is submitted (as upstream's direct writes and bbport do). On since
// 2026-10-23: FAST_CPU_SIGNAL_AT_REPLAY_VERIFY found no label that would end with another value
// (0 of 1.69 million in a 60 s run; same-submission writes to one label told apart), +2.2% FPS.
inline const bool g_signal_at_replay_switch = [] {
    const char* value = std::getenv("FAST_CPU_SIGNAL_AT_REPLAY");
    return !value || value[0] != '0';
}();
// FAST_CPU_SIGNAL_AT_REPLAY_VERIFY=1: signals are written after the submission as before; the
// replay also reads each target when it reaches the signal's packet (where the switch would write
// it) and, before writing it, checks that nothing else wrote those bytes in between (else the
// earlier write would have been overwritten: a mismatch). Counts the submissions and waits of
// B's guest-write ordering the switch would leave out.
inline const bool g_signal_at_replay_verify = [] {
    const char* value = std::getenv("FAST_CPU_SIGNAL_AT_REPLAY_VERIFY");
    return value && value[0] == '1';
}();
inline const bool g_signal_at_replay = g_signal_at_replay_switch && !g_signal_at_replay_verify;
// What sync points submit for (SignalPolicy::Mode): Old unless one of the two switches above acts.
inline const SignalPolicy::Mode g_signal_mode = g_signal_at_replay ? SignalPolicy::Mode::AtReplay
                                                : g_async_idle    ? SignalPolicy::Mode::Precise
                                                                  : SignalPolicy::Mode::Old;
// FAST_CPU_ASYNC_IDLE_VERIFY's check of SignalPolicy::SubmitOrderHolds (Scheduler::
// CheckSignalOrder): only while signals are written at a submission's replay. With
// FAST_CPU_SIGNAL_AT_REPLAY acting the replay writes a signal when it reaches its packet, so one
// encoded since the last submission and written already is that mode's design, not a mismatch.
inline const bool g_signal_order_check =
    g_async_idle_verify && g_signal_mode != SignalPolicy::Mode::AtReplay;
// FAST_CPU_TASK_PUBLISH (read once at start-up): the recorder's open batch is published before a
// sync point's submission and drain (Liverpool::SyncRecorder, FlushGuestSignals), so the replay
// works through it while the producer encodes the submission (the stream shadow flush, the pass
// end) instead of starting at the drain. '2' also publishes before every ordered task stage B
// runs (DrawPipe's task hook): sooner replay, but a wake of a sleeping recorder per task. '0' off.
// Publication only: the replay order is unchanged.
inline const unsigned g_task_publish = [] {
    const char* value = std::getenv("FAST_CPU_TASK_PUBLISH");
    return !value ? 1u : value[0] == '0' ? 0u : value[0] == '2' ? 2u : 1u;
}();

class Scheduler {
public:
    explicit Scheduler(const Instance& instance, bool allow_recorder = false);
    ~Scheduler();

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    void Flush(SubmitInfo& info);

    /// Sends the current execution context to the GPU
    /// and increments the scheduler timeline semaphore.
    void Flush();

    /// Flush() for the guest's frame-end (SubmitDone) task. With FAST bit 56 (SkipEmptyFinish,
    /// vk_scheduler.cpp) it submits nothing while the command buffer holds no more than the GPU
    /// pass profiler's resume stamps and no guest signal or host copy needs a submission; the
    /// open tick then goes out with the next submission (at the latest the next frame end's).
    /// Bit clear: exactly Flush().
    void FlushFrameEnd();

    /// Sends the current execution context to the GPU and waits for it to complete.
    void Finish();

    /// bbport: retire submitted work without ending the partially recorded command buffer.
    /// Covers this scheduler's game queue only, not a separate OpenXR/SteamVR queue.
    void WaitSubmitted();

    /// Waits for the given tick to trigger on the GPU.
    void Wait(u64 tick);

    /// Attempts to execute operations whose tick the GPU has caught up with: for about
    /// FAST_PENDING_OPS_BUDGET_US at a time (the rest at the next call), all of them with `all`.
    void PopPendingOperations(bool force_poll = false, bool all = false);

    /// Starts a new rendering scope with provided state.
    void BeginRendering(const RenderState& new_state);

    /// Ends current rendering scope. `site` names the caller in FAST_PASS_BREAK_STATS.
    void EndRendering(std::source_location site = std::source_location::current());

    /// Returns the current render state (as begun; a pass continued by bit 43 keeps its own
    /// attachments, see Continues).
    const RenderState& GetRenderState() const {
        return render_state;
    }

    /// True while a render pass is open that BeginRendering(state) would continue: the same
    /// state, or (FAST bit 43, vk_pass_breaks.h) one that differs only by consumed clears or by
    /// fewer colour attachments. Plain equality when the bit is off.
    [[nodiscard]] bool Continues(const RenderState& state) const;

    /// FAST_PASS_BREAK_STATS: Pipeline::BindResources is about to end the open pass for buffer
    /// barriers; `read_after_read` when every one of them only orders a read after a read.
    void NoteBarrierBreak(bool read_after_read);

    /// Runs right before every submit of this scheduler (the stream buffer's shadow flush);
    /// an empty function removes it.
    void SetPreSubmitHook(Common::UniqueFunction<void>&& hook) {
        pre_submit_hook = std::move(hook);
    }

    /// Sparse buffers (buffer_cache/sparse_pool.h): `submit(queue)` sends the binds queued so far
    /// in one vkQueueBindSparse on this scheduler's queue, under the queue lock, right before each
    /// submit, and returns the value of `timeline` the submit then waits for (0: none yet).
    void SetSparseBinds(std::function<u64(vk::Queue)> submit, vk::Semaphore timeline) {
        sparse_binds = std::move(submit);
        sparse_timeline = timeline;
    }

    /// VR_GPU_PASSES (profiling): times one piece of work outside a render pass (a game
    /// dispatch), summed under `key` in the `VR gpu passes:` line. No-ops otherwise.
    bool GpuCategoryBegin(u64 key); // non-nesting
    void GpuWorkBegin(u64 key, u32 x, u32 y);
    /// A GPU crash marker (vk_gpu_crash.h, while its markers are on) at this point of the
    /// recorded commands: into the recorder Commands() would use, replayed as a checkpoint.
    void GpuCheckpoint(u8 kind, u64 a, u32 x, u32 y, u32 extra = 0);
    void GpuWorkEnd();

    /// True while a render pass (dynamic rendering scope) is open.
    [[nodiscard]] bool IsRendering() const noexcept {
        return is_rendering;
    }

    /// The copy sink (vk_shader_hle.cpp CopySink), while copies wait: run right after a pass ends
    /// (EndRenderingNow), before anything else is recorded. Null: nothing waits.
    void SetPassEndHook(void (*hook)(Scheduler&)) noexcept {
        pass_end_hook = hook;
    }

    /// No foveation for the passes begun while set: the game's UI after the composite (menus
    /// merged into the game's pass came out coarse outside the middle, 2026-10-24). Setting it
    /// ends an open foveated pass, so the UI draws begin a pass of their own.
    void BlockFoveation(bool blocked);

    /// The pass a draw is about to begin is shaded at full rate at least out to `inner` of the
    /// way from the eye's axis (0: the foveation's own zones). The lamp shadows' mask pass
    /// (light_shadow.h MaskFoveaInner); set before every BeginRendering of a game draw.
    void SetFoveationInnerFloor(float inner) {
        foveation_inner_floor = inner;
    }

    /// Dynamic resolution (core/vr/s1_dynamic_resolution.h): the percent each side of the view
    /// is drawn smaller in the passes about to begin (0: full scale). Their foveation map's zones
    /// follow the view (vr_foveation.h Map scale), so a drop also shades fewer full-rate pixels.
    /// Set by the rasterizer at the frame's first scene draw, 0 once the scene is reconstructed.
    void SetFoveationDrop(u32 drop) {
        foveation_drop = drop < 100 ? drop : 0;
    }
    /// The drop the stereo passes' rate maps follow now (0 with VR_FOVEATION_DRS=0): what any pass
    /// that must match the mask's full-rate zone (vr_shadow_edges.comp FoveaAt) has to use.
    [[nodiscard]] u32 FoveationDrop() const;

    /// DIBR (VR_FOVEATED_DIBR, on by default): the size of its mono scene, whose single-view
    /// passes then take foveation too. Set at each DIBR composite; {} = none.
    void SetMonoFoveation(vk::Extent2D extent);

    /// Native stereo: the eye's render size (the world draws'). Once known, only both-eye passes
    /// of that size or half of it take foveation: a 4096x4096 shadow atlas drawn for both eyes is
    /// not in screen space, so its coarse corners were wherever the eye's would be (2026-10-24).
    void SetFoveationEye(u32 width, u32 height) {
        foveation_eye = {width, height};
    }

    /// Whether BeginRendering(state) begins a new pass (bbport vk_scheduler.h:964).
    [[nodiscard]] bool WillBeginRendering(const RenderState& state) const noexcept {
        return !(is_rendering && render_state == state);
    }

    /// Render passes begun so far (a continued one not counted; recording owner only, like
    /// render_state). While it and IsRendering() hold, the open pass is the one begun then
    /// (FAST_GPU_PASS_MERGE's begin-state memo, vk_pass_merge.h).
    [[nodiscard]] u64 PassBegins() const noexcept {
        return pass_begins;
    }

    /// How long the current command buffer has been recording (vr.ini gpu_submit_ms).
    [[nodiscard]] std::chrono::steady_clock::duration RecordingTime() const noexcept {
        return std::chrono::steady_clock::now() - cmdbuf_begin_time;
    }

    /// FAST bit 62 (Rasterizer::MaybeSubmitMidFrame), producer only: guest signals encoded into
    /// the open command buffer complete only after its submission, so a mid-frame cut that
    /// carries one is never deferred.
    [[nodiscard]] bool SignalsSinceSubmit() const noexcept {
        return signals_encoded != signals_at_submit;
    }
    /// FAST bit 62: the mid-frame cadence deferred a cut in the open command buffer. Wait()
    /// counts the forced flushes of such a tick (a CPU waiter the old cadence may have served
    /// sooner); they still flush, so nothing waits for more than one submission.
    void NoteDeferredCut() noexcept {
        deferred_cut_tick = CurrentTick();
    }
    /// Submissions of this scheduler so far (producer only; the 'FAST submit_cadence' line).
    [[nodiscard]] u64 SubmitCount() const noexcept {
        return submit_count;
    }
    /// FAST bit 62's line: Wait() calls that had to flush the open tick, and of them those whose
    /// tick held a deferred mid-frame cut, since the last call.
    struct OpenTickWaits {
        u64 flushes, deferred;
    };
    OpenTickWaits TakeOpenTickWaits() noexcept {
        const OpenTickWaits waits{open_tick_flushes, deferred_tick_flushes};
        open_tick_flushes = deferred_tick_flushes = 0;
        return waits;
    }

    /// Returns the current pipeline dynamic state tracking.
    DynamicState& GetDynamicState() {
        // FAST_CPU_STAGEB_SPLIT: a draw's values computed into its own state (SetDesiredDynamic).
        if (dynamic_desired)
            return *dynamic_desired;
        SplitSync("dynamic_state");
        return dynamic_state;
    }
    /// FAST_CPU_STAGEB_SPLIT: GetDynamicState() returns `desired` until reset with nullptr.
    void SetDesiredDynamic(DynamicState* desired) noexcept {
        dynamic_desired = desired;
    }
    /// The command buffer's dynamic state, whatever SetDesiredDynamic says.
    DynamicState& RingDynamicState() {
        SplitSync("dynamic_state");
        return dynamic_state;
    }

    // Recording-owned state follows the command buffer when recording changes threads.
    ScratchPush::Memo& PushStateMemo() {
        SplitSync("push_memo");
        return push_state_memo;
    }

    /// Returns the current command buffer.
    vk::CommandBuffer CommandBuffer(std::source_location site = std::source_location::current()) {
        FastRender::AssertRecordingAccess();
        ++command_uses;
        return Main().Raw("raw_buffer", site);
    }
    RecorderProxy Commands(std::source_location site = std::source_location::current()) {
        FastRender::AssertRecordingAccess();
        // B2 exists only with the split thread: the member first, the thread-local flag only then.
        if (split_threaded && t_on_stageb2) [[unlikely]] {
            return {recorder, site}; // FAST_CPU_STAGEB_SPLIT=2: B2 encodes into the ring
        }
        ++command_uses;
        if (blob_open) {
            return {*blob, site};
        }
        SplitSync("commands");
        return {recorder, site};
    }
    /// FAST_CPU_STAGEB_SPLIT (stage 1; '1' on, read once
    /// at start-up): while a resolve blob is open, Commands() collects into it; it goes into the
    /// ring at CloseResolveBlob, or first when anything here uses the ring directly (a submission,
    /// a drain, a guest signal, a raw borrow). Recording owner only.
    /// FAST_CPU_STAGEB_SPLIT is on (the draws use the resolve blob and the split halves).
    bool StageBSplit() const noexcept {
        return blob_recorder != nullptr;
    }
    void OpenResolveBlob() {
        if (blob_recorder && !blob_open) {
            if (split_threaded) {
                SplitAcquire();
            }
            blob_open = true;
        }
    }
    void CloseResolveBlob() {
        if (blob_open) {
            if (split_threaded) {
                SplitSubmit(false); // what it holds goes to B2 as a job without a draw
                return;
            }
            Main();
            blob_open = false;
        }
    }

    /// FAST_CPU_STAGEB_SPLIT=2 (stage 3): the encode half of the plain draws on
    /// fast:StageB2. B1 (stage B) fills the job of slot SplitSlot() while its resolve blob is
    /// open and hands both over with SplitSubmit(true); B2 appends the blob to the ring, then
    /// calls `encode(context, slot)`. B2 alone uses the ring, the dynamic-state tracker and the
    /// push memo while jobs are queued: any other use (B1 outside an open blob, stage A, a
    /// submission) waits until B2 has finished them (SplitSync), then has them to itself.
    static constexpr u32 SplitSlots = 8;
    using SplitEncode = void (*)(void* context, u32 slot);
    void StartSplitThread(SplitEncode encode, void* context);
    bool SplitThreaded() const noexcept {
        return split_threaded;
    }
    u32 SplitSlot() const noexcept {
        return u32(split_head.load(std::memory_order_relaxed) % SplitSlots);
    }
    /// B1, blob open: hands the blob (and with `draw` the slot's job) to B2; the blob is closed.
    void SplitSubmit(bool draw);
    /// Waits until B2 has encoded every queued job (no-op on B2 and without the thread).
    void SplitSync(std::string_view reason) {
        if (split_threaded && !t_on_stageb2 &&
            split_head.load(std::memory_order_relaxed) !=
                split_tail.load(std::memory_order_acquire)) [[unlikely]] {
            SplitWait(reason);
        }
    }
    class ResolveBlob {
    public:
        explicit ResolveBlob(Scheduler& scheduler) : scheduler{scheduler} {
            scheduler.OpenResolveBlob();
        }
        ~ResolveBlob() {
            scheduler.CloseResolveBlob();
        }
        void Close() {
            scheduler.CloseResolveBlob();
        }
        ResolveBlob(const ResolveBlob&) = delete;
        ResolveBlob& operator=(const ResolveBlob&) = delete;

    private:
        Scheduler& scheduler;
    };
    /// How often Commands() or CommandBuffer() was taken (recording owner only, like the
    /// recorder). Code that leaves it and CurrentTick() unchanged recorded no command (profiler
    /// timestamps aside, which go to the recorder directly): FAST bit 57 (vk_shader_hle.cpp)
    /// checks that the copy shader's buffer lookups recorded nothing.
    [[nodiscard]] u64 CommandUses() const noexcept {
        return command_uses;
    }
    /// Hands a CommandBuffer() borrow back to the recorder (CommandRecorder::ReleaseRaw). The
    /// caller must not use any raw handle of the current command buffer after this.
    void ReleaseCommandBuffer() {
        Main().ReleaseRaw();
    }
    void FlushRecorder(std::string_view reason) { Main().Drain(reason); }
    bool RecorderThreaded() const noexcept {
        return recorder.Threaded();
    }
#ifdef _WIN32
    void SetStageBThread(void* thread) { recorder.SetStageBThread(thread); }
#endif
    /// VR_FRAME_MATCH (diagnostic): the main recorder, read-only.
    const CommandRecorder& RecorderView() const noexcept {
        return recorder;
    }
    /// False while the recorder is off or a raw handle is borrowed: proxy calls then record
    /// directly into the command buffer (FAST bit 41's statistics).
    bool RecorderEncoding() const noexcept {
        return recorder.Enabled();
    }
    /// FAST_GPU_PASS_MERGE_VERIFY (vk_pass_merge.h), recording owner: the Vulkan command packets
    /// encoded so far (control packets excluded) and the raw borrows; while neither moves, nothing
    /// was recorded through the recorder (meaningful only while RecorderEncoding()).
    [[nodiscard]] u64 RecorderCommandPackets() const noexcept {
        const_cast<Scheduler*>(this)->SplitSync("packet_count");
        return recorder.CommandPackets() + recorder.RawBorrows() +
               (blob_open ? blob->CommandPackets() : 0);
    }
    void RecordFrameMark();
    /// FAST_CPU_TASK_PUBLISH: hands the recorder what is encoded so far (producer only).
    void PublishRecorder() {
        Main().Publish();
    }
    /// FAST_CPU_ASYNC_IDLE (producer only): the replay calls `task(context, argument)` when it
    /// reaches this point, after every packet encoded before (the submission before it included:
    /// its guest signals are written by then). The task must not record commands.
    void QueueReplayTask(void (*task)(void* context, u64 argument), void* context, u64 argument);
    /// Teardown (recording owner, nothing else recording): returns once every deferred operation
    /// has run, normal and priority, including those the operations queue themselves. Submits as
    /// it goes, so none waits for a tick never submitted.
    void DrainDeferredOperations();
    bool GuestSignalsPending() const {
        return signals_completed.load(std::memory_order_acquire) < signals_encoded;
    }
    /// Whether a sync point submits for the guest signals before it drains the recorder
    /// (SignalPolicy::NeedsFlush in g_signal_mode; exactly GuestSignalsPending() with both
    /// FAST_CPU_ASYNC_IDLE and FAST_CPU_SIGNAL_AT_REPLAY off). Producer only.
    bool SignalsNeedFlush() const {
        const SignalPolicy::Counts counts{
            signals_encoded, signals_completed.load(std::memory_order_acquire), signals_at_submit};
        if (g_signal_order_check) [[unlikely]]
            CheckSignalOrder(counts);
        return SignalPolicy::NeedsFlush(g_signal_mode, counts);
    }
    void FlushGuestSignals(std::string_view reason);
    /// FAST_CPU_GUARD (Stage A): guest signals encoded so far by any thread (one B encodes while
    /// running a task is counted once A has seen that task passed), and those written.
    u64 GuestSignalsEncoded() const {
        return signals_encoded_shared.load(std::memory_order_acquire);
    }
    u64 GuestSignalsCompleted() const {
        return signals_completed.load(std::memory_order_acquire);
    }
    bool ForwardGuestWait(u64 address, u32 mask, u32 reference) const;
    void InvalidateGuestWait() {
        pending_fence_sequence = 0;
    }
    // Guest signals are value packets, submitted after all preceding encoded work.
    void SignalGuest(u64 address, u64 value, u32 bytes, u32 interrupt = ~u32{0},
                     GuestSignalValue source = GuestSignalValue::Literal);

    /// Returns the current command buffer tick.
    [[nodiscard]] u64 CurrentTick() const noexcept {
        return master_semaphore.CurrentTick();
    }

    /// Returns true when a tick has been triggered by the GPU.
    [[nodiscard]] bool IsFree(u64 tick) noexcept {
        if (master_semaphore.IsFree(tick)) {
            return true;
        }
        master_semaphore.Refresh();
        return master_semaphore.IsFree(tick);
    }

    /// Returns the master timeline semaphore.
    [[nodiscard]] MasterSemaphore* GetMasterSemaphore() noexcept {
        return &master_semaphore;
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Will be run when submitting or calling PopPendingOperations.
    void DeferOperation(Common::UniqueFunction<void>&& func) {
        std::unique_lock lk(pending_ops_mutex);
        pending_ops.emplace(std::move(func), CurrentTick());
        has_pending_ops.store(true, std::memory_order_release);
    }

    /// Defers an operation until the gpu has reached the current cpu tick.
    /// Runs as soon as possible in another thread.
    void DeferPriorityOperation(Common::UniqueFunction<void>&& func) {
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops.emplace(std::move(func), CurrentTick());
            priority_ops_queued.fetch_add(1, std::memory_order_release);
        }
        priority_pending_ops_cv.notify_one();
    }

    static std::mutex submit_mutex;

private:
    void AllocateWorkerCommandBuffers(u64 tick);
    /// Ends the open pass. Its callers note why (BeginRendering a state change, EndRendering
    /// its call site).
    void EndRenderingNow();
    /// Bit 43: whether BeginRendering(next) continues the open pass; if so render_state takes
    /// next's clears and depth, and `subset` says the pass keeps more colour attachments.
    bool TryContinue(const RenderState& next, bool& subset);
    void ReportPassContinue();
    void ResetProducerState();
    static void ReplayRecorderControl(void* context, u32 kind, const std::byte* payload);

    void SubmitExecution(SubmitInfo& info);

    /// SubmitExecution's part under submit_mutex: ends the command buffer, submits it, begins the
    /// next one.
    void SubmitLocked(SubmitInfo info, u64 signal_value);

    /// FAST bit 56: why the command buffer needs its submission (Empty: it may be skipped).
    enum class EmptyCheck : u32 {
        Empty,
        Raw,         // a raw borrow: commands the recorder cannot count
        RecorderOff, // commands recorded directly
        Commands,    // command packets after the profiler's resume stamps
        Rendering,   // a pass is open
        Signals,     // deferred guest writes/IRQs complete only after a submission
        HostCopies,  // FAST bit 10 copies a submission waits for
        Repeat,      // FlushFrameEnd only: the previous frame end already left this tick open
        Count,
    };
    EmptyCheck CheckEmpty() const;
    /// Replay owner (FAST_SKIP_EMPTY_FINISH_VERIFY): the command buffer about to be submitted
    /// must have replayed exactly `expected` command packets, all query resets or timestamps,
    /// and no deferred guest signal may be waiting for it.
    void CheckEmptySubmit(u64 expected);
    void ReportSkipEmptyFinish();

    void PriorityPendingOpsThread(std::stop_token stoken);

private:
    const Instance& instance;
    MasterSemaphore master_semaphore;
    CommandPool command_pool;
    DynamicState dynamic_state;
    DynamicState* dynamic_desired{}; // FAST_CPU_STAGEB_SPLIT (SetDesiredDynamic)
    ScratchPush::Memo push_state_memo;
    vk::CommandBuffer current_cmdbuf;
    u8 crash_source{}; // GpuCrash::Source: the game's (the recorder's) or a presenter's
    CommandRecorder recorder;
    // FAST_CPU_STAGEB_SPLIT (OpenResolveBlob): the blob and whether Commands() goes to it.
    std::unique_ptr<CommandRecorder> blob_recorder;
    CommandRecorder* blob{}; // the open one: blob_recorder, or (=2) the slot's
    bool blob_open{};
    // The ring recorder for a producer that uses it directly: an open blob's packets go first.
    CommandRecorder& Main(std::source_location site = std::source_location::current()) {
        if (t_on_stageb2) {
            return recorder;
        }
        SplitSync(site.function_name());
        if (blob_open && !blob->BlobEmpty())
            recorder.AppendBlob(*blob);
        return recorder;
    }
    // FAST_CPU_STAGEB_SPLIT=2 (SplitSubmit): the job ring. head: jobs B1 handed over; tail: jobs
    // B2 finished. A slot is B1's from SplitAcquire to SplitSubmit, then B2's until tail passes.
    static inline thread_local bool t_on_stageb2{};
    struct SplitJob {
        std::unique_ptr<CommandRecorder> blob;
        bool draw{};
    };
    std::array<SplitJob, SplitSlots> split_jobs;
    bool split_threaded{};
    SplitEncode split_encode{};
    void* split_context{};
    alignas(64) std::atomic<u64> split_head{};
    alignas(64) std::atomic<u64> split_tail{};
    alignas(64) std::atomic<bool> split_b2_sleeping{}, split_b1_waiting{};
    std::atomic<bool> split_stop{};
    std::mutex split_mutex;
    std::condition_variable split_b2_cv, split_b1_cv;
    std::jthread split_thread;
    u64 split_waits{}, split_jobs_drawn{}, split_jobs_blob{}, split_full_waits{}; // B1's
    static void SplitPause() noexcept {
#if defined(_M_X64) || defined(__x86_64__)
        _mm_pause();
#endif
    }
    std::unordered_map<std::string_view, u64> split_reasons; // SplitWait's, logged builds
    void SplitReport();
    void SplitAcquire();
    void SplitWait(std::string_view reason);
    void SplitThreadMain();
    static vk::CommandBuffer FlushBlob(void* context, std::string_view reason,
                                       std::source_location site, u32 mode);
    u64 command_uses{}; // CommandUses
    u64 signals_encoded{};
    std::atomic<u64> signals_encoded_shared{}; // signals_encoded for other threads
    std::atomic<u64> signals_completed{};
    u64 pending_fence_address{}, pending_fence_sequence{};
    u32 pending_fence_value{};
    struct DeferredGuestSignal {
        u64 address, value, sequence;
        u32 bytes, interrupt;
        GuestSignalValue source;
        // FAST_CPU_SIGNAL_AT_REPLAY: the host copies queued before the signal (its replay waits
        // for them, as a submission's does for its own).
        u64 host_copies;
        // FAST_CPU_SIGNAL_AT_REPLAY_VERIFY (replay owner, on the deferred copy): the target's
        // bytes and the time when the replay reached the packet.
        u64 replayed_bytes, replayed_ns;
        bool replayed;
        // _VERIFY: the value the deferred write put there.
        u64 written{};
    };
    std::vector<DeferredGuestSignal> deferred_signals; // replay owner only
    void CompleteGuestSignals();
    // Writes one signal's bytes and raises its interrupt (replay owner), then counts it written.
    // Returns the value written.
    u64 WriteGuestSignal(const DeferredGuestSignal& signal);
    // FAST_CPU_SIGNAL_AT_REPLAY: FlushGuestSignals without a submission (producer).
    void WaitGuestSignals(std::string_view reason);
    // FAST_CPU_SIGNAL_AT_REPLAY_VERIFY (replay owner): the GuestSignal packet's replay notes the
    // target; the deferred write compares it.
    void NoteSignalReplayed(DeferredGuestSignal& signal);
    // `own`: the value an earlier signal of the same submission wrote to the same bytes, which
    // the switch writes before this one too (not another writer).
    void CheckSignalReplayed(const DeferredGuestSignal& signal, std::optional<u64> own);
    // FAST_CPU_ASYNC_IDLE_VERIFY: SignalPolicy::SubmitOrderHolds where Precise would decide.
    static void CheckSignalOrder(const SignalPolicy::Counts& counts);
    // FAST_CPU_SIGNAL_AT_REPLAY_VERIFY: the latest signal whose packet the replay reached.
    std::atomic<u64> signals_replayed{};
    // FAST_CPU_SIGNAL_AT_REPLAY (producer): FlushGuestSignals calls with signals pending, those
    // that found them written within the spin, the ones that drained the recorder after it.
    u64 at_replay_flushes{}, at_replay_spun{}, at_replay_drained{};
    std::condition_variable_any event_cv;
    struct PendingOp {
        Common::UniqueFunction<void> callback;
        u64 gpu_tick;
    };
    std::queue<PendingOp> pending_ops;
    std::recursive_mutex pending_ops_mutex;
    // Published under pending_ops_mutex, including when the optimization is disabled.
    std::atomic<bool> has_pending_ops{};
    u32 pending_polls{}; // guarded by pending_ops_mutex
    std::queue<PendingOp> priority_pending_ops;
    std::mutex priority_pending_ops_mutex;
    std::condition_variable_any priority_pending_ops_cv;
    // DrainDeferredOperations: priority operations queued, and finished (counted after the
    // callback returns: one popped but still waiting for its tick or running is not).
    std::atomic<u64> priority_ops_queued{};
    std::atomic<u64> priority_ops_done{};
    std::jthread priority_pending_ops_thread;
    RenderState render_state;
    bool is_rendering = false;
    void (*pass_end_hook)(Scheduler&) = nullptr; // SetPassEndHook
    std::unique_ptr<FoveationCache> foveation_cache;
    bool foveation_blocked = false; // BlockFoveation
    float foveation_inner_floor = 0.0f; // SetFoveationInnerFloor
    u32 foveation_drop = 0;             // SetFoveationDrop
    bool pass_foveated = false;     // the open pass has the foveation rate map
    vk::Extent2D mono_foveation{};  // SetMonoFoveation
    vk::Extent2D foveation_eye{};   // SetFoveationEye
    bool probe_break_noted = false;       // VR_PERF_PROBE: this pass's end already counted
    u64 pass_begins = 0; // PassBegins(): render passes begun (not continued) so far
    // Bit 43 part 2: dynamicRenderingUnusedAttachments is enabled on the device.
    bool unused_attachments = false;
    // Bit 43 part 2: the last state the open pass continued as a subset, while render_state has
    // not changed since (the same state continues again with one memcmp).
    bool subset_key_valid = false;
    RenderState subset_key{};
    // Bit 43: the 'FAST pass_continue' line (the recording owner's, like render_state).
    u64 begin_calls{}, continued_clears{}, continued_subsets{}, continue_verified{},
        continue_mismatches{};
    std::unique_ptr<PassBreakStats> pass_stats; // FAST_PASS_BREAK_STATS, the game's scheduler
    std::chrono::steady_clock::time_point cmdbuf_begin_time{};
    // FAST bit 56 (producer-owned, like render_state): the recorder's command packets when this
    // command buffer began and after the GPU pass profiler's resume stamps, and its raw borrows.
    u64 buffer_commands{};
    struct EmptyMark {
        u64 commands, raws;
    } empty_mark{};
    struct SkipFinishStats {
        u64 calls{}, skipped{}, verified{}, waited{};
        std::array<u64, static_cast<size_t>(EmptyCheck::Count)> kept{};
    } skip_finish{};
    u64 skip_open_tick{}, skip_verify_sequence{};
    // FAST bit 62 (producer-owned, like skip_finish): signals_encoded at the last submission,
    // submissions so far, the open tick with a deferred mid-frame cut, Wait()'s forced flushes.
    u64 signals_at_submit{}, submit_count{}, deferred_cut_tick{};
    u64 open_tick_flushes{}, deferred_tick_flushes{};
    bool verify_empty_submit{}; // the next SubmitExecution tells the replay what to expect
    std::atomic<u64> empty_checked{}, empty_mismatches{}; // replay owner, read by the report
    Common::UniqueFunction<void> pre_submit_hook{};
    std::function<u64(vk::Queue)> sparse_binds{};
    vk::Semaphore sparse_timeline{};
    tracy::VkCtxScope* profiler_scope{};
};

} // namespace Vulkan
