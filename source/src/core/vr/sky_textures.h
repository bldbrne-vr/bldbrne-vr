// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// The game's sky, moon and cloud textures in the player's own dump, as the runtime's sky detail
// (video_core/texture_cache/sky_detail.cpp) and the launcher's sharper skies
// (launcher/sky_upscale.cpp) both find them: the map texture lists (dvdroot_ps4/map/*/m*.tpfbhd,
// the game update's *.tpfbhd.patch) and the PS4 TPFs in their .tpfbdt. Plain C++ without the
// emulator's headers: the launcher builds it too.
namespace VR::SkyTextures {

// One file of a texture list (BHF4 as the game's map lists have it: 0x24-byte headers, UTF-16
// names): its name in lower case ("m21_00_moon_a.tpf.dcx"), where it is in the .tpfbdt and its
// packed size.
struct ListEntry {
    std::string name;
    std::uint64_t offset{}, size{};
};
std::vector<ListEntry> ReadList(const std::vector<std::uint8_t>& bhd);

// The .tpfbdt holding a list's files (m24_0000.tpfbhd -> m24_0000.tpfbdt, the update's
// m24_0000.tpfbhd.patch -> m24_0000.tpfbdt.patch); nullopt when `file` is not a texture list.
std::optional<std::filesystem::path> DataFile(const std::filesystem::path& file);

// A sky, moon or cloud albedo texture (not its "_l" low-detail twin or its other maps). Most of
// the game's cloud layers are spelled "clond" (m21_00_clond_100_a, m26_clond_103_a, ...).
bool Wanted(const std::string& name);

// The texture's name without .tpf.dcx ("m21_00_moon_a"): the sharper image is <name>.png.
std::string TextureName(const std::string& list_name);

// The copy's scale: a panorama (a strip around the horizon, 4:1 or wider) to `max_size` wide,
// anything else 2x while it stays within `max_size`; 1 = no copy (also under 256 x 128).
std::uint32_t ScaleFor(std::uint32_t w, std::uint32_t h, std::uint32_t max_size);

// Weapon replacements encode their original dimensions as half the PNG dimensions.
// Never use the sky panorama scale; division keeps the limit check overflow-safe.
constexpr std::uint32_t WeaponScaleFor(std::uint32_t w, std::uint32_t h,
                                       std::uint32_t max_size) {
    return w >= 256 && h >= 128 && w <= max_size / 2 && h <= max_size / 2 ? 2 : 1;
}

enum class Blocks { Bc1, Bc7, Other };

// The first texture of a PS4 TPF (the sky, moon and cloud files hold one each).
struct Texture {
    std::uint32_t offset{}, size{}; // its data in the TPF
    std::uint32_t width{}, height{};
    std::uint8_t format{}; // the TPF's: 0 BC1, 1 BC1 with 1-bit alpha, 102.. BC7
    std::uint32_t dxgi{};
    Blocks blocks{Blocks::Other};
};
std::optional<Texture> FirstTexture(const std::vector<std::uint8_t>& tpf);

// The bytes that identify a texture: the top level's first 64 KiB (the whole top level of a
// 512 x 512 BC1 moon is 128 KiB). The runtime hashes the same bytes of the game's image.
inline constexpr std::size_t KeyBytes = 64 * 1024;

// Every texture of a PS4 TPF with its name in lower case (a weapon's file holds its albedo,
// normal, roughness and specular maps: "wp_a_0800_a", "wp_a_0800_n", ...).
struct NamedTexture : Texture {
    std::string name;
};
std::vector<NamedTexture> Textures(const std::vector<std::uint8_t>& tpf);

// ---- Sharper weapons (the launcher's, launcher/sky_upscale.h Kind::Weapons) ----------------------
// The game's weapon part files: dvdroot_ps4/parts/wp_*.partsbnd.dcx (DCX -> BND4 -> WP_A_XXXX.tpf
// and its model), never the "_l" low-detail copies drawn far away. `file_name` in lower case.
bool WeaponFile(const std::string& file_name);
// A weapon's albedo ("wp_a_0800_a"), not its normal, roughness or specular maps: only colour
// textures are upscaled by the AI (a normal map's vectors are not an image of anything).
bool WeaponWanted(const std::string& texture_name);
// One file of a BND4 (its base name in lower case, where it is, its size).
struct BndFile {
    std::string name;
    std::uint64_t offset{}, size{};
};
std::vector<BndFile> ReadBnd4(const std::vector<std::uint8_t>& bnd);

// The top level as RGBA8, the stored (sRGB-encoded) values: the PS4 tiling undone (8 x 8 blocks
// per tile, Morton order x0 y0 x1 y1 x2 y2), then BC1 (format 1: index 3 of a three-colour block
// transparent) or BC7 decoded. Empty for any other format or a truncated texture.
std::vector<std::uint8_t> DecodeTop(const std::vector<std::uint8_t>& tpf, const Texture& texture);

} // namespace VR::SkyTextures
