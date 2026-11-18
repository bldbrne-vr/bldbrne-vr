// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "core/vr/dlss5_file.h"
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
std::string LaunchConfig(const std::string &text, const fs::path &home, int refresh_hz = 90,
                         const std::optional<fs::path>& addons = std::nullopt);
enum class StereoPreset { Native, Dibr };
enum class StereoChoice { Auto, Dibr, Native };
inline constexpr const char* StereoLabel(StereoPreset preset) {
    return preset == StereoPreset::Dibr ? "Simplified Rendering" : "Native stereo";
}
// How native stereo draws its two eyes (the runtime's VR_NATIVE_VIEWS, NativeStereo::LayerDraws).
// Multiview: the graphics driver repeats every draw for the second eye. Layers: the game draws
// each eye itself (an RX 6700's multiview left the right eye white, GitHub #9). Auto: layers on
// AMD RX 400 to RX 6000 cards, multiview on the rest, decided by the runtime on the card it opens
// (VR::NativeViews::LayersByDefault); the launcher shows its own guess (SystemInfo::auto_layers).
enum class NativeViews { Auto, Multiview, Layers };
// VR_NATIVE_VIEWS for a launch, DIBR ones too (the runtime's shader cache records the mode, so
// both kinds of session must agree): the player's choice; empty for Auto, which the runtime
// decides on the card it really opens (a PC with two graphics cards gets the right one).
std::wstring NativeViewsValue(NativeViews choice);
// Known dedicated GPUs with at most 8 GiB use one centre render. Unknown memory keeps native.
StereoPreset StartupStereo(uint64_t dedicated_bytes);
// Also DIBR when the card's Vulkan driver lacks the multiview native stereo needs (a right eye
// left gray on some AMD cards, 2026-10-21), and on AMD RX 400, RX 500 and RX 5000 cards
// (SystemInfo::auto_dibr, 2026-10-26).
struct SystemInfo;
StereoPreset StartupStereo(const SystemInfo &system);
// vr.ini with a consistent startup preset. The player's other keys stay.
std::string LaunchIni(const std::string &text, StereoPreset preset = StereoPreset::Native, int refresh_hz = 90, bool follow_headset = false);
// The game's one-time vr.ini changes of an old default that a row shows (vr_session.cpp
// LoadSettings), made by the launcher too: an updated install shows the new default before the
// game's next start, and a choice made in the launcher then is never changed by the game.
// Restore the fan and card-based water defaults once. Apply pending player choices in the same write.
std::string MigrateIni(const std::string &text, const std::vector<VR::Ini::Edit> &choices = {});
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
// Optional effects workaround; verifies the supported function before enabling the launch copy.
std::string PatchIntelSfx(const std::string &xml, const fs::path &eboot, bool enabled);
// Non-aggro mode (the launcher only, off by default): Kyo's Player Stealth, one byte of the game's
// own debug flags (zero-initialised data), so enemies do not see you until you attack them.
std::string PatchNonAggro(const std::string &xml, bool enabled);
// param.sfo's APP_VER ("01.00" or "01.09"), nullopt when it cannot be read.
std::optional<std::string> AppVersion(const fs::path &param_sfo);
// The game's save folder as the emulator picks it (core/libraries/save_data/savedata.cpp):
// INSTALL_DIR_SAVEDATA, else TITLE_ID. CUSA03173 saves in CUSA00207, CUSA00900 in CUSA00900.
// Throws when neither is a plain title id (four capitals, five digits).
std::string SaveDataFolder(const fs::path &param_sfo);
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
    NativeViews native_views = NativeViews::Auto;
    int refresh_hz=90; // Saved manual cap, or resolved cap in a private launch copy.
    bool follow_headset=false; // Preserve existing fixed-rate profiles until explicitly selected.
    bool intel_sfx_workaround = true;
    bool non_aggro = false;
    bool free_camera = false; // launcher-only; implies non-aggro for this launch // Non-aggro mode (PatchNonAggro); never in a PERF SCAN
    // The headset the player picked from core/vr/headset_panels.h (its id); empty: the one the
    // runtime's name says, if it says.
    std::string headset;
    std::string dlc; // Empty: automatic discovery; otherwise an extracted add-on folder/root.
    static Profile Load(const Paths &paths);
    void Save(const Paths &paths) const;
};
struct DlcStatus {
    fs::path root;
    std::string message;
    bool valid = true; // false: the chosen folder holds no usable DLC (the game still launches).
    bool found = false; // The Old Hunters detected, or known bundled edition.
};
DlcStatus ResolveDlc(const Paths& paths, const Profile& profile);

// The launcher's VR settings are the in-game VR page's (src/core/vr/vr_page.h Rows/Tabs, read
// from there): its tabs in its order, each tab's pages as headings, its labels, help and choices,
// then the page's rows the launcher alone shows. Upscaling and Save stay in the game; the
// launcher's own Graphics tab (graphics_config and the launch options) and Extras follow.
struct Option {
    const char *label;
    std::vector<std::string> values; // one per key; Keep leaves that key as it is
    bool ini_only = false;           // shown when the file holds it, never offered (vr_page.h)
};
inline constexpr const char *Keep = "*";
struct Setting {
    const char *category;
    const char *label; // the page's words: "{entry:...}" names the player's buttons (RowLabel)
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
    const char *id = nullptr;    // the in-game row (vr_page.h Rows); nullptr: the launcher's own
    const char *group = nullptr; // the heading it sits under in its tab
    const char *words = "";      // more words the search finds it by
    bool Number() const {
        return options.empty();
    }
};
// The VR settings page's tabs: the in-game page's but Upscaling and Save, then Graphics, Extras.
const std::vector<const char *> &Categories();
// The heading of the page's rows the game's VR page does not show (vr.ini and the launcher).
inline constexpr const char *LauncherOnlyGroup = "Only in the launcher";
// A key's value in the game when vr.ini lacks it, for the keys of the rows that set two keys and
// foveation (each from the game's own settings); nullopt: not known here.
std::optional<std::string> CodeDefault(const std::string &key);
// Display interpretation only. Keep the original file spelling until the player makes a choice.
std::optional<std::string> ChoiceOverride(std::string_view key, std::optional<std::string> value);
std::optional<std::string> CardDefault(std::string_view key, uint64_t dedicated_bytes);
bool FogEnabled(const std::optional<std::string> &choice, uint64_t dedicated_bytes);
// Whether every word of `needle` (lower case, split at spaces) is in `text` (lower case).
bool SearchFinds(const std::string &needle, const std::string &text);
// The eye size the headset asked for at the game's last start (user/log/headset.json, written by
// the game): the larger eye. nullopt before the first launch or when the file is unreadable.
struct HeadsetRequest {
    std::string system;
    unsigned width{}, height{};
    unsigned max_width{8192}, max_height{8192}; // Older headset.json files lack limits.
};
std::optional<HeadsetRequest> LastHeadsetRequest(const Paths &paths);
// Per-eye widths above this tend to run out of graphics memory (PS VR2 on SteamVR at 100% asks
// for 3400x3468, 2026-10-23): the launcher shows them in red and offers a lower scaling. 2240
// since 2026-10-23 (was 2200).
inline constexpr unsigned HighEyeWidth = 2240;
// The game's graphics memory for native stereo, in MB, from each eye's render and output size.
// Fitted on 2026-10-23 (RTX 5080, 45 s mock runs, 1456x1576 to 2000x2160 per eye, DLSS Ultra
// Performance / Quality and no upscaler; within 150 MB of each): with an upscaler 2095 MB plus
// 701 MB per million render pixels and 365 per million output pixels (per eye); without one
// 1409 MB plus 951 per million pixels. A headset session runs higher than those short runs
// (PS VR2 at 3400x3472, Ultra Performance: 8.6 GB with that day's cuts against 7.7 predicted), so
// the estimate adds VramSessionMargin: 14% (the user's choice, 2026-11-03; 15% before). That day
// 8-minute runs through seven areas on a 6 GB card's budget (the caches held to it) peaked at
// 5361 MB at 1816x1968 (DLSS from 1208x1312: 4510 fitted) and 4472 MB at 1456x1576 (3644 fitted).
// A 16 GB card holds more than that (8.1 GB in a 2200x2376 headset session): its caches keep what
// fits.
inline constexpr double VramSessionMargin = 1.14;
inline double EstimateVramMb(unsigned render_w, unsigned render_h, unsigned out_w, unsigned out_h,
                             bool upscaler) {
    const double render = double(render_w) * render_h / 1e6, output = double(out_w) * out_h / 1e6;
    const double mb = upscaler ? 2095 + 701 * render + 365 * output : 1409 + 951 * output;
    return mb * VramSessionMargin;
}
// What the headset software (SteamVR, Link), Windows and other programs keep of the card. Windows
// pages the game's memory out from about 90% of what is left (PS VR2: 12.5 GB of a 13.6 GB
// budget on a 16 GB card, single-digit frame rates).
inline constexpr double VramReserveMb = 1536;
inline bool VramFitsCard(double estimate_mb, uint64_t card_bytes) {
    return !estimate_mb || !card_bytes ||
           estimate_mb <= (double(card_bytes >> 20) - VramReserveMb) * 0.9;
}
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
// A launch's backup of these saves (the soft save stands on it, core/vr/soft_save_backup.h): the
// one BackupSaves makes now, or the newest when the saves have not changed since; empty without
// saves.
fs::path LaunchBackup(const Paths &paths, const fs::path &home);
std::vector<SaveBackup> SaveBackups(const Paths &paths); // newest first
// Backs the current saves up first, then replaces each user's savedata the backup holds.
void RestoreSaves(const Paths &paths, const fs::path &home, const fs::path &backup);
// vr.ini graphics_config (src/core/vr/graphics_settings.h): version 1, scale -50..50 (steps of
// 10, relative to the headset's size), upscaler 0 off / 1 DLSS / 2 FSR 4, DLSS profile, FSR 4
// profile (0 performance, 1 balanced, 2 quality, 3 ultra performance, 4 ultra quality, 5 balanced
// plus), DLSS 5 on, its profile, then three 0..100 DLSS 5 strengths. An absent or invalid value is
// the game's default (everything off at 100%).
using GraphicsFields = std::array<int, 10>;
// A preset's name, and the DLSS model the game picks for it (graphics_settings.h
// DlssModelForProfile): M for Ultra Quality and Balanced Plus, K for the rest.
const char *PresetName(int preset);
inline const char *PresetDlssModel(int preset) { return preset == 4 || preset == 5 ? "M" : "K"; }
constexpr GraphicsFields DefaultGraphics{1, 0, 0, 1, 1, 0, 1, 100, 80, 85};
// For validated graphics fields: DIBR runs the regular upscalers, but not DLSS 5.
// Share this between the AA control and category reset so a bypassed preference stays intact.
constexpr bool UpscalerBypassesAa(const GraphicsFields& fields, StereoPreset stereo) {
    return fields[2] != 0 &&
           (stereo == StereoPreset::Native || !(fields[2] == 1 && fields[5]));
}
constexpr bool FoveatedUpscaleAvailable(const GraphicsFields& fields, StereoPreset stereo) {
    return stereo == StereoPreset::Native && fields[2] != 0 && !(fields[2] == 1 && fields[5]);
}
GraphicsFields ParseGraphics(const std::optional<std::string> &value);
std::string GraphicsText(const GraphicsFields &fields);
// The upscaler libraries the runtime loads from its own folder (dlss / fsr4).
bool DlssIncluded(const fs::path &runtime);
bool Fsr4Included(const fs::path &runtime);
bool Fsr31Included(const fs::path &runtime);
bool Fsr41Included(const fs::path &runtime);
// DLSS 5: the DLSS bridge with its neural exports (vr_dlss_runtime.cpp VrDlssSetNeuralRuntime) and
// its forwarder. NVIDIA's nvngx_dlssnr.dll is never shipped: the player adds their own
// (core/vr/dlss5_file.h; the DLSS 5 tab).
bool Dlss5Included(const fs::path &runtime);
struct Dlss5Found {
    fs::path file;
    VR::Dlss5File::Check check;
};
// Every nvngx_dlssnr.dll on this PC where games keep it (Steam, Epic and Xbox game folders, NVIDIA's
// driver downloads, beside the runtime), each checked: accepted ones first, the newest first.
// Takes seconds: off the UI thread.
std::vector<Dlss5Found> FindDlss5(const fs::path &runtime);
// Copies `from` to the player's DLSS 5 file (user/dlss5) when it is NVIDIA's signed file; throws
// the reason otherwise. Returns the copy's check.
VR::Dlss5File::Check AddDlss5(const Paths &paths, const fs::path &from);
void RemoveDlss5(const Paths &paths);
// The graphics card most likely to run the game (the dedicated one with the most memory) and the
// machine's memory, read once through DXGI.
struct SystemInfo {
    std::string gpu;      // "NVIDIA GeForce RTX 4080"
    std::string driver;   // "560.94" on NVIDIA, else the Windows driver version
    uint64_t vram{};      // dedicated video memory, bytes
    uint64_t ram{};       // physical memory, bytes
    bool integrated{};    // no dedicated graphics card found
    bool nvidia{};
    bool amd{};           // vendor 0x1002
    // Native stereo's Auto eyes are layers on this card: an AMD RX 400 to RX 6000
    // (VR::NativeViews::LayersByDefault, the runtime's own rule).
    bool auto_layers{};
    // Auto stereo starts DIBR on this card: an AMD RX 400, RX 500 or RX 5000
    // (VR::NativeViews::DibrByDefault).
    bool auto_dibr{};
    // Vulkan's multiview support on that card (native stereo draws both eyes with it). Limited:
    // the driver reports multiview without geometry- or tessellation-shader multiview, or none.
    bool multiview_known{};
    bool multiview_limited{};
    std::vector<std::string> notes; // what has not been tested, plainly
};
// `probe_multiview`: also create a Vulkan instance to read multiview support (about 0.1 s the
// first time, then cached); false leaves multiview_known false.
SystemInfo ReadSystem(bool probe_multiview = true);
// A new player's upscaler, by graphics card (2026-10-30, the user: a new player starts with the
// upscaler their card runs well, for a clean image; a player who already has a launcher profile
// keeps their own settings). NVIDIA, DLSS: RTX 5090 Ultra Quality; RTX 5070 / 5080 and RTX
// 4080 / 4090 (Ti, SUPER) Quality; any other RTX 30-series or newer with 10 GB or more Balanced.
// AMD, FSR 3.1.5: RX 9070 and RX 7900 Ultra Quality (their GRE: Quality); RX 7800 and RX 6800 /
// 6900 / 6950 Quality; RX 9060, RX 7700 and RX 6700 / 6750 Balanced Plus, with 10 GB or more. A
// laptop chip (weaker than its desktop name) gets Balanced at most on NVIDIA and none on AMD;
// every other card starts without one.
struct StartupUpscale {
    int upscaler{0}; // graphics_config's provider: 0 off, 1 DLSS, 3 FSR 3.1.5
    int preset{1};   // that provider's preset (PresetName)
    bool operator==(const StartupUpscale &) const = default;
};
StartupUpscale StartupUpscaler(const SystemInfo &system);
// graphics_config for a new player: the game's defaults with StartupUpscaler's choice.
GraphicsFields StartupGraphics(const SystemInfo &system);
// A new player's foveation (2026-11-04, the user: a powerful card starts on Light, a wider sharp
// centre): the cards StartupUpscaler starts on Quality or Ultra Quality get 3 (Light); every
// other card keeps -1 (Auto: On where the card supports it).
int StartupFoveation(const SystemInfo &system);
// A new player's far shadows (2026-11-05, the user: 8 GB or less at the fastest): true writes
// shadows_far=reduced on a card with 8 GB or less, or integrated graphics; false (memory unknown
// or a bigger card) writes nothing.
bool StartupReducedShadows(const SystemInfo &system);
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
