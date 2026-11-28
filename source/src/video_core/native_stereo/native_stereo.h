// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
// Experimental full-scene stereo plumbing; only compiled in the isolated worktree.
#include <cstdlib>
#include "common/types.h"
#include <span>
#include <string_view>
#include <array>
#include <vector>
#include "video_core/renderer_vulkan/vk_common.h"
VK_DEFINE_HANDLE(VmaAllocator)
namespace Vulkan { class ConstantRing; class Scheduler; class Instance; class Pipeline; struct RenderState; }
namespace VideoCore { class StreamBuffer; struct ImageInfo; class SparsePool; }
namespace VR { struct CameraRecord; struct Fov; }
namespace Vulkan::StereoSetCodec { struct Rights; }
namespace AmdGpu { union Regs; }
namespace NativeStereo {
inline const bool g_native_stereo = std::getenv("VR_NATIVE_STEREO") != nullptr;
inline bool Enabled() { return g_native_stereo; }
// FAST_CPU_FUSED_PAIR (on unless '0'; read once): with bit 25, pairing takes what the binding
// carried (PairCarried) instead of the registry pass. The pass's audits (VR_DESCRIPTOR_LOOKUP_AUDIT,
// VR_NATIVE_CAMERA_AUDIT, VR_PERF_CHECK) and bit 25's sampled check (FAST_STEREO_HANDLES_VERIFY)
// keep the registry pass.
inline const bool g_fused_pair = [] {
    const char* value = std::getenv("FAST_CPU_FUSED_PAIR");
    const char* handles_verify = std::getenv("FAST_STEREO_HANDLES_VERIFY");
    return (!value || value[0] != '0') && !std::getenv("VR_DESCRIPTOR_LOOKUP_AUDIT") &&
           !std::getenv("VR_NATIVE_CAMERA_AUDIT") && !std::getenv("VR_PERF_CHECK") &&
           !(handles_verify && std::atoi(handles_verify) > 0);
}();
inline bool FusedPair() { return g_native_stereo && g_fused_pair; }
// Layer draws: both eyes without the driver's multiview. A both-eye pass renders its two-layer
// attachments with layerCount 2 and viewMask 0, each of its draws is recorded once per eye (the
// right eye's with vr_hud's sign bit set: Shader::PushData::NativeRightEyeBit), and the last
// vertex stage writes gl_Layer from that bit. VR_NATIVE_VIEWS=layers or multiview chooses; by
// default AMD RX 6000 cards draw layers (VR::NativeViews::LayersByDefault): an RX 6700's multiview
// right eye stayed white or grey once the world loaded (GitHub #9, driver 2.0.353; the menus and
// DIBR were fine), while other generations' multiview works (RX 9070 XT). Needs
// shaderOutputLayer. Chosen once at device creation, before any pipeline or pass, in every session.
bool LayerDraws(); // native stereo with layer draws
// The card's mode whether or not native stereo is on (Shader::Profile::native_layer_draws): DIBR
// and native sessions on one card then keep one shader cache, whose profile they share.
bool LayerDrawsMode();
void ChooseLayerDraws(u32 vendor_id, u32 device_id, std::string_view device_name,
                      bool shader_output_layer);
// Narrow, opt-in experiment for the audited Bloodborne directional shadow atlas.
bool SharedShadowAtlas(const VideoCore::ImageInfo&);
// A perspective (spot light) camera drew into the shared atlas: logged once, never fatal.
void SharedShadowPerspective(float m66, float m67);
// The captured frame distance; numeric environment overrides remain for experiments.
float Ipd(const VR::CameraRecord&);
bool AsymmetricProjection();
// The air pass (shader_recompiler/ir/passes/air_light.h): each eye's inverse projection (ndc x, y,
// raw depth -> its view) from the flat camera's cascades336, by LightingBuffer's own remap.
// False, both the flat camera's, without asymmetric eyes or when the layout is not that.
bool EyeInverseProjections(const float* cascades336, const VR::CameraRecord&,
                           std::array<std::array<float, 16>, 2>& out);
// The lamp shadows' edge pass (vr_shadow_edges.h): each eye's inverse projection (ndc x, y, raw
// depth -> its view) as LightingBuffer last remapped a light pass's camera (the lamp shaders' own
// matrix, light_shadow.h SceneMaps::EdgePass), outside a mirror's pass. False before the first.
bool LightInverseProjections(std::array<std::array<float, 16>, 2>& out);
// The scene projection for one eye. A held Monocular narrows only its optical eye;
// the headset submission continues to use CameraRecord::eye_fov unchanged.
VR::Fov SceneEyeFov(const VR::CameraRecord&, u32 eye);
// FAST_GPU_MIRROR_FLIP (on unless '0'; read once), with the asymmetric projection: a planar
// reflection's mirrored camera (NativeProjection::MirroredCamera) and the rest of its pass
// (NativeProjection::MirrorPass) get each eye's frustum upside down (NativeProjection::Flipped).
bool MirrorFlip();
void ClearCameraPairs();
void RegisterCameraPair(vk::DescriptorBufferInfo left, vk::DescriptorBufferInfo right);
// The right of the camera pair registered last on this thread (CameraBuffer, LightingBuffer,
// ScreenPointBuffer, ScreenRectBuffer, LightListBuffer or RegisterCameraPair), empty when there
// is none.
vk::DescriptorBufferInfo LastCameraRight();

// Each eye's asymmetric remap {x_scale, y_scale, x_offset, y_offset} for this camera record
// (RuntimeInfo::native_projection, Shader::VrPushData::projection); all zero unless both eyes
// have one. Memoized per thread: the draws of a camera update share it. `mirror`: a draw of a
// planar reflection's pass, each frustum upside down (NativeProjection::Flipped).
std::array<std::array<float,4>,2> DrawProjection(const VR::CameraRecord&, bool mirror = false);
// The draw's light tile in guest pixels [x0, y0, x1, y1): not empty and not the whole screen
// (LightScissors' registers, no camera; VrPushData's light-scissor permutations).
bool LightTile(const AmdGpu::Regs&, std::array<float,4>* rect = nullptr);
// LightTile remapped into each eye (false also when an eye has no projection); `mirror` as
// DrawProjection's.
bool LightScissors(const AmdGpu::Regs&, const VR::CameraRecord&, std::array<std::array<float,4>,2>&,
                   bool mirror = false);
// Optional exact per-eye camera capture, including camera-upload memo hits. OFF callers
// pass null: no additional camera reads/copies or inverse calculations are performed.
// `mirrored` (MirrorFlip; null: never): set when the buffer is a planar reflection's mirrored
// camera (NativeProjection::MirroredCamera, with the record's render camera when it has one),
// which then got each eye's frustum upside down and its clip plane per eye.
using CameraConstants = std::array<std::array<float, 216>, 2>;
// CameraBuffer's answer without pairing anything: a camera it pairs, and `mirrored` as its own
// (Rasterizer::VrMirrorAhead: a draw's camera before its fragment lights).
bool SceneCamera(const float*, const VR::CameraRecord&, bool* mirrored);
bool CameraBuffer(VideoCore::StreamBuffer&, const float*, u32, const VR::CameraRecord&, vk::DescriptorBufferInfo&, CameraConstants* capture = nullptr, bool* projection_paired = nullptr, bool* mirrored = nullptr);
// `mirror` as DrawProjection's (a light of a planar reflection's pass). `world_scale` (null: not
// asked): set to the record's world scale when paired (LightListBuffer's).
bool LightingBuffer(VideoCore::StreamBuffer&, const float*, u32 size, u32 alignment, const VR::CameraRecord&, vk::DescriptorBufferInfo&, bool mirror = false, float* world_scale = nullptr);
// FAST_GPU_LIGHT_EYE (on unless '0'; read once), with the asymmetric projection: a deferred
// point or spot light program whose light list's layout is known
// (NativeProjection::LightListLayout). '0': its list is bound as before, the same in both eyes.
bool LightListShader(u64 pgm_hash);
// LightListShader's light list (its buffer 1, of NativeProjection::LightListFor's size), for a
// draw whose light208 camera (buffer 0) LightingBuffer paired with a record of `world_scale`: a
// copy per eye with each light moved like that eye's camera (NativeProjection::RemapLightList,
// with LightingBuffer's eye shift). `guest` as ScreenPointBuffer's. False, having mapped nothing,
// for anything else.
bool LightListBuffer(VideoCore::StreamBuffer&, const float* source, u32 size, u32 alignment,
                     bool guest, u64 pgm_hash, float world_scale, float ipd,
                     vk::DescriptorBufferInfo&);
// NativeProjection::ScreenPointShader; the glow's god rays (0xe55e45eb) only while
// FAST_GPU_GODRAY_EYE (default on, read once) is not '0'.
bool ScreenPointShader(u64 pgm_hash);
// ScreenPointShader's buffer: a copy per eye with the screen point (at the shader's
// NativeProjection::ScreenPointDword) remapped.
// `guest`: `source` is guest memory (else a host copy, e.g. the constant ring's).
bool ScreenPointBuffer(VideoCore::StreamBuffer&, const float* source, u32 size, u32 alignment,
                       bool guest, u64 pgm_hash, const VR::CameraRecord&,
                       vk::DescriptorBufferInfo&);
// Shader::Info::ScreenRect's buffer: a copy per eye with the pixel rectangle (dwords {dst x, y,
// src x, y (0xFF: none), end x, y} of a width x height eye image) where that eye sees it;
// `groups`: the workgroups of `group` threads covering either eye's rectangle.
bool ScreenRectBuffer(VideoCore::StreamBuffer&, const u32* source, u32 size, u32 alignment,
                      bool guest, const std::array<u8, 6>& dwords, u32 width, u32 height,
                      std::array<u32, 2> group, const VR::CameraRecord&,
                      vk::DescriptorBufferInfo&, std::array<u32, 2>& groups);
bool CameraBuffer(Vulkan::ConstantRing&, const float*, u32, const VR::CameraRecord&,
                  vk::DescriptorBufferInfo&, vk::DescriptorBufferInfo&, bool*,
                  bool* mirrored = nullptr);
bool LightingBuffer(Vulkan::ConstantRing&, const float*, u32, u32, const VR::CameraRecord&,
                    vk::DescriptorBufferInfo&, vk::DescriptorBufferInfo&);

// `flipped`: paired upside down (a planar reflection's mirrored camera, or a light of its pass).
void AuditCameraBuffer(const float*, u32 size, u64 shader, u32 stage, u32 binding,
                       bool mapped, bool projected, const VR::CameraRecord&,
                       bool flipped = false);
// Registry entry of one buffer: its address is stable from RegisterBuffer until ForgetBuffer.
struct BufferEntry;
BufferEntry* RegisterBuffer(vk::Buffer, VkDeviceSize, VmaAllocator, bool stream);
// A sparse view of the buffer cache (buffer_cache/sparse_pool.h) at guest `view_begin`: its
// twin shares the view's pieces, except blocks the right eye writes (RightForWrite).
BufferEntry* RegisterSparseView(vk::Buffer, VkDeviceSize, VmaAllocator, VideoCore::SparsePool*,
                                u64 view_begin);
void ForgetBuffer(vk::Buffer);
// The buffer keeps its entry, its right twin goes (the buffer cache's eviction of an idle
// buffer's twin): memos holding the twin see a new buffer generation; the twin is destroyed once
// the GPU is past the current tick (background thread). A later per-eye write makes a new twin
// from the left copy. Returns its bytes (0: no twin, or a stream buffer's).
VkDeviceSize ReleaseRight(Vulkan::Scheduler&, vk::Buffer);
vk::Buffer ExistingRight(vk::Buffer);
vk::Buffer RightBuffer(Vulkan::Scheduler&,vk::Buffer,bool create);
// The right buffer of `b` for a per-eye write of [offset, offset + size): a sparse view's twin
// has that range in its own memory after this (copy-on-write, maybe a new twin handle); any
// other buffer as RightBuffer(create). `b` itself when VRAM has no room (mono there).
vk::Buffer RightForWrite(Vulkan::Scheduler&, vk::Buffer b, u64 offset, u64 size);
// The parts of [offset, offset + size) the twin of `b` holds in its own memory: all of it for
// an ordinary twin, the private blocks for a sparse view's (the rest shares the left bytes, so
// writing the same bytes to both eyes skips it). Empty, and `right` null, without a twin.
std::vector<std::pair<u64, u64>> RightOwnRanges(vk::Buffer b, u64 offset, u64 size, vk::Buffer& right);
// A sparse view's twin (shares the left memory in part): FAST bit 63's paired paths skip it.
bool IsSparseTwin(vk::Buffer b);
void Barrier(Vulkan::Scheduler&);
// outer_right (FAST bit 63): the caller's own barriers before and after already name this right
// destination, so a pair whose right destination is outer_right and whose source has no right
// twin records both copies with no barrier between them; anything else keeps its barriers.
void Copy(Vulkan::Scheduler&,vk::Buffer,vk::Buffer,std::span<const vk::BufferCopy>, bool hle = false, vk::Buffer outer_right = {});
// Sparse views (buffer_cache/sparse_pool.h): a merge shares the left bytes, so only the right
// eye's copy of `src`, if it has one, moves into `dst`'s (made from dst's left when missing).
void CopyRight(Vulkan::Scheduler&, vk::Buffer src, vk::Buffer dst, std::span<const vk::BufferCopy>);
inline void Copy(Vulkan::Scheduler& s,vk::Buffer a,vk::Buffer b,const vk::BufferCopy& c) { Copy(s,a,b,std::span{&c,1}); }
void RegisterView(vk::ImageView,vk::ImageView,vk::ImageView);
void ForgetView(vk::ImageView);
vk::ImageView RightView(vk::ImageView);
vk::ImageView ArrayView(vk::ImageView);
void MultiviewState(Vulkan::RenderState&);
struct Descriptors {
    std::vector<vk::WriteDescriptorSet> writes;
    std::vector<vk::DescriptorBufferInfo> buffers;
    std::vector<vk::DescriptorImageInfo> images;
    // The infos the writes point into are the first buffers_used / images_used (PairCarried keeps
    // the arrays' size from call to call and fills their front).
    size_t buffers_used{}, images_used{};
    u32 mirrors{}; // stream mirrors recorded (FAST_CPU_FUSED_PAIR_VERIFY compares them)
};
// Fast bit 25: right-eye handles carried through binding instead of registry lookups. The
// hint arrays run parallel to the rasterizer's buffer/image info arrays (by address) and to
// its barrier list (by index). A hint is used only for the handle it names, under the batch
// registry lock (PairCarried: without it), and only while no buffer entry was erased / no view
// changed since binding (the generations); anything else takes the registry lookup.
// camera (FAST_CPU_FUSED_PAIR): the right of the descriptor's own camera pair (the one its
// binding registered), buffer null when it has none.
struct BufferHint { vk::Buffer left{}; BufferEntry* entry{}; vk::DescriptorBufferInfo camera{}; };
struct ViewHint { vk::ImageView left{}, right{}; }; // right null with left set: unknown
struct Handles {
    const vk::DescriptorBufferInfo* buffer_base{};
    std::span<const BufferHint> buffers;
    const vk::DescriptorImageInfo* image_base{};
    std::span<const ViewHint> images;
    std::span<const BufferHint> barriers;
    u64 buffer_generation{}, view_generation{};
    // FAST_CPU_FUSED_PAIR: the binding's info counts (every write points into them) and whether
    // the buffer hints carry their camera pairs (PairCarried).
    size_t buffer_count{}, image_count{};
    bool fused{};
};
u64 BufferGeneration();
u64 ViewGeneration();
// fold (FAST bit 28): unpaired samplers are copied into images too, so every write points into
// buffers or images and the recorder can copy each array once.
void PairDescriptors(Vulkan::Scheduler&,const Vulkan::Pipeline&,std::span<const vk::WriteDescriptorSet>, Descriptors&, bool, const Handles* = nullptr, bool fold = false);
// FAST_CPU_FUSED_PAIR: PairDescriptors from the carried handles alone (`handles.fused`), with the
// same writes, infos and recorded copies; the registry lock only for a descriptor that carried
// nothing or a right buffer to create. Returns false, having done nothing, when an entry was
// erased or a view changed since binding: the caller then runs PairDescriptors.
// FAST_CPU_FUSED_PAIR_VERIFY=1: PairDescriptors runs (from the registry alone) and is used; the
// carried pairing is compared with it.
bool PairCarried(Vulkan::Scheduler&, const Vulkan::Pipeline&, std::span<const vk::WriteDescriptorSet>,
                 Descriptors&, bool reuse, const Handles& handles, bool fold);
// ExistingRight of every barrier's buffer under one registry lock (null: no right).
void PairBarriers(std::span<const vk::BufferMemoryBarrier2>, const Handles&, std::span<vk::Buffer>);
// VR_COMPACT_SETS (Vulkan::StereoSetCodec): what Fused::Pair would give each paired info's right
// eye, from the carried handles alone, when its pairing needs nothing recorded or looked up (no
// right buffer to create, no stream mirror, no registry lookup) and no entry was erased or view
// changed since binding. False: the caller pairs as before (`out` then means nothing).
bool CompactRights(const Vulkan::Pipeline&, std::span<const vk::WriteDescriptorSet> writes,
                   const Handles& handles, Vulkan::StereoSetCodec::Rights& out);
}
