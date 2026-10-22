// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "core/vr/vr_ini.h"
#include <array>
#include <filesystem>
#include <map>
#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <vector>
#include <windows.h>

namespace Launcher {
namespace fs = std::filesystem;
using Json = nlohmann::json;
inline constexpr int MinGameRefreshHz = 30, MaxGameRefreshHz = 240;
constexpr bool ValidGameRefreshHz(int hz) { return hz >= MinGameRefreshHz && hz <= MaxGameRefreshHz; }
// Follow mode samples the active headset rate at launch; unavailable/unsupported rates use 90.
int GameRefreshRate(bool follow_headset, int manual_hz, float headset_hz);
std::string Utf8(std::wstring_view text);
std::wstring Wide(std::string_view text);
std::string PathText(const fs::path &path);
fs::path Path(std::string_view text);
std::string Read(const fs::path &path);
std::string FileSha256(const fs::path &path);
fs::path Write(const fs::path &path, const std::string &text,
               const std::optional<std::string> &expected = std::nullopt);
std::wstring Quote(std::wstring_view text);
fs::path NormalizeSaveHome(const fs::path &path);
fs::path GameExecutable(const fs::path &path);
// config.json for a launch: the save home plus the headset preset's emulator settings. Unknown
// keys are kept.
std::string LaunchConfig(const std::string &text, const fs::path &home, int refresh_hz = 90);
enum class StereoPreset { Native, Dibr };
enum class StereoChoice { Auto, Dibr, Native };
// Known dedicated GPUs with at most 8 GiB use one centre render. Unknown memory keeps native.
StereoPreset StartupStereo(uint64_t dedicated_bytes);
// Also DIBR when the card's Vulkan driver lacks the multiview native stereo needs (a right eye
// left gray on some AMD cards, 2026-10-21).
struct SystemInfo;
StereoPreset StartupStereo(const SystemInfo &system);
// vr.ini with a consistent startup preset. The player's other keys stay.
std::string LaunchIni(const std::string &text, StereoPreset preset = StereoPreset::Native, int refresh_hz = 90, bool follow_headset = false);
// The eye separation for the native renderer, in metres: vr.ini ipd_mm, or 64 mm for "Use headset".
// VR_NATIVE_IPD for the game: "auto" for the included runtime (it reads ipd_mm or the headset
// itself, live); the eye distance in metres for a custom one (an older runtime needs a number).
std::wstring NativeIpd(const std::string &ini, bool bundled_runtime);
// bbvr_patch.xml next to the runtime, else next to the launcher.
fs::path PatchFile(const fs::path &runtime);
// Refuses an eboot whose frame-limiter code differs from the build the patch file was audited on.
// Returns supported APP_VER metadata after the executable signature checks pass.
std::string CheckGameBuild(const fs::path &eboot);
// Adapt only the launch-time patch copy to the validated game metadata.
std::string PatchFrameRate(const std::string& xml,int hz);
std::string PatchForVersion(const std::string &xml, const std::string &version);
// param.sfo's APP_VER ("01.00" or "01.09"), nullopt when it cannot be read.
std::optional<std::string> AppVersion(const fs::path &param_sfo);
// Diagnostic metadata and streamed SHA-256 of the on-disk executable. Missing/unreadable
// fields are reported as unavailable; this does not decide compatibility or modify the game.
std::string GameSupportInfo(const fs::path &eboot);
// A path as the game can read it: shadPS4's narrow main() gets '?' for characters outside the
// system code page, so such a path becomes its Windows short name (or this throws).
std::wstring ArgumentPath(const fs::path &path);
fs::path ExecutableDirectory();
fs::path LocalAppData();
struct Paths {
    fs::path root;
    fs::path User() const {
        return root / "user";
    }
    fs::path Ini() const {
        return User() / "vr.ini";
    }
    fs::path Profile() const {
        return root / "launcher.json";
    }
    fs::path Config() const {
        return User() / "config.json";
    }
    // %LocalAppData%\Bldbrne-VRData; the pre-0.2.7 BloodborneVRData while it could not be moved.
    // Pure: it never moves anything (MigrateLegacyData does, once, at the launcher's start).
    static Paths Default();
};
// The data folder (settings, saves, shader cache, logs) under %LocalAppData%: named after the
// game before 0.2.7, after the app since.
inline constexpr const char *DataFolderName = "Bldbrne-VRData";
inline constexpr const char *LegacyDataFolderName = "BloodborneVRData";
// The installer's folders under %LocalAppData% (the Velopack package id): BloodborneVR up to the
// bridge release, Bldbrne-VR since.
inline constexpr const wchar_t *LegacyInstallFolderName = L"BloodborneVR";
// Moves <base>\BloodborneVRData to <base>\Bldbrne-VRData when only the old one exists, and
// rewrites launcher.json's paths that pointed into it. False (nothing changed) when there was
// nothing to move or the move failed (a file in use): Paths::Default then keeps the old folder.
bool MigrateLegacyData(const fs::path &base);
bool MigrateLegacyData(); // under %LocalAppData%
struct Profile {
    StereoPreset SelectedStereo(uint64_t dedicated_bytes) const;
    StereoPreset SelectedStereo(const SystemInfo &system) const;
    std::string game;
    std::string saves;
    std::string runtime;
    // The desktop mirror windowed unless the player chose fullscreen (fullscreen_chosen): a
    // profile saved before 0.2.3 stored the old default, fullscreen, without anyone choosing it.
    bool fullscreen = false;
    bool fullscreen_chosen = false;
    // On unless the player turned it off (check_updates_chosen): a profile saved before 0.2.3
    // stored the old default, off, without anyone choosing it.
    bool check_updates = true;
    bool check_updates_chosen = false;
    bool use_bundled_runtime = true;
    StereoChoice stereo = StereoChoice::Auto;
    int refresh_hz=90; // Saved manual cap, or resolved cap in a private launch copy.
    bool follow_headset=false; // Preserve existing fixed-rate profiles until explicitly selected.
    static Profile Load(const Paths &paths);
    void Save(const Paths &paths) const;
};
// The launcher's VR settings mirror the in-game VR page (src/core/vr/vr_page.h Rows/Tabs): the
// same tabs, labels and vr.ini values, without what is read from the headset. Graphics and DLSS 5
// are drawn by the launcher itself: their rows share one key, graphics_config.
struct Option {
    const char *label;
    std::vector<std::string> values; // one per key; Keep leaves that key as it is
};
inline constexpr const char *Keep = "*";
struct Setting {
    const char *category;
    const char *label;
    const char *help;
    std::vector<std::string> keys;
    std::vector<Option> options; // empty: a number row
    const char *details = "";    // the row's tooltip
    // Number rows (one key): the shown value runs min..max in steps of `step`, the file holds
    // shown * scale (written whole when it is, else with 4 significant digits, as the page does).
    // `zero`: a value of 0 means something else, shown as this text, just below min.
    int min = 0, max = 0, step = 1;
    double scale = 1;
    int fallback = 0; // the shown value of the code default
    const char *unit = "";
    bool sign = false;
    const char *zero = nullptr;
    bool Number() const {
        return options.empty();
    }
};
const std::vector<const char *> &Categories();
const std::vector<Setting> &Settings();
// A number row's file value for a shown value, and the shown value for a file value (clamped,
// 0 kept when the row has a zero text; nullopt for text that is not a number).
std::string NumberValue(const Setting &row, int shown);
std::optional<int> NumberShown(const Setting &row, const std::string &value);
// Choice matching as the page does it (vr_page.h SameValue): equal numbers ("0.50" and "0.5"),
// a file's true/false against an option's 1/0, or equal text.
bool SameValue(const std::string &a, const std::string &b);
// What a key reset to the game default leaves in the file (nullopt: the key is removed), as the
// page's SettingsStore::Remove does it: weighted_weapons is written On, because an absent key lets
// a private weapon_weight= above 0 take over (vr_session.h WeaponWeightStrength).
std::optional<std::string> ResetValue(const std::string &key);
// Save backups: <data>/save_backups/<local time>/<user>/savedata for every numbered user under the
// save home. A backup identical to the newest one is not repeated; the newest MaxSaveBackups stay.
constexpr size_t MaxSaveBackups = 10;
struct SaveBackup {
    fs::path folder;
    std::string label; // "2026-10-14 21:05:33"
};
fs::path SaveBackupRoot(const Paths &paths);
// The new backup's folder, or empty when there were no saves or nothing changed.
fs::path BackupSaves(const Paths &paths, const fs::path &home);
std::vector<SaveBackup> SaveBackups(const Paths &paths); // newest first
// Backs the current saves up first, then replaces each user's savedata the backup holds.
void RestoreSaves(const Paths &paths, const fs::path &home, const fs::path &backup);
// vr.ini graphics_config (src/core/vr/graphics_settings.h): version 1, scale -50..50 (steps of
// 10, relative to the headset's size), upscaler 0 off / 1 DLSS / 2 FSR 4, DLSS profile, FSR 4
// profile (0 performance, 1 balanced, 2 quality), DLSS 5 on, its profile, then three 0..100 DLSS 5
// strengths. An absent or invalid value is the game's default (everything off at 100%).
using GraphicsFields = std::array<int, 10>;
constexpr GraphicsFields DefaultGraphics{1, 0, 0, 1, 1, 0, 1, 100, 80, 85};
GraphicsFields ParseGraphics(const std::optional<std::string> &value);
std::string GraphicsText(const GraphicsFields &fields);
// The upscaler libraries the runtime loads from its own folder (dlss / fsr4).
bool DlssIncluded(const fs::path &runtime);
bool Fsr4Included(const fs::path &runtime);
bool Fsr31Included(const fs::path &runtime);
// DLSS 5 is the DLSS bridge with its neural export (vr_dlss_runtime.cpp VrDlssSetNeuralConfig).
bool Dlss5Included(const fs::path &runtime);
// The graphics card most likely to run the game (the dedicated one with the most memory) and the
// machine's memory, read once through DXGI.
struct SystemInfo {
    std::string gpu;      // "NVIDIA GeForce RTX 4080"
    std::string driver;   // "560.94" on NVIDIA, else the Windows driver version
    uint64_t vram{};      // dedicated video memory, bytes
    uint64_t ram{};       // physical memory, bytes
    bool integrated{};    // no dedicated graphics card found
    bool nvidia{};
    // Vulkan's multiview support on that card (native stereo draws both eyes with it). Limited:
    // the driver reports multiview without geometry- or tessellation-shader multiview, or none.
    bool multiview_known{};
    bool multiview_limited{};
    std::vector<std::string> notes; // what has not been tested, plainly
};
// `probe_multiview`: also create a Vulkan instance to read multiview support (about 0.1 s the
// first time, then cached); false leaves multiview_known false.
SystemInfo ReadSystem(bool probe_multiview = true);
std::string NvidiaDriver(uint64_t umd_version); // DXGI user-mode driver version -> "560.94"
// A desktop shortcut that starts the game straight away (Bldbrne-VR.exe --play).
fs::path CreatePlayShortcut();
// The desktop Play shortcut (Play Bldbrne-VR, Play BB-VR before 0.2.7), made again for this
// launcher when it is this app's to take over: its target gone, in this launcher's folder (a
// portable ZIP over an older one), or in the old installer's folder. Another working copy's
// shortcut stays. True when repaired.
bool RepairPlayShortcut();
// A graphics restart relaunches the game and the old process exits with 0 (emulator.cpp
// Relaunch): the new one is a shadps4.exe whose parent is `pid`. 0 when there is none.
DWORD RestartedProcess(DWORD pid);
// What an exit code means, in plain words; empty for a normal exit (0).
std::string ExitReason(DWORD code);
// The game log (user/log/shad_log.txt, or the newest *.log); empty when logging is off.
fs::path LogFile(const Paths &paths);
// The log's last Critical and Error lines, oldest first.
std::vector<std::string> LogProblems(const Paths &paths, size_t max = 6);
// The game has built its shaders at least once (user/cache/<serial>/profile.bin).
bool ShaderCacheReady(const Paths &paths);
// The active OpenXR runtime's name ("Oculus", "SteamVR"...), empty when none is set.
std::string OpenXrRuntime();
struct LaunchPlan {
    fs::path executable;
    fs::path directory;
    std::wstring command;
    std::vector<wchar_t> environment;
    std::string game_support_info; // captured before launch for this session's report
};
void ValidateSetup(const Profile &profile);
LaunchPlan Prepare(const Profile &profile, const Paths &paths);
LaunchPlan Prepare(const Profile &profile, const Paths &paths, StereoPreset preset);
HANDLE Launch(const LaunchPlan &plan);
bool RuntimeRunning();
std::optional<fs::path> PickPath(HWND owner, bool folder, const wchar_t *title,
                                 const wchar_t *pattern = L"*.*");
void OpenPath(const fs::path &path);
} // namespace Launcher
