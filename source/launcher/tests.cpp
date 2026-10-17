#include "core.h"
#include <algorithm>
#include <cstdint>
#include <fstream>
#include <iostream>
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
// A param.sfo holding one key, APP_VER.
static void WriteParamSfo(const fs::path &path, const std::string &version) {
    std::string sfo("\0PSF", 4);
    auto u16 = [&](uint16_t v) { sfo += char(v & 0xff), sfo += char(v >> 8); };
    auto u32 = [&](uint32_t v) { u16(uint16_t(v)), u16(uint16_t(v >> 16)); };
    u32(0x0101), u32(36), u32(44), u32(1);         // version, key table, data table, count
    u16(0), u16(0x0204), u32(uint32_t(version.size() + 1)), u32(8), u32(0); // APP_VER entry
    sfo += std::string("APP_VER\0", 8);
    sfo += version + std::string(8 - version.size(), '\0');
    std::ofstream(path, std::ios::binary) << sfo;
}
int main() {
    try {
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
        const auto unicode = root / fs::path(L"game folder \u03a9");
        fs::create_directories(unicode / "sce_sys");
        // The audited frame-limiter bytes at their file offsets; zeros elsewhere.
        const auto limiter_offset = std::streamoff(0x28eb0) + 0x02434ADA - 0x400000;
        {
            std::ofstream eboot(unicode / "eboot.bin", std::ios::binary);
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
            std::fstream eboot(unicode / "eboot.bin", std::ios::binary | std::ios::in | std::ios::out);
            eboot.seekp(limiter_offset);
            eboot.put(byte);
        };
        WriteParamSfo(unicode / "sce_sys" / "param.sfo", "01.00");
        Check(AppVersion(unicode / "sce_sys" / "param.sfo") == "01.00", "APP_VER read");
        Check(!AppVersion(root / "missing.sfo"), "missing param.sfo has no version");
        Check(Path(PathText(unicode)) == unicode, "Unicode path round trip");
        Check(GameExecutable(unicode) == unicode / "eboot.bin", "folder resolution");
        Throws([&] { GameExecutable(root); }, "missing game rejected");
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
                  json["General"]["extra_dmem_in_mbytes"] == 4000 &&
                  json["Vulkan"]["pipeline_cache_enabled"] == true &&
                  json["Vulkan"]["pipeline_cache_archived"] == false &&
                  json["Vulkan"]["vkvalidation_enabled"] == false,
              "headset preset emulator settings");
        Throws([&] { LaunchConfig("{bad", save_home); }, "corrupt config rejected");
        Throws([&] { LaunchConfig("{\"GPU\":3}", save_home); }, "invalid section rejected");
        const std::string user_ini = "# mine\r\nlaser=1\r\nstereo_mode=1\r\nlate_warp=1\r\n";
        const auto launch_ini = LaunchIni(user_ini);
        Check(VR::Ini::ValueOf(launch_ini, "stereo_mode") == "2" &&
                  VR::Ini::ValueOf(launch_ini, "late_warp") == "0" &&
                  VR::Ini::ValueOf(launch_ini, "refresh_hz") == "90" &&
                  VR::Ini::ValueOf(launch_ini, "eye_size") == "auto",
              "renderer keys set for the native preset");
        Check(VR::Ini::ValueOf(launch_ini, "laser") == "1" && launch_ini.starts_with("# mine\r\n"),
              "player keys and comments kept");
        Check(LaunchIni(launch_ini) == launch_ini, "renderer keys are idempotent");
        Check(std::stof(NativeIpd("ipd_mm=60")) == 0.06f, "chosen eye distance reaches renderer");
        Check(std::stof(NativeIpd("ipd_mm=0")) == 0.064f && std::stof(NativeIpd("")) == 0.064f &&
                  std::stof(NativeIpd("ipd_mm=abc")) == 0.064f,
              "headset eye distance falls back to 64 mm");
        auto runtime = root / "shadps4.exe";
        std::ofstream(runtime) << "test";
        std::ofstream(root / "bbvr_patch.xml") << "<Patch/>";
        SetEnvironmentVariableW(L"VR_TEST_LEAK", L"1");
        SetEnvironmentVariableW(L"FAST_TOGGLES", L"0");
        Paths paths{root / "app"};
        Profile profile{PathText(unicode), PathText(save_home), PathText(runtime)};
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
        auto plan = Prepare(profile, paths);
        int argc{};
        auto argv = CommandLineToArgvW(plan.command.c_str(), &argc);
        Check(argc == 8, "launch argument count");
        // The game folder has an Omega in its name: it reaches the game as a path it can read.
        Check(std::wstring(argv[1]) == L"--openxr" &&
                  fs::equivalent(fs::path(argv[7]), unicode / "eboot.bin"),
              "launch arguments point at the game, spaces and all");
        Check(std::wstring(argv[4]) == L"-p" && std::wstring(argv[5]) == L"bbvr_patch.xml" &&
                  Read(paths.User() / "patches" / "bbvr_patch.xml") == "<Patch/>",
              "headset patch passed by name from user/patches");
        LocalFree(argv);
        auto environment = [](const LaunchPlan &p) {
            std::set<std::wstring> out;
            for (const wchar_t *at = p.environment.data(); *at; at += wcslen(at) + 1)
                out.insert(at);
            return out;
        };
        auto env = environment(plan);
        Check(env.contains(L"VR_NATIVE_STEREO=1") && env.contains(L"VR_NATIVE_ASYMMETRIC=1") &&
                  env.contains(L"VR_FAST_MUTEX=1") && env.contains(L"VR_COPY_MERGE_KB=16") &&
                  env.contains(L"VR_DIRECT_BUFFER_METADATA=0"),
              "launch runs the full native asymmetric preset");
        Check(std::any_of(env.begin(), env.end(),
                          [](const std::wstring &e) {
                              return e.starts_with(L"VR_NATIVE_IPD=") &&
                                     std::stof(e.substr(14)) == 0.06f;
                          }),
              "native eye distance from vr.ini");
        Check(!env.contains(L"VR_TEST_LEAK=1") && !env.contains(L"FAST_TOGGLES=0"),
              "inherited renderer switches stripped");
        Check(VR::Ini::ValueOf(Read(paths.Ini()), "stereo_mode") == "2" &&
                  VR::Ini::ValueOf(Read(paths.Ini()), "laser") == "1",
              "launch writes renderer keys and keeps player keys");
        poke('x');
        Write(paths.Ini(), "laser=1\n", Read(paths.Ini()));
        Throws([&] { Prepare(profile, paths); }, "other game builds refused");
        Check(Read(paths.Ini()) == "laser=1\n", "refused build leaves vr.ini untouched");
        poke('\x0f');
        WriteParamSfo(unicode / "sce_sys" / "param.sfo", "01.09");
        Throws([&] { Prepare(profile, paths); }, "patches for another APP_VER refused");
        WriteParamSfo(unicode / "sce_sys" / "param.sfo", "01.00");
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
        Check(Paths::Default().root.filename() == "BloodborneVRData",
              "user data is outside the installer root");
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
        profile.use_bundled_runtime = false;
        profile.Save(paths);
        auto loaded = Profile::Load(paths);
        Check(loaded.game == profile.game && loaded.saves == profile.saves, "profile round trip");
        Check(loaded.runtime == profile.runtime && !loaded.use_bundled_runtime,
              "custom runtime survives profile reload");
        profile.use_bundled_runtime = true;
        profile.Save(paths);
        loaded = Profile::Load(paths);
        const auto bundled = ExecutableDirectory() / "runtime" / "shadps4.exe";
        Check(loaded.use_bundled_runtime &&
                  loaded.runtime ==
                      (fs::is_regular_file(bundled) ? PathText(bundled) : std::string{}),
              "included runtime mode resolves current package instead of stale saved path");
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
            for (const auto &key : row.keys)
                Check(keys.insert(key).second, "settings do not fight over keys");
            for (const auto &option : row.options)
                Check(option.values.size() == row.keys.size(), "multi-key options are complete");
        }
        // Every in-game VR page row that is a plain vr.ini value (vr_page.h Rows), except Smooth
        // motion, which the headset preset turns off.
        for (const char *key :
             {"height_base", "height_adjust_cm", "swap_sticks", "snap_turn_deg", "haptic_gain",
              "controls", "inject", "laser", "grip_adjust", "seated", "body_cutaway", "body_mode",
              "ipd_mm", "hud_vitals", "vitals_heartbeat", "vitals_edge", "ui_counters",
              "hit_vignette", "ui_hud_scale_deg", "player_hp_bonus_pct", "dodge_iframes_extra",
              "melee_kill_ragdoll", "melee_ragdoll_impulse", "melee_stamina_pct", "aa_mode",
              "ui_enemy_hp_bars", "body_align", "body_posture", "body_lean_side_deg",
              "body_comfort", "body_align_ms", "holster_mode", "holster_haptics", "health_color",
              "health_color_strength", "stamina_color", "stamina_color_strength",
              "shoulder_switch", "weapon_wall", "two_hand_guide", "two_hand_swing_hum",
              "gun_wrist_reload", "blade_sounds", "physical_levers", "lamp_travel",
              "vr_grab_attacks", "color_contrast", "color_saturation", "color_gamma"})
            Check(keys.contains(key), "every in-game VR page setting is offered");
        auto row_of = [](const char *key) {
            for (const auto &row : Settings())
                if (row.keys[0] == key)
                    return row;
            throw std::runtime_error(std::string("no row for ") + key);
        };
        // Number rows write what the page writes.
        const auto vibration = row_of("haptic_gain");
        Check(NumberValue(vibration, 7) == "1" && NumberValue(vibration, 10) == "1.429" &&
                  NumberValue(vibration, 1) == "0.1429",
              "vibration slider is the page's n/7");
        Check(NumberShown(vibration, "1") == 7 && NumberShown(vibration, "1.4") == 10 &&
                  NumberShown(vibration, "0.5") == 4 && !NumberShown(vibration, "loud"),
              "vibration file values shown as the page does");
        const auto strength = row_of("health_color_strength");
        Check(NumberValue(strength, 50) == "0.5" && NumberShown(strength, "0.5") == 50,
              "percent sliders hold fractions");
        const auto eyes = row_of("ipd_mm");
        Check(NumberShown(eyes, "0") == 0 && NumberShown(eyes, "41") == 41 &&
                  NumberValue(eyes, 63) == "63",
              "eye distance keeps 0 for Standard");
        Check(NumberValue(row_of("color_saturation"), -100) == "-100", "signed colour values");
        Check(std::stof(NativeIpd("ipd_mm=42")) == 0.042f, "the page's 40..80 mm reaches renderer");
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
            const auto shortened = ArgumentPath(unicode / "eboot.bin");
            Check(std::all_of(shortened.begin(), shortened.end(), [](wchar_t c) { return c < 128; }) ||
                      shortened == (unicode / "eboot.bin").native(),
                  "non-ASCII paths become their short name when Windows has one");
        }
        Check(SameValue("0.50", "0.5") && SameValue("true", "1") && SameValue("false", "0") &&
                  !SameValue("1", "true") && !SameValue("look_down", "always"),
              "choices match like the in-game page");
        Check(!NumberShown(row_of("ipd_mm"), "95") && !NumberShown(row_of("ipd_mm"), "30"),
              "values outside the slider show as Custom");

        // graphics_config: the game's ten fields, defaults for anything it would reject.
        Check(ParseGraphics(std::nullopt) == DefaultGraphics, "absent graphics is the default");
        Check(GraphicsText(DefaultGraphics) == "1,0,0,1,1,0,1,100,80,85", "graphics text");
        const GraphicsFields dlss{1, -20, 1, 2, 1, 0, 1, 100, 80, 85};
        Check(ParseGraphics(GraphicsText(dlss)) == dlss, "graphics round trip");
        for (const char *bad : {"", "1,0,0", "2,0,0,1,1,0,1,100,80,85", "1,5,0,1,1,0,1,100,80,85",
                                "1,0,3,1,1,0,1,100,80,85", "1,0,0,1,1,0,1,100,80,101",
                                "1,0,0,1,1,0,1,100,80,85,7", "1, 0,0,1,1,0,1,100,80,85"})
            Check(ParseGraphics(std::string(bad)) == DefaultGraphics, "invalid graphics rejected");

        // Exit codes, driver versions.
        Check(ExitReason(0).empty() && ExitReason(0xC0000005) == "a memory access error" &&
                  ExitReason(0x80000003) == "an internal check failed" &&
                  ExitReason(0x12345678) == "error 0x12345678",
              "exit reasons");
        Check(NvidiaDriver((32ull << 48) | (0ull << 32) | (15ull << 16) | 6094) == "560.94",
              "NVIDIA driver version");

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
        std::cout << checks << " checks passed\n";
        fs::remove_all(root);
        return 0;
    } catch (const std::exception &ex) {
        std::cerr << "FAIL: " << ex.what() << "\n";
        return 1;
    }
}
