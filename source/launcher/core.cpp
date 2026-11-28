// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "core.h"
#include "mods.h"
#include "core/vr/game_reflections.h"
#include "common/addon_content.h"
#include "core/vr/aa_mode.h"
#include "core/vr/blade_sounds.h"
#include "core/vr/body_settings.h"
#include "core/vr/gun_settings.h"
#include "core/vr/melee_settings.h"
#include "core/vr/mirror_world.h"
#include "core/vr/native_views.h"
#include "core/vr/quick_settings.h"
#include "core/vr/trick_weapons.h"
#include "core/vr/two_hand_grip.h"
#include "core/vr/vr_page.h"
#include "core/vr/vr_session.h"
#include <algorithm>
#include <array>
#include <bcrypt.h>
#include <cctype>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <dxgi.h>
#include <fstream>
#include <iterator>
#include <mutex>
#include <sstream>
#include <pugixml.hpp>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <stdexcept>
#include <system_error>
#include <tlhelp32.h>
#include <wrl/client.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

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
// A PKG chosen as the game, or the folder it sits in (its own entries only, at most 64, the
// answer kept for that path: this runs every frame), gets told what the launcher needs instead
// (2026-10-26, the user). The launcher never unpacks a PKG: that needs decryption keys.
static bool PkgInstead(const fs::path &path) {
    const auto is_pkg = [](const fs::path &p) {
        return _wcsicmp(p.extension().c_str(), L".pkg") == 0;
    };
    if (is_pkg(path))
        return true;
    thread_local fs::path checked;
    thread_local bool found = false;
    if (path != checked) {
        checked = path;
        found = false;
        std::error_code ec;
        int seen = 0;
        for (fs::directory_iterator it(path, ec), end; !ec && it != end && seen < 64;
             it.increment(ec), ++seen)
            if (is_pkg(it->path()) && it->is_regular_file(ec)) {
                found = true;
                break;
            }
    }
    return found;
}
fs::path GameExecutable(const fs::path &path) {
    const auto exe = fs::is_directory(path) ? path / "eboot.bin" : path;
    if (!fs::is_regular_file(exe) || _wcsicmp(exe.filename().c_str(), L"eboot.bin") != 0) {
        if (!path.empty() && PkgInstead(path))
            throw std::runtime_error(
                "That is a PKG file (or the folder it is in), not the game itself. Choose the "
                "game's extracted folder: the one with eboot.bin and sce_sys / param.sfo. "
                "Extract the PKG with a separate tool first, then choose that folder.");
        throw std::runtime_error("Choose the game folder containing eboot.bin.");
    }
    if (!fs::is_regular_file(exe.parent_path() / "sce_sys" / "param.sfo"))
        throw std::runtime_error("The game folder is missing sce_sys / param.sfo.");
    return exe;
}
// The measured 90 Hz native headset preset (the private test harness's try_native.py
// --native-asymmetric --rate 90), without its diagnostics. FAST_TOGGLES is left to the build's
// own default mask, which is the preset's.
static constexpr const wchar_t *RendererEnvironment[]{
    L"VR_NATIVE_STEREO=1",
    // Each eye renders exactly its lens frustum (and the headset-size resolution template).
    L"VR_NATIVE_ASYMMETRIC=1",
    // The stream buffer's shadow in VR, cached memory in a flat run (buffer.cpp).
    L"VR_STREAM_MEM=auto",
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
        {"fov_margin_deg", "6"},
        {"frame_cap", "auto"},        {"ui_layer", "1"},
        {"menu_present", "layer"},    {"vr_menu", "1"},
        {"late_warp", "0"},           {"eye_size", "auto"},
        {"eye_sharpen", "0"},
        // The DLSS model follows the preset (K, or M for Ultra Quality and Balanced Plus). A scan
        // applied by 0.5.0 or 0.5.1 pinned K here, and nothing in the menus can change it back.
        {"dlss_model", "builtin"}};
    return edits;
}
std::string PatchFrameRate(const std::string &xml, int hz) {
    if(hz<30 || hz>360)throw std::runtime_error("Invalid frame-rate target.");
    pugi::xml_document doc;
    if (!doc.load_string(xml.c_str()))
        throw std::runtime_error("Invalid game patch.");
    const std::array<unsigned, 4> sites{0x02434840, 0x0243485A, 0x0243487E, 0x024348B3};
    std::array<int, 4> seen{};
    const float deadline = 1.f / float(hz);
    const auto bytes = reinterpret_cast<const unsigned char *>(&deadline);
    const char *hex = "0123456789ABCDEF";
    std::string value = "41C7442418";
    for (unsigned i = 0; i < 4; ++i) {
        value += hex[bytes[i] >> 4];
        value += hex[bytes[i] & 15];
    }
    for (auto meta : doc.child("Patch").children("Metadata"))
        if (std::string(meta.attribute("isEnabled").value()) == "true")
            for (auto line : meta.child("PatchList").children("Line")) {
                unsigned addr = std::stoul(line.attribute("Address").value(), nullptr, 16);
                for (size_t i = 0; i < sites.size(); ++i)
                    if (addr == sites[i]) {
                        if (++seen[i] != 1 ||
                            (std::string(line.attribute("Value").value()) != "41C74424188988883C" &&
                             std::string(line.attribute("Value").value()) != "41C7442418610B363C"))
                            throw std::runtime_error("Frame-deadline patch no "
                                                     "longer matches the audited template.");
                        line.attribute("Value").set_value(value.c_str());
                    }
            }
    if (std::any_of(seen.begin(), seen.end(), [](int x) { return x != 1; }))
        throw std::runtime_error("Missing frame-deadline patches.");
    std::ostringstream out;
    doc.save(out);
    return out.str();
}
std::string LaunchConfig(const std::string &text, const fs::path &home, int refresh_hz,
                         const std::optional<fs::path>& addons) {
    auto json = text.empty() ? Json::object() : Json::parse(text);
    if (!json.is_object())
        throw std::runtime_error("config.json must contain an object.");
    for (const char *section : {"General", "GPU", "Vulkan"})
        if (json.contains(section) && !json[section].is_object())
            throw std::runtime_error(std::string("config.json has an invalid ") + section +
                                     " section.");
    auto &general = json["General"];
    general["home_dir"] = PathText(home);
    if (addons) general["addon_install_dir"] = PathText(*addons);
    // The VR resolution template allocates its eye targets from this. Its graphics heaps
    // (4080 + 2304 MiB) and the game's other startup allocations (1416 MiB) take 7800 of the
    // 8620 MiB this gives; 512 MiB more are the render resource cache's
    // (core/vr/bloodborne_render_jobs.cpp: with less free it keeps the game's 160 MiB, which
    // Laurence's opening cinematic overflows).
    general["extra_dmem_in_mbytes"] = 4012;
    general["show_splash"] = false;
    general["discord_rpc_enabled"] = false;
    auto &gpu = json["GPU"];
    if(!ValidGameRefreshHz(refresh_hz))throw std::runtime_error("Invalid launch refresh rate.");
    gpu["vblank_frequency"] = refresh_hz;
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
// Video memory no longer picks DIBR (2026-10-22): every card starts in native stereo, and DIBR is
// the player's choice on the Play page. Only a driver without native stereo's multiview and the
// AMD RX 400, RX 500 and RX 5000 cards (2026-10-26) start in DIBR (StartupStereo(const
// SystemInfo&)).
StereoPreset StartupStereo(uint64_t) {
    return StereoPreset::Native;
}
StereoPreset StartupStereo(const SystemInfo &system) {
    return system.multiview_limited || system.auto_dibr ? StereoPreset::Dibr
                                                        : StartupStereo(system.vram);
}
std::wstring NativeViewsValue(NativeViews choice) {
    switch (choice) {
    case NativeViews::Multiview:
        return L"multiview";
    case NativeViews::Layers:
        return L"layers";
    case NativeViews::Auto:
        break;
    }
    return {};
}
int GameRefreshRate(bool follow_headset, int manual_hz, float headset_hz) {
    if (!ValidGameRefreshHz(manual_hz)) throw std::runtime_error("Invalid game frame-rate cap.");
    if (!follow_headset) return manual_hz;
    return std::isfinite(headset_hz) && headset_hz >= MinGameRefreshHz && headset_hz <= MaxGameRefreshHz
        ? int(std::lround(headset_hz)) : 90;
}
std::string LaunchIni(const std::string &text, StereoPreset preset, int refresh_hz, bool follow_headset) {
    if(!ValidGameRefreshHz(refresh_hz))throw std::runtime_error("Invalid launch refresh rate.");
    auto edits = RendererIni();
    // Zero means keep the runtime's display rate. Gameplay's cap is still set in
    // config.json and the private game patch using the resolved numeric rate.
    edits.push_back({"refresh_hz",follow_headset ? "0" : std::to_string(refresh_hz)});
    // The frame-rate hooks' cap: following the headset, game_fps=0 holds the game at the
    // headset's own refresh and follows a change made mid-game (the rate the session reports,
    // else the shortest frame period seen: never a runtime's throttled period, 60d6d78f1);
    // otherwise the chosen rate.
    edits.push_back({"game_fps", follow_headset ? "0" : std::to_string(refresh_hz)});
    edits.push_back({"stereo", "1"});
    if (preset == StereoPreset::Dibr)
        edits.push_back({"late_warp", "1"});
    return VR::Ini::ApplyEdits(text, edits);
}
std::string MigrateIni(const std::string &text, const std::vector<VR::Ini::Edit> &choices) {
    // Merely opening the launcher on a fresh install must leave the file for the game to create.
    if (choices.empty() && VR::Ini::Trim(text).empty())
        return text;
    std::vector<VR::Ini::Edit> edits;
    if (VR::Ini::ValueOf(text, "quick_fan_seen") != "1") {
        const auto pick = VR::Ini::ValueOf(text, "quick_arc_pick").value_or("");
        const auto style = VR::Ini::ValueOf(text, "quick_fan_style").value_or("");
        if (pick != "fan" && pick != "2" && pick != "sector" && pick != "1")
            edits.push_back({"quick_arc_pick", "fan"});
        edits.push_back({"quick_fan_style", style == "brass" || style == "1" ? "brass" : "lancet"});
        edits.push_back({"quick_fan_seen", "1"});
    }
    const auto graphics_seen = VR::Ini::ValueOf(text, "newgfx_096_seen");
    if (graphics_seen != "1" && graphics_seen != "true") {
        // The runtime retires the old water override once. A Wetness pick sets both keys,
        // but the launcher only queues values that differ from disk. Keep the water half
        // of an explicit pick even if only its wet-streets half needed writing.
        const bool picked_wetness = std::any_of(choices.begin(), choices.end(), [](const auto &edit) {
            return (edit.key == "water_reflections" || edit.key == "wet_streets") && edit.value;
        });
        if (!picked_wetness)
            edits.push_back({"water_reflections", std::nullopt});
        edits.push_back({"newgfx_096_seen", "1"});
    }
    if (edits.empty() && choices.empty())
        return text;
    edits.insert(edits.end(), choices.begin(), choices.end());
    return VR::Ini::ApplyEdits(text, edits);
}
std::wstring NativeIpd(const std::string &ini, bool bundled_runtime) {
    // The included runtime (0.3.1+) takes the eye distance per frame from vr.ini ipd_mm (live, the
    // in-game Menu + stick too) or the headset: "auto". A custom runtime may be older, and an older
    // runtime reads VR_NATIVE_IPD as a number only ("auto" would be 0: both eyes at one point, the
    // world huge): it gets the number, as before 0.3.1 (2026-10-18).
    if (bundled_runtime)
        return L"auto";
    float mm = 0;
    if (const auto value = VR::Ini::ValueOf(ini, "ipd_mm")) {
        try {
            mm = std::stof(*value);
        } catch (const std::exception &) {
        }
    }
    // 0 (or anything the game would not accept) is Standard: 64 mm for such a runtime. The page
    // allows 40..80, and Menu + stick in game up to 120.
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
    throw std::runtime_error("bbvr_patch.xml is missing. Use a complete package.");
}
using Common::Addons::SfoText;
std::optional<std::string> AppVersion(const fs::path &param_sfo) {
    return SfoText(param_sfo, "APP_VER");
}
std::string SaveDataFolder(const fs::path &param_sfo) {
    auto id = SfoText(param_sfo, "INSTALL_DIR_SAVEDATA");
    if (!id)
        id = SfoText(param_sfo, "TITLE_ID");
    const auto plain = [](const std::string &s) {
        return s.size() == 9 &&
               std::all_of(s.begin(), s.begin() + 4, [](char c) { return c >= 'A' && c <= 'Z'; }) &&
               std::all_of(s.begin() + 4, s.end(), [](char c) { return c >= '0' && c <= '9'; });
    };
    if (!id || !plain(*id))
        throw std::runtime_error("The game's save folder (param.sfo INSTALL_DIR_SAVEDATA or "
                                 "TITLE_ID) cannot be read. Select the complete game folder.");
    return *id;
}
std::string FileSha256(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    if (!file)
        throw std::runtime_error("Cannot read executable");
    struct Hash {
        BCRYPT_ALG_HANDLE algorithm{};
        BCRYPT_HASH_HANDLE handle{};
        ~Hash() {
            if (handle)
                BCryptDestroyHash(handle);
            if (algorithm)
                BCryptCloseAlgorithmProvider(algorithm, 0);
        }
    } hash;
    if (BCryptOpenAlgorithmProvider(&hash.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
        BCryptCreateHash(hash.algorithm, &hash.handle, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("Cannot create SHA-256 hash");
    std::array<unsigned char, 64 * 1024> buffer;
    while (file) {
        file.read(reinterpret_cast<char *>(buffer.data()), std::streamsize(buffer.size()));
        const auto count = file.gcount();
        if (count && BCryptHashData(hash.handle, buffer.data(), ULONG(count), 0) < 0)
            throw std::runtime_error("Cannot hash executable");
    }
    if (file.bad() || !file.eof())
        throw std::runtime_error("Cannot finish reading executable");
    std::array<unsigned char, 32> digest{};
    if (BCryptFinishHash(hash.handle, digest.data(), ULONG(digest.size()), 0) < 0)
        throw std::runtime_error("Cannot finish SHA-256 hash");
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (const auto byte : digest) {
        out += hex[byte >> 4];
        out += hex[byte & 15];
    }
    return out;
}
std::string GameSupportInfo(const fs::path &eboot) {
    std::string out = "Game metadata";
    for (const auto *key : {"TITLE_ID", "APP_VER", "VERSION"}) {
        std::optional<std::string> value;
        try {
            value = SfoText(eboot.parent_path() / "sce_sys" / "param.sfo", key);
        } catch (const std::exception &) {
        }
        out += " " + std::string(key) + " " + value.value_or("unavailable");
    }
    std::string hash = "unavailable";
    try {
        hash = FileSha256(eboot);
    } catch (const std::exception &) {
    }
    return out + "\nGame eboot.bin SHA-256 " + hash + "\n";
}
std::string CheckGameBuild(const fs::path &eboot) {
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
            throw std::runtime_error("This game build is not supported. Bldbrne-VR needs Bloodborne "
                                     "version 1.09 (eboot.bin unmodified).");
    }
    // Updated installations can retain 01.00 metadata or report 01.09. The executable
    // signatures above decide build compatibility; metadata selects the launch-time patches.
    const auto version = AppVersion(eboot.parent_path() / "sce_sys" / "param.sfo");
    if (version != "01.00" && version != "01.09")
        throw std::runtime_error("The executable passed Bldbrne-VR's build checks, but its game metadata "
                                 "(APP_VER " + version.value_or("unknown") +
                                 ") is unsupported. Select the complete Bloodborne 1.09 game folder "
                                 "with readable 01.00 or 01.09 metadata; do not downgrade the game.");
    return *version;
}
std::string PatchForVersion(const std::string &xml, const std::string &version) {
    if (version != "01.00" && version != "01.09")
        throw std::runtime_error("Unsupported game metadata for Bldbrne-VR patches.");
    pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size(), pugi::parse_full) ||
        !document.child("Patch"))
        throw std::runtime_error("bbvr_patch.xml is invalid. Use a complete Bldbrne-VR package.");
    size_t count = 0;
    for (auto metadata : document.child("Patch").children("Metadata")) {
        auto app_version = metadata.attribute("AppVer");
        const std::string_view original = app_version.value();
        if (original != "01.00" && original != "01.09")
            throw std::runtime_error("bbvr_patch.xml contains unsupported version metadata. "
                                     "Use the patch file included with Bldbrne-VR.");
        app_version.set_value(version.c_str());
        ++count;
    }
    if (count == 0)
        throw std::runtime_error("bbvr_patch.xml contains no patches. Use a complete Bldbrne-VR package.");
    std::ostringstream output;
    document.save(output, "    ", pugi::format_default, pugi::encoding_utf8);
    return output.str();
}
std::string PatchIntelSfx(const std::string &xml, const fs::path &eboot, bool enabled) {
    pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size(), pugi::parse_full))
        throw std::runtime_error("Invalid game patch file.");
    auto metadata = document.child("Patch").find_child_by_attribute(
        "Metadata", "Name", "Intel 12th Gen+ SFX workaround");
    if (!metadata) {
        if (enabled)
            throw std::runtime_error("This package does not include the Intel SFX workaround.");
        return xml;
    }
    // The signature below is meaningful only for this exact patch. Reject a damaged
    // or ambiguous package instead of reporting protection that was not applied.
    const auto line = metadata.child("PatchList").child("Line");
    size_t matching_entries = 0;
    for (const auto entry : document.child("Patch").children("Metadata"))
        if (std::string_view(entry.attribute("Name").value()) == "Intel 12th Gen+ SFX workaround")
            ++matching_entries;
    if (matching_entries != 1 || !metadata.attribute("isEnabled") ||
        std::string_view(metadata.attribute("AppElf").value()) != "eboot.bin" ||
        !line || line.next_sibling("Line") ||
        std::string_view(line.attribute("Type").value()) != "bytes" ||
        std::string_view(line.attribute("Address").value()) != "0x02CF83E0" ||
        std::string_view(line.attribute("Value").value()) != "c3")
        throw std::runtime_error("The Intel SFX patch is invalid. Use a complete Bldbrne-VR package.");
    if (enabled) {
        // Bloodborne 1.09: emoose's workaround returns before this function's prologue.
        // Check the exact site in addition to CheckGameBuild's frame-limiter signatures.
        constexpr unsigned char expected[]{0x55, 0x48, 0x89, 0xe5, 0x41, 0x57, 0x41, 0x56,
                                           0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x81, 0xec,
                                           0xc8, 0x00, 0x00, 0x00};
        unsigned char actual[sizeof(expected)]{};
        std::ifstream file(eboot, std::ios::binary);
        file.seekg(std::streamoff(0x28eb0) + 0x02cf83e0 - 0x400000);
        file.read(reinterpret_cast<char *>(actual), sizeof(actual));
        if (!file || !std::equal(std::begin(expected), std::end(expected), actual))
            throw std::runtime_error("The Intel SFX workaround does not match this game executable. "
                                     "Use the unmodified Bloodborne 1.09 executable.");
    }
    metadata.attribute("isEnabled").set_value(enabled ? "true" : "false");
    std::ostringstream output;
    document.save(output, "    ", pugi::format_default, pugi::encoding_utf8);
    return output.str();
}
std::string PatchNonAggro(const std::string &xml, bool enabled) {
    pugi::xml_document document;
    if (!document.load_buffer(xml.data(), xml.size(), pugi::parse_full))
        throw std::runtime_error("Invalid game patch file.");
    auto metadata =
        document.child("Patch").find_child_by_attribute("Metadata", "Name", "Non-aggro mode");
    if (!metadata) {
        if (enabled)
            throw std::runtime_error("This package does not include Non-aggro mode.");
        return xml;
    }
    // Exactly the one-byte flag write: a damaged package must not write anything else.
    const auto line = metadata.child("PatchList").child("Line");
    size_t entries = 0;
    for (const auto entry : document.child("Patch").children("Metadata"))
        if (std::string_view(entry.attribute("Name").value()) == "Non-aggro mode")
            ++entries;
    if (entries != 1 || !metadata.attribute("isEnabled") ||
        std::string_view(metadata.attribute("AppElf").value()) != "eboot.bin" || !line ||
        line.next_sibling("Line") || std::string_view(line.attribute("Type").value()) != "bytes" ||
        std::string_view(line.attribute("Address").value()) != "0x0593E88E" ||
        std::string_view(line.attribute("Value").value()) != "01")
        throw std::runtime_error("The Non-aggro patch is invalid. Use a complete Bldbrne-VR package.");
    metadata.attribute("isEnabled").set_value(enabled ? "true" : "false");
    std::ostringstream output;
    document.save(output, "    ", pugi::format_default, pugi::encoding_utf8);
    return output.str();
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
fs::path LocalAppData() {
    PWSTR value = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &value)))
        throw std::runtime_error("Cannot locate your local application data.");
    fs::path out = value;
    CoTaskMemFree(value);
    return out;
}
Paths Paths::Default() {
    const auto base = LocalAppData();
    const auto now = base / DataFolderName;
    // Not moved (MigrateLegacyData could not): the old folder stays in use, nothing is lost.
    std::error_code error;
    if (!fs::exists(now, error) && fs::exists(base / LegacyDataFolderName, error))
        return {base / LegacyDataFolderName};
    return {now};
}
// Profile paths under the old data folder, rewritten for the new one (case-insensitive prefix).
static std::string Rebase(const std::string &text, const fs::path &from, const fs::path &to) {
    const std::wstring value = Wide(text), prefix = from.native();
    if (value.size() < prefix.size() ||
        _wcsnicmp(value.c_str(), prefix.c_str(), prefix.size()) != 0 ||
        (value.size() > prefix.size() && value[prefix.size()] != L'\\' &&
         value[prefix.size()] != L'/'))
        return text;
    return Utf8(to.native() + value.substr(prefix.size()));
}
bool MigrateLegacyData() {
    return MigrateLegacyData(LocalAppData());
}
bool MigrateLegacyData(const fs::path &base) {
    const auto old = base / LegacyDataFolderName, now = base / DataFolderName;
    std::error_code error;
    if (fs::exists(now, error) || !fs::is_directory(old, error))
        return false;
    // Never while the game runs: its saves, settings and caches are in there.
    if (RuntimeRunning())
        return false;
    // The profile rewritten before anything moves, so a failure leaves everything as it was.
    std::optional<std::string> rewritten;
    if (const auto text = Read(old / "launcher.json"); !text.empty()) {
        try {
            auto json = Json::parse(text);
            for (const char *key : {"game", "saves", "runtime"})
                if (json.contains(key) && json[key].is_string())
                    json[key] = Rebase(json[key].get<std::string>(), old, now);
            rewritten = json.dump(2) + "\n";
        } catch (const std::exception &) {
            // Unreadable profile: moved as it is (Profile::Load also follows a moved save home).
        }
    }
    fs::rename(old, now, error); // same volume: instant; fails while something holds a file
    if (error)
        return false;
    if (rewritten) {
        try {
            Write(now / "launcher.json", *rewritten);
        } catch (const std::exception &) {
            fs::rename(now, old, error); // back as it was; the next start tries again
            return false;
        }
    }
    return true;
}
StereoPreset Profile::SelectedStereo(const SystemInfo &system) const {
    if (stereo == StereoChoice::Auto)
        return StartupStereo(system);
    return SelectedStereo(system.vram);
}
StereoPreset Profile::SelectedStereo(uint64_t dedicated_bytes) const {
    if (stereo == StereoChoice::Dibr)
        return StereoPreset::Dibr;
    if (stereo == StereoChoice::Native)
        return StereoPreset::Native;
    return StartupStereo(dedicated_bytes);
}
Profile Profile::Load(const Paths &paths) {
    Profile out;
    const auto text = Read(paths.Profile());
    if (!text.empty()) {
        const auto json = Json::parse(text);
        const auto stereo = json.value("stereo", Json{});
        out.stereo = stereo == "dibr" ? StereoChoice::Dibr
                     : stereo == "native" ? StereoChoice::Native : StereoChoice::Auto;
        const auto views = json.value("native_views", Json{});
        out.native_views = views == "multiview" ? NativeViews::Multiview
                           : views == "layers"  ? NativeViews::Layers
                                                : NativeViews::Auto;
        out.refresh_hz=json.value("refresh_hz",90);
        out.follow_headset=json.value("follow_headset",false);
        out.intel_sfx_workaround = json.value("intel_sfx_workaround", true);
        out.non_aggro = json.value("non_aggro", false);
        out.free_camera = json.value("free_camera", false);
        out.headset = json.value("headset", "");
        if(!ValidGameRefreshHz(out.refresh_hz))out.refresh_hz=90;
        out.game = json.value("game", "");
        out.dlc = json.value("dlc", "");
        out.saves = json.value("saves", "");
        out.runtime = json.value("runtime", "");
        out.fullscreen_chosen = json.value("fullscreen_chosen", false);
        out.fullscreen = out.fullscreen_chosen && json.value("fullscreen", false);
        out.check_updates_chosen = json.value("check_updates_chosen", false);
        out.check_updates =
            out.check_updates_chosen ? json.value("check_updates", true) : true;
        // Always the included runtime (2026-10-18): players pointed the launcher at their
        // own shadPS4, which lacks the VR renderer, its fixes and patches, and crashed or rendered
        // the world huge. A saved custom choice (use_bundled_runtime false) is ignored.
        out.use_bundled_runtime = true;
    }
    if (out.saves.empty()) {
        out.saves = PathText(paths.User() / "home");
        fs::create_directories(Path(out.saves));
    }
    // A save home still inside the old data folder after the move (an unreadable profile moved as
    // it was): the same place in the new one.
    if (std::error_code error; paths.root.filename() == DataFolderName &&
                               !fs::exists(Path(out.saves), error)) {
        const auto moved =
            Rebase(out.saves, paths.root.parent_path() / LegacyDataFolderName, paths.root);
        if (moved != out.saves && fs::exists(Path(moved), error))
            out.saves = moved;
    }
    const auto bundled = ExecutableDirectory() / "runtime" / "shadps4.exe";
    if (out.use_bundled_runtime)
        out.runtime = fs::is_regular_file(bundled) ? PathText(bundled) : std::string{};
    return out;
}
void Profile::Save(const Paths &paths) const {
    if (!ValidGameRefreshHz(refresh_hz)) throw std::runtime_error("Invalid game frame-rate cap.");
    Write(paths.Profile(), Json{{"game", game},{"dlc",dlc},{"refresh_hz",refresh_hz},{"follow_headset",follow_headset},
                                {"intel_sfx_workaround", intel_sfx_workaround},
                                {"non_aggro", non_aggro},
                                {"free_camera", free_camera},
                                {"headset", headset},
                                {"stereo", stereo == StereoChoice::Dibr ? "dibr"
                                           : stereo == StereoChoice::Native ? "native" : "auto"},
                                {"native_views",
                                 native_views == NativeViews::Multiview ? "multiview"
                                 : native_views == NativeViews::Layers  ? "layers"
                                                                        : "auto"},
                                {"saves", saves},
                                {"runtime", runtime},
                                {"fullscreen", fullscreen},
                                {"fullscreen_chosen", fullscreen_chosen},
                                {"check_updates", check_updates},
                                {"check_updates_chosen", check_updates_chosen},
                                {"use_bundled_runtime", use_bundled_runtime}}
                                   .dump(2) +
                               "\n");
}
// The Old Hunters' three areas in the game's own files: Hunter's Nightmare (m34), the Research
// Hall (m35) and the Fishing Hamlet (m36), each its map folder and its MapStudio layout. The Game
// of the Year Edition carries the DLC's licence itself: with no add-on mounted its saves mark the
// DLC in use (userdata0010 byte 0x16 = 1; CUSA00900 without the add-on: 0, 2026-11-03). The areas
// are in every 1.09 game folder, licensed or not, so they only show this edition's folder is
// complete; they never prove the DLC for another edition.
static bool BundledOldHunters(const fs::path &game) {
    const auto map = game / "dvdroot_ps4" / "map";
    for (const char *area : {"m34_00_00_00", "m35_00_00_00", "m36_00_00_00"}) {
        std::error_code error;
        if (!fs::is_directory(map / area, error) ||
            !fs::is_regular_file(map / "MapStudio" / (std::string(area) + ".msb.dcx"), error))
            return false;
    }
    return true;
}
DlcStatus ResolveDlc(const Paths& paths, const Profile& profile) {
    const auto game = fs::absolute(GameExecutable(Path(profile.game))).parent_path();
    const auto title = SfoText(game / "sce_sys" / "param.sfo", "TITLE_ID");
    if (!title || !Common::Addons::TitleId(*title))
        return {{}, "Cannot detect DLC: the game's TITLE_ID is missing or invalid.", false};
    fs::path configured;
    const auto text = Read(paths.Config());
    if (!text.empty()) {
        const auto json = Json::parse(text);
        // Most configs have no addon_install_dir (or an empty one): no configured folder, not an
        // error (Path("") throws "Choose a folder or file first.", which blocked every launch).
        if (json.contains("General") && json["General"].is_object()) {
            const auto& general = json["General"];
            if (general.contains("addon_install_dir") && general["addon_install_dir"].is_string() &&
                !VR::Ini::Trim(general["addon_install_dir"].get<std::string>()).empty())
                configured = Path(general["addon_install_dir"].get<std::string>());
        }
    }
    // The runtime starts in the data directory, not the launcher's working directory.
    // Resolve typed paths now; existing relative runtime config is relative to that data root.
    const auto data_root = fs::absolute(paths.root);
    if (!configured.empty() && configured.is_relative()) configured = data_root / configured;
    const auto requested = profile.dlc.empty() ? fs::path{} : fs::absolute(Path(profile.dlc));
    const auto selected = Common::Addons::Find(game, requested, configured,
                                               data_root/"user"/"addcont", *title, "SPEXPANSIONDLC03");
    // The Game of the Year Edition (CUSA03173) has The Old Hunters in the game itself: said to be
    // included only when its three areas are really in the game folder (BundledOldHunters).
    const bool bundled = *title == "CUSA03173";
    const bool bundled_found = bundled && BundledOldHunters(game);
    const std::string bundled_missing =
        "The Old Hunters not found: this edition includes it, but its areas are missing from the "
        "game folder (the game starts without it).";
    if (!selected.scan.entries.empty()) {
        const bool hunters = std::any_of(selected.scan.entries.begin(), selected.scan.entries.end(),
            [](const auto& e){ return e.label == "SPEXPANSIONDLC03"; });
        std::string message = hunters ? "DLC FOUND: The Old Hunters."
            : bundled_found ? "DLC INCLUDED: The Old Hunters is built into this edition."
            : bundled       ? bundled_missing
                            : "Additional content detected.";
        if (selected.scan.limited || selected.scan.unreadable)
            message += " (Some folders could not be checked; choose the DLC folder directly.)";
        return {selected.root, message, true, hunters || bundled_found};
    }
    // A stale separate-DLC choice must not block an edition with bundled DLC.
    if (bundled)
        return {data_root/"user"/"addcont",
                bundled_found ? "DLC INCLUDED: The Old Hunters is built into this edition."
                              : bundled_missing,
                true, bundled_found};
    if (!profile.dlc.empty())
        return {selected.root, "No compatible extracted DLC found for " + *title +
            " (the game starts without it). Choose the add-on itself, its parent, or an addcont "
            "folder. Each add-on needs sce_sys/param.sfo with matching game metadata; PKG files "
            "and folder names alone are not enough. Clear this field to use automatic discovery.", false};
    std::string message = "The Old Hunters not detected (the game starts without it). If installed elsewhere, choose its folder below.";
    if (selected.scan.limited || selected.scan.unreadable)
        message += " Some folders could not be checked; choose the DLC folder directly.";
    return {selected.root, message, true, false};
}
void ValidateSetup(const Profile &profile) {
    if (!ValidGameRefreshHz(profile.refresh_hz)) throw std::runtime_error("Invalid game frame-rate cap.");
    if (profile.game.empty())
        throw std::runtime_error("Choose your game folder to begin.");
    GameExecutable(Path(profile.game));
    const auto runtime = Path(profile.runtime);
    if (!fs::is_regular_file(runtime) || _wcsicmp(runtime.filename().c_str(), L"shadps4.exe") != 0)
        throw std::runtime_error(
            "The bundled VR runtime is missing. Open Bldbrne-VR from a complete installation.");
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
fs::path LaunchBackup(const Paths &paths, const fs::path &home) {
    if (auto made = BackupSaves(paths, home); !made.empty())
        return made;
    // Unchanged since the newest backup: that one holds these very saves.
    const auto fingerprint = SaveFingerprint(home);
    const auto backups = SaveBackups(paths);
    if (!fingerprint.empty() && !backups.empty() &&
        Read(backups.front().folder / "fingerprint.txt") == fingerprint)
        return backups.front().folder;
    return {};
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
    const bool valid = out[0] == 1 && out[1] >= -50 && out[1] <= 50 &&
                       out[2] >= 0 && out[2] <= 4 && out[3] >= 0 && out[3] <= 5 && out[4] >= 0 &&
                       out[4] <= 5 && out[5] >= 0 && out[5] <= 1 && out[6] >= 0 && out[6] <= 2 &&
                       std::all_of(out.begin() + 7, out.end(),
                                   [](int v) { return v >= 0 && v <= 100; });
    return valid ? out : DefaultGraphics;
}
const char *PresetName(int preset) {
    switch (preset) {
    case 0: return "Performance";
    case 1: return "Balanced";
    case 2: return "Quality";
    case 3: return "Ultra Performance";
    case 4: return "Ultra Quality";
    case 5: return "Balanced Plus";
    default: return "Unknown";
    }
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
bool Fsr31Included(const fs::path &runtime) {
    return IsFile(runtime.parent_path() / "fsr31" / "vr_fsr31_stereo.dll");
}
// FSR 4.1: its bridge and AMD's signed loader and upscaler (FSR SDK 2.4.0) in runtime/fsr41.
bool Fsr41Included(const fs::path &runtime) {
    const auto folder = runtime.parent_path() / "fsr41";
    return IsFile(folder / "vr_fsr41_stereo.dll") &&
           IsFile(folder / "amd_fidelityfx_loader_vk.dll") &&
           IsFile(folder / "amd_fidelityfx_upscaler_vk.dll");
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
// The game's crash record (src/common/crash_record.h: user/log/crash.txt, written by every build,
// the play build too): its newest lines when it changed in the last few minutes (this crash).
static std::vector<std::string> RecentCrashRecord(const Paths &paths) {
    std::vector<std::string> out;
    const auto file = paths.User() / "log" / "crash.txt";
    std::error_code error;
    const auto written = fs::last_write_time(file, error);
    if (error || fs::file_time_type::clock::now() - written > std::chrono::minutes(5))
        return out;
    std::istringstream lines(Read(file));
    std::vector<std::string> all;
    for (std::string line; std::getline(lines, line);) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (!line.empty())
            all.push_back(line);
    }
    // Keep the last three notes, including their indented diagnostic lines. Cap the output
    // even if a damaged record consists entirely of continuation lines.
    size_t first = all.size();
    size_t notes = 0;
    while (first > 0 && all.size() - first < 64) {
        --first;
        if (all[first].front() != ' ' && ++notes == 3)
            break;
    }
    for (size_t i = first; i < all.size(); ++i) {
        if (all[i].find("out of memory") != std::string::npos) {
            out.push_back("The PC ran out of memory (graphics memory, or RAM and page file). Close "
                          "other programs, keep 30 GB or more free on the system drive (Windows "
                          "grows its page file there), or lower Graphics > Scaling.");
            break;
        }
    }
    for (size_t i = first; i < all.size(); ++i)
        out.push_back("Crash record: " + all[i]);
    if (!out.empty())
        out.push_back("Details for a report: " + PathText(file));
    return out;
}

std::vector<std::string> LogProblems(const Paths &paths, size_t max) {
    std::vector<std::string> out;
    const auto file = LogFile(paths);
    if (file.empty())
        return RecentCrashRecord(paths); // the play build: no log, but its crash record
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
    for (auto &line : RecentCrashRecord(paths))
        out.push_back(std::move(line));
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
    if (!DlssIncluded(runtime) || !IsFile(runtime.parent_path() / "dlss" / "nvngx.dll_dlssnr.dll"))
        return false;
    // Mapped without running it or loading what it imports: only its export table is read.
    const HMODULE bridge = LoadLibraryExW((runtime.parent_path() / "dlss" / "vr_dlss_stereo.dll").c_str(),
                                          nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!bridge)
        return false;
    const bool neural = GetProcAddress(bridge, "VrDlssSetNeuralConfig") != nullptr &&
                        GetProcAddress(bridge, "VrDlssSetNeuralRuntime") != nullptr;
    FreeLibrary(bridge);
    return neural;
}
namespace {
std::wstring RegistryText(HKEY root, const wchar_t *key, const wchar_t *value) {
    wchar_t text[1024]{};
    DWORD size = sizeof(text);
    if (RegGetValueW(root, key, value, RRF_RT_REG_SZ, nullptr, text, &size) != ERROR_SUCCESS)
        return {};
    return text;
}
// The folders games are installed in: every Steam library's steamapps/common, Epic's install
// locations, and the Xbox app's and Epic's default folders on each drive.
std::vector<fs::path> GameFolders() {
    std::vector<fs::path> roots, out;
    const auto steam = RegistryText(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath");
    if (!steam.empty()) {
        roots.push_back(fs::path(steam) / "steamapps" / "common");
        // libraryfolders.vdf lines: "path"		"D:\\SteamLibrary"
        std::istringstream vdf(Read(fs::path(steam) / "steamapps" / "libraryfolders.vdf"));
        for (std::string line; std::getline(vdf, line);) {
            const auto key = line.find("\"path\"");
            if (key == std::string::npos)
                continue;
            const auto open = line.find('"', key + 6), close = line.rfind('"');
            if (open == std::string::npos || close <= open)
                continue;
            std::string path;
            for (size_t i = open + 1; i < close; ++i) {
                path += line[i];
                if (line[i] == '\\' && i + 1 < close && line[i + 1] == '\\')
                    ++i;
            }
            roots.push_back(fs::path(Wide(path)) / "steamapps" / "common");
        }
    }
    std::error_code error;
    for (const auto &item :
         fs::directory_iterator("C:/ProgramData/Epic/EpicGamesLauncher/Data/Manifests", error)) {
        if (item.path().extension() != ".item")
            continue;
        try {
            const auto json = Json::parse(Read(item.path()));
            if (json.contains("InstallLocation"))
                out.push_back(fs::path(Wide(json["InstallLocation"].get<std::string>())));
        } catch (const std::exception &) {
        }
    }
    for (wchar_t drive = L'C'; drive <= L'Z'; ++drive) {
        const std::wstring root{drive, L':', L'\\'};
        if (GetDriveTypeW(root.c_str()) != DRIVE_FIXED)
            continue;
        for (const wchar_t *folder :
             {L"XboxGames", L"Program Files\\Epic Games", L"SteamLibrary\\steamapps\\common"})
            roots.push_back(fs::path(root) / folder);
    }
    for (const auto &root : roots) {
        for (const auto &item : fs::directory_iterator(root, error))
            if (item.is_directory(error))
                out.push_back(item.path());
        error.clear();
    }
    return out;
}
bool SameName(const fs::path &path, const wchar_t *name) {
    return _wcsicmp(path.filename().c_str(), name) == 0;
}
} // namespace
std::vector<Dlss5Found> FindDlss5(const fs::path &runtime) {
    std::vector<fs::path> files;
    std::error_code error;
    const auto options = fs::directory_options::skip_permission_denied;
    // A game keeps it in its own folder or a few folders down (bin/, Binaries/Win64/...).
    for (const auto &game : GameFolders()) {
        for (fs::recursive_directory_iterator it(game, options, error), end; !error && it != end;
             it.increment(error)) {
            if (it.depth() >= 4)
                it.disable_recursion_pending();
            if (SameName(it->path(), VR::Dlss5File::Name))
                files.push_back(it->path());
        }
        error.clear();
    }
    // NVIDIA's driver keeps the DLSS files it downloads under NGX/models/<feature>/versions.
    for (fs::recursive_directory_iterator it("C:/ProgramData/NVIDIA/NGX/models", options, error), end;
         !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error) && PathText(it->path()).find("dlssnr") != std::string::npos)
            files.push_back(it->path());
    }
    error.clear();
    files.push_back(runtime.parent_path() / "dlss" / VR::Dlss5File::Name);
    std::vector<Dlss5Found> out;
    for (const auto &file : files) {
        if (!IsFile(file) || std::any_of(out.begin(), out.end(), [&](const Dlss5Found &f) {
                return fs::equivalent(f.file, file, error);
            }))
            continue;
        out.push_back({file, VR::Dlss5File::Verify(file)});
    }
    std::stable_sort(out.begin(), out.end(), [](const Dlss5Found &a, const Dlss5Found &b) {
        if (a.check.ok != b.check.ok)
            return a.check.ok;
        return VR::Dlss5File::Newer(a.check.version, b.check.version);
    });
    return out;
}
VR::Dlss5File::Check AddDlss5(const Paths &paths, const fs::path &from) {
    const auto source = VR::Dlss5File::Verify(from);
    if (!source.ok)
        throw std::runtime_error(source.problem);
    const auto target = VR::Dlss5File::InUser(paths.User());
    std::error_code error;
    if (fs::equivalent(from, target, error))
        return source;
    fs::create_directories(target.parent_path());
    auto temporary = target;
    temporary += ".new";
    fs::copy_file(from, temporary, fs::copy_options::overwrite_existing);
    const auto copy = VR::Dlss5File::Verify(temporary);
    if (!copy.ok) {
        fs::remove(temporary, error);
        throw std::runtime_error("The copy failed its check: " + copy.problem);
    }
    fs::rename(temporary, target);
    return copy;
}
void RemoveDlss5(const Paths &paths) {
    std::error_code error;
    fs::remove(VR::Dlss5File::InUser(paths.User()), error);
    if (error)
        throw std::runtime_error("Cannot remove the DLSS 5 file: " + error.message());
}
// The four-digit model after `series` in a card's name ("NVIDIA GeForce RTX 4070 Ti" -> 4070,
// `rest` " Ti"); 0 without one.
static int ModelNumber(std::string_view name, std::string_view series, std::string_view &rest) {
    for (size_t at = name.find(series); at != std::string_view::npos;
         at = name.find(series, at + 1)) {
        size_t i = at + series.size();
        while (i < name.size() && name[i] == ' ')
            ++i;
        size_t j = i;
        while (j < name.size() && j - i < 5 && std::isdigit(static_cast<unsigned char>(name[j])))
            ++j;
        int n = 0;
        if (j - i == 4 && std::from_chars(name.data() + i, name.data() + j, n).ec == std::errc{}) {
            rest = name.substr(j);
            return n;
        }
    }
    rest = {};
    return 0;
}
StartupUpscale StartupUpscaler(const SystemInfo &system) {
    constexpr uint64_t GiB = 1ull << 30;
    // A "10 GB" card reports a little under 10 GiB of dedicated memory.
    if (system.integrated || system.vram < 10 * GiB - GiB / 2)
        return {};
    const std::string_view name = system.gpu;
    const bool laptop = name.find("Laptop") != std::string_view::npos ||
                        name.find("Max-Q") != std::string_view::npos;
    std::string_view rest;
    if (system.nvidia) {
        const int n = ModelNumber(name, "RTX", rest);
        if (n < 3000 || n >= 6000)
            return {};
        const int tier = (n / 10) % 10; // 5090 -> 9
        if (!laptop && n / 1000 == 5 && tier == 9)
            return {1, 4}; // Ultra Quality
        // Quality: RTX 5070 / 5080, and the RTX 4080 / 4090 (as fast as them or faster).
        if (!laptop && ((n / 1000 == 5 && (tier == 7 || tier == 8)) ||
                        (n / 1000 == 4 && (tier == 8 || tier == 9))))
            return {1, 2};
        return {1, 1}; // Balanced
    }
    if (system.amd) {
        const int n = ModelNumber(name, "RX", rest);
        // A mobile chip: "RX 7600M XT", "RX 6800S".
        if (laptop || (!rest.empty() && (rest.front() == 'M' || rest.front() == 'S')))
            return {};
        const int series = n / 1000, hundreds = (n / 100) % 10, tens = (n / 10) % 10;
        // A GRE (RX 9070 GRE, 7900 GRE: about an RTX 4070's speed) gets Quality, not Ultra
        // Quality: FSR's Ultra Quality renders 75%, more than DLSS Quality's 67%.
        const StartupUpscale top =
            rest.find("GRE") != std::string_view::npos ? StartupUpscale{3, 2} : StartupUpscale{3, 4};
        if (series == 9 && hundreds == 0)
            return tens == 7 ? top : tens == 6 ? StartupUpscale{3, 5} : StartupUpscale{};
        if (series == 7)
            return hundreds == 9   ? top
                   : hundreds == 8 ? StartupUpscale{3, 2}
                   : hundreds == 7 ? StartupUpscale{3, 5}
                                   : StartupUpscale{};
        if (series == 6)
            return hundreds >= 8 ? StartupUpscale{3, 2}
                   : hundreds == 7 ? StartupUpscale{3, 5}
                                   : StartupUpscale{};
    }
    return {};
}
GraphicsFields StartupGraphics(const SystemInfo &system) {
    auto out = DefaultGraphics;
    const auto pick = StartupUpscaler(system);
    out[2] = pick.upscaler;
    if (pick.upscaler == 1)
        out[3] = pick.preset;
    else if (pick.upscaler == 3)
        out[4] = pick.preset;
    return out;
}
int StartupFoveation(const SystemInfo &system) {
    const auto pick = StartupUpscaler(system);
    return pick.upscaler != 0 && (pick.preset == 2 || pick.preset == 4) ? 3 : -1;
}
bool StartupReducedShadows(const SystemInfo &system) {
    constexpr uint64_t GiB = 1ull << 30;
    // Below 9.5 GiB is 8 GB or less; unknown memory: nothing written.
    return system.vram != 0 && (system.integrated || system.vram < 10 * GiB - GiB / 2);
}
std::string NvidiaDriver(uint64_t umd) {
    // 32.0.15.6094 -> 560.94: the last digit of the third part and the fourth part.
    const auto value = (umd >> 16 & 0xffff) % 10 * 10000 + (umd & 0xffff);
    const auto text = std::to_string(value);
    return text.size() < 3 ? text : text.substr(0, text.size() - 2) + "." + text.substr(text.size() - 2);
}
namespace {
// The card's multiview features through the system Vulkan loader, matched to the DXGI adapter by
// vendor and device id. Unknown (no loader, no match, an error): nothing is assumed.
struct MultiviewSupport {
    bool known{}, multiview{}, geometry{}, tessellation{};
};
MultiviewSupport ProbeMultiview(UINT vendor, UINT device) {
    MultiviewSupport out;
    const HMODULE lib = LoadLibraryExW(L"vulkan-1.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!lib)
        return out;
    const auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(
        reinterpret_cast<void *>(GetProcAddress(lib, "vkGetInstanceProcAddr")));
    const auto create =
        gipa ? reinterpret_cast<PFN_vkCreateInstance>(gipa(nullptr, "vkCreateInstance")) : nullptr;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Bldbrne-VR launcher";
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    VkInstance instance = VK_NULL_HANDLE;
    if (create && create(&info, nullptr, &instance) == VK_SUCCESS && instance) {
        const auto destroy =
            reinterpret_cast<PFN_vkDestroyInstance>(gipa(instance, "vkDestroyInstance"));
        const auto enumerate = reinterpret_cast<PFN_vkEnumeratePhysicalDevices>(
            gipa(instance, "vkEnumeratePhysicalDevices"));
        const auto properties = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(
            gipa(instance, "vkGetPhysicalDeviceProperties"));
        const auto features = reinterpret_cast<PFN_vkGetPhysicalDeviceFeatures2>(
            gipa(instance, "vkGetPhysicalDeviceFeatures2"));
        uint32_t count = 0;
        if (enumerate && properties && features &&
            enumerate(instance, &count, nullptr) == VK_SUCCESS) {
            std::vector<VkPhysicalDevice> devices(count);
            if (enumerate(instance, &count, devices.data()) >= VK_SUCCESS)
                for (uint32_t i = 0; i < count && !out.known; ++i) {
                    VkPhysicalDeviceProperties p{};
                    properties(devices[i], &p);
                    if (p.vendorID != vendor || p.deviceID != device ||
                        p.apiVersion < VK_API_VERSION_1_1)
                        continue;
                    VkPhysicalDeviceMultiviewFeatures mv{
                        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MULTIVIEW_FEATURES};
                    VkPhysicalDeviceFeatures2 f{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
                    f.pNext = &mv;
                    features(devices[i], &f);
                    out = {true, mv.multiview == VK_TRUE, mv.multiviewGeometryShader == VK_TRUE,
                           mv.multiviewTessellationShader == VK_TRUE};
                }
        }
        if (destroy)
            destroy(instance, nullptr);
    }
    // The loader stays loaded: unloading it while an overlay layer's thread lingers can crash.
    return out;
}
} // namespace
SystemInfo ReadSystem(bool probe_multiview) {
    SystemInfo out;
    UINT vendor = 0, device = 0;
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
            out.amd = d.VendorId == 0x1002;
            out.auto_layers = VR::NativeViews::LayersByDefault(d.VendorId, d.DeviceId, out.gpu);
            out.auto_dibr = VR::NativeViews::DibrByDefault(d.VendorId, d.DeviceId, out.gpu);
            out.vram = d.DedicatedVideoMemory;
            vendor = d.VendorId;
            device = d.DeviceId;
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
    if (vendor && probe_multiview) {
        // Once per card and run: creating a Vulkan instance costs tens of milliseconds.
        static std::mutex probe_mutex; // the scan and the UI both read the system
        std::lock_guard probe_lock{probe_mutex};
        static UINT probed_vendor = 0, probed_device = 0;
        static MultiviewSupport probed{};
        if (probed_vendor != vendor || probed_device != device) {
            probed = ProbeMultiview(vendor, device);
            probed_vendor = vendor;
            probed_device = device;
        }
        out.multiview_known = probed.known;
        out.multiview_limited =
            probed.known && !(probed.multiview && probed.geometry && probed.tessellation);
    }
    constexpr uint64_t GiB = 1ull << 30;
    // Integrated graphics report a small carve-out (typically 128-512 MB) as dedicated memory.
    out.integrated = out.vram < GiB;
    if (out.gpu.empty())
        out.notes.push_back("No graphics card was found.");
    else if (out.integrated)
        out.notes.push_back("This looks like integrated graphics. Bldbrne-VR needs a dedicated graphics "
                            "card.");
    else if (out.vram < 8 * GiB - GiB / 2)
        out.notes.push_back("Less than 8 GB of video memory has not been tested.");
    if (out.multiview_limited)
        out.notes.push_back("This graphics driver lacks the multiview support native stereo needs "
                            "(geometry or tessellation shaders). Auto uses Simplified Rendering.");
    else if (out.auto_dibr)
        out.notes.push_back("On this AMD card Auto uses Simplified Rendering. Native stereo draws each "
                            "eye itself (layers). Native stereo is untested on it.");
    if (out.ram && out.ram < 16 * GiB - GiB)
        out.notes.push_back("Less than 16 GB of system memory has not been tested.");
    return out;
}
fs::path CreatePlayShortcut() {
    const auto check = [](HRESULT result, const char *action) {
        if (FAILED(result)) {
            const int code = HRESULT_FACILITY(result) == FACILITY_WIN32 ? HRESULT_CODE(result)
                                                                        : int(result);
            throw std::system_error(code, std::system_category(), action);
        }
    };
    PWSTR desktop = nullptr;
    check(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop), "Cannot locate your desktop");
    const auto file = fs::path(desktop) / L"Play Bldbrne-VR.lnk";
    CoTaskMemFree(desktop);
    std::wstring exe(32768, L'\0');
    exe.resize(GetModuleFileNameW(nullptr, exe.data(), DWORD(exe.size())));
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> persist;
    check(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                          IID_PPV_ARGS(&link)), "Could not start Windows shortcut creation");
    if (
        FAILED(link->SetPath(exe.c_str())) || FAILED(link->SetArguments(L"--play")) ||
        FAILED(link->SetWorkingDirectory(fs::path(exe).parent_path().c_str())) ||
        FAILED(link->SetIconLocation(exe.c_str(), 0)) ||
        FAILED(link->SetDescription(L"Start Bldbrne-VR with your launcher settings")) ||
        FAILED(link.As(&persist)))
        throw std::runtime_error("Could not configure the Play Bldbrne-VR shortcut.");
    const auto save_message = "Could not save the shortcut to " + PathText(file);
    check(persist->Save(file.c_str(), TRUE),save_message.c_str());
    return file;
}
// A shortcut's target, or empty when it cannot be read.
static fs::path ShortcutTarget(const fs::path &file) {
    Microsoft::WRL::ComPtr<IShellLinkW> link;
    Microsoft::WRL::ComPtr<IPersistFile> persist;
    std::wstring target(32768, L'\0');
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&link))) ||
        FAILED(link.As(&persist)) || FAILED(persist->Load(file.c_str(), STGM_READ)) ||
        FAILED(link->GetPath(target.data(), int(target.size()), nullptr, SLGP_RAWPATH)))
        return {};
    target.resize(wcslen(target.c_str()));
    return target;
}
// Whether a Play shortcut's target is this launcher's to take over: gone; in this launcher's own
// folder (a portable ZIP extracted over an older one leaves the old BloodborneVR.exe beside it);
// or in the old installer's folder, %LocalAppData%\BloodborneVR (the app this one replaces).
static bool OursToRepair(const fs::path &target) {
    std::error_code error;
    if (!fs::exists(target, error))
        return true;
    std::wstring self(32768, L'\0');
    self.resize(GetModuleFileNameW(nullptr, self.data(), DWORD(self.size())));
    if (fs::equivalent(target.parent_path(), fs::path(self).parent_path(), error))
        return true;
    const std::wstring installed = (LocalAppData() / LegacyInstallFolderName).native() + L"\\";
    return _wcsnicmp(target.c_str(), installed.c_str(), installed.size()) == 0;
}
bool RepairPlayShortcut() {
    PWSTR desktop = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desktop)))
        return false;
    const fs::path folder = desktop;
    CoTaskMemFree(desktop);
    const auto current = folder / L"Play Bldbrne-VR.lnk", legacy = folder / L"Play BB-VR.lnk";
    std::error_code error;
    std::wstring self(32768, L'\0');
    self.resize(GetModuleFileNameW(nullptr, self.data(), DWORD(self.size())));
    bool repaired = false;
    // The shortcut's name before 0.2.7: replaced by the new one when it was this app's.
    if (fs::exists(legacy, error)) {
        const auto target = ShortcutTarget(legacy);
        if (!target.empty() && OursToRepair(target)) {
            CreatePlayShortcut();
            fs::remove(legacy, error);
            repaired = true;
        }
    }
    if (!repaired && fs::exists(current, error)) {
        const auto target = ShortcutTarget(current);
        // This very launcher: nothing to do. Another working copy's: left alone.
        if (!target.empty() && !fs::equivalent(target, fs::path(self), error) &&
            OursToRepair(target)) {
            CreatePlayShortcut();
            repaired = true;
        }
    }
    return repaired;
}
LaunchPlan Prepare(const Profile &profile, const Paths &paths) {
    return Prepare(profile, paths, profile.SelectedStereo(ReadSystem()));
}
LaunchPlan Prepare(const Profile &profile, const Paths &paths, StereoPreset preset) {
    ValidateSetup(profile);
    const auto game = GameExecutable(Path(profile.game));
    const auto runtime = Path(profile.runtime);
    Mods::CheckForLaunch(Mods::GameRoot(game), runtime);
    const auto home = NormalizeSaveHome(Path(profile.saves));
    const auto patch = PatchFile(runtime);
    const auto version = CheckGameBuild(game);
    auto launch_patch = PatchForVersion(Read(patch), version);
    launch_patch = PatchIntelSfx(launch_patch, game, profile.intel_sfx_workaround);
    // Free camera brings its own non-aggro while it is on (bloodborne_camera.cpp): the patch is
    // only the player's own Non-aggro mode, so Y in a free camera launch gives aggro back.
    launch_patch = PatchNonAggro(launch_patch, profile.non_aggro);
    if(profile.refresh_hz!=90)launch_patch=PatchFrameRate(launch_patch,profile.refresh_hz);
    const auto runtime_argument = ArgumentPath(runtime);
    const auto game_argument = ArgumentPath(game);
    // DLC is optional: found or not, a folder that holds none or a check that fails, the game
    // launches. Only a usable folder is written; otherwise config.json's add-on folder is left.
    DlcStatus dlc;
    try {
        dlc = ResolveDlc(paths, profile);
    } catch (const std::exception &e) {
        dlc = {{}, std::string("Couldn't check for DLC (the game starts without it): ") + e.what(), false};
    }
    std::optional<fs::path> addons;
    if (dlc.valid && !dlc.root.empty())
        addons = dlc.root;
    const auto save_backup = LaunchBackup(paths, home);
    fs::create_directories(paths.User());
    const auto before = Read(paths.Config());
    Write(paths.Config(), LaunchConfig(before, home, profile.refresh_hz, addons), before);
    const auto ini_before = Read(paths.Ini());
    const auto ini = LaunchIni(ini_before, preset, profile.refresh_hz, profile.follow_headset);
    Write(paths.Ini(), ini, ini_before);
    // The patch goes by bare name: the game finds it in user/patches through its own (wide)
    // user path, whatever characters the installation's path has.
    const auto patch_copy = paths.User() / "patches" / "bbvr_patch.xml";
    Write(patch_copy, launch_patch, Read(patch_copy));
    LaunchPlan plan{runtime, paths.root};
    // The SFX line stays last (the support report's Windows details follow it).
    plan.game_support_info = GameSupportInfo(game) + "DLC: " + dlc.message +
                             "\nDLC directory: " + PathText(dlc.root) + "\nFree camera " +
                             (profile.free_camera ? "ON\n" : "OFF\n") + "Non-aggro mode " +
                             (profile.non_aggro       ? "ON\n"
                              : profile.free_camera ? "WHILE FREE CAMERA\n"
                                                    : "OFF\n") +
                             "Skip dangerous SFX " +
                             (profile.intel_sfx_workaround ? "ON\n" : "OFF\n");
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
            _wcsnicmp(at, L"MOCKXR_", 7) != 0 && _wcsnicmp(at, L"BBVR_BENCHMARK", 14) != 0 &&
            _wcsnicmp(at, L"BBVR_SHADER_CACHE_ROOT=", 23) != 0)
            entries.emplace_back(at);
    }
    FreeEnvironmentStringsW(environment);
    entries.push_back(L"VR_INI=" + ArgumentPath(paths.Ini()));
    const auto dynamic_resolution = VR::Ini::ValueOf(ini, "dynamic_resolution");
    entries.emplace_back(dynamic_resolution && SameValue(*dynamic_resolution, "0")
                             ? L"VR_S1_DRS=0" : L"VR_S1_DRS=1");
    // Soft save for every player (2026-10-30, the user): the game turns it on only when this
    // backup is a byte-for-byte copy of the save it starts with (core/vr/soft_save_backup.h).
    if (!save_backup.empty())
        entries.push_back(L"VR_SOFT_SAVE_LAUNCHER_BACKUP=" + ArgumentPath(save_backup));
    if (profile.free_camera) entries.emplace_back(L"VR_FREE_CAMERA=1");
    for (const auto *entry : RendererEnvironment) {
        // Native flags are presence-based: '=0' would still turn stereo on.
        if (preset == StereoPreset::Dibr && std::wstring_view(entry).starts_with(L"VR_NATIVE_"))
            continue;
        entries.emplace_back(entry);
    }
    if (preset == StereoPreset::Native)
        entries.push_back(L"VR_NATIVE_IPD=" + NativeIpd(ini, profile.use_bundled_runtime));
    // Native stereo's eyes, a DIBR launch's too: it only names the mode (no stereo of its own),
    // and the runtime's shader cache then stays the same across DIBR and native sessions.
    if (const auto views = NativeViewsValue(profile.native_views); !views.empty())
        entries.push_back(L"VR_NATIVE_VIEWS=" + views);
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
std::optional<HeadsetRequest> LastHeadsetRequest(const Paths &paths) {
    try {
        const auto text = Read(paths.User() / "log" / "headset.json");
        if (text.empty())
            return std::nullopt;
        const auto json = Json::parse(text);
        HeadsetRequest out{json.value("system", std::string{})};
        for (int eye = 0; eye < 2; ++eye) {
            out.width = std::max(out.width, json.at("width").at(eye).get<unsigned>());
            out.height = std::max(out.height, json.at("height").at(eye).get<unsigned>());
        }
        out.max_width = json.value("max_width", 8192u);
        out.max_height = json.value("max_height", 8192u);
        if (out.max_width < 64 || out.max_height < 64 ||
            out.max_width > 8192 || out.max_height > 8192)
            return std::nullopt;
        if (!out.width || !out.height || out.width > 16384 || out.height > 16384)
            return std::nullopt;
        return out;
    } catch (const std::exception &) {
        return std::nullopt;
    }
}
// The in-game tabs the launcher leaves to the game: Upscaling (its Graphics tab has it all) and
// Save (the soft save's buttons).
static bool GameOnlyTab(std::string_view name) {
    return name == "Upscaling" || name == "Save";
}
const std::vector<const char *> &Categories() {
    static const std::vector<const char *> all = [] {
        std::vector<const char *> out;
        for (const auto &tab : VR::Page::Tabs())
            if (!GameOnlyTab(tab.name))
                out.push_back(tab.name);
        out.push_back("Graphics");
        out.push_back("Extras");
        return out;
    }();
    return all;
}
std::optional<std::string> ChoiceOverride(std::string_view key, std::optional<std::string> value) {
    if (value && key == "light_shadow_detail") {
        // Older generated files wrote "low" even when the card still chose shadow enablement, and
        // the old Low (four sharp lamps) is today's Medium: both read as the default. An explicit
        // Game's or Low selection also sets light_shadows, which remains an override. (2026-11-20:
        // Low is "lite", Medium "medium", High "high": light_shadow.h TierOf.)
        if (*value == "low") return std::nullopt;
        if (*value == "0") return "medium";
        if (*value == "1") return "high";
    }
    if (value && key == "water_reflections")
        return VR::WaterReflectionsEnabled(*value) ? "1" : "0";
    return value;
}

std::optional<std::string> CardDefault(std::string_view key, uint64_t dedicated_bytes) {
    const auto card = VR::CardTier::For(VR::CardTier::Of(dedicated_bytes));
    if (key == "light_shadows") return card.lamp_shadows ? "1" : "0";
    if (key == "light_shadow_detail") return VR::CardTier::LampShadowDetail(dedicated_bytes);
    if (key == "fog_banks") return card.fog_banks ? "1" : "0";
    if (key == "moon_shafts") return card.moon_glow ? "1" : "0";
    if (key == "ambient_occlusion") return card.ambient_occlusion ? "1" : "0";
    if (key == "bounce_light") return card.bounce_light ? "1" : "0";
    if (key == "water_reflections") return card.water_reflections ? "1" : "0";
    if (key == "wet_streets") return card.wet_streets ? "1" : "0";
    return std::nullopt;
}

bool FogEnabled(const std::optional<std::string> &choice, uint64_t dedicated_bytes) {
    if (choice)
        return *choice == "1" || *choice == "true";
    return VR::CardTier::For(VR::CardTier::Of(dedicated_bytes)).fog_banks;
}

std::optional<std::string> CodeDefault(const std::string &key) {
    // core/vr/vr_session.h, two_hand_grip.h, body_settings.h and quick_settings.h (2026-10-31);
    // foveation: core/vr/graphics_settings.h (-1, Auto: on where the card supports it).
    static const std::map<std::string, std::string, std::less<>> defaults = [] {
        std::map<std::string, std::string, std::less<>> d{
            {"foveation", "-1"},             {"upscale_focus", "2"},
            {"dynamic_resolution", "1"},
            {"snap_turn_deg", "30"},         {"smooth_turn_deg_per_s", "120"},
            {"vitals_heartbeat", "1"},       {"vitals_edge", "1"},
            {"health_color", "1"},           {"health_color_strength", "0.5"},
            {"stamina_color", "1"},          {"stamina_color_strength", "0.7"},
            {"ui_enemy_hp_bars", "0"},       {"ui_boss_bar", "1"},
            {"melee_kill_ragdoll", "1"},     {"melee_ragdoll_impulse", "1"},
            {"two_hand_optimized_controls", "0"}, {"two_hand_trigger_hold", "1"},
            {"body_align", "1"},             {"body_posture", "lean"},
            {"quick_arc_pick", "fan"},       {"quick_fan_style", "lancet"},
            {"x_button", "fan"},
            {"canted_eyes", "1"},            {"cinematic_ladder_skip", "1"},
            {"vr_interactions", "1"}};
        // Every other choice row's (2026-11-25: "Game default" names the value it
        // stands for): read from the game's own settings, in vr.ini's words as the VR page's
        // Defaults write them (vr_page_runtime.cpp ValueIn on default settings), so the name
        // follows the code.
        const VR::Settings s{};
        const VR::Body::BodySettings b{};
        const VR::Quick::QuickSettings q{};
        const VR::Tricks::Settings t{};
        const auto flag = [](bool on) { return std::string(on ? "1" : "0"); };
        const auto number = [](double v) {
            char text[32];
            snprintf(text, sizeof(text), "%g", v);
            return std::string(text);
        };
        using Mode = VR::Body::BodyMode;
        const std::pair<const char *, std::string> read[]{
            // Combat
            {"blade_sounds", flag(VR::BladeSound::Settings{}.on)},
            {"gun_wrist_reload", flag(VR::Gun::GunSettings{}.wrist_reload)},
            {"two_hand_guide", flag(VR::TwoHand::Settings{}.guide)},
            {"riposte_style", flag(s.riposte_style)},
            {"weapon_wall", !t.weapon_wall ? "0" : t.weapon_wall_map ? "1" : "2"},
            {"vr_grab_attacks", flag(s.vr_grab_attacks)},
            {"lockon_follow", std::to_string(s.lockon_follow)},
            {"weighted_weapons",
             s.weighted_weapons == 2 ? "2" : flag(s.WeaponWeightStrength() > 0.0f)},
            // Controls
            {"inject", flag(s.inject)},
            {"controls", s.controls_classic ? "classic" : "motion"},
            {"grip_adjust", flag(b.grip_adjust)},
            {"laser", flag(s.laser)},
            {"left_handed", flag(s.left_handed)},
            {"swap_sticks", flag(s.swap_sticks)},
            {"seated", flag(s.seated)},
            // Difficulty
            {"player_hp_bonus_pct", std::to_string(s.player_hp_bonus_pct)},
            {"melee_stamina_pct", std::to_string(VR::Melee::MeleeSettings{}.stamina_pct)},
            {"dodge_iframes_extra", std::to_string(s.dodge_iframes_extra)},
            // Extras
            {"first_person", std::to_string(s.first_person)},
            {"cinematic", flag(s.cinematic)},
            // HUD & feedback
            {"ui_counters", s.ui_counters == 1   ? "always"
                            : s.ui_counters == 2 ? "hidden"
                                                 : "look_down"},
            {"ui_hud_scale_deg", number(s.ui_hud_scale_deg)},
            {"hud_vitals", s.hud_vitals == 1 ? "wrist" : "classic"},
            {"hit_vignette", flag(s.hit_vignette)},
            // Items & world (the lamp page, the summon bell and the levers are on in code: ValueIn)
            {"doll_respec", flag(s.doll_respec)},
            {"holster_mode", std::to_string(q.holster_mode)},
            {"lamp_travel", "1"},
            {"physical_levers", "1"},
            {"physical_summon_bell", "1"},
            {"holster_haptics", flag(q.holster_haptics)},
            {"shoulder_vial", flag(q.shoulder_vial)},
            {"shoulder_switch", flag(s.shoulder_switch)},
            // Picture and Graphics
            {"aa_mode", VR::Aa::Name(VR::Aa::DefaultMode)},
            {"lod_bias", std::to_string(s.lod_bias)},
            {"eye_adaptation", flag(s.eye_adaptation)},
            {"shadows_far", s.shadows_far_reduced ? "reduced" : "full"},
            {"shadows_busy", flag(s.shadows_busy)},
            {"raised_bricks", flag(s.raised_bricks)},
            {"frame_overlap", flag(s.frame_overlap)},
            // You
            {"body_cutaway", flag(b.body_cutaway)},
            {"tp_view_follows_pitch", number(s.tp_view_follows_pitch)},
            {"body_comfort", flag(b.body_comfort)},
            {"height_base", s.height_player ? "player" : "hunter"},
            {"body_mode", b.mode == Mode::ArmsOnly    ? "arms_only"
                          : b.mode == Mode::HandsOnly ? "hands_only"
                          : b.mode == Mode::Full      ? "full"
                                                      : "no_head"},
        };
        for (const auto &[k, v] : read)
            d.emplace(k, v);
        return d;
    }();
    const auto it = defaults.find(key);
    return it == defaults.end() ? std::nullopt : std::optional<std::string>(it->second);
}
// melee_settings.cpp Flag: "1" or "true" only.
static bool StaminaSwitchOn(const std::vector<std::string> &lines) {
    return std::ranges::any_of(lines, [](const std::string &v) { return v == "1" || v == "true"; });
}
std::optional<std::string> StaminaPctAsRead(const std::vector<std::string> &pct_lines,
                                            const std::vector<std::string> &old_switch_lines) {
    // Each pct line as the game's std::stoi reads it (a sign, then digits; anything after them is
    // left; nothing to read, or out of range, refuses the line), the last read one winning. A read
    // one wins over the old switch in either order: before it, the switch is ignored; after it, it
    // sets the value again.
    std::optional<int> pct;
    for (const std::string &line : pct_lines) {
        try {
            pct = std::clamp(std::stoi(line), 0, 100);
        } catch (const std::exception &) {
        }
    }
    if (pct)
        return std::to_string(*pct);
    if (StaminaSwitchOn(old_switch_lines))
        return "100";
    return std::nullopt;
}
std::vector<std::string> ResetAlsoRemoves(std::string_view key,
                                          const std::vector<std::string> &old_switch_lines) {
    if (key == "melee_stamina_pct" && StaminaSwitchOn(old_switch_lines))
        return {"melee_stamina"};
    return {};
}
bool SearchFinds(const std::string &needle, const std::string &text) {
    // A word counts where a word of the text starts with it: "turn" finds turning, not return.
    const auto starts_word = [&](const std::string &word) {
        for (auto at = text.find(word); at != std::string::npos; at = text.find(word, at + 1))
            if (at == 0 || !std::isalnum(static_cast<unsigned char>(text[at - 1])))
                return true;
        return false;
    };
    std::istringstream words(needle);
    std::string word;
    bool any = false;
    while (words >> word) {
        if (!starts_word(word))
            return false;
        any = true;
    }
    return any;
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
std::optional<std::string> ResetValue(const std::string &key) {
    if (key == "weighted_weapons")
        return "1";
    return std::nullopt;
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
namespace {
// What the launcher adds to an in-game row (vr_page.h Rows): its tooltip, from what the game code
// does; more words the search finds it by; a number row's shown code default; and the launcher's
// own text for a number's 0 (nullptr: the page's).
struct RowNote {
    const char *id;
    const char *details;
    const char *words = "";
    int fallback = 0;
    const char *zero = nullptr;
};
const RowNote Notes[]{
    // You
    {"height",
     "As the hunter: standing straight, your eyes are at the hunter's eye height. As the player: "
     "your real height, measured from the headset's floor. Seated play always uses the hunter's "
     "height.",
     "tall short eye level standing floor"},
    {"height_adjust", "Moves your eyes up or down from the height choice, in centimetres.",
     "taller shorter floor offset cm", 0},
    {"ipd",
     "Headset: your headset's own eye distance. A smaller value makes the world look larger, a "
     "larger one smaller.",
     "ipd eye distance world scale size big small", 0},
    {"body_mode",
     "Full: everything but the head. The hunter's shadow always shows the whole body.",
     "arms hands full body visible avatar"},
    {"cutaway",
     "Full body only: parts of the body closer than about 20 cm to your eyes fade out, so you can "
     "look down past the collar and chest.",
     "collar chest coat fade clipping look down"},
    {"body_follow",
     "How the hunter's body follows your head. Lean keeps the spine's length; Legacy shift is the "
     "older behaviour.",
     "posture lean spine body follow"},
    {"camera_distance",
     "Third person: how far the camera sits behind the hunter, from 50% to 100% of the game's "
     "distance. Closer only, so the camera never goes through walls. With a gamepad: hold the "
     "left stick click and push the right stick up (closer) or down.",
     "third person camera distance zoom closer orbit", 100},
    {"camera_tilt",
     "Third person: how much the view tilts down as a gamepad's right stick raises the camera. "
     "Off keeps the view level, which is easiest on the stomach; Full looks down at the hunter "
     "like the flat game.",
     "third person camera tilt pitch gamepad right stick orbit"},
    {"ladder_skip",
     "On (the default): on a long ladder, where the climb would switch to the flat view, the "
     "screen goes dark while the hunter climbs on by himself, faster, and you're back in VR near "
     "the top or bottom. A hit or a fall gives you control back at once. Off: the flat climb. "
     "Only with Scenes on a screen on (otherwise ladders stay in first person).",
     "ladder climb skip fade dark black stairs"},
    {"lean_side", "The furthest the hunter leans sideways before he takes a step.",
     "lean step sideways posture body", 20},
    {"lean_response", "How long the body takes to follow your posture, in milliseconds.",
     "smoothing posture delay latency body", 50},
    {"body_comfort",
     "On: extra protection against the body clipping into your view on sideways leans, and a "
     "staged return of the body after a roll or hit.",
     "clipping comfort roll hit body"},
    // Controls
    {"move_with",
     "Right stick: the right stick walks and the left stick turns. The stick clicks and face "
     "buttons change hands too.",
     "left handed swap sticks locomotion walk thumbstick"},
    {"turning",
     "Snap turns jump by the chosen angle. Smooth turning rotates at 90 (slow), 120, 180 (fast) "
     "or 240 (very fast) degrees per second.",
     "snap smooth turn rotate rotation comfort degrees"},
    {"controls", "Motion: swing the controller to attack. Buttons: attack with buttons only.",
     "motion buttons classic gestures swing attack scheme"},
    {"stance",
     "Seated: smaller strong swings and throws, your lap counts as a place for Blood Vials, and "
     "walking around your room does not move the hunter.",
     "seated sitting standing chair"},
    {"vibration", "Scales every controller vibration. 7 is the game's normal strength.",
     "haptics rumble vibration strength", 7},
    {"inject",
     "Stab: take a vial from the pouch at your hip or over your right shoulder (or with X button: "
     "Vial, press X), then touch it to your wrist or body to inject it. Button: X uses a vial, as "
     "in the original game.",
     "blood vial heal healing stab wrist"},
    {"x_button",
     "Fan menu: X opens and closes the quick-item fan, the Blood Vial comes from the pouch at your "
     "hip or over your right shoulder. Vial: X readies a vial to stab, as before; the pouch and "
     "the shoulder still work. With Blood Vial: Button, X always uses a vial.",
     "x button fan menu quick items vial heal"},
    {"laser", "A thin line along the gun barrel while you raise your gun hand to aim.",
     "gun aim sight pointer"},
    {"grip_adjust",
     "On: hold the grip-adjust buttons 1.2 s standing still to change how the weapon sits in your "
     "hand.",
     "weapon hold angle grip offset"},
    // Combat
    {"weighted_weapons",
     "On (Regular, the default): weight, gravity and air resistance; the weapon's shape and one- "
     "or two-handed support affect handling. On (Light): the same, softened. Off: direct "
     "handling. Whip forms are excluded.",
     "weight momentum heavy inertia physics floaty light regular"},
    {"weapon_wall",
     "On (the default): weapons stop at walls and at each other. Weapons only: they only stop at "
     "each other.",
     "collision walls clash solid clip"},
    {"blade_sounds",
     "On: weapons ring when they touch, and drawing one along the other makes a sharpening sound.",
     "sharpen ring clink sound audio"},
    {"riposte_style",
     "Visceral attacks briefly turn the scene black, red and white with ink outlines. Beast Roar "
     "briefly turns the scene deep crimson.",
     "visceral beast roar flash effect"},
    {"ragdoll",
     "Off: an enemy your blade kills plays its death animation. Limp: it goes limp at once. The "
     "pushes throw the body as hard as your swing, from light to strongest (Normal is the game "
     "default).",
     "ragdoll physics death corpse body push throw"},
    {"two_handed",
     "Original (the default): the right grip transforms and B / Y slide your hands. Optimized: "
     "tap the right stick to transform instead of lock-on; the left trigger near a transformed "
     "handle attaches your left hand (Hold: while squeezed, Toggle: press to grab, press again to "
     "let go); hold either grip to slide that hand. Motion controls only.",
     "two hand two-handed grip trigger attach transform optimized"},
    {"two_hand_guide",
     "Shows where your left hand can grab the handle. In Original motion controls, hold the "
     "right grip to reveal it. In Optimized, the guide appears when a two-handed handle is available.",
     "two hand left hand guide marker"},
    {"gun_wrist_reload",
     "After a shot, release the trigger. Snap the muzzle up and lower it slightly, or flick it "
     "down more sharply and let it rebound. Choose Off/Original to fire again without a gesture.",
     "reload gun flick wrist pistol"},
    {"vr_grab_attacks",
     "On: enemy grab attacks stay in VR, with the view following the hunter. Off (the default): "
     "the existing behaviour.",
     "grab enemy grab camera cutscene"},
    {"lockon_follow",
     "On: in first person, locked on, the view smoothly keeps your target in the middle (a big "
     "jump, like rolling past it, is one quick snap), and the left stick moves you round it as "
     "the flat game's lock-on does. Your own head never turns the view. Look away from the target "
     "for a moment to release the lock. Off (the default): the view stays yours.",
     "lock on lock-on target targeting follow camera turn"},
    {"two_hand_swing_hum", "Vibration strength during fast two-handed swings.",
     "vibration haptics two hand swing", 100},
    // Items & world
    {"holster_mode",
     "Grip + Body: grip opens the menu and body slots are available. No Grip: disables only "
     "grip-to-open-menu, including on SteamVR; grabbing and holding still work. No Body: "
     "disables body slots. Menu button shortcuts remain available.",
     "inventory pouch belt holster quick items slots"},
    {"quick_arc_pick",
     "Fan A and Fan B: the items you carry on a fan round your left hand (Fan A gothic panes of "
     "candle-lit glass, Fan B an engraved brass rim); move the hand a little toward one to light "
     "it, and pull the hand away to close the menu. Boxes: the arc of cards above your left hand; "
     "put your fist on an item.",
     "quick items menu fan boxes inventory"},
    {"shoulder_vial",
     "On: reach over your right shoulder with the left hand and squeeze the grip to draw a Blood "
     "Vial, as from the pouch. Waits while the right grip is held (a two-handed grip). Needs Item "
     "access: No Grip or Grip + Body.",
     "blood vial shoulder heal"},
    {"shoulder_switch",
     "On: reach over a shoulder and press B (right) or Y (left) to switch that hand's weapon; hold "
     "for about 0.4 seconds to go unarmed.",
     "weapon switch shoulder unarmed swap"},
    {"upscale_focus",
     "Quality, Medium (the default), Performance: DLSS or FSR reconstructs only the middle of "
     "each eye (90%, 85%, 75% of its width and height), around the lens centre; the rest is the "
     "game's image enlarged and smoothed by a TAA, where the lenses blur anyway. Performance is "
     "the fastest. Off: DLSS or FSR reconstructs the whole of each eye. Requires native stereo "
     "and regular DLSS or FSR (DLSS 5 keeps its own neural centre). Perf Scan uses Medium "
     "for supported configurations. The in-game setting applies while playing.",
     "foveated upscale dlss fsr taa performance middle centre lens"},
    {"vr_interactions",
     "Immersive (the default): in first person, doors, gates, chests and coffins open in front of "
     "you without the hunter's animation, a lamp lights at once, and a knock on a resident's door "
     "is answered without the knocking animation, while you keep moving. In third person the "
     "hunter knocks in VR. Flat: the hunter's own animations, shown on the virtual screen. "
     "Resting and travelling at a lamp stay on the screen either way. Lamps and knocks change at "
     "the next start.",
     "door gate chest coffin lamp light open knock resident immersive flat animation interaction"},
    {"physical_levers",
     "On: grip a lever with either hand and pull it. Off: the game's original interaction.",
     "lever pull elevator gate"},
    {"lamp_travel",
     "On: rest, refill and travel between lit lamps from the lamp itself. Offline only, and "
     "experimental.",
     "lamp lantern rest travel teleport fast travel"},
    {"physical_summon_bell",
     "On (the default): in first person, B draws the summon bell into your hand; shake it to "
     "summon. B again cancels. Off: the game's own bell.",
     "bell summon beckoning help helper co-op"},
    {"doll_respec",
     "On (the default): the Doll's list offers Rechannel Blood Echoes: her Level Up on your own "
     "build, using the lowest starting value of each stat across all origins "
     "(7/8/9/9/5/6). Redistribute for free at the same level; your origin stays unchanged. "
     "Leave to cancel. Normal levelling uses Channel Blood Echoes.",
     "respec rebirth reallocate stats attributes doll level up"},
    {"holster_haptics", "On: a small vibration when your hand reaches a pouch.",
     "vibration haptics pouch"},
    // HUD & feedback
    {"hud_vitals",
     "Wrist gauge: a Blood Gauge on the left wrist shows health, stamina and bullets; the game's "
     "own health display appears when you look down. Classic: the game's HUD.",
     "health stamina bar wrist gauge hud bullets"},
    {"low_hp",
     "On: a heartbeat in both controllers and darkened edges of your view while your health is "
     "low.",
     "heartbeat low health warning"},
    {"counters",
     "Look down: the counters fade in when you look well below the horizon. Menus and dialogs "
     "always show them.",
     "blood echoes insight currency"},
    {"hit_flash", "On: the edges of your view flash red when you take a hit.",
     "damage red flash vignette"},
    {"hud_size",
     "The size of the health bars, item icons and prompts. Medium is the game default.",
     "ui scale size hud"},
    {"low_hp_edges",
     "A red edge around your view below 30% health, in three strengths (Medium is the game "
     "default). It takes priority over the exhaustion color.",
     "red vignette low health color edge"},
    {"exhaustion_color",
     "Colors soften briefly when your stamina runs out, then return, in three strengths (Medium "
     "is the game default).",
     "stamina desaturation grey gray color"},
    {"health_bars",
     "Bosses only (the default): the boss's bar near the top. All: also the bars and damage "
     "numbers over enemies you hit or lock on to. The lock-on dot and your own HUD always show.",
     "boss bar enemy hp bar damage numbers"},
    // The strengths in steps of 10, as the launcher had them before Low-health edges and
    // Exhaustion color (their three steps) replaced the rows.
    {"health_color_strength",
     "The red edge's exact strength. Low-health edges picks 30, 50 (the game default) or 80%; any "
     "other strength shows there as Custom.",
     "red vignette low health color edge strength intensity", 50},
    {"stamina_color_strength",
     "The exhaustion effect's exact strength. Exhaustion color picks 40, 70 (the game default) or "
     "100%; any other strength shows there as Custom.",
     "stamina desaturation grey gray color strength intensity", 70},
    // Picture
    {"aa",
     "SMAA: dual-eye SMAA replaces the game's FXAA (the default). Game FXAA: the game's own. "
     "None: no extra edge smoothing. Off while DLSS or FSR is on.",
     "anti aliasing jaggies smaa fxaa edges"},
    {"lod",
     "Performance (the default): the game's own simpler models far away, the best frame rate. "
     "Balanced: a little more detail at a distance. Quality: every object's full model at any "
     "distance, the slowest.",
     "lod draw distance detail pop-in fps"},
    {"color_contrast", "Separation between light and dark areas.", "brightness picture color", 0},
    {"color_saturation", "Colour intensity. -100 % is black and white.",
     "black white monochrome picture color", 0},
    {"color_gamma", "Brightness of the midtones; higher is brighter.",
     "brightness dark picture color", 0},
    // Difficulty
    {"hp_bonus",
     "Raises the hunter's maximum health (+10 % by default). Your save file is not changed.",
     "easy health hp difficulty"},
    {"empty_vial_refill",
     "When you die or rest at a lamp with no Blood Vials on you and none in storage, the game "
     "gives you this many (3 by default), so you never have to farm vials to keep going. If you "
     "have any left, on you or stored, you get none. Offline only.",
     "blood vials free refill death respawn heal empty farm difficulty",
     VR::Lamps::DefaultVialsWhenOut},
    {"iframes",
     "Extra invincibility after a quickstep or roll, in 1/30 s frames (off by default). "
     "Backsteps get none.",
     "dodge roll quickstep invincibility frames iframes"},
    {"attack_stamina",
     "Light swings cost this share of the game's stamina (80% by default). Strong swings always "
     "cost the full amount; with too little stamina a strong swing becomes a light one. Dodges, "
     "sprinting and guns are unchanged.",
     "stamina cost attack swing"},
    {"outgoing_damage",
     "Your attacks' damage to enemies, from -60% to +30% in steps of 5. 0% is the game's normal "
     "damage.",
     "damage harder easier difficulty attack", 0},
    // Picture > Performance (foveation), Graphics > Performance (frame overlap)
    {"foveation",
     "Less detail toward the lens edges, which the lenses blur anyway, for more frames. Auto (the "
     "default): on where the graphics card supports it (NVIDIA RTX 20, AMD RX 6000, Intel Arc and "
     "newer); a new player with a fast card starts on Light. Light: a wider sharp centre and never "
     "the coarsest steps. Performance: coarser "
     "still. In the game it changes at once (the VR page's Picture > Performance).",
     "foveated ffr fixed foveated performance fps lens edges light"},
    {"frame_overlap",
     "On (the default): the game prepares its next frame while the graphics card finishes this "
     "one, which removes the look-around hitches in busy areas such as the sewers and Old "
     "Yharnam. Off: each frame waits until the graphics card is done; try it if the headset shows "
     "doubled or stuttering images while the graphics card runs near its limit.",
     "stutter hitch fps smooth performance"},
    {"canted_eyes",
     "For headsets whose screens are angled outward (Pimax); the game finds them itself. "
     "On (the default): the game turns each eye to its screen itself, which looks right whatever "
     "the headset's software does. Off: each eye is drawn facing straight ahead over its whole "
     "view, and the headset's software turns it to the screen. It costs a little GPU time, and "
     "only on such a headset: headsets with straight screens are never affected.",
     "pimax canted angled screens cross eyed parallel projections double vision"},
    {"shadows_far",
     "Reduced (the default): the farthest band of shadows is left out, which gives more FPS in big "
     "open areas with little visible change. Full: every shadow at every distance, as the game "
     "draws them. A new player whose graphics card has 8 GB or less is set to Reduced.",
     "shadow shadows distance performance fps"},
    {"reflections",
     "Off: no mirrored scene. Water: water mirrors the scene around it, as the game draws it (a "
     "second drawing of the scene, which costs FPS where there is water, such as Old Yharnam and "
     "the sewers; applies at once). Wet streets and water: puddles, wet stones and polished floors "
     "mirror what is round them too (the game's own screen-space reflections, which it never turns "
     "on, found by LanternEngine, traced by the mod's own code that looks the same in both eyes; "
     "it costs FPS everywhere; applies at the next start). Not set: by your graphics card's "
     "memory (water from 12 GB, wet streets from 13.8 GiB).",
     "water reflection reflections mirror ssr wet puddles wetness performance fps"},
    {"shadows_busy",
     "Off (the default): only Far shadows decides. On: while a view is very busy, the "
     "next-farthest shadows are left out too, and they come back once the frame rate recovers. "
     "Helps a graphics card that is close to its limit.",
     "shadow shadows busy performance fps"},
    // New graphics (2026-11-10): the lighting the mod adds; with DLSS or FSR or without (since
    // 2026-11-19, the user: "we need to make these work with upscaling off as well").
    {"new_shadows",
     "Game's: only the game's own shadows. Low, Medium and High: the lights near you (your "
     "lantern, a torch, a gun's flash, the lamps of the street) cast real shadows, and the lamps "
     "that matter most get sharp, detailed shadow edges near you. Low: 2 lamps sharp up to about "
     "2 m, and lighter shadows from a torch in your hand; the cheapest. Medium: 3 lamps sharp up "
     "to about 4 m. High: 5 lamps sharp up to about 5 m. A torch or lantern in your hand costs "
     "the most, since it lights nearly everything you see. In the game's VR menu it applies at "
     "once (the first time in a session it takes a moment to show). Not set: by your graphics "
     "card's memory (Low on a 12 GB card, Medium on 16 GB and up).",
     "shadow shadows lantern torch lamp lamps detail sharp high low dynamic lighting performance fps"},
    {"new_fog",
     "On: the fog banks that linger over the ground, and the ground mist of the Forbidden Woods, "
     "are drawn by the mod's own fog: it stays where it is in the world, lamps and the moon light "
     "it, and it has its own color. Game's: the game's fog as it draws it. The fog needs the "
     "moon's glow, so Moon glow is on while Fog is on. In the game's VR menu it changes when you "
     "travel to another area (the area you are in keeps its fog until then; a death does not "
     "change it). Not set: by your graphics card's memory (on from 12 GB).",
     "fog mist banks volumetric volumetrics ground haze performance fps"},
    {"moon_glow",
     "On: moonlight in the air: a soft glow towards the moon, haze, and rays through trees and "
     "windows. It is always on while Fog is on. In the game's VR menu it applies at once. "
     "Not set: by your graphics card's memory (on from 12 GB).",
     "moon glow haze god rays godrays shafts beams moonlight air performance fps"},
    {"new_ambient",
     "AO: deeper, more grounded shading in corners, under ledges and where things meet, in place "
     "of the game's; with it blood looks wet and fine detail shimmers less. AO and bounce: also "
     "light reflected off lit walls and floors onto what is near them, the most expensive of the "
     "new graphics. In the game's VR menu it applies at once (the first time in a session it "
     "takes a moment to show). Not set: by your graphics card's memory (AO from 10 GB, bounce "
     "from 13.8 GiB).",
     "ambient occlusion ao gtao bounce gi global illumination lighting"},
    {"raised_bricks",
     "On: brick walls, big stone blocks and roof tiles get real depth. Their joints sink and "
     "darken, and each eye sees them from its own side, so they look built rather than painted. "
     "Every other surface is drawn exactly as the game draws it. In the game's VR menu it applies "
     "at once.",
     "parallax bricks brick stones walls tiles roof depth relief bump raised joints"},
    {"cinematic_look",
     "A film-like color curve on the game's final image: deeper blacks and calmer shadows. 0 is "
     "the game's own look. Applies at once.",
     "cinematic grade color contrast film look blacks",
     30}, // (the game's default: vr_session.h cinematic_look; unset showed "0% (default)" until 2026-11-17)
    {"eye_adaptation",
     "On: stepping from bright light into a dark place, the view starts darker and opens up "
     "over a couple of seconds as your eyes adjust; stepping back out, it starts too bright and "
     "settles. Once adjusted, it is the game's own brightness. Costs nothing measurable. "
     "Applies at once.",
     "eye adaptation auto exposure adaptive eyes brightness dark bright hdr"},
};
const RowNote *NoteOf(std::string_view id) {
    for (const auto &note : Notes)
        if (id == note.id)
            return &note;
    return nullptr;
}
// The page's rows its tabs leave out (vr.ini tuning since 2026-10-30), at the end of their tab.
constexpr std::pair<const char *, const char *> LauncherOnly[]{
    {"You", "lean_side"},
    {"You", "lean_response"},
    {"You", "body_comfort"},
    {"Combat", "two_hand_swing_hum"},
    {"Items & world", "holster_haptics"},
    {"HUD & feedback", "health_color_strength"},
    {"HUD & feedback", "stamina_color_strength"},
};
// A game row as a launcher row: its keys, choices (an empty value leaves that key, Keep) or
// number range, and the launcher's note.
Setting FromPage(const VR::Page::Row &row, const char *category, const char *group) {
    Setting s{category, row.label, row.help};
    for (const char *key : row.keys)
        if (key)
            s.keys.emplace_back(key);
    if (row.widget == VR::Page::Widget::Choice) {
        for (const auto &o : row.options) {
            Option option{o.text, {}, o.ini_only};
            for (size_t j = 0; j < s.keys.size(); ++j)
                option.values.emplace_back(o.values[j] ? o.values[j] : Keep);
            s.options.push_back(std::move(option));
        }
    } else {
        s.min = row.min;
        s.max = row.max;
        s.step = row.step;
        s.unit = row.unit;
        if (row.widget == VR::Page::Widget::Slider) {
            // A float 1/7 or 1/100 as the exact fraction (0.5 is 3.5 sevenths, shown as 4).
            const double inverse = 1.0 / row.ini_scale;
            s.scale = std::abs(inverse - std::round(inverse)) < 1e-3 ? 1.0 / std::round(inverse)
                                                                     : row.ini_scale;
        } else {
            s.sign = row.signed_value;
            s.zero = row.zero_text;
        }
    }
    s.id = row.id;
    s.group = group;
    if (const auto *note = NoteOf(row.id)) {
        s.details = note->details;
        s.words = note->words;
        s.fallback = note->fallback;
        if (note->zero)
            s.zero = note->zero;
    }
    return s;
}
} // namespace
// Left-handed (2026-11-22: "it needs to be a setting in Launcher", "only for first
// person + motion controls"): the world mirrored, so the trick weapon is in your left hand.
static Setting LeftHandedRow() {
    Setting row = Choice(
        "Controls", "Left-handed", "The weapon in your left hand, the gun in your right.",
        {"left_handed"}, {{"Off", {"0"}}, {"On", {"1"}}},
        "On: the whole game is mirrored, so you hold the trick weapon in your left hand and the "
        "gun in your right. The controllers trade jobs (your left trigger and grip are the "
        "weapon's, your right stick walks), and the world, the hunter and the sound are mirrored "
        "to match. Text and menus stay readable. Only with motion controls in first person; a "
        "gamepad or third person stays right-handed. Applies at the next start. Off (the "
        "default): right-handed, as the game was made.");
    row.group = LauncherOnlyGroup;
    row.words = "left handed lefty mirror mirrored swap hands weapon hand off hand";
    return row;
}
// Left-handed play: the tooltips that name your hands, for the other side. Those with bare button
// letters have their own words ("A" is also a word); the rest trade left and right (SideWords).
std::string LeftHandedDetails(const Setting &row) {
    if (row.id == nullptr || row.details == nullptr) {
        return {};
    }
    const std::string_view id = row.id;
    if (id == "inject") {
        return "Stab: take a vial from the pouch at your hip or over your left shoulder (or with A "
               "button: Vial, press A), then touch it to your wrist or body to inject it. Button: A "
               "uses a vial, as in the original game.";
    }
    if (id == "x_button") {
        return "Fan menu: A opens and closes the quick-item fan, the Blood Vial comes from the pouch "
               "at your hip or over your left shoulder. Vial: A readies a vial to stab, as before; "
               "the pouch and the shoulder still work. With Blood Vial: Button, A always uses a "
               "vial.";
    }
    if (id == "two_handed") {
        return "Original (the default): the left grip transforms and Y / B slide your hands. "
               "Optimized: tap the left stick to transform instead of lock-on; the right trigger "
               "near a transformed handle attaches your right hand (Hold: while squeezed, Toggle: "
               "press to grab, press again to let go); hold either grip to slide that hand. Motion "
               "controls only.";
    }
    if (id == "shoulder_switch") {
        return "On: reach over a shoulder and press Y (left) or B (right) to switch that hand's "
               "weapon; hold for about 0.4 seconds to go unarmed.";
    }
    if (id == "physical_summon_bell") {
        return "On (the default): in first person, Y draws the summon bell into your hand; shake it "
               "to summon. Y again cancels. Off: the game's own bell.";
    }
    if (id == "move_with" || id == "two_hand_guide" || id == "quick_arc_pick" ||
        id == "shoulder_vial" || id == "hud_vitals") {
        return VR::Mirror::SideWords(row.details);
    }
    return {};
}

// Built from the in-game VR page (vr_page.h Tabs and Rows). Not offered: Smooth motion
// (late_warp_motion), which needs late_warp=1 while the headset preset runs late_warp=0, and the
// Upscaling tab's rows (the Graphics tab draws graphics_config itself).
const std::vector<Setting> &Settings() {
    static const std::vector<Setting> all = [] {
        std::vector<Setting> out;
        const auto &rows = VR::Page::Rows();
        const auto add = [&](const char *id, const char *category, const char *group) {
            if (const int at = VR::Page::RowIndex(id); at >= 0)
                out.push_back(FromPage(rows[size_t(at)], category, group));
        };
        for (const auto &tab : VR::Page::Tabs()) {
            if (GameOnlyTab(tab.name))
                continue;
            // A tab with pages: each page's name is the heading of its rows.
            for (int page = 0; page < tab.Pages(); ++page)
                for (const int r : tab.RowsOf(page))
                    if (r >= 0 && std::string_view(rows[size_t(r)].id) != "smooth_motion")
                        out.push_back(FromPage(rows[size_t(r)], tab.name,
                                               tab.Pages() > 1 ? tab.PageName(page) : nullptr));
            for (const auto &[in, id] : LauncherOnly)
                if (std::string_view(in) == tab.name)
                    add(id, tab.name, LauncherOnlyGroup);
            // Left-handed play (core/vr/mirror_world.h): launcher only, read at the start (the
            // controllers' bindings are made then); the page has no row for it.
            if (std::string_view(tab.name) == "Controls")
                out.push_back(LeftHandedRow());
        }
        // The performance choice that needs a restart; the live ones (foveated rendering among
        // them since 2026-11-01) are on the page's Picture tab (its Performance page).
        add("frame_overlap", "Graphics", "Performance");
        auto dynamic_resolution = Choice(
            "Graphics", "Dynamic Resolution", "Lower scene resolution when the GPU is under sustained load.",
            {"dynamic_resolution"}, {{"On", {"1"}}, {"Off", {"0"}}},
            "On (default) lets both eyes drop up to 30% per dimension and return to full resolution "
            "when GPU time allows. The shorter side stays at or above 1,160 pixels. Smaller inputs "
            "keep their chosen size. Applies next launch in native stereo with the upscaler off or "
            "with regular DLSS, Foveated upscale included.");
        dynamic_resolution.group = "Performance";
        dynamic_resolution.words = "adaptive resolution scale gpu frame time fps performance";
        out.push_back(std::move(dynamic_resolution));
        add("upscale_focus", "Graphics", "Upscaling"); // the in-game Upscaling tab's own row
        add("canted_eyes", "Graphics", "Headset");
        // Launcher only: the page has no row for them.
        auto camera = Choice("Extras", "Camera", "Choose the initial perspective.",
                             {"first_person"}, {{"First person", {"1"}}, {"Third person", {"0"}}},
                             "Third person turns the VR body off. Launcher only: the in-game VR "
                             "page has no row for it.");
        camera.words = "third person first person view perspective";
        out.push_back(std::move(camera));
        auto scenes =
            Choice("Extras", "Scenes on a screen",
                   "Show grabs, viscerals, doors and ladders on a virtual screen.", {"cinematic"},
                   {{"On", {"1"}}, {"Off", {"0"}}},
                   "On: viscerals, grabs, levers and ladders play from the game's own camera on "
                   "a virtual screen, and so do doors, chests and lamps with VR interactions on "
                   "Flat. Off: they stay in first person. Resting and travelling at a lamp stay "
                   "on the screen either way. Launcher only: the in-game VR page has no row for "
                   "it.");
        scenes.words = "cinematic cutscene screen visceral door ladder lamp";
        out.push_back(std::move(scenes));
        return out;
    }();
    return all;
}
} // namespace Launcher
