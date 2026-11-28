#include "core/vr/s1_dynamic_resolution.h"
#include "video_core/native_stereo/draw_registers.h"
#include "video_core/native_stereo/profile_scopes.h"
#include "video_core/native_stereo/push_state_memo.h"
#include "video_core/fast/b_handler_profile.h"
#include "video_core/fast/packet_codec.h"
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/native_stereo/native_stereo.h"
#include "video_core/native_stereo/lighting_reconstruction.h"
#include "video_core/renderer_vulkan/vr_shadow_hold.h"
#include "shader_recompiler/ir/passes/bump_probe.h"
#include "shader_recompiler/ir/passes/fog_grid.h"
#include "shader_recompiler/ir/passes/fog_light.h"
#include "shader_recompiler/ir/passes/particle_air.h"
#include "shader_recompiler/ir/passes/cinematic_look.h"
#include "core/vr/live_tune.h"
#include "shader_recompiler/ir/passes/air_light.h"
#include "shader_recompiler/ir/passes/bounce_light.h"
#include "shader_recompiler/ir/passes/moon_shafts.h"
#include <xxhash.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cstdio>
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
#include "common/slow_scope.h"
#include "common/memory_write_trace.h"
#include "common/debug.h"
#include "core/debug_state.h"
#include "core/emulator_settings.h"
#include "core/memory.h"
#include "core/vr/aa_mode.h"
#include "core/vr/game_area.h"
#include "core/vr/map_objects.h"
#include "core/vr/muzzle_flash.h"
#include "core/vr/tonitrus_charge.h"
#include "core/vr/torch_glow.h"
#include "video_core/renderer_vulkan/vr_bump_class.h"
#include "shader_recompiler/ir/passes/body_shadow.h"
#include "shader_recompiler/ir/passes/light_shadow.h"
#include "core/vr/graphics_settings.h"
#include "video_core/renderer_vulkan/vr_eye_axes.h"
#include "shader_recompiler/ir/passes/ambient_occlusion.h"
#include "core/vr/lamp_glow.h"
#include "core/vr/c0000_skeleton.h"
#include "core/vr/live_tune.h"
#include "core/vr/vr_session.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/fast_render.h"
#include "video_core/fast/drain_audit.h"
#include "video_core/native_stereo/diag_skip.h"
#include "video_core/renderer_vulkan/image_copy_tail.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_draw_pipe.h"
#include "video_core/renderer_vulkan/vk_gpu_pass_profiler.h"
#include "video_core/renderer_vulkan/vk_constant_ring.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_gpu_crash.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vr_frame_match.h"
#include "video_core/renderer_vulkan/vr_dispatch_mono.h"
#include "video_core/renderer_vulkan/vr_merged_eyes.h"
#include "video_core/ds_ab.h"
#include "video_core/renderer_vulkan/vr_batch_census.h"
#include "video_core/renderer_vulkan/vr_draw_census.h"
#include "video_core/renderer_vulkan/vk_prepared_bindings.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_hle.h"
#include "video_core/renderer_vulkan/vr_half_tan_memo.h"
#include "video_core/renderer_vulkan/vr_inframe.h"
#include "video_core/renderer_vulkan/vr_perf.h"
#include "video_core/renderer_vulkan/vr_render_taa.h"
#include "video_core/renderer_vulkan/vr_dibr_upscale.h"
#include "video_core/renderer_vulkan/vr_smaa.h"
#include "video_core/renderer_vulkan/vr_temporal_capture.h"
#include "video_core/renderer_vulkan/vr_temporal_jitter.h"
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
// Tests (shader_recompiler/ir/passes/bump_probe.h, VR_BUMP_PROBE=<folder>): a probed program's
// texture, its first level's bytes as stored (tiled; BC blocks whole), once per address.
void VrBumpProbe(const Shader::Info& stage, u32 index, const AmdGpu::Image& tsharp,
                 Core::MemoryManager* memory) {
    namespace Probe = Shader::Optimization::BumpProbe;
    const u32 bump = Probe::Find(stage.pgm_hash);
    if (bump == Probe::NotProbed) {
        return;
    }
    static std::mutex mutex;
    static std::unordered_set<VAddr> seen;
    static u32 written = 0;
    std::scoped_lock lock{mutex};
    const VAddr address = tsharp.Address();
    // Up to 1200 textures 256 texels wide or more (the street's own came after 120 of the
    // hunter's clothes and props in the first try).
    if (written >= 1200 || tsharp.width + 1 < 256 || !seen.insert(address).second) {
        return;
    }
    const auto fmt = static_cast<u32>(tsharp.data_format);
    const bool bc8 = fmt == 35 || fmt == 38;                      // BC1, BC4: 8-byte blocks
    const bool bc16 = fmt == 36 || fmt == 37 || (fmt >= 39 && fmt <= 41); // BC2, 3, 5, 6, 7
    const u32 block = bc8 || bc16 ? 4 : 1;
    const u32 bytes = bc8 ? 8 : bc16 ? 16 : fmt == 10 ? 4 : 0;   // Format8_8_8_8 = 10
    const u32 w = tsharp.width + 1, h = tsharp.height + 1, pitch = tsharp.Pitch();
    if (bytes == 0) {
        LOG_INFO(Render_Vulkan, "VR bump probe: shader {:#x} image {}{}: format {} not written",
                 stage.pgm_hash, index, index == bump ? " (bump)" : "", fmt);
        return;
    }
    const u64 size = std::min<u64>(u64{(pitch + block - 1) / block} * ((h + block - 1) / block) * bytes,
                                   8ull << 20);
    if (!memory->IsValidGpuMapping(address, size)) {
        return;
    }
    const std::string path = fmt::format("{}/{:08x}_img{}_{:x}.bin", Probe::Dir(), stage.pgm_hash, index, address);
    if (std::FILE* const f = std::fopen(path.c_str(), "wb")) {
        std::fwrite(std::bit_cast<const u8*>(address), 1, size, f);
        std::fclose(f);
        ++written;
    }
    LOG_INFO(Render_Vulkan,
             "VR bump probe: shader {:#x} image {}{}: {:#x} {}x{} pitch {} format {} num {} "
             "tiling {} sel {}{}{}{} levels {}..{} -> {}",
             stage.pgm_hash, index, index == bump ? " (bump)" : "", address, w, h, pitch, fmt,
             static_cast<u32>(tsharp.num_format), static_cast<u32>(tsharp.tiling_index),
             static_cast<u32>(tsharp.dst_sel_x), static_cast<u32>(tsharp.dst_sel_y),
             static_cast<u32>(tsharp.dst_sel_z), static_cast<u32>(tsharp.dst_sel_w),
             static_cast<u32>(tsharp.base_level), static_cast<u32>(tsharp.last_level), path);
}

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
// FAST_CPU_COPY_WRITES ('0' off, on by default since 0.5.5; read once):
// DrawPacket::AddCopyShaderWrites.
const bool g_copy_writes = [] {
    const char* value = std::getenv("FAST_CPU_COPY_WRITES");
    return !(value && value[0] == '0');
}();
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
// FAST_RING_DEDUP=1 (off by default; bbport 0.7 BB_RING_DEDUP): a constant whose bytes the current
// ring page already holds (same address, size and content; for camera and lighting buffers the
// same camera) binds that copy and its eye pairs: no second write into the ring's
// write-combined pages, no second GPU read over the bus, no second stereo pairing on A. Off: the
// hash and scratch copy cost Stage A 8-17% more cycles per draw than they saved (10-29, 4c/4t to
// full CPU, no fps change); kept for PCs that are short on memory bandwidth.
// FAST_RING_DEDUP_VERIFY=1 keeps each slot's bytes and compares them on a hit.
const bool g_ring_dedup = [] {
    const char* value = std::getenv("FAST_RING_DEDUP");
    return value && value[0] == '1';
}();
const bool g_ring_dedup_verify = [] {
    const char* value = std::getenv("FAST_RING_DEDUP_VERIFY");
    return value && value[0] == '1';
}();
struct RingReuse {
    u64 key{}, address{}, bytes{}, generation{}, ring{}, camera_key{};
    vk::DescriptorBufferInfo raw{}, left{}, right{};
    bool flat{}, camera{}, lighting{}, projected{}, mirrored{};
    std::vector<u8> shadow; // FAST_RING_DEDUP_VERIFY
};
constexpr size_t RingReuseSlots = 4096;
// Stage A only (PrepareConstants), direct-mapped by the content hash.
std::array<RingReuse, RingReuseSlots>& RingReuseTable() {
    static auto table = std::make_unique<std::array<RingReuse, RingReuseSlots>>();
    return *table;
}
struct RingDedupStats {
    u64 hits{}, misses{}, mismatches{}, bytes_saved{};
} g_ring_dedup_stats;
// A new ConstantRing restarts its generations: slots of an earlier ring never match.
u64 g_ring_serial = 0;
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
// FAST_CPU_SLIM_SIZES=1 (diagnostic): the slim form's bytes by part (EncodeSlim's size pass, stage
// A), averaged per packet in a line after FAST_CPU_SLIM_PACKET's.
const bool g_slim_sizes = [] {
    const char* value = std::getenv("FAST_CPU_SLIM_SIZES");
    return value && value[0] == '1';
}();
constexpr std::array<const char*, 13> SlimPartNames = {
    "header", "camera", "compute", "constants", "vertex", "deltas", "verify",
    "stage_ids", "user", "flat", "reads", "resources", "memory"};
thread_local std::array<u64, 13> t_slim_sizes{};
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
                LOG_INFO(Render_Vulkan, "Owned draw inputs enabled (serial validation)");
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
    // FAST_CPU_COPY_WRITES: the game's copy shader (vk_shader_hle.cpp COPY_SHADER_HASH, run as
    // buffer copies on B) writes only its copies' destinations, which its control buffer lists
    // ({dst_idx, src_idx, end} per group, the HLE's own arithmetic); its destination descriptor
    // covers ~4 GB, which made every constant-engine dump wait for B to drain (2026-10-24).
    // False: not that shader's destination, or a list that cannot be read (the descriptor then).
    bool AddCopyShaderWrites(const Shader::Info& stage, const Shader::BufferResource& desc) {
        static constexpr u64 CopyShaderHash = 0xfefebf9f;
        if (!g_copy_writes || stage.pgm_hash != CopyShaderHash ||
            stage.l_stage != Shader::LogicalStage::Compute || stage.buffers.size() < 3 ||
            &desc != &stage.buffers[2])
            return false;
        struct Control {
            u32 dst_idx, src_idx, end;
        };
        const auto ctl = stage.buffers[0].GetSharp(stage);
        const auto dst = stage.buffers[2].GetSharp(stage);
        const u32 stride = dst.GetStride();
        const u32 count = compute.dim_x;
        const u64 list_bytes = u64(count) * sizeof(Control);
        if (ctl.GetStride() != sizeof(Control) || !stride || !ctl.base_address ||
            !dst.base_address || count == 0 || count > 4096 ||
            Core::Memory::Instance()->ClampRangeSize(ctl.base_address, list_bytes) < list_bytes) {
            static u32 logged = 0;
            if (logged++ < 8)
                LOG_INFO(Render_Vulkan,
                         "FAST_CPU_COPY_WRITES: list not taken: ctl stride {} base {:#x} dst stride "
                         "{} base {:#x} groups {}",
                         ctl.GetStride(), ctl.base_address, stride, dst.base_address, count);
            return false;
        }
        static u32 taken = 0;
        if (taken++ < 4)
            LOG_INFO(Render_Vulkan, "FAST_CPU_COPY_WRITES: {} copies to {:#x}+", count,
                     dst.base_address);
        const auto* list = reinterpret_cast<const Control*>(ctl.base_address);
        // The copies' destinations, sorted and merged where less than a page apart (the guards
        // compare by page at the finest), so a list scattered over gigabytes stays a few ranges.
        static thread_local std::vector<std::pair<u64, u64>> ranges;
        ranges.clear();
        u64 lo = ~u64{0}, hi = 0;
        for (u32 i = 0; i < count; ++i) {
            const u32 offset = list[i].dst_idx * stride; // u32, as ExecuteCopyShaderHLE
            const u32 size = (list[i].end + 1) * stride;
            if (size) {
                ranges.emplace_back(dst.base_address + offset, dst.base_address + offset + size);
                lo = std::min(lo, ranges.back().first);
                hi = std::max(hi, ranges.back().second);
            }
        }
        static constexpr u64 Page = 4096;
        // A/B switch 2 (2026-11-02; this sort was ~2% of stage A at 320x240): the written pages
        // without a sort. The guards compare by page at the finest, so the pages the copies touch,
        // in runs of consecutive pages, are what they need (the merge below may also join two
        // runs one untouched page apart: more than written, never less). A span over 64 MiB
        // takes the sort.
        if (FastRender::AbOn(2) && !ranges.empty() && hi - lo <= (u64{64} << 20)) {
            static thread_local std::vector<u64> pages_set;
            const u64 first_page = lo / Page;
            const u64 page_count = (hi - 1) / Page - first_page + 1;
            pages_set.assign((page_count + 63) / 64, 0);
            for (const auto& [b, e] : ranges) {
                // Pages [first, last] of this copy, a word at a time.
                const u64 first = b / Page - first_page, last = (e - 1) / Page - first_page;
                for (u64 w = first / 64; w <= last / 64; ++w) {
                    const u64 from = w == first / 64 ? first % 64 : 0;
                    const u64 to = w == last / 64 ? last % 64 : 63;
                    const u64 high = to == 63 ? ~u64{0} : (u64{2} << to) - 1;
                    pages_set[w] |= high & (~u64{0} << from);
                }
            }
            // The runs of set pages, a word at a time (bits past page_count are clear).
            const size_t words = pages_set.size();
            u64 p = 0;
            while (p < page_count) {
                size_t w = p / 64;
                u64 word = pages_set[w] & (~u64{0} << (p % 64));
                while (!word && ++w < words)
                    word = pages_set[w];
                if (!word)
                    break;
                const u64 start = w * 64 + std::countr_zero(word);
                w = start / 64;
                u64 clear = ~pages_set[w] & (~u64{0} << (start % 64));
                while (!clear && ++w < words)
                    clear = ~pages_set[w];
                const u64 stop =
                    clear ? std::min<u64>(w * 64 + std::countr_zero(clear), page_count) : page_count;
                AddWrite((first_page + start) * Page, (stop - start) * Page);
                p = stop;
            }
            return true;
        }
        std::sort(ranges.begin(), ranges.end());
        u64 begin = 0, end = 0;
        for (const auto& [b, e] : ranges) {
            if (end && b <= end + Page) {
                end = std::max(end, e);
                continue;
            }
            if (end)
                AddWrite(begin, end - begin);
            begin = b;
            end = e;
        }
        if (end)
            AddWrite(begin, end - begin);
        return true;
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
                        if (desc.is_written && !desc.IsSpecial()) {
                            if (!AddCopyShaderWrites(*stage, desc))
                                AddWrite(sharp.base_address, sharp.GetSize());
                            if (sharp.GetSize() > (u64{64} << 20)) {
                                // Diagnostic: the writes PendingWriteGuard calls big (logged once each).
                                static std::set<std::tuple<u64, u64, u64>> seen;
                                if (seen.size() < 64 &&
                                    seen.emplace(stage->pgm_hash, sharp.base_address, sharp.GetSize()).second)
                                    LOG_INFO(Render_Vulkan,
                                             "FAST big write: shader {:#x} stage {} binding {} base {:#x} "
                                             "size {:#x} stride {} records {} mapped {:#x}",
                                             stage->pgm_hash, static_cast<u32>(stage->l_stage),
                                             &desc - stage->buffers.data(),
                                             sharp.base_address, sharp.GetSize(), sharp.GetStride(),
                                             sharp.num_records,
                                             Core::Memory::Instance()->ClampRangeSize(
                                                 sharp.base_address, sharp.GetSize()));
                            }
                        }
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
                        (size == 864 || size == 208 || size == 272 || size == 336 ||
                         size == 416 ||
                         (size == 320 &&
                          stage->pgm_hash == NativeProjection::ReflectionProgram)) &&
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
            if (g_slim_sizes && (slim_packets & 65535) == 0) {
                std::string parts;
                for (size_t i = 0; i < SlimPartNames.size(); ++i)
                    parts += fmt::format(" {}={:.0f}", SlimPartNames[i],
                                         double(t_slim_sizes[i]) / double(slim_packets));
                LOG_INFO(Render_Vulkan, "FAST_CPU_SLIM_SIZES: bytes/packet by part:{}", parts);
            }
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
        // FAST_CPU_SLIM_SIZES: the size pass adds each part's bytes (the stage parts summed).
        const bool sizes = g_slim_sizes && output == nullptr;
        size_t mark = 0;
        const auto part = [&](size_t i) {
            if (sizes) {
                t_slim_sizes[i] += writer.Size() - mark;
                mark = writer.Size();
            }
        };
        value(h);
        part(0);
        if (send_camera)
            value(camera);
        part(1);
        if (send_compute)
            value(compute);
        part(2);
        writer.ArrayIf(constants, SlimKeeps);
        part(3);
        if (vertex_prepared) {
            words(vertex.attributes);
            words(vertex.bindings);
            words(vertex.guest_buffers);
            words(vertex.ranges);
        }
        part(4);
        words(deltas);
        part(5);
        if (verify)
            value(*reference);
        part(6);
        for (size_t i = 0; i < stage_count; ++i) {
            value(owned[i].owner);
            value(owned[i].program_base);
            part(7);
            words(owned[i].user);
            part(8);
            words(owned[i].flat);
            part(9);
            words(reads[i]);
            part(10);
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
            part(11);
        }
        for (size_t i = 0; i < memory_count; ++i) {
            value(memory[i].address);
            words(memory[i].words);
        }
        part(12);
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
            v.blade_clip.normal != w.blade_clip.normal || v.blade_clip.offset != w.blade_clip.offset ||
            v.free_eye_cut != w.free_eye_cut || v.probe_selected != w.probe_selected)
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
        u32 primitive_type{};
        std::array<u32, 2> jitter{}, inverse{};
        uint64_t clip_key{};
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

    void Update(const Rasterizer& r, const GraphicsPipeline* pipeline, bool is_indexed,
                std::array<float, 2> jitter) {
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
            r.UpdateViewportScissorState(pipeline, jitter);
        } else {
            const ViewportInputs inputs{
                .primitive_type = primitive_type,
                .jitter = {std::bit_cast<u32>(jitter[0]), std::bit_cast<u32>(jitter[1])},
                .inverse = {std::bit_cast<u32>(r.vr_scene_inverse[0]),
                            std::bit_cast<u32>(r.vr_scene_inverse[1])},
                .clip_key=r.drs.scissor_key,
            };
            if (!Lookup(0, viewport, inputs, owned, regs, DynSkipViewportRanges)) {
                r.UpdateViewportScissorState(pipeline, jitter);
                viewports = state.viewports;
                scissors = state.scissors;
            } else if (Verify()) {
                r.UpdateViewportScissorState(pipeline, jitter);
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
    packet.vr.shadow_near = kind == PacketKind::Draw || kind == PacketKind::DrawIndirect ? shadow_near_a : 0u;
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
    // A draw into a shadow atlas tile: steadied (vr_shadow_hold.h; its tile's light was seen by
    // PrepareConstants at the latest now).
    if (prepare_constants && (kind == PacketKind::Draw || kind == PacketKind::DrawIndirect) && ShadowHold::Active()) {
        if (const auto snap = ShadowHold::Snap()) {
            packet.vr.shadow_snapped = true;
            packet.vr.shadow_snap = *snap;
        }
    }
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
        if (packet.offset == WaterReflectionMark) {
            WaterReflectionClear();
            break;
        }
        FilterDraw();
        break;
    case PacketKind::Draw:
        vr_shadow_near_clear = packet.vr.shadow_near != 0;
        vr_shadow_near_serial = packet.vr.shadow_near;
        DrawRecord(static_cast<const GraphicsPipeline*>(packet.selected), packet.indexed,
                   packet.offset);
        vr_shadow_near_clear = false;
        break;
    case PacketKind::DrawIndirect:
        vr_shadow_near_clear = packet.vr.shadow_near != 0;
        vr_shadow_near_serial = packet.vr.shadow_near;
        DrawIndirectRecord(static_cast<const GraphicsPipeline*>(packet.selected), packet.indexed,
                           packet.address, packet.offset, packet.size, packet.count,
                           packet.count_address);
        vr_shadow_near_clear = false;
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
    if (!constant_ring) {
        constant_ring = std::make_unique<ConstantRing>(instance, scheduler);
        ++g_ring_serial;
    }
    const u64 constant_ring_serial = g_ring_serial;
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
    // FAST_RING_DEDUP: the camera and lighting buffers' eye pairs depend on the packet's camera as
    // well (and the mirror flip); hashed once per packet when one is looked up.
    u64 camera_key = 0;
    const auto CameraKey = [&] {
        if (!camera_key)
            camera_key = XXH3_64bits_withSeed(&packet.camera, sizeof(packet.camera),
                                              VrMirrorFlip() ? 2 : 1) | 1;
        return camera_key;
    };
    // (Every size LightingBuffer pairs below: wet streets' 320 too, whose pair follows the frame's
    // dynamic resolution step and the last one's, 2026-11-24.)
    const auto Paired = [](const auto& c) {
        return !c.flat && (c.bytes == 864 || c.bytes == 208 || c.bytes == 272 || c.bytes == 320 ||
                           c.bytes == 336 || c.bytes == 416);
    };
    const auto SlotKey = [](const auto& c, const u8* bytes) {
        const u64 address = c.flat ? 0 : c.sharp.base_address;
        return XXH3_64bits_withSeed(bytes, c.bytes, address ^ (c.bytes << 48) ^ u64(c.flat));
    };
    // A copy of the same bytes in this ring page: the page is reused only once the GPU is past
    // every packet that used it, and this packet marks it used as well (constant_page).
    const auto ReuseRingConstant = [&](auto& c, const u8* bytes) {
        auto& table = RingReuseTable();
        const u64 key = SlotKey(c, bytes);
        RingReuse& slot = table[key & (RingReuseSlots - 1)];
        const u64 address = c.flat ? 0 : c.sharp.base_address;
        const bool paired = Paired(c);
        if (slot.key != key || slot.address != address || slot.bytes != c.bytes ||
            slot.flat != c.flat || slot.generation != constant_ring->AllocationGeneration() ||
            slot.ring != constant_ring_serial || (paired && slot.camera_key != CameraKey())) {
            ++g_ring_dedup_stats.misses;
            return false;
        }
        if (g_ring_dedup_verify && std::memcmp(slot.shadow.data(), bytes, c.bytes) != 0) {
            if (++g_ring_dedup_stats.mismatches <= 16)
                LOG_ERROR(Render_Vulkan, "FAST_RING_DEDUP verify: {:#x}+{} hashed equal to a "
                                         "different copy; a new copy is used",
                          address, c.bytes);
            ++g_ring_dedup_stats.misses;
            return false;
        }
        c.raw = slot.raw;
        c.left = slot.left;
        c.right = slot.right;
        c.camera = slot.camera;
        c.lighting = slot.lighting;
        c.projected = slot.projected;
        c.mirrored = slot.mirrored;
        ++g_ring_dedup_stats.hits;
        g_ring_dedup_stats.bytes_saved += c.bytes + (c.camera || c.lighting ? 2 * c.bytes : 0);
        return true;
    };
    const auto RememberRingConstant = [&](const auto& c, const u8* bytes) {
        const u64 key = SlotKey(c, bytes);
        RingReuse& slot = RingReuseTable()[key & (RingReuseSlots - 1)];
        slot.key = key;
        slot.address = c.flat ? 0 : c.sharp.base_address;
        slot.bytes = c.bytes;
        slot.flat = c.flat;
        slot.generation = constant_ring->AllocationGeneration();
        slot.ring = constant_ring_serial;
        slot.camera_key = Paired(c) ? CameraKey() : 0;
        slot.raw = c.raw;
        slot.left = c.left;
        slot.right = c.right;
        slot.camera = c.camera;
        slot.lighting = c.lighting;
        slot.projected = c.projected;
        slot.mirrored = c.mirrored;
        if (g_ring_dedup_verify)
            slot.shadow.assign(bytes, bytes + c.bytes);
    };
    // A/B switch 1 (FAST_CPU_SPARSE_HOLD): one shared hold of the mapping lock for this draw's
    // guest copies (the loop never waits for B nor takes the lock exclusively: the ring space is
    // reserved above).
    Core::MemoryManager::SparseReadHold sparse_reads{*memory};
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
        // FAST_RING_DEDUP: the bytes go through a cache-hot scratch copy first; a constant this
        // ring page already holds (RingReuse) binds that copy and its eye pairs instead.
        const bool dedup = g_ring_dedup && !probe;
        const u8* scratch = nullptr;
        if (dedup) {
            thread_local std::vector<u8> bytes;
            bytes.resize(c.bytes);
            if (c.flat) {
                std::memcpy(bytes.data(), packet.owned[c.input].flat.data(), c.bytes);
            } else {
                const auto* source = ScratchDraw::MemorySource(
                    reinterpret_cast<const void*>(c.sharp.base_address), c.bytes);
                if (source == reinterpret_cast<const void*>(c.sharp.base_address))
                    memory->CopySparseMemory(c.sharp.base_address, bytes.data(), c.bytes);
                else std::memcpy(bytes.data(), source, c.bytes);
            }
            scratch = bytes.data();
            if (ReuseRingConstant(c, scratch)) {
                packet.constant_page = constant_ring->Page();
                ++constant_prepared;
                // (The same bytes as a draw before: still this draw's light, vr_shadow_hold.h.)
                if (!c.flat && c.bytes == 864 && ShadowHold::Active()) [[unlikely]]
                    ShadowHold::LightCamera(reinterpret_cast<const float*>(scratch));
                continue;
            }
        }
        const auto [target, offset] = constant_ring->Map(c.bytes, alignment);
        if (scratch) {
            std::memcpy(target, scratch, c.bytes);
        } else if (c.flat) {
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
        // The stereo pairs read the bytes from the scratch copy when there is one (cached memory,
        // not the ring's write-combined pages).
        const auto* floats = reinterpret_cast<const float*>(scratch ? scratch : target);
        if (probe) {
            c.probe = true;
            c.left = {constant_ring->Handle(), offset, c.bytes};
            ++g.probes;
            continue;
        }
        ++constant_prepared;
        c.raw = {constant_ring->Handle(), offset, c.bytes};
        // The moon's shadow atlas held over frames (vr_shadow_hold.h; a test switch): the scene's
        // camera, and a held frame's cascades replaced by the held atlas' own (the ring's raw
        // copy too: what binds it unpaired reads the same atlas).
        std::array<float, 84> held_cascades;
        const float* lighting_floats = floats;
        if (ShadowHold::Active() && !c.flat) [[unlikely]] {
            if (c.bytes == 864) {
                ShadowHold::Camera(floats);
                ShadowHold::LightCamera(floats);
            } else if (c.bytes == 336 && ShadowHold::Cascades(floats, held_cascades.data())) {
                lighting_floats = held_cascades.data();
                std::memcpy(target, held_cascades.data(), 336);
            }
        }
        if (!c.flat && c.bytes == 864)
            c.camera = NativeStereo::CameraBuffer(*constant_ring, floats,
                alignment, packet.camera, c.left, c.right, &c.projected,
                VrMirrorFlip() ? &c.mirrored : nullptr);
        else if (!c.flat && (c.bytes == 208 || c.bytes == 272 || c.bytes == 320 ||
                             c.bytes == 336 || c.bytes == 416))
            c.lighting = NativeStereo::LightingBuffer(*constant_ring, lighting_floats,
                static_cast<u32>(c.bytes), alignment, packet.camera, c.left, c.right);
        if (dedup)
            RememberRingConstant(c, scratch);
    }
    // Flat constants are A's own copies of the packet's user data: no download can change them,
    // so a packet with no guest constant needs no check on B.
    if (!guest_constants && !packet.constant_verify)
        packet.constant_downloads = DrawPacket::NoDownloads;
    if ((packet.sequence & 65535) == 0) {
        LOG_INFO(Render_Vulkan,
                 "FAST constant prepare: candidates={} prepared={} same_draw={} pending={} gpu_dirty={} "
                 "table_mode={} pending_unknown={} pending_invalid={} pending_range={} pending_table={} "
                 "pending_big={} guard_ranges={} guard_unknown_adds={} ring_dedup={} hits={} "
                 "misses={} saved_mb={:.1f} verify_mismatches={}",
                 constant_candidates, constant_prepared, constant_same_draw_refused,
                 constant_pending_refused, constant_gpu_refused, constant_writes.TableMode(),
                 constant_refused_by[1], constant_refused_by[2], constant_refused_by[3],
                 constant_refused_by[4], constant_refused_by[5], constant_writes.RangeCount(),
                 constant_writes.UnknownAdds(),
                 g_ring_dedup ? (g_ring_dedup_verify ? "verify" : "on") : "off",
                 g_ring_dedup_stats.hits, g_ring_dedup_stats.misses,
                 double(g_ring_dedup_stats.bytes_saved) / (1 << 20), g_ring_dedup_stats.mismatches);
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

#include "video_core/native_stereo/native_audit.inl"

// FAST_CPU_STAGEB_SPLIT=2: what fast:StageB2 encodes of a plain draw, after the slot's blob.
struct Rasterizer::SplitDrawJob {
    Pipeline::Prepared resources; // barriers recorded on stage B (EncodeBarriers)
    VertexBinds binds;
    DynamicState dynamic;
    Shader::PushData push_data;
    Shader::VrPushData vr_push;
    const GraphicsPipeline* pipeline{};
    vk::Pipeline handle;
    u32 count{}, instances{}, vertex_offset{}, instance_offset{};
    bool indexed{};
};

void Rasterizer::SplitEncode(void* context, u32 slot) {
    auto& r = *static_cast<Rasterizer*>(context);
    auto& job = r.split_jobs[slot];
    const auto cmdbuf = r.scheduler.Commands();
    VideoCore::BufferCache::EncodeVertexBinds(job.binds, cmdbuf);
    job.pipeline->EncodeResources(job.resources, job.push_data, job.vr_push);
    auto& tracker = r.scheduler.RingDynamicState();
    tracker.SetFrom(job.dynamic);
    tracker.Commit(r.instance, cmdbuf);
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, job.handle);
    const EyeDraws eyes{.layout = job.pipeline->GetLayout(),
                        .hud = job.push_data.vr_hud,
                        .both = job.pipeline->GetGraphicsKey().probe_view_mask == 3 &&
                                NativeStereo::LayerDraws()};
    DrawEyes(cmdbuf, eyes, [&] {
        if (job.indexed) {
            cmdbuf.drawIndexed(job.count, job.instances, 0, s32(job.vertex_offset),
                               job.instance_offset);
        } else {
            cmdbuf.draw(job.count, job.instances, job.vertex_offset, job.instance_offset);
        }
    });
}

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
    // VR_STALL_SAMPLER only (logs-on builds): flat mode has no VR presenter frame to start it.
    Common::Slow::StartSampler();
    // Bit 40's skip rule in native stereo asks the VR state (Stage A, vk_rasterizer_vr_draws.cpp).
    pipeline_cache.SetVrSkipVeto([this](const Shader::Info* vs) { return VrSkipVeto(vs); });
    vr_body_maps = std::make_unique<VrBodyMaps>(instance, scheduler); // no buffers until used
    vr_pyramid = std::make_unique<VrDepthPyramid>(instance, scheduler); // no buffer until used
    vr_edges = std::make_unique<VrShadowEdges>(instance, scheduler);    // no buffer until used
    vr_fog_grid = std::make_unique<VrFogGrid>(instance, scheduler);     // nothing until used
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
    if (StageBSplitThreadRequested() && scheduler.StageBSplit()) {
        split_jobs = std::make_unique<SplitDrawJob[]>(Scheduler::SplitSlots);
        scheduler.StartSplitThread(&Rasterizer::SplitEncode, this);
    }
}

Rasterizer::~Rasterizer() {
    published_pipe.store(nullptr, std::memory_order_release);
#ifdef _WIN32
    if (draw_pipe) {
        draw_pipe->Drain(DrawPipe::Shutdown);
        scheduler.SetStageBThread(nullptr);
    }
#endif
    draw_pipe.reset(); // Drain and join before destroying caches, packets or scheduler users.
    scheduler.SplitSync("shutdown"); // fast:StageB2 done with this rasterizer's jobs
    if (buffer_cache.SparseEnabled()) {
        // Deferred cleanups hold this rasterizer's caches (normal ones) and the sparse pool
        // (priority twin releases, which run on the scheduler's thread, outliving the caches):
        // every one runs while all caches still stand. Members go texture cache first.
        scheduler.DrainDeferredOperations();
    }
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

// FAST_CE_TASK_READS=0: every ordered task counts as reading unlisted guest memory, as before.
static const bool g_ce_task_reads = [] {
    const char* value = std::getenv("FAST_CE_TASK_READS");
    return !value || value[0] != '0';
}();

u64 Rasterizer::QueueOrderedTask(void (*handler)(void*, std::span<const std::byte>), void* context,
                                 std::span<const std::byte> packet, bool writes_known,
                                 bool reads_guest) {
    if (!worker_active || !FastRender::On(FastRender::OrderedFenceTasks))
        return false;
    const auto ticket = draw_pipe->PushTask(handler, context, packet);
    if (constant_mode && !writes_known) {
        constant_writes.Unknown(ticket);
        pending_writes.Unknown(ticket);
    }
    // Ordered tasks that may read guest memory they do not list (DMA, flips, the frame end) hold
    // back later CE dumps until B is past them. Write-only ones (fences, WriteData, query
    // results, submission retire and idle) do not: their writes go through the write guard.
    if (ce.mode) {
        if (reads_guest || !g_ce_task_reads)
            ce.untracked = ticket;
        else
            ++ce.write_only_tasks;
    }
    return ticket;
}

// FAST_CPU_CE_TICKET ('1' on, off by default; read once): see CeDumpWait.
static const bool g_ce_ticket = [] {
    const char* value = std::getenv("FAST_CPU_CE_TICKET");
    return value && value[0] == '1';
}();

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
            using Hit = FastRender::PendingWriteGuard::Hit;
            const Hit hit = g_guard && g_guard_ce && !texture_cache.ReadsBackLinearImages()
                                ? pending_writes.Check(address, bytes, progress,
                                                       FastRender::PendingWriteGuard::Match::Pages)
                                : Hit::Count;
            ++ce.guard_hits[static_cast<u32>(hit)];
            // FAST_CPU_CE_TICKET: a queued write (or untracked task) in these pages needs only
            // B past it, as the full drain makes sure: wait for the newest such ticket instead.
            u64 ticket_wait = 0;
            if (g_ce_ticket && (hit == Hit::Write || hit == Hit::Page || hit == Hit::Big ||
                                hit == Hit::Unknown)) {
                ticket_wait = pending_writes.NewestPending(
                    address, bytes, progress, FastRender::PendingWriteGuard::Match::Pages);
            }
            const bool new_full = g_guard && hit != Hit::None &&
                                  (ticket_wait == 0 || ticket_wait == UINT64_MAX);
            if (g_guard && !g_guard_verify && !new_full && hit != Hit::None) {
                ++ce.ticket_waits;
                ce.wait_ns += draw_pipe->WaitFor(ticket_wait);
            }
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
                 "untracked_nopipe={} dma={} meta={} imgfmt={} indirect={} huge={} box={:#x}..{:#x} "
                 "guard hits none={} unknown={} invalid={} write={} page={} big={} signal_queued={} "
                 "signal_unwritten={} off={} ticket_waits={} write_only_tasks={}",
                 ce.dumps, ce.skipped, ce.waited, ce.wait_ns / 1000, ce.untracked_waits, ce.full_off,
                 ce.full_mode, ce.full_box, ce.full_guard, ce.notes, ce.untracked_packets,
                 ce.untracked_by[0], ce.untracked_by[1], ce.untracked_by[2], ce.untracked_by[3],
                 ce.untracked_by[4], ce.untracked_by[5], ce.lo, ce.hi, ce.guard_hits[0],
                 ce.guard_hits[1], ce.guard_hits[2], ce.guard_hits[3], ce.guard_hits[4],
                 ce.guard_hits[5], ce.guard_hits[6], ce.guard_hits[7], ce.guard_hits[8],
                 ce.ticket_waits, ce.write_only_tasks);
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
#ifdef _WIN32
    // Producer handoff is drained here. Hand back the borrowed handle before pipe teardown.
    scheduler.SetStageBThread(requested && NativeStereo::Enabled() && !split_jobs
                                  ? draw_pipe->NativeThread() : nullptr);
#endif
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

    const auto glow_cut = VrGlowDepthCut(pipeline);
    vr_glow_depth_min = -1.0f;
    if ((regs.depth_control.depth_enable && regs.depth_buffer.DepthValid()) ||
        (regs.depth_control.stencil_enable && regs.depth_buffer.StencilValid()) || glow_cut) {
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
        if (glow_cut && !regs.depth_control.depth_enable && !regs.depth_control.stencil_enable) {
            // Only over the scene the depth belongs to, both of them eye pairs (the glow's blur
            // into its own small target has the same shaders and registers).
            const auto& color = texture_cache.GetImage(cb_descs[0].first);
            if (!image.native_stereo || !color.native_stereo ||
                color.info.size.width != image.info.size.width ||
                color.info.size.height != image.info.size.height) {
                bound_images.pop_back();
                image_id = {};
                return;
            }
            vr_glow_depth_min = *glow_cut;
        }
        image.binding.is_target = 1u;
    } else {
        db_desc.first = {};
    }
}

// Rasterizer::vr_glow_depth_min's draw, before its targets are found: the god rays' composite
// shaders with colour target 0, a valid depth the game leaves off, native stereo, the depth
// bounds feature and the camera's projection. The stored depth of a point glow_near_m in front:
// the game's D3D-style 0..1, z_ndc = f/(f-n) - n*f/((f-n)*z) (vr_stereo_warp_math.h).
std::optional<float> Rasterizer::VrGlowDepthCut(const GraphicsPipeline* pipeline) const {
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    const float near_m = VR::GetSettings().glow_near_m;
    if (near_m <= 0.0f || !NativeStereo::Enabled() || !instance.IsDepthBoundsSupported() ||
        !regs.depth_buffer.DepthValid() || !cb_descs[0].first ||
        (pipeline->GetGraphicsKey().mrt_mask & 1) == 0) {
        return std::nullopt;
    }
    const auto& stages = pipeline->GetStages();
    const auto* vs = stages[u32(Shader::LogicalStage::Vertex)];
    const auto* fs = stages[u32(Shader::LogicalStage::Fragment)];
    if (!vs || !fs || !NativeProjection::GlowRaysComposite(vs->pgm_hash, fs->pgm_hash)) {
        return std::nullopt;
    }
    const auto& record = VR::DrawFrameRecord();
    const float n = record.near_z, f = record.far_z;
    if (!record.has_projection || !(n > 0.0f) || !(f > n)) {
        return std::nullopt;
    }
    const float z = std::max(near_m * record.world_scale, n);
    return std::clamp(f / (f - n) - n * f / ((f - n) * z), 0.0f, 1.0f);
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
        if (!IsRegularDraw() || ShadowCellSkipped(true) || GameLampMapSkipped()) {
            RecordPacket({}, nullptr, PacketKind::FilteredDraw);
            return;
        }
        if (WaterReflectionSkipped()) {
            // Stage B clears its target once a frame (WaterReflectionClear).
            RecordPacket({}, nullptr, PacketKind::FilteredDraw, false, 0, WaterReflectionMark);
            return;
        }
    } else if (!FilterDraw() || ShadowCellSkipped(true) || GameLampMapSkipped()) {
        return;
    } else if (WaterReflectionSkipped()) {
        WaterReflectionClear();
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
    if (WaterReflectionCapped(std::as_const(ScratchDraw::Registers(liverpool)).num_indices,
                              pipeline))
        return; // over the water reflection's budget (no packet: nothing to draw)
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
        vr_shadow_near_clear = shadow_near_a != 0;
        vr_shadow_near_serial = shadow_near_a;
        DrawRecord(pipeline, is_indexed, index_offset);
        vr_shadow_near_clear = false;
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
        if (base != nullptr && vr_warp && cut != 0 &&
            (base->GetGraphicsKey().mrt_mask != 0 || cut == Shader::VrCut::BladeMouth)) {
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
// The bump map bound for this draw (vr_bump_class.h): its class, from the texture's own bytes the
// first time its address, size and format are seen (BC1 and BC3 only; any other format, an
// unmapped range or a texture too small: not a height map), and again when its turn comes
// (vk_rasterizer.h VrBumpState). Reads at most SampleBlocks blocks.
void Rasterizer::VrBumpClassify(const Shader::Info& stage, const Shader::ImageResource& desc,
                                const AmdGpu::Image& tsharp) {
    // The heights made since the last frame: their images take them at their next use.
    auto& heights = texture_cache.BumpHeights();
    if (vr_bump.poll_frame != vr_bump.frame) {
        vr_bump.poll_frame = vr_bump.frame;
        vr_bump.ready.clear();
        heights.Poll(vr_bump.ready);
        for (const auto& [address, size] : vr_bump.ready) {
            // The derived bytes now replace this range on upload.
            texture_cache.InvalidateMemory(address, size,
                                           VideoCore::TextureMemory::Invalidation::HostWrite);
        }
    }
    const u32 fmt = static_cast<u32>(tsharp.data_format);
    const u32 width = tsharp.width + 1, height = tsharp.height + 1, pitch = tsharp.Pitch();
    // (Until 2026-11-12 the size's bits were xor-ed over the address' own.)
    const u64 key = tsharp.Address() * 0x9E3779B97F4A7C15ull ^
                    (u64{fmt} << 40 | u64{width} << 20 | u64{height});
    const auto it = vr_bump.classes.find(key);
    if (it != vr_bump.classes.end() &&
        (vr_bump.frame < it->second.next || vr_bump.recheck_frame == vr_bump.frame)) {
        vr_bump.draw_need = it->second.slot < 0 ? it->second.need
                            : heights.Made(it->second.slot, vr_bump.frame) ? NeedDepth : NeedNone;
        return;
    }
    VrBump::Class c{};
    const bool bc1 = fmt == 35, bc3 = fmt == 37;
    const u32 block_bytes = bc1 ? 8 : 16;
    const u64 bytes = u64{(pitch + 3) / 4} * ((height + 3) / 4) * block_bytes;
    if ((bc1 || bc3) && width >= 16 && height >= 16 && tsharp.Address() != 0 &&
        memory->IsValidGpuMapping(tsharp.Address(), bytes)) {
        c = VrBump::Classify(std::bit_cast<const u8*>(tsharp.Address()), bytes, block_bytes,
                             bc3 ? 8u : 0u);
    }
    // A flat blue: a height from its normals, where they are stonework (bump_height.h; with the
    // setting on only; VR_BUMP_DERIVE=0, tests: none).
    static const bool derive = [] {
        const char* const env = std::getenv("VR_BUMP_DERIVE");
        return env == nullptr || env[0] != '0';
    }();
    s32 slot = -1;
    bool retry = false;
    if (c.kind == 1 && derive && vr_bump.depth_m > 0.0f) {
        const VideoCore::ImageInfo info{tsharp, desc};
        VideoCore::BumpHeight::Layout layout;
        if (VideoCore::BumpHeight::Store::Eligible(info, layout) &&
            memory->IsValidGpuMapping(info.guest_address, info.guest_size)) {
            slot = heights.Request(info, layout, std::bit_cast<const u8*>(info.guest_address),
                                   vr_bump.frame);
            retry = slot < 0;
        }
    }
    const u8 need = c.kind == 2 ? NeedDepth : NeedNone;
    vr_bump.draw_need = slot < 0 ? need
                        : heights.Made(slot, vr_bump.frame) ? NeedDepth : NeedNone;
    const bool again = it != vr_bump.classes.end();
    const bool changed = again && it->second.need != need;
    const u64 next = vr_bump.frame + (retry ? BumpRetryFrames
                                      : again ? BumpRecheckFrames : BumpFirstRecheckFrames);
    if (again) {
        vr_bump.recheck_frame = vr_bump.frame;
        it->second = {need, next, slot};
    } else {
        if (vr_bump.classes.size() >= BumpClassesMax) {
            vr_bump.classes.clear(); // bounded (a long session's every texture): seen again, read again
        }
        vr_bump.classes.emplace(key, VrBumpState::Entry{need, next, slot});
    }
    if constexpr (Common::Log::Enabled) {
        static u32 logged = 0;
        if (changed || (!again && (c.kind == 2 || logged < 64))) {
            ++logged;
            LOG_INFO(Render_Vulkan,
                     "VR bump class: shader {:#x} image {} {:#x} {}x{} format {}: {}{} (scale "
                     "{:.2f}; faces {:.2f}, joints {:.2f}, black {:.2f}, floor {:.2f})",
                     stage.pgm_hash, stage.vr_bump_image, tsharp.Address(), width, height, fmt,
                     VrBump::KindName(c.kind), changed ? " (changed)" : "", c.scale, c.face,
                     c.joint, c.black, c.floor);
        }
    }
}

// The frame's depth for the raised brickwork (parallax.h BumpGaps): the setting's, 0 when off;
// VR_BUMP_PARALLAX=<metres> (tests) and the live-tuning key lighting.brick_depth_cm override it.
// And the class cache's clock.
void Rasterizer::VrBumpEndFrame() {
    namespace Tune = Shader::Optimization::BumpGaps;
    static const float override_m = [] {
        const char* const env = std::getenv("VR_BUMP_PARALLAX");
        const float v = env != nullptr ? std::strtof(env, nullptr) : -1.0f;
        return std::isfinite(v) ? v : -1.0f;
    }();
    ++vr_bump.frame;
    float depth = VR::GetSettings().raised_bricks ? Tune::ParallaxDepthM : 0.0f;
    if (override_m >= 0.0f) {
        depth = override_m;
    }
    depth = 0.01f * VR::Tune::Get("lighting.brick_depth_cm", 100.0f * depth);
    vr_bump.depth_m = std::isfinite(depth) ? std::clamp(depth, 0.0f, Tune::MaxDepthM) : 0.0f;
}

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
    vr_push.SetCut(recording_vr.cut_code, recording_vr.free_eye_cut);
    // The raised brickwork's depth for this draw (vk_rasterizer.h VrBumpState: by its bump
    // map's class; a program that reads none keeps what the block holds).
    if (vr_bump.draw_need != NeedKeep) {
        vr_bump.pushed_depth = vr_bump.draw_need == NeedDepth ? vr_bump.depth_m : 0.0f;
    }
    vr_push.bump_depth = vr_bump.pushed_depth;
    if constexpr (Common::Log::Enabled) {
        // (Tests: each fragment program that draws with a depth, once: why it has one.)
        static std::unordered_set<u64> noted;
        const auto* frag = pipeline->GetStages()[u32(Shader::LogicalStage::Fragment)];
        if (vr_push.bump_depth > 0.0f && frag != nullptr && noted.size() < 512 &&
            noted.insert(frag->pgm_hash ^ (u64{vr_bump.draw_need} << 60) ^
                         (u64{recording_vr.cut_code != 0} << 62))
                .second) {
            LOG_INFO(Render_Vulkan,
                     "VR bump depth: shader {:#x} depth {:.3f} need {} (0 none, 1 depth, 2 kept) "
                     "image {} cut {}",
                     frag->pgm_hash, vr_push.bump_depth, vr_bump.draw_need, frag->vr_bump_image,
                     recording_vr.cut_code);
        }
    }
    // A motion variant reads the buffers it was selected with; every other draw pushes zeros.
    if (pipeline->HasVrMotion()) {
        vr_push.motion_params = vr_motion_buffers.params_address;
        vr_push.motion_positions = vr_motion_buffers.positions_address;
    }
}

// Layer draws: the draw about to be recorded, its pipeline and vr_hud final (DrawEyes records its
// right eye's copy). Both eyes as the pass and the pipeline decide: the pipeline's key.
void Rasterizer::NoteEyeDraws(const GraphicsPipeline* pipeline, float hud) {
    eye_draws = {.layout = pipeline->GetLayout(),
                 .hud = hud,
                 .both = pipeline->GetGraphicsKey().probe_view_mask == 3 &&
                         NativeStereo::LayerDraws()};
}

bool Rasterizer::VrMirrorFlip() const {
    return NativeStereo::MirrorFlip() && pipeline_cache.UsesVrPushData();
}

static_assert(Shader::Optimization::FogLight::BrightLights == 32, "vk_rasterizer.h VrFogState::bright");

// Over the dim line (fog_light.h FillLuminance), or a light the game spawned that was over it
// within BrightHoldFrames and is BrightKeepLuminance or more now: its peak decides, not each
// frame's flicker. The map's own lights take the plain line at no cost, and so does a spawned one
// at BrightTrackLuminance or more (far over the line: it never dips under, and remembering it
// would push the lanterns out of BrightLights, as the candles and lamp glows of Cathedral Ward
// did); one near the line takes one look through BrightLights (and follows it: a lantern walks).
bool Rasterizer::VrBrightLately(const float* l, const std::array<float, 12>& m, float lum) {
    namespace Fog = Shader::Optimization::FogLight;
    const bool now = lum >= Fog::FillLuminance;
    if (Fog::IsMapLight(l[7]) || lum < Fog::BrightKeepLuminance || lum >= Fog::BrightTrackLuminance) {
        return now;
    }
    std::array<float, 3> world;
    for (u32 r = 0; r < 3; ++r) {
        world[r] = m[4 * r] * l[0] + m[4 * r + 1] * l[1] + m[4 * r + 2] * l[2] + m[4 * r + 3];
    }
    const u64 frame = vr_fog.frame;
    VrFogState::BrightLight* same = nullptr;
    VrFogState::BrightLight* oldest = &vr_fog.bright[0];
    for (VrFogState::BrightLight& e : vr_fog.bright) {
        if (e.frame != 0 &&
            std::abs(e.reach_k - l[3]) <= Fog::BrightSameReach * std::max(e.reach_k, l[3])) {
            const float dx = e.world[0] - world[0], dy = e.world[1] - world[1],
                        dz = e.world[2] - world[2];
            if (dx * dx + dy * dy + dz * dz < Fog::SameM * Fog::SameM) {
                same = &e;
                break;
            }
        }
        if (e.frame < oldest->frame) {
            oldest = &e;
        }
    }
    if (now) {
        *(same != nullptr ? same : oldest) = {world, l[3], frame};
        return true;
    }
    if (same != nullptr && frame - same->frame <= Fog::BrightHoldFrames) {
        same->world = world;
        return true;
    }
    return false;
}

// The draw's screen is its screen scissor's size: a reflection's pass has one of its own (the sewer
// water's: half the eye images, 728x788 of 1456x1576).
void Rasterizer::VrFogCapture(u64 pgm_hash, const float* source, u32 size, bool guest,
                              const Shader::Optimization::FogLight::ListView& draw_view) {
    using NativeProjection::LightList;
    const LightList list = NativeProjection::LightListFor(pgm_hash, size);
    // A planar reflection's lights are in its mirrored camera's view. (A spot light's list: the
    // lights on the camera only; its first 16 dwords are a point light's.)
    if (list.kind == NativeProjection::LightKind::None || size > LightList::MaxBytes ||
        VrInMirrorPass()) {
        return;
    }
    namespace Fog = Shader::Optimization::FogLight;
    namespace LightTune = Shader::Optimization::LightShadow;
    std::scoped_lock lock{vr_fog.mutex};
    const auto* paired = draw_view.Resolve(vr_fog.view, vr_fog.view_frame, vr_fog.frame);
    if (paired == nullptr) {
        vr_scene.draw_slots_valid = false;
        return; // reflection, or no camera for this frame; a later valid list can still contribute
    }
    std::array<float, LightList::MaxBytes / sizeof(float)> lights;
    std::memcpy(lights.data(), guest ? ScratchDraw::MemorySource(source, size) : source, size);
    // This list's lights are in its own camera's view (its light208, bound just before it): a snap
    // turn can leave it 30 degrees from the AO pass's for a frame.
    const auto& list_view = *paired;
    // This draw's lights' map slots (light_shadow.h SceneMaps::DrawSlotsAt), for its lamp shader:
    // each light's world place against the slots' lamps (within 5 cm, reach within 2%).
    const bool scene_maps = VrSceneMapsOn();
    const bool shadow_mask = scene_maps && LightTune::SceneMaps::Mask();
    if (shadow_mask) {
        const VrShadowCpuScope timed{0};
        namespace Scene = LightTune::SceneMaps;
        vr_scene.draw_slots.fill(0u);
        const auto& m = list_view;
        for (u32 k = 0; k < std::min<u32>(list.count, Scene::DrawLights); ++k) {
            const float* l = lights.data() + list.Stride() * k;
            if (list.kind == NativeProjection::LightKind::Spot || !(l[3] > 0.0f)) {
                vr_scene.draw_slots[k / 4] |= 255u << (8 * (k % 4));
                continue;
            }
            float w[3];
            for (u32 r = 0; r < 3; ++r) {
                w[r] = m[4 * r] * l[0] + m[4 * r + 1] * l[1] + m[4 * r + 2] * l[2] + m[4 * r + 3];
            }
            u32 best = 255u; // (checked: none)
            float best_d2 = Scene::SlotKeepM * Scene::SlotKeepM;
            for (u32 j = 0; j < vr_scene.count; ++j) {
                const auto& lamp = vr_scene.chosen[j];
                const float dx = lamp.world[0] - w[0], dy = lamp.world[1] - w[1], dz = lamp.world[2] - w[2];
                const float d2 = dx * dx + dy * dy + dz * dz;
                // (Its reach within KeepReachShare, as the slot's own keep: a candle or a fire
                // pulses its reach a few percent, and at 2% its shadow went out those frames.)
                if ((vr_scene.flags[j] & Scene::SlotUsed) && d2 < best_d2 &&
                    std::abs(lamp.k - l[3]) <= Scene::KeepReachShare * lamp.k) {
                    best = j + 1u;
                    best_d2 = d2;
                }
            }
            vr_scene.draw_slots[k / 4] |= best << (8 * (k % 4));
        }
        vr_scene.draw_slots_valid = true;
        vr_scene.draw_slots_pgm = pgm_hash;
    }
    // (Diagnostics, VR_LIGHT_AUDIT=<file>: every 3rd frame's lists, each light where it is in the
    // camera's view and in the world.)
    if (static const char* const audit_path = std::getenv("VR_LIGHT_AUDIT"); audit_path) {
        static std::FILE* audit = std::fopen(audit_path, "w");
        static u32 audit_lines = 0;
        if (audit && vr_fog.view_frame % 3 == 0 && audit_lines < 400000) {
            const auto& m = list_view;
            std::fprintf(audit, "%llu %s %llx cam %.3f %.3f %.3f fwd %.3f %.3f %.3f",
                         static_cast<unsigned long long>(vr_fog.view_frame),
                         list.kind == NativeProjection::LightKind::Spot         ? "spot"
                         : list.kind == NativeProjection::LightKind::CubeShadow ? "cube"
                                                                                : "point",
                         static_cast<unsigned long long>(pgm_hash), m[3], m[7], m[11], m[2], m[6],
                         m[10]);
            for (u32 k = 0; k < list.count; ++k) {
                const float* l = lights.data() + list.Stride() * k;
                float w[3];
                for (u32 r = 0; r < 3; ++r) {
                    w[r] = m[4 * r] * l[0] + m[4 * r + 1] * l[1] + m[4 * r + 2] * l[2] + m[4 * r + 3];
                }
                std::fprintf(audit, " | v %.3f %.3f %.3f w %.3f %.3f %.3f k %.5f rgb %.3f %.3f %.3f t %.3f",
                             l[0], l[1], l[2], w[0], w[1], w[2], l[3], l[4], l[5], l[6], l[7]);
                // (The rest of the record, as bits: what else tells lights apart.)
                std::fputs(" x", audit);
                for (u32 d = 8; d < list.Stride(); ++d) {
                    std::fprintf(audit, " %08x", std::bit_cast<u32>(l[d]));
                }
            }
            std::fputc('\n', audit);
            if (++audit_lines % 64 == 0) {
                std::fflush(audit);
            }
        }
    }
    // (The game's own shadowed lamps: their first 16 dwords are a point light's; they take part
    // like one.)
    // The lights the game keeps on the camera (light_shadow.h AttachedFromM): every light more
    // than AttachedFromM from the eye is a candidate where it is seen (its place in the camera's
    // view, its reach and colour); a candidate seen again at that place (AttachedSameM, the same
    // reach and colour within 2%) on AttachedSightings frames once the camera has moved
    // AttachedMoveM or turned AttachedTurn since it was first seen there is one. (The game lists
    // such a light only where its small reach touches the picture: a few frames in many.) A camera
    // that has not moved decides nothing; a world-fixed light moved with the camera cannot match.
    auto& att = vr_attached;
    if (att.frame != vr_fog.view_frame) {
        att.frame = vr_fog.view_frame;
        att.view = vr_fog.view;
        att.no_cast_count = 0;
        // Every 60 frames: the cells not seen for AttachedForgetFrames go (the world's lamps
        // leave one at each place they pass through while the camera moves).
        if (att.frame - att.pruned >= 60) {
            att.pruned = att.frame;
            std::erase_if(att.candidates, [&](const auto& kv) {
                return att.frame - kv.second.last_frame > LightTune::AttachedForgetFrames;
            });
            att.confirmed_dirty = true;
        }
    }
    const auto moved_since = [&](const std::array<float, 12>& then) {
        float move = 0.0f, turn = 0.0f;
        for (u32 r = 0; r < 3; ++r) {
            const float d = att.view[4 * r + 3] - then[4 * r + 3];
            move += d * d;
            for (u32 c = 0; c < 3; ++c) {
                turn = std::max(turn, std::abs(att.view[4 * r + c] - then[4 * r + c]));
            }
        }
        return move >= LightTune::AttachedMoveM * LightTune::AttachedMoveM ||
               turn >= LightTune::AttachedTurn;
    };
    const auto same_light = [](const VrAttached::Candidate& c, const float* l) {
        const float dx = l[0] - c.pos[0], dy = l[1] - c.pos[1], dz = l[2] - c.pos[2];
        const auto close = [](float x, float y) {
            return std::abs(x - y) <= 0.02f * std::max(std::abs(x), std::abs(y)) + 1e-6f;
        };
        return dx * dx + dy * dy + dz * dz < LightTune::AttachedSameM * LightTune::AttachedSameM &&
               close(l[3], c.reach) && close(l[4], c.colour[0]) && close(l[5], c.colour[1]) &&
               close(l[6], c.colour[2]);
    };
    const bool collect_shadow_lamps = scene_maps && list.kind != NativeProjection::LightKind::Spot;
    const u32 shadow_tier = collect_shadow_lamps && shadow_mask ? VrLightShadowTier() : 0u;
    for (u32 k = 0; k < list.count; ++k) {
        const float* l = lights.data() + list.Stride() * k;
        if (!std::all_of(l, l + 7, [](float v) { return std::isfinite(v); }) || !(l[3] > 0.0f) ||
            !(l[4] + l[5] + l[6] > 0.0f)) {
            continue;
        }
        // Over the dim line, or a spawned light bright lately (the glow's and the grey and blue
        // lights' shadows' test: VrBrightLately).
        const bool bright = VrBrightLately(l, list_view, 0.3f * l[4] + 0.6f * l[5] + 0.1f * l[6]);
        // The lights that cast no shadow (light_shadow.h NoCastLights): this frame's place.
        {
            const auto& m = list_view;
            float w[3];
            for (u32 r = 0; r < 3; ++r) {
                w[r] = m[4 * r] * l[0] + m[4 * r + 1] * l[1] + m[4 * r + 2] * l[2] + m[4 * r + 3];
            }
            // The scene maps' candidates (light_shadow.h SceneMaps): a lamp our rules let cast,
            // within the preset's range, scored by the share of the view its light covers.
            if (collect_shadow_lamps) {
                const VrShadowCpuScope timed{1};
                const float dist = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
                const float reach = 1.0f / std::sqrt(l[3]);
                const float lum = 0.3f * l[4] + 0.6f * l[5] + 0.1f * l[6];
                const bool warm = l[4] > LightTune::WarmShare * l[6];
                const bool casts =
                    list.kind == NativeProjection::LightKind::CubeShadow ||
                    (warm && lum >= LightTune::DimMinLuminance && reach <= LightTune::WarmReachM) ||
                    (!warm && reach <= LightTune::ShadowFillReachM && bright);
                const auto near_pick = [&](const LightTune::NoCastLight& pick) {
                    const float dx = w[0] - pick.x, dy = w[1] - pick.y, dz = w[2] - pick.z;
                    return dx * dx + dy * dy + dz * dz <= LightTune::NoCastSameM * LightTune::NoCastSameM &&
                           std::abs(l[3] - pick.k) <= LightTune::NoCastShare * pick.k + 1e-4f;
                };
                att.RefreshConfirmed();
                const bool kept_on_camera = std::ranges::any_of(att.confirmed, [&](const auto& c) {
                    return same_light(c, l);
                });
                // (Held lights too with the mask: nothing else gives them shadows now.)
                const bool held_ok = dist > LightTune::HeldOutM || shadow_mask;
                namespace SceneTune = LightTune::SceneMaps;
                const float near_m = shadow_mask ? SceneTune::MaskedLightRangeM(reach, shadow_tier)
                                                 : LightTune::NearLightM;
                const float fade_m = shadow_mask ? near_m * SceneTune::RangeFadeShare
                                                 : LightTune::NearFadeM;
                // Listed past its range while a slot holds it (a live switch to a shorter tier,
                // a warp): VrEndFrame gives that lingering slot no shadow, as the fade does at
                // the range, instead of its last strength until the slot ages out.
                if (shadow_mask && dist > near_m && k < SceneTune::DrawLights) {
                    const u32 slot = (vr_scene.draw_slots[k / 4] >> (8 * (k % 4))) & 255u;
                    if (slot >= 1u && slot <= VrScene::SlotCount) {
                        vr_scene.out_of_range |= 1u << (slot - 1u);
                    }
                }
                if (casts && dist <= near_m && held_ok &&
                    std::ranges::none_of(LightTune::NoCastLights, near_pick) && !kept_on_camera &&
                    std::ranges::none_of(vr_scene.building, [&](const VrScene::Lamp& o) {
                        return std::abs(o.world[0] - w[0]) + std::abs(o.world[1] - w[1]) +
                                   std::abs(o.world[2] - w[2]) < 0.02f;
                    })) {
                    const float at = std::max(dist, 0.5f);
                    // Its shadow's strength: the lamp shaders' rule (a game-shadowed lamp in full; a
                    // warm one in over DimFadeLuminance; a grey or blue one over BrightFadeLuminance),
                    // fading in over the outer part of its range.
                    const auto ramp = [](float x, float from, float to) {
                        return std::clamp((x - from) / (to - from), 0.0f, 1.0f);
                    };
                    const bool warm_casts = warm && lum >= LightTune::DimMinLuminance &&
                                            reach <= LightTune::WarmReachM;
                    const float strength =
                        list.kind == NativeProjection::LightKind::CubeShadow ? 1.0f
                        : warm_casts ? ramp(lum, LightTune::DimMinLuminance,
                                            LightTune::DimMinLuminance + LightTune::DimFadeLuminance)
                        : warm ? 0.0f
                        : !Fog::IsMapLight(l[7])
                            ? ramp(lum, Fog::BrightKeepLuminance - LightTune::SpawnedFadeLuminance,
                                   Fog::BrightKeepLuminance)
                            : ramp(lum, Fog::FillLuminance - LightTune::BrightFadeLuminance, Fog::FillLuminance);
                    const float fade = std::clamp((near_m - dist) / fade_m, 0.0f, 1.0f);
                    // Its clearance: the holder's hand (ClearM) within arm's reach, blending out to
                    // its fixture's (by its reach) from HeldInM to HeldOutM.
                    const float fixture =
                        LightTune::ReachOld()
                            ? std::max(LightTune::ClearFixtureM, reach * LightTune::ClearPerRadius)
                            : LightTune::FixtureClearM(reach);
                    const float t = std::clamp((dist - LightTune::HeldInM) / (LightTune::HeldOutM - LightTune::HeldInM),
                                               0.0f, 1.0f);
                    const float held_out = t * t * (3.0f - 2.0f * t);
                    const float clear = LightTune::ClearM + (fixture - LightTune::ClearM) * held_out;
                    // (A held light first: the one you carry lights everything near you.)
                    // ... times its shadow's strength (a lamp fading out at 30..40 m, or a dim one,
                    // never takes a slot from a candle near you: Oedon had 18 lamps for 16 slots, 4
                    // of them past 30 m, and the nearest swapped in and out at the cut).
                    const float score = held_out < 1.0f ? 2.0f
                                                        : std::min(1.0f, reach * reach / (at * at)) *
                                                              std::max(strength * fade, 0.01f);
                    vr_scene.building.push_back({{w[0], w[1], w[2]}, l[3], score, strength * fade, clear,
                                                 held_out < 1.0f, dist});
                }
            }
            const auto close = [](float x, float want) {
                return std::abs(x - want) <= LightTune::NoCastShare * std::abs(want) + 1e-4f;
            };
            for (const auto& pick : LightTune::NoCastLights) {
                const float dx = w[0] - pick.x, dy = w[1] - pick.y, dz = w[2] - pick.z;
                if (dx * dx + dy * dy + dz * dz > LightTune::NoCastSameM * LightTune::NoCastSameM ||
                    !close(l[3], pick.k) || !close(l[4], pick.r) || !close(l[5], pick.g) ||
                    !close(l[6], pick.b)) {
                    continue;
                }
                const bool known = std::any_of(
                    att.no_cast.begin(), att.no_cast.begin() + att.no_cast_count, [&](const auto& n) {
                        return std::abs(n[0] - l[0]) + std::abs(n[1] - l[1]) + std::abs(n[2] - l[2]) <
                               1e-3f;
                    });
                if (!known && att.no_cast_count < att.no_cast.size()) {
                    att.no_cast[att.no_cast_count++] = {l[0], l[1], l[2], l[3]};
                }
            }
        }
        // (The lights on the camera: beyond arm's reach only.)
        const float eye2 = l[0] * l[0] + l[1] * l[1] + l[2] * l[2];
        bool on_camera_list = false;
        if (eye2 > LightTune::AttachedFromM * LightTune::AttachedFromM) {
            // Its cell: the place in AttachedSameM steps; a light on a cell's border (the one on
            // the camera sits at 2.02 and 4.12 m: rounding moves it across) is looked for in the
            // neighbouring cells too.
            const auto cell_of = [](float v) { return s32(std::floor(v / LightTune::AttachedSameM)); };
            const auto key_of = [](s32 x, s32 y, s32 z) {
                return u64(u32(x) & 0x1FFFFFu) | (u64(u32(y) & 0x1FFFFFu) << 21) |
                       (u64(u32(z) & 0x1FFFFFu) << 42);
            };
            const s32 cx = cell_of(l[0]), cy = cell_of(l[1]), cz = cell_of(l[2]);
            const u64 key = key_of(cx, cy, cz);
            auto it = att.candidates.find(key);
            for (s32 dx = -1; dx <= 1 && (it == att.candidates.end() || !same_light(it->second, l)); ++dx) {
                for (s32 dy = -1; dy <= 1 && (it == att.candidates.end() || !same_light(it->second, l)); ++dy) {
                    for (s32 dz = -1; dz <= 1; ++dz) {
                        const auto near_it = att.candidates.find(key_of(cx + dx, cy + dy, cz + dz));
                        if (near_it != att.candidates.end() && same_light(near_it->second, l)) {
                            it = near_it;
                            break;
                        }
                    }
                }
            }
            if (it != att.candidates.end() && !same_light(it->second, l)) {
                // Another light in the cell (its reach or colour): the cell takes the newest.
                att.confirmed_dirty |= it->second.attached;
                it->second = {{l[0], l[1], l[2]}, l[3], {l[4], l[5], l[6]}, att.view, att.frame, 1u, false};
                it = att.candidates.end();
            } else if (it != att.candidates.end()) {
                auto* const c = &it->second;
                if (c->last_frame != att.frame) {
                    c->last_frame = att.frame;
                    ++c->sightings;
                    if (!c->attached && c->sightings >= LightTune::AttachedSightings &&
                        moved_since(c->first_view)) {
                        c->attached = true;
                        att.confirmed_dirty = true;
                        if constexpr (Common::Log::Enabled) {
                            static u32 logged = 0;
                            if (logged++ < 8) {
                                LOG_INFO(Render_Vulkan,
                                         "VR lamp shadows: a light kept on the camera at "
                                         "({:.2f} {:.2f} {:.2f}), reach k {:.4f}, rgb {:.2f} {:.2f} "
                                         "{:.2f} ({} sightings): it casts no shadow",
                                         l[0], l[1], l[2], l[3], l[4], l[5], l[6], c->sightings);
                            }
                        }
                    }
                }
                on_camera_list = c->attached;
            } else if (att.candidates.size() < LightTune::AttachedCandidates) {
                att.candidates.emplace(key, VrAttached::Candidate{{l[0], l[1], l[2]}, l[3],
                                                                  {l[4], l[5], l[6]}, att.view,
                                                                  att.frame, 1u, false});
            }
        }
        // (A spot light: the rest is the point lights' and the game's shadowed lamps'.)
        if (list.kind == NativeProjection::LightKind::Spot) {
            continue;
        }
        // Removed (vr.ini camera_fill_light off): no light in the air either.
        if (on_camera_list && !VrCameraFillKept()) {
            continue;
        }
        // His body maps' lamps (vr_body_maps.h): every light bright enough to shadow, once; the
        // MaxLights nearest the camera (a street lists a hundred and more, the torch in his hand
        // anywhere among them). A save lamp's glow as VR_LAMP_GLOW makes it (core/vr/lamp_glow.h).
        const float glow =
            VR::LampGlow::Boost() > 1.0f &&
                    VR::LampGlow::IsGlow(l[4], l[5], l[6],
                                         std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]))
                ? VR::LampGlow::Boost()
                : 1.0f;
        // The lamp shadows' rule (light_shadow.h WarmShare): a warm light from DimMinLuminance,
        // any other from FillLuminance and never the camera's (the game's grey light: a torch
        // held that close is warm, and casts).
        const float cast_lum = glow * (0.3f * l[4] + 0.6f * l[5] + 0.1f * l[6]);
        const bool warm_light = l[4] > Shader::Optimization::LightShadow::WarmShare * l[6];
        const bool on_camera = l[0] * l[0] + l[1] * l[1] + l[2] * l[2] <
                               Shader::Optimization::LightShadow::HeldInM *
                                   Shader::Optimization::LightShadow::HeldInM;
        const bool cast_bright = bright || cast_lum >= Fog::FillLuminance;
        if (!on_camera_list &&
            (warm_light ? cast_lum >= Shader::Optimization::LightShadow::DimMinLuminance
                        : !on_camera && cast_bright)) {
            const float d2 = l[0] * l[0] + l[1] * l[1] + l[2] * l[2];
            const auto& m = list_view;
            std::array<float, 3> world;
            for (u32 r = 0; r < 3; ++r) {
                world[r] = m[4 * r] * l[0] + m[4 * r + 1] * l[1] + m[4 * r + 2] * l[2] +
                           m[4 * r + 3];
            }
            if (std::ranges::none_of(vr_body.building, [&](const VrBody::Light& b) {
                    return b.world == world && b.reach == l[3];
                })) {
                if (vr_body.building.size() < VrBody::MaxLights) {
                    vr_body.building.push_back(
                        {world, l[3], {glow * l[4], glow * l[5], glow * l[6]}, d2, cast_bright});
                } else {
                    auto farthest = std::ranges::max_element(
                        vr_body.building, {}, &VrBody::Light::camera2);
                    if (farthest->camera2 > d2) {
                        *farthest = {world, l[3], {glow * l[4], glow * l[5], glow * l[6]}, d2,
                                     cast_bright};
                    }
                }
            }
        }
        if (vr_fog.building.size() >= Fog::CaptureLights || Fog::IsWideLight(l[3]) || !bright) {
            // The torch in his hand (fog_grid.h Lamps::Held*): a warm, bright light the game spawned
            // beside the eye, too wide for the air's list. Kept for the fog grid alone.
            {
                namespace Lamps = Shader::Optimization::FogGrid::Lamps;
                const float lum = 0.3f * l[4] + 0.6f * l[5] + 0.1f * l[6];
                if (!Fog::IsMapLight(l[7]) && l[3] > 0.0f && lum >= Lamps::HeldLuminance &&
                    l[4] > LightTune::WarmShare * l[6] &&
                    l[0] * l[0] + l[1] * l[1] + l[2] * l[2] < Lamps::HeldNearM * Lamps::HeldNearM) {
                    const auto& m = list_view;
                    for (u32 r = 0; r < 3; ++r) {
                        vr_fog.held_world[r] = m[4 * r] * l[0] + m[4 * r + 1] * l[1] + m[4 * r + 2] * l[2] + m[4 * r + 3];
                    }
                    vr_fog.held_reach = l[3];
                    vr_fog.held_colour = {l[4], l[5], l[6]};
                    vr_fog.held_frame = vr_fog.frame;
                }
            }
            if constexpr (Common::Log::Enabled) {
                // (Tests, VR_AIR_DIAG=1: a light the game spawned beside the eye that counts as a
                // fill and lights no air: the torch in his hand?)
                static const bool diag = [] {
                    const char* const env = std::getenv("VR_AIR_DIAG");
                    return env != nullptr && env[0] == '1';
                }();
                static u32 seen = 0;
                if (diag && !Shader::Optimization::FogLight::IsMapLight(l[7]) &&
                    l[0] * l[0] + l[1] * l[1] + l[2] * l[2] < 4.0f && seen++ % 600 == 0 && seen < 12000) {
                    LOG_INFO(Render_Vulkan,
                             "VR fog light: left out near the eye: at ({:.2f} {:.2f} {:.2f}) r {:.2f} m colour "
                             "({:.2f} {:.2f} {:.2f}) word {:.1f}, {} captured",
                             l[0], l[1], l[2], l[3] > 0.0f ? 1.0f / std::sqrt(l[3]) : -1.0f, l[4], l[5], l[6], l[7],
                             vr_fog.building.size());
                }
            }
            continue; // a fill light lights no air (and the frame keeps CaptureLights)
        }
        // A light lights several tiles: one list per tile it reaches. Its bits' hash in an open
        // table (256 slots for at most CaptureLights: never full; 0 is an empty slot).
        u64 key = 0xcbf29ce484222325ull;
        for (u32 c = 0; c < 4; ++c) {
            key = (key ^ std::bit_cast<u32>(l[c])) * 0x100000001b3ull;
        }
        key |= 1;
        auto& keys = vr_fog.building_keys;
        u32 slot = u32(key ^ (key >> 29)) & (keys.size() - 1);
        bool known = false;
        while (keys[slot] != 0) {
            if (keys[slot] == key) {
                known = true;
                break;
            }
            slot = (slot + 1) & (keys.size() - 1);
        }
        if (!known) {
            keys[slot] = key;
            VrFogLight f{{l[0], l[1], l[2]}, l[3], {l[4], l[5], l[6]}};
            // Where it is in the world: this frame's light pass camera (VrFogCaptureView).
            const auto& m = list_view;
            for (u32 r = 0; r < 3; ++r) {
                f.world[r] = m[4 * r] * l[0] + m[4 * r + 1] * l[1] + m[4 * r + 2] * l[2] +
                             m[4 * r + 3];
            }
            f.map = Shader::Optimization::FogLight::IsMapLight(l[7]);
            vr_fog.building.push_back(f);
        }
    }
}

// The frame's camera from the scene's own camera block (864 bytes: its world-to-view rows 8..19),
// at the frame's first draw that binds one. 2026-11-15, the user in the headset: lamp shadows
// "move around when I rotate my head, if I translate they are steady". The pass that traces them
// (the AO pass, the shadow mask's) comes before the frame's first light pass, binds no light208,
// and so put the lamps on the camera of the frame BEFORE (160 of 160 uploads measured): every
// traced shadow turned by what the head turned in a frame. The light pass's own camera, when it
// comes, is the same one (the views-apart count says when it is not).
void Rasterizer::VrFogCaptureSceneView(const float* c) {
    // A perspective camera of the scene's own size (not a light's, not a lamp's cube face).
    if (!(c[66] == 1.f && c[67] == 0.f && c[4] > 600.f && c[5] > 300.f)) {
        return;
    }
    const float* const r = c + 8;
    const float det = r[0] * (r[5] * r[10] - r[6] * r[9]) - r[1] * (r[4] * r[10] - r[6] * r[8]) +
                      r[2] * (r[4] * r[9] - r[5] * r[8]);
    if (!(det > 0.9f && det < 1.1f)) {
        return; // a mirror's, or no rigid view
    }
    std::array<float, 12> view;
    for (u32 i = 0; i < 3; ++i) {
        for (u32 j = 0; j < 3; ++j) {
            view[4 * i + j] = r[4 * j + i];
        }
        view[4 * i + 3] = -(r[i] * r[3] + r[4 + i] * r[7] + r[8 + i] * r[11]);
    }
    for (const float v : view) {
        if (!std::isfinite(v)) {
            return;
        }
    }
    // A reflection's camera (FogLight::IsFramesView): a proper camera too (determinant 1: the test
    // above passes it), drawn before the scene's own, so it was the frame's first scene camera and
    // became the frame's camera with water_reflections on; the pass tracker (VrInMirrorPass) learns
    // of the reflection's pass from this very draw's camera, after the caller asked it. Its place
    // is not the frame's render camera's.
    if (const VR::CameraRecord rec = VR::DrawFrameRecord();
        !Shader::Optimization::FogLight::MirrorViewsByPassOnly() && rec.id != 0 && rec.has_render_position &&
        !Shader::Optimization::FogLight::IsFramesView(view.data(), rec.render_position.x, rec.render_position.y,
                                                      rec.render_position.z)) {
        return;
    }
    std::scoped_lock lock{vr_fog.mutex};
    if (vr_fog.view_frame == vr_fog.frame) {
        return;
    }
    vr_fog.view = view;
    vr_fog.view_frame = vr_fog.frame;
}

void Rasterizer::VrFogCaptureView(const float* source, u32 size, bool guest, bool frame_candidate,
                                  Shader::Optimization::FogLight::ListView& draw_view) {
    if (VrInMirrorPass()) {
        draw_view.Reject();
        return;
    }
    std::array<float, 16> view;
    std::memcpy(view.data(),
                (guest ? static_cast<const float*>(ScratchDraw::MemorySource(source, size))
                       : source) + 32,
                sizeof(view));
    if (!NativeProjection::AffineView(view.data())) {
        return; // not a camera's view to world (its rotation, then 0 0 0 1)
    }
    // A reflection's camera by its place (FogLight::IsFramesView), whatever the pass tracker said
    // above: never the frame's camera, and its lists never the frame's lights.
    const VR::CameraRecord rec = VR::DrawFrameRecord();
    const bool mirror =
        !Shader::Optimization::FogLight::MirrorViewsByPassOnly() && rec.id != 0 && rec.has_render_position &&
        !Shader::Optimization::FogLight::IsFramesView(view.data(), rec.render_position.x, rec.render_position.y,
                                                      rec.render_position.z);
    if (mirror) {
        draw_view.Reject();
        return;
    }
    draw_view.Set(view.data());
    if (!frame_candidate) {
        return; // spot/cube light208: paired with its list, never replaces the frame camera
    }
    std::scoped_lock lock{vr_fog.mutex};
    if constexpr (Common::Log::Enabled) {
        std::copy_n(view.begin(), 12, vr_fog.last_list_view.begin());
    }
    if (vr_fog.view_frame == vr_fog.frame) {
        if constexpr (Common::Log::Enabled) {
            float apart = 0.0f;
            for (u32 i = 0; i < 12; ++i) {
                apart = std::max(apart, std::abs(view[i] - vr_fog.view[i]) / (i % 4 == 3 ? 1.0f : 0.02f));
            }
            if (apart > 1.0f && vr_fog.views_apart_frame != vr_fog.frame) {
                vr_fog.views_apart_frame = vr_fog.frame;
                ++vr_fog.views_apart;
            }
        }
        return; // the frame's first: the AO pass's, the pixels' camera
    }
    std::copy_n(view.begin(), 12, vr_fog.view.begin());
    vr_fog.view_frame = vr_fog.frame;
}

// His body maps (vr_body_maps.h), at the frame's first lamp draw (a lamp program declares the
// maps' buffer), outside its render pass: the lamps whose light reaches him (the last light pass's
// list, in the world: a frame late, as the lamps' own lights are drawn), the strongest MaxMaps,
// each a frame aimed at him.
void Rasterizer::VrBodyMapsBeforeDraw(const GraphicsPipeline* pipeline) {
    namespace Body = Shader::Optimization::BodyShadow;
    namespace Fog = Shader::Optimization::FogLight;
    namespace LightTune = Shader::Optimization::LightShadow;
    // (Its fragment stage's buffers: a bit worked out once a pipeline, not a scan every draw.)
    if (!(pipeline->VrFsBuffers() & Pipeline::VrFsBodyMaps) || !Body::Enabled() ||
        !Body::MapsEnabled() || !vr_body_maps || !VrLampShadowsOn() || !VR::IsEnabled() || VrInMirrorPass()) {
        return;
    }
    const VR::CameraRecord rec = VR::DrawFrameRecord();
    if (rec.id == 0 || vr_body.maps_frame == rec.id) {
        return;
    }
    vr_body.maps_frame = rec.id;
    // Why a frame has no maps (the first few, then one in 600).
    static u32 why_count = 0;
    const auto why = [&](const char* reason, size_t n) {
        if constexpr (Common::Log::Enabled) {
            if (why_count < 8 || why_count % 600 == 0) {
                LOG_INFO(Render_Vulkan, "VR body maps: frame {}: none ({}, {})", rec.id, reason, n);
            }
            ++why_count;
        }
    };
    if (!vr_body_maps->Prepare() || !rec.has_player_position) {
        why("not ready or no player", 0);
        return;
    }
    std::array<VR::Vec3, VR::PlayerBoneCount> bones{};
    if (!VR::RecordPlayerBones(rec.id, bones)) {
        why("no bones", 0);
        return;
    }
    const double units = rec.world_scale > 0.0f ? rec.world_scale : 1.0;
    // His sphere (relative to his origin): his bones' box, a coat and a hat around them.
    std::array<double, 3> lo{1e30, 1e30, 1e30}, hi{-1e30, -1e30, -1e30};
    for (const auto& bone : bones) {
        const double p[3] = {bone.x, bone.y, bone.z};
        for (u32 c = 0; c < 3; ++c) {
            lo[c] = std::min(lo[c], p[c]);
            hi[c] = std::max(hi[c], p[c]);
        }
    }
    std::array<double, 3> centre{};
    double radius = 0.0;
    for (u32 c = 0; c < 3; ++c) {
        centre[c] = 0.5 * (lo[c] + hi[c]);
        radius += (hi[c] - lo[c]) * (hi[c] - lo[c]);
    }
    radius = 0.5 * std::sqrt(radius) + 0.35 * units;
    if (!std::isfinite(radius) || radius > 3.0 * units) {
        why("body too big", 0);
        return;
    }
    struct Candidate {
        std::array<double, 3> at{};
        double weight{};
    };
    std::vector<Candidate> candidates;
    // The nearest listed light (logs): its distance from his middle, reach, brightness.
    double near_d = 1e30, near_reach = 0.0, near_lum = 0.0;
    {
        // The last frame's lights (VrEndFrame keeps them, in the world): a frame late.
        std::scoped_lock lock{vr_fog.mutex};
        for (const auto& f : vr_body.lights) {
            const double lum = 0.3 * f.colour[0] + 0.6 * f.colour[1] + 0.1 * f.colour[2];
            const double reach = f.reach > 0.0f ? 1.0 / std::sqrt(double(f.reach)) : 0.0;
            const std::array<double, 3> at{f.world[0] - rec.player_position.x,
                                           f.world[1] - rec.player_position.y,
                                           f.world[2] - rec.player_position.z};
            const double dx = at[0] - centre[0], dy = at[1] - centre[1], dz = at[2] - centre[2];
            const double d2 = dx * dx + dy * dy + dz * dz;
            if (std::sqrt(d2) < near_d) {
                near_d = std::sqrt(d2);
                near_reach = reach;
                near_lum = lum;
            }
            // The lamp shadows' own rule (light_shadow.h WarmShare): a warm light from
            // DimMinLuminance and WarmReachM (a dim one when he is within DimNearShare of its
            // reach), any other bright and narrow.
            const bool warm = f.colour[0] > LightTune::WarmShare * f.colour[2];
            const bool bright = f.bright || lum >= Fog::FillLuminance;
            const bool casts =
                reach > 0.0 &&
                (warm ? lum >= LightTune::DimMinLuminance && reach <= LightTune::WarmReachM * units
                      : bright && reach <= LightTune::ShadowFillReachM * units);
            if (!casts) {
                continue;
            }
            // (The share of its reach a dim one's shadows cover: light_shadow.h DimShareLuminance.)
            const double share =
                LightTune::DimNearShare +
                (1.0 - LightTune::DimNearShare) *
                    std::clamp((lum - LightTune::DimShareLuminance) /
                                   (Fog::FillLuminance - LightTune::DimShareLuminance),
                               0.0, 1.0);
            const double reaches = share * reach;
            if (d2 > (reaches + radius) * (reaches + radius)) {
                continue;
            }
            const double weight = lum * std::max(0.05, 1.0 - d2 / (reach * reach));
            bool same = false;
            for (Candidate& c : candidates) {
                const double ex = c.at[0] - at[0], ey = c.at[1] - at[1], ez = c.at[2] - at[2];
                if (ex * ex + ey * ey + ez * ez < 0.09 * units * units) {
                    c.weight = std::max(c.weight, weight);
                    same = true;
                    break;
                }
            }
            if (!same) {
                candidates.push_back({at, weight});
            }
        }
    }
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
        return a.weight > b.weight;
    });
    // The lamps his shadow comes from (body_shadow.h BodyLampsAt): the strongest three at him.
    vr_body.top_count = 0;
    for (const Candidate& c : candidates) {
        if (vr_body.top_count == vr_body.top.size()) {
            break;
        }
        vr_body.top[vr_body.top_count++] = {c.at[0] + rec.player_position.x,
                                            c.at[1] + rec.player_position.y,
                                            c.at[2] + rec.player_position.z};
    }
    vr_body.top_frame = rec.id;
    std::array<VrBodyMaps::Lamp, VrBodyMaps::MaxMaps> lamps{};
    u32 count = 0;
    for (const Candidate& c : candidates) {
        if (count == VrBodyMaps::MaxMaps) {
            break;
        }
        std::array<double, 3> z{centre[0] - c.at[0], centre[1] - c.at[1], centre[2] - c.at[2]};
        const double d = std::sqrt(z[0] * z[0] + z[1] * z[1] + z[2] * z[2]);
        if (!(d > 0.01 * units)) {
            continue;
        }
        for (double& v : z) {
            v /= d;
        }
        std::array<double, 3> up{0.0, 1.0, 0.0};
        if (std::abs(z[1]) > 0.95) {
            up = {1.0, 0.0, 0.0};
        }
        std::array<double, 3> x{up[1] * z[2] - up[2] * z[1], up[2] * z[0] - up[0] * z[2],
                                up[0] * z[1] - up[1] * z[0]};
        const double xl = std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        for (double& v : x) {
            v /= xl;
        }
        const std::array<double, 3> y{z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2],
                                      z[0] * x[1] - z[1] * x[0]};
        // The frustum fitted to his bones as the lamp sees them (each with a margin for the coat
        // and the hat round it): his whole body on the map, as many texels as it can have.
        // Inside him (the torch in his hand) his holding hand is left out by the near plane.
        constexpr double pi = 3.14159265358979;
        constexpr double widest = 80.0 * pi / 180.0, narrowest = 5.0 * pi / 180.0;
        const bool outside = d > radius;
        double half = narrowest;
        for (const auto& bone : bones) {
            const double v[3] = {bone.x - c.at[0], bone.y - c.at[1], bone.z - c.at[2]};
            const double len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (len < Body::CarryM * units) {
                continue; // the hand that holds it
            }
            const double cosine = std::clamp((v[0] * z[0] + v[1] * z[1] + v[2] * z[2]) / len,
                                             -1.0, 1.0);
            half = std::max(half, std::acos(cosine) + std::atan(0.35 * units / len));
        }
        half = std::min(half, widest);
        VrBodyMaps::Lamp& lamp = lamps[count++];
        lamp.origin = c.at;
        lamp.axes = {x[0], x[1], x[2], y[0], y[1], y[2], z[0], z[1], z[2]};
        lamp.f = float(1.0 / std::tan(half));
        lamp.near_z = float(outside ? std::max(d - radius, 0.05 * units) : Body::CarryM * units);
    }
    if (count == 0) {
        size_t listed = 0;
        {
            std::scoped_lock lock{vr_fog.mutex};
            listed = vr_body.lights.size();
        }
        if constexpr (Common::Log::Enabled) {
            if (why_count < 8 || why_count % 600 == 0) {
                LOG_INFO(Render_Vulkan,
                         "VR body maps: the nearest light {:.2f} from his middle (reach {:.2f}, "
                         "lum {:.2f}); his middle ({:.2f} {:.2f} {:.2f}) r {:.2f}, units {:.3f}, "
                         "origin ({:.2f} {:.2f} {:.2f})",
                         near_d, near_reach, near_lum, centre[0], centre[1], centre[2], radius,
                         units, rec.player_position.x, rec.player_position.y,
                         rec.player_position.z);
            }
        }
        why("no lamp reaches him; lights listed", listed);
        return;
    }
    scheduler.EndRendering();
    const bool recorded = vr_body_maps->Record(scheduler.Commands(), rec.id, {lamps.data(), count});
    // VR_BODY_MAPS_DUMP=1 (tests): where his bones fall on map 0 (texels), beside the dump.
    static const bool dump_bones = [] {
        const char* const env = std::getenv("VR_BODY_MAPS_DUMP");
        return env != nullptr && env[0] == '1';
    }();
    static u32 bones_built = 0;
    if (dump_bones && recorded && bones_built++ % 300 == 150) {
        const auto& lamp = lamps[0];
        double lo_x = 1e9, lo_y = 1e9, hi_x = -1e9, hi_y = -1e9;
        for (const auto& bone : bones) {
            const double v[3] = {bone.x - lamp.origin[0], bone.y - lamp.origin[1],
                                 bone.z - lamp.origin[2]};
            double q[3];
            for (u32 r = 0; r < 3; ++r) {
                q[r] = lamp.axes[3 * r] * v[0] + lamp.axes[3 * r + 1] * v[1] +
                       lamp.axes[3 * r + 2] * v[2];
            }
            if (q[2] <= 0.0) {
                continue;
            }
            const double tx = (q[0] / q[2] * lamp.f * 0.5 + 0.5) * VrBodyMaps::MapSize;
            const double ty = (q[1] / q[2] * lamp.f * 0.5 + 0.5) * VrBodyMaps::MapSize;
            lo_x = std::min(lo_x, tx);
            lo_y = std::min(lo_y, ty);
            hi_x = std::max(hi_x, tx);
            hi_y = std::max(hi_y, ty);
        }
        LOG_INFO(Render_Vulkan,
                 "VR body maps: built {}: his bones on map 0 at texels x {:.0f}..{:.0f}, y "
                 "{:.0f}..{:.0f}",
                 bones_built, lo_x, hi_x, lo_y, hi_y);
    }
    if constexpr (Common::Log::Enabled) {
        static u32 logged = 0;
        if (logged < 12 && (++logged % 3) == 1) {
            LOG_INFO(Render_Vulkan, "VR body maps: frame {}: {} lamp(s) near him, maps {}", rec.id,
                     count, recorded ? "built" : "not built (no complete capture)");
        }
    }
}

// The game's fill light ahead of the camera kept (vr.ini camera_fill_light; the live key
// lighting.camera_fill overrides it, tests).
bool Rasterizer::VrCameraFillKept() const {
    return VR::Tune::Get("lighting.camera_fill", VR::GetSettings().camera_fill_light ? 1.0f : 0.0f) >=
           0.5f;
}

// The lamp shadows' blocker grid (light_shadow.h Grid): made and cleared to zero (every voxel
// empty) once, outside a render pass, before the first lamp draw whose program declares it.
void Rasterizer::VrShadowGridBeforeDraw(const GraphicsPipeline* pipeline) {
    namespace Grid = Shader::Optimization::LightShadow::Grid;
    if (vr_grid_ready || !Grid::Enabled() || !VrLampShadowsOn() || !VR::IsEnabled()) {
        return;
    }
    const auto* fs = pipeline->GetStages()[u32(Shader::LogicalStage::Fragment)];
    if (fs == nullptr || std::ranges::none_of(fs->buffers, [](const auto& buffer) {
            return buffer.buffer_type == Shader::BufferType::VrShadowGrid;
        })) {
        return;
    }
    constexpr u64 bytes = u64(Grid::BufferWords) * sizeof(u32); // the cells and the block stamps
    vr_grid_buffer = std::make_unique<VideoCore::Buffer>(
        instance, scheduler, VideoCore::MemoryUsage::DeviceLocal, 0, VideoCore::AllFlags, bytes);
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.Commands();
    NativeStereo::Barrier(scheduler);
    cmdbuf.fillBuffer(vr_grid_buffer->Handle(), 0, bytes, 0u);
    NativeStereo::Barrier(scheduler);
    vr_grid_ready = true;
    LOG_INFO(Render_Vulkan, "VR lamp shadows: blocker grid {}x{}x{} of {} m ({} KiB)", Grid::SideX,
             Grid::SideY, Grid::SideZ, Grid::VoxelM, bytes >> 10);
}

// The hunter's capsules (body_shadow.h): his bones this frame (VR::RecordPlayerBones: world axes
// minus his origin, game units) on to the light pass's flat camera (VrFogCaptureView), once a
// frame; none (valid 0) without his bones, the camera or in a mirror's pass.
u32 Rasterizer::VrBodyUpload(std::span<u32> out, u64 pgm_hash) {
    namespace Body = Shader::Optimization::BodyShadow;
    namespace C = VR::Body::C0000;
    static_assert(Body::BufferDwords == std::tuple_size_v<decltype(vr_body.data)>);
    const VrShadowCpuScope timed{4};
    const auto bytes = static_cast<u32>(Body::BufferDwords * sizeof(u32));
    // (VR_MARCH_OPT 64: no memset first: the copy below writes every word; one lock for the
    // sections that need vr_fog's, not one each.)
    const bool opt_cpu = Shader::Optimization::LightShadow::SceneMaps::Opt(64);
    if (!opt_cpu) {
        std::ranges::fill(out, 0u);
    }
    if (!Body::Enabled() || VrInMirrorPass() || !VR::IsEnabled()) {
        if (opt_cpu) {
            std::ranges::fill(out, 0u);
        }
        return bytes;
    }
    const VR::CameraRecord rec = VR::DrawFrameRecord();
    if (vr_body.frame != rec.id) {
        vr_body.frame = rec.id;
        vr_body.data.fill(0u);
        std::array<VR::Vec3, VR::PlayerBoneCount> bones{};
        std::array<float, 12> view{};
        bool have_view = false;
        {
            std::scoped_lock lock{vr_fog.mutex};
            have_view = vr_fog.view_frame != 0;
            view = vr_fog.view;
        }
        if (rec.id != 0 && rec.has_player_position && have_view &&
            VR::RecordPlayerBones(rec.id, bones)) {
            const float units = rec.world_scale > 0.0f ? rec.world_scale : 1.0f;
            const auto to_view = [&](const VR::Vec3& b) {
                const std::array<float, 3> d{rec.player_position.x + b.x - view[3],
                                             rec.player_position.y + b.y - view[7],
                                             rec.player_position.z + b.z - view[11]};
                return std::array<float, 3>{view[0] * d[0] + view[4] * d[1] + view[8] * d[2],
                                            view[1] * d[0] + view[5] * d[1] + view[9] * d[2],
                                            view[2] * d[0] + view[6] * d[1] + view[10] * d[2]};
            };
            const VR::Vec3 neck = bones[C::Neck], head = bones[C::Head];
            const VR::Vec3 crown{head.x + 0.9f * (head.x - neck.x), head.y + 0.9f * (head.y - neck.y),
                                 head.z + 0.9f * (head.z - neck.z)};
            struct Limb {
                VR::Vec3 a, b;
                float radius_m;
            };
            const std::array<Limb, Body::MaxCapsules> limbs{{
                {bones[C::L_Thigh], bones[C::L_Calf], 0.095f},
                {bones[C::L_Calf], bones[C::L_Foot], 0.065f},
                {bones[C::R_Thigh], bones[C::R_Calf], 0.095f},
                {bones[C::R_Calf], bones[C::R_Foot], 0.065f},
                {bones[C::Pelvis], bones[C::Spine1], 0.15f},
                {bones[C::Spine1], neck, 0.16f},
                {neck, crown, 0.09f},
                {bones[C::L_Clavicle], bones[C::R_Clavicle], 0.08f},
                {bones[C::L_UpperArm], bones[C::L_Forearm], 0.055f},
                {bones[C::L_Forearm], bones[C::L_Hand], 0.045f},
                {bones[C::R_UpperArm], bones[C::R_Forearm], 0.055f},
                {bones[C::R_Forearm], bones[C::R_Hand], 0.045f},
            }};
            std::array<float, 3> lo{1e30f, 1e30f, 1e30f}, hi{-1e30f, -1e30f, -1e30f};
            bool finite = true;
            for (u32 k = 0; k < Body::MaxCapsules; ++k) {
                const auto a = to_view(limbs[k].a), b = to_view(limbs[k].b);
                const u32 base = Body::HeaderDwords + k * Body::CapsuleDwords;
                for (u32 c = 0; c < 3; ++c) {
                    finite = finite && std::isfinite(a[c]) && std::isfinite(b[c]);
                    lo[c] = std::min({lo[c], a[c], b[c]});
                    hi[c] = std::max({hi[c], a[c], b[c]});
                    vr_body.data[base + c] = std::bit_cast<u32>(a[c]);
                    vr_body.data[base + 4 + c] = std::bit_cast<u32>(b[c]);
                }
                vr_body.data[base + 3] = std::bit_cast<u32>(limbs[k].radius_m * units);
            }
            const std::array<float, 3> centre{0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]),
                                              0.5f * (lo[2] + hi[2])};
            const float radius = 0.5f * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) +
                                                  (hi[1] - lo[1]) * (hi[1] - lo[1]) +
                                                  (hi[2] - lo[2]) * (hi[2] - lo[2])) +
                                 0.2f * units;
            // Sane: a body some 0.5 to 3 m across, within 6 m of the camera.
            const float span_m = 2.0f * radius / units;
            const float away_m = std::sqrt(centre[0] * centre[0] + centre[1] * centre[1] +
                                           centre[2] * centre[2]) / units;
            if (finite && span_m > 0.4f && span_m < 4.0f && away_m < 6.0f) {
                const float half = NativeStereo::Enabled() ? 0.5f * NativeStereo::Ipd(rec) * rec.world_scale : 0.0f;
                vr_body.data[0] = Body::MaxCapsules;
                vr_body.data[1] = std::bit_cast<u32>(half);
                vr_body.data[2] = 1u;
                vr_body.data[3] = std::bit_cast<u32>(units);
                for (u32 c = 0; c < 3; ++c) {
                    vr_body.data[4 + c] = std::bit_cast<u32>(centre[c]);
                }
                vr_body.data[7] = std::bit_cast<u32>(radius);
                // His body maps built for this frame (vr_body_maps.h): each lamp in this flat view,
                // and this view to the lamp's frame (q = A (world - player - origin)).
                const auto built = vr_body_maps ? vr_body_maps->Maps(rec.id)
                                                : std::array<VrBodyMaps::Built, VrBodyMaps::MaxMaps>{};
                const std::array<double, 3> cam{view[3], view[7], view[11]};
                const std::array<double, 3> player{rec.player_position.x, rec.player_position.y,
                                                   rec.player_position.z};
                for (u32 k = 0; k < Body::MaxMaps && k < built.size(); ++k) {
                    if (!built[k].valid) {
                        continue;
                    }
                    const auto& lamp = built[k].lamp;
                    const u32 h = Body::MapsAt + k * Body::MapDwords;
                    const auto lamp_view = to_view(VR::Vec3{float(lamp.origin[0]),
                                                            float(lamp.origin[1]),
                                                            float(lamp.origin[2])});
                    vr_body.data[h + 0] = 1u;
                    vr_body.data[h + 1] = built[k].base;
                    vr_body.data[h + 2] = std::bit_cast<u32>(lamp.f);
                    vr_body.data[h + 3] = std::bit_cast<u32>(lamp.near_z);
                    for (u32 c = 0; c < 3; ++c) {
                        vr_body.data[h + 4 + c] = std::bit_cast<u32>(lamp_view[c]);
                    }
                    vr_body.data[h + 7] = VrBodyMaps::MapSize;
                    for (u32 r = 0; r < 3; ++r) {
                        double row[4]{};
                        for (u32 j = 0; j < 3; ++j) {
                            const double a = lamp.axes[3 * r + j];
                            for (u32 m = 0; m < 3; ++m) {
                                row[m] += a * view[4 * j + m];
                            }
                            row[3] += a * (cam[j] - player[j] - lamp.origin[j]);
                        }
                        for (u32 m = 0; m < 4; ++m) {
                            vr_body.data[h + 8 + 4 * r + m] = std::bit_cast<u32>(float(row[m]));
                        }
                    }
                }
            } else {
                vr_body.data.fill(0u);
            }
            if constexpr (Common::Log::Enabled) {
                static u32 logged = 0;
                if (logged < 20 && (++logged % 4) == 1) {
                    LOG_INFO(Render_Vulkan,
                             "VR body shadow: {} capsules, sphere ({:.2f} {:.2f} {:.2f}) r {:.2f}, "
                             "{:.2f} m across, {:.2f} m from the camera{}",
                             vr_body.data[2] ? Body::MaxCapsules : 0u, centre[0], centre[1],
                             centre[2], radius, span_m, away_m, vr_body.data[2] ? "" : " (refused)");
                }
            }
        }
    }
    // The blocker grid's header (light_shadow.h Grid, body_shadow.h GridAt), whatever his body's
    // state: once a frame, while the grid exists and the light pass's flat view is known.
    if (vr_grid_ready && vr_grid_header != rec.id) {
        namespace Grid = Shader::Optimization::LightShadow::Grid;
        vr_grid_header = rec.id;
        std::array<float, 12> view{};
        bool have_view = false;
        {
            std::scoped_lock lock{vr_fog.mutex};
            have_view = vr_fog.view_frame != 0;
            view = vr_fog.view;
        }
        const u32 g = Body::GridAt;
        std::fill_n(vr_body.data.begin() + g, Body::GridDwords, 0u);
        if (have_view && rec.id != 0) {
            const float units = rec.world_scale > 0.0f ? rec.world_scale : 1.0f;
            const float half =
                NativeStereo::Enabled() ? 0.5f * NativeStereo::Ipd(rec) * rec.world_scale : 0.0f;
            // Stamps 1 .. StampMask (0: empty).
            vr_grid_counter = vr_grid_counter % Grid::StampMask + 1u;
            // Live keys (tests): lighting.grid=0 turns it off; lighting.shadow_thick_cm sets
            // the march's blocker thickness (0: light_shadow.h ThickM).
            vr_body.data[g + 0] = VR::Tune::Get("lighting.grid", 1.0f) >= 0.5f ? 1u : 0u;
            vr_body.data[g + 7] = std::bit_cast<u32>(
                std::clamp(VR::Tune::Get("lighting.shadow_thick_cm", 0.0f), 0.0f, 200.0f) * 0.01f *
                units);
            vr_body.data[g + 1] = vr_grid_counter;
            vr_body.data[g + 2] = std::bit_cast<u32>(Grid::VoxelM * units);
            vr_body.data[g + 3] = u32(Grid::LifeS * Grid::FramesPerS);
            vr_body.data[g + 4] = std::bit_cast<u32>(half);
            vr_body.data[g + 5] = std::bit_cast<u32>(Grid::NearSkipM * units);
            vr_body.data[g + 6] = std::bit_cast<u32>(units);
            for (u32 k = 0; k < 12; ++k) {
                vr_body.data[g + 8 + k] = std::bit_cast<u32>(view[k]);
            }
        }
    }
    // (Opt 64: vr_fog's lock once for the three sections below.)
    std::unique_lock sections_lock{vr_fog.mutex, std::defer_lock};
    if (opt_cpu) {
        sections_lock.lock();
    }
    // The lights the game keeps on the camera (body_shadow.h AttachedAt): those seen lately.
    {
        namespace LightTune = Shader::Optimization::LightShadow;
        const u32 a = Body::AttachedAt;
        std::fill_n(vr_body.data.begin() + a, Body::AttachedDwords, 0u);
        std::unique_lock lock{vr_fog.mutex, std::defer_lock};
        if (!opt_cpu) {
            lock.lock();
        }
        u32 n = 0;
        vr_attached.RefreshConfirmed();
        for (const u64 key : vr_attached.confirmed_keys) {
            const auto found = vr_attached.candidates.find(key);
            if (n == LightTune::AttachedCameraSlots) {
                break;
            }
            if (found == vr_attached.candidates.end() || !found->second.attached ||
                vr_attached.frame - found->second.last_frame > LightTune::AttachedForgetFrames) {
                continue;
            }
            const auto& c = found->second;
            for (u32 i = 0; i < 3; ++i) {
                vr_body.data[a + 4 + 4 * n + i] = std::bit_cast<u32>(c.pos[i]);
            }
            vr_body.data[a + 4 + 4 * n + 3] = std::bit_cast<u32>(c.reach);
            ++n;
        }
        // Then this frame's NoCastLights (an empty slot's reach 0 matches nothing).
        for (u32 j = 0; j < vr_attached.no_cast_count && vr_attached.frame == vr_fog.view_frame; ++j) {
            for (u32 i = 0; i < 4; ++i) {
                vr_body.data[a + 4 + 4 * (LightTune::AttachedCameraSlots + j) + i] =
                    std::bit_cast<u32>(vr_attached.no_cast[j][i]);
            }
        }
        vr_body.data[a] = n == 0 && vr_attached.no_cast_count == 0 ? 0u : LightTune::AttachedMax;
        vr_body.data[a + 1] = std::bit_cast<u32>(
            NativeStereo::Enabled() ? 0.5f * NativeStereo::Ipd(rec) * rec.world_scale : 0.0f);
        // Removed too unless vr.ini camera_fill_light (the live key lighting.camera_fill).
        vr_body.data[a + 2] = VrCameraFillKept() ? 0u : 1u;
    }
    // The lamps his shadow comes from (body_shadow.h BodyLampsAt), this frame's, on to its flat
    // view; none listed (0): any lamp, as before.
    {
        const u32 bl = Body::BodyLampsAt;
        std::fill_n(vr_body.data.begin() + bl, Body::BodyLampsDwords, 0u);
        std::unique_lock lock{vr_fog.mutex, std::defer_lock};
        if (!opt_cpu) {
            lock.lock();
        }
        // (Picked at this frame's first lamp draw. A frame's passes need not carry the same
        // camera record, air_light.h FrameSlack: a neighbour's still counts as this frame's.)
        if (Shader::Optimization::AirLight::FrameNear(vr_body.top_frame, VR::DrawFrameRecord().id) &&
            vr_fog.view_frame != 0) {
            const auto& m = vr_fog.view;
            for (u32 j = 0; j < vr_body.top_count; ++j) {
                const auto& w = vr_body.top[j];
                const double d[3] = {w[0] - m[3], w[1] - m[7], w[2] - m[11]};
                for (u32 c = 0; c < 3; ++c) {
                    vr_body.data[bl + 4 + 4 * j + c] =
                        std::bit_cast<u32>(float(m[c] * d[0] + m[4 + c] * d[1] + m[8 + c] * d[2]));
                }
            }
            vr_body.data[bl] = vr_body.top_count;
        }
    }
    // The scene maps' slots (body_shadow.h SlotsAt; light_shadow.h SceneMaps): the chosen lamps
    // on to this frame's flat view, only once this frame's maps were cleared (the AO pass
    // writes them), none in a mirror's pass.
    {
        namespace Scene = Shader::Optimization::LightShadow::SceneMaps;
        const u32 sa = Body::SlotsAt;
        std::fill_n(vr_body.data.begin() + sa, Body::SlotsDwords, 0u);
        std::unique_lock lock{vr_fog.mutex, std::defer_lock};
        if (!opt_cpu) {
            lock.lock();
        }
        // Live keys (tests, A/B in one run): lighting.scene_maps=0 lists no slots (every lamp
        // marches), lighting.scene_grid=0 keeps the grid out of the cubes (header SlotsAt + 1).
        if (VrSceneMapsOn() && vr_scene.buffer && vr_fog.view_frame != 0 &&
            // (Cleared at this frame's AO pass, whose record a lamp draw need not share: a
            // neighbour's counts, air_light.h FrameSlack. The cubes' half is the one that clear
            // took: the clearing record's, the same as this draw's whenever the two agree.)
            Shader::Optimization::AirLight::FrameNear(vr_scene.cleared_frame, VR::DrawFrameRecord().id) &&
            VR::Tune::Get("lighting.scene_maps", 1.0f) >= 0.5f) {
            vr_body.data[sa + 1] = VR::Tune::Get("lighting.scene_grid", 1.0f) >= 0.5f ? 1u : 0u;
            vr_body.data[sa + 2] = u32(vr_scene.cleared_frame & 1u) | (vr_scene.edge_mask << Scene::EdgeSlotShift);
            vr_body.data[sa + 3] = vr_scene.mask ? vr_scene.mask_pixels : 0u;
            const auto& m = vr_fog.view;
            for (u32 r = 0; r < 3; ++r) {
                for (u32 c = 0; c < 3; ++c) {
                    vr_body.data[sa + 4 + 3 * r + c] = std::bit_cast<u32>(m[4 * r + c]);
                }
            }
            // (VR_MARCH_OPT 16: SlotHis where his shadow comes from the slot's lamp, BodyVisibility's
            // own test made here: the lamp in the flat view within MapMatchM of one of his lamps,
            // written above; none listed: every lamp. Once a frame: the slots, his lamps and the
            // view are the frame's; at every lamp draw it cost ~1 us a frame.)
            if (Scene::Opt(16)) {
                const std::array<u64, 5> his_key{VR::DrawFrameRecord().id, vr_body.top_frame,
                                                 vr_body.top_count, vr_scene.count, vr_fog.view_frame};
                if (his_key != vr_scene.his_key) {
                    vr_scene.his_key = his_key;
                    vr_scene.his_mask = 0;
                    const u32 bl = Body::BodyLampsAt;
                    const u32 listed = vr_body.data[bl];
                    const float units = std::max(std::bit_cast<float>(vr_body.data[3]), 1e-3f);
                    const float tolerance = Body::MapMatchM * units;
                    for (u32 j = 0; j < vr_scene.count && j < 32; ++j) {
                        const auto& lamp = vr_scene.chosen[j];
                        const float d[3] = {lamp.world[0] - m[3], lamp.world[1] - m[7], lamp.world[2] - m[11]};
                        bool his = listed == 0;
                        float flat[3];
                        for (u32 c = 0; c < 3; ++c) {
                            flat[c] = m[c] * d[0] + m[4 + c] * d[1] + m[8 + c] * d[2];
                        }
                        for (u32 h = 0; h < listed && h < Body::BodyLamps && !his; ++h) {
                            float dd = 0.0f;
                            for (u32 c = 0; c < 3; ++c) {
                                const float v = flat[c] - std::bit_cast<float>(vr_body.data[bl + 4 + 4 * h + c]);
                                dd += v * v;
                            }
                            his = std::sqrt(dd) < tolerance;
                        }
                        vr_scene.his_mask |= his ? (1u << j) : 0u;
                    }
                }
            }
            for (u32 j = 0; j < vr_scene.count; ++j) {
                const auto& lamp = vr_scene.chosen[j];
                const u32 e = sa + Scene::SlotHeader + Scene::SlotDwords * j;
                const float d[3] = {lamp.world[0] - m[3], lamp.world[1] - m[7], lamp.world[2] - m[11]};
                for (u32 c = 0; c < 3; ++c) {
                    vr_body.data[e + c] = std::bit_cast<u32>(d[c]);
                }
                vr_body.data[e + 3] = std::bit_cast<u32>(lamp.k);
                vr_body.data[e + 4] = std::bit_cast<u32>(lamp.strength);
                // (VR_MARCH_OPT 16: SlotHis where his shadow comes from this lamp, BodyVisibility's own
                // test once here: the lamp in the flat view within MapMatchM of one of his lamps,
                // written above; none listed: every lamp.)
                u32 flags = vr_scene.flags[j];
                if (Scene::Opt(16) && ((vr_scene.his_mask >> j) & 1u) != 0) {
                    flags |= Scene::SlotHis;
                }
                // (High lamp shadows: the frame's heroes, picked with the slots.)
                if (((vr_scene.hero_mask >> j) & 1u) != 0) {
                    flags |= Scene::SlotHero;
                }
                vr_body.data[e + 5] = flags;
                vr_body.data[e + 6] = std::bit_cast<u32>(lamp.clear);
                // Its trace's reach from the eye (light_shadow.h TraceFarLevels): the dial's; at
                // least the first level over its first TraceNewFrames frames (no whole cube yet).
                const float trace_m = vr_scene.age[j] < Scene::TraceNewFrames
                                          ? std::max(vr_scene.dial.reach_m, Scene::TraceFarLevels[0])
                                          : vr_scene.dial.reach_m;
                vr_body.data[e + 7] = std::bit_cast<u32>(trace_m);
                // The march's step scale (light_shadow.h StepDial; 1 without it).
                vr_body.data[e + 11] = std::bit_cast<u32>(
                    Scene::StepDial() ? Scene::StepDialScale[std::min(vr_scene.step_dial.level, Scene::StepDialLevels - 1)]
                                      : 1.0f);
                // Where its lamp was when last frame's cube was made (a moving light's cube is a
                // frame behind it).
                const auto& was = vr_scene.prev[j];
                const float dp[3] = {was[0] - m[3], was[1] - m[7], was[2] - m[11]};
                for (u32 c = 0; c < 3; ++c) {
                    vr_body.data[e + 8 + c] = std::bit_cast<u32>(dp[c]);
                }
            }
            // (Live key, tests: lighting.scene_slots caps the slots used, 1..Slots.)
            vr_body.data[sa] = std::min<u32>(
                vr_scene.count,
                u32(std::clamp(VR::Tune::Get("lighting.scene_slots", float(Scene::Slots)), 1.0f, float(Scene::Slots))));
            if constexpr (Common::Log::Enabled) {
                // Whether the camera the slots are put on is this frame's (the AO pass's 208-byte
                // buffer, VrFogCaptureView, binds before this one).
                static u32 n = 0;
                // (Diagnostics, to go: which programs get a frame-old camera, and how far it is
                // from the newest light pass camera seen.)
                static u32 old_ao = 0, old_other = 0, new_ao = 0, new_other = 0;
                static u64 old_hash = 0;
                static float old_turn = 0.0f;
                const bool is_ao = Shader::Optimization::AmbientOcclusion::IsAoProgram(pgm_hash);
                const bool fresh_view = vr_fog.view_frame == vr_fog.frame;
                (fresh_view ? (is_ao ? new_ao : new_other) : (is_ao ? old_ao : old_other))++;
                if (!fresh_view) {
                    old_hash = pgm_hash;
                }
                {
                    float apart = 0.0f;
                    for (u32 i = 0; i < 11; ++i) {
                        if (i % 4 != 3) {
                            apart = std::max(apart, std::abs(vr_fog.last_list_view[i] - vr_fog.view[i]));
                        }
                    }
                    old_turn = std::max(old_turn, apart);
                }
                if (n++ % 4000 == 0) {
                    LOG_INFO(Render_Vulkan,
                             "VR scene shadow maps: {} slots, camera {}; uploads with this frame's camera: AO {} others {}; "
                             "with a frame-old one: AO {} others {} (last program {:#x}); the slots' camera and the newest "
                             "light pass camera differ by up to {:.4f} (rotation terms); frames whose light pass camera "
                             "was another than the slots': {}",
                             vr_scene.count, fresh_view ? "this frame's" : "a frame old", new_ao, new_other, old_ao, old_other,
                             old_hash, old_turn, vr_fog.views_apart);
                    old_ao = old_other = new_ao = new_other = 0;
                    old_turn = 0.0f;
                }
            }
        }
    }
    if (sections_lock.owns_lock()) {
        sections_lock.unlock();
    }
    // Tests (body_shadow.h ProbeAt): light classes muted or moved for their shadows, live.
    {
        const u32 p = Body::ProbeAt;
        std::fill_n(vr_body.data.begin() + p, Body::ProbeDwords, 0u);
        const auto mask = [](const char* key) {
            return static_cast<u32>(std::clamp(VR::Tune::Get(key, 0.0f), 0.0f, 15.0f));
        };
        vr_body.data[p] = mask("lighting.mute_cast");
        vr_body.data[p + 1] = std::bit_cast<u32>(0.01f * std::clamp(VR::Tune::Get("lighting.nudge_x", 0.0f), -500.0f, 500.0f));
        vr_body.data[p + 2] = std::bit_cast<u32>(0.01f * std::clamp(VR::Tune::Get("lighting.nudge_y", 0.0f), -500.0f, 500.0f));
        vr_body.data[p + 3] = std::bit_cast<u32>(0.01f * std::clamp(VR::Tune::Get("lighting.nudge_z", 0.0f), -500.0f, 500.0f));
        vr_body.data[p + 4] = mask("lighting.nudge_sel");
    }
    std::copy_n(vr_body.data.begin(), std::min<size_t>(out.size(), vr_body.data.size()), out.begin());
    if (opt_cpu && out.size() > vr_body.data.size()) {
        std::fill(out.begin() + vr_body.data.size(), out.end(), 0u);
    }
    // This draw's lights' slots (VrFogCapture, this draw's list: bound before this buffer); none
    // otherwise (its lights cast none rather than another draw's).
    {
        namespace Scene = Shader::Optimization::LightShadow::SceneMaps;
        std::scoped_lock lock{vr_fog.mutex};
        const bool valid = vr_scene.draw_slots_valid && vr_scene.draw_slots_pgm == pgm_hash;
        for (u32 j = 0; j < Scene::DrawSlotWords && Body::SlotsAt + Scene::DrawSlotsAt + j < out.size(); ++j) {
            out[Body::SlotsAt + Scene::DrawSlotsAt + j] = valid ? vr_scene.draw_slots[j] : 0u;
        }
        vr_scene.draw_slots_valid = false;
        // The edge pass's serial (light_shadow.h EdgeSerialShift: dword 2's top bits): this frame's
        // (or a neighbour's, air_light.h FrameSlack), else 0: the lamps keep the blend.
        {
            const u32 serial =
                vr_edges_live != 0 && Shader::Optimization::AirLight::FrameNear(vr_edges_frame, VR::DrawFrameRecord().id)
                    ? vr_edges_live & Scene::EdgeSerialMask
                    : 0u;
            u32& word = out[Body::SlotsAt + 2];
            word = (word & ~(Scene::EdgeSerialMask << Scene::EdgeSerialShift)) | (serial << Scene::EdgeSerialShift);
        }
        if constexpr (Common::Log::Enabled) {
            // (How many draws had their lights' slots from the CPU, how many match on the GPU.)
            static u32 with = 0, without = 0, frames = 0;
            static u64 last = 0;
            (valid ? with : without) += pgm_hash != 0 ? 1u : 0u;
            if (const u64 f = VR::DrawFrameRecord().id; f != last) {
                last = f;
                if (++frames % 900 == 0) {
                    LOG_INFO(Render_Vulkan, "VR shadow mask: {} lamp draws with their slots from the CPU, {} matched on the GPU (900 frames)",
                             with, without);
                    with = without = 0;
                }
            }
        }
    }
    return bytes;
}

u32 Rasterizer::VrFogUpload(std::span<u32> out) {
    namespace Fog = Shader::Optimization::FogLight;
    namespace AirLight = Shader::Optimization::AirLight;
    std::ranges::fill(out, 0u);
    std::scoped_lock lock{vr_fog.mutex};
    // The light pass came since the last composite: its lights are this composite's (and the
    // next one's in this frame: the other eye, until the flip).
    if (!vr_fog.building.empty()) {
        vr_fog.ready.swap(vr_fog.building);
        vr_fog.building.clear();
        vr_fog.building_keys.fill(0);
    }
    if (!vr_fog.on || VrInMirrorPass()) {
        return Fog::BufferDwords * sizeof(u32);
    }
    // The lamp memory needs this frame's camera (the light pass's); without one, the frame's
    // own list as it is.
    const bool memory = vr_fog.view_frame == vr_fog.frame;
    if (!memory && vr_fog.ready.empty()) {
        return Fog::BufferDwords * sizeof(u32);
    }
    const bool update_memory = memory && vr_fog.memory_frame != vr_fog.frame;
    const u16 map_id = static_cast<u16>(VR::GameArea::Slot().load(std::memory_order_relaxed) >> 16);
    const VR::MapObjects::Table* const objects =
        update_memory && Fog::Fixtures() ? VR::MapObjects::Of(map_id) : nullptr;
    // The lights this map hangs where no flame is (map_objects.h NoHalo: the Fishing Hamlet's
    // pole lights): no glow of their own.
    const std::vector<VR::MapObjects::NoHaloAnchor>* const no_halo_list =
        update_memory || !memory ? VR::MapObjects::NoHaloOf(map_id) : nullptr;
    // Lights within MergeM of each other as one (a candle cluster): colours summed, at their
    // light-weighted centre, the widest reach.
    auto& merged = vr_fog.merged;
    std::array<u8, Fog::CaptureLights> previous_member;
    const auto captured = std::min(vr_fog.ready.size(), previous_member.size());
    const auto merge_lights = [&]<bool Packed>() {
        Fog::MergeCenters centers;
        merged.clear();
        for (u32 i = 0; i < captured; ++i) {
            const VrFogLight& f = vr_fog.ready[i];
            const float lum = 0.3f * f.colour[0] + 0.6f * f.colour[1] + 0.1f * f.colour[2];
            const bool on_lantern = objects && !f.map && VR::MapObjects::HasLanternReach(f.reach) &&
                                    objects->On(f.world[0], f.world[1], f.world[2]);
            const bool no_halo = no_halo_list && !f.map &&
                                 VR::MapObjects::NoHalo(*no_halo_list, f.world[0], f.world[1], f.world[2], f.reach);
            u32 index;
            if constexpr (Packed) {
                index = centers.Find(merged, f.position, on_lantern);
            } else {
                const auto first = std::ranges::find_if(merged, [&](const VrFogCluster& cluster) {
                    const auto& p = cluster.light.position;
                    const float dx = p[0] - f.position[0], dy = p[1] - f.position[1], dz = p[2] - f.position[2];
                    return dx * dx + dy * dy + dz * dz < Fog::MergeM * Fog::MergeM &&
                           cluster.light.on_lantern == on_lantern;
                });
                index = static_cast<u32>(first - merged.begin());
            }
            if (index == merged.size()) {
                previous_member[i] = Fog::EndMember;
                merged.push_back({f, std::max(lum, 1e-4f), static_cast<u8>(i)});
                merged.back().light.on_lantern = on_lantern;
                merged.back().light.no_halo = no_halo;
                if constexpr (Packed) {
                    centers.Set(index, f.position);
                }
                continue;
            }
            auto& cluster = merged[index];
            previous_member[i] = cluster.last_member;
            cluster.last_member = static_cast<u8>(i);
            VrFogLight& group = cluster.light;
            float& w = cluster.weight;
            const float t = std::max(lum, 1e-4f) / (w + std::max(lum, 1e-4f));
            for (u32 c = 0; c < 3; ++c) {
                group.position[c] += (f.position[c] - group.position[c]) * t;
                group.world[c] += (f.world[c] - group.world[c]) * t;
                group.colour[c] += f.colour[c];
            }
            group.reach = std::min(group.reach, f.reach);
            group.map = group.map || f.map;
            group.no_halo = group.no_halo && no_halo; // (a cluster without a halo: all its members)
            if constexpr (Packed) {
                centers.Set(index, group.position);
            }
            w += std::max(lum, 1e-4f);
        }
    };
    if (update_memory || !memory) {
        // Last time's cluster count only selects the search algorithm. Both return the
        // first eligible cluster in this capture. Later composites reuse the lamp memory.
        if (merged.size() >= Fog::MergeCenters::PackedFrom) {
            merge_lights.template operator()<true>();
        } else {
            merge_lights.template operator()<false>();
        }
    }
    const auto distance2 = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
        return dx * dx + dy * dy + dz * dz;
    };
    const auto& view = vr_fog.view;
    const std::array<float, 3> camera{view[3], view[7], view[11]};
    // The lamp memory (fog_light.h), once a frame: the listed lights found again in the world,
    // faded in; the others held or faded out.
    if (update_memory) {
        const auto now = std::chrono::steady_clock::now();
        // Bounded: a hitch or a menu without the scene is not the lights being gone.
        const float dt =
            vr_fog.memory_frame == 0
                ? 0.0f
                : std::clamp(std::chrono::duration<float>(now - vr_fog.memory_time).count(),
                             0.0f, 0.1f);
        vr_fog.memory_time = now;
        vr_fog.memory_frame = vr_fog.frame;
        vr_fog.memory_dt = dt;
        if (distance2(camera, vr_fog.memory_camera) > Fog::JumpM * Fog::JumpM) {
            vr_fog.memory.clear();
        }
        vr_fog.memory_camera = camera;
        for (VrFogRemembered& m : vr_fog.memory) {
            m.seen = false;
        }
        // The newest explosion (muzzle_flash.h Blast): its light never glows in the air, the mist
        // round it lights up for a moment (FogLight::IsBlastLight, BlastGlow).
        float since_blast = -1.0f;
        VR::MuzzleFlash::ListenBlasts(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
        if (std::array<float, 3> where{}; VR::MuzzleFlash::TakeBlast(vr_fog.blast_seen, where)) {
            vr_fog.blast_world = where;
            vr_fog.blast_t0 = now;
            vr_fog.blast_on = true;
        }
        if (vr_fog.blast_on) {
            since_blast = std::chrono::duration<float>(now - vr_fog.blast_t0).count();
            if (since_blast > 2.0f * Fog::BlastBornS) {
                vr_fog.blast_on = false; // (no light born from now on can be its)
                since_blast = -1.0f;
            }
        }
        for (const VrFogCluster& group : merged) {
            const VrFogLight& f = group.light;
            const float radius = 1.0f / std::sqrt(f.reach);
            VrFogRemembered* same = nullptr;
            float nearest = Fog::SameM * Fog::SameM;
            for (VrFogRemembered& m : vr_fog.memory) {
                const float d2 = distance2(m.world, f.world);
                if (!m.seen && d2 < nearest &&
                    std::abs(radius - m.radius) <= Fog::SameRadius * std::max(radius, m.radius) &&
                    (!m.fixture || (!f.map && VR::MapObjects::HasLanternReach(f.reach))) &&
                    (!f.on_lantern || (!m.map && VR::MapObjects::HasLanternReach(m.reach)))) {
                    same = &m;
                    nearest = d2;
                }
            }
            bool created = false;
            if (same == nullptr && vr_fog.memory.size() < Fog::MemoryLights) {
                same = &vr_fog.memory.emplace_back();
                created = true;
            } else if (same == nullptr) {
                created = true;
                // Full: a listed light takes the place of the faintest one not listed (the
                // frame lists fewer than the memory holds, so there is one; never drop the
                // light in your hand for an old lamp).
                for (VrFogRemembered& m : vr_fog.memory) {
                    if (!m.seen && (same == nullptr || m.weight < same->weight ||
                                    (m.weight == same->weight && m.unseen > same->unseen))) {
                        same = &m;
                    }
                }
                if (same != nullptr) {
                    *same = VrFogRemembered{};
                }
            }
            if (same != nullptr) {
                const bool clustered = previous_member[group.last_member] != Fog::EndMember;
                const u8 owner = Fog::HaloMember(vr_fog.ready, previous_member, group.last_member,
                                                 created ? f.world : same->halo_world);
                const auto& halo = vr_fog.ready[owner].world;
                // A replacement wick must earn its own source votes.
                if (!created && f.map && (clustered || same->clustered) && AirLight::Sources() &&
                    distance2(halo, same->halo_world) >= AirLight::SourceMatchM * AirLight::SourceMatchM) {
                    same->source = 0.0f;
                    same->sourced = false;
                    same->flame = 0.0f;
                }
                same->halo_world = halo;
                same->clustered = clustered;
                // Its flicker: the brightness's running mean and mean change (FlickerS).
                const float lum = 0.3f * f.colour[0] + 0.6f * f.colour[1] + 0.1f * f.colour[2];
                if (same->weight <= 0.0f && same->age <= 0.0f) {
                    same->lum_mean = lum; // new: no change yet
                    same->lum_dev = 0.0f;
                    same->steady = f.colour;
                } else {
                    const float a = dt / (dt + Fog::FlickerS);
                    same->lum_dev += a * (std::abs(lum - same->lum_mean) - same->lum_dev);
                    same->lum_mean += a * (lum - same->lum_mean);
                    const float s = dt / (dt + Fog::SteadyS);
                    for (u32 c = 0; c < 3; ++c) {
                        same->steady[c] += s * (f.colour[c] - same->steady[c]);
                    }
                }
                same->world = f.world;
                same->reach = f.reach;
                same->radius = radius;
                same->colour = f.colour;
                same->map = f.map;
                same->no_halo = f.no_halo;
                {
                    const Fog::Fixture next = Fog::FixtureNext(
                        {same->fixture, same->strayed}, f.map, objects != nullptr, f.on_lantern);
                    same->fixture = next.is;
                    same->strayed = next.strayed;
                }
                // Placed or lively (FogLight::PlacedStillS), for a light without the map's tag.
                if (created) {
                    same->rest = f.world;
                    same->still = 0.0f;
                    same->lively = false;
                    same->placed = false;
                }
                if (!f.map) {
                    const bool flickers = same->lum_dev > Fog::FlameFlicker * std::max(same->lum_mean, 1e-3f);
                    const bool in_hand = vr_fog.held_frame == vr_fog.frame &&
                                         distance2(f.world, vr_fog.held_world) < Fog::SameM * Fog::SameM;
                    // An explosion's light (FogLight::IsBlastLight): born beside it, near its time.
                    if (!same->blast && since_blast >= 0.0f &&
                        Fog::IsBlastLight(created ? 0.0f : same->age, since_blast,
                                          std::sqrt(distance2(f.world, vr_fog.blast_world)))) {
                        same->blast = true;
                        if constexpr (Common::Log::Enabled) {
                            static u32 logs = 0;
                            if (logs < 24) {
                                ++logs;
                                LOG_INFO(Render_Vulkan,
                                         "VR fog light: an explosion's light at ({:.1f} {:.1f} {:.1f}) r {:.1f} m, "
                                         "{:.2f} m from it, born {:.2f} s after it: no glow, its mist lit a moment",
                                         f.world[0], f.world[1], f.world[2], radius,
                                         std::sqrt(distance2(f.world, vr_fog.blast_world)),
                                         since_blast - (created ? 0.0f : same->age));
                            }
                        }
                    }
                    const bool moved = distance2(f.world, same->rest) > Fog::PlacedMoveM * Fog::PlacedMoveM;
                    if (moved) {
                        same->rest = f.world;
                        same->still = 0.0f;
                    } else {
                        same->still += dt;
                    }
                    if (moved || flickers || in_hand || same->fixture) {
                        same->lively = true;
                        same->placed = false;
                    } else if (same->still >= Fog::PlacedStillS) {
                        same->placed = true;
                        same->lively = false;
                    }
                }
                same->seen = true;
                // A light the game spawned, born beside the eye: a shot. The muzzle flash is lit
                // where it was born (FogLight's Muzzle*), not more than once in MuzzleHoldS.
                // OFF by default since 2026-11-12 night (fog_light.h MuzzleFromLists): the game
                // drops a wall lantern (a spawned light too) while it is out of view and lists it
                // anew as it comes back, which lit a shot's flash beside you every time (the user:
                // "When a new light appears, the screen glows ... as if the light is figuring
                // itself out"). The guns publish their shots (core/vr/muzzle_flash.h).
                if (Fog::MuzzleFromLists() && created && !f.map && !same->fixture &&
                    distance2(f.world, camera) < Fog::MuzzleNearM * Fog::MuzzleNearM &&
                    (!vr_fog.muzzle_on ||
                     std::chrono::duration<float>(now - vr_fog.muzzle_t0).count() > Fog::MuzzleHoldS)) {
                    vr_fog.muzzle_world = f.world;
                    vr_fog.muzzle_t0 = now;
                    vr_fog.muzzle_on = true;
                }
            }
        }
        static const bool flames_only = [] {
            const char* const env = std::getenv("VR_FOG_FLAMES");
            return env == nullptr || env[0] != '0';
        }();
        // No flame, no halo (air_light.h Sources): the votes the scene's copy gave a few frames
        // ago, counted into the remembered lights.
        const bool air_sources = AirLight::Sources();
        if (air_sources) {
            VrAirFrame(VR::DrawFrameRecord().id);
            VrAirSourceVotes();
        }
        for (VrFogRemembered& m : vr_fog.memory) {
            // A flame (it flickers) or a light the game spawned lights the air; a map light
            // that holds steady (a mood light) fades out of it (FogLight::FlameFlicker).
            // (A spawned light at once: a shot's flash must not wait.)
            // (A light without the map's tag: lively, a flame at once; placed, the map's rule;
            // not yet known which, no air lit: FogLight::PlacedStillS.)
            // (An explosion's light never: FogLight::IsBlastLight; its mist below, BlastGlow. Nor a
            // light the map hangs where no flame is: map_objects.h NoHalo.)
            const bool by_behaviour = Fog::PlacedByBehaviour() && !m.map;
            if (m.blast || m.no_halo) {
                m.flame = 0.0f;
            } else if (!flames_only || VR::Tune::Get("lighting.flames_only", 1.0f) < 0.5f ||
                       (!m.map && (!by_behaviour || m.lively ||
                                   (m.placed && !Fog::PlacedAsked(m.colour[0], m.colour[2]))))) {
                m.flame = 1.0f;
            } else if (by_behaviour && !m.placed) {
                m.flame = 0.0f;
            } else if (air_sources ? m.sourced
                                   : (m.lum_dev > Fog::FlameFlicker * std::max(m.lum_mean, 1e-3f) ||
                                      !Fog::IsMoodLight(m.reach, m.colour[0], m.colour[2]))) {
                m.flame = std::min(1.0f, m.flame + dt / Fog::FlameInS);
            } else {
                m.flame = std::max(0.0f, m.flame - dt / Fog::FlameOutS);
            }
            if (m.seen) {
                m.unseen = 0.0f;
                m.age += dt;
                // A spawned light (a shot's flash, a torch) at once; the map's lamps and its
                // fixtures (a lantern's flame: FogLight::FixtureNext) fade in.
                m.weight = std::min(
                    1.0f, m.weight + dt / (m.map || m.fixture ? Fog::FadeInS : Fog::FlashInS));
                continue;
            }
            m.unseen += dt;
            // (A placed light is kept like the map's: its flame's votes with it.)
            const bool held = (m.map || m.fixture || (by_behaviour && m.placed)) &&
                              (m.unseen < Fog::HoldS ||
                               distance2(camera, m.world) < Fog::HoldNearM * Fog::HoldNearM);
            if (!held) {
                // A flash goes as fast as it came; a carried torch slowly.
                const bool flash = !m.map && !m.fixture && m.age < Fog::FlashLifeS;
                m.weight -= dt / (flash ? Fog::FlashOutS : Fog::FadeOutS);
            }
        }
        std::erase_if(vr_fog.memory,
                      [](const VrFogRemembered& m) { return !m.seen && m.weight <= 0.0f; });
    }
    // The candidates: the remembered lights where this frame's camera sees them (the inverse of
    // its rigid view to world), each by its weight; or the frame's list as it is.
    using Candidate = VrFogCandidate;
    std::vector<Candidate>& candidates = vr_fog.candidates;
    candidates.clear();
    if (memory) {
        const auto into_view = [&](const std::array<float, 3>& world) {
            const std::array<float, 3> d{world[0] - view[3], world[1] - view[7], world[2] - view[11]};
            std::array<float, 3> p{};
            for (u32 c = 0; c < 3; ++c) {
                p[c] = view[c] * d[0] + view[4 + c] * d[1] + view[8 + c] * d[2];
            }
            return p;
        };
        candidates.reserve(vr_fog.memory.size());
        for (VrFogRemembered& m : vr_fog.memory) {
            if (m.weight <= 0.0f ||
                (m.flame <= 0.0f && !Shader::Optimization::AirLight::Sources())) {
                continue;
            }
            const auto p = into_view(m.world);
            // (A lamp's or a fixture's halo breathes gently round its colour's mean:
            // FogLight::HaloColour.)
            candidates.push_back({p, m.reach,
                                  (m.map || m.fixture) && Fog::SteadyHalo() ? Fog::HaloColour(m.steady, m.colour)
                                                                           : m.colour,
                                  m.weight * m.flame, 0.0f, &m, m.weight,
                                  m.clustered ? into_view(m.halo_world) : p});
        }
    } else {
        candidates.reserve(merged.size());
        for (const VrFogCluster& group : merged) {
            const VrFogLight& f = group.light;
            const u8 owner = Fog::HaloMember(vr_fog.ready, previous_member, group.last_member, f.world);
            // (A light the map hangs where no flame is: no glow, map_objects.h NoHalo.)
            candidates.push_back({f.position, f.reach, f.colour, f.no_halo ? 0.0f : 1.0f, 0.0f, nullptr, 0.0f,
                                  vr_fog.ready[owner].position});
        }
    }
    // The strongest MaxLights: brightness times reach over distance, by weight; one drawn last
    // frame counts KeepChosen times.
    const auto score = [](const Candidate& f) {
        const float lum = 0.3f * f.colour[0] + 0.6f * f.colour[1] + 0.1f * f.colour[2];
        const float d = std::sqrt(f.position[0] * f.position[0] + f.position[1] * f.position[1] +
                                  f.position[2] * f.position[2]);
        const float keep = f.remembered && f.remembered->chosen ? Fog::KeepChosen : 1.0f;
        // (One without a glow of its own ranks behind every one with: by its smoke's weight.)
        return keep * std::max(f.weight, 1e-3f * f.smoke) * lum / std::sqrt(f.reach) / std::max(d, 1.0f);
    };
    std::vector<const Candidate*>& order = vr_fog.order;
    order.clear();
    for (Candidate& c : candidates) {
        c.score = score(c);
        order.push_back(&c);
    }
    const u32 total = u32(order.size());
    const u32 count = std::min(total, Fog::MaxLights);
    std::partial_sort(order.begin(), order.begin() + count, order.end(),
                      [](const Candidate* a, const Candidate* b) { return a->score > b->score; });
    // The lights with a glow of their own first (their order kept): the glow's loop ends after
    // them (the count's high half), only the pixels that vote on the sources go on to the rest.
    const u32 glow_count = static_cast<u32>(
        std::stable_partition(order.begin(), order.begin() + count,
                              [](const Candidate* c) { return c->weight > 0.0f; }) -
        order.begin());
    for (VrFogRemembered& m : vr_fog.memory) {
        m.chosen = false;
    }
    for (u32 k = 0; k < count; ++k) {
        if (order[k]->remembered) {
            order[k]->remembered->chosen = true;
        }
    }
    if (memory && Shader::Optimization::AirLight::Sources()) {
        VrAirSourceList(count, order.data());
    }
    const VR::CameraRecord rec = VR::DrawFrameRecord();
    const float half = NativeStereo::Enabled() ? 0.5f * NativeStereo::Ipd(rec) * rec.world_scale : 0.0f;
    out[0] = count | (glow_count << 16);
    out[1] = std::bit_cast<u32>(std::isfinite(half) ? half : 0.0f);
    out[2] = std::bit_cast<u32>(VR::Tune::Get("lighting.air", vr_fog.density));
    // Flames fill the room (FogLight::FillStrength; VR_FLAME_FILL overrides it).
    static const float fill_strength = [] {
        const char* const env = std::getenv("VR_FLAME_FILL");
        return env ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 1.0f) : Fog::FillStrength;
    }();
    out[3] = std::bit_cast<u32>(VR::Tune::Get("lighting.fill", fill_strength));
    // Shadowed air (FogLight::AirLamps): the strongest lamps from AirNearM to AirFarM of the eye
    // march their shadows (never the lantern in your hand); each one's share fades over
    // AirFadeS, moved once a frame (this runs for both eyes' composites).
    static const bool air_shadows = [] {
        const char* const env = std::getenv("VR_AIR_SHADOWS");
        return env == nullptr || env[0] != '0';
    }();
    const bool shadow_step = vr_fog.shadow_frame != vr_fog.frame;
    vr_fog.shadow_frame = vr_fog.frame;
    u32 shadowed = 0;
    // The fog grid lights the mist with these lights itself (fog_grid.h Lamps): it takes the ones
    // with a glow of their own, and the halo's extras for the mist are left out.
    const bool grid_lamps =
        Shader::Optimization::FogGrid::Enabled() && Shader::Optimization::FogGrid::Lamps::Mode() != 0;
    vr_fog.grid_light_count = 0;
    vr_fog.grid_flash = false;
    vr_fog.grid_lights_frame = vr_fog.frame;
    for (u32 k = 0; k < count; ++k) {
        const Candidate& f = *order[k];
        u32* d = out.data() + Fog::HeaderDwords + Fog::LightDwords * k;
        // The hunter's own lantern lights the haze all round the eye: a share of it.
        const float eye_distance = std::sqrt(f.position[0] * f.position[0] +
                                             f.position[1] * f.position[1] +
                                             f.position[2] * f.position[2]);
        // (A small light keeps a small glow: fog_light.h SmallLightM; reach is 1 / radius^2.)
        const float small = std::min(1.0f, 1.0f / std::max(f.reach * Fog::SmallLightM * Fog::SmallLightM, 1e-6f));
        const float share = f.weight * small * Fog::NearEyeShare(eye_distance);
        float shadow = 0.0f;
        if (air_shadows && f.remembered != nullptr &&
            VR::Tune::Get("lighting.air_shadows", 1.0f) >= 0.5f) {
            VrFogRemembered& m = *f.remembered;
            const bool want = shadowed < Fog::AirLamps && eye_distance >= Fog::AirNearM &&
                              eye_distance <= Fog::AirFarM;
            shadowed += want ? 1u : 0u;
            if (shadow_step) {
                const float move = vr_fog.memory_dt / Fog::AirFadeS;
                m.shadow = want ? std::min(1.0f, m.shadow + move) : std::max(0.0f, m.shadow - move);
            }
            shadow = m.shadow;
        }
        // The air's colour (Fog::AirColour): a very bright light capped, its hue a share greyer.
        const auto air = Fog::AirColour(f.colour);
        // In the mist (air_light.h Mist::LampGlow): a lamp standing in it lights more of it.
        float in_mist = 1.0f;
        if (k < vr_air.widen.size()) {
            vr_air.widen[k] = 1.0f;
            vr_air.widen_count = count;
        }
        if (grid_lamps && k < glow_count && k + 1 < vr_fog.grid_lights.size()) {
            vr_fog.grid_lights[vr_fog.grid_light_count++] = {
                f.position,
                f.reach,
                {f.weight * small * air[0], f.weight * small * air[1], f.weight * small * air[2]},
                Shader::Optimization::FogGrid::Lamps::CoreM2};
        }
        if (f.remembered != nullptr && vr_air.ground_valid && Shader::Optimization::AirLight::Mist::Enabled() &&
            vr_air.mist_carry_frame != ~0ull) { // (the mist's numbers were made: with the cells or the fog grid)
            namespace Mist = Shader::Optimization::AirLight::Mist;
            const float over = std::max(f.remembered->world[1] - vr_air.ground, 0.0f);
            const float height = std::clamp(VR::Tune::Get("lighting.mist_height", Mist::HeightM), 0.2f, 30.0f);
            const float density = std::clamp(VR::Tune::Get("lighting.mist", Mist::Density), 0.0f, 2.0f);
            // (With the fog grid's lamps, fog_grid.h Lamps, the mist round a lamp is lit there: no
            // extra glow and no wider reach here. What a lamp seen THROUGH the mist gets back stays.)
            const float layer = grid_lamps ? 0.0f
                                           : std::exp(-over / height) * std::min(density / Mist::Density, 2.0f) *
                                                 vr_air_aim.open_sky * vr_air.area_share;
            in_mist += std::clamp(VR::Tune::Get("lighting.mist_lamp", Mist::LampGlow), 0.0f, 4.0f) * layer;
            // Nearer lamps more (Mist::NearGlow), and all of them wider in the air (LampWide).
            const float near_share = std::clamp((Mist::NearGlowOutM - eye_distance) /
                                                    (Mist::NearGlowOutM - Mist::NearGlowInM), 0.0f, 1.0f);
            in_mist += std::clamp(VR::Tune::Get("lighting.mist_near_glow", Mist::NearGlow), 0.0f, 4.0f) *
                       near_share * near_share * (3.0f - 2.0f * near_share) * layer;
            if (k < vr_air.widen.size()) {
                vr_air.widen[k] =
                    1.0f + std::clamp(VR::Tune::Get("lighting.mist_lamp_wide", Mist::LampWide), 0.0f, 3.0f) * layer;
            }
            // Seen through the mist (Mist::FarGlow): the layer's mean along the way from the eye
            // (its height over the ground) to the light, times the way's length and the density.
            const float eye_over = std::max(view[7] - vr_air.ground, 0.0f);
            const float rise = over - eye_over;
            const float mean = std::abs(rise) < 1e-2f
                                   ? std::exp(-eye_over / height)
                                   : height * (std::exp(-eye_over / height) - std::exp(-over / height)) / rise;
            const float way = std::min(eye_distance, std::clamp(VR::Tune::Get("lighting.mist_far", Mist::FarM), 5.0f, 300.0f));
            const float tau = density * vr_air_aim.open_sky * Mist::BanksMean * std::max(mean, 0.0f) *
                              std::max(way - Mist::NearStartM, 0.0f);
            in_mist += std::clamp(VR::Tune::Get("lighting.mist_far_glow", Mist::FarGlow), 0.0f, 8.0f) *
                       (1.0f - std::exp(-tau));
        }
        const auto& halo = f.halo_position;
        const std::array<float, 8> values{halo[0], halo[1], halo[2], f.reach,
                                          share * in_mist * air[0], share * in_mist * air[1],
                                          share * in_mist * air[2], shadow};
        for (u32 c = 0; c < values.size(); ++c) {
            d[c] = std::bit_cast<u32>(values[c]);
        }
    }
    if constexpr (Common::Log::Enabled) {
        // Every 600th upload with lights (about 10 s), 40 at most: the strongest six.
        static u32 uploads = 0, logs = 0;
        if (count > 0 && uploads++ % 600 == 0 && logs < 40) {
            ++logs;
            std::string lights;
            for (u32 k = 0; k < std::min(count, 6u); ++k) {
                const Candidate& f = *order[k];
                const VrFogRemembered* const m = f.remembered;
                lights += fmt::format(
                    " [({:.1f} {:.1f} {:.1f}) r {:.1f} m ({:.2f} {:.2f} {:.2f}) {} flicker {:.3f} "
                    "flame {:.2f}]",
                    f.halo_position[0], f.halo_position[1], f.halo_position[2], 1.0f / std::sqrt(f.reach),
                    f.colour[0], f.colour[1], f.colour[2],
                    m && m->map ? "map" : m && m->fixture ? "fixture" : "spawned",
                    m ? m->lum_dev / std::max(m->lum_mean, 1e-3f) : -1.0f, m ? m->flame : -1.0f);
            }
            LOG_INFO(Render_Vulkan,
                     "VR fog light: {} of {} light(s), {} remembered, camera ({:.1f} {:.1f} "
                     "{:.1f}), render camera ({:.1f} {:.1f} {:.1f}), half shift {:.4f}, density {}:{}",
                     count, total, vr_fog.memory.size(), camera[0], camera[1], camera[2],
                     rec.render_position.x, rec.render_position.y, rec.render_position.z, half,
                     vr_fog.density, lights);
        }
    }
    // An explosion's light: no glow of its own in the air, the mist round it lit for a moment
    // (FogLight::BlastGlow), BlastWidenM wider. (Room kept for the torch in his hand and a shot.)
    if (grid_lamps && memory) {
        for (const VrFogRemembered& m : vr_fog.memory) {
            const float glow = m.blast ? Fog::BlastGlow(m.age) * m.weight : 0.0f;
            if (glow <= 0.0f || vr_fog.grid_light_count + 2 >= vr_fog.grid_lights.size()) {
                continue;
            }
            const std::array<float, 3> d{m.world[0] - view[3], m.world[1] - view[7], m.world[2] - view[11]};
            std::array<float, 3> p{};
            for (u32 c = 0; c < 3; ++c) {
                p[c] = view[c] * d[0] + view[4 + c] * d[1] + view[8 + c] * d[2];
            }
            const float reach = Fog::BlastReach(m.reach);
            const float small = std::min(1.0f, 1.0f / std::max(reach * Fog::SmallLightM * Fog::SmallLightM, 1e-6f));
            const auto air = Fog::AirColour(m.colour);
            vr_fog.grid_lights[vr_fog.grid_light_count++] = {
                p, reach, {glow * small * air[0], glow * small * air[1], glow * small * air[2]},
                Shader::Optimization::FogGrid::Lamps::CoreM2};
        }
    }
    // The torch in his hand lights the mist too (fog_grid.h Lamps::Held*; the capture kept it).
    if (grid_lamps && memory && vr_fog.held_frame == vr_fog.frame &&
        vr_fog.grid_light_count + 1 < vr_fog.grid_lights.size()) {
        namespace Lamps = Shader::Optimization::FogGrid::Lamps;
        const float share = std::clamp(VR::Tune::Get("lighting.grid_held", Lamps::HeldShare), 0.0f, 8.0f);
        if (share > 0.0f) {
            const std::array<float, 3> d{vr_fog.held_world[0] - view[3], vr_fog.held_world[1] - view[7],
                                         vr_fog.held_world[2] - view[11]};
            std::array<float, 3> p{};
            for (u32 c = 0; c < 3; ++c) {
                p[c] = view[c] * d[0] + view[4 + c] * d[1] + view[8 + c] * d[2];
            }
            // (Its own hue, not the air's greyer one: a warm light in the cold mist. As bright as
            // the air colour would be.)
            auto air = Fog::AirColour(vr_fog.held_colour);
            {
                const auto& c = vr_fog.held_colour;
                const float lum = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
                const float air_lum = 0.2126f * air[0] + 0.7152f * air[1] + 0.0722f * air[2];
                if (lum > 1e-4f) {
                    air = {c[0] * air_lum / lum, c[1] * air_lum / lum, c[2] * air_lum / lum};
                }
            }
            vr_fog.grid_lights[vr_fog.grid_light_count++] = {
                p, vr_fog.held_reach, {share * air[0], share * air[1], share * air[2]}, Lamps::HeldCoreM2};
        }
    }
    // A shot the gun published (muzzle_flash.h): the flash at its muzzle, now (the light lists may
    // never show the shot's own light: a candle-lit hall fills them first).
    {
        std::array<float, 3> where{};
        if (VR::MuzzleFlash::Take(vr_fog.muzzle_seen, where)) {
            vr_fog.muzzle_world = where;
            vr_fog.muzzle_t0 = std::chrono::steady_clock::now();
            vr_fog.muzzle_on = true;
        }
    }
    // The muzzle flash's block: its view position (this frame's camera), reach, colour times its
    // envelope, 1 while it shines (FogLight's Muzzle*).
    if (vr_fog.muzzle_on) {
        const float age =
            std::chrono::duration<float>(std::chrono::steady_clock::now() - vr_fog.muzzle_t0).count();
        if (age > Fog::MuzzleLifeS || !memory) {
            vr_fog.muzzle_on = age <= Fog::MuzzleLifeS && vr_fog.muzzle_on;
        } else {
            const float hold = VR::Tune::Get("lighting.muzzle_hold", Fog::MuzzleHoldS);
            const float fade = std::max(VR::Tune::Get("lighting.muzzle_fade", Fog::MuzzleFadeS), 1e-3f);
            const float envelope = age < hold ? 1.0f : std::exp(-(age - hold) / fade);
            const float intensity = VR::Tune::Get("lighting.muzzle", Fog::MuzzleIntensity) * envelope;
            const std::array<float, 3> d{vr_fog.muzzle_world[0] - view[3],
                                         vr_fog.muzzle_world[1] - view[7],
                                         vr_fog.muzzle_world[2] - view[11]};
            u32* const mz = out.data() + Fog::MuzzleDword;
            for (u32 c = 0; c < 3; ++c) {
                mz[c] = std::bit_cast<u32>(view[c] * d[0] + view[4 + c] * d[1] + view[8 + c] * d[2]);
                mz[4 + c] = std::bit_cast<u32>(Fog::MuzzleColour[c] * intensity);
            }
            const float radius = std::max(VR::Tune::Get("lighting.muzzle_radius", Fog::MuzzleRadiusM), 0.5f);
            mz[3] = std::bit_cast<u32>(1.0f / (radius * radius));
            mz[7] = std::bit_cast<u32>(Fog::MuzzleSoftM2); // its core (above 0.5: it shines)
            // The flash lights the mist too (fog_grid.h Lamps): the last of the grid's lights.
            if (grid_lamps && vr_fog.grid_light_count < vr_fog.grid_lights.size()) {
                const float share = std::clamp(
                    VR::Tune::Get("lighting.grid_flash", Shader::Optimization::FogGrid::Lamps::FlashShare), 0.0f,
                    8.0f);
                if (share > 0.0f) {
                    vr_fog.grid_lights[vr_fog.grid_light_count++] = {
                        {std::bit_cast<float>(mz[0]), std::bit_cast<float>(mz[1]), std::bit_cast<float>(mz[2])},
                        1.0f / (radius * radius),
                        {Fog::MuzzleColour[0] * intensity * share, Fog::MuzzleColour[1] * intensity * share,
                         Fog::MuzzleColour[2] * intensity * share},
                        Fog::MuzzleSoftM2};
                    vr_fog.grid_flash = true;
                }
            }
        }
    }
    // The slot's light at `world`. A negative signed core requests unshadowed surface lighting.
    const auto light_slot = [&](const std::array<float, 3>& world, const std::array<float, 3>& colour,
                                float radius, float signed_core) {
        const std::array<float, 3> d{world[0] - view[3], world[1] - view[7], world[2] - view[11]};
        u32* const mz = out.data() + Fog::MuzzleDword;
        for (u32 c = 0; c < 3; ++c) {
            mz[c] = std::bit_cast<u32>(view[c] * d[0] + view[4 + c] * d[1] + view[8 + c] * d[2]);
            mz[4 + c] = std::bit_cast<u32>(colour[c]);
        }
        mz[3] = std::bit_cast<u32>(1.0f / (radius * radius));
        mz[7] = std::bit_cast<u32>(signed_core);
    };
    // The Tonitrus charged (tonitrus_charge.h HeadGlow): when no flash shines, a faint blue light
    // at its head, before the torch's glow (the charge's 7 s at most).
    bool slot_lit = vr_fog.muzzle_on;
    std::array<float, 3> head{};
    int head_lit = 0;
    float head_fade = 1.0f;
    if (memory && !slot_lit &&
        VR::TonitrusCharge::HeadGlow.Take(VR::TonitrusCharge::GlowFreshS, head, head_lit, &head_fade)) {
        namespace T = VR::TonitrusCharge;
        const float intensity =
            T::GlowIntensity * std::clamp(head_fade, 0.0f, 1.0f) * VR::Tune::Get("lighting.tonitrus", 1.0f);
        if (intensity > 0.0f) {
            // (Out of the ball, towards the eye: GlowTowardEyeM.)
            const std::array<float, 3> to_eye{view[3] - head[0], view[7] - head[1], view[11] - head[2]};
            const float apart = std::sqrt(to_eye[0] * to_eye[0] + to_eye[1] * to_eye[1] + to_eye[2] * to_eye[2]);
            const float move = std::min(T::GlowTowardEyeM, 0.5f * apart) / std::max(apart, 1e-3f);
            for (u32 c = 0; c < 3; ++c) {
                head[c] += to_eye[c] * move;
            }
            light_slot(head,
                       {T::GlowColour[0] * intensity, T::GlowColour[1] * intensity,
                        T::GlowColour[2] * intensity},
                       std::max(VR::Tune::Get("lighting.tonitrus_radius", T::GlowRadiusM), 0.5f),
                       -T::GlowSoftM2);
            slot_lit = true;
        }
    }
    // The torch held up (torch_glow.h): when no flash shines, a gentle glow at its flame, a share
    // of the game's own torch light (the remembered light nearest the flame) reaching further.
    std::array<float, 3> flame{};
    int flame_level = 0;
    if (memory && !slot_lit && VR::TorchGlow::Take(Fog::TorchFreshS, flame, flame_level)) {
        const VrFogRemembered* torch = nullptr;
        float nearest = Fog::TorchMatchM * Fog::TorchMatchM;
        for (const VrFogRemembered& m : vr_fog.memory) {
            const float d2 = distance2(m.world, flame);
            if (m.seen && m.reach > 0.0f && d2 < nearest) {
                torch = &m;
                nearest = d2;
            }
        }
        if (torch != nullptr) {
            const bool high = flame_level >= 2;
            const float share = (high ? Fog::TorchShareHigh : Fog::TorchShareRaised) *
                                VR::Tune::Get("lighting.torch_glow", 1.0f);
            const float grow = (high ? Fog::TorchRangeHigh : Fog::TorchRangeRaised) - 1.0f;
            const float range = 1.0f + grow * VR::Tune::Get("lighting.torch_range", 1.0f);
            const float radius = std::max(torch->radius * range, 0.5f);
            light_slot(flame, {torch->colour[0] * share, torch->colour[1] * share, torch->colour[2] * share},
                       radius, Fog::TorchSoftM2);
        }
    }
    // Lit smoke (particle_air.h): this frame's strongest lights and the camera's world-to-view
    // rows, for the particle draws after this composite (they only copy it: VrParticleUpload).
    if (memory) {
        namespace Air = Shader::Optimization::ParticleAir;
        static const float strength = [] {
            const char* const env = std::getenv("VR_PARTICLE_AIR_STRENGTH");
            return env ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 4.0f)
                       : Air::Strength;
        }();
        auto& p = vr_fog.particle;
        std::ranges::fill(p, 0u);
        // Not the light in your hand (within NearEyeM: your lantern lit all the mist round you
        // grey, 2026-11-11 pictures): the strongest others, as the air has them.
        u32 lights = 0;
        // (With the sources, air_light.h: a light without a glow of its own lights the smoke
        // too, so the strongest by the smoke's own weight, their colour not times the flame.)
        std::array<std::pair<float, u32>, Fog::MaxLights> by_smoke;
        for (u32 k = 0; k < count; ++k) {
            const Candidate& f = *order[k];
            const float lum = 0.3f * f.colour[0] + 0.6f * f.colour[1] + 0.1f * f.colour[2];
            const float dist = std::sqrt(f.position[0] * f.position[0] + f.position[1] * f.position[1] +
                                         f.position[2] * f.position[2]);
            const float weight = std::max(f.weight, f.smoke);
            by_smoke[k] = {weight * lum / std::sqrt(f.reach) / std::max(dist, 1.0f), k};
        }
        std::sort(by_smoke.begin(), by_smoke.begin() + count,
                  [](const auto& a, const auto& b) { return a.first > b.first; });
        for (u32 j = 0; j < count && lights < Air::MaxLights; ++j) {
            const u32 k = by_smoke[j].second;
            const Candidate& f = *order[k];
            const auto& [x, y, z] = f.position;
            // (Eased in from NearEyeInM to NearEyeOutM: fog_light.h NearEyeEase.)
            const float near_ease = Fog::NearEyeEase(std::sqrt(x * x + y * y + z * z));
            if (near_ease <= 0.0f) {
                continue;
            }
            u32* const to = p.data() + Air::HeaderDwords + Air::LightDwords * lights;
            to[0] = std::bit_cast<u32>(x);
            to[1] = std::bit_cast<u32>(y);
            to[2] = std::bit_cast<u32>(z);
            to[3] = std::bit_cast<u32>(f.reach);
            const auto air = Fog::AirColour(f.colour);
            const float weight = std::max(f.weight, f.smoke) * near_ease;
            for (u32 c = 0; c < 3; ++c) {
                to[4 + c] = std::bit_cast<u32>(weight * air[c]);
            }
            // Its core: a light with a glow of its own (a flame at its place) the usual one, one
            // without the soft one (particle_air.h SoftCoreM).
            const bool flame = f.remembered == nullptr || f.weight > 0.5f * f.smoke;
            const float core = flame ? Air::CoreM : Air::SoftCoreM;
            to[7] = std::bit_cast<u32>(core * core);
            ++lights;
        }
        p[0] = lights;
        p[1] = std::bit_cast<u32>(VR::Tune::Get("lighting.smoke", strength));
        for (u32 r = 0; r < 3; ++r) {
            p[Air::RowsDword + 4 * r + 0] = std::bit_cast<u32>(view[r]);
            p[Air::RowsDword + 4 * r + 1] = std::bit_cast<u32>(view[4 + r]);
            p[Air::RowsDword + 4 * r + 2] = std::bit_cast<u32>(view[8 + r]);
        }
        vr_fog.particle_dwords = Air::HeaderDwords + Air::LightDwords * lights;
        vr_fog.particle_frame = vr_fog.frame;
    }
    return Fog::BufferDwords * sizeof(u32); // (the muzzle block after the lights)
}

static_assert(Shader::Optimization::ParticleAir::BufferDwords == 80, "vr_fog.particle's size");
static_assert(Shader::Optimization::ParticleAir::LightDwords ==
                  Shader::Optimization::FogLight::LightDwords,
              "lit smoke copies the air's light records");

u32 Rasterizer::VrParticleUpload(std::span<u32> out) {
    namespace Air = Shader::Optimization::ParticleAir;
    std::ranges::fill(out, 0u); // no lights: the smoke as the game draws it
    if (VrInMirrorPass()) {
        return Air::HeaderDwords * sizeof(u32);
    }
    std::scoped_lock lock{vr_fog.mutex};
    if (!vr_fog.on || vr_fog.particle_frame != vr_fog.frame || vr_fog.particle_dwords == 0) {
        return Air::HeaderDwords * sizeof(u32);
    }
    std::copy_n(vr_fog.particle.begin(), vr_fog.particle_dwords, out.begin());
    return vr_fog.particle_dwords * sizeof(u32);
}

// The stand-in of a written buffer of ours that is not made yet (`which`: the shadow mask, the
// scene maps, the blocker grid): its own small buffer of zeros. Never a range of the shared stream
// buffer: native stereo gives a buffer a shader may write a right-eye twin, and a twin of the
// stream buffer leaves the right eye reading stale cameras and lights. The air's cells had it on
// 2026-11-14 (the moonlight off); these three had it whenever our ambient occlusion ran without
// the lamp shadows, in multiview and in layers: the right eye another picture in every frame
// (found 2026-11-17, when 0.9.6 made that a default for the smaller cards).
vk::DescriptorBufferInfo Rasterizer::VrWrittenStandIn(u32 which, u64 bytes) {
    constexpr u64 Bytes = 64;
    ASSERT(which < vr_written_none.size() && bytes <= Bytes);
    auto& none = vr_written_none[which];
    if (!none) [[unlikely]] {
        none = std::make_unique<VideoCore::Buffer>(instance, scheduler, VideoCore::MemoryUsage::Stream, 0,
                                                   VideoCore::AllFlags, Bytes);
        std::memset(none->mapped_data.data(), 0, none->mapped_data.size());
    }
    return {none->Handle(), 0, bytes};
}

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

namespace {
// The shadow atlas cells whose draws are not drawn (Rasterizer::ShadowCellSkipped): vr.ini
// shadows_far=reduced (the default) skips the farthest cascade, the bottom-right quarter of the
// 2x2 cascade atlas (its 4x4 cell 3,3: the Sewers' cascades sit at 1,1 / 3,1 / 1,3 / 3,3, by
// VR_DRAW_BREAKDOWN). VR_SHADOW_SKIP_CELLS (tests, read once) names cells instead, "col,row"
// pairs separated by ';' ("none" for none).
const std::optional<u32> g_shadow_skip_cells = []() -> std::optional<u32> {
    const char* value = std::getenv("VR_SHADOW_SKIP_CELLS");
    if (!value) {
        return std::nullopt;
    }
    u32 mask = 0;
    for (const char* p = value; *p;) {
        int col = -1, row = -1;
        if (std::sscanf(p, "%d,%d", &col, &row) == 2 && col >= 0 && col < 4 && row >= 0 && row < 4)
            mask |= 1u << (row * 4 + col);
        p = std::strchr(p, ';');
        if (!p)
            break;
        ++p;
    }
    return mask;
}();
// Fewer shadows when busy (vr.ini shadows_busy): the game frame's draws counted on Stage A before
// any is skipped (FastRender::NoteDraw), judged at each frame end (FlushFrameEnd, an ordered task:
// Stage B's thread when the pipe runs, so the answer is atomic) against shadows_busy_on/_off.
u64 g_busy_frame_start = 0; // FlushFrameEnd's thread only
u32 g_busy_streak = 0;      // frames in a row on the other side of the threshold
std::atomic<bool> g_busy{false};
// Steady, not flickering (a cascade appearing and going is visible): on after 5 busy frames in a
// row, off only after 90 calm ones (a second at 90 Hz).
constexpr u32 BusyOnFrames = 5, BusyOffFrames = 90;
void UpdateBusyShadows() {
    const auto& s = VR::GetSettings();
    const u64 now = FastRender::g_counters.draws.load(std::memory_order_relaxed);
    const u64 draws = now - g_busy_frame_start;
    g_busy_frame_start = now;
    const bool was = g_busy.load(std::memory_order_relaxed);
    const bool other = was ? draws < u64(s.shadows_busy_off) : draws > u64(s.shadows_busy_on);
    g_busy_streak = other ? g_busy_streak + 1 : 0;
    bool busy = was;
    if (!s.shadows_busy) {
        busy = false;
    } else if (g_busy_streak >= (was ? BusyOffFrames : BusyOnFrames)) {
        busy = !was;
        g_busy_streak = 0;
    }
    g_busy.store(busy, std::memory_order_relaxed);
    if (busy != was) {
        static u32 logged = 0;
        if (++logged <= 64 || (logged & 1023) == 0)
            LOG_INFO(Render_Vulkan, "Fewer shadows when busy: {} at {} draws (change #{})",
                     busy ? "on" : "off", draws, logged);
    }
}
} // namespace

// The scene shadow maps (light_shadow.h SceneMaps): made at the first draw that declares them;
// the frame's first draw that writes them (the AO pass) clears them outside its pass, the frame's
// first that reads them (a lamp draw) waits for those writes.
void Rasterizer::VrSceneMapsBeforeDraw(const GraphicsPipeline* pipeline) {
    namespace Scene = Shader::Optimization::LightShadow::SceneMaps;
    // (Neither the maps nor a mask it reads first: a bit worked out once a pipeline, no scan.)
    if (!(pipeline->VrFsBuffers() & (Pipeline::VrFsSceneMaps | Pipeline::VrFsShadowMaskRead)) ||
        !VrSceneMapsOn() || !VR::IsEnabled() || VrInMirrorPass()) {
        return;
    }
    const auto* fs = pipeline->GetStages()[u32(Shader::LogicalStage::Fragment)];
    if (fs == nullptr) {
        return;
    }
    auto it = std::ranges::find(fs->buffers, Shader::BufferType::VrSceneMaps,
                                &Shader::BufferResource::buffer_type);
    if (it == fs->buffers.end()) {
        // (A lamp draw with the mask reads it only: the frame's first waits for the AO pass.)
        it = std::ranges::find(fs->buffers, Shader::BufferType::VrShadowMask,
                               &Shader::BufferResource::buffer_type);
        if (it == fs->buffers.end() || it->is_written) {
            return;
        }
    }
    constexpr u64 bytes = u64(Scene::Texels) * sizeof(u32); // a half: one frame's cubes
    if (!vr_scene.buffer) {
        vr_scene.buffer = std::make_unique<VideoCore::Buffer>(
            instance, scheduler, VideoCore::MemoryUsage::DeviceLocal, 0, VideoCore::AllFlags, 2 * bytes);
        LOG_INFO(Render_Vulkan, "VR scene shadow maps: {} cubes of {}x{} a face, two frames ({} KiB)",
                 Scene::Slots, Scene::Side, Scene::Side, (2 * bytes) >> 10);
    }
    const u64 frame = VR::DrawFrameRecord().id;
    if (it->is_written) {
        if (vr_scene.cleared_frame == frame) {
            return;
        }
        // The mask (the AO pass writes it): made once, for this pass's size (both eyes); a larger
        // pass later writes none (the lamps cast none) rather than past its end.
        // The mask (the AO pass writes it): for this pass's size (both eyes); made anew, larger,
        // when a pass outgrows it (the old one freed once the GPU is past it); never shrunk.
        if (Scene::Mask()) {
            const auto& screen = std::as_const(ScratchDraw::Registers(liverpool)).screen_scissor;
            const u32 pixels = std::min(std::max(screen.GetWidth(), 1u) * std::max(screen.GetHeight(), 1u) * 2u,
                                        Scene::MaskMaxPixels);
            if (!vr_scene.mask || pixels > vr_scene.mask_pixels) {
                if (vr_scene.mask) {
                    scheduler.DeferOperation([old = std::move(vr_scene.mask)]() mutable { old.reset(); });
                }
                const u64 mask_bytes = (u64(Scene::MaskHeader) + u64(pixels) * Scene::MaskWords) * sizeof(u32);
                vr_scene.mask = std::make_unique<VideoCore::Buffer>(
                    instance, scheduler, VideoCore::MemoryUsage::DeviceLocal, 0, VideoCore::AllFlags, mask_bytes);
                vr_scene.mask_pixels = pixels;
                LOG_INFO(Render_Vulkan, "VR shadow mask: {}x{} a eye, {} slots ({} KiB)", screen.GetWidth(),
                         screen.GetHeight(), Scene::Slots, mask_bytes >> 10);
            }
        }
        // Only the slots this frame uses (the shaders never go past its count; a slot taken anew is
        // fresh, its lookups skipped until its cube is this frame's): less to clear on a weak GPU.
        std::scoped_lock lock{vr_fog.mutex};
        const u64 used = std::min<u64>(vr_scene.count, Scene::Slots) * u64(Scene::SlotTexels) * sizeof(u32);
        scheduler.EndRendering();
        NativeStereo::Barrier(scheduler);
        // (The old march in the mask reads no cubes: none cleared.)
        const bool clear = used != 0 && !(Scene::Mask() && Scene::MarchMode() && Scene::TraceAll());
        if (clear) {
            scheduler.Commands().fillBuffer(vr_scene.buffer->Handle(), (frame & 1u) * bytes, used, Scene::Empty);
        }
        // (The second barrier is the clear's: with no clear the two stood back to back with no GPU
        // work between them. VR_OPT_SHADOW_BARRIER=0: both, as before (tests).)
        static const bool one_barrier = [] {
            const char* const env = std::getenv("VR_OPT_SHADOW_BARRIER");
            return env == nullptr || env[0] != '0';
        }();
        if (clear || !one_barrier) {
            NativeStereo::Barrier(scheduler);
        }
        vr_scene.cleared_frame = frame;

    } else {
        if (vr_scene.barrier_frame == frame) {
            return;
        }
        vr_scene.barrier_frame = frame;
        scheduler.EndRendering();
        NativeStereo::Barrier(scheduler);
    }
}

// The depth pyramid (vr_depth_pyramid.h, light_shadow.h SceneMaps::Pyramid): before the frame's
// first AO pass whose program reads it, the depth that pass samples (its image as the draw binds
// it: the same texture cache entry and view, both eyes), outside the render pass.
void Rasterizer::VrPyramidBeforeDraw(const GraphicsPipeline* pipeline) {
    namespace Scene = Shader::Optimization::LightShadow::SceneMaps;
    // (Its fragment stage's buffers: a bit worked out once a pipeline, not a scan every draw.)
    if (!(pipeline->VrFsBuffers() & Pipeline::VrFsDepthPyramid) || !vr_pyramid ||
        !Scene::Pyramid() || !VrSceneMaskOn() || !VR::IsEnabled() || VrInMirrorPass()) {
        return;
    }
    const auto* fs = pipeline->GetStages()[u32(Shader::LogicalStage::Fragment)];
    const u64 frame = VR::DrawFrameRecord().id;
    if (frame == 0 || vr_pyramid_frame == frame) {
        return;
    }
    vr_pyramid_frame = frame;
    vr_pyramid_ready = false;
    const u32 index = fs->vr_light_shadow.depth_image & 0xFFFFu;
    if (index >= fs->images.size()) {
        return;
    }
    const auto& res = fs->images[index];
    const AmdGpu::Image tsharp = res.GetSharp(*fs);
    if (tsharp.Address() == 0 || tsharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid ||
        !memory->IsValidGpuMapping(tsharp.Address(), 0)) {
        return;
    }
    VideoCore::TextureCache::ImageDesc desc = TextureDescFor(tsharp, res);
    VideoCore::ImageId image_id = texture_cache.FindImage(desc);
    auto* image = &texture_cache.GetImage(image_id);
    if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
        image_id = depth_image_id;
        image = &texture_cache.GetImage(image_id);
    }
    auto& view = texture_cache.FindTexture(image_id, desc);
    scheduler.EndRendering();
    TransitTexture(*image, false, desc.view_info.range);
    const vk::ImageView left = *view.image_view;
    const vk::ImageView right = view.native_right ? *view.native_right : left;
    vr_pyramid_ready = vr_pyramid->Record(scheduler.Commands(), left, right,
                                          image->backing->state.layout, tsharp.width + 1,
                                          tsharp.height + 1);
    if constexpr (Common::Log::Enabled) {
        static u32 logged = 0;
        if (logged++ < 3) {
            LOG_INFO(Render_Vulkan, "VR depth pyramid: frame {}: {}x{} (format {}), both eyes {}, {}",
                     frame, tsharp.width + 1, tsharp.height + 1, u32(tsharp.GetDataFmt()),
                     view.native_right ? "apart" : "one view", vr_pyramid_ready ? "built" : "not built");
        }
    }
}

// The lamp shadows' edge pass (vr_shadow_edges.h, light_shadow.h SceneMaps::EdgePass): before the
// frame's first lamp draw whose program reads its bits, the depth that draw samples (both eyes),
// the mask, each eye's inverse projection (the lamp shaders' own matrix, as LightingBuffer last
// remapped it) and the frame's edge lamps as the slot header has them, outside the render pass.
// Anything missing: no pass, serial 0 (VrBodyUpload), the lamps keep the mask's blend.
void Rasterizer::VrEdgesBeforeDraw(const GraphicsPipeline* pipeline) {
    namespace LS = Shader::Optimization::LightShadow;
    namespace Scene = LS::SceneMaps;
    namespace Body = Shader::Optimization::BodyShadow;
    if (!(pipeline->VrFsBuffers() & Pipeline::VrFsShadowEdges) || !vr_edges || !Scene::EdgePass() ||
        !VrSceneMaskOn() || !VR::IsEnabled() || VrInMirrorPass()) {
        return;
    }
    const auto* fs = pipeline->GetStages()[u32(Shader::LogicalStage::Fragment)];
    const u64 frame = VR::DrawFrameRecord().id;
    if (frame == 0 || vr_edges_frame == frame) {
        return;
    }
    // Tried once a frame, before its first lamp draw. A frame whose inputs are not there yet keeps
    // the blend: a later try would split the lamps' pass (the dispatch ends the rendering), and the
    // last pass's bits are up to air_light.h FrameSlack frames old, edges where the head was.
    // (a code review, 2026-11-20, asked for a retry; this is why there is none.)
    vr_edges_frame = frame;
    vr_edges_live = 0;
    // The frame's slots and edge lamps, as the lamp draws' body buffer carries them.
    if (vr_body.frame != frame || !vr_scene.mask) {
        return;
    }
    const u32 sa = Body::SlotsAt;
    const u32 edge_bits = (vr_body.data[sa + 2] >> Scene::EdgeSlotShift) & ((1u << Scene::Slots) - 1u);
    const u32 count = std::min<u32>(vr_body.data[sa], Scene::Slots);
    if (edge_bits == 0 || count == 0) {
        return;
    }
    std::array<std::array<float, 16>, 2> inverse;
    if (!NativeStereo::LightInverseProjections(inverse)) {
        return;
    }
    std::array<std::array<float, 16>, 2> forward;
    for (u32 eye = 0; eye < 2; ++eye) {
        // Gauss-Jordan with partial pivoting: the inverse's inverse (view -> ndc x w, raw depth x w, w).
        std::array<double, 32> m{};
        for (u32 r = 0; r < 4; ++r) {
            for (u32 c = 0; c < 4; ++c) {
                if (!std::isfinite(inverse[eye][4 * r + c])) {
                    return;
                }
                m[8 * r + c] = inverse[eye][4 * r + c];
            }
            m[8 * r + 4 + r] = 1.0;
        }
        for (u32 c = 0; c < 4; ++c) {
            u32 pivot = c;
            for (u32 r = c + 1; r < 4; ++r) {
                if (std::abs(m[8 * r + c]) > std::abs(m[8 * pivot + c])) {
                    pivot = r;
                }
            }
            if (std::abs(m[8 * pivot + c]) < 1e-20) {
                return;
            }
            for (u32 k = 0; k < 8; ++k) {
                std::swap(m[8 * c + k], m[8 * pivot + k]);
            }
            const double d = m[8 * c + c];
            for (u32 k = 0; k < 8; ++k) {
                m[8 * c + k] /= d;
            }
            for (u32 r = 0; r < 4; ++r) {
                if (r != c) {
                    const double f = m[8 * r + c];
                    for (u32 k = 0; k < 8; ++k) {
                        m[8 * r + k] -= f * m[8 * c + k];
                    }
                }
            }
        }
        for (u32 r = 0; r < 4; ++r) {
            for (u32 c = 0; c < 4; ++c) {
                forward[eye][4 * r + c] = static_cast<float>(m[8 * r + 4 + c]);
            }
        }
    }
    // The depth the lamp draw samples (its image as the draw binds it).
    const u32 index = fs->vr_light_shadow.depth_image & 0xFFFFu;
    if (index >= fs->images.size()) {
        return;
    }
    const auto& res = fs->images[index];
    const AmdGpu::Image tsharp = res.GetSharp(*fs);
    if (tsharp.Address() == 0 || tsharp.GetDataFmt() == AmdGpu::DataFormat::FormatInvalid ||
        !memory->IsValidGpuMapping(tsharp.Address(), 0)) {
        return;
    }
    const u32 width = tsharp.width + 1, height = tsharp.height + 1;
    // ndc from the pixel: the draw's viewport, backwards (the air pass's way); a viewport not the
    // picture's size: the plain one.
    const auto& vp = ScratchDraw::Registers(liverpool).viewports[0];
    float ax = 2.0f / float(width), bx = -1.0f, ay = 2.0f / float(height), by = -1.0f;
    if (std::abs(vp.xscale) > 0.5f && std::abs(vp.yscale) > 0.5f &&
        std::abs(2.0f * std::abs(vp.xscale) - float(width)) < 2.0f &&
        std::abs(2.0f * std::abs(vp.yscale) - float(height)) < 2.0f) {
        ax = 1.0f / vp.xscale;
        bx = -vp.xoffset / vp.xscale;
        ay = 1.0f / vp.yscale;
        by = -vp.yoffset / vp.yscale;
    }
    std::array<u32, VrShadowEdges::ParamWords> params{};
    const auto put = [&](u32 at, float v) { params[at] = std::bit_cast<u32>(v); };
    for (u32 eye = 0; eye < 2; ++eye) {
        for (u32 d = 0; d < 16; ++d) {
            put(16 * eye + d, inverse[eye][d]);
            put(32 + 16 * eye + d, forward[eye][d]);
        }
    }
    put(64, ax);
    put(65, bx);
    put(66, ay);
    put(67, by);
    // The edge lamps in slot order (a lamp shader's rank among them), each eye's view of each:
    // its flat view place (the slot's world axes turned back) less the eye's half shift.
    std::array<float, 9> rot;
    for (u32 d = 0; d < 9; ++d) {
        rot[d] = std::bit_cast<float>(vr_body.data[sa + 4 + d]);
    }
    const float half = std::bit_cast<float>(vr_body.data[Body::AttachedAt + 1]);
    u32 lamps = 0;
    const u32 lamp_room = Scene::EdgePassLampCapacity(Scene::EdgeBits());
    for (u32 j = 0; j < count && lamps < lamp_room; ++j) {
        if (((edge_bits >> j) & 1u) == 0) {
            continue;
        }
        const u32 e = sa + Scene::SlotHeader + Scene::SlotDwords * j;
        const float w[3] = {std::bit_cast<float>(vr_body.data[e]), std::bit_cast<float>(vr_body.data[e + 1]),
                            std::bit_cast<float>(vr_body.data[e + 2])};
        float flat[3];
        for (u32 c = 0; c < 3; ++c) {
            flat[c] = rot[c] * w[0] + rot[3 + c] * w[1] + rot[6 + c] * w[2];
        }
        const u32 at = 76 + 12 * lamps;
        put(at + 0, flat[0] + half); // the left eye sits at -half in the flat view
        put(at + 1, flat[1]);
        put(at + 2, flat[2]);
        put(at + 3, flat[0] - half);
        put(at + 4, flat[1]);
        put(at + 5, flat[2]);
        params[at + 6] = vr_body.data[e + 3]; // k: the falloff
        params[at + 7] = vr_body.data[e + 6]; // its fixture's clearance
        params[at + 8] = (vr_body.data[e + 5] & Scene::SlotHis) != 0 ? 1u : 0u;
        params[at + 9] = j;
        ++lamps;
    }
    params[68] = lamps;
    params[69] = Scene::EdgePassSteps();
    put(70, float(Scene::EdgeRange()) - 0.5f);
    put(71, Scene::EdgeFar().first);
    put(72, Scene::EdgeFar().second);
    put(73, Scene::EdgeMinContrast);
    params[74] = Scene::EdgeNoise() == 1 ? 1u : 0u;
    params[75] = vr_scene.mask_pixels;
    put(136, LS::StartM); // (vr_shadow_edges.comp TuneAt: after EdgeMaxLamps lamps)
    put(137, LS::MaxM);
    put(138, LS::BiasM);
    put(139, LS::BiasPerEyeM);
    put(140, LS::BiasPerM);
    put(141, LS::ThickM);
    put(142, LS::ThickPerM);
    params[150] = Scene::EdgeBits(); // (BitsAt: a pixel's bits, 8 or 16)
    // The centre it sharpens (vr_shadow_edges.comp FoveaAt): where the mask pass shades at its full
    // rate, the foveation's inner zone or the mask's own (light_shadow.h MaskFoveaInner), a little
    // inside its border; the eyes' axes as the rate map has them (vk_scheduler.cpp). Foveation off:
    // everywhere (0). The user, 2026-11-20: "sharpen edges only in the centre".
    {
        float radius = 0.0f;
        std::array<float, 2> cx{0.5f, 0.5f}, cy{0.5f, 0.5f};
        if (instance.IsFoveationEnabled() && VR::IsImmersive()) {
            const Foveation::Settings zones =
                instance.IsFoveationFixed()
                    ? instance.GetFoveationSettings()
                    : Foveation::Choose(nullptr, VR::Graphics::foveation.load(std::memory_order_relaxed));
            if (zones.on) {
                radius = std::max(zones.inner, Scene::MaskFoveaInner()) - Scene::EdgeFoveaMargin;
                radius = std::max(radius, 0.05f);
                EyeAxes(VR::GetHead(), cx, cy);
                if (!instance.IsFoveationLayered()) {
                    cx = {0.5f * (cx[0] + cx[1]), 0.5f * (cx[0] + cx[1])};
                    cy = {0.5f * (cy[0] + cy[1]), 0.5f * (cy[0] + cy[1])};
                }
            }
        }
        put(144, cx[0]); // (vr_shadow_edges.comp FoveaAt)
        put(145, cy[0]);
        put(146, cx[1]);
        put(147, cy[1]);
        put(148, radius);
        // Under dynamic resolution the mask's rate map follows the narrower view (vr_foveation.h
        // Map scale, Scheduler::FoveationDrop): the same view share here, so the pass sharpens
        // exactly where the mask was shaded at its full rate (2026-11-24 review).
        const u32 drop = scheduler.FoveationDrop();
        put(149, drop != 0 ? float(100 - drop) / 100.0f : 1.0f);
    }
    VideoCore::TextureCache::ImageDesc desc = TextureDescFor(tsharp, res);
    VideoCore::ImageId image_id = texture_cache.FindImage(desc);
    auto* image = &texture_cache.GetImage(image_id);
    if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
        image_id = depth_image_id;
        image = &texture_cache.GetImage(image_id);
    }
    auto& view = texture_cache.FindTexture(image_id, desc);
    scheduler.EndRendering();
    TransitTexture(*image, false, desc.view_info.range);
    const vk::ImageView left = *view.image_view;
    const vk::ImageView right = view.native_right ? *view.native_right : left;
    const u64 mask_bytes = (u64(Scene::MaskHeader) + u64(vr_scene.mask_pixels) * Scene::MaskWords) * sizeof(u32);
    // (A serial that fits the slot header's bits, never 0.)
    u32 serial = ++vr_edges_counter & Scene::EdgeSerialMask;
    if (serial == 0) {
        serial = ++vr_edges_counter & Scene::EdgeSerialMask;
    }
    // (The parameters and header through the stream buffer: VrShadowEdges::Record.)
    std::array<u32, VrShadowEdges::DataAt> head{};
    VrShadowEdges::FillHead(head, std::span<const u32, VrShadowEdges::ParamWords>{params}, serial, width, height,
                            Scene::EdgeBits());
    auto& stream = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
    const u64 head_offset = stream.Copy(head.data(), sizeof(head), instance.StorageMinAlignment());
    const bool ok = vr_edges->Record(scheduler.Commands(), left, right, image->backing->state.layout, width,
                                     height, vr_scene.mask->Handle(), mask_bytes, stream.Handle(), head_offset,
                                     Scene::EdgeBits());
    vr_edges_live = ok ? serial : 0u;
    if constexpr (Common::Log::Enabled) {
        static u32 logged = 0;
        if (logged++ < 3 || frame % 600 == 0) {
            LOG_INFO(Render_Vulkan,
                     "VR shadow edges: frame {}: {}x{}, {} lamps (bits {:#x}), viewport {} {} {} {}, eyes {}, {}",
                     frame, width, height, lamps, edge_bits, vp.xscale, vp.xoffset, vp.yscale, vp.yoffset,
                     view.native_right ? "apart" : "one view", ok ? "recorded" : "not recorded");
            // (Each edge lamp where the left eye sees it, by the pass's own mapping: tests.)
            for (u32 i = 0; i < lamps; ++i) {
                const u32 at = 76 + 12 * i;
                const float q[4] = {std::bit_cast<float>(params[at]), std::bit_cast<float>(params[at + 1]),
                                    std::bit_cast<float>(params[at + 2]), 1.0f};
                float c[4];
                for (u32 r = 0; r < 4; ++r) {
                    c[r] = forward[0][4 * r] * q[0] + forward[0][4 * r + 1] * q[1] +
                           forward[0][4 * r + 2] * q[2] + forward[0][4 * r + 3] * q[3];
                }
                const float nx = c[0] / c[3], ny = c[1] / c[3];
                LOG_INFO(Render_Vulkan,
                         "VR shadow edges: lamp {} (slot {}) view {:.2f} {:.2f} {:.2f}: left eye pixel {:.0f} {:.0f}, w {:.3f}",
                         i, params[at + 9], q[0], q[1], q[2], (nx - bx) / ax - 0.5f, (ny - by) / ay - 0.5f, c[3]);
            }
        }
    }
}

// The other casters (light_shadow.h SceneMaps::Caster*): this frame's enemies and NPCs near him
// (VR::RecordShadowCasters) on to the world's axes from this frame's camera; a body whose sphere
// lies in view stays out (the screen splat has its real outline).
const std::array<u32, 452>& Rasterizer::VrShadowCastersUpload() {
    namespace Scene = Shader::Optimization::LightShadow::SceneMaps;
    static_assert(Scene::CasterDwords == 452 && VR::ShadowCasters::MaxCapsules <= Scene::CasterCapsules &&
                  VR::ShadowCasters::MaxBodies <= Scene::CasterBodies);
    // (Without the lamp shadows nothing reads them: none, as made, no upkeep. 2026-11-17.)
    if (!VrSceneMaskOn()) {
        return vr_scene.casters;
    }
    const VR::CameraRecord rec = VR::DrawFrameRecord();
    if (vr_scene.casters_frame == rec.id) {
        return vr_scene.casters;
    }
    vr_scene.casters_frame = rec.id;
    const VrShadowCpuScope timed{3};
    vr_scene.casters.fill(0u);
    VR::ShadowCasters in;
    std::array<float, 12> m{};
    {
        std::scoped_lock lock{vr_fog.mutex};
        if (vr_fog.view_frame == 0) {
            return vr_scene.casters;
        }
        m = vr_fog.view;
    }
    if (!VR::RecordShadowCasters(rec.id, in)) {
        return vr_scene.casters;
    }
    const auto rel = [&](const VR::Vec3& v) {
        return std::array<float, 3>{v.x - m[3], v.y - m[7], v.z - m[11]};
    };
    auto& out = vr_scene.casters;
    const auto put = [&](u32 at, float v) { out[at] = std::bit_cast<u32>(v); };
    u32 bodies = 0, capsules = 0;
    for (u32 b = 0; b < std::min(in.body_count, Scene::CasterBodies); ++b) {
        const auto range = in.bodies[b];
        if (range.count == 0 || range.first + range.count > in.capsule_count) {
            continue;
        }
        // Its sphere: round its capsules' ends.
        float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
        for (u32 c = range.first; c < range.first + range.count; ++c) {
            const auto& cap = in.capsules[c];
            for (const auto& e : {rel(cap.a), rel(cap.b)}) {
                for (u32 i = 0; i < 3; ++i) {
                    lo[i] = std::min(lo[i], e[i] - cap.radius);
                    hi[i] = std::max(hi[i], e[i] + cap.radius);
                }
            }
        }
        const float centre[3] = {0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), 0.5f * (lo[2] + hi[2])};
        const float radius = 0.5f * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) +
                                              (hi[2] - lo[2]) * (hi[2] - lo[2]));
        if (!std::isfinite(radius) || radius > 20.0f) {
            continue;
        }
        // In view (the flat view's +z ahead; 50 degrees each way, the sphere wholly inside): left out.
        float f[3];
        for (u32 c = 0; c < 3; ++c) {
            f[c] = m[c] * centre[0] + m[4 + c] * centre[1] + m[8 + c] * centre[2];
        }
        // In view by the eyes' own field of view (each axis' narrower side: an edge case counts as
        // out of view, where a capsule shadow beats none).
        float tx = 10.0f, ty = 10.0f;
        for (const auto& fov : rec.eye_fov) {
            tx = std::min({tx, std::tan(std::abs(fov.angle_left)), std::tan(std::abs(fov.angle_right))});
            ty = std::min({ty, std::tan(std::abs(fov.angle_up)), std::tan(std::abs(fov.angle_down))});
        }
        if (!(tx > 0.1f && tx < 10.0f) || !(ty > 0.1f && ty < 10.0f)) {
            tx = ty = 1.0f; // (no field of view: 45 degrees each way)
        }
        const float ahead = f[2] - radius;
        const bool in_view = ahead > 0.0f && std::abs(f[0]) + radius < ahead * tx && std::abs(f[1]) + radius < ahead * ty;
        // (In view: its sphere only, flagged, for the grid to leave it out; no capsules.)
        const u32 first = capsules;
        (void)in_view; // (in view too: their capsules sit inside their outline, which the screen casts)
        for (u32 c = range.first; c < range.first + range.count && capsules < Scene::CasterCapsules; ++c) {
            const auto& cap = in.capsules[c];
            const auto a = rel(cap.a), bb = rel(cap.b);
            const u32 at = Scene::CasterCapsulesAt + 8 * capsules++;
            put(at, a[0]);
            put(at + 1, a[1]);
            put(at + 2, a[2]);
            put(at + 3, cap.radius);
            put(at + 4, bb[0]);
            put(at + 5, bb[1]);
            put(at + 6, bb[2]);
        }
        const u32 at = Scene::CasterBodiesAt + 8 * bodies++;
        put(at, centre[0]);
        put(at + 1, centre[1]);
        put(at + 2, centre[2]);
        put(at + 3, radius);
        out[at + 4] = first;
        out[at + 5] = capsules - first;
        out[at + 6] = 0u;
    }
    out[0] = capsules;
    out[1] = bodies;
    if constexpr (Common::Log::Enabled) {
        // (Tests: who casts, every 300 frames.)
        static u32 logged = 0;
        if ((logged++ % 300) == 0) {
            std::string line;
            for (u32 b = 0; b < bodies; ++b) {
                const u32 at = Scene::CasterBodiesAt + 8 * b;
                const auto g = [&](u32 i) { return std::bit_cast<float>(out[at + i]); };
                // (... and where it is in the camera's view: right of it, ahead of it.)
                const auto view = [&](u32 c) { return m[c] * g(0) + m[4 + c] * g(1) + m[8 + c] * g(2); };
                line += fmt::format(" | {} caps at {:.1f} {:.1f} {:.1f} r {:.1f} (right {:.1f} ahead {:.1f})", out[at + 5], g(0), g(1),
                                    g(2), g(3), view(0), view(2));
            }
            LOG_INFO(Render_Vulkan, "VR shadow casters: {} bodies, {} capsules{}", bodies, capsules, line);
        }
    }
    {
        float tx = 10.0f, ty = 10.0f;
        for (const auto& fov : rec.eye_fov) {
            tx = std::min({tx, std::tan(std::abs(fov.angle_left)), std::tan(std::abs(fov.angle_right))});
            ty = std::min({ty, std::tan(std::abs(fov.angle_up)), std::tan(std::abs(fov.angle_down))});
        }
        if (!(tx > 0.1f && tx < 10.0f) || !(ty > 0.1f && ty < 10.0f)) {
            tx = ty = 0.0f; // (none: the grid's voxels all cast)
        }
        put(2, tx);
        put(3, ty);
    }
    return vr_scene.casters;
}

// The game's lamp shadow maps (2026-11-12 night: "skip them when our shadows are on"):
// a game-shadowed lamp shader of ours reads them as lit (light_shadow_pass.cpp), so the draws
// into them (the per-face clear, any scenery, the blur) are skipped once that shader has bound
// them. One our pass did not take never notes its maps: they are drawn as ever.
void Rasterizer::NoteGameLampMap(VAddr address) {
    std::scoped_lock lock{vr_lamp_maps_mutex};
    if (std::ranges::find(vr_lamp_maps, address) == vr_lamp_maps.end() && vr_lamp_maps.size() < 64) {
        vr_lamp_maps.push_back(address);
        vr_lamp_map_count.store(static_cast<u32>(vr_lamp_maps.size()), std::memory_order_relaxed);
        LOG_INFO(Render_Vulkan, "VR lamp shadows: the game's lamp shadow map at {:#x} is no longer "
                                "drawn ({} known)",
                 address, vr_lamp_maps.size());
    }
}

bool Rasterizer::GameLampMapSkipped() {
    // VR_LAMP_MAP_SKIP=0 (tests): drawn as ever.
    static const bool on = [] {
        const char* const env = std::getenv("VR_LAMP_MAP_SKIP");
        return env == nullptr || env[0] != '0';
    }();
    if (!on || vr_lamp_map_count.load(std::memory_order_relaxed) == 0) {
        return false;
    }
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    const auto& cb = regs.color_buffers[0];
    if (!cb) {
        return false;
    }
    const VAddr address = cb.Address();
    bool skip;
    {
        std::scoped_lock lock{vr_lamp_maps_mutex};
        skip = std::ranges::find(vr_lamp_maps, address) != vr_lamp_maps.end();
    }
    if constexpr (Common::Log::Enabled) {
        if (skip && ++vr_lamp_maps_skipped % 20000 == 0) {
            LOG_INFO(Render_Vulkan, "VR lamp shadows: {} draws into the game's lamp shadow maps skipped",
                     vr_lamp_maps_skipped);
        }
    }
    return skip;
}

bool Rasterizer::ShadowCellSkipped(bool can_confine) {
    shadow_near_a = 0;
    if constexpr (Common::Log::Enabled) {
        // Test diagnostics (VR_SHADOW_CELL_LOG): every square 2048..8192 depth target's draws by
        // the 4x4 cell of their viewport centre, with the viewport's size and the depth clears
        // among them, logged every 20000 such draws: the atlas layout per area, and whether a
        // skipped cell is cleared by the game.
        static const bool diag = std::getenv("VR_SHADOW_CELL_LOG") != nullptr;
        if (diag) {
            const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
            const auto& db = regs.depth_buffer;
            const u32 w = db.Pitch(), h = db.Height();
            if (db.DepthValid() && w >= 2048 && w <= 8192 && w == h) {
                struct Cell { u64 draws{}, clears{}; float half_w{}, half_h{}; u64 target{}; };
                static std::array<Cell, 16> cells{};
                static u64 total = 0;
                const auto& vp = regs.viewports[0];
                const int col = std::clamp(int(vp.xoffset * 4.0f / w), 0, 3);
                const int row = std::clamp(int(vp.yoffset * 4.0f / h), 0, 3);
                auto& c = cells[row * 4 + col];
                ++c.draws;
                c.clears += regs.depth_render_control.depth_clear_enable ? 1 : 0;
                c.half_w = vp.xscale;
                c.half_h = vp.yscale;
                c.target = db.DepthAddress();
                if (++total % 20000 == 0) {
                    std::string text;
                    for (int i = 0; i < 16; ++i) {
                        if (cells[i].draws)
                            text += fmt::format(" [{},{}: {} draws, {} clears, vp {:.0f}x{:.0f}, "
                                                "target {:#x}]",
                                                i % 4, i / 4, cells[i].draws, cells[i].clears,
                                                2 * cells[i].half_w, 2 * std::abs(cells[i].half_h),
                                                cells[i].target);
                    }
                    LOG_INFO(Render_Vulkan, "Shadow cell diag ({}x{}):{}", w, h, text);
                    cells = {};
                }
            }
        }
    }
    // The atlas held over frames (vr_shadow_hold.h; a test switch): every draw into it left out.
    if (ShadowHold::Active()) [[unlikely]] {
        ShadowHold::NoCell();
        const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
        const auto& db = regs.depth_buffer;
        if (db.DepthValid()) {
            const u32 width = db.Pitch(), height = db.Height();
            if (width >= 2048 && width <= 8192 && width == height) {
                const auto& hold_vp = regs.viewports[0];
                const u32 hold_cell = u32(std::clamp(int(hold_vp.yoffset * 4.0f / height), 0, 3)) * 4u +
                                      u32(std::clamp(int(hold_vp.xoffset * 4.0f / width), 0, 3));
                const bool whole_atlas = 2.0f * std::abs(hold_vp.xscale) >= 0.99f * float(width);
                const auto hold_verdict = ShadowHold::SkipDraw(whole_atlas, hold_cell);
                if (hold_verdict != ShadowHold::Verdict::Skip && !whole_atlas && regs.viewport_control.xoffset_enable &&
                    regs.viewport_control.xscale_enable && regs.viewport_control.yoffset_enable &&
                    regs.viewport_control.yscale_enable && !regs.IsClipDisabled()) {
                    ShadowHold::CellDraw(hold_cell, {hold_vp.xoffset, hold_vp.xscale, hold_vp.yoffset, hold_vp.yscale}, width);
                }
                switch (hold_verdict) {
                case ShadowHold::Verdict::Skip:
                    return true;
                case ShadowHold::Verdict::NearClear:
                    // (Every draw into a near cascade of a held frame, direct or indirect. Until
                    // 2026-11-18 only the first direct one had the mark: one not recorded after
                    // all, its pipeline not ready or left out by the VR draw rules, took it along,
                    // and the next such draw cleared the whole atlas, the held cascades with it.)
                    shadow_near_a = ShadowHold::HeldFrame();
                    break;
                default:
                    break;
                }
            }
        }
    }
    // Busy frames (shadows_busy): the farthest cascade and the next one, cells 3,3 and 1,3.
    const u32 cells = g_shadow_skip_cells ? *g_shadow_skip_cells
                      : g_busy.load(std::memory_order_relaxed) ? (1u << 15) | (1u << 13)
                      : VR::GetSettings().shadows_far_reduced ? (1u << 15)
                                                              : 0u;
    if (cells == 0) {
        return false;
    }
    // Stage A, registers only: a square depth target of 2048..8192 (a shadow atlas) and the
    // viewport centre's cell of its 4x4 grid.
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    const auto& db = regs.depth_buffer;
    if (!db.DepthValid()) {
        return false;
    }
    const u32 width = db.Pitch(), height = db.Height();
    if (width < 2048 || width > 8192 || width != height) {
        return false;
    }
    const auto& vp = regs.viewports[0];
    const int col = std::clamp(int(vp.xoffset * 4.0f / width), 0, 3);
    const int row = std::clamp(int(vp.yoffset * 4.0f / height), 0, 3);
    return (cells >> (row * 4 + col)) & 1;
}

namespace {
// Water reflections off (vr.ini water_reflections=0):
// the game's planar water reflection is the scene mirrored below the water plane, drawn into its
// own half-size target by the only draws that clip (distances its vertex shaders export,
// PA_CL_VS_OUT_CNTL, or the user clip planes, PA_CL_CLIP_CNTL): Old Yharnam ~120-200 a frame
// (~8% of stage B), the sewers 10-25. The frame's scene target gets a few clipping draws too (1-4
// a frame among ~100 others) and keeps them: only a target whose draws are mostly clipping ones
// (at least 8, more than twice its others, counts halved as they grow) loses them; its other
// draws stay. sRGB colour targets never count: the game paints a bloodied character's blood into
// its own 8-bit sRGB texture (256 or 512 square) with clipping draws too (the
// GXFlver_*_Bloody_*_Pnt shaders); taken for reflections, those were left out and cleared every
// frame, and the Bloody shaders then drew the character black or flat grey (players with water
// reflections off, 2026-11-04). The reflection itself is 512x576, R16G16B16A16 float in Old
// Yharnam and R8G8B8A8 unorm in the sewers. Stage A's thread only (Draw, DrawIndirect).
class WaterTargets {
public:
    bool LeaveOut(u64 address, bool clip) {
        // Only targets that took a clipping draw are kept (the scene target and the reflections:
        // a frame draws into many more targets than the table holds).
        Target* t = Find(address, clip);
        if (!t) {
            return false;
        }
        (clip ? t->clip : t->other)++;
        if (t->clip + t->other >= 4096) {
            t->clip /= 2;
            t->other /= 2;
        }
        const bool reflection = t->clip >= 8 && t->clip > 2 * t->other;
        if (reflection != t->reflection) {
            t->reflection = reflection;
            static u32 logged = 0;
            if (++logged <= 32 || (logged & 1023) == 0)
                LOG_INFO(Render_Vulkan,
                         "Water reflections off: target {:#x} {} ({} clipping draws, {} others)",
                         address, reflection ? "is a reflection, its clipping draws left out"
                                             : "is drawn again",
                         t->clip, t->other);
        }
        return clip && reflection;
    }

private:
    struct Target {
        u64 address{};
        u32 clip{};
        u32 other{};
        bool reflection{};
    };
    Target* Find(u64 address, bool add) {
        Target* fewest = &targets[0];
        for (auto& t : targets) {
            if (t.address == address)
                return &t;
            if (t.clip + t.other < fewest->clip + fewest->other)
                fewest = &t;
        }
        if (!add)
            return nullptr;
        *fewest = Target{.address = address}; // the least used one makes way
        return fewest;
    }
    std::array<Target, 8> targets{};
};
WaterTargets g_water_targets;
} // namespace

namespace {
// Water reflections on, capped (2026-11-11: "cap them to 92 draws max?", then "72!!", then 64 once actors stay outside it; "64
// draws as the cap"): the planar
// reflection's cost is its draws' CPU work (~5 us each; Old Yharnam ~120-200 a frame, the sewers
// 10-25), not the GPU's. A frame keeps at most `cap` of them, the biggest first: each draw's
// index count goes to a histogram (powers of two), and the last frame's histogram sets this
// frame's floor, the smallest size whose draws and every bigger one fit the cap; draws below it
// are left out, and once `cap` are kept, the rest too. The sky dome, walls, buildings and big props
// stay (and cover the target: the game draws it whole every frame, so nothing stale is left
// where a small prop was); small clutter, which the ripples blur anyway, goes. The sewers stay
// whole. An indirect draw (its count unknown) counts as the biggest. No clear: the kept draws
// cover the target as the game's do. VR_WATER_DRAWS=<cap> (0: every draw).
class WaterBudget {
public:
    bool Keep(u32 indices, u64 frame, u32 cap) {
        if (frame != frame_) {
            // The floor bucket: the biggest ones all fit, and of the floor's own only as many as
            // are left of the cap (`boundary_`), so the cap is filled, biggest first.
            u32 sum = 0;
            floor_ = 0;
            boundary_ = cap;
            for (int b = 32; b >= 0; --b) {
                if (sum + hist_[b] > cap) {
                    floor_ = u32(b);
                    boundary_ = cap - sum;
                    break;
                }
                sum += hist_[b];
            }
            last_total_ = total_;
            last_dropped_ = dropped_;
            hist_.fill(0);
            kept_ = dropped_ = total_ = in_floor_ = 0;
            frame_ = frame;
        }
        const u32 bucket = u32(std::bit_width(indices)); // 0..32
        ++hist_[bucket];
        ++total_;
        const bool fits = bucket > floor_ || (bucket == floor_ && in_floor_ < boundary_);
        if (!fits || kept_ >= cap) {
            ++dropped_;
            return false;
        }
        in_floor_ += bucket == floor_ ? 1u : 0u;
        ++kept_;
        return true;
    }
    u32 Floor() const {
        return floor_;
    }
    u32 LastTotal() const {
        return last_total_;
    }
    u32 LastDropped() const {
        return last_dropped_;
    }

private:
    std::array<u32, 33> hist_{};
    u32 floor_{}, boundary_{~0u}, in_floor_{}, kept_{}, dropped_{}, total_{}, last_total_{},
        last_dropped_{};
    u64 frame_{~0ull};
};
WaterBudget g_water_budget;
} // namespace

// A character's draw (its vertex stage reads a bone palette of 640 bytes or more:
// vk_rasterizer_temporal.cpp HasCharacterSkeleton's test, Motion::ClassifyBuffer).
static bool WaterCharacterDraw(const GraphicsPipeline* pipeline) {
    const auto& vs = pipeline->GetStage(Shader::LogicalStage::Vertex);
    for (const auto& resource : vs.buffers) {
        if (resource.IsSpecial()) {
            continue;
        }
        const auto buffer = resource.GetSharp(vs);
        const u32 size = buffer.Valid() ? u32(buffer.GetSize()) : 0u;
        if (buffer.GetStride() == 16 && size >= 640 && size != 864 && size <= 16384) {
            return true;
        }
    }
    return false;
}

bool Rasterizer::WaterReflectionCapped(u32 indices, const GraphicsPipeline* pipeline) {
    static const u32 cap = [] {
        const char* const env = std::getenv("VR_WATER_DRAWS");
        return env ? u32(std::strtoul(env, nullptr, 10)) : 64u; // the tester, 2026-11-11: 64 (actors outside it)
    }();
    const u32 live_cap = static_cast<u32>(VR::Tune::Get("water.cap", static_cast<float>(cap)));
    if (!VR::GetSettings().water_reflections || live_cap == 0) {
        return false;
    }
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    const auto& cb0 = regs.color_buffers[0];
    if (!cb0 || (cb0.GetNumberFmt() == AmdGpu::NumberFormat::Srgb && FastRender::AbOn(11))) {
        return false; // an sRGB target is never the reflection (the blood paint is)
    }
    const bool clip = regs.vs_output_control.clip_distance_enable != 0 ||
                      regs.clipper_control.user_clip_plane_enable != 0;
    if (!g_water_targets.LeaveOut(cb0.Address(), clip)) {
        return false; // not a reflection's clipping draw
    }
    // Enemies, NPCs and hunters wading always stay in the water (2026-11-11: "make
    // sure actors (enemies and npcs) are included"), outside the budget.
    static std::atomic<u64> actor_draws{0};
    if (WaterCharacterDraw(pipeline)) {
        actor_draws.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    const bool keep = g_water_budget.Keep(indices, VR::DrawFrameRecord().id, live_cap);
    if constexpr (Common::Log::Enabled) {
        static u32 frames = 0;
        static u64 last = 0;
        const u64 id = VR::DrawFrameRecord().id;
        if (id != last) {
            last = id;
            if (++frames % 900 == 1) { // about every 10 s
                LOG_INFO(Render_Vulkan,
                         "Water reflections capped at {}: last frame {} reflection draws, {} left "
                         "out ({} character draws kept so far, outside the cap); this frame's floor "
                         "{} indices",
                         cap, g_water_budget.LastTotal(), g_water_budget.LastDropped(),
                         actor_draws.load(std::memory_order_relaxed),
                         g_water_budget.Floor() == 0 ? 0u : (1u << (g_water_budget.Floor() - 1)));
            }
        }
    }
    return !keep;
}

bool Rasterizer::WaterReflectionSkipped() {
    if (VR::GetSettings().water_reflections) {
        return false;
    }
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    const auto& cb0 = regs.color_buffers[0];
    if (!cb0 || (cb0.GetNumberFmt() == AmdGpu::NumberFormat::Srgb && FastRender::AbOn(11))) {
        return false; // an sRGB target is never the reflection (the blood paint is)
    }
    const bool clip = regs.vs_output_control.clip_distance_enable != 0 ||
                      regs.clipper_control.user_clip_plane_enable != 0;
    const bool left_out = g_water_targets.LeaveOut(cb0.Address(), clip);
    if constexpr (Common::Log::Enabled) {
        // Test diagnostics (VR_WATER_SKIP_LOG): each target and shader pair left out, once, with
        // the target's size and format, to tell the planar reflection from other clipping draws.
        static const bool diag = std::getenv("VR_WATER_SKIP_LOG") != nullptr;
        if (diag && left_out) {
            static std::unordered_set<u64> seen;
            const auto hash_of = [&](u32 stage) -> u64 {
                const auto* pgm = regs.ProgramForStage(stage);
                return pgm && pgm->Address<u32*>()
                           ? AmdGpu::SearchBinaryInfo(pgm->Address<u32*>()).shader_hash
                           : 0;
            };
            const u64 vs = hash_of(static_cast<u32>(Shader::Stage::Vertex));
            const u64 ps = hash_of(static_cast<u32>(Shader::Stage::Fragment));
            if (seen.size() < 512 && seen.insert(cb0.Address() ^ (vs << 20) ^ (ps << 40)).second) {
                const auto& db = regs.depth_buffer;
                LOG_INFO(Render_Vulkan,
                         "Water skip diag: target {:#x} {}x{} slices {} fmt {}/{} vs {:#x} ps {:#x} "
                         "depth {} {}x{}",
                         cb0.Address(), cb0.Pitch(), cb0.Height(), cb0.NumSlices(),
                         u32(cb0.GetDataFmt()), u32(cb0.GetNumberFmt()), vs, ps,
                         db.DepthValid() ? "yes" : "no", db.Pitch(), db.Height());
            }
        }
    }
    return left_out;
}

namespace {
// Water reflections off: the frame number on Stage B's thread (FlushFrameEnd) and the frame each
// reflection target was last cleared in.
u64 g_water_frame = 1;
std::array<std::pair<u64, u64>, 4> g_water_cleared{}; // target address, frame
} // namespace

// A left-out reflection draw: the frame's first one into its target clears it (to 0, both eyes).
// The game never clears the reflection (no fast clear: its draws cover it every frame), so with
// them left out the water read whatever was drawn last, the first frame's few draws, frozen.
// Stage B (the draw's registers).
void Rasterizer::WaterReflectionClear() {
    static u64 left_out = 0, cleared = 0;
    ++left_out;
    const auto& col_buf = std::as_const(ScratchDraw::Registers(liverpool)).color_buffers[0];
    if (!col_buf) {
        return;
    }
    const u64 address = col_buf.Address();
    auto* slot = &g_water_cleared[0];
    for (auto& entry : g_water_cleared) {
        if (entry.first == address) {
            slot = &entry;
            break;
        }
        if (entry.second < slot->second) {
            slot = &entry; // the one cleared longest ago makes way
        }
    }
    if (slot->first == address && slot->second == g_water_frame) {
        return;
    }
    *slot = {address, g_water_frame};
    VideoCore::TextureCache::ImageDesc desc(col_buf,
                                            std::as_const(ScratchDraw::ColorExtents(liverpool))[0]);
    const auto image_id = texture_cache.FindImage(desc);
    texture_cache.FindRenderTarget(image_id, desc);
    texture_cache.GetImage(image_id).Clear(vk::ClearValue{}, desc.view_info.range);
    if (++cleared <= 4 || (cleared & 0xFFF) == 0) {
        LOG_INFO(Render_Vulkan,
                 "Water reflections off: target {:#x} cleared ({} reflection draws left out, {} "
                 "clears)",
                 address, left_out, cleared);
    }
}

void Rasterizer::DrawRecord(const GraphicsPipeline* pipeline, bool is_indexed, u32 index_offset) {
    // VR_FRAME_MATCH: the recorder packets this draw commits, at any return.
    // With FAST_B_PROFILE: the draw's own stage B ticks by category (start and end snapshots).
    struct FrameMatchBlob {
        const Scheduler* scheduler;
        u64 from;
        std::array<u64, FastRender::BProfile::Count> start{};
        static std::array<u64, FastRender::BProfile::Count> Snapshot() {
            auto ticks = FastRender::BProfile::state.ticks;
            if (FastRender::BProfile::state.active)
                ticks[FastRender::BProfile::state.current] +=
                    Common::FencedRDTSC() - FastRender::BProfile::state.last;
            return ticks;
        }
        ~FrameMatchBlob() {
            if (!scheduler)
                return;
            std::array<u64, FastRender::BProfile::Count> spent{};
            if (FastRender::BProfile::Enabled()) {
                const auto end = Snapshot();
                for (size_t i = 0; i < spent.size(); ++i)
                    spent[i] = end[i] - start[i];
            }
            FrameMatch::Get().Blob(scheduler->RecorderView(), from, spent.data(), spent.size());
        }
    } frame_match_blob{FrameMatch::Enabled() ? &scheduler : nullptr,
                       FrameMatch::Enabled() ? scheduler.RecorderView().WritePosition() : 0};
    // VR_DRAW_CENSUS: this draw's stage B cycles, added to its pass shape at any return.
    struct CensusTicks {
        u64 start;
        ~CensusTicks() {
            if (start)
                DrawCensus::Get().AddTicks(Common::FencedRDTSC() - start);
        }
    } census_ticks{DrawCensus::Enabled() ? Common::FencedRDTSC() : 0};
    if (FrameMatch::Enabled() && FastRender::BProfile::Enabled())
        frame_match_blob.start = FrameMatchBlob::Snapshot();
    FastRender::BProfile::Scope b_detail{FastRender::BProfile::DrawSetup};
    EyeSpecVariants::g_draw_mask.pipeline = nullptr; // VR_EYE_SPEC: only this draw's own binding
    recording_view_mask = pipeline->GetGraphicsKey().probe_view_mask;
    const auto& regs = ScratchDraw::Registers(liverpool);
    AssertRecordingOwner();
    // AbOn(12): the bind memo entries BindTextures will look up for this draw's stages (their keys
    // made on A), asked for now; the render state and the buffers come first.
    if (binding_packet && binding_packet->resources_prepared &&
        pipeline == binding_packet->selected && FastRender::AbOn(12) &&
        PerfMemo::g_switches.Get().bind_memo) {
        for (size_t i = 0; i < binding_packet->stage_count; ++i) {
            const auto& prepared = binding_packet->resources[i];
            if (prepared.keyed)
                bind_memo.Prefetch(prepared.key, prepared.key_hash);
        }
    }
    if (vr_warp && pipeline->VrFsBuffers() != 0) {
        VrBodyMapsBeforeDraw(pipeline);
        VrShadowGridBeforeDraw(pipeline);
        VrSceneMapsBeforeDraw(pipeline);
        VrFogGridBeforeDraw(pipeline);
        VrAirBeforeDraw(pipeline);
        VrPyramidBeforeDraw(pipeline);
        VrEdgesBeforeDraw(pipeline);
    }
    // FAST_CPU_STAGEB_SPLIT stage 1: what this draw resolves (targets, resources,
    // barriers, uploads) records into a blob, appended to the ring before the submit check below.
    Scheduler::ResolveBlob resolve_blob{scheduler};
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
    if (FrameMatch::Enabled()) {
        std::array<u32, AmdGpu::NUM_COLOR_BUFFERS + 1> targets{};
        for (u32 cb = 0; cb < AmdGpu::NUM_COLOR_BUFFERS; ++cb)
            targets[cb] = cb_descs[cb].first.index;
        targets.back() = db_desc.first.index;
        FrameMatch::Get().Note(pipeline, pipeline->GetStages(), targets.data(), targets.size());
    }
    repeat.allowed = true; // FAST_CPU_REPEAT_DRAW
    if (!BindResources(pipeline)) {
        return;
    }
    auto state = BeginRendering(pipeline);
    if (S1Drs::enabled) drs.Target(state.width,state.height,state.probe_view_mask);
    if (DrawCensus::Enabled()) [[unlikely]] {
        // The shadow atlas cell as ShadowCellSkipped finds it (a square 2048..8192 depth target).
        int cell = -1;
        if (regs.depth_buffer.DepthValid()) {
            const u32 w = regs.depth_buffer.Pitch(), h = regs.depth_buffer.Height();
            if (w >= 2048 && w <= 8192 && w == h) {
                const auto& vp = regs.viewports[0];
                cell = std::clamp(int(vp.yoffset * 4.0f / h), 0, 3) * 4 +
                       std::clamp(int(vp.xoffset * 4.0f / w), 0, 3);
            }
        }
        DrawCensus::Get().Note(state.width, state.height, state.num_color_attachments,
                               state.depth_stencil_attachment.image_view != vk::ImageView{},
                               state.probe_view_mask, cell,
                               [&](Shader::LogicalStage want) -> u64 {
                                   for (const auto* stage : pipeline->GetStages())
                                       if (stage && stage->l_stage == want)
                                           return stage->pgm_hash; // the guest program's hash
                                   return 0;
                               }(Shader::LogicalStage::Vertex),
                               [&](Shader::LogicalStage want) -> u64 {
                                   for (const auto* stage : pipeline->GetStages())
                                       if (stage && stage->l_stage == want)
                                           return stage->pgm_hash;
                                   return 0;
                               }(Shader::LogicalStage::Fragment));
    }
    VrNativeAudit(pipeline, &state);
    if (vr_temporal_enabled)
        pipeline = VrTemporalSelect(pipeline, state);
    // FAST_CPU_STAGEB_SPLIT=2 (stage 3): a plain draw's encode half goes to fast:StageB2 as the
    // slot's job, its blob kept open for the barriers and the pass begin (stage B owns the pass).
    // Draws with VR extras of their own stay here: their first use of the command stream waits
    // until B2 has caught up (Scheduler::SplitSync).
    SplitDrawJob* const job = [&]() -> SplitDrawJob* {
        if (!scheduler.SplitThreaded() || GpuPassProfiler::For(scheduler) != nullptr)
            return nullptr;
        if (vr_shadow_near_clear && vr_shadow_near_serial != vr_shadow_near_done)
            return nullptr; // (its clear goes into this pass, before its draw)
        if (vr_warp && vr_aa_action != 0)
            return nullptr;
        if (vr_ui_layer && vr_ui.armed && !vr_ui.incomplete)
            return nullptr;
        if (vr_warp && is_indexed &&
            (probe_selected || recording_vr.hands_split || recording_vr.body_split ||
             recording_vr.split))
            return nullptr;
        return &split_jobs[scheduler.SplitSlot()];
    }();
    // Before anything of this draw is bound into the command buffer (vertex buffers, descriptors,
    // dynamic state): its transitions and uploads so far go with the submitted part.
    if (!job) {
        resolve_blob.Close(); // FAST_CPU_STAGEB_SPLIT: the resolve part ends here
    }
    MaybeSubmitMidFrame(state);
    // VR aa_mode=smaa: the game's FXAA draw, its images bound, replaced by SMAA.
    if (vr_warp && vr_aa_action != 0 && VrAaReplace(pipeline, state)) {
        ResetBindings();
        return;
    }
    // VR UI layer: this UI colour draw is also drawn into K and W (before the game's own); their
    // depth/stencil twin is made ready before this draw's binds.
    const bool ui_shadow = vr_ui_layer && VrUiPrepare(pipeline, state);
    if (ui_shadow) {
        VrUiDepthBefore(pipeline, state);
    }
    const bool ui_mask = vr_ui_layer && VrUiMaskPrepare(pipeline, state, ui_shadow);

    u64 motion_geometry{};
    // FAST_CPU_STAGEB_SPLIT stage 2: the binds resolved into split_binds, then encoded from it
    // (=2: into the job, encoded on B2).
    Vulkan::VertexBinds* const binds =
        job ? &job->binds : scheduler.StageBSplit() ? &split_binds : nullptr;
    if (binds) {
        binds->Clear();
    }
    // FAST_CPU_VERTEX_STREAM (buffer_cache.cpp): what a non-indexed draw fetches, its vkCmdDraw
    // arguments below (the split job's and the UI layer's shadow and mask draws repeat them).
    std::optional<VideoCore::VertexFetch> fetch;
    if (!is_indexed) {
        const auto [first_vertex, first_instance] = GetDrawOffsets(
            regs, pipeline->GetStage(Shader::LogicalStage::Vertex), pipeline->GetFetchShader());
        fetch = VideoCore::VertexFetch{first_vertex, regs.num_indices, first_instance,
                                       regs.num_instances.NumInstances()};
    }
    buffer_cache.BindVertexBuffers(*pipeline, buffer_barriers,
                                   pipeline->HasVrMotion() ? &motion_geometry : nullptr,
                                   binding_packet && binding_packet->vertex_prepared &&
                                           binding_packet->selected == pipeline
                                       ? &binding_packet->vertex_view : nullptr,
                                   binds, fetch ? &*fetch : nullptr);
    if (is_indexed) {
        buffer_cache.BindIndexBuffer(index_offset, buffer_barriers, binds);
    }
    if (binds && !job) {
        VideoCore::BufferCache::EncodeVertexBinds(*binds, scheduler.Commands());
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
                    .5f * NativeStereo::Ipd(r) * r.world_scale / (VrHalfTan(r.vfov) * r.aspect);
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
    if (pipeline->HasVrCapture()) {
        // Body maps (vr_body_maps.h): this draw of his stores its positions; a draw the frame
        // cannot take reads the disabled entry 0 (it stores nothing) of the same buffers.
        const u64 frame = VR::DrawFrameRecord().id;
        u32 param = 0;
        u64 params = 0, positions = 0;
        VrBodyMaps::Draw draw = recording_vr.capture_draw;
        draw.vertex_offset = vertex_offset;
        draw.first_instance = instance_offset;
        if (!recording_vr.capture ||
            !vr_body_maps->AddDraw(frame, draw, param, params, positions)) {
            vr_body_maps->Disabled(params, positions);
            param = 0;
        }
        push_data.vr_hud = Shader::VrMotion::PackPush(u32(push_data.vr_hud), param);
        vr_push.motion_params = params;
        vr_push.motion_positions = positions;
    } else if (recording_vr.capture_missed) {
        vr_body_maps->Missed(VR::DrawFrameRecord().id);
    }
    if (pipeline->HasVrMotion()) {
        FastRender::BProfile::Scope b_motion{FastRender::BProfile::RecMotion};
        const u32 motion = VrTemporalPrepareDraw(pipeline, motion_geometry, is_indexed,
                                                 index_offset, vertex_offset, instance_offset);
        push_data.vr_hud = Shader::VrMotion::PackPush(u32(push_data.vr_hud), motion);
    }
    if (Shader::VrCut::IsHone(recording_vr.cut_code)) {
        const u32 code = recording_vr.cut_code;
        // HonedSteel parts 2 and 3 are the two Rakuyo blades.
        const float polish_per_step = (code & 2u) ? (.60f / 255.f) : (.52f / 255.f);
        push_data.xoffset = float((code >> 16) & 255u) * polish_per_step;
        const u32 band = code >> 24;
        push_data.yoffset = band == 255 ? -2.f : float(band) / 254.f;
    } else if (recording_vr.cut_code == Shader::VrCut::BladeMouth) {
        push_data.xoffset = recording_vr.blade_clip.offset;
        push_data.yoffset = std::bit_cast<float>(recording_vr.blade_clip.normal);
    }
    vr_push.scene_inverse = vr_scene_inverse;
    if (job) {
        // =2: the pairing into the job; its barriers end the pass here, the rest goes to B2.
        pipeline->PairResources(set_writes, buffer_barriers, NativeHandles(), nullptr,
                                job->resources);
        pipeline->EncodeBarriers(job->resources.barriers);
        job->resources.barriers.clear();
        job->push_data = push_data;
        job->vr_push = vr_push;
    } else if (scheduler.StageBSplit()) {
        // FAST_CPU_STAGEB_SPLIT stage 2: the pairing into an owned copy, then the encode from it.
        pipeline->PairResources(set_writes, buffer_barriers, NativeHandles(), nullptr,
                                split_resources);
        pipeline->EncodeResources(split_resources, push_data, vr_push);
    } else {
        pipeline->BindResources(set_writes, buffer_barriers, push_data, vr_push, NativeHandles(),
                                nullptr, {buffer_infos.data(), buffer_infos.size()},
                                {image_infos.data(), image_infos.size()});
    }
    // VR_EYE_SPEC: this draw's shared buffer bindings, as its own pairing just made them (the job
    // and split paths pair elsewhere: they bind the base pipeline).
    const vk::Pipeline eye_handle =
        EyeSpecVariants::Bound(pipeline, static_cast<const Pipeline*>(pipeline));
    UpdateDynamicState(pipeline, is_indexed, false, job ? &job->dynamic : nullptr);
    NoteEyeDraws(pipeline, push_data.vr_hud);

    if (vr_warp && is_indexed && probe_selected) {
        FastRender::BProfile::Scope b_extra{FastRender::BProfile::RecVrExtra};
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
        FastRender::BProfile::Scope b_extra{FastRender::BProfile::RecVrExtra};
        VrUiShadowDraw(pipeline, state, VrDrawCall::Of(issue), false, ui_mask);
    }
    if (ui_mask && !ui_shadow) {
        FastRender::BProfile::Scope b_extra{FastRender::BProfile::RecVrExtra};
        VrUiMaskReplay(pipeline, state, VrDrawCall::Of(issue));
    }
    if (SkipNoopDraw(pipeline, state, is_indexed, ui_shadow)) {
        // Bit 54: no pass, pipeline or draw for a draw that cannot write memory.
        DebugState.IncDrawCall();
        if (vr_warp) {
            VrAfterDraw(pipeline, state);
        }
        ResetBindings();
        repeat.valid = false;
        return;
    }
    if (GpuPassProfiler* const profiler = GpuPassProfiler::For(scheduler);
        profiler != nullptr && scheduler.WillBeginRendering(state)) [[unlikely]] {
        LabelGpuPass(*profiler, pipeline, state); // FAST_GPU_PASS_PROFILE, bit 38
    }
    {
        // The lamp shadows' mask pass (the game's AO pass) at a finer rate than the foveation's
        // (light_shadow.h MaskFoveaInner); every other pass as the foveation has it.
        const auto* const mask_fs = pipeline->GetStages()[static_cast<size_t>(Shader::LogicalStage::Fragment)];
        // (... and the two half-size passes that make the depth it reads, the linear depth
        // 0x01ae5388 and the depth shrink 0xe820ee9d: traced finely over depth drawn in the
        // foveation's blocks, every surface in the ring was a staircase that shadowed itself,
        // a dark ring from the foveation's edge outward. The user saw it in a test run, 2026-11-15.)
        const bool mask_pass =
            mask_fs != nullptr && VrSceneMaskOn() &&
            (Shader::Optimization::AmbientOcclusion::IsAoProgram(mask_fs->pgm_hash) ||
             mask_fs->pgm_hash == 0x01ae5388u || mask_fs->pgm_hash == 0xe820ee9du);
        // (Live key lighting.shadow_fovea, in steps of 0.02: each value is a rate map of its own.)
        float mask_floor = 0.0f;
        if (mask_pass) {
            mask_floor = std::round(std::clamp(VR::Tune::Get("lighting.shadow_fovea",
                                                             Shader::Optimization::LightShadow::SceneMaps::MaskFoveaInner()),
                                               0.0f, 4.0f) *
                                    50.0f) /
                         50.0f;
        }
        scheduler.SetFoveationInnerFloor(mask_floor);
    }
    scheduler.BeginRendering(state);
    VrShadowNearClear(state, regs.depth_clear);
    if (GpuCrash::CheckpointsOn()) [[unlikely]] {
        const auto& hashes = pipeline->GetGraphicsKey().stage_hashes;
        GpuCrash::NoteDraw(hashes[static_cast<size_t>(Shader::LogicalStage::Vertex)],
                           hashes[static_cast<size_t>(Shader::LogicalStage::Fragment)]);
    }
    if (BatchCensus::Enabled()) [[unlikely]] {
        NoteBatchCensus(pipeline, state, is_indexed, index_offset);
    }

    // FAST_GPU_PASS_PROFILE_DRAWS=1: each lamp draw's own GPU time (its shader and its number
    // among that shader's draws this frame).
    if (GpuPassProfiler* const profiler = GpuPassProfiler::For(scheduler);
        profiler != nullptr && GpuPassProfiler::DrawsOn()) [[unlikely]] {
        const auto* const fs_info = pipeline->GetStages()[static_cast<size_t>(
            Shader::LogicalStage::Fragment)];
        const u64 ps = fs_info != nullptr ? fs_info->pgm_hash : 0;
        const auto layout = NativeProjection::LightListLayout(ps);
        if (fs_info != nullptr && layout.kind != NativeProjection::LightKind::None) {
            if (lamp_draw_frame != profiler->FrameNo()) {
                lamp_draw_frame = profiler->FrameNo();
                lamp_draw_seen.clear();
            }
            const u32 nth = lamp_draw_seen[ps]++;
            const u64 key = GpuPassProfiler::Mix(GpuPassProfiler::Mix(0x1A4D'0000'0001ull, ps), nth);
            const u32 views = std::max(1, std::popcount(u32(state.probe_view_mask)));
            profiler->StampDraw(key, u32(views), GpuPassProfiler::Group::Scene, [&] {
                return fmt::format("lamp draw ps {:08x} ({} {} light(s)) #{}", ps,
                                   NativeProjection::Name(layout.kind), layout.count, nth);
            });
        }
    }
    if (job) {
        // =2: the pass begin went into the blob; B2 binds and draws (SplitEncode).
        job->pipeline = pipeline;
        job->handle = pipeline->Handle();
        job->indexed = is_indexed;
        job->count = regs.num_indices;
        job->instances = regs.num_instances.NumInstances();
        job->vertex_offset = u32(vertex_offset);
        job->instance_offset = instance_offset;
        scheduler.SplitSubmit(true);
    } else if (is_indexed) {
        const auto cmdbuf = scheduler.Commands();
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, eye_handle);
        if (!vr_warp || !VrDrawCapeSplit(cmdbuf, regs.num_instances.NumInstances(),
                                         s32(vertex_offset), instance_offset)) {
            // VR_EXP_DRAW_SPLIT=1 (GPU timing experiment only): a G-buffer triangle list drawn as
            // two draws of its halves, same state: the GPU's cost of one more draw.
            static const bool exp_split = [] {
                const char* v = std::getenv("VR_EXP_DRAW_SPLIT");
                return v && v[0] == '1';
            }();
            const u32 count = regs.num_indices;
            const bool split = exp_split && state.num_color_attachments == 6 && count >= 6 &&
                               regs.primitive_type == AmdGpu::PrimitiveType::TriangleList;
            if (exp_split && state.num_color_attachments == 6) {
                static u64 split_draws = 0, kept_draws = 0;
                ++(split ? split_draws : kept_draws);
                if (((split_draws + kept_draws) & 0xFFFF) == 0)
                    LOG_INFO(Render_Vulkan,
                             "VR_EXP_DRAW_SPLIT: {} G-buffer draws split, {} not (primitive {})",
                             split_draws, kept_draws, static_cast<u32>(regs.primitive_type));
            }
            DrawEyes(cmdbuf, eye_draws, [&] {
                if (split) {
                    const u32 half = count / 6 * 3;
                    cmdbuf.drawIndexed(half, regs.num_instances.NumInstances(), 0,
                                       s32(vertex_offset), instance_offset);
                    cmdbuf.drawIndexed(count - half, regs.num_instances.NumInstances(), half,
                                       s32(vertex_offset), instance_offset);
                    return;
                }
                cmdbuf.drawIndexed(regs.num_indices, regs.num_instances.NumInstances(), 0,
                                   s32(vertex_offset), instance_offset);
            });
        }
    } else {
        const auto cmdbuf = scheduler.Commands();
        cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics, eye_handle);
        DrawEyes(cmdbuf, eye_draws, [&] {
            cmdbuf.draw(regs.num_indices, regs.num_instances.NumInstances(), vertex_offset,
                        instance_offset);
        });
    }
    if (shadow_after) {
        FastRender::BProfile::Scope b_extra{FastRender::BProfile::RecVrExtra};
        VrUiShadowDraw(pipeline, state, VrDrawCall::Of(issue), true, ui_mask);
    }
    DebugState.IncDrawCall();
    if (vr_warp) {
        FastRender::BProfile::Scope b_extra{FastRender::BProfile::RecVrExtra};
        VrAfterDraw(pipeline, state);
    }

    ResetBindings();
    // FAST_CPU_REPEAT_DRAW: what this draw recorded is the next draw's starting point; anything
    // recorded before the next draw's BindResources makes it miss.
    repeat.uses_end = scheduler.CommandUses();
}

// VR_BATCH_CENSUS (vr_batch_census.h): this draw's identity at each batching level, right after its
// pass begin (the bindings, push data and dynamic state are this draw's).
void Rasterizer::NoteBatchCensus(const GraphicsPipeline* pipeline, const RenderState& state,
                                 bool is_indexed, u32 index_offset) {
    using BatchCensus::Mix;
    using BatchCensus::MixBytes;
    const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
    BatchCensus::DrawKey k;
    k.pass = scheduler.PassBegins();
    k.pipeline = reinterpret_cast<u64>(static_cast<VkPipeline>(pipeline->Handle()));
    u64 material = k.pipeline;
    for (const auto& info : image_infos) {
        material = Mix(material, reinterpret_cast<u64>(static_cast<VkSampler>(info.sampler)));
        material = Mix(material, reinterpret_cast<u64>(static_cast<VkImageView>(info.imageView)));
        material = Mix(material, u64(info.imageLayout));
    }
    k.material = material;
    u64 buffers = 0, offsets = 0;
    for (const auto& info : buffer_infos) {
        buffers = Mix(buffers, reinterpret_cast<u64>(static_cast<VkBuffer>(info.buffer)));
        buffers = Mix(buffers, info.range);
        offsets = Mix(offsets, info.offset);
    }
    k.buffers = buffers;
    k.offsets = offsets;
    const auto& d = scheduler.GetDynamicState();
    u64 dyn = 0;
    for (const auto& v : d.viewports)
        dyn = MixBytes(dyn, &v, sizeof(v));
    for (const auto& s : d.scissors)
        dyn = MixBytes(dyn, &s, sizeof(s));
    const std::array<u64, 12> flags{
        u64(d.depth_test_enabled) | u64(d.depth_write_enabled) << 1 |
            u64(d.depth_bounds_test_enabled) << 2 | u64(d.depth_bias_enabled) << 3 |
            u64(d.stencil_test_enabled) << 4 | u64(d.primitive_restart_enable) << 5,
        u64(d.depth_compare_op),
        std::bit_cast<u32>(d.depth_bias_constant) | u64(std::bit_cast<u32>(d.depth_bias_slope)) << 32,
        u64(d.stencil_front_ops.compare_op) | u64(d.stencil_front_ops.pass_op) << 8 |
            u64(d.stencil_front_ops.fail_op) << 16 | u64(d.stencil_front_ops.depth_fail_op) << 24,
        u64(d.stencil_front_reference) | u64(d.stencil_front_write_mask) << 32,
        u64(d.stencil_front_compare_mask) | u64(d.stencil_back_reference) << 32,
        u64(d.stencil_back_write_mask) | u64(d.stencil_back_compare_mask) << 32,
        u64(d.stencil_back_ops.compare_op) | u64(d.stencil_back_ops.pass_op) << 8 |
            u64(d.stencil_back_ops.fail_op) << 16 | u64(d.stencil_back_ops.depth_fail_op) << 24,
        u64(static_cast<VkCullModeFlags>(d.cull_mode)) | u64(d.front_face) << 8,
        std::bit_cast<u32>(d.blend_constants[0]) | u64(std::bit_cast<u32>(d.blend_constants[1])) << 32,
        std::bit_cast<u32>(d.blend_constants[2]) | u64(std::bit_cast<u32>(d.blend_constants[3])) << 32,
        std::bit_cast<u32>(d.depth_bounds_min) | u64(std::bit_cast<u32>(d.depth_bounds_max)) << 32};
    dyn = MixBytes(dyn, flags.data(), sizeof(flags));
    for (const auto& mask : d.color_write_masks)
        dyn = Mix(dyn, u64(static_cast<VkColorComponentFlags>(mask)));
    k.dynamic = dyn;
    k.user_data = MixBytes(0, push_data.ud_regs.data(), sizeof(push_data.ud_regs));
    k.index_type = is_indexed ? 1 + static_cast<u32>(regs.index_buffer_type.index_type) : 0;
    k.index_addr = is_indexed ? regs.index_base_address.Address() + index_offset : 0;
    k.instances = regs.num_instances.NumInstances();
    k.indices = regs.num_indices;
    k.gbuffer = state.num_color_attachments == 6;
    BatchCensus::Get().Note(k);
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
        if (!IsRegularDraw() || ShadowCellSkipped() || GameLampMapSkipped()) {
            RecordPacket({}, nullptr, PacketKind::FilteredDraw);
            return;
        }
        if (WaterReflectionSkipped()) {
            // Stage B clears its target once a frame (WaterReflectionClear).
            RecordPacket({}, nullptr, PacketKind::FilteredDraw, false, 0, WaterReflectionMark);
            return;
        }
    } else if (!FilterDraw() || ShadowCellSkipped() || GameLampMapSkipped()) {
        return;
    } else if (WaterReflectionSkipped()) {
        WaterReflectionClear();
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
    if (WaterReflectionCapped(~0u, pipeline))
        return; // over the water reflection's budget
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
        vr_shadow_near_clear = shadow_near_a != 0;
        vr_shadow_near_serial = shadow_near_a;
        DrawIndirectRecord(pipeline, is_indexed, arg_address, offset, stride, max_count,
                           count_address);
        vr_shadow_near_clear = false;
    }
}

// The shadow atlas held over frames (vr_shadow_hold.h): the first draw into a near cascade that
// a held frame really records clears the near cascades' part of the atlas (its top half, or the
// top left quarter with one near cascade), in its own pass, just begun.
void Rasterizer::VrShadowNearClear(const RenderState& state, float depth) {
    if (!vr_shadow_near_clear || vr_shadow_near_serial == vr_shadow_near_done ||
        state.depth_stencil_attachment.image_view == vk::ImageView{}) [[likely]] {
        return;
    }
    vr_shadow_near_done = vr_shadow_near_serial;
    vk::ClearAttachment near_clear{.aspectMask = vk::ImageAspectFlagBits::eDepth};
    near_clear.clearValue.depthStencil = vk::ClearDepthStencilValue{.depth = depth, .stencil = 0};
    const vk::ClearRect near_rect{
        .rect = vk::Rect2D{vk::Offset2D{0, 0},
                           vk::Extent2D{static_cast<u32>(ShadowHold::NearTiles() >= 2 ? state.width : state.width / 2),
                                        static_cast<u32>(state.height / 2)}},
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    scheduler.Commands().clearAttachments(near_clear, near_rect);
}

void Rasterizer::DrawIndirectRecord(const GraphicsPipeline* pipeline, bool is_indexed,
                                    VAddr arg_address, u32 offset, u32 stride, u32 max_count,
                                    VAddr count_address) {
    FastRender::BProfile::Scope b_detail{FastRender::BProfile::DrawSetup};
    EyeSpecVariants::g_draw_mask.pipeline = nullptr; // VR_EYE_SPEC: only this draw's own binding
    recording_view_mask = pipeline->GetGraphicsKey().probe_view_mask;
    AssertRecordingOwner();
    if (vr_warp) {
        VrBodyMapsBeforeDraw(pipeline);
        VrShadowGridBeforeDraw(pipeline);
        VrSceneMapsBeforeDraw(pipeline);
        VrFogGridBeforeDraw(pipeline);
        VrAirBeforeDraw(pipeline);
        VrPyramidBeforeDraw(pipeline);
        VrEdgesBeforeDraw(pipeline);
    }
    // FAST_CPU_STAGEB_SPLIT stage 1: what this draw resolves (targets, resources,
    // barriers, uploads) records into a blob, appended to the ring before the submit check below.
    Scheduler::ResolveBlob resolve_blob{scheduler};
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
    if (S1Drs::enabled) drs.Target(state.width,state.height,state.probe_view_mask);
    VrNativeAudit(pipeline, &state);
    resolve_blob.Close(); // FAST_CPU_STAGEB_SPLIT: the resolve part ends here
    MaybeSubmitMidFrame(state);
    // VR UI layer: this UI colour draw is also drawn into K and W (before the game's own); their
    // depth/stencil twin is made ready before this draw's binds.
    const bool ui_shadow = vr_ui_layer && VrUiPrepare(pipeline, state);
    if (ui_shadow) {
        VrUiDepthBefore(pipeline, state);
    }
    const bool ui_mask = vr_ui_layer && VrUiMaskPrepare(pipeline, state, ui_shadow);

    Vulkan::VertexBinds* const binds = scheduler.StageBSplit() ? &split_binds : nullptr;
    if (binds) {
        binds->Clear();
    }
    buffer_cache.BindVertexBuffers(*pipeline, buffer_barriers, nullptr,
                                   binding_packet && binding_packet->vertex_prepared &&
                                           binding_packet->selected == pipeline
                                       ? &binding_packet->vertex_view : nullptr,
                                   binds);
    if (is_indexed) {
        buffer_cache.BindIndexBuffer(0, buffer_barriers, binds);
    }
    if (binds) {
        VideoCore::BufferCache::EncodeVertexBinds(*binds, scheduler.Commands());
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
                    .5f * NativeStereo::Ipd(r) * r.world_scale / (VrHalfTan(r.vfov) * r.aspect);
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
    if (scheduler.StageBSplit()) {
        // FAST_CPU_STAGEB_SPLIT stage 2: the pairing into an owned copy, then the encode from it.
        pipeline->PairResources(set_writes, buffer_barriers, NativeHandles(), nullptr,
                                split_resources);
        pipeline->EncodeResources(split_resources, push_data, vr_push);
    } else {
        pipeline->BindResources(set_writes, buffer_barriers, push_data, vr_push, NativeHandles(),
                                nullptr, {buffer_infos.data(), buffer_infos.size()},
                                {image_infos.data(), image_infos.size()});
    }
    UpdateDynamicState(pipeline, is_indexed, true);
    NoteEyeDraws(pipeline, push_data.vr_hud);
    if (ui_shadow || ui_mask) {
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
        if (ui_shadow) {
            VrUiShadowDraw(pipeline, state, VrDrawCall::Of(issue), false, ui_mask);
        } else {
            VrUiMaskReplay(pipeline, state, VrDrawCall::Of(issue));
        }
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
    VrShadowNearClear(state, std::as_const(ScratchDraw::Registers(liverpool)).depth_clear);
    if (GpuCrash::CheckpointsOn()) [[unlikely]] {
        const auto& hashes = pipeline->GetGraphicsKey().stage_hashes;
        GpuCrash::NoteDraw(hashes[static_cast<size_t>(Shader::LogicalStage::Vertex)],
                           hashes[static_cast<size_t>(Shader::LogicalStage::Fragment)]);
    }

    // We can safely ignore both SGPR UD indices and results of fetch shader parsing, as vertex and
    // instance offsets will be automatically applied by Vulkan from indirect args buffer.

    const auto cmdbuf = scheduler.Commands();
    cmdbuf.bindPipeline(vk::PipelineBindPoint::eGraphics,
                        EyeSpecVariants::Bound(pipeline, static_cast<const Pipeline*>(pipeline)));

    if (is_indexed) {
        ASSERT(sizeof(VkDrawIndexedIndirectCommand) == stride);

        DrawEyes(cmdbuf, eye_draws, [&] {
            if (count_address != 0) {
                cmdbuf.drawIndexedIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                                count_base, max_count, stride);
            } else {
                cmdbuf.drawIndexedIndirect(buffer->Handle(), base, max_count, stride);
            }
        });
        DebugState.IncDrawCall();
    } else {
        ASSERT(sizeof(VkDrawIndirectCommand) == stride);

        DrawEyes(cmdbuf, eye_draws, [&] {
            if (count_address != 0) {
                cmdbuf.drawIndirectCount(buffer->Handle(), base, count_buffer->Handle(),
                                         count_base, max_count, stride);
            } else {
                cmdbuf.drawIndirect(buffer->Handle(), base, max_count, stride);
            }
        });
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
        // (The lamp shadows' two hosts, for the tools: the game's AO pass (the shadow mask's
        // march) and a pass that begins with a lamp shader's draw (the lamp draws).)
        const char* const tag =
            Shader::Optimization::AmbientOcclusion::IsAoProgram(ps_hash) ? " {AO pass}"
            : ps != nullptr && ps->vr_light_shadow.on && ps->vr_light_shadow.count != 0
                ? " {lamp draws}"
                : "";
        return fmt::format("pass {}x{}{} [{}] depth {} vs {:08x} ps {:08x}{}", state.width,
                           state.height, state.probe_view_mask != 0 ? " multiview" : "", colors,
                           depth != vk::Format::eUndefined ? vk::to_string(depth)
                                                           : std::string{"-"},
                           vs_hash, ps_hash, tag);
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
    check.collect = check.reason == Reason::Taken &&
                    (EyeOverlap::VerifyPeriod() != 0 || MergedEyes::VerifyPeriod() != 0);
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
    std::unordered_set<u64> seen, merged_seen; // VR_MERGED_EYES=v samples on its own
    u64 taken{}, merged_taken{};
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
bool Rasterizer::EyeOverlapVerify(const Shader::Info& cs, const RecordEye& record_eye,
                                  const std::function<void()>* merged) {
    const u32 period = merged ? MergedEyes::VerifyPeriod() : EyeOverlap::VerifyPeriod();
    if (period == 0) {
        return false;
    }
    auto& state = EyeOverlapVerifyStateOf();
    {
        // The first taken dispatch of every shader, then every period-th.
        std::scoped_lock lock{state.sample_mutex};
        auto& seen = merged ? state.merged_seen : state.seen;
        auto& taken = merged ? state.merged_taken : state.taken;
        if (!seen.insert(cs.pgm_hash).second && ++taken % period != 0) {
            return false;
        }
    }
    auto& skipped = merged ? MergedEyes::g_stats.verify_skipped
                           : EyeOverlap::g_stats.verify_skipped;
    const auto skip = [&skipped] {
        skipped.fetch_add(1, std::memory_order_relaxed);
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
    if (merged) {
        (*merged)(); // both eyes in one launch
    } else {
        record_eye(1); // swapped: right, barrier, left
        barrier();
        record_eye(0);
    }
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
                              total, hash = cs.pgm_hash, is_merged = merged != nullptr] {
        auto& verify_skipped = is_merged ? MergedEyes::g_stats.verify_skipped
                                         : EyeOverlap::g_stats.verify_skipped;
        auto& verify_ok =
            is_merged ? MergedEyes::g_stats.verify_ok : EyeOverlap::g_stats.verify_ok;
        auto& verify_mismatch = is_merged ? MergedEyes::g_stats.verify_mismatch
                                          : EyeOverlap::g_stats.verify_mismatch;
        auto& in_flight = EyeOverlapVerifyStateOf().in_flight;
        if (a->mapped_data.size() < total || b->mapped_data.size() < total) {
            verify_skipped.fetch_add(1, std::memory_order_relaxed);
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
            verify_ok.fetch_add(1, std::memory_order_relaxed);
        } else {
            const u64 n = verify_mismatch.fetch_add(1, std::memory_order_relaxed) + 1;
            if (n <= 16 || n % 256 == 0) {
                const auto& span = spans[first];
                LOG_ERROR(Render_Vulkan,
                          "{} verify: cs {:08x}: {} bytes differ between the two launches and "
                          "{}, first in target {}/{} ({}) at byte {}, mismatch #{}",
                          is_merged ? "VR merged eyes" : "FAST dispatch_eye_overlap", hash,
                          differing, is_merged ? "the merged one" : "the swapped eye order",
                          first, spans.size(),
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
    if (VrLogDispatch(pipeline, false)) {
        return; // the draw inspector's `skipcs`
    }
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
                            eye_overlap, {buffer_infos.data(), buffer_infos.size()},
                            {image_infos.data(), image_infos.size()});
    // VR_DISPATCH_MONO=d: whether both eyes of this dispatch do the same work (set_writes now
    // holds the paired writes).
    if (DispatchMono::Mode() == 'd' && NativeStereo::Enabled()) [[unlikely]] {
        DispatchMono::Get().Note(cs.pgm_hash, cs_program.dim_x * cs_program.dim_y * cs_program.dim_z,
                                 DispatchMono::Classify(cs, pipeline->NativeWritableBindings(),
                                                        set_writes));
    }

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
    // VR_MERGED_EYES (vr_merged_eyes.h): a taken pair of one group deep, without screen
    // rectangles, in one launch once its variant is built (until then: asked for, two launches).
    vk::Pipeline merged_handle{};
    if (MergedEyes::Variant* const merged = eyes_overlap ? pipeline->Merged() : nullptr;
        merged && cs_program.dim_z == 1 && vr_rect_groups[0] == 0 && vr_rect_groups[1] == 0 &&
        !cs.vr_screen_rect.Valid()) {
        const auto state = merged->state.load(std::memory_order_acquire);
        if (state == MergedEyes::State::Ready) {
            merged_handle = *merged->pipeline;
        } else if (state == MergedEyes::State::None) {
            auto none = MergedEyes::State::None;
            merged->state.compare_exchange_strong(none, MergedEyes::State::Wanted,
                                                  std::memory_order_acq_rel);
        }
        if (state != MergedEyes::State::Failed) {
            MergedEyes::NoteDispatch(bool(merged_handle));
        }
    }
    if (merged_handle) {
        // Its layout is defined like the pipeline's: the descriptors and push constants
        // recorded with pipeline->GetLayout() serve it. The eye is the group's z (vr_hud unread).
        const std::function<void()> record_merged = [&] {
            cmdbuf.bindPipeline(vk::PipelineBindPoint::eCompute, merged_handle);
            ScratchPush::Invalidate();
            cmdbuf.pushConstants(pipeline->GetLayout(), vk::ShaderStageFlagBits::eCompute, 0u,
                                 sizeof(push_data), &push_data);
            cmdbuf.dispatch(dim_x, dim_y, 2u);
        };
        if (!EyeOverlapVerify(cs, record_eye, &record_merged)) {
            record_merged();
        }
        NativeStereo::Barrier(scheduler);
    } else if (eyes_overlap && EyeOverlapVerify(cs, record_eye)) {
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
    if (VrLogDispatch(pipeline, true)) {
        return; // the draw inspector's `skipcs`
    }
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
                            eye_overlap, {buffer_infos.data(), buffer_infos.size()},
                            {image_infos.data(), image_infos.size()});

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

// The lamp shadows' buffers given back while they are off (the 2026-11-21 review: switched to
// the game's, their 40 to 50 MB stayed until a restart): once they have been off ReleaseFrames
// frames (a quick switch back keeps them), the scene maps, the mask, the blocker grid, the depth
// pyramid and the edges, each freed once the GPU is past the frames that used them (Scheduler::
// DeferOperation); made again at their first use, as on the first switch. The body maps stay (some
// 1.5 MB, and their mesh pool is shared by the draws that filled it). The recording thread, where
// they are made.
void Rasterizer::VrLampShadowsRelease() {
    constexpr u32 ReleaseFrames = 300; // some 3 s at 90 frames a second
    if (VrLampShadowsOn()) {
        vr_lamps_off_frames = 0;
        return;
    }
    if (vr_lamps_off_frames > ReleaseFrames || ++vr_lamps_off_frames <= ReleaseFrames) {
        return;
    }
    u64 freed = 0;
    if (vr_scene.buffer) {
        freed += vr_scene.buffer->SizeBytes();
        scheduler.DeferOperation([old = std::move(vr_scene.buffer)]() mutable { old.reset(); });
    }
    if (vr_scene.mask) {
        freed += vr_scene.mask->SizeBytes();
        scheduler.DeferOperation([old = std::move(vr_scene.mask)]() mutable { old.reset(); });
        vr_scene.mask_pixels = 0;
    }
    if (vr_grid_buffer) {
        freed += vr_grid_buffer->SizeBytes();
        scheduler.DeferOperation([old = std::move(vr_grid_buffer)]() mutable { old.reset(); });
        vr_grid_ready = false;
        vr_grid_header = ~0ull;
    }
    if (vr_pyramid) {
        freed += vr_pyramid->Bytes();
        vr_pyramid->Release();
        vr_pyramid_ready = false; // (its binding takes the stand-in)
    }
    if (vr_edges) {
        freed += vr_edges->Bytes();
        vr_edges->Release();
    }
    vr_scene.cleared_frame = ~0ull;
    vr_scene.barrier_frame = ~0ull;
    if (freed != 0) {
        LOG_INFO(Render_Vulkan, "VR lamp shadows: off for {} frames, their buffers given back ({} KiB)",
                 ReleaseFrames, freed >> 10);
    }
}

void Rasterizer::FlushFrameEnd() {
    // The graphics settings in force for the next frame (vk_pipeline_cache.h VrLatchGraphics: the
    // lamp shadows switch live, the menu's row or a draw-inspector `set`).
    VrLatchGraphics();
    VrLampShadowsRelease();
    if (FrameMatch::Enabled())
        FrameMatch::Get().FrameEnd();
    if (DrawCensus::Enabled())
        DrawCensus::Get().FrameEnd();
    if (BatchCensus::Enabled())
        BatchCensus::Get().FrameEnd();
    if (DsAb::Alternating())
        DsAb::Get().FrameEnd();
    buffer_cache.RetileFrameEnd(); // VR_RETILE_DEFER=m
    UpdateBusyShadows();
    ++g_water_frame; // Water reflections off: each target cleared once a frame
    DrainDrawPipe(DrawPipe::RasterizerEntry);
    scheduler.FlushFrameEnd();
    frame_end_tick = scheduler.CurrentTick() - 1; // DeferGuestFlush: this frame's last submitted
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

// A/B switch 3 (FAST_GUEST_FLUSH_DEFER=0 off; 2026-11-02): Liverpool's flushes at the end of a
// guest submission (SignalIdle, the last RetireSubmission) under bit 62's busy rule. In the VR mock
// (2000x2160 DLSS, GPU-bound) they were 10.4 of the 14.7 submissions per game frame
// (VR_SUBMIT_CENSUS), each a queue boundary of 20-50 us GPU idle. With FAST_CPU_SIGNAL_AT_REPLAY
// the replay writes the guest signals at their packets, so these submissions only hand the work
// to the GPU sooner: while the GPU still runs the last one submitted, the work stays in the open
// command buffer for the next cut (a pass switch, the flip, the frame end or a Wait()), and the
// replay tasks queued after it run where they did (SignalsWritten still checks their signals).
// Held only while the GPU still runs the previous game frame (its last tick at FlushFrameEnd):
// then it has that frame's rest and this frame's submitted part queued. These flushes come when
// the guest queue runs empty, so the stage may idle after one; a paced frame's GPU finishes the
// previous frame early in this one, and its flushes late in the frame are taken as before (held
// on any GPU work alone, a 90 Hz run left work unsubmitted while the GPU ran dry). Taken as
// before too: in the other signal modes (a submission's replay writes the signals), with bit 62
// or gpu_submit_ms off, or past the busy cap. Deferral is only on B: before its last queued
// packet is released it submits any still-open deferred tick. This keeps a quiet producer from
// stranding work, and keeps the flush within Drain/Wait's ownership boundary.
bool Rasterizer::DeferGuestFlush() {
    static const bool enabled = [] {
        const char* value = std::getenv("FAST_GUEST_FLUSH_DEFER");
        return !(value && value[0] == '0');
    }();
    if (!FastRender::IsStageB() || !enabled || !FastRender::AbOn(3) ||
        g_signal_mode != SignalPolicy::Mode::AtReplay ||
        !FastRender::On(SubmitCadence) || VR::GetSettings().gpu_submit_ms <= 0.0f) {
        return false;
    }
    auto& s = submit_cadence;
    if (scheduler.RecordingTime() >=
        std::chrono::duration<float, std::milli>(SubmitCadenceBusyMs())) {
        ++s.guest_cap;
        return false;
    }
    MasterSemaphore& semaphore = *scheduler.GetMasterSemaphore();
    const auto now = std::chrono::steady_clock::now();
    if (semaphore.IsFree(frame_end_tick) || (now - cadence_poll >= SubmitCadencePoll() && [&] {
                                                 cadence_poll = now;
                                                 ++s.polls;
                                                 semaphore.Refresh();
                                                 return semaphore.IsFree(frame_end_tick);
                                             }())) {
        ++s.guest_idle;
        return false;
    }
    ++s.guest_deferred;
    scheduler.NoteDeferredCut();
    guest_deferred_tick = scheduler.CurrentTick();
    draw_pipe->BeforeIdle(+[](void* context) {
        auto& self = *static_cast<Rasterizer*>(context);
        const u64 tick = std::exchange(self.guest_deferred_tick, 0);
        if (tick == self.scheduler.CurrentTick()) {
            if (g_task_publish)
                self.scheduler.PublishRecorder();
            self.scheduler.Flush();
        }
    });
    return true;
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
             "busy_ms={:.2f}; open_tick_flushes={} on_deferred_tick={}; verify={} verify_idle={}; "
             "guest flushes held={:.2f}/f taken caught_up={:.2f}/f cap={:.2f}/f",
             FastRender::On(SubmitCadence) ? 1 : 0, s.frames, s.submits / frames,
             s.midframe / frames, s.old_cuts, s.old_idle_known, eligible, s.idle, s.cap, s.signals,
             s.deferred, s.deferred / frames, s.polls / frames, SubmitCadenceBusyMs(), waits.flushes,
             waits.deferred, s.verified, s.verify_idle, s.guest_deferred / frames,
             s.guest_idle / frames, s.guest_cap / frames);
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
    // A warp, a death or a new map: both caches clean up after it (the same request).
    const u64 transition_request = VR::TakeTransitionTextureCleanupRequest();
    texture_cache.RunGarbageCollector(transition_request);
    buffer_cache.RunGarbageCollector(transition_request);
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

// FAST_CPU_REPEAT_DRAW (vk_rasterizer.h RepeatDraw), read once at start-up.
static const char g_repeat_draw = [] {
    const char* value = std::getenv("FAST_CPU_REPEAT_DRAW");
    return value && (value[0] == '1' || value[0] == 'v') ? value[0] : '0';
}();

bool Rasterizer::RepeatKey(const Pipeline* pipeline, std::array<u32, 9>& targets,
                           Shader::PushData& base) {
    repeat_flat.clear();
    for (const auto* stage : pipeline->GetStages()) {
        if (stage) {
            const auto flat = stage->FlatUserData();
            repeat_flat.push_back(u32(flat.size()));
            repeat_flat.insert(repeat_flat.end(), flat.begin(), flat.end());
        }
    }
    targets.fill(0);
    for (u32 cb = 0; cb < 8; ++cb)
        targets[cb] = cb_descs[cb].first.index;
    targets[8] = db_desc.first.index;
    base = MakeUserData(std::as_const(ScratchDraw::Registers(liverpool)));
    auto& r = repeat;
    // The first condition that fails, counted (logged with the hits).
    const u32 why = !r.valid                   ? 0
                    : r.pipeline != pipeline   ? 1
                    : r.flat != repeat_flat    ? 2
                    : r.targets != targets     ? 3
                    : std::memcmp(&r.base, &base, sizeof(base)) != 0 ? 4
                    : r.uses_end != scheduler.CommandUses() ? 5
                    : r.faults != FastRender::g_fault_epoch.load(std::memory_order_acquire) ? 6
                    : r.texture_epoch != texture_cache.RegistryEpoch() ? 7
                    : r.buffer_epoch != buffer_cache.CopyMemoEpoch()   ? 8
                    : r.perf_epoch != PerfMemo::Epoch()                ? 9
                    : r.buffer_gen != NativeStereo::BufferGeneration() ||
                              r.view_gen != NativeStereo::ViewGeneration()
                        ? 10
                        : 11;
    ++r.why[why];
    if constexpr (Common::Log::Enabled) {
        if ((++r.keys & 0xFFFF) == 0)
            LOG_INFO(Render_Vulkan,
                     "FAST_CPU_REPEAT_DRAW: {} keys, hits {}; misses: invalid {} pipeline {} "
                     "user data {} targets {} push {} recorded {} fault {} images {} buffers {} "
                     "perf {} stereo {}; stored {} verified {} mismatches {}",
                     r.keys, r.why[11], r.why[0], r.why[1], r.why[2], r.why[3], r.why[4],
                     r.why[5], r.why[6], r.why[7], r.why[8], r.why[9], r.why[10], r.stored,
                     r.verified, r.mismatches);
    }
    return why == 11;
}

void Rasterizer::RepeatRestore() {
    const auto& r = repeat;
    // The infos go back to the same arrays (static_vector: the same addresses), so the
    // remembered writes and hints point at them as they did.
    image_infos = r.image_infos;
    buffer_infos = r.buffer_infos;
    set_writes = r.writes;
    set_write_index = r.set_write_index;
    buffer_barriers.clear();
    stereo_handles = r.stereo_handles;
    buffer_hints = r.buffer_hints;
    view_hints = r.view_hints;
    barrier_hints.clear();
    native_handles = r.native_handles;
    push_data = r.push;
    // The binding flags BindTextures sets (BeginRendering's feedback check reads them).
    for (const auto id : r.bound_images)
        texture_cache.GetImage(id).binding.is_bound = 1u;
    bound_images = r.bound_images;
}

void Rasterizer::RepeatStore(const Pipeline* pipeline, const std::array<u32, 9>& targets,
                             const Shader::PushData& base) {
    auto& r = repeat;
    r.pipeline = pipeline;
    std::swap(r.flat, repeat_flat);
    r.targets = targets;
    r.base = base;
    r.push = push_data;
    r.writes.assign(set_writes.begin(), set_writes.end());
    r.set_write_index = set_write_index;
    r.image_infos = image_infos;
    r.buffer_infos = buffer_infos;
    r.bound_images = bound_images;
    r.stereo_handles = stereo_handles;
    r.buffer_hints = buffer_hints;
    r.view_hints = view_hints;
    r.native_handles = native_handles;
    r.valid = true; // uses_end: at the end of the draw (DrawRecord)
    r.uses_end = ~u64{0};
    ++r.stored;
}

const char* Rasterizer::RepeatCompare() const {
    const auto& r = repeat;
    if (r.writes.size() != set_writes.size() || r.set_write_index != set_write_index)
        return "write count";
    for (size_t i = 0; i < set_writes.size(); ++i) {
        const auto& a = r.writes[i];
        const auto& b = set_writes[i];
        if (a.dstBinding != b.dstBinding || a.dstArrayElement != b.dstArrayElement ||
            a.descriptorCount != b.descriptorCount || a.descriptorType != b.descriptorType ||
            a.pImageInfo != b.pImageInfo || a.pBufferInfo != b.pBufferInfo)
            return "write";
    }
    if (r.image_infos.size() != image_infos.size() ||
        !std::equal(image_infos.begin(), image_infos.end(), r.image_infos.begin()))
        return "image infos";
    if (r.buffer_infos.size() != buffer_infos.size() ||
        !std::equal(buffer_infos.begin(), buffer_infos.end(), r.buffer_infos.begin()))
        return "buffer infos";
    if (!buffer_barriers.empty())
        return "barriers";
    if (std::memcmp(&r.push, &push_data, sizeof(push_data)) != 0)
        return "push data";
    if (r.bound_images.size() != bound_images.size() ||
        !std::equal(bound_images.begin(), bound_images.end(), r.bound_images.begin()))
        return "bound images";
    if (r.stereo_handles != stereo_handles || r.buffer_hints.size() != buffer_hints.size() ||
        r.view_hints.size() != view_hints.size() ||
        std::memcmp(r.buffer_hints.data(), buffer_hints.data(),
                    buffer_hints.size() * sizeof(buffer_hints[0])) != 0 ||
        std::memcmp(r.view_hints.data(), view_hints.data(),
                    view_hints.size() * sizeof(view_hints[0])) != 0)
        return "hints";
    return nullptr;
}

bool Rasterizer::BindResources(const Pipeline* pipeline) {
    // FAST_CPU_REPEAT_DRAW: only DrawRecord's call may take or leave the remembered bindings.
    const bool repeat_allowed = g_repeat_draw != '0' && repeat.allowed;
    repeat.allowed = false;
    std::array<u32, 9> repeat_targets;
    Shader::PushData repeat_base;
    bool repeat_hit = false;
    if (!repeat_allowed) {
        repeat.valid = false;
    } else {
        repeat_hit = RepeatKey(pipeline, repeat_targets, repeat_base);
        if (repeat_hit && g_repeat_draw == '1') {
            resource_bindings = nullptr;
            FastRender::BProfile::Scope b_profile{FastRender::BProfile::Resources};
            Perf::NextDraw();
            vr_mirror_pipeline = pipeline;
            vr_mirror_noted = vr_mirror_looked = false;
            RepeatRestore();
            ++repeat.hits;
            if constexpr (Common::Log::Enabled) {
                if (((repeat.hits + repeat.misses) & 0x7FFFF) == 0)
                    LOG_INFO(Render_Vulkan, "FAST_CPU_REPEAT_DRAW: hits {} misses {} stored {}",
                             repeat.hits, repeat.misses, repeat.stored);
            }
            return true;
        }
        repeat.misses += repeat_hit ? 0 : 1;
    }
    const u64 repeat_uses = scheduler.CommandUses();
    // The epochs from before the resolve: a change during it makes the next draw resolve too.
    const u64 epoch_faults = FastRender::g_fault_epoch.load(std::memory_order_acquire);
    const u64 epoch_texture = texture_cache.RegistryEpoch();
    const u64 epoch_buffer = buffer_cache.CopyMemoEpoch();
    const u32 epoch_perf = PerfMemo::Epoch();
    const u64 epoch_buffer_gen = NativeStereo::BufferGeneration();
    const u64 epoch_view_gen = NativeStereo::ViewGeneration();
    const bool full = BindResourcesFull(pipeline);
    if (!repeat_allowed)
        return full;
    if (repeat_hit) {
        // Verify: the full path ran; it must agree and must have recorded nothing.
        ++repeat.verified;
        const char* what = scheduler.CommandUses() != repeat_uses ? "recorded" : RepeatCompare();
        if (what) {
            ++repeat.mismatches;
            if (repeat.mismatches <= 64 || (repeat.mismatches & 1023) == 0)
                LOG_ERROR(Render_Vulkan,
                          "FAST_CPU_REPEAT_DRAW verify: {} differs (mismatch #{} of {} checked)",
                          what, repeat.mismatches, repeat.verified);
        }
        if constexpr (Common::Log::Enabled) {
            if ((repeat.verified & 0xFFFF) == 0)
                LOG_INFO(Render_Vulkan, "FAST_CPU_REPEAT_DRAW verify: {} checked, {} mismatches",
                         repeat.verified, repeat.mismatches);
        }
    }
    const auto& writes = pipeline->Writes();
    if (full && !pipeline->IsCompute() && !writes.dma && !writes.buffers && !writes.images) {
        RepeatStore(pipeline, repeat_targets, repeat_base);
        repeat.faults = epoch_faults;
        repeat.texture_epoch = epoch_texture;
        repeat.buffer_epoch = epoch_buffer;
        repeat.perf_epoch = epoch_perf;
        repeat.buffer_gen = epoch_buffer_gen;
        repeat.view_gen = epoch_view_gen;
    } else {
        repeat.valid = false;
    }
    return full;
}

bool Rasterizer::BindResourcesFull(const Pipeline* pipeline) {
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
    const auto& stage_writes = pipeline->Writes();
    const bool writes_memory =
        (small_read_direct || note_eye_writes) && (stage_writes.dma || stage_writes.buffers);
    buffer_cache.SetSmallReadDirect(small_read_direct && !writes_memory);
    // Bit 53 also needs a draw's storage image writes (bit 33 does not: they are not buffers). An
    // image bound both sampled and written takes TransitTexture's force_general path, whose
    // tracked access (ShaderRead | ColorAttachment*) has no write flag, so a later game dispatch
    // binding it the same way gets no transition barrier: only the middle eye barrier then orders
    // the draw's layer-1 accesses before the right dispatch.
    if (note_eye_writes && !eye_unordered_writes && (writes_memory || stage_writes.images)) {
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
    if (stage_writes.dma) {
        Perf::NoteDma();
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
    // VR_RETILE_DEFER: perf_copy_memo's hits bind without ObtainBuffer.
    buffer_cache.MaterializeRetiles(src_addr, src_size, VideoCore::BufferCache::RetileHleCopy);
    buffer_cache.MaterializeRetiles(dst_addr, dst_size, VideoCore::BufferCache::RetileHleCopy);
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
        buffer_cache.MarkUsed(VideoCore::BufferId{found->src_buffer});
        buffer_cache.MarkUsed(VideoCore::BufferId{found->dst_buffer});
        // Bound for writing without ObtainBuffer: the motion snapshots there are stale.
        buffer_cache.NoteMotionWrite(dst_addr, dst_size);
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
    // VR_PERF_PROBE (tests): why a two-buffer compute shader is
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

    const auto identity_dword = [](const AmdGpu::Buffer& b) {
        const auto fmt = b.GetNumberFmt();
        return b.GetStride() == 4 && b.GetDataFmt() == AmdGpu::DataFormat::Format32 &&
               (fmt == AmdGpu::NumberFormat::Float || fmt == AmdGpu::NumberFormat::Uint ||
                fmt == AmdGpu::NumberFormat::Sint) &&
               b.dst_sel_x == 4 && !b.swizzle_enable && !b.add_tid_enable;
    };

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
            const vk::BufferImageCopy region{
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
            // VR_RETILE_DEFER: nothing reads these destinations in play (0 reads in ~3,000
            // copies, Old Yharnam and the lamp, 2026-10-31): the re-tile waits for a reader.
            if (!buffer_cache.DeferImageRetile(buf1.base_address, bytes, image0_id, region)) {
                const auto [destination, offset] =
                    buffer_cache.ObtainBuffer(buf1.base_address, bytes, true, true);
                auto copy = region;
                NativeStereo::Barrier(scheduler);
                texture_cache.GetTileManager().TileImage(source, std::span{&copy, 1},
                                                         destination->Handle(), offset, bytes);
                NativeStereo::Barrier(scheduler);
            }
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
        image0.info.pitch != image1.info.pitch || image0.info.num_bits != image1.info.num_bits) {
        return reject("the two images differ in size, pitch or bits");
    }

    u32 tail_bytes = 0;
    if (image0.info.guest_size != buf0.GetSize()) {
        const auto tail = ImageCopyTailBytes(buf0.base_address, buf1.base_address,
                                            image0.info.guest_size, buf0.GetSize());
        // Unlike the existing exact-size path, only widen the audited identity-copy
        // shader. SGPR10 bounds its stores; a partial dispatch must stay on the GPU.
        if (!tail || info.pgm_hash != 0x3d5ebf4e || desc0.is_written || !desc1.is_written ||
            !identity_dword(buf0) || !identity_dword(buf1) ||
            buf0.GetNumberFmt() != buf1.GetNumberFmt() ||
            info.UserData().size() <= 10 || info.UserData()[10] < buf0.GetSize() / 4 ||
            cs_pgm.dim_y != 1 || cs_pgm.dim_z != 1) {
            return reject("not a complete padded identity copy");
        }
        const auto& a = image0.info;
        const auto& b = image1.info;
        // Keep the new path to matching single-mip, single-sample colour images.
        // A byte copy between different tiled layouts is not an image copy.
        if (image0.native_stereo != image1.native_stereo ||
            a.type != AmdGpu::ImageType::Color2D || a.type != b.type || a.size != b.size ||
            a.pixel_format != b.pixel_format || a.resources != b.resources ||
            a.resources.levels != 1 || a.resources.layers != 1 || a.size.depth != 1 ||
            a.num_samples != 1 || b.num_samples != 1 ||
            a.props.is_depth || b.props.is_depth || a.props.has_stencil || b.props.has_stencil ||
            a.props.is_block || b.props.is_block || a.tile_mode != b.tile_mode ||
            a.array_mode != b.array_mode || a.bank_swizzle != b.bank_swizzle ||
            a.alt_tile != b.alt_tile || a.props.is_pow2 != b.props.is_pow2 ||
            a.mips_layout[0].size != a.guest_size || b.mips_layout[0].size != b.guest_size ||
            a.mips_layout[0].offset != 0 || b.mips_layout[0].offset != 0 ||
            a.mips_layout[0].pitch != b.mips_layout[0].pitch ||
            a.mips_layout[0].height != b.mips_layout[0].height) {
            return reject("padded copy image layouts differ or are unsupported");
        }
        // The extra bytes must be buffer data, not part of another cached image.
        // Otherwise the general shader path must handle its image coherence too.
        bool aliases_image = false;
        for (const auto address : {buf0.base_address + a.guest_size,
                                   buf1.base_address + a.guest_size}) {
            texture_cache.ForEachImageInRegion(
                address, *tail,
                [&](VideoCore::ImageId, VideoCore::Image&) { aliases_image = true; });
        }
        if (aliases_image) {
            return reject("padded copy tail aliases an image");
        }
        tail_bytes = *tail;
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
    ++dst_image.write_serial;
    if (tail_bytes != 0) {
        // bbport copies the padding separately. Use our existing buffer tracking
        // and stereo copy so right-eye data and GPU-written bytes stay intact.
        const VAddr src_tail = src_image.info.guest_address + src_image.info.guest_size;
        const VAddr dst_tail = dst_image.info.guest_address + dst_image.info.guest_size;
        const auto [src, src_offset, dst, dst_offset] =
            ObtainCopyBuffers(src_tail, tail_bytes, dst_tail, tail_bytes);
        NativeStereo::Barrier(scheduler);
        NativeStereo::Copy(scheduler, src->Handle(), dst->Handle(),
                           vk::BufferCopy{src_offset, dst_offset, tail_bytes});
        NativeStereo::Barrier(scheduler);
        texture_cache.InvalidateMemoryFromGPU(dst_tail, tail_bytes);
    }
    return true;
}

// FAST bit 55 (gpu_plan.md item 4, G'): the game's clear shader cs 8b355b5a, which
// IsComputeImageClear refuses (13 calls/f in the SMAA headset mode, about 10.5 us each as two
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

// FAST_MIP0_CLEAR=0 (read once at start-up) refuses first-level clears of mipped images again
// (IsComputeImageClear), for A/B.
static const bool g_mip0_clear = [] {
    const char* value = std::getenv("FAST_MIP0_CLEAR");
    return value == nullptr || value[0] != '0';
}();

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

    // Image clear must be valid: the whole image, or (g_mip0_clear) exactly the first level of a
    // mipped image at its own address. Wet streets (2026-11-11): every frame the game clears
    // level 0 of the reflection trace's target (700x756 RGBA16F, 7 levels in a 1024x1024 chain:
    // 8 MiB of 0xaaae00), which was refused here, filled as bytes in both eyes and marked the
    // image GpuDirty, so binding it re-uploaded all 7 levels of both eyes (~0.2 ms a frame of
    // fill, detile and copy, and two more pipeline breaks) for a picture the trace then wrote
    // whole. Now level 0 of both eyes is cleared on the GPU and the other levels kept. Not over a
    // pending CPU upload (Dirty): that upload must still reach the other levels.
    VideoCore::Image& image1 = texture_cache.GetImage(image1_id);
    const bool whole = image1.info.guest_size == buf1.GetSize();
    const bool first_level = !whole && g_mip0_clear && image1.info.resources.levels > 1 &&
                             image1.info.guest_address == buf1.base_address &&
                             image1.info.mips_layout[0].offset == 0 &&
                             image1.info.mips_layout[0].size == buf1.GetSize() &&
                             False(image1.flags & VideoCore::ImageFlagBits::Dirty);
    if ((!whole && !first_level) || image1.info.num_bits != buf1_bpp ||
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
    VideoCore::SubresourceRange range = {
        .base =
            {
                .level = 0,
                .layer = 0,
            },
        .extent = image1.info.resources,
    };
    if (first_level) {
        range.extent.levels = 1; // every layer (both eyes), level 0 only
    }
    Common::MemoryWriteTrace::Event("compute.image.clear", buf1.base_address, buf1.GetSize(),
        info.pgm_hash, Common::MemoryWriteTrace::SourceWord(values, 16));
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
    // The cleared range the right eye's own (a sparse view's twin shares the rest); left when
    // mono.
    const vk::Buffer right = NativeStereo::RightForWrite(scheduler, left, offset, bytes);
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
    Common::MemoryWriteTrace::Event("compute.buffer.fill", address, bytes, *word, info.pgm_hash);
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

// FAST_CPU_CB_MEMO (on unless '0', flat and DIBR too since 2026-11-01; read once at start-up, a
// plain load per use): BindBuffers finds a stage's prepared constants in one pass over the packet's
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
    std::array<const DrawPacket::ConstantBinding*, Shader::NUM_BUFFERS> prepared;
    const bool cb_memo = g_cb_memo;
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
            // Every used slot is initialized before lookup; unused tail slots are never read.
            prepared[index] = nullptr;
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
    float light_view_ipd = 0.f;
    Shader::Optimization::FogLight::ListView fog_draw_view;
    const auto write_descriptor = [&](const VideoCore::Buffer* owner,
                                      vk::DescriptorBufferInfo camera_right) {
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
            buffer_hints.resize(buffer_infos.size() - 1);
            buffer_hints.push_back({left, owner ? owner->NativeEntry() : nullptr, camera_right});
        }
    };
    // Second pass to re-bind buffers that were updated after binding
    for (u32 i = 0; i < buffer_bindings.size(); i++) {
        const auto& [buffer_id, vsharp, size] = buffer_bindings[i];
        const auto& desc = stage.buffers[i];
        const auto* constant = prepared[i];
        // Flat user data is bound verbatim. A vertex constant can carry an eye-specific
        // camera only in the 864-byte scene layout. Keep the audit and verification paths.
        const bool plain_constant = constant && size != 864 && !desc.is_written &&
            (stage.l_stage == Shader::LogicalStage::Vertex ||
             desc.buffer_type == Shader::BufferType::Flatbuf) &&
            !cb_verify && !g_camera_audit;
        if (plain_constant) {
            ++constant_bind_hits;
            buffer_infos.push_back(constant->raw);
            write_descriptor(nullptr, {});
            continue;
        }
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
            // A perspective camera in the shared atlas (a spot light's shadow: entering Ludwig's
            // arena, player report 2026-11-07) was an ASSERT, which ended the game. A light's view
            // is the same for both eyes, so the atlas stays shared; noted once, never fatal. The
            // statement keeps the assert's shape (do / if / the same test), so this hot function's
            // renderer PGO profile still applies.
            do {
                if (!(scene[66] == 0.f && scene[67] == 1.f)) [[unlikely]] {
                    LOG_DEBUG(Render_Vulkan, "VR native shared shadow: a perspective camera");
                    NativeStereo::SharedShadowPerspective(scene[66], scene[67]);
                }
            } while (false);
        }
        NativeStereo::CameraConstants temporal_camera;
        const VideoCore::Buffer* owner = nullptr; // bit 25: the buffer this descriptor names
        const bool capture_temporal = vr_temporal && vr_temporal->gbuffer &&
                                      stage.l_stage == Shader::LogicalStage::Vertex &&
                                      vr_temporal->scene.Active();
        constant_bind_hits += constant != nullptr;
        bool projection_paired = false, mirrored = false;
        vk::DescriptorBufferInfo native_camera;
        // (The cheap tests first: most buffers are not a camera block. Every term before the
        // pairing call is a plain test, so the order changes nothing else.)
        const bool camera =
            size == 864 && NativeStereo::Enabled() &&
            (stage.l_stage == Shader::LogicalStage::Compute || recording_view_mask != 0) &&
            !desc.IsSpecial() && !desc.is_written &&
            (constant ? constant->camera : NativeStereo::CameraBuffer(
                buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream),
                reinterpret_cast<const float*>(vsharp.base_address), alignment,
                VR::DrawFrameRecord(), native_camera, capture_temporal ? &temporal_camera : nullptr,
                &projection_paired, mirror_flip ? &mirrored : nullptr));
        // The frame's camera for the lamp shadows' slots (VrFogCaptureSceneView): the first scene
        // camera block bound this frame.
        if (size == 864 && !desc.IsSpecial() && !desc.is_written && vr_fog.view_frame != vr_fog.frame &&
            recording_view_mask != 0 && stage.l_stage != Shader::LogicalStage::Compute &&
            VrSceneMapsOn() && !VrInMirrorPass() &&
            (constant || !buffer_cache.IsRegionGpuModified(vsharp.base_address, size))) [[unlikely]] {
            VrFogCaptureSceneView(constant ? reinterpret_cast<const float*>(constant_ring->mapped_data.data() +
                                                                           constant->raw.offset)
                                           : static_cast<const float*>(ScratchDraw::MemorySource(
                                                 reinterpret_cast<const void*>(vsharp.base_address), 864)));
        }
        if (camera && constant) {
            ++constant_pair_hits;
            native_camera = constant->left;
            projection_paired = constant->projected;
            mirrored = constant->mirrored;
            NativeStereo::RegisterCameraPair(constant->left, constant->right);
        }
        if (camera && capture_temporal) {
            const auto& viewport = ScratchDraw::Registers(liverpool).viewports[0];
            const float xs = viewport.xscale < 0 ? -1.f : 1.f;
            const float ys = viewport.yscale < 0 ? -1.f : 1.f;
            if (constant) {
                // Same ring bytes as the previous copies, consumed immediately by the scene.
                using CameraBytes = VrTemporalScene::CameraBytes;
                const auto ring_bytes = std::as_bytes(constant_ring->mapped_data);
                vr_temporal->scene.CaptureCameraBytes(
                    CameraBytes{ring_bytes.subspan(constant->left.offset, 864)},
                    CameraBytes{ring_bytes.subspan(constant->right.offset, 864)},
                    xs, ys);
            } else {
                vr_temporal->scene.CaptureCamera(temporal_camera, xs, ys);
            }
        }
        if (camera)
            native_paired_buffers |= u64{1} << i;
        if (projection_paired)
            native_projected_buffers |= u64{1} << i;
        if (camera && stage.l_stage != Shader::LogicalStage::Compute && !vr_mirror_noted)
            VrMirrorCamera(mirrored, vsharp.base_address);
        // (320: the game's reflections, NativeProjection::ReflectionLayout; that program only.)
        const bool light_size =
            size == 208 || size == 272 || size == 336 || size == 416 ||
            (size == 320 && stage.pgm_hash == NativeProjection::ReflectionProgram);
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
            if (ring_light) {
                light_view_scale = binding_packet ? binding_packet->camera.world_scale : 0.f;
                light_view_ipd = binding_packet ? NativeStereo::Ipd(binding_packet->camera) : 0.f;
            } else {
                light_view_ipd = NativeStereo::Ipd(VR::DrawFrameRecord());
            }
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
                light_view_ipd, native_camera);
        // Light in the air (fog_light.h): the light pass's camera (its light208 buffer, before
        // its list), for where its lights are in the world ...
        if (i == 0 && size == 208 && (vr_fog.on || Shader::Optimization::BodyShadow::Enabled()) &&
            stage.l_stage == Shader::LogicalStage::Fragment && !desc.IsSpecial() && !desc.is_written) {
            const auto kind = NativeProjection::LightListLayout(stage.pgm_hash).kind;
            const bool frame_candidate = kind == NativeProjection::LightKind::Point ||
                (kind == NativeProjection::LightKind::None && VrSceneMapsOn() &&
                 Shader::Optimization::AmbientOcclusion::IsAoProgram(stage.pgm_hash));
            if ((kind != NativeProjection::LightKind::None || frame_candidate) &&
                (constant || !buffer_cache.IsRegionGpuModified(vsharp.base_address, size))) {
                VrFogCaptureView(constant ? reinterpret_cast<const float*>(
                                                constant_ring->mapped_data.data() + constant->raw.offset)
                                          : reinterpret_cast<const float*>(vsharp.base_address),
                                 u32(size), constant == nullptr, frame_candidate, fog_draw_view);
            }
        }
        // ... and the frame's point lights, from the light pass's lists (the fog's, and his body
        // maps' lamps: vr_body_maps.h).
        if (i == 1 && (vr_fog.on || Shader::Optimization::BodyShadow::Enabled()) &&
            stage.l_stage == Shader::LogicalStage::Fragment &&
            !desc.IsSpecial() && !desc.is_written &&
            NativeProjection::LightListFor(stage.pgm_hash, size).kind !=
                NativeProjection::LightKind::None &&
            (constant || !buffer_cache.IsRegionGpuModified(vsharp.base_address, size)))
            VrFogCapture(stage.pgm_hash,
                         constant ? reinterpret_cast<const float*>(constant_ring->mapped_data.data() +
                                                                   constant->raw.offset)
                                  : reinterpret_cast<const float*>(vsharp.base_address),
                         u32(size), constant == nullptr, fog_draw_view);
        // Moonlight in the air (moon_shafts.h): the shadow pass's cascades, the directional
        // light's light sets (their buffer 0, the game's own bytes).
        // (The cascades also without the moonlight in the air, for the air pass's lamp glow: the
        // scene copy takes each eye's matrices from them. Until 2026-11-14 they were captured
        // only with the moonlight on, and with it off no lamp had a glow at all.)
        // (Buffer 0 of a fragment program first: Halos() is a function-local static, its guard read
        // at every buffer of every draw until 2026-11-17.)
        if (i == 0 && stage.l_stage == Shader::LogicalStage::Fragment &&
            (vr_moon.on || (vr_fog.on && Shader::Optimization::AirLight::Halos())) &&
            !desc.IsSpecial() && !desc.is_written &&
            (Shader::Optimization::MoonShafts::IsShadowProgram(stage.pgm_hash) ||
             (vr_moon.on && Shader::Optimization::MoonShafts::IsDirLightProgram(stage.pgm_hash)))) {
            if (Shader::Optimization::MoonShafts::IsDirLightProgram(stage.pgm_hash)) {
                // The GPU writes the table: the composite binds it as this pass does.
                VrMoonCaptureSets(vsharp, u32(size));
            } else if (constant || !buffer_cache.IsRegionGpuModified(vsharp.base_address, size)) {
                VrMoonCaptureCascades(
                    constant ? reinterpret_cast<const float*>(constant_ring->mapped_data.data() +
                                                              constant->raw.offset)
                             : reinterpret_cast<const float*>(vsharp.base_address),
                    u32(size), constant == nullptr);
            }
        }
        // The air pass's mist (air_light.h Mist): this frame's view to world, from the reflections
        // pass's own buffer (the half-size host's: its rows 32..43).
        if (vr_moon.on && i == 0 && size == 320 && stage.l_stage == Shader::LogicalStage::Fragment &&
            !desc.IsSpecial() && !desc.is_written &&
            Shader::Optimization::AirLight::IsHalfProgram(stage.pgm_hash) &&
            (constant || !buffer_cache.IsRegionGpuModified(vsharp.base_address, size))) {
            VrAirCaptureView(constant ? reinterpret_cast<const float*>(constant_ring->mapped_data.data() +
                                                                       constant->raw.offset)
                                      : reinterpret_cast<const float*>(vsharp.base_address),
                             u32(size), constant == nullptr);
        }
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
            } else if (desc.buffer_type == Shader::BufferType::VrGrade) {
                const float strength = VR::IsEnabled() ? VR::Tune::Get("lighting.grade", 0.01f * VR::GetSettings().cinematic_look) : 0.0f;
                // Float 15: the tone map's filmic highlights (cinematic_look.h FilmicShare).
                const float filmic = VR::IsEnabled() ? VR::Tune::Get("lighting.filmic", VR::GetSettings().filmic_highlights ? Shader::Optimization::CinematicLook::FilmicShare : 0.0f) : 0.0f;
                // Floats 16..20: eye adaptation (cinematic_look.h), its followers' shares from this
                // frame's time (once a frame: the grade and the tone map both bind this buffer).
                namespace Look = Shader::Optimization::CinematicLook;
                static u64 adapt_id = ~0ull;
                static auto adapt_time = std::chrono::steady_clock::now();
                static float adapt_dt = 1.0f / 90.0f;
                const u64 frame_id = VR::DrawFrameRecord().id;
                if (frame_id != adapt_id) {
                    const auto now = std::chrono::steady_clock::now();
                    adapt_dt = adapt_id == ~0ull ? 1.0f / 90.0f
                                                 : std::clamp(std::chrono::duration<float>(now - adapt_time).count(), 0.0f, 0.1f);
                    adapt_time = now;
                    adapt_id = frame_id;
                }
                Look::Adaptation adapt{};
                adapt.strength = VR::IsEnabled() && VR::GetSettings().eye_adaptation
                                     ? VR::Tune::Get("lighting.adapt", Look::AdaptStrength)
                                     : 0.0f;
                adapt.fast = 1.0f - std::exp(-adapt_dt / Look::FastS);
                adapt.slow = 1.0f - std::exp(-adapt_dt / std::max(VR::Tune::Get("lighting.adapt_up_s", Look::SlowS), 0.05f));
                adapt.slow_down = 1.0f - std::exp(-adapt_dt / std::max(VR::Tune::Get("lighting.adapt_down_s", Look::SlowDownS), 0.05f));
                adapt.stops = VR::Tune::Get("lighting.adapt_stops", Look::MaxStops);
                adapt.threshold = VR::Tune::Get("lighting.adapt_threshold", Look::ThresholdStops);
                adapt.frame = static_cast<u32>(frame_id);
                // A muzzle flash is not daylight: the levels hold still while one shines (v25, the
                // Grand Cathedral: the adaptation took a shot for a step outside and brightened the
                // flash to white for half a second).
                if (vr_fog.muzzle_on &&
                    std::chrono::duration<float>(std::chrono::steady_clock::now() - vr_fog.muzzle_t0).count() <=
                        Shader::Optimization::FogLight::MuzzleLifeS) {
                    adapt.fast = 0.0f;
                    adapt.slow = 0.0f;
                    adapt.slow_down = 0.0f;
                }
                // Tests (VR_EYE_ADAPT_LOG=1): the state four times a second (current and adapted
                // levels in stops, the exposure they give), read back from the host-visible
                // state buffer (a frame or two old).
                static const bool adapt_log = [] {
                    const char* const env = std::getenv("VR_EYE_ADAPT_LOG");
                    return env != nullptr && env[0] == '1';
                }();
                static auto adapt_logged = std::chrono::steady_clock::now();
                if (adapt_log && vr_adapt_buffer &&
                    std::chrono::steady_clock::now() - adapt_logged > std::chrono::milliseconds(250)) {
                    adapt_logged = std::chrono::steady_clock::now();
                    const auto* const st = reinterpret_cast<const float*>(vr_adapt_buffer->mapped_data.data());
                    const u32 slot = (static_cast<u32>(frame_id) & 1u) * 2u;
                    const float cur = st[slot], ada = st[slot + 1];
                    const float gap = cur - ada;
                    const float past = std::max(std::abs(gap) - adapt.threshold, 0.0f) * (gap < 0.0f ? -1.0f : 1.0f);
                    const auto* const su = reinterpret_cast<const u32*>(st);
                    LOG_INFO(Render_Vulkan, "VR eye adaptation: current {:.2f} adapted {:.2f} stops, gap {:+.2f}, exposure x{:.2f} "
                             "(frame {}, valid {}, claimed {}, counts {} {} {})",
                             cur, ada, gap, std::exp2(std::clamp(adapt.strength * past, -adapt.stops, adapt.stops)),
                             static_cast<u32>(frame_id), su[4], su[5], su[9], su[11], su[13]);
                }
                auto grade = Look::Params(strength, filmic, adapt);
                // The ambient light's level (cinematic_look.h AmbientFloat; the ambient composite).
                // (The Forbidden Woods a little darker of their own: cinematic_look.h WoodsAmbient.)
                const bool in_woods = VR::GameArea::Slot().load(std::memory_order_relaxed) >> 16 == 0x1B00u;
                grade[Look::AmbientFloat] =
                    VR::IsEnabled() ? std::clamp(VR::Tune::Get("lighting.ambient", in_woods ? Look::WoodsAmbientNow() : 1.0f),
                                                 0.0f, 2.0f)
                                    : 1.0f;
                auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                const u32 size = static_cast<u32>(sizeof(grade));
                const u64 offset = vk_buffer.Copy(grade.data(), size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, size);
            } else if (desc.buffer_type == Shader::BufferType::VrFogLights) {
                auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                std::array<u32, Shader::Optimization::FogLight::BufferDwords> fog;
                const u32 ubo_size = VrFogUpload(fog);
                VrAirWiden(fog, stage.pgm_hash);
                const u64 offset = vk_buffer.Copy(fog.data(), ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::VrAdaptation) {
                // Eye adaptation's state: one small buffer for every frame (its contents carry
                // over), host-visible like the GDS buffer the game's own atomics use, zeroed once.
                namespace Look = Shader::Optimization::CinematicLook;
                if (!vr_adapt_buffer) {
                    vr_adapt_buffer = std::make_unique<VideoCore::Buffer>(
                        instance, scheduler, VideoCore::MemoryUsage::Stream, 0, VideoCore::AllFlags,
                        Look::AdaptDwords * sizeof(u32));
                    std::memset(vr_adapt_buffer->mapped_data.data(), 0,
                                vr_adapt_buffer->mapped_data.size());
                }
                buffer_infos.emplace_back(vr_adapt_buffer->Handle(), 0,
                                          Look::AdaptDwords * sizeof(u32));
            } else if (desc.buffer_type == Shader::BufferType::VrBodyMaps) {
                // His body maps (vr_body_maps.h): the module's buffer once it exists; until then a
                // stream range of the same size the header marks unused (valid 0).
                if (vr_body_maps && vr_body_maps->Ready()) {
                    buffer_infos.emplace_back(vr_body_maps->MapsBuffer(), 0,
                                              VrBodyMaps::MapsBytes());
                } else {
                    auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                    static const std::array<u32, 4> none{};
                    const u64 offset = vk_buffer.Copy(none.data(), sizeof(none), alignment);
                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, sizeof(none));
                }
            } else if (desc.buffer_type == Shader::BufferType::VrShadowCasters) {
                auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                const auto& casters = VrShadowCastersUpload();
                const u32 bytes = static_cast<u32>(casters.size() * sizeof(u32));
                const u64 offset = vk_buffer.Copy(casters.data(), bytes, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, bytes);
            } else if (desc.buffer_type == Shader::BufferType::VrShadowMask) {
                // The shadow mask (light_shadow.h SceneMaps::Mask) once made; until then a stream
                // range of zeros the size of its header and one pixel (a lamp shader's loads stay
                // inside it; its size, 0, says no mask: lit; the capacity, 0, keeps the AO out).
                if (vr_scene.mask) {
                    buffer_infos.emplace_back(
                        vr_scene.mask->Handle(), 0,
                        (u64(Shader::Optimization::LightShadow::SceneMaps::MaskHeader) +
                         u64(vr_scene.mask_pixels) * Shader::Optimization::LightShadow::SceneMaps::MaskWords) *
                            sizeof(u32));
                } else {
                    buffer_infos.push_back(VrWrittenStandIn(
                        0, (u64(Shader::Optimization::LightShadow::SceneMaps::MaskHeader) +
                            Shader::Optimization::LightShadow::SceneMaps::MaskWords + 1) *
                               sizeof(u32)));
                }
            } else if (desc.buffer_type == Shader::BufferType::VrSceneMaps) {
                // The scene shadow maps (light_shadow.h SceneMaps) once made; until then a small
                // stand-in (the header's slot count stays 0: nothing reads or writes it).
                if (vr_scene.buffer) {
                    buffer_infos.emplace_back(
                        vr_scene.buffer->Handle(), 0,
                        2 * u64(Shader::Optimization::LightShadow::SceneMaps::Texels) * sizeof(u32));
                } else {
                    buffer_infos.push_back(VrWrittenStandIn(1, 4 * sizeof(u32)));
                }
            } else if (desc.buffer_type == Shader::BufferType::VrShadowEdges) {
                // The edge pass's bits (vr_shadow_edges.h) once made; else zeros the size of its
                // header (its serial 0: no lamp reads a bit; the index stays in the header).
                if (vr_edges && vr_edges->Buffer() && vr_edges->Bytes() != 0) {
                    buffer_infos.emplace_back(vr_edges->Buffer(), 0, vr_edges->Bytes());
                } else {
                    auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                    static const std::array<u32, VrShadowEdges::DataAt> none{};
                    const u64 offset = vk_buffer.Copy(none.data(), sizeof(none), alignment);
                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, sizeof(none));
                }
            } else if (desc.buffer_type == Shader::BufferType::VrDepthPyramid) {
                // The depth pyramid (vr_depth_pyramid.h) recorded for this frame; else a stand-in
                // of zeros (its header's size 0: the march reads no pyramid).
                // (Recorded before the frame's first AO draw; a later one may carry a
                // neighbouring camera record: air_light.h FrameSlack.)
                if (vr_pyramid && vr_pyramid_ready &&
                    Shader::Optimization::AirLight::FrameNear(vr_pyramid_frame, VR::DrawFrameRecord().id)) {
                    buffer_infos.emplace_back(vr_pyramid->Buffer(), 0, vr_pyramid->Bytes());
                } else {
                    auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                    static const std::array<u32, VrDepthPyramid::HeaderWords> none{};
                    const u64 offset = vk_buffer.Copy(none.data(), sizeof(none), alignment);
                    buffer_infos.emplace_back(vk_buffer.Handle(), offset, sizeof(none));
                }
            } else if (desc.buffer_type == Shader::BufferType::VrShadowGrid) {
                // The blocker grid (light_shadow.h Grid) once made; until then a small stand-in:
                // the header (VrBodyUpload) keeps the grid off, nothing reads or writes it.
                if (vr_grid_ready && vr_grid_buffer) {
                    buffer_infos.emplace_back(
                        vr_grid_buffer->Handle(), 0,
                        u64(Shader::Optimization::LightShadow::Grid::BufferWords) * sizeof(u32));
                } else {
                    buffer_infos.push_back(VrWrittenStandIn(2, 4 * sizeof(u32)));
                }
            } else if (desc.buffer_type == Shader::BufferType::VrBodyCapsules) {
                auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                std::array<u32, Shader::Optimization::BodyShadow::BufferDwords> body;
                const u32 ubo_size = VrBodyUpload(body, stage.pgm_hash);
                const u64 offset = vk_buffer.Copy(body.data(), ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::VrParticleLights) {
                auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                std::array<u32, Shader::Optimization::ParticleAir::BufferDwords> lights;
                const u32 ubo_size = VrParticleUpload(lights);
                const u64 offset = vk_buffer.Copy(lights.data(), ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::VrAir) {
                // The air pass (air_light.h): this draw's part and its camera.
                auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                std::array<u32, Shader::Optimization::AirLight::ParamDwords> air;
                const u32 ubo_size = VrAirUpload(air, stage);
                const u64 offset = vk_buffer.Copy(air.data(), ubo_size, alignment);
                buffer_infos.emplace_back(vk_buffer.Handle(), offset, ubo_size);
            } else if (desc.buffer_type == Shader::BufferType::VrAirCells) {
                // Its cells once made; until then a few zeros in a buffer of their own (the
                // parameters name one cell). Never a range of the shared stream buffer: the
                // compute hosts declare the cells written, and native stereo gives a written
                // buffer a right-eye twin; a twin of the stream buffer left the right eye reading
                // stale cameras and lights (2026-11-14: the right eye's picture garbage whenever a
                // host bound the stand-in, e.g. with the moonlight off).
                if (vr_air.cells) {
                    buffer_infos.emplace_back(vr_air.cells->Handle(), 0, vr_air.cells_words * sizeof(u32));
                } else {
                    if (!vr_air.cells_none) {
                        vr_air.cells_none = std::make_unique<VideoCore::Buffer>(
                            instance, scheduler, VideoCore::MemoryUsage::Stream, 0, VideoCore::AllFlags,
                            4 * sizeof(u32));
                        std::memset(vr_air.cells_none->mapped_data.data(), 0,
                                    vr_air.cells_none->mapped_data.size());
                    }
                    buffer_infos.emplace_back(vr_air.cells_none->Handle(), 0, 4 * sizeof(u32));
                }
            } else if (desc.buffer_type == Shader::BufferType::VrFogGrid) {
                // The fog grid (fog_grid.h): the last one built, or its stand-in.
                buffer_infos.emplace_back(VrFogGridBuffer());
            } else if (desc.buffer_type == Shader::BufferType::VrAirProbe) {
                // The moon's colour probe: one small host-visible buffer (the eye adaptation's way).
                VrAirProbeBuffer();
                buffer_infos.emplace_back(vr_air.probe->Handle(), 0,
                                          u64(Shader::Optimization::AirLight::ProbeSlots) *
                                              Shader::Optimization::AirLight::ProbeSlotDwords * sizeof(u32));
            } else if (desc.buffer_type == Shader::BufferType::VrMoonShafts) {
                auto& vk_buffer = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
                std::array<u32, Shader::Optimization::MoonShafts::BufferDwords> moon;
                const u32 ubo_size = VrMoonUpload(moon);
                VrAirMoon(moon, stage.pgm_hash);
                const u64 offset = vk_buffer.Copy(moon.data(), ubo_size, alignment);
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
        if constexpr (Common::Log::Enabled) {
            // A written buffer of ours in the shared stream buffer would give that buffer a
            // right-eye twin (VrWrittenStandIn): said once, in the log builds' test runs.
            if (desc.IsSpecial() && desc.is_written && NativeStereo::Enabled() &&
                desc.buffer_type != Shader::BufferType::SharedMemory &&
                buffer_infos.back().buffer ==
                    buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream).Handle()) [[unlikely]] {
                static std::atomic<bool> said{false};
                if (!said.exchange(true)) {
                    LOG_ERROR(Render_Vulkan,
                              "VR written stand-in: buffer {} of shader {:#x} (type {}) is a range of the "
                              "stream buffer; the right eye will read stale data",
                              i, stage.pgm_hash, static_cast<u32>(desc.buffer_type));
                }
            }
        }

        // FAST_CPU_FUSED_PAIR: a camera, lighting, screen point, screen rect or light list
        // descriptor's own pair, registered just above (the constant's, else the last one):
        // what the pairing scan finds for this left half, every pair's left being an allocation
        // of its own. A reflection pass's light paired here has its own (mirror_light); a light
        // list always has (LightListBuffer), also from a prepared constant.
        vk::DescriptorBufferInfo camera_right{};
        if (stereo_handles && native_handles.fused &&
            (camera || lighting || screen_point || screen_rect || light_list)) {
            camera_right = constant && (camera || (lighting && ring_light))
                               ? constant->right
                               : NativeStereo::LastCameraRight();
        }

        write_descriptor(owner, camera_right);
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
    // Moonlight in the air (moon_shafts.h): the shadow pass's atlas, the light composite's.
    if (vr_moon.on && stage.l_stage == Shader::LogicalStage::Fragment &&
        Shader::Optimization::MoonShafts::IsShadowProgram(stage.pgm_hash)) [[unlikely]] {
        VrMoonCaptureAtlas(stage);
    }
    // The air pass (air_light.h): the moon's shadow pass's eye depth and picture, the composite's.
    if ((vr_moon.on || vr_fog.on) && stage.l_stage == Shader::LogicalStage::Fragment &&
        (Shader::Optimization::MoonShafts::IsShadowProgram(stage.pgm_hash) ||
         Shader::Optimization::BounceLight::IsCompositeProgram(stage.pgm_hash))) [[unlikely]] {
        VrAirCapture(stage);
    }
    // The bump map's class for this draw (a G-buffer program with one: BumpHeightPass).
    if (stage.l_stage == Shader::LogicalStage::Fragment) {
        vr_bump.draw_need = NeedKeep;
        if (stage.vr_bump_image < stage.images.size()) {
            const auto& desc = stage.images[stage.vr_bump_image];
            const auto tsharp = resource_bindings
                                    ? resource_bindings->image_values[stage.vr_bump_image].sharp
                                    : desc.GetSharp(stage);
            VrBumpClassify(stage, desc, tsharp);
        }
    }
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
        if (Shader::Optimization::BumpProbe::Dir() != nullptr &&
            stage.l_stage == Shader::LogicalStage::Fragment) [[unlikely]] {
            VrBumpProbe(stage, static_cast<u32>(&image_desc - stage.images.data()), tsharp, memory);
        }
        // (A lamp's shadow map: a small array of at least six layers, a cube's faces; never one
        // of the full-size targets the same shader also reads.)
        if (stage.vr_light_shadow.game_shadowed &&
            tsharp.GetType() == AmdGpu::ImageType::Color2DArray && tsharp.NumLayers() >= 6 &&
            tsharp.width + 1 <= 512u && tsharp.height + 1 <= 512u) [[unlikely]] {
            NoteGameLampMap(tsharp.Address());
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
        LOG_INFO(Render_Vulkan, "Begin-state memo: {} hits / {} misses / {} checked",
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
                       "Begin-state memo differs from full render state");
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
               "Begin-state memo differs from full render state");
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
        // (The shadow atlas held over frames, vr_shadow_hold.h: a held frame's first draw into
        // the nearest cascade must not clear the whole atlas, as the pending meta clear would:
        // the other cascades keep their picture, and DrawRecord clears the nearest's quarter.)
        const bool is_depth_clear =
            !vr_shadow_near_clear &&
            ((regs.depth_render_control.depth_clear_enable && regs.depth_control.depth_enable &&
              regs.depth_control.depth_write_enable) ||
             texture_cache.IsMetaCleared(htile_address, slice));
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

bool Rasterizer::InvalidateMemory(VAddr addr, u64 size,
                                 VideoCore::TextureMemory::Invalidation source) {
    Common::Slow::Scope slow{Common::Slow::GuestInvalidate, size};
    if (DA::Enabled() && !liverpool->IsRecordingThread())
        DA::Get().Write({{addr, size}, DA::Kind::CpuFault});
    if (!IsMapped(addr, size)) {
        // Not GPU mapped memory, can skip invalidation logic entirely.
        return false;
    }
    using VideoCore::TextureMemory::Invalidation;
    // Buffer page retirement includes protection changes; its coverage is independent
    // of which image bytes this notification can prove were written.
    const bool host = source == Invalidation::HostWrite || source == Invalidation::ProtectionChange;
    buffer_cache.InvalidateMemory(addr, size, host);
    texture_cache.InvalidateMemory(addr, size, source);
    return true;
}

void Rasterizer::NoteFreshRange(VAddr addr, u64 size) {
    if (IsMapped(addr, size)) {
        buffer_cache.NoteFreshRange(addr, size);
    }
}

bool Rasterizer::InvalidateWriteFaultPage(VAddr addr) {
    if (DA::Enabled() && !liverpool->IsRecordingThread())
        DA::Get().Write({{addr, 8}, DA::Kind::CpuFault});
    if (!IsMapped(addr, 8)) {
        return false;
    }
    buffer_cache.InvalidateWriteFault(addr, 8);
    texture_cache.InvalidateMemory(addr, 8, VideoCore::TextureMemory::Invalidation::AddressFault);
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
    texture_cache.InvalidateMemory(addr, 8, VideoCore::TextureMemory::Invalidation::AddressFault);
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
    texture_cache.InvalidateMemory(addr, 8, VideoCore::TextureMemory::Invalidation::AddressFault);
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
    Common::MemoryWriteTrace::Event("cpu.read.fault", addr, size);
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
    Common::MemoryWriteTrace::Event("gpu.unmap", addr, size);
    DrainDrawPipe(DrawPipe::RasterizerEntry);
    buffer_cache.InvalidateMemory(addr, size);
    texture_cache.UnmapMemory(addr, size);
    page_manager.OnGpuUnmap(addr, size);
    {
        std::scoped_lock lock{mapped_ranges_mutex};
        mapped_ranges -= decltype(mapped_ranges)::interval_type::right_open(addr, addr + size);
    }
}

void Rasterizer::UpdateDynamicState(const GraphicsPipeline* pipeline, const bool is_indexed,
                                    bool indirect, DynamicState* deferred) const {
    FastRender::BProfile::Scope b_profile{FastRender::BProfile::RecDynamicState};
    ScratchProfile::Cpu scratch_dynamic(29, "Rasterizer::UpdateDynamicState");
    // FAST_CPU_STAGEB_SPLIT stage 2: the draw's values go into split_dynamic (resolve side; it
    // keeps the game's last values), then the command buffer's tracker takes them through its
    // setters and commits what differs (encode side).
    const bool split = scheduler.StageBSplit();
    if (split) {
        if (!split_dynamic) {
            split_dynamic = std::make_unique<DynamicState>();
        }
        scheduler.SetDesiredDynamic(split_dynamic.get());
    }
    // Decide once for both the viewport and its memo key. Indirect arguments are GPU-owned;
    // CPU counts left by a previous draw must not turn scene jitter off for this one.
    std::array<float, 2> jitter{};
    if (vr_temporal && vr_temporal->active && !vr_temporal->resolved && db_desc.first &&
        db_desc.first == vr_temporal->depth_id) {
        const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
        jitter = TemporalDrawJitter(indirect, regs.num_indices, regs.num_instances.NumInstances(),
                                    vr_temporal->scene.Jitter());
    } else if (vr_dibr_upscale && db_desc.first && VR::DibrFrameActive()) {
        // The DIBR upscaler (vr_dibr_upscale.h): the same viewport jitter for the draws into a
        // depth of its input size (the centre's scene, not shadow maps, probes or light grids).
        const VR::TaaJitter frame_jitter = VR::FrameTaaJitter();
        if (frame_jitter.on && frame_jitter.viewport) {
            const auto& depth = db_desc.second.info; // the bound depth's own description
            if (depth.size.width == frame_jitter.width &&
                depth.size.height == frame_jitter.height) {
                const auto& regs = std::as_const(ScratchDraw::Registers(liverpool));
                jitter = TemporalDrawJitter(indirect, regs.num_indices,
                                            regs.num_instances.NumInstances(),
                                            {frame_jitter.px_x, frame_jitter.px_y});
            }
        }
    }
    if (FastRender::On(DynStateSkip)) {
        dyn_skip->Update(*this, pipeline, is_indexed, jitter);
    } else {
        UpdateViewportScissorState(pipeline, jitter);
        UpdateDepthStencilState();
        UpdatePrimitiveState(is_indexed);
    }
    UpdateRasterizationState();
    UpdateColorBlendingState(pipeline);
    if (vr_glow_depth_min >= 0.0f) {
        // After the depth state (DynSkip keeps the game's own): no depth test or write, as the
        // game draws it, and the bounds test keeps the glow over the far scene and the sky.
        auto& glow_state = scheduler.GetDynamicState();
        glow_state.SetDepthBoundsTestEnabled(true);
        glow_state.SetDepthBounds(vr_glow_depth_min, 1.0f);
    }

    if (split) {
        scheduler.SetDesiredDynamic(nullptr);
        if (deferred) {
            *deferred = *split_dynamic; // =2: fast:StageB2 commits it (SplitEncode)
            return;
        }
        auto& tracker = scheduler.RingDynamicState();
        tracker.SetFrom(*split_dynamic);
        tracker.Commit(instance, scheduler.Commands());
        return;
    }
    auto& dynamic_state = scheduler.GetDynamicState();
    dynamic_state.Commit(instance, scheduler.Commands());
}

void Rasterizer::UpdateViewportScissorState(const GraphicsPipeline* pipeline,
                                           std::array<float, 2> jitter) const {
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
            viewport.x += jitter[0] * vr_scene_inverse[0];
            viewport.y += jitter[1] * vr_scene_inverse[1];
            viewport.width = xscale * 2.0f;
            viewport.height = yscale * 2.0f;
            if (i == 0 && recording_vr.shadow_snapped) [[unlikely]] {
                // A shadow atlas tile, steadied (vr_shadow_hold.h): moved by less than a texel
                // and a little smaller about its centre.
                const auto& snap = recording_vr.shadow_snap;
                viewport.x = xoffset + snap[0] - xscale * snap[2];
                viewport.y = yoffset + snap[1] - yscale * snap[3];
                viewport.width = xscale * snap[2] * 2.0f;
                viewport.height = yscale * snap[3] * 2.0f;
            }
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
        if (drs.scissor_key) {
            S1Drs::ClipAxis(vp_scsr.top_left_x, vp_scsr.bottom_right_x, drs.width, drs.inset_x);
            S1Drs::ClipAxis(vp_scsr.top_left_y, vp_scsr.bottom_right_y, drs.height, drs.inset_y);
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
