// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <filesystem>
#include <chrono>
#include <thread>
#include <algorithm>
namespace Common {
inline bool PrepareShaderCacheDirectory(const std::filesystem::path& directory) {
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    return !error && std::filesystem::is_directory(directory, error) && !error;
}
}
#ifdef _WIN32
#include <windows.h>
namespace Common {
// No sharing: another process must not preload, clear or write this cache while
// its owner is alive. Windows releases the handle even after forced termination.
class ShaderCacheLock {
    HANDLE handle = INVALID_HANDLE_VALUE;
    DWORD error = ERROR_SUCCESS;
public:
    ShaderCacheLock() = default;
    ShaderCacheLock(const ShaderCacheLock&) = delete;
    ShaderCacheLock& operator=(const ShaderCacheLock&) = delete;
    ~ShaderCacheLock() { Release(); }
    void Release() noexcept {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
    DWORD LastError() const noexcept { return error; }
    bool Acquire(const std::filesystem::path& directory,
                 std::chrono::milliseconds wait = std::chrono::milliseconds{0}) {
        Release();
        const auto deadline = std::chrono::steady_clock::now() + wait;
        const auto path = directory / L".bbvr-cache.lock";
        for (;;) {
            handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                 OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            error = handle != INVALID_HANDLE_VALUE ? ERROR_SUCCESS : GetLastError();
            if (handle != INVALID_HANDLE_VALUE) return true;
            // Only a live owner can recover shortly. Permissions/path errors fail promptly.
            if (error != ERROR_SHARING_VIOLATION && error != ERROR_LOCK_VIOLATION) return false;
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline) return false;
            std::this_thread::sleep_until((std::min)(deadline, now + std::chrono::milliseconds{50}));
        }
    }
};
}
#endif
