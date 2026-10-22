// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <filesystem>
#include <system_error>
namespace ScratchCapture {
inline bool Consume(const std::filesystem::path& path) {
    std::error_code error;
    // A temporarily locked marker stays in place for the next frame. Capture requests
    // are diagnostics: a sharing violation must not escape onto the GPU command thread.
    return std::filesystem::remove(path, error);
}
}
