// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Shared by the launcher and runtime, so "detected" means the runtime can register it.
// Startup/setup only: bounded, non-recursive directory and metadata reads.
namespace Common::Addons {
namespace fs = std::filesystem;
inline std::optional<std::string> SfoText(const fs::path& path, std::string_view name,
                                          bool strict = false) {
    // Bound both the file and every index before reading untrusted installation metadata.
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    const auto size = file.tellg();
    if (!file || size < 20 || size > (strict ? 64 * 1024 : 4 * 1024 * 1024))
        return std::nullopt;
    std::string data(size_t(size), '\0');
    file.seekg(0);
    if (!file.read(data.data(), std::streamsize(data.size())) ||
        data.compare(0, 4, std::string("\0PSF", 4)) != 0)
        return std::nullopt;
    auto u16 = [&](size_t at) -> uint16_t {
        return uint16_t(uint8_t(data[at])) | uint16_t(uint8_t(data[at + 1])) << 8;
    };
    auto u32 = [&](size_t at) -> uint32_t {
        return uint32_t(u16(at)) | uint32_t(u16(at + 2)) << 16;
    };
    const size_t keys = u32(8), values = u32(12), count = u32(16);
    if (count > (data.size() - 20) / 16 || keys < 20 + 16 * count || values < keys ||
        values > data.size())
        return std::nullopt;
    std::optional<std::string> result;
    for (size_t i = 0; i < count; ++i) {
        const size_t entry = 20 + 16 * i;
        const size_t key = keys + u16(entry);
        if (key >= values) {
            if (strict)
                return std::nullopt;
            continue;
        }
        const auto end = data.find('\0', key);
        if (end == std::string::npos || end >= values) {
            if (strict)
                return std::nullopt;
            continue;
        }
        const bool matches = std::string_view(data).substr(key, end - key) == name;
        if (!matches && !strict)
            continue;
        const size_t length = u32(entry + 4), offset = values + size_t(u32(entry + 12));
        if (offset > data.size() || length > data.size() - offset)
            return std::nullopt;
        if (!matches)
            continue;
        if (u16(entry + 2) != 0x0204 || length == 0 || length > 128)
            return std::nullopt;
        auto text = data.substr(offset, length);
        const auto nul = text.find('\0');
        if (nul == std::string::npos)
            return std::nullopt;
        text.resize(nul);
        if (text.empty() || std::any_of(text.begin(), text.end(),
                                        [](unsigned char c) { return c < 0x20 || c > 0x7e; }))
            return std::nullopt;
        result = std::move(text);
        if (!strict)
            return result;
    }
    return result;
}

inline bool TitleId(std::string_view id) {
    return id.size() == 9 && id.starts_with("CUSA") &&
           std::all_of(id.begin() + 4, id.end(), [](char c) { return c >= '0' && c <= '9'; });
}
struct Entry {
    fs::path path;
    std::string label;
};
struct ScanResult {
    std::vector<Entry> entries;
    bool limited = false;
    bool unreadable = false;
};
inline ScanResult Scan(const fs::path& root, std::string_view title) {
    ScanResult out;
    if (root.empty() || !TitleId(title))
        return out;
    auto inspect = [&](const fs::path& dir) {
        const auto sfo = dir / "sce_sys" / "param.sfo";
        // Real add-on metadata, not a folder-name-based unlock.
        const auto category = SfoText(sfo, "CATEGORY", true);
        if (!category || !category->starts_with("ac"))
            return;
        const auto content = SfoText(sfo, "CONTENT_ID", true);
        if (!content || content->size() <= 20 || content->size() > 36 ||
            content->substr(7, 9) != title || (*content)[6] != '-' || (*content)[16] != '_' ||
            (*content)[19] != '-')
            return;
        const auto label = content->substr(20);
        if (!std::all_of(label.begin(), label.end(), [](unsigned char c) {
                return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                       c == '_' || c == '-';
            }))
            return;
        if (std::none_of(out.entries.begin(), out.entries.end(),
                         [&](const Entry& e) { return e.label == label; }))
            out.entries.push_back({dir, label});
    };
    // Accept the add-on itself, its parent, or a shadPS4 addcont root containing TITLE_ID.
    // Visit the title-specific directory first; never enumerate other titles recursively.
    for (const auto& dir : {root / std::string(title), root}) {
        inspect(dir);
        std::error_code error;
        fs::directory_iterator it(dir, error), end;
        if (error) {
            if (error != std::errc::no_such_file_or_directory &&
                error != std::errc::not_a_directory)
                out.unreadable = true;
            continue;
        }
        std::vector<fs::path> children;
        size_t visited = 0;
        while (it != end && !error) {
            if (++visited > 512) {
                out.limited = true;
                break;
            }
            std::error_code type_error;
            if (it->is_directory(type_error))
                children.push_back(it->path());
            if (type_error)
                out.unreadable = true;
            it.increment(error);
        }
        if (error)
            out.unreadable = true;
        std::sort(children.begin(), children.end());
        for (const auto& child : children)
            inspect(child);
    }
    return out;
}
// The Old Hunters: its entitlement in the add-on folder, or built into the Game of the Year
// Edition. The runtime's DLC shader list is read only then (vk_shader_prebuild.cpp); the
// launcher's ResolveDlc names the same label and edition.
inline constexpr std::string_view OldHuntersLabel = "SPEXPANSIONDLC03";
inline constexpr std::string_view OldHuntersBuiltInTitle = "CUSA03173";
inline bool HasOldHunters(std::string_view title, const ScanResult& scan) {
    return title == OldHuntersBuiltInTitle ||
           std::any_of(scan.entries.begin(), scan.entries.end(),
                       [](const Entry& e) { return e.label == OldHuntersLabel; });
}
struct Selection {
    fs::path root;
    ScanResult scan;
};
inline Selection Find(const fs::path& game, const fs::path& requested, const fs::path& configured,
                      const fs::path& installed, std::string_view title,
                      std::string_view preferred_label = {}) {
    if (!requested.empty())
        return {requested, Scan(requested, title)};
    // Nearby extracted add-ons first, then existing runtime configuration and installed content.
    std::vector<fs::path> roots{game,
                                game / "dlc",
                                game / "DLC",
                                game / "addcont",
                                game / "user" / "addcont",
                                game.parent_path() / "dlc",
                                game.parent_path() / "DLC",
                                game.parent_path() / "addcont",
                                game.parent_path() / "user" / "addcont",
                                configured,
                                installed};
    Selection missing{installed, {}};
    std::optional<Selection> first_match;
    std::vector<fs::path> seen;
    for (const auto& root : roots) {
        if (root.empty() || std::find(seen.begin(), seen.end(), root) != seen.end())
            continue;
        seen.push_back(root);
        auto scan = Scan(root, title);
        if (!scan.entries.empty()) {
            if (preferred_label.empty() ||
                std::any_of(scan.entries.begin(), scan.entries.end(),
                            [&](const Entry& e) { return e.label == preferred_label; }))
                return {root, std::move(scan)};
            if (!first_match)
                first_match = Selection{root, scan};
        }
        missing.scan.limited |= scan.limited;
        missing.scan.unreadable |= scan.unreadable;
    }
    return first_match ? std::move(*first_match) : std::move(missing);
}
} // namespace Common::Addons
