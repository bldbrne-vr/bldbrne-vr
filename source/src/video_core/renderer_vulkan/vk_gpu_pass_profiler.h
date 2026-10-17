// SPDX-License-Identifier: GPL-2.0-or-later
//
// FAST_GPU_PASS_PROFILE=1 with fast bit 38 (GpuPassProfile): the GPU time of each pass of the draw
// stream, ranked every 2 s in the `FAST gpu passes:` line (ms per game frame and calls per frame
// per label, and per group: scene passes, dispatches, the HLE copy shader, transfers, native
// stereo copies, temporal/DLSS/FSR, SMAA/TAA, VR host passes, gaps, the late warp).
//
// Port of bbport's BB_GPU_PROFILE (gpu/shadps4/video_core/renderer_vulkan/vk_gpu_profiler.h,
// docs/parallel_gpu.md:426-454): a timestamp is written where each render pass, dispatch, copy
// category and host pass starts, and the time to the next timestamp is charged to its label
// (barriers and copies recorded in between included, as in bbport). Scopes (uploads, buffer
// copies, native stereo copies, scene proxy fills) resume the label they interrupted
// (bbport Runtime::TransferMark, vk_runtime.cpp:248-270).
//
// Differences from bbport:
//  - The timestamps go through Scheduler::Commands(): with the recorder thread they are packets,
//    replayed in order into the command buffer of the work they time (bbport recorded them with
//    Scheduler::Record).
//  - Never inside a render pass and never ends one (bbport called EndRendering,
//    vk_gpu_profiler.cpp:39-41): a mark while a pass is open is skipped (counted `in_pass`), so
//    the command stream keeps exactly its passes. Passes get their timestamp in
//    Scheduler::BeginRendering after the previous pass ended; inside a multiview pass a
//    timestamp would also take one query per view.
//  - A frame's slice is reset in the stream at its first timestamp and read without waiting,
//    once the scheduler's timeline passed the slice's last submission (bbport: host reset, read
//    with eWait four frames later). A slice still in flight after NumSlices frames skips the
//    frame instead of waiting (`skipped`).
//  - A label repeated with no other mark between continues its segment (one timestamp; the
//    calls still count).
//  - Submissions: the gap between two command buffers is its own label (bbport marked the
//    submission, vk_scheduler.cpp:932), and the label open before the submission resumes in the
//    next command buffer.
//  - The late warp's own timestamps (OpenXR thread, own submissions) are added as `late_warp`.
//
// Off: nothing is recorded with the environment variable unset (no profiler exists) or with bit
// 38 clear (latched per frame in EndFrame, so the bit may change at any time). Thread safety:
// everything but NoteExternal is used by the thread recording into the draw scheduler (Stage B or
// the GPU command thread; the draw pipe's drains order the hand-over), like the scheduler's own
// producer state.
//
// FAST_GPU_PASS_PROFILE=2: coarse, without the transfer/native-copy scopes (fewer timestamps).
// FAST_GPU_PASS_PROFILE_VERIFY=1: a `FAST gpu passes verify:` line with the self-checks
// (timestamp order, slices whose results were missing, marks skipped inside passes, frame span
// percentiles to compare with VR_GPU_TIMING's `VR gpu frame:` span).

#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/types.h"
#include "video_core/fast_render.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

class Instance;
class Scheduler;
struct RenderState;

namespace GpuPassDetail {
struct Slice {
    std::vector<u64> keys;  // the label of each timestamp but the closing one
    std::vector<u32> calls; // marks merged into each segment (0: a resumed label)
    u64 tick = 0;           // the submission that holds the closing timestamp
    u64 frame = 0;          // EndFrame count when the slice was opened
    u32 used = 0;           // timestamps written, the closing one included once closed
    u32 not_ready = 0;      // reads that found results missing after the tick was done
    bool pending = false;   // closed, its results not read yet
};
struct Label {
    std::string name;
    u8 group = 0;
};
struct Total {
    u64 ticks = 0;
    u64 calls = 0;
};
struct ExternalSum {
    std::atomic<u64> ns{0};
    std::atomic<u64> count{0};
};
} // namespace GpuPassDetail

class GpuPassProfiler {
public:
    /// Fast bit 38: timestamps are written while it is set (FAST_GPU_PASS_PROFILE=1 creates the
    /// profiler). Not in kDefaultMask.
    static constexpr auto GpuPassProfile = static_cast<FastRender::Bit>(38);

    enum class Group : u8 {
        Scene,        // the game's render passes (one label per pass shape and shaders)
        HostPass,     // other render passes begun through the scheduler (unlabelled)
        Dispatch,     // the game's compute dispatches
        HleCopy,      // the HLE'd copy shader (vk_shader_hle.cpp)
        Transfer,     // buffer/image uploads, downloads, copies, clears, detiling
        NativeStereo, // right-eye buffer copies, mirrors, the eye crops
        Temporal,     // DLSS / FSR 4 resolve and merge, scene proxy fills
        Aa,           // SMAA, render TAA
        VrHost,       // presenter and VR host passes in the draw stream (UI, eye copies, FSR 1)
        Gap,          // between submissions (GPU idle or other queue work)
        LateWarp,     // the late warp's submissions (OpenXR thread)
        Count,
    };

    /// The late warp's spans (VrLateWarp::ReadTimestamps).
    enum class External : u32 { Classify, Reproject, Warp, EyeTaa, Upscale, Copy, Count };

    /// The profiler when it records into `scheduler` this frame, else null.
    [[nodiscard]] static GpuPassProfiler* For(const Scheduler& scheduler) noexcept {
        GpuPassProfiler* const p = instance_ptr.load(std::memory_order_acquire);
        return p != nullptr && p->target == &scheduler && p->active.load(std::memory_order_relaxed)
                   ? p
                   : nullptr;
    }

    /// Rasterizer construction: creates the profiler for the draw scheduler when
    /// FAST_GPU_PASS_PROFILE is 1 or 2.
    static void Init(const Instance& instance, Scheduler& scheduler);
    /// Rasterizer destruction: stops it, waits for the scheduler's work, frees the query pool.
    static void Shutdown(Scheduler& scheduler);

    [[nodiscard]] static constexpr u64 Mix(u64 h, u64 v) noexcept {
        h ^= v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2);
        return h * 0xC2B2AE3D27D4EB4Full;
    }

    /// Starts a segment labelled `key`; `describe` names it the first time the key is seen.
    template <typename Describe>
    void Mark(u64 key, Group group, Describe&& describe) {
        Name(key, group, describe);
        Stamp(key, true);
    }
    /// Mark on `scheduler` when it is profiled this frame.
    static void MarkAt(Scheduler& scheduler, u64 key, Group group, const char* name) {
        if (GpuPassProfiler* const p = For(scheduler)) [[unlikely]] {
            p->Mark(key, group, [name] { return std::string{name}; });
        }
    }

    /// Labels the render pass the next Scheduler::BeginRendering begins (call it only right
    /// before a BeginRendering that begins one: Scheduler::WillBeginRendering).
    template <typename Describe>
    void LabelNextPass(u64 key, Group group, Describe&& describe) {
        Name(key, group, describe);
        next_pass = key;
    }
    /// LabelNextPass on `scheduler`, when it is profiled and `state` begins a pass.
    static void LabelPassAt(Scheduler& scheduler, const RenderState& state, u64 key, Group group,
                            const char* name) {
        if (GpuPassProfiler* const p = For(scheduler)) [[unlikely]] {
            p->LabelPassIfNew(state, key, group, name);
        }
    }

    /// Scheduler::BeginRendering, a pass about to begin (the previous one ended): its timestamp,
    /// labelled by LabelNextPass or by its shape.
    void BeginPass(const RenderState& state);

    /// Scheduler::SubmitExecution, before the submission: the gap label; returns the label to
    /// resume in the next command buffer.
    [[nodiscard]] u64 SubmitBegin();
    /// Continues `key`'s segment (a label seen before) after a nested one or a submission.
    void Resume(u64 key);

    /// The flip (Presenter::PrepareFrame / PrepareBlankFrame, before the flush): closes the
    /// frame's slice, reads finished ones, latches bit 38 for the next frame, reports every 2 s.
    static void EndFrameAt(Scheduler& scheduler) {
        GpuPassProfiler* const p = instance_ptr.load(std::memory_order_acquire);
        if (p != nullptr && p->target == &scheduler) [[unlikely]] {
            p->EndFrame();
        }
    }

    /// Any thread: GPU time of work submitted outside the draw stream (the late warp).
    static void NoteExternal(External which, double ms) noexcept {
        GpuPassProfiler* const p = instance_ptr.load(std::memory_order_acquire);
        if (p == nullptr || !p->active.load(std::memory_order_relaxed) || !(ms > 0.0)) {
            return;
        }
        auto& sum = p->external[static_cast<u32>(which)];
        sum.ns.fetch_add(static_cast<u64>(ms * 1e6), std::memory_order_relaxed);
        sum.count.fetch_add(1, std::memory_order_relaxed);
    }

    /// A segment for nested work (a transfer category) while it lives; the label it interrupted
    /// continues afterwards. `name` null: no segment.
    class Scope {
    public:
        Scope(Scheduler& scheduler, u64 key, Group group, const char* name) {
            if (GpuPassProfiler* const p = For(scheduler)) [[unlikely]] {
                Open(p, key, group, name);
            }
        }
        ~Scope() {
            if (profiler != nullptr) [[unlikely]] {
                profiler->Resume(resume);
            }
        }
        Scope(const Scope&) = delete;
        Scope& operator=(const Scope&) = delete;

    private:
        void Open(GpuPassProfiler* p, u64 key, Group group, const char* name);
        GpuPassProfiler* profiler = nullptr;
        u64 resume = 0;
    };

private:
    static constexpr u32 NumSlices = 8;
    static constexpr u32 SliceQueries = 8192;
    static constexpr u32 NumExternal = static_cast<u32>(External::Count);

    GpuPassProfiler(const Instance& instance, Scheduler& scheduler, vk::QueryPool pool,
                    double tick_ns, u32 valid_bits, bool coarse, bool verify);

    template <typename Describe>
    void Name(u64 key, Group group, Describe& describe) {
        if (labels.find(key) == labels.end()) {
            labels.emplace(key, GpuPassDetail::Label{std::string(describe()),
                                                     static_cast<u8>(group)});
        }
    }
    void LabelPassIfNew(const RenderState& state, u64 key, Group group, const char* name);
    /// Writes a timestamp for `key` (or continues its open segment); false when it could not.
    bool Stamp(u64 key, bool call);
    void EndFrame();
    void Collect();
    void Accumulate(GpuPassDetail::Slice& slice);
    void Report();

    static inline std::atomic<GpuPassProfiler*> instance_ptr{nullptr};

    // Fixed after construction.
    Scheduler& scheduler;
    const Scheduler* target;
    vk::Device device;
    vk::QueryPool pool;
    double tick_ms = 1e-6;
    u64 tick_mask = ~u64{0};
    bool coarse = false;
    bool verify = false;
    u64 max_ticks = 0; // longer segments: out of order

    // The recording thread of `scheduler`.
    bool frame_on = false;
    u64 current = 0;   // the label of the open segment
    u64 next_pass = 0; // LabelNextPass, consumed by BeginPass
    u32 current_slice = 0;
    u32 oldest = 0;
    u32 pending_count = 0;
    u64 frame_no = 0;
    std::array<GpuPassDetail::Slice, NumSlices> slices{};
    std::unordered_map<u64, GpuPassDetail::Label> labels;
    std::unordered_map<u64, GpuPassDetail::Total> totals; // the report window
    std::vector<u64> results;
    std::vector<float> frame_spans; // VERIFY: the window's frame spans (ms)
    u64 window_frames = 0;
    u64 window_span_ticks = 0;
    u64 window_lost_ticks = 0;
    u64 window_stamps = 0;
    u64 window_skipped = 0;
    u64 window_flips = 0;
    u64 last_read_frame = ~u64{0};
    u64 last_close_stamp = 0;
    u64 in_pass = 0, overflow = 0, close_in_pass = 0; // the report window
    u64 nonmonotonic = 0, unavailable = 0, read_errors = 0, slices_read = 0; // the session
    bool was_wanted = false; // bit 38 at the last flip (its changes are logged)
    std::chrono::steady_clock::time_point window_start = std::chrono::steady_clock::now();

    // Any thread: frame_on as of the last flip (For, NoteExternal).
    std::atomic<bool> active{false};
    std::array<GpuPassDetail::ExternalSum, NumExternal> external{};
};

} // namespace Vulkan
