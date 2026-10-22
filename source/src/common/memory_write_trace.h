// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <cstdint>
#ifdef BBVR_MEMORY_WRITE_TRACE
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
namespace Common::MemoryWriteTrace {
// Private diagnostic build only. No guest writes, GPU waits, or log subsystem calls. SourceWord reads only known copy sources.
// Eight million bounded records; output is explicit opt-in and never a shipping default.
struct Trace {
    std::mutex mutex;
    FILE* file{};
    std::uint64_t count{};
    Trace() {
        if (const char* path = std::getenv("VR_MEMORY_WRITE_TRACE"); path && *path) {
            file = std::fopen(path, "wb");
            if (file) std::fputs("seq,ns,thread,event,address,size,a,b\n", file);
        }
    }
    ~Trace() { if (file) std::fclose(file); }
};
inline Trace trace;
inline void Event(const char* name, std::uint64_t address, std::uint64_t size,
                  std::uint64_t a = 0, std::uint64_t b = 0) {
    if (!trace.file || size == 0) return;
    if (size < 16 && std::strncmp(name, "backing.", 8) == 0) return;
    if ((std::strcmp(name, "gpu.write.range") == 0 ||
         std::strcmp(name, "buffer.upload.range") == 0) &&
        (size < 16384 || size > (16ull << 20))) return;
    const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
    const auto thread = std::hash<std::thread::id>{}(std::this_thread::get_id());
    std::scoped_lock lock{trace.mutex};
    if (trace.count >= 8000000) return;
    std::fprintf(trace.file, "%llu,%lld,%zu,%s,%llx,%llx,%llx,%llx\n",
        static_cast<unsigned long long>(++trace.count), static_cast<long long>(ns), thread, name,
        static_cast<unsigned long long>(address), static_cast<unsigned long long>(size),
        static_cast<unsigned long long>(a), static_cast<unsigned long long>(b));
    if ((trace.count & 255) == 0) std::fflush(trace.file);
}
inline std::uint32_t SourceWord(const void* data, std::uint64_t size) {
    std::uint32_t word{};
    if (trace.file && size >= sizeof(word)) std::memcpy(&word, data, sizeof(word));
    return word;
}
}
#else
namespace Common::MemoryWriteTrace {
inline void Event(const char*, std::uint64_t, std::uint64_t,
                  std::uint64_t = 0, std::uint64_t = 0) {}
inline std::uint32_t SourceWord(const void*, std::uint64_t) { return 0; }
}
#endif
