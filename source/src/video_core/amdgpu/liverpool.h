// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <condition_variable>
#include <coroutine>
#include <exception>
#include <mutex>
#include <memory>
#include <semaphore>
#include <span>
#include <thread>
#include <vector>
#include <queue>

#include "../../../scratchpad/dual-eye-perf/register_delta.h"
#include "common/assert.h"
#include "common/slot_vector.h"
#include "common/types.h"
#include "common/unique_function.h"
#include "video_core/amdgpu/cb_db_extent.h"
#include "video_core/amdgpu/regs.h"
#include "video_core/fast_render.h"

namespace ScratchDraw {
struct GraphicsSubmissionScan;
class SpeculativePool;
}

namespace Vulkan {
class Rasterizer;
struct GraphicsSubmission;
}

namespace Libraries::VideoOut {
struct VideoOutPort;
}

namespace AmdGpu {

struct Liverpool {
    static constexpr u32 GfxQueueId = 0u;
    static constexpr u32 NumGfxRings = 1u;     // actually 2, but HP is reserved by system software
    static constexpr u32 NumComputePipes = 7u; // actually 8, but #7 is reserved by system software
    static constexpr u32 NumQueuesPerPipe = 8u;
    static constexpr u32 NumComputeRings = NumComputePipes * NumQueuesPerPipe;
    static constexpr u32 NumTotalQueues = NumGfxRings + NumComputeRings;
    static_assert(NumTotalQueues < 64u); // need to fit into u64 bitmap for ffs

    enum ContextRegs : u32 {
        DbZInfo = 0xA010,
        CbColor0Base = 0xA318,
        CbColor1Base = 0xA327,
        CbColor2Base = 0xA336,
        CbColor3Base = 0xA345,
        CbColor4Base = 0xA354,
        CbColor5Base = 0xA363,
        CbColor6Base = 0xA372,
        CbColor7Base = 0xA381,
        CbColor0Cmask = 0xA31F,
        CbColor1Cmask = 0xA32E,
        CbColor2Cmask = 0xA33D,
        CbColor3Cmask = 0xA34C,
        CbColor4Cmask = 0xA35B,
        CbColor5Cmask = 0xA36A,
        CbColor6Cmask = 0xA379,
        CbColor7Cmask = 0xA388,
    };

    Regs regs{};
    using RegisterDeltas = ScratchDraw::RegisterDeltaTracker<Regs::NumRegs>;
    RegisterDeltas register_deltas;
    /// 3k: ClearState sends a reset flag instead of MarkAll's full ~213 KB register file
    /// (bbport vk_rasterizer.cpp:1194/1318 regs_reset); consumers reset their replay file.
    static constexpr auto ClearStateReset = static_cast<FastRender::Bit>(30);
    template <typename T>
    void MarkRegisterWrite(const T& field) {
        const auto first =
            (reinterpret_cast<const char*>(&field) - reinterpret_cast<const char*>(&regs)) /
            sizeof(u32);
        register_deltas.Mark(first, (sizeof(T) + sizeof(u32) - 1) / sizeof(u32));
    }
    std::array<CbDbExtent, NUM_COLOR_BUFFERS> last_cb_extent{};
    CbDbExtent last_db_extent{};

public:
    explicit Liverpool();
    ~Liverpool();

    void SubmitGfx(std::span<const u32> dcb, std::span<const u32> ccb);
    void SubmitAsc(u32 gnm_vqid, std::span<const u32> acb);

    void SubmitDone() noexcept {
        std::scoped_lock lk{submit_mutex};
        mapped_queues[GfxQueueId].ccb_buffer_offset = 0;
        mapped_queues[GfxQueueId].dcb_buffer_offset = 0;
        submit_done = true;
        submit_cv.notify_one();
    }

    void WaitGpuIdle() noexcept {
        std::unique_lock lk{submit_mutex};
        submit_cv.wait(lk, [this] { return num_submits == 0; });
    }

    bool IsGpuIdle() const {
        return num_submits == 0;
    }

    void SetVoPort(Libraries::VideoOut::VideoOutPort* port) {
        vo_port = port;
    }

    void BindRasterizer(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
    }

    bool IsRecordingThread() const {
        return std::this_thread::get_id() == gpu_id || FastRender::IsStageB();
    }

    template <bool wait_done = false>
    void SendCommand(auto&& func) {
        if (std::this_thread::get_id() == gpu_id || FastRender::IsStageB()) {
            if constexpr (wait_done) {
                SyncRecorder("send_command_wait", true);
                func();
                SyncRecorder("send_command_done");
                return;
            } else {
                return func();
            }
        }
        if constexpr (wait_done) {
            std::binary_semaphore sem{0};
            {
                std::scoped_lock lk{submit_mutex};
                command_queue.emplace([this, &sem, &func] {
                    SyncRecorder("send_command_wait", true);
                    func();
                    SyncRecorder("send_command_done");
                    sem.release();
                });
                ++num_commands;
                submit_cv.notify_one();
            }
            sem.acquire();
        } else {
            std::scoped_lock lk{submit_mutex};
            command_queue.emplace(std::move(func));
            ++num_commands;
            submit_cv.notify_one();
        }
    }

    void ReserveCopyBufferSpace() {
        GpuQueue& gfx_queue = mapped_queues[GfxQueueId];
        std::scoped_lock lk(gfx_queue.m_access);
        constexpr size_t GfxReservedSize = 2_MB >> 2;
        gfx_queue.ccb_buffer.reserve(GfxReservedSize);
        gfx_queue.dcb_buffer.reserve(GfxReservedSize);
    }

    inline ComputeProgram& GetCsRegs() {
        return mapped_queues[curr_qid].cs_state;
    }

    struct AscQueueInfo {
        static constexpr size_t Pm4BufferSize = 1024;
        VAddr map_addr;
        u32* read_addr;
        u32 ring_size_dw;
        u32 pipe_id;
        std::array<u32, Pm4BufferSize> tmp_packet;
        u32 tmp_dwords;
    };
    Common::SlotVector<AscQueueInfo> asc_queues{};

private:
    struct Task {
        struct promise_type {
            auto get_return_object() {
                Task task{};
                task.handle = std::coroutine_handle<promise_type>::from_promise(*this);
                return task;
            }
            static constexpr std::suspend_always initial_suspend() noexcept {
                // We want the task to be suspended at start
                return {};
            }
            static constexpr std::suspend_always final_suspend() noexcept {
                return {};
            }
            void unhandled_exception() {
                try {
                    std::rethrow_exception(std::current_exception());
                } catch (const std::exception& e) {
                    UNREACHABLE_MSG("Unhandled exception: {}", e.what());
                }
            }
            void return_void() {}
            struct empty {};
            std::suspend_always yield_value(empty&&) {
                return {};
            }
        };

        using Handle = std::coroutine_handle<promise_type>;
        Handle handle;
    };

    using CmdBuffer = std::pair<std::span<const u32>, std::span<const u32>>;
    CmdBuffer CopyCmdBuffers(std::span<const u32> dcb, std::span<const u32> ccb);
    // `dcb_copy`: the DCB as it was at submit (SubmitGfx), used only to recover from a malformed
    // packet in guest memory (liverpool.cpp, "Malformed packets"); empty for IBs and with
    // VR_PM4_STRICT=1.
    struct PendingFence {
        u64 ticket{};
        VAddr address{};
        u32 value{};
    } pending_fence;
    void ExecuteFencePacket(std::span<const std::byte> packet);
    // `prep`: toggle bit 31, the top-level buffer's speculative preparation (vk_pipeline_cache.h).
    Task ProcessGraphics(std::span<const u32> dcb, std::span<const u32> ccb,
                         std::vector<u32> dcb_copy = {},
                         std::shared_ptr<ScratchDraw::GraphicsSubmissionScan> scan = {},
                         std::shared_ptr<Vulkan::GraphicsSubmission> prep = {});
    Task ProcessCeUpdate(std::span<const u32> ccb);
    template <bool is_indirect = false>
    Task ProcessCompute(std::span<const u32> acb, u32 vqid);

    void ProcessCommands();
    void Process(std::stop_token stoken);

    struct GpuQueue {
        std::mutex m_access{};
        std::atomic<u32> dcb_buffer_offset;
        std::atomic<u32> ccb_buffer_offset;
        std::vector<u32> dcb_buffer;
        std::vector<u32> ccb_buffer;
        std::queue<Task::Handle> submits{};
        ComputeProgram cs_state{};
    };
    std::array<GpuQueue, NumTotalQueues> mapped_queues{};
    u32 num_mapped_queues{1u}; // GFX is always available

    VAddr indirect_args_addr{};
    u32 num_counter_pairs{};
    u64 pixel_counter{};

    struct ConstantEngine {
        void Reset() {
            ce_count = 0;
            de_count = 0;
            ce_compare_count = 0;
        }

        [[nodiscard]] u32 Diff() const {
            ASSERT_MSG(ce_count >= de_count, "DE counter is ahead of CE");
            return ce_count - de_count;
        }

        u32 ce_compare_count{};
        u32 ce_count{};
        u32 de_count{};
        static std::array<u8, 48_KB> constants_heap;
    } cblock{};

    Vulkan::Rasterizer* rasterizer{};
    Libraries::VideoOut::VideoOutPort* vo_port{};
    std::jthread process_thread{};
    std::unique_ptr<ScratchDraw::SpeculativePool> scan_pool;
    u64 scan_ready{}, scan_pending{}, scan_unavailable{}, scan_stale{};
    u64 scan_packets{}, scan_replay_checks{}, scan_hash_checks{};
    u64 scan_mismatches{}, scan_queued{}, scan_dropped{};
    u64 gfx_submits{}; // top-level GFX submissions; guarded by the GFX queue's m_access
    u64 clear_states{}, clear_state_resets{}, clear_state_log_states{}, clear_state_log_frames{};
    void SyncRecorder(const char* reason, bool submit = false);
    bool SyncGuestWrites(const char* reason);
    // FAST_CPU_ASYNC_IDLE (vk_scheduler.h): the ordered submission retirement and GPU-idle task
    // leave their wait to the recorder (false: the old path runs, its _VERIFY checks included).
    bool RetireSubmission(Task::Handle finished);
    bool SignalIdle();
    // Replay tasks (Scheduler::QueueReplayTask): `signals` is the guest signals encoded before.
    static void RetireOnReplay(void* context, u64 signals);
    static void IdleOnReplay(void* context, u64 signals);
    static void VerifyOnReplay(void* context, u64 signals); // FAST_CPU_ASYNC_IDLE_VERIFY
    void ReportAsyncIdle(); // FAST_CPU_ASYNC_IDLE_VERIFY's counts, now and then
    // Retirements queued to the recorder and not run yet (under submit_mutex), and every replay
    // task above still queued (Process drains the recorder for them before it returns).
    std::atomic<u32> async_retires{};
    std::atomic<u32> async_tasks{};
    // FAST_CPU_ASYNC_IDLE_VERIFY: the replay's GPU-idle decision (1 no, 2 yes) for the old path to
    // compare, and the counts (replay: checks, unwritten; B or A: idles, differ).
    std::atomic<u32> verify_idle{};
    std::atomic<u64> verify_checks{}, verify_unwritten{}, verify_idles{}, verify_differ{};
    std::atomic<u32> num_submits{};        // Includes submissions still executing on B.
    std::atomic<u32> num_decode_submits{}; // Submissions still owned by the decoder.
    std::atomic<u32> num_commands{};
    std::atomic<bool> submit_done{};
    std::mutex submit_mutex;
    std::condition_variable_any submit_cv;
    std::queue<Common::UniqueFunction<void>> command_queue{};
    std::thread::id gpu_id;
    s32 curr_qid{-1};
};

} // namespace AmdGpu
