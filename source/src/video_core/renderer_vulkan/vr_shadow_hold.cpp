// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/renderer_vulkan/vr_shadow_hold.h"

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <span>
#include <utility>

#include "video_core/native_stereo/lighting_reconstruction.h"
#include "common/logging/log.h"
#include "core/vr/live_tune.h"
#include "core/vr/vr_session.h"

namespace Vulkan::ShadowHold {
namespace {

using Matrix = std::array<double, 16>; // row-major, column vectors

Matrix Product(const Matrix& a, const Matrix& b) {
    Matrix c{};
    for (u32 r = 0; r < 4; ++r) {
        for (u32 k = 0; k < 4; ++k) {
            const double v = a[4 * r + k];
            for (u32 col = 0; col < 4; ++col) {
                c[4 * r + col] += v * b[4 * k + col];
            }
        }
    }
    return c;
}

// Gauss-Jordan with partial pivoting; false when singular or not finite.
bool Invert(Matrix m, Matrix& out) {
    Matrix inv{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    for (u32 col = 0; col < 4; ++col) {
        u32 pivot = col;
        for (u32 r = col + 1; r < 4; ++r) {
            if (std::abs(m[4 * r + col]) > std::abs(m[4 * pivot + col])) {
                pivot = r;
            }
        }
        const double p = m[4 * pivot + col];
        if (!std::isfinite(p) || std::abs(p) < 1e-30) {
            return false;
        }
        if (pivot != col) {
            for (u32 c = 0; c < 4; ++c) {
                std::swap(m[4 * pivot + c], m[4 * col + c]);
                std::swap(inv[4 * pivot + c], inv[4 * col + c]);
            }
        }
        for (u32 c = 0; c < 4; ++c) {
            m[4 * col + c] /= p;
            inv[4 * col + c] /= p;
        }
        for (u32 r = 0; r < 4; ++r) {
            const double f = m[4 * r + col];
            if (r == col || f == 0.0) {
                continue;
            }
            for (u32 c = 0; c < 4; ++c) {
                m[4 * r + c] -= f * m[4 * col + c];
                inv[4 * r + c] -= f * inv[4 * col + c];
            }
        }
    }
    for (const double v : inv) {
        if (!std::isfinite(v)) {
            return false;
        }
    }
    out = inv;
    return true;
}

constexpr u32 Cascades336 = 4;
// A cascade's window may have moved this share of its width since the atlas was drawn (a tile is
// 2048 texels: 1.5% is 30 of them); beyond it the atlas is drawn again. VR_SHADOW_HOLD_LIMIT
// (tests) names another.
constexpr double DefaultLimit = 0.015;
// A frame's atlas draws when no frame start follows (another area's layout): the hold ends.
constexpr u32 RunawayDraws = 4000;
// Whole-atlas draws in a row with no tile's draw between (a frame makes at most four): no casters.
constexpr u32 IdleAtlasDraws = 16;
// The light's axes in the world: another one beyond this (a reflection's camera, another area).
constexpr double SameLight = 0.02;
constexpr u32 ForeignFrames = 6;
// After a reflection's cascades were seen, nothing is held for this many frames (while the water
// is in view they come every frame).
constexpr u32 MirrorFrames = 120;
// The camera's move between two frames as a share of a window: its turn in radians (a window's
// middle lies about its own width ahead), and its way over the nearest window's width, about
// 15 m. An estimate for the first held frame only: while held the windows' move is measured.
constexpr double NearWindowM = 15.0;

// (On by default from 2026-11-15 to 11-18. OFF by default since, the user's call for 0.9.6: with
// the two far cascades held a carriage's shadow 8 to 15 m from the camera showed only from close
// by and a piece of it blinked (third person, the Great Bridge), also with the steadying off; with
// the cache off it was right. The cause is not found: the frame's start, the held matrices and the
// clear were checked. VR_SHADOW_HOLD=4 or the live key lighting.shadow_hold=4: on, the two far
// cascades drawn one frame in four.)
inline constexpr u32 DefaultStride = 0;
// The cascades drawn every frame, from the nearest (vr_shadow_hold.h: NearCascades).
// VR_SHADOW_HOLD_NEAR=0..2 (tests) names another count.
const u32 g_near = [] {
    const char* const env = std::getenv("VR_SHADOW_HOLD_NEAR");
    return env ? static_cast<u32>(std::clamp(std::atoi(env), 0, 2)) : NearCascades;
}();
struct Config {
    u32 stride{DefaultStride};
    double limit{DefaultLimit};
    bool snap{true};
};
// Steady cascades (vr_shadow_hold.h): the size's steps, the slack before the step above is taken
// (in steps: a fit breathing about a step's edge does not flip), the anchor's grid and how far
// the camera goes from an anchor before another is taken, and how close (texels) the light's own
// camera and the cascades' matrices must put the same place.
constexpr double SnapStep = 1.02;
constexpr double SnapSlack = 0.2;
constexpr double AnchorGridM = 16.0;
constexpr double AnchorKeepM = 12.0;
constexpr double SnapAgreeTexels = 0.6;
constexpr u32 SnapAgreeFrames = 8;
constexpr u32 SnapDisagreeFrames = 3;
const Config g_config = [] {
    Config c;
    if (const char* const env = std::getenv("VR_SHADOW_HOLD")) {
        const int n = std::atoi(env);
        c.stride = n >= 2 ? static_cast<u32>(std::min(n, 16)) : 0;
    }
    if (const char* const env = std::getenv("VR_SHADOW_SNAP")) {
        c.snap = std::atoi(env) != 0;
    }
    if (const char* const env = std::getenv("VR_SHADOW_HOLD_LIMIT")) {
        const double v = std::atof(env);
        if (v > 0.0 && v < 0.5) {
            c.limit = v;
        }
    }
    return c;
}();

// An atlas tile (a cascade's): the light's camera for it, and this frame's steadying.
struct Tile {
    bool have{}, fresh{};            // the light's clip rows are known; ... from this frame
    std::array<double, 8> clip{};    // world -> clip x and y
    std::array<float, 4> viewport{}; // x offset, x scale, y offset, y scale (texels)
    u32 side{};
    bool frozen{}, snapped{};        // this frame's answer is given; ... and moves something
    double dx{}, dy{}, qx{1.0}, qy{1.0};
    int step_x{INT_MIN}, step_y{INT_MIN};
    // The check: which cascade's it is, which way up it is read, frames that agreed / did not.
    int cascade{-1};
    bool flip{}, ok{};
    u32 agree{}, disagree{};
};

struct State {
    // Stage A only.
    std::array<Tile, 4> tiles{};
    int tile{-1};                            // the draw being made is into this one
    std::array<int, Cascades336> tile_of{-1, -1, -1, -1};
    bool snap_ok{}, v_flip{};
    u32 snap_logs{};
    bool tiles_read{};                       // the tiles' answers were used by a cascades' buffer
    bool have_anchor{};
    std::array<double, 3> anchor{};
    std::array<Matrix, Cascades336> ref_raw{}; // world -> shadow as the game fitted it (the move's measure)
    bool have_view{};
    Matrix view{}, inverse_view{};
    std::array<float, 12> view_rows{};       // the camera block's view rows they were made from
    bool have_ref{};                         // world -> shadow of what the atlas holds
    std::array<Matrix, Cascades336> ref{};
    bool have_light{};
    std::array<double, 6> light{};           // the light's two axes across the atlas (unit, world)
    u32 foreign{};
    bool mirror_seen{};                      // cascades of another camera came by (a water reflection's)
    u32 mirror_frame{};                      // ... at this frame count
    bool holding{};                          // the atlas draws that follow are left out
    bool atlas_held{};                       // ... and those before the last whole-atlas draw were
    u32 seen_cells{};                        // the atlas' cells drawn into since the frame's start
    u32 after_cells{};                       // ... of them, those a whole-atlas draw came after
    u32 idle_atlas{};                        // whole-atlas draws in a row with no tile's between
    bool resolved{};                         // the frame's cascades were seen since its start
    u32 held{};                              // frames since the atlas was drawn
    u32 skipped{};                           // draws left out since the frame's start
    u32 turn{};
    double shift{};                          // the windows' move since the atlas was drawn (share)
    double rate{};                           // ... a frame, as last measured
    bool have_last{};
    Matrix last_inverse_view{};              // the camera at the frame before
    double camera_rate{};                    // the camera's own move a frame, as a share (CameraShare)
    // Counts (logs).
    u32 frames{}, holds{}, by_stride{}, by_limit{}, by_turn{}, by_light{}, by_runaway{};
    double worst{};
};
State g_state;

struct Entry {
    std::array<float, 84> raw, replaced;
};
std::mutex g_table_mutex;
std::array<Entry, 4> g_table{};
u32 g_table_used = 0, g_table_next = 0;

Matrix Affine(const float* rows3x4) {
    Matrix m{};
    for (u32 i = 0; i < 12; ++i) {
        m[i] = rows3x4[i];
    }
    m[15] = 1.0;
    return m;
}

} // Anonymous namespace

bool On() {
    return g_config.stride != 0;
}

bool Active() {
    return g_config.stride != 0 || g_config.snap;
}

void NoCell() {
    g_state.tile = -1;
}

void CellDraw(u32 cell, const std::array<float, 4>& viewport, u32 side) {
    State& s = g_state;
    s.tile = -1;
    const u32 col = cell % 4, row = cell / 4;
    const float quarter = 0.25f * static_cast<float>(side);
    if (!g_config.snap || (col & 1) == 0 || (row & 1) == 0 || std::abs(viewport[1] - quarter) > 0.02f * quarter ||
        std::abs(std::abs(viewport[3]) - quarter) > 0.02f * quarter) {
        return;
    }
    s.tile = static_cast<int>((col >> 1) + 2 * (row >> 1));
    // (The first draw into a tile since the cascades were read: a new frame's tiles.)
    if (s.tiles_read) {
        s.tiles_read = false;
        for (Tile& tile : s.tiles) {
            tile.fresh = tile.frozen = tile.snapped = false;
        }
    }
    Tile& t = s.tiles[s.tile];
    t.viewport = viewport;
    t.side = side;
}

namespace {

// The size's step: texels a metre brought down to a power of SnapStep, the one of the last frame
// kept while the fit is within it (and SnapSlack above). The share to draw at (at most 1).
double Quantize(double texels_per_metre, int& step) {
    const double level = std::log(texels_per_metre) / std::log(SnapStep);
    if (step == INT_MIN || level < step || level > step + 1.0 + SnapSlack) {
        step = static_cast<int>(std::floor(level));
    }
    return std::pow(SnapStep, step) / texels_per_metre;
}

bool SnapWanted() {
    return g_config.snap && VR::Tune::Get("lighting.shadow_snap", 1.0f) >= 0.5f;
}

// A tile's change applied to a matrix that ends in atlas coordinates (rows 0 and 1 of 0..1 across
// the atlas, row 3 the w).
void ApplyTile(const Tile& t, bool v_flip, Matrix& m) {
    const double side = t.side;
    const double cu = t.viewport[0] / side;
    const double cv = v_flip ? 1.0 - t.viewport[2] / side : t.viewport[2] / side;
    const double du = t.dx / side, dv = (v_flip ? -t.dy : t.dy) / side;
    for (u32 c = 0; c < 4; ++c) {
        const double w = m[12 + c];
        m[c] = cu * w + (m[c] - cu * w) * t.qx + du * w;
        m[4 + c] = cv * w + (m[4 + c] - cv * w) * t.qy + dv * w;
    }
}

} // Anonymous namespace

void LightCamera(const float* scene) {
    State& s = g_state;
    if (s.tile < 0 || !(scene[66] == 0.f && scene[67] == 1.f)) {
        return;
    }
    Tile& t = s.tiles[s.tile];
    Matrix projection;
    for (u32 i = 0; i < 16; ++i) {
        projection[i] = scene[52 + i];
    }
    const Matrix clip = Product(projection, Affine(scene + 8));
    for (u32 i = 0; i < 8; ++i) {
        if (!std::isfinite(clip[i])) {
            return;
        }
        t.clip[i] = clip[i];
    }
    t.have = t.fresh = true;
}

std::optional<std::array<float, 4>> Snap() {
    State& s = g_state;
    if (s.tile < 0) {
        return std::nullopt;
    }
    Tile& t = s.tiles[s.tile];
    if (!t.frozen) {
        // The tile's first draw of the frame (its light was seen by now): the answer for all of
        // its draws, and for the cascades' buffer that follows them.
        t.frozen = true;
        t.snapped = false;
        t.dx = t.dy = 0.0;
        t.qx = t.qy = 1.0;
        const bool mirror = s.mirror_seen && s.frames - s.mirror_frame < MirrorFrames;
        if (t.ok && t.fresh && s.snap_ok && s.have_view && !mirror && SnapWanted()) {
            const std::array<double, 3> camera{s.inverse_view[3], s.inverse_view[7], s.inverse_view[11]};
            bool keep = s.have_anchor;
            for (u32 i = 0; i < 3; ++i) {
                keep = keep && std::abs(camera[i] - s.anchor[i]) <= AnchorKeepM;
            }
            if (!keep) {
                for (u32 i = 0; i < 3; ++i) {
                    s.anchor[i] = std::round(camera[i] / AnchorGridM) * AnchorGridM;
                }
                s.have_anchor = true;
            }
            const double* const x = t.clip.data();
            const double* const y = t.clip.data() + 4;
            const double sx = std::abs(t.viewport[1]) * std::sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
            const double sy = std::abs(t.viewport[3]) * std::sqrt(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
            if (sx > 1e-6 && sy > 1e-6 && std::isfinite(sx) && std::isfinite(sy)) {
                t.qx = Quantize(sx, t.step_x);
                t.qy = Quantize(sy, t.step_y);
                const double ax = x[0] * s.anchor[0] + x[1] * s.anchor[1] + x[2] * s.anchor[2] + x[3];
                const double ay = y[0] * s.anchor[0] + y[1] * s.anchor[1] + y[2] * s.anchor[2] + y[3];
                // (The anchor's texel once the tile is drawn at its share, about its centre.)
                const double tx = t.viewport[0] + t.viewport[1] * t.qx * ax;
                const double ty = t.viewport[2] + t.viewport[3] * t.qy * ay;
                t.dx = std::round(tx) - tx;
                t.dy = std::round(ty) - ty;
                t.snapped = std::isfinite(t.dx) && std::isfinite(t.dy);
                if constexpr (Common::Log::Enabled) {
                    if (s.frames % 900 == 450) {
                        LOG_INFO(Render_Vulkan,
                                 "VR steady cascades: tile {} (cascade {}): {:.2f} x {:.2f} texels a metre, drawn at {:.4f} x "
                                 "{:.4f}, moved ({:+.3f} {:+.3f}) texels; anchor ({:.0f} {:.0f} {:.0f})",
                                 s.tile, t.cascade, sx, sy, t.qx, t.qy, t.dx, t.dy, s.anchor[0], s.anchor[1], s.anchor[2]);
                    }
                }
            }
        }
    }
    if (!t.snapped) {
        return std::nullopt;
    }
    return std::array<float, 4>{static_cast<float>(t.dx), static_cast<float>(t.dy), static_cast<float>(t.qx),
                                static_cast<float>(t.qy)};
}

u32 NearTiles() {
    return g_near;
}

u32 HeldFrame() {
    return g_state.frames | 0x80000000u; // (never 0: 0 is "not a held frame's draw")
}

Verdict SkipDraw(bool whole_atlas, u32 cell) {
    State& s = g_state;
    // The game's whole-atlas draw (no depth test, no depth write, no colour: it makes the depth
    // readable) draws nothing here. Until 2026-11-18 it was taken for a frame's start. It is not
    // one: the game makes it once after a frame's four cascades, or after each cascade, or after
    // three of them and again after the fourth, and twice in a row where a cascade has no caster
    // (all traced at the Great Bridge in Central Yharnam, within one minute at one spot). Each
    // stretch between two of them was a "frame" of its own, held or drawn by itself: with one a
    // cascade and four frames a round, the same cascade was the drawn one frame after frame and
    // the others stayed empty behind the game's clear. Shadows near the hunter blinked out and
    // jumped (the user, a carriage's shadow there). On a held frame it is left out: with the
    // game's clear still owed (no draw into a near cascade took it) it would wipe the atlas.
    if (whole_atlas) {
        s.after_cells |= s.seen_cells;
        if (++s.idle_atlas > IdleAtlasDraws && s.holding) {
            // (No caster anywhere for frames: nothing to hold; the game's clear goes through.)
            s.holding = s.atlas_held = false;
            s.have_ref = false;
            ++s.by_runaway;
        }
        return s.holding ? Verdict::Skip : Verdict::Draw;
    }
    s.idle_atlas = 0;
    // A frame's start: the cascades' round begins again, a draw into a cell this frame already
    // drew into with a whole-atlas draw since (every layout makes one after a cascade's last draw
    // of the frame at the latest).
    const u32 bit = 1u << (cell & 15u);
    if ((s.after_cells & bit) != 0) {
        s.seen_cells = 0;
        s.after_cells = 0;
        const u32 turn = VR::DrawFrameRecord().turn_serial;
        // What this frame's move will be: the camera's own of the last frame (always measured),
        // and while held the windows' as measured.
        const double step = std::max(s.rate, s.camera_rate);
        const double next = s.shift + step;
        // (Live key lighting.shadow_hold: another count; 0 or 1: every frame drawn.)
        const u32 stride = static_cast<u32>(
            std::clamp(VR::Tune::Get("lighting.shadow_hold", static_cast<float>(g_config.stride)), 0.0f, 16.0f));
        // (The atlas as it is now is what would be kept: drawn by the last frame's tiles, or held
        // since longer; its own matrices were the last cascades' buffer's, or the kept ones.)
        bool hold = s.resolved && stride >= 2 && s.have_ref;
        if (hold && s.mirror_seen && s.frames - s.mirror_frame < MirrorFrames) {
            hold = false; // a reflection reads the atlas with matrices of its own
        }
        if (hold && s.held + 1 >= stride) {
            hold = false;
            ++s.by_stride;
        } else if (hold && turn != s.turn) {
            hold = false;
            ++s.by_turn;
        } else if (hold && !(next < g_config.limit)) {
            hold = false;
            ++s.by_limit;
            s.rate *= 0.5; // (measured only while held: asked again soon)
        }
        s.turn = turn;
        s.resolved = false;
        s.skipped = 0;
        // The game's order: a frame's tiles, then the cascades' buffer that reads them. So that
        // buffer is read by this decision.
        s.holding = s.atlas_held = hold;
        s.held = hold ? s.held + 1 : 0;
        s.holds += hold;
        if (!hold) {
            s.shift = 0.0;
        }
        // (Every build counts the frames: the reflection's rest (MirrorFrames) and the held frame's
        // number are in them. Until 2026-11-18 only the log builds counted, so in a release build a
        // reflection once seen kept the cache off for the rest of the session.)
        ++s.frames;
        if constexpr (Common::Log::Enabled) {
            if (s.frames % 1800 == 0) {
                LOG_INFO(Render_Vulkan,
                         "VR shadow hold (TEST, every {} frames, limit {:.3f}): {} frames, {} held; drawn again by "
                         "the count {}, the windows' move {}, a turn {}, another light {}, no frame start {}; the "
                         "largest move while held {:.4f}, the last rate {:.5f} a frame, the camera's {:.5f}",
                         g_config.stride, g_config.limit, s.frames, s.holds, s.by_stride, s.by_limit, s.by_turn,
                         s.by_light, s.by_runaway, s.worst, s.rate, s.camera_rate);
            }
        }
    }
    s.seen_cells |= bit;
    if (!s.holding) {
        return Verdict::Draw;
    }
    if ((cell == NearCell && g_near >= 1) || (cell == NearCell2 && g_near >= 2)) {
        return Verdict::NearClear; // (every one of them: Rasterizer::VrShadowNearClear)
    }
    if (++s.skipped > RunawayDraws) {
        s.holding = s.atlas_held = false;
        s.have_ref = false;
        ++s.by_runaway;
        return Verdict::Draw;
    }
    return Verdict::Skip;
}

void Camera(const float* scene) {
    // The scene's own camera: a perspective projection (the light's is orthographic: 0, 1), its
    // viewport's size, and no mirror (a reflection's view turns the world inside out).
    if (!(scene[66] == 1.f && scene[67] == 0.f && scene[4] > 100.f && scene[5] > 100.f)) {
        return;
    }
    State& s = g_state;
    // The same view rows, bit for bit, as the last camera made into the held view: nothing changes
    // (only this function writes the view). Most of a frame's draws bind the same camera block:
    // the inverse was taken hundreds of times a frame, ~1% of the GPU command thread in the
    // Forbidden Woods (2026-11-17's profile).
    if (s.have_view && std::memcmp(s.view_rows.data(), scene + 8, sizeof(s.view_rows)) == 0) {
        return;
    }
    const Matrix view = Affine(scene + 8);
    const double det = view[0] * (view[5] * view[10] - view[6] * view[9]) -
                       view[1] * (view[4] * view[10] - view[6] * view[8]) +
                       view[2] * (view[4] * view[9] - view[5] * view[8]);
    if (!(det > 0.5 && det < 2.0)) {
        return;
    }
    Matrix inverse;
    if (!Invert(view, inverse)) {
        return;
    }
    s.view = view;
    s.inverse_view = inverse;
    std::memcpy(s.view_rows.data(), scene + 8, sizeof(s.view_rows));
    s.have_view = true;
}

bool Cascades(const float* raw, float* out) {
    State& s = g_state;
    if (!s.have_view ||
        NativeProjection::RecognizeLighting(std::span<const float>{raw, 84}) !=
            NativeProjection::LightingLayout::Cascades) {
        return false;
    }
    Matrix inverse_p, p;
    for (u32 i = 0; i < 16; ++i) {
        inverse_p[i] = raw[i];
    }
    if (!Invert(inverse_p, p)) {
        return false;
    }
    // World to shadow of this frame's own fit.
    std::array<Matrix, Cascades336> now;
    for (u32 k = 0; k < Cascades336; ++k) {
        Matrix m;
        for (u32 i = 0; i < 16; ++i) {
            m[i] = raw[16 + 16 * k + i];
        }
        Matrix view_to_shadow = Product(m, p);
        const double w = view_to_shadow[15];
        if (!std::isfinite(w) || std::abs(w) < 1e-12) {
            return false;
        }
        for (double& v : view_to_shadow) {
            v /= w;
        }
        now[k] = Product(view_to_shadow, s.view);
    }
    // The light's axes: the same light as the held atlas', and the scene's own camera (a
    // reflection's cascades, brought to the world with the scene's view, point elsewhere).
    std::array<double, 6> light{};
    for (u32 r = 0; r < 2; ++r) {
        const double* row = now[0].data() + 4 * r;
        const double length = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
        if (!(length > 1e-12)) {
            return false;
        }
        for (u32 i = 0; i < 3; ++i) {
            light[3 * r + i] = row[i] / length;
        }
    }
    if (s.have_light) {
        double apart = 0.0;
        for (u32 i = 0; i < 6; ++i) {
            apart = std::max(apart, std::abs(light[i] - s.light[i]));
        }
        if (apart > SameLight) {
            // (Another camera's cascades: a planar water reflection draws its own shadow pass. Its
            // matrices are not re-aimed, so while one is about nothing is held: MirrorFrames.)
            s.mirror_seen = true;
            s.mirror_frame = s.frames;
            if (++s.foreign < ForeignFrames) {
                return false; // not this buffer's business
            }
            // Another light for good: nothing held fits it.
            s.have_ref = false;
            s.holding = false;
            ++s.by_light;
        }
    }
    s.foreign = 0;
    s.light = light;
    s.have_light = true;
    if (!s.resolved) {
        // Once a frame: how far the camera went since the last one.
        if (s.have_last) {
            const Matrix move = Product(s.view, s.last_inverse_view);
            const double cosine = std::clamp((move[0] + move[5] + move[10] - 1.0) * 0.5, -1.0, 1.0);
            const double way = std::sqrt(move[3] * move[3] + move[7] * move[7] + move[11] * move[11]);
            s.camera_rate = std::acos(cosine) + way / NearWindowM;
        }
        s.last_inverse_view = s.inverse_view;
        s.have_last = true;
    }
    // Steady cascades. The game's order in a frame: the whole-atlas draw, the cascades' buffer,
    // then the draws into the tiles. So at a frame's first cascades: the frame before is checked
    // (its cascades against the light's camera seen at its tiles: which tile is which cascade's,
    // and which way up the atlas is read), and this frame's tiles get their answers, from the
    // cascades' own matrices.
    if (g_config.snap && !s.resolved) {
        {
            const std::array<double, 3> at{s.inverse_view[3], s.inverse_view[7], s.inverse_view[11]};
            for (u32 i = 0; i < s.tiles.size(); ++i) {
                Tile& t = s.tiles[i];
                if (!t.fresh || t.side == 0) {
                    continue;
                }
                const double side = t.side;
                // (Unmoved: what the game's own matrices describe.)
                const double tx = t.viewport[0] + t.viewport[1] * (t.clip[0] * at[0] + t.clip[1] * at[1] + t.clip[2] * at[2] + t.clip[3]);
                const double ty = t.viewport[2] + t.viewport[3] * (t.clip[4] * at[0] + t.clip[5] * at[1] + t.clip[6] * at[2] + t.clip[7]);
                int found = -1;
                bool flip = false;
                for (u32 k = 0; k < Cascades336; ++k) {
                    const Matrix& m = now[k];
                    const double u = m[0] * at[0] + m[1] * at[1] + m[2] * at[2] + m[3];
                    const double v = m[4] * at[0] + m[5] * at[1] + m[6] * at[2] + m[7];
                    if constexpr (Common::Log::Enabled) {
                        if (s.snap_logs < 32) {
                            ++s.snap_logs;
                            LOG_INFO(Render_Vulkan,
                                     "VR steady cascades: tile {} cascade {}: the camera's place at texel ({:.2f} {:.2f}) by the "
                                     "light's camera, ({:.2f} {:.2f} / flipped {:.2f}) by the cascade's matrix; viewport {:.1f} "
                                     "{:.1f} {:.1f} {:.1f}",
                                     i, k, tx, ty, u * side, v * side, (1.0 - v) * side, t.viewport[0], t.viewport[1],
                                     t.viewport[2], t.viewport[3]);
                        }
                    }
                    if (std::abs(tx - u * side) > SnapAgreeTexels) {
                        continue;
                    }
                    if (std::abs(ty - v * side) <= SnapAgreeTexels) {
                        found = static_cast<int>(k);
                        flip = false;
                    } else if (std::abs(ty - (1.0 - v) * side) <= SnapAgreeTexels) {
                        found = static_cast<int>(k);
                        flip = true;
                    }
                }
                if (found >= 0 && (t.agree == 0 || (t.cascade == found && t.flip == flip))) {
                    t.cascade = found;
                    t.flip = flip;
                    t.disagree = 0;
                    if (++t.agree >= SnapAgreeFrames && !t.ok) {
                        t.ok = true;
                        LOG_INFO(Render_Vulkan, "VR steady cascades: tile {} is cascade {}'s (the atlas read {})", i, found,
                                 flip ? "upside down" : "as drawn");
                    }
                } else if (++t.disagree >= SnapDisagreeFrames) {
                    if (t.ok) {
                        LOG_INFO(Render_Vulkan, "VR steady cascades: tile {} left alone (its light and the cascades disagree)", i);
                    }
                    t.ok = false;
                    t.agree = 0;
                } else if (!t.ok) {
                    t.agree = 0;
                }
            }
        }
        // The cascades' tiles, of those that were checked; all read the same way up, or none.
        s.tile_of = {-1, -1, -1, -1};
        s.snap_ok = false;
        bool one_way = true;
        for (u32 i = 0; i < s.tiles.size(); ++i) {
            const Tile& t = s.tiles[i];
            if (!t.ok) {
                continue;
            }
            if (s.snap_ok && t.flip != s.v_flip) {
                one_way = false;
            }
            s.v_flip = t.flip;
            s.snap_ok = true;
            s.tile_of[t.cascade] = static_cast<int>(i);
        }
        if (!one_way) {
            s.tile_of = {-1, -1, -1, -1};
            s.snap_ok = false;
        }
        if constexpr (Common::Log::Enabled) {
            if (s.frames % 900 == 200) {
                LOG_INFO(Render_Vulkan,
                         "VR steady cascades: cascades' tiles {} {} {} {}; tiles' lights seen this frame {} {} {} {}, ever {} {} "
                         "{} {}; frames agreed {} {} {} {}",
                         s.tile_of[0], s.tile_of[1], s.tile_of[2], s.tile_of[3], s.tiles[0].fresh, s.tiles[1].fresh,
                         s.tiles[2].fresh, s.tiles[3].fresh, s.tiles[0].have, s.tiles[1].have, s.tiles[2].have, s.tiles[3].have,
                         s.tiles[0].agree, s.tiles[1].agree, s.tiles[2].agree, s.tiles[3].agree);
            }
        }
    }
    s.tiles_read = true;
    // What was drawn this frame was drawn moved (Snap): its matrices with the same change.
    std::array<Matrix, Cascades336> steady = now;
    std::array<bool, Cascades336> moved{};
    bool any_moved = false;
    for (u32 k = 0; k < Cascades336; ++k) {
        const int i = s.tile_of[k];
        if (i >= 0 && s.tiles[i].frozen && s.tiles[i].snapped) {
            ApplyTile(s.tiles[i], s.v_flip, steady[k]);
            moved[k] = any_moved = true;
        }
    }
    const auto write_moved = [&](u32 k) {
        Matrix m;
        for (u32 i = 0; i < 16; ++i) {
            m[i] = raw[16 + 16 * k + i];
        }
        ApplyTile(s.tiles[s.tile_of[k]], s.v_flip, m);
        for (u32 i = 0; i < 16; ++i) {
            out[16 + 16 * k + i] = static_cast<float>(m[i]);
        }
    };
    const auto remember = [&] {
        std::scoped_lock lock{g_table_mutex};
        Entry& e = g_table[g_table_next];
        std::memcpy(e.raw.data(), raw, sizeof(e.raw));
        std::memcpy(e.replaced.data(), out, sizeof(e.replaced));
        g_table_next = (g_table_next + 1) % g_table.size();
        g_table_used = std::min<u32>(g_table_used + 1, static_cast<u32>(g_table.size()));
    };
    s.resolved = true;
    if (!s.atlas_held || !s.have_ref) {
        // The atlas was drawn by the tiles before this buffer: it is this fit's.
        s.ref = steady;
        s.ref_raw = now;
        s.have_ref = true;
        s.shift = 0.0;
        if (!any_moved) {
            return false;
        }
        std::memcpy(out, raw, 84 * sizeof(float));
        for (u32 k = 0; k < Cascades336; ++k) {
            if (moved[k]) {
                write_moved(k);
            }
        }
        remember();
        return true;
    }
    // Held: the atlas' own world to shadow, from this frame's screen. And how far this frame's
    // fit has gone from it: ref inverse(now) is a move (and a scale) within the atlas, whose
    // tiles are half its width.
    double shift = 0.0;
    std::memcpy(out, raw, 84 * sizeof(float));
    const Matrix screen_to_world = Product(s.inverse_view, inverse_p);
    // (From the second cascade: the nearest is drawn every frame with the game's own matrices,
    // moved as it was drawn.)
    for (u32 k = 0; k < g_near; ++k) {
        if (moved[k]) {
            write_moved(k);
        }
    }
    for (u32 k = g_near; k < Cascades336; ++k) {
        const Matrix replaced = Product(s.ref[k], screen_to_world);
        for (u32 i = 0; i < 16; ++i) {
            out[16 + 16 * k + i] = static_cast<float>(replaced[i]);
        }
        Matrix inverse_now;
        if (Invert(now[k], inverse_now)) {
            const Matrix move = Product(s.ref_raw[k], inverse_now);
            shift = std::max({shift, std::abs(move[3]) * 2.0, std::abs(move[7]) * 2.0,
                              std::abs(move[0] - 1.0), std::abs(move[5] - 1.0)});
            if constexpr (Common::Log::Enabled) {
                static u32 logs = 0;
                if (logs < 16 || s.frames % 600 == 0) {
                    ++logs;
                    LOG_INFO(Render_Vulkan,
                             "VR shadow hold: frame {} held {} cascade {}: move x {:.5f} y {:.5f} z {:.5f}, scale "
                             "{:.5f} {:.5f} {:.5f}, off axis {:.5f} {:.5f}",
                             s.frames, s.held, k, move[3], move[7], move[11], move[0], move[5], move[10],
                             move[1], move[4]);
                }
            }
        } else {
            shift = 1.0;
        }
    }
    s.shift = shift;
    s.rate = s.held != 0 ? shift / s.held : shift;
    s.worst = std::max(s.worst, shift);
    remember();
    return true;
}

bool Lookup(float* values) {
    if (!Active()) {
        return false;
    }
    std::scoped_lock lock{g_table_mutex};
    for (u32 i = 0; i < g_table_used; ++i) {
        if (std::memcmp(g_table[i].raw.data(), values, sizeof(g_table[i].raw)) == 0) {
            std::memcpy(values, g_table[i].replaced.data(), sizeof(g_table[i].replaced));
            return true;
        }
    }
    return false;
}

} // namespace Vulkan::ShadowHold
