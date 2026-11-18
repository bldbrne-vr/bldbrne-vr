// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <vector>
#include <algorithm>
#include <bit>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <span>

#include "common/types.h"
#include "shader_recompiler/ir/passes/fog_grid.h"

// Fog patches (2026-11-14): fog that belongs to a PLACE. The mist (air_light.h Mist) is
// one blanket, its density a matter of height only, and as a blanket it changed the character of
// every area but the Forbidden Woods. A patch is a soft shape that stands at a place in the world:
// a bank in a hollow, mist over a pool, a strip along a path or an alley. You see it ahead, walk
// into it and out of it; the far view stays what it is. Not a bubble round the hunter.
//
// A patch: a centre, three radii (y is up; x and z the world's, or turned about y: the x radius
// then lies along Turn degrees from the world's x towards its z), a density (per metre, the
// mist's own unit: Mist::Density is 0.12), a soft edge (the outer share of the radius over which it
// thins to nothing), round or a rounded box seen from above, and for one that hugs the ground a
// fall height: the centre's y is then the ground's, the density thins upward by exp(-height /
// fall) and the y radius is only where it ends. A little slow noise of the world place (Noise of
// the density) so a patch is not a clean lens. Glow: its light counts this many times (1: the
// mist's own; the mist's strength was chosen in the Forbidden Woods' dark, and in a brighter place
// the same fog reads as a dark veil: a patch there takes more), its dimming of what is behind it
// does not change.
//
// They live in the fog grid (fog_grid.h, VR_FOG_GRID=1): the rasterizer picks the MaxPatches
// nearest of the hunter's map within ReachM (Pick), uploads them with the grid's parameters, and
// every cell adds them to its mist (host_shaders/vr_fog_grid.comp). So they are lit, shadowed by
// the moon and summed exactly as the mist is, the same in both eyes, and they are there where the
// blanket is not (the grid is built wherever a patch is near). No game shader changes.
// Clear within NearStartM..NearFullM of the eye (the hands stay clear; a shorter way than the
// blanket's: you stand IN a patch), and thinning out from FarStartM to FarEndM of the eye (the
// grid ends at 64 m: a patch never pops in).
// Live key lighting.patches (0..4: times every density; 0: none). vr.ini ground_mist off: none.
// Tests: VR_FOG_PATCH="x,y,z,rx,ry,rz,density[,fall[,edge[,box[,noise[,glow[,turn]]]]]]" (up to four, apart by
// ';') adds patches at world places whatever the map, without a rebuild.
namespace Shader::Optimization::FogPatches {

inline constexpr u32 MaxPatches = FogGrid::MaxPatches;
inline constexpr float NearStartM = 0.8f;
inline constexpr float NearFullM = 3.0f;
inline constexpr float FarStartM = 38.0f;
inline constexpr float FarEndM = 56.0f;
inline constexpr float ReachM = FarEndM;
// (Tests: VR_FOG_CLOCK=seconds stops the fog's clocks there, the banks' swell and wandering and
// the mist's drift: two shots of one moment. Below 0: the clocks run.)
inline float ClockPin() {
    static const float v = [] {
        const char* const env = std::getenv("VR_FOG_CLOCK");
        return env != nullptr ? static_cast<float>(std::atof(env)) : -1.0f;
    }();
    return v;
}
// The taken-over fog stays where it is in the world (2026-11-17: "our fog patches still
// wobble as I move near them ... it seems as if the fog is travelling forward if I move forward
// towards it, and backwards if I move backwards, and it travels with the speed of the hunter").
// The patches thinned out by their distance from the eye (NearStartM to NearFullM, a floor bank's
// own clear distance 2 m), as the game's billboards fade near the camera: a clearing tied to the
// viewer goes with the viewer, a bank's near edge ran ahead of him as he walked at it, and in third
// person it sat round the camera, metres behind the hunter. With the game's fog taken over: the
// common near fade is LensStartM to LensFullM (never on the lens, nothing more), a bank with a fall
// has no clear distance of its own, a hand-placed patch keeps the old fade as its own clear
// distance, and what clears the fog round the hunter is his body (Wake). VR_FOG_WORLD=0: as it was.
namespace World {
inline constexpr float LensStartM = 0.25f;
inline constexpr float LensFullM = 0.7f;
inline bool Anchored() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_FOG_WORLD");
        return FogGrid::FogTaken() && !(env != nullptr && env[0] == '0');
    }();
    return on;
}
inline float NearStart() {
    return Anchored() ? LensStartM : NearStartM;
}
inline float NearFull() {
    return Anchored() ? LensFullM : NearFullM;
}
// A patch's own clear distance by that rule.
inline float Clear(float clear, bool taken, bool falls) {
    if (!Anchored()) {
        return clear;
    }
    if (taken) {
        return falls ? 0.0f : clear;
    }
    return std::max(clear, NearFullM);
}
} // namespace World

// A little of the taken-over fog at the hunter's feet (2026-11-17: "when we WALK
// through the fog and it disappears, we should have the option to have a little bit of it visible
// at our feet (that reacts as we walk)"). The fog thins out near the eye (NearStartM to NearFullM,
// and each bank's own clear distance): walking into a bank it is gone round the hunter. With this,
// that share of the fog's low part (what is at the ground: the grid's shader weighs a bank's fog
// by exp(-height / Organic::LayerM)) stays inside the near clearing, and the hunter's own push
// (Wake) works on it. In the Shadows of Yharnam's room by default (ShadowsRoom::FeetNow);
// everywhere with VR_FOG_FEET_LAYER=share. (A first try the same night made all of the fog full
// from under a metre of the eye and flattened the picture: "did you just flatten it!?".)
namespace Feet {
inline float EverywhereNow() {
    static const float v = [] {
        const char* const env = std::getenv("VR_FOG_FEET_LAYER");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 1.0f) : 0.0f;
    }();
    return v;
}
} // namespace Feet
// The Shadows of Yharnam's room (the Forbidden Woods' clearing between its fog gate and its way
// out): more of the air's own mist there (2026-11-17: "we can also up the
// atmospheric/haze fog during that boss battle (just that one)"): the mist's density times
// HazeNow() with the eye in the room, eased in over the last metres to it (Share).
// VR_FOG_SHADOWS_HAZE=times (1: as the rest of the woods).
namespace ShadowsRoom {
inline constexpr u32 Map = 0x1B00;
inline constexpr float X = -335.0f, Y = -186.0f, Z = 481.0f;
inline constexpr float RadiusM = 32.0f;
// The share of the low fog that stays at the hunter's feet in the room (Feet):
// VR_FOG_SHADOWS_FEET=share (0: none).
inline float FeetNow() {
    static const float v = [] {
        // (No case of its own, the tester, 2026-11-17: "I don't want unique cases": none unless asked.)
        const char* const env = std::getenv("VR_FOG_SHADOWS_FEET");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 1.0f) : 0.0f;
    }();
    return v;
}
inline float HazeNow() {
    static const float v = [] {
        const char* const env = std::getenv("VR_FOG_SHADOWS_HAZE");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 4.0f) : 1.4f;
    }();
    return v;
}
// How much of the room an eye here is in: all of it well inside, none 8 m outside.
inline float Share(u32 map, float x, float y, float z) {
    if (map != Map || std::abs(y - Y) > 8.0f) {
        return 0.0f;
    }
    const float dx = x - X, dz = z - Z;
    const float t = std::clamp((RadiusM + 8.0f - std::sqrt(dx * dx + dz * dz)) / 12.0f, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
} // namespace ShadowsRoom
// A ground-hugging patch whose ground is this far over the eye (Pick).
inline constexpr float AboveStartM = 1.5f;
inline constexpr float AboveEndM = 3.5f;
// (0.3 and 0.2 for some hours of 2026-11-16: that hid the lumps and the fog with them, where the
// game's own shows a wisp up there. The lumps were the noise read by cells too deep for it, mended
// in the grid's shader; the fog is back at its own strength. VR_FOG_ABOVE="share,noise": tests.)
inline constexpr float AboveShare = 1.0f;
inline constexpr float AboveNoise = 1.0f;
inline std::array<float, 2> AboveNow() {
    static const std::array<float, 2> v = [] {
        std::array<float, 2> a{AboveShare, AboveNoise};
        if (const char* const env = std::getenv("VR_FOG_ABOVE")) {
            std::sscanf(env, "%f,%f", &a[0], &a[1]);
        }
        return std::array<float, 2>{std::clamp(a[0], 0.0f, 1.0f), std::clamp(a[1], 0.0f, 1.0f)};
    }();
    return v;
}
// ... with the grid's far plane where it is now (fog_grid.h FarNow: the same shares of it).
inline float FarStartNow() {
    return FarStartM * FogGrid::FarNow() / FogGrid::FarM;
}
inline float FarEndNow() {
    return FarEndM * FogGrid::FarNow() / FogGrid::FarM;
}
// The noise: two octaves of the mist's gyroid at this scale (1 / m: a lump some 8 m across),
// drifting at DriftShare of the mist's wind.
inline constexpr float NoiseScale = 0.75f;
// With the game's fog taken over (its wisps are finer: a lump some 3 m across).
inline constexpr float TakenNoiseScale = 2.0f;
// (Tests: VR_FOG_PATCH_NOISE_SCALE=<1 / m>.)
inline float NoiseScaleNow() {
    static const float v = [] {
        const char* const env = std::getenv("VR_FOG_PATCH_NOISE_SCALE");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.05f, 8.0f)
                              : FogGrid::FogTaken() ? TakenNoiseScale : NoiseScale;
    }();
    return v;
}
inline constexpr float DriftShare = 0.35f;
// How fast the noise changes in place (its own units a second: the wisps form and dissolve where
// they are, as the game's lingering fog's puffs do; 0: it only drifts). VR_FOG_PATCH_EVOLVE (tests).
inline constexpr float Evolve = 0.0f;
inline constexpr float TakenEvolve = 0.25f; // with the game's fog taken over: a wisp lasts some 20 s
inline float EvolveNow() {
    static const float v = [] {
        const char* const env = std::getenv("VR_FOG_PATCH_EVOLVE");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 4.0f)
                              : FogGrid::FogTaken() ? TakenEvolve : Evolve;
    }();
    return v;
}
// A map's patches fade in over this after the hunter's map changes (a warp, a door, a boundary).
inline constexpr float EaseS = 3.0f;

struct Patch {
    u16 map;       // (area << 8) | block, core/vr/game_area.h: 0x1B00 the Forbidden Woods; 0: any
    float x, y, z; // the centre (world; y the ground's with a fall height)
    float rx, ry, rz;
    float density; // per metre at its heart
    float fall;    // m: thins upward by exp(-height / fall); 0: a plain ellipsoid
    float edge;    // 0..1: the outer share of the radius that thins out
    bool box;      // a rounded box seen from above (a street, an alley), else round
    float noise;   // 0..1: how much of the density the drifting noise takes or adds
    float glow;    // its light, times (1: the mist's own)
    float turn;    // degrees about y: the x radius lies along (cos, 0, sin) of it
    float clear{0.0f}; // m from the eye: none of it within 0.4 of this, all of it from this on (0: the
                       // patches' common near fade only). A distant-view bank is not fog in your street.
    float slope_x{0.0f}, slope_z{0.0f}; // its ground's rise per metre along the world's x and z (Slope)
};

// The patches. Places are the game's world coordinates (VR_TEST_POS_LOG; the fog patches' log
// line gives the camera's place and where it looks).
inline constexpr Patch Table[] = {
    // The Forbidden Woods, the first lamp: a bank across the low ground down the path, some 12 m
    // ahead of the lamp towards the houses (on top of the blanket; under the canopy the mist is
    // in the moon's shadow and dark: glow 4). 2026-11-15, the user in the headset, of this bank
    // before the bridge: "way too bright and feels out of place", "less white": thinner (0.6 to
    // 0.4) and its light 1.6 times the mist's, not 4.
    {0x1B00, -199.7f, -76.8f, 244.4f, 9.0f, 4.0f, 6.0f, 0.4f, 1.5f, 0.6f, false, 0.4f, 1.6f, 0.0f},
    // Central Yharnam, the lamp: low in the street towards the gate, 10 m from the lamp (the
    // street is bright at dusk: glow 3, or it reads as a dark veil).
    // (Taken out 2026-11-15: {0x1801, -199.1, -28.65, 60.0, 7 x 2.5 x 7, 0.8 ...}. Fog over a lamp
    // dims the lamp and its halo with it, "the weak lamp halo" of the Yharnam video; at 4.5 m
    // across it still did. Until lamps light a patch enough to keep their glow, no patch on one.)
    // (Taken out 2026-11-15, the user: "remove the extra fog you added in central yharnam": the
    // strip along the terrace behind the lamp, {0x1801, -187.0, -28.65, 74.9, 7 x 2.5 x 3, 0.6,
    // box, turned 45}. Central Yharnam has the game's own fog only.)
    // Cathedral Ward, outside Oedon Chapel: low in the hollow past the kerb by the statue, where
    // the game's own ground fog lies.
    {0x1800, -20.1f, -11.1f, 117.0f, 8.0f, 2.5f, 6.0f, 0.6f, 0.8f, 0.6f, false, 0.4f, 1.3f, 0.0f},
};

// The game's own lingering fog taken over (core/vr/fog_emitters.h): its emitters as patches, put
// here once at start, before the first frame (core/vr/bloodborne_woods_shadows.cpp).
inline std::vector<Patch>& EmitterPatches() {
    static std::vector<Patch> patches;
    return patches;
}

// A taken-over floor bank fitted to the floor it lies on (2026-11-16; the user, of the clouds the
// banks of a floor above made against the sky, turned down for some hours: "we weren't able to fix
// the blockyness we just hid it fully"). The map file gives a bank's emitter as a point; its shape
// here is one size for all, 9 m each way, and where the floor ended sooner the bank hung past the
// ledge in the open air, or went through a wall into the next room. The game's own collision world
// is asked once a bank (core/vr/bloodborne_woods_shadows.cpp, the loop thread, when the hunter
// first comes near): how far the floor goes from the emitter along +x, -x, +z, -z before a wall
// or a drop (FitStepM steps, at most the bank's radius), and where the ground is under it. The
// bank is then moved to the middle of what is free and narrowed to it (FitFrom), never narrower
// than FitMinM. VR_FOG_FIT=0: as the map file has them.
inline constexpr float FitStepM = 1.5f;
inline constexpr float FitMinM = 1.5f;
struct Fit {
    float dx{}, dy{}, dz{}; // the centre's move (dy: to the ground under it)
    float rx{}, rz{};       // its radii
    bool measured{};
    float sx{}, sz{};       // the ground's rise per metre along the world's x and z (SlopeFrom)
};
// A floor bank's ground as a plane (2026-11-16, the user: "making fog like follow stairs if it
// needs to"): with one height for a whole bank it lay level across stairs and slopes, sunk into
// the upper steps and hung over the lower ones. The ground is asked every SlopeStepM along +x, -x,
// +z, -z of the emitter, as far as SlopeReachM (the loop thread, with the fit); the rise per metre
// along an axis is the least squares line through the emitter's own ground. A floor that is all
// but level (under SlopeLevel) stays level: not one number of it changes. Never steeper than
// SlopeMax. VR_FOG_SLOPE=1: on; unset: every bank level, until it is judged on its own.
inline constexpr float SlopeStepM = 1.5f;
inline constexpr float SlopeReachM = 6.0f;
inline constexpr float SlopeLevel = 0.03f;
inline constexpr float SlopeMax = 0.7f;
inline bool SlopeOn() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_FOG_SLOPE");
        return env != nullptr && env[0] == '1';
    }();
    return on;
}
// `samples`: how far along the axis (m, either way), the ground there over the emitter's (m).
inline float SlopeFrom(std::span<const std::array<float, 2>> samples) {
    float along = 0.0f, rise = 0.0f;
    for (const std::array<float, 2>& s : samples) {
        along += s[0] * s[0];
        rise += s[0] * s[1];
    }
    if (!(along > 1e-3f)) {
        return 0.0f;
    }
    const float slope = std::clamp(rise / along, -SlopeMax, SlopeMax);
    return std::abs(slope) < SlopeLevel ? 0.0f : slope;
}
inline Fit FitFrom(const std::array<float, 4>& free, float rx, float rz, float ground_dy) {
    const float xp = std::clamp(free[0], 0.0f, rx), xn = std::clamp(free[1], 0.0f, rx);
    const float zp = std::clamp(free[2], 0.0f, rz), zn = std::clamp(free[3], 0.0f, rz);
    return Fit{0.5f * (xp - xn), ground_dy, 0.5f * (zp - zn), std::max(0.5f * (xp + xn), std::min(FitMinM, rx)),
               std::max(0.5f * (zp + zn), std::min(FitMinM, rz)), true};
}
// (Off unless VR_FOG_FIT=1 since the evening of 2026-11-16: through the clinic's gate the fitted
// banks, cut from 9 m that overlapped to 2 to 5 m with gaps, read as small separate patches; "Did
// you break it???". Kept for a bank past a ledge, to be judged on its own.)
inline bool FitOn() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_FOG_FIT");
        return env != nullptr && env[0] == '1';
    }();
    return on;
}

// Banks that are not one patch copied (2026-11-16: "the fog shouldn't feel like the same
// patch copy pasted next to each other ... They should move slightly differently, they should have
// slightly different sizes"): a taken-over floor bank's own size, turn, height, thickness and slow
// wander, from its place (the same bank the same every time), in Pick. In the grid's shader its
// height and outline follow the noise's wide wave (Billow, EdgeBreak) and banks that overlap merge
// under a soft limit (MergeCap, per metre) instead of adding up: "If they are very close then can
// merge and have areas where there's a bit less of it". VR_FOG_ORGANIC=0: one shape for all;
// VR_FOG_ORGANIC_SHAPE="billow,cap,edge" (tests; 0,0,0: none of the three).
namespace Organic {
inline constexpr float Billow = 0.55f;
// (0.42 on the fog with its wisps until the evening of 2026-11-16; 0.30 on the banks' shapes,
// MergeByShape: gate, alley and yard shot against the game's own, both eyes. Then 0.375, the
// user: "it stacks up to 2 but the second is weaker than the first": with a floor bank's 0.26 a
// second bank on the same place adds half of one, a third a quarter, never past twice one. Then
// 0.40, "a tiny tiny bump if there's more fog there": 1.5, 1.8, 1.9 ... never past 2.1 times one;
// the alley shot at 0.375 and at 0.42, no lump at either.)
inline constexpr float MergeCap = 0.40f;
inline constexpr float EdgeBreak = 0.5f;
inline constexpr float WanderM = 1.2f;
// Every bank's ground this much over its emitter (2026-11-18, once the fog was seen from
// the frame's own camera and no longer rode up with it: "raise 0.15m", then "let's make it 0.1m").
// The same for every bank and every camera. VR_FOG_RAISE=m (tests; 0: at the emitter's height).
inline float RaiseNow() {
    static const float v = [] {
        const char* const env = std::getenv("VR_FOG_RAISE");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), -1.0f, 2.0f) : 0.10f;
    }();
    return v;
}
// Each bank's ground its own (2026-11-16: "in the small random variation you add, there
// should be some vertical variation as well"): up to this much higher, a third of it lower.
// VR_FOG_ORGANIC_LIFT=m (0: none); unset: 0.4.
inline float LiftNow() {
    static const float v = [] {
        const char* const env = std::getenv("VR_FOG_ORGANIC_LIFT");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 2.0f) : 0.4f;
    }();
    return v;
}
// A bank shades its own inside ("if we know how thick a patch is that can affect the light"): the
// light a place of a taken-over floor bank gets falls with the fog of its own bank above it, by
// exp(-k x that column); the heart's whole column stays as bright as without, so the top and the
// thin rim come out brighter and the foot of a thick heart darker. VR_FOG_SELF_SHADE=k (0: none);
// unset: 2.
inline float SelfShadeNow() {
    static const float v = [] {
        const char* const env = std::getenv("VR_FOG_SELF_SHADE");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 16.0f) : 2.0f;
    }();
    return v;
}
// Clearings where much fog lies together ("when we have lots of fog together ... choose areas to
// thinner ... so it doesn't look fully uniform like a cloud, but like... real"): slow lanes of
// thinner fog through crowded banks, a pattern of the ground's x and z some ClearM across that
// drifts with the wind; a lane takes up to ClearShare of the fog where the banks are at their
// merge's limit, next to none of a lone bank's thin rim. VR_FOG_CLEARINGS="share,metres" (share
// 0: none); unset: 0.6 of it, 14 m.
// The merge's limit by the banks' shapes, not by their fog with its wisps (2026-11-16 evening:
// on the wisps too it pressed them flat once the banks were denser, "fully uniform like a cloud";
// lifted to let them through, three banks piled into one opaque lump). VR_FOG_MERGE_SHAPE=0: as
// before.
// A heavy calm pool at the floor, ragged wisps above (2026-11-16 night: the banks read as evenly
// soft clouds; the game's fog is low, cold and heavy, it pools, and what moves is what rises off
// it). The wisps' strength is LayerCalm of itself at the ground and LayerTop of itself high up,
// between by 1 - exp(-height / LayerM). VR_FOG_LAYERS="calm,metres,top" ("1,0.45,1": the same
// at every height, as before).
// (Tried at 0.35, 0.45 m, 1.2 and shot at the gate, the alley and the yard beside the same fog
// without it: the pool came out fuller and more even, its holes near the ground filled. The user:
// "I like the ours before". Off: the same at every height. VR_FOG_LAYERS="0.35,0.45,1.2" for it.)
inline constexpr float LayerCalm = 1.0f;
inline constexpr float LayerM = 0.45f;
inline constexpr float LayerTop = 1.0f;
inline std::array<float, 3> LayersNow() {
    static const std::array<float, 3> v = [] {
        std::array<float, 3> l{LayerCalm, LayerM, LayerTop};
        if (const char* const env = std::getenv("VR_FOG_LAYERS")) {
            std::sscanf(env, "%f,%f,%f", &l[0], &l[1], &l[2]);
        }
        return std::array<float, 3>{std::clamp(l[0], 0.0f, 2.0f), std::clamp(l[1], 0.05f, 10.0f),
                                    std::clamp(l[2], 0.0f, 2.0f)};
    }();
    return v;
}
inline bool MergeByShape() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_FOG_MERGE_SHAPE");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
inline constexpr float ClearShare = 0.6f;
inline constexpr float ClearM = 14.0f;
// How far the wisps' own wide wave bends the lanes (in the lanes' phase: 1.2 is a fifth of a lane's
// width): no straight lane, none that repeats. The third number of VR_FOG_CLEARINGS (0: straight).
inline constexpr float ClearBend = 1.2f;
inline std::array<float, 3> ClearNow() {
    static const std::array<float, 3> v = [] {
        std::array<float, 3> c{ClearShare, ClearM, ClearBend};
        if (const char* const env = std::getenv("VR_FOG_CLEARINGS")) {
            std::sscanf(env, "%f,%f,%f", &c[0], &c[1], &c[2]);
        }
        return std::array<float, 3>{std::clamp(c[0], 0.0f, 1.0f), std::clamp(c[1], 2.0f, 80.0f),
                                    std::clamp(c[2], 0.0f, 6.0f)};
    }();
    return v;
}
inline bool On() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_FOG_ORGANIC");
        return env == nullptr || env[0] != '0';
    }();
    return on;
}
inline std::array<float, 3> ShapeNow() {
    static const std::array<float, 3> v = [] {
        std::array<float, 3> s{Billow, MergeCap, EdgeBreak};
        if (const char* const env = std::getenv("VR_FOG_ORGANIC_SHAPE")) {
            std::sscanf(env, "%f,%f,%f", &s[0], &s[1], &s[2]);
        }
        return std::array<float, 3>{std::clamp(s[0], 0.0f, 0.9f), std::clamp(s[1], 0.0f, 8.0f), std::clamp(s[2], 0.0f, 2.0f)};
    }();
    return v;
}
// The bank as its own: `turn_too` unless it was fitted to its floor along the world's axes.
inline Patch Own(Patch p, float time_s, bool turn_too) {
    u32 h = std::bit_cast<u32>(p.x) * 0x9E3779B1u ^ std::bit_cast<u32>(p.z) * 0x85EBCA77u ^
            std::bit_cast<u32>(p.y) * 0xC2B2AE3Du;
    const auto next = [&h] {
        h ^= h >> 15;
        h *= 0x2C1B3C6Du;
        h ^= h >> 12;
        h *= 0x297A2D39u;
        h ^= h >> 15;
        return static_cast<float>(h & 0xFFFFu) / 65535.0f;
    };
    p.rx *= 0.8f + 0.5f * next();
    p.rz *= 0.8f + 0.5f * next();
    const float tall = 0.75f + 0.65f * next();
    p.fall *= tall;
    p.ry *= tall;
    p.density *= 0.85f + 0.3f * next();
    const float turn = 180.0f * next();
    if (turn_too) {
        p.turn = turn;
    }
    // (Its slow wander: a loop of its own, a minute or two round.)
    const float px = next(), pz = next();
    const float tx = 50.0f + 60.0f * next(), tz = 50.0f + 60.0f * next();
    p.x += WanderM * std::sin(6.2831853f * (time_s / tx + px));
    p.z += WanderM * std::cos(6.2831853f * (time_s / tz + pz));
    // (The last number drawn, so every one before it is what it was.)
    if (const float lift = LiftNow(); lift > 0.0f) {
        p.y += lift * (1.3333f * next() - 0.3333f);
    }
    return p;
}
} // namespace Organic
// Each emitter patch's fit (as EmitterPatches() lists them), written by the loop thread, read by
// Pick: under FitMutex.
inline std::mutex& FitMutex() {
    static std::mutex mutex;
    return mutex;
}
inline std::vector<Fit>& Fits() {
    static std::vector<Fit> fits;
    return fits;
}

// The patches' own colour (2026-11-16, the game's lingering fog taken over: in Central Yharnam's
// warm dusk light the first banks came out cream, the game's are a cold grey-green): a patch's
// light takes this hue (r g b, weighed to a luminance of 1 and then given the air's own
// luminance, so it stays as bright as the place) by its share of the mist in a cell, times
// `mix` (0: the air's colour, as before). VR_FOG_EMIT_TINT="r,g,b,mix" (tests).
// (The evening of 2026-11-16: the grey-green 0.86, 1, 0.95 read cold and flat beside the game's
// warm puff through the clinic's gate; a neutral, faintly warm 1, 0.98, 0.92.)
inline std::array<float, 4> TintNow() {
    static const std::array<float, 4> v = [] {
        std::array<float, 4> t{1.0f, 0.98f, 0.92f, 1.0f};
        if (const char* const env = std::getenv("VR_FOG_EMIT_TINT")) {
            std::sscanf(env, "%f,%f,%f,%f", &t[0], &t[1], &t[2], &t[3]);
        }
        const float lum = std::max(0.3f * t[0] + 0.6f * t[1] + 0.1f * t[2], 1e-3f);
        return std::array<float, 4>{t[0] / lum, t[1] / lum, t[2] / lum, std::clamp(t[3], 0.0f, 1.0f)};
    }();
    return v;
}

// The banks' own light against the air's (2026-11-17: "can we make the fog patches in
// the dream a tiny bit brighter? (same in yharnam, a little bit more white?) - maybe we do that
// across the board ... just a tad"): their colour at this many times the air's own luminance, in
// every area. VR_FOG_EMIT_BRIGHT (1: as it was).
inline constexpr float TakenBright = 1.1f;
inline float BrightNow() {
    static const float v = [] {
        const char* const env = std::getenv("VR_FOG_EMIT_BRIGHT");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 4.0f) : TakenBright;
    }();
    return v;
}

// Tests: VR_FOG_PATCH (above).
inline std::span<const Patch> TestPatches() {
    static std::array<Patch, 4> patches;
    static const u32 count = [] {
        const char* env = std::getenv("VR_FOG_PATCH");
        u32 n = 0;
        while (env != nullptr && *env != 0 && n < patches.size()) {
            float v[13] = {0, 0, 0, 4, 2, 4, 0.3f, 1.0f, 0.6f, 0, 0.4f, 1.0f, 0.0f};
            const int got = std::sscanf(env, "%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3],
                                        &v[4], &v[5], &v[6], &v[7], &v[8], &v[9], &v[10], &v[11], &v[12]);
            if (got >= 7) {
                patches[n++] = Patch{0,    v[0], v[1],        v[2],  v[3],  v[4], v[5],
                                     v[6], v[7], v[8], v[9] > 0.5f, v[10], v[11], v[12]};
            }
            while (*env != 0 && *env != ';') {
                ++env;
            }
            if (*env == ';') {
                ++env;
            }
        }
        return n;
    }();
    return {patches.data(), count};
}

// The columns of the grid a picked patch can be seen in (fog_grid.h RectVec4: first x, first y, last
// x, last y; a column outside them never sets the patch up, vr_fog_grid.comp). `patch`: its 16
// floats as Pick gives them; `rows`: the grid's view to world (three rows of four: the rotation,
// then the camera's place); `frustum`: tan x lo, tan y lo, tan x width, tan y width; the grid's
// columns and rows. Its box in the grid's view (the patch as tall as the shader spreads a low bank
// at its far side), the box's tangents as columns, a column more each way. A patch the
// eye is in or by: every column. Never fewer columns than the patch's fog can be in.
// `shear`: how much higher and lower its ground's slope takes its far sides (m). `grow`: how many
// times its radii its fog can reach (a taken-over bank's outline goes out with the wide wave:
// 1 + 0.375 x Organic::EdgeBreak, as the shader has it).
inline std::array<float, 4> Columns(std::span<const float, 16> patch, const std::array<float, 12>& rows,
                                    const std::array<float, 4>& frustum, u32 gx, u32 gy, float shear = 0.0f,
                                    float grow = 1.0f) {
    const float* const v = patch.data();
    const float d[3] = {v[0] - rows[3], v[1] - rows[7], v[2] - rows[11]};
    float view[3];
    for (u32 c = 0; c < 3; ++c) {
        view[c] = rows[c] * d[0] + rows[4 + c] * d[1] + rows[8 + c] * d[2];
    }
    const float distance = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    // Its half sizes along its own axes (x along (cos, 0, sin) of its turn, y up, z the third), as
    // tall as the shader makes it at its far side.
    const float rx = grow / std::max(v[4], 1e-4f), rz = grow / std::max(v[6], 1e-4f);
    const float ry =
        grow / std::max(v[5], 1e-4f) + 0.5f * (distance + std::max(rx, rz)) * frustum[3] / float(gy) + shear;
    // ... and along the view's right, up and ahead: an ellipsoid's, or a box's (a patch that is a
    // rounded box seen from above lies inside its box).
    float half[3];
    for (u32 c = 0; c < 3; ++c) {
        const float ux = rows[c], uy = rows[4 + c], uz = rows[8 + c];
        const float ex = rx * (v[13] * ux + v[14] * uz), ey = ry * uy, ez = rz * (v[13] * uz - v[14] * ux);
        half[c] = 1.03f * (v[10] > 0.5f ? std::abs(ex) + std::abs(ey) + std::abs(ez)
                                         : std::sqrt(ex * ex + ey * ey + ez * ez));
    }
    std::array<float, 4> rect{0.0f, 0.0f, float(gx - 1), float(gy - 1)};
    const float z_near = view[2] - half[2], z_far = view[2] + half[2];
    if (!std::isfinite(half[0]) || !std::isfinite(half[1]) || !std::isfinite(half[2]) || !std::isfinite(view[0]) ||
        !std::isfinite(view[1]) || !std::isfinite(view[2])) {
        return rect; // (every column)
    }
    if (z_far <= 0.0f) {
        return {1.0f, 1.0f, 0.0f, 0.0f}; // behind the eye: none
    }
    if (z_near > FogGrid::NearM) {
        const auto low = [&](float a) { return a < 0.0f ? a / z_near : a / z_far; };
        const auto high = [&](float a) { return a > 0.0f ? a / z_near : a / z_far; };
        rect[0] = std::floor((low(view[0] - half[0]) - frustum[0]) / frustum[2] * float(gx)) - 1.0f;
        rect[1] = std::floor((low(view[1] - half[1]) - frustum[1]) / frustum[3] * float(gy)) - 1.0f;
        rect[2] = std::ceil((high(view[0] + half[0]) - frustum[0]) / frustum[2] * float(gx)) + 1.0f;
        rect[3] = std::ceil((high(view[1] + half[1]) - frustum[1]) / frustum[3] * float(gy)) + 1.0f;
    }
    for (float& r : rect) {
        r = std::clamp(r, -4.0f, 4096.0f);
    }
    return rect;
}

// The taken-over fog's own life (2026-11-16, the user: "do they all pulsate with the same timing?
// it needs to have some variance so it feels more real"): the game's puffs are each born, swell,
// thin and die on their own clock. A patch's density swells and thins by BreathDepth of itself,
// over its own period (BreathMinS to BreathMaxS, from its place) from its own start, two waves that
// never line up; its mean stays what it was. On the CPU, once a patch a frame.
// (0.85 over 10 to 24 s for some hours: with the banks blinking as the eye walked it read as
// "things ... doing something"; slower and gentler.)
// (0.25 tried the night of 2026-11-16; the user: keep it as it was.)
inline constexpr float BreathDepth = 0.5f;
inline constexpr float BreathMinS = 18.0f;
inline constexpr float BreathMaxS = 40.0f;
inline float Breath(const Patch& p, float time_s) {
    u32 h = std::bit_cast<u32>(p.x) * 0x9E3779B1u ^ std::bit_cast<u32>(p.z) * 0x85EBCA77u ^
            std::bit_cast<u32>(p.y) * 0xC2B2AE3Du;
    h ^= h >> 15;
    h *= 0x2C1B3C6Du;
    h ^= h >> 12;
    const float h1 = static_cast<float>(h & 0xFFFFu) / 65535.0f, h2 = static_cast<float>(h >> 16) / 65535.0f;
    const float a = 6.2831853f * (time_s / (BreathMinS + (BreathMaxS - BreathMinS) * h1) + h2);
    const float wave = 0.65f * std::sin(a) + 0.35f * std::sin(1.83f * a + 6.2831853f * h1);
    return (1.0f - BreathDepth * (0.5f + 0.5f * wave)) / (1.0f - 0.5f * BreathDepth);
}

// The hunter's wake in the taken-over fog: it parts round his body and stays parted behind him for
// a few seconds, wider and fainter as it closes. A point at his feet and the last places he left
// (one every StepM), as the grid's parameters (fog_grid.h WakeVec4). VR_FOG_WAKE=0: none (tests).
namespace Wake {
// (The user, 2026-11-17: "just have them fade like before ... make the fade cinematic as if we push
// them around. BUT the moving around needs to happen when we actually approach very near or
// enter": the clearing the eye's fade made, 2 m and all clear at its heart, but round the hunter's
// own body, with the rim of what he moved aside and the slow closing behind him.)
inline constexpr float BodyRadiusM = 1.8f; // (0.75 and half until 2026-11-17: "the fog doesn't seem to clear that well")
inline constexpr float BodyStrength = 0.95f; // (0.85 at first, a hole: "soften it ... doesn't fully fully go away")
// What he displaced lies thicker round him: the rim's share of a point's strength (at the point's
// radius the fog is as thick as it was, beyond it thicker by up to a fifth of the strength).
inline constexpr float Rim = 0.9f;
// The wisps are shoved outward by this share of their distance from him at the heart.
inline constexpr float Shove = 0.6f;
inline constexpr float StepM = 0.8f;
inline constexpr float StartRadiusM = 1.5f;
inline constexpr float EndRadiusM = 2.4f;
inline constexpr float Strength = 0.6f;
inline constexpr float LifeS = 5.0f;
inline constexpr float ReachM = 16.0f; // cells further along the ray than this never ask
inline constexpr float WarpM = 25.0f;  // further than this in one frame: a warp, the trail forgotten

// Both off until the user has judged them in the headset (2026-11-16: "this should be judged by me
// first... only kept if it's truly better otherwise not worth the hassle"): the hunter's own push
// (VR_FOG_WAKE=1) and the enemies' and NPCs' near him (VR_FOG_WAKE_ENEMIES=1).
inline bool On() {
    static const bool on = [] {
        // (With the fog anchored in the world the hunter's body is what clears it round him: on,
        // unless VR_FOG_WAKE=0. Without: only where asked, as before.)
        const char* const env = std::getenv("VR_FOG_WAKE");
        return FogGrid::FogTaken() && (env != nullptr ? env[0] == '1' : World::Anchored());
    }();
    return on;
}
inline bool Enemies() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_FOG_WAKE_ENEMIES");
        return FogGrid::FogTaken() && env != nullptr && env[0] == '1';
    }();
    return on;
}
// (Tests: VR_FOG_WAKE_GHOST="x0,y,z0,x1,z1,m/s": a walker going to and fro between two places,
// one of the others: the fog's reaction to a body seen from a still camera.) Its place and speed
// along the ground at a time (false: none asked for).
inline bool Ghost(double time_s, std::array<float, 3>& at, std::array<float, 2>& speed) {
    static const std::array<float, 6> ghost = [] {
        std::array<float, 6> g{};
        if (const char* const env = std::getenv("VR_FOG_WAKE_GHOST")) {
            std::sscanf(env, "%f,%f,%f,%f,%f,%f", &g[0], &g[1], &g[2], &g[3], &g[4], &g[5]);
        }
        return g;
    }();
    if (!(ghost[5] > 0.0f)) {
        return false;
    }
    const float length = std::max(std::hypot(ghost[3] - ghost[0], ghost[4] - ghost[2]), 0.1f);
    const float lap = static_cast<float>(std::fmod(time_s * ghost[5] / length, 2.0));
    const float u = lap < 1.0f ? lap : 2.0f - lap;
    const float way = lap < 1.0f ? 1.0f : -1.0f;
    at = {ghost[0] + (ghost[3] - ghost[0]) * u, ghost[1], ghost[2] + (ghost[4] - ghost[2]) * u};
    speed = {way * ghost[5] * (ghost[3] - ghost[0]) / length, way * ghost[5] * (ghost[4] - ghost[2]) / length};
    return true;
}
// The hunter's points (his feet and TrailPoints places he left) and the others' (their feet, no
// trail): together at most FogGrid::MaxWake.
inline constexpr u32 TrailPoints = 3;
inline constexpr u32 OtherPoints = 4;
static_assert(1 + TrailPoints + OtherPoints <= FogGrid::MaxWake);

struct Trail {
    std::array<std::array<float, 3>, TrailPoints> at{};
    std::array<float, TrailPoints> born{};
    u32 count{};
    std::array<float, 3> last{};
    bool any{};
    // (The flowing trail: his path, a place every PathStepS, the newest at `path_head`.)
    static constexpr u32 PathPlaces = 64;
    std::array<std::array<float, 3>, PathPlaces> path{};
    std::array<float, PathPlaces> path_when{};
    u32 path_head{};
    u32 path_count{};
};

// The flowing trail (2026-11-17: "while moving backwards the [fog] appeared almost...
// like in steps, not fluidly"): a point dropped every StepM came in at full strength at once, a
// new clearing popping in behind him each step (in front of the eyes, walking backwards). The
// trail's points are where he was TrailLagS ago, read from his own path: they follow him like a
// tail and never jump; one that has caught up with him (he stands) has faded out by then, within
// TrailNearM of him. Each one's strength and radius by its lag, as a dropped point's by its age.
// VR_FOG_TRAIL=0: the stepped trail, as it was.
inline constexpr std::array<float, TrailPoints> TrailLagS{0.6f, 1.5f, 2.8f};
inline constexpr float PathStepS = 0.06f;
inline constexpr float TrailNearM = 0.9f;
inline bool Flowing() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_FOG_TRAIL");
        return !(env != nullptr && env[0] == '0');
    }();
    return on;
}
// Where he was `lag` seconds before `time_s`, by his path (his place now where it does not reach
// that far back).
inline std::array<float, 3> PathAt(const Trail& t, const std::array<float, 3>& feet, float time_s, float lag) {
    const float when = time_s - lag;
    std::array<float, 3> newer = feet;
    float newer_when = time_s;
    for (u32 n = 0; n < t.path_count; ++n) {
        const u32 i = (t.path_head + Trail::PathPlaces - n) % Trail::PathPlaces;
        if (t.path_when[i] <= when) {
            const float span = newer_when - t.path_when[i];
            const float f = span > 1e-4f ? std::clamp((when - t.path_when[i]) / span, 0.0f, 1.0f) : 1.0f;
            return {t.path[i][0] + (newer[0] - t.path[i][0]) * f, t.path[i][1] + (newer[1] - t.path[i][1]) * f,
                    t.path[i][2] + (newer[2] - t.path[i][2]) * f};
        }
        newer = t.path[i];
        newer_when = t.path_when[i];
    }
    return newer; // (the oldest place known)
}

// His feet's place this frame into the trail; the points as floats (4 * FogGrid::WakeVec4s: the
// head, the strengths, a point each): his own with `own`, then the others' feet. Returns how many
// points.
inline u32 Update(Trail& t, const std::array<float, 3>& feet, float time_s, bool own,
                  std::span<const std::array<float, 3>> others, std::span<float> out) {
    std::ranges::fill(out, 0.0f);
    const auto apart = [](const std::array<float, 3>& a, const std::array<float, 3>& b) {
        return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]));
    };
    // (The clock is a day's, fog_grid: back over midnight, the path forgotten.)
    const bool back = t.path_count != 0 && time_s < t.path_when[t.path_head];
    if (!t.any || back || !(apart(feet, t.last) < WarpM)) {
        t = Trail{};
        t.last = feet;
        t.any = true;
    } else if (Flowing()) {
        t.last = feet;
    } else if (apart(feet, t.last) > StepM) {
        for (u32 i = TrailPoints - 1; i > 0; --i) {
            t.at[i] = t.at[i - 1];
            t.born[i] = t.born[i - 1];
        }
        t.at[0] = t.last;
        t.born[0] = time_s;
        t.count = std::min(t.count + 1, TrailPoints);
        t.last = feet;
    }
    // (Tests: VR_FOG_WAKE_GAIN="strength,radius" times both, to see where the wake is.)
    static const std::array<float, 2> gain = [] {
        std::array<float, 2> g{1.0f, 1.0f};
        if (const char* const env = std::getenv("VR_FOG_WAKE_GAIN")) {
            std::sscanf(env, "%f,%f", &g[0], &g[1]);
        }
        return g;
    }();
    u32 n = 0;
    const auto put = [&](const std::array<float, 3>& at, float radius, float strength) {
        radius *= gain[1];
        strength = std::min(strength * gain[0], 1.0f);
        if (n >= FogGrid::MaxWake) {
            return;
        }
        out[4 + n] = strength;
        float* const v = out.data() + 4 * (1 + FogGrid::MaxWake / 4 + n);
        v[0] = at[0];
        v[1] = at[1];
        v[2] = at[2];
        v[3] = 1.0f / (radius * radius);
        ++n;
    };
    if (Flowing() && (t.path_count == 0 || time_s - t.path_when[t.path_head] >= PathStepS)) {
        t.path_head = t.path_count == 0 ? 0 : (t.path_head + 1) % Trail::PathPlaces;
        t.path[t.path_head] = feet;
        t.path_when[t.path_head] = time_s;
        t.path_count = std::min(t.path_count + 1, Trail::PathPlaces);
    }
    if (own && Flowing()) {
        put(feet, BodyRadiusM, BodyStrength);
        for (const float lag : TrailLagS) {
            const std::array<float, 3> at = PathAt(t, feet, time_s, lag);
            const float near = std::clamp(apart(at, feet) / TrailNearM, 0.0f, 1.0f);
            const float age = lag / LifeS;
            const float strength = Strength * (1.0f - age) * (1.0f - age) * near * near * (3.0f - 2.0f * near);
            if (strength > 1e-3f) {
                put(at, StartRadiusM + (EndRadiusM - StartRadiusM) * age, strength);
            }
        }
    } else if (own) {
        put(feet, BodyRadiusM, BodyStrength);
        for (u32 i = 0; i < t.count; ++i) {
            const float age = (time_s - t.born[i]) / LifeS;
            if (!(age >= 0.0f && age < 1.0f)) {
                continue;
            }
            put(t.at[i], StartRadiusM + (EndRadiusM - StartRadiusM) * age, Strength * (1.0f - age) * (1.0f - age));
        }
    }
    for (u32 i = 0; i < others.size() && i < OtherPoints; ++i) {
        put(others[i], BodyRadiusM, BodyStrength);
    }
    out[0] = static_cast<float>(n);
    out[1] = ReachM;
    out[2] = Rim;
    out[3] = Shove;
    return n;
}
} // namespace Wake

// The parameters' vec4s of one patch (fog_grid.h: from PatchVec4 + PatchHeadVec4s, PatchVec4s each):
//  0 the centre, the density (times the strength)
//  1 1 / the radii, the edge
//  2 the ground's y, 1 / the fall height (0: none), box (0 or 1), noise
//  3 glow, the turn's cosine and sine, 0
inline constexpr u32 PatchVec4s = FogGrid::PatchVec4s;

// The nearest patches of this map to the camera, as floats for the parameters (PatchVec4s vec4 a
// patch, at most MaxPatches). Returns how many.
// `strength`: the hand-placed ones' (0 with the ground mist's setting off); `taken`: the game's
// own fog taken over, which is no setting of ours (the game's is not started: without these the
// place would have none).
// `time_s`: the taken-over fog's own clock (Breath).
// `slopes`: each picked patch's ground slope, two floats a patch (x, z), where asked.
inline u32 Pick(u32 map, const std::array<float, 3>& camera, float strength, float taken, float time_s,
                std::span<float> out, std::span<float> slopes = {}) {
    struct Near {
        Patch patch;
        float distance;
        float seen; // times its density: the strength of its sort, less from below
        bool taken; // the game's own fog taken over
        float noise; // times its noise: smoother from below
    };
    std::array<Near, MaxPatches> near{};
    u32 count = 0;
    const auto consider = [&](const Patch& p, float strength, bool is_taken = false, float breath = 1.0f) {
        if (!(p.density * strength > 1e-5f) || !(p.rx > 0.01f) || !(p.ry > 0.01f) || !(p.rz > 0.01f)) {
            return;
        }
        const float dx = p.x - camera[0], dy = p.y - camera[1], dz = p.z - camera[2];
        // (Fog that lies on a floor a storey over the eye: from below most of it is behind that
        // floor, and what the patch's round shape puts past the floor's edge hung in the open air
        // as bright lumps against the sky. Thinner and smoother from AboveStartM over the eye, down
        // to AboveShare of it and AboveNoise of its noise from AboveEndM: the faint, even wisp the
        // game's own shows up there.)
        const float above =
            p.fall > 0.01f ? std::clamp((dy - AboveStartM) / (AboveEndM - AboveStartM), 0.0f, 1.0f) : 0.0f;
        const float seen = 1.0f - (1.0f - AboveNow()[0]) * above;
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz) - std::max({p.rx, p.ry, p.rz});
        if (!(distance < FarEndNow())) {
            return;
        }
        // (Kept in order of distance: the farthest gives way.)
        const bool full = count == MaxPatches;
        u32 at = full ? MaxPatches - 1 : count++;
        if (full && near[at].distance <= distance) {
            return;
        }
        for (; at > 0 && near[at - 1].distance > distance; --at) {
            near[at] = near[at - 1];
        }
        near[at] = {p, distance, seen * strength * breath, is_taken,
                    1.0f - (1.0f - AboveNow()[1]) * above};
    };
    for (const Patch& p : Table) {
        if (p.map == 0 || p.map == map) {
            consider(p, strength);
        }
    }
    {
        // (Each one fitted to its floor once measured: FitFrom.)
        std::scoped_lock lock{FitMutex()};
        const auto& emitters = EmitterPatches();
        const auto& fits = Fits();
        const bool fit = FitOn() && fits.size() == emitters.size();
        const bool slope = SlopeOn() && fits.size() == emitters.size();
        // (Its ground a plane through the emitter: where the bank has wandered or been moved to,
        // the ground under its centre is the plane's there.)
        const auto sloped = [&](Patch q, const Patch& p, const Fit& f) {
            if (slope && f.measured) {
                q.slope_x = f.sx;
                q.slope_z = f.sz;
                q.y += f.sx * (q.x - p.x) + f.sz * (q.z - p.z);
            }
            q.y += Organic::RaiseNow(); // (every one of the game's banks, whatever its shape)
            return q;
        };
        for (std::size_t i = 0; i < emitters.size(); ++i) {
            const Patch& p = emitters[i];
            if (p.map != map) {
                continue;
            }
            const bool own = Organic::On() && p.fall > 0.01f;
            // (Its swell by the place the map file gives it, not by where it has wandered to.)
            const float breath = Breath(p, time_s);
            if (fit && fits[i].measured) {
                Patch fitted = p;
                fitted.x += fits[i].dx;
                fitted.y += fits[i].dy;
                fitted.z += fits[i].dz;
                fitted.rx = fits[i].rx;
                fitted.rz = fits[i].rz;
                consider(sloped(own ? Organic::Own(fitted, time_s, false) : fitted, p, fits[i]), taken, true, breath);
            } else if (slope) {
                consider(sloped(own ? Organic::Own(p, time_s, true) : p, p, fits[i]), taken, true, breath);
            } else {
                consider(sloped(own ? Organic::Own(p, time_s, true) : p, p, Fit{}), taken, true, breath);
            }
        }
    }
    for (const Patch& p : TestPatches()) {
        consider(p, strength);
    }
    for (u32 i = 0; i < count; ++i) {
        const Patch& p = near[i].patch;
        float* const v = out.data() + 4 * PatchVec4s * i;
        v[0] = p.x;
        v[1] = p.y;
        v[2] = p.z;
        v[3] = std::clamp(p.density * near[i].seen, 0.0f, 16.0f);
        v[4] = 1.0f / p.rx;
        v[5] = 1.0f / p.ry;
        v[6] = 1.0f / p.rz;
        v[7] = std::clamp(p.edge, 0.05f, 1.0f);
        v[8] = p.y;
        v[9] = p.fall > 0.01f ? 1.0f / p.fall : 0.0f;
        v[10] = p.box ? 1.0f : 0.0f;
        // (Past 1 the noise takes all of the density in places: holes and wisps, as the game's
        // own lingering fog has; core/vr/fog_emitters.h.)
        v[11] = std::clamp(p.noise * near[i].noise, 0.0f, 3.0f);
        // (Below 0: the game's own fog taken over, lit by the plain rules of every area.)
        v[12] = std::clamp(p.glow, 0.0f, 16.0f) * (near[i].taken ? -1.0f : 1.0f);
        v[13] = std::cos(p.turn * 0.017453292f);
        v[14] = std::sin(p.turn * 0.017453292f);
        v[15] = std::clamp(World::Clear(p.clear, near[i].taken, p.fall > 0.01f), 0.0f, 200.0f);
        if (slopes.size() >= 2 * (i + 1)) {
            slopes[2 * i] = p.slope_x;
            slopes[2 * i + 1] = p.slope_z;
        }
    }
    return count;
}

} // namespace Shader::Optimization::FogPatches
