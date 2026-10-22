// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// bbport-inspired ordered recording worker; Windows atomic waits replace futex calls.
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <thread>
#include <immintrin.h>
#include "video_core/fast/b_handler_profile.h"
#include "video_core/fast/busy_clock.h"
#include "video_core/fast/byte_ring.h"
#include "common/logging/log.h"
#include "common/rdtsc.h"
#include "common/thread.h"
#include "video_core/fast_render.h"

namespace Vulkan {
// FAST_CPU_IDLE_CLOCK (on unless '0'; read once at start-up): without FAST_B_PROFILE, stage B
// times its busy stretches (FastRender::BusyClock) instead of every handler, and its idle spin
// reads the clock every 64 pauses instead of after each one. That clock read was the 0.5% of B's
// samples in QueryPerformanceCounter: idle time, not per-packet work. The FAST stats' handler
// fields then also count the ring's per-packet bookkeeping (see fast_render.cpp).
inline const bool g_pipe_idle_clock = [] {
    const char* value = std::getenv("FAST_CPU_IDLE_CLOCK");
    return !value || value[0] != '0';
}();
// FAST_CPU_IDLE_CLOCK_VERIFY=1: every handler is timed as well, and each window must cover them.
inline const bool g_pipe_idle_clock_verify = [] {
    const char* value = std::getenv("FAST_CPU_IDLE_CLOCK_VERIFY");
    return value && value[0] == '1';
}();

class DrawPipe {
public:
    using Handler = void (*)(void*, std::span<const std::byte>);
    // `task_hook` (FAST_CPU_TASK_PUBLISH=2, may be null): stage B calls it with `context` before
    // each ordered task, i.e. every packet not for `handler`.
    DrawPipe(Handler handler, void* context, void (*task_hook)(void*) = nullptr)
        : default_handler(handler), default_context(context), task_hook(task_hook),
          thread([this](std::stop_token stop) { Run(stop); }) {}
    ~DrawPipe() {
        Drain(Shutdown);
        thread.request_stop();
        ready.fetch_add(1, std::memory_order_seq_cst);
        ready.notify_one();
        thread.join(); // All atomics/ring storage must outlive the worker.
    }
    enum Reason : u32 {
        Opcode = 0,
        Selection,
        Commands,
        Compute,
        ConstRam,
        Submission,
        Idle,
        RasterizerEntry,
        Toggle,
        Shutdown,
        Count
    };
    void Push(std::span<const std::byte> packet) {
        PushTask(default_handler, default_context, packet);
    }
    u64 PushTask(Handler handler, void* context, std::span<const std::byte> packet) {
        const Envelope envelope{handler, context};
        auto* destination = ring.Begin(static_cast<u32>(sizeof(envelope) + packet.size()));
        std::memcpy(destination, &envelope, sizeof(envelope));
        if (!packet.empty())
            std::memcpy(destination + sizeof(envelope), packet.data(), packet.size());
        ring.Commit();
        Signal();
        return ring.Published();
    }
    template <class Encode>
    u64 PushEncoded(u32 size, Encode&& encode) {
        const Envelope envelope{default_handler, default_context};
        auto* destination = ring.Begin(static_cast<u32>(sizeof(envelope)) + size);
        std::memcpy(destination, &envelope, sizeof(envelope));
        encode(destination + sizeof(envelope));
        ring.Commit();
        Signal();
        return ring.Published();
    }
    bool Pending(u64 ticket) const {
        return ticket && !ring.Reached(ticket);
    }
    bool Drain(u32 reason, u32 opcode = 256) {
        if (FastRender::IsStageB() || ring.Reached(ring.Published()))
            return false;
        const auto start = std::chrono::steady_clock::now();
        if (ring.Drain())
            ReportLostWake();
        if (opcode < 256)
            FastRender::g_counters.pipe_opcode_drains[opcode].fetch_add(1,
                                                                        std::memory_order_relaxed);
        FastRender::g_counters.pipe_drains[std::min<u32>(reason, Count - 1)].fetch_add(
            1, std::memory_order_relaxed);
        FastRender::g_counters.pipe_wait_ns.fetch_add(
            std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() -
                                                                 start)
                .count(),
            std::memory_order_relaxed);
        return true;
    }
    /// bbport WaitForPacket: wait until B has consumed `ticket`, not the whole queue.
    /// Wait time joins the aggregate pipe wait; the full-drain counters are unchanged.
    u64 WaitFor(u64 ticket) {
        if (FastRender::IsStageB() || !Pending(ticket))
            return 0;
        const auto start = std::chrono::steady_clock::now();
        if (ring.Wait(ticket))
            ReportLostWake();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
        FastRender::g_counters.pipe_wait_ns.fetch_add(ns, std::memory_order_relaxed);
        return static_cast<u64>(ns);
    }
    u64 Published() const {
        return ring.Published();
    }

private:
    void Signal() {
        // SC handshake pairs with the consumer's sleeping announcement/recheck.
        if (sleeping.load(std::memory_order_seq_cst)) {
            ready.fetch_add(1, std::memory_order_seq_cst);
            ready.notify_one();
        }
    }
    void Run(std::stop_token stop) {
        FastRender::g_on_stage_b = true;
        Common::SetCurrentThreadName("fast:StageB");
        Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
        FastRender::RegisterThread(FastRender::Role::StageB);
        const auto spin = std::chrono::microseconds([] {
            const char* value = std::getenv("FAST_PIPE_SPIN_US");
            return value ? std::clamp(std::atoi(value), 0, 1000) : 50;
        }());
        // Fenced timestamps only for the detailed B profile; handler_ticks tolerates the skew.
        const bool b_profile = FastRender::BProfile::Enabled();
        // FAST_CPU_IDLE_CLOCK: the detailed B profile keeps the per-packet timestamps it needs.
        const bool idle_clock = g_pipe_idle_clock && !b_profile;
        const bool clock_verify = idle_clock && g_pipe_idle_clock_verify;
        LOG_INFO(Render_Vulkan, "FAST stage B: wakes {} (FAST_CPU_LAZY_WAKE), clock {} "
                                "(FAST_CPU_IDLE_CLOCK)",
                 WakeName(ring.WakeMode()),
                 b_profile ? "per packet (FAST_B_PROFILE)"
                           : !idle_clock ? "per packet"
                                         : clock_verify ? "busy stretches, verify" : "busy stretches");
        FastRender::BusyClock busy;
        ClockCheck check;
        bool idle = true; // FAST_CPU_IDLE_CLOCK: no window open
        u64 handled = 0;  // the log cadence
        while (!stop.stop_requested()) {
            auto packet = ring.Peek();
            if (packet.empty()) {
                if (!idle) {
                    idle = true;
                    CloseWindow(busy, check, clock_verify);
                }
                const auto until = std::chrono::steady_clock::now() + spin;
                if (idle_clock) {
                    // The clock every 64 pauses (bbport: 256), not after each one.
                    for (u32 pauses = 1;
                         spin.count() && packet.empty() && !stop.stop_requested(); ++pauses) {
                        _mm_pause();
                        packet = ring.Peek();
                        if ((pauses & 63) == 0 && std::chrono::steady_clock::now() >= until)
                            break;
                    }
                } else {
                    while (packet.empty() && !stop.stop_requested() &&
                           std::chrono::steady_clock::now() < until) {
                        _mm_pause();
                        packet = ring.Peek();
                    }
                }
            }
            if (packet.empty()) {
                const auto seen = ready.load(std::memory_order_seq_cst);
                sleeping.store(true, std::memory_order_seq_cst);
                if (ring.Reached(ring.Published()) && !stop.stop_requested())
                    ready.wait(seen);
                sleeping.store(false, std::memory_order_seq_cst);
                continue;
            }
            Envelope envelope;
            std::memcpy(&envelope, packet.data(), sizeof(envelope));
            if (task_hook && envelope.handler != default_handler)
                task_hook(default_context);
            if (idle_clock) {
                if (idle) {
                    idle = false;
                    busy.Open(__rdtsc());
                }
                if (clock_verify) {
                    const auto handler_start = __rdtsc();
                    envelope.handler(envelope.context, packet.subspan(sizeof(envelope)));
                    busy.timed += __rdtsc() - handler_start;
                } else {
                    envelope.handler(envelope.context, packet.subspan(sizeof(envelope)));
                }
                if (busy.Packet())
                    CloseWindow(busy, check, clock_verify);
            } else {
                const auto handler_start = b_profile ? Common::FencedRDTSC() : __rdtsc();
                FastRender::BProfile::Begin(handler_start, envelope.handler != default_handler);
                envelope.handler(envelope.context, packet.subspan(sizeof(envelope)));
                const auto handler_end = b_profile ? Common::FencedRDTSC() : __rdtsc();
                const auto handler_ticks = handler_end - handler_start;
                FastRender::BProfile::End(handler_end);
                FastRender::Bump(FastRender::g_counters.handler_packets);
                auto& total = FastRender::g_counters.handler_ticks;
                total.store(total.load(std::memory_order_relaxed) + handler_ticks,
                            std::memory_order_relaxed);
            }
            ring.Release();
            if constexpr (Common::Log::Enabled) {
                if ((++handled & ((u64{1} << 18) - 1)) == 0)
                    ReportPipe(check, clock_verify);
            }
        }
        if (!idle)
            CloseWindow(busy, check, clock_verify);
    }
    // FAST_CPU_IDLE_CLOCK_VERIFY's totals (stage B only).
    struct ClockCheck {
        u64 windows{}, ticks{}, timed{}, mismatches{};
    };
    // FAST_CPU_IDLE_CLOCK: closes the open window and adds it to the FAST stats' handler totals
    // (stage B is their only writer).
    static void CloseWindow(FastRender::BusyClock& busy, ClockCheck& check, bool verify) {
        const auto window = busy.Close(__rdtsc());
        auto& ticks = FastRender::g_counters.handler_ticks;
        ticks.store(ticks.load(std::memory_order_relaxed) + window.ticks,
                    std::memory_order_relaxed);
        auto& packets = FastRender::g_counters.handler_packets;
        packets.store(packets.load(std::memory_order_relaxed) + window.packets,
                      std::memory_order_relaxed);
        if (!verify)
            return;
        ++check.windows;
        check.ticks += window.ticks;
        check.timed += window.timed;
        // The window spans its handlers and the bookkeeping between them, so it never lasts less.
        if (window.ticks < window.timed && ++check.mismatches <= 8)
            LOG_ERROR(Render_Vulkan,
                      "FAST_CPU_IDLE_CLOCK verify: a window of {} packets lasted {} ticks, less "
                      "than its handlers' {} (mismatches={} of windows={})",
                      window.packets, window.ticks, window.timed, check.mismatches,
                      check.windows);
    }
    static const char* WakeName(FastRender::RingWake mode) {
        switch (mode) {
        case FastRender::RingWake::EveryPacket:
            return "every packet";
        case FastRender::RingWake::Target:
            return "at the sleeper's target";
        case FastRender::RingWake::Verify:
            return "at the sleeper's target, verify";
        }
        return "?";
    }
    // Stage B, every 2^18 packets (about 2 s): the wake counts, and the idle clock's check.
    void ReportPipe(const ClockCheck& check, bool clock_verify) const {
        const auto wakes = ring.Counts();
        // post_wake_spin_us: the woken waiters' spins (FAST_CPU_LAZY_WAKE), in their busy time.
        LOG_INFO(Render_Vulkan,
                 "FAST lazy wake: mode={} sleeps={} wakes={} would_wake={} verified={} "
                 "mismatches={} post_wake_spin_us={}",
                 WakeName(ring.WakeMode()), wakes.sleeps, wakes.wakes, wakes.would_wake,
                 wakes.verified, wakes.mismatches, wakes.post_wake_spin_ns / 1000);
        // Space's waits (the producer finding the ring full) report nothing themselves.
        if (wakes.mismatches)
            LOG_ERROR(Render_Vulkan, "FAST_CPU_LAZY_WAKE verify: {} waits had no targeted wake",
                      wakes.mismatches);
        if (clock_verify)
            LOG_INFO(Render_Vulkan,
                     "FAST idle clock verify: windows={} busy_ticks={} handler_ticks={} "
                     "bookkeeping={:.3f}% mismatches={}",
                     check.windows, check.ticks, check.timed,
                     check.ticks ? 100.0 * double(check.ticks - std::min(check.ticks, check.timed)) /
                                       double(check.ticks)
                                 : 0.0,
                     check.mismatches);
    }
    // FAST_CPU_LAZY_WAKE_VERIFY (the waiting thread): stage B reached and decided on this wait's
    // target without the targeted wake; the every-packet wake woke the waiter instead.
    void ReportLostWake() const {
        const auto lost = ring.LastLostWake();
        if (lost.mismatches <= 8)
            LOG_ERROR(Render_Vulkan,
                      "FAST_CPU_LAZY_WAKE verify: a wait for {:#x} had no targeted wake (B decided "
                      "on {:#x}; mismatches={} of verified={})",
                      lost.target, lost.decided, lost.mismatches, lost.verified);
    }
    struct Envelope {
        Handler handler;
        void* context;
    };
    static_assert(sizeof(Envelope) % 16 == 0); // Aligned typed packet arrays follow it.
    Handler default_handler;
    void* default_context;
    void (*const task_hook)(void*);
    FastRender::ByteRing<> ring;
    std::atomic<u64> ready{0};
    std::atomic<bool> sleeping{false};
    std::jthread thread;
};
} // namespace Vulkan
