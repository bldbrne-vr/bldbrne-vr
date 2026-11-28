// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "core/vr/bloodborne_visceral_stage.h"
#include "native_stereo.h"
#include <mutex>
#include "video_core/fast/b_handler_profile.h"
#include "common/assert.h"
#include "common/render_diagnostic.h"
#include "core/vr/vr_session.h"
#include "core/vr/s1_dynamic_resolution.h"
#include "core/vr/eye_distance.h"
#include "core/vr/lamp_glow.h"
#include "core/vr/monocular.h"
#include "core/vr/moon_screen.h"
#include "core/vr/native_views.h"
#include "draw_memory.h"
#include "fused_pair.h"
#include "lighting_reconstruction.h"
#include "profile_scopes.h"
#include "registry_lookup_cache.h"
#include "shader_recompiler/info.h"
#include "video_core/amdgpu/liverpool.h"
#include "video_core/vram_report.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/buffer_cache/sparse_pool.h"
#include "video_core/renderer_vulkan/vk_pipeline_common.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_constant_ring.h"
#include "video_core/renderer_vulkan/vk_stereo_set_codec.h"
#include "video_core/texture_cache/image_info.h"
#include <array>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <algorithm>
#include <fmt/ranges.h>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <vk_mem_alloc.h>
namespace NativeStereo {
struct BufferEntry { VkDeviceSize bytes{}; VmaAllocator allocator{}; bool stream{}; VkBuffer right{}; VmaAllocation allocation{};
    // Sparse views (buffer_cache/sparse_pool.h): the pool, the view's guest address, and one bit
    // per 64 KB block of `right`, set where the twin binds its own piece (else the left's).
    VideoCore::SparsePool* sparse{}; u64 view_begin{}; std::vector<u64> private_bits;
    std::vector<u32> twin_pieces; }; // the pieces `right` binds (ReleaseTwin gives them back)
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
// VR_NATIVE_SHARED_SHADOWS ('0' off; on by default since 2026-11-04): the 4096x4096 shadow atlas
// one layer, drawn once for both eyes (the game fits it to its one camera). Re-measured at the
// user's settings: GPU time the same within noise, peak VRAM 170-300 MB lower (the second layer and
// the right-eye buffer copies made from it).
const bool g_shared_shadows = [] {
    const char* setting = std::getenv("VR_NATIVE_SHARED_SHADOWS");
    return !setting || setting[0] != '0';
}();
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
void SharedShadowPerspective(float m66, float m67) {
    static std::atomic<bool> noted{false};
    if (!noted.exchange(true, std::memory_order_relaxed)) {
        LOG_WARNING(Render_Vulkan,
                    "VR native shared shadow: a perspective camera drew into the atlas ({} / {}); "
                    "kept shared (a light's view is the same for both eyes)",
                    m66, m67);
    }
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
namespace {
// The card's mode, decided in every session, DIBR too (LayerDrawsMode).
std::atomic<bool> g_layer_draws{false};
}
bool LayerDraws() {
    return Enabled() && g_layer_draws.load(std::memory_order_relaxed);
}
bool LayerDrawsMode() {
    return g_layer_draws.load(std::memory_order_relaxed);
}
void ChooseLayerDraws(u32 vendor_id, u32 device_id, std::string_view device_name,
                      bool shader_output_layer) {
    const char* value = std::getenv("VR_NATIVE_VIEWS");
    const std::string_view choice = value ? value : "";
    bool layers = VR::NativeViews::LayersByDefault(vendor_id, device_id, device_name);
    if (choice == "layers") layers = true;
    else if (choice == "multiview") layers = false;
    if (layers && !shader_output_layer) {
        if (Enabled())
            LOG_WARNING(Render_Vulkan, "Native stereo: layer draws need shaderOutputLayer; multiview");
        layers = false;
    }
    g_layer_draws.store(layers, std::memory_order_relaxed);
    if (!Enabled()) return;
    LOG_INFO(Render_Vulkan, "Native stereo views: {} (vendor {:#x}, device {:#x}{})",
             layers ? "layer draws, one draw per eye" : "multiview", vendor_id, device_id,
             value ? fmt::format(", VR_NATIVE_VIEWS={}", value) : std::string{});
    RENDER_DIAGNOSTIC_ONCE("Native stereo views: {}", layers ? "layers" : "multiview");
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
// The Monocular uses the touched eye's existing scene draw as its narrow-field optical view.
// Keep CameraRecord::eye_fov untouched: the OpenXR submission and source crop still use the
// headset FOV. Only scene/reconstruction projections use this narrower FOV.
VR::Fov SceneEyeFov(const VR::CameraRecord& record, u32 eye) {
    // Only a frame the headset placed is seen through its lenses: the game's own camera (a
    // cutscene, a flat cinematic) keeps its projection, whatever FOVs its record carries
    // (VR_KEEP_EYE_FOV keeps them on flat records; a code review, 2026-11-17). Every caller
    // takes no FOV as no remap.
    if (eye >= record.eye_fov.size() || !record.driven) return {};
    VR::Fov f = record.eye_fov[eye];
    if (record.monocular_eye != static_cast<s32>(eye) ||
        !std::isfinite(record.monocular_zoom)) return f;
    const float magnification = std::clamp(record.monocular_zoom, 1.0f, VR::Monocular::MaxZoom);
    // The stabilized aim slides the narrowed window inside the eye's own (core/vr/monocular.h
    // ZoomWindow): the same eye, view and draws, a few degrees of turn. No shift: as before.
    const float shift_x =
        std::isfinite(record.monocular_shift[0]) ? record.monocular_shift[0] : 0.0f;
    const float shift_y =
        std::isfinite(record.monocular_shift[1]) ? record.monocular_shift[1] : 0.0f;
    // DrawProjection asks once per draw. The headset FOV, zoom and slide are constant within a
    // camera update, so compute the four trigonometric remaps only once per eye and recording
    // thread.
    struct Memo {
        std::array<float, 7> key{};
        VR::Fov value{};
        bool valid{};
    };
    thread_local std::array<Memo, 2> memo;
    Memo& cached = memo[eye];
    const std::array<float, 7> key{f.angle_left, f.angle_right, f.angle_up, f.angle_down,
                                   magnification, shift_x, shift_y};
    if (cached.valid && std::memcmp(cached.key.data(), key.data(), sizeof(key)) == 0) {
        return cached.value;
    }
    const auto w = VR::Monocular::ZoomWindow(
        {std::tan(f.angle_left), std::tan(f.angle_right), std::tan(f.angle_up),
         std::tan(f.angle_down)},
        magnification, {shift_x, shift_y});
    cached.key = key;
    cached.value = {std::atan(w[0]), std::atan(w[1]), std::atan(w[2]), std::atan(w[3])};
    cached.valid = true;
    return cached.value;
}
static std::array<NativeProjection::Transform,2> EyeProjections(const float* center,
                                                               const VR::CameraRecord& record) {
    std::array<NativeProjection::Transform,2> projections{};
    if (AsymmetricProjection()) {
        for (u32 eye=0; eye<2; ++eye) {
            const auto v = SceneEyeFov(record, eye);
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
// The last light pass's eye inverse projections (LightInverseProjections).
static std::mutex g_light_inverse_mutex;
static std::array<std::array<float, 16>, 2> g_light_inverse{};
static bool g_light_inverse_valid = false;
// Last frame's eye frustum over this frame's, for the game's histories (wet streets' reflection,
// the temporal SSAO), which were drawn last frame: under dynamic resolution each eye's frustum is
// its full one times the view's share p = (100 - drop)% in scale and offset (S1Drs::Expanded
// widens about the tangent centre), so last frame's is this one's times p_last / p_now (2026-11-24
// review: this frame's put the reflections up to 80 px off on a step frame). 1 when the step did
// not change. Both steps come with the draw's record, latched once a flip (vr_session.cpp
// LatchNextDrawResolution), so every pass of a flip and both threads that pair these buffers (A's
// ring constants, B's other buffers) agree.
static float PreviousFrustumScale(const VR::CameraRecord& record) {
    const unsigned now = record.s1_render_drop, last = record.s1_previous_drop;
    if (now == last || now >= 100 || last >= 100) return 1.0f;
    return float(100 - last) / float(100 - now);
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
    if constexpr (Common::Log::Enabled) {
        // Wet streets: the game's trace parameters (dwords 64..79), the first three buffers.
        static std::atomic<u32> told{};
        if (layout == NativeProjection::LightingLayout::Reflection &&
            told.fetch_add(1, std::memory_order_relaxed) < 3) {
            std::string line;
            for (u32 d = 64; d < 80; ++d) line += fmt::format(" {:.6g}", source[d]);
            LOG_INFO(Render_Vulkan, "VR reflections: trace parameters (dwords 64..79):{}", line);
        }
    }
    const bool decal = layout == NativeProjection::LightingLayout::Decal;
    std::array<NativeProjection::Transform,2> transforms;
    // Use this pass's inverse projection, matching CameraBuffer's per-camera
    // decision. Auxiliary narrow-FOV cameras must not inherit the main eye remap.
    const float* inverse_projection = source + (decal ? 16 : 0);
    const float tan_x = inverse_projection[0], tan_y = inverse_projection[5];
    // RecognizeLighting already validated this inverse projection, including the
    // bounded roundoff allowance. Do not repeat an exact-one check here.
    for (u32 eye=0; eye<2; ++eye) {
        const auto v = SceneEyeFov(record, eye);
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
    // Their history side takes last frame's frustum (PreviousFrustumScale).
    const float last_scale = PreviousFrustumScale(record);
    std::array<NativeProjection::Transform, 2> previous = transforms;
    if (last_scale != 1.0f && (history || layout == NativeProjection::LightingLayout::Reflection)) {
        for (auto& t : previous) {
            t.x_scale *= last_scale;
            t.y_scale *= last_scale;
            t.x_offset *= last_scale;
            t.y_offset *= last_scale;
        }
    }
    if constexpr (Common::Log::Enabled) {
        // VR_TEMPORAL_TRACE (tests): once a record, the records whose step differs from the last
        // flip's (where the history layouts take last frame's frustum), whatever the buffer.
        static std::atomic<u64> told{~0ull};
        if (S1Drs::trace && last_scale != 1.0f &&
            told.exchange(record.id, std::memory_order_relaxed) != record.id)
            LOG_WARNING(Render_Vulkan, "DRS history: record {} drop {} after {} (x{:.4f}; first {})",
                        record.id, record.s1_render_drop, record.s1_previous_drop, last_scale,
                        NativeProjection::Name(layout));
    }
    const u64 stride = alignment ? ((size + alignment - 1) / alignment) * alignment : size;
    const auto [mapped, offset] = stream.Map(stride + size, alignment);
    ASSERT(mapped);
    // (The edge pass reads the lamp shaders' matrices: a light pass's, not a mirror's; both eyes
    // published together, never one new beside one old: a code review, 2026-11-20.)
    // (Once a frame: the first light pass's; every lamp draw of a frame has the same.)
    static u64 published_record = ~0ull;
    const bool publish = layout == NativeProjection::LightingLayout::Light && !mirror && size >= 64 &&
                         record.id != published_record;
    std::array<std::array<float, 16>, 2> inverse{};
    for (u32 eye=0; eye<2; ++eye) {
        std::array<float,104> values{};
        std::memcpy(values.data(),source,size);
        const float e = (eye ? .5f : -.5f) * Ipd(record) * record.world_scale;
        NativeProjection::RemapLighting(values.data(),source,layout,e,transforms[eye],history,
                                        &previous[eye]);
        std::memcpy(mapped+eye*stride,values.data(),size);
        if (publish) std::memcpy(inverse[eye].data(), values.data(), 64);
    }
    if (publish) {
        published_record = record.id;
        std::scoped_lock lock{g_light_inverse_mutex};
        g_light_inverse = inverse;
        g_light_inverse_valid = true;
    }
    stream.Commit();
    left = {stream.Handle(),offset,size};
    camera_pairs.push_back({left,{stream.Handle(),offset+stride,size}});
    if (world_scale) *world_scale = record.world_scale;
    return true;
}
bool LightInverseProjections(std::array<std::array<float, 16>, 2>& out) {
    std::scoped_lock lock{g_light_inverse_mutex};
    if (!g_light_inverse_valid) return false;
    out = g_light_inverse;
    return true;
}
bool EyeInverseProjections(const float* source, const VR::CameraRecord& record,
                           std::array<std::array<float, 16>, 2>& out) {
    for (auto& m : out) std::memcpy(m.data(), source, sizeof(m));
    if (!AsymmetricProjection()) return false;
    if (NativeProjection::RecognizeLighting({source, 84}) !=
        NativeProjection::LightingLayout::Cascades) return false;
    const float tan_x = source[0], tan_y = source[5];
    std::array<std::array<float, 16>, 2> eyes;
    for (u32 eye = 0; eye < 2; ++eye) {
        const auto v = SceneEyeFov(record, eye);
        const auto transform = NativeProjection::Make(
            {v.angle_left, v.angle_right, v.angle_up, v.angle_down}, tan_x, tan_y, false);
        if (!transform.valid) return false;
        std::array<float, 104> values{};
        std::memcpy(values.data(), source, 336);
        const float e = (eye ? .5f : -.5f) * Ipd(record) * record.world_scale;
        NativeProjection::RemapLighting(values.data(), source,
                                        NativeProjection::LightingLayout::Cascades, e, transform,
                                        false);
        std::memcpy(eyes[eye].data(), values.data(), sizeof(eyes[eye]));
    }
    out = eyes;
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
    // VR_LAMP_GLOW (tests, core/vr/lamp_glow.h): the save lamps' dim glow brighter;
    // VR_CAMERA_LIGHT: the game's grey light on the camera dimmer, or off.
    if (list.kind == NativeProjection::LightKind::Point &&
        (VR::LampGlow::Boost() > 1.0f || VR::CameraLight::Scale() < 1.0f)) {
        for (u32 i = 0; i < list.count; ++i) {
            float* p = center.data() + list.Stride() * i;
            const float metres = std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) / world_scale;
            float k = 1.0f;
            if (VR::LampGlow::IsGlow(p[4], p[5], p[6], metres)) {
                k = VR::LampGlow::Boost();
            } else if (VR::CameraLight::Is(p[4], p[5], p[6], metres)) {
                k = VR::CameraLight::Scale();
            }
            if (k != 1.0f) {
                for (u32 c : {4u, 5u, 6u, 8u, 9u, 10u}) {
                    p[c] *= k;
                }
            }
        }
    }
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
    // The first 256 lists, then one in 64 (the views a test looks at come later).
    const u32 audit_n = g_light_list_audit ? audited.fetch_add(1, std::memory_order_relaxed) : 0;
    if (g_light_list_audit && (audit_n < 256 || audit_n % 64 == 0)) {
        std::string lights;
        for (u32 i = 0; i < list.count; ++i) {
            const float* p = center.data() + list.Stride() * i;
            lights += fmt::format(" ({:.3f}, {:.3f}, {:.3f}: {:.2f} m", p[0], p[1], p[2],
                                  std::sqrt(p[0] * p[0] + p[1] * p[1] + p[2] * p[2]) / world_scale);
            // A point light's reach (1/r^2 at 3) and colour (4..6): fog_light.h IsFillLight's.
            if (list.kind == NativeProjection::LightKind::Point)
                lights += fmt::format(", r {:.2f} m, lum {:.2f}, rgb {:.2f} {:.2f} {:.2f}",
                                      p[3] > 0.f ? 1.f / std::sqrt(p[3]) / world_scale : -1.f,
                                      0.3f * p[4] + 0.6f * p[5] + 0.1f * p[6], p[4], p[5], p[6]);
            lights += ")";
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
        const auto v = SceneEyeFov(record, eye);
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
    // The game's light shafts' centre: the visible moon, for our god rays' glow (moon_screen.h).
    const bool shafts = pgm_hash == 0xd506a1;
    if (shafts) {
        VR::MoonScreen::Publish(source[at], source[at + 1]);
    }
    // The first three of each shader: the light shafts' and the glow god rays'.
    static std::array<std::atomic<u32>, 2> logged{};
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
            const auto v = SceneEyeFov(record, eye);
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
    const auto l=SceneEyeFov(record,0); const auto r=SceneEyeFov(record,1);
    const std::array<float,10> key{record.vfov,record.aspect,l.angle_left,l.angle_right,l.angle_up,
                                   l.angle_down,r.angle_left,r.angle_right,r.angle_up,r.angle_down};
    if (!memo.valid || std::memcmp(memo.key.data(),key.data(),sizeof(key))!=0) {
        std::array<std::array<float,4>,2> result{};
        const float ty=std::tan(record.vfov*.5f);
        for (u32 eye=0;eye<2;++eye) {
            const auto f=SceneEyeFov(record,eye);
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
        const auto f=SceneEyeFov(record,eye);
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
        const auto v=SceneEyeFov(record,eye);
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
BufferEntry* RegisterSparseView(vk::Buffer b, VkDeviceSize n, VmaAllocator allocator,
                                VideoCore::SparsePool* pool, u64 view_begin) {
    BufferEntry* const meta = RegisterBuffer(b, n, allocator, false);
    if (meta) {
        std::lock_guard lock(R().mutex);
        meta->sparse = pool;
        meta->view_begin = view_begin;
    }
    return meta;
}
void ForgetBuffer(vk::Buffer b) {
    if(!Enabled()||!b) return;
    std::lock_guard lock(R().mutex);
    auto it=R().buffers.find(Key(VkBuffer(b)));
    if(it==R().buffers.end()) return;
    if(it->second.right) vmaDestroyBuffer(it->second.allocator,it->second.right,it->second.allocation);
    // A sparse twin (no allocation of its own): its private pieces lose a reference. The view goes
    // after the GPU is done with it, and so with its twin.
    if (it->second.sparse && it->second.right) {
        it->second.sparse->ReleaseTwin(it->second.twin_pieces);
    }
    R().buffers.erase(it);
    ++R().buffer_generation;
}
VkDeviceSize ReleaseRight(Vulkan::Scheduler& s, vk::Buffer b) {
    if(!Enabled()||!b) return 0;
    VmaAllocator allocator{};
    VkBuffer right{};
    VmaAllocation allocation{};
    VkDeviceSize bytes{};
    {
        std::lock_guard lock(R().mutex);
        auto it=R().buffers.find(Key(VkBuffer(b)));
        if(it==R().buffers.end()||!it->second.right||it->second.stream) return 0;
        Buffer& meta=it->second;
        if (meta.sparse) {
            // A sparse twin: its private pieces (its only memory) go once the GPU is done with it.
            VideoCore::SparsePool* const pool = meta.sparse;
            const VkBuffer old = meta.right;
            const VmaAllocator alloc = meta.allocator;
            u64 own = 0;
            for (const u64 word : meta.private_bits) own += std::popcount(word);
            std::vector<u32> pieces = std::move(meta.twin_pieces);
            std::atomic_ref<VkBuffer>(meta.right).store(VK_NULL_HANDLE, std::memory_order_release);
            meta.private_bits.clear();
            meta.twin_pieces.clear();
            ++R().buffer_generation;
            s.DeferPriorityOperation([pool, old, alloc, pieces] {
                vmaDestroyBuffer(alloc, old, VK_NULL_HANDLE);
                pool->ReleaseTwin(pieces);
            });
            return own << VideoCore::SparseLogic::BlockBits;
        }
        allocator=meta.allocator; right=meta.right; allocation=meta.allocation; bytes=meta.bytes;
        std::atomic_ref<VkBuffer>(meta.right).store(VK_NULL_HANDLE, std::memory_order_release);
        meta.allocation=VK_NULL_HANDLE;
        ++R().buffer_generation; // carried hints and memos with the twin's handle rebuild
    }
    s.DeferPriorityOperation([allocator,right,allocation]{ vmaDestroyBuffer(allocator,right,allocation); });
    return bytes;
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
// --- Sparse twins (buffer_cache/sparse_pool.h); the caller holds the registry mutex. ---
constexpr vk::BufferUsageFlags SparseTwinUsage =
    vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst |
    vk::BufferUsageFlagBits::eStorageBuffer | vk::BufferUsageFlagBits::eUniformBuffer |
    vk::BufferUsageFlagBits::eIndirectBuffer | vk::BufferUsageFlagBits::eVertexBuffer |
    vk::BufferUsageFlagBits::eIndexBuffer;
// [offset, offset + size) of the view, clamped, as blocks [first, end).
static std::pair<u64, u64> SparseBlocks(const Buffer& m, u64 offset, u64 size) {
    namespace L = VideoCore::SparseLogic;
    const u64 end = std::min<u64>(m.bytes, size > m.bytes - std::min<u64>(offset, m.bytes)
                                               ? m.bytes : offset + size);
    if (offset >= end) return {0, 0};
    return {offset >> L::BlockBits, L::AlignUp(end) >> L::BlockBits};
}
static bool SparseBit(const Buffer& m, u64 block) {
    return block / 64 < m.private_bits.size() && ((m.private_bits[block / 64] >> (block % 64)) & 1);
}
static bool SparseAllPrivate(const Buffer& m, u64 offset, u64 size) {
    const auto [first, end] = SparseBlocks(m, offset, size);
    for (u64 b = first; b < end; ++b) if (!SparseBit(m, b)) return false;
    return true;
}
static bool SparseAnyPrivate(const Buffer& m, u64 offset, u64 size) {
    const auto [first, end] = SparseBlocks(m, offset, size);
    for (u64 b = first; b < end; ++b) if (SparseBit(m, b)) return true;
    return false;
}
// f(offset, size) for each run of private blocks, clipped to [offset, offset + size).
template <class F>
static void SparseForPrivate(const Buffer& m, u64 offset, u64 size, F&& f) {
    namespace L = VideoCore::SparseLogic;
    const u64 end = std::min<u64>(m.bytes, offset + std::min<u64>(size, m.bytes));
    const auto [first, last] = SparseBlocks(m, offset, size);
    for (u64 b = first; b < last;) {
        if (!SparseBit(m, b)) { ++b; continue; }
        u64 e = b;
        while (e < last && SparseBit(m, e)) ++e;
        const u64 lo = std::max<u64>(offset, b << L::BlockBits);
        const u64 hi = std::min<u64>(end, e << L::BlockBits);
        if (hi > lo) f(lo, hi - lo);
        b = e;
    }
}
// VRAM has no piece for a sparse view's twin or for the right eye's own blocks: the view's twin
// becomes an ordinary one from then on, a full copy as before sparse twins (VMA may place it in
// system memory), so a per-eye write never lands in the left eye's buffer. Its bytes: the old
// sparse twin's (the right eye's own blocks with them), else the left's. The old twin goes once
// the GPU is done with it.
static vk::Buffer OrdinaryTwinLocked(Vulkan::Scheduler& s, vk::Buffer b, Buffer& meta) {
    VideoCore::SparsePool* const pool = meta.sparse;
    const VkBuffer old = meta.right;
    std::vector<u32> old_pieces = std::move(meta.twin_pieces);
    const VkBufferCreateInfo ci{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=meta.bytes,
        .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
    VkBuffer created{};
    VmaAllocation allocation{};
    // VRAM first, then system memory by name (the GPU reads it over PCIe, slower but right). With
    // neither, the process is out of memory: stop here rather than hand back the left buffer.
    for (const VmaMemoryUsage usage :
         {VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE, VMA_MEMORY_USAGE_AUTO_PREFER_HOST}) {
        const VmaAllocationCreateInfo aci{.usage = usage};
        if (vmaCreateBuffer(meta.allocator, &ci, &aci, &created, &allocation, nullptr) ==
            VK_SUCCESS)
            break;
        created = VK_NULL_HANDLE;
        allocation = VK_NULL_HANDLE;
    }
    ASSERT_MSG(created, "Sparse twins: no VRAM or system memory for a {} MiB right eye twin",
               meta.bytes >> 20);
    Barrier(s);
    s.Commands().copyBuffer(old ? vk::Buffer{old} : b, vk::Buffer{created}, vk::BufferCopy{0,0,meta.bytes});
    Barrier(s);
    if (old) {
        const VmaAllocator alloc = meta.allocator;
        s.DeferPriorityOperation([pool, old, alloc, old_pieces] {
            vmaDestroyBuffer(alloc, old, VK_NULL_HANDLE);
            pool->ReleaseTwin(old_pieces);
        });
    }
    static bool warned = false;
    if (!warned) {
        warned = true;
        LOG_WARNING(Render_Vulkan, "Sparse twins: no VRAM for a sparse view's right eye: its twin "
                    "is an ordinary copy from now on ({} MiB)", meta.bytes >> 20);
    }
    meta.sparse = nullptr;
    meta.private_bits.clear();
    meta.allocation = allocation;
    std::atomic_ref<VkBuffer>(meta.right).store(created, std::memory_order_release);
    ++R().buffer_generation;
    return vk::Buffer{created};
}
// The twin of a sparse view: made sharing the view's pieces (no copy: the right eye then reads
// the left bytes, as a twin copied from the left buffer did). An ordinary twin when VRAM has no
// room for it.
static vk::Buffer SparseTwinLocked(Vulkan::Scheduler& s, vk::Buffer b, Buffer& meta) {
    if (meta.right) return vk::Buffer{meta.right};
    std::vector<u64> bits;
    std::vector<u32> pieces;
    const vk::Buffer twin =
        meta.sparse->CreateTwin(meta.view_begin, meta.bytes, SparseTwinUsage, bits, pieces);
    if (!twin) return OrdinaryTwinLocked(s, b, meta);
    meta.private_bits = std::move(bits);
    meta.twin_pieces = std::move(pieces);
    meta.allocation = VK_NULL_HANDLE;
    std::atomic_ref<VkBuffer>(meta.right).store(VkBuffer(twin), std::memory_order_release);
    return twin;
}
// A per-eye write of [offset, offset + size) of a sparse view: the blocks the twin still shares
// get pieces of their own, filled with the left bytes the right eye read there until now, behind
// a NEW twin handle (the old one keeps its binds for the commands already recorded with it, and
// goes once the GPU is done with them).
// FAST_SPARSE_TWIN_ALL_PRIVATE=1 (a test, read once): a write makes the whole twin its own, as a
// twin copied from the left was: the eyes of a run with it and one without must match.
static const bool g_twin_all_private = [] {
    const char* value = std::getenv("FAST_SPARSE_TWIN_ALL_PRIVATE");
    return value && value[0] == '1';
}();
static vk::Buffer SparseWriteLocked(Vulkan::Scheduler& s, vk::Buffer b, Buffer& meta, u64 offset,
                                    u64 size) {
    namespace L = VideoCore::SparseLogic;
    if (g_twin_all_private) {
        offset = 0;
        size = meta.bytes;
    }
    SparseTwinLocked(s, b, meta);
    if (!meta.sparse) {
        // Now an ordinary twin (OrdinaryTwinLocked made one or stopped): a per-eye write never
        // goes to the left buffer.
        ASSERT_MSG(meta.right, "Sparse twins: a per-eye write without a right buffer");
        return vk::Buffer{meta.right};
    }
    if (SparseAllPrivate(meta, offset, size)) return vk::Buffer{meta.right};
    const auto [first, end] = SparseBlocks(meta, offset, size);
    if (first >= end) return vk::Buffer{meta.right};
    std::vector<u64> fresh;
    if (!meta.sparse->Privatize(meta.view_begin, meta.view_begin + (first << L::BlockBits),
                                (end - first) << L::BlockBits, fresh)) {
        return OrdinaryTwinLocked(s, b, meta); // nothing taken: a full copy keeps stereo right
    }
    std::vector<u64> bits;
    std::vector<u32> pieces;
    const vk::Buffer twin =
        meta.sparse->CreateTwin(meta.view_begin, meta.bytes, SparseTwinUsage, bits, pieces);
    if (!twin) {
        meta.sparse->Unprivatize(meta.view_begin, fresh); // no right piece left uninitialized
        return OrdinaryTwinLocked(s, b, meta);
    }
    std::vector<vk::BufferCopy> cow;
    std::sort(fresh.begin(), fresh.end());
    for (const u64 block : fresh) {
        const u64 at = block << L::BlockBits;
        if (!cow.empty() && cow.back().dstOffset + cow.back().size == at) {
            cow.back().size += L::BlockSize;
        } else {
            cow.push_back({at, at, L::BlockSize});
        }
    }
    if (!cow.empty()) {
        Barrier(s);
        s.Commands().copyBuffer(b, twin, cow);
        Barrier(s);
    }
    VideoCore::SparsePool* const pool = meta.sparse;
    const VkBuffer old = meta.right;
    const VmaAllocator alloc = meta.allocator;
    std::vector<u32> old_pieces = std::move(meta.twin_pieces);
    s.DeferPriorityOperation([pool, old, alloc, old_pieces] {
        vmaDestroyBuffer(alloc, old, VK_NULL_HANDLE);
        pool->ReleaseTwin(old_pieces);
    });
    meta.private_bits = std::move(bits);
    meta.twin_pieces = std::move(pieces);
    std::atomic_ref<VkBuffer>(meta.right).store(VkBuffer(twin), std::memory_order_release);
    ++R().buffer_generation; // carried hints and memos with the old twin's handle rebuild
    if constexpr (Common::Log::Enabled) {
        // How often a write needs new blocks (each a new twin handle and a memo rebuild).
        static u64 handles = 0, blocks = 0, next_log = 1;
        ++handles;
        blocks += fresh.size();
        if (handles >= next_log) {
            next_log = handles < 64 ? handles + 8 : handles + 256;
            LOG_INFO(Render_Vulkan, "Sparse twins: {} new twin handles so far, {} MiB made the right "
                     "eye's own (last: {} KB of a {} MiB view)", handles,
                     (blocks << L::BlockBits) >> 20, (fresh.size() << L::BlockBits) >> 10,
                     meta.bytes >> 20);
        }
    }
    return twin;
}
// Caller holds the registry mutex; `meta` is b's own entry (looked up or carried, bit 25).
// meta.right is stored atomically: FAST_CPU_FUSED_PAIR reads carried entries without the lock.
static vk::Buffer RightOfLocked(Vulkan::Scheduler& s,vk::Buffer b,bool create,Buffer& meta) {
    if (meta.sparse) {
        if (!create) return meta.right ? vk::Buffer{meta.right} : b;
        // A write with no range: the whole twin its own, as a twin copied from the left was.
        return SparseWriteLocked(s, b, meta, 0, meta.bytes);
    }
    if(!meta.right&&create) {
        const VkBufferCreateInfo ci{.sType=VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,.size=meta.bytes,
            .usage=VK_BUFFER_USAGE_TRANSFER_SRC_BIT|VK_BUFFER_USAGE_TRANSFER_DST_BIT|VK_BUFFER_USAGE_STORAGE_BUFFER_BIT|VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT|VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT};
        const VmaAllocationCreateInfo aci{.usage=VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE};
        VkBuffer created{};
        ASSERT(vmaCreateBuffer(meta.allocator,&ci,&aci,&created,&meta.allocation,nullptr)==VK_SUCCESS);
        VideoCore::VramReport::Name(meta.allocator,meta.allocation,fmt::format("stereo right buffer {} MiB of handle {:#x}",meta.bytes>>20,reinterpret_cast<u64>(VkBuffer(b))));
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
static vk::Buffer RightForWriteLocked(Vulkan::Scheduler& s, vk::Buffer b, Buffer& meta, u64 offset,
                                      u64 size) {
    return meta.sparse ? SparseWriteLocked(s, b, meta, offset, size) : RightOfLocked(s, b, true, meta);
}
vk::Buffer RightForWrite(Vulkan::Scheduler& s, vk::Buffer b, u64 offset, u64 size) {
    if (!Enabled() || !b) return b;
    std::lock_guard lock(R().mutex);
    auto it = R().buffers.find(Key(VkBuffer(b)));
    ASSERT_MSG(it != R().buffers.end(), "Native stereo: unregistered writable buffer");
    return RightForWriteLocked(s, b, it->second, offset, size);
}
std::vector<std::pair<u64, u64>> RightOwnRanges(vk::Buffer b, u64 offset, u64 size, vk::Buffer& right) {
    right = vk::Buffer{};
    if (!Enabled() || !b) return {};
    std::lock_guard lock(R().mutex);
    auto it = R().buffers.find(Key(VkBuffer(b)));
    if (it == R().buffers.end() || !it->second.right) return {};
    right = vk::Buffer{it->second.right};
    if (!it->second.sparse) return {{offset, size}};
    std::vector<std::pair<u64, u64>> ranges;
    SparseForPrivate(it->second, offset, size, [&](u64 o, u64 n) { ranges.emplace_back(o, n); });
    return ranges;
}
bool IsSparseTwin(vk::Buffer b) {
    if (!Enabled() || !b) return false;
    std::lock_guard lock(R().mutex);
    auto it = R().buffers.find(Key(VkBuffer(b)));
    return it != R().buffers.end() && it->second.sparse != nullptr;
}
void CopyRight(Vulkan::Scheduler& s, vk::Buffer src, vk::Buffer dst,
               std::span<const vk::BufferCopy> copies) {
    if (!Enabled()) return;
    const vk::Buffer right_src = ExistingRight(src);
    if (!right_src) return;
    {
        // Sparse views both: the right eye's own pieces are the blocks', so the new view's twin
        // binds them as it is made (sharing the rest): nothing to copy.
        std::lock_guard lock(R().mutex);
        auto si = R().buffers.find(Key(VkBuffer(src))), di = R().buffers.find(Key(VkBuffer(dst)));
        if (si != R().buffers.end() && di != R().buffers.end() && si->second.sparse &&
            di->second.sparse) {
            SparseTwinLocked(s, dst, di->second);
            if (di->second.sparse) return;
            // No VRAM for a sparse twin: an ordinary one, a copy of the left bytes, which gets
            // the source's right bytes below like any ordinary twin.
        }
    }
    const vk::Buffer right_dst = RightBuffer(s, dst, true); // its own barriers when made
    if (!right_dst || right_dst == dst) return;
    Barrier(s);
    s.Commands().copyBuffer(right_src, right_dst, copies);
    Barrier(s);
}
void Copy(Vulkan::Scheduler& s,vk::Buffer src,vk::Buffer dst,std::span<const vk::BufferCopy> copies, bool hle, vk::Buffer outer_right) {
    ScratchProfile::Cpu scratch_cpu(19,"Copy");
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
    // Flat too: the HLE copy shader's tiny adjacent ranges as separate regions cost the GPU ~8x
    // the merged copies (flat 720p: 2.9 ms a frame against VR's 0.36 ms for the same calls).
    if(!Enabled()) { s.Commands().copyBuffer(src,dst,copies); return; }
    // FAST_GPU_PASS_PROFILE: the HLE copy shader's copies stay in its label (many batches).
    ScratchProfile::Gpu scratch_gpu(s,40,!hle);
    {
        // Into a sparse view: where the source holds per-eye bytes the destination's twin gets
        // those blocks first (RightForWrite); the right copy then goes only where the twin has
        // blocks of its own (the shared rest is the left memory, written by the left copy).
        std::unique_lock lock(R().mutex);
        auto di = R().buffers.find(Key(VkBuffer(dst)));
        if (di != R().buffers.end() && di->second.sparse) {
            Buffer& dm = di->second;
            auto si = R().buffers.find(Key(VkBuffer(src)));
            const Buffer* const sm = si != R().buffers.end() ? &si->second : nullptr;
            const vk::Buffer right_src = sm && sm->right ? vk::Buffer{sm->right} : vk::Buffer{};
            if (right_src) {
                for (const auto& c : copies) {
                    if (!sm->sparse || SparseAnyPrivate(*sm, c.srcOffset, c.size)) {
                        SparseWriteLocked(s, dst, dm, c.dstOffset, c.size);
                    }
                }
            }
            std::vector<vk::BufferCopy> right_copies;
            if (dm.right && dm.sparse) {
                for (const auto& c : copies) {
                    SparseForPrivate(dm, c.dstOffset, c.size, [&](u64 o, u64 n) {
                        right_copies.push_back({c.srcOffset + (o - c.dstOffset), o, n});
                    });
                }
            } else if (dm.right) {
                // SparseWriteLocked above found no VRAM and made the twin an ordinary one (its
                // private bits gone): every range is the right eye's own from now on.
                right_copies.assign(copies.begin(), copies.end());
            }
            const vk::Buffer right_dst = dm.right ? vk::Buffer{dm.right} : vk::Buffer{};
            lock.unlock();
            s.Commands().copyBuffer(src, dst, copies);
            if (!right_copies.empty() && right_dst && right_dst != dst) {
                Barrier(s);
                s.Commands().copyBuffer(right_src ? right_src : src, right_dst, right_copies);
                Barrier(s);
            }
            return;
        }
    }
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
            LOG_INFO(Render_Vulkan, "HLE copy distribution: post-merge buckets 1/2-7/8-31/32-127/128+ = {}/{}/{}/{}/{}; dword-aligned 8+ batches {}, bytes {}", hle_batches[0], hle_batches[1], hle_batches[2], hle_batches[3], hle_batches[4], hle_aligned_large, hle_large_bytes);
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
    // Sparse twins first (2026-11-06, a review; Fused::Pair does the same): a write that needs new
    // private blocks gives its entry a new right handle (SparseWriteLocked), so a read of the same
    // buffer paired before it in this draw would keep the old twin, whose blocks there still show
    // the left eye. Every written sparse range is made private before anything is paired; the
    // pass below then finds it private, and every read and write of it takes the same right.
    for (const auto& w : original) {
        if (w.descriptorType != vk::DescriptorType::eStorageBuffer) continue;
        const bool written = reuse ? (w.dstBinding < cached.size() && cached[w.dstBinding]) : (*writable)[w.dstBinding];
        if (!written) continue;
        for (u32 i = 0; i < w.descriptorCount; ++i) {
            const auto& left = w.pBufferInfo[i];
            if (!left.buffer) continue;
            std::unique_lock lock(R().mutex, std::defer_lock);
            if (!batch_registry) lock.lock();
            Buffer* entry = nullptr;
            if (buffer_hints) {
                const size_t index = HintIndex(w.pBufferInfo + i, handles->buffer_base);
                if (index < handles->buffers.size() && handles->buffers[index].left == left.buffer)
                    entry = handles->buffers[index].entry;
            }
            if (!entry) {
                const auto it = R().buffers.find(Key(VkBuffer(left.buffer)));
                if (it != R().buffers.end()) entry = &it->second;
            }
            if (entry && entry->sparse)
                SparseWriteLocked(s, left.buffer, *entry, left.offset, left.range);
        }
    }
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
                    if (written && hinted && hinted->sparse) {
                        right.buffer = SparseWriteLocked(s, left.buffer, *hinted, left.offset, left.range);
                    } else if (written && !hinted) {
                        std::unique_lock lock(R().mutex, std::defer_lock);
                        if (!batch_registry) lock.lock();
                        auto it = R().buffers.find(Key(VkBuffer(left.buffer)));
                        right.buffer = it != R().buffers.end()
                                           ? RightForWriteLocked(s, left.buffer, it->second, left.offset, left.range)
                                           : (batch_registry ? RightBufferLocked(s, left.buffer, true)
                                                             : RightBuffer(s, left.buffer, true));
                    } else {
                        right.buffer=hinted ? RightOfLocked(s,left.buffer,written,*hinted) :
                            batch_registry ? RightBufferLocked(s,left.buffer,written) : RightBuffer(s,left.buffer,written);
                    }
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
    // A sparse view's twin shares memory with the left: a write needs its range made its own.
    static bool Sparse(const Buffer& entry) { return entry.sparse != nullptr; }
    vk::Buffer Write(const vk::DescriptorBufferInfo& left, Buffer& entry) {
        ++created;
        if (dry) return entry.right ? vk::Buffer{entry.right} : left.buffer;
        Hold hold{*this};
        ++recorded;
        return SparseWriteLocked(s, left.buffer, entry, left.offset, left.range);
    }
    // Fused::Pair's first pass: a written sparse range made private before anything is paired, so
    // every read and write of it in the draw takes the same right (the pass's Write then finds it
    // private: no second twin). Not carried: the registry's entry. Others: nothing.
    void PrivatizeWrite(const vk::DescriptorBufferInfo& left, Buffer* entry) {
        if (dry || (entry && !entry->sparse)) return;
        Hold hold{*this};
        if (!entry) {
            const auto it = R().buffers.find(Key(VkBuffer(left.buffer)));
            if (it == R().buffers.end() || !it->second.sparse) return;
            entry = &it->second;
        }
        ++recorded;
        SparseWriteLocked(s, left.buffer, *entry, left.offset, left.range);
    }
    // A written descriptor's buffer without its right yet: created here, in the pass's order.
    vk::Buffer Create(vk::Buffer left, Buffer& entry) {
        ++created;
        if (dry) return left;
        Hold hold{*this};
        ++recorded;
        return RightOfLocked(s, left, true, entry);
    }
    // RightBufferLocked, and the entry's stream flag for the mirror.
    vk::Buffer Lookup(const vk::DescriptorBufferInfo& info, bool written, bool& stream) {
        ++lookups;
        const vk::Buffer left = info.buffer;
        Hold hold{*this};
        const auto it = R().buffers.find(Key(VkBuffer(left)));
        ASSERT_MSG(it != R().buffers.end() || !written, "Native stereo: unregistered writable buffer");
        if (it == R().buffers.end()) return left;
        stream = it->second.stream;
        if (written && it->second.sparse) {
            if (dry) return it->second.right ? vk::Buffer{it->second.right} : left;
            ++recorded;
            return SparseWriteLocked(s, left, it->second, info.offset, info.range);
        }
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
bool CompactRights(const Vulkan::Pipeline& pipeline, std::span<const vk::WriteDescriptorSet> writes,
                   const Handles& h, Vulkan::StereoSetCodec::Rights& out) {
    using Vulkan::StereoSetCodec::Kind;
    const auto& registry = R();
    if (registry.buffer_generation.load(std::memory_order_acquire) != h.buffer_generation ||
        registry.view_generation.load(std::memory_order_acquire) != h.view_generation) {
        return false;
    }
    const auto writable = pipeline.NativeWritableBindings();
    const auto handle = [](auto object) {
        return reinterpret_cast<u64>(static_cast<typename decltype(object)::CType>(object));
    };
    out.paired = 0;
    out.cameras_used = 0;
    const auto push = [&out](u64 value) {
        if (out.paired == out.handles.size()) {
            out.handles.resize(out.handles.size() + 64);
        }
        out.handles[out.paired++] = value;
    };
    u32 buffer_index = 0;
    for (const auto& w : writes) {
        const u32 n = w.descriptorCount;
        switch (Vulkan::StereoSetCodec::KindOf(w.descriptorType)) {
        case Kind::Buffer: {
            if (w.pNext || !w.pBufferInfo) {
                return false;
            }
            const bool written = w.dstBinding < writable.size() && writable[w.dstBinding];
            for (u32 i = 0; i < n; ++i, ++buffer_index) {
                const vk::DescriptorBufferInfo& left = w.pBufferInfo[i];
                u64 right = handle(left.buffer);
                if (left.buffer) {
                    // Fused::Pair's carried rules, where it needs nothing recorded or looked up.
                    const std::size_t index = Fused::IndexIn(w.pBufferInfo + i, h.buffer_base,
                                                             h.buffer_count);
                    const BufferHint* const hint =
                        index < h.buffers.size() && h.buffers[index].left == left.buffer
                            ? &h.buffers[index]
                            : nullptr;
                    if (!hint || !hint->entry) {
                        return false; // the registry lookup and the camera scan
                    }
                    const vk::Buffer have = CarriedOps::RightOf(*hint->entry);
                    if (!have && written) {
                        return false; // its right buffer would be created
                    }
                    if (written && CarriedOps::Sparse(*hint->entry)) {
                        if constexpr (Common::Log::Enabled) {
                            static std::atomic<u64> fallbacks{0};
                            const u64 n = ++fallbacks;
                            if ((n & (n - 1)) == 0 && n >= 1024) {
                                LOG_INFO(Render_Vulkan, "Sparse twins: {} draws left the compact "
                                         "pairing for a written sparse twin", n);
                            }
                        }
                        return false; // a sparse twin's written range may need blocks of its own
                    }
                    const vk::Buffer right_buffer = have ? have : left.buffer;
                    if (right_buffer != left.buffer && !written && CarriedOps::Stream(*hint->entry)) {
                        return false; // a stream mirror would be recorded
                    }
                    right = handle(right_buffer);
                    if (hint->camera.buffer) {
                        if (out.cameras_used == out.cameras.size()) {
                            out.cameras.resize(out.cameras.size() + 8);
                        }
                        out.cameras[out.cameras_used++] = {buffer_index, 0, hint->camera};
                    }
                }
                push(right);
            }
            break;
        }
        case Kind::Image: {
            if (w.pNext || !w.pImageInfo) {
                return false;
            }
            for (u32 i = 0; i < n; ++i) {
                const vk::DescriptorImageInfo& left = w.pImageInfo[i];
                const std::size_t index =
                    Fused::IndexIn(w.pImageInfo + i, h.image_base, h.image_count);
                if (!(index < h.images.size() && h.images[index].left == left.imageView &&
                      (h.images[index].right || !left.imageView))) {
                    return false; // the registry's view lookup
                }
                push(handle(h.images[index].right));
            }
            break;
        }
        default:
            break;
        }
    }
    return true;
}
}
