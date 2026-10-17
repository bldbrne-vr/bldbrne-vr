// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The VR tile's movie side (header-only, no emulator dependencies; tests/test_vr_tile.cpp).
// Since 2026-10-05 the icon is a cell of the game's own atlas (Icon::atlas, core/vr/tpf_patch.h),
// not the embedded bitmap described below (the emulator no longer ships a bitmap icon: since
// 2026-10-09 the goggles are composited at install over the user's own atlas,
// core/vr/tile_icon_compose.h; the bitmap path stays for the tests).
//
// Bloodborne's Options grid is the Scaleform movie game asset (an uncompressed SWF, GFX v11):
// sprite "Top" (#74) places the row-0 tiles Item_0_0..Item_0_2 (sprite #45: the tile frame #41 at
// depth 1 and the highlight "Cursor" #44 at depth 3; the suitcase / body / gear icons are the
// book's background art). The game's C++ drives each cell's instance by name ("Item_%d_%d") and
// hides the instance of an empty cell. AddTile adds the 4th row-0 tile "Item_0_3" one column right
// of System: a new sprite = our icon bitmap (DefineBitsLossless2 + a bitmap-filled DefineShape,
// depth 1, inside the frame's border) under sprite #45's own frame (moved to depth 2, as over the
// weapon tiles' parchment) and Cursor. Without the code patches that make cell 3 a real entry the
// game hides it again, icon included.
namespace VR::GfxTile {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using s32 = std::int32_t;

constexpr u16 TopSpriteId = 74;  // "Top"
constexpr u16 TileSpriteId = 45; // the row-0 tile (frame + Cursor)
constexpr const char* FirstName = "Item_0_0";
constexpr const char* SourceName = "Item_0_2"; // System: the new tile goes one column right of it
constexpr const char* NewName = "Item_0_3";
constexpr u16 NewDepth = 23; // between Item_0_2 (21) and Item_1_0 (25), free in Top
// The icon's box in the tile's own coordinates (stage px): the frame #41 (placed at (1.9, 20.6)
// x1.01, shape bounds (0.2..86.2, -19.1..67.8)) covers (2.1..89.0, 1.3..89.1); its border is
// ~3 px. The row-0 icons (suitcase, body, gear) are 80 x 80 cells of MENU_Common_00100 drawn 1:1
// in stage px with a non-smoothed fill, System's at (6.3, 5.1) in its tile: ours the same (the
// icon agent's runtime check: the game draws such a bitmap point-sampled, RMS 0.0 against nearest
// sampling).
constexpr float IconX = 6.3f;
constexpr float IconY = 5.1f;
constexpr float IconSize = 80.0f;

// The icon: DefineBitsLossless2 format 5 payload (zlib of premultiplied A,R,G,B bytes per pixel,
// row by row), drawn into the IconSize box (1 texel per stage px for an 80 x 80 icon), not
// smoothed, as the game's own row-0 icons.
struct Icon {
    u16 width{0};
    u16 height{0};
    const u8* zlib_argb{nullptr};
    std::size_t zlib_size{0};
    // No bitmap: the game's own parchment (#46, an empty weapon tile's) fills the frame instead
    // (VR_TILE_ICON=parchment, a fallback / diagnostic).
    bool parchment{false};
    // No bitmap of our own: an 80 x 80 cell of the game's icon atlas MENU_Common_00100 (the
    // movie's external image the row-0 icons come from) at (atlas_x, atlas_y) px, filled like the
    // game's own icons (core/vr/tpf_patch.h puts the goggles there). The default since 2026-10-05:
    // an embedded bitmap makes the game's Scaleform create a texture of its own, a path no retail
    // movie takes, and with it the GPU command processor's load crash came in about 1 in 10 starts.
    bool atlas{false};
    u16 atlas_x{0};
    u16 atlas_y{0};
};

constexpr u16 ParchmentShapeId = 46; // 80 x 80 px at (4.0, 0.2), MENU_Common_00100
constexpr const char* AtlasImageName = "MENU_Common_00100"; // GFX DefineExternalImage2 (tag 1009)

struct Result {
    std::vector<u8> movie; // empty on failure
    std::string error;     // why, on failure
    u16 bitmap_id{0};
    u16 shape_id{0};
    u16 sprite_id{0};
    float x{0.0f}; // Item_0_3's stage position
    float y{0.0f};
};

namespace Detail {

struct Tag {
    u32 type{0};
    std::size_t start{0}; // the tag header
    std::size_t body{0};
    std::size_t length{0};
};

// Tags from `off` to `end` (a sprite's control tags stop at End; the file's stop at End or the end).
inline bool ReadTags(std::span<const u8> d, std::size_t off, std::size_t end, std::vector<Tag>& out) {
    while (off < end) {
        if (off + 2 > end) {
            return false;
        }
        Tag tag;
        tag.start = off;
        const u32 code = d[off] | (d[off + 1] << 8);
        off += 2;
        tag.type = code >> 6;
        std::size_t length = code & 0x3F;
        if (length == 0x3F) {
            if (off + 4 > end) {
                return false;
            }
            length = d[off] | (d[off + 1] << 8) | (d[off + 2] << 16) | (std::size_t{d[off + 3]} << 24);
            off += 4;
        }
        if (length > end - off) {
            return false;
        }
        tag.body = off;
        tag.length = length;
        out.push_back(tag);
        off += length;
        if (tag.type == 0) {
            return true;
        }
    }
    return true;
}

class BitReader {
public:
    BitReader(std::span<const u8> d, std::size_t off) : d{d}, byte{off} {}
    bool Ok() const {
        return ok;
    }
    u32 Ub(int n) {
        u32 v = 0;
        for (int i = 0; i < n; ++i) {
            if (byte >= d.size()) {
                ok = false;
                return 0;
            }
            v = (v << 1) | ((d[byte] >> (7 - bit)) & 1);
            if (++bit == 8) {
                bit = 0;
                ++byte;
            }
        }
        return v;
    }
    s32 Sb(int n) {
        const u32 v = Ub(n);
        if (n > 0 && n < 32 && (v & (1u << (n - 1))) != 0) {
            return static_cast<s32>(v) - static_cast<s32>(1u << n);
        }
        return static_cast<s32>(v);
    }
    std::size_t Align() {
        if (bit != 0) {
            bit = 0;
            ++byte;
        }
        return byte;
    }

private:
    std::span<const u8> d;
    std::size_t byte;
    int bit{0};
    bool ok{true};
};

class BitWriter {
public:
    void Ub(u32 v, int n) {
        for (int i = n - 1; i >= 0; --i) {
            if (bit == 0) {
                out.push_back(0);
            }
            if ((v >> i) & 1) {
                out.back() |= static_cast<u8>(0x80 >> bit);
            }
            bit = (bit + 1) & 7;
        }
    }
    void Sb(s32 v, int n) {
        Ub(static_cast<u32>(v) & (n >= 32 ? 0xFFFFFFFFu : ((1u << n) - 1)), n);
    }
    std::vector<u8> Bytes() const {
        return out;
    }

private:
    std::vector<u8> out;
    int bit{0};
};

// Bits a signed field needs for every value in the list.
inline int SignedBits(std::initializer_list<s32> values) {
    int n = 1;
    for (const s32 v : values) {
        const std::int64_t m = v < 0 ? -std::int64_t{v} : std::int64_t{v};
        int b = 0;
        while ((m >> b) != 0) {
            ++b;
        }
        n = std::max(n, b + 1);
    }
    return n;
}

struct Matrix {
    double sx{1.0}, sy{1.0}, r0{0.0}, r1{0.0};
    s32 tx{0}, ty{0}; // twips
};

inline Matrix ReadMatrix(BitReader& b) {
    Matrix m;
    if (b.Ub(1)) {
        const int n = static_cast<int>(b.Ub(5));
        m.sx = b.Sb(n) / 65536.0;
        m.sy = b.Sb(n) / 65536.0;
    }
    if (b.Ub(1)) {
        const int n = static_cast<int>(b.Ub(5));
        m.r0 = b.Sb(n) / 65536.0;
        m.r1 = b.Sb(n) / 65536.0;
    }
    const int n = static_cast<int>(b.Ub(5));
    m.tx = b.Sb(n);
    m.ty = b.Sb(n);
    b.Align();
    return m;
}

// MATRIX with an optional scale (16.16) and a translation in twips.
inline void WriteMatrix(BitWriter& w, std::optional<std::pair<s32, s32>> scale, s32 tx, s32 ty) {
    if (scale) {
        const int n = SignedBits({scale->first, scale->second});
        w.Ub(1, 1);
        w.Ub(static_cast<u32>(n), 5);
        w.Sb(scale->first, n);
        w.Sb(scale->second, n);
    } else {
        w.Ub(0, 1);
    }
    w.Ub(0, 1); // no rotation
    if (tx == 0 && ty == 0) {
        w.Ub(0, 5);
    } else {
        const int n = SignedBits({tx, ty});
        w.Ub(static_cast<u32>(n), 5);
        w.Sb(tx, n);
        w.Sb(ty, n);
    }
}

// A PlaceObject2 / PlaceObject3's depth, character, matrix and instance name.
struct Place {
    u16 depth{0};
    std::optional<u16> character;
    Matrix matrix;
    bool has_matrix{false};
    std::string name;
};

inline std::optional<std::string> CString(std::span<const u8> d, std::size_t& p, std::size_t end) {
    std::string s;
    while (p < end && d[p] != 0) {
        s += static_cast<char>(d[p++]);
    }
    if (p >= end) {
        return std::nullopt;
    }
    ++p;
    return s;
}

inline void SkipCxformWithAlpha(BitReader& b) {
    const bool add = b.Ub(1) != 0;
    const bool mult = b.Ub(1) != 0;
    const int n = static_cast<int>(b.Ub(4));
    if (mult) {
        for (int i = 0; i < 4; ++i) {
            b.Sb(n);
        }
    }
    if (add) {
        for (int i = 0; i < 4; ++i) {
            b.Sb(n);
        }
    }
    b.Align();
}

inline std::optional<Place> ReadPlace(std::span<const u8> d, const Tag& tag) {
    if (tag.type != 26 && tag.type != 70) {
        return std::nullopt;
    }
    const std::size_t end = tag.body + tag.length;
    std::size_t p = tag.body;
    if (p + (tag.type == 70 ? 4 : 3) > end) {
        return std::nullopt;
    }
    const u8 flags = d[p++];
    const u8 flags2 = tag.type == 70 ? d[p++] : 0;
    Place place;
    place.depth = static_cast<u16>(d[p] | (d[p + 1] << 8));
    p += 2;
    if (tag.type == 70 && ((flags2 & 0x08) != 0 || ((flags2 & 0x10) != 0 && (flags & 0x02) != 0))) {
        if (!CString(d, p, end)) { // class name
            return std::nullopt;
        }
    }
    if (flags & 0x02) {
        if (p + 2 > end) {
            return std::nullopt;
        }
        place.character = static_cast<u16>(d[p] | (d[p + 1] << 8));
        p += 2;
    }
    if (flags & 0x04) {
        BitReader b{d.subspan(0, end), p};
        place.matrix = ReadMatrix(b);
        place.has_matrix = true;
        p = b.Align();
        if (!b.Ok()) {
            return std::nullopt;
        }
    }
    if (flags & 0x08) {
        BitReader b{d.subspan(0, end), p};
        SkipCxformWithAlpha(b);
        p = b.Align();
        if (!b.Ok()) {
            return std::nullopt;
        }
    }
    if (flags & 0x10) {
        p += 2; // ratio
    }
    if (flags & 0x20) {
        const auto name = CString(d, p, end);
        if (!name) {
            return std::nullopt;
        }
        place.name = *name;
    }
    return place;
}

inline void PutU16(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v));
    out.push_back(static_cast<u8>(v >> 8));
}

inline void PutTag(std::vector<u8>& out, u32 type, const std::vector<u8>& body, bool long_form) {
    if (!long_form && body.size() < 0x3F) {
        PutU16(out, (type << 6) | static_cast<u32>(body.size()));
    } else {
        PutU16(out, (type << 6) | 0x3F);
        const u32 n = static_cast<u32>(body.size());
        out.push_back(static_cast<u8>(n));
        out.push_back(static_cast<u8>(n >> 8));
        out.push_back(static_cast<u8>(n >> 16));
        out.push_back(static_cast<u8>(n >> 24));
    }
    out.insert(out.end(), body.begin(), body.end());
}

// Tags that define a character id in their first two bytes.
inline bool DefinesCharacter(u32 type) {
    switch (type) {
    case 2: case 6: case 7: case 10: case 11: case 20: case 21: case 22: case 32: case 33:
    case 34: case 35: case 36: case 37: case 39: case 46: case 48: case 60: case 75: case 83:
    case 84: case 87: case 90: case 1001: case 1005: case 1009:
        return true;
    default:
        return false;
    }
}

} // namespace Detail

// The movie with the 4th row-0 tile, or an error (the movie is not the expected one: anything
// else than Bloodborne's own game asset layout, or it already has an Item_0_3).
inline Result AddTile(std::span<const u8> d, const Icon& icon) {
    using namespace Detail;
    Result r;
    const auto fail = [&r](std::string why) {
        r.movie.clear();
        r.error = std::move(why);
        return r;
    };
    if (d.size() < 21 || !((d[0] == 'G' && d[1] == 'F' && d[2] == 'X') ||
                           (d[0] == 'F' && d[1] == 'W' && d[2] == 'S'))) {
        return fail("not an uncompressed GFX / SWF movie");
    }
    const u32 length = d[4] | (d[5] << 8) | (d[6] << 16) | (u32{d[7]} << 24);
    if (length != d.size()) {
        return fail("header length differs from the file size");
    }
    const bool own_bitmap = !icon.parchment && !icon.atlas;
    if (own_bitmap &&
        (icon.width == 0 || icon.height == 0 || icon.zlib_argb == nullptr || icon.zlib_size < 2)) {
        return fail("no icon");
    }
    if (icon.atlas && (icon.atlas_x % 4 != 0 || icon.atlas_y % 4 != 0 ||
                       icon.atlas_x + IconSize > 1024 || icon.atlas_y + IconSize > 1024)) {
        return fail("atlas cell outside MENU_Common_00100");
    }
    const int rect_bits = d[8] >> 3;
    const std::size_t header_end = 8 + (5 + 4 * rect_bits + 7) / 8 + 4;
    std::vector<Tag> tags;
    if (!ReadTags(d, header_end, d.size(), tags) || tags.empty()) {
        return fail("malformed tags");
    }
    // The characters: the ids in use, sprite #45's control tags, sprite #74.
    u32 max_id = 0;
    const Tag* tile = nullptr;
    const Tag* top = nullptr;
    bool parchment_shape = false;
    std::optional<u16> atlas_image; // the external image MENU_Common_00100's character id
    for (const Tag& tag : tags) {
        if (DefinesCharacter(tag.type) && tag.length >= 2) {
            const u32 id = d[tag.body] | (d[tag.body + 1] << 8);
            max_id = std::max(max_id, id);
            parchment_shape |= tag.type == 2 && id == ParchmentShapeId;
            // DefineExternalImage2: u32 id, u16 format, u16 width, u16 height, then the export
            // name (u8 length + chars): 1024 x 1024 "MENU_Common_00100".
            const std::string_view name{AtlasImageName};
            if (tag.type == 1009 && tag.length >= 11 + name.size() &&
                d[tag.body + 2] == 0 && d[tag.body + 3] == 0 &&
                (d[tag.body + 6] | (d[tag.body + 7] << 8)) == 1024 &&
                (d[tag.body + 8] | (d[tag.body + 9] << 8)) == 1024 &&
                d[tag.body + 10] == name.size() &&
                std::memcmp(d.data() + tag.body + 11, name.data(), name.size()) == 0) {
                atlas_image = static_cast<u16>(id);
            }
            if (tag.type == 39 && id == TileSpriteId) {
                tile = &tag;
            } else if (tag.type == 39 && id == TopSpriteId) {
                top = &tag;
            }
        }
    }
    if (tile == nullptr || top == nullptr || tile->length < 4 || top->length < 4) {
        return fail("sprite #45 or #74 not found");
    }
    if (max_id + 3 >= 16384) {
        return fail("no free character ids");
    }
    if (icon.parchment && !parchment_shape) {
        return fail("no parchment shape #46");
    }
    if (icon.atlas && !atlas_image) {
        return fail("no external image MENU_Common_00100");
    }
    // Sprite #45: exactly the frame #41 (depth 1) and the Cursor (depth 3), as in the game's movie.
    std::vector<Tag> tile_tags;
    if (!ReadTags(d, tile->body + 4, tile->body + tile->length, tile_tags)) {
        return fail("sprite #45 malformed");
    }
    const Tag* frame = nullptr;
    bool cursor = false;
    for (const Tag& tag : tile_tags) {
        const auto place = ReadPlace(d, tag);
        if (!place) {
            if (tag.type == 26 || tag.type == 70) {
                return fail("sprite #45: a placement could not be read");
            }
            continue;
        }
        if (place->depth == 2) {
            return fail("sprite #45 already uses depth 2");
        }
        if (place->depth == 1 && place->character == 41) {
            frame = &tag;
        }
        cursor |= place->depth == 3 && place->name == "Cursor";
    }
    if (frame == nullptr || !cursor) {
        return fail("sprite #45 is not the frame + Cursor tile");
    }
    // Sprite #74: where Item_0_0 and Item_0_2 are, no Item_0_3 yet, depth 23 free.
    std::vector<Tag> top_tags;
    if (!ReadTags(d, top->body + 4, top->body + top->length, top_tags)) {
        return fail("sprite #74 malformed");
    }
    const Tag* source = nullptr;
    std::optional<Place> first_place, source_place;
    for (const Tag& tag : top_tags) {
        const auto place = ReadPlace(d, tag);
        if (!place) {
            if (tag.type == 26 || tag.type == 70) {
                return fail("sprite #74: a placement could not be read");
            }
            continue;
        }
        if (place->depth == NewDepth) {
            return fail("sprite #74 already uses depth 23");
        }
        if (place->name == NewName) {
            return fail("the movie already has an Item_0_3");
        }
        if (place->name == FirstName) {
            first_place = place;
        } else if (place->name == SourceName) {
            source = &tag;
            source_place = place;
        }
    }
    if (source == nullptr || !first_place || !source_place || !source_place->has_matrix ||
        !first_place->has_matrix || source_place->character != TileSpriteId) {
        return fail("Item_0_0 / Item_0_2 not found in sprite #74");
    }
    // One column right of System: the row's own column step (Item_0_0 .. Item_0_2 is two steps).
    const s32 step = (source_place->matrix.tx - first_place->matrix.tx) / 2;
    if (step < 60 * 20 || step > 120 * 20) {
        return fail("unexpected column step");
    }
    const s32 new_tx = source_place->matrix.tx + step;
    const s32 new_ty = source_place->matrix.ty;
    if (icon.parchment) {
        r.sprite_id = static_cast<u16>(max_id + 1);
    } else if (icon.atlas) {
        r.bitmap_id = *atlas_image; // the game's own external image, not a new character
        r.shape_id = static_cast<u16>(max_id + 1);
        r.sprite_id = static_cast<u16>(max_id + 2);
    } else {
        r.bitmap_id = static_cast<u16>(max_id + 1);
        r.shape_id = static_cast<u16>(max_id + 2);
        r.sprite_id = static_cast<u16>(max_id + 3);
    }
    r.x = new_tx / 20.0f;
    r.y = new_ty / 20.0f;

    // DefineBitsLossless2: the icon (an icon of our own only).
    std::vector<u8> bitmap;
    if (own_bitmap) {
        PutU16(bitmap, r.bitmap_id);
        bitmap.push_back(5); // 32-bit ARGB
        PutU16(bitmap, icon.width);
        PutU16(bitmap, icon.height);
        bitmap.insert(bitmap.end(), icon.zlib_argb, icon.zlib_argb + icon.zlib_size);
    }

    // DefineShape: the icon box, filled with the bitmap (not smoothed, fill 0x42 as the game's
    // own icon fills), drawn like the game's own parchment rect #46 (from the bottom-right
    // corner, fill style 1). The atlas: the same box filled with MENU_Common_00100, 1 texel per
    // stage px, its cell (atlas_x, atlas_y) at the box's corner (as #46 shows its cell).
    const s32 x0 = static_cast<s32>(std::lround(IconX * 20.0f));
    const s32 y0 = static_cast<s32>(std::lround(IconY * 20.0f));
    const s32 size = static_cast<s32>(std::lround(IconSize * 20.0f));
    const u16 texels_w = icon.atlas ? static_cast<u16>(IconSize) : icon.width;
    const u16 texels_h = icon.atlas ? static_cast<u16>(IconSize) : icon.height;
    const s32 fill_tx = icon.atlas ? x0 - s32{icon.atlas_x} * 20 : x0;
    const s32 fill_ty = icon.atlas ? y0 - s32{icon.atlas_y} * 20 : y0;
    std::vector<u8> shape;
    PutU16(shape, r.shape_id);
    if (!icon.parchment) {
        BitWriter w;
        const int n = SignedBits({x0, x0 + size, y0, y0 + size});
        w.Ub(static_cast<u32>(n), 5);
        w.Sb(x0, n);
        w.Sb(x0 + size, n);
        w.Sb(y0, n);
        w.Sb(y0 + size, n);
        const auto bounds = w.Bytes();
        shape.insert(shape.end(), bounds.begin(), bounds.end());
    }
    shape.push_back(1);    // one fill style
    shape.push_back(0x42); // bitmap, not smoothed (point-sampled like the row's other icons)
    PutU16(shape, r.bitmap_id);
    if (!icon.parchment) {
        BitWriter w;
        const s32 sx = static_cast<s32>(std::lround(size * 65536.0 / texels_w));
        const s32 sy = static_cast<s32>(std::lround(size * 65536.0 / texels_h));
        WriteMatrix(w, std::make_pair(sx, sy), fill_tx, fill_ty);
        const auto m = w.Bytes();
        shape.insert(shape.end(), m.begin(), m.end());
    }
    shape.push_back(0);    // no line styles
    shape.push_back(0x10); // 1 fill bit, 0 line bits
    if (!icon.parchment) {
        BitWriter w;
        const int n = SignedBits({x0 + size, y0 + size});
        w.Ub(0, 1);     // style change
        w.Ub(0b00101, 5); // fill style 1, move to
        w.Ub(static_cast<u32>(n), 5);
        w.Sb(x0 + size, n);
        w.Sb(y0 + size, n);
        w.Ub(1, 1); // fill style 1 = the bitmap
        const int e = std::max(2, SignedBits({size, -size}));
        const auto edge = [&w, e](bool vertical, s32 delta) {
            w.Ub(1, 1); // edge
            w.Ub(1, 1); // straight
            w.Ub(static_cast<u32>(e - 2), 4);
            w.Ub(0, 1); // not general
            w.Ub(vertical ? 1 : 0, 1);
            w.Sb(delta, e);
        };
        edge(false, -size);
        edge(true, -size);
        edge(false, size);
        edge(true, size);
        w.Ub(0, 6); // end of shape
        const auto records = w.Bytes();
        shape.insert(shape.end(), records.begin(), records.end());
    }

    // DefineSprite: the icon at depth 1 under sprite #45's own frame (moved from depth 1 to 2, as
    // the frame over the parchment of the weapon tiles, #47) and Cursor (depth 3).
    std::vector<u8> sprite;
    PutU16(sprite, r.sprite_id);
    PutU16(sprite, 1); // one frame, as #45
    {
        std::vector<u8> place{0x06}; // character, matrix
        PutU16(place, 1);
        if (icon.parchment) {
            // #46 (4.0..84.0, 0.2..80.2) scaled into the icon box.
            PutU16(place, ParchmentShapeId);
            BitWriter w;
            const s32 scale = static_cast<s32>(std::lround(IconSize / 80.0 * 65536.0));
            WriteMatrix(w, std::make_pair(scale, scale),
                        static_cast<s32>(std::lround((IconX - 4.0 * IconSize / 80.0) * 20.0)),
                        static_cast<s32>(std::lround((IconY - 0.2 * IconSize / 80.0) * 20.0)));
            const auto m = w.Bytes();
            place.insert(place.end(), m.begin(), m.end());
        } else {
            PutU16(place, r.shape_id);
            place.push_back(0x00); // identity matrix
        }
        PutTag(sprite, 26, place, false);
    }
    for (const Tag& tag : tile_tags) {
        const std::size_t at = sprite.size();
        sprite.insert(sprite.end(), d.begin() + tag.start, d.begin() + tag.body + tag.length);
        if (&tag == frame) {
            const std::size_t depth_at = at + (tag.body - tag.start) + (tag.type == 70 ? 2 : 1);
            sprite[depth_at] = 2;
            sprite[depth_at + 1] = 0;
        }
    }

    // Sprite #74 with Item_0_3 placed right after Item_0_2.
    std::vector<u8> top_body(d.begin() + top->body, d.begin() + top->body + 4);
    for (const Tag& tag : top_tags) {
        top_body.insert(top_body.end(), d.begin() + tag.start, d.begin() + tag.body + tag.length);
        if (&tag == source) {
            std::vector<u8> place{0x26}; // name, matrix, character
            PutU16(place, NewDepth);
            PutU16(place, r.sprite_id);
            BitWriter w;
            WriteMatrix(w, std::nullopt, new_tx, new_ty);
            const auto m = w.Bytes();
            place.insert(place.end(), m.begin(), m.end());
            for (const char* c = NewName; *c != 0; ++c) {
                place.push_back(static_cast<u8>(*c));
            }
            place.push_back(0);
            PutTag(top_body, 26, place, false);
        }
    }

    std::vector<u8>& out = r.movie;
    out.assign(d.begin(), d.begin() + header_end);
    for (const Tag& tag : tags) {
        if (&tag == top) {
            if (own_bitmap) {
                PutTag(out, 36, bitmap, true);
            }
            if (!icon.parchment) {
                PutTag(out, 2, shape, false);
            }
            PutTag(out, 39, sprite, true);
            PutTag(out, 39, top_body, true);
            continue;
        }
        out.insert(out.end(), d.begin() + tag.start, d.begin() + tag.body + tag.length);
    }
    const u32 total = static_cast<u32>(out.size());
    out[4] = static_cast<u8>(total);
    out[5] = static_cast<u8>(total >> 8);
    out[6] = static_cast<u8>(total >> 16);
    out[7] = static_cast<u8>(total >> 24);
    // The result must read back: every tag, sprite #74's placements and the new tile.
    std::vector<Tag> check;
    const std::size_t added = icon.parchment ? 1 : own_bitmap ? 3 : 2;
    if (!ReadTags(out, header_end, out.size(), check) || check.size() != tags.size() + added) {
        return fail("the patched movie does not read back");
    }
    return r;
}

// An icon's DefineBitsLossless2 payload must inflate to width * height * 4 bytes; checked by
// the caller that has zlib (tests). FNV-1a 64 of a byte range (signatures of the game's file).
inline std::uint64_t Fnv1a64(std::span<const u8> bytes) {
    std::uint64_t h = 0xcbf29ce484222325ull;
    for (const u8 c : bytes) {
        h ^= c;
        h *= 0x100000001b3ull;
    }
    return h;
}

} // namespace VR::GfxTile
