// SPDX-License-Identifier: GPL-2.0-or-later
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
