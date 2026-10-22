// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <fmt/format.h>

#include "common/types.h"
#include "shader_recompiler/specialization.h"
#include "video_core/renderer_vulkan/vk_graphics_pipeline.h"

// Why a shader or a pipeline compiled: the 'why:' tail of the 'Compiling ...' log lines and the
// 'FAST compile reasons:' counters (vk_pipeline_cache.cpp, vk_pipeline_async.cpp). Worked out
// only for a compile (a selection that finds its permutation or pipeline never comes here), only
// when the line is logged (nothing in a play build). The pure parts are here, so the unit tests
// (tests/test_compile_reason.cpp) cover them.
//
// A shader: a new program, or a new permutation of one, named by the fields it differs in from
// the program's closest permutation (the fewest differing), as StageSpecialization's comparison
// sees them. A pipeline: a new permutation (a stage module translated this session that no
// pipeline used yet), existing modules with a new key, named by the key fields it differs in from
// the closest key with the same vertex module (the same stage modules first; the latest 64 of
// them), or the same key again; inside its VR variant's tag when it is one. Either one also says
// when the disk cache had it but the warm-up did not load it (a stale stage after a revision
// bump, the other mode's, ...): a new area and the first session after a bump look alike else.
namespace Vulkan::CompileReason {

// The names of what differs, comma-separated; `count` keeps counting past the ones shown.
struct Names {
    static constexpr u32 MaxShown = 12;
    std::string text;
    u32 count{};

    void Add(std::string_view name) {
        if (count++ < MaxShown) {
            Separate();
            text += name;
        }
    }
    void Add(std::string_view name, size_t index) {
        if (count++ < MaxShown) {
            Separate();
            fmt::format_to(std::back_inserter(text), "{}[{}]", name, index);
        }
    }
    std::string Text() const {
        return count > MaxShown ? fmt::format("{} (+{} more)", text, count - MaxShown) : text;
    }

private:
    void Separate() {
        if (!text.empty()) {
            text += ", ";
        }
    }
};

// A field compared by value (a bitfield too: passed by copy).
template <typename T>
void Field(Names& out, std::string_view name, T a, T b) {
    if (!(a == b)) {
        out.Add(name);
    }
}

// A field without a comparison of its own (a register union): its bytes.
template <typename T>
void Bytes(Names& out, std::string_view name, size_t index, const T& a, const T& b) {
    if (std::memcmp(&a, &b, sizeof(T)) != 0) {
        out.Add(name, index);
    }
}

constexpr std::string_view LogicalStageName(u32 stage) {
    constexpr std::array<std::string_view, 6> names{"fs", "tcs", "tes", "vs", "gs", "cs"};
    return stage < names.size() ? names[stage] : "?";
}

// The RuntimeInfo fields RuntimeInfo::operator== compares that differ: the VR fields, then the
// stage's own (fs.color_buffers[2], vs.step_rate_0, ...).
inline void DiffRuntime(const Shader::RuntimeInfo& a, const Shader::RuntimeInfo& b, Names& out) {
    if (a.vr_motion.enabled != b.vr_motion.enabled) {
        out.Add("vr_motion");
    } else if (!(a.vr_motion == b.vr_motion)) {
        out.Add("vr_motion.addresses"); // baked (no VrPushData): the buffers were allocated anew
    }
    Field(out, "native_stereo", a.native_stereo, b.native_stereo);
    Field(out, "native_multiview", a.native_multiview, b.native_multiview);
    Field(out, "native_asymmetric", a.native_asymmetric, b.native_asymmetric);
    if (a.native_projection != b.native_projection) {
        out.Add("native_projection");
    }
    Field(out, "native_light_scissor", a.native_light_scissor, b.native_light_scissor);
    if (a.native_light_bounds != b.native_light_bounds) {
        out.Add("native_light_bounds");
    }
    using Shader::Stage;
    // The tessellation fields the stages share (protected bases, reached through the union).
    const auto common = [&](std::string_view cp_stride, std::string_view type,
                            std::string_view topology, std::string_view partitioning) {
        Field(out, cp_stride, a.hs_es_vs_info.hs_output_cp_stride,
              b.hs_es_vs_info.hs_output_cp_stride);
        Field(out, type, a.es_vs_info.tess_type, b.es_vs_info.tess_type);
        Field(out, topology, a.es_vs_info.tess_topology, b.es_vs_info.tess_topology);
        Field(out, partitioning, a.es_vs_info.tess_partitioning, b.es_vs_info.tess_partitioning);
    };
    switch (a.stage) {
    case Stage::Fragment: {
        const auto& x = a.fs_info;
        const auto& y = b.fs_info;
        for (u32 i = 0; i < Shader::MaxColorBuffers; ++i) {
            if (!(x.color_buffers[i] == y.color_buffers[i])) {
                out.Add("fs.color_buffers", i);
            }
        }
        Field(out, "fs.en_flags", x.en_flags, y.en_flags);
        Field(out, "fs.addr_flags", x.addr_flags, y.addr_flags);
        Field(out, "fs.num_inputs", x.num_inputs, y.num_inputs);
        Field(out, "fs.z_export_format", x.z_export_format, y.z_export_format);
        Field(out, "fs.mrtz_mask", x.mrtz_mask, y.mrtz_mask);
        Field(out, "fs.dual_source_blending", x.dual_source_blending, y.dual_source_blending);
        Field(out, "fs.clip_distance_emulation", x.clip_distance_emulation,
              y.clip_distance_emulation);
        Field(out, "fs.vr_cut", x.vr_cut, y.vr_cut);
        const u32 inputs = std::min({x.num_inputs, y.num_inputs, u32(x.inputs.size())});
        for (u32 i = 0; i < inputs; ++i) {
            if (!(x.inputs[i] == y.inputs[i])) {
                out.Add("fs.inputs", i);
            }
        }
        break;
    }
    case Stage::Vertex: {
        const auto& x = a.vs_info;
        const auto& y = b.vs_info;
        common("vs.hs_output_cp_stride", "vs.tess_type", "vs.tess_topology",
               "vs.tess_partitioning");
        Field(out, "vs.num_outputs", x.num_outputs, y.num_outputs);
        Field(out, "vs.outputs", x.outputs, y.outputs);
        Field(out, "vs.foreground_probe", x.foreground_probe, y.foreground_probe);
        Field(out, "vs.tess_emulated_primitive", x.tess_emulated_primitive,
              y.tess_emulated_primitive);
        Field(out, "vs.emulate_depth_negative_one_to_one", x.emulate_depth_negative_one_to_one,
              y.emulate_depth_negative_one_to_one);
        Field(out, "vs.clip_disable", x.clip_disable, y.clip_disable);
        Field(out, "vs.step_rate_0", x.step_rate_0, y.step_rate_0);
        Field(out, "vs.step_rate_1", x.step_rate_1, y.step_rate_1);
        Field(out, "vs.user_clip_plane_mask", x.user_clip_plane_mask, y.user_clip_plane_mask);
        break;
    }
    case Stage::Export:
        common("es.hs_output_cp_stride", "es.tess_type", "es.tess_topology",
               "es.tess_partitioning");
        Field(out, "es.vertex_data_size", a.es_info.vertex_data_size, b.es_info.vertex_data_size);
        break;
    case Stage::Hull: {
        const auto& x = a.hs_info;
        const auto& y = b.hs_info;
        Field(out, "hs.hs_output_cp_stride", a.hs_es_vs_info.hs_output_cp_stride,
              b.hs_es_vs_info.hs_output_cp_stride);
        Field(out, "hs.num_input_control_points", x.num_input_control_points,
              y.num_input_control_points);
        Field(out, "hs.num_threads", x.num_threads, y.num_threads);
        Field(out, "hs.tess_type", x.tess_type, y.tess_type);
        Field(out, "hs.offchip_lds_enable", x.offchip_lds_enable, y.offchip_lds_enable);
        Field(out, "hs.ls_stride", x.ls_stride, y.ls_stride);
        Field(out, "hs.hs_output_base", x.hs_output_base, y.hs_output_base);
        break;
    }
    case Stage::Local:
        Field(out, "ls.ls_stride", a.ls_info.ls_stride, b.ls_info.ls_stride);
        break;
    case Stage::Geometry: {
        const auto& x = a.gs_info;
        const auto& y = b.gs_info;
        Field(out, "gs.num_outputs", x.num_outputs, y.num_outputs);
        Field(out, "gs.outputs", x.outputs, y.outputs);
        // As GeometryRuntimeInfo::operator== has it: both non-zero, not equal.
        if (!(x.num_invocations && y.num_invocations)) {
            out.Add("gs.num_invocations");
        }
        Field(out, "gs.output_vertices", x.output_vertices, y.output_vertices);
        Field(out, "gs.in_primitive", x.in_primitive, y.in_primitive);
        Field(out, "gs.out_primitive", x.out_primitive, y.out_primitive);
        Field(out, "gs.vs_copy_hash", x.vs_copy_hash, y.vs_copy_hash);
        break;
    }
    case Stage::Compute:
        Field(out, "cs.workgroup_size", a.cs_info.workgroup_size, b.cs_info.workgroup_size);
        Field(out, "cs.tgid_enable", a.cs_info.tgid_enable, b.cs_info.tgid_enable);
        break;
    default:
        break;
    }
}

// What StageSpecialization::operator== finds different between an existing permutation and the
// one wanted (`existing == wanted`, as GetProgram's search compares them): nothing exactly when
// it finds them equal.
inline void DiffSpec(const Shader::StageSpecialization& existing,
                     const Shader::StageSpecialization& wanted, Names& out) {
    if (!existing.Valid()) {
        out.Add("invalid permutation");
        return;
    }
    if (existing.vs_attribs != wanted.vs_attribs) {
        const size_t count = std::max(existing.vs_attribs.size(), wanted.vs_attribs.size());
        for (size_t i = 0; i < count; ++i) {
            if (i >= existing.vs_attribs.size() || i >= wanted.vs_attribs.size() ||
                !(existing.vs_attribs[i] == wanted.vs_attribs[i])) {
                out.Add("vs_attribs", i);
            }
        }
    }
    DiffRuntime(existing.runtime_info, wanted.runtime_info, out);
    if (existing.fetch_shader_data != wanted.fetch_shader_data) {
        out.Add("fetch_shader");
    }
    if (existing.fmasks != wanted.fmasks) {
        out.Add("fmasks");
    }
    // As the comparison: no bound resource on either side, the rest is not compared.
    if (existing.bitset.none() && wanted.bitset.none()) {
        return;
    }
    Field(out, "start.unified", existing.start.unified, wanted.start.unified);
    Field(out, "start.buffer", existing.start.buffer, wanted.start.buffer);
    Field(out, "start.user_data", existing.start.user_data, wanted.start.user_data);
    // Only the sharps the wanted permutation binds (its bitset), as the comparison.
    u32 binding = 0;
    for (size_t i = 0; i < existing.buffers.size(); ++i) {
        if (wanted.bitset[binding++] &&
            (i >= wanted.buffers.size() || !(existing.buffers[i] == wanted.buffers[i]))) {
            out.Add("buffers", i);
        }
    }
    for (size_t i = 0; i < existing.images.size(); ++i) {
        if (wanted.bitset[binding++] &&
            (i >= wanted.images.size() || !(existing.images[i] == wanted.images[i]))) {
            out.Add("images", i);
        }
    }
    for (size_t i = 0; i < existing.samplers.size(); ++i) {
        if (i >= wanted.samplers.size() || !(existing.samplers[i] == wanted.samplers[i])) {
            out.Add("samplers", i);
        }
    }
}

// The GraphicsPipelineKey fields that differ (`stages`: how many of them are stage modules,
// stage_hashes[fs] and so on, named first).
inline void DiffKey(const GraphicsPipelineKey& a, const GraphicsPipelineKey& b, Names& out,
                    u32& stages) {
    stages = 0;
    for (u32 i = 0; i < MaxShaderStages; ++i) {
        if (a.stage_hashes[i] != b.stage_hashes[i]) {
            ++stages;
            out.Add(fmt::format("stage_hashes[{}]", LogicalStageName(i)));
        }
    }
    for (size_t i = 0; i < a.vertex_buffer_formats.size(); ++i) {
        if (a.vertex_buffer_formats[i] != b.vertex_buffer_formats[i]) {
            out.Add("vertex_buffer_formats", i);
        }
    }
    Field(out, "probe_view_mask", a.probe_view_mask, b.probe_view_mask);
    Field(out, "patch_control_points", a.patch_control_points, b.patch_control_points);
    Field(out, "num_color_attachments", a.num_color_attachments, b.num_color_attachments);
    for (size_t i = 0; i < a.color_buffers.size(); ++i) {
        if (!(a.color_buffers[i] == b.color_buffers[i])) {
            out.Add("color_buffers", i);
        }
    }
    for (size_t i = 0; i < a.blend_controls.size(); ++i) {
        Bytes(out, "blend_controls", i, a.blend_controls[i], b.blend_controls[i]);
    }
    for (size_t i = 0; i < a.write_masks.size(); ++i) {
        if (a.write_masks[i] != b.write_masks[i]) {
            out.Add("write_masks", i);
        }
    }
    if (std::memcmp(&a.cb_shader_mask, &b.cb_shader_mask, sizeof(a.cb_shader_mask)) != 0) {
        out.Add("cb_shader_mask");
    }
    Field(out, "logic_op", a.logic_op, b.logic_op);
    Field(out, "num_samples", a.num_samples, b.num_samples);
    Field(out, "depth_samples", a.depth_samples, b.depth_samples);
    for (size_t i = 0; i < a.color_samples.size(); ++i) {
        if (a.color_samples[i] != b.color_samples[i]) {
            out.Add("color_samples", i);
        }
    }
    Field(out, "mrt_mask", a.mrt_mask, b.mrt_mask);
    Field(out, "z_format", a.z_format, b.z_format);
    Field(out, "stencil_format", a.stencil_format, b.stencil_format);
    Field(out, "depth_clamp_enable", a.depth_clamp_enable, b.depth_clamp_enable);
    Field(out, "prim_type", a.prim_type, b.prim_type);
    Field(out, "polygon_mode", a.polygon_mode, b.polygon_mode);
    Field(out, "clip_space", a.clip_space, b.clip_space);
    Field(out, "provoking_vtx_last", a.provoking_vtx_last, b.provoking_vtx_last);
    Field(out, "depth_clip_enable", a.depth_clip_enable, b.depth_clip_enable);
}

// The VR variant a pipeline compiles for: the selection's own fields (a motion variant may carry
// its base's cut too), or a cut level prebuilt ahead of the cutaway.
enum class Variant : u8 { None, Cut, CutPrebuild, Motion, Probe, Count };
constexpr size_t NumVariants = static_cast<size_t>(Variant::Count);

template <typename Selection>
constexpr Variant VariantOf(const Selection& selection) {
    return selection.vr_probe_mode != 0  ? Variant::Probe
           : selection.vr_motion.enabled ? Variant::Motion
           : selection.vr_fs_cut != 0    ? Variant::Cut
                                         : Variant::None;
}

// What a pipeline compile is, one each, counted in 'FAST compile reasons:'. The VR variants and
// the cached ones (the disk cache had the key, the warm-up did not load it) are also counted on
// their own, beside their kind: a variant compiled twice still counts as again.
enum Kind : u32 {
    NewPermutation, // a stage module no pipeline used yet (translated this session)
    NewKey,         // every stage module in pipelines already: another key
    Again,          // this very key compiled before (a failed or expired job, ReplaceShader)
    NumKinds,
};
enum ShaderKind : u32 {
    NewProgram,
    NewShaderPermutation,
    Dropped, // the warm-up dropped its module (the driver rejected a cached pipeline of it)
    NumShaderKinds,
};

constexpr std::array<std::string_view, NumKinds> KindNames{"new_permutation", "new_key", "again"};
// A variant's counter and its tag on the line.
constexpr std::array<std::string_view, NumVariants> VariantNames{"", "cut", "cut_prebuild",
                                                                 "motion", "probe"};
constexpr std::array<std::string_view, NumVariants> VariantTags{"", "cut variant", "cut prebuild",
                                                                "motion variant", "probe variant"};

// One pipeline compile's tags, for the render threads' stall time (kind NumKinds: not worked
// out, its line was not logged).
struct Tags {
    Kind kind = NumKinds;
    Variant variant = Variant::None;
    bool cached = false;
};

// A graphics pipeline's key in the disk cache is named 'g_0x<hash>', the hash its 'Compiling
// graphics pipeline' line prints: that hash, or 0 for another name.
inline u64 PipelineBlobHash(std::string_view name) {
    constexpr std::string_view prefix = "g_0x";
    if (!name.starts_with(prefix) || name.size() == prefix.size()) {
        return 0;
    }
    u64 hash = 0;
    const char* last = name.data() + name.size();
    const auto [end, error] = std::from_chars(name.data() + prefix.size(), last, hash, 16);
    return error == std::errc{} && end == last ? hash : 0;
}

// The pipelines compiled or loaded so far, for comparing a new key with, and the counters. Any
// thread: the render threads (a compile there) and the compile threads (a background or
// prepared job), under its own lock; the idle-priority preparation workers never come here.
class Tracker {
public:
    // Keys compared per vertex module: its latest ones.
    static constexpr size_t MaxCompared = 64;

    // A pipeline that exists without a compile (the warm-up's): compared with, not counted.
    void Add(const GraphicsPipelineKey& key, u64 hash) {
        std::scoped_lock lock{mutex};
        AddLocked(key, hash);
    }

    // A stage module the warm-up loaded (its stage hash: the permutation's id,
    // pipeline_cache_policy.h): a pipeline using it does not make it a new permutation.
    void AddModule(u64 stage_hash) {
        std::scoped_lock lock{mutex};
        modules.insert(stage_hash);
    }

    // The warm-up: a key the disk cache has that it did not load, and why (a static string).
    void NotPreloaded(u64 hash, std::string_view why) {
        std::scoped_lock lock{mutex};
        notes[hash].cached = why;
    }

    // The warm-up: a cached stage permutation from another build (its stage hash; and
    // HashCombine(program, 0) for the program's first translation, whose id needs it).
    void StaleStage(u64 stage_hash) {
        std::scoped_lock lock{mutex};
        stale_stages.insert(stage_hash);
    }

    // A background job that went stale before compiling: the next compile of its key says so.
    void StaleJob(u64 hash) {
        std::scoped_lock lock{mutex};
        notes[hash].stale_job = true;
    }

    // A pipeline about to compile: why, counted (`owner`: on a render thread); `tags` for its
    // stall time.
    std::string Pipeline(const GraphicsPipelineKey& key, u64 hash, Variant variant, bool owner,
                         Tags& tags) {
        std::string why;
        Note note{};
        {
            std::scoped_lock lock{mutex};
            tags.kind = ClassifyLocked(key, hash, why);
            if (tags.kind != Again) {
                AddLocked(key, hash);
            }
            if (const auto it = notes.find(hash); it != notes.end()) {
                note = it->second; // once: a later compile of it is this session's again
                notes.erase(it);
            }
        }
        if (!note.cached.empty()) {
            fmt::format_to(std::back_inserter(why), "; cached, not preloaded: {}", note.cached);
        }
        if (note.stale_job) {
            why += "; its background job went stale";
        }
        if (variant != Variant::None) {
            why = fmt::format("{} ({})", VariantTags[static_cast<u32>(variant)], why);
        }
        tags.variant = variant;
        tags.cached = !note.cached.empty();
        Count(counts[owner ? 0 : 1], tags, 1);
        changed.store(true, std::memory_order_relaxed);
        return why;
    }

    // A shader about to be translated: a new program (`existing` null or empty), or a new
    // permutation `wanted` of a program with the permutations `existing` (the render thread);
    // `stage_hash`: the one it gets.
    template <typename ModuleList>
    std::string Module(const ModuleList* existing, const Shader::StageSpecialization* wanted,
                       u64 stage_hash) {
        ShaderKind kind = NewProgram;
        std::string why = existing == nullptr || existing->empty() || wanted == nullptr
                              ? std::string{"new program"}
                              : Permutation(*existing, *wanted, kind);
        bool stale = false;
        {
            std::scoped_lock lock{mutex};
            stale = stale_stages.erase(stage_hash) != 0;
        }
        if (stale) {
            why += "; cached, stale (another build's)";
            shaders_stale.fetch_add(1, std::memory_order_relaxed);
        }
        shaders[kind].fetch_add(1, std::memory_order_relaxed);
        changed.store(true, std::memory_order_relaxed);
        return why;
    }

    // A render thread stalled `ns` on a compile tagged `tags` (compiled there, or waited for).
    void Stall(const Tags& tags, u64 ns) {
        if (tags.kind >= NumKinds) {
            return;
        }
        Count(owner_ns, tags, ns);
        changed.store(true, std::memory_order_relaxed);
    }

    // The counters (cumulative) when any changed since the last call, else empty. owner_ms: the
    // render threads' stall time by the same tags.
    std::string Report() {
        if (!changed.exchange(false, std::memory_order_relaxed)) {
            return {};
        }
        auto text = fmt::format("shaders new_program={} new_permutation={} dropped={}, of them "
                                "cached_stale={}; pipelines (render thread/background)",
                                Load(shaders[NewProgram]), Load(shaders[NewShaderPermutation]),
                                Load(shaders[Dropped]), Load(shaders_stale));
        auto out = std::back_inserter(text);
        const auto& [owners, background] = counts;
        for (u32 kind = 0; kind < NumKinds; ++kind) {
            fmt::format_to(out, " {}={}/{}", KindNames[kind], Load(owners.kinds[kind]),
                           Load(background.kinds[kind]));
        }
        fmt::format_to(out, ", of them cached={}/{}", Load(owners.cached), Load(background.cached));
        for (u32 variant = 1; variant < NumVariants; ++variant) {
            fmt::format_to(out, " {}={}/{}", VariantNames[variant], Load(owners.variants[variant]),
                           Load(background.variants[variant]));
        }
        const auto ms = [](const std::atomic<u64>& ns) {
            return static_cast<double>(Load(ns)) / 1e6;
        };
        text += "; owner_ms";
        for (u32 kind = 0; kind < NumKinds; ++kind) {
            fmt::format_to(out, " {}={:.2f}", KindNames[kind], ms(owner_ns.kinds[kind]));
        }
        fmt::format_to(out, ", of them cached={:.2f}", ms(owner_ns.cached));
        for (u32 variant = 1; variant < NumVariants; ++variant) {
            fmt::format_to(out, " {}={:.2f}", VariantNames[variant],
                           ms(owner_ns.variants[variant]));
        }
        return text;
    }

    // The closest permutation (the latest MaxCompared): what `wanted` differs in from it.
    template <typename ModuleList>
    static std::string Permutation(const ModuleList& existing,
                                   const Shader::StageSpecialization& wanted, ShaderKind& kind) {
        const size_t size = existing.size();
        size_t best = size;
        Names best_names;
        for (size_t i = size > MaxCompared ? size - MaxCompared : 0; i < size; ++i) {
            // A gap the warm-up left below a permutation it loaded: nothing there to compare.
            if (!existing[i].spec.Valid()) {
                continue;
            }
            Names names;
            DiffSpec(existing[i].spec, wanted, names);
            if (best == size || names.count < best_names.count) {
                best = i;
                best_names = std::move(names);
                if (best_names.count == 0) {
                    break;
                }
            }
        }
        kind = NewShaderPermutation;
        if (best == size) {
            return fmt::format("new permutation (none of its {} loaded to compare)", size);
        }
        if (best_names.count != 0) {
            return fmt::format("new permutation: {} (closest #{} of {})", best_names.Text(), best,
                               size);
        }
        if (!existing[best].module) {
            kind = Dropped;
            return fmt::format("again: permutation #{} (the warm-up dropped its module)", best);
        }
        return fmt::format("new permutation: nothing compared differs from #{} of {}", best, size);
    }

private:
    // Per tag: compiles (a render thread's, a compile thread's), or the render threads' stall ns.
    struct Counts {
        std::array<std::atomic<u64>, NumKinds> kinds{};
        std::array<std::atomic<u64>, NumVariants> variants{}; // [0]: none, unused
        std::atomic<u64> cached{};
    };

    // Why a key compiles besides its kind (once, its next compile).
    struct Note {
        std::string_view cached; // why the warm-up did not load it (empty: not cached)
        bool stale_job{};
    };

    static u64 Load(const std::atomic<u64>& value) {
        return value.load(std::memory_order_relaxed);
    }

    static void Count(Counts& counts, const Tags& tags, u64 value) {
        counts.kinds[tags.kind].fetch_add(value, std::memory_order_relaxed);
        if (tags.variant != Variant::None) {
            counts.variants[static_cast<u32>(tags.variant)].fetch_add(value,
                                                                      std::memory_order_relaxed);
        }
        if (tags.cached) {
            counts.cached.fetch_add(value, std::memory_order_relaxed);
        }
    }

    Kind ClassifyLocked(const GraphicsPipelineKey& key, u64 hash, std::string& why) const {
        // Any key compiled or loaded before, also one past the latest 64 of its vertex module.
        if (compiled.contains(hash)) {
            why = "again: this key compiled before";
            return Again;
        }
        Names fresh;
        for (u32 i = 0; i < MaxShaderStages; ++i) {
            if (key.stage_hashes[i] != 0 && !modules.contains(key.stage_hashes[i])) {
                fresh.Add(LogicalStageName(i));
            }
        }
        if (fresh.count != 0) {
            why = "new permutation: " + fresh.Text();
            return NewPermutation;
        }
        const auto bucket = by_vertex.find(key.stage_hashes[u32(Shader::LogicalStage::Vertex)]);
        if (bucket == by_vertex.end() || bucket->second.keys.empty()) {
            why = "existing modules, new key (no pipeline with its vertex module to compare)";
            return NewKey;
        }
        // The fewest differing stage modules first, then the fewest differing fields.
        const GraphicsPipelineKey* best = nullptr;
        Names best_names;
        u32 best_stages = 0;
        for (const auto& other : bucket->second.keys) {
            Names names;
            u32 stages = 0;
            DiffKey(other, key, names, stages);
            if (best == nullptr || stages < best_stages ||
                (stages == best_stages && names.count < best_names.count)) {
                best = &other;
                best_stages = stages;
                best_names = std::move(names);
            }
        }
        why = best_names.count == 0
                  ? std::string{"existing modules, new key: padding bytes only"}
                  : fmt::format("existing modules, new key: {} (closest of {} with its vertex "
                                "module)",
                                best_names.Text(), bucket->second.keys.size());
        return NewKey;
    }

    void AddLocked(const GraphicsPipelineKey& key, u64 hash) {
        compiled.insert(hash);
        for (u32 i = 0; i < MaxShaderStages; ++i) {
            if (key.stage_hashes[i] != 0) {
                modules.insert(key.stage_hashes[i]);
            }
        }
        auto& bucket = by_vertex[key.stage_hashes[u32(Shader::LogicalStage::Vertex)]];
        if (bucket.keys.size() < MaxCompared) {
            bucket.keys.push_back(key);
        } else {
            bucket.keys[bucket.next] = key; // the oldest
            bucket.next = (bucket.next + 1) % MaxCompared;
        }
    }

    struct Bucket {
        std::vector<GraphicsPipelineKey> keys;
        size_t next{};
    };
    std::mutex mutex;
    std::unordered_map<u64, Bucket> by_vertex;
    std::unordered_set<u64> compiled;    // every key hash compiled or loaded (8 bytes each)
    std::unordered_set<u64> modules;     // the stage hashes in them, and the warm-up's modules
    std::unordered_map<u64, Note> notes; // by key hash
    std::unordered_set<u64> stale_stages;
    std::array<Counts, 2> counts{}; // a render thread's compiles, a compile thread's
    Counts owner_ns{};
    std::array<std::atomic<u64>, NumShaderKinds> shaders{};
    std::atomic<u64> shaders_stale{};
    std::atomic<bool> changed{false};
};

} // namespace Vulkan::CompileReason
