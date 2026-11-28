// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "core.h"
#include "mods.h"
#include "sky_upscale.h"
#include "core/file_sys/mod_layers.h"
#include <miniz.h>
#include <objbase.h>
#include <fstream>
#include <iostream>
#include <iterator>

using namespace Launcher;
extern "C" __declspec(dllexport) void BldbrneVRManagedModsV1() {}
static void Expect(bool v, const char* what) {
    if (!v) throw std::runtime_error(what);
}
template<class F> static void Reject(F f, std::string_view message = {}) {
    try { f(); } catch (const std::exception& e) {
        if (std::string_view(e.what()).find(message) == std::string_view::npos) throw;
        return;
    }
    throw std::runtime_error("Expected operation to fail");
}
static void MakeZip(const fs::path& p, std::initializer_list<const char*> names) {
    mz_zip_archive zip{};
    Expect(mz_zip_writer_init_file(&zip, p.string().c_str(), 0), "create ZIP");
    std::vector<std::pair<std::string,std::string>> replacements;
    for (auto name : names) {
        // Construct deliberately malformed names even when miniz's writer rejects them.
        auto safe = std::string(strlen(name), char('a' + replacements.size()));
        replacements.emplace_back(safe, name);
        if (!mz_zip_writer_add_mem(&zip, safe.c_str(), "mod", 3, MZ_BEST_SPEED)) {
            mz_zip_writer_end(&zip);
            throw std::runtime_error("Could not write ZIP fixture");
        }
    }
    Expect(mz_zip_writer_finalize_archive(&zip) && mz_zip_writer_end(&zip), "close ZIP");
    std::ifstream in(p, std::ios::binary);
    std::string bytes{std::istreambuf_iterator<char>(in), {}};
    in.close();
    for (const auto& [safe,name] : replacements) {
        for (size_t at = 0; (at = bytes.find(safe,at)) != bytes.npos; at += safe.size())
            bytes.replace(at, safe.size(), name);
    }
    std::ofstream(p, std::ios::binary).write(bytes.data(), bytes.size());
}
int wmain(int argc, wchar_t** argv) {
    GUID guid{};
    CoCreateGuid(&guid);
    wchar_t id[40]{};
    StringFromGUID2(guid, id, 40);
    const fs::path root = fs::temp_directory_path() / id;
    if (!fs::create_directory(root)) return 1;
    int result = 0;
    try {
        const auto game = root / "CUSA00900", legacy = root / "CUSA00900-mods";
        std::ofstream(root / "packed.zar") << "test";
        Expect(Mods::GameRoot(root / "packed-UPDATE" / "eboot.bin") == root / "packed",
               "selected update must resolve mods next to its packed base game");
        const fs::path file = "dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx";
        fs::create_directories((game / file).parent_path());
        fs::create_directories((legacy / file).parent_path());
        std::ofstream(game / file) << "base";
        std::ofstream(legacy / file) << "legacy";
        auto zip = root / "Test.zip";
        MakeZip(zip, {"wrapper/dvdroot_ps4/param/gameparam/gameparam.parambnd.dcx",
                      "wrapper/dvdroot_ps4/chr/new.dcx", "wrapper/readme.txt"});
        auto pkg = Mods::Import(game, zip);
        Expect(!pkg.enabled && GameMods::Active(game).empty(), "import should be disabled");
        auto report = Mods::Scan(game, pkg);
        Expect(report.files == 2 && report.overrides.size() == 1 && report.shared.size() == 1, "scan");
        Expect(report.has_file_list, "new imports should verify their ZIP file list");
        const fs::path actor = "dvdroot_ps4/chr/new.dcx";
        std::ofstream(pkg.path / "content" / actor) << "x";
        Reject([&] { Mods::Toggle(game, pkg, report); }, "Wrong size for dvdroot_ps4/chr/new.dcx");
        std::ofstream(pkg.path / "content" / actor) << "mod";
        Reject([&] { Mods::Toggle(game, pkg, {}); });
        auto enabled = Mods::Toggle(game, pkg, report);
        auto on = GameMods::Active(game).at(0);
        Expect(enabled.path == on && enabled.enabled, "selection after enabling");
        Expect(Read(on / "content" / file) == "mod" && Read(legacy / file) == "legacy", "overlay");
        const auto runtime = fs::absolute(argv[0]);
        auto textures = root / "Textures.zip";
        MakeZip(textures, {"map/m29/test.tpf", "parts/test.dcx"});
        auto texture_mod = Mods::Import(game, textures);
        auto texture_on = Mods::Toggle(game, texture_mod, Mods::Scan(game, texture_mod));
        for (const auto& selected : {root / "CUSA00900-UPDATE", root / "CUSA00900-patch", legacy}) {
            fs::create_directories(selected);
            for (const auto& input : {selected, selected / "eboot.bin"}) {
                const auto maps = Skies::MapFolders(input);
                const auto parts = Skies::PartsFolders(input);
                Expect(maps == Skies::MapFolders(game) && !maps.empty() &&
                       maps.back() == texture_on.path / "content/dvdroot_ps4/map", "selected overlay missed mod skies");
                Expect(parts == Skies::PartsFolders(game) && !parts.empty() &&
                       parts.back() == texture_on.path / "content/dvdroot_ps4/parts", "selected overlay missed mod weapons");
            }
        }
        Mods::Remove(game, Mods::Toggle(game, texture_on, {}));
        Expect(Skies::MapFolders(root / "CUSA00900-UPDATE").empty(), "disabled skies still discovered");
        Mods::CheckForLaunch(game,runtime);
        fs::remove(on / "content" / actor);
        Reject([&] { Mods::CheckForLaunch(game, runtime); }, "Missing dvdroot_ps4/chr/new.dcx");
        std::ofstream(on / "content" / actor) << "mod";
        Mods::CheckForLaunch(game,runtime);
        const auto active_list = on / "bbvr-import-v1/files.txt";
        const auto saved_list = root / "test-file-list";
        fs::rename(active_list, saved_list);
        Reject([&] { Mods::CheckForLaunch(game, runtime); }, "file list for Test is damaged");
        fs::rename(saved_list, active_list);
        Reject([&] { Mods::CheckForLaunch(game,root/"old.exe"); });
        fs::create_directories(legacy / "dvdroot_ps4/chr");
        std::ofstream(legacy / "dvdroot_ps4/chr/new.dcx") << "other";
        Reject([&] { Mods::CheckForLaunch(game,runtime); });
        auto other = root / "Other.zip";
        MakeZip(other, {"param/gameparam/gameparam.parambnd.dcx"});
        auto second = Mods::Import(game, other);
        auto clash = Mods::Scan(game, second);
        Expect(clash.conflicts.size() == 1, "managed conflict");
        Reject([&] { Mods::Toggle(game, second, clash); });
        auto disabled = Mods::Toggle(game, enabled, {});
        Expect(disabled.path == pkg.path && !disabled.enabled, "selection after disabling");
        Expect(GameMods::Active(game).empty() && Read(game / file) == "base", "disable");
        Reject([&] { Mods::Remove(game, {legacy,false}); });
        Mods::Remove(game,pkg);
        Expect(!fs::exists(pkg.path) && fs::exists(zip) && Read(legacy/file) == "legacy", "remove imported copy only");
        Reject([&] { Mods::Toggle(game, {legacy,true}, {}); });
        Expect(Read(legacy / file) == "legacy", "external files must remain active");
        auto hasPackage = [&](const fs::path& path) {
            const auto list = Mods::List(game);
            return std::ranges::any_of(list, [&](const auto& p) { return p.path == path; });
        };
        Expect(!hasPackage(legacy), "existing game files must not appear as an imported mod");
        const auto packed = root / "CUSA00900-mods.zar";
        std::ofstream(packed) << "packed";
        Expect(!hasPackage(packed) && Mods::Scan(game, second).conflicts.empty(), "folder takes priority over packed overlay");
        const auto aside = root / "fixture-loose";
        fs::rename(legacy, aside);
        Reject([&] { Mods::Toggle(game, second, {}); }, ".zar");
        Reject([&] { Mods::Toggle(game, {packed,true}, {}); });
        Expect(Read(packed) == "packed", "packed overlay must remain untouched");
        fs::rename(aside, legacy);
        // Missing payload: launch should identify the broken mod, and the user can still disable it.
        pkg = Mods::Import(game, zip);
        Mods::Toggle(game, pkg, Mods::Scan(game, pkg));
        on = GameMods::Active(game).at(0);
        fs::rename(on / "content", root / "misplaced-content");
        Expect(hasPackage(on) && hasPackage(second.path), "damaged mod hid itself or a sibling");
        Reject([&] { Mods::CheckForLaunch(game, runtime); }, "game files for Test");
        Mods::Toggle(game, {on, true}, {});
        Mods::Remove(game, pkg);
        Mods::CheckForLaunch(game, runtime);

        pkg = Mods::Import(game, zip);
        const auto intact = Mods::Scan(game, pkg);
        const auto list_path = pkg.path / "bbvr-import-v1/files.txt";
        std::ofstream(list_path, std::ios::binary) << "3\tdvdroot_ps4/chr/new.dcx\n";
        Reject([&] { Mods::Scan(game, pkg); }, "file list for Test is damaged");
        std::ofstream(list_path, std::ios::binary) << "3\tdvdroot_ps4/chr/new.dcx\nend\t2\n";
        Reject([&] { Mods::Scan(game, pkg); }, "file list for Test is damaged");
        // Older imported packages had only the ownership marker and their payload.
        fs::remove(list_path);
        fs::remove(list_path.parent_path());
        Reject([&] { Mods::Toggle(game, pkg, intact); }, "file list disappeared");
        auto old_report = Mods::Scan(game, pkg);
        Expect(!old_report.has_file_list && old_report.files == 2, "older imports remain reviewable");
        enabled = Mods::Toggle(game, pkg, old_report);
        Mods::CheckForLaunch(game, runtime);
        Mods::Remove(game, Mods::Toggle(game, enabled, {}));

        // Interruption after extraction started: only the unpublished copy can be removed.
        pkg = Mods::Import(game, zip);
        auto stage = pkg.path; stage.replace_extension(".importing");
        fs::rename(pkg.path, stage);
        Expect(hasPackage(stage) && GameMods::Active(game).empty(), "unfinished import must be visible but inactive");
        Reject([&] { Mods::Import(game, zip); }, "earlier operation");
        Reject([&] { Mods::Toggle(game, {stage}, {}); }, "unfinished");
        Mods::Remove(game, {stage});
        fs::create_directory(stage);
        std::ofstream(stage / "bbvr-package") << "Bldbrne";
        Expect(hasPackage(stage), "interrupted marker creation");
        Mods::Remove(game, {stage});
        fs::create_directory(stage);
        std::ofstream(stage / "unrelated.txt") << "leave alone";
        Expect(!hasPackage(stage), "unowned folder listed for removal");
        Reject([&] { Mods::Import(game, zip); }, "Rename your ZIP");
        Reject([&] { Mods::Remove(game, {stage}); }, "ownership");
        Expect(Read(stage / "unrelated.txt") == "leave alone", "unowned file was changed");
        fs::rename(stage, root / "unrelated-folder");

        // Windows sharing violation after removal began. The entry must survive a retry.
        pkg = Mods::Import(game, zip);
        auto deleting = pkg.path; deleting.replace_extension(".removing");
        fs::rename(pkg.path, deleting);
        const auto handle = CreateFileW((deleting / "content" / file).c_str(), GENERIC_READ,
                                        FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        Expect(handle != INVALID_HANDLE_VALUE, "could not lock removal fixture");
        {
            std::unique_ptr<void, decltype(&CloseHandle)> close(handle, CloseHandle);
            Reject([&] { Mods::Remove(game, {deleting}); }, "Retry removal");
            Expect(hasPackage(deleting) && GameMods::Active(game).empty(), "lost interrupted removal");
        }
        Mods::Remove(game, {deleting});
        Expect(!fs::exists(deleting) && fs::exists(zip) && Read(legacy / file) == "legacy", "retry touched original files");
        fs::create_directory(deleting); // interruption after the marker was deleted
        Expect(hasPackage(deleting), "empty removal stage hidden");
        Mods::Remove(game, {deleting});
        auto fake = pkg.path; fake.replace_extension(".on");
        fs::create_directories(fake / "content" / "dvdroot_ps4");
        std::ofstream(fake / "bbvr-package") << std::string(GameMods::Owner.size(), 'x');
        Expect(!hasPackage(fake) && GameMods::Active(game).empty(), "runtime accepted a foreign marker");
        for (const auto name : {"../chr/test.dcx","chr/../test.dcx","chr/CON.dcx","chr/x.dcx:stream",
                                "chr/bad.exe","/chr/test.dcx","chr/te~1.dcx"}) {
            auto bad = root / "Bad.zip";
            MakeZip(bad, {name});
            Reject([&] { Mods::Import(game,bad); });
        }
        auto bad = root / "Bad.zip";
        MakeZip(bad, {"chr/part.dcx", "chr/part.dcx/piece.dcx"});
        Reject([&] { Mods::Import(game,bad); }, "same path for a file and a folder");
        MakeZip(bad, {"chr/part.dcx"});
        auto retried = Mods::Import(game,bad);
        Mods::Remove(game,retried);
        MakeZip(bad, {"chr/a.dcx","CHR/A.DCX"});
        Reject([&] { Mods::Import(game,bad); });
        MakeZip(bad, {"option1/chr/a.dcx","option2/chr/b.dcx"});
        Reject([&] { Mods::Import(game,bad); });
        if (argc == 3) {
            // Exercise a user's ZIP against the pathname layout of their installation, read-only.
            const auto mock = root / "second", overlay = root / "second-mods";
            fs::create_directory(mock);
            auto sample = Mods::Import(mock, argv[1]);
            auto content = sample.path / "content";
            for (const auto& entry : fs::recursive_directory_iterator(content)) {
                if (!entry.is_regular_file()) continue;
                auto rel = entry.path().lexically_relative(content);
                if (fs::is_regular_file(fs::path(argv[2])/rel)) {
                    fs::create_directories((overlay/rel).parent_path());
                    std::ofstream(overlay/rel) << "installed";
                }
            }
            auto r = Mods::Scan(mock, sample);
            std::cout << r.files << " assets; " << r.overrides.size() << " external overlaps\n";
            for (const auto& s : r.overrides) std::cout << s << '\n';
            Mods::Toggle(mock,sample,r);
            Mods::CheckForLaunch(mock,runtime);
            Mods::Toggle(mock,{GameMods::Active(mock).at(0),true},{});
            Mods::Remove(mock,sample);
            Expect(!fs::exists(sample.path) && fs::exists(argv[1]), "remove sample and keep source ZIP");
        }
        std::cout << "Mod manager tests passed\n";
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        result = 1;
    }
    fs::remove_all(root);
    return result;
}
