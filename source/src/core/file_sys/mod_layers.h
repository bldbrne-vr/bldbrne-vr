// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>
#ifdef _WIN32
#include <windows.h>
#endif

namespace GameMods {
namespace fs = std::filesystem;
inline constexpr std::string_view Owner = "Bldbrne-VR imported loose assets v1";

inline bool PlainDirectory(const fs::path& path) {
#ifdef _WIN32
    const auto flags = GetFileAttributesW(path.c_str());
    return flags != INVALID_FILE_ATTRIBUTES &&
           (flags & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT)) == FILE_ATTRIBUTE_DIRECTORY;
#else
    std::error_code ec;
    return fs::is_directory(fs::symlink_status(path, ec));
#endif
}

inline bool Owned(const fs::path& package) {
    const auto marker = package / "bbvr-package";
    std::error_code ec;
    if (!fs::is_regular_file(fs::symlink_status(marker, ec)) || fs::file_size(marker, ec) != Owner.size())
        return false;
    char text[Owner.size()]{};
    std::ifstream in(marker, std::ios::binary);
    return in.read(text, sizeof(text)) && std::string_view(text, sizeof(text)) == Owner;
}

inline fs::path Library(fs::path game) {
    if (game.extension() == ".zar") game.replace_extension();
    return game += "-bbvr-mods";
}

// A complete, staged package becomes visible with one directory rename.
// This list is collected at mount time. The game's unchanged asset reads use its normal backends.
inline std::vector<fs::path> Active(const fs::path& game) {
    std::vector<fs::path> out;
    const auto library = Library(game);
    std::error_code ec;
    if (!PlainDirectory(library)) return out;
    for (fs::directory_iterator it(library, ec), end; !ec && it != end; it.increment(ec)) {
        const auto& p = it->path();
        if (p.extension() == ".on" && PlainDirectory(p) && Owned(p) &&
            PlainDirectory(p / "content") && PlainDirectory(p / "content" / "dvdroot_ps4"))
            out.push_back(p);
    }
    std::sort(out.begin(), out.end());
    return out;
}
} // namespace GameMods
