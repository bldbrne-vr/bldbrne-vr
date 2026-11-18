// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <type_traits>
#include <unordered_set>
#include <xxhash.h>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "core/file_sys/fs.h"
#include "shader_recompiler/backend/spirv/emit_spirv.h"
#include "shader_recompiler/ir/passes/srt.h"
#include "shader_recompiler/vr_motion.h"
#include "video_core/renderer_vulkan/vk_pipeline_serialization.h"
#include "video_core/renderer_vulkan/vk_shader_record.h"
#include "video_core/shader_archive.h"

namespace Vulkan::ShaderRecord {
namespace {

static_assert(std::is_trivially_copyable_v<Shader::RuntimeInfo>);
static_assert(std::is_trivially_copyable_v<Shader::Backend::Bindings>);
static_assert(std::is_trivially_copyable_v<Shader::Gcn::VertexAttribute>);
static_assert(std::is_trivially_copyable_v<Shader::TranslationCapture::Read>);

constexpr u32 Magic = 0x52534242; // "BBSR"
constexpr u32 Version = 2;        // 2: no game code (fetch shader parsed, copy shader by hash)

const char* RecordDir() {
    static const char* dir = []() -> const char* {
        const char* v = std::getenv("VR_SHADER_RECORD");
        if (!v || !*v) {
            return nullptr;
        }
        // A list names its permutations by their content: the indexed naming's would collide
        // between sessions (vk_shader_record.h).
        if (!PermutationIds()) {
            LOG_WARNING(Render_Vulkan,
                        "VR_SHADER_RECORD needs FAST_CPU_PERM_IDS=1; nothing is recorded");
            return nullptr;
        }
        return v;
    }();
    return dir;
}

// A little binary writer / reader; every read is bounds-checked.
struct Out {
    std::vector<u8> bytes;
    template <typename T>
    void Put(const T& v) {
        static_assert(std::is_trivially_copyable_v<T>);
        const auto* p = reinterpret_cast<const u8*>(&v);
        bytes.insert(bytes.end(), p, p + sizeof(T));
    }
    template <typename T>
    void PutVec(const std::vector<T>& v) {
        Put(u32(v.size()));
        const auto* p = reinterpret_cast<const u8*>(v.data());
        bytes.insert(bytes.end(), p, p + v.size() * sizeof(T));
    }
};
struct In {
    std::span<const u8> bytes;
    size_t at{};
    bool ok{true};
    template <typename T>
    void Get(T& v) {
        if (!ok || bytes.size() - at < sizeof(T)) {
            ok = false;
            return;
        }
        std::memcpy(&v, bytes.data() + at, sizeof(T));
        at += sizeof(T);
    }
    template <typename T>
    void GetVec(std::vector<T>& v, u32 max = 1u << 24) {
        u32 n = 0;
        Get(n);
        if (!ok || n > max || (bytes.size() - at) / sizeof(T) < n) {
            ok = false;
            return;
        }
        v.resize(n);
        std::memcpy(v.data(), bytes.data() + at, size_t(n) * sizeof(T));
        at += size_t(n) * sizeof(T);
    }
};

std::vector<u8> Encode(const Saved& r) {
    Out o;
    o.Put(Magic), o.Put(Version), o.Put(r.hash), o.Put(r.perm_id), o.Put(r.stage), o.Put(r.l_stage);
    o.Put(r.program_base), o.Put(r.user), o.Put(r.binding), o.Put(r.spv_hash),
        o.Put(r.runtime_info);
    o.PutVec(r.flat), o.PutVec(r.walker), o.Put(r.has_fetch), o.Put(r.fetch_size);
    o.Put(r.vertex_offset_sgpr), o.Put(r.instance_offset_sgpr), o.PutVec(r.attributes);
    o.PutVec(r.reads), o.PutVec(r.tess_constants);
    return std::move(o.bytes);
}

bool Decode(std::span<const u8> bytes, Saved& r) {
    In in{bytes};
    u32 magic = 0, version = 0;
    in.Get(magic), in.Get(version);
    if (!in.ok || magic != Magic || version != Version) {
        return false;
    }
    in.Get(r.hash), in.Get(r.perm_id), in.Get(r.stage), in.Get(r.l_stage), in.Get(r.program_base);
    in.Get(r.user), in.Get(r.binding), in.Get(r.spv_hash), in.Get(r.runtime_info);
    in.GetVec(r.flat), in.GetVec(r.walker), in.Get(r.has_fetch), in.Get(r.fetch_size);
    in.Get(r.vertex_offset_sgpr), in.Get(r.instance_offset_sgpr), in.GetVec(r.attributes, 256);
    in.GetVec(r.reads, 256), in.GetVec(r.tess_constants, 4096);
    const bool stages_known = r.stage <= u32(Shader::Stage::Compute) &&
                              r.l_stage < u32(Shader::LogicalStage::NumLogicalStages);
    r.runtime_info.gs_info.vs_copy = {}; // never a pointer from the file
    return in.ok && in.at == bytes.size() && stages_known;
}

// Every intact record of the file, and the bytes they span (a damaged record ends the reading).
u64 ReadRecords(const std::filesystem::path& file, std::vector<Saved>& records) {
    std::ifstream in(file, std::ios::binary);
    std::vector<u8> bytes;
    u64 intact = 0;
    u32 size = 0;
    while (in.read(reinterpret_cast<char*>(&size), sizeof(size))) {
        if (size > (64u << 20)) {
            break;
        }
        bytes.resize(size);
        if (!in.read(reinterpret_cast<char*>(bytes.data()), size)) {
            break;
        }
        Saved r;
        if (!Decode(bytes, r)) {
            break;
        }
        records.push_back(std::move(r));
        intact += sizeof(size) + size;
    }
    return intact;
}

// The thread's translation waiting for its permutation's name (Record, then Commit).
thread_local std::optional<Saved> t_pending;

std::mutex g_write_mutex;
// The permutations the folder holds (its earlier sessions' and this one's), read at the first
// record; under g_write_mutex.
std::optional<std::unordered_set<u64>> g_recorded;

// Zeros for a replayed Info's user data once its translation is done: nothing it keeps points
// at a record's (temporary) registers.
constexpr std::array<u32, Shader::ShaderParams::NumShaderUserData> NoUserData{};

} // namespace

bool RecordOn() {
    return RecordDir() != nullptr;
}

std::filesystem::path RecordFolder() {
    return RecordOn() ? std::filesystem::path{RecordDir()} : std::filesystem::path{};
}

void Record(const Shader::Info& info, const Entry& entry, std::span<const u32> spv) {
    if (!RecordOn()) {
        return;
    }
    Saved& r = t_pending.emplace();
    r.hash = info.pgm_hash;
    r.stage = u32(info.stage);
    r.l_stage = u32(info.l_stage);
    r.program_base = entry.program_base;
    r.user = entry.user;
    r.binding = entry.binding;
    r.spv_hash = XXH3_64bits(spv.data(), spv.size_bytes());
    r.runtime_info = entry.runtime_info;
    r.runtime_info.gs_info.vs_copy = {}; // a guest pointer: a replay takes the code by its hash
    const auto flat = info.FlatUserData();
    r.flat.assign(flat.begin(), flat.end());
    if (const auto* walker = reinterpret_cast<const u8*>(info.srt_info.walker_func)) {
        r.walker.assign(walker, walker + info.srt_info.walker_func_size);
    }
    // What the translation read beyond the code and the flattened user data, as it read it
    // (Shader::TranslationCapture): reading it again here would follow pointers that may no
    // longer hold it.
    if (entry.fetch) {
        r.has_fetch = true;
        r.fetch_size = entry.fetch->size;
        r.vertex_offset_sgpr = entry.fetch->vertex_offset_sgpr;
        r.instance_offset_sgpr = entry.fetch->instance_offset_sgpr;
        r.attributes = entry.fetch->attributes;
    }
    r.reads = entry.capture.reads;
    r.tess_constants = entry.capture.tess_constants;
}

void Commit(u64 perm_id) {
    if (!t_pending) {
        return;
    }
    Saved r = std::move(*t_pending);
    t_pending.reset();
    r.perm_id = perm_id;
    const auto bytes = Encode(r);
    std::scoped_lock lock{g_write_mutex};
    const auto path = std::filesystem::path(RecordDir()) / "records.bin";
    if (!g_recorded) {
        // The folder's earlier sessions: their permutations are not written again, and a record
        // cut short (a session stopped mid-write) is cut off so the new ones stay readable.
        std::vector<Saved> earlier;
        const u64 intact = ReadRecords(path, earlier);
        std::error_code error;
        if (std::filesystem::file_size(path, error) > intact && !error) {
            std::filesystem::resize_file(path, intact, error);
        }
        g_recorded.emplace();
        for (const auto& e : earlier) {
            g_recorded->insert(e.perm_id);
        }
    }
    if (!g_recorded->insert(perm_id).second) {
        return;
    }
    std::ofstream out(path, std::ios::binary | std::ios::app);
    const u32 size = u32(bytes.size());
    out.write(reinterpret_cast<const char*>(&size), sizeof(size));
    out.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
}

void RecordKey(std::string_view name, std::span<const u8> blob) {
    if (!RecordOn()) {
        return;
    }
    const auto keys = std::filesystem::path(RecordDir()) / "keys";
    const auto file = keys / fmt::format("{}.key", name);
    std::scoped_lock lock{g_write_mutex};
    std::error_code error;
    if (std::filesystem::exists(file, error)) {
        return; // an earlier session's: the same key (its stage hashes are content ids)
    }
    std::filesystem::create_directories(keys, error);
    // Written whole, then named: a session stopped mid-write leaves no half key.
    auto part = file;
    part += ".part";
    {
        std::ofstream out(part, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(blob.data()), std::streamsize(blob.size()));
        if (!out) {
            return;
        }
    }
    std::filesystem::rename(part, file, error);
}

std::vector<Saved> LoadRecords(const std::filesystem::path& file) {
    std::vector<Saved> records;
    ReadRecords(file, records);
    return records;
}

size_t LoadPrograms(const std::vector<Saved>& records, VideoCore::ShaderArchive::Programs& out,
                    std::filesystem::path* dir_out) {
    std::filesystem::path dir, executable;
    if (const char* v = std::getenv("VR_SHADER_ARCHIVE_DIR"); v && *v) {
        dir = v; // <game>/dvdroot_ps4/shader
        executable = dir / ".." / ".." / "eboot.bin";
    } else {
        auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
        dir = mnt->GetHostPath("/app0/dvdroot_ps4/shader");
        executable = mnt->GetHostPath("/app0/eboot.bin");
    }
    if (dir_out) {
        *dir_out = dir;
    }
    std::unordered_set<u64> wanted;
    for (const auto& r : records) {
        wanted.insert(r.hash); // a geometry shader's .gpo also holds its copy shader
        if (r.l_stage == u32(Shader::LogicalStage::Geometry)) {
            wanted.insert(r.runtime_info.gs_info.vs_copy_hash);
        }
    }
    std::error_code error;
    size_t sources = 0;
    if (!dir.empty() && std::filesystem::is_directory(dir, error)) {
        sources = out.Load(dir, wanted);
    }
    // The engine's own shaders (post effects, the interface's) are in the executable: read
    // (whole, for the moment) only when the archives lacked some.
    std::erase_if(wanted, [&](u64 hash) { return out.Has(hash); });
    if (!wanted.empty() && !executable.empty() && out.LoadExecutable(executable, wanted)) {
        ++sources;
    }
    return sources;
}

Translated Translate(const Saved& r, const VideoCore::ShaderArchive::Programs& programs,
                     const Shader::Profile& profile, Shader::Pools& pools, bool with_spec) {
    Translated out;
    const auto code = programs.Code(r.hash);
    if (code.empty()) {
        out.failure = "not in the game's shaders";
        return out;
    }
    // The user-data pointers the translation follows (the vertex V# tables, the tessellation
    // constants' V#), aimed at host copies of what it read.
    auto user = r.user;
    std::map<u32, std::vector<u32>> tables;
    // A read at Shader::IR::NumScalarRegs is of the user-data registers themselves (a V# held
    // there, no pointer followed): the record's registers already hold it.
    constexpr u32 InRegisters = u32(Shader::IR::NumScalarRegs);
    for (const auto& read : r.reads) {
        if (read.pointer_index == InRegisters) {
            if (size_t(read.dword_offset) + read.words.size() > user.size() ||
                !std::equal(read.words.begin(), read.words.end(),
                            user.begin() + read.dword_offset)) {
                out.failure = "a recorded register read disagrees with the registers";
                return out;
            }
            continue;
        }
        if (read.pointer_index + 1 >= user.size() || read.dword_offset > 4096) {
            out.failure = "a recorded read is out of range";
            return out;
        }
        auto& table = tables[read.pointer_index];
        table.resize(std::max<size_t>(table.size(), size_t(read.dword_offset) + 4));
        std::memcpy(table.data() + read.dword_offset, read.words.data(), sizeof(read.words));
    }
    for (auto& [index, table] : tables) {
        const u64 address = reinterpret_cast<u64>(table.data());
        user[index] = u32(address);
        user[index + 1] = u32(address >> 32);
    }
    auto runtime_info = r.runtime_info;
    if (r.l_stage == u32(Shader::LogicalStage::Geometry)) {
        runtime_info.gs_info.vs_copy = programs.CopyCode(runtime_info.gs_info.vs_copy_hash);
        if (runtime_info.gs_info.vs_copy.empty()) {
            out.failure = "its copy shader is not in the game's shaders";
            return out;
        }
    }
    Shader::Gcn::FetchShaderData fetch{};
    fetch.size = r.fetch_size;
    fetch.vertex_offset_sgpr = r.vertex_offset_sgpr;
    fetch.instance_offset_sgpr = r.instance_offset_sgpr;
    fetch.attributes = r.attributes;
    for (const auto& attribute : fetch.attributes) {
        if (attribute.sgpr_base != InRegisters && !tables.contains(attribute.sgpr_base)) {
            out.failure = "a vertex attribute's V# was not recorded";
            return out;
        }
    }
    const Shader::ShaderParams params{.user_data = user, .code = code, .hash = r.hash};
    out.info = std::make_unique<Shader::Info>(Shader::Stage(r.stage),
                                              Shader::LogicalStage(r.l_stage), params);
    auto& info = *out.info;
    info.pgm_base = r.program_base;
    if (!r.tess_constants.empty()) {
        info.tess_constants_override = r.tess_constants.data();
    }
    if (r.has_fetch) {
        info.fetch_override = &fetch;
    }
    const auto clear = [&] {
        info.tess_constants_override = nullptr;
        info.fetch_override = nullptr;
        info.user_data = NoUserData;
    };
    Shader::SrtSnapshot srt{.flat = r.flat, .walker = r.walker};
    const auto program = Shader::TranslateProgram(code, pools, info, runtime_info, profile, &srt);
    if (srt.differs || info.translation_failed) {
        out.failure = srt.differs ? "its flattened user data are laid out unlike the recording's"
                                  : "translation failed";
        clear();
        return out;
    }
    // As CompileModule.
    info.uses_reserved_motion_varyings =
        info.l_stage == Shader::LogicalStage::Vertex &&
        (info.stores.GetAny(Shader::IR::Attribute::Param0 +
                            (Shader::VrMotion::CurrentLocation - 1)) ||
         info.stores.GetAny(Shader::IR::Attribute::Param0 + Shader::VrMotion::CurrentLocation) ||
         info.stores.GetAny(Shader::IR::Attribute::Param0 + Shader::VrMotion::PreviousLocation));
    auto binding = r.binding;
    out.spv = Shader::Backend::SPIRV::EmitSPIRV(profile, runtime_info, program, binding);
    if (with_spec) {
        // As GetProgram: the specialization of the selection's runtime info (the fragment stage
        // keeps only the colour targets its program writes), from the start bindings.
        auto spec_runtime = r.runtime_info;
        spec_runtime.gs_info.vs_copy = runtime_info.gs_info.vs_copy;
        if (info.stage == Shader::Stage::Fragment) {
            spec_runtime.fs_info.KeepWrittenColorBuffers(info.mrt_mask, profile.needs_unorm_fixup);
        }
        out.spec.emplace(info, spec_runtime, profile, r.binding);
    }
    clear();
    return out;
}

void ReplayIfAsked(const Shader::Profile& profile) {
    const char* dir = std::getenv("VR_SHADER_REPLAY");
    if (!dir || !*dir) {
        return;
    }
    const auto records = LoadRecords(std::filesystem::path(dir) / "records.bin");
    VideoCore::ShaderArchive::Programs programs;
    std::filesystem::path shader_dir;
    const auto start = std::chrono::steady_clock::now();
    const size_t archives = LoadPrograms(records, programs, &shader_dir);
    LOG_INFO(Render_Vulkan,
             "Shader replay: {} records; {} game files ({} and the executable) gave {} programs "
             "({} KiB) in {} ms",
             records.size(), archives, shader_dir.string(), programs.Count(),
             programs.Bytes() >> 10,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - start)
                 .count());

    struct Tally {
        u32 same{}, different{}, missing{}, failed{};
    };
    std::map<u32, Tally> by_stage;
    Shader::Pools pools;
    const auto translate_start = std::chrono::steady_clock::now();
    u32 logged = 0;
    for (const auto& r : records) {
        auto& t = by_stage[r.l_stage];
        const auto result = Translate(r, programs, profile, pools, false);
        if (result.failure) {
            ++(programs.Code(r.hash).empty() ? t.missing : t.failed);
            if (!programs.Code(r.hash).empty() && logged++ < 20) {
                LOG_WARNING(Render_Vulkan, "Shader replay: {:#x} stage {}: {}", r.hash, r.l_stage,
                            result.failure);
            }
            continue;
        }
        if (XXH3_64bits(result.spv.data(), result.spv.size() * sizeof(u32)) == r.spv_hash) {
            ++t.same;
        } else {
            ++t.different;
            if (logged++ < 20) {
                LOG_WARNING(Render_Vulkan,
                            "Shader replay: {:#x} stage {} perm {:#x}: SPIR-V differs ({} words)",
                            r.hash, r.l_stage, r.perm_id, result.spv.size());
            }
        }
    }
    for (const auto& [stage, t] : by_stage) {
        LOG_INFO(Render_Vulkan,
                 "Shader replay: stage {}: identical {} different {} not in the archives {} "
                 "failed {}",
                 stage, t.same, t.different, t.missing, t.failed);
    }
    LOG_INFO(Render_Vulkan, "Shader replay: translated in {} ms",
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - translate_start)
                 .count());
}

} // namespace Vulkan::ShaderRecord
