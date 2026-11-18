// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"
#include <string_view>

namespace VR {
// The original water-reflections key also accepts on/off in hand-edited configs.
inline constexpr bool WaterReflectionsEnabled(std::string_view value) {
    return value != "0" && value != "false" && value != "off";
}

// The game's own screen-space reflections (vr.ini wet_streets, Picture > New graphics >
// Wetness): a pass the renderer has (FS 0x6c2323c2, composited by the deferred ambient
// pass 0x3fe9e964) but the game never turns on. Read at start: the switch is a few bytes of the
// game's code, written once at load when every site still holds the game's own bytes.
void ReflectionsEbootLoaded(VAddr base, u64 size);
} // namespace VR
