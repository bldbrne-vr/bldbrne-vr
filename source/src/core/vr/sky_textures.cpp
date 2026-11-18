// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/vr/sky_textures.h"

#include <algorithm>
#include <bit>
#include <cctype>
#include <cstring>

#include "core/vr/bc7_decode.h"

namespace VR::SkyTextures {

namespace {

template <typename T>
bool ReadAt(const std::vector<std::uint8_t>& d, std::size_t offset, T& out) {
    if (offset + sizeof(T) > d.size()) {
        return false;
    }
    std::memcpy(&out, d.data() + offset, sizeof(T));
    return true;
}

bool EndsWith(const std::string& s, std::string_view end) {
    return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
}

// One BC1 block into 4 x 4 RGBA texels (row by row). `punch`: index 3 of a three-colour block
// (c0 <= c1) is transparent black, else opaque black.
void DecodeBc1(const std::uint8_t* b, bool punch, std::uint8_t out[64]) {
    const unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
    std::uint8_t p[4][4];
    const auto expand = [](unsigned c, std::uint8_t* o) {
        const unsigned r = c >> 11 & 31, g = c >> 5 & 63, bl = c & 31;
        o[0] = static_cast<std::uint8_t>(r << 3 | r >> 2);
        o[1] = static_cast<std::uint8_t>(g << 2 | g >> 4);
        o[2] = static_cast<std::uint8_t>(bl << 3 | bl >> 2);
        o[3] = 255;
    };
    expand(c0, p[0]);
    expand(c1, p[1]);
    for (int k = 0; k < 3; ++k) {
        if (c0 > c1) {
            p[2][k] = static_cast<std::uint8_t>((2 * p[0][k] + p[1][k]) / 3);
            p[3][k] = static_cast<std::uint8_t>((p[0][k] + 2 * p[1][k]) / 3);
        } else {
            p[2][k] = static_cast<std::uint8_t>((p[0][k] + p[1][k]) / 2);
            p[3][k] = 0;
        }
    }
    p[2][3] = 255;
    p[3][3] = c0 > c1 || !punch ? 255 : 0;
    const std::uint32_t index = b[4] | b[5] << 8 | b[6] << 16 | static_cast<std::uint32_t>(b[7]) << 24;
    for (int i = 0; i < 16; ++i) {
        std::memcpy(out + i * 4, p[index >> (2 * i) & 3], 4);
    }
}

} // namespace

std::vector<ListEntry> ReadList(const std::vector<std::uint8_t>& d) {
    std::vector<ListEntry> out;
    std::int32_t count{};
    std::int64_t header{};
    if (d.size() < 0x40 || std::memcmp(d.data(), "BHF4", 4) != 0 || !ReadAt(d, 0x0C, count) ||
        !ReadAt(d, 0x20, header) || header != 0x24 || count <= 0 || count > 100000 ||
        d[0x30] != 1) {
        return out;
    }
    for (std::int32_t k = 0; k < count; ++k) {
        const std::size_t at = 0x40 + static_cast<std::size_t>(header) * static_cast<std::size_t>(k);
        std::int64_t size{};
        std::int32_t offset{}, name{};
        if (!ReadAt(d, at + 8, size) || !ReadAt(d, at + 24, offset) || !ReadAt(d, at + 32, name) ||
            size <= 0 || offset < 0 || name < 0) {
            break;
        }
        ListEntry e{.offset = static_cast<std::uint64_t>(offset),
                    .size = static_cast<std::uint64_t>(size)};
        for (std::size_t p = static_cast<std::size_t>(name); p + 1 < d.size(); p += 2) {
            const unsigned c = d[p] | d[p + 1] << 8;
            if (c == 0) {
                break;
            }
            e.name.push_back(c < 0x80 ? static_cast<char>(std::tolower(static_cast<int>(c))) : '?');
        }
        out.push_back(std::move(e));
    }
    return out;
}

std::optional<std::filesystem::path> DataFile(const std::filesystem::path& file) {
    const std::string name = file.filename().string();
    const std::size_t ext = name.find(".tpfbhd");
    if (ext == std::string::npos) {
        return std::nullopt;
    }
    const std::string suffix = name.substr(ext + 7);
    if (!suffix.empty() && suffix != ".patch") {
        return std::nullopt;
    }
    return file.parent_path() / (name.substr(0, ext) + ".tpfbdt" + suffix);
}

bool Wanted(const std::string& name) {
    if (!EndsWith(name, "_a.tpf.dcx") && !EndsWith(name, "_a.tpf")) {
        return false;
    }
    return name.find("sky") != std::string::npos || name.find("moon") != std::string::npos ||
           name.find("cloud") != std::string::npos || name.find("clond") != std::string::npos;
}

std::string TextureName(const std::string& list_name) {
    return list_name.substr(0, list_name.find(".tpf"));
}

std::uint32_t ScaleFor(std::uint32_t w, std::uint32_t h, std::uint32_t max_size) {
    if (w < 256 || h < 128) {
        return 1;
    }
    std::uint32_t scale = w >= 4 * h ? std::bit_floor(std::max(max_size / w, 1u)) : 2u;
    scale = std::min(scale, 8u);
    while (scale > 1 && (w * scale > max_size || h * scale > max_size)) {
        scale /= 2;
    }
    return scale;
}

std::optional<Texture> FirstTexture(const std::vector<std::uint8_t>& tpf) {
    // PS4 TPF: the first entry at 0x10 (data offset, size, format, type, mips, flags, width,
    // height, 8 more, name offset, a flag), its DXGI format at 0x30.
    Texture t;
    std::int32_t size{};
    std::uint16_t w{}, h{};
    if (tpf.size() < 0x34 || std::memcmp(tpf.data(), "TPF\0", 4) != 0 || tpf[0x0C] != 4 ||
        !ReadAt(tpf, 0x10, t.offset) || !ReadAt(tpf, 0x14, size) || !ReadAt(tpf, 0x1C, w) ||
        !ReadAt(tpf, 0x1E, h) || !ReadAt(tpf, 0x30, t.dxgi) || size <= 0 || w == 0 || h == 0 ||
        static_cast<std::uint64_t>(t.offset) + static_cast<std::uint64_t>(size) > tpf.size()) {
        return std::nullopt;
    }
    t.size = static_cast<std::uint32_t>(size);
    t.width = w;
    t.height = h;
    t.format = tpf[0x18];
    if (t.format == 0 || t.format == 1 || t.dxgi == 71 || t.dxgi == 72) {
        t.blocks = Blocks::Bc1;
    } else if (t.format == 102 || t.format == 106 || t.format == 107 || t.format == 112 ||
               t.dxgi == 98 || t.dxgi == 99) {
        t.blocks = Blocks::Bc7;
    }
    return t;
}

std::vector<NamedTexture> Textures(const std::vector<std::uint8_t>& tpf) {
    // PS4 TPF: `count` entries of 0x24 bytes from 0x10 (data offset, size, format, type, mips,
    // flags, width, height, 8 more, name offset, a flag, DXGI format), UTF-16 names.
    std::vector<NamedTexture> out;
    std::int32_t count{};
    if (tpf.size() < 0x10 || std::memcmp(tpf.data(), "TPF\0", 4) != 0 || tpf[0x0C] != 4 ||
        !ReadAt(tpf, 8, count) || count <= 0 || count > 4096 ||
        0x10 + static_cast<std::size_t>(count) * 0x24 > tpf.size()) {
        return out;
    }
    for (std::int32_t i = 0; i < count; ++i) {
        const std::size_t e = 0x10 + static_cast<std::size_t>(i) * 0x24;
        NamedTexture t;
        std::int32_t size{};
        std::uint16_t w{}, h{};
        std::uint32_t name_at{};
        if (!ReadAt(tpf, e, t.offset) || !ReadAt(tpf, e + 4, size) || !ReadAt(tpf, e + 0xC, w) ||
            !ReadAt(tpf, e + 0xE, h) || !ReadAt(tpf, e + 0x18, name_at) ||
            !ReadAt(tpf, e + 0x20, t.dxgi) || size <= 0 || w == 0 || h == 0 ||
            static_cast<std::uint64_t>(t.offset) + static_cast<std::uint64_t>(size) > tpf.size()) {
            continue;
        }
        for (std::size_t o = name_at; o + 1 < tpf.size() && t.name.size() < 128; o += 2) {
            const unsigned c = tpf[o] | tpf[o + 1] << 8;
            if (c == 0) {
                break;
            }
            t.name += static_cast<char>(c < 0x80 ? std::tolower(static_cast<int>(c)) : '?');
        }
        t.size = static_cast<std::uint32_t>(size);
        t.width = w;
        t.height = h;
        t.format = tpf[e + 8];
        if (t.format == 0 || t.format == 1 || t.dxgi == 71 || t.dxgi == 72) {
            t.blocks = Blocks::Bc1;
        } else if (t.format == 102 || t.format == 106 || t.format == 107 || t.format == 112 ||
                   t.dxgi == 98 || t.dxgi == 99) {
            t.blocks = Blocks::Bc7;
        }
        out.push_back(std::move(t));
    }
    return out;
}

bool WeaponFile(const std::string& file_name) {
    return file_name.starts_with("wp_") && EndsWith(file_name, ".partsbnd.dcx") &&
           !EndsWith(file_name, "_l.partsbnd.dcx");
}

bool WeaponWanted(const std::string& texture_name) {
    return texture_name.starts_with("wp_") && EndsWith(texture_name, "_a");
}

std::vector<BndFile> ReadBnd4(const std::vector<std::uint8_t>& bnd) {
    std::vector<BndFile> out;
    std::int32_t count{};
    std::int64_t header{};
    if (bnd.size() < 0x40 || std::memcmp(bnd.data(), "BND4", 4) != 0 || !ReadAt(bnd, 0x0C, count) ||
        !ReadAt(bnd, 0x20, header) || header < 0x24 || header > 0x100 || count <= 0 ||
        count > 65536) {
        return out;
    }
    const bool utf16 = bnd[0x30] != 0;
    for (std::int32_t k = 0; k < count; ++k) {
        const std::size_t entry = 0x40 + static_cast<std::size_t>(header) * static_cast<std::size_t>(k);
        std::int64_t size{};
        std::uint32_t offset{}, name_at{};
        if (!ReadAt(bnd, entry + 8, size) || !ReadAt(bnd, entry + 24, offset) ||
            !ReadAt(bnd, entry + 32, name_at)) {
            break;
        }
        if (size <= 0 || offset > bnd.size() || static_cast<std::uint64_t>(size) > bnd.size() - offset) {
            continue;
        }
        BndFile f{{}, offset, static_cast<std::uint64_t>(size)};
        for (std::size_t o = name_at; o < bnd.size() && f.name.size() < 512; o += utf16 ? 2 : 1) {
            const unsigned c = utf16 ? (o + 1 < bnd.size() ? bnd[o] | bnd[o + 1] << 8 : 0) : bnd[o];
            if (c == 0) {
                break;
            }
            if (c == '\\' || c == '/') {
                f.name.clear(); // the base name only
                continue;
            }
            f.name += static_cast<char>(c < 0x80 ? std::tolower(static_cast<int>(c)) : '?');
        }
        out.push_back(std::move(f));
    }
    return out;
}

std::vector<std::uint8_t> DecodeTop(const std::vector<std::uint8_t>& tpf, const Texture& t) {
    if (t.blocks == Blocks::Other) {
        return {};
    }
    const bool bc7 = t.blocks == Blocks::Bc7;
    const std::size_t block_bytes = bc7 ? 16 : 8;
    const std::uint32_t bw = (t.width + 3) / 4, bh = (t.height + 3) / 4;
    const std::uint32_t tiles_x = (bw + 7) / 8, tiles_y = (bh + 7) / 8;
    const std::size_t tiled = static_cast<std::size_t>(tiles_x) * tiles_y * 64 * block_bytes;
    if (tiled > t.size) {
        return {};
    }
    const std::uint8_t* src = tpf.data() + t.offset;
    std::vector<std::uint8_t> rgba(static_cast<std::size_t>(t.width) * t.height * 4);
    std::uint8_t texels[64];
    for (std::uint32_t tile = 0; tile < tiles_x * tiles_y; ++tile) {
        const std::uint32_t tx = tile % tiles_x, ty = tile / tiles_x;
        for (std::uint32_t e = 0; e < 64; ++e) {
            const std::uint32_t x = tx * 8 + ((e & 1) | (e >> 1 & 2) | (e >> 2 & 4));
            const std::uint32_t y = ty * 8 + ((e >> 1 & 1) | (e >> 2 & 2) | (e >> 3 & 4));
            if (x >= bw || y >= bh) {
                continue;
            }
            const std::uint8_t* block = src + (static_cast<std::size_t>(tile) * 64 + e) * block_bytes;
            if (bc7) {
                Bc7::DecodeBlock(block, texels);
            } else {
                DecodeBc1(block, t.format == 1, texels);
            }
            for (std::uint32_t row = 0; row < 4 && y * 4 + row < t.height; ++row) {
                const std::uint32_t columns = std::min(4u, t.width - x * 4);
                std::memcpy(rgba.data() + ((static_cast<std::size_t>(y) * 4 + row) * t.width + x * 4) * 4,
                            texels + row * 16, columns * 4);
            }
        }
    }
    return rgba;
}

} // namespace VR::SkyTextures
