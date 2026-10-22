#include "video_core/native_stereo/draw_registers.h"
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include <atomic>
#include <bit>
#include "video_core/renderer_vulkan/vr_temporal_optimizations.h"
#include <cstdlib>
#include <mutex>
#include <chrono>
#include <cmath>
#include <cstring>
#include <optional>
#include <tuple>
#include "video_core/buffer_cache/buffer.h"
#include <vk_mem_alloc.h>
#include "video_core/native_stereo/draw_memory.h"
#include "video_core/renderer_vulkan/vr_temporal_failure.h"
#include "video_core/renderer_vulkan/vr_dlss_preset.h"
#include "core/vr/graphics_resolution.h"
#include "core/vr/graphics_status.h"
#include "video_core/renderer_vulkan/vr_scene_proxy.h"
#include <xxhash.h>
#include "video_core/native_stereo/profile_scopes.h"
#include "video_core/native_stereo/native_stereo.h"
#include "common/crash_record.h"
#include "common/render_diagnostic.h"
#include "common/memory_write_trace.h"
#include "common/logging/log.h"
#include "core/memory.h"
#include "core/vr/vr_session.h"
#include "video_core/renderer_vulkan/vr_body_split.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/renderer_vulkan/vk_gpu_pass_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vr_temporal_capture.h"
#include "video_core/renderer_vulkan/vr_fsr4_interleave.h"
#include "video_core/renderer_vulkan/vr_dlss_direct.h"
#include "video_core/renderer_vulkan/vr_upscaler_fast.h"
namespace Vulkan {
namespace {
// Bit 37 provider options for this frame's plan. Only FSR has any, so DLSS and OFF never
// reconfigure when the bit flips; FSR recreates both eyes (and restarts history) on a change.
// Bit off = no flags = FSR 4's original passes, exactly as before the bit existed, except bit
// 45's FAST_FSR4_INTERLEAVE_VERIFY (environment only, so it never changes mid-run): it asks
// for its A/A twin whatever bit 37 says, and takes the place of bit 37's twin.
// FAST_DLSS_DIRECT_VERIFY adds the DLSS twin whatever bit 46 says (bit off, it compares DLSS
// with itself), so flipping bit 46 never reconfigures DLSS either.
uint32_t UpscalerProviderFlags(const VR::Graphics::Config& accepted) {
    if (accepted.upscaler == VR::Graphics::Upscaler::Dlss)
        return !accepted.dlss5 && DlssDirectVerifyPeriod()
                   ? VrTemporalPair::DlssVerifyTwin |
                         (DlssDirectVerifyEye() ? VrTemporalPair::DlssVerifyTwinRight : 0u)
                   : 0u;
    if (accepted.upscaler != VR::Graphics::Upscaler::Fsr4)
        return 0;
    const uint32_t interleave_verify =
        Fsr4InterleaveVerifyPeriod() ? uint32_t(VrTemporalPair::Fsr4VerifyInterleave) : 0u;
    if (!FastRender::On(UpscalerFast))
        return interleave_verify;
    // An A/A twin (FAST_UPSCALER_VERIFY_TWIN=same) also runs with the Fsr4Post part off; the
    // eyes and the twin then both use the original post pass. The fenced SPD pass is not a
    // part: it follows the bit (FAST_UPSCALER_SPD_FENCE=0 keeps the original for A/B runs).
    const bool post = UpscalerFastOn(UpscalerFastPart::Fsr4Post);
    const bool same = UpscalerFastVerifySame();
    uint32_t flags = post ? VrTemporalPair::Fsr4OptimizedPost : 0u;
    if (UpscalerFastSpdFence())
        flags |= VrTemporalPair::Fsr4FencedSpd;
    if (interleave_verify)
        flags |= interleave_verify;
    else if (UpscalerFastVerifyPeriod() && (post || same))
        flags |= VrTemporalPair::Fsr4VerifyTwin | (same ? VrTemporalPair::Fsr4VerifySame : 0u);
    return flags;
}
void ReportUpscalerFast(uint64_t frame, bool active) {
    static uint64_t last_resolved{}, last_avoided{}, last_released{}, last_frame{};
    if (frame - last_frame < 300)
        return;
    const auto& s = g_upscaler_fast;
    const auto resolved = s.resolved.load(std::memory_order_relaxed);
    const auto avoided = s.raw_avoided.load(std::memory_order_relaxed);
    const auto released = s.raw_released.load(std::memory_order_relaxed);
    const double frames = double(frame - last_frame);
    if (FastRender::On(UpscalerFast) || avoided != last_avoided || released != last_released) {
        constexpr std::array<const char*, 3> posts{"original", "optimized", "missing"};
        constexpr std::array<const char*, 4> spds{"none", "original", "fenced", "missing"};
        const auto verify = UpscalerFastVerifyPeriod();
        LOG_INFO(Render_Vulkan,
                 "FAST upscaler: on={} parts={:#x} active={} resolved/f={:.2f} raw_avoided/f={:.2f} "
                 "raw_released/f={:.2f} fsr4_post={} fsr4_spd={} verify={} twin={}{} compared={} "
                 "mismatched_frames={} mismatched_texels={} max_abs={:.6g} skipped={}",
                 FastRender::On(UpscalerFast), UpscalerFastParts(), active,
                 (resolved - last_resolved) / frames, (avoided - last_avoided) / frames,
                 (released - last_released) / frames,
                 posts[std::min<uint32_t>(s.fsr4_post.load(std::memory_order_relaxed), 2)],
                 spds[std::min<uint32_t>(s.fsr4_spd.load(std::memory_order_relaxed), 3)], verify,
                 !verify ? "none" : UpscalerFastVerifySame() ? "same" : "original",
                 verify && UpscalerFastVerifyLockstep() ? "+lockstep" : "",
                 s.verify_compared.load(std::memory_order_relaxed),
                 s.verify_mismatched_frames.load(std::memory_order_relaxed),
                 s.verify_mismatched_texels.load(std::memory_order_relaxed),
                 std::bit_cast<float>(s.verify_max_abs.load(std::memory_order_relaxed)),
                 s.verify_skipped.load(std::memory_order_relaxed));
    }
    last_resolved = resolved;
    last_avoided = avoided;
    last_released = released;
    last_frame = frame;
}
// FAST bit 45 counters, every 300 frames while the bit is on or pairs were recorded.
void ReportFsr4Interleave(uint64_t frame) {
    static uint64_t last_frame{}, last_paired{}, last_interleaved{}, last_captured{},
        last_barriers{}, last_sequential{}, last_dispatches{};
    if (frame - last_frame < 300)
        return;
    const auto& s = g_fsr4_interleave;
    constexpr auto relaxed = std::memory_order_relaxed;
    const auto paired = s.paired.load(relaxed);
    const auto interleaved = s.interleaved.load(relaxed);
    const auto captured = s.captured.load(relaxed);
    const auto barriers = s.barriers.load(relaxed);
    const auto sequential = s.sequential_barriers.load(relaxed);
    const auto dispatches = s.dispatches.load(relaxed);
    if (FastRender::On(Fsr4Interleave) || paired != last_paired) {
        constexpr std::array<const char*, 3> bridges{"none", "per-eye", "pair"};
        const double frames = double(frame - last_frame);
        const double taped = double(std::max<uint64_t>(captured - last_captured, 1));
        LOG_INFO(Render_Vulkan,
                 "FAST fsr4 interleave: on={} bridge={} paired/f={:.2f} interleaved/f={:.2f} "
                 "barriers/pair={:.1f} (per-eye order {:.1f}) dispatches/pair={:.1f} fallback "
                 "desc={} frame={} state={} shape={} record={} unsupported={} verify={} "
                 "compared={} mismatched_frames={} mismatched_texels={} max_abs={:.6g} "
                 "skipped={}",
                 FastRender::On(Fsr4Interleave),
                 bridges[std::min<uint32_t>(s.bridge.load(relaxed), 2)],
                 (paired - last_paired) / frames, (interleaved - last_interleaved) / frames,
                 (barriers - last_barriers) / taped, (sequential - last_sequential) / taped,
                 (dispatches - last_dispatches) / taped,
                 s.fallback[VR_FSR4_PAIR_DESC].load(relaxed),
                 s.fallback[VR_FSR4_PAIR_FRAME].load(relaxed),
                 s.fallback[VR_FSR4_PAIR_STATE].load(relaxed),
                 s.fallback[VR_FSR4_PAIR_SHAPE].load(relaxed),
                 s.fallback[VR_FSR4_PAIR_RECORD].load(relaxed),
                 s.fallback[VR_FSR4_PAIR_UNSUPPORTED].load(relaxed), Fsr4InterleaveVerifyPeriod(),
                 s.verify_compared.load(relaxed), s.verify_mismatched_frames.load(relaxed),
                 s.verify_mismatched_texels.load(relaxed),
                 std::bit_cast<float>(s.verify_max_abs.load(relaxed)),
                 s.verify_skipped.load(relaxed));
    }
    last_frame = frame;
    last_paired = paired;
    last_interleaved = interleaved;
    last_captured = captured;
    last_barriers = barriers;
    last_sequential = sequential;
    last_dispatches = dispatches;
}
// Bit 46: one "FAST dlss_direct" line every 300 frames while the bit is on, verification runs
// or its counters move (per-frame figures over the window).
void ReportDlssDirect(uint64_t frame) {
    static uint64_t last_direct{}, last_copied{}, last_fallback{}, last_saved{}, last_frame{};
    if (frame - last_frame < 300)
        return;
    const auto& s = g_dlss_direct;
    const auto direct = s.direct.load(std::memory_order_relaxed);
    const auto copied = s.copied.load(std::memory_order_relaxed);
    const auto fallback = s.fallback.load(std::memory_order_relaxed);
    const auto saved = s.saved_bytes.load(std::memory_order_relaxed);
    const double frames = double(frame - last_frame);
    const auto verify = DlssDirectVerifyPeriod();
    if (FastRender::On(DlssDirect) || verify || direct != last_direct) {
        constexpr std::array<const char*, 4> twins{"none", "ready", "unavailable", "failed"};
        LOG_INFO(Render_Vulkan,
                 "FAST dlss_direct: on={} direct/f={:.2f} copied/f={:.2f} fallback/f={:.2f} "
                 "saved_mb/f={:.1f} layer_copy={} twin={} verify={} eye={} compared={} "
                 "compared_direct={} mismatched_frames={} mismatched_texels={} max_abs={:.6g} "
                 "skipped={}",
                 FastRender::On(DlssDirect), (direct - last_direct) / frames,
                 (copied - last_copied) / frames, (fallback - last_fallback) / frames,
                 double(saved - last_saved) / frames / 1e6, DlssDirectLayerCopyName(),
                 twins[std::min<uint32_t>(s.twin.load(std::memory_order_relaxed), 3)], verify,
                 DlssDirectVerifyEye() ? "right" : "left",
                 s.verify_compared.load(std::memory_order_relaxed),
                 s.verify_direct.load(std::memory_order_relaxed),
                 s.verify_mismatched_frames.load(std::memory_order_relaxed),
                 s.verify_mismatched_texels.load(std::memory_order_relaxed),
                 std::bit_cast<float>(s.verify_max_abs.load(std::memory_order_relaxed)),
                 s.verify_skipped.load(std::memory_order_relaxed));
    }
    last_direct = direct;
    last_copied = copied;
    last_fallback = fallback;
    last_saved = saved;
    last_frame = frame;
}
bool FullObjectMotion() {
    static const bool full = [] {
        const char* value = std::getenv("VR_OBJECT_MOTION_ALL");
        return value && value[0] == '1';
    }();
    return full;
}
bool HasCharacterSkeleton(const GraphicsPipeline* pipeline) {
    const auto& vs = pipeline->GetStage(Shader::LogicalStage::Vertex);
    for (const auto& resource : vs.buffers) {
        if (resource.IsSpecial()) continue;
        const auto buffer = resource.GetSharp(vs);
        if (buffer.Valid() && buffer.GetStride() == 16 &&
            Motion::ClassifyBuffer(buffer.GetSize()) == Motion::BufferRole::Skeleton)
            return true;
    }
    return false;
}
// Toggle bit 36: IsPlayerMotion's answer with the buffers tested first and the camera record
// read only for the first packet-owned 416-byte transform with unit rows. On the recording thread
// a packet's record is installed (DrawCameraScope) and is read in place: DrawFrameRecord would
// take the session's record_mutex and copy the whole record for every G-buffer draw.
bool IsPlayerMotionFast(const Shader::Info& vs) {
    ++g_motion_fast.player_calls;
    for (const auto& resource : vs.buffers) {
        if (resource.IsSpecial()) continue;
        const auto buffer = resource.GetSharp(vs);
        if (buffer.GetSize() != 416 || !buffer.base_address) continue;
        const auto* original = reinterpret_cast<const void*>(buffer.base_address);
        const auto* source = ScratchDraw::MemorySource(original, 12 * sizeof(float));
        if (source == original) continue;
        float m[12];
        std::memcpy(m, source, sizeof(m));
        bool valid = true;
        for (unsigned row = 0; row < 3; ++row) {
            const float norm = m[row*4]*m[row*4] + m[row*4+1]*m[row*4+1] + m[row*4+2]*m[row*4+2];
            valid &= std::isfinite(norm) && norm > .6f && norm < 1.6f && std::isfinite(m[row*4+3]);
        }
        if (!valid) continue;
        std::optional<VR::CameraRecord> fallback;
        const VR::CameraRecord* camera = VR::DrawCameraOverride();
        if (camera)
            ++g_motion_fast.player_unlocked;
        else
            camera = &fallback.emplace(VR::DrawFrameRecord());
        if (!camera->has_player_position || !std::isfinite(camera->world_scale) ||
            camera->world_scale <= 0)
            return false;
        return VrBodySplit::IsPlayersObject(
            (m[3]-camera->player_position.x)/camera->world_scale,
            (m[7]-camera->player_position.y)/camera->world_scale,
            (m[11]-camera->player_position.z)/camera->world_scale, m[5] > .9995f);
    }
    return false;
}
bool IsPlayerMotionSlow(const Shader::Info& vs);
// Reuse the body renderer's player-origin heuristic for rigid hand/weapon pieces.
// Only packet-owned transforms may participate; no bone readback or palette hash.
bool IsPlayerMotion(const Shader::Info& vs) {
    if (!FastRender::On(MotionFast))
        return IsPlayerMotionSlow(vs);
    const bool fast = IsPlayerMotionFast(vs);
    if (!MotionVerifySample())
        return fast;
    const bool slow = IsPlayerMotionSlow(vs);
    if (slow != fast) {
        ++g_motion_fast.verify_mismatches;
        LOG_ERROR(Render_Vulkan, "FAST motion verify: player test fast={} old={}", fast, slow);
    }
    return slow;
}
bool IsPlayerMotionSlow(const Shader::Info& vs) {
    const auto camera = VR::DrawFrameRecord();
    if (!camera.has_player_position || !std::isfinite(camera.world_scale) || camera.world_scale <= 0)
        return false;
    for (const auto& resource : vs.buffers) {
        if (resource.IsSpecial()) continue;
        const auto buffer = resource.GetSharp(vs);
        if (buffer.GetSize() != 416 || !buffer.base_address) continue;
        const auto* original = reinterpret_cast<const void*>(buffer.base_address);
        const auto* source = ScratchDraw::MemorySource(original, 12 * sizeof(float));
        if (source == original) continue;
        float m[12];
        std::memcpy(m, source, sizeof(m));
        bool valid = true;
        for (unsigned row = 0; row < 3; ++row) {
            const float norm = m[row*4]*m[row*4] + m[row*4+1]*m[row*4+1] + m[row*4+2]*m[row*4+2];
            valid &= std::isfinite(norm) && norm > .6f && norm < 1.6f && std::isfinite(m[row*4+3]);
        }
        if (!valid) continue;
        return VrBodySplit::IsPlayersObject(
            (m[3]-camera.player_position.x)/camera.world_scale,
            (m[7]-camera.player_position.y)/camera.world_scale,
            (m[11]-camera.player_position.z)/camera.world_scale, m[5] > .9995f);
    }
    return false;
}
// Toggle bit 36: GetVerified's {range, topology} without hashing the index list when its bytes
// provably did not change. bbport hashes only inside its range-cache miss (bbport
// gpu/shadps4/video_core/renderer_vulkan/vk_rasterizer.cpp:1728-1740) and trusts a hit for 32
// frames (motion_history.h:53-59), which can pair changed indices with old history; GetVerified
// instead hashed every list on every draw. Here a hit needs proof: the range's write-tracking
// epoch (BufferCache::TrackingEpoch) equals the one read before the bytes were last hashed while
// every page of them was write-protected (CPU-clean) and not GPU-modified. A guest write to such a
// page faults and bumps the epoch under the tracker lock before it lands, as does every upload,
// GPU mark or invalidation, and the epochs only grow: an unchanged epoch means the bytes hashed
// then are the bytes now. The same proof keeps perf_copy_memo exact (ObtainCopyBuffers).
template <class Scan>
Motion::IndexRangeCache::Result ProvenIndexRange(VideoCore::BufferCache& buffers,
                                                 Motion::IndexRangeCache& cache,
                                                 const Motion::IndexRangeCache::Key& key,
                                                 uint64_t bytes, uint64_t frame, Scan&& scan) {
    const auto* data = reinterpret_cast<const void*>(key.address);
    ++g_motion_fast.index_draws;
    const uint64_t epoch = buffers.TrackingEpoch(key.address, bytes);
    if (const auto* hit = cache.FindProven(key, epoch, frame)) {
        ++g_motion_fast.index_proven;
        g_motion_fast.skipped_bytes += bytes;
        if (!MotionVerifySample())
            return *hit;
        const uint64_t topology = XXH3_64bits(data, bytes);
        if (topology == hit->topology)
            return *hit;
        if (buffers.TrackingEpoch(key.address, bytes) != epoch) {
            ++g_motion_fast.verify_races; // written while verifying: not a proof failure
        } else {
            ++g_motion_fast.verify_mismatches;
            LOG_ERROR(Render_Vulkan,
                      "FAST motion verify: index list {:#x} ({} bytes) changed under tracking "
                      "epoch {}: hash {:#x} cached {:#x}",
                      key.address, bytes, epoch, topology, hit->topology);
        }
        return cache.GetVerified(key, topology, frame, scan); // the old path; drops the proof
    }
    // Epoch first, then the page states, then the bytes: a change after the epoch was read
    // bumps it past the recorded proof.
    const bool provable = epoch && !buffers.IsRegionCpuModified(key.address, bytes) &&
                          !buffers.IsRegionGpuModified(key.address, bytes);
    const uint64_t topology = XXH3_64bits(data, bytes);
    if (provable)
        ++g_motion_fast.index_hashed;
    else
        ++g_motion_fast.index_unprovable;
    g_motion_fast.hashed_bytes += bytes;
    return cache.GetVerified(key, topology, frame, scan, provable ? epoch : 0);
}
// Toggle bit 36: one "FAST motion" line every 2 s (per-frame figures over the window).
void LogMotionFast() {
    static auto last = std::chrono::steady_clock::now();
    static MotionFastCounters seen{};
    auto& c = g_motion_fast;
    ++c.frames;
    const auto now = std::chrono::steady_clock::now();
    if (now - last < std::chrono::seconds(2))
        return;
    last = now;
    const double frames = double(c.frames - seen.frames);
    const auto per_frame = [frames](uint64_t now_value, uint64_t then) {
        return frames > 0 ? double(now_value - then) / frames : 0.0;
    };
    LOG_INFO(Render_Vulkan,
             "FAST motion: frames={:.0f} index_draws/f={:.1f} proven/f={:.1f} hashed/f={:.1f} "
             "unprovable/f={:.1f} hashed_kb/f={:.1f} skipped_kb/f={:.1f} camera_reused/f={:.1f} "
             "of {:.1f} player_unlocked/f={:.1f} of {:.1f} variant_memo/f={:.1f} of {:.1f} "
             "verify={} mismatches={} races={}",
             frames, per_frame(c.index_draws, seen.index_draws),
             per_frame(c.index_proven, seen.index_proven),
             per_frame(c.index_hashed, seen.index_hashed),
             per_frame(c.index_unprovable, seen.index_unprovable),
             per_frame(c.hashed_bytes, seen.hashed_bytes) / 1024.0,
             per_frame(c.skipped_bytes, seen.skipped_bytes) / 1024.0,
             per_frame(c.camera_reused, seen.camera_reused),
             per_frame(c.camera_calls, seen.camera_calls),
             per_frame(c.player_unlocked, seen.player_unlocked),
             per_frame(c.player_calls, seen.player_calls),
             per_frame(c.variant_memo, seen.variant_memo),
             per_frame(c.variant_calls, seen.variant_calls), c.verify_checks,
             c.verify_mismatches, c.verify_races);
    seen = c;
}
} // namespace

// S1, FAST bit 51: the whole guest frame at the upscaler's input size, reconstructed after
// tonemapping and before the HUD (bbport's default for every output but 1080p; design in
// offline analysis). The boot patch is the switch
// (memory_patcher.cpp ApplyHeadsetResolution: FAST_TOGGLES bit 51, FAST_S1=1 or vr.ini
// upscale_whole_frame=1): it is latched at the first draw and cannot flip while the game runs, so
// a FAST_TOGGLE_FILE change of the bit is ignored. Bit off: nothing below runs.
static constexpr auto WholeFrameUpscale = static_cast<FastRender::Bit>(51);
namespace {
// Recover S1 after transient scene failures (for example the death/loading screen has no
// scene depth). The boot-patched guest remains at the input size, so leaving the provider
// off permanently would leave an enlarged low-resolution image. Keep the provider configured
// while waiting and retry with ZERO jitter and reset history until a complete stereo frame
// succeeds. A failed probe therefore cannot expose jittered fallback scenery. Hard provider
// failures still require a settings change/restart. Historical flag: FAST_GPU_S1_ASYM=0 opts out.
const bool g_s1_retry = [] {
    const char* value = std::getenv("FAST_GPU_S1_ASYM");
    return !value || value[0] != '0';
}();
// FAST_GPU_S1_ASYM_VERIFY=1 (with the retry on): every waiting frame also does the old Begin with
// the provider OFF and compares the frame state it leaves with the pause's (S1RetryIdle).
const bool g_s1_retry_verify = [] {
    const char* value = std::getenv("FAST_GPU_S1_ASYM_VERIFY");
    return g_s1_retry && value && value[0] == '1';
}();
// S1 + DLSS 5: the sRGB twin of X's 8-bit UNORM format (eUndefined: none).
vk::Format S1SrgbTwin(vk::Format format) {
    switch (format) {
    case vk::Format::eR8G8B8A8Unorm: return vk::Format::eR8G8B8A8Srgb;
    case vk::Format::eB8G8R8A8Unorm: return vk::Format::eB8G8R8A8Srgb;
    case vk::Format::eA8B8G8R8UnormPack32: return vk::Format::eA8B8G8R8SrgbPack32;
    default: return vk::Format::eUndefined;
    }
}
// The S1 output the first call saw, for every later one. The first draw comes after the game's
// patches were applied (before its first instruction), so it cannot be read at start-up; it is
// published once through an atomic flag, a plain load per native-stereo draw where a
// function-local static paid a guard (a thread-local read) on every call. Publish the
// boot input alongside it: accepted settings can change before the required restart.
std::atomic<bool> whole_frame_set{false};
VR::Graphics::Extent whole_frame_output{};
VR::Graphics::Extent whole_frame_input{};
// S1 history describes the boot-sized scene; auxiliary G-buffers have their own cameras.
const bool g_temporal_main_extent = [] {
    const char* value = std::getenv("VR_TEMPORAL_MAIN_EXTENT");
    return !value || value[0] != '0';
}();
std::mutex whole_frame_mutex;
SHAD_NO_INLINE VR::Graphics::Extent FirstWholeFrameOutput() {
    std::scoped_lock lock{whole_frame_mutex};
    if (!whole_frame_set.load(std::memory_order_relaxed)) {
        whole_frame_output = VR::Graphics::S1Output();
        whole_frame_input = VR::Graphics::WholeFrameResolution(whole_frame_output,
            VR::Graphics::Unpack(VR::Graphics::s1_config.load(std::memory_order_relaxed)));
        whole_frame_set.store(true, std::memory_order_release);
    }
    return whole_frame_output;
}
VR::Graphics::Extent WholeFrameOutput() {
    if (whole_frame_set.load(std::memory_order_acquire)) [[likely]] {
        return whole_frame_output;
    }
    return FirstWholeFrameOutput();
}
void ReportWholeFrame(uint64_t frame, const VrTemporalCapture& t, uint32_t resolves,
                      uint32_t uploads) {
    static uint64_t last_frame{};
    static uint64_t seen_frames{}, seen_reconstructed{}, seen_inframe{}, seen_ui_scene{};
    static uint64_t seen_hud_frames{}, seen_hud_syncs{}, seen_hud_draws{};
    if (frame - last_frame < 300)
        return;
    const auto& s = t.s1;
    const double frames = double(std::max<uint64_t>(s.frames - seen_frames, 1));
    const auto accepted = VR::Graphics::Unpack(t.accepted_key);
    LOG_INFO(Render_Vulkan,
             "FAST s1: on=1 bit51={} input={}x{} output={}x{} provider={} preset={} active={} "
             "reconstructed/f={:.2f} failed={} missed={} inframe/f={:.2f} ui_scene/f={:.2f} "
             "resolves={} uploads={} (proxies off: 0 expected)",
             FastRender::On(WholeFrameUpscale), t.width, t.height, s.width, s.height,
             VR::Graphics::Neural(accepted)                      ? "dlss5"
             : accepted.upscaler == VR::Graphics::Upscaler::Dlss ? "dlss"
             : accepted.upscaler == VR::Graphics::Upscaler::Fsr31 ? "fsr31"
             : accepted.upscaler == VR::Graphics::Upscaler::Fsr4 ? "fsr4"
                                                                 : "off",
             int(VR::Graphics::EffectiveProfile(accepted)), t.active,
             double(s.reconstructed - seen_reconstructed) / frames, s.failed, s.missed,
             double(s.inframe - seen_inframe) / frames, double(s.ui_scene - seen_ui_scene) / frames,
             resolves, uploads);
    if constexpr (Common::Log::Enabled) {
        const double hud_frames = double(std::max<uint64_t>(s.hud_frames - seen_hud_frames, 1));
        LOG_INFO(Render_Vulkan,
                 "VR UI depth: {:.2f} copies/frame, {:.2f} shadow draws/frame ({} HUD frames)",
                 double(s.hud_syncs - seen_hud_syncs) / hud_frames,
                 double(s.hud_draws - seen_hud_draws) / hud_frames,
                 s.hud_frames - seen_hud_frames);
        seen_hud_frames = s.hud_frames;
        seen_hud_syncs = s.hud_syncs;
        seen_hud_draws = s.hud_draws;
    }
    seen_frames = s.frames;
    seen_reconstructed = s.reconstructed;
    seen_inframe = s.inframe;
    seen_ui_scene = s.ui_scene;
    last_frame = frame;
    if (s.latches || g_s1_retry_verify)
        LOG_INFO(Render_Vulkan,
                 "FAST s1_asym: on={} latches={} retries={} recovered={} waited={} wait={} "
                 "next_retry={} verify={} checked={} mismatches={}",
                 g_s1_retry, s.latches, s.retries, s.recovered, s.waited, s.retry.wait,
                 s.retry.at, g_s1_retry_verify, s.retry_checked, s.retry_mismatches);
}
// FAST_GPU_S1_ASYM: S1's provider latched off at this frame by incomplete frames (or a scene
// without its post-processing trigger): it waits, configured, for TemporalRetry's frame.
void S1RetryLatch(VrTemporalCapture& t, const char* why) {
    auto& s = t.s1;
    s.retry.Latch(t.frame);
    ++s.latches;
    LOG_INFO(Render_Vulkan,
             "VR temporal S1: provider idle after frame {} ({}); retry at frame {} "
             "(FAST_GPU_S1_ASYM, {} frames)",
             t.frame, why, s.retry.at, s.retry.wait);
}
// FAST_GPU_S1_ASYM: a frame while the provider waits for its retry. No Begin (with the provider
// OFF it retires the provider and its history); the scene is paused as that Begin leaves it.
void S1RetryIdle(VrTemporalCapture& t, const VR::Graphics::Config& off,
                 const VrTemporalPair::Plan& plan) {
    auto& s = t.s1;
    ++s.waited;
    t.active = false;
    t.scene.Pause();
    if (!g_s1_retry_verify)
        return;
    // FAST_GPU_S1_ASYM_VERIFY: the old work too (Begin with the provider OFF, which retires it:
    // the retry then prepares it again), and the frame state it leaves against the pause's. The
    // old answer is used.
    const auto state = [&] {
        const auto jitter = t.scene.Jitter();
        return std::tuple{t.scene.Objects() != nullptr, jitter[0], jitter[1],
                          t.scene.Output().has_value(), t.scene.Active()};
    };
    const auto paused = state();
    const auto result = t.scene.Begin(t.frame, off, plan);
    const bool ready = result == VrTemporalPair::Result::Ready;
    const auto old = state();
    ++s.retry_checked;
    if (ready || old != paused) {
        ++s.retry_mismatches;
        if (s.retry_mismatches <= 16)
            LOG_ERROR(Render_Vulkan,
                      "FAST_GPU_S1_ASYM verify: frame {} waits for its retry; Begin with the "
                      "provider OFF gives ready={} objects={} jitter={},{} output={} active={}, "
                      "the pause objects={} jitter={},{} output={} active={}; the old answer is "
                      "used, mismatch #{}",
                      t.frame, ready, std::get<0>(old), std::get<1>(old), std::get<2>(old),
                      std::get<3>(old), std::get<4>(old), std::get<0>(paused),
                      std::get<1>(paused), std::get<2>(paused), std::get<3>(paused),
                      std::get<4>(paused), s.retry_mismatches);
    }
    t.active = ready;
}
} // namespace

VrTemporalCapture::~VrTemporalCapture() { RetireViews(0); RetireViews(1); RetireS1(); }
void VrTemporalCapture::RetireS1() {
    auto& s = s1;
    s.verify_pending = false; // the readback's images are going away
    if (!s.color && !s.depth && !s.encode && !s.ui_ds) return;
    scheduler.DeferOperation([color = std::move(s.color), depth = std::move(s.depth),
                              encode = std::move(s.encode), views = std::move(s.srgb),
                              download = s.verify_download, ui_ds = std::move(s.ui_ds),
                              ui_ds_view = std::move(s.ui_ds_view)]() mutable {
        for (auto& view : views) view.reset();
        ui_ds_view.reset();
        ui_ds.Destroy();
        color.Destroy();
        depth.Destroy();
        encode.Destroy();
        download.reset();
    });
    s.ui_ds_format = vk::Format::eUndefined;
    s.ui_ds_layout = vk::ImageLayout::eUndefined;
    s.ui_ds_layers = 0;
    s.ui_ds_dirty = true;
    s.ui_ds_failed = false;
    s.srgb = {};
    s.color_format = s.depth_format = s.srgb_format = s.encode_format = vk::Format::eUndefined;
    s.color_layout = vk::ImageLayout::eUndefined;
}
// The reconstruction's target: X's own format (the bytes as the game stores them), the output
// size, both layers; mutable for the in-frame items' sRGB views.
bool VrTemporalCapture::S1Color(const VideoCore::Image& x) {
    auto& s = s1;
    const vk::Format format = x.backing->image.image_ci.format;
    if (s.color && s.color_format == format) return true;
    const auto features =
        instance.GetPhysicalDevice().getFormatProperties(format).optimalTilingFeatures;
    if (!(features & vk::FormatFeatureFlagBits::eBlitDst) ||
        !(features & vk::FormatFeatureFlagBits::eColorAttachment)) {
        LOG_WARNING(Render_Vulkan, "VR temporal S1: the composite target's format {} cannot "
                    "receive the reconstruction", vk::to_string(format));
        return false;
    }
    RetireS1();
    s.color = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
    s.color.Create(vk::ImageCreateInfo{
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {s.width, s.height, 1},
        .mipLevels = 1,
        .arrayLayers = 2,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eSampled,
        .initialLayout = vk::ImageLayout::eUndefined});
    s.color_format = format;
    s.color_layout = vk::ImageLayout::eUndefined;
    LOG_INFO(Render_Vulkan, "VR temporal S1: output scene {}x{} x2 {}", s.width, s.height,
             vk::to_string(format));
    return true;
}
// The scene depth at the output size (nearest blits, depth aspect): the in-frame items' depth.
bool VrTemporalCapture::S1Depth(const VideoCore::Image& depth) {
    auto& s = s1;
    const vk::Format format = depth.backing->image.image_ci.format;
    if (s.depth && s.depth_format == format) return true;
    if (!s.depth_blit) return false;
    const auto features =
        instance.GetPhysicalDevice().getFormatProperties(format).optimalTilingFeatures;
    if (!(features & vk::FormatFeatureFlagBits::eBlitSrc) ||
        !(features & vk::FormatFeatureFlagBits::eBlitDst)) {
        s.depth_blit = false;
        LOG_WARNING(Render_Vulkan, "VR temporal S1: the scene depth's format {} cannot be "
                    "blitted; in-frame items stay in the input-size target", vk::to_string(format));
        return false;
    }
    if (s.depth) {
        scheduler.DeferOperation([old = std::move(s.depth)]() mutable { old.Destroy(); });
    }
    s.depth = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
    s.depth.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {s.width, s.height, 1},
        .mipLevels = 1,
        .arrayLayers = 2,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined});
    s.depth_format = format;
    return true;
}
// FAST bit 59: the HUD passes' depth/stencil at the output size (Scaleform's stencil masks).
// The game keeps drawing its masks into its own input-size image; before a shadowed HUD draw that
// tests depth or stencil, a dirty twin takes its contents again (nearest blit, every aspect).
// Outside a render pass. Null: the format cannot be blitted or attached (the caller falls back).
vk::ImageView VrTemporalCapture::S1UiDepth(VideoCore::Image& ds, vk::ImageAspectFlags tested_aspects,
                                            bool shared_view_only) {
    auto& s = s1;
    const vk::Format format = ds.backing->image.image_ci.format;
    const uint32_t layers = std::max(1u, ds.backing->image.image_ci.arrayLayers);
    vk::ImageAspectFlags aspect{};
    switch (format) {
    case vk::Format::eD16Unorm:
    case vk::Format::eD32Sfloat:
    case vk::Format::eX8D24UnormPack32:
        aspect = vk::ImageAspectFlagBits::eDepth;
        break;
    case vk::Format::eS8Uint:
        aspect = vk::ImageAspectFlagBits::eStencil;
        break;
    case vk::Format::eD16UnormS8Uint:
    case vk::Format::eD24UnormS8Uint:
    case vk::Format::eD32SfloatS8Uint:
        aspect = vk::ImageAspectFlagBits::eDepth | vk::ImageAspectFlagBits::eStencil;
        break;
    default:
        return {};
    }
    // A blit needs a single-sampled source the game's image can be read from.
    if (ds.backing->num_samples > 1 ||
        !(ds.usage_flags & vk::ImageUsageFlagBits::eTransferSrc)) {
        return {};
    }
    bool initialize_unused_layers = false;
    if (!s.ui_ds || s.ui_ds_format != format || s.ui_ds_layers != layers) {
        if (s.ui_ds_failed && s.ui_ds_format == format) return {};
        const auto features =
            instance.GetPhysicalDevice().getFormatProperties(format).optimalTilingFeatures;
        if (!(features & vk::FormatFeatureFlagBits::eBlitSrc) ||
            !(features & vk::FormatFeatureFlagBits::eBlitDst) ||
            !(features & vk::FormatFeatureFlagBits::eDepthStencilAttachment)) {
            s.ui_ds_failed = true;
            s.ui_ds_format = format;
            LOG_WARNING(Render_Vulkan, "VR temporal S1: the HUD's depth/stencil format {} cannot "
                        "be blitted; such HUD frames stay at the input size", vk::to_string(format));
            return {};
        }
        if (s.ui_ds) {
            scheduler.DeferOperation([old = std::move(s.ui_ds),
                                      view = std::move(s.ui_ds_view)]() mutable {
                view.reset();
                old.Destroy();
            });
        }
        s.ui_ds = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
        s.ui_ds.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = format,
            .extent = {s.width, s.height, 1},
            .mipLevels = 1,
            .arrayLayers = layers,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eDepthStencilAttachment |
                     vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined});
        auto view = instance.GetDevice().createImageViewUnique({
            .image = s.ui_ds.image,
            .viewType = layers > 1 ? vk::ImageViewType::e2DArray : vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {aspect, 0, 1, 0, layers}});
        if (view.result != vk::Result::eSuccess) {
            s.ui_ds_failed = true;
            s.ui_ds_format = format;
            return {};
        }
        s.ui_ds_view = std::move(view.value);
        s.ui_ds_format = format;
        s.ui_ds_layers = layers;
        s.ui_ds_layout = vk::ImageLayout::eUndefined;
        s.ui_ds_dirty = true;
        s.ui_ds_failed = false;
        initialize_unused_layers = layers > 1;
        LOG_INFO(Render_Vulkan, "VR temporal S1: HUD at the output size (FAST bit 59), "
                 "depth/stencil twin {}x{} x{} {}", s.width, s.height, layers,
                 vk::to_string(format));
    }
    const auto cmd = scheduler.Commands();
    const auto twin_to = [&](vk::ImageLayout layout, vk::PipelineStageFlags2 stage,
                             vk::AccessFlags2 access) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask = stage,
            .dstAccessMask = access,
            .oldLayout = s.ui_ds_layout,
            .newLayout = layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = s.ui_ds.image,
            .subresourceRange = {aspect, 0, 1, 0, layers}};
        cmd.pipelineBarrier2(
            vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
        s.ui_ds_layout = layout;
    };
    constexpr auto ds_stages = vk::PipelineStageFlagBits2::eEarlyFragmentTests |
                               vk::PipelineStageFlagBits2::eLateFragmentTests;
    constexpr auto ds_access = vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                               vk::AccessFlagBits2::eDepthStencilAttachmentWrite;
    // K/W are multiview render targets for compatibility with the game's pipeline, but
    // VrUiLayer::EnsureTargets and SnapshotHud sample/copy only layer zero into the shared
    // UI atlas. Initialize unused layers deterministically; their clipping results are never
    // consumed. Only the mask for layer zero needs refreshing after each guest mask draw.
    if (initialize_unused_layers) {
        twin_to(vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eTransferWrite);
        cmd.clearDepthStencilImage(s.ui_ds.image, vk::ImageLayout::eTransferDstOptimal,
                                   vk::ClearDepthStencilValue{1.0f, 0},
                                   vk::ImageSubresourceRange{aspect, 0, 1, 1, layers - 1});
    }
    // A different mask image needs a copy even when neither draw wrote to it.
    if (s.ui_ds_dirty || s.ui_ds_source != ds.GetImage() || !shared_view_only) {
        s.ui_ds_valid_aspects = {};
        s.ui_ds_dirty = false;
    }
    // Scaleform clipping normally tests stencil alone. Keep depth and stencil validity
    // separately so a later depth test still copies depth, without copying it for every mask.
    const auto copy_aspects = tested_aspects & aspect & ~s.ui_ds_valid_aspects;
    if (copy_aspects) {
        const vk::ImageLayout game_layout = ds.backing->state.layout;
        ds.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {});
        twin_to(vk::ImageLayout::eTransferDstOptimal, vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eTransferWrite);
        const vk::ImageBlit region{
            .srcSubresource = {copy_aspects, 0, 0, shared_view_only ? 1u : layers},
            .srcOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0},
                vk::Offset3D{static_cast<int32_t>(ds.info.size.width),
                             static_cast<int32_t>(ds.info.size.height), 1}},
            .dstSubresource = {copy_aspects, 0, 0, shared_view_only ? 1u : layers},
            .dstOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0},
                vk::Offset3D{static_cast<int32_t>(s.width), static_cast<int32_t>(s.height), 1}}};
        cmd.blitImage(ds.GetImage(), vk::ImageLayout::eTransferSrcOptimal, s.ui_ds.image,
                      vk::ImageLayout::eTransferDstOptimal, region, vk::Filter::eNearest);
        twin_to(vk::ImageLayout::eDepthStencilAttachmentOptimal, ds_stages, ds_access);
        if (game_layout != vk::ImageLayout::eUndefined &&
            game_layout != vk::ImageLayout::eTransferSrcOptimal) {
            ds.Transit(game_layout, ds_access, {}); // the game's own pass expects it back
        }
        s.ui_ds_valid_aspects |= copy_aspects;
        s.ui_ds_source = ds.GetImage();
        ++s.hud_syncs;
    } else if (s.ui_ds_layout != vk::ImageLayout::eDepthStencilAttachmentOptimal) {
        twin_to(vk::ImageLayout::eDepthStencilAttachmentOptimal, ds_stages, ds_access);
    }
    return *s.ui_ds_view;
}
bool VrTemporalCapture::S1SrgbViews(vk::Format srgb) {
    auto& s = s1;
    if (!s.color) return false;
    if (s.srgb_format == srgb && s.srgb[0] && s.srgb[1]) return true;
    if (s.srgb[0] || s.srgb[1]) {
        scheduler.DeferOperation([old = std::move(s.srgb)]() mutable {
            for (auto& view : old) view.reset();
        });
        s.srgb = {};
    }
    s.srgb_format = vk::Format::eUndefined;
    const vk::ImageViewUsageCreateInfo usage{.usage = vk::ImageUsageFlagBits::eColorAttachment};
    for (uint32_t eye = 0; eye < 2; ++eye) {
        auto result = instance.GetDevice().createImageViewUnique({
            .pNext = &usage,
            .image = s.color.image,
            .viewType = vk::ImageViewType::e2D,
            .format = srgb,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, eye, 1}});
        if (result.result != vk::Result::eSuccess) return false;
        s.srgb[eye] = std::move(result.value);
    }
    s.srgb_format = srgb;
    return true;
}
// S1 + DLSS 5: the provider's eyes are linear (the chain composes in linear light), `color` holds
// X's bytes (sRGB-encoded UNORM). A blit into this sRGB twin encodes; a copy then moves the bytes.
bool VrTemporalCapture::S1Encode(vk::Format srgb) {
    auto& s = s1;
    if (s.encode && s.encode_format == srgb) return true;
    const auto features =
        instance.GetPhysicalDevice().getFormatProperties(srgb).optimalTilingFeatures;
    if (!(features & vk::FormatFeatureFlagBits::eBlitDst)) {
        LOG_WARNING(Render_Vulkan, "VR temporal S1: {} cannot receive DLSS 5's encode blit",
                    vk::to_string(srgb));
        return false;
    }
    if (s.encode) {
        scheduler.DeferOperation([old = std::move(s.encode)]() mutable { old.Destroy(); });
    }
    s.encode = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
    s.encode.Create(vk::ImageCreateInfo{
        .imageType = vk::ImageType::e2D,
        .format = srgb,
        .extent = {s.width, s.height, 1},
        .mipLevels = 1,
        .arrayLayers = 2,
        .samples = vk::SampleCountFlagBits::e1,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
        .initialLayout = vk::ImageLayout::eUndefined});
    s.encode_format = srgb;
    LOG_INFO(Render_Vulkan, "VR temporal S1: DLSS 5 linear output encoded through {} {}x{} x2",
             vk::to_string(srgb), s.width, s.height);
    return true;
}
// Outside a render pass. Each user either replaces every texel or reads what the last one left.
void VrTemporalCapture::S1ColorTo(vk::ImageLayout layout, vk::PipelineStageFlags2 stage,
                                  vk::AccessFlags2 access) {
    auto& s = s1;
    const vk::ImageMemoryBarrier2 barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = stage,
        .dstAccessMask = access,
        .oldLayout = s.color_layout,
        .newLayout = layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = s.color.image,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, 0, 1, 0, 2}};
    scheduler.Commands().pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &barrier});
    s.color_layout = layout;
}
// FAST_S1_VERIFY=N (bit 51). S1 cannot equal the old path texel for texel (another render size),
// so every Nth reconstruction is checked against its own input instead: both layers of X and of
// the output are read back (never waited for) and averaged on a 32 x 32 grid (8 x 8 samples per
// cell). Each output eye must match its own input eye (mean |difference| at most 8 of 255 per
// channel) and not the other eye better: this catches a swapped eye, swapped channels, a shifted,
// scaled or empty reconstruction. One "FAST s1 verify" line per comparison.
static uint32_t S1VerifyPeriod() {
    static const uint32_t period = [] {
        const char* value = std::getenv("FAST_S1_VERIFY");
        return value ? uint32_t(std::strtoul(value, nullptr, 10)) : 0u;
    }();
    return period;
}
// After the blits, X in GENERAL (provider input) and the output in TRANSFER_DST.
void VrTemporalCapture::S1Verify(const VideoCore::Image& x, uint64_t frame) {
    auto& s = s1;
    const uint32_t period = S1VerifyPeriod();
    if (!period) return;
    if (s.verify_pending && scheduler.IsFree(s.verify_ticket)) S1Compare();
    if (s.verify_pending || frame % period) return;
    const vk::Format format = s.color_format;
    if (format != vk::Format::eR8G8B8A8Unorm && format != vk::Format::eR8G8B8A8Srgb &&
        format != vk::Format::eB8G8R8A8Unorm && format != vk::Format::eB8G8R8A8Srgb) return;
    const uint32_t xw = x.info.size.width, xh = x.info.size.height;
    const uint64_t x_bytes = uint64_t(xw) * xh * 4, o_bytes = uint64_t(s.width) * s.height * 4;
    const uint64_t size = 2 * (x_bytes + o_bytes);
    if (!s.verify_download || s.verify_size < size) {
        if (s.verify_download) {
            scheduler.DeferOperation([old = std::move(s.verify_download)]() mutable { old.reset(); });
        }
        auto buffer = std::make_shared<VideoCore::UniqueBuffer>(instance.GetDevice(),
                                                                instance.GetAllocator());
        VmaAllocationInfo allocation{};
        buffer->Create({.size = size, .usage = vk::BufferUsageFlagBits::eTransferDst},
                       VideoCore::MemoryUsage::Download, &allocation, true);
        s.verify_mapped = allocation.pMappedData;
        s.verify_download = std::move(buffer);
        s.verify_size = size;
    }
    if (!s.verify_mapped) return;
    S1ColorTo(vk::ImageLayout::eTransferSrcOptimal, vk::PipelineStageFlagBits2::eTransfer,
              vk::AccessFlagBits2::eTransferRead);
    const vk::ImageSubresourceRange layers{vk::ImageAspectFlagBits::eColor, 0, 1, 0, 2};
    vk::ImageMemoryBarrier2 x_barrier{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask = vk::AccessFlagBits2::eTransferRead,
        .oldLayout = vk::ImageLayout::eGeneral,
        .newLayout = vk::ImageLayout::eGeneral,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = x.GetImage(),
        .subresourceRange = layers};
    const auto cmd = scheduler.Commands();
    cmd.pipelineBarrier2(
        vk::DependencyInfo{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &x_barrier});
    const vk::Buffer buffer = *s.verify_download;
    for (uint32_t eye = 0; eye < 2; ++eye) {
        const vk::BufferImageCopy from_x{.bufferOffset = x_bytes * eye,
                                         .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, eye, 1},
                                         .imageExtent = {xw, xh, 1}};
        cmd.copyImageToBuffer(x.GetImage(), vk::ImageLayout::eGeneral, buffer, from_x);
        const vk::BufferImageCopy from_output{
            .bufferOffset = 2 * x_bytes + o_bytes * eye,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, eye, 1},
            .imageExtent = {s.width, s.height, 1}};
        cmd.copyImageToBuffer(s.color.image, vk::ImageLayout::eTransferSrcOptimal, buffer,
                              from_output);
    }
    // X's next writer (the UI draws) after these reads; the host after the copies.
    x_barrier.srcStageMask = vk::PipelineStageFlagBits2::eTransfer;
    x_barrier.srcAccessMask = vk::AccessFlagBits2::eTransferRead;
    x_barrier.dstStageMask = vk::PipelineStageFlagBits2::eAllCommands;
    x_barrier.dstAccessMask = vk::AccessFlagBits2::eMemoryRead | vk::AccessFlagBits2::eMemoryWrite;
    const vk::BufferMemoryBarrier2 host{.srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
                                        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
                                        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
                                        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
                                        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                        .buffer = buffer,
                                        .offset = 0,
                                        .size = VK_WHOLE_SIZE};
    cmd.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1,
                                            .pBufferMemoryBarriers = &host,
                                            .imageMemoryBarrierCount = 1,
                                            .pImageMemoryBarriers = &x_barrier});
    s.verify_xw = xw;
    s.verify_xh = xh;
    s.verify_ticket = scheduler.CurrentTick();
    s.verify_pending = true;
}
void VrTemporalCapture::S1Compare() {
    auto& s = s1;
    s.verify_pending = false;
    if (!s.verify_mapped) return;
    const auto* bytes = static_cast<const uint8_t*>(s.verify_mapped);
    const uint32_t xw = s.verify_xw, xh = s.verify_xh, ow = s.width, oh = s.height;
    const size_t x_bytes = size_t(xw) * xh * 4, o_bytes = size_t(ow) * oh * 4;
    const std::array<const uint8_t*, 2> in{bytes, bytes + x_bytes};
    const std::array<const uint8_t*, 2> out{bytes + 2 * x_bytes, bytes + 2 * x_bytes + o_bytes};
    constexpr uint32_t Grid = 32, Samples = 8;
    const auto cell = [](const uint8_t* image, uint32_t w, uint32_t h, uint32_t gx, uint32_t gy) {
        std::array<double, 3> sum{};
        for (uint32_t sy = 0; sy < Samples; ++sy)
            for (uint32_t sx = 0; sx < Samples; ++sx) {
                const auto px = uint32_t((gx * Samples + sx + .5) * w / (Grid * Samples));
                const auto py = uint32_t((gy * Samples + sy + .5) * h / (Grid * Samples));
                const uint8_t* p = image + (size_t(std::min(py, h - 1)) * w + std::min(px, w - 1)) * 4;
                for (uint32_t c = 0; c < 3; ++c) sum[c] += p[c];
            }
        for (auto& v : sum) v /= Samples * Samples;
        return sum;
    };
    double same = 0, cross = 0;
    for (uint32_t eye = 0; eye < 2; ++eye)
        for (uint32_t gy = 0; gy < Grid; ++gy)
            for (uint32_t gx = 0; gx < Grid; ++gx) {
                const auto o = cell(out[eye], ow, oh, gx, gy);
                const auto a = cell(in[eye], xw, xh, gx, gy);
                const auto b = cell(in[1 - eye], xw, xh, gx, gy);
                for (uint32_t c = 0; c < 3; ++c) {
                    same += std::abs(o[c] - a[c]);
                    cross += std::abs(o[c] - b[c]);
                }
            }
    same /= 2.0 * Grid * Grid * 3;
    cross /= 2.0 * Grid * Grid * 3;
    const bool mismatch = same > 8.0 || (cross > 4.0 && same >= cross);
    ++s.verify_compared;
    s.verify_mismatched += mismatch;
    s.verify_same = same;
    s.verify_cross = cross;
    if (mismatch) {
        LOG_ERROR(Render_Vulkan,
                  "FAST s1 verify: compared={} mismatched={} mean_abs_own_eye={:.2f} "
                  "mean_abs_other_eye={:.2f} (of 255, 32x32 cells; input {}x{} output {}x{}) "
                  "MISMATCH",
                  s.verify_compared, s.verify_mismatched, same, cross, xw, xh, ow, oh);
    } else {
        LOG_INFO(Render_Vulkan,
                 "FAST s1 verify: compared={} mismatched={} mean_abs_own_eye={:.2f} "
                 "mean_abs_other_eye={:.2f} (of 255, 32x32 cells; input {}x{} output {}x{})",
                 s.verify_compared, s.verify_mismatched, same, cross, xw, xh, ow, oh);
    }
}
void VrTemporalCapture::RetireViews(unsigned slot) {
    if (!views[slot].image) return;
    scheduler.DeferOperation([old=std::move(views[slot].eyes)]() mutable {
        for(auto& view:old) view.reset();
    });
    views[slot].image=nullptr;
    views[slot].uid=0;
    views[slot].format={};
}
bool VrTemporalCapture::Views(VideoCore::Image& image, bool depth, vk::Format format) {
    auto& v=views[depth?1:0];
    if (v.image==image.GetImage() && v.uid==image.image_uid && v.format==format) return true;
    RetireViews(depth?1:0);
    v.image=image.GetImage(); v.uid=image.image_uid; v.format=format;
    // Another format (S1 + DLSS 5's sRGB twin) is only sampled: the image's storage usage need
    // not be valid for it (the texture cache creates every image with a mutable format).
    const vk::ImageViewUsageCreateInfo sampled{.usage=vk::ImageUsageFlagBits::eSampled};
    for(uint32_t eye=0;eye<2;++eye) {
        auto result=instance.GetDevice().createImageViewUnique({
            .pNext=format!=vk::Format::eUndefined ? &sampled : nullptr,
            .image=v.image,
            .viewType=vk::ImageViewType::e2D,
            .format=format!=vk::Format::eUndefined ? format : image.backing->image.image_ci.format,
            .subresourceRange={depth?vk::ImageAspectFlagBits::eDepth:vk::ImageAspectFlagBits::eColor,0,1,eye,1}});
        if(result.result!=vk::Result::eSuccess) { RetireViews(depth?1:0); return false; }
        v.eyes[eye]=std::move(result.value);
    }
    return true;
}
void Rasterizer::VrTemporalBeforeDraw(const GraphicsPipeline* pipeline, bool indirect,
                                      bool indexed, u32 index_offset) {
    ScratchProfile::Cpu scratch_temporal(39, "Rasterizer::VrTemporalBeforeDraw");
    if (!NativeStereo::Enabled()) return;
    // S1 (bit 51): the guest itself renders at the input size, so no reduced-raster proxies.
    // Same recording thread as every reader of vr_temporal_scaled (draws, dispatches, VrEndFrame).
    const auto whole_frame = WholeFrameOutput();
    if (whole_frame.width && vr_temporal_scaled) {
        vr_temporal_scaled = false;
        LOG_INFO(Render_Vulkan, "VR temporal: S1 whole frame (FAST bit 51), output {}x{}; "
                 "reduced raster proxies off", whole_frame.width, whole_frame.height);
    }
    if (vr_temporal_scaled && pipeline_cache.GetProfile().max_push_constant_size < 136) {
        static bool warned{};
        if (!warned && VR::Graphics::Accepted().upscaler != VR::Graphics::Upscaler::Off) {
            LOG_WARNING(Render_Vulkan, "VR temporal: reduced raster requires 136 push-constant bytes; keeping native rendering");
            warned = true;
        }
        return;
    }
    const auto& key=pipeline->GetGraphicsKey();
    // Culled objects can still submit a direct draw with zero vertices/indices. Such a
    // draw writes neither depth nor motion: do not capture its camera, attach a motion
    // target, or count it as scene coverage. Indirect counts are only known on the GPU.
    const bool has_geometry = indirect || ScratchDraw::Registers(liverpool).num_indices != 0;
    bool gbuffer=has_geometry && key.probe_view_mask==3 &&
                 std::popcount(key.mrt_mask)>=5 && db_desc.first;
    if (gbuffer && g_temporal_main_extent && whole_frame.width &&
        whole_frame_input.width && whole_frame_input.height) {
        // Match the size actually patched at boot, even after an accepted preset change
        // that requires a restart. Same-size camera/depth ambiguity remains an error.
        const auto& depth = texture_cache.GetImage(db_desc.first);
        if (depth.info.size.width != whole_frame_input.width ||
            depth.info.size.height != whole_frame_input.height) {
            gbuffer = false;
            static bool noted{};
            if (!noted) {
                noted = true;
                LOG_INFO(Render_Vulkan,
                    "VR temporal main extent: exclude auxiliary depth {}x{} (uid {}); "
                    "boot scene {}x{}, output {}x{}",
                    depth.info.size.width,depth.info.size.height,depth.image_uid,
                    whole_frame_input.width,whole_frame_input.height,
                    whole_frame.width,whole_frame.height);
            }
        }
    }
    if (!vr_temporal) {
        if (!gbuffer || VR::Graphics::Accepted().upscaler==VR::Graphics::Upscaler::Off) return;
        vr_temporal=std::make_unique<VrTemporalCapture>(instance,scheduler);
        vr_temporal->s1.on = whole_frame.width && whole_frame.height;
        vr_temporal->s1.width = whole_frame.width;
        vr_temporal->s1.height = whole_frame.height;
        LOG_WARNING(Render_Vulkan,"VR temporal: {} enabled",vr_temporal->s1.on ? "S1 whole-frame reconstruction" :
            vr_temporal_scaled ? "reduced raster stereo" : "native resolution");
    }
    auto& t=*vr_temporal;
    if(t.frame!=vr_temporal_frame) {
        const auto accepted=VR::Graphics::Accepted();
        const auto accepted_key=VR::Graphics::Pack(accepted);
        // Bit 52 / vr.ini dlss_model: a model change recreates DLSS and, like a new accepted
        // configuration, retries a faulted provider (a refused model must not pin the next native).
        const auto dlss_preset=DlssPresetForFrame(accepted,vr_temporal_frame);
        if(accepted_key!=t.accepted_key || dlss_preset!=t.dlss_preset) { t.faulted=false; t.incomplete_frames=0; t.failure.clear(); t.s1.retry.Reset(); }
        t.accepted_key=accepted_key; t.dlss_preset=dlss_preset;
        if(gbuffer) {
            const auto& depth=texture_cache.GetImage(db_desc.first);
            t.width=depth.info.size.width; t.height=depth.info.size.height;
        }
        t.frame=vr_temporal_frame;
        if (FastRender::On(MotionFast)) LogMotionFast();
        if (t.frame % 600 == 0) t.index_ranges.Trim(t.frame);
        t.depth_id={}; t.color_id={};
        t.motion_characters=t.motion_player=t.motion_camera_only=0;
        t.depth_uid=t.color_uid=0; t.draws=t.motion_draws=t.refused=0; t.index_syncs=t.index_writes=0; t.resolved=false; t.reconstructed=false;
        const auto profile = VR::Graphics::EffectiveProfile(accepted);
        const auto input = VR::Graphics::InputResolution({t.width,t.height},
            vr_temporal_scaled ? accepted.upscaler : VR::Graphics::Upscaler::Off, profile);
        t.input_width=input.width; t.input_height=input.height; t.reduced_draws=0;
        // S1: the guest's size is the input; the provider writes the headset's output size.
        const uint32_t output_width = t.s1.on ? t.s1.width : t.width;
        const uint32_t output_height = t.s1.on ? t.s1.height : t.height;
        t.s1.pending = t.s1.valid = false;
        VrTemporalPair::Plan plan{{{{t.input_width,t.input_height,output_width,output_height},
                                    {t.input_width,t.input_height,output_width,output_height}}},false};
        plan.provider_flags=UpscalerProviderFlags(accepted);
        plan.dlss_preset=dlss_preset; // a change recreates both DLSS eyes
        // FAST_GPU_S1_ASYM: S1's provider latched by incomplete frames waits, configured and
        // idle, until its retry frame; then it runs again and ProbeFailures more incomplete
        // frames in a row latch it again (vr_temporal_failure.h).
        bool idle=false;
        if(t.faulted && t.s1.on && g_s1_retry && t.s1.retry.Scheduled()) {
            if(t.s1.retry.Waiting(t.frame)) idle=true;
            else {
                t.faulted=false; t.failure.clear();
                t.s1.retry.Start(t.incomplete_frames);
                ++t.s1.retries;
                LOG_INFO(Render_Vulkan,"VR temporal S1: frame {} retries the provider without jitter (retry {})",
                         t.frame,t.s1.retries);
            }
        }
        if (accepted.upscaler == VR::Graphics::Upscaler::Fsr31 && !t.s1.on) {
            t.faulted = true;
            t.failure = "FSR 3.1.5 requires whole-frame reconstruction; enable it and restart";
            VR::Graphics::PublishStatus(accepted, VR::Graphics::RunState::Unavailable);
            RENDER_DIAGNOSTIC_ONCE("{}", t.failure);
        }
        auto request=accepted;
        if(t.faulted) request.upscaler=VR::Graphics::Upscaler::Off;
        if(idle) S1RetryIdle(t,request,plan);
        else {
            // S1: DLSS 5 runs too (VrS1Reconstruct feeds its linear chain through sRGB views of
            // X). A provider whose scale differs from the boot patch's still reconstructs from the
            // patched size until the restart the status asks for (ResolutionRestartRequired).
            const bool recovery_probe = t.s1.on && t.s1.retry.probing;
            const auto result=t.scene.Begin(t.frame,request,plan,false,recovery_probe);
            t.active=result==VrTemporalPair::Result::Ready;
            if (result==VrTemporalPair::Result::Failed || result==VrTemporalPair::Result::Unavailable ||
                result==VrTemporalPair::Result::Invalid) {
                t.faulted=true; t.failure=t.scene.Problem();
                t.s1.retry.Reset(); // a provider failure is not retried
                VR::Graphics::PublishStatus(accepted, VR::Graphics::RunState::Unavailable);
                LOG_BENCHMARK_WARNING(Render_Vulkan,"VR temporal native: unavailable ({})",t.failure);
            }
        }
    }
    t.gbuffer=gbuffer && t.active && !t.resolved;
    // Static scenery uses depth/camera reprojection, without a motion shader variant,
    // position history, or index readback. Keep the broad path for matched A/B tests.
    t.object_candidate = false;
    t.motion_index_data = {};
    t.motion_index_address = 0;
    if (t.gbuffer) {
        const bool character = HasCharacterSkeleton(pipeline);
        const bool player = !character && IsPlayerMotion(pipeline->GetStage(Shader::LogicalStage::Vertex));
        t.object_candidate = FullObjectMotion() || character || player;
        t.motion_characters += character;
        t.motion_player += player;
        t.motion_camera_only += !t.object_candidate;
    }
    if(!t.active || t.resolved) return;
    if(gbuffer) {
        // Motion history observes indices; it must never make guest memory coherent by
        // writing a widened GPU readback over neighboring allocations after a load.
        // Prepare before draw binding because genuinely GPU-produced indices may wait.
        if (indexed && t.object_candidate) {
            const auto& regs = ScratchDraw::Registers(liverpool);
            const u32 stride = regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2 : 4;
            const VAddr address = regs.index_base_address.Address<VAddr>() + u64(index_offset) * stride;
            const u64 bytes = u64(regs.num_indices) * stride;
            if (address && bytes && bytes <= UINT32_MAX && IsMapped(address, bytes)) {
                t.motion_index_address = address;
                t.motion_index_data = buffer_cache.ReadForMotion(address, static_cast<u32>(bytes));
                if (!t.motion_index_data.empty() &&
                    t.motion_index_data.data() != reinterpret_cast<const u8*>(address)) {
                    ++t.index_syncs;
                    ++t.index_writes;
                }
            }
        }
        const auto& depth=texture_cache.GetImage(db_desc.first);
        if (depth.info.size.width!=t.width || depth.info.size.height!=t.height ||
            !depth.native_stereo || depth.backing->num_samples!=1 ||
            (t.depth_id && (t.depth_id!=db_desc.first || t.depth_uid!=depth.image_uid))) {
            t.scene.IncompleteMotion(); ++t.refused;
        } else { t.depth_id=db_desc.first; t.depth_uid=depth.image_uid; }
        ++t.draws;
        if(indirect) { t.scene.IncompleteMotion(); ++t.refused; }
    }
    if(t.depth_id && db_desc.first==t.depth_id && cb_descs[0].first && std::popcount(key.mrt_mask)<=2) {
        const auto& color=texture_cache.GetImage(cb_descs[0].first);
        if(color.native_stereo && color.backing->num_samples==1 &&
            color.info.size.width==t.width && color.info.size.height==t.height &&
            color.backing->image.image_ci.format==vk::Format::eR16G16B16A16Sfloat) {
            t.color_id=cb_descs[0].first; t.color_uid=color.image_uid;
        }
    }
}
const GraphicsPipeline* Rasterizer::VrTemporalSelect(const GraphicsPipeline* pipeline, RenderState& state) {
    ScratchProfile::Cpu scratch_temporal(40, "Rasterizer::VrTemporalSelect");
    if(!vr_temporal || !vr_temporal->gbuffer || !vr_temporal->object_candidate) return pipeline;
    auto& t=*vr_temporal;
    auto* objects=t.scene.Objects();
    const GraphicsPipeline* variant=nullptr;
    bool deferred=false; // the variant still compiles (bit 40: never waited for here)
    // The live motion buffers. With VrPushData the variant is the same for every allocation (the
    // cache keeps it by its flag) and the draw pushes these addresses (vr_motion_buffers).
    const auto motion=objects ? objects->Specialization() : Shader::VrMotion{};
    if(objects && FastRender::On(MotionFast)) {
        // Toggle bit 36: GetVrMotionPipeline takes the selection lock that stage A holds while it
        // selects each draw's pipeline, for every object-motion draw. Its answer for a base, once
        // non-null, is the cache entry {motion, variant}: a hit needs the same motion and passes
        // checks on the pipeline (constant for it) and two per-draw ones, repeated here. New
        // buffers miss here once per base (the cache's own entry then hits, VrMotionVariant).
        ++g_motion_fast.variant_calls;
        const auto it=t.motion_variants.find(pipeline);
        if(it!=t.motion_variants.end() && it->second.motion==motion && NativeStereo::Enabled() &&
            !ScratchDraw::Registers(liverpool).clipper_control.clip_disable) {
            ++g_motion_fast.variant_memo;
            variant=it->second.variant;
            if(MotionVerifySample()) {
                const auto* slow=pipeline_cache.GetVrMotionPipeline(pipeline,motion);
                if(slow!=variant) {
                    ++g_motion_fast.verify_mismatches;
                    LOG_ERROR(Render_Vulkan,"FAST motion verify: motion variant of {} is {} memo {}",
                        static_cast<const void*>(pipeline),static_cast<const void*>(slow),
                        static_cast<const void*>(variant));
                    t.motion_variants.erase(pipeline);
                    variant=slow;
                }
            }
        } else {
            variant=pipeline_cache.GetVrMotionPipeline(pipeline,motion,false,&deferred);
            // A null answer may or may not have replaced the cache entry: forget ours.
            if(variant) t.motion_variants.insert_or_assign(pipeline,VrTemporalCapture::MotionVariant{motion,variant});
            else t.motion_variants.erase(pipeline);
        }
    } else {
        if(!t.motion_variants.empty()) t.motion_variants.clear();
        variant=objects ? pipeline_cache.GetVrMotionPipeline(pipeline,motion,false,&deferred) : nullptr;
    }
    if(deferred) {
        // Fast bit 40: its motion variant still compiles in the background. Meanwhile the draw
        // goes as static scenery does (camera motion only): marking the frame's motion
        // incomplete would drop every frame's reconstruction until it is ready, and 8 such
        // frames in a row turn the provider off (TemporalFailure).
        return pipeline;
    }
    const bool attached=variant && objects->Attach(state);
    if(!attached) {
        if(t.refused<3) {
            const auto& key=pipeline->GetGraphicsKey();
            LOG_WARNING(Render_Vulkan,
                        "VR temporal motion: {} (views={}, samples={}, MRTs={}, mask={:#x}, "
                        "clip_disabled={})",
                        variant ? "target attachment refused" : "pipeline variant refused",
                        key.probe_view_mask, key.num_samples, key.num_color_attachments,
                        key.mrt_mask,
                        bool(ScratchDraw::Registers(liverpool).clipper_control.clip_disable));
            const auto& regs = ScratchDraw::Registers(liverpool);
            LOG_WARNING(Render_Vulkan,"VR temporal motion eligibility: primitive={} clip_dist={} user_clip={} state_views={} attachments={}",
                uint32_t(key.prim_type),uint32_t(regs.vs_output_control.clip_distance_enable),
                uint32_t(regs.clipper_control.user_clip_plane_enable),state.probe_view_mask,state.num_color_attachments);
            for(const auto* stage:pipeline->GetStages()) if(stage)
                LOG_WARNING(Render_Vulkan,"VR temporal motion stage: hardware={} logical={} hash={:#x} reserved={}",
                    uint32_t(stage->stage),uint32_t(stage->l_stage),stage->pgm_hash,stage->uses_reserved_motion_varyings);
        }
        t.scene.IncompleteMotion(); ++t.refused; return pipeline;
    }
    vr_motion_buffers=motion;
    return variant;
}
uint32_t Rasterizer::VrTemporalPrepareDraw(const GraphicsPipeline* pipeline, uint64_t geometry,
        bool indexed, uint32_t index_offset, uint32_t vertex_offset, uint32_t first_instance) {
    ScratchProfile::Cpu scratch_temporal(41, "Rasterizer::VrTemporalPrepareDraw");
    if(!vr_temporal || !pipeline->HasVrMotion()) return 0;
    const auto& regs = ScratchDraw::Registers(liverpool);
    // No vertex invocation means no missing motion, even if a motion pipeline was selected.
    if (regs.num_indices == 0) return 0;
    auto& t=*vr_temporal;
    auto* objects=t.scene.Objects();
    if(!objects) return 0;
    const auto refuse=[&](const char* why) {
        if(t.refused<3) LOG_WARNING(Render_Vulkan,"VR temporal motion draw refused: {}",why);
        t.scene.IncompleteMotion(); ++t.refused; return 0u;
    };
    if(!geometry) return refuse("vertex identity unavailable");
    Motion::VertexRange range{vertex_offset,regs.num_indices};
    uint64_t index_address=0, topology=0;
    if(indexed) {
        const uint32_t bytes_per_index=regs.index_buffer_type.index_type==AmdGpu::IndexType::Index16?2:4;
        index_address=regs.index_base_address.Address<VAddr>()+uint64_t(index_offset)*bytes_per_index;
        const uint64_t bytes=uint64_t(regs.num_indices)*bytes_per_index;
        if (t.motion_index_address != index_address || t.motion_index_data.size() != bytes) {
            return refuse("index snapshot unavailable");
        }
        const auto* data = static_cast<const void*>(t.motion_index_data.data());
        const bool guest_data = data == reinterpret_cast<const void*>(index_address);
        const auto scan = [&] {
            return bytes_per_index==2 ? Motion::IndexedRange(std::span(static_cast<const uint16_t*>(data),regs.num_indices),int32_t(vertex_offset),regs.enable_primitive_restart!=0)
                : Motion::IndexedRange(std::span(static_cast<const uint32_t*>(data),regs.num_indices),int32_t(vertex_offset),regs.enable_primitive_restart!=0);
        };
        if(guest_data && FastRender::On(MotionFast) && TemporalOptimizations()) {
            // Toggle bit 36: hash only when the bytes may have changed (ProvenIndexRange).
            const auto proven=ProvenIndexRange(buffer_cache,t.index_ranges,
                {index_address,regs.num_indices,bytes_per_index,int32_t(vertex_offset),regs.enable_primitive_restart!=0},
                bytes,t.frame,scan);
            range=proven.range;
            topology=proven.topology;
        } else {
            topology=XXH3_64bits(data,bytes);
            range = TemporalOptimizations() ? t.index_ranges.GetVerified(
                {index_address,regs.num_indices,bytes_per_index,int32_t(vertex_offset),regs.enable_primitive_restart!=0},
                topology,t.frame,scan).range : scan();
        }
    }
    if(!range.count) return refuse("empty or invalid index range");
    const auto index=objects->Prepare({.shader=pipeline->GetStage(Shader::LogicalStage::Vertex).pgm_hash,
        .geometry=geometry,.indices=index_address,.topology=topology,.index_count=regs.num_indices,
        .instances=regs.num_instances.NumInstances(),.first_instance=first_instance,.vertices=range});
    if(!index) return refuse("position history allocation");
    ++t.motion_draws;
    return index;
}
void Rasterizer::VrTemporalDispatch(uint64_t shader) {
    ScratchProfile::Cpu scratch_temporal(42, "Rasterizer::VrTemporalDispatch");
    if(!vr_temporal || shader!=0x9a9cf8a9 || !vr_temporal->active || vr_temporal->resolved) return;
    auto& t=*vr_temporal;
    t.resolved=true;
    if(t.s1.on) {
        // S1: the scene is complete (jitter and object motion end here as before); the provider
        // takes the tonemapped frame at the composite instead (VrS1Reconstruct).
        t.s1.pending=true;
        return;
    }
    auto* color=texture_cache.TryGetImage(t.color_id,t.color_uid);
    auto* depth=texture_cache.TryGetImage(t.depth_id,t.depth_uid);
    const auto fail=[&](const char* why, bool persistent = false) {
        // Loading/camera cuts can invalidate one frame's motion. Resolve resets history;
        // retry the next complete scene rather than disabling the provider until a menu edit.
        // Repeated missing object coverage must not leave a permanently reduced, spatially
        // enlarged image while the menu says the temporal provider is enabled.
        persistent = TemporalFailure(t.incomplete_frames, persistent);
        t.faulted=persistent; t.failure=why; t.scene.IncompleteMotion();
        VR::Graphics::PublishStatus(VR::Graphics::Unpack(t.accepted_key), persistent ?
            VR::Graphics::RunState::Unavailable : VR::Graphics::RunState::Retrying);
        LOG_BENCHMARK_WARNING(Render_Vulkan,"VR temporal native: frame {} {}: {} (draws={} motion={} refused={})",
            t.frame,persistent ? "refused" : "skipped; retry next frame",why,t.draws,t.motion_draws,t.refused);
    };
    if(!color || !depth || !color->native_stereo || !depth->native_stereo ||
        color->GetImage()==depth->GetImage() || !t.Views(*color,false) || !t.Views(*depth,true)) {
        fail("scene color/depth or native eye views unavailable"); return;
    }
    scheduler.EndRendering();
    if (GpuPassProfiler::For(scheduler) != nullptr) [[unlikely]] {
        // FAST_GPU_PASS_PROFILE (bit 38): the provider's resolve of both eyes.
        const auto upscaler = VR::Graphics::Unpack(t.accepted_key).upscaler;
        const char* const label =
            upscaler == VR::Graphics::Upscaler::Dlss   ? "VR temporal resolve (DLSS, both eyes)"
            : upscaler == VR::Graphics::Upscaler::Fsr31 ? "VR temporal resolve (FSR 3.1.5, both eyes)"
            : upscaler == VR::Graphics::Upscaler::Fsr4 ? "VR temporal resolve (FSR 4, both eyes)"
                                                       : "VR temporal resolve (both eyes)";
        GpuPassProfiler::MarkAt(scheduler, 0x7E3B'0000'0000ull | static_cast<u64>(upscaler),
                                GpuPassProfiler::Group::Temporal, label);
    }
    if(!vr_temporal_scaled) {
        color->Transit(vk::ImageLayout::eGeneral,vk::AccessFlagBits2::eShaderRead,{});
        depth->Transit(vk::ImageLayout::eGeneral,vk::AccessFlagBits2::eShaderRead,{});
    }
    std::array<VrTemporalImages::Source,2> sources;
    if(vr_temporal_scaled) {
        if(!color->scene_proxy || !depth->scene_proxy || !t.reduced_draws) {
            fail("reduced raster inputs missing"); return;
        }
        // Bit 46 also passes the images behind the views, so DLSS may read them in place.
        auto& color_proxy=*color->scene_proxy;
        auto& depth_proxy=*depth->scene_proxy;
        for(uint32_t eye=0;eye<2;++eye) sources[eye]={color_proxy.Read(*color,eye),
            depth_proxy.Read(*depth,eye),t.input_width,t.input_height,color_proxy.ReadImage(),
            depth_proxy.ReadImage(),color_proxy.ReadFormat(),depth_proxy.ReadFormat(),eye};
    } else for(uint32_t eye=0;eye<2;++eye)
        sources[eye]={*t.views[0].eyes[eye],*t.views[1].eyes[eye],t.width,t.height,
            color->GetImage(),depth->GetImage(),color->backing->image.image_ci.format,
            depth->backing->image.image_ci.format,eye};
    const auto now=std::chrono::steady_clock::now();
    const float frame_ms=t.last_resolve.time_since_epoch().count() ?
        std::clamp(std::chrono::duration<float,std::milli>(now-t.last_resolve).count(),1.f,1000.f):11.111f;
    t.last_resolve=now;
    const auto result=t.scene.Resolve(sources,frame_ms);
    const auto output=t.scene.Output();
    if(result!=VrTemporalPair::Result::Ready || !output) {
        fail(t.scene.Problem().empty()?"temporal resolve refused or provider failed":t.scene.Problem().c_str(),
             result==VrTemporalPair::Result::Failed || result==VrTemporalPair::Result::Unavailable);
        return;
    }
    t.incomplete_frames = 0;
    // Bloodborne stores postprocessing data in scene alpha. Preserve it exactly as bbport
    // does, instead of copying undefined/provider-owned alpha over the game's data.
    // Optimized merge writes every RGB/alpha pixel from provider/canonical input.
    // Suppress proxy resolve only for this overwrite transition; keep the reduced
    // source valid until merge succeeds so the ordinary failure path can still flush it.
    GpuPassProfiler::MarkAt(scheduler, 0x7E3B'0000'0100ull, GpuPassProfiler::Group::Temporal,
                            "VR temporal merge into the scene (RGB, game alpha kept)");
    if (color->scene_proxy && TemporalOptimizations()) color->scene_proxy->attachment_scope = true;
    color->Transit(vk::ImageLayout::eGeneral,
        vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite, {});
    if (color->scene_proxy && TemporalOptimizations()) color->scene_proxy->attachment_scope = false;
    if (!t.scene.Merge({*t.views[0].eyes[0], *t.views[0].eyes[1]})) {
        fail("HDR RGB merge unavailable", true); return;
    }
    t.reconstructed = true;
    UpscalerFastNote(g_upscaler_fast.resolved);
    VR::Graphics::PublishStatus(VR::Graphics::Unpack(t.accepted_key), VR::Graphics::RunState::Active,
        {t.input_width,t.input_height},{t.width,t.height});
    if(color->scene_proxy)color->scene_proxy->NativeWritten();
    color->flags|=VideoCore::ImageFlagBits::GpuModified;
    color->flags&=~VideoCore::ImageFlagBits::Dirty;
    if(t.frame<10 || t.frame%60==0)
        LOG_BENCHMARK_INFO(Render_Vulkan,"VR temporal native: frame {} both eyes recorded {}x{}, motion draws={}/{}, raster {}x{} -> {}x{}, reduced_draws={}, index_syncs={} index_writes={} characters={} player={} camera_only={}",
            t.frame,t.width,t.height,t.motion_draws,t.draws,t.input_width,t.input_height,t.width,t.height,t.reduced_draws,t.index_syncs,t.index_writes,t.motion_characters,t.motion_player,t.motion_camera_only);
}
// S1 (bit 51): right after the composite draw (its target X: the tonemapped frame at the input
// size, scene jittered) and before our in-frame items and the UI layer's scene copy. The provider
// reconstructs X into the output-size twin (VrTemporalCapture::s1.color); the game keeps X for
// its UI, the desktop and the fallback eyes.
void Rasterizer::VrS1Reconstruct() {
    ScratchProfile::Cpu scratch_temporal(43, "Rasterizer::VrS1Reconstruct");
    auto& t=*vr_temporal;
    auto& s=t.s1;
    s.pending=false;
    s.valid=false;
    ++s.frames;
    const auto fail=[&](const char* why, bool persistent = false) {
        // FAST_GPU_S1_ASYM: a streak of incomplete frames (not a provider failure) idles the
        // provider until a retry instead of turning it off for the session.
        const bool retry = g_s1_retry && !persistent;
        persistent = TemporalFailure(t.incomplete_frames, persistent, t.draws != 0);
        if(persistent) {
            if(retry) S1RetryLatch(t,why);
            else s.retry.Reset();
        }
        t.faulted=persistent; t.failure=why; t.scene.IncompleteMotion(); ++s.failed;
        VR::Graphics::PublishStatus(VR::Graphics::Unpack(t.accepted_key), persistent && !retry ?
            VR::Graphics::RunState::Unavailable : VR::Graphics::RunState::Retrying);
        LOG_BENCHMARK_WARNING(Render_Vulkan,"VR temporal native: frame {} {}: {} (S1; draws={} motion={} refused={})",
            t.frame,!persistent ? "skipped; retry next frame" : retry ? "skipped; retry later (idle)" : "refused",
            why,t.draws,t.motion_draws,t.refused);
        scheduler.GetDynamicState().Invalidate(); // a refused provider may have recorded passes
    };
    // X: the first HUD draw's own target when it is the scene's size, else the last colour target
    // of the scene's size drawn before it (VrTrackTargets, as for the DIBR/UI capture).
    VideoCore::Image* x=nullptr;
    VideoCore::ImageId x_id{};
    if(const auto own_id=cb_descs[0].first; own_id && cb_descs[0].second.view_info.range.base.layer==0) {
        auto& own=texture_cache.GetImage(own_id);
        if(own.info.size.width==t.width && own.info.size.height==t.height) { x=&own; x_id=own_id; }
    }
    if(!x) {
        const VrColorTarget* best=nullptr;
        for(const auto& target : vr_dibr.color)
            if(target.id && target.extent==vk::Extent2D{t.width,t.height} &&
               (!best || target.last_draw>best->last_draw)) best=&target;
        x=best ? texture_cache.TryGetImage(best->id,best->uid) : nullptr;
        if(x) x_id=best->id;
    }
    auto* depth=texture_cache.TryGetImage(t.depth_id,t.depth_uid);
    // DLSS 5: its chain composes in linear light (HDR input, auto exposure; dlss5_bridge
    // compose.comp), so the provider samples X through its sRGB twin (linear 0..1) and the output
    // is encoded back into X's bytes below. DLSS and FSR 4 take X's bytes as they are.
    const bool neural=VR::Graphics::Neural(VR::Graphics::Unpack(t.accepted_key));
    const vk::Format x_srgb=neural && x ? S1SrgbTwin(x->backing->image.image_ci.format)
                                        : vk::Format::eUndefined;
    if(neural && x && x_srgb==vk::Format::eUndefined) {
        if (s.failed < 4)
            LOG_WARNING(Render_Vulkan,"VR temporal S1: DLSS 5 needs an 8-bit UNORM composite target, "
                        "not {}",vk::to_string(x->backing->image.image_ci.format));
        fail("S1: DLSS 5 needs linear input; the composite target has no sRGB view",true); return;
    }
    if(!x || !depth || !x->native_stereo || !depth->native_stereo || x->backing->num_samples!=1 ||
        x->info.size.width!=t.width || x->info.size.height!=t.height ||
        x->GetImage()==depth->GetImage() ||
        !t.Views(*x,false,x_srgb) || !t.Views(*depth,true)) {
        if (s.failed < 4)
            LOG_WARNING(Render_Vulkan,
                        "VR temporal S1: composite x={} {}x{} stereo={} samples={} tracked_draws={} | t {}x{} "
                        "| depth={} stereo={} same_image={} | views x={} depth={}",
                        x != nullptr, x ? x->info.size.width : 0u, x ? x->info.size.height : 0u,
                        x && x->native_stereo, x ? u32(x->backing->num_samples) : 0u,
                        u32(vr_dibr.draws), t.width, t.height,
                        depth != nullptr, depth && depth->native_stereo,
                        x && depth && x->GetImage() == depth->GetImage(),
                        x && t.Views(*x, false, x_srgb), depth && t.Views(*depth, true));
        fail("S1: composite target or scene depth unavailable"); return;
    }
    if(!t.S1Color(*x) || (neural && !t.S1Encode(x_srgb))) {
        fail("S1: output scene unavailable",true); return;
    }
    scheduler.EndRendering();
    const auto upscaler=VR::Graphics::Unpack(t.accepted_key).upscaler;
    GpuPassProfiler::MarkAt(scheduler, 0x7E3B'0000'0300ull | static_cast<u64>(upscaler),
        GpuPassProfiler::Group::Temporal,
        neural ? "VR S1 reconstruction (DLSS 5, both eyes, after the composite)"
        : upscaler==VR::Graphics::Upscaler::Dlss ? "VR S1 reconstruction (DLSS, both eyes, after the composite)"
                                                 : "VR S1 reconstruction (FSR 4, both eyes, after the composite)");
    x->Transit(vk::ImageLayout::eGeneral,vk::AccessFlagBits2::eShaderRead,{});
    depth->Transit(vk::ImageLayout::eGeneral,vk::AccessFlagBits2::eShaderRead,{});
    std::array<VrTemporalImages::Source,2> sources;
    for(uint32_t eye=0;eye<2;++eye)
        sources[eye]={*t.views[0].eyes[eye],*t.views[1].eyes[eye],t.width,t.height};
    const auto now=std::chrono::steady_clock::now();
    const float frame_ms=t.last_resolve.time_since_epoch().count() ?
        std::clamp(std::chrono::duration<float,std::milli>(now-t.last_resolve).count(),1.f,1000.f):11.111f;
    t.last_resolve=now;
    const auto result=t.scene.Resolve(sources,frame_ms);
    const auto output=t.scene.Output();
    if(result!=VrTemporalPair::Result::Ready || !output) {
        fail(t.scene.Problem().empty()?"temporal resolve refused or provider failed":t.scene.Problem().c_str(),
             result==VrTemporalPair::Result::Failed || result==VrTemporalPair::Result::Unavailable);
        return;
    }
    t.incomplete_frames=0;
    if(s.retry.Success()) { // FAST_GPU_S1_ASYM: a retry that reconstructs; the wait starts over
        ++s.recovered;
        LOG_INFO(Render_Vulkan,"VR temporal S1: frame {} reconstructed again (FAST_GPU_S1_ASYM, retry {})",
                 t.frame,s.retries);
    }
    // The provider's eyes (RGBA16F, GENERAL) into the twin's layers: same size, a format
    // conversion only. Alpha is the provider's; the eyes' projection layers ignore it.
    GpuPassProfiler::MarkAt(scheduler, 0x7E3B'0000'0310ull, GpuPassProfiler::Group::Temporal,
                            "VR S1 output copy (provider eyes into the output-size scene)");
    const auto cmd=scheduler.Commands();
    const vk::MemoryBarrier2 provider_done{
        .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask=vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,
        .dstAccessMask=vk::AccessFlagBits2::eTransferRead};
    cmd.pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount=1,.pMemoryBarriers=&provider_done});
    s.color_layout=vk::ImageLayout::eUndefined; // every texel is replaced
    t.S1ColorTo(vk::ImageLayout::eTransferDstOptimal,vk::PipelineStageFlagBits2::eTransfer,
                vk::AccessFlagBits2::eTransferWrite);
    // DLSS 5: blit (encode) into the sRGB twin, then copy its bytes into the output scene.
    const vk::Image blit_target=neural ? s.encode.image : s.color.image;
    const auto encode_to=[&](vk::ImageLayout from, vk::ImageLayout to, vk::AccessFlags2 src,
                             vk::AccessFlags2 dst) {
        const vk::ImageMemoryBarrier2 barrier{
            .srcStageMask=vk::PipelineStageFlagBits2::eTransfer,.srcAccessMask=src,
            .dstStageMask=vk::PipelineStageFlagBits2::eTransfer,.dstAccessMask=dst,
            .oldLayout=from,.newLayout=to,
            .srcQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,.dstQueueFamilyIndex=VK_QUEUE_FAMILY_IGNORED,
            .image=s.encode.image,.subresourceRange={vk::ImageAspectFlagBits::eColor,0,1,0,2}};
        cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount=1,.pImageMemoryBarriers=&barrier});
    };
    if(neural) // every texel is replaced; the previous frame's copy out of it was a transfer read
        encode_to(vk::ImageLayout::eUndefined,vk::ImageLayout::eTransferDstOptimal,{},
                  vk::AccessFlagBits2::eTransferWrite);
    for(uint32_t eye=0;eye<2;++eye) {
        const auto& o=(*output)[eye];
        const vk::ImageBlit region{
            .srcSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
            .srcOffsets=std::array<vk::Offset3D,2>{vk::Offset3D{0,0,0},
                vk::Offset3D{static_cast<int32_t>(o.width),static_cast<int32_t>(o.height),1}},
            .dstSubresource={vk::ImageAspectFlagBits::eColor,0,eye,1},
            .dstOffsets=std::array<vk::Offset3D,2>{vk::Offset3D{0,0,0},
                vk::Offset3D{static_cast<int32_t>(s.width),static_cast<int32_t>(s.height),1}}};
        cmd.blitImage(vk::Image{o.image},vk::ImageLayout::eGeneral,blit_target,
                      vk::ImageLayout::eTransferDstOptimal,region,vk::Filter::eNearest);
    }
    if(neural) {
        encode_to(vk::ImageLayout::eTransferDstOptimal,vk::ImageLayout::eTransferSrcOptimal,
                  vk::AccessFlagBits2::eTransferWrite,vk::AccessFlagBits2::eTransferRead);
        const vk::ImageCopy copy{.srcSubresource={vk::ImageAspectFlagBits::eColor,0,0,2},
                                 .dstSubresource={vk::ImageAspectFlagBits::eColor,0,0,2},
                                 .extent={s.width,s.height,1}};
        cmd.copyImage(s.encode.image,vk::ImageLayout::eTransferSrcOptimal,s.color.image,
                      vk::ImageLayout::eTransferDstOptimal,copy);
    }
    t.S1Verify(*x,s.reconstructed); // FAST_S1_VERIFY=N only
    // Between two game draws (not dispatches, as the old resolve point): the next draw sets its
    // dynamic state again after the provider's passes.
    scheduler.GetDynamicState().Invalidate();
    s.valid=true;
    s.x_id=x_id;
    s.x_uid=x->image_uid;
    ++s.reconstructed;
    t.reconstructed=true;
    UpscalerFastNote(g_upscaler_fast.resolved);
    VR::Graphics::PublishStatus(VR::Graphics::Unpack(t.accepted_key), VR::Graphics::RunState::Active,
        {t.width,t.height},{s.width,s.height});
    if(t.frame<10 || t.frame%60==0)
        LOG_BENCHMARK_INFO(Render_Vulkan,"VR temporal native: frame {} both eyes recorded {}x{}, motion draws={}/{}, raster {}x{} -> {}x{}, reduced_draws={}, index_syncs={} index_writes={} characters={} player={} camera_only={} (S1 whole frame, {} after the composite)",
            t.frame,s.width,s.height,t.motion_draws,t.draws,t.width,t.height,s.width,s.height,t.draws,t.index_syncs,t.index_writes,t.motion_characters,t.motion_player,t.motion_camera_only,
            neural ? "DLSS 5, linear in, sRGB-encoded out" : "LDR");
}
void Rasterizer::VrTemporalEndFrame() {
    // S1 (bit 51): before the status below, which it does not change.
    if(vr_temporal && vr_temporal->s1.on) {
        auto& s=vr_temporal->s1;
        if(s.pending) { s.pending=false; ++s.missed; } // no composite after the scene
        s.valid=false;
        uint32_t uploads=0, resolves=0;
        for(auto [id,uid]:vr_temporal->proxies)
            if(auto* image=texture_cache.TryGetImage(id,uid);image && image->scene_proxy) {
                uploads+=std::exchange(image->scene_proxy->uploads,0);
                resolves+=std::exchange(image->scene_proxy->resolves,0);
            }
        ReportWholeFrame(vr_temporal_frame,*vr_temporal,resolves,uploads);
    }
    if (!NativeStereo::Enabled() ||
        (vr_temporal_scaled && pipeline_cache.GetProfile().max_push_constant_size < 136)) {
        VR::Graphics::PublishStatus(VR::Graphics::Accepted(), VR::Graphics::RunState::Unsupported);
    } else if (!vr_temporal || (!vr_temporal->faulted &&
               (!vr_temporal->active || !vr_temporal->draws))) {
        VR::Graphics::PublishStatus(VR::Graphics::Accepted(),
            vr_temporal && vr_temporal->scene.Preparing() ? VR::Graphics::RunState::Preparing :
                                                          VR::Graphics::RunState::Waiting);
    }
    if(vr_temporal && vr_temporal->active && !vr_temporal->resolved && vr_temporal->draws) {
        vr_temporal->faulted=true;
        vr_temporal->failure="scene post-processing trigger was not seen";
        // FAST_GPU_S1_ASYM: S1 retries this too (a frame the game drew without its post-processing).
        const bool retry=g_s1_retry && vr_temporal->s1.on;
        if(retry) S1RetryLatch(*vr_temporal,vr_temporal->failure.c_str());
        else vr_temporal->s1.retry.Reset();
        VR::Graphics::PublishStatus(VR::Graphics::Unpack(vr_temporal->accepted_key),
            retry ? VR::Graphics::RunState::Retrying : VR::Graphics::RunState::Unavailable);
        if(retry)
            LOG_BENCHMARK_WARNING(Render_Vulkan,"VR temporal native: frame {} skipped; retry later (idle): {}",
                        vr_temporal->frame,vr_temporal->failure);
        else
            LOG_BENCHMARK_WARNING(Render_Vulkan,"VR temporal native: {}",vr_temporal->failure);
    }
    if(vr_temporal_scaled && vr_temporal) {
        uint32_t uploads=0, resolves=0;
        for(auto [id,uid]:vr_temporal->proxies)
            if(auto* image=texture_cache.TryGetImage(id,uid);image && image->scene_proxy) {
                auto& proxy=*image->scene_proxy;
                proxy.NativeAccess(*image,vk::AccessFlagBits2::eMemoryRead);
                uploads+=std::exchange(proxy.uploads,0); resolves+=std::exchange(proxy.resolves,0);
            }
        if(vr_temporal_frame%60==0 && vr_temporal->active)
            LOG_INFO(Render_Vulkan,"VR temporal proxy: uploads={} resolves={}",uploads,resolves);
    }
    ReportUpscalerFast(vr_temporal_frame, vr_temporal && vr_temporal->active);
    ReportFsr4Interleave(vr_temporal_frame);
    ReportDlssDirect(vr_temporal_frame);
    ++vr_temporal_frame;
}
} // namespace Vulkan

namespace Vulkan {
bool VrSceneProxy::Eligible(const VideoCore::Image& source) {
    if (!source.native_stereo || !source.backing || source.backing->num_samples != 1 ||
        source.info.resources.levels != 1 || source.backing->image.image_ci.arrayLayers != 2 ||
        source.info.props.is_block)
        return false;
    const auto features = source.instance->GetPhysicalDevice()
                              .getFormatProperties(source.backing->image.image_ci.format)
                              .optimalTilingFeatures;
    const auto need = vk::FormatFeatureFlagBits::eBlitSrc | vk::FormatFeatureFlagBits::eBlitDst;
    return (features & need) == need;
}
VrSceneProxy::VrSceneProxy(VideoCore::Image& source, uint32_t width, uint32_t height,
                           VideoCore::TextureCache& texture_cache)
    : instance(*source.instance), scheduler(*source.scheduler),
      image(instance.GetDevice(), instance.GetAllocator()), native(source.GetImage()),
      aspects(source.aspect_mask) {
    auto ci = source.backing->image.image_ci;
    ci.extent = {width, height, 1};
    ci.mipLevels = 1;
    ci.arrayLayers = 2;
    ci.initialLayout = vk::ImageLayout::eUndefined;
    ci.usage |= vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst;
    if (!image.TryCreate(ci)) {
        u64 freed = 0;
        const bool recovered = VideoCore::TextureMemory::Recover(
            source.info.guest_size, [&](u64 bytes) { freed += texture_cache.ReclaimMemory(bytes); },
            [&] { return image.TryCreate(ci); });
        LOG_WARNING(Render_Vulkan, "VR scene allocation recovery: {} MiB released, recovered={}",
                    freed >> 20, recovered);
        if (!recovered) {
            Common::CrashRecord::Note(
                "out of memory: VR scene allocation failed after cache cleanup");
        }
        ASSERT_MSG(recovered, "Failed allocating VR scene image after texture-cache recovery");
    }
}
VrSceneProxy::~VrSceneProxy() {
    scheduler.DeferOperation(
        [old = std::move(image), v = std::move(views), r = std::move(reads)]() mutable {
            v.clear();
            for (auto& x : r)
                x.reset();
            old.Destroy();
        });
}
bool VrSceneProxy::Matches(const VideoCore::Image& source, uint32_t w, uint32_t h) const {
    return native == source.GetImage() && image.image_ci.extent.width == w &&
           image.image_ci.extent.height == h;
}
void VrSceneProxy::Transition(vk::ImageLayout next, vk::AccessFlags2 access) {
    if (layout == next &&
        (next == vk::ImageLayout::eColorAttachmentOptimal ||
         next == vk::ImageLayout::eDepthStencilAttachmentOptimal ||
         (next == vk::ImageLayout::eGeneral && access == vk::AccessFlagBits2::eShaderRead)))
        return;
    scheduler.EndRendering();
    const vk::ImageMemoryBarrier2 b{.srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                    .srcAccessMask = vk::AccessFlagBits2::eMemoryRead |
                                                     vk::AccessFlagBits2::eMemoryWrite,
                                    .dstStageMask = vk::PipelineStageFlagBits2::eAllCommands,
                                    .dstAccessMask = access,
                                    .oldLayout = layout,
                                    .newLayout = next,
                                    .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                                    .image = image,
                                    .subresourceRange = {aspects, 0, 1, 0, 2}};
    const vk::DependencyInfo dependency{.imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    // Bit 37: recorded like the guest's own barriers (a raw borrow here, at the first reduced
    // G-buffer draw, kept that whole command buffer off the recording thread).
    if (UpscalerFastOn(UpscalerFastPart::Recording)) {
        scheduler.Commands().pipelineBarrier2(dependency);
        UpscalerFastNote(g_upscaler_fast.raw_avoided);
    } else {
        scheduler.CommandBuffer().pipelineBarrier2(dependency);
    }
    layout = next;
}
void VrSceneProxy::Fill(VideoCore::Image& source, bool to_native) {
    copying = true;
    if (to_native)
        ++resolves;
    else
        ++uploads;
    scheduler.EndRendering();
    // FAST_GPU_PASS_PROFILE (bit 38): bbport vk_scene_resolution.cpp:154 (TransferMark).
    const GpuPassProfiler::Scope pass_scope{
        scheduler, to_native ? 0x7E3B'0000'0201ull : 0x7E3B'0000'0200ull,
        GpuPassProfiler::Group::Temporal,
        to_native ? "VR scene proxy resolve to the native image"
                  : "VR scene proxy fill from the native image"};
    source.Transit(
        to_native ? vk::ImageLayout::eTransferDstOptimal : vk::ImageLayout::eTransferSrcOptimal,
        to_native ? vk::AccessFlagBits2::eTransferWrite : vk::AccessFlagBits2::eTransferRead, {});
    Transition(
        to_native ? vk::ImageLayout::eTransferSrcOptimal : vk::ImageLayout::eTransferDstOptimal,
        to_native ? vk::AccessFlagBits2::eTransferRead : vk::AccessFlagBits2::eTransferWrite);
    const vk::Offset3D n{int(source.info.size.width), int(source.info.size.height), 1};
    const vk::Offset3D p{int(image.image_ci.extent.width), int(image.image_ci.extent.height), 1};
    // Bit 37: proxy uploads/resolves are recorded (value-only blits, same order).
    const bool recorded = UpscalerFastOn(UpscalerFastPart::Recording);
    for (auto aspect : {vk::ImageAspectFlagBits::eColor, vk::ImageAspectFlagBits::eDepth,
                        vk::ImageAspectFlagBits::eStencil}) {
        if (!(aspects & aspect))
            continue;
        const vk::ImageBlit b{.srcSubresource = {aspect, 0, 0, 2},
                              .srcOffsets = std::array{vk::Offset3D{}, to_native ? p : n},
                              .dstSubresource = {aspect, 0, 0, 2},
                              .dstOffsets = std::array{vk::Offset3D{}, to_native ? n : p}};
        const vk::Image src = to_native ? vk::Image(image) : source.GetImage();
        const vk::Image dst = to_native ? source.GetImage() : vk::Image(image);
        if (recorded) {
            scheduler.Commands().blitImage(src, vk::ImageLayout::eTransferSrcOptimal, dst,
                                           vk::ImageLayout::eTransferDstOptimal, b,
                                           vk::Filter::eNearest);
            UpscalerFastNote(g_upscaler_fast.raw_avoided);
        } else {
            scheduler.CommandBuffer().blitImage(src, vk::ImageLayout::eTransferSrcOptimal, dst,
                                                vk::ImageLayout::eTransferDstOptimal, b,
                                                vk::Filter::eNearest);
        }
    }
    if (to_native)
        dirty = false;
    else {
        valid = true;
        dirty = false;
    }
    copying = false;
}
void VrSceneProxy::NativeAccess(VideoCore::Image& source, vk::AccessFlags2 access) {
    if (copying || attachment_scope)
        return;
    if (native != source.GetImage()) {
        valid = dirty = false;
        return;
    }
    if (dirty)
        Fill(source, true);
    const auto writes = vk::AccessFlagBits2::eShaderWrite | vk::AccessFlagBits2::eTransferWrite |
                        vk::AccessFlagBits2::eColorAttachmentWrite |
                        vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                        vk::AccessFlagBits2::eMemoryWrite;
    if (access & writes)
        NativeWritten();
}
vk::ImageView VrSceneProxy::Attachment(VideoCore::Image& source,
                                       const VideoCore::ImageViewInfo& desc, bool discard,
                                       bool written) {
    if (!valid && !discard)
        Fill(source, false);
    const bool depth = bool(aspects & vk::ImageAspectFlagBits::eDepth);
    Transition(depth ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                     : vk::ImageLayout::eColorAttachmentOptimal,
               depth ? (vk::AccessFlagBits2::eDepthStencilAttachmentRead |
                        vk::AccessFlagBits2::eDepthStencilAttachmentWrite)
                     : (vk::AccessFlagBits2::eColorAttachmentRead |
                        vk::AccessFlagBits2::eColorAttachmentWrite));
    valid = true;
    dirty |= written || discard;
    for (auto& [key, view] : views)
        if (key == desc)
            return *view;
    const vk::ImageViewUsageCreateInfo usage{
        .usage = depth ? vk::ImageUsageFlagBits::eDepthStencilAttachment
                       : vk::ImageUsageFlagBits::eColorAttachment};
    auto view =
        instance.GetDevice().createImageViewUnique({.pNext = &usage,
                                                    .image = image,
                                                    .viewType = vk::ImageViewType::e2DArray,
                                                    .format = desc.format,
                                                    .components = desc.mapping,
                                                    .subresourceRange = {aspects, 0, 1, 0, 2}});
    ASSERT(view.result == vk::Result::eSuccess);
    views.emplace_back(desc, std::move(view.value));
    return *views.back().second;
}
vk::ImageView VrSceneProxy::Read(VideoCore::Image& source, uint32_t eye) {
    if (!valid)
        Fill(source, false);
    Transition(vk::ImageLayout::eGeneral, vk::AccessFlagBits2::eShaderRead);
    if (!reads[eye]) {
        auto view = instance.GetDevice().createImageViewUnique(
            {.image = image,
             .viewType = vk::ImageViewType::e2D,
             .format = image.image_ci.format,
             .subresourceRange = {source.info.props.is_depth ? vk::ImageAspectFlagBits::eDepth
                                                             : vk::ImageAspectFlagBits::eColor,
                                  0, 1, eye, 1}});
        ASSERT(view.result == vk::Result::eSuccess);
        reads[eye] = std::move(view.value);
    }
    return *reads[eye];
}
} // namespace Vulkan

namespace Vulkan {
std::optional<RenderState> Rasterizer::VrTemporalRenderState(const GraphicsPipeline* pipeline) {
    if (!vr_temporal || !vr_temporal->active || vr_temporal->resolved ||
        pipeline->GetGraphicsKey().probe_view_mask != 3)
        return {};
    auto& t = *vr_temporal;
    boost::container::small_vector<VideoCore::ImageId, 9> attachments;
    const auto check = [&](VideoCore::ImageId id, const VideoCore::ImageViewInfo& view) {
        if (!id)
            return true;
        auto& image = texture_cache.GetImage(id);
        if (image.info.size.width != t.width || image.info.size.height != t.height ||
            (image.binding.is_bound && !image.info.props.is_depth) || image.binding.needs_rebind ||
            view.range.base.level || view.range.base.layer)
            return false;
        if (!image.scene_proxy ||
            !image.scene_proxy->Matches(image, t.input_width, t.input_height)) {
            if (!VrSceneProxy::Eligible(image))
                return false;
            texture_cache.UpdateImage(id);
            if (image.scene_proxy)
                image.scene_proxy->NativeAccess(image, vk::AccessFlagBits2::eMemoryRead);
            image.scene_proxy =
                std::make_shared<VrSceneProxy>(image, t.input_width, t.input_height, texture_cache);
            if (std::ranges::find(t.proxies, std::pair{id, image.image_uid}) == t.proxies.end())
                t.proxies.emplace_back(id, image.image_uid);
        }
        attachments.push_back(id);
        return true;
    };
    const auto count = std::bit_width(pipeline->GetGraphicsKey().mrt_mask);
    for (uint32_t i = 0; i < count; ++i)
        if (!check(cb_descs[i].first, cb_descs[i].second.view_info))
            return {};
    if (!check(db_desc.first, db_desc.second.view_info) || attachments.empty())
        return {};
    for (const auto id : attachments)
        texture_cache.GetImage(id).scene_proxy->attachment_scope = true;
    auto state = BeginRenderingFull(pipeline);
    // Finding a missing stencil can grow slot storage and move the images. Keep IDs across
    // BeginRenderingFull and fetch each image again before accessing its proxy.
    for (const auto id : attachments)
        texture_cache.GetImage(id).scene_proxy->attachment_scope = false;
    for (uint32_t i = 0; i < count; ++i)
        if (cb_descs[i].first) {
            auto& image = texture_cache.GetImage(cb_descs[i].first);
            auto& a = state.color_attachments[i];
            a.image_view =
                image.scene_proxy->Attachment(image, cb_descs[i].second.view_info, a.is_clear);
            a.image_layout = image.scene_proxy->Layout();
        }
    if (db_desc.first) {
        auto& image = texture_cache.GetImage(db_desc.first);
        auto& a = state.depth_stencil_attachment;
        const bool discard = (!a.has_depth || a.depth_clear) && (!a.has_stencil || a.stencil_clear);
        a.image_view = image.scene_proxy->Attachment(
            image, db_desc.second.view_info, discard,
            db_desc.second.view_info.is_storage ||
                ScratchDraw::Registers(liverpool).depth_control.stencil_enable || a.depth_clear ||
                a.stencil_clear);
        a.image_layout = image.scene_proxy->Layout();
    }
    state.width = t.input_width;
    state.height = t.input_height;
    vr_scene_inverse = {float(t.width) / t.input_width, float(t.height) / t.input_height};
    ++t.reduced_draws;
    return state;
}
} // namespace Vulkan
