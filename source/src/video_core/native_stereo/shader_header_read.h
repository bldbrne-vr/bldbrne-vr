// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Allocation-free raw discovery. Call only inside a speculative read boundary.
#pragma once
#include "video_core/amdgpu/regs_shader.h"
#include <cstring>
#include <limits>
namespace ScratchDraw {
inline bool ReadShaderHeaderUnsafe(uintptr_t code, AmdGpu::BinaryInfo& output) {
    if (!code || code > std::numeric_limits<uintptr_t>::max() - 0x10000 - sizeof(output)) return false;
    uint32_t prefix[2];
    std::memcpy(prefix, reinterpret_cast<const void*>(code), sizeof(prefix));
    if (prefix[0] == 0xBEEB03FF) {
        const uint64_t offset = (uint64_t(prefix[1]) + 1) * 8;
        if (offset <= std::numeric_limits<uintptr_t>::max() - code - sizeof(output)) {
            std::memcpy(&output, reinterpret_cast<const void*>(code + offset), sizeof(output));
            if (output.Valid()) return output.length >= 4 && output.length % 4 == 0;
        }
    }
    for (size_t i = 0; i < 0x4000; ++i) {
        std::memcpy(&output, reinterpret_cast<const void*>(code + i * 4), sizeof(output));
        if (output.Valid()) return output.length >= 4 && output.length % 4 == 0;
    }
    return false;
}
}
