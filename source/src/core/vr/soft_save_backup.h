// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// The launcher's backup a soft save stands on (2026-10-30, the user: soft save for every player,
// not only the portable trial). The launcher backs a player's saves up at every launch
// (launcher/core.cpp BackupSaves, the save home's layout) and names that backup to the game
// (VR_SOFT_SAVE_LAUNCHER_BACKUP). It counts only as a plain directory outside the save home whose
// copy of the save folder holds every file the live one does, byte for byte: this save, as it is
// at this start. Pure (no emulator dependencies; tests/test_soft_save.cpp).
#include <algorithm>
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <system_error>

namespace VR::SoftSave {

inline constexpr std::uintmax_t BackupCheckMaxBytes = 128ull << 20;
inline constexpr int BackupCheckMaxFiles = 1024;

// `inner` is `outer` or inside it (both canonical).
inline bool Within(const std::filesystem::path& inner, const std::filesystem::path& outer) {
    auto i = inner.begin();
    for (auto o = outer.begin(); o != outer.end(); ++o, ++i) {
        if (i == inner.end() || *i != *o)
            return false;
    }
    return true;
}

inline bool SameBytes(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    const auto size = std::filesystem::file_size(a, ec);
    if (ec || std::filesystem::file_size(b, ec) != size || ec)
        return false;
    std::ifstream x(a, std::ios::binary), y(b, std::ios::binary);
    if (!x.good() || !y.good())
        return false;
    std::array<char, 65536> p{}, q{};
    for (;;) {
        x.read(p.data(), p.size());
        y.read(q.data(), q.size());
        if (x.gcount() != y.gcount() || !std::equal(p.begin(), p.begin() + x.gcount(), q.begin()))
            return false;
        if (x.gcount() == 0 || x.eof() || y.eof())
            return x.eof() == y.eof() && !x.bad() && !y.bad();
    }
}

// The backup's place: an existing plain directory, neither the save home nor inside it nor
// holding it; its save folder (`save`, relative) a plain directory too. Empty when not.
inline std::filesystem::path BackupFolder(const std::filesystem::path& home,
                                          const std::filesystem::path& backup,
                                          const std::filesystem::path& save) {
    std::error_code ec;
    const auto h = std::filesystem::canonical(home, ec);
    if (ec)
        return {};
    const auto b = std::filesystem::canonical(backup, ec);
    if (ec || !std::filesystem::is_directory(b, ec) || ec || Within(b, h) || Within(h, b))
        return {};
    const auto copy = b / save;
    if (std::filesystem::canonical(copy, ec) != copy || ec ||
        std::filesystem::symlink_status(copy, ec).type() != std::filesystem::file_type::directory)
        return {};
    return b;
}

// The save's character and system files are in the backup (the trial's own check).
inline bool BackupHolds(const std::filesystem::path& backup, const std::filesystem::path& save) {
    std::error_code ec;
    for (const char* file : {"userdata0000", "userdata0010"}) {
        const auto path = backup / save / file;
        if (std::filesystem::canonical(path, ec) != path || ec ||
            std::filesystem::symlink_status(path, ec).type() !=
                std::filesystem::file_type::regular)
            return false;
    }
    return true;
}

// The backup is a copy of the live save as it is now: every plain file of the save folder is in
// it with the same bytes (at most BackupCheckMaxFiles / BackupCheckMaxBytes; a link or special
// file in the live save fails it).
inline bool BackupMatches(const std::filesystem::path& home, const std::filesystem::path& backup,
                          const std::filesystem::path& save) {
    const auto b = BackupFolder(home, backup, save);
    if (b.empty() || !BackupHolds(b, save))
        return false;
    std::error_code ec;
    const auto live = std::filesystem::canonical(home, ec) / save;
    if (ec || std::filesystem::canonical(live, ec) != live || ec ||
        !std::filesystem::is_directory(live, ec))
        return false;
    std::uintmax_t bytes = 0;
    int files = 0;
    for (auto it = std::filesystem::recursive_directory_iterator(live, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        const auto type = it->symlink_status(ec).type();
        if (ec)
            return false;
        if (type == std::filesystem::file_type::directory)
            continue;
        if (type != std::filesystem::file_type::regular || ++files > BackupCheckMaxFiles)
            return false;
        bytes += it->file_size(ec);
        if (ec || bytes > BackupCheckMaxBytes)
            return false;
        const auto counterpart = b / save / it->path().lexically_relative(live);
        if (std::filesystem::symlink_status(counterpart, ec).type() !=
                std::filesystem::file_type::regular ||
            ec || !SameBytes(it->path(), counterpart))
            return false;
    }
    return !ec && files > 0;
}

} // namespace VR::SoftSave
