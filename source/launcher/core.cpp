#include "core.h"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <dxgi.h>
#include <fstream>
#include <iterator>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdexcept>
#include <system_error>
#include <tlhelp32.h>
#include <wrl/client.h>

namespace Launcher {
static void WinError(const char *message) {
    throw std::system_error(int(GetLastError()), std::system_category(), message);
}
std::string Utf8(std::wstring_view text) {
    if (text.empty())
        return {};
    const int size = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(),
                                         int(text.size()), nullptr, 0, nullptr, nullptr);
    if (!size)
        WinError("Invalid Unicode");
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), out.data(),
                        size, nullptr, nullptr);
    return out;
}
std::wstring Wide(std::string_view text) {
    if (text.empty())
        return {};
    const int size = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(),
                                         int(text.size()), nullptr, 0);
    if (!size)
        WinError("Invalid UTF-8");
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), out.data(),
                        size);
    return out;
}
std::string PathText(const fs::path &path) {
    return Utf8(path.native());
}
fs::path Path(std::string_view text) {
    std::string clean(VR::Ini::Trim(text));
    if (clean.size() >= 2 && clean.front() == '"' && clean.back() == '"')
        clean = clean.substr(1, clean.size() - 2);
    if (clean.empty())
        throw std::runtime_error("Choose a folder or file first.");
    return fs::absolute(fs::path(Wide(clean))).lexically_normal();
}
// error_code overloads: a runtime on a share that went away must not throw mid-frame.
static bool IsFile(const fs::path &path) {
    std::error_code error;
    return fs::is_regular_file(path, error);
}
std::string Read(const fs::path &path) {
    if (!fs::exists(path))
        return {};
    if (fs::file_size(path) > 4 * 1024 * 1024)
        throw std::runtime_error("Configuration file is too large.");
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read " + PathText(path));
    std::string text((std::istreambuf_iterator<char>(file)), {});
    if (file.bad())
        throw std::runtime_error("Cannot finish reading " + PathText(path));
    return text;
}
fs::path Write(const fs::path &path, const std::string &text,
               const std::optional<std::string> &expected) {
    fs::create_directories(path.parent_path());
    const auto lock_path = fs::path(path.native() + L".lock");
    // The game's writer holds the same lock briefly (vr_ini.h FileLock); one older than its
    // LockStaleMs belongs to a writer that crashed, and is taken over as the game does.
    HANDLE lock = INVALID_HANDLE_VALUE;
    for (int attempt = 0; lock == INVALID_HANDLE_VALUE; ++attempt) {
        lock = CreateFileW(lock_path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (lock != INVALID_HANDLE_VALUE)
            break;
        if (GetLastError() != ERROR_FILE_EXISTS || attempt == 20)
            WinError("The configuration is locked by another writer");
        std::error_code error;
        const auto written = fs::last_write_time(lock_path, error);
        if (!error && fs::file_time_type::clock::now() - written >
                          std::chrono::milliseconds(VR::Ini::LockStaleMs))
            fs::remove(lock_path, error);
        else
            Sleep(50);
    }
    struct Lease {
        HANDLE h;
        ~Lease() {
            CloseHandle(h);
        }
    } lease{lock};
    const auto before = Read(path);
    if (expected && before != *expected)
        throw std::runtime_error("This file changed outside the launcher. Reload before saving.");
    if (before == text && fs::exists(path))
        return {};
    const auto stamp = std::to_wstring(std::chrono::system_clock::now().time_since_epoch().count());
    fs::path backup;
    if (fs::exists(path)) {
        backup = fs::path(path.native() + L"." + stamp + L".bak");
        fs::copy_file(path, backup);
    }
    const auto temp = fs::path(path.native() + L"." + stamp + L".tmp");
    HANDLE out = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE)
        WinError("Cannot create temporary configuration");
    DWORD written = 0;
    const bool ok = WriteFile(out, text.data(), DWORD(text.size()), &written, nullptr) &&
                    written == text.size() && FlushFileBuffers(out);
    CloseHandle(out);
    if (!ok) {
        DeleteFileW(temp.c_str());
        throw std::runtime_error("Could not save the configuration.");
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto error = GetLastError();
        DeleteFileW(temp.c_str());
        SetLastError(error);
        WinError("Cannot replace configuration");
    }
    return backup;
}
std::wstring Quote(std::wstring_view text) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : text) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        out.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"')
            out += L'\\';
        out += c;
    }
    out.append(slashes * 2, L'\\');
    return out + L'"';
}
fs::path NormalizeSaveHome(const fs::path &path) {
    if (path.empty())
        throw std::runtime_error("Choose a save home folder.");
    auto full = fs::absolute(path).lexically_normal();
    for (auto p = full; p != p.root_path() && !p.empty(); p = p.parent_path()) {
        if (_wcsicmp(p.filename().c_str(), L"savedata") == 0) {
            const auto user = p.parent_path().filename().native();
            if (!user.empty() && std::all_of(user.begin(), user.end(),
                                             [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
                return p.parent_path().parent_path();
        }
    }
    if (full.filename().native().starts_with(L"CUSA"))
        throw std::runtime_error("Choose the home folder containing 1000 / savedata / CUSA..., "
                                 "rather than an individual save.");
    return full;
}
fs::path GameExecutable(const fs::path &path) {
    const auto exe = fs::is_directory(path) ? path / "eboot.bin" : path;
    if (!fs::is_regular_file(exe) || _wcsicmp(exe.filename().c_str(), L"eboot.bin") != 0)
        throw std::runtime_error("Choose the game folder containing eboot.bin.");
    if (!fs::is_regular_file(exe.parent_path() / "sce_sys" / "param.sfo"))
        throw std::runtime_error("The game folder is missing sce_sys / param.sfo.");
    return exe;
}
// The measured 90 Hz native headset preset (scratchpad/dual-eye-perf/try_native.py
// --native-asymmetric --rate 90), without its diagnostics. FAST_TOGGLES is left to the build's
// own default mask, which is the preset's.
static constexpr int RefreshHz = 90;
static constexpr const wchar_t *RendererEnvironment[]{
    L"VR_NATIVE_STEREO=1",
    // Each eye renders exactly its lens frustum (and the headset-size resolution template).
    L"VR_NATIVE_ASYMMETRIC=1",
    L"VR_STREAM_MEM=shadow",
    L"VR_NATIVE_REUSE_DESCRIPTORS=1",
    L"VR_NATIVE_REUSE_CAMERA=1",
    L"VR_NATIVE_COALESCE_COPIES=1",
    L"VR_STEREO_MIN_HOLD_ONE=1",
    L"VR_REUSE_SLEEP_TIMER=1",
    L"VR_FAST_MUTEX=1",
    L"VR_NATIVE_BATCH_REGISTRY=1",
    L"VR_LARGE_BIND_MEMO=1",
    L"VR_EXTRA_LARGE_BIND_MEMO=1",
    L"VR_RETIRED_UPLOAD_PREFLIGHT=1",
    L"VR_REUSE_READONLY_LOOKUP=1",
    L"VR_REUSE_FETCH_STORAGE=1",
    L"VR_PREFLIGHT_PROJECTION=1",
    L"VR_MOVE_FETCH_METADATA=1",
    L"VR_BATCH_SMALL_UPLOADS=1",
    L"VR_SPARSE_RANGE_CACHE=1",
    L"VR_NATIVE_PUSH_DESCRIPTORS=1",
    L"VR_NATIVE_SWAP_WRITES=1",
    L"VR_NATIVE_DIRECT_IMAGE_BUFFER=1",
    L"VR_SERIAL_INPUT_FASTPATH=1",
    L"VR_NATIVE_HLE_OUTER_BARRIERS=1",
    L"VR_COPY_MERGE_KB=16",
    L"VR_DIRECT_BUFFER_METADATA=0",
};
static const std::vector<VR::Ini::Edit> &RendererIni() {
    static const std::vector<VR::Ini::Edit> edits{
        {"stereo_mode", "2"},         {"dibr_seen", "1"},
        {"vfov_deg", "0"},            {"render_pitch_bias_deg", "0"},
        {"fov_margin_deg", "6"},      {"refresh_hz", std::to_string(RefreshHz)},
        {"frame_cap", "auto"},        {"ui_layer", "1"},
        {"menu_present", "layer"},    {"vr_menu", "1"},
        {"late_warp", "0"},           {"eye_size", "auto"},
        {"eye_sharpen", "0"}};
    return edits;
}
std::string LaunchConfig(const std::string &text, const fs::path &home) {
    auto json = text.empty() ? Json::object() : Json::parse(text);
    if (!json.is_object())
        throw std::runtime_error("config.json must contain an object.");
    for (const char *section : {"General", "GPU", "Vulkan"})
        if (json.contains(section) && !json[section].is_object())
            throw std::runtime_error(std::string("config.json has an invalid ") + section +
                                     " section.");
    auto &general = json["General"];
    general["home_dir"] = PathText(home);
    // The VR resolution template allocates its eye targets from this.
    general["extra_dmem_in_mbytes"] = 4000;
    general["show_splash"] = false;
    general["discord_rpc_enabled"] = false;
    auto &gpu = json["GPU"];
    gpu["vblank_frequency"] = RefreshHz;
    gpu["dump_shaders"] = false;
    // Learned pipelines are kept and preloaded, so later launches compile less.
    auto &vulkan = json["Vulkan"];
    vulkan["pipeline_cache_enabled"] = true;
    vulkan["pipeline_cache_archived"] = false;
    vulkan["vkvalidation_enabled"] = false;
    vulkan["vkvalidation_sync_enabled"] = false;
    vulkan["vkvalidation_gpu_enabled"] = false;
    vulkan["vkcrash_diagnostic_enabled"] = false;
    vulkan["renderdoc_enabled"] = false;
    vulkan["vkguest_markers"] = false;
    vulkan["vkhost_markers"] = false;
    return json.dump(2) + "\n";
}
std::string LaunchIni(const std::string &text) {
    return VR::Ini::ApplyEdits(text, RendererIni());
}
std::wstring NativeIpd(const std::string &ini) {
    float mm = 0;
    if (const auto value = VR::Ini::ValueOf(ini, "ipd_mm")) {
        try {
            mm = std::stof(*value);
        } catch (const std::exception &) {
        }
    }
    // 0 (or anything the game would not accept) is Standard: 64 mm. The page allows 40..80, and
    // Menu + stick in game up to 120.
    if (!(mm >= 40 && mm <= 120))
        mm = 64;
    return std::to_wstring(mm / 1000);
}
fs::path PatchFile(const fs::path &runtime) {
    for (const auto &folder :
         {runtime.parent_path(), ExecutableDirectory() / "runtime", ExecutableDirectory()}) {
        const auto file = folder / "bbvr_patch.xml";
        if (fs::is_regular_file(file))
            return file;
    }
    throw std::runtime_error("bbvr_patch.xml is missing. Use a complete package, or place it next "
                             "to your custom shadps4.exe.");
}
std::optional<std::string> AppVersion(const fs::path &param_sfo) {
    // param.sfo: magic, version, key table, data table, count; then 16-byte index entries.
    std::ifstream file(param_sfo, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(file)), {});
    auto u32 = [&](size_t at) -> uint32_t {
        if (at + 4 > data.size())
            return 0;
        return uint32_t(uint8_t(data[at])) | uint32_t(uint8_t(data[at + 1])) << 8 |
               uint32_t(uint8_t(data[at + 2])) << 16 | uint32_t(uint8_t(data[at + 3])) << 24;
    };
    if (data.size() < 20 || data.compare(0, 4, std::string("\0PSF", 4)) != 0)
        return std::nullopt;
    const auto keys = u32(8), values = u32(12), count = u32(16);
    for (uint32_t i = 0; i < count && 20 + 16 * (i + 1) <= data.size(); ++i) {
        const size_t entry = 20 + 16 * i;
        const size_t key = keys + (uint8_t(data[entry]) | uint8_t(data[entry + 1]) << 8);
        const auto length = u32(entry + 4), offset = u32(entry + 12);
        if (key >= data.size() || data.compare(key, 8, std::string("APP_VER\0", 8)) != 0)
            continue;
        if (values + offset + length > data.size())
            return std::nullopt;
        auto text = data.substr(values + offset, length);
        return text.substr(0, text.find('\0'));
    }
    return std::nullopt;
}
void CheckGameBuild(const fs::path &eboot) {
    // Frame-limiter code the patch file's sleep path and 1 ms tail edit (file offset = segment
    // offset 0x28eb0 + address - 0x400000), as run_probe.py checks it.
    struct Site {
        std::uint32_t address;
        std::uint8_t bytes[6];
        std::size_t size;
    };
    static constexpr Site sites[]{{0x02434ADA, {0x0f, 0x84, 0x9a, 0x00, 0x00, 0x00}, 6},
                                  {0x02434BAC, {0xe4, 0x47, 0x8f, 0x02}, 4},
                                  {0x02434BB6, {0xde, 0x47, 0x8f, 0x02}, 4},
                                  {0x04D29390, {0x6f, 0x12, 0x83, 0x3a}, 4},
                                  {0x04D29558, {0x6f, 0x12, 0x83, 0xba}, 4}};
    std::ifstream file(eboot, std::ios::binary);
    for (const auto &site : sites) {
        std::uint8_t read[6]{};
        file.seekg(std::streamoff(0x28eb0) + site.address - 0x400000);
        file.read(reinterpret_cast<char *>(read), std::streamsize(site.size));
        if (!file || !std::equal(read, read + site.size, site.bytes))
            throw std::runtime_error("This game build is not supported. BB-VR needs Bloodborne "
                                     "version 1.09 (eboot.bin unmodified).");
    }
    // Every patch in bbvr_patch.xml is for AppVer 01.00: the game skips them all for another
    // param.sfo APP_VER, silently.
    if (const auto version = AppVersion(eboot.parent_path() / "sce_sys" / "param.sfo");
        version != "01.00")
        throw std::runtime_error("This game's version (APP_VER " + version.value_or("unknown") +
                                 ") is not supported. BB-VR needs Bloodborne 1.09 (APP_VER 01.00).");
}
std::wstring ArgumentPath(const fs::path &path) {
    const auto &text = path.native();
    if (std::all_of(text.begin(), text.end(), [](wchar_t c) { return c < 128; }))
        return text;
    // The Windows short name is plain ASCII when the volume keeps short names.
    std::wstring short_name(32768, L'\0');
    const DWORD size = GetShortPathNameW(text.c_str(), short_name.data(), DWORD(short_name.size()));
    short_name.resize(size < short_name.size() ? size : 0);
    if (!short_name.empty() && std::all_of(short_name.begin(), short_name.end(),
                                           [](wchar_t c) { return c < 128; }))
        return short_name;
    // Still fine when the system code page holds every character.
    BOOL lossy = FALSE;
    WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, text.c_str(), int(text.size()), nullptr, 0,
                        nullptr, &lossy);
    if (!lossy)
        return text;
    throw std::runtime_error("The game cannot open " + PathText(path) +
                             ": its name has characters Windows passes on as '?'. Move it to a "
                             "folder whose name uses plain English letters.");
}
fs::path ExecutableDirectory() {
    std::wstring path(32768, L'\0');
    const auto size = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!size || size >= path.size())
        WinError("Cannot locate the launcher");
    path.resize(size);
    return fs::path(path).parent_path();
}
Paths Paths::Default() {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &value)))
        throw std::runtime_error("Cannot locate your local application data.");
    fs::path root = fs::path(value) / "BloodborneVRData";
    CoTaskMemFree(value);
    return {root};
}
Profile Profile::Load(const Paths &paths) {
    Profile out;
    const auto text = Read(paths.Profile());
    if (!text.empty()) {
        const auto json = Json::parse(text);
        out.game = json.value("game", "");
        out.saves = json.value("saves", "");
        out.runtime = json.value("runtime", "");
        out.fullscreen = json.value("fullscreen", true);
        out.check_updates = json.value("check_updates", false);
        out.use_bundled_runtime = json.value("use_bundled_runtime", true);
    }
    if (out.saves.empty()) {
        out.saves = PathText(paths.User() / "home");
        fs::create_directories(Path(out.saves));
    }
    const auto bundled = ExecutableDirectory() / "runtime" / "shadps4.exe";
    if (out.use_bundled_runtime)
        out.runtime = fs::is_regular_file(bundled) ? PathText(bundled) : std::string{};
    return out;
}
void Profile::Save(const Paths &paths) const {
    Write(paths.Profile(), Json{{"game", game},
                                {"saves", saves},
                                {"runtime", runtime},
                                {"fullscreen", fullscreen},
                                {"check_updates", check_updates},
                                {"use_bundled_runtime", use_bundled_runtime}}
                                   .dump(2) +
                               "\n");
}
void ValidateSetup(const Profile &profile) {
    if (profile.game.empty())
        throw std::runtime_error("Choose your game folder to begin.");
    GameExecutable(Path(profile.game));
    const auto runtime = Path(profile.runtime);
    if (!fs::is_regular_file(runtime) || _wcsicmp(runtime.filename().c_str(), L"shadps4.exe") != 0)
        throw std::runtime_error(
            "Select shadps4.exe in Runtime and setup, or use a complete package.");
    const auto home = NormalizeSaveHome(Path(profile.saves));
    if (!fs::is_directory(home))
        throw std::runtime_error("The save home folder does not exist.");
    auto installation = ExecutableDirectory();
    if (installation.filename() == "current" &&
        fs::exists(installation.parent_path() / "Update.exe"))
        installation = installation.parent_path();
    const auto install_text = fs::weakly_canonical(installation).native();
    const auto home_text = fs::weakly_canonical(home).native();
    const auto prefix = install_text + fs::path::preferred_separator;
    if (_wcsicmp(home_text.c_str(), install_text.c_str()) == 0 ||
        _wcsnicmp(home_text.c_str(), prefix.c_str(), prefix.size()) == 0)
        throw std::runtime_error("Choose a save home outside the application installation so "
                                 "updates and uninstalling cannot remove it.");
}
fs::path SaveBackupRoot(const Paths &paths) {
    return paths.root / "save_backups";
}
static bool Numbered(const fs::path &name) {
    const auto text = name.native();
    return !text.empty() &&
           std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
}
// Bloodborne's save folders (the title IDs bbvr_patch.xml lists): other games' saves in a shared
// shadPS4 home are neither copied nor touched.
static bool BloodborneSave(const fs::path &name) {
    static constexpr const wchar_t *ids[]{L"CUSA00207", L"CUSA00208", L"CUSA00299", L"CUSA00900",
                                          L"CUSA01363", L"CUSA03014", L"CUSA03023", L"CUSA03173"};
    return std::any_of(std::begin(ids), std::end(ids),
                       [&](const wchar_t *id) { return _wcsicmp(name.c_str(), id) == 0; });
}
// home/<user>/savedata/<Bloodborne ID> folders, as paths relative to `root`.
static std::vector<fs::path> SaveFolders(const fs::path &root) {
    std::vector<fs::path> out;
    std::error_code error;
    for (const auto &user : fs::directory_iterator(root, error)) {
        if (!user.is_directory() || !Numbered(user.path().filename()))
            continue;
        for (const auto &game : fs::directory_iterator(user.path() / "savedata", error))
            if (game.is_directory() && BloodborneSave(game.path().filename()))
                out.push_back(fs::relative(game.path(), root));
    }
    std::sort(out.begin(), out.end());
    return out;
}
// Every save file's path, size and time: equal fingerprints mean nothing changed.
static std::string SaveFingerprint(const fs::path &root) {
    std::vector<std::string> lines;
    for (const auto &folder : SaveFolders(root))
        for (const auto &file : fs::recursive_directory_iterator(root / folder)) {
            if (!file.is_regular_file())
                continue;
            lines.push_back(PathText(fs::relative(file.path(), root)) + "|" +
                            std::to_string(file.file_size()) + "|" +
                            std::to_string(file.last_write_time().time_since_epoch().count()));
        }
    std::sort(lines.begin(), lines.end());
    std::string out;
    for (const auto &line : lines)
        out += line + "\n";
    return out;
}
// Folder names are UTC (YYYYMMDD-HHMMSS-mmm[-NN]) so they sort by age even when the clock's
// time zone or daylight saving changes; the label is the local time.
static std::string BackupLabel(const std::string &name) {
    SYSTEMTIME utc{}, local{};
    utc.wYear = WORD(std::atoi(name.substr(0, 4).c_str()));
    utc.wMonth = WORD(std::atoi(name.substr(4, 2).c_str()));
    utc.wDay = WORD(std::atoi(name.substr(6, 2).c_str()));
    utc.wHour = WORD(std::atoi(name.substr(9, 2).c_str()));
    utc.wMinute = WORD(std::atoi(name.substr(11, 2).c_str()));
    utc.wSecond = WORD(std::atoi(name.substr(13, 2).c_str()));
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local))
        local = utc;
    char text[32];
    snprintf(text, sizeof(text), "%04u-%02u-%02u %02u:%02u:%02u", local.wYear, local.wMonth,
             local.wDay, local.wHour, local.wMinute, local.wSecond);
    return text;
}
std::vector<SaveBackup> SaveBackups(const Paths &paths) {
    std::vector<SaveBackup> out;
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(SaveBackupRoot(paths), error)) {
        const auto name = PathText(entry.path().filename());
        // A partial copy starts with '.'.
        if (!entry.is_directory() || name.size() < 19 || name[0] == '.' ||
            !fs::exists(entry.path() / "fingerprint.txt"))
            continue;
        out.push_back({entry.path(), BackupLabel(name)});
    }
    std::sort(out.begin(), out.end(), [](const auto &a, const auto &b) {
        return a.folder.filename().native() > b.folder.filename().native();
    });
    return out;
}
fs::path BackupSaves(const Paths &paths, const fs::path &home) {
    if (!fs::is_directory(home))
        return {};
    const auto fingerprint = SaveFingerprint(home);
    if (fingerprint.empty())
        return {};
    auto backups = SaveBackups(paths);
    if (!backups.empty() && Read(backups.front().folder / "fingerprint.txt") == fingerprint)
        return {};
    const auto root = SaveBackupRoot(paths);
    fs::create_directories(root);
    // Partial copies a failed backup left behind.
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(root, error))
        if (entry.path().filename().native().starts_with(L"."))
            fs::remove_all(entry.path(), error);
    SYSTEMTIME t{};
    GetSystemTime(&t);
    wchar_t stamp[32]{};
    swprintf_s(stamp, L"%04u%02u%02u-%02u%02u%02u-%03u", t.wYear, t.wMonth, t.wDay, t.wHour,
               t.wMinute, t.wSecond, t.wMilliseconds);
    auto folder = root / stamp;
    for (int n = 2; fs::exists(folder); ++n) {
        wchar_t suffix[8]{};
        swprintf_s(suffix, L"-%02d", n);
        folder = root / (std::wstring(stamp) + suffix);
    }
    const auto partial = root / (L"." + folder.filename().native());
    for (const auto &save : SaveFolders(home)) {
        fs::create_directories((partial / save).parent_path());
        fs::copy(home / save, partial / save, fs::copy_options::recursive);
    }
    Write(partial / "fingerprint.txt", fingerprint);
    fs::rename(partial, folder);
    // Retire the oldest, never the one just made (a clock set back would sort it last).
    backups = SaveBackups(paths);
    size_t kept = 1;
    for (const auto &backup : backups)
        if (backup.folder != folder && ++kept > MaxSaveBackups)
            fs::remove_all(backup.folder, error);
    return folder;
}
void RestoreSaves(const Paths &paths, const fs::path &home, const fs::path &backup) {
    if (!fs::exists(backup / "fingerprint.txt"))
        throw std::runtime_error("That backup is incomplete.");
    const auto saves = SaveFolders(backup);
    if (saves.empty())
        throw std::runtime_error("That backup holds no saves.");
    // Stage the copy first: backing up the current saves may retire the oldest backup.
    std::vector<std::pair<fs::path, fs::path>> staged;
    for (const auto &save : saves) {
        const auto target = home / save;
        const auto stage = fs::path(target.native() + L".restoring");
        fs::remove_all(stage);
        fs::create_directories(stage.parent_path());
        fs::copy(backup / save, stage, fs::copy_options::recursive);
        staged.push_back({stage, target});
    }
    BackupSaves(paths, home);
    for (const auto &[stage, target] : staged) {
        const auto replaced = fs::path(target.native() + L".replaced");
        std::error_code error;
        fs::remove_all(replaced, error);
        const bool had = fs::exists(target);
        if (had)
            fs::rename(target, replaced);
        try {
            fs::rename(stage, target);
        } catch (const std::exception &) {
            // Put the current saves back; they are also in the newest backup.
            if (had)
                fs::rename(replaced, target, error);
            throw std::runtime_error("Could not restore the saves (a file may be in use). Your "
                                     "current saves were kept.");
        }
        fs::remove_all(replaced, error);
    }
}
GraphicsFields ParseGraphics(const std::optional<std::string> &value) {
    if (!value)
        return DefaultGraphics;
    GraphicsFields out{};
    size_t at = 0;
    for (size_t i = 0; i < out.size(); ++i) {
        const auto end = i + 1 < out.size() ? value->find(',', at) : value->size();
        if (end == std::string::npos)
            return DefaultGraphics;
        const auto *first = value->data() + at, *last = value->data() + end;
        if (std::from_chars(first, last, out[i]).ptr != last || first == last)
            return DefaultGraphics;
        at = end + 1;
    }
    // The game's own limits (graphics_settings.h Parse).
    const bool valid = out[0] == 1 && out[1] >= -50 && out[1] <= 50 && out[1] % 10 == 0 &&
                       out[2] >= 0 && out[2] <= 2 && out[3] >= 0 && out[3] <= 2 && out[4] >= 0 &&
                       out[4] <= 2 && out[5] >= 0 && out[5] <= 1 && out[6] >= 0 && out[6] <= 2 &&
                       std::all_of(out.begin() + 7, out.end(),
                                   [](int v) { return v >= 0 && v <= 100; });
    return valid ? out : DefaultGraphics;
}
std::string GraphicsText(const GraphicsFields &fields) {
    std::string out;
    for (const int v : fields)
        out += (out.empty() ? "" : ",") + std::to_string(v);
    return out;
}
bool DlssIncluded(const fs::path &runtime) {
    const auto folder = runtime.parent_path() / "dlss";
    return IsFile(folder / "nvngx_dlss.dll") && IsFile(folder / "vr_dlss_stereo.dll");
}
bool Fsr4Included(const fs::path &runtime) {
    const auto folder = runtime.parent_path() / "fsr4";
    return IsFile(folder / "vr_fsr4_stereo.dll") &&
           IsFile(folder / "fsr4_shaders" / "LICENSE-FSR4-v07.txt");
}
DWORD RestartedProcess(DWORD pid) {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return 0;
    PROCESSENTRY32W entry{sizeof(entry)};
    DWORD found = 0;
    for (BOOL next = Process32FirstW(snapshot, &entry); next && !found;
         next = Process32NextW(snapshot, &entry))
        if (entry.th32ParentProcessID == pid && _wcsicmp(entry.szExeFile, L"shadps4.exe") == 0)
            found = entry.th32ProcessID;
    CloseHandle(snapshot);
    return found;
}
std::string ExitReason(DWORD code) {
    switch (code) {
    case 0:
        return {};
    case 1:
        return "it could not start";
    case 0x80000003:
        return "an internal check failed";
    case 0xC0000005:
        return "a memory access error";
    case 0xC00000FD:
        return "a stack overflow";
    case 0xC0000409:
        return "a fatal runtime error";
    case 20000:
    case 20001:
    case 20002:
        return "an unhandled error";
    }
    char text[32];
    snprintf(text, sizeof(text), "error 0x%08lX", code);
    return text;
}
fs::path LogFile(const Paths &paths) {
    const auto folder = paths.User() / "log";
    if (IsFile(folder / "shad_log.txt"))
        return folder / "shad_log.txt";
    fs::path newest;
    fs::file_time_type newest_time{};
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(folder, error)) {
        const auto time = entry.last_write_time(error);
        if (!error && entry.path().extension() == ".log" && (newest.empty() || time > newest_time)) {
            newest = entry.path();
            newest_time = time;
        }
    }
    return newest;
}
std::vector<std::string> LogProblems(const Paths &paths, size_t max) {
    std::vector<std::string> out;
    const auto file = LogFile(paths);
    if (file.empty())
        return out;
    // The tail is enough: a crash is logged last.
    std::ifstream in(file, std::ios::binary);
    in.seekg(0, std::ios::end);
    const auto size = std::streamoff(in.tellg());
    if (!in || size <= 0)
        return out;
    const auto start = std::max<std::streamoff>(0, size - 256 * 1024);
    in.seekg(start);
    std::string tail(size_t(size - start), '\0');
    in.read(tail.data(), std::streamsize(tail.size()));
    size_t at = 0;
    while (at < tail.size()) {
        auto end = tail.find('\n', at);
        if (end == std::string::npos)
            end = tail.size();
        auto line = tail.substr(at, end - at);
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.find("<Critical>") != std::string::npos ||
            line.find("<Error>") != std::string::npos) {
            out.push_back(line.size() > 400 ? line.substr(0, 400) + "..." : line);
            if (out.size() > max)
                out.erase(out.begin());
        }
        at = end + 1;
    }
    return out;
}
bool ShaderCacheReady(const Paths &paths) {
    std::error_code error;
    for (const auto &entry : fs::directory_iterator(paths.User() / "cache", error))
        if (entry.is_directory(error) && IsFile(entry.path() / "profile.bin"))
            return true;
    return false;
}
std::string OpenXrRuntime() {
    wchar_t value[4096]{};
    DWORD size = sizeof(value);
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime",
                     RRF_RT_REG_SZ, nullptr, value, &size) != ERROR_SUCCESS ||
        !IsFile(value))
        return {};
    try {
        const auto json = Json::parse(Read(value));
        const auto name = json.at("runtime").value("name", std::string{});
        if (!name.empty())
            return name;
    } catch (const std::exception &) {
    }
    return PathText(fs::path(value).stem());
}
bool Dlss5Included(const fs::path &runtime) {
    if (!DlssIncluded(runtime))
        return false;
    // Mapped without running it or loading what it imports: only its export table is read.
    const HMODULE bridge = LoadLibraryExW((runtime.parent_path() / "dlss" / "vr_dlss_stereo.dll").c_str(),
                                          nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!bridge)
        return false;
    const bool neural = GetProcAddress(bridge, "VrDlssSetNeuralConfig") != nullptr;
    FreeLibrary(bridge);
    return neural;
}
std::string NvidiaDriver(uint64_t umd) {
    // 32.0.15.6094 -> 560.94: the last digit of the third part and the fourth part.
    const auto value = (umd >> 16 & 0xffff) % 10 * 10000 + (umd & 0xffff);
    const auto text = std::to_string(value);
    return text.size() < 3 ? text : text.substr(0, text.size() - 2) + "." + text.substr(text.size() - 2);
}
SystemInfo ReadSystem() {
    SystemInfo out;
    MEMORYSTATUSEX memory{sizeof(memory)};
    if (GlobalMemoryStatusEx(&memory))
        out.ram = memory.ullTotalPhys;
    Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) {
        Microsoft::WRL::ComPtr<IDXGIAdapter1> adapter;
        for (UINT i = 0; factory->EnumAdapters1(i, &adapter) != DXGI_ERROR_NOT_FOUND; ++i) {
            DXGI_ADAPTER_DESC1 d{};
            adapter->GetDesc1(&d);
            if (d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE || d.DedicatedVideoMemory <= out.vram)
                continue;
            out.gpu = Utf8(d.Description);
            out.nvidia = d.VendorId == 0x10de;
            out.vram = d.DedicatedVideoMemory;
            LARGE_INTEGER umd{};
            if (SUCCEEDED(adapter->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd))) {
                const auto v = uint64_t(umd.QuadPart);
                out.driver = d.VendorId == 0x10de
                                 ? NvidiaDriver(v)
                                 : std::to_string(v >> 48) + "." + std::to_string(v >> 32 & 0xffff) +
                                       "." + std::to_string(v >> 16 & 0xffff) + "." +
                                       std::to_string(v & 0xffff);
            }
        }
    }
    constexpr uint64_t GiB = 1ull << 30;
    // Integrated graphics report a small carve-out (typically 128-512 MB) as dedicated memory.
    out.integrated = out.vram < GiB;
    if (out.gpu.empty())
        out.notes.push_back("No graphics card was found.");
    else if (out.integrated)
        out.notes.push_back("This looks like integrated graphics. BB-VR needs a dedicated graphics "
                            "card.");
    else if (out.vram < 8 * GiB - GiB / 2)
        out.notes.push_back("Less than 8 GB of video memory has not been tested.");
    if (out.ram && out.ram < 16 * GiB - GiB)
        out.notes.push_back("Less than 16 GB of system memory has not been tested.");
    return out;
}
fs::path CreatePlayShortcut() {
    PWSTR desktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
        throw std::runtime_error("Cannot locate your desktop.");
    const auto file = fs::path(desktop) / L"Play BB-VR.lnk";
    CoTaskMemFree(desktop);
    std::wstring exe(32768, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size())));
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> persist;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))) ||
        FAILED(link->SetPath(exe.c_str())) || FAILED(link->SetArguments(L"--play")) ||
        FAILED(link->SetWorkingDirectory(fs::path(exe).parent_path().c_str())) ||
        FAILED(link->SetIconLocation(exe.c_str(), 0)) ||
        FAILED(link->SetDescription(L"Start Bloodborne VR with your launcher settings")) ||
        FAILED(link.As(&persist)) || FAILED(persist->Save(file.c_str(), TRUE)))
        throw std::runtime_error("Could not create the desktop shortcut.");
    return file;
}
LaunchPlan Prepare(const Profile &profile, const Paths &paths) {
    ValidateSetup(profile);
    const auto game = GameExecutable(Path(profile.game));
    const auto runtime = Path(profile.runtime);
    const auto home = NormalizeSaveHome(Path(profile.saves));
    const auto patch = PatchFile(runtime);
    CheckGameBuild(game);
    const auto runtime_argument = ArgumentPath(runtime);
    const auto game_argument = ArgumentPath(game);
    BackupSaves(paths, home);
    fs::create_directories(paths.User());
    const auto before = Read(paths.Config());
    Write(paths.Config(), LaunchConfig(before, home), before);
    const auto ini_before = Read(paths.Ini());
    const auto ini = LaunchIni(ini_before);
    Write(paths.Ini(), ini, ini_before);
    // The patch goes by bare name: the game finds it in user/patches through its own (wide)
    // user path, whatever characters the installation's path has.
    const auto patch_copy = paths.User() / "patches" / "bbvr_patch.xml";
    Write(patch_copy, Read(patch), Read(patch_copy));
    LaunchPlan plan{runtime, paths.root};
    // argv[0] too: a graphics restart relaunches the game from it.
    plan.command = Quote(runtime_argument) + L" --openxr --fullscreen " +
                   (profile.fullscreen ? L"true" : L"false") + L" -p bbvr_patch.xml --game " +
                   Quote(game_argument);
    wchar_t *environment = GetEnvironmentStringsW();
    if (!environment)
        WinError("Cannot read process environment");
    std::vector<std::wstring> entries;
    // Inherited renderer switches (a developer shell's) never reach the game: it runs the preset.
    for (const wchar_t *at = environment; *at; at += wcslen(at) + 1) {
        if (_wcsnicmp(at, L"VR_", 3) != 0 && _wcsnicmp(at, L"FAST_", 5) != 0 &&
            _wcsnicmp(at, L"MOCKXR_", 7) != 0)
            entries.emplace_back(at);
    }
    FreeEnvironmentStringsW(environment);
    entries.push_back(L"VR_INI=" + ArgumentPath(paths.Ini()));
    entries.insert(entries.end(), std::begin(RendererEnvironment), std::end(RendererEnvironment));
    entries.push_back(L"VR_NATIVE_IPD=" + NativeIpd(ini));
    std::sort(entries.begin(), entries.end(),
              [](const auto &a, const auto &b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    for (const auto &entry : entries) {
        plan.environment.insert(plan.environment.end(), entry.begin(), entry.end());
        plan.environment.push_back(0);
    }
    plan.environment.push_back(0);
    return plan;
}
HANDLE Launch(const LaunchPlan &plan) {
    STARTUPINFOW startup{sizeof(startup)};
    PROCESS_INFORMATION process{};
    auto command = plan.command;
    if (!CreateProcessW(plan.executable.c_str(), command.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                        (void *)plan.environment.data(), plan.directory.c_str(), &startup,
                        &process))
        WinError("Cannot start the VR runtime");
    CloseHandle(process.hThread);
    return process.hProcess;
}
bool RuntimeRunning() {
    const HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        WinError("Cannot check running games");
    PROCESSENTRY32W entry{sizeof(entry)};
    bool found = false;
    for (BOOL next = Process32FirstW(snapshot, &entry); next;
         next = Process32NextW(snapshot, &entry)) {
        if (_wcsicmp(entry.szExeFile, L"shadps4.exe") == 0) {
            found = true;
            break;
        }
    }
    CloseHandle(snapshot);
    return found;
}
std::optional<fs::path> PickPath(HWND owner, bool folder, const wchar_t *title,
                                 const wchar_t *pattern) {
    Microsoft::WRL::ComPtr<IFileOpenDialog> dialog;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&dialog))))
        throw std::runtime_error("Cannot open the Windows file picker.");
    DWORD flags{};
    dialog->GetOptions(&flags);
    dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST |
                       (folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST));
    dialog->SetTitle(title);
    COMDLG_FILTERSPEC filter{L"Supported files", pattern};
    if (!folder)
        dialog->SetFileTypes(1, &filter);
    const auto result = dialog->Show(owner);
    if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED))
        return {};
    if (FAILED(result))
        throw std::runtime_error("The file picker could not complete.");
    Microsoft::WRL::ComPtr<IShellItem> item;
    dialog->GetResult(&item);
    PWSTR selected = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &selected)))
        throw std::runtime_error("Cannot read the selected path.");
    fs::path path(selected);
    CoTaskMemFree(selected);
    return path;
}
void OpenPath(const fs::path &path) {
    if (reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)) <= 32)
        throw std::runtime_error("Cannot open " + PathText(path));
}
// The in-game VR page's tabs, in its order (vr_page.h Tabs).
const std::vector<const char *> &Categories() {
    static const std::vector<const char *> all{
        "General", "Controls",  "Comfort", "HUD",     "Assists",      "Display", "Graphics",
        "DLSS 5",  "Body",      "Inventory", "Feedback", "Weapons", "Interactions", "Color"};
    return all;
}
static Setting Choice(const char *category, const char *label, const char *help,
                      std::vector<std::string> keys, std::vector<Option> options,
                      const char *details) {
    return {category, label, help, std::move(keys), std::move(options), details};
}
static Setting Number(const char *category, const char *label, const char *help,
                      const char *key, int min, int max, int step, double scale, int fallback,
                      const char *unit, bool sign, const char *details,
                      const char *zero = nullptr) {
    Setting row{category, label, help, {key}, {}, details};
    row.min = min;
    row.max = max;
    row.step = step;
    row.scale = scale;
    row.fallback = fallback;
    row.unit = unit;
    row.sign = sign;
    row.zero = zero;
    return row;
}
// vr_page.h SameValue: `a` the file's value, `b` an option's.
bool SameValue(const std::string &a, const std::string &b) {
    auto number = [](const std::string &v, double &out) {
        return !v.empty() && std::from_chars(v.data(), v.data() + v.size(), out).ptr ==
                                 v.data() + v.size();
    };
    double x{}, y{};
    if (number(a, x) && number(b, y))
        return std::abs(x - y) < 1e-4 * std::max(1.0, std::abs(y));
    if ((a == "true" && b == "1") || (a == "false" && b == "0"))
        return true;
    return a == b;
}
std::string NumberValue(const Setting &row, int shown) {
    const double value = shown * row.scale;
    if (value == std::floor(value))
        return std::to_string(static_cast<long long>(value));
    char text[32];
    snprintf(text, sizeof(text), "%.4g", value);
    return text;
}
std::optional<int> NumberShown(const Setting &row, const std::string &value) {
    double parsed{};
    const auto *first = value.data(), *last = value.data() + value.size();
    if (value.empty() || std::from_chars(first, last, parsed).ptr != last)
        return std::nullopt;
    const auto shown = static_cast<int>(std::lround(parsed / row.scale));
    if (row.zero && shown == 0)
        return 0;
    // Outside the slider (set by hand, or in game beyond the page's range): shown as Custom.
    if (shown < row.min || shown > row.max)
        return std::nullopt;
    return shown;
}
// Labels, help and values follow the in-game VR page (vr_page.h Rows); the details come from the
// game code. Not offered: the page's read-only headset resolution, and Smooth motion
// (late_warp_motion), which needs late_warp=1 while the headset preset runs late_warp=0.
// Scenes on a screen and Camera are launcher-only: the page has no row for them.
const std::vector<Setting> &Settings() {
    static const std::vector<Setting> all{
        // General
        Choice("General", "Height",
               "As the hunter: his eye height. As the player: your real height (from the "
               "headset's floor).",
               {"height_base"}, {{"As the hunter", {"hunter"}}, {"As the player", {"player"}}},
               "As the hunter: standing straight, your eyes are at the hunter's eye height. As "
               "the player: your real height, measured from the headset's floor. Seated play "
               "always uses the hunter's height."),
        Number("General", "Adjust height",
               "Taller or shorter, from the choice above. Also corrects a floor that is a few cm "
               "off.",
               "height_adjust_cm", -10, 10, 1, 1, 0, " cm", true,
               "Moves your eyes up or down from the height choice, in centimetres."),
        Choice("General", "Move with", "Which thumb walks. Your right hand still holds the blade.",
               {"swap_sticks"}, {{"Left stick", {"0"}}, {"Right stick", {"1"}}},
               "Right stick: the right stick walks and the left stick turns. The stick clicks "
               "and face buttons change hands too."),
        Choice("General", "Turning", "How the turning stick turns you.",
               {"snap_turn_deg", "smooth_turn_deg_per_s"},
               {{"Snap 15\xC2\xB0", {"15", Keep}},
                {"Snap 30\xC2\xB0", {"30", Keep}},
                {"Snap 45\xC2\xB0", {"45", Keep}},
                {"Snap 90\xC2\xB0", {"90", Keep}},
                {"Smooth, slow", {"0", "90"}},
                {"Smooth", {"0", "120"}},
                {"Smooth, fast", {"0", "180"}}},
               "Snap turns jump by the chosen angle. Smooth turning rotates at 90 (slow), 120 or "
               "180 (fast) degrees per second."),
        Number("General", "Vibration", "Strength of every vibration. 0 turns it off.",
               "haptic_gain", 0, 10, 1, 1.0 / 7, 7, "", false,
               "Scales every controller vibration. 7 is the game's normal strength."),
        // Controls
        Choice("Controls", "Control style",
               "Motion: swing to attack. Buttons: the classic layout.", {"controls"},
               {{"Motion", {"motion"}}, {"Buttons", {"classic"}}},
               "Motion: swing the controller to attack. Buttons: attack with buttons only."),
        Choice("Controls", "Blood Vial", "Stab the vial into your wrist, or use a button.",
               {"inject"}, {{"Stab to inject", {"1"}}, {"Button", {"0"}}},
               "Stab: X readies a vial, then touch it to your wrist or body to inject it. "
               "Button: X uses a vial, as in the original game."),
        Choice("Controls", "Gun laser", "A thin line from the gun's barrel.", {"laser"},
               {{"Off", {"0"}}, {"On", {"1"}}},
               "A thin line along the gun barrel while you raise your gun hand to aim."),
        Choice("Controls", "Grip adjust",
               "Press the grip-adjust button twice and hold to set how the weapon sits in your "
               "hand.",
               {"grip_adjust"}, {{"On", {"1"}}, {"Off", {"0"}}},
               "On: double-press and hold the grip-adjust button to change how the weapon sits "
               "in your hand. The in-game VR page names the button for your controllers."),
        Choice("Controls", "Stance", "Seated: smaller swings and throws, the lap counts for the vial.",
               {"seated"}, {{"Standing", {"0"}}, {"Seated", {"1"}}},
               "Seated: smaller strong swings and throws, your lap counts as a place for Blood "
               "Vials, and walking around your room does not move the hunter."),
        // Comfort
        Choice("Comfort", "Body near your eyes",
               "Full body only: fade his coat where it comes close to your eyes.",
               {"body_cutaway"}, {{"Fade", {"1"}}, {"Show", {"0"}}},
               "Full body only: parts of the body closer than about 20 cm to your eyes fade out, "
               "so you can look down past the collar and chest."),
        Choice("Comfort", "Hunter's body",
               "See the hunter's whole body, only his hands and forearms, or only his hands.",
               {"body_mode"},
               {{"Full", {"no_head"}}, {"Arms only", {"arms_only"}}, {"Hands only", {"hands_only"}}},
               "Full: everything but the head. The hunter's shadow always shows the whole body."),
        Number("Comfort", "World size (eye distance)",
               "A smaller eye distance makes the world look larger.", "ipd_mm", 40, 80, 1, 1, 0,
               " mm", false,
               "The fast renderer uses a fixed eye distance chosen at launch: Standard is 64 mm, "
               "not your headset's own setting. Smaller values make the world look larger.",
               "Standard (64 mm)"),
        Choice("Comfort", "Scenes on a screen",
               "Show grabs, viscerals, doors and ladders on a virtual screen.", {"cinematic"},
               {{"On", {"1"}}, {"Off", {"0"}}},
               "On: viscerals, grabs, doors, levers, lamps and ladders play from the game's own "
               "camera on a virtual screen. Off: they stay in first person. Launcher only: the "
               "in-game VR page has no row for it."),
        // HUD
        Choice("HUD", "Health & stamina", "On your wrist, or in the corner like the game.",
               {"hud_vitals"}, {{"Wrist gauge", {"wrist"}}, {"Classic", {"classic"}}},
               "Wrist gauge: a Blood Gauge on the left wrist shows health, stamina and bullets; "
               "the game's own health display appears when you look down. Classic: the game's "
               "HUD."),
        Choice("HUD", "Low-health warning",
               "A heartbeat in both hands and dark edges when your health is low.",
               {"vitals_heartbeat", "vitals_edge"}, {{"On", {"1", "1"}}, {"Off", {"0", "0"}}},
               "On: a heartbeat in both controllers and darkened edges of your view while your "
               "health is low."),
        Choice("HUD", "Echoes & Insight", "Look down: they show when you look down at your feet.",
               {"ui_counters"},
               {{"Look down", {"look_down"}}, {"Always", {"always"}}, {"Hidden", {"hidden"}}},
               "Look down: the counters fade in when you look well below the horizon. Menus and "
               "dialogs always show them."),
        Choice("HUD", "Hit flash", "The edges of your view flash red when you are hit.",
               {"hit_vignette"}, {{"On", {"1"}}, {"Off", {"0"}}},
               "On: the edges of your view flash red when you take a hit."),
        Choice("HUD", "HUD size", "The size of the health bars, items and prompts.",
               {"ui_hud_scale_deg"},
               {{"Small", {"0.035"}}, {"Medium", {"0.042"}}, {"Large", {"0.05"}}},
               "The size of the health bars, item icons and prompts. Medium is the game "
               "default."),
        // Assists
        Choice("Assists", "Extra health", "More maximum health for the hunter.",
               {"player_hp_bonus_pct"},
               {{"Off", {"0"}}, {"+10 %", {"10"}}, {"+20 %", {"20"}}, {"+30 %", {"30"}},
                {"+50 %", {"50"}}},
               "Raises the hunter's maximum health (+10 % by default). Your save file is not "
               "changed."),
        Choice("Assists", "Longer dodge invincibility", "Extra frames of invincibility in a dodge.",
               {"dodge_iframes_extra"},
               {{"Off", {"0"}}, {"+1 frame", {"1"}}, {"+2 frames", {"2"}}, {"+3 frames", {"3"}},
                {"+4 frames", {"4"}}},
               "Extra invincibility after a quickstep or roll, in 1/30 s frames (+1 by default). "
               "Backsteps get none."),
        Choice("Assists", "Instant ragdoll",
               "An enemy your blade kills goes limp at once. Off: its death animation.",
               {"melee_kill_ragdoll"}, {{"On", {"1"}}, {"Off", {"0"}}},
               "On: an enemy killed by your blade goes limp straight away. Off: it plays its "
               "death animation."),
        Choice("Assists", "Ragdoll push", "How hard your swing throws the body. None: it just falls.",
               {"melee_ragdoll_impulse"},
               {{"None", {"0"}}, {"Light", {"0.5"}}, {"Normal", {"1"}}, {"Strong", {"2"}},
                {"Strongest", {"3"}}},
               "How far your swing throws a body. Normal is the game default."),
        Choice("Assists", "Light attack stamina",
               "Light swing cost. Strong swings cost 100%; low stamina makes them light.",
               {"melee_stamina_pct"},
               {{"Original", {"100"}}, {"80%", {"80"}}, {"50%", {"50"}}, {"10%", {"10"}}},
               "Light swings cost this share of the game's stamina (80% by default). Strong "
               "swings always cost the full amount; with too little stamina a strong swing "
               "becomes a light one. Dodges, sprinting and guns are unchanged."),
        // Display
        Choice("Display", "Anti-aliasing",
               "Game FXAA, dual-eye SMAA, or None. Bypassed while DLSS or FSR 4 is on.",
               {"aa_mode"}, {{"Game FXAA", {"fxaa"}}, {"SMAA", {"smaa"}}, {"None", {"off"}}},
               "SMAA: dual-eye SMAA replaces the game's FXAA (the default). Game FXAA: the "
               "game's own. None: no extra edge smoothing."),
        Choice("Display", "Show enemy health bars",
               "The bars over enemies you hit or lock on to. Boss bars always show.",
               {"ui_enemy_hp_bars"}, {{"On", {"1"}}, {"Off", {"0"}}},
               "On: bars and damage numbers over enemies you hit or lock on to. Boss bars, the "
               "lock-on dot and your own HUD always show."),
        Choice("Display", "Camera", "Choose the initial perspective.", {"first_person"},
               {{"First person", {"1"}}, {"Third person", {"0"}}},
               "Third person turns the VR body off. Launcher only: the in-game VR page has no "
               "row for it."),
        // Body
        Choice("Body", "Tracked posture", "Lean keeps the spine's length as you move your head.",
               {"body_align", "body_posture"},
               {{"Lean", {"1", "lean"}}, {"Legacy shift", {"1", "shift"}}, {"Off", {"0", "lean"}}},
               "How the hunter's body follows your head. Lean keeps the spine's length; Legacy "
               "shift is the older behaviour."),
        Number("Body", "Side lean limit",
               "How far the hunter leans sideways. More lean lets you move farther before he "
               "steps.",
               "body_lean_side_deg", 0, 35, 1, 1, 20, "\xC2\xB0", false,
               "The furthest the hunter leans sideways before he takes a step."),
        Choice("Body", "Extra clipping protection",
               "Protect sideways leans and restore the body in stages after a roll or hit.",
               {"body_comfort"}, {{"Off", {"0"}}, {"On", {"1"}}},
               "On: extra protection against the body clipping into your view on sideways leans, "
               "and a staged return of the body after a roll or hit."),
        Number("Body", "Posture smoothing",
               "Less smoothing follows faster; more softens small movements.", "body_align_ms", 0,
               500, 25, 1, 50, " ms", false,
               "How long the body takes to follow your posture, in milliseconds."),
        // Inventory
        Choice("Inventory", "Item access",
               "Left grip takes and holds items. Release returns an unused vial, or throws a "
               "throwable. Vials stay right; pocket/chest follow equipped quick slots 1/2.",
               {"holster_mode"}, {{"Menu", {"0"}}, {"Body slots", {"1"}}, {"Both", {"2"}}},
               "Menu: the quick-item menu. Body slots: pouches on the hunter's body (Y still "
               "opens the menu). Both: either."),
        Choice("Inventory", "Pouch touch feedback", "A small vibration when your hand finds a pouch.",
               {"holster_haptics"}, {{"On", {"1"}}, {"Off", {"0"}}},
               "On: a small vibration when your hand reaches a pouch."),
        // Feedback
        Choice("Feedback", "Low-health red edges",
               "A restrained red edge below 30% health. Takes priority over stamina color "
               "feedback.",
               {"health_color"}, {{"Off", {"0"}}, {"On", {"1"}}},
               "On: a red edge around your view below 30% health."),
        Number("Feedback", "Health intensity", "Strength of the low-health red edge.",
               "health_color_strength", 0, 100, 10, 0.01, 50, " %", false,
               "How strong the low-health red edge is."),
        Choice("Feedback", "Exhaustion desaturation",
               "Briefly soften colors at exhausted stamina, then fade back. Never full grayscale.",
               {"stamina_color"}, {{"Off", {"0"}}, {"On", {"1"}}},
               "On: colours soften briefly when your stamina runs out, then return."),
        Number("Feedback", "Stamina intensity", "Strength of the brief exhaustion effect.",
               "stamina_color_strength", 0, 100, 10, 0.01, 50, " %", false,
               "How strong the exhaustion effect is."),
        Choice("Feedback", "Shoulder weapon switch",
               "B at right shoulder / Y at left: tap to switch, hold 1.6 seconds for unarmed. Tap "
               "unarmed to draw slot 1.",
               {"shoulder_switch"}, {{"Off", {"0"}}, {"On", {"1"}}},
               "On: reach over a shoulder and press B (right) or Y (left) to switch that hand's "
               "weapon; hold for 1.6 seconds to go unarmed."),
        // Weapons
        Choice("Weapons", "Solid weapons", "Stop at walls and each other. Weapons only: just each other.",
               {"weapon_wall"}, {{"Off", {"0"}}, {"On", {"1"}}, {"Weapons only", {"2"}}},
               "On: weapons stop at walls and at each other. Weapons only (the default): they "
               "only stop at each other."),
        Choice("Weapons", "Grip guide", "Hold the right grip to show where your left hand can grab.",
               {"two_hand_guide"}, {{"On", {"1"}}, {"Off", {"0"}}},
               "On: holding the right grip shows where your left hand can take the weapon with "
               "both hands."),
        Number("Weapons", "Swing feel", "Faint feedback during fast swings with both hands on one weapon.",
               "two_hand_swing_hum", 0, 100, 10, 0.01, 100, " %", false,
               "Vibration strength during fast two-handed swings."),
        Choice("Weapons", "Wrist-flick reload",
               "Release the trigger, steady your hand, then flick your left wrist in any direction "
               "to reload.",
               {"gun_wrist_reload"}, {{"Off", {"0"}}, {"On", {"1"}}},
               "On: flick your left wrist in any direction to reload the gun."),
        Choice("Weapons", "Blade sounds",
               "Your weapons ring when they meet; draw one along the other to sharpen it.",
               {"blade_sounds"}, {{"On", {"1"}}, {"Off", {"0"}}},
               "On: weapons ring when they touch, and drawing one along the other makes a "
               "sharpening sound."),
        // Interactions
        Choice("Interactions", "Physical levers",
               "Grip a lever with either hand and pull. Off: original interaction.",
               {"physical_levers"}, {{"On", {"1"}}, {"Off", {"0"}}},
               "On: grip a lever with either hand and pull it. Off: the game's original "
               "interaction."),
        Choice("Interactions", "Lamp rest & travel",
               "Rest, refill and travel between lit lamps. Offline only. Experimental.",
               {"lamp_travel"}, {{"Off", {"0"}}, {"On", {"1"}}},
               "On: rest, refill and travel between lit lamps from the lamp itself. Offline "
               "only, and experimental."),
        Choice("Interactions", "VR Grab attacks",
               "Stay in VR during enemy grabs. The view follows the hunter. Off: existing "
               "behavior.",
               {"vr_grab_attacks"}, {{"Off", {"0"}}, {"On", {"1"}}},
               "On: enemy grab attacks stay in VR, with the view following the hunter. Off (the "
               "default): the existing behaviour."),
        // Color
        Number("Color", "Contrast", "Light and dark separation. 0 is unchanged.", "color_contrast",
               -50, 50, 5, 1, 0, " %", true, "Separation between light and dark areas."),
        Number("Color", "Saturation", "Color intensity. -100 is monochrome; 0 is unchanged.",
               "color_saturation", -100, 100, 5, 1, 0, " %", true,
               "Colour intensity. -100 % is black and white."),
        Number("Color", "Gamma", "Midtone brightness. Higher is brighter; 0 is unchanged.",
               "color_gamma", -50, 50, 5, 1, 0, " %", true,
               "Brightness of the midtones; higher is brighter.")};
    return all;
}
} // namespace Launcher
