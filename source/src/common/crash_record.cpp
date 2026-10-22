// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <atomic>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <string>
#include <fmt/format.h>
#include "common/crash_record.h"
#include "common/types.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/thread.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Common::CrashRecord {
namespace {

// A crashing process may fault on several threads at once: the first few notes are enough.
std::atomic<int> g_notes{0};
constexpr int MaxNotes = 4;

const char* BaseName(const char* path) {
    const char* name = path;
    for (const char* p = path; p && *p; ++p) {
        if (*p == '/' || *p == '\\') {
            name = p + 1;
        }
    }
    return name;
}

void Append(const std::string& what) {
    if (g_notes.fetch_add(1) >= MaxNotes) {
        return;
    }
    const std::time_t now = std::time(nullptr);
    std::tm local{};
#ifdef _WIN32
    localtime_s(&local, &now);
#else
    localtime_r(&now, &local);
#endif
    char stamp[32]{};
    std::strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &local);
    std::string line = fmt::format("{} [{}] {}\n", stamp, Common::GetCurrentThreadName(), what);
    try {
        const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
        std::error_code ignored;
        std::filesystem::create_directories(dir, ignored);
#ifdef _WIN32
        FILE* file = _wfopen((dir / "crash.txt").c_str(), L"ab");
#else
        FILE* file = std::fopen((dir / "crash.txt").c_str(), "ab");
#endif
        if (file) {
            std::fwrite(line.data(), 1, line.size(), file);
            std::fclose(file);
        }
    } catch (...) {
        // Nothing more can be done while crashing.
    }
}

#ifdef _WIN32
LONG WINAPI NoteException(EXCEPTION_POINTERS* info) {
    const EXCEPTION_RECORD* r = info ? info->ExceptionRecord : nullptr;
    if (r) {
        const auto address = reinterpret_cast<uintptr_t>(r->ExceptionAddress);
        HMODULE module{};
        wchar_t name[MAX_PATH]{};
        std::string where = fmt::format("{:#x}", address);
        if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                   GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               reinterpret_cast<LPCWSTR>(r->ExceptionAddress), &module) &&
            GetModuleFileNameW(module, name, MAX_PATH) != 0) {
            const std::filesystem::path path{name};
            where = fmt::format("{}+{:#x} ({:#x})", path.filename().string(),
                                address - reinterpret_cast<uintptr_t>(module), address);
        } else {
            where += " (outside any module: guest code or generated code)";
        }
        std::string access;
        if (r->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && r->NumberParameters >= 2) {
            const auto kind = r->ExceptionInformation[0];
            access = fmt::format(", {} {:#x}",
                                 kind == 0 ? "reading" : kind == 1 ? "writing" : "executing",
                                 r->ExceptionInformation[1]);
        }
        Append(fmt::format("exception {:#010x} at {}{}", static_cast<u32>(r->ExceptionCode),
                           where, access));
        Common::Log::Flush();
    }
    return EXCEPTION_CONTINUE_SEARCH; // the process ends as it would have, with the same code
}
#endif

} // namespace

void NoteFailedCheck(const char* file, int line, bool unreachable) {
    Append(fmt::format("{} at {}:{}", unreachable ? "unreachable code reached" : "a check failed",
                       BaseName(file), line));
    Common::Log::Flush(); // the check's message, still queued for the log file
}

void Note(const char* what) {
    Append(what);
}

void InstallExceptionNote() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(NoteException);
#endif
}

} // namespace Common::CrashRecord
