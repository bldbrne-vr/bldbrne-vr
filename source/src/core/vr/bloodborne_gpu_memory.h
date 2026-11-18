// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "common/types.h"

namespace VR::Bloodborne {
// The game's GPU memory allocators and resource loader copies (bbport's guest hooks, the same
// sites): see bloodborne_gpu_memory.cpp.
void GpuMemoryEbootLoaded(VAddr base, u64 size);
} // namespace VR::Bloodborne
