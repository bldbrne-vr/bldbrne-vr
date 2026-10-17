// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <vector>

#include "core/vr/gfx_tile.h"

// The area name's dark strip, taken out of the HUD movie (game asset) as it loads. Fe_fla.
// MapName_162 (the name shown for ~5 s on entering an area) has four children: at depth 1 a
// 1920 x 240 white-to-black linear gradient across the stage's bottom, MULTIPLY-blended (the dark
// rectangle behind the name, in the flat game, on the virtual screen and in VR alike), at depth 3
// the ink smoke (SUBTRACT), at depth 5 the rule (ADD) and at depth 7 the words. Every colour of
// the gradient is made white: multiplying by white changes nothing, so the strip is gone while the
// smoke, the rule, the words and their fades stay the game's own. Same length, nothing moved; the
// layout is checked from the user's own movie and anything unexpected leaves it unchanged.
namespace VR::GfxAreaName {

constexpr const char* SpriteClass = "Fe_fla.MapName_162";
constexpr GfxTile::u16 DimDepth = 1;
constexpr GfxTile::u8 BlendMultiply = 3;

struct Result {
    std::vector<GfxTile::u8> movie; // empty: left as it is (see `why`)
    const char* why{""};
    int colours{0}; // gradient records whitened
};

namespace Detail {

using namespace GfxTile::Detail;
using GfxTile::u16;
using GfxTile::u8;

inline u16 U16(std::span<const u8> d, std::size_t p) {
    return static_cast<u16>(d[p] | (d[p + 1] << 8));
}

// A PlaceObject3's blend mode: its last byte when blend mode is the only one of the second
// flags' optional fields (no filters, cache-as-bitmap or class name) and there are no clip
// actions after it.
inline std::optional<u8> BlendMode(std::span<const u8> d, const Tag& tag) {
    if (tag.type != 70 || tag.length < 4) {
        return std::nullopt;
    }
    const u8 flags = d[tag.body];
    const u8 flags2 = d[tag.body + 1];
    if (flags2 != 0x02 || (flags & 0x80) != 0) {
        return std::nullopt;
    }
    return d[tag.body + tag.length - 1];
}

// The one character a single-frame sprite places.
inline std::optional<u16> OnlyChild(std::span<const u8> d, const Tag& sprite) {
    std::vector<Tag> inner;
    if (sprite.length < 4 || !ReadTags(d, sprite.body + 4, sprite.body + sprite.length, inner)) {
        return std::nullopt;
    }
    std::optional<u16> child;
    for (const auto& t : inner) {
        if (t.type != 26 && t.type != 70) {
            continue;
        }
        const auto place = ReadPlace(d, t);
        if (!place || !place->character || child) {
            return std::nullopt;
        }
        child = place->character;
    }
    return child;
}

} // namespace Detail

inline Result ClearDim(std::span<const GfxTile::u8> movie) {
    using namespace Detail;
    Result r;
    if (movie.size() < 16 || !((movie[0] == 'G' && movie[1] == 'F' && movie[2] == 'X') ||
                               (movie[0] == 'F' && movie[1] == 'W' && movie[2] == 'S'))) {
        r.why = "not an uncompressed movie";
        return r;
    }
    const std::size_t header_end = 8 + (5 + 4 * (movie[8] >> 3) + 7) / 8 + 4;
    std::vector<Tag> tags;
    if (header_end >= movie.size() || !ReadTags(movie, header_end, movie.size(), tags)) {
        r.why = "its tags do not parse";
        return r;
    }
    // The sprite by its class (SymbolClass).
    std::optional<u16> sprite_id;
    for (const auto& t : tags) {
        if (t.type != 76 || t.length < 2) {
            continue;
        }
        std::size_t p = t.body + 2;
        const std::size_t end = t.body + t.length;
        for (u16 i = 0, n = U16(movie, t.body); i < n && p + 2 <= end; ++i) {
            const u16 id = U16(movie, p);
            p += 2;
            const auto name = CString(movie, p, end);
            if (!name) {
                break;
            }
            if (*name == SpriteClass) {
                sprite_id = id;
            }
        }
    }
    const auto defined = [&](u16 id) -> const Tag* {
        for (const auto& t : tags) {
            if (DefinesCharacter(t.type) && t.length >= 2 && U16(movie, t.body) == id) {
                return &t;
            }
        }
        return nullptr;
    };
    const Tag* sprite = sprite_id ? defined(*sprite_id) : nullptr;
    if (sprite == nullptr || sprite->type != 39) {
        r.why = "no MapName sprite";
        return r;
    }
    // Its multiply-blended child at depth 1: a sprite holding the gradient shape.
    std::vector<Tag> inner;
    if (sprite->length < 4 ||
        !ReadTags(movie, sprite->body + 4, sprite->body + sprite->length, inner)) {
        r.why = "the MapName sprite does not parse";
        return r;
    }
    std::optional<u16> dim_id;
    for (const auto& t : inner) {
        const auto place = ReadPlace(movie, t);
        if (place && place->depth == DimDepth && place->character) {
            if (BlendMode(movie, t) != BlendMultiply) {
                r.why = "its depth 1 is not multiply-blended";
                return r;
            }
            dim_id = place->character;
        }
    }
    const Tag* dim = dim_id ? defined(*dim_id) : nullptr;
    const auto shape_id = dim && dim->type == 39 ? OnlyChild(movie, *dim) : std::nullopt;
    const Tag* shape = shape_id ? defined(*shape_id) : nullptr;
    if (shape == nullptr || (shape->type != 2 && shape->type != 22 && shape->type != 32)) {
        r.why = "its depth 1 is not a sprite of one shape";
        return r;
    }
    // DefineShape(2/3): id, bounds, then one fill style: a linear gradient (matrix, records).
    const std::size_t end = shape->body + shape->length;
    BitReader bounds{movie.subspan(0, end), shape->body + 2};
    const int bits = static_cast<int>(bounds.Ub(5));
    for (int i = 0; i < 4; ++i) {
        bounds.Sb(bits);
    }
    std::size_t p = bounds.Align();
    if (!bounds.Ok() || p + 2 > end || movie[p] != 1 || movie[p + 1] != 0x10) {
        r.why = "its shape is not one linear gradient";
        return r;
    }
    BitReader matrix{movie.subspan(0, end), p + 2};
    ReadMatrix(matrix);
    p = matrix.Align();
    if (!matrix.Ok() || p >= end) {
        r.why = "its gradient does not parse";
        return r;
    }
    const int records = movie[p] & 0x0F;
    const std::size_t colour = shape->type == 32 ? 4 : 3; // RGBA in DefineShape3
    ++p;
    if (records < 2 || p + records * (1 + colour) > end) {
        r.why = "its gradient does not parse";
        return r;
    }
    r.movie.assign(movie.begin(), movie.end());
    for (int i = 0; i < records; ++i) {
        const std::size_t at = p + i * (1 + colour) + 1;
        for (std::size_t c = 0; c < 3; ++c) { // alpha (DefineShape3) left as it is
            r.movie[at + c] = 0xFF;
        }
        ++r.colours;
    }
    r.why = "the dim strip made white (multiplied, it changes nothing)";
    return r;
}

} // namespace VR::GfxAreaName
