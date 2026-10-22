// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <string>

#include "core/vr/gfx_tile.h"

// A native, non-interactive VR label on the title screen. Patch only the three retail
// movies; modified menus are left alone. The label belongs to Top so it follows the
// game's title fades and disappears with the title, including when loading a save.
namespace VR::GfxTitle {

inline std::vector<GfxTile::u8> AddLabel(std::span<const GfxTile::u8> movie, bool low_res = false) {
    using namespace GfxTile;
    using namespace GfxTile::Detail;
    const auto hash = Fnv1a64(movie);
    if (!((movie.size() == 9071 && hash == 0xdef7567a487254fdull) ||
          (movie.size() == 9087 && hash == 0x4ec375c2a750f696ull) ||
          (movie.size() == 9099 && hash == 0x80611d31f1ac6fc1ull))) {
        return {};
    }
    const std::size_t header_end = 8 + (5 + 4 * (movie[8] >> 3) + 7) / 8 + 4;
    std::vector<Tag> tags;
    if (!ReadTags(movie, header_end, movie.size(), tags)) {
        return {};
    }
    // All three variants share Top (#43), with depths through 38 and character IDs
    // through 43. A new child uses the same external font as the title's own text.
    constexpr u16 LabelId = 44;
    constexpr u16 LabelDepth = 39;
    const Tag* top = nullptr;
    for (const auto& tag : tags) {
        if (tag.type == 39 && tag.length >= 4 && movie[tag.body] == 43 &&
            movie[tag.body + 1] == 0) {
            top = &tag;
        }
    }
    if (top == nullptr) {
        return {};
    }
    std::vector<Tag> children;
    if (!ReadTags(movie, top->body + 4, top->body + top->length, children)) {
        return {};
    }

    // Keep the 64 px VR label; allow room for a smaller, inline DIBR status.
    std::vector<u8> text;
    PutU16(text, LabelId);
    BitWriter rect;
    const int width = (low_res ? 720 : 160) * 20;
    const int bits = SignedBits({0, width, 1680});
    rect.Ub(bits, 5);
    rect.Sb(0, bits);
    rect.Sb(width, bits);
    rect.Sb(0, bits);
    rect.Sb(1680, bits);
    const auto bounds = rect.Bytes();
    text.insert(text.end(), bounds.begin(), bounds.end());
    // Retail text flags: text, wrap, multiline, read-only, color; font class,
    // auto-size, non-selectable, layout, HTML, outlines.
    text.insert(text.end(), {0xec, 0xa3});
    constexpr char font[] = "MenuFont_01";
    text.insert(text.end(), font, font + sizeof(font));
    PutU16(text, 1280); // font height in twips
    text.insert(text.end(), {0xcc, 0xcc, 0xcc, 0xff});
    text.push_back(0); // left alignment
    PutU16(text, 0);   // left margin
    PutU16(text, 0);   // right margin
    PutU16(text, 0);   // indent
    PutU16(text, 40);  // leading, as the retail text
    text.push_back(0); // no variable binding
    std::string html = "<p align=\"left\"><font size=\"64\" color=\"#cccccc\" "
                       "letterSpacing=\"0.000000\" kerning=\"1\"><b>VR</b></font>";
    if (low_res) {
        html += "<font size=\"48\" color=\"#cccccc\" "
                "letterSpacing=\"0.000000\" kerning=\"1\"> (LOW RES MODE)</font>";
    }
    html += "</p>";
    text.insert(text.end(), html.begin(), html.end());
    text.push_back(0);

    std::vector<u8> place{0x26}; // character, matrix, instance name
    PutU16(place, LabelDepth);
    PutU16(place, LabelId);
    BitWriter transform;
    // Match the copyright's left edge and ServerVersion's rendered glyph bottoms.
    WriteMatrix(transform, std::nullopt, 125 * 20, 906 * 20);
    const auto matrix = transform.Bytes();
    place.insert(place.end(), matrix.begin(), matrix.end());
    constexpr char name[] = "VrLabel"; // outside the game's localized/dynamic text names
    place.insert(place.end(), name, name + sizeof(name));

    std::vector<u8> sprite(movie.begin() + top->body, movie.begin() + top->body + 4);
    for (const auto& child : children) {
        if (child.type == 1) {
            PutTag(sprite, 26, place, false);
        }
        sprite.insert(sprite.end(), movie.begin() + child.start,
                      movie.begin() + child.body + child.length);
    }
    std::vector<u8> result(movie.begin(), movie.begin() + header_end);
    for (const auto& tag : tags) {
        if (&tag == top) {
            PutTag(result, 37, text, true);
            PutTag(result, 39, sprite, true);
        } else {
            result.insert(result.end(), movie.begin() + tag.start,
                          movie.begin() + tag.body + tag.length);
        }
    }
    const auto length = static_cast<u32>(result.size());
    for (int i = 0; i < 4; ++i) {
        result[4 + i] = static_cast<u8>(length >> (8 * i));
    }
    return result;
}

} // namespace VR::GfxTitle
