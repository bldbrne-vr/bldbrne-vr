// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

// The arms / camera sync log (2026-10-18, logs-on builds only; nothing in a BBVR_NO_LOGS build):
// VR_SYNC_LOG=1 writes sync_log.txt in the working directory, one line per event with the steady
// clock first: the XR frame thread's hand samples ("pub"), the game's pose copy taking one for the
// arms ("pose"), the camera update taking a head sample ("cam") and the frame handed to the
// runtime ("end"). Which hand sample the arms of a shown frame had against its camera's head
// sample, and when it reached the eyes (offline sync analysis).

#ifdef BBVR_NO_LOGS

namespace Common::SyncLog {
inline bool On() {
    return false;
}
template <typename... Args>
inline void Line(const char*, Args...) {}
} // namespace Common::SyncLog

#else

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <mutex>

namespace Common::SyncLog {

inline bool On() {
    static const bool on = std::getenv("VR_SYNC_LOG") != nullptr;
    return on;
}

inline void Line(const char* format, ...) {
    if (!On()) {
        return;
    }
    static std::mutex mutex;
    static std::FILE* file = std::fopen("sync_log.txt", "w");
    if (file == nullptr) {
        return;
    }
    const long long now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    std::scoped_lock lk{mutex};
    std::fprintf(file, "%lld ", now);
    va_list args;
    va_start(args, format);
    std::vfprintf(file, format, args);
    va_end(args);
    std::fputc('\n', file);
    static unsigned lines = 0;
    if (++lines % 256 == 0) {
        std::fflush(file);
    }
}

} // namespace Common::SyncLog

#endif
