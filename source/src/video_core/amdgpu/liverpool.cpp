#include "video_core/flip_order_trace.h"
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/fast_render.h"
#include "video_core/fast/drain_audit.h"
#include "video_core/native_stereo/graphics_submission_scan.h"
#include "video_core/native_stereo/speculative_pool.h"
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <string_view>

#include <boost/preprocessor/stringize.hpp>

#include "common/assert.h"
#include "common/debug.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/process.h"
#include "core/libraries/videoout/driver.h"
#include "core/libraries/videoout/video_out.h"
#include "core/memory.h"
#include "core/platform.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace AmdGpu {
namespace DA = FastRender::DrainAudit;

// Rate-limited coroutine progress diagnostic; no wait or fence is changed.
static void TraceYield(const char* name, u32 line) {
    static const bool enabled = std::getenv("FAST_WAIT_TRACE") != nullptr;
    if (!enabled)
        return;
    static thread_local std::array<u64, 4096> counts{};
    auto& count = counts[line % counts.size()];
    ++count;
    if (count >= 1024 && (count & (count - 1)) == 0)
        LOG_WARNING(Render, "FAST coroutine wait: task={} line={} yields={}", name, line, count);
}

static const char* dcb_task_name{"DCB_TASK"};
static const char* ccb_task_name{"CCB_TASK"};

#define MAX_NAMES 56
static_assert(Liverpool::NumComputeRings <= MAX_NAMES);

#define NAME_NUM(z, n, name) BOOST_PP_STRINGIZE(name) BOOST_PP_STRINGIZE(n),
#define NAME_ARRAY(name, num) {BOOST_PP_REPEAT(num, NAME_NUM, name)}

static const char* acb_task_name[] = NAME_ARRAY(ACB_TASK, MAX_NAMES);

#define YIELD(name)                                                                                \
    TraceYield(name, __LINE__);                                                                    \
    FIBER_EXIT;                                                                                    \
    co_yield {};                                                                                   \
    FIBER_ENTER(name);

#define YIELD_CE() YIELD(ccb_task_name)
#define YIELD_GFX() YIELD(dcb_task_name)
#define YIELD_ASC(id) YIELD(acb_task_name[id])

#define RESUME(task, name)                                                                         \
    FIBER_EXIT;                                                                                    \
    task.handle.resume();                                                                          \
    FIBER_ENTER(name);

#define RESUME_CE(task) RESUME(task, ccb_task_name)
#define RESUME_GFX(task) RESUME(task, dcb_task_name)
#define RESUME_ASC(task, id) RESUME(task, acb_task_name[id])

std::array<u8, 48_KB> Liverpool::ConstantEngine::constants_heap;

static std::span<const u32> NextPacket(std::span<const u32> span, size_t offset) {
    if (offset > span.size()) {
        LOG_ERROR(
            Lib_GnmDriver,
            ": packet length exceeds remaining submission size. Packet dword count={}, remaining "
            "submission dwords={}",
            offset, span.size());
        // Return empty subspan so check for next packet bails out
        return {};
    }

    return span.subspan(offset);
}

// Malformed packets. A GFX DCB header the CP cannot parse (type 0 / 1, an unknown type-3
// opcode, an EOP packet with impossible selects) used to be fatal ("Unimplemented PM4 type 0").
// Bloodborne hits it in about 1 of 55 starts, always on the first loading-screen frame after a
// long CP stall, with payload-like values where a header should be: the buffer most likely
// changed in guest memory after the game submitted it. SubmitGfx keeps a copy of every DCB as
// submitted (about 20 MB/s, a memcpy on the submitting thread); the CP still parses guest memory
// exactly as before, and only at a malformed packet does it look at the copy: if the copy
// differs there and a packet of the copy starts there, parsing continues from the copy at the
// same offset; if not, the rest of that buffer is skipped. Either way the event is logged loudly
// (critical, with the buffer, the offset and both windows). VR_PM4_STRICT=1: no copy, and the old
// fatal error.
namespace {
u32 ScanVerifyInterval() {
    static const u32 interval = [] {
        const char* value = std::getenv("FAST_SCAN_VERIFY");
        return value ? static_cast<u32>(std::strtoul(value, nullptr, 10)) : 0u;
    }();
    return interval;
}

bool Pm4Strict() {
    static const bool strict = std::getenv("VR_PM4_STRICT") != nullptr;
    return strict;
}

std::atomic<u32> g_bad_packets{0};

std::string DwordWindow(const u32* p, size_t total, size_t off) {
    std::string s;
    const size_t lo = off >= 6 ? off - 6 : 0;
    const size_t hi = std::min(total, off + 7);
    for (size_t i = lo; i < hi; ++i) {
        s += fmt::format("{}{:08x}", i == off ? " >" : " ", p[i]);
    }
    return s;
}

// The EOP selects SignalFence can handle (anything else is UNREACHABLE / ASSERT there).
bool EopSelectsValid(const PM4CmdEventWriteEop* eop) {
    const u32 data_sel = static_cast<u32>(eop->data_sel.Value());
    const u32 int_sel = static_cast<u32>(eop->int_sel.Value());
    if (data_sel > static_cast<u32>(DataSelect::PerfCounter) ||
        int_sel > static_cast<u32>(InterruptSelect::IrqWhenWriteConfirm)) {
        return false;
    }
    return !(eop->int_sel.Value() == InterruptSelect::IrqOnly && data_sel != 0);
}

// VR_PM4_TEST_CORRUPT=<every_n>[,<both>] (tests only): every n-th GFX submit (from the 2000th,
// past the boot) gets a zero dword written over a packet header in the middle of its DCB after
// the copy was taken (<both>=1: before, so the copy has it too), as if the buffer had been
// overwritten after the submit.
void TestCorrupt(std::span<const u32> dcb, bool before_copy) {
    static const auto cfg = [] {
        std::pair<u32, u32> c{0, 0};
        if (const char* v = std::getenv("VR_PM4_TEST_CORRUPT")) {
            std::sscanf(v, "%u,%u", &c.first, &c.second);
        }
        return c;
    }();
    static u64 n = 0;
    if (cfg.first == 0 || (cfg.second != 0) != before_copy) {
        return;
    }
    if (++n < 2000 || n % cfg.first != 0 || dcb.size() < 64) {
        return;
    }
    size_t off = 0;
    while (off < dcb.size() / 2) {
        const u32 h = dcb[off];
        const u32 type = h >> 30;
        if (type == 2) {
            ++off;
        } else if (type == 3) {
            off += ((h >> 16) & 0x3fff) + 2;
        } else {
            return;
        }
    }
    if (off >= dcb.size()) {
        return;
    }
    LOG_WARNING(Render, "PM4 test: zeroing header {:08x} at +{:#x} of DCB {} ({} dw, {} the copy)",
                dcb[off], off, fmt::ptr(dcb.data()), dcb.size(), before_copy ? "before" : "after");
    // Through the backing (as the EOP writes): the DCB's pages can be write-protected.
    void* const at = const_cast<u32*>(dcb.data() + off);
    const u32 zero = 0;
    if (!Core::Memory::Instance()->TryWriteBacking(at, &zero, sizeof(zero))) {
        std::memcpy(at, &zero, sizeof(zero));
    }
}
} // namespace

Liverpool::Liverpool() {
    num_counter_pairs = Libraries::Kernel::sceKernelIsNeoMode() ? 16 : 8;
    if (ScanVerifyInterval()) {
        scan_pool = std::make_unique<ScratchDraw::SpeculativePool>(2, [] {
            Common::SetCurrentThreadName("scratch:Scan");
            Common::SetCurrentThreadPriority(Common::ThreadPriority::Low);
        });
    }
    process_thread = std::jthread{std::bind_front(&Liverpool::Process, this)};
}

Liverpool::~Liverpool() {
    process_thread.request_stop();
    process_thread.join();
    scan_pool.reset();
}

void Liverpool::SyncRecorder(const char* reason, bool submit) {
    if (!rasterizer)
        return;
    rasterizer->DrainDrawPipe(2);
    auto& scheduler = rasterizer->GetScheduler();
    if (!scheduler.RecorderThreaded())
        return;
    // SignalsNeedFlush: GuestSignalsPending() unless FAST_CPU_ASYNC_IDLE or
    // FAST_CPU_SIGNAL_AT_REPLAY, then only signals no queued submission follows.
    if (submit || scheduler.SignalsNeedFlush()) {
        if (Vulkan::g_task_publish)
            scheduler.PublishRecorder(); // FAST_CPU_TASK_PUBLISH: replay during the submission
        scheduler.Flush();
    }
    scheduler.FlushRecorder(reason);
}

// FAST_CPU_ASYNC_IDLE (vk_scheduler.h). The ordered retirement of a guest submission (stage B, or
// the command processor after draining B when the task was not queued). Before, the last
// outstanding one submitted, waited for the recorder's replay (the submission and the guest
// signals it writes), then decremented num_submits; now the recorder decrements it when its replay
// gets there, and B goes on. A retirement that is not the last never waited and still does not.
// False: the old path (recorder not threaded, or the switch off); with
// FAST_CPU_ASYNC_IDLE_VERIFY the old path runs here with the replay's check.
bool Liverpool::RetireSubmission(Task::Handle finished) {
    if (!(Vulkan::g_async_idle || Vulkan::g_async_idle_verify) || !rasterizer)
        return false;
    rasterizer->DrainDrawPipe(2); // as SyncRecorder: a no-op on B and after the caller's drain
    auto& scheduler = rasterizer->GetScheduler();
    if (!scheduler.RecorderThreaded())
        return false;
    bool last;
    {
        std::scoped_lock lock{submit_mutex};
        // num_submits as the old path saw it: retirements still queued already counted out.
        last = Vulkan::SignalPolicy::LastSubmission(num_submits, async_retires);
    }
    if (last) {
        // SyncRecorder("gpu_idle", true) up to its drain.
        if (Vulkan::g_task_publish)
            scheduler.PublishRecorder();
        scheduler.Flush();
        // The submission may have changed the recorder's mode (a FAST mask change): the old drain.
        const bool queue = scheduler.RecorderThreaded();
        const u64 signals = scheduler.GuestSignalsEncoded();
        if (Vulkan::g_async_idle && queue) {
            // The frame stays alive until here on B, as before; nothing replayed refers to it.
            finished.destroy();
            {
                // Before its task is queued: the replay decrements both.
                std::scoped_lock lock{submit_mutex};
                ++async_retires;
            }
            async_tasks.fetch_add(1, std::memory_order_relaxed);
            scheduler.QueueReplayTask(&RetireOnReplay, this, signals);
            return true;
        }
        if (Vulkan::g_async_idle_verify && queue) {
            async_tasks.fetch_add(1, std::memory_order_relaxed);
            scheduler.QueueReplayTask(&VerifyOnReplay, this, signals);
        }
        scheduler.FlushRecorder("gpu_idle");
    }
    finished.destroy();
    std::scoped_lock lock{submit_mutex};
    --num_submits;
    submit_cv.notify_all();
    return true;
}

// FAST_CPU_ASYNC_IDLE. The GPU-idle task: the interrupt (when nothing was submitted meanwhile) is
// raised by the recorder once its replay has written everything encoded before, as B's drain
// waited for. False: the old path; FAST_CPU_ASYNC_IDLE_VERIFY runs the old path here and compares
// the replay's decision with B's.
bool Liverpool::SignalIdle() {
    if (!(Vulkan::g_async_idle || Vulkan::g_async_idle_verify) || !rasterizer)
        return false;
    rasterizer->DrainDrawPipe(2); // as SyncRecorder: a no-op on B and after the caller's drain
    auto& scheduler = rasterizer->GetScheduler();
    if (!scheduler.RecorderThreaded())
        return false;
    // SyncRecorder("gpu_idle_signal") up to its drain.
    if (scheduler.SignalsNeedFlush()) {
        if (Vulkan::g_task_publish)
            scheduler.PublishRecorder();
        scheduler.Flush();
    }
    const bool queue = scheduler.RecorderThreaded(); // see RetireSubmission
    const u64 signals = scheduler.GuestSignalsEncoded();
    if (Vulkan::g_async_idle && queue) {
        async_tasks.fetch_add(1, std::memory_order_relaxed);
        scheduler.QueueReplayTask(&IdleOnReplay, this, signals);
        return true;
    }
    const bool verify = Vulkan::g_async_idle_verify && queue;
    if (verify) {
        verify_idle.store(0, std::memory_order_relaxed);
        async_tasks.fetch_add(1, std::memory_order_relaxed);
        scheduler.QueueReplayTask(&VerifyOnReplay, this, signals);
    }
    scheduler.FlushRecorder("gpu_idle_signal");
    // A later guest submission cancels this idle notification. Its own
    // ordered idle task will follow; never signal ahead of queued fences.
    const bool idle = num_submits == 0 && num_commands == 0;
    if (verify) {
        const u32 replayed = verify_idle.load(std::memory_order_acquire);
        const u64 idles = verify_idles.fetch_add(1, std::memory_order_relaxed) + 1;
        if (replayed != (idle ? 2u : 1u)) {
            // The async path decides where the replay got there; B decides after its wake-up. A
            // submission or command between the two is a race either path may lose.
            const u64 differ = verify_differ.fetch_add(1, std::memory_order_relaxed) + 1;
            if (differ <= 32)
                LOG_ERROR(Render_Vulkan,
                          "FAST_CPU_ASYNC_IDLE verify: GPU idle {} at the replay, {} after B's "
                          "drain (num_submits={} num_commands={}), mismatch #{}",
                          replayed == 2 ? "yes" : replayed == 1 ? "no" : "not decided",
                          idle ? "yes" : "no", num_submits.load(), num_commands.load(), differ);
        }
        if ((idles & 1023) == 0)
            ReportAsyncIdle();
    }
    if (idle)
        Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle);
    return true;
}

namespace {
std::atomic<u32> g_async_unwritten_logged{};
// FAST_CPU_ASYNC_IDLE: at a replay task, every guest signal encoded before it must be written
// (its submission, or with FAST_CPU_SIGNAL_AT_REPLAY its own packet, came first). Cheap enough to
// check always; a failure would let the guest see GPU idle or a retired submission early.
bool SignalsWritten(Vulkan::Rasterizer& rasterizer, u64 signals, const char* where) {
    const u64 written = rasterizer.GetScheduler().GuestSignalsCompleted();
    if (written >= signals)
        return true;
    if (g_async_unwritten_logged.fetch_add(1, std::memory_order_relaxed) < 32)
        LOG_ERROR(Render_Vulkan,
                  "FAST_CPU_ASYNC_IDLE: {} replayed with guest signals written={} of {} encoded "
                  "before it",
                  where, written, signals);
    return false;
}
} // namespace

void Liverpool::RetireOnReplay(void* context, u64 signals) {
    auto& self = *static_cast<Liverpool*>(context);
    SignalsWritten(*self.rasterizer, signals, "a submission's retirement");
    {
        std::scoped_lock lock{self.submit_mutex};
        --self.num_submits;
        --self.async_retires;
        self.submit_cv.notify_all();
    }
    self.async_tasks.fetch_sub(1, std::memory_order_release); // the last use of `self`
}

void Liverpool::IdleOnReplay(void* context, u64 signals) {
    auto& self = *static_cast<Liverpool*>(context);
    SignalsWritten(*self.rasterizer, signals, "the GPU-idle task");
    // As the old task decided after its drain: a later guest submission cancels this
    // notification (its own idle task follows).
    if (self.num_submits == 0 && self.num_commands == 0)
        Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle);
    self.async_tasks.fetch_sub(1, std::memory_order_release); // the last use of `self`
}

// FAST_CPU_ASYNC_IDLE_VERIFY: where the async path would act, B waiting in its drain meanwhile.
void Liverpool::VerifyOnReplay(void* context, u64 signals) {
    auto& self = *static_cast<Liverpool*>(context);
    self.verify_checks.fetch_add(1, std::memory_order_relaxed);
    if (!SignalsWritten(*self.rasterizer, signals, "verify"))
        self.verify_unwritten.fetch_add(1, std::memory_order_relaxed);
    self.verify_idle.store(self.num_submits == 0 && self.num_commands == 0 ? 2u : 1u,
                           std::memory_order_release);
    self.async_tasks.fetch_sub(1, std::memory_order_release);
}

void Liverpool::ReportAsyncIdle() {
    LOG_INFO(Render_Vulkan,
             "FAST_CPU_ASYNC_IDLE verify: replay checks={} signals_unwritten={} idle_decisions={} "
             "differing={}",
             verify_checks.load(std::memory_order_relaxed),
             verify_unwritten.load(std::memory_order_relaxed),
             verify_idles.load(std::memory_order_relaxed),
             verify_differ.load(std::memory_order_relaxed));
}

bool Liverpool::SyncGuestWrites(const char* reason) {
    if (!rasterizer)
        return false;
    // B completion publishes the scheduler's producer state, but deferred
    // guest writes may still be queued on R. Make those writes visible before
    // the CPU dereferences command memory; do not submit when none are pending.
    rasterizer->DrainDrawPipe(2);
    auto& scheduler = rasterizer->GetScheduler();
    const bool pending = scheduler.GuestSignalsPending();
    scheduler.FlushGuestSignals(reason);
    return pending;
}

void Liverpool::ProcessCommands() {
    // Process incoming commands with high priority
    while (num_commands) {
        Common::UniqueFunction<void> callback{};
        {
            std::scoped_lock lk{submit_mutex};
            callback = std::move(command_queue.front());
            command_queue.pop();
            --num_commands;
        }
        if (rasterizer)
            rasterizer->DrainDrawPipe(2);
        callback();
    }
}

void Liverpool::Process(std::stop_token stoken) {
    Common::SetCurrentThreadName("shadPS4:GpuCommandProcessor");
    // The command processor is the frame's critical path:
    // keep it ahead of the game's own worker threads and background load
    // (VR_CP_NORMAL_PRIORITY=1: the old normal priority, for A/B tests).
    if (!std::getenv("VR_CP_NORMAL_PRIORITY")) {
        Common::SetCurrentThreadPriority(Common::ThreadPriority::High);
    }
    gpu_id = std::this_thread::get_id();
    FastRender::RegisterThread(FastRender::Role::GpuCommand);

    while (!stoken.stop_requested()) {
        {
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(submit_cv, lk, stoken, [this] {
                return num_commands || num_decode_submits || submit_done;
            });
        }
        if (stoken.stop_requested()) {
            break;
        }

        VideoCore::StartCapture();

        curr_qid = -1;

        while (num_decode_submits || num_commands) {
            ProcessCommands();

            curr_qid = (curr_qid + 1) % num_mapped_queues;

            auto& queue = mapped_queues[curr_qid];

            Task::Handle task{};
            {
                std::scoped_lock lock{queue.m_access};
                if (queue.submits.empty()) {
                    continue;
                }
                task = queue.submits.front();
            }
            task.resume();

            if (task.done()) {
                {
                    std::scoped_lock lock{queue.m_access};
                    queue.submits.pop();
                }
                --num_decode_submits;
                // The coroutine storage and guest-visible completion remain alive until
                // every earlier draw and deferred fence has run on B.
                const auto complete = [](void* context, std::span<const std::byte> bytes) {
                    auto& self = *static_cast<Liverpool*>(context);
                    Task::Handle finished;
                    std::memcpy(&finished, bytes.data(), sizeof(finished));
                    if (self.RetireSubmission(finished))
                        return; // FAST_CPU_ASYNC_IDLE
                    if (self.num_submits == 1)
                        self.SyncRecorder("gpu_idle", true);
                    finished.destroy();
                    std::scoped_lock lock{self.submit_mutex};
                    --self.num_submits;
                    self.submit_cv.notify_all();
                };
                const auto bytes = std::as_bytes(std::span{&task, size_t{1}});
                if (!rasterizer || !rasterizer->QueueOrderedTask(complete, this, bytes, true)) {
                    if (rasterizer)
                        rasterizer->DrainDrawPipe(5);
                    complete(this, bytes);
                }
            }
        }

        if (submit_done.exchange(false)) {
            const auto finish = [](void* context, std::span<const std::byte>) {
                auto& self = *static_cast<Liverpool*>(context);
                VideoCore::EndCapture();
                if (self.rasterizer) {
                    self.rasterizer->OnSubmit();
                    // FAST bit 56: usually empty after the gpu_idle flush; then not submitted.
                    self.rasterizer->FlushFrameEnd();
                }
            };
            if (!rasterizer || !rasterizer->QueueOrderedTask(finish, this, {}, true)) {
                if (rasterizer)
                    rasterizer->DrainDrawPipe(5);
                finish(this, {});
            }
        }

        const auto idle = [](void* context, std::span<const std::byte>) {
            auto& self = *static_cast<Liverpool*>(context);
            if (self.SignalIdle())
                return; // FAST_CPU_ASYNC_IDLE
            self.SyncRecorder("gpu_idle_signal");
            // A later guest submission cancels this idle notification. Its own
            // ordered idle task will follow; never signal ahead of queued fences.
            if (self.num_submits == 0 && self.num_commands == 0)
                Platform::IrqC::Instance()->Signal(Platform::InterruptId::GpuIdle);
        };
        if (!rasterizer || !rasterizer->QueueOrderedTask(idle, this, {}, true)) {
            if (rasterizer)
                rasterizer->DrainDrawPipe(6);
            idle(this, {});
        }
    }
    if (rasterizer)
        rasterizer->DrainDrawPipe(9);
    // FAST_CPU_ASYNC_IDLE: the recorder's replay tasks refer to this object; none may outlive it
    // (B is drained, so this thread owns the scheduler, as SyncRecorder's callers do).
    if (rasterizer && async_tasks.load(std::memory_order_acquire) != 0)
        rasterizer->GetScheduler().FlushRecorder("liverpool_exit");
}

Liverpool::Task Liverpool::ProcessCeUpdate(std::span<const u32> ccb) {
    FIBER_ENTER(ccb_task_name);

    while (!ccb.empty()) {
        ProcessCommands();

        const auto* header = reinterpret_cast<const PM4Header*>(ccb.data());
        const u32 type = header->type;
        if (type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", type);
        }

        const PM4ItOpcode opcode = header->type3.opcode;
        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            // const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::WriteConstRam: {
            const auto* write_const = reinterpret_cast<const PM4WriteConstRam*>(header);
            memcpy(cblock.constants_heap.data() + write_const->Offset(), &write_const->data,
                   write_const->Size());
            break;
        }
        case PM4ItOpcode::DumpConstRam: {
            const auto* dump_const = reinterpret_cast<const PM4DumpConstRam*>(header);
            const bool audit = DA::Enabled() && rasterizer && rasterizer->DrawWorkerActive();
            const auto overlap =
                audit ? DA::Get().Probe({dump_const->Address<VAddr>(), dump_const->Size()},
                                        [&](u64 t) { return rasterizer->IsDrawTaskPending(t); })
                      : DA::Overlap{};
            const bool targeted =
                rasterizer && rasterizer->CeDumpWait(dump_const->Address<VAddr>(), dump_const->Size());
            const bool drained = !targeted && rasterizer && rasterizer->DrainDrawPipe(4);
            if (audit) {
                LOG_INFO(Render_Vulkan,
                         "FAST drain ce: frame={} address={:#x} bytes={} pending_packets={} "
                         "overlapping={} unknown={} drained={}",
                         FastRender::g_counters.frames.load(), dump_const->Address<VAddr>(),
                         dump_const->Size(), overlap.packets, overlap.matching, overlap.unknown,
                         drained);
                DA::Get().Write({{dump_const->Address<VAddr>(), dump_const->Size()}, DA::Kind::Ce});
            }
            // The targeted path must not touch B's scheduler; its guard already excluded
            // dumps that overlap queued GPU writes or deferred signals.
            if (rasterizer && !targeted)
                rasterizer->GetScheduler().FlushGuestSignals("ce_dump_order");
            memcpy(dump_const->Address<void*>(),
                   cblock.constants_heap.data() + dump_const->Offset(), dump_const->Size());
            break;
        }
        case PM4ItOpcode::IncrementCeCounter: {
            ++cblock.ce_count;
            break;
        }
        case PM4ItOpcode::WaitOnDeCounterDiff: {
            const auto diff = it_body[0];
            while ((cblock.de_count - cblock.ce_count) >= diff) {
                YIELD_CE();
            }
            break;
        }
        case PM4ItOpcode::IndirectBufferConst: {
            SyncGuestWrites("ce_indirect_read");
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task =
                ProcessCeUpdate({indirect_buffer->Address<const u32>(), indirect_buffer->ib_size});
            RESUME_CE(task);

            while (!task.handle.done()) {
                YIELD_CE();
                RESUME_CE(task);
            }
            break;
        }
        default:
            const u32 count = header->type3.NumWords();
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), count);
        }
        ccb = NextPacket(ccb, header->type3.NumWords() + 1);
    }

    FIBER_EXIT;
}

void Liverpool::ExecuteFencePacket(std::span<const std::byte> bytes) {
    // The ring owns this packet until its handler returns.
    const auto* header = reinterpret_cast<const PM4Header*>(bytes.data());
    switch (header->type3.opcode) {
    case PM4ItOpcode::EventWriteEos: {
        const auto* event_eos = reinterpret_cast<const PM4CmdEventWriteEos*>(header);
        if (rasterizer) {
            rasterizer->ProcessDownloadImages();
        }
        event_eos->SignalFence([this](void* address, u64 data, u32 num_bytes) {
            FlipOrderTrace::Write("eos", reinterpret_cast<uintptr_t>(address), data, num_bytes,
                                  rasterizer && rasterizer->GetScheduler().RecorderThreaded());
            if (rasterizer && rasterizer->GetScheduler().RecorderThreaded()) {
                rasterizer->GetScheduler().SignalGuest(reinterpret_cast<u64>(address), data,
                                                       num_bytes);
                return;
            }
            auto* memory = Core::Memory::Instance();
            if (!memory->TryWriteBacking(address, &data, num_bytes)) {
                memcpy(address, &data, num_bytes);
            }
            FlipOrderTrace::Write("eos_direct_end", reinterpret_cast<uintptr_t>(address), data,
                                  num_bytes);
        });
        if (event_eos->command == PM4CmdEventWriteEos::Command::GdsStore) {
            ASSERT(event_eos->size == 1);
            if (rasterizer) {
                rasterizer->Finish();
                const u32 value = rasterizer->ReadDataFromGds(event_eos->gds_index);
                *event_eos->Address() = value;
            }
        }
        break;
    }
    case PM4ItOpcode::EventWriteEop: {
        const auto* event_eop = reinterpret_cast<const PM4CmdEventWriteEop*>(header);
        if (rasterizer) {
            rasterizer->ProcessDownloadImages();
        }
        if (rasterizer && rasterizer->GetScheduler().RecorderThreaded()) {
            u64 address{}, value{};
            u32 bytes{}, irq = ~u32{0};
            event_eop->SignalFence(
                [&](void* ptr, u64 data, u32 size) {
                    address = reinterpret_cast<u64>(ptr);
                    value = data;
                    bytes = size;
                },
                [&] { irq = static_cast<u32>(Platform::InterruptId::GfxEop); });
            const auto source = event_eop->data_sel.Value() == DataSelect::GpuClock64
                                    ? Vulkan::GuestSignalValue::GpuClock
                                : event_eop->data_sel.Value() == DataSelect::PerfCounter
                                    ? Vulkan::GuestSignalValue::PerfCounter
                                    : Vulkan::GuestSignalValue::Literal;
            FlipOrderTrace::Write("eop", address, value, bytes, 1);
            rasterizer->GetScheduler().SignalGuest(address, value, bytes, irq, source);
            break;
        }
        event_eop->SignalFence(
            [](void* address, u64 data, u32 num_bytes) {
                FlipOrderTrace::Write("eop_direct_begin", reinterpret_cast<uintptr_t>(address),
                                      data, num_bytes);
                auto* memory = Core::Memory::Instance();
                if (!memory->TryWriteBacking(address, &data, num_bytes)) {
                    memcpy(address, &data, num_bytes);
                }
                FlipOrderTrace::Write("eop_direct_end", reinterpret_cast<uintptr_t>(address), data,
                                      num_bytes);
            },
            [] { Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxEop); });
        break;
    }
    default:
        UNREACHABLE();
    }
}

Liverpool::Task Liverpool::ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb,
                                           std::vector<u32> owned_copy,
                                           std::shared_ptr<ScratchDraw::GraphicsSubmissionScan> scan,
                                           std::shared_ptr<Vulkan::GraphicsSubmission> prep) {
    FIBER_ENTER(dcb_task_name);

    // Bit 31 keeps the one submit-time copy in the preparation (SubmitGfx).
    const std::span<const u32> dcb_copy =
        prep && !Vulkan::SharedCommandCopy(*prep).empty() ? Vulkan::SharedCommandCopy(*prep)
                                                          : std::span<const u32>{owned_copy};
    // Bit 31 (bbport vk_draw_prep.cpp:250-305): no register copy here unless the ordered
    // scanner needs a rebase; prepared draws are looked up by packet offset below.
    if (prep)
        Vulkan::BeginGraphicsSubmission(*prep, regs);

    // Seed from the actual starting registers once per top-level buffer. This
    // diagnostic establishes replay correctness before an ordered scanner is
    // allowed to inherit state across submissions or prepare renderer inputs.
    if (scan && scan_pool) {
        auto seed = std::make_shared<Regs>(regs);
        if (scan_pool->TrySubmit([scan, seed] { scan->Run(*seed); })) ++scan_queued;
        else { ++scan_dropped; scan->finished.store(true, std::memory_order_release); }
    }
    struct CancelScan {
        std::shared_ptr<ScratchDraw::GraphicsSubmissionScan> scan;
        ~CancelScan() { if (scan) scan->cancelled.store(true, std::memory_order_relaxed); }
    } cancel_scan{scan};
    const auto verify_draw = [&](size_t offset) {
        if (prep && rasterizer && FastRender::On(Vulkan::GraphicsPrepBit)) {
            // Incremental: rehashes only the register blocks written since the last draw.
            const auto checksum = register_deltas.Checksum(regs.reg_array, ScratchDraw::HashRegisterWords);
            rasterizer->SetPreparedDraw(Vulkan::NextPreparedDraw(*prep, offset), checksum);
        }
        if (!scan) return;
        const auto checksum = register_deltas.Checksum(regs.reg_array, ScratchDraw::HashRegisterWords);
        switch (scan->Compare(offset, checksum)) {
        case ScratchDraw::GraphicsSubmissionScan::Result::Ready: ++scan_ready; break;
        case ScratchDraw::GraphicsSubmissionScan::Result::Pending: ++scan_pending; break;
        case ScratchDraw::GraphicsSubmissionScan::Result::Unavailable: ++scan_unavailable; break;
        case ScratchDraw::GraphicsSubmissionScan::Result::Stale: ++scan_stale; break;
        }
        const auto total = scan_ready + scan_pending + scan_unavailable + scan_stale;
        if (total % ScanVerifyInterval() == 0) {
            const auto full = RegisterDeltas::FullChecksum(regs.reg_array, ScratchDraw::HashRegisterWords);
            ++scan_hash_checks;
            if (full != checksum) {
                ++scan_mismatches;
                LOG_CRITICAL(Render, "FAST scanner register checksum mismatch #{}", scan_mismatches);
            }
        }
        if (total % 4096 == 0)
            LOG_INFO(Render, "FAST scanner: ready={} pending={} unavailable={} stale={} replay={} hashes={} queued={} dropped={} failures={} mismatches={}",
                     scan_ready, scan_pending, scan_unavailable, scan_stale, scan_replay_checks,
                     scan_hash_checks, scan_queued, scan_dropped, scan_pool->failures.load(), scan_mismatches);
    };
    cblock.Reset();

    // TODO: potentially, ASCs also can depend on CE and in this case the
    // CE task should be moved into more global scope
    Task ce_task{};

    if (!ccb.empty()) {
        // In case of CCB provided kick off CE asap to have the constant heap ready to use
        ce_task = ProcessCeUpdate(ccb);
        RESUME_GFX(ce_task);
    }
    const bool host_markers_enabled = rasterizer && EmulatorSettings.IsVkHostMarkersEnabled();
    const bool guest_markers_enabled = rasterizer && EmulatorSettings.IsVkGuestMarkersEnabled();

    const auto base_addr = reinterpret_cast<uintptr_t>(dcb.data());
    // Malformed packets (see Pm4Strict): `dcb` points into guest memory, or into `dcb_copy` from
    // the offset where the guest's packet was malformed and the copy's was not.
    const u32* const guest_dcb = dcb.data();
    const size_t dcb_dw = dcb.size();
    bool on_copy = false;
    const auto malformed = [&](std::string_view what) {
        const u32* const cur = dcb.data();
        const size_t off = static_cast<size_t>(cur - (on_copy ? dcb_copy.data() : guest_dcb));
        const bool have_copy = !on_copy && dcb_copy.size() == dcb_dw;
        // The copy can take over when it differs within this packet's first dwords (an EOP's
        // body included) and a packet starts at this offset in the copy's own packet chain (so
        // a size the guest's memory got wrong earlier cannot land the CP in the copy's payload).
        const size_t span = std::min<size_t>(8, dcb_dw - off);
        const bool differs =
            have_copy && std::memcmp(cur, dcb_copy.data() + off, span * sizeof(u32)) != 0;
        size_t at = 0;
        while (differs && at < off) {
            const u32 h = dcb_copy[at];
            if ((h >> 30) == 2) {
                at += 1;
            } else if ((h >> 30) == 3) {
                at += ((h >> 16) & 0x3fff) + 2;
            } else {
                break;
            }
        }
        const u32 copy_type = have_copy ? dcb_copy[off] >> 30 : 0;
        const bool resume = differs && at == off && (copy_type == 2 || copy_type == 3);
        const u32 n = ++g_bad_packets;
        if (n <= 8 || n % 100 == 0) {
            size_t changed = 0;
            if (have_copy) {
                for (size_t i = 0; i < dcb_dw; ++i) {
                    changed += guest_dcb[i] != dcb_copy[i] ? 1 : 0;
                }
            }
            LOG_CRITICAL(Render,
                         "PM4: malformed GFX packet #{} ({}, header {:08x}) at +{:#x} of the {} dw "
                         "DCB at {}{}; {} (VR_PM4_STRICT=1: fatal as before)",
                         n, what, *cur, off, dcb_dw, fmt::ptr(guest_dcb),
                         on_copy ? " (already on the submit-time copy)" : "",
                         resume ? "continuing from the submit-time copy"
                                : "skipping the rest of this buffer");
            LOG_CRITICAL(Render, "PM4:   now {}", DwordWindow(cur - off, dcb_dw, off));
            if (have_copy) {
                LOG_CRITICAL(Render, "PM4:   at submit {} ({} of {} dw changed since the submit)",
                             DwordWindow(dcb_copy.data(), dcb_dw, off), changed, dcb_dw);
            }
        }
        if (resume) {
            dcb = std::span<const u32>{dcb_copy.data() + off, dcb_dw - off};
            on_copy = true;
        } else {
            dcb = {};
        }
    };
    while (!dcb.empty()) {
        ProcessCommands();

        const auto* header = reinterpret_cast<const PM4Header*>(dcb.data());
        const u32 type = header->type;

        std::unique_ptr<Regs> replay_expected;
        switch (type) {
        default:
            if (Pm4Strict()) {
                UNREACHABLE_MSG("Wrong PM4 type {}", type);
            }
            malformed(fmt::format("type {}", type));
            continue;
        case 0:
            if (Pm4Strict()) {
                UNREACHABLE_MSG("Unimplemented PM4 type 0, base reg: {}, size: {}",
                                header->type0.base.Value(), header->type0.NumWords());
            }
            malformed("type 0");
            continue;
        case 2:
            // Type-2 packet are used for padding purposes
            dcb = NextPacket(dcb, 1);
            continue;
        case 3:
            const u32 count = header->type3.NumWords();
            const PM4ItOpcode opcode = header->type3.opcode;
            // Only predict an equality wait on this still-pending, explicitly valued fence.
            // Tickets prevent stale values from being reused after a drain and later enqueue.
            bool queued_fence_wait = false;
            if (opcode == PM4ItOpcode::WaitRegMem &&
                FastRender::On(FastRender::OrderedFenceTasks) && rasterizer) {
                const auto* wait = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
                queued_fence_wait = wait->mem_space == PM4CmdWaitRegMem::MemSpace::Memory &&
                                    wait->function == PM4CmdWaitRegMem::Function::Equal &&
                                    wait->Address<VAddr>() == pending_fence.address &&
                                    (pending_fence.value & wait->mask) == wait->ref &&
                                    rasterizer->IsDrawTaskPending(pending_fence.ticket);
            }
            // B1b found these are all unsigned >= 0. No guest value can block this
            // comparison. Commands still execute in order on B; real waits retain drains.
            // Limit this first candidate to the measured stage-pipe path.
            bool trivial_wait = false;
            if (opcode == PM4ItOpcode::WaitRegMem &&
                FastRender::On(FastRender::TrivialWaitElision) && rasterizer &&
                rasterizer->DrawWorkerActive()) {
                const auto* wait = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
                trivial_wait = wait->mem_space == PM4CmdWaitRegMem::MemSpace::Memory &&
                               wait->function == PM4CmdWaitRegMem::Function::GreaterThanEqual &&
                               wait->ref == 0;
            }
            // S2: only register writes and direct draws can overlap B. Everything that
            // may observe/write guest memory, signal a fence or advance VR waits in order.
            switch (opcode) {
            case PM4ItOpcode::SetConfigReg:
            case PM4ItOpcode::SetContextReg:
            case PM4ItOpcode::SetShReg:
            case PM4ItOpcode::SetUconfigReg:
            case PM4ItOpcode::IndexType:
            case PM4ItOpcode::IndexBase:
            case PM4ItOpcode::IndexBufferSize:
            case PM4ItOpcode::NumInstances:
            case PM4ItOpcode::DrawIndex2:
            case PM4ItOpcode::DrawIndexOffset2:
            case PM4ItOpcode::DrawIndexAuto:
            case PM4ItOpcode::DispatchDirect:
                break;
            case PM4ItOpcode::WaitRegMem: {
                const auto* wait = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
                const bool audit = DA::Enabled() && rasterizer && rasterizer->DrawWorkerActive();
                const auto writer = audit && wait->mem_space == PM4CmdWaitRegMem::MemSpace::Memory
                                        ? DA::Get().Last({wait->Address<VAddr>(), 4})
                                        : DA::Writer{};
                const bool writer_pending = audit && rasterizer->IsDrawTaskPending(writer.ticket);
                const bool drained = !queued_fence_wait && !trivial_wait && rasterizer &&
                                     rasterizer->DrainDrawPipe(0, static_cast<u32>(opcode));
                if (audit)
                    LOG_INFO(Render_Vulkan,
                             "FAST drain wait: frame={} address={:#x} ref={:#x} mask={:#x} "
                             "function={} memory={} writer={} writer_pending={} literal={} "
                             "value={:#x} forwarded={} drained={}",
                             FastRender::g_counters.frames.load(), wait->Address<VAddr>(),
                             wait->ref, wait->mask, u32(wait->function.Value()),
                             wait->mem_space == PM4CmdWaitRegMem::MemSpace::Memory,
                             DA::Name(writer.kind), writer_pending, writer.literal, writer.value,
                             queued_fence_wait, drained);
                break;
            }
            case PM4ItOpcode::EventWrite:
            case PM4ItOpcode::WriteData:
            case PM4ItOpcode::EventWriteEop:
            case PM4ItOpcode::EventWriteEos:
            case PM4ItOpcode::AcquireMem:
                if (!FastRender::On(FastRender::OrderedFenceTasks) && rasterizer)
                    rasterizer->DrainDrawPipe(0, static_cast<u32>(opcode));
                break;
            case PM4ItOpcode::Nop: {
                const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
                const bool inert =
                    nop->header.count.Value() == 0 ||
                    (nop->data_block[0] != PM4CmdNop::PayloadType::PatchedFlip &&
                     (!guest_markers_enabled ||
                      (nop->data_block[0] != PM4CmdNop::PayloadType::DebugMarkerPush &&
                       nop->data_block[0] != PM4CmdNop::PayloadType::DebugColorMarkerPush &&
                       nop->data_block[0] != PM4CmdNop::PayloadType::DebugMarkerPop)));
                if (!(FastRender::On(FastRender::SkipPureStateDrain) && inert) && rasterizer)
                    rasterizer->DrainDrawPipe(0, static_cast<u32>(opcode));
                break;
            }
            case PM4ItOpcode::IncrementDeCounter:
            case PM4ItOpcode::WaitOnCeCounter:
                // These only advance/yield the A-owned CE state machine. Draw packets
                // already own user/flat data and fetch V#s. A real CE memory dump
                // still enforces its own read-before-write boundary in ProcessCeUpdate.
                if (!FastRender::On(FastRender::SkipPureStateDrain) && rasterizer)
                    rasterizer->DrainDrawPipe(0, static_cast<u32>(opcode));
                break;
            case PM4ItOpcode::SetBase:
            case PM4ItOpcode::ContextControl:
            case PM4ItOpcode::ClearState:
                // ClearState marks all blocks (or a reset) below. SetBase only changes A's
                // indirect address, captured when an indirect draw is built.
                if (!FastRender::On(FastRender::SkipPureStateDrain) && rasterizer)
                    rasterizer->DrainDrawPipe(0, static_cast<u32>(opcode));
                break;
            case PM4ItOpcode::DmaData:
                if (FastRender::On(FastRender::SkipNoopDmaDrain) &&
                    reinterpret_cast<const PM4DmaData*>(header)->dst_addr_lo == 0x3022C) {
                    break; // The command implementation below already does nothing for this target.
                }
                if (!FastRender::On(FastRender::OrderedFenceTasks) && rasterizer)
                    rasterizer->DrainDrawPipe(0, static_cast<u32>(opcode));
                break;
            default:
                if (rasterizer)
                    rasterizer->DrainDrawPipe(0, static_cast<u32>(opcode));
                break;
            }
            if (scan && ++scan_packets % ScanVerifyInterval() == 0) {
                replay_expected = std::make_unique<Regs>(regs);
                size_t words;
                if (!ScratchDraw::ReplayGraphicsPacket(*replay_expected, dcb, words)) replay_expected.reset();
            }
            switch (opcode) {
            case PM4ItOpcode::Nop: {
                const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
                if (nop->header.count.Value() == 0) {
                    break;
                }

                switch (nop->data_block[0]) {
                case PM4CmdNop::PayloadType::PatchedFlip: {
                    // There is no evidence that GPU CP drives flip events by parsing
                    // special NOP packets. For convenience lets assume that it does.
                    SyncRecorder("flip_handoff");
                    const auto flip = Libraries::VideoOut::DecodeFlip(
                        std::span{nop->data_block + 1, size_t(nop->header.count.Value())});
                    ASSERT_MSG(flip.has_value(), "Invalid patched flip identity");
                    if (!flip) break;
                    static const bool trace_flip = std::getenv("VR_FLIP_ORDER_TRACE") != nullptr;
                    if (trace_flip) {
                        const u32 buffer = flip->buffer;
                        if (buffer < vo_port->buffer_labels.size())
                            FlipOrderTrace::Event("packet",
                                reinterpret_cast<uintptr_t>(&vo_port->buffer_labels[buffer]),
                                vo_port->buffer_labels[buffer], 8, flip->ticket);
                        LOG_INFO(Render, "Flip order packet: buffer={} ticket={}", buffer,
                                 flip->ticket);
                    }
                    // Retain IRQ observers; the flip itself belongs to this packet, not a FIFO.
                    Platform::IrqC::Instance()->Signal(Platform::InterruptId::GfxFlip);
                    Libraries::VideoOut::ExecuteEopFlip(*flip);
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPush: {
                    if (guest_markers_enabled) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        rasterizer->ScopeMarkerBegin(label, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugColorMarkerPush: {
                    if (guest_markers_enabled) {
                        const auto marker_sz = nop->header.count.Value() * 2;
                        const std::string_view label{
                            reinterpret_cast<const char*>(&nop->data_block[1]), marker_sz};
                        const u32 color = *reinterpret_cast<const u32*>(
                            reinterpret_cast<const u8*>(&nop->data_block[1]) + marker_sz);
                        rasterizer->ScopedMarkerInsertColor(label, color, true);
                    }
                    break;
                }
                case PM4CmdNop::PayloadType::DebugMarkerPop: {
                    if (guest_markers_enabled) {
                        rasterizer->ScopeMarkerEnd(true);
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::ContextControl: {
                break;
            }
            case PM4ItOpcode::ClearState: {
                regs.SetDefaults();
                // Bit 31 hashes the registers per draw: adopt the defaults' precomputed
                // block checksums instead of rehashing all of them (bbport regs_reset).
                const RegisterDeltas::ResetChecksums* defaults = nullptr;
                if (FastRender::On(Vulkan::GraphicsPrepBit)) {
                    static const auto precomputed = [] {
                        const auto state = std::make_unique<Regs>();
                        state->SetDefaults();
                        return RegisterDeltas::MakeReset(state->reg_array,
                                                         ScratchDraw::HashRegisterWords);
                    }();
                    defaults = precomputed.get();
                }
                // Bit 30: the next packet carries a reset flag and the consumer applies the
                // same SetDefaults (bbport regs.h:203, vk_rasterizer.cpp:1318), then deltas.
                if (FastRender::On(ClearStateReset)) {
                    register_deltas.Reset(defaults);
                    ++clear_state_resets;
                } else if (defaults) {
                    register_deltas.MarkAll(*defaults);
                } else {
                    register_deltas.MarkAll();
                }
                if (++clear_states % 4096 == 0) {
                    const u64 frames = FastRender::g_counters.frames.load(std::memory_order_relaxed);
                    const u64 window = frames - clear_state_log_frames;
                    LOG_INFO(Render,
                             "FAST clearstate: clear_states={} resets={} full={} frames={} "
                             "per_frame={:.1f}",
                             clear_states, clear_state_resets, clear_states - clear_state_resets,
                             frames,
                             window ? double(clear_states - clear_state_log_states) / double(window)
                                    : 0.0);
                    clear_state_log_states = clear_states;
                    clear_state_log_frames = frames;
                }
                break;
            }
            case PM4ItOpcode::SetConfigReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ConfigRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);
                std::memcpy(&regs.reg_array[reg_addr], payload, (count - 1) * sizeof(u32));
                register_deltas.Mark(reg_addr, count - 1);
                break;
            }
            case PM4ItOpcode::SetContextReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto reg_addr = Regs::ContextRegWordOffset + set_data->reg_offset;
                const auto* payload = reinterpret_cast<const u32*>(header + 2);

                std::memcpy(&regs.reg_array[reg_addr], payload, (count - 1) * sizeof(u32));
                register_deltas.Mark(reg_addr, count - 1);

                // In the case of HW, render target memory has alignment as color block operates on
                // tiles. There is no information of actual resource extents stored in CB context
                // regs, so any deduction of it from slices/pitch will lead to a larger surface
                // created. The same applies to the depth targets. Fortunately, the guest always
                // sends a trailing NOP packet right after the context regs setup, so we can use the
                // heuristic below and extract the hint to determine actual resource dims.

                switch (reg_addr) {
                case ContextRegs::CbColor0Base:
                case ContextRegs::CbColor1Base:
                case ContextRegs::CbColor2Base:
                case ContextRegs::CbColor3Base:
                case ContextRegs::CbColor4Base:
                case ContextRegs::CbColor5Base:
                case ContextRegs::CbColor6Base:
                case ContextRegs::CbColor7Base: {
                    const auto col_buf_id = (reg_addr - ContextRegs::CbColor0Base) /
                                            (ContextRegs::CbColor1Base - ContextRegs::CbColor0Base);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x0e || nop_offset == 0x0d || nop_offset == 0x0b) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    } else {
                        last_cb_extent[col_buf_id].raw = 0;
                    }
                    break;
                }
                case ContextRegs::CbColor0Cmask:
                case ContextRegs::CbColor1Cmask:
                case ContextRegs::CbColor2Cmask:
                case ContextRegs::CbColor3Cmask:
                case ContextRegs::CbColor4Cmask:
                case ContextRegs::CbColor5Cmask:
                case ContextRegs::CbColor6Cmask:
                case ContextRegs::CbColor7Cmask: {
                    const auto col_buf_id =
                        (reg_addr - ContextRegs::CbColor0Cmask) /
                        (ContextRegs::CbColor1Cmask - ContextRegs::CbColor0Cmask);
                    ASSERT(col_buf_id < NUM_COLOR_BUFFERS);

                    const auto nop_offset = header->type3.count;
                    if (nop_offset == 0x04) {
                        ASSERT_MSG(payload[nop_offset] == 0xc0001000,
                                   "NOP hint is missing in CB setup sequence");
                        last_cb_extent[col_buf_id].raw = payload[nop_offset + 1];
                    }
                    break;
                }
                case ContextRegs::DbZInfo: {
                    if (header->type3.count == 8) {
                        ASSERT_MSG(payload[20] == 0xc0001000,
                                   "NOP hint is missing in DB setup sequence");
                        last_db_extent.raw = payload[21];
                    } else {
                        last_db_extent.raw = 0;
                    }
                    break;
                }
                default:
                    break;
                }
                break;
            }
            case PM4ItOpcode::SetShReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                const auto set_size = (count - 1) * sizeof(u32);

                if (set_data->reg_offset >= 0x200 &&
                    set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                    ASSERT(set_size <= sizeof(ComputeProgram));
                    auto* addr = reinterpret_cast<u32*>(&mapped_queues[GfxQueueId].cs_state) +
                                 (set_data->reg_offset - 0x200);
                    std::memcpy(addr, header + 2, set_size);
                } else {
                    std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset],
                                header + 2, set_size);
                    register_deltas.Mark(Regs::ShRegWordOffset + set_data->reg_offset,
                                         set_size / sizeof(u32));
                }
                break;
            }
            case PM4ItOpcode::SetUconfigReg: {
                const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
                std::memcpy(&regs.reg_array[Regs::UconfigRegWordOffset + set_data->reg_offset],
                            header + 2, (count - 1) * sizeof(u32));
                register_deltas.Mark(Regs::UconfigRegWordOffset + set_data->reg_offset, count - 1);
                break;
            }
            case PM4ItOpcode::SetPredication: {
                LOG_WARNING(Render, "Unimplemented IT_SET_PREDICATION");
                break;
            }
            case PM4ItOpcode::IndexType: {
                const auto* index_type = reinterpret_cast<const PM4CmdDrawIndexType*>(header);
                regs.index_buffer_type.raw = index_type->raw;
                MarkRegisterWrite(regs.index_buffer_type);
                break;
            }
            case PM4ItOpcode::DrawIndex2: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndex2*>(header);
                regs.max_index_size = draw_index->max_size;
                MarkRegisterWrite(regs.max_index_size);
                regs.index_base_address.base_addr_lo = draw_index->index_base_lo;
                MarkRegisterWrite(regs.index_base_address);
                regs.index_base_address.base_addr_hi = draw_index->index_base_hi;
                MarkRegisterWrite(regs.index_base_address);
                regs.num_indices = draw_index->index_count;
                MarkRegisterWrite(regs.num_indices);
                regs.draw_initiator = draw_index->draw_initiator;
                MarkRegisterWrite(regs.draw_initiator);
                verify_draw(static_cast<size_t>(dcb.data() - (on_copy ? dcb_copy.data() : guest_dcb)));
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(fmt::format("gfx:{}:DrawIndex2", cmd_address));
                        rasterizer->Draw(true);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->Draw(true);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndexOffset2: {
                const auto* draw_index_off =
                    reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
                regs.max_index_size = draw_index_off->max_size;
                MarkRegisterWrite(regs.max_index_size);
                regs.num_indices = draw_index_off->index_count;
                MarkRegisterWrite(regs.num_indices);
                regs.draw_initiator = draw_index_off->draw_initiator;
                MarkRegisterWrite(regs.draw_initiator);
                verify_draw(static_cast<size_t>(dcb.data() - (on_copy ? dcb_copy.data() : guest_dcb)));
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(
                            fmt::format("gfx:{}:DrawIndexOffset2", cmd_address));
                        rasterizer->Draw(true, draw_index_off->index_offset);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->Draw(true, draw_index_off->index_offset);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndexAuto: {
                const auto* draw_index = reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
                regs.num_indices = draw_index->index_count;
                MarkRegisterWrite(regs.num_indices);
                regs.draw_initiator = draw_index->draw_initiator;
                MarkRegisterWrite(regs.draw_initiator);
                verify_draw(static_cast<size_t>(dcb.data() - (on_copy ? dcb_copy.data() : guest_dcb)));
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(
                            fmt::format("gfx:{}:DrawIndexAuto", cmd_address));
                        rasterizer->Draw(false);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->Draw(false);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndirect: {
                const auto* draw_indirect = reinterpret_cast<const PM4CmdDrawIndirect*>(header);
                const auto offset = draw_indirect->data_offset;
                const auto stride = sizeof(DrawIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(
                            fmt::format("gfx:{}:DrawIndirect", cmd_address));
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset, stride, 1, 0);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset, stride, 1, 0);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndirectMulti: {
                const auto* draw_indirect =
                    reinterpret_cast<const PM4CmdDrawIndirectMulti*>(header);
                const auto offset = draw_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(
                            fmt::format("gfx:{}:DrawIndirectMulti", cmd_address));
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset,
                                                 draw_indirect->stride, draw_indirect->count, 0);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DrawIndirect(false, indirect_args_addr, offset,
                                                 draw_indirect->stride, draw_indirect->count, 0);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndexIndirect: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirect*>(header);
                const auto offset = draw_index_indirect->data_offset;
                const auto stride = sizeof(DrawIndexedIndirectArgs);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(
                            fmt::format("gfx:{}:DrawIndexIndirect", cmd_address));
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset, stride, 1, 0);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset, stride, 1, 0);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(
                            fmt::format("gfx:{}:DrawIndexIndirectMulti", cmd_address));
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset,
                                                 draw_index_indirect->stride,
                                                 draw_index_indirect->count, 0);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset,
                                                 draw_index_indirect->stride,
                                                 draw_index_indirect->count, 0);
                    }
                }
                break;
            }
            case PM4ItOpcode::DrawIndexIndirectCountMulti: {
                const auto* draw_index_indirect =
                    reinterpret_cast<const PM4CmdDrawIndexIndirectCountMulti*>(header);
                const auto offset = draw_index_indirect->data_offset;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDump(base_addr, reinterpret_cast<uintptr_t>(header), regs);
                }
                if (rasterizer) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(
                            fmt::format("gfx:{}:DrawIndexIndirectCountMulti", cmd_address));
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset,
                                                 draw_index_indirect->stride,
                                                 draw_index_indirect->count,
                                                 draw_index_indirect->count_indirect_enable.Value()
                                                     ? draw_index_indirect->count_addr
                                                     : 0);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DrawIndirect(true, indirect_args_addr, offset,
                                                 draw_index_indirect->stride,
                                                 draw_index_indirect->count,
                                                 draw_index_indirect->count_indirect_enable.Value()
                                                     ? draw_index_indirect->count_addr
                                                     : 0);
                    }
                }
                break;
            }
            case PM4ItOpcode::DispatchDirect: {
                const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
                auto& cs_program = GetCsRegs();
                cs_program.dim_x = dispatch_direct->dim_x;
                cs_program.dim_y = dispatch_direct->dim_y;
                cs_program.dim_z = dispatch_direct->dim_z;
                cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (rasterizer && (cs_program.dispatch_initiator & 1)) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(
                            fmt::format("gfx:{}:DispatchDirect", cmd_address));
                        rasterizer->DispatchDirect();
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DispatchDirect();
                    }
                }
                break;
            }
            case PM4ItOpcode::DispatchIndirect: {
                const auto* dispatch_indirect =
                    reinterpret_cast<const PM4CmdDispatchIndirect*>(header);
                auto& cs_program = GetCsRegs();
                const auto offset = dispatch_indirect->data_offset;
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                if (DebugState.DumpingCurrentReg()) {
                    DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                                   cs_program);
                }
                if (rasterizer && (cs_program.dispatch_initiator & 1)) {
                    const auto cmd_address = reinterpret_cast<const void*>(header);
                    if (host_markers_enabled) {
                        rasterizer->ScopeMarkerBegin(
                            fmt::format("gfx:{}:DispatchIndirect", cmd_address));
                        rasterizer->DispatchIndirect(indirect_args_addr, offset, size);
                        rasterizer->ScopeMarkerEnd();
                    } else {
                        rasterizer->DispatchIndirect(indirect_args_addr, offset, size);
                    }
                }
                break;
            }
            case PM4ItOpcode::NumInstances: {
                const auto* num_instances = reinterpret_cast<const PM4CmdDrawNumInstances*>(header);
                regs.num_instances.num_instances = num_instances->num_instances;
                MarkRegisterWrite(regs.num_instances);
                break;
            }
            case PM4ItOpcode::IndexBase: {
                const auto* index_base = reinterpret_cast<const PM4CmdDrawIndexBase*>(header);
                regs.index_base_address.base_addr_lo = index_base->addr_lo;
                MarkRegisterWrite(regs.index_base_address);
                regs.index_base_address.base_addr_hi = index_base->addr_hi;
                MarkRegisterWrite(regs.index_base_address);
                break;
            }
            case PM4ItOpcode::IndexBufferSize: {
                const auto* index_size = reinterpret_cast<const PM4CmdDrawIndexBufferSize*>(header);
                regs.num_indices = index_size->num_indices;
                MarkRegisterWrite(regs.num_indices);
                break;
            }
            case PM4ItOpcode::SetBase: {
                const auto* set_base = reinterpret_cast<const PM4CmdSetBase*>(header);
                ASSERT(set_base->base_index == PM4CmdSetBase::BaseIndex::DrawIndexIndirPatchTable);
                indirect_args_addr = set_base->Address<u64>();
                break;
            }
            case PM4ItOpcode::EventWrite: {
                const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
                LOG_DEBUG(Render, "Encountered EventWrite: event_type = {}, event_index = {}",
                          magic_enum::enum_name(event->event_type.Value()),
                          magic_enum::enum_name(event->event_index.Value()));
                if (event->event_type.Value() == EventType::SoVgtStreamoutFlush) {
                    // TODO: handle proper synchronization, for now signal that update is done
                    // immediately
                    regs.cp_strmout_cntl.offset_update_done = 1;
                    MarkRegisterWrite(regs.cp_strmout_cntl);
                } else if (event->event_index.Value() == EventIndex::ZpassDone) {
                    if (event->event_type.Value() == EventType::PixelPipeStatDump) {
                        static constexpr u64 OcclusionCounterValidMask = 0x8000000000000000ULL;
                        static constexpr u64 OcclusionCounterStep = 0x2FFFFFFULL;
                        // The synthetic counter belongs to A; the memory side effect belongs
                        // to B. Snapshot the value/count so later packets cannot change this task.
                        struct QueryWrite {
                            VAddr address;
                            u64 value;
                            u32 pairs;
                        };
                        const QueryWrite query{event->Address<VAddr>(),
                                               pixel_counter | OcclusionCounterValidMask,
                                               static_cast<u32>(num_counter_pairs)};
                        pixel_counter += OcclusionCounterStep;
                        const auto write = [](void* context, std::span<const std::byte> bytes) {
                            auto& self = *static_cast<Liverpool*>(context);
                            if (self.rasterizer)
                                self.rasterizer->GetScheduler().FlushGuestSignals("zpass_order");
                            QueryWrite query;
                            std::memcpy(&query, bytes.data(), sizeof(query));
                            auto* results = reinterpret_cast<u64*>(query.address);
                            for (u32 i = 0; i < query.pairs; ++i, results += 2)
                                *results = query.value;
                        };
                        pending_fence = {};
                        const auto bytes = std::as_bytes(std::span{&query, 1});
                        const auto ticket = rasterizer ? rasterizer->QueueOrderedTask(write, this, bytes, true) : 0;
                        if (rasterizer)
                            rasterizer->GuardConstantWrite(query.address, u64(query.pairs) * 16, ticket);
                        if (!ticket) {
                            if (rasterizer)
                                rasterizer->DrainDrawPipe(0, static_cast<u32>(opcode));
                            write(this, bytes);
                        }
                    }
                }
                break;
            }
            case PM4ItOpcode::EventWriteEos:
            case PM4ItOpcode::EventWriteEop: {
                if (opcode == PM4ItOpcode::EventWriteEop && !Pm4Strict() &&
                    !EopSelectsValid(reinterpret_cast<const PM4CmdEventWriteEop*>(header)))
                    [[unlikely]] {
                    malformed("EOP with invalid selects");
                    continue;
                }
                const auto bytes = std::as_bytes(
                    std::span{reinterpret_cast<const u32*>(header), size_t(count + 1)});
                const u64 queued =
                    rasterizer
                        ? rasterizer->QueueOrderedTask(
                              [](void* context, std::span<const std::byte> packet) {
                                  static_cast<Liverpool*>(context)->ExecuteFencePacket(packet);
                              },
                              this, bytes, true)
                        : 0;
                if (rasterizer) {
                    if (opcode == PM4ItOpcode::EventWriteEop) {
                        const auto* e = reinterpret_cast<const PM4CmdEventWriteEop*>(header);
                        if (e->data_sel != DataSelect::None)
                            rasterizer->GuardDeferredConstantWrite(reinterpret_cast<VAddr>(e->Address<u32>()),
                                e->data_sel == DataSelect::Data32Low ? 4u : 8u);
                    } else {
                        const auto* e = reinterpret_cast<const PM4CmdEventWriteEos*>(header);
                        rasterizer->GuardDeferredConstantWrite(e->Address<VAddr>(),
                            e->command == PM4CmdEventWriteEos::Command::SignalFence ? 4u : e->size.Value() * 4u);
                    }
                }
                if (DA::Enabled()) {
                    if (opcode == PM4ItOpcode::EventWriteEop) {
                        const auto* e = reinterpret_cast<const PM4CmdEventWriteEop*>(header);
                        if (e->data_sel != DataSelect::None)
                            DA::Get().Write({{reinterpret_cast<VAddr>(e->Address<u32>()),
                                              e->data_sel == DataSelect::Data32Low ? 4u : 8u},
                                             DA::Kind::Eop,
                                             queued,
                                             e->DataQWord(),
                                             e->data_sel == DataSelect::Data32Low ||
                                                 e->data_sel == DataSelect::Data64});
                    } else {
                        const auto* e = reinterpret_cast<const PM4CmdEventWriteEos*>(header);
                        DA::Get().Write({{e->Address<VAddr>(),
                                          e->command == PM4CmdEventWriteEos::Command::SignalFence
                                              ? 4u
                                              : e->size.Value() * 4u},
                                         DA::Kind::Eos,
                                         queued,
                                         e->data,
                                         e->command == PM4CmdEventWriteEos::Command::SignalFence});
                    }
                }
                pending_fence = {};
                if (queued && opcode == PM4ItOpcode::EventWriteEop) {
                    const auto* event = reinterpret_cast<const PM4CmdEventWriteEop*>(header);
                    if (event->data_sel == DataSelect::Data32Low ||
                        event->data_sel == DataSelect::Data64)
                        pending_fence = {queued, reinterpret_cast<VAddr>(event->Address<u32>()),
                                         event->data_lo};
                } else if (queued) {
                    const auto* event = reinterpret_cast<const PM4CmdEventWriteEos*>(header);
                    if (event->command == PM4CmdEventWriteEos::Command::SignalFence)
                        pending_fence = {queued, event->Address<VAddr>(), event->data};
                }
                if (!queued) {
                    if (rasterizer)
                        rasterizer->DrainDrawPipe(0, static_cast<u32>(opcode));
                    ExecuteFencePacket(bytes);
                }
                break;
            }
            case PM4ItOpcode::DmaData: {
                const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
                if (dma_data->dst_addr_lo == 0x3022C || !rasterizer)
                    break;
                const auto bytes = std::as_bytes(
                    std::span{reinterpret_cast<const u32*>(header), size_t(count + 1)});
                const auto execute = [](void* context, std::span<const std::byte> packet) {
                    auto& self = *static_cast<Liverpool*>(context);
                    self.rasterizer->GetScheduler().InvalidateGuestWait();
                    const auto* dma_data = reinterpret_cast<const PM4DmaData*>(packet.data());
                    if (dma_data->src_sel == DmaDataSrc::Data &&
                        dma_data->dst_sel == DmaDataDst::Gds) {
                        self.rasterizer->FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(),
                                                    dma_data->data, true);
                    } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                                dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                               dma_data->dst_sel == DmaDataDst::Gds) {
                        self.rasterizer->CopyBuffer(dma_data->dst_addr_lo,
                                                    dma_data->SrcAddress<VAddr>(),
                                                    dma_data->NumBytes(), true, false);
                    } else if (dma_data->src_sel == DmaDataSrc::Data &&
                               (dma_data->dst_sel == DmaDataDst::Memory ||
                                dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                        self.rasterizer->FillBuffer(dma_data->DstAddress<VAddr>(),
                                                    dma_data->NumBytes(), dma_data->data, false);
                    } else if (dma_data->src_sel == DmaDataSrc::Gds &&
                               (dma_data->dst_sel == DmaDataDst::Memory ||
                                dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                        self.rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(),
                                                    dma_data->src_addr_lo, dma_data->NumBytes(),
                                                    false, true);
                    } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                                dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                               (dma_data->dst_sel == DmaDataDst::Memory ||
                                dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                        self.rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(),
                                                    dma_data->SrcAddress<VAddr>(),
                                                    dma_data->NumBytes(), false, false);
                    } else {
                        UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}",
                                        u32(dma_data->src_sel.Value()),
                                        u32(dma_data->dst_sel.Value()));
                    }
                };
                pending_fence = {};
                const auto dma_ticket = rasterizer->QueueOrderedTask(execute, this, bytes, true);
                if (dma_data->dst_sel == DmaDataDst::Memory || dma_data->dst_sel == DmaDataDst::MemoryUsingL2)
                    rasterizer->GuardConstantWrite(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(), dma_ticket);
                if (DA::Enabled() && (dma_data->dst_sel == DmaDataDst::Memory ||
                                      dma_data->dst_sel == DmaDataDst::MemoryUsingL2))
                    DA::Get().Write({{dma_data->DstAddress<VAddr>(), dma_data->NumBytes()},
                                     DA::Kind::Dma,
                                     dma_ticket,
                                     dma_data->data,
                                     dma_data->src_sel == DmaDataSrc::Data});
                if (DA::Enabled() && dma_ticket &&
                    (dma_data->src_sel == DmaDataSrc::Memory ||
                     dma_data->src_sel == DmaDataSrc::MemoryUsingL2)) {
                    const DA::Range source{dma_data->SrcAddress<VAddr>(), dma_data->NumBytes()};
                    DA::Get().Reads(dma_ticket, std::span{&source, 1}, false,
                                    [&](u64 t) { return rasterizer->IsDrawTaskPending(t); });
                }
                if (!dma_ticket) {
                    rasterizer->DrainDrawPipe(0, static_cast<u32>(opcode));
                    execute(this, bytes);
                }
                break;
            }
            case PM4ItOpcode::WriteData: {
                // Own the complete variable-size payload, not the guest command-buffer pointer.
                // Memory-reading PM4 (CondExec, indirect buffers, Rewind, unpredicted waits)
                // retains its pre-opcode drain; PfpSyncMe remains the PFP/ME visibility boundary.
                const auto bytes = std::as_bytes(
                    std::span{reinterpret_cast<const u32*>(header), size_t(count + 1)});
                const auto write = [](void* context, std::span<const std::byte> packet) {
                    auto& self = *static_cast<Liverpool*>(context);
                    if (self.rasterizer)
                        self.rasterizer->GetScheduler().FlushGuestSignals("guest_write_order");
                    const auto* cmd = reinterpret_cast<const PM4CmdWriteData*>(packet.data());
                    ASSERT(cmd->dst_sel.Value() == 2 || cmd->dst_sel.Value() == 5);
                    ASSERT(!cmd->wr_one_addr.Value());
                    const u32 data_size = (cmd->header.count.Value() - 2) * sizeof(u32);
                    FlipOrderTrace::Write("write_data_begin",
                                          reinterpret_cast<uintptr_t>(cmd->Address<void*>()),
                                          data_size >= 4 ? cmd->data[0] : 0, data_size);
                    std::memcpy(cmd->Address<void*>(), cmd->data, data_size);
                    FlipOrderTrace::Write("write_data_end",
                                          reinterpret_cast<uintptr_t>(cmd->Address<void*>()),
                                          data_size >= 4 ? cmd->data[0] : 0, data_size);
                };
                // A later write may replace a predicted fence's value.
                pending_fence = {};
                const auto write_ticket =
                    rasterizer ? rasterizer->QueueOrderedTask(write, this, bytes, true) : 0;
                if (rasterizer) {
                    const auto* cmd = reinterpret_cast<const PM4CmdWriteData*>(header);
                    rasterizer->GuardConstantWrite(reinterpret_cast<VAddr>(cmd->Address<void*>()),
                        (cmd->header.count.Value() - 2) * sizeof(u32), write_ticket);
                }
                if (DA::Enabled()) {
                    const auto* cmd = reinterpret_cast<const PM4CmdWriteData*>(header);
                    DA::Get().Write({{reinterpret_cast<VAddr>(cmd->Address<void*>()),
                                      (cmd->header.count.Value() - 2) * sizeof(u32)},
                                     DA::Kind::WriteData,
                                     write_ticket,
                                     cmd->Size() ? cmd->data[0] : 0,
                                     true});
                }
                if (!write_ticket) {
                    if (rasterizer)
                        rasterizer->DrainDrawPipe(0, static_cast<u32>(opcode));
                    write(this, bytes);
                }
                break;
            }
            case PM4ItOpcode::CopyData: {
                const auto* copy_data = reinterpret_cast<const PM4CmdCopyData*>(header);
                LOG_WARNING(Render,
                            "unhandled IT_COPY_DATA src_sel = {}, dst_sel = {}, "
                            "count_sel = {}, wr_confirm = {}, engine_sel = {}",
                            u32(copy_data->src_sel.Value()), u32(copy_data->dst_sel.Value()),
                            copy_data->count_sel.Value(), copy_data->wr_confirm.Value(),
                            u32(copy_data->engine_sel.Value()));
                break;
            }
            case PM4ItOpcode::MemSemaphore: {
                const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
                if (mem_semaphore->IsSignaling()) {
                    if (rasterizer && rasterizer->GetScheduler().RecorderThreaded()) {
                        auto& scheduler = rasterizer->GetScheduler();
                        u64 value = 1;
                        if (mem_semaphore->signal_type ==
                            PM4CmdMemSemaphore::SignalType::Increment) {
                            scheduler.FlushGuestSignals("memory_semaphore_increment");
                            // A tracked guest read may fault/download through Liverpool.
                            // Perform it on the producer, never while it waits for replay.
                            value = *mem_semaphore->Address<u64*>() + 1;
                        }
                        rasterizer->GuardDeferredConstantWrite(mem_semaphore->Address<u64>(), 8);
                    scheduler.SignalGuest(mem_semaphore->Address<u64>(), value, 8);
                    } else
                        mem_semaphore->Signal();
                } else {
                    if (rasterizer)
                        rasterizer->GetScheduler().FlushGuestSignals("memory_semaphore_order");

                    if (!mem_semaphore->Signaled())
                        SyncRecorder("memory_semaphore_wait", true);
                    while (!mem_semaphore->Signaled()) {
                        YIELD_GFX();
                        SyncGuestWrites("graphics_semaphore_resume");
                    }
                    mem_semaphore->Decrement();
                }
                break;
            }
            case PM4ItOpcode::AcquireMem: {
                // const auto* acquire_mem = reinterpret_cast<PM4CmdAcquireMem*>(header);
                break;
            }
            case PM4ItOpcode::Rewind: {
                SyncGuestWrites("rewind_read");
                if (!rasterizer) {
                    break;
                }
                // On the submit-time copy the valid bit (and what follows it, written after the
                // submit) is in guest memory: wait there and continue from there.
                const u32* const at = on_copy ? guest_dcb + (dcb.data() - dcb_copy.data())
                                              : reinterpret_cast<const u32*>(header);
                const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(at);
                if (!rewind->Valid())
                    SyncRecorder("rewind_wait", true);
                while (!rewind->Valid()) {
                    YIELD_GFX();
                    SyncGuestWrites("graphics_rewind_resume");
                }
                if (on_copy) {
                    dcb = std::span<const u32>{at, dcb_dw - static_cast<size_t>(at - guest_dcb)};
                    on_copy = false;
                }
                break;
            }
            case PM4ItOpcode::WaitRegMem: {
                if (trivial_wait) {
                    FastRender::Bump(FastRender::g_counters.trivial_waits);
                    break;
                }
                if (queued_fence_wait) {
                    FastRender::Bump(FastRender::g_counters.queued_fence_waits);
                    break;
                }
                const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
                if (rasterizer && wait_reg_mem->mem_space == PM4CmdWaitRegMem::MemSpace::Memory &&
                    wait_reg_mem->function == PM4CmdWaitRegMem::Function::Equal &&
                    rasterizer->GetScheduler().ForwardGuestWait(
                        wait_reg_mem->Address<u64>(), wait_reg_mem->mask, wait_reg_mem->ref)) {
                    FastRender::g_counters.forwarded_fence_waits.fetch_add(
                        1, std::memory_order_relaxed);
                    break;
                }
                if (rasterizer)
                    rasterizer->GetScheduler().FlushGuestSignals("guest_wait_order");
                // ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
                // Optimization: VO label waits are special because the emulator
                // will write to the label when presentation is finished. So if
                // there are no other submits to yield to we can sleep the thread
                // instead and allow other tasks to run.
                if (!wait_reg_mem->Test(regs.reg_array))
                    SyncRecorder("wait_reg_mem", true);
                const u64* wait_addr = wait_reg_mem->Address<u64*>();
                if (vo_port->IsVoLabel(wait_addr) &&
                    num_submits == mapped_queues[GfxQueueId].submits.size()) {
                    vo_port->WaitVoLabel([&] { return wait_reg_mem->Test(regs.reg_array); });
                    break;
                }
                while (!wait_reg_mem->Test(regs.reg_array)) {
                    YIELD_GFX();
                    // Other queues may have encoded new deferred signals while
                    // this coroutine was suspended. B completion alone does not
                    // publish their values: submit/replay before testing again.
                    const bool pending = SyncGuestWrites("graphics_wait_resume");
                    if (pending && std::getenv("FAST_WAIT_TRACE"))
                        LOG_INFO(
                            Render,
                            "FAST wait resume: address={:#x} ref={:#x} mask={:#x} satisfied={}",
                            wait_reg_mem->Address<u64>(), wait_reg_mem->ref, wait_reg_mem->mask,
                            wait_reg_mem->Test(regs.reg_array));
                }
                break;
            }
            case PM4ItOpcode::IndirectBuffer: {
                SyncGuestWrites("graphics_indirect_read");
                const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
                auto task = ProcessGraphics(
                    {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, {});
                RESUME_GFX(task);

                while (!task.handle.done()) {
                    YIELD_GFX();
                    RESUME_GFX(task);
                }
                break;
            }
            case PM4ItOpcode::IncrementDeCounter: {
                ++cblock.de_count;
                break;
            }
            case PM4ItOpcode::WaitOnCeCounter: {
                while (cblock.ce_count <= cblock.de_count && !ce_task.handle.done()) {
                    RESUME_GFX(ce_task);
                }
                break;
            }
            case PM4ItOpcode::PfpSyncMe: {
                if (rasterizer) {
                    rasterizer->CpSync();
                }
                break;
            }
            case PM4ItOpcode::StrmoutBufferUpdate: {
                const auto* strmout = reinterpret_cast<const PM4CmdStrmoutBufferUpdate*>(header);
                LOG_WARNING(Render_Vulkan,
                            "Unimplemented IT_STRMOUT_BUFFER_UPDATE, update_memory = {}, "
                            "source_select = {}, buffer_select = {}",
                            strmout->update_memory.Value(),
                            magic_enum::enum_name(strmout->source_select.Value()),
                            strmout->buffer_select.Value());
                break;
            }
            case PM4ItOpcode::GetLodStats: {
                LOG_WARNING(Render_Vulkan, "Unimplemented IT_GET_LOD_STATS");
                break;
            }
            case PM4ItOpcode::CondExec: {
                SyncGuestWrites("conditional_read");
                const auto* cond_exec = reinterpret_cast<const PM4CmdCondExec*>(header);
                if (cond_exec->command.Value() != 0) {
                    LOG_WARNING(Render, "IT_COND_EXEC used a reserved command");
                }
                const auto skip = *cond_exec->Address() == false;
                if (skip) {
                    dcb = NextPacket(dcb,
                                     header->type3.NumWords() + 1 + cond_exec->exec_count.Value());
                    continue;
                }
                break;
            }
            default:
                if (Pm4Strict()) {
                    UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                                    static_cast<u32>(opcode), count);
                }
                malformed(fmt::format("unknown type-3 opcode {:#x}, count {}",
                                      static_cast<u32>(opcode), count));
                continue;
            }
            if (replay_expected) {
                ++scan_replay_checks;
                if (replay_expected->reg_array != regs.reg_array) {
                    ++scan_mismatches;
                    LOG_CRITICAL(Render, "FAST scanner packet replay mismatch #{} opcode={:#x}",
                                 scan_mismatches, static_cast<u32>(opcode));
                }
            }
            dcb = NextPacket(dcb, header->type3.NumWords() + 1);
            break;
        }
    }

    // Bit 31: the end state must equal the scanner's, or it rebases at the next buffer.
    if (prep)
        Vulkan::EndGraphicsSubmission(
            *prep, FastRender::On(Vulkan::GraphicsPrepBit)
                       ? std::optional<u64>{register_deltas.Checksum(regs.reg_array,
                                                                     ScratchDraw::HashRegisterWords)}
                       : std::nullopt);

    if (ce_task.handle) {
        while (!ce_task.handle.done()) {
            RESUME_GFX(ce_task);
        }
        ce_task.handle.destroy();
    }

    FIBER_EXIT;
}

template <bool is_indirect>
Liverpool::Task Liverpool::ProcessCompute(std::span<const u32> acb, u32 vqid) {
    if (rasterizer)
        rasterizer->DrainDrawPipe(3);
    FIBER_ENTER(acb_task_name[vqid]);
    auto& queue = asc_queues[{vqid}];
    const bool host_markers_enabled = rasterizer && EmulatorSettings.IsVkHostMarkersEnabled();

    struct IndirectPatch {
        const PM4Header* header;
        VAddr indirect_addr;
    };
    boost::container::small_vector<IndirectPatch, 4> indirect_patches;

    auto base_addr = reinterpret_cast<VAddr>(acb.data());
    size_t acb_size = acb.size_bytes();
    while (!acb.empty()) {
        ProcessCommands();

        auto* header = reinterpret_cast<const PM4Header*>(acb.data());
        u32 next_dw_off = header->type3.NumWords() + 1;

        // If we have a buffered packet, use it.
        if (queue.tmp_dwords > 0) [[unlikely]] {
            header = reinterpret_cast<const PM4Header*>(queue.tmp_packet.data());
            next_dw_off = header->type3.NumWords() + 1 - queue.tmp_dwords;
            std::memcpy(queue.tmp_packet.data() + queue.tmp_dwords, acb.data(),
                        next_dw_off * sizeof(u32));
            queue.tmp_dwords = 0;
        }

        // If the packet is split across ring boundary, buffer until next submission
        if (next_dw_off > acb.size()) [[unlikely]] {
            std::memcpy(queue.tmp_packet.data(), acb.data(), acb.size_bytes());
            queue.tmp_dwords = acb.size();
            if constexpr (!is_indirect) {
                *queue.read_addr += acb.size();
                *queue.read_addr %= queue.ring_size_dw;
            }
            break;
        }

        if (header->type == 2) {
            // Type-2 packet are used for padding purposes
            next_dw_off = 1;
            acb = NextPacket(acb, next_dw_off);
            if constexpr (!is_indirect) {
                *queue.read_addr += next_dw_off;
                *queue.read_addr %= queue.ring_size_dw;
            }
            continue;
        }

        if (header->type != 3) {
            // No other types of packets were spotted so far
            UNREACHABLE_MSG("Invalid PM4 type {}", header->type.Value());
        }

        const PM4ItOpcode opcode = header->type3.opcode;

        const auto* it_body = reinterpret_cast<const u32*>(header) + 1;
        if (rasterizer)
            rasterizer->DrainDrawPipe(3, static_cast<u32>(opcode));
        switch (opcode) {
        case PM4ItOpcode::Nop: {
            const auto* nop = reinterpret_cast<const PM4CmdNop*>(header);
            break;
        }
        case PM4ItOpcode::IndirectBuffer: {
            SyncGuestWrites("compute_indirect_read");
            const auto* indirect_buffer = reinterpret_cast<const PM4CmdIndirectBuffer*>(header);
            auto task = ProcessCompute<true>(
                {indirect_buffer->Address<const u32>(), indirect_buffer->ib_size}, vqid);
            RESUME_ASC(task, vqid);

            while (!task.handle.done()) {
                YIELD_ASC(vqid);
                RESUME_ASC(task, vqid);
            }
            break;
        }
        case PM4ItOpcode::DmaData: {
            const auto* dma_data = reinterpret_cast<const PM4DmaData*>(header);
            if (rasterizer)
                rasterizer->GetScheduler().InvalidateGuestWait();
            if (dma_data->dst_addr_lo == 0x3022C || !rasterizer) {
                break;
            }
            if (dma_data->src_sel == DmaDataSrc::Data && dma_data->dst_sel == DmaDataDst::Gds) {
                rasterizer->FillBuffer(dma_data->dst_addr_lo, dma_data->NumBytes(), dma_data->data,
                                       true);
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       dma_data->dst_sel == DmaDataDst::Gds) {
                rasterizer->CopyBuffer(dma_data->dst_addr_lo, dma_data->SrcAddress<VAddr>(),
                                       dma_data->NumBytes(), true, false);
            } else if (dma_data->src_sel == DmaDataSrc::Data &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                rasterizer->FillBuffer(dma_data->DstAddress<VAddr>(), dma_data->NumBytes(),
                                       dma_data->data, false);
            } else if (dma_data->src_sel == DmaDataSrc::Gds &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                rasterizer->CopyBuffer(dma_data->DstAddress<VAddr>(), dma_data->src_addr_lo,
                                       dma_data->NumBytes(), false, true);
            } else if ((dma_data->src_sel == DmaDataSrc::Memory ||
                        dma_data->src_sel == DmaDataSrc::MemoryUsingL2) &&
                       (dma_data->dst_sel == DmaDataDst::Memory ||
                        dma_data->dst_sel == DmaDataDst::MemoryUsingL2)) {
                const u32 num_bytes = dma_data->NumBytes();
                const VAddr src_addr = dma_data->SrcAddress<VAddr>();
                const VAddr dst_addr = dma_data->DstAddress<VAddr>();
                const PM4Header* header =
                    reinterpret_cast<const PM4Header*>(dst_addr - sizeof(PM4Header));
                if (dst_addr >= base_addr && dst_addr < base_addr + acb_size &&
                    num_bytes == sizeof(PM4CmdDispatchIndirect::GroupDimensions) &&
                    header->type == 3 && header->type3.opcode == PM4ItOpcode::DispatchDirect) {
                    indirect_patches.emplace_back(header, src_addr);
                } else {
                    rasterizer->CopyBuffer(dst_addr, src_addr, num_bytes, false, false);
                }
            } else {
                UNREACHABLE_MSG("WriteData src_sel = {}, dst_sel = {}",
                                u32(dma_data->src_sel.Value()), u32(dma_data->dst_sel.Value()));
            }
            break;
        }
        case PM4ItOpcode::AcquireMem: {
            break;
        }
        case PM4ItOpcode::Rewind: {
            SyncGuestWrites("compute_rewind_read");
            if (!rasterizer) {
                break;
            }
            const PM4CmdRewind* rewind = reinterpret_cast<const PM4CmdRewind*>(header);
            while (!rewind->Valid()) {
                YIELD_ASC(vqid);
                SyncGuestWrites("compute_rewind_resume");
            }
            break;
        }
        case PM4ItOpcode::SetShReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetData*>(header);
            const auto set_size = (header->type3.NumWords() - 1) * sizeof(u32);

            if (set_data->reg_offset >= 0x200 &&
                set_data->reg_offset <= (0x200 + sizeof(ComputeProgram) / 4)) {
                ASSERT(set_size <= sizeof(ComputeProgram));
                auto* addr = reinterpret_cast<u32*>(&mapped_queues[vqid + 1].cs_state) +
                             (set_data->reg_offset - 0x200);
                std::memcpy(addr, header + 2, set_size);
            } else {
                std::memcpy(&regs.reg_array[Regs::ShRegWordOffset + set_data->reg_offset],
                            header + 2, set_size);
                register_deltas.Mark(Regs::ShRegWordOffset + set_data->reg_offset,
                                     set_size / sizeof(u32));
            }
            break;
        }
        case PM4ItOpcode::SetQueueReg: {
            const auto* set_data = reinterpret_cast<const PM4CmdSetQueueReg*>(header);
            LOG_WARNING(Render, "Encountered compute SetQueueReg: vqid = {}, reg_offset = {:#x}",
                        set_data->vqid.Value(), set_data->reg_offset.Value());
            break;
        }
        case PM4ItOpcode::DispatchDirect: {
            const auto* dispatch_direct = reinterpret_cast<const PM4CmdDispatchDirect*>(header);
            if (auto it = std::ranges::find(indirect_patches, header, &IndirectPatch::header);
                it != indirect_patches.end()) {
                const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
                rasterizer->DispatchIndirect(it->indirect_addr, 0, size);
                break;
            }
            auto& cs_program = GetCsRegs();
            cs_program.dim_x = dispatch_direct->dim_x;
            cs_program.dim_y = dispatch_direct->dim_y;
            cs_program.dim_z = dispatch_direct->dim_z;
            cs_program.dispatch_initiator = dispatch_direct->dispatch_initiator;
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (rasterizer && (cs_program.dispatch_initiator & 1)) {
                const auto cmd_address = reinterpret_cast<const void*>(header);
                if (host_markers_enabled) {
                    rasterizer->ScopeMarkerBegin(
                        fmt::format("asc[{}]:{}:DispatchDirect", vqid, cmd_address));
                    rasterizer->DispatchDirect();
                    rasterizer->ScopeMarkerEnd();
                } else {
                    rasterizer->DispatchDirect();
                }
            }
            break;
        }
        case PM4ItOpcode::DispatchIndirect: {
            const auto* dispatch_indirect =
                reinterpret_cast<const PM4CmdDispatchIndirectMec*>(header);
            auto& cs_program = GetCsRegs();
            const auto ib_address = dispatch_indirect->Address<VAddr>();
            const auto size = sizeof(PM4CmdDispatchIndirect::GroupDimensions);
            if (DebugState.DumpingCurrentReg()) {
                DebugState.PushRegsDumpCompute(base_addr, reinterpret_cast<uintptr_t>(header),
                                               cs_program);
            }
            if (rasterizer && (cs_program.dispatch_initiator & 1)) {
                const auto cmd_address = reinterpret_cast<const void*>(header);
                if (host_markers_enabled) {
                    rasterizer->ScopeMarkerBegin(
                        fmt::format("asc[{}]:{}:DispatchIndirect", vqid, cmd_address));
                    rasterizer->DispatchIndirect(ib_address, 0, size);
                    rasterizer->ScopeMarkerEnd();
                } else {
                    rasterizer->DispatchIndirect(ib_address, 0, size);
                }
            }
            break;
        }
        case PM4ItOpcode::WriteData: {
            const auto* write_data = reinterpret_cast<const PM4CmdWriteData*>(header);
            if (rasterizer)
                rasterizer->GetScheduler().FlushGuestSignals("guest_write_order");
            ASSERT(write_data->dst_sel.Value() == 2 || write_data->dst_sel.Value() == 5);
            const u32 data_size = (header->type3.count.Value() - 2) * 4;
            if (!write_data->wr_one_addr.Value()) {
                FlipOrderTrace::Write("compute_write_begin",
                                      reinterpret_cast<uintptr_t>(write_data->Address<void*>()),
                                      data_size >= 4 ? write_data->data[0] : 0, data_size);
                std::memcpy(write_data->Address<void*>(), write_data->data, data_size);
                FlipOrderTrace::Write("compute_write_end",
                                      reinterpret_cast<uintptr_t>(write_data->Address<void*>()),
                                      data_size >= 4 ? write_data->data[0] : 0, data_size);
            } else {
                UNREACHABLE();
            }
            break;
        }
        case PM4ItOpcode::MemSemaphore: {
            const auto* mem_semaphore = reinterpret_cast<const PM4CmdMemSemaphore*>(header);
            if (mem_semaphore->IsSignaling()) {
                if (rasterizer && rasterizer->GetScheduler().RecorderThreaded()) {
                    auto& scheduler = rasterizer->GetScheduler();
                    u64 value = 1;
                    if (mem_semaphore->signal_type == PM4CmdMemSemaphore::SignalType::Increment) {
                        scheduler.FlushGuestSignals("memory_semaphore_increment");
                        // A tracked guest read may fault/download through Liverpool.
                        // Perform it on the producer, never while it waits for replay.
                        value = *mem_semaphore->Address<u64*>() + 1;
                    }
                    rasterizer->GuardDeferredConstantWrite(mem_semaphore->Address<u64>(), 8);
                    scheduler.SignalGuest(mem_semaphore->Address<u64>(), value, 8);
                } else
                    mem_semaphore->Signal();
            } else {
                if (rasterizer)
                    rasterizer->GetScheduler().FlushGuestSignals("memory_semaphore_order");

                if (!mem_semaphore->Signaled())
                    SyncRecorder("memory_semaphore_wait", true);
                while (!mem_semaphore->Signaled()) {
                    YIELD_ASC(vqid);
                    SyncGuestWrites("compute_semaphore_resume");
                }
                mem_semaphore->Decrement();
            }
            break;
        }
        case PM4ItOpcode::WaitRegMem: {
            const auto* wait_reg_mem = reinterpret_cast<const PM4CmdWaitRegMem*>(header);
            if (rasterizer && wait_reg_mem->mem_space == PM4CmdWaitRegMem::MemSpace::Memory &&
                wait_reg_mem->function == PM4CmdWaitRegMem::Function::Equal &&
                rasterizer->GetScheduler().ForwardGuestWait(
                    wait_reg_mem->Address<u64>(), wait_reg_mem->mask, wait_reg_mem->ref)) {
                FastRender::g_counters.forwarded_fence_waits.fetch_add(1,
                                                                       std::memory_order_relaxed);
                break;
            }
            if (rasterizer)
                rasterizer->GetScheduler().FlushGuestSignals("guest_wait_order");
            ASSERT(wait_reg_mem->engine.Value() == PM4CmdWaitRegMem::Engine::Me);
            if (!wait_reg_mem->Test(regs.reg_array))
                SyncRecorder("wait_reg_mem", true);
            while (!wait_reg_mem->Test(regs.reg_array)) {
                YIELD_ASC(vqid);
                SyncGuestWrites("compute_wait_resume");
            }
            break;
        }
        case PM4ItOpcode::ReleaseMem: {
            const auto* release_mem = reinterpret_cast<const PM4CmdReleaseMem*>(header);
            if (DA::Enabled() && release_mem->data_sel != DataSelect::None)
                DA::Get().Write({{release_mem->Address<VAddr>(),
                                  release_mem->data_sel == DataSelect::Data32Low ? 4u : 8u},
                                 DA::Kind::Release,
                                 0,
                                 release_mem->DataQWord(),
                                 release_mem->data_sel == DataSelect::Data32Low ||
                                     release_mem->data_sel == DataSelect::Data64});
            if (rasterizer) {
                rasterizer->ProcessDownloadImages();
            }
            if (rasterizer && rasterizer->GetScheduler().RecorderThreaded()) {
                u64 address{}, value{};
                u32 bytes{}, irq = ~u32{0};
                release_mem->SignalFence(
                    [&](void* ptr, u64 data, u32 size) {
                        address = reinterpret_cast<u64>(ptr);
                        value = data;
                        bytes = size;
                    },
                    [&] { irq = queue.pipe_id; },
                    [this](VAddr dst, u16 index, u16 count) {
                        rasterizer->CopyBuffer(dst, index, count * sizeof(u32), false, true);
                        rasterizer->Finish(); // GDS download remains synchronous
                    });
                const auto source = release_mem->data_sel.Value() == DataSelect::GpuClock64
                                        ? Vulkan::GuestSignalValue::GpuClock
                                    : release_mem->data_sel.Value() == DataSelect::PerfCounter
                                        ? Vulkan::GuestSignalValue::PerfCounter
                                        : Vulkan::GuestSignalValue::Literal;
                FlipOrderTrace::Write("release_mem", address, value, bytes, 1);
                rasterizer->GuardDeferredConstantWrite(address, bytes);
                rasterizer->GetScheduler().SignalGuest(address, value, bytes, irq, source);
                break;
            }
            release_mem->SignalFence(
                [](void* address, u64 data, u32 bytes) {
                    FlipOrderTrace::Write("release_mem_direct_begin",
                                          reinterpret_cast<uintptr_t>(address), data, bytes);
                    if (!Core::Memory::Instance()->TryWriteBacking(address, &data, bytes))
                        std::memcpy(address, &data, bytes);
                    FlipOrderTrace::Write("release_mem_direct_end",
                                          reinterpret_cast<uintptr_t>(address), data, bytes);
                },
                [pipe_id = queue.pipe_id] {
                    Platform::IrqC::Instance()->Signal(static_cast<Platform::InterruptId>(pipe_id));
                },
                [this](VAddr dst, u16 gds_index, u16 num_dwords) {
                    rasterizer->CopyBuffer(dst, gds_index, num_dwords * sizeof(u32), false, true);
                });
            break;
        }
        case PM4ItOpcode::EventWrite: {
            // const auto* event = reinterpret_cast<const PM4CmdEventWrite*>(header);
            break;
        }
        default:
            UNREACHABLE_MSG("Unknown PM4 type 3 opcode {:#x} with count {}",
                            static_cast<u32>(opcode), header->type3.NumWords());
        }

        acb = NextPacket(acb, next_dw_off);

        if constexpr (!is_indirect) {
            *queue.read_addr += next_dw_off;
            *queue.read_addr %= queue.ring_size_dw;
        }
    }

    FIBER_EXIT;
}

Liverpool::CmdBuffer Liverpool::CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];
    ASSERT_MSG(queue.dcb_buffer.capacity() >= queue.dcb_buffer_offset + dcb.size(),
               "dcb copy buffer out of reserved space");
    ASSERT_MSG(queue.ccb_buffer.capacity() >= queue.ccb_buffer_offset + ccb.size(),
               "ccb copy buffer out of reserved space");

    queue.dcb_buffer.resize(
        std::max(queue.dcb_buffer.size(), queue.dcb_buffer_offset + dcb.size()));
    queue.ccb_buffer.resize(
        std::max(queue.ccb_buffer.size(), queue.ccb_buffer_offset + ccb.size()));

    const u32 prev_dcb_buffer_offset = queue.dcb_buffer_offset;
    const u32 prev_ccb_buffer_offset = queue.ccb_buffer_offset;
    if (!dcb.empty()) {
        std::memcpy(queue.dcb_buffer.data() + queue.dcb_buffer_offset, dcb.data(),
                    dcb.size_bytes());
        queue.dcb_buffer_offset += dcb.size();
        dcb = std::span<const u32>{queue.dcb_buffer.begin() + prev_dcb_buffer_offset,
                                   queue.dcb_buffer.begin() + queue.dcb_buffer_offset};
    }

    if (!ccb.empty()) {
        std::memcpy(queue.ccb_buffer.data() + queue.ccb_buffer_offset, ccb.data(),
                    ccb.size_bytes());
        queue.ccb_buffer_offset += ccb.size();
        ccb = std::span<const u32>{queue.ccb_buffer.begin() + prev_ccb_buffer_offset,
                                   queue.ccb_buffer.begin() + queue.ccb_buffer_offset};
    }

    return std::make_pair(dcb, ccb);
}

void Liverpool::SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb) {
    auto& queue = mapped_queues[GfxQueueId];

    std::vector<u32> dcb_copy{};
    if (EmulatorSettings.IsCopyGpuBuffers()) {
        std::tie(dcb, ccb) = CopyCmdBuffers(dcb, ccb);
    } else if (!Pm4Strict()) {
        // The DCB as submitted: only read if a packet in guest memory turns out malformed.
        TestCorrupt(dcb, true);
        dcb_copy.assign(dcb.begin(), dcb.end());
        TestCorrupt(dcb, false);
    }

    auto scan = ScanVerifyInterval()
        ? ScratchDraw::GraphicsSubmissionScan::Build(dcb_copy.empty() ? dcb : std::span<const u32>(dcb_copy))
        : nullptr;
    // Bit 31: one copy per submit on this thread, taken over from dcb_copy when there is one.
    std::shared_ptr<Vulkan::GraphicsSubmission> prep;
    if (rasterizer && FastRender::On(Vulkan::GraphicsPrepBit))
        prep = rasterizer->GetPipelineCache().BuildGraphicsSubmission(dcb, dcb_copy);
    auto task = ProcessGraphics(dcb, ccb, std::move(dcb_copy), std::move(scan), prep);
    {
        std::scoped_lock lock{queue.m_access};
        // Under the queue lock: preparation order is queue order (BBV liverpool.cpp:2724-2733).
        if (prep)
            Vulkan::EnqueueGraphicsSubmission(prep, gfx_submits);
        ++gfx_submits;
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    ++num_submits;
    ++num_decode_submits;
    submit_cv.notify_one();
}

void Liverpool::SubmitAsc(u32 gnm_vqid, std::span<const u32> acb) {
    ASSERT_MSG(gnm_vqid > 0 && gnm_vqid < NumTotalQueues, "Invalid virtual ASC queue index");
    auto& queue = mapped_queues[gnm_vqid];

    const auto vqid = gnm_vqid - 1;
    const auto& task = ProcessCompute(acb, vqid);
    {
        std::scoped_lock lock{queue.m_access};
        queue.submits.emplace(task.handle);
    }

    std::scoped_lock lk{submit_mutex};
    num_mapped_queues = std::max(num_mapped_queues, gnm_vqid + 1);
    ++num_submits;
    ++num_decode_submits;
    submit_cv.notify_one();
}

} // namespace AmdGpu
