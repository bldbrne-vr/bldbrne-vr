// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <unordered_map>
#include <sirit/sirit.h>

#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/value.h"
#include "shader_recompiler/profile.h"

namespace Shader::Backend::SPIRV {

using Sirit::Id;

struct VectorIds {
    [[nodiscard]] Id& operator[](u32 index) {
        return ids[index - 1];
    }

    [[nodiscard]] const Id& operator[](u32 index) const {
        return ids[index - 1];
    }

    [[nodiscard]] Id& Get(u32 index) {
        return ids[index - 1];
    }

    [[nodiscard]] const Id& Get(u32 index) const {
        return ids[index - 1];
    }

    std::array<Id, 4> ids;
};

class EmitContext final : public Sirit::Module {
public:
    // `merged_eyes` (native stereo compute only, vr_merged_eyes.h): both eyes in one launch of
    // twice the groups in z; the eye is the group's z (vr_hud is not read), the game's z is 0.
    // Never part of a cached shader: the variant is built in memory.
    explicit EmitContext(const Profile& profile, const RuntimeInfo& runtime_info, Info& info,
                         Bindings& binding, bool merged_eyes = false);
    ~EmitContext();

    Id Def(const IR::Value& value);

    void DefineBufferProperties();
    void DefineAmdPerVertexAttribs();
    void DefineWorkgroupIndex();

    [[nodiscard]] Id DefineInput(Id type, std::optional<u32> location = std::nullopt,
                                 std::optional<spv::BuiltIn> builtin = std::nullopt) {
        const Id input_id{DefineVariable(type, builtin, spv::StorageClass::Input)};
        if (location) {
            Decorate(input_id, spv::Decoration::Location, *location);
        }
        return input_id;
    }

    [[nodiscard]] Id DefineOutput(Id type, std::optional<u32> location = std::nullopt,
                                  std::optional<spv::BuiltIn> builtin = std::nullopt) {
        const Id output_id{DefineVariable(type, builtin, spv::StorageClass::Output)};
        if (location) {
            Decorate(output_id, spv::Decoration::Location, *location);
            if (l_stage == LogicalStage::Vertex && stage == Stage::Vertex) {
                ASSERT(*location < 64);
                info.vertex_output_locations |= u64{1} << *location;
            }
        }
        return output_id;
    }

    [[nodiscard]] Id DefineUniformConst(Id type, u32 set, u32 binding, bool readonly = false) {
        const Id uniform_id{DefineVar(type, spv::StorageClass::UniformConstant)};
        Decorate(uniform_id, spv::Decoration::DescriptorSet, set);
        Decorate(uniform_id, spv::Decoration::Binding, binding);
        if (readonly) {
            Decorate(uniform_id, spv::Decoration::NonWritable);
        }
        return uniform_id;
    }

    template <bool global = true>
    [[nodiscard]] Id DefineVar(Id type, spv::StorageClass storage_class,
                               std::optional<Id> initializer = std::nullopt) {
        const Id pointer_type_id{TypePointer(storage_class, type)};
        return global ? AddGlobalVariable(pointer_type_id, storage_class, initializer)
                      : AddLocalVariable(pointer_type_id, storage_class, initializer);
    }

    [[nodiscard]] Id DefineVariable(Id type, std::optional<spv::BuiltIn> builtin,
                                    spv::StorageClass storage_class,
                                    std::optional<Id> initializer = std::nullopt) {
        const Id id{DefineVar(type, storage_class, initializer)};
        if (builtin) {
            Decorate(id, spv::Decoration::BuiltIn, *builtin);
        }
        interfaces.push_back(id);
        return id;
    }

    [[nodiscard]] Id ConstU32(u32 value) {
        return Constant(U32[1], value);
    }

    template <typename... Args>
    [[nodiscard]] Id ConstU32(Args&&... values) {
        constexpr u32 size = static_cast<u32>(sizeof...(values));
        static_assert(size >= 2);
        const std::array constituents{Constant(U32[1], values)...};
        const Id type = size <= 4 ? U32[size] : TypeArray(U32[1], ConstU32(size));
        return ConstantComposite(type, constituents);
    }

    [[nodiscard]] Id ConstS32(s32 value) {
        return Constant(S32[1], value);
    }

    template <typename... Args>
    [[nodiscard]] Id ConstS32(Args&&... values) {
        constexpr u32 size = static_cast<u32>(sizeof...(values));
        static_assert(size >= 2);
        const std::array constituents{Constant(S32[1], values)...};
        const Id type = size <= 4 ? S32[size] : TypeArray(S32[1], ConstU32(size));
        return ConstantComposite(type, constituents);
    }

    [[nodiscard]] Id ConstF32(f32 value) {
        return Constant(F32[1], value);
    }

    template <typename... Args>
    [[nodiscard]] Id ConstF32(Args... values) {
        constexpr u32 size = static_cast<u32>(sizeof...(values));
        static_assert(size >= 2);
        const std::array constituents{Constant(F32[1], values)...};
        const Id type = size <= 4 ? F32[size] : TypeArray(F32[1], ConstU32(size));
        return ConstantComposite(type, constituents);
    }

    inline Id AddLabel() {
        last_label = Module::AddLabel();
        return last_label;
    }

    inline Id AddLabel(Id label) {
        last_label = Module::AddLabel(label);
        return last_label;
    }

    Id EmitDwordMemoryRead(Id address, auto&& fallback) {
        const Id available_label = OpLabel();
        const Id fallback_label = OpLabel();
        const Id merge_label = OpLabel();

        const Id addr = OpFunctionCall(U64, get_bda_pointer, address);
        const Id is_available = OpINotEqual(U1[1], addr, u64_zero_value);
        OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
        OpBranchConditional(is_available, available_label, fallback_label);

        // Available
        AddLabel(available_label);
        const Id addr_ptr = OpConvertUToPtr(physical_pointer_type_u32, addr);
        const Id result = OpLoad(U32[1], addr_ptr, spv::MemoryAccessMask::Aligned, 4u);
        OpBranch(merge_label);

        // Fallback
        AddLabel(fallback_label);
        const Id fallback_result = fallback();
        OpBranch(merge_label);

        // Merge
        AddLabel(merge_label);
        const Id final_result =
            OpPhi(U32[1], fallback_result, fallback_label, result, available_label);
        return final_result;
    }

    Id EmitSharedMemoryAccess(const Id result_type, const Id shared_mem, const Id index) {
        if (std::popcount(static_cast<u32>(info.shared_types)) > 1) {
            return OpAccessChain(result_type, shared_mem, u32_zero_value, index);
        }
        // Workgroup layout struct omitted.
        return OpAccessChain(result_type, shared_mem, index);
    }

    Id EmitFlatbufferLoad(Id flatbuf_offset) {
        const auto& flatbuf_buffer{buffers[flatbuf_index]};
        ASSERT(flatbuf_buffer.binding >= 0 && flatbuf_buffer.buffer_type == BufferType::Flatbuf);
        const auto [flatbuf_buffer_id, flatbuf_pointer_type] =
            flatbuf_buffer.aliases[u32(PointerType::U32)];
        const auto ptr{
            NativeBufferPointer(flatbuf_pointer_type, flatbuf_buffer_id, u32_zero_value, flatbuf_offset)};
        return OpLoad(U32[1], ptr);
    }

    Info& info;
    const RuntimeInfo& runtime_info;
    const Profile& profile;
    Stage stage;
    LogicalStage l_stage{};

    Id last_label{};
    // VR light shadows (emit_spirv_light_shadow.cpp): the shader's one-time setup, made at its
    // first VrLightVisibility (all of them are in the entry block).
    struct VrLightShadowState {
        bool ready{};
        std::array<Id, 4> rows{}; // the ndc-to-view matrix's rows
        bool rows_given{};        // ... set by the caller (the air pass: its own camera), not read
                                  // from Info::vr_light_shadow's matrix buffer
        std::array<Id, 4> inv{};  // its inverse's rows (view to clip)
        bool has_inv{};           // inv is made (Setup's inverse, or the shadowed air's)
        Id a{}, b{};              // coordinates = a * ndc + b
        Id ok{};                  // a and b are defined
        Id p{};                   // the pixel's view position
        Id normal{};              // its surface's normal (depth neighbours, facing the eye)
        Id normal_ok{};           // ... trusted: no depth edge across the pixel
        Id seed{};                // per-pixel, per-frame jitter in [0, 1)
        Id seed_still{};          // the pixel's noise without the frame's turn (the AO's)
        Id frame_turn{};          // per-frame only, in [0, 1) (the golden ratio's steps; 0 with white noise)
        Id ball{};                // a per-pixel, per-frame point of the unit cube [-1, 1]^3
        Id pixel{};               // a pixel's size in coordinates (vec2)
        Id vis{}, occl{}, step{}; // Private variables of the branch and the march
        Id mask{};                // the AO's sector bits (u32)
        Id gi{};                  // the bounce light's sum (vec3)
        // The other eye (light_shadow.h CrossEye): made once, with Setup.
        bool other_ready{};
        Id this_eye{}, other_eye{};      // eye indices (u32)
        std::array<Id, 4> other_inv{};   // its view-to-clip rows
        Id other_row2{}, other_row3{};   // its ndc-to-view matrix's z and w rows
        Id other_differs{};              // its matrix is not this eye's (bool)
        // The blocker grid (light_shadow.h Grid): made once with the setup.
        bool grid_ready{};
        u32 grid_buffer{~0u};            // ctx.buffers index (~0u: none)
        Id grid_on{};                    // bool
        Id grid_stamp{}, grid_life{};    // u32
        Id grid_inv_voxel{};             // 1 / the voxel's side (game units)
        Id grid_shift{};                 // this eye's view to the flat view (vec3)
        std::array<Id, 3> grid_rows{};   // the flat view to the world (vec4 rows)
        Id grid_hit{};                   // Private float: the march's grid hit
        Id grid_near{};                  // the eye distance it holds from (view units)
        Id grid_reach{};                 // ... and to
        Id grid_thick{};                 // the blockers' thickness (view units; 0: ThickM)
        // The lights the game keeps on the camera (light_shadow.h AttachedFromM).
        bool attached_ready{};
        Id attached_count{};             // u32
        Id attached_shift{};             // this eye's view to the flat view (vec3)
        std::array<Id, 4> attached{};    // their places and reach (flat view; vec4: x y z k)
        Id attached_remove{};            // they light nothing (bool)
        Id probe_mask{};                 // tests: the light classes casting nothing (u32)
        Id probe_sel{};                  // tests: the light classes moved for their shadows (u32)
        Id probe_nudge{};                // tests: that move, in the eye's view (vec3)
        // The scene maps (light_shadow.h SceneMaps): slots used, each slot's lamp (flat view x y
        // z, reach k), this eye's shift to the flat view (vec3), the maps' buffer.
        bool loop_ready{};               // the lights' loop emitted (light_shadow.h LoopLights)
        Id loop_vis{};                   // its results: a private array, one per light
        Id loop_vis_ptr{};               // a pointer to one of them
        u32 loop_stride{};
        bool scene_ready{};
        u32 scene_buffer{};
        Id scene_count{};
        u32 scene_body{};                // the body buffer (the slots: read one at a time)
        Id scene_half{};                 // this frame's half of the cubes (u32 0 or 1)
        Id scene_capacity{};             // the mask's pixels (u32)
        std::array<Id, 3> scene_rot{};   // the flat view's rows into the world's axes (f32x3)
        bool mask_ready{};
        bool hero_ready{};               // High's setups made (light_shadow.h High: HeroSetup)
        bool hero_loop_ready{};          // High's heroes' loop emitted (HeroLoop)
        Id hero_vis{};                   // its results: a light's march, or -1 (no hero)
        Id hero_vis_ptr{};
        u32 hero_count{};                // its lights (at most SceneMaps::DrawLights)
        u32 mask_buffer{};
        u32 pyr_buffer{~0u};             // the depth pyramid (the AO pass's march; ~0u: none)
        Id pyr_scale{};                  // its step scale this pixel (1 without a pyramid)
        std::array<Id, 3> mask_word{};   // this pixel's mask (the lamp shaders)
        std::array<std::array<Id, 3>, 4> mask_corner{}; // the 4 half-res neighbours' words
        Id mask_fx{}, mask_fy{};         // their bilinear weights
        std::array<Id, 4> mask_w{};      // ... times their depth's nearness to this pixel's (normalized)
        Id mask_min{};                   // tests (MaskShow): the darkest slot here, the AO pass
        // The mask's edges marched again (light_shadow.h SceneMaps::Edges): what the lamp read's
        // one-time part leaves for each light's march (all unset with the edges off).
        Id edge_pv{};                    // the pixel's view place (vec3)
        Id edge_own{};                   // ... and its distance
        Id edge_w{};                     // the matrix's w there
        Id edge_ndc{};                   // the pixel's ndc (vec2)
        std::array<Id, 4> edge_rows{};   // the ndc-to-view matrix's rows (vec4)
        Id edge_a{}, edge_b{};           // coordinates = a * ndc + b (vec2)
        Id edge_pixel{};                 // a pixel's size in coordinates (vec2)
        Id edge_ok{};                    // a and b are defined (bool)
        Id edge_seen{};                  // invocation-local bits for slots already traced in this draw
        Id edge_visibility{};            // per-invocation results of the multi-light loop
        Id edge_slots{};                 // frame-wide CPU selection, shared between eyes
        Id edge_set{};                   // ... the same bits for the mask's march (PlainStepShare)
        Id edge_code{};                  // the edge pass's byte at this pixel (light_shadow.h EdgePass)
        Id edge_packed{};                // ... with the corner set: bits 0..3 the corners, 4..11 the byte
        Id edge_near{};                  // ... and the pixel's nearness (EdgeFar)
        std::array<Id, 4> edge_corner{}; // neighbours that bound each lamp's edge on this pixel
        Id scene_shift{};
        Id scene_voxel{};                // the grid's voxel (its entries' bias), f32
        // This shader's earlier lights, for grouping small ones (light_shadow.h GroupM): place
        // (this eye's view), small, marched (bools), visibility after the march.
        struct Grouped {
            Id centre;
            Id small;
            Id marched;
            Id vis;
        };
        std::vector<Grouped> grouped;
    } vr_light_shadow{};

    Id void_id{};
    Id U8{};
    Id S8{};
    Id U16{};
    Id S16{};
    Id U64{};
    VectorIds F16{};
    VectorIds F32{};
    VectorIds F64{};
    VectorIds S32{};
    VectorIds U32{};
    VectorIds U1{};

    Id full_result_i32x2;
    Id full_result_u32x2;
    Id frexp_result_f32;
    Id frexp_result_f64;

    Id pi_x2;

    Id true_value{};
    Id false_value{};
    Id u8_one_value{};
    Id u8_zero_value{};
    Id u16_zero_value{};
    Id u32_one_value{};
    Id u32_zero_value{};
    Id f32_zero_value{};
    Id u64_one_value{};
    Id u64_zero_value{};

    Id shared_u16{};
    Id shared_u32{};
    Id shared_u64{};

    Id input_u32{};
    Id input_f32{};
    Id input_s32{};
    Id output_u32{};
    Id output_f32{};
    Id output_s32{};

    Id gl_in{};

    boost::container::small_vector<Id, 16> interfaces;

    Id output_position{};
    Id output_point_size{};
    Id output_layer{};
    Id output_viewport_index{};
    Id primitive_id{};
    Id motion_out_cur{}, motion_out_prev{}, motion_in_cur{}, motion_in_prev{}, motion_frag_out{};
    // The motion buffers' addresses: pushed per draw with VrPushData (vr_motion is only its
    // flag), otherwise both baked into this permutation.
    // Native stereo's multiview passes, or the DIBR centre's single view (its upscaler,
    // renderer_vulkan/vr_dibr_upscale.h: baked addresses, no VrPushData).
    bool VertexMotion() const {
        // A body-map capture (VrMotion::Capture) is the moon's single-view shadow pass's.
        return stage == Stage::Vertex && l_stage == LogicalStage::Vertex &&
               (!runtime_info.native_stereo || runtime_info.native_multiview ||
                runtime_info.vr_motion.Capture()) &&
               !runtime_info.vs_info.clip_disable && runtime_info.vr_motion.enabled &&
               (UsesVrPushData() ||
                (runtime_info.vr_motion.params_address && runtime_info.vr_motion.positions_address));
    }
    bool FragmentMotion() const {
        return l_stage == LogicalStage::Fragment &&
               (!runtime_info.native_stereo || runtime_info.native_multiview) &&
               runtime_info.vr_motion.enabled && !runtime_info.vr_motion.Capture();
    }
    struct NativeScreenExport {
        u32 param{}, component{}, axis{};
        float slope{};
        bool normalized{};
        IR::Value dynamic_scale; // optional uniform coefficient of the actual clip position
    };
    std::vector<NativeScreenExport> native_screen_exports;
    bool native_camera_position = false;
    bool native_projection_position = false;
    Id NativeEyeIndex();
    // While valid, NativeEyeIndex (and so every per-eye buffer and image access) is this u32
    // instead of the stage's own eye: the light shadows read the other eye's depth and matrices
    // (emit_spirv_light_shadow.cpp, light_shadow.h CrossEye). Set and cleared around those reads.
    Id eye_override{};
    // Layer draws (Profile::native_layer_draws): a both-eye graphics stage takes its eye from
    // vr_hud's sign bit (multiview: ViewIndex).
    bool NativeLayerDraws() const {
        return runtime_info.native_multiview && profile.native_layer_draws;
    }
    // ...and the last vertex stage (the hardware VS stage: the vertex or evaluation program, or
    // the geometry program) writes gl_Layer, the eye's layer of the two-layer attachments.
    bool NativeLayerOutput() const {
        return NativeLayerDraws() && (stage == Stage::Vertex || l_stage == LogicalStage::Geometry);
    }
    // vr_hud as a graphics stage reads its flags: without a layer draw's eye bit.
    Id NativeHud(Id hud);
    Id NativeBufferPointer(Id type, Id buffer, Id member, Id index);
    Id NativeImagePointer(Id type, Id image, Id lod = {});
    Id NativeNonUniform(Id resource);
    // VR_EXP_EYE_INDEX (timing experiment): the descriptor eye index of a graphics stage.
    bool ExpEyeGraphics() const;
    Id DescriptorEyeIndex();
    // VR_EYE_SPEC: the descriptor (`eye`, or 0 where the pipeline is specialized with the binding
    // shared) and the element index (`index`, moved by the eye's stride where it has one).
    std::pair<Id, Id> EyeSpecAccess(Id buffer, Id eye, Id index);
    // VrPushData (Profile::SupportsVrPushData): a native stereo graphics stage declares the whole
    // push block and reads the draw's projection, light tiles, cut and motion buffers from it at
    // run time.
    bool UsesVrPushData() const {
        return runtime_info.native_stereo && l_stage != LogicalStage::Compute &&
               profile.SupportsVrPushData();
    }
    // A float of the push block: member `index`, or `component` of that vector member.
    Id PushF32(u32 index, std::optional<u32> component = std::nullopt);
    // This eye's `component` of a VrPushData pair (the left eye's member at `index`, the right
    // eye's at index + 1), selected as the baked constants were: OpSelect(left, left's, right's).
    Id PushEyeF32(Id left, u32 index, u32 component);
    Id probe_view_index{};
    Id vertex_index{};
    Id instance_id{};
    Id push_data_block{};
    Id base_vertex{};
    Id frag_coord{};
    Id shading_rate{}; // ShadingRateKHR (the lamp shadows' AO pass, with attachment rates)
    Id front_facing{};
    Id frag_depth{};
    Id stencil_ref{};
    Id sample_mask{};
    Id sample_index{};
    Id clip_distances{};
    Id blade_mouth_distance{};
    Id hone_span{};
    Id hone_polish{}; // fragment prologue value shared by gloss and the three reflectance channels
    Id cull_distances{};

    Id patch_vertices{};
    Id output_tess_level_outer{};
    Id output_tess_level_inner{};
    Id tess_coord;
    std::array<Id, 30> patches{};

    Id workgroup_id{};
    bool merged_eyes{};
    Id num_workgroups_id{};
    Id workgroup_index_id{};
    Id local_invocation_id{};
    Id invocation_id{};
    Id subgroup_local_invocation_id{};
    Id image_u32{};
    Id image_f32{};

    Id shared_memory_u16{};
    Id shared_memory_u32{};
    Id shared_memory_u64{};

    Id shared_memory_u16_type{};
    Id shared_memory_u32_type{};
    Id shared_memory_u64_type{};

    Id bary_coord{};
    Id bary_coord_smooth{};
    Id bary_coord_smooth_centroid{};
    Id bary_coord_smooth_sample{};
    Id bary_coord_nopersp{};
    Id bary_coord_nopersp_sample{};

    struct TextureDefinition {
        const VectorIds* data_types;
        Id id;
        Id sampled_type;
        Id image_type;
        AmdGpu::ImageType view_type;
        bool is_integer = false;
        bool is_storage = false;
        MipStorageFallbackMode mip_fallback_mode{};
    };

    enum class PointerType : u32 {
        U8,
        U16,
        U32,
        F32,
        U64,
        F64,
        NumAlias,
    };

    enum class PointerSize : u32 {
        B8,
        B16,
        B32,
        B64,
        NumClass,
    };

    struct BufferSpv {
        Id id;
        Id pointer_type;
    };

    struct BufferDefinition {
        u32 binding;
        BufferType buffer_type;
        std::array<Id, u32(PointerSize::NumClass)> offsets;
        std::array<BufferSpv, u32(PointerType::NumAlias)> aliases;

        template <class Self>
        auto& Alias(this Self& self, PointerType alias) {
            return self.aliases[u32(alias)];
        }

        template <class Self>
        auto& Offset(this Self& self, PointerSize size) {
            return self.offsets[u32(size)];
        }
    };

    Bindings& binding;
    boost::container::small_vector<Id, 16> buf_type_ids;
    boost::container::small_vector<BufferDefinition, 16> buffers;
    // VR_EXP_EYE_INDEX=c (timing experiment): the variables of buffers of a size the eyes pair.
    boost::container::small_vector<Id, 8> exp_pair_buffers;
    // VR_EYE_SPEC (shader_recompiler/eye_spec.h): a graphics stage's buffer bindings' markers,
    // how many compares read each, and the variables of each binding's aliases (by id).
    struct EyeSpecMarker {
        u32 binding;
        Id marker;
        u32 uses;
        Id delta{}; // read-only bindings: the stride and size markers (else invalid)
        Id size{};
        u32 delta_uses{};
        u32 size_uses{};
    };
    struct EyeSpecVar {
        u32 var;          // an alias variable's id
        u32 marker_index; // in eye_spec_markers
        u32 shift;        // its element size, log2 bytes
    };
    boost::container::small_vector<EyeSpecMarker, 16> eye_spec_markers;
    boost::container::small_vector<EyeSpecVar, 32> eye_spec_vars;
    boost::container::small_vector<TextureDefinition, 8> images;
    boost::container::small_vector<Id, 4> samplers;
    std::unordered_map<u32, Id> first_to_last_label_map;

    size_t flatbuf_index{};
    size_t bda_pagetable_index{};
    size_t fault_buffer_index{};
    Id physical_pointer_type_u32;

    Id sampler_type{};
    Id sampler_pointer_type{};

    struct SpirvAttribute {
        union {
            Id id;
            std::array<Id, 3> id_array;
        };
        Id pointer_type;
        Id component_type;
        u32 num_components;
        bool is_integer{};
        bool is_loaded{};
        bool is_array{};
    };
    Id input_attr_array;
    Id output_attr_array;
    std::array<SpirvAttribute, IR::NumParams> input_params{};
    std::array<SpirvAttribute, IR::NumParams> output_params{};
    std::array<SpirvAttribute, IR::NumRenderTargets> frag_outputs{};

    Id uf11_to_f32{};
    Id f32_to_uf11{};
    Id uf10_to_f32{};
    Id f32_to_uf10{};

    Id get_bda_pointer{};

    Id read_const{};
    Id read_const_dynamic{};

private:
    void DefineArithmeticTypes();
    void DefineInterfaces();
    void DefineInputs();
    void DefineVertexBlock();
    void DefineOutputs();
    void DefinePushDataBlock();
    void DefineBuffers();
    void DefineImagesAndSamplers();
    void DefineSharedMemory();
    void DefineFunctions();

    SpirvAttribute GetAttributeInfo(AmdGpu::NumberFormat fmt, Id id, u32 num_components,
                                    bool output, bool loaded = false, bool array = false);

    BufferSpv DefineBuffer(bool is_written, u32 elem_shift, BufferType buffer_type, Id data_type);

    Id DefineFloat32ToUfloatM5(u32 mantissa_bits, std::string_view name);
    Id DefineUfloatM5ToFloat32(u32 mantissa_bits, std::string_view name);

    Id DefineGetBdaPointer();

    Id DefineReadConst(bool dynamic);

    Id GetBufferSize(u32 sharp_idx);
};

} // namespace Shader::Backend::SPIRV
