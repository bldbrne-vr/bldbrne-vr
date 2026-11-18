// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// "Power management mode" of this executable in the NVIDIA driver's profiles, after bbport's
// platform/win32/nvidia_profile.cpp (0.7). With the driver's default (optimal power) the GPU picks
// its clocks from how busy it is; we are CPU-bound and the GPU idles between pieces of a frame, so
// NVIDIA cards can sit at low clocks with every piece taking longer (bbport: half the frame rate in
// heavy places on an RTX 3080 and 5060). The cure is "Prefer maximum performance" in the NVIDIA
// Control Panel, set here for this executable only through NVAPI's driver settings (the interface
// ids and structures of NVIDIA's public nvapi.h), unless the player chose a mode for it.
// The driver reads the profile when it loads: this runs before the Vulkan instance.
// The setting follows the executable's name (shadps4.exe): other copies of it use it too.

#include "video_core/renderer_vulkan/vr_nvidia_profile.h"

#ifdef _WIN32

#include <windows.h>
#include <shlobj.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string_view>
#include <type_traits>
#include <vector>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/vr/vr_session.h"

namespace Vulkan {
namespace {

using NvStatus = int;
using Session = void*;
using Profile = void*;
constexpr NvStatus Ok = 0, SettingNotFound = -160, ProfileNotFound = -163,
                   ExecutableNotFound = -166;
constexpr std::uint32_t PreferredPstateId = 0x1057EB71; // PREFERRED_PSTATE_ID
constexpr std::uint32_t PreferMax = 1;                  // PREFERRED_PSTATE_PREFER_MAX
constexpr std::uint32_t OptimalPower = 5;               // PREFERRED_PSTATE_DEFAULT
constexpr int StringMax = 2048;
using UnicodeString = std::uint16_t[StringMax];
constexpr const wchar_t* OwnProfile = L"Bldbrne-VR";

#pragma pack(push, 4)
struct Setting { // NVDRS_SETTING_V1
    std::uint32_t version;
    UnicodeString name;
    std::uint32_t id;
    std::uint32_t type;     // 0: DWORD
    std::uint32_t location; // 0: this profile, 1: global, 2: base, 3: default
    std::uint32_t is_current_predefined;
    std::uint32_t is_predefined_valid;
    union {
        std::uint32_t u32;
        std::uint8_t binary[4 + 4096];
        UnicodeString wide;
    } predefined;
    union {
        std::uint32_t u32;
        std::uint8_t binary[4 + 4096];
        UnicodeString wide;
    } current;
};
#pragma pack(pop)

struct Application { // NVDRS_APPLICATION_V4
    std::uint32_t version;
    std::uint32_t is_predefined;
    UnicodeString app_name;
    UnicodeString friendly_name;
    UnicodeString launcher;
    UnicodeString file_in_folder;
    std::uint32_t flags; // isMetro:1, isCommandLine:1
    UnicodeString command_line;
};

struct ProfileInfo { // NVDRS_PROFILE_V1
    std::uint32_t version;
    UnicodeString name;
    std::uint32_t gpu_support;
    std::uint32_t is_predefined;
    std::uint32_t num_apps;
    std::uint32_t num_settings;
};

template <typename T>
constexpr std::uint32_t Version(std::uint32_t v) {
    return std::uint32_t(sizeof(T)) | (v << 16);
}

void Copy(std::uint16_t* dst, const wchar_t* src) {
    std::size_t i = 0;
    for (; src[i] && i + 1 < StringMax; ++i) {
        dst[i] = std::uint16_t(src[i]);
    }
    dst[i] = 0;
}

struct Api {
    using Query = void*(__cdecl*)(std::uint32_t);
    NvStatus(__cdecl* initialize)();
    NvStatus(__cdecl* create_session)(Session*);
    NvStatus(__cdecl* destroy_session)(Session);
    NvStatus(__cdecl* load_settings)(Session);
    NvStatus(__cdecl* save_settings)(Session);
    NvStatus(__cdecl* find_application)(Session, UnicodeString, Profile*, Application*);
    NvStatus(__cdecl* find_profile)(Session, UnicodeString, Profile*);
    NvStatus(__cdecl* create_profile)(Session, ProfileInfo*, Profile*);
    NvStatus(__cdecl* create_application)(Session, Profile, Application*);
    NvStatus(__cdecl* get_setting)(Session, Profile, std::uint32_t, Setting*);
    NvStatus(__cdecl* set_setting)(Session, Profile, Setting*);
    NvStatus(__cdecl* delete_setting)(Session, Profile, std::uint32_t);

    bool Load() {
        HMODULE module = LoadLibraryA("nvapi64.dll");
        if (!module) {
            return false;
        }
        const auto query = reinterpret_cast<Query>(GetProcAddress(module, "nvapi_QueryInterface"));
        if (!query) {
            return false;
        }
        const auto get = [&](auto& fn, std::uint32_t id) {
            fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(query(id));
            return fn != nullptr;
        };
        return get(initialize, 0x0150E828) && get(create_session, 0x0694D52E) &&
               get(destroy_session, 0xDAD9CFF8) && get(load_settings, 0x375DBD6B) &&
               get(save_settings, 0xFCBC7E14) && get(find_application, 0xEEE566B2) &&
               get(find_profile, 0x7E4A9A0B) && get(create_profile, 0xCC176068) &&
               get(create_application, 0x4347A9DE) && get(get_setting, 0x73BF8338) &&
               get(set_setting, 0x577DD202) && get(delete_setting, 0xE4A26362);
    }
};

enum class Mode { Off, On, Check };

Mode RequestedMode() {
    if (const char* env = std::getenv("VR_NV_POWER"); env && *env) {
        const std::string_view v{env};
        if (v == "check") {
            return Mode::Check;
        }
        return v == "0" || v == "off" ? Mode::Off : Mode::On;
    }
    return VR::GetSettings().nvidia_max_power ? Mode::On : Mode::Off;
}

// Written when this set the mode, so nvidia_max_power=0 takes back only what this put there. The
// driver setting follows the executable's name on the whole machine, so the marker is in the
// player's one data folder (%LocalAppData%\Bldbrne-VRData, the launcher's), not in this copy's
// user folder: any copy takes back what another copy set.
std::filesystem::path MarkerPath() {
    wchar_t local[MAX_PATH];
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, local))) {
        return std::filesystem::path{local} / L"Bldbrne-VRData" / L"nvidia_power_profile.txt";
    }
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "nvidia_power_profile.txt";
}
// Where the marker was first kept (test builds of 2026-10-29): read and removed, never written.
std::filesystem::path OldMarkerPath() {
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "nvidia_power_profile.txt";
}

void ApplyOnce() {
    const Mode mode = RequestedMode();
    const bool dry = mode == Mode::Check;
    const auto marker = MarkerPath();
    const auto old_marker = OldMarkerPath();
    std::error_code ec;
    const bool marked =
        std::filesystem::exists(marker, ec) || std::filesystem::exists(old_marker, ec);
    const auto unmark = [&] {
        std::filesystem::remove(marker, ec);
        std::filesystem::remove(old_marker, ec);
    };
    if (mode == Mode::Off && !marked) {
        return; // nothing of ours to take back
    }
    Api api{};
    if (!api.Load() || api.initialize() != Ok) {
        return; // no NVIDIA driver
    }
    Session session = nullptr;
    if (api.create_session(&session) != Ok) {
        return;
    }
    const auto done = [&](const char* problem, NvStatus status) {
        if (problem) {
            LOG_WARNING(Render_Vulkan, "NVIDIA profile: {} (NVAPI status {})", problem, status);
        }
        api.destroy_session(session);
    };
    NvStatus status = api.load_settings(session);
    if (status != Ok) {
        return done("could not read the driver settings", status);
    }
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return done("no executable name", 0);
    }
    const wchar_t* slash = std::wcsrchr(path, L'\\');
    const wchar_t* exe = slash ? slash + 1 : path;

    // The structures are large (tens of KiB): on the heap.
    auto app = std::make_unique<Application>();
    std::vector<std::uint16_t> name(StringMax);
    Profile profile = nullptr;
    app->version = Version<Application>(4);
    Copy(name.data(), exe);
    status = api.find_application(session, name.data(), &profile, app.get());
    const bool listed = status == Ok;
    if (!listed && status != ExecutableNotFound) {
        return done("could not look up the executable's profile", status);
    }

    auto setting = std::make_unique<Setting>();
    if (listed) {
        setting->version = Version<Setting>(1);
        status = api.get_setting(session, profile, PreferredPstateId, setting.get());
        if (status != Ok && status != SettingNotFound) {
            return done("could not read the power management mode", status);
        }
    } else {
        status = SettingNotFound;
    }
    const bool found = status == Ok;
    const std::uint32_t current = found ? setting->current.u32 : OptimalPower;
    // In the executable's own profile, not inherited from the global or base profile.
    const bool own_value = found && setting->location == 0 && !setting->is_current_predefined;

    if (mode == Mode::Off) {
        if (own_value && current == PreferMax) {
            status = api.delete_setting(session, profile, PreferredPstateId);
            if (status == Ok) {
                status = api.save_settings(session);
            }
            if (status != Ok) {
                return done("could not take back prefer maximum performance", status);
            }
            LOG_INFO(Render_Vulkan, "NVIDIA profile: power management back to the driver's "
                                    "default (nvidia_max_power=0)");
        }
        unmark();
        return done(nullptr, 0);
    }

    if (current == PreferMax) {
        LOG_INFO(Render_Vulkan, "NVIDIA profile: power management already prefers maximum "
                                "performance");
        return done(nullptr, 0);
    }
    if (own_value) {
        LOG_INFO(Render_Vulkan, "NVIDIA profile: power management mode {} chosen by the player, "
                                "kept",
                 current);
        if (marked && !dry) {
            unmark(); // the player's now, not ours to take back
        }
        return done(nullptr, 0);
    }
    if (dry) {
        LOG_INFO(Render_Vulkan, "NVIDIA profile (check): would set prefer maximum performance "
                                "(mode {} now; executable {} a profile)",
                 current, listed ? "in" : "not yet in");
        return done(nullptr, 0);
    }
    if (!listed) {
        // A profile of our own holding this executable.
        std::vector<std::uint16_t> profile_name(StringMax);
        Copy(profile_name.data(), OwnProfile);
        status = api.find_profile(session, profile_name.data(), &profile);
        if (status == ProfileNotFound) {
            auto info = std::make_unique<ProfileInfo>();
            info->version = Version<ProfileInfo>(1);
            Copy(info->name, OwnProfile);
            status = api.create_profile(session, info.get(), &profile);
        }
        if (status != Ok) {
            return done("could not make the Bldbrne-VR profile", status);
        }
        app = std::make_unique<Application>();
        app->version = Version<Application>(4);
        Copy(app->app_name, exe);
        Copy(app->friendly_name, OwnProfile);
        status = api.create_application(session, profile, app.get());
        if (status != Ok) {
            return done("could not add the executable to the Bldbrne-VR profile", status);
        }
    }
    setting = std::make_unique<Setting>();
    setting->version = Version<Setting>(1);
    setting->id = PreferredPstateId;
    setting->type = 0;
    setting->current.u32 = PreferMax;
    status = api.set_setting(session, profile, setting.get());
    if (status == Ok) {
        status = api.save_settings(session);
    }
    if (status != Ok) {
        return done("could not set prefer maximum performance", status);
    }
    std::filesystem::create_directories(marker.parent_path(), ec);
    std::ofstream out{marker};
    out << "PREFERRED_PSTATE prefer maximum performance set for the executable's profile; "
           "nvidia_max_power=0 takes it back\n";
    if (out && marker != old_marker) {
        std::filesystem::remove(old_marker, ec);
    }
    LOG_INFO(Render_Vulkan, "NVIDIA profile: power management mode {} -> prefer maximum "
                            "performance (vr.ini nvidia_max_power=0 takes it back)",
             current);
    done(nullptr, 0);
}

} // namespace

void ApplyNvidiaPowerProfile() {
    static const bool once = (ApplyOnce(), true);
    (void)once;
}

} // namespace Vulkan

#else

namespace Vulkan {
void ApplyNvidiaPowerProfile() {}
} // namespace Vulkan

#endif
