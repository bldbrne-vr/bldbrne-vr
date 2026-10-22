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
        const auto line = fmt::format("{} pid={} build={} rev={} {}\n", stamp, pid,
            g_version, g_scm_rev, message.substr(0, 2048));
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
}
