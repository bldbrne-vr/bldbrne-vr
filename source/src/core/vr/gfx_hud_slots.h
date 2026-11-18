// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cmath>
#include <span>
#include <string_view>
#include <vector>

#include "core/vr/gfx_tile.h"

// The HUD movie's widgets moved apart as it loads (menu/fe.gfx). Every gameplay widget is a named
// child of the movie's root (PlayerHUD, TalkWindowTop, BossTop, ActionButtonTop, GetItemList,
// ConditionGaugeList, MapName, TextEffect_*), placed once by a root PlaceObject. Moving that
// placement moves the widget, its animations and its masks together: what the game draws is
// unchanged, only where on the stage. The UI layer then cuts each widget out of its own slot, so
// two widgets the game shows at once never share a cut-out (ui_layout.h). The matrix is rewritten
// with its scale and rotation kept bit for bit; any layout not as expected leaves the movie as it
// is (the caller serves the game's own file).
namespace VR::GfxHudSlots {

using GfxTile::s32;
using GfxTile::u16;
using GfxTile::u32;
using GfxTile::u8;

struct Move {
    std::string_view name; // the root child's instance name
    s32 dx{0};             // twips (stage px * 20)
    s32 dy{0};
    // Empty: the root placement moves (the whole widget). Else only the placements at these depths
    // inside the widget's own sprite, every keyframe of them (a banner's words and glow without
    // its screen-wide dim). The sprite must be placed nowhere else.
    std::span<const u16> depths{};
};

struct Result {
    std::vector<u8> movie; // empty: left as it is (see `why`)
    const char* why{""};
    int moved{0}; // placements rewritten (root and nested)
};

namespace Detail {

using namespace GfxTile::Detail;

// The raw MATRIX fields (16.16 scale and rotate, twips translate), to write back exactly.
struct RawMatrix {
    bool has_scale{false}, has_rotate{false};
    s32 sx{0}, sy{0}, r0{0}, r1{0}, tx{0}, ty{0};
};

inline RawMatrix ReadRaw(BitReader& b) {
    RawMatrix m;
    if ((m.has_scale = b.Ub(1) != 0)) {
        const int n = static_cast<int>(b.Ub(5));
        m.sx = b.Sb(n);
        m.sy = b.Sb(n);
    }
    if ((m.has_rotate = b.Ub(1) != 0)) {
        const int n = static_cast<int>(b.Ub(5));
        m.r0 = b.Sb(n);
        m.r1 = b.Sb(n);
    }
    const int n = static_cast<int>(b.Ub(5));
    m.tx = b.Sb(n);
    m.ty = b.Sb(n);
    b.Align();
    return m;
}

inline std::vector<u8> WriteRaw(const RawMatrix& m) {
    BitWriter w;
    w.Ub(m.has_scale ? 1 : 0, 1);
    if (m.has_scale) {
        const int n = SignedBits({m.sx, m.sy});
        w.Ub(static_cast<u32>(n), 5);
        w.Sb(m.sx, n);
        w.Sb(m.sy, n);
    }
    w.Ub(m.has_rotate ? 1 : 0, 1);
    if (m.has_rotate) {
        const int n = SignedBits({m.r0, m.r1});
        w.Ub(static_cast<u32>(n), 5);
        w.Sb(m.r0, n);
        w.Sb(m.r1, n);
    }
    if (m.tx == 0 && m.ty == 0) {
        w.Ub(0, 5);
    } else {
        const int n = SignedBits({m.tx, m.ty});
        w.Ub(static_cast<u32>(n), 5);
        w.Sb(m.tx, n);
        w.Sb(m.ty, n);
    }
    return w.Bytes();
}

// Where a PlaceObject2 / 3's matrix is (body offsets), or none.
struct MatrixSpan {
    std::size_t begin{0}, end{0};
    RawMatrix m;
};

inline std::optional<MatrixSpan> FindMatrix(std::span<const u8> d, const Tag& tag) {
    const std::size_t end = tag.body + tag.length;
    std::size_t p = tag.body;
    if (p + (tag.type == 70 ? 4 : 3) > end) {
        return std::nullopt;
    }
    const u8 flags = d[p++];
    const u8 flags2 = tag.type == 70 ? d[p++] : 0;
    p += 2; // depth
    if (tag.type == 70 && ((flags2 & 0x08) != 0 || ((flags2 & 0x10) != 0 && (flags & 0x02) != 0))) {
        if (!CString(d, p, end)) {
            return std::nullopt;
        }
    }
    if (flags & 0x02) {
        p += 2;
    }
    if ((flags & 0x04) == 0 || p >= end) {
        return std::nullopt;
    }
    BitReader b{d.subspan(0, end), p};
    MatrixSpan s;
    s.begin = p;
    s.m = ReadRaw(b);
    s.end = b.Align();
    if (!b.Ok() || s.end > end) {
        return std::nullopt;
    }
    return s;
}

// The short form only where the original had it (the exporter writes some short tags long).
inline void PutTagHeader(std::vector<u8>& out, u32 type, std::size_t length, bool was_long) {
    if (length < 0x3F && !was_long) {
        const u32 code = (type << 6) | static_cast<u32>(length);
        out.push_back(static_cast<u8>(code));
        out.push_back(static_cast<u8>(code >> 8));
        return;
    }
    const u32 code = (type << 6) | 0x3F;
    out.push_back(static_cast<u8>(code));
    out.push_back(static_cast<u8>(code >> 8));
    for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<u8>(length >> (8 * i)));
    }
}

} // namespace Detail

// The root's first tag (after the header's RECT, frame rate and count), or 0: not a movie.
inline std::size_t RootTags(std::span<const u8> d) {
    if (d.size() < 21 || !((d[0] == 'G' && d[1] == 'F' && d[2] == 'X') ||
                           (d[0] == 'F' && d[1] == 'W' && d[2] == 'S'))) {
        return 0;
    }
    const std::size_t bits = 5 + 4 * std::size_t{static_cast<u8>(d[8] >> 3)};
    const std::size_t off = 8 + (bits + 7) / 8 + 4;
    return off < d.size() ? off : 0;
}

// `moves` applied to the root's placements of those names (and to later root placements at the
// same depth: an animation's keys). A name not found, or found twice, leaves the movie as it is.
inline Result MoveRootClips(std::span<const u8> d, std::span<const Move> moves) {
    using namespace Detail;
    Result r;
    const std::size_t first = RootTags(d);
    if (first == 0) {
        r.why = "not an uncompressed GFX movie";
        return r;
    }
    std::vector<Tag> tags;
    if (!ReadTags(d, first, d.size(), tags)) {
        r.why = "its tags do not parse";
        return r;
    }
    // Root depth -> the move of the whole widget placed there; sprite id -> the move of some of
    // its own depths.
    std::vector<std::pair<u16, const Move*>> at;
    std::vector<std::pair<u16, const Move*>> inside;
    std::vector<int> found(moves.size(), 0);
    for (const auto& tag : tags) {
        const auto place = ReadPlace(d, tag);
        if (!place || place->name.empty()) {
            continue;
        }
        for (std::size_t i = 0; i < moves.size(); ++i) {
            if (place->name != moves[i].name) {
                continue;
            }
            ++found[i];
            if (moves[i].depths.empty()) {
                at.emplace_back(place->depth, &moves[i]);
            } else if (place->character) {
                inside.emplace_back(*place->character, &moves[i]);
            } else {
                found[i] = 2; // a nested move needs the widget's sprite
            }
        }
    }
    for (const int n : found) {
        if (n != 1) {
            r.why = n == 0 ? "a widget is not on the root" : "a widget is placed twice";
            return r;
        }
    }
    // A sprite whose own depths move must be placed by its widget alone, anywhere in the movie.
    for (const auto& [sprite, move] : inside) {
        int uses = 0;
        const auto count = [&](std::span<const Tag> list) {
            for (const auto& t : list) {
                const auto p = ReadPlace(d, t);
                uses += p && p->character == sprite ? 1 : 0;
            }
        };
        count(tags);
        for (const auto& tag : tags) {
            std::vector<Tag> inner;
            if (tag.type == 39 && tag.length >= 4 &&
                ReadTags(d, tag.body + 4, tag.body + tag.length, inner)) {
                count(inner);
            }
        }
        if (uses != 1) {
            r.why = "a widget's sprite is placed elsewhere too";
            return r;
        }
    }
    // Copies `list` (one tag sequence: the root's or a sprite's) to `out`, the placements `pick`
    // returns a move for rewritten.
    const auto emit = [&](std::vector<u8>& out, std::span<const Tag> list, std::size_t from,
                          std::size_t to, auto&& pick, auto&& nested) {
        std::size_t copied = from;
        for (const auto& tag : list) {
            std::vector<u8> body;
            if (tag.type == 39) {
                if (!nested(tag, body)) {
                    continue;
                }
            } else {
                const auto place = ReadPlace(d, tag);
                const Move* move = place ? pick(place->depth) : nullptr;
                const auto span = move ? FindMatrix(d, tag) : std::nullopt;
                if (!span) {
                    continue;
                }
                RawMatrix m = span->m;
                m.tx += move->dx;
                m.ty += move->dy;
                const auto matrix = WriteRaw(m);
                body.assign(d.begin() + static_cast<std::ptrdiff_t>(tag.body),
                            d.begin() + static_cast<std::ptrdiff_t>(span->begin));
                body.insert(body.end(), matrix.begin(), matrix.end());
                body.insert(body.end(), d.begin() + static_cast<std::ptrdiff_t>(span->end),
                            d.begin() + static_cast<std::ptrdiff_t>(tag.body + tag.length));
                ++r.moved;
            }
            out.insert(out.end(), d.begin() + static_cast<std::ptrdiff_t>(copied),
                       d.begin() + static_cast<std::ptrdiff_t>(tag.start));
            PutTagHeader(out, tag.type, body.size(), tag.body - tag.start == 6);
            out.insert(out.end(), body.begin(), body.end());
            copied = tag.body + tag.length;
        }
        out.insert(out.end(), d.begin() + static_cast<std::ptrdiff_t>(copied),
                   d.begin() + static_cast<std::ptrdiff_t>(to));
    };
    const auto none = [](const Tag&, std::vector<u8>&) { return false; };
    // A moved sprite: its id and frame count, then its own tags with those depths rewritten.
    const auto sprite = [&](const Tag& tag, std::vector<u8>& body) {
        const u16 id = static_cast<u16>(d[tag.body] | (d[tag.body + 1] << 8));
        const bool moved = std::any_of(inside.begin(), inside.end(),
                                       [id](const auto& e) { return e.first == id; });
        std::vector<Tag> inner;
        if (!moved || tag.length < 4 || !ReadTags(d, tag.body + 4, tag.body + tag.length, inner)) {
            return false;
        }
        // Several moves may share the sprite, each with its own depths.
        const auto pick = [&inside, id](u16 depth) -> const Move* {
            for (const auto& [s, m] : inside) {
                if (s == id && std::find(m->depths.begin(), m->depths.end(), depth) !=
                                   m->depths.end()) {
                    return m;
                }
            }
            return nullptr;
        };
        body.assign(d.begin() + static_cast<std::ptrdiff_t>(tag.body),
                    d.begin() + static_cast<std::ptrdiff_t>(tag.body + 4));
        emit(body, inner, tag.body + 4, tag.body + tag.length, pick, none);
        return true;
    };
    const auto root_pick = [&](u16 depth) -> const Move* {
        for (const auto& [where, m] : at) {
            if (where == depth) {
                return m;
            }
        }
        return nullptr;
    };
    std::vector<u8> out(d.begin(), d.begin() + static_cast<std::ptrdiff_t>(first));
    out.reserve(d.size() + 256);
    emit(out, tags, first, d.size(), root_pick, sprite);
    const u32 length = static_cast<u32>(out.size());
    for (int i = 0; i < 4; ++i) {
        out[4 + i] = static_cast<u8>(length >> (8 * i));
    }
    r.movie = std::move(out);
    r.why = "moved";
    return r;
}

} // namespace VR::GfxHudSlots
