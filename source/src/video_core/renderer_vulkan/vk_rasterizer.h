// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "video_core/native_stereo/asymmetric_projection.h"
#include "video_core/native_stereo/native_stereo.h"
#include "video_core/fast/constant_write_guard.h"
#include <array>
#include <optional>
#include <span>
#include <unordered_map>
#include <vector>

#include "common/recursive_lock.h"
#include "common/shared_first_mutex.h"
#include "core/vr/render_taa.h"
#include "video_core/buffer_cache/buffer_cache.h"
#include "video_core/page_manager.h"
#include "video_core/renderer_vulkan/vk_dispatch_eye_overlap.h"
#include "video_core/renderer_vulkan/vk_pass_merge.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_recorder.h"
#include "video_core/texture_cache/download_log.h"
#include "video_core/texture_cache/texture_cache.h"

namespace AmdGpu {
struct Liverpool;
}

namespace Core {
class MemoryManager;
}

namespace Vulkan {
class RecorderProxy;
class ConstantRing;
namespace VrCapeTable {
struct Range;
}
namespace VrBodyTable {
struct Key;
}
namespace VrHandsTable {
struct Range;
}

class Scheduler;
class DrawPipe;
class RenderState;
class GraphicsPipeline;
class VrStereoWarp;
class VrInFrame;
class VrUiLayer;
class VrSmaa;
class VrRenderTaa;
class VrDibrDlss;
struct ForegroundProbe;
struct NativeAudit;
struct VrTemporalCapture;
class GpuPassProfiler;

class Rasterizer {
public:
    explicit Rasterizer(const Instance& instance, Scheduler& scheduler,
                        AmdGpu::Liverpool* liverpool);
    ~Rasterizer();

    [[nodiscard]] Scheduler& GetScheduler() noexcept {
        return scheduler;
    }

    [[nodiscard]] const Instance& GetInstance() const noexcept {
        return instance;
    }

    [[nodiscard]] VideoCore::BufferCache& GetBufferCache() noexcept {
        return buffer_cache;
    }

    /// VR_PERF_CHECK=1 or VR_PERF_STATS=1: the per-draw shortcuts' statistics are gathered and
    /// logged every 5 s ('VR perf check:').
    static bool PerfStats() noexcept;
    /// Statistics of the copy shader's HLE (vk_shader_hle.cpp) in the current 5 s window, with
    /// PerfStats() only: what a range cache could skip there (perf2 plan, point 2c).
    struct CopyHleStats {
        u64 calls{};
        u64 copies{};
        u64 batches{};
        u64 src_bytes{};
        u64 dst_bytes{};
        u64 src_cpu_mod{}; // batches whose source range had a CPU-modified page (an upload)
        u64 src_gpu_mod{}; // ... a GPU-modified page
        u64 dst_cpu_mod{};
        u64 dst_gpu_mod{};
        u64 repeats{}; // calls equal (control data, source and destination) to one of the last 16
        std::array<u64, 16> recent{};
        u32 recent_next{};
        double us{}; // wall time inside the HLE
    } copy_stats;

    /// The copy shader HLE's buffers for one batch (vk_shader_hle.cpp): ObtainBuffer of the source
    /// range (read) and of the destination range (written), or with perf_copy_memo the buffers
    /// remembered for the same ranges while nothing changed there (vk_perf_memo.h CopyEntry).
    struct CopyBuffers {
        VideoCore::Buffer* src;
        u32 src_offset;
        VideoCore::Buffer* dst;
        u32 dst_offset;
    };
    CopyBuffers ObtainCopyBuffers(VAddr src_addr, u32 src_size, VAddr dst_addr, u32 dst_size);

    [[nodiscard]] VideoCore::TextureCache& GetTextureCache() noexcept {
        return texture_cache;
    }

    bool DrainDrawPipe(u32 reason = 7, u32 opcode = 256);
    bool IsDrawTaskPending(u64 ticket) const;
    bool DrawWorkerActive() const {
        return worker_active;
    }
    u64 QueueOrderedTask(void (*handler)(void*, std::span<const std::byte>), void* context,
                         std::span<const std::byte> packet, bool writes_known = false);
    void GuardDeferredConstantWrite(VAddr address, u64 bytes);
    void GuardConstantWrite(VAddr address, u64 bytes, u64 ticket);
    /// FAST bit 16 (bbport WaitForPendingReads): before a CE DumpConstRam, wait only for
    /// queued packets that read the dumped bytes. False means: take the full drain and
    /// flush deferred guest signals as before.
    bool CeDumpWait(VAddr address, u64 bytes);
    void Draw(bool is_indexed, u32 index_offset = 0);
    /// Bit 31, Stage A: the next Draw's speculative preparation and the live register checksum.
    void SetPreparedDraw(const PreparedDraw* draw, u64 checksum) {
        prepared_draw = draw;
        prepared_checksum = checksum;
        prepared_set = true;
    }
    void DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 size, u32 max_count,
                      VAddr count_address);

    void DispatchDirect();
    void DispatchIndirect(VAddr address, u32 offset, u32 size);

    void ScopeMarkerBegin(const std::string_view& str, bool from_guest = false);
    void ScopeMarkerEnd(bool from_guest = false);
    void ScopedMarkerInsert(const std::string_view& str, bool from_guest = false);
    void ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                 bool from_guest = false);

    void FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds);
    void CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds);
    u32 ReadDataFromGds(u32 gsd_offset);
    /// `emulator_write`: the emulator's own write of exactly these bytes (MemoryManager::
    /// InvalidateMemory: a file read, a video frame, a protection change), not a guest fault.
    /// `protection_only`: a protection change (sceKernelMprotect), which writes no byte.
    bool InvalidateMemory(VAddr addr, u64 size, bool emulator_write = false,
                          bool protection_only = false);
    /// Fast renderer (FastRender::FaultWindow): after a guest write fault at `addr`.
    void UnprotectFaultWindow(VAddr addr);
    /// InvalidateMemory(addr, 8) for a guest write fault there (FAST_GPU_PAGE_KEEP).
    bool InvalidateWriteFaultPage(VAddr addr);
    /// FAST_CPU_FAULT_WINDOW: a guest write fault at `addr` (FAST bit 0): InvalidateMemory(addr,
    /// 8) and its fault window in one protection change, with one mapping check. False when
    /// `addr` is not GPU mapped.
    bool InvalidateWriteFault(VAddr addr);
    bool ReadMemory(VAddr addr, u64 size);
    void ProcessDownloadImages();
    bool IsMapped(VAddr addr, u64 size);
    void MapMemory(VAddr addr, u64 size);
    void UnmapMemory(VAddr addr, u64 size);

    void CpSync();
    u64 Flush();
    /// The guest's SubmitDone flush: Flush(), except that FAST bit 56 skips a submission with
    /// nothing in it (Scheduler::FlushFrameEnd).
    void FlushFrameEnd();
    void Finish();
    void OnSubmit();

    PipelineCache& GetPipelineCache() {
        return pipeline_cache;
    }

    // VR DIBR stereo (stereo_mode 2, vk_rasterizer_vr.cpp). The presenter owns the warp; the
    // rasterizer captures the scene colour and depth into it at the game's composite draw.
    void SetVrStereoWarp(VrStereoWarp* warp) {
        vr_warp = warp;
    }
    // PrepareFrame: this frame's composite target (scene + HUD by now), if still the same image,
    // and how its true colour components are read (the colour buffer's component swap).
    [[nodiscard]] VideoCore::Image* VrCompositeTarget();
    [[nodiscard]] vk::ComponentMapping VrCompositeComponents() const {
        return vr_dibr.color_components;
    }
    // The camera update DrawFrameRecord named at the frame's composite (VR_STAMP tests).
    [[nodiscard]] u64 VrCompositeRecord() const {
        return vr_dibr.composite_record;
    }
    // PrepareFrame / PrepareBlankFrame: the frame is over, nothing of it is captured any more.
    void VrEndFrame();
    bool VrDibrDlssFrame() const { return vr_dibr.dlss; }

    // VR UI layer (vk_rasterizer_vr.cpp, renderer_vulkan/vr_ui_layer.h): in a UI-layer frame every
    // UI colour draw into the composite target X is also drawn into the presenter's K and W.
    void SetVrUiLayer(VrUiLayer* layer) {
        vr_ui_layer = layer;
    }
    // What this frame's UI capture holds (PrepareFrame reads it before VrEndFrame).
    struct VrUiFrameInfo {
        u8 mode{};         // the UI layer in effect (VR::UiLayerFrameMode) when the composite came
        bool marker{};     // the composite was drawn
        bool armed{};      // ... and X was accepted: the UI draws are shadowed
        bool native{};     // ... and drawn at native size (mode 1: not the HUD panel)
        bool incomplete{}; // X was evicted or written outside a shadowed draw in the UI phase
        bool scene_in_warp{}; // the eyes' scene copy is the DIBR warp's (else the UI layer's)
        VideoCore::ImageId x_id{};
        u64 x_uid{};
        vk::ComponentMapping x_components{};
        vk::Extent2D x_extent{}; // K and W's size (the output size under FAST bit 59)
        vk::Format x_format{};
        // FAST bit 59 (S1HudOutput): K and W at S1's output size, X at the input size: the
        // shadow draws' viewports and scissors are scaled by these (1 = off).
        float hud_sx{1.0f};
        float hud_sy{1.0f};
        u32 ui_draws{};  // draws after the composite
        u32 shadowed{};  // ... colour draws into X drawn into K and W
        u32 offtarget{}; // ... into other targets (Scaleform's own; not shadowed)
        u32 backdrop{};  // ... colour draws into X that read X (skipped with ui_backdrop=skip)
        u32 nonaffine{}; // ... with a blend not affine in the destination (approximate)
        u32 dst_alpha{}; // ... with a blend reading destination alpha
        u32 x_writes{};  // dispatches that write X in the UI phase
        // The menu's HUD (VrUiLayer::SnapshotHud): the colour draws before the first that the
        // gameplay HUD never makes (vr_hud_draws), 0 = that draw came first; -1 = every draw of
        // the frame is the HUD's.
        s32 hud_draws{-1};
        u64 first_other{}; // that draw's signature (VrUiDrawSignature)
        const char* refused{}; // why X was not accepted (nullptr: accepted or no composite)
        // The lock-on marker's draws drawn into its own pair (VrUiIsLockMarker) and the factors
        // they were drawn for (vr.ini ui_lock_marker_scale / ui_lock_marker_alpha).
        u32 marker_draws{};
        u32 marker_after{}; // shadowed draws into K and W after the marker's last one
        float marker_scale{1.0f};
        float marker_alpha{1.0f};
    };
    [[nodiscard]] const VrUiFrameInfo& VrUiFrame() const {
        return vr_ui;
    }
    // X of this frame's UI capture, if still the same image.
    [[nodiscard]] VideoCore::Image* VrUiTarget();
    // The frame's UI was resolved (Presenter::RecordVrUi, before VrEndFrame): a gameplay frame's
    // shadowed draws teach the HUD's draws (a draw seen in VrHudLearnFrames gameplay frames in a
    // row is the HUD's; one unseen for VrHudForgetFrames gameplay frames is forgotten), so that a
    // menu's frame keeps K and W before its first draw that is not the HUD's.
    // `plain`: only the HUD was up (VR::IsPlainHud). A plain translucent rect (no texture, no
    // stencil, alpha blended: how a menu dims the screen, and how prompts and subtitles back
    // their text) is learnt only from such frames.
    void VrUiCommitHud(bool gameplay, bool plain);

    template <typename Func>
    void ForEachMappedRangeInRange(VAddr addr, u64 size, Func&& func) {
        const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
        Common::RecursiveSharedLock lock{mapped_ranges_mutex};
        for (const auto& mapped_range : (mapped_ranges & range)) {
            func(mapped_range);
        }
    }

private:
    struct PreparedResources;
    struct DrawPacket;
    // Cache lines by writer. Stage A writes the constant guard and CE state, producer_vr, bit 40's
    // skip state and the prepared draw for every draw; Stage B writes the members around them for
    // every draw. Each such group starts a line of its own (alignas(64); the Rasterizer comes from
    // make_unique, which honours it), so neither stage's stores take a line the other reads or
    // writes; what both only read for every draw shares a line written at creation or a toggle.
    alignas(64) const PreparedResources* resource_bindings{}; // B only, scoped to one bound stage.
    u64 resource_prepare_hits{}, resource_prepare_fallbacks{}, resource_prepare_verified{};
    // Read by A and B for every draw; written only when created or toggled.
    alignas(64) std::unique_ptr<ConstantRing> constant_ring;
    std::unique_ptr<DrawPacket> draw_packet;
    std::unique_ptr<DrawPacket> recording_packet;
    std::unique_ptr<DrawPipe> draw_pipe;
    std::atomic<DrawPipe*> published_pipe{nullptr};
    VrStereoWarp* vr_warp{}; // SetVrStereoWarp; PrepareVrDraw (A) and DrawRecord (B) read it
    bool worker_active{};
    alignas(64) FastRender::ConstantWriteGuard constant_writes; // A-only, as is all down to `ce`
    bool constant_mode{};
    u64 constant_candidates{}, constant_prepared{}, constant_same_draw_refused{},
        constant_pending_refused{}, constant_gpu_refused{}; // A-only counters.
    std::array<u64, 6> constant_refused_by{}; // A-only, indexed by ConstantWriteGuard::Hit.
    // FAST_CPU_GUARD (A only): bbport's pending-write rule, used instead of constant_writes
    // (still fed only with the switch off or FAST_CPU_GUARD_VERIFY=1).
    FastRender::PendingWriteGuard pending_writes;
    struct GuardProgress;
    struct GuardStats { // A only (the "FAST_CPU_GUARD:" line)
        std::array<u64, size_t(FastRender::PendingWriteGuard::Hit::Count)> refused_by{};
        // constant_writes' Hit::Range refusals by the group that made them: ticketed and still
        // queued, ticketed and already passed (not pruned yet), permanent with a deferred signal,
        // permanent with an image footprint, permanent with neither.
        std::array<u64, 5> old_cause{};
        // FAST_CPU_GUARD_VERIFY only (the old_* causes too): refused by both guards; refused by
        // the old guard only (by the causes above), of those GPU-written when checked, and the
        // rest copied for B to compare (probes); accepted by the old guard only: refused by the
        // new rule for bytes a queued write lands on (never expected), or for a page it marks.
        u64 both{}, freed{}, freed_gpu_dirty{}, probes{}, stricter{}, stricter_page{};
        std::array<u64, 5> freed_cause{};
    } guard_stats;
    // A-only CE dump state (bit 16). Reads are noted only when they touch the box, the
    // union of dumped ranges. The box grows only through a full drain, so every queued
    // packet was encoded against the current box.
    struct CeState {
        FastRender::GranuleTickets reads;
        u64 lo{}, hi{};
        u64 untracked{}; // newest queued packet/task whose guest reads are not listed
        bool mode{}, overflow{};
        u64 dumps{}, skipped{}, waited{}, wait_ns{}, untracked_waits{};
        u64 full_off{}, full_mode{}, full_box{}, full_guard{}, notes{}, untracked_packets{};
        std::array<u64, 6> untracked_by{};
        // FAST_CPU_GUARD_CE_VERIFY (CeVerifyTargeted): dumps checked; what their full drain
        // changed after the targeted wait's point: the dumped guest bytes, the GPU-modified state
        // of their pages, queued readers (FAST_DRAIN_AUDIT=1) and unlisted ones; and mismatches
        // (any of these).
        u64 verified{}, bytes_changed{}, pages_marked{}, readers{}, unknown_readers{},
            mismatches{};
    } ce;
    static constexpr u64 kCeBoxLimit = 4ull << 20;
    // FAST_CPU_GUARD_CE_VERIFY: before the full drain of a dump the new rule would have let take
    // the targeted wait, that wait, then the comparison (A only).
    void CeVerifyTargeted(VAddr address, u64 bytes);
    // B-only validation counters (one declaration each: alignas applies to every declarator).
    alignas(64) u64 constant_bind_hits{};
    u64 constant_pair_hits{};
    // FAST_CPU_GUARD, B only (with the counters above, off A's lines): dropped constants (image
    // downloads) and verify comparisons, by group: accepted by the new rule only, by both, by the
    // old guard only (CheckGuardConstants).
    struct GuardChecks {
        u64 packets{}, download_packets{}, dropped_packets{}, dropped{};
        std::array<u64, 3> checked{}, differ{}, cached{}, direct{}, rebind{};
    } guard_checks;
    VideoCore::DownloadLogReader guard_downloads; // B only
    void CheckGuardConstants(DrawPacket& packet);
    DrawPacket* binding_packet{}; // B only, scoped to ConsumePacket.
    void PrepareConstants(DrawPacket& packet, bool enabled);
    bool UpdateDrawWorker();
    void ConsumePacket(DrawPacket& packet);
    struct PreparedVrDraw {
        const VrCapeTable::Range* split{};
        u32 split_count{};
        const VrBodyTable::Key* body_split{};
        const GraphicsPipeline* body_cut_pipeline{};
        bool body_cape_hidden{}, body_head_hidden{}, body_shoulder_cut{};
        const VrHandsTable::Range* hands_split{};
        u32 hands_split_count{}, cut_code{};
        bool probe_selected{};
    };
    alignas(64) PreparedVrDraw producer_vr{};  // Stage A (PrepareVrDraw), a line of its own
    alignas(64) PreparedVrDraw recording_vr{}; // Stage B (ConsumePacket), like what follows
    // Native stereo, recording thread: the eye images' size (the last world draw's target), and the
    // workgroups the next dispatch needs for each eye's pixel rectangle (BindBuffers:
    // NativeStereo::ScreenRectBuffer; 0: its own).
    std::array<u32, 2> vr_eye_extent{};
    std::array<u32, 2> vr_rect_groups{};
    // Native stereo, recording thread: a planar reflection's pass (FAST_GPU_MIRROR_FLIP,
    // NativeProjection::MirrorPass). BindBuffers notes each graphics draw's scene camera
    // (VrMirrorCamera); the draws of the pass pair their lighting, and push their projection and
    // light tiles (VrNativeDrawValues), with each eye's frustum upside down, as its mirrored
    // camera got it. VrMirrorFlip: the switch, and a device whose draws push their projection
    // (a baked permutation cannot follow the pass, so nothing is flipped without one).
    // A draw's camera is noted once, its first in binding order; the fragment stage binds first
    // and a light quad binds its light before its camera, so VrMirrorAhead notes it from the
    // pipeline BindResources binds (`vr_mirror_pipeline`) before a fragment light is paired
    // (`vr_mirror_looked`: it did, for this draw).
    NativeProjection::MirrorPass vr_mirror{};
    const Pipeline* vr_mirror_pipeline{};
    bool vr_mirror_logged{}, vr_mirror_noted{}, vr_mirror_looked{};
    bool VrMirrorFlip() const;
    bool VrInMirrorPass() const;
    void VrMirrorCamera(bool mirrored, VAddr camera);
    void VrMirrorAhead();
    // Native stereo, recording thread, the draw being recorded (VrNativeDrawValues, from the
    // camera record of its other VR values): its light tile in each eye when it is a light or
    // glow quad with a tile of its own (the hardware scissor is their union,
    // UpdateViewportScissorState), and what Pipeline::BindResources pushes after push_data
    // (Shader::VrPushData: its projection, light tiles, cut and motion buffers too, when the
    // device has room).
    struct VrLightTiles {
        bool valid{};
        std::array<std::array<float, 4>, 2> bounds{};
    } vr_light_tiles{};
    Shader::VrPushData vr_push{};
    void VrNativeDrawValues(const GraphicsPipeline* pipeline, const VR::CameraRecord& record);
    bool PrepareVrDraw(const GraphicsPipeline*& pipeline, bool indexed, bool indirect, u32 offset);
    const GraphicsPipeline* ReselectVrCut(const GraphicsPipeline* pipeline);
    // FAST_CPU_VIEW_PREDICT (PipelineCache::GetDrawPipeline), the recording thread in native
    // stereo. VrViewPredicted: Stage A selected `pipeline` with its predicted view mask 0 (the
    // only way it selects 0 there) and the targets want 0, so it is drawn without the reselect;
    // with FAST_CPU_VIEW_PREDICT_VERIFY=1 the reselect runs too and decides on a mismatch.
    // VrViewReselected: the draw is reselected (`mispredicted`: Stage A's 0 where they want 3).
    const GraphicsPipeline* VrViewPredicted(const GraphicsPipeline* pipeline);
    void VrViewReselected(bool mispredicted);
    enum class PacketKind { Draw, DrawIndirect, Dispatch, DispatchIndirect, FilteredDraw };
    void RecordPacket(std::unique_lock<std::recursive_mutex> selection_lock,
                      const Pipeline* pipeline, PacketKind kind, bool indexed = false,
                      VAddr address = 0, u32 offset = 0, u32 size = 0, u32 count = 0,
                      VAddr count_address = 0);
    void DrawRecord(const GraphicsPipeline* pipeline, bool is_indexed, u32 index_offset);
    void DrawIndirectRecord(const GraphicsPipeline* pipeline, bool is_indexed, VAddr arg_address,
                            u32 offset, u32 stride, u32 max_count, VAddr count_address);
    void DispatchDirectRecord(const ComputePipeline* pipeline);
    void DispatchIndirectRecord(const ComputePipeline* pipeline, VAddr address, u32 offset,
                                u32 size);
    /// FAST bit 53 (vk_dispatch_eye_overlap.h): the dispatch's check before its pairing, or null
    /// (bit off, no native stereo): then the old command stream.
    EyeOverlap::Check* EyeOverlapBegin(const Shader::Info& cs);
    /// After the pairing: true when the middle eye barrier is left out (counted either way).
    bool EyeOverlapTake(EyeOverlap::Check* check, vk::Buffer left_args = {},
                        vk::Buffer right_args = {});
    /// FAST_DISPATCH_EYE_OVERLAP_VERIFY: records both eyes twice (see the header); false when
    /// this dispatch is not sampled (nothing recorded).
    template <typename RecordEye>
    bool EyeOverlapVerify(const Shader::Info& cs, const RecordEye& record_eye);
    /// FAST_GPU_PASS_PROFILE (bit 38): labels the game render pass `state` begins.
    void LabelGpuPass(GpuPassProfiler& profiler, const GraphicsPipeline* pipeline,
                      const RenderState& state);
    /// FAST bit 54 (SkipNoopDraws, vk_rasterizer.cpp): true when this draw's render pass,
    /// pipeline bind and draw are left out because they cannot write any memory.
    bool SkipNoopDraw(const GraphicsPipeline* pipeline, const RenderState& state, bool is_indexed,
                      bool ui_shadow);
    /// FAST_SKIP_NOOP_DRAWS_VERIFY: the independent re-derivation; null when it agrees.
    const char* NoopDrawRecheck(const GraphicsPipeline* pipeline, const RenderState& state) const;
    void ReportNoopDraws();
    // Recording thread only (like target_memo_stats).
    struct NoopDrawStats {
        u64 eligible{}, skipped{}, verify_tick{}, verified{}, mismatches{};
        std::array<u64, 7> fallback{}; // by NoopDrawFallback (static_assert in SkipNoopDraw)
        u64 report_frames{}, report_skipped{};
    } noop_draw_stats{};
    std::vector<const GraphicsPipeline*> noop_draw_denied;  // verify mismatches: never skipped
    std::vector<std::array<u32, 6>> noop_draw_logged;       // the once-per-key register log

    std::unique_ptr<NativeAudit> native_audit;
    void VrNativeAudit(const Pipeline* pipeline, const RenderState* state);
    void VrNativeAuditEnd();
    void PrepareRenderState(const GraphicsPipeline* pipeline);
    RenderState BeginRendering(const GraphicsPipeline* pipeline);
    RenderState BeginRenderingFull(const GraphicsPipeline* pipeline);
    struct BeginMemo;
    std::unique_ptr<BeginMemo> begin_memo;
    // FAST_CPU_PASS_MEMO (S9, vk_rasterizer.cpp): the begin-state memo keyed by what
    // BeginRenderingFull reads (bbport RenderStateMemo), and a lighter draw setup around it.
    struct PassMemo;
    std::unique_ptr<PassMemo> pass_memo;
    RenderState BeginRenderingPassMemo(const GraphicsPipeline* pipeline);
    RenderState PassMemoVerifyHit(const GraphicsPipeline* pipeline);
    /// pipeline_cache.NoteTargetViews with FAST_CPU_PASS_MEMO's repeat skip (and its verify).
    void NoteTargetViews(u64 targets, u32 desired);
    void Resolve();
    void DepthStencilCopy(bool is_depth, bool is_stencil);
    void EliminateFastClear();

    void UpdateDynamicState(const GraphicsPipeline* pipeline, bool is_indexed) const;
    struct DynSkip; // Recording-owner only: memo of the Update*State results (bit 29).
    std::unique_ptr<DynSkip> dyn_skip;
    void UpdateViewportScissorState(const GraphicsPipeline* pipeline) const;
    void UpdateDepthStencilState() const;
    void UpdatePrimitiveState(bool is_indexed) const;
    void UpdateRasterizationState() const;
    void UpdateColorBlendingState(const GraphicsPipeline* pipeline) const;

    u32 recording_view_mask{}; // B-owned; never participates in A pipeline selection.
    bool IsRegularDraw() const;
    void AssertRecordingOwner() const;
    bool FilterDraw();

    void BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                     Shader::PushData& push_data);
    // BindTextures' parts shared by the full path and the bind memo's path (perf_bind_memo).
    void BindSamplers(const Shader::Info& stage, Shader::Backend::Bindings& binding);
    void TransitTexture(VideoCore::Image& image, bool is_storage,
                        const VideoCore::SubresourceRange& range);
    void BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding);
    /// vr.ini gpu_submit_ms: submits the command buffer recorded so far when it has recorded
    /// that long and the next draw switches the render pass (never inside a pass).
    void MaybeSubmitMidFrame(const RenderState& next_state);
    /// The texture cache's description of a T# bound through `res` (ImageInfo + ImageViewInfo:
    /// a pure function of the T#'s 256 bits and the resource's is_written/is_depth/is_array).
    const VideoCore::TextureCache::ImageDesc& TextureDescFor(const AmdGpu::Image& tsharp,
        const Shader::ImageResource& res, const u64* prepared_hash = nullptr);
    bool BindResources(const Pipeline* pipeline);

    void ResetBindings() {
        for (auto& image_id : bound_images) {
            texture_cache.GetImage(image_id).binding = {};
        }
        bound_images.clear();
    }

    // VR: VRDRAW debug line (VR_DEBUG_DRAWS) and the DIBR capture around each draw.
    void VrLogDraw(const GraphicsPipeline* pipeline, const RenderState& state, float vr_shift,
                   float vr_hud);
    std::shared_ptr<ForegroundProbe> foreground_probe;
    bool probe_selected{};
    const GraphicsPipeline* probe_pipelines[2]{};
    void VrProbePrepare(const GraphicsPipeline* pipeline);
    void VrProbeDraw(const GraphicsPipeline* pipeline, const RenderState& state,
                     s32 vertex_offset, u32 instance_offset);
    void VrProbeEndFrame();
    void VrBeforeDraw(const GraphicsPipeline* pipeline);
    void VrAfterDraw(const GraphicsPipeline* pipeline, const RenderState& state);
    // `depth`: also the scene depth candidates (DIBR frames).
    void VrTrackTargets(const GraphicsPipeline* pipeline, const RenderState& state, bool depth);
    // The camera update the frame's main-view draws really use: the one whose render position and
    // view direction its vertex shader's constants hold (the frame's first draw can come after the
    // game thread already placed the next camera). Tags the frame with it (VR::SetFrameRecord).
    void VrMatchCamera(const GraphicsPipeline* pipeline, const RenderState& state);
    bool VrCaptureDepth(vk::Extent2D scene, bool allow_dlss_resize = true);
    // In-frame items (renderer_vulkan/vr_inframe.h, core/vr/inframe.h): drawn into the scene
    // colour with a private depth copy right after the composite, before the DIBR capture (which
    // then reads that copy). True: drawn this frame.
    bool VrInjectInFrame();
    void VrDibrCaptureAtMarker();
    // UI layer: arming at the composite, the per-draw decision and the K/W passes.
    void VrUiArm(s32 mode);
    bool VrUiPrepare(const GraphicsPipeline* pipeline, const RenderState& state);
    // One recorded draw call of the current draw (replayed into K and W).
    struct VrDrawCall {
        const void* context;
        void (*call)(const void*, RecorderProxy);
        void operator()(RecorderProxy cmdbuf) const {
            call(context, cmdbuf);
        }
        template <typename F>
        static VrDrawCall Of(const F& f) {
            return {&f, [](const void* c, RecorderProxy cmdbuf) {
                        (*static_cast<const F*>(c))(cmdbuf);
                    }};
        }
    };
    // `after_draw` (FAST_GPU_PASS_MERGE, vk_pass_merge.h): the game's own draw into X is already
    // recorded (VrUiShadowAfter said so); the pair then goes K, W.
    void VrUiShadowDraw(const GraphicsPipeline* pipeline, const RenderState& state,
                        VrDrawCall issue, bool after_draw);
    // FAST_GPU_PASS_MERGE: whether this UI draw goes into X first, its shadows after (its X pass
    // open, nothing of the shadows reads what the draw writes); counts and verifies the decision.
    bool VrUiShadowAfter(const GraphicsPipeline* pipeline, const RenderState& state);
    void ReportPassMerge();
    // FAST_GPU_PASS_MERGE, the begin-state memo (bit 2): the open pass is the shadow pair a
    // swapped UI draw began right after its X pass (the memo's state), nothing begun since.
    [[nodiscard]] bool VrUiMemoResumes() const;
    // ... whether the memo (BeginMemo, or FAST_CPU_PASS_MEMO's PassMemo) is valid and holds
    // `state` (vk_rasterizer.cpp, where both are defined).
    [[nodiscard]] bool BeginMemoHolds(const RenderState& state) const;
    // ... and FAST_GPU_PASS_MERGE_VERIFY on such a memo hit: the full path's state when it
    // differs (logged), else none.
    std::optional<RenderState> VrUiMemoVerify(const GraphicsPipeline* pipeline,
                                              const RenderState& state);
    void VrUiNoteDispatch();
    [[nodiscard]] std::string VrUiLogTag() const;
    // VR draw filter (vk_rasterizer_vr_draws.cpp): the player's cape in first person (vr.ini
    // hide_cape) and the VR_DRAW_CTL inspector. True = drop this draw.
    bool VrFilterDraw(const GraphicsPipeline* pipeline, bool is_indexed, bool indirect,
                      u32 index_offset);
    bool VrCapeSkip(const GraphicsPipeline* pipeline, u32 index_offset);
    // The inspector's log of a compute dispatch (vk_rasterizer_vr_draws.cpp).
    bool VrLogDispatch(const ComputePipeline* pipeline, bool indirect);
    // Fast bit 40's skip rule in native stereo (vk_pipeline_skip.h), Stage A, asked only for a
    // draw the rule would leave out (`vs`: its vertex program, may be null): Frame outside an
    // immersive frame, Ui after this frame's composite or without one in the frame before
    // (vr_skip_ui_frame: the frame counter when Draw selected it), Player for the hunter's own
    // draw, else None.
    PipelineSkip::Veto VrSkipVeto(const Shader::Info* vs);
    // Stage A, each draw's selected pipeline: the composite (VR::Settings::hud_after_vs) starts
    // the UI phase. Asked again only when the pipeline changes (most draws repeat the last one).
    void VrSkipNoteDraw(const GraphicsPipeline* pipeline) {
        if (!pipeline_cache.VrSkipVetoes()) {
            return;
        }
        if (pipeline != vr_skip_last_pipeline) {
            vr_skip_last_pipeline = pipeline;
            vr_skip_composite = VrIsComposite(pipeline);
        }
        if (vr_skip_composite) {
            vr_skip_ui_frame = FastRender::g_counters.frames.load(std::memory_order_relaxed);
        }
    }
    // Its vertex program is the composite's (hud_after_vs, read once a frame).
    bool VrIsComposite(const GraphicsPipeline* pipeline);
    // Stage A, every draw: off the line of B's probe_selected before them.
    alignas(64) const GraphicsPipeline* vr_skip_last_pipeline{};
    bool vr_skip_composite{};
    u64 vr_skip_ui_frame{~u64{0}};
    u64 vr_skip_hud_vs{};
    u64 vr_skip_hud_frame{~u64{0}};
    u64 vr_skip_ui_missing{~u64{0}}; // the first candidate frame without a composite before it
    bool vr_skip_ui_logged{};
    // VR body cutaway: the pipeline this draw uses instead of `pipeline` (the permutation whose
    // fragment shader fades out the hunter's own body near the first-person camera), or
    // `pipeline` itself. Called right after VrFilterDraw passed the draw.
    const GraphicsPipeline* VrCutPipeline(const GraphicsPipeline* pipeline);
    // Records the indexed draw VrFilterDraw just passed without its cape triangles (several
    // drawIndexed of the kept ranges). False = record it whole as usual.
    bool VrDrawCapeSplit(RecorderProxy cmdbuf, u32 instances, s32 vertex_offset,
                         u32 instance_offset);
    void VrDrawsEndFrame();
    // vr.ini aa_mode (vk_rasterizer_vr_aa.cpp, core/vr/aa_mode.h): VrFilterDraw asks what to do
    // with each direct draw (the game's FXAA draw: draw it, skip it, or SMAA in its place);
    // VrAaReplace runs SMAA once the draw's images are bound. True = the FXAA draw is done (not
    // to be drawn); false = draw it as the game does.
    u8 VrAaDecide(const GraphicsPipeline* pipeline, bool indirect);
    bool VrAaReplace(const GraphicsPipeline* pipeline, const RenderState& state);
    void VrAaNote(u8 outcome, const std::string& detail);
    // aa_mode=taa: the render TAA's resolve from B into A (both bound and transitioned by
    // VrAaReplace). False (`why`): nothing recorded, VrAaReplace uses SMAA.
    bool VrTaaReplace(VideoCore::Image& b, VideoCore::Image& a, std::string* why);
    void VrTaaNoteRefusal(const std::string& why);

    bool IsComputeMetaClear(const Pipeline* pipeline);
    bool IsComputeImageCopy(const Pipeline* pipeline);
    // VR_PERF_PROBE: one log line per (shader, reason) the copy HLE refused (vr_perf.h).
    void ProbeCopyReject(const Shader::Info& info, u32 threads_x, u32 dim_x, const char* why);
    bool IsComputeImageClear(const Pipeline* pipeline);
    // FAST bit 55 or VR_PERF_PROBE: one log line per (shader, reason) the image clear refused.
    void ProbeClearReject(const Shader::Info& info, const char* why);
    // FAST bit 55 (ComputeClearFill, vk_rasterizer.cpp): the game's clear shader cs 8b355b5a as
    // fills of its destination buffer when IsComputeImageClear refused it. True = recorded.
    bool IsComputeClearFill(const Pipeline* pipeline);
    // FAST_COMPUTE_CLEAR_FILL_VERIFY: reads back what the sampled dispatch wrote after it ran,
    // restores the windows' guards and compares on the host (a deferred operation).
    void FinishClearFillVerify();
    // A FAST_COMPUTE_CLEAR_FILL_VERIFY sample: IsComputeClearFill poisoned, filled and read back
    // these windows and poisoned them again; the game's dispatch runs next. Recording owner.
    struct ClearFillSample {
        struct Region {
            vk::Buffer buffer{};
            u64 offset{}, bytes{}; // a window of `buffer` (an eye's copy of the destination)
            u32 slot{};            // its place in each phase of the readback
            bool right{};
        };
        std::array<Region, 4> regions{};
        u32 count{};
        u64 inside_begin{}, inside_end{}; // the filled bytes (the same offsets in both eyes)
        u32 word{}, poison{};
        bool pending{};
    };
    // B's, from a line of their own (vr_skip_* above are A's). These three also leave `instance`
    // and `scheduler` below a line of their own (168 + 16 + 1 bytes, padded to 192).
    alignas(64) ClearFillSample clear_fill_sample;
    std::shared_ptr<VideoCore::Buffer> clear_fill_readback; // its host readback, made on first use
    bool recording_direct_dispatch{}; // DispatchDirectRecord is binding its resources

private:
    friend class VideoCore::BufferCache;

    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::PageManager page_manager;
    VideoCore::BufferCache buffer_cache;
    VideoCore::TextureCache texture_cache;
    AmdGpu::Liverpool* liverpool;
    Core::MemoryManager* memory;
    boost::icl::interval_set<VAddr> mapped_ranges;
    // Its reader count is written by every IsMapped (B's page tracking, guest faults): off the
    // line of `liverpool` and `memory`, which A reads for every draw.
    alignas(64) Common::SharedFirstMutex mapped_ranges_mutex;
    PipelineCache pipeline_cache;
    alignas(64) const PreparedDraw* prepared_draw{}; // Stage A only; consumed by the next Draw.
    u64 prepared_checksum{};
    bool prepared_set{};
    // Bit 32, Stage A only: the validated bit 31 hit of the draw RecordPacket builds next, set
    // only when that draw records the hit's own pipeline (consumed by RecordPacket).
    const PreparedDraw* packet_prepared{};

    using RenderTargetInfo = std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc>;
    alignas(64) std::array<RenderTargetInfo, AmdGpu::NUM_COLOR_BUFFERS> cb_descs; // B, per draw
    std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc> db_desc;
    boost::container::static_vector<vk::DescriptorImageInfo, Shader::NUM_IMAGES> image_infos;
    boost::container::static_vector<vk::DescriptorBufferInfo, Shader::NUM_BUFFERS> buffer_infos;
    boost::container::static_vector<VideoCore::ImageId, Shader::NUM_IMAGES> bound_images;
    // Fast bit 25 (native stereo): right-eye handles carried through binding. buffer_hints and
    // view_hints run parallel to buffer_infos / image_infos (samplers leave empty hints),
    // barrier_hints to BindBuffers' buffer_barriers. Decided once per BindResources.
    bool stereo_handles{};
    // FAST bit 53, recording thread: a draw wrote buffers, raw memory or storage images since the
    // last game dispatch's closing eye barrier (kept whenever native stereo is on, whatever the
    // bit).
    bool eye_unordered_writes{true};
    EyeOverlap::Check eye_check;
    boost::container::static_vector<NativeStereo::BufferHint, Shader::NUM_BUFFERS> buffer_hints;
    boost::container::static_vector<NativeStereo::ViewHint, Shader::NUM_IMAGES> view_hints;
    boost::container::static_vector<NativeStereo::BufferHint, Shader::NUM_BUFFERS> barrier_hints;
    NativeStereo::Handles native_handles{};
    /// The hint of the image info just added (a rollback before it truncates the hints too).
    void NoteViewHint(vk::ImageView left, vk::ImageView right) {
        view_hints.resize(image_infos.size() - 1);
        view_hints.push_back({left, right});
    }
    /// Pipeline::BindResources' handles: null unless this draw's binding carried them.
    const NativeStereo::Handles* NativeHandles();

    u32 set_write_index{};
    Pipeline::DescriptorWrites set_writes;
    Pipeline::BufferBarriers buffer_barriers;
    Shader::PushData push_data;

    using BufferBindingInfo = std::tuple<VideoCore::BufferId, AmdGpu::Buffer, u64>;
    boost::container::static_vector<BufferBindingInfo, Shader::NUM_BUFFERS> buffer_bindings;
    using ImageBindingInfo = std::pair<VideoCore::ImageId, VideoCore::TextureCache::ImageDesc>;
    boost::container::static_vector<ImageBindingInfo, Shader::NUM_IMAGES> image_bindings;
    // TextureDescFor's memo: the same T# is bound draw after draw, and building its ImageInfo
    // (format lookups, the mip layout) every time was ~5% of the command processor.
    struct TextureDescMemo {
        std::array<u64, 4> tsharp{};
        u32 flags{};
        bool valid{};
        VideoCore::TextureCache::ImageDesc desc{};
    };
    static constexpr size_t TextureDescMemoSize = 512;
    std::array<TextureDescMemo, TextureDescMemoSize> texture_desc_memo{};
    // Independent value cache: switching modes cannot retain stale resource handles.
    // 2048 sets x 2 ways = 4096 entries; all descriptor words and flags are compared.
    static constexpr size_t TextureDescSets = 2048;
    std::array<std::array<TextureDescMemo, 2>, TextureDescSets> texture_desc_two_way{};
    std::array<u8, TextureDescSets> texture_desc_victim{};
    // perf_bind_memo (vk_perf_memo.h): a stage's image bindings by their T#s. Per binding: the
    // image (after the stencil -> depth redirect) and FindImage's own result, the view and the
    // backing it belongs to, the subresource range its transition uses (null image = a null
    // descriptor). An entry is used only while the texture cache's registry epoch is the one it
    // was made in (no image registered or unregistered since: every FindImage gives the same),
    // every image keeps that backing (the same view) and the same depth link, and none needs a
    // rebind; the per-draw work (UpdateImage: tracking, LRU, a CPU-dirty image's upload; the
    // transitions; the binding flags) still runs for every binding.
    struct BindMemoBinding {
        VideoCore::ImageId image_id{};
        VideoCore::ImageId raw_id{};
        vk::ImageView view{};
        // Bit 25: the view's native stereo right (lives as long as `view`); null: not known
        // yet (made with the bit off), filled on its first use with the bit on.
        mutable vk::ImageView right{};
        const void* backing{};
        VideoCore::SubresourceRange range{};
    };
    using BindMemo = PerfMemo::BindTable<BindMemoBinding, 10>;
    BindMemo bind_memo{};
    PerfMemo::BindKey bind_key{};
    PerfMemo::Counters bind_counters{};
    u64 bind_uncacheable{}; // stages that cannot be remembered (see BindTextures)
    u64 bind_invalid{};     // found, but an image changed (backing, depth link, rebind)
    bool BindMemoValid(const BindMemo::Entry& entry);
    // perf_copy_memo (vk_perf_memo.h CopyTable, ObtainCopyBuffers).
    PerfMemo::CopyTable<10> copy_memo{};
    PerfMemo::Counters copy_counters{};
    u64 copy_unmemo{};       // batches that cannot be remembered (the stream buffer, untracked)
    u64 copy_inconclusive{}; // VR_PERF_CHECK: a page changed state during the check (a race)
    // VR_PERF_STATS / VR_PERF_CHECK: BindTextures' wall time by the bind memo's state ([0] off,
    // [1] on) in the current 5 s window (BindResources, logged by PerfRefresh).
    struct BindTime {
        u64 ns{};
        u64 calls{};
    };
    std::array<BindTime, 2> bind_time{};
    void BindTexturesFromMemo(const BindMemo::Entry& entry, Shader::Backend::Bindings& binding);
    // FAST bit 26: the bind memo's lean hit path (bbport vk_rasterizer.cpp:3593-3677). False
    // (nothing bound) when an image changed; the caller then runs the full path.
    bool BindTexturesFromMemoFast(const BindMemo::Entry& entry,
                                  Shader::Backend::Bindings& binding);
    struct BindMemoHitStats {
        u64 hits{};
        u64 misses{};     // keyed stages without a current entry (the full path ran)
        u64 invalid{};    // found, but an image changed (also counted in bind_invalid)
        u64 lru_locks{};  // TouchImageOnce's real LRU touches: once per image per GC period
        u64 verified{};   // FAST_BIND_MEMO_HIT_VERIFY samples
        u64 mismatches{}; // ... that disagreed with the slow way
    };
    BindMemoHitStats bind_memo_hit{}; // recording thread only
    // FAST_CPU_TEX_MEMO: the native stereo right of a remembered binding's view, for its view
    // hint on the lean hit path (the entry's, else one registry lookup kept in the entry).
    // `verify`: FAST_CPU_TEX_MEMO_VERIFY, read once by the caller.
    vk::ImageView TexMemoRight(const BindMemoBinding& b, bool verify);
    struct TexMemoStats {
        u64 stages{};     // lean memo hits that noted their views' hints (logs only)
        u64 hints{};      // rights carried in the entry (logs only)
        u64 resolved{};   // ... not known yet: one registry lookup, then kept (logs only)
        u64 verified{};   // FAST_CPU_TEX_MEMO_VERIFY: rights also looked up in the registry
        u64 mismatches{}; // ... that disagreed (the registry's answer was used)
    };
    TexMemoStats tex_memo{}; // recording thread only
    // FAST_CPU_FAULT_WINDOW_VERIFY: InvalidateWriteFault's work, compared and counted.
    void VerifyWriteFault(VAddr addr, VAddr start, u64 window, bool window_mapped, bool first);
    bool fault_process_pending{};
    bool attachment_feedback_loop{};
    u32 midframe_submits{}; // since the last flip (VR_STREAM_STATS / the gpu frame log)
    // FAST bit 62 (MaybeSubmitMidFrame): the 'FAST submit_cadence' line, recording thread only.
    struct SubmitCadenceStats {
        u64 frames{}, submits{}, midframe{};
        u64 old_cuts{}, old_idle_known{}; // bit off: the gpu_submit_ms cuts; GPU known idle
        u64 idle{}, cap{}, signals{};     // bit on: cuts by reason
        u64 deferred{}, polls{};          // bit on: cuts left for later; semaphore queries
        u64 verified{}, verify_idle{};    // FAST_SUBMIT_CADENCE_VERIFY samples; ... GPU idle
        u64 verify_sequence{};
    } submit_cadence{};
    u64 submit_count_mark{};
    std::chrono::steady_clock::time_point cadence_poll{};
    void ReportSubmitCadence();

    // Per-draw CPU shortcuts (vk_perf_memo.h; vr.ini perf_spec_memo / perf_rt_memo /
    // perf_bind_memo / perf_copy_memo, VR_TUNE_FILE perf.*): every 256 draws/dispatches the switches are read again (at most every 250 ms) and,
    // with VR_PERF_CHECK=1 or VR_PERF_STATS=1, every 5 s a 'VR perf check:' line is logged.
    void PerfTick() {
        if ((++perf_tick & 255u) == 0) {
            PerfRefresh();
        }
    }
    void PerfRefresh();
    // VR_PERF_CHECK=1: a remembered render-target description against the one built afresh.
    void PerfCheckDesc(const char* what, u32 index, const VideoCore::TextureCache::ImageDesc& memo,
                       const VideoCore::TextureCache::ImageDesc& fresh);
    u32 perf_tick{};
    s64 perf_refreshed_ms{};
    s64 perf_logged_ms{};
    // perf_rt_memo: the image description of each colour target and of the depth target (a pure
    // function of the CB / DB registers and the extent hint) of the draws before, by their bytes.
    static constexpr size_t CbDescKeySize = sizeof(AmdGpu::ColorBuffer) + sizeof(AmdGpu::CbDbExtent);
    static constexpr size_t DbDescKeySize = sizeof(AmdGpu::DepthBuffer) + sizeof(AmdGpu::DepthView) +
                                            sizeof(AmdGpu::DepthControl) + sizeof(VAddr) +
                                            sizeof(AmdGpu::CbDbExtent);
    std::array<PerfMemo::BytesMemo<CbDescKeySize, VideoCore::TextureCache::ImageDesc>,
               AmdGpu::NUM_COLOR_BUFFERS>
        cb_desc_memo{};
    PerfMemo::BytesMemo<DbDescKeySize, VideoCore::TextureCache::ImageDesc> db_desc_memo{};
    PerfMemo::Counters rt_counters{};
    // FAST bit 27 (bbport FindTargetMemoized, vk_rasterizer.cpp:591-644, and LastTarget,
    // vk_rasterizer.h:518-526): per target slot, the image FindImage gave for these description
    // bytes, used while the texture cache's registry epoch is the one it was found in. Only
    // perfect matches are kept (no overlap resolved, `desc` not moved to a subresource), so the
    // slot's description (cb_descs / db_desc) is still this key's and is neither rebuilt nor
    // copied; every path that rebuilds it without storing drops the entry.
    template <size_t N>
    struct TargetMemo {
        std::array<unsigned char, N> key{};
        u64 epoch{}; // 0: empty (the registry epoch starts at 1)
        VideoCore::ImageId image_id{};
    };
    std::array<TargetMemo<CbDescKeySize>, AmdGpu::NUM_COLOR_BUFFERS> cb_target_memo{};
    TargetMemo<DbDescKeySize> db_target_memo{};
    bool target_memo_armed{}; // the bit at the last PrepareRenderState
    struct TargetMemoStats {
        u64 hits{}, misses{}, stores{}, verified{}, mismatches{}, verify_tick{}, draws{};
    } target_memo_stats{};
    template <size_t N, typename MakeDesc>
    VideoCore::ImageId FindTargetMemoized(TargetMemo<N>& memo, const PerfMemo::KeyBytes<N>& key,
                                          VideoCore::TextureCache::ImageDesc& desc,
                                          MakeDesc&& make_desc);
    // Statistics only (VR_PERF_CHECK / VR_PERF_STATS): draws whose pipeline and every stage's
    // flattened user data (its sharps) equal the draw before's - what a "same resources" skip of
    // BindResources could reuse at most.
    std::vector<u32> perf_prev_res;
    std::vector<u32> perf_cur_res;
    const void* perf_prev_pipeline{};
    u64 perf_res_same{};
    u64 perf_res_total{};

    // VR DIBR capture state of the frame being drawn (GPU thread). Images are kept by id and uid
    // (ids are recycled) and never across frames.
    struct VrDepthCandidate {
        VideoCore::ImageId id{};
        u64 uid{};
        u32 draws{};   // depth-writing scene draws this frame
        float zmin{0.0f};
        float zmax{1.0f};
        bool zero_to_w{true}; // clip space z 0..W (the DIBR math assumes it)
    };
    // The last colour target of each size: the composite's own target, or (Bloodborne: the
    // composite is a stencil pass) the last scene-sized target before it, is the scene colour X.
    struct VrColorTarget {
        VideoCore::ImageId id{};
        u64 uid{};
        vk::Extent2D extent{};
        vk::ComponentMapping components{}; // undoes the colour buffer's component swap
        u32 last_draw{};
    };
    struct VrDibrFrame {
        bool dlss{};
        vk::Extent2D depth_output_extent{};
        bool captured{};   // the composite was seen (captured or refused)
        bool depth_done{}; // depth captured before a composite that writes depth
        u32 draws{};
        std::array<VrDepthCandidate, 4> depth{};
        std::array<VrColorTarget, 4> color{};
        vk::Extent2D depth_extent{}; // of the captured depth
        VideoCore::ImageId color_id{};
        u64 color_uid{};
        vk::ComponentMapping color_components{};
        u64 first_draw_record{}; // VR::DrawFrameRecord at the frame's first draw (its update)
        u64 composite_record{};  // ... at the composite (often the next frame's; test stamps)
        bool inframe{};          // VrInjectInFrame drew: the scene depth is its private copy
        bool inframe_checked{};  // AER / mono: whether this frame has in-frame items, decided
        bool inframe_wanted{};   // ... (at its first draw)
        // VrMatchCamera: the camera update found in the main view's constants (0 = not yet), and
        // how many main-view draws were looked at.
        u64 matched_record{};
        u32 match_tries{};
        bool taa_resolved{}; // aa_mode=taa resolved this frame: its stable depth is the DIBR's
    } vr_dibr;
    // VrMatchCamera's statistics, logged every 600 frames.
    struct VrCameraMatchStats {
        u32 matched{};
        u32 unmatched{};
        std::array<u32, 6> offsets{}; // the update used minus the first draw's: <=-3 .. +2
        u32 ambiguous{};
    } vr_camera_match{};
    std::unique_ptr<VrInFrame> vr_inframe; // created at the first composite in VR
    std::unique_ptr<VrInFrame> vr_inframe_right; // native eye has its own depth and lighting probe
    // vr.ini aa_mode (vk_rasterizer_vr_aa.cpp): what VrFilterDraw decided for the current draw,
    // the SMAA passes (created at the first FXAA draw with aa_mode=smaa) and the log's state.
    u8 vr_aa_action{};
    std::unique_ptr<VrSmaa> vr_smaa;
    bool vr_temporal_enabled{}, vr_temporal_scaled{};
    std::array<float,2> vr_scene_inverse{1.f,1.f};
    std::optional<RenderState> VrTemporalRenderState(const GraphicsPipeline* pipeline);
    u64 vr_temporal_frame{};
    std::unique_ptr<VrTemporalCapture> vr_temporal;
    void VrTemporalBeforeDraw(const GraphicsPipeline* pipeline, bool indirect,
                              bool indexed = false, u32 index_offset = 0);
    const GraphicsPipeline* VrTemporalSelect(const GraphicsPipeline* pipeline, RenderState& state);
    // The motion buffers of the variant VrTemporalSelect last returned (recording thread): with
    // VrPushData the draw pushes their addresses (VrNativeDrawValues), the variant bakes none.
    Shader::VrMotion vr_motion_buffers{};
    u32 VrTemporalPrepareDraw(const GraphicsPipeline* pipeline, u64 geometry, bool indexed,
                             u32 index_offset, u32 vertex_offset, u32 first_instance);
    void VrTemporalDispatch(u64 shader);
    void VrS1Reconstruct(); // S1 (FAST bit 51): the provider after the composite
    void VrTemporalEndFrame();

    struct VrAaLog {
        u8 last{0xFF};     // the last outcome logged (VrAaOutcome)
        u32 lines{0};
        u64 counts[5]{};   // FXAA draws drawn / skipped / replaced by SMAA / refused / by TAA
    } vr_aa_log{};
    // aa_mode=taa (vk_rasterizer_vr_aa.cpp): the resolve, the last resolved frame (its camera,
    // depth map and reset facts), the viewport scale of the jittered draws (its signs turn the
    // NDC jitter into window pixels) and the `VR taa:` line's counts.
    std::unique_ptr<VrDibrDlss> vr_dibr_dlss;
    std::unique_ptr<VrRenderTaa> vr_render_taa;
    struct VrTaaState {
        VR::RenderTaa::FrameFacts facts{};
        VR::RenderTaa::Camera camera{};
        VR::RenderTaa::DepthAffine depth{};
        float xscale{1.0f};
        float yscale{-1.0f};
        u64 frames{}, resolved{}, jittered{}, refused{};
        std::array<u64, static_cast<size_t>(VR::RenderTaa::Reset::Count)> resets{};
        std::string last_refusal;
        u32 refusal_lines{};
    } vr_taa{};
    VrUiFrameInfo vr_ui{};
    VrUiLayer* vr_ui_layer{};
    // The gameplay HUD's draws (VrUiCommitHud): signature -> how many gameplay frames in a row
    // drew it, the last one that did, whether it counts as the HUD's.
    struct VrHudDraw {
        u32 streak{};
        u32 plain_streak{}; ///< ... of them with the HUD alone up
        u64 last{};
        bool hud{};
    };
    std::unordered_map<u64, VrHudDraw> vr_hud_draws;
    std::vector<u64> vr_ui_sigs; ///< this frame's shadowed draws, in order
    std::vector<u8> vr_ui_solid; ///< ... 1: a plain translucent rect (VrUiCommitHud)
    bool vr_ui_ds_wrote{false}; ///< bit 58: the last UI draw may write the game's depth/stencil
    bool vr_ui_hud_snap{false};  ///< the next shadow draw keeps K and W first (SnapshotHud)
    u64 vr_hud_frames{0};        ///< gameplay frames committed
    [[nodiscard]] u64 VrUiDrawSignature(const GraphicsPipeline* pipeline);
    /// This UI draw is the lock-on marker's (VR::Ui::LockMarker): locked on, the factors active,
    /// and its one sampled image is the marker's atlas (known by its content).
    [[nodiscard]] bool VrUiIsLockMarker(const RenderState& state);
    bool vr_ui_marker_draw{false}; ///< the draw VrUiPrepare accepted is the lock-on marker's
    // FAST_GPU_PASS_MERGE (vk_pass_merge.h): the 'FAST pass_merge:' counts (the recording thread,
    // like the shadow draws).
    struct VrUiMergeStats {
        u64 draws{};       ///< UI draws asked (VrUiShadowAfter)
        u64 after{};       ///< ... drawn into X first, their shadows after
        u64 shadows{};     ///< shadow passes asked for (VrUiShadowDraw)
        u64 kept{};        ///< ... without an explicit end (KeepOpen)
        u64 continued{};   ///< ... of them: the open pass went on (the same pass)
        u64 clear_ends{};  ///< ... kept but ended explicitly all the same: their state clears
        u64 verified{};    ///< FAST_GPU_PASS_MERGE_VERIFY: decisions checked a second way
        u64 mismatches{};  ///< ... that disagreed (logged; the draw kept the old order)
        u64 memo_resumed{}; ///< begin-state memo hits through VrUiMemoResumes
        std::array<u64, PassMerge::NumFallbacks> fallback{};
    } vr_ui_merge{};
    // The last swapped UI draw: the begin-state memo held its X state, and the scheduler's
    // PassBegins() after its shadow passes (VrUiMemoResumes).
    struct VrUiResume {
        bool valid{};
        u64 passes{};
    } vr_ui_resume{};
};

} // namespace Vulkan
