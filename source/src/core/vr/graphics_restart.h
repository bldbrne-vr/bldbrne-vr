// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <string>
#include <vector>
namespace VR::Graphics {
// Preserve isolated config/save paths and guest arguments. Relaunch adds the current PID.
inline std::vector<std::string> RestartArguments(const std::vector<std::string>& original) {
    std::vector<std::string> result;
    bool guest = false;
    for (size_t i = 0; i < original.size(); ++i) {
        const auto& arg = original[i];
        if (arg == "--") guest = true;
        if (!guest && arg == "--wait-for-pid") { if (i + 1 < original.size()) ++i; continue; }
        if (!guest && arg.starts_with("--wait-for-pid=")) continue;
        result.push_back(arg);
    }
    return result;
}
} // namespace VR::Graphics
