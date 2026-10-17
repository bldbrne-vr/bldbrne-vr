// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// The OpenXR runtime's own queue (Instance::HasOwnXrQueue; VR_XR_OWN_QUEUE, vr_xr_queue_policy.h)
// and the ordering between it and the graphics queue (the game's, the presenter's copies, the late
// warp). Header-only; one per OpenXRPresenter. With the shared queue (Own() false) every call here
// does nothing and the presenter runs exactly as before.
//
// Two queues of one family need no ownership transfer, but nothing orders their work: the
// presenter adds the semaphore waits a single queue's submission order gave for free.
//  - Before the runtime reads an image the graphics queue wrote (xrReleaseSwapchainImage and the
//    xrEndFrame after it), the XR queue waits for that write: RuntimeWaitsFor (the writer's
//    timeline at its tick: the game's and the present thread's master semaphores),
//    RuntimeWaitsForGame (everything submitted on the graphics queue so far), or for the late warp
//    a signal in the warp's own batch (PlanGameBatch, GameBatchSubmitted).
//    The runtime submits its own work after it, on the XR queue, so it is ordered after the write.
//  - Before the graphics queue writes an image the runtime handed back, it waits for everything the
//    runtime submitted on the XR queue up to that image's acquire: SignalRuntime after
//    xrAcquireSwapchainImage (one signal for the images acquired together: AcquireBatch),
//    GameWaitsForRuntime before the first write (the late warp: a wait in its own batch,
//    PlanGameBatch). Usually the host has already seen the XR queue past it, and nothing waits.
// Every wait is for a value whose signal was submitted before it (host order), so the two queues
// never wait for each other in a circle.
//
// Locks: Mutex() guards the XR queue (the runtime's four queue calls, xrCreateSwapchain and
// xrDestroySwapchain, our submits on it, and vkDeviceWaitIdle with the other queues' locks). It is
// taken after Scheduler::submit_mutex and the presenter's xr_mutex, or with them in one std::lock.
// After it only the desktop present queue's lock (a leaf) is taken, and, by the frame thread while
// it also holds xr_mutex, the submit lock around the late warp: nobody waits for the XR queue's
// lock or xr_mutex while holding the submit lock alone, so neither order can close a circle. The
// frame thread keeps only the XR queue's lock across xrEndFrame; the GPU thread waits for it
// (holding the other two) only when it makes or remakes swapchains; the present thread takes all
// three at once. Outside the presenter, every vkDeviceWaitIdle takes it (Instance::GetXrQueueMutex)
// in one std::lock with the others it takes (the desktop swapchain's rebuilds in vk_presenter.cpp
// and Swapchain::SetHDR, the instance's teardown), except the imgui texture's under the submit
// lock alone: with the XR queue that one waits for the graphics queue only
// (imgui_impl_vulkan.cpp UploadTextureData::Destroy; the XR queue's lock there could deadlock).

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>

#include <fmt/format.h>

#include "common/logging/log.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vr_device_lost.h"
#include "video_core/renderer_vulkan/vr_xr_queue_policy.h"

namespace Vulkan {

class VrXrQueue {
public:
    VrXrQueue() = default;
    ~VrXrQueue() {
        Shutdown();
    }
    VrXrQueue(const VrXrQueue&) = delete;
    VrXrQueue& operator=(const VrXrQueue&) = delete;

    // `own`: the runtime was given `xr_queue` (else it shares `game_queue`, and nothing here
    // does anything). `xr_mutex` guards `xr_queue` (Instance::GetXrQueueMutex).
    bool Init(vk::Device dev, vk::Queue game, vk::Queue xr, std::mutex& xr_mutex, bool own_queue) {
        Shutdown();
        device = dev;
        game_queue = game;
        xr_queue = xr;
        mutex = &xr_mutex;
        own = false;
        if (!own_queue || !xr || xr == game) {
            return !own_queue;
        }
        if (!CreateTimeline(runtime_timeline) || !CreateTimeline(game_timeline)) {
            Shutdown();
            return false;
        }
        own = true;
        return true;
    }

    void Shutdown() {
        for (vk::Semaphore* s : {&runtime_timeline, &game_timeline}) {
            if (*s && device) {
                device.destroySemaphore(*s);
            }
            *s = vk::Semaphore{};
        }
        own = false;
    }

    [[nodiscard]] bool Own() const {
        return own;
    }
    // The queue the runtime was given.
    [[nodiscard]] vk::Queue Queue() const {
        return own ? xr_queue : game_queue;
    }
    // The XR queue's lock (own queue only).
    [[nodiscard]] std::mutex& Mutex() const {
        return *mutex;
    }
    // This thread holds Mutex() (Lock, or Held after the caller's own std::lock).
    [[nodiscard]] static bool HeldHere() {
        return held_here != 0;
    }

    // Takes the XR queue's lock unless the runtime shares the graphics queue (whose lock, as
    // before, the caller holds) or this thread holds it already.
    class Lock {
    public:
        explicit Lock(const VrXrQueue& queue) {
            if (queue.Own() && held_here == 0) {
                lock = std::unique_lock{queue.Mutex()};
                held_here = 1;
            }
        }
        ~Lock() {
            if (lock.owns_lock()) {
                held_here = 0;
            }
        }
        Lock(const Lock&) = delete;
        Lock& operator=(const Lock&) = delete;

    private:
        std::unique_lock<std::mutex> lock;
    };

    // The caller took Mutex() itself (with other locks, in one std::lock): marked as held by this
    // thread while this lives, so Lock inside does not take it again.
    class Held {
    public:
        explicit Held(bool held) : on{held && held_here == 0} {
            if (on) {
                held_here = 1;
            }
        }
        ~Held() {
            if (on) {
                held_here = 0;
            }
        }
        Held(const Held&) = delete;
        Held& operator=(const Held&) = delete;

    private:
        bool on;
    };

    // The XR queue's lock held, right after xrAcquireSwapchainImage: the XR queue signals its
    // timeline after everything the runtime submitted on it so far. The value for
    // GameWaitsForRuntime (0: shared queue, or the signal failed).
    u64 SignalRuntime() {
        if (!own || failed) {
            return 0;
        }
        const u64 value = runtime_submitted.load(std::memory_order_relaxed) + 1;
        if (!Signal(xr_queue, runtime_timeline, value, "the XR queue's signal")) {
            return 0;
        }
        runtime_submitted.store(value, std::memory_order_release);
        stats.runtime_signals.fetch_add(1, std::memory_order_relaxed);
        return value;
    }

    // The XR queue's lock (and the caller's xr_mutex, which guards the values) held for its whole
    // life: the images acquired while it lives (Add: where each one's SignalRuntime value goes)
    // get one signal after the last of them, when it ends (or when it is full), not one each. No
    // reader of a value runs before it is written: they hold xr_mutex.
    class AcquireBatch {
    public:
        explicit AcquireBatch(VrXrQueue& queue) : xrq{queue} {}
        ~AcquireBatch() {
            Flush();
        }
        AcquireBatch(const AcquireBatch&) = delete;
        AcquireBatch& operator=(const AcquireBatch&) = delete;

        void Add(u64& acquire_value) {
            batch.Add(acquire_value, [this] { return xrq.SignalRuntime(); });
        }
        void Flush() {
            batch.Flush([this] { return xrq.SignalRuntime(); });
        }

    private:
        VrXrQueue& xrq;
        VrXrQueuePolicy::SignalBatch<8> batch{};
    };

    // An image acquired ahead for its writer (OpenXRPresenter::PreAcquire): `ready` the runtime was
    // already done with it (its zero-timeout xrWaitSwapchainImage succeeded); else its writer, the
    // GPU thread, waits for it (up to 20 ms) with the game's submit lock held. Counted for the
    // VR perf line.
    void NotePreAcquire(bool ready) {
        stats.pre_acquired.fetch_add(1, std::memory_order_relaxed);
        if (!ready) {
            stats.pre_not_ready.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Scheduler::submit_mutex held, before the graphics queue's first write into an image acquired
    // at `value` (SignalRuntime): the graphics queue waits for the XR queue's work up to it, unless
    // the host already saw the XR queue past it.
    void GameWaitsForRuntime(u64 value) {
        if (!own || failed) {
            return;
        }
        if (PlanGameWait(value) == VrXrQueuePolicy::Wait::Queue &&
            WaitFor(game_queue, runtime_timeline, value, "the graphics queue's wait")) {
            game_waited = value;
            stats.game_waits.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // A graphics-queue batch's own wait and signal (PlanGameBatch): no semaphore, nothing.
    struct BatchSync {
        vk::Semaphore wait{}; // the batch waits for it at wait_value (all commands)
        u64 wait_value{0};
        vk::Semaphore signal{}; // the batch signals it at signal_value
        u64 signal_value{0};
    };

    // Scheduler::submit_mutex and the XR queue's lock held, before the graphics queue's batch that
    // writes an image acquired at `acquired` (SignalRuntime) and is the last write before the
    // runtime reads it (the late warp's): what that batch waits for and signals itself, in place
    // of GameWaitsForRuntime's and RuntimeWaitsForGame's empty batches. GameBatchSubmitted after
    // the submit (both locks still held).
    BatchSync PlanGameBatch(u64 acquired) {
        BatchSync sync{};
        if (!own || failed) {
            return sync;
        }
        if (PlanGameWait(acquired) == VrXrQueuePolicy::Wait::Queue) {
            sync.wait = runtime_timeline;
            sync.wait_value = acquired;
        }
        sync.signal = game_timeline;
        sync.signal_value = game_signalled + 1;
        return sync;
    }

    // The batch planned by PlanGameBatch was submitted: its wait is recorded, and the XR queue
    // waits for its signal (the runtime's work on the image follows the write). Not called when
    // the submit failed: nothing changes then.
    void GameBatchSubmitted(const BatchSync& sync) {
        if (!own || failed) {
            return;
        }
        if (sync.wait) {
            game_waited = std::max(game_waited, sync.wait_value);
            stats.game_waits.fetch_add(1, std::memory_order_relaxed);
        }
        if (sync.signal) {
            game_signalled = sync.signal_value;
            RuntimeWaitsFor(game_timeline, sync.signal_value);
        }
    }

    // The XR queue's lock held, before xrReleaseSwapchainImage of an image the graphics queue
    // wrote in a submission signalling `semaphore` (a timeline) at `value`: the XR queue waits for
    // it (once per value).
    void RuntimeWaitsFor(vk::Semaphore semaphore, u64 value) {
        if (!own || failed || !semaphore || value == 0) {
            return;
        }
        if (!runtime_waited.Need(Key(semaphore), value)) {
            stats.runtime_skipped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        if (WaitFor(xr_queue, semaphore, value, "the XR queue's wait")) {
            stats.runtime_waits.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // Scheduler::submit_mutex and the XR queue's lock held, before xrReleaseSwapchainImage of an
    // image written by work submitted on the graphics queue with no timeline of its own (the
    // present thread's and an adopted pair's without one): the graphics queue signals ours after
    // it, the XR queue waits for that. (The late warp signals in its own batch: PlanGameBatch.)
    void RuntimeWaitsForGame() {
        if (!own || failed) {
            return;
        }
        const u64 value = game_signalled + 1;
        if (!Signal(game_queue, game_timeline, value, "the graphics queue's signal")) {
            return;
        }
        game_signalled = value;
        RuntimeWaitsFor(game_timeline, value);
    }

    // For the 5 s VR perf line: the waits and signals made (and left out) since the last call, the
    // empty batches among them (each one vkQueueSubmit), and the images acquired ahead that were
    // not ready (NotePreAcquire: the GPU thread waited for those holding the game's submit lock).
    [[nodiscard]] std::string TakeSummary(double seconds) {
        if (!own) {
            return {};
        }
        const auto take = [](std::atomic<u64>& counter) {
            return static_cast<double>(counter.exchange(0, std::memory_order_relaxed));
        };
        const double per = seconds > 0.0 ? 1.0 / seconds : 0.0;
        const double signals = take(stats.runtime_signals) * per;
        const double waits = take(stats.runtime_waits) * per;
        const double skipped = take(stats.runtime_skipped) * per;
        const double game_waits = take(stats.game_waits) * per;
        const double game_host = take(stats.game_host) * per;
        const double empty = take(stats.empty_submits) * per;
        const double ahead = take(stats.pre_acquired) * per;
        const double not_ready = take(stats.pre_not_ready) * per;
        return fmt::format("; XR queue: {:.1f} signals/s, its waits {:.1f}/s ({:.1f}/s already "
                           "waited for), the game queue's waits {:.1f}/s ({:.1f}/s seen done), "
                           "{:.1f} empty submits/s; images acquired ahead {:.1f}/s ({:.1f}/s not "
                           "ready: waited for by the GPU thread){}",
                           signals, waits, skipped, game_waits, game_host, empty, ahead, not_ready,
                           failed.load(std::memory_order_relaxed)
                               ? " (FAILED: the queues are no longer ordered)"
                               : "");
    }

private:
    struct Stats {
        std::atomic<u64> runtime_signals{};
        std::atomic<u64> runtime_waits{};
        std::atomic<u64> runtime_skipped{};
        std::atomic<u64> game_waits{};
        std::atomic<u64> game_host{};
        std::atomic<u64> empty_submits{}; // Signal and WaitFor: one vkQueueSubmit each
        std::atomic<u64> pre_acquired{};  // NotePreAcquire
        std::atomic<u64> pre_not_ready{};
    };

    static u64 Key(vk::Semaphore semaphore) {
        return reinterpret_cast<u64>(static_cast<VkSemaphore>(semaphore));
    }

    // Scheduler::submit_mutex held: whether the graphics queue must still wait for the XR queue's
    // timeline at `value` (Wait::Queue). Host: the host saw it reached, recorded as waited for.
    VrXrQueuePolicy::Wait PlanGameWait(u64 value) {
        u64 seen = 0;
        if (value > game_waited) {
            const auto [result, counter] = device.getSemaphoreCounterValue(runtime_timeline);
            seen = result == vk::Result::eSuccess ? counter : 0;
        }
        const VrXrQueuePolicy::Wait wait = VrXrQueuePolicy::ChooseWait(game_waited, value, seen);
        if (wait == VrXrQueuePolicy::Wait::Host) {
            game_waited = value;
            stats.game_host.fetch_add(1, std::memory_order_relaxed);
        }
        return wait;
    }

    bool CreateTimeline(vk::Semaphore& out) {
        const vk::SemaphoreTypeCreateInfo type{
            .semaphoreType = vk::SemaphoreType::eTimeline,
            .initialValue = 0,
        };
        const auto [result, semaphore] = device.createSemaphore({.pNext = &type});
        if (result != vk::Result::eSuccess) {
            LOG_ERROR(Render_Vulkan, "VR: the XR queue's timeline semaphore failed ({})",
                      vk::to_string(result));
            return false;
        }
        out = semaphore;
        return true;
    }

    bool Fail(const char* what, vk::Result result) {
        VrDeviceLost::Note(result, what);
        if (!failed.exchange(true)) {
            LOG_ERROR(Render_Vulkan, "VR: {} failed ({}); the XR queue is no longer ordered",
                      what, vk::to_string(result));
        }
        return false;
    }

    // An empty batch on `queue` signalling `semaphore` at `value` after everything submitted on it
    // before (the signal's first scope includes all earlier submissions).
    bool Signal(vk::Queue queue, vk::Semaphore semaphore, u64 value, const char* what) {
        const vk::TimelineSemaphoreSubmitInfo timeline{
            .signalSemaphoreValueCount = 1,
            .pSignalSemaphoreValues = &value,
        };
        const vk::SubmitInfo submit{
            .pNext = &timeline,
            .signalSemaphoreCount = 1,
            .pSignalSemaphores = &semaphore,
        };
        stats.empty_submits.fetch_add(1, std::memory_order_relaxed);
        const vk::Result result = queue.submit(submit, vk::Fence{});
        return result == vk::Result::eSuccess || Fail(what, result);
    }

    // An empty batch on `queue` waiting for `semaphore` at `value`: everything submitted on `queue`
    // later starts after it (a wait's second scope includes all later submissions).
    bool WaitFor(vk::Queue queue, vk::Semaphore semaphore, u64 value, const char* what) {
        const vk::PipelineStageFlags stage = vk::PipelineStageFlagBits::eAllCommands;
        const vk::TimelineSemaphoreSubmitInfo timeline{
            .waitSemaphoreValueCount = 1,
            .pWaitSemaphoreValues = &value,
        };
        const vk::SubmitInfo submit{
            .pNext = &timeline,
            .waitSemaphoreCount = 1,
            .pWaitSemaphores = &semaphore,
            .pWaitDstStageMask = &stage,
        };
        stats.empty_submits.fetch_add(1, std::memory_order_relaxed);
        const vk::Result result = queue.submit(submit, vk::Fence{});
        return result == vk::Result::eSuccess || Fail(what, result);
    }

    vk::Device device{};
    vk::Queue game_queue{};
    vk::Queue xr_queue{};
    std::mutex* mutex{nullptr};
    bool own{false};
    std::atomic<bool> failed{false};
    // The XR queue's timeline: signalled after the runtime's acquires (the XR queue's lock).
    vk::Semaphore runtime_timeline{};
    std::atomic<u64> runtime_submitted{0};
    u64 game_waited{0}; // Scheduler::submit_mutex: the graphics queue waits for runtime_timeline
    // The graphics queue's timeline for RuntimeWaitsForGame and PlanGameBatch (both locks).
    vk::Semaphore game_timeline{};
    u64 game_signalled{0};
    // The XR queue's lock: the highest value of each timeline the XR queue waits for.
    VrXrQueuePolicy::WaitedValues<8> runtime_waited{};
    Stats stats{}; // counted under either lock, taken by the frame thread (its log line)

    static inline thread_local u32 held_here{0};
};

} // namespace Vulkan
