// SPDX-License-Identifier: GPL-2.0-or-later

// Why a shader or pipeline compiled (video_core/renderer_vulkan/vk_compile_reason.h): the fields
// a new permutation differs in from its program's closest one (nothing exactly when the
// specialization's own comparison finds them equal; the warm-up's gaps skipped), and for a
// pipeline a new permutation, the key fields that differ from the closest key with the same
// vertex module (the same stage modules first, the latest 64 compared), the same key again (any
// key before), the VR variant, and what the disk cache had but the warm-up did not load; and the
// counters and the stall time by reason.

#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vk_compile_reason.h"

namespace {

using namespace Vulkan;
using namespace Vulkan::CompileReason;
using Shader::LogicalStage;
using Shader::RuntimeInfo;
using Shader::Stage;
using Shader::StageSpecialization;

RuntimeInfo Fragment() {
    RuntimeInfo info{};
    info.Initialize(Stage::Fragment);
    info.native_stereo = true;
    info.native_multiview = true;
    info.fs_info.num_inputs = 2;
    return info;
}

RuntimeInfo Vertex() {
    RuntimeInfo info{};
    info.Initialize(Stage::Vertex);
    info.vs_info.num_outputs = 1;
    return info;
}

std::string Diff(const RuntimeInfo& a, const RuntimeInfo& b) {
    Names names;
    DiffRuntime(a, b, names);
    EXPECT_EQ(names.count == 0, a == b) << names.Text();
    return names.Text();
}

TEST(CompileReason, RuntimeNamesTheVrFieldsAndTheFragmentSlots) {
    const auto base = Fragment();
    auto other = base;
    other.native_projection[0][1] = 0.5f;
    other.fs_info.color_buffers[2].export_format = AmdGpu::ShaderExportFormat::ABGR_FP16;
    other.fs_info.vr_cut = 1;
    EXPECT_EQ(Diff(base, other), "native_projection, fs.color_buffers[2], fs.vr_cut");

    other = base;
    other.vr_motion.enabled = true;
    other.native_light_scissor = true;
    other.fs_info.clip_distance_emulation = true;
    other.fs_info.num_inputs = 3;
    other.fs_info.en_flags.persp_center_ena = 1;
    EXPECT_EQ(Diff(base, other), "vr_motion, native_light_scissor, fs.en_flags, fs.num_inputs, "
                                 "fs.clip_distance_emulation");

    // Baked motion buffers allocated anew.
    auto baked = base;
    baked.vr_motion.enabled = true;
    baked.vr_motion.params_address = 0x1000;
    auto moved = baked;
    moved.vr_motion.params_address = 0x2000;
    EXPECT_EQ(Diff(baked, moved), "vr_motion.addresses");

    // An input past num_inputs is not compared.
    other = base;
    other.fs_info.inputs[5].param_index = 7;
    EXPECT_EQ(Diff(base, other), "");
    other.fs_info.inputs[1].param_index = 7;
    EXPECT_EQ(Diff(base, other), "fs.inputs[1]");
}

TEST(CompileReason, RuntimeNamesTheOtherStagesMembers) {
    const auto base = Vertex();
    auto other = base;
    other.vs_info.step_rate_0 = 4;
    other.es_vs_info.tess_type = AmdGpu::TessellationType::Quad;
    EXPECT_EQ(Diff(base, other), "vs.tess_type, vs.step_rate_0");

    RuntimeInfo hull{};
    hull.Initialize(Stage::Hull);
    auto hull2 = hull;
    hull2.hs_info.num_threads = 3;
    hull2.hs_es_vs_info.hs_output_cp_stride = 16;
    EXPECT_EQ(Diff(hull, hull2), "hs.hs_output_cp_stride, hs.num_threads");

    // Not compared by the comparison: no name either.
    RuntimeInfo compute{};
    compute.Initialize(Stage::Compute);
    auto compute2 = compute;
    compute2.cs_info.shared_memory_size = 1024;
    EXPECT_EQ(Diff(compute, compute2), "");
    compute2.cs_info.workgroup_size[0] = 64;
    EXPECT_EQ(Diff(compute, compute2), "cs.workgroup_size");

    // A geometry stage with no invocations never compares equal: named, so it shows.
    RuntimeInfo geometry{};
    geometry.Initialize(Stage::Geometry);
    EXPECT_EQ(Diff(geometry, geometry), "gs.num_invocations");
    geometry.gs_info.num_invocations = 1;
    EXPECT_EQ(Diff(geometry, geometry), "");
}

// A permutation as GetProgram keeps it, without reading sharps: `bound` sharps set in the bitset.
StageSpecialization Spec(const Shader::Info* info, const RuntimeInfo& runtime, u32 buffers,
                         u32 bound) {
    StageSpecialization spec{};
    spec.info = info;
    spec.runtime_info = runtime;
    for (u32 i = 0; i < buffers; ++i) {
        auto& buffer = spec.buffers.emplace_back();
        std::memset(&buffer, 0, sizeof(buffer));
        buffer.stride = 16;
        if (bound & (1u << i)) {
            spec.bitset.set(i);
        }
    }
    spec.samplers.emplace_back();
    std::memset(&spec.samplers[0], 0, sizeof(spec.samplers[0]));
    return spec;
}

std::string Diff(const StageSpecialization& existing, const StageSpecialization& wanted) {
    Names names;
    DiffSpec(existing, wanted, names);
    EXPECT_EQ(names.count == 0, existing == wanted) << names.Text();
    return names.Text();
}

TEST(CompileReason, SpecNamesWhatItsComparisonCompares) {
    const int dummy = 0;
    const auto* info = reinterpret_cast<const Shader::Info*>(&dummy); // never read
    const auto base = Spec(info, Fragment(), 3, 0b011);

    // Only the sharps the wanted permutation binds.
    auto other = base;
    other.buffers[2].stride = 32;
    EXPECT_EQ(Diff(base, other), "");
    other.buffers[1].stride = 32;
    EXPECT_EQ(Diff(base, other), "buffers[1]");

    other = base;
    other.start.user_data = 4;
    other.samplers[0].force_degamma = 1;
    other.runtime_info.fs_info.vr_cut = 1;
    EXPECT_EQ(Diff(base, other), "fs.vr_cut, start.user_data, samplers[0]");

    // Nothing bound on either side: the binding start and the sharps are not compared.
    const auto unbound = Spec(info, Fragment(), 2, 0);
    other = unbound;
    other.start.unified = 3;
    other.buffers[0].stride = 4;
    EXPECT_EQ(Diff(unbound, other), "");

    other = unbound;
    other.vs_attribs.emplace_back();
    EXPECT_EQ(Diff(unbound, other), "vs_attribs[0]");
}

// Program::Module's two members the reason reads.
struct Module {
    bool module;
    StageSpecialization spec;
};

TEST(CompileReason, ANewPermutationNamesItsClosestOne) {
    const int dummy = 0;
    const auto* info = reinterpret_cast<const Shader::Info*>(&dummy);
    auto wanted_rt = Fragment();
    wanted_rt.fs_info.vr_cut = 1;
    const auto wanted = Spec(info, wanted_rt, 2, 0b11);

    auto far_rt = Fragment();
    far_rt.native_multiview = false;
    far_rt.fs_info.color_buffers[0].export_format = AmdGpu::ShaderExportFormat::ABGR_FP16;
    auto mid_rt = Fragment();
    mid_rt.fs_info.clip_distance_emulation = true;
    std::vector<Module> existing{{true, Spec(info, far_rt, 2, 0b11)},
                                 {true, Spec(info, Fragment(), 2, 0b11)},
                                 {true, Spec(info, mid_rt, 2, 0b11)}};
    ShaderKind kind{};
    EXPECT_EQ(Tracker::Permutation(existing, wanted, kind),
              "new permutation: fs.vr_cut (closest #1 of 3)");
    EXPECT_EQ(kind, NewShaderPermutation);

    // The same permutation with its module dropped by the warm-up: compiled again at its index.
    existing.push_back({false, wanted});
    EXPECT_EQ(Tracker::Permutation(existing, wanted, kind),
              "again: permutation #3 (the warm-up dropped its module)");
    EXPECT_EQ(kind, Dropped);

    Tracker tracker;
    EXPECT_EQ(tracker.Module<std::vector<Module>>(nullptr, nullptr, 1), "new program");
    const std::vector<Module> none;
    EXPECT_EQ(tracker.Module(&none, &wanted, 2), "new program");
}

TEST(CompileReason, TheWarmUpsGapsAreNotCompared) {
    const int dummy = 0;
    const auto* info = reinterpret_cast<const Shader::Info*>(&dummy);
    auto wanted_rt = Fragment();
    wanted_rt.fs_info.vr_cut = 1;
    wanted_rt.fs_info.clip_distance_emulation = true;
    const auto wanted = Spec(info, wanted_rt, 2, 0b11);

    // The warm-up loaded permutation #2 alone: #0 and #1 are gaps (no specialization).
    std::vector<Module> existing{{false, StageSpecialization{}},
                                 {false, StageSpecialization{}},
                                 {true, Spec(info, Fragment(), 2, 0b11)}};
    ShaderKind kind{};
    EXPECT_EQ(Tracker::Permutation(existing, wanted, kind),
              "new permutation: fs.clip_distance_emulation, fs.vr_cut (closest #2 of 3)");
    existing.pop_back();
    EXPECT_EQ(Tracker::Permutation(existing, wanted, kind),
              "new permutation (none of its 2 loaded to compare)");
    EXPECT_EQ(kind, NewShaderPermutation);
}

TEST(CompileReason, AStaleCachedStageSaysSoOnce) {
    Tracker tracker;
    tracker.StaleStage(0x77);
    EXPECT_EQ(tracker.Module<std::vector<Module>>(nullptr, nullptr, 0x77),
              "new program; cached, stale (another build's)");
    EXPECT_EQ(tracker.Module<std::vector<Module>>(nullptr, nullptr, 0x77), "new program");
    EXPECT_EQ(tracker.Report(), "shaders new_program=2 new_permutation=0 dropped=0, of them "
                                "cached_stale=1; pipelines (render thread/background) "
                                "new_permutation=0/0 new_key=0/0 again=0/0, of them cached=0/0 "
                                "cut=0/0 cut_prebuild=0/0 motion=0/0 probe=0/0; owner_ms "
                                "new_permutation=0.00 new_key=0.00 again=0.00, of them "
                                "cached=0.00 cut=0.00 cut_prebuild=0.00 motion=0.00 probe=0.00");
}

GraphicsPipelineKey Key(u64 vs, u64 fs) {
    GraphicsPipelineKey key{};
    key.stage_hashes[u32(LogicalStage::Vertex)] = vs;
    key.stage_hashes[u32(LogicalStage::Fragment)] = fs;
    key.num_color_attachments = 1;
    key.mrt_mask = 1;
    return key;
}

u64 Hash(const GraphicsPipelineKey& key) {
    return std::hash<GraphicsPipelineKey>{}(key);
}

void Add(Tracker& tracker, const GraphicsPipelineKey& key) {
    tracker.Add(key, Hash(key));
}

std::string Compile(Tracker& tracker, const GraphicsPipelineKey& key, Variant variant, bool owner,
                    Tags* tags = nullptr) {
    Tags local{};
    return tracker.Pipeline(key, Hash(key), variant, owner, tags ? *tags : local);
}

TEST(CompileReason, PipelinesByPermutationKeyAndVariant) {
    Tracker tracker;
    Add(tracker, Key(1, 2)); // the warm-up's

    Tags tags{};
    EXPECT_EQ(Compile(tracker, Key(1, 3), Variant::None, true, &tags), "new permutation: fs");
    EXPECT_EQ(tags.kind, NewPermutation);
    EXPECT_EQ(Compile(tracker, Key(4, 5), Variant::None, false), "new permutation: fs, vs");

    auto blended = Key(1, 2);
    blended.blend_controls[0].enable = 1;
    blended.write_masks[0] = vk::ColorComponentFlagBits::eR;
    EXPECT_EQ(Compile(tracker, blended, Variant::None, true),
              "existing modules, new key: blend_controls[0], write_masks[0] (closest of 2 with "
              "its vertex module)");
    EXPECT_EQ(Compile(tracker, blended, Variant::None, false), "again: this key compiled before");

    auto cut = Key(1, 6);
    EXPECT_EQ(Compile(tracker, cut, Variant::Cut, true), "cut variant (new permutation: fs)");
    cut.prim_type = AmdGpu::PrimitiveType::TriangleStrip;
    EXPECT_EQ(Compile(tracker, cut, Variant::CutPrebuild, false),
              "cut prebuild (existing modules, new key: prim_type (closest of 4 with its vertex "
              "module))");
    // A variant compiled again counts as again too.
    EXPECT_EQ(Compile(tracker, blended, Variant::Motion, true, &tags),
              "motion variant (again: this key compiled before)");
    EXPECT_EQ(tags.kind, Again);
    EXPECT_EQ(tags.variant, Variant::Motion);
    tracker.Stall(tags, 2'500'000);
    tracker.Stall(Tags{}, 9'000'000); // not worked out (its line was not logged): not counted

    EXPECT_EQ(tracker.Report(),
              "shaders new_program=0 new_permutation=0 dropped=0, of them cached_stale=0; "
              "pipelines (render thread/background) new_permutation=2/1 new_key=1/1 again=1/1, of "
              "them cached=0/0 cut=1/0 cut_prebuild=0/1 motion=1/0 probe=0/0; owner_ms "
              "new_permutation=0.00 new_key=0.00 again=2.50, of them cached=0.00 cut=0.00 "
              "cut_prebuild=0.00 motion=2.50 probe=0.00");
    EXPECT_EQ(tracker.Report(), ""); // nothing compiled since
}

TEST(CompileReason, WhatTheDiskCacheHadSaysSoOnce) {
    Tracker tracker;
    // Modules the warm-up loaded without loading a pipeline of them.
    tracker.AddModule(1);
    tracker.AddModule(2);

    const auto stale = Key(1, 3); // its fragment stage was stale: translated again
    tracker.NotPreloaded(Hash(stale), "a stage stale");
    Tags tags{};
    EXPECT_EQ(Compile(tracker, stale, Variant::None, true, &tags),
              "new permutation: fs; cached, not preloaded: a stage stale");
    EXPECT_TRUE(tags.cached);
    tracker.Stall(tags, 80'000'000);

    // Its modules loaded: existing modules, not a new permutation.
    const auto idle = Key(1, 2);
    tracker.NotPreloaded(Hash(idle), "object motion, no upscaler");
    EXPECT_EQ(Compile(tracker, idle, Variant::Motion, false),
              "motion variant (existing modules, new key: stage_hashes[fs] (closest of 1 with its "
              "vertex module); cached, not preloaded: object motion, no upscaler)");

    // A background job of it went stale: the owner compiles it.
    auto later = Key(1, 2);
    later.prim_type = AmdGpu::PrimitiveType::TriangleStrip;
    tracker.StaleJob(Hash(later));
    EXPECT_EQ(Compile(tracker, later, Variant::None, true, &tags),
              "existing modules, new key: prim_type (closest of 2 with its vertex module); its "
              "background job went stale");
    EXPECT_FALSE(tags.cached);
    tracker.Stall(tags, 20'000'000);

    // Once: compiled this session since.
    EXPECT_EQ(Compile(tracker, stale, Variant::None, true, &tags),
              "again: this key compiled before");
    EXPECT_FALSE(tags.cached);
    tracker.Stall(tags, 1'500'000);

    EXPECT_EQ(tracker.Report(),
              "shaders new_program=0 new_permutation=0 dropped=0, of them cached_stale=0; "
              "pipelines (render thread/background) new_permutation=1/0 new_key=1/1 again=1/0, of "
              "them cached=1/1 cut=0/0 cut_prebuild=0/0 motion=0/1 probe=0/0; owner_ms "
              "new_permutation=80.00 new_key=20.00 again=1.50, of them cached=80.00 cut=0.00 "
              "cut_prebuild=0.00 motion=0.00 probe=0.00");
}

TEST(CompileReason, TheCacheKeyNamesGiveTheHash) {
    EXPECT_EQ(PipelineBlobHash("g_0x00000000deadbeef"), 0xdeadbeefull);
    EXPECT_EQ(PipelineBlobHash(fmt::format("g_{:#018x}", 0x8a1f00c2d4e5b611ull)),
              0x8a1f00c2d4e5b611ull);
    EXPECT_EQ(PipelineBlobHash("c_0x00000000deadbeef"), 0u); // a compute pipeline's
    EXPECT_EQ(PipelineBlobHash("g_0x"), 0u);
    EXPECT_EQ(PipelineBlobHash("g_0x12zz"), 0u);
}

TEST(CompileReason, TheSameStageModulesComeFirst) {
    Tracker tracker;
    auto same_modules = Key(1, 2);
    same_modules.prim_type = AmdGpu::PrimitiveType::TriangleStrip;
    same_modules.mrt_mask = 3;
    Add(tracker, same_modules);
    Add(tracker, Key(1, 7)); // one field closer, but another fragment module
    EXPECT_EQ(Compile(tracker, Key(1, 2), Variant::None, true),
              "existing modules, new key: mrt_mask, prim_type (closest of 2 with its vertex "
              "module)");
    // A combination of known modules no key has: the closest names the stage module too.
    Add(tracker, Key(9, 8));
    EXPECT_EQ(Compile(tracker, Key(1, 8), Variant::None, true),
              "existing modules, new key: stage_hashes[fs] (closest of 3 with its vertex "
              "module)");
}

TEST(CompileReason, AtMostTheLatest64KeysOfAVertexModule) {
    Tracker tracker;
    for (u32 i = 0; i < 100; ++i) {
        auto key = Key(1, 2);
        key.patch_control_points = i;
        Add(tracker, key);
    }
    auto key = Key(1, 2);
    key.patch_control_points = 1000;
    EXPECT_EQ(Compile(tracker, key, Variant::None, true),
              "existing modules, new key: patch_control_points (closest of 64 with its vertex "
              "module)");
    // An old key no longer compared with: still the same key again.
    key.patch_control_points = 0;
    EXPECT_EQ(Compile(tracker, key, Variant::Motion, true),
              "motion variant (again: this key compiled before)");
}

TEST(CompileReason, LongListsKeepCounting) {
    Names names;
    for (u32 i = 0; i < 15; ++i) {
        names.Add("x", i);
    }
    EXPECT_EQ(names.count, 15u);
    EXPECT_EQ(names.Text(),
              "x[0], x[1], x[2], x[3], x[4], x[5], x[6], x[7], x[8], x[9], x[10], x[11] (+3 more)");
}

} // namespace
