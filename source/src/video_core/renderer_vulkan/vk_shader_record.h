// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

// The startup shader build (2026-11-03, the hit stutter: a character's first blood switches its
// model to the game's "Bloody" materials, whose shaders were translated in the middle of play).
// A shader permutation translated during play is recorded with everything its translation read
// besides the program's code, so it can be translated again with the game not running: its
// code then comes from the game's own shader archives and executable (video_core/
// shader_archive.h). A record holds no game code: the program and a geometry shader's copy
// shader are taken from the game's files by hash, the fetch shader as its parsed attributes.
//
// What a translation reads (the research of 2026-11-03): the 16 user-data registers, the
// flattened user data the SRT walker built from them (every V#/T#/S# the resource tracking
// reads), the walker itself (compared, never run: Shader::SrtSnapshot), the runtime info and the
// start bindings. A vertex stage also reads its parsed fetch shader and the vertex V#s it names
// (Info::ReadUdReg); a tessellation stage the tessellation constants; a geometry stage its copy
// shader. The recording keeps them as the translation reads them (Shader::TranslationCapture);
// a replay aims the user-data pointers at host copies of the recorded words, sets the parsed
// fetch shader and the constants on the Info (Info::fetch_override, tess_constants_override)
// and the copy shader's span at the archive's code: no guest memory is read.
//
// VR_SHADER_RECORD=<folder> (with FAST_CPU_PERM_IDS=1, without it nothing is recorded): every
// module CompileModule translates is appended to <folder>/records.bin with the hash of the SPIR-V
// it produced, and every graphics pipeline key the session writes to the cache goes to
// <folder>/keys as well. A recording session first builds the folder as its shader list
// (PipelineCache::PrebuildFromList, unless VR_SHADER_LIST names another), so play finds the
// permutations the folder holds and records only new ones, and the keys it writes name the
// folder's permutations by the folder's ids: sessions recorded into one folder add up, each
// permutation once. (Two sessions recording apart could name one permutation twice: an id hashes
// all of its specialization, an equal one compares less. The build merges those, at a cost.) VR_SHADER_REPLAY=<folder>: at start-up every record is
// translated again and compared with that hash ("Shader replay:" lines; test tooling). The
// startup build itself is PipelineCache::PrebuildFromList (vk_shader_prebuild.cpp).

#include <array>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "common/types.h"
#include "shader_recompiler/backend/bindings.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/recompiler.h"
#include "shader_recompiler/runtime_info.h"
#include "shader_recompiler/specialization.h"
#include "shader_recompiler/translation_capture.h"

namespace VideoCore::ShaderArchive {
class Programs;
}

namespace Vulkan::ShaderRecord {

bool RecordOn();
// VR_SHADER_RECORD's folder (empty when not recording).
std::filesystem::path RecordFolder();

// CompileModule's inputs before the translation changed them, and what the translation read
// beyond them (the thread's captures around TranslateProgram and EmitSPIRV).
struct Entry {
    std::array<u32, Shader::ShaderParams::NumShaderUserData> user{};
    VAddr program_base{};
    Shader::RuntimeInfo runtime_info{};
    Shader::Backend::Bindings binding{};
    Shader::TranslationCapture capture{};
    std::optional<Shader::Gcn::FetchShaderData> fetch{};
};

// After the translation and the SPIR-V: the thread's record, kept until its permutation is named
// (a program's first permutation is named by its specialization, built after the translation).
// No-op unless RecordOn().
void Record(const Shader::Info& info, const Entry& entry, std::span<const u32> spv);
// The permutation the thread's last Record translated is named `perm_id`: appends that record
// (no-op without one, or when the folder holds that permutation already).
void Commit(u64 perm_id);

// A graphics pipeline key as the cache saves it, under the cache's name: <folder>/keys/<name>.key
// (no-op unless RecordOn(), or when the folder holds that key already).
void RecordKey(std::string_view name, std::span<const u8> blob);

// One recorded translation.
struct Saved {
    u64 hash{};
    u64 perm_id{};
    u32 stage{};
    u32 l_stage{};
    u64 program_base{};
    std::array<u32, Shader::ShaderParams::NumShaderUserData> user{};
    Shader::Backend::Bindings binding{};
    u64 spv_hash{};
    Shader::RuntimeInfo runtime_info{}; // gs_info.vs_copy empty (a guest pointer)
    std::vector<u32> flat;
    std::vector<u8> walker;
    bool has_fetch{};
    u32 fetch_size{};
    s8 vertex_offset_sgpr{-1};
    s8 instance_offset_sgpr{-1};
    std::vector<Shader::Gcn::VertexAttribute> attributes;
    std::vector<Shader::TranslationCapture::Read> reads;
    std::vector<u8> tess_constants;
};

// Every intact record of a records.bin (a damaged one ends the reading).
std::vector<Saved> LoadRecords(const std::filesystem::path& file);

// A record translated again from the archive's code.
struct Translated {
    std::unique_ptr<Shader::Info> info; // its user data are zeros afterwards (a guest-free Info)
    std::vector<u32> spv;
    // The permutation's specialization, built while the recorded inputs were set (`info` is its
    // Info: a caller that keeps another Info for the program points it there).
    std::optional<Shader::StageSpecialization> spec;
    const char* failure{}; // why there is no SPIR-V
};
Translated Translate(const Saved& record, const VideoCore::ShaderArchive::Programs& programs,
                     const Shader::Profile& profile, Shader::Pools& pools, bool with_spec);

// The programs (and geometry copy shaders) the records need, read from the game's archives:
// VR_SHADER_ARCHIVE_DIR, else the mounted game's /app0/dvdroot_ps4/shader.
size_t LoadPrograms(const std::vector<Saved>& records, VideoCore::ShaderArchive::Programs& out,
                    std::filesystem::path* dir_out = nullptr);

// VR_SHADER_REPLAY: translates every record of the folder again and logs how many give the same
// SPIR-V.
void ReplayIfAsked(const Shader::Profile& profile);

} // namespace Vulkan::ShaderRecord
