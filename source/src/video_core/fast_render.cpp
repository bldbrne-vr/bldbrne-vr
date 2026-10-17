#include "video_core/host_copies.h"
#include "../../scratchpad/fast-renderer/b_handler_profile.h"
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/fast_render.h"

#include <array>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <map>
#include <string>
#include <thread>

#include <fmt/format.h>
#include <magic_enum/magic_enum.hpp>
#include "common/logging/log.h"
#include "common/thread.h"
#include "video_core/amdgpu/pm4_opcodes.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace FastRender {

namespace {

constexpr auto kPollInterval = std::chrono::milliseconds(250);
constexpr auto kReportInterval = std::chrono::seconds(2);
constexpr std::array<const char*, static_cast<size_t>(Role::Count)> kRoleNames{"gpu", "recorder",
                                                                               "stageB", "copy0", "copy1"};

#ifdef _WIN32
std::array<HANDLE, static_cast<size_t>(Role::Count)> g_threads{};
std::mutex g_threads_mutex;
#endif

bool ParseMask(const std::string& text, u64& mask) {
    size_t at = text.find_first_not_of(" \t\r\n");
    if (at == std::string::npos) {
        return false;
    }
    try {
        size_t used = 0;
        mask = std::stoull(text.substr(at), &used, 0);
        return used > 0;
    } catch (...) {
        return false;
    }
}

struct ThreadSample {
    u64 cycles{};
    u64 cpu_100ns{};
    bool valid{};
};

ThreadSample SampleThread(Role role) {
#ifdef _WIN32
    std::scoped_lock lock{g_threads_mutex};
    const HANDLE thread = g_threads[static_cast<size_t>(role)];
    FILETIME created{}, exited{}, kernel{}, user{};
    ULONG64 cycles{};
    if (thread && GetThreadTimes(thread, &created, &exited, &kernel, &user)) {
        QueryThreadCycleTime(thread, &cycles);
        const auto ticks = [](FILETIME value) {
            return (u64(value.dwHighDateTime) << 32) | value.dwLowDateTime;
        };
        return {cycles, ticks(kernel) + ticks(user), true};
    }
#endif
    return {};
}

struct FlushCount {
    u64 calls{}, drains{}, previous_calls{}, previous_drains{};
};
std::mutex flush_mutex;
std::map<std::string, FlushCount> flush_counts;
void ReportRecorderFlushes(double frames) {
    const auto packets = g_counters.encoded_packets.exchange(0, std::memory_order_relaxed);
    const auto bytes = g_counters.encoded_bytes.exchange(0, std::memory_order_relaxed);
    const auto ns = g_counters.encode_sampled_ns.exchange(0, std::memory_order_relaxed);
    const auto samples = g_counters.encode_samples.exchange(0, std::memory_order_relaxed);
    const auto replay = g_counters.replay_ns.exchange(0, std::memory_order_relaxed);
    const auto wait = g_counters.recorder_wait_ns.exchange(0, std::memory_order_relaxed);
    const auto forwarded = g_counters.forwarded_fence_waits.exchange(0, std::memory_order_relaxed);
    const auto spin = g_counters.drain_spin_ns.exchange(0, std::memory_order_relaxed);
    // flush_spin_us/f: the part of flush_wait_us/f spent spinning (FAST_CPU_DRAIN_SPIN), busy
    // time on the waiting thread rather than sleep.
    if (frames > 0 && packets)
        LOG_INFO(Render_Vulkan,
                 "FAST encode: encode_us/f={:.3f} sampled=1/{} samples={} packets/f={:.1f} "
                 "bytes/f={:.0f} replay_us/f={:.3f} flush_wait_us/f={:.3f} "
                 "flush_spin_us/f={:.3f} forwarded_fence_waits/f={:.3f}",
                 ns * double(g_encode_sample_every) / 1000.0 / frames, g_encode_sample_every,
                 samples, packets / frames, bytes / frames, replay / 1000.0 / frames,
                 wait / 1000.0 / frames, spin / 1000.0 / frames, forwarded / frames);
    std::scoped_lock lock{flush_mutex};
    if (flush_counts.empty() || frames <= 0)
        return;
    std::string reasons;
    u64 total = 0;
    for (auto& [name, c] : flush_counts) {
        const u64 calls = c.calls - c.previous_calls, drains = c.drains - c.previous_drains;
        c.previous_calls = c.calls;
        c.previous_drains = c.drains;
        if (!calls)
            continue;
        total += drains;
        reasons += fmt::format(" {}={:.3f}/{:.3f}", name, calls / frames, drains / frames);
    }
    LOG_INFO(Render_Vulkan, "FAST recorder: flushes/f={:.3f} reason_calls/drains_per_frame:{}",
             total / frames, reasons);
}

void Run(std::stop_token stop, std::string file) {
    Common::SetCurrentThreadName("fast:Stats");
    using Clock = std::chrono::steady_clock;
    std::string last_text;
    auto window_start = Clock::now();
    const auto reporting_start = window_start;
    std::array<ThreadSample, static_cast<size_t>(Role::Count)> samples_start{};
    for (size_t i = 0; i < samples_start.size(); ++i) {
        samples_start[i] = SampleThread(static_cast<Role>(i));
    }
    u64 draws_start = g_counters.draws.load(std::memory_order_relaxed);
    u64 dispatches_start = g_counters.dispatches.load(std::memory_order_relaxed);
    u64 frames_start = g_counters.frames.load(std::memory_order_relaxed);
    u64 faults_start = g_counters.faults.load(std::memory_order_relaxed);
    u64 mask_start = g_mask.load(std::memory_order_relaxed);
    bool mixed = false;
    const double tsc_hz = Common::EstimateRDTSCFrequency();
    LOG_INFO(Render_Vulkan, "FAST TSC calibration: {:.0f} Hz, B profile {}", tsc_hz,
             BProfile::Enabled());
    std::array<u64, BProfile::Count> profile_start{};
    u64 wait_start = g_counters.pipe_wait_ns.load(std::memory_order_relaxed);
    std::array<u64, 10> drains_start{};
    std::array<u64, 256> opcode_start{};
    u64 handler_start = 0, packets_start = 0, fence_waits_start = 0, trivial_start = 0;
    u64 drain_spin_b_start = 0;
    constexpr std::array<const char*, 10> drain_names{"opcode", "selection", "commands", "compute",
                                                      "const",  "submit",    "idle",     "entry",
                                                      "toggle", "shutdown"};

    while (!stop.stop_requested()) {
        std::this_thread::sleep_for(kPollInterval);
        if (!file.empty()) {
            std::ifstream in(file);
            std::string text((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
            u64 mask = 0;
            if (in && text != last_text && ParseMask(text, mask)) {
                last_text = text;
                const u64 old = g_mask.exchange(mask, std::memory_order_relaxed);
                if (old != mask) {
                    mixed = true;
                    LOG_INFO(Render_Vulkan, "FAST toggles: {:#x} -> {:#x}", old, mask);
                }
            }
        }
        const auto now = Clock::now();
        if (now - window_start < kReportInterval) {
            continue;
        }
        const double seconds = std::chrono::duration<double>(now - window_start).count();
        const u64 draws = g_counters.draws.load(std::memory_order_relaxed);
        const u64 dispatches = g_counters.dispatches.load(std::memory_order_relaxed);
        const u64 frames = g_counters.frames.load(std::memory_order_relaxed);
        const u64 faults = g_counters.faults.load(std::memory_order_relaxed);
        const double frame_count = static_cast<double>(frames - frames_start);
        const double draw_count = static_cast<double>(draws - draws_start);
        const u64 mask = g_mask.load(std::memory_order_relaxed);
        std::string threads;
        for (size_t i = 0; i < samples_start.size(); ++i) {
            const auto sample = SampleThread(static_cast<Role>(i));
            const auto previous = samples_start[i];
            samples_start[i] = sample;
            if (!sample.valid || !previous.valid || sample.cpu_100ns < previous.cpu_100ns) {
                continue;
            }
            // CPU service includes user/kernel work and spin waits, not sleeping time.
            // Cycles remain a separate counter: they are not a duration.
            const double busy_us = (sample.cpu_100ns - previous.cpu_100ns) * 0.1;
            const double cycles = double(sample.cycles - previous.cycles);
            if (i == static_cast<size_t>(Role::GpuCommand)) {
                threads += fmt::format(" render_us/f={:.0f}",
                                       frame_count > 0 ? busy_us / frame_count : 0.0);
            }
            threads += fmt::format(
                " {}_busy={:.1f}% {}_us/f={:.0f} {}_us/draw={:.3f} {}_cycles/draw={:.0f}",
                kRoleNames[i], 100.0 * busy_us / (seconds * 1e6), kRoleNames[i],
                frame_count > 0 ? busy_us / frame_count : 0.0, kRoleNames[i],
                draw_count > 0 ? busy_us / draw_count : 0.0, kRoleNames[i],
                draw_count > 0 ? cycles / draw_count : 0.0);
            if (i == static_cast<size_t>(Role::StageB)) {
                // Included in the busy figures above: B's recorder flush waits spinning
                // (FAST_CPU_DRAIN_SPIN), which can raise them while fps improves. Judge the spin
                // switches by fps and frame time.
                const u64 spin = g_counters.drain_spin_b_ns.load(std::memory_order_relaxed);
                threads += fmt::format(" stageB_drain_spin_us/f={:.1f}",
                                       frame_count > 0 ? double(spin - drain_spin_b_start) / 1e3 /
                                                             frame_count
                                                       : 0.0);
                drain_spin_b_start = spin;
            }
        }
        {
        std::scoped_lock copy_stats_lock{host_copy_stats_mutex};
        if (auto* queue = active_host_copies.load(std::memory_order_acquire)) {
            static u64 previous_bytes{}, previous_batches{};
            const auto bytes = queue->copied_bytes.load(std::memory_order_relaxed);
            const auto batches = queue->completed_batches.load(std::memory_order_relaxed);
            threads += fmt::format(" copy_bytes/f={:.0f} copy_batches/f={:.3f}",
                frame_count ? double(bytes - previous_bytes) / frame_count : 0.0,
                frame_count ? double(batches - previous_batches) / frame_count : 0.0);
            previous_bytes = bytes; previous_batches = batches;
            static std::array<u64, 3> previous_calls{}, previous_ns{};
            constexpr const char* names[]{"protection", "mapping", "submit"};
            for (unsigned i = 0; i < 3; ++i) {
                const auto calls = copy_wait_calls[i].load(std::memory_order_relaxed);
                const auto ns = copy_wait_ns[i].load(std::memory_order_relaxed);
                threads += fmt::format(" copy_wait_{}/f={:.3f} copy_wait_{}_us/f={:.3f}",
                    names[i], frame_count ? double(calls - previous_calls[i]) / frame_count : 0.0,
                    names[i], frame_count ? double(ns - previous_ns[i]) / 1e3 / frame_count : 0.0);
                previous_calls[i] = calls; previous_ns[i] = ns;
            }
        }
        }
        const auto wait_now = g_counters.pipe_wait_ns.load(std::memory_order_relaxed);
        threads += fmt::format(" pipe_wait_ms={:.3f}", double(wait_now - wait_start) / 1e6);
        wait_start = wait_now;
        u64 drain_total = 0;
        for (size_t i = 0; i < drains_start.size(); ++i) {
            const auto now = g_counters.pipe_drains[i].load(std::memory_order_relaxed);
            threads += fmt::format(" drain_{}={}", drain_names[i], now - drains_start[i]);
            drain_total += now - drains_start[i];
            drains_start[i] = now;
        }
        for (size_t i = 0; i < opcode_start.size(); ++i) {
            const auto value = g_counters.pipe_opcode_drains[i].load(std::memory_order_relaxed);
            const auto delta = value - opcode_start[i];
            if (delta)
                threads +=
                    fmt::format(" drain_pm4_{:02x}_{}={}", i,
                                magic_enum::enum_name(static_cast<AmdGpu::PM4ItOpcode>(i)), delta);
            opcode_start[i] = value;
        }
        const auto ticks = g_counters.handler_ticks.load(std::memory_order_relaxed);
        const auto packets = g_counters.handler_packets.load(std::memory_order_relaxed);
        const auto packet_count = packets - packets_start;
        // Elapsed TSC ticks exclude queue spin/sleep but can include preemption. With
        // FAST_CPU_IDLE_CLOCK (the default unless FAST_B_PROFILE) they are B's busy stretches:
        // the handlers plus the ring's per-packet bookkeeping between them (a few ns a packet),
        // added with their packets every 64 packets and when B goes idle, so a window's
        // handler_* fields can lag by up to 63 packets.
        threads += fmt::format(" drains/f={:.2f} handler_ticks/packet={:.0f} handler_packets={}",
                               frame_count > 0 ? drain_total / frame_count : 0.0,
                               packet_count ? double(ticks - handler_start) / packet_count : 0.0,
                               packet_count);
        threads += fmt::format(
            " handler_us/packet={:.3f} handler_us/f={:.3f}",
            packet_count ? double(ticks - handler_start) / tsc_hz * 1e6 / packet_count : 0.0,
            frame_count > 0 ? double(ticks - handler_start) / tsc_hz * 1e6 / frame_count : 0.0);
        if (BProfile::Enabled()) {
            for (size_t i = 0; i < BProfile::Count; ++i) {
                const auto value = BProfile::totals[i].load(std::memory_order_relaxed);
                threads += fmt::format(" B_{}_us/f={:.3f}", BProfile::names[i],
                                       frame_count > 0 ? double(value - profile_start[i]) / tsc_hz *
                                                             1e6 / frame_count
                                                       : 0.0);
                profile_start[i] = value;
            }
        }
        const auto fence_waits = g_counters.queued_fence_waits.load(std::memory_order_relaxed);
        threads += fmt::format(" queued_fence_waits={}", fence_waits - fence_waits_start);
        fence_waits_start = fence_waits;
        const auto trivial = g_counters.trivial_waits.load(std::memory_order_relaxed);
        threads +=
            fmt::format(" trivial_waits={} trivial_waits/f={:.3f}", trivial - trivial_start,
                        frame_count > 0 ? double(trivial - trivial_start) / frame_count : 0.0);
        trivial_start = trivial;
        handler_start = ticks;
        packets_start = packets;
        LOG_INFO(Render_Vulkan,
                 "FAST stats: mask={:#x} mixed={} fps={:.2f} draws/f={:.0f} disp/f={:.0f}{} "
                 "faults/s={:.0f} elapsed_s={:.3f} window_s={:.3f} gameplay_compiles={}",
                 mask, (mixed || mask != mask_start) ? 1 : 0, frame_count / seconds,
                 frame_count > 0 ? draw_count / frame_count : 0.0,
                 frame_count > 0 ? (dispatches - dispatches_start) / frame_count : 0.0, threads,
                 (faults - faults_start) / seconds,
                 std::chrono::duration<double>(now - reporting_start).count(), seconds,
                 g_counters.gameplay_compiles.load(std::memory_order_relaxed));
        ReportRecorderFlushes(frame_count);
        if (CacheStatsEnabled()) {
            const auto take = [](std::atomic<u64>& value) {
                return value.exchange(0, std::memory_order_relaxed);
            };
            const u64 desc_calls = take(g_counters.texture_desc_calls);
            const u64 desc_misses = take(g_counters.texture_desc_misses);
            const u64 view_calls = take(g_counters.view_calls);
            const u64 view_steps = take(g_counters.view_steps);
            const u64 view_misses = take(g_counters.view_misses);
            LOG_INFO(Render_Vulkan,
                     "FAST caches: desc_calls={} desc_misses={} desc_miss_pct={:.3f} "
                     "view_calls={} view_steps={} view_misses={} view_steps_avg={:.3f}",
                     desc_calls, desc_misses, desc_calls ? 100.0 * desc_misses / desc_calls : 0.0,
                     view_calls, view_steps, view_misses,
                     view_calls ? double(view_steps) / view_calls : 0.0);
        }
        window_start = now;
        draws_start = draws;
        dispatches_start = dispatches;
        frames_start = frames;
        faults_start = faults;
        mask_start = mask;
        mixed = false;
    }
}

} // namespace

void Start() {
    static std::once_flag once;
    std::call_once(once, [] {
        u64 mask = kDefaultMask;
        if (const char* env = std::getenv("FAST_TOGGLES"); env && ParseMask(env, mask)) {
            g_mask.store(mask, std::memory_order_relaxed);
        }
        std::string file;
        if (const char* env = std::getenv("FAST_TOGGLE_FILE")) {
            file = env;
        }
        LOG_INFO(Render_Vulkan, "FAST toggles: start mask {:#x}, file '{}'",
                 g_mask.load(std::memory_order_relaxed), file);
        static std::jthread thread([file](std::stop_token stop) { Run(stop, file); });
    });
}

void NoteRecorderEncoded(u64 packets, u64 bytes, u64 ns, u64 samples) {
    g_counters.encoded_packets.fetch_add(packets, std::memory_order_relaxed);
    g_counters.encoded_bytes.fetch_add(bytes, std::memory_order_relaxed);
    g_counters.encode_sampled_ns.fetch_add(ns, std::memory_order_relaxed);
    g_counters.encode_samples.fetch_add(samples, std::memory_order_relaxed);
}
void NoteRecorderReplay(u64 ns) {
    g_counters.replay_ns.fetch_add(ns, std::memory_order_relaxed);
}
void NoteRecorderWait(u64 ns) {
    g_counters.recorder_wait_ns.fetch_add(ns, std::memory_order_relaxed);
}
void NoteRecorderSpin(u64 ns, bool on_stage_b) {
    g_counters.drain_spin_ns.fetch_add(ns, std::memory_order_relaxed);
    if (on_stage_b)
        g_counters.drain_spin_b_ns.fetch_add(ns, std::memory_order_relaxed);
}

void NoteRecorderFlush(std::string_view reason, std::source_location site, bool pending) {
    std::string key{reason};
    if (reason.starts_with("raw")) {
        std::string_view file{site.file_name()};
        file.remove_prefix(file.find_last_of("/\\") + 1);
        key += fmt::format("@{}:{}", file, site.line());
    }
    std::scoped_lock lock{flush_mutex};
    auto& count = flush_counts[key];
    ++count.calls;
    count.drains += pending;
}

void RegisterThread(Role role) {
#ifdef _WIN32
    HANDLE handle = nullptr;
    if (DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &handle,
                        THREAD_QUERY_LIMITED_INFORMATION, FALSE, 0)) {
        std::scoped_lock lock{g_threads_mutex};
        auto& slot = g_threads[static_cast<size_t>(role)];
        if (slot)
            CloseHandle(slot);
        slot = handle;
    }
#endif
}

} // namespace FastRender
