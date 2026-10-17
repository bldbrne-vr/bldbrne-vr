// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
namespace VR::Lever {
inline std::atomic<bool> physical_levers{true};
inline bool Enabled() { return physical_levers.load(std::memory_order_relaxed); }
inline void SetEnabled(bool enabled) { physical_levers.store(enabled, std::memory_order_relaxed); }
} // namespace VR::Lever
