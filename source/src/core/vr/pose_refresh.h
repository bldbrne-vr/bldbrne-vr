// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <bitset>
#include <cstring>
#include <type_traits>

namespace VR::Body {
// Undo only our last write, and only if those locals are still exactly that write.
// A held animation can then be solved again with fresh hands, without accumulating IK
// or overwriting a pose changed by the guest since we last touched it.
template <class T, std::size_t N>
bool RestoreHeldLocals(T* local, const std::array<T, N>& animated, const std::array<T, N>& written,
                       const std::bitset<N>& edited) {
    static_assert(std::is_trivially_copyable_v<T>);
    if (!local || edited.none())
        return false;
    for (std::size_t i = 0; i < N; ++i)
        if (edited[i] && std::memcmp(&local[i], &written[i], sizeof(T)) != 0)
            return false;
    for (std::size_t i = 0; i < N; ++i)
        if (edited[i])
            local[i] = animated[i];
    return true;
}
} // namespace VR::Body
