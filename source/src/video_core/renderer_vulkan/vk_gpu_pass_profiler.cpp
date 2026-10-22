// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

// FAST_GPU_PASS_PROFILE (vk_gpu_pass_profiler.h). Port of bbport
// gpu/shadps4/video_core/renderer_vulkan/vk_gpu_profiler.cpp.

#include <algorithm>
#include <cstdlib>
#include <utility>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_gpu_pass_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {

constexpr std::array<const char*, static_cast<size_t>(GpuPassProfiler::Group::Count)> GroupNames{
    "scene",         "host_pass", "dispatch", "hle_copy", "transfer", "native_stereo",
    "temporal",      "aa",        "vr_host",  "gap",      "late_warp_xr",
};

constexpr std::array<const char*, static_cast<size_t>(GpuPassProfiler::External::Count)>
    ExternalNames{
        "XR late warp: classify",     "XR late warp: reproject",
        "XR late warp: warp",         "XR late warp: eye TAA",
        "XR late warp: upscale/sharpen", "XR late warp: copy to the swapchain",
    };

// bbport's submission key (vk_scheduler.cpp:932, 0x5B317), salted like every key here.
constexpr u64 GapKey = 0x5B317'0000'0001ull;
constexpr u64 FrameStartKey = 0xF5A3'0000'0001ull;
constexpr u64 ExternalKey = 0xE7E2'0000'0000ull;

constexpr u32 MaxRanked = 40;

} // Anonymous namespace

void GpuPassProfiler::Init(const Instance& instance, Scheduler& scheduler) {
    const char* env = std::getenv("FAST_GPU_PASS_PROFILE");
    if (env == nullptr || (env[0] != '1' && env[0] != '2') || env[1] != '\0' ||
        instance_ptr.load(std::memory_order_acquire) != nullptr) {
        return;
    }
    const vk::PhysicalDevice physical = instance.GetPhysicalDevice();
    const auto families = physical.getQueueFamilyProperties();
    const u32 family = instance.GetGraphicsQueueFamilyIndex();
    const u32 valid_bits = family < families.size() ? families[family].timestampValidBits : 0;
    const float period = physical.getProperties().limits.timestampPeriod;
    if (valid_bits == 0 || !(period > 0.0f)) {
        LOG_WARNING(Render_Vulkan, "FAST gpu passes: off (the graphics queue has no timestamps)");
        return;
    }
    const vk::Device device = instance.GetDevice();
    auto [result, pool] = device.createQueryPool({
        .queryType = vk::QueryType::eTimestamp,
        .queryCount = NumSlices * SliceQueries,
    });
    if (result != vk::Result::eSuccess) {
        LOG_WARNING(Render_Vulkan, "FAST gpu passes: off (no timestamp query pool: {})",
                    vk::to_string(result));
        return;
    }
    const char* verify_env = std::getenv("FAST_GPU_PASS_PROFILE_VERIFY");
    const bool verify = verify_env != nullptr && verify_env[0] == '1';
    const bool coarse = env[0] == '2';
    // Never deleted: the OpenXR thread may still be in NoteExternal at shutdown.
    auto* profiler =
        new GpuPassProfiler(instance, scheduler, pool, period, valid_bits, coarse, verify);
    instance_ptr.store(profiler, std::memory_order_release);
    LOG_INFO(Render_Vulkan,
             "FAST gpu passes: ready ({} slices of {} timestamps, {:.3f} ns per tick, {} valid "
             "bits, {}{}); timestamps while fast bit 38 is set (now {})",
             NumSlices, SliceQueries, period, valid_bits, coarse ? "coarse" : "full",
             verify ? ", verify" : "",
             FastRender::On(GpuPassProfile) ? "set" : "clear: add 0x4000000000 to FAST_TOGGLES");
}

void GpuPassProfiler::Shutdown(Scheduler& scheduler) {
    GpuPassProfiler* const p = instance_ptr.load(std::memory_order_acquire);
    if (p == nullptr || p->target != &scheduler) {
        return;
    }
    p->active.store(false, std::memory_order_relaxed);
    instance_ptr.store(nullptr, std::memory_order_release);
    // Every command buffer that writes into the pool submitted and finished before it goes.
    scheduler.Finish();
    p->device.destroyQueryPool(p->pool);
    p->pool = vk::QueryPool{};
}

GpuPassProfiler::GpuPassProfiler(const Instance& instance, Scheduler& scheduler_,
                                 vk::QueryPool pool_, double tick_ns, u32 valid_bits,
                                 bool coarse_, bool verify_)
    : scheduler{scheduler_}, target{&scheduler_}, device{instance.GetDevice()}, pool{pool_},
      tick_ms{tick_ns * 1e-6}, tick_mask{valid_bits >= 64 ? ~u64{0}
                                                          : (u64{1} << valid_bits) - 1},
      coarse{coarse_}, verify{verify_} {
    // A segment longer than a second is a timestamp out of order (or a wrapped counter).
    max_ticks = static_cast<u64>(1000.0 / tick_ms);
    for (auto& slice : slices) {
        slice.keys.reserve(SliceQueries);
        slice.calls.reserve(SliceQueries);
    }
    results.resize(SliceQueries);
    labels.reserve(2048);
    for (u32 i = 0; i < NumExternal; ++i) {
        labels.emplace(ExternalKey + i,
                       GpuPassDetail::Label{ExternalNames[i], static_cast<u8>(Group::LateWarp)});
    }
    labels.emplace(GapKey,
                   GpuPassDetail::Label{"(between submissions: GPU idle or other queue work)",
                                        static_cast<u8>(Group::Gap)});
    labels.emplace(FrameStartKey, GpuPassDetail::Label{"(frame start, until the first mark)",
                                                       static_cast<u8>(Group::HostPass)});
    was_wanted = FastRender::On(GpuPassProfile);
}

bool GpuPassProfiler::Stamp(u64 key, bool call) {
    next_pass = 0;
    if (scheduler.IsRendering()) {
        // Never inside a pass, and never ends one: the stream keeps its passes (in a multiview
        // pass a timestamp would also take one query per view).
        ++in_pass;
        return false;
    }
    auto& slice = slices[current_slice];
    if (slice.used != 0 && slice.keys.back() == key) {
        slice.calls.back() += call ? 1u : 0u; // the open segment continues
        current = key;
        return true;
    }
    if (slice.used + 2 > SliceQueries) {
        ++overflow; // full: the rest of the frame goes to the open label (one stays for the close)
        return false;
    }
    const u32 base = current_slice * SliceQueries;
    // In the recorded stream: replayed in order with the work it times, recorder thread or not.
    const auto cmdbuf = scheduler.Commands();
    if (slice.used == 0) {
        cmdbuf.resetQueryPool(pool, base, SliceQueries);
    }
    cmdbuf.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool, base + slice.used);
    ++slice.used;
    slice.keys.push_back(key);
    slice.calls.push_back(call ? 1u : 0u);
    current = key;
    return true;
}

void GpuPassProfiler::BeginPass(const RenderState& state) {
    u64 key = std::exchange(next_pass, 0);
    if (key == 0) {
        const auto& ds = state.depth_stencil_attachment;
        const bool depth = ds.has_depth || ds.has_stencil;
        key = Mix(Mix(0x9A55'0000'0001ull, u64{state.width} | u64{state.height} << 16 |
                                               u64{state.num_color_attachments} << 32 |
                                               u64{state.num_layers} << 40),
                  u64{state.probe_view_mask} << 1 | (depth ? 1u : 0u));
        const auto describe = [&] {
            return fmt::format("host pass {}x{} c{}{}{}{}", state.width, state.height,
                               state.num_color_attachments, depth ? "+d" : "",
                               state.num_layers > 1 ? " layered" : "",
                               state.probe_view_mask != 0 ? " multiview" : "");
        };
        Name(key, Group::HostPass, describe);
    }
    Stamp(key, true);
}

void GpuPassProfiler::LabelPassIfNew(const RenderState& state, u64 key, Group group,
                                     const char* name) {
    if (!scheduler.WillBeginRendering(state)) {
        return;
    }
    const u64 sized = Mix(key, u64{state.width} | u64{state.height} << 16);
    const auto describe = [&] { return fmt::format("{} {}x{}", name, state.width, state.height); };
    Name(sized, group, describe);
    next_pass = sized;
}

u64 GpuPassProfiler::SubmitBegin() {
    const u64 resume = current;
    Stamp(GapKey, true);
    return resume;
}

void GpuPassProfiler::Resume(u64 key) {
    if (key == 0 || !frame_on) {
        return;
    }
    Stamp(key, false);
}

void GpuPassProfiler::Scope::Open(GpuPassProfiler* p, u64 key, Group group, const char* name) {
    if (name == nullptr || p->coarse) {
        return;
    }
    const u64 interrupted = p->current;
    const auto describe = [name] { return std::string{name}; };
    p->Name(key, group, describe);
    if (p->Stamp(key, true)) {
        profiler = p;
        resume = interrupted;
    }
}

void GpuPassProfiler::EndFrame() {
    ++frame_no;
    ++window_flips;
    auto& slice = slices[current_slice];
    bool continuing = false;
    if (frame_on && slice.used != 0) {
        if (scheduler.IsRendering()) {
            // Not expected at a flip: the frame goes on into the next one (never ends a pass).
            ++close_in_pass;
            continuing = true;
        } else {
            // The closing timestamp, in the submission that the flush right after makes.
            scheduler.Commands().writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, pool,
                                                 current_slice * SliceQueries + slice.used);
            ++slice.used;
            slice.tick = scheduler.CurrentTick();
            slice.pending = true;
            ++pending_count;
            current_slice = (current_slice + 1) % NumSlices;
        }
    }
    Collect();

    const bool wanted = FastRender::On(GpuPassProfile);
    if (wanted != was_wanted) {
        was_wanted = wanted;
        LOG_INFO(Render_Vulkan, "FAST gpu passes: {}",
                 wanted ? "on (fast bit 38 set)" : "paused (fast bit 38 clear)");
    }
    if (!wanted) {
        if (continuing) {
            // Its timestamps are never read; the slice's next use resets them in the stream.
            auto& open = slices[current_slice];
            open.used = 0;
            open.keys.clear();
            open.calls.clear();
        }
        frame_on = false;
    } else if (continuing) {
        frame_on = true;
    } else if (pending_count == NumSlices) {
        // Every slice still in flight (the GPU this far behind): skip a frame, never wait.
        ++window_skipped;
        frame_on = false;
    } else {
        auto& next = slices[current_slice];
        next.used = 0;
        next.keys.clear();
        next.calls.clear();
        next.frame = frame_no;
        next.not_ready = 0;
        next.pending = false;
        frame_on = true;
        // What the next command buffer runs before the frame's first mark (resumed after the
        // flush that follows).
        current = FrameStartKey;
        next_pass = 0;
    }
    active.store(frame_on, std::memory_order_relaxed);
    Report();
}

void GpuPassProfiler::Collect() {
    while (pending_count != 0) {
        auto& slice = slices[oldest];
        if (!scheduler.IsFree(slice.tick)) {
            break; // read once the submission with the closing timestamp is done: never waits
        }
        const vk::Result result = device.getQueryPoolResults(
            pool, oldest * SliceQueries, slice.used, slice.used * sizeof(u64), results.data(),
            sizeof(u64), vk::QueryResultFlagBits::e64);
        if (result == vk::Result::eNotReady && ++slice.not_ready < 8) {
            break; // not expected once the tick is done; a few frames' grace, then dropped
        }
        if (result == vk::Result::eSuccess) {
            Accumulate(slice);
        } else if (result == vk::Result::eNotReady) {
            ++unavailable;
        } else {
            ++read_errors;
        }
        slice.pending = false;
        slice.used = 0;
        slice.keys.clear();
        slice.calls.clear();
        oldest = (oldest + 1) % NumSlices;
        --pending_count;
    }
}

void GpuPassProfiler::Accumulate(GpuPassDetail::Slice& slice) {
    ++slices_read;
    const u32 count = slice.used;
    if (count < 2 || slice.keys.size() + 1 != count || slice.calls.size() + 1 != count) {
        ++read_errors;
        return;
    }
    // From the previous frame's close to this frame's first timestamp: no label.
    if (last_read_frame != ~u64{0} && slice.frame == last_read_frame + 1) {
        const u64 lost = (results[0] - last_close_stamp) & tick_mask;
        if (lost <= max_ticks) {
            window_lost_ticks += lost;
        }
    }
    u64 span = 0;
    for (u32 i = 0; i + 1 < count; ++i) {
        const u64 ticks = (results[i + 1] - results[i]) & tick_mask;
        if (ticks > max_ticks) {
            ++nonmonotonic;
            continue;
        }
        auto& total = totals[slice.keys[i]];
        total.ticks += ticks;
        total.calls += slice.calls[i];
        span += ticks;
    }
    window_span_ticks += span;
    window_stamps += count;
    ++window_frames;
    if (verify) {
        frame_spans.push_back(static_cast<float>(static_cast<double>(span) * tick_ms));
    }
    last_read_frame = slice.frame;
    last_close_stamp = results[count - 1];
}

void GpuPassProfiler::Report() {
    const auto now = std::chrono::steady_clock::now();
    if (now - window_start < std::chrono::seconds(2)) {
        return;
    }
    const double seconds = std::chrono::duration<double>(now - window_start).count();
    window_start = now;
    std::array<u64, NumExternal> external_ns{};
    std::array<u64, NumExternal> external_count{};
    for (u32 i = 0; i < NumExternal; ++i) {
        external_ns[i] = external[i].ns.exchange(0, std::memory_order_relaxed);
        external_count[i] = external[i].count.exchange(0, std::memory_order_relaxed);
    }
    const u64 flips = std::exchange(window_flips, 0);
    if (window_frames != 0) {
        const double frames = static_cast<double>(window_frames);
        struct Row {
            u64 key;
            double ms;
            double calls;
        };
        std::vector<Row> rows;
        rows.reserve(totals.size() + NumExternal);
        std::array<double, static_cast<size_t>(Group::Count)> group_ms{};
        const auto group_of = [&](u64 key) -> size_t {
            const auto it = labels.find(key);
            return it != labels.end() ? it->second.group : static_cast<size_t>(Group::HostPass);
        };
        for (const auto& [key, total] : totals) {
            const double ms = static_cast<double>(total.ticks) * tick_ms / frames;
            group_ms[group_of(key)] += ms;
            rows.push_back({key, ms, static_cast<double>(total.calls) / frames});
        }
        // The late warp: its own submissions (OpenXR thread), per game frame of this window.
        for (u32 i = 0; i < NumExternal; ++i) {
            if (external_count[i] == 0) {
                continue;
            }
            const double ms = static_cast<double>(external_ns[i]) * 1e-6 / frames;
            group_ms[static_cast<size_t>(Group::LateWarp)] += ms;
            rows.push_back(
                {ExternalKey + i, ms, static_cast<double>(external_count[i]) / frames});
        }
        std::sort(rows.begin(), rows.end(),
                  [](const Row& a, const Row& b) { return a.ms > b.ms; });
        std::string groups;
        for (size_t g = 0; g < group_ms.size(); ++g) {
            if (group_ms[g] > 0.0) {
                groups += fmt::format(" {} {:.3f}", GroupNames[g], group_ms[g]);
            }
        }
        std::string ranked;
        u32 shown = 0;
        double rest_ms = 0.0;
        for (const auto& row : rows) {
            const auto it = labels.find(row.key);
            if (shown < MaxRanked && row.ms >= 0.001 && it != labels.end()) {
                ranked += fmt::format(" | {} {:.3f} ms/f {:.1f}/f", it->second.name, row.ms,
                                      row.calls);
                ++shown;
            } else {
                rest_ms += row.ms;
            }
        }
        if (rest_ms > 0.0) {
            ranked += fmt::format(" | (other labels) {:.3f} ms/f", rest_ms);
        }
        const double span_ms = static_cast<double>(window_span_ticks) * tick_ms / frames;
        const double gap_ms = group_ms[static_cast<size_t>(Group::Gap)];
        LOG_INFO(Render_Vulkan,
                 "FAST gpu passes: frames={} flips={} fps={:.1f} span_ms/f={:.3f} "
                 "busy_ms/f={:.3f} gap_ms/f={:.3f} unlabelled_ms/f={:.3f} stamps/f={:.1f} "
                 "labels={} skipped={} overflow={} in_pass={}{} | groups:{}{}",
                 window_frames, flips, frames / seconds, span_ms, span_ms - gap_ms, gap_ms,
                 static_cast<double>(window_lost_ticks) * tick_ms / frames,
                 static_cast<double>(window_stamps) / frames, rows.size(), window_skipped,
                 overflow, in_pass, coarse ? " coarse" : "", groups, ranked);
        if (verify && !frame_spans.empty()) {
            const auto rank = [this](double q) -> double {
                const size_t n = frame_spans.size();
                const size_t k = std::min<size_t>(n - 1, static_cast<size_t>(q * n));
                std::nth_element(frame_spans.begin(), frame_spans.begin() + k, frame_spans.end());
                return frame_spans[k];
            };
            const double p50 = rank(0.5);
            const double p99 = rank(0.99);
            const double worst = *std::max_element(frame_spans.begin(), frame_spans.end());
            LOG_INFO(Render_Vulkan,
                     "FAST gpu passes verify: slices_read={} nonmonotonic={} unavailable={} "
                     "read_errors={} close_in_pass={} pending={} span_ms p50={:.3f} p99={:.3f} "
                     "max={:.3f} (VR_GPU_TIMING's `VR gpu frame:` span should match)",
                     slices_read, nonmonotonic, unavailable, read_errors, close_in_pass,
                     pending_count, p50, p99, worst);
        }
    }
    totals.clear();
    frame_spans.clear();
    window_frames = 0;
    window_span_ticks = 0;
    window_lost_ticks = 0;
    window_stamps = 0;
    window_skipped = 0;
    in_pass = 0;
    overflow = 0;
    close_in_pass = 0;
}

} // namespace Vulkan
