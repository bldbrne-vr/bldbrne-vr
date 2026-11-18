// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "core/vr/cigarette.h"

// Kos Lights (goods 9000) built in memory from the player's own game files, the ones the game
// really loads (a -mods or patch copy included): the row in EquipParamGoods and the texts in
// item.msgbnd, as scripts/build_cigarette_patch.py builds them but for the icon, one the game
// ships (core/vr/cigarette.h). A mod's own copy of these files no longer hides the item
// (2026-11-04: a -mods gameparam did, and the game had no goods 9000). Header-only, no emulator
// dependencies; zlib is the caller's (core/vr/bloodborne_kos_lights.cpp; tests/test_kos_lights.cpp).
namespace VR::KosLights {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using u64 = std::uint64_t;

inline constexpr s32 Goods = VR::Cigarette::Goods;
inline constexpr s32 Icon = VR::Cigarette::Icon;
// The reusable, equippable Monocular layout the row is copied from.
inline constexpr s32 TemplateRow = 1411;
inline constexpr std::size_t RowSize = 108; // EQUIP_PARAM_GOODS_ST
// item.msgbnd: goods names (10), the inventory list's line (20) and the long description (24); the
// Monocular's are "Monocular", "Monocular used to see things up close." and its story. (24 and 27,
// as the first script had them, showed "?GoodsInfo?" in the mock's inventory, 2026-11-04.)
inline constexpr s32 NamesFmg = 10, InfosFmg = 20, CaptionsFmg = 24;
inline constexpr std::u16string_view Name = u"Kos Lights";
inline constexpr std::u16string_view Info = u"An inexhaustible packet of yellow-paper cigarettes.";
inline constexpr std::u16string_view Caption =
    u"A finely rolled tobacco, once prescribed to Gehrman by Yharnam's Surgeon General, effects "
    u"unknown but believed to be positive\n\n\"A packet a day keeps the lungs youthful and the heart "
    u"vigorous\"\n\nThe Surgeon General died of a mysterious respiratory illness shortly thereafter.";

template <typename T>
inline bool ReadAt(std::span<const u8> d, std::size_t at, T& out) {
    if (at > d.size() || d.size() - at < sizeof(T)) {
        return false;
    }
    std::memcpy(&out, d.data() + at, sizeof(T));
    return true;
}
template <typename T>
inline void PutAt(std::vector<u8>& d, std::size_t at, T value) {
    std::memcpy(d.data() + at, &value, sizeof(T));
}

// ---- PARAM (Bloodborne: little endian, 64-bit row offsets; rows of 24 bytes after a 64-byte
// header: id, 4 pad, data offset, name offset) ----------------------------------------------------

struct Row {
    s32 id{};
    u64 data{};
    u64 name{};
};

// The row table, or nullopt (with `why`) when it is not one this code understands.
inline std::optional<std::vector<Row>> ParamRows(std::span<const u8> p, std::string& why) {
    u16 count{};
    if (p.size() < 64 || p[44] != 0 || p[45] != 4 || !ReadAt(p, 10, count)) {
        why = "not a 64-bit little-endian PARAM";
        return std::nullopt;
    }
    const std::size_t end = 64 + std::size_t{count} * 24;
    if (count == 0 || count >= 32768 || end > p.size()) {
        why = "an invalid PARAM row table";
        return std::nullopt;
    }
    std::vector<Row> rows(count);
    for (std::size_t i = 0; i < count; ++i) {
        const std::size_t at = 64 + i * 24;
        ReadAt(p, at, rows[i].id);
        ReadAt(p, at + 8, rows[i].data);
        ReadAt(p, at + 16, rows[i].name);
        if (rows[i].data < end || rows[i].data >= p.size() || rows[i].name >= p.size()) {
            why = "PARAM row offsets out of range";
            return std::nullopt;
        }
    }
    // A mod's table may be out of id order (the user's -mods gameparam is); a duplicate id is
    // ambiguous: refused, as the script does.
    std::vector<s32> ids(count);
    for (std::size_t i = 0; i < count; ++i) {
        ids[i] = rows[i].id;
    }
    std::sort(ids.begin(), ids.end());
    if (std::adjacent_find(ids.begin(), ids.end()) != ids.end()) {
        why = "duplicate PARAM row ids";
        return std::nullopt;
    }
    return rows;
}

inline bool HasRow(const std::vector<Row>& rows, s32 id) {
    for (const auto& r : rows) {
        if (r.id == id) {
            return true;
        }
    }
    return false;
}

// Kos Lights' 108 bytes: the template row with the script's fields set (reference -1, category 0,
// no effect variation or use animation, our icon, one held, none stored, never sold, kept on use,
// equippable, not auto-equipped, never dropped or stored, no replacement, no virtual weapon).
inline std::optional<std::vector<u8>> KosRow(std::span<const u8> p, const std::vector<Row>& rows,
                                             std::string& why) {
    const Row* from = nullptr;
    u64 next = p.size();
    for (const auto& r : rows) {
        if (r.id == TemplateRow) {
            from = &r;
        }
    }
    if (from == nullptr) {
        why = "no Monocular row (1411) to copy";
        return std::nullopt;
    }
    for (const auto& r : rows) {
        if (r.data > from->data && r.data < next) {
            next = r.data;
        }
    }
    for (const auto& r : rows) {
        if (r.name > from->data && r.name < next) {
            next = r.name;
        }
    }
    if (next - from->data < RowSize) {
        why = "the goods rows are not 108 bytes (another game version?)";
        return std::nullopt;
    }
    std::vector<u8> row(p.begin() + static_cast<std::ptrdiff_t>(from->data),
                        p.begin() + static_cast<std::ptrdiff_t>(from->data + RowSize));
    PutAt<s32>(row, 0, -1);     // refId
    PutAt<s32>(row, 4, 0);      // sfxVariationId
    PutAt<s32>(row, 16, -1);    // sellValue
    PutAt<s32>(row, 24, -1);    // replaceItemId
    PutAt<u16>(row, 44, static_cast<u16>(Icon)); // iconId
    PutAt<u16>(row, 54, 1);     // maxNum
    row[59] = 0;                // refCategory
    row[62] = 0;                // goodsUseAnim
    row[68] = static_cast<u8>((row[68] & ~0x80) | 0x40); // not consumed, equippable
    row[69] = static_cast<u8>(row[69] & ~(0x01 | 0x08 | 0x10)); // not auto-equipped, dropped, stored
    PutAt<u16>(row, 72, 0);     // maxRepositoryNum
    PutAt<s32>(row, 92, -1);    // refVirtualWepId
    return row;
}

// The PARAM with `row` appended as `id` (scripts/build_cigarette_patch.py's append_good: the row
// table grows by one entry and is written in id order, every original byte after it moves 24 bytes
// down unchanged, the new row's data goes at the end and has no name).
inline std::optional<std::vector<u8>> AppendRow(std::span<const u8> p, s32 id,
                                                std::span<const u8> row, std::string& why) {
    const auto rows = ParamRows(p, why);
    if (!rows) {
        return std::nullopt;
    }
    if (HasRow(*rows, id)) {
        why = "the row already exists";
        return std::nullopt;
    }
    const std::size_t end = 64 + rows->size() * 24;
    std::vector<u8> out;
    out.reserve(p.size() + 24 + row.size());
    out.insert(out.end(), p.begin(), p.begin() + 64);
    u32 strings{};
    ReadAt(p, 0, strings);
    PutAt<u32>(out, 0, strings + 24);
    PutAt<u16>(out, 10, static_cast<u16>(rows->size() + 1));
    PutAt<u64>(out, 48, end + 24);
    const u64 added = p.size() + 24;
    auto sorted = *rows;
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Row& a, const Row& b) { return a.id < b.id; });
    bool placed = false;
    const auto put_row = [&out](s32 rid, u64 data, u64 name) {
        const std::size_t at = out.size();
        out.resize(at + 24, 0);
        PutAt<s32>(out, at, rid);
        PutAt<u64>(out, at + 8, data);
        PutAt<u64>(out, at + 16, name);
    };
    for (const auto& r : sorted) {
        if (!placed && id < r.id) {
            put_row(id, added, 0);
            placed = true;
        }
        put_row(r.id, r.data + 24, r.name ? r.name + 24 : 0);
    }
    if (!placed) {
        put_row(id, added, 0);
    }
    out.insert(out.end(), p.begin() + static_cast<std::ptrdiff_t>(end), p.end());
    out.insert(out.end(), row.begin(), row.end());
    return out;
}

// ---- BND4 (headers of 0x24+ bytes: flags, -1, size, unpacked size, u32 offset, id, name) -------

// The archive's entry `id`: its header's offset, data offset and size.
struct Entry {
    std::size_t header{};
    u32 offset{};
    s64 size{};
};
// The entry whose header is at `at`, stored unpacked (a parambnd's and msgbnd's entries are).
inline std::optional<Entry> EntryAt(std::span<const u8> bnd, std::size_t at) {
    Entry e{at};
    s64 unpacked{};
    if (!ReadAt(bnd, at + 8, e.size) || !ReadAt(bnd, at + 16, unpacked) ||
        !ReadAt(bnd, at + 24, e.offset) || e.size <= 0 || unpacked != e.size ||
        e.offset + static_cast<u64>(e.size) > bnd.size()) {
        return std::nullopt;
    }
    return e;
}
// The entry headers' count and size, or false.
inline bool Bnd4Headers(std::span<const u8> bnd, s32& count, s64& header) {
    return bnd.size() >= 0x40 && std::memcmp(bnd.data(), "BND4", 4) == 0 &&
           ReadAt(bnd, 0x0C, count) && ReadAt(bnd, 0x20, header) && header >= 0x24 &&
           header <= 0x100 && count > 0 && count <= 4096 &&
           0x40 + static_cast<u64>(header) * static_cast<u64>(count) <= bnd.size();
}
inline std::optional<Entry> FindBnd4(std::span<const u8> bnd, s32 id) {
    s32 count{};
    s64 header{};
    if (!Bnd4Headers(bnd, count, header)) {
        return std::nullopt;
    }
    for (s32 k = 0; k < count; ++k) {
        const std::size_t at = 0x40 + static_cast<std::size_t>(header) * static_cast<std::size_t>(k);
        s32 entry_id{};
        if (ReadAt(bnd, at + 28, entry_id) && entry_id == id) {
            return EntryAt(bnd, at);
        }
    }
    return std::nullopt;
}
// The entry whose UTF-16 name ends with `suffix`.
inline std::optional<Entry> FindBnd4Named(std::span<const u8> bnd, std::u16string_view suffix) {
    s32 count{};
    s64 header{};
    if (!Bnd4Headers(bnd, count, header) || bnd[0x30] == 0) {
        return std::nullopt;
    }
    for (s32 k = 0; k < count; ++k) {
        const std::size_t at = 0x40 + static_cast<std::size_t>(header) * static_cast<std::size_t>(k);
        u32 name{};
        if (!ReadAt(bnd, at + 32, name)) {
            return std::nullopt;
        }
        std::size_t end = name;
        while (end + 1 < bnd.size() && (bnd[end] != 0 || bnd[end + 1] != 0)) {
            end += 2;
        }
        if (end + 1 >= bnd.size() || name > end) {
            continue;
        }
        const std::size_t length = (end - name) / 2;
        if (length < suffix.size()) {
            continue;
        }
        bool same = true;
        for (std::size_t c = 0; c < suffix.size() && same; ++c) {
            u16 ch{};
            ReadAt(bnd, name + 2 * (length - suffix.size() + c), ch);
            same = ch == suffix[c];
        }
        if (same) {
            return EntryAt(bnd, at);
        }
    }
    return std::nullopt;
}
inline std::span<const u8> EntryData(std::span<const u8> bnd, const Entry& e) {
    return bnd.subspan(e.offset, static_cast<std::size_t>(e.size));
}
// The archive with one entry's data replaced: the new data at the end (16-aligned), its header's
// sizes and offset updated; every other byte as it was.
inline std::optional<std::vector<u8>> ReplaceBnd4(std::span<const u8> bnd, const Entry& e,
                                                  std::span<const u8> data) {
    const std::size_t at = (bnd.size() + 15) & ~std::size_t{15};
    if (at + data.size() > 0xFFFFFFFFull || e.header + 32 > bnd.size()) {
        return std::nullopt;
    }
    std::vector<u8> out(bnd.begin(), bnd.end());
    out.resize(at, 0);
    out.insert(out.end(), data.begin(), data.end());
    PutAt<s64>(out, e.header + 8, static_cast<s64>(data.size()));
    PutAt<s64>(out, e.header + 16, static_cast<s64>(data.size()));
    PutAt<u32>(out, e.header + 24, static_cast<u32>(at));
    return out;
}

// ---- FMG (version 2, 64-bit string offsets; UTF-16) -------------------------------------------

// id -> text (nullopt: an entry without text). nullopt when it is not one.
using Texts = std::map<s32, std::optional<std::u16string>>;
inline std::optional<Texts> ParseFmg(std::span<const u8> f) {
    s32 groups{}, strings{};
    s64 offsets{};
    if (f.size() < 40 || f[0] != 0 || f[2] != 2 || !ReadAt(f, 12, groups) ||
        !ReadAt(f, 16, strings) || !ReadAt(f, 24, offsets) || groups < 0 || strings < 0 ||
        groups > 1'000'000 || strings > 1'000'000 || offsets < 40 ||
        static_cast<u64>(offsets) + 8ull * static_cast<u64>(strings) > f.size() ||
        40 + 16ull * static_cast<u64>(groups) > f.size()) {
        return std::nullopt;
    }
    Texts texts;
    // 64-bit range arithmetic and a counted loop (2026-11-06, a review: a malformed range could
    // overflow s32, and one ending at INT_MAX never ended); all the ranges together cover at most
    // the file's strings, so a malformed file cannot make it loop for ever either.
    s64 covered = 0;
    for (s32 g = 0; g < groups; ++g) {
        s32 index{}, first{}, last{};
        const std::size_t at = 40 + 16 * static_cast<std::size_t>(g);
        ReadAt(f, at, index);
        ReadAt(f, at + 4, first);
        ReadAt(f, at + 8, last);
        const s64 count = static_cast<s64>(last) - static_cast<s64>(first) + 1;
        covered += count;
        if (count <= 0 || index < 0 || static_cast<s64>(index) + count > strings ||
            covered > strings) {
            return std::nullopt;
        }
        for (s64 k = 0; k < count; ++k) {
            const s32 id = static_cast<s32>(static_cast<s64>(first) + k);
            u64 text_at{};
            ReadAt(f, static_cast<std::size_t>(offsets) + 8 * static_cast<std::size_t>(index + k),
                   text_at);
            if (text_at == 0) {
                texts[id] = std::nullopt;
                continue;
            }
            std::u16string text;
            std::size_t c = text_at;
            for (;;) {
                u16 ch{};
                if (!ReadAt(f, c, ch)) {
                    return std::nullopt;
                }
                if (ch == 0) {
                    break;
                }
                text.push_back(static_cast<char16_t>(ch));
                c += 2;
            }
            texts[id] = std::move(text);
        }
    }
    return texts;
}
// The script's pack_fmg: one single-id range per entry, sorted; an entry without text (or empty)
// has offset 0.
inline std::vector<u8> PackFmg(const Texts& texts) {
    const std::size_t count = texts.size();
    const std::size_t start = 40 + 24 * count;
    std::vector<u8> strings;
    std::vector<u64> offsets;
    offsets.reserve(count);
    for (const auto& [id, text] : texts) {
        if (!text || text->empty()) {
            offsets.push_back(0);
            continue;
        }
        offsets.push_back(start + strings.size());
        for (const char16_t ch : *text) {
            strings.push_back(static_cast<u8>(ch & 0xFF));
            strings.push_back(static_cast<u8>(ch >> 8));
        }
        strings.push_back(0);
        strings.push_back(0);
    }
    std::vector<u8> out(start + strings.size(), 0);
    out[2] = 2;
    PutAt<u32>(out, 4, static_cast<u32>(out.size()));
    out[8] = 1;
    PutAt<s32>(out, 12, static_cast<s32>(count));
    PutAt<s32>(out, 16, static_cast<s32>(count));
    PutAt<s32>(out, 20, 255);
    PutAt<s64>(out, 24, static_cast<s64>(40 + 16 * count));
    std::size_t k = 0;
    for (const auto& [id, text] : texts) {
        const std::size_t at = 40 + 16 * k;
        PutAt<s32>(out, at, static_cast<s32>(k));
        PutAt<s32>(out, at + 4, id);
        PutAt<s32>(out, at + 8, id);
        PutAt<u64>(out, 40 + 16 * count + 8 * k, offsets[k]);
        ++k;
    }
    std::memcpy(out.data() + start, strings.data(), strings.size());
    return out;
}

// ---- TPF: our packet art in the Monocular's icon file, for our VR card ------------------------

inline constexpr std::size_t IconPayload = 13 * 13 * 64 * 16; // 416 x 416 BC7, PS4 tiled
// `tpf` (menu_knowledge_00012's, unpacked) with our texture in it and its internal name made
// MENU_Knowledge_<icon> (the same length: no offset changes). False (with `why`) when the
// template is not the one-texture 400 x 400 BC7 the game ships.
inline bool FillIcon(std::vector<u8>& tpf, std::span<const u8> payload, s32 icon, std::string& why) {
    u32 count{}, offset{}, size{}, tile{}, name{};
    u16 width{}, height{};
    if (icon <= 0 || icon >= 100000 || tpf.size() < 0x30 || std::memcmp(tpf.data(), "TPF\0", 4) != 0 ||
        !ReadAt<u32>(tpf, 8, count) || count != 1 || tpf[12] != 4 || !ReadAt(tpf, 16, offset) ||
        !ReadAt(tpf, 20, size) || !ReadAt(tpf, 28, width) || !ReadAt(tpf, 30, height) ||
        !ReadAt(tpf, 36, tile) || !ReadAt(tpf, 40, name)) {
        why = "the icon template is not a one-texture PS4 TPF";
        return false;
    }
    if ((tpf[24] != 102 && tpf[24] != 106 && tpf[24] != 107) || width != 400 || height != 400 ||
        tile != 13 || size != IconPayload || payload.size() != IconPayload ||
        static_cast<u64>(offset) + size > tpf.size()) {
        why = "the icon template is not a 400 x 400 BC7 icon";
        return false;
    }
    static constexpr std::u16string_view From = u"MENU_Knowledge_00012";
    const std::size_t bytes = (From.size() + 1) * 2;
    if (static_cast<u64>(name) + bytes > offset) {
        why = "the icon template's texture name is not where expected";
        return false;
    }
    for (std::size_t c = 0; c <= From.size(); ++c) {
        u16 ch{};
        ReadAt<u16>(tpf, name + 2 * c, ch);
        if (ch != (c < From.size() ? From[c] : 0)) {
            why = "the icon template's texture is not MENU_Knowledge_00012";
            return false;
        }
    }
    for (int i = 0, v = icon; i < 5; ++i, v /= 10) {
        PutAt<u16>(tpf, name + 2 * (19 - i), static_cast<u16>(u'0' + v % 10));
    }
    std::memcpy(tpf.data() + offset, payload.data(), payload.size());
    return true;
}

} // namespace VR::KosLights
