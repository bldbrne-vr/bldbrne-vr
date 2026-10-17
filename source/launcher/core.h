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
std::string Utf8(std::wstring_view text);
std::wstring Wide(std::string_view text);
std::string PathText(const fs::path &path);
fs::path Path(std::string_view text);
std::string Read(const fs::path &path);
fs::path Write(const fs::path &path, const std::string &text,
               const std::optional<std::string> &expected = std::nullopt);
std::wstring Quote(std::wstring_view text);
fs::path NormalizeSaveHome(const fs::path &path);
fs::path GameExecutable(const fs::path &path);
// config.json for a launch: the save home plus the headset preset's emulator settings. Unknown
// keys are kept.
std::string LaunchConfig(const std::string &text, const fs::path &home);
// vr.ini with the renderer keys the native headset preset needs. The player's other keys stay.
std::string LaunchIni(const std::string &text);
// The eye separation for the native renderer, in metres: vr.ini ipd_mm, or 64 mm for "Use headset".
std::wstring NativeIpd(const std::string &ini);
// bbvr_patch.xml next to the runtime, else next to the launcher.
fs::path PatchFile(const fs::path &runtime);
// Refuses an eboot whose frame-limiter code differs from the build the patch file was audited on.
void CheckGameBuild(const fs::path &eboot);
// param.sfo's APP_VER ("01.00"), nullopt when it cannot be read.
std::optional<std::string> AppVersion(const fs::path &param_sfo);
// A path as the game can read it: shadPS4's narrow main() gets '?' for characters outside the
// system code page, so such a path becomes its Windows short name (or this throws).
std::wstring ArgumentPath(const fs::path &path);
fs::path ExecutableDirectory();
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
    static Paths Default();
};
struct Profile {
    std::string game;
    std::string saves;
    std::string runtime;
    bool fullscreen = true;
    bool check_updates = false;
    bool use_bundled_runtime = true;
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
    std::vector<std::string> notes; // what has not been tested, plainly
};
SystemInfo ReadSystem();
std::string NvidiaDriver(uint64_t umd_version); // DXGI user-mode driver version -> "560.94"
// A desktop shortcut that starts the game straight away (BloodborneVR.exe --play).
fs::path CreatePlayShortcut();
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
};
void ValidateSetup(const Profile &profile);
LaunchPlan Prepare(const Profile &profile, const Paths &paths);
HANDLE Launch(const LaunchPlan &plan);
bool RuntimeRunning();
std::optional<fs::path> PickPath(HWND owner, bool folder, const wchar_t *title,
                                 const wchar_t *pattern = L"*.*");
void OpenPath(const fs::path &path);
} // namespace Launcher
