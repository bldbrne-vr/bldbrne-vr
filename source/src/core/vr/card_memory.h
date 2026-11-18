// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace VR {

// The graphics card's own memory in bytes (the largest dedicated memory of the system's hardware
// adapters, as the launcher reads it: DXGI), asked once; 0 where it cannot be told (another
// system, no adapter): the base tier (core/vr/card_tier.h). VR_CARD_GB=<n> (tests): as if the
// card had n GiB.
u64 CardMemoryBytes();

} // namespace VR
