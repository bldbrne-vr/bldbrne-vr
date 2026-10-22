// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <immintrin.h>

namespace ScratchStream {
// Non-overlapping CPU shadow -> mapped GPU memory only. The build requires AVX2.
// Never read outside the source range; stream whole aligned cache lines only.
inline void CopyAvx2(void* destination, const void* source, std::size_t bytes) {
    if (bytes < 4096) { std::memcpy(destination, source, bytes); return; }
    auto* dst = static_cast<unsigned char*>(destination);
    auto* src = static_cast<const unsigned char*>(source);
    const auto head = std::min(bytes, (64 - (reinterpret_cast<std::uintptr_t>(dst) & 63)) & 63);
    std::memcpy(dst, src, head);
    dst += head; src += head; bytes -= head;
    while (bytes >= 64) {
        const auto a = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src));
        const auto b = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(src + 32));
        _mm256_stream_si256(reinterpret_cast<__m256i*>(dst), a);
        _mm256_stream_si256(reinterpret_cast<__m256i*>(dst + 32), b);
        src += 64; dst += 64; bytes -= 64;
    }
    std::memcpy(dst, src, bytes);
    // Publish non-temporal writes before a later Vulkan queue submission or ring reuse.
    _mm_sfence();
}
}
