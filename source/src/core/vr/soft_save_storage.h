// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace VR::SoftSave {
// A save home's own archive (snapshots, recovery backups, the restore journal), beside it
// (2026-10-31 review: homes side by side, Saves/homeA and Saves/homeB, shared Saves/soft_save and
// loaded and recovered each other's saves): "soft_save" beside a home named "home" (the launcher's
// and the trial's), else "soft_save_<its name>". Folders side by side never share a name, so never
// an archive. Empty for a home with no name or one named like an archive (it could be another
// home's): no soft save there.
inline std::filesystem::path Archive(const std::filesystem::path& home) {
    const auto name = home.filename();
    auto lower = name.native();
    for (auto& c : lower)
        if (c >= 'A' && c <= 'Z')
            c = static_cast<std::filesystem::path::value_type>(c - 'A' + 'a');
    if (name.empty() || lower.starts_with(std::filesystem::path("soft_save").native()))
        return {};
    auto archive = home.parent_path() / "soft_save";
    if (name != "home") {
        archive += "_";
        archive += name.native();
    }
    return archive;
}

// Storage has no game pointers. The caller must exclude ALL guest save opens/mutations
// and have observed an idle save manager before calling Capture or Restore.
class Storage {
    std::filesystem::path live, root;
    int slot{};
    static std::string Id() {
        static std::atomic<unsigned long long> serial{};
        return std::to_string(std::chrono::system_clock::now().time_since_epoch().count()) + "_" +
               std::to_string(++serial);
    }
    static void Require(bool value, const char* why) {
        if (!value)
            throw std::runtime_error(why);
    }
    static void PlainDirectory(const std::filesystem::path& path) {
        Require(std::filesystem::canonical(path) == path, "Redirected save path");
        Require(std::filesystem::is_directory(path), "Missing save directory");
    }
    static void PlainTree(const std::filesystem::path& path) {
        PlainDirectory(path);
        for (const auto& entry : std::filesystem::recursive_directory_iterator(path)) {
            const auto type = entry.symlink_status().type();
            Require(type == std::filesystem::file_type::regular ||
                        type == std::filesystem::file_type::directory,
                    "Save tree contains a link or special file");
            Require(std::filesystem::canonical(entry.path()) == entry.path(),
                    "Redirected save entry");
        }
    }
    static std::string ReadMetadata(const std::filesystem::path& path, std::uintmax_t limit) {
        Require(std::filesystem::canonical(path) == path &&
                    std::filesystem::symlink_status(path).type() ==
                        std::filesystem::file_type::regular,
                "Redirected metadata path");
        const auto size = std::filesystem::file_size(path);
        Require(size <= limit, "Unexpected metadata size");
        std::ifstream in(path, std::ios::binary);
        Require(in.good(), "Cannot read metadata");
        std::string text(static_cast<size_t>(size), '\0');
        in.read(text.data(), static_cast<std::streamsize>(size));
        Require(in.gcount() == static_cast<std::streamsize>(size) && !in.bad() &&
                    in.peek() == std::char_traits<char>::eof(),
                "Metadata changed while reading");
        return text;
    }
    static std::string Manifest(const std::filesystem::path& dir) {
        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
            if (entry.is_regular_file() && entry.path() != dir / "soft_save_manifest") {
                Require(files.size() < 1024, "Unexpected save file count");
                files.push_back(entry.path());
            }
        }
        std::sort(files.begin(), files.end());
        std::ostringstream result;
        std::uintmax_t total = 0;
        for (const auto& file : files) {
            const auto size = std::filesystem::file_size(file);
            total += size;
            Require(total <= 128 * 1024 * 1024, "Unexpected save directory size");
            std::ifstream in(file, std::ios::binary);
            Require(in.good(), "Cannot read snapshot file");
            std::uint64_t hash = 14695981039346656037ULL;
            std::array<char, 65536> bytes;
            while (in.read(bytes.data(), bytes.size()) || in.gcount()) {
                for (std::streamsize i = 0; i < in.gcount(); ++i) {
                    hash ^= static_cast<unsigned char>(bytes[i]);
                    hash *= 1099511628211ULL;
                }
            }
            Require(in.eof() && !in.bad(), "Snapshot read failed");
            result << file.lexically_relative(dir).generic_string() << '\t' << size << ' ' << hash
                   << '\n';
            Require(result.good() && static_cast<std::streamoff>(result.tellp()) <= 1024 * 1024,
                    "Unexpected manifest size");
        }
        return result.str();
    }
    std::string Character() const {
        std::ostringstream name;
        name << "userdata" << std::setw(4) << std::setfill('0') << slot;
        return name.str();
    }
    // What a restore takes from the snapshot: this slot's character, the system save and the
    // game's one backup pair (the soft save had the game save this slot just before its
    // snapshot, so the pair is this slot's). The other slots and sce_sys stay as they are now:
    // a restore never rolls another character back.
    std::array<std::string, 4> Restored() const {
        return {Character(), "userdata0010", "backup0000", "backup0010"};
    }
    // `manifest` with each of `names` taken from `from` (a line per file; absent there: none).
    static std::string WithFiles(const std::string& manifest, const std::string& from,
                                 const std::array<std::string, 4>& names) {
        const auto named = [&](const std::string& line) {
            const auto tab = line.find('\t');
            return std::find(names.begin(), names.end(), line.substr(0, tab)) != names.end();
        };
        std::vector<std::string> lines;
        for (const auto* text : {&manifest, &from}) {
            std::istringstream in(*text);
            for (std::string line; std::getline(in, line);)
                if (named(line) == (text == &from))
                    lines.push_back(line);
        }
        std::sort(lines.begin(), lines.end());
        std::string out;
        for (const auto& line : lines)
            out += line + '\n';
        return out;
    }
    void Validate(const std::filesystem::path& dir) const {
        PlainTree(dir);
        Require(std::filesystem::file_size(dir / Character()) == 0x140000,
                "Unexpected character save size");
        Require(std::filesystem::file_size(dir / "userdata0010") == 0x40000,
                "Unexpected system save size");
    }
    static void Copy(const std::filesystem::path& from, const std::filesystem::path& to) {
        PlainTree(from);
        Require(!std::filesystem::exists(to), "Snapshot destination already exists");
        std::filesystem::copy(from, to, std::filesystem::copy_options::recursive);
    }
    static void Replace(const std::filesystem::path& from, const std::filesystem::path& to) {
#ifdef _WIN32
        if (!MoveFileExW(from.c_str(), to.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::system_error(GetLastError(), std::system_category(), "Publish soft save");
#else
        std::filesystem::rename(from, to);
#endif
    }
    void WriteAtomic(const std::string& file, const std::string& value) {
        const auto temp = root / (file + "_" + Id() + ".tmp");
        {
            std::ofstream out(temp, std::ios::binary | std::ios::trunc);
            out.exceptions(std::ios::badbit | std::ios::failbit);
            out << value;
            out.close();
        }
        Replace(temp, root / file);
    }
    void Publish(const std::string& value) {
        WriteAtomic("current", value + "\n");
    }
    void RequireRecovered() const {
        Require(!std::filesystem::exists(root / "restore_pending"),
                "An interrupted restore requires recovery before another save operation");
    }
    void CheckPaths() const {
        RequireRecovered();
        Validate(live);
        // Retained generations are not part of this operation. Validate their contents
        // only when they are selected, copied or recovered.
        PlainDirectory(root);
        Require(live != root && root.parent_path() != live, "Invalid snapshot directory");
    }

    // A generation's order: the Id in its name (`prefix` + ticks + "_" + serial), or nothing for a
    // name that is not exactly one.
    static std::optional<std::pair<unsigned long long, unsigned long long>> Order(
        const std::string& name, const std::string& prefix) {
        if (!name.starts_with(prefix) || name.size() > 100)
            return {};
        const auto id = name.substr(prefix.size());
        const auto under = id.find('_');
        if (under == std::string::npos || under == 0 || under + 1 == id.size() ||
            id.find_first_not_of("0123456789_") != std::string::npos ||
            id.find('_', under + 1) != std::string::npos)
            return {};
        try {
            return std::pair{std::stoull(id.substr(0, under)), std::stoull(id.substr(under + 1))};
        } catch (...) {
            return {};
        }
    }
    // The selected snapshot's name ("none" or empty without one).
    std::string Selected() const {
        std::error_code ec;
        if (!std::filesystem::exists(root / "current", ec) || ec)
            return {};
        std::istringstream input(ReadMetadata(root / "current", 128));
        std::string name;
        std::getline(input, name);
        return name;
    }
    // Old generations go (2026-10-30, soft save for every player: each is a whole save folder,
    // ~15 MB, kept forever before): the newest KeepSnapshots snapshots and the selected one, and
    // the newest KeepBackups pre-restore backups stay, never `made` (the one the calling operation
    // just made: a clock set back would sort it oldest). Only plain directories named exactly as
    // this storage names them; nothing while a restore is pending. A failure leaves extra copies,
    // never fails the operation that called it.
    static constexpr size_t KeepSnapshots = 3;
    static constexpr size_t KeepBackups = 3;
    void Prune(const std::filesystem::path& made = {}) noexcept {
        try {
            if (std::filesystem::exists(root / "restore_pending"))
                return;
            PlainDirectory(root);
            const std::string selected = Selected();
            using Entry = std::pair<std::pair<unsigned long long, unsigned long long>,
                                    std::filesystem::path>;
            std::vector<Entry> snapshots, backups;
            for (const auto& entry : std::filesystem::directory_iterator(root)) {
                if (entry.symlink_status().type() != std::filesystem::file_type::directory)
                    continue;
                const auto name = entry.path().filename().string();
                if (!made.empty() && name == made.filename().string())
                    continue;
                if (const auto o = Order(name, "snapshot_"); o && name != selected)
                    snapshots.push_back({*o, entry.path()});
                else if (const auto b = Order(name, "before_load_"))
                    backups.push_back({*b, entry.path()});
            }
            const auto drop = [](std::vector<Entry>& list, size_t keep, bool manifest) {
                std::sort(list.begin(), list.end(),
                          [](const Entry& a, const Entry& b) { return a.first > b.first; });
                for (size_t i = keep; i < list.size(); ++i) {
                    std::error_code ec;
                    if (std::filesystem::canonical(list[i].second, ec) != list[i].second || ec)
                        continue;
                    std::filesystem::remove_all(list[i].second, ec);
                    if (manifest && !ec)
                        std::filesystem::remove(list[i].second.string() + ".manifest", ec);
                }
            };
            // The selected snapshot and the one just made are kept apart, so fewer others.
            const bool made_snapshot = made.filename().string().starts_with("snapshot_") &&
                                       made.filename().string() != selected;
            const bool made_backup = made.filename().string().starts_with("before_load_");
            size_t keep_snapshots = KeepSnapshots;
            keep_snapshots -= selected.starts_with("snapshot_") && keep_snapshots > 0 ? 1 : 0;
            keep_snapshots -= made_snapshot && keep_snapshots > 0 ? 1 : 0;
            drop(snapshots, keep_snapshots, false);
            drop(backups, made_backup ? KeepBackups - 1 : KeepBackups, true);
        } catch (...) {
        }
    }

public:
    // When a snapshot was made: the system_clock ticks its name carries (Id), or nothing for a
    // name that is not a generation's.
    static std::optional<std::chrono::system_clock::time_point> MadeAt(
        const std::filesystem::path& snapshot) {
        const auto order = Order(snapshot.filename().string(), "snapshot_");
        if (!order)
            return {};
        return std::chrono::system_clock::time_point{std::chrono::system_clock::duration{
            static_cast<std::chrono::system_clock::rep>(order->first)}};
    }
    // Before guest startup, roll back a directory swap interrupted by a process crash.
    static bool Recover(const std::filesystem::path& live, const std::filesystem::path& root) {
        if (!std::filesystem::exists(root / "restore_pending"))
            return false;
        // Retained generations are not part of this operation. Validate their contents
        // only when they are selected, copied or recovered.
        PlainDirectory(root);
        Require(std::filesystem::canonical(live.parent_path()) == live.parent_path(),
                "Redirected recovery parent");
        std::istringstream journal(ReadMetadata(root / "restore_pending", 128));
        std::string name;
        int slot = -1;
        Require(bool(journal >> name >> slot), "Invalid restore journal");
        Require(name.starts_with("before_load_") && name.size() < 100 &&
                    name.find_first_not_of("before_load_0123456789") == std::string::npos &&
                    slot >= 0 && slot <= 9,
                "Invalid recovery backup");
        const auto backup = root / name;
        PlainTree(backup);
        const auto expected = ReadMetadata(root / (name + ".manifest"), 1024 * 1024);
        Require(!expected.empty() && expected == Manifest(backup),
                "Recovery backup checksum mismatch");
        const auto id = Id();
        const auto staged = root / ("recovery_" + id);
        Copy(backup, staged);
        Require(Manifest(staged) == expected, "Recovery copy verification failed");
        const auto displaced = root / ("interrupted_" + id);
        const bool exists = std::filesystem::exists(live);
        if (exists) {
            PlainTree(live);
            std::filesystem::rename(live, displaced);
        }
        try {
            std::filesystem::rename(staged, live);
        } catch (...) {
            if (exists) {
                std::error_code ec;
                std::filesystem::rename(displaced, live, ec);
            }
            throw;
        }
        std::filesystem::remove(root / "restore_pending");
        return true;
    }
    Storage(std::filesystem::path live_path, std::filesystem::path storage_path, int character_slot)
        : live(std::move(live_path)), root(std::move(storage_path)), slot(character_slot) {
        Require(slot >= 0 && slot <= 9, "Invalid character slot");
        Require(live.is_absolute() && root.is_absolute(), "Save paths must be absolute");
        Validate(live);
        // The integration supplies the canonical, explicitly backed-up portable trial home.
        Require(std::filesystem::canonical(root.parent_path()) == root.parent_path(),
                "Redirected snapshot parent");
        std::filesystem::create_directory(root);
        // Retained generations are not part of this operation. Validate their contents
        // only when they are selected, copied or recovered.
        PlainDirectory(root);
    }
    std::filesystem::path Current() const {
        // A new trial normally has no pointer yet; do not use an exception for that state.
        std::error_code ec;
        if (!std::filesystem::exists(root / "current", ec) || ec)
            return {};
        try {
            RequireRecovered();
            PlainDirectory(root);
            std::istringstream input(ReadMetadata(root / "current", 128));
            std::string name;
            std::getline(input, name);
            if (!name.starts_with("snapshot_") || name.size() > 100 ||
                name.find_first_not_of("snapshot_0123456789") != std::string::npos)
                return {};
            const auto path = root / name;
            Validate(path);
            std::istringstream meta(ReadMetadata(path / "soft_save_slot", 16));
            int saved_slot = -1;
            if (!(meta >> saved_slot) || saved_slot != slot)
                return {};
            const auto expected = ReadMetadata(path / "soft_save_manifest", 1024 * 1024);
            if (expected.empty() || expected != Manifest(path))
                return {};
            return path;
        } catch (...) {
            return {};
        }
    }
    using Position = std::array<float, 3>;
    static std::optional<Position> SnapshotPosition(const std::filesystem::path& snapshot) {
        std::error_code ec;
        if (!std::filesystem::exists(snapshot / "soft_save_position", ec) && !ec)
            return {}; // Compatibility with snapshots made before position metadata existed.
        std::istringstream in(ReadMetadata(snapshot / "soft_save_position", 128));
        Position position{};
        for (auto& value : position)
            Require(bool(in >> value) && std::isfinite(value), "Invalid saved position");
        in >> std::ws;
        Require(in.eof(), "Invalid saved position suffix");
        return position;
    }
    std::filesystem::path Capture(std::optional<Position> position = {}) {
        CheckPaths();
        const auto name = "snapshot_" + Id();
        const auto pending = root / (name + ".tmp");
        const auto source_manifest = Manifest(live); // Bound count/size before copying anything.
        Copy(live, pending);
        Validate(pending);
        Require(Manifest(pending) == source_manifest && Manifest(live) == source_manifest,
                "Snapshot copy verification failed");
        {
            std::ofstream meta(pending / "soft_save_slot");
            meta.exceptions(std::ios::badbit | std::ios::failbit);
            meta << slot << '\n';
            meta.close();
        }
        if (position) {
            std::ofstream out(pending / "soft_save_position", std::ios::binary);
            out.exceptions(std::ios::badbit | std::ios::failbit);
            out << std::setprecision(std::numeric_limits<float>::max_digits10);
            for (auto value : *position) {
                Require(std::isfinite(value), "Invalid saved position");
                out << value << ' ';
            }
            out.close();
        }
        {
            const auto manifest = Manifest(pending);
            std::ofstream out(pending / "soft_save_manifest", std::ios::binary);
            out.exceptions(std::ios::badbit | std::ios::failbit);
            out << manifest;
            out.close();
        }
        const auto complete = root / name;
        std::filesystem::rename(pending, complete);
        Publish(name); // Previous complete snapshot remains selected until this succeeds.
        Prune(complete);
        return complete;
    }
    void Clear() {
        RequireRecovered();
        // Retained generations are not part of this operation. Validate their contents
        // only when they are selected, copied or recovered.
        PlainDirectory(root);
        Publish("none"); // Retain recovery data; no recursive deletion.
    }
    std::filesystem::path Restore() {
        CheckPaths();
        const auto snapshot = Current();
        Require(!snapshot.empty(), "No valid soft save for this slot");
        const auto id = Id();
        const auto backup = root / ("before_load_" + id);
        const auto backup_manifest = Manifest(live); // Reject oversized input before copying.
        Copy(live, backup);
        Validate(backup);
        Require(backup_manifest == Manifest(backup) && backup_manifest == Manifest(live),
                "Recovery backup verification failed");
        WriteAtomic(backup.filename().string() + ".manifest", backup_manifest);
        // Staged from the live save, with only this slot's files from the snapshot (Restored).
        const auto staged = root / ("restore_" + id);
        Copy(live, staged);
        for (const auto& name : Restored()) {
            std::filesystem::remove(staged / name);
            if (std::filesystem::exists(snapshot / name)) {
                Require(std::filesystem::symlink_status(snapshot / name).type() ==
                            std::filesystem::file_type::regular,
                        "Snapshot entry is not a plain file");
                std::filesystem::copy_file(snapshot / name, staged / name);
            }
        }
        Require(Manifest(staged) == WithFiles(backup_manifest, Manifest(snapshot), Restored()),
                "Restore staging verification failed");
        Validate(staged);
        const auto displaced = root / ("displaced_" + id);
        WriteAtomic("restore_pending", backup.filename().string() + "\n" + std::to_string(slot));
        std::filesystem::rename(live, displaced);
        try {
            std::filesystem::rename(staged, live);
        } catch (...) {
            std::error_code rollback;
            std::filesystem::rename(displaced, live, rollback);
            if (rollback)
                throw std::runtime_error("Restore and rollback failed; recovery copy: " +
                                         backup.string());
            std::filesystem::remove(root / "restore_pending");
            throw;
        }
        std::filesystem::remove(root / "restore_pending");
        // The moved-aside original is byte-identical to the independently verified backup.
        // Retire that duplicate only after the swap and recovery journal have completed;
        // a failed cleanup merely leaves extra recovery data, never fails the restore.
        std::error_code cleanup;
        std::filesystem::remove_all(displaced, cleanup);
        // The verified pre-restore backup remains recoverable after lantern invalidation (the
        // newest KeepBackups of them, and always this one).
        Prune(backup);
        return backup;
    }
};
} // namespace VR::SoftSave
