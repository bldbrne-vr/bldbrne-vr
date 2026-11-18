// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// The startup shader build (vk_shader_record.h): after the warm-up has loaded the player's own
// cache, every permutation of the shader list that the session can select is translated from
// the game's archives and added - to the programs in memory and to the disk cache - unless the
// player's cache already holds an equal one; then every pipeline of the list whose stages are
// all there is created, as the warm-up creates its pipelines, and written to the disk cache (a
// compute permutation is its own pipeline). The next start's warm-up loads all of it like any
// other cached work, so a list entry is built once per player (and again after an update that
// retires the cached translations).
//
// The list's permutation ids are its recording sessions' content ids (FAST_CPU_PERM_IDS); here
// each gets the player's name for it (the next index for its program, or its id), and the list's
// pipeline keys are renamed to match, so the player's cache and the list never collide.
//
// A start whose cache already holds the list (the marker the build left in the cache: the list's
// id - a hash of this session's selection of it - with every permutation's new name) loads the
// permutations the warm-up did not (it loads what a cached pipeline uses) from the cache and
// translates only those the cache lost; with nothing to build it reads no key either (the list's
// pipelines are in the cache since that build). The emulator build is not in the id: a build
// that translates a shader differently retires its cached permutation (the cache's version
// words), and only that one is built again. A list that grew reuses the marker's names and
// builds only its new entries.
//
// The list (records.bin and keys/*.key) is the shader_list folder beside the emulator, or
// VR_SHADER_LIST=<folder> ("0": none), or a recording session's own folder (VR_SHADER_RECORD:
// there each added permutation keeps the list's id, so the keys the session records name it as
// the list does); without a records.bin there nothing runs.
//
// The Old Hunters' areas have a list of their own, shader_list_dlc beside the emulator (or
// VR_SHADER_LIST_DLC=<folder>, "0": none), built after the game's the same way, with its own
// marker, and only when the add-on is installed (or built into the edition): a player without
// it never reads that list. It holds every permutation its pipelines use, so it stands alone.
// None while recording (the session's folder is its list) or with VR_SHADER_LIST=0.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <SDL3/SDL_filesystem.h>
#include <xxhash.h>

#include "video_core/native_stereo/native_stereo.h"
#include "common/addon_content.h"
#include "common/hash.h"
#include "common/logging/log.h"
#include "common/scm_rev.h"
#include "common/singleton.h"
#include "core/emulator_settings.h"
#include "core/file_format/psf.h"
#include "core/vr/graphics_settings.h"
#include "core/vr/vr_session.h"
#include "shader_recompiler/ir/passes/billboard_face_eye.h"
#include "shader_recompiler/ir/passes/light_shadow.h"
#include "shader_recompiler/ir/passes/ambient_occlusion.h"
#include "shader_recompiler/ir/passes/moon_shadow.h"
#include "shader_recompiler/ir/passes/reflections.h"
#include "shader_recompiler/ir/passes/bounce_light.h"
#include "shader_recompiler/ir/passes/fog_light.h"
#include "shader_recompiler/ir/passes/cinematic_look.h"
#include "shader_recompiler/ir/passes/parallax.h"
#include "shader_recompiler/ir/passes/particle_air.h"
#include "shader_recompiler/ir/passes/moon_shafts.h"
#include "shader_recompiler/ir/passes/specular_aa.h"
#include "shader_recompiler/ir/passes/wet_blood.h"
#include "video_core/cache_storage.h"
#include "video_core/startup_compile.h"
#include "video_core/pipeline_cache_policy.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_shader_record.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"
#include "video_core/shader_archive.h"

namespace Vulkan {
namespace {

namespace Startup = VideoCore::StartupCompile;

// A permutation this session can select (the warm-up's rules, LoadPipelineStage): this stereo
// mode, this billboard rule, no baked motion buffers, and object motion only with an upscaler.
// `hash`: its program's (pgm_hash), for the checks whose key bits mean another thing on another
// program (the light composite's bounce bit is the air pass's own bit on its programs, the
// parallax bit their fog grid's).
bool Selectable(const Shader::RuntimeInfo& runtime_info, u64 hash, const Shader::Profile& profile) {
    if (runtime_info.native_stereo != NativeStereo::Enabled() ||
        runtime_info.billboard_face_eye !=
            Shader::Optimization::BillboardFaceEye::Enabled(
                NativeStereo::Enabled(), VR::IsEnabled(), std::getenv("VR_BILLBOARD_FACE_EYE"))) {
        return false;
    }
    if (runtime_info.native_light_scissor && runtime_info.vr_light_shadows != VrLightShadowLevel()) {
        return false;
    }
    // A permutation of the game's AO pass made with ours, in a session without it; a mask-only one
    // (ambient_occlusion.h MaskOnly) in a session that is not.
    if (const u8 ao = runtime_info.vr_ambient_occlusion; ao & Shader::RuntimeInfo::VrAmbientOcclusionBit) {
        const bool upscaler = VR::Graphics::Accepted().upscaler != VR::Graphics::Upscaler::Off;
        const bool ours = Shader::Optimization::AmbientOcclusion::Enabled(
            VR::IsEnabled(), VR::GetSettings().ambient_occlusion, upscaler,
            std::getenv("VR_AMBIENT_OCCLUSION"));
        const bool mask_only = Shader::Optimization::AmbientOcclusion::MaskOnly(
            ours, VrLampShadowsOn(), Shader::Optimization::LightShadow::SceneMaps::Mask());
        // ... and made with the lamp shadows' work or without it (RuntimeInfo::VrAoLampShadows),
        // in a session that is the other way.
        const bool lamps = Shader::Optimization::LightShadow::SceneMaps::Enabled() && VrLampShadowsOn();
        if (((ao & Shader::RuntimeInfo::VrAoMaskOnly) ? !mask_only : !ours) ||
            ((ao & Shader::RuntimeInfo::VrAoLampShadows) != 0) != lamps) {
            return false;
        }
    }
    // A permutation of the game's moon-shadow mask made with ours, in a session without it.
    if ((runtime_info.vr_ambient_occlusion & Shader::RuntimeInfo::VrMoonShadows) &&
        !Shader::Optimization::MoonShadow::Enabled(
            VR::IsEnabled(), VR::GetSettings().moon_shadows,
            VR::Graphics::Accepted().upscaler != VR::Graphics::Upscaler::Off,
            std::getenv("VR_MOON_SHADOWS"))) {
        return false;
    }
    // A permutation of the ambient composite with our specular occlusion, in a session without it.
    if ((runtime_info.vr_ambient_occlusion & Shader::RuntimeInfo::VrSpecularOcclusion) &&
        !Shader::Optimization::AmbientOcclusion::SpecEnabled(
            Shader::Optimization::AmbientOcclusion::Enabled(
                VR::IsEnabled(), VR::GetSettings().ambient_occlusion,
                VR::Graphics::Accepted().upscaler != VR::Graphics::Upscaler::Off,
                std::getenv("VR_AMBIENT_OCCLUSION")),
            std::getenv("VR_SPEC_OCCLUSION"))) {
        return false;
    }
    // (The two reflection checks below are for the reflection programs only: as in
    // LoadPipelineStage, an AO program carries VrAmbientOcclusionBit, and on it bit 16 is
    // VrAoLampShadows and bit 8 VrAoMaskOnly. Until 2026-11-17 an AO permutation with the lamp
    // shadows was left out of the start-up build with wet streets off or with our AO off.)
    const bool ao_program =
        (runtime_info.vr_ambient_occlusion & Shader::RuntimeInfo::VrAmbientOcclusionBit) != 0;
    // A permutation of the game's reflections (trace or composite) made with ours, in a
    // session without wet streets.
    if (!ao_program &&
        (runtime_info.vr_ambient_occlusion &
         (Shader::RuntimeInfo::VrReflections | Shader::RuntimeInfo::VrReflectionComposite)) &&
        !Shader::Optimization::Reflections::Enabled(VR::IsEnabled(),
                                                    VR::GetSettings().wet_streets,
                                                    std::getenv("VR_REFLECTIONS_TRACE"))) {
        return false;
    }
    // The composite made with or without blood on glossy floors (RuntimeInfo::VrBloodFloors), in
    // a session that chose the other.
    if (const u8 ao = runtime_info.vr_ambient_occlusion;
        !ao_program && (ao & Shader::RuntimeInfo::VrReflectionComposite) &&
        ((ao & Shader::RuntimeInfo::VrBloodFloors) != 0) !=
            Shader::Optimization::Reflections::BloodEnabled(VR::IsEnabled(),
                                                            VR::GetSettings().wet_streets,
                                                            std::getenv("VR_BLOOD_REFLECTION"))) {
        return false;
    }
    // A permutation of the light composite made with our bounce or our lights in the air, in a
    // session without it. (The composite's only: bit 1 is VrAirPass on the air pass's programs,
    // left out of the start-up build with the bounce off until 2026-11-17.)
    if (const u8 composite = runtime_info.vr_composite_light;
        Shader::Optimization::BounceLight::IsCompositeProgram(hash) &&
        ((composite & Shader::RuntimeInfo::VrCompositeBounce) &&
         !Shader::Optimization::BounceLight::Enabled(
             VR::IsEnabled(), VR::GetSettings().bounce_light,
             VR::Graphics::Accepted().upscaler != VR::Graphics::Upscaler::Off,
             std::getenv("VR_BOUNCE_LIGHT"))) ||
        ((composite & Shader::RuntimeInfo::VrCompositeFog) &&
         !Shader::Optimization::FogLight::Enabled(
             VR::IsEnabled(), VR::GetSettings().fog_light,
             VR::Graphics::Accepted().upscaler != VR::Graphics::Upscaler::Off,
             std::getenv("VR_FOG_LIGHT"))) ||
        ((composite & Shader::RuntimeInfo::VrCompositeShafts) &&
         !Shader::Optimization::MoonShafts::Enabled(
             VR::IsEnabled(), VR::MoonGlowOn(VR::GetSettings()),
             VR::Graphics::Accepted().upscaler != VR::Graphics::Upscaler::Off,
             std::getenv("VR_MOON_SHAFTS")))) {
        return false;
    }
    // A light program or the ambient composite with our widened gloss, in a session without it.
    if ((runtime_info.vr_ambient_occlusion & Shader::RuntimeInfo::VrSpecularAa) &&
        !Shader::Optimization::SpecularAa::Enabled(VR::IsEnabled(),
                                                  VR::GetSettings().ambient_occlusion,
                                                  std::getenv("VR_SPEC_AA"))) {
        return false;
    }
    // A G-buffer program with our raised brickwork, in a session without it.
    if ((runtime_info.vr_composite_light & Shader::RuntimeInfo::VrBumpGaps) &&
        !Shader::Optimization::BumpGaps::Enabled(VR::IsEnabled(),
                                                 VR::GetSettings().raised_bricks,
                                                 std::getenv("VR_BUMP_GAPS"))) {
        return false;
    }
    // A parallax program with our deeper stones, in a session without them. (A parallax
    // program's only: bit 64 is VrFogGrid on the air pass's programs.)
    if (Shader::Optimization::Parallax::IsParallaxProgram(hash) &&
        (runtime_info.vr_composite_light & Shader::RuntimeInfo::VrParallax) &&
        !Shader::Optimization::Parallax::Enabled(VR::IsEnabled(),
                                                 VR::GetSettings().ambient_occlusion,
                                                 std::getenv("VR_PARALLAX"))) {
        return false;
    }
    // The tone map with our eye adaptation, in a session without it.
    if ((runtime_info.vr_ambient_occlusion & Shader::RuntimeInfo::VrEyeAdaptation) &&
        !Shader::Optimization::CinematicLook::AdaptEnabled(VR::IsEnabled(),
                                                           VR::GetSettings().eye_adaptation,
                                                           std::getenv("VR_EYE_ADAPT"))) {
        return false;
    }
    // A soft particle program with our lit smoke, in a session without it.
    if ((runtime_info.vr_composite_light & Shader::RuntimeInfo::VrParticleAir) &&
        !Shader::Optimization::ParticleAir::Enabled(
            VR::IsEnabled(), VR::GetSettings().fog_light,
            VR::Graphics::Accepted().upscaler != VR::Graphics::Upscaler::Off,
            std::getenv("VR_PARTICLE_AIR"))) {
        return false;
    }
    // A _Bloody_ G-buffer program with our wet blood, in a session without it.
    if ((runtime_info.vr_composite_light & Shader::RuntimeInfo::VrWetBlood) &&
        !Shader::Optimization::WetBlood::Enabled(VR::IsEnabled(),
                                                VR::GetSettings().ambient_occlusion,
                                                std::getenv("VR_WET_BLOOD"))) {
        return false;
    }
    if (runtime_info.vr_motion.Baked() ||
        (runtime_info.vr_motion.enabled && !profile.SupportsVrPushData())) {
        return false;
    }
    return !runtime_info.vr_motion.enabled ||
           VR::Graphics::Accepted().upscaler != VR::Graphics::Upscaler::Off;
}

// VR_SHADER_LIST=<folder>, else the recording folder, else shader_list beside the emulator;
// empty for "0".
std::filesystem::path ListDir() {
    if (const char* v = std::getenv("VR_SHADER_LIST"); v && *v) {
        return std::string_view{v} == "0" ? std::filesystem::path{} : std::filesystem::path{v};
    }
    if (auto folder = ShaderRecord::RecordFolder(); !folder.empty()) {
        return folder;
    }
    const char* base = SDL_GetBasePath(); // UTF-8
    return base ? std::filesystem::path(reinterpret_cast<const char8_t*>(base)) / "shader_list"
                : std::filesystem::path{};
}

// The Old Hunters' list: VR_SHADER_LIST_DLC=<folder>, else shader_list_dlc beside the emulator;
// empty for "0", while recording, or with VR_SHADER_LIST=0.
std::filesystem::path DlcListDir() {
    if (const char* v = std::getenv("VR_SHADER_LIST"); v && std::string_view{v} == "0") {
        return {};
    }
    if (!ShaderRecord::RecordFolder().empty()) {
        return {};
    }
    if (const char* v = std::getenv("VR_SHADER_LIST_DLC"); v && *v) {
        return std::string_view{v} == "0" ? std::filesystem::path{} : std::filesystem::path{v};
    }
    const char* base = SDL_GetBasePath(); // UTF-8
    return base ? std::filesystem::path(reinterpret_cast<const char8_t*>(base)) / "shader_list_dlc"
                : std::filesystem::path{};
}

// The Old Hunters as the game finds it (Libraries::AppContent registers the add-on folder's
// entitlements), or built into the edition.
bool OldHuntersInstalled() {
    const auto title = Common::Singleton<PSF>::Instance()->GetString("TITLE_ID");
    return title &&
           Common::Addons::HasOldHunters(
               *title, Common::Addons::Scan(EmulatorSettings.GetAddonInstallDir(), *title));
}

// The list's id -> the player's permutation (and its program); id 0: it could not be built.
using Renamed = std::unordered_map<u64, std::pair<u64, u64>>;

// The marker: a ShaderProfile blob beside the cache's "profile", so clearing the cache clears it.
// Each list has its own (the game's, The Old Hunters').
constexpr std::string_view MarkerName = "shader_list";
constexpr std::string_view DlcMarkerName = "shader_list_dlc";
constexpr u32 MarkerMagic = 0x4C534242; // "BBSL"
constexpr u32 MarkerVersion = 2; // 2: the list's id without the emulator build, kept apart
struct MarkerHead {
    u32 magic;
    u32 version;
    u64 list_id;
    u64 build_id;
    u64 count;
};
struct MarkerEntry {
    u64 listed;
    u64 program;
    u64 id;
};

// The selected records: another list or selection (the upscaler on, say) gives another id.
u64 ListId(const std::vector<ShaderRecord::Saved>& records) {
    std::vector<u64> words;
    words.reserve(records.size() * 3);
    for (const auto& r : records) {
        words.insert(words.end(), {r.perm_id, r.hash, r.spv_hash});
    }
    return XXH3_64bits(words.data(), words.size() * sizeof(u64));
}

u64 BuildId() {
    const std::string_view build{Common::g_scm_rev};
    return XXH3_64bits(build.data(), build.size());
}

// The marker's names, whatever list it was written for (a list that grew reuses them; its id is
// kept for the logs). A record that could not be built (id 0) is tried again by another build of
// the emulator.
bool LoadMarker(std::string_view name, Renamed& renamed) {
    std::vector<u8> data;
    Storage::DataBase::Instance().Load(Storage::BlobType::ShaderProfile, std::string{name}, data);
    MarkerHead head{};
    if (data.size() < sizeof(head)) {
        return false;
    }
    std::memcpy(&head, data.data(), sizeof(head));
    const size_t entries = (data.size() - sizeof(head)) / sizeof(MarkerEntry);
    if (head.magic != MarkerMagic || head.version != MarkerVersion || head.count == 0 ||
        head.count != entries || data.size() != sizeof(head) + entries * sizeof(MarkerEntry)) {
        return false;
    }
    const bool other_build = head.build_id != BuildId();
    for (size_t i = 0; i < entries; ++i) {
        MarkerEntry entry{};
        std::memcpy(&entry, data.data() + sizeof(head) + i * sizeof(entry), sizeof(entry));
        if (entry.id == 0 && other_build) {
            continue;
        }
        renamed[entry.listed] = {entry.program, entry.id};
    }
    return true;
}

// Only the names of this list's records (`listed`): a record a list dropped is forgotten.
void SaveMarker(std::string_view name, u64 list_id, const Renamed& renamed,
                const std::unordered_set<u64>& listed) {
    std::vector<MarkerEntry> kept;
    kept.reserve(renamed.size());
    for (const auto& [id, to] : renamed) {
        if (listed.contains(id)) {
            kept.push_back({id, to.first, to.second});
        }
    }
    if (kept.empty()) {
        return;
    }
    const MarkerHead head{MarkerMagic, MarkerVersion, list_id, BuildId(), kept.size()};
    std::vector<u8> data(sizeof(head) + kept.size() * sizeof(MarkerEntry));
    std::memcpy(data.data(), &head, sizeof(head));
    std::memcpy(data.data() + sizeof(head), kept.data(), kept.size() * sizeof(MarkerEntry));
    Storage::DataBase::Instance().Save(Storage::BlobType::ShaderProfile, std::string{name},
                                       std::move(data));
}

} // namespace

void PipelineCache::PrebuildFromList() {
    PrebuildList(ListDir(), false);
    const auto dlc = DlcListDir();
    std::error_code exists_error;
    if (dlc.empty() || !std::filesystem::is_regular_file(dlc / "records.bin", exists_error)) {
        return;
    }
    if (!OldHuntersInstalled()) {
        LOG_INFO(Render_Vulkan,
                 "Shader prebuild (The Old Hunters): the add-on is not installed; its list is not "
                 "read");
        return;
    }
    PrebuildList(dlc, true);
}

void PipelineCache::PrebuildList(const std::filesystem::path& dir, bool dlc) {
    std::error_code exists_error;
    if (dir.empty() || !Storage::DataBase::Instance().IsOpened() ||
        !std::filesystem::is_regular_file(dir / "records.bin", exists_error)) {
        return;
    }
    const std::string_view marker = dlc ? DlcMarkerName : MarkerName;
    const std::string_view tag = dlc ? "Shader prebuild (The Old Hunters)" : "Shader prebuild";
    const auto start = std::chrono::steady_clock::now();
    auto records = ShaderRecord::LoadRecords(dir / "records.bin");
    const size_t listed = records.size();
    // One record per permutation (a folder joined by hand may hold one twice).
    std::unordered_set<u64> seen;
    std::erase_if(records, [&](const auto& r) {
        return !Selectable(r.runtime_info, r.hash, profile) || !seen.insert(r.perm_id).second;
    });
    if (records.empty()) {
        LOG_INFO(Render_Vulkan, "{}: {} listed permutations, none for this session", tag, listed);
        return;
    }
    const size_t selected = records.size();
    Startup::Begin(dlc ? "Checking DLC shaders" : "Checking base-game shaders", selected);
    // A permutation of the player's (its program, its id) with a module, or null. Read here on
    // the start-up thread before any draw, as the warm-up reads them.
    const auto find_module = [&](const std::pair<u64, u64>& to) -> const Program::Module* {
        const auto program = program_cache.find(to.first);
        if (program == program_cache.end()) {
            return nullptr;
        }
        const auto& modules = program->second->modules;
        const auto module = std::ranges::find(modules, to.second, &Program::Module::id);
        return module != modules.end() && module->module ? &*module : nullptr;
    };
    // An earlier start built the records the marker names: each whose permutation the warm-up
    // loaded needs nothing, and one that no cached pipeline uses (the warm-up never loads it) is
    // loaded from the cache here. The others are built: all of them without a marker, else a
    // cleared cache's, a retired permutation's, a grown list's new ones.
    const u64 list_id = ListId(records);
    Renamed renamed;
    LoadMarker(marker, renamed);
    std::vector<u64> compute_listed;
    std::vector<ShaderRecord::Saved> work;
    u32 unbuildable = 0, from_cache = 0;
    for (auto& r : records) {
        Startup::Update(static_cast<size_t>(&r - records.data()));
        if (r.l_stage == u32(Shader::LogicalStage::Compute)) {
            compute_listed.push_back(r.perm_id);
        }
        const auto to = renamed.find(r.perm_id);
        if (to != renamed.end() && to->second.second == 0) {
            ++unbuildable; // an earlier start could not build it (its log says why)
            continue;
        }
        if (to != renamed.end() && find_module(to->second)) {
            continue;
        }
        if (to != renamed.end() && LoadCachedPermutation(to->second.second, r.l_stage) &&
            find_module(to->second)) {
            ++from_cache;
            continue;
        }
        if (to != renamed.end()) {
            renamed.erase(to);
        }
        work.push_back(std::move(r));
    }
    records = {};
    const size_t building = work.size();
    u32 added = 0, reused = 0, failed = 0, uncached = 0;
    std::string reasons;
    long long translated_ms = 0;
    if (!work.empty()) {
        VideoCore::ShaderArchive::Programs programs;
        std::filesystem::path shader_dir;
        ShaderRecord::LoadPrograms(work, programs, &shader_dir);
        if (programs.Count() == 0) {
            LOG_WARNING(Render_Vulkan, "{}: no game shaders found in {}; skipped", tag,
                        shader_dir.string());
            return;
        }

        std::map<std::string_view, u32> why; // the failures, by reason
        Startup::Begin(dlc ? "Preparing DLC shaders" : "Preparing base-game shaders",
                       work.size() * 2);
        // Keep only one batch's translated IR/SPIR-V alive. Install it, release the temporary
        // results and the workers' pools, then leave a short quiet interval before the next.
        for (size_t first = 0; first < work.size(); first += Startup::BatchSize) {
            const size_t count = std::min(Startup::BatchSize, work.size() - first);
            std::vector<ShaderRecord::Translated> results(count);
            const auto translating = std::chrono::steady_clock::now();
            Startup::RunBatch(
                count,
                [&](size_t i) {
                    thread_local Shader::Pools pools;
                    results[i] =
                        ShaderRecord::Translate(work[first + i], programs, profile, pools, true);
                },
                [&](size_t done) { Startup::Update(first * 2 + done); });
            translated_ms += std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - translating)
                                 .count();
            {
                std::unique_lock lock(programs_mutex);
                for (size_t i = 0; i < count; ++i) {
                    Startup::Update(first * 2 + count + i);
                    const auto& r = work[first + i];
                    auto& t = results[i];
                    if (t.failure || !t.spec || t.spv.empty()) {
                        renamed[r.perm_id] = {r.hash, 0};
                        ++failed;
                        ++why[t.failure ? t.failure : "no SPIR-V"];
                        continue;
                    }
                    auto [it, new_program] = program_cache.try_emplace(r.hash);
                    if (new_program) {
                        it.value() = std::make_unique<Program>();
                        auto& info = it.value()->info;
                        info = *t.info; // the program's first translation, as GetProgram
                        // Its SRT walker, which flattens each draw's user data (RefreshFlatBuf):
                        // the replayed translation compared its own with the recording's and kept
                        // neither (an SrtSnapshot's walker is never run), so the recording's - the
                        // same bytes - goes into the shared walker code, as a cached program's does
                        // when it loads. This runs once, on this thread, before any draw
                        // translates.
                        if (!r.walker.empty()) {
                            info.srt_info.walker_func =
                                Shader::RegisterWalkerCode(r.walker.data(), r.walker.size());
                            info.srt_info.walker_func_size = r.walker.size();
                        }
                    }
                    Program& program = *it.value();
                    auto spec = std::move(*t.spec);
                    spec.info = &program.info;
                    // The archive's copy shader goes with `programs` below; like a loaded meta's,
                    // this runtime info keeps only the copy shader's hash.
                    spec.runtime_info.gs_info.vs_copy = {};
                    if (const auto found =
                            std::ranges::find(program.modules, spec, &Program::Module::spec);
                        found != program.modules.end()) {
                        if (found->module) {
                            renamed[r.perm_id] = {r.hash, found->id};
                            ++reused;
                        } else {
                            renamed[r.perm_id] = {r.hash, 0};
                            ++failed; // a cached permutation the driver rejected: the draw path's
                            ++why["rejected before"];
                        }
                        continue;
                    }
                    const size_t index = program.modules.size();
                    bool cacheable = true;
                    // Recording into the list's folder: the list's own id, which its keys and the
                    // keys this session records use.
                    u64 id = ShaderRecord::RecordOn() ? r.perm_id
                             : PermutationIds()       ? PermutationId(r.hash, spec)
                                                      : HashCombine(r.hash, u64(index));
                    while (program.HasId(id)) {
                        id = PipelineCachePolicy::NextId(id);
                        cacheable = false;
                    }
                    const auto module = CompileSPV(t.spv, instance.GetDevice());
                    if (!module) {
                        renamed[r.perm_id] = {r.hash, 0};
                        ++failed;
                        ++why["the driver refused it"];
                        continue;
                    }
                    const u64 outputs = t.info->vertex_output_locations;
                    // Its own resources when they are not the program's (Program::Module::info),
                    // with its recording's SRT walker as a new program's gets.
                    std::shared_ptr<Shader::Info> own;
                    if (!new_program) {
                        own = OwnResources(Shader::Info(*t.info), program.info, index);
                        if (own && !r.walker.empty()) {
                            own->srt_info.walker_func =
                                Shader::RegisterWalkerCode(r.walker.data(), r.walker.size());
                            own->srt_info.walker_func_size = r.walker.size();
                        }
                    }
                    if (cacheable) {
                        RegisterShaderBinary(std::vector<u32>(t.spv), id, r.hash, index);
                        RegisterShaderMeta(own ? *own : program.info, spec.fetch_shader_data, spec,
                                           id, index, outputs);
                    } else {
                        ++uncached;
                    }
                    program.AddPermut(module, std::move(spec), id, outputs, std::move(own));
                    renamed[r.perm_id] = {r.hash, id};
                    ++added;
                }
                ++programs_generation;
            }
            results.clear();
            Startup::Update((first + count) * 2);
            if (first + count < work.size())
                Startup::Pause();
        }
        work = {};
        for (const auto& [reason, count] : why) {
            reasons += fmt::format("{}{} {}", reasons.empty() ? " (" : ", ", count, reason);
        }
        if (!reasons.empty()) {
            reasons += ')';
        }
        SaveMarker(marker, list_id, renamed, seen);
    }

    // Nothing built: the list's pipelines were created (and cached) by the start that built it.
    if (building == 0) {
        LOG_INFO(Render_Vulkan,
                 "{}: the cache holds this list's {} permutations already ({} loaded here, {} "
                 "could not be built); {} ms",
                 tag, selected - unbuildable, from_cache, unbuildable,
                 std::chrono::duration_cast<std::chrono::milliseconds>(
                     std::chrono::steady_clock::now() - start)
                     .count());
        return;
    }

    // The pipelines whose stages are all there, created as the warm-up creates its own.
    std::vector<PreloadJob> jobs;
    u32 keys = 0, incomplete = 0, present = 0, misaligned = 0;
    // A compute permutation is its pipeline (the key is its id).
    for (const u64 listed_id : compute_listed) {
        const auto to = renamed.find(listed_id);
        const auto* module = to != renamed.end() ? find_module(to->second) : nullptr;
        if (!module) {
            continue;
        }
        ++keys;
        PreloadJob job{};
        job.is_compute = true;
        job.compute_key.value = module->id;
        if (!compute_pipelines.try_emplace(job.compute_key).second) {
            ++present; // the warm-up's, or another record's that named the same permutation
            continue;
        }
        job.infos[0] = module->info ? module->info.get()
                                    : &program_cache.find(to->second.first)->second->info;
        job.modules[0] = module->module;
        jobs.push_back(std::move(job));
    }
    std::error_code error;
    for (const auto& file : std::filesystem::directory_iterator(dir / "keys", error)) {
        const auto name = file.path().filename().string();
        if (!name.starts_with("g_") || file.path().extension() != ".key") {
            continue; // a compute pipeline's key is its permutation's id (above)
        }
        std::vector<u8> blob(file.file_size(error));
        std::ifstream in(file.path(), std::ios::binary);
        if (error || !in.read(reinterpret_cast<char*>(blob.data()), std::streamsize(blob.size()))) {
            continue;
        }
        PreloadJob job{};
        if (!ReadGraphicsKeyBlob(std::move(blob), job.graphics_key, job.graphics_sdata)) {
            continue;
        }
        ++keys;
        bool complete = true;
        std::array<Shader::Backend::Bindings, MaxShaderStages> starts{};
        for (u32 stage = 0; stage < MaxShaderStages && complete; ++stage) {
            auto& stage_hash = job.graphics_key.stage_hashes[stage];
            if (stage_hash == 0) {
                continue;
            }
            const auto to = renamed.find(stage_hash);
            const auto* module = to != renamed.end() ? find_module(to->second) : nullptr;
            if (!module) {
                complete = false;
                break;
            }
            stage_hash = module->id;
            job.infos[stage] = module->info ? module->info.get()
                                            : &program_cache.find(to->second.first)->second->info;
            starts[stage] = module->spec.start;
            job.runtime_infos[stage] = module->spec.runtime_info;
            job.modules[stage] = module->module;
            if (module->spec.fetch_shader_data) {
                job.fetch_shader = module->spec.fetch_shader_data;
            }
        }
        if (!complete) {
            ++incomplete;
            continue;
        }
        // Recorded with stages that were translated with other resources (a VR pass's buffers
        // added since the list was made): left to the draw path.
        if (!BindingsLineUp(job.infos, starts)) {
            ++misaligned;
            continue;
        }
        if (!graphics_pipelines.try_emplace(job.graphics_key).second) {
            ++present;
            continue;
        }
        jobs.push_back(std::move(job));
    }
    Startup::Begin(dlc ? "Compiling DLC pipelines" : "Compiling base-game pipelines", jobs.size());
    Startup::RunBatches(jobs.size(), [&](size_t i) {
        auto& job = jobs[i];
        g_tolerate_pipeline_rejection = true; // a refused pipeline is left out, never asserted
        if (job.is_compute) {
            job.compute = std::make_unique<ComputePipeline>(
                instance, scheduler, desc_heap, profile, *pipeline_cache, job.compute_key,
                *job.infos[0], job.modules[0], job.compute_sdata, true);
        } else {
            job.graphics = std::make_unique<GraphicsPipeline>(
                instance, scheduler, desc_heap, profile, job.graphics_key, *pipeline_cache,
                job.infos, job.runtime_infos, job.fetch_shader, job.modules, job.graphics_sdata,
                true);
        }
        g_tolerate_pipeline_rejection = false;
    });
    u32 created = 0, refused = 0;
    for (auto& job : jobs) {
        if (job.is_compute) {
            if (!job.compute || !job.compute->Handle()) {
                compute_pipelines.erase(job.compute_key); // the dispatch path compiles it
                ++refused;
                continue;
            }
            RegisterPipelineData(job.compute_key, job.compute_sdata);
            compute_pipelines.find(job.compute_key).value() = std::move(job.compute);
            ++created;
            continue;
        }
        if (!job.graphics || !job.graphics->Handle()) {
            graphics_pipelines.erase(job.graphics_key); // the draw path compiles it
            ++refused;
            continue;
        }
        RegisterPipelineData(job.graphics_key, std::hash<GraphicsPipelineKey>{}(job.graphics_key),
                             job.graphics_sdata);
        graphics_pipelines.find(job.graphics_key).value() = std::move(job.graphics);
        ++created;
    }
    const auto all_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
    LOG_INFO(Render_Vulkan,
             "{}: {} of {} listed permutations for this session, {} built{}: {} "
             "added, {} already cached, {} not built{}, {} added but not cached; translated in {} "
             "ms. Pipelines: {} listed, {} created, {} already present, {} missing a stage, {} "
             "with stages not lined up (left to the draw path), {} refused; {} ms in all",
             tag, selected, listed, building,
             building < selected ? " (the rest were in the cache)" : "", added, reused, failed,
             reasons, uncached, translated_ms, keys, created, present, incomplete, misaligned,
             refused, all_ms);
}

} // namespace Vulkan
