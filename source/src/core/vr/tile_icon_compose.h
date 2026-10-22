// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "core/vr/vr_tile_goggles_layer.h"

// The VR tile's icon, composited at install from OUR goggles layer (core/vr/vr_tile_goggles_layer.h)
// and the empty parchment cell of the player's own icon atlas (MENU_Common_00100 of
// menu/common.tpf.dcx, the cell at (720, 160) px): no pixel of the game is in the source. Header
// only, no emulator dependencies (core/vr/bloodborne_vrtile.cpp PrepareAtlas, tests/test_vr_tile.cpp).
//
// The steps are the ones our icon was authored with (its compose step,
// which laid the 4x supersampled render over the parchment upsampled bicubic, then downsampled it
// Lanczos and sharpened it), reordered so only an 80 x 80 layer ships:
//  1. the parchment cell, BC1-decoded (as Pillow decodes BC1);
//  2. its x4 bicubic / 4 Lanczos round trip (Pillow's resampling kernels and support, as 1-D
//     80 x 80 matrices applied to rows and columns);
//  3. times the layer's ground multiplier, plus its premultiplied colour, rounded;
//  4. an unsharp mask (Gaussian sigma 1 px, 60 %, threshold 2);
//  5. BC1-encoded (4-colour blocks: the principal axis, 4 least-squares refits and a +-1 local
//     search over both endpoints, as our earlier icon was encoded offline).
// Measured on the player's v1.09 atlas (offline analysis): step 4's image is
// within PSNR 46.7 dB of the icon as it was authored, and the encoded icon's decode matches the
// old shipped icon's decode to within BC1's own error.
namespace VR::TileIcon {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;

constexpr int Size = 80; // px (20 x 20 BC1 blocks)
constexpr int Blocks = Size / 4;
constexpr std::size_t CellBytes = static_cast<std::size_t>(Blocks) * Blocks * 8;
// The game's empty parchment cell in MENU_Common_00100 (row 2, column 9 of its 80-px grid).
constexpr u32 ParchmentX = 720;
constexpr u32 ParchmentY = 160;

// ---- BC1 -------------------------------------------------------------------------------------

inline std::array<double, 3> From565(u16 v) {
    const u32 r = (v >> 11) & 31, g = (v >> 5) & 63, b = v & 31;
    return {static_cast<double>((r << 3) | (r >> 2)), static_cast<double>((g << 2) | (g >> 4)),
            static_cast<double>((b << 3) | (b >> 2))};
}

inline std::array<std::array<double, 3>, 4> Palette(u16 c0, u16 c1) {
    const auto a = From565(c0);
    const auto b = From565(c1);
    std::array<std::array<double, 3>, 4> p{};
    p[0] = a;
    p[1] = b;
    for (int c = 0; c < 3; ++c) {
        if (c0 > c1) {
            p[2][c] = std::floor((2 * a[c] + b[c]) / 3);
            p[3][c] = std::floor((a[c] + 2 * b[c]) / 3);
        } else {
            p[2][c] = std::floor((a[c] + b[c]) / 2);
            p[3][c] = 0.0;
        }
    }
    return p;
}

// One block -> 16 RGB texels (row by row).
inline void DecodeBlock(const u8* block, u8 out[16][3]) {
    const u16 c0 = static_cast<u16>(block[0] | (block[1] << 8));
    const u16 c1 = static_cast<u16>(block[2] | (block[3] << 8));
    const u32 bits = static_cast<u32>(block[4]) | (static_cast<u32>(block[5]) << 8) |
                     (static_cast<u32>(block[6]) << 16) | (static_cast<u32>(block[7]) << 24);
    const auto p = Palette(c0, c1);
    for (int i = 0; i < 16; ++i) {
        const auto& c = p[(bits >> (2 * i)) & 3];
        for (int k = 0; k < 3; ++k) {
            out[i][k] = static_cast<u8>(c[k]);
        }
    }
}

// A cell's blocks (row by row) -> Size x Size RGB.
inline std::vector<u8> DecodeCell(std::span<const u8> blocks) {
    std::vector<u8> rgb(static_cast<std::size_t>(Size) * Size * 3);
    for (int by = 0; by < Blocks; ++by) {
        for (int bx = 0; bx < Blocks; ++bx) {
            u8 px[16][3];
            DecodeBlock(blocks.data() + (static_cast<std::size_t>(by) * Blocks + bx) * 8, px);
            for (int i = 0; i < 16; ++i) {
                const std::size_t o =
                    (static_cast<std::size_t>(by * 4 + i / 4) * Size + (bx * 4 + i % 4)) * 3;
                rgb[o] = px[i][0];
                rgb[o + 1] = px[i][1];
                rgb[o + 2] = px[i][2];
            }
        }
    }
    return rgb;
}

namespace Detail {

inline u16 To565(const std::array<double, 3>& c) {
    // Round half to even (the default rounding mode), as the offline encoder rounded.
    const auto q = [](double v, int top) {
        return static_cast<u32>(std::clamp(std::nearbyint(v * top / 255.0), 0.0,
                                           static_cast<double>(top)));
    };
    return static_cast<u16>((q(c[0], 31) << 11) | (q(c[1], 63) << 5) | q(c[2], 31));
}

struct Scored {
    u16 hi{0};
    u16 lo{0};
    std::array<int, 16> idx{};
    double err{0.0};
};

inline Scored Score(const double px[16][3], u16 c0, u16 c1) {
    Scored s;
    if (c0 == c1) {
        const auto p = Palette(c0, c1);
        s.hi = c0;
        s.lo = c1;
        for (int i = 0; i < 16; ++i) {
            for (int k = 0; k < 3; ++k) {
                s.err += (px[i][k] - p[0][k]) * (px[i][k] - p[0][k]);
            }
        }
        return s;
    }
    s.hi = std::max(c0, c1);
    s.lo = std::min(c0, c1);
    const auto p = Palette(s.hi, s.lo);
    for (int i = 0; i < 16; ++i) {
        double best = 0.0;
        for (int j = 0; j < 4; ++j) {
            double d = 0.0;
            for (int k = 0; k < 3; ++k) {
                d += (px[i][k] - p[j][k]) * (px[i][k] - p[j][k]);
            }
            if (j == 0 || d < best) {
                best = d;
                s.idx[i] = j;
            }
        }
        s.err += best;
    }
    return s;
}

// The dominant eigenvector of a symmetric 3 x 3 matrix (cyclic Jacobi), or false when its largest
// eigenvalue is not above 1e-9.
inline bool PrincipalAxis(double a[3][3], std::array<double, 3>& axis) {
    double v[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
    for (int sweep = 0; sweep < 50; ++sweep) {
        const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
        if (off < 1e-30) {
            break;
        }
        for (int p = 0; p < 2; ++p) {
            for (int q = p + 1; q < 3; ++q) {
                if (std::abs(a[p][q]) < 1e-300) {
                    continue;
                }
                const double theta = (a[q][q] - a[p][p]) / (2 * a[p][q]);
                const double t = (theta >= 0 ? 1.0 : -1.0) /
                                 (std::abs(theta) + std::sqrt(theta * theta + 1));
                const double c = 1 / std::sqrt(t * t + 1), s = t * c;
                for (int k = 0; k < 3; ++k) {
                    const double akp = a[k][p], akq = a[k][q];
                    a[k][p] = c * akp - s * akq;
                    a[k][q] = s * akp + c * akq;
                }
                for (int k = 0; k < 3; ++k) {
                    const double apk = a[p][k], aqk = a[q][k];
                    a[p][k] = c * apk - s * aqk;
                    a[q][k] = s * apk + c * aqk;
                }
                for (int k = 0; k < 3; ++k) {
                    const double vkp = v[k][p], vkq = v[k][q];
                    v[k][p] = c * vkp - s * vkq;
                    v[k][q] = s * vkp + c * vkq;
                }
            }
        }
    }
    int m = 0;
    for (int i = 1; i < 3; ++i) {
        if (a[i][i] > a[m][m]) {
            m = i;
        }
    }
    if (!(a[m][m] > 1e-9)) {
        return false;
    }
    axis = {v[0][m], v[1][m], v[2][m]};
    return true;
}

} // namespace Detail

// 16 RGB texels (row by row, 0..255) -> one BC1 block in 4-colour mode where it can be.
inline std::array<u8, 8> EncodeBlock(const double px[16][3]) {
    using namespace Detail;
    std::array<double, 3> mean{};
    for (int i = 0; i < 16; ++i) {
        for (int k = 0; k < 3; ++k) {
            mean[k] += px[i][k] / 16.0;
        }
    }
    bool flat = true;
    for (int i = 1; i < 16 && flat; ++i) {
        for (int k = 0; k < 3; ++k) {
            flat = flat && px[i][k] == px[0][k];
        }
    }
    double cov[3][3]{};
    if (!flat) {
        for (int i = 0; i < 16; ++i) {
            for (int r = 0; r < 3; ++r) {
                for (int c = 0; c < 3; ++c) {
                    cov[r][c] += (px[i][r] - mean[r]) * (px[i][c] - mean[c]) / 15.0;
                }
            }
        }
    }
    std::array<double, 3> axis{};
    if (!PrincipalAxis(cov, axis)) {
        const double k = 1.0 / std::sqrt(3.0);
        axis = {k, k, k};
    }
    double tmin = 0.0, tmax = 0.0;
    for (int i = 0; i < 16; ++i) {
        double t = 0.0;
        for (int k = 0; k < 3; ++k) {
            t += (px[i][k] - mean[k]) * axis[k];
        }
        tmin = i == 0 ? t : std::min(tmin, t);
        tmax = i == 0 ? t : std::max(tmax, t);
    }
    std::array<double, 3> e0{}, e1{};
    for (int k = 0; k < 3; ++k) {
        e0[k] = mean[k] + axis[k] * tmax;
        e1[k] = mean[k] + axis[k] * tmin;
    }
    std::vector<std::pair<u16, u16>> cands;
    for (int it = 0; it < 4; ++it) {
        const u16 c0 = To565(e0), c1 = To565(e1);
        cands.emplace_back(c0, c1);
        if (c0 == c1) {
            break;
        }
        // Least squares for the two endpoints on the current assignment (4-colour weights).
        const Scored s = Score(px, c0, c1);
        static constexpr std::array<double, 4> Weight = {1.0, 0.0, 2.0 / 3.0, 1.0 / 3.0};
        double m00 = 0, m01 = 0, m11 = 0;
        std::array<double, 3> r0{}, r1{};
        for (int i = 0; i < 16; ++i) {
            const double w = Weight[static_cast<std::size_t>(s.idx[i])], u = 1 - w;
            m00 += w * w;
            m01 += w * u;
            m11 += u * u;
            for (int k = 0; k < 3; ++k) {
                r0[k] += w * px[i][k];
                r1[k] += u * px[i][k];
            }
        }
        std::array<double, 3> sol0{}, sol1{};
        const double det = m00 * m11 - m01 * m01;
        if (std::abs(det) > 1e-9) {
            for (int k = 0; k < 3; ++k) {
                sol0[k] = (m11 * r0[k] - m01 * r1[k]) / det;
                sol1[k] = (m00 * r1[k] - m01 * r0[k]) / det;
            }
        } else {
            // Every texel on one weight: the minimum-norm solution.
            const double w = Weight[static_cast<std::size_t>(s.idx[0])], u = 1 - w;
            for (int k = 0; k < 3; ++k) {
                const double target = mean[k] / (w * w + u * u);
                sol0[k] = w * target;
                sol1[k] = u * target;
            }
        }
        if (c0 > c1) {
            e0 = sol0;
            e1 = sol1;
        } else {
            e0 = sol1;
            e1 = sol0;
        }
    }
    Scored best{};
    bool have = false;
    for (const auto& [c0, c1] : cands) {
        const Scored s = Score(px, c0, c1);
        if (!have || s.err < best.err) {
            best = s;
            have = true;
        }
    }
    // Local search: +-1 step of either endpoint's channels while it lowers the error.
    std::vector<u32> seen;
    const auto visited = [&seen](u16 a, u16 b) {
        const u32 key = (static_cast<u32>(a) << 16) | b;
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) {
            return true;
        }
        seen.push_back(key);
        return false;
    };
    static constexpr std::array<int, 6> Steps = {1 << 11, 1 << 5, 1, -(1 << 11), -(1 << 5), -1};
    bool improved = true;
    while (improved) {
        improved = false;
        for (int which = 0; which < 2; ++which) {
            for (const int st : Steps) {
                std::array<int, 2> c = {best.hi, best.lo};
                const int ov = c[static_cast<std::size_t>(which)];
                const int nv = ov + st;
                if (nv < 0 || nv > 0xFFFF) {
                    continue;
                }
                const int dir = st > 0 ? 1 : -1;
                if ((st == (1 << 11) || st == -(1 << 11)) && !(((ov >> 11) & 31) + dir >= 0 &&
                                                               ((ov >> 11) & 31) + dir <= 31)) {
                    continue;
                }
                if ((st == (1 << 5) || st == -(1 << 5)) &&
                    !(((ov >> 5) & 63) + dir >= 0 && ((ov >> 5) & 63) + dir <= 63)) {
                    continue;
                }
                if ((st == 1 || st == -1) && !((ov & 31) + st >= 0 && (ov & 31) + st <= 31)) {
                    continue;
                }
                c[static_cast<std::size_t>(which)] = nv;
                if (visited(static_cast<u16>(c[0]), static_cast<u16>(c[1]))) {
                    continue;
                }
                const Scored s = Score(px, static_cast<u16>(c[0]), static_cast<u16>(c[1]));
                if (s.err < best.err - 1e-9) {
                    best = s;
                    improved = true;
                }
            }
        }
    }
    if (best.hi == best.lo) {
        best.idx.fill(0);
    }
    u32 bits = 0;
    for (int i = 0; i < 16; ++i) {
        bits |= static_cast<u32>(best.idx[i]) << (2 * i);
    }
    return {static_cast<u8>(best.hi & 0xFF), static_cast<u8>(best.hi >> 8),
            static_cast<u8>(best.lo & 0xFF), static_cast<u8>(best.lo >> 8),
            static_cast<u8>(bits & 0xFF),     static_cast<u8>((bits >> 8) & 0xFF),
            static_cast<u8>((bits >> 16) & 0xFF), static_cast<u8>(bits >> 24)};
}

// Size x Size RGB -> CellBytes of BC1 blocks (row by row).
inline std::vector<u8> EncodeCell(std::span<const u8> rgb) {
    std::vector<u8> out;
    out.reserve(CellBytes);
    for (int by = 0; by < Blocks; ++by) {
        for (int bx = 0; bx < Blocks; ++bx) {
            double px[16][3];
            for (int i = 0; i < 16; ++i) {
                const std::size_t o =
                    (static_cast<std::size_t>(by * 4 + i / 4) * Size + (bx * 4 + i % 4)) * 3;
                for (int k = 0; k < 3; ++k) {
                    px[i][k] = rgb[o + static_cast<std::size_t>(k)];
                }
            }
            const auto b = EncodeBlock(px);
            out.insert(out.end(), b.begin(), b.end());
        }
    }
    return out;
}

// ---- Resampling (Pillow's kernels and coefficient rule) --------------------------------------

namespace Detail {

inline double Bicubic(double x) {
    constexpr double a = -0.5;
    x = std::abs(x);
    if (x < 1.0) {
        return ((a + 2.0) * x - (a + 3.0)) * x * x + 1.0;
    }
    if (x < 2.0) {
        return (((x - 5.0) * x + 8.0) * x - 4.0) * a;
    }
    return 0.0;
}

inline double Sinc(double x) {
    if (x == 0.0) {
        return 1.0;
    }
    x *= 3.14159265358979323846;
    return std::sin(x) / x;
}

inline double Lanczos(double x) {
    return x >= -3.0 && x < 3.0 ? Sinc(x) * Sinc(x / 3.0) : 0.0;
}

// out x in weights: each output sample's normalised kernel over the inputs (Pillow's
// precompute_coeffs: centre (o + 0.5) * scale, support scaled when downsampling).
template <typename Kernel>
std::vector<double> Coefficients(int in, int out, Kernel kernel, double support) {
    std::vector<double> w(static_cast<std::size_t>(out) * in, 0.0);
    const double scale = static_cast<double>(in) / out;
    const double fs = std::max(scale, 1.0);
    const double sup = support * fs;
    for (int o = 0; o < out; ++o) {
        const double centre = (o + 0.5) * scale;
        const int lo = std::max(static_cast<int>(centre - sup + 0.5), 0);
        const int hi = std::min(static_cast<int>(centre + sup + 0.5), in);
        double total = 0.0;
        for (int x = lo; x < hi; ++x) {
            total += kernel((x - centre + 0.5) / fs);
        }
        for (int x = lo; x < hi; ++x) {
            w[static_cast<std::size_t>(o) * in + x] =
                total != 0.0 ? kernel((x - centre + 0.5) / fs) / total : 0.0;
        }
    }
    return w;
}

// The x4 bicubic up, / 4 Lanczos down round trip as one Size x Size matrix.
inline std::vector<double> RoundTrip() {
    constexpr int Hi = Size * 4;
    const auto up = Coefficients(Size, Hi, Bicubic, 2.0);    // Hi x Size
    const auto down = Coefficients(Hi, Size, Lanczos, 3.0);  // Size x Hi
    std::vector<double> f(static_cast<std::size_t>(Size) * Size, 0.0);
    for (int o = 0; o < Size; ++o) {
        for (int h = 0; h < Hi; ++h) {
            const double d = down[static_cast<std::size_t>(o) * Hi + h];
            if (d == 0.0) {
                continue;
            }
            for (int i = 0; i < Size; ++i) {
                f[static_cast<std::size_t>(o) * Size + i] += d * up[static_cast<std::size_t>(h) * Size + i];
            }
        }
    }
    return f;
}

// A separable Gaussian (sigma 1 px, radius 3, edges clamped): first down the columns, then
// along the rows.
inline std::vector<double> Blur(const std::vector<double>& x) {
    std::array<double, 7> k{};
    double total = 0.0;
    for (int i = -3; i <= 3; ++i) {
        k[static_cast<std::size_t>(i + 3)] = std::exp(-0.5 * i * i);
        total += k[static_cast<std::size_t>(i + 3)];
    }
    for (double& v : k) {
        v /= total;
    }
    const auto clampi = [](int i) { return std::clamp(i, 0, Size - 1); };
    std::vector<double> a(x.size(), 0.0), b(x.size(), 0.0);
    for (int y = 0; y < Size; ++y) {
        for (int xx = 0; xx < Size; ++xx) {
            for (int c = 0; c < 3; ++c) {
                double s = 0.0;
                for (int j = 0; j < 7; ++j) {
                    s += k[static_cast<std::size_t>(j)] *
                         x[(static_cast<std::size_t>(clampi(y + j - 3)) * Size + xx) * 3 + c];
                }
                a[(static_cast<std::size_t>(y) * Size + xx) * 3 + c] = s;
            }
        }
    }
    for (int y = 0; y < Size; ++y) {
        for (int xx = 0; xx < Size; ++xx) {
            for (int c = 0; c < 3; ++c) {
                double s = 0.0;
                for (int j = 0; j < 7; ++j) {
                    s += k[static_cast<std::size_t>(j)] *
                         a[(static_cast<std::size_t>(y) * Size + clampi(xx + j - 3)) * 3 + c];
                }
                b[(static_cast<std::size_t>(y) * Size + xx) * 3 + c] = s;
            }
        }
    }
    return b;
}

} // namespace Detail

// Steps 2-4: the decoded parchment (Size x Size RGB) -> the icon (Size x Size RGB).
inline std::vector<u8> Compose(std::span<const u8> ground) {
    using namespace Detail;
    const auto f = RoundTrip();
    std::vector<double> g(ground.begin(), ground.end());
    // Rows, then columns: F G F^T per channel.
    std::vector<double> t(g.size(), 0.0), gb(g.size(), 0.0);
    for (int y = 0; y < Size; ++y) {
        for (int o = 0; o < Size; ++o) {
            for (int c = 0; c < 3; ++c) {
                double s = 0.0;
                for (int i = 0; i < Size; ++i) {
                    s += f[static_cast<std::size_t>(o) * Size + i] *
                         g[(static_cast<std::size_t>(y) * Size + i) * 3 + c];
                }
                t[(static_cast<std::size_t>(y) * Size + o) * 3 + c] = s;
            }
        }
    }
    for (int o = 0; o < Size; ++o) {
        for (int xx = 0; xx < Size; ++xx) {
            for (int c = 0; c < 3; ++c) {
                double s = 0.0;
                for (int i = 0; i < Size; ++i) {
                    s += f[static_cast<std::size_t>(o) * Size + i] *
                         t[(static_cast<std::size_t>(i) * Size + xx) * 3 + c];
                }
                gb[(static_cast<std::size_t>(o) * Size + xx) * 3 + c] = s;
            }
        }
    }
    // The layer over it, rounded to the 8-bit image the sharpening sees.
    std::vector<double> img(g.size());
    for (std::size_t p = 0; p < static_cast<std::size_t>(Size) * Size; ++p) {
        const double m = GfxTile::GroundLo +
                         GfxTile::GogglesGround[p] / 255.0 * (GfxTile::GroundHi - GfxTile::GroundLo);
        for (std::size_t c = 0; c < 3; ++c) {
            const double col =
                GfxTile::ColourLo +
                GfxTile::GogglesColour[p * 3 + c] / 255.0 * (GfxTile::ColourHi - GfxTile::ColourLo);
            img[p * 3 + c] = std::clamp(std::nearbyint(gb[p * 3 + c] * m + col), 0.0, 255.0);
        }
    }
    // Unsharp mask: sigma 1 px, 60 %, threshold 2.
    const auto blurred = Blur(img);
    std::vector<u8> out(img.size());
    for (std::size_t i = 0; i < img.size(); ++i) {
        const double d = img[i] - std::nearbyint(blurred[i]);
        const double v = std::abs(d) >= 2.0 ? img[i] + d * 0.6 : img[i];
        out[i] = static_cast<u8>(std::clamp(std::nearbyint(v), 0.0, 255.0));
    }
    return out;
}

// The parchment's plausibility (the cell must hold the game's parchment, not an unused black or a
// mod's flat colour): its mean luminance and spread.
inline std::string CheckGround(std::span<const u8> ground) {
    double sum = 0.0, sum2 = 0.0;
    const std::size_t n = static_cast<std::size_t>(Size) * Size;
    for (std::size_t p = 0; p < n; ++p) {
        const double l = 0.299 * ground[p * 3] + 0.587 * ground[p * 3 + 1] + 0.114 * ground[p * 3 + 2];
        sum += l;
        sum2 += l * l;
    }
    const double mean = sum / static_cast<double>(n);
    const double sd = std::sqrt(std::max(sum2 / static_cast<double>(n) - mean * mean, 0.0));
    if (mean < 40.0 || mean > 240.0 || sd < 2.0) {
        return "the parchment cell does not look like parchment (mean " +
               std::to_string(static_cast<int>(mean)) + ", spread " +
               std::to_string(static_cast<int>(sd)) + ")";
    }
    return {};
}

// The whole of it: the parchment cell's BC1 blocks (row by row) -> the icon's BC1 blocks, or
// empty and why.
inline std::vector<u8> Build(std::span<const u8> parchment_blocks, std::string& why) {
    if (parchment_blocks.size() != CellBytes) {
        why = "the parchment cell is not 80 x 80 BC1";
        return {};
    }
    const auto ground = DecodeCell(parchment_blocks);
    why = CheckGround(ground);
    if (!why.empty()) {
        return {};
    }
    return EncodeCell(Compose(ground));
}

} // namespace VR::TileIcon
