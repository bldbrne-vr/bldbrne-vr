// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cstdint>
#include <stdexcept>
#include <string_view>

namespace VR::Color {

enum class Control : unsigned { Contrast, Saturation, Gamma };

constexpr int Limit(Control control) {
    return control == Control::Saturation ? 100 : 50;
}

struct Settings {
    int contrast{};
    int saturation{};
    int gamma{};

    bool Active() const { return contrast != 0 || saturation != 0 || gamma != 0; }
    std::uint32_t Packed() const {
        return Active() ? std::uint32_t(contrast + 100) |
                              (std::uint32_t(saturation + 100) << 8) |
                              (std::uint32_t(gamma + 100) << 16)
                        : 0u;
    }
    std::array<float, 4> Push(bool bgra = false) const {
        return {1.0f + contrast * 0.01f, 1.0f + saturation * 0.01f,
                1.0f / (1.0f + gamma * 0.01f), bgra ? 1.0f : 0.0f};
    }
};

inline std::atomic<std::uint32_t> current{0x646464u};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);

inline Settings Get() {
    const auto word = current.load(std::memory_order_relaxed);
    return {int(word & 255u) - 100, int((word >> 8) & 255u) - 100,
            int((word >> 16) & 255u) - 100};
}

inline void Set(Control control, std::string_view value) {
    int number{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), number);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::invalid_argument("Color adjustment must be an integer percentage");
    }
    const unsigned shift = 8u * static_cast<unsigned>(control);
    const auto bits = std::uint32_t(std::clamp(number, -Limit(control), Limit(control)) + 100)
                      << shift;
    auto before = current.load(std::memory_order_relaxed);
    while (!current.compare_exchange_weak(before, (before & ~(255u << shift)) | bits,
                                          std::memory_order_relaxed)) {}
}

}
