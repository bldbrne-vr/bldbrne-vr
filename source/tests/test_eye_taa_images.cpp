// SPDX-License-Identifier: GPL-2.0-or-later

// CPU image tests for vr_eye_taa.comp, NOT GPU/headset benchmarks: grayscale scenes, scored
// against independently area-sampled geometry (16x16 supersampled). "Spatial" is the squared
// error against it, "temporal" the frame-to-frame change of that error (crawl, shimmer); lower is
// better. Treat the numbers as rankings, not amounts (the mock A/B is the real check).
// Filters, each a straight port:
//   None      - this refresh only (what the baseline filter shows while the game camera walks or turns)
//   Shipping  - the baseline filter: Catmull-Rom history, 3x3 colour clamp, 0.85
//   Branch    - the depth-gated comparison filter: 2% depth match, 25% cap at depth edges
//   Follow    - eye_taa_follow: VrEyeTaa::Choose per frame; a still world runs Shipping exactly,
//               a moving one the parallax-gated path (VrEyeTaa::MovingWeights, ParallaxEdge)
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <vector>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vr_eye_taa_math.h"

namespace {
namespace T = Vulkan::VrEyeTaa;
constexpr int Width = 96, Height = 64;
constexpr float Taa = 0.85f;
enum class Filter { None, Shipping, Branch, Follow };
struct Pixel {
    float c{}, w{}; // colour; 1/z in metres (eye_w as DecodeEyeW gives it; the history's alpha)
    bool clamp_only{false};
};
using Image = std::vector<Pixel>;

// Projected motion per refresh of a surface 20 m away (1/z = 0.05). `rotation`: every depth moves
// alike (head turning, the world still); else a sideways translation (walking, strafing): a
// surface at 1/z w moves dx * w / 0.05.
struct Motion {
    float dx{}, dy{};
    bool rotation{false};
};
struct Options { // vr.ini eye_taa_gate_px, eye_taa_edge_cap (defaults)
    float gate{0.05f};
    float cap{0.5f};
    bool half{false}; // the history rounded to RGBA16F, as on the GPU
};
struct Counts {
    long history{}, rejected{}, bilinear{}, capped{};
};

const Pixel& At(const Image& image, int x, int y) {
    return image[std::clamp(y, 0, Height - 1) * Width + std::clamp(x, 0, Width - 1)];
}

std::array<float, 4> Cubic(float f) {
    return {((-0.5f * f + 1) * f - 0.5f) * f, (1.5f * f - 2.5f) * f * f + 1,
            ((-1.5f * f + 2) * f + 0.5f) * f, (0.5f * f - 0.5f) * f * f};
}

float Half(float x) {
    if (x == 0.0f || !std::isfinite(x)) {
        return x;
    }
    int e = 0;
    const float m = std::frexp(x, &e);
    return std::ldexp(std::nearbyint(std::ldexp(m, 11)), e - 11);
}

// The depth-gated comparison filter, for reference only.
namespace Branch {
bool DepthMatches(float stored, float expected) {
    if (stored < 0.0f || expected < 0.0f) {
        return false;
    }
    if (stored == 0.0f || expected == 0.0f) {
        return stored == expected;
    }
    return std::abs(stored - expected) <= 0.02f * std::max(stored, expected);
}
float HistoryWeights(const std::array<float, 4>& depth, float expected, float x, float y,
                     std::array<float, 4>& weights, bool current_edge) {
    weights = {(1 - x) * (1 - y), x * (1 - y), (1 - x) * y, x * y};
    float sum = 0;
    bool reliable = true;
    for (int i = 0; i < 4; ++i) {
        sum += DepthMatches(depth[i], expected) ? weights[i] : 0.0f;
        reliable &= weights[i] == 0.0f || depth[i] >= 0.0f;
    }
    if (sum < 0.25f)
        return 0.0f;
    if (current_edge && reliable && sum < 0.999f)
        return 0.25f;
    for (int i = 0; i < 4; ++i)
        weights[i] = DepthMatches(depth[i], expected) ? weights[i] / sum : 0.0f;
    return current_edge ? 0.25f : 1.0f;
}
} // namespace Branch

// One refresh of `filter`: `cur` blended with `prev` (the last result). `moving`: Follow's
// FLAG_MOVING (VrEyeTaa::Choose said the world moved).
Image Accumulate(const Image& cur, const Image& prev, Motion m, Filter filter, const Options& o,
                 bool moving, Counts* counts = nullptr) {
    Image out = cur;
    for (int y = 0; y < Height; ++y) {
        for (int x = 0; x < Width; ++x) {
            const Pixel& p = At(cur, x, y);
            Pixel& r = out[y * Width + x];
            // The history's alpha (vr_eye_taa.comp): the surface's 1/z, a fill's HistUnknown
            // while moving, HistNone without a depth.
            r.w = p.w < 0 ? T::HistNone : (moving && p.clamp_only ? T::HistUnknown : p.w);
            if (filter == Filter::None || p.w < 0) {
                continue;
            }
            const float kf = m.rotation ? 1.0f : p.w / 0.05f;
            const float qx = x + m.dx * kf, qy = y + m.dy * kf;
            if (qx < -0.5f || qy < -0.5f || qx > Width - 0.5f || qy > Height - 0.5f)
                continue;
            const int ix = static_cast<int>(std::floor(qx)), iy = static_cast<int>(std::floor(qy));
            const float fx = qx - ix, fy = qy - iy;
            const std::array<Pixel, 4> taps{At(prev, ix, iy), At(prev, ix + 1, iy),
                                            At(prev, ix, iy + 1), At(prev, ix + 1, iy + 1)};
            const auto wx = Cubic(fx), wy = Cubic(fy);
            float lo = p.c, hi = p.c;
            float w_lo = p.w, w_hi = p.w;
            bool branch_edge = false;
            for (int j = -1; j <= 1; ++j) {
                for (int i = -1; i <= 1; ++i) {
                    const auto& n = At(cur, x + i, y + j);
                    lo = std::min(lo, n.c);
                    hi = std::max(hi, n.c);
                    branch_edge |= n.w >= 0 && !Branch::DepthMatches(n.w, p.w);
                    if (n.w >= 0) {
                        w_lo = std::min(w_lo, n.w);
                        w_hi = std::max(w_hi, n.w);
                    }
                }
            }
            if (filter == Filter::Branch) {
                std::array<float, 4> weights{};
                const std::array<float, 4> depths{taps[0].w, taps[1].w, taps[2].w, taps[3].w};
                const float limit =
                    Branch::HistoryWeights(depths, p.w, fx, fy, weights, branch_edge);
                if (limit == 0) {
                    if (counts)
                        ++counts->rejected;
                    continue;
                }
                float history = 0;
                bool sharp = true;
                for (int j = -1; j <= 2; ++j)
                    for (int i = -1; i <= 2; ++i) {
                        const auto& t = At(prev, ix + i, iy + j);
                        history += wx[i + 1] * wy[j + 1] * t.c;
                        sharp &= Branch::DepthMatches(t.w, p.w);
                    }
                if (sharp) {
                    const auto [a, b] = std::minmax_element(
                        taps.begin(), taps.end(), [](Pixel a, Pixel b) { return a.c < b.c; });
                    history = std::clamp(history, a->c, b->c);
                } else {
                    history = 0;
                    for (int k = 0; k < 4; ++k)
                        history += weights[k] * taps[k].c;
                    if (counts)
                        ++counts->bilinear;
                }
                const float share = std::min(Taa, limit);
                r.c = (1 - share) * p.c + share * std::clamp(history, lo, hi);
                if (counts)
                    ++counts->history;
                continue;
            }
            // Shipping (and Follow's shared part): Catmull-Rom rows, clamped to the 2x2.
            float acc = 0;
            for (int j = -1; j <= 2; ++j) {
                float row = 0;
                for (int i = -1; i <= 2; ++i)
                    row += wx[i + 1] * At(prev, ix + i, iy + j).c;
                acc += wy[j + 1] * row;
            }
            float h = std::clamp(acc, std::min(std::min(taps[0].c, taps[1].c),
                                               std::min(taps[2].c, taps[3].c)),
                                 std::max(std::max(taps[0].c, taps[1].c),
                                          std::max(taps[2].c, taps[3].c)));
            float weight = 1 - Taa; // this refresh's
            if (filter == Filter::Follow && moving) {
                const float k = m.rotation ? 0.0f : std::hypot(m.dx, m.dy) / 0.05f;
                std::array<float, 4> weights{};
                bool all = true;
                const std::array<float, 4> stored{taps[0].w, taps[1].w, taps[2].w, taps[3].w};
                if (!T::MovingWeights(stored, p.w, p.clamp_only ? 0.0f : k, o.gate, fx, fy,
                                      weights, all)) {
                    if (counts)
                        ++counts->rejected;
                    continue;
                }
                if (!all) {
                    h = 0;
                    for (int t = 0; t < 4; ++t)
                        h += weights[t] * taps[t].c;
                    if (counts)
                        ++counts->bilinear;
                }
                if (T::ParallaxEdge(w_lo, w_hi, k, o.gate)) {
                    weight = std::max(weight, 1 - o.cap);
                    if (counts)
                        ++counts->capped;
                }
            }
            const float hc = std::clamp(h, lo, hi);
            r.c = hc * (1 - weight) + p.c * weight;
            if (counts)
                ++counts->history;
        }
    }
    if (o.half) {
        for (auto& p : out) {
            p.c = Half(p.c);
            p.w = Half(p.w);
        }
    }
    return out;
}

// Repeated diagonal rails 20 m away (1/z 0.05), a flat bright wall 40 m behind them (0.025);
// `far` puts both 25x further (500 / 1000 m).
Pixel Rails(float x, float y, float sx, float sy, float slope, float thickness, bool far) {
    const float t = x + sx - slope * (y + sy);
    const float d = std::abs(t - 9.0f * std::round(t / 9.0f));
    const float s = far ? 0.04f : 1.0f;
    return d < thickness * 0.5f ? Pixel{0.1f, 0.05f * s} : Pixel{0.8f, 0.025f * s};
}

// A floor 1.58 m below the eye (fx 532) from a horizon at row 4 (sky above): 1/z grows by
// 1/(fx h) per row (affine on a plane). Stripes 2 m apart across it (world x).
constexpr float FloorStep = 1.0f / (532.0f * 1.58f);
Pixel Floor(float x, float y, float sx) {
    const float w = (y - 4.0f) * FloorStep;
    if (w <= 0) {
        return {0.5f, 0.0f};
    }
    // Strafing sx px at 20 m = sx / (0.05 fx) m sideways.
    const float X = (x - 48.0f) / (532.0f * w) + sx / (0.05f * 532.0f);
    return {X - 2.0f * std::floor(X / 2.0f) < 1.0f ? 0.75f : 0.2f, w};
}

enum class Scene { Rails, FarRails, Floor };
Image Render(Scene scene, float sx, float sy, float slope, float thickness, int samples) {
    Image image(Width * Height);
    const auto at = [&](float x, float y) {
        return scene == Scene::Floor ? Floor(x, y, sx)
                                     : Rails(x, y, sx, sy, slope, thickness, scene == Scene::FarRails);
    };
    for (int y = 0; y < Height; ++y) {
        for (int x = 0; x < Width; ++x) {
            auto p = at(x + 0.5f, y + 0.5f);
            p.c = 0;
            for (int j = 0; j < samples; ++j)
                for (int i = 0; i < samples; ++i)
                    p.c += at(x + (i + 0.5f) / samples, y + (j + 0.5f) / samples).c;
            p.c /= samples * samples;
            image[y * Width + x] = p;
        }
    }
    return image;
}

struct Score {
    double spatial{}, temporal{};
    Counts counts{};
};

// A 48-refresh sequence; the first 9 refreshes are not scored. Follow's per-frame switch is
// VrEyeTaa::Choose on frames whose body moves when the motion is a translation.
Score Run(Scene scene, Filter filter, Motion m, float slope, float thickness, int samples,
          const Options& o = {}, std::vector<Image>* frames = nullptr) {
    Score s{};
    Image last;
    std::vector<float> last_error(Width * Height);
    T::Frame prev{};
    for (int frame = 0; frame < 48; ++frame) {
        T::Frame f{};
        f.valid = true;
        f.turn = f.space = 1;
        f.display_ns = 1'000'000'000 + frame * 8'333'333LL;
        f.capture_ns = f.display_ns - 50'000'000;
        f.body.x = m.rotation ? 0.0f : frame * 0.01f;
        const auto choice = T::Choose(true, frame > 0, prev, f);
        prev = f;
        const auto input = Render(scene, frame * m.dx, frame * m.dy, slope, thickness, samples);
        const auto truth = Render(scene, frame * m.dx, frame * m.dy, slope, thickness, 16);
        Image result;
        if (frame == 0) {
            result = Accumulate(input, input, m, Filter::None, o, false);
        } else {
            result = Accumulate(input, last, m, filter, o, choice.moving,
                                frame > 8 ? &s.counts : nullptr);
        }
        if (filter == Filter::Follow && frame > 0) {
            EXPECT_EQ(choice.history, T::History::World);
            EXPECT_EQ(choice.moving, !m.rotation);
        }
        for (int y = 4; y < Height - 4; ++y) {
            for (int x = 4; x < Width - 4; ++x) {
                const int p = y * Width + x;
                const float e = result[p].c - truth[p].c;
                if (frame > 8) {
                    s.spatial += e * e;
                    s.temporal += std::pow(e - last_error[p], 2);
                }
                last_error[p] = e;
            }
        }
        if (frames)
            frames->push_back(result);
        last = std::move(result);
    }
    return s;
}

constexpr std::array<Motion, 5> Motions(bool rotation) {
    return {Motion{0.31f, -0.13f, rotation}, Motion{-0.31f, 0.13f, rotation},
            Motion{0.07f, 0.03f, rotation}, Motion{0.79f, 0.21f, rotation},
            Motion{1.25f, -0.4f, rotation}};
}

// Totals over motions x slopes x widths.
Score Total(Filter filter, bool rotation, int samples, std::initializer_list<float> widths,
            const Options& o = {}, Scene scene = Scene::Rails) {
    Score t{};
    for (const auto m : Motions(rotation))
        for (const float slope : {0.2f, 0.65f, -0.4f})
            for (const float width : widths) {
                const Score s = Run(scene, filter, m, slope, width, samples, o);
                t.spatial += s.spatial;
                t.temporal += s.temporal;
                t.counts.history += s.counts.history;
                t.counts.rejected += s.counts.rejected;
                t.counts.bilinear += s.counts.bilinear;
                t.counts.capped += s.counts.capped;
            }
    return t;
}

// Standing and looking around (the world still): Follow IS the baseline filter, bit for bit,
// on every scene, width (incl. 0.3 / 0.5 px rails) and with half-float history.
TEST(EyeTaaImages, AStillWorldRunsTheShippingFilterBitForBit) {
    for (const int samples : {1, 4}) {
        for (const float width : {0.3f, 0.5f, 0.8f, 1.4f}) {
            for (const bool half : {false, true}) {
                Options o{};
                o.half = half;
                o.cap = 0.25f; // never applies on a still world
                std::vector<Image> a, b;
                Run(Scene::Rails, Filter::Shipping, {0.31f, -0.13f, true}, 0.2f, width, samples,
                    o, &a);
                Run(Scene::Rails, Filter::Follow, {0.31f, -0.13f, true}, 0.2f, width, samples, o,
                    &b);
                ASSERT_EQ(a.size(), b.size());
                for (std::size_t f = 0; f < a.size(); ++f)
                    for (int p = 0; p < Width * Height; ++p)
                        ASSERT_EQ(a[f][p].c, b[f][p].c) << samples << " " << width << " " << f;
            }
        }
    }
}

// Why the branch's filter cannot replace it there: under rotation its depth test and 25% cap
// throw away the history exactly on the thin features the TAA smooths.
TEST(EyeTaaImages, UnderRotationTheShippingFilterKeepsThinRailsSmooth) {
    for (const float width : {0.3f, 0.5f, 0.8f, 1.4f}) {
        const Score none = Total(Filter::None, true, 1, {width});
        const Score ship = Total(Filter::Shipping, true, 1, {width});
        const Score branch = Total(Filter::Branch, true, 1, {width});
        std::printf("rotation, point-sampled, rails %.1f px: temporal none %.0f / shipping %.0f "
                    "/ branch %.0f; spatial %.0f / %.0f / %.0f\n",
                    width, none.temporal, ship.temporal, branch.temporal, none.spatial,
                    ship.spatial, branch.spatial);
        EXPECT_LT(ship.temporal, 0.5 * none.temporal) << width;
        EXPECT_LT(ship.temporal, branch.temporal) << width;
    }
}

// Walking (today: no history at all on these refreshes). Point-sampled input: the warp's
// silhouettes are nearest-sampled. With the default edge cap (0.5) Follow beats no history and
// the branch on shimmer AND smear at every width; without a cap the history lags parallax edges
// (spatial worse than none at 0.8 / 1.4 px), with 0.25 much of the shimmer comes back.
TEST(EyeTaaImages, WalkingKeepsAHistoryThatBeatsNoneAndTheBranch) {
    for (const float width : {0.3f, 0.5f, 0.8f, 1.4f}) {
        const Score none = Total(Filter::None, false, 1, {width});
        const Score ship = Total(Filter::Shipping, false, 1, {width});
        const Score branch = Total(Filter::Branch, false, 1, {width});
        Options uncapped{}, quarter{};
        uncapped.cap = 1.0f;
        quarter.cap = 0.25f;
        const Score follow = Total(Filter::Follow, false, 1, {width});
        const Score follow_1 = Total(Filter::Follow, false, 1, {width}, uncapped);
        const Score follow_q = Total(Filter::Follow, false, 1, {width}, quarter);
        std::printf("walking, point-sampled, rails %.1f px: temporal none %.0f / unchecked "
                    "shipping %.0f / branch %.0f / follow %.0f (cap 1: %.0f, 0.25: %.0f); "
                    "spatial %.0f / %.0f / %.0f / %.0f (%.0f, %.0f)\n",
                    width, none.temporal, ship.temporal, branch.temporal, follow.temporal,
                    follow_1.temporal, follow_q.temporal, none.spatial, ship.spatial,
                    branch.spatial, follow.spatial, follow_1.spatial, follow_q.spatial);
        EXPECT_LT(follow.temporal, 0.6 * none.temporal) << width;
        EXPECT_LT(follow.spatial, 0.75 * none.spatial) << width;
        EXPECT_LT(follow.temporal, branch.temporal) << width;
        EXPECT_LT(follow.spatial, branch.spatial) << width;
    }
}

TEST(EyeTaaImages, WalkingOnPreAntialiasedInputPrintsTheBlurCost) {
    // Already smooth input: any history blurs it (this metric also rates the baseline filter
    // under rotation far worse than none, which the mock's -77% crawl contradicts). Reported, not
    // asserted: the real image is in between (the game's AA is on, the warp's silhouettes are
    // nearest-sampled). The cap is what bounds it.
    for (const float width : {0.5f, 1.4f}) {
        Options quarter{}, uncapped{};
        quarter.cap = 0.25f;
        uncapped.cap = 1.0f;
        const Score none = Total(Filter::None, false, 4, {width});
        const Score branch = Total(Filter::Branch, false, 4, {width});
        const Score follow = Total(Filter::Follow, false, 4, {width});
        const Score follow_1 = Total(Filter::Follow, false, 4, {width}, uncapped);
        const Score follow_q = Total(Filter::Follow, false, 4, {width}, quarter);
        std::printf("walking, 4x4 AA input, rails %.1f px: temporal none %.0f / branch %.0f / "
                    "follow %.0f (cap 1: %.0f, 0.25: %.0f); spatial %.0f / %.0f / %.0f (%.0f, "
                    "%.0f)\n",
                    width, none.temporal, branch.temporal, follow.temporal, follow_1.temporal,
                    follow_q.temporal, none.spatial, branch.spatial, follow.spatial,
                    follow_1.spatial, follow_q.spatial);
        EXPECT_LT(follow.temporal, follow_1.temporal) << width;
    }
}

// A floor from 14 m to the horizon while strafing at ~2.7 m/s: every floor pixel keeps its
// history and the sharp (Catmull-Rom) sample; the branch's fixed 2% test loses it far away.
TEST(EyeTaaImages, ASlopedFloorKeepsItsHistoryWhileStrafing) {
    for (const bool half : {false, true}) {
        Options o{};
        o.half = half;
        const Motion strafe{0.6f, 0.0f, false};
        const Score follow = Run(Scene::Floor, Filter::Follow, strafe, 0, 0, 1, o);
        const Score branch = Run(Scene::Floor, Filter::Branch, strafe, 0, 0, 1, o);
        const Score none = Run(Scene::Floor, Filter::None, strafe, 0, 0, 1, o);
        std::printf("floor strafe (half %d): follow history %ld rejected %ld bilinear %ld; branch "
                    "history %ld rejected %ld bilinear %ld; temporal none %.0f follow %.0f branch "
                    "%.0f\n",
                    half, follow.counts.history, follow.counts.rejected, follow.counts.bilinear,
                    branch.counts.history, branch.counts.rejected, branch.counts.bilinear,
                    none.temporal, follow.temporal, branch.temporal);
        EXPECT_EQ(follow.counts.rejected, 0);
        EXPECT_EQ(follow.counts.bilinear, 0);
        EXPECT_GT(branch.counts.bilinear + branch.counts.rejected, 0);
        EXPECT_LT(follow.temporal, none.temporal);
    }
}

// Rails 500 m in front of a wall 1000 m away, half-float history: rounding never rejects, so
// the history survives exactly as without rounding (only the true occlusion edges differ).
TEST(EyeTaaImages, HalfFloatRoundingKeepsDistantHistory) {
    for (const auto m : Motions(false)) {
        Options exact{}, half{};
        half.half = true;
        const Score a = Run(Scene::FarRails, Filter::Follow, m, 0.2f, 1.4f, 1, exact);
        const Score b = Run(Scene::FarRails, Filter::Follow, m, 0.2f, 1.4f, 1, half);
        EXPECT_EQ(a.counts.rejected, b.counts.rejected) << m.dx;
        EXPECT_EQ(a.counts.bilinear, b.counts.bilinear) << m.dx;
        EXPECT_NEAR(b.temporal, a.temporal, 0.02 * a.temporal + 1.0) << m.dx;
    }
}

// The branch's moving-occluder fixture, with the camera walking: the wall a rail uncovered keeps
// none of the rail's old colour (the unchecked filter smears it).
TEST(EyeTaaImages, MovingOccluderDoesNotLeaveTrailsOnTexturedBackground) {
    Image old(Width * Height), cur(Width * Height);
    for (int y = 0; y < Height; ++y) {
        for (int x = 0; x < Width; ++x) {
            const Pixel wall{(x + y) % 2 ? 0.8f : 0.3f, 0.025f};
            old[y * Width + x] = x < 48 ? Pixel{0.1f, 0.05f} : wall;
            cur[y * Width + x] = x < 44 ? Pixel{0.1f, 0.05f} : wall;
        }
    }
    const Motion walk{0.35f, 0.0f, false};
    const auto unchecked = Accumulate(cur, old, walk, Filter::Shipping, {}, true);
    const auto follow = Accumulate(cur, old, walk, Filter::Follow, {}, true);
    double old_error = 0, error = 0;
    for (int y = 4; y < Height - 4; ++y) {
        for (int x = 44; x < 48; ++x) {
            const int p = y * Width + x;
            old_error += std::abs(unchecked[p].c - cur[p].c);
            error += std::abs(follow[p].c - cur[p].c);
        }
    }
    EXPECT_GT(old_error, 1.0); // the fixture catches the smear
    EXPECT_NEAR(error, 0.0, 1e-5);
}

// Fills (invented: colour-clamped history only) keep the history a surface pixel would lose,
// but never the body's.
TEST(EyeTaaImages, FillsKeepColourClampedHistoryButNotTheBodys) {
    Image old(Width * Height, {0.4f, 0.05f}), cur(Width * Height, {0.5f, 0.025f});
    for (auto& p : cur)
        p.clamp_only = true;
    const Motion walk{0.35f, 0.0f, false};
    Counts counts{};
    Accumulate(cur, old, walk, Filter::Follow, {}, true, &counts);
    EXPECT_EQ(counts.rejected, 0);
    EXPECT_GT(counts.history, 0);
    for (auto& p : old)
        p.w = T::HistNone;
    counts = {};
    Accumulate(cur, old, walk, Filter::Follow, {}, true, &counts);
    EXPECT_EQ(counts.history, 0);
}

TEST(EyeTaaImages, StillImageAndFlatTextureAreNotBlurred) {
    auto input = Render(Scene::Rails, 0, 0, 0.2f, 1.4f, 1);
    for (const bool moving : {false, true}) {
        const auto edge = Accumulate(input, input, {}, Filter::Follow, {}, moving);
        for (int p = 0; p < Width * Height; ++p)
            EXPECT_FLOAT_EQ(edge[p].c, input[p].c);
    }
}

// eye_taa_edge_cap: only at depth edges with parallax, only while the world moves.
TEST(EyeTaaImages, TheEdgeCapAppliesOnlyAtParallaxEdgesWhileMoving) {
    Options o{};
    o.cap = 0.25f;
    const Score rotation = Run(Scene::Rails, Filter::Follow, {0.31f, -0.13f, true}, 0.2f, 1.4f,
                               1, o);
    EXPECT_EQ(rotation.counts.capped, 0);
    const Score walk = Run(Scene::Rails, Filter::Follow, {0.31f, -0.13f, false}, 0.2f, 1.4f, 1,
                           o);
    EXPECT_GT(walk.counts.capped, 0);
    EXPECT_LT(walk.counts.capped, walk.counts.history);
    const Score floor = Run(Scene::Floor, Filter::Follow, {0.6f, 0.0f, false}, 0, 0, 1, o);
    EXPECT_EQ(floor.counts.capped, 0); // a slope is not an edge
}
} // namespace
