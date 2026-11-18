// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include "core/emulator_settings.h"

// The BDA page table (BufferCache::bda_pagetable_buffer): one device address per 16 KB page of
// the whole 40-bit guest space, 512 MB, read only by shaders using direct memory access (the
// GPU setting directMemoryAccess, off by default and for Bloodborne). It is allocated in full
// only when that setting is on as the buffer cache is made; otherwise a minimal buffer stands in
// and nothing writes it. Shaders take the direct path only when the full table exists, so turning
// the setting on later cannot make one read past the minimal buffer (2026-10-23: 512 MB of VRAM
// back for every player).
namespace VideoCore::DmaTable {

// True until the buffer cache decides; shaders made before it follow the setting alone.
inline std::atomic<bool> allocated{true};

inline bool ShadersMayUse() {
    return EmulatorSettings.IsDirectMemoryAccessEnabled() &&
           allocated.load(std::memory_order_acquire);
}

} // namespace VideoCore::DmaTable
