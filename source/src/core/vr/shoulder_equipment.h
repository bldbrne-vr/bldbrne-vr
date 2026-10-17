// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>
#include <cstring>

namespace VR::ShoulderEquipment {
constexpr std::int32_t BareFists = 1000000;
constexpr unsigned Selected = 0x0c, Ids = 0x64, Handles = 0x24;
constexpr unsigned SavedSelected = 0x24c;
template <typename T> T Read(const std::uint8_t* p) {
    T value;
    std::memcpy(&value, p, sizeof(value));
    return value;
}
inline void Write(std::uint8_t* p, std::int32_t value) { std::memcpy(p, &value, sizeof(value)); }
inline bool Valid(const std::uint8_t* equipment) {
    for (unsigned side = 0; side < 2; ++side) {
        const auto slot = Read<std::int32_t>(equipment + Selected + 4 * side);
        if (slot < 0 || slot > 1)
            return false;
    }
    return true;
}
struct Remembered {
    int slot{-1}, return_slot{-1};
    std::int32_t id{}, handle{}, inventory_index{-1};
    bool HadWeapon() const { return id >= 0 && id != BareFists; }
};
} // namespace VR::ShoulderEquipment
