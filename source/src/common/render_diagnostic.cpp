// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "common/render_diagnostic.h"
#include "common/path_util.h"
#include "common/scm_rev.h"
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace Common::RenderDiagnostic {
void Write(std::string_view message) noexcept {
    try {
        static std::mutex mutex;
        static unsigned records = 0;
        std::scoped_lock lock{mutex};
        if (records++ >= 64) return;
        const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const auto path = dir / "render-diagnostics.txt";
        const auto size = std::filesystem::file_size(path, ec);
        // Retain one bounded previous file; never grow indefinitely across launches.
        if (!ec && size >= 256 * 1024) {
            const auto previous = dir / "render-diagnostics.previous.txt";
            std::filesystem::remove(previous, ec);
            ec.clear();
            std::filesystem::rename(path, previous, ec);
            if (ec) return;
        }
        const auto now = std::time(nullptr);
        std::tm utc{};
#ifdef _WIN32
        gmtime_s(&utc, &now);
        const auto pid = GetCurrentProcessId();
#else
        gmtime_r(&now, &utc);
        const auto pid = getpid();
#endif
        char stamp[32]{};
        std::strftime(stamp, sizeof(stamp), "%Y-%m-%dT%H:%M:%SZ", &utc);
        // Stamp startup records too: a crash may precede the first VR settings snapshot.
        const auto line = fmt::format("{} pid={} build={} release={} rev={} {}\n", stamp, pid,
            g_version, g_scm_desc[0] ? g_scm_desc : "unknown",
            g_scm_rev[0] ? g_scm_rev : "unknown", message.substr(0, 2048));
#ifdef _WIN32
        FILE* file = _wfopen(path.c_str(), L"ab");
#else
        FILE* file = std::fopen(path.c_str(), "ab");
#endif
        if (file) {
            std::fwrite(line.data(), 1, line.size(), file);
            std::fclose(file);
        }
    } catch (...) { /* Diagnostics must never break rendering. */ }
}
namespace {
std::mutex game_mutex;
std::string game_line;
} // namespace
void SetGame(std::string line) noexcept {
    try {
        {
            std::scoped_lock lock{game_mutex};
            game_line = line;
        }
        Write(line);
    } catch (...) { /* Diagnostics must never break the boot. */ }
}
std::string Game() {
    std::scoped_lock lock{game_mutex};
    return game_line.empty() ? std::string{"Game: unknown"} : game_line;
}
void WriteHeadset(std::string_view system, unsigned width0, unsigned height0, unsigned width1,
                  unsigned height1, unsigned max_width, unsigned max_height) noexcept {
    try {
        std::string name;
        for (const char c : system.substr(0, 64))
            if (c >= 0x20 && c < 0x7f && c != '"' && c != '\\')
                name.push_back(c);
        const auto text =
            fmt::format("{{\"system\": \"{}\", \"width\": [{}, {}], \"height\": [{}, {}], \"max_width\": {}, \"max_height\": {}}}\n",
                        name, width0, width1, height0, height1, max_width, max_height);
        const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
#ifdef _WIN32
        FILE* file = _wfopen((dir / "headset.json").c_str(), L"wb");
#else
        FILE* file = std::fopen((dir / "headset.json").c_str(), "wb");
#endif
        if (file) {
            std::fwrite(text.data(), 1, text.size(), file);
            std::fclose(file);
        }
    } catch (...) { /* Diagnostics must never break rendering. */ }
}
}
