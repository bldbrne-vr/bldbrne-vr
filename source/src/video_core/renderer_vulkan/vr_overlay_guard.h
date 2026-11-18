// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

namespace Vulkan {

// Keeps ASUS GPU Tweak III's OSD out of the game's Vulkan (vr_overlay_guard.cpp). Must run
// before the Vulkan loader first loads: at startup before DLSS's early instance (emulator.cpp),
// and in CreateInstance; it acts once. Only this process: GTIII keeps working in every other
// game. A no-op off Windows. VR_ALLOW_GTIII_OSD=1 lets the OSD in.
void KeepOverlayLayersOut();

} // namespace Vulkan
