// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstring>
#include <numbers>

#include "video_core/texture_cache/bump_height_derive.h"

namespace VideoCore::BumpHeight {

namespace {

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using Cx = std::complex<float>;

// The slopes are held under this (a normal near the horizon would dominate the surface).
constexpr float MaxSlope = 4.0f;
// The stones' tops: the height's maximum within the size / EnvelopeDiv texels round a texel,
// smoothed; the depth under it as a share of its 97th percentile, DeadZone of it a face still.
constexpr u32 EnvelopeDiv = 32;
constexpr float TopShare = 0.97f;
constexpr float DeadZone = 0.25f;
// Stonework (measured on Central and Old Yharnam's 383 flat bump maps, each looked at: 34 pass,
// all bricks, cobbles and tiles): on a SelectSize copy, faces (depth under FaceDepth) joined into
// stones; a stone is a face of StoneMin to StoneMax of the area that fills StoneFill of the box
// round it. Many stones that are most of the faces, or a few that are all of them (large tiles).
constexpr u32 SelectSize = 128;
constexpr u32 SelectWork = 256;
constexpr float FaceDepth = 0.08f;
constexpr float JointDepth = 0.35f;
constexpr float StoneMin = 0.0005f;
constexpr float StoneMax = 0.15f;
constexpr float StoneFill = 0.6f;
constexpr u32 ManyStones = 30;
constexpr float ManyShare = 0.6f;
constexpr float ManyFace = 0.25f;
constexpr u32 FewStones = 9;
constexpr float FewShare = 0.95f;
constexpr float FewFace = 0.6f;
constexpr float MinJoint = 0.10f;

// exp(-2 pi i k / n), k < n / 2.
std::vector<Cx> Twiddles(u32 n) {
    std::vector<Cx> tw(n / 2);
    for (u32 k = 0; k < n / 2; ++k) {
        const double a = -2.0 * std::numbers::pi * k / n;
        tw[k] = Cx{static_cast<float>(std::cos(a)), static_cast<float>(std::sin(a))};
    }
    return tw;
}

// In place, n a power of two, the values `stride` apart.
void Fft(Cx* a, u32 n, u32 stride, bool inverse, const std::vector<Cx>& tw) {
    for (u32 i = 1, j = 0; i < n; ++i) {
        u32 bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(a[i * stride], a[j * stride]);
        }
    }
    for (u32 len = 2; len <= n; len <<= 1) {
        const u32 half = len / 2, step = n / len;
        for (u32 i = 0; i < n; i += len) {
            for (u32 k = 0; k < half; ++k) {
                const Cx w = inverse ? std::conj(tw[k * step]) : tw[k * step];
                Cx& lo = a[(i + k) * stride];
                Cx& hi = a[(i + k + half) * stride];
                const Cx v = hi * w;
                hi = lo - v;
                lo = lo + v;
            }
        }
    }
    if (inverse) {
        const float scale = 1.0f / static_cast<float>(n);
        for (u32 i = 0; i < n; ++i) {
            a[i * stride] *= scale;
        }
    }
}

void Fft2(std::vector<Cx>& a, u32 n, bool inverse, const std::vector<Cx>& tw) {
    for (u32 y = 0; y < n; ++y) {
        Fft(&a[y * n], n, 1, inverse, tw);
    }
    // A column at a time through a row of its own (a column in place strides a row per value).
    std::vector<Cx> column(n);
    for (u32 x = 0; x < n; ++x) {
        for (u32 y = 0; y < n; ++y) {
            column[y] = a[y * n + x];
        }
        Fft(column.data(), n, 1, inverse, tw);
        for (u32 y = 0; y < n; ++y) {
            a[y * n + x] = column[y];
        }
    }
}

// A cycle per sample of bin k of n.
float Frequency(u32 k, u32 n) {
    return (k < n / 2 ? static_cast<float>(k) : static_cast<float>(k) - static_cast<float>(n)) /
           static_cast<float>(n);
}

// Where the tiled block `g` of a level `pitch_blocks` wide lies: 8x8-block micro tiles, row
// major, Morton order inside (the detiler's own: host_shaders/detilers/micro_64bpp.comp).
void BlockPlace(u32 g, u32 pitch_blocks, u32& bx, u32& by) {
    const u32 tile = g / 64, e = g % 64, per_row = pitch_blocks / 8;
    bx = (tile % per_row) * 8 + ((e & 1) | ((e >> 1) & 2) | ((e >> 2) & 4));
    by = (tile / per_row) * 8 + (((e >> 1) & 1) | ((e >> 2) & 2) | ((e >> 3) & 4));
}

struct Block {
    u16 c0, c1;
    u32 indices;
    bool four;
};

Block ReadBlock(const u8* p) {
    Block b;
    std::memcpy(&b.c0, p, 2);
    std::memcpy(&b.c1, p + 2, 2);
    std::memcpy(&b.indices, p + 4, 4);
    b.four = b.c0 > b.c1;
    return b;
}

bool Usable(const Layout& l, u64 size) {
    if (l.size < MinSize || l.size > MaxSize || (l.size & (l.size - 1)) != 0 || l.levels == 0 ||
        l.levels > MaxMips) {
        return false;
    }
    for (u32 m = 0; m < l.levels; ++m) {
        const Mip& mip = l.mips[m];
        const u32 blocks = (l.size >> m) / 4;
        if ((l.size >> m) < MinMipSize || mip.pitch_blocks < 8 || mip.pitch_blocks % 8 != 0 ||
            mip.height_blocks % 8 != 0 || blocks > mip.pitch_blocks ||
            blocks > mip.height_blocks ||
            u64{mip.pitch_blocks} * mip.height_blocks * 8 > mip.bytes ||
            u64{mip.offset} + mip.bytes > size) {
            return false;
        }
    }
    return true;
}

// The normal's red and green of level 0, averaged down to n x n.
void ReadNormals(const u8* bytes, const Layout& l, u32 n, std::vector<float>& red,
                 std::vector<float>& green) {
    const Mip& mip = l.mips[0];
    const u32 k = l.size / n, blocks = l.size / 4;
    const u32 count = mip.pitch_blocks * mip.height_blocks;
    for (u32 g = 0; g < count; ++g) {
        u32 bx, by;
        BlockPlace(g, mip.pitch_blocks, bx, by);
        if (bx >= blocks || by >= blocks) {
            continue;
        }
        const Block b = ReadBlock(bytes + mip.offset + u64{g} * 8);
        const auto r8 = [](u16 c) { const u32 v = (c >> 11) & 31u; return (v << 3) | (v >> 2); };
        const auto g8 = [](u16 c) { const u32 v = (c >> 5) & 63u; return (v << 2) | (v >> 4); };
        const u32 r0 = r8(b.c0), r1 = r8(b.c1), g0 = g8(b.c0), g1 = g8(b.c1);
        const std::array<u32, 4> pr{r0, r1, b.four ? (2 * r0 + r1) / 3 : (r0 + r1) / 2,
                                    b.four ? (r0 + 2 * r1) / 3 : (r0 + r1) / 2};
        const std::array<u32, 4> pg{g0, g1, b.four ? (2 * g0 + g1) / 3 : (g0 + g1) / 2,
                                    b.four ? (g0 + 2 * g1) / 3 : (g0 + g1) / 2};
        for (u32 t = 0; t < 16; ++t) {
            const u32 s = (b.indices >> (2 * t)) & 3u;
            const u32 x = (bx * 4 + (t & 3)) / k, y = (by * 4 + (t >> 2)) / k;
            red[y * n + x] += static_cast<float>(pr[s]);
            green[y * n + x] += static_cast<float>(pg[s]);
        }
    }
    const float scale = 1.0f / static_cast<float>(k * k);
    for (u32 i = 0; i < n * n; ++i) {
        red[i] *= scale;
        green[i] *= scale;
    }
}

// The depth below the stones' tops, 0 (a face) to 1, n x n; false: no relief at all.
bool Depth(const std::vector<float>& red, const std::vector<float>& green, u32 n,
           std::vector<float>& depth, float& top) {
    const u32 count = n * n;
    const std::vector<Cx> tw = Twiddles(n);
    std::vector<Cx> p(count), q(count);
    double mean_p = 0.0, mean_q = 0.0;
    for (u32 i = 0; i < count; ++i) {
        const float x = red[i] * (2.0f / 255.0f) - 1.0f;
        const float y = green[i] * (2.0f / 255.0f) - 1.0f;
        const float z = std::sqrt(std::clamp(1.0f - x * x - y * y, 0.02f, 1.0f));
        const float sp = std::clamp(-x / z, -MaxSlope, MaxSlope);
        const float sq = std::clamp(-y / z, -MaxSlope, MaxSlope);
        p[i] = sp;
        q[i] = sq;
        mean_p += sp;
        mean_q += sq;
    }
    const float mp = static_cast<float>(mean_p / count), mq = static_cast<float>(mean_q / count);
    for (u32 i = 0; i < count; ++i) {
        p[i] -= mp;
        q[i] -= mq;
    }
    // The least-squares surface of the slopes: H = (-i u P - i v Q) / (u^2 + v^2).
    Fft2(p, n, false, tw);
    Fft2(q, n, false, tw);
    const float two_pi = 2.0f * std::numbers::pi_v<float>;
    for (u32 y = 0; y < n; ++y) {
        const float v = two_pi * Frequency(y, n);
        for (u32 x = 0; x < n; ++x) {
            const float u = two_pi * Frequency(x, n);
            const float d = u * u + v * v;
            const u32 i = y * n + x;
            p[i] = d > 0.0f ? (Cx{0.0f, -u} * p[i] + Cx{0.0f, -v} * q[i]) / d : Cx{};
        }
    }
    Fft2(p, n, true, tw);
    std::vector<float> height(count);
    for (u32 i = 0; i < count; ++i) {
        height[i] = p[i].real();
    }
    // The tops: the maximum within r, wrapping, then smoothed (a Gaussian of r / 2).
    const u32 r = std::max(3u, n / EnvelopeDiv);
    std::vector<float> rows(count), env(count);
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            float m = height[y * n + x];
            for (u32 d = 1; d <= r; ++d) {
                m = std::max(m, std::max(height[y * n + ((x + d) & (n - 1))],
                                         height[y * n + ((x + n - d) & (n - 1))]));
            }
            rows[y * n + x] = m;
        }
    }
    for (u32 y = 0; y < n; ++y) {
        for (u32 x = 0; x < n; ++x) {
            float m = rows[y * n + x];
            for (u32 d = 1; d <= r; ++d) {
                m = std::max(m, std::max(rows[((y + d) & (n - 1)) * n + x],
                                         rows[((y + n - d) & (n - 1)) * n + x]));
            }
            env[y * n + x] = m;
        }
    }
    for (u32 i = 0; i < count; ++i) {
        q[i] = env[i];
    }
    Fft2(q, n, false, tw);
    const float sigma = static_cast<float>(r) * 0.5f;
    const float k = -2.0f * std::numbers::pi_v<float> * std::numbers::pi_v<float> * sigma * sigma;
    for (u32 y = 0; y < n; ++y) {
        const float fy = Frequency(y, n);
        for (u32 x = 0; x < n; ++x) {
            const float fx = Frequency(x, n);
            q[y * n + x] *= std::exp(k * (fx * fx + fy * fy));
        }
    }
    Fft2(q, n, true, tw);
    depth.resize(count);
    for (u32 i = 0; i < count; ++i) {
        const float d = q[i].real() - height[i];
        depth[i] = std::isfinite(d) ? std::max(d, 0.0f) : 0.0f;
    }
    std::vector<float> sorted = depth;
    const auto nth = sorted.begin() + static_cast<std::ptrdiff_t>(TopShare * (count - 1));
    std::nth_element(sorted.begin(), nth, sorted.end());
    top = *nth;
    if (!(top > 1e-4f)) {
        return false;
    }
    for (float& d : depth) {
        d = std::clamp((d / top - DeadZone) / (1.0f - DeadZone), 0.0f, 1.0f);
    }
    return true;
}

// Stonework or not (the constants above).
bool Stonework(const std::vector<float>& depth, u32 n, Stats& stats) {
    constexpr u32 N = SelectSize;
    const u32 f = n / N;
    std::vector<float> d(N * N);
    for (u32 y = 0; y < N; ++y) {
        for (u32 x = 0; x < N; ++x) {
            float sum = 0.0f;
            for (u32 j = 0; j < f; ++j) {
                for (u32 i = 0; i < f; ++i) {
                    sum += depth[(y * f + j) * n + x * f + i];
                }
            }
            d[y * N + x] = sum / static_cast<float>(f * f);
        }
    }
    std::vector<u32> parent(N * N);
    for (u32 i = 0; i < N * N; ++i) {
        parent[i] = i;
    }
    const auto find = [&](u32 x) {
        while (parent[x] != x) {
            parent[x] = parent[parent[x]];
            x = parent[x];
        }
        return x;
    };
    u32 faces = 0, joints = 0;
    for (u32 y = 0; y < N; ++y) {
        for (u32 x = 0; x < N; ++x) {
            const u32 i = y * N + x;
            joints += d[i] > JointDepth;
            if (!(d[i] < FaceDepth)) {
                continue;
            }
            ++faces;
            for (const u32 j : {y * N + ((x + 1) & (N - 1)), ((y + 1) & (N - 1)) * N + x}) {
                if (d[j] < FaceDepth) {
                    const u32 a = find(i), b = find(j);
                    if (a != b) {
                        parent[a] = b;
                    }
                }
            }
        }
    }
    // Every face's texels together: sorted by root.
    std::vector<u32> order;
    order.reserve(faces);
    for (u32 i = 0; i < N * N; ++i) {
        if (d[i] < FaceDepth) {
            order.push_back(i);
        }
    }
    std::vector<u32> root(N * N);
    for (const u32 i : order) {
        root[i] = find(i);
    }
    std::ranges::sort(order, [&](u32 a, u32 b) { return root[a] < root[b]; });
    // The shortest span of an axis that holds a face, wrapping.
    const auto span = [](const std::array<bool, N>& present) {
        u32 longest = 0, run = 0;
        for (u32 i = 0; i < 2 * N; ++i) {
            run = present[i & (N - 1)] ? 0 : run + 1;
            longest = std::max(longest, run);
        }
        return longest >= N ? 0u : N - longest;
    };
    u32 stones = 0, stone_area = 0;
    for (size_t at = 0; at < order.size();) {
        size_t end = at;
        std::array<bool, N> ys{}, xs{};
        while (end < order.size() && root[order[end]] == root[order[at]]) {
            ys[order[end] / N] = true;
            xs[order[end] % N] = true;
            ++end;
        }
        const u32 area = static_cast<u32>(end - at);
        at = end;
        if (static_cast<float>(area) < StoneMin * N * N ||
            static_cast<float>(area) > StoneMax * N * N) {
            continue;
        }
        const u32 box = span(ys) * span(xs);
        if (box != 0 && static_cast<float>(area) >= StoneFill * static_cast<float>(box)) {
            ++stones;
            stone_area += area;
        }
    }
    stats.face = static_cast<float>(faces) / (N * N);
    stats.joint = static_cast<float>(joints) / (N * N);
    stats.stones = stones;
    stats.stone_share = faces != 0 ? static_cast<float>(stone_area) / static_cast<float>(faces)
                                   : 0.0f;
    if (stats.joint < MinJoint) {
        return false;
    }
    return (stones >= ManyStones && stats.stone_share >= ManyShare && stats.face >= ManyFace) ||
           (stones >= FewStones && stats.stone_share >= FewShare && stats.face >= FewFace);
}

// The blue wanted at a level `s` texels wide, from the n x n one.
std::vector<float> Resample(const std::vector<float>& blue, u32 n, u32 s) {
    std::vector<float> out(s * s);
    if (s == n) {
        return blue;
    }
    if (s > n) { // twice the size: between the four nearest, wrapping
        for (u32 y = 0; y < s; ++y) {
            const float fy = (static_cast<float>(y) + 0.5f) * static_cast<float>(n) / s - 0.5f;
            const float y0f = std::floor(fy);
            const u32 y0 = static_cast<u32>(static_cast<int>(y0f) + static_cast<int>(n)) & (n - 1);
            const u32 y1 = (y0 + 1) & (n - 1);
            const float wy = fy - y0f;
            for (u32 x = 0; x < s; ++x) {
                const float fx = (static_cast<float>(x) + 0.5f) * static_cast<float>(n) / s - 0.5f;
                const float x0f = std::floor(fx);
                const u32 x0 =
                    static_cast<u32>(static_cast<int>(x0f) + static_cast<int>(n)) & (n - 1);
                const u32 x1 = (x0 + 1) & (n - 1);
                const float wx = fx - x0f;
                out[y * s + x] =
                    (blue[y0 * n + x0] * (1 - wx) + blue[y0 * n + x1] * wx) * (1 - wy) +
                    (blue[y1 * n + x0] * (1 - wx) + blue[y1 * n + x1] * wx) * wy;
            }
        }
        return out;
    }
    const u32 f = n / s;
    for (u32 y = 0; y < s; ++y) {
        for (u32 x = 0; x < s; ++x) {
            float sum = 0.0f;
            for (u32 j = 0; j < f; ++j) {
                for (u32 i = 0; i < f; ++i) {
                    sum += blue[(y * f + j) * n + x * f + i];
                }
            }
            out[y * s + x] = sum / static_cast<float>(f * f);
        }
    }
    return out;
}

// A level's blocks take the blue wanted: each block's two 5-bit blues fitted (least squares) to
// its sixteen texels through the indices it has; red and green are not touched, nor the block's
// mode (four colours, or three and a transparent one).
void WriteBlue(u8* bytes, const Mip& mip, u32 s, const std::vector<float>& want) {
    const u32 blocks = s / 4, count = mip.pitch_blocks * mip.height_blocks;
    for (u32 g = 0; g < count; ++g) {
        u32 bx, by;
        BlockPlace(g, mip.pitch_blocks, bx, by);
        if (bx >= blocks || by >= blocks) {
            continue;
        }
        u8* const at = bytes + mip.offset + u64{g} * 8;
        const Block b = ReadBlock(at);
        float a11 = 0, a12 = 0, a22 = 0, b1 = 0, b2 = 0, sum = 0;
        u32 used = 0;
        for (u32 t = 0; t < 16; ++t) {
            const u32 sel = (b.indices >> (2 * t)) & 3u;
            if (!b.four && sel == 3) {
                continue; // the transparent texel: black whatever the endpoints
            }
            static constexpr std::array<float, 4> Four{1.0f, 0.0f, 2.0f / 3.0f, 1.0f / 3.0f};
            static constexpr std::array<float, 4> Three{1.0f, 0.0f, 0.5f, 0.0f};
            const float w = b.four ? Four[sel] : Three[sel];
            const float v = want[(by * 4 + (t >> 2)) * s + bx * 4 + (t & 3)];
            a11 += w * w;
            a12 += w * (1 - w);
            a22 += (1 - w) * (1 - w);
            b1 += w * v;
            b2 += (1 - w) * v;
            sum += v;
            ++used;
        }
        if (used == 0) {
            continue;
        }
        const float mean = sum / static_cast<float>(used);
        const float det = a11 * a22 - a12 * a12;
        float e0 = mean, e1 = mean;
        if (det > 1e-4f) {
            e0 = (b1 * a22 - b2 * a12) / det;
            e1 = (a11 * b2 - a12 * b1) / det;
        }
        const auto five = [](float v) {
            return static_cast<u32>(std::clamp(std::lround(v * (31.0f / 255.0f)), 0l, 31l));
        };
        u32 q0 = five(e0), q1 = five(e1);
        // Same red and green in both endpoints: blue alone orders them, and the order is the
        // block's mode.
        if ((b.c0 >> 5) == (b.c1 >> 5)) {
            if (b.four) {
                if (q0 <= q1) {
                    q0 = q1 = five(mean);
                    if (q1 > 0) {
                        --q1;
                    } else {
                        q0 = 1;
                    }
                }
            } else if (q0 > q1) {
                q0 = q1 = five(mean);
            }
        }
        const u16 c0 = static_cast<u16>((b.c0 & ~31u) | q0);
        const u16 c1 = static_cast<u16>((b.c1 & ~31u) | q1);
        std::memcpy(at, &c0, 2);
        std::memcpy(at + 2, &c1, 2);
    }
}

} // namespace

bool Derive(std::uint8_t* bytes, std::uint64_t size, const Layout& layout, Stats& stats) {
    stats = {};
    if (bytes == nullptr || !Usable(layout, size)) {
        return false;
    }
    // Stonework or not on a SelectWork copy first (nine textures in ten are not: a quarter of
    // the work), then the height at full working size.
    u32 n = std::min(layout.size, SelectWork);
    std::vector<float> red(n * n), green(n * n), depth;
    ReadNormals(bytes, layout, n, red, green);
    float top = 0.0f;
    if (!Depth(red, green, n, depth, top)) {
        return false;
    }
    stats.relief = top * static_cast<float>(layout.size / n);
    if (!Stonework(depth, n, stats)) {
        return false;
    }
    if (std::min(layout.size, WorkSize) > n) {
        n = std::min(layout.size, WorkSize);
        red.assign(n * n, 0.0f);
        green.assign(n * n, 0.0f);
        ReadNormals(bytes, layout, n, red, green);
        if (!Depth(red, green, n, depth, top)) {
            return false;
        }
    }
    for (float& d : depth) {
        d = 255.0f - d * (255.0f - FloorBlue); // the blue wanted
    }
    for (u32 m = 0; m < layout.levels; ++m) {
        const u32 s = layout.size >> m;
        WriteBlue(bytes, layout.mips[m], s, Resample(depth, n, s));
    }
    return true;
}

} // namespace VideoCore::BumpHeight
