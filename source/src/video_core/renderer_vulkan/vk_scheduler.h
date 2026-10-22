#include "video_core/native_stereo/push_state_memo.h"
// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once
#include "video_core/fast_render.h"

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <source_location>
#include <thread>
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
    std::array<vk::Semaphore, 3> wait_semas;
    std::array<u64, 3> wait_ticks;
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
// FAST_CPU_SIGNAL_AT_REPLAY=1 (off by default; read once at start-up; bbport's RecorderFences): a
// guest signal is written when the recorder's replay reaches it, after the host copies queued
// before it, instead of after the next submission. A guest-visible write on B (WriteData, a
// query, a CE dump) then waits only until the replay has written the signals before it, with no
// submission; the mid-frame cadence (FAST bit 62) may defer cuts that carry signals. The values
// and their order among themselves and B's writes are unchanged, but the guest sees a label
// before the work ahead of it is submitted (as upstream's direct writes and bbport do): a timing
// change no image comparison covers, hence opt-in. See FAST_CPU_SIGNAL_AT_REPLAY_VERIFY.
inline const bool g_signal_at_replay_switch = [] {
    const char* value = std::getenv("FAST_CPU_SIGNAL_AT_REPLAY");
    return value && value[0] == '1';
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

    /// Attempts to execute operations whose tick the GPU has caught up with.
    void PopPendingOperations(bool force_poll = false);

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

    /// VR_GPU_PASSES (profiling): times one piece of work outside a render pass (a game
    /// dispatch), summed under `key` in the `VR gpu passes:` line. No-ops otherwise.
    bool GpuCategoryBegin(u64 key); // non-nesting
    void GpuWorkBegin(u64 key, u32 x, u32 y);
    void GpuWorkEnd();

    /// True while a render pass (dynamic rendering scope) is open.
    [[nodiscard]] bool IsRendering() const noexcept {
        return is_rendering;
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
        return dynamic_state;
    }

    // Recording-owned state follows the command buffer when recording changes threads.
    ScratchPush::Memo& PushStateMemo() { return push_state_memo; }

    /// Returns the current command buffer.
    vk::CommandBuffer CommandBuffer(std::source_location site = std::source_location::current()) {
        FastRender::AssertRecordingAccess();
        ++command_uses;
        return recorder.Raw("raw_buffer", site);
    }
    RecorderProxy Commands(std::source_location site = std::source_location::current()) {
        FastRender::AssertRecordingAccess();
        ++command_uses;
        return {recorder, site};
    }
    /// How often Commands() or CommandBuffer() was taken (recording owner only, like the
    /// recorder). Code that leaves it and CurrentTick() unchanged recorded no command (profiler
    /// timestamps aside, which go to the recorder directly): FAST bit 57 (vk_shader_hle.cpp)
    /// checks that the copy shader's buffer lookups recorded nothing.
    [[nodiscard]] u64 CommandUses() const noexcept {
        return command_uses;
    }
    /// Hands a CommandBuffer() borrow back to the recorder (CommandRecorder::ReleaseRaw). The
    /// caller must not use any raw handle of the current command buffer after this.
    void ReleaseCommandBuffer() noexcept {
        recorder.ReleaseRaw();
    }
    void FlushRecorder(std::string_view reason) { recorder.Drain(reason); }
    bool RecorderThreaded() const noexcept {
        return recorder.Threaded();
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
        return recorder.CommandPackets() + recorder.RawBorrows();
    }
    void RecordFrameMark();
    /// FAST_CPU_TASK_PUBLISH: hands the recorder what is encoded so far (producer only).
    void PublishRecorder() {
        recorder.Publish();
    }
    /// FAST_CPU_ASYNC_IDLE (producer only): the replay calls `task(context, argument)` when it
    /// reaches this point, after every packet encoded before (the submission before it included:
    /// its guest signals are written by then). The task must not record commands.
    void QueueReplayTask(void (*task)(void* context, u64 argument), void* context, u64 argument);
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
    ScratchPush::Memo push_state_memo;
    vk::CommandBuffer current_cmdbuf;
    CommandRecorder recorder;
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
    };
    std::vector<DeferredGuestSignal> deferred_signals; // replay owner only
    void CompleteGuestSignals();
    // Writes one signal's bytes and raises its interrupt (replay owner), then counts it written.
    void WriteGuestSignal(const DeferredGuestSignal& signal);
    // FAST_CPU_SIGNAL_AT_REPLAY: FlushGuestSignals without a submission (producer).
    void WaitGuestSignals(std::string_view reason);
    // FAST_CPU_SIGNAL_AT_REPLAY_VERIFY (replay owner): the GuestSignal packet's replay notes the
    // target; the deferred write compares it.
    void NoteSignalReplayed(DeferredGuestSignal& signal);
    void CheckSignalReplayed(const DeferredGuestSignal& signal);
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
    std::jthread priority_pending_ops_thread;
    RenderState render_state;
    bool is_rendering = false;
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
    tracy::VkCtxScope* profiler_scope{};
};

} // namespace Vulkan
