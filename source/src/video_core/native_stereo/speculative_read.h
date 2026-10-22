// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Scratch Windows prerequisite for speculative resource-table reads.
#pragma once
#ifdef _WIN32
#include <windows.h>
namespace ScratchDraw {
inline thread_local bool speculative_read_active{};
// Called before normal renderer fault dispatch. Only data-read access violations
// inside the narrow guard bypass it; writes and instruction faults remain errors.
inline bool IsSpeculativeReadFault(EXCEPTION_POINTERS* fault) {
    return speculative_read_active && fault && fault->ExceptionRecord &&
           fault->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION &&
           fault->ExceptionRecord->NumberParameters >= 2 &&
           fault->ExceptionRecord->ExceptionInformation[0] == 0;
}
// The callback may only perform raw reads/copies. Do allocations and construct
// C++ owners outside this SEH boundary so a read fault cannot skip their teardown.
__declspec(noinline) inline bool TrySpeculativeRead(void (*read)(void*), void* context) {
    const bool previous = speculative_read_active;
    bool success = false;
    speculative_read_active = true;
    __try {
        __try {
            read(context);
            success = true;
        } __except (IsSpeculativeReadFault(GetExceptionInformation())
                        ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) {
        }
    } __finally {
        speculative_read_active = previous;
    }
    return success;
}
}
#endif
