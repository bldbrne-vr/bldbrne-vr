#include "../../../scratchpad/dual-eye-perf/draw_registers.h"
#include "../../../scratchpad/dual-eye-perf/profile_scopes.h"
#include "../../../scratchpad/dual-eye-perf/push_state_memo.h"
#include "../../../scratchpad/fast-renderer/b_handler_profile.h"
#include "../../../scratchpad/fast-renderer/packet_codec.h"
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "../../../scratchpad/dual-eye-perf/native_stereo.h"
#include "../../../scratchpad/dual-eye-perf/lighting_reconstruction.h"
#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <nlohmann/json.hpp>
#include <mutex>
#include <numeric>
#include <optional>
#include <set>
#include <string>
#include <unordered_set>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vulkan/vulkan_format_traits.hpp>

#include "common/alignment.h"
#include "common/debug.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/vr/aa_mode.h"
#include "core/vr/live_tune.h"
#include "core/vr/vr_session.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/fast_render.h"
#include "../../../scratchpad/fast-renderer/drain_audit.h"
#include "../../../scratchpad/dual-eye-perf/diag_skip.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_draw_pipe.h"
#include "video_core/renderer_vulkan/vk_gpu_pass_profiler.h"
#include "video_core/renderer_vulkan/vk_constant_ring.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_prepared_bindings.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_vulkan/vr_half_tan_memo.h"
#include "video_core/renderer_vulkan/vr_inframe.h"
#include "video_core/renderer_vulkan/vr_perf.h"
#include "video_core/renderer_vulkan/vr_render_taa.h"
#include "video_core/renderer_vulkan/vr_smaa.h"
#include "video_core/renderer_vulkan/vr_temporal_capture.h"
#include "video_core/texture_cache/download_log.h"
#include "video_core/texture_cache/host_compatibility.h"
#include "video_core/texture_cache/image_view.h"
#include "video_core/texture_cache/texture_cache.h"
#include "video_core/texture_cache/tile_manager.h"

#ifdef MemoryBarrier
#undef MemoryBarrier
#endif

namespace Vulkan {
namespace DA = FastRender::DrainAudit;

namespace {
// FAST_CPU_GUARD (on unless '0'; read once at start-up): the constant ring's write guard keeps
// bbport's rule (FastRender::PendingWriteGuard): only writes still queued refuse a constant, and
// image downloads are caught on B (VideoCore::DownloadLog). Off: the merged ranges as before.
// VR_TRACKED_SMALL_BUFFERS keeps the old guard: B then binds small buffers from the cache, where
// a texel buffer may hold an image's data instead of the guest bytes.
const bool g_guard = [] {
    const char* value = std::getenv("FAST_CPU_GUARD");
    return (!value || value[0] != '0') && !std::getenv("VR_TRACKED_SMALL_BUFFERS");
}();
// FAST_CPU_GUARD_VERIFY=1: the old guard decides, as with the switch off; a constant only the new
// rule accepts is copied too, and B compares every constant the new rule accepted with the guest
// bytes it would have read itself, and asks whether it would have read them at all. CE dumps are
// checked too (FAST_CPU_GUARD_CE_VERIFY).
const bool g_guard_verify = [] {
    const char* value = std::getenv("FAST_CPU_GUARD_VERIFY");
    return g_guard && value && value[0] == '1';
}();
// The old guard's image footprints (DrawPacket::constant_images) are read by it alone.
const bool g_constant_images = !g_guard || g_guard_verify;
// FAST_CPU_GUARD_CE (on unless '0'; with FAST_CPU_GUARD only): a CE dump (CeDumpWait) asks the
// new rule too, so a dump that only the old ranges covered takes bit 16's targeted wait instead
// of a full drain and guest signal flush. Off: every covered dump drains in full, as the old
// ranges made all of them do in the 2026-10-14 captures (full_guard = dumps). Never with
// readbackLinearImages: OnSubmit's ordered task then downloads images into guest memory, a write
// neither the targeted wait nor the new rule sees (the old guard's image footprints refused it).
const bool g_guard_ce = [] {
    const char* value = std::getenv("FAST_CPU_GUARD_CE");
    return g_guard && (!value || value[0] != '0');
}();
// FAST_CPU_GUARD_CE_VERIFY=1 (or FAST_CPU_GUARD_VERIFY=1): a dump the new rule would let take the
// targeted wait drains in full all the same, after a check of what that wait would have left
// queued (Rasterizer::CeVerifyTargeted).
const bool g_guard_ce_verify = [] {
    const char* value = std::getenv("FAST_CPU_GUARD_CE_VERIFY");
    return g_guard_ce && (g_guard_verify || (value && value[0] == '1'));
}();
// FAST_CPU_A_HEADROOM (on unless '0'): Stage A takes a render target's footprint for the guard
// from a memo on its register words (DrawPacket::TargetFootprint) instead of a new ImageInfo
// (and, for depth, an image view description) every draw.
const bool g_a_headroom = [] {
    const char* value = std::getenv("FAST_CPU_A_HEADROOM");
    return !value || value[0] != '0';
}();
// FAST_CPU_A_HEADROOM_VERIFY=1: every memo hit is built again and compared; a mismatch is logged
// and the fresh footprint used.
const bool g_a_headroom_verify = [] {
    const char* value = std::getenv("FAST_CPU_A_HEADROOM_VERIFY");
    return g_a_headroom && value && value[0] == '1';
}();
// FAST_CPU_SLIM_PACKET (on unless '0'; read once at start-up): stage A sends B a slimmer draw
// packet (DrawPacket::EncodeSlim / DecodeSlim; bbport's is about a third of ours). A draw carries
// no compute registers (B reads them only for a dispatch: ComputeRegisters behind IsCompute or a
// compute stage); the camera record only when its bytes differ from the last one sent (B keeps
// that one; an id would let B see the render-camera hook's later writes to the record); the
// constant list without the refused entries (B binds and checks only bound or probed ones); the
// fixed fields in one header, read in one copy without per-field bounds checks (FAST_PIPE_VERIFY
// keeps them); and B borrows the vertex arrays (Vulkan::VertexView) instead of copying them. B
// binds the same values either way. Off: Encode/Decode as before.
const bool g_slim_packet = [] {
    const char* value = std::getenv("FAST_CPU_SLIM_PACKET");
    return !value || value[0] != '0';
}();
// FAST_CPU_SLIM_PACKET_VERIFY=1: every packet carries both forms. B decodes the slim one into a
// packet of its own, compares it with the full one, consumes the full one as with the switch off,
// and counts a draw's compute register reads (the slim form would have left them the last
// dispatch's).
const bool g_slim_packet_verify = [] {
    const char* value = std::getenv("FAST_CPU_SLIM_PACKET_VERIFY");
    return g_slim_packet && value && value[0] == '1';
}();
} // namespace

class ScratchCapturedInputs {
    std::array<ScratchDraw::OwnedInputs, Shader::MaxStageTypes> owned;
    std::array<ScratchDraw::Inputs, Shader::MaxStageTypes> views;
    std::optional<ScratchDraw::Scope> scope;

public:
    explicit ScratchCapturedInputs(const Pipeline* pipeline) {
        static const bool enabled = [] {
            const char* value = std::getenv("VR_DRAW_INPUT_SNAPSHOTS");
            const bool on = value && value[0] == '1';
            if (on)
                LOG_INFO(Render_Vulkan, "Scratch owned draw inputs enabled (serial validation)");
            return on;
        }();
        if (!enabled)
            return;
        size_t count = 0;
        if (pipeline)
            for (const auto* stage : pipeline->GetStages()) {
                if (!stage)
                    continue;
                owned[count].Capture(stage, stage->UserData(), stage->FlatUserData(),
                                     stage->ProgramBase());
                views[count] = owned[count].View();
                ++count;
            }
        scope.emplace(std::span<const ScratchDraw::Inputs>{views.data(), count});
    }
};

// S1 deliberately consumes immediately: cache ownership and guest-memory dependencies
// still need the ordered tasks of S2-S10 before this may run asynchronously.
// bbport PreparedStage, adapted to owned draw packets and the existing texture memos.
// No cache lookup, guest-memory access or Vulkan work belongs in this preparation.
struct Rasterizer::PreparedResources {
    // Bit 58: the same layout as the workers' records (vk_prepared_bindings.h).
    using Image = PreparedBindingImage;
    std::vector<Image> images;
    std::vector<AmdGpu::Sampler> samplers;
    std::vector<AmdGpu::Buffer> buffers;
    std::span<const Image> image_values;
    std::span<const AmdGpu::Sampler> sampler_values;
    std::span<const AmdGpu::Buffer> buffer_values;
    PerfMemo::BindKey key;
    u64 key_hash{};
    bool keyed{};

    static u64 ImageHash(const std::array<u64, 4>& raw, u32 flags) {
        return PreparedImageHash(raw, flags);
    }
    void Build(const Shader::Info& stage) {
        images.clear();
        samplers.clear();
        buffers.clear();
        key.Clear();
        keyed = !stage.images.empty();
        for (const auto& desc : stage.images) {
            const auto sharp = desc.GetSharp(stage);
            std::array<u64, 4> raw;
            static_assert(sizeof(raw) == sizeof(sharp));
            std::memcpy(raw.data(), &sharp, sizeof(raw));
            const u32 flags = u32(desc.is_written) | u32(desc.is_depth) << 1 |
                              u32(desc.is_array) << 2;
            images.push_back({sharp, ImageHash(raw, flags)});
            keyed &= !desc.is_written;
            key.Add(sharp, PerfMemo::BindAttrs(desc.is_depth, desc.is_array, desc.is_written,
                                              static_cast<u32>(desc.mip_fallback_mode),
                                              desc.constant_mip_index));
        }
        keyed &= key.Usable();
        key_hash = keyed ? key.Hash() : 0;
        for (const auto& desc : stage.samplers) {
            auto sharp = desc.GetSharp(stage);
            if (desc.disable_aniso) {
                ASSERT(desc.associated_image < images.size());
                const auto& image = images[desc.associated_image].sharp;
                if (image.base_level == 0 && image.last_level == 0)
                    sharp.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            }
            samplers.push_back(sharp);
        }
        for (const auto& desc : stage.buffers)
            buffers.push_back(desc.GetSharp(stage));
        image_values = images;
        sampler_values = samplers;
        buffer_values = buffers;
    }
    bool Matches(const PreparedResources& other) const {
        const auto equal = [](const auto& a, const auto& b) {
            return a.size() == b.size() &&
                   (a.empty() || std::memcmp(a.data(), b.data(), a.size() * sizeof(a[0])) == 0);
        };
        return equal(image_values, other.image_values) && equal(sampler_values, other.sampler_values) &&
               equal(buffer_values, other.buffer_values) && keyed == other.keyed &&
               (!keyed || (key == other.key && key_hash == other.key_hash));
    }
};

struct Rasterizer::DrawPacket {
    ScratchDraw::RegisterInputs registers;
    AmdGpu::Liverpool::RegisterDeltas::Packet deltas;
    std::span<const AmdGpu::Liverpool::RegisterDeltas::Block> delta_values;
    bool regs_reset{}; // ClearStateReset: SetDefaults before delta_values (bbport regs_reset).
    // Consumer side: blocks of `registers` written since its last reset, so a reset restores
    // only those from the defaults. Untracked (live copy, or bit off) means a full SetDefaults.
    std::vector<u16> replay_touched;
    std::bitset<AmdGpu::Liverpool::RegisterDeltas::NumBlocks> replay_touched_bits;
    bool replay_untracked{true};
    u64 reset_hits{}, reset_misses{}, reset_blocks{}, reset_checks{}, reset_mismatches{};
    bool packet_views{};
    std::array<ScratchDraw::OwnedInputs, Shader::MaxStageTypes> owned;
    std::array<ScratchDraw::Inputs, Shader::MaxStageTypes> inputs;
    std::array<std::vector<ScratchDraw::RegisterRead>, Shader::MaxStageTypes> reads;
    std::unique_ptr<AmdGpu::Regs> reference;
    std::array<AmdGpu::CbDbExtent, AmdGpu::NUM_COLOR_BUFFERS> cb_extent;
    AmdGpu::CbDbExtent db_extent;
    AmdGpu::ComputeProgram compute;
    // FAST_CPU_SLIM_PACKET (B): the packet consumed carried `compute` (a draw's slim form does
    // not: B keeps the last dispatch's, which Replay leaves in `registers`).
    bool compute_sent{true};
    // FAST_CPU_SLIM_PACKET, stage A: this packet's choices (SlimChoose, before both encode passes)
    // and the camera record B holds (the last one sent; forgotten for a new B decoder).
    bool send_camera{true}, send_compute{true};
    FastRender::HeldValue<VR::CameraRecord> held_camera;
    u64 slim_packets{}, slim_bytes{}, slim_full_bytes{}, slim_cameras{}, slim_computes{},
        slim_constants{}, slim_kept{};
    // FAST_CPU_SLIM_PACKET_VERIFY, stage B: the slim form's own decoder (it keeps the camera as B
    // does) and the comparison's counts.
    std::unique_ptr<DrawPacket> slim_check;
    u64 slim_verified{}, slim_mismatches{}, slim_compute_reads{}, slim_verify_bytes{},
        slim_verify_full_bytes{};
    // The vertex arrays BindVertexBuffers reads: `vertex`'s own, or (FAST_CPU_SLIM_PACKET with
    // packet views) the packet's.
    VertexView vertex_view;
    bool verify{};
    u64 sequence{}, verified{}, bytes{};
    struct OwnedMemory {
        VAddr address{};
        std::vector<u32> words;
    };
    std::vector<OwnedMemory> memory;
    std::vector<ScratchDraw::MemoryRead> memory_views;
    struct ConstantBinding {
        const Shader::Info* stage{};
        u32 binding{}, input{};
        AmdGpu::Buffer sharp{};
        u64 bytes{};
        bool flat{}, camera{}, lighting{}, projected{};
        // FAST_GPU_MIRROR_FLIP: `camera` is a planar reflection's mirrored camera (paired upside
        // down; B notes it, Rasterizer::VrMirrorCamera).
        bool mirrored{};
        // FAST_CPU_GUARD_VERIFY: copied only for B to compare (at `left`); never bound (`raw` is
        // empty, as for a refused constant). `stricter`: bound on the old guard's word, which the
        // new rule refused (B counts it apart from those both accepted).
        bool probe{}, stricter{};
        vk::DescriptorBufferInfo raw{}, left{}, right{};
    };
    std::vector<ConstantBinding> constants;
    std::span<const ConstantBinding> constant_values;
    std::array<PreparedResources, Shader::MaxStageTypes> resources;
    bool resources_prepared{};
    // Bit 58, Stage A only: `resources` view the workers' records (ReuseBindings).
    bool resources_from_workers{};
    enum ResourcePrepCount : size_t {
        RpOffered,
        RpHits,
        RpUnprepared, // no worker record (no prepared draw, bit 32/58 off on the worker, absent)
        RpRejected,   // a record PacketReuse refused (stages, flat data or program base)
        RpShape,      // descriptor counts differ from this pipeline's stages
        RpVerified,
        RpMismatches,
        RpCounts,
    };
    std::array<u64, RpCounts> resource_prep{};
    std::unique_ptr<PreparedResources> resource_check; // FAST_RESOURCE_PREP_VERIFY
    VertexPreparation vertex;
    bool vertex_prepared{};
    std::vector<DA::Range> constant_writers;
    std::vector<DA::Range> constant_images; // the old guard's only (empty with FAST_CPU_GUARD)
    u64 image_writers{}; // AddImageWrite
    bool constant_unknown{};
    u32 constant_page{ConstantRing::NoPage};
    // FAST_CPU_GUARD: the image downloads begun before A copied the constants
    // (VideoCore::DownloadLog::Snapshot), or NoDownloads when B need not ask; and whether B
    // compares them with the guest bytes (FAST_CPU_GUARD_VERIFY). Sent with a ring page only.
    static constexpr u64 NoDownloads = ~u64{0};
    u64 constant_downloads{NoDownloads};
    bool constant_verify{};
    // Bit 32, Stage A only: PacketReuse statistics and the FAST_PACKET_PREP_VERIFY snapshot.
    enum PacketPrepCount : size_t {
        PpOffered,
        PpHits,
        PpVertex,
        PpResources,
        PpAbsent,
        PpStages,
        PpFlat,
        PpFetch,
        PpVerified,
        PpMismatches,
        PpCounts,
    };
    std::array<u64, PpCounts> packet_prep{};
    struct ReuseCheck {
        size_t stages{};
        std::array<std::vector<ScratchDraw::RegisterRead>, Shader::MaxStageTypes> reads;
        std::vector<ConstantBinding> constants;
        std::vector<DA::Range> writers, images;
        std::vector<std::pair<VAddr, size_t>> memory;
        VertexPreparation vertex;
        bool unknown{}, vertex_prepared{};
    };
    std::unique_ptr<ReuseCheck> reuse_check;
    PreparedPacket reuse_packet{};  // the header of the prepared draw PacketReuse accepted
    const std::byte* reuse_base{};  // its arena bytes (offsets count from here)
    const ConstantBinding* Constant(const Shader::Info& stage, u32 binding) const {
        for (const auto& c : constant_values)
            if (c.stage == &stage && c.binding == binding && c.raw.buffer) return &c;
        return nullptr;
    }
    size_t memory_count{}, stage_count{};
    const Pipeline* selected{};
    PacketKind kind{};
    PreparedVrDraw vr{};
    VR::CameraRecord camera{};
    // A-only diagnostic metadata; never serialized or used for scheduling.
    std::vector<DA::Range> audit_reads;
    std::vector<DA::Writer> audit_writes;
    bool audit_unknown{};
    bool indexed{};
    VAddr address{}, count_address{};
    u32 offset{}, size{}, count{};
    void CaptureMemory(VAddr source, size_t bytes) {
        for (size_t i = 0; i < memory_count; ++i) {
            if (memory[i].address == source && memory[i].words.size() * sizeof(u32) >= bytes)
                return;
        }
        if (memory.size() == memory_count)
            memory.emplace_back();
        auto& read = memory[memory_count++];
        read.address = source;
        read.words.resize((bytes + sizeof(u32) - 1) / sizeof(u32));
        // Copy after releasing pipeline selection: a GPU readback may drain B.
    }
    explicit DrawPacket(AmdGpu::Liverpool& live) : registers(live) {
        // A new consumer starts with a complete file, including after a future reset.
        live.register_deltas.MarkAll();
    }
    void AddWrite(VAddr at, u64 length) {
        if (at && length) constant_writers.push_back({at, length});
    }
    // An image's footprint as a writer; FAST_CPU_GUARD skips these (bit i: constant_writers[i],
    // the first 64 only: a later one counts as a buffer write, which only refuses more).
    void AddImageWrite(VAddr at, u64 length) {
        if (at && length && constant_writers.size() < 64)
            image_writers |= u64{1} << constant_writers.size();
        AddWrite(at, length);
    }
    void AddImage(const VideoCore::ImageInfo& info) {
        AddImageWrite(info.guest_address, info.guest_size);
        AddImageWrite(info.stencil_addr, info.stencil_size);
        if (!g_constant_images) // FAST_CPU_GUARD: only the old guard reads them
            return;
        if (info.guest_address && info.guest_size)
            constant_images.push_back({info.guest_address, info.guest_size});
        if (info.stencil_addr && info.stencil_size)
            constant_images.push_back({info.stencil_addr, info.stencil_size});
    }
    // FAST_CPU_A_HEADROOM: what AddImage takes from a render target's ImageInfo, remembered per
    // colour slot and for depth on the register words it is made from (the ImageInfo
    // constructors read nothing else: the CB or DB registers, the slice count, the HTILE base and
    // the extent hint).
    struct Footprint {
        VAddr address{}, stencil{};
        u64 bytes{}, stencil_bytes{};
        bool operator==(const Footprint&) const = default;
    };
    static_assert(sizeof(AmdGpu::ColorBuffer) % sizeof(u32) == 0 &&
                  sizeof(AmdGpu::DepthBuffer) % sizeof(u32) == 0 &&
                  sizeof(AmdGpu::DepthView) == sizeof(u32) &&
                  sizeof(AmdGpu::CbDbExtent) == sizeof(u32));
    static constexpr size_t ColorKeyWords =
        (sizeof(AmdGpu::ColorBuffer) + sizeof(AmdGpu::CbDbExtent)) / sizeof(u32);
    static constexpr size_t DepthKeyWords =
        (sizeof(AmdGpu::DepthBuffer) + sizeof(AmdGpu::DepthView) + sizeof(VAddr) +
         sizeof(AmdGpu::CbDbExtent)) / sizeof(u32);
    template <size_t Words>
    struct TargetMemo {
        std::array<u32, Words> key{};
        Footprint footprint{};
        bool valid{};
    };
    std::array<TargetMemo<ColorKeyWords>, AmdGpu::NUM_COLOR_BUFFERS> color_targets{};
    TargetMemo<DepthKeyWords> depth_target{};
    u64 target_hits{}, target_misses{}, target_verified{}, target_mismatches{};
    static Footprint FootprintOf(const VideoCore::ImageInfo& info) {
        return {info.guest_address, info.stencil_addr, info.guest_size, info.stencil_size};
    }
    template <size_t Words, class Make>
    Footprint TargetFootprint(TargetMemo<Words>& memo, const std::array<u32, Words>& key,
                              Make&& make) {
        if (memo.valid && memo.key == key) {
            if constexpr (Common::Log::Enabled)
                ++target_hits;
            if (!g_a_headroom_verify)
                return memo.footprint;
            const Footprint fresh = make();
            ++target_verified;
            if (!(fresh == memo.footprint)) {
                if (++target_mismatches <= 32)
                    LOG_ERROR(Render_Vulkan,
                              "FAST_CPU_A_HEADROOM verify: target footprint {:#x}+{:#x} (stencil "
                              "{:#x}+{:#x}) memo {:#x}+{:#x}; the fresh one is used, mismatch #{}",
                              fresh.address, fresh.bytes, fresh.stencil, fresh.stencil_bytes,
                              memo.footprint.address, memo.footprint.bytes, target_mismatches);
                memo.footprint = fresh;
            }
            return fresh;
        }
        if constexpr (Common::Log::Enabled)
            ++target_misses;
        memo.key = key;
        memo.footprint = make();
        memo.valid = true;
        return memo.footprint;
    }
    void AddFootprint(const Footprint& f) { // AddImage, in its order
        AddImageWrite(f.address, f.bytes);
        AddImageWrite(f.stencil, f.stencil_bytes);
        if (!g_constant_images)
            return;
        if (f.address && f.bytes)
            constant_images.push_back({f.address, f.bytes});
        if (f.stencil && f.stencil_bytes)
            constant_images.push_back({f.stencil, f.stencil_bytes});
    }
    // The render targets' AddImage calls of Build, through the footprint memo.
    void AddTargets(const AmdGpu::Liverpool& live) {
        const auto& regs = live.regs;
        for (u32 i = 0; i < AmdGpu::NUM_COLOR_BUFFERS; ++i) {
            if (!regs.color_buffers[i])
                continue;
            std::array<u32, ColorKeyWords> key;
            std::memcpy(key.data(), &regs.color_buffers[i], sizeof(regs.color_buffers[i]));
            std::memcpy(key.data() + sizeof(regs.color_buffers[i]) / sizeof(u32),
                        &live.last_cb_extent[i], sizeof(live.last_cb_extent[i]));
            AddFootprint(TargetFootprint(color_targets[i], key, [&] {
                return FootprintOf(
                    VideoCore::ImageInfo{regs.color_buffers[i], live.last_cb_extent[i]});
            }));
        }
        if (regs.depth_buffer.DepthValid() || regs.depth_buffer.StencilValid()) {
            const VAddr htile = regs.depth_htile_data_base.GetAddress();
            std::array<u32, DepthKeyWords> key;
            auto* at = reinterpret_cast<std::byte*>(key.data());
            std::memcpy(at, &regs.depth_buffer, sizeof(regs.depth_buffer));
            at += sizeof(regs.depth_buffer);
            std::memcpy(at, &regs.depth_view, sizeof(regs.depth_view));
            at += sizeof(regs.depth_view);
            std::memcpy(at, &htile, sizeof(htile));
            at += sizeof(htile);
            std::memcpy(at, &live.last_db_extent, sizeof(live.last_db_extent));
            AddFootprint(TargetFootprint(depth_target, key, [&] {
                // TextureCache::ImageDesc's `info`, without the view description it also builds.
                return FootprintOf(VideoCore::ImageInfo{regs.depth_buffer,
                                                        regs.depth_view.NumSlices(), htile,
                                                        live.last_db_extent, false});
            }));
        }
        if constexpr (Common::Log::Enabled)
            if ((sequence & 65535) == 0)
                LOG_INFO(Render_Vulkan,
                         "FAST_CPU_A_HEADROOM target footprints: hits={} misses={} verified={} "
                         "mismatches={}",
                         target_hits, target_misses, target_verified, target_mismatches);
    }
    void Build(AmdGpu::Liverpool& live, const Pipeline* pipeline, bool graphics, bool prepare_constants,
               const PreparedDraw* prepared = nullptr) {
        static const u64 verify_every = [] {
            const char* value = std::getenv("FAST_PIPE_VERIFY");
            return value ? std::strtoull(value, nullptr, 0) : 0ULL;
        }();
        packet_views = FastRender::On(FastRender::PacketViews);
        ++sequence;
        verify = verify_every && sequence % verify_every == 0;
        if (verify) {
            if (!reference)
                reference = std::make_unique<AmdGpu::Regs>();
            *reference = live.regs;
        }
        regs_reset = live.register_deltas.Capture(live.regs.reg_array, deltas);
        delta_values = deltas;
        bytes += deltas.size() * sizeof(AmdGpu::Liverpool::RegisterDeltas::Block);
        cb_extent = live.last_cb_extent;
        db_extent = live.last_db_extent;
        compute = live.GetCsRegs();
        stage_count = memory_count = 0;
        constants.clear();
        constant_writers.clear();
        image_writers = 0;
        constant_images.clear();
        constant_unknown = false;
        constant_page = ConstantRing::NoPage;
        constant_downloads = NoDownloads;
        constant_verify = false;
        if (prepare_constants && graphics) {
            if (g_a_headroom) {
                AddTargets(live);
            } else {
                for (u32 i = 0; i < AmdGpu::NUM_COLOR_BUFFERS; ++i)
                    if (live.regs.color_buffers[i])
                        AddImage(VideoCore::ImageInfo{live.regs.color_buffers[i],
                                                      live.last_cb_extent[i]});
                if (live.regs.depth_buffer.DepthValid() || live.regs.depth_buffer.StencilValid()) {
                    const VideoCore::TextureCache::ImageDesc desc{
                        live.regs.depth_buffer, live.regs.depth_view, live.regs.depth_control,
                        live.regs.depth_htile_data_base.GetAddress(), live.last_db_extent};
                    AddImage(desc.info);
                }
            }
        }
        const bool geometry = graphics && pipeline && FastRender::On(FastRender::GeometryPrepare);
        // Bit 32: `prepared` is a validated bit 31 hit for exactly `pipeline` (Rasterizer::Draw).
        const u32 reuse = prepared && graphics && pipeline
                              ? PacketReuse(*prepared, *static_cast<const GraphicsPipeline*>(pipeline))
                              : 0;
        const size_t target_writers = constant_writers.size();
        const size_t target_images = constant_images.size();
        BuildStages(pipeline, graphics, prepare_constants, prepared, reuse);
        BuildVertex(live, pipeline, geometry, reuse);
        if (prepared)
            NoteReuse(live, pipeline, graphics, prepare_constants, geometry, reuse, target_writers,
                      target_images);
        constant_values = constants;
        resources_prepared = pipeline && FastRender::On(FastRender::ResourcePrepare);
        static constexpr auto WorkerBindings = static_cast<FastRender::Bit>(58);
        resources_from_workers = pipeline && graphics && FastRender::On(WorkerBindings) &&
                                 ReuseBindings(prepared, reuse);
        resources_prepared |= resources_from_workers;
        if (resources_prepared && !resources_from_workers) {
            ScratchDraw::Scope input_scope{{inputs.data(), stage_count}};
            for (size_t i = 0; i < stage_count; ++i)
                resources[i].Build(*static_cast<const Shader::Info*>(owned[i].owner));
        }
        memory_views.clear();
        for (size_t i = 0; i < memory_count; ++i) {
            memory_views.push_back({memory[i].address, memory[i].words});
        }
        vertex_view = VertexView{vertex}; // the serial consumer's (ConsumePacket on A)
    }
    // Per stage: the owned inputs, the fetch V#s and the resource loop (constant candidates,
    // written ranges, native captures). `reuse` takes parts of them from the worker instead.
    void BuildStages(const Pipeline* pipeline, bool graphics, bool prepare_constants,
                     const PreparedDraw* prepared, u32 reuse) {
        if (!pipeline)
            return;
        for (const auto* stage : pipeline->GetStages()) {
            if (!stage)
                continue;
            const auto i = stage_count++;
            owned[i].Capture(stage, stage->UserData(), stage->FlatUserData(),
                             stage->ProgramBase());
            reads[i].clear();
            if (graphics && stage->l_stage == Shader::LogicalStage::Vertex) {
                if (reuse & PreparedPacket::Vertex) {
                    // Bit 32: the V#s TryPreparedGraphics compared with ReadUdReg on this thread,
                    // in the order of this pipeline's fetch attributes (PacketReuse).
                    const auto& source = prepared->stages[i];
                    for (u32 r = 0; r < source.num_reads; ++r) {
                        const u32* words = source.reads + size_t(r) * 6;
                        ScratchDraw::RegisterRead read{words[0], words[1]};
                        std::memcpy(read.words.data(), words + 2, sizeof(read.words));
                        reads[i].push_back(read);
                    }
                } else {
                    const auto& fetch =
                        static_cast<const GraphicsPipeline*>(pipeline)->GetFetchShader();
                    if (fetch)
                        for (const auto& attribute : fetch->attributes) {
                            ScratchDraw::RegisterRead read{attribute.sgpr_base,
                                                           attribute.dword_offset};
                            const auto sharp = stage->ReadUdReg<AmdGpu::Buffer>(
                                attribute.sgpr_base, attribute.dword_offset);
                            static_assert(sizeof(sharp) == sizeof(read.words));
                            std::memcpy(read.words.data(), &sharp, sizeof(sharp));
                            reads[i].push_back(read);
                        }
                }
            }
            if (stage->tess_consts_dword_offset >= 0) {
                ScratchDraw::RegisterRead read{
                    static_cast<u32>(stage->tess_consts_ptr_base),
                    static_cast<u32>(stage->tess_consts_dword_offset)};
                const auto sharp =
                    stage->ReadUdReg<AmdGpu::Buffer>(read.pointer_index, read.dword_offset);
                std::memcpy(read.words.data(), &sharp, sizeof(sharp));
                reads[i].push_back(read);
                CaptureMemory(sharp.base_address,
                              sizeof(Shader::TessellationDataConstantBuffer));
            }
            if (!(reuse & PreparedPacket::Resources)) {
                if (prepare_constants) {
                    // uses_dma is ReadConst (shader_info_collection_pass.cpp), a
                    // guest read. Its writes are only special BDA/fault buffers;
                    // guest buffer/image stores are enumerated below.
                    for (const auto& desc : stage->images) {
                        if (!desc.is_written) continue;
                        const auto sharp = desc.GetSharp(*stage);
                        if (!sharp.Address() || sharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid) continue;
                        if (sharp.pitch < sharp.width ||
                            !magic_enum::enum_contains(sharp.GetDataFmt()) ||
                            !magic_enum::enum_contains(sharp.GetNumberFmt())) {
                            constant_unknown = true;
                            continue;
                        }
                        AddImage(VideoCore::ImageInfo{sharp, desc});
                    }
                }
                for (const auto& desc : stage->buffers) {
                    if (prepare_constants) {
                        const auto sharp = desc.GetSharp(*stage);
                        if (desc.is_written && !desc.IsSpecial())
                            AddWrite(sharp.base_address, sharp.GetSize());
                        const bool flat = desc.buffer_type == Shader::BufferType::Flatbuf;
                        const u64 bytes = flat ? owned[i].flat.size() * sizeof(u32) : sharp.GetSize();
                        if (bytes && bytes <= 16384 && !desc.is_written &&
                            (flat || (!desc.IsSpecial() && sharp.base_address &&
                             Core::Memory::Instance()->ClampRangeSize(sharp.base_address, bytes) == bytes))) {
                            ConstantBinding c;
                            c.stage = stage;
                            c.binding = static_cast<u32>(&desc - stage->buffers.data());
                            c.input = static_cast<u32>(i);
                            c.sharp = sharp;
                            c.bytes = bytes;
                            c.flat = flat;
                            constants.push_back(c);
                        }
                    }
                    if (desc.IsSpecial() || desc.is_written)
                        continue;
                    const auto sharp = desc.GetSharp(*stage);
                    if (!sharp.base_address)
                        continue;
                    // These are CPU-side decisions on B, unlike ordinary GPU buffer bindings.
                    const auto size = sharp.GetSize();
                    if (NativeStereo::Enabled() &&
                        (size == 864 || size == 208 || size == 272 || size == 336 || size == 416) &&
                        Core::Memory::Instance()->ClampRangeSize(sharp.base_address, size) ==
                            size) {
                        CaptureMemory(sharp.base_address, size);
                    } else if (stage->l_stage == Shader::LogicalStage::Vertex &&
                               sharp.GetSize() == 416 && sharp.base_address >= 0x10000) {
                        CaptureMemory(sharp.base_address, 12 * sizeof(float));
                    }
                }
            }
            inputs[i] = owned[i].View();
            inputs[i].register_reads = reads[i];
        }
        if (reuse & PreparedPacket::Resources)
            ReuseResources(*prepared, prepare_constants);
    }
    void BuildVertex(AmdGpu::Liverpool& live, const Pipeline* pipeline, bool geometry, u32 reuse) {
        vertex_prepared = geometry;
        if (!vertex_prepared)
            return;
        const auto& graphics_pipeline = *static_cast<const GraphicsPipeline*>(pipeline);
        if (reuse & PreparedPacket::Vertex) {
            // PacketReuse's header (reuse_packet).
            ReuseVertex(graphics_pipeline.HasVrMotion());
            return;
        }
        ScratchDraw::Scope input_scope{{inputs.data(), stage_count}};
        VideoCore::BufferCache::PrepareVertexInputs(
            graphics_pipeline, live.regs.vgt_instance_step_rate_0,
            live.regs.vgt_instance_step_rate_1, vertex);
    }

    // Bit 32 (bbport BBV vk_rasterizer.cpp:941-990 and 2574-2579, which checks only the
    // attribute count): the parts of `prepared`, a validated bit 31 hit for exactly `pipeline`,
    // that this packet may take. TryPreparedGraphics compared on this thread the register
    // checksum (instance step rates and the render state included), the program generation,
    // each stage's header hash, program base and flat data, the fetch code and every fetch V#.
    // Checked here: what Build reads besides those.
    u32 PacketReuse(const PreparedDraw& prepared, const GraphicsPipeline& pipeline) {
        if (!prepared.packet) {
            ++packet_prep[PpAbsent];
            return 0;
        }
        auto& p = reuse_packet;
        reuse_base = reinterpret_cast<const std::byte*>(prepared.packet);
        std::memcpy(&p, reuse_base, sizeof(p));
        // The prepared stages in Build's order, none with tessellation constants. The flat data
        // and program base (inline buffers) once more: a selection since the hit (the first
        // build of a VR cut variant in PrepareVrDraw) refreshes them from the guest tables, and
        // the packet owns the current words.
        constexpr u32 NoStage = ~0u;
        u32 k = 0, vs = NoStage;
        bool same_flat = true;
        for (const auto* stage : pipeline.GetStages()) {
            if (!stage)
                continue;
            if (k == prepared.num_stages || prepared.stages[k].info != stage ||
                stage->tess_consts_dword_offset >= 0) {
                ++packet_prep[PpStages];
                return 0;
            }
            const auto& source = prepared.stages[k];
            const auto flat = stage->FlatUserData();
            same_flat = same_flat && stage->ProgramBase() == source.pgm_base &&
                        flat.size() == source.flat_size &&
                        (flat.empty() ||
                         std::memcmp(flat.data(), source.flat, flat.size_bytes()) == 0);
            if (stage->l_stage == Shader::LogicalStage::Vertex)
                vs = k;
            ++k;
        }
        if (k != prepared.num_stages) {
            ++packet_prep[PpStages];
            return 0;
        }
        u32 parts = 0;
        if (p.parts & PreparedPacket::Resources) {
            // NativeStereo::Enabled() is fixed at start; the worker used its own (`native`: its
            // view mask before FAST_CPU_VIEW_PREDICT's guess, which may be 0 in native stereo).
            if (same_flat && NativeStereo::Enabled() == prepared.native)
                parts |= PreparedPacket::Resources;
            else
                ++packet_prep[PpFlat];
        }
        // Bit 58: sharps depend on nothing but the flat data, user data and program base
        // TryPreparedGraphics and the loop above compared (bbport: valid exactly when `flat`
        // matched). Stage A reads the record only with the bit on (DrawPacket::ReuseBindings).
        if ((p.parts & PreparedPacket::Bindings) && same_flat)
            parts |= PreparedPacket::Bindings;
        if (p.parts & PreparedPacket::Vertex) {
            // This pipeline's own fetch attributes, byte for byte: VertexAttribute::operator==
            // (the specialization's) skips the inst_offset and format override GetSharp applies.
            const auto& fetch = pipeline.GetFetchShader();
            const size_t count = fetch ? fetch->attributes.size() : 0;
            const u32 num_reads = vs == NoStage ? 0 : prepared.stages[vs].num_reads;
            if (count == p.num_attributes && count == num_reads &&
                (count == 0 || std::memcmp(fetch->attributes.data(), reuse_base + p.vertex,
                                           count * sizeof(Shader::Gcn::VertexAttribute)) == 0))
                parts |= PreparedPacket::Vertex;
            else
                ++packet_prep[PpFetch];
        }
        return parts;
    }
    // Bit 58: each stage's resource sharps, texture hashes and bind key from the worker record
    // PacketReuse accepted with the flat data, as views into the submission arena (alive until
    // Encode, which copies them into the packet). False: bit 13's A build, or B's own path.
    bool ReuseBindings(const PreparedDraw* prepared, u32 reuse) {
        static const u64 verify_every = [] {
            const char* value = std::getenv("FAST_RESOURCE_PREP_VERIFY");
            return value ? std::strtoull(value, nullptr, 0) : 0ULL;
        }();
        auto& n = resource_prep;
        ++n[RpOffered];
        bool ok = false;
        if (!(reuse & PreparedPacket::Bindings)) {
            const bool had = prepared && prepared->packet &&
                             (reuse_packet.parts & PreparedPacket::Bindings);
            ++n[had ? RpRejected : RpUnprepared];
        } else {
            ok = true;
            const std::byte* at = reuse_base + reuse_packet.bindings;
            for (size_t i = 0; i < stage_count; ++i) {
                const auto view = ReadPreparedBindings(at);
                at += view.bytes;
                const auto& stage = *static_cast<const Shader::Info*>(owned[i].owner);
                auto& r = resources[i];
                if (view.images.size() != stage.images.size() ||
                    view.samplers.size() != stage.samplers.size() ||
                    view.buffers.size() != stage.buffers.size() ||
                    view.key_words.size() > r.key.words.size() ||
                    view.key_words.size() % PerfMemo::BindKey::WordsPerImage != 0) {
                    ++n[RpShape];
                    ok = false;
                    break;
                }
                r.image_values = view.images;
                r.sampler_values = view.samplers;
                r.buffer_values = view.buffers;
                r.keyed = view.keyed;
                r.key_hash = view.key_hash;
                r.key.Clear();
                if (view.keyed) {
                    std::memcpy(r.key.words.data(), view.key_words.data(),
                                view.key_words.size_bytes());
                    r.key.count =
                        static_cast<u32>(view.key_words.size() / PerfMemo::BindKey::WordsPerImage);
                }
            }
        }
        if (ok) {
            ++n[RpHits];
            if (verify_every && n[RpHits] % verify_every == 0) {
                // The fresh build bit 13 makes on this thread, compared field by field; a
                // mismatch keeps the fresh result for this packet.
                ++n[RpVerified];
                if (!resource_check)
                    resource_check = std::make_unique<PreparedResources>();
                ScratchDraw::Scope input_scope{{inputs.data(), stage_count}};
                for (size_t i = 0; i < stage_count; ++i) {
                    const auto& stage = *static_cast<const Shader::Info*>(owned[i].owner);
                    resource_check->Build(stage);
                    if (!resources[i].Matches(*resource_check)) {
                        ++n[RpMismatches];
                        LOG_ERROR(Render_Vulkan,
                                  "FAST resource_prep mismatch #{} (packet {}, stage {}); the "
                                  "fresh results are used",
                                  n[RpMismatches], sequence, i);
                        resources[i].Build(stage);
                    }
                }
            }
        }
        if ((n[RpOffered] & 32767) == 0)
            LOG_INFO(Render_Vulkan,
                     "FAST resource_prep: offered={} hits={} misses={} unprepared={} rejected={} "
                     "shape={} verified={} mismatches={}",
                     n[RpOffered], n[RpHits], n[RpOffered] - n[RpHits], n[RpUnprepared],
                     n[RpRejected], n[RpShape], n[RpVerified], n[RpMismatches]);
        return ok;
    }
    void ReuseResources(const PreparedDraw& prepared, bool prepare_constants) {
        const auto& p = reuse_packet;
        const auto* at = reuse_base + p.resources;
        if (prepare_constants) {
            for (u32 n = 0; n < p.num_constants; ++n) {
                PreparedConstant source;
                std::memcpy(&source, at + size_t(n) * sizeof(source), sizeof(source));
                ConstantBinding c;
                c.stage = prepared.stages[source.input].info;
                c.binding = source.binding;
                c.input = source.input;
                c.sharp = source.sharp;
                c.bytes = source.bytes;
                c.flat = source.flat != 0;
                constants.push_back(c);
            }
        }
        at += size_t(p.num_constants) * sizeof(PreparedConstant);
        PreparedRange range;
        for (u32 n = 0; n < p.num_writers; ++n, at += sizeof(range)) {
            std::memcpy(&range, at, sizeof(range));
            if (prepare_constants)
                AddWrite(range.address, range.bytes);
        }
        for (u32 n = 0; n < p.num_captures; ++n, at += sizeof(range)) {
            std::memcpy(&range, at, sizeof(range));
            CaptureMemory(range.address, range.bytes);
        }
        if (prepare_constants && p.unknown != 0)
            constant_unknown = true;
    }
    void ReuseVertex(bool motion) {
        const auto& p = reuse_packet;
        vertex.attributes.clear();
        vertex.bindings.clear();
        vertex.guest_buffers.clear();
        vertex.ranges.clear();
        const auto* at = reuse_base + p.vertex + PreparedPacket::FetchBytes(p.num_attributes);
        for (u32 n = 0; n < p.num_attributes; ++n, at += sizeof(PreparedVertexInput)) {
            PreparedVertexInput input;
            std::memcpy(&input, at, sizeof(input));
            vertex.attributes.push_back(vk::VertexInputAttributeDescription2EXT{
                .location = input.semantic,
                .binding = input.semantic,
                .format = input.format,
                .offset = 0,
            });
            vertex.bindings.push_back(vk::VertexInputBindingDescription2EXT{
                .binding = input.semantic,
                .stride = input.buffer.GetStride(),
                .inputRate = input.instance ? vk::VertexInputRate::eInstance
                                            : vk::VertexInputRate::eVertex,
                .divisor = input.divisor,
            });
            vertex.guest_buffers.push_back(input.buffer);
        }
        for (u32 n = 0; n < p.num_ranges; ++n, at += sizeof(VertexPreparation::Range)) {
            VertexPreparation::Range range;
            std::memcpy(&range, at, sizeof(range));
            vertex.ranges.push_back(range);
        }
        vertex.geometry = motion ? p.geometry : 0;
    }
    // Bit 32 statistics and FAST_PACKET_PREP_VERIFY=N: every Nth reuse is built again the
    // ordinary way and compared; the packet keeps the ordinary results (a mismatch falls back).
    void NoteReuse(AmdGpu::Liverpool& live, const Pipeline* pipeline, bool graphics,
                   bool prepare_constants, bool geometry, u32 reuse, size_t target_writers,
                   size_t target_images) {
        static const u64 reuse_verify_every = [] {
            const char* value = std::getenv("FAST_PACKET_PREP_VERIFY");
            return value ? std::strtoull(value, nullptr, 0) : 0ULL;
        }();
        auto& n = packet_prep;
        ++n[PpOffered];
        if (reuse) {
            ++n[PpHits];
            n[PpVertex] += (reuse & PreparedPacket::Vertex) != 0;
            n[PpResources] += (reuse & PreparedPacket::Resources) != 0;
        }
        if (reuse && reuse_verify_every && n[PpHits] % reuse_verify_every == 0) {
            if (!reuse_check)
                reuse_check = std::make_unique<ReuseCheck>();
            auto& fast = *reuse_check;
            fast.stages = stage_count;
            for (size_t i = 0; i < stage_count; ++i)
                fast.reads[i] = reads[i];
            fast.constants = constants;
            fast.writers = constant_writers;
            fast.images = constant_images;
            fast.unknown = constant_unknown;
            fast.memory.clear();
            for (size_t i = 0; i < memory_count; ++i)
                fast.memory.emplace_back(memory[i].address, memory[i].words.size());
            fast.vertex_prepared = vertex_prepared;
            fast.vertex = vertex;
            stage_count = memory_count = 0;
            constants.clear();
            constant_writers.resize(target_writers);
            if (target_writers < 64)
                image_writers &= (u64{1} << target_writers) - 1;
            constant_images.resize(target_images);
            constant_unknown = false;
            BuildStages(pipeline, graphics, prepare_constants, nullptr, 0);
            BuildVertex(live, pipeline, geometry, 0);
            ++n[PpVerified];
            if (const char* part = ReuseDiffers(fast)) {
                ++n[PpMismatches];
                LOG_ERROR(Render_Vulkan,
                          "FAST packet prep mismatch #{} in {} (packet {}, parts {:#x}); the "
                          "ordinary results are used",
                          n[PpMismatches], part, sequence, reuse);
            }
        }
        if ((n[PpOffered] & 32767) == 0)
            LOG_INFO(Render_Vulkan,
                     "FAST packet prep: hits={} misses={} packets={} vertex={} resources={} "
                     "absent={} stages={} flat={} fetch={} verified={} mismatches={}",
                     n[PpHits], n[PpOffered] - n[PpHits], sequence, n[PpVertex], n[PpResources],
                     n[PpAbsent], n[PpStages], n[PpFlat], n[PpFetch], n[PpVerified],
                     n[PpMismatches]);
    }
    const char* ReuseDiffers(const ReuseCheck& fast) const {
        const auto same_read = [](const ScratchDraw::RegisterRead& a,
                                  const ScratchDraw::RegisterRead& b) {
            return a.pointer_index == b.pointer_index && a.dword_offset == b.dword_offset &&
                   a.words == b.words;
        };
        const auto same_constant = [](const ConstantBinding& a, const ConstantBinding& b) {
            return a.stage == b.stage && a.binding == b.binding && a.input == b.input &&
                   std::memcmp(&a.sharp, &b.sharp, sizeof(a.sharp)) == 0 && a.bytes == b.bytes &&
                   a.flat == b.flat;
        };
        const auto same_range = [](const DA::Range& a, const DA::Range& b) {
            return a.address == b.address && a.bytes == b.bytes;
        };
        if (fast.stages != stage_count)
            return "stages";
        for (size_t i = 0; i < stage_count; ++i)
            if (!std::ranges::equal(fast.reads[i], reads[i], same_read))
                return "reads";
        if (!std::ranges::equal(fast.constants, constants, same_constant))
            return "constants";
        if (!std::ranges::equal(fast.writers, constant_writers, same_range))
            return "writers";
        if (!std::ranges::equal(fast.images, constant_images, same_range))
            return "images";
        if (fast.unknown != constant_unknown)
            return "unknown";
        if (fast.memory.size() != memory_count)
            return "captures";
        for (size_t i = 0; i < memory_count; ++i)
            if (fast.memory[i].first != memory[i].address ||
                fast.memory[i].second != memory[i].words.size())
                return "captures";
        if (fast.vertex_prepared != vertex_prepared)
            return "vertex";
        if (vertex_prepared) {
            const auto& a = fast.vertex;
            const auto& b = vertex;
            if (!std::ranges::equal(a.attributes, b.attributes) ||
                !std::ranges::equal(a.bindings, b.bindings) ||
                a.guest_buffers.size() != b.guest_buffers.size() ||
                (!a.guest_buffers.empty() &&
                 std::memcmp(a.guest_buffers.data(), b.guest_buffers.data(),
                             a.guest_buffers.size() * sizeof(AmdGpu::Buffer)) != 0) ||
                !std::ranges::equal(a.ranges, b.ranges) || a.geometry != b.geometry)
                return "vertex";
        }
        return nullptr;
    }
    void AuditResources(AmdGpu::Liverpool& live, const Pipeline* pipeline) {
        audit_reads.clear();
        audit_writes.clear();
        audit_unknown = !pipeline;
        const auto add = [&](VAddr at, u64 bytes) {
            if (at && bytes)
                audit_reads.push_back({at, bytes});
        };
        const auto image = [&](const VideoCore::ImageInfo& info) {
            add(info.guest_address, info.guest_size);
            add(info.stencil_addr, info.stencil_size);
            // Metadata sizes are not fully modeled; retain an explicit unknown flag.
            audit_unknown |=
                info.meta_info.cmask_addr || info.meta_info.fmask_addr || info.meta_info.htile_addr;
        };
        if (pipeline)
            for (const auto* stage : pipeline->GetStages()) {
                if (!stage)
                    continue;
                const auto writer = stage->l_stage == Shader::LogicalStage::Compute
                                        ? DA::Kind::Compute
                                        : DA::Kind::Graphics;
                audit_unknown |= stage->uses_dma;
                for (const auto& desc : stage->buffers) {
                    if (desc.IsSpecial())
                        continue;
                    const auto sharp = desc.GetSharp(*stage);
                    add(sharp.base_address, sharp.GetSize());
                    if (desc.is_written)
                        audit_writes.push_back({{sharp.base_address, sharp.GetSize()}, writer});
                }
                for (const auto& desc : stage->images) {
                    const auto sharp = desc.GetSharp(*stage);
                    if (!sharp.Address() || sharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid)
                        continue;
                    if (sharp.pitch < sharp.width ||
                        !magic_enum::enum_contains(sharp.GetDataFmt()) ||
                        !magic_enum::enum_contains(sharp.GetNumberFmt())) {
                        audit_unknown = true;
                        continue;
                    }
                    const VideoCore::ImageInfo info{sharp, desc};
                    image(info);
                    if (desc.is_written)
                        audit_writes.push_back({{info.guest_address, info.guest_size}, writer});
                }
            }
        for (size_t i = 0; i < stage_count; ++i)
            for (const auto& read : reads[i]) {
                AmdGpu::Buffer sharp;
                std::memcpy(&sharp, read.words.data(), sizeof(sharp));
                add(sharp.base_address, sharp.GetSize());
            }
        for (size_t i = 0; i < memory_count; ++i)
            add(memory[i].address, memory[i].words.size() * 4);
        if (kind == PacketKind::Draw || kind == PacketKind::DrawIndirect ||
            kind == PacketKind::FilteredDraw) {
            for (u32 i = 0; i < AmdGpu::NUM_COLOR_BUFFERS; ++i)
                if (live.regs.color_buffers[i])
                    image(VideoCore::ImageInfo{live.regs.color_buffers[i], live.last_cb_extent[i]});
            audit_unknown |= live.regs.depth_buffer.DepthValid();
            if (indexed) {
                const u32 width =
                    live.regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2 : 4;
                add(live.regs.index_base_address.Address<VAddr>() + u64(offset) * width,
                    u64(live.regs.num_indices) * width);
            }
        }
        if (kind == PacketKind::DrawIndirect || kind == PacketKind::DispatchIndirect) {
            add(address + offset, size);
            add(count_address, 4);
            audit_unknown |= indexed;
        }
    }
    // Bit 16: guest ranges B (or the recorder through B) may still read after this packet is
    // queued, kept only where they touch the CE box [lo, hi). Conservative: written buffers,
    // ring-served constants and render targets are listed too; owned captures are not.
    std::vector<DA::Range> ce_reads;
    u32 ce_untracked{}; // reason bits: 1 no pipeline, 2 uses_dma, 4 metadata, 8 image format, 16 indirect
    void CollectCeReads(AmdGpu::Liverpool& live, const Pipeline* pipeline, VAddr lo, VAddr hi) {
        ce_reads.clear();
        ce_untracked = pipeline ? 0 : 1;
        const auto add = [&](VAddr at, u64 bytes) {
            if (at && bytes && at < hi && lo < at + bytes)
                ce_reads.push_back({at, bytes});
        };
        // Image footprints are only computed for bases this close below the box.
        constexpr u64 kImageWindow = 256ull << 20;
        const auto close_to = [&](VAddr at) { return at && at < hi && at + kImageWindow > lo; };
        const auto image = [&](const VideoCore::ImageInfo& info) {
            add(info.guest_address, info.guest_size);
            add(info.stencil_addr, info.stencil_size);
            // Metadata sizes are not modeled; each is bounded by its surface's footprint.
            const u64 meta_bound = std::max<u64>(info.guest_size, 64_KB);
            for (const VAddr meta : {VAddr(info.meta_info.cmask_addr), VAddr(info.meta_info.fmask_addr),
                                     VAddr(info.meta_info.htile_addr)})
                if (meta && !info.guest_size && close_to(meta))
                    ce_untracked |= 4;
                else
                    add(meta, meta_bound);
        };
        if (pipeline)
            for (const auto* stage : pipeline->GetStages()) {
                if (!stage)
                    continue;
                ce_untracked |= stage->uses_dma ? 2 : 0;
                for (const auto& desc : stage->buffers) {
                    if (desc.IsSpecial())
                        continue;
                    const auto sharp = desc.GetSharp(*stage);
                    add(sharp.base_address, sharp.GetSize());
                }
                for (const auto& desc : stage->images) {
                    const auto sharp = desc.GetSharp(*stage);
                    if (!close_to(sharp.Address()) ||
                        sharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid)
                        continue;
                    if (sharp.pitch < sharp.width ||
                        !magic_enum::enum_contains(sharp.GetDataFmt()) ||
                        !magic_enum::enum_contains(sharp.GetNumberFmt())) {
                        ce_untracked |= 8;
                        continue;
                    }
                    image(VideoCore::ImageInfo{sharp, desc});
                }
            }
        for (size_t i = 0; i < stage_count; ++i)
            for (const auto& read : reads[i]) {
                AmdGpu::Buffer sharp;
                std::memcpy(&sharp, read.words.data(), sizeof(sharp));
                add(sharp.base_address, sharp.GetSize());
            }
        if (kind == PacketKind::Draw || kind == PacketKind::DrawIndirect ||
            kind == PacketKind::FilteredDraw) {
            for (u32 i = 0; i < AmdGpu::NUM_COLOR_BUFFERS; ++i)
                if (live.regs.color_buffers[i] && close_to(live.regs.color_buffers[i].Address()))
                    image(VideoCore::ImageInfo{live.regs.color_buffers[i], live.last_cb_extent[i]});
            const auto& db = live.regs.depth_buffer;
            if ((db.DepthValid() || db.StencilValid()) &&
                (close_to(db.DepthAddress()) || close_to(db.StencilAddress()) ||
                 close_to(live.regs.depth_htile_data_base.GetAddress()))) {
                const VideoCore::TextureCache::ImageDesc desc{
                    db, live.regs.depth_view, live.regs.depth_control,
                    live.regs.depth_htile_data_base.GetAddress(), live.last_db_extent};
                image(desc.info);
            }
            if (indexed && kind == PacketKind::Draw) {
                const u32 width =
                    live.regs.index_buffer_type.index_type == AmdGpu::IndexType::Index16 ? 2 : 4;
                add(live.regs.index_base_address.Address<VAddr>() + u64(offset) * width,
                    u64(live.regs.num_indices) * width);
            }
        }
        if (kind == PacketKind::DrawIndirect || kind == PacketKind::DispatchIndirect) {
            // DrawIndirectMulti reads stride * max_count bytes of argument records.
            const u64 args = u64(std::max<u32>(size, 4)) * std::max<u32>(count, 1); // bbport 1127-1131
            add(address + offset, args);
            add(count_address, 4);
            ce_untracked |= indexed || args > (16u << 20) ? 16 : 0;
        }
    }
    void CaptureMemoryContents() {
        // Register/descriptor metadata is already owned. Fault-driven readbacks
        // can now wait for B without holding its pipeline-selection mutex.
        FastRender::ProducerScope allow_readback{false};
        for (size_t i = 0; i < memory_count; ++i) {
            auto& read = memory[i];
            std::memcpy(read.words.data(), reinterpret_cast<const void*>(read.address),
                        read.words.size() * sizeof(u32));
        }
    }
    // Copy only owned bytes into the transport. Never publish vector/span objects or guest pointers
    // as payload storage. Pointer identities below refer to stable compiled pipelines/programs.
    size_t Encode(std::byte* output = nullptr) const {
        FastRender::PacketWriter writer{output, packet_views};
        const auto value = [&](const auto& item) { writer.Value(item); };
        const auto words = [&](const auto& items) { writer.Array(items); };
        value(packet_views);
        value(selected);
        value(constant_page);
        if (constant_page != ConstantRing::NoPage) {
            value(constant_downloads);
            value(constant_verify);
        }
        words(constants);
        value(resources_prepared);
        value(vertex_prepared);
        if (vertex_prepared) {
            words(vertex.attributes);
            words(vertex.bindings);
            words(vertex.guest_buffers);
            words(vertex.ranges);
            value(vertex.geometry);
        }
        value(kind);
        value(vr);
        value(camera);
        value(indexed);
        value(address);
        value(count_address);
        value(offset);
        value(size);
        value(count);
        value(sequence);
        value(bytes);
        value(verify);
        value(cb_extent);
        value(db_extent);
        value(compute);
        value(regs_reset);
        words(deltas);
        if (verify)
            value(*reference);
        value(static_cast<u32>(stage_count));
        for (size_t i = 0; i < stage_count; ++i) {
            value(owned[i].owner);
            value(owned[i].program_base);
            words(owned[i].user);
            words(owned[i].flat);
            words(reads[i]);
            if (resources_prepared) {
                const auto& r = resources[i];
                // The views: Build's own vectors, or the workers' records (bit 58).
                words(r.image_values);
                words(r.sampler_values);
                words(r.buffer_values);
                value(r.keyed);
                if (r.keyed) {
                    words(std::span{r.key.words.data(), r.key.count * PerfMemo::BindKey::WordsPerImage});
                    value(r.key_hash);
                }
            }
        }
        value(static_cast<u32>(memory_count));
        for (size_t i = 0; i < memory_count; ++i) {
            value(memory[i].address);
            words(memory[i].words);
        }
        return writer.Size();
    }
    void Decode(std::span<const std::byte> data) {
        FastRender::BProfile::Scope b_profile{FastRender::BProfile::Replay};
        FastRender::PacketReader reader{data};
        const auto value = [&](auto& item) { reader.Value(item); };
        const auto words = [&](auto& items) { reader.Copy(items); };
        value(packet_views);
        reader.SetAligned(packet_views);
        const auto payload = [&](auto& storage, auto& view) {
            using T = std::remove_cvref_t<decltype(storage[0])>;
            if (packet_views) {
                view = reader.View<T>();
            } else {
                words(storage);
                view = storage;
            }
        };
        value(selected);
        value(constant_page);
        constant_downloads = NoDownloads;
        constant_verify = false;
        if (constant_page != ConstantRing::NoPage) {
            value(constant_downloads);
            value(constant_verify);
        }
        payload(constants, constant_values);
        value(resources_prepared);
        value(vertex_prepared);
        if (vertex_prepared) {
            words(vertex.attributes);
            words(vertex.bindings);
            words(vertex.guest_buffers);
            words(vertex.ranges);
            value(vertex.geometry);
            vertex_view = VertexView{vertex};
        }
        value(kind);
        value(vr);
        value(camera);
        value(indexed);
        value(address);
        value(count_address);
        value(offset);
        value(size);
        value(count);
        value(sequence);
        value(bytes);
        value(verify);
        value(cb_extent);
        value(db_extent);
        value(compute);
        compute_sent = true;
        value(regs_reset);
        payload(deltas, delta_values);
        if (verify) {
            if (!reference)
                reference = std::make_unique<AmdGpu::Regs>();
            value(*reference);
        }
        u32 stages;
        value(stages);
        ASSERT(stages <= owned.size());
        stage_count = stages;
        for (size_t i = 0; i < stage_count; ++i) {
            value(owned[i].owner);
            value(owned[i].program_base);
            inputs[i].owner = owned[i].owner;
            inputs[i].program_base = owned[i].program_base;
            payload(owned[i].user, inputs[i].user);
            payload(owned[i].flat, inputs[i].flat);
            payload(reads[i], inputs[i].register_reads);
            if (resources_prepared) {
                auto& r = resources[i];
                payload(r.images, r.image_values);
                payload(r.samplers, r.sampler_values);
                payload(r.buffers, r.buffer_values);
                const auto& stage = *static_cast<const Shader::Info*>(owned[i].owner);
                ASSERT(r.image_values.size() == stage.images.size() &&
                       r.sampler_values.size() == stage.samplers.size() &&
                       r.buffer_values.size() == stage.buffers.size());
                value(r.keyed);
                r.key.Clear();
                if (r.keyed) {
                    const auto count = reader.CopyPrefix(std::span<u64>{r.key.words});
                    ASSERT(count % PerfMemo::BindKey::WordsPerImage == 0);
                    r.key.count = static_cast<u32>(count / PerfMemo::BindKey::WordsPerImage);
                    ASSERT(r.key.count == r.image_values.size());
                    value(r.key_hash);
                }
            }
        }
        u32 memories;
        value(memories);
        ASSERT(memories <= reader.Remaining() / sizeof(u32));
        memory_count = memories;
        memory_views.clear();
        if (packet_views) {
            for (u32 i = 0; i < memories; ++i) {
                VAddr address;
                value(address);
                memory_views.push_back({address, reader.View<u32>()});
            }
        } else {
            memory.resize(memories);
            for (auto& read : memory) {
                value(read.address);
                words(read.words);
                memory_views.push_back({read.address, read.words});
            }
        }
        ASSERT(reader.Remaining() == 0);
        // DrawPipe releases the queue storage only after ConsumePacket returns.
        // No view may escape that handler; Vulkan recording copies its own payloads.
    }

    // FAST_CPU_SLIM_PACKET: the fixed fields, sent and read in one copy (Encode sends them one by
    // one), and what follows them.
    enum SlimFlag : u32 {
        SlimViews = 1u << 0,
        SlimConstantVerify = 1u << 1,
        SlimResources = 1u << 2,
        SlimVertex = 1u << 3,
        SlimIndexed = 1u << 4,
        SlimVerify = 1u << 5,
        SlimRegsReset = 1u << 6,
        SlimCamera = 1u << 7,  // the camera record follows (it changed)
        SlimCompute = 1u << 8, // the compute registers follow (a dispatch)
    };
    struct SlimHeader {
        const Pipeline* selected;
        VAddr address, count_address;
        u64 constant_downloads, sequence, bytes, geometry;
        PreparedVrDraw vr;
        std::array<AmdGpu::CbDbExtent, AmdGpu::NUM_COLOR_BUFFERS> cb_extent;
        AmdGpu::CbDbExtent db_extent;
        u32 constant_page, offset, size, count, stage_count, memory_count, flags;
        PacketKind kind;
    };
    static_assert(std::is_trivially_copyable_v<SlimHeader>);
    // B binds a prepared constant only with a ring binding, and checks a probe; the refused rest
    // (no binding) it never reads.
    static bool SlimKeeps(const ConstantBinding& c) {
        return c.raw.buffer || c.probe;
    }
    static bool SlimDraw(PacketKind kind) {
        return kind != PacketKind::Dispatch && kind != PacketKind::DispatchIndirect;
    }
    // Stage A, before both encode passes: what this packet carries beyond the rest. The camera
    // record is compared byte for byte with the one B holds (the last one sent).
    void SlimChoose() {
        send_compute = !SlimDraw(kind);
        send_camera = held_camera.Differs(camera);
    }
    // Stage A: the slim form (with _VERIFY followed by the full one at the next aligned offset)
    // into the pipe; the push's ticket.
    u64 PushSlim(DrawPipe& pipe) {
        SlimChoose();
        const size_t slim = EncodeSlim();
        const size_t full_at =
            g_slim_packet_verify ? Common::AlignUp(slim, FastRender::PacketAlignment) : slim;
        const size_t full = g_slim_packet_verify ? Encode() : 0;
        const auto total = static_cast<u32>(full_at + full);
        const u64 ticket = pipe.PushEncoded(total, [&](std::byte* destination) {
            const auto encoded = EncodeSlim(destination);
            ASSERT(encoded == slim);
            if (!g_slim_packet_verify)
                return;
            std::memset(destination + slim, 0, full_at - slim);
            const auto encoded_full = Encode(destination + full_at);
            ASSERT(encoded_full == full);
        });
        if (send_camera)
            held_camera.Sent(camera);
        if constexpr (Common::Log::Enabled) {
            ++slim_packets;
            slim_bytes += slim;
            slim_full_bytes += full;
            slim_cameras += send_camera;
            slim_computes += send_compute;
            slim_constants += constants.size();
            slim_kept += std::ranges::count_if(constants, SlimKeeps);
            if ((slim_packets & 65535) == 0)
                LOG_INFO(Render_Vulkan,
                         "FAST_CPU_SLIM_PACKET: packets={} bytes/packet={} full_bytes/packet={} "
                         "(verify only) camera_sends={} compute_sends={} constants={} sent={}",
                         slim_packets, slim_bytes / slim_packets, slim_full_bytes / slim_packets,
                         slim_cameras, slim_computes, slim_constants, slim_kept);
        }
        return ticket;
    }
    size_t EncodeSlim(std::byte* output = nullptr) const {
        FastRender::PacketWriter writer{output, packet_views};
        const auto value = [&](const auto& item) { writer.Value(item); };
        const auto words = [&](const auto& items) { writer.Array(items); };
        const bool page = constant_page != ConstantRing::NoPage;
        SlimHeader h{};
        h.selected = selected;
        h.address = address;
        h.count_address = count_address;
        h.constant_downloads = page ? constant_downloads : NoDownloads;
        h.sequence = sequence;
        h.bytes = bytes;
        h.geometry = vertex.geometry;
        h.vr = vr;
        h.cb_extent = cb_extent;
        h.db_extent = db_extent;
        h.constant_page = constant_page;
        h.offset = offset;
        h.size = size;
        h.count = count;
        h.stage_count = static_cast<u32>(stage_count);
        h.memory_count = static_cast<u32>(memory_count);
        h.kind = kind;
        h.flags = (packet_views ? SlimViews : 0) |
                  (page && constant_verify ? SlimConstantVerify : 0) |
                  (resources_prepared ? SlimResources : 0) | (vertex_prepared ? SlimVertex : 0) |
                  (indexed ? SlimIndexed : 0) | (verify ? SlimVerify : 0) |
                  (regs_reset ? SlimRegsReset : 0) | (send_camera ? SlimCamera : 0) |
                  (send_compute ? SlimCompute : 0);
        value(h);
        if (send_camera)
            value(camera);
        if (send_compute)
            value(compute);
        writer.ArrayIf(constants, SlimKeeps);
        if (vertex_prepared) {
            words(vertex.attributes);
            words(vertex.bindings);
            words(vertex.guest_buffers);
            words(vertex.ranges);
        }
        words(deltas);
        if (verify)
            value(*reference);
        for (size_t i = 0; i < stage_count; ++i) {
            value(owned[i].owner);
            value(owned[i].program_base);
            words(owned[i].user);
            words(owned[i].flat);
            words(reads[i]);
            if (resources_prepared) {
                const auto& r = resources[i];
                words(r.image_values);
                words(r.sampler_values);
                words(r.buffer_values);
                value(r.keyed);
                if (r.keyed) {
                    words(std::span{r.key.words.data(),
                                    r.key.count * PerfMemo::BindKey::WordsPerImage});
                    value(r.key_hash);
                }
            }
        }
        for (size_t i = 0; i < memory_count; ++i) {
            value(memory[i].address);
            words(memory[i].words);
        }
        return writer.Size();
    }
    // Stage B: Decode's results from the slim form, except what B never reads (see the switch);
    // the bytes it took (with _VERIFY the full form follows at the next aligned offset).
    size_t DecodeSlim(std::span<const std::byte> data) {
        FastRender::BProfile::Scope b_profile{FastRender::BProfile::Replay};
        FastRender::PacketCursor reader{data, FastRender::g_pipe_verify};
        SlimHeader h;
        reader.Value(h);
        packet_views = (h.flags & SlimViews) != 0;
        reader.SetAligned(packet_views);
        const auto payload = [&](auto& storage, auto& view) {
            using T = std::remove_cvref_t<decltype(storage[0])>;
            if (packet_views) {
                view = reader.View<T>();
            } else {
                reader.Copy(storage);
                view = storage;
            }
        };
        selected = h.selected;
        address = h.address;
        count_address = h.count_address;
        constant_downloads = h.constant_downloads;
        sequence = h.sequence;
        bytes = h.bytes;
        vr = h.vr;
        cb_extent = h.cb_extent;
        db_extent = h.db_extent;
        constant_page = h.constant_page;
        offset = h.offset;
        size = h.size;
        count = h.count;
        kind = h.kind;
        constant_verify = (h.flags & SlimConstantVerify) != 0;
        resources_prepared = (h.flags & SlimResources) != 0;
        vertex_prepared = (h.flags & SlimVertex) != 0;
        indexed = (h.flags & SlimIndexed) != 0;
        verify = (h.flags & SlimVerify) != 0;
        regs_reset = (h.flags & SlimRegsReset) != 0;
        if (h.flags & SlimCamera)
            reader.Value(camera);
        compute_sent = (h.flags & SlimCompute) != 0;
        if (compute_sent)
            reader.Value(compute);
        payload(constants, constant_values);
        if (vertex_prepared) {
            if (packet_views) {
                vertex_view.attributes = reader.View<vk::VertexInputAttributeDescription2EXT>();
                vertex_view.bindings = reader.View<vk::VertexInputBindingDescription2EXT>();
                vertex_view.guest_buffers = reader.View<AmdGpu::Buffer>();
                vertex_view.ranges = reader.View<VertexPreparation::Range>();
                vertex_view.geometry = h.geometry;
            } else {
                reader.Copy(vertex.attributes);
                reader.Copy(vertex.bindings);
                reader.Copy(vertex.guest_buffers);
                reader.Copy(vertex.ranges);
                vertex.geometry = h.geometry;
                vertex_view = VertexView{vertex};
            }
        }
        payload(deltas, delta_values);
        if (verify) {
            if (!reference)
                reference = std::make_unique<AmdGpu::Regs>();
            reader.Value(*reference);
        }
        ASSERT(h.stage_count <= owned.size());
        stage_count = h.stage_count;
        for (size_t i = 0; i < stage_count; ++i) {
            reader.Value(owned[i].owner);
            reader.Value(owned[i].program_base);
            inputs[i].owner = owned[i].owner;
            inputs[i].program_base = owned[i].program_base;
            payload(owned[i].user, inputs[i].user);
            payload(owned[i].flat, inputs[i].flat);
            payload(reads[i], inputs[i].register_reads);
            if (resources_prepared) {
                auto& r = resources[i];
                payload(r.images, r.image_values);
                payload(r.samplers, r.sampler_values);
                payload(r.buffers, r.buffer_values);
                const auto& stage = *static_cast<const Shader::Info*>(owned[i].owner);
                ASSERT(r.image_values.size() == stage.images.size() &&
                       r.sampler_values.size() == stage.samplers.size() &&
                       r.buffer_values.size() == stage.buffers.size());
                reader.Value(r.keyed);
                r.key.Clear();
                if (r.keyed) {
                    const auto count = reader.CopyPrefix(std::span<u64>{r.key.words});
                    ASSERT(count % PerfMemo::BindKey::WordsPerImage == 0);
                    r.key.count = static_cast<u32>(count / PerfMemo::BindKey::WordsPerImage);
                    ASSERT(r.key.count == r.image_values.size());
                    reader.Value(r.key_hash);
                }
            }
        }
        const u32 memories = h.memory_count;
        if (reader.Checked())
            ASSERT(memories <= reader.Remaining() / sizeof(u32));
        memory_count = memories;
        memory_views.clear();
        if (packet_views) {
            for (u32 i = 0; i < memories; ++i) {
                VAddr at;
                reader.Value(at);
                memory_views.push_back({at, reader.View<u32>()});
            }
        } else {
            memory.resize(memories);
            for (auto& read : memory) {
                reader.Value(read.address);
                reader.Copy(read.words);
                memory_views.push_back({read.address, read.words});
            }
        }
        return reader.Position();
    }
    // FAST_CPU_SLIM_PACKET_VERIFY, stage B: the slim form into slim_check, the full one into this
    // packet (consumed as with the switch off), compared. A draw's compute register reads are
    // counted while it is consumed (SlimVerifyReads after).
    void DecodeSlimVerify(std::span<const std::byte> data) {
        auto& slim = *slim_check;
        const size_t used = slim.DecodeSlim(data);
        const size_t full_at = Common::AlignUp(used, FastRender::PacketAlignment);
        ASSERT(full_at <= data.size());
        Decode(data.subspan(full_at));
        ++slim_verified;
        slim_verify_bytes += used;
        slim_verify_full_bytes += data.size() - full_at;
        if (const char* part = slim.SlimDiffers(*this)) {
            if (++slim_mismatches <= 32 || (slim_mismatches & 1023) == 0)
                LOG_ERROR(Render_Vulkan,
                          "FAST_CPU_SLIM_PACKET verify: packet {} (kind {}) differs in {}; the "
                          "full form is used, mismatch #{}",
                          sequence, static_cast<u32>(kind), part, slim_mismatches);
        }
        registers.compute_unsent = !slim.compute_sent;
        registers.compute_unsent_reads = 0;
        if constexpr (Common::Log::Enabled) {
            if ((slim_verified & 65535) == 0)
                LOG_INFO(Render_Vulkan,
                         "FAST_CPU_SLIM_PACKET verify: packets={} mismatches={} "
                         "draws_reading_compute={} bytes/packet={} full_bytes/packet={}",
                         slim_verified, slim_mismatches, slim_compute_reads,
                         slim_verify_bytes / slim_verified, slim_verify_full_bytes / slim_verified);
        }
    }
    void SlimVerifyReads() {
        if (registers.compute_unsent_reads != 0 &&
            (++slim_compute_reads <= 32 || (slim_compute_reads & 1023) == 0))
            LOG_ERROR(
                Render_Vulkan,
                "FAST_CPU_SLIM_PACKET verify: draw packet {} (kind {}) read the compute "
                "registers {} times, which its slim form leaves the last dispatch's, case #{}",
                sequence, static_cast<u32>(kind), registers.compute_unsent_reads,
                slim_compute_reads);
        registers.compute_unsent = false;
        registers.compute_unsent_reads = 0;
    }
    // FAST_CPU_SLIM_PACKET_VERIFY: the first part in which this slim decode differs from `full`'s
    // (the same packet's full form), or null. Arrays and records are compared byte for byte: both
    // forms copy them from the same stage A objects.
    const char* SlimDiffers(const DrawPacket& full) const {
        const auto same = [](const auto& a, const auto& b) {
            using T = std::remove_cvref_t<decltype(*std::data(a))>;
            static_assert(std::is_same_v<T, std::remove_cvref_t<decltype(*std::data(b))>>);
            return std::size(a) == std::size(b) &&
                   (std::size(a) == 0 ||
                    std::memcmp(std::data(a), std::data(b), std::size(a) * sizeof(T)) == 0);
        };
        const auto same_value = [](const auto& a, const auto& b) {
            static_assert(sizeof(a) == sizeof(b));
            return std::memcmp(&a, &b, sizeof(a)) == 0;
        };
        // The header's copy may not keep its padding: field by field.
        const auto& v = vr;
        const auto& w = full.vr;
        if (v.split != w.split || v.split_count != w.split_count || v.body_split != w.body_split ||
            v.body_cut_pipeline != w.body_cut_pipeline ||
            v.body_cape_hidden != w.body_cape_hidden || v.body_head_hidden != w.body_head_hidden ||
            v.body_shoulder_cut != w.body_shoulder_cut || v.hands_split != w.hands_split ||
            v.hands_split_count != w.hands_split_count || v.cut_code != w.cut_code ||
            v.probe_selected != w.probe_selected)
            return "vr";
        if (packet_views != full.packet_views || selected != full.selected ||
            constant_page != full.constant_page || constant_downloads != full.constant_downloads ||
            constant_verify != full.constant_verify ||
            resources_prepared != full.resources_prepared ||
            vertex_prepared != full.vertex_prepared || kind != full.kind ||
            indexed != full.indexed || address != full.address ||
            count_address != full.count_address || offset != full.offset || size != full.size ||
            count != full.count || sequence != full.sequence || bytes != full.bytes ||
            verify != full.verify || regs_reset != full.regs_reset ||
            !same_value(cb_extent, full.cb_extent) || !same_value(db_extent, full.db_extent))
            return "header";
        if (!same_value(camera, full.camera))
            return "camera";
        if (compute_sent ? !same_value(compute, full.compute) : !SlimDraw(kind))
            return "compute";
        size_t kept = 0;
        for (const auto& c : full.constant_values) {
            if (!SlimKeeps(c))
                continue;
            if (kept == constant_values.size() || !same_value(constant_values[kept], c))
                return "constants";
            ++kept;
        }
        if (kept != constant_values.size())
            return "constants";
        if (vertex_prepared) {
            const auto& a = vertex_view;
            const auto& b = full.vertex_view;
            if (!same(a.attributes, b.attributes) || !same(a.bindings, b.bindings) ||
                !same(a.guest_buffers, b.guest_buffers) || !same(a.ranges, b.ranges) ||
                a.geometry != b.geometry)
                return "vertex";
        }
        if (!same(delta_values, full.delta_values))
            return "registers";
        if (verify && !same_value(*reference, *full.reference))
            return "reference";
        if (stage_count != full.stage_count)
            return "stages";
        for (size_t i = 0; i < stage_count; ++i) {
            const auto& a = inputs[i];
            const auto& b = full.inputs[i];
            if (a.owner != b.owner || a.program_base != b.program_base || !same(a.user, b.user) ||
                !same(a.flat, b.flat) || !same(a.register_reads, b.register_reads))
                return "inputs";
            if (!resources_prepared)
                continue;
            const auto& r = resources[i];
            const auto& s = full.resources[i];
            if (!same(r.image_values, s.image_values) ||
                !same(r.sampler_values, s.sampler_values) ||
                !same(r.buffer_values, s.buffer_values) || r.keyed != s.keyed ||
                (r.keyed && (r.key_hash != s.key_hash || r.key.count != s.key.count ||
                             !same(std::span{r.key.words.data(),
                                             r.key.count * PerfMemo::BindKey::WordsPerImage},
                                   std::span{s.key.words.data(),
                                             s.key.count * PerfMemo::BindKey::WordsPerImage}))))
                return "resources";
        }
        if (memory_count != full.memory_count || memory_views.size() != full.memory_views.size())
            return "memory";
        for (size_t i = 0; i < memory_views.size(); ++i)
            if (memory_views[i].address != full.memory_views[i].address ||
                !same(memory_views[i].words, full.memory_views[i].words))
                return "memory";
        return nullptr;
    }
    static const AmdGpu::Regs& DefaultRegisters() {
        static const auto defaults = [] {
            auto regs = std::make_unique<AmdGpu::Regs>();
            regs->SetDefaults();
            return regs;
        }();
        return *defaults;
    }
    // bbport vk_rasterizer.cpp:1318: `if (packet.regs_reset) regs.SetDefaults();`. Here only the
    // blocks this consumer wrote since its last reset differ from the defaults.
    void ResetRegisters() {
        static const u64 verify_every = [] {
            const char* value = std::getenv("FAST_CLEARSTATE_VERIFY");
            return value ? std::strtoull(value, nullptr, 0) : 0ULL;
        }();
        using Deltas = AmdGpu::Liverpool::RegisterDeltas;
        auto& file = registers.regs.reg_array;
        const auto& defaults = DefaultRegisters().reg_array;
        if (replay_untracked) {
            registers.regs.SetDefaults();
            ++reset_misses;
        } else {
            for (const u16 index : replay_touched) {
                const size_t first = size_t(index) * Deltas::WordsPerBlock;
                std::copy_n(defaults.data() + first,
                            std::min(Deltas::WordsPerBlock, file.size() - first),
                            file.data() + first);
            }
            reset_blocks += replay_touched.size();
            ++reset_hits;
        }
        replay_touched.clear();
        replay_touched_bits.reset();
        replay_untracked = false;
        const u64 resets = reset_hits + reset_misses;
        if (verify_every && resets % verify_every == 0) {
            ++reset_checks;
            if (std::memcmp(file.data(), defaults.data(), sizeof(file)) != 0) {
                size_t word = 0;
                while (file[word] == defaults[word])
                    ++word;
                ++reset_mismatches;
                LOG_ERROR(Render_Vulkan,
                          "FAST clearstate packet {} register {:#x}: replay={:#x} default={:#x}",
                          sequence, word, file[word], defaults[word]);
                registers.regs.SetDefaults();
            }
        }
        if ((resets & 4095) == 0)
            LOG_INFO(Render_Vulkan,
                     "FAST clearstate replay: hits={} misses={} blocks/hit={} checks={} "
                     "mismatches={}",
                     reset_hits, reset_misses, reset_hits ? reset_blocks / reset_hits : 0,
                     reset_checks, reset_mismatches);
    }
    void Replay() {
        FastRender::BProfile::Scope b_profile{FastRender::BProfile::Replay};
        if (regs_reset)
            ResetRegisters();
        AmdGpu::Liverpool::RegisterDeltas::Apply(registers.regs.reg_array, delta_values);
        if (!FastRender::On(AmdGpu::Liverpool::ClearStateReset)) {
            replay_untracked = true;
        } else if (!replay_untracked) {
            for (const auto& block : delta_values)
                if (!replay_touched_bits[block.index]) {
                    replay_touched_bits[block.index] = true;
                    replay_touched.push_back(block.index);
                }
        }
        registers.cb_extent = cb_extent;
        registers.db_extent = db_extent;
        if (compute_sent) // FAST_CPU_SLIM_PACKET: a draw's slim form has none (unchanged since)
            registers.compute = compute;
        if (verify) {
            for (size_t word = 0; word < AmdGpu::Regs::NumRegs; ++word) {
                ASSERT_MSG(registers.regs.reg_array[word] == reference->reg_array[word],
                           "FAST packet {} register {:#x}: replay={:#x} live={:#x}", sequence, word,
                           registers.regs.reg_array[word], reference->reg_array[word]);
            }
            ++verified;
        }
        if ((sequence & 65535) == 0) {
            LOG_INFO(Render_Vulkan,
                     "FAST packet loopback: packets={} verified={} register_bytes/packet={}",
                     sequence, verified, bytes / sequence);
            LOG_INFO(Render_Vulkan, "FAST packet payload: views={} sequence={}", packet_views, sequence);
        }
    }
};

// Toggle bit 29 (reader_4 item 14): skip dynamic-state recomputation whose inputs did not change.
// Register inputs are tracked per delta-tracker block, the granularity of bbport's RegDirty
// (BB amdgpu/regs.h:198-221); bbport itself recomputes every draw (vk_rasterizer.cpp:4308).
// Hits still go through DynamicState::Set*, so Scheduler's Invalidate() on a new command buffer
// and the VR writers between draws (vk_rasterizer_vr.cpp:1359) behave as before.
static constexpr auto DynStateSkip = static_cast<FastRender::Bit>(29);

struct DynSkipRange {
    u32 first, count;
};
// Words from the first field through the last one (inclusive), padding between them included.
#define DYN_SKIP_SPAN(first_field, last_field)                                                     \
    DynSkipRange{u32(offsetof(AmdGpu::Regs, first_field) / sizeof(u32)),                           \
                 u32((offsetof(AmdGpu::Regs, last_field) +                                         \
                      sizeof(decltype(AmdGpu::Regs::last_field)) + sizeof(u32) - 1) /              \
                         sizeof(u32) -                                                             \
                     offsetof(AmdGpu::Regs, first_field) / sizeof(u32))}
// Register reads of UpdateViewportScissorState, UpdateDepthStencilState and UpdatePrimitiveState.
// primitive_type shares its block with num_indices (rewritten by every draw), so it is compared
// by value with the non-register inputs instead.
static constexpr std::array DynSkipViewportRanges{
    DYN_SKIP_SPAN(screen_scissor, screen_scissor),
    DYN_SKIP_SPAN(window_offset, window_scissor),
    DYN_SKIP_SPAN(generic_scissor, viewport_scissors),
    DYN_SKIP_SPAN(viewports, viewports),
    DYN_SKIP_SPAN(clipper_control, viewport_control), // clipper, polygon, viewport control
    DYN_SKIP_SPAN(mode_control, mode_control)};
static constexpr std::array DynSkipDepthRanges{
    DYN_SKIP_SPAN(depth_render_control, depth_buffer), // includes depth bounds
    DYN_SKIP_SPAN(stencil_control, stencil_ref_back),
    DYN_SKIP_SPAN(depth_control, depth_control),
    DYN_SKIP_SPAN(polygon_control, polygon_control),
    DYN_SKIP_SPAN(poly_offset, poly_offset)};
static constexpr std::array DynSkipPrimitiveRanges{
    DYN_SKIP_SPAN(primitive_restart_index, primitive_restart_index),
    DYN_SKIP_SPAN(clipper_control, polygon_control),
    DYN_SKIP_SPAN(enable_primitive_restart, enable_primitive_restart)};
#undef DYN_SKIP_SPAN
template <size_t N>
constexpr bool DynSkipValid(const std::array<DynSkipRange, N>& ranges) {
    for (const auto& range : ranges)
        if (range.count == 0 || range.count > AmdGpu::Regs::NumRegs - range.first)
            return false;
    return true;
}
static_assert(DynSkipValid(DynSkipViewportRanges) && DynSkipValid(DynSkipDepthRanges) &&
              DynSkipValid(DynSkipPrimitiveRanges));
static constexpr u32 DynSkipPrimitiveTypeWord =
    u32(offsetof(AmdGpu::Regs, primitive_type) / sizeof(u32));
static constexpr u32 DynSkipBlockWords =
    u32(sizeof(decltype(AmdGpu::Liverpool::RegisterDeltas::Block::words)) / sizeof(u32));
static constexpr u32 DynSkipBlocks =
    (AmdGpu::Regs::NumRegs + DynSkipBlockWords - 1) / DynSkipBlockWords;
enum DynSkipGroup : u8 {
    DynSkipViewport = 1,
    DynSkipDepth = 2,
    DynSkipPrimitive = 4,
    DynSkipAll = 7,
};
static constexpr auto DynSkipBlockGroups = [] {
    std::array<u8, DynSkipBlocks> groups{};
    const auto mark = [&groups](const auto& ranges, u8 group) {
        for (const auto& range : ranges)
            for (u32 block = range.first / DynSkipBlockWords;
                 block <= (range.first + range.count - 1) / DynSkipBlockWords; ++block)
                groups[block] = u8(groups[block] | group);
    };
    mark(DynSkipViewportRanges, DynSkipViewport);
    mark(DynSkipDepthRanges, DynSkipDepth);
    mark(DynSkipPrimitiveRanges, DynSkipPrimitive);
    return groups;
}();
template <size_t N>
constexpr u32 DynSkipWords(const std::array<DynSkipRange, N>& ranges) {
    u32 words = 0;
    for (const auto& range : ranges)
        words += range.count;
    return words;
}
template <u32 Words>
struct DynSkipKey {
    std::array<u32, Words> words{};
    template <size_t N>
    bool Matches(const AmdGpu::Regs& regs, const std::array<DynSkipRange, N>& ranges) const {
        // Branch-free OR reduction: vectorizes, no memcmp call per short span.
        const u32* key = words.data();
        u32 diff = 0;
        for (const auto& range : ranges) {
            const u32* reg = regs.reg_array.data() + range.first;
            for (u32 i = 0; i < range.count; ++i)
                diff |= reg[i] ^ key[i];
            key += range.count;
        }
        return diff == 0;
    }
    template <size_t N>
    void Store(const AmdGpu::Regs& regs, const std::array<DynSkipRange, N>& ranges) {
        u32* key = words.data();
        for (const auto& range : ranges) {
            std::memcpy(key, regs.reg_array.data() + range.first, range.count * sizeof(u32));
            key += range.count;
        }
    }
};
static bool DynSkipSame(float a, float b) {
    return a == b || std::bit_cast<u32>(a) == std::bit_cast<u32>(b);
}

// Owned by the recording owner (B, or A when serial); worker toggles drain B before handing over.
struct Rasterizer::DynSkip {
    struct NoInputs {
        bool operator==(const NoInputs&) const = default;
    };
    struct ViewportInputs {
        u32 primitive_type{}, jitter_on{};
        std::array<u32, 2> jitter{}, inverse{};
        bool operator==(const ViewportInputs&) const = default;
    };
    struct PrimitiveInputs {
        u32 primitive_type{};
        bool indexed{};
        bool operator==(const PrimitiveInputs&) const = default;
    };
    template <u32 Words, typename Inputs>
    struct Entry {
        bool valid{};
        Inputs inputs{};
        DynSkipKey<Words> key{};
    };
    // DynamicState after UpdateDepthStencilState; gated fields are applied and compared only
    // when their enable is set, as that function sets them.
    struct DepthState {
        bool depth_test{}, depth_write{}, bounds_test{}, bias{}, stencil{};
        vk::CompareOp depth_op{};
        float bounds_min{}, bounds_max{}, bias_constant{}, bias_clamp{}, bias_slope{};
        StencilOps front_ops{}, back_ops{};
        u32 front_reference{}, back_reference{}, front_write_mask{}, back_write_mask{},
            front_compare_mask{}, back_compare_mask{};
        void Read(const DynamicState& d) {
            depth_test = d.depth_test_enabled;
            depth_write = d.depth_write_enabled;
            depth_op = d.depth_compare_op;
            bounds_test = d.depth_bounds_test_enabled;
            bounds_min = d.depth_bounds_min;
            bounds_max = d.depth_bounds_max;
            bias = d.depth_bias_enabled;
            bias_constant = d.depth_bias_constant;
            bias_clamp = d.depth_bias_clamp;
            bias_slope = d.depth_bias_slope;
            stencil = d.stencil_test_enabled;
            front_ops = d.stencil_front_ops;
            back_ops = d.stencil_back_ops;
            front_reference = d.stencil_front_reference;
            back_reference = d.stencil_back_reference;
            front_write_mask = d.stencil_front_write_mask;
            back_write_mask = d.stencil_back_write_mask;
            front_compare_mask = d.stencil_front_compare_mask;
            back_compare_mask = d.stencil_back_compare_mask;
        }
        void Apply(DynamicState& d) const {
            d.SetDepthTestEnabled(depth_test);
            if (depth_test) {
                d.SetDepthWriteEnabled(depth_write);
                d.SetDepthCompareOp(depth_op);
            }
            d.SetDepthBoundsTestEnabled(bounds_test);
            if (bounds_test)
                d.SetDepthBounds(bounds_min, bounds_max);
            d.SetDepthBiasEnabled(bias);
            if (bias)
                d.SetDepthBias(bias_constant, bias_clamp, bias_slope);
            d.SetStencilTestEnabled(stencil);
            if (stencil) {
                d.SetStencilOps(front_ops, back_ops);
                d.SetStencilReferences(front_reference, back_reference);
                d.SetStencilWriteMasks(front_write_mask, back_write_mask);
                d.SetStencilCompareMasks(front_compare_mask, back_compare_mask);
            }
        }
        bool Same(const DepthState& o) const {
            if (depth_test != o.depth_test || bounds_test != o.bounds_test || bias != o.bias ||
                stencil != o.stencil)
                return false;
            if (depth_test && (depth_write != o.depth_write || depth_op != o.depth_op))
                return false;
            if (bounds_test && !(DynSkipSame(bounds_min, o.bounds_min) &&
                                 DynSkipSame(bounds_max, o.bounds_max)))
                return false;
            if (bias && !(DynSkipSame(bias_constant, o.bias_constant) &&
                          DynSkipSame(bias_clamp, o.bias_clamp) &&
                          DynSkipSame(bias_slope, o.bias_slope)))
                return false;
            return !stencil ||
                   (front_ops == o.front_ops && back_ops == o.back_ops &&
                    front_reference == o.front_reference && back_reference == o.back_reference &&
                    front_write_mask == o.front_write_mask &&
                    back_write_mask == o.back_write_mask &&
                    front_compare_mask == o.front_compare_mask &&
                    back_compare_mask == o.back_compare_mask);
        }
    };
    struct PrimitiveState {
        bool restart{}, discard{};
        vk::CullModeFlags cull{};
        vk::FrontFace front{};
        void Read(const DynamicState& d) {
            restart = d.primitive_restart_enable;
            discard = d.rasterizer_discard_enable;
            cull = d.cull_mode;
            front = d.front_face;
        }
        void Apply(DynamicState& d) const {
            d.SetPrimitiveRestartEnabled(restart);
            d.SetRasterizerDiscardEnabled(discard);
            d.SetCullMode(cull);
            d.SetFrontFace(front);
        }
        bool Same(const PrimitiveState& o) const {
            return restart == o.restart && discard == o.discard && cull == o.cull &&
                   front == o.front;
        }
    };

    const DrawPacket* owner{}; // packet whose replayed register file `dirty` describes
    u8 dirty{DynSkipAll};      // groups whose owner words may differ from the stored keys
    Entry<DynSkipWords(DynSkipViewportRanges), ViewportInputs> viewport;
    Entry<DynSkipWords(DynSkipDepthRanges), NoInputs> depth;
    Entry<DynSkipWords(DynSkipPrimitiveRanges), PrimitiveInputs> primitive;
    Viewports viewports{};
    Scissors scissors{};
    DepthState depth_state{};
    PrimitiveState primitive_state{};
    u64 calls{}, unowned{}, bypass{}, uses{}, verified{}, mismatches{};
    std::array<u64, 3> clean{}, equal{}, misses{};

    // After every Replay of a consumed packet. Any packet not seen here (bit off, other
    // consumer) breaks the chain, so the next owner starts with every group dirty.
    void Note(const DrawPacket* packet,
              std::span<const AmdGpu::Liverpool::RegisterDeltas::Block> deltas) {
        if (owner != packet) {
            owner = packet;
            dirty = DynSkipAll;
            return;
        }
        for (const auto& block : deltas) {
            dirty = u8(dirty | (block.index < DynSkipBlocks ? DynSkipBlockGroups[block.index]
                                                            : u8(DynSkipAll)));
            if (dirty == DynSkipAll)
                return;
        }
    }

    // True when the stored result is exact for these registers and inputs. Level 1: the owner
    // packet's replays did not touch the group's blocks since its key was last proven equal to
    // them. Level 2 (also the serial path on live registers): compare the words.
    template <u32 Words, typename Inputs, size_t N>
    bool Lookup(u32 index, Entry<Words, Inputs>& entry, const Inputs& inputs, bool owned,
                const AmdGpu::Regs& regs, const std::array<DynSkipRange, N>& ranges) {
        const u8 group = u8(1u << index);
        bool hit = false;
        if (entry.valid && entry.inputs == inputs) {
            if (owned && !(dirty & group)) {
                hit = true;
                ++clean[index];
            } else if (entry.key.Matches(regs, ranges)) {
                hit = true;
                ++equal[index];
            }
        }
        if (!hit) {
            ++misses[index];
            entry.key.Store(regs, ranges);
            entry.inputs = inputs;
            entry.valid = true;
        }
        if (owned)
            dirty = u8(dirty & ~group);
        else if (!hit)
            dirty = u8(dirty | group); // the key now describes another register file
        return hit;
    }

    // FAST_DYN_SKIP_VERIFY=N: every Nth hit recomputes the slow way and compares.
    bool Verify() {
        static const u64 verify_every = [] {
            const char* value = std::getenv("FAST_DYN_SKIP_VERIFY");
            return value ? std::strtoull(value, nullptr, 0) : 0ULL;
        }();
        if (!verify_every || ++uses % verify_every != 0)
            return false;
        ++verified;
        return true;
    }
    void Mismatch(const char* group, bool owned) {
        ++mismatches;
        LOG_ERROR(Render_Vulkan, "FAST dyn_skip: stale {} state at call {} (owned={})", group,
                  calls, owned);
    }
    static bool SameViewports(const Viewports& a, const Viewports& b) {
        return std::ranges::equal(a, b, [](const vk::Viewport& x, const vk::Viewport& y) {
            return DynSkipSame(x.x, y.x) && DynSkipSame(x.y, y.y) &&
                   DynSkipSame(x.width, y.width) && DynSkipSame(x.height, y.height) &&
                   DynSkipSame(x.minDepth, y.minDepth) && DynSkipSame(x.maxDepth, y.maxDepth);
        });
    }

    void Update(const Rasterizer& r, const GraphicsPipeline* pipeline, bool is_indexed) {
        auto& state = r.scheduler.GetDynamicState();
        const auto& regs = ScratchDraw::Registers(r.liverpool);
        const bool owned = owner && owner == r.binding_packet && &regs == &owner->registers.regs;
        ++calls;
        unowned += !owned;
        const u32 primitive_type = regs.reg_array[DynSkipPrimitiveTypeWord];

        const auto* light_vertex = pipeline->GetStages()[u32(Shader::LogicalStage::Vertex)];
        if (r.recording_view_mask == 3 && light_vertex &&
            NativeProjection::ScreenQuadShader(light_vertex->pgm_hash)) {
            // The light-scissor union depends on the per-eye cameras: always recompute.
            ++bypass;
            r.UpdateViewportScissorState(pipeline);
        } else {
            const auto& temporal = r.vr_temporal;
            const bool jitter_on =
                temporal && temporal->active && !temporal->resolved && r.db_desc.first &&
                r.db_desc.first == temporal->depth_id &&
                (regs.num_indices > 6 || regs.num_instances.NumInstances() > 1);
            const auto jitter = jitter_on ? temporal->scene.Jitter() : std::array<float, 2>{};
            const ViewportInputs inputs{
                .primitive_type = primitive_type,
                .jitter_on = jitter_on,
                .jitter = {std::bit_cast<u32>(jitter[0]), std::bit_cast<u32>(jitter[1])},
                .inverse = {std::bit_cast<u32>(r.vr_scene_inverse[0]),
                            std::bit_cast<u32>(r.vr_scene_inverse[1])},
            };
            if (!Lookup(0, viewport, inputs, owned, regs, DynSkipViewportRanges)) {
                r.UpdateViewportScissorState(pipeline);
                viewports = state.viewports;
                scissors = state.scissors;
            } else if (Verify()) {
                r.UpdateViewportScissorState(pipeline);
                if (!SameViewports(state.viewports, viewports) ||
                    !std::ranges::equal(state.scissors, scissors)) {
                    Mismatch("viewport", owned);
                    viewports = state.viewports;
                    scissors = state.scissors;
                }
            } else {
                state.SetViewports(viewports);
                state.SetScissors(scissors);
            }
        }

        if (!Lookup(1, depth, NoInputs{}, owned, regs, DynSkipDepthRanges)) {
            r.UpdateDepthStencilState();
            depth_state.Read(state);
        } else if (Verify()) {
            r.UpdateDepthStencilState();
            DepthState fresh;
            fresh.Read(state);
            if (!fresh.Same(depth_state)) {
                Mismatch("depth", owned);
                depth_state = fresh;
            }
        } else {
            depth_state.Apply(state);
        }

        const PrimitiveInputs primitive_inputs{.primitive_type = primitive_type,
                                               .indexed = is_indexed};
        if (!Lookup(2, primitive, primitive_inputs, owned, regs, DynSkipPrimitiveRanges)) {
            r.UpdatePrimitiveState(is_indexed);
            primitive_state.Read(state);
        } else if (Verify()) {
            r.UpdatePrimitiveState(is_indexed);
            PrimitiveState fresh;
            fresh.Read(state);
            if (!fresh.Same(primitive_state)) {
                Mismatch("primitive", owned);
                primitive_state = fresh;
            }
        } else {
            primitive_state.Apply(state);
        }

        if ((calls & 65535) == 0)
            LOG_INFO(Render_Vulkan,
                     "FAST dyn_skip: hits={} misses={} calls={} unowned={} light_bypass={} "
                     "clean/equal/miss viewport={}/{}/{} depth={}/{}/{} primitive={}/{}/{} "
                     "verified={} mismatches={}",
                     clean[0] + clean[1] + clean[2] + equal[0] + equal[1] + equal[2],
                     misses[0] + misses[1] + misses[2], calls, unowned, bypass, clean[0], equal[0],
                     misses[0], clean[1], equal[1], misses[1], clean[2], equal[2], misses[2],
                     verified, mismatches);
    }
};

void Rasterizer::RecordPacket(std::unique_lock<std::recursive_mutex> selection_lock,
                              const Pipeline* pipeline, PacketKind kind, bool indexed,
                              VAddr address, u32 offset, u32 size, u32 count, VAddr count_address) {
    if (!draw_packet)
        draw_packet = std::make_unique<DrawPacket>(*liverpool);
    auto& packet = *draw_packet;
    packet.selected = pipeline;
    packet.vr = producer_vr;
    packet.camera = VR::DrawFrameRecord();
    packet.kind = kind;
    packet.indexed = indexed;
    packet.address = address;
    packet.offset = offset;
    packet.size = size;
    packet.count = count;
    packet.count_address = count_address;
    const bool prepare_constants = worker_active && FastRender::On(FastRender::ConstantRing);
    // Bit 32: set by Draw for this draw packet only.
    const PreparedDraw* const reuse_from = std::exchange(packet_prepared, nullptr);
    packet.Build(*liverpool, pipeline,
                 kind == PacketKind::Draw || kind == PacketKind::DrawIndirect || kind == PacketKind::FilteredDraw, prepare_constants,
                 kind == PacketKind::Draw ? reuse_from : nullptr);
    if (DA::Enabled())
        packet.AuditResources(*liverpool, pipeline);
    // Read the stage descriptors while the selection lock still pins their Info inputs.
    const bool ce_note = worker_active && ce.mode;
    if (ce_note)
        packet.CollectCeReads(*liverpool, pipeline, ce.lo, ce.hi);
    if (selection_lock.owns_lock())
        selection_lock.unlock();
    packet.CaptureMemoryContents();
    PrepareConstants(packet, prepare_constants);
    if (worker_active) {
        u64 ticket;
        if (g_slim_packet) {
            ticket = packet.PushSlim(*draw_pipe);
        } else {
            const auto size = static_cast<u32>(packet.Encode());
            ticket = draw_pipe->PushEncoded(size, [&](std::byte* destination) {
                const auto encoded = packet.Encode(destination);
                ASSERT(encoded == size);
            });
        }
        if (prepare_constants) {
            if (packet.constant_page != ConstantRing::NoPage) constant_ring->Published(ticket);
            if (!g_guard || g_guard_verify) {
                const bool table = constant_writes.TableMode();
                for (const auto& write : packet.constant_writers) {
                    // Image footprints are also permanent ranges below; in table mode their
                    // coarse ticketed copies would only block neighbouring constants.
                    if (table && std::any_of(packet.constant_images.begin(),
                                             packet.constant_images.end(), [&](const auto& image) {
                                                 return image.address == write.address &&
                                                        image.bytes == write.bytes;
                                             }))
                        continue;
                    constant_writes.Add(write.address, write.bytes, ticket);
                }
                // Image downloads may occur at a later fence, after B's original draw
                // ticket has completed. Keep image aliases excluded for this activation.
                for (const auto& image : packet.constant_images)
                    constant_writes.Add(image.address, image.bytes, UINT64_MAX,
                                        FastRender::ConstantWriteGuard::KindImage);
                if (packet.constant_unknown) constant_writes.Unknown(ticket);
            }
            if (g_guard) {
                // FAST_CPU_GUARD: a render or storage target's footprint is no guest write (GPU
                // image writes stay in the image; downloads are DownloadLog's, checked on B).
                // Every other writer refuses until B has passed this packet.
                for (size_t i = 0; i < packet.constant_writers.size(); ++i)
                    if (i >= 64 || !((packet.image_writers >> i) & 1))
                        pending_writes.AddWrite(packet.constant_writers[i].address,
                                                packet.constant_writers[i].bytes, ticket);
                if (packet.constant_unknown) pending_writes.Unknown(ticket);
            }
        }
        if (ce_note) {
            u32 untracked = packet.ce_untracked;
            for (const auto& read : packet.ce_reads) {
                if (FastRender::GranuleTickets::Fits(read.address, read.bytes))
                    ce.reads.Note(read.address, read.bytes, ticket);
                else
                    untracked |= 32;
            }
            ce.notes += packet.ce_reads.size();
            if (untracked) {
                ce.untracked = ticket;
                ++ce.untracked_packets;
                for (u32 bit = 0; bit < ce.untracked_by.size(); ++bit)
                    ce.untracked_by[bit] += (untracked >> bit) & 1;
            }
        }
        if (DA::Enabled()) {
            DA::Get().Reads(ticket, packet.audit_reads, packet.audit_unknown,
                            [&](u64 t) { return IsDrawTaskPending(t); });
            for (auto writer : packet.audit_writes) {
                writer.ticket = ticket;
                DA::Get().Write(writer);
            }
        }
        return;
    }
    ConsumePacket(packet);
    if (worker_active)
        liverpool->register_deltas.MarkAll();
}

void Rasterizer::ConsumePacket(DrawPacket& packet) {
    FastRender::BProfile::Scope b_detail{FastRender::BProfile::Maintenance};
    ScratchPush::Invalidate();
    VR::DrawCameraScope camera_scope{&packet.camera};
    binding_packet = &packet;
    recording_vr = packet.vr;
    probe_selected = recording_vr.probe_selected;
    packet.Replay();
    if (FastRender::On(DynStateSkip))
        dyn_skip->Note(&packet, packet.delta_values);
    else
        dyn_skip->owner = nullptr;
    ScratchDraw::RegisterScope registers{packet.registers};
    ScratchDraw::Scope inputs{{packet.inputs.data(), packet.stage_count}};
    ScratchDraw::MemoryScope memory{packet.memory_views};
    if (FastRender::IsStageB()) {
        PerfTick();
        scheduler.PopPendingOperations();
    }
    // FAST_CPU_GUARD: one load unless an image download began after A copied the constants.
    if (packet.constant_page != ConstantRing::NoPage &&
        (packet.constant_verify || (packet.constant_downloads != DrawPacket::NoDownloads &&
                                    !VideoCore::g_download_log.Unchanged(packet.constant_downloads))))
        CheckGuardConstants(packet);
    switch (packet.kind) {
    case PacketKind::FilteredDraw:
        FilterDraw();
        break;
    case PacketKind::Draw:
        DrawRecord(static_cast<const GraphicsPipeline*>(packet.selected), packet.indexed,
                   packet.offset);
        break;
    case PacketKind::DrawIndirect:
        DrawIndirectRecord(static_cast<const GraphicsPipeline*>(packet.selected), packet.indexed,
                           packet.address, packet.offset, packet.size, packet.count,
                           packet.count_address);
        break;
    case PacketKind::Dispatch:
        DispatchDirectRecord(static_cast<const ComputePipeline*>(packet.selected));
        break;
    case PacketKind::DispatchIndirect:
        DispatchIndirectRecord(static_cast<const ComputePipeline*>(packet.selected), packet.address,
                               packet.offset, packet.size);
        break;
    }
    if (packet.constant_page != ConstantRing::NoPage)
        constant_ring->Used(packet.constant_page, scheduler.CurrentTick());
    if ((packet.sequence & 65535) == 0 && constant_bind_hits)
        LOG_INFO(Render_Vulkan, "FAST constants: packets={} direct_bindings={} native_pairs={}",
                 packet.sequence, constant_bind_hits, constant_pair_hits);
    binding_packet = nullptr;
}

// FAST_CPU_GUARD's view of B's progress for one packet's checks (PendingWriteGuard's
// Progress): each ticket is asked about at most once; a ticket at or below one seen passed has
// passed, one at or above one seen queued counts as queued (it may have passed since: refusing
// it is only conservative).
struct Rasterizer::GuardProgress {
    const Rasterizer& r;
    u64 passed{}, queued{~u64{0}};
    bool Pending(u64 ticket) {
        if (!ticket || ticket <= passed)
            return false;
        if (ticket >= queued)
            return true;
        if (r.IsDrawTaskPending(ticket)) {
            queued = ticket;
            return true;
        }
        passed = ticket;
        return false;
    }
    u64 SignalsEncoded() const {
        return r.scheduler.GuestSignalsEncoded();
    }
    bool SignalWritten(u64 sequence) const {
        return r.scheduler.GuestSignalsCompleted() >= sequence;
    }
};

void Rasterizer::PrepareConstants(DrawPacket& packet, bool enabled) {
    // No selection mutex is held here: allocation retirement and readback may drain B.
    FastRender::ProducerScope allow_readback{false};
    using OldGuard = FastRender::ConstantWriteGuard;
    using NewHit = FastRender::PendingWriteGuard::Hit;
    const bool guard_table = enabled && FastRender::On(FastRender::GuardTable);
    if (constant_mode != enabled || constant_writes.TableMode() != guard_table) {
        // A guard representation change restarts from a drained pipe, like a mode change:
        // queued writers are complete and deferred signals are flushed.
        DrainDrawPipe(DrawPipe::Toggle);
        scheduler.FlushGuestSignals("constant_toggle");
        constant_mode = enabled;
        constant_writes = OldGuard{guard_table};
        pending_writes = {};
        // FAST_CPU_GUARD: cached images write guest memory only through downloads (B checks).
        if (enabled && (!g_guard || g_guard_verify))
            for (const auto& [address, bytes] : texture_cache.ConstantImageExclusions())
                constant_writes.Add(address, bytes, UINT64_MAX, OldGuard::KindImage);
    }
    if (!enabled || packet.constants.empty() || packet.constant_unknown) return;
    if (!constant_ring) constant_ring = std::make_unique<ConstantRing>(instance, scheduler);
    const u64 alignment = std::max<u64>(instance.StorageMinAlignment(), instance.NonCoherentAtomSize());
    u64 maximum = 0;
    for (const auto& c : packet.constants) maximum += 3 * (c.bytes + alignment);
    if (!constant_ring->BeginPacket(maximum,
            [&](u64 ticket) { return IsDrawTaskPending(ticket); },
            [&] { DrainDrawPipe(DrawPipe::RasterizerEntry); })) return;
    ScratchDraw::MemoryScope owned_memory{packet.memory_views};
    const auto pending = [&](u64 t) { return IsDrawTaskPending(t); };
    GuardProgress progress{*this};
    auto& g = guard_stats;
    // The old guard's refusals by cause (GuardStats::old_cause): ticketed and queued, ticketed
    // and passed, permanent with a deferred signal, with an image footprint, anything else.
    const auto old_cause = [&](OldGuard::Hit hit, u64 address, u64 bytes) -> size_t {
        if (hit != OldGuard::Hit::Range)
            return 4;
        const auto cause = constant_writes.Describe(address, bytes, pending);
        if (!cause.permanent)
            return cause.stale ? 1 : 0;
        return (cause.kinds & OldGuard::KindSignal) ? 2 : (cause.kinds & OldGuard::KindImage) ? 3 : 4;
    };
    // FAST_CPU_GUARD: the image downloads begun before the copies below (B checks, and compares
    // with FAST_CPU_GUARD_VERIFY). Kept only when a guest constant is bound or probed.
    if (g_guard)
        packet.constant_downloads = VideoCore::g_download_log.Snapshot();
    packet.constant_verify = g_guard_verify;
    bool guest_constants = false;
    // FAST_CPU_GUARD matches queued writes by page (PendingWriteGuard::Match::Pages): what B's
    // own read would bind from the cache is refused, as IsRegionGpuModified below does for the
    // writes B has passed.
    constexpr auto pages = FastRender::PendingWriteGuard::Match::Pages;
    constant_candidates += packet.constants.size();
    for (auto& c : packet.constants) {
        bool probe = false;
        if (!c.flat) {
            const DA::Range source{c.sharp.base_address, c.bytes};
            if (std::any_of(packet.constant_writers.begin(), packet.constant_writers.end(),
                            [&](const auto& write) { return source.Overlaps(write); })) {
                ++constant_same_draw_refused;
                continue;
            }
            if (g_guard && !g_guard_verify) {
                const NewHit hit =
                    pending_writes.Check(source.address, source.bytes, progress, pages);
                if (hit != NewHit::None) {
                    ++constant_pending_refused;
                    ++g.refused_by[static_cast<size_t>(hit)];
                    continue;
                }
            } else {
                const OldGuard::Hit hit = constant_writes.Check(source.address, source.bytes, pending);
                // FAST_CPU_GUARD_VERIFY: the old guard decides; the new rule is asked as well.
                const NewHit fresh =
                    g_guard_verify ? pending_writes.Check(source.address, source.bytes, progress, pages)
                                   : NewHit::None;
                if (hit != OldGuard::Hit::None) {
                    ++constant_pending_refused;
                    ++constant_refused_by[static_cast<size_t>(hit)];
                    // FAST_CPU_GUARD=0 stays the code before the switch: no cause statistics
                    // (Describe searches the permanent list); the verify run has them.
                    if (!g_guard_verify)
                        continue;
                    const size_t cause = old_cause(hit, source.address, source.bytes);
                    ++g.old_cause[cause];
                    if (fresh != NewHit::None) {
                        ++g.both;
                        ++g.refused_by[static_cast<size_t>(fresh)];
                        continue;
                    }
                    ++g.freed;
                    ++g.freed_cause[cause];
                    if (buffer_cache.IsRegionGpuModified(source.address, source.bytes)) {
                        ++g.freed_gpu_dirty;
                        continue;
                    }
                    probe = true; // copied below for B to compare, not bound
                } else if (fresh == NewHit::Page) {
                    // Bytes on a page a queued write marks, which the old guard's byte ranges
                    // let through: B's own read would bind the cache there (B counts it).
                    ++g.stricter_page;
                    c.stricter = true;
                } else if (fresh != NewHit::None) {
                    // Every other refusal of the new rule should be one of the old guard's.
                    c.stricter = true;
                    if (++g.stricter <= 16)
                        LOG_ERROR(Render_Vulkan,
                                  "FAST_CPU_GUARD verify: the new rule refuses {:#x}+{} ({}), the "
                                  "old guard does not; the old answer is used, case #{}",
                                  source.address, source.bytes, static_cast<u32>(fresh), g.stricter);
                }
            }
            if (!probe && buffer_cache.IsRegionGpuModified(source.address, source.bytes)) {
                ++constant_gpu_refused;
                continue;
            }
            guest_constants = true;
        }
        const auto [target, offset] = constant_ring->Map(c.bytes, alignment);
        if (c.flat) {
            std::memcpy(target, packet.owned[c.input].flat.data(), c.bytes);
        } else {
            const auto* source = ScratchDraw::MemorySource(
                reinterpret_cast<const void*>(c.sharp.base_address), c.bytes);
            if (source == reinterpret_cast<const void*>(c.sharp.base_address))
                memory->CopySparseMemory(c.sharp.base_address, target, c.bytes);
            else std::memcpy(target, source, c.bytes);
        }
        constant_ring->Commit();
        packet.constant_page = constant_ring->Page();
        if (probe) {
            c.probe = true;
            c.left = {constant_ring->Handle(), offset, c.bytes};
            ++g.probes;
            continue;
        }
        ++constant_prepared;
        c.raw = {constant_ring->Handle(), offset, c.bytes};
        if (!c.flat && c.bytes == 864)
            c.camera = NativeStereo::CameraBuffer(*constant_ring, reinterpret_cast<const float*>(target),
                alignment, packet.camera, c.left, c.right, &c.projected,
                VrMirrorFlip() ? &c.mirrored : nullptr);
        else if (!c.flat && (c.bytes == 208 || c.bytes == 272 || c.bytes == 336 || c.bytes == 416))
            c.lighting = NativeStereo::LightingBuffer(*constant_ring, reinterpret_cast<const float*>(target),
                static_cast<u32>(c.bytes), alignment, packet.camera, c.left, c.right);
    }
    // Flat constants are A's own copies of the packet's user data: no download can change them,
    // so a packet with no guest constant needs no check on B.
    if (!guest_constants && !packet.constant_verify)
        packet.constant_downloads = DrawPacket::NoDownloads;
    if ((packet.sequence & 65535) == 0) {
        LOG_INFO(Render_Vulkan,
                 "FAST constant prepare: candidates={} prepared={} same_draw={} pending={} gpu_dirty={} "
                 "table_mode={} pending_unknown={} pending_invalid={} pending_range={} pending_table={} "
                 "pending_big={} guard_ranges={} guard_unknown_adds={}",
                 constant_candidates, constant_prepared, constant_same_draw_refused,
                 constant_pending_refused, constant_gpu_refused, constant_writes.TableMode(),
                 constant_refused_by[1], constant_refused_by[2], constant_refused_by[3],
                 constant_refused_by[4], constant_refused_by[5], constant_writes.RangeCount(),
                 constant_writes.UnknownAdds());
        const auto at = [&](NewHit hit) { return g.refused_by[static_cast<size_t>(hit)]; };
        LOG_INFO(Render_Vulkan,
                 "FAST_CPU_GUARD: {} new_unknown={} new_invalid={} new_write={} new_page={} "
                 "new_big={} new_signal_queued={} new_signal_unwritten={} new_entries={} "
                 "new_wrapping={} old_queued={} old_passed={} old_signal={} old_image={} "
                 "old_other={} both={} freed={} (queued={} passed={} signal={} image={} other={}) "
                 "freed_gpu_dirty={} probes={} stricter={} stricter_page={}",
                 g_guard ? (g_guard_verify ? "verify (the old guard decides)" : "on") : "off",
                 at(NewHit::Unknown), at(NewHit::Invalid), at(NewHit::Write), at(NewHit::Page),
                 at(NewHit::Big), at(NewHit::SignalQueued), at(NewHit::SignalUnwritten),
                 pending_writes.Count(), pending_writes.UnknownAdds(), g.old_cause[0],
                 g.old_cause[1], g.old_cause[2], g.old_cause[3], g.old_cause[4], g.both, g.freed,
                 g.freed_cause[0], g.freed_cause[1], g.freed_cause[2], g.freed_cause[3],
                 g.freed_cause[4], g.freed_gpu_dirty, g.probes, g.stricter, g.stricter_page);
    }
}

void Rasterizer::CheckGuardConstants(DrawPacket& packet) {
    auto& n = guard_checks;
    // FAST_CPU_GUARD: an image download began after A copied these constants, or was writing
    // then. The constants it may have written are bound the ordinary way: B reads the guest
    // bytes itself, as it did for every constant over an image before the switch. The log's
    // ranges come from guard_downloads (its lock once per download, not once per packet).
    const bool downloaded = packet.constant_downloads != DrawPacket::NoDownloads &&
                            !VideoCore::g_download_log.Unchanged(packet.constant_downloads);
    std::span<const VideoCore::DownloadLogReader::Range> written;
    const bool known =
        !downloaded ||
        guard_downloads.Since(VideoCore::g_download_log, packet.constant_downloads, written);
    const auto rewritten = [&](const DrawPacket::ConstantBinding& c) {
        const VAddr address = c.sharp.base_address;
        return downloaded &&
               (!known || std::any_of(written.begin(), written.end(), [&](const auto& w) {
                    return w.first < address + c.bytes && address < w.first + w.second;
                }));
    };
    if (downloaded && !packet.constant_verify) {
        // Copied and changed only when a bound constant meets a download.
        const auto meets = [&](const DrawPacket::ConstantBinding& c) {
            return !c.flat && c.raw.buffer && rewritten(c);
        };
        if constexpr (Common::Log::Enabled)
            ++n.download_packets;
        if (std::any_of(packet.constant_values.begin(), packet.constant_values.end(), meets)) {
            if (packet.constant_values.data() != packet.constants.data())
                packet.constants.assign(packet.constant_values.begin(),
                                        packet.constant_values.end());
            u64 dropped = 0;
            for (auto& c : packet.constants) {
                if (!meets(c))
                    continue;
                c.raw = vk::DescriptorBufferInfo{};
                c.camera = c.lighting = c.projected = c.mirrored = false;
                ++dropped;
            }
            packet.constant_values = packet.constants;
            n.dropped += dropped;
            if (++n.dropped_packets <= 16 || (n.dropped_packets & 1023) == 0)
                LOG_INFO(Render_Vulkan,
                         "FAST_CPU_GUARD: an image download after packet {}'s constants were "
                         "copied: {} of them bound from the guest bytes{}; packets={} "
                         "constants={} (of {} packets after a download)",
                         packet.sequence, dropped, known ? "" : " (all: the log lost track)",
                         n.dropped_packets, n.dropped, n.download_packets);
        }
    }
    if (!packet.constant_verify)
        return;
    // FAST_CPU_GUARD_VERIFY: each constant the new rule accepted against what B would read now
    // (ObtainBuffer's stream copy), and whether B would have read the guest bytes at all. In
    // three groups: accepted by the new rule only (probes, never bound), by both, and by the old
    // guard only (`stricter`: bound on its word; their cache bindings are its own gap). Not
    // compared: those a later download may have written (the switch rebinds them the ordinary
    // way). Compared but also counted apart (`direct`): those bit 33 could have bound in place
    // instead, from bytes uploaded into a cached buffer before any write through the backing (a
    // label, a download) that its tracker does not see; a run with FAST bit 33 off has none.
    thread_local std::vector<u8> live;
    for (const auto& c : packet.constant_values) {
        if (c.flat || (!c.raw.buffer && !c.probe))
            continue;
        const size_t group = c.probe ? 0 : c.stricter ? 2 : 1;
        if (rewritten(c)) {
            ++n.rebind[group];
            continue;
        }
        const VAddr address = c.sharp.base_address;
        live.resize(c.bytes);
        if (memory->IsValidMapping(address))
            memory->CopySparseMemory(address, live.data(), c.bytes);
        else
            std::memcpy(live.data(), reinterpret_cast<const void*>(address), c.bytes);
        const u64 offset = c.probe ? c.left.offset : c.raw.offset;
        const bool same =
            std::memcmp(live.data(), constant_ring->mapped_data.data() + offset, c.bytes) == 0;
        const bool cached = buffer_cache.IsRegionGpuModified(address, c.bytes);
        ++n.checked[group];
        n.differ[group] += !same;
        n.cached[group] += cached;
        n.direct[group] += !cached && buffer_cache.SmallDirectCandidate(address, c.bytes);
        if (group == 2 || (same && !cached))
            continue;
        const u64 mismatches = n.differ[0] + n.differ[1] + n.cached[0] + n.cached[1];
        if (mismatches <= 64 || (mismatches & 1023) == 0)
            LOG_ERROR(Render_Vulkan,
                      "FAST_CPU_GUARD verify: {} constant {:#x}+{} of packet {}:{}{}, mismatch #{}",
                      c.probe ? "newly accepted" : "accepted", address, c.bytes, packet.sequence,
                      same ? "" : " the guest bytes differ",
                      cached ? " B would bind the GPU-written cache" : "", mismatches);
    }
    if ((++n.packets & 65535) == 0)
        LOG_INFO(Render_Vulkan,
                 "FAST_CPU_GUARD verify: packets={} newly_accepted={} (differ={} cached={} "
                 "direct={} rebind={}) accepted_by_both={} (differ={} cached={} direct={} "
                 "rebind={}) old_only={} (differ={} cached={} direct={} rebind={})",
                 n.packets, n.checked[0], n.differ[0], n.cached[0], n.direct[0], n.rebind[0],
                 n.checked[1], n.differ[1], n.cached[1], n.direct[1], n.rebind[1], n.checked[2],
                 n.differ[2], n.cached[2], n.direct[2], n.rebind[2]);
}

void Rasterizer::GuardDeferredConstantWrite(VAddr address, u64 bytes) {
    if (!constant_mode)
        return;
    if (!g_guard || g_guard_verify)
        constant_writes.Add(address, bytes, UINT64_MAX, FastRender::ConstantWriteGuard::KindSignal);
    // FAST_CPU_GUARD: written once B has run the tasks queued so far (the fence's own is the
    // newest) and the recorder has submitted the work before it.
    if (g_guard) {
        auto* pipe = published_pipe.load(std::memory_order_acquire);
        pending_writes.AddSignal(address, bytes, pipe ? pipe->Published() : 0);
    }
}
void Rasterizer::GuardConstantWrite(VAddr address, u64 bytes, u64 ticket) {
    if (!constant_mode || !ticket)
        return;
    if (!g_guard || g_guard_verify)
        constant_writes.Add(address, bytes, ticket);
    if (g_guard)
        pending_writes.AddWrite(address, bytes, ticket);
}

struct Rasterizer::BeginMemo {
    static constexpr size_t KeySize =
        sizeof(decltype(AmdGpu::Regs::color_buffers)) +
        sizeof(decltype(AmdGpu::Regs::depth_buffer)) + sizeof(decltype(AmdGpu::Regs::depth_view)) +
        sizeof(decltype(AmdGpu::Regs::depth_control)) +
        sizeof(decltype(AmdGpu::Regs::depth_render_control)) +
        sizeof(decltype(AmdGpu::Regs::depth_htile_data_base)) +
        sizeof(decltype(AmdGpu::Regs::depth_clear)) +
        sizeof(decltype(AmdGpu::Regs::stencil_clear)) +
        sizeof(decltype(GraphicsPipelineKey::color_samples)) + 2 * sizeof(u32);
    using Key = PerfMemo::KeyBytes<KeySize>;
    struct Target {
        VideoCore::ImageId id{};
        VideoCore::ImageViewInfo view{};
        vk::Image backing{};
        vk::ImageLayout layout{};
        bool operator==(const Target&) const = default;
    };
    bool valid{};
    u64 epoch{}, tick{}, hits{}, misses{}, checked{};
    Key key;
    std::array<Target, AmdGpu::NUM_COLOR_BUFFERS + 1> targets{};
    RenderState state{};
    static Key MakeKey(const AmdGpu::Regs& regs, const GraphicsPipelineKey& pipeline) {
        Key result;
        result.Add(regs.color_buffers);
        result.Add(regs.depth_buffer);
        result.Add(regs.depth_view);
        result.Add(regs.depth_control);
        result.Add(regs.depth_render_control);
        result.Add(regs.depth_htile_data_base);
        result.Add(regs.depth_clear);
        result.Add(regs.stencil_clear);
        result.Add(pipeline.color_samples).Add(pipeline.mrt_mask).Add(pipeline.probe_view_mask);
        ASSERT(result.Complete());
        return result;
    }
};

namespace {
// FAST_CPU_PASS_MEMO (S9; on unless '0', read once at start-up, a plain load per draw): with the
// begin-state memo on (bit 2), BeginRendering takes Rasterizer::PassMemo instead of BeginMemo;
// TargetViews::Note skips a repeat of its last entry; the draw reads the packet's camera record in
// place instead of copying it. (The targets are read with GetImage: with FAST_CPU_LRU_TICK a
// repeated one returns after one compare, which a peek would only save.)
// Rendering and guest memory are unchanged: every skip leaves out only work that would have
// changed nothing (see each).
const bool g_pass_memo = [] {
    const char* value = std::getenv("FAST_CPU_PASS_MEMO");
    return !value || value[0] != '0';
}();
// FAST_CPU_PASS_MEMO_VERIFY=1 (with the memo on): a pass-memo hit runs BeginRenderingFull as
// well and takes its state, comparing the state, the registry and meta epochs and each
// attachment's state against the hit; the meta surfaces are looked up on every probe;
// TargetViews::Note loads and stores as without the memo and checks a skippable repeat.
// Mismatches are logged (LOG_ERROR, the first 16, then every 1024th) and counted in the
// FAST_CPU_PASS_MEMO line.
const bool g_pass_memo_verify = [] {
    const char* value = std::getenv("FAST_CPU_PASS_MEMO_VERIFY");
    return g_pass_memo && value && value[0] == '1';
}();
const PipelineSkip::TargetViews::NoteMemo g_note_memo =
    !g_pass_memo          ? PipelineSkip::TargetViews::NoteMemo::Off
    : g_pass_memo_verify ? PipelineSkip::TargetViews::NoteMemo::Verify
                         : PipelineSkip::TargetViews::NoteMemo::Memo;
} // namespace

// FAST_CPU_PASS_MEMO (S9 of the CPU parity roadmap; bbport RenderStateMemo, BB vk_rasterizer.cpp:
// 3679-3739 with MakeBeginSignature): the begin-state memo (BeginMemo, FAST bit 2) keyed by what
// BeginRenderingFull reads rather than by the register blocks it comes from. Per attachment that
// is its id and view (cb_descs / db_desc), the backing and layout the memo was stored over (the
// pre-call ones, as BeginMemo::Target), the meta surface register IsMetaCleared and TouchMeta
// read, and the description's addresses FindRenderTarget / FindDepthTarget register (CMASK and
// FMASK; HTILE and the stencil image); beyond them only the pipeline's mrt_mask, colour samples
// and view mask, the depth control, the depth buffer's valid bits and perf_depth_one_layout. The
// clear values are read only for a clear, which a stored state never has. BeginMemo gathered and
// compared 552 register bytes (all eight colour buffers) and nine targets every draw; a probe
// compares this pipeline's slots in place and stops at the first difference. A slot's meta
// surface is known to have no cleared slice while the texture cache's MetaClearEpoch is the one
// the memo saw them clear at (no robin_map lookups); after it moved they are looked up as
// BeginMemo did. The hit conditions are BeginMemo's otherwise (eligibility, registry epoch,
// command buffer tick, the open pass continuing the state).
struct Rasterizer::PassMemo {
    // The first check a probe failed (the FAST_CPU_PASS_MEMO line), Hit when none did.
    enum Miss : u32 { Hit, Invalid, Tick, Epoch, Clears, Key, Depth, Target, Meta, Pass, Count };
    static constexpr u32 DepthSlot = AmdGpu::NUM_COLOR_BUFFERS;
    struct Slot {
        VideoCore::ImageId id{};
        VideoCore::ImageViewInfo view{};
        vk::Image backing{};
        vk::ImageLayout layout{};
        VAddr meta{};                // CMASK / HTILE register address (IsMetaCleared, TouchMeta)
        std::array<VAddr, 2> desc{}; // colour: cmask, fmask; depth: htile, stencil address
        u32 stencil_size{};          // depth only
    };
    bool valid{};
    u32 mrt_mask{}, probe_view_mask{}, depth_control{}, depth_flags{};
    std::array<u8, AmdGpu::NUM_COLOR_BUFFERS> color_samples{};
    u64 epoch{}, tick{}, meta_epoch{};
    std::array<Slot, AmdGpu::NUM_COLOR_BUFFERS + 1> slots{}; // colour 0..7, depth 8
    RenderState state{};
    // Statistics (recording thread): probes by outcome, hits after a meta lookup, verify counts.
    std::array<u64, Count> probes{};
    u64 calls{}, looked_up{}, verified{}, mismatches{}, note_mismatches{};

    // The image state BeginRenderingFull would change or read beyond the slot (BeginMemo's
    // eligibility): not bound as a texture, no rebind, clean, one layout for all subresources.
    static bool Plain(const VideoCore::Image& image) {
        return !image.binding.is_bound && !image.binding.needs_rebind &&
               False(image.flags & VideoCore::ImageFlagBits::Dirty) &&
               image.backing->subresource_states.empty();
    }
    // What the depth attachment's state reads of the depth buffer and the perf switch.
    static u32 DepthFlags(const AmdGpu::Regs& regs) {
        return u32(regs.depth_buffer.DepthValid()) | u32(regs.depth_buffer.StencilValid()) << 1 |
               u32(Perf::DepthOneLayout()) << 2;
    }
    static u32 DepthControlBits(const AmdGpu::Regs& regs) {
        static_assert(sizeof(regs.depth_control) == sizeof(u32));
        return std::bit_cast<u32>(regs.depth_control);
    }
    bool Mismatch() {
        ++mismatches;
        return mismatches <= 16 || (mismatches & 1023) == 0;
    }
    void Report() const {
        LOG_INFO(Render_Vulkan,
                 "FAST_CPU_PASS_MEMO: hits={} misses: invalid={} tick={} epoch={} clears={} "
                 "key={} depth={} target={} meta={} pass={}; hits after a meta lookup={} "
                 "verified={} mismatches={} note mismatches={}",
                 probes[Hit], probes[Invalid], probes[Tick], probes[Epoch], probes[Clears],
                 probes[Key], probes[Depth], probes[Target], probes[Meta], probes[Pass],
                 looked_up, verified, mismatches, note_mismatches);
    }
};

void Rasterizer::NoteTargetViews(u64 targets, u32 desired) {
    if (pipeline_cache.NoteTargetViews(targets, desired, g_note_memo)) {
        return;
    }
    // FAST_CPU_PASS_MEMO_VERIFY: the slot of a repeated entry did not hold it.
    if (!pass_memo)
        pass_memo = std::make_unique<PassMemo>();
    auto& memo = *pass_memo;
    ++memo.note_mismatches;
    if (memo.Mismatch()) {
        LOG_ERROR(Render_Vulkan,
                  "FAST_CPU_PASS_MEMO verify: target views key {:#x} (view mask {}) noted again "
                  "but its slot did not hold it; stored, mismatch #{}",
                  targets, desired, memo.mismatches);
    }
}

static Shader::PushData MakeUserData(const AmdGpu::Regs& regs) {
    // TODO(roamic): Add support for multiple viewports and geometry shaders when ViewportIndex
    // is encountered and implemented in the recompiler.
    Shader::PushData push_data{};
    push_data.xoffset = regs.viewport_control.xoffset_enable ? regs.viewports[0].xoffset : 0.f;
    push_data.xscale = regs.viewport_control.xscale_enable ? regs.viewports[0].xscale : 1.f;
    push_data.yoffset = regs.viewport_control.yoffset_enable ? regs.viewports[0].yoffset : 0.f;
    push_data.yscale = regs.viewport_control.yscale_enable ? regs.viewports[0].yscale : 1.f;
    return push_data;
}

#include "../../../scratchpad/dual-eye-perf/native_audit.inl"

Rasterizer::Rasterizer(const Instance& instance_, Scheduler& scheduler_,
                       AmdGpu::Liverpool* liverpool_)
    : instance{instance_}, scheduler{scheduler_}, page_manager{this},
      buffer_cache{instance, scheduler, liverpool_, texture_cache, page_manager},
      texture_cache{instance, scheduler, liverpool_, buffer_cache, page_manager},
      liverpool{liverpool_}, memory{Core::Memory::Instance()},
      pipeline_cache{instance, scheduler, liverpool},
      bind_memo{[] {
                    const char* value = std::getenv("VR_LARGE_BIND_MEMO");
                    return !value || value[0] != '0';
                }(),
                [] {
                    const char* value = std::getenv("VR_EXTRA_LARGE_BIND_MEMO");
                    return !value || value[0] != '0';
                }()} {
    FastRender::Start();
    GpuPassProfiler::Init(instance, scheduler); // FAST_GPU_PASS_PROFILE only
    // Bit 40's skip rule in native stereo asks the VR state (Stage A, vk_rasterizer_vr_draws.cpp).
    pipeline_cache.SetVrSkipVeto([this](const Shader::Info* vs) { return VrSkipVeto(vs); });
    dyn_skip = std::make_unique<DynSkip>();
    const char* temporal_check = std::getenv("VR_TEMPORAL_NATIVE_CHECK");
    // Accepted settings drive the normal path. Native-size AA is a diagnostic override.
    vr_temporal_enabled = true;
    const char* scaled_check = std::getenv("VR_TEMPORAL_SCALED_CHECK");
    vr_temporal_scaled =
        !(temporal_check && temporal_check[0] == '1') || (scaled_check && scaled_check[0] == '1');
    if (!EmulatorSettings.IsNullGPU()) {
        liverpool->BindRasterizer(this);
    }
    memory->SetRasterizer(this);
}

Rasterizer::~Rasterizer() {
    published_pipe.store(nullptr, std::memory_order_release);
    draw_pipe.reset(); // Drain and join before destroying caches, packets or scheduler users.
    GpuPassProfiler::Shutdown(scheduler); // FAST_GPU_PASS_PROFILE only: finishes, frees its pool
    if (constant_ring) {
        scheduler.Finish();
        constant_ring.reset();
    }
}

bool Rasterizer::DrainDrawPipe(u32 reason, u32 opcode) {
    if (auto* pipe = published_pipe.load(std::memory_order_acquire))
        return pipe->Drain(reason, opcode);
    return false;
}

bool Rasterizer::IsDrawTaskPending(u64 ticket) const {
    auto* pipe = published_pipe.load(std::memory_order_acquire);
    return pipe && pipe->Pending(ticket);
}

u64 Rasterizer::QueueOrderedTask(void (*handler)(void*, std::span<const std::byte>), void* context,
                                 std::span<const std::byte> packet, bool writes_known) {
    if (!worker_active || !FastRender::On(FastRender::OrderedFenceTasks))
        return false;
    const auto ticket = draw_pipe->PushTask(handler, context, packet);
    if (constant_mode && !writes_known) {
        constant_writes.Unknown(ticket);
        pending_writes.Unknown(ticket);
    }
    // Ordered tasks (DMA, WriteData, submit/finish) may read guest memory they do not list.
    if (ce.mode) ce.untracked = ticket;
    return ticket;
}

bool Rasterizer::CeDumpWait(VAddr address, u64 bytes) {
    static constexpr auto CeTargetedWait = static_cast<FastRender::Bit>(16);
    ++ce.dumps;
    const bool on = worker_active && draw_pipe && constant_mode && bytes && !ce.overflow &&
                    FastRender::On(CeTargetedWait) && !FastRender::On(FastRender::CopyThreads) &&
                    EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Disabled;
    bool targeted = false;
    if (!on) {
        ce.mode = false;
        ++ce.full_off;
    } else {
        const bool covered = ce.hi > ce.lo && address >= ce.lo && address + bytes <= ce.hi;
        if (!covered) {
            // Growth happens only here, followed by the caller's full drain: every packet
            // still queued afterwards is collected against the grown box.
            ce.lo = ce.hi > ce.lo ? std::min(ce.lo, address) : address;
            ce.hi = std::max(ce.hi, address + bytes);
            ++ce.full_box;
            if (ce.hi - ce.lo > kCeBoxLimit) {
                ce.overflow = true;
                ce.mode = false;
                LOG_WARNING(Render_Vulkan, "FAST ce: dump box {:#x}..{:#x} exceeds {} bytes; full drains",
                            ce.lo, ce.hi, kCeBoxLimit);
            }
        }
        if (ce.overflow) {
        } else if (!ce.mode) {
            ce.mode = true; // the caller's drain retires every packet queued before notes began
            ++ce.full_mode;
        } else if (covered) {
            // FAST_CPU_GUARD: the writes still queued (or signals not yet written) only. Writes
            // by page, as for constants: a queued GPU write marks its pages when B records it,
            // and the dump's CPU write must come after that, as the full drain orders it, even
            // where the write misses the dumped bytes. Without FAST_CPU_GUARD_CE, or with
            // readbackLinearImages (see g_guard_ce), every covered dump drains in full.
            GuardProgress progress{*this};
            const bool new_full =
                g_guard && (!g_guard_ce || texture_cache.ReadsBackLinearImages() ||
                            pending_writes.Intersects(address, bytes, progress,
                                                      FastRender::PendingWriteGuard::Match::Pages));
            const bool full =
                g_guard && !g_guard_verify
                    ? new_full
                    : constant_writes.Intersects(address, bytes,
                                                 [&](u64 t) { return IsDrawTaskPending(t); });
            if (g_guard_ce_verify && !new_full) {
                ++ce.full_guard; // drained in full all the same, after the check
                CeVerifyTargeted(address, bytes);
            } else if (full) {
                ++ce.full_guard; // a queued GPU write or deferred signal targets these bytes
            } else {
                targeted = true;
                const u64 newest = ce.reads.Newest(address, bytes);
                const u64 target = std::max(ce.untracked, newest);
                if (!draw_pipe->Pending(target)) {
                    ++ce.skipped;
                } else {
                    ++ce.waited;
                    ce.untracked_waits += ce.untracked > newest;
                    ce.wait_ns += draw_pipe->WaitFor(target);
                }
            }
        }
    }
    if ((ce.dumps & 16383) == 0)
        LOG_INFO(Render_Vulkan,
                 "FAST ce: dumps={} skipped={} waited={} wait_us={} untracked_waits={} full_off={} "
                 "full_mode={} full_box={} full_guard={} notes={} untracked_packets={} "
                 "untracked_nopipe={} dma={} meta={} imgfmt={} indirect={} huge={} box={:#x}..{:#x}",
                 ce.dumps, ce.skipped, ce.waited, ce.wait_ns / 1000, ce.untracked_waits, ce.full_off,
                 ce.full_mode, ce.full_box, ce.full_guard, ce.notes, ce.untracked_packets,
                 ce.untracked_by[0], ce.untracked_by[1], ce.untracked_by[2], ce.untracked_by[3],
                 ce.untracked_by[4], ce.untracked_by[5], ce.lo, ce.hi);
    return targeted;
}

// FAST_CPU_GUARD_CE_VERIFY. The targeted path would write the dump right after waiting for the
// newest queued packet that reads or writes these bytes (ce.reads) or reads unlisted memory
// (ce.untracked). That wait comes first here; then the dumped bytes and their pages' GPU-modified
// state are taken, and the full drain and guest signal flush the caller would otherwise skip run
// (the caller's own then find nothing to do). Whatever changed in between is what the targeted
// dump would have preceded although it was queued before it: guest bytes written by a queued
// task or signal (WriteData, a query or DMA write, a label, a download in OnSubmit), and pages a
// queued GPU write marked (B would record it after the dump, so the dump's CPU write would no
// longer come after it). With FAST_DRAIN_AUDIT=1 the queued packets' own read lists are asked as
// well (the targeted dump would change what they read). Each is a mismatch (LOG_ERROR).
void Rasterizer::CeVerifyTargeted(VAddr address, u64 bytes) {
    const u64 newest = ce.reads.Newest(address, bytes);
    const u64 target = std::max(ce.untracked, newest);
    if (draw_pipe->Pending(target))
        draw_pipe->WaitFor(target);
    const auto pending = [&](u64 t) { return IsDrawTaskPending(t); };
    const DA::Overlap readers =
        DA::Enabled() ? DA::Get().Probe({address, bytes}, pending) : DA::Overlap{};
    thread_local std::vector<u8> before;
    before.resize(bytes);
    std::memcpy(before.data(), reinterpret_cast<const void*>(address), bytes);
    const bool dirty_before = buffer_cache.IsRegionGpuModified(address, bytes);
    DrainDrawPipe(DrawPipe::ConstRam); // the caller's CE drain (liverpool.cpp, reason 4)
    scheduler.FlushGuestSignals("ce_dump_order");
    const bool bytes_changed =
        std::memcmp(before.data(), reinterpret_cast<const void*>(address), bytes) != 0;
    const bool pages_marked = !dirty_before && buffer_cache.IsRegionGpuModified(address, bytes);
    ++ce.verified;
    ce.bytes_changed += bytes_changed;
    ce.pages_marked += pages_marked;
    ce.readers += readers.matching != 0;
    ce.unknown_readers += readers.unknown != 0;
    if (bytes_changed || pages_marked || readers.matching) {
        if (++ce.mismatches <= 32 || (ce.mismatches & 1023) == 0)
            LOG_ERROR(Render_Vulkan,
                      "FAST_CPU_GUARD_CE verify: a targeted wait (for {}) before the dump to "
                      "{:#x}+{} would have left{}{}{}; the full drain is used, mismatch #{}",
                      target, address, bytes,
                      bytes_changed ? " a queued write of the guest bytes" : "",
                      pages_marked ? " a queued GPU write marking their pages" : "",
                      readers.matching ? " queued readers of them" : "", ce.mismatches);
    }
    if ((ce.verified & 4095) == 0)
        LOG_INFO(Render_Vulkan,
                 "FAST_CPU_GUARD_CE verify: would_target={} bytes_changed={} pages_marked={} "
                 "readers={} unknown_readers={} mismatches={} (readers{})",
                 ce.verified, ce.bytes_changed, ce.pages_marked, ce.readers, ce.unknown_readers,
                 ce.mismatches, DA::Enabled() ? "" : ": FAST_DRAIN_AUDIT=1 to check them");
}

bool Rasterizer::UpdateDrawWorker() {
    // Capture/debug paths can call Vulkan directly outside the packet recorder.
    static const bool diagnostic_environment =
        std::getenv("VR_FOREGROUND_PROBE") || std::getenv("VR_NATIVE_AUDIT_DIR") ||
        std::getenv("VR_DRAW_REGISTER_SNAPSHOTS") || std::getenv("VR_DRAW_INPUT_SNAPSHOTS");
    // The stage pipe transfers producer ownership; the recorder independently
    // owns Vulkan replay. CPU visibility boundaries drain B before flushing R's
    // deferred guest signals, rather than equating B completion with submission.
    const bool requested =
        FastRender::On(FastRender::RecordingThread) && !diagnostic_environment &&
        !EmulatorSettings.IsVkHostMarkersEnabled() && !EmulatorSettings.IsVkGuestMarkersEnabled() &&
        !EmulatorSettings.IsRenderdocEnabled() && !DebugState.DumpingCurrentReg();
    if (requested == worker_active)
        return requested;
    DrainDrawPipe(DrawPipe::Toggle);
    scheduler.Flush();
    scheduler.FlushRecorder("pipeline_toggle");
    // Serial loopback and B have separate replay files: every ownership change starts full.
    liverpool->register_deltas.MarkAll();
    if (requested && !draw_pipe) {
        recording_packet = std::make_unique<DrawPacket>(*liverpool);
        // FAST_CPU_SLIM_PACKET: the new decoder holds no camera record yet. _VERIFY's decoder of
        // the slim form is made here, on A, like this one (the constructor marks A's deltas).
        if (draw_packet)
            draw_packet->held_camera.Forget();
        if (g_slim_packet_verify)
            recording_packet->slim_check = std::make_unique<DrawPacket>(*liverpool);
        draw_pipe = std::make_unique<DrawPipe>(
            [](void* context, std::span<const std::byte> bytes) {
                auto& self = *static_cast<Rasterizer*>(context);
                auto& packet = *self.recording_packet;
                if (!g_slim_packet) {
                    packet.Decode(bytes);
                    self.ConsumePacket(packet);
                } else if (!g_slim_packet_verify) {
                    const size_t used = packet.DecodeSlim(bytes);
                    ASSERT(used == bytes.size());
                    self.ConsumePacket(packet);
                } else {
                    packet.DecodeSlimVerify(bytes);
                    self.ConsumePacket(packet);
                    packet.SlimVerifyReads();
                }
            },
            this,
            // FAST_CPU_TASK_PUBLISH=2: the recorder gets B's open batch before an ordered task.
            g_task_publish == 2 ? +[](void* context) {
                static_cast<Rasterizer*>(context)->scheduler.PublishRecorder();
            } : nullptr);
        published_pipe.store(draw_pipe.get(), std::memory_order_release);
    }
    constant_mode = false;
    constant_writes = {};
    pending_writes = {};
    worker_active = requested;
    LOG_INFO(Render_Vulkan, "FAST recording worker {} (owned packet pipeline)", requested);
    return requested;
}

void Rasterizer::CpSync() {
    DrainDrawPipe(DrawPipe::RasterizerEntry);
    scheduler.EndRendering();
    auto cmdbuf = scheduler.Commands();

    const vk::MemoryBarrier ib_barrier{
        .srcAccessMask = vk::AccessFlagBits::eShaderWrite,
        .dstAccessMask = vk::AccessFlagBits::eIndirectCommandRead,
    };
    cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eComputeShader,
                           vk::PipelineStageFlagBits::eDrawIndirect,
                           vk::DependencyFlagBits::eByRegion, ib_barrier, {}, {});
}

namespace {
// FAST_CPU_LEAN_SCOPES: out of line, called only when FAST_PIPE_VERIFY is set (or the switch is
// '0'), so AssertRecordingOwner is a load and a branch.
SHAD_NO_INLINE void CheckRecordingOwner(bool worker_active) {
    ASSERT_MSG(!FastRender::g_pipe_verify || !worker_active || FastRender::IsStageB(),
               "Recording state accessed by stage A");
}
} // namespace

void Rasterizer::AssertRecordingOwner() const {
    if (FastRender::g_pipe_check_call) [[unlikely]] {
        CheckRecordingOwner(worker_active);
    }
}

bool Rasterizer::IsRegularDraw() const {
    const auto& regs = ScratchDraw::Registers(liverpool);
    using Mode = AmdGpu::ColorControl::OperationMode;
    if (regs.color_control.mode == Mode::EliminateFastClear ||
        regs.color_control.mode == Mode::FmaskDecompress ||
        regs.color_control.mode == Mode::Resolve ||
        regs.primitive_type == AmdGpu::PrimitiveType::None)
        return false;
    const bool depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const bool stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    return !(regs.color_control.mode == Mode::Disable && (depth_copy || stencil_copy));
}

bool Rasterizer::FilterDraw() {
    FastRender::BProfile::Scope b_detail{FastRender::BProfile::Special};
    AssertRecordingOwner();
    const auto& regs = ScratchDraw::Registers(liverpool);
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::EliminateFastClear) {
        // Clears the render target if FCE is launched before any draws
        EliminateFastClear();
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::FmaskDecompress) {
        // TODO: check for a valid MRT1 to promote the draw to the resolve pass.
        LOG_TRACE(Render_Vulkan, "FMask decompression pass skipped");
        ScopedMarkerInsert("FmaskDecompress");
        return false;
    }
    if (regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Resolve) {
        LOG_TRACE(Render_Vulkan, "Resolve pass");
        Resolve();
        return false;
    }
    if (regs.primitive_type == AmdGpu::PrimitiveType::None) {
        LOG_TRACE(Render_Vulkan, "Primitive type 'None' skipped");
        ScopedMarkerInsert("PrimitiveTypeNone");
        return false;
    }

    const bool cb_disabled =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const auto depth_copy =
        regs.depth_render_override.force_z_dirty && regs.depth_render_override.force_z_valid &&
        regs.depth_buffer.DepthValid() && regs.depth_buffer.DepthWriteValid() &&
        regs.depth_buffer.DepthAddress() != regs.depth_buffer.DepthWriteAddress();
    const auto stencil_copy =
        regs.depth_render_override.force_stencil_dirty &&
        regs.depth_render_override.force_stencil_valid && regs.depth_buffer.StencilValid() &&
        regs.depth_buffer.StencilWriteValid() &&
        regs.depth_buffer.StencilAddress() != regs.depth_buffer.StencilWriteAddress();
    if (cb_disabled && (depth_copy || stencil_copy)) {
        // Games may disable color buffer and enable force depth/stencil dirty and valid to
        // do a copy from one depth-stencil surface to another, without a pixel shader.
        // We need to detect this case and perform the copy, otherwise it will have no effect.
        LOG_TRACE(Render_Vulkan, "Performing depth-stencil override copy");
        DepthStencilCopy(depth_copy, stencil_copy);
        return false;
    }

    return true;
}

// FAST bit 27: render-target lookup memo (bbport FindTargetMemoized, vk_rasterizer.cpp:591-644).
static constexpr auto TargetLookupMemo = static_cast<FastRender::Bit>(27);

// FAST_TARGET_MEMO_VERIFY=N: every Nth hit takes the full path and compares. Read once at start-up
// (a plain load per target lookup, no function-local static's guard).
static const u64 g_target_memo_verify_every = [] {
    const char* value = std::getenv("FAST_TARGET_MEMO_VERIFY");
    return value ? std::strtoull(value, nullptr, 0) : 0ULL;
}();
static u64 TargetMemoVerifyEvery() {
    return g_target_memo_verify_every;
}

// The fields of a target description FindImage and the attachments depend on (verify only).
static bool SameTargetDesc(const VideoCore::TextureCache::ImageDesc& a,
                           const VideoCore::TextureCache::ImageDesc& b) {
    const auto& x = a.info;
    const auto& y = b.info;
    return x.guest_address == y.guest_address && x.guest_size == y.guest_size &&
           x.size == y.size && x.pixel_format == y.pixel_format && x.type == y.type &&
           x.resources == y.resources && x.num_samples == y.num_samples && x.pitch == y.pitch &&
           x.tile_mode == y.tile_mode && x.meta_info.cmask_addr == y.meta_info.cmask_addr &&
           x.meta_info.fmask_addr == y.meta_info.fmask_addr &&
           x.meta_info.htile_addr == y.meta_info.htile_addr && x.stencil_addr == y.stencil_addr &&
           a.view_info.range == b.view_info.range && a.view_info.format == b.view_info.format &&
           a.view_info.type == b.view_info.type &&
           a.view_info.is_storage == b.view_info.is_storage && a.type == b.type;
}

template <size_t N, typename MakeDesc>
VideoCore::ImageId Rasterizer::FindTargetMemoized(TargetMemo<N>& memo,
                                                  const PerfMemo::KeyBytes<N>& key,
                                                  VideoCore::TextureCache::ImageDesc& desc,
                                                  MakeDesc&& make_desc) {
    auto& stats = target_memo_stats;
    const u64 epoch = texture_cache.RegistryEpoch();
    const bool known = memo.epoch == epoch && key.Complete() &&
                       std::memcmp(memo.key.data(), key.bytes.data(), N) == 0;
    const u64 verify_every = TargetMemoVerifyEvery();
    if (known && (verify_every == 0 || ++stats.verify_tick % verify_every != 0)) {
        // No image was registered or unregistered since this perfect match: FindImage would give
        // it again and leave `desc` (still this key's) alone. What FindImage does besides the
        // lookup: the access tick and the LRU place (bbport MarkFound, texture_cache.h:103-105).
        ++stats.hits;
        texture_cache.GetImage(memo.image_id).tick_accessed_last = scheduler.CurrentTick();
        return memo.image_id;
    }
    if (known) {
        // FAST_TARGET_MEMO_VERIFY: the full path, against what the memo would have given.
        const VideoCore::TextureCache::ImageDesc remembered = desc;
        make_desc();
        const VideoCore::ImageId image_id = texture_cache.FindImage(desc);
        ++stats.verified;
        if (image_id != memo.image_id || !SameTargetDesc(remembered, desc)) {
            memo.epoch = 0;
            if (++stats.mismatches <= 20) {
                LOG_ERROR(Render_Vulkan,
                          "FAST target memo verify MISMATCH #{}: remembered image {} {:#x} "
                          "{}x{} fmt {} range {}/{}, full path image {} {:#x} {}x{} fmt {} "
                          "range {}/{}",
                          stats.mismatches, memo.image_id.index, remembered.info.guest_address,
                          remembered.info.size.width, remembered.info.size.height,
                          vk::to_string(remembered.info.pixel_format),
                          remembered.view_info.range.base.level,
                          remembered.view_info.range.base.layer, image_id.index,
                          desc.info.guest_address, desc.info.size.width, desc.info.size.height,
                          vk::to_string(desc.info.pixel_format), desc.view_info.range.base.level,
                          desc.view_info.range.base.layer);
            }
        }
        return image_id;
    }
    ++stats.misses;
    make_desc();
    const VideoCore::ImageId image_id = texture_cache.FindImage(desc);
    memo.epoch = 0; // `desc` was rebuilt
    if (key.Complete() && epoch == texture_cache.RegistryEpoch()) {
        // FindImage's perfect match (texture_cache.cpp FindImage): with the registry unchanged,
        // an image matching like this was in the region walked, so no overlap was resolved and
        // the same request in this epoch gives the same image.
        const auto& found = texture_cache.GetImage(image_id).info;
        const auto& wanted = desc.info;
        if (found.guest_address == wanted.guest_address &&
            found.guest_size == wanted.guest_size && found.size == wanted.size &&
            (found.type == wanted.type || wanted.size == VideoCore::Extent3D{1, 1, 1}) &&
            VideoCore::IsVulkanFormatCompatible(found.pixel_format, wanted.pixel_format)) {
            memo.key = key.bytes;
            memo.epoch = epoch;
            memo.image_id = image_id;
            ++stats.stores;
        }
    }
    return image_id;
}

void Rasterizer::PrepareRenderState(const GraphicsPipeline* pipeline) {
    FastRender::BProfile::Scope b_profile{FastRender::BProfile::RenderTargets};
    AssertRecordingOwner();
    ScratchProfile::Cpu scratch_cpu(11, "Rasterizer::PrepareRenderState");
    // Prefetch render targets to handle overlaps with bound textures (e.g. mipgen)
    const auto& key = pipeline->GetGraphicsKey();
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    if (regs.color_control.degamma_enable) {
        LOG_WARNING(Render_Vulkan, "Color buffers require gamma correction");
    }

    const bool skip_cb_binding =
        regs.color_control.mode == AmdGpu::ColorControl::OperationMode::Disable;
    const bool rt_memo = PerfMemo::g_switches.Get().rt_memo;
    const bool check = PerfMemo::CheckMode();
    const u32 memo_epoch = PerfMemo::Epoch();
    const bool target_memo = FastRender::On(TargetLookupMemo);
    if (target_memo != target_memo_armed) {
        // While the bit is off the slots' descriptions are rebuilt without the memo.
        target_memo_armed = target_memo;
        for (auto& memo : cb_target_memo) {
            memo.epoch = 0;
        }
        db_target_memo.epoch = 0;
    }
    if (target_memo && (++target_memo_stats.draws & ((u64{1} << 19) - 1)) == 0) {
        const auto& s = target_memo_stats;
        LOG_INFO(Render_Vulkan,
                 "FAST target memo: hits={} misses={} stores={} verified={} mismatches={} "
                 "draws={}",
                 s.hits, s.misses, s.stores, s.verified, s.mismatches, s.draws);
    }
    for (s32 cb = 0; cb < std::bit_width(key.mrt_mask); ++cb) {
        auto& image_id = cb_descs[cb].first;
        auto& desc = cb_descs[cb].second;
        const auto& col_buf = regs.color_buffers[cb];
        const u32 target_mask = regs.color_target_mask.GetMask(cb);
        if (skip_cb_binding || !col_buf || !target_mask || (key.mrt_mask & (1 << cb)) == 0) {
            image_id = {};
            continue;
        }
        const auto& hint = std::as_const(ScratchDraw::ColorExtents(liverpool))[cb];
        const auto build_desc = [&] {
            if (rt_memo) {
                // perf_rt_memo: ImageDesc(col_buf, hint) is a pure function of these bytes. The
                // copy is taken before FindImage, which may move the view to a subresource of the
                // image it resolves to.
                PerfMemo::KeyBytes<CbDescKeySize> key;
                key.Add(col_buf).Add(hint);
                auto& memo = cb_desc_memo[cb];
                if (const auto* known = memo.Find(key, memo_epoch)) {
                    ++rt_counters.hits;
                    if (check) {
                        const VideoCore::TextureCache::ImageDesc fresh{col_buf, hint};
                        PerfCheckDesc("colour target", cb, *known, fresh);
                    }
                    desc = *known;
                } else {
                    ++rt_counters.misses;
                    std::construct_at(&desc, col_buf, hint);
                    memo.Store(key, desc, memo_epoch);
                }
            } else {
                std::construct_at(&desc, col_buf, hint);
            }
        };
        if (target_memo) {
            PerfMemo::KeyBytes<CbDescKeySize> target_key;
            target_key.Add(col_buf).Add(hint);
            image_id = bound_images.emplace_back(
                FindTargetMemoized(cb_target_memo[cb], target_key, desc, build_desc));
        } else {
            build_desc();
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        }
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    }

    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& hint = std::as_const(ScratchDraw::DepthExtent(liverpool));
        auto& image_id = db_desc.first;
        auto& desc = db_desc.second;
        const auto build_desc = [&] {
            if (rt_memo) {
                PerfMemo::KeyBytes<DbDescKeySize> key;
                key.Add(regs.depth_buffer)
                    .Add(regs.depth_view)
                    .Add(regs.depth_control)
                    .Add(htile_address)
                    .Add(hint);
                if (const auto* known = db_desc_memo.Find(key, memo_epoch)) {
                    ++rt_counters.hits;
                    if (check) {
                        const VideoCore::TextureCache::ImageDesc fresh{
                            regs.depth_buffer, regs.depth_view, regs.depth_control, htile_address,
                            hint};
                        PerfCheckDesc("depth target", 0, *known, fresh);
                    }
                    desc = *known;
                } else {
                    ++rt_counters.misses;
                    std::construct_at(&desc, regs.depth_buffer, regs.depth_view,
                                      regs.depth_control, htile_address, hint);
                    db_desc_memo.Store(key, desc, memo_epoch);
                }
            } else {
                std::construct_at(&desc, regs.depth_buffer, regs.depth_view, regs.depth_control,
                                  htile_address, hint);
            }
        };
        if (target_memo) {
            PerfMemo::KeyBytes<DbDescKeySize> target_key;
            target_key.Add(regs.depth_buffer)
                .Add(regs.depth_view)
                .Add(regs.depth_control)
                .Add(htile_address)
                .Add(hint);
            image_id = bound_images.emplace_back(
                FindTargetMemoized(db_target_memo, target_key, desc, build_desc));
        } else {
            build_desc();
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
        }
        auto& image = texture_cache.GetImage(image_id);
        image.binding.is_target = 1u;
    } else {
        db_desc.first = {};
    }
}

namespace {
// FAST_CPU_DRAW_NOALLOC (on unless '0'; read once at start-up, a plain load per draw): DrawRecord
// reads the vertex and instance offset SGPRs from the pipeline's own fetch shader data.
// GetDrawOffsets took a std::optional<FetchShaderData> and GraphicsPipeline::GetFetchShader gives
// a std::optional<const FetchShaderData>, so every draw built a converted copy whose attribute
// vector was allocated and freed on the heap (RtlAllocateHeap alone ~1.3% of Stage B; the sampler's
// stack scan put it under BindIndexBuffer, whose stale return address it found first).
const bool g_draw_noalloc = [] {
    const char* value = std::getenv("FAST_CPU_DRAW_NOALLOC");
    return !value || value[0] != '0';
}();
} // namespace

// Any std::optional of the fetch shader data, by reference: no conversion copies it.
template <typename FetchShader>
static std::pair<u32, u32> GetDrawOffsets(const AmdGpu::Regs& regs, const Shader::Info& info,
                                          const std::optional<FetchShader>& fetch_shader) {
    u32 vertex_offset = regs.index_offset;
    u32 instance_offset = 0;
    if (fetch_shader) {
        if (vertex_offset == 0 && fetch_shader->vertex_offset_sgpr != -1) {
            vertex_offset = info.UserData()[fetch_shader->vertex_offset_sgpr];
        }
        if (fetch_shader->instance_offset_sgpr != -1) {
            instance_offset = info.UserData()[fetch_shader->instance_offset_sgpr];
        }
    }
    return {vertex_offset, instance_offset};
}

void Rasterizer::EliminateFastClear() {
    auto& col_buf = std::as_const(ScratchDraw::Registers(liverpool)).color_buffers[0];
    if (!col_buf || !col_buf.info.fast_clear) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc(col_buf,
                                            std::as_const(ScratchDraw::ColorExtents(liverpool))[0]);
    const auto image_id = texture_cache.FindImage(desc);
    const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
    if (!texture_cache.IsMetaCleared(col_buf.CmaskAddress(), col_buf.view.slice_start)) {
        return;
    }
    for (u32 slice = col_buf.view.slice_start; slice <= col_buf.view.slice_max; ++slice) {
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);
    }
    auto& image = texture_cache.GetImage(image_id);
    const auto clear_value = LiverpoolToVK::ColorBufferClearValue(col_buf);

    ScopeMarkerBegin(fmt::format("EliminateFastClear:MRT={:#x}:M={:#x}", col_buf.Address(),
                                 col_buf.CmaskAddress()));
    image.Clear(clear_value, desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::Draw(bool is_indexed, u32 index_offset) {
    // Bit 31: set by the command processor for this draw packet only (a null draw counts as
    // absent in the statistics).
    const bool try_prepared = std::exchange(prepared_set, false);
    const PreparedDraw* const prepared = std::exchange(prepared_draw, nullptr);
    UpdateDrawWorker();
    FastRender::NoteDraw();
    ScratchProfile::Cpu scratch_cpu(5, "Rasterizer::Draw");
    RENDERER_TRACE;
    if (!worker_active) {
        PerfTick();
        scheduler.PopPendingOperations();
    }

    if (worker_active) {
        if (!IsRegularDraw()) {
            RecordPacket({}, nullptr, PacketKind::FilteredDraw);
            return;
        }
    } else if (!FilterDraw()) {
        return;
    }

    const bool own_camera = worker_active || FastRender::On(FastRender::PacketLoopback);
    const VR::CameraRecord draw_camera = own_camera ? VR::DrawFrameRecord() : VR::CameraRecord{};
    VR::DrawCameraScope camera_scope{own_camera ? &draw_camera : nullptr};
    FastRender::ProducerScope producer_scope{worker_active};
    auto selection_lock = pipeline_cache.LockSelection();
    pipeline_cache.NoteAsyncDraw(); // bit 40
    // FAST_CPU_VIEW_PREDICT (native stereo): 3, or 0 for targets that are not eye pairs, from the
    // worker's guess on a prepared hit, else this draw's own (GetDrawPipeline).
    const u32 view_mask = NativeStereo::Enabled() ? 3 : 0;
    bool validated = false;
    const GraphicsPipeline* pipeline =
        try_prepared ? pipeline_cache.TryPreparedGraphics(prepared, prepared_checksum, view_mask,
                                                          own_camera ? &draw_camera : nullptr,
                                                          &validated)
                     : nullptr;
    const GraphicsPipeline* const validated_pipeline = validated ? pipeline : nullptr;
    if (!pipeline) // bit 40's skip rule (native stereo, FAST_ASYNC_PIPELINE_SKIP=1): null while
                   // its pipeline compiles
        pipeline = pipeline_cache.GetDrawPipeline(view_mask, true);
    if (!pipeline) {
        return;
    }
    if (DiagSkip::Skip(pipeline->GetStages())) // FAST_DIAG_SKIP_FILE (diagnostic)
        return;
    VrSkipNoteDraw(pipeline); // bit 40's skip rule: the UI phase starts at the composite
    if (!PrepareVrDraw(pipeline, is_indexed, false, index_offset))
        return;
    if (worker_active || FastRender::On(FastRender::PacketLoopback)) {
        // Bit 32: the worker's packet preparation belongs to the hit's own pipeline only
        // (PrepareVrDraw may substitute a body-cut variant).
        packet_prepared = validated_pipeline && pipeline == validated_pipeline &&
                                  FastRender::On(PacketPrepBit)
                              ? prepared
                              : nullptr;
        RecordPacket(std::move(selection_lock), pipeline, PacketKind::Draw, is_indexed, 0,
                     index_offset);
    } else {
        recording_vr = producer_vr;
        probe_selected = recording_vr.probe_selected;
        selection_lock.unlock();
        DrawRecord(pipeline, is_indexed, index_offset);
    }
}

// FAST bit 54 (SkipNoopDraws; gpu_plan.md #3, research_gpu_passes.md §3, research_gpu_multiview.md
// §3.2 and B). About 10 draws a frame use the shared full-screen VS (89cda9e4) with no pixel
// shader and depth and stencil off: on Gnm the DB does their work (HTILE resummarize or
// decompress through DB_RENDER_CONTROL), and our renderer has no HTILE. Each still costs a render
// pass with no attachment at the device's maximum size (BeginRenderingFull) and a rasterized
// quad, ~9.8 us: the `pass 32768x32768 [] depth - vs 89cda9e4 ps 00000000` label, 0.10 ms per
// frame at 1456x1576 and 0.155 at 2000x2160.
//
// Only three commands of such a draw are left out: its vkCmdBeginRendering (with the end of the
// open pass that implies), vkCmdBindPipeline and vkCmdDraw*. Everything before them still runs
// and is recorded: target and resource lookups, transitions, uploads, barriers (the indirect
// argument barrier included), descriptors, push constants, dynamic state, the VR hooks, the
// mid-frame submit decision, VrAfterDraw. Those three commands cannot write memory when all of
// these hold, each checked here per draw on the pipeline actually bound:
//  - the pass has no colour, depth or stencil attachment (RenderState, every view null);
//  - the guest has no pixel shader, and the only guest stage is a plain hardware VS (no GS, HS or
//    DS, so no ES/LS either). The host may add its clip-distance discard fragment shader or the
//    rect/quad-list passthrough tessellation shaders; they write nothing;
//  - no guest stage writes memory: no written buffer or image, no image atomic, no storage image,
//    no raw (DMA) access, no GDS, fault or BDA page-table buffer;
//  - streamout is off (VGT_STRMOUT_CONFIG 0); the recompiler emits no transform feedback anyway;
//  - nothing counts its samples: the renderer begins no occlusion or pipeline-statistics query
//    (its only queries are timestamps), and ZPASS_DONE results are synthetic on stage A
//    (liverpool.cpp EventWrite), independent of any draw;
//  - no VR hook records into the draw: no UI shadow draw (K/W), probe, cape/body/hands split, AA
//    replacement or motion variant;
//  - without VK_EXT_vertex_input_dynamic_state, no fetch shader. There only a pipeline with vertex
//    bindings takes its strides as dynamic state (vk_graphics_pipeline.cpp), and this draw's
//    bindVertexBuffers2 strides would be set while the previous pipeline stays bound; when the
//    next draw uses that pipeline again, bit 35 drops its bind and its draw would follow a stride
//    set for a pipeline with static strides (VUID-vkCmdDraw-None-08608). With the extension every
//    pipeline has the same dynamic state list, so no other setter recorded here can do that.
// The open pass then stays open into the next draw, which begins or continues it by the usual
// state comparison, exactly as when the game issues those two draws back to back; every barrier
// site ends the pass itself, as it already had to for the empty pass. Decided per draw from one
// read of the bit, so it may flip between any two draws (and frames). Off: the old stream.
//
// FAST_SKIP_NOOP_DRAWS_VERIFY=N: every Nth skippable draw is recorded the old way after an
// independent re-derivation from the guest registers, the pipeline key, every RenderState slot
// and every guest stage (NoopDrawRecheck); a disagreement logs an error and that pipeline is never
// skipped again. There is no destination to read back: the bit-identical output is checked with
// the A/B's sample_L/R captures. Every 4096 skippable draws a 'FAST skip_noop_draws:' line gives
// the counts, and the first 32 distinct register keys are logged once (what these draws are).
static constexpr auto SkipNoopDraws = static_cast<FastRender::Bit>(54);

// Why a draw of the skippable shape (no attachment, no guest pixel shader) was recorded anyway.
enum NoopDrawFallback : u32 {
    NoopStages,    // a GS, HS or DS, or the VS is an ES/LS
    NoopWrites,    // a stage may write memory
    NoopStreamout, // VGT_STRMOUT_CONFIG is not 0
    NoopVrHook,    // a VR hook records into this draw
    NoopStrides,   // vertex bindings with dynamic strides only on some pipelines (no
                   // VK_EXT_vertex_input_dynamic_state)
    NoopDenied,    // the verify disagreed on this pipeline before
    NoopVerify,    // sampled by FAST_SKIP_NOOP_DRAWS_VERIFY
    NoopFallbacks,
};

static u64 NoopDrawVerifyEvery() {
    static const u64 every = [] {
        const char* value = std::getenv("FAST_SKIP_NOOP_DRAWS_VERIFY");
        return value ? static_cast<u64>(std::strtoull(value, nullptr, 0)) : u64{0};
    }();
    return every;
}

// A guest stage that may write memory: a written buffer or image, an image atomic, a storage
// image, raw (DMA) memory access, or a GDS, fault or BDA page-table buffer (shared with bit 40's
// skip rule, vk_pipeline_async.cpp).
static bool NoopStageMayWrite(const Shader::Info& info) {
    return PipelineSkip::StageMayWrite(info);
}

template <typename T>
static u32 NoopRawRegister(const T& value) {
    static_assert(sizeof(T) == sizeof(u32) && std::is_trivially_copyable_v<T>);
    u32 raw;
    std::memcpy(&raw, &value, sizeof(raw));
    return raw;
}

bool Rasterizer::SkipNoopDraw(const GraphicsPipeline* pipeline, const RenderState& state,
                              bool is_indexed, bool ui_shadow) {
    if (!FastRender::On(SkipNoopDraws)) {
        return false;
    }
    using Shader::LogicalStage;
    const auto stages = pipeline->GetStages();
    const auto& db = state.depth_stencil_attachment;
    if (state.num_color_attachments != 0 || db.has_depth || db.has_stencil || db.image_view ||
        stages[static_cast<u32>(LogicalStage::Fragment)] != nullptr) {
        return false; // an attachment or a guest pixel shader: not this shape
    }
    auto& s = noop_draw_stats;
    static_assert(NoopFallbacks == std::tuple_size_v<decltype(s.fallback)>);
    if ((++s.eligible & 4095) == 0) {
        ReportNoopDraws();
    }
    const auto fallback = [&s](NoopDrawFallback reason) {
        ++s.fallback[reason];
        return false;
    };
    const Shader::Info* const vs = stages[static_cast<u32>(LogicalStage::Vertex)];
    if (vs == nullptr || vs->stage != Shader::Stage::Vertex ||
        stages[static_cast<u32>(LogicalStage::TessellationControl)] != nullptr ||
        stages[static_cast<u32>(LogicalStage::TessellationEval)] != nullptr ||
        stages[static_cast<u32>(LogicalStage::Geometry)] != nullptr) {
        return fallback(NoopStages);
    }
    if (NoopStageMayWrite(*vs)) {
        return fallback(NoopWrites);
    }
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    if (regs.vgt_strmout_config.raw != 0) {
        return fallback(NoopStreamout);
    }
    if (ui_shadow || pipeline->HasVrMotion() ||
        (vr_warp && (vr_aa_action != 0 ||
                     (is_indexed && (probe_selected || recording_vr.split != nullptr ||
                                     recording_vr.body_split != nullptr ||
                                     recording_vr.hands_split != nullptr))))) {
        return fallback(NoopVrHook);
    }
    if (!instance.IsVertexInputDynamicState() && pipeline->GetFetchShader()) {
        return fallback(NoopStrides); // see the comment above SkipNoopDraws
    }
    if (!noop_draw_denied.empty() &&
        std::find(noop_draw_denied.begin(), noop_draw_denied.end(), pipeline) !=
            noop_draw_denied.end()) {
        return fallback(NoopDenied);
    }
    if (noop_draw_logged.size() < 32) {
        const std::array<u32, 6> key{static_cast<u32>(vs->pgm_hash),
                                     static_cast<u32>(vs->pgm_hash >> 32),
                                     NoopRawRegister(regs.depth_render_control),
                                     NoopRawRegister(regs.depth_render_override),
                                     NoopRawRegister(regs.color_control),
                                     NoopRawRegister(regs.depth_control)};
        if (std::find(noop_draw_logged.begin(), noop_draw_logged.end(), key) ==
            noop_draw_logged.end()) {
            noop_draw_logged.push_back(key);
            LOG_INFO(Render_Vulkan,
                     "FAST skip_noop_draws: key vs {:#x} DB_RENDER_CONTROL {:#x} "
                     "DB_RENDER_OVERRIDE {:#x} CB_COLOR_CONTROL {:#x} DB_DEPTH_CONTROL {:#x} "
                     "depth_valid {} stencil_valid {} htile {:#x} prim {} indices {} indexed {}",
                     vs->pgm_hash, key[2], key[3], key[4], key[5],
                     regs.depth_buffer.DepthValid() ? 1 : 0,
                     regs.depth_buffer.StencilValid() ? 1 : 0,
                     regs.depth_htile_data_base.GetAddress(),
                     static_cast<u32>(regs.primitive_type), regs.num_indices, is_indexed ? 1 : 0);
        }
    }
    if (const u64 every = NoopDrawVerifyEvery(); every != 0 && ++s.verify_tick % every == 0) {
        ++s.verified;
        if (const char* why = NoopDrawRecheck(pipeline, state)) {
            ++s.mismatches;
            noop_draw_denied.push_back(pipeline);
            LOG_ERROR(Render_Vulkan,
                      "FAST skip_noop_draws verify: mismatch #{} on vs {:#x}: {}; this pipeline "
                      "is drawn the old way from now on",
                      s.mismatches, vs->pgm_hash, why);
        }
        return fallback(NoopVerify); // the sampled draw itself: the old way
    }
    ++s.skipped;
    return true;
}

const char* Rasterizer::NoopDrawRecheck(const GraphicsPipeline* pipeline,
                                        const RenderState& state) const {
    using Shader::LogicalStage;
    const auto& key = pipeline->GetGraphicsKey();
    if (key.mrt_mask != 0 || key.num_color_attachments != 0) {
        return "the pipeline key has colour targets";
    }
    if (key.stage_hashes[static_cast<u32>(LogicalStage::Fragment)] != 0) {
        return "the pipeline key has a pixel shader";
    }
    for (const auto& attachment : state.color_attachments) {
        if (attachment.image_view || attachment.is_clear != 0) {
            return "a colour attachment slot is set";
        }
    }
    const auto& db = state.depth_stencil_attachment;
    if (db.image_view || db.is_clear != 0) {
        return "the depth/stencil attachment is set";
    }
    if (db_desc.first) {
        return "a depth target was resolved";
    }
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    if (regs.ps_program.Address<const u32*>() != nullptr) {
        return "the registers have a pixel shader";
    }
    if (regs.stage_enable.raw != AmdGpu::ShaderStageEnable::VgtStages::Vs) {
        return "the registers enable stages other than the VS";
    }
    if (regs.vgt_strmout_config.raw != 0) {
        return "the registers enable streamout";
    }
    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid())) {
        return "the registers enable depth or stencil on a valid buffer";
    }
    u32 present = 0;
    for (const Shader::Info* stage : pipeline->GetStages()) {
        if (stage == nullptr) {
            continue;
        }
        ++present;
        if (stage->l_stage != LogicalStage::Vertex || stage->stage != Shader::Stage::Vertex) {
            return "a guest stage other than the hardware VS";
        }
        if (NoopStageMayWrite(*stage)) {
            return "a guest stage may write memory";
        }
    }
    if (present != 1) {
        return "not exactly one guest stage";
    }
    return nullptr;
}

void Rasterizer::ReportNoopDraws() {
    auto& s = noop_draw_stats;
    const u64 frames = FastRender::g_counters.frames.load(std::memory_order_relaxed);
    const u64 frame_delta = frames - s.report_frames;
    const double per_frame =
        frame_delta != 0 ? static_cast<double>(s.skipped - s.report_skipped) /
                               static_cast<double>(frame_delta)
                         : 0.0;
    s.report_frames = frames;
    s.report_skipped = s.skipped;
    const auto& f = s.fallback;
    LOG_INFO(Render_Vulkan,
             "FAST skip_noop_draws: eligible={} skipped={} ({:.2f}/frame since the last line) "
             "fallback stages={} writes={} streamout={} vr={} strides={} denied={} verify={} "
             "verified={} mismatches={}",
             s.eligible, s.skipped, per_frame, f[NoopStages], f[NoopWrites], f[NoopStreamout],
             f[NoopVrHook], f[NoopStrides], f[NoopDenied], f[NoopVerify], s.verified,
             s.mismatches);
}

namespace {
// VR_NATIVE_CAMERA_AUDIT, read once at start-up: a plain load per draw and per buffer where the
// function-local statics paid a guard (a thread-local read) each time.
const bool g_camera_audit = std::getenv("VR_NATIVE_CAMERA_AUDIT") != nullptr;
// FAST_CPU_TAN_MEMO (on unless '0'; read once at start-up, a plain load per use): a world draw's
// eye shift takes its camera record's std::tan(0.5f * vfov) from HalfTanMemo (keyed by the vfov's
// exact bits, the same float) instead of a tanf per draw (0.55% of Stage B).
const bool g_tan_memo = [] {
    const char* value = std::getenv("FAST_CPU_TAN_MEMO");
    return !value || value[0] != '0';
}();
// FAST_CPU_TAN_MEMO_VERIFY=1: every draw also computes it, compares the bits and uses the computed
// value; a mismatch is logged (the first 16, then every 1024th).
const bool g_tan_memo_verify = [] {
    const char* value = std::getenv("FAST_CPU_TAN_MEMO_VERIFY");
    return value && value[0] == '1';
}();
// The recording thread's memo and counts (one thread records at a time; per thread, as the other
// recording memos, so an ownership change needs no hand-over).
struct TanMemoState {
    HalfTanMemo memo;
    u64 calls{}, misses{}, verified{}, mismatches{};
};
thread_local TanMemoState tan_memo;
float VrHalfTan(float vfov) {
    if (!g_tan_memo) {
        return HalfTanMemo::Compute(vfov);
    }
    auto& t = tan_memo;
    if constexpr (Common::Log::Enabled) {
        t.misses += !t.memo.valid || t.memo.bits != std::bit_cast<u32>(vfov);
    }
    float value = t.memo(vfov);
    if (g_tan_memo_verify) {
        ++t.verified;
        const float full = HalfTanMemo::Compute(vfov);
        if (std::bit_cast<u32>(full) != std::bit_cast<u32>(value)) {
            if (++t.mismatches <= 16 || (t.mismatches & 1023) == 0) {
                LOG_ERROR(Render_Vulkan,
                          "FAST_CPU_TAN_MEMO verify: vfov {:#x}: memo {:#x}, full {:#x}; the full "
                          "value is used, mismatch #{}",
                          std::bit_cast<u32>(vfov), std::bit_cast<u32>(value),
                          std::bit_cast<u32>(full), t.mismatches);
            }
            value = full;
        }
    }
    if constexpr (Common::Log::Enabled) {
        if ((++t.calls & 0xfffff) == 0) {
            LOG_INFO(Render_Vulkan,
                     "FAST_CPU_TAN_MEMO: {} world draws, {} tangents computed; verified={} "
                     "mismatches={}",
                     t.calls, t.misses, t.verified, t.mismatches);
        }
    }
    return value;
}

// FAST_CPU_VIEW_PREDICT_VERIFY=1 (read once at start-up): a draw Stage A selected with the
// predicted view mask 0 is reselected here all the same, as the old order did, and compared
// (Rasterizer::VrViewPredicted).
const bool g_view_predict_verify = [] {
    const char* value = std::getenv("FAST_CPU_VIEW_PREDICT_VERIFY");
    return value && value[0] == '1';
}();
// The recording thread's counts (per thread, as the tan memo's): the reselects Stage A's guess
// saved, the ones its wrong 0 cost, the ones left (Stage A's 3 for targets that are not eye
// pairs), and the verify mode's comparisons.
struct ViewPredictState {
    u64 saved{}, mispredicted{}, left{}, verified{}, mismatches{};
};
thread_local ViewPredictState view_predict_state;

// Every 16384 recorded draws that were reselected or kept their 0: Stage B's counts and Stage
// A's and the workers' (PipelineCache::ViewPredictCounter).
void LogViewPredict(const ViewPredictState& s, const PipelineCache& cache) {
    if (((s.saved + s.mispredicted + s.left) & 16383) != 0) {
        return;
    }
    using C = PipelineCache::ViewPredictCounter;
    LOG_INFO(Render_Vulkan,
             "FAST_CPU_VIEW_PREDICT: reselects saved={} mispredicted={} left={}; Stage A "
             "selected={} prepared={}, workers prepared={}; mono pipelines built={} queued={} "
             "(their multiview twins not built); verified={} mismatches={}",
             s.saved, s.mispredicted, s.left, cache.ViewPredictCount(C::ViewSelected),
             cache.ViewPredictCount(C::ViewPrepared), cache.ViewPredictCount(C::ViewWorker),
             cache.ViewPredictCount(C::ViewBuilt), cache.ViewPredictCount(C::ViewQueued),
             s.verified, s.mismatches);
}
} // namespace

void Rasterizer::VrViewReselected(bool mispredicted) {
    if constexpr (Common::Log::Enabled) {
        if (!pipeline_cache.ViewPredict()) {
            return;
        }
        auto& s = view_predict_state;
        ++(mispredicted ? s.mispredicted : s.left);
        LogViewPredict(s, pipeline_cache);
    }
}

const GraphicsPipeline* Rasterizer::VrViewPredicted(const GraphicsPipeline* pipeline) {
    if (g_view_predict_verify) {
        auto& s = view_predict_state;
        // The old order: Stage A's multiview pipeline reselected here, GetGraphicsPipeline(0)
        // from this draw's registers and its cut variant (ReselectVrCut). Stage A's VrCutPipeline
        // asked for the same variant of the same base: a variant still compiling then is drawn
        // uncut, as it is here while it compiles (a timing, not a mismatch).
        ++s.verified;
        const GraphicsPipeline* base = pipeline_cache.GetGraphicsPipeline(0);
        bool same = base != nullptr && base == pipeline;
        const u32 cut = recording_vr.cut_code;
        if (base != nullptr && vr_warp && cut != 0 && base->GetGraphicsKey().mrt_mask != 0) {
            const GraphicsPipeline* variant = pipeline_cache.GetVrCutPipeline(base, cut, false);
            if (recording_vr.body_split) {
                same = same && (recording_vr.body_cut_pipeline == nullptr ||
                                recording_vr.body_cut_pipeline == variant);
            } else {
                same = same || (variant != nullptr && pipeline == variant);
            }
        }
        if (!same) {
            ++s.mismatches;
            if (s.mismatches <= 16 || (s.mismatches & 1023) == 0) {
                LOG_ERROR(Render_Vulkan,
                          "FAST_CPU_VIEW_PREDICT verify: Stage A's pipeline {:#x} (key {:#x}), "
                          "reselected {:#x} (key {:#x}), cut {:#x}; the reselected one is drawn, "
                          "mismatch #{}",
                          reinterpret_cast<uintptr_t>(pipeline),
                          std::hash<GraphicsPipelineKey>{}(pipeline->GetGraphicsKey()),
                          reinterpret_cast<uintptr_t>(base),
                          base ? std::hash<GraphicsPipelineKey>{}(base->GetGraphicsKey()) : 0,
                          cut, s.mismatches);
            }
            if (base != nullptr) {
                pipeline = vr_warp ? ReselectVrCut(base) : base;
            }
        }
    }
    if constexpr (Common::Log::Enabled) {
        auto& s = view_predict_state;
        ++s.saved;
        LogViewPredict(s, pipeline_cache);
    }
    return pipeline;
}

// Native stereo, before the draw's push: its light tiles and, with VrPushData, the values pushed
// after push_data, all from `record`, the camera record of the draw's shift and camera buffers
// (the packet's on Stage B, the one Stage A selected the draw with). They are exactly what a
// permutation baked before: NativeStereo::DrawProjection is BuildRuntimeInfo's projection,
// LightScissors its light bounds, VrCut its cut, the motion buffers its vr_motion addresses. A
// draw without a tile pushes a test every pixel passes, as its permutation without the test
// drew. `pipeline` is the one the draw binds: after VrTemporalSelect (DrawRecord; an indirect
// draw never gets a motion variant). A draw of a planar reflection's pass (VrInMirrorPass, its
// cameras noted by BindBuffers before this) gets both upside down, as its mirrored camera.
void Rasterizer::VrNativeDrawValues(const GraphicsPipeline* pipeline,
                                    const VR::CameraRecord& record) {
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    const auto* light_vertex = pipeline->GetStages()[u32(Shader::LogicalStage::Vertex)];
    const bool mirror = VrInMirrorPass();
    // GuestShaderKey is HashCombine(program, 0); classification uses the raw program hash.
    vr_light_tiles.valid = recording_view_mask == 3 && light_vertex &&
                           NativeProjection::ScreenQuadShader(light_vertex->pgm_hash) &&
                           NativeStereo::LightScissors(regs, record, vr_light_tiles.bounds, mirror);
    vr_push = {};
    if (!pipeline_cache.UsesVrPushData()) {
        return; // only scene_inverse is pushed (set with the push)
    }
    if (NativeStereo::AsymmetricProjection()) {
        vr_push.projection = NativeStereo::DrawProjection(record, mirror);
    }
    if (vr_light_tiles.valid) {
        vr_push.light = vr_light_tiles.bounds;
    }
    vr_push.SetCut(recording_vr.cut_code);
    // A motion variant reads the buffers it was selected with; every other draw pushes zeros.
    if (pipeline->HasVrMotion()) {
        vr_push.motion_params = vr_motion_buffers.params_address;
        vr_push.motion_positions = vr_motion_buffers.positions_address;
    }
}

bool Rasterizer::VrMirrorFlip() const {
    return NativeStereo::MirrorFlip() && pipeline_cache.UsesVrPushData();
}

// The draw's screen is its screen scissor's size: a reflection's pass has one of its own (the sewer
// water's: half the eye images, 728x788 of 1456x1576).
bool Rasterizer::VrInMirrorPass() const {
    if (vr_mirror.screen[0] == 0) {
        return false; // no reflection's pass: one load
    }
    const auto& screen = std::as_const(ScratchDraw::Registers(liverpool)).screen_scissor;
    return vr_mirror.Contains({screen.GetWidth(), screen.GetHeight()}, recording_view_mask == 3);
}

// BindBuffers paired a graphics draw's scene camera at guest address `camera` (`mirrored`:
// NativeStereo::CameraBuffer's answer, Stage A's for a ring constant), or VrMirrorAhead found it.
// The draw's first one decides.
void Rasterizer::VrMirrorCamera(bool mirrored, VAddr camera) {
    vr_mirror_noted = true;
    if (!mirrored && vr_mirror.screen[0] == 0) {
        return;
    }
    const auto& screen = std::as_const(ScratchDraw::Registers(liverpool)).screen_scissor;
    const std::array<u32, 2> size{screen.GetWidth(), screen.GetHeight()};
    if (!vr_mirror.Camera(mirrored, size) || vr_mirror_logged) {
        return;
    }
    vr_mirror_logged = true;
    if constexpr (Common::Log::Enabled) {
        const auto record = VR::DrawFrameRecord();
        const auto eyes = NativeStereo::DrawProjection(record);
        const auto flipped = NativeStereo::DrawProjection(record, true);
        LOG_INFO(Render_Vulkan,
                 "VR mirror flip: camera {:#x} (camera update {}) is a planar reflection's, "
                 "behind its own clip plane; its pass ({}x{}) is drawn with each eye's frustum "
                 "upside down: y offset left {:.4f} right {:.4f} (the eyes' {:.4f} {:.4f}); "
                 "FAST_GPU_MIRROR_FLIP=0 turns it off",
                 camera, record.id, size[0], size[1], flipped[0][3], flipped[1][3], eyes[0][3],
                 eyes[1][3]);
    }
}

// A multiview draw's fragment light is about to be paired and its scene camera is not noted yet:
// the draw's first scene camera in binding order (the stages BindResources binds, its buffers by
// index), as BindBuffers will pair it (Stage A's answer for a prepared constant, else
// NativeStereo::SceneCamera's), noted now. A light in the draw that starts a reflection's pass
// then pairs upside down, and one in the draw after it does not. Once per draw, only for draws
// with such a light.
void Rasterizer::VrMirrorAhead() {
    vr_mirror_looked = true;
    for (const auto* stage : vr_mirror_pipeline->GetStages()) {
        if (!stage || stage->l_stage == Shader::LogicalStage::Compute) {
            continue;
        }
        for (u32 i = 0; i < stage->buffers.size(); ++i) {
            const auto& desc = stage->buffers[i];
            if (desc.IsSpecial() || desc.is_written) {
                continue;
            }
            const auto sharp = desc.GetSharp(*stage);
            if (sharp.base_address == 0 || sharp.GetSize() < 864) {
                continue;
            }
            const auto* c = binding_packet ? binding_packet->Constant(*stage, i) : nullptr;
            bool camera = false, mirrored = false;
            if (c && !c->flat && std::memcmp(&c->sharp, &sharp, sizeof(sharp)) == 0) {
                camera = c->bytes == 864 && c->camera;
                mirrored = c->mirrored;
            } else if (memory->ClampRangeSize(sharp.base_address, sharp.GetSize()) == 864) {
                camera = NativeStereo::SceneCamera(
                    reinterpret_cast<const float*>(sharp.base_address), VR::DrawFrameRecord(),
                    &mirrored);
            }
            if (camera) {
                VrMirrorCamera(mirrored, sharp.base_address);
                return;
            }
        }
    }
}

void Rasterizer::DrawRecord(const GraphicsPipeline* pipeline, bool is_indexed, u32 index_offset) {
    FastRender::BProfile::Scope b_detail{FastRender::BProfile::DrawSetup};
    recording_view_mask = pipeline->GetGraphicsKey().probe_view_mask;
    const auto& regs = ScratchDraw::Registers(liverpool);
    AssertRecordingOwner();
    if (vr_warp) {
        vr_aa_action = VrAaDecide(pipeline, false);
        if (vr_aa_action == static_cast<u8>(VR::Aa::Action::Skip))
            return;
        VrBeforeDraw(pipeline);
    }

    if (vr_warp && is_indexed)
        VrProbePrepare(pipeline);
    PrepareRenderState(pipeline);
    if (NativeStereo::Enabled()) {
        const auto& target_regs = std::as_const(ScratchDraw::Registers(liverpool));
        bool any = false, paired = true;
        u64 targets = 0; // bit 40's skip rule: these attachments' key (PipelineSkip::AddTarget)
        for (u32 cb = 0; cb < std::bit_width(pipeline->GetGraphicsKey().mrt_mask); ++cb) {
            const auto id = cb_descs[cb].first;
            if (id) {
                any = true;
                paired &= texture_cache.GetImage(id).native_stereo;
                targets = PipelineSkip::AddTarget(targets, target_regs.color_buffers[cb].Address(),
                                                  cb);
            }
        }
        if (db_desc.first) {
            any = true;
            paired &= texture_cache.GetImage(db_desc.first).native_stereo;
            targets = PipelineSkip::AddTarget(targets, target_regs.depth_buffer.DepthAddress(), 8);
        }
        const u32 desired = any && paired ? 3 : 0;
        // Bit 40's skip rule leaves out only draws into eye pairs (Stage A reads it).
        NoteTargetViews(targets, desired);
        if (recording_view_mask != desired) {
            FastRender::BProfile::Scope b_detail{FastRender::BProfile::Reselect};
            FastRender::Bump(FastRender::g_counters.view_mask_reselects);
            VrViewReselected(recording_view_mask == 0);
            recording_view_mask = desired;
            pipeline = pipeline_cache.GetGraphicsPipeline(desired);
            if (vr_warp)
                pipeline = ReselectVrCut(pipeline);
        } else if (desired == 0) {
            pipeline = VrViewPredicted(pipeline); // FAST_CPU_VIEW_PREDICT: Stage A's 0 held
        }
    }
    if (vr_temporal_enabled)
        VrTemporalBeforeDraw(pipeline, false, is_indexed, index_offset);
    if (!BindResources(pipeline)) {
        return;
    }
    auto state = BeginRendering(pipeline);
    VrNativeAudit(pipeline, &state);
    if (vr_temporal_enabled)
        pipeline = VrTemporalSelect(pipeline, state);
    // Before anything of this draw is bound into the command buffer (vertex buffers, descriptors,
    // dynamic state): its transitions and uploads so far go with the submitted part.
    MaybeSubmitMidFrame(state);
    // VR aa_mode=smaa: the game's FXAA draw, its images bound, replaced by SMAA.
    if (vr_warp && vr_aa_action != 0 && VrAaReplace(pipeline, state)) {
        ResetBindings();
        return;
    }
    // VR UI layer: this UI colour draw is also drawn into K and W (before the game's own).
    const bool ui_shadow = vr_ui_layer && VrUiPrepare(pipeline, state);

    u64 motion_geometry{};
    buffer_cache.BindVertexBuffers(*pipeline, buffer_barriers,
                                   pipeline->HasVrMotion() ? &motion_geometry : nullptr,
                                   binding_packet && binding_packet->vertex_prepared &&
                                           binding_packet->selected == pipeline
                                       ? &binding_packet->vertex_view : nullptr);
    if (is_indexed) {
        buffer_cache.BindIndexBuffer(index_offset, buffer_barriers);
    }
    {
        const u64 vs_hash = pipeline->GuestShaderKey(Shader::LogicalStage::Vertex);
        auto vr = VR::StereoForDraw(state.width, state.height,
                                    state.depth_stencil_attachment.has_depth, vs_hash);
        if (vr_ui.native) {
            vr = {}; // a UI-layer frame's UI is native size and the same for both eyes
        }
        if (VR::DebugDrawLog()) {
            VrLogDraw(pipeline, state, vr.shift, vr.hud);
        }
        if (NativeStereo::Enabled()) {
            // FAST_CPU_PASS_MEMO: the draw's camera record read in place (DrawFrameRecord's copy
            // of it; on Stage B the packet's, under its DrawCameraScope).
            std::optional<VR::CameraRecord> camera_copy;
            const VR::CameraRecord* const camera = g_pass_memo ? VR::DrawCameraOverride() : nullptr;
            const auto& r = camera ? *camera : camera_copy.emplace(VR::DrawFrameRecord());
            const u32 paired = u32(push_data.vr_hud);
            vr = {};
            // Perspective W is checked in the shader. World draws need no depth target.
            const bool world = !vr_ui.native && state.probe_view_mask == 3 && r.vfov > 0 &&
                               r.aspect > 0 &&
                               (state.num_color_attachments > 0 ||
                                (paired & Shader::PushData::NativeProjectionPaired));
            if (g_camera_audit && !vr_ui.native && state.probe_view_mask == 3 &&
                state.num_color_attachments == 0) {
                static thread_local std::set<std::tuple<u64, u32, u32, u32>> seen;
                if (seen.emplace(vs_hash, state.width, state.height, paired).second)
                    LOG_INFO(
                        Render_Vulkan,
                        "VR depth-only draw: VS key {:#x} target {}x{} paired flags {} world {}",
                        vs_hash, state.width, state.height, paired, world);
            }
            vr.hud = float(paired | (world ? Shader::PushData::NativeWorld : 0));
            if (world)
                vr_eye_extent = {state.width, state.height};
            if (world)
                vr.shift =
                    .5f * NativeStereo::Ipd() * r.world_scale / (VrHalfTan(r.vfov) * r.aspect);
            VrNativeDrawValues(pipeline, r);
        }
        push_data.vr_shift = vr.shift;
        push_data.vr_hud = vr.hud;
        if (vr.hud < VR::RenderTaa::JitterTest) {
            // aa_mode=taa: how this jittered draw's NDC maps to window pixels (the resolve).
            vr_taa.xscale = std::as_const(ScratchDraw::Registers(liverpool)).viewports[0].xscale;
            vr_taa.yscale = std::as_const(ScratchDraw::Registers(liverpool)).viewports[0].yscale;
        }
    }

    FastRender::BProfile::Scope b_recording{FastRender::BProfile::Recording};
    const auto& vs_info = pipeline->GetStage(Shader::LogicalStage::Vertex);
    const auto& fetch_shader = pipeline->GetFetchShader();
    const auto [vertex_offset, instance_offset] =
        g_draw_noalloc ? GetDrawOffsets(regs, vs_info, fetch_shader)
                       : GetDrawOffsets(regs, vs_info, // FAST_CPU_DRAW_NOALLOC=0: the copy
                                        std::optional<Shader::Gcn::FetchShaderData>{fetch_shader});
    if (pipeline->HasVrMotion()) {
        const u32 motion = VrTemporalPrepareDraw(pipeline, motion_geometry, is_indexed,
                                                 index_offset, vertex_offset, instance_offset);
        push_data.vr_hud = Shader::VrMotion::PackPush(u32(push_data.vr_hud), motion);
    }
    vr_push.scene_inverse = vr_scene_inverse;
    pipeline->BindResources(set_writes, buffer_barriers, push_data, vr_push, NativeHandles());
    UpdateDynamicState(pipeline, is_indexed);

    if (vr_warp && is_indexed && probe_selected) {
        VrProbeDraw(pipeline, state, s32(vertex_offset), instance_offset);
    }
    // The UI layer's shadow draws (K and W): before the game's draw, or (FAST_GPU_PASS_MERGE,
    // vk_pass_merge.h) after it, its X pass going on.
    const auto issue = [&](RecorderProxy cb) {
        if (is_indexed) {
            cb.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                           s32(vertex_offset), instance_offset);
        } else {
            cb.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                    instance_offset);
        }
    };
    const bool shadow_after = ui_shadow && VrUiShadowAfter(pipeline, state);
    if (ui_shadow && !shadow_after) {
        VrUiShadowDraw(pipeline, state, VrDrawCall::Of(issue), false);
    }
    if (SkipNoopDraw(pipeline, state, is_indexed, ui_shadow)) {
        // Bit 54: no pass, pipeline or draw for a draw that cannot write memory.
        DebugState.IncDrawCall();
        if (vr_warp) {
            VrAfterDraw(pipeline, state);
        }
        ResetBindings();
        return;
    }
    if (GpuPassProfiler* const profiler = GpuPassProfiler::For(scheduler);
        profiler != nullptr && scheduler.WillBeginRendering(state)) [[unlikely]] {
        LabelGpuPass(*profiler, pipeline, state); // FAST_GPU_PASS_PROFILE, bit 38
    }
    scheduler.BeginRendering(state);

    const auto cmdbuf = scheduler.Commands();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        if (!vr_warp || !VrDrawCapeSplit(cmdbuf, regs.num_instances.NumInstances(),
                                         s32(vertex_offset), instance_offset)) {
            cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                               s32(vertex_offset), instance_offset);
        }
    } else {
        cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                    instance_offset);
    }
    if (shadow_after) {
        VrUiShadowDraw(pipeline, state, VrDrawCall::Of(issue), true);
    }
    DebugState.IncDrawCall();
    if (vr_warp) {
        VrAfterDraw(pipeline, state);
    }

    ResetBindings();
}

void Rasterizer::DrawIndirect(bool is_indexed, VAddr arg_address, u32 offset, u32 stride,
                              u32 max_count, VAddr count_address) {
    UpdateDrawWorker();
    FastRender::NoteDraw();
    ScratchProfile::Cpu scratch_cpu(6, "Rasterizer::DrawIndirect");
    RENDERER_TRACE;
    if (!worker_active) {
        PerfTick();
        scheduler.PopPendingOperations();
    }

    if (worker_active) {
        if (!IsRegularDraw()) {
            RecordPacket({}, nullptr, PacketKind::FilteredDraw);
            return;
        }
    } else if (!FilterDraw()) {
        return;
    }

    const bool own_camera = worker_active || FastRender::On(FastRender::PacketLoopback);
    const VR::CameraRecord draw_camera = own_camera ? VR::DrawFrameRecord() : VR::CameraRecord{};
    VR::DrawCameraScope camera_scope{own_camera ? &draw_camera : nullptr};
    FastRender::ProducerScope producer_scope{worker_active};
    auto selection_lock = pipeline_cache.LockSelection();
    const GraphicsPipeline* pipeline =
        pipeline_cache.GetDrawPipeline(NativeStereo::Enabled() ? 3 : 0); // FAST_CPU_VIEW_PREDICT
    if (!pipeline) {
        return;
    }
    if (DiagSkip::Skip(pipeline->GetStages())) // FAST_DIAG_SKIP_FILE (diagnostic)
        return;
    VrSkipNoteDraw(pipeline);
    if (!PrepareVrDraw(pipeline, is_indexed, true, 0))
        return;
    if (worker_active || FastRender::On(FastRender::PacketLoopback)) {
        RecordPacket(std::move(selection_lock), pipeline, PacketKind::DrawIndirect, is_indexed,
                     arg_address, offset, stride, max_count, count_address);
    } else {
        recording_vr = producer_vr;
        probe_selected = recording_vr.probe_selected;
        selection_lock.unlock();
        DrawIndirectRecord(pipeline, is_indexed, arg_address, offset, stride, max_count,
                           count_address);
    }
}

void Rasterizer::DrawIndirectRecord(const GraphicsPipeline* pipeline, bool is_indexed,
                                    VAddr arg_address, u32 offset, u32 stride, u32 max_count,
                                    VAddr count_address) {
    FastRender::BProfile::Scope b_detail{FastRender::BProfile::DrawSetup};
    recording_view_mask = pipeline->GetGraphicsKey().probe_view_mask;
    AssertRecordingOwner();
    if (vr_warp) {
        vr_aa_action = VrAaDecide(pipeline, true);
        if (vr_aa_action == static_cast<u8>(VR::Aa::Action::Skip))
            return;
        VrBeforeDraw(pipeline);
    }

    PrepareRenderState(pipeline);
    if (NativeStereo::Enabled()) {
        const auto& target_regs = std::as_const(ScratchDraw::Registers(liverpool));
        bool any = false, paired = true;
        u64 targets = 0; // bit 40's skip rule: these attachments' key (PipelineSkip::AddTarget)
        for (u32 cb = 0; cb < std::bit_width(pipeline->GetGraphicsKey().mrt_mask); ++cb) {
            const auto id = cb_descs[cb].first;
            if (id) {
                any = true;
                paired &= texture_cache.GetImage(id).native_stereo;
                targets = PipelineSkip::AddTarget(targets, target_regs.color_buffers[cb].Address(),
                                                  cb);
            }
        }
        if (db_desc.first) {
            any = true;
            paired &= texture_cache.GetImage(db_desc.first).native_stereo;
            targets = PipelineSkip::AddTarget(targets, target_regs.depth_buffer.DepthAddress(), 8);
        }
        const u32 desired = any && paired ? 3 : 0;
        // Bit 40's skip rule leaves out only draws into eye pairs (Stage A reads it).
        NoteTargetViews(targets, desired);
        if (recording_view_mask != desired) {
            FastRender::BProfile::Scope b_detail{FastRender::BProfile::Reselect};
            FastRender::Bump(FastRender::g_counters.view_mask_reselects);
            VrViewReselected(recording_view_mask == 0);
            recording_view_mask = desired;
            pipeline = pipeline_cache.GetGraphicsPipeline(desired);
            if (vr_warp)
                pipeline = ReselectVrCut(pipeline);
        } else if (desired == 0) {
            pipeline = VrViewPredicted(pipeline); // FAST_CPU_VIEW_PREDICT: Stage A's 0 held
        }
    }
    if (vr_temporal_enabled)
        VrTemporalBeforeDraw(pipeline, true);
    if (!BindResources(pipeline)) {
        return;
    }
    const auto state = BeginRendering(pipeline);
    VrNativeAudit(pipeline, &state);
    MaybeSubmitMidFrame(state);
    // VR UI layer: this UI colour draw is also drawn into K and W (before the game's own).
    const bool ui_shadow = vr_ui_layer && VrUiPrepare(pipeline, state);

    buffer_cache.BindVertexBuffers(*pipeline, buffer_barriers, nullptr,
                                   binding_packet && binding_packet->vertex_prepared &&
                                           binding_packet->selected == pipeline
                                       ? &binding_packet->vertex_view : nullptr);
    if (is_indexed) {
        buffer_cache.BindIndexBuffer(0, buffer_barriers);
    }
    {
        const u64 vs_hash = pipeline->GuestShaderKey(Shader::LogicalStage::Vertex);
        auto vr = VR::StereoForDraw(state.width, state.height,
                                    state.depth_stencil_attachment.has_depth, vs_hash);
        if (vr_ui.native) {
            vr = {}; // a UI-layer frame's UI is native size and the same for both eyes
        }
        if (VR::DebugDrawLog()) {
            VrLogDraw(pipeline, state, vr.shift, vr.hud);
        }
        if (NativeStereo::Enabled()) {
            // FAST_CPU_PASS_MEMO: the draw's camera record read in place (DrawFrameRecord's copy
            // of it; on Stage B the packet's, under its DrawCameraScope).
            std::optional<VR::CameraRecord> camera_copy;
            const VR::CameraRecord* const camera = g_pass_memo ? VR::DrawCameraOverride() : nullptr;
            const auto& r = camera ? *camera : camera_copy.emplace(VR::DrawFrameRecord());
            const u32 paired = u32(push_data.vr_hud);
            vr = {};
            // Perspective W is checked in the shader. World draws need no depth target.
            const bool world = !vr_ui.native && state.probe_view_mask == 3 && r.vfov > 0 &&
                               r.aspect > 0 &&
                               (state.num_color_attachments > 0 ||
                                (paired & Shader::PushData::NativeProjectionPaired));
            if (g_camera_audit && !vr_ui.native && state.probe_view_mask == 3 &&
                state.num_color_attachments == 0) {
                static thread_local std::set<std::tuple<u64, u32, u32, u32>> seen;
                if (seen.emplace(vs_hash, state.width, state.height, paired).second)
                    LOG_INFO(
                        Render_Vulkan,
                        "VR depth-only draw: VS key {:#x} target {}x{} paired flags {} world {}",
                        vs_hash, state.width, state.height, paired, world);
            }
            vr.hud = float(paired | (world ? Shader::PushData::NativeWorld : 0));
            if (world)
                vr_eye_extent = {state.width, state.height};
            if (world)
                vr.shift =
                    .5f * NativeStereo::Ipd() * r.world_scale / (VrHalfTan(r.vfov) * r.aspect);
            VrNativeDrawValues(pipeline, r);
        }
        push_data.vr_shift = vr.shift;
        push_data.vr_hud = vr.hud;
        if (vr.hud < VR::RenderTaa::JitterTest) {
            // aa_mode=taa: how this jittered draw's NDC maps to window pixels (the resolve).
            vr_taa.xscale = std::as_const(ScratchDraw::Registers(liverpool)).viewports[0].xscale;
            vr_taa.yscale = std::as_const(ScratchDraw::Registers(liverpool)).viewports[0].yscale;
        }
    }

    const auto& [buffer, base] =
        buffer_cache.ObtainBuffer(arg_address + offset, stride * max_count, false);

    VideoCore::Buffer* count_buffer{};
    u32 count_base{};
    if (count_address != 0) {
        std::tie(count_buffer, count_base) = buffer_cache.ObtainBuffer(count_address, 4, false);
    }

    if (auto barrier = buffer->GetBarrier(vk::AccessFlagBits2::eIndirectCommandRead,
                                          vk::PipelineStageFlagBits2::eDrawIndirect)) {
        buffer_barriers.emplace_back(*barrier);
    }
    if (count_buffer) {
        if (auto barrier = count_buffer->GetBarrier(vk::AccessFlagBits2::eIndirectCommandRead,
                                                    vk::PipelineStageFlagBits2::eDrawIndirect)) {
            buffer_barriers.emplace_back(*barrier);
        }
    }

    FastRender::BProfile::Scope b_recording{FastRender::BProfile::Recording};
    vr_push.scene_inverse = vr_scene_inverse;
    pipeline->BindResources(set_writes, buffer_barriers, push_data, vr_push, NativeHandles());
    UpdateDynamicState(pipeline, is_indexed);
    if (ui_shadow) {
        const auto issue = [&](RecorderProxy cb) {
            if (is_indexed) {
                if (count_address != 0) {
                    cb.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                                count_base, max_count, stride);
                } else {
                    cb.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
                }
            } else if (count_address != 0) {
                cb.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(), count_base,
                                     max_count, stride);
            } else {
                cb.drawIndirect(buffer->Handle(), base, max_count, stride);
            }
        };
        // Always before the game's draw here (FAST_GPU_PASS_MERGE swaps only direct draws).
        VrUiShadowDraw(pipeline, state, VrDrawCall::Of(issue), false);
    }
    if (SkipNoopDraw(pipeline, state, is_indexed, ui_shadow)) {
        // Bit 54: no pass, pipeline or draw; the argument buffers' barriers are already recorded.
        DebugState.IncDrawCall();
        if (vr_warp) {
            VrAfterDraw(pipeline, state);
        }
        ResetBindings();
        return;
    }
    if (GpuPassProfiler* const profiler = GpuPassProfiler::For(scheduler);
        profiler != nullptr && scheduler.WillBeginRendering(state)) [[unlikely]] {
        LabelGpuPass(*profiler, pipeline, state); // FAST_GPU_PASS_PROFILE, bit 38
    }
    scheduler.BeginRendering(state);

    // We can safely ignore both SGPR UD indices and results of fetch shader parsing, as vertex and
    // instance offsets will be automatically applied by Vulkan from indirect args buffer.

    const auto cmdbuf = scheduler.Commands();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, pipeline->Handle());

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                            count_base, max_count, stride);
        } else {
            cmdbuf.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
        }
        DebugState.IncDrawCall();
    } else {
        ASSERT(sizeof(VkDrawIndirectCommand) == stride);

        if (count_address != 0) {
            cmdbuf.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(), count_base,
                                     max_count, stride);
        } else {
            cmdbuf.drawIndirect(buffer->Handle(), base, max_count, stride);
        }
        DebugState.IncDrawCall();
    }
    if (vr_warp) {
        VrAfterDraw(pipeline, state);
    }

    ResetBindings();
}

// FAST_GPU_PASS_PROFILE (bit 38): the label of the game render pass that `state` begins: its
// size, view mask, shaders and target formats (bbport vk_rasterizer.cpp:4666-4699, MarkPass).
void Rasterizer::LabelGpuPass(GpuPassProfiler& profiler, const GraphicsPipeline* pipeline,
                              const RenderState& state) {
    const u64 vs_hash = pipeline->GetStage(Shader::LogicalStage::Vertex).pgm_hash;
    const Shader::Info* const ps =
        pipeline->GetStages()[static_cast<u32>(Shader::LogicalStage::Fragment)];
    const u64 ps_hash = ps != nullptr ? ps->pgm_hash : 0;
    const u32 targets =
        std::min<u32>(static_cast<u32>(std::bit_width(pipeline->GetGraphicsKey().mrt_mask)),
                      AmdGpu::NUM_COLOR_BUFFERS);
    std::array<vk::Format, AmdGpu::NUM_COLOR_BUFFERS> formats{};
    for (u32 cb = 0; cb < targets; ++cb) {
        if (cb_descs[cb].first) {
            formats[cb] = texture_cache.GetImage(cb_descs[cb].first).info.pixel_format;
        }
    }
    const vk::Format depth = db_desc.first
                                 ? texture_cache.GetImage(db_desc.first).info.pixel_format
                                 : vk::Format::eUndefined;
    u64 key = GpuPassProfiler::Mix(0x5CE7'0000'0001ull,
                                   u64{state.width} | u64{state.height} << 16 |
                                       u64{state.probe_view_mask} << 32 |
                                       u64{state.num_layers} << 40);
    key = GpuPassProfiler::Mix(key, vs_hash);
    key = GpuPassProfiler::Mix(key, ps_hash);
    key = GpuPassProfiler::Mix(key, static_cast<u64>(depth));
    for (const vk::Format format : formats) {
        key = GpuPassProfiler::Mix(key, static_cast<u64>(format));
    }
    profiler.LabelNextPass(key, GpuPassProfiler::Group::Scene, [&] {
        std::string colors;
        for (u32 cb = 0; cb < targets; ++cb) {
            colors += cb != 0 ? " " : "";
            colors += formats[cb] != vk::Format::eUndefined ? vk::to_string(formats[cb])
                                                             : std::string{"-"};
        }
        return fmt::format("pass {}x{}{} [{}] depth {} vs {:08x} ps {:08x}", state.width,
                           state.height, state.probe_view_mask != 0 ? " multiview" : "", colors,
                           depth != vk::Format::eUndefined ? vk::to_string(depth)
                                                           : std::string{"-"},
                           vs_hash, ps_hash);
    });
}

// FAST bit 53 (vk_dispatch_eye_overlap.h): what is known before the pairing.
EyeOverlap::Check* Rasterizer::EyeOverlapBegin(const Shader::Info& cs) {
    if (!NativeStereo::Enabled() || !FastRender::On(DispatchEyeOverlap)) {
        return nullptr;
    }
    using EyeOverlap::Reason;
    auto& check = eye_check;
    check.classified = false;
    check.reason = Reason::Taken;
    if (eye_unordered_writes) {
        check.reason = Reason::DrawWrite;
    } else if (cs.uses_dma) {
        check.reason = Reason::RawMemory;
    } else {
        for (const auto& desc : cs.buffers) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer ||
                desc.buffer_type == Shader::BufferType::BdaPagetable ||
                desc.buffer_type == Shader::BufferType::FaultBuffer) {
                check.reason = Reason::SpecialBuffer;
                break;
            }
        }
    }
    check.collect = check.reason == Reason::Taken && EyeOverlap::VerifyPeriod() != 0;
    return &check;
}

bool Rasterizer::EyeOverlapTake(EyeOverlap::Check* check, vk::Buffer left_args,
                                vk::Buffer right_args) {
    if (!check) {
        return false;
    }
    using EyeOverlap::Reason;
    if (check->reason == Reason::Taken && !check->classified) {
        check->reason = Reason::OtherDescriptor; // not paired: nothing is known
    }
    const auto has = [](const auto& list, vk::Buffer buffer) {
        return buffer && std::find(list.begin(), list.end(), buffer) != list.end();
    };
    if (check->reason == Reason::Taken &&
        (has(check->left_written, right_args) || has(check->right_written, left_args))) {
        check->reason = Reason::Indirect;
    }
    EyeOverlap::Note(check->reason);
    return check->reason == Reason::Taken;
}

namespace {
// FAST_DISPATCH_EYE_OVERLAP_VERIFY state, shared by the direct and indirect dispatch paths. Never
// destroyed: its scratch buffers must not outlive the device in a static destructor at exit.
struct EyeOverlapVerifyState {
    std::mutex sample_mutex;
    std::unordered_set<u64> seen;
    u64 taken{};
    std::atomic<bool> in_flight{false}; // cleared by the deferred comparison
    // Replaced only while no sample is in flight; a pending comparison keeps its own references.
    std::shared_ptr<VideoCore::Buffer> saved, old_order, swapped;
};
EyeOverlapVerifyState& EyeOverlapVerifyStateOf() {
    static auto* const state = new EyeOverlapVerifyState{};
    return *state;
}
} // namespace

// FAST_DISPATCH_EYE_OVERLAP_VERIFY (vk_dispatch_eye_overlap.h). Called for a taken dispatch after
// its pipeline, descriptors and pre-barriers are recorded; record_eye(eye) pushes that eye's
// constants and records its dispatch. The caller records the closing barrier.
template <typename RecordEye>
bool Rasterizer::EyeOverlapVerify(const Shader::Info& cs, const RecordEye& record_eye) {
    const u32 period = EyeOverlap::VerifyPeriod();
    if (period == 0) {
        return false;
    }
    auto& state = EyeOverlapVerifyStateOf();
    {
        // The first taken dispatch of every shader, then every period-th.
        std::scoped_lock lock{state.sample_mutex};
        if (!state.seen.insert(cs.pgm_hash).second && ++state.taken % period != 0) {
            return false;
        }
    }
    auto& stats = EyeOverlap::g_stats;
    const auto skip = [&stats] {
        stats.verify_skipped.fetch_add(1, std::memory_order_relaxed);
        return false;
    };
    if (state.in_flight.load(std::memory_order_acquire)) {
        return skip();
    }
    // What the two eyes write: buffer ranges (byte copies) and storage image levels (both layers).
    // region.bufferOffset is the target's place in the scratch buffers.
    struct Target {
        vk::Buffer buffer{};
        vk::DeviceSize offset{};
        vk::Image image{};
        vk::BufferImageCopy region{};
        vk::DeviceSize bytes{};
    };
    boost::container::small_vector<Target, 32> targets;
    vk::DeviceSize total = 0;
    const auto add = [&](Target target, vk::DeviceSize align) {
        total = (total + align - 1) / align * align;
        target.region.bufferOffset = total;
        total += target.bytes;
        targets.push_back(target);
    };
    for (const auto& info : eye_check.written) {
        if (!info.buffer) {
            continue;
        }
        if (info.range == 0 || info.range == VK_WHOLE_SIZE) {
            return skip();
        }
        add(Target{.buffer = info.buffer, .offset = info.offset, .bytes = info.range}, 16);
    }
    for (const auto& [image_id, desc] : image_bindings) {
        if (!image_id || desc.type != VideoCore::TextureCache::BindingType::Storage) {
            continue;
        }
        const auto& image = texture_cache.GetImage(image_id);
        const auto& ci = image.backing->image.image_ci;
        const u32 texel = vk::blockSize(ci.format);
        if (!image.native_stereo || texel == 0 || ci.samples != vk::SampleCountFlagBits::e1 ||
            image.backing->state.layout != vk::ImageLayout::eGeneral ||
            !image.backing->subresource_states.empty()) {
            return skip();
        }
        const auto& range = desc.view_info.range;
        for (u32 level = range.base.level; level < range.base.level + range.extent.levels;
             ++level) {
            const u32 width = std::max(ci.extent.width >> level, 1u);
            const u32 height = std::max(ci.extent.height >> level, 1u);
            for (u32 layer = 0; layer < 2; ++layer) {
                add(Target{.image = image.GetImage(),
                           .region = vk::BufferImageCopy{0, 0, 0,
                                                         vk::ImageSubresourceLayers{
                                                             image.aspect_mask, level, layer, 1},
                                                         vk::Offset3D{0, 0, 0},
                                                         vk::Extent3D{width, height, 1}},
                           .bytes = vk::DeviceSize{width} * height * texel},
                    std::lcm(vk::DeviceSize{16}, vk::DeviceSize{texel}));
            }
        }
    }
    constexpr vk::DeviceSize MaxBytes = 64ull << 20;
    if (targets.empty() || total > MaxBytes) {
        return skip();
    }
    if (!state.saved || state.saved->SizeBytes() < total) {
        const vk::DeviceSize size = std::max<vk::DeviceSize>(total, 1ull << 20);
        state.saved = std::make_shared<VideoCore::Buffer>(
            instance, scheduler, VideoCore::MemoryUsage::DeviceLocal, 0,
            vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst, size);
        state.old_order = std::make_shared<VideoCore::Buffer>(
            instance, scheduler, VideoCore::MemoryUsage::Download, 0,
            vk::BufferUsageFlagBits::eTransferDst, size);
        state.swapped = std::make_shared<VideoCore::Buffer>(
            instance, scheduler, VideoCore::MemoryUsage::Download, 0,
            vk::BufferUsageFlagBits::eTransferDst, size);
    }
    state.in_flight.store(true, std::memory_order_release);
    const vk::Buffer saved = state.saved->Handle();

    const auto cmdbuf = scheduler.Commands();
    const auto barrier = [&] { NativeStereo::Barrier(scheduler); };
    const auto save = [&](vk::Buffer to) {
        for (const auto& target : targets) {
            if (target.image) {
                cmdbuf.copyImageToBuffer(target.image, vk::ImageLayout::eGeneral, to,
                                         target.region);
            } else {
                cmdbuf.copyBuffer(target.buffer, to,
                                  vk::BufferCopy{target.offset, target.region.bufferOffset,
                                                 target.bytes});
            }
        }
    };
    const auto restore = [&](vk::Buffer from) {
        for (const auto& target : targets) {
            if (target.image) {
                cmdbuf.copyBufferToImage(from, target.image, vk::ImageLayout::eGeneral,
                                         target.region);
            } else {
                cmdbuf.copyBuffer(from, target.buffer,
                                  vk::BufferCopy{target.region.bufferOffset, target.offset,
                                                 target.bytes});
            }
        }
    };
    barrier();
    save(saved);
    barrier();
    record_eye(0); // the old order: left, barrier, right
    barrier();
    record_eye(1);
    barrier();
    save(state.old_order->Handle());
    barrier();
    restore(saved);
    barrier();
    record_eye(1); // swapped: right, barrier, left
    barrier();
    record_eye(0);
    barrier();
    save(state.swapped->Handle());
    const vk::MemoryBarrier2 to_host{
        .srcStageMask = vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask = vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmdbuf.pipelineBarrier2(
        vk::DependencyInfo{.memoryBarrierCount = 1, .pMemoryBarriers = &to_host});

    struct Span {
        vk::DeviceSize offset, bytes;
        u32 level, layer;
        bool image;
    };
    std::vector<Span> spans;
    spans.reserve(targets.size());
    for (const auto& target : targets) {
        spans.push_back({target.region.bufferOffset, target.bytes,
                         target.region.imageSubresource.mipLevel,
                         target.region.imageSubresource.baseArrayLayer, bool(target.image)});
    }
    scheduler.DeferOperation([a = state.old_order, b = state.swapped, spans = std::move(spans),
                              total, hash = cs.pgm_hash] {
        auto& stats = EyeOverlap::g_stats;
        auto& in_flight = EyeOverlapVerifyStateOf().in_flight;
        if (a->mapped_data.size() < total || b->mapped_data.size() < total) {
            stats.verify_skipped.fetch_add(1, std::memory_order_relaxed);
            in_flight.store(false, std::memory_order_release);
            return;
        }
        const u8* const old_bytes = a->mapped_data.data();
        const u8* const new_bytes = b->mapped_data.data();
        u64 differing = 0;
        size_t first = spans.size();
        vk::DeviceSize first_byte = 0;
        for (size_t k = 0; k < spans.size(); ++k) {
            const auto& span = spans[k];
            if (std::memcmp(old_bytes + span.offset, new_bytes + span.offset, span.bytes) == 0) {
                continue;
            }
            for (vk::DeviceSize i = 0; i < span.bytes; ++i) {
                if (old_bytes[span.offset + i] != new_bytes[span.offset + i]) {
                    if (first == spans.size()) {
                        first = k;
                        first_byte = i;
                    }
                    ++differing;
                }
            }
        }
        if (first == spans.size()) {
            stats.verify_ok.fetch_add(1, std::memory_order_relaxed);
        } else {
            const u64 n = stats.verify_mismatch.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 16 || n % 256 == 0) {
                const auto& span = spans[first];
                LOG_ERROR(Render_Vulkan,
                          "FAST dispatch_eye_overlap verify: cs {:08x}: {} bytes differ between "
                          "the old and the swapped eye order, first in target {}/{} ({}) at "
                          "byte {}, mismatch #{}",
                          hash, differing, first, spans.size(),
                          span.image ? fmt::format("image level {} layer {}", span.level,
                                                   span.layer)
                                     : std::string{"buffer range"},
                          first_byte, n);
            }
        }
        in_flight.store(false, std::memory_order_release);
    });
    return true;
}

void Rasterizer::DispatchDirect() {
    UpdateDrawWorker();
    FastRender::NoteDispatch();
    ScratchProfile::Cpu scratch_cpu(7, "Rasterizer::DispatchDirect");
    RENDERER_TRACE;
    if (!worker_active) {
        PerfTick();
        scheduler.PopPendingOperations();
    }

    const bool own_camera = worker_active || FastRender::On(FastRender::PacketLoopback);
    const VR::CameraRecord draw_camera = own_camera ? VR::DrawFrameRecord() : VR::CameraRecord{};
    VR::DrawCameraScope camera_scope{own_camera ? &draw_camera : nullptr};
    FastRender::ProducerScope producer_scope{worker_active};
    auto selection_lock = pipeline_cache.LockSelection();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }
    if (DiagSkip::Skip(pipeline->GetStages())) // FAST_DIAG_SKIP_FILE (diagnostic)
        return;

    if (worker_active || FastRender::On(FastRender::PacketLoopback)) {
        RecordPacket(std::move(selection_lock), pipeline, PacketKind::Dispatch);
    } else {
        selection_lock.unlock();
        DispatchDirectRecord(pipeline);
    }
}

void Rasterizer::DispatchDirectRecord(const ComputePipeline* pipeline) {
    FastRender::BProfile::Scope b_detail{FastRender::BProfile::ComputeHle};
    const auto& cs_program = ScratchDraw::ComputeRegisters(liverpool);
    VrNativeAudit(pipeline, nullptr);
    const auto& cs = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (vr_temporal_enabled)
        VrTemporalDispatch(cs.pgm_hash);
    if (ExecuteShaderHLE(cs, std::as_const(ScratchDraw::Registers(liverpool)), cs_program, *this)) {
        return;
    }

    // Bit 55 (IsComputeClearFill) takes direct dispatches only: dim_x is their group count.
    vr_rect_groups = {};
    recording_direct_dispatch = true;
    const bool bound = BindResources(pipeline);
    recording_direct_dispatch = false;
    if (!bound) {
        return;
    }
    // Each eye's pixel rectangle (BindBuffers: NativeStereo::ScreenRectBuffer) may need more
    // workgroups than the flat game's.
    const u32 dim_x = vr_rect_groups[0] ? vr_rect_groups[0] : cs_program.dim_x;
    const u32 dim_y = vr_rect_groups[1] ? vr_rect_groups[1] : cs_program.dim_y;
    if (vr_ui.armed) {
        VrUiNoteDispatch();
    }

    scheduler.EndRendering();
    if (GpuPassProfiler* const profiler = GpuPassProfiler::For(scheduler)) [[unlikely]] {
        // bbport vk_rasterizer.cpp:1978-1987: after the binding (its transfers mark themselves).
        profiler->Mark(GpuPassProfiler::Mix(0xD15A'0000'0001ull, cs.pgm_hash),
                       GpuPassProfiler::Group::Dispatch, [&] {
                           return fmt::format("dispatch cs {:08x} ({}x{}x{} groups at first "
                                              "sight{})",
                                              cs.pgm_hash, cs_program.dim_x, cs_program.dim_y,
                                              cs_program.dim_z,
                                              NativeStereo::Enabled() ? ", per eye" : "");
                       });
    }
    FastRender::BProfile::Scope b_recording{FastRender::BProfile::Recording};
    EyeOverlap::Check* const eye_overlap = EyeOverlapBegin(cs); // bit 53, null when off
    // Compute pushes PushData alone (no VR values after it).
    pipeline->BindResources(set_writes, buffer_barriers, push_data, {}, NativeHandles(),
                            eye_overlap);

    const auto cmdbuf = scheduler.Commands();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    scheduler.GpuWorkBegin(cs.pgm_hash, cs_program.dim_x, cs_program.dim_y); // VR_GPU_PASSES
    const bool eyes_overlap = EyeOverlapTake(eye_overlap);
    const auto record_eye = [&](u32 eye) {
        auto eye_push = push_data;
        if (eye != 0) {
            eye_push.vr_hud = 1.0f;
        }
        ScratchPush::Invalidate();
        cmdbuf.pushConstants(pipeline->GetLayout(), vk::ShaderStageFlagBits::eCompute, 0u,
                             sizeof(eye_push), &eye_push);
        cmdbuf.dispatch(dim_x, dim_y, cs_program.dim_z);
    };
    if (eyes_overlap && EyeOverlapVerify(cs, record_eye)) {
        NativeStereo::Barrier(scheduler);
    } else {
        cmdbuf.dispatch(dim_x, dim_y, cs_program.dim_z);
        if (NativeStereo::Enabled()) {
            if (!eyes_overlap) {
                NativeStereo::Barrier(scheduler);
            }
            auto right_push = push_data;
            right_push.vr_hud = 1.0f;
            ScratchPush::Invalidate();
            cmdbuf.pushConstants(pipeline->GetLayout(), vk::ShaderStageFlagBits::eCompute, 0u,
                                 sizeof(right_push), &right_push);
            cmdbuf.dispatch(dim_x, dim_y, cs_program.dim_z);
            NativeStereo::Barrier(scheduler);
        }
    }
    if (NativeStereo::Enabled()) {
        eye_unordered_writes = false; // everything before is ordered by the closing barrier
    }
    scheduler.GpuWorkEnd();
    DebugState.IncDispatch();
    if (clear_fill_sample.pending) [[unlikely]] {
        FinishClearFillVerify(); // bit 55's sampled dispatch has run
    }

    ResetBindings();
}

void Rasterizer::DispatchIndirect(VAddr address, u32 offset, u32 size) {
    UpdateDrawWorker();
    FastRender::NoteDispatch();
    RENDERER_TRACE;
    if (!worker_active) {
        PerfTick();
        scheduler.PopPendingOperations();
    }

    const bool own_camera = worker_active || FastRender::On(FastRender::PacketLoopback);
    const VR::CameraRecord draw_camera = own_camera ? VR::DrawFrameRecord() : VR::CameraRecord{};
    VR::DrawCameraScope camera_scope{own_camera ? &draw_camera : nullptr};
    FastRender::ProducerScope producer_scope{worker_active};
    auto selection_lock = pipeline_cache.LockSelection();
    const ComputePipeline* pipeline = pipeline_cache.GetComputePipeline();
    if (!pipeline) {
        return;
    }
    if (DiagSkip::Skip(pipeline->GetStages())) // FAST_DIAG_SKIP_FILE (diagnostic)
        return;

    if (worker_active || FastRender::On(FastRender::PacketLoopback)) {
        RecordPacket(std::move(selection_lock), pipeline, PacketKind::DispatchIndirect, false,
                     address, offset, size);
    } else {
        selection_lock.unlock();
        DispatchIndirectRecord(pipeline, address, offset, size);
    }
}

void Rasterizer::DispatchIndirectRecord(const ComputePipeline* pipeline, VAddr address, u32 offset,
                                        u32 size) {
    ScratchCapturedInputs captured_inputs{pipeline};
    if (vr_temporal_enabled)
        VrTemporalDispatch(pipeline->GetStage(Shader::LogicalStage::Compute).pgm_hash);
    if (!BindResources(pipeline)) {
        return;
    }
    if (vr_ui.armed) {
        VrUiNoteDispatch();
    }

    const auto [buffer, base] = buffer_cache.ObtainBuffer(address + offset, size, false);

    if (auto barrier = buffer->GetBarrier(vk::AccessFlagBits2::eIndirectCommandRead,
                                          vk::PipelineStageFlagBits2::eDrawIndirect)) {
        buffer_barriers.emplace_back(*barrier);
    }

    scheduler.EndRendering();
    if (GpuPassProfiler* const profiler = GpuPassProfiler::For(scheduler)) [[unlikely]] {
        const u64 cs_hash = pipeline->GetStage(Shader::LogicalStage::Compute).pgm_hash;
        profiler->Mark(GpuPassProfiler::Mix(0xD15A'0000'0002ull, cs_hash),
                       GpuPassProfiler::Group::Dispatch, [&] {
                           return fmt::format("dispatch indirect cs {:08x}{}", cs_hash,
                                              NativeStereo::Enabled() ? " (per eye)" : "");
                       });
    }
    FastRender::BProfile::Scope b_recording{FastRender::BProfile::Recording};
    const auto& cs = pipeline->GetStage(Shader::LogicalStage::Compute);
    EyeOverlap::Check* const eye_overlap = EyeOverlapBegin(cs); // bit 53, null when off
    // Compute pushes PushData alone (no VR values after it).
    pipeline->BindResources(set_writes, buffer_barriers, push_data, {}, NativeHandles(),
                            eye_overlap);

    const auto cmdbuf = scheduler.Commands();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline->Handle());
    scheduler.GpuWorkBegin(cs.pgm_hash, 0, 0); // VR_GPU_PASSES
    // The right eye's arguments: the right copy of the argument buffer when it has one. Never
    // created here, so looking it up before the left dispatch gives the same handle.
    const vk::Buffer left_args = buffer->Handle();
    const vk::Buffer right_args = NativeStereo::Enabled()
                                      ? NativeStereo::RightBuffer(scheduler, left_args, false)
                                      : vk::Buffer{};
    const bool eyes_overlap = EyeOverlapTake(eye_overlap, left_args, right_args);
    const auto record_eye = [&](u32 eye) {
        auto eye_push = push_data;
        if (eye != 0) {
            eye_push.vr_hud = 1.0f;
        }
        ScratchPush::Invalidate();
        cmdbuf.pushConstants(pipeline->GetLayout(), vk::ShaderStageFlagBits::eCompute, 0u,
                             sizeof(eye_push), &eye_push);
        cmdbuf.dispatchIndirect(eye != 0 ? right_args : left_args, base);
    };
    if (eyes_overlap && EyeOverlapVerify(cs, record_eye)) {
        NativeStereo::Barrier(scheduler);
    } else {
        cmdbuf.dispatchIndirect(left_args, base);
        if (NativeStereo::Enabled()) {
            if (!eyes_overlap) {
                NativeStereo::Barrier(scheduler);
            }
            auto right_push = push_data;
            right_push.vr_hud = 1.0f;
            ScratchPush::Invalidate();
            cmdbuf.pushConstants(pipeline->GetLayout(), vk::ShaderStageFlagBits::eCompute, 0u,
                                 sizeof(right_push), &right_push);
            cmdbuf.dispatchIndirect(right_args, base);
            NativeStereo::Barrier(scheduler);
        }
    }
    if (NativeStereo::Enabled()) {
        eye_unordered_writes = false; // everything before is ordered by the closing barrier
    }
    scheduler.GpuWorkEnd();
    DebugState.IncDispatch();

    ResetBindings();
}

u64 Rasterizer::Flush() {
    DrainDrawPipe(DrawPipe::RasterizerEntry);
    const u64 current_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return current_tick;
}

void Rasterizer::FlushFrameEnd() {
    DrainDrawPipe(DrawPipe::RasterizerEntry);
    scheduler.FlushFrameEnd();
}

void Rasterizer::Finish() {
    DrainDrawPipe(DrawPipe::RasterizerEntry);
    scheduler.Finish();
}

// FAST bit 62 (gpu_plan.md #9; research_gpu_gaps.md item 1 and its adversarial check): a
// two-level mid-frame cadence. The old rule cuts at the first render-pass switch after
// gpu_submit_ms (1.5) of recording. With the bit that cut is taken only while the GPU has finished
// everything submitted (it would otherwise sit idle until the cut); while it still has work, the
// cut waits for FAST_SUBMIT_CADENCE_BUSY_MS (3.5) of recording, checking the GPU again at every
// later pass switch. So a GPU-bound frame has fewer queue boundaries (20-47 us of GPU gap each at
// 2000x2160) and a CPU-bound one is fed as before. Never more submissions than the old rule: every
// cut taken is one it takes too (a pass switch past gpu_submit_ms). A cut whose command buffer
// carries guest signals is never deferred (they complete only after its submission; not so with
// FAST_CPU_SIGNAL_AT_REPLAY, vk_scheduler.h, where the replay writes them unsubmitted). A deferral
// drops no fence, semaphore or timeline signal: the open tick is signalled by the next submission
// (the cadence, a sync-driven flush, Wait()'s forced flush or the frame end), with the same
// commands in the same order. bbport: SubmitForSignalsNow (BBV vk_rasterizer.cpp:380-415), the
// same idle/batch split for its guest-signal flushes. Read per pass switch: it may flip any time.
static constexpr auto SubmitCadence = static_cast<FastRender::Bit>(62);

namespace {
float SubmitCadenceBusyMs() {
    static const float ms = [] {
        const char* value = std::getenv("FAST_SUBMIT_CADENCE_BUSY_MS");
        const float parsed = value ? std::strtof(value, nullptr) : 3.5f;
        return std::clamp(parsed > 0.0f ? parsed : 3.5f, 0.5f, 16.0f);
    }();
    return ms;
}
// The GPU's semaphore is queried (a driver call) at most this often while cuts are deferred;
// in between, the progress known from other queries decides.
std::chrono::steady_clock::duration SubmitCadencePoll() {
    static const std::chrono::steady_clock::duration poll = std::chrono::microseconds([] {
        const char* value = std::getenv("FAST_SUBMIT_CADENCE_POLL_US");
        return value ? std::strtoll(value, nullptr, 10) : 50ll;
    }());
    return poll;
}
// FAST_SUBMIT_CADENCE_VERIFY=N: every Nth deferral queries the semaphore regardless of the poll
// interval; a GPU found idle after all (a cut the idle rule should have taken: a starved GPU, not
// a wrong image) is counted, logged and the cut taken. The deferral itself changes only where the
// command buffer ends; whether a waiter needed the open tick sooner shows as on_deferred_tick.
u64 SubmitCadenceVerifyEvery() {
    static const u64 every = [] {
        const char* value = std::getenv("FAST_SUBMIT_CADENCE_VERIFY");
        return value ? std::strtoull(value, nullptr, 0) : 0ULL;
    }();
    return every;
}
} // namespace

void Rasterizer::MaybeSubmitMidFrame(const RenderState& next_state) {
    FastRender::BProfile::Scope b_detail{FastRender::BProfile::Submit};
    const float submit_ms = VR::GetSettings().gpu_submit_ms;
    if (submit_ms <= 0.0f) {
        return;
    }
    if (scheduler.Continues(next_state)) {
        return; // the pass goes on (the same state, or one bit 43 continues): never split one
    }
    const auto recorded = scheduler.RecordingTime();
    if (recorded < std::chrono::duration<float, std::milli>(submit_ms)) {
        return;
    }
    auto& s = submit_cadence;
    MasterSemaphore& semaphore = *scheduler.GetMasterSemaphore();
    const u64 last_tick = scheduler.CurrentTick() - 1; // the last one submitted
    if (!FastRender::On(SubmitCadence)) {
        ++s.old_cuts;
        // The GPU's last known progress only: no driver query, the old path stays as it was.
        if (semaphore.IsFree(last_tick)) {
            ++s.old_idle_known;
        }
    } else if (recorded >= std::chrono::duration<float, std::milli>(
                               std::max(SubmitCadenceBusyMs(), submit_ms))) {
        ++s.cap;
    } else if (!g_signal_at_replay && scheduler.SignalsSinceSubmit()) {
        // Fallback: guest writes/IRQs complete only after this submission. Not with
        // FAST_CPU_SIGNAL_AT_REPLAY (the replay writes them on its own); with its _VERIFY these
        // are the cuts it would have weighed as any other.
        ++s.signals;
    } else if (semaphore.IsFree(last_tick) || [&] {
                   const auto now = std::chrono::steady_clock::now();
                   if (now - cadence_poll < SubmitCadencePoll()) {
                       return false;
                   }
                   cadence_poll = now;
                   ++s.polls;
                   semaphore.Refresh();
                   return semaphore.IsFree(last_tick);
               }()) {
        ++s.idle; // the GPU has run dry: feed it now, as the old rule does
    } else {
        const u64 every = SubmitCadenceVerifyEvery();
        bool idle_after_all = false;
        if (every != 0 && ++s.verify_sequence % every == 0) {
            ++s.verified;
            semaphore.Refresh();
            idle_after_all = semaphore.IsFree(last_tick);
        }
        if (!idle_after_all) {
            ++s.deferred;
            scheduler.NoteDeferredCut();
            return;
        }
        ++s.verify_idle;
        static u32 logged = 0; // recording thread only
        if (logged++ < 8) {
            LOG_WARNING(Render_Vulkan,
                        "FAST submit_cadence verify: a cut was deferred while the GPU had finished "
                        "tick {} (recorded {:.3f} ms; poll interval {} us); taken",
                        last_tick,
                        std::chrono::duration<double, std::milli>(recorded).count(),
                        std::chrono::duration_cast<std::chrono::microseconds>(SubmitCadencePoll())
                            .count());
        }
        ++s.idle;
    }
    scheduler.Flush();
    ++midframe_submits;
}

void Rasterizer::ReportSubmitCadence() {
    auto& s = submit_cadence;
    const auto waits = scheduler.TakeOpenTickWaits();
    const double frames = s.frames ? static_cast<double>(s.frames) : 1.0;
    const u64 eligible = s.idle + s.cap + s.signals + s.deferred;
    LOG_INFO(Render_Vulkan,
             "FAST submit_cadence: bit={} frames={} submits/f={:.2f} midframe/f={:.2f}; bit off: "
             "cuts={} (gpu known idle {}); bit on: eligible={} (pass switches past gpu_submit_ms) "
             "taken idle={} cap={} fallback signals={} deferred={} ({:.2f}/f) polls/f={:.2f} "
             "busy_ms={:.2f}; open_tick_flushes={} on_deferred_tick={}; verify={} verify_idle={}",
             FastRender::On(SubmitCadence) ? 1 : 0, s.frames, s.submits / frames,
             s.midframe / frames, s.old_cuts, s.old_idle_known, eligible, s.idle, s.cap, s.signals,
             s.deferred, s.deferred / frames, s.polls / frames, SubmitCadenceBusyMs(), waits.flushes,
             waits.deferred, s.verified, s.verify_idle);
    const u64 sequence = s.verify_sequence;
    s = {};
    s.verify_sequence = sequence;
}

void Rasterizer::OnSubmit() {
    DrainDrawPipe(DrawPipe::RasterizerEntry);
    Perf::Refresh(); // vr.ini / VR_TUNE_FILE perf_* switches, the VR_PERF_CHECK line
    // VR_STREAM_STATS=1: the stream buffer's uploads per game frame, every 300 frames.
    static const bool stream_stats = std::getenv("VR_STREAM_STATS") != nullptr;
    if (stream_stats) {
        static u64 frames = 0, bytes = 0, calls = 0;
        const auto [b, c] =
            buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream).TakeCopyStats();
        bytes += b;
        calls += c;
        static u64 submits = 0, tiles = 0, tile_creates = 0;
        submits += midframe_submits;
        const auto [tu, tc] = texture_cache.GetTileManager().TakeScratchStats();
        tiles += tu;
        tile_creates += tc;
        if (++frames == 300) {
            LOG_INFO(Render_Vulkan,
                     "VR stream: {:.2f} MB in {} copies per game frame (avg {} B); mid-frame "
                     "submits per game frame {:.1f}; (de)tiling scratch buffers per game frame "
                     "{:.1f} used, {:.2f} created",
                     double(bytes) / frames / (1 << 20), calls / frames, calls ? bytes / calls : 0,
                     double(submits) / frames, double(tiles) / frames,
                     double(tile_creates) / frames);
            frames = bytes = calls = submits = tiles = tile_creates = 0;
        }
    }
    {
        // FAST bit 62: this game frame's submissions (the frame-end flush that follows counts in
        // the next one) and mid-frame cuts, every 600 game frames.
        auto& s = submit_cadence;
        const u64 count = scheduler.SubmitCount();
        s.submits += count - submit_count_mark;
        submit_count_mark = count;
        s.midframe += midframe_submits;
        if (++s.frames == 600) {
            ReportSubmitCadence();
        }
    }
    midframe_submits = 0;
    if (fault_process_pending) {
        fault_process_pending = false;
        buffer_cache.ProcessFaultBuffer();
    }
    texture_cache.ProcessDownloadImages();
    texture_cache.RunGarbageCollector();
    buffer_cache.RunGarbageCollector();
}

// Fast bit 25: native stereo right handles carried through binding instead of registry lookups
// in PairDescriptors / per barrier (reader_4 2b/2c). bbport keeps resolved views in its binding
// memo the same way (BB vk_rasterizer.cpp:3593-3677 BindTexturesFromSet).
static constexpr auto StereoHandles = static_cast<FastRender::Bit>(25);

const NativeStereo::Handles* Rasterizer::NativeHandles() {
    if (!stereo_handles)
        return nullptr;
    native_handles.buffer_base = buffer_infos.data();
    native_handles.buffers = {buffer_hints.data(), buffer_hints.size()};
    native_handles.image_base = image_infos.data();
    native_handles.images = {view_hints.data(), view_hints.size()};
    native_handles.barriers = {barrier_hints.data(), barrier_hints.size()};
    native_handles.buffer_count = buffer_infos.size();
    native_handles.image_count = image_infos.size();
    return &native_handles;
}

// FAST_RESOURCE_PREP_FORCE_FALLBACK=N, read once at start-up (BindResources asks per stage; no
// function-local static's guard).
static const u64 g_resource_prep_force_fallback_every = [] {
    const char* v = std::getenv("FAST_RESOURCE_PREP_FORCE_FALLBACK");
    return v ? std::strtoull(v, nullptr, 0) : 0ULL;
}();

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    resource_bindings = nullptr;
    FastRender::BProfile::Scope b_profile{FastRender::BProfile::Resources};
    AssertRecordingOwner();
    ScratchProfile::Cpu scratch_cpu(8, "Rasterizer::BindResources");
    // Bit 33 (BufferCache::SmallReadDirect), decided once per draw or dispatch (the bit may flip
    // between packets): small read-only ranges are bound in place only when no stage writes
    // buffers or raw memory, which could write in this same draw what one of its bindings reads
    // in place (the stream copy is a snapshot).
    // Bit 53 (vk_dispatch_eye_overlap.h): such a draw also keeps the next game dispatch's middle
    // eye barrier; noted under native stereo whatever the bit, so the bit may flip at any time.
    const bool small_read_direct = FastRender::On(VideoCore::BufferCache::SmallReadDirect);
    const bool note_eye_writes = NativeStereo::Enabled() && !pipeline->IsCompute();
    const bool writes_memory = (small_read_direct || note_eye_writes) && [pipeline] {
        for (const auto* stage : pipeline->GetStages()) {
            if (!stage) {
                continue;
            }
            if (stage->uses_dma) {
                return true;
            }
            for (const auto& desc : stage->buffers) {
                if (desc.is_written) {
                    return true;
                }
            }
        }
        return false;
    }();
    buffer_cache.SetSmallReadDirect(small_read_direct && !writes_memory);
    // Bit 53 also needs a draw's storage image writes (bit 33 does not: they are not buffers). An
    // image bound both sampled and written takes TransitTexture's force_general path, whose
    // tracked access (ShaderRead | ColorAttachment*) has no write flag, so a later game dispatch
    // binding it the same way gets no transition barrier: only the middle eye barrier then orders
    // the draw's layer-1 accesses before the right dispatch.
    if (note_eye_writes && !eye_unordered_writes &&
        (writes_memory || [pipeline] {
            for (const auto* stage : pipeline->GetStages()) {
                if (!stage) {
                    continue;
                }
                for (const auto& image : stage->images) {
                    if (image.is_written) {
                        return true;
                    }
                }
            }
            return false;
        }())) {
        eye_unordered_writes = true;
    }
    if (IsComputeImageCopy(pipeline) || IsComputeMetaClear(pipeline) ||
        IsComputeImageClear(pipeline) || IsComputeClearFill(pipeline)) {
        return false;
    }

    set_write_index = 0;
    set_writes.clear();
    buffer_barriers.clear();
    buffer_infos.clear();
    NativeStereo::ClearCameraPairs();
    image_infos.clear();
    // Bit 25, decided once per draw. Generations read before any entry is carried: a registry
    // change after this point makes PairDescriptors ignore the hints.
    stereo_handles = NativeStereo::Enabled() && FastRender::On(StereoHandles);
    if (stereo_handles) {
        buffer_hints.clear();
        view_hints.clear();
        barrier_hints.clear();
        native_handles.buffer_generation = NativeStereo::BufferGeneration();
        native_handles.view_generation = NativeStereo::ViewGeneration();
        // FAST_CPU_FUSED_PAIR: the buffer hints also carry each descriptor's own camera pair,
        // and pairing takes the hints alone (NativeStereo::PairCarried).
        native_handles.fused = NativeStereo::FusedPair();
    }

    // The resync memo (vr_perf.h): a new draw's bindings. A raw-memory (DMA) shader may write any
    // buffer: every memo is invalid from here.
    Perf::NextDraw();
    for (const auto* stage : pipeline->GetStages()) {
        if (stage && stage->uses_dma) {
            Perf::NoteDma();
            break;
        }
    }

    bool uses_dma = false;
    const bool perf_stats = PerfStats();

    // FAST_GPU_MIRROR_FLIP: this draw's scene camera is noted afresh (VrMirrorCamera).
    vr_mirror_pipeline = pipeline;
    vr_mirror_noted = vr_mirror_looked = false;

    // Bind resource buffers and textures.
    Shader::Backend::Bindings binding{};
    push_data = MakeUserData(std::as_const(ScratchDraw::Registers(liverpool)));
    for (const auto* stage : pipeline->GetStages()) {
        if (!stage) {
            continue;
        }
        set_writes.resize(set_writes.size() + stage->buffers.size() + stage->images.size() +
                          stage->samplers.size());
        if (Perf::Probe()) [[unlikely]] {
            Perf::SetContext(stage->pgm_hash);
        }
        resource_bindings = nullptr;
        if (binding_packet && binding_packet->resources_prepared) {
            // A stereo pipeline reselection may change resource layout: take the old path.
            const u64 force_fallback_every = g_resource_prep_force_fallback_every;
            const bool forced = force_fallback_every &&
                                binding_packet->sequence % force_fallback_every == 0;
            if (pipeline == binding_packet->selected && !forced) {
                for (size_t i = 0; i < binding_packet->stage_count; ++i) {
                    if (binding_packet->owned[i].owner != stage)
                        continue;
                    resource_bindings = &binding_packet->resources[i];
                    if (binding_packet->verify) {
                        PreparedResources reference;
                        reference.Build(*stage);
                        ASSERT_MSG(resource_bindings->Matches(reference),
                                   "FAST prepared resources differ from owned stage inputs");
                        ++resource_prepare_verified;
                    }
                    break;
                }
            }
            resource_prepare_hits += resource_bindings != nullptr;
            resource_prepare_fallbacks += resource_bindings == nullptr;
            if ((resource_prepare_hits + resource_prepare_fallbacks) % 65536 == 0)
                LOG_INFO(Render_Vulkan,
                         "FAST prepared resources: used={} fallback={} verified={}",
                         resource_prepare_hits, resource_prepare_fallbacks,
                         resource_prepare_verified);
        }
        stage->PushUd(binding, push_data);
        BindBuffers(*stage, binding, push_data);
        if (perf_stats) {
            // Statistics only: BindTextures' wall time by the bind memo's state (perf_bind_memo
            // A/B inside one launch; logged every 5 s in PerfRefresh).
            const bool memo_on = PerfMemo::g_switches.Get().bind_memo;
            const auto t0 = std::chrono::steady_clock::now();
            BindTextures(*stage, binding);
            auto& bt = bind_time[memo_on ? 1 : 0];
            bt.ns += u64(std::chrono::duration_cast<std::chrono::nanoseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count());
            ++bt.calls;
        } else {
            BindTextures(*stage, binding);
        }
        uses_dma |= stage->uses_dma;
    }
    resource_bindings = nullptr;

    if (perf_stats) {
        // Statistics only: the pipeline and every stage's flattened user data (the user data and
        // every sharp it points at) equal to the draw before's.
        perf_cur_res.clear();
        if (pipeline)
            for (const auto* stage : pipeline->GetStages()) {
                if (stage) {
                    perf_cur_res.push_back(u32(stage->FlatUserData().size()));
                    perf_cur_res.insert(perf_cur_res.end(), stage->FlatUserData().begin(),
                                        stage->FlatUserData().end());
                }
            }
        ++perf_res_total;
        if (pipeline == perf_prev_pipeline && perf_cur_res == perf_prev_res) {
            ++perf_res_same;
        }
        perf_prev_pipeline = pipeline;
        std::swap(perf_prev_res, perf_cur_res);
    }

    if (uses_dma) {
        // We only use fault buffer for DMA right now.
        Common::RecursiveSharedLock lock{mapped_ranges_mutex};
        for (auto& range : mapped_ranges) {
            buffer_cache.SynchronizeBuffersInRange(range.lower(), range.upper() - range.lower());
        }
        fault_process_pending = true;
    }

    return true;
}

Rasterizer::CopyBuffers Rasterizer::ObtainCopyBuffers(VAddr src_addr, u32 src_size, VAddr dst_addr,
                                                      u32 dst_size) {
    if (!PerfMemo::g_switches.Get().copy_memo) {
        const auto [src, src_offset] = buffer_cache.ObtainBuffer(src_addr, src_size, false);
        const auto [dst, dst_offset] = buffer_cache.ObtainBuffer(dst_addr, dst_size, true);
        return {src, src_offset, dst, dst_offset};
    }
    // perf_copy_memo (vk_perf_memo.h CopyEntry): every epoch read BEFORE the full pass below; an
    // entry is found only while all of them are still the ones read before the pass that made
    // it: no buffer registered or unregistered, no GPU-modified range dropped, no page of either
    // range's 256 KB tracking words changed its CPU or GPU state since (a CPU write is noticed by
    // its write fault before it lands, under the tracker's lock).
    const PerfMemo::CopyKey key{
        .src_addr = src_addr, .dst_addr = dst_addr, .src_size = src_size, .dst_size = dst_size};
    const u32 memo_epoch = PerfMemo::Epoch();
    const u64 cache_epoch = buffer_cache.CopyMemoEpoch();
    const u64 src_track = buffer_cache.TrackingEpoch(src_addr, src_size);
    const u64 dst_track = buffer_cache.TrackingEpoch(dst_addr, dst_size);
    const PerfMemo::CopyEntry* found =
        copy_memo.Find(key, cache_epoch, src_track, dst_track, memo_epoch);
    if (found && !PerfMemo::CheckMode()) {
        ++copy_counters.hits;
        return {&buffer_cache.GetBuffer(VideoCore::BufferId{found->src_buffer}), found->src_offset,
                &buffer_cache.GetBuffer(VideoCore::BufferId{found->dst_buffer}), found->dst_offset};
    }

    // VR_PERF_CHECK=1 with an entry: the full pass must have nothing to do (no CPU-modified page
    // to upload in either range, every destination page already GPU-modified and in the range
    // set) and must find the remembered buffers and offsets. Inconclusive (not counted) when a
    // page changed state while it was being checked: a race with a CPU write, not a mismatch.
    bool idle = true;
    bool conclusive = true;
    if (found) {
        idle = !buffer_cache.IsRegionCpuModified(src_addr, src_size) &&
               !buffer_cache.IsRegionCpuModified(dst_addr, dst_size) &&
               buffer_cache.IsRangeFullyGpuModified(dst_addr, dst_size);
        conclusive = buffer_cache.CopyMemoEpoch() == cache_epoch &&
                     buffer_cache.TrackingEpoch(src_addr, src_size) == src_track &&
                     buffer_cache.TrackingEpoch(dst_addr, dst_size) == dst_track;
    }
    const auto [src, src_offset] = buffer_cache.ObtainBuffer(src_addr, src_size, false);
    const auto [dst, dst_offset] = buffer_cache.ObtainBuffer(dst_addr, dst_size, true);
    if (found) {
        ++copy_counters.hits;
        if (!conclusive) {
            ++copy_inconclusive;
        } else {
            ++copy_counters.checked;
            const auto* m_src = &buffer_cache.GetBuffer(VideoCore::BufferId{found->src_buffer});
            const auto* m_dst = &buffer_cache.GetBuffer(VideoCore::BufferId{found->dst_buffer});
            if ((!idle || m_src != src || found->src_offset != src_offset || m_dst != dst ||
                 found->dst_offset != dst_offset) &&
                copy_counters.NoteMismatch()) {
                LOG_WARNING(
                    Render_Vulkan,
                    "VR perf check: copy memo mismatch #{}: src {:#x}+{:#x} dst {:#x}+{:#x}: "
                    "work left {} (src CPU-modified {}, dst CPU-modified {}, dst not all "
                    "GPU-modified {}), src buffer {} offset {}/{}, dst buffer {} offset "
                    "{}/{}",
                    copy_counters.total_mismatches, src_addr, src_size, dst_addr, dst_size, !idle,
                    buffer_cache.IsRegionCpuModified(src_addr, src_size),
                    buffer_cache.IsRegionCpuModified(dst_addr, dst_size),
                    !buffer_cache.IsRangeFullyGpuModified(dst_addr, dst_size),
                    m_src == src ? "same" : "differs", found->src_offset, src_offset,
                    m_dst == dst ? "same" : "differs", found->dst_offset, dst_offset);
            }
        }
    } else {
        ++copy_counters.misses;
    }

    // Remember cache buffers only (not the stream buffer a small read-only range is copied into
    // again each time) of ranges the tracker covers.
    const VideoCore::BufferId src_id = buffer_cache.BufferIdAt(src_addr);
    const VideoCore::BufferId dst_id = buffer_cache.BufferIdAt(dst_addr);
    const bool stored = src_size != 0 && dst_size != 0 && src_id && dst_id &&
                        &buffer_cache.GetBuffer(src_id) == src &&
                        &buffer_cache.GetBuffer(dst_id) == dst &&
                        copy_memo.Store({.key = key,
                                         .cache_epoch = cache_epoch,
                                         .src_track = src_track,
                                         .dst_track = dst_track,
                                         .memo_epoch = memo_epoch,
                                         .src_buffer = src_id.index,
                                         .dst_buffer = dst_id.index,
                                         .src_offset = src_offset,
                                         .dst_offset = dst_offset});
    copy_unmemo += stored ? 0 : 1;
    return {src, src_offset, dst, dst_offset};
}

// Read once at start-up (BindResources asks per draw; no function-local static's guard).
static const bool g_perf_stats =
    PerfMemo::g_check_mode ||
    (std::getenv("VR_PERF_STATS") != nullptr && std::getenv("VR_PERF_STATS")[0] == '1');
bool Rasterizer::PerfStats() noexcept {
    return g_perf_stats;
}

void Rasterizer::PerfRefresh() {
    const s64 now = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    if (now - perf_refreshed_ms >= 250) {
        perf_refreshed_ms = now;
        const auto& vs = VR::GetSettings();
        PerfMemo::Switches next{.spec_memo = vs.perf_spec_memo,
                                .rt_memo = vs.perf_rt_memo,
                                .bind_memo = vs.perf_bind_memo,
                                .copy_memo = vs.perf_copy_memo};
        if (VR::Tune::Active()) {
            // Live A/B inside one run (tests): any toggle drops every memo (the epoch).
            next.spec_memo = VR::Tune::Get("perf.spec_memo", next.spec_memo ? 1.0f : 0.0f) != 0.0f;
            next.rt_memo = VR::Tune::Get("perf.rt_memo", next.rt_memo ? 1.0f : 0.0f) != 0.0f;
            next.bind_memo = VR::Tune::Get("perf.bind_memo", next.bind_memo ? 1.0f : 0.0f) != 0.0f;
            next.copy_memo = VR::Tune::Get("perf.copy_memo", next.copy_memo ? 1.0f : 0.0f) != 0.0f;
        }
        // Logged at the first refresh (the state in force, on by default) and at every change.
        static bool logged_once = false;
        const bool changed = PerfMemo::g_switches.Apply(next);
        if (changed || !logged_once) {
            logged_once = true;
            LOG_INFO(Render_Vulkan,
                     "VR perf: spec_memo {} rt_memo {} bind_memo {} copy_memo {} (epoch {})",
                     next.spec_memo ? 1 : 0, next.rt_memo ? 1 : 0, next.bind_memo ? 1 : 0,
                     next.copy_memo ? 1 : 0, PerfMemo::Epoch());
        }
    }
    if (!PerfStats() || now - perf_logged_ms < 5000) {
        return;
    }
    const auto selection_lock = pipeline_cache.LockSelection();
    if (perf_logged_ms != 0) {
        const auto& sc = pipeline_cache.spec_counters;
        const auto& sw = PerfMemo::g_switches.Get();
        LOG_INFO(
            Render_Vulkan,
            "VR perf check: spec memo {} hits / {} misses ({:.1f}%), checked {}, mismatches {} "
            "(total {}); rt memo {} / {} ({:.1f}%), checked {}, mismatches {} (total {}); same "
            "pipeline and resources as the draw before: {} of {} ({:.1f}%); switches spec {} "
            "rt {}{}",
            sc.hits, sc.misses, sc.HitRate() * 100.0, sc.checked, sc.mismatches,
            sc.total_mismatches, rt_counters.hits, rt_counters.misses,
            rt_counters.HitRate() * 100.0, rt_counters.checked, rt_counters.mismatches,
            rt_counters.total_mismatches, perf_res_same, perf_res_total,
            perf_res_total ? 100.0 * double(perf_res_same) / double(perf_res_total) : 0.0,
            sw.spec_memo ? 1 : 0, sw.rt_memo ? 1 : 0,
            PerfMemo::CheckMode() ? " [VR_PERF_CHECK]" : "");
        const auto& cs = copy_stats;
        LOG_INFO(Render_Vulkan,
                 "VR perf check: bind memo {} hits / {} misses ({:.1f}%), checked {}, mismatches "
                 "{} (total {}), invalid {}, not rememberable {}; switch bind {}; copy hle {} "
                 "calls, {} copies in {} batches, src {:.2f} MB (batches with CPU-modified pages "
                 "{}, GPU-modified {}), dst {:.2f} MB (CPU-modified {}, GPU-modified {}), repeats "
                 "{}, {:.3f} ms{}",
                 bind_counters.hits, bind_counters.misses, bind_counters.HitRate() * 100.0,
                 bind_counters.checked, bind_counters.mismatches, bind_counters.total_mismatches,
                 bind_invalid, bind_uncacheable, sw.bind_memo ? 1 : 0, cs.calls, cs.copies,
                 cs.batches, double(cs.src_bytes) / (1 << 20), cs.src_cpu_mod, cs.src_gpu_mod,
                 double(cs.dst_bytes) / (1 << 20), cs.dst_cpu_mod, cs.dst_gpu_mod, cs.repeats,
                 cs.us / 1000.0, PerfMemo::CheckMode() ? " [VR_PERF_CHECK]" : "");
        LOG_INFO(Render_Vulkan,
                 "VR perf check: copy memo {} hits / {} misses ({:.1f}%), checked {}, mismatches "
                 "{} (total {}), inconclusive {}, not rememberable {}; switch copy {}{}",
                 copy_counters.hits, copy_counters.misses, copy_counters.HitRate() * 100.0,
                 copy_counters.checked, copy_counters.mismatches, copy_counters.total_mismatches,
                 copy_inconclusive, copy_unmemo, sw.copy_memo ? 1 : 0,
                 PerfMemo::CheckMode() ? " [VR_PERF_CHECK]" : "");
        const auto us_per = [](const BindTime& t) {
            return t.calls ? double(t.ns) / 1000.0 / double(t.calls) : 0.0;
        };
        LOG_INFO(Render_Vulkan,
                 "VR perf check: BindTextures with bind memo off {} calls {:.3f} us/call {:.2f} "
                 "ms; on {} calls {:.3f} us/call {:.2f} ms{}",
                 bind_time[0].calls, us_per(bind_time[0]), double(bind_time[0].ns) / 1e6,
                 bind_time[1].calls, us_per(bind_time[1]), double(bind_time[1].ns) / 1e6,
                 PerfMemo::CheckMode() ? " [VR_PERF_CHECK]" : "");
    }
    pipeline_cache.spec_counters.Reset();
    rt_counters.Reset();
    bind_counters.Reset();
    bind_invalid = bind_uncacheable = 0;
    bind_time = {};
    copy_counters.Reset();
    copy_unmemo = copy_inconclusive = 0;
    {
        const auto recent = copy_stats.recent;
        const u32 recent_next = copy_stats.recent_next;
        copy_stats = {};
        copy_stats.recent = recent;
        copy_stats.recent_next = recent_next;
    }
    perf_res_same = perf_res_total = 0;
    perf_logged_ms = now;
}

void Rasterizer::PerfCheckDesc(const char* what, u32 index,
                               const VideoCore::TextureCache::ImageDesc& memo,
                               const VideoCore::TextureCache::ImageDesc& fresh) {
    ++rt_counters.checked;
    const auto& a = memo.info;
    const auto& b = fresh.info;
    // Field by field (not the bytes: bit-field units and the unset mip levels are not part of
    // the description).
    std::string diff;
    const auto field = [&diff](bool same, const char* name) {
        if (!same) {
            diff += diff.empty() ? name : fmt::format(",{}", name);
        }
    };
    field(a.meta_info.cmask_addr == b.meta_info.cmask_addr, "cmask");
    field(a.meta_info.fmask_addr == b.meta_info.fmask_addr, "fmask");
    field(a.meta_info.htile_addr == b.meta_info.htile_addr, "htile");
    field(a.meta_info.htile_clear_mask == b.meta_info.htile_clear_mask, "htile_clear_mask");
    field(a.props.is_volume == b.props.is_volume && a.props.is_tiled == b.props.is_tiled &&
              a.props.is_pow2 == b.props.is_pow2 && a.props.is_block == b.props.is_block &&
              a.props.is_depth == b.props.is_depth && a.props.has_stencil == b.props.has_stencil,
          "props");
    field(a.pixel_format == b.pixel_format, "pixel_format");
    field(a.type == b.type, "type");
    field(a.resources.levels == b.resources.levels && a.resources.layers == b.resources.layers,
          "resources");
    field(a.size.width == b.size.width && a.size.height == b.size.height &&
              a.size.depth == b.size.depth,
          "size");
    field(a.num_bits == b.num_bits, "num_bits");
    field(a.num_samples == b.num_samples, "num_samples");
    field(a.pitch == b.pitch, "pitch");
    field(a.tile_mode == b.tile_mode, "tile_mode");
    field(a.array_mode == b.array_mode, "array_mode");
    field(a.mips_layout[0].size == b.mips_layout[0].size &&
              a.mips_layout[0].pitch == b.mips_layout[0].pitch &&
              a.mips_layout[0].height == b.mips_layout[0].height &&
              a.mips_layout[0].offset == b.mips_layout[0].offset,
          "mip0");
    field(a.guest_address == b.guest_address, "guest_address");
    field(a.guest_size == b.guest_size, "guest_size");
    field(a.bank_swizzle == b.bank_swizzle, "bank_swizzle");
    field(a.alt_tile == b.alt_tile, "alt_tile");
    field(a.stencil_addr == b.stencil_addr && a.stencil_size == b.stencil_size, "stencil");
    field(memo.view_info.type == fresh.view_info.type, "view.type");
    field(memo.view_info.format == fresh.view_info.format, "view.format");
    field(memo.view_info.range == fresh.view_info.range, "view.range");
    field(memo.view_info.mapping == fresh.view_info.mapping, "view.mapping");
    field(memo.view_info.min_lod == fresh.view_info.min_lod, "view.min_lod");
    field(memo.view_info.is_storage == fresh.view_info.is_storage, "view.is_storage");
    field(memo.type == fresh.type, "binding type");
    if (!diff.empty() && rt_counters.NoteMismatch()) {
        LOG_WARNING(Render_Vulkan,
                    "VR perf check: rt memo mismatch #{}: {} {}: differs in {}; remembered {:#x} "
                    "{}x{} fmt {} layers {} size {:#x}, built {:#x} {}x{} fmt {} layers {} size "
                    "{:#x}",
                    rt_counters.total_mismatches, what, index, diff, a.guest_address, a.size.width,
                    a.size.height, vk::to_string(a.pixel_format), a.resources.layers, a.guest_size,
                    b.guest_address, b.size.width, b.size.height, vk::to_string(b.pixel_format),
                    b.resources.layers, b.guest_size);
    }
}

bool Rasterizer::IsComputeMetaClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Most of the time when a metadata is updated with a shader it gets cleared. It means
    // we can skip the whole dispatch and update the tracked state instead. Also, it is not
    // intended to be consumed and in such rare cases (e.g. HTile introspection, CRAA) we
    // will need its full emulation anyways.
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);

    // Assume if a shader reads metadata, it is a copy shader.
    for (const auto& desc : info.buffers) {
        const VAddr address = desc.GetSharp(info).base_address;
        if (!desc.IsSpecial() && !desc.is_written && texture_cache.IsMeta(address)) {
            return false;
        }
    }

    // Metadata surfaces are tiled and thus need address calculation to be written properly.
    // If a shader wants to encode HTILE, for example, from a depth image it will have to compute
    // proper tile address from dispatch invocation id. This address calculation contains an xor
    // operation so use it as a heuristic for metadata writes that are probably not clears.
    if (!info.has_bitwise_xor) {
        // Assume if a shader writes metadata without address calculation, it is a clear shader.
        for (const auto& desc : info.buffers) {
            const VAddr address = desc.GetSharp(info).base_address;
            if (!desc.IsSpecial() && desc.is_written && texture_cache.ClearMeta(address)) {
                // Assume all slices were updates
                LOG_TRACE(Render_Vulkan, "Metadata update skipped");
                return true;
            }
        }
    }
    return false;
}

void Rasterizer::ProbeCopyReject(const Shader::Info& info, u32 threads_x, u32 dim_x,
                                 const char* why) {
    static std::mutex mutex;
    static std::set<std::pair<u64, std::string>> seen;
    {
        std::scoped_lock lock{mutex};
        if (seen.size() >= 64 || !seen.emplace(info.pgm_hash, why).second) {
            return;
        }
    }
    std::string details;
    for (u32 i = 0; i < info.buffers.size(); ++i) {
        const auto& desc = info.buffers[i];
        const AmdGpu::Buffer sharp = desc.GetSharp(info);
        details +=
            fmt::format("; buffer {} {:#x}+{:#x} formatted {} written {}", i, sharp.base_address,
                        sharp.GetSize(), desc.is_formatted ? 1 : 0, desc.is_written ? 1 : 0);
        texture_cache.ForEachImageInRegion(
            sharp.base_address, sharp.GetSize(), [&](VideoCore::ImageId, VideoCore::Image& image) {
                details += fmt::format(
                    " [image uid {} {:#x}+{:#x} {}x{} {} bits {} pitch {} tile {} depth {} "
                    "gpu-modified {} dirty {}]",
                    image.image_uid, image.info.guest_address, image.info.guest_size,
                    image.info.size.width, image.info.size.height,
                    vk::to_string(image.info.pixel_format), image.info.num_bits, image.info.pitch,
                    static_cast<u32>(image.info.tile_mode), image.info.props.is_depth ? 1 : 0,
                    True(image.flags & VideoCore::ImageFlagBits::GpuModified) ? 1 : 0,
                    True(image.flags & VideoCore::ImageFlagBits::Dirty) ? 1 : 0);
            });
    }
    LOG_INFO(Render_Vulkan, "VR perf probe: copy HLE refused cs {:#x} ({} threads, dim_x {}): {}{}",
             info.pgm_hash, threads_x, dim_x, why, details);
}

// VR_NATIVE_DIRECT_IMAGE_BUFFER (on unless '0'), read once at start-up.
static const bool g_native_direct_image_buffer = [] {
    const char* value = std::getenv("VR_NATIVE_DIRECT_IMAGE_BUFFER");
    return !value || value[0] != '0';
}();

bool Rasterizer::IsComputeImageCopy(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = std::as_const(ScratchDraw::ComputeRegisters(liverpool));
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    // VR_PERF_PROBE (tests; documents/vr-gpu-guide.md 2.5 B): why a two-buffer compute shader is
    // not turned into an image copy, logged once per shader and reason (64 at most), with both
    // buffers and the images at their addresses. No effect on what is recorded.
    const auto reject = [&](const char* why) {
        if (Perf::Probe() && info.buffers.size() == 2) [[unlikely]] {
            ProbeCopyReject(info, cs_pgm.num_thread_x.full, cs_pgm.dim_x, why);
        }
        return false;
    };
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return reject("not 64 threads, 2 buffers and no images");
    }

    // Those 2 buffers must both be formatted. One must be source and another destination.
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (!desc0.is_formatted || !desc1.is_formatted || desc0.is_written == desc1.is_written) {
        return reject("not two formatted buffers, one written");
    }

    // Buffers must have the same size and each thread of the dispatch must copy 1 dword of data
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    if (buf0.GetSize() != buf1.GetSize() || cs_pgm.dim_x != (buf0.GetSize() / 256)) {
        return reject("buffer sizes differ or dim_x != size / 256");
    }

    // Find images the buffer alias
    const auto image0_id = texture_cache.FindImageFromRange(buf0.base_address, buf0.GetSize());
    if (!image0_id) {
        return reject(desc0.is_written ? "no valid image at buffer 0 (the destination)"
                                       : "no valid image at buffer 0 (the source)");
    }
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        const bool direct_buffer = g_native_direct_image_buffer;
        // Audited shader 3d5ebf4e: one formatted dword load/store per invocation,
        // guarded by SGPR10. Only replace complete, non-overlapping, identity copies.
        auto& source = texture_cache.GetImage(image0_id);
        const auto identity_dword = [](const AmdGpu::Buffer& b) {
            const auto fmt = b.GetNumberFmt();
            return b.GetStride() == 4 && b.GetDataFmt() == AmdGpu::DataFormat::Format32 &&
                   (fmt == AmdGpu::NumberFormat::Float || fmt == AmdGpu::NumberFormat::Uint ||
                    fmt == AmdGpu::NumberFormat::Sint) &&
                   b.dst_sel_x == 4 && !b.swizzle_enable && !b.add_tid_enable;
        };
        const u64 bytes = buf0.GetSize();
        if (direct_buffer && NativeStereo::Enabled() && info.pgm_hash == 0x3d5ebf4e &&
            !desc0.is_written && desc1.is_written && identity_dword(buf0) && identity_dword(buf1) &&
            buf0.GetNumberFmt() == buf1.GetNumberFmt() && info.UserData().size() > 10 &&
            info.UserData()[10] >= bytes / 4 && cs_pgm.dim_y == 1 && cs_pgm.dim_z == 1 &&
            bytes == source.info.guest_size && source.info.resources.levels == 1 &&
            source.info.resources.layers == 1 && source.info.size.depth == 1 &&
            source.info.mips_layout[0].size == bytes &&
            (buf1.base_address + bytes <= buf0.base_address ||
             buf0.base_address + bytes <= buf1.base_address)) {
            const auto& mip = source.info.mips_layout[0];
            const auto [destination, offset] =
                buffer_cache.ObtainBuffer(buf1.base_address, bytes, true, true);
            vk::BufferImageCopy region{
                .bufferOffset = 0,
                .bufferRowLength = mip.pitch,
                .bufferImageHeight = mip.height,
                .imageSubresource{.aspectMask =
                                      source.aspect_mask & ~vk::ImageAspectFlagBits::eStencil,
                                  .mipLevel = 0,
                                  .baseArrayLayer = 0,
                                  .layerCount = 1},
                .imageOffset = {0, 0, 0},
                .imageExtent = {source.info.size.width, source.info.size.height, 1},
            };
            NativeStereo::Barrier(scheduler);
            texture_cache.GetTileManager().TileImage(source, std::span{&region, 1},
                                                     destination->Handle(), offset, bytes);
            NativeStereo::Barrier(scheduler);
            texture_cache.InvalidateMemoryFromGPU(buf1.base_address, bytes);
            // FAST bit 39 copy-chain probe (counters only; the re-tile above reads the image
            // itself with the bit on): is this destination uploaded into an image again?
            if (FastRender::On(VideoCore::TileManager::TileFromImage)) {
                VideoCore::g_image_sourced_writes.Note(
                    buf1.base_address, VideoCore::TileManager::TiledLayoutKey(source.info));
                VideoCore::g_tile_from_image_stats.chain_writes.fetch_add(
                    1, std::memory_order_relaxed);
            }
            static u64 direct_count = 0;
            if (++direct_count <= 8 || direct_count % 600 == 0)
                LOG_INFO(Render_Vulkan, "VR direct image buffer: {} copies, {} bytes, {} -> {}",
                         direct_count, bytes, source.info.guest_address, buf1.base_address);
            return true;
        }
        return reject(desc1.is_written ? "no image at buffer 1 (the destination)"
                                       : "no image at buffer 1 (the source)");
    }

    // Image copy must be valid
    VideoCore::Image& image0 = texture_cache.GetImage(image0_id);
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image0.info.guest_size != image1.info.guest_size ||
        image0.info.pitch != image1.info.pitch || image0.info.guest_size != buf0.GetSize() ||
        image0.info.num_bits != image1.info.num_bits) {
        return reject("the two images differ in size, pitch or bits, or image 0's size is not "
                      "the buffer's");
    }

    // Perform image copy
    VideoCore::Image& src_image = desc0.is_written ? image1 : image0;
    VideoCore::Image& dst_image = desc0.is_written ? image0 : image1;
    if (instance.IsMaintenance8Supported() ||
        src_image.info.props.is_depth == dst_image.info.props.is_depth) {
        dst_image.CopyImage(src_image);
    } else {
        const auto& copy_buffer =
            buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::DeviceLocal);
        dst_image.CopyImageWithBuffer(src_image, copy_buffer.Handle(), 0);
    }
    dst_image.flags |= VideoCore::ImageFlagBits::GpuModified;
    dst_image.flags &= ~VideoCore::ImageFlagBits::Dirty;
    return true;
}

// FAST bit 55 (gpu_plan.md item 4, G'): the game's clear shader cs 8b355b5a, which
// IsComputeImageClear refuses (13 calls/f in claude_mode_smaa_hs, about 10.5 us each as two
// dispatches and two global barriers), recorded as vkCmdFillBuffer of exactly the bytes it writes,
// in both eyes. Audited from its GCN dump (one binary for all four permutations): 64 threads per
// group; s_buffer_load_dwordx4 of the clear vector from the V# in s[8:11] at offset 0; then
// buffer_store_format_xyzw of those raw dwords, idxen, offset 0, into the V# in s[4:7] at elements
// g * 128 + t and g * 128 + t + 64. So it writes elements [0, dim_x * 128) and reads nothing else.
// The permutations differ only in the destination's format (8_8_8_8 unorm with a BGRA dst_sel,
// 16_16 float, 16_16_16_16 float, 10_11_11 float). bbport turns the same shader into an image
// clear (BBV/renderer_vulkan/vk_rasterizer.cpp:2895-2949, accepted there once its page-table
// buffer stopped counting, :2777-2786). This bit keeps the shader's buffer semantics instead: the
// dispatch binding's own ObtainBuffer and InvalidateMemoryFromGPU, the same right-eye buffer, and
// fills of the dispatch's bytes, so guest memory and both caches end as after the dispatch. Read
// per dispatch: it may flip between packets and frames. FAST_COMPUTE_CLEAR_FILL_VERIFY=N: every
// Nth fill also runs the dispatch, and the bytes are compared (FinishClearFillVerify).
static constexpr auto ComputeClearFill = static_cast<FastRender::Bit>(55);

namespace {
constexpr u64 ClearShaderHash = 0x8b355b5a;

// Why IsComputeImageClear refused the compute dispatch it saw last on this thread. BindResources
// asks IsComputeClearFill right after it (its counters only).
enum class ClearRefusal : u8 { Accepted, Shape, Buffers, Sizes, NoImage, Image, Count };
thread_local ClearRefusal t_clear_refusal = ClearRefusal::Accepted;

// Why bit 55 left an eligible cs 8b355b5a dispatch to the shader.
enum class FillFallback : u8 { Indirect, Shape, ClearValue, Layout, Extent, Value, Verify, Count };
constexpr std::array<const char*, static_cast<size_t>(FillFallback::Count)> FillFallbackNames{
    "indirect", "shape", "clear_value", "layout", "extent", "value", "verify"};

struct ClearFillStats {
    std::atomic<u64> eligible{}, taken{}, bytes{}, verified{}, mismatches{}, verify_busy{},
        verify_skipped{};
    std::array<std::atomic<u64>, static_cast<size_t>(FillFallback::Count)> fallback{};
    std::array<std::atomic<u64>, static_cast<size_t>(ClearRefusal::Count)> image_clear{};
    std::atomic<s64> next_report_ns{};
    std::atomic<u32> logged_fallbacks{};
};
ClearFillStats g_clear_fill;

// FAST_COMPUTE_CLEAR_FILL_VERIFY's readback: three phases (before, after the fill, after the
// dispatch) of up to four windows (the head and tail of the filled bytes, each eye), one sample
// in flight. A window keeps up to ClearVerifyGuard bytes on each side of the filled bytes.
constexpr u64 ClearVerifyWindow = 64 * 1024;
constexpr u64 ClearVerifyGuard = 256;
constexpr u64 ClearVerifySlot = 2 * ClearVerifyWindow + 2 * ClearVerifyGuard;
constexpr u64 ClearVerifyBytes = 3 * 4 * ClearVerifySlot;
constexpr u64 ClearVerifyAt(u32 phase, u32 slot) {
    return (u64{phase} * 4 + slot) * ClearVerifySlot;
}
std::atomic<bool> g_clear_verify_busy{false};

// Every 5 s from the eligible dispatches: cumulative counts.
void ReportClearFill() {
    auto& s = g_clear_fill;
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    s64 next = s.next_report_ns.load(std::memory_order_relaxed);
    if (now < next || !s.next_report_ns.compare_exchange_strong(next, now + 5'000'000'000LL,
                                                                 std::memory_order_relaxed)) {
        return;
    }
    if (next == 0) {
        return; // the first eligible dispatch starts the period
    }
    const auto n = [](const std::atomic<u64>& counter) {
        return counter.load(std::memory_order_relaxed);
    };
    const auto fallback = [&](FillFallback why) {
        return n(s.fallback[static_cast<size_t>(why)]);
    };
    const auto refused = [&](ClearRefusal why) {
        return n(s.image_clear[static_cast<size_t>(why)]);
    };
    u64 fallbacks = 0;
    for (const auto& counter : s.fallback) {
        fallbacks += n(counter);
    }
    LOG_INFO(Render_Vulkan,
             "FAST compute_clear_fill: cs {:08x} eligible={} taken={} filled_mib={:.2f} "
             "fallback={} (indirect={} shape={} clear_value={} layout={} extent={} value={} "
             "verify={}) image_clear accepted={} refused_by: shape={} buffers={} sizes={} "
             "no_image={} image={} verified={} mismatches={} verify_busy={} verify_skipped={}",
             ClearShaderHash, n(s.eligible), n(s.taken),
             static_cast<double>(n(s.bytes)) / (1024.0 * 1024.0), fallbacks,
             fallback(FillFallback::Indirect), fallback(FillFallback::Shape),
             fallback(FillFallback::ClearValue), fallback(FillFallback::Layout),
             fallback(FillFallback::Extent), fallback(FillFallback::Value),
             fallback(FillFallback::Verify), refused(ClearRefusal::Accepted),
             refused(ClearRefusal::Shape), refused(ClearRefusal::Buffers),
             refused(ClearRefusal::Sizes), refused(ClearRefusal::NoImage),
             refused(ClearRefusal::Image), n(s.verified), n(s.mismatches), n(s.verify_busy),
             n(s.verify_skipped));
}

// The first fallback for each reason: the dispatch and both V#s as the shader sees them.
void LogClearFillFallback(const Shader::Info& info, const AmdGpu::ComputeProgram& cs,
                          FillFallback why) {
    std::string sharps;
    for (u32 i = 0; i < info.buffers.size(); ++i) {
        const AmdGpu::Buffer sharp = info.buffers[i].GetSharp(info);
        sharps += fmt::format(
            "; buffer {} {:#x}+{:#x} stride {} format {} number {} dst_sel {}{}{}{} swizzle {} "
            "add_tid {} type {}",
            i, static_cast<u64>(sharp.base_address), sharp.GetSize(), sharp.GetStride(),
            static_cast<u32>(sharp.GetDataFmt()), static_cast<u32>(sharp.GetNumberFmt()),
            static_cast<u32>(sharp.dst_sel_x), static_cast<u32>(sharp.dst_sel_y),
            static_cast<u32>(sharp.dst_sel_z), static_cast<u32>(sharp.dst_sel_w),
            static_cast<u32>(sharp.swizzle_enable), static_cast<u32>(sharp.add_tid_enable),
            static_cast<u32>(info.buffers[i].buffer_type));
    }
    LOG_INFO(Render_Vulkan,
             "FAST compute_clear_fill: first fallback '{}' of cs {:08x}: {}x{}x{} threads "
             "(partial x {}), {}x{}x{} groups from {},{},{}, {} buffers, {} images, dma {}{}",
             FillFallbackNames[static_cast<size_t>(why)], info.pgm_hash, cs.num_thread_x.full,
             cs.num_thread_y.full, cs.num_thread_z.full, cs.num_thread_x.partial, cs.dim_x,
             cs.dim_y, cs.dim_z, cs.start_x, cs.start_y, cs.start_z, info.buffers.size(),
             info.images.size(), info.uses_dma ? 1 : 0, sharps);
}

// The dword that vkCmdFillBuffer repeats to write what the audited shader's formatted store
// writes for `clear` (lower_buffer_format_to_raw.cpp StoreBufferFormat: the inverse dst_sel, then
// the pack), or nothing when that is not one known repeated dword. Exact by construction: zero in
// every format the store lowers with no number conversion, and 0.0 / 1.0 only where the pack is
// exact (packUnorm4x8, packHalf2x16, a 32-bit float store). Anything else stays a dispatch.
std::optional<u32> ClearFillWord(const AmdGpu::Buffer& dst, const std::array<u32, 4>& clear) {
    using AmdGpu::DataFormat;
    using AmdGpu::NumberFormat;
    if (dst.GetNumberConversion() != AmdGpu::NumberConversion::None) {
        return std::nullopt;
    }
    const DataFormat format = dst.GetDataFmt();
    const NumberFormat number = dst.GetNumberFmt();
    switch (format) {
    case DataFormat::Format8:
    case DataFormat::Format8_8:
    case DataFormat::Format8_8_8_8:
    case DataFormat::Format16:
    case DataFormat::Format16_16:
    case DataFormat::Format10_11_11:
    case DataFormat::Format2_10_10_10:
    case DataFormat::Format16_16_16_16:
    case DataFormat::Format32:
    case DataFormat::Format32_32:
    case DataFormat::Format32_32_32:
    case DataFormat::Format32_32_32_32:
        break;
    default:
        return std::nullopt; // FormatInvalid stores nothing; others are not lowered
    }
    const u32 count = AmdGpu::NumComponents(format);
    if (count == 0 || count > 4) {
        return std::nullopt;
    }
    constexpr u32 One = 0x3F800000u; // 1.0f
    const AmdGpu::CompMapping inverse = dst.DstSelect().Inverse();
    std::array<u32, 4> stored{};
    bool zero = true;
    for (u32 i = 0; i < count; ++i) {
        const AmdGpu::CompSwizzle select = inverse.array[i];
        switch (select) {
        case AmdGpu::CompSwizzle::Zero:
            stored[i] = 0;
            break;
        case AmdGpu::CompSwizzle::One:
            stored[i] = One;
            break;
        case AmdGpu::CompSwizzle::Red:
        case AmdGpu::CompSwizzle::Green:
        case AmdGpu::CompSwizzle::Blue:
        case AmdGpu::CompSwizzle::Alpha:
            stored[i] =
                clear[static_cast<u32>(select) - static_cast<u32>(AmdGpu::CompSwizzle::Red)];
            break;
        default:
            return std::nullopt;
        }
        zero = zero && stored[i] == 0;
    }
    if (zero) {
        const bool plain = number == NumberFormat::Unorm || number == NumberFormat::Snorm ||
                           number == NumberFormat::Uint || number == NumberFormat::Sint ||
                           number == NumberFormat::Float;
        return plain ? std::optional<u32>{0u} : std::nullopt;
    }
    for (u32 i = 0; i < count; ++i) {
        if (stored[i] != 0 && stored[i] != One) {
            return std::nullopt;
        }
    }
    const auto half = [&stored](u32 i) { return stored[i] == One ? 0x3C00u : 0u; };
    switch (format) {
    case DataFormat::Format8_8_8_8: {
        if (number != NumberFormat::Unorm) {
            return std::nullopt;
        }
        u32 word = 0;
        for (u32 i = 0; i < 4; ++i) {
            word |= stored[i] == One ? 0xFFu << (8 * i) : 0u;
        }
        return word;
    }
    case DataFormat::Format16_16:
        if (number != NumberFormat::Float) {
            return std::nullopt;
        }
        return half(0) | half(1) << 16;
    case DataFormat::Format16_16_16_16: {
        if (number != NumberFormat::Float) {
            return std::nullopt;
        }
        const u32 low = half(0) | half(1) << 16;
        const u32 high = half(2) | half(3) << 16;
        return low == high ? std::optional<u32>{low} : std::nullopt;
    }
    case DataFormat::Format32:
    case DataFormat::Format32_32:
    case DataFormat::Format32_32_32:
    case DataFormat::Format32_32_32_32:
        if (number != NumberFormat::Float) {
            return std::nullopt;
        }
        for (u32 i = 1; i < count; ++i) {
            if (stored[i] != stored[0]) {
                return std::nullopt;
            }
        }
        return stored[0];
    default:
        return std::nullopt;
    }
}
} // namespace

void Rasterizer::ProbeClearReject(const Shader::Info& info, const char* why) {
    static std::mutex mutex;
    static std::set<std::pair<u64, std::string>> seen;
    {
        std::scoped_lock lock{mutex};
        if (seen.size() >= 64 || !seen.emplace(info.pgm_hash, why).second) {
            return;
        }
    }
    const auto& cs_pgm = std::as_const(ScratchDraw::ComputeRegisters(liverpool));
    std::string details;
    for (u32 i = 0; i < info.buffers.size(); ++i) {
        const auto& desc = info.buffers[i];
        const AmdGpu::Buffer sharp = desc.GetSharp(info);
        const VAddr address = sharp.base_address;
        details += fmt::format(
            "; buffer {} {:#x}+{:#x} stride {} format {} number {} dst_sel {}{}{}{} swizzle {} "
            "type {} formatted {} written {}",
            i, address, sharp.GetSize(), sharp.GetStride(), static_cast<u32>(sharp.GetDataFmt()),
            static_cast<u32>(sharp.GetNumberFmt()), static_cast<u32>(sharp.dst_sel_x),
            static_cast<u32>(sharp.dst_sel_y), static_cast<u32>(sharp.dst_sel_z),
            static_cast<u32>(sharp.dst_sel_w), static_cast<u32>(sharp.swizzle_enable),
            static_cast<u32>(desc.buffer_type), desc.is_formatted ? 1 : 0,
            desc.is_written ? 1 : 0);
        if (desc.IsSpecial() || address <= 1) {
            continue;
        }
        if (!desc.is_written && sharp.GetSize() >= 16 &&
            memory->ClampRangeSize(address, 16) == 16) {
            std::array<u32, 4> words{};
            memory->CopySparseMemory(address, reinterpret_cast<u8*>(words.data()), sizeof(words));
            details += fmt::format(" words {:#x} {:#x} {:#x} {:#x}{}", words[0], words[1], words[2],
                                   words[3],
                                   buffer_cache.IsRegionGpuModified(address, 16) ? " (gpu-modified)"
                                                                                 : "");
        }
        if (desc.is_written) {
            texture_cache.ForEachImageInRegion(
                address, sharp.GetSize(), [&](VideoCore::ImageId, VideoCore::Image& image) {
                    details += fmt::format(
                        " [image uid {} {:#x}+{:#x} {}x{} {} bits {} pitch {} tile {} depth {} "
                        "gpu-modified {} dirty {}]",
                        image.image_uid, image.info.guest_address, image.info.guest_size,
                        image.info.size.width, image.info.size.height,
                        vk::to_string(image.info.pixel_format), image.info.num_bits,
                        image.info.pitch, static_cast<u32>(image.info.tile_mode),
                        image.info.props.is_depth ? 1 : 0,
                        True(image.flags & VideoCore::ImageFlagBits::GpuModified) ? 1 : 0,
                        True(image.flags & VideoCore::ImageFlagBits::Dirty) ? 1 : 0);
                });
        }
    }
    LOG_INFO(Render_Vulkan,
             "FAST compute_clear probe: image clear refused cs {:#x} ({}x{}x{} threads, {}x{}x{} "
             "groups, {} images): {}{}",
             info.pgm_hash, cs_pgm.num_thread_x.full, cs_pgm.num_thread_y.full,
             cs_pgm.num_thread_z.full, cs_pgm.dim_x, cs_pgm.dim_y, cs_pgm.dim_z,
             info.images.size(), why, details);
}

bool Rasterizer::IsComputeImageClear(const Pipeline* pipeline) {
    if (!pipeline->IsCompute()) {
        return false;
    }

    // Ensure shader only has 2 bound buffers
    const auto& cs_pgm = std::as_const(ScratchDraw::ComputeRegisters(liverpool));
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    // Bit 55: why the shader is refused, noted for IsComputeClearFill's counters. Also logged
    // once per (shader, reason): with the bit for cs 8b355b5a only, with VR_PERF_PROBE for it and
    // for every shader past the first check. No effect on what is recorded.
    const auto reject = [&](ClearRefusal why, const char* text) {
        t_clear_refusal = why;
        const bool target = info.pgm_hash == ClearShaderHash;
        if ((target && FastRender::On(ComputeClearFill)) ||
            ((why != ClearRefusal::Shape || target) && Perf::Probe())) [[unlikely]] {
            ProbeClearReject(info, text);
        }
        return false;
    };
    if (cs_pgm.num_thread_x.full != 64 || info.buffers.size() != 2 || !info.images.empty()) {
        return reject(ClearRefusal::Shape, "not 64 threads, 2 buffers and no images");
    }

    // From those 2 buffers, first must hold the clear vector and second the image being cleared
    const auto& desc0 = info.buffers[0];
    const auto& desc1 = info.buffers[1];
    if (desc0.is_formatted || !desc1.is_formatted || desc0.is_written || !desc1.is_written) {
        return reject(ClearRefusal::Buffers,
                      "buffer 0 is not read-only raw or buffer 1 not written formatted");
    }

    // First buffer must have size of vec4 and second the size of a single layer
    const AmdGpu::Buffer buf0 = desc0.GetSharp(info);
    const AmdGpu::Buffer buf1 = desc1.GetSharp(info);
    const u32 buf1_bpp = AmdGpu::NumBitsPerBlock(buf1.GetDataFmt());
    if (buf0.GetSize() != 16 || (cs_pgm.dim_x * 128ULL * (buf1_bpp / 8)) != buf1.GetSize()) {
        return reject(ClearRefusal::Sizes,
                      "buffer 0 is not 16 bytes or dim_x * 128 * bpp / 8 is not buffer 1's size");
    }

    // Find image the buffer alias
    const auto image1_id =
        texture_cache.FindImageFromRange(buf1.base_address, buf1.GetSize(), false);
    if (!image1_id) {
        return reject(ClearRefusal::NoImage, "no image at buffer 1's address");
    }

    // Image clear must be valid
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    if (image1.info.guest_size != buf1.GetSize() || image1.info.num_bits != buf1_bpp ||
        image1.info.props.is_depth) {
        return reject(ClearRefusal::Image,
                      "the image at buffer 1 differs in size or bits, or is depth");
    }
    t_clear_refusal = ClearRefusal::Accepted;
    if (info.pgm_hash == ClearShaderHash && FastRender::On(ComputeClearFill)) [[unlikely]] {
        g_clear_fill.image_clear[static_cast<size_t>(ClearRefusal::Accepted)].fetch_add(
            1, std::memory_order_relaxed);
    }

    // Perform image clear
    const float* values = reinterpret_cast<float*>(buf0.base_address);
    const vk::ClearValue clear = {
        .color = {.float32 = std::array<float, 4>{values[0], values[1], values[2], values[3]}},
    };
    const VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    image1.Clear(clear, range);
    image1.flags |= VideoCore::ImageFlagBits::GpuModified;
    image1.flags &= ~VideoCore::ImageFlagBits::Dirty;
    return true;
}

// FAST_COMPUTE_CLEAR_FILL_VERIFY=N, read once at start-up (no function-local static's guard).
static const u64 g_clear_fill_verify_every = [] {
    const char* value = std::getenv("FAST_COMPUTE_CLEAR_FILL_VERIFY");
    return value ? std::strtoull(value, nullptr, 0) : 0ULL;
}();

bool Rasterizer::IsComputeClearFill(const Pipeline* pipeline) {
    if (!pipeline->IsCompute() || !FastRender::On(ComputeClearFill)) {
        return false;
    }
    const auto& info = pipeline->GetStage(Shader::LogicalStage::Compute);
    if (info.pgm_hash != ClearShaderHash) {
        return false;
    }
    auto& stats = g_clear_fill;
    stats.eligible.fetch_add(1, std::memory_order_relaxed);
    stats.image_clear[static_cast<size_t>(t_clear_refusal)].fetch_add(1,
                                                                      std::memory_order_relaxed);
    ReportClearFill();
    const auto& cs_pgm = std::as_const(ScratchDraw::ComputeRegisters(liverpool));
    const auto fallback = [&](FillFallback why) {
        stats.fallback[static_cast<size_t>(why)].fetch_add(1, std::memory_order_relaxed);
        const u32 bit = 1u << static_cast<u32>(why);
        if ((stats.logged_fallbacks.fetch_or(bit, std::memory_order_relaxed) & bit) == 0) {
            LogClearFillFallback(info, cs_pgm, why);
        }
        return false;
    };
    // An indirect dispatch's group count is in memory, not in dim_x.
    if (!recording_direct_dispatch) {
        return fallback(FillFallback::Indirect);
    }
    if (cs_pgm.num_thread_x.full != 64 || cs_pgm.num_thread_x.partial != 0 ||
        cs_pgm.num_thread_y.full != 1 || cs_pgm.num_thread_z.full != 1 || cs_pgm.dim_x == 0 ||
        cs_pgm.dim_y != 1 || cs_pgm.dim_z != 1 || cs_pgm.start_x != 0 || cs_pgm.start_y != 0 ||
        cs_pgm.start_z != 0 || info.buffers.size() != 2 || !info.images.empty() ||
        !info.samplers.empty() || info.uses_dma) {
        return fallback(FillFallback::Shape);
    }
    const auto& clear_desc = info.buffers[0];
    const auto& dst_desc = info.buffers[1];
    if (clear_desc.IsSpecial() || dst_desc.IsSpecial() || clear_desc.is_written ||
        clear_desc.is_formatted || !dst_desc.is_written || !dst_desc.is_formatted) {
        return fallback(FillFallback::Shape);
    }

    // The clear vector: the 16 bytes the dispatch's binding gives the shader (BindBuffers). This
    // packet's prepared constant is the ring copy it binds; otherwise guest memory now, as the
    // stream copy takes it, unless the GPU wrote it (then the buffer cache's bytes are bound,
    // which the CPU cannot see). Both eyes read the same bytes: the ring and the stream buffer
    // are mirrored into the right eye's copy.
    const AmdGpu::Buffer clear_sharp = clear_desc.GetSharp(info);
    const VAddr clear_address = clear_sharp.base_address;
    const u64 clear_bytes = clear_address > 1 && clear_sharp.GetSize() > 0
                                ? memory->ClampRangeSize(clear_address, clear_sharp.GetSize())
                                : 0;
    if (clear_bytes < 16 || clear_bytes > VideoCore::BufferCache::CACHING_PAGESIZE) {
        return fallback(FillFallback::ClearValue);
    }
    // A read-only compute binding of a camera or lighting constant's size can be bound as per-eye
    // data instead of these bytes (BindBuffers: NativeStereo::CameraBuffer / LightingBuffer, or
    // the ring's camera and lighting pairs), so each eye may read another vector: the dispatch.
    if (clear_bytes == 864 || clear_bytes == 208 || clear_bytes == 272 || clear_bytes == 336 ||
        clear_bytes == 416) {
        return fallback(FillFallback::ClearValue);
    }
    std::array<u32, 4> clear{};
    const DrawPacket::ConstantBinding* const constant =
        binding_packet ? binding_packet->Constant(info, 0) : nullptr;
    if (constant && !constant->flat &&
        std::memcmp(&constant->sharp, &clear_sharp, sizeof(clear_sharp)) == 0) {
        if (constant->camera || constant->lighting) {
            return fallback(FillFallback::ClearValue);
        }
        if (!constant_ring || constant->raw.buffer != constant_ring->Handle() ||
            constant->bytes < sizeof(clear) ||
            constant->raw.offset + sizeof(clear) > constant_ring->mapped_data.size()) {
            return fallback(FillFallback::ClearValue);
        }
        std::memcpy(clear.data(), constant_ring->mapped_data.data() + constant->raw.offset,
                    sizeof(clear));
    } else if (buffer_cache.IsRegionGpuModified(clear_address, clear_bytes)) {
        return fallback(FillFallback::ClearValue);
    } else {
        memory->CopySparseMemory(clear_address, reinterpret_cast<u8*>(clear.data()),
                                 sizeof(clear));
    }

    // The destination: dim_x * 128 elements of one format element each, from element 0, inside
    // the range the dispatch binds (no out-of-range store, which robustness may place anywhere
    // in that range).
    const AmdGpu::Buffer dst = dst_desc.GetSharp(info);
    const VAddr address = dst.base_address;
    const u32 element = AmdGpu::NumBitsPerBlock(dst.GetDataFmt()) / 8;
    if (address <= 1 || address % 4 != 0 || element == 0 || dst.GetStride() != element ||
        dst.swizzle_enable || dst.add_tid_enable) {
        return fallback(FillFallback::Layout);
    }
    const u64 bound = memory->ClampRangeSize(address, dst.GetSize());
    const u64 bytes = u64{cs_pgm.dim_x} * 128 * element;
    if (bytes > bound || bytes % 4 != 0 || bound > std::numeric_limits<u32>::max()) {
        return fallback(FillFallback::Extent);
    }
    if (clear_address < address + bound && address < clear_address + clear_bytes) {
        return fallback(FillFallback::Layout); // the shader would read what it writes
    }
    const std::optional<u32> word = ClearFillWord(dst, clear);
    if (!word) {
        return fallback(FillFallback::Value);
    }

    const u64 verify_every = g_clear_fill_verify_every;
    static u64 verify_count = 0; // the recording owner's
    bool sample = false;
    if (verify_every != 0 && verify_count++ % verify_every == 0) {
        if (!g_clear_verify_busy.exchange(true, std::memory_order_acq_rel)) {
            if (!clear_fill_readback) {
                clear_fill_readback = std::make_shared<VideoCore::Buffer>(
                    instance, scheduler, VideoCore::MemoryUsage::Download, 0,
                    vk::BufferUsageFlagBits::eTransferDst | vk::BufferUsageFlagBits::eTransferSrc,
                    ClearVerifyBytes);
            }
            sample = clear_fill_readback->mapped_data.size() >= ClearVerifyBytes;
            if (!sample) {
                g_clear_verify_busy.store(false, std::memory_order_release);
            }
        } else {
            stats.verify_busy.fetch_add(1, std::memory_order_relaxed);
        }
    }

    // From here the calls the dispatch's binding makes for its written buffer, in its order
    // (BindResources, BindBuffers, PairDescriptors): the same buffer, synchronization, write
    // marks, image invalidation and right-eye buffer.
    Perf::NextDraw();
    const auto [dst_buffer, offset] =
        buffer_cache.ObtainBuffer(address, static_cast<u32>(bound), true, true);
    texture_cache.InvalidateMemoryFromGPU(address, bound);
    if (offset % 4 != 0) { // not with a dword-aligned address in a page-aligned buffer
        if (sample) {
            g_clear_verify_busy.store(false, std::memory_order_release);
        }
        return fallback(FillFallback::Layout); // the dispatch's binding repeats the calls above
    }
    const vk::Buffer left = dst_buffer->Handle();
    const vk::Buffer right = NativeStereo::RightBuffer(scheduler, left, true); // left when mono
    const bool paired = right && right != left;

    // A sample's windows: the filled bytes [offset, verify_end) with up to ClearVerifyGuard bytes
    // of guard on each side, inside the buffer.
    const u64 verify_end = offset + bytes;
    const u64 verify_lo = offset > ClearVerifyGuard ? offset - ClearVerifyGuard : 0;
    const u64 verify_hi =
        std::min<u64>(Common::AlignDown(static_cast<u64>(dst_buffer->SizeBytes()), 4),
                      verify_end + ClearVerifyGuard);
    if (sample && clear_address < address + (verify_hi - offset) &&
        address - (offset - verify_lo) < clear_address + clear_bytes) {
        // The guards stay poisoned while the sampled dispatch runs: never where it reads.
        sample = false;
        stats.verify_skipped.fetch_add(1, std::memory_order_relaxed);
        g_clear_verify_busy.store(false, std::memory_order_release);
    }

    if (sample) {
        // Poison the windows, fill, read back, poison them again and let the game's dispatch run
        // on the poison (FinishClearFillVerify reads it back, then restores the guards). Inside
        // the filled bytes the dispatch must write exactly what the fill wrote; outside, both
        // must leave the poison. So every inside byte of a window is written by both and no
        // guard byte by either, whatever the bytes held before. All bytes end as the dispatch
        // leaves them (the guards as before) unless the sample mismatches: then inside bytes the
        // dispatch skipped keep the poison and guard bytes it wrote get their old value back.
        auto& v = clear_fill_sample;
        v = ClearFillSample{};
        const u64 end = verify_end;
        const u64 lo = verify_lo;
        const u64 hi = verify_hi;
        std::array<std::pair<u64, u64>, 2> windows{};
        u32 num_windows = 1;
        if (hi - lo <= ClearVerifySlot) {
            windows[0] = {lo, hi - lo};
        } else {
            windows[0] = {lo, offset + ClearVerifyWindow - lo};
            windows[1] = {end - ClearVerifyWindow, hi - (end - ClearVerifyWindow)};
            num_windows = 2;
        }
        for (u32 eye = 0; eye < (paired ? 2u : 1u); ++eye) {
            for (u32 w = 0; w < num_windows; ++w) {
                v.regions[v.count] = {.buffer = eye != 0 ? right : left,
                                      .offset = windows[w].first,
                                      .bytes = windows[w].second,
                                      .slot = v.count,
                                      .right = eye != 0};
                ++v.count;
            }
        }
        v.inside_begin = offset;
        v.inside_end = end;
        v.word = *word;
        v.poison = *word ^ 0xA5A5A5A5u;
        v.pending = true;
        const vk::Buffer readback = clear_fill_readback->Handle();
        const auto cmdbuf = scheduler.Commands();
        const auto regions = std::span{v.regions.data(), v.count};
        NativeStereo::Barrier(scheduler);
        for (const auto& r : regions) {
            cmdbuf.copyBuffer(r.buffer, readback,
                              vk::BufferCopy{r.offset, ClearVerifyAt(0, r.slot), r.bytes});
        }
        NativeStereo::Barrier(scheduler);
        for (const auto& r : regions) {
            cmdbuf.fillBuffer(r.buffer, r.offset, r.bytes, v.poison);
        }
        NativeStereo::Barrier(scheduler);
        cmdbuf.fillBuffer(left, offset, bytes, v.word);
        if (paired) {
            cmdbuf.fillBuffer(right, offset, bytes, v.word);
        }
        NativeStereo::Barrier(scheduler);
        for (const auto& r : regions) {
            cmdbuf.copyBuffer(r.buffer, readback,
                              vk::BufferCopy{r.offset, ClearVerifyAt(1, r.slot), r.bytes});
        }
        NativeStereo::Barrier(scheduler);
        for (const auto& r : regions) {
            cmdbuf.fillBuffer(r.buffer, r.offset, r.bytes, v.poison);
        }
        NativeStereo::Barrier(scheduler);
        return fallback(FillFallback::Verify);
    }

    scheduler.EndRendering();
    if (GpuPassProfiler* const profiler = GpuPassProfiler::For(scheduler)) [[unlikely]] {
        profiler->Mark(GpuPassProfiler::Mix(0xD15A'0000'0003ull, info.pgm_hash),
                       GpuPassProfiler::Group::Dispatch, [&] {
                           return fmt::format("compute clear fill cs {:08x} (bit 55{})",
                                              info.pgm_hash, paired ? ", both eyes" : "");
                       });
    }
    // Every earlier access of either eye's copy before the fills, the fills before everything
    // after: the dispatch pair's two global barriers, without one between the eyes (the fills
    // write separate allocations). The post barrier makes the writes visible to every later
    // read, as Buffer::shader_reads_visible expects of writes GetBarrier does not see.
    NativeStereo::Barrier(scheduler);
    const auto cmdbuf = scheduler.Commands();
    cmdbuf.fillBuffer(left, offset, bytes, *word);
    if (paired) {
        cmdbuf.fillBuffer(right, offset, bytes, *word);
    }
    NativeStereo::Barrier(scheduler);
    stats.taken.fetch_add(1, std::memory_order_relaxed);
    stats.bytes.fetch_add(paired ? 2 * bytes : bytes, std::memory_order_relaxed);
    return true;
}

void Rasterizer::FinishClearFillVerify() {
    const ClearFillSample sample = std::exchange(clear_fill_sample, ClearFillSample{});
    const auto readback = clear_fill_readback; // the compare below keeps it alive
    // The dispatch's writes (a mono dispatch ends with no barrier) before the reads, the reads
    // before the guards' restore, the restore before any later access (GetBarrier sees none of
    // them), the copies before the host reads.
    NativeStereo::Barrier(scheduler);
    const auto cmdbuf = scheduler.Commands();
    for (u32 k = 0; k < sample.count; ++k) {
        const auto& r = sample.regions[k];
        cmdbuf.copyBuffer(r.buffer, readback->Handle(),
                          vk::BufferCopy{r.offset, ClearVerifyAt(2, r.slot), r.bytes});
    }
    NativeStereo::Barrier(scheduler);
    // The guards (each window's bytes outside the filled range) get their bytes from before the
    // sample back; the bytes inside keep what the dispatch wrote.
    for (u32 k = 0; k < sample.count; ++k) {
        const auto& r = sample.regions[k];
        const u64 r_end = r.offset + r.bytes;
        if (r.offset < sample.inside_begin) {
            const u64 head = std::min(r_end, sample.inside_begin) - r.offset;
            cmdbuf.copyBuffer(readback->Handle(), r.buffer,
                              vk::BufferCopy{ClearVerifyAt(0, r.slot), r.offset, head});
        }
        if (r_end > sample.inside_end) {
            const u64 start = std::max(r.offset, sample.inside_end);
            cmdbuf.copyBuffer(
                readback->Handle(), r.buffer,
                vk::BufferCopy{ClearVerifyAt(0, r.slot) + (start - r.offset), start, r_end - start});
        }
    }
    NativeStereo::Barrier(scheduler);
    const vk::MemoryBarrier2 to_host{
        .srcStageMask = vk::PipelineStageFlagBits2::eTransfer,
        .srcAccessMask = vk::AccessFlagBits2::eTransferWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eHost,
        .dstAccessMask = vk::AccessFlagBits2::eHostRead,
    };
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .memoryBarrierCount = 1,
        .pMemoryBarriers = &to_host,
    });
    scheduler.DeferOperation([readback, sample] {
        const auto& data = readback->mapped_data;
        if (!readback->is_coherent) {
            vmaInvalidateAllocation(readback->buffer.allocator, readback->buffer.allocation, 0,
                                    VK_WHOLE_SIZE);
        }
        u64 bad = 0;
        std::string first;
        for (u32 k = 0; k < sample.count; ++k) {
            const auto& r = sample.regions[k];
            const u8* const before = data.data() + ClearVerifyAt(0, r.slot);
            const u8* const filled = data.data() + ClearVerifyAt(1, r.slot);
            const u8* const dispatched = data.data() + ClearVerifyAt(2, r.slot);
            for (u64 j = 0; j < r.bytes; ++j) {
                const u64 at = r.offset + j;
                const bool inside = at >= sample.inside_begin && at < sample.inside_end;
                const u8 poison = static_cast<u8>(sample.poison >> (8 * (at % 4)));
                // Both ran on poisoned windows: inside, the dispatch must have written the fill's
                // bytes (the fill word differs from the poison in every byte); outside, neither
                // may have written.
                const bool same = inside ? filled[j] == dispatched[j]
                                         : filled[j] == poison && dispatched[j] == poison;
                if (!same && bad++ == 0) {
                    first = fmt::format("{} eye at {:#x} ({} the filled {:#x}..{:#x}): before "
                                        "{:#04x} fill {:#04x} dispatch {:#04x}",
                                        r.right ? "right" : "left", at,
                                        inside ? "inside" : "outside", sample.inside_begin,
                                        sample.inside_end, before[j], filled[j], dispatched[j]);
                }
            }
        }
        auto& s = g_clear_fill;
        if (bad != 0) {
            const u64 n = s.mismatches.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 64 || (n & 1023) == 0) {
                LOG_ERROR(Render_Vulkan,
                          "FAST compute_clear_fill verify: cs {:08x} fill word {:#010x} differs "
                          "from the dispatch in {} bytes, first {}; mismatch #{}",
                          ClearShaderHash, sample.word, bad, first, n);
            }
        } else {
            s.verified.fetch_add(1, std::memory_order_relaxed);
        }
        g_clear_verify_busy.store(false, std::memory_order_release);
    });
}

namespace {
// SmallUploadBatch's switches, read once at start-up: a plain load per stage and per buffer where
// the function-local statics paid a guard (a thread-local read) on every call.
const bool g_upload_scan_audit = std::getenv("VR_UPLOAD_SCAN_AUDIT") != nullptr;
const bool g_batch_small_uploads = [] {
    const char* setting = std::getenv("VR_BATCH_SMALL_UPLOADS");
    return (!setting || setting[0] != '0') && std::getenv("VR_TRACKED_SMALL_BUFFERS") == nullptr &&
           std::getenv("VR_EXACT_UPLOAD_MEMO") == nullptr;
}();
const bool g_single_upload_fastpath = std::getenv("VR_SINGLE_UPLOAD_FASTPATH") != nullptr;
// Keep the established BindBuffers control flow intact so its PGO remains usable.
// The optional experiment owns only temporary offsets; Vulkan sees the same resources.
class SmallUploadBatch {
    using Binding = std::tuple<VideoCore::BufferId, AmdGpu::Buffer, u64>;

public:
    SmallUploadBatch(VideoCore::BufferCache& cache_, const Instance& instance_,
                     const Shader::Info& stage_, std::span<const Binding> bindings_) noexcept
        : cache{cache_}, instance{instance_}, stage{stage_}, bindings{bindings_} {}
    struct AuditCounts {
        u64 obtains{}, probes{}, repeated{}, dirty_queries{}, singletons{}, attempts{}, batches{},
            fallbacks{};
    };
    static bool AuditEnabled() {
        return g_upload_scan_audit;
    }
    static AuditCounts& Audit() {
        thread_local AuditCounts counts;
        return counts;
    }
    ~SmallUploadBatch() {
        if (!AuditEnabled())
            return;
        auto& a = Audit();
        thread_local u64 stages{};
        if (++stages % 10000 == 0)
            LOG_INFO(Render_Vulkan,
                     "VR upload scan: {} stages, {} obtains, {} probes, {} repeated, {} dirty "
                     "queries, {} singletons, {} attempts, {} batches, {} fallbacks",
                     stages, a.obtains, a.probes, a.repeated, a.dirty_queries, a.singletons,
                     a.attempts, a.batches, a.fallbacks);
    }
    [[gnu::noinline]] std::pair<VideoCore::Buffer*, u32> Obtain(u32 i) {
        if (AuditEnabled())
            ++Audit().obtains;
        const bool enabled = g_batch_small_uploads;
        auto& stream = cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
        if (enabled && i >= end) {
            u32 limit = i;
            while (limit < bindings.size() && Eligible(limit)) {
                const auto& [id, sharp, bytes] = bindings[limit];
                ranges[limit - i] = {sharp.base_address, static_cast<u32>(bytes)};
                ++limit;
            }
            // An eligible singleton can use the same no-wait copy path, avoiding
            // ObtainBuffer's repeated dirty check. Failed preflight keeps the fallback.
            const bool singleton = g_single_upload_fastpath;
            if (AuditEnabled()) {
                Audit().singletons += limit - i == 1;
                Audit().attempts += limit - i >= (singleton ? 1u : 2u);
            }
            if (limit - i >= (singleton ? 1u : 2u) &&
                stream.TryCopyReadOnlyBatch({ranges.data(), limit - i}, {offsets.data(), limit - i},
                                            instance.UniformMinAlignment())) {
                if (AuditEnabled())
                    ++Audit().batches;
                begin = i;
                end = limit;
            }
        }
        if (enabled && i < end)
            return {&stream, offsets[i - begin]};
        const auto& [id, sharp, bytes] = bindings[i];
        const auto& desc = stage.buffers[i];
        if (AuditEnabled())
            ++Audit().fallbacks;
        // Bit 33: BindBuffers asks the buffer for a shader-read barrier (VideoCore::SmallRead).
        VideoCore::SmallRead small_read{.shader_read = true};
        return cache.ObtainBuffer(sharp.base_address, bytes, desc.is_written, desc.is_formatted,
                                  id, &small_read);
    }

private:
    bool Eligible(u32 i) const {
        const auto& [id, sharp, bytes] = bindings[i];
        const auto& desc = stage.buffers[i];
        const bool basic = id && !desc.IsSpecial() && !desc.is_written && bytes > 0 &&
                           bytes <= VideoCore::BufferCache::CACHING_PAGESIZE && bytes != 864;
        if (AuditEnabled()) {
            static_assert(Shader::NUM_BUFFERS <= 64);
            auto& a = Audit();
            ++a.probes;
            a.repeated += (seen & (u64{1} << i)) != 0;
            seen |= u64{1} << i;
            a.dirty_queries += basic;
        }
        return basic && !cache.IsRegionGpuModified(sharp.base_address, bytes);
    }
    VideoCore::BufferCache& cache;
    const Instance& instance;
    const Shader::Info& stage;
    std::span<const Binding> bindings;
    std::array<ScratchBatch::Range, Shader::NUM_BUFFERS> ranges;
    std::array<u32, Shader::NUM_BUFFERS> offsets;
    mutable u64 seen{};
    u32 begin = 0, end = 0;
};

// FAST_CPU_CB_MEMO (on unless '0', native stereo only; read once at start-up, a plain load per
// use): BindBuffers finds a stage's prepared constants in one pass over the packet's
// (DrawPacket::Constant scanned them all again for every buffer), and asks once per stage what
// no buffer of it can change: the depth image's shared shadow test (its LRU touch does nothing
// after the first one of a tick) and the asymmetric projection.
const bool g_cb_memo = [] {
    const char* value = std::getenv("FAST_CPU_CB_MEMO");
    return !value || value[0] != '0';
}();
// FAST_CPU_CB_MEMO_VERIFY=1: every buffer also takes the old lookup and tests; a mismatch is
// logged and the old answer used.
const bool g_cb_memo_verify = [] {
    const char* value = std::getenv("FAST_CPU_CB_MEMO_VERIFY");
    return value && value[0] == '1';
}();
struct CbMemoStats {
    u64 calls{}, hits{}, misses{}, verified{}, mismatches{};
};
} // namespace

void Rasterizer::BindBuffers(const Shader::Info& stage, Shader::Backend::Bindings& binding,
                             Shader::PushData& push_data) {
    FastRender::BProfile::Scope b_profile{FastRender::BProfile::Buffers};
    ScratchProfile::Cpu scratch_cpu(9, "Rasterizer::BindBuffers");
    buffer_bindings.clear();
    std::array<const DrawPacket::ConstantBinding*, Shader::NUM_BUFFERS> prepared{};
    const bool cb_memo = g_cb_memo && NativeStereo::Enabled(); // flat mode: as before
    const bool cb_verify = cb_memo && g_cb_memo_verify;
    static thread_local CbMemoStats cb_stats;
    const auto cb_mismatch = [&](const char* what, u32 index, auto memo, auto full) {
        if (++cb_stats.mismatches <= 64 || (cb_stats.mismatches & 1023) == 0) {
            LOG_ERROR(Render_Vulkan,
                      "FAST_CPU_CB_MEMO verify: {} of shader {:#x} buffer {}: memo {}, full {}; "
                      "the full answer is used, mismatch #{}",
                      what, stage.pgm_hash, index, memo, full, cb_stats.mismatches);
        }
    };
    // FAST_CPU_CB_MEMO: this stage's prepared constants by binding, the first match as Constant
    // (a stage without buffers asks for none, so it scans nothing).
    std::array<const DrawPacket::ConstantBinding*, Shader::NUM_BUFFERS> by_binding;
    if (cb_memo && binding_packet && !stage.buffers.empty()) {
        const size_t count = std::min<size_t>(stage.buffers.size(), by_binding.size());
        std::fill_n(by_binding.begin(), count, nullptr);
        for (const auto& c : binding_packet->constant_values) {
            if (c.stage == &stage && c.binding < count && c.raw.buffer && !by_binding[c.binding])
                by_binding[c.binding] = &c;
        }
    }

    {
        ScratchProfile::Cpu scratch_lookup(16, "BindBuffers::ResolveRanges");
        for (const auto& desc : stage.buffers) {
            const u32 index = static_cast<u32>(buffer_bindings.size());
            const auto vsharp = resource_bindings ? resource_bindings->buffer_values[index]
                                                 : desc.GetSharp(stage);
            if (binding_packet) {
                const DrawPacket::ConstantBinding* c =
                    cb_memo ? by_binding[index] : binding_packet->Constant(stage, index);
                if constexpr (Common::Log::Enabled) {
                    if (cb_memo) {
                        ++(c ? cb_stats.hits : cb_stats.misses);
                    }
                }
                if (cb_verify) {
                    ++cb_stats.verified;
                    const auto* full = binding_packet->Constant(stage, index);
                    if (full != c) {
                        cb_mismatch("prepared constant", index, static_cast<const void*>(c),
                                    static_cast<const void*>(full));
                        c = full;
                    }
                }
                if (c && ((c->flat && desc.buffer_type == Shader::BufferType::Flatbuf &&
                           c->bytes == stage.FlatUserData().size() * sizeof(u32)) ||
                          (!c->flat && !desc.IsSpecial() && !desc.is_written &&
                           std::memcmp(&c->sharp, &vsharp, sizeof(vsharp)) == 0)))
                    prepared[index] = c;
            }
            if (prepared[index]) {
                buffer_bindings.emplace_back(VideoCore::BufferId{}, vsharp, prepared[index]->bytes);
            } else if (!desc.IsSpecial() && vsharp.base_address != 0 && vsharp.GetSize() > 0) {
                const u64 size = memory->ClampRangeSize(vsharp.base_address, vsharp.GetSize());
                const auto buffer_id = buffer_cache.FindBuffer(vsharp.base_address, size);
                buffer_bindings.emplace_back(buffer_id, vsharp, size);
            } else {
                buffer_bindings.emplace_back(VideoCore::BufferId{}, vsharp, 0);
            }
        }
    }
    u64 native_paired_buffers = 0, native_projected_buffers = 0;
    SmallUploadBatch upload_batch{
        buffer_cache, instance, stage, {buffer_bindings.data(), buffer_bindings.size()}};
    const auto shared_shadow_test = [&] {
        return stage.l_stage != Shader::LogicalStage::Compute && db_desc.first &&
               NativeStereo::SharedShadowAtlas(texture_cache.GetImage(db_desc.first).info);
    };
    // FAST_CPU_CB_MEMO: asked once here (the shadow test only when a buffer follows, as the loop
    // asked it); with the switch off every buffer asks again below.
    const bool shared_shadow_memo = cb_memo && !buffer_bindings.empty() && shared_shadow_test();
    bool asymmetric_memo = cb_memo && NativeStereo::AsymmetricProjection();
    const u32 alignment = instance.StorageMinAlignment(); // a device limit
    const bool mirror_flip = VrMirrorFlip(); // FAST_GPU_MIRROR_FLIP: CameraBuffer's `mirrored`
    // FAST_GPU_LIGHT_EYE: this stage's buffer 0 was paired as a deferred light's light208 camera,
    // with a camera record of light_view_scale; only then is its light list (buffer 1) paired.
    bool light_view_paired = false;
    float light_view_scale = 0.f;
    // Second pass to re-bind buffers that were updated after binding
    for (u32 i = 0; i < buffer_bindings.size(); i++) {
        const auto& [buffer_id, vsharp, size] = buffer_bindings[i];
        const auto& desc = stage.buffers[i];
        bool shared_shadow = cb_memo ? shared_shadow_memo : shared_shadow_test();
        if (cb_verify) {
            const bool full = shared_shadow_test();
            if (full != shared_shadow) {
                cb_mismatch("shared shadow test", i, shared_shadow, full);
                shared_shadow = full;
            }
            const bool asymmetric = NativeStereo::AsymmetricProjection();
            if (asymmetric != asymmetric_memo) {
                cb_mismatch("asymmetric projection", i, asymmetric_memo, asymmetric);
                asymmetric_memo = asymmetric;
            }
        }
        if (shared_shadow && !desc.IsSpecial() && !desc.is_written && size == 864) {
            const auto* scene = static_cast<const float*>(
                ScratchDraw::MemorySource(reinterpret_cast<const void*>(vsharp.base_address), 864));
            ASSERT_MSG(scene[66] == 0.f && scene[67] == 1.f,
                       "Shared shadow experiment encountered a non-orthographic camera");
        }
        NativeStereo::CameraConstants temporal_camera;
        const VideoCore::Buffer* owner = nullptr; // bit 25: the buffer this descriptor names
        const bool capture_temporal = vr_temporal && vr_temporal->gbuffer &&
                                      stage.l_stage == Shader::LogicalStage::Vertex &&
                                      vr_temporal->scene.Objects();
        const auto* constant = prepared[i];
        constant_bind_hits += constant != nullptr;
        bool projection_paired = false, mirrored = false;
        vk::DescriptorBufferInfo native_camera;
        const bool camera =
            NativeStereo::Enabled() &&
            (stage.l_stage == Shader::LogicalStage::Compute || recording_view_mask != 0) &&
            !desc.IsSpecial() && !desc.is_written && size == 864 &&
            (constant ? constant->camera : NativeStereo::CameraBuffer(
                buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream),
                reinterpret_cast<const float*>(vsharp.base_address), alignment,
                VR::DrawFrameRecord(), native_camera, capture_temporal ? &temporal_camera : nullptr,
                &projection_paired, mirror_flip ? &mirrored : nullptr));
        if (camera && constant) {
            ++constant_pair_hits;
            native_camera = constant->left;
            projection_paired = constant->projected;
            mirrored = constant->mirrored;
            NativeStereo::RegisterCameraPair(constant->left, constant->right);
            if (capture_temporal) {
                std::memcpy(temporal_camera[0].data(), constant_ring->mapped_data.data() + constant->left.offset, 864);
                std::memcpy(temporal_camera[1].data(), constant_ring->mapped_data.data() + constant->right.offset, 864);
            }
        }
        if (camera && capture_temporal) {
            const auto& viewport = ScratchDraw::Registers(liverpool).viewports[0];
            vr_temporal->scene.CaptureCamera(temporal_camera, viewport.xscale < 0 ? -1.f : 1.f,
                                             viewport.yscale < 0 ? -1.f : 1.f);
        }
        if (camera)
            native_paired_buffers |= u64{1} << i;
        if (projection_paired)
            native_projected_buffers |= u64{1} << i;
        if (camera && stage.l_stage != Shader::LogicalStage::Compute && !vr_mirror_noted)
            VrMirrorCamera(mirrored, vsharp.base_address);
        const bool light_size = size == 208 || size == 272 || size == 336 || size == 416;
        // A light of a planar reflection's pass (VrInMirrorPass): paired here upside down, also
        // when Stage A paired it (A does not know the pass), from A's copy of its bytes. Its draw's
        // camera is noted first (VrMirrorAhead): it may bind after the light.
        if (mirror_flip && light_size && stage.l_stage == Shader::LogicalStage::Fragment &&
            recording_view_mask == 3 && !vr_mirror_noted && !vr_mirror_looked &&
            !desc.IsSpecial() && !desc.is_written)
            VrMirrorAhead();
        const bool mirror_light =
            light_size && stage.l_stage == Shader::LogicalStage::Fragment && VrInMirrorPass();
        const bool ring_light = constant && !mirror_light;
        const bool lighting =
            (cb_memo ? asymmetric_memo : NativeStereo::AsymmetricProjection()) &&
            (stage.l_stage == Shader::LogicalStage::Compute ||
             (recording_view_mask != 0 && stage.l_stage == Shader::LogicalStage::Fragment)) &&
            !desc.IsSpecial() && !desc.is_written && light_size &&
            (constant || !buffer_cache.IsRegionGpuModified(vsharp.base_address, size)) &&
            (ring_light ? constant->lighting : NativeStereo::LightingBuffer(
                buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream),
                constant ? reinterpret_cast<const float*>(constant_ring->mapped_data.data() +
                                                          constant->raw.offset)
                         : reinterpret_cast<const float*>(vsharp.base_address),
                u32(size), alignment, VR::DrawFrameRecord(), native_camera, mirror_light,
                i == 0 ? &light_view_scale : nullptr));
        if (lighting && ring_light) {
            ++constant_pair_hits;
            native_camera = constant->left;
            NativeStereo::RegisterCameraPair(constant->left, constant->right);
        }
        if (i == 0 && lighting && size == 208) {
            light_view_paired = true;
            // Stage A paired the ring copy with the packet's record (PrepareConstants); without
            // one the scale stays 0 and LightListBuffer refuses.
            if (ring_light)
                light_view_scale = binding_packet ? binding_packet->camera.world_scale : 0.f;
        }
        // The light shafts' and the glow god rays' centre per eye (NativeStereo::ScreenPointShader).
        const bool screen_point =
            !camera && !lighting &&
            (cb_memo ? asymmetric_memo : NativeStereo::AsymmetricProjection()) &&
            recording_view_mask != 0 && stage.l_stage == Shader::LogicalStage::Fragment &&
            i == 0 && NativeStereo::ScreenPointShader(stage.pgm_hash) && !desc.IsSpecial() &&
            !desc.is_written && (constant || !buffer_cache.IsRegionGpuModified(vsharp.base_address, size)) &&
            NativeStereo::ScreenPointBuffer(
                buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream),
                constant ? reinterpret_cast<const float*>(constant_ring->mapped_data.data() +
                                                          constant->raw.offset)
                         : reinterpret_cast<const float*>(vsharp.base_address),
                u32(size), alignment, constant == nullptr, stage.pgm_hash, VR::DrawFrameRecord(),
                native_camera);
        // A compute pass over a pixel rectangle of the eye images (Info::vr_screen_rect, e.g. the
        // heat haze's grab of the scene): each eye's rectangle where that eye sees the effect.
        const auto& rect = stage.vr_screen_rect;
        bool screen_rect = false;
        if (!camera && !lighting && !screen_point && rect.Valid() && i == rect.binding &&
            rect.image < stage.images.size() && NativeStereo::Enabled() &&
            stage.l_stage == Shader::LogicalStage::Compute && !desc.IsSpecial() &&
            !desc.is_written && vr_eye_extent[0] != 0 &&
            (constant || !buffer_cache.IsRegionGpuModified(vsharp.base_address, size))) {
            const auto image = stage.images[rect.image].GetSharp(stage);
            const u32 width = image.width + 1;
            const u32 height = image.height + 1;
            const auto& cs = std::as_const(ScratchDraw::ComputeRegisters(liverpool));
            screen_rect =
                width == vr_eye_extent[0] && height == vr_eye_extent[1] &&
                NativeStereo::ScreenRectBuffer(
                    buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream),
                    constant ? reinterpret_cast<const u32*>(constant_ring->mapped_data.data() +
                                                            constant->raw.offset)
                             : reinterpret_cast<const u32*>(vsharp.base_address),
                    u32(size), alignment, constant == nullptr,
                    {rect.dst[0], rect.dst[1], rect.src[0], rect.src[1], rect.end[0], rect.end[1]},
                    width, height, {cs.num_thread_x.full, cs.num_thread_y.full},
                    VR::DrawFrameRecord(), native_camera, vr_rect_groups);
        }
        // A deferred light's list, buffer 1 after its paired light208 (NativeStereo::LightListShader):
        // a copy per eye with its lights moved like that eye's camera.
        const bool light_list =
            light_view_paired && i == 1 && !camera && !lighting && !screen_point && !screen_rect &&
            stage.l_stage == Shader::LogicalStage::Fragment && recording_view_mask != 0 &&
            !desc.IsSpecial() && !desc.is_written && NativeStereo::LightListShader(stage.pgm_hash) &&
            (constant || !buffer_cache.IsRegionGpuModified(vsharp.base_address, size)) &&
            NativeStereo::LightListBuffer(
                buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream),
                constant ? reinterpret_cast<const float*>(constant_ring->mapped_data.data() +
                                                          constant->raw.offset)
                         : reinterpret_cast<const float*>(vsharp.base_address),
                u32(size), alignment, constant == nullptr, stage.pgm_hash, light_view_scale,
                native_camera);
        if (g_camera_audit && NativeStereo::AsymmetricProjection() && !desc.IsSpecial() &&
            !desc.is_written &&
            (size == 864 || size == 208 || size == 272 || size == 336 || size == 416) &&
            !buffer_cache.IsRegionGpuModified(vsharp.base_address, size))
            NativeStereo::AuditCameraBuffer(reinterpret_cast<const float*>(vsharp.base_address),
                                            u32(size), stage.pgm_hash, u32(stage.l_stage), i,
                                            camera || lighting, projection_paired || lighting,
                                            VR::DrawFrameRecord(),
                                            (camera && mirrored) || (lighting && mirror_light));
        // Buffer is not from the cache, either a special buffer or unbound.
        if (camera || lighting || screen_point || screen_rect || light_list) {
            buffer_infos.push_back(native_camera);
        } else if (constant) {
            buffer_infos.push_back(constant->raw);
        } else if (!buffer_id) {
            if (desc.buffer_type == Shader::BufferType::GdsBuffer) {
                const auto* gds_buf = buffer_cache.GetGdsBuffer();
                buffer_infos.emplace_back(gds_buf->Handle(), 0, gds_buf->SizeBytes());
                owner = gds_buf;
            } else if (desc.buffer_type == Shader::BufferType::Flatbuf) {
                auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                const u32 ubo_size = stage.FlatUserData().size() * sizeof(u32);
                const u64 offset = vk_buffer.Copy(stage.FlatUserData().data(), ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::ClipPlanes) {
                // Permutations compiled without enabled planes never read the buffer, so the
                // declared binding is satisfied with a null descriptor instead of a copy.
                if (std::as_const(ScratchDraw::Registers(liverpool))
                        .clipper_control.user_clip_plane_enable == 0) {
                    buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
                } else {
                    auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                    std::array<float, AmdGpu::NUM_CLIP_PLANES * 4> planes{};
                    for (u32 i = 0; i < AmdGpu::NUM_CLIP_PLANES; ++i) {
                        const auto& plane =
                            std::as_const(ScratchDraw::Registers(liverpool)).clip_user_data[i];
                        planes[i * 4 + 0] = std::bit_cast<float>(plane.data_x);
                        planes[i * 4 + 1] = std::bit_cast<float>(plane.data_y);
                        planes[i * 4 + 2] = std::bit_cast<float>(plane.data_z);
                        planes[i * 4 + 3] = std::bit_cast<float>(plane.data_w);
                    }
                    const u32 ubo_size = static_cast<u32>(sizeof(planes));
                    const u64 offset = vk_buffer.Copy(planes.data(), ubo_size, alignment);
                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
                }
            } else if (desc.buffer_type == Shader::BufferType::BdaPagetable) {
                const auto* bda_buffer = buffer_cache.GetBdaPageTableBuffer();
                buffer_infos.emplace_back(bda_buffer->Handle(), 0, bda_buffer->SizeBytes());
                owner = bda_buffer;
            } else if (desc.buffer_type == Shader::BufferType::FaultBuffer) {
                const auto* fault_buffer = buffer_cache.GetFaultBuffer();
                buffer_infos.emplace_back(fault_buffer->Handle(), 0, fault_buffer->SizeBytes());
                owner = fault_buffer;
            } else if (desc.buffer_type == Shader::BufferType::SharedMemory) {
                auto& lds_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                const auto& cs_program = std::as_const(ScratchDraw::ComputeRegisters(liverpool));
                const auto lds_size = cs_program.SharedMemSize() * cs_program.NumWorkgroups();
                const auto [data, offset] = lds_buffer.Map(lds_size, alignment);
                std::memset(data, 0, lds_size);
                buffer_infos.emplace_back(lds_buffer.Handle(), offset, lds_size);
            } else {
                buffer_infos.emplace_back(VK_NULL_HANDLE, 0, VK_WHOLE_SIZE);
            }
        } else {
            const auto [vk_buffer, offset] = upload_batch.Obtain(i);
            const u32 offset_aligned = Common::AlignDown(offset, alignment);
            const u32 adjust = offset - offset_aligned;
            if (adjust % 4 != 0) {
                LOG_WARNING(Render_Vulkan, "Buffer binding {} in shader {:#x} isn't dword aligned",
                            i, stage.pgm_hash);
            }
            push_data.AddOffset(binding.buffer, adjust);
            buffer_infos.emplace_back(vk_buffer->Handle(), offset_aligned, size + adjust);
            owner = vk_buffer;
            if (auto barrier =
                    vk_buffer->GetBarrier(desc.is_written ? vk::AccessFlagBits2::eShaderWrite
                                                          : vk::AccessFlagBits2::eShaderRead,
                                          vk::PipelineStageFlagBits2::eAllCommands)) {
                buffer_barriers.emplace_back(*barrier);
                // Only BindBuffers has added barriers so far: barrier_hints index them.
                if (stereo_handles && buffer_barriers.size() <= barrier_hints.capacity()) {
                    barrier_hints.resize(buffer_barriers.size() - 1);
                    barrier_hints.push_back({vk_buffer->Handle(), vk_buffer->NativeEntry()});
                }
            }
            if (desc.is_written && desc.is_formatted) {
                texture_cache.InvalidateMemoryFromGPU(vsharp.base_address, size);
            }
        }

        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eStorageBuffer;
        set_write.pBufferInfo = &buffer_infos.back();
        ++binding.buffer;
        if (stereo_handles) {
            // Bit 25: the registry entry of this descriptor's buffer. Camera, ring and utility
            // descriptors name the stream buffer or the constant ring (both registered).
            const vk::Buffer left = buffer_infos.back().buffer;
            if (!owner && left) {
                auto& stream = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                if (left == stream.Handle())
                    owner = &stream;
                else if (constant_ring && left == constant_ring->Handle())
                    owner = constant_ring.get();
            }
            // FAST_CPU_FUSED_PAIR: a camera, lighting, screen point, screen rect or light list
            // descriptor's own pair, registered just above (the constant's, else the last one):
            // what the pairing scan finds for this left half, every pair's left being an allocation
            // of its own. A reflection pass's light paired here has its own (mirror_light); a light
            // list always has (LightListBuffer), also from a prepared constant.
            vk::DescriptorBufferInfo camera_right{};
            if (native_handles.fused &&
                (camera || lighting || screen_point || screen_rect || light_list)) {
                camera_right = constant && (camera || (lighting && ring_light))
                                   ? constant->right
                                   : NativeStereo::LastCameraRight();
            }
            buffer_hints.resize(buffer_infos.size() - 1);
            buffer_hints.push_back({left, owner ? owner->NativeEntry() : nullptr, camera_right});
        }
    }
    // The final position comes from the hardware vertex stage, or from the geometry stage, which
    // may build it from what its export stage computed. Either may have read a paired camera:
    // the flags of all of them are combined, so neither the eye shift nor the remap is applied twice.
    if (NativeStereo::Enabled() &&
        (stage.stage == Shader::Stage::Vertex || stage.stage == Shader::Stage::Geometry ||
         stage.stage == Shader::Stage::Export)) {
        u32 flags = 0;
        if (stage.native_camera_buffers &&
            (stage.native_camera_buffers & native_paired_buffers) == stage.native_camera_buffers)
            flags |= Shader::PushData::NativeCameraPaired;
        if (stage.native_projection_buffers &&
            (stage.native_projection_buffers & native_projected_buffers) ==
                stage.native_projection_buffers)
            flags |= Shader::PushData::NativeProjectionPaired;
        push_data.vr_hud = float(u32(push_data.vr_hud) | flags);
    }
    if constexpr (Common::Log::Enabled) {
        if (cb_memo && (++cb_stats.calls & 0xfffff) == 0) {
            LOG_INFO(Render_Vulkan,
                     "FAST_CPU_CB_MEMO: {} stages; prepared constant lookups hits={} misses={}; "
                     "verified={} mismatches={}",
                     cb_stats.calls, cb_stats.hits, cb_stats.misses, cb_stats.verified,
                     cb_stats.mismatches);
        }
    }
}

const VideoCore::TextureCache::ImageDesc& Rasterizer::TextureDescFor(
    const AmdGpu::Image& tsharp, const Shader::ImageResource& res, const u64* prepared_hash) {
    if (FastRender::CacheStatsEnabled())
        FastRender::g_counters.texture_desc_calls.fetch_add(1, std::memory_order_relaxed);
    static_assert(sizeof(AmdGpu::Image) == sizeof(std::array<u64, 4>));
    std::array<u64, 4> raw;
    std::memcpy(raw.data(), &tsharp, sizeof(raw));
    const u32 flags =
        (res.is_written ? 1u : 0u) | (res.is_depth ? 2u : 0u) | (res.is_array ? 4u : 0u);
    const u64 hash = prepared_hash ? *prepared_hash : PreparedResources::ImageHash(raw, flags);
    const auto matches = [&](const TextureDescMemo& entry) {
        return entry.valid && entry.flags == flags && entry.tsharp == raw;
    };
    auto* selected = &texture_desc_memo[hash & (TextureDescMemoSize - 1)];
    if (FastRender::On(FastRender::TextureDescTwoWay)) {
        const size_t index = hash & (TextureDescSets - 1);
        auto& ways = texture_desc_two_way[index];
        if (matches(ways[0]))
            selected = &ways[0];
        else if (matches(ways[1]))
            selected = &ways[1];
        else if (!ways[0].valid)
            selected = &ways[0];
        else if (!ways[1].valid)
            selected = &ways[1];
        else {
            auto& victim = texture_desc_victim[index];
            selected = &ways[victim];
            victim ^= 1;
        }
    }
    auto& memo = *selected;
    if (!memo.valid || memo.flags != flags || memo.tsharp != raw) {
        if (FastRender::CacheStatsEnabled())
            FastRender::g_counters.texture_desc_misses.fetch_add(1, std::memory_order_relaxed);
        memo.tsharp = raw;
        memo.flags = flags;
        memo.desc = VideoCore::TextureCache::ImageDesc{tsharp, res};
        memo.valid = true;
    }
    return memo.desc;
}

void Rasterizer::TransitTexture(VideoCore::Image& image, bool is_storage,
                                const VideoCore::SubresourceRange& range) {
    // The image is either bound as storage in a separate descriptor or bound as render
    // target in feedback loop. Depth images are excluded because they can't be bound as
    // storage and feedback loop doesn't make sense for them
    if ((image.binding.force_general || image.binding.is_target) && !image.info.props.is_depth) {
        image.Transit(instance.IsAttachmentFeedbackLoopLayoutSupported() && image.binding.is_target
                          ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                          : vk::ImageLayout::eGeneral,
                      vk::AccessFlagBits2::eShaderRead |
                          (image.info.props.is_depth
                               ? vk::AccessFlagBits2::eDepthStencilAttachmentWrite
                               : vk::AccessFlagBits2::eColorAttachmentWrite |
                                     vk::AccessFlagBits2::eColorAttachmentRead),
                      {});
    } else {
        if (is_storage) {
            image.Transit(vk::ImageLayout::eGeneral,
                          vk::AccessFlagBits2::eShaderRead | vk::AccessFlagBits2::eShaderWrite,
                          range);
        } else {
            const auto new_layout = image.info.props.is_depth
                                        ? vk::ImageLayout::eDepthStencilReadOnlyOptimal
                                        : vk::ImageLayout::eShaderReadOnlyOptimal;
            image.Transit(new_layout, vk::AccessFlagBits2::eShaderRead, range);
        }
    }
}

bool Rasterizer::BindMemoValid(const BindMemo::Entry& entry) {
    // The registry epoch is the entry's (BindTable::Find): every image in it is still registered
    // and FindImage would give every T# the same image. What can change without a registry
    // change: the depth link of a stencil image, the backing (a sample-count change: another
    // view), a pending rebind.
    for (u32 i = 0; i < entry.num_bindings; ++i) {
        const auto& b = entry.bindings[i];
        if (!b.image_id) {
            continue;
        }
        auto& raw = texture_cache.GetImage(b.raw_id);
        const VideoCore::ImageId depth_id = texture_cache.GetAssociatedDepth(raw);
        if (b.raw_id == b.image_id ? bool(depth_id) : depth_id != b.image_id) {
            return false;
        }
        const auto& image = texture_cache.GetImage(b.image_id);
        if (image.binding.needs_rebind || static_cast<const void*>(image.backing) != b.backing) {
            return false;
        }
    }
    return true;
}

void Rasterizer::BindTexturesFromMemo(const BindMemo::Entry& entry,
                                      Shader::Backend::Bindings& binding) {
    const u32 first_image_idx = image_infos.size();
    // First pass: what FindImage does besides the lookup (the found image's last access, its LRU
    // place) and the binding flags (no storage image in a remembered stage: force_general stays).
    const u64 tick = scheduler.CurrentTick();
    for (u32 i = 0; i < entry.num_bindings; ++i) {
        const auto& b = entry.bindings[i];
        if (!b.image_id) {
            continue;
        }
        auto& raw = texture_cache.GetImage(b.raw_id);
        if (b.raw_id == b.image_id) {
            raw.tick_accessed_last = tick; // UpdateImage below gives it its LRU place
        } else {
            texture_cache.MarkFound(b.raw_id); // a stencil T#'s own image: tick and LRU place
        }
        auto& image = b.raw_id == b.image_id ? raw : texture_cache.GetImage(b.image_id);
        image.binding.is_bound = 1u;
    }
    // Second pass: as the full path, with the remembered view and range (FindTexture's
    // UpdateImage: the CPU-write tracking, the LRU, the upload of a CPU-dirty image).
    for (u32 i = 0; i < entry.num_bindings; ++i) {
        const auto& b = entry.bindings[i];
        if (!b.image_id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            if (stereo_handles)
                NoteViewHint({}, {});
            continue;
        }
        bound_images.emplace_back(b.image_id);
        auto& image = texture_cache.GetImage(b.image_id);
        texture_cache.UpdateImage(b.image_id);
        TransitTexture(image, false, b.range);
        image.usage.texture |= 1u;
        image_infos.emplace_back(VK_NULL_HANDLE, b.view, image.backing->state.layout);
        if (stereo_handles) {
            if (!b.right) // an entry made with the bit off: one registry lookup, then kept
                b.right = NativeStereo::RightView(b.view);
            NoteViewHint(b.view, b.right);
        }
    }
    u32 image_info_idx = first_image_idx;
    for (u32 k = 0; k < entry.key.count; ++k) {
        const u32 array_size = entry.array_sizes[k];
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType = vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];
        image_info_idx += array_size;
        binding.unified += array_size;
    }
}

namespace {
// FAST_CPU_TEX_MEMO (on unless '0'; read once at start-up, a plain load per use): the bind
// memo's lean hit path (bit 26) notes each view's native stereo right as a view hint (bit 25),
// as the full path and the slow memo path already do. Without the hints PairDescriptors looked
// every view of a memo hit up in the registry (most image descriptors, about 5 hash lookups per
// draw).
const bool g_tex_memo = [] {
    const char* value = std::getenv("FAST_CPU_TEX_MEMO");
    return !value || value[0] != '0';
}();
// FAST_CPU_TEX_MEMO_VERIFY=1: every carried right is also looked up in the registry (what
// PairDescriptors would do) and compared; a mismatch is logged and the registry's answer used.
const bool g_tex_memo_verify = [] {
    const char* value = std::getenv("FAST_CPU_TEX_MEMO_VERIFY");
    return value && value[0] == '1';
}();
} // namespace

vk::ImageView Rasterizer::TexMemoRight(const BindMemoBinding& b, bool verify) {
    if (b.right) {
        if constexpr (Common::Log::Enabled) {
            ++tex_memo.hints;
        }
    } else {
        // An entry made with bit 25 off: one registry lookup, then kept (a view's right never
        // changes while the view lives, and the entry is valid only while its view does).
        b.right = NativeStereo::RightView(b.view);
        if constexpr (Common::Log::Enabled) {
            ++tex_memo.resolved;
        }
    }
    if (!verify) {
        return b.right;
    }
    ++tex_memo.verified;
    const vk::ImageView full = NativeStereo::RightView(b.view);
    if (full != b.right) {
        if (++tex_memo.mismatches <= 64) {
            LOG_ERROR(Render_Vulkan,
                      "FAST_CPU_TEX_MEMO verify: view {:#x} carried right {:#x}, registry {:#x}",
                      u64(VkImageView(b.view)), u64(VkImageView(b.right)), u64(VkImageView(full)));
        }
        return full; // the registry decides
    }
    return b.right;
}

// FAST_BIND_MEMO_HIT_VERIFY=N, read once at start-up (no function-local static's guard).
static const u32 g_bind_memo_hit_verify_period = [] {
    const char* v = std::getenv("FAST_BIND_MEMO_HIT_VERIFY");
    const int n = v ? std::atoi(v) : 0;
    return n > 0 ? u32(n) : 0u;
}();

bool Rasterizer::BindTexturesFromMemoFast(const BindMemo::Entry& entry,
                                          Shader::Backend::Bindings& binding) {
    // FAST bit 26 (bbport vk_rasterizer.cpp:3593-3677): one validation pass that resolves each
    // image once, then one bind pass on those images, instead of BindMemoValid and two passes
    // with a GetImage (an LRU touch) each. The bound image gets its LRU place from UpdateImage
    // (its fast path implies a touch in this GC period); a stencil T#'s own image from
    // MarkFoundOnce (once per GC period, no lock per call; bbport texture_cache.h:103-105).
    // Nothing here registers an image, so the pointers stay valid (only an insert moves them).
    const u32 verify_period = g_bind_memo_hit_verify_period;
    auto& stats = bind_memo_hit;
    const bool verify = verify_period != 0 && (stats.hits + stats.invalid) % verify_period == 0;
    const u32 count = entry.num_bindings;
    std::array<VideoCore::Image*, BindMemo::Entry::MaxBindings> raws;
    std::array<VideoCore::Image*, BindMemo::Entry::MaxBindings> images;
    // BindMemoValid's checks: the registry epoch is the entry's (every image still registered,
    // FindImage would give every T# the same image); the depth link of a stencil image, the
    // backing and a pending rebind can change without it.
    bool valid = true;
    for (u32 i = 0; i < count; ++i) {
        const auto& b = entry.bindings[i];
        if (!b.image_id) {
            continue;
        }
        auto& raw = texture_cache.PeekImage(b.raw_id);
        const VideoCore::ImageId depth_id = texture_cache.GetAssociatedDepth(raw);
        if (b.raw_id == b.image_id ? bool(depth_id) : depth_id != b.image_id) {
            valid = false;
            break;
        }
        auto& image = b.raw_id == b.image_id ? raw : texture_cache.PeekImage(b.image_id);
        if (image.binding.needs_rebind || static_cast<const void*>(image.backing) != b.backing) {
            valid = false;
            break;
        }
        raws[i] = &raw;
        images[i] = &image;
    }
    if (verify) {
        // FAST_BIND_MEMO_HIT_VERIFY=N: every Nth use the slow validation too (GetImage).
        ++stats.verified;
        const bool slow = BindMemoValid(entry);
        if (slow != valid) {
            if (++stats.mismatches <= 64) {
                LOG_ERROR(Render_Vulkan,
                          "FAST bind_memo_hit verify: validation fast={} slow={} ({} bindings)",
                          valid, slow, count);
            }
            valid = false; // the full path decides
        }
    }
    if (!valid) {
        ++stats.invalid;
        return false;
    }

    const u32 first_image_idx = image_infos.size();
    const u64 tick = scheduler.CurrentTick();
    // FAST_CPU_TEX_MEMO: the views' hints for PairDescriptors (native stereo, bit 25 only).
    const bool hints = stereo_handles && g_tex_memo;
    const bool tex_verify = hints && g_tex_memo_verify;
    for (u32 i = 0; i < count; ++i) {
        const auto& b = entry.bindings[i];
        if (!b.image_id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            if (hints)
                NoteViewHint({}, {});
            continue;
        }
        auto& image = *images[i];
        if (b.raw_id == b.image_id) {
            image.tick_accessed_last = tick; // UpdateImage below gives it its LRU place
        } else if (texture_cache.MarkFoundOnce(*raws[i], tick)) {
            ++stats.lru_locks; // a stencil T#'s own image: tick and LRU place
        }
        image.binding.is_bound = 1u; // no storage image in a remembered stage: force_general stays
        bound_images.emplace_back(b.image_id);
        texture_cache.UpdateImage(b.image_id);
        TransitTexture(image, false, b.range);
        image.usage.texture |= 1u;
        image_infos.emplace_back(VK_NULL_HANDLE, b.view, image.backing->state.layout);
        if (hints)
            NoteViewHint(b.view, TexMemoRight(b, tex_verify));
    }
    u32 image_info_idx = first_image_idx;
    for (u32 k = 0; k < entry.key.count; ++k) {
        const u32 array_size = entry.array_sizes[k];
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType = vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];
        image_info_idx += array_size;
        binding.unified += array_size;
    }
    if constexpr (Common::Log::Enabled) {
        if (hints && (++tex_memo.stages & 0x3FFFF) == 0) {
            LOG_INFO(Render_Vulkan,
                     "FAST_CPU_TEX_MEMO view hints: stages={} hits={} misses={} verified={} "
                     "mismatches={}",
                     tex_memo.stages, tex_memo.hints, tex_memo.resolved, tex_memo.verified,
                     tex_memo.mismatches);
        }
    }

    if (verify) {
        // Every image is still the one resolved and its LRU entry carries this GC period.
        for (u32 i = 0; i < count; ++i) {
            const auto& b = entry.bindings[i];
            if (!b.image_id) {
                continue;
            }
            const bool current = images[i] == &texture_cache.PeekImage(b.image_id) &&
                                 raws[i] == &texture_cache.PeekImage(b.raw_id);
            const bool touched = current && texture_cache.LruTouchedThisPeriod(*images[i]) &&
                                 texture_cache.LruTouchedThisPeriod(*raws[i]);
            if (!touched && ++stats.mismatches <= 64) {
                LOG_ERROR(Render_Vulkan,
                          "FAST bind_memo_hit verify: binding {} current={} LRU not touched "
                          "in this GC period (stencil={})",
                          i, current, b.raw_id != b.image_id);
            }
        }
    }
    if ((++stats.hits & 0x3FFFF) == 0) {
        LOG_INFO(Render_Vulkan,
                 "FAST bind_memo_hit: hits={} misses={} invalid={} lru_locks={} verified={} "
                 "mismatches={}",
                 stats.hits, stats.misses, stats.invalid, stats.lru_locks, stats.verified,
                 stats.mismatches);
    }
    return true;
}

void Rasterizer::BindTextures(const Shader::Info& stage, Shader::Backend::Bindings& binding) {
    ScratchProfile::Cpu scratch_cpu(10, "Rasterizer::BindTextures");
    image_bindings.clear();
    const u32 first_image_idx = image_infos.size();

    // perf_bind_memo (vk_perf_memo.h): the bindings remembered for exactly these T#s. Not for a
    // stage with a storage image (FindTexture marks it GPU-modified and may queue a read-back)
    // or with more images than the key holds.
    const BindMemo::Entry* found = nullptr;
    static constexpr auto BindMemoHit = static_cast<FastRender::Bit>(26);
    bool hit_fast = false;
    bool keyed = false;
    u64 key_hash = 0;
    u64 registry_before = 0;
    const auto& key = resource_bindings ? resource_bindings->key : bind_key;
    if (PerfMemo::g_switches.Get().bind_memo && !stage.images.empty()) {
        if (resource_bindings) {
            keyed = resource_bindings->keyed;
            key_hash = resource_bindings->key_hash;
        } else {
            bind_key.Clear();
            keyed = true;
            for (const auto& image_desc : stage.images) {
                if (image_desc.is_written) {
                    keyed = false;
                    break;
                }
                bind_key.Add(image_desc.GetSharp(stage),
                             PerfMemo::BindAttrs(image_desc.is_depth, image_desc.is_array,
                                                 image_desc.is_written,
                                                 static_cast<u32>(image_desc.mip_fallback_mode),
                                                 image_desc.constant_mip_index));
            }
            keyed = keyed && bind_key.Usable();
            key_hash = keyed ? bind_key.Hash() : 0;
        }
        if (keyed) {
            registry_before = texture_cache.RegistryEpoch();
            found = bind_memo.Find(key, key_hash, registry_before, PerfMemo::Epoch());
            // FAST bit 26: the lean hit path validates while it resolves the images
            // (BindTexturesFromMemoFast); VR_PERF_CHECK=1 still validates here first.
            hit_fast = FastRender::On(BindMemoHit);
            if (hit_fast && !found) {
                ++bind_memo_hit.misses;
            }
            if (found && (!hit_fast || PerfMemo::CheckMode()) && !BindMemoValid(*found)) {
                found = nullptr;
                ++bind_invalid;
            }
        } else {
            ++bind_uncacheable;
        }
    }
    if (found && !PerfMemo::CheckMode()) {
        if (!hit_fast) {
            ++bind_counters.hits;
            BindTexturesFromMemo(*found, binding);
            BindSamplers(stage, binding);
            return;
        }
        if (BindTexturesFromMemoFast(*found, binding)) {
            ++bind_counters.hits;
            BindSamplers(stage, binding);
            return;
        }
        found = nullptr; // an image changed (nothing was bound): as BindMemoValid's rejection
        ++bind_invalid;
    }

    // VR_PERF_CHECK=1 with a remembered entry: the memo's path runs first, its descriptor writes,
    // image infos and bound images are kept aside and rolled back, then the full path below makes
    // the bindings that are used and the two are compared field by field.
    const u32 chk_write0 = set_write_index;
    const u32 chk_bound0 = bound_images.size();
    const u32 chk_unified0 = binding.unified;
    boost::container::small_vector<vk::DescriptorImageInfo, 16> chk_infos;
    boost::container::small_vector<vk::WriteDescriptorSet, 16> chk_writes;
    boost::container::small_vector<VideoCore::ImageId, 16> chk_bound;
    u32 chk_unified = 0;
    if (found) {
        if (hit_fast) {
            (void)BindTexturesFromMemoFast(*found, binding); // validated above: binds
        } else {
            BindTexturesFromMemo(*found, binding);
        }
        chk_infos.assign(image_infos.begin() + first_image_idx, image_infos.end());
        chk_writes.assign(set_writes.begin() + chk_write0, set_writes.begin() + set_write_index);
        chk_bound.assign(bound_images.begin() + chk_bound0, bound_images.end());
        chk_unified = binding.unified;
        image_infos.resize(first_image_idx);
        bound_images.resize(chk_bound0);
        set_write_index = chk_write0;
        binding.unified = chk_unified0;
    }

    // For loading/storing to explicit mip levels, when no native instruction support, bind an array
    // of descriptors consecutively, 1 for each mip level. The shader can index this with LOD
    // operand.
    // This array holds the size of each consecutive array with the number of bindings consumed.
    // This is currently always 1 for anything other than mip fallback arrays.
    boost::container::small_vector<u32, 8> image_descriptor_array_sizes;
    // perf_bind_memo: FindImage's own result per binding (before the stencil -> depth redirect);
    // false once something here must not be remembered (a rejected T#, a rebind).
    boost::container::small_vector<VideoCore::ImageId, 16> raw_ids;
    bool rememberable = keyed;

    for (const auto& image_desc : stage.images) {
        const auto* prepared = resource_bindings
            ? &resource_bindings->image_values[&image_desc - stage.images.data()] : nullptr;
        const auto tsharp = prepared ? prepared->sharp : image_desc.GetSharp(stage);
        if (texture_cache.IsMeta(tsharp.Address())) {
            LOG_WARNING(Render_Vulkan, "Unexpected metadata read by a shader (texture)");
        }

        const auto data_fmt = tsharp.GetDataFmt();
        const auto num_fmt = tsharp.GetNumberFmt();
        if (tsharp.Address() == 0 || data_fmt == AmdGpu::DataFormat::FormatInvalid) {
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            raw_ids.emplace_back();
            image_descriptor_array_sizes.push_back(1);
            continue;
        }

        if (!memory->IsValidGpuMapping(tsharp.Address(), 0) || tsharp.pitch < tsharp.width ||
            !magic_enum::enum_contains(data_fmt) || !magic_enum::enum_contains(num_fmt)) {
            LOG_WARNING(Render_Vulkan,
                        "Rejecting invalid T# address={:#x}, pitch={}, width={}, "
                        "data_format={}, num_format={}",
                        tsharp.Address(), tsharp.pitch, tsharp.width, static_cast<u32>(data_fmt),
                        static_cast<u32>(num_fmt));
            image_bindings.emplace_back(std::piecewise_construct, std::tuple{}, std::tuple{});
            raw_ids.emplace_back();
            image_descriptor_array_sizes.push_back(1);
            rememberable = false; // the mapping may become valid
            continue;
        }

        const Shader::MipStorageFallbackMode mip_fallback_mode = image_desc.mip_fallback_mode;
        const u32 num_bindings = image_desc.NumBindings(stage);
        const auto& tsharp_desc = TextureDescFor(tsharp, image_desc, prepared ? &prepared->hash : nullptr);

        for (auto i = 0; i < num_bindings; i++) {
            auto& [image_id, desc] = image_bindings.emplace_back(
                std::piecewise_construct, std::tuple{}, std::tuple{tsharp_desc});

            if (mip_fallback_mode == Shader::MipStorageFallbackMode::ConstantIndex) {
                ASSERT(num_bindings == 1);
                desc.view_info.range.base.level += image_desc.constant_mip_index;
                desc.view_info.range.extent.levels = 1;
            } else if (mip_fallback_mode == Shader::MipStorageFallbackMode::DynamicIndex) {
                desc.view_info.range.base.level += i;
                desc.view_info.range.extent.levels = 1;
            }

            image_id = texture_cache.FindImage(desc);
            raw_ids.emplace_back(image_id);
            auto* image = &texture_cache.GetImage(image_id);
            if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
                // If this image has an associated depth image, it's a stencil attachment.
                // Redirect the access to the actual depth-stencil buffer.
                image_id = depth_image_id;
                image = &texture_cache.GetImage(image_id);
            }
            if (image->binding.is_bound) {
                // The image is already bound. In case if it is about to be used as storage we
                // need to force general layout on it.
                image->binding.force_general |= image_desc.is_written;
            }
            image->binding.is_bound = 1u;
        }

        image_descriptor_array_sizes.push_back(num_bindings);
    }

    // Second pass to re-bind images that were updated after binding
    for (auto& [image_id, desc] : image_bindings) {
        bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        if (!image_id) {
            image_infos.emplace_back(VK_NULL_HANDLE, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
            if (stereo_handles)
                NoteViewHint({}, {});
        } else {
            if (auto& old_image = texture_cache.GetImage(image_id);
                old_image.binding.needs_rebind) {
                old_image.binding = {};
                image_id = texture_cache.FindImage(desc);
                rememberable = false;
            }

            bound_images.emplace_back(image_id);

            auto& image = texture_cache.GetImage(image_id);
            auto& image_view = texture_cache.FindTexture(image_id, desc);

            TransitTexture(image, is_storage, desc.view_info.range);
            image.usage.storage |= is_storage;
            image.usage.texture |= !is_storage;

            image_infos.emplace_back(VK_NULL_HANDLE, *image_view.image_view,
                                     image.backing->state.layout);
            // Bit 25: what NativeStereo::RightView returns (image_view.cpp RegisterView).
            if (stereo_handles)
                NoteViewHint(*image_view.image_view, image_view.native_right
                                                         ? *image_view.native_right
                                                         : *image_view.image_view);
        }
    }

    u32 image_info_idx = first_image_idx;
    u32 image_binding_idx = 0;
    for (u32 array_size : image_descriptor_array_sizes) {
        const auto& [_, desc] = image_bindings[image_binding_idx];
        const bool is_storage = desc.type == VideoCore::TextureCache::BindingType::Storage;
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = array_size;
        set_write.descriptorType =
            is_storage ? vk::DescriptorType::eStorageImage : vk::DescriptorType::eSampledImage;
        set_write.pImageInfo = &image_infos[image_info_idx];

        image_info_idx += array_size;
        image_binding_idx += array_size;
        binding.unified += array_size;
    }

    if (keyed) {
        if (found) {
            // VR_PERF_CHECK=1: the memo's products against the full path's, field by field.
            ++bind_counters.hits;
            ++bind_counters.checked;
            std::string diff;
            const auto note = [&diff](const std::string& what) {
                diff += diff.empty() ? what : "," + what;
            };
            const u32 n_infos = image_infos.size() - first_image_idx;
            if (chk_infos.size() != n_infos) {
                note(fmt::format("infos {} vs {}", chk_infos.size(), n_infos));
            } else {
                for (u32 i = 0; i < n_infos; ++i) {
                    const auto& m = chk_infos[i];
                    const auto& f = image_infos[first_image_idx + i];
                    if (m.sampler != f.sampler || m.imageView != f.imageView ||
                        m.imageLayout != f.imageLayout) {
                        note(fmt::format(
                            "info{} view {:#x}/{:#x} layout {}/{}", i,
                            u64(VkImageView(m.imageView)), u64(VkImageView(f.imageView)),
                            vk::to_string(m.imageLayout), vk::to_string(f.imageLayout)));
                    }
                    if (i < found->num_bindings && image_bindings[i].first &&
                        found->bindings[i].range != image_bindings[i].second.view_info.range) {
                        note(fmt::format("range{}", i));
                    }
                }
            }
            const u32 n_writes = set_write_index - chk_write0;
            if (chk_writes.size() != n_writes) {
                note(fmt::format("writes {} vs {}", chk_writes.size(), n_writes));
            } else {
                for (u32 i = 0; i < n_writes; ++i) {
                    const auto& m = chk_writes[i];
                    const auto& f = set_writes[chk_write0 + i];
                    if (m.dstSet != f.dstSet || m.dstBinding != f.dstBinding ||
                        m.dstArrayElement != f.dstArrayElement ||
                        m.descriptorCount != f.descriptorCount ||
                        m.descriptorType != f.descriptorType || m.pImageInfo != f.pImageInfo ||
                        m.pBufferInfo != f.pBufferInfo ||
                        m.pTexelBufferView != f.pTexelBufferView) {
                        note(fmt::format("write{} binding {}/{} count {}/{}", i, m.dstBinding,
                                         f.dstBinding, m.descriptorCount, f.descriptorCount));
                    }
                }
            }
            const u32 n_bound = bound_images.size() - chk_bound0;
            if (chk_bound.size() != n_bound || !std::equal(chk_bound.begin(), chk_bound.end(),
                                                           bound_images.begin() + chk_bound0)) {
                note("bound images");
            }
            if (chk_unified != binding.unified) {
                note(fmt::format("unified {}/{}", chk_unified, binding.unified));
            }
            if (!diff.empty() && bind_counters.NoteMismatch()) {
                LOG_WARNING(Render_Vulkan,
                            "VR perf check: bind memo mismatch #{}: shader {:#x}, {} images, {} "
                            "bindings: {}",
                            bind_counters.total_mismatches, stage.pgm_hash, key.count,
                            image_bindings.size(), diff);
            }
        } else {
            ++bind_counters.misses;
        }
        // Remember a pure lookup only: no image registered or unregistered while binding (the
        // same registry as the next use needs), nothing rejected, no rebind, within capacity.
        using Entry = BindMemo::Entry;
        const u64 registry_after = texture_cache.RegistryEpoch();
        if (rememberable && registry_after == registry_before &&
            image_bindings.size() <= Entry::MaxBindings &&
            image_descriptor_array_sizes.size() == key.count) {
            Entry& e = bind_memo.Prepare(key, key_hash);
            bool fits = true;
            for (u32 k = 0; k < image_descriptor_array_sizes.size(); ++k) {
                fits = fits && image_descriptor_array_sizes[k] <= 0xFFu;
                e.array_sizes[k] = static_cast<u8>(image_descriptor_array_sizes[k]);
            }
            for (u32 i = 0; i < image_bindings.size(); ++i) {
                const auto& [image_id, desc] = image_bindings[i];
                auto& b = e.bindings[i];
                b.image_id = image_id;
                b.raw_id = raw_ids[i];
                b.view = image_infos[first_image_idx + i].imageView;
                // Always written: a reused entry must not keep another view's right.
                const u32 hint = first_image_idx + i;
                b.right = stereo_handles && hint < view_hints.size() &&
                                  view_hints[hint].left == b.view
                              ? view_hints[hint].right
                              : vk::ImageView{};
                b.range = desc.view_info.range;
                b.backing = image_id
                                ? static_cast<const void*>(texture_cache.GetImage(image_id).backing)
                                : nullptr;
            }
            e.num_bindings = image_bindings.size();
            if (fits) {
                BindMemo::Commit(e, registry_after, PerfMemo::Epoch());
            }
        } else {
            ++bind_uncacheable;
        }
    }

    BindSamplers(stage, binding);
}

void Rasterizer::BindSamplers(const Shader::Info& stage, Shader::Backend::Bindings& binding) {
    for (const auto& sampler : stage.samplers) {
        auto ssharp = resource_bindings
            ? resource_bindings->sampler_values[&sampler - stage.samplers.data()] : sampler.GetSharp(stage);
        if (!resource_bindings && sampler.disable_aniso) {
            const auto& tsharp = stage.images[sampler.associated_image].GetSharp(stage);
            if (tsharp.base_level == 0 && tsharp.last_level == 0) {
                ssharp.max_aniso.Assign(AmdGpu::AnisoRatio::One);
            }
        }
        const auto vk_sampler = texture_cache.GetSampler(
            ssharp, std::as_const(ScratchDraw::Registers(liverpool)).ta_bc_base);
        image_infos.emplace_back(vk_sampler, VK_NULL_HANDLE, vk::ImageLayout::eGeneral);
        auto& set_write = set_writes[set_write_index++];
        set_write.dstSet = VK_NULL_HANDLE;
        set_write.dstBinding = binding.unified++;
        set_write.dstArrayElement = 0;
        set_write.descriptorCount = 1;
        set_write.descriptorType = vk::DescriptorType::eSampler;
        set_write.pImageInfo = &image_infos.back();
    }
}

// VR_BEGIN_STATE_MEMO and the memo hit's VR_PERF_CHECK, read once at start-up (a plain load per
// draw, no function-local guard).
static const bool g_begin_state_memo_env = std::getenv("VR_BEGIN_STATE_MEMO") != nullptr;
static const bool g_begin_memo_check = std::getenv("VR_PERF_CHECK") != nullptr;

RenderState Rasterizer::BeginRendering(const GraphicsPipeline* pipeline) {
    FastRender::BProfile::Scope b_profile{FastRender::BProfile::BeginRendering};
    vr_scene_inverse = {1.f, 1.f};
    if (vr_temporal_scaled) {
        if (auto reduced = VrTemporalRenderState(pipeline)) {
            return *reduced;
        }
    }
    if (!g_begin_state_memo_env && !FastRender::On(FastRender::BeginStateMemo)) {
        return BeginRenderingFull(pipeline);
    }
    if (g_pass_memo)
        return BeginRenderingPassMemo(pipeline);
    if (!begin_memo)
        begin_memo = std::make_unique<BeginMemo>();
    auto& memo = *begin_memo;
    if (((memo.hits + memo.misses) & 65535) == 65535)
        LOG_INFO(Render_Vulkan, "Scratch begin-state memo: {} hits / {} misses / {} checked",
                 memo.hits, memo.misses, memo.checked);
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    const auto& pipeline_key = pipeline->GetGraphicsKey();
    const auto key = BeginMemo::MakeKey(regs, pipeline_key);
    std::array<BeginMemo::Target, AmdGpu::NUM_COLOR_BUFFERS + 1> targets{};
    bool eligible = !regs.depth_render_control.depth_clear_enable &&
                    !regs.depth_render_control.stencil_clear_enable;
    const auto collect = [&](u32 slot, const RenderTargetInfo& target, VAddr meta) {
        const auto& [id, desc] = target;
        if (!id)
            return;
        const auto& image = texture_cache.GetImage(id);
        eligible &= !image.binding.is_bound && !image.binding.needs_rebind &&
                    False(image.flags & VideoCore::ImageFlagBits::Dirty) &&
                    image.backing->subresource_states.empty() &&
                    !texture_cache.IsMetaCleared(meta, desc.view_info.range.base.layer);
        targets[slot] = {id, desc.view_info, image.GetImage(), image.backing->state.layout};
    };
    for (u32 cb = 0; cb < std::bit_width(pipeline_key.mrt_mask); ++cb)
        collect(cb, cb_descs[cb], regs.color_buffers[cb].CmaskAddress());
    collect(AmdGpu::NUM_COLOR_BUFFERS, db_desc, regs.depth_htile_data_base.GetAddress());
    const u64 epoch = texture_cache.RegistryEpoch();
    // The open pass satisfies the remembered state: equal, or (bit 43) its own attachments with
    // their clears done, or a superset of them. Either way nothing ended it since, so no target
    // changed layout (a transition ends the pass), as with plain equality. FAST_GPU_PASS_MERGE:
    // or it is the shadow pair a swapped UI draw began right after that pass (VrUiMemoResumes).
    bool resumed = false;
    if (eligible && memo.valid && epoch == memo.epoch && memo.tick == scheduler.CurrentTick() &&
        (scheduler.Continues(memo.state) || (resumed = VrUiMemoResumes())) &&
        targets == memo.targets && key.bytes == memo.key.bytes) {
        ++memo.hits;
        if (g_begin_memo_check) {
            const auto reference = BeginRenderingFull(pipeline);
            ASSERT_MSG(reference == memo.state && !attachment_feedback_loop,
                       "Scratch begin-state memo differs from full render state");
            ++memo.checked;
        }
        if (resumed) {
            if (auto reference = VrUiMemoVerify(pipeline, memo.state)) {
                memo.valid = false; // FAST_GPU_PASS_MERGE_VERIFY disagreed (logged)
                return *reference;
            }
        }
        attachment_feedback_loop = false;
        for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb)
            if (targets[cb].id)
                texture_cache.GetImage(targets[cb].id).usage.render_target = 1;
        if (targets.back().id)
            texture_cache.GetImage(targets.back().id).usage.depth_target = true;
        return memo.state;
    }
    ++memo.misses;
    memo.valid = false;
    auto state = BeginRenderingFull(pipeline);
    bool clears =
        state.depth_stencil_attachment.depth_clear || state.depth_stencil_attachment.stencil_clear;
    for (u32 cb = 0; cb < state.num_color_attachments; ++cb)
        clears |= state.color_attachments[cb].is_clear != 0;
    // Store the pre-call inputs only if the full path did not change their identity/layout.
    // A layout transition gets one extra miss, then becomes reusable on the next draw.
    if (eligible && !clears && !attachment_feedback_loop &&
        epoch == texture_cache.RegistryEpoch()) {
        memo.key = key;
        memo.targets = targets;
        memo.epoch = epoch;
        memo.tick = scheduler.CurrentTick();
        memo.state = state;
        memo.valid = true;
    }
    return state;
}

bool Rasterizer::BeginMemoHolds(const RenderState& state) const {
    if (g_pass_memo) // FAST_CPU_PASS_MEMO: BeginRendering's memo is PassMemo
        return pass_memo && pass_memo->valid && pass_memo->state == state;
    return begin_memo && begin_memo->valid && begin_memo->state == state;
}

// FAST_CPU_PASS_MEMO: BeginRendering through PassMemo (see there).
RenderState Rasterizer::BeginRenderingPassMemo(const GraphicsPipeline* pipeline) {
    using Memo = PassMemo;
    if (!pass_memo)
        pass_memo = std::make_unique<PassMemo>();
    auto& memo = *pass_memo;
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    const auto& key = pipeline->GetGraphicsKey();
    const u32 num_color = std::bit_width(key.mrt_mask);
    const u64 meta_epoch = texture_cache.MetaClearEpoch();
    // While MetaClearEpoch is the memo's, its slots' meta surfaces have no cleared slice; after
    // it moved they are looked up as BeginMemo did (with the verify, always: the epoch's answer
    // is checked against them).
    const bool look_up = meta_epoch != memo.meta_epoch || g_pass_memo_verify;
    std::array<VideoCore::Image*, Memo::DepthSlot + 1> images{}; // a hit's, for their usage
    const auto same_image = [&](u32 slot, VideoCore::ImageId id, const Memo::Slot& s) {
        auto& image = texture_cache.GetImage(id);
        images[slot] = &image;
        return Memo::Plain(image) && image.GetImage() == s.backing &&
               image.backing->state.layout == s.layout;
    };
    const auto cleared = [&](VAddr meta, const Memo::Slot& s) {
        return look_up && texture_cache.IsMetaCleared(meta, s.view.range.base.layer);
    };
    bool resumed = false; // FAST_GPU_PASS_MERGE's rule 3 (VrUiMemoResumes) gave the hit
    const Memo::Miss miss = [&] {
        if (!memo.valid)
            return Memo::Invalid;
        if (memo.tick != scheduler.CurrentTick())
            return Memo::Tick;
        if (memo.epoch != texture_cache.RegistryEpoch())
            return Memo::Epoch;
        if (regs.depth_render_control.depth_clear_enable ||
            regs.depth_render_control.stencil_clear_enable)
            return Memo::Clears;
        if (key.mrt_mask != memo.mrt_mask || key.probe_view_mask != memo.probe_view_mask ||
            key.color_samples != memo.color_samples)
            return Memo::Key;
        for (u32 cb = 0; cb < num_color; ++cb) {
            const auto& [id, desc] = cb_descs[cb];
            const auto& s = memo.slots[cb];
            if (id != s.id)
                return Memo::Target;
            if (!id)
                continue;
            const VAddr meta = regs.color_buffers[cb].CmaskAddress();
            if (meta != s.meta || desc.info.meta_info.cmask_addr != s.desc[0] ||
                desc.info.meta_info.fmask_addr != s.desc[1] || desc.view_info != s.view ||
                !same_image(cb, id, s))
                return Memo::Target;
            if (cleared(meta, s))
                return Memo::Meta;
        }
        const auto& [id, desc] = db_desc;
        const auto& s = memo.slots[Memo::DepthSlot];
        if (id != s.id)
            return Memo::Target;
        if (id) {
            if (Memo::DepthControlBits(regs) != memo.depth_control ||
                Memo::DepthFlags(regs) != memo.depth_flags)
                return Memo::Depth;
            const VAddr meta = regs.depth_htile_data_base.GetAddress();
            if (meta != s.meta || desc.info.meta_info.htile_addr != s.desc[0] ||
                desc.info.stencil_addr != s.desc[1] || desc.info.stencil_size != s.stencil_size ||
                desc.view_info != s.view || !same_image(Memo::DepthSlot, id, s))
                return Memo::Target;
            if (cleared(meta, s))
                return Memo::Meta;
        }
        // As BeginMemo: the open pass continues the stored state, so nothing ended it since and
        // no target changed layout (a transition ends the pass). FAST_GPU_PASS_MERGE: or it is
        // the shadow pair a swapped UI draw began right after that pass (VrUiMemoResumes).
        if (!scheduler.Continues(memo.state) && !(resumed = VrUiMemoResumes()))
            return Memo::Pass;
        return Memo::Hit;
    }();
    if constexpr (Common::Log::Enabled) {
        ++memo.probes[miss];
        if ((++memo.calls & 0x3FFFF) == 0)
            memo.Report();
    }
    if (g_pass_memo_verify && miss == Memo::Meta && meta_epoch == memo.meta_epoch &&
        memo.Mismatch()) {
        LOG_ERROR(Render_Vulkan,
                  "FAST_CPU_PASS_MEMO verify: a target's meta surface has a cleared slice at an "
                  "unchanged MetaClearEpoch {}; missed, mismatch #{}",
                  meta_epoch, memo.mismatches);
    }
    if (miss == Memo::Hit) {
        if (meta_epoch != memo.meta_epoch) {
            // Every slot was looked up clear of a clear: so it stays while the epoch is this one.
            memo.meta_epoch = meta_epoch;
            if constexpr (Common::Log::Enabled)
                ++memo.looked_up;
        }
        if (g_pass_memo_verify || g_begin_memo_check) [[unlikely]] {
            auto reference = PassMemoVerifyHit(pipeline);
            if (resumed)
                VrUiMemoVerify(pipeline, memo.state); // counted (and compared): the full state
            return reference;                         // is used here either way
        }
        if (resumed) {
            if (auto reference = VrUiMemoVerify(pipeline, memo.state)) {
                memo.valid = false; // FAST_GPU_PASS_MERGE_VERIFY disagreed (logged)
                return *reference;
            }
        }
        attachment_feedback_loop = false;
        for (u32 cb = 0; cb < num_color; ++cb)
            if (images[cb])
                images[cb]->usage.render_target = 1;
        if (images[Memo::DepthSlot])
            images[Memo::DepthSlot]->usage.depth_target = true;
        return memo.state;
    }
    memo.valid = false;
    // The pre-call inputs (BeginMemo's targets and eligibility), kept if the full path cleared
    // nothing, changed no image registration and gained no cleared meta slice.
    bool eligible = !regs.depth_render_control.depth_clear_enable &&
                    !regs.depth_render_control.stencil_clear_enable;
    const auto snapshot = [&](VideoCore::ImageId id, const VideoCore::ImageViewInfo& view,
                              VAddr meta, Memo::Slot& s) {
        const auto& image = texture_cache.GetImage(id);
        eligible &= Memo::Plain(image);
        s.view = view;
        s.backing = image.GetImage();
        s.layout = image.backing->state.layout;
        s.meta = meta;
    };
    memo.mrt_mask = key.mrt_mask;
    memo.probe_view_mask = key.probe_view_mask;
    memo.color_samples = key.color_samples;
    for (u32 cb = 0; cb < num_color; ++cb) {
        const auto& [id, desc] = cb_descs[cb];
        auto& s = memo.slots[cb];
        s.id = id;
        if (!id)
            continue;
        snapshot(id, desc.view_info, regs.color_buffers[cb].CmaskAddress(), s);
        s.desc = {desc.info.meta_info.cmask_addr, desc.info.meta_info.fmask_addr};
        s.stencil_size = 0;
    }
    {
        const auto& [id, desc] = db_desc;
        auto& s = memo.slots[Memo::DepthSlot];
        s.id = id;
        if (id) {
            snapshot(id, desc.view_info, regs.depth_htile_data_base.GetAddress(), s);
            s.desc = {desc.info.meta_info.htile_addr, desc.info.stencil_addr};
            s.stencil_size = desc.info.stencil_size;
            memo.depth_control = Memo::DepthControlBits(regs);
            memo.depth_flags = Memo::DepthFlags(regs);
        }
    }
    const u64 epoch = texture_cache.RegistryEpoch();
    const u64 meta_before = texture_cache.MetaClearEpoch();
    auto state = BeginRenderingFull(pipeline);
    bool clears =
        state.depth_stencil_attachment.depth_clear || state.depth_stencil_attachment.stencil_clear;
    for (u32 cb = 0; cb < state.num_color_attachments; ++cb)
        clears |= state.color_attachments[cb].is_clear != 0;
    // With no clear, the full path found each slot's meta slice clear of one (IsMetaCleared) and
    // TouchMeta kept it so; with MetaClearEpoch unchanged nothing set a slice since.
    if (eligible && !clears && !attachment_feedback_loop &&
        epoch == texture_cache.RegistryEpoch() && meta_before == texture_cache.MetaClearEpoch()) {
        memo.epoch = epoch;
        memo.tick = scheduler.CurrentTick();
        memo.meta_epoch = meta_before;
        memo.state = state;
        memo.valid = true;
    }
    return state;
}

// FAST_CPU_PASS_MEMO_VERIFY (and VR_PERF_CHECK, as for BeginMemo) on a pass-memo hit: the full
// path's state is taken; it must equal the memo's and leave the registry, the meta surfaces and
// each attachment's flags, backing, layouts and meta addresses as they were (what the hit leaves
// out changes nothing).
RenderState Rasterizer::PassMemoVerifyHit(const GraphicsPipeline* pipeline) {
    auto& memo = *pass_memo;
    struct Seen {
        VideoCore::ImageFlagBits flags{};
        const void* backing{};
        vk::ImageLayout layout{};
        size_t subresources{};
        VAddr cmask{}, fmask{}, htile{};
        bool operator==(const Seen&) const = default;
    };
    std::array<VideoCore::ImageId, PassMemo::DepthSlot + 1> ids{};
    for (u32 cb = 0; cb < std::bit_width(memo.mrt_mask); ++cb)
        ids[cb] = cb_descs[cb].first;
    ids[PassMemo::DepthSlot] = db_desc.first;
    const auto seen = [&] {
        std::array<Seen, PassMemo::DepthSlot + 1> result{};
        for (u32 slot = 0; slot < ids.size(); ++slot) {
            if (!ids[slot])
                continue;
            const auto& image = texture_cache.PeekImage(ids[slot]);
            const auto& meta = image.info.meta_info;
            result[slot] = {image.flags,
                            image.backing,
                            image.backing->state.layout,
                            image.backing->subresource_states.size(),
                            meta.cmask_addr,
                            meta.fmask_addr,
                            meta.htile_addr};
        }
        return result;
    };
    const auto before = seen();
    const u64 epoch = texture_cache.RegistryEpoch();
    const u64 meta_epoch = texture_cache.MetaClearEpoch();
    const auto reference = BeginRenderingFull(pipeline);
    const bool same_state = reference == memo.state && !attachment_feedback_loop;
    ASSERT_MSG(!g_begin_memo_check || same_state,
               "Scratch begin-state memo differs from full render state");
    const bool same = same_state && epoch == texture_cache.RegistryEpoch() &&
                      meta_epoch == texture_cache.MetaClearEpoch() && before == seen();
    ++memo.verified;
    if (!same) {
        memo.valid = false;
        if (memo.Mismatch()) {
            LOG_ERROR(Render_Vulkan,
                      "FAST_CPU_PASS_MEMO verify: a hit differs from the full path (state {}, "
                      "registry epoch {}, meta epoch {}, targets {}); the full state is used, "
                      "mismatch #{}",
                      same_state ? "same" : "differs",
                      epoch == texture_cache.RegistryEpoch() ? "same" : "moved",
                      meta_epoch == texture_cache.MetaClearEpoch() ? "same" : "moved",
                      before == seen() ? "same" : "changed", memo.mismatches);
        }
    }
    return reference;
}

RenderState Rasterizer::BeginRenderingFull(const GraphicsPipeline* pipeline) {
    attachment_feedback_loop = false;
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    const auto& key = pipeline->GetGraphicsKey();
    RenderState state{};
    state.width = instance.GetMaxFramebufferWidth();
    state.height = instance.GetMaxFramebufferHeight();
    state.num_layers = std::numeric_limits<u16>::max();
    state.num_color_attachments = std::bit_width(key.mrt_mask);
    for (auto cb = 0u; cb < state.num_color_attachments; ++cb) {
        auto& [image_id, desc] = cb_descs[cb];
        if (!image_id) {
            state.color_attachments[cb] = {};
            continue;
        }
        auto* image = &texture_cache.GetImage(image_id);
        if (image->binding.needs_rebind) {
            image_id = bound_images.emplace_back(texture_cache.FindImage(desc));
            image = &texture_cache.GetImage(image_id);
        }
        texture_cache.UpdateImage(image_id);
        image->SetBackingSamples(key.color_samples[cb]);
        const auto& image_view = texture_cache.FindRenderTarget(image_id, desc);
        const auto slice = image_view.info.range.base.layer;
        const auto mip = image_view.info.range.base.level;

        const auto& col_buf = regs.color_buffers[cb];
        const bool is_clear = texture_cache.IsMetaCleared(col_buf.CmaskAddress(), slice);
        texture_cache.TouchMeta(col_buf.CmaskAddress(), slice, false);

        if (image->binding.is_bound) {
            ASSERT_MSG(!image->binding.force_general,
                       "Having image both as storage and render target is unsupported");
            image->Transit(instance.IsAttachmentFeedbackLoopLayoutSupported()
                               ? vk::ImageLayout::eAttachmentFeedbackLoopOptimalEXT
                               : vk::ImageLayout::eGeneral,
                           vk::AccessFlagBits2::eColorAttachmentWrite, {});
            attachment_feedback_loop = true;
        } else {
            image->Transit(vk::ImageLayout::eColorAttachmentOptimal,
                           vk::AccessFlagBits2::eColorAttachmentWrite |
                               vk::AccessFlagBits2::eColorAttachmentRead,
                           desc.view_info.range);
        }

        state.width = std::min<u32>(state.width, std::max(image->info.size.width >> mip, 1u));
        state.height = std::min<u32>(state.height, std::max(image->info.size.height >> mip, 1u));
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        const auto clear_value =
            is_clear ? LiverpoolToVK::ColorBufferClearValue(col_buf) : vk::ClearValue{};
        auto& attachment = state.color_attachments[cb];
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image->backing->state.layout;
        attachment.clear_value = clear_value.color.uint32;
        attachment.is_clear = is_clear;

        image->usage.render_target = 1u;
    }
    for (u32 cb = state.num_color_attachments; cb < state.color_attachments.size(); ++cb) {
        state.color_attachments[cb] = {};
    }

    if (auto image_id = db_desc.first; image_id) {
        auto& desc = db_desc.second;
        const auto htile_address = regs.depth_htile_data_base.GetAddress();
        const auto& target_view = texture_cache.FindDepthTarget(image_id, desc);
        auto& image = texture_cache.GetImage(image_id);
        // vr.ini perf_depth_one_layout (vr_perf.h, vr_perf_logic.h KeepDepthWritable): a draw
        // with depth writes off that does not sample this depth (BindTextures sets is_bound for
        // this draw's textures, the stencil alias redirected to the depth image), while the depth
        // is in the writable layout, keeps the depth-write view and that layout; its writes stay
        // off (the dynamic depth-write enable and stencil write masks). A depth-write toggle then
        // no longer changes the view and the layout, which ended the render pass and transitioned
        // the image. `desc` itself is never changed: VrTrackTargets and the rt memo read its
        // is_storage as "this draw writes depth". The view is the one a depth-writing draw gets
        // (the same description with is_storage set).
        const bool has_stencil_now = image.info.props.has_stencil;
        const bool writable_now =
            image.backing->subresource_states.empty() &&
            image.backing->state.layout == (has_stencil_now
                                                ? vk::ImageLayout::eDepthStencilAttachmentOptimal
                                                : vk::ImageLayout::eDepthAttachmentOptimal);
        const bool keep_writable = VideoCore::PerfLogic::KeepDepthWritable(
            desc.view_info.is_storage, Perf::DepthOneLayout(), image.binding.is_bound,
            writable_now);
        const auto& image_view = [&]() -> const VideoCore::ImageView& {
            if (!keep_writable) {
                return target_view;
            }
            VideoCore::ImageViewInfo writable = desc.view_info;
            writable.is_storage = true;
            return image.FindView(writable, false);
        }();

        const auto slice = image_view.info.range.base.layer;
        const bool is_depth_clear =
            (regs.depth_render_control.depth_clear_enable && regs.depth_control.depth_enable &&
             regs.depth_control.depth_write_enable) ||
            texture_cache.IsMetaCleared(htile_address, slice);
        const bool is_stencil_clear = regs.depth_render_control.stencil_clear_enable;
        texture_cache.TouchMeta(htile_address, slice, false);
        ASSERT(desc.view_info.range.extent.levels == 1 && !image.binding.needs_rebind);

        const bool has_stencil = image.info.props.has_stencil;
        if (keep_writable) {
            Perf::Count().depth_kept.fetch_add(1, std::memory_order_relaxed);
        }
        // Stencil writes can be enabled while depth writes are off (vr_perf_logic.h).
        using DL = VideoCore::PerfLogic::DepthLayout;
        const auto new_layout = [&] {
            switch (VideoCore::PerfLogic::DepthAttachmentLayout(
                desc.view_info.is_storage || keep_writable, has_stencil,
                regs.depth_control.stencil_enable)) {
            case DL::DepthStencilAttachment:
                return vk::ImageLayout::eDepthStencilAttachmentOptimal;
            case DL::DepthAttachment:
                return vk::ImageLayout::eDepthAttachmentOptimal;
            case DL::DepthReadOnlyStencilAttachment:
                return vk::ImageLayout::eDepthReadOnlyStencilAttachmentOptimal;
            case DL::DepthStencilReadOnly:
                return vk::ImageLayout::eDepthStencilReadOnlyOptimal;
            case DL::DepthReadOnly:
            default:
                return vk::ImageLayout::eDepthReadOnlyOptimal;
            }
        }();
        image.Transit(new_layout,
                      vk::AccessFlagBits2::eDepthStencilAttachmentWrite |
                          vk::AccessFlagBits2::eDepthStencilAttachmentRead,
                      desc.view_info.range);

        state.width = std::min<u32>(state.width, image.info.size.width);
        state.height = std::min<u32>(state.height, image.info.size.height);
        state.num_layers = std::min<u32>(state.num_layers, image_view.info.range.extent.layers);

        auto& attachment = state.depth_stencil_attachment;
        attachment.image_view = *image_view.image_view;
        attachment.image_layout = image.backing->state.layout;
        attachment.clear_value = {};

        if (regs.depth_buffer.DepthValid()) {
            attachment.clear_value[0] = is_depth_clear ? std::bit_cast<u32>(regs.depth_clear) : 0u;
            attachment.has_depth = true;
            attachment.depth_clear = is_depth_clear;
        }
        if (regs.depth_buffer.StencilValid()) {
            attachment.clear_value[1] = is_stencil_clear ? regs.stencil_clear : 0u;
            attachment.has_stencil = true;
            attachment.stencil_clear = is_stencil_clear;
        }

        image.usage.depth_target = true;
    } else {
        state.depth_stencil_attachment = {};
    }

    if (state.num_layers == std::numeric_limits<u16>::max()) {
        state.num_layers = 1;
    }

    state.probe_view_mask = pipeline->GetGraphicsKey().probe_view_mask;
    NativeStereo::MultiviewState(state);
    return state;
}

void Rasterizer::Resolve() {
    const auto& mrt0_hint = std::as_const(ScratchDraw::ColorExtents(liverpool))[0];
    const auto& mrt1_hint = std::as_const(ScratchDraw::ColorExtents(liverpool))[1];
    VideoCore::TextureCache::ImageDesc mrt0_desc{
        std::as_const(ScratchDraw::Registers(liverpool)).color_buffers[0], mrt0_hint};
    VideoCore::TextureCache::ImageDesc mrt1_desc{
        std::as_const(ScratchDraw::Registers(liverpool)).color_buffers[1], mrt1_hint};
    auto& mrt0_image = texture_cache.GetImage(texture_cache.FindImage(mrt0_desc, true));
    auto& mrt1_image = texture_cache.GetImage(texture_cache.FindImage(mrt1_desc, true));

    ScopeMarkerBegin(
        fmt::format("Resolve:MRT0={:#x}:MRT1={:#x}",
                    std::as_const(ScratchDraw::Registers(liverpool)).color_buffers[0].Address(),
                    std::as_const(ScratchDraw::Registers(liverpool)).color_buffers[1].Address()));
    mrt1_image.Resolve(mrt0_image, mrt0_desc.view_info.range, mrt1_desc.view_info.range);
    ScopeMarkerEnd();
}

void Rasterizer::DepthStencilCopy(bool is_depth, bool is_stencil) {
    auto& regs = std::as_const(ScratchDraw::Registers(liverpool));

    auto read_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), std::as_const(ScratchDraw::DepthExtent(liverpool)),
        false);
    auto write_desc = VideoCore::TextureCache::ImageDesc(
        regs.depth_buffer, regs.depth_view, regs.depth_control,
        regs.depth_htile_data_base.GetAddress(), std::as_const(ScratchDraw::DepthExtent(liverpool)),
        true);

    auto& read_image = texture_cache.GetImage(texture_cache.FindImage(read_desc));
    auto& write_image = texture_cache.GetImage(texture_cache.FindImage(write_desc));

    VideoCore::SubresourceRange sub_range;
    sub_range.base.layer = std::as_const(ScratchDraw::Registers(liverpool)).depth_view.slice_start;
    sub_range.extent.layers =
        std::as_const(ScratchDraw::Registers(liverpool)).depth_view.NumSlices() -
        sub_range.base.layer;

    ScopeMarkerBegin(fmt::format(
        "DepthStencilCopy:DR={:#x}:SR={:#x}:DW={:#x}:SW={:#x}", regs.depth_buffer.DepthAddress(),
        regs.depth_buffer.StencilAddress(), regs.depth_buffer.DepthWriteAddress(),
        regs.depth_buffer.StencilWriteAddress()));

    read_image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead,
                       sub_range);
    write_image.Transit(vk::ImageLayout::eTransferDstOptimal, vk::AccessFlagBits2::eTransferWrite,
                        sub_range);

    auto aspect_mask = vk::ImageAspectFlags(0);
    if (is_depth) {
        aspect_mask |= vk::ImageAspectFlagBits::eDepth;
    }
    if (is_stencil) {
        aspect_mask |= vk::ImageAspectFlagBits::eStencil;
    }

    vk::ImageCopy region = {
        .srcSubresource =
            {
                .aspectMask = aspect_mask,
                .mipLevel = 0,
                .baseArrayLayer = sub_range.base.layer,
                .layerCount = sub_range.extent.layers,
            },
        .srcOffset = {0, 0, 0},
        .dstSubresource =
            {
                .aspectMask = aspect_mask,
                .mipLevel = 0,
                .baseArrayLayer = sub_range.base.layer,
                .layerCount = sub_range.extent.layers,
            },
        .dstOffset = {0, 0, 0},
        .extent = {write_image.info.size.width, write_image.info.size.height, 1},
    };
    scheduler.Commands().copyImage(read_image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                                   write_image.GetImage(), vk::ImageLayout::eTransferDstOptimal,
                                   region);

    ScopeMarkerEnd();
}

void Rasterizer::FillBuffer(VAddr address, u32 num_bytes, u32 value, bool is_gds) {
    DrainDrawPipe(DrawPipe::RasterizerEntry);
    buffer_cache.FillBuffer(address, num_bytes, value, is_gds);
}

void Rasterizer::CopyBuffer(VAddr dst, VAddr src, u32 num_bytes, bool dst_gds, bool src_gds) {
    DrainDrawPipe(DrawPipe::RasterizerEntry);
    buffer_cache.CopyBuffer(dst, src, num_bytes, dst_gds, src_gds);
}

u32 Rasterizer::ReadDataFromGds(u32 gds_offset) {
    DrainDrawPipe(DrawPipe::RasterizerEntry);
    auto* gds_buf = buffer_cache.GetGdsBuffer();
    u32 value;
    std::memcpy(&value, gds_buf->mapped_data.data() + gds_offset, sizeof(u32));
    return value;
}

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size) {
    if (DA::Enabled() && !liverpool->IsRecordingThread())
        DA::Get().Write({{addr, size}, DA::Kind::CpuFault});
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.InvalidateMemory(addr, size);
    return true;
}

bool Rasterizer::InvalidateWriteFaultPage(VAddr addr) {
    if (DA::Enabled() && !liverpool->IsRecordingThread())
        DA::Get().Write({{addr, 8}, DA::Kind::CpuFault});
    if (!IsMapped(addr, 8)) {
        return false;
    }
    buffer_cache.InvalidateWriteFault(addr, 8);
    texture_cache.InvalidateMemory(addr, 8);
    return true;
}

namespace {
// bbport measured ~7x fewer write faults with a 64 KiB window (docs/parallel_gpu.md, "Guest
// write faults"): the game fills its per-frame buffers sequentially, page after page.
// Only buffer pages: texture pages stay watched and fault as before.
u64 FaultWindowBytes() {
    static const u64 window = [] {
        u64 kib = 64;
        if (const char* env = std::getenv("FAST_FAULT_WINDOW_KB")) {
            kib = std::strtoull(env, nullptr, 10);
        }
        return std::bit_ceil(std::clamp<u64>(kib, 4, 4096)) * 1024;
    }();
    return window;
}
} // namespace

void Rasterizer::UnprotectFaultWindow(VAddr addr) {
    const u64 window = FaultWindowBytes();
    const VAddr start = Common::AlignDown(addr, window);
    if (IsMapped(start, window)) {
        buffer_cache.UnprotectCleanWindow(start, window);
    }
}

bool Rasterizer::InvalidateWriteFault(VAddr addr) {
    if (DA::Enabled() && !liverpool->IsRecordingThread())
        DA::Get().Write({{addr, 8}, DA::Kind::CpuFault});
    const u64 window = FaultWindowBytes();
    const VAddr start = Common::AlignDown(addr, window);
    // One mapping check when the window holds the fault's 8 bytes (the handler, InvalidateMemory
    // and UnprotectFaultWindow each made one).
    const bool window_mapped = IsMapped(start, window);
    if (!(window_mapped && addr + 8 <= start + window) && !IsMapped(addr, 8)) {
        return false; // not GPU mapped memory, as InvalidateMemory
    }
    // With readbacks off the window goes first: it lifts the faulting page with its neighbours,
    // so the page's own invalidation below finds it CPU modified and changes no protection. With
    // readbacks on a GPU-modified page must flush first, so the window follows, as before.
    const bool first =
        window_mapped && EmulatorSettings.GetReadbacksMode() == GpuReadbacksMode::Disabled;
    if (VideoCore::PageManager::ExactFaultWindowVerify()) [[unlikely]] {
        VerifyWriteFault(addr, start, window, window_mapped, first);
        return true;
    }
    if (first) {
        buffer_cache.MarkFaultWindow(start, window);
    }
    buffer_cache.InvalidateWriteFault(addr, 8);
    texture_cache.InvalidateMemory(addr, 8);
    if (window_mapped && !first) {
        buffer_cache.MarkFaultWindow(start, window);
    }
    return true;
}

void Rasterizer::VerifyWriteFault(VAddr addr, VAddr start, u64 window, bool window_mapped,
                                  bool first) {
    // FAST_CPU_FAULT_WINDOW_VERIFY: InvalidateWriteFault's work, with the window's pages also
    // worked out the previous way (MarkCleanPagesCpuModified's) under the region lock and
    // compared. A difference is a mismatch, except the GPU-modified pages KEEP_GPU keeps by
    // design. Also counts how often the faulting page's own invalidation still changed its
    // protection after the window had lifted it (the saving that did not happen: an upload on
    // another thread re-protected it in between), and who takes the faults (an empty window: an
    // image page, no tracked buffer there, or a page another thread's fault had just lifted).
    struct Counters {
        std::atomic<u64> faults, stage_a, stage_b, recorder, empty, marked, gpu_marked, gpu_kept,
            previous, keep_gpu, keep_gpu_windows, mismatches, lifted, kept, second_changes;
    };
    static Counters counters{};
    VideoCore::FaultWindowCounts counts{.fault = addr};
    if (first) {
        buffer_cache.MarkFaultWindow(start, window, &counts);
    }
    const u64 updates = VideoCore::PageManager::WatcherUpdates();
    buffer_cache.InvalidateWriteFault(addr, 8);
    const u64 own_changes = VideoCore::PageManager::WatcherUpdates() - updates; // this thread's
    texture_cache.InvalidateMemory(addr, 8);
    if (window_mapped && !first) {
        buffer_cache.MarkFaultWindow(start, window, &counts);
    }
    const auto relaxed = std::memory_order_relaxed;
    const u64 n = counters.faults.fetch_add(1, relaxed) + 1;
    if (FastRender::IsStageB()) {
        counters.stage_b.fetch_add(1, relaxed);
    } else if (FastRender::IsRecorderThread()) {
        counters.recorder.fetch_add(1, relaxed); // e.g. a guest signal's memcpy fallback
    } else if (liverpool->IsRecordingThread()) {
        counters.stage_a.fetch_add(1, relaxed);
    }
    if (counts.marked == 0 && counts.gpu_kept == 0) {
        counters.empty.fetch_add(1, relaxed);
    }
    counters.marked.fetch_add(counts.marked, relaxed);
    counters.gpu_marked.fetch_add(counts.gpu_marked, relaxed);
    counters.gpu_kept.fetch_add(counts.gpu_kept, relaxed);
    counters.previous.fetch_add(counts.previous, relaxed);
    counters.keep_gpu.fetch_add(counts.keep_gpu, relaxed);
    if (first) {
        counters.lifted.fetch_add(counts.fault_lifted, relaxed);
        counters.kept.fetch_add(counts.fault_kept, relaxed);
        if (counts.fault_lifted != 0 && own_changes != 0) {
            counters.second_changes.fetch_add(1, relaxed);
        }
    }
    if (counts.mismatched != 0) {
        const u64 m = counters.mismatches.fetch_add(1, relaxed) + 1;
        if (m <= 32) {
            LOG_ERROR(Render_Vulkan,
                      "FAST_CPU_FAULT_WINDOW verify: window {:#x}+{:#x} (write fault at {:#x}) "
                      "marked {} page(s), the previous window {} (KEEP_GPU kept {}): {} page(s) "
                      "differ, mismatch #{}",
                      start, window, addr, counts.marked, counts.previous, counts.keep_gpu,
                      counts.mismatched, m);
        }
    } else if (counts.keep_gpu != 0 && counters.keep_gpu_windows.fetch_add(1, relaxed) < 4) {
        LOG_WARNING(Render_Vulkan,
                    "FAST_CPU_FAULT_WINDOW verify: KEEP_GPU kept {} GPU-modified page(s) the "
                    "previous window marked CPU modified (window {:#x}+{:#x}); their GPU data now "
                    "stays instead of the guest's bytes (by design, opt-in)",
                    counts.keep_gpu, start, window);
    }
    if ((n & 4095) == 0) {
        const u64 stage_a = counters.stage_a.load(relaxed);
        const u64 stage_b = counters.stage_b.load(relaxed);
        const u64 recorder = counters.recorder.load(relaxed);
        LOG_INFO(Render_Vulkan,
                 "FAST_CPU_FAULT_WINDOW verify: faults={} (stage_a={} stage_b={} recorder={} "
                 "others={}) empty_windows={} pages_marked={} (gpu_pages_marked={}) "
                 "gpu_pages_kept={} previous_pages={} keep_gpu_pages={} mismatches={}; faulting "
                 "page lifted by the window={} kept={}, its invalidation still changed "
                 "protection={}",
                 n, stage_a, stage_b, recorder, n - std::min(n, stage_a + stage_b + recorder),
                 counters.empty.load(relaxed), counters.marked.load(relaxed),
                 counters.gpu_marked.load(relaxed), counters.gpu_kept.load(relaxed),
                 counters.previous.load(relaxed), counters.keep_gpu.load(relaxed),
                 counters.mismatches.load(relaxed), counters.lifted.load(relaxed),
                 counters.kept.load(relaxed), counters.second_changes.load(relaxed));
    }
}

bool Rasterizer::ReadMemory(VAddr addr, u64 size) {
    DrainDrawPipe(DrawPipe::RasterizerEntry);
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    buffer_cache.ReadMemory(addr, size);
    return true;
}

void Rasterizer::ProcessDownloadImages() {
    DrainDrawPipe(DrawPipe::RasterizerEntry);
    texture_cache.ProcessDownloadImages();
}

bool Rasterizer::IsMapped(VAddr addr, u64 size) {
    if (size == 0) {
        // There is no memory, so not mapped.
        return false;
    }
    if (static_cast<u64>(addr) > std::numeric_limits<u64>::max() - size) {
        // Memory range wrapped the address space, cannot be mapped.
        return false;
    }
    const auto range = decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);

    Common::RecursiveSharedLock lock{mapped_ranges_mutex};
    return boost::icl::contains(mapped_ranges, range);
}

void Rasterizer::MapMemory(VAddr addr, u64 size) {
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges += decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
    page_manager.OnGpuMap(addr, size);
}

void Rasterizer::UnmapMemory(VAddr addr, u64 size) {
    DrainDrawPipe(DrawPipe::RasterizerEntry);
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.UnmapMemory(addr, size);
    page_manager.OnGpuUnmap(addr, size);
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed) const {
    ScratchProfile::Cpu scratch_dynamic(29, "Rasterizer::UpdateDynamicState");
    if (FastRender::On(DynStateSkip)) {
        dyn_skip->Update(*this, pipeline, is_indexed);
    } else {
        UpdateViewportScissorState(pipeline);
        UpdateDepthStencilState();
        UpdatePrimitiveState(is_indexed);
    }
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.Commit(instance, scheduler.Commands());
}

void Rasterizer::UpdateViewportScissorState(const GraphicsPipeline* pipeline) const {
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));

    const auto combined_scissor_value_tl = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::max({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const auto combined_scissor_value_br = [](s16 scr, s16 win, s16 gen, s16 win_offset) {
        return std::min({scr, s16(win + win_offset), s16(gen + win_offset)});
    };
    const bool enable_offset = !regs.window_scissor.window_offset_disable;

    AmdGpu::Scissor scsr{};
    scsr.top_left_x = combined_scissor_value_tl(
        regs.screen_scissor.top_left_x, s16(regs.window_scissor.top_left_x),
        s16(regs.generic_scissor.top_left_x),
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.top_left_y = combined_scissor_value_tl(
        regs.screen_scissor.top_left_y, s16(regs.window_scissor.top_left_y),
        s16(regs.generic_scissor.top_left_y),
        enable_offset ? regs.window_offset.window_y_offset : 0);
    scsr.bottom_right_x = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_x, regs.window_scissor.bottom_right_x,
        regs.generic_scissor.bottom_right_x,
        enable_offset ? regs.window_offset.window_x_offset : 0);
    scsr.bottom_right_y = combined_scissor_value_br(
        regs.screen_scissor.bottom_right_y, regs.window_scissor.bottom_right_y,
        regs.generic_scissor.bottom_right_y,
        enable_offset ? regs.window_offset.window_y_offset : 0);

    boost::container::static_vector<vk::Viewport, AmdGpu::NUM_VIEWPORTS> viewports;
    boost::container::static_vector<vk::Rect2D, AmdGpu::NUM_VIEWPORTS> scissors;

    if (regs.polygon_control.enable_window_offset &&
        (regs.window_offset.window_x_offset != 0 || regs.window_offset.window_y_offset != 0)) {
        LOG_ERROR(Render_Vulkan,
                  "PA_SU_SC_MODE_CNTL.VTX_WINDOW_OFFSET_ENABLE support is not yet implemented.");
    }

    const auto& vp_ctl = regs.viewport_control;
    for (u32 i = 0; i < AmdGpu::NUM_VIEWPORTS; i++) {
        const auto& vp = regs.viewports[i];
        const auto& vp_d = regs.viewport_depths[i];
        if (vp.xscale == 0) {
            continue;
        }

        const auto zoffset = vp_ctl.zoffset_enable ? vp.zoffset : 0.f;
        const auto zscale = vp_ctl.zscale_enable ? vp.zscale : 1.f;

        vk::Viewport viewport{};

        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_pipeline_graphics.c#L688-689
        // https://gitlab.freedesktop.org/mesa/mesa/-/blob/209a0ed/src/amd/vulkan/radv_cmd_buffer.c#L3103-3109
        // When the clip space is ranged [-1...1], the zoffset is centered.
        // By reversing the above viewport calculations, we get the following:
        if (regs.clipper_control.clip_space == AmdGpu::ClipSpace::MinusWToW) {
            viewport.minDepth = zoffset - zscale;
            viewport.maxDepth = zoffset + zscale;
        } else {
            viewport.minDepth = zoffset;
            viewport.maxDepth = zoffset + zscale;
        }

        if (!instance.IsDepthRangeUnrestrictedSupported()) {
            // Unrestricted depth range not supported by device. Restrict to valid range.
            viewport.minDepth = std::max(viewport.minDepth, 0.f);
            viewport.maxDepth = std::min(viewport.maxDepth, 1.f);
        }

        if (regs.IsClipDisabled()) {
            // In case if clipping is disabled we patch the shader to convert vertex position
            // from screen space coordinates to NDC by defining a render space as full hardware
            // window range [0..16383, 0..16383] and setting the viewport to its size.
            viewport.x = 0.f;
            viewport.y = 0.f;
            viewport.width = float(std::min<u32>(instance.GetMaxViewportWidth(), 16_KB));
            viewport.height = float(std::min<u32>(instance.GetMaxViewportHeight(), 16_KB));
        } else {
            const auto xoffset = vp_ctl.xoffset_enable ? vp.xoffset : 0.f;
            const auto xscale = vp_ctl.xscale_enable ? vp.xscale : 1.f;
            const auto yoffset = vp_ctl.yoffset_enable ? vp.yoffset : 0.f;
            const auto yscale = vp_ctl.yscale_enable ? vp.yscale : 1.f;

            viewport.x = xoffset - xscale;
            viewport.y = yoffset - yscale;
            if (vr_temporal && vr_temporal->active && !vr_temporal->resolved && db_desc.first &&
                db_desc.first == vr_temporal->depth_id &&
                (regs.num_indices > 6 || regs.num_instances.NumInstances() > 1)) {
                const auto jitter = vr_temporal->scene.Jitter();
                viewport.x += jitter[0] * vr_scene_inverse[0];
                viewport.y += jitter[1] * vr_scene_inverse[1];
            }
            viewport.width = xscale * 2.0f;
            viewport.height = yscale * 2.0f;
        }

        viewport.x /= vr_scene_inverse[0];
        viewport.width /= vr_scene_inverse[0];
        viewport.y /= vr_scene_inverse[1];
        viewport.height /= vr_scene_inverse[1];
        viewports.push_back(viewport);

        auto vp_scsr = scsr;
        if (regs.mode_control.vport_scissor_enable) {
            vp_scsr.top_left_x =
                std::max(vp_scsr.top_left_x, s16(regs.viewport_scissors[i].top_left_x));
            vp_scsr.top_left_y =
                std::max(vp_scsr.top_left_y, s16(regs.viewport_scissors[i].top_left_y));
            vp_scsr.bottom_right_x = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_x),
                                              regs.viewport_scissors[i].bottom_right_x);
            vp_scsr.bottom_right_y = std::min(AmdGpu::Scissor::Clamp(vp_scsr.bottom_right_y),
                                              regs.viewport_scissors[i].bottom_right_y);
        }
        // A light or glow quad with a tile of its own: its tiles in both eyes (VrNativeDrawValues,
        // from the same camera record as the fragment test's baked or pushed bounds).
        if (i == 0 && vr_light_tiles.valid) {
            const auto& light_bounds = vr_light_tiles.bounds;
            if (g_camera_audit) {
                static thread_local std::set<std::array<std::array<float, 4>, 2>> seen;
                if (seen.insert(light_bounds).second)
                    LOG_INFO(Render_Vulkan,
                             "VR light scissor: shader {:#x} hardware union active; left [{}, {}, "
                             "{}, {}] right [{}, {}, {}, {}]",
                             pipeline->GetStages()[u32(Shader::LogicalStage::Vertex)]->pgm_hash,
                             light_bounds[0][0], light_bounds[0][1], light_bounds[0][2],
                             light_bounds[0][3], light_bounds[1][0], light_bounds[1][1],
                             light_bounds[1][2], light_bounds[1][3]);
            }
            // Expand only the hardware scissor. The fragment shader keeps each eye's
            // exact tile boundary, avoiding double lighting where the two eyes overlap.
            const float l = std::max(0.f, std::min(light_bounds[0][0], light_bounds[1][0]));
            const float t = std::max(0.f, std::min(light_bounds[0][1], light_bounds[1][1]));
            const float r = std::max(l, std::max(light_bounds[0][2], light_bounds[1][2]));
            const float b = std::max(t, std::max(light_bounds[0][3], light_bounds[1][3]));
            const s32 left = s32(std::floor(l / vr_scene_inverse[0])),
                      top = s32(std::floor(t / vr_scene_inverse[1]));
            scissors.push_back({{left, top},
                                {u32(std::max(0, s32(std::ceil(r / vr_scene_inverse[0])) - left)),
                                 u32(std::max(0, s32(std::ceil(b / vr_scene_inverse[1])) - top))}});
            continue;
        }
        const s32 left = s32(std::floor(vp_scsr.top_left_x / vr_scene_inverse[0]));
        const s32 top = s32(std::floor(vp_scsr.top_left_y / vr_scene_inverse[1]));
        const s32 right =
            s32(std::ceil((vp_scsr.top_left_x + vp_scsr.GetWidth()) / vr_scene_inverse[0]));
        const s32 bottom =
            s32(std::ceil((vp_scsr.top_left_y + vp_scsr.GetHeight()) / vr_scene_inverse[1]));
        scissors.push_back(
            {{left, top}, {u32(std::max(0, right - left)), u32(std::max(0, bottom - top))}});
    }

    if (viewports.empty()) {
        // Vulkan requires providing at least one viewport.
        constexpr vk::Viewport empty_viewport = {
            .x = -1.0f,
            .y = -1.0f,
            .width = 1.0f,
            .height = 1.0f,
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        constexpr vk::Rect2D empty_scissor = {
            .offset = {0, 0},
            .extent = {1, 1},
        };
        viewports.push_back(empty_viewport);
        scissors.push_back(empty_scissor);
    }

    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetViewports(viewports);
    dynamic_state.SetScissors(scissors);
}

void Rasterizer::UpdateDepthStencilState() const {
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto depth_test_enabled =
        regs.depth_control.depth_enable && regs.depth_buffer.DepthValid();
    dynamic_state.SetDepthTestEnabled(depth_test_enabled);
    if (depth_test_enabled) {
        dynamic_state.SetDepthWriteEnabled(regs.depth_control.depth_write_enable &&
                                           !regs.depth_render_control.depth_clear_enable);
        dynamic_state.SetDepthCompareOp(LiverpoolToVK::CompareOp(regs.depth_control.depth_func));
    }

    const auto depth_bounds_test_enabled = regs.depth_control.depth_bounds_enable;
    dynamic_state.SetDepthBoundsTestEnabled(depth_bounds_test_enabled);
    if (depth_bounds_test_enabled) {
        dynamic_state.SetDepthBounds(regs.depth_bounds_min, regs.depth_bounds_max);
    }

    const auto depth_bias_enabled = regs.polygon_control.NeedsBias();
    dynamic_state.SetDepthBiasEnabled(depth_bias_enabled);
    if (depth_bias_enabled) {
        const bool front = regs.polygon_control.enable_polygon_offset_front;
        dynamic_state.SetDepthBias(
            front ? regs.poly_offset.front_offset : regs.poly_offset.back_offset,
            regs.poly_offset.depth_bias,
            (front ? regs.poly_offset.front_scale : regs.poly_offset.back_scale) / 16.f);
    }

    const auto stencil_test_enabled =
        regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid();
    dynamic_state.SetStencilTestEnabled(stencil_test_enabled);
    if (stencil_test_enabled) {
        const StencilOps front_ops{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_front),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_front),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_front),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_ref_func),
        };
        const StencilOps back_ops = regs.depth_control.backface_enable ? StencilOps{
            .fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_fail_back),
            .pass_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zpass_back),
            .depth_fail_op = LiverpoolToVK::StencilOp(regs.stencil_control.stencil_zfail_back),
            .compare_op = LiverpoolToVK::CompareOp(regs.depth_control.stencil_bf_func),
        } : front_ops;
        dynamic_state.SetStencilOps(front_ops, back_ops);

        const bool stencil_clear = regs.depth_render_control.stencil_clear_enable;
        const auto front = regs.stencil_ref_front;
        const auto back =
            regs.depth_control.backface_enable ? regs.stencil_ref_back : regs.stencil_ref_front;
        // GCN REPLACE_OP writes DB_STENCILREFMASK.STENCILOPVAL, so a face whose stencil ops
        // include ReplaceOp takes its Vulkan reference from op_val.
        const auto& sc = regs.stencil_control;
        const auto uses_op_val = [](AmdGpu::StencilFunc fail, AmdGpu::StencilFunc zpass,
                                    AmdGpu::StencilFunc zfail) {
            return fail == AmdGpu::StencilFunc::ReplaceOp ||
                   zpass == AmdGpu::StencilFunc::ReplaceOp ||
                   zfail == AmdGpu::StencilFunc::ReplaceOp;
        };
        const bool front_op =
            uses_op_val(sc.stencil_fail_front, sc.stencil_zpass_front, sc.stencil_zfail_front);
        const bool back_op =
            regs.depth_control.backface_enable
                ? uses_op_val(sc.stencil_fail_back, sc.stencil_zpass_back, sc.stencil_zfail_back)
                : front_op;
        const auto ref_conflict = [](AmdGpu::CompareFunc func, const AmdGpu::StencilRefMask& ref) {
            return func != AmdGpu::CompareFunc::Always && func != AmdGpu::CompareFunc::Never &&
                   ref.stencil_test_val != ref.stencil_op_val;
        };
        if ((front_op && ref_conflict(regs.depth_control.stencil_ref_func, front)) ||
            (back_op && regs.depth_control.backface_enable &&
             ref_conflict(regs.depth_control.stencil_bf_func, back))) {
            LOG_WARNING(Render_Vulkan, "Stencil test requires test_val while ReplaceOp requires "
                                       "op_val; the stencil test will use op_val");
        }
        dynamic_state.SetStencilReferences(front_op ? front.stencil_op_val : front.stencil_test_val,
                                           back_op ? back.stencil_op_val : back.stencil_test_val);
        dynamic_state.SetStencilWriteMasks(!stencil_clear ? front.stencil_write_mask : 0U,
                                           !stencil_clear ? back.stencil_write_mask : 0U);
        dynamic_state.SetStencilCompareMasks(front.stencil_mask, back.stencil_mask);
    }
}

void Rasterizer::UpdatePrimitiveState(const bool is_indexed) const {
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    auto& dynamic_state = scheduler.GetDynamicState();

    const auto is_list_topology = [](const AmdGpu::PrimitiveType type) {
        const auto topology = LiverpoolToVK::PrimitiveType(type);
        return topology == vk::PrimitiveTopology::ePointList ||
               topology == vk::PrimitiveTopology::eLineList ||
               topology == vk::PrimitiveTopology::eTriangleList ||
               topology == vk::PrimitiveTopology::eLineListWithAdjacency ||
               topology == vk::PrimitiveTopology::eTriangleListWithAdjacency;
    };
    const auto is_patch_list_topology = [](const AmdGpu::PrimitiveType type) {
        // Quad and rect lists are emulated using tessellation.
        return type == AmdGpu::PrimitiveType::PatchPrimitive ||
               type == AmdGpu::PrimitiveType::QuadList || type == AmdGpu::PrimitiveType::RectList;
    };

    const auto prim_restart =
        (regs.enable_primitive_restart & 1) != 0 &&
        (instance.IsListRestartSupported() || !is_list_topology(regs.primitive_type)) &&
        (instance.IsPatchListRestartSupported() || !is_patch_list_topology(regs.primitive_type));
    ASSERT_MSG(!is_indexed || !prim_restart || regs.primitive_restart_index == 0xFFFF ||
                   regs.primitive_restart_index == 0xFFFFFFFF,
               "Primitive restart index other than -1 is not supported yet");

    const auto cull_mode = LiverpoolToVK::IsPrimitiveCulled(regs.primitive_type)
                               ? LiverpoolToVK::CullMode(regs.polygon_control.CullingMode())
                               : vk::CullModeFlagBits::eNone;
    const auto front_face = LiverpoolToVK::FrontFace(regs.polygon_control.front_face);

    dynamic_state.SetPrimitiveRestartEnabled(prim_restart);
    dynamic_state.SetRasterizerDiscardEnabled(regs.clipper_control.dx_rasterization_kill);
    dynamic_state.SetCullMode(cull_mode);
    dynamic_state.SetFrontFace(front_face);
}

void Rasterizer::UpdateRasterizationState() const {
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetLineWidth(regs.line_control.Width());
}

void Rasterizer::UpdateColorBlendingState(const GraphicsPipeline* pipeline) const {
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.SetBlendConstants(regs.blend_constants);
    dynamic_state.SetColorWriteMasks(pipeline->GetGraphicsKey().write_masks);
    dynamic_state.SetAttachmentFeedbackLoopEnabled(attachment_feedback_loop);
}

void Rasterizer::ScopeMarkerBegin(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.Commands();
    cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopeMarkerEnd(bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.Commands();
    cmdbuf.endDebugUtilsLabelEXT();
}

void Rasterizer::ScopedMarkerInsert(const std::string_view& str, bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.Commands();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
    });
}

void Rasterizer::ScopedMarkerInsertColor(const std::string_view& str, const u32 color,
                                         bool from_guest) {
    if ((from_guest && !EmulatorSettings.IsVkGuestMarkersEnabled()) ||
        (!from_guest && !EmulatorSettings.IsVkHostMarkersEnabled())) {
        return;
    }
    const auto cmdbuf = scheduler.Commands();
    cmdbuf.insertDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
        .pLabelName = str.data(),
        .color = std::array<f32, 4>(
            {(f32)((color >> 16) & 0xff) / 255.0f, (f32)((color >> 8) & 0xff) / 255.0f,
             (f32)(color & 0xff) / 255.0f, (f32)((color >> 24) & 0xff) / 255.0f})});
}

} // namespace Vulkan
