// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <cstdint>

namespace VR::HonedSteel {
// The shaders' cache revision: bump it whenever the honed-steel shader code or its baked numbers
// change (Guard, Tip below, the material's lift in emit_spirv_context_get_set.cpp). Both stages'
// keys carry it (vk_pipeline_cache.cpp BuildRuntimeInfo), so no old cached shader is reused.
constexpr unsigned ShaderRevision = 3;

// Split parts only. Each key is the original 16-bit index stream of one colour LOD.
// Parts 0/1 are Mercy left/right and 2/3 Rakuyo left/right.
struct Face {
    std::uint64_t hash{};
    unsigned part{};
    std::uint64_t alternate{}; // low-detail asset with the same draw count
    constexpr bool Matches(std::uint64_t value) const {
        return value != 0 && (value == hash || value == alternate);
    }
};
constexpr Face Find(unsigned count) {
    switch (count) {
    case 420: return {0xde4b00beec185aacull, 1, 0x9133f6bebe6ffa88ull};
    case 465: return {0x719385b85086adfdull, 0};
    case 471: return {0x7e87fcac42c387c4ull, 0};
    case 546: return {0x20b8571de3354a11ull, 2};
    case 549: return {0xfe94020303c9294eull, 2};
    case 1263: return {0x919b03c024772b49ull, 1};
    case 1266: return {0x3aec72043a35c00aull, 1};
    case 1374: return {0x57b6b6377cdb6d29ull, 3};
    case 1380: return {0x2d701837cfd41687ull, 3};
    case 1422: return {0xe151b5bdb2200479ull, 0, 0x1e5e361c3709c8a7ull};
    case 1650: return {0x5361d764f621db26ull, 2};
    case 1653: return {0x218bc908265d444full, 2};
    case 2112: return {0x345705918f568fb0ull, 1, 0x3d3383d93141e346ull};
    case 2376: return {0x79f2fb342c13373bull, 0, 0x0e9d1f157877bfbfull};
    case 2760: return {0x13b11f14f03f73b2ull, 2, 0x9e32513544ab2b7aull};
    case 4179: return {0xd55b84f7e4202952ull, 3, 0x7e834969d48abf37ull};
    case 6996: return {0xb4430d14e081b6e9ull, 3, 0xfd185f3962497d77ull};
    default: return {};
    }
}

// In FLVER model space the blades point down -Y. Measured against all non-steel UV
// islands, including the guard. Round outwards to leave their foremost texels dry.
constexpr float Guard(unsigned part) {
    constexpr float metres[]{.113f, .115f, .127f, .237f};
    return metres[part & 3];
}
constexpr float Tip(unsigned part) {
    constexpr float metres[]{.603f, .632f, .518f, 1.054f};
    return metres[part & 3];
}
} // namespace VR::HonedSteel
