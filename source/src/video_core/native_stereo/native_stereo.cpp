// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "core/vr/bloodborne_visceral_stage.h"
#include "native_stereo.h"
#include "video_core/fast/b_handler_profile.h"
#include "common/assert.h"
#include "core/vr/vr_session.h"
#include "core/vr/eye_distance.h"
#include "draw_memory.h"
#include "fused_pair.h"
#include "lighting_reconstruction.h"
#include "profile_scopes.h"
#include "registry_lookup_cache.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_constant_ring.h"
#include "video_core/texture_cache/image_info.h"
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fmt/ranges.h>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <tuple>
#include <unordered_map>
#include <vk_mem_alloc.h>
namespace NativeStereo {
struct BufferEntry { VkDeviceSize bytes{}; VmaAllocator allocator{}; bool stream{}; VkBuffer right{}; VmaAllocation allocation{}; };
namespace {
template<class H> u64 Key(H h) { return std::bit_cast<u64>(h); }
using Buffer = BufferEntry;
struct Views { vk::ImageView right{}, array{}; };
struct CameraPair { vk::DescriptorBufferInfo left, right; };
thread_local std::vector<CameraPair> camera_pairs;
// Generations change under the mutex; binding reads them unlocked (bit 25, Handles).
// buffer_generation changes whenever an entry is erased (entry pointers die).
struct Registry { std::recursive_mutex mutex; std::unordered_map<u64,Buffer> buffers; std::unordered_map<u64,Views> views; std::atomic<u64> view_generation{1}, buffer_generation{1}; ScratchRegistry::LookupCache<vk::ImageView> right_view_cache; };
// The environment switches below are read once at start-up: a plain load per use where a
// function-local static paid its guard (a thread-local read) on every call.
const u32 g_handles_verify_period = [] {
    const char* v = std::getenv("FAST_STEREO_HANDLES_VERIFY");
    const int n = v ? std::atoi(v) : 0;
    return n > 0 ? u32(n) : 0u;
}();
const bool g_batch_registry = [] {
    const char* setting = std::getenv("VR_NATIVE_BATCH_REGISTRY");
    return !setting || setting[0] != '0';
}();
const bool g_reuse_readonly_lookup = [] {
    const char* setting = std::getenv("VR_REUSE_READONLY_LOOKUP");
    return !setting || setting[0] != '0';
}();
const bool g_perf_check_set = std::getenv("VR_PERF_CHECK") != nullptr;
const bool g_shared_shadows = std::getenv("VR_NATIVE_SHARED_SHADOWS") != nullptr;
const bool g_reuse_camera = [] {
    const char* setting = std::getenv("VR_NATIVE_REUSE_CAMERA");
    return !setting || setting[0] != '0';
}();
const bool g_pack_camera = g_reuse_camera || std::getenv("VR_NATIVE_PACK_CAMERA") != nullptr;
const bool g_coalesce_copies = [] {
    const char* setting = std::getenv("VR_NATIVE_COALESCE_COPIES");
    return !setting || setting[0] != '0';
}();
const bool g_copy_transfer_barriers = std::getenv("VR_NATIVE_COPY_TRANSFER_BARRIERS") != nullptr;
const bool g_hle_outer_barriers = [] {
    const char* setting = std::getenv("VR_NATIVE_HLE_OUTER_BARRIERS");
    return !setting || setting[0] != '0';
}();
const bool g_copy_stats = std::getenv("VR_NATIVE_COPY_STATS") != nullptr;
const bool g_view_lookup_cache = std::getenv("VR_NATIVE_VIEW_LOOKUP_CACHE") != nullptr;
const bool g_view_lookup_check = std::getenv("VR_NATIVE_VIEW_LOOKUP_CHECK") != nullptr;
const bool g_descriptor_lookup_audit = std::getenv("VR_DESCRIPTOR_LOOKUP_AUDIT") != nullptr;
const bool g_camera_audit = std::getenv("VR_NATIVE_CAMERA_AUDIT") != nullptr;
// FAST_GPU_GODRAY_EYE (default on; '0': the god rays radiate from the flat game's point in both
// eyes, as before): the light glow's god rays (FS 0xe55e45eb) get their centre per eye.
const bool g_godray_eye = [] {
    const char* setting = std::getenv("FAST_GPU_GODRAY_EYE");
    return !setting || setting[0] != '0';
}();
// FAST_GPU_SSAO_HISTORY_EYE (on unless '0': free in the quiet uncapped A/B of 2026-10-15; '0': both eyes fetch the last frame's AO through the flat
// game's reprojection, as before): the temporal SSAO's history reprojection (ssao272 dwords
// 44..59) per eye, NativeProjection::EyeOcclusionHistory. Asymmetric projection only.
const bool g_ssao_history_eye = [] {
    const char* setting = std::getenv("FAST_GPU_SSAO_HISTORY_EYE");
    return !setting || setting[0] != '0';
}();
// FAST_CPU_LEAN_SCOPES: the registry's address once it exists, a plain load per R() (several per
// draw); '0': never published, every call takes the function-local static's guard as before.
std::atomic<Registry*> g_registry{};
SHAD_NO_INLINE Registry& FirstR() {
    static auto* state = [] {
        if (Enabled() && g_batch_registry) LOG_INFO(Render_Vulkan,"VR native descriptor registry: batch lock enabled");
        return new Registry;
    }();
    if (FastRender::g_lean_scopes) g_registry.store(state, std::memory_order_release);
    return *state;
}
Registry& R() {
    if (Registry* const state = g_registry.load(std::memory_order_acquire)) [[likely]] return *state;
    return FirstR();
}
// Bit 25 counters (per recording thread) and FAST_STEREO_HANDLES_VERIFY=N: every Nth hinted
// use is recomputed through the registry; the registry's answer is used on a mismatch.
struct HandleStats { u64 calls{}, buffer_hits{}, buffer_misses{}, image_hits{}, image_misses{}, barrier_hits{}, barrier_misses{}, stale{}, verified{}, mismatches{}; };
thread_local HandleStats handle_stats;
bool HandleVerifyNow() {
    static thread_local u32 tick = 0;
    return g_handles_verify_period != 0 && ++tick % g_handles_verify_period == 0;
}
// Index of `p` in the array at `base`, or ~0 when it points elsewhere.
template<class T> size_t HintIndex(const T* p, const T* base) {
    const auto offset = reinterpret_cast<std::uintptr_t>(p) - reinterpret_cast<std::uintptr_t>(base);
    return offset % sizeof(T) == 0 ? size_t(offset / sizeof(T)) : ~size_t{0};
}
// Caller holds R().mutex. The registry's entry wins on a mismatch.
Buffer* VerifyEntry(vk::Buffer b, Buffer* hinted, const char* what) {
    const auto it = R().buffers.find(Key(VkBuffer(b)));
    Buffer* const slow = it == R().buffers.end() ? nullptr : &it->second;
    ++handle_stats.verified;
    if (slow != hinted) {
        ++handle_stats.mismatches;
        LOG_ERROR(Render_Vulkan, "FAST stereo handles verify: {} buffer {:#x} entry mismatch #{}",
                  what, Key(VkBuffer(b)), handle_stats.mismatches);
    }
    return slow;
}
}
bool SharedShadowAtlas(const VideoCore::ImageInfo& info) {
    return Enabled() && g_shared_shadows && info.props.is_depth &&
        info.pixel_format == vk::Format::eD32Sfloat && info.size.width == 4096 &&
        info.size.height == 4096 && info.size.depth == 1 && info.resources.layers == 1 &&
        info.resources.levels == 1 && info.num_samples == 1;
}
namespace {
const auto g_ipd_override = VR::EyeDistance::NativeOverride(std::getenv("VR_NATIVE_IPD"));
const bool g_asymmetric = std::getenv("VR_NATIVE_ASYMMETRIC") != nullptr;
const bool g_mirror_flip = [] {
    const char* setting = std::getenv("FAST_GPU_MIRROR_FLIP");
    return !setting || setting[0] != '0';
}();
// FAST_GPU_MIRROR_ABOVE (on unless '0', read once): a mirrored camera may sit on the kept side of
// its plane too (NativeProjection::MirroredCamera's either_side), when the record has the render
// camera.
const bool g_mirror_above = [] {
    const char* setting = std::getenv("FAST_GPU_MIRROR_ABOVE");
    return !setting || setting[0] != '0';
}();
// FAST_GPU_CLIP_PLANE_EYE (on unless '0', read once): with the asymmetric projection, every paired
// scene camera with a clip plane (dwords 196..199 not 0, 0, 0, 1) gets it per eye
// (NativeProjection::EyeClipPlane), not only a mirrored one: the plane is in clip space, so the
// eye's own projection must carry it too. The frame camera (no plane) keeps its bits.
const bool g_clip_plane_eye = [] {
    const char* setting = std::getenv("FAST_GPU_CLIP_PLANE_EYE");
    return !setting || setting[0] != '0';
}();
// FAST_GPU_LIGHT_EYE (on unless '0': free in the quiet uncapped A/B of 2026-10-15; read once): with the asymmetric projection, a deferred light's
// list gets each eye's copy (LightListBuffer), its lights where that eye sees them.
const bool g_light_eye = [] {
    const char* setting = std::getenv("FAST_GPU_LIGHT_EYE");
    return !setting || setting[0] != '0';
}();
// VR_LIGHT_LIST_AUDIT (diagnostic, read once): the first 256 lists LightListBuffer pairs, each
// light's flat view position and distance from the camera (e.g. to find the hunter's lantern).
const bool g_light_list_audit = std::getenv("VR_LIGHT_LIST_AUDIT") != nullptr;
}
float Ipd(const VR::CameraRecord& record) {
    return VR::EyeDistance::Native(record.ipd, g_ipd_override);
}
bool AsymmetricProjection() { return Enabled() && g_asymmetric; }
bool MirrorFlip() { return g_mirror_flip && AsymmetricProjection(); }
void ClearCameraPairs() { camera_pairs.clear(); }
void RegisterCameraPair(vk::DescriptorBufferInfo left, vk::DescriptorBufferInfo right) {
    camera_pairs.push_back({left, right});
}
vk::DescriptorBufferInfo LastCameraRight() {
    return camera_pairs.empty() ? vk::DescriptorBufferInfo{} : camera_pairs.back().right;
}
static u64 PairTick(VideoCore::StreamBuffer& stream) { return stream.scheduler->CurrentTick(); }
static u64 PairTick(Vulkan::ConstantRing& stream) { return stream.AllocationGeneration(); }

// Captured Bloodborne scene CB: a perspective projection and matching viewport dimensions.
// Reject other 864-byte buffers instead of patching them by size alone.
static bool SceneLayout(const float* c) {
    return c[4]>100 && c[5]>100 && std::abs(c[5]*c[7]-1.f)<.001f && c[52]>0 && c[57]>0 &&
           c[66]==1.f && c[67]==0.f && std::abs(c[195]-1.f)<.0001f && c[50]<0.f && c[51]>0.f;
}
static std::array<NativeProjection::Transform,2> EyeProjections(const float* center,
                                                               const VR::CameraRecord& record) {
    std::array<NativeProjection::Transform,2> projections{};
    if (AsymmetricProjection()) {
        for (u32 eye=0; eye<2; ++eye) {
            const auto& v = record.eye_fov[eye];
            projections[eye] = NativeProjection::Make(
                {v.angle_left,v.angle_right,v.angle_up,v.angle_down},
                1.f/center[52],1.f/center[57], false);
        }
        if (!projections[0].valid || !projections[1].valid) projections = {};
    }
    return projections;
}
// NativeProjection::MirroredCamera with the frame's render camera, when the record has it.
static bool Mirrored(const float* center, const VR::CameraRecord& record) {
    const std::array<float,3> render{record.render_position.x, record.render_position.y,
                                     record.render_position.z};
    return NativeProjection::MirroredCamera(center,
                                            record.has_render_position ? render.data() : nullptr,
                                            g_mirror_above);
}
bool SceneCamera(const float* source, const VR::CameraRecord& record, bool* mirrored) {
    if (mirrored) *mirrored = false;
    if (!Enabled() || Ipd(record)==0.f) return false;
    std::array<float, 216> center;
    std::memcpy(center.data(), ScratchDraw::MemorySource(source, sizeof(center)), sizeof(center));
    if (!SceneLayout(center.data())) return false;
    if (record.has_render_position && !Mirrored(center.data(), record)) {
        const float expected[3]{record.render_position.x,record.render_position.y,record.render_position.z};
        const float actual[3]{center[183],center[187],center[191]};
        VR::Bloodborne::TraceVisceralDraw(record.id, expected, actual, center.data());
    }
    if (mirrored)
        *mirrored = EyeProjections(center.data(), record)[0].valid &&
                    Mirrored(center.data(), record);
    return true;
}

template<class Upload>
static bool CameraBufferImpl(Upload& stream, const float* source, u32 alignment, const VR::CameraRecord& record, vk::DescriptorBufferInfo& left, CameraConstants* capture, bool* projection_paired, bool* mirrored) {
    if (projection_paired) *projection_paired = false;
    if (mirrored) *mirrored = false;
    ScratchProfile::Cpu scratch_camera(18,"NativeStereo::CameraBuffer");
    FastRender::BProfile::Scope b_profile{FastRender::BProfile::NativePairing};
    if (!Enabled() || Ipd(record)==0.f) return false;
    std::array<float, 216> center;
    std::memcpy(center.data(),
                ScratchDraw::MemorySource(source, sizeof(center)),
                sizeof(center));
    if (!SceneLayout(center.data())) return false;
    const float scale = record.world_scale;
    const float ipd = Ipd(record);
    if (record.has_render_position && !Mirrored(center.data(), record)) {
        const float expected[3]{record.render_position.x,record.render_position.y,record.render_position.z};
        const float actual[3]{center[183],center[187],center[191]};
        VR::Bloodborne::TraceVisceralDraw(record.id, expected, actual, center.data());
    }
    auto projections = EyeProjections(center.data(), record);
    // A planar reflection's mirrored camera: each eye's frustum upside down, so the water's
    // flipped read lands where that eye sees the reflection (NativeProjection::MirroredCamera).
    const bool mirror = mirrored && projections[0].valid && Mirrored(center.data(), record);
    if (mirror) {
        for (auto& t : projections) t = NativeProjection::Flipped(t);
        *mirrored = true;
    }
    if (projection_paired) *projection_paired = projections[0].valid && projections[1].valid;
    const bool clip_plane = center[196] != 0.f || center[197] != 0.f || center[198] != 0.f ||
                            center[199] != 1.f;
    const bool reuse_camera = g_reuse_camera;
    struct CameraMemo {
        std::array<float,216> center{};
        std::array<vk::DescriptorBufferInfo,2> pair{};
        u64 stream_serial{}, generation{}, tick{};
        u32 scale_bits{}, ipd_bits{};
        CameraConstants captured{};
        bool capture_valid{};
        std::array<NativeProjection::Transform,2> projections{};
        // The mirror flip's own (a symmetric eye's flipped frustum is its own; the per-eye clip
        // plane is not, and the render camera, not the bytes, can change the answer).
        bool mirror{};
        bool valid{};
    };
    static thread_local CameraMemo memo;
    static thread_local u64 hits{}, misses{};
    const auto tick = PairTick(stream);
    if (reuse_camera && memo.valid && memo.stream_serial == stream.perf_serial &&
        memo.generation == stream.AllocationGeneration() && memo.tick == tick &&
        memo.scale_bits == std::bit_cast<u32>(scale) &&
        memo.ipd_bits == std::bit_cast<u32>(ipd) && memo.projections == projections &&
        memo.mirror == mirror && (!capture || memo.capture_valid) &&
        std::memcmp(center.data(), memo.center.data(), sizeof(center)) == 0) {
        ++hits;
        left = memo.pair[0];
        if (capture) *capture = memo.captured;
        camera_pairs.push_back({memo.pair[0], memo.pair[1]});
        return true;
    }
    if (reuse_camera) {
        ++misses;
        if ((misses & 1023) == 0)
            LOG_INFO(Render_Vulkan, "VR camera upload reuse: {} hits / {} misses", hits, misses);
    }
    std::array<vk::DescriptorBufferInfo,2> pair;
    const bool pack = g_pack_camera;
    std::array<std::array<float,216>,2> host;
    for (u32 eye=0; eye<2; ++eye) {
        auto f=center;
        const float e=(eye ? .5f : -.5f)*ipd*scale;
        f[11]-=e; // row zero of the 3x4 view matrix
        for (u32 r=0;r<3;++r) f[183+4*r]+=center[180+4*r]*e;
        for (u32 r=0;r<4;++r) for (u32 c=0;c<4;++c) {
            f[20+4*r+c]+=center[180+4*r]*e*center[48+c];
            for (u32 block : {112u,128u,144u,160u})
                f[block+4*r+c]+=center[block+4*r]*e*center[52]*center[48+c];
        }
        if (projections[eye].valid) NativeProjection::Scene(f.data(), projections[eye]);
        if (mirror || (g_clip_plane_eye && projections[eye].valid && clip_plane))
            NativeProjection::EyeClipPlane(f.data(), center.data(), e, projections[eye]);
        if (capture) (*capture)[eye] = f;
        if (pack) host[eye] = f;
        else {
            const u64 offset=stream.Copy(f.data(),sizeof(f),alignment);
            pair[eye]={stream.Handle(),offset,sizeof(f)};
        }
    }
    if (pack) {
        // One allocation and retirement record, with the same per-eye alignment and bytes.
        const u64 bytes = sizeof(host[0]);
        const u64 stride = alignment ? ((bytes + alignment - 1) / alignment) * alignment : bytes;
        const auto [mapped, offset] = stream.Map(stride + bytes, alignment);
        ASSERT(mapped);
        std::memcpy(mapped, host[0].data(), bytes);
        std::memcpy(mapped + stride, host[1].data(), bytes);
        stream.Commit();
        pair[0] = {stream.Handle(), offset, bytes};
        pair[1] = {stream.Handle(), offset + stride, bytes};
    }
    if (reuse_camera) {
        memo.center = center;
        memo.capture_valid = capture != nullptr;
        if (capture) memo.captured = *capture;
        memo.projections = projections;
        memo.mirror = mirror;
        memo.pair = pair;
        memo.stream_serial = stream.perf_serial;
        memo.generation = stream.AllocationGeneration();
        memo.tick = PairTick(stream);
        memo.scale_bits = std::bit_cast<u32>(scale);
        memo.ipd_bits = std::bit_cast<u32>(ipd);
        memo.valid = true;
    }
    left=pair[0]; camera_pairs.push_back({pair[0],pair[1]}); return true;
}
// Bloodborne's deferred-light variants share a camera layout even as light counts
// and types produce new shader hashes. Recognize that layout rather than a partial
// list of programs, so turning toward a new light cannot retain a symmetric inverse.
template<class Upload>
static bool LightingBufferImpl(Upload& stream, const float* source, u32 size,
                    u32 alignment, const VR::CameraRecord& record,
                    vk::DescriptorBufferInfo& left, bool mirror, float* world_scale = nullptr) {
    if (!AsymmetricProjection()) return false;
    source =
        static_cast<const float *>(ScratchDraw::MemorySource(source, size));
    const auto layout = NativeProjection::RecognizeLighting({source,size/sizeof(float)});
    if (layout == NativeProjection::LightingLayout::None) return false;
    const bool decal = layout == NativeProjection::LightingLayout::Decal;
    std::array<NativeProjection::Transform,2> transforms;
    // Use this pass's inverse projection, matching CameraBuffer's per-camera
    // decision. Auxiliary narrow-FOV cameras must not inherit the main eye remap.
    const float* inverse_projection = source + (decal ? 16 : 0);
    const float tan_x = inverse_projection[0], tan_y = inverse_projection[5];
    // RecognizeLighting already validated this inverse projection, including the
    // bounded roundoff allowance. Do not repeat an exact-one check here.
    for (u32 eye=0; eye<2; ++eye) {
        const auto& v = record.eye_fov[eye];
        transforms[eye] = NativeProjection::Make(
            {v.angle_left,v.angle_right,v.angle_up,v.angle_down}, tan_x,tan_y, false);
    }
    if (!transforms[0].valid || !transforms[1].valid) return false;
    // A light of a planar reflection's pass: reconstructed in its camera's flipped frustum.
    if (mirror) for (auto& t : transforms) t = NativeProjection::Flipped(t);
    // The temporal SSAO's history reprojection, once per buffer: both eyes conjugate the same move.
    const bool history = g_ssao_history_eye &&
                         layout == NativeProjection::LightingLayout::Occlusion &&
                         NativeProjection::OcclusionHistoryMove(source);
    const u64 stride = alignment ? ((size + alignment - 1) / alignment) * alignment : size;
    const auto [mapped, offset] = stream.Map(stride + size, alignment);
    ASSERT(mapped);
    for (u32 eye=0; eye<2; ++eye) {
        std::array<float,104> values{};
        std::memcpy(values.data(),source,size);
        const float e = (eye ? .5f : -.5f) * Ipd(record) * record.world_scale;
        NativeProjection::RemapLighting(values.data(),source,layout,e,transforms[eye],history);
        std::memcpy(mapped+eye*stride,values.data(),size);
    }
    stream.Commit();
    left = {stream.Handle(),offset,size};
    camera_pairs.push_back({left,{stream.Handle(),offset+stride,size}});
    if (world_scale) *world_scale = record.world_scale;
    return true;
}
bool LightListShader(u64 pgm_hash) {
    return g_light_eye && AsymmetricProjection() &&
           NativeProjection::LightListLayout(pgm_hash).kind != NativeProjection::LightKind::None;
}
bool LightListBuffer(VideoCore::StreamBuffer& stream, const float* source, u32 size,
                     u32 alignment, bool guest, u64 pgm_hash, float world_scale, float ipd,
                     vk::DescriptorBufferInfo& left) {
    using NativeProjection::LightList;
    const LightList list = NativeProjection::LightListFor(pgm_hash, size);
    if (!g_light_eye || !AsymmetricProjection() || list.kind == NativeProjection::LightKind::None ||
        size > LightList::MaxBytes || ipd == 0.f || !(world_scale > 0.f) ||
        !std::isfinite(world_scale))
        return false;
    // One read of the list (guest memory may change under it); both eyes are made from it.
    std::array<float, LightList::MaxBytes / sizeof(float)> center;
    std::memcpy(center.data(), guest ? ScratchDraw::MemorySource(source, size) : source, size);
    std::array<std::array<float, LightList::MaxBytes / sizeof(float)>, 2> eyes;
    for (u32 eye = 0; eye < 2; ++eye) {
        // LightingBuffer's shift of the draw's light208 (and CameraBuffer's of its scene camera).
        const float e = (eye ? .5f : -.5f) * ipd * world_scale;
        std::memcpy(eyes[eye].data(), center.data(), size);
        if (!NativeProjection::RemapLightList(eyes[eye].data(), center.data(), list, e))
            return false;
    }
    const u64 stride = alignment ? ((size + alignment - 1) / alignment) * alignment : size;
    const auto [mapped, offset] = stream.Map(stride + size, alignment);
    ASSERT(mapped);
    std::memcpy(mapped, eyes[0].data(), size);
    std::memcpy(mapped + stride, eyes[1].data(), size);
    stream.Commit();
    left = {stream.Handle(), offset, size};
    camera_pairs.push_back({left, {stream.Handle(), offset + stride, size}});
    // The first three of each kind.
    static std::array<std::atomic<u32>, 3> logged{};
    if (logged[static_cast<u32>(list.kind) - 1].fetch_add(1, std::memory_order_relaxed) < 3)
        LOG_INFO(Render_Vulkan,
                 "VR light list: {} {} light(s) per eye (fs {:#x}, {} bytes): light 0 view x "
                 "{:.4f} -> left {:.4f} right {:.4f} (world scale {})",
                 u32(list.count), NativeProjection::Name(list.kind), pgm_hash, size, center[0],
                 eyes[0][0], eyes[1][0], world_scale);
    static std::atomic<u32> audited{0};
    if (g_light_list_audit && audited.fetch_add(1, std::memory_order_relaxed) < 256) {
        std::string lights;
        for (u32 i = 0; i < list.count; ++i) {
            const float* p = center.data() + list.Stride() * i;
            lights += fmt::format(" ({:.3f}, {:.3f}, {:.3f}: {:.2f} m)", p[0], p[1], p[2],
                                  std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) / world_scale);
        }
        LOG_INFO(Render_Vulkan, "VR light list audit: fs {:#x}, {} {} light(s), flat view:{}",
                 pgm_hash, u32(list.count), NativeProjection::Name(list.kind), lights);
    }
    return true;
}
bool ScreenPointShader(u64 pgm_hash) {
    return NativeProjection::ScreenPointShader(pgm_hash) &&
           (g_godray_eye || pgm_hash != 0xe55e45eb);
}
bool ScreenPointBuffer(VideoCore::StreamBuffer& stream, const float* source, u32 size,
                       u32 alignment, bool guest, u64 pgm_hash, const VR::CameraRecord& record,
                       vk::DescriptorBufferInfo& left) {
    const int dword = NativeProjection::ScreenPointDword(pgm_hash);
    const u32 at = u32(dword);
    if (dword < 0 || !AsymmetricProjection() || size < (at + 2) * sizeof(float) || size > 4096 ||
        !(record.vfov > 0) || !(record.aspect > 0))
        return false;
    if (guest)
        source = static_cast<const float*>(ScratchDraw::MemorySource(source, size));
    if (!std::isfinite(source[at]) || !std::isfinite(source[at + 1]))
        return false;
    const float ty = std::tan(record.vfov * .5f);
    std::array<NativeProjection::Transform, 2> transforms;
    for (u32 eye = 0; eye < 2; ++eye) {
        const auto& v = record.eye_fov[eye];
        transforms[eye] = NativeProjection::Make({v.angle_left, v.angle_right, v.angle_up,
                                                  v.angle_down}, ty * record.aspect, ty, false);
        if (!transforms[eye].valid)
            return false;
    }
    const u64 stride = alignment ? ((size + alignment - 1) / alignment) * alignment : size;
    const auto [mapped, offset] = stream.Map(stride + size, alignment);
    ASSERT(mapped);
    for (u32 eye = 0; eye < 2; ++eye) {
        std::array<float, 1024> values{};
        std::memcpy(values.data(), source, size);
        const auto p = NativeProjection::RemapScreenPoint(source[at], source[at + 1], transforms[eye]);
        values[at] = p[0];
        values[at + 1] = p[1];
        std::memcpy(mapped + eye * stride, values.data(), size);
    }
    stream.Commit();
    left = {stream.Handle(), offset, size};
    camera_pairs.push_back({left, {stream.Handle(), offset + stride, size}});
    // The first three of each shader: the light shafts' and the glow god rays'.
    static std::array<std::atomic<u32>, 2> logged{};
    const bool shafts = pgm_hash == 0xd506a1;
    if (logged[shafts ? 0 : 1].fetch_add(1, std::memory_order_relaxed) < 3) {
        const auto l = NativeProjection::RemapScreenPoint(source[at], source[at + 1], transforms[0]);
        const auto r = NativeProjection::RemapScreenPoint(source[at], source[at + 1], transforms[1]);
        LOG_INFO(Render_Vulkan,
                 "VR {}: per-eye centre: flat ({:.3f}, {:.3f}) -> left ({:.3f}, {:.3f}) "
                 "right ({:.3f}, {:.3f}) (fs {:#x}, dword {})",
                 shafts ? "light shafts" : "glow god rays", source[at], source[at + 1], l[0], l[1],
                 r[0], r[1], pgm_hash, at);
    }
    return true;
}
bool ScreenRectBuffer(VideoCore::StreamBuffer& stream, const u32* source, u32 size, u32 alignment,
                      bool guest, const std::array<u8, 6>& dwords, u32 width, u32 height,
                      std::array<u32, 2> group, const VR::CameraRecord& record,
                      vk::DescriptorBufferInfo& left, std::array<u32, 2>& groups) {
    constexpr u8 None = 0xFF;
    // How near an effect's eye parallax is covered (the left eye sees near things further right).
    constexpr float Nearest = 0.3f;
    if (size > 4096 || !width || !height || !group[0] || !group[1] || !(record.vfov > 0) ||
        !(record.aspect > 0))
        return false;
    for (const u8 d : {dwords[0], dwords[1], dwords[4], dwords[5]})
        if (d == None || (d + 1u) * 4u > size)
            return false;
    const bool read = dwords[2] != None;
    if (read && ((dwords[2] + 1u) * 4u > size || (dwords[3] + 1u) * 4u > size))
        return false;
    if (guest)
        source = static_cast<const u32*>(ScratchDraw::MemorySource(source, size));
    const std::array<u32, 4> rect{source[dwords[0]], source[dwords[1]], source[dwords[4]],
                                  source[dwords[5]]};
    // Only a rectangle read and written at the same pixels (a grab of the eye image).
    if (rect[0] >= rect[2] || rect[1] >= rect[3] || rect[2] > width || rect[3] > height ||
        (read && (source[dwords[2]] != rect[0] || source[dwords[3]] != rect[1])))
        return false;
    const float ty = std::tan(record.vfov * .5f);
    const float shift = .5f * Ipd(record) * record.world_scale / (ty * record.aspect);
    std::array<std::array<u32, 4>, 2> eyes;
    for (u32 eye = 0; eye < 2; ++eye) {
        NativeProjection::Transform t{.valid = true};
        if (AsymmetricProjection()) {
            const auto& v = record.eye_fov[eye];
            t = NativeProjection::Make({v.angle_left, v.angle_right, v.angle_up, v.angle_down},
                                       ty * record.aspect, ty, false);
            if (!t.valid)
                return false;
        }
        const float parallax = .5f * t.x_scale * shift / Nearest * float(width);
        eyes[eye] = NativeProjection::RemapScreenRect(rect, width, height, t, eye == 0, parallax);
    }
    groups = {0, 0};
    for (const auto& e : eyes) {
        groups[0] = std::max(groups[0], (e[2] - e[0] + group[0] - 1) / group[0]);
        groups[1] = std::max(groups[1], (e[3] - e[1] + group[1] - 1) / group[1]);
    }
    const u64 stride = alignment ? ((size + alignment - 1) / alignment) * alignment : size;
    const auto [mapped, offset] = stream.Map(stride + size, alignment);
    ASSERT(mapped);
    for (u32 eye = 0; eye < 2; ++eye) {
        std::array<u32, 1024> values{};
        std::memcpy(values.data(), source, size);
        const auto& e = eyes[eye];
        values[dwords[0]] = e[0];
        values[dwords[1]] = e[1];
        values[dwords[4]] = e[2];
        values[dwords[5]] = e[3];
        if (read) {
            values[dwords[2]] = e[0];
            values[dwords[3]] = e[1];
        }
        std::memcpy(mapped + eye * stride, values.data(), size);
    }
    stream.Commit();
    left = {stream.Handle(), offset, size};
    camera_pairs.push_back({left, {stream.Handle(), offset + stride, size}});
    static std::atomic<u32> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 3)
        LOG_INFO(Render_Vulkan,
                 "VR screen rect: flat [{}, {}) x [{}, {}) -> left [{}, {}) x [{}, {}), right "
                 "[{}, {}) x [{}, {}) of {}x{}",
                 rect[0], rect[2], rect[1], rect[3], eyes[0][0], eyes[0][2], eyes[0][1],
                 eyes[0][3], eyes[1][0], eyes[1][2], eyes[1][1], eyes[1][3], width, height);
    return true;
}
bool CameraBuffer(VideoCore::StreamBuffer& stream, const float* source, u32 alignment,
                  const VR::CameraRecord& record, vk::DescriptorBufferInfo& left,
                  CameraConstants* capture, bool* projected, bool* mirrored) {
    return CameraBufferImpl(stream, source, alignment, record, left, capture, projected, mirrored);
}
bool LightingBuffer(VideoCore::StreamBuffer& stream, const float* source, u32 bytes,
                    u32 alignment, const VR::CameraRecord& record, vk::DescriptorBufferInfo& left,
                    bool mirror, float* world_scale) {
    return LightingBufferImpl(stream, source, bytes, alignment, record, left, mirror, world_scale);
}
bool CameraBuffer(Vulkan::ConstantRing& stream, const float* source, u32 alignment,
                  const VR::CameraRecord& record, vk::DescriptorBufferInfo& left,
                  vk::DescriptorBufferInfo& right, bool* projected, bool* mirrored) {
    ClearCameraPairs();
    const bool result = CameraBufferImpl(stream, source, alignment, record, left, nullptr, projected, mirrored);
    if (result) right = camera_pairs.back().right;
    return result;
}
// Stage A does not know the pass a light belongs to: the recording thread pairs the lights of a
// planar reflection's pass again, upside down (Rasterizer::BindBuffers,
// NativeProjection::MirrorPass).
bool LightingBuffer(Vulkan::ConstantRing& stream, const float* source, u32 bytes,
                    u32 alignment, const VR::CameraRecord& record,
                    vk::DescriptorBufferInfo& left, vk::DescriptorBufferInfo& right) {
    ClearCameraPairs();
    const bool result = LightingBufferImpl(stream, source, bytes, alignment, record, left, false);
    if (result) right = camera_pairs.back().right;
    return result;
}
std::array<std::array<float,4>,2> DrawProjection(const VR::CameraRecord& record, bool mirror) {
    // Every draw of a camera update asks again: memoized by the exact bits of its inputs.
    struct Memo { std::array<float,10> key{}; std::array<std::array<float,4>,2> value{}; bool valid{}; };
    thread_local Memo memo;
    const auto& l=record.eye_fov[0]; const auto& r=record.eye_fov[1];
    const std::array<float,10> key{record.vfov,record.aspect,l.angle_left,l.angle_right,l.angle_up,
                                   l.angle_down,r.angle_left,r.angle_right,r.angle_up,r.angle_down};
    if (!memo.valid || std::memcmp(memo.key.data(),key.data(),sizeof(key))!=0) {
        std::array<std::array<float,4>,2> result{};
        const float ty=std::tan(record.vfov*.5f);
        for (u32 eye=0;eye<2;++eye) {
            const auto& f=record.eye_fov[eye];
            const auto t=NativeProjection::Make({f.angle_left,f.angle_right,f.angle_up,f.angle_down},ty*record.aspect,ty,false);
            if (t.valid) result[eye]={t.x_scale,t.y_scale,t.x_offset,t.y_offset};
        }
        if (result[0][0]<=0 || result[1][0]<=0) result={};
        memo={key,result,true};
    }
    if (!mirror || memo.value[0][0]<=0) return memo.value;
    auto flipped=memo.value; // NativeProjection::Flipped
    for (auto& eye : flipped) eye[3]=NativeProjection::FlippedOffset(eye[3]);
    return flipped;
}
bool LightTile(const AmdGpu::Regs& regs, std::array<float,4>* tile_rect) {
    if (!AsymmetricProjection() || !regs.mode_control.vport_scissor_enable || regs.IsClipDisabled()) return false;
    const auto& tile=regs.viewport_scissors[0];
    const auto& screen=regs.screen_scissor;
    if (tile.top_left_x<=screen.top_left_x && tile.top_left_y<=screen.top_left_y &&
        tile.bottom_right_x>=screen.bottom_right_x && tile.bottom_right_y>=screen.bottom_right_y) return false;
    const bool offset=!regs.window_scissor.window_offset_disable;
    const int ox=offset?regs.window_offset.window_x_offset:0, oy=offset?regs.window_offset.window_y_offset:0;
    const auto& win=regs.window_scissor; const auto& gen=regs.generic_scissor;
    const std::array<float,4> rect{
        float(std::max({int(screen.top_left_x),int(s16(win.top_left_x))+ox,int(s16(gen.top_left_x))+ox,int(s16(tile.top_left_x))})),
        float(std::max({int(screen.top_left_y),int(s16(win.top_left_y))+oy,int(s16(gen.top_left_y))+oy,int(s16(tile.top_left_y))})),
        float(std::min({int(screen.bottom_right_x),int(win.bottom_right_x)+ox,int(gen.bottom_right_x)+ox,int(tile.bottom_right_x)})),
        float(std::min({int(screen.bottom_right_y),int(win.bottom_right_y)+oy,int(gen.bottom_right_y)+oy,int(tile.bottom_right_y)}))};
    // An empty tile draws nothing: the plain path's hardware scissor is empty too, and no
    // light-scissor permutation is made for it (most tile draws were empty, each baking its
    // own bounds into a new fragment shader).
    if (rect[0]>=rect[2] || rect[1]>=rect[3]) return false;
    if (tile_rect) *tile_rect=rect;
    return true;
}
bool LightScissors(const AmdGpu::Regs& regs, const VR::CameraRecord& record,
                   std::array<std::array<float,4>,2>& result, bool mirror) {
    std::array<float,4> rect;
    if (!LightTile(regs,&rect)) return false;
    const auto& vp=regs.viewports[0]; const auto& ctl=regs.viewport_control;
    const std::array<float,4> viewport{ctl.xoffset_enable?vp.xoffset:0.f,ctl.yoffset_enable?vp.yoffset:0.f,
                                    ctl.xscale_enable?vp.xscale:1.f,ctl.yscale_enable?vp.yscale:1.f};
    const float ty=std::tan(record.vfov*.5f);
    for (u32 eye=0;eye<2;++eye) {
        const auto& f=record.eye_fov[eye];
        const auto t=NativeProjection::Make({f.angle_left,f.angle_right,f.angle_up,f.angle_down},ty*record.aspect,ty,false);
        if (!t.valid) return false;
        result[eye]=NativeProjection::RemapScissor(rect,viewport,mirror ? NativeProjection::Flipped(t) : t);
    }
    return true;
}
void AuditCameraBuffer(const float* f, u32 size, u64 shader, u32 stage, u32 binding,
                       bool mapped, bool projected, const VR::CameraRecord& record, bool flipped) {
    if (!AsymmetricProjection()) return;
    f = static_cast<const float *>(ScratchDraw::MemorySource(f, size));
    const bool scene = size==864 && f[4]>100 && f[5]>100 &&
        std::abs(f[5]*f[7]-1)<.001f && f[52]>0 && f[57]>0 && f[66]==1 && f[67]==0 &&
        std::abs(f[195]-1)<.0001f && f[50]<0 && f[51]>0;
    const auto layout=NativeProjection::RecognizeLighting({f,size/sizeof(float)});
    const bool candidate=scene || NativeProjection::InversePerspective(f) ||
        (size==416 && NativeProjection::InversePerspective(f+16));
    if (!candidate) return;
    const bool known=scene || layout!=NativeProjection::LightingLayout::None;
    const float tx=scene ? 1.f/f[52] : f[layout==NativeProjection::LightingLayout::Decal?16:0];
    const float ty=scene ? 1.f/f[57] : f[layout==NativeProjection::LightingLayout::Decal?21:5];
    std::array<bool,2> covered{}, valid_eyes{};
    for (u32 eye=0;eye<2;++eye) {
        const auto& v=record.eye_fov[eye];
        const std::array<float,4> fov{v.angle_left,v.angle_right,v.angle_up,v.angle_down};
        covered[eye]=NativeProjection::Make(fov,tx,ty).valid;
        valid_eyes[eye]=NativeProjection::Make(fov,tx,ty,false).valid;
    }
    const char* status=!known ? "unrecognized-layout" :
        (!valid_eyes[0] || !valid_eyes[1]) ? "invalid-eye-FOV" :
        !mapped ? "not-remapped" : !projected ? "not-projected" : "remapped";
    // Keep refusals AND later successes: startup without located eyes must not hide
    // a later unrecognized camera. Coverage is diagnostic, not an inconsistent
    // per-pass fallback that would leave depth and reconstruction in different spaces.
    // A planar reflection's mirrored camera (as CameraBuffer tells it) says so at the end, and
    // whether it was paired upside down (`flipped`: FAST_GPU_MIRROR_FLIP with the asymmetric
    // projection and the per-draw push block); a light of its pass that was says so too.
    const u32 mirror = flipped ? (scene ? 1 : 2) : scene && Mirrored(f, record) ? 3 : 0;
    static constexpr std::array<const char*,4> mirror_tags{
        "", " mirrored-camera flipped", " mirror-pass flipped", " mirrored-camera unflipped"};
    // A paired ssao272's history reprojection (FAST_GPU_SSAO_HISTORY_EYE): per eye, or the flat
    // camera's where NativeProjection::OcclusionHistoryMove refused it.
    const u32 history =
        !(layout==NativeProjection::LightingLayout::Occlusion && mapped && g_ssao_history_eye) ? 0 :
        NativeProjection::OcclusionHistoryMove(f) ? 1 : 2;
    static constexpr std::array<const char*,3> history_tags{
        "", " history per-eye", " history flat"};
    static thread_local std::set<std::tuple<u64,u32,u32,u32,std::string,bool,bool,u32,u32>> seen;
    if (seen.emplace(shader,stage,binding,size,status,covered[0],covered[1],mirror,
                     history).second) {
        if (!known) LOG_INFO(Render_Vulkan,"VR camera unrecognized constants: shader {:#x} stage {} binding {} bytes {} data {}",
                             shader,stage,binding,size,fmt::join(std::span{f,size/sizeof(float)}," "));
        LOG_INFO(Render_Vulkan,"VR camera audit: shader {:#x} stage {} binding {} bytes {} layout {} status {} coverage L{} R{} guest tan {} {}{}{}",
                 shader,stage,binding,size,scene?"scene864":NativeProjection::Name(layout),status,
                 covered[0],covered[1],tx,ty,mirror_tags[mirror],history_tags[history]);
    }
}
BufferEntry* RegisterBuffer(vk::Buffer b,VkDeviceSize n,VmaAllocator allocator,bool stream) {
    if(!Enabled()||!b) return nullptr;
    std::lock_guard lock(R().mutex);
    auto& meta=R().buffers[Key(VkBuffer(b))];
    ASSERT(!meta.right); meta={n,allocator,stream};
    return &meta; // unordered_map nodes keep their address until erased
}
void ForgetBuffer(vk::Buffer b) {
    if(!Enabled()||!b) return;
    std::lock_guard lock(R().mutex);
    auto it=R().buffers.find(Key(VkBuffer(b)));
    if(it==R().buffers.end()) return;
    if(it->second.right) vmaDestroyBuffer(it->second.allocator,it->second.right,it->second.allocation);
    R().buffers.erase(it);
    ++R().buffer_generation;
}
vk::Buffer ExistingRight(vk::Buffer b) {
    if(!Enabled()||!b) return {};
    std::lock_guard lock(R().mutex);
    auto it=R().buffers.find(Key(VkBuffer(b)));
    return it==R().buffers.end()?vk::Buffer{}:vk::Buffer{it->second.right};
}
void Barrier(Vulkan::Scheduler& s) {
    s.EndRendering();
    const vk::MemoryBarrier2 m{.srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
        .srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
        .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
        .dstAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite};
    s.Commands().pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount=1,.pMemoryBarriers=&m});
}
// Caller holds the registry mutex; `meta` is b's own entry (looked up or carried, bit 25).
// meta.right is stored atomically: FAST_CPU_FUSED_PAIR reads carried entries without the lock.
static vk::Buffer RightOfLocked(Vulkan::Scheduler& s,vk::Buffer b,bool create,Buffer& meta) {
    if(!meta.right&&create) {
        const VkBufferCreateInfo ci{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=meta.bytes,
            .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
        const VmaAllocationCreateInfo aci{.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkBuffer created{};
        ASSERT(vmaCreateBuffer(meta.allocator,&ci,&aci,&created,&meta.allocation,nullptr)==VK_SUCCESS);
        std::atomic_ref<VkBuffer>(meta.right).store(created, std::memory_order_release);
        Barrier(s);
        s.Commands().copyBuffer(b,meta.right,vk::BufferCopy{0,0,meta.bytes});
        Barrier(s);
    }
    return meta.right?vk::Buffer{meta.right}:b;
}
// Caller holds the registry mutex for the complete lookup/create operation.
static vk::Buffer RightBufferLocked(Vulkan::Scheduler& s,vk::Buffer b,bool create) {
    if(!b) return b;
    auto it=R().buffers.find(Key(VkBuffer(b)));
    ASSERT_MSG(it!=R().buffers.end()||!create,"Native stereo: unregistered writable buffer");
    if(it==R().buffers.end()) return b;
    return RightOfLocked(s,b,create,it->second);
}
vk::Buffer RightBuffer(Vulkan::Scheduler& s,vk::Buffer b,bool create) {
    if(!Enabled()||!b) return b;
    std::lock_guard lock(R().mutex);
    return RightBufferLocked(s,b,create);
}
void Copy(Vulkan::Scheduler& s,vk::Buffer src,vk::Buffer dst,std::span<const vk::BufferCopy> copies, bool hle, vk::Buffer outer_right) {
    ScratchProfile::Cpu scratch_cpu(19,"Copy");
    if(!Enabled()) { s.Commands().copyBuffer(src,dst,copies); return; }
    const size_t original_regions=copies.size();
    const bool coalesce=g_coalesce_copies;
    if(coalesce && copies.size()>1){
        // Merge only consecutive, exactly adjacent source AND destination ranges.
        // Preserve ordering and every byte; never copy across a gap.
        thread_local std::vector<vk::BufferCopy> merged;
        merged.clear();merged.reserve(copies.size());
        for(const auto& c:copies){
            if(!merged.empty()&&merged.back().srcOffset+merged.back().size==c.srcOffset&&
                merged.back().dstOffset+merged.back().size==c.dstOffset)merged.back().size+=c.size;
            else merged.push_back(c);
        }
        copies=merged;
    }
    // FAST_GPU_PASS_PROFILE: the HLE copy shader's copies stay in its label (many batches).
    ScratchProfile::Gpu scratch_gpu(s,40,!hle);
    auto right_src=ExistingRight(src),right_dst=ExistingRight(dst);
    if(right_src&&!right_dst) right_dst=RightBuffer(s,dst,true);
    const bool scoped_copy = g_copy_transfer_barriers;
    const bool hle_outer_barriers = g_hle_outer_barriers;
    if (hle && hle_outer_barriers) {
        // ExecuteCopyShaderHLE owns the surrounding global transfer dependencies,
        // including a dependency before each following batch/migration. The two
        // eyes use separate backing allocations, so they need no dependency on
        // each other. Lazy right-buffer creation above retains its own barriers.
        s.Commands().copyBuffer(src, dst, copies);
        if (right_dst)
            s.Commands().copyBuffer(right_src ? right_src : src, right_dst, copies);
    } else if (outer_right && right_dst == outer_right && !right_src && right_dst != dst &&
               right_dst != src && src != dst) {
        // FAST bit 63 (vk_transfer_eye_barriers.h): the caller's pre- and post-barriers name
        // both destinations; the eyes write disjoint buffers from one read-only source.
        s.Commands().copyBuffer(src, dst, copies);
        s.Commands().copyBuffer(src, right_dst, copies);
    } else if (right_dst && scoped_copy) {
        // Both eyes use disjoint backing allocations. Make their prior reads/writes
        // available before both copies, then expose the transfer writes together.
        // Keep a global memory dependency, but wait for COPY rather than every GPU
        // stage after the operation. No data or copies are omitted.
        s.EndRendering();
        const vk::MemoryBarrier2 before{
            .srcStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .srcAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eCopy,
            .dstAccessMask=vk::AccessFlagBits2::eTransferRead|vk::AccessFlagBits2::eTransferWrite};
        const vk::MemoryBarrier2 after{
            .srcStageMask=vk::PipelineStageFlagBits2::eCopy,
            .srcAccessMask=vk::AccessFlagBits2::eTransferWrite,
            .dstStageMask=vk::PipelineStageFlagBits2::eAllCommands,
            .dstAccessMask=vk::AccessFlagBits2::eMemoryRead|vk::AccessFlagBits2::eMemoryWrite};
        s.Commands().pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount=1,.pMemoryBarriers=&before});
        s.Commands().copyBuffer(src,dst,copies);
        s.Commands().copyBuffer(right_src?right_src:src,right_dst,copies);
        s.Commands().pipelineBarrier2(vk::DependencyInfo{.memoryBarrierCount=1,.pMemoryBarriers=&after});
    } else {
        s.Commands().copyBuffer(src,dst,copies);
        if(right_dst) { Barrier(s); s.Commands().copyBuffer(right_src?right_src:src,right_dst,copies); Barrier(s); }
    }
    if(g_copy_stats){
        static u64 calls=0,paired=0,bytes=0,paired_bytes=0,regions=0,originals=0;
        // Count after adjacency coalescing, specifically for the HLE candidate.
        // Buckets: 1, 2-7, 8-31, 32-127, 128+ regions. These are eligibility
        // upper bounds: aliasing, descriptor limits and cost still need checking.
        static std::array<u64, 5> hle_batches{};
        static u64 hle_aligned_large=0, hle_large_bytes=0;
        static auto last=std::chrono::steady_clock::now();
        ++calls;paired+=bool(right_dst);regions+=copies.size();originals+=original_regions;
        if (hle) {
            const auto n = copies.size();
            ++hle_batches[n < 2 ? 0 : n < 8 ? 1 : n < 32 ? 2 : n < 128 ? 3 : 4];
            bool aligned = true;
            u64 batch_bytes = 0;
            for (const auto& c : copies) {
                aligned &= ((c.srcOffset | c.dstOffset | c.size) & 3) == 0;
                batch_bytes += c.size;
            }
            if (n >= 8 && aligned) { ++hle_aligned_large; hle_large_bytes += batch_bytes; }
        }
        for(const auto& c:copies){bytes+=c.size;if(right_dst)paired_bytes+=c.size;}
        auto now=std::chrono::steady_clock::now();
        if(std::chrono::duration<double>(now-last).count()>=5){
            LOG_INFO(Render_Vulkan,"VR native copy: {:.3f} s, {} calls ({} paired), {} regions ({} original), {} bytes left + {} right; transfer barriers {} coalesce {}",std::chrono::duration<double>(now-last).count(),calls,paired,regions,originals,bytes,paired_bytes,scoped_copy,coalesce);
            LOG_INFO(Render_Vulkan, "Scratch HLE copy distribution: post-merge buckets 1/2-7/8-31/32-127/128+ = {}/{}/{}/{}/{}; dword-aligned 8+ batches {}, bytes {}", hle_batches[0], hle_batches[1], hle_batches[2], hle_batches[3], hle_batches[4], hle_aligned_large, hle_large_bytes);
            hle_batches.fill(0); hle_aligned_large=hle_large_bytes=0;
            calls=paired=bytes=paired_bytes=regions=originals=0;last=now;
        }
    }
}
void RegisterView(vk::ImageView b,vk::ImageView right,vk::ImageView array) {
    if(!Enabled()) return; std::lock_guard lock(R().mutex); R().views[Key(VkImageView(b))]={right,array}; ++R().view_generation;
}
void ForgetView(vk::ImageView b) {
    if(!Enabled()) return; std::lock_guard lock(R().mutex); R().views.erase(Key(VkImageView(b))); ++R().view_generation;
}
static vk::ImageView RightViewLocked(vk::ImageView b) {
    const auto key = Key(VkImageView(b));
    auto resolve = [&] { auto it=R().views.find(key); return it==R().views.end()?b:it->second.right; };
    if (!g_view_lookup_cache) return resolve();
    // All callers hold R().mutex; registration and destruction bump the generation.
    const auto right = R().right_view_cache.Get(key, R().view_generation.load(), resolve);
    if (g_view_lookup_check) {
        ASSERT_MSG(right == resolve(), "Native view lookup cache mismatch");
        static u64 checks = 0;
        if (++checks % 1000000 == 0) LOG_INFO(Render_Vulkan, "Native view lookup cache: {} exact checks passed", checks);
    }
    return right;
}
vk::ImageView RightView(vk::ImageView b) {
    std::lock_guard lock(R().mutex);
    return RightViewLocked(b);
}
vk::ImageView ArrayView(vk::ImageView b) {
    if(!b)return b; std::lock_guard lock(R().mutex); auto it=R().views.find(Key(VkImageView(b)));
    ASSERT_MSG(it!=R().views.end(),"Native stereo: attachment has no stereo backing"); return it->second.array;
}
void MultiviewState(Vulkan::RenderState& state) {
    if(!Enabled()||state.probe_view_mask!=3)return;
    for(u32 i=0;i<state.num_color_attachments;++i) state.color_attachments[i].image_view=ArrayView(state.color_attachments[i].image_view);
    state.depth_stencil_attachment.image_view=ArrayView(state.depth_stencil_attachment.image_view);
    state.probe_view_mask=3; state.num_layers=1;
}
u64 BufferGeneration() { return R().buffer_generation.load(std::memory_order_acquire); }
u64 ViewGeneration() { return R().view_generation.load(std::memory_order_acquire); }
void PairBarriers(std::span<const vk::BufferMemoryBarrier2> barriers, const Handles& handles,
                  std::span<vk::Buffer> rights) {
    ASSERT(rights.size() >= barriers.size());
    std::lock_guard lock(R().mutex);
    const bool fresh = R().buffer_generation.load(std::memory_order_relaxed) == handles.buffer_generation;
    for (size_t i = 0; i < barriers.size(); ++i) {
        const vk::Buffer b = barriers[i].buffer;
        Buffer* meta = nullptr;
        if (b) {
            if (fresh && i < handles.barriers.size() && handles.barriers[i].left == b)
                meta = handles.barriers[i].entry;
            if (meta) {
                ++handle_stats.barrier_hits;
                if (HandleVerifyNow()) meta = VerifyEntry(b, meta, "barrier");
            } else {
                // ExistingRight's lookup, without a lock per barrier.
                ++handle_stats.barrier_misses;
                const auto it = R().buffers.find(Key(VkBuffer(b)));
                meta = it == R().buffers.end() ? nullptr : &it->second;
            }
        }
        rights[i] = meta ? vk::Buffer{meta->right} : vk::Buffer{};
    }
}
// The right eye's copy of the range a descriptor reads from a stream buffer with a right twin.
static void MirrorStream(Vulkan::Scheduler& s,const vk::DescriptorBufferInfo& left,vk::Buffer right) {
    Barrier(s);
    // FAST_GPU_PASS_PROFILE (bit 38): its own segment, the pass label resumes.
    const Vulkan::GpuPassProfiler::Scope pass_scope{s, 0x57E5'0000'0041ull,
        Vulkan::GpuPassProfiler::Group::NativeStereo,
        "native stereo: stream buffer mirror for the right eye"};
    s.Commands().copyBuffer(left.buffer,right,vk::BufferCopy{left.offset,left.offset,left.range});
    Barrier(s);
}
void PairDescriptors(Vulkan::Scheduler& s,const Vulkan::Pipeline& pipeline,std::span<const vk::WriteDescriptorSet> original, Descriptors& out, bool reuse, const Handles* handles, bool fold) {
    ScratchProfile::Cpu scratch_cpu(15,"PairDescriptors");
    FastRender::BProfile::Scope b_profile{FastRender::BProfile::NativePairing};
    const bool audit = g_descriptor_lookup_audit;
    struct Audit { u64 calls{}, buffers{}, readonly{}, adjacent{}, remapped{}, mirrors{}, camera_tests{}; };
    static thread_local Audit counts;
    vk::Buffer previous_readonly{};
    bool have_previous = false;
    out.writes.clear();out.buffers.clear();out.images.clear();out.mirrors=0;
    size_t n=0; for(const auto& w:original)n+=w.descriptorCount*2;
    out.writes.reserve(original.size());out.buffers.reserve(n);out.images.reserve(n);
    // Only the non-reuse path needs this; MSVC's unordered_map allocates on construction.
    std::optional<std::unordered_map<u32,bool>> writable;u32 binding=0;
    if (!reuse) writable.emplace();
    if (!reuse) for(const auto* stage:pipeline.GetStages()) { if(!stage)continue;
        for(const auto& b:stage->buffers)(*writable)[binding++]=b.is_written;
        for(const auto& i:stage->images)binding+=i.NumBindings(*stage);
        binding+=stage->samplers.size();
    }
    const auto cached = reuse ? pipeline.NativeWritableBindings() : std::span<const u8>{};
    const bool batch_registry = g_batch_registry; // logged when the registry is created (FirstR)
    // Storage is reserved and pipeline metadata is prepared before extending the lock scope.
    // The GPU command order is unchanged; unlocked helpers require this owning lock.
    std::unique_lock registry_lock(R().mutex, std::defer_lock);
    if (batch_registry) registry_lock.lock();
    // Bit 25: carried handles, read only under the batch lock (see Handles).
    const bool buffer_hints = handles && batch_registry &&
        R().buffer_generation.load(std::memory_order_relaxed) == handles->buffer_generation;
    const bool view_hints = handles && batch_registry &&
        R().view_generation.load(std::memory_order_relaxed) == handles->view_generation;
    if (handles) {
        auto& st = handle_stats;
        if (batch_registry) st.stale += u64(!buffer_hints) + u64(!view_hints);
        if ((++st.calls & 65535) == 0)
            LOG_INFO(Render_Vulkan, "FAST stereo handles: hits={} misses={} buffers={}/{} images={}/{} barriers={}/{} stale={} verified={} mismatches={} calls={}",
                     st.buffer_hits + st.image_hits + st.barrier_hits,
                     st.buffer_misses + st.image_misses + st.barrier_misses,
                     st.buffer_hits, st.buffer_misses, st.image_hits, st.image_misses,
                     st.barrier_hits, st.barrier_misses, st.stale, st.verified, st.mismatches,
                     st.calls);
    }
    // This descriptor's carried registry entry, or null: look it up.
    const auto buffer_entry = [&](const vk::DescriptorBufferInfo* info) -> Buffer* {
        if (!buffer_hints || !info->buffer) return nullptr;
        const size_t index = HintIndex(info, handles->buffer_base);
        Buffer* entry = index < handles->buffers.size() && handles->buffers[index].left == info->buffer
            ? handles->buffers[index].entry : nullptr;
        if (!entry) { ++handle_stats.buffer_misses; return nullptr; }
        ++handle_stats.buffer_hits;
        return HandleVerifyNow() ? VerifyEntry(info->buffer, entry, "descriptor") : entry;
    };
    const bool reuse_lookup = g_reuse_readonly_lookup;
    const bool check_lookup = g_perf_check_set;
    vk::Buffer memo_left{}, memo_right{};
    bool memo_valid = false;
    for(auto w:original) {
        if(w.descriptorType==vk::DescriptorType::eStorageBuffer) {
            const bool written = reuse ? (w.dstBinding < cached.size() && cached[w.dstBinding]) : (*writable)[w.dstBinding];
            const auto first=out.buffers.size();
            for(u32 i=0;i<w.descriptorCount;++i) {
                auto left=w.pBufferInfo[i],right=left;
                Buffer* const hinted = buffer_entry(w.pBufferInfo + i);
                if (audit) {
                    ++counts.buffers;
                    if (!written && left.buffer) {
                        ++counts.readonly;
                        if (have_previous && previous_readonly == left.buffer) ++counts.adjacent;
                        previous_readonly = left.buffer;
                        have_previous = true;
                    } else {
                        have_previous = false;
                    }
                    counts.camera_tests += camera_pairs.size();
                }
                // The memo never escapes this registry-locked call. Writable inputs
                // can create right backing, so they always invalidate it and execute normally.
                if (reuse_lookup && batch_registry && !written && left.buffer) {
                    if (memo_valid && memo_left == left.buffer) {
                        right.buffer = memo_right;
                        if (check_lookup) {
                            ASSERT_MSG(right.buffer == RightBufferLocked(s, left.buffer, false),
                                       "Read-only descriptor lookup mismatch");
                        }
                    } else {
                        right.buffer = hinted ? RightOfLocked(s, left.buffer, false, *hinted)
                                              : RightBufferLocked(s, left.buffer, false);
                        memo_left = left.buffer;
                        memo_right = right.buffer;
                        memo_valid = true;
                    }
                } else {
                    memo_valid = false;
                    right.buffer=hinted ? RightOfLocked(s,left.buffer,written,*hinted) :
                        batch_registry ? RightBufferLocked(s,left.buffer,written) : RightBuffer(s,left.buffer,written);
                }
                // Stream buffers receive CPU writes directly; mirror only the read range.
                if(right.buffer!=left.buffer&&!written) {
                    if (audit) ++counts.remapped;
                    std::unique_lock lock(R().mutex, std::defer_lock);
                    if (!batch_registry) lock.lock();
                    const auto& meta=hinted ? *hinted : R().buffers.at(Key(VkBuffer(left.buffer)));
                    if (audit && meta.stream) ++counts.mirrors;
                    if(meta.stream) {
                        MirrorStream(s,left,right.buffer);
                        ++out.mirrors;
                    }
                }
                for (const auto& c : camera_pairs) if (left.buffer==c.left.buffer && left.offset==c.left.offset && left.range==c.left.range) {
                    right=c.right;
                    if (g_camera_audit && pipeline.IsCompute()) {
                        ASSERT_MSG(right.buffer!=left.buffer || right.offset!=left.offset,
                                   "Native compute camera must select a distinct right-eye allocation");
                        const auto hash=pipeline.GetStage(Shader::LogicalStage::Compute).pgm_hash;
                        static thread_local std::set<std::tuple<u64,u32,u64>> seen;
                        if (seen.emplace(hash,w.dstBinding,left.range).second)
                            LOG_INFO(Render_Vulkan,"VR compute camera pair: shader {:#x} binding {} bytes {} left offset {} right offset {}; vr_hud=1 selects descriptor 1",
                                     hash,w.dstBinding,left.range,left.offset,right.offset);
                    }
                    break;
                }
                out.buffers.push_back(left);out.buffers.push_back(right);
            }
            w.descriptorCount*=2;w.pBufferInfo=out.buffers.data()+first;
        } else if(w.descriptorType==vk::DescriptorType::eSampledImage||w.descriptorType==vk::DescriptorType::eStorageImage) {
            const auto first=out.images.size();
            for(u32 i=0;i<w.descriptorCount;++i) {
                auto left=w.pImageInfo[i],right=left;
                // Bit 25: the view's right carried from BindTextures (ImageView::native_right).
                const ViewHint* hint = nullptr;
                if (view_hints) {
                    const size_t index = HintIndex(w.pImageInfo + i, handles->image_base);
                    if (index < handles->images.size() && handles->images[index].left == left.imageView &&
                        (handles->images[index].right || !left.imageView))
                        hint = &handles->images[index];
                    if (hint) ++handle_stats.image_hits; else ++handle_stats.image_misses;
                    if (hint && HandleVerifyNow()) {
                        ++handle_stats.verified;
                        if (RightViewLocked(left.imageView) != hint->right) {
                            ++handle_stats.mismatches;
                            LOG_ERROR(Render_Vulkan, "FAST stereo handles verify: view {:#x} right mismatch #{}",
                                      Key(VkImageView(left.imageView)), handle_stats.mismatches);
                            hint = nullptr;
                        }
                    }
                }
                right.imageView=hint ? hint->right : batch_registry ? RightViewLocked(left.imageView) : RightView(left.imageView);
                out.images.push_back(left);out.images.push_back(right);
            }
            w.descriptorCount*=2;w.pImageInfo=out.images.data()+first;
        } else if(fold&&w.descriptorType==vk::DescriptorType::eSampler) {
            // Capacity n covers this (2 per descriptor reserved): no reallocation moves earlier infos.
            const auto first=out.images.size();
            out.images.insert(out.images.end(),w.pImageInfo,w.pImageInfo+w.descriptorCount);
            w.pImageInfo=out.images.data()+first;
        }
        out.writes.push_back(w);
    }
    out.buffers_used=out.buffers.size();out.images_used=out.images.size();
    if (audit && ++counts.calls % 10000 == 0) {
        LOG_INFO(Render_Vulkan, "VR descriptor lookup audit: calls {}, buffers {}, readonly {}, adjacent {}, remapped {}, mirrors {}, camera test upper bound {}",
                 counts.calls, counts.buffers, counts.readonly, counts.adjacent,
                 counts.remapped, counts.mirrors, counts.camera_tests);
    }
}
namespace {
// FAST_CPU_FUSED_PAIR_VERIFY=1 (read once): PairDescriptors runs from the registry alone and is
// used; the carried pairing runs too, records nothing, and is compared with it.
const bool g_fused_pair_verify = [] {
    const char* value = std::getenv("FAST_CPU_FUSED_PAIR_VERIFY");
    return value && value[0] == '1';
}();
// FAST_CPU_FUSED_PAIR counters (per recording thread; only with logging compiled in).
struct FusedStats { u64 calls{}, carried{}, stale{}, overflow{}, lookups{}, created{}, mirrors{}, verified{}, mismatches{}; };
thread_local FusedStats fused_stats;
// The registry side of Fused::Pair. Carried entries are read without the lock: an entry lives as
// long as its buffer, which this draw binds, and none was erased since binding (the generation);
// its right is written only under the lock by a recording thread (RightOfLocked, atomically).
// Whatever was not carried takes the lock, as PairDescriptors does. With VR_NATIVE_BATCH_REGISTRY
// (the default) that is once, at the first such descriptor, held to the end of the pass as
// PairDescriptors holds its batch lock (but only in a pass that misses): k misses cost one lock,
// not k. Without it, each lookup locks on its own, as PairDescriptors' lookups then do. `dry`
// (verify): the registry pass already recorded this draw's creations and mirrors; record nothing.
struct CarriedOps {
    Vulkan::Scheduler& s;
    bool dry{};
    u32 recorded{}, lookups{}, created{}, mirrors{};
    std::unique_lock<std::recursive_mutex> lock{R().mutex, std::defer_lock};
    // The registry lock around one lookup.
    struct Hold {
        CarriedOps& ops;
        explicit Hold(CarriedOps& o) : ops(o) {
            if (!ops.lock.owns_lock()) ops.lock.lock();
        }
        ~Hold() {
            if (!g_batch_registry) ops.lock.unlock();
        }
    };
    static Fused::Kind KindOf(const vk::WriteDescriptorSet& w) {
        switch (w.descriptorType) {
        case vk::DescriptorType::eStorageBuffer: return Fused::Kind::Buffer;
        case vk::DescriptorType::eSampledImage:
        case vk::DescriptorType::eStorageImage: return Fused::Kind::Image;
        case vk::DescriptorType::eSampler: return Fused::Kind::Sampler;
        default: return Fused::Kind::Other;
        }
    }
    static vk::Buffer RightOf(Buffer& entry) {
        return vk::Buffer{std::atomic_ref<VkBuffer>(entry.right).load(std::memory_order_acquire)};
    }
    static bool Stream(const Buffer& entry) { return entry.stream; }
    // A written descriptor's buffer without its right yet: created here, in the pass's order.
    vk::Buffer Create(vk::Buffer left, Buffer& entry) {
        ++created;
        if (dry) return left;
        Hold hold{*this};
        ++recorded;
        return RightOfLocked(s, left, true, entry);
    }
    // RightBufferLocked, and the entry's stream flag for the mirror.
    vk::Buffer Lookup(vk::Buffer left, bool written, bool& stream) {
        ++lookups;
        Hold hold{*this};
        const auto it = R().buffers.find(Key(VkBuffer(left)));
        ASSERT_MSG(it != R().buffers.end() || !written, "Native stereo: unregistered writable buffer");
        if (it == R().buffers.end()) return left;
        stream = it->second.stream;
        if (written && !it->second.right) {
            ++created;
            if (!dry) {
                ++recorded;
                return RightOfLocked(s, left, true, it->second);
            }
        }
        return it->second.right ? vk::Buffer{it->second.right} : left;
    }
    void Mirror(const vk::DescriptorBufferInfo& left, vk::Buffer right) {
        ++mirrors;
        if (dry) return;
        ++recorded;
        MirrorStream(s, left, right);
    }
    vk::ImageView LookupView(vk::ImageView left) {
        ++lookups;
        Hold hold{*this};
        return RightViewLocked(left);
    }
    // PairDescriptors' camera scan, for a descriptor that carried no hint.
    void ScanCamera(const vk::DescriptorBufferInfo& left, vk::DescriptorBufferInfo& right) {
        ++lookups;
        for (const auto& c : camera_pairs) {
            if (left.buffer == c.left.buffer && left.offset == c.left.offset && left.range == c.left.range) {
                right = c.right;
                break;
            }
        }
    }
    // The binding's writes always point into its own arrays, each info once (twice their sizes
    // hold the pairs): an overflow is a caller's error, caught before anything was recorded.
    bool Overflow() const {
        ASSERT_MSG(recorded == 0, "FAST_CPU_FUSED_PAIR: a write outside the binding's arrays");
        return false;
    }
};
// Fused::Pair into `out` (its writes a copy of `original`). False: nothing recorded, `out` unusable.
bool PairFromHandles(const Vulkan::Pipeline& pipeline, std::span<const vk::WriteDescriptorSet> original,
                     Descriptors& out, const Handles& h, bool fold, CarriedOps& ops) {
    out.writes.assign(original.begin(), original.end());
    // The arrays only grow (no fill per call) and their front is used (buffers_used / images_used).
    if (out.buffers.size() < 2 * h.buffer_count) out.buffers.resize(2 * h.buffer_count);
    if (out.images.size() < 2 * h.image_count) out.images.resize(2 * h.image_count);
    const Fused::Bound<vk::DescriptorBufferInfo, vk::DescriptorImageInfo, BufferHint, ViewHint> bound{
        h.buffer_base, h.buffer_count, h.buffers, h.image_base, h.image_count, h.images};
    const bool paired = Fused::Pair(std::span<vk::WriteDescriptorSet>{out.writes}, bound,
                                    pipeline.NativeWritableBindings(),
                                    std::span<vk::DescriptorBufferInfo>{out.buffers},
                                    std::span<vk::DescriptorImageInfo>{out.images},
                                    out.buffers_used, out.images_used, fold, ops);
    out.mirrors = ops.mirrors;
    return paired;
}
// The first difference between the registry pass's write `a` and the carried pass's `b`.
const char* WriteDifference(const vk::WriteDescriptorSet& a, const vk::WriteDescriptorSet& b, bool fold, u32& element) {
    element = 0;
    if (a.pNext != b.pNext || a.dstSet != b.dstSet || a.dstBinding != b.dstBinding ||
        a.dstArrayElement != b.dstArrayElement || a.descriptorCount != b.descriptorCount ||
        a.descriptorType != b.descriptorType)
        return "write";
    switch (CarriedOps::KindOf(a)) {
    case Fused::Kind::Buffer:
        for (; element < a.descriptorCount; ++element) {
            const auto &x = a.pBufferInfo[element], &y = b.pBufferInfo[element];
            if (x.buffer != y.buffer || x.offset != y.offset || x.range != y.range) return "buffer info";
        }
        return nullptr;
    case Fused::Kind::Image:
        for (; element < a.descriptorCount; ++element) {
            const auto &x = a.pImageInfo[element], &y = b.pImageInfo[element];
            if (x.sampler != y.sampler || x.imageView != y.imageView || x.imageLayout != y.imageLayout) return "image info";
        }
        return nullptr;
    case Fused::Kind::Sampler:
        if (fold) {
            for (; element < a.descriptorCount; ++element)
                if (a.pImageInfo[element].sampler != b.pImageInfo[element].sampler) return "sampler info";
            return nullptr;
        }
        [[fallthrough]];
    default:
        return a.pBufferInfo != b.pBufferInfo || a.pImageInfo != b.pImageInfo ||
               a.pTexelBufferView != b.pTexelBufferView ? "pointer" : nullptr;
    }
}
}
bool PairCarried(Vulkan::Scheduler& s, const Vulkan::Pipeline& pipeline,
                 std::span<const vk::WriteDescriptorSet> original, Descriptors& out, bool reuse,
                 const Handles& handles, bool fold) {
    auto& st = fused_stats;
    if constexpr (Common::Log::Enabled) {
        if ((++st.calls & 65535) == 0)
            LOG_INFO(Render_Vulkan, "FAST_CPU_FUSED_PAIR: calls={} carried={} stale={} overflow={} lookups={} created={} mirrors={} verified={} mismatches={}",
                     st.calls, st.carried, st.stale, st.overflow, st.lookups, st.created, st.mirrors,
                     st.verified, st.mismatches);
    }
    // Carried entries and view rights are the registry's only while no entry was erased and no
    // view registered or forgotten since the binding read the generations: else the registry pass.
    const auto& registry = R();
    if (registry.buffer_generation.load(std::memory_order_acquire) != handles.buffer_generation ||
        registry.view_generation.load(std::memory_order_acquire) != handles.view_generation) {
        if constexpr (Common::Log::Enabled) ++st.stale;
        return false;
    }
    ScratchProfile::Cpu scratch_cpu(15,"PairDescriptors");
    FastRender::BProfile::Scope b_profile{FastRender::BProfile::NativePairing};
    if (!g_fused_pair_verify) [[likely]] {
        CarriedOps ops{s};
        if (!PairFromHandles(pipeline, original, out, handles, fold, ops)) {
            if constexpr (Common::Log::Enabled) ++st.overflow;
            return false;
        }
        if constexpr (Common::Log::Enabled) {
            ++st.carried;
            st.lookups += ops.lookups;
            st.created += ops.created;
            st.mirrors += ops.mirrors;
        }
        return true;
    }
    // Verify: the registry pass records and decides; then the carried pass, recording nothing.
    PairDescriptors(s, pipeline, original, out, reuse, nullptr, fold);
    thread_local Descriptors carried;
    CarriedOps ops{s, true};
    const char* what = nullptr;
    size_t at = 0;
    u32 element = 0;
    const bool paired = PairFromHandles(pipeline, original, carried, handles, fold, ops);
    if (ops.lock.owns_lock()) ops.lock.unlock(); // not held while comparing and logging
    if (!paired) {
        what = "overflow";
    } else if (carried.writes.size() != out.writes.size()) {
        what = "write count";
    } else if (carried.mirrors != out.mirrors) {
        what = "stream mirrors";
    } else {
        for (; at < out.writes.size() && !what; ++at)
            what = WriteDifference(out.writes[at], carried.writes[at], fold, element);
        at -= what ? 1 : 0;
    }
    if constexpr (Common::Log::Enabled) {
        ++st.verified;
        st.lookups += ops.lookups;
        if (what && (++st.mismatches <= 64 || (st.mismatches & 1023) == 0)) {
            // Both writes' infos are shown only for an info difference (same type and count).
            const bool infos = std::strcmp(what, "buffer info") == 0 || std::strcmp(what, "image info") == 0;
            const auto& w = at < out.writes.size() ? out.writes[at] : vk::WriteDescriptorSet{};
            const bool buffer = infos && CarriedOps::KindOf(w) == Fused::Kind::Buffer;
            const bool image = infos && CarriedOps::KindOf(w) == Fused::Kind::Image;
            LOG_ERROR(Render_Vulkan,
                      "FAST_CPU_FUSED_PAIR verify: {} differs at write {} (binding {}, type {}) element {}: registry {:#x}+{:#x}, carried {:#x}+{:#x}; mirrors {}/{}; the registry pass is used, mismatch #{}",
                      what, at, w.dstBinding, static_cast<u32>(w.descriptorType), element,
                      buffer ? Key(VkBuffer(w.pBufferInfo[element].buffer)) : image ? Key(VkImageView(w.pImageInfo[element].imageView)) : 0,
                      buffer ? w.pBufferInfo[element].offset : 0,
                      buffer ? Key(VkBuffer(carried.writes[at].pBufferInfo[element].buffer)) : image ? Key(VkImageView(carried.writes[at].pImageInfo[element].imageView)) : 0,
                      buffer ? carried.writes[at].pBufferInfo[element].offset : 0,
                      out.mirrors, carried.mirrors, st.mismatches);
        }
    }
    return true;
}
}
