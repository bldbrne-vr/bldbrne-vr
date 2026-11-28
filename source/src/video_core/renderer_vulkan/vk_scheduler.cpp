#include "core/vr/s1_dynamic_resolution.h"
#include "video_core/host_copies.h"
#include "video_core/flip_order_trace.h"
#include "video_core/native_stereo/native_stereo.h"
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
#include <future>
#include <thread>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <source_location>
#include <string>
#include <unordered_map>
#include <vector>
#include <boost/container/static_vector.hpp>
#include <immintrin.h>
#include "common/assert.h"
#include "common/debug.h"
#include "common/logging/log.h"
#include "common/scope_exit.h"
#include "common/thread.h"
#include "core/memory.h"
#include "video_core/renderer_vulkan/vk_gpu_crash.h"
#include "core/platform.h"
#include "imgui/renderer/texture_manager.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderer_vulkan/vk_gpu_frame_timer.h"
#include "video_core/renderer_vulkan/vk_gpu_pass_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vr_foveation.h"
#include "video_core/renderer_vulkan/vr_eye_axes.h"
#include "core/vr/eye_cant.h"
#include "core/vr/eye_fov_cache.h"
#include "core/vr/graphics_settings.h"
#include "core/vr/vr_session.h"
#include <vk_mem_alloc.h>
#include "video_core/renderer_vulkan/vk_pass_breaks.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_submit_lock_trace.h"
#include "video_core/renderer_vulkan/vr_perf.h"
#include "shader_recompiler/ir/passes/light_shadow.h"

namespace Vulkan {

bool VrSceneMaskOn(); // (vk_pipeline_cache.h: the mask's switch and the session's lamp shadows)

// The draw scheduler owns its rate maps. Replaced maps retire at the recording tick,
// after every command referencing their image has completed.
struct FoveationResources {
    vk::Device device;
    VmaAllocator allocator{};
    VkImage image{};
    VmaAllocation image_allocation{};
    vk::ImageView view{};

    ~FoveationResources() {
        if (view) device.destroyImageView(view);
        if (image) vmaDestroyImage(allocator, image, image_allocation);
    }
};
struct FoveationMap {
    u32 width{}, height{};
    bool mono{};
    Foveation::Settings zones{};
    u32 drop{};
    bool from_fov{};
    std::unique_ptr<FoveationResources> resources;
};
constexpr size_t MaxFoveationMaps = 64;
struct FoveationCache {
    boost::container::static_vector<FoveationMap, MaxFoveationMaps> maps;
    size_t next_victim{};
    std::chrono::steady_clock::time_point retry_after{}, last_prebuild{};
    struct Done {
        u32 width, height;
        float inner, outer, ring;
        u32 floor;
    };
    std::vector<Done> prebuilt;
};

std::mutex Scheduler::submit_mutex;

namespace {

// VR_GPU_TIMING=1: the game's GPU time per frame (vk_gpu_frame_timer.h).
bool GpuTimingAsked() {
    static const bool wanted = [] {
        const char* value = std::getenv("VR_GPU_TIMING");
        return value != nullptr && *value != '\0' && *value != '0';
    }();
    return wanted;
}
// ... or the lamp shadows' trace dial, which reads LastGpuFrameMs (light_shadow.h SceneMaps::Dial),
// only with the lamp shadows on (VrSceneMaskOn: live since 2026-11-21, so a session started with
// the game's shadows times its frames from the moment ours are turned on).
bool GpuTimingWanted() {
    static const bool dials = [] {
        namespace Scene = Shader::Optimization::LightShadow::SceneMaps;
        return (Scene::TraceAll() && Scene::Dial() && Scene::TraceFarPinned() < 0.0f) ||
               Scene::StepDial();
    }();
    return GpuTimingAsked() || (dials && VrSceneMaskOn()) ||
           (S1Drs::enabled && S1Drs::timing_wanted.load(std::memory_order_relaxed));
}
// LastGpuFrameMs: the last frame read back, and the frames read so far.
std::atomic<float> g_last_gpu_ms{-1.0f};
std::atomic<unsigned> g_gpu_frames{0};

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
    std::vector<float> longest_ms; // the window: each frame's longest single command buffer
    u64 long_buffers{0};           // the window: command buffers longer than 2 ms
    std::chrono::steady_clock::time_point window_start{}; // its first frame (VR_GPU_TIMING_WINDOW_S)
    u64 buffers{0};
    u32 skipped{0};
};
GpuFrameTimer gpu_timer;
// The timer made (its query pool and target set, once, by the recording thread): read by
// GpuFrameTimerMark on another thread.
std::atomic<bool> g_gpu_timer_made{false};
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
    if (!GpuTimingWanted()) {
        return; // (not wanted now: nothing stamped; GpuFrameTimerMarkNow drops what is left)
    }
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
    const double longest_avg = avg(t.longest_ms);
    const double longest_max = *std::max_element(t.longest_ms.begin(), t.longest_ms.end());
    const double longest_p50 = rank(t.longest_ms, 0.5);
    const double longest_p99 = rank(t.longest_ms, 0.99);
    LOG_BENCHMARK_INFO(Render_Vulkan,
             "VR gpu frame: {} game frames: GPU busy per frame avg {:.2f} / p50 {:.2f} / p99 "
             "{:.2f} / max {:.2f} ms, span (first start -> last end) avg {:.2f} / max {:.2f} ms, "
             "idle before a frame avg {:.2f} ms; {:.1f} command buffers per frame, {} untimed",
             frames, busy_avg, busy_p50, busy_p99, busy_max, span_avg, span_max, gap_avg,
             static_cast<double>(t.buffers) / static_cast<double>(frames), t.skipped);
    // How long one command buffer holds the GPU (2026-11-21, the SteamVR stutter: SteamVR's
    // compositor over Link is an ordinary-priority client of Meta's runtime). LOG_INFO: compiled out
    // of release builds (BBVR_NO_LOGS), benchmark children included.
    LOG_INFO(Render_Vulkan,
             "VR gpu frame: longest command buffer per frame avg {:.2f} / p50 {:.2f} / p99 {:.2f} / "
             "max {:.2f} ms; {:.2f} buffers per frame over 2 ms",
             longest_avg, longest_p50, longest_p99, longest_max,
             static_cast<double>(t.long_buffers) / static_cast<double>(frames));
    GpuPassReport(t, frames);
    t.busy_ms.clear();
    t.span_ms.clear();
    t.gap_ms.clear();
    t.longest_ms.clear();
    t.long_buffers = 0;
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
    g_last_gpu_ms.store(static_cast<float>(static_cast<double>(busy) * to_ms), std::memory_order_relaxed);
    g_gpu_frames.fetch_add(1, std::memory_order_release);
    if (!GpuTimingAsked()) {
        // (The trace dial's only: no windows, no `VR gpu frame:` lines.)
        t.prev_end = last;
        t.spans.clear();
        return;
    }
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
    u64 longest = 0;
    for (const auto& [s, e] : t.spans) {
        longest = std::max(longest, e - s);
        t.long_buffers += static_cast<double>(e - s) * to_ms > 2.0 ? 1 : 0;
    }
    t.longest_ms.push_back(static_cast<float>(static_cast<double>(longest) * to_ms));
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
        // Not wanted now (the lamp shadows' dial, ours switched to the game's): the frames stamped
        // meanwhile are let go of, so the first frame read once it is wanted again is a new one
        // (the 2026-11-21 review: the slots filled while off, and the dial's first reading after
        // the switch back was a frame from before it). The recording thread, as GpuTimerBegin /
        // End; a slot is stamped again only after a reset recorded later on the same queue.
        auto& t = gpu_timer;
        if (t.target == &scheduler) {
            // (Under the lock before looking: GpuTimerEnd pushes under it.)
            std::scoped_lock lk{Scheduler::submit_mutex};
            for (const GpuFrameTimer::Pending& p : t.pending) {
                t.used[p.slot] = false;
            }
            t.pending.clear();
            t.spans.clear();
        }
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
        g_gpu_timer_made.store(true, std::memory_order_release);
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
enum RecorderControl : u32 { SubmitCommands, FrameMarker, GuestSignal, ReplayTask, Checkpoint };
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
// FAST_CPU_SIGNAL_AT_REPLAY: FlushGuestSignals spins up to this long (as the recorder's drain
// spin; 4096 pauses on a 5800X3D) for the replay to write the signals before it drains. A time,
// not a pause count: an Intel PAUSE lasts several times a Zen one.
constexpr auto SignalSpin = std::chrono::microseconds(70);
// FAST_CPU_SIGNAL_AT_REPLAY_VERIFY's counts: `verified` and the rest by the replay, `flushes`
// and `unwritten` by the producer (A or B, whichever owns the scheduler).
struct SignalReplayVerify {
    std::atomic<u64> verified{}, mismatches{}, own{}, window_ns{}, skipped{};
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
    // (Once the timer is made, every frame: not wanted, GpuFrameTimerMarkNow lets the frames
    // stamped before go, so the first frame read when it is wanted again is a new one.)
    if (GpuTimingWanted() || g_gpu_timer_made.load(std::memory_order_acquire))
        scheduler.RecordFrameMark();
}
float LastGpuFrameMs(unsigned* frames) {
    if (frames != nullptr) {
        *frames = g_gpu_frames.load(std::memory_order_acquire);
    }
    return g_last_gpu_ms.load(std::memory_order_relaxed);
}
// FAST_CPU_STAGEB_SPLIT (vk_scheduler.h OpenResolveBlob), read once at start-up: '1' the blob
// and the split halves on stage B alone, '2' also the encode half on fast:StageB2.
static const char g_stageb_split_mode = [] {
    const char* value = std::getenv("FAST_CPU_STAGEB_SPLIT");
    return value && (value[0] == '1' || value[0] == '2') ? value[0] : '0';
}();
static const bool g_stageb_split = g_stageb_split_mode != '0';
bool StageBSplitThreadRequested() {
    return g_stageb_split_mode == '2';
}

vk::CommandBuffer Scheduler::FlushBlob(void* context, std::string_view reason,
                                       std::source_location site, u32 mode) {
    auto& self = *static_cast<Scheduler*>(context);
    self.SplitSync("blob_flush");
    self.recorder.AppendBlob(*self.blob);
    if (mode == 2)
        return self.recorder.Raw(reason, site);
    if (mode == 1)
        self.recorder.Drain(reason, site);
    return {};
}

void Scheduler::StartSplitThread(SplitEncode encode, void* context) {
    if (!blob_recorder || split_threaded)
        return;
    for (auto& job : split_jobs)
        job.blob = std::make_unique<CommandRecorder>(current_cmdbuf, CommandRecorder::Blob{}, this,
                                                     FlushBlob);
    split_encode = encode;
    split_context = context;
    split_threaded = true;
    split_thread = std::jthread([this](std::stop_token stop) {
        t_on_stageb2 = true;
        FastRender::g_on_stage_b = true; // the recording checks treat it as stage B
        Common::SetCurrentThreadName("fast:StageB2");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
        Common::PreferPerformanceCores();
        u64 tail = 0;
        while (!stop.stop_requested()) {
            u64 head = split_head.load(std::memory_order_acquire);
            // Jobs come every few microseconds in a pass: spin a little before sleeping.
            for (u32 spin = 0; head == tail && spin < 4096; ++spin) {
                SplitPause();
                head = split_head.load(std::memory_order_acquire);
            }
            if (head == tail) {
                std::unique_lock lock{split_mutex};
                split_b2_sleeping.store(true, std::memory_order_seq_cst);
                while ((head = split_head.load(std::memory_order_seq_cst)) == tail &&
                       !stop.stop_requested()) {
                    split_b2_cv.wait_for(lock, std::chrono::milliseconds(2));
                }
                split_b2_sleeping.store(false, std::memory_order_relaxed);
                if (head == tail)
                    continue;
            }
            for (; tail != head; ++tail) {
                const u32 slot = u32(tail % SplitSlots);
                auto& job = split_jobs[slot];
                const u64 controls = job.blob->BlobEmpty() ? 0 : recorder.AppendBlob(*job.blob);
                if (job.draw)
                    split_encode(split_context, slot);
                // Guest signals reach the replay as soon as they would on one thread. Before the
                // tail moves: from then on the ring may be B1's.
                if (controls)
                    recorder.Publish();
                split_tail.store(tail + 1, std::memory_order_seq_cst);
                if (split_b1_waiting.load(std::memory_order_seq_cst)) {
                    std::scoped_lock lock{split_mutex};
                    split_b1_cv.notify_all();
                }
            }
        }
    });
    LOG_INFO(Render_Vulkan,
             "FAST_CPU_STAGEB_SPLIT=2: the draws' encode half on fast:StageB2 ({} job slots)",
             SplitSlots);
}

void Scheduler::SplitAcquire() {
    const u64 head = split_head.load(std::memory_order_relaxed);
    if (head - split_tail.load(std::memory_order_acquire) >= SplitSlots) {
        FastRender::BProfile::Scope b_wait{FastRender::BProfile::SplitWait};
        // Every slot queued: B1 waits for the oldest.
        const u64 want = head - SplitSlots + 1;
        for (u32 spin = 0; split_tail.load(std::memory_order_acquire) < want && spin < 8192;
             ++spin)
            SplitPause();
        if (split_tail.load(std::memory_order_acquire) < want) {
            std::unique_lock lock{split_mutex};
            split_b1_waiting.store(true, std::memory_order_seq_cst);
            while (split_tail.load(std::memory_order_seq_cst) < want)
                split_b1_cv.wait_for(lock, std::chrono::milliseconds(2));
            split_b1_waiting.store(false, std::memory_order_relaxed);
        }
        ++split_full_waits;
    }
    blob = split_jobs[head % SplitSlots].blob.get();
}

void Scheduler::SplitSubmit(bool draw) {
    ASSERT(blob_open && split_threaded);
    blob_open = false;
    const u64 head = split_head.load(std::memory_order_relaxed);
    auto& job = split_jobs[head % SplitSlots];
    if (!draw && job.blob->BlobEmpty())
        return; // nothing to hand over: the slot stays B1's for the next draw
    job.draw = draw;
    ++(draw ? split_jobs_drawn : split_jobs_blob);
    if constexpr (Common::Log::Enabled) {
        if (draw && (split_jobs_drawn & 0x7FFFF) == 0)
            SplitReport(); // every ~524k draws: a few seconds
    }
    split_head.store(head + 1, std::memory_order_seq_cst);
    if (split_b2_sleeping.load(std::memory_order_seq_cst)) {
        std::scoped_lock lock{split_mutex};
        split_b2_cv.notify_one();
    }
}

void Scheduler::SplitWait(std::string_view reason) {
    FastRender::BProfile::Scope b_wait{FastRender::BProfile::SplitWait};
    const u64 want = split_head.load(std::memory_order_relaxed);
    for (u32 spin = 0; split_tail.load(std::memory_order_acquire) != want && spin < 16384; ++spin)
        SplitPause();
    if (split_tail.load(std::memory_order_acquire) != want) {
        std::unique_lock lock{split_mutex};
        split_b1_waiting.store(true, std::memory_order_seq_cst);
        while (split_tail.load(std::memory_order_seq_cst) != want)
            split_b1_cv.wait_for(lock, std::chrono::milliseconds(2));
        split_b1_waiting.store(false, std::memory_order_relaxed);
    }
    ++split_waits;
    if constexpr (Common::Log::Enabled) {
        ++split_reasons[reason];
    }
}

void Scheduler::SplitReport() {
    std::string text;
    for (const auto& [name, count] : split_reasons)
        text += fmt::format(" {}={}", name, count);
    LOG_INFO(Render_Vulkan, "FAST_CPU_STAGEB_SPLIT=2: draws {} blob jobs {} full waits {} syncs {}:{}",
             split_jobs_drawn, split_jobs_blob, split_full_waits, split_waits, text);
}

void Scheduler::RecordFrameMark() {
    if (Main().Threaded())
        Main().QueueControl(FrameMarker, nullptr, 0);
    else {
        Main().Drain("frame_marker");
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
    case Checkpoint: {
        u64 id;
        std::memcpy(&id, payload, sizeof(id));
        self.current_cmdbuf.setCheckpointNV(reinterpret_cast<const void*>(static_cast<uintptr_t>(id)));
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
    for (size_t i = 0; i < deferred_signals.size(); ++i) {
        const auto& p = deferred_signals[i];
        if (g_signal_at_replay_verify) [[unlikely]] {
            // The latest earlier signal of this submission to the same bytes, written just now.
            std::optional<u64> own;
            for (size_t j = i; j-- > 0;) {
                const auto& q = deferred_signals[j];
                if (q.bytes && q.address == p.address && q.bytes == p.bytes) {
                    own = q.written;
                    break;
                }
            }
            CheckSignalReplayed(p, own);
        }
        deferred_signals[i].written = WriteGuestSignal(p);
    }
    deferred_signals.clear();
}
u64 Scheduler::WriteGuestSignal(const DeferredGuestSignal& p) {
    u64 value = 0;
    if (p.bytes) {
        void* address = reinterpret_cast<void*>(p.address);
        value = p.source == GuestSignalValue::GpuClock      ? AmdGpu::GetGpuClock64()
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
    return value;
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
void Scheduler::CheckSignalReplayed(const DeferredGuestSignal& p, std::optional<u64> own) {
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
    const u64 mask = p.bytes == 8 ? ~u64{0} : (u64{1} << (8 * p.bytes)) - 1;
    if (now_bytes != p.replayed_bytes && own && now_bytes == (*own & mask)) {
        // An earlier signal of this submission wrote them; at replay it comes first as well.
        v.own.fetch_add(1, std::memory_order_relaxed);
    } else if (now_bytes != p.replayed_bytes) {
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
                 "FAST_CPU_SIGNAL_AT_REPLAY verify: signals={} mismatches={} same_stream={} "
                 "interrupt_only={} earlier_by_us_avg={:.1f}; guest-write orderings={} of which the switch would "
                 "still wait for {} (the others neither submit nor wait)",
                 verified, v.mismatches.load(std::memory_order_relaxed),
                 v.own.load(std::memory_order_relaxed), v.skipped.load(std::memory_order_relaxed),
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
    Main().QueueControl(ReplayTask, &packet, sizeof(packet), true, false);
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
    Main().QueueControl(GuestSignal, &signal, sizeof(signal), g_signal_at_replay, false);
    // Bound the separate deferred-signal queue even for a command stream with no draws.
    if (signals_encoded - signals_completed.load(std::memory_order_acquire) >= 2048) {
        if (!g_signal_at_replay) // written at replay: the drain alone writes them
            Flush();
        Main().Drain("signal_capacity");
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
            Main().Publish(); // FAST_CPU_TASK_PUBLISH: replay while the submission is encoded
        Flush();
    }
    Main().Drain(reason);
}
// FAST_CPU_SIGNAL_AT_REPLAY: the replay writes each signal when it reaches its packet, so a guest
// write that must follow them waits for that and nothing else: no submission, and not for the
// commands encoded after the newest signal.
void Scheduler::WaitGuestSignals(std::string_view reason) {
    if (!recorder.Threaded()) {
        Main().Drain(reason); // inline replay writes them
        return;
    }
    const u64 target = signals_encoded;
    Main().Publish(); // already published by SignalGuest; cheap when nothing is new
    ++at_replay_flushes;
    bool written = false;
    const auto until = std::chrono::steady_clock::now() + SignalSpin;
    for (u32 pause = 1;; ++pause) {
        if (signals_completed.load(std::memory_order_acquire) >= target) {
            written = true;
            break;
        }
        _mm_pause();
        if ((pause & 63) == 0 && std::chrono::steady_clock::now() >= until)
            break;
    }
    if (written) {
        ++at_replay_spun;
    } else {
        ++at_replay_drained;
        Main().Drain(reason);
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
    crash_source = static_cast<u8>(allow_recorder ? GpuCrash::Source::Game
                                                  : GpuCrash::Source::Presenter);
    if (allow_recorder) {
        GpuCrash::WatchTimeline(master_semaphore.Handle()); // the game's ticks, for the reports
    }
    if (allow_recorder && PassBreaks::StatsEnabled()) {
        pass_stats = std::make_unique<PassBreakStats>();
    }
    recorder.SetControlHandler(this, ReplayRecorderControl);
    if (allow_recorder && g_stageb_split) {
        blob_recorder = std::make_unique<CommandRecorder>(current_cmdbuf, CommandRecorder::Blob{},
                                                          this, FlushBlob);
        blob = blob_recorder.get();
    }
    AllocateWorkerCommandBuffers(master_semaphore.CurrentTick());
    ResetProducerState();
    priority_pending_ops_thread =
        std::jthread(std::bind_front(&Scheduler::PriorityPendingOpsThread, this));
}

Scheduler::~Scheduler() {
    if (split_thread.joinable()) {
        split_thread.request_stop();
        {
            std::scoped_lock lock{split_mutex};
            split_b2_cv.notify_all();
        }
        split_thread.join();
    }
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
    Main().Raw("gpu_pass_profile", std::source_location::current());
    if (t.open < 0 || t.pass_open || is_rendering)
        return false;
    GpuRecBegin(current_cmdbuf, {key, 0, 0, 0, 254, 0, 0});
    return true;
}

void Scheduler::GpuCheckpoint(u8 kind, u64 a, u32 x, u32 y, u32 extra) {
    const u64 id = GpuCrash::Mark(static_cast<GpuCrash::Kind>(kind), CurrentTick(), a, x, y, extra,
                                  static_cast<GpuCrash::Source>(crash_source));
    if (id == 0) {
        return;
    }
    // The recorder Commands() would give (so the marker keeps its place among the commands).
    CommandRecorder* rec = &recorder;
    if (split_threaded && t_on_stageb2) {
        rec = &recorder;
    } else if (blob_open) {
        rec = blob;
    } else {
        SplitSync("gpu_checkpoint");
    }
    if (rec->IsBlob() || rec->Enabled()) {
        // Its replay records only the checkpoint: held push constants stay held.
        rec->QueueControl(Checkpoint, &id, sizeof(id), false, false);
    } else {
        rec->Direct().setCheckpointNV(reinterpret_cast<const void*>(static_cast<uintptr_t>(id)));
    }
}

void Scheduler::GpuWorkBegin(u64 key, u32 x, u32 y) {
    if (GpuCrash::CheckpointsOn()) [[unlikely]] {
        GpuCheckpoint(static_cast<u8>(GpuCrash::Kind::Work), key, x, y);
    }
    auto& t = gpu_timer;
    if (pass_target.load(std::memory_order_acquire) != this)
        return;
    Main().Raw("gpu_pass_profile", std::source_location::current());
    if (t.open < 0 || t.pass_open || is_rendering) {
        return;
    }
    Main().Drain("gpu_pass_profile");
    GpuRecBegin(current_cmdbuf, {key, 0, static_cast<u16>(std::min<u32>(x, 65535)),
                                 static_cast<u16>(std::min<u32>(y, 65535)), 255, 0, 0});
}

void Scheduler::GpuWorkEnd() {
    if (pass_target.load(std::memory_order_acquire) == this) {
        Main().Raw("gpu_pass_profile", std::source_location::current());
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

namespace {
// VR_FOVEATION_STEPS="<mode>@<s>,..." (tests only): vr.ini foveation set to <mode> <s> seconds
// after the first foveated pass, as the VR page sets it ("0@0,1@20,3@40": off, on, light).
void FoveationSteps() {
    struct Step {
        int mode;
        double at_s;
    };
    static const std::vector<Step> steps = [] {
        std::vector<Step> out;
        const char* p = std::getenv("VR_FOVEATION_STEPS");
        while (p != nullptr && *p != 0) {
            char* end{};
            const long mode = std::strtol(p, &end, 10);
            if (end == p || *end != '@')
                break;
            const char* at = end + 1;
            const double s = std::strtod(at, &end);
            if (end == at)
                break;
            out.push_back({int(mode), s});
            p = *end == ',' ? end + 1 : end;
        }
        return out;
    }();
    if (steps.empty())
        return;
    static const auto start = std::chrono::steady_clock::now();
    static size_t next = 0;
    const double now_s =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    for (; next < steps.size() && now_s >= steps[next].at_s; ++next) {
        VR::Graphics::foveation.store(steps[next].mode, std::memory_order_relaxed);
        LOG_INFO(Render_Vulkan, "VR foveation: VR_FOVEATION_STEPS set foveation={} at {:.1f} s",
                 steps[next].mode, now_s);
    }
}

// The zones of the next eye-sized pass: the live vr.ini foveation (the VR page sets it), or the
// run's VR_FOVEATED. Off: no attachment, full rate.
int FoveationModeNow() {
    static const bool stepped = std::getenv("VR_FOVEATION_STEPS") != nullptr;
    if (stepped) FoveationSteps();
    return VR::Graphics::foveation.load(std::memory_order_relaxed);
}

FoveationMap* FindFoveationMap(FoveationCache& cache, u32 width, u32 height, bool mono, const Foveation::Settings& zones,
                               u32 drop) {
    for (auto& m : cache.maps) {
        if (m.width == width && m.height == height && m.mono == mono &&
            m.zones.inner == zones.inner && m.zones.outer == zones.outer &&
            m.zones.ring == zones.ring && m.drop == drop) {
            return &m;
        }
    }
    return nullptr;
}
vk::ImageView FoveationView(FoveationCache& cache, Scheduler& scheduler, const Instance& instance,
                            CommandRecorder& recorder, u32 width,
                            u32 height, bool mono, const Foveation::Settings& zones, u32 drop) {
    auto& maps = cache.maps;
    FoveationMap* existing = FindFoveationMap(cache, width, height, mono, zones, drop);
    // The usual case, every eye-sized pass: the map is final. No head read (GetHead locks the
    // head state the OpenXR thread writes; read per pass, it cost the recording thread ~1 ms a
    // frame).
    if (existing && existing->from_fov) {
        return existing->resources->view;
    }
    using Clock = std::chrono::steady_clock;
    auto& retry_after = cache.retry_after;
    if (retry_after != Clock::time_point{}) {
        if (Clock::now() < retry_after) {
            return existing ? existing->resources->view : vk::ImageView{};
        }
        retry_after = {};
    }
    const auto head = VR::GetHead();
    const bool fov =
        mono || (VR::EyeFov::Valid(head.eye_fov[0]) && VR::EyeFov::Valid(head.eye_fov[1]));
    if (existing && !fov) {
        return existing->resources->view;
    }
    const auto texel = instance.GetFoveationTexel();
    const u32 layers = !mono && instance.IsFoveationLayered() ? 2 : 1;
    const u32 nx = (width + texel.width - 1) / texel.width;
    const u32 ny = (height + texel.height - 1) / texel.height;
    std::array<float, 2> cx{0.5f, 0.5f}, cy{0.5f, 0.5f};
    if (fov && !mono)
        EyeAxes(head, cx, cy); // the lens centres (vr_eye_axes.h)
    if (layers == 1 && !mono) { // one map for both eyes: between the two axes
        cx[0] = 0.5f * (cx[0] + cx[1]);
        cy[0] = 0.5f * (cy[0] + cy[1]);
    }
    const size_t layer_bytes = size_t(nx) * ny;
    const VmaAllocator allocator = instance.GetAllocator();
    const vk::ImageCreateInfo image_ci{
        .imageType = vk::ImageType::e2D,
        .format = vk::Format::eR8Uint,
        .extent = {nx, ny, 1},
        .mipLevels = 1,
        .arrayLayers = layers,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eFragmentShadingRateAttachmentKHR |
                 vk::ImageUsageFlagBits::eTransferDst,
    };
    const VmaAllocationCreateInfo image_alloc{.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
    VkImage image{};
    VmaAllocation image_allocation{};
    const auto failed = [&] {
        retry_after = Clock::now() + std::chrono::seconds(1);
        return existing ? existing->resources->view : vk::ImageView{};
    };
    if (vmaCreateImage(allocator, reinterpret_cast<const VkImageCreateInfo*>(&image_ci),
                       &image_alloc, &image, &image_allocation, nullptr) != VK_SUCCESS) {
        return failed();
    }
    auto release_image = SCOPE_GUARD { vmaDestroyImage(allocator, image, image_allocation); };
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = layer_bytes * layers,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    const VmaAllocationCreateInfo buffer_alloc{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO};
    VkBuffer buffer{};
    VmaAllocation buffer_allocation{};
    VmaAllocationInfo buffer_info{};
    if (vmaCreateBuffer(allocator, &buffer_ci, &buffer_alloc, &buffer, &buffer_allocation,
                        &buffer_info) != VK_SUCCESS) {
        return failed();
    }
    auto upload = detail::MakeScopeGuard([allocator, buffer, buffer_allocation] {
        vmaDestroyBuffer(allocator, buffer, buffer_allocation);
    });
    if (!buffer_info.pMappedData) {
        return failed();
    }
    auto* mapped = static_cast<u8*>(buffer_info.pMappedData);
    for (u32 layer = 0; layer < layers; ++layer) {
        if (!Foveation::WriteMap({mapped + layer * layer_bytes, layer_bytes}, width, height,
                                 texel.width, texel.height, cx[layer], cy[layer], zones,
                                 instance.HasFoveation4x4(), float(100 - drop) / 100.0f))
            return failed();
    }
    if (vmaFlushAllocation(allocator, buffer_allocation, 0, VK_WHOLE_SIZE) != VK_SUCCESS) {
        return failed();
    }
    auto [view_result, view] = instance.GetDevice().createImageView({
        .image = image,
        .viewType = vk::ImageViewType::e2DArray,
        .format = vk::Format::eR8Uint,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers},
    });
    if (view_result != vk::Result::eSuccess) {
        return failed();
    }
    auto release_view = SCOPE_GUARD { instance.GetDevice().destroyImageView(view); };
    auto resources = std::make_unique<FoveationResources>();
    resources->device = instance.GetDevice();
    resources->allocator = allocator;
    resources->image = image;
    resources->image_allocation = image_allocation;
    resources->view = view;
    release_view.Cancel();
    // The staging allocation is only needed until this transfer completes.
    scheduler.DeferOperation([upload = std::move(upload)] {});
    release_image.Cancel();
    const vk::CommandBuffer cmd = recorder.Raw("foveation_map", std::source_location::current());
    vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eNone,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eTransferDstOptimal,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, layers},
    };
    cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                            .pImageMemoryBarriers = &barrier});
    std::array<vk::BufferImageCopy, 2> regions{};
    for (u32 layer = 0; layer < layers; ++layer) {
        regions[layer] = vk::BufferImageCopy{
            .bufferOffset = vk::DeviceSize(layer) * nx * ny,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, layer, 1},
            .imageExtent = {nx, ny, 1},
        };
    }
    cmd.copyBufferToImage(buffer, image, vk::ImageLayout::eTransferDstOptimal, layers,
                          regions.data());
    barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    barrier.srcAccessMask = vk::AccessFlagBits2::eTransferWrite;
    barrier.dstStageMask = vk::PipelineStageFlagBits2::eFragmentShadingRateAttachmentKHR;
    barrier.dstAccessMask = vk::AccessFlagBits2::eFragmentShadingRateAttachmentReadKHR;
    barrier.oldLayout = vk::ImageLayout::eTransferDstOptimal;
    barrier.newLayout = vk::ImageLayout::eFragmentShadingRateAttachmentOptimalKHR;
    cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                            .pImageMemoryBarriers = &barrier});
    LOG_INFO(Render_Vulkan,
             "VR foveation: rate map {}x{} ({} layer(s)) for {}x{} {}passes, inner {:.2f}, outer "
             "{:.2f}, axes L ({:.3f}, {:.3f}) R ({:.3f}, {:.3f}), view at {}%{}",
             nx, ny, layers, width, height, mono ? "DIBR (mono) " : "", zones.inner, zones.outer,
             cx[0], cy[0], cx[1], cy[1], 100 - drop,
             fov ? "" : " (image centre until the headset's angles are known)");
    if (!existing && maps.size() == MaxFoveationMaps) {
        existing = &maps[cache.next_victim];
        cache.next_victim = (cache.next_victim + 1) % MaxFoveationMaps;
    }
    FoveationMap entry{width, height, mono, zones, drop, fov, std::move(resources)};
    if (existing) {
        scheduler.DeferOperation([old = std::move(existing->resources)] {});
        *existing = std::move(entry);
    } else {
        maps.push_back(std::move(entry));
    }
    return view;
}

// VR_FOVEATED_DIBR ('0' off, on by default; read once): foveation for DIBR's mono scene passes
// too (SetMonoFoveation), with the same card checks and per-pipeline gating as native stereo.
const bool g_foveated_dibr = [] {
    const char* value = std::getenv("VR_FOVEATED_DIBR");
    return !(value && value[0] == '0');
}();
// VR_FOVEATION_DRS ('0' off, on by default; read once): the zones follow dynamic resolution's
// narrower view (SetFoveationDrop); off, every pass takes the full-scale map (the A/B).
const bool g_foveation_follows_drs = [] {
    const char* value = std::getenv("VR_FOVEATION_DRS");
    return !(value && value[0] == '0');
}();

// Dynamic resolution's steps (core/vr/s1_dynamic_resolution.h; 2026-11-24): their maps made ahead,
// at most one a frame, so the first frame of a step - one already late, the reason for the step -
// makes none, and no frame makes a burst of them. Only while the governor can step (timing_wanted:
// the last frame could be scaled) and only the steps the floor in force allows (none at 100%: no
// map for a player whose resolution never drops), each extent and set of zones once a floor. Never
// into the cap's last PrebuildRoom maps, which stay for the passes' own (a pass without its map
// shades at full rate). A map that cannot be made is not tried again here (the step's own pass
// still asks, as before).
void PrebuildFoveationSteps(FoveationCache& cache, Scheduler& scheduler, const Instance& instance,
                            CommandRecorder& recorder, u32 width,
                            u32 height, const Foveation::Settings& zones) {
    constexpr size_t PrebuildRoom = 16;
    const u32 floor = S1Drs::floor_percent.load(std::memory_order_relaxed);
    if (floor >= 100 || !S1Drs::timing_wanted.load(std::memory_order_relaxed)) {
        return;
    }
    auto& done = cache.prebuilt;
    for (const auto& d : done) {
        if (d.width == width && d.height == height && d.inner == zones.inner &&
            d.outer == zones.outer && d.ring == zones.ring && d.floor == floor) {
            return;
        }
    }
    u32 missing = 0; // the first step without its map
    for (u32 level = 1; level <= 3 && missing == 0; ++level) {
        const u32 drop = 100 - std::max(100 - 10 * level, floor);
        if (drop != 0 && !FindFoveationMap(cache, width, height, false, zones, drop)) {
            missing = drop;
        }
    }
    if (missing != 0 && cache.maps.size() + PrebuildRoom < MaxFoveationMaps) {
        // (A frame's passes begin within a few milliseconds of each other.)
        auto& last_build = cache.last_prebuild;
        const auto now = std::chrono::steady_clock::now();
        if (now - last_build < std::chrono::milliseconds(6)) {
            return;
        }
        last_build = now;
        if (FoveationView(cache, scheduler, instance, recorder, width, height, false, zones, missing)) {
            return; // (the next step's at a later frame)
        }
        // (Out of memory: no retries from here.)
    }
    if (done.size() < 32) {
        done.push_back({width, height, zones.inner, zones.outer, zones.ring, floor});
    }
}
} // namespace

u32 Scheduler::FoveationDrop() const {
    return g_foveation_follows_drs ? foveation_drop : 0u;
}

void Scheduler::SetMonoFoveation(vk::Extent2D extent) {
    if (g_foveated_dibr) {
        mono_foveation = extent;
    }
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

    // Layer draws (NativeStereo::LayerDraws): a both-eye pass renders the two layers of its
    // attachments' array views without multiview; each draw is recorded once per eye.
    const bool layer_draws = render_state.probe_view_mask == 3 && NativeStereo::LayerDraws();
    vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .offset = {0, 0},
                .extent = {render_state.width, render_state.height},
            },
        .layerCount = layer_draws ? 2u : render_state.num_layers,
        .viewMask = layer_draws ? 0u : render_state.probe_view_mask,
        .colorAttachmentCount = render_state.num_color_attachments,
        .pColorAttachments = color_attachments.data(),
        .pDepthAttachment = db.has_depth ? &depth_attachment : nullptr,
        .pStencilAttachment = db.has_stencil ? &stencil_attachment : nullptr,
    };

    // Fixed foveated rendering (vr_foveation.h): the game's eye-sized passes that draw both eyes,
    // or under DIBR the single-view passes of its scene's size (SetMonoFoveation). VR host draws
    // record their own passes and never get here with both views. Only while the world is around
    // the player: the title, menus and cinematics on the virtual screen are seen flat, so the left
    // eye's coarse outer corner showed as their bottom-left corner (2026-10-23).
    vk::RenderingFragmentShadingRateAttachmentInfoKHR foveation{};
    pass_foveated = false;
    const bool mono_pass = render_state.probe_view_mask == 0 && render_state.num_layers <= 1 &&
                           mono_foveation.width != 0 &&
                           render_state.width == mono_foveation.width &&
                           render_state.height == mono_foveation.height;
    // SetFoveationEye: once the eye's size is known, a both-eye pass of any other size (a shadow
    // atlas) is drawn at full rate.
    const auto eye_sized = [&](u32 w, u32 h) {
        const auto& e = foveation_eye;
        return e.width == 0 || (w == e.width && h == e.height) ||
               (w == (e.width + 1) / 2 && h == (e.height + 1) / 2);
    };
    const bool stereo_pass = render_state.probe_view_mask == 3 &&
                             eye_sized(render_state.width, render_state.height);
    int applied_mode = VR::Graphics::FfrPending;
    if (instance.IsFoveationEnabled() && !foveation_blocked && (stereo_pass || mono_pass) &&
        render_state.width >= 256 && render_state.height >= 256 && VR::IsImmersive()) {
        const bool fixed = instance.IsFoveationFixed();
        const int mode = fixed ? VR::Graphics::FfrOverride : FoveationModeNow();
        auto zones = fixed ? instance.GetFoveationSettings() : Foveation::Choose(nullptr, mode);
        if (zones.on && !foveation_cache)
            foveation_cache = std::make_unique<FoveationCache>();
        // (The pass about to begin asked for a wider full-rate zone: the lamp shadows' mask,
        // light_shadow.h MaskFoveaInner. Its own map, kept like the others.)
        if (zones.on && foveation_inner_floor > zones.inner) {
            zones.inner = foveation_inner_floor;
            zones.outer = std::max(zones.outer, zones.inner);
        }
        if (const vk::ImageView view =
                zones.on ? FoveationView(*foveation_cache, *this, instance, blob_open ? *blob : Main(), render_state.width,
                                         render_state.height, render_state.probe_view_mask != 3,
                                         zones,
                                         // (DIBR never scales: its mono view is the whole image)
                                         stereo_pass ? FoveationDrop() : 0u)
                         : vk::ImageView{}) {
            foveation = vk::RenderingFragmentShadingRateAttachmentInfoKHR{
                .imageView = view,
                .imageLayout = vk::ImageLayout::eFragmentShadingRateAttachmentOptimalKHR,
                .shadingRateAttachmentTexelSize = instance.GetFoveationTexel(),
            };
            rendering_info.pNext = &foveation;
            pass_foveated = true;
        }
        if (!fixed)
            applied_mode = zones.on && !pass_foveated ? VR::Graphics::FfrNoMap : mode;
        if (zones.on && stereo_pass && g_foveation_follows_drs && S1Drs::enabled) {
            PrebuildFoveationSteps(*foveation_cache, *this, instance, blob_open ? *blob : Main(), render_state.width,
                                   render_state.height, zones);
        }
    }
    if (pass_target.load(std::memory_order_acquire) == this) {
        Main().Raw("gpu_pass_profile", std::source_location::current());
        GpuPassBegin(this, current_cmdbuf, render_state);
    }
    Commands().beginRendering(rendering_info);
    if (applied_mode != VR::Graphics::FfrPending &&
        VR::Graphics::foveation_applied.load(std::memory_order_relaxed) != applied_mode) {
        VR::Graphics::foveation_applied.store(applied_mode, std::memory_order_relaxed);
    }
    if (GpuCrash::CheckpointsOn()) [[unlikely]] {
        GpuCheckpoint(static_cast<u8>(GpuCrash::Kind::RenderPass),
                      (render_state.probe_view_mask & 0xff) |
                          (render_state.depth_stencil_attachment.image_view ? 0x100u : 0u),
                      render_state.width, render_state.height,
                      render_state.num_layers | (u32{render_state.num_color_attachments} << 16));
    }
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

void Scheduler::BlockFoveation(bool blocked) {
    foveation_blocked = blocked;
    if (blocked && is_rendering && pass_foveated) {
        EndRendering();
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
        Main().Raw("gpu_pass_profile", std::source_location::current());
        GpuPassEnd(this, current_cmdbuf);
    } // VR_GPU_PASSES only
    if (pass_end_hook) [[unlikely]] {
        pass_end_hook(*this); // the copy sink's waiting copies
    }
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
    const_cast<Scheduler*>(this)->SplitSync("empty_check"); // B2's packets counted
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
    Main().Drain("finish");
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
    Main().Drain("tick_wait");
    master_semaphore.Wait(tick);
}

// FAST_PENDING_OPS_BUDGET_US (default 1000, 0 = no limit; read once at start-up): how long one
// call runs the finished deferred operations before leaving the rest to the next call (the next
// draw, dispatch or submission, microseconds later). A texture GC that evicted 237 images had
// them all freed in one call, each a dedicated allocation's vkFreeMemory of ~1 ms, inside a
// submission: the recorder stopped for 238 ms with stage B and the GPU thread waiting behind it
// (2026-10-24). The operations only free what the GPU has finished with, so a later turn costs
// nothing but memory held a frame longer.
static const s64 g_pending_ops_budget_ns = [] {
    const char* value = std::getenv("FAST_PENDING_OPS_BUDGET_US");
    return (value ? std::strtoll(value, nullptr, 10) : 1000LL) * 1000LL;
}();

void Scheduler::PopPendingOperations(bool force_poll, bool all) {
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
    const bool budgeted = !all && g_pending_ops_budget_ns > 0;
    const auto start = budgeted ? std::chrono::steady_clock::now()
                                : std::chrono::steady_clock::time_point{};
    while (!pending_ops.empty() && master_semaphore.IsFree(pending_ops.front().gpu_tick)) {
        pending_ops.front().callback();
        pending_ops.pop();
        phase.Count(1);
        if (budgeted && std::chrono::steady_clock::now() - start >=
                            std::chrono::nanoseconds{g_pending_ops_budget_ns}) {
            break;
        }
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
    if (GpuCrash::CheckpointsOn()) [[unlikely]] {
        if (const u64 id = GpuCrash::Mark(GpuCrash::Kind::CommandsBegin, tick, tick, 0, 0, 0,
                                          static_cast<GpuCrash::Source>(crash_source))) {
            current_cmdbuf.setCheckpointNV(reinterpret_cast<const void*>(static_cast<uintptr_t>(id)));
        }
    }

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

// VR_SUBMIT_CENSUS=1 (diagnostics, logged builds): where the submissions come from. Each one's
// return addresses above SubmitExecution, as offsets into the executable, are counted per chain;
// every 4000 submissions the busiest chains are logged ("VR submit census:"), named afterwards
// with the build's .map. A GPU-bound frame pays 20-50 us of GPU idle per queue boundary.
#if defined(_WIN32) && !defined(BBVR_NO_LOGS)
extern "C" char __ImageBase;
extern "C" __declspec(dllimport) unsigned short __stdcall RtlCaptureStackBackTrace(
    unsigned long frames_to_skip, unsigned long frames_to_capture, void** back_trace,
    unsigned long* back_trace_hash);
namespace {
bool SubmitCensusOn() {
    static const bool on = [] {
        const char* value = std::getenv("VR_SUBMIT_CENSUS");
        return value && *value == '1';
    }();
    return on;
}
[[gnu::noinline]] void CountSubmission(const void* scheduler) {
    constexpr u32 Depth = 10;
    struct Chain {
        u64 count{};
        const void* scheduler{};
        std::array<u32, Depth> offsets{};
    };
    static std::mutex mutex;
    static std::unordered_map<u64, Chain> chains;
    static u64 total{};
    void* frames[Depth]{};
    const u32 n = RtlCaptureStackBackTrace(1, Depth, frames, nullptr); // from SubmitExecution up
    const auto base = reinterpret_cast<uintptr_t>(&__ImageBase);
    std::array<u32, Depth> offsets{};
    u64 hash = reinterpret_cast<uintptr_t>(scheduler);
    for (u32 i = 0; i < n; ++i) {
        const auto address = reinterpret_cast<uintptr_t>(frames[i]);
        offsets[i] = address >= base && address - base < 0xffffffffu ? u32(address - base) : 0;
        hash = (hash ^ offsets[i]) * 0x100000001b3ull;
    }
    std::scoped_lock lock{mutex};
    auto& chain = chains[hash];
    ++chain.count;
    chain.scheduler = scheduler;
    chain.offsets = offsets;
    if (++total % 4000 != 0)
        return;
    std::vector<const Chain*> top;
    for (const auto& [key, c] : chains)
        top.push_back(&c);
    std::sort(top.begin(), top.end(), [](auto* a, auto* b) { return a->count > b->count; });
    top.resize(std::min<size_t>(top.size(), 16));
    for (const Chain* c : top) {
        std::string text;
        for (const u32 offset : c->offsets)
            text += fmt::format(" {:x}", offset);
        LOG_INFO(Render_Vulkan, "VR submit census: {} of {} sched={} chain{}", c->count, total,
                 fmt::ptr(c->scheduler), text);
    }
}
} // namespace
#endif // _WIN32 && !BBVR_NO_LOGS

void Scheduler::SubmitExecution(SubmitInfo& info) {
    ScratchProfile::Cpu scratch_cpu(14, "Scheduler::SubmitExecution");
#if defined(_WIN32) && !defined(BBVR_NO_LOGS)
    if (SubmitCensusOn()) [[unlikely]] {
        CountSubmission(this);
    }
#endif
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
        expect_commands = Main().CommandPackets() - buffer_commands;
    }
    // Allocate the logical tick on the producer; resource retirement and stream watches
    // must refer to encoded work even while Vulkan recording is behind it.
    const u64 tick = master_semaphore.NextTick();
    // FAST bit 62: the guest signals encoded so far ride on this submission.
    signals_at_submit = signals_encoded;
    ++submit_count;
    if (recorder.Threaded()) {
        const RecorderSubmit submit{info, tick, FastRender::FlushHostCopies(), expect_commands};
        Main().QueueControl(SubmitCommands, &submit, sizeof(submit));
    } else {
        Main().Drain("submit");
        FastRender::WaitHostCopies(FastRender::CopyWait::Submit);
        CheckEmptySubmit(expect_commands);
        SubmitLocked(info, tick);
    }
    ResetProducerState();
    Main().Configure();
    buffer_commands = Main().CommandPackets(); // FAST bit 56: the new command buffer
    if (pass_profiler != nullptr) [[unlikely]] {
        pass_profiler->Resume(pass_resume);
    }
    // FAST bit 56: the profiler's resume stamps do not make the command buffer non-empty.
    empty_mark = {Main().CommandPackets(), recorder.RawBorrows()};

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
        if (GpuCrash::CheckpointsOn()) [[unlikely]] {
            if (const u64 id = GpuCrash::Mark(GpuCrash::Kind::CommandsEnd, signal_value,
                                              signal_value, 0, 0, 0,
                                              static_cast<GpuCrash::Source>(crash_source))) {
                current_cmdbuf.setCheckpointNV(
                    reinterpret_cast<const void*>(static_cast<uintptr_t>(id)));
            }
        }
        Check(current_cmdbuf.end());
        recorder.ResetReplayedCommands(); // FAST bit 56 verify: the census is per command buffer
    }

    const vk::Semaphore timeline = master_semaphore.Handle();
    info.AddSignal(timeline, signal_value);

    std::array<vk::PipelineStageFlags, 4> wait_stage_masks = {
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eColorAttachmentOutput,
        vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlagBits::eAllCommands,
    };
    // Sparse buffers: the binds queued so far go out first, and this submit waits for the newest
    // (one already signalled costs nothing), at every stage whichever slot it takes.
    u64 sparse_wait = 0; // GpuCrash::NoteSubmit
    if (sparse_binds) {
        LockTrace::Phase phase{"sparse binds"};
        const u64 wait = sparse_binds(instance.GetGraphicsQueue());
        sparse_wait = wait;
        if (wait != 0) {
            ASSERT(info.num_wait_semas < info.wait_semas.size());
            wait_stage_masks[info.num_wait_semas] = vk::PipelineStageFlagBits::eAllCommands;
            info.AddWait(sparse_timeline, wait);
        }
    }

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
    GpuCrash::NoteSubmit(signal_value, info.num_wait_semas, sparse_wait,
                         static_cast<GpuCrash::Source>(crash_source));
    // A lost device: gpu-crash.txt says what the GPU was doing (written before the check ends
    // the process).
    if (submit_result == vk::Result::eErrorDeviceLost) [[unlikely]] {
        GpuCrash::Report(submit_result, fmt::format("the game's submit of tick {}", signal_value));
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

void Scheduler::DrainDeferredOperations() {
    // Rounds until both queues are empty and every priority operation queued has finished. A
    // cleanup may queue more: a normal one a priority one with the tick current then (DeleteBuffer
    // under BackgroundFree). So each round makes every tick recorded so far complete (Finish),
    // runs every normal operation (their ticks complete now), makes the ticks those queued work
    // with complete, then waits for the priority operations counted so far.
    const bool priority_thread = priority_pending_ops_thread.joinable();
    for (u64 round = 1;; ++round) {
        Finish();
        PopPendingOperations(true, true);
        Finish();
        const u64 queued = priority_ops_queued.load(std::memory_order_acquire);
        auto waited_since = std::chrono::steady_clock::now();
        while (priority_thread && priority_ops_done.load(std::memory_order_acquire) < queued) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (std::chrono::steady_clock::now() - waited_since >= std::chrono::seconds(1)) {
                // Queued since by another thread with a tick not submitted: submit it.
                LOG_WARNING(Render_Vulkan, "Teardown: {} priority operations still running",
                            queued - priority_ops_done.load(std::memory_order_acquire));
                Flush();
                waited_since = std::chrono::steady_clock::now();
            }
        }
        bool settled;
        {
            std::scoped_lock lk{pending_ops_mutex, priority_pending_ops_mutex};
            settled = pending_ops.empty() &&
                      (!priority_thread || priority_ops_done.load(std::memory_order_acquire) ==
                                               priority_ops_queued.load(std::memory_order_acquire));
        }
        if (settled) {
            return;
        }
        if (round % 16 == 0) {
            LOG_WARNING(Render_Vulkan, "Teardown: deferred operations still queued after {} rounds",
                        round);
        }
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
        priority_ops_done.fetch_add(1, std::memory_order_release); // DrainDeferredOperations
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
