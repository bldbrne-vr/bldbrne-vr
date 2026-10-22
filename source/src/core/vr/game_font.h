// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <fmt/format.h>

#include "common/types.h"

// Bloodborne's own menu font, menu/<language>/font.gfx: a Scaleform font library holding one
// DefineFont3 tag of quadratic glyph outlines ("TT-Reimin Y10 Bold" in the western languages).
// Shared by the quick-item labels (bloodborne_assets.cpp RasterizeLabel) and the VR settings page
// (vr_page_render.cpp, through RasterizeLine). Header-only and pure: the bytes come from the
// caller (the assets loader reads the file through the game's mount).
namespace VR::GameText {

namespace detail {
template <typename T>
bool ReadAt(const std::vector<u8>& data, size_t offset, T& out) {
    if (offset + sizeof(T) > data.size()) {
        return false;
    }
    std::memcpy(&out, data.data() + offset, sizeof(T));
    return true;
}
} // namespace detail
using detail::ReadAt;

// ---- The game's font (Scaleform DefineFont3) ----------------------------------------------------

struct GameFont {
    std::vector<u8> tag;                // the DefineFont3 body
    size_t table{0};                    // its glyph offset table (offsets are from here)
    std::vector<u32> offsets;           // glyph shapes
    std::vector<s16> advance;           // EM units (1024 * 20 to the em)
    std::unordered_map<u32, u32> glyph; // code point -> glyph index
    float ascent{0.0f};
    float descent{0.0f};
    std::string name;
};
inline constexpr float FontEm = 20480.0f; // DefineFont3: 20 x the 1024-unit em square

struct BitReader {
    const std::vector<u8>& d;
    size_t byte;
    u32 bit{0};
    bool ok{true};
    u32 Ub(u32 n) {
        u32 v = 0;
        for (u32 i = 0; i < n; ++i) {
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
    s32 Sb(u32 n) {
        const u32 v = Ub(n);
        return n > 0 && (v & (1u << (n - 1))) != 0 ? static_cast<s32>(v) - (1 << n)
                                                   : static_cast<s32>(v);
    }
};

// The font library's DefineFont3 tag (tag 75) in an uncompressed GFX / SWF file.
inline std::unique_ptr<GameFont> ParseGameFont(const std::vector<u8>& f, std::string& why) {
    if (f.size() < 32 ||
        (std::memcmp(f.data(), "GFX", 3) != 0 && std::memcmp(f.data(), "FWS", 3) != 0)) {
        why = "not an uncompressed GFX";
        return nullptr;
    }
    const u32 rect_bits = 5 + 4 * (f[8] >> 3);
    size_t o = 8 + (rect_bits + 7) / 8 + 4;
    while (o + 2 <= f.size()) {
        u16 code_length{};
        ReadAt(f, o, code_length);
        o += 2;
        const u32 code = code_length >> 6;
        u32 length = code_length & 0x3F;
        if (length == 0x3F) {
            ReadAt(f, o, length);
            o += 4;
        }
        if (o + length > f.size()) {
            break;
        }
        if (code == 0) {
            break;
        }
        if (code != 75) {
            o += length;
            continue;
        }
        auto font = std::make_unique<GameFont>();
        font->tag.assign(f.begin() + o, f.begin() + o + length);
        const std::vector<u8>& t = font->tag;
        if (t.size() < 8) {
            break;
        }
        const u8 flags = t[2];
        const u8 name_length = t[4];
        size_t at = 5 + name_length;
        for (size_t i = 5; i < at && i < t.size(); ++i) {
            font->name += static_cast<char>(t[i]);
        }
        u16 count{};
        ReadAt(t, at, count);
        at += 2;
        const bool wide = (flags & 0x08) != 0;
        font->table = at;
        u32 code_table{};
        font->offsets.resize(count);
        for (u16 i = 0; i < count; ++i) {
            if (wide) {
                ReadAt(t, at + 4 * i, font->offsets[i]);
            } else {
                u16 v{};
                ReadAt(t, at + 2 * i, v);
                font->offsets[i] = v;
            }
        }
        if (wide) {
            ReadAt(t, at + 4 * size_t{count}, code_table);
        } else {
            u16 v{};
            ReadAt(t, at + 2 * size_t{count}, v);
            code_table = v;
        }
        const size_t codes = font->table + code_table;
        if ((flags & 0x80) == 0 || (flags & 0x04) == 0 ||
            codes + 2 * size_t{count} + 6 > t.size()) {
            why = fmt::format("DefineFont3 flags {:#x}: no layout or no wide codes", flags);
            return nullptr;
        }
        for (u16 i = 0; i < count; ++i) {
            u16 c{};
            ReadAt(t, codes + 2 * size_t{i}, c);
            font->glyph.emplace(c, i);
        }
        s16 ascent{}, descent{};
        size_t layout = codes + 2 * size_t{count};
        ReadAt(t, layout, ascent);
        ReadAt(t, layout + 2, descent);
        layout += 6;
        font->advance.resize(count);
        for (u16 i = 0; i < count; ++i) {
            ReadAt(t, layout + 2 * size_t{i}, font->advance[i]);
        }
        font->ascent = ascent;
        font->descent = descent;
        return font;
    }
    why = "no DefineFont3";
    return nullptr;
}

struct Segment {
    float x0, y0, x1, y1;
};

// One glyph's outline (SWF shape records: moves, straight and quadratic edges, y down) as line
// segments in pixels: `scale` pixels per EM unit, origin at (`ox`, `oy`) = the pen on the
// baseline.
inline bool GlyphSegments(const GameFont& font, u32 glyph, float scale, float ox, float oy,
                   std::vector<Segment>& out) {
    BitReader bits{font.tag, font.table + font.offsets[glyph]};
    if (bits.byte >= font.tag.size()) {
        return false;
    }
    const u32 fill_bits = bits.Ub(4);
    const u32 line_bits = bits.Ub(4);
    s32 x = 0, y = 0;
    float px = ox, py = oy;
    for (int records = 0; records < 4096 && bits.ok; ++records) {
        if (bits.Ub(1) == 0) {
            const u32 new_styles = bits.Ub(1), line_style = bits.Ub(1), fill1 = bits.Ub(1),
                      fill0 = bits.Ub(1), move = bits.Ub(1);
            if (new_styles == 0 && line_style == 0 && fill1 == 0 && fill0 == 0 && move == 0) {
                return bits.ok; // the end record
            }
            if (move != 0) {
                const u32 n = bits.Ub(5);
                x = bits.Sb(n);
                y = bits.Sb(n);
                px = ox + x * scale;
                py = oy + y * scale;
            }
            if (fill0 != 0) {
                bits.Ub(fill_bits);
            }
            if (fill1 != 0) {
                bits.Ub(fill_bits);
            }
            if (line_style != 0) {
                bits.Ub(line_bits);
            }
            if (new_styles != 0) {
                return false; // never in a font
            }
            continue;
        }
        if (bits.Ub(1) != 0) {
            const u32 n = bits.Ub(4) + 2;
            if (bits.Ub(1) != 0) {
                x += bits.Sb(n);
                y += bits.Sb(n);
            } else if (bits.Ub(1) != 0) {
                y += bits.Sb(n);
            } else {
                x += bits.Sb(n);
            }
            const float nx = ox + x * scale, ny = oy + y * scale;
            out.push_back({px, py, nx, ny});
            px = nx;
            py = ny;
        } else {
            const u32 n = bits.Ub(4) + 2;
            const s32 cx = x + bits.Sb(n), cy = y + bits.Sb(n);
            x = cx + bits.Sb(n);
            y = cy + bits.Sb(n);
            const float qx = ox + cx * scale, qy = oy + cy * scale;
            const float nx = ox + x * scale, ny = oy + y * scale;
            // Flattened: a segment every ~1.5 pixels of the control polygon, 2 to 12 of them.
            const float length = std::hypot(qx - px, qy - py) + std::hypot(nx - qx, ny - qy);
            const int steps = std::clamp(static_cast<int>(length / 1.5f), 2, 12);
            float lx = px, ly = py;
            for (int k = 1; k <= steps; ++k) {
                const float u = static_cast<float>(k) / steps, a = (1 - u) * (1 - u),
                            b = 2 * (1 - u) * u, c = u * u;
                const float sx = a * px + b * qx + c * nx, sy = a * py + b * qy + c * ny;
                out.push_back({lx, ly, sx, sy});
                lx = sx;
                ly = sy;
            }
            px = nx;
            py = ny;
        }
    }
    return false;
}

// Non-zero fill of closed outlines into `cover` (w x h, 0..1): 5 sample rows per pixel row, the
// covered spans of each with exact horizontal coverage.
inline void FillOutlines(const std::vector<Segment>& segments, u32 w, u32 h, std::vector<float>& cover) {
    constexpr int Rows = 5;
    cover.assign(static_cast<size_t>(w) * h, 0.0f);
    struct Crossing {
        float x;
        int dir;
    };
    std::vector<Crossing> xs;
    for (u32 y = 0; y < h; ++y) {
        float* const row = cover.data() + static_cast<size_t>(y) * w;
        for (int r = 0; r < Rows; ++r) {
            const float sy = static_cast<float>(y) + (r + 0.5f) / Rows;
            xs.clear();
            for (const Segment& s : segments) {
                if ((s.y0 <= sy && sy < s.y1) || (s.y1 <= sy && sy < s.y0)) {
                    const float t = (sy - s.y0) / (s.y1 - s.y0);
                    xs.push_back({s.x0 + t * (s.x1 - s.x0), s.y1 > s.y0 ? 1 : -1});
                }
            }
            std::sort(xs.begin(), xs.end(),
                      [](const Crossing& a, const Crossing& b) { return a.x < b.x; });
            int winding = 0;
            for (size_t i = 0; i + 1 < xs.size(); ++i) {
                winding += xs[i].dir;
                if (winding == 0) {
                    continue;
                }
                const float a = std::max(xs[i].x, 0.0f);
                const float b = std::min(xs[i + 1].x, static_cast<float>(w));
                if (b <= a) {
                    continue;
                }
                constexpr float k = 1.0f / Rows;
                const int ia = static_cast<int>(a), ib = static_cast<int>(b);
                if (ia == ib) {
                    row[ia] += (b - a) * k;
                    continue;
                }
                row[ia] += (static_cast<float>(ia + 1) - a) * k;
                for (int x = ia + 1; x < ib; ++x) {
                    row[x] += k;
                }
                if (ib < static_cast<int>(w)) {
                    row[ib] += (b - static_cast<float>(ib)) * k;
                }
            }
        }
    }
}

// One line of `utf8` in `font` at `em_px` pixels to the em (24 stage px is the game's body text):
// 8-bit coverage, `pad` pixels of margin on every side. The pen starts at x = pad, the baseline
// is at y = baseline (pad + the font's ascent); `advance` = the pen's travel (the width the game
// aligns by), letter_spacing_px added after every glyph but the last (Scaleform letterSpacing).
// Characters the font lacks are skipped (`missing` counts them). False for an empty line.
struct Line {
    u32 width{0};
    u32 height{0};
    float baseline{0.0f};
    float advance{0.0f};
    u32 missing{0};
    std::vector<u8> cover;
};

inline float LineAdvance(const GameFont& font, const char* utf8, float em_px,
                         float letter_spacing_px = 0.0f) {
    const float scale = em_px / FontEm;
    float pen = 0.0f;
    u32 count = 0;
    for (const char* s = utf8; *s != 0;) {
        u32 c = static_cast<unsigned char>(*s++);
        if (c >= 0x80) {
            const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            c &= extra == 3 ? 0x07 : extra == 2 ? 0x0F : 0x1F;
            for (int i = 0; i < extra && *s != 0; ++i) {
                c = (c << 6) | (static_cast<unsigned char>(*s++) & 0x3F);
            }
        }
        const auto it = font.glyph.find(c);
        if (it == font.glyph.end()) {
            continue;
        }
        if (count++ > 0) {
            pen += letter_spacing_px;
        }
        pen += font.advance[it->second] * scale;
    }
    return pen;
}

inline bool RasterizeLine(const GameFont& font, const char* utf8, float em_px,
                          float letter_spacing_px, u32 pad, Line& out) {
    out = {};
    const float scale = em_px / FontEm;
    std::vector<std::pair<u32, float>> glyphs; // glyph, pen
    float pen = 0.0f;
    for (const char* s = utf8; *s != 0;) {
        u32 c = static_cast<unsigned char>(*s++);
        if (c >= 0x80) {
            const int extra = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
            c &= extra == 3 ? 0x07 : extra == 2 ? 0x0F : 0x1F;
            for (int i = 0; i < extra && *s != 0; ++i) {
                c = (c << 6) | (static_cast<unsigned char>(*s++) & 0x3F);
            }
        }
        const auto it = font.glyph.find(c);
        if (it == font.glyph.end()) {
            ++out.missing;
            continue;
        }
        if (!glyphs.empty()) {
            pen += letter_spacing_px;
        }
        glyphs.emplace_back(it->second, pen);
        pen += font.advance[it->second] * scale;
    }
    if (glyphs.empty()) {
        return false;
    }
    out.advance = pen;
    out.width = std::min<u32>(static_cast<u32>(std::ceil(pen)) + 2 * pad + 2, 4096);
    out.height = static_cast<u32>(std::ceil((font.ascent + font.descent) * scale)) + 2 * pad + 2;
    out.baseline = static_cast<float>(pad) + font.ascent * scale;
    std::vector<Segment> segments;
    for (const auto& [glyph, x] : glyphs) {
        if (!GlyphSegments(font, glyph, scale, static_cast<float>(pad) + x, out.baseline,
                           segments)) {
            return false;
        }
    }
    std::vector<float> cover;
    FillOutlines(segments, out.width, out.height, cover);
    out.cover.resize(cover.size());
    for (size_t i = 0; i < cover.size(); ++i) {
        out.cover[i] = static_cast<u8>(std::clamp(cover[i], 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    return true;
}

} // namespace VR::GameText
