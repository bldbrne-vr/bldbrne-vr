// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"

namespace VR::Bloodborne {
// The Doll's respec (vr.ini doll_respec, core/vr/doll_respec.h): hooks checked and installed once
// the eboot is loaded; the site-A tick (the main player's update) ends a respec once its Level Up
// menu is gone, with the build the hunter left with.
void RespecEbootLoaded(VAddr base, u64 size);
void RespecSiteA(u8* player);
} // namespace VR::Bloodborne
