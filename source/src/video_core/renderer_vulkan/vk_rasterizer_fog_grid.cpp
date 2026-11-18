// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// The fog grid's side of the rasterizer (shader_recompiler/ir/passes/fog_grid.h; vr_fog_grid.h):
// when it is built (before the frame's scene copy: VrFogGridBeforeDraw), with what
// (VrFogGridParams: the grid's shape for both eyes, the mist's numbers of the frame's air pass,
// the moon's glow and cascades as the light composite would get them, the camera's move since
// the last build for the history), and the buffer the scene's copy binds (VrFogGridBuffer).

#include "video_core/native_stereo/native_stereo.h"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include "common/logging/log.h"
#include "core/vr/c0000_skeleton.h"
#include "core/vr/game_area.h"
#include "core/vr/live_tune.h"
#include "core/vr/map_lights.h"
#include "core/vr/mist_ground.h"
#include "core/vr/moon_screen.h"
#include "core/vr/vr_session.h"
#include "shader_recompiler/ir/passes/air_light.h"
#include "shader_recompiler/ir/passes/fog_grid.h"
#include "shader_recompiler/ir/passes/fog_patches.h"
#include "shader_recompiler/ir/passes/moon_shafts.h"
#include "video_core/renderer_vulkan/vk_gpu_pass_profiler.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"

namespace Vulkan {

namespace {
namespace Air = Shader::Optimization::AirLight;
namespace Grid = Shader::Optimization::FogGrid;
namespace Patches = Shader::Optimization::FogPatches;
namespace Moon = Shader::Optimization::MoonShafts;

// What the grid did over the last second (VR_AIR_DIAG=1): builds, how many read the past, why the
// others did not, the largest turn and step between two builds, and how far the turn the cascades
// give is from the one the reflections pass's rows give (two sources of the same camera).
struct GridDiag {
    u32 builds{}, with_past{}, no_shadows{}, gap{}, warp{}, stale{}, no_axes{}, other{};
    double turn_deg{}, step_m{}, apart_deg{};
    std::chrono::steady_clock::time_point since{std::chrono::steady_clock::now()};
};
GridDiag g_grid_diag;

// Tests: VR_FOG_GRID_TURN=view takes the turn between two builds from the view-to-world rows (the
// reflections pass's) instead of the cascades'.
bool TurnFromView() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_FOG_GRID_TURN");
        return env != nullptr && env[0] == 'v';
    }();
    return on;
}

// The angle of a rotation (rows), degrees.
double AngleDeg(const std::array<double, 9>& r) {
    return std::acos(std::clamp(0.5 * (r[0] + r[4] + r[8] - 1.0), -1.0, 1.0)) * 57.29577951308232;
}

// The lamps' tiles (fog_grid.h Lamps): each light into the boxes of the grid its reach touches,
// the TileLights strongest a box (by what the light gives at the box's nearest place), as four
// bytes a box. `lights`: view position, reach (1 / radius^2), colour, core. The frustum and the
// slices are the grid's (tangents, near, octaves). Per light only the boxes inside the bounds of
// its sphere are looked at.
void LampTiles(std::span<const std::array<float, 8>> lights, const std::array<float, 4>& frustum, float octaves,
               std::span<u32> tiles) {
    namespace L = Grid::Lamps;
    struct Best {
        std::array<float, L::TileLights> score;
        std::array<u8, L::TileLights> light;
        u8 count;
    };
    static std::array<Best, L::Tiles> best; // (the render thread's own: no allocation a frame)
    for (Best& b : best) {
        b.count = 0;
    }
    // The boxes' edges: tangents across and up (a cell's width of slack), depths.
    std::array<float, L::TilesX + 1> edge_x;
    std::array<float, L::TilesY + 1> edge_y;
    std::array<float, L::TilesZ + 1> edge_z;
    for (u32 i = 0; i <= L::TilesX; ++i) {
        edge_x[i] = frustum[0] + frustum[2] * float(i) / float(L::TilesX);
    }
    for (u32 i = 0; i <= L::TilesY; ++i) {
        edge_y[i] = frustum[1] + frustum[3] * float(i) / float(L::TilesY);
    }
    for (u32 i = 0; i <= L::TilesZ; ++i) {
        edge_z[i] = Grid::NearM * std::exp2(octaves * float(i) / float(L::TilesZ));
    }
    const float slack_x = frustum[2] / float(L::TilesX) * 0.125f, slack_y = frustum[3] / float(L::TilesY) * 0.125f;
    for (u32 index = 0; index < lights.size(); ++index) {
        const auto& l = lights[index];
        const float reach = l[3];
        if (!(reach > 1e-6f)) {
            continue;
        }
        const float radius2 = 1.0f / reach, radius = std::sqrt(radius2);
        const float lum = 0.3f * l[4] + 0.6f * l[5] + 0.1f * l[6];
        const float z_lo = l[2] - radius, z_hi = l[2] + radius;
        for (u32 bz = 0; bz < L::TilesZ; ++bz) {
            // (A slice of slack each way: the cells' middles are inside, their ends may not be.)
            const float za = edge_z[bz] * 0.96f, zb = edge_z[bz + 1] * 1.04f;
            if (z_hi <= za || z_lo >= zb) {
                continue;
            }
            const float dz = l[2] < za ? za - l[2] : l[2] > zb ? l[2] - zb : 0.0f;
            // The sphere's reach across at this band, as tangents (the widest of the band's ends).
            const float across = std::sqrt(std::max(radius2 - dz * dz, 0.0f));
            const float near_z = std::max(std::max(z_lo, za), 1e-3f), far_z = std::min(z_hi, zb);
            const auto range = [&](float centre, float lo_edge, float width, u32 n, u32& first, u32& last) {
                const float a = centre - across, b = centre + across;
                const float t0 = std::min(a / near_z, a / far_z), t1 = std::max(b / near_z, b / far_z);
                const float f0 = (t0 - lo_edge) / width * float(n), f1 = (t1 - lo_edge) / width * float(n);
                if (f1 < 0.0f || f0 >= float(n)) {
                    return false;
                }
                first = static_cast<u32>(std::clamp(std::floor(f0), 0.0f, float(n - 1)));
                last = static_cast<u32>(std::clamp(std::floor(f1), 0.0f, float(n - 1)));
                return true;
            };
            u32 x0, x1, y0, y1;
            if (!range(l[0], frustum[0], frustum[2], L::TilesX, x0, x1) ||
                !range(l[1], frustum[1], frustum[3], L::TilesY, y0, y1)) {
                continue;
            }
            for (u32 ty = y0; ty <= y1; ++ty) {
                // The box's bounds in the view (its corners' extremes), the light's distance to them.
                const float ya = edge_y[ty] - slack_y, yb = edge_y[ty + 1] + slack_y;
                const float box_y0 = std::min(ya * za, ya * zb), box_y1 = std::max(yb * za, yb * zb);
                const float dy = l[1] < box_y0 ? box_y0 - l[1] : l[1] > box_y1 ? l[1] - box_y1 : 0.0f;
                for (u32 tx = x0; tx <= x1; ++tx) {
                    const float xa = edge_x[tx] - slack_x, xb = edge_x[tx + 1] + slack_x;
                    const float box_x0 = std::min(xa * za, xa * zb), box_x1 = std::max(xb * za, xb * zb);
                    const float dx = l[0] < box_x0 ? box_x0 - l[0] : l[0] > box_x1 ? l[0] - box_x1 : 0.0f;
                    const float d2 = dx * dx + dy * dy + dz * dz;
                    if (d2 >= radius2) {
                        continue;
                    }
                    const float score = lum * (1.0f / (d2 + l[7]) - reach);
                    Best& b = best[(ty * L::TilesX + tx) * L::TilesZ + bz];
                    u32 at = b.count;
                    if (at == L::TileLights) {
                        if (b.score[at - 1] >= score) {
                            continue; // full, and the weakest kept is stronger
                        }
                        --at;
                    } else {
                        ++b.count;
                    }
                    for (; at > 0 && b.score[at - 1] < score; --at) {
                        b.score[at] = b.score[at - 1];
                        b.light[at] = b.light[at - 1];
                    }
                    b.score[at] = score;
                    b.light[at] = static_cast<u8>(index);
                }
            }
        }
    }
    for (u32 t = 0; t < L::Tiles; ++t) {
        const Best& b = best[t];
        u32 word = 0xFFFFFFFFu;
        for (u32 i = b.count; i-- > 0;) {
            word = (word << 8) | b.light[i];
        }
        tiles[t] = word;
    }
}

// a^T b: the turn from b's frame to a's, both given as rows that take their own view to one
// shared space.
std::array<double, 9> Between(const std::array<double, 9>& a, const std::array<double, 9>& b) {
    std::array<double, 9> out{};
    for (u32 i = 0; i < 3; ++i) {
        for (u32 j = 0; j < 3; ++j) {
            for (u32 r = 0; r < 3; ++r) {
                out[3 * i + j] += a[3 * r + i] * b[3 * r + j];
            }
        }
    }
    return out;
}
} // namespace

// fog_grid.h's parameters for this frame. False: nothing to build (the frame's air pass gave no
// numbers, no camera; neither mist nor moonlit air here). `past`: the last build's lit shares can
// be read. `atlas`: the game has a moon shadow atlas. Out `shadows`: the cells ask the atlas.
bool Rasterizer::VrFogGridParams(std::span<u32> out, bool past, bool atlas, bool& shadows) {
    namespace Mist = Air::Mist;
    std::ranges::fill(out, 0u);
    shadows = false;
    auto& state = vr_fog_grid_state;
    const auto put = [&](u32 vec, u32 c, float v) { out[4 * vec + c] = std::bit_cast<u32>(v); };
    const VR::CameraRecord rec = VR::DrawFrameRecord();
    // The mist's numbers: what the frame's compute host worked out and the scene's copy gets too
    // (VrAirUpload's vr_air.mist_carry: the camera's 12, the mist's 16, the high haze, the
    // distance haze's 3), so the grid and the haze past it are one mist.
    if (!Air::FrameNear(vr_air.mist_carry_frame, rec.id, Air::HalfHostFrames + 1)) {
        return false;
    }
    const std::array<u32, 32>& carry = vr_air.mist_carry;
    const u32* const mist = carry.data() + 12;
    const bool misty = std::bit_cast<float>(mist[1]) > 1e-5f;
    // The moon: its glow in the thin air and its cascades, as the light composite's buffer has
    // them (moon_shafts.h: the area's strength and reach, the live key; 0 steps: no moon here).
    std::array<u32, Moon::BufferDwords> moon;
    VrMoonUpload(moon, true);
    const auto moonf = [&](u32 d) { return std::bit_cast<float>(moon[d]); };
    const bool moon_on = moon[0] != 0;
    const bool glow = moon_on && moonf(11) > 0.0f && moonf(4) > 0.0f;
    // The fog patches near the hunter (fog_patches.h): his map's, the nearest ones; eased in after
    // the map changes. `camera` is the hunter's place (his feet, 1.6 m up; VrAirUpload's
    // mist_place): what the patches picked, the world volume's box and his push are centred on.
    // `eye` is the frame's camera, where the fog is seen from (the carried view's own place).
    std::array<float, 4 * Grid::PatchVec4s * Grid::MaxPatches> patches{};
    std::array<float, 2 * Grid::MaxPatches> patch_slopes{}; // (fog_patches.h Slope)
    u32 patch_count = 0;
    const std::array<float, 3> eye{std::bit_cast<float>(carry[3]), std::bit_cast<float>(carry[7]),
                                   std::bit_cast<float>(carry[11])};
    const std::array<float, 3> camera = vr_air.mist_place_frame == vr_air.mist_carry_frame ? vr_air.mist_place : eye;
    {
        const auto now = std::chrono::steady_clock::now();
        const u32 map = VR::GameArea::Slot().load(std::memory_order_relaxed) >> 16;
        const float dt = std::clamp(std::chrono::duration<float>(now - state.patch_time).count(), 0.0f, 1.0f);
        state.patch_time = now;
        if (map != state.patch_map) {
            state.patch_map = map;
            state.patch_share = 0.0f;
        } else {
            state.patch_share = std::min(state.patch_share + dt / Patches::EaseS, 1.0f);
        }
        const float key = std::clamp(VR::Tune::Get("lighting.patches", 1.0f), 0.0f, 4.0f);
        const float share = state.patch_share * state.patch_share * (3.0f - 2.0f * state.patch_share);
        const float taken = key * share;
        const float strength = VR::GetSettings().ground_mist ? taken : 0.0f;
        if (taken > 0.0f && std::isfinite(camera[0]) && std::isfinite(camera[1]) && std::isfinite(camera[2])) {
            patch_count = Patches::Pick(map, camera, strength, taken,
                                        Patches::ClockPin() >= 0.0f
                                            ? Patches::ClockPin()
                                            : static_cast<float>(std::fmod(
                                                  std::chrono::duration<double>(now.time_since_epoch()).count(), 86400.0)),
                                        patches, patch_slopes);
        }
    }
    if (!misty && !glow && patch_count == 0) {
        return false; // no air to light here: no grid, nothing dispatched
    }
    // The eyes' projections, as VrAirUpload gives them to the scene's copy: the flat camera's
    // cascades, each eye's ndc-to-view matrix.
    std::array<float, 84> cascades;
    u32 cascades_serial;
    {
        std::scoped_lock lock{vr_moon.mutex};
        if (vr_moon.cascades_frame == ~0u ||
            vr_moon.frame - vr_moon.cascades_frame > static_cast<u32>(Air::FrameSlack)) {
            return false;
        }
        cascades = vr_moon.cascades;
        cascades_serial = vr_moon.cascades_serial;
    }
    std::array<std::array<float, 16>, 2> inverse;
    NativeStereo::EyeInverseProjections(cascades.data(), rec, inverse);
    const float half = NativeStereo::Enabled() ? 0.5f * NativeStereo::Ipd(rec) * rec.world_scale : 0.0f;
    if (!std::isfinite(half)) {
        return false;
    }
    // The frustum: the head's, widened to hold both eyes (fog_grid.h): each eye's corners (its
    // matrix at a mid depth), moved by the eye's own shift as seen at the grid's near plane.
    float lo[2] = {1e9f, 1e9f}, hi[2] = {-1e9f, -1e9f};
    for (u32 eye = 0; eye < 2; ++eye) {
        const auto& m = inverse[eye];
        const float shift = eye == 0 ? -half : half;
        for (u32 corner = 0; corner < 4; ++corner) {
            const float h[4] = {corner & 1 ? 1.0f : -1.0f, corner & 2 ? 1.0f : -1.0f, 0.5f, 1.0f};
            float q[4];
            for (u32 r = 0; r < 4; ++r) {
                q[r] = m[4 * r] * h[0] + m[4 * r + 1] * h[1] + m[4 * r + 2] * h[2] + m[4 * r + 3] * h[3];
            }
            // (View z is ahead: a corner that is not is no camera.)
            const float tx = q[0] / q[2], ty = q[1] / q[2];
            if (!(q[2] / q[3] > 1e-6f) || !std::isfinite(tx) || !std::isfinite(ty)) {
                return false;
            }
            lo[0] = std::min(lo[0], tx + std::min(shift, 0.0f) / Grid::NearM);
            hi[0] = std::max(hi[0], tx + std::max(shift, 0.0f) / Grid::NearM);
            lo[1] = std::min(lo[1], ty);
            hi[1] = std::max(hi[1], ty);
        }
    }
    const u32 gx = Grid::Small() ? Grid::SmallX : Grid::Large() ? Grid::LargeX : Grid::GridX;
    const u32 gy = Grid::Small() ? Grid::SmallY : Grid::Large() ? Grid::LargeY : Grid::GridY;
    const u32 gz = Grid::Small() ? Grid::SmallZ : Grid::Large() ? Grid::LargeZ : Grid::GridZ;
    const float guard_x = Grid::GuardCells * (hi[0] - lo[0]) / (float(gx) - 2.0f * Grid::GuardCells);
    const float guard_y = Grid::GuardCells * (hi[1] - lo[1]) / (float(gy) - 2.0f * Grid::GuardCells);
    const std::array<float, 4> frustum{lo[0] - guard_x, lo[1] - guard_y, hi[0] - lo[0] + 2.0f * guard_x,
                                       hi[1] - lo[1] + 2.0f * guard_y};
    if (!(frustum[2] > 1e-3f) || !(frustum[3] > 1e-3f) || !(frustum[2] < 64.0f) || !(frustum[3] < 64.0f)) {
        return false;
    }
    const float octaves = std::log2(Grid::FarNow() / Grid::NearM);
    out[0] = gx;
    out[1] = gy;
    out[2] = gz;
    out[3] = ++state.counter | 0x40000000u; // the tag, never 0
    for (u32 c = 0; c < 4; ++c) {
        put(1, c, frustum[c]);
    }
    put(2, 0, Grid::NearM);
    put(2, 1, Grid::FarNow());
    put(2, 2, octaves);
    put(2, 3, 1.0f / octaves);
    std::copy_n(carry.begin(), 12, out.begin() + 4 * 3); // this frame's view to world
    const std::array<float, 3> colour = VrAirColour();
    for (u32 c = 0; c < 3; ++c) {
        put(6, c, colour[c]);
    }
    // (The mist's dwords: air_light.h Mist's list.)
    out[4 * 6 + 3] = mist[4]; // its reach along the ray
    std::copy_n(mist, 4, out.begin() + 4 * 7); // time, density, 1 / height, the eye's height
    out[4 * 8 + 0] = mist[5];                  // ambient
    out[4 * 8 + 1] = mist[6];                  // forward
    out[4 * 8 + 2] = mist[7];                  // the noise's scale
    out[4 * 8 + 3] = mist[11];                 // extinction
    std::copy_n(mist + 8, 3, out.begin() + 4 * 9); // the wind
    out[4 * 9 + 3] = mist[14];                 // patchy
    out[4 * 10 + 0] = mist[12];                // near start
    out[4 * 10 + 1] = mist[13];                // near full
    out[4 * 10 + 2] = mist[15];                // strength
    out[4 * 10 + 3] = carry[28];               // high: the thin haze's share
    out[4 * 11 + 0] = carry[29];               // 1 / the haze's height
    put(11, 1, Mist::PhaseG);
    // (Towards the moon: fog_grid.h WoodsMoonward; by the woods' share, as the lit gain below.)
    put(11, 2, std::clamp(VR::Tune::Get("lighting.grid_moonward", Grid::WoodsMoonward), 0.0f, 8.0f) *
                   std::clamp(vr_air.moon_full, 0.0f, 1.0f));
    put(11, 3, std::clamp(VR::Tune::Get("lighting.grid_moonward_focus", Grid::MoonwardFocus), 0.5f, 16.0f));
    // Towards the moon (the phases' direction), the eyes' shift.
    if (moon_on) {
        std::copy_n(moon.begin() + 8, 3, out.begin() + 4 * 12);
    }
    put(12, 3, half);
    // The moonlit thin air (moon_shafts.h; the light composite's buffer: density 4, reach 5, g 6,
    // forward 7, strength 11).
    if (glow) {
        put(13, 0, moonf(4));
        put(13, 1, moonf(5));
        put(13, 2, moonf(6));
        put(13, 3, moonf(7));
        // (Less of it while the game's own moon rays draw: fog_grid.h GameRaysShare.)
        {
            const auto now = std::chrono::steady_clock::now();
            const float dt = std::clamp(std::chrono::duration<float>(now - state.game_rays_time).count(), 0.0f, 1.0f);
            state.game_rays_time = now;
            const s64 seen = VR::MoonScreen::g_rays_ms.load(std::memory_order_acquire);
            const bool drawing = seen >= 0 && VR::MoonScreen::NowMs() - seen < Grid::GameRaysFreshMs;
            const float target = drawing ? Grid::GameRaysShareNow() : 1.0f;
            const float step = dt / Grid::GameRaysEaseS;
            const float before = state.game_rays;
            state.game_rays = target > before ? std::min(before + step, target) : std::max(before - step, target);
            if constexpr (Common::Log::Enabled) {
                if ((before == 1.0f) != (state.game_rays == 1.0f)) {
                    LOG_INFO(Render_Vulkan, "VR fog grid: the game's own moon rays {}: the moonlit thin air {}",
                             drawing ? "are drawing" : "have stopped", drawing ? "steps back" : "is all its own again");
                }
            }
        }
        put(14, 0, moonf(11) * (1.0f + (Grid::WoodsGlow - 1.0f) * vr_air.moon_full) * state.game_rays);
    }
    put(14, 1, Moon::NearStartM);
    put(14, 2, Moon::NearFullM);
    put(14, 3, Moon::ExtinctionKnob());
    put(15, 0, Moon::FarKeepKnob());
    // (The far fade ends where the grid does: fog_grid.h GlowFarFade.)
    put(15, 1, Grid::GlowFarFade(Moon::FarFade, glow ? moonf(5) : Moon::FarM, Grid::FarNow()));
    put(15, 2, Grid::TakenShadeNow()[0]); // the game's fog taken over: its own rule (vr_fog_grid.comp)
    put(15, 3, Grid::TakenShadeNow()[1]);
    // The moon's shadow in the cells: the atlas and the cascades (the composite's buffer, word
    // for word), how much of it the mist takes, how many places a cell asks.
    shadows = moon_on && atlas;
    const float taps = std::clamp(std::round(VR::Tune::Get("lighting.grid_taps", float(Grid::Taps))), 1.0f, 8.0f);
    const float woods = std::clamp(vr_air.moon_full, 0.0f, 1.0f);
    put(16, 0, std::clamp(VR::Tune::Get("lighting.grid_contrast",
                                        Grid::Contrast + (Grid::WoodsContrast - Grid::Contrast) * woods),
                          0.0f, 1.0f));
    put(16, 1, std::clamp(VR::Tune::Get("lighting.grid_lit",
                                        Grid::LitGain + (Grid::WoodsLitGain - Grid::LitGain) * woods),
                          0.0f, 8.0f));
    put(16, 2, taps);
    put(16, 3, std::max(taps, float(Grid::FreshTaps)));
    if (moon_on) {
        put(22, 0, moonf(2));
        put(22, 1, moonf(20));
        put(22, 2, moonf(21));
        put(22, 3, shadows ? 1.0f : 0.0f);
        std::copy_n(moon.begin() + 16, 4, out.begin() + 4 * 23);
        std::copy_n(moon.begin() + Moon::HeaderDwords, Moon::Cascades * Moon::CascadeDwords,
                    out.begin() + 4 * Grid::CascadeVec4);
    }
    // The fog patches.
    put(Grid::PatchVec4, 0, float(patch_count));
    // (With the game's fog taken over: never on the lens, and no clearing that goes with the
    // eye; fog_patches.h World.)
    put(Grid::PatchVec4, 1, Patches::World::NearStart());
    put(Grid::PatchVec4, 2, Patches::World::NearFull());
    // (A little of the low fog stays at the hunter's feet: fog_patches.h Feet; in the Shadows of
    // Yharnam's room of its own, everywhere where asked. His own push works on it.)
    const float feet_layer =
        Patches::Organic::On() && Grid::FogTaken()
            ? std::max(Patches::Feet::EverywhereNow(),
                       Patches::ShadowsRoom::Share(state.patch_map, camera[0], camera[1], camera[2]) *
                           Patches::ShadowsRoom::FeetNow())
            : 0.0f;
    put(Grid::PatchVec4, 3, Patches::NoiseScaleNow());
    put(Grid::PatchVec4 + 1, 0, Patches::FarStartNow());
    put(Grid::PatchVec4 + 1, 1, Patches::FarEndNow());
    put(Grid::PatchVec4 + 1, 2, Patches::DriftShare);
    put(Grid::PatchVec4 + 1, 3, Patches::EvolveNow());
    {
        const std::array<float, 3> organic = Patches::Organic::ShapeNow();
        for (u32 c = 0; c < 3; ++c) {
            put(Grid::OrganicVec4, c, Patches::Organic::On() ? organic[c] : 0.0f);
        }
        put(Grid::OrganicVec4, 3, Patches::Organic::On() ? Patches::Organic::SelfShadeNow() : 0.0f);
        const std::array<float, 3> clear = Patches::Organic::ClearNow();
        put(Grid::ClearVec4, 0, Patches::Organic::On() ? clear[0] : 0.0f);
        put(Grid::ClearVec4, 1, 6.2831853f / clear[1]);
        put(Grid::ClearVec4, 2, Patches::Organic::MergeByShape() ? 1.0f : 0.0f);
        put(Grid::ClearVec4, 3, clear[2]);
        const std::array<float, 3> layers = Patches::Organic::LayersNow();
        put(Grid::LayerVec4, 0, Patches::Organic::On() ? layers[0] : 1.0f);
        put(Grid::LayerVec4, 1, 1.0f / layers[1]);
        put(Grid::LayerVec4, 2, Patches::Organic::On() ? layers[2] : 1.0f);
        put(Grid::LayerVec4, 3, feet_layer);
        // The world-fixed volume (fog_grid.h World): its box's low corner, on its cubes' own
        // lattice round the hunter (so a cube is the same place of the world frame after frame).
        namespace W = Grid::World;
        state.volume = W::On() && Patches::Organic::On() && patch_count != 0;
        put(Grid::WorldVec4, 0, std::floor(camera[0] / W::CubeM) * W::CubeM - 0.5f * float(W::SizeX) * W::CubeM);
        put(Grid::WorldVec4, 1, std::floor((camera[1] - 1.6f - W::BelowM) / W::TallM) * W::TallM);
        put(Grid::WorldVec4, 2, std::floor(camera[2] / W::CubeM) * W::CubeM - 0.5f * float(W::SizeZ) * W::CubeM);
        put(Grid::WorldVec4, 3, state.volume ? (W::Mode() == 3 ? 3.0f : 1.0f) : 0.0f);
    }
    // The mist's ground from the map (core/vr/mist_ground.h): the lattice the game's loop grows
    // round the hunter, eased here (a lattice begun anew glides); a cell not known yet at the one
    // level plane's height (the mist's own: the eye's height less its height over the plane).
    if (misty && VR::MistGround::On()) {
        namespace MG = VR::MistGround;
        static_assert(MG::Cells == Grid::GroundCells);
        static MG::Shown shown; // (one rasterizer, one hunter)
        static std::chrono::steady_clock::time_point last{};
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.25f);
        last = now;
        std::array<float, MG::Cells * MG::Cells> ground;
        float corner_x = 0.0f, corner_z = 0.0f;
        if (MG::Show(shown, eye[1] - std::bit_cast<float>(mist[3]), dt, ground, corner_x, corner_z)) {
            put(Grid::GroundVec4, 0, corner_x);
            put(Grid::GroundVec4, 1, corner_z);
            put(Grid::GroundVec4, 2, 1.0f / MG::CellM);
            put(Grid::GroundVec4, 3, 1.0f);
            for (u32 i = 0; i < ground.size(); ++i) {
                put(Grid::GroundVec4 + 1 + i / 4, i % 4, ground[i]);
            }
        }
    }
    {
        // The patches' own colour: the hue at the air's own luminance (fog_patches.h TintNow), a
        // tad brighter (BrightNow).
        const std::array<float, 4> tint = Patches::TintNow();
        // (Under the blood moon their own night's level: air_light.h BloodMoon::BankLevel.)
        const float air_lum = 0.3f * colour[0] + 0.6f * colour[1] + 0.1f * colour[2];
        const float lum = (air_lum + (Air::Mist::BloodMoon::BankLevelNow() - air_lum) * vr_air.blood_moon) *
                          Patches::BrightNow();
        for (u32 c = 0; c < 3; ++c) {
            put(Grid::PatchVec4 + 2, c, tint[c] * lum);
        }
        put(Grid::PatchVec4 + 2, 3, tint[3]);
    }
    for (u32 d = 0; d < 4 * Grid::PatchVec4s * patch_count; ++d) {
        put(Grid::PatchVec4 + Grid::PatchHeadVec4s + d / 4, d % 4, patches[d]);
    }
    // The columns each patch can be seen in (fog_grid.h RectVec4): its bounding sphere (as tall as
    // the shader spreads a low bank at its far side, vr_fog_grid.comp PatchAt) as a box in the
    // grid's view, the box's tangents as columns, a column more each way. A patch the eye is in or
    // by: every column.
    {
        std::array<float, 12> rows;
        for (u32 d = 0; d < 12; ++d) {
            rows[d] = std::bit_cast<float>(carry[d]);
        }
        for (u32 i = 0; i < patch_count; ++i) {
            // (A bank on a slope: its far sides that much higher and lower.)
            const float* const v = patches.data() + 4 * Grid::PatchVec4s * i;
            const float shear = (std::abs(patch_slopes[2 * i]) + std::abs(patch_slopes[2 * i + 1])) /
                                std::max(std::min(v[4], v[6]), 1e-4f);
            // (A taken-over bank's outline goes out past its radii with the wide wave.)
            const float grow =
                v[12] < 0.0f && Patches::Organic::On() ? 1.0f + 0.375f * Patches::Organic::ShapeNow()[2] : 1.0f;
            const std::array<float, 4> rect = Patches::Columns(
                std::span<const float, 16>{patches.data() + 4 * Grid::PatchVec4s * i, 16}, rows, frustum, gx, gy, shear,
                grow);
            for (u32 c = 0; c < 4; ++c) {
                put(Grid::RectVec4 + i, c, rect[c]);
            }
            put(Grid::SlopeVec4 + i, 0, patch_slopes[2 * i]);
            put(Grid::SlopeVec4 + i, 1, patch_slopes[2 * i + 1]);
            // (Its self-shade's gain, vr_fog_grid.comp PatchAt: what keeps its heart's whole
            // column as bright as with no shade; once a bank here, not an exp a bank and cell.)
            float gain = 1.0f;
            if (const float k = Patches::Organic::On() ? Patches::Organic::SelfShadeNow() : 0.0f;
                k > 0.0f && v[12] < 0.0f && v[9] > 0.0f) {
                const float c0 = std::max(k * v[3] / v[9], 1e-4f);
                gain = c0 / (1.0f - std::exp(-c0));
            }
            put(Grid::SlopeVec4 + i, 2, gain);
        }
    }
    // The hunter's wake in the taken-over fog (fog_patches.h Wake): his feet (the mist's camera is
    // his eye, 1.6 m up) and his trail.
    if ((Patches::Wake::On() || Patches::Wake::Enemies() || feet_layer > 0.0f) && patch_count != 0) {
        static Patches::Wake::Trail trail; // (one rasterizer, one hunter)
        std::array<float, 4 * Grid::WakeVec4s> wake;
        // The others near him (VR_FOG_WAKE_ENEMIES): the feet of each enemy's and NPC's body, the
        // lamp shadows' casters (VR::ShadowCasters: within 15 m of him, nearest first).
        std::array<std::array<float, 3>, Patches::Wake::OtherPoints> others{};
        u32 other_count = 0;
        if (Patches::Wake::Enemies()) {
            VR::ShadowCasters casters;
            if (VR::RecordShadowCasters(rec.id, casters)) {
                for (u32 b = 0; b < casters.body_count && other_count < others.size(); ++b) {
                    const auto range = casters.bodies[b];
                    if (range.count == 0 || range.first + range.count > casters.capsule_count) {
                        continue;
                    }
                    float x = 0.0f, z = 0.0f, low = 1e30f;
                    for (u32 c = range.first; c < range.first + range.count; ++c) {
                        const auto& cap = casters.capsules[c];
                        x += 0.5f * (cap.a.x + cap.b.x);
                        z += 0.5f * (cap.a.z + cap.b.z);
                        low = std::min({low, cap.a.y - cap.radius, cap.b.y - cap.radius});
                    }
                    x /= float(range.count);
                    z /= float(range.count);
                    if (std::isfinite(x) && std::isfinite(z) && std::isfinite(low)) {
                        others[other_count++] = {x, low, z};
                    }
                }
            }
        }
        const float wake_time = static_cast<float>(std::fmod(
            std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(), 86400.0));
        std::array<float, 3> feet{camera[0], camera[1] - 1.6f, camera[2]};
        // (His own place where the game's loop gave it: in third person the camera is metres
        // behind him.)
        if (std::array<float, 3> hunter{}; VR::GameArea::Hunter(hunter, 500)) {
            feet = hunter;
        }
        // (Tests: VR_FOG_WAKE_GHOST, fog_patches.h Wake::Ghost: one of the others, an enemy walking
        // to and fro.)
        if (std::array<float, 3> ghost_at{}; other_count < others.size()) {
            std::array<float, 2> ghost_speed{};
            if (Patches::Wake::Ghost(wake_time, ghost_at, ghost_speed)) {
                others[other_count++] = ghost_at;
            }
        }
        Patches::Wake::Update(trail, feet, wake_time, Patches::Wake::On() || feet_layer > 0.0f,
                              std::span<const std::array<float, 3>>{others.data(), other_count}, wake);
        for (u32 d = 0; d < wake.size(); ++d) {
            put(Grid::WakeVec4 + d / 4, d % 4, wake[d]);
        }
    }
    // The stir field's frame (fog_grid.h Stir): how much of the last one stays, and the movers:
    // the hunter's body, the enemies and NPCs near him, his hands when they swing.
    state.stir = state.volume && Grid::Stir::On();
    if (state.stir) {
        namespace S = Grid::Stir;
        const auto now = std::chrono::steady_clock::now();
        const float gone = std::chrono::duration<float>(now - state.stir_time).count();
        const float dt = std::clamp(gone, 0.0f, 0.1f);
        state.stir_time = now;
        // (More than a second since the last field: a loading screen, a menu: begun anew.)
        const bool past = state.stir_built && gone < 1.0f;
        put(Grid::StirVec4, 0, dt);
        put(Grid::StirVec4, 1, 1.0f);
        put(Grid::StirVec4, 3, float(S::Test() & 7));
        put(Grid::StirVec4 + 1, 0, state.stir_corner[0]);
        put(Grid::StirVec4 + 1, 1, state.stir_corner[1]);
        put(Grid::StirVec4 + 1, 2, past ? 1.0f : 0.0f);
        put(Grid::StirVec4 + 2, 0, std::exp(-dt / S::AsideS));
        put(Grid::StirVec4 + 2, 1, std::exp(-dt / S::ClearS));
        put(Grid::StirVec4 + 2, 2, std::exp(-dt / S::ChurnS));
        put(Grid::StirVec4 + 2, 3, 1.0f - std::exp(-dt * S::SpreadPerS));
        u32 movers = 0;
        const auto mover = [&](const std::array<float, 3>& at, float radius, float vx, float vz, float strength,
                               float churn) {
            if (movers >= S::MaxMovers || !(strength > 0.01f) || !std::isfinite(at[0] + at[1] + at[2] + vx + vz)) {
                return;
            }
            const u32 v = Grid::StirVec4 + 3 + 2 * movers++;
            put(v, 0, at[0]);
            put(v, 1, at[1]);
            put(v, 2, at[2]);
            put(v, 3, 1.0f / (radius * radius));
            put(v + 1, 0, vx);
            put(v + 1, 1, vz);
            put(v + 1, 2, std::min(strength, 1.0f));
            put(v + 1, 3, std::clamp(churn, 0.0f, 1.0f));
        };
        // A mover's speed along the ground from its last place (none over a warp's step).
        const auto speed = [&](float x, float z, float last_x, float last_z, bool known) {
            std::array<float, 2> v{};
            if (known && past && dt > 1e-4f) {
                v = {(x - last_x) / dt, (z - last_z) / dt};
                if (std::hypot(v[0], v[1]) > S::MaxSpeedMPerS) {
                    v = {};
                }
            }
            return v;
        };
        // The hunter's body (his own place where the game's loop gave it: fog_patches.h Wake).
        std::array<float, 3> feet{camera[0], camera[1] - 1.6f, camera[2]};
        if (std::array<float, 3> hunter{}; VR::GameArea::Hunter(hunter, 500)) {
            feet = hunter;
        }
        {
            const auto v = speed(feet[0], feet[2], state.stir_hunter[0], state.stir_hunter[2], state.stir_hunter_known);
            mover(feet, Patches::Wake::BodyRadiusM, v[0], v[1], Patches::Wake::BodyStrength,
                  S::BodyChurn * std::hypot(v[0], v[1]) / 4.0f);
            state.stir_hunter = feet;
            state.stir_hunter_known = true;
        }
        // His hands (the weapon's swing): his bones this frame, where the frame has his place.
        if (std::array<VR::Vec3, VR::PlayerBoneCount> bones{};
            (S::Test() & 16) == 0 && rec.id != 0 && rec.has_player_position && VR::RecordPlayerBones(rec.id, bones)) {
            namespace C = VR::Body::C0000;
            static constexpr std::array<u32, 2> Hands{C::R_Weapon, C::L_Weapon};
            for (u32 h = 0; h < 2; ++h) {
                const VR::Vec3& b = bones[Hands[h]];
                const std::array<float, 3> at{rec.player_position.x + b.x, rec.player_position.y + b.y,
                                              rec.player_position.z + b.z};
                const auto v = speed(at[0], at[2], state.stir_hand[h][0], state.stir_hand[h][2], state.stir_hand_known[h]);
                const float strength = S::HandStrength(std::hypot(v[0], v[1]), at[1] - feet[1]);
                mover({at[0], feet[1], at[2]}, S::HandRadiusM, v[0], v[1], strength, strength);
                state.stir_hand[h] = at;
                state.stir_hand_known[h] = true;
            }
        } else {
            state.stir_hand_known = {};
        }
        // The others near him (the lamp shadows' casters): each body's middle, its speed from the
        // nearest of the last frame's.
        {
            std::array<std::array<float, 2>, 8> seen{};
            u32 seen_count = 0;
            VR::ShadowCasters casters;
            if ((S::Test() & 8) == 0 && VR::RecordShadowCasters(rec.id, casters)) {
                for (u32 b = 0; b < casters.body_count && seen_count < seen.size(); ++b) {
                    const auto range = casters.bodies[b];
                    if (range.count == 0 || range.first + range.count > casters.capsule_count) {
                        continue;
                    }
                    float x = 0.0f, z = 0.0f, low = 1e30f;
                    for (u32 c = range.first; c < range.first + range.count; ++c) {
                        const auto& cap = casters.capsules[c];
                        x += 0.5f * (cap.a.x + cap.b.x);
                        z += 0.5f * (cap.a.z + cap.b.z);
                        low = std::min({low, cap.a.y - cap.radius, cap.b.y - cap.radius});
                    }
                    x /= float(range.count);
                    z /= float(range.count);
                    if (!std::isfinite(x) || !std::isfinite(z) || !std::isfinite(low)) {
                        continue;
                    }
                    std::array<float, 2> v{};
                    float nearest = 1.0f;
                    for (u32 o = 0; o < state.stir_other_count; ++o) {
                        const float apart = std::hypot(x - state.stir_others[o][0], z - state.stir_others[o][1]);
                        if (apart < nearest) {
                            nearest = apart;
                            v = speed(x, z, state.stir_others[o][0], state.stir_others[o][1], true);
                        }
                    }
                    mover({x, low, z}, S::OtherRadiusM, v[0], v[1], S::OtherStrength,
                          S::BodyChurn * std::hypot(v[0], v[1]) / 4.0f);
                    seen[seen_count++] = {x, z};
                }
            }
            state.stir_others = seen;
            state.stir_other_count = seen_count;
            // (Tests: the ghost walker, fog_patches.h Wake::Ghost.)
            std::array<float, 3> ghost_at{};
            std::array<float, 2> ghost_speed{};
            if (Patches::Wake::Ghost(std::fmod(std::chrono::duration<double>(now.time_since_epoch()).count(), 86400.0),
                                     ghost_at, ghost_speed)) {
                mover(ghost_at, S::OtherRadiusM, ghost_speed[0], ghost_speed[1], S::OtherStrength,
                      S::BodyChurn * std::hypot(ghost_speed[0], ghost_speed[1]) / 4.0f);
            }
        }
        put(Grid::StirVec4, 2, float(movers));
        state.stir_corner = {std::bit_cast<float>(out[4 * Grid::WorldVec4]), std::bit_cast<float>(out[4 * Grid::WorldVec4 + 2])};
        state.stir_built = true;
    } else {
        state.stir_built = false;
        state.stir_hunter_known = false;
        state.stir_hand_known = {};
        state.stir_other_count = 0;
    }
    // The lamps (fog_grid.h Lamps): the composite's lights of this frame and the flash, and the
    // boxes each one reaches. None listed, or no strength: the count stays 0 (the buffer's zeros)
    // and no cell looks.
    u32 lamp_count = 0;
    if (Grid::Lamps::Mode() != 0) {
        namespace L = Grid::Lamps;
        const float strength = std::clamp(VR::Tune::Get("lighting.grid_lamps", L::Strength), 0.0f, 4.0f);
        std::array<std::array<float, 8>, L::MaxLights> lamps;
        // (A lamp's free lengths before a wall, +x +y +z -x -y -z: far every way unless measured.)
        std::array<std::array<float, 6>, L::MaxLights> lamp_free;
        lamp_free.fill({1e3f, 1e3f, 1e3f, 1e3f, 1e3f, 1e3f});
        {
            std::scoped_lock lock{vr_fog.mutex};
            if (strength > 0.0f && vr_fog.on && vr_fog.grid_lights_frame == vr_fog.frame) {
                lamp_count = std::min<u32>(vr_fog.grid_light_count, L::MaxLights);
                for (u32 i = 0; i < lamp_count; ++i) {
                    const auto& g = vr_fog.grid_lights[i];
                    lamps[i] = {g.position[0], g.position[1], g.position[2], g.reach,
                                g.colour[0],   g.colour[1],   g.colour[2],   g.core2};
                }
            }
        }
        // The map's own lamps (fog_grid.h Lamps::MapLamps; core/vr/map_lights.h): the nearest to the
        // eye within the patches' reach, in the grid's view; one the frame's list has already
        // (within MapLampSameM of it) left out.
        if (strength > 0.0f && L::MapLamps() && lamp_count < L::MaxLights) {
            struct Near {
                const VR::MapLights::Light* light;
                float distance;
                u32 index; // in MapLights::Lamps()
            };
            std::array<Near, L::MaxLights> near{};
            u32 count = 0;
            const float reach = Patches::FarEndNow();
            const auto& map_lamps = VR::MapLights::Lamps();
            for (u32 index = 0; index < map_lamps.size(); ++index) {
                const VR::MapLights::Lamp& lamp = map_lamps[index];
                if (lamp.map != state.patch_map) {
                    continue;
                }
                const float dx = lamp.light.x - camera[0], dy = lamp.light.y - camera[1], dz = lamp.light.z - camera[2];
                const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) - lamp.light.radius;
                if (!(distance < reach)) {
                    continue;
                }
                u32 at = count < near.size() ? count++ : u32(near.size()) - 1;
                if (at == near.size() - 1 && near[at].light != nullptr && near[at].distance <= distance) {
                    continue;
                }
                for (; at > 0 && near[at - 1].distance > distance; --at) {
                    near[at] = near[at - 1];
                }
                near[at] = {&lamp.light, distance, index};
            }
            const auto row = [&](u32 r, u32 c) { return std::bit_cast<float>(carry[4 * r + c]); };
            const u32 listed = lamp_count;
            for (u32 i = 0; i < count && lamp_count < L::MaxLights; ++i) {
                const VR::MapLights::Light& l = *near[i].light;
                const float d[3] = {l.x - row(0, 3), l.y - row(1, 3), l.z - row(2, 3)};
                const std::array<float, 3> view{row(0, 0) * d[0] + row(1, 0) * d[1] + row(2, 0) * d[2],
                                                row(0, 1) * d[0] + row(1, 1) * d[1] + row(2, 1) * d[2],
                                                row(0, 2) * d[0] + row(1, 2) * d[1] + row(2, 2) * d[2]};
                bool same = false;
                for (u32 k = 0; k < listed; ++k) {
                    const float ax = lamps[k][0] - view[0], ay = lamps[k][1] - view[1], az = lamps[k][2] - view[2];
                    same = same || ax * ax + ay * ay + az * az < L::MapLampSameM * L::MapLampSameM;
                }
                if (same || !std::isfinite(view[0]) || !std::isfinite(view[1]) || !std::isfinite(view[2])) {
                    continue;
                }
                // (Its free lengths before a wall, once the loop thread has measured them.)
                {
                    std::scoped_lock lock{VR::MapLights::ReachMutex()};
                    const auto& reaches = VR::MapLights::Reaches();
                    if (near[i].index < reaches.size() && reaches[near[i].index].measured) {
                        const auto& axes = reaches[near[i].index].axes;
                        lamp_free[lamp_count] = {axes[0], axes[2], axes[4], axes[1], axes[3], axes[5]};
                    }
                }
                lamps[lamp_count++] = {view[0],
                                       view[1],
                                       view[2],
                                       1.0f / (l.radius * l.radius),
                                       L::MapLampShare * l.r,
                                       L::MapLampShare * l.g,
                                       L::MapLampShare * l.b,
                                       L::CoreM2};
            }
        }
        if (lamp_count > 0) {
            // (A lamp's light dies in the fog, and counts as much more as keeps the fog FallEvenM
            // from it as bright as it was: fog_grid.h Lamps::Fall*.)
            const std::array<float, 2> fall = L::FallNow();
            put(Grid::LampVec4, 3,
                Grid::FogTaken() ? L::TakenGainNow() * std::exp(fall[0] * L::FallEvenM) - 1.0f : 0.0f);
            put(Grid::LampFallVec4, 0, fall[0]);
            put(Grid::LampFallVec4, 1, fall[1]);
            put(Grid::LampFallVec4, 2, L::FallEvenM);
            put(Grid::LampFallVec4, 3, std::exp(fall[1] * L::FallEvenM));
            // (... and gets further where the fog is much darker than the lamp: Lamps::Far*.)
            const std::array<float, 3> reach_more = L::FarNow();
            put(Grid::LampFarVec4, 0, fall[0] * (1.0f - reach_more[0]));
            put(Grid::LampFarVec4, 1, fall[1] * (1.0f - reach_more[0]));
            put(Grid::LampFarVec4, 2, reach_more[1]);
            put(Grid::LampFarVec4, 3, 1.0f / (reach_more[2] - reach_more[1]));
            for (u32 i = 0; i < lamp_count; ++i) {
                put(Grid::LampFreeVec4 + 2 * i, 3,
                    strength * L::LightAtEven(lamps[i][4], lamps[i][5], lamps[i][6], lamps[i][3], lamps[i][7]));
            }
            if constexpr (Common::Log::Enabled) {
                // (Tests: what the contrast is made of, to choose FarLo and FarHi by.)
                static u32 said = 0;
                if (said < 24 && state.counter % 240 == 2) {
                    ++said;
                    const auto get = [&](u32 v, u32 c) { return std::bit_cast<float>(out[4 * v + c]); };
                    std::string each;
                    for (u32 i = 0; i < lamp_count; ++i) {
                        each += fmt::format(" {:.4f}", strength * L::LightAtEven(lamps[i][4], lamps[i][5], lamps[i][6],
                                                                                 lamps[i][3], lamps[i][7]));
                    }
                    LOG_INFO(Render_Vulkan,
                             "VR fog lamps: the air's luminance {:.3f}, the banks' {:.3f} | the mist: ambient {:.3f} forward "
                             "{:.3f} strength {:.3f}, contrast {:.2f} lit gain {:.2f} | the banks: contrast {:.2f} lit gain "
                             "{:.2f} | {} lamps, each one's light at {:.1f} m:{}",
                             0.3f * get(6, 0) + 0.6f * get(6, 1) + 0.1f * get(6, 2),
                             0.3f * get(Grid::PatchVec4 + 2, 0) + 0.6f * get(Grid::PatchVec4 + 2, 1) +
                                 0.1f * get(Grid::PatchVec4 + 2, 2),
                             get(8, 0), get(8, 1), get(10, 2), get(16, 0), get(16, 1), get(15, 2), get(15, 3), lamp_count,
                             L::FallEvenM, each);
                }
            }
            for (u32 i = 0; i < lamp_count; ++i) {
                for (u32 c = 0; c < 6; ++c) {
                    put(Grid::LampFreeVec4 + 2 * i + c / 3, c % 3, lamp_free[i][c]);
                }
            }
            const std::array<float, 4> shape = L::TakenShapeNow();
            for (u32 c = 0; c < 4; ++c) {
                put(Grid::LampShapeVec4, c, shape[c]);
            }
            put(Grid::LampVec4, 0, float(lamp_count));
            put(Grid::LampVec4, 1, strength);
            put(Grid::LampVec4, 2, L::Mode() == 2 ? 1.0f : 0.0f);
            for (u32 i = 0; i < lamp_count; ++i) {
                for (u32 c = 0; c < 8; ++c) {
                    put(Grid::LampVec4 + 1 + L::LightVec4s * i + c / 4, c % 4, lamps[i][c]);
                }
            }
            LampTiles(std::span<const std::array<float, 8>>{lamps.data(), lamp_count}, frustum, octaves,
                      out.subspan(4 * Grid::LampTileVec4, L::Tiles));
        }
    }
    if constexpr (Common::Log::Enabled) {
        // (Tests: where the camera is and where it looks, to place a patch by.)
        static u32 logged = 0;
        if (logged < 100 && state.counter % 120 == 1) {
            ++logged;
            const auto row = [&](u32 r, u32 c) { return std::bit_cast<float>(carry[4 * r + c]); };
            LOG_INFO(Render_Vulkan,
                     "VR fog patches: {} near (map {:#x}), {} lamps{}, camera ({:.2f} {:.2f} {:.2f}) looks along ({:.3f} "
                     "{:.3f} {:.3f}), its right ({:.3f} {:.3f} {:.3f}), the eye {:.2f} m over the ground",
                     patch_count, state.patch_map, lamp_count, lamp_count > 0 && vr_fog.grid_flash ? " (a flash)" : "", camera[0], camera[1], camera[2], row(0, 2), row(1, 2), row(2, 2),
                     row(0, 0), row(1, 0), row(2, 0), std::bit_cast<float>(mist[3]));
        }
    }

    // The history (fog_grid.h): the camera's move since the last build. Its turn from the moon's
    // light space: the nearest cascade's view-to-shadow rows are scaled axes of it, the very ones
    // this frame's shadow mask was drawn with, and the light space does not turn with the head.
    // Its step from the grid's own camera place (the carried view's: the render camera's, else the
    // hunter's), through the last build's view-to-world rows.
    const auto now = std::chrono::steady_clock::now();
    const float history_key = std::clamp(VR::Tune::Get("lighting.grid_history", 1.0f), 0.0f, 16.0f);
    const bool jitter = VR::Tune::Get("lighting.grid_jitter", 1.0f) > 0.5f;
    std::array<double, 9> axes{}, view{};
    bool axes_ok = moon_on;
    for (u32 r = 0; r < 3 && axes_ok; ++r) {
        const double x = moonf(Moon::HeaderDwords + 4 * r), y = moonf(Moon::HeaderDwords + 4 * r + 1),
                     z = moonf(Moon::HeaderDwords + 4 * r + 2);
        const double len = std::sqrt(x * x + y * y + z * z);
        axes_ok = len > 1e-12 && std::isfinite(len);
        axes[3 * r] = x / len;
        axes[3 * r + 1] = y / len;
        axes[3 * r + 2] = z / len;
    }
    // (Three axes at right angles, or it is no rotation.)
    for (u32 a = 0; a < 3 && axes_ok; ++a) {
        const u32 b = (a + 1) % 3;
        axes_ok = std::abs(axes[3 * a] * axes[3 * b] + axes[3 * a + 1] * axes[3 * b + 1] +
                           axes[3 * a + 2] * axes[3 * b + 2]) < 2e-3;
    }
    for (u32 r = 0; r < 3; ++r) {
        for (u32 c = 0; c < 3; ++c) {
            view[3 * r + c] = std::bit_cast<float>(carry[4 * r + c]);
        }
    }
    // (The place this build is made from: the carried view's own, as its turn above is. The
    // carry may be a few frames old (the host change-over): the camera record's place of THIS
    // frame against a grid built from that older view moved the history by a step the grid never
    // made. With the carry this frame's it is the record's render camera, the very same floats;
    // without a render camera the hunter's place 1.6 m up, a fixed offset that no step sees.)
    const std::array<double, 3> place{eye[0], eye[1], eye[2]};
    const u32 place_kind = rec.has_render_position ? 1u : rec.has_player_position ? 2u : 0u;
    const bool fresh = cascades_serial != state.last_cascades;
    const float dt = std::chrono::duration<float>(now - state.last_time).count();
    bool use_past = false;
    auto& diag = g_grid_diag;
    ++diag.builds;
    if (!shadows) {
        ++diag.no_shadows;
    } else if (!past || !state.last_valid || history_key <= 0.0f || place_kind == 0 ||
               place_kind != state.last_place_kind || state.last_size != std::array<u32, 3>{gx, gy, gz}) {
        ++diag.other;
    } else if (!axes_ok) {
        ++diag.no_axes;
    } else if (!fresh) {
        ++diag.stale;
    } else if (!(dt >= 0.0f) || dt > Grid::GapS) {
        ++diag.gap;
    } else {
        const std::array<double, 9> by_cascades = Between(state.last_axes, axes);
        const std::array<double, 9> by_view = Between(state.last_view, view);
        const std::array<double, 9>& turn = TurnFromView() ? by_view : by_cascades;
        std::array<double, 3> step{};
        double step2 = 0.0;
        for (u32 i = 0; i < 3; ++i) {
            for (u32 r = 0; r < 3; ++r) {
                step[i] += state.last_view[3 * r + i] * (place[r] - state.last_place[r]);
            }
            step2 += step[i] * step[i];
        }
        if (!(step2 <= double(Grid::WarpM) * Grid::WarpM) || !(AngleDeg(turn) <= Grid::MaxTurnDeg)) {
            ++diag.warp;
        } else {
            use_past = true;
            ++diag.with_past;
            for (u32 i = 0; i < 3; ++i) {
                for (u32 c = 0; c < 3; ++c) {
                    put(18 + i, c, static_cast<float>(turn[3 * i + c]));
                }
                put(18 + i, 3, static_cast<float>(step[i]));
            }
            for (u32 c = 0; c < 4; ++c) {
                put(21, c, state.last_frustum[c]);
            }
            diag.turn_deg = std::max(diag.turn_deg, AngleDeg(turn));
            diag.step_m = std::max(diag.step_m, std::sqrt(step2));
            // (How far the two sources of the turn are apart: by_cascades^T by_view.)
            std::array<double, 9> apart{};
            for (u32 i = 0; i < 3; ++i) {
                for (u32 j = 0; j < 3; ++j) {
                    for (u32 k = 0; k < 3; ++k) {
                        apart[3 * i + j] += by_cascades[3 * k + i] * by_view[3 * k + j];
                    }
                }
            }
            diag.apart_deg = std::max(diag.apart_deg, AngleDeg(apart));
        }
    }
    const float share =
        use_past ? std::clamp(1.0f - std::exp(-dt / (Grid::HistoryS * history_key)), Grid::MinBlend, 1.0f) : 1.0f;
    put(17, 0, share);
    put(17, 1, jitter ? 1.0f : 0.0f);
    put(17, 2, float(state.counter & 1023u));
    put(17, 3, use_past ? 1.0f : 0.0f);
    // This build's camera, for the next one.
    state.last_valid = shadows && axes_ok && place_kind != 0;
    state.last_axes = axes;
    state.last_view = view;
    state.last_place = place;
    state.last_place_kind = place_kind;
    state.last_frustum = frustum;
    state.last_size = {gx, gy, gz};
    state.last_time = now;
    state.last_cascades = cascades_serial;
    for (const u32 word : out.subspan(4, 4 * Grid::LampTileVec4 - 4)) {
        if (!std::isfinite(std::bit_cast<float>(word))) {
            state.last_valid = false;
            return false;
        }
    }
    if constexpr (Common::Log::Enabled) {
        static const bool log = [] {
            const char* const env = std::getenv("VR_AIR_DIAG");
            return env != nullptr && env[0] == '1';
        }();
        if (now - diag.since >= std::chrono::seconds(1)) {
            if (log) {
                LOG_WARNING(Render_Vulkan,
                            "VR fog grid diag: builds {} with the past {} | none: no shadows {} gap {} warp {} stale "
                            "cascades {} no axes {} other {} | largest turn {:.3f} deg, step {:.3f} m; the cascades' "
                            "turn and the view rows' apart {:.4f} deg at most | share {:.3f}, place from {}",
                            diag.builds, diag.with_past, diag.no_shadows, diag.gap, diag.warp, diag.stale,
                            diag.no_axes, diag.other, diag.turn_deg, diag.step_m, diag.apart_deg, share,
                            place_kind == 1 ? "the render camera" : place_kind == 2 ? "the hunter" : "nothing");
            }
            diag = GridDiag{};
            diag.since = now;
        }
    }
    return true;
}
static_assert(Air::MistDword + 16 <= Air::RaysDword, "the mist's 16 dwords, as vr_air.mist_carry holds them");
static_assert(Grid::PatchVec4 == Grid::CascadeVec4 + Moon::Cascades * Moon::CascadeDwords / 4 &&
                  Moon::CascadeDwords == 16,
              "the cascades, as the moon's buffer holds them");
static_assert(Patches::MaxPatches == 32, "host_shaders/vr_fog_grid.comp MAX_PATCHES");
static_assert(Grid::MaxWake == 8 && Grid::WakeVec4 == Grid::LampTileVec4 + 480 && Grid::RectVec4 == Grid::WakeVec4 + 11 &&
                  Grid::LampShapeVec4 == Grid::RectVec4 + 32 && Grid::LampFreeVec4 == Grid::LampShapeVec4 + 1 &&
                  Grid::OrganicVec4 == Grid::LampFreeVec4 + 50 && Grid::SlopeVec4 == Grid::OrganicVec4 + 1 &&
                  Grid::ClearVec4 == Grid::SlopeVec4 + 32 && Grid::LayerVec4 == Grid::ClearVec4 + 1 &&
                  Grid::WorldVec4 == Grid::LayerVec4 + 1 && Grid::LampFallVec4 == Grid::WorldVec4 + 1 && Grid::GroundVec4 == Grid::LampFallVec4 + 1 &&
                  Grid::StirVec4 == Grid::GroundVec4 + 257 && Grid::LampFarVec4 == Grid::StirVec4 + 27 && Grid::Stir::MaxMovers == 12 && Grid::World::SizeX == 128 && Grid::World::SizeY == 48,
              "host_shaders/vr_fog_grid.comp wake, patch_rect, lamp_shape, lamp_free, organic, patch_slope, clearings");
static_assert(Grid::Lamps::MaxLights == 25 && Grid::Lamps::TilesX == 20 && Grid::Lamps::TilesY == 12 &&
                  Grid::Lamps::TilesZ == 8 && Grid::Lamps::TileLights == 4,
              "host_shaders/vr_fog_grid.comp MAX_LAMPS, LAMP_TILES, the tile word's four bytes");

// Before the frame's scene copy (the air pass's apply, a permutation made for the grid): the
// grid, built from this frame's numbers. The scene's copy is drawn after the sky and every
// particle, and everything the grid needs is known by then.
void Rasterizer::VrFogGridBeforeDraw(const GraphicsPipeline* pipeline) {
    if (!Grid::Enabled() || !vr_fog_grid || !VR::IsEnabled() || VrInMirrorPass()) {
        return;
    }
    const auto* fs = pipeline->GetStages()[u32(Shader::LogicalStage::Fragment)];
    // (The game's own moon rays drawing: core/vr/moon_screen.h.)
    if (fs != nullptr && (fs->pgm_hash == 0xe55e45eb || fs->pgm_hash == 0xd506a1)) {
        VR::MoonScreen::GameRaysDrawn();
    }
    if (fs == nullptr || !Air::IsApplyProgram(fs->pgm_hash) ||
        std::ranges::none_of(fs->buffers, [](const auto& buffer) {
            return buffer.buffer_type == Shader::BufferType::VrFogGrid;
        })) {
        return;
    }
    // (The program copies other pictures too: only the copy of this frame's scene.)
    const u64 frame = VR::DrawFrameRecord().id;
    if (frame == 0 || !VrAirSceneCopy(*fs, frame)) {
        return;
    }
    const u32 gx = Grid::Small() ? Grid::SmallX : Grid::Large() ? Grid::LargeX : Grid::GridX;
    const u32 gy = Grid::Small() ? Grid::SmallY : Grid::Large() ? Grid::LargeY : Grid::GridY;
    const u32 gz = Grid::Small() ? Grid::SmallZ : Grid::Large() ? Grid::LargeZ : Grid::GridZ;
    bool past = false;
    if (!vr_fog_grid->Prepare(gx, gy, gz, past)) {
        return;
    }
    // The moon's shadow atlas (the light composite's host image: the T# the shadow pass compared
    // against), if the game has one now.
    const AmdGpu::Image tsharp = Shader::GetVrHostImage(Shader::VrHostImage::MoonAtlas);
    const bool has_atlas = tsharp && tsharp.Address() != 0 &&
                           tsharp.GetDataFmt() != AmdGpu::DataFormat::FormatInvalid &&
                           memory->IsValidGpuMapping(tsharp.Address(), 0);
    std::array<u32, Grid::ParamDwords> params;
    bool shadows = false;
    if (!VrFogGridParams(params, past, has_atlas, shadows)) {
        return;
    }
    // Its view, as a draw would bind it (the same texture cache entry), ready to be read.
    vk::ImageView atlas{};
    vk::ImageLayout atlas_layout = vk::ImageLayout::eGeneral;
    scheduler.EndRendering();
    if (shadows) {
        Shader::ImageResource res{};
        res.sharp_idx = Shader::VrHostImageBase + static_cast<u32>(Shader::VrHostImage::MoonAtlas);
        res.is_depth = true;
        VideoCore::TextureCache::ImageDesc desc = TextureDescFor(tsharp, res);
        VideoCore::ImageId image_id = texture_cache.FindImage(desc);
        auto* image = &texture_cache.GetImage(image_id);
        if (auto depth_image_id = texture_cache.GetAssociatedDepth(*image)) {
            image_id = depth_image_id;
            image = &texture_cache.GetImage(image_id);
        }
        auto& view = texture_cache.FindTexture(image_id, desc);
        TransitTexture(*image, false, desc.view_info.range);
        atlas = *view.image_view;
        atlas_layout = image->backing->state.layout;
    }
    // The parameters: a range of the stream buffer, read by our dispatch only.
    auto& stream = buffer_cache.GetUtilityBuffer(VideoCore::MemoryUsage::Stream);
    const u64 offset = stream.Copy(params.data(), sizeof(params), instance.StorageMinAlignment());
    {
        const GpuPassProfiler::Scope pass_scope{scheduler, 0xF06'6B1D'0001ull, GpuPassProfiler::Group::VrHost,
                                                "VR fog grid (its dispatch)"};
        vr_fog_grid->Record(scheduler.Commands(), vk::DescriptorBufferInfo{stream.Handle(), offset, sizeof(params)},
                            atlas, atlas_layout, shadows, vr_fog_grid_state.volume, vr_fog_grid_state.stir);
    }
    vr_fog_grid_state.frame = frame;
    if constexpr (Common::Log::Enabled) {
        static u32 logged = 0;
        if (logged < 12 && (logged < 3 || vr_fog_grid_state.counter % 1800 == 0)) {
            ++logged;
            const auto f = [&](u32 d) { return std::bit_cast<float>(params[d]); };
            LOG_INFO(Render_Vulkan,
                     "VR fog grid: frame {}: {}x{}x{}, tan x {:.3f}..{:.3f}, y {:.3f}..{:.3f}, half eye shift {:.4f}, "
                     "mist density {:.3f}, colour ({:.3f} {:.3f} {:.3f}), moon glow strength {:.3f} over {:.0f} m, "
                     "shadows {} (atlas {:.0f}x{:.0f}, ends {:.1f} {:.1f} {:.1f} {:.1f}), the past {} (share {:.3f})",
                     frame, params[0], params[1], params[2], f(4), f(4) + f(6), f(5), f(5) + f(7), f(51), f(29),
                     f(24), f(25), f(26), f(56), f(53), shadows ? "on" : "off", f(89), f(90), f(92), f(93), f(94),
                     f(95), f(71) > 0.5f ? "read" : "not read", f(68));
        }
    }
}

// The buffer the scene's copy binds: the grid's once it was built (it keeps the last one built;
// the copy's own flag says whether that is this frame's), until then a stand-in of its own: a
// header with tag 0 (nothing built: no mist) and eight cells. Never a range of the shared stream
// buffer (the cells' lesson, vk_rasterizer.cpp's VrAirCells branch).
// (Built, not only made: Prepare makes the buffer, and the frame's parameters can still say no
// before the first dispatch; the copy reads the header's sizes and eight cells before it looks at
// its own flag, and a made buffer's memory is not ours yet. A review's finding, 2026-11-17.)
vk::DescriptorBufferInfo Rasterizer::VrFogGridBuffer() {
    if (vr_fog_grid && vr_fog_grid->Buffer() && vr_fog_grid->Built()) {
        return {vr_fog_grid->Buffer(), 0, vr_fog_grid->Bytes()};
    }
    auto& none = vr_fog_grid_state.none;
    const u64 bytes = Grid::NoneDwords * sizeof(u32);
    if (!none) {
        none = std::make_unique<VideoCore::Buffer>(instance, scheduler, VideoCore::MemoryUsage::Stream, 0,
                                                   VideoCore::AllFlags, bytes);
        std::array<u32, Grid::NoneDwords> words{};
        // (A shape the copy's arithmetic stays finite with: two cells a side, a unit frustum.)
        words[0] = words[1] = words[2] = 2;
        const float shape[8] = {-1.0f, -1.0f, 2.0f, 2.0f, Grid::NearM, Grid::FarM, 7.0f, 1.0f / 7.0f};
        std::memcpy(words.data() + 4, shape, sizeof(shape));
        std::memcpy(none->mapped_data.data(), words.data(), bytes);
    }
    return {none->Handle(), 0, bytes};
}

} // namespace Vulkan
