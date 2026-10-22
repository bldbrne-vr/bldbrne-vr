// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <string>
namespace BenchmarkFiles {
inline std::string EnvironmentUtf8(const wchar_t* name) {
    const auto n = GetEnvironmentVariableW(name, nullptr, 0);
    if (!n) return {};
    std::wstring value(n, L'\0');
    const auto got = GetEnvironmentVariableW(name, value.data(), n);
    if (!got || got >= n) return {};
    value.resize(got);
    const auto bytes = WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), nullptr, 0, nullptr, nullptr);
    std::string text(bytes, '\0');
    WideCharToMultiByte(CP_UTF8, 0, value.data(), int(value.size()), text.data(), bytes, nullptr, nullptr);
    return text;
}
inline const char* DumpDirectory() {
    static const auto path = EnvironmentUtf8(L"MOCKXR_DUMP_DIR");
    return path.empty() ? nullptr : path.c_str();
}
inline FILE* Open(const std::string& path, const char* mode) {
    const std::wstring wide_mode(mode, mode + std::char_traits<char>::length(mode));
    return _wfopen(std::filesystem::u8path(path).c_str(), wide_mode.c_str());
}
inline int Remove(const std::string& path) {
    return _wremove(std::filesystem::u8path(path).c_str());
}
}
