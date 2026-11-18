// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/vr/readable_memory.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace VR {

ReadableMemory::Region ReadableMemory::At(std::uintptr_t at) {
    if (at == 0) {
        return {};
    }
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION m{};
    if (VirtualQuery(reinterpret_cast<LPCVOID>(at), &m, sizeof(m)) != sizeof(m) ||
        m.State != MEM_COMMIT) {
        return {};
    }
    constexpr DWORD Readable = PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY | PAGE_EXECUTE_READ |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    if ((m.Protect & Readable) == 0 || (m.Protect & PAGE_GUARD) != 0) {
        return {};
    }
    const auto lo = reinterpret_cast<std::uintptr_t>(m.BaseAddress);
    return {lo, lo + m.RegionSize};
#else
    return {at, ~std::uintptr_t{0}};
#endif
}

std::size_t ReadableMemory::Span(const void* p, std::size_t max) {
    const auto start = reinterpret_cast<std::uintptr_t>(p);
    if (start == 0) {
        return 0;
    }
    constexpr std::uintptr_t Top = ~std::uintptr_t{0}; // (no max(): windows.h's macro)
    const std::uintptr_t end = max > Top - start ? Top : start + max;
    std::uintptr_t at = start;
    for (int i = 0; i < MaxRegions && at < end; ++i) {
        Region r{};
        for (const Region& k : known_) {
            if (k.lo <= at && at < k.hi) {
                r = k;
                break;
            }
        }
        if (r.hi == 0) {
            r = At(at);
            if (!(r.lo <= at && at < r.hi)) {
                break; // (this byte cannot be read)
            }
            known_[next_++ % Known] = r;
        }
        at = r.hi < end ? r.hi : end;
    }
    return static_cast<std::size_t>(at - start);
}

} // namespace VR
