// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include "common/types.h"

// The hunter's skeleton numbers for the VR features (core/vr/c0000_skeleton.h Skeleton: the
// reference pose, its model-space FK, the body's dummy polys, the arm lengths, the Havok -> FLVER
// map), read from the player's own game when it runs, never compiled in:
//  - the reference pose (and the parents and names it is checked by) from the hkaSkeleton behind
//    the main player's pose, in the game's memory (the game loaded it from chr/c0000.anibnd.dcx
//    -> skeleton.hkx);
//  - the bind pose and dummy polys of c0000.flver (chr/c0000.chrbnd.dcx, 47 KB) through the
//    game's own mounts (a mod's replacement file is what the game sees too).
// Both must be the retail v1.09 data (hash checks of the raw inputs, core/vr/c0000_rig.h): one
// log line "VR skeleton: read at runtime, matches the reference: yes|no ...". Until it is read
// C0000::Active() is nullptr and the body, the gun's barrel, the vial's arm zone and the wrist
// gauge wait; if it cannot be read or does not match they stay off for the session.
// VR_SKELETON_DUMP=<file>: also writes the skeleton's numbers there as text (dev checks).
namespace VR::Bloodborne {

// `hk_skeleton`: the main player's hkaSkeleton (the pose's +0x00). Reads and installs the skeleton
// the first time (any thread; serialised). True once C0000::Active() is set; false when it
// cannot be (logged once; every later call is false at once).
bool EnsureSkeleton(const u8* hk_skeleton);

// The reason the skeleton is unavailable ("" while it may still come or once it is in).
[[nodiscard]] const char* SkeletonFailure();

} // namespace VR::Bloodborne
