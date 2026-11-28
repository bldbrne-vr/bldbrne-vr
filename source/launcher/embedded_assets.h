// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <cstddef>

namespace Launcher {
// The launcher's images (resource.h ids), compiled in as byte arrays by embed_assets.cmake. Not
// Win32 resources: Velopack copies those into the install root's stub, where a large unused blob
// gets the stub flagged by antivirus (embed_assets.cmake).
struct EmbeddedAsset {
    const unsigned char* data{};
    std::size_t size{};
};
EmbeddedAsset FindEmbeddedAsset(int id);
} // namespace Launcher
