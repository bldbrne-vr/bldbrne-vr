// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "common/types.h"
namespace VR::Bloodborne {
// Checks retail hit-part layout; piggybacks on the existing final-damage hook.
bool HeadshotsWanted();
void InitHeadshots(VAddr base, u64 size);
// Called once for a confirmed player hit AFTER the existing offhand multiplier.
// Resolves the target's collision bone; only left-hand firearm head hits get the bonus.
float GunHeadshotDamage(const u8* result, const u8* target, float damage);
} // namespace VR::Bloodborne
