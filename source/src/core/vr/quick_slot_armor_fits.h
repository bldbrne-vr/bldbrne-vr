// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <cstdint>

namespace VR::BodyMounts {

// Signed millimetres along the local outward normal. Measured on the equipped
// outfit at the two pocket locations, relative to Hunter Garb (71000).
struct QuickSlotArmorFit {
    std::int8_t chest_mm{};
    std::int8_t waist_mm{};
};

struct QuickSlotArmorVariants {
    QuickSlotArmorFit male{}, female{};
};

// Playable torso IDs are 1000 + 10000 * index. Gaps have the neutral fit.
inline constexpr std::array<QuickSlotArmorVariants, 41> QuickSlotArmorFits{{
    {},
    {{-10, 0}, {-10, 0}}, // 11000 Charred Hunter Garb
    {{0, 10}, {0, 20}}, // 21000 Yahar'gul Black Garb
    {{5, 0}, {5, 5}}, // 31000 Graveguard Robe
    {{-15, 0}, {-10, 5}}, // 41000 Bone Ash Armor
    {{-10, -5}, {0, 0}}, // 51000 Henryk's Hunter Garb
    {{-5, 5}, {20, -10}}, // 61000 Yharnam Hunter Garb
    {{0, 0}, {0, 0}}, // 71000 Hunter Garb
    {{-10, -5}, {-5, 10}}, // 81000 Knight's Garb
    {},
    {{10, 5}, {5, 10}}, // 101000 Cainhurst Armor
    {{5, 5}, {-5, -5}}, // 111000 Black Church Garb
    {{20, 25}, {25, 30}}, // 121000 Choir Garb
    {{-15, -10}, {-5, -5}}, // 131000 Gascoigne's Garb
    {{-20, 5}, {-10, 15}}, // 141000 Tomb Prospector Garb
    {{-5, -10}, {5, 0}}, // 151000 Executioner Garb
    {},
    {},
    {{-10, 5}, {-5, 10}}, // 181000 Crowfeather Garb
    {{-10, -5}, {-5, 0}}, // 191000 Student Uniform
    {{0, -5}, {10, 0}}, // 201000 Gehrman's Hunter Garb
    {{-5, 10}, {0, 15}}, // 211000 Madman Garb
    {{0, 5}, {10, 10}}, // 221000 Doll Clothes
    {{-20, -25}, {-10, -25}}, // 231000 Foreign Garb
    {{-20, -5}, {-10, 0}}, // 241000 Ashen Hunter Garb
    {},
    {},
    {},
    {{5, -5}, {10, -5}}, // 281000 Hunter Garb
    {{10, 5}, {-5, -5}}, // 291000 White Church Garb
    {{0, -10}, {5, -5}}, // 301000 Student Uniform
    {{-20, -5}, {-10, 5}}, // 311000 Noble Dress
    {{-20, -30}, {-10, -30}}, // 321000 Sweaty Clothes
    {{10, -5}, {20, 0}}, // 331000 Butcher Garb
    {{-10, -15}, {0, -10}}, // 341000 Harrowed Garb
    {{-5, 5}, {0, 10}}, // 351000 Old Hunter Garb
    {{-15, -10}, {-10, -5}}, // 361000 Decorative Old Hunter Garb
    {{-20, -10}, {-10, -5}}, // 371000 Maria Hunter Garb
    {{-10, -10}, {-5, -5}}, // 381000 Constable's Garb
    {{-20, -25}, {-10, -20}}, // 391000 Beast Hide Garb
    {{30, 30}, {30, 30}}, // 401000 Khaki Haori
}};

constexpr QuickSlotArmorFit QuickSlotFit(std::int32_t armor, std::uint8_t is_male) {
    if (is_male > 1)
        return {};
    if (armor == -1) // Empty equipment slot in EquipGameData.
        return {-20, -30};
    if (armor < 11000 || armor > 401000 || armor % 10000 != 1000)
        return {};
    const auto& variants = QuickSlotArmorFits[armor / 10000];
    return is_male ? variants.male : variants.female;
}

} // namespace VR::BodyMounts
