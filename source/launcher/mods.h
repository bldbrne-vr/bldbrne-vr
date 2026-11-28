// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace Launcher::Mods {
namespace fs = std::filesystem;
struct Package {
    fs::path path;
    bool enabled{};
    bool Interrupted() const { return path.extension() == ".removing" || path.extension() == ".importing"; }
};
struct Report {
    size_t files{};
    std::vector<std::string> conflicts, overrides, shared;
    bool has_file_list{};
};
fs::path GameRoot(const fs::path& executable);
std::vector<Package> List(const fs::path& game);
Package Import(const fs::path& game, const fs::path& zip);
Report Scan(const fs::path& game, const Package& package);
Package Toggle(const fs::path& game, const Package& package, const Report& reviewed);
void Remove(const fs::path& game, const Package& package);
void CheckForLaunch(const fs::path& game, const fs::path& runtime);
} // namespace Launcher::Mods
