// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "mods.h"
#include "core.h"
#include "core/file_sys/mod_layers.h"
#include <miniz.h>
#include <algorithm>
#include <charconv>
#include <cstdio>
#include <map>
#include <memory>
#include <set>

namespace Launcher::Mods {
namespace {
using File = std::unique_ptr<FILE, decltype(&fclose)>;
constexpr unsigned MaxEntries = 20000;
constexpr uint64_t MaxBytes = 8ull << 30;
constexpr char ImportMetadata[] = "bbvr-import-v1";
constexpr char FileList[] = "files.txt";

std::string Lower(std::string s) {
    for (auto& c : s) if (c >= 'A' && c <= 'Z') c += 'a' - 'A';
    return s;
}
void Check(bool ok, const std::string& why) {
    if (!ok) throw std::runtime_error(why);
}
bool Reparse(const fs::path& p) {
    auto flags = GetFileAttributesW(p.c_str());
    Check(flags != INVALID_FILE_ATTRIBUTES, "Cannot inspect " + PathText(p));
    return (flags & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}
void PlainLibrary(const fs::path& p) {
    if (fs::exists(p)) Check(fs::is_directory(p) && !Reparse(p),
        "The mod library must be a regular folder. Check this location: " + PathText(p));
}
bool SafePart(const std::string& s) {
    if (s.empty() || s.size() > 180 || s.back() == '.' || s.back() == ' ' ||
        s.find_first_of(":<>|\"?*~") != s.npos) return false;
    for (unsigned char c : s) if (c < 32 || c >= 127) return false;
    const auto base = Lower(s.substr(0, s.find('.')));
    return base != "aux" && base != "con" && base != "prn" && base != "nul" &&
           base != "conin$" && base != "conout$" &&
           !(base.size() == 4 && (base.starts_with("com") || base.starts_with("lpt")) &&
             base.back() >= '0' && base.back() <= '9');
}
bool AssetDir(const std::string& s) {
    static const std::set<std::string> dirs{"chr","event","facegen","font","map","menu","msg","mtd",
        "obj","other","param","paramdef","parts","remo","script","sfx","shader","sound"};
    return dirs.contains(s);
}
// Accept loose game data. Unknown installers or archives needing a merge cannot be partly installed.
bool AssetFile(const std::string& s) {
    static const std::set<std::string> types{".dcx",".fev",".fsb",".tpf",".tpfbhd",".tpfbdt",
        ".gfx",".gnf",".hkx",".dds",".emevd",".msb",".arealoadlist",".bnk",".wem"};
    if (s.ends_with(".tpfbhd.patch") || s.ends_with(".tpfbdt.patch")) return true;
    return types.contains(fs::path(s).extension().string());
}
fs::path Legacy(const fs::path& game) { auto p = game; return p += L"-mods"; }
struct Archive {
    File file;
    mz_zip_archive zip{};
    explicit Archive(const fs::path& path) : file(_wfopen(path.c_str(), L"rb"), fclose) {
        // miniz reads the ZIP directory before exposing its entry count. Bound its allocations
        // even for a malformed ZIP64 header. File contents themselves are streamed to disk.
        // (static: the captureless allocators below may use it; clang-cl rejects a local one.)
        static constexpr auto bounded = [](size_t count, size_t size) {
            return size && count <= (16u << 20) / size;
        };
        zip.m_pAlloc = [](void*, size_t count, size_t size) -> void* {
            return bounded(count, size) ? std::calloc(count, size) : nullptr;
        };
        zip.m_pRealloc = [](void*, void* p, size_t count, size_t size) -> void* {
            return bounded(count, size) ? std::realloc(p, count * size) : nullptr;
        };
        zip.m_pFree = [](void*, void* p) { std::free(p); };
        Check(file && fs::file_size(path) <= MaxBytes && mz_zip_reader_init_cfile(&zip, file.get(), 0, 0),
              "Cannot open this ZIP (maximum 8 GiB).");
    }
    ~Archive() { mz_zip_reader_end(&zip); }
};
struct Entry { unsigned index; fs::path dest; uint64_t size; };
std::vector<Entry> Entries(Archive& a) {
    const auto count = mz_zip_reader_get_num_files(&a.zip);
    Check(count <= MaxEntries, "This ZIP exceeds the limit of 20,000 entries. Choose a smaller mod package.");
    std::vector<Entry> result;
    std::set<std::string> seen;
    std::optional<std::string> wrapper;
    uint64_t total = 0;
    for (unsigned i = 0; i < count; ++i) {
        mz_zip_archive_file_stat st{};
        const auto len = mz_zip_reader_get_filename(&a.zip, i, nullptr, 0);
        Check(len > 1 && len <= 240 && mz_zip_reader_file_stat(&a.zip, i, &st), "Invalid ZIP entry.");
        std::string name(len, '\0');
        mz_zip_reader_get_filename(&a.zip, i, name.data(), len);
        name.pop_back();
        std::replace(name.begin(), name.end(), '\\', '/');
        const unsigned kind = (st.m_external_attr >> 16) & 0170000;
        Check((kind == 0 || kind == 0100000 || kind == 0040000) &&
              !(st.m_external_attr & FILE_ATTRIBUTE_REPARSE_POINT), "Links in a mod ZIP are unsupported.");
        if (name.ends_with('/')) name.pop_back();
        std::vector<std::string> parts;
        for (size_t start = 0;;) {
            auto end = name.find('/', start);
            auto part = Lower(name.substr(start, end - start));
            Check(SafePart(part), "Unsafe or unsupported ZIP path: " + name);
            parts.push_back(std::move(part));
            if (end == name.npos) break;
            start = end + 1;
        }
        if (st.m_is_directory) continue;
        const auto& leaf = parts.back();
        if (leaf.ends_with(".txt") || leaf.ends_with(".md")) continue;
        size_t first = 0;
        while (first + 1 < parts.size() && !AssetDir(parts[first])) ++first;
        Check(first + 1 < parts.size() && AssetFile(leaf), "Unsupported mod layout or file: " + name);
        std::string prefix, rel = "dvdroot_ps4";
        for (size_t n = 0; n < first; ++n) prefix += parts[n] + "/";
        Check(!wrapper || *wrapper == prefix, "This ZIP contains more than one version or game folder. Import a ZIP containing only the version you want.");
        wrapper = prefix;
        for (size_t n = first; n < parts.size(); ++n) rel += "/" + parts[n];
        Check(seen.insert(rel).second, "Duplicate ZIP destination: " + rel);
        Check(st.m_is_supported && !st.m_is_encrypted && (st.m_method != 0 || st.m_comp_size == st.m_uncomp_size) &&
              st.m_uncomp_size <= (2ull << 30) &&
              st.m_uncomp_size <= MaxBytes - total, "Unsupported or oversized ZIP member: " + name);
        total += st.m_uncomp_size;
        result.push_back({i, fs::path(rel), st.m_uncomp_size});
    }
    Check(!result.empty(), "No Bloodborne game files were found. Choose a loose-file mod ZIP containing folders such as chr, map, event or param.");
    return result;
}

// Names in this list are lookup keys only. All filesystem paths come from the directory walk.
using FileSizes = std::map<std::string, uint64_t>;
std::optional<FileSizes> ReadFileList(const fs::path& package) {
    const auto metadata = package / ImportMetadata;
    const auto status = fs::symlink_status(metadata);
    if (!fs::exists(status)) return std::nullopt; // older packages have no versioned metadata folder
    const auto damaged = "The imported file list for " + PathText(package.stem()) +
                         " is damaged. Disable the mod if enabled, remove the imported copy, then import its ZIP again.";
    Check(fs::is_directory(status) && !Reparse(metadata), damaged);
    const auto path = metadata / FileList;
    Check(fs::is_regular_file(fs::symlink_status(path)) && !Reparse(path), damaged);
    std::ifstream in(path, std::ios::binary);
    FileSizes files;
    // ZIP names are at most 239 bytes plus our dvdroot_ps4 prefix, size and separator.
    char line[280];
    while (in.getline(line, sizeof(line))) {
        if (in.eof()) break; // every record, including the terminator, ends in a newline
        const std::string_view row(line, static_cast<size_t>(in.gcount() - 1));
        if (row.starts_with("end\t")) {
            Check(!files.empty() && row.substr(4) == std::to_string(files.size()) &&
                  in.peek() == std::char_traits<char>::eof() && !in.bad(), damaged);
            return files;
        }
        const auto sep = row.find('\t');
        Check(sep != row.npos && sep + 1 < row.size() && files.size() < MaxEntries, damaged);
        uint64_t size{};
        const auto [ptr, ec] = std::from_chars(row.data(), row.data() + sep, size);
        Check(ec == std::errc{} && ptr == row.data() + sep && size <= (2ull << 30) &&
              files.emplace(std::string(row.substr(sep + 1)), size).second, damaged);
    }
    throw std::runtime_error(damaged);
}
bool Present(const fs::path& path) { return fs::exists(path); }
// A crash can interrupt writing the very first marker. No assets have been extracted then.
// Permit only a nonrecursive cleanup of the empty folder or its lone partial marker.
bool EmptyStage(const Package& p) {
    if (!p.Interrupted()) return false;
    std::error_code ec;
    auto it = fs::directory_iterator(p.path, ec);
    if (ec) return false;
    if (it == fs::directory_iterator{}) return true;
    const auto marker = it->path();
    if (p.path.extension() != ".importing" || marker.filename() != "bbvr-package" ||
        !fs::is_regular_file(it->symlink_status(ec))) return false;
    const auto size = it->file_size(ec);
    if (ec || size >= GameMods::Owner.size() || it.increment(ec) != fs::directory_iterator{} || ec) return false;
    char prefix[GameMods::Owner.size()]{};
    std::ifstream in(marker, std::ios::binary);
    return in.read(prefix, static_cast<std::streamsize>(size)) &&
           GameMods::Owner.starts_with(std::string_view(prefix, static_cast<size_t>(size)));
}
}

fs::path GameRoot(const fs::path& exe) {
    auto p = fs::absolute(exe).parent_path();
    auto name = p.filename().wstring();
    for (const std::wstring suffix : {L"-mods", L"-patch", L"-UPDATE"}) {
        if (name.ends_with(suffix)) {
            auto base = p.parent_path() / name.substr(0, name.size() - suffix.size());
            auto archive = base;
            archive += L".zar";
            if (fs::is_directory(base) || fs::is_regular_file(archive)) return base;
        }
    }
    return p;
}

std::vector<Package> List(const fs::path& game) {
    std::vector<Package> out;
    const auto lib = GameMods::Library(game);
    PlainLibrary(lib);
    if (Present(lib)) {
        for (const auto& entry : fs::directory_iterator(lib)) {
            const auto& p = entry.path();
            Package item{p, p.extension() == ".on"};
            if (!item.enabled && p.extension() != ".off" && !item.Interrupted()) continue;
            if (!GameMods::PlainDirectory(p)) continue;
            if (!GameMods::Owned(p) && !EmptyStage(item)) continue;
            out.push_back(item);
        }
        std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.path < b.path; });
    }
    return out;
}

Package Import(const fs::path& game, const fs::path& zip) {
    Check(_wcsicmp(zip.extension().c_str(), L".zip") == 0, "Choose a ZIP file.");
    Archive archive(zip);
    const auto entries = Entries(archive);
    const auto root = GameMods::Library(game);
    PlainLibrary(root);
    fs::create_directories(root);
    auto label = zip.stem();
    Check(!label.empty() && label.native().size() <= 160, "Please shorten the mod ZIP name.");
    auto dest = root / label; dest += ".off";
    auto active = root / label; active += ".on";
    auto stage = dest; stage.replace_extension(".importing");
    auto deleting = dest; deleting.replace_extension(".removing");
    for (const auto& path : {stage, deleting}) {
        if (!Present(path)) continue;
        Package unfinished{path};
        Check(fs::is_directory(path) && !Reparse(path) && (GameMods::Owned(path) || EmptyStage(unfinished)),
              "This ZIP name is already used by an unrecognized file or folder. Rename your ZIP or move this location aside: " + PathText(path));
        throw std::runtime_error("An earlier operation on this mod was interrupted. On Mods, select Rescan, review the unfinished entry and remove it before importing again.");
    }
    for (const auto& existing : {dest, active}) {
        if (!Present(existing)) continue;
        Check(GameMods::Owned(existing), "A folder already uses this mod name, but the launcher cannot identify its owner. Rename your ZIP or move this folder aside: " + PathText(existing));
        throw std::runtime_error("A mod with this ZIP name is already installed. To replace it, disable and remove its imported copy first.");
    }
    Check(fs::create_directory(stage), "This ZIP is already being imported. Wait for it to finish.");
    try {
        // Write the ownership marker before extracting the first asset so an interrupted import is recoverable.
        File marker(_wfopen((stage / "bbvr-package").c_str(), L"wbx"), fclose);
        const auto owner = GameMods::Owner;
        Check(marker && fwrite(owner.data(), 1, owner.size(), marker.get()) == owner.size() &&
              fclose(marker.release()) == 0, "Cannot start the import. Check the free space and write access beside the game folder.");
        const auto metadata = stage / ImportMetadata;
        fs::create_directory(metadata);
        std::ofstream file_list(metadata / FileList, std::ios::binary);
        Check(file_list.is_open(), "Could not create the imported file list. Check free space and write access, then try again.");
        for (const auto& e : entries) {
            const auto path = stage / "content" / e.dest;
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            Check(!ec, "Could not create the folder for " + PathText(e.dest) +
                  ". Check free space, write access, and whether the ZIP uses the same path for a file and a folder.");
            File file(_wfopen(path.c_str(), L"wbx"), fclose);
            Check(file && mz_zip_reader_extract_to_cfile(&archive.zip, e.index, file.get(), 0) &&
                  fclose(file.release()) == 0, "Could not extract " + PathText(e.dest) + ". Check free disk space and try a fresh copy of the ZIP.");
            file_list << e.size << '\t' << e.dest.generic_string() << '\n';
        }
        file_list << "end\t" << entries.size() << '\n';
        file_list.close();
        Check(file_list.good(), "Could not save the imported file list. Check free disk space and try again.");
        std::error_code ec;
        fs::rename(stage, dest, ec);
        Check(!ec, "Could not finish importing " + PathText(label) + ". Close programs using the mod folder, select Rescan and try again.");
    } catch (...) {
        // Use the same marker-last cleanup as manual removal. A locked file during rollback
        // must leave an identifiable, inactive copy even if some siblings were already removed.
        try { Remove(game, {stage}); }
        catch (const std::exception&) {
            throw std::runtime_error("Import did not finish. Close programs using the imported files, select Rescan and remove the unfinished copy before trying again.");
        }
        throw;
    }
    return {dest, false};
}

static void RequireListed(const std::vector<Package>& packages, const Package& pkg) {
    Check(std::ranges::any_of(packages, [&](const auto& p) {
        return p.path == pkg.path && p.enabled == pkg.enabled;
    }), "The installed mods changed. Select Rescan, then review the mod again.");
}
Report Scan(const fs::path& game, const Package& pkg) {
    Report report;
    const auto packages = List(game);
    RequireListed(packages, pkg);
    if (pkg.Interrupted()) return report;
    struct Layer { fs::path root; std::string mod_name; };
    std::vector<Layer> layers;
    for (const auto& p : packages) {
        if (p.enabled && p.path != pkg.path) layers.push_back({p.path / "content", PathText(p.path.stem())});
    }
    // Match the runtime's directory-first lookup. This overlay is only a source for conflict checks.
    auto legacy = Legacy(game);
    if (fs::is_directory(legacy)) layers.push_back({legacy, {}});
    else if (fs::is_regular_file(legacy += L".zar"))
        report.conflicts.push_back("Your existing shadPS4 files are in a .zar archive. The launcher cannot check their contents for conflicts, so this mod cannot be enabled with this setup.");
    const auto content = pkg.path / "content";
    Check(fs::is_directory(content) && !Reparse(content) && fs::is_directory(content / "dvdroot_ps4") &&
          !Reparse(content / "dvdroot_ps4"), "The game files for " + PathText(pkg.path.stem()) +
          " are missing or linked elsewhere. Disable the mod if enabled, remove the imported copy, then import its ZIP again.");
    auto expected = ReadFileList(pkg.path);
    report.has_file_list = expected.has_value();
    const auto repair = " in " + PathText(pkg.path.stem()) +
                        ". Disable the mod if enabled, remove its imported copy, then import its ZIP again.";
    for (const auto& entry : fs::recursive_directory_iterator(content / "dvdroot_ps4")) {
        Check(!Reparse(entry.path()), "Package contains a link: " + PathText(entry.path()));
        if (!entry.is_regular_file()) continue;
        ++report.files;
        const auto rel = entry.path().lexically_relative(content);
        const auto text = Utf8(rel.generic_wstring());
        if (expected) {
            const auto found = expected->find(Lower(text));
            if (found != expected->end()) {
                if (entry.file_size() != found->second)
                    throw std::runtime_error("Wrong size for " + text + repair);
                expected->erase(found);
            }
        }
        if (text.ends_with("gameparam.parambnd.dcx") || text.ends_with("item.msgbnd.dcx"))
            report.shared.push_back(text);
        for (const auto& [root, mod_name] : layers) {
            auto parent = (root / rel).parent_path();
            while (parent != root && !Present(parent)) parent = parent.parent_path();
            if (!fs::is_directory(parent) || fs::is_directory(root / rel)) {
                report.conflicts.push_back(text + (mod_name.empty()
                    ? " is a file in one installation and a folder in the other. This mod is incompatible with the existing shadPS4 files."
                    : " is a file in one mod and a folder in another. Disable the other mod first (" + mod_name + ")."));
            } else if (Present(root / rel)) {
                if (mod_name.empty()) report.overrides.push_back(text);
                else report.conflicts.push_back(text + " is also supplied by " + mod_name + ". Only one of these mods can be enabled.");
            }
        }
    }
    if (expected && !expected->empty())
        throw std::runtime_error("Missing " + expected->begin()->first + repair);
    std::sort(report.overrides.begin(), report.overrides.end());
    Check(report.files != 0, PathText(pkg.path.stem()) + " contains no game files. Disable it if enabled, remove the imported copy, then import its ZIP again.");
    return report;
}
Package Toggle(const fs::path& game, const Package& p, const Report& reviewed) {
    Check(!p.Interrupted(), "Remove the unfinished copy before importing the ZIP again.");
    RequireListed(List(game), p);
    Report current;
    if (!p.enabled) {
        current = Scan(game, p);
        if (!current.conflicts.empty()) throw std::runtime_error(current.conflicts.front());
        Check(!reviewed.has_file_list || current.has_file_list,
              "The original file list disappeared after review. Remove the imported copy and import the ZIP again.");
        Check(current.overrides == reviewed.overrides, "The files this mod would replace have changed. Select Rescan and review the new list before enabling.");
    }
    auto dest = p.path;
    dest.replace_extension(p.enabled ? ".off" : ".on");
    Check(!Present(dest), "Another file or folder already uses the destination. Check this location and select Rescan: " + PathText(dest));
    if (!p.enabled) {
        // Remember the exact external filenames the user saw. A later external mod must be reviewed.
        const auto ack = p.path / "overrides.json";
        const auto choices = Json(current.overrides).dump();
        Check(choices.size() <= 4 * 1024 * 1024, "Too many files overlap your existing shadPS4 installation to save these choices. Leave this mod disabled or import a smaller compatible package.");
        if (Present(ack))
            Check(fs::is_regular_file(ack) && !Reparse(ack) && fs::file_size(ack) <= 4 * 1024 * 1024,
                  "This mod's saved choices are damaged. Remove its disabled copy and import the ZIP again.");
        Write(ack, choices);
    }
    std::error_code ec;
    fs::rename(p.path, dest, ec);
    Check(!ec, "Could not switch " + PathText(p.path.filename()) + ". Close any program using its files, check that the game folder is writable and try again.");
    return {dest, !p.enabled};
}
void Remove(const fs::path& game, const Package& p) {
    Check(!p.enabled, "Disable an imported mod before removing it.");
    const auto lib = GameMods::Library(game);
    PlainLibrary(lib);
    const auto root = fs::canonical(lib);
    const auto target = fs::canonical(p.path);
    Check(target.parent_path() == root && (target.extension() == ".off" || target.extension() == ".removing" ||
          target.extension() == ".importing") &&
          !Reparse(p.path),
          "Only an imported package in this game's mod library can be removed.");
    // A power loss can land between deleting the ownership marker and deleting its empty folder.
    if (EmptyStage(p)) {
        fs::remove(target / "bbvr-package");
        fs::remove(target); // Nonrecursive, fails if anything appeared in it.
        return;
    }
    Check(GameMods::Owned(target) && !Reparse(target / "bbvr-package"), "The launcher could not verify ownership of this folder. No files were removed.");
    // Keep the ownership marker until all payload files are gone. A sharing violation leaves
    // an inactive, identifiable entry whose removal the user can retry.
    for (const auto& e : fs::recursive_directory_iterator(target))
        Check(!Reparse(e.path()), "Remove this link from the imported copy, then retry removal: " + PathText(e.path()));
    auto deleting = target;
    deleting.replace_extension(".removing");
    if (target != deleting) {
        Check(!Present(deleting), "Finish the earlier removal first.");
        std::error_code ec;
        fs::rename(target, deleting, ec);
        Check(!ec, "Could not start removing " + PathText(target.stem()) + ". Close any program using its files, then try Remove mod again.");
    }
    try {
        for (const auto& e : fs::directory_iterator(deleting))
            if (e.path().filename() != "bbvr-package") fs::remove_all(e.path());
        fs::remove(deleting / "bbvr-package");
        fs::remove(deleting);
    } catch (const fs::filesystem_error&) {
        throw std::runtime_error("Removal is unfinished. Close programs using the mod's files, then select Review and Retry removal.");
    }
}
void CheckForLaunch(const fs::path& game, const fs::path& runtime) {
    if (!Present(GameMods::Library(game))) return;
    auto packages = List(game);
    std::erase_if(packages, [](const auto& pkg) { return !pkg.enabled; });
    if (packages.empty()) return;
    // Inspect the executable's export table without starting it or invoking any code in it.
    const auto image = LoadLibraryExW(runtime.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES);
    const bool supported = image && GetProcAddress(image, "BldbrneVRManagedModsV1");
    if (image) FreeLibrary(image);
    Check(supported, "This emulator does not support launcher-managed mods. Select the updated Bldbrne-VR runtime or disable imported mods.");
    for (const auto& active : packages) {
        auto r = Scan(game, active);
        if (!r.conflicts.empty()) throw std::runtime_error(PathText(active.path.stem()) + ": " + r.conflicts.front());
        if (r.overrides.empty()) continue;
        const auto ack_path = active.path / "overrides.json";
        const auto review = "The saved file choices for " + PathText(active.path.stem()) + " need another review. In Mods, disable it, review the listed files, then enable it again.";
        std::error_code ec;
        Check(fs::is_regular_file(fs::symlink_status(ack_path, ec)) && fs::file_size(ack_path, ec) <= 4 * 1024 * 1024, review);
        const auto ack = Json::parse(Read(ack_path), nullptr, false);
        // Overlaps are saved sorted. Walk both lists once, even when thousands of files overlap.
        Check(ack.is_array() && std::all_of(ack.begin(), ack.end(), [](const Json& item) { return item.is_string(); }), review);
        auto accepted = ack.begin();
        for (const auto& file : r.overrides) {
            while (accepted != ack.end() && accepted->get_ref<const std::string&>() < file) ++accepted;
            Check(accepted != ack.end() && accepted->get_ref<const std::string&>() == file, review);
            ++accepted;
        }
    }
}
} // namespace Launcher::Mods
