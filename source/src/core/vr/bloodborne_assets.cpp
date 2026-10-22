// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <mutex>
#include <cmath>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <imgui.h>
#include <cmrc/cmrc.hpp>
#include <miniz.h>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/emulator_settings.h"
#include "core/file_sys/fs.h"
#include "core/vr/bloodborne_assets.h"
#include "core/vr/music_box_mesh.h"
#include "core/vr/music_box_texture.h"
#include "core/vr/bc7_decode.h"
#include "core/vr/game_font.h"
#include "core/vr/vr_session.h"
#include "imgui/renderer/font_data.h"

CMRC_DECLARE(res);

namespace VR::Assets {

namespace {

constexpr float FontPx = 48.0f;  // the atlas' glyph size (labels are scaled in the frame)
constexpr int OutlinePx = 3;     // outline radius in label texels
constexpr int PadPx = OutlinePx + 3;

template <typename T>
bool ReadAt(const std::vector<u8>& data, size_t offset, T& out) {
    if (offset + sizeof(T) > data.size()) {
        return false;
    }
    std::memcpy(&out, data.data() + offset, sizeof(T));
    return true;
}

u32 BigEndian32(const std::vector<u8>& d, size_t o) {
    return o + 4 <= d.size() ? (u32{d[o]} << 24) | (u32{d[o + 1]} << 16) | (u32{d[o + 2]} << 8) |
                                   u32{d[o + 3]}
                             : 0;
}

// DCX ("DCX\0", DCS sizes big-endian, DCP "DFLT", DCA + 8: a zlib stream).
std::optional<std::vector<u8>> Dcx(const std::vector<u8>& file) {
    if (file.size() < 0x50 || std::memcmp(file.data(), "DCX\0", 4) != 0) {
        return std::nullopt;
    }
    size_t dcs = 0;
    while (dcs + 12 < file.size() && std::memcmp(file.data() + dcs, "DCS\0", 4) != 0) {
        ++dcs;
    }
    size_t dca = dcs;
    while (dca + 8 < file.size() && std::memcmp(file.data() + dca, "DCA\0", 4) != 0) {
        ++dca;
    }
    if (dcs + 12 >= file.size() || dca + 8 >= file.size()) {
        return std::nullopt;
    }
    const u32 size = BigEndian32(file, dcs + 4);
    const u32 packed = BigEndian32(file, dcs + 8);
    // The sfx archive is the largest read here (87 MB unpacked).
    if (size == 0 || size > 128u << 20 || dca + 8 + packed > file.size() + 16) {
        return std::nullopt;
    }
    std::vector<u8> out(size);
    mz_ulong length = size;
    const auto available = static_cast<mz_ulong>(file.size() - (dca + 8));
    if (mz_uncompress(out.data(), &length, file.data() + dca + 8, available) != MZ_OK ||
        length != size) {
        return std::nullopt;
    }
    return out;
}

// BND4: the file with `id` (nullopt when absent).
std::optional<std::vector<u8>> Bnd4File(const std::vector<u8>& bnd, s32 id) {
    s32 count{};
    s64 header{};
    if (bnd.size() < 0x40 || std::memcmp(bnd.data(), "BND4", 4) != 0 ||
        !ReadAt(bnd, 0x0C, count) || !ReadAt(bnd, 0x20, header) || header < 0x20 ||
        header > 0x100 || count <= 0 || count > 4096) {
        return std::nullopt;
    }
    for (s32 k = 0; k < count; ++k) {
        const size_t entry = 0x40 + static_cast<size_t>(header) * static_cast<size_t>(k);
        s64 size{};
        u32 offset{};
        s32 file_id{};
        if (!ReadAt(bnd, entry + 8, size) || !ReadAt(bnd, entry + 24, offset) ||
            !ReadAt(bnd, entry + 28, file_id)) {
            return std::nullopt;
        }
        if (file_id == id && size > 0 && offset + static_cast<u64>(size) <= bnd.size()) {
            return std::vector<u8>(bnd.begin() + offset, bnd.begin() + offset + size);
        }
    }
    return std::nullopt;
}

// BND4 with UTF-16 names (the sfx archive: "N:\SPRJ\data\INTERROOT_ps4\sfx\...\s10010.flver"):
// every file whose name ends with one of `wanted` (ASCII, case-insensitive), by that suffix.
std::unordered_map<std::string, std::vector<u8>> Bnd4FilesNamed(
    const std::vector<u8>& bnd, const std::vector<std::string>& wanted) {
    std::unordered_map<std::string, std::vector<u8>> out;
    s32 count{};
    s64 header{};
    if (bnd.size() < 0x40 || std::memcmp(bnd.data(), "BND4", 4) != 0 || !ReadAt(bnd, 0x0C, count) ||
        !ReadAt(bnd, 0x20, header) || header < 0x24 || header > 0x100 || count <= 0 ||
        count > 65536 || bnd[0x30] == 0) {
        return out;
    }
    for (s32 k = 0; k < count; ++k) {
        const size_t entry = 0x40 + static_cast<size_t>(header) * static_cast<size_t>(k);
        s64 size{};
        u32 offset{}, name{};
        if (!ReadAt(bnd, entry + 8, size) || !ReadAt(bnd, entry + 24, offset) ||
            !ReadAt(bnd, entry + 32, name) || size <= 0 ||
            offset + static_cast<u64>(size) > bnd.size()) {
            continue;
        }
        // The name's ASCII tail (the base name), lower case.
        std::string tail;
        for (size_t o = name; o + 1 < bnd.size(); o += 2) {
            const u32 c = u32{bnd[o]} | (u32{bnd[o + 1]} << 8);
            if (c == 0) {
                break;
            }
            if (c == '\\' || c == '/') {
                tail.clear();
                continue;
            }
            tail += static_cast<char>(c < 0x80 ? std::tolower(static_cast<int>(c)) : '?');
        }
        if (std::find(wanted.begin(), wanted.end(), tail) != wanted.end()) {
            out[tail] = std::vector<u8>(bnd.begin() + offset, bnd.begin() + offset + size);
        }
    }
    return out;
}

void AppendUtf8(std::string& s, u32 c) {
    if (c < 0x80) {
        s += static_cast<char>(c);
    } else if (c < 0x800) {
        s += static_cast<char>(0xC0 | (c >> 6));
        s += static_cast<char>(0x80 | (c & 0x3F));
    } else {
        s += static_cast<char>(0xE0 | (c >> 12));
        s += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
        s += static_cast<char>(0x80 | (c & 0x3F));
    }
}

// FMG (version 2, 64-bit offsets): id -> UTF-8.
std::unordered_map<s32, std::string> Fmg(const std::vector<u8>& d) {
    std::unordered_map<s32, std::string> out;
    s32 groups{};
    s64 strings{};
    if (!ReadAt(d, 0x0C, groups) || !ReadAt(d, 0x18, strings) || groups <= 0 || groups > 65536) {
        return out;
    }
    for (s32 g = 0; g < groups; ++g) {
        s32 index{}, first{}, last{};
        const size_t at = 0x28 + 16 * static_cast<size_t>(g);
        if (!ReadAt(d, at, index) || !ReadAt(d, at + 4, first) || !ReadAt(d, at + 8, last) ||
            last < first || last - first > 100000) {
            break;
        }
        for (s32 id = first; id <= last; ++id) {
            s64 offset{};
            if (!ReadAt(d, static_cast<size_t>(strings) + 8 * static_cast<size_t>(index + id -
                                                                                   first),
                        offset) ||
                offset <= 0 || static_cast<size_t>(offset) >= d.size()) {
                continue;
            }
            std::string text;
            for (size_t o = static_cast<size_t>(offset); o + 1 < d.size(); o += 2) {
                const u32 c = u32{d[o]} | (u32{d[o + 1]} << 8);
                if (c == 0) {
                    break;
                }
                AppendUtf8(text, c);
            }
            if (!text.empty()) {
                out[id] = std::move(text);
            }
        }
    }
    return out;
}

// Decodes one UTF-8 character and advances `s` (0 at the end or on a broken sequence).
u32 NextUtf8(const char*& s) {
    const auto b = [&s](int i) { return static_cast<u32>(static_cast<unsigned char>(s[i])); };
    const u32 c0 = b(0);
    if (c0 == 0) {
        return 0;
    }
    if (c0 < 0x80) {
        s += 1;
        return c0;
    }
    if ((c0 & 0xE0) == 0xC0 && (b(1) & 0xC0) == 0x80) {
        s += 2;
        return ((c0 & 0x1F) << 6) | (b(1) & 0x3F);
    }
    if ((c0 & 0xF0) == 0xE0 && (b(1) & 0xC0) == 0x80 && (b(2) & 0xC0) == 0x80) {
        s += 3;
        return ((c0 & 0x0F) << 12) | ((b(1) & 0x3F) << 6) | (b(2) & 0x3F);
    }
    return 0;
}

// The console language's msg folder (PS4 system language ids).
const char* LanguageFolder(int language) {
    switch (language) {
    case 0:
        return "jpnjp";
    case 2:
        return "frafr";
    case 3:
        return "spaes";
    case 4:
        return "deude";
    case 5:
        return "itait";
    case 6:
        return "nldnl";
    case 7:
        return "porpt";
    case 8:
        return "rusru";
    case 9:
        return "korkr";
    case 10:
        return "zhotw";
    case 11:
        return "zhocn";
    case 12:
        return "finfi";
    case 13:
        return "swese";
    case 14:
        return "dandk";
    case 15:
        return "norno";
    case 16:
        return "polpl";
    case 17:
        return "porbr";
    case 18:
        return "enggb";
    case 19:
        return "turtr";
    case 20:
        return "spaar";
    default:
        return "engus";
    }
}

std::optional<std::vector<u8>> ReadGameFile(const std::string& guest) {
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    return mnt->ReadFile(guest);
}

// PS4 TPF with one BC7 texture (format 102), micro-tiled (mode 13): detiled to linear blocks.
std::shared_ptr<Icon> DecodeIcon(const std::vector<u8>& tpf, std::string& why) {
    s32 count{};
    u32 offset{}, width16{};
    s32 size{};
    if (tpf.size() < 0x40 || std::memcmp(tpf.data(), "TPF\0", 4) != 0 || !ReadAt(tpf, 8, count) ||
        count < 1 || tpf[0x0C] != 4) {
        why = "not a PS4 TPF";
        return nullptr;
    }
    ReadAt(tpf, 0x10, offset);
    ReadAt(tpf, 0x14, size);
    const u8 format = tpf[0x18];
    u16 w{}, h{};
    ReadAt(tpf, 0x1C, w);
    ReadAt(tpf, 0x1E, h);
    u32 tile_mode{};
    ReadAt(tpf, 0x24, tile_mode);
    (void)width16;
    if (format != 102 && format != 106 && format != 107) {
        why = fmt::format("format {} is not BC7", format);
        return nullptr;
    }
    const u32 bw = (w + 3) / 4, bh = (h + 3) / 4;
    const u32 pw = (bw + 7) / 8 * 8, ph = (bh + 7) / 8 * 8; // padded to whole 8 x 8 tiles
    if (w == 0 || h == 0 || w > 2048 || h > 2048 ||
        static_cast<u64>(offset) + static_cast<u64>(pw) * ph * 16 > tpf.size() ||
        static_cast<u64>(size) < static_cast<u64>(pw) * ph * 16) {
        why = fmt::format("{}x{} does not fit its {} bytes", w, h, size);
        return nullptr;
    }
    auto icon = std::make_shared<Icon>();
    icon->width = pw * 4;
    icon->height = ph * 4;
    icon->used = w;
    icon->bc7.resize(static_cast<size_t>(pw) * ph * 16);
    const u8* src = tpf.data() + offset;
    const u32 tiles_x = pw / 8;
    for (u32 tile = 0; tile < tiles_x * (ph / 8); ++tile) {
        const u32 tx = tile % tiles_x, ty = tile / tiles_x;
        for (u32 e = 0; e < 64; ++e) {
            // Morton order inside the micro tile: x0 y0 x1 y1 x2 y2.
            const u32 x = (e & 1) | ((e >> 1) & 2) | ((e >> 2) & 4);
            const u32 y = ((e >> 1) & 1) | ((e >> 2) & 2) | ((e >> 3) & 4);
            const u32 dst = (ty * 8 + y) * pw + tx * 8 + x;
            std::memcpy(icon->bc7.data() + static_cast<size_t>(dst) * 16,
                        src + (static_cast<size_t>(tile) * 64 + e) * 16, 16);
        }
    }
    if (tile_mode != 13) {
        LOG_WARNING(Core, "VR assets: icon tile mode {} (expected 13); decoded as 13", tile_mode);
    }
    return icon;
}

// ---- Held-item models (the sfx archive)
// ----------------------------------------------------------

// The models of the held-item table (core/vr/bloodborne_quickitems.cpp HeldModelOf): all read in
// one pass over the archive (87 MB unpacked, dropped again once they are out).
constexpr std::array<s32, 29> HeldModelIds = {
    10002, // Blood Vial
    10004, // Shining Coins
    10005, // a bell
    10009, // Numbing Mist
    10010, // Molotov Cocktail
    10012, // Pebble
    10013, // Pungent Blood Cocktail's parts
    10014, // Oil Urn
    10015, // Pungent Blood Cocktail
    10016, // Throwing Knife / Poison Knife
    10017, // Blue Elixir
    10019, // a blood bottle
    10021, // a small lamp
    10022, // Sedative
    10023, // Lead Elixir
    10024, // Monocular
    10020, 10051, 10070, // bone blade, delayed Molotov, Vermin
    11010, 11012, 11013, 11014, 11015, 11016, // Hunter tools and music box
    11050, 11051, 11052, 11401, // DLC tools and Silencing Blank
};
constexpr const char* SfxArchive = "/app0/dvdroot_ps4/sfx/frpg_sfxbnd_commoneffects.ffxbnd.dcx";

ModelTexture::Format TpfFormat(u8 format, u32& block_bytes) {
    switch (format) {
    case 0:
    case 1:
    case 24:
    case 25:
    case 108:
    case 109:
        block_bytes = 8;
        return ModelTexture::Format::Bc1;
    case 5:
    case 23:
    case 33:
    case 110:
        block_bytes = 16;
        return ModelTexture::Format::Bc3;
    case 102:
    case 106:
    case 107:
    case 112:
        block_bytes = 16;
        return ModelTexture::Format::Bc7;
    default:
        block_bytes = 0;
        return ModelTexture::Format::None;
    }
}

// PS4 micro-tiled blocks (tile mode 13, thin 1D: 8 x 8 blocks per tile in Morton order x0 y0 x1
// y1 x2 y2, tiles row by row, every mip level padded to whole tiles) -> linear blocks appended to
// `dst`. False when `size` bytes do not hold the level.
bool DetileLevel(const u8* src, size_t size, u32 bw, u32 bh, u32 block_bytes,
                 std::vector<u8>& dst) {
    const u32 pw = (bw + 7) / 8 * 8, ph = (bh + 7) / 8 * 8;
    if (static_cast<size_t>(pw) * ph * block_bytes > size) {
        return false;
    }
    const size_t base = dst.size();
    dst.resize(base + static_cast<size_t>(bw) * bh * block_bytes);
    const u32 tiles_x = pw / 8;
    for (u32 tile = 0; tile < tiles_x * (ph / 8); ++tile) {
        const u32 tx = tile % tiles_x, ty = tile / tiles_x;
        for (u32 e = 0; e < 64; ++e) {
            const u32 x = tx * 8 + ((e & 1) | ((e >> 1) & 2) | ((e >> 2) & 4));
            const u32 y = ty * 8 + (((e >> 1) & 1) | ((e >> 2) & 2) | ((e >> 3) & 4));
            if (x >= bw || y >= bh) {
                continue;
            }
            std::memcpy(dst.data() + base + (static_cast<size_t>(y) * bw + x) * block_bytes,
                        src + (static_cast<size_t>(tile) * 64 + e) * block_bytes, block_bytes);
        }
    }
    return true;
}

// A PS4 TPF's first texture with its whole mip chain.
bool DecodeTexture(const std::vector<u8>& tpf, ModelTexture& out, std::string& why) {
    s32 count{};
    if (tpf.size() < 0x40 || std::memcmp(tpf.data(), "TPF\0", 4) != 0 || !ReadAt(tpf, 8, count) ||
        count < 1 || tpf[0x0C] != 4) {
        why = "not a PS4 TPF";
        return false;
    }
    u32 offset{}, tile_mode{};
    s32 size{};
    u16 w{}, h{};
    ReadAt(tpf, 0x10, offset);
    ReadAt(tpf, 0x14, size);
    ReadAt(tpf, 0x1C, w);
    ReadAt(tpf, 0x1E, h);
    ReadAt(tpf, 0x24, tile_mode);
    const u8 format = tpf[0x18];
    const u32 mips = std::max<u32>(tpf[0x1A], 1);
    u32 block_bytes = 0;
    out.format = TpfFormat(format, block_bytes);
    if (out.format == ModelTexture::Format::None) {
        why = fmt::format("texture format {} not handled", format);
        return false;
    }
    if (w == 0 || h == 0 || w > 4096 || h > 4096 || tile_mode != 13 || size <= 0 ||
        static_cast<u64>(offset) + static_cast<u64>(size) > tpf.size()) {
        why = fmt::format("{}x{} tile mode {} ({} bytes at {:#x})", w, h, tile_mode, size, offset);
        return false;
    }
    out.width = (w + 3u) / 4 * 4;
    out.height = (h + 3u) / 4 * 4;
    out.levels = 0;
    out.blocks.clear();
    out.level_offset.clear();
    size_t at = offset;
    const size_t end = static_cast<size_t>(offset) + static_cast<size_t>(size);
    for (u32 level = 0; level < mips; ++level) {
        const u32 lw = std::max<u32>(w >> level, 1), lh = std::max<u32>(h >> level, 1);
        const u32 bw = (lw + 3) / 4, bh = (lh + 3) / 4;
        out.level_offset.push_back(static_cast<u32>(out.blocks.size()));
        if (!DetileLevel(tpf.data() + at, end - at, bw, bh, block_bytes, out.blocks)) {
            out.level_offset.pop_back();
            break; // the levels so far are complete
        }
        at += static_cast<size_t>((bw + 7) / 8 * 8) * ((bh + 7) / 8 * 8) * block_bytes;
        ++out.levels;
        if (lw == 1 && lh == 1) {
            break;
        }
    }
    if (out.levels == 0) {
        why = "level 0 does not fit";
        return false;
    }
    return true;
}

std::string FlverString(const std::vector<u8>& d, size_t offset, bool unicode) {
    std::string s;
    for (size_t o = offset; o < d.size(); o += unicode ? 2 : 1) {
        const u32 c = unicode ? (o + 1 < d.size() ? u32{d[o]} | (u32{d[o + 1]} << 8) : 0) : d[o];
        if (c == 0) {
            break;
        }
        AppendUtf8(s, c);
    }
    return s;
}

// FLVER bone transform (as the FLVER tools build it): scale, then rotations about X, Z and Y (in
// that order), then the translation. 3 x 4 row-major [R | t].
std::array<float, 12> BoneMatrix(const float t[3], const float r[3], const float s[3]) {
    const float cx = std::cos(r[0]), sx = std::sin(r[0]);
    const float cy = std::cos(r[1]), sy = std::sin(r[1]);
    const float cz = std::cos(r[2]), sz = std::sin(r[2]);
    // Ry * Rz * Rx
    const float rx[9] = {1, 0, 0, 0, cx, -sx, 0, sx, cx};
    const float rz[9] = {cz, -sz, 0, sz, cz, 0, 0, 0, 1};
    const float ry[9] = {cy, 0, sy, 0, 1, 0, -sy, 0, cy};
    float zx[9]{}, m[9]{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            for (int k = 0; k < 3; ++k) {
                zx[i * 3 + j] += rz[i * 3 + k] * rx[k * 3 + j];
            }
        }
    }
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            for (int k = 0; k < 3; ++k) {
                m[i * 3 + j] += ry[i * 3 + k] * zx[k * 3 + j];
            }
        }
    }
    return {m[0] * s[0], m[1] * s[1], m[2] * s[2], t[0],        m[3] * s[0], m[4] * s[1],
            m[5] * s[2], t[1],        m[6] * s[0], m[7] * s[1], m[8] * s[2], t[2]};
}

std::array<float, 12> MatMul(const std::array<float, 12>& a, const std::array<float, 12>& b) {
    std::array<float, 12> m{};
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 4; ++j) {
            float v = j == 3 ? a[i * 4 + 3] : 0.0f;
            for (int k = 0; k < 3; ++k) {
                v += a[i * 4 + k] * b[k * 4 + j];
            }
            m[i * 4 + j] = v;
        }
    }
    return m;
}

// FLVER2 (Bloodborne: version 0x20014, little endian): every mesh's first full-detail face set,
// the vertices through their mesh's default bone (and its parents), and the first material's
// diffuse texture name (`texture`: its base name, lower case, without the extension).
bool DecodeFlver(const std::vector<u8>& d, Model& model, std::string& texture, std::string& why) {
    if (d.size() < 0x80 || std::memcmp(d.data(), "FLVER\0", 6) != 0 || d[6] != 'L') {
        why = "not a little-endian FLVER";
        return false;
    }
    u32 version{}, data_offset{};
    s32 dummies{}, materials{}, bones{}, meshes{}, buffers{};
    ReadAt(d, 0x08, version);
    ReadAt(d, 0x0C, data_offset);
    ReadAt(d, 0x14, dummies);
    ReadAt(d, 0x18, materials);
    ReadAt(d, 0x1C, bones);
    ReadAt(d, 0x20, meshes);
    ReadAt(d, 0x24, buffers);
    const u8 index_size = d[0x48];
    const bool unicode = d[0x49] != 0;
    s32 face_sets{}, layouts{}, textures{};
    ReadAt(d, 0x50, face_sets);
    ReadAt(d, 0x54, layouts);
    ReadAt(d, 0x58, textures);
    if (version < 0x20000 || dummies < 0 || materials < 1 || bones < 0 || meshes < 1 ||
        buffers < 1 || face_sets < 1 || layouts < 1 || textures < 0 || meshes > 64 ||
        bones > 1024 || buffers > 256 || face_sets > 1024 || layouts > 256 || textures > 1024) {
        why = fmt::format("FLVER {:#x} with counts {}/{}/{}/{}/{}", version, materials, bones,
                          meshes, buffers, face_sets);
        return false;
    }
    const float uv_factor = version >= 0x2000F ? 2048.0f : 1024.0f;
    const size_t material_at = 0x80 + 0x40 * static_cast<size_t>(dummies);
    const size_t bone_at = material_at + 0x20 * static_cast<size_t>(materials);
    const size_t mesh_at = bone_at + 0x80 * static_cast<size_t>(bones);
    const size_t face_at = mesh_at + 0x30 * static_cast<size_t>(meshes);
    const size_t buffer_at = face_at + 0x20 * static_cast<size_t>(face_sets);
    const size_t layout_at = buffer_at + 0x20 * static_cast<size_t>(buffers);
    const size_t texture_at = layout_at + 0x10 * static_cast<size_t>(layouts);
    if (texture_at + 0x20 * static_cast<size_t>(textures) > d.size()) {
        why = "truncated header";
        return false;
    }
    // Bones: their model-space transforms (parents precede children in these files, but follow the
    // chain to be safe).
    std::vector<std::array<float, 12>> local(static_cast<size_t>(bones));
    std::vector<s16> parent(static_cast<size_t>(bones), -1);
    for (s32 b = 0; b < bones; ++b) {
        const size_t at = bone_at + 0x80 * static_cast<size_t>(b);
        float t[3]{}, r[3]{}, s[3]{};
        for (int i = 0; i < 3; ++i) {
            ReadAt(d, at + 4 * i, t[i]);
            ReadAt(d, at + 0x10 + 4 * i, r[i]);
            ReadAt(d, at + 0x20 + 4 * i, s[i]);
        }
        ReadAt(d, at + 0x1C, parent[static_cast<size_t>(b)]);
        local[static_cast<size_t>(b)] = BoneMatrix(t, r, s);
    }
    const auto bone_world = [&](s32 b) {
        std::array<float, 12> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
        for (int hops = 0; b >= 0 && b < bones && hops < 64; ++hops) {
            m = MatMul(local[static_cast<size_t>(b)], m);
            b = parent[static_cast<size_t>(b)];
        }
        return m;
    };
    s32 first_material = -1;
    for (s32 mi = 0; mi < meshes; ++mi) {
        const size_t at = mesh_at + 0x30 * static_cast<size_t>(mi);
        s32 material{}, default_bone{}, fs_count{}, fs_offset{}, vb_count{}, vb_offset{};
        ReadAt(d, at + 0x04, material);
        ReadAt(d, at + 0x10, default_bone);
        ReadAt(d, at + 0x20, fs_count);
        ReadAt(d, at + 0x24, fs_offset);
        ReadAt(d, at + 0x28, vb_count);
        ReadAt(d, at + 0x2C, vb_offset);
        if (fs_count < 1 || vb_count < 1 || vb_count > 8 || material < 0 || material >= materials) {
            continue;
        }
        if (first_material < 0) {
            first_material = material;
        }
        const std::array<float, 12> m = bone_world(default_bone);
        // The vertex buffers of this mesh, merged by vertex index.
        const size_t first_vertex = model.vertices.size();
        u32 vertex_count = 0;
        for (s32 v = 0; v < vb_count; ++v) {
            s32 vb{};
            ReadAt(d, static_cast<size_t>(vb_offset) + 4 * static_cast<size_t>(v), vb);
            if (vb < 0 || vb >= buffers) {
                continue;
            }
            const size_t vat = buffer_at + 0x20 * static_cast<size_t>(vb);
            s32 layout{}, stride{}, count{}, offset{};
            ReadAt(d, vat + 0x04, layout);
            ReadAt(d, vat + 0x08, stride);
            ReadAt(d, vat + 0x0C, count);
            ReadAt(d, vat + 0x1C, offset);
            if (layout < 0 || layout >= layouts || stride <= 0 || count <= 0 || count > 200000 ||
                data_offset + static_cast<u64>(offset) + static_cast<u64>(stride) * count >
                    d.size()) {
                continue;
            }
            if (vertex_count == 0) {
                vertex_count = static_cast<u32>(count);
                model.vertices.resize(first_vertex + vertex_count,
                                      ModelVertex{{0, 0, 0}, {0, 1, 0}, {0, 0}});
            }
            const size_t lat = layout_at + 0x10 * static_cast<size_t>(layout);
            s32 members{}, members_at{};
            ReadAt(d, lat, members);
            ReadAt(d, lat + 0x0C, members_at);
            for (s32 k = 0; k < std::min(members, 32); ++k) {
                const size_t mat = static_cast<size_t>(members_at) + 0x14 * static_cast<size_t>(k);
                s32 member_offset{};
                u32 type{}, semantic{};
                s32 index{};
                ReadAt(d, mat + 0x04, member_offset);
                ReadAt(d, mat + 0x08, type);
                ReadAt(d, mat + 0x0C, semantic);
                ReadAt(d, mat + 0x10, index);
                if ((semantic != 0 && semantic != 3 && semantic != 5) || index != 0) {
                    continue; // position, normal, the first uv
                }
                for (u32 i = 0; i < std::min<u32>(vertex_count, static_cast<u32>(count)); ++i) {
                    const size_t src = data_offset + static_cast<size_t>(offset) +
                                       static_cast<size_t>(stride) * i +
                                       static_cast<size_t>(member_offset);
                    ModelVertex& out = model.vertices[first_vertex + i];
                    float f[4]{};
                    bool ok = true;
                    if (type == 0x02 || type == 0x03 || type == 0x01) {
                        const int n = type == 0x01 ? 2 : 3;
                        for (int c = 0; c < n; ++c) {
                            ReadAt(d, src + 4 * c, f[c]);
                        }
                    } else if (type == 0x10 || type == 0x11 || type == 0x13 || type == 0x2F) {
                        for (int c = 0; c < 4; ++c) {
                            const float b = src + c < d.size() ? d[src + c] : 127;
                            f[c] = (b - 127.0f) / 127.0f;
                        }
                    } else if (type == 0x15 || type == 0x16 || type == 0x12 || type == 0x1A) {
                        for (int c = 0; c < 4; ++c) {
                            s16 v{};
                            ReadAt(d, src + 2 * c, v);
                            f[c] = semantic == 5 ? v / uv_factor : v / 32767.0f;
                        }
                    } else {
                        ok = false;
                    }
                    if (!ok) {
                        continue;
                    }
                    if (semantic == 0) {
                        for (int r = 0; r < 3; ++r) {
                            out.position[r] = m[r * 4] * f[0] + m[r * 4 + 1] * f[1] +
                                              m[r * 4 + 2] * f[2] + m[r * 4 + 3];
                        }
                    } else if (semantic == 3) {
                        float n[3];
                        for (int r = 0; r < 3; ++r) {
                            n[r] = m[r * 4] * f[0] + m[r * 4 + 1] * f[1] + m[r * 4 + 2] * f[2];
                        }
                        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                        for (int r = 0; r < 3; ++r) {
                            out.normal[r] = l > 1e-6f ? n[r] / l : (r == 1 ? 1.0f : 0.0f);
                        }
                    } else {
                        out.uv[0] = f[0];
                        out.uv[1] = f[1];
                    }
                }
            }
        }
        if (vertex_count == 0) {
            continue;
        }
        // The first face set without LOD (0x03000000) or motion-blur / edge-compression bits.
        for (s32 f = 0; f < fs_count; ++f) {
            s32 fs{};
            ReadAt(d, static_cast<size_t>(fs_offset) + 4 * static_cast<size_t>(f), fs);
            if (fs < 0 || fs >= face_sets) {
                continue;
            }
            const size_t fat = face_at + 0x20 * static_cast<size_t>(fs);
            u32 flags{};
            s32 count{}, offset{}, size{};
            ReadAt(d, fat, flags);
            const bool strip = d[fat + 4] != 0;
            ReadAt(d, fat + 0x08, count);
            ReadAt(d, fat + 0x0C, offset);
            ReadAt(d, fat + 0x18, size);
            const u32 isize = size != 0 ? static_cast<u32>(size) : index_size;
            if ((flags & 0xC3000000u) != 0 || count < 3 || (isize != 16 && isize != 32) ||
                data_offset + static_cast<u64>(offset) + static_cast<u64>(count) * (isize / 8) >
                    d.size()) {
                continue;
            }
            const auto index = [&](s32 i) -> u32 {
                const size_t at = data_offset + static_cast<size_t>(offset) +
                                  static_cast<size_t>(i) * (isize / 8);
                if (isize == 16) {
                    u16 v{};
                    ReadAt(d, at, v);
                    return v == 0xFFFF ? 0xFFFFFFFFu : v;
                }
                u32 v{};
                ReadAt(d, at, v);
                return v;
            };
            const auto add = [&](u32 a, u32 b, u32 c) {
                if (a >= vertex_count || b >= vertex_count || c >= vertex_count || a == b ||
                    b == c || a == c) {
                    return;
                }
                model.indices.push_back(static_cast<u32>(first_vertex) + a);
                model.indices.push_back(static_cast<u32>(first_vertex) + b);
                model.indices.push_back(static_cast<u32>(first_vertex) + c);
            };
            if (strip) {
                bool flip = false;
                for (s32 i = 0; i + 2 < count; ++i) {
                    const u32 a = index(i), b = index(i + 1), c = index(i + 2);
                    if (a == 0xFFFFFFFFu || b == 0xFFFFFFFFu || c == 0xFFFFFFFFu) {
                        flip = false;
                        continue;
                    }
                    flip ? add(a, c, b) : add(a, b, c);
                    flip = !flip;
                }
            } else {
                for (s32 i = 0; i + 2 < count; i += 3) {
                    add(index(i), index(i + 1), index(i + 2));
                }
            }
            break;
        }
    }
    if (model.vertices.empty() || model.indices.empty()) {
        why = "no triangles";
        return false;
    }
    for (int c = 0; c < 3; ++c) {
        model.min[c] = 1e9f;
        model.max[c] = -1e9f;
    }
    for (const ModelVertex& v : model.vertices) {
        for (int c = 0; c < 3; ++c) {
            model.min[c] = std::min(model.min[c], v.position[c]);
            model.max[c] = std::max(model.max[c], v.position[c]);
        }
    }
    // The first material's diffuse texture.
    texture.clear();
    s32 tex_count{}, tex_first{};
    const size_t mat_at = material_at + 0x20 * static_cast<size_t>(first_material);
    ReadAt(d, mat_at + 0x08, tex_count);
    ReadAt(d, mat_at + 0x0C, tex_first);
    for (s32 t = tex_first; t >= 0 && t < tex_first + tex_count && t < textures; ++t) {
        const size_t tat = texture_at + 0x20 * static_cast<size_t>(t);
        u32 path{}, type{};
        ReadAt(d, tat, path);
        ReadAt(d, tat + 4, type);
        if (FlverString(d, type, unicode).find("Diffuse") == std::string::npos) {
            continue;
        }
        std::string name = FlverString(d, path, unicode);
        if (const size_t slash = name.find_last_of("\\/"); slash != std::string::npos) {
            name = name.substr(slash + 1);
        }
        if (const size_t dot = name.find_last_of('.'); dot != std::string::npos) {
            name = name.substr(0, dot);
        }
        for (char& c : name) {
            c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        }
        texture = name;
        break;
    }
    return true;
}

// The game's font (Scaleform DefineFont3): core/vr/game_font.h.
using namespace VR::GameText;

struct Font {
    ImFontAtlas atlas;
    ImFont* font{nullptr};
    unsigned char* pixels{nullptr};
    int width{0};
    int height{0};
};

struct Loader {
    std::mutex mutex;
    std::condition_variable cv;
    bool started{false};
    std::unordered_map<s32, std::string> names;
    std::unordered_map<s32, std::shared_ptr<const Icon>> icons;
    std::unordered_map<s32, bool> icon_failed;
    std::deque<s32> icon_queue;
    std::unordered_map<s32, std::shared_ptr<const Model>> models;
    std::unordered_map<s32, bool> model_failed;
    std::deque<s32> model_queue;
    // menu/common.tpf.dcx textures (the VR settings page): decoded ones, the names asked for and
    // not yet loaded, and the names the file lacks.
    std::unordered_map<std::string, std::shared_ptr<const MenuTexture>> menu_textures;
    std::vector<std::string> menu_queue;
    std::unordered_map<std::string, bool> menu_failed;
    std::atomic<bool> names_ready{false};
    std::atomic<Font*> font{nullptr};
    std::atomic<GameFont*> game_font{nullptr};
    // The VR pages' font (their text is English): engus's when the console language's font is
    // another file (Korean, Chinese: their Latin glyphs are smaller and Korean has no "·" / "—"),
    // else the same as game_font. Set together with it.
    std::atomic<GameFont*> page_font{nullptr};
    std::vector<ImWchar> ranges;
};

Loader& L() {
    static Loader* loader = new Loader; // never destroyed: its thread may outlive statics
    return *loader;
}

void LoadNames(Loader& l) {
    const int language = EmulatorSettings.GetConsoleLanguage();
    std::optional<std::vector<u8>> file;
    std::string folder = LanguageFolder(language);
    for (const std::string& f : {folder, std::string{"engus"}}) {
        file = ReadGameFile("/app0/dvdroot_ps4/msg/" + f + "/item.msgbnd.dcx");
        if (file) {
            folder = f;
            break;
        }
    }
    std::unordered_map<s32, std::string> names;
    if (file) {
        if (auto bnd = Dcx(*file)) {
            if (auto fmg = Bnd4File(*bnd, 10)) {
                names = Fmg(*fmg);
            }
        }
    }
    LOG_INFO(Core, "VR assets: {} goods names ({}){}", names.size(), folder,
             names.empty() ? " - none: the arc shows no names" : "");
    std::scoped_lock lock{l.mutex};
    l.names = std::move(names);
    l.names_ready.store(true, std::memory_order_release);
}

void BuildFont(Loader& l) {
    const int language = EmulatorSettings.GetConsoleLanguage();
    const bool cjk = language == 0 || language == 9 || language == 10 || language == 11;
    auto* font = new Font;
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(font->atlas.GetGlyphRangesDefault());
    {
        std::scoped_lock lock{l.mutex};
        for (const auto& [id, name] : l.names) {
            builder.AddText(name.c_str());
        }
    }
    ImVector<ImWchar> ranges;
    builder.BuildRanges(&ranges);
    l.ranges.assign(ranges.begin(), ranges.end());
    ImFontConfig config{};
    config.OversampleH = 2;
    config.OversampleV = 2;
    config.PixelSnapH = false;
    config.FontDataOwnedByAtlas = true;
    font->atlas.Flags |= ImFontAtlasFlags_NoMouseCursors | ImFontAtlasFlags_NoBakedLines;
    font->font = font->atlas.AddFontFromMemoryCompressedTTF(
        imgui_font_notosans_regular_compressed_data,
        static_cast<int>(imgui_font_notosans_regular_compressed_size), FontPx, &config,
        l.ranges.data());
    if (cjk && font->font != nullptr) {
        ImFontConfig merge = config;
        merge.MergeMode = true;
        merge.FontNo = language == 0 ? 0 : language == 9 ? 1 : language == 11 ? 2 : 3;
        font->atlas.AddFontFromMemoryCompressedTTF(
            imgui_font_notosanscjk_regular_compressed_data,
            static_cast<int>(imgui_font_notosanscjk_regular_compressed_size), FontPx, &merge,
            l.ranges.data());
    }
    int bpp = 0;
    font->atlas.GetTexDataAsAlpha8(&font->pixels, &font->width, &font->height, &bpp);
    if (font->font == nullptr || font->pixels == nullptr || bpp != 1) {
        LOG_WARNING(Core, "VR assets: the label font could not be built; no labels");
        return;
    }
    LOG_INFO(Core, "VR assets: label font {} glyphs, atlas {}x{}{}", font->font->Glyphs.Size,
             font->width, font->height, cjk ? " (CJK)" : "");
    l.font.store(font, std::memory_order_release);
}

// The game's own font for labels (the console language's menu folder, else engus).
void LoadGameFont(Loader& l) {
    const auto start = std::chrono::steady_clock::now();
    const int language = EmulatorSettings.GetConsoleLanguage();
    std::string why = "missing";
    for (const std::string folder : {std::string{LanguageFolder(language)}, std::string{"engus"}}) {
        const auto file = ReadGameFile("/app0/dvdroot_ps4/menu/" + folder + "/font.gfx");
        if (!file) {
            continue;
        }
        if (auto font = ParseGameFont(*file, why)) {
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                                    std::chrono::steady_clock::now() - start)
                                    .count();
            LOG_INFO(Core, "VR assets: the game's font for labels: {} glyphs ({}, {}) in {} us",
                     font->offsets.size(), folder, font->glyph.contains('A') ? "latin" : "no latin",
                     micros);
            // The VR pages (vr_menu=1; English text): the English menu font when this
            // language's is another file (most languages share engus's byte for byte).
            GameFont* page = font.get();
            if (folder != "engus" && VR::GetSettings().vr_menu != 0) {
                const auto english = ReadGameFile("/app0/dvdroot_ps4/menu/engus/font.gfx");
                std::string page_why;
                if (english && *english != *file) {
                    if (auto latin = ParseGameFont(*english, page_why)) {
                        page = latin.release();
                        LOG_INFO(Core, "VR assets: the VR pages' English in the engus font ({} "
                                       "glyphs; {}'s is another font)",
                                 page->offsets.size(), folder);
                    }
                }
            }
            l.page_font.store(page, std::memory_order_release);
            l.game_font.store(font.release(), std::memory_order_release);
            return;
        }
    }
    LOG_WARNING(Core, "VR assets: no game font ({}); labels use Noto Sans", why);
}

// Replace only s11012's surface atlas; geometry and existing GPU material path stay intact.
bool RegalMusicBox(Model& model) {
    namespace MB = VR::MusicBox;
    const auto file=cmrc::res::get_filesystem().open("src/resources/vr/music_box_regal.dds");
    if (!MB::RegalDdsValid({file.begin(),file.size()})) return false;
    if (!MB::ApplyRegalUVs(model.vertices,model.indices)) return false;
    ModelTexture texture{};
    texture.format=ModelTexture::Format::Bc1;
    texture.width=texture.height=MB::RegalSize;
    texture.levels=MB::RegalLevels;
    u32 offset=0;
    for(u32 level=0;level<texture.levels;level++) {
        texture.level_offset.push_back(offset);
        const u32 blocks=std::max(1u,(texture.width>>level)/4);
        offset+=blocks*blocks*8;
    }
    texture.blocks.assign(file.begin()+128,file.end());
    model.diffuse=std::move(texture);
    return true;
}

// Every model of the held-item table plus `wanted`, from one read of the sfx archive.
void LoadModels(Loader& l, s32 wanted) {
    const auto start = std::chrono::steady_clock::now();
    std::vector<s32> ids(HeldModelIds.begin(), HeldModelIds.end());
    if (std::find(ids.begin(), ids.end(), wanted) == ids.end()) {
        ids.push_back(wanted);
    }
    {
        std::scoped_lock lock{l.mutex};
        std::erase_if(ids, [&l, wanted](s32 id) {
            return id != wanted && (l.models.contains(id) || l.model_failed.contains(id));
        });
    }
    std::unordered_map<s32, std::shared_ptr<Model>> loaded;
    std::string why = "the archive is missing";
    for (const char* archive : {SfxArchive, "/app0/dvdroot_ps4/sfx/frpg_sfxbnd_dlceffects.ffxbnd.dcx"}) {
    if (auto file = ReadGameFile(archive)) {
        if (auto bnd = Dcx(*file)) {
            file.reset();
            std::vector<std::string> names;
            for (const s32 id : ids) {
                if (loaded.contains(id)) continue;
                names.push_back(fmt::format("s{:05}.flver", id));
            }
            auto flvers = Bnd4FilesNamed(*bnd, names);
            std::vector<std::string> texture_names;
            std::unordered_map<s32, std::string> texture_of;
            for (const s32 id : ids) {
                const auto it = flvers.find(fmt::format("s{:05}.flver", id));
                if (it == flvers.end()) {
                    continue;
                }
                auto model = std::make_shared<Model>();
                model->id = id;
                std::string texture, model_why;
                if (!DecodeFlver(it->second, *model, texture, model_why)) {
                    LOG_WARNING(Core, "VR assets: model s{:05}: {}", id, model_why);
                    continue;
                }
                if (id == 11012 && !VR::MusicBox::RemoveOriginalCrank(model->vertices, model->indices)) {
                    LOG_WARNING(Core, "VR assets: s11012 crank layout unrecognized; using fallback box");
                    continue;
                }
                if (!texture.empty()) {
                    texture_names.push_back(texture + ".tpf");
                    texture_of[id] = texture + ".tpf";
                }
                loaded[id] = model;
            }
            const auto tpfs = Bnd4FilesNamed(*bnd, texture_names);
            for (auto& [id, model] : loaded) {
                if (!texture_of.contains(id)) continue;
                Model* const m = model.get();
                const auto name = texture_of.find(id);
                const auto tpf = name != texture_of.end() ? tpfs.find(name->second) : tpfs.end();
                std::string texture_why = "no diffuse texture";
                if (tpf == tpfs.end() || !DecodeTexture(tpf->second, m->diffuse, texture_why)) {
                    m->diffuse = {};
                    LOG_WARNING(Core, "VR assets: model s{:05}: {} ({})", id, texture_why,
                                name != texture_of.end() ? name->second : "-");
                }
            }
            why.clear();
        } else {
            why = "not a DCX";
        }
    }
        if (loaded.size() == ids.size()) break;
    }
    if (const auto it=loaded.find(11012); it!=loaded.end()) {
        if (!RegalMusicBox(*it->second))
            LOG_WARNING(Core, "VR assets: music box regal atlas invalid; keeping native material");
    }
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
    std::scoped_lock lock{l.mutex};
    for (const s32 id : ids) {
        const auto it = loaded.find(id);
        if (it == loaded.end()) {
            l.model_failed[id] = true;
            continue;
        }
        const Model& m = *it->second;
        LOG_INFO(Core,
                 "VR assets: model s{:05}: {} vertices, {} triangles, {:.3f} x {:.3f} x {:.3f} m, "
                 "texture {}x{} {} levels {}",
                 id, m.vertices.size(), m.indices.size() / 3, m.max[0] - m.min[0],
                 m.max[1] - m.min[1], m.max[2] - m.min[2], m.diffuse.width, m.diffuse.height,
                 m.diffuse.levels,
                 m.diffuse.format == ModelTexture::Format::Bc1   ? "BC1"
                 : m.diffuse.format == ModelTexture::Format::Bc3 ? "BC3"
                 : m.diffuse.format == ModelTexture::Format::Bc7 ? "BC7"
                                                                 : "none");
        l.models[id] = it->second;
    }
    LOG_INFO(Core, "VR assets: {} of {} held-item models in {} ms{}", loaded.size(), ids.size(),
             micros / 1000, why.empty() ? "" : " (" + why + ")");
}

// The asked-for textures of menu/common.tpf.dcx (PS4 TPF: BC7, micro-tiled), decoded to RGBA8 from
// one read of the file (9 MB, 31 MB unpacked, dropped afterwards).
void LoadMenuTextures(Loader& l, std::vector<std::string> names) {
    const auto start = std::chrono::steady_clock::now();
    std::unordered_map<std::string, std::shared_ptr<const MenuTexture>> loaded;
    std::string why = "the file is missing";
    if (auto file = ReadGameFile("/app0/dvdroot_ps4/menu/common.tpf.dcx")) {
        if (auto tpf = Dcx(*file)) {
            file.reset();
            why.clear();
            s32 count{};
            const std::vector<u8>& d = *tpf;
            if (d.size() < 0x10 || std::memcmp(d.data(), "TPF\0", 4) != 0 || !ReadAt(d, 8, count) ||
                d[0x0C] != 4) {
                why = "not a PS4 TPF";
                count = 0;
            }
            for (s32 i = 0; i < count; ++i) {
                const size_t e = 0x10 + static_cast<size_t>(i) * 0x24;
                u32 offset{}, name_offset{};
                s32 size{};
                u16 w{}, h{};
                if (!ReadAt(d, e, offset) || !ReadAt(d, e + 4, size) || !ReadAt(d, e + 12, w) ||
                    !ReadAt(d, e + 14, h) || !ReadAt(d, e + 0x18, name_offset)) {
                    break;
                }
                std::string name;
                for (size_t c = name_offset; c + 1 < d.size() && (d[c] != 0 || d[c + 1] != 0);
                     c += 2) {
                    name += static_cast<char>(d[c]);
                }
                if (std::find(names.begin(), names.end(), name) == names.end()) {
                    continue;
                }
                const u8 format = d[e + 8];
                if (format != 102 && format != 106 && format != 107) {
                    LOG_WARNING(Core, "VR assets: menu texture {}: format {} is not BC7", name,
                                format);
                    continue;
                }
                const u32 bw = (w + 3u) / 4, bh = (h + 3u) / 4;
                std::vector<u8> blocks;
                if (size <= 0 || static_cast<u64>(offset) + static_cast<u64>(size) > d.size() ||
                    !DetileLevel(d.data() + offset, static_cast<size_t>(size), bw, bh, 16,
                                 blocks)) {
                    LOG_WARNING(Core, "VR assets: menu texture {} {}x{} does not fit its {} bytes",
                                name, w, h, size);
                    continue;
                }
                auto texture = std::make_shared<MenuTexture>();
                texture->width = bw * 4;
                texture->height = bh * 4;
                texture->rgba.resize(static_cast<size_t>(texture->width) * texture->height * 4);
                Bc7::DecodeImage(blocks.data(), static_cast<int>(bw), static_cast<int>(bh),
                                 texture->rgba.data());
                loaded[name] = std::move(texture);
            }
        } else {
            why = "not a DCX";
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    std::scoped_lock lock{l.mutex};
    for (const std::string& name : names) {
        const auto it = loaded.find(name);
        if (it == loaded.end()) {
            l.menu_failed[name] = true;
            LOG_WARNING(Core, "VR assets: menu texture {} not loaded{}", name,
                        why.empty() ? std::string{} : " (" + why + ")");
            continue;
        }
        LOG_INFO(Core, "VR assets: menu texture {} {}x{} (BC7 decoded)", name, it->second->width,
                 it->second->height);
        l.menu_textures[name] = it->second;
    }
    LOG_INFO(Core, "VR assets: {} of {} menu textures from common.tpf.dcx in {} ms", loaded.size(),
             names.size(), ms);
}

// One job of this thread. One that runs out of memory (std::bad_alloc on a PC short of RAM and page
// file) or meets an unreadable file is skipped: the VR UI does without that asset, and the thread,
// and the game, keep going. Before 2026-10-17 such a throw ended the game (exit 0xe06d7363).
template <class Job> bool Guarded(const char* what, Job&& job) {
    try {
        job();
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR(Core, "VR assets: {} skipped ({})", what, e.what());
    } catch (...) {
        LOG_ERROR(Core, "VR assets: {} skipped", what);
    }
    return false;
}

void Run() {
    Common::SetCurrentThreadName("VR:Assets");
    auto& l = L();
    Guarded("goods names", [&] { LoadNames(l); });
    Guarded("the VR font", [&] { BuildFont(l); });
    Guarded("the game font", [&] { LoadGameFont(l); });
    for (;;) {
        s32 id = -1;
        s32 model = -1;
        std::vector<std::string> menu;
        {
            std::unique_lock lock{l.mutex};
            l.cv.wait(lock, [&l] {
                return !l.icon_queue.empty() || !l.model_queue.empty() || !l.menu_queue.empty();
            });
            if (!l.menu_queue.empty()) {
                menu = l.menu_queue; // kept queued (asked again = no new request) until loaded
            } else if (!l.model_queue.empty()) {
                model = l.model_queue.front();
                l.model_queue.pop_front();
            } else {
                id = l.icon_queue.front();
                l.icon_queue.pop_front();
            }
        }
        if (!menu.empty()) {
            const bool ok = Guarded("menu textures", [&] { LoadMenuTextures(l, menu); });
            std::scoped_lock lock{l.mutex};
            std::erase_if(l.menu_queue, [&menu](const std::string& name) {
                return std::find(menu.begin(), menu.end(), name) != menu.end();
            });
            if (!ok) {
                for (const std::string& name : menu) {
                    if (!l.menu_textures.contains(name)) {
                        l.menu_failed[name] = true; // not asked for again
                    }
                }
            }
            continue;
        }
        if (model >= 0) {
            if (!Guarded("a model", [&] { LoadModels(l, model); })) {
                std::scoped_lock lock{l.mutex};
                if (!l.models.contains(model)) {
                    l.model_failed[model] = true; // not asked for again
                }
            }
            continue;
        }
        const auto start = std::chrono::steady_clock::now();
        const std::string path =
            fmt::format("/app0/dvdroot_ps4/menu/menu_knowledge_{:05}.tpf.dcx", id);
        std::shared_ptr<Icon> icon;
        std::string why = "missing";
        if (!Guarded("an icon", [&] {
                if (auto file = ReadGameFile(path)) {
                    if (auto tpf = Dcx(*file)) {
                        icon = DecodeIcon(*tpf, why);
                    } else {
                        why = "not a DCX";
                    }
                }
            })) {
            icon.reset();
            why = "out of memory or unreadable";
        }
        const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - start)
                                .count();
        std::scoped_lock lock{l.mutex};
        if (icon) {
            LOG_INFO(Core, "VR assets: icon {} {}x{} BC7 ({} us)", id, icon->width, icon->height,
                     micros);
            l.icons[id] = icon;
        } else {
            LOG_WARNING(Core, "VR assets: icon {} ({}): {}", id, path, why);
            l.icon_failed[id] = true;
        }
    }
}

} // Anonymous namespace

void Start() {
    auto& l = L();
    std::scoped_lock lock{l.mutex};
    if (l.started) {
        return;
    }
    l.started = true;
    std::thread(Run).detach();
}

std::string GoodsName(s32 goods) {
    auto& l = L();
    if (!l.names_ready.load(std::memory_order_acquire)) {
        Start();
        return {};
    }
    std::scoped_lock lock{l.mutex};
    const auto it = l.names.find(goods);
    return it != l.names.end() ? it->second : std::string{};
}

std::shared_ptr<const Icon> GetIcon(s32 icon_id) {
    if (icon_id < 0) {
        return nullptr;
    }
    Start();
    auto& l = L();
    {
        std::scoped_lock lock{l.mutex};
        if (const auto it = l.icons.find(icon_id); it != l.icons.end()) {
            return it->second;
        }
        if (l.icon_failed.contains(icon_id) ||
            std::find(l.icon_queue.begin(), l.icon_queue.end(), icon_id) != l.icon_queue.end()) {
            return nullptr;
        }
        l.icon_queue.push_back(icon_id);
    }
    l.cv.notify_one();
    return nullptr;
}

std::shared_ptr<const Model> GetModel(s32 model_id) {
    if (model_id <= 0) {
        return nullptr;
    }
    Start();
    auto& l = L();
    {
        std::scoped_lock lock{l.mutex};
        if (const auto it = l.models.find(model_id); it != l.models.end()) {
            return it->second;
        }
        if (l.model_failed.contains(model_id) ||
            std::find(l.model_queue.begin(), l.model_queue.end(), model_id) !=
                l.model_queue.end()) {
            return nullptr;
        }
        l.model_queue.push_back(model_id);
    }
    l.cv.notify_one();
    return nullptr;
}

void RequestMenuTextures(const std::vector<std::string>& names) {
    Start();
    auto& l = L();
    bool added = false;
    {
        std::scoped_lock lock{l.mutex};
        for (const std::string& name : names) {
            if (l.menu_textures.contains(name) || l.menu_failed.contains(name) ||
                std::find(l.menu_queue.begin(), l.menu_queue.end(), name) != l.menu_queue.end()) {
                continue;
            }
            l.menu_queue.push_back(name);
            added = true;
        }
    }
    if (added) {
        l.cv.notify_one();
    }
}

std::shared_ptr<const MenuTexture> GetMenuTexture(const std::string& name) {
    auto& l = L();
    {
        std::scoped_lock lock{l.mutex};
        if (const auto it = l.menu_textures.find(name); it != l.menu_textures.end()) {
            return it->second;
        }
    }
    RequestMenuTextures({name});
    return nullptr;
}

const GameText::GameFont* GetGameFont() {
    const GameFont* const font = L().game_font.load(std::memory_order_acquire);
    if (font == nullptr) {
        Start();
    }
    return font;
}

const GameText::GameFont* GetPageFont() {
    const GameFont* const font = L().page_font.load(std::memory_order_acquire);
    if (font == nullptr) {
        Start();
    }
    return font;
}

namespace {

// The label in the game's font: false when a character has no glyph there.
bool RasterizeGameFont(const GameFont& font, const char* utf8, std::vector<u8>& fill, u32& width,
                       u32& height) {
    const float scale = FontPx / FontEm;
    std::vector<std::pair<u32, float>> glyphs; // glyph, pen (pixels)
    float pen = 0.0f;
    for (const char* s = utf8; *s != 0;) {
        const u32 c = NextUtf8(s);
        if (c == 0) {
            break;
        }
        const auto it = font.glyph.find(c);
        if (it == font.glyph.end()) {
            return false;
        }
        glyphs.emplace_back(it->second, pen);
        pen += font.advance[it->second] * scale;
    }
    if (glyphs.empty()) {
        return false;
    }
    width = std::min<u32>(static_cast<u32>(std::ceil(pen)) + 2 * PadPx, 2048);
    height = static_cast<u32>(std::ceil((font.ascent + font.descent) * scale)) + 2 * PadPx;
    const float baseline = PadPx + font.ascent * scale;
    std::vector<Segment> segments;
    for (const auto& [glyph, x] : glyphs) {
        if (!GlyphSegments(font, glyph, scale, PadPx + x, baseline, segments)) {
            return false;
        }
    }
    std::vector<float> cover;
    FillOutlines(segments, width, height, cover);
    fill.resize(cover.size());
    for (size_t i = 0; i < cover.size(); ++i) {
        fill[i] = static_cast<u8>(std::clamp(cover[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    return true;
}

// The label in Noto Sans (the emulator's embedded font; CJK merged for those languages).
bool RasterizeNoto(const Font* f, const char* utf8, std::vector<u8>& fill, u32& out_width,
                   u32& out_height) {
    // Lay the glyphs out on one line.
    struct Placed {
        const ImFontGlyph* g;
        float x;
    };
    std::vector<Placed> glyphs;
    float pen = 0.0f;
    const char* s = utf8;
    while (*s != 0) {
        const u32 c = NextUtf8(s);
        if (c == 0) {
            break;
        }
        const ImFontGlyph* g = f->font->FindGlyph(static_cast<ImWchar>(c));
        if (g == nullptr) {
            continue;
        }
        glyphs.push_back({g, pen});
        pen += g->AdvanceX;
    }
    if (glyphs.empty()) {
        return false;
    }
    const int line = static_cast<int>(std::ceil(f->font->FontSize));
    const u32 width = static_cast<u32>(std::ceil(pen)) + 2 * PadPx;
    const u32 height = static_cast<u32>(line) + 2 * PadPx;
    out_width = std::min<u32>(width, 2048);
    out_height = height;
    fill.assign(static_cast<size_t>(out_width) * out_height, 0);
    for (const Placed& p : glyphs) {
        const ImFontGlyph& g = *p.g;
        if (!g.Visible) {
            continue;
        }
        const float gx0 = p.x + g.X0, gy0 = g.Y0, gx1 = p.x + g.X1, gy1 = g.Y1;
        const int x0 = static_cast<int>(std::floor(gx0)), x1 = static_cast<int>(std::ceil(gx1));
        const int y0 = static_cast<int>(std::floor(gy0)), y1 = static_cast<int>(std::ceil(gy1));
        for (int y = y0; y < y1; ++y) {
            for (int x = x0; x < x1; ++x) {
                const int ox = x + PadPx, oy = y + PadPx;
                if (ox < 0 || oy < 0 || ox >= static_cast<int>(out_width) ||
                    oy >= static_cast<int>(out_height)) {
                    continue;
                }
                // Bilinear sample of the atlas at this texel's centre.
                const float u = g.U0 + (g.U1 - g.U0) * ((x + 0.5f - gx0) / (gx1 - gx0));
                const float v = g.V0 + (g.V1 - g.V0) * ((y + 0.5f - gy0) / (gy1 - gy0));
                const float fx = u * f->width - 0.5f, fy = v * f->height - 0.5f;
                const int ix = static_cast<int>(std::floor(fx)),
                          iy = static_cast<int>(std::floor(fy));
                const float ax = fx - ix, ay = fy - iy;
                const auto texel = [f](int tx, int ty) -> float {
                    tx = std::clamp(tx, 0, f->width - 1);
                    ty = std::clamp(ty, 0, f->height - 1);
                    return f->pixels[static_cast<size_t>(ty) * f->width + tx];
                };
                const float a = (texel(ix, iy) * (1 - ax) + texel(ix + 1, iy) * ax) * (1 - ay) +
                                (texel(ix, iy + 1) * (1 - ax) + texel(ix + 1, iy + 1) * ax) * ay;
                u8& dst = fill[static_cast<size_t>(oy) * out_width + ox];
                dst = std::max(dst, static_cast<u8>(std::clamp(a, 0.0f, 255.0f)));
            }
        }
    }
    return true;
}

} // Anonymous namespace

bool RasterizeLabel(const char* utf8, u32 px, LabelBitmap& out) {
    if (utf8 == nullptr || utf8[0] == 0) {
        return false;
    }
    (void)px; // one size (FontPx); the frame scales it
    const GameFont* const game = L().game_font.load(std::memory_order_acquire);
    Font* const f = L().font.load(std::memory_order_acquire);
    std::vector<u8> fill;
    if (game == nullptr || !RasterizeGameFont(*game, utf8, fill, out.width, out.height)) {
        if (f == nullptr) {
            Start(); // the fonts load in the background
            return false;
        }
        if (!RasterizeNoto(f, utf8, fill, out.width, out.height)) {
            return false;
        }
    }
    // The halo: the fill blurred over OutlinePx texels each way (two box passes, running sums:
    // linear in the label's size, it runs on the GPU thread) and lifted, a soft shadow behind the
    // letters, never a hard outline.
    constexpr int R = OutlinePx;
    const int w = static_cast<int>(out.width), h = static_cast<int>(out.height);
    std::vector<u16> across(static_cast<size_t>(w) * h, 0);
    for (int y = 0; y < h; ++y) {
        const u8* const row = fill.data() + static_cast<size_t>(y) * w;
        int sum = 0;
        for (int x = -R; x < w + R; ++x) {
            if (x + R < w) {
                sum += row[x + R];
            }
            if (x - R - 1 >= 0) {
                sum -= row[x - R - 1];
            }
            if (x >= 0 && x < w) {
                across[static_cast<size_t>(y) * w + x] = static_cast<u16>(sum);
            }
        }
    }
    out.rg.assign(static_cast<size_t>(w) * h * 2, 0);
    constexpr float Lift = 2.4f / static_cast<float>((2 * R + 1) * (2 * R + 1));
    for (int x = 0; x < w; ++x) {
        int sum = 0;
        for (int y = -R; y < h + R; ++y) {
            if (y + R < h) {
                sum += across[static_cast<size_t>(y + R) * w + x];
            }
            if (y - R - 1 >= 0) {
                sum -= across[static_cast<size_t>(y - R - 1) * w + x];
            }
            if (y >= 0 && y < h) {
                const size_t i = (static_cast<size_t>(y) * w + x) * 2;
                const u8 f = fill[static_cast<size_t>(y) * w + x];
                out.rg[i] = f;
                out.rg[i + 1] = static_cast<u8>(std::clamp(
                    std::max(static_cast<float>(sum) * Lift, static_cast<float>(f)), 0.0f, 255.0f));
            }
        }
    }
    return true;
}

} // namespace VR::Assets
