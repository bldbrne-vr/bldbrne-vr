// SPDX-FileCopyrightText: Copyright 2025-2026 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <atomic>
#include <cstdlib>
#include "common/shader_cache_lock.h"
#include "common/render_diagnostic.h"
#include "imgui/notifications_layer.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/polyfill_thread.h"
#include "common/thread.h"
#include "core/emulator_settings.h"

#include "video_core/cache_pack.h"
#include "video_core/cache_storage.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"

#include <miniz.h>

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <string_view>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#ifdef _WIN32
// The launcher probes this export without running the executable.
extern "C" __declspec(dllexport) int BbvrSharedShaderCacheV1() { return 1; }
#endif

namespace {
#ifdef _WIN32
Common::ShaderCacheLock cache_lock;
#endif


std::mutex submit_mutex{};
std::atomic<u32> num_requests{};
std::condition_variable_any request_cv{};
std::queue<std::packaged_task<void()>> req_queue{};
std::mutex m_request{};
std::atomic<u32> num_written{};
std::atomic<u32> num_write_failures{};

mz_zip_archive zip_ar{};
bool ar_is_read_only{true};

// Bloodborne VR fork: a blob is written to "<name>.<ext>.tmp" and renamed over the final name only
// once complete, so a session killed in the middle of a write (the exit path, a crash) never leaves a
// truncated entry for the next warm-up; a leftover .tmp is deleted when the cache opens.
constexpr std::string_view TmpSuffix = ".tmp";

// The pack (video_core/cache_pack.h; the folder cache only): the start's pack and what replaces
// its blobs since - a blob's own file (written later, so newer) and a removal mark
// ("<name>.<ext>.gone"). Under pack_mutex. The profile and the shader lists' markers (.bin) stay
// their own files (the launcher looks for profile.bin).
constexpr std::string_view PackFile = "shaders.pack";
constexpr std::string_view NextPackFile = "shaders.pack.next"; // folded by the last session
constexpr std::string_view NextPackTmp = "shaders.pack.next.tmp";
constexpr std::string_view GoneSuffix = ".gone";
// Own files that make a new pack worth writing (fewer cost little to read).
constexpr size_t MinPackFiles = 32;
std::mutex pack_mutex{};
std::unique_ptr<VideoCore::CachePack::Reader> pack_reader{};
std::unordered_set<std::string> loose_blobs{};
std::unordered_set<std::string> gone_blobs{};
std::jthread packer{};
std::atomic<bool> packer_started{};
// The pack, or the last session's next one, could not be read at the open (another program held
// it): it is kept for the next start, and no new pack is written meanwhile. Under pack_mutex.
bool pack_held{};

// A blob type's extension as the pack keeps it, or -1 for a file the pack never takes.
int PackType(std::string_view extension) {
    if (extension == ".meta") {
        return 0;
    }
    if (extension == ".spv") {
        return 1;
    }
    if (extension == ".key") {
        return 2;
    }
    return -1;
}

// A file that opens for reading: a pack that does not is held by another program (an antivirus,
// a backup), not damaged.
bool Readable(const std::filesystem::path& path) {
    return std::ifstream{path, std::ios::binary}.is_open();
}

s64 WriteTime(const std::filesystem::directory_entry& entry) {
    std::error_code ec;
    return entry.last_write_time(ec).time_since_epoch().count();
}

std::string Stem(const std::string& file_name) {
    return file_name.substr(0, file_name.rfind('.'));
}

// The folder's own blob files and removal marks: "<name>.<ext>" -> size and write time.
struct FolderFiles {
    std::unordered_map<std::string, std::pair<u64, s64>> blobs;
    std::unordered_map<std::string, std::pair<u64, s64>> marks; // by the marked blob's name
};
FolderFiles ListFolder(const std::filesystem::path& dir) {
    FolderFiles out;
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator{dir, ec};
         !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
        const auto& path = it->path();
        const std::string name = path.filename().string();
        std::error_code size_error;
        const u64 size = it->file_size(size_error);
        if (size_error) {
            continue;
        }
        if (PackType(path.extension().string()) >= 0) {
            out.blobs[name] = {size, WriteTime(*it)};
        } else if (path.extension().string() == GoneSuffix) {
            out.marks[path.stem().string()] = {size, WriteTime(*it)};
        }
    }
    return out;
}

// At the open: the pack the last session folded replaces the old one; then the pack is opened,
// and the files it took in (unchanged since) are deleted.
void OpenPack(const std::filesystem::path& dir) {
    using namespace VideoCore::CachePack;
    const auto start = std::chrono::steady_clock::now();
    std::error_code ec;
    const auto pack_path = dir / PackFile;
    const auto next_path = dir / NextPackFile;
    bool held = false;
    if (std::filesystem::exists(next_path, ec)) {
        bool good = false;
        {
            Reader check;
            good = check.Open(next_path);
        }
        if (good) {
            std::filesystem::rename(next_path, pack_path, ec);
        }
        if (!good && Readable(next_path)) {
            LOG_WARNING(Render, "Pipeline cache: the last session's pack is damaged; not used");
            std::filesystem::remove(next_path, ec);
        } else if (!good || ec) {
            held = true;
            LOG_WARNING(Render, "Pipeline cache: the last session's pack could not {} (held by "
                                "another program?); tried again at the next start",
                        good ? "replace the old one" : "be read");
        }
    }
    auto reader = std::make_unique<Reader>();
    if (!reader->Open(pack_path)) {
        if (std::filesystem::exists(pack_path, ec)) {
            if (Readable(pack_path)) {
                LOG_WARNING(Render,
                            "Pipeline cache: {} is damaged; not used (its shaders compile again)",
                            pack_path.string());
                std::filesystem::remove(pack_path, ec);
            } else {
                held = true;
                LOG_WARNING(Render,
                            "Pipeline cache: {} could not be read (held by another program?); "
                            "kept for the next start",
                            pack_path.string());
            }
        }
        reader.reset();
    }
    auto files = ListFolder(dir);
    u32 taken = 0;
    if (reader) {
        for (const auto& a : reader->AbsorbedFiles()) {
            const bool mark = a.name.ends_with(GoneSuffix);
            auto& map = mark ? files.marks : files.blobs;
            const auto it = map.find(mark ? Stem(a.name) : a.name);
            if (it != map.end() && it->second == std::pair<u64, s64>{a.size, a.time}) {
                std::filesystem::remove(dir / a.name, ec);
                map.erase(it);
                ++taken;
            }
        }
    }
    std::scoped_lock lock{pack_mutex};
    loose_blobs.clear();
    gone_blobs.clear();
    for (const auto& [name, _] : files.blobs) {
        loose_blobs.insert(name);
    }
    for (const auto& [name, _] : files.marks) {
        gone_blobs.insert(name);
    }
    pack_reader = std::move(reader);
    pack_held = held;
    LOG_INFO(Render,
             "Pipeline cache: pack of {} blobs ({:.1f} MB), {} own files beside it, {} removal "
             "marks ({} files it took in deleted); {} ms",
             pack_reader ? pack_reader->Entries().size() : 0,
             pack_reader ? double(pack_reader->DataBytes()) / (1 << 20) : 0.0, loose_blobs.size(),
             gone_blobs.size(), taken,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - start)
                 .count());
}

// Where a blob ("<name>.<ext>") is read from: its own file; the pack (`out` is the blob, or empty
// when it is damaged); or nowhere - a shader blob that is neither in the pack nor a file (the
// folder's files are known since the open: no file is opened to find it missing).
enum class PackAnswer { File, Pack, Missing };
PackAnswer PackLoad(const std::string& file_name, std::vector<u8>& out) {
    if (PackType(std::filesystem::path{file_name}.extension().string()) < 0) {
        return PackAnswer::File; // the profile and the list's marker are always files
    }
    std::scoped_lock lock{pack_mutex};
    if (loose_blobs.contains(file_name)) {
        return PackAnswer::File;
    }
    const auto* entry = pack_reader && !gone_blobs.contains(file_name)
                            ? pack_reader->Find(file_name)
                            : nullptr;
    if (entry == nullptr) {
        out.clear();
        return PackAnswer::Missing;
    }
    if (!pack_reader->Read(*entry, out)) {
        LOG_WARNING(Render, "Pipeline cache: the pack's {} is damaged (it compiles again)",
                    file_name);
    }
    return PackAnswer::Pack;
}

// The packer (DataBase::PackInBackground): the pack's blobs still in use and the own files into
// a new pack beside the old one, swapped in by the next start. Once the queued writes are done.
void PackFolder(const std::stop_token& stop, std::filesystem::path dir,
                const std::atomic<u32>* pending) {
    using namespace VideoCore::CachePack;
    Common::SetCurrentThreadName("shadPS4:CachePack");
    for (int i = 0; i < 600 && pending->load() != 0; ++i) {
        if (stop.stop_requested()) {
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    const auto start = std::chrono::steady_clock::now();
    const auto files = ListFolder(dir);
    std::vector<const Entry*> keep;
    {
        std::scoped_lock lock{pack_mutex};
        if (pack_reader) {
            for (const auto& entry : pack_reader->Entries()) {
                if (!gone_blobs.contains(entry.name) && !files.blobs.contains(entry.name)) {
                    keep.push_back(&entry);
                }
            }
        }
    }
    const auto tmp = dir / NextPackTmp; // a killed session's is deleted at the next open
    std::error_code ec;
    Writer writer;
    bool ok = writer.Begin(tmp);
    std::vector<u8> data;
    for (const auto* entry : keep) {
        if (!ok || stop.stop_requested()) {
            ok = false;
            break;
        }
        bool read = false;
        {
            std::scoped_lock lock{pack_mutex};
            read = pack_reader && pack_reader->Read(*entry, data);
        }
        if (read) {
            ok = writer.Add(entry->type, entry->name, data);
        }
    }
    u32 taken = 0;
    for (const auto& [name, stat] : files.blobs) {
        if (!ok || stop.stop_requested()) {
            ok = false;
            break;
        }
        std::ifstream in(dir / name, std::ios::binary);
        data.resize(stat.first);
        if (!in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())) ||
            in.peek() != std::ifstream::traits_type::eof()) {
            continue; // changed while read: it stays its own file
        }
        const int type = PackType(std::filesystem::path{name}.extension().string());
        ok = writer.Add(u8(type), name, data);
        writer.Absorb({name, stat.first, stat.second});
        ++taken;
    }
    for (const auto& [name, stat] : files.marks) {
        writer.Absorb({name + std::string{GoneSuffix}, stat.first, stat.second});
    }
    if (!ok || !writer.Finish()) {
        std::filesystem::remove(tmp, ec);
        if (!stop.stop_requested()) {
            LOG_WARNING(Render, "Pipeline cache: the pack could not be written");
        }
        return;
    }
    std::filesystem::rename(tmp, dir / NextPackFile, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return;
    }
    LOG_INFO(Render,
             "Pipeline cache: packed {} blobs ({:.1f} MB; {} own files taken in) in {} ms; the "
             "next start reads them from the pack",
             writer.Count(), double(writer.Bytes()) / (1 << 20), taken,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - start)
                 .count());
}

void StopPacker() {
    if (packer.joinable()) {
        packer.request_stop();
        packer.join();
    }
    packer = {};
}

} // namespace

namespace Storage {

void ProcessIO(const std::stop_token& stoken) {
    Common::SetCurrentThreadName("shadPS4:PipelineCacheIO");

    while (!stoken.stop_requested()) {
        {
            std::unique_lock lk{submit_mutex};
            Common::CondvarWait(request_cv, lk, stoken, [&] { return num_requests.load() != 0; });
        }

        if (stoken.stop_requested()) {
            break;
        }

        while (num_requests) {
            std::packaged_task<void()> request{};
            {
                std::scoped_lock lock{m_request};
                if (req_queue.empty()) {
                    continue;
                }
                request = std::move(req_queue.front());
                req_queue.pop();
            }

            if (request.valid()) {
                request();
                request.get_future().wait();
            }

            --num_requests;
        }
    }
}

constexpr std::string GetBlobFileExtension(BlobType type) {
    switch (type) {
    case BlobType::ShaderMeta: {
        return "meta";
    }
    case BlobType::ShaderBinary: {
        return "spv";
    }
    case BlobType::PipelineKey: {
        return "key";
    }
    case BlobType::ShaderProfile: {
        return "bin";
    }
    default:
        UNREACHABLE();
    }
}

void DataBase::Open() {
    if (opened) {
        return;
    }

    const auto& game_info = Common::ElfInfo::Instance();

    using namespace Common::FS;
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_zero_struct(&zip_ar);

        cache_path = GetUserPath(PathType::CacheDir) /
                     std::filesystem::path{game_info.GameSerial()}.replace_extension(".zip");

        if (!mz_zip_reader_init_file(&zip_ar, cache_path.string().c_str(),
                                     MZ_ZIP_FLAG_READ_ALLOW_WRITING) ||
            !mz_zip_validate_archive(&zip_ar, 0)) {
            LOG_INFO(Render, "Cache archive {} is not found or archive is corrupted",
                     cache_path.string().c_str());
            mz_zip_reader_end(&zip_ar);
            mz_zip_writer_init_file(&zip_ar, cache_path.string().c_str(), 0);
        }
    } else {
        auto cache_root = GetUserPath(PathType::CacheDir);
#ifdef _WIN32
        // Only the pipeline/shader cache moves. User data, saves, configuration and
        // upscaler-private caches remain in the benchmark child's private profile.
        if (const auto shared = _wgetenv(L"BBVR_SHADER_CACHE_ROOT"); shared && *shared) {
            const std::filesystem::path requested{shared};
            if (requested.is_absolute()) cache_root = requested;
        }
#endif
        cache_path = cache_root / game_info.GameSerial();
        if (!Common::PrepareShaderCacheDirectory(cache_path)) {
            LOG_BENCHMARK_WARNING(Render, "Pipeline cache directory unavailable; this session uses memory only");
            RENDER_DIAGNOSTIC_ONCE("Shader disk cache directory unavailable: {}", cache_path.string());
            shadNotifications::QueueNotification(
                "Shader disk cache unavailable. This session may stutter while shaders compile.",
                12.0f, shadNotifications::position::TopLeft);
            return;
        }
#ifdef _WIN32
        if (!cache_lock.Acquire(cache_path, std::chrono::seconds{3})) {
            LOG_BENCHMARK_WARNING(Render, "Pipeline cache busy or unavailable; this session uses memory only");
            RENDER_DIAGNOSTIC_ONCE("Shader disk cache unavailable after bounded retry: error={} path={}",
                                  cache_lock.LastError(), cache_path.string());
            shadNotifications::QueueNotification(
                "Shader disk cache busy or unavailable. This session may stutter. Close other game instances before relaunching.",
                12.0f, shadNotifications::position::TopLeft);
            return;
        }
#endif
        // Writes a killed session left half done (see TmpSuffix).
        std::error_code ec;
        std::vector<std::filesystem::path> unfinished;
        for (const auto& entry : std::filesystem::directory_iterator{cache_path, ec}) {
            if (entry.path().extension().string() == TmpSuffix) {
                unfinished.push_back(entry.path());
            }
        }
        for (const auto& path : unfinished) {
            std::filesystem::remove(path, ec);
        }
        if (!unfinished.empty()) {
            LOG_INFO(Render, "Pipeline cache: removed {} unfinished writes of an earlier session",
                     unfinished.size());
        }
        OpenPack(cache_path);
    }

    num_written = 0;
    num_write_failures = 0;
    io_worker = std::jthread{ProcessIO};
    opened = true;
}

void DataBase::Close() {
    if (!IsOpened()) {
        return;
    }

    // Let the queued writes finish first (a stop request would drop them), for at most 3 s.
    const auto drain_start = std::chrono::steady_clock::now();
    u32 left = num_requests.load();
    while (left != 0 && std::chrono::steady_clock::now() - drain_start < std::chrono::seconds(3)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        left = num_requests.load();
    }
    if (left != 0) {
        LOG_WARNING(Render, "Pipeline cache: {} writes still queued at the close (dropped)", left);
    }

    StopPacker(); // a pack left half written is deleted at the next open
    io_worker.request_stop();
    io_worker.join();
    {
        std::scoped_lock lock{pack_mutex};
        pack_reader.reset();
        loose_blobs.clear();
        gone_blobs.clear();
    }

    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_finalize_archive(&zip_ar);
        mz_zip_writer_end(&zip_ar);
    }

    opened = false;
#ifdef _WIN32
    cache_lock.Release();
#endif
    LOG_INFO(Render, "Cache dumped ({} entries written this session, {} failed)",
             num_written.load(), num_write_failures.load());
}

template <typename T>
bool WriteVector(const BlobType type, std::filesystem::path&& path_, std::vector<T>&& v) {
    {
        auto request = std::packaged_task<void()>{[=]() {
            auto path{path_};
            path.replace_extension(GetBlobFileExtension(type));
            if (EmulatorSettings.IsPipelineCacheArchived()) {
                ASSERT_MSG(!ar_is_read_only,
                           "The archive is read-only. Did you forget to call `FinishPreload`?");
                if (!mz_zip_writer_add_mem(&zip_ar, path.string().c_str(), v.data(),
                                           v.size() * sizeof(T), MZ_BEST_COMPRESSION)) {
                    LOG_ERROR(Render, "Failed to add {} to the archive", path.string().c_str());
                }
            } else {
                using namespace Common::FS;
                auto tmp = path;
                tmp += TmpSuffix;
                size_t written = 0;
                {
                    const auto file = IOFile{tmp, FileAccessMode::Create};
                    written = file.Write(v);
                }
                std::error_code ec;
                if (written == v.size()) {
                    std::filesystem::rename(tmp, path, ec); // replaces an older entry
                }
                if (written != v.size() || ec) {
                    std::filesystem::remove(tmp, ec);
                    ++num_write_failures;
                    LOG_ERROR(Render, "Pipeline cache: could not write {}", path.string());
                    return;
                }
                if (PackType(path.extension().string()) >= 0) {
                    std::scoped_lock lock{pack_mutex};
                    loose_blobs.insert(path.filename().string()); // newer than the pack's
                }
            }
            ++num_written;
        }};
        std::scoped_lock lock{m_request};
        req_queue.emplace(std::move(request));
    }

    std::scoped_lock lk{submit_mutex};
    ++num_requests;
    request_cv.notify_one();
    return true;
}

template <typename T>
void LoadVector(BlobType type, std::filesystem::path& path, std::vector<T>& v) {
    using namespace Common::FS;
    path.replace_extension(GetBlobFileExtension(type));
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        int index{-1};
        index = mz_zip_reader_locate_file(&zip_ar, path.string().c_str(), nullptr, 0);
        if (index < 0) {
            LOG_WARNING(Render, "File {} is not found in the archive", path.string().c_str());
            return;
        }
        mz_zip_archive_file_stat stat{};
        mz_zip_reader_file_stat(&zip_ar, index, &stat);
        v.resize(stat.m_uncomp_size / sizeof(T));
        mz_zip_reader_extract_to_mem(&zip_ar, index, v.data(), stat.m_uncomp_size, 0);
    } else {
        if (std::vector<u8> bytes; PackLoad(path.filename().string(), bytes) != PackAnswer::File) {
            v.resize(bytes.size() / sizeof(T));
            std::memcpy(v.data(), bytes.data(), v.size() * sizeof(T));
            return;
        }
        const auto file = IOFile{path, FileAccessMode::Read};
        v.resize(file.GetSize() / sizeof(T));
        file.Read(v);
    }
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u8>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

bool DataBase::Save(BlobType type, const std::string& name, std::vector<u32>&& data) {
    if (!opened) {
        return false;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return WriteVector(type, std::move(path), std::move(data));
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u8>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::Load(BlobType type, const std::string& name, std::vector<u32>& data) {
    if (!opened) {
        return;
    }

    auto path = EmulatorSettings.IsPipelineCacheArchived() ? std::filesystem::path{name}
                                                           : cache_path / name;
    return LoadVector(type, path, data);
}

void DataBase::ForEachBlob(BlobType type, const std::function<void(std::vector<u8>&& data)>& func) {
    ForEachBlob(type, [&](const std::string&, std::vector<u8>&& data) { func(std::move(data)); });
}

void DataBase::ForEachBlob(
    BlobType type, const std::function<void(const std::string& name, std::vector<u8>&& data)>& func) {
    const auto& ext = GetBlobFileExtension(type);
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (int index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            file_name.fill(0);
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            if (std::string{file_name.data()}.ends_with(ext)) {
                mz_zip_archive_file_stat stat{};
                mz_zip_reader_file_stat(&zip_ar, index, &stat);
                std::vector<u8> data(stat.m_uncomp_size);
                mz_zip_reader_extract_to_mem(&zip_ar, index, data.data(), data.size(), 0);
                func(std::filesystem::path{file_name.data()}.stem().string(), std::move(data));
            }
        }
    } else {
        // The pack's blobs of this type that no own file or removal replaces, then the own files.
        std::vector<const VideoCore::CachePack::Entry*> packed;
        {
            std::scoped_lock lock{pack_mutex};
            if (pack_reader) {
                for (const auto& entry : pack_reader->Entries()) {
                    if (entry.type == u8(type) && !loose_blobs.contains(entry.name) &&
                        !gone_blobs.contains(entry.name)) {
                        packed.push_back(&entry);
                    }
                }
            }
        }
        for (const auto* entry : packed) {
            std::vector<u8> data;
            bool read = false;
            {
                std::scoped_lock lock{pack_mutex};
                read = pack_reader && pack_reader->Read(*entry, data);
            }
            if (read) {
                func(Stem(entry->name), std::move(data));
            }
        }
        for (const auto& file_name : std::filesystem::directory_iterator{cache_path}) {
            if (file_name.path().extension().string().ends_with(ext)) {
                using namespace Common::FS;
                const auto& file = IOFile{file_name, FileAccessMode::Read};
                if (file.IsOpen()) {
                    std::vector<u8> data(file.GetSize());
                    file.Read(data);
                    func(file_name.path().stem().string(), std::move(data));
                }
            }
        }
    }
}

void DataBase::ForEachName(
    const std::function<void(BlobType type, const std::string& name)>& func) {
    if (!opened) {
        return;
    }
    // The blob type of a file name's extension (".meta", ...). A stem with a dot of its own is no
    // blob's name (Save, Load and Remove replace the extension after the last dot).
    const auto visit = [&func](const std::filesystem::path& path) {
        const std::string extension = path.extension().string();
        const std::string stem = path.stem().string();
        if (extension.size() < 2 || extension[0] != '.' || stem.empty() ||
            stem.find('.') != std::string::npos) {
            return;
        }
        for (const BlobType type : {BlobType::ShaderMeta, BlobType::ShaderBinary,
                                    BlobType::PipelineKey, BlobType::ShaderProfile}) {
            if (std::string_view{extension}.substr(1) == GetBlobFileExtension(type)) {
                func(type, stem);
                return;
            }
        }
    };
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        const auto num_files = mz_zip_reader_get_num_files(&zip_ar);
        for (mz_uint index = 0; index < num_files; ++index) {
            std::array<char, MZ_ZIP_MAX_ARCHIVE_FILENAME_SIZE> file_name{};
            mz_zip_reader_get_filename(&zip_ar, index, file_name.data(), file_name.size());
            visit(std::filesystem::path{file_name.data()});
        }
        return;
    }
    {
        // The pack's blobs (an own file of the same name is visited below).
        std::scoped_lock lock{pack_mutex};
        if (pack_reader) {
            for (const auto& entry : pack_reader->Entries()) {
                if (!loose_blobs.contains(entry.name) && !gone_blobs.contains(entry.name)) {
                    visit(std::filesystem::path{entry.name});
                }
            }
        }
    }
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator{cache_path, ec};
         !ec && it != std::filesystem::directory_iterator{}; it.increment(ec)) {
        visit(it->path());
    }
}

bool DataBase::Remove(BlobType type, const std::string& name) {
    if (!opened || EmulatorSettings.IsPipelineCacheArchived()) {
        return false; // the archive cannot drop single entries
    }
    auto path = cache_path / name;
    path.replace_extension(GetBlobFileExtension(type));
    std::error_code ec;
    const bool removed = std::filesystem::remove(path, ec);
    // In the pack (or perhaps in the one being written): a removal mark hides it.
    const std::string file_name = path.filename().string();
    bool packed = false;
    {
        std::scoped_lock lock{pack_mutex};
        loose_blobs.erase(file_name);
        packed = pack_reader && pack_reader->Find(file_name) != nullptr &&
                 !gone_blobs.contains(file_name);
        if (!packed && !packer_started.load()) {
            return removed;
        }
    }
    auto mark = path;
    mark += GoneSuffix;
    if (std::ofstream{mark, std::ios::binary | std::ios::trunc}) {
        std::scoped_lock lock{pack_mutex};
        gone_blobs.insert(file_name);
    }
    return removed || packed;
}

bool DataBase::Clear() {
    if (!opened || EmulatorSettings.IsPipelineCacheArchived()) {
        return false;
    }
    StopPacker();
    {
        std::scoped_lock lock{pack_mutex};
        pack_reader.reset();
        loose_blobs.clear();
        gone_blobs.clear();
    }
    std::error_code ec;
    std::vector<std::filesystem::path> doomed;
    for (const auto& entry : std::filesystem::directory_iterator{cache_path, ec}) {
        const auto ext = entry.path().extension().string();
        const auto name = entry.path().filename().string();
        if (ext == ".key" || ext == ".meta" || ext == ".spv" || ext == ".bin" || ext == TmpSuffix ||
            ext == GoneSuffix || name == PackFile || name == NextPackFile) {
            doomed.push_back(entry.path());
        }
    }
    for (const auto& path : doomed) {
        std::filesystem::remove(path, ec);
    }
    return true;
}

void DataBase::PackInBackground() {
    if (!opened || EmulatorSettings.IsPipelineCacheArchived() || packer.joinable()) {
        return;
    }
    {
        std::scoped_lock lock{pack_mutex};
        const size_t own = loose_blobs.size() + num_requests.load();
        if (pack_held ||
            (own < MinPackFiles && gone_blobs.size() < MinPackFiles && (pack_reader || own == 0))) {
            return;
        }
    }
    packer_started = true;
    packer = std::jthread{[dir = cache_path](std::stop_token stop) {
        PackFolder(stop, dir, &num_requests);
    }};
}

void DataBase::FinishPreload() {
    if (EmulatorSettings.IsPipelineCacheArchived()) {
        mz_zip_writer_init_from_reader(&zip_ar, cache_path.string().c_str());
        ar_is_read_only = false;
    }
}

} // namespace Storage
