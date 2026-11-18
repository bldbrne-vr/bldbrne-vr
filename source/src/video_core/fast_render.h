// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
//
// Fast renderer switches and statistics (fast/v2).
//
// Every optimization of the fast renderer has a bit in one 64-bit mask. The mask starts from
// kDefaultMask, FAST_TOGGLES (hex or decimal) replaces it at start, and FAST_TOGGLE_FILE names a
// file whose content replaces it while the game runs, so one session can A/B a change in
// alternating phases. A bit is read with one relaxed load; code that keeps state across draws
// must tolerate the bit changing between two draws.
//
// Statistics: every 2 s a "FAST stats" line gives the game frame rate, draws and dispatches per
// frame, and each registered thread's CPU service time per frame/draw (GetThreadTimes), with cycles
// reported separately. A window in which the mask changed is marked mixed=1 and left out of
// comparisons.

#pragma once

#include <array>
#include <atomic>
#include <cstdlib>
#include <source_location>
#include <string_view>
#include "common/assert.h"
#include "common/types.h"
#include "video_core/fast/lean_scopes.h"

namespace FastRender {

enum Bit : u32 {
    /// Phase 1: on a guest write fault, unprotect the aligned window around the faulting page
    /// (FAST_FAULT_WINDOW_KB, 64 by default) instead of one page.
    FaultWindow = 0,
    /// P1: TextureCache::UpdateImage returns without the mutex for a clean, fully tracked image
    /// already touched in this GC period (bbport +2%).
    UpdateImageFast = 1,
    /// Reuse an identical active render state after validating its targets.
    BeginStateMemo = 2,
    /// S1: owned draw inputs and verified register-delta replay on the same thread.
    PacketLoopback = 3,
    /// S2 conservative recording worker (implies owned packet inputs).
    RecordingThread = 4,
    /// S3: the existing no-op DMA to 0x3022c does not drain the recording worker.
    SkipNoopDmaDrain = 5,
    SkipPureStateDrain = 6,
    OrderedFenceTasks = 7,
    RecorderInline = 8,
    RecorderThread = 9,
    CopyThreads = 10, // B3: protected guest uploads to coherent staging.
    GeometryPrepare = 14, // Per-draw vertex preparation on A, no cross-draw cache.
    ConstantRing = 11, // B4: A prepares read-only buffers and native pairs, opt-in.
    PendingPollLimit = 12, // Skip empty cleanup locks; amortize GPU completion queries.
    ResourcePrepare = 13, // Pure resource sharps, texture keys and hashes on A.
    PacketViews = 15, // Borrow aligned queue payload arrays while B consumes a packet.
    CeTargetedWait = 16, // bbport WaitForPendingReads at CE DumpConstRam (box-filtered notes).
    LockFreeDirtyQuery = 17, // bbport lock-free IsRegionGpuModified.
    GuardTable = 19, // bbport granule tickets for the constant-ring write guard (measured slower).
    SamplerMemo = 23, // per-thread sampler handle memo.
    UploadEarlyExit = 24, // bbport upload early exits, redundant GPU-dirty re-query removal.
    StereoHandles = 25, // carry right-eye image/buffer handles through binding.
    BindMemoHit = 26, // bbport one-pass texture-set memo hit path.
    TargetLookupMemo = 27, // bbport FindTargetMemoized.
    PushDescBulk = 28, // bbport bulk push-descriptor encode.
    DynStateSkip = 29, // skip unchanged dynamic state (measured no gain).
    ClearStateReset = 30, // bbport regs_reset instead of a full register file per ClearState.
    GraphicsPrep = 31, // bbport speculative scanner + preparation workers, cheap consumer.
    PacketPrep = 32, // workers also prepare vertex inputs and the constant list (bbport 4.4).
    SmallReadDirect = 33, // bind clean cached small read-only buffers in place.
    IndexBind = 34, // index buffer obtain memo.
    BindFilter = 35, // skip redundant recorded binds (lean recording).
    MotionFast = 36, // temporal object motion: proven index ranges, camera/player/variant memos.
    UpscalerFast = 37, // DLSS/FSR passes through the recorder (FSR4 LDS post pass opt-in only).
    GpuPassProfile = 38, // per-pass GPU timestamps; also needs FAST_GPU_PASS_PROFILE=1.
    TileFromImage = 39, // re-tile image-to-buffer copies straight from the image.
    AsyncPipelines = 40, // background graphics pipeline compiles, kicked by the bit-31 workers.
    MergeScaledHud = 42, // pass merge also for the HUD at S1's output size (VrUiShadowDraw).
    LazyReplayWake = 47, // the recorder wakes its replay only while it sleeps (CommandRecorder::Publish).
    IndirectNoDrain = 48, // indirect draws and dispatches overlap stage B (Liverpool::ProcessGraphics).
    PublishSmall = 49,    // the recorder publishes every 4 KB (CommandRecorder::Commit).
    BackgroundFree = 50,  // freed buffer/image memory released on a background thread.
    PooledBuffers = 52,   // cache buffers under 64 MB from VMA blocks, not dedicated (buffer.cpp).
    // A5 image candidate; never part of kDefaultMask without headset acceptance.
    TightNativeFrustum = 20,
    TextureDescTwoWay = 18,  // A4 pure-value cache candidate
    KeepEyeFov = 21,         // A2 image candidate, opt-in only
    TrivialWaitElision = 22, // B1c: unsigned >= 0 memory waits, opt-in
    NumBits = 64,
};

/// Scratchpad default adds measured borrowed packet views and the bbport ports measured faster in
/// in-run A/B (2026-10-13): 12,16,17,23-28,30-37,39-41,43-46,53-57,59-63. 0.5.5
/// (2026-10-24, soaked, headset-checked) adds 42 MergeScaledHud, 47 LazyReplayWake, 48
/// IndirectNoDrain, 50 BackgroundFree and 52 PooledBuffers. 19, 29, 49, 51, 58 and the diagnostic
/// 38 stay off. 41 SmaaRecorder (vr_smaa.cpp), 43 PassContinue (vk_pass_breaks.h),
/// 44 SmallReadSettle (buffer.h), 45 Fsr4Interleave (vr_fsr4_interleave.h) and 46 DlssDirect
/// (vr_dlss_direct.h; on since 2026-10-23: models K and M reconstruct, images within run-to-run
/// noise, colour and left depth copies no longer allocated) are declared next to their code.
/// FAST_TOGGLES=0x307 selects serial + recorder; 0x7 disables the recorder too.
constexpr u64 kDefaultMask =
    (u64{1} << FaultWindow) | (u64{1} << UpdateImageFast) | (u64{1} << BeginStateMemo) |
    (u64{1} << RecordingThread) | (u64{1} << SkipNoopDmaDrain) |
    (u64{1} << SkipPureStateDrain) | (u64{1} << OrderedFenceTasks) |
    (u64{1} << RecorderInline) | (u64{1} << RecorderThread) |
    (u64{1} << ConstantRing) | (u64{1} << TrivialWaitElision) |
    (u64{1} << GeometryPrepare) | (u64{1} << PacketViews) | (u64{1} << PendingPollLimit) |
    (u64{1} << CeTargetedWait) | (u64{1} << LockFreeDirtyQuery) | (u64{1} << SamplerMemo) |
    (u64{1} << UploadEarlyExit) | (u64{1} << StereoHandles) | (u64{1} << BindMemoHit) |
    (u64{1} << TargetLookupMemo) | (u64{1} << PushDescBulk) | (u64{1} << ClearStateReset) |
    (u64{1} << GraphicsPrep) | (u64{1} << PacketPrep) | (u64{1} << SmallReadDirect) |
    (u64{1} << IndexBind) | (u64{1} << BindFilter) | (u64{1} << MotionFast) |
    (u64{1} << UpscalerFast) | (u64{1} << TileFromImage) | (u64{1} << AsyncPipelines) |
    (u64{1} << 41) | (u64{1} << 43) | (u64{1} << 44) | (u64{1} << 45) | (u64{1} << 46) |
    (u64{1} << MergeScaledHud) | (u64{1} << LazyReplayWake) | (u64{1} << IndirectNoDrain) |
    (u64{1} << BackgroundFree) | (u64{1} << PooledBuffers) | (u64{1} << 53) |
    (u64{1} << 54) | (u64{1} << 55) | (u64{1} << 56) | (u64{1} << 57) | (u64{1} << 59) |
    (u64{1} << 60) | (u64{1} << 61) | (u64{1} << 62) | (u64{1} << 63);
static_assert(kDefaultMask == 0xfbf5ffbfdfc3dbf7);
static_assert((kDefaultMask & (u64{1} << CopyThreads)) == 0);

inline std::atomic<u64> g_mask{kDefaultMask};
/// FAST_TOGGLE_FILE's optional second word (2026-11-02): the switches of changes under in-run A/B,
/// each on unless the file clears its bit (AbOn); the "FAST stats" line ends with ab=<mask>.
/// 0: the HLE copy overlap bitmap (vk_shader_hle.cpp DestinationsOverlapBitmap); 1: the per-draw
/// shared hold of the mapping lock (memory.cpp SparseReadHold); 2: the copy shader's written pages
/// without a sort (vk_rasterizer.cpp AddCopyShaderWrites); 4: the recorder's idle spin reading the
/// clock every 64 pauses (vk_recorder.cpp CommandRecorder::Run); 5: the recorder's idle spin at
/// all (cleared: straight to the wait); 6: stage B's idle spin at all (vk_draw_pipe.h DrawPipe::Run);
/// 7: S1's in-frame items drawn in place in its depth, no private copy per eye
/// (vk_rasterizer_vr.cpp VrInjectInFrame); 8: both eyes' light probe chains built level by level
/// together, one barrier per level (vr_inframe.cpp VrInFrame::ProbeScenes); 9: the recorder's idle
/// spin at its length (cleared: 5x); 10: stage B's idle spin at its length (cleared: 4x). The spins'
/// lengths hang on ProcessCpus (none with fewer than 8 CPUs to run on). 11: water reflections off
/// never take an sRGB target for the reflection (vk_rasterizer.cpp WaterReflectionSkipped; cleared:
/// as before, the blood-paint textures wiped too, for benchmarks only). 16: VR_EYE_SPEC's variants
/// used (vr_eye_spec_variants.h UseBit; cleared: every draw binds its base pipeline). 13: the copy
/// shader's copies inside a render pass kept for the pass's end (vk_shader_hle.cpp CopySink; also
/// off with FAST_HLE_COPY_SINK=0; every one of the 64 FAST bits is taken).
/// Tried and removed: a PM4 stream prefetch (0%).
inline std::atomic<u64> g_ab_mask{~u64{0}};
[[nodiscard]] inline bool AbOn(unsigned bit) noexcept {
    return ((g_ab_mask.load(std::memory_order_relaxed) >> bit) & 1) != 0;
}

[[nodiscard]] inline bool On(Bit bit) noexcept {
    return (g_mask.load(std::memory_order_relaxed) >> bit) & 1;
}

/// Threads whose CPU time the statistics report.
enum class Role : u32 { GpuCommand, Recorder, StageB, Copy0, Copy1, Count };
inline thread_local bool g_on_stage_b{};
inline bool IsStageB() noexcept {
    return g_on_stage_b;
}
// The recorder's replay thread (CommandRecorder::Run), for FAST_CPU_FAULT_WINDOW_VERIFY's counts.
inline thread_local bool g_on_recorder{};
inline bool IsRecorderThread() noexcept {
    return g_on_recorder;
}
inline thread_local bool g_preparing_packet{};
struct ProducerScope {
    bool previous{g_preparing_packet};
    explicit ProducerScope(bool enabled) {
        g_preparing_packet = enabled;
    }
    ~ProducerScope() {
        g_preparing_packet = previous;
    }
};
inline const bool g_pipe_verify = std::getenv("FAST_PIPE_VERIFY") != nullptr;
// FAST_CPU_LEAN_SCOPES: the FAST_PIPE_VERIFY checks (this one and Rasterizer::AssertRecordingOwner)
// call out of line only when they can fail; '0': every call does, as the uninlined assert did.
inline const bool g_pipe_check_call = !g_lean_scopes || g_pipe_verify;
SHAD_NO_INLINE inline void CheckRecordingAccess() {
    ASSERT_MSG(!g_pipe_verify || !g_preparing_packet,
               "Recording/cache access during stage-A preparation");
}
inline void AssertRecordingAccess() {
    if (g_pipe_check_call) [[unlikely]] {
        CheckRecordingAccess();
    }
}

/// Once, before the GPU command thread starts: reads FAST_TOGGLES / FAST_TOGGLE_FILE and starts
/// the background thread that polls the file and prints the statistics.
void Start();

/// On the thread itself.
void RegisterThread(Role role);
/// The logical CPUs the process may run on (its affinity mask), read once, at the first call (the
/// renderer threads' start, after a launcher or benchmark set the mask). The idle spins' default
/// hangs on it: spinning threads cost a weak CPU the time its other threads need.
unsigned ProcessCpus();
void NoteRecorderFlush(std::string_view reason, std::source_location site, bool pending);
void NoteRecorderEncoded(u64 packets, u64 bytes, u64 sampled_ns, u64 samples);
void NoteRecorderReplay(u64 ns);
void NoteRecorderWait(u64 ns);
// FAST_RECORDER_WAIT_DETAIL=1: attribute the existing drain wall/spin intervals by reason.
// Diagnostic only; no scheduling, submission or memory-ordering changes.
void NoteRecorderWaitReason(std::string_view reason, std::source_location site, u64 wait_ns,
                            u64 spin_ns, bool on_stage_b);
/// FAST_CPU_DRAIN_SPIN: the time a flush wait spent spinning before it slept or found the replay
/// done (busy time on the waiting thread; on_stage_b: B's).
void NoteRecorderSpin(u64 ns, bool on_stage_b);

struct Counters {
    // Single writer (the GPU command thread), so increments need no locked instruction.
    std::atomic<u64> draws{0};
    std::atomic<u64> dispatches{0};
    std::atomic<u64> view_mask_reselects{0};
    std::atomic<u64> pipe_wait_ns{0};
    std::array<std::atomic<u64>, 10> pipe_drains{};
    std::array<std::atomic<u64>, 256> pipe_opcode_drains{};
    std::atomic<u64> handler_ticks{0};
    std::atomic<u64> handler_packets{0};
    std::atomic<u64> queued_fence_waits{0};
    std::atomic<u64> trivial_waits{0};
    // Many writers.
    std::atomic<u64> frames{0};
    std::atomic<u64> gameplay_compiles{0};
    std::atomic<u64> faults{0};
    std::atomic<u64> texture_desc_calls{0}, texture_desc_misses{0};
    std::atomic<u64> view_calls{0}, view_steps{0}, view_misses{0};
    std::atomic<u64> encoded_packets{0}, encoded_bytes{0}, encode_sampled_ns{0}, encode_samples{0};
    std::atomic<u64> replay_ns{0}, recorder_wait_ns{0}, forwarded_fence_waits{0};
    std::atomic<u64> drain_spin_ns{0}, drain_spin_b_ns{0}; // NoteRecorderSpin
};
inline Counters g_counters;
// Diagnostic counters are opt-in; matched timing runs leave them disabled.
inline const bool g_cache_stats = [] {
    const char* value = std::getenv("FAST_CACHE_STATS");
    return value && value[0] == '1';
}();
inline bool CacheStatsEnabled() {
    return g_cache_stats;
}
// CommandRecorder::Allocate/Commit time the encode of one packet in this many (a power of two) for
// the FAST encode line's encode_us/f: 256 with FAST_CPU_LEAN_SCOPES (two clock reads per sample), 64
// with '0', as before. The estimate scales by the same number either way.
inline const u32 g_encode_sample_every = g_lean_scopes ? 256u : 64u;

inline void Bump(std::atomic<u64>& counter) noexcept {
    counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
}
inline void NoteDraw() noexcept {
    Bump(g_counters.draws);
}
inline void NoteDispatch() noexcept {
    Bump(g_counters.dispatches);
}
inline void NoteFrame() noexcept {
    g_counters.frames.fetch_add(1, std::memory_order_relaxed);
}
inline void NoteFault() noexcept {
    g_counters.faults.fetch_add(1, std::memory_order_relaxed);
}
// Every guest page fault the renderer's handler sees, hooked or not, before it acts on it
// (FAST_CPU_REPEAT_DRAW: memory may have changed for the renderer since it was read).
inline std::atomic<u64> g_fault_epoch{};
inline void NoteAnyFault() noexcept {
    g_fault_epoch.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace FastRender
