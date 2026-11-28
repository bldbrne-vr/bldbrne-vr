// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstring>
#include <ranges>
#include <span>
#include "common/types.h"
#include "shader_recompiler/frontend/tessellation.h"
#include "shader_recompiler/vr_motion.h"
#include "video_core/amdgpu/pixel_format.h"
#include "video_core/amdgpu/regs_shader.h"
#include "video_core/amdgpu/regs_vertex.h"

namespace Shader {

enum class Stage : u32 {
    Fragment,
    Vertex,
    Geometry,
    Export,
    Hull,
    Local,
    Compute,
};

// Vertex intentionally comes after TCS/TES due to order of compilation
enum class LogicalStage : u32 {
    Fragment,
    TessellationControl,
    TessellationEval,
    Vertex,
    Geometry,
    Compute,
    NumLogicalStages
};

constexpr u32 MaxStageTypes = static_cast<u32>(LogicalStage::NumLogicalStages);
constexpr auto MaxEmulatedClipDistances = 4u;

constexpr Stage StageFromIndex(size_t index) noexcept {
    return static_cast<Stage>(index);
}

struct CommonHsEsVsRuntimeInfo {
    u32 hs_output_cp_stride;

    bool operator<=>(const CommonHsEsVsRuntimeInfo&) const noexcept = default;
};

struct CommonEsVsRuntimeInfo : protected CommonHsEsVsRuntimeInfo {
    AmdGpu::TessellationType tess_type;
    AmdGpu::TessellationTopology tess_topology;
    AmdGpu::TessellationPartitioning tess_partitioning;

    bool operator<=>(const CommonEsVsRuntimeInfo&) const noexcept = default;
};

struct LocalRuntimeInfo {
    u32 ls_stride;

    auto operator<=>(const LocalRuntimeInfo&) const noexcept = default;
};

struct ExportRuntimeInfo : protected CommonEsVsRuntimeInfo {
    u32 vertex_data_size;

    bool operator<=>(const ExportRuntimeInfo&) const noexcept = default;
};

enum class Output : u8 {
    None,
    PointSize,
    EdgeFlag,
    KillFlag,
    GsCutFlag,
    RenderTargetIndex,
    ViewportIndex,
    CullDist0,
    CullDist1,
    CullDist2,
    CullDist3,
    CullDist4,
    CullDist5,
    CullDist6,
    CullDist7,
    ClipDist0,
    ClipDist1,
    ClipDist2,
    ClipDist3,
    ClipDist4,
    ClipDist5,
    ClipDist6,
    ClipDist7,
};
using OutputMap = std::array<Output, 4>;

struct VertexRuntimeInfo : protected CommonEsVsRuntimeInfo {
    u32 num_outputs;
    std::array<OutputMap, 3> outputs;
    u32 foreground_probe{}; // 1 explicit eye, 2 multiview
    bool tess_emulated_primitive{};
    bool emulate_depth_negative_one_to_one{};
    bool clip_disable{};
    u32 step_rate_0;
    u32 step_rate_1;
    /// UCP_ENA bits from PA_CL_CLIP_CNTL, lowered to clip distances in the shader.
    u32 user_clip_plane_mask{};
    // Outside the six guest plane bits. Keeps the same on-disk runtime-info layout.
    static constexpr u32 BladeMouthMask = 1u << 31;
    // Honed steel: HoneMask | the shader revision << 8 | the part (core/vr/honed_steel.h).
    static constexpr u32 HoneMask = 1u << 30;
    bool HonesBlade() const { return (user_clip_plane_mask >> 30) == 1u; }
    bool ClipsBlade() const { return user_clip_plane_mask == BladeMouthMask; }

    bool operator<=>(const VertexRuntimeInfo& other) const noexcept = default;
};

struct HullRuntimeInfo : protected CommonHsEsVsRuntimeInfo {
    u32 num_input_control_points;
    u32 num_threads;
    AmdGpu::TessellationType tess_type;
    bool offchip_lds_enable;
    u32 ls_stride;
    u32 hs_output_base;

    bool operator==(const HullRuntimeInfo&) const = default;

    // It might be possible for a non-passthrough TCS to have these conditions, in some dumb
    // situation. In that case, it should be fine to assume passthrough and declare some extra
    // output control points and attributes that shouldnt be read by the TES anyways
    bool IsPassthrough() const {
        return hs_output_base == 0 && ls_stride == hs_output_cp_stride && num_threads == 1;
    };

    // regs.ls_hs_config.hs_output_control_points contains the number of threads, which
    // isn't exactly the number of output control points.
    // For passthrough shaders, the register field is set to 1, so use the number of
    // input control points
    u32 NumOutputControlPoints() const {
        return IsPassthrough() ? num_input_control_points : num_threads;
    }
};

static constexpr auto GsMaxOutputStreams = 4u;
using GsOutputPrimTypes = std::array<AmdGpu::GsOutputPrimitiveType, GsMaxOutputStreams>;
struct GeometryRuntimeInfo {
    u32 num_outputs;
    std::array<OutputMap, 3> outputs;
    u32 num_invocations{};
    u32 output_vertices{};
    u32 in_vertex_data_size{};
    u32 out_vertex_data_size{};
    AmdGpu::PrimitiveType in_primitive;
    GsOutputPrimTypes out_primitive;
    AmdGpu::GsScenario mode;
    std::span<const u32> vs_copy;
    u64 vs_copy_hash;

    bool operator==(const GeometryRuntimeInfo& other) const {
        return num_outputs == other.num_outputs && outputs == other.outputs && num_invocations &&
               other.num_invocations && output_vertices == other.output_vertices &&
               in_primitive == other.in_primitive &&
               std::ranges::equal(out_primitive, other.out_primitive) &&
               vs_copy_hash == other.vs_copy_hash;
    }
};

enum class MrtSwizzle : u8 {
    Identity = 0,
    Alt = 1,
    Reverse = 2,
    ReverseAlt = 3,
};
static constexpr u32 MaxColorBuffers = 8;

struct PsColorBuffer {
    AmdGpu::DataFormat data_format : 6;
    AmdGpu::NumberFormat num_format : 4;
    AmdGpu::NumberConversion num_conversion : 3;
    AmdGpu::ShaderExportFormat export_format : 4;
    AmdGpu::CompMapping swizzle;

    bool operator==(const PsColorBuffer& other) const = default;
};

struct FragmentRuntimeInfo {
    struct PsInput {
        u8 param_index;
        bool is_default;
        bool is_flat;
        u8 default_value;

        bool IsDefault() const {
            return is_default && !is_flat;
        }

        bool operator==(const PsInput&) const noexcept = default;
    };
    AmdGpu::PsInput en_flags;
    AmdGpu::PsInput addr_flags;
    u32 num_inputs;
    std::array<PsInput, 32> inputs;
    std::array<PsColorBuffer, MaxColorBuffers> color_buffers;
    AmdGpu::ShaderExportFormat z_export_format;
    u8 mrtz_mask{};
    bool dual_source_blending{false};
    bool clip_distance_emulation{false};
    // VR body cutaway (Bloodborne VR, vk_rasterizer_vr_draws.cpp): 0 = off; otherwise the
    // fragments nearer to the camera than near_mm (low 16 bits) are discarded and those between
    // near_mm and far_mm (bits 16..30) fade out with a screen-space dither (view depth =
    // 1 / gl_FragCoord.w, in millimetres of game units); bit 31: back faces are cut at any
    // depth. Only the hunter's own body draws in first-person VR use such a permutation.
    // VrPushData (vr_push_data.h): the draw pushes its radii and this is only 1, so one cut
    // permutation serves every radius, boost level and world scale.
    u32 vr_cut{0};
    bool HonesBlade() const { return (vr_cut & 0xfffcu) == 0xfff0u; }
    bool CutsFragments() const { return vr_cut != 0 && !HonesBlade(); }

    // The recompiler reads color_buffers[i] only for the targets the program exports
    // (Info::mrt_mask, fixed by its code; with dual source blending MRT1's export converts with
    // target 0's entry): the others are cleared. Of a written target it reads the export format,
    // the number conversion, the swizzle through CompMapping::Map (Zero and One keep their own
    // component) and the number format's class (the output's type); the data format and the exact
    // number format only for the unorm fix-up (`unorm_fixup`: Profile::needs_unorm_fixup). So two
    // permutations never differ in what their SPIR-V does not read (PipelineCache::GetProgram),
    // e.g. a written target going from Format10_11_11 to Format16_16_16_16 (float, alpha Zero
    // then Alpha).
    void KeepWrittenColorBuffers(u32 mrt_mask, bool unorm_fixup) noexcept {
        if (dual_source_blending && (mrt_mask & 2u) != 0) {
            mrt_mask |= 1u;
        }
        for (u32 i = 0; i < MaxColorBuffers; ++i) {
            auto& cb = color_buffers[i];
            if ((mrt_mask & (1u << i)) == 0) {
                std::memset(&cb, 0, sizeof(PsColorBuffer));
                continue;
            }
            for (u32 c = 0; c < 4; ++c) {
                cb.swizzle.array[c] =
                    AmdGpu::CompSwizzle(u32(AmdGpu::CompSwizzle::Red) + cb.swizzle.Map(c));
            }
            if (!unorm_fixup) {
                cb.data_format = AmdGpu::DataFormat::FormatInvalid;
                if (AmdGpu::GetNumberClass(cb.num_format) == AmdGpu::NumberClass::Float) {
                    cb.num_format = AmdGpu::NumberFormat::Float;
                }
            }
        }
    }

    bool operator==(const FragmentRuntimeInfo& other) const noexcept {
        return std::ranges::equal(color_buffers, other.color_buffers) &&
               en_flags == other.en_flags && addr_flags == other.addr_flags &&
               num_inputs == other.num_inputs && z_export_format == other.z_export_format &&
               mrtz_mask == other.mrtz_mask && dual_source_blending == other.dual_source_blending &&
               clip_distance_emulation == other.clip_distance_emulation &&
               vr_cut == other.vr_cut &&
               std::ranges::equal(inputs.begin(), inputs.begin() + num_inputs, other.inputs.begin(),
                                  other.inputs.begin() + num_inputs);
    }
};

struct ComputeRuntimeInfo {
    u32 shared_memory_size;
    std::array<u32, 3> workgroup_size;
    std::array<bool, 3> tgid_enable;

    bool operator==(const ComputeRuntimeInfo& other) const noexcept {
        return workgroup_size == other.workgroup_size && tgid_enable == other.tgid_enable;
    }
};

/**
 * Stores information relevant to shader compilation sourced from liverpool registers.
 * It may potentially differ with the same shader module so must be checked.
 * It's also possible to store any other custom information that needs to be part of shader key.
 */
struct RuntimeInfo {
    VrMotion vr_motion{};
    bool native_stereo;
    bool billboard_face_eye{};
    bool native_multiview;
    // VrPushData (vr_push_data.h): the hardware vertex or geometry stage remaps each eye with the
    // projection the draw pushes, and native_projection stays zero. Otherwise the projection is
    // baked in native_projection (zero: no remap).
    bool native_asymmetric{};
    std::array<std::array<float,4>,2> native_projection{};
    // A light rectangle's fragment stage with a tile of its own: the per-eye tile test, its
    // bounds baked in native_light_bounds (zero with VrPushData: the draw pushes them).
    bool native_light_scissor{};
    // VR light shadows (ir/passes/light_shadow.h): such a light rectangle's fragment stage
    // marches its near lights through the depth (the byte after native_light_scissor: the
    // struct's layout is unchanged, older cache entries read it as off). LightShadow::Level: 0
    // none, 1 the shadows, 2 High (the hero lamps' own march; a byte since 2026-11-17, a bool
    // before, so older entries hold 0 or 1).
    u8 vr_light_shadows{};
    // The game's screen-space shading passes computed by ours (the next padding byte: the layout
    // is unchanged; older cache entries hold 0 or 1, the AO bit): VrAmbientOcclusionBit, its AO
    // pass (ir/passes/ambient_occlusion.h); VrMoonShadows, its moon-shadow mask
    // (ir/passes/moon_shadow.h).
    static constexpr u8 VrAmbientOcclusionBit = 1;
    static constexpr u8 VrMoonShadows = 2;
    // Wet streets (ir/passes/reflections.h): VrReflections, the game's trace computed by ours
    // (VrReflectionsJitter: its jitter moves every frame, with an upscaler); VrReflectionComposite,
    // its composite counting the reflection once.
    static constexpr u8 VrReflections = 4;
    static constexpr u8 VrReflectionsJitter = 8;
    // The same bit on the game's AO programs (never a trace program): VrAoMaskOnly, ours writes the
    // lamp shadows' mask and leaves the game's AO (ambient_occlusion.h MaskOnly).
    static constexpr u8 VrAoMaskOnly = 8;
    static constexpr u8 VrReflectionComposite = 16;
    // The same bit on the game's AO programs (never the composite): VrAoLampShadows, ours does the
    // lamp shadows' work in that pass too (light_shadow.h SceneMaps: the cubes, the mask), only
    // with the lamp shadows on (2026-11-17: the mask is the default since, a session without lamp
    // shadows must not pay for it).
    static constexpr u8 VrAoLampShadows = 16;
    // ... and, with it, the tier that mask was made for (light_shadow.h SceneMaps::EdgeTier: its
    // march's step shares and edge reach are compiled in): VrAoLampLow Low, VrAoLampHigh High,
    // neither Medium. The trace's and the composite's bits (4, 32): their passes and the cache's
    // checks act on them only for those programs (2026-11-21, a code review: a Low mask writer
    // was taken for a Medium one and the other way round).
    static constexpr u8 VrAoLampLow = 4;
    static constexpr u8 VrAoLampHigh = 32;
    // The same bit again on the composite (never a trace or AO program): VrBloodFloors, the
    // reflection seen through blood on glossy floors (reflections.h Blood*). Its own key bit, so
    // turning it on retranslates this one program and no other.
    static constexpr u8 VrBloodFloors = 8;
    // With our AO: the same composite darkens its shine by the specular occlusion
    // (ir/passes/ambient_occlusion.h SpecOcclusion).
    static constexpr u8 VrSpecularOcclusion = 32;
    // The game's light programs and its ambient composite read a gloss widened by the normal's
    // variance (ir/passes/specular_aa.h: less shimmer on shiny details).
    static constexpr u8 VrSpecularAa = 64;
    // The tone map's exposure follows the eye's adaptation (ir/passes/cinematic_look.h).
    static constexpr u8 VrEyeAdaptation = 128;
    u8 vr_ambient_occlusion{};
    // The light composite's VR lighting (the last padding byte before native_light_bounds: the
    // layout is unchanged; older cache entries hold 0 or 1): VrCompositeBounce, one bounce
    // (ir/passes/bounce_light.h); VrCompositeFog, the lights in the air (ir/passes/fog_light.h).
    static constexpr u8 VrCompositeBounce = 1;
    static constexpr u8 VrCompositeFog = 2;
    // VrCompositeShafts, the moonlight in the air (ir/passes/moon_shafts.h).
    static constexpr u8 VrCompositeShafts = 4;
    // Not the light composite's: the game's _Bloody_ G-buffer programs make their blood wet
    // (ir/passes/wet_blood.h; this byte had bits to spare).
    static constexpr u8 VrWetBlood = 8;
    // Not the light composite's either: the game's soft particle programs take the frame's
    // lights (ir/passes/particle_air.h); VrParticleAirPremul, their blend is premultiplied.
    static constexpr u8 VrParticleAir = 16;
    static constexpr u8 VrParticleAirPremul = 32;
    // The game's three parallax programs walk their height maps in finer layers
    // (ir/passes/parallax.h: deeper stones).
    static constexpr u8 VrParallax = 64;
    // A G-buffer program (five or more colour targets) whose bump map's height darkens its gaps
    // (ir/passes/parallax.h BumpGaps).
    static constexpr u8 VrBumpGaps = 128;
    // Not the light composite's either: the shadow mask's blur and the scene's copy are the air
    // pass's two hosts (ir/passes/air_light.h); on those two programs only.
    static constexpr u8 VrAirPass = 1;
    // On the air pass's three programs only (never a parallax program, whose bit this is): the
    // mist comes from the fog grid (ir/passes/fog_grid.h). Its own key bit: a permutation made
    // with the grid is never taken for one made without.
    static constexpr u8 VrFogGrid = 64;
    u8 vr_composite_light{};
    std::array<std::array<float,4>,2> native_light_bounds{};
    Stage stage;
    u32 num_user_data;
    u32 num_input_vgprs;
    u32 num_allocated_vgprs;
    AmdGpu::FpDenormMode fp_denorm_mode32;
    AmdGpu::FpDenormMode fp_denorm_mode16_64;
    AmdGpu::FpRoundMode fp_round_mode32;
    AmdGpu::FpRoundMode fp_round_mode16_64;
    union {
        LocalRuntimeInfo ls_info;
        ExportRuntimeInfo es_info;
        VertexRuntimeInfo vs_info;
        HullRuntimeInfo hs_info;
        GeometryRuntimeInfo gs_info;
        FragmentRuntimeInfo fs_info;
        ComputeRuntimeInfo cs_info;
        // Hs/Es/VsRuntimeInfo inherit from these so we can
        // access common info with correct offsets
        CommonHsEsVsRuntimeInfo hs_es_vs_info;
        CommonEsVsRuntimeInfo es_vs_info;
    };

    void Initialize(Stage stage_) {
        memset(this, 0, sizeof(*this));
        stage = stage_;
    }

    bool operator==(const RuntimeInfo& other) const noexcept {
        if (billboard_face_eye != other.billboard_face_eye) return false;
        if (vr_motion != other.vr_motion) {
            return false;
        }
        if (native_stereo != other.native_stereo || native_multiview != other.native_multiview || native_asymmetric != other.native_asymmetric || native_projection != other.native_projection) return false;
        if (native_light_scissor != other.native_light_scissor || native_light_bounds != other.native_light_bounds) return false;
        if (vr_light_shadows != other.vr_light_shadows) return false;
        if (vr_ambient_occlusion != other.vr_ambient_occlusion) return false;
        if (vr_composite_light != other.vr_composite_light) return false;
        switch (stage) {
        case Stage::Fragment:
            return fs_info == other.fs_info;
        case Stage::Vertex:
            return vs_info == other.vs_info;
        case Stage::Compute:
            return cs_info == other.cs_info;
        case Stage::Export:
            return es_info == other.es_info;
        case Stage::Geometry:
            return gs_info == other.gs_info;
        case Stage::Hull:
            return hs_info == other.hs_info;
        case Stage::Local:
            return ls_info == other.ls_info;
        default:
            return true;
        }
    }

    void InitFromTessConstants(Shader::TessellationDataConstantBuffer& tess_constants) {
        hs_es_vs_info.hs_output_cp_stride = tess_constants.hs_cp_stride;
        if (stage == Stage::Hull) {
            hs_info.ls_stride = tess_constants.ls_stride;
            hs_info.hs_output_base = tess_constants.hs_output_base;
        }
    }
};

} // namespace Shader

template <>
struct fmt::formatter<Shader::Stage> {
    constexpr auto parse(format_parse_context& ctx) {
        return ctx.begin();
    }
    auto format(const Shader::Stage stage, format_context& ctx) const {
        constexpr static std::array names = {"fs", "vs", "gs", "es", "hs", "ls", "cs"};
        return fmt::format_to(ctx.out(), "{}", names[static_cast<size_t>(stage)]);
    }
};
