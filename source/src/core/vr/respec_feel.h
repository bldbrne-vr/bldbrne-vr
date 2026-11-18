// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cstdint>

// The Doll's respec in the hands (core/vr/doll_respec.h): counters its game side bumps (the stubs
// with a locked increment, the site-A tick) and the presenter's haptics play when they move.
namespace VR::Respec::Feel {
inline std::atomic<std::uint32_t> freed{};   // an attribute down: a level freed
inline std::atomic<std::uint32_t> placed{};  // a freed level put into an attribute
inline std::atomic<std::uint32_t> refused{}; // the confirm refused: a freed level left
inline std::atomic<std::uint32_t> kept{};    // a new build written by the menu's level-up
static_assert(sizeof(std::atomic<std::uint32_t>) == 4);
} // namespace VR::Respec::Feel
