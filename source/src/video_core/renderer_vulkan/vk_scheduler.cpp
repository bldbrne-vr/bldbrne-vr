#include "video_core/host_copies.h"
#include "video_core/flip_order_trace.h"
#include "video_core/native_stereo/pgo_capture.h"
#include "video_core/native_stereo/profile_scopes.h"
#include "video_core/native_stereo/push_state_memo.h"
#include "video_core/fast/b_handler_profile.h"
// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <source_location>
#include <string>
#include <unordered_map>
#include <vector>
#include <immintrin.h>
#include "common/assert.h"
#include "common/debug.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/memory.h"
#include "core/platform.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderer_vulkan/vk_gpu_frame_timer.h"
#include "video_core/renderer_vulkan/vk_gpu_pass_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pass_breaks.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_submit_lock_trace.h"
#include "video_core/renderer_vulkan/vr_perf.h"

namespace Vulkan {

std::mutex Scheduler::submit_mutex;

namespace {

// VR_GPU_TIMING=1: the game's GPU time per frame (vk_gpu_frame_timer.h).
bool GpuTimingWanted() {
    static const bool wanted = [] {
        const char* value = std::getenv("VR_GPU_TIMING");
        return value != nullptr && *value != '\0' && *value != '0';
    }();
    return wanted;
}

// VR_GPU_TIMING_WINDOW_S=<s> (the launcher's performance scan): a `VR gpu frame:` window closes
// after this much time instead of after 300 frames, so a slow PC reaches the scan's warm-up and
// measurement windows as soon as a fast one; the scan's head sweep has the same period, so every
// window holds one whole sweep. 0 / unset: 300 frames.
std::chrono::nanoseconds GpuTimingWindow() {
    static const std::chrono::nanoseconds window = [] {
        const char* value = std::getenv("VR_GPU_TIMING_WINDOW_S");
        const double s = value != nullptr ? std::atof(value) : 0.0;
        return std::chrono::nanoseconds{std::isfinite(s) && s >= 0.5 && s <= 60.0
                                            ? static_cast<s64>(s * 1e9)
                                            : 0};
    }();
    return window;
}

// VR_GPU_PASSES=1 (with VR_GPU_TIMING=1): also each render pass of the game's frames, timed and
// summed per render target (`VR gpu passes:` every 300 frames). Each pass gets a timestamp before
// it begins and after it ends, both at ALL_COMMANDS (when all earlier work is done): the pass's
// share is begin -> end, the work between two passes (compute, copies, clears) begin of the next
// minus the end of the previous.
bool GpuPassesWanted() {
    static const bool wanted = [] {
        const char* value = std::getenv("VR_GPU_PASSES");
        return value != nullptr && *value != '\0' && *value != '0';
    }();
    return wanted;
}

struct GpuPassRec {
    u64 key;   // the first colour view and the depth view (stable for a cached target)
    u32 query; // begin; end = query + 1
    u16 width, height;
    u8 colors, depth, clear;
};

// All fields under Scheduler::submit_mutex (SubmitExecution holds it while it ends one command
// buffer and begins the next; GpuFrameTimerMark takes it).
struct GpuFrameTimer {
    static constexpr u32 Slots = 2048; // command buffers in flight at most (2 queries each)
    std::atomic<const Scheduler*> target{nullptr}; // other scheduler threads test ownership
    vk::Device device{};
    vk::QueryPool pool{};
    double tick_ns{1.0};
    bool failed{false};
    s32 open{-1}; // the slot of the command buffer being recorded (-1: not timed)
    u32 next{0};
    std::array<bool, Slots> used{};
    struct Pending {
        u32 slot;
        u64 tick;  // the submission's timeline value: its timestamps are written once it is done
        u64 frame; // the game frame it belongs to
        std::vector<GpuPassRec> passes; // VR_GPU_PASSES
    };
    std::deque<Pending> pending;
    // VR_GPU_PASSES
    static constexpr u32 PassQueries = 65536;
    vk::QueryPool pass_pool{};
    u32 pass_next{0};
    std::vector<GpuPassRec> cur_passes; // the command buffer being recorded
    bool pass_open{false};
    struct PassSum {
        double ms{};
        u64 count{};
        u16 width{}, height{};
        u8 colors{}, depth{}, clear{};
    };
    std::unordered_map<u64, PassSum> pass_sums; // the window
    double between_ms{};                        // the window: work between passes
    u64 between_count{};
    u64 pass_frames{};
    u64 frame_pass_end{0}; // the read-back buffer's last pass end (0: none yet)
    u64 frame{0};          // game frames ended so far
    u64 read_frame{~u64{0}};
    std::vector<std::pair<u64, u64>> spans; // the frame being read back: each buffer's start, end
    u64 prev_end{0};
    std::vector<float> busy_ms, span_ms, gap_ms; // the window
    std::chrono::steady_clock::time_point window_start{}; // its first frame (VR_GPU_TIMING_WINDOW_S)
    u64 buffers{0};
    u32 skipped{0};
};
GpuFrameTimer gpu_timer;
// VR_GPU_PASSES: the timed scheduler, read by its recording thread without the lock (the pass
// fields below are only touched by that thread: BeginRendering, EndRendering, and
// GpuTimerBegin / GpuTimerEnd, which it calls).
std::atomic<const Scheduler*> pass_target{nullptr};

void GpuTimerBegin(const Scheduler* scheduler, vk::CommandBuffer cmdbuf) {
    auto& t = gpu_timer;
    if (t.target != scheduler || !t.pool) {
        return;
    }
    t.open = -1;
    if (t.used[t.next]) {
        ++t.skipped; // every slot still in flight
        return;
    }
    const u32 slot = t.next;
    t.next = (t.next + 1) % GpuFrameTimer::Slots;
    t.used[slot] = true;
    cmdbuf.resetQueryPool(t.pool, slot * 2, 2);
    cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eTopOfPipe, t.pool, slot * 2);
    t.open = static_cast<s32>(slot);
    t.cur_passes.clear();
    t.pass_open = false;
}

void GpuTimerEnd(const Scheduler* scheduler, vk::CommandBuffer cmdbuf, u64 tick) {
    auto& t = gpu_timer;
    if (t.target != scheduler || t.open < 0) {
        return;
    }
    const u32 slot = static_cast<u32>(t.open);
    t.open = -1;
    cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eBottomOfPipe, t.pool, slot * 2 + 1);
    t.pending.push_back({slot, tick, t.frame, std::move(t.cur_passes)});
    t.cur_passes.clear();
    t.pass_open = false;
}

void GpuRecBegin(vk::CommandBuffer cmdbuf, const GpuPassRec& rec) {
    auto& t = gpu_timer;
    const u32 q = t.pass_next;
    t.pass_next = (t.pass_next + 2) % GpuFrameTimer::PassQueries;
    t.cur_passes.push_back(rec);
    t.cur_passes.back().query = q;
    cmdbuf.resetQueryPool(t.pass_pool, q, 2);
    cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, t.pass_pool, q);
    t.pass_open = true;
}

// VR_GPU_PASSES: a render pass of the timed scheduler begins / ends.
void GpuPassBegin(const Scheduler* scheduler, vk::CommandBuffer cmdbuf, const RenderState& rs) {
    auto& t = gpu_timer;
    if (pass_target.load(std::memory_order_acquire) != scheduler || t.open < 0 || t.pass_open) {
        return;
    }
    const u32 q = t.pass_next;
    t.pass_next = (t.pass_next + 2) % GpuFrameTimer::PassQueries;
    const auto view = [](vk::ImageView v) {
        return static_cast<u64>(reinterpret_cast<std::uintptr_t>(static_cast<VkImageView>(v)));
    };
    const u64 color = rs.num_color_attachments > 0 ? view(rs.color_attachments[0].image_view) : 0;
    const u64 depth =
        rs.depth_stencil_attachment.has_depth || rs.depth_stencil_attachment.has_stencil
            ? view(rs.depth_stencil_attachment.image_view)
            : 0;
    u8 clear = 0;
    for (u32 i = 0; i < rs.num_color_attachments; ++i) {
        clear |= rs.color_attachments[i].is_clear ? 1 : 0;
    }
    if (rs.depth_stencil_attachment.depth_clear || rs.depth_stencil_attachment.stencil_clear) {
        clear |= 2;
    }
    t.cur_passes.push_back({color * 0x9E3779B97F4A7C15ull ^ depth, q, rs.width, rs.height,
                            static_cast<u8>(rs.num_color_attachments),
                            static_cast<u8>(depth != 0 ? 1 : 0), clear});
    cmdbuf.resetQueryPool(t.pass_pool, q, 2);
    cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, t.pass_pool, q);
    t.pass_open = true;
}

void GpuPassEnd(const Scheduler* scheduler, vk::CommandBuffer cmdbuf) {
    auto& t = gpu_timer;
    if (pass_target.load(std::memory_order_acquire) != scheduler || !t.pass_open) {
        return;
    }
    t.pass_open = false;
    cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, t.pass_pool,
                           t.cur_passes.back().query + 1);
}

void GpuPassReport(GpuFrameTimer& t, std::size_t frames) {
    if (!t.pass_pool || frames == 0) {
        return;
    }
    std::vector<std::pair<u64, GpuFrameTimer::PassSum>> v(t.pass_sums.begin(), t.pass_sums.end());
    std::sort(v.begin(), v.end(),
              [](const auto& a, const auto& b) { return a.second.ms > b.second.ms; });
    double total = t.between_ms;
    u64 passes = 0;
    for (const auto& [k, ps] : v) {
        total += ps.ms;
        passes += ps.count;
    }
    const double n = static_cast<double>(frames);
    std::string top;
    double cs_ms = 0.0;
    u64 cs_count = 0;
    for (const auto& [k, ps] : v) {
        if (ps.colors == 255) {
            cs_ms += ps.ms;
            cs_count += ps.count;
        }
    }
    top += fmt::format("; of the passes: game dispatches {:.3f} ms ({:.1f}/fr)", cs_ms / n,
                       static_cast<double>(cs_count) / n);
    for (std::size_t i = 0; i < v.size(); ++i) {
        const auto& ps = v[i].second;
        if (ps.colors == 254) {
            top += fmt::format("; cat {} {:.3f} ms ({:.1f}/fr)", v[i].first, ps.ms / n,
                               double(ps.count) / n);
            continue;
        }
        if (ps.colors == 255) {
            top += fmt::format("; cs {:016x} {}x{} groups {:.3f} ms ({:.1f}/fr)", v[i].first,
                               ps.width, ps.height, ps.ms / n, static_cast<double>(ps.count) / n);
            continue;
        }
        top += fmt::format("; {}x{} c{}{}{} {:.3f} ms ({:.1f}/fr)", ps.width, ps.height, ps.colors,
                           ps.depth ? "+d" : "", ps.clear ? " clr" : "", ps.ms / n,
                           static_cast<double>(ps.count) / n);
    }
    LOG_INFO(Render_Vulkan,
             "VR gpu passes: {} frames: {:.3f} ms per frame in {} targets ({:.1f} passes per "
             "frame) + {:.3f} ms between passes (compute, copies, clears; {:.1f} gaps per frame) "
             "= {:.3f} ms{}",
             frames, (total - t.between_ms) / n, v.size(), static_cast<double>(passes) / n,
             t.between_ms / n, static_cast<double>(t.between_count) / n, total / n, top);
    t.pass_sums.clear();
    t.between_ms = 0.0;
    t.between_count = 0;
}

__attribute__((no_profile_instrument_function)) void GpuTimerReport(GpuFrameTimer& t) {
#ifdef BBVR_PGO_GENERATE
    ScratchPgo::Tick();
#endif
    const auto rank = [](std::vector<float>& v, double q) -> double {
        if (v.empty()) {
            return 0.0;
        }
        const std::size_t n = v.size();
        const std::size_t k = std::min<std::size_t>(
            n - 1, static_cast<std::size_t>(std::max(std::ceil(q * n), 1.0)) - 1);
        std::nth_element(v.begin(), v.begin() + k, v.end());
        return v[k];
    };
    const auto avg = [](const std::vector<float>& v) -> double {
        double sum = 0.0;
        for (const float x : v) {
            sum += x;
        }
        return v.empty() ? 0.0 : sum / static_cast<double>(v.size());
    };
    const std::size_t frames = t.busy_ms.size();
    const double busy_avg = avg(t.busy_ms);
    const double span_avg = avg(t.span_ms);
    const double gap_avg = avg(t.gap_ms);
    const double busy_max = *std::max_element(t.busy_ms.begin(), t.busy_ms.end());
    const double span_max = *std::max_element(t.span_ms.begin(), t.span_ms.end());
    const double busy_p50 = rank(t.busy_ms, 0.5);
    const double busy_p99 = rank(t.busy_ms, 0.99);
    LOG_BENCHMARK_INFO(Render_Vulkan,
             "VR gpu frame: {} game frames: GPU busy per frame avg {:.2f} / p50 {:.2f} / p99 "
             "{:.2f} / max {:.2f} ms, span (first start -> last end) avg {:.2f} / max {:.2f} ms, "
             "idle before a frame avg {:.2f} ms; {:.1f} command buffers per frame, {} untimed",
             frames, busy_avg, busy_p50, busy_p99, busy_max, span_avg, span_max, gap_avg,
             static_cast<double>(t.buffers) / static_cast<double>(frames), t.skipped);
    GpuPassReport(t, frames);
    t.busy_ms.clear();
    t.span_ms.clear();
    t.gap_ms.clear();
    t.buffers = 0;
    t.skipped = 0;
}

// The read-back frame is complete (a buffer of a later frame came back): its busy time.
void GpuTimerFinishFrame(GpuFrameTimer& t) {
    if (t.spans.empty()) {
        return;
    }
    std::sort(t.spans.begin(), t.spans.end());
    u64 busy = 0;
    u64 start = t.spans[0].first;
    u64 end = t.spans[0].second;
    u64 last = end;
    for (std::size_t i = 1; i < t.spans.size(); ++i) {
        const auto [s, e] = t.spans[i];
        last = std::max(last, e);
        if (s <= end) {
            end = std::max(end, e);
            continue;
        }
        busy += end - start;
        start = s;
        end = e;
    }
    busy += end - start;
    const u64 first = t.spans.front().first;
    const double to_ms = t.tick_ns * 1e-6;
    const auto now = std::chrono::steady_clock::now();
    if (t.busy_ms.empty()) {
        t.window_start = now;
    }
    t.busy_ms.push_back(static_cast<float>(static_cast<double>(busy) * to_ms));
    t.span_ms.push_back(static_cast<float>(static_cast<double>(last - first) * to_ms));
    t.gap_ms.push_back(t.prev_end != 0 && first > t.prev_end
                           ? static_cast<float>(static_cast<double>(first - t.prev_end) * to_ms)
                           : 0.0f);
    t.prev_end = last;
    t.buffers += t.spans.size();
    t.spans.clear();
    const auto window = GpuTimingWindow();
    if (window.count() > 0 ? now - t.window_start >= window : t.busy_ms.size() >= 300) {
        GpuTimerReport(t);
    }
}

} // Anonymous namespace

static void GpuFrameTimerMarkNow(Scheduler& scheduler, const Instance& instance) {
    if (!GpuTimingWanted()) {
        return;
    }
    const Common::Log::DeferScope log_defer; // its log lines: once the lock is released
    const LockTrace::Start lock_start;
    std::scoped_lock lk{Scheduler::submit_mutex};
    LockTrace::Hold trace{"gpu frame timer", lock_start}; // VR_LOCK_TRACE only
    auto& t = gpu_timer;
    if (t.failed) {
        return;
    }
    if (t.target == nullptr) {
        const vk::Device device = instance.GetDevice();
        const float period = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
        auto [result, pool] = device.createQueryPool({
            .queryType = vk::QueryType::eTimestamp,
            .queryCount = GpuFrameTimer::Slots * 2,
        });
        if (result != vk::Result::eSuccess || period <= 0.0f) {
            t.failed = true;
            LOG_WARNING(Render_Vulkan, "VR gpu frame: no timestamp queries ({}); not timed",
                        vk::to_string(result));
            return;
        }
        t.device = device;
        t.pool = pool;
        t.tick_ns = period;
        t.target = &scheduler; // timed from its next command buffer on
        LOG_INFO(Render_Vulkan,
                 "VR gpu frame: timing the game's frames on the GPU (VR_GPU_TIMING)");
        if (const auto window = GpuTimingWindow(); window.count() > 0) {
            // The scan's analyzer reads this to judge warm-up and windows in seconds.
            LOG_BENCHMARK_INFO(Render_Vulkan, "VR gpu frame: windows of {:.1f} s",
                               std::chrono::duration<double>(window).count());
        }
        if (GpuPassesWanted()) {
            auto [pr, pp] = device.createQueryPool({
                .queryType = vk::QueryType::eTimestamp,
                .queryCount = GpuFrameTimer::PassQueries,
            });
            if (pr == vk::Result::eSuccess) {
                t.pass_pool = pp;
                pass_target.store(&scheduler, std::memory_order_release);
                LOG_INFO(Render_Vulkan, "VR gpu passes: timing each render pass (VR_GPU_PASSES)");
            }
        }
        return;
    }
    if (t.target != &scheduler) {
        return;
    }
    ++t.frame;
    MasterSemaphore* semaphore = scheduler.GetMasterSemaphore();
    semaphore->Refresh();
    while (!t.pending.empty()) {
        const GpuFrameTimer::Pending p = t.pending.front();
        if (!semaphore->IsFree(p.tick)) {
            break;
        }
        std::array<u64, 2> ts{};
        const vk::Result result =
            t.device.getQueryPoolResults(t.pool, p.slot * 2, 2, sizeof(ts), ts.data(), sizeof(u64),
                                         vk::QueryResultFlagBits::e64);
        if (result == vk::Result::eNotReady) {
            break;
        }
        t.pending.pop_front();
        t.used[p.slot] = false;
        if (result != vk::Result::eSuccess || ts[1] < ts[0]) {
            continue;
        }
        if (p.frame != t.read_frame) {
            GpuTimerFinishFrame(t);
            t.read_frame = p.frame;
        }
        t.spans.emplace_back(ts[0], ts[1]);
        if (t.pass_pool) {
            const double to_ms = t.tick_ns * 1e-6;
            u64 prev = ts[0];
            bool ok = true;
            for (const GpuPassRec& r : p.passes) {
                std::array<u64, 2> pt{};
                if (t.device.getQueryPoolResults(t.pass_pool, r.query, 2, sizeof(pt), pt.data(),
                                                 sizeof(u64), vk::QueryResultFlagBits::e64) !=
                        vk::Result::eSuccess ||
                    pt[1] < pt[0]) {
                    ok = false;
                    continue;
                }
                if (pt[0] > prev) {
                    t.between_ms += static_cast<double>(pt[0] - prev) * to_ms;
                    ++t.between_count;
                }
                auto& ps = t.pass_sums[r.key];
                ps.ms += static_cast<double>(pt[1] - std::max(pt[0], prev)) * to_ms;
                ++ps.count;
                ps.width = r.width;
                ps.height = r.height;
                ps.colors = r.colors;
                ps.depth = r.depth;
                ps.clear |= r.clear;
                prev = std::max(prev, pt[1]);
            }
            if (ok && ts[1] > prev) {
                t.between_ms += static_cast<double>(ts[1] - prev) * to_ms;
                ++t.between_count;
            }
        }
    }
}

namespace {
enum RecorderControl : u32 { SubmitCommands, FrameMarker, GuestSignal, ReplayTask };
// FAST_SKIP_EMPTY_FINISH_VERIFY: RecorderSubmit::expect_commands of an unchecked submission.
constexpr u64 NoEmptyCheck = ~u64{0};
struct RecorderSubmit {
    SubmitInfo info;
    u64 tick;
    u64 host_copy_ticket;
    u64 expect_commands;
};
static_assert(std::is_trivially_copyable_v<RecorderSubmit>);
// FAST_CPU_ASYNC_IDLE (Scheduler::QueueReplayTask).
struct ReplayTaskPacket {
    void (*task)(void* context, u64 argument);
    void* context;
    u64 argument;
};
static_assert(std::is_trivially_copyable_v<ReplayTaskPacket>);
// FAST_CPU_SIGNAL_AT_REPLAY: FlushGuestSignals spins this many pauses (about 70 us on a 5800X3D,
// as the recorder's drain spin) for the replay to write the signals before it drains.
constexpr u32 SignalSpinPauses = 4096;
// FAST_CPU_SIGNAL_AT_REPLAY_VERIFY's counts: `verified` and the rest by the replay, `flushes`
// and `unwritten` by the producer (A or B, whichever owns the scheduler).
struct SignalReplayVerify {
    std::atomic<u64> verified{}, mismatches{}, window_ns{}, skipped{};
    std::atomic<u64> flushes{}, unwritten{};
};
SignalReplayVerify g_signal_replay_verify;
// FAST_CPU_ASYNC_IDLE_VERIFY: Scheduler::CheckSignalOrder's counts (producer).
std::atomic<u64> g_signal_order_checks{}, g_signal_order_redundant{}, g_signal_order_mismatches{};
u64 NowNs() {
    return static_cast<u64>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count());
}
} // namespace
void GpuFrameTimerMark(Scheduler& scheduler, const Instance&) {
    if (GpuTimingWanted())
        scheduler.RecordFrameMark();
}
void Scheduler::RecordFrameMark() {
    if (recorder.Threaded())
        recorder.QueueControl(FrameMarker, nullptr, 0);
    else {
        recorder.Drain("frame_marker");
        GpuFrameTimerMarkNow(*this, instance);
    }
}
void Scheduler::ReplayRecorderControl(void* context, u32 kind, const std::byte* payload) {
    auto& self = *static_cast<Scheduler*>(context);
    switch (kind) {
    case SubmitCommands: {
        const auto& p = *reinterpret_cast<const RecorderSubmit*>(payload);
        FastRender::WaitHostCopies(p.host_copy_ticket);
        self.CheckEmptySubmit(p.expect_commands);
        self.SubmitLocked(p.info, p.tick);
        self.CompleteGuestSignals();
        break;
    }
    case FrameMarker:
        GpuFrameTimerMarkNow(self, self.instance);
        break;
    case GuestSignal: {
        DeferredGuestSignal signal;
        std::memcpy(&signal, payload, sizeof(signal));
        if (g_signal_at_replay) {
            // Written now, in stream order with the other signals: after the host copies queued
            // before it, as the next submission's replay would have waited for them.
            FastRender::WaitHostCopies(signal.host_copies);
            self.WriteGuestSignal(signal);
            break;
        }
        if (g_signal_at_replay_verify) [[unlikely]]
            self.NoteSignalReplayed(signal);
        self.deferred_signals.push_back(signal);
        break;
    }
    case ReplayTask: {
        ReplayTaskPacket packet;
        std::memcpy(&packet, payload, sizeof(packet));
        packet.task(packet.context, packet.argument);
        break;
    }
    default:
        UNREACHABLE();
    }
}
void Scheduler::CompleteGuestSignals() {
    // These writes become guest-visible only AFTER vkQueueSubmit of all preceding
    // work. Several internal fences can share that submission; their writes/IRQs
    // remain ordered. No render-thread pointer/callback is retained.
    for (const auto& p : deferred_signals) {
        if (g_signal_at_replay_verify) [[unlikely]]
            CheckSignalReplayed(p);
        WriteGuestSignal(p);
    }
    deferred_signals.clear();
}
void Scheduler::WriteGuestSignal(const DeferredGuestSignal& p) {
    if (p.bytes) {
        void* address = reinterpret_cast<void*>(p.address);
        const u64 value = p.source == GuestSignalValue::GpuClock      ? AmdGpu::GetGpuClock64()
                          : p.source == GuestSignalValue::PerfCounter ? AmdGpu::GetGpuPerfCounter()
                                                                      : p.value;
        FlipOrderTrace::Write("deferred_begin", p.address, value, p.bytes, p.sequence);
        if (!Core::Memory::Instance()->TryWriteBacking(address, &value, p.bytes))
            std::memcpy(address, &value, p.bytes);
        FlipOrderTrace::Write("deferred_end", p.address, value, p.bytes, p.sequence);
    }
    if (p.interrupt != ~u32{0})
        Platform::IrqC::Instance()->Signal(static_cast<Platform::InterruptId>(p.interrupt));
    signals_completed.store(p.sequence, std::memory_order_release);
}
// FAST_CPU_SIGNAL_AT_REPLAY_VERIFY, when the replay reaches the GuestSignal packet: the target's
// bytes now are what the switch's write would replace here.
void Scheduler::NoteSignalReplayed(DeferredGuestSignal& p) {
    signals_replayed.store(p.sequence, std::memory_order_release);
    if (!p.bytes)
        return;
    p.replayed_bytes = 0;
    Core::Memory::Instance()->CopySparseMemory(p.address, reinterpret_cast<u8*>(&p.replayed_bytes),
                                               p.bytes);
    p.replayed_ns = NowNs();
    p.replayed = true;
}
// FAST_CPU_SIGNAL_AT_REPLAY_VERIFY, right before the deferred write: if another write reached the
// target since the replay passed the packet (the guest CPU, a DMA or WriteData run meanwhile), the
// switch's earlier write would have come before it and been overwritten, where the deferred one
// comes after it: the final bytes would differ. A guest read in between (the label seen sooner) is
// the switch's intended change and cannot be seen from here.
void Scheduler::CheckSignalReplayed(const DeferredGuestSignal& p) {
    auto& v = g_signal_replay_verify;
    if (!p.replayed) {
        v.skipped.fetch_add(1, std::memory_order_relaxed); // an interrupt alone
        return;
    }
    u64 now_bytes = 0;
    Core::Memory::Instance()->CopySparseMemory(p.address, reinterpret_cast<u8*>(&now_bytes),
                                               p.bytes);
    const u64 window = NowNs() - p.replayed_ns;
    v.window_ns.fetch_add(window, std::memory_order_relaxed);
    const u64 verified = v.verified.fetch_add(1, std::memory_order_relaxed) + 1;
    if (now_bytes != p.replayed_bytes) {
        const u64 mismatches = v.mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
        if (mismatches <= 32 || (mismatches & 1023) == 0)
            LOG_ERROR(Render_Vulkan,
                      "FAST_CPU_SIGNAL_AT_REPLAY verify: {:#x}+{} changed from {:#x} to {:#x} in "
                      "the {} us between the signal's replay and its deferred write of {:#x} "
                      "(sequence {}); written at replay it would have been overwritten, mismatch "
                      "#{}",
                      p.address, p.bytes, p.replayed_bytes, now_bytes, window / 1000, p.value,
                      p.sequence, mismatches);
    }
    if ((verified & 4095) == 0)
        LOG_INFO(Render_Vulkan,
                 "FAST_CPU_SIGNAL_AT_REPLAY verify: signals={} mismatches={} interrupt_only={} "
                 "earlier_by_us_avg={:.1f}; guest-write orderings={} of which the switch would "
                 "still wait for {} (the others neither submit nor wait)",
                 verified, v.mismatches.load(std::memory_order_relaxed),
                 v.skipped.load(std::memory_order_relaxed),
                 double(v.window_ns.load(std::memory_order_relaxed)) / 1000.0 / double(verified),
                 v.flushes.load(std::memory_order_relaxed),
                 v.unwritten.load(std::memory_order_relaxed));
}
// FAST_CPU_ASYNC_IDLE_VERIFY: Precise submits only for signals encoded since the last submission,
// trusting that no other can be pending without a submission queued after it. Where Old submits
// for an earlier one (its submission not replayed yet), Precise leaves out a redundant submission.
// Only while signals are written at a submission's replay (g_signal_order_check: not with
// FAST_CPU_SIGNAL_AT_REPLAY acting, whose replay writes them at their packets by design).
void Scheduler::CheckSignalOrder(const SignalPolicy::Counts& counts) {
    const u64 checks = g_signal_order_checks.fetch_add(1, std::memory_order_relaxed) + 1;
    if (SignalPolicy::Pending(counts) && !SignalPolicy::SinceSubmit(counts))
        g_signal_order_redundant.fetch_add(1, std::memory_order_relaxed);
    if (!SignalPolicy::SubmitOrderHolds(counts)) {
        const u64 mismatches =
            g_signal_order_mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
        if (mismatches <= 32)
            LOG_ERROR(Render_Vulkan,
                      "FAST_CPU_ASYNC_IDLE verify: signals encoded={} written={} at the last "
                      "submission={}: one encoded since the submission is written already, "
                      "mismatch #{}",
                      counts.encoded, counts.completed, counts.at_submit, mismatches);
    }
    if ((checks & 4095) == 0)
        LOG_INFO(Render_Vulkan,
                 "FAST_CPU_ASYNC_IDLE verify: signal flush decisions={} redundant_submissions={} "
                 "(Precise leaves them out) mismatches={}",
                 checks, g_signal_order_redundant.load(std::memory_order_relaxed),
                 g_signal_order_mismatches.load(std::memory_order_relaxed));
}
void Scheduler::QueueReplayTask(void (*task)(void* context, u64 argument), void* context,
                                u64 argument) {
    const ReplayTaskPacket packet{task, context, argument};
    // Its replay records nothing: FAST_CPU_PUSH_SPLIT keeps the held push constants across it.
    recorder.QueueControl(ReplayTask, &packet, sizeof(packet), true, false);
}
void Scheduler::SignalGuest(u64 address, u64 value, u32 bytes, u32 interrupt,
                            GuestSignalValue source) {
    static_assert(std::is_trivially_copyable_v<DeferredGuestSignal>);
    ASSERT(bytes == 0 || bytes == 4 || bytes == 8);
    ASSERT(recorder.Threaded());
    InvalidateGuestWait();
    if (!bytes && interrupt == ~u32{0})
        return;
    // FAST_CPU_SIGNAL_AT_REPLAY: the host copies queued so far precede the write (a submission
    // flushes and waits for them the same way).
    const DeferredGuestSignal signal{address,
                                     value,
                                     ++signals_encoded,
                                     bytes,
                                     interrupt,
                                     source,
                                     g_signal_at_replay ? FastRender::FlushHostCopies() : 0,
                                     0,
                                     0,
                                     false};
    signals_encoded_shared.store(signals_encoded, std::memory_order_release);
    if (bytes >= 4 && source == GuestSignalValue::Literal) {
        pending_fence_address = address;
        pending_fence_value = static_cast<u32>(value);
        pending_fence_sequence = signal.sequence;
    }
    FlipOrderTrace::Write("deferred_encode", address, value, bytes, signal.sequence);
    // Its replay only queues the signal (ReplayRecorderControl): nothing reaches the command
    // buffer, so FAST_CPU_PUSH_SPLIT keeps the held push constants across it.
    // FAST_CPU_SIGNAL_AT_REPLAY: published at once, so the replay writes it as soon as it gets
    // there and a later guest write on the producer rarely finds it unwritten.
    recorder.QueueControl(GuestSignal, &signal, sizeof(signal), g_signal_at_replay, false);
    // Bound the separate deferred-signal queue even for a command stream with no draws.
    if (signals_encoded - signals_completed.load(std::memory_order_acquire) >= 2048) {
        if (!g_signal_at_replay) // written at replay: the drain alone writes them
            Flush();
        recorder.Drain("signal_capacity");
    }
}
void Scheduler::FlushGuestSignals(std::string_view reason) {
    InvalidateGuestWait();
    if (!GuestSignalsPending())
        return;
    if (g_signal_at_replay) {
        WaitGuestSignals(reason);
        return;
    }
    if (g_signal_at_replay_verify) [[unlikely]] {
        // The switch would wait only while the replay has not reached the newest signal yet.
        auto& v = g_signal_replay_verify;
        v.flushes.fetch_add(1, std::memory_order_relaxed);
        if (signals_replayed.load(std::memory_order_acquire) < signals_encoded)
            v.unwritten.fetch_add(1, std::memory_order_relaxed);
    }
    // The replayed value packets must be submitted before they can signal. FAST_CPU_ASYNC_IDLE
    // (Precise): not when a submission encoded after all of them is still on its way, whose
    // replay writes them before the drain returns.
    if (SignalsNeedFlush() || g_signal_mode == SignalPolicy::Mode::Old) {
        if (g_task_publish)
            recorder.Publish(); // FAST_CPU_TASK_PUBLISH: replay while the submission is encoded
        Flush();
    }
    recorder.Drain(reason);
}
// FAST_CPU_SIGNAL_AT_REPLAY: the replay writes each signal when it reaches its packet, so a guest
// write that must follow them waits for that and nothing else: no submission, and not for the
// commands encoded after the newest signal.
void Scheduler::WaitGuestSignals(std::string_view reason) {
    if (!recorder.Threaded()) {
        recorder.Drain(reason); // inline replay writes them
        return;
    }
    const u64 target = signals_encoded;
    recorder.Publish(); // already published by SignalGuest; cheap when nothing is new
    ++at_replay_flushes;
    bool written = false;
    for (u32 pause = 0; pause < SignalSpinPauses; ++pause) {
        if (signals_completed.load(std::memory_order_acquire) >= target) {
            written = true;
            break;
        }
        _mm_pause();
    }
    if (written) {
        ++at_replay_spun;
    } else {
        ++at_replay_drained;
        recorder.Drain(reason);
    }
    if ((at_replay_flushes & 2047) == 0)
        LOG_INFO(Render_Vulkan,
                 "FAST_CPU_SIGNAL_AT_REPLAY: guest-write orderings with signals unwritten={} "
                 "written within the spin={} drained={}",
                 at_replay_flushes, at_replay_spun, at_replay_drained);
}
bool Scheduler::ForwardGuestWait(u64 address, u32 mask, u32 reference) const {
    // Only the exact latest still-pending, explicitly-valued write can satisfy a
    // same-stream equality wait. Real CPU/external waits use FlushGuestSignals.
    return recorder.Threaded() && pending_fence_sequence &&
           pending_fence_sequence > signals_completed.load(std::memory_order_acquire) &&
           address == pending_fence_address && (pending_fence_value & mask) == reference;
}

// FAST bit 43 (PassContinue) and FAST_PASS_BREAK_STATS (vk_pass_breaks.h).
namespace {

// Why the open pass and the next state differ: the first difference, in this order. Only
// BreakSubset and BreakConsumed can be continued (bit 43 parts 2 and 1); the statistics count
// them as breaks while the bit or the part is off.
enum PassBreakKind : u32 {
    BreakSize,         // extent or layer count
    BreakViewMask,     // multiview mask
    BreakColourView,   // a colour target the two share
    BreakColourLayout, //
    BreakNewClear,     // a clear asked for again, or with another value (colour, depth, stencil)
    BreakDepthView,    //
    BreakDepthLayout,  //
    BreakDepthFlags,   // depth or stencil aspect in one of them only
    BreakSuperset,     // more colour targets, the shared ones the same (the motion slot added)
    BreakSubset,       // fewer colour targets, the shared ones the same (part 2)
    BreakConsumed,     // only consumed clears, or bytes vkCmdBeginRendering ignores (part 1)
    NumPassBreakReasons,
};
constexpr std::array<const char*, NumPassBreakReasons> PassBreakNames{
    "size",   "viewmask", "cview",    "clayout", "clear",   "dview",
    "dlayout", "dflags",  "superset", "subset",  "consumed"};

// `consumed`: some clear the open pass began with is not asked for by `next` (it loads there).
// A load is satisfied by whatever the open pass's begin did, because End + Begin(LOAD) of the
// same views, layouts, area, layers and view mask reads back exactly what the store wrote; a
// clear only by the same clear (a pass that began with it and has not been ended since).
u32 ComparePassStates(const RenderState& open, const RenderState& next, bool& consumed) {
    consumed = false;
    if (open.width != next.width || open.height != next.height ||
        open.num_layers != next.num_layers) {
        return BreakSize;
    }
    if (open.probe_view_mask != next.probe_view_mask ||
        open.probe_reserved != next.probe_reserved) {
        return BreakViewMask;
    }
    const u32 shared = next.num_color_attachments < open.num_color_attachments
                           ? next.num_color_attachments
                           : open.num_color_attachments;
    for (u32 i = 0; i < shared; ++i) {
        const auto& a = open.color_attachments[i];
        const auto& b = next.color_attachments[i];
        if (a.image_view != b.image_view) {
            return BreakColourView;
        }
        if (a.image_layout != b.image_layout) {
            return BreakColourLayout;
        }
        if (b.is_clear != 0) {
            if (a.is_clear != b.is_clear || a.clear_value != b.clear_value) {
                return BreakNewClear;
            }
        } else if (a.is_clear != 0) {
            consumed = true;
        }
    }
    const auto& x = open.depth_stencil_attachment;
    const auto& y = next.depth_stencil_attachment;
    if (x.image_view != y.image_view) {
        return BreakDepthView;
    }
    if (x.image_layout != y.image_layout) {
        return BreakDepthLayout;
    }
    if (x.has_depth != y.has_depth || x.has_stencil != y.has_stencil) {
        return BreakDepthFlags;
    }
    if (y.depth_clear) {
        if (!x.depth_clear || x.clear_value[0] != y.clear_value[0]) {
            return BreakNewClear;
        }
    } else if (x.depth_clear) {
        consumed = true;
    }
    if (y.stencil_clear) {
        if (!x.stencil_clear || x.clear_value[1] != y.clear_value[1]) {
            return BreakNewClear;
        }
    } else if (x.stencil_clear) {
        consumed = true;
    }
    if (next.num_color_attachments > open.num_color_attachments) {
        return BreakSuperset;
    }
    if (next.num_color_attachments < open.num_color_attachments) {
        return BreakSubset;
    }
    return BreakConsumed;
}

// FAST_PASS_CONTINUE_VERIFY: the same decision derived another way. In both states the clear
// flags and values of what `next` loads are zeroed, so are the slots past `next`'s colour count
// (a subset's extra targets are the open pass's own), and the two must then be equal byte for
// byte: only what vkCmdBeginRendering ignores, or a clear already done, may differ.
bool NormalizedPassStatesEqual(RenderState open, RenderState next) {
    const u32 count = next.num_color_attachments;
    if (count > open.num_color_attachments || count > next.color_attachments.size()) {
        return false;
    }
    open.num_color_attachments = static_cast<u16>(count);
    for (u32 i = 0; i < next.color_attachments.size(); ++i) {
        auto& a = open.color_attachments[i];
        auto& b = next.color_attachments[i];
        if (i >= count) {
            a = {};
            b = {};
        } else if (b.is_clear == 0) {
            a.is_clear = 0;
            a.clear_value = {};
            b.clear_value = {};
        }
    }
    auto& x = open.depth_stencil_attachment;
    auto& y = next.depth_stencil_attachment;
    if (!y.depth_clear) {
        x.depth_clear = false;
        x.clear_value[0] = 0;
        y.clear_value[0] = 0;
    }
    if (!y.stencil_clear) {
        x.stencil_clear = false;
        x.clear_value[1] = 0;
        y.clear_value[1] = 0;
    }
    x.clear_value[2] = x.clear_value[3] = 0;
    y.clear_value[2] = y.clear_value[3] = 0;
    return open == next;
}

// FAST_PASS_BREAK_STATS: one explicit-end call site (EndRendering's caller).
struct PassBreakSite {
    const char* file{};
    u32 line{};
    u64 ends{}, gbuffer_ends{}, restarts{};
};

const char* BaseName(const char* path) {
    const char* base = path;
    for (const char* p = path; *p != '\0'; ++p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

} // Anonymous namespace

// FAST_PASS_BREAK_STATS, on the game's scheduler (the one with the recorder). Like render_state
// it is only touched by the thread that records (BeginRendering, EndRendering and
// NoteBarrierBreak run there), and it prints there.
struct PassBreakStats {
    static constexpr u32 NumSites = 64;
    std::array<PassBreakSite, NumSites + 1> sites{}; // the last one: sites past the table
    std::array<u64, NumPassBreakReasons> state{};
    u64 begins{}, ends{}, gbuffer_ends{}, explicit_ends{}, restarts{}, barrier_breaks{},
        read_only_barrier_breaks{}, continued_clears{}, continued_subsets{};
    // The last explicit end, until the next pass begins: a begin with the state it ended is a
    // restart, charged to that site.
    bool pending{};
    u32 pending_site{};
    RenderState ended{};
    // The window.
    std::chrono::steady_clock::time_point start{std::chrono::steady_clock::now()};
    u64 frames_start{FastRender::g_counters.frames.load(std::memory_order_relaxed)};
    u64 elided_start{}, broadened_start{}, read_after_read_start{};

    u32 SiteIndex(const std::source_location& where) {
        const char* const file = where.file_name();
        const u32 line = where.line();
        const u32 hash =
            static_cast<u32>((reinterpret_cast<std::uintptr_t>(file) >> 4) * 0x9E3779B1u) ^
            (line * 0x85EBCA6Bu);
        for (u32 probe = 0; probe < NumSites; ++probe) {
            const u32 i = (hash + probe) % NumSites;
            auto& site = sites[i];
            if (site.file == file && site.line == line) {
                return i;
            }
            if (site.file == nullptr) {
                site.file = file;
                site.line = line;
                return i;
            }
        }
        return NumSites;
    }

    void NoteEnd(const RenderState& open) {
        ++ends;
        gbuffer_ends += open.num_color_attachments >= 4 ? 1 : 0;
    }

    void NoteStateBreak(const RenderState& open, u32 reason) {
        NoteEnd(open);
        ++state[reason];
        pending = false;
    }

    void NoteExplicitEnd(const RenderState& open, const std::source_location& where) {
        NoteEnd(open);
        ++explicit_ends;
        const u32 i = SiteIndex(where);
        auto& site = sites[i];
        ++site.ends;
        site.gbuffer_ends += open.num_color_attachments >= 4 ? 1 : 0;
        pending = true;
        pending_site = i;
        ended = open;
    }

    void NoteBegin(const RenderState& next) {
        ++begins;
        if (pending) {
            pending = false;
            bool consumed = false;
            const u32 reason = ComparePassStates(ended, next, consumed);
            if (ended == next || reason == BreakConsumed || reason == BreakSubset) {
                ++restarts;
                ++sites[pending_site].restarts;
            }
        }
        if ((begins & 255) == 0) {
            MaybeReport();
        }
    }

    void MaybeReport() {
        const auto now = std::chrono::steady_clock::now();
        if (now - start < std::chrono::seconds(2)) {
            return;
        }
        const u64 frames_now = FastRender::g_counters.frames.load(std::memory_order_relaxed);
        const u64 elided_now = PassBreaks::g_barriers.elided.load(std::memory_order_relaxed);
        const u64 broadened_now = PassBreaks::g_barriers.broadened.load(std::memory_order_relaxed);
        const u64 read_after_read_now =
            PassBreaks::g_barriers.read_after_read.load(std::memory_order_relaxed);
        const u64 frames = frames_now - frames_start;
        const double f = frames ? static_cast<double>(frames) : 1.0;
        std::string reasons;
        for (u32 r = 0; r < NumPassBreakReasons; ++r) {
            reasons += fmt::format("{}{}={:.2f}", r ? " " : "", PassBreakNames[r],
                                   static_cast<double>(state[r]) / f);
        }
        std::array<u32, NumSites + 1> order{};
        u32 used = 0;
        for (u32 i = 0; i <= NumSites; ++i) {
            if (sites[i].ends != 0) {
                order[used++] = i;
            }
        }
        std::sort(order.begin(), order.begin() + used,
                  [&](u32 a, u32 b) { return sites[a].ends > sites[b].ends; });
        std::string top;
        for (u32 k = 0; k < used && k < 12; ++k) {
            const auto& site = sites[order[k]];
            top += fmt::format("; {}:{} {:.2f}/f (gbuffer {:.2f}, restarts {:.2f})",
                               site.file ? BaseName(site.file) : "other", site.line,
                               static_cast<double>(site.ends) / f,
                               static_cast<double>(site.gbuffer_ends) / f,
                               static_cast<double>(site.restarts) / f);
        }
        LOG_INFO(Render_Vulkan,
                 "FAST pass_breaks: frames={} mask={:#x} passes/f={:.1f} ends/f={:.1f} "
                 "gbuffer_ends/f={:.2f} state/f [{}] explicit/f={:.1f} restarts/f={:.1f} "
                 "barrier_breaks/f={:.2f} (read_after_read_only={:.2f}) continued/f clears={:.2f} "
                 "subsets={:.2f} buffer_barriers/f read_after_read={:.1f} elided={:.1f} "
                 "broadened={:.1f}; explicit ends by site{}",
                 frames, FastRender::g_mask.load(std::memory_order_relaxed),
                 static_cast<double>(begins) / f, static_cast<double>(ends) / f,
                 static_cast<double>(gbuffer_ends) / f, reasons,
                 static_cast<double>(explicit_ends) / f, static_cast<double>(restarts) / f,
                 static_cast<double>(barrier_breaks) / f,
                 static_cast<double>(read_only_barrier_breaks) / f,
                 static_cast<double>(continued_clears) / f,
                 static_cast<double>(continued_subsets) / f,
                 static_cast<double>(read_after_read_now - read_after_read_start) / f,
                 static_cast<double>(elided_now - elided_start) / f,
                 static_cast<double>(broadened_now - broadened_start) / f, top);
        for (auto& site : sites) {
            site.ends = site.gbuffer_ends = site.restarts = 0; // keep the keys
        }
        state = {};
        begins = ends = gbuffer_ends = explicit_ends = restarts = barrier_breaks =
            read_only_barrier_breaks = continued_clears = continued_subsets = 0;
        start = now;
        frames_start = frames_now;
        elided_start = elided_now;
        broadened_start = broadened_now;
        read_after_read_start = read_after_read_now;
    }
};

Scheduler::Scheduler(const Instance& instance, bool allow_recorder)
    : instance{instance}, master_semaphore{instance}, command_pool{instance, &master_semaphore},
      recorder{current_cmdbuf, allow_recorder} {
#if TRACY_GPU_ENABLED
    profiler_scope = reinterpret_cast<tracy::VkCtxScope*>(std::malloc(sizeof(tracy::VkCtxScope)));
#endif
    unused_attachments = instance.IsDynamicRenderingUnusedAttachmentsSupported();
    if (allow_recorder && PassBreaks::StatsEnabled()) {
        pass_stats = std::make_unique<PassBreakStats>();
    }
    recorder.SetControlHandler(this, ReplayRecorderControl);
    AllocateWorkerCommandBuffers(master_semaphore.CurrentTick());
    ResetProducerState();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
    recorder.Stop();
#if TRACY_GPU_ENABLED
    std::free(profiler_scope);
#endif
    if (gpu_timer.target == this) {
        std::scoped_lock lk{submit_mutex};
        gpu_timer.device.destroyQueryPool(gpu_timer.pool);
        gpu_timer.pool = vk::QueryPool{};
        pass_target.store(nullptr);
        if (gpu_timer.pass_pool) {
            gpu_timer.device.destroyQueryPool(gpu_timer.pass_pool);
            gpu_timer.pass_pool = vk::QueryPool{};
        }
        gpu_timer.target = nullptr;
        gpu_timer.failed = true; // never again in this process
    }
}

bool Scheduler::GpuCategoryBegin(u64 key) {
    auto& t = gpu_timer;
    if (pass_target.load(std::memory_order_acquire) != this)
        return false;
    recorder.Raw("gpu_pass_profile", std::source_location::current());
    if (t.open < 0 || t.pass_open || is_rendering)
        return false;
    GpuRecBegin(current_cmdbuf, {key, 0, 0, 0, 254, 0, 0});
    return true;
}

void Scheduler::GpuWorkBegin(u64 key, u32 x, u32 y) {
    auto& t = gpu_timer;
    if (pass_target.load(std::memory_order_acquire) != this)
        return;
    recorder.Raw("gpu_pass_profile", std::source_location::current());
    if (t.open < 0 || t.pass_open || is_rendering) {
        return;
    }
    recorder.Drain("gpu_pass_profile");
    GpuRecBegin(current_cmdbuf, {key, 0, static_cast<u16>(std::min<u32>(x, 65535)),
                                 static_cast<u16>(std::min<u32>(y, 65535)), 255, 0, 0});
}

void Scheduler::GpuWorkEnd() {
    if (pass_target.load(std::memory_order_acquire) == this) {
        recorder.Raw("gpu_pass_profile", std::source_location::current());
        GpuPassEnd(this, current_cmdbuf);
    }
}

// VR_PERF_PROBE: why a render pass with 4+ colour targets (the G-buffer) ends.
namespace {
u32 PassBreakReason(const RenderState& a, const RenderState& b) {
    if (a.num_color_attachments != b.num_color_attachments || a.width != b.width ||
        a.height != b.height || a.num_layers != b.num_layers) {
        return 1; // size / count
    }
    for (u32 i = 0; i < a.num_color_attachments; ++i) {
        const auto& x = a.color_attachments[i];
        const auto& y = b.color_attachments[i];
        if (x.image_view != y.image_view) {
            return 2;
        }
        if (x.image_layout != y.image_layout) {
            return 3;
        }
        if (x.is_clear != y.is_clear || x.clear_value != y.clear_value) {
            return 4;
        }
    }
    const auto& x = a.depth_stencil_attachment;
    const auto& y = b.depth_stencil_attachment;
    if (x.image_view != y.image_view) {
        return 5;
    }
    if (x.image_layout != y.image_layout) {
        return 6;
    }
    return 7; // depth clear / flags
}
} // Anonymous namespace

bool Scheduler::Continues(const RenderState& state) const {
    if (!is_rendering) {
        return false;
    }
    if (render_state == state) {
        return true;
    }
    if (!FastRender::On(PassContinue)) {
        return false;
    }
    if (subset_key_valid && subset_key == state) {
        return true; // continued as this subset already, render_state unchanged since
    }
    bool consumed = false;
    const u32 reason = ComparePassStates(render_state, state, consumed);
    const u32 parts = PassBreaks::Parts();
    const bool clears_ok = !consumed || (parts & PassBreaks::ConsumedClears) != 0;
    if (reason == BreakConsumed) {
        return (parts & PassBreaks::ConsumedClears) != 0;
    }
    return reason == BreakSubset && unused_attachments &&
           (parts & PassBreaks::SubsetPasses) != 0 && clears_ok;
}

bool Scheduler::TryContinue(const RenderState& next, bool& subset) {
    // The same decision as Continues (which the draw path asked first), then checked.
    subset = next.num_color_attachments < render_state.num_color_attachments;
    if (subset_key_valid && subset_key == next) {
        ++continued_subsets; // nothing to adopt: render_state already took this state's flags
        return true;
    }
    if (!Continues(next)) {
        return false;
    }
    if (const u64 every = PassBreaks::VerifyEvery(); every != 0) [[unlikely]] {
        static thread_local u64 tick = 0;
        if (++tick % every == 0) {
            ++continue_verified;
            if (!NormalizedPassStatesEqual(render_state, next)) {
                ++continue_mismatches;
                if (continue_mismatches <= 32 || (continue_mismatches & 1023) == 0) {
                    LOG_ERROR(Render_Vulkan,
                              "FAST pass_continue verify: mismatch #{} (colours {} -> {}, "
                              "{}x{}x{}); the pass is ended and begun the old way",
                              continue_mismatches, render_state.num_color_attachments,
                              next.num_color_attachments, next.width, next.height,
                              next.num_layers);
                }
                return false;
            }
        }
    }
    if (subset) {
        // The pass keeps its extra colour targets (the draws that follow may use them again);
        // the shared ones and the depth take next's flags, so its consumed clears compare equal.
        for (u32 i = 0; i < next.num_color_attachments; ++i) {
            render_state.color_attachments[i] = next.color_attachments[i];
        }
        render_state.depth_stencil_attachment = next.depth_stencil_attachment;
        subset_key = next;
        subset_key_valid = true;
        ++continued_subsets;
    } else {
        render_state = next;
        subset_key_valid = false; // render_state changed: a remembered subset is compared again
        ++continued_clears;
    }
    return true;
}

void Scheduler::ReportPassContinue() {
    LOG_INFO(Render_Vulkan,
             "FAST pass_continue: on={} parts={:#x} unused_attachments={} continued_clears={} "
             "continued_subsets={} verified={} mismatches={} buffer_reads_elided={} "
             "read_barriers_broadened={}",
             FastRender::On(PassContinue) ? 1 : 0, PassBreaks::Parts(), unused_attachments ? 1 : 0,
             continued_clears, continued_subsets, continue_verified, continue_mismatches,
             PassBreaks::g_barriers.elided.load(std::memory_order_relaxed),
             PassBreaks::g_barriers.broadened.load(std::memory_order_relaxed));
}

void Scheduler::BeginRendering(const RenderState& new_state) {
    FastRender::BProfile::Scope b_profile{FastRender::BProfile::BeginRendering};
    if (is_rendering && render_state == new_state) {
        return;
    }
    if ((++begin_calls & 0x3ffff) == 0 && FastRender::On(PassContinue)) {
        ReportPassContinue();
    }
    if (is_rendering) {
        // Bit 43: a state the open pass already satisfies continues it (vk_pass_breaks.h).
        bool subset = false;
        const bool continued = FastRender::On(PassContinue) && TryContinue(new_state, subset);
        if (pass_stats) [[unlikely]] {
            if (continued) {
                ++(subset ? pass_stats->continued_subsets : pass_stats->continued_clears);
            } else {
                bool consumed = false;
                pass_stats->NoteStateBreak(
                    render_state, ComparePassStates(render_state, new_state, consumed));
            }
        }
        if (continued) {
            return;
        }
    }
    if (is_rendering && Perf::Probe() && render_state.num_color_attachments >= 4) [[unlikely]] {
        Perf::NotePassBreak(PassBreakReason(render_state, new_state));
        probe_break_noted = true;
    }
    EndRenderingNow();
    if (pass_stats) [[unlikely]] {
        pass_stats->NoteBegin(new_state);
    }
    // FAST_GPU_PASS_PROFILE (bit 38): the pass's timestamp, after the previous pass ended.
    if (GpuPassProfiler* const pass_profiler = GpuPassProfiler::For(*this)) [[unlikely]] {
        pass_profiler->BeginPass(new_state);
    }
    is_rendering = true;
    render_state = new_state;
    ++pass_begins;

    std::array<vk::RenderingAttachmentInfo, 8> color_attachments;
    for (u32 i = 0; i < render_state.num_color_attachments; ++i) {
        const auto& cb = render_state.color_attachments[i];
        color_attachments[i] = vk::RenderingAttachmentInfo{
            .imageView = cb.image_view,
            .imageLayout = cb.image_layout,
            .loadOp = cb.is_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
            .storeOp = vk::AttachmentStoreOp::eStore,
            .clearValue = vk::ClearValue{.color = vk::ClearColorValue{.uint32 = cb.clear_value}},
        };
    }

    const auto& db = render_state.depth_stencil_attachment;
    const vk::RenderingAttachmentInfo depth_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.depth_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue =
            vk::ClearValue{.depthStencil = vk::ClearDepthStencilValue{.depth = std::bit_cast<float>(
                                                                          db.clear_value[0])}},
    };
    const vk::RenderingAttachmentInfo stencil_attachment = {
        .imageView = db.image_view,
        .imageLayout = db.image_layout,
        .loadOp = db.stencil_clear ? vk::AttachmentLoadOp::eClear : vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
        .clearValue = vk::ClearValue{.depthStencil =
                                         vk::ClearDepthStencilValue{.stencil = db.clear_value[1]}},
    };

    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = render_state.num_layers,
        .viewMask = render_state.probe_view_mask,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    if (pass_target.load(std::memory_order_acquire) == this) {
        recorder.Raw("gpu_pass_profile", std::source_location::current());
        GpuPassBegin(this, current_cmdbuf, render_state);
    }
    Commands().beginRendering(rendering_info);
}

void Scheduler::EndRendering(std::source_location site) {
    if (!is_rendering) {
        return;
    }
    if (Perf::Probe() && render_state.num_color_attachments >= 4) [[unlikely]] {
        if (!probe_break_noted) {
            Perf::NotePassBreak(0); // an explicit end: the stack says who
        }
    }
    if (pass_stats) [[unlikely]] {
        pass_stats->NoteExplicitEnd(render_state, site);
    }
    EndRenderingNow();
}

void Scheduler::NoteBarrierBreak(bool read_after_read) {
    if (pass_stats && is_rendering) {
        ++pass_stats->barrier_breaks;
        pass_stats->read_only_barrier_breaks += read_after_read ? 1 : 0;
    }
}

void Scheduler::EndRenderingNow() {
    if (!is_rendering) {
        return;
    }
    probe_break_noted = false;
    subset_key_valid = false;
    is_rendering = false;
    Commands().endRendering();
    if (pass_target.load(std::memory_order_acquire) == this) {
        recorder.Raw("gpu_pass_profile", std::source_location::current());
        GpuPassEnd(this, current_cmdbuf);
    } // VR_GPU_PASSES only
}

void Scheduler::Flush(SubmitInfo& info) {
    // When flushing, we only send data to the driver; no waiting is necessary.
    SubmitExecution(info);
}

void Scheduler::Flush() {
    SubmitInfo info{};
    Flush(info);
}

// FAST bit 56 (gpu_plan.md #7, research_gpu_gaps.md item 2). The guest's SubmitDone task
// (Liverpool's finish) runs right after the GPU-idle flush and drain, so its command buffer is
// normally empty. Submitting it anyway costs one queue boundary per frame (20-47 us of GPU gap
// at 2000x2160) and makes the idle task's drain wait for a vkQueueSubmit before the GpuIdle IRQ.
// Not submitting keeps ticks dense: the open tick is signalled by the next submission, with real
// work, and Wait() still flushes it for anyone who needs it sooner. bbport keeps this flush
// (BBV/amdgpu/liverpool.cpp:247-262). The check runs per frame end, so the bit may flip between
// frames.
static constexpr auto SkipEmptyFinish = static_cast<FastRender::Bit>(56);

namespace {
// FAST_SKIP_EMPTY_FINISH_VERIFY=N: every Nth skippable frame end is submitted as before, and the
// replay checks that its command buffer held only the packets counted as the profiler's stamps and
// that no deferred guest signal waited for it. It cannot see commands recorded around the recorder;
// CheckEmpty falls back on those (raw borrows, recorder off).
u64 SkipEmptyFinishVerifyEvery() {
    static const u64 every = [] {
        const char* value = std::getenv("FAST_SKIP_EMPTY_FINISH_VERIFY");
        return value ? std::strtoull(value, nullptr, 0) : 0ULL;
    }();
    return every;
}
} // namespace

Scheduler::EmptyCheck Scheduler::CheckEmpty() const {
    if (recorder.RawBorrows() != empty_mark.raws)
        return EmptyCheck::Raw;
    if (!recorder.Enabled())
        return EmptyCheck::RecorderOff;
    if (recorder.CommandPackets() != empty_mark.commands)
        return EmptyCheck::Commands;
    if (is_rendering)
        return EmptyCheck::Rendering;
    // Encoded after the last submission (they complete only after the next one), or still
    // waiting for the replay of an earlier one: keep the old flush, as SyncRecorder does.
    // FAST_CPU_ASYNC_IDLE (Precise) and FAST_CPU_SIGNAL_AT_REPLAY: only the first kind (the
    // queued submission's replay writes the others; Liverpool's async GPU-idle task no longer
    // drains before this runs).
    if (SignalsNeedFlush())
        return EmptyCheck::Signals;
    const auto* copies = FastRender::active_host_copies.load(std::memory_order_acquire);
    if (copies != nullptr && copies->Pending())
        return EmptyCheck::HostCopies;
    return EmptyCheck::Empty;
}

void Scheduler::FlushFrameEnd() {
    if (!FastRender::On(SkipEmptyFinish)) {
        Flush();
        return;
    }
    auto& s = skip_finish;
    ++s.calls;
    EmptyCheck check = CheckEmpty();
    // No submission at all since the last skipped frame end (a frame without guest GPU work):
    // submit this one, so operations deferred to the open tick (GC frees, stream and pool
    // retirement; the priority thread waits on it without flushing) wait one frame end at most.
    // A frame with GPU work has its gpu_idle submission before this, so this never costs.
    if (check == EmptyCheck::Empty && skip_open_tick == CurrentTick())
        check = EmptyCheck::Repeat;
    const u64 verify_every = SkipEmptyFinishVerifyEvery();
    if (check != EmptyCheck::Empty) {
        ++s.kept[static_cast<size_t>(check)];
        Flush();
    } else if (verify_every != 0 && ++skip_verify_sequence % verify_every == 0) {
        ++s.verified;
        verify_empty_submit = true; // SubmitExecution hands the replay the packets it expects
        Flush();
    } else {
        // Nothing to end, submit or reset: the command buffer, its tick and the producer state
        // (dynamic state, push and bind memos) all stay valid for the work that comes next.
        ++s.skipped;
        skip_open_tick = CurrentTick();
        // As a submission would: the mid-frame cadence (vr.ini gpu_submit_ms) counts from here.
        cmdbuf_begin_time = std::chrono::steady_clock::now();
    }
    if (s.calls % 600 == 0)
        ReportSkipEmptyFinish();
}

void Scheduler::CheckEmptySubmit(u64 expected) {
    if (expected == NoEmptyCheck)
        return;
    const auto replayed = recorder.ReplayedCommands();
    empty_checked.fetch_add(1, std::memory_order_relaxed);
    // The replay's own view as well: no deferred guest write or IRQ rides on this submission
    // (CompleteGuestSignals completes them right after it), so not submitting it would have
    // delayed nothing the guest can see.
    const size_t riding_signals = deferred_signals.size();
    if (replayed.commands == expected && replayed.stamps == replayed.commands &&
        riding_signals == 0)
        return;
    if (empty_mismatches.fetch_add(1, std::memory_order_relaxed) < 8) {
        LOG_ERROR(Render_Vulkan,
                  "FAST skip_empty_finish verify: a skippable frame-end command buffer replayed {} "
                  "command packets ({} query resets or timestamps), {} expected; {} guest "
                  "signals waited for it",
                  replayed.commands, replayed.stamps, expected, riding_signals);
    }
}

void Scheduler::ReportSkipEmptyFinish() {
    auto& s = skip_finish;
    const auto kept = [&s](EmptyCheck check) { return s.kept[static_cast<size_t>(check)]; };
    LOG_INFO(Render_Vulkan,
             "FAST skip_empty_finish: frame_ends={} skipped={} ({:.3f} per frame end) verified={} "
             "kept: commands={} raw={} recorder_off={} rendering={} signals={} host_copies={} "
             "repeat={}; waits_on_skipped_tick={} verify_checked={} verify_mismatches={}",
             s.calls, s.skipped, static_cast<double>(s.skipped) / static_cast<double>(s.calls),
             s.verified, kept(EmptyCheck::Commands), kept(EmptyCheck::Raw),
             kept(EmptyCheck::RecorderOff), kept(EmptyCheck::Rendering),
             kept(EmptyCheck::Signals), kept(EmptyCheck::HostCopies), kept(EmptyCheck::Repeat),
             s.waited,
             empty_checked.load(std::memory_order_relaxed),
             empty_mismatches.load(std::memory_order_relaxed));
    s = {};
}

void Scheduler::Finish() {
    // When finishing, we need to wait for the submission to have executed on the device.
    const u64 presubmit_tick = CurrentTick();
    SubmitInfo info{};
    SubmitExecution(info);
    recorder.Drain("finish");
    Wait(presubmit_tick);
}

void Scheduler::WaitSubmitted() {
    const u64 tick = CurrentTick();
    if (tick > 1) {
        Wait(tick - 1);
    }
}

void Scheduler::Wait(u64 tick) {
    ScratchProfile::Cpu scratch_cpu(13, "Scheduler::Wait");
    // Stream-ring allocation calls this very frequently for already retired watches.
    if (master_semaphore.IsFree(tick))
        return;
    if (tick >= master_semaphore.CurrentTick()) {
        // FAST bit 56: someone needs the tick a skipped frame end left open, still empty.
        if (skip_open_tick != 0 && tick == skip_open_tick && CheckEmpty() == EmptyCheck::Empty)
            ++skip_finish.waited;
        // FAST bit 62: a forced flush of the open tick (the 'FAST submit_cadence' line).
        ++open_tick_flushes;
        if (deferred_cut_tick != 0 && deferred_cut_tick == master_semaphore.CurrentTick())
            ++deferred_tick_flushes;
        // Make sure we are not waiting for the current tick without signalling
        SubmitInfo info{};
        Flush(info);
    }
    recorder.Drain("tick_wait");
    master_semaphore.Wait(tick);
}

void Scheduler::PopPendingOperations(bool force_poll) {
    const bool limit = FastRender::On(FastRender::PendingPollLimit);
    if (limit && !has_pending_ops.load(std::memory_order_acquire)) {
        return;
    }
    std::unique_lock lk(pending_ops_mutex);
    // Called on every draw: query the GPU's semaphore only when an operation waits on a tick not
    // yet known to be done (the query is a driver call; the profile showed it on every draw).
    if (pending_ops.empty()) {
        return;
    }
    if (!master_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        // bbport amortizes this driver query across 32 draws/dispatches. Submissions
        // still poll immediately, so sparse workloads do not defer cleanup indefinitely.
        if (limit && !force_poll && (++pending_polls & 31) != 0) {
            return;
        }
        pending_polls = 0;
        master_semaphore.Refresh();
    }
    LockTrace::Phase phase{"deferred operations"}; // VR_LOCK_TRACE (in a submit's hold)
    while (!pending_ops.empty() && master_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
        phase.Count(1);
    }
    // DeferOperation uses this same mutex: an enqueue cannot lose its publication.
    has_pending_ops.store(!pending_ops.empty(), std::memory_order_release);
}

void Scheduler::AllocateWorkerCommandBuffers(u64 tick) {
    const vk::CommandBufferBeginInfo begin_info = {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    };

    current_cmdbuf = command_pool.Commit(tick);
    Check(current_cmdbuf.begin(begin_info));
    GpuTimerBegin(this, current_cmdbuf); // VR_GPU_TIMING only

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        static const auto scope_loc =
            GPU_SCOPE_LOCATION("Guest Frame", MarkersPalette::GpuMarkerColor);
        new (profiler_scope) tracy::VkCtxScope{profiler_ctx, &scope_loc, current_cmdbuf, true};
    }
#endif
}

void Scheduler::ResetProducerState() {
    ScratchPush::Invalidate();
    dynamic_state.Invalidate();
    cmdbuf_begin_time = std::chrono::steady_clock::now();
}

void Scheduler::SubmitExecution(SubmitInfo& info) {
    ScratchProfile::Cpu scratch_cpu(14, "Scheduler::SubmitExecution");
    if (pre_submit_hook) {
        pre_submit_hook(); // before the submit: the GPU reads what it wrote
    }
    EndRendering();
    // FAST_GPU_PASS_PROFILE (bit 38): the gap until the next command buffer has its own label
    // (bbport vk_scheduler.cpp:930-934); the label open now continues in the next one.
    GpuPassProfiler* const pass_profiler = GpuPassProfiler::For(*this);
    const u64 pass_resume = pass_profiler != nullptr ? pass_profiler->SubmitBegin() : 0;
    // FAST_SKIP_EMPTY_FINISH_VERIFY: a frame end FlushFrameEnd found empty holds only the
    // profiler's resume and gap stamps, every command packet encoded into this command buffer.
    u64 expect_commands = NoEmptyCheck;
    if (verify_empty_submit) [[unlikely]] {
        verify_empty_submit = false;
        expect_commands = recorder.CommandPackets() - buffer_commands;
    }
    // Allocate the logical tick on the producer; resource retirement and stream watches
    // must refer to encoded work even while Vulkan recording is behind it.
    const u64 tick = master_semaphore.NextTick();
    // FAST bit 62: the guest signals encoded so far ride on this submission.
    signals_at_submit = signals_encoded;
    ++submit_count;
    if (recorder.Threaded()) {
        const RecorderSubmit submit{info, tick, FastRender::FlushHostCopies(), expect_commands};
        recorder.QueueControl(SubmitCommands, &submit, sizeof(submit));
    } else {
        recorder.Drain("submit");
        FastRender::WaitHostCopies(FastRender::CopyWait::Submit);
        CheckEmptySubmit(expect_commands);
        SubmitLocked(info, tick);
    }
    ResetProducerState();
    recorder.Configure();
    buffer_commands = recorder.CommandPackets(); // FAST bit 56: the new command buffer
    if (pass_profiler != nullptr) [[unlikely]] {
        pass_profiler->Resume(pass_resume);
    }
    // FAST bit 56: the profiler's resume stamps do not make the command buffer non-empty.
    empty_mark = {recorder.CommandPackets(), recorder.RawBorrows()};

    // Apply pending operations, once the queue lock is released: they free what the GPU has
    // finished with (images, buffers, views, staging memory: 2-4 ms during an area load, which the
    // headset's frame thread and the desktop present used to wait behind). None of them touches a
    // queue; pending_ops_mutex guards the list, and the same operations already run without the
    // queue lock on every draw and dispatch (Rasterizer::Draw / DispatchDirect...), so no other
    // thread relies on submit_mutex to keep them out.
    PopPendingOperations(true);
}

void Scheduler::SubmitLocked(SubmitInfo info, u64 signal_value) {
    const Common::Log::DeferScope log_defer; // its log lines: once the lock is released
    const LockTrace::Start lock_start;
    std::scoped_lock lk{submit_mutex};
    LockTrace::Hold trace{"game submit", lock_start}; // VR_LOCK_TRACE only: before lk is released

#if TRACY_GPU_ENABLED
    auto* profiler_ctx = instance.GetProfilerContext();
    if (profiler_ctx) {
        profiler_scope->~VkCtxScope();
        TracyVkCollect(profiler_ctx, current_cmdbuf);
    }
#endif

    {
        LockTrace::Phase phase{"end command buffer"};
        GpuTimerEnd(this, current_cmdbuf, signal_value); // VR_GPU_TIMING only
        Check(current_cmdbuf.end());
        recorder.ResetReplayedCommands(); // FAST bit 56 verify: the census is per command buffer
    }

    const vk::Semaphore timeline = master_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    static constexpr std::array<vk::PipelineStageFlags, 3> wait_stage_masks = {
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::PipelineStageFlagBits::eAllCommands,
    };

    const vk::TimelineSemaphoreSubmitInfo timeline_si = {
        .waitSemaphoreValueCount = info.num_wait_semas,
        .pWaitSemaphoreValues = info.wait_ticks.data(),
        .signalSemaphoreValueCount = info.num_signal_semas,
        .pSignalSemaphoreValues = info.signal_ticks.data(),
    };

    const vk::SubmitInfo submit_info = {
        .pNext = &timeline_si,
        .waitSemaphoreCount = info.num_wait_semas,
        .pWaitSemaphores = info.wait_semas.data(),
        .pWaitDstStageMask = wait_stage_masks.data(),
        .commandBufferCount = 1U,
        .pCommandBuffers = &current_cmdbuf,
        .signalSemaphoreCount = info.num_signal_semas,
        .pSignalSemaphores = info.signal_semas.data(),
    };

    {
        LockTrace::Phase phase{"imgui texture upload"};
        ImGui::Core::TextureManager::Submit();
    }
    vk::Result submit_result{};
    {
        LockTrace::Phase phase{"vkQueueSubmit"};
        submit_result = instance.GetGraphicsQueue().submit(submit_info, info.fence);
    }
    ASSERT_MSG(submit_result != vk::Result::eErrorDeviceLost, "Device lost during submit");

    {
        LockTrace::Phase phase{"semaphore refresh"};
        master_semaphore.Refresh();
    }
    {
        LockTrace::Phase phase{"next command buffer"};
        AllocateWorkerCommandBuffers(signal_value + 1); // replay owns pool/timestamps
    }
}

void Scheduler::PriorityPendingOpsThread(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuSchedPriorityPendingOpsRunner");

    while (!stoken.stop_requested()) {
        PendingOp op;
        {
            std::unique_lock lk(priority_pending_ops_mutex);
            priority_pending_ops_cv.wait(lk, stoken,
                                         [this] { return !priority_pending_ops.empty(); });
            if (stoken.stop_requested()) {
                break;
            }

            op = std::move(priority_pending_ops.front());
            priority_pending_ops.pop();
        }

        master_semaphore.Wait(op.gpu_tick);
        if (stoken.stop_requested()) {
            break;
        }

        op.callback();
    }
}

void DynamicState::Commit(const Instance& instance, const RecorderProxy& cmdbuf) {
    if (dirty_state.viewports) {
        dirty_state.viewports = false;
        cmdbuf.setViewportWithCount(viewports);
    }
    if (dirty_state.scissors) {
        dirty_state.scissors = false;
        cmdbuf.setScissorWithCount(scissors);
    }
    if (dirty_state.depth_test_enabled) {
        dirty_state.depth_test_enabled = false;
        cmdbuf.setDepthTestEnable(depth_test_enabled);
    }
    if (dirty_state.depth_write_enabled) {
        dirty_state.depth_write_enabled = false;
        // Note that this must be set in a command buffer even if depth test is disabled.
        cmdbuf.setDepthWriteEnable(depth_write_enabled);
    }
    if (depth_test_enabled && dirty_state.depth_compare_op) {
        dirty_state.depth_compare_op = false;
        cmdbuf.setDepthCompareOp(depth_compare_op);
    }
    if (dirty_state.depth_bounds_test_enabled) {
        dirty_state.depth_bounds_test_enabled = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBoundsTestEnable(depth_bounds_test_enabled);
        }
    }
    if (depth_bounds_test_enabled && dirty_state.depth_bounds) {
        dirty_state.depth_bounds = false;
        if (instance.IsDepthBoundsSupported()) {
            cmdbuf.setDepthBounds(depth_bounds_min, depth_bounds_max);
        }
    }
    if (dirty_state.depth_bias_enabled) {
        dirty_state.depth_bias_enabled = false;
        cmdbuf.setDepthBiasEnable(depth_bias_enabled);
    }
    if (depth_bias_enabled && dirty_state.depth_bias) {
        dirty_state.depth_bias = false;
        cmdbuf.setDepthBias(depth_bias_constant, depth_bias_clamp, depth_bias_slope);
    }
    if (dirty_state.stencil_test_enabled) {
        dirty_state.stencil_test_enabled = false;
        cmdbuf.setStencilTestEnable(stencil_test_enabled);
    }
    if (stencil_test_enabled) {
        if (dirty_state.stencil_front_ops && dirty_state.stencil_back_ops &&
            stencil_front_ops == stencil_back_ops) {
            dirty_state.stencil_front_ops = false;
            dirty_state.stencil_back_ops = false;
            cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFrontAndBack, stencil_front_ops.fail_op,
                                stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                stencil_front_ops.compare_op);
        } else {
            if (dirty_state.stencil_front_ops) {
                dirty_state.stencil_front_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eFront, stencil_front_ops.fail_op,
                                    stencil_front_ops.pass_op, stencil_front_ops.depth_fail_op,
                                    stencil_front_ops.compare_op);
            }
            if (dirty_state.stencil_back_ops) {
                dirty_state.stencil_back_ops = false;
                cmdbuf.setStencilOp(vk::StencilFaceFlagBits::eBack, stencil_back_ops.fail_op,
                                    stencil_back_ops.pass_op, stencil_back_ops.depth_fail_op,
                                    stencil_back_ops.compare_op);
            }
        }
        if (dirty_state.stencil_front_reference && dirty_state.stencil_back_reference &&
            stencil_front_reference == stencil_back_reference) {
            dirty_state.stencil_front_reference = false;
            dirty_state.stencil_back_reference = false;
            cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_reference);
        } else {
            if (dirty_state.stencil_front_reference) {
                dirty_state.stencil_front_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_reference);
            }
            if (dirty_state.stencil_back_reference) {
                dirty_state.stencil_back_reference = false;
                cmdbuf.setStencilReference(vk::StencilFaceFlagBits::eBack, stencil_back_reference);
            }
        }
        if (dirty_state.stencil_front_write_mask && dirty_state.stencil_back_write_mask &&
            stencil_front_write_mask == stencil_back_write_mask) {
            dirty_state.stencil_front_write_mask = false;
            dirty_state.stencil_back_write_mask = false;
            cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                       stencil_front_write_mask);
        } else {
            if (dirty_state.stencil_front_write_mask) {
                dirty_state.stencil_front_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eFront,
                                           stencil_front_write_mask);
            }
            if (dirty_state.stencil_back_write_mask) {
                dirty_state.stencil_back_write_mask = false;
                cmdbuf.setStencilWriteMask(vk::StencilFaceFlagBits::eBack, stencil_back_write_mask);
            }
        }
        if (dirty_state.stencil_front_compare_mask && dirty_state.stencil_back_compare_mask &&
            stencil_front_compare_mask == stencil_back_compare_mask) {
            dirty_state.stencil_front_compare_mask = false;
            dirty_state.stencil_back_compare_mask = false;
            cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFrontAndBack,
                                         stencil_front_compare_mask);
        } else {
            if (dirty_state.stencil_front_compare_mask) {
                dirty_state.stencil_front_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eFront,
                                             stencil_front_compare_mask);
            }
            if (dirty_state.stencil_back_compare_mask) {
                dirty_state.stencil_back_compare_mask = false;
                cmdbuf.setStencilCompareMask(vk::StencilFaceFlagBits::eBack,
                                             stencil_back_compare_mask);
            }
        }
    }
    if (dirty_state.primitive_restart_enable) {
        dirty_state.primitive_restart_enable = false;
        cmdbuf.setPrimitiveRestartEnable(primitive_restart_enable);
    }
    if (dirty_state.rasterizer_discard_enable) {
        dirty_state.rasterizer_discard_enable = false;
        cmdbuf.setRasterizerDiscardEnable(rasterizer_discard_enable);
    }
    if (dirty_state.cull_mode) {
        dirty_state.cull_mode = false;
        cmdbuf.setCullMode(cull_mode);
    }
    if (dirty_state.front_face) {
        dirty_state.front_face = false;
        cmdbuf.setFrontFace(front_face);
    }
    if (dirty_state.blend_constants) {
        dirty_state.blend_constants = false;
        cmdbuf.setBlendConstants(blend_constants.data());
    }
    if (dirty_state.color_write_masks) {
        dirty_state.color_write_masks = false;
        if (instance.IsDynamicColorWriteMaskSupported()) {
            cmdbuf.setColorWriteMaskEXT(0, color_write_masks);
        }
    }
    if (dirty_state.line_width) {
        dirty_state.line_width = false;
        cmdbuf.setLineWidth(line_width);
    }
    if (dirty_state.feedback_loop_enabled && instance.IsAttachmentFeedbackLoopLayoutSupported()) {
        dirty_state.feedback_loop_enabled = false;
        cmdbuf.setAttachmentFeedbackLoopEnableEXT(feedback_loop_enabled
                                                      ? vk::ImageAspectFlagBits::eColor
                                                      : vk::ImageAspectFlagBits::eNone);
    }
}

} // namespace Vulkan
