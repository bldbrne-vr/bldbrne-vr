// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/vr/bc7_decode.h"
#include "core/vr/game_font.h"
#include "core/vr/thumb_layout.h"
#include "core/vr/tpf_patch.h"

// The game's button icons as the VR controllers' buttons (vr.ini ui_vr_buttons; header-only, no
// emulator dependencies; tests/test_key_glyphs.cpp).
//
// Every button icon the game draws (the key guide under each menu, YES / NO dialogs, the quick
// menu grid, messages, the controls help) is one of the 32 x 32 KG_* textures of
// game asset (BC7, one mip level, PS4 tile mode 13), asked for by name by the movies
// (DefineExternalImage2 "KG_OK") and the game's text (<img src="KG_R1">, <?conclusion?>). The
// icons the VR menu layout presses (openxr_presenter.cpp, the game-menu branch) are redrawn at
// load, in the same atlas pass as the VR tile's icon (bloodborne_vrtile.cpp PrepareAtlas): our
// badges in the style of the VR page's (vr_page_draw.h Badge: a dark disc for a face button, a
// pill for a trigger or grip), their letters in the game's own font from the user's dump. No game
// pixel is in the source.
//
//   ✕ (KG_Cancel) -> A     ○ (KG_OK) -> B       □ (KG_R_L) -> LG     △ (KG_R_U) -> RG
//   L1 -> LT               R1 -> RT              OPTIONS (KG_Start) -> ≡ (the Menu button)
//
// The rest stay the game's: L2 / R2 (nothing in a VR menu), L3 / R3 and the sticks (the sticks'
// own clicks and pushes), the D-pad and the touchpad.
//
// The gameplay HUD (game asset: the action prompts, "Operate Device", "Open Door") draws its own
// smaller ✕, ○ and △ from two of its atlases (HudGlyphs). Those are shown in gameplay, so they get
// the gameplay layout's buttons (core/vr/thumb_layout.h): ✕ interacts (B), ○ dodges (A), △ is the
// Blood Vial (X); with swap_sticks the face buttons trade hands (Y, X, A).
namespace VR::KeyGlyphs {

using u8 = std::uint8_t;
using u32 = std::uint32_t;

constexpr int Size = 32; // texels
constexpr int Super = 4; // supersampling per axis

enum class Shape : u8 { Disc, Pill, Menu };

struct Glyph {
    std::u16string_view texture;
    Shape shape;
    const char* text; // Disc / Pill: the letters
};

inline constexpr std::array<Glyph, 7> Glyphs{{
    {u"KG_Cancel", Shape::Disc, "A"}, // the game's ✕: confirm (A, or X)
    {u"KG_OK", Shape::Disc, "B"},     // ○: back (B, or Y)
    {u"KG_R_L", Shape::Pill, "LG"},   // □: the left grip
    {u"KG_R_U", Shape::Pill, "RG"},   // △: the right grip
    {u"KG_L1", Shape::Pill, "LT"},    // the left trigger
    {u"KG_R1", Shape::Pill, "RT"},    // the right trigger
    {u"KG_Start", Shape::Menu, ""},   // OPTIONS: the Menu button (≡)
}};

// ---- Drawing (straight-alpha RGBA8, 32 x 32) ----------------------------------------------------

namespace detail {

struct Rgba {
    float r, g, b, a;
};

// Signed distance (hi-res px) to a rounded box centred at (cx, cy).
inline float RoundBox(float x, float y, float cx, float cy, float hw, float hh, float r) {
    const float qx = std::abs(x - cx) - (hw - r);
    const float qy = std::abs(y - cy) - (hh - r);
    const float ox = std::max(qx, 0.0f), oy = std::max(qy, 0.0f);
    return std::sqrt(ox * ox + oy * oy) + std::min(std::max(qx, qy), 0.0f) - r;
}

inline float Cover(float d) {
    return std::clamp(0.5f - d, 0.0f, 1.0f);
}

// Premultiplied "over" into a hi-res canvas.
inline void Over(std::vector<Rgba>& c, int i, float r, float g, float b, float a) {
    Rgba& p = c[i];
    p.r = r * a + p.r * (1.0f - a);
    p.g = g * a + p.g * (1.0f - a);
    p.b = b * a + p.b * (1.0f - a);
    p.a = a + p.a * (1.0f - a);
}

} // namespace detail

// The badge in `size` x `size` texels (its parts `scale` x their size in a 32-texel icon), straight
// alpha; the colour of transparent texels is bled from their neighbours (no dark fringe when
// filtered, and BC7 blocks stay on one colour line). Empty when a glyph's letters cannot be drawn
// (no font, or a letter it lacks).
inline std::vector<u8> Draw(const Glyph& g, const GameText::GameFont* font, int size = Size,
                            float scale = 1.0f) {
    using namespace detail;
    const int Hi = size * Super;
    std::vector<Rgba> c(static_cast<std::size_t>(Hi) * Hi, Rgba{0, 0, 0, 0});
    const float s = static_cast<float>(Super) * scale;
    const float cx = 0.5f * Hi, cy = 0.5f * Hi;
    // The body: a disc of the game's own icons' size (26 texels across) or a pill as wide as its
    // L1 / R1 (32 x 21).
    float hw, hh, r;
    if (g.shape == Shape::Pill) {
        hw = 15.5f * s;
        hh = 10.5f * s;
        r = 5.0f * s;
    } else {
        hw = hh = r = 13.0f * s;
    }
    const float rim = 1.4f * s;
    for (int y = 0; y < Hi; ++y) {
        for (int x = 0; x < Hi; ++x) {
            const float d = RoundBox(x + 0.5f, y + 0.5f, cx, cy, hw, hh, r);
            const float body = Cover(d);
            if (body <= 0.0f) {
                continue;
            }
            // dark body, a lighter rim, a faint highlight on the upper half of the rim
            const float inner = Cover(d + rim); // 1 deeper inside than the rim
            const float upper = std::clamp((cy - (y + 0.5f)) / hh, 0.0f, 1.0f);
            const float rim_v = (96.0f + 36.0f * upper) / 255.0f;
            const float in_v = 40.0f / 255.0f;
            const float v = in_v * inner + rim_v * (1.0f - inner);
            Over(c, y * Hi + x, v, v, v * 1.04f, body);
        }
    }
    const float ink_r = 228 / 255.0f, ink_g = 228 / 255.0f, ink_b = 232 / 255.0f;
    if (g.shape == Shape::Menu) {
        // ≡: three bars with round ends.
        for (int bar = -1; bar <= 1; ++bar) {
            const float by = cy + bar * 4.6f * s;
            for (int y = 0; y < Hi; ++y) {
                for (int x = 0; x < Hi; ++x) {
                    const float d =
                        RoundBox(x + 0.5f, y + 0.5f, cx, by, 6.6f * s, 1.15f * s, 1.15f * s);
                    const float a = Cover(d);
                    if (a > 0.0f) {
                        Over(c, y * Hi + x, ink_r, ink_g, ink_b, a);
                    }
                }
            }
        }
    } else {
        if (font == nullptr) {
            return {};
        }
        const float em = (g.shape == Shape::Pill ? 14.0f : 19.0f) * s;
        GameText::Line line;
        if (!GameText::RasterizeLine(*font, g.text, em, g.shape == Shape::Pill ? 0.4f * s : 0.0f,
                                     2, line) ||
            line.missing != 0) {
            return {};
        }
        // Centre the letters' ink (not the pen box) on the badge.
        int x0 = static_cast<int>(line.width), x1 = -1, y0 = static_cast<int>(line.height), y1 = -1;
        for (u32 y = 0; y < line.height; ++y) {
            for (u32 x = 0; x < line.width; ++x) {
                if (line.cover[y * line.width + x] > 24) {
                    x0 = std::min(x0, static_cast<int>(x));
                    x1 = std::max(x1, static_cast<int>(x));
                    y0 = std::min(y0, static_cast<int>(y));
                    y1 = std::max(y1, static_cast<int>(y));
                }
            }
        }
        if (x1 < x0) {
            return {};
        }
        const int ox = static_cast<int>(std::lround(cx - 0.5f * (x0 + x1 + 1)));
        const int oy = static_cast<int>(std::lround(cy - 0.5f * (y0 + y1 + 1)));
        for (u32 y = 0; y < line.height; ++y) {
            for (u32 x = 0; x < line.width; ++x) {
                const int tx = ox + static_cast<int>(x), ty = oy + static_cast<int>(y);
                const u8 a = line.cover[y * line.width + x];
                if (a != 0 && tx >= 0 && ty >= 0 && tx < Hi && ty < Hi) {
                    Over(c, ty * Hi + tx, ink_r, ink_g, ink_b, a / 255.0f);
                }
            }
        }
    }
    // Down to size x size (box filter on premultiplied colour), then straight alpha.
    std::vector<Rgba> lo(size * size, Rgba{0, 0, 0, 0});
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            Rgba sum{0, 0, 0, 0};
            for (int j = 0; j < Super; ++j) {
                for (int i = 0; i < Super; ++i) {
                    const Rgba& p = c[(y * Super + j) * Hi + x * Super + i];
                    sum.r += p.r;
                    sum.g += p.g;
                    sum.b += p.b;
                    sum.a += p.a;
                }
            }
            const float n = static_cast<float>(Super * Super);
            lo[y * size + x] = {sum.r / n, sum.g / n, sum.b / n, sum.a / n};
        }
    }
    std::vector<u8> out(size * size * 4);
    std::vector<bool> known(size * size, false);
    for (int i = 0; i < size * size; ++i) {
        const Rgba& p = lo[i];
        const float a = p.a;
        if (a > 1.0f / 512.0f) {
            out[i * 4 + 0] = static_cast<u8>(std::clamp(p.r / a, 0.0f, 1.0f) * 255.0f + 0.5f);
            out[i * 4 + 1] = static_cast<u8>(std::clamp(p.g / a, 0.0f, 1.0f) * 255.0f + 0.5f);
            out[i * 4 + 2] = static_cast<u8>(std::clamp(p.b / a, 0.0f, 1.0f) * 255.0f + 0.5f);
            known[i] = true;
        }
        out[i * 4 + 3] = static_cast<u8>(std::clamp(a, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
    // Bleed: every transparent texel takes the mean colour of its known 8-neighbours, ring by ring.
    for (bool grew = true; grew;) {
        grew = false;
        std::vector<bool> next = known;
        for (int y = 0; y < size; ++y) {
            for (int x = 0; x < size; ++x) {
                if (known[y * size + x]) {
                    continue;
                }
                int sum[3]{}, n = 0;
                for (int j = -1; j <= 1; ++j) {
                    for (int i = -1; i <= 1; ++i) {
                        const int nx = x + i, ny = y + j;
                        if (nx >= 0 && ny >= 0 && nx < size && ny < size && known[ny * size + nx]) {
                            for (int k = 0; k < 3; ++k) {
                                sum[k] += out[(ny * size + nx) * 4 + k];
                            }
                            ++n;
                        }
                    }
                }
                if (n > 0) {
                    for (int k = 0; k < 3; ++k) {
                        out[(y * size + x) * 4 + k] = static_cast<u8>((sum[k] + n / 2) / n);
                    }
                    next[y * size + x] = true;
                    grew = true;
                }
            }
        }
        known = std::move(next);
    }
    return out;
}

// ---- BC7 (modes 5 and 6, whichever fits a block better) -----------------------------------------

namespace detail {

inline constexpr std::array<int, 4> W2{0, 21, 43, 64};
inline constexpr std::array<int, 16> W4{0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};

inline int Lerp(int a, int b, int w) {
    return ((64 - w) * a + w * b + 32) >> 6;
}

struct Bits128 {
    std::array<u8, 16> b{};
    int at{0};
    void Put(u32 value, int count) {
        for (int i = 0; i < count; ++i, ++at) {
            if ((value >> i) & 1u) {
                b[at >> 3] |= static_cast<u8>(1u << (at & 7));
            }
        }
    }
};

// The two ends of the texels' principal axis over `channels` (0..3 of RGBA), as floats.
inline void Axis(const u8 (&px)[16][4], int first, int channels, float (&lo)[4], float (&hi)[4]) {
    float mean[4]{};
    for (const auto& p : px) {
        for (int k = 0; k < channels; ++k) {
            mean[k] += p[first + k] / 16.0f;
        }
    }
    float cov[4][4]{};
    for (const auto& p : px) {
        for (int i = 0; i < channels; ++i) {
            for (int j = 0; j < channels; ++j) {
                cov[i][j] += (p[first + i] - mean[i]) * (p[first + j] - mean[j]);
            }
        }
    }
    float axis[4]{1, 1, 1, 1};
    for (int it = 0; it < 12; ++it) {
        float next[4]{};
        for (int i = 0; i < channels; ++i) {
            for (int j = 0; j < channels; ++j) {
                next[i] += cov[i][j] * axis[j];
            }
        }
        float len = 0.0f;
        for (int i = 0; i < channels; ++i) {
            len += next[i] * next[i];
        }
        if (len < 1e-12f) {
            break;
        }
        len = std::sqrt(len);
        for (int i = 0; i < channels; ++i) {
            axis[i] = next[i] / len;
        }
    }
    float tmin = 1e9f, tmax = -1e9f;
    for (const auto& p : px) {
        float t = 0.0f;
        for (int k = 0; k < channels; ++k) {
            t += (p[first + k] - mean[k]) * axis[k];
        }
        tmin = std::min(tmin, t);
        tmax = std::max(tmax, t);
    }
    for (int k = 0; k < channels; ++k) {
        lo[k] = std::clamp(mean[k] + axis[k] * tmin, 0.0f, 255.0f);
        hi[k] = std::clamp(mean[k] + axis[k] * tmax, 0.0f, 255.0f);
    }
}

// Least-squares ends for given indices (weights w[i] / 64).
template <std::size_t N>
inline bool Refit(const u8 (&px)[16][4], int first, int channels, const int (&idx)[16],
                  const std::array<int, N>& w, float (&lo)[4], float (&hi)[4]) {
    float aa = 0, ab = 0, bb = 0, ax[4]{}, bx[4]{};
    for (int t = 0; t < 16; ++t) {
        const float b = w[idx[t]] / 64.0f, a = 1.0f - b;
        aa += a * a;
        ab += a * b;
        bb += b * b;
        for (int k = 0; k < channels; ++k) {
            ax[k] += a * px[t][first + k];
            bx[k] += b * px[t][first + k];
        }
    }
    const float det = aa * bb - ab * ab;
    if (std::abs(det) < 1e-6f) {
        return false;
    }
    for (int k = 0; k < channels; ++k) {
        lo[k] = std::clamp((ax[k] * bb - bx[k] * ab) / det, 0.0f, 255.0f);
        hi[k] = std::clamp((bx[k] * aa - ax[k] * ab) / det, 0.0f, 255.0f);
    }
    return true;
}

struct Encoded {
    std::array<u8, 16> block{};
    long long error{-1};
};

// Mode 6: RGBA together, 7-bit ends with a p-bit each, 4-bit indices.
inline Encoded Mode6(const u8 (&px)[16][4]) {
    float lo[4], hi[4];
    Axis(px, 0, 4, lo, hi);
    Encoded best;
    for (int pass = 0; pass < 3; ++pass) {
        // Quantize: per end, the p-bit that fits its four channels better.
        int q[2][4], p[2], e[2][4];
        for (int end = 0; end < 2; ++end) {
            const float* v = end == 0 ? lo : hi;
            long long best_err = -1;
            for (int pb = 0; pb < 2; ++pb) {
                long long err = 0;
                int qq[4];
                for (int k = 0; k < 4; ++k) {
                    qq[k] = std::clamp(static_cast<int>(std::lround((v[k] - pb) / 2.0f)), 0, 127);
                    const float d = v[k] - ((qq[k] << 1) | pb);
                    err += static_cast<long long>(d * d);
                }
                if (best_err < 0 || err < best_err) {
                    best_err = err;
                    p[end] = pb;
                    std::memcpy(q[end], qq, sizeof(qq));
                }
            }
            for (int k = 0; k < 4; ++k) {
                e[end][k] = (q[end][k] << 1) | p[end];
            }
        }
        int idx[16];
        long long err = 0;
        for (int t = 0; t < 16; ++t) {
            long long bt = -1;
            for (int i = 0; i < 16; ++i) {
                long long d = 0;
                for (int k = 0; k < 4; ++k) {
                    const int v = Lerp(e[0][k], e[1][k], W4[i]) - px[t][k];
                    d += v * v;
                }
                if (bt < 0 || d < bt) {
                    bt = d;
                    idx[t] = i;
                }
            }
            err += bt;
        }
        if (best.error < 0 || err < best.error) {
            // The anchor (texel 0) must have its index's top bit clear: swap the ends if not.
            const bool swap = idx[0] >= 8;
            const int a = swap ? 1 : 0, z = swap ? 0 : 1;
            Bits128 b;
            b.Put(1u << 6, 7);
            for (int k = 0; k < 4; ++k) {
                b.Put(static_cast<u32>(q[a][k]), 7);
                b.Put(static_cast<u32>(q[z][k]), 7);
            }
            b.Put(static_cast<u32>(p[a]), 1);
            b.Put(static_cast<u32>(p[z]), 1);
            for (int t = 0; t < 16; ++t) {
                b.Put(static_cast<u32>(swap ? 15 - idx[t] : idx[t]), t == 0 ? 3 : 4);
            }
            best.block = b.b;
            best.error = err;
        }
        if (!Refit(px, 0, 4, idx, W4, lo, hi)) {
            break;
        }
    }
    return best;
}

// Mode 5: RGB (7-bit ends, 2-bit indices) and alpha (8-bit ends, 2-bit indices) apart.
inline Encoded Mode5(const u8 (&px)[16][4]) {
    float clo[4], chi[4], alo[4], ahi[4];
    Axis(px, 0, 3, clo, chi);
    Axis(px, 3, 1, alo, ahi);
    Encoded best;
    for (int pass = 0; pass < 3; ++pass) {
        int qc[2][3], ec[2][3], qa[2];
        for (int k = 0; k < 3; ++k) {
            qc[0][k] = std::clamp(static_cast<int>(std::lround(clo[k] / 255.0f * 127.0f)), 0, 127);
            qc[1][k] = std::clamp(static_cast<int>(std::lround(chi[k] / 255.0f * 127.0f)), 0, 127);
            for (int end = 0; end < 2; ++end) {
                ec[end][k] = (qc[end][k] << 1) | (qc[end][k] >> 6);
            }
        }
        qa[0] = std::clamp(static_cast<int>(std::lround(alo[0])), 0, 255);
        qa[1] = std::clamp(static_cast<int>(std::lround(ahi[0])), 0, 255);
        int ci[16], ai[16];
        long long err = 0;
        for (int t = 0; t < 16; ++t) {
            long long bc = -1, ba = -1;
            for (int i = 0; i < 4; ++i) {
                long long d = 0;
                for (int k = 0; k < 3; ++k) {
                    const int v = Lerp(ec[0][k], ec[1][k], W2[i]) - px[t][k];
                    d += v * v;
                }
                if (bc < 0 || d < bc) {
                    bc = d;
                    ci[t] = i;
                }
                const int va = Lerp(qa[0], qa[1], W2[i]) - px[t][3];
                if (ba < 0 || va * va < ba) {
                    ba = static_cast<long long>(va) * va;
                    ai[t] = i;
                }
            }
            err += bc + ba;
        }
        if (best.error < 0 || err < best.error) {
            // Each anchor (texel 0) must have its index's top bit clear: swap those ends if not.
            const bool cs = ci[0] >= 2, as = ai[0] >= 2;
            Bits128 b;
            b.Put(1u << 5, 6);
            b.Put(0, 2); // no rotation
            for (int k = 0; k < 3; ++k) {
                b.Put(static_cast<u32>(qc[cs ? 1 : 0][k]), 7);
                b.Put(static_cast<u32>(qc[cs ? 0 : 1][k]), 7);
            }
            b.Put(static_cast<u32>(qa[as ? 1 : 0]), 8);
            b.Put(static_cast<u32>(qa[as ? 0 : 1]), 8);
            for (int t = 0; t < 16; ++t) {
                b.Put(static_cast<u32>(cs ? 3 - ci[t] : ci[t]), t == 0 ? 1 : 2);
            }
            for (int t = 0; t < 16; ++t) {
                b.Put(static_cast<u32>(as ? 3 - ai[t] : ai[t]), t == 0 ? 1 : 2);
            }
            best.block = b.b;
            best.error = err;
        }
        const bool c_ok = Refit(px, 0, 3, ci, W2, clo, chi);
        const bool a_ok = Refit(px, 3, 1, ai, W2, alo, ahi);
        if (!c_ok && !a_ok) {
            break;
        }
    }
    return best;
}

} // namespace detail

// `rgba` (straight alpha, width x height, multiples of 4) to BC7 blocks, row by row.
inline std::vector<u8> EncodeBc7(std::span<const u8> rgba, int width, int height) {
    std::vector<u8> out;
    out.reserve(static_cast<std::size_t>(width / 4) * (height / 4) * 16);
    for (int by = 0; by < height / 4; ++by) {
        for (int bx = 0; bx < width / 4; ++bx) {
            u8 px[16][4];
            for (int t = 0; t < 16; ++t) {
                const int x = bx * 4 + (t & 3), y = by * 4 + (t >> 2);
                std::memcpy(px[t], rgba.data() + (static_cast<std::size_t>(y) * width + x) * 4, 4);
            }
            const auto m6 = detail::Mode6(px);
            const auto m5 = detail::Mode5(px);
            const auto& pick = m5.error >= 0 && m5.error < m6.error ? m5 : m6;
            out.insert(out.end(), pick.block.begin(), pick.block.end());
        }
    }
    return out;
}

// ---- Into the atlas -----------------------------------------------------------------------------

// Writes every glyph (Glyphs) into the unpacked game asset in place: `written` counts them; the
// returned text lists what was refused ("" = all). A texture that is not the retail 32 x 32 BC7
// one is left as it is.
inline std::string Apply(std::span<u8> tpf, const GameText::GameFont* font, int& written) {
    written = 0;
    std::string why;
    const auto refuse = [&why](std::u16string_view name, const char* reason) {
        if (!why.empty()) {
            why += "; ";
        }
        for (const char16_t ch : name) {
            why += static_cast<char>(ch);
        }
        why += ": ";
        why += reason;
    };
    for (const Glyph& g : Glyphs) {
        const auto t = TpfPatch::FindTexture(tpf, g.texture);
        if (!t) {
            refuse(g.texture, "no such texture");
            continue;
        }
        if (t->format != 102 || t->tile_mode != 13 || t->mips != 1 || t->width != Size ||
            t->height != Size || t->size < 1024 ||
            static_cast<std::size_t>(t->offset) + 1024 > tpf.size()) {
            refuse(g.texture, "not the 32 x 32 BC7 tile-mode-13 icon");
            continue;
        }
        const std::vector<u8> rgba = Draw(g, font);
        if (rgba.empty()) {
            refuse(g.texture, "its letters are not in the font");
            continue;
        }
        const std::vector<u8> blocks = EncodeBc7(rgba, Size, Size);
        u8* const base = tpf.data() + t->offset;
        for (u32 by = 0; by < Size / 4; ++by) {
            for (u32 bx = 0; bx < Size / 4; ++bx) {
                std::memcpy(base + TpfPatch::TiledBlockOffset(bx, by, Size / 4, 16),
                            blocks.data() + (static_cast<std::size_t>(by) * (Size / 4) + bx) * 16,
                            16);
            }
        }
        ++written;
    }
    return why;
}


// ---- The gameplay HUD's own icons --------------------------------------------------------------

// A button glyph drawn into a HUD atlas cell (top-left x, y, size x size texels), the logical
// control of the gameplay layout that sends its button.
struct HudGlyph {
    std::u16string_view texture;
    u32 x, y, size;
    Thumbs::Control logical;
    const char* game; // the game's glyph (the log)
};

inline constexpr std::array<HudGlyph, 4> HudGlyphs{{
    {u"MENU_Common_00091", 661, 123, 26, Thumbs::Control::A, "circle"},   // dodge
    {u"MENU_Common_00091", 694, 123, 26, Thumbs::Control::B, "cross"},    // interact (prompts)
    {u"MENU_Common_00092", 663, 108, 25, Thumbs::Control::X, "triangle"}, // the Blood Vial
    {u"MENU_Common_00092", 703, 108, 25, Thumbs::Control::B, "cross"},    // interact
}};

// Redraws every HudGlyphs cell in place (the cell and a 1-texel ring around it, which only held
// the old glyph's faint edge; the blocks it touches decoded, changed and encoded again; the rest of
// those blocks keep the game's texels). `swap`: swap_sticks. `written` counts them; the returned
// text lists what was refused ("" = all).
inline std::string ApplyHud(std::span<u8> tpf, const GameText::GameFont* font, bool swap,
                            int& written) {
    written = 0;
    std::string why;
    const auto refuse = [&why](const HudGlyph& h, const char* reason) {
        if (!why.empty()) {
            why += "; ";
        }
        why += h.game;
        why += ": ";
        why += reason;
    };
    for (const HudGlyph& h : HudGlyphs) {
        const auto t = TpfPatch::FindTexture(tpf, h.texture);
        if (!t || t->format != 102 || t->tile_mode != 13 || t->mips != 1 || t->width % 4 != 0 ||
            t->height % 4 != 0 || h.x < 1 || h.y < 1 || h.x + h.size + 1 > t->width ||
            h.y + h.size + 1 > t->height || t->size < 0 ||
            static_cast<std::size_t>(t->size) < static_cast<std::size_t>(t->width) * t->height ||
            static_cast<std::size_t>(t->offset) + t->size > tpf.size()) {
            refuse(h, "not the HUD's BC7 atlas");
            continue;
        }
        const Glyph g{h.texture, Shape::Disc, Thumbs::LabelFor(h.logical, swap)};
        // The game's discs fill their cells: ours too (26 texels across in a 32-texel icon).
        const std::vector<u8> badge =
            Draw(g, font, static_cast<int>(h.size), (static_cast<float>(h.size) - 0.5f) / 26.0f);
        if (badge.empty()) {
            refuse(h, "its letter is not in the font");
            continue;
        }
        // The blocks under the cell and its ring, decoded.
        const u32 x0 = h.x - 1, y0 = h.y - 1, x1 = h.x + h.size + 1, y1 = h.y + h.size + 1;
        const u32 bx0 = x0 / 4, by0 = y0 / 4, bx1 = (x1 + 3) / 4, by1 = (y1 + 3) / 4;
        const int w = static_cast<int>(bx1 - bx0) * 4, hgt = static_cast<int>(by1 - by0) * 4;
        std::vector<u8> region(static_cast<std::size_t>(w) * hgt * 4);
        u8* const base = tpf.data() + t->offset;
        const u32 blocks_wide = t->width / 4;
        for (u32 by = by0; by < by1; ++by) {
            for (u32 bx = bx0; bx < bx1; ++bx) {
                u8 texels[64];
                Bc7::DecodeBlock(base + TpfPatch::TiledBlockOffset(bx, by, blocks_wide, 16), texels);
                for (int r = 0; r < 4; ++r) {
                    std::memcpy(&region[((static_cast<std::size_t>(by - by0) * 4 + r) * w +
                                         (bx - bx0) * 4) *
                                        4],
                                texels + r * 16, 16);
                }
            }
        }
        // The ring: cleared; the cell: our badge.
        for (u32 y = y0; y < y1; ++y) {
            for (u32 x = x0; x < x1; ++x) {
                u8* p = &region[((static_cast<std::size_t>(y - by0 * 4)) * w + (x - bx0 * 4)) * 4];
                const bool cell = x >= h.x && y >= h.y && x < h.x + h.size && y < h.y + h.size;
                if (cell) {
                    std::memcpy(p, &badge[((y - h.y) * h.size + (x - h.x)) * 4], 4);
                } else {
                    p[3] = 0;
                }
            }
        }
        const std::vector<u8> blocks = EncodeBc7(region, w, hgt);
        for (u32 by = by0; by < by1; ++by) {
            for (u32 bx = bx0; bx < bx1; ++bx) {
                std::memcpy(base + TpfPatch::TiledBlockOffset(bx, by, blocks_wide, 16),
                            blocks.data() + ((by - by0) * (bx1 - bx0) + (bx - bx0)) * 16, 16);
            }
        }
        ++written;
    }
    return why;
}

} // namespace VR::KeyGlyphs
