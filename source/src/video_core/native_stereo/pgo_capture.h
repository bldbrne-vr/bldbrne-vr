// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#ifdef BBVR_PGO_GENERATE
#include <chrono>
#include <cstdlib>
#include "common/logging/log.h"
extern "C" int __llvm_profile_write_file();
extern "C" void __llvm_profile_set_dumped();
namespace ScratchPgo {
// Called on the renderer thread only, from the cold five-second report path.
__attribute__((no_profile_instrument_function)) inline void Tick() {
    static const auto start = std::chrono::steady_clock::now();
    static bool attempted = false;
    const char* value = std::getenv("VR_PGO_DUMP_AFTER_S");
    if (attempted || !value) return;
    const int seconds = std::atoi(value);
    if (seconds < 1 || seconds > 600) return;
    if (std::chrono::steady_clock::now() - start < std::chrono::seconds(seconds)) return;
    attempted = true;
    const int status = __llvm_profile_write_file();
    if (status == 0) __llvm_profile_set_dumped();
    LOG_INFO(Render_Vulkan, "VR PGO: explicit profile dump result {}", status);
}
}
#endif
