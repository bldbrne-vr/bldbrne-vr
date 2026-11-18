// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

namespace VR::Bloodborne {

// Kos Lights (goods 9000) in the game's own data, built in memory at boot from the files the game
// really loads (a -mods or patch copy included) and served in their place; files that already
// have it are left as they are. VR or not: a save that owns the item always finds its row.
void KosLightsEbootLoaded();

} // namespace VR::Bloodborne
