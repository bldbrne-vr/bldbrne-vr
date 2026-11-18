// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "core.h"
#include "controls_guide.h"
#include "core/vr/controls_view.h"
#include "core/vr/native_views.h"
#include "core/vr/sky_textures.h"
#include "core/vr/vr_page.h"
#include "release_version.h"
#include "sky_upscale.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <sstream>
#include <pugixml.hpp>
#include <set>
#include <shellapi.h>
using namespace Launcher;
using namespace std::string_literals;
static int checks = 0;
static void Check(bool pass, const char *name) {
    ++checks;
    if (!pass)
        throw std::runtime_error(name);
}
template <class F> void Throws(F fn, const char *name) {
    bool threw = false;
    try {
        fn();
    } catch (const std::exception &) {
        threw = true;
    }
    Check(threw, name);
}
// Minimal string metadata with independent APP_VER and VERSION values.
static void WriteParamSfo(const fs::path &path, const std::string &app_version,
                          const std::string &title = "CUSA03173", const std::string &version = "",
                          const std::string &save_dir = "", const std::string &content_id = "") {
    std::vector<std::pair<std::string, std::string>> fields{
        {"APP_VER", app_version}, {"TITLE_ID", title}, {"VERSION", version}};
    if (!save_dir.empty())
        fields.insert(fields.begin() + 1, {"INSTALL_DIR_SAVEDATA", save_dir});
    if (!content_id.empty()) {
        fields.push_back({"CATEGORY", "ac"});
        fields.push_back({"CONTENT_ID", content_id});
    }
    std::string keys, values;
    for (const auto &[key, value] : fields) {
        keys += key + '\0';
        values += value + '\0';
    }
    std::string sfo("\0PSF", 4);
    auto u16 = [&](uint16_t v) { sfo += char(v & 0xff), sfo += char(v >> 8); };
    auto u32 = [&](uint32_t v) { u16(uint16_t(v)), u16(uint16_t(v >> 16)); };
    const auto header = uint32_t(20 + 16 * fields.size());
    u32(0x0101), u32(header), u32(uint32_t(header + keys.size())), u32(uint32_t(fields.size()));
    uint16_t key_at = 0;
    uint32_t value_at = 0;
    for (const auto &[key, value] : fields) {
        u16(key_at), u16(0x0204), u32(uint32_t(value.size() + 1)),
            u32(uint32_t(value.size() + 1)), u32(value_at);
        key_at += uint16_t(key.size() + 1);
        value_at += uint32_t(value.size() + 1);
    }
    std::ofstream(path, std::ios::binary) << sfo << keys << values;
}
int main() {
    // BBVR_PREPARE_CHECK_DATA=<data folder>: the launch step on a copy of a player's data folder
    // (launcher.json + user/config.json): its DLC status, Prepare's plan and the config it writes.
    // Only that folder is written; nothing is launched.
    if (const char *data = std::getenv("BBVR_PREPARE_CHECK_DATA")) {
        try {
            const Paths paths{fs::path(data)};
            const auto profile = Profile::Load(paths);
            const auto status = ResolveDlc(paths, profile);
            std::printf("DLC: valid=%d found=%d root=%s\n%s\n", int(status.valid), int(status.found),
                        PathText(status.root).c_str(), status.message.c_str());
            const auto plan = Prepare(profile, paths);
            std::printf("PREPARE OK: %s\nconfig.json after:\n%s\n", PathText(plan.executable).c_str(),
                        Read(paths.Config()).c_str());
            return 0;
        } catch (const std::exception &e) {
            std::printf("PREPARE FAILED: %s\n", e.what());
            return 1;
        }
    }
    try {
        // Version labels are public; the package values must sort for Velopack, including
        // when upgrading from an old installation that uses its original package number.
        const std::pair<std::string_view, std::string_view> releases[]{
            {"0.2.7", "0.2.7"}, {"0.8.81", "0.8.81"}, {"0.9.2", "0.9.2"},
            {"0.9.21", "0.9.121"}, {"0.9.22", "0.9.122"}, {"0.9.3-rc.1", "0.9.130-rc.1"},
            {"0.9.3", "0.9.130"}, {"0.9.5", "0.9.150"}, {"1.0.0", "1.0.100"},
        };
        for (const auto& [label, package] : releases) {
            Check(ReleaseVersion::Package(label) == package, "sortable release package version");
            Check(ReleaseVersion::Display(package) == label, "update uses the public release label");
        }
        Check(ReleaseVersion::Display("0.9.21") == "0.9.21", "legacy hotfix still displays as published");
        Check(!ReleaseVersion::Package("0.9.20") && !ReleaseVersion::Package("0.9.11") &&
                  !ReleaseVersion::Package("0.9.121") && !ReleaseVersion::Package("0.9.02") &&
                  !ReleaseVersion::Package("0.9.2.1"),
              "ambiguous, pre-cutover and reserved release labels are rejected");
        const auto root = fs::temp_directory_path() /
                          ("bbvr-launcher-test-" + std::to_string(GetCurrentProcessId()));
        fs::create_directories(root);
        const auto config = root / "vr.ini";
        const std::string original = "# keep\r\nlaser = 0\r\nunknown=custom\nlaser=0\nlast=x";
        Write(config, original);
        auto edited = VR::Ini::ApplyEdits(
            original, {{"laser", "1"}, {"last", std::nullopt}, {"seated", "1"}});
        Check(edited == "# keep\r\nlaser=1\r\nunknown=custom\nlaser=1\nseated=1\r\n",
              "INI preserves unrelated content and handles duplicates");
        Check(VR::Ini::ValueOf(edited, "laser") == "1", "INI last value");
        // DLSS 5: only NVIDIA's own signed DLSSNR file is accepted or copied (core/vr/dlss5_file.h).
        Check(VR::Dlss5File::Newer("310.9.16.0", "310.8.0.0") &&
                  !VR::Dlss5File::Newer("310.8.0.0", "310.9.16.0") &&
                  !VR::Dlss5File::Newer("310.8.0.0", "310.8.0.0"),
              "DLSS 5 versions compare by number");
        Check(!VR::Dlss5File::Verify(root / "missing.dll").ok, "DLSS 5: a missing file is refused");
        {
            wchar_t system[MAX_PATH]{};
            GetSystemDirectoryW(system, MAX_PATH);
            const auto other = root / "nvngx_dlssnr.dll";
            fs::copy_file(fs::path(system) / "kernel32.dll", other);
            const auto check = VR::Dlss5File::Verify(other);
            Check(!check.ok && !check.problem.empty(), "DLSS 5: another signed DLL is refused");
            Paths player{root / "player"};
            Throws([&] { AddDlss5(player, other); }, "DLSS 5: a refused file is not added");
            Check(!fs::exists(VR::Dlss5File::InUser(player.User())), "DLSS 5: nothing copied");
        }
        auto backup = Write(config, edited, original);
        Check(Read(backup) == original, "backup preserves exact original");
        Throws([&] { Write(config, "bad", original); }, "conflicting edits rejected");
        Check(Read(config) == edited, "conflicting edits do not overwrite");
        HANDLE lock = CreateFileW((config.native() + L".lock").c_str(), GENERIC_WRITE, 0, nullptr,
                                  CREATE_NEW, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        Check(lock != INVALID_HANDLE_VALUE, "acquire competing lock");
        Throws([&] { Write(config, "locked"); }, "concurrent writer rejected");
        CloseHandle(lock);
        Write(config, "", edited);
        Check(Read(config).empty(), "reset clears overrides");
        const auto game_folder = root / "game folder";
        fs::create_directories(game_folder / "sce_sys");
        // The audited frame-limiter bytes at their file offsets; zeros elsewhere.
        const auto limiter_offset = std::streamoff(0x28eb0) + 0x02434ADA - 0x400000;
        {
            std::ofstream eboot(game_folder / "eboot.bin", std::ios::binary);
            const std::pair<std::uint32_t, std::string> sites[]{
                {0x02434ADA, "\x0f\x84\x9a\x00\x00\x00"s},
                {0x02434BAC, "\xe4\x47\x8f\x02"s},
                {0x02434BB6, "\xde\x47\x8f\x02"s},
                {0x04D29390, "\x6f\x12\x83\x3a"s},
                {0x04D29558, "\x6f\x12\x83\xba"s}};
            for (const auto &[address, bytes] : sites) {
                eboot.seekp(std::streamoff(0x28eb0) + address - 0x400000);
                eboot.write(bytes.data(), std::streamsize(bytes.size()));
            }
        }
        auto poke = [&](char byte) {
            std::fstream eboot(game_folder / "eboot.bin", std::ios::binary | std::ios::in | std::ios::out);
            eboot.seekp(limiter_offset);
            eboot.put(byte);
        };
        WriteParamSfo(game_folder / "sce_sys" / "param.sfo", "01.00");
        Check(AppVersion(game_folder / "sce_sys" / "param.sfo") == "01.00", "APP_VER read");
        Check(!AppVersion(root / "missing.sfo"), "missing param.sfo has no version");
        // Report exact file bytes and separate metadata; never infer update compatibility from
        // APP_VER alone (our GOTY reference says APP_VER 01.00, VERSION 01.09).
        const auto diagnostic_game = root / "diagnostic game";
        fs::create_directories(diagnostic_game / "sce_sys");
        const auto diagnostic_eboot = diagnostic_game / "eboot.bin";
        const auto diagnostic_sfo = diagnostic_game / "sce_sys" / "param.sfo";
        WriteParamSfo(diagnostic_sfo, "01.00", "CUSA03173", "01.09");
        std::ofstream(diagnostic_eboot, std::ios::binary) << "abc";
        const auto report = GameSupportInfo(diagnostic_eboot);
        Check(report.find("TITLE_ID CUSA03173 APP_VER 01.00 VERSION 01.09") != std::string::npos &&
                  report.find("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") !=
                      std::string::npos,
              "report identifies game metadata and standard SHA-256 vector");
        std::ofstream(diagnostic_eboot, std::ios::binary) << std::string(1000000, 'a');
        Check(GameSupportInfo(diagnostic_eboot).find(
                  "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") !=
                  std::string::npos,
              "SHA-256 streams multiple chunks");
        std::ofstream(diagnostic_eboot, std::ios::binary);
        Check(GameSupportInfo(diagnostic_eboot).find(
                  "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") !=
                  std::string::npos,
              "SHA-256 handles an empty file");
        fs::remove(diagnostic_eboot);
        Check(GameSupportInfo(diagnostic_eboot).find("SHA-256 unavailable") != std::string::npos,
              "missing executable does not prevent a support report");
        std::ofstream(diagnostic_sfo, std::ios::binary) << "bad metadata";
        Check(!AppVersion(diagnostic_sfo) &&
                  GameSupportInfo(diagnostic_eboot).find("TITLE_ID unavailable") != std::string::npos,
              "malformed metadata is reported as unavailable");
        WriteParamSfo(diagnostic_sfo, "01.09", "CUSA00900", "01.00");
        Check(GameSupportInfo(diagnostic_eboot).find("TITLE_ID CUSA00900 APP_VER 01.09 VERSION 01.00") !=
                  std::string::npos,
              "US title and version metadata remain independent");
        // The benchmark save goes where the emulator looks (INSTALL_DIR_SAVEDATA, else TITLE_ID).
        Check(SaveDataFolder(diagnostic_sfo) == "CUSA00900",
              "CUSA00900 (no INSTALL_DIR_SAVEDATA) saves under its title id");
        WriteParamSfo(diagnostic_sfo, "01.00", "CUSA03173", "01.09", "CUSA00207");
        Check(SaveDataFolder(diagnostic_sfo) == "CUSA00207",
              "CUSA03173 saves in its INSTALL_DIR_SAVEDATA folder");
        WriteParamSfo(diagnostic_sfo, "01.00", "CUSA03173", "01.09", "../x");
        Throws([&] { SaveDataFolder(diagnostic_sfo); }, "a save folder that is not a title id is refused");
        WriteParamSfo(diagnostic_sfo, "01.09", "CUSA00900", "01.00");

        {
            std::fstream sfo(diagnostic_sfo, std::ios::binary | std::ios::in | std::ios::out);
            sfo.seekp(48); // TITLE_ID value offset: malicious 32-bit offset must not wrap.
            sfo.write("\xff\xff\xff\xff", 4);
        }
        Check(GameSupportInfo(diagnostic_eboot).find("TITLE_ID unavailable APP_VER 01.09") !=
                  std::string::npos,
              "out-of-range SFO field cannot wrap into valid data");

        const auto unicode = root / fs::path(L"game folder \u03a9") / "eboot.bin";
        Check(Path(PathText(unicode)) == unicode, "Unicode path round trip");
        Check(GameExecutable(game_folder) == game_folder / "eboot.bin", "folder resolution");
        Throws([&] { GameExecutable(root); }, "missing game rejected");
        {
            // A PKG, or the folder holding one, is told to extract it first; other folders not.
            const auto said = [](const fs::path &p) {
                try {
                    GameExecutable(p);
                } catch (const std::exception &e) {
                    return std::string(e.what());
                }
                return std::string();
            };
            const auto pkg_dir = root / "downloads";
            fs::create_directories(pkg_dir);
            std::ofstream(pkg_dir / "CUSA03173-app.PKG") << "x";
            Check(said(pkg_dir / "CUSA03173-app.PKG").find("PKG") != std::string::npos,
                  "a PKG file is explained");
            Check(said(pkg_dir).find("Extract the PKG") != std::string::npos,
                  "a folder holding a PKG is explained");
            Check(said(root).find("PKG") == std::string::npos,
                  "a folder without a PKG keeps the plain message");
        }
        Throws([&] { NormalizeSaveHome({}); },
               "empty save home cannot resolve to working directory");
        auto save_home = root / "save home";
        fs::create_directories(save_home / "1000" / "savedata" / "CUSA03173");
        Check(NormalizeSaveHome(save_home / "1000" / "savedata" / "CUSA03173") == save_home,
              "save leaf normalized");
        Check(NormalizeSaveHome(save_home / "1000" / "savedata") == save_home,
              "savedata normalized");
        auto json = Json::parse(LaunchConfig(
            "{\"General\":{\"untouched\":42},\"GPU\":{\"vblank_frequency\":60},\"future\":true}",
            save_home));
        Check(json["General"]["home_dir"] == PathText(save_home), "real runtime home_dir schema");
        Check(json["General"]["untouched"] == 42 && json["future"] == true,
              "unknown JSON preserved");
        Check(json["GPU"]["vblank_frequency"] == 90 &&
                  json["General"]["extra_dmem_in_mbytes"] == 3500 &&
                  json["Vulkan"]["pipeline_cache_enabled"] == true &&
                  json["Vulkan"]["pipeline_cache_archived"] == false &&
                  json["Vulkan"]["vkvalidation_enabled"] == false,
              "headset preset emulator settings");
        const auto with_dlc = Json::parse(LaunchConfig(json.dump(), save_home, 90, root/"add-ons"));
        Check(with_dlc["General"]["addon_install_dir"] == PathText(root/"add-ons") &&
                  with_dlc["General"]["untouched"] == 42, "DLC root passed without losing settings");
        Throws([&] { LaunchConfig("{bad", save_home); }, "corrupt config rejected");
        Throws([&] { LaunchConfig("{\"GPU\":3}", save_home); }, "invalid section rejected");
        const std::string user_ini = "# mine\r\nlaser=1\r\nstereo_mode=1\r\nlate_warp=1\r\n";
        const auto launch_ini = LaunchIni(user_ini);
        Check(VR::Ini::ValueOf(launch_ini, "stereo_mode") == "2" &&
                  VR::Ini::ValueOf(launch_ini, "late_warp") == "0" &&
                  VR::Ini::ValueOf(launch_ini, "refresh_hz") == "90" &&
                  VR::Ini::ValueOf(launch_ini, "game_fps") == "90" &&
                  VR::Ini::ValueOf(launch_ini, "eye_size") == "auto",
              "renderer keys set for the native preset");
        Check(VR::Ini::ValueOf(launch_ini, "laser") == "1" && launch_ini.starts_with("# mine\r\n"),
              "player keys and comments kept");
        Check(LaunchIni(launch_ini) == launch_ini, "renderer keys are idempotent");
        for (const auto preset : {StereoPreset::Native, StereoPreset::Dibr}) {
            for (const float detected : {72.f, 90.f, 120.f, 0.f}) {
                const int rate = GameRefreshRate(true, 144, detected);
                const auto following = LaunchIni(user_ini + "game_fps=0\r\n", preset, rate, true);
                Check(VR::Ini::ValueOf(following, "game_fps") == "0" &&
                          VR::Ini::ValueOf(following, "refresh_hz") == "0" &&
                          Json::parse(LaunchConfig("{}", save_home, rate))["GPU"]["vblank_frequency"] == rate,
                      "follow headset leaves the cap to the game's session refresh (game_fps=0, its "
                      "vblank at the detected rate, 90 if unknown) without requesting a display-rate change");
                Check(LaunchIni(following, preset, rate, true) == following,
                      "follow headset replaces the old auto cap idempotently");
            }
        }
        const auto manual = LaunchIni(user_ini, StereoPreset::Native, 72, false);
        Check(VR::Ini::ValueOf(manual, "game_fps") == "72" &&
                  VR::Ini::ValueOf(manual, "refresh_hz") == "72",
              "manual frame cap and display request remain fixed");
        // The quick item menu's one-time change, as the game makes it (MigrateIni).
        const std::string old_ini = "# mine\r\nquick_arc_pick=hover\r\nlaser=1\r\n";
        const auto migrated = MigrateIni(old_ini);
        Check(VR::Ini::ValueOf(migrated, "quick_arc_pick") == "fan" &&
                  VR::Ini::ValueOf(migrated, "quick_fan_style") == "lancet" &&
                  VR::Ini::ValueOf(migrated, "quick_fan_seen") == "1" &&
                  VR::Ini::ValueOf(migrated, "laser") == "1" && migrated.starts_with("# mine\r\n"),
              "the old Boxes default becomes Fan A once");
        Check(MigrateIni(migrated) == migrated, "the quick menu change is made once");
        const auto chosen = VR::Ini::ApplyEdits(migrated, {{"quick_arc_pick", "hover"}});
        Check(MigrateIni(chosen) == chosen, "Boxes chosen after it stays");
        const auto sector = MigrateIni("quick_arc_pick=sector\r\nquick_fan_style=brass\r\n");
        Check(VR::Ini::ValueOf(sector, "quick_arc_pick") == "sector" &&
                  VR::Ini::ValueOf(sector, "quick_fan_style") == "brass",
              "an own choice stays");
        Check(MigrateIni("").empty(), "a new install's file is the game's");
        const std::string old_water = "# player settings\r\nquick_fan_seen=1\r\nwater_reflections=1\r\nlaser=1\r\n";
        const auto by_card = MigrateIni(old_water);
        Check(!VR::Ini::ValueOf(by_card, "water_reflections") &&
                  VR::Ini::ValueOf(by_card, "newgfx_096_seen") == "1" &&
                  VR::Ini::ValueOf(by_card, "laser") == "1" && by_card.starts_with("# player settings\r\n") &&
                  MigrateIni(by_card) == by_card, "legacy water returns to Auto once");
        for (const auto &original : {std::string{}, old_water, by_card}) {
            for (const auto &[water, streets] : {std::pair{"0", "0"}, {"1", "0"}, {"1", "1"}}) {
                const auto saved = MigrateIni(original, {{"water_reflections", water}, {"wet_streets", streets}});
                Check(VR::Ini::ValueOf(saved, "water_reflections") == water &&
                          VR::Ini::ValueOf(saved, "wet_streets") == streets &&
                          VR::Ini::ValueOf(saved, "newgfx_096_seen") == "1" && MigrateIni(saved) == saved,
                      "an explicit Wetness choice survives the first launch and later reloads");
            }
        }
        const auto partial = MigrateIni(old_water, {{"wet_streets", "0"}});
        Check(VR::Ini::ValueOf(partial, "water_reflections") == "1",
              "a water choice that already matches disk survives a pending migration");
        const auto automatic = MigrateIni(partial, {{"water_reflections", std::nullopt}, {"wet_streets", std::nullopt}});
        Check(!VR::Ini::ValueOf(automatic, "water_reflections") && !VR::Ini::ValueOf(automatic, "wet_streets") &&
                  MigrateIni(automatic) == automatic, "resetting Wetness leaves both keys on Auto");
        const auto seen_true = old_water + "newgfx_096_seen=true\r\n";
        Check(MigrateIni(seen_true) == seen_true, "accept the runtime's text spelling of a completed migration");
        // The included runtime: always "auto" (it reads ipd_mm itself, live). A custom one: the
        // number, Standard 64 mm (an older runtime reads "auto" as 0).
        Check(NativeIpd("ipd_mm=60", true) == L"auto" && NativeIpd("", true) == L"auto",
              "included runtime reads the eye distance itself");
        Check(std::stof(NativeIpd("ipd_mm=60", false)) == 0.06f,
              "chosen eye distance reaches a custom runtime");
        Check(std::stof(NativeIpd("ipd_mm=0", false)) == 0.064f &&
                  std::stof(NativeIpd("", false)) == 0.064f &&
                  std::stof(NativeIpd("ipd_mm=abc", false)) == 0.064f,
              "a custom runtime's Standard eye distance is 64 mm");
        auto runtime = root / "shadps4.exe";
        std::ofstream(runtime) << "test";
        const auto patch_template = Read(ExecutableDirectory() / "bbvr_patch.xml");
        {
            const auto eboot_path = game_folder / "eboot.bin";
            const auto without_sfx = PatchIntelSfx(PatchForVersion(patch_template, "01.00"),
                                                   eboot_path, false);
            Check(PatchIntelSfx(without_sfx, root / "missing.bin", false) == without_sfx,
                  "disabled SFX workaround requires no new executable reads");
            Throws([&] { PatchIntelSfx(without_sfx, eboot_path, true); },
                   "SFX workaround refuses mismatched function bytes");
            const std::string prologue = "\x55\x48\x89\xe5\x41\x57\x41\x56\x41\x55"
                                         "\x41\x54\x53\x48\x81\xec\xc8\x00\x00\x00"s;
            {
                std::fstream eboot(eboot_path, std::ios::binary | std::ios::in | std::ios::out);
                eboot.seekp(std::streamoff(0x28eb0) + 0x02cf83e0 - 0x400000);
                eboot.write(prologue.data(), prologue.size());
            }
            for (const auto &version : {"01.00", "01.09"}) {
                const auto xml = PatchForVersion(patch_template, version);
                const auto enabled = PatchIntelSfx(xml, eboot_path, true);
                pugi::xml_document document;
                document.load_string(enabled.c_str());
                const auto entry = document.child("Patch").find_child_by_attribute(
                    "Metadata", "Name", "Intel 12th Gen+ SFX workaround");
                Check(entry.attribute("isEnabled").as_bool() &&
                          std::string(entry.attribute("AppVer").value()) == version &&
                          std::string(entry.child("PatchList").child("Line").attribute("Value").value()) == "c3",
                      "SFX opt-in enables the correct versioned patch");
                Check(PatchIntelSfx(enabled, eboot_path, false) ==
                          PatchIntelSfx(xml, eboot_path, false),
                      "disabling SFX changes only its enable flag");
            }
            for (const auto &version : {"01.00", "01.09"}) {
                const auto xml = PatchForVersion(patch_template, version);
                const auto on = PatchNonAggro(xml, true);
                pugi::xml_document document;
                document.load_string(on.c_str());
                const auto entry = document.child("Patch").find_child_by_attribute(
                    "Metadata", "Name", "Non-aggro mode");
                Check(entry.attribute("isEnabled").as_bool() &&
                          std::string(entry.attribute("AppVer").value()) == version &&
                          std::string(entry.child("PatchList").child("Line").attribute("Address").value()) ==
                              "0x0593E88E",
                      "Non-aggro mode enables the stealth flag");
                Check(PatchNonAggro(on, false) == PatchNonAggro(xml, false),
                      "disabling Non-aggro mode changes only its enable flag");
                pugi::xml_document off;
                off.load_string(PatchNonAggro(xml, false).c_str());
                Check(!off.child("Patch")
                           .find_child_by_attribute("Metadata", "Name", "Non-aggro mode")
                           .attribute("isEnabled")
                           .as_bool(),
                      "Non-aggro mode is off unless chosen");
            }
            Throws([&] { PatchNonAggro("<Patch><Metadata Name='Other'/></Patch>", true); },
                   "Non-aggro mode refuses a package without it");
            const auto old_package = "<Patch><Metadata Name='Other'/></Patch>"s;
            for (int damage = 0; damage < 5; ++damage) {
                pugi::xml_document damaged;
                damaged.load_string(patch_template.c_str());
                auto entry = damaged.child("Patch").find_child_by_attribute(
                    "Metadata", "Name", "Intel 12th Gen+ SFX workaround");
                auto line = entry.child("PatchList").child("Line");
                if (damage == 0) entry.remove_attribute("isEnabled");
                if (damage == 1) line.attribute("Address").set_value("0x02434ADA");
                if (damage == 2) line.attribute("Value").set_value("90");
                if (damage == 3) entry.child("PatchList").append_copy(line);
                if (damage == 4) damaged.child("Patch").append_copy(entry);
                std::ostringstream xml;
                damaged.save(xml);
                Throws([&] { PatchIntelSfx(xml.str(), eboot_path, true); },
                       "malformed SFX patch is refused despite matching game signature");
            }
            Check(PatchIntelSfx(old_package, eboot_path, false) == old_package,
                  "older package works with workaround off");
            Throws([&] { PatchIntelSfx(old_package, eboot_path, true); },
                   "opt-in rejects packages missing the workaround");
        }
        Check(!patch_template.empty(), "shipped patch template available");
        std::ofstream(root / "bbvr_patch.xml", std::ios::binary) << patch_template;
        SetEnvironmentVariableW(L"VR_TEST_LEAK", L"1");
        SetEnvironmentVariableW(L"FAST_TOGGLES", L"0");
        Paths paths{root / "app"};
        Profile profile{PathText(game_folder), PathText(save_home), PathText(runtime)};
        Check(profile.intel_sfx_workaround, "SFX workaround defaults on");
        Check(!profile.non_aggro, "Non-aggro mode defaults off");
        ValidateSetup(profile);
        Check(!fs::exists(paths.Config()), "setup validation does not write configuration");
        auto incomplete = profile;
        incomplete.game.clear();
        Throws([&] { ValidateSetup(incomplete); }, "empty game rejected during setup");
        incomplete = profile;
        incomplete.saves.clear();
        Throws([&] { ValidateSetup(incomplete); }, "empty save home rejected during setup");
        incomplete = profile;
        incomplete.runtime.clear();
        Throws([&] { ValidateSetup(incomplete); }, "missing runtime rejected during setup");
        Write(paths.Ini(), "ipd_mm=60\nlaser=1\n");
        constexpr uint64_t GiB = 1ull << 30;
        Check(StartupStereo(8 * GiB) == StereoPreset::Native &&
                  StartupStereo(6 * GiB) == StereoPreset::Native &&
                  StartupStereo(8 * GiB - 1) == StereoPreset::Native,
              "8 GiB and smaller GPUs start in native stereo (DIBR is the player's choice)");
        {
            SystemInfo big{};
            big.vram = 16 * GiB;
            SystemInfo limited = big;
            limited.multiview_known = limited.multiview_limited = true;
            SystemInfo unknown = big;
            Check(StartupStereo(big) == StereoPreset::Native &&
                      StartupStereo(limited) == StereoPreset::Dibr &&
                      StartupStereo(unknown) == StereoPreset::Native,
                  "Auto uses DIBR when the driver lacks native stereo's multiview");
            Profile manual{};
            manual.stereo = StereoChoice::Native;
            Check(manual.SelectedStereo(limited) == StereoPreset::Native,
                  "an explicit Native choice is kept even with limited multiview");
            SystemInfo older = big;
            older.amd = older.auto_layers = older.auto_dibr = true; // an RX 5700 XT
            Profile automatic{};
            Check(StartupStereo(older) == StereoPreset::Dibr &&
                      automatic.SelectedStereo(older) == StereoPreset::Dibr &&
                      manual.SelectedStereo(older) == StereoPreset::Native,
                  "an AMD RX 400 to RX 5000 starts in DIBR on Auto; an explicit Native choice stays");
        }
        {
            // A new player's upscaler by card (StartupUpscaler): 1 DLSS / 3 FSR 3.1.5 and the
            // preset (1 Balanced, 2 Quality, 4 Ultra Quality, 5 Balanced Plus).
            const auto card = [&](const char *name, uint64_t vram, bool nvidia) {
                SystemInfo s{};
                s.gpu = name;
                s.vram = vram;
                s.nvidia = nvidia;
                s.amd = !nvidia;
                return StartupUpscaler(s);
            };
            const auto nv = [&](const char *name, uint64_t gb) { return card(name, gb * GiB, true); };
            const auto amd = [&](const char *name, uint64_t gb) { return card(name, gb * GiB, false); };
            const StartupUpscale off{}, dlss_b{1, 1}, dlss_q{1, 2}, dlss_uq{1, 4};
            const StartupUpscale fsr_uq{3, 4}, fsr_q{3, 2}, fsr_bp{3, 5};
            Check(nv("NVIDIA GeForce RTX 5090", 32) == dlss_uq &&
                      nv("NVIDIA GeForce RTX 5080", 16) == dlss_q &&
                      nv("NVIDIA GeForce RTX 5070 Ti", 16) == dlss_q &&
                      nv("NVIDIA GeForce RTX 5070", 12) == dlss_q &&
                      nv("NVIDIA GeForce RTX 4090", 24) == dlss_q &&
                      nv("NVIDIA GeForce RTX 4080 SUPER", 16) == dlss_q &&
                      nv("NVIDIA GeForce RTX 4080", 16) == dlss_q,
                  "RTX 5090 starts with DLSS Ultra Quality, RTX 5070 / 5080 / 4080 / 4090 with DLSS "
                  "Quality");
            Check(nv("NVIDIA GeForce RTX 4070 Ti SUPER", 16) == dlss_b &&
                      nv("NVIDIA GeForce RTX 4070 SUPER", 12) == dlss_b &&
                      nv("NVIDIA GeForce RTX 3060", 12) == dlss_b &&
                      nv("NVIDIA GeForce RTX 4060 Ti", 16) == dlss_b &&
                      nv("NVIDIA GeForce RTX 3090", 24) == dlss_b &&
                      nv("NVIDIA GeForce RTX 3080 Ti", 12) == dlss_b &&
                      card("NVIDIA GeForce RTX 3080", 10 * GiB - GiB / 4, true) == dlss_b &&
                      nv("NVIDIA GeForce RTX 5060 Ti", 16) == dlss_b,
                  "other RTX 30-series and newer with 10 GB or more start with DLSS Balanced");
            Check(nv("NVIDIA GeForce RTX 3070", 8) == off && nv("NVIDIA GeForce RTX 4060", 8) == off &&
                      nv("NVIDIA GeForce RTX 5060", 8) == off &&
                      nv("NVIDIA GeForce RTX 2080 Ti", 11) == off &&
                      nv("NVIDIA GeForce GTX 1080 Ti", 11) == off && nv("NVIDIA TITAN RTX", 24) == off,
                  "smaller cards, RTX 20-series and older start without an upscaler");
            Check(nv("NVIDIA GeForce RTX 5090 Laptop GPU", 24) == dlss_b &&
                      nv("NVIDIA GeForce RTX 4080 Laptop GPU", 12) == dlss_b &&
                      nv("NVIDIA GeForce RTX 4060 Laptop GPU", 8) == off,
                  "a laptop chip gets DLSS Balanced at most");
            Check(amd("AMD Radeon RX 9070 XT", 16) == fsr_uq && amd("AMD Radeon RX 9070", 16) == fsr_uq &&
                      amd("AMD Radeon RX 7900 XTX", 24) == fsr_uq &&
                      amd("AMD Radeon RX 7900 XT", 20) == fsr_uq &&
                      amd("AMD Radeon RX 7900 GRE", 16) == fsr_q &&
                      amd("AMD Radeon RX 9070 GRE", 12) == fsr_q,
                  "RX 9070 and RX 7900 start with FSR 3.1.5 Ultra Quality, their GRE with Quality");
            Check(amd("AMD Radeon RX 7800 XT", 16) == fsr_q && amd("AMD Radeon RX 6950 XT", 16) == fsr_q &&
                      amd("AMD Radeon RX 6800", 16) == fsr_q,
                  "RX 7800 and RX 6800 / 6900 / 6950 start with FSR 3.1.5 Quality");
            Check(amd("AMD Radeon RX 7700 XT", 12) == fsr_bp && amd("AMD Radeon RX 6700 XT", 12) == fsr_bp &&
                      amd("AMD Radeon RX 6750 XT", 12) == fsr_bp &&
                      amd("AMD Radeon RX 9060 XT", 16) == fsr_bp,
                  "RX 9060, RX 7700 and RX 6700 / 6750 start with FSR 3.1.5 Balanced Plus");
            Check(amd("AMD Radeon RX 7600 XT", 16) == off && amd("AMD Radeon RX 6650 XT", 8) == off &&
                      amd("AMD Radeon RX 9060 XT", 8) == off && amd("AMD Radeon RX 5700 XT", 8) == off &&
                      amd("AMD Radeon RX 7900M", 16) == off && amd("AMD Radeon RX 6800S", 8) == off &&
                      amd("AMD Radeon RX Vega 64", 8) == off,
                  "weaker AMD cards and mobile chips start without an upscaler");
            SystemInfo intel{};
            intel.gpu = "Intel(R) Arc(TM) A770 Graphics";
            intel.vram = 16 * GiB;
            SystemInfo integrated = intel;
            integrated.nvidia = integrated.integrated = true;
            Check(StartupUpscaler(intel) == off && StartupUpscaler(integrated) == off,
                  "Intel and integrated graphics start without an upscaler");
            SystemInfo rtx5080{};
            rtx5080.gpu = "NVIDIA GeForce RTX 5080";
            rtx5080.vram = 16 * GiB;
            rtx5080.nvidia = true;
            SystemInfo rx7800 = rtx5080;
            rx7800.gpu = "AMD Radeon RX 7800 XT";
            rx7800.nvidia = false;
            rx7800.amd = true;
            auto want_dlss = DefaultGraphics, want_fsr = DefaultGraphics;
            want_dlss[2] = 1;
            want_dlss[3] = 2;
            want_fsr[2] = 3;
            want_fsr[4] = 2;
            // A new player's foveation (StartupFoveation): Light (3) on the Quality / Ultra
            // Quality cards, else Auto (-1).
            const auto fov = [&](const char *name, uint64_t gb, bool nvidia) {
                SystemInfo s{};
                s.gpu = name;
                s.vram = gb * GiB;
                s.nvidia = nvidia;
                s.amd = !nvidia;
                return StartupFoveation(s);
            };
            Check(fov("NVIDIA GeForce RTX 5090", 32, true) == 3 &&
                      fov("NVIDIA GeForce RTX 5080", 16, true) == 3 &&
                      fov("NVIDIA GeForce RTX 5070", 12, true) == 3 &&
                      fov("NVIDIA GeForce RTX 4090", 24, true) == 3 &&
                      fov("NVIDIA GeForce RTX 4080 SUPER", 16, true) == 3 &&
                      fov("AMD Radeon RX 9070 XT", 16, false) == 3 &&
                      fov("AMD Radeon RX 7900 GRE", 16, false) == 3 &&
                      fov("AMD Radeon RX 7800 XT", 16, false) == 3 &&
                      fov("AMD Radeon RX 6800", 16, false) == 3,
                  "a new player's fast card starts on Light foveation");
            Check(fov("NVIDIA GeForce RTX 4070 Ti SUPER", 16, true) == -1 &&
                      fov("NVIDIA GeForce RTX 3060", 12, true) == -1 &&
                      fov("NVIDIA GeForce RTX 5090 Laptop GPU", 24, true) == -1 &&
                      fov("NVIDIA GeForce RTX 4060", 8, true) == -1 &&
                      fov("AMD Radeon RX 7700 XT", 12, false) == -1 &&
                      fov("AMD Radeon RX 6650 XT", 8, false) == -1 && StartupFoveation(intel) == -1 &&
                      StartupFoveation(integrated) == -1,
                  "every other card keeps Auto foveation");
            // A new player's far shadows (StartupReducedShadows): Reduced written on a card with
            // 8 GB or less or integrated graphics, nothing on a bigger card or unknown memory.
            const auto shadows = [&](uint64_t vram) {
                SystemInfo s{};
                s.gpu = "card";
                s.vram = vram;
                s.nvidia = true;
                return StartupReducedShadows(s);
            };
            Check(shadows(8 * GiB) && shadows(8 * GiB - GiB / 8) && shadows(6 * GiB) &&
                      StartupReducedShadows(integrated) && !shadows(10 * GiB - GiB / 4) &&
                      !shadows(12 * GiB) && !shadows(0) &&
                      [] {
                          SystemInfo none{};
                          none.integrated = true;
                          return !StartupReducedShadows(none);
                      }(),
                  "8 GB or less starts on Reduced far shadows; 10 GB and up or unknown: nothing");
            Check(StartupGraphics(rtx5080) == want_dlss && StartupGraphics(rx7800) == want_fsr &&
                      StartupGraphics(intel) == DefaultGraphics &&
                      ParseGraphics(GraphicsText(StartupGraphics(rtx5080))) == want_dlss,
                  "a new player's graphics_config: the game's defaults with that upscaler and preset");
        }
        {
            // The Controls page (controls_guide.h): the game's controls Config from vr.ini, its
            // defaults for absent keys (vr_session.h left_scheme 2, inject on, the arc on).
            std::map<std::string, std::string> ini;
            const ControlsGuide::Lookup value = [&](const std::string &key) -> std::optional<std::string> {
                const auto it = ini.find(key);
                return it == ini.end() ? std::nullopt : std::optional<std::string>{it->second};
            };
            const auto meta = ControlsGuide::ConfigFrom(value, false);
            const auto steam = ControlsGuide::ConfigFrom(value, true);
            Check(meta.layout.left_scheme == 2 && meta.layout.inject && meta.quick_arc &&
                      meta.belt_vial && meta.swings && meta.melee_physical &&
                      meta.physical_evasion && meta.snap_turn_deg == 30.0f && !meta.layout.y_menu &&
                      !meta.layout.menu_runtime && steam.layout.y_menu && steam.layout.menu_runtime,
                  "the Controls page reads the game's defaults; Y is the menu on SteamVR only");
            ini["controls"] = "classic";
            const auto classic = ControlsGuide::ConfigFrom(value, true);
            ini = {{"y_menu", "off"}, {"left_scheme", "1"}, {"snap_turn_deg", "0"}};
            const auto off = ControlsGuide::ConfigFrom(value, true);
            ini = {{"y_menu", "on"}};
            const auto on = ControlsGuide::ConfigFrom(value, false);
            Check(classic.layout.classic && !classic.layout.y_menu && !off.layout.y_menu &&
                      off.layout.left_scheme == 1 && off.snap_turn_deg == 0.0f && on.layout.y_menu,
                  "classic, scheme 1 and y_menu=off keep Menu; y_menu=on makes Y the menu anywhere");
            ini.clear();
            bool sections = true, expanded = true;
            // The same Quest controllers use different menu buttons on Meta vs SteamVR / VDXR.
            for (const auto runtime : {"Oculus", "SteamVR/OpenXR", "Virtual Desktop", "VDXR"}) {
                for (const auto mode : {"auto", "on", "off"}) {
                    ini = {{"y_menu", mode}};
                    const bool y_menu = std::string_view(mode) == "on" ||
                        (std::string_view(mode) == "auto" && std::string_view(runtime) != "Oculus");
                    bool y_options = false;
                    for (const auto &s : ControlsGuide::Controls(ControlsGuide::Platform::Quest, value, runtime))
                        for (const auto &l : s.lines)
                            y_options |= l.control == "Y" && l.title == "Options";
                    Check(y_options == y_menu, "Quest menu help follows the runtime and y_menu override");
                    for (const auto &s : ControlsGuide::Interactions(ControlsGuide::Platform::Quest, value, runtime))
                        for (const auto &l : s.lines)
                            if (l.title == "VR settings")
                                Check((l.help.find("(Y)") != std::string::npos) == y_menu,
                                      "interaction guide names Y only when it opens the menu");
                }
            }
            ini.clear();
            for (int p = 0; p < ControlsGuide::PlatformCount; ++p) {
                const auto platform = static_cast<ControlsGuide::Platform>(p);
                const auto controls = ControlsGuide::Controls(platform, value);
                const auto play = ControlsGuide::Interactions(platform, value);
                for (bool menus : {false, true}) {
                    const auto diagram = ControlsGuide::Diagram(platform, value, menus);
                    bool matches = !diagram.empty();
                    for (const auto& button : diagram) {
                        matches = matches && button.x >= 0 && button.x <= 240 &&
                                  button.y >= 0 && button.y <= 440;
                        const auto& lines = controls[menus ? 1 : 0].lines;
                        matches = matches && std::any_of(lines.begin(), lines.end(),
                            [&](const ControlsGuide::Line& line) {
                                return line.control == button.line.control &&
                                       line.title == button.line.title &&
                                       line.help == button.line.help;
                            });
                    }
                    Check(matches, "controller artwork uses the same mappings as the text guide");
                }
                sections = sections && controls.size() == 3 && !play.empty() &&
                           !ControlsGuide::PlatformNote(platform).empty();
                for (const auto *list : {&controls, &play}) {
                    for (const auto &s : *list) {
                        sections = sections && !s.lines.empty();
                        for (const auto &l : s.lines) {
                            for (const std::string *t : {&l.control, &l.title, &l.sub, &l.help})
                                expanded = expanded && t->find('{') == std::string::npos;
                            sections = sections && !l.title.empty();
                        }
                    }
                }
            }
            Check(sections, "every headset's Controls page lists each section's controls and how to play");
            Check(expanded, "the Controls page's words have every {entry} and {control} filled in");
            for (const auto platform : {ControlsGuide::Platform::Psvr2, ControlsGuide::Platform::Frame,
                                        ControlsGuide::Platform::Index}) {
                const auto diagram = ControlsGuide::Diagram(platform, value, false, "Oculus");
                Check(std::any_of(diagram.begin(), diagram.end(), [](const auto& button) {
                    return button.line.title == "Options" &&
                           button.line.sub.find("recenter") != std::string::npos;
                }), "SteamVR controller tabs use their menu binding after a Quest runtime session");
            }
            // The VR interactions guide (2026-11-03): its topics, searchable, each setting a real
            // in-game row, the button names the controller's.
            {
                const auto guide = ControlsGuide::Interactions(ControlsGuide::Platform::Quest, value);
                const auto ps = ControlsGuide::Interactions(ControlsGuide::Platform::Psvr2, value);
                const auto count = [&](const std::vector<ControlsGuide::Section> &sections,
                                       const std::string &needle) {
                    int n = 0;
                    for (const auto &s : sections)
                        for (const auto &l : s.lines)
                            n += ControlsGuide::Finds(l, needle) ? 1 : 0;
                    return n;
                };
                bool rows = true, words = true;
                for (const auto &s : guide)
                    for (const auto &l : s.lines) {
                        rows = rows && (!l.row || VR::Page::RowIndex(l.row) >= 0);
                        words = words && !l.words.empty() && !l.help.empty();
                    }
                Check(rows && words, "every guide line has help, search words and a real row");
                for (const char *needle :
                     {"visceral", "lantern", "monocular", "unarmed", "two hand", "corpse", "molotov",
                      "music box", "bell", "shoulder", "lever", "throw", "vial", "dual",
                      "amygdalan arm", "boom hammer", "logarius wheel", "doll respec", "ladder",
                      "wheelchair", "skip cutscene"})
                    Check(count(guide, needle) > 0, (std::string("the guide finds ") + needle).c_str());
                // Kos Lights is a secret: never in the guide.
                Check(count(guide, "kos lights") == 0 && count(guide, "cigarette") == 0,
                      "the guide keeps Kos Lights a secret");
                Check(count(guide, "zzzz") == 0 && count(guide, "lant") > 0,
                      "the guide's search: word starts, nothing for nonsense");
                bool ps_names = false, quest_names = false;
                for (const auto &s : ps)
                    for (const auto &l : s.lines)
                        ps_names = ps_names || l.help.find("Circle") != std::string::npos;
                for (const auto &s : guide)
                    for (const auto &l : s.lines)
                        quest_names = quest_names || l.help.find("Circle") != std::string::npos;
                Check(ps_names && !quest_names, "the guide names the picked controller's buttons");
            }
            // Frame's Menus help names its own buttons (its X and Y are the left d-pad).
            const auto frame = ControlsGuide::Controls(ControlsGuide::Platform::Frame, value);
            bool frame_names = false, touch_names = false;
            for (const auto &l : frame[1].lines) {
                frame_names = frame_names || l.help.find("D-pad") != std::string::npos;
                touch_names = touch_names || l.help.find(" X does the same") != std::string::npos ||
                              l.help.find(" Y does the same") != std::string::npos;
            }
            Check(frame_names && !touch_names, "Frame's menu help names its d-pad, not X and Y");
            // PS VR2: the PlayStation names (SteamVR hands its Sense controllers over as Touch),
            // never Touch's letters, and no Menu button (Create and Options are SteamVR's).
            bool ps_names = false, ps_letters = false, ps_menu = false;
            for (const auto &s : ControlsGuide::Controls(ControlsGuide::Platform::Psvr2, value))
                for (const auto &l : s.lines) {
                    ps_names = ps_names || l.control == "Cross" || l.control == "Triangle";
                    ps_menu = ps_menu || l.control == "Menu" || l.control == "Create";
                    for (const char *touch : {"A", "B", "X", "Y"})
                        ps_letters = ps_letters || l.control == touch;
                }
            const auto ps_grip = ControlsGuide::RowWords("grip_adjust", ControlsGuide::Platform::Psvr2, value);
            Check(ps_names && !ps_letters && !ps_menu &&
                      ps_grip.label.find("Cross") != std::string::npos,
                  "PS VR2's Controls page and settings name Cross, Circle, Square and Triangle");
            Check(ControlsGuide::PlatformOf("SteamVR/PlayStation VR2") == ControlsGuide::Platform::Psvr2 &&
                      ControlsGuide::PlatformOf("Meta Quest 3") == ControlsGuide::Platform::Quest &&
                      ControlsGuide::PlatformOf("Steam Frame") == ControlsGuide::Platform::Frame &&
                      ControlsGuide::PlatformOf("SteamVR/lighthouse") == ControlsGuide::Platform::Quest,
                  "the Controls page opens on the last headset's controllers");
            // Two hands follow the player's scheme.
            const auto two_hands = [&] {
                for (const auto &s : ControlsGuide::Interactions(ControlsGuide::Platform::Quest, value))
                    for (const auto &l : s.lines)
                        if (l.title == "Two-hand a weapon")
                            return l.help;
                return std::string{};
            };
            const auto original = two_hands();
            ini["two_hand_optimized_controls"] = "1";
            const auto optimized = two_hands();
            Check(original.find("left grip") != std::string::npos &&
                      optimized.find("left trigger") != std::string::npos,
                  "the Controls page's two-handed help follows the optimized controls setting");
            // The how-to and the controller page must agree, including swapped sticks and
            // settings that leave the optimized preference saved but inactive.
            for (int p = 0; p < ControlsGuide::PlatformCount; ++p) {
                const auto platform = static_cast<ControlsGuide::Platform>(p);
                for (bool swapped : {false, true}) {
                    ini = {{"two_hand_optimized_controls", "1"},
                           {"swap_sticks", swapped ? "1" : "0"}};
                    const auto help = [&](const char *title) {
                        for (const auto &s : ControlsGuide::Interactions(platform, value))
                            for (const auto &l : s.lines)
                                if (l.title == title)
                                    return l.help;
                        return std::string{};
                    };
                    using Family = VR::ControlsView::Family;
                    constexpr Family families[]{Family::Touch, Family::Sense, Family::Frame,
                                                Family::Index, Family::Vive, Family::Wmr};
                    const auto parts = VR::ControlsView::PartsOf(
                        families[p], VR::ControlsView::DefaultProfile(families[p]),
                        platform != ControlsGuide::Platform::Quest);
                    // A stick's label and its click can differ (right stick / R3 on PS VR2).
                    const auto transform_control = VR::ControlsView::NameOf(
                        parts, VR::Thumbs::Physical(VR::Thumbs::Control::ClickR, swapped));
                    Check(!transform_control.empty() &&
                              help("Transform").find(transform_control) != std::string::npos &&
                              help("Simon's Bowblade").find(transform_control) != std::string::npos &&
                              help("Dual blades").find(transform_control) != std::string::npos &&
                              help("Lock on").find("instead of locking on") != std::string::npos,
                          "optimized guide uses the controller page's transform button everywhere");
                    Check(help("Transform by hand").empty() && help("Other transforms").empty() &&
                              help("Spear grip").empty() &&
                              help("Simon's Bowblade").find("half a second") == std::string::npos &&
                              help("Grip guide").find("no right-grip hold") != std::string::npos &&
                              help("Weapon specials").find("right trigger to spin") != std::string::npos,
                          "optimized guide does not teach inactive grip transforms or left-trigger spin");
                    Check(help("Two-hand a weapon").find("release it to let go") != std::string::npos,
                          "optimized Hold guide explains trigger release");
                    ini["two_hand_trigger_hold"] = "0";
                    Check(help("Two-hand a weapon").find("press it again to let go") != std::string::npos,
                          "optimized Toggle guide explains the second press");
                    for (const auto &[key, setting] :
                         std::initializer_list<std::pair<const char *, const char *>>{
                             {"controls", "classic"}, {"right_grip", "draw"},
                             {"two_hand_optimized_controls", "0"}}) {
                        ini[key] = setting;
                        Check(help("Lock on").find("to lock on to an enemy") != std::string::npos &&
                                  help("Two-hand a weapon").find("left trigger") == std::string::npos,
                              "inactive optimized preference does not advertise optimized bindings");
                        if (std::string_view(key) != "two_hand_optimized_controls")
                            Check(help("Dual blades").find("squeeze the left grip to split") !=
                                      std::string::npos && help("Transform by hand").empty(),
                                  "Classic and Draw guides use the left grip for paired blades");
                        ini.erase(key);
                        ini["two_hand_optimized_controls"] = "1";
                    }
                }
            }
            ini.clear();
            // BBVR_DUMP_CONTROLS=1: the page's words for every headset with the defaults, to read.
            if (std::getenv("BBVR_DUMP_CONTROLS")) {
                ini.clear();
                for (int p = 0; p < ControlsGuide::PlatformCount; ++p) {
                    const auto platform = static_cast<ControlsGuide::Platform>(p);
                    std::printf("=== %s: %s\n", ControlsGuide::PlatformNames[p],
                                ControlsGuide::PlatformNote(platform).c_str());
                    auto all = ControlsGuide::Controls(platform, value);
                    for (auto &s : ControlsGuide::Interactions(platform, value))
                        all.push_back(std::move(s));
                    for (const auto &s : all) {
                        std::printf("-- %s (%s)\n", s.title.c_str(), s.note.c_str());
                        for (const auto &l : s.lines)
                            std::printf("[%s] %s | %s :: %s\n", l.control.c_str(), l.title.c_str(),
                                        l.sub.c_str(), l.help.c_str());
                    }
                }
            }
        }
        Check(StartupStereo(0) == StereoPreset::Native &&
                  StartupStereo(8 * GiB + 1) == StereoPreset::Native &&
                  StartupStereo(12 * GiB) == StereoPreset::Native,
              "unknown and larger GPUs keep native stereo");
        SetEnvironmentVariableW(L"BBVR_BENCHMARK_INPUT_LOCK", L"1");
        SetEnvironmentVariableW(L"BBVR_SHADER_CACHE_ROOT", L"C:/unrelated-cache");
        {
            const auto base_game = root/"base game";
            fs::create_directories(base_game/"sce_sys");
            fs::copy_file(GameExecutable(Path(profile.game)), base_game/"eboot.bin");
            WriteParamSfo(base_game/"sce_sys"/"param.sfo", "01.00", "CUSA00900");
            const auto folder = root/"separate DLC"/"renamed";
            fs::create_directories(folder/"sce_sys");
            WriteParamSfo(folder/"sce_sys"/"param.sfo", "01.00", "CUSA00900", "", "",
                          "EP9000-CUSA00900_00-SPEXPANSIONDLC03");
            auto choice=profile; choice.game=PathText(base_game); choice.dlc=PathText(folder);
            auto found=ResolveDlc(paths,choice);
            Check(found.valid && found.root==folder && found.message.find("DLC FOUND: The Old Hunters")!=std::string::npos,
                  "explicit extracted DLC is validated and detected");
            choice.Save(paths);
            Check(Profile::Load(paths).dlc==choice.dlc, "DLC selection persists");
            choice.dlc=PathText(root/"missing DLC");
            Check(!ResolveDlc(paths,choice).valid, "bad explicit DLC selection is actionable");
            // DLC is optional: a bad folder, or a config without (or with an empty) add-on
            // folder, never blocks the launch (0.8.7: "Choose a folder or file first.").
            const auto before=Read(paths.Config());
            fs::create_directories(paths.User());
            for (const char* general : {R"({"General":{"home_dir":"x"}})",
                                        R"({"General":{"addon_install_dir":""}})",
                                        R"({"General":{"addon_install_dir":null}})"}) {
                Write(paths.Config(), general, Read(paths.Config()));
                auto automatic=choice; automatic.dlc.clear();
                bool resolved=false;
                try { resolved=ResolveDlc(paths,automatic).valid; } catch (const std::exception&) {}
                Check(resolved, "no or an empty addon_install_dir is no configured folder");
                Check(!Prepare(choice,paths).executable.empty(), "a bad DLC folder does not block launch");
                Check(!Json::parse(Read(paths.Config()))["General"].contains("addon_install_dir") ||
                          Json::parse(Read(paths.Config()))["General"]["addon_install_dir"]==
                              Json::parse(general)["General"]["addon_install_dir"],
                      "a bad DLC folder leaves config.json's add-on folder as it was");
            }
            Write(paths.Config(), before, Read(paths.Config()));
            // The Game of the Year Edition is said to include The Old Hunters only with its
            // three areas really in the game folder; never blocked either way.
            auto bundled=profile; bundled.dlc.clear();
            const auto bundled_game=fs::absolute(GameExecutable(Path(profile.game))).parent_path();
            const auto missing=ResolveDlc(paths,bundled);
            Check(missing.valid && !missing.found &&
                      missing.message.find("The Old Hunters not found")!=std::string::npos &&
                      missing.message.find("DLC INCLUDED")==std::string::npos,
                  "bundled edition without its DLC areas is not called included");
            const auto map=bundled_game/"dvdroot_ps4"/"map";
            fs::create_directories(map/"MapStudio");
            for (const char* area : {"m34_00_00_00","m35_00_00_00","m36_00_00_00"}) {
                const auto partial=ResolveDlc(paths,bundled);
                Check(partial.valid && !partial.found, "bundled edition with only some DLC areas is not included");
                fs::create_directories(map/area);
                std::ofstream(map/"MapStudio"/(std::string(area)+".msb.dcx"),std::ios::binary) << "x";
            }
            const auto included=ResolveDlc(paths,bundled);
            Check(included.valid && included.found &&
                      included.message.find("DLC INCLUDED: The Old Hunters")!=std::string::npos,
                  "bundled edition with its three DLC areas says DLC INCLUDED");
            bundled.dlc=choice.dlc;
            const auto bundled_status=ResolveDlc(paths,bundled);
            Check(bundled_status.valid && bundled_status.found,
                  "stale separate DLC does not block bundled edition");
            Check(!Prepare(bundled,paths).executable.empty(), "bundled edition with a stale DLC folder launches");
            // BBVR_DLC_CHECK_GAME=<game folder>: what the launcher says about a real game folder's
            // DLC (read only; a scratch data folder, nothing of the player's is used).
            if (const char* real = std::getenv("BBVR_DLC_CHECK_GAME")) {
                auto probe=profile; probe.game=real; probe.dlc.clear();
                const auto status=ResolveDlc(Paths{root/"dlc-check"},probe);
                std::printf("DLC check %s: valid=%d found=%d root=%s\n%s\n", real, int(status.valid),
                            int(status.found), PathText(status.root).c_str(), status.message.c_str());
            }
            profile.Save(paths);
        }
        auto plan = Prepare(profile, paths, StereoPreset::Native);
        Check(plan.game_support_info.find("APP_VER 01.00") != std::string::npos &&
                  plan.game_support_info.find("SHA-256 unavailable") == std::string::npos,
              "launch plan captures readable game identity");
        const auto captured_game_info = plan.game_support_info;
        Check(captured_game_info.find("Skip dangerous SFX ON") != std::string::npos,
              "support report records enabled SFX protection");
        Check(captured_game_info.ends_with("Skip dangerous SFX ON\n") &&
                  captured_game_info.find("\n\nSkip dangerous SFX") == std::string::npos,
              "SFX support entry is one complete line before Windows details");
        const auto original_game_hash = GameSupportInfo(game_folder / "eboot.bin");
        {
            auto opted_out = profile;
            opted_out.intel_sfx_workaround = false;
            const auto off = Prepare(opted_out, paths, StereoPreset::Native);
            Check(off.game_support_info.find("Skip dangerous SFX OFF") != std::string::npos,
                  "support report records SFX opt-out");
            pugi::xml_document off_patch;
            off_patch.load_string(Read(paths.User() / "patches" / "bbvr_patch.xml").c_str());
            Check(!off_patch.child("Patch").find_child_by_attribute(
                       "Metadata", "Name", "Intel 12th Gen+ SFX workaround")
                       .attribute("isEnabled").as_bool(),
                  "opt-out disables the patch in the actual launch copy");
            Prepare(profile, paths, StereoPreset::Native);
            Check(GameSupportInfo(game_folder / "eboot.bin") == original_game_hash &&
                      Read(root / "bbvr_patch.xml") == patch_template,
                  "SFX toggles never edit the game executable or package template");
        }

        int argc{};
        auto argv = CommandLineToArgvW(plan.command.c_str(), &argc);
        Check(argc == 8, "launch argument count");
        Check(std::wstring(argv[1]) == L"--openxr" &&
                  fs::equivalent(fs::path(argv[7]), game_folder / "eboot.bin"),
              "launch arguments point at the game, spaces and all");
        Check(std::wstring(argv[4]) == L"-p" && std::wstring(argv[5]) == L"bbvr_patch.xml" &&
                  Read(paths.User() / "patches" / "bbvr_patch.xml") ==
                      PatchForVersion(patch_template, "01.00"),
              "headset patch passed by name from user/patches");
        LocalFree(argv);
        auto environment = [](const LaunchPlan &p) {
            std::set<std::wstring> out;
            for (const wchar_t *at = p.environment.data(); *at; at += wcslen(at) + 1)
                out.insert(at);
            return out;
        };
        auto env = environment(plan);
        Check(std::none_of(env.begin(), env.end(), [](const auto& entry) {
            return entry.starts_with(L"BBVR_BENCHMARK") || entry.starts_with(L"BBVR_SHADER_CACHE_ROOT=");
        }), "normal launch strips inherited benchmark input lock and cache override");
        SetEnvironmentVariableW(L"BBVR_BENCHMARK_INPUT_LOCK", nullptr);
        SetEnvironmentVariableW(L"BBVR_SHADER_CACHE_ROOT", nullptr);

        Check(env.contains(L"VR_NATIVE_STEREO=1") && env.contains(L"VR_NATIVE_ASYMMETRIC=1") &&
                  env.contains(L"VR_FAST_MUTEX=1") && env.contains(L"VR_COPY_MERGE_KB=16") &&
                  env.contains(L"VR_DIRECT_BUFFER_METADATA=0"),
              "launch runs the full native asymmetric preset");
        // The included runtime reads ipd_mm itself (live): "auto", never a number that overrides it.
        Check(env.contains(L"VR_NATIVE_IPD=auto"), "native eye distance: the included runtime's own");
        Check(!env.contains(L"VR_TEST_LEAK=1") && !env.contains(L"FAST_TOGGLES=0"),
              "inherited renderer switches stripped");
        Check(NativeViewsValue(NativeViews::Auto).empty() &&
                  NativeViewsValue(NativeViews::Multiview) == L"multiview" &&
                  NativeViewsValue(NativeViews::Layers) == L"layers",
              "native stereo eyes: Auto is the runtime's, a choice is always passed");
        Check(VR::NativeViews::LayersByDefault(0x1002, 0x73df, "AMD Radeon RX 6700") &&
                  VR::NativeViews::LayersByDefault(0x1002, 0x73bf, "AMD Radeon RX 6900 XT") &&
                  VR::NativeViews::LayersByDefault(0x1002, 0x73ff, "AMD Radeon RX 6600") &&
                  VR::NativeViews::LayersByDefault(0x1002, 0x743f, "AMD Radeon RX 6500 XT") &&
                  VR::NativeViews::LayersByDefault(0x1002, 0x0000, "AMD Radeon RX 6800M") &&
                  VR::NativeViews::LayersByDefault(0x1002, 0x731f, "AMD Radeon RX 5700 XT") &&
                  VR::NativeViews::LayersByDefault(0x1002, 0x67df, "Radeon RX 580 Series") &&
                  VR::NativeViews::LayersByDefault(0x1002, 0x6987, "AMD Radeon RX 640") &&
                  !VR::NativeViews::LayersByDefault(0x1002, 0x0000, "AMD Radeon RX 640") &&
                  !VR::NativeViews::LayersByDefault(0x1002, 0x744c, "AMD Radeon RX 7900 XTX") &&
                  !VR::NativeViews::LayersByDefault(0x1002, 0x7480, "AMD Radeon RX 7600") &&
                  !VR::NativeViews::LayersByDefault(0x1002, 0x7550, "AMD Radeon RX 9070 XT") &&
                  !VR::NativeViews::LayersByDefault(0x1002, 0x7590, "AMD Radeon RX 9060 XT") &&
                  !VR::NativeViews::LayersByDefault(0x10de, 0x2c02, "NVIDIA GeForce RTX 5080") &&
                  !VR::NativeViews::LayersByDefault(0x8086, 0x56a0, "Intel(R) Arc(TM) A770"),
              "layers by default on AMD RX 400 to RX 6000 only (RX 7000 and 9000 keep multiview)");
        {
            namespace V = VR::NativeViews;
            // Every Polaris and RDNA 1 chip in pci.ids, and the model in a name without a device id.
            bool older = true;
            for (const uint32_t id : {0x67c0u, 0x67dfu, 0x67e0u, 0x67efu, 0x67ffu, 0x6980u, 0x6987u,
                                      0x699fu, 0x6fdfu, 0x7310u, 0x731fu, 0x7340u, 0x7347u,
                                      0x734fu, 0x7360u, 0x7362u})
                older = older && V::DibrByDefault(0x1002, id, "");
            for (const char *name : {"AMD Radeon RX 480", "Radeon RX 470D", "AMD Radeon RX 580 2048SP",
                                     "AMD Radeon RX 590 GME", "AMD Radeon RX 550X",
                                     "AMD Radeon RX 5500 XT", "AMD Radeon RX 5700 XT"})
                older = older && V::DibrByDefault(0x1002, 0, name);
            Check(older, "DIBR by default on every AMD RX 400, RX 500 and RX 5000");
            bool others = false;
            for (const uint32_t id : {0x73a0u, 0x73dfu, 0x743fu, 0x744cu, 0x7480u, 0x7550u,
                                      0x7590u, 0x687fu})
                others = others || V::DibrByDefault(0x1002, id, "");
            for (const char *name : {"AMD Radeon RX 6700", "AMD Radeon RX 640", "AMD Radeon RX 7600",
                                     "AMD Radeon RX 9070 XT", "Radeon RX Vega",
                                     "AMD Radeon RX 4800"})
                others = others || V::DibrByDefault(0x1002, 0, name);
            others = others || V::DibrByDefault(0x10de, 0x67df, "NVIDIA GeForce RTX 5080") ||
                     V::DibrByDefault(0x8086, 0x56a0, "Intel(R) Arc(TM) A770");
            Check(!others, "RX 6000, 7000, 9000, Vega, NVIDIA and Intel keep native stereo on Auto");
        }
        {
            const auto views_of = [](const std::set<std::wstring> &e) {
                return std::count_if(e.begin(), e.end(), [](const auto &entry) {
                    return entry.starts_with(L"VR_NATIVE_VIEWS=");
                });
            };
            SetEnvironmentVariableW(L"VR_FREE_CAMERA", L"1");
            Check(!environment(Prepare(profile, paths)).contains(L"VR_FREE_CAMERA=1"),
                  "Inherited free camera cannot enable sightseeing");
            auto sightseeing = profile;
            sightseeing.free_camera = true;
            sightseeing.non_aggro = false;
            const auto free_plan = Prepare(sightseeing, paths);
            Check(environment(free_plan).contains(L"VR_FREE_CAMERA=1") &&
                      free_plan.game_support_info.find("Non-aggro mode WHILE FREE CAMERA") !=
                          std::string::npos,
                  "Free camera brings non-aggro only while it is on");
            Check(PatchNonAggro(Read(paths.User() / "patches" / "bbvr_patch.xml"), false) == Read(paths.User() / "patches" / "bbvr_patch.xml"),
                  "A free camera launch leaves the non-aggro patch to the player's own setting");
            SetEnvironmentVariableW(L"VR_FREE_CAMERA", nullptr);
            // Soft save for every player: a launch with a save names this launch's backup of it;
            // an inherited one never reaches the game.
            SetEnvironmentVariableW(L"VR_SOFT_SAVE_LAUNCHER_BACKUP", L"C:\\nowhere");
            auto saver = profile;
            const auto saver_home = root / "soft save home";
            const auto saver_file =
                saver_home / "1000" / "savedata" / "CUSA00207" / "SPRJ0005" / "userdata0000";
            fs::create_directories(saver_file.parent_path());
            std::ofstream(saver_file, std::ios::binary) << "hunter";
            saver.saves = PathText(saver_home);
            std::wstring named;
            for (const auto &entry : environment(Prepare(saver, paths)))
                if (entry.starts_with(L"VR_SOFT_SAVE_LAUNCHER_BACKUP="))
                    named = entry.substr(29);
            Check(!named.empty() && named != L"C:\\nowhere" &&
                      Read(fs::path(named) / "1000" / "savedata" / "CUSA00207" / "SPRJ0005" /
                           "userdata0000") == "hunter",
                  "a launch names its backup of the save to the game (soft save)");
            Check(!environment(Prepare(profile, paths)).contains(L"VR_SOFT_SAVE_LAUNCHER_BACKUP=C:\\nowhere"),
                  "an inherited soft save backup never reaches the game");
            SetEnvironmentVariableW(L"VR_SOFT_SAVE_LAUNCHER_BACKUP", nullptr);
            Prepare(profile, paths);
            // Auto passes nothing on any card: the runtime decides on the card it opens.
            SetEnvironmentVariableW(L"VR_NATIVE_VIEWS", L"layers");
            Check(views_of(environment(Prepare(profile, paths, StereoPreset::Native))) == 0,
                  "Auto native stereo eyes pass nothing, and an inherited value does not leak");
            auto multiview = profile;
            multiview.native_views = NativeViews::Multiview;
            const auto multiview_env = environment(Prepare(multiview, paths, StereoPreset::Native));
            Check(multiview_env.contains(L"VR_NATIVE_VIEWS=multiview") && views_of(multiview_env) == 1,
                  "a Multiview choice reaches the game, not the inherited value");
            SetEnvironmentVariableW(L"VR_NATIVE_VIEWS", L"multiview");
            auto layers = profile;
            layers.native_views = NativeViews::Layers;
            const auto layers_env = environment(Prepare(layers, paths, StereoPreset::Native));
            Check(layers_env.contains(L"VR_NATIVE_VIEWS=layers") && views_of(layers_env) == 1,
                  "a Layers choice reaches the game, not the inherited value");
            // DIBR names the chosen mode too: one shader cache profile across both kinds of
            // session (the runtime clears a cache written with another profile).
            const auto dibr_views = environment(Prepare(layers, paths, StereoPreset::Dibr));
            Check(dibr_views.contains(L"VR_NATIVE_VIEWS=layers") && views_of(dibr_views) == 1 &&
                      !dibr_views.contains(L"VR_NATIVE_STEREO=1"),
                  "DIBR passes the chosen eyes, and still no native stereo");
            SetEnvironmentVariableW(L"VR_NATIVE_VIEWS", nullptr);
        }
        Check(VR::Ini::ValueOf(Read(paths.Ini()), "stereo_mode") == "2" &&
                  VR::Ini::ValueOf(Read(paths.Ini()), "laser") == "1",
              "launch writes renderer keys and keeps player keys");
        SetEnvironmentVariableW(L"VR_NATIVE_STEREO", L"1");
        SetEnvironmentVariableW(L"VR_NATIVE_ASYMMETRIC", L"1");
        const auto dibr = Prepare(profile, paths, StereoPreset::Dibr);
        const auto dibr_env = environment(dibr);
        Check(std::none_of(dibr_env.begin(), dibr_env.end(), [](const auto &entry) {
                  return entry.starts_with(L"VR_NATIVE_");
              }), "DIBR removes inherited and preset native flags entirely");
        Check(dibr_env.contains(L"VR_FAST_MUTEX=1") &&
                  !dibr_env.contains(L"FAST_TOGGLES=0"),
              "DIBR keeps common optimizations without inherited overrides");
        const auto dibr_ini = Read(paths.Ini());
        Check(VR::Ini::ValueOf(dibr_ini, "late_warp") == "1" &&
                  VR::Ini::ValueOf(dibr_ini, "stereo") == "1" &&
                  VR::Ini::ValueOf(dibr_ini, "stereo_mode") == "2" &&
                  VR::Ini::ValueOf(dibr_ini, "ipd_mm") == "60" &&
                  VR::Ini::ValueOf(dibr_ini, "laser") == "1",
              "DIBR enables stereo and late warp, preserving player settings");
        Check(LaunchIni(dibr_ini, StereoPreset::Dibr) == dibr_ini,
              "DIBR configuration is idempotent");
        for (const auto *saved : {"1,0,1,1,1,0,1,100,80,85",
                                  "1,0,2,1,1,0,1,100,80,85",
                                  "1,0,1,1,1,1,1,100,80,85"}) {
            const auto preset_ini = LaunchIni(std::string("graphics_config=") + saved +
                                                 "\naa_mode=smaa\nstereo=0\n",
                                             StereoPreset::Dibr);
            Check(VR::Ini::ValueOf(preset_ini, "graphics_config") == saved &&
                      VR::Ini::ValueOf(preset_ini, "aa_mode") == "smaa" &&
                      VR::Ini::ValueOf(preset_ini, "stereo") == "1",
                  "DIBR preserves DLSS, FSR and neural preferences and enables stereo");
        }
        const auto native_again = Prepare(profile, paths, StereoPreset::Native);
        Check(environment(native_again).contains(L"VR_NATIVE_STEREO=1") &&
                  VR::Ini::ValueOf(Read(paths.Ini()), "late_warp") == "0",
              "returning to native restores a consistent preset");
        SetEnvironmentVariableW(L"VR_NATIVE_STEREO", nullptr);
        SetEnvironmentVariableW(L"VR_NATIVE_ASYMMETRIC", nullptr);
        poke('x');
        Check(GameSupportInfo(game_folder / "eboot.bin") != original_game_hash &&
                  plan.game_support_info == captured_game_info,
              "report snapshot survives a subsequent executable change");
        Write(paths.Ini(), "laser=1\n", Read(paths.Ini()));
        Throws([&] { Prepare(profile, paths); }, "other game builds refused");
        Check(Read(paths.Ini()) == "laser=1\n", "refused build leaves vr.ini untouched");
        poke('\x0f');
        const auto sfo = game_folder / "sce_sys" / "param.sfo";
        const auto generated_patch = paths.User() / "patches" / "bbvr_patch.xml";
        // Switching both ways must regenerate every metadata entry from the shipped template.
        for (const std::string version : {"01.00", "01.09", "01.00"}) {
            const auto eboot_time = fs::last_write_time(game_folder / "eboot.bin");
            WriteParamSfo(sfo, version);
            const auto original_sfo = Read(sfo);
            Check(CheckGameBuild(game_folder / "eboot.bin") == version,
                  "supported metadata accepted only after executable validation");
            Prepare(profile, paths);
            const auto generated = Read(generated_patch);
            pugi::xml_document expected, actual;
            Check(bool(expected.load_string(patch_template.c_str(), pugi::parse_full)) &&
                      bool(actual.load_string(generated.c_str(), pugi::parse_full)),
                  "original and generated patches parse");
            size_t metadata_count = 0;
            for (auto entry : actual.child("Patch").children("Metadata")) {
                Check(std::string(entry.attribute("AppVer").value()) == version,
                      "every patch version matches runtime metadata");
                ++metadata_count;
            }
            Check(metadata_count > 0, "generated patch has metadata");
            for (auto entry : expected.child("Patch").children("Metadata"))
                entry.attribute("AppVer").set_value(version.c_str());
            std::ostringstream expected_text, actual_text;
            expected.save(expected_text);
            actual.save(actual_text);
            Check(expected_text.str() == actual_text.str(),
                  "patch addresses, values, title IDs and enabled flags preserved");
            Check(Read(sfo) == original_sfo && Read(root / "bbvr_patch.xml") == patch_template &&
                      fs::last_write_time(game_folder / "eboot.bin") == eboot_time,
                  "game files and packaged patch template untouched");
            Prepare(profile, paths);
            Check(Read(generated_patch) == generated, "repeated launch patch is stable");
            poke('x');
            Throws([&] { Prepare(profile, paths); }, "both metadata versions reject wrong executable");
            Check(Read(generated_patch) == generated, "rejected executable leaves launch patch intact");
            poke('\x0f');
        }
        for (const std::string version : {"01.08", "02.00", ""}) {
            WriteParamSfo(sfo, version);
            const auto before_patch = Read(generated_patch), before_ini = Read(paths.Ini());
            Throws([&] { Prepare(profile, paths); }, "unsupported metadata refused");
            Check(Read(generated_patch) == before_patch && Read(paths.Ini()) == before_ini,
                  "unsupported metadata leaves launch files untouched");
        }
        std::ofstream(sfo, std::ios::binary | std::ios::trunc) << "broken";
        Throws([&] { Prepare(profile, paths); }, "malformed metadata refused");
        WriteParamSfo(sfo, "01.00");
        for (const std::string bad_patch : {"<Patch>", "<Patch/>",
                "<Patch><Metadata AppVer='02.00'/></Patch>", "<Patch><Metadata/></Patch>"}) {
            std::ofstream(root / "bbvr_patch.xml", std::ios::binary) << bad_patch;
            const auto before_patch = Read(generated_patch), before_ini = Read(paths.Ini());
            Throws([&] { Prepare(profile, paths); }, "invalid patch template refused");
            Check(Read(generated_patch) == before_patch && Read(paths.Ini()) == before_ini,
                  "invalid template leaves launch files untouched");
        }
        std::ofstream(root / "bbvr_patch.xml", std::ios::binary) << patch_template;
        std::wstring weird = L"C:\\folder with spaces\\\"quoted\"\\";
        argv = CommandLineToArgvW((L"app.exe " + Quote(weird)).c_str(), &argc);
        Check(argc == 2 && std::wstring(argv[1]) == weird, "Windows command quoting round trip");
        LocalFree(argv);
        bool vr_ini = false;
        for (const wchar_t *p = plan.environment.data(); *p; p += wcslen(p) + 1)
            if (std::wstring(p) == L"VR_INI=" + paths.Ini().native())
                vr_ini = true;
        Check(vr_ini, "INI environment points to stable user data");
        Check(plan.directory == paths.root, "stable runtime working directory");
        Check(Paths::Default().root.filename() == DataFolderName ||
                  Paths::Default().root.filename() == LegacyDataFolderName,
              "user data is outside the installer root");
        if (RuntimeRunning()) {
            // MigrateLegacyData refuses while a game runs (its files are in use): nothing to test.
            std::cout << "note: shadPS4 is running, data-folder move checks skipped\n";
        } else {
            // The pre-0.2.7 data folder, moved once with the profile's paths into it rewritten
            // (a temporary base: never the player's own folder).
            const auto base = root / "migrate";
            const auto old = base / LegacyDataFolderName, now = base / DataFolderName;
            fs::create_directories(old / "user" / "home");
            const auto game_elsewhere = PathText(root / "game");
            Write(old / "launcher.json",
                  Json{{"game", game_elsewhere},
                       {"saves", PathText(old / "user" / "home")},
                       {"runtime", ""}}
                      .dump(2));
            Check(MigrateLegacyData(base), "the old data folder is moved");
            Check(!fs::exists(old) && fs::is_directory(now / "user" / "home"),
                  "the data moved whole to the new name");
            const auto moved = Json::parse(Read(now / "launcher.json"));
            Check(moved["saves"] == PathText(now / "user" / "home"),
                  "a save path inside the old folder follows it");
            Check(moved["game"] == game_elsewhere, "paths elsewhere are kept");
            Check(!MigrateLegacyData(base), "moved once");
            fs::create_directories(old);
            Check(!MigrateLegacyData(base) && fs::exists(old),
                  "both folders: nothing is touched");
            fs::remove_all(base);
        }
        const auto protected_home = ExecutableDirectory() / "test-save-protection";
        fs::create_directories(protected_home);
        auto unsafe_profile = profile;
        unsafe_profile.saves = PathText(protected_home);
        const auto config_before = Read(paths.Config());
        Throws([&] { Prepare(unsafe_profile, paths); },
               "saves inside application payload rejected");
        Check(Read(paths.Config()) == config_before,
              "unsafe save selection leaves config untouched");
        fs::remove(protected_home);
        // A saved custom runtime (0.3.1 and earlier let players choose one) is ignored: always the
        // included one.
        profile.use_bundled_runtime = false;
        profile.Save(paths);
        auto loaded = Profile::Load(paths);
        Check(loaded.intel_sfx_workaround, "SFX protection persists enabled");
        profile.intel_sfx_workaround = false;
        profile.Save(paths);
        Check(!Profile::Load(paths).intel_sfx_workaround, "explicit SFX opt-out persists");
        Check(!Profile::Load(paths).free_camera, "Free camera defaults off");
        profile.free_camera = true;
        profile.Save(paths);
        Check(Profile::Load(paths).free_camera, "Free camera persists");
        profile.free_camera = false;
        profile.Save(paths);
        profile.non_aggro = true;
        profile.Save(paths);
        Check(Profile::Load(paths).non_aggro, "Non-aggro mode persists");
        profile.non_aggro = false;
        profile.Save(paths);
        profile.intel_sfx_workaround = true;
        profile.Save(paths);
        Check(loaded.game == profile.game && loaded.saves == profile.saves, "profile round trip");
        Check(loaded.stereo == StereoChoice::Auto, "stereo defaults to auto");
        Check(loaded.SelectedStereo(8ull << 30) == StereoPreset::Native &&
                  loaded.SelectedStereo(12ull << 30) == StereoPreset::Native,
              "auto stereo is native whatever the GPU memory");
        profile.stereo = StereoChoice::Dibr;
        profile.Save(paths);
        loaded = Profile::Load(paths);
        Check(loaded.stereo == StereoChoice::Dibr &&
                  loaded.SelectedStereo(12ull << 30) == StereoPreset::Dibr &&
                  loaded.SelectedStereo(0) == StereoPreset::Dibr,
              "manual DIBR persists and overrides large or unknown GPU memory");
        profile.stereo = StereoChoice::Native;
        profile.Save(paths);
        loaded = Profile::Load(paths);
        Check(loaded.stereo == StereoChoice::Native &&
                  loaded.SelectedStereo(8ull << 30) == StereoPreset::Native,
              "manual native persists and overrides low memory default");
        Check(loaded.native_views == NativeViews::Auto, "native stereo eyes default to auto");
        for (const auto views : {NativeViews::Layers, NativeViews::Multiview, NativeViews::Auto}) {
            profile.native_views = views;
            profile.Save(paths);
            Check(Profile::Load(paths).native_views == views, "native stereo eyes persist");
        }
        for (const auto &value : {"\"unknown\"", "null", "123"}) {
            Write(paths.Profile(), std::string("{\"native_views\":") + value + "}");
            Check(Profile::Load(paths).native_views == NativeViews::Auto,
                  "an unreadable native stereo eyes value is auto");
        }
        profile.Save(paths);
        for (const auto &value : {"\"unknown\"", "null", "123"}) {
            Write(paths.Profile(), std::string("{\"stereo\":") + value + "}");
            Check(Profile::Load(paths).stereo == StereoChoice::Auto,
                  "invalid stereo preference falls back to auto");
        }
        profile.stereo = StereoChoice::Auto;
        profile.Save(paths);
        loaded = Profile::Load(paths);
        const auto bundled = ExecutableDirectory() / "runtime" / "shadps4.exe";
        Check(loaded.use_bundled_runtime &&
                  loaded.runtime ==
                      (fs::is_regular_file(bundled) ? PathText(bundled) : std::string{}),
              "a saved custom runtime is ignored: the included one");
        profile.use_bundled_runtime = true;
        profile.Save(paths);
        loaded = Profile::Load(paths);
        Check(loaded.use_bundled_runtime &&
                  loaded.runtime ==
                      (fs::is_regular_file(bundled) ? PathText(bundled) : std::string{}),
              "included runtime mode resolves current package instead of stale saved path");
        // Update checks: on by default; a pre-0.2.3 profile's stored "off" (the old default, never
        // chosen) loads as on; a player's own "off" stays off.
        Check(Profile{}.check_updates, "update checks are on by default");
        Check(!Profile{}.fullscreen, "the desktop mirror is windowed by default");
        Write(paths.Profile(), "{\"game\": \"\", \"fullscreen\": true}");
        Check(!Profile::Load(paths).fullscreen, "an old profile's unchosen fullscreen loads windowed");
        Check(Profile::Load(paths).stereo == StereoChoice::Auto,
              "old profiles without stereo retain automatic selection");
        Check(Profile::Load(paths).intel_sfx_workaround,
              "existing profiles without an SFX choice enable protection");
        profile.fullscreen = true;
        profile.fullscreen_chosen = true;
        profile.Save(paths);
        Check(Profile::Load(paths).fullscreen, "a player's own fullscreen survives a reload");
        profile.fullscreen = false;
        profile.fullscreen_chosen = false;
        Write(paths.Profile(), "{\"game\": \"\", \"check_updates\": false}");
        Check(Profile::Load(paths).check_updates, "an old profile's unchosen off loads as on");
        profile.check_updates = false;
        profile.check_updates_chosen = true;
        profile.Save(paths);
        loaded = Profile::Load(paths);
        Check(!loaded.check_updates && loaded.check_updates_chosen,
              "a player's own off survives a reload");
        profile.check_updates = true;
        profile.check_updates_chosen = false;
        Write(paths.Profile(), "{broken");
        Throws([&] { Profile::Load(paths); }, "corrupt profile is never silently overwritten");
        std::set<std::string> keys;
        for (const auto &row : Settings()) {
            Check(!row.keys.empty() &&
                      (row.Number() ? row.keys.size() == 1 && row.min < row.max && row.step > 0 &&
                                          row.fallback >= (row.zero ? 0 : row.min) &&
                                          row.fallback <= row.max
                                    : !row.options.empty()),
                  "setting has options or a valid range");
            Check(std::find_if(Categories().begin(), Categories().end(), [&](const char *c) {
                      return std::string_view(c) == row.category;
                  }) != Categories().end(),
                  "setting belongs to a page tab");
            // The two strength sliders set the key their merged row sets with its steps.
            const bool fine = row.id && (std::string_view(row.id) == "health_color_strength" ||
                                         std::string_view(row.id) == "stamina_color_strength");
            for (const auto &key : row.keys)
                Check(keys.insert(key).second != fine, "settings do not fight over keys");
            for (const auto &option : row.options)
                Check(option.values.size() == row.keys.size(), "multi-key options are complete");
        }
        // Every row on every in-game tab (vr_page.h Tabs), in its tab, in the page's order and
        // with its keys and choices, but Smooth motion, which the headset preset turns off; the
        // tabs the game keeps (Upscaling, Save) are not the launcher's.
        {
            const auto &rows = VR::Page::Rows();
            std::vector<std::string> page_tabs;
            for (const auto &tab : VR::Page::Tabs()) {
                const std::string_view name = tab.name;
                if (name == "Upscaling" || name == "Save")
                    continue;
                page_tabs.emplace_back(name);
                size_t next = 0; // the launcher's rows of this tab, in order
                std::vector<const Setting *> mine;
                for (const auto &row : Settings())
                    if (name == row.category)
                        mine.push_back(&row);
                for (const int r : tab.AllRows()) {
                    const auto &game = rows[size_t(r)];
                    if (std::string_view(game.id) == "smooth_motion")
                        continue;
                    Check(next < mine.size() && mine[next]->id &&
                              std::string_view(mine[next]->id) == game.id,
                          "every in-game row is in its launcher tab, in the page's order");
                    const Setting &s = *mine[next++];
                    Check(std::string_view(s.label) == game.label &&
                              std::string_view(s.help) == game.help,
                          "a launcher row has the page's words");
                    std::vector<std::string> game_keys;
                    for (const char *key : game.keys)
                        if (key)
                            game_keys.emplace_back(key);
                    Check(s.keys == game_keys, "a launcher row writes the page's keys");
                    Check(s.options.size() == game.options.size(),
                          "a launcher row has the page's choices");
                    for (size_t i = 0; i < s.options.size(); ++i) {
                        Check(std::string_view(s.options[i].label) == game.options[i].text &&
                                  s.options[i].ini_only == game.options[i].ini_only,
                              "a launcher choice is the page's");
                        for (size_t j = 0; j < s.keys.size(); ++j)
                            Check(s.options[i].values[j] ==
                                      (game.options[i].values[j] ? game.options[i].values[j] : Keep),
                                  "a launcher choice writes the page's values");
                    }
                }
                // What follows the page's rows: only the rows its tabs leave out.
                for (; next < mine.size(); ++next)
                    Check(mine[next]->group &&
                              std::string_view(mine[next]->group) == LauncherOnlyGroup,
                          "a tab's extra rows are the launcher-only ones");
            }
            std::vector<std::string> launcher_tabs(Categories().begin(), Categories().end());
            page_tabs.push_back("Graphics");
            page_tabs.push_back("Extras");
            Check(launcher_tabs == page_tabs, "the launcher's tabs are the page's, then its own");
        }
        // Every plain vr.ini value the page or its tuning rows set (2026-10-31: Damage dealt,
        // Physical summon bell and the boss bar were missing).
        for (const char *key :
             {"height_base", "height_adjust_cm", "swap_sticks", "snap_turn_deg", "haptic_gain",
              "controls", "inject", "x_button", "laser", "grip_adjust", "seated", "body_cutaway",
              "body_mode",
              "ipd_mm", "hud_vitals", "vitals_heartbeat", "vitals_edge", "ui_counters",
              "hit_vignette", "ui_hud_scale_deg", "player_hp_bonus_pct", "dodge_iframes_extra",
              "melee_kill_ragdoll", "melee_ragdoll_impulse", "melee_stamina_pct", "aa_mode", "lod_bias",
              "ui_enemy_hp_bars", "ui_boss_bar", "body_align", "body_posture", "body_lean_side_deg",
              "body_comfort", "body_align_ms", "holster_mode", "holster_haptics",
              "shoulder_vial", "quick_arc_pick", "health_color",
              "health_color_strength", "stamina_color", "stamina_color_strength",
              "shoulder_switch", "weapon_wall", "two_hand_guide", "two_hand_swing_hum",
              "gun_wrist_reload", "blade_sounds", "vr_interactions", "upscale_focus",
              "physical_levers",
              "lamp_travel",
              "physical_summon_bell", "cinematic_ladder_skip", "outgoing_damage_pct",
              "vr_grab_attacks",
              "two_hand_optimized_controls", "two_hand_trigger_hold", "weighted_weapons",
              "riposte_style", "color_contrast", "color_saturation", "color_gamma", "foveation",
              "frame_overlap", "shadows_far", "water_reflections", "shadows_busy",
              "light_shadow_detail", "canted_eyes", "cinematic", "first_person"})
            Check(keys.contains(key), "every in-game VR page setting is offered");
        auto row_of = [](const char *key) {
            for (const auto &row : Settings())
                if (row.keys[0] == key)
                    return row;
            throw std::runtime_error(std::string("no row for ") + key);
        };
        // Number rows write what the page writes.
        Check(!keys.contains("two_hand_hold_b_reposition"), "legacy B toggle is not offered");
        const auto two_handed = row_of("two_hand_optimized_controls");
        Check(two_handed.keys.size() == 2 && two_handed.keys[1] == "two_hand_trigger_hold" &&
                  two_handed.options.size() == 3 && two_handed.options[0].values[0] == "0" &&
                  two_handed.options[0].values[1] == Keep &&
                  two_handed.options[1].values == std::vector<std::string>{"1", "1"} &&
                  two_handed.options[2].values == std::vector<std::string>{"1", "0"},
              "two-handed controls: Original, then optimized with Hold or Toggle");
        // A key the file lacks counts as its game default: an older launcher's single rows (only
        // health_color=1, say) still pick their merged row's choice.
        for (const auto &[key, value] :
             std::initializer_list<std::pair<const char *, const char *>>{
                 {"health_color_strength", "0.5"}, {"stamina_color_strength", "0.7"},
                 {"ui_boss_bar", "1"}, {"ui_enemy_hp_bars", "0"}, {"melee_ragdoll_impulse", "1"},
                 {"two_hand_trigger_hold", "1"}, {"smooth_turn_deg_per_s", "120"}})
            Check(CodeDefault(key) == value, "the game's defaults for two-key rows");
        Check(!CodeDefault("laser"), "a key without a known default has none");
        // Use the actual option labels the launcher offers, including both keys of paired rows.
        const uint64_t tier_bytes[] = {8ull << 30, 10ull << 30, 14'817'637'171ull, 14'817'637'172ull};
        for (const auto &[key, labels] : std::initializer_list<
                 std::pair<const char *, std::array<const char *, 4>>>{
                 {"light_shadows", {"Game's", "Game's", "Low", "Low"}},
                 {"fog_banks", {"Game's", "Game's", "On", "On"}},
                 {"moon_shafts", {"Off", "Off", "On", "On"}},
                 {"ambient_occlusion", {"Game's", "AO", "AO", "AO and bounce"}},
                 {"water_reflections", {"Off", "Off", "Water", "Wet streets and water"}}}) {
            const auto row = row_of(key);
            for (size_t i = 0; i < labels.size(); ++i) {
                bool found = false;
                for (const auto &option : row.options) {
                    if (std::string_view(option.label) != labels[i]) continue;
                    found = true;
                    for (size_t j = 0; j < row.keys.size(); ++j)
                        Check(CardDefault(row.keys[j], tier_bytes[i]) == option.values[j],
                              "Auto resolves every key of the displayed option");
                }
                Check(found, "Auto names an option available in the launcher");
            }
        }
        Check(!FogEnabled(std::nullopt, tier_bytes[0]) && FogEnabled(std::nullopt, tier_bytes[3]) &&
                  FogEnabled("true", tier_bytes[0]) && !FogEnabled("0", tier_bytes[3]),
              "Moon glow follows the pending Fog choice, including Auto");
        Check(!CardDefault("laser", tier_bytes[3]), "ordinary settings keep their own defaults");
        // The legacy generated detail line by itself still lets the card choose enablement.
        const auto shadow_choice = [&](const std::string &ini, uint64_t bytes) {
            const auto read = [&](const char *key) {
                return ChoiceOverride(key, VR::Ini::ValueOf(ini, key));
            };
            const auto enabled = read("light_shadows");
            const auto detail = read("light_shadow_detail");
            const bool automatic = !enabled && !detail;
            const auto row = row_of("light_shadows");
            for (const auto &option : row.options) {
                if (SameValue(enabled.value_or(CardDefault("light_shadows", bytes).value()), option.values[0]) &&
                    SameValue(detail.value_or(CardDefault("light_shadow_detail", bytes).value()), option.values[1]))
                    return automatic ? std::string("Auto (") + option.label + ")" : std::string(option.label);
            }
            return std::string("Custom");
        };
        constexpr uint64_t lamp_threshold = 23ull << 29; // 11.5 GiB of dedicated memory
        for (const char *ini : {"", "#light_shadow_detail=low\n", "light_shadow_detail=low\n"}) {
            Check(shadow_choice(ini, lamp_threshold - 1) == "Auto (Game's)" &&
                      shadow_choice(ini, lamp_threshold) == "Auto (Low)" &&
                      shadow_choice(ini, 16ull << 30) == "Auto (Low)",
                  "fresh and legacy defaults show the card's actual shadow choice");
        }
        Check(shadow_choice("light_shadows=1\nlight_shadow_detail=low\n", 8ull << 30) == "Low" &&
                  shadow_choice("light_shadows=0\nlight_shadow_detail=low\n", 16ull << 30) == "Game's" &&
                  shadow_choice("light_shadow_detail=high\n", lamp_threshold) == "High" &&
                  shadow_choice("light_shadow_detail=0\n", lamp_threshold) == "Low" &&
                  shadow_choice("light_shadow_detail=1\n", lamp_threshold) == "High",
              "manual shadow overrides survive even when the card would pick differently");
        for (const char *off : {"0", "false", "off"})
            Check(ChoiceOverride("water_reflections", off) == "0", "recognize water Off spellings");
        for (const char *on : {"1", "true", "on"})
            Check(ChoiceOverride("water_reflections", on) == "1", "recognize water On spellings");
        Check(!ChoiceOverride("water_reflections", std::nullopt) && ChoiceOverride("wet_streets", "off") == "off",
              "keep Auto and scope water's spelling rules to its own setting");
        // Each two-key row's defaults pick the option the page calls the default.
        for (const auto &[key, option] :
             std::initializer_list<std::pair<const char *, const char *>>{
                 {"health_color", "Medium"}, {"stamina_color", "Medium"},
                 {"ui_enemy_hp_bars", "Bosses only"}, {"melee_kill_ragdoll", "Normal"},
                 {"two_hand_optimized_controls", "Original"}, {"quick_arc_pick", "Fan A"},
                 {"body_align", "Lean"}, {"snap_turn_deg", "Snap 30\xC2\xB0"},
                 {"vitals_heartbeat", "On"}, {"foveation", "Auto"},
                 {"cinematic_ladder_skip", "On"}, {"vr_interactions", "Immersive"},
                 {"upscale_focus", "Medium"}}) {
            const auto row = row_of(key);
            const Option *picked = nullptr;
            for (const auto &o : row.options) {
                bool same = true;
                for (size_t j = 0; j < row.keys.size(); ++j)
                    same &= o.values[j] == Keep || SameValue(*CodeDefault(row.keys[j]), o.values[j]);
                if (same) {
                    picked = &o;
                    break;
                }
            }
            Check(picked && std::string_view(picked->label) == option,
                  "the game's defaults pick the page's default choice");
        }
        Check(SearchFinds("boss bar", "health bars bosses only ui_boss_bar ui boss bar") &&
                  !SearchFinds("boss stamina", "health bars bosses only") && !SearchFinds("", "x"),
              "search needs every word");
        Check(SearchFinds("turn", "smooth turning") && !SearchFinds("turn", "a staged return") &&
                  SearchFinds("3.1.5", "fsr 3.1.5") && SearchFinds("aa", "aa_mode"),
              "search words match from the start of a word");
        // Every row's words with a Quest's buttons and the game's defaults.
        {
            const ControlsGuide::Lookup none = [](const std::string &) {
                return std::optional<std::string>{};
            };
            for (const auto &row : Settings()) {
                const auto words =
                    row.id ? ControlsGuide::RowWords(row.id, ControlsGuide::Platform::Quest, none)
                           : ControlsGuide::RowText{row.label, row.help};
                Check(!words.label.empty() && words.label.find('{') == std::string::npos &&
                          words.help.find('{') == std::string::npos,
                      "every row's words name real buttons");
                // BBVR_DUMP_SETTINGS=1: every row as the VR settings page lists it, to read.
                if (std::getenv("BBVR_DUMP_SETTINGS")) {
                    std::printf("%s | %s | %s :: %s\n   ", row.category, row.group ? row.group : "-",
                                words.label.c_str(), words.help.c_str());
                    if (row.Number())
                        std::printf("%d..%d step %d%s (default %d)%s", row.min, row.max, row.step,
                                    row.unit, row.fallback, row.zero ? " zero text" : "");
                    for (const auto &o : row.options)
                        std::printf("[%s%s]", o.label, o.ini_only ? " (file only)" : "");
                    std::printf("\n");
                }
            }
        }
        const auto vibration = row_of("haptic_gain");
        Check(NumberValue(vibration, 7) == "1" && NumberValue(vibration, 10) == "1.429" &&
                  NumberValue(vibration, 1) == "0.1429",
              "vibration slider is the page's n/7");
        Check(NumberShown(vibration, "1") == 7 && NumberShown(vibration, "1.4") == 10 &&
                  NumberShown(vibration, "0.5") == 4 && !NumberShown(vibration, "loud"),
              "vibration file values shown as the page does");
        const auto strength = row_of("two_hand_swing_hum");
        Check(NumberValue(strength, 50) == "0.5" && NumberShown(strength, "0.5") == 50 &&
                  strength.fallback == 100,
              "percent sliders hold fractions");
        // The tone filter's unset value is the game's default, 30%, not the game's own look.
        Check(row_of("cinematic_look").fallback == 30, "tone filter shows the game's 30% default");
        const auto eyes = row_of("ipd_mm");
        Check(NumberShown(eyes, "0") == 0 && NumberShown(eyes, "41") == 41 &&
                  NumberValue(eyes, 63) == "63" && eyes.zero && std::string_view(eyes.zero) == "Headset",
              "eye distance keeps 0 for the headset's own");
        const auto damage = row_of("outgoing_damage_pct");
        Check(damage.sign && damage.min == -60 && damage.max == 30 && damage.step == 5 &&
                  NumberValue(damage, -15) == "-15" && damage.category == std::string_view("Difficulty"),
              "damage dealt is the page's signed -60..30 % in steps of 5");
        Check(row_of("ui_enemy_hp_bars").options.size() == 4 &&
                  row_of("ui_enemy_hp_bars").options[3].ini_only,
              "health bars keep the page's file-only Enemies only");
        Check(NumberValue(row_of("color_saturation"), -100) == "-100", "signed colour values");
        Check(std::stof(NativeIpd("ipd_mm=42", false)) == 0.042f, "the page's 40..80 mm reaches renderer");
        // The page's snap turns leave the smooth speed alone.
        for (const auto &option : row_of("snap_turn_deg").options)
            Check(option.values[0] != "0" ? option.values[1] == Keep : option.values[1] != Keep,
                  "snap turns keep the smooth speed");
        for (const auto &row : Settings()) {
            Check(row.details && *row.details, "every setting has a tooltip");
            for (const auto &key : row.keys)
                Check(key != "late_warp_motion" && key != "graphics_config",
                      "inert or composite keys are not plain rows");
        }

        Check(ArgumentPath(L"C:\\plain\\path") == L"C:\\plain\\path", "ASCII paths pass unchanged");
        {
            // An existing non-ASCII path can use an ASCII alias, a lossless code-page
            // conversion, or a clear setup error, depending on the system and volume.
            fs::create_directories(unicode.parent_path());
            std::ofstream(unicode) << "test";
            std::wstring argument;
            try {
                argument = ArgumentPath(unicode);
            } catch (const std::runtime_error &error) {
                Check(std::string_view(error.what()).find("Move it to a folder") !=
                          std::string_view::npos,
                      "unrepresentable launch paths give a setup error");
            }
            if (!argument.empty())
                Check(fs::equivalent(fs::path(argument), unicode),
                      "representable launch paths still point at the game");
        }
        Check(SameValue("0.50", "0.5") && SameValue("true", "1") && SameValue("false", "0") &&
                  !SameValue("1", "true") && !SameValue("look_down", "always"),
              "choices match like the in-game page");
        Check(ResetValue("weighted_weapons") == "1" && !ResetValue("weapon_wall") &&
                  !ResetValue("weapon_weight"),
              "a reset writes weighted weapons On, as the page does; other keys are removed");
        Check(!NumberShown(row_of("ipd_mm"), "95") && !NumberShown(row_of("ipd_mm"), "30"),
              "values outside the slider show as Custom");

        // graphics_config: the game's ten fields, defaults for anything it would reject.
        Check(ParseGraphics(std::nullopt) == DefaultGraphics, "absent graphics is the default");
        Check(GraphicsText(DefaultGraphics) == "1,0,0,1,1,0,1,100,80,85", "graphics text");
        const GraphicsFields dlss{1, -20, 1, 2, 1, 0, 1, 100, 80, 85};
        Check(ParseGraphics(GraphicsText(dlss)) == dlss, "graphics round trip");
        // Both launcher AA controls use this rule; match the game's DIBR support for every
        // provider and preserve AA when a saved DLSS 5 choice cannot run in DIBR.
        for (int provider = 0; provider <= 4; ++provider) {
            for (int neural : {0, 1}) {
                auto fields = DefaultGraphics;
                fields[2] = provider;
                fields[5] = neural;
                const auto game = VR::Graphics::Parse(GraphicsText(fields));
                Check(game && UpscalerBypassesAa(fields, StereoPreset::Dibr) ==
                                  VR::Graphics::DibrUpscaleSupports(*game),
                      "DIBR AA availability matches the runtime for every upscaler");
                Check(UpscalerBypassesAa(fields, StereoPreset::Native) == (provider != 0),
                      "native AA remains available only with upscaling off");
            }
        }
        // 5 is no upscaler (FSR 4.1 is 4 since 2026-10-31).
        for (const char *bad : {"", "1,0,0", "2,0,0,1,1,0,1,100,80,85", "1,5,0,1,1,0,1,100,80,85",
                                "1,0,5,1,1,0,1,100,80,85", "1,0,0,1,1,0,1,100,80,101",
                                "1,0,0,1,1,0,1,100,80,85,7", "1, 0,0,1,1,0,1,100,80,85"})
            Check(ParseGraphics(std::string(bad)) == DefaultGraphics, "invalid graphics rejected");

        auto fsr31_graphics = DefaultGraphics;
        fsr31_graphics[2] = 3;
        fsr31_graphics[4] = 2;
        Check(ParseGraphics(GraphicsText(fsr31_graphics)) == fsr31_graphics,
              "FSR 3.1.5 provider and profile round trip");
        auto fsr41_graphics = DefaultGraphics;
        fsr41_graphics[2] = 4;
        fsr41_graphics[4] = 0;
        Check(ParseGraphics(GraphicsText(fsr41_graphics)) == fsr41_graphics,
              "FSR 4.1 provider and profile round trip");
        // The game's own parser agrees, and its in-memory key keeps FSR 4.1 (its third bit).
        {
            const auto game = VR::Graphics::Parse(GraphicsText(fsr41_graphics));
            Check(game && game->upscaler == VR::Graphics::Upscaler::Fsr41 &&
                      VR::Graphics::IsFsr(game->upscaler) &&
                      VR::Graphics::Unpack(VR::Graphics::Pack(*game)) == *game &&
                      VR::Graphics::Unpack(VR::Graphics::Pack(VR::Graphics::Config{})) ==
                          VR::Graphics::Config{} &&
                      !VR::Graphics::Parse("1,0,5,1,1,0,1,100,80,85"),
                  "the game parses and packs FSR 4.1");
            for (int provider = 0; provider <= 4; ++provider) {
                VR::Graphics::Config c{};
                c.upscaler = VR::Graphics::Upscaler(provider);
                Check(VR::Graphics::Unpack(VR::Graphics::Pack(c)).upscaler == c.upscaler &&
                          (provider > 3 || (VR::Graphics::Pack(c) >> 37) == 0),
                      "every upscaler packs, the first four as before FSR 4.1");
            }
        }
        // Exit codes, driver versions.
        Check(ExitReason(0).empty() && ExitReason(0xC0000005) == "a memory access error" &&
                  ExitReason(0x80000003) == "an internal check failed" &&
                  ExitReason(0x12345678) == "error 0x12345678",
              "exit reasons");
        Check(NvidiaDriver((32ull << 48) | (0ull << 32) | (15ull << 16) | 6094) == "560.94",
              "NVIDIA driver version");

        // The headset's request from the game's last start (log/headset.json).
        {
            Paths headset{root / "headset"};
            fs::create_directories(headset.User() / "log");
            Check(!LastHeadsetRequest(headset), "no headset file before the first launch");
            std::ofstream(headset.User() / "log" / "headset.json", std::ios::binary)
                << "{\"system\": \"PS VR2\", \"width\": [3400, 3392], \"height\": [3468, 3472]}\n";
            const auto request = LastHeadsetRequest(headset);
            Check(request && request->system == "PS VR2" && request->width == 3400 &&
                      request->height == 3472,
                  "headset request read, larger eye");
            Check(request && request->max_width == 8192 && request->max_height == 8192,
                  "older headset records retain the legacy cap");
            std::ofstream(headset.User() / "log" / "headset.json", std::ios::binary)
                << R"({"system":"Link","width":[2000,2000],"height":[2160,2160],"max_width":2848,"max_height":3136})";
            const auto capped = LastHeadsetRequest(headset);
            Check(capped && capped->max_width == 2848 && capped->max_height == 3136,
                  "headset output limits read for the scaling preview");
            std::ofstream(headset.User() / "log" / "headset.json", std::ios::binary)
                << R"({"width":[2000,2000],"height":[2160,2160],"max_width":0})";
            Check(!LastHeadsetRequest(headset), "invalid headset cap is ignored");
            std::ofstream(headset.User() / "log" / "headset.json", std::ios::binary) << "{\"width\": 5}";
            Check(!LastHeadsetRequest(headset), "a malformed headset file is ignored");
            // The graphics-memory estimate against two measured points (without its session
            // margin).
            const double quality = EstimateVramMb(1336, 1440, 2000, 2160, true) / VramSessionMargin;
            const double native =
                EstimateVramMb(2000, 2160, 2000, 2160, false) / VramSessionMargin;
            Check(std::abs(quality - 4973) < 150 && std::abs(native - 5575) < 150,
                  "graphics-memory estimate matches the 2000x2160 measurements");
            Check(EstimateVramMb(1360, 1392, 3400, 3472, true) >
                      EstimateVramMb(800, 864, 2000, 2160, true),
                  "a larger output needs more graphics memory");
            constexpr uint64_t gb16 = uint64_t(16303) << 20, gb8 = uint64_t(8188) << 20;
            Check(!VramFitsCard(EstimateVramMb(3400, 3472, 3400, 3472, false), gb16) &&
                      VramFitsCard(EstimateVramMb(1360, 1392, 3400, 3472, true), gb16) &&
                      VramFitsCard(EstimateVramMb(1336, 1440, 2000, 2160, true), gb8),
                  "PS VR2 native warns on 16 GB; Ultra Performance and Quest 3 Quality on 8 GB fit");
        }

        // Log problems and the shader cache.
        {
            Paths logs{root / "logs"};
            fs::create_directories(logs.User() / "log");
            Check(LogProblems(logs).empty(), "no log, no problems");
            std::ofstream(logs.User() / "log" / "shad_log.txt", std::ios::binary)
                << "[Lib] <Info> fine\r\n[Debug] <Critical> assert.cpp: Assertion Failed!\r\n"
                   "[Render] <Error> lost\r\n";
            const auto problems = LogProblems(logs);
            Check(problems.size() == 2 && problems[0].find("Assertion Failed!") != std::string::npos &&
                      problems[1].back() != '\r',
                  "critical and error lines found");
            // The game's crash record (every build): its newest lines join, with where it is.
            std::ofstream(logs.User() / "log" / "crash.txt", std::ios::binary)
                << "2026-10-17 15:00:00 [Main] old\r\n"
                   "2026-10-17 15:07:13 [GpuCommandProcessor] a check failed at buffer.cpp:139\r\n"
                   "2026-10-17 15:07:13 [GpuCommandProcessor] exception 0x80000003 at "
                   "shadps4.exe+0x2ffc6 (0x70000002ffc6)\r\n";
            const auto with_record = LogProblems(logs);
            Check(with_record.size() == 6 &&
                      with_record[3].find("buffer.cpp:139") != std::string::npos &&
                      with_record[4].find("0x80000003") != std::string::npos &&
                      with_record[5].find("crash.txt") != std::string::npos,
                  "the crash record follows the log's lines");
            fs::remove(logs.User() / "log" / "shad_log.txt");
            const auto play_build = LogProblems(logs);
            Check(play_build.size() == 4 && play_build[1].find("buffer.cpp:139") != std::string::npos,
                  "the play build (no log) still shows its crash record");
            std::ofstream(logs.User() / "log" / "crash.txt", std::ios::binary | std::ios::app)
                << "2026-10-17 17:38:37 [GpuCommandProcessor] out of memory: a buffer could not be "
                   "allocated (Vulkan error -2)\r\n";
            const auto out_of_memory = LogProblems(logs);
            Check(!out_of_memory.empty() &&
                      out_of_memory[0].find("ran out of memory") != std::string::npos,
                  "an out-of-memory crash says what to do first");
            std::ofstream(logs.User() / "log" / "crash.txt", std::ios::binary)
                << "2026-10-18 12:00:00 [Main] older note\n"
                   "2026-10-18 12:00:01 [Main] a check failed at melee.cpp:123\n"
                   "2026-10-18 12:00:01 [Main] exception 0xc0000005 at shadps4.exe+0x123, reading 0x8\n"
                   "  Thread ID: 1234\n"
                   "  Registers: RIP=0x123 RSP=0x456 RBP=0x789\n"
                   "  Registers: RAX=0x0 RCX=0x8 RDX=0x0 R8=0x0 R9=0x0\n"
                   "  Accessed memory: free\n";
            const auto detailed = LogProblems(logs);
            Check(detailed.size() == 8 && detailed[1].find("melee.cpp:123") != std::string::npos &&
                      detailed[2].find("reading 0x8") != std::string::npos &&
                      detailed[3].find("Thread ID: 1234") != std::string::npos &&
                      detailed[6].find("Accessed memory: free") != std::string::npos,
                  "copied report keeps the fault location and all diagnostic continuations");
            {
                std::ofstream record(logs.User() / "log" / "crash.txt", std::ios::binary);
                for (int i = 0; i < 100; ++i)
                    record << "  continuation\n";
            }
            Check(LogProblems(logs).size() == 65, "crash report continuations are bounded");
            fs::last_write_time(logs.User() / "log" / "crash.txt",
                                fs::file_time_type::clock::now() - std::chrono::hours(1));
            Check(LogProblems(logs).empty(), "an old crash record is not this crash");
            Check(!ShaderCacheReady(logs), "no shader cache yet");
            fs::create_directories(logs.User() / "cache" / "CUSA03173");
            std::ofstream(logs.User() / "cache" / "CUSA03173" / "profile.bin") << "x";
            Check(ShaderCacheReady(logs), "shader cache found");
        }

        // Save backups: made, not repeated, pruned, restored.
        {
            Paths data{root / "backup data"};
            const auto home = root / "backup home";
            const auto save = home / "1000" / "savedata" / "CUSA03173" / "SPRJ0005" / "userdata0000";
            fs::create_directories(save.parent_path());
            const auto other = home / "1000" / "savedata" / "CUSA99999" / "game.bin";
            fs::create_directories(other.parent_path());
            std::ofstream(other) << "another game";
            Check(BackupSaves(data, home).empty(), "no saves, no backup");
            std::ofstream(save, std::ios::binary) << "first";
            const auto first = BackupSaves(data, home);
            Check(!first.empty() && Read(first / "1000" / "savedata" / "CUSA03173" / "SPRJ0005" /
                                         "userdata0000") == "first",
                  "backup copies the saves");
            Check(BackupSaves(data, home).empty(), "unchanged saves are not backed up again");
            Check(LaunchBackup(data, home) == first,
                  "a launch with unchanged saves stands on their newest backup (soft save)");
            Check(LaunchBackup(data, root / "no such home").empty(),
                  "a launch without saves has no backup to stand on");
            Check(!fs::exists(first / "1000" / "savedata" / "CUSA99999"),
                  "other games' saves are not copied");
            for (int i = 0; i < 12; ++i) {
                std::ofstream(save, std::ios::binary) << "save " << i;
                Check(!BackupSaves(data, home).empty(), "changed saves are backed up");
            }
            const auto list = SaveBackups(data);
            Check(list.size() == MaxSaveBackups, "only the newest backups stay");
            Check(list.front().label.size() == 19 && list.front().label[4] == '-' &&
                      list.front().label[13] == ':',
                  "backup label is a readable time");
            std::ofstream(save, std::ios::binary) << "current";
            RestoreSaves(data, home, list.back().folder);
            Check(Read(save) == "save 2", "restore brings the backup back");
            Check(Read(other) == "another game", "restore leaves other games' saves alone");
            Check(Read(SaveBackups(data).front().folder / "1000" / "savedata" / "CUSA03173" /
                       "SPRJ0005" / "userdata0000") == "current",
                  "restore backs the replaced saves up first");
            Check(!fs::exists(save.parent_path().parent_path().parent_path().native() + L".restoring") &&
                      !fs::exists(save.parent_path().parent_path().parent_path().native() + L".replaced"),
                  "restore leaves no staging folders");
        }
        {
            // Sharper skies: the texture reader the runtime shares, and the image steps.
            namespace Sky = VR::SkyTextures;
            Check(Sky::Wanted("m21_00_moon_a.tpf.dcx") && Sky::Wanted("m26_clond_103_a.tpf.dcx") &&
                      !Sky::Wanted("m21_00_moon_n.tpf.dcx") && !Sky::Wanted("m21_00_wall_a.tpf.dcx"),
                  "skies: which names");
            Check(Sky::TextureName("m21_00_moon_a.tpf.dcx") == "m21_00_moon_a", "skies: image name");
            Check(Sky::ScaleFor(2048, 512, 4096) == 2 && Sky::ScaleFor(1024, 256, 4096) == 4 &&
                      Sky::ScaleFor(512, 512, 4096) == 2 && Sky::ScaleFor(2048, 2048, 4096) == 2 &&
                      Sky::ScaleFor(128, 128, 4096) == 1 && Sky::ScaleFor(2048, 512, 8192) == 4,
                  "skies: copy scales");
            Check(Sky::DataFile("m24_0000.tpfbhd")->filename() == "m24_0000.tpfbdt" &&
                      Sky::DataFile("m24_0000.tpfbhd.patch")->filename() == "m24_0000.tpfbdt.patch" &&
                      !Sky::DataFile("m24_0000.tpfbdt"),
                  "skies: list and data files");
            // A list with one file (BHF4: 0x24-byte headers, UTF-16 names).
            std::vector<std::uint8_t> bhd(0x40 + 0x24);
            std::memcpy(bhd.data(), "BHF4", 4);
            const std::int32_t one = 1, offset = 0x1234, name_at = int(bhd.size());
            const std::int64_t header = 0x24, packed = 77;
            std::memcpy(&bhd[0x0C], &one, 4);
            std::memcpy(&bhd[0x20], &header, 8);
            bhd[0x30] = 1;
            std::memcpy(&bhd[0x40 + 8], &packed, 8);
            std::memcpy(&bhd[0x40 + 24], &offset, 4);
            std::memcpy(&bhd[0x40 + 32], &name_at, 4);
            for (const char c : "M21_00_Moon_a.tpf.dcx"s + '\0') {
                bhd.push_back(std::uint8_t(c));
                bhd.push_back(0);
            }
            const auto list = Sky::ReadList(bhd);
            Check(list.size() == 1 && list[0].name == "m21_00_moon_a.tpf.dcx" &&
                      list[0].offset == 0x1234 && list[0].size == 77,
                  "skies: list read");
            // An 8 x 4 PS4 TPF, BC1: block 0 red, block 1 (Morton next along x) a three-colour
            // block of index 3 (transparent with 1-bit alpha, else opaque black).
            std::vector<std::uint8_t> tpf(0x40 + 64 * 8);
            std::memcpy(tpf.data(), "TPF\0", 4);
            tpf[0x0C] = 4;
            const std::uint32_t data_at = 0x40, dxgi = 72;
            const std::int32_t data_size = 64 * 8;
            const std::uint16_t tw = 8, th = 4;
            std::memcpy(&tpf[0x10], &data_at, 4);
            std::memcpy(&tpf[0x14], &data_size, 4);
            std::memcpy(&tpf[0x1C], &tw, 2);
            std::memcpy(&tpf[0x1E], &th, 2);
            std::memcpy(&tpf[0x30], &dxgi, 4);
            const std::uint8_t red[8]{0x00, 0xF8, 0x00, 0xF8, 0, 0, 0, 0};
            const std::uint8_t keyed[8]{0x00, 0x00, 0x1F, 0x00, 0xFF, 0xFF, 0xFF, 0xFF};
            std::memcpy(&tpf[0x40], red, 8);
            std::memcpy(&tpf[0x48], keyed, 8);
            for (const std::uint8_t format : {std::uint8_t(1), std::uint8_t(0)}) {
                tpf[0x18] = format;
                const auto texture = Sky::FirstTexture(tpf);
                Check(texture && texture->blocks == Sky::Blocks::Bc1 && texture->width == 8 &&
                          texture->height == 4,
                      "skies: TPF read");
                const auto rgba = Sky::DecodeTop(tpf, *texture);
                Check(rgba.size() == 8 * 4 * 4 && rgba[0] == 255 && rgba[1] == 0 && rgba[3] == 255 &&
                          rgba[4 * 4 + 3] == (format == 1 ? 0 : 255) && rgba[4 * 4] == 0,
                      "skies: BC1 decoded from its tiles");
            }
            Skies::Plane flat{16, 8, std::vector<float>(16 * 8, 100.0f)};
            Skies::Blur(flat, 4);
            const auto resized = Skies::Resize(flat, 8, 4);
            Check(std::all_of(flat.v.begin(), flat.v.end(),
                              [](float v) { return std::abs(v - 100) < 1e-3f; }) &&
                      resized.w == 8 && resized.h == 4 &&
                      std::all_of(resized.v.begin(), resized.v.end(),
                                  [](float v) { return std::abs(v - 100) < 1e-3f; }),
                  "skies: blur and resize keep a flat image");
            // Left half covered (200), right half transparent black: the colour carries over.
            Skies::Plane alpha{16, 16, std::vector<float>(256)}, rgb[3];
            for (auto &plane : rgb)
                plane = {16, 16, std::vector<float>(256)};
            for (int i = 0; i < 256; ++i)
                if (i % 16 < 8) {
                    alpha.v[i] = 255;
                    for (auto &plane : rgb)
                        plane.v[i] = 200;
                }
            Skies::FillTransparent(rgb, alpha);
            Check(std::all_of(rgb[0].v.begin(), rgb[0].v.end(),
                              [](float v) { return std::abs(v - 200) < 1; }),
                  "skies: transparent texels take the covered colour");
            // A keyed disc: a smooth edge, solid inside, clear outside.
            Skies::Plane disc{32, 32, std::vector<float>(1024)};
            for (int y = 0; y < 32; ++y)
                for (int x = 0; x < 32; ++x)
                    disc.v[y * 32 + x] =
                        (x - 15.5f) * (x - 15.5f) + (y - 15.5f) * (y - 15.5f) < 144 ? 255.0f : 0.0f;
            const auto edge = Skies::UpscaleAlpha(disc, 64, 64, true);
            const auto between = std::count_if(edge.v.begin(), edge.v.end(),
                                               [](float v) { return v > 5 && v < 250; });
            Check(edge.w == 64 && edge.v[32 * 64 + 32] > 254 && edge.v[0] < 1 && between > 40 &&
                      between < 600,
                  "skies: a keyed edge becomes a narrow smooth one");
            // Folders as the runtime mounts them; nothing to make without the upscaler.
            const auto game = root / "skies" / "CUSA03173";
            fs::create_directories(game / "dvdroot_ps4" / "map");
            fs::create_directories(root / "skies" / "CUSA03173-UPDATE" / "dvdroot_ps4" / "map");
            fs::create_directories(root / "skies" / "CUSA03173-mods");
            const auto maps = Skies::MapFolders(game / "eboot.bin");
            Check(maps.size() == 2 &&
                      maps[1] == root / "skies" / "CUSA03173-UPDATE" / "dvdroot_ps4" / "map",
                  "skies: the game's and its update's map folders");
            Skies::Progress progress;
            Throws([&] { Skies::Make(game, root / "skies-user", root / "no-upscaler", progress); },
                   "skies: no upscaler, nothing made");
            Check(Skies::Look(root / "skies-user").images == 0, "skies: none made");
            // Sharper weapons: the parts folders, their own folder, nothing without the upscaler.
            fs::create_directories(game / "dvdroot_ps4" / "parts");
            const auto parts = Skies::PartsFolders(game / "eboot.bin");
            Check(!parts.empty() && parts[0] == game / "dvdroot_ps4" / "parts",
                  "weapons: the game's parts folder");
            // Weapon-only overlays must not depend on an unrelated map folder.
            const auto update = root / "skies" / "CUSA03173-UPDATE";
            const auto patch = root / "skies" / "CUSA03173-patch";
            const auto mods = root / "skies" / "CUSA03173-mods";
            fs::remove_all(update / "dvdroot_ps4" / "map");
            for (const auto &overlay : {update, patch, mods})
                fs::create_directories(overlay / "dvdroot_ps4" / "parts");
            Check(Skies::PartsFolders(game / "eboot.bin") == std::vector<fs::path>{
                      game / "dvdroot_ps4" / "parts", update / "dvdroot_ps4" / "parts",
                      mods / "dvdroot_ps4" / "parts"},
                  "weapons: parts-only overlays, UPDATE preferred over patch");
            fs::remove_all(update);
            fs::remove_all(game / "dvdroot_ps4" / "map");
            Check(Skies::PartsFolders(game) == std::vector<fs::path>{
                      game / "dvdroot_ps4" / "parts", patch / "dvdroot_ps4" / "parts",
                      mods / "dvdroot_ps4" / "parts"} && Skies::MapFolders(game).empty(),
                  "weapons: patch fallback and base parts without maps");
            Check(Skies::RecipeFor(Skies::Kind::Skies) == 1 &&
                      Skies::RecipeFor(Skies::Kind::Weapons) == 2,
                  "weapons: rebuild old images without invalidating skies");
            Check(Sky::WeaponScaleFor(1024, 256, 4096) == 2 &&
                      Sky::WeaponScaleFor(512, 128, 4096) == 2 &&
                      Sky::WeaponScaleFor(2048, 2048, 4096) == 2 &&
                      Sky::WeaponScaleFor(2049, 512, 4096) == 1 &&
                      Sky::WeaponScaleFor(512, 2049, 4096) == 1 &&
                      Sky::WeaponScaleFor(128, 128, 4096) == 1 &&
                      Sky::WeaponScaleFor(512, 64, 4096) == 1 &&
                      Sky::WeaponScaleFor(0xffffffffu, 256, 4096) == 1,
                  "weapons: exactly 2x within the size bounds, never panorama scaling");
            Check(Skies::Folder(root / "skies-user", Skies::Kind::Weapons) ==
                      root / "skies-user" / "weapon_detail",
                  "weapons: their own folder");
            Throws([&] {
                Skies::Make(game, root / "skies-user", root / "no-upscaler", progress,
                            Skies::Kind::Weapons);
            }, "weapons: no upscaler, nothing made");
            Check(Skies::Look(root / "skies-user", Skies::Kind::Weapons).images == 0,
                  "weapons: none made");
            {
            // The weapon file's pieces: a BND4 holding a TPF with its albedo and normal map.
            namespace Sky = VR::SkyTextures;
            Check(Sky::WeaponFile("wp_a_0800.partsbnd.dcx") && !Sky::WeaponFile("wp_a_0800_l.partsbnd.dcx") &&
                      !Sky::WeaponFile("am_m_1000.partsbnd.dcx"),
                  "weapons: the part files, never the low-detail ones");
            Check(Sky::WeaponWanted("wp_a_0800_a") && !Sky::WeaponWanted("wp_a_0800_n") &&
                      !Sky::WeaponWanted("wp_a_0800_r") && !Sky::WeaponWanted("m21_00_moon_a"),
                  "weapons: only the albedos");
            std::vector<std::uint8_t> tpf(0x200);
            std::memcpy(tpf.data(), "TPF\0", 4);
            tpf[8] = 2;
            tpf[0x0C] = 4;
            const auto entry = [&](int i, std::uint32_t at, std::int32_t size, std::uint8_t format,
                                   std::uint16_t w, std::uint16_t h, std::uint32_t name,
                                   std::uint32_t dxgi) {
                const size_t e = 0x10 + size_t(i) * 0x24;
                std::memcpy(&tpf[e], &at, 4);
                std::memcpy(&tpf[e + 4], &size, 4);
                tpf[e + 8] = format;
                std::memcpy(&tpf[e + 0xC], &w, 2);
                std::memcpy(&tpf[e + 0xE], &h, 2);
                std::memcpy(&tpf[e + 0x18], &name, 4);
                std::memcpy(&tpf[e + 0x20], &dxgi, 4);
            };
            entry(0, 0x100, 0x40, 0, 512, 256, 0x80, 72);
            entry(1, 0x140, 0x40, 108, 512, 256, 0xA0, 71);
            const auto name16 = [&](size_t at, const char *text) {
                for (size_t i = 0; text[i]; ++i)
                    tpf[at + 2 * i] = std::uint8_t(text[i]);
            };
            name16(0x80, "WP_A_0800_a");
            name16(0xA0, "WP_A_0800_n");
            const auto textures = Sky::Textures(tpf);
            Check(textures.size() == 2 && textures[0].name == "wp_a_0800_a" &&
                      textures[0].blocks == Sky::Blocks::Bc1 && textures[0].width == 512 &&
                      textures[0].offset == 0x100 && textures[1].name == "wp_a_0800_n",
                  "weapons: every texture of a TPF, named");
            std::vector<std::uint8_t> bnd(0x100);
            std::memcpy(bnd.data(), "BND4", 4);
            const std::int32_t files = 1;
            const std::int64_t header = 0x24, size = std::int64_t(tpf.size());
            const std::uint32_t at = 0x100, name_at = 0xA0;
            std::memcpy(&bnd[0x0C], &files, 4);
            std::memcpy(&bnd[0x20], &header, 8);
            bnd[0x30] = 1;
            std::memcpy(&bnd[0x40 + 8], &size, 8);
            std::memcpy(&bnd[0x40 + 24], &at, 4);
            std::memcpy(&bnd[0x40 + 32], &name_at, 4);
            const char *path = "N:/SPRJ/parts/WP_A_0800.tpf";
            for (size_t i = 0; path[i]; ++i)
                bnd[name_at + 2 * i] = std::uint8_t(path[i]);
            bnd.insert(bnd.end(), tpf.begin(), tpf.end());
            const auto list = Sky::ReadBnd4(bnd);
            Check(list.size() == 1 && list[0].name == "wp_a_0800.tpf" && list[0].offset == 0x100 &&
                      list[0].size == tpf.size(),
                  "weapons: a BND4's files by their base names");
            bnd.resize(0x120);
            Check(Sky::ReadBnd4(bnd).empty(), "weapons: a truncated BND4 lists nothing");
            }
        }
        std::cout << checks << " checks passed\n";
        fs::remove_all(root);
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "FAIL: " << ex.what() << "\n";
        return 1;
    }
}
