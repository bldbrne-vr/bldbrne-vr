// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <chrono>
#include <cstdint>

namespace Libraries::AudioOut {

inline std::uint64_t HostTimeUs() {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

}
