// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <unordered_set>

#include <xxhash.h>
#include "shader_recompiler/ir/passes/billboard_face_eye.h"

#include "video_core/native_stereo/native_stereo.h"
#include "common/hash.h"
#include "common/serdes.h"
#include "core/emulator_settings.h"
#include "core/vr/graphics_settings.h"
#include "core/vr/vr_session.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "video_core/cache_storage.h"
#include "video_core/pipeline_cache_policy.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Serialization {
/* You should increment versions below once corresponding serialization scheme is changed. */
/* Bloodborne VR fork: the version words, and which revision to bump for which change (the layout,
 * every stage's SPIR-V, only the vertex epilogue, only the fragment programs, only the vr_cut
 * permutations), are in
 * video_core/pipeline_cache_policy.h. */
// FAST_CPU_PERM_IDS (off unless '1'; read once at start-up; pipeline_cache_policy.h Naming): a
// permutation is named on disk by its id (PermutationId) instead of HashCombine(program, its
// index), its meta "<id>" and its binary "<id>" (not "<program>_<index>"), and the meta's header
// holds the program's hash too. Rendering is the same either way; the files are not: the first
// session with the ids compiles every pipeline once (the indexed naming's keys are kept for it,
// Reject::OtherNaming), hence opt-in. Off: the caches' names and words before the ids, as before.
static const bool g_perm_ids = [] {
    const char* value = std::getenv("FAST_CPU_PERM_IDS");
    return value && value[0] == '1';
}();
static const Vulkan::PipelineCachePolicy::Naming g_naming =
    g_perm_ids ? Vulkan::PipelineCachePolicy::Naming::Ids
               : Vulkan::PipelineCachePolicy::Naming::Indexed;
static const u32 ShaderMetaVersion = Vulkan::PipelineCachePolicy::ShaderMetaVersion(g_naming);
static const u32 PipelineKeyVersion = Vulkan::PipelineCachePolicy::PipelineKeyVersion(g_naming);
// The other naming's key word: such a key is kept for that naming's sessions.
static const u32 OtherNamingKeyVersion = Vulkan::PipelineCachePolicy::PipelineKeyVersion(
    Vulkan::PipelineCachePolicy::OtherNaming(g_naming));

// The binary's word depends on what the program's SPIR-V depends on (its scope).
static u32 ShaderBinaryVersionOf(const Shader::RuntimeInfo& runtime_info) {
    using namespace Vulkan::PipelineCachePolicy;
    const bool fragment = runtime_info.stage == Shader::Stage::Fragment;
    // The geometry stage runs the vertex epilogue before every EmitVertex.
    const bool epilogue = runtime_info.stage == Shader::Stage::Vertex ||
                          runtime_info.stage == Shader::Stage::Geometry;
    return ShaderBinaryVersion(
        ScopeOf(epilogue, fragment, fragment ? runtime_info.fs_info.vr_cut : 0u));
}

// A permutation index past this is a corrupted meta (a program has a handful of permutations).
static constexpr size_t MaxCachedPermutation = 4096;

// A meta's name (both namings), or a binary's with the ids: the permutation's id (the pipeline
// keys' stage hash).
static std::string BlobName(u64 perm_id) {
    return fmt::format("{:#018x}", perm_id);
}
// A binary's name: its id with FAST_CPU_PERM_IDS, else "<program>_<index>" as before.
static std::string BinaryName(u64 perm_id, u64 pgm_hash, size_t perm_idx) {
    return g_perm_ids ? BlobName(perm_id) : fmt::format("{:#018x}_{}", pgm_hash, perm_idx);
}
} // namespace Serialization

namespace Vulkan {

bool PermutationIds() {
    return Serialization::g_perm_ids;
}

namespace {
// FAST_CPU_CACHE_GC (off unless '1', and only with FAST_CPU_PERM_IDS=1, whose ids it reads the
// names by; off: every meta and binary stays, as before): the warm-up deletes the metas and
// binaries no permutation of the session is named by (PipelineCachePolicy::CollectUnnamed): the
// stale ones, and the indexed naming's too, which a session with FAST_CPU_PERM_IDS off then
// compiles again. Files only: what a session loads and renders is the same, but the deletion
// cannot be undone (an older build or the indexed naming finds nothing), hence opt-in.
// FAST_CPU_CACHE_GC_VERIFY=1: also checks that every id a kept key names is a permutation the
// warm-up holds and that no program holds an id twice (LOG_ERROR for the first mismatches, one
// LOG_INFO count line). Read once at start-up.
const bool g_cache_gc = [] {
    const char* value = std::getenv("FAST_CPU_CACHE_GC");
    return Serialization::g_perm_ids && value && value[0] == '1';
}();
const bool g_cache_gc_verify = [] {
    const char* value = std::getenv("FAST_CPU_CACHE_GC_VERIFY");
    return g_cache_gc && value && value[0] == '1';
}();
} // namespace

u64 PermutationId(u64 pgm_hash, const Shader::StageSpecialization& spec) {
    // Exactly the bytes a meta holds for it (not the archive's spare capacity past them).
    Serialization::Archive ar;
    spec.Serialize(ar);
    const u8* end = ar.CurrPtr();
    const std::vector<u8> bytes = ar.TakeOff(); // moved: the same storage
    const size_t size = static_cast<size_t>(end - bytes.data());
    return PipelineCachePolicy::NonZeroId(XXH3_64bits_withSeed(bytes.data(), size, pgm_hash));
}

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{1}); // compute

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("c_{:#018x}", key.value), ar.TakeOff());
}

void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar{};
    Serialization::Writer pldata{ar};

    pldata.Write(Serialization::PipelineKeyVersion);
    pldata.Write(u32{0}); // graphics

    key.Serialize(ar);
    sdata.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::PipelineKey,
                                       fmt::format("g_{:#018x}", hash), ar.TakeOff());
}

void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, u64 perm_id, size_t perm_idx,
                        u64 vertex_output_locations) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Serialization::Archive ar;
    Serialization::Writer meta{ar};

    // A motion permutation only with VrPushData (its flag alone): baked addresses never persist.
    ASSERT(!spec.runtime_info.vr_motion.Baked());
    meta.Write(Serialization::ShaderMetaVersion);
    meta.Write(Serialization::ShaderBinaryVersionOf(spec.runtime_info));

    // The header: the id (the file's name), the program (FAST_CPU_PERM_IDS only: a stale meta
    // still names it for the compile reasons) and the index the warm-up puts it at.
    meta.Write(perm_id);
    if (Serialization::g_perm_ids) {
        meta.Write(u64{info.pgm_hash});
    }
    meta.Write(u64{perm_idx});

    spec.Serialize(ar);
    auto serialized_info = info;
    serialized_info.vertex_output_locations = vertex_output_locations;
    serialized_info.Serialize(ar);

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderMeta,
                                       Serialization::BlobName(perm_id), ar.TakeOff());
}

void RegisterShaderBinary(std::vector<u32>&& spv, u64 perm_id, u64 pgm_hash, size_t perm_idx) {
    if (!Storage::DataBase::Instance().IsOpened()) {
        return;
    }

    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderBinary,
                                       Serialization::BinaryName(perm_id, pgm_hash, perm_idx),
                                       std::move(spv));
}

using Vulkan::PipelineCachePolicy::Reject;

// `expected_id`: the permutation id the pipeline key names this stage by (the meta's file name).
// `program`: the program's hash as the header has it, also for a stale meta of this layout (0 for
// another layout's, and with the indexed naming, whose header has none).
static Reject LoadShaderMeta(Serialization::Archive& ar, Shader::Info& info,
                             std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                             Shader::StageSpecialization& spec, size_t& perm_idx,
                             u64 expected_id, u64& program) {
    Serialization::Reader meta{ar};
    program = 0;

    u32 meta_version{};
    meta.Read(meta_version);
    if (ar.Failed()) {
        return Reject::Corrupt;
    }
    if (meta_version != Serialization::ShaderMetaVersion) {
        return Reject::StageStale; // another layout: nothing below can be read
    }

    u32 binary_version{};
    meta.Read(binary_version);

    u64 perm_id_ar{};
    u64 program_ar{};
    u64 perm_idx_ar{};
    meta.Read(perm_id_ar);
    if (Serialization::g_perm_ids) {
        meta.Read(program_ar);
    }
    meta.Read(perm_idx_ar);

    const bool spec_ok = spec.Deserialize(ar);
    if (ar.Failed() || !spec_ok || perm_id_ar != expected_id ||
        perm_idx_ar > Serialization::MaxCachedPermutation) {
        return Reject::Corrupt;
    }
    perm_idx = static_cast<size_t>(perm_idx_ar);
    program = program_ar;
    // Checked before the Info is read (that registers the program's SRT walker code).
    if (binary_version != Serialization::ShaderBinaryVersionOf(spec.runtime_info)) {
        return Reject::StageStale; // this stage's SPIR-V changed since (a revision bump)
    }
    if (!info.Deserialize(ar) || ar.Failed() ||
        (Serialization::g_perm_ids && info.pgm_hash != program_ar)) {
        return Reject::Corrupt;
    }

    fetch_shader_data = spec.fetch_shader_data;
    return Reject::None;
}

void ComputePipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};
    key.Write(value);
}

bool ComputePipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};
    key.Read(value);
    return true;
}

void ComputePipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    // Nothing here yet
    return;
}

bool ComputePipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    // Nothing here yet
    return true;
}

bool PipelineCache::LoadComputePipeline(Serialization::Archive& ar) {
    const auto reset = [&] {
        selection.infos.fill(nullptr);
        selection.modules.fill(nullptr);
        selection.fetch_shader.reset();
    };
    reset();
    load_reject = Reject::None;
    load_other_mode = false;

    compute_key.Deserialize(ar);

    ComputePipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);
    if (ar.Failed()) {
        load_reject = Reject::Corrupt;
        return false;
    }
    if (compute_pipelines.find(compute_key) != compute_pipelines.end()) {
        load_reject = Reject::Duplicate;
        return false;
    }

    std::vector<u8> meta_blob;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                       fmt::format("{:#018x}", compute_key.value), meta_blob);
    if (meta_blob.empty()) {
        load_reject = Reject::StageMissing;
        return false;
    }

    Serialization::Archive meta_ar{std::move(meta_blob)};

    if (!LoadPipelineStage(meta_ar, 0, compute_key.value)) {
        reset();
        return false;
    }
    if (load_other_mode) {
        load_reject = Reject::OtherMode;
        reset();
        return false;
    }

    const auto [it, is_new] = compute_pipelines.try_emplace(compute_key);

    if (preload_jobs != nullptr) {
        // Created by the warm-up's workers; the placeholder marks the key as taken.
        auto& job = preload_jobs->emplace_back();
        job.is_compute = true;
        job.compute_key = compute_key;
        job.infos = selection.infos;
        job.modules = selection.modules;
        job.compute_sdata = std::move(sdata);
    } else {
        it.value() = std::make_unique<ComputePipeline>(instance, scheduler, desc_heap, profile,
                                                       *pipeline_cache, compute_key, *selection.infos[0],
                                                       selection.modules[0], sdata, true);
    }

    selection.infos.fill(nullptr);
    selection.modules.fill(nullptr);

    return true;
}

void GraphicsPipelineKey::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer key{ar};

    key.Write(this, sizeof(*this));
}

bool GraphicsPipelineKey::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader key{ar};

    key.Read(this, sizeof(*this));
    return true;
}

void GraphicsPipeline::SerializationSupport::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer sdata{ar};

    sdata.Write(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Write(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Write(&divisors, sizeof(divisors));
    sdata.Write(multisampling);
    sdata.Write(tcs);
    sdata.Write(tes);
    sdata.Write(fragment); // clip-distance emulation discard shader (when the pipeline has no FS)
}

bool GraphicsPipeline::SerializationSupport::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader sdata{ar};

    sdata.Read(&vertex_attributes, sizeof(vertex_attributes));
    sdata.Read(&vertex_bindings, sizeof(vertex_bindings));
    sdata.Read(&divisors, sizeof(divisors));
    sdata.Read(multisampling);
    sdata.Read(tcs);
    sdata.Read(tes);
    sdata.Read(fragment);
    if (ar.Failed()) {
        return false;
    }
    // The auxiliary shaders (rect/quad-list tessellation, the clip-distance discard shader) are
    // handed to the driver as they are: they must be SPIR-V.
    using Vulkan::PipelineCachePolicy::LooksLikeSpirv;
    for (const auto* aux : {&tcs, &tes, &fragment}) {
        if (!aux->empty() && !LooksLikeSpirv(*aux)) {
            ar.Fail();
            return false;
        }
    }
    return true;
}

bool PipelineCache::LoadGraphicsPipeline(Serialization::Archive& ar) {
    // The stages of a pipeline that did not load must never reach the next one (a pipeline
    // without a fragment program would get the previous one's).
    const auto reset = [&] {
        selection.infos.fill(nullptr);
        selection.modules.fill(nullptr);
        load_runtime_infos = {};
        selection.fetch_shader.reset();
    };
    reset();
    load_reject = Reject::None;
    load_other_mode = false;
    load_motion_idle = false;

    selection.graphics_key.Deserialize(ar);

    GraphicsPipeline::SerializationSupport sdata{};
    sdata.Deserialize(ar);
    if (ar.Failed()) {
        load_reject = Reject::Corrupt;
        return false;
    }
    if (graphics_pipelines.find(selection.graphics_key) != graphics_pipelines.end()) {
        load_reject = Reject::Duplicate;
        return false;
    }

    // Each stage's program is kept when it loads, also when another stage of this pipeline is
    // stale (a scoped revision bump): the pipeline compiles on first use, its other programs do
    // not. The fragment stage loads first, so a stale one does not stop the stages after it.
    bool stale = false;
    for (int stage_idx = 0; stage_idx < MaxShaderStages; ++stage_idx) {
        const auto& hash = selection.graphics_key.stage_hashes[stage_idx];
        if (!hash) {
            continue;
        }

        std::vector<u8> meta_blob;
        Storage::DataBase::Instance().Load(Storage::BlobType::ShaderMeta,
                                           fmt::format("{:#018x}", hash), meta_blob);
        if (meta_blob.empty()) {
            load_reject = Reject::StageMissing;
            reset();
            return false;
        }

        Serialization::Archive meta_ar{std::move(meta_blob)};

        if (!LoadPipelineStage(meta_ar, stage_idx, hash)) {
            if (load_reject == Reject::StageStale) {
                stale = true;
                continue;
            }
            reset();
            return false;
        }
    }
    if (stale) {
        load_reject = Reject::StageStale;
        reset();
        return false;
    }
    if (load_other_mode) {
        load_reject = Reject::OtherMode;
        reset();
        return false;
    }
    if (load_motion_idle) {
        load_reject = Reject::MotionIdle;
        reset();
        return false;
    }

    const auto [it, is_new] = graphics_pipelines.try_emplace(selection.graphics_key);

    if (preload_jobs != nullptr) {
        // Created by the warm-up's workers; the placeholder marks the key as taken.
        auto& job = preload_jobs->emplace_back();
        job.graphics_key = selection.graphics_key;
        job.infos = selection.infos;
        job.runtime_infos = load_runtime_infos;
        job.modules = selection.modules;
        job.fetch_shader = selection.fetch_shader;
        job.graphics_sdata = std::move(sdata);
    } else {
        it.value() = std::make_unique<GraphicsPipeline>(
            instance, scheduler, desc_heap, profile, selection.graphics_key, *pipeline_cache, selection.infos,
            load_runtime_infos, selection.fetch_shader, selection.modules, sdata, true);
    }

    selection.infos.fill(nullptr);
    selection.modules.fill(nullptr);
    selection.fetch_shader.reset();

    return true;
}

bool PipelineCache::LoadPipelineStage(Serialization::Archive& ar, size_t stage,
                                      u64 expected_id) {
    auto program = std::make_unique<Program>();
    Shader::StageSpecialization spec{};
    spec.info = &program->info;
    size_t perm_idx{};
    u64 program_hash{};
    std::optional<Shader::Gcn::FetchShaderData> stage_fetch{};
    load_reject = LoadShaderMeta(ar, program->info, stage_fetch, spec, perm_idx, expected_id,
                                 program_hash);
    if (load_reject != Reject::None) {
        // Another build's: its translation in play says so (vk_compile_reason.h; logs only), by
        // its id, and a new program's (whose id needs the translation) by HashCombine(program, 0).
        if constexpr (Common::Log::Enabled) {
            if (load_reject == Reject::StageStale) {
                compile_reasons.StaleStage(expected_id);
                if (program_hash != 0) {
                    compile_reasons.StaleStage(HashCombine(program_hash, u64{0}));
                }
            }
        }
        return false;
    }

    // Device addresses are never valid across executions: a motion permutation is cached only
    // with VrPushData, as its flag alone (the draw pushes the addresses).
    if (spec.runtime_info.vr_motion.Baked() ||
        (spec.runtime_info.vr_motion.enabled && !profile.SupportsVrPushData())) {
        load_reject = Reject::Corrupt;
        return false;
    }
    // Flat and native stereo share the cache (NativeStereo::Enabled() is fixed per process). The
    // other mode's permutation still takes its index here, so the ones this session adds never
    // reuse it, but its pipeline is not built: it can never be selected, and its push range
    // would be this session's.
    if (spec.runtime_info.native_stereo != NativeStereo::Enabled() ||
        spec.runtime_info.billboard_face_eye != Shader::Optimization::BillboardFaceEye::Enabled(
            NativeStereo::Enabled(), VR::IsEnabled(), std::getenv("VR_BILLBOARD_FACE_EYE"))) {
        load_other_mode = true;
    }
    // Object motion runs only with an upscaler (VrObjectMotion::Begin). A session started without
    // one loads a motion variant's permutations the same way but not its pipeline: an upscaler
    // turned on later creates it on first use, with nothing to translate.
    if (spec.runtime_info.vr_motion.enabled &&
        VR::Graphics::Accepted().upscaler == VR::Graphics::Upscaler::Off) {
        load_motion_idle = true;
    }
    std::vector<u32> spv{};
    Storage::DataBase::Instance().Load(
        Storage::BlobType::ShaderBinary,
        Serialization::BinaryName(expected_id, program->info.pgm_hash, perm_idx), spv);
    if (spv.empty()) {
        load_reject = Reject::StageMissing;
        return false;
    }
    if (!Vulkan::PipelineCachePolicy::LooksLikeSpirv(spv)) {
        load_reject = Reject::Corrupt;
        return false;
    }

    // The meta is named by the key's stage hash (the permutation's id with FAST_CPU_PERM_IDS, else
    // HashCombine(program, index)); the program's list still puts it at the index it had when it
    // was written (the lookups find the first equal specialization, in the same order as in that
    // session), and the pipeline keys name it by that hash.

    vk::ShaderModule module{};

    const u64 vertex_output_locations = program->info.vertex_output_locations;
    std::unique_lock program_lock(programs_mutex);
    auto [it_pgm, new_program] = program_cache.try_emplace(program->info.pgm_hash);
    if (new_program) {
        module = CompileSPV(spv, instance.GetDevice());
        it_pgm.value() = std::move(program);
    } else {
        const auto& modules = it_pgm.value()->modules;
        if (perm_idx < modules.size() && modules[perm_idx].spec.Valid() &&
            modules[perm_idx].id == expected_id) {
            // The same file another key loaded: the same permutation.
            module = modules[perm_idx].module;
        } else if (const auto& it = std::ranges::find(modules, spec, &Program::Module::spec);
                   it != modules.end() ||
                   (perm_idx < modules.size() && modules[perm_idx].spec.Valid()) ||
                   it_pgm.value()->HasId(expected_id)) {
            // A matching permutation is valid only at its original index and under its own id,
            // and another permutation at its index or under its id is never replaced. Either means
            // the store holds entries from more than one cache generation (or, for the id, a
            // 64-bit collision), so this pipeline is left to compile at runtime.
            LOG_WARNING(Render_Vulkan,
                        "Cached permutation {} ({:#018x}) of {}_{:x} conflicts with {}, skipping "
                        "preload",
                        perm_idx, expected_id, program->info.stage, program->info.pgm_hash,
                        it != modules.end()
                            ? fmt::format("index {} ({:#018x})", std::distance(modules.begin(), it),
                                          it->id)
                            : std::string{"another permutation at its index or id"});
            load_reject = Reject::Conflict;
            return false;
        } else {
            module = CompileSPV(spv, instance.GetDevice());
        }
    }
    ++programs_generation;
    // Like a compile's selection: the pipeline is created with its stages' own runtime infos.
    load_runtime_infos[stage] = spec.runtime_info;
    it_pgm.value()->InsertPermut(module, std::move(spec), perm_idx, expected_id,
                                vertex_output_locations);

    selection.infos[stage] = &it_pgm.value()->info;
    selection.modules[stage] = module;
    // Like the draw path: only a stage that has a fetch shader sets it (a geometry stage after
    // the export stage must not clear the vertex fetch).
    if (stage_fetch) {
        selection.fetch_shader = stage_fetch;
    }

    return true;
}

namespace {
// Fast bit 40 in the start mask. The warm-up runs before the rasterizer calls
// FastRender::Start, so FAST_TOGGLES is parsed here as Start parses it (fast_render.cpp
// ParseMask); without it, the current mask (the default one until Start).
bool AsyncPipelinesAtStart() {
    u64 mask = FastRender::g_mask.load(std::memory_order_relaxed);
    if (const char* env = std::getenv("FAST_TOGGLES")) {
        const std::string text{env};
        if (const size_t at = text.find_first_not_of(" \t\r\n"); at != std::string::npos) {
            try {
                size_t used = 0;
                const u64 parsed = std::stoull(text.substr(at), &used, 0);
                if (used > 0) {
                    mask = parsed;
                }
            } catch (...) {
            }
        }
    }
    return ((mask >> AsyncPipelinesBit) & 1) != 0;
}
} // namespace

void PipelineCache::WarmUp() {
    // Fast bit 40 (vk_pipeline_async.cpp) decides here from the start mask.
    const bool async_bit = AsyncPipelinesAtStart();
    if (!EmulatorSettings.IsPipelineCacheEnabled()) {
        // bbport keeps its pipeline cache on by default (shim emulator_settings.h:64); with bit
        // 40 so do we, unless FAST_ASYNC_PIPELINE_CACHE=0.
        const char* keep = std::getenv("FAST_ASYNC_PIPELINE_CACHE");
        if (!async_bit || (keep && keep[0] == '0')) {
            return;
        }
        LOG_INFO(Render, "FAST async_pipelines: pipeline cache on (bit 40; the setting is off, "
                         "FAST_ASYNC_PIPELINE_CACHE=0 follows it)");
    }

    // Runs once at startup, before the game's first frame (the PipelineCache constructor): every
    // pipeline the cache holds is created here, so none of them compiles during play.
    const auto start = std::chrono::steady_clock::now();
    auto& db = Storage::DataBase::Instance();
    db.Open();
    const bool archived = EmulatorSettings.IsPipelineCacheArchived();

    const auto save_profile = [&] {
        std::vector<u8> data(sizeof(profile));
        std::memcpy(data.data(), &profile, sizeof(profile));
        db.Save(Storage::BlobType::ShaderProfile, "profile", std::move(data));
    };

    // Check if cache is compatible
    std::vector<u8> profile_data{};
    db.Load(Storage::BlobType::ShaderProfile, "profile", profile_data);
    if (profile_data.empty()) {
        db.FinishPreload();
        save_profile();
        LOG_INFO(Render, "Preloaded 0 pipelines (no cache yet: this session writes it)");
        return;
    }
    bool compatible = profile_data.size() == sizeof(Shader::Profile);
    if (compatible) {
        Shader::Profile cached_profile{};
        std::memcpy(&cached_profile, profile_data.data(), sizeof(cached_profile));
        compatible = cached_profile == profile;
    }
    if (!compatible) {
        // Written for another GPU / driver profile (or another profile layout). Before, the cache
        // was then ignored for good (closed: nothing written either, every session compiled
        // everything). Now it starts over and this session writes it for this profile.
        if (!archived && db.Clear()) {
            LOG_WARNING(Render, "Pipeline cache was written for another GPU / driver profile: "
                                "cleared, this session writes it again");
            db.FinishPreload();
            save_profile();
            LOG_INFO(Render, "Preloaded 0 pipelines (the cache starts over)");
            return;
        }
        LOG_WARNING(Render,
                    "Pipeline cache isn't compatible with current system. Ignoring the cache");
        db.Close();
        return;
    }

    using namespace PipelineCachePolicy;
    u32 num_pipelines{};
    u32 num_total_pipelines{};
    std::array<u32, static_cast<size_t>(Reject::Count)> rejects{};
    std::vector<std::string> doomed{};
    std::unordered_set<u64> named{};
    std::vector<PreloadJob> jobs{};
    preload_jobs = &jobs;

    db.ForEachBlob(Storage::BlobType::PipelineKey,
                   [&](const std::string& name, std::vector<u8>&& data) {
                       ++num_total_pipelines;

                       Serialization::Archive ar{std::move(data)};
                       Serialization::Reader pldata{ar};

                       u32 version{};
                       pldata.Read(version);
                       u32 is_compute{};
                       pldata.Read(is_compute);

                       Reject why = Reject::None;
                       if (ar.Failed()) {
                           why = Reject::Corrupt;
                       } else if (version != Serialization::PipelineKeyVersion) {
                           // FAST_CPU_PERM_IDS: the other naming's key stays for its sessions.
                           why = version == Serialization::OtherNamingKeyVersion
                                     ? Reject::OtherNaming
                                     : Reject::KeyVersion;
                       } else if (!(is_compute ? LoadComputePipeline(ar)
                                               : LoadGraphicsPipeline(ar))) {
                           why = load_reject == Reject::None ? Reject::Corrupt : load_reject;
                       }
                       // The permutations a key that stays on disk names (FAST_CPU_CACHE_GC
                       // keeps their metas and binaries).
                       if (g_cache_gc && KeptKey(why)) {
                           if (is_compute) {
                               named.insert(compute_key.value);
                           } else {
                               for (const auto id : selection.graphics_key.stage_hashes) {
                                   if (id != 0) {
                                       named.insert(id);
                                   }
                               }
                           }
                       }

                       if (why == Reject::None) {
                           ++num_pipelines;
                           return;
                       }
                       ++rejects[static_cast<size_t>(why)];
                       if (PruneRejected(why, archived)) {
                           doomed.push_back(name);
                       }
                       // Its compile in play says why it was not preloaded (vk_compile_reason.h;
                       // logs only). A duplicate's key is loaded.
                       if constexpr (Common::Log::Enabled) {
                           const u64 hash = CompileReason::PipelineBlobHash(name);
                           if (hash != 0 && why != Reject::Duplicate) {
                               compile_reasons.NotPreloaded(hash, RejectName(why));
                           }
                       }
                   });
    selection.infos.fill(nullptr);
    selection.modules.fill(nullptr);
    selection.fetch_shader.reset();
    preload_jobs = nullptr;
    const auto loaded_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::steady_clock::now() - start)
                               .count();

    // The driver compiles: the slow part (tens of ms each when the driver's own cache misses), on
    // worker threads. The pipelines only read their programs (loaded above, not changed again
    // here) and create Vulkan objects; the warm-up waits for all of them before it returns, so
    // nothing else runs meanwhile.
    const u32 hw = std::max(1u, std::thread::hardware_concurrency());
    const u32 num_workers = std::clamp<u32>(hw > 2 ? hw - 2 : 1, 1u, 8u);
    {
        std::atomic<size_t> next{0};
        const auto work = [&] {
            // Bit 40: a pipeline the driver rejects is left without a handle, pruned below.
            g_tolerate_pipeline_rejection = async_bit;
            for (size_t i = next++; i < jobs.size(); i = next++) {
                auto& job = jobs[i];
                if (job.is_compute) {
                    job.compute = std::make_unique<ComputePipeline>(
                        instance, scheduler, desc_heap, profile, *pipeline_cache, job.compute_key,
                        *job.infos[0], job.modules[0], job.compute_sdata, true);
                } else {
                    job.graphics = std::make_unique<GraphicsPipeline>(
                        instance, scheduler, desc_heap, profile, job.graphics_key, *pipeline_cache,
                        job.infos, job.runtime_infos, job.fetch_shader, job.modules,
                        job.graphics_sdata, true);
                }
            }
            g_tolerate_pipeline_rejection = false;
        };
        std::vector<std::jthread> workers{};
        for (u32 w = 1; w < num_workers && w < jobs.size(); ++w) {
            workers.emplace_back(work);
        }
        work(); // this thread too
    } // joined
    // Bit 40: a pipeline the driver rejected (bbport throws CorruptData and rebuilds its whole
    // cache, BBV vk_graphics_pipeline.cpp:424-431, vk_pipeline_serialization.cpp:382-446) is
    // pruned instead: its key and its stages' meta and SPIR-V are deleted, its placeholder is
    // removed, and the cached modules it was built from are dropped for this session (GetProgram
    // compiles such a permutation again at its own index), so no draw can reach them.
    std::vector<std::pair<Storage::BlobType, std::string>> pruned{};
    std::vector<vk::ShaderModule> rejected_modules{};
    u32 rejected_graphics{}, rejected_compute{};
    const auto prune_stage = [&](const Shader::Info* info, vk::ShaderModule module, u64 perm_id) {
        if (!info || !module) {
            return;
        }
        // The meta is named by the key's stage hash; with FAST_CPU_PERM_IDS so is the binary,
        // else it is "<program>_<index>" at every index the module is at, as before.
        pruned.emplace_back(Storage::BlobType::ShaderMeta, Serialization::BlobName(perm_id));
        if (Serialization::g_perm_ids) {
            pruned.emplace_back(Storage::BlobType::ShaderBinary, Serialization::BlobName(perm_id));
        } else if (const auto program = program_cache.find(info->pgm_hash);
                   program != program_cache.end()) {
            const auto& modules = program->second->modules;
            for (size_t perm = 0; perm < modules.size(); ++perm) {
                if (modules[perm].module == module) {
                    pruned.emplace_back(Storage::BlobType::ShaderBinary,
                                        Serialization::BinaryName(perm_id, info->pgm_hash, perm));
                }
            }
        }
        if (std::ranges::find(rejected_modules, module) == rejected_modules.end()) {
            rejected_modules.push_back(module);
        }
    };
    for (auto& job : jobs) {
        if (job.is_compute) {
            if (async_bit && job.compute && !job.compute->Handle()) {
                ++rejected_compute;
                compute_pipelines.erase(job.compute_key);
                pruned.emplace_back(Storage::BlobType::PipelineKey,
                                    fmt::format("c_{:#018x}", job.compute_key.value));
                prune_stage(job.infos[0], job.modules[0], job.compute_key.value);
                continue;
            }
            compute_pipelines.find(job.compute_key).value() = std::move(job.compute);
        } else {
            if (async_bit && job.graphics && !job.graphics->Handle()) {
                ++rejected_graphics;
                graphics_pipelines.erase(job.graphics_key);
                const u64 hash = std::hash<GraphicsPipelineKey>{}(job.graphics_key);
                pruned.emplace_back(Storage::BlobType::PipelineKey,
                                    fmt::format("g_{:#018x}", hash));
                if constexpr (Common::Log::Enabled) {
                    compile_reasons.NotPreloaded(hash, "the driver rejected it");
                }
                for (u32 stage = 0; stage < MaxShaderStages; ++stage) {
                    prune_stage(job.infos[stage], job.modules[stage],
                                job.graphics_key.stage_hashes[stage]);
                }
                continue;
            }
            graphics_pipelines.find(job.graphics_key).value() = std::move(job.graphics);
        }
    }
    if (rejected_graphics + rejected_compute != 0) {
        // Nothing has selected a program yet: no draw, memo or worker holds these modules.
        u32 dropped{};
        if (!rejected_modules.empty()) {
            std::unique_lock program_lock(programs_mutex);
            const auto device = instance.GetDevice();
            for (const auto& [_, program] : program_cache) {
                for (auto& permutation : program->modules) {
                    if (permutation.module &&
                        std::ranges::find(rejected_modules, permutation.module) !=
                            rejected_modules.end()) {
                        device.destroyShaderModule(permutation.module);
                        permutation.module = vk::ShaderModule{};
                        ++dropped;
                    }
                }
            }
            ++programs_generation;
            PerfMemo::BumpEpoch();
        }
        u32 removed_blobs{};
        for (const auto& [type, name] : pruned) {
            removed_blobs += db.Remove(type, name) ? 1 : 0;
        }
        LOG_WARNING(Render,
                    "FAST async_pipelines: the driver rejected {} cached graphics and {} compute "
                    "pipelines: pruned ({} of {} entries deleted), {} cached modules dropped "
                    "(compiled again on first use)",
                    rejected_graphics, rejected_compute, removed_blobs, pruned.size(), dropped);
    }

    // A rejected key can never load: deleted, so later sessions neither read nor report it (its
    // pipeline writes a fresh key when it compiles again).
    u32 removed{};
    for (const auto& name : doomed) {
        removed += db.Remove(Storage::BlobType::PipelineKey, name) ? 1 : 0;
    }

    // FAST_CPU_CACHE_GC: then the metas and binaries no permutation of this session is named by
    // (the earlier names, the stale and corrupt ones, the ones only a pruned key named, the ones no
    // key ever named). Every permutation the warm-up holds stays, also one whose pipeline did not
    // load (a pipeline compiled again from it this session writes a key that names it): a meta
    // only pruned keys name goes the session after. Not in the archived (zip) mode.
    if (g_cache_gc && !archived) {
        std::unordered_set<u64> live{named};
        u32 held{};
        {
            std::shared_lock program_lock(programs_mutex);
            for (const auto& [pgm_hash, program] : program_cache) {
                for (const auto& permutation : program->modules) {
                    if (permutation.spec.Valid()) {
                        live.insert(permutation.id);
                        ++held;
                    }
                }
            }
        }
        if (g_cache_gc_verify) {
            // Every id a kept key names is a permutation the warm-up holds, and no program holds
            // one twice (the keys would not tell its permutations apart).
            u32 mismatches{};
            std::unordered_set<u64> held_ids{};
            {
                std::shared_lock program_lock(programs_mutex);
                for (const auto& [pgm_hash, program] : program_cache) {
                    std::unordered_set<u64> ids{};
                    for (const auto& permutation : program->modules) {
                        if (!permutation.spec.Valid()) {
                            continue;
                        }
                        held_ids.insert(permutation.id);
                        if (!ids.insert(permutation.id).second && ++mismatches <= 8) {
                            LOG_ERROR(Render, "FAST_CPU_CACHE_GC verify: program {:#x} holds the "
                                              "id {:#018x} twice",
                                      pgm_hash, permutation.id);
                        }
                    }
                }
            }
            for (const u64 id : named) {
                if (!held_ids.contains(id) && ++mismatches <= 8) {
                    LOG_ERROR(Render, "FAST_CPU_CACHE_GC verify: a kept key names {:#018x}, no "
                                      "loaded permutation is",
                              id);
                }
            }
            LOG_INFO(Render,
                     "FAST_CPU_CACHE_GC verify: {} ids named by the kept keys, {} permutations "
                     "held, {} mismatches",
                     named.size(), held, mismatches);
        }
        std::vector<std::pair<Storage::BlobType, std::string>> unnamed{};
        db.ForEachName([&](Storage::BlobType type, const std::string& name) {
            const bool shader = type == Storage::BlobType::ShaderMeta ||
                                type == Storage::BlobType::ShaderBinary;
            if (shader && CollectUnnamed(name, archived, [&](u64 id) { return live.contains(id); })) {
                unnamed.emplace_back(type, name);
            }
        });
        u32 metas{}, binaries{};
        for (const auto& [type, name] : unnamed) {
            if (db.Remove(type, name)) {
                ++(type == Storage::BlobType::ShaderMeta ? metas : binaries);
            }
        }
        if (metas + binaries != 0) {
            LOG_INFO(Render,
                     "Pipeline cache: deleted {} shader metas and {} binaries no permutation of "
                     "this session is named by ({} held)",
                     metas, binaries, held);
        }
    }

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    LOG_INFO(Render,
             "Preloaded {} pipelines in {} ms ({} programs; the files read in {} ms, the "
             "pipelines created on {} threads)",
             num_pipelines, ms, program_cache.size(), loaded_ms, num_workers);
    if (num_total_pipelines > num_pipelines) {
        std::string why{};
        for (size_t i = 1; i < rejects.size(); ++i) {
            if (rejects[i] != 0) {
                why += fmt::format("{}{} {}", why.empty() ? "" : ", ", rejects[i],
                                   RejectName(static_cast<Reject>(i)));
            }
        }
        // The other mode's keys are not stale: they stay for that mode's sessions. Nor are the
        // motion variants' of a session without an upscaler (their programs are loaded).
        std::string kept{};
        if (const u32 n = rejects[static_cast<size_t>(Reject::OtherMode)]) {
            kept += fmt::format(" (the other mode's {} are kept for its sessions)", n);
        }
        if (const u32 n = rejects[static_cast<size_t>(Reject::MotionIdle)]) {
            kept += fmt::format(" (the {} object-motion variants are kept for an upscaler)", n);
        }
        if (const u32 n = rejects[static_cast<size_t>(Reject::OtherNaming)]) {
            kept += fmt::format(" (the other permutation naming's {} are kept for its sessions: "
                                "FAST_CPU_PERM_IDS={})",
                                n, Serialization::g_perm_ids ? 0 : 1);
        }
        LOG_WARNING(Render,
                    "{} cached pipelines were not preloaded ({}); {} removed from the cache: they "
                    "compile on first use and are cached again{}",
                    num_total_pipelines - num_pipelines, why, removed, kept);
    }

    db.FinishPreload();
}

void PipelineCache::Sync() {
    Storage::DataBase::Instance().Close();
}

} // namespace Vulkan

namespace Shader {

void Info::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer info{ar};

    info.Write(this, sizeof(InfoPersistent));
    info.Write(flattened_ud_buf);
    srt_info.Serialize(ar);
}

bool Info::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader info{ar};

    info.Read(this, sizeof(Shader::InfoPersistent));
    info.Read(flattened_ud_buf);
    if (ar.Failed()) {
        return false;
    }

    return srt_info.Deserialize(ar);
}

void Gcn::FetchShaderData::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer fetch{ar};
    ar.Grow(6 + attributes.size() * sizeof(VertexAttribute));

    fetch.Write(size);
    fetch.Write(vertex_offset_sgpr);
    fetch.Write(instance_offset_sgpr);
    fetch.Write(attributes);
}

bool Gcn::FetchShaderData::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader fetch{ar};

    fetch.Read(size);
    fetch.Read(vertex_offset_sgpr);
    fetch.Read(instance_offset_sgpr);
    fetch.Read(attributes);

    return true;
}

void PersistentSrtInfo::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer srt{ar};

    srt.Write(this, sizeof(*this));
    if (walker_func_size) {
        srt.Write(reinterpret_cast<void*>(walker_func), walker_func_size);
    }
}

bool PersistentSrtInfo::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader srt{ar};

    srt.Read(this, sizeof(*this));

    // The pointer read above is the writing process's: never kept.
    walker_func = nullptr;
    if (ar.Failed() || walker_func_size > ar.Remaining()) {
        walker_func_size = 0;
        ar.Fail();
        return false;
    }
    if (walker_func_size) {
        walker_func = RegisterWalkerCode(ar.CurrPtr(), walker_func_size);
        ar.Advance(walker_func_size);
    }

    return true;
}

void StageSpecialization::Serialize(Serialization::Archive& ar) const {
    Serialization::Writer spec{ar};

    spec.Write(start);
    spec.Write(runtime_info);

    spec.Write(bitset.to_string());

    if (fetch_shader_data) {
        spec.Write(sizeof(*fetch_shader_data));
        fetch_shader_data->Serialize(ar);
    } else {
        spec.Write(size_t{0});
    }

    spec.Write(vs_attribs);
    spec.Write(buffers);
    spec.Write(images);
    spec.Write(fmasks);
    spec.Write(samplers);
}

bool StageSpecialization::Deserialize(Serialization::Archive& ar) {
    Serialization::Reader spec{ar};

    spec.Read(start);
    spec.Read(runtime_info);

    std::string bits{};
    spec.Read(bits);
    // std::bitset throws on anything but '0' / '1': a corrupted meta fails instead.
    if (ar.Failed() || bits.find_first_not_of("01") != std::string::npos) {
        ar.Fail();
        return false;
    }
    bitset = std::bitset<MaxStageResources>(bits);

    u64 fetch_data_size{};
    spec.Read(fetch_data_size);

    if (fetch_data_size) {
        Gcn::FetchShaderData fetch_data;
        fetch_data.Deserialize(ar);
        fetch_shader_data = fetch_data;
    }

    spec.Read(vs_attribs);
    spec.Read(buffers);
    spec.Read(images);
    spec.Read(fmasks);
    spec.Read(samplers);

    return !ar.Failed();
}

} // namespace Shader
