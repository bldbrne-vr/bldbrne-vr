// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <optional>
#include <span>
#include <vector>

#include "core/vr/gfx_area_name.h"
#include "core/vr/gfx_tile.h"

// The dark box behind the NPCs' subtitles, taken out of the HUD movie (menu/fe.gfx) as it loads
// (vr.ini ui_dialogue_backing; the tester, 2026-10-16: in VR it is a dark slab in the world during
// every conversation). Fe_fla.TalkWindow_98 places at depth 1 a sprite whose depth 1 is a
// MULTIPLY-blended sprite (at ~60 % alpha) of one shape: a 1920 x 340 white-to-black linear
// gradient across the stage's bottom and a solid black fill. Its colours are lifted toward white:
// all the way (none: multiplying by white changes nothing) or most of the way (faint); the
// subtitle's text, its fades and everything else stay the game's own. Same length, nothing moved;
// the layout is checked from the player's own movie and anything unexpected leaves it unchanged.
namespace VR::GfxTalkWindow {

constexpr const char* SpriteClass = "Fe_fla.TalkWindow_98";

enum class Backing : int { None = 0, Faint = 1, Game = 2 };

// A colour channel of the box as `b` draws it: none = white; faint = 30 % of its darkness.
constexpr GfxTile::u8 Lift(GfxTile::u8 c, Backing b) {
    if (b == Backing::None) {
        return 0xFF;
    }
    if (b == Backing::Faint) {
        return static_cast<GfxTile::u8>(255 - ((255 - c) * 30 + 50) / 100);
    }
    return c;
}

struct Result {
    std::vector<GfxTile::u8> movie; // empty: left as it is (see `why`)
    const char* why{""};
    int colours{0}; // fill colours lifted (gradient records and solid fills)
};

inline Result ClearBacking(std::span<const GfxTile::u8> movie, Backing backing) {
    using namespace GfxAreaName::Detail;
    using GfxTile::u16;
    using GfxTile::u8;
    Result r;
    if (backing == Backing::Game) {
        r.why = "the game's own box kept (ui_dialogue_backing=2)";
        return r;
    }
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
    // A sprite's character at `depth` (the first placed there), and that placement's tag.
    const auto at_depth = [&](const Tag& sprite, u16 depth,
                              std::optional<u8>* blend) -> std::optional<u16> {
        std::vector<Tag> inner;
        if (sprite.type != 39 || sprite.length < 4 ||
            !ReadTags(movie, sprite.body + 4, sprite.body + sprite.length, inner)) {
            return std::nullopt;
        }
        for (const auto& t : inner) {
            const auto place = ReadPlace(movie, t);
            if (place && place->depth == depth && place->character) {
                if (blend != nullptr) {
                    *blend = BlendMode(movie, t);
                }
                return place->character;
            }
        }
        return std::nullopt;
    };
    const Tag* talk = sprite_id ? defined(*sprite_id) : nullptr;
    if (talk == nullptr || talk->type != 39) {
        r.why = "no TalkWindow sprite";
        return r;
    }
    const auto window_id = at_depth(*talk, 1, nullptr);
    const Tag* window = window_id ? defined(*window_id) : nullptr;
    if (window == nullptr || window->type != 39) {
        r.why = "its depth 1 is not a sprite";
        return r;
    }
    std::optional<u8> blend;
    const auto dim_id = at_depth(*window, 1, &blend);
    if (!dim_id || blend != GfxAreaName::BlendMultiply) {
        r.why = "the window's depth 1 is not multiply-blended";
        return r;
    }
    const Tag* dim = defined(*dim_id);
    const auto shape_id = dim && dim->type == 39 ? OnlyChild(movie, *dim) : std::nullopt;
    const Tag* shape = shape_id ? defined(*shape_id) : nullptr;
    if (shape == nullptr || (shape->type != 2 && shape->type != 22 && shape->type != 32)) {
        r.why = "the box is not a sprite of one shape";
        return r;
    }
    // DefineShape(1/2/3): id, bounds, then its fill styles: linear gradients and solid fills only.
    const std::size_t end = shape->body + shape->length;
    BitReader bounds{movie.subspan(0, end), shape->body + 2};
    const int bits = static_cast<int>(bounds.Ub(5));
    for (int i = 0; i < 4; ++i) {
        bounds.Sb(bits);
    }
    std::size_t p = bounds.Align();
    const std::size_t colour = shape->type == 32 ? 4 : 3; // RGBA in DefineShape3
    if (!bounds.Ok() || p >= end || movie[p] == 0 || movie[p] == 0xFF) {
        r.why = "the box has no fill styles";
        return r;
    }
    const int fills = movie[p++];
    std::vector<std::size_t> colours; // where each RGB starts
    for (int f = 0; f < fills; ++f) {
        if (p >= end) {
            r.why = "its fills do not parse";
            return r;
        }
        const u8 type = movie[p++];
        if (type == 0x00) {
            if (p + colour > end) {
                r.why = "its fills do not parse";
                return r;
            }
            colours.push_back(p);
            p += colour;
        } else if (type == 0x10) {
            BitReader matrix{movie.subspan(0, end), p};
            ReadMatrix(matrix);
            p = matrix.Align();
            if (!matrix.Ok() || p >= end) {
                r.why = "its gradient does not parse";
                return r;
            }
            const int records = movie[p++] & 0x0F;
            if (records < 2 || p + records * (1 + colour) > end) {
                r.why = "its gradient does not parse";
                return r;
            }
            for (int i = 0; i < records; ++i) {
                colours.push_back(p + i * (1 + colour) + 1);
            }
            p += records * (1 + colour);
        } else {
            r.why = "the box has a fill that is not a colour or a linear gradient";
            return r;
        }
    }
    r.movie.assign(movie.begin(), movie.end());
    for (const std::size_t at : colours) {
        for (std::size_t c = 0; c < 3; ++c) { // alpha (DefineShape3) left as it is
            r.movie[at + c] = Lift(r.movie[at + c], backing);
        }
        ++r.colours;
    }
    r.why = backing == Backing::None ? "the subtitle box made white (multiplied, it changes nothing)"
                                     : "the subtitle box made faint";
    return r;
}

} // namespace VR::GfxTalkWindow
