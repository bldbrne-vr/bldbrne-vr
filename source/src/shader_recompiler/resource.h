// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include "shader_recompiler/ir/type.h"
#include "shader_recompiler/vr_push_data.h"
#include "video_core/amdgpu/resource.h"

#include <boost/container/static_vector.hpp>

namespace Shader {

static constexpr u32 NUM_USER_DATA_REGS = 16;
static constexpr u32 NUM_IMAGES = 64;
static constexpr u32 NUM_BUFFERS = 40;
static constexpr u32 NUM_SAMPLERS = 16;
static constexpr u32 NUM_FMASKS = 8;

enum class BufferType : u32 {
    Guest,
    Flatbuf,
    BdaPagetable,
    FaultBuffer,
    GdsBuffer,
    SharedMemory,
    ClipPlanes,
    VrFogLights, // the light composite's lights in the air (ir/passes/fog_light.h)
    VrMoonShafts, // the light composite's moonlight in the air (ir/passes/moon_shafts.h)
    VrGrade, // cinematic grade parameters
    VrParticleLights, // the soft particles' lights (ir/passes/particle_air.h)
    VrAdaptation,     // eye adaptation's state, written by the tone map (ir/passes/cinematic_look.h)
    VrBodyCapsules,   // the hunter's body for the lamp shadows (ir/passes/body_shadow.h)
    VrBodyMaps,       // his body's depth maps from the lamps near him (body_shadow.h MaxMaps)
    VrSceneMaps,
    VrShadowMask,     // each half-res pixel's shadow from the slot lamps (light_shadow.h SceneMaps::Mask)
    VrShadowCasters,  // enemies and NPCs out of view, as capsules (light_shadow.h SceneMaps::Caster*)      // the lamps' cubes of the nearest surfaces (light_shadow.h SceneMaps)
    VrShadowGrid,     // the blockers both eyes have seen round him (light_shadow.h Grid), written
                      // by the lamp programs themselves
    VrDepthPyramid,   // the AO pass's depth in blocks, nearest and farthest (light_shadow.h
                      // SceneMaps::Pyramid; vr_depth_pyramid.h)
    VrAir,            // the air pass's parameters (ir/passes/air_light.h)
    VrAirCells,       // its half-size cells: the moonlight in the air, written by the shadow
                      // mask's blur, read by the scene's copy
    VrAirProbe,       // the moon's colour, summed by the light composite (host-visible)
    VrFogGrid,        // the fog grid's cells, both eyes' (ir/passes/fog_grid.h; vr_fog_grid.h):
                      // written by a dispatch of our own, read by the scene's copy
    VrShadowEdges,    // the edge pass's two bits a lamp a pixel (light_shadow.h EdgePass)
};

// VR host images (vr_host_image.cpp): an image a shader reads that is none of its draw's own, a
// slot the rasterizer fills with a T# it captured at another draw (the moon's shadow atlas for
// the light composite, ir/passes/moon_shafts.h). Such an ImageResource has sharp_idx
// VrHostImageBase + slot, never a user-data index; GetSharp gives the slot's T#, or a null image
// while it has none. The T# stays until another one is captured (a preparation worker that reads
// it ahead of the draw sees the same image).
inline constexpr u32 VrHostImageBase = 0xFFFFFF00u;
enum class VrHostImage : u32 {
    MoonAtlas = 0,
    AirDepth, // the eye depth the moon's shadow pass reads (ir/passes/air_light.h)
    Count,
};
AmdGpu::Image GetVrHostImage(VrHostImage slot);
// True when the slot's T# changed.
bool SetVrHostImage(VrHostImage slot, const AmdGpu::Image& image);
// The same for a buffer (a guest buffer of another draw, bound as that draw binds it: its GPU
// contents): a BufferResource of type Guest with sharp_idx VrHostBufferBase + slot.
inline constexpr u32 VrHostBufferBase = 0xFFFFFF00u;
enum class VrHostBuffer : u32 {
    MoonSets = 0, // the directional light's light-set table (ir/passes/moon_shafts.h)
    Count,
};
AmdGpu::Buffer GetVrHostBuffer(VrHostBuffer slot);
bool SetVrHostBuffer(VrHostBuffer slot, const AmdGpu::Buffer& buffer);

struct Info;

struct BufferResource {
    u32 sharp_idx;
    IR::Type used_types;
    AmdGpu::Buffer inline_cbuf;
    BufferType buffer_type;
    u8 instance_attrib{};
    bool is_written{};
    bool is_formatted{};

    bool IsSpecial() const noexcept {
        return buffer_type != BufferType::Guest;
    }

    constexpr bool IsVrHost() const noexcept {
        return !inline_cbuf && sharp_idx >= VrHostBufferBase &&
               sharp_idx < VrHostBufferBase + static_cast<u32>(VrHostBuffer::Count);
    }

    constexpr AmdGpu::Buffer GetSharp(const auto& info) const noexcept {
        AmdGpu::Buffer buffer{};
        if (inline_cbuf) {
            buffer = inline_cbuf;
            if (inline_cbuf.base_address != 1) {
                buffer.base_address += info.ProgramBase(); // address fixup
            }
        } else if (IsVrHost()) {
            buffer = GetVrHostBuffer(static_cast<VrHostBuffer>(sharp_idx - VrHostBufferBase));
        } else {
            buffer = info.template ReadUdSharp<AmdGpu::Buffer>(sharp_idx);
        }
        if (!buffer.Valid()) {
            LOG_DEBUG(Render, "Encountered invalid buffer sharp");
            return AmdGpu::Buffer::Null();
        }
        return buffer;
    }
};
using BufferResourceList = boost::container::static_vector<BufferResource, NUM_BUFFERS>;

enum class MipStorageFallbackMode : u32 { None, DynamicIndex, ConstantIndex };

struct ImageResource {
    u32 sharp_idx;
    bool is_depth{};
    bool is_atomic{};
    bool is_array{};
    bool is_written{};
    bool is_r128{};
    MipStorageFallbackMode mip_fallback_mode{};
    u32 constant_mip_index{};

    constexpr bool IsVrHost() const noexcept {
        return sharp_idx >= VrHostImageBase &&
               sharp_idx < VrHostImageBase + static_cast<u32>(VrHostImage::Count);
    }

    constexpr AmdGpu::Image GetSharp(const auto& info) const noexcept {
        AmdGpu::Image image{};
        if (IsVrHost()) {
            image = GetVrHostImage(static_cast<VrHostImage>(sharp_idx - VrHostImageBase));
        } else if (!is_r128) {
            image = info.template ReadUdSharp<AmdGpu::Image>(sharp_idx);
        } else {
            const auto raw = info.template ReadUdSharp<u128>(sharp_idx);
            std::memcpy(&image, &raw, sizeof(raw));
            image.pitch = image.width;
        }
        if (!image.Valid()) {
            LOG_DEBUG(Render_Vulkan, "Encountered invalid image sharp");
            image = AmdGpu::Image::Null(is_depth);
        } else if (is_depth) {
            const auto data_fmt = image.GetDataFmt();
            if (data_fmt != AmdGpu::DataFormat::Format16 &&
                data_fmt != AmdGpu::DataFormat::Format32) {
                LOG_DEBUG(Render_Vulkan,
                          "Encountered non-depth image used with depth instruction!");
                image = AmdGpu::Image::Null(true);
            }
        }
        return image;
    }

    u32 NumBindings(const auto& info) const {
        const AmdGpu::Image tsharp = GetSharp(info);
        return (mip_fallback_mode == MipStorageFallbackMode::DynamicIndex)
                   ? (tsharp.last_level - tsharp.base_level + 1)
                   : 1;
    }
};
using ImageResourceList = boost::container::static_vector<ImageResource, NUM_IMAGES>;

struct SamplerResource {
    u32 sharp_idx;
    AmdGpu::Sampler inline_sampler;
    u32 is_inline_sampler : 1;
    u32 associated_image : 4;
    u32 disable_aniso : 1;

    constexpr AmdGpu::Sampler GetSharp(const auto& info) const noexcept {
        return is_inline_sampler ? inline_sampler
                                 : info.template ReadUdSharp<AmdGpu::Sampler>(sharp_idx);
    }
};
using SamplerResourceList = boost::container::static_vector<SamplerResource, NUM_SAMPLERS>;

struct FMaskResource {
    u32 sharp_idx;

    constexpr AmdGpu::Image GetSharp(const auto& info) const noexcept {
        return info.template ReadUdSharp<AmdGpu::Image>(sharp_idx);
    }
};
using FMaskResourceList = boost::container::static_vector<FMaskResource, NUM_FMASKS>;

struct PushData {
    static constexpr u32 XOffsetIndex = 0;
    static constexpr u32 YOffsetIndex = 1;
    static constexpr u32 XScaleIndex = 2;
    static constexpr u32 YScaleIndex = 3;
    static constexpr u32 UdRegsIndex = 4;
    static constexpr u32 BufOffsetIndex = UdRegsIndex + NUM_USER_DATA_REGS / 4;
    static constexpr u32 VrShiftIndex = BufOffsetIndex + 3;
    static constexpr u32 VrHudIndex = BufOffsetIndex + 4;
    // In native graphics only, vr_hud carries these small integer flags as a float.
    static constexpr u32 NativeWorld = 1;
    static constexpr u32 NativeCameraPaired = 2;
    static constexpr u32 NativeProjectionPaired = 4;
    // Layer draws (NativeStereo::LayerDraws): the right eye's copy of a both-eye draw pushes
    // vr_hud with its sign bit set (the flags are >= 0: their value is its absolute value).
    static constexpr u32 NativeRightEyeBit = 0x8000'0000u;

    float xoffset;
    float yoffset;
    float xscale;
    float yscale;
    std::array<u32, NUM_USER_DATA_REGS> ud_regs;
    std::array<u8, NUM_BUFFERS> buf_offsets;
    float vr_shift; // VR stereo: clip-space x offset for perspective geometry
    float vr_hud;   // VR HUD panel: round(v) = scale * 16, remainder = stereo offset;
                    // < -1: aa_mode=taa jitter, y = v + 2 and x = vr_shift (NDC, times w)

    void AddOffset(u32 binding, u32 offset) {
        ASSERT(offset < 256 && binding < buf_offsets.size());
        buf_offsets[binding] = offset;
    }
};
static_assert(sizeof(PushData) <= 128,
              "PushData size is greater than minimum size guaranteed by Vulkan spec");
// Native stereo graphics: the per-draw VR values follow PushData (vr_push_data.h).
static_assert(sizeof(PushData) == VrPushData::Begin &&
              PushData::VrHudIndex + 1 == VrPushData::SceneInverseIndex);

} // namespace Shader
