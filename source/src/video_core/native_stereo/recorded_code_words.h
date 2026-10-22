// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Own each instruction word before decoding it; peeks and later reads see one value.
#pragma once
#include <cstddef>
#include <cstdint>
#include <vector>
namespace ScratchDraw {
inline std::uint32_t ReadRecordedWord(const std::uint32_t* source, std::size_t index,
                                     std::vector<std::uint32_t>& words) {
    while (words.size() <= index) words.push_back(source[words.size()]);
    return words[index];
}
} // namespace ScratchDraw
