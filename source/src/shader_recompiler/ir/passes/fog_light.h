// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

#include <cstdlib>

#include "common/types.h"

// Light in the air (2026-11-09: "Proper fog light"): every point light of the frame
// lights the air it reaches, seen along each pixel's view ray up to its surface, over the whole
// screen (a glow added in the light pass itself showed that pass's tiles as faint rectangles).
// The rasterizer collects the frame's point lights from the light pass's own lists (their buffer
// 1, NativeProjection::LightListLayout; Rasterizer::VrFogCapture) and hands the light composite
// (BounceLight::IsCompositeProgram) the MaxLights strongest in a buffer of its own
// (Shader::BufferType::VrFogLights, Rasterizer::VrFogUpload). BouncePass (light_shadow_pass.cpp)
// adds VrFogLight to the composite's colour, in the scene's own light before the tone map (the
// game's exposure and bloom take it as theirs): per light, the integral of its own falloff
// (1 / d^2 - reach) along the ray inside its reach, in closed form, times its colour and Density
// (the buffer's: VR_FOG_DENSITY), softened within about CoreM of the light (no infinite core,
// no flat disc: the ray's closest distance h counts as sqrt(h^2 + CoreM^2)) and fading with h as
// exp(-h / HaloM) (a glow round each light, not a veil over the room: the game's big fill
// lights reach whole rooms); and only while the light is in front of the surface's plane (over
// +-PlaneM; the normal from the depth's neighbours on the side without an edge): a light behind
// a wall or a pillar lights none of the air in front of it. A light within NearEyeM of the eye
// (the hunter's own lantern) counts NearEyeScale of its colour: its haze is all round the eye.
namespace Shader::Optimization::FogLight {

// Startup policy (vr.ini fog_light, VR_FOG_LIGHT=0|1 overrides it): with an upscaler only.
inline bool Enabled(bool vr, bool setting, bool upscaler, const char* override_value) {
    return override_value ? override_value[0] != '0' : vr && setting && upscaler;
}

// The buffer: a header of HeaderDwords (the light count as a u32, the eyes' half shift along
// view x in the lists' units, the density, 0), then up to MaxLights lights of LightDwords each
// (the flat camera's view position xyz, the reach, the colour rgb, 0).
inline constexpr u32 MaxLights = 24;
inline constexpr u32 HeaderDwords = 4;
inline constexpr u32 LightDwords = 8;
// Then the muzzle flash's MuzzleDwords (below, MuzzleDword on): its view position xyz, its reach
// (1 / radius^2), its colour rgb (times its envelope), 1 while it shines.
inline constexpr u32 MuzzleDwords = 8;
inline constexpr u32 MuzzleDword = HeaderDwords + MaxLights * LightDwords;
inline constexpr u32 BufferDwords = HeaderDwords + MaxLights * LightDwords + MuzzleDwords;
// (2026-11-11: 0.006, was 0.003: a soft halo round the lamp posts and the candle walls; 0.010
// made balls of light. Then 0.005, the user in the headset: "tone down light in the air a tiny
// bit". A buffer value: no shader changes with it.)
inline constexpr float Density = 0.005f;
inline constexpr float CoreM = 0.15f;
inline constexpr float HaloM = 1.5f;
inline constexpr float PlaneM = 0.2f;
// TESTS (the flame examples, 2026-11-14): VR_FLAME_CORE_M / VR_FLAME_HALO_M in metres replace
// CoreM / HaloM in the lamps' glow in the air (a smaller core: a tight bright glare at the flame;
// a smaller halo: the glow keeps closer to it). Read once, baked into the programs.
inline float TestMetres(const char* name, float normal) {
    const char* const env = std::getenv(name);
    const float v = env != nullptr ? static_cast<float>(std::atof(env)) : 0.0f;
    return v > 0.001f && v < 100.0f ? v : normal;
}
inline float GlowCoreM() {
    static const float v = TestMetres("VR_FLAME_CORE_M", CoreM);
    return v;
}
inline float GlowHaloM() {
    static const float v = TestMetres("VR_FLAME_HALO_M", HaloM);
    return v;
}
// (The plane test goes by the angle since 2026-11-14, not by PlaneM of distance: the user in the
// headset, the flames' light "lights some triangles weirdly (like flat shading... light stops on
// a surface that doesn't reach the next one at all". The surface's facing comes from the depth:
// one per triangle. Over +-0.2 m a light a few metres off was in front of a triangle or behind it
// within a couple of degrees, so uneven ground and round stones lit triangle by triangle. By the
// cosine between the facing and the way to the light now: the glow in the air all there from
// AirFaceTo down to none at AirFaceFrom (a light must be well behind a surface, about 20 degrees,
// to light none of the air in front of it: a wall still hides the lamp behind it), the fill on
// surfaces from none at FillFaceFrom to all at FillFaceTo (a soft turn like bounced light's own,
// nothing on a face turned away).)
inline constexpr float AirFaceFrom = -0.35f;
inline constexpr float AirFaceTo = 0.05f;
inline constexpr float FillFaceFrom = -0.2f;
inline constexpr float FillFaceTo = 0.75f;
inline constexpr float NearEyeM = 1.5f;
inline constexpr float NearEyeScale = 0.35f;
// ... eased, not stepped (the user in the headset, 2026-11-14, standing at the Forbidden Woods
// lamp: "at the very exact spot there is a light blinking": the lamp was about NearEyeM from the
// head, and each sway of the head switched its glow between NearEyeScale and all of it):
// NearEyeScale within NearEyeInM (the lantern at your hip or in your hand), all of it from
// NearEyeOutM, a smooth step between. The smoke's lights the same way from none to all.
inline constexpr float NearEyeInM = 1.0f;
inline constexpr float NearEyeOutM = 3.0f;
inline constexpr float NearEyeEase(float eye_distance) {
    const float t = eye_distance <= NearEyeInM    ? 0.0f
                    : eye_distance >= NearEyeOutM ? 1.0f
                                                  : (eye_distance - NearEyeInM) / (NearEyeOutM - NearEyeInM);
    return t * t * (3.0f - 2.0f * t);
}
inline constexpr float NearEyeShare(float eye_distance) {
    return NearEyeScale + (1.0f - NearEyeScale) * NearEyeEase(eye_distance);
}
// The capture keeps up to CaptureLights distinct lights a frame (the first ones); lights within
// MergeM of each other share one halo. The weighted centre still lights smoke and mist.
inline constexpr u32 CaptureLights = 96;
inline constexpr float MergeM = 0.5f;
inline constexpr u8 EndMember{CaptureLights};

// Search four packed centres together. Preserve the game's list order when more than one
// cluster is close enough, and keep fixture lanterns separate from nearby flashes.
struct MergeCenters {
    static constexpr u32 PackedFrom = 12;
    alignas(16) std::array<float, CaptureLights> x, y, z;

    void Set(u32 i, const std::array<float, 3>& p) {
        x[i] = p[0];
        y[i] = p[1];
        z[i] = p[2];
    }

    template <typename Groups>
    u32 Find(const Groups& groups, const std::array<float, 3>& p, bool lantern) const {
        const auto close_by = [&](u32 i) {
            const float dx = x[i] - p[0], dy = y[i] - p[1], dz = z[i] - p[2];
            return dx * dx + dy * dy + dz * dz < MergeM * MergeM;
        };
        const u32 count = static_cast<u32>(groups.size());
        u32 i = 0;
        for (; i + 4 <= count; i += 4) {
            u32 mask = 0;
            for (u32 lane = 0; lane < 4; ++lane) {
                mask |= static_cast<u32>(close_by(i + lane)) << lane;
            }
            while (mask) {
                const u32 index = i + std::countr_zero(mask);
                if (groups[index].light.on_lantern == lantern) {
                    return index;
                }
                mask &= mask - 1;
            }
        }
        for (; i < count; ++i) {
            if (close_by(i) && groups[i].light.on_lantern == lantern) {
                break;
            }
        }
        return i;
    }
};

// Pick an actual emitter nearest the previous halo (the cluster centre on first sight).
// Links refer to the bounded capture list for this upload only. A lone light needs no search.
template <typename Lights>
inline u8 HaloMember(const Lights& lights, const std::array<u8, CaptureLights>& previous,
                     u8 last, const std::array<float, 3>& target) {
    if (previous[last] == EndMember) {
        return last;
    }
    const auto distance2 = [&](u8 i) {
        const auto& p = lights[i].world;
        const float x = p[0] - target[0], y = p[1] - target[1], z = p[2] - target[2];
        return x * x + y * y + z * z;
    };
    u8 best = last;
    float nearest = distance2(best);
    for (u8 i = previous[last]; i != EndMember; i = previous[i]) {
        if (nearest == 0.0f) {
            break; // the remembered wick is still at this exact position
        }
        const float d = distance2(i);
        if (d < nearest || (d == nearest && lights[i].world < lights[best].world)) {
            best = i;
            nearest = d;
        }
    }
    return best;
}

// The game's fill lights (placed to brighten whole areas: the moonlight's blues, a fire's warm
// bounce, the grey light it keeps just in front of the camera; no flame, lamp or fire of their
// own) against the real sources (candles reach 2 m, fires and lamps 4 to 5 m, all bright): a
// fill reaches farther than FillReachM or is dimmer than FillLuminance. Only real sources light
// the air (2026-11-09: "big invisible fill lights - these are the ones that should be
// off"). `reach` is the list's falloff term (1 / radius^2), `r g b` its colour.
// (6 m and more; the candle wall of a chalice dungeon, cool white at 0.94 and 4 m, is real.)
inline constexpr float FillReachM = 5.75f;
inline constexpr float FillLuminance = 0.6f;
inline bool IsFillLight(float reach, float r, float g, float b) {
    const float radius = reach > 0.0f ? 1.0f / std::sqrt(reach) : 1e9f;
    return radius > FillReachM || 0.3f * r + 0.6f * g + 0.1f * b < FillLuminance;
}
// The fill rule's reach half (its brightness half: Rasterizer::VrBrightLately).
inline bool IsWideLight(float reach) {
    return !(reach > 0.0f) || 1.0f / std::sqrt(reach) > FillReachM;
}
// The dim line for the lights the game spawns (2026-11-16: the cane men's violet
// lanterns outside Oedon Chapel flicker 0.57..0.63 once a second across FillLuminance, so their
// glow faded and snapped back and their shadows went off and on). Its peak decides: a spawned
// light FillLuminance or brighter within BrightHoldFrames stays bright while it is
// BrightKeepLuminance or more. The map's own lights keep the plain line (Cathedral Ward's blue
// mood fills sit steady from 0.46 to 0.58). BrightLights: the spawned lights followed at once,
// only those under BrightTrackLuminance (a light far over the line never dips under it).
inline constexpr float BrightKeepLuminance = 0.5f;
inline constexpr float BrightTrackLuminance = 0.8f;
// The same light: within SameM, its reach term (1 / radius^2) within this share (the lanterns'
// radius flickers too, 3.91..4.09 m: the term by 10%).
inline constexpr float BrightSameReach = 0.25f;
inline constexpr u32 BrightHoldFrames = 120;
// 32 (16 until the 2026-11-16 review): every spawned light in the tracked band takes a place, the
// wide fills and the camera's light too (their glow and body shadows read the same test), so a
// crowded street could recycle a lantern's place; twice the room costs a few hundred bytes.
inline constexpr u32 BrightLights = 32;

// The lamp memory (2026-11-10; the idea is LanternEngine's "Steady lamp fog", Lantern Engine by
// Awaken): the game lists a light only while it lights a surface in view, so turning the head
// dropped lamps the eye stands near, and their glow popped off and on. The rasterizer keeps each
// light where it was in the world (the light pass's camera: Rasterizer::VrFogCaptureView), fades
// a new one in over FadeInS and a gone one out over FadeOutS, and holds the map's own lamps
// while unlisted for HoldS, or as long as they are within HoldNearM. The lights the game spawns
// (a carried torch, a fire bottle's flash) are not held: they fade out once gone. A listed light
// is the remembered one within SameM whose radius differs by less than SameRadius (a share).
// Over MaxLights, a light drawn last frame counts KeepChosen times: lights close in importance
// do not trade places as the head moves. A camera move over JumpM in a frame (a lamp's warp, a
// menu's preview camera) forgets them all.
inline constexpr float FadeInS = 1.0f;
inline constexpr float FadeOutS = 1.0f;
inline constexpr float HoldS = 30.0f;
inline constexpr float HoldNearM = 40.0f;
inline constexpr float SameM = 0.75f;
inline constexpr float SameRadius = 0.25f;
inline constexpr float KeepChosen = 1.25f;
inline constexpr float JumpM = 20.0f;
inline constexpr u32 MemoryLights = 128;
// Flashes (2026-11-11, of a gunshot: "a small light that can cast shadows momentarily
// ... more dynamic stuff"): a light the game spawns lights the air at once (FlashInS: within a
// frame), not over FadeInS; one gone within FlashLifeS of its first listing (a shot, a fire
// bottle's burst, an explosion) fades out over FlashOutS, a longer one (a carried torch) over
// FadeOutS. The map's own lamps keep the slow fades.
inline constexpr float FlashInS = 0.01f;
inline constexpr float FlashLifeS = 0.6f;
inline constexpr float FlashOutS = 0.12f;
// Another view's camera (a planar reflection's) among the light pass's light208 buffers.
// The user in the headset, 2026-11-17, the Forbidden Woods with water_reflections=1: a lantern's
// light on two gravestones went out for one to three frames about twice a second, and so did
// shadows elsewhere ("static shadows jump around"). Measured in the mock at his spot: only with
// water_reflections on (23 blink frames of 238; every other setting of his on and that one off:
// none), and "frames whose light lists' camera was not the AO pass's" went from 1 a run to 4299.
// The reflection's pass draws the scene again through a camera mirrored at the water, with its
// own light208 and light lists; the rasterizer told that pass by the order of the recorded draws
// (NativeProjection::MirrorPass: from a draw that pairs a mirrored camera to one that pairs any
// other), which a draw inside it with the scene's own camera ends early. The pass's next light208
// then counted as the frame's camera: the lamps' shadows, the air's lights and the fog grid's
// lamps were placed through the reflection's camera for that frame. That camera is a proper one
// (the game hands it over standing under the water with its pitch turned over, determinant 1:
// the day's log), so its rows do not tell it apart; its place does: the frame's own render
// camera's is known (VR::CameraRecord::render_position), each eye's within half the eye distance
// of it and the reflection's twice the camera's height over the water away (36 m at his spot).
// A light208 (its rows view to world, row-major, a place at 3, 7, 11) further than FrameViewM
// from the frame's render camera is another view's: never the frame's camera, and its lists never
// the frame's lights. (A frame with no render camera known: as before, by the pass alone.)
// VR_FOG_MIRROR_VIEWS=1 (tests): by the pass alone.
inline constexpr float FrameViewM = 0.5f;
inline constexpr bool IsFramesView(const float* m, float x, float y, float z) {
    const float dx = m[3] - x, dy = m[7] - y, dz = m[11] - z;
    return dx * dx + dy * dy + dz * dz <= FrameViewM * FrameViewM;
}

// Buffer 0 and buffer 1 of one light draw. A missing/unreadable camera can use this frame's view;
// a confirmed reflection suppresses only its own list. Rows are written only for a paired view.
class ListView {
public:
    using Rows = std::array<float, 12>;

    void Set(const float* view) {
        std::copy_n(view, rows.size(), rows.begin());
        source = Source::Paired;
    }
    void Reject() { source = Source::Reflection; }
    const Rows* Resolve(const Rows& frame, u64 view_frame, u64 frame_id) const {
        switch (source) {
        case Source::Paired: return &rows;
        case Source::Reflection: return nullptr;
        default: return view_frame == frame_id ? &frame : nullptr;
        }
    }

private:
    enum class Source : u8 { Frame, Paired, Reflection };
    Source source = Source::Frame;
    Rows rows;
};

inline bool MirrorViewsByPassOnly() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_FOG_MIRROR_VIEWS");
        return env != nullptr && env[0] == '1';
    }();
    return on;
}
// The map's fixtures among the lights the game spawns (the user in the headset, 2026-11-17, the
// Forbidden Woods: "I GET BLINKING LIGHTS ... the lantern just in front of me"; "I need it fixed
// for good not just another heuristic"). A hanging lantern's light is no light of the map's file:
// the game spawns it with the flame and lists it only in frames where it lights a surface in
// view, so as a spawned light its glow came at once and went in FlashOutS with every listing
// (measured: core/vr/map_objects.h). What tells it from a torch or a shot is the map's own file:
// its 2.5 m flame sits on a verified lantern model. A remembered light becomes a fixture there
// unless a listing has found it off every lantern (a torch carried past a lantern was
// seen off it before, or is seen off it next: never one, or no longer, for as long as it is
// remembered); a listing while the hunter's map has no table (not known yet at a load) tells
// nothing and changes nothing. A fixture fades in like the map's lamps and is held like them
// while the game leaves it out.
// VR_FOG_FIXTURES=0: as it was (tests).
inline bool Fixtures() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_FOG_FIXTURES");
        return !(env != nullptr && env[0] == '0');
    }();
    return on;
}
// The halo's brightness (the user, the same day, of a Woods lantern in the mock: "it's still
// blinking but slowly.. I would prefer if the halo doesn't really blink"): the game swings a
// lantern's light from 1.2 to 2.0 and back in about 1.6 s (a triangle; measured), and the halo
// took every frame's value. A map's lamp or fixture lights the air with its colour's running mean
// over SteadyS and HaloBreath of the frame's swing round it (then, of the mean alone: "I would
// have liked to have the halo shrinking and growing like we had it in the mock before.. gently and
// without large value changes (so maybe it goes from value to value - 20% and then back)": half
// the game's swing, 1.4..1.8 where the light goes 1.2..2.0, its low 22% under its high); the
// lights the game spawns otherwise (a torch, a shot) keep the frame's value.
// VR_FOG_STEADY=0: every light the frame's value (tests).
inline constexpr float SteadyS = 2.5f;
inline constexpr float HaloBreath = 0.5f;
inline constexpr std::array<float, 3> HaloColour(const std::array<float, 3>& mean, const std::array<float, 3>& now) {
    return {mean[0] + HaloBreath * (now[0] - mean[0]), mean[1] + HaloBreath * (now[1] - mean[1]),
            mean[2] + HaloBreath * (now[2] - mean[2])};
}
inline bool SteadyHalo() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_FOG_STEADY");
        return !(env != nullptr && env[0] == '0');
    }();
    return on;
}
struct Fixture {
    bool is{};      // the map's own fixture
    bool strayed{}; // seen off every object: a carried light
    constexpr bool operator==(const Fixture&) const = default;
};
// A remembered light's state after a listing (a new entry starts from Fixture{}). `map`: the
// map's own light (a lamp already); `known`: the hunter's map has a table of its objects;
// `on_object`: the table has one where the light is.
inline constexpr Fixture FixtureNext(Fixture was, bool map, bool known, bool on_object) {
    if (map) {
        return {false, was.strayed};
    }
    if (!known) {
        return was;
    }
    return on_object ? Fixture{!was.strayed, was.strayed} : Fixture{false, true};
}
// Flames only (2026-11-11: "it lights misc lights that are not connected to lamps,
// torches, etc. Like the game's own environmental pointlights, which can create glows in random
// places ... I really like it around candles and torches though"): the game flickers the light of
// every flame (candles, lamp posts, torches, pyres: their colour changes from frame to frame,
// +-10 to 20%), while the lights it places to set a mood (the blue moonlight fills round the
// Tomb, warm fills by a door) hold perfectly steady. The lamp memory follows each light's
// brightness (its mean and its mean change, over FlickerS) and counts a map light as a flame
// once its change is FlameFlicker of its mean: it lights the air as it fades in (over FlameInS),
// and stops (over FlameOutS) after long steadiness. The lights the game spawns (your lantern, a
// torch, a shot's flash, a fire bottle) always count. VR_FOG_FLAMES=0: every light, as before.
// Then (the user, the same day: "lots of candles didn't have the halo glow anymore (that I really
// liked)"): many of the game's candles hold steady too (the Hunter's Dream's: white, 2 m), so a
// steady light is dropped only when it is a mood light: wider than MoodRadiusM and blue (its blue
// more than MoodBlue times its red: the moonlight fills, 3.5 to 5 m, blue 1.6 to 5 times red).
// Candles, lamps and every warm or white light keep their glow whether they flicker or not.
// Then (the user, after the headset: "more halos than I liked"): any steady light wider than
// MoodRadiusM is a mood light, warm or blue: only the small steady ones (candles, 2 m) and every
// flame that flickers (lamp posts, fires, torches) keep their glow. MoodBlue is no longer used.
inline constexpr float MoodRadiusM = 3.0f;
inline constexpr float MoodBlue = 1.4f;
inline bool IsMoodLight(float reach, float /*r*/, float /*b*/) {
    const float radius = reach > 0.0f ? 1.0f / std::sqrt(reach) : 1e9f;
    return radius > MoodRadiusM;
}
inline constexpr float FlickerS = 0.5f;
inline constexpr float FlameFlicker = 0.02f;
inline constexpr float FlameInS = 0.25f;
inline constexpr float FlameOutS = 3.0f;
// A light's record dword 7: 20 or more (20, 50, 1000) on the map's own lights, 1 or less on the
// lights the game spawns (LanternEngine's finding).
inline constexpr float MapLightWord = 5.0f;
inline bool IsMapLight(float word7) {
    return word7 >= MapLightWord;
}

// The air's colour of a light (LanternEngine's lamp fog): a light brighter than AirCap in its
// strongest channel scaled down to it (a fire's white-hot core does not blow the mist out), and
// its hue AirGrey of the way to its own grey (warm haze, not orange paint).
inline constexpr float AirCap = 4.0f;
// A small light's glow (a firefly's 0.8 m light drew a ball of glow 1.6 m across): its share is
// the square of its reach over SmallLightM, at most 1 (a lantern's 2.5 m light keeps all of it).
inline constexpr float SmallLightM = 2.5f;
inline constexpr float AirGrey = 0.25f;
// The air's light at a pixel at most (and a number).
inline constexpr float MaxAir = 64.0f;

// Flames fill the room (2026-11-11: "the fake raytrace lighting, with candles casting
// shadows and lighting rooms"): a flame's light bounces round the room it is in, so the far walls
// of a candlelit chapel glow faintly warm, not only the pools under each candle. The composite
// adds, per light of the buffer among the first FillLights (the strongest; flames only, as the
// air's), its air colour times a soft window of the surface's distance from it, out to
// FillRadiusScale of its reach's radius ((1 - d^2 / R^2)^2), while the light is in front of the
// surface's plane (the air's own plane test: no light through a wall), times the surface's own
// colour and the buffer's fill strength (header dword 3: FillStrength, VR_FLAME_FILL overrides it:
// a buffer value, no shader change). No shadows (bounced light is soft), no noise.
inline constexpr u32 FillLights = 8;
inline constexpr float FillRadiusScale = 2.5f;
// (0.05 since the same day, the user on the 0.15 pictures: "I don't like the flame fill.. it is
// too strong".)
inline constexpr float FillStrength = 0.05f;

// The muzzle flash (2026-11-11: "I want a light from the muzzle, that one needs to cast
// shadows and look cool! I don't mind the bullet light"): the game's own light rides the bullet.
// When a light the game spawns is first listed within MuzzleNearM of the eye (a shot: a gun, the
// cannon, a fire bottle's throw), a flash is lit where it was born: MuzzleColour times
// MuzzleIntensity (VR_TUNE_FILE lighting.muzzle), full for MuzzleHoldS, then fading with a
// MuzzleFadeS time constant, gone after MuzzleLifeS. The composite lights every surface within
// MuzzleRadiusM of it (the game's falloff, the surface's colour, its normal) and shadows it by a
// march of MuzzleSteps through the eye's depth (the lamp shadows' march): the barrel, the hand and
// the beast in front of it throw sharp shadows for that instant. Nothing at all between shots.
inline constexpr float MuzzleNearM = 2.5f;
// ... only with VR_MUZZLE_FROM_LISTS=1 (tests) since 2026-11-12 night: a lamp the game drops out
// of view and lists anew coming back (a wall lantern is a spawned light) lit a shot's flash beside
// the player each time. Every shot of a pistol, rifle, Blunderbuss or cannon is published by the
// gun itself (core/vr/muzzle_flash.h); this was the way for other flashes.
inline bool MuzzleFromLists() {
    static const bool on = [] {
        const char* const env = std::getenv("VR_MUZZLE_FROM_LISTS");
        return env != nullptr && env[0] == '1';
    }();
    return on;
}
// (Then the user: "the whole point of the muzzle flash is that it should light the surroundings
// and cast shadows": 30 and 12 m, held 50 ms, were 10, 7 m and 30 ms. VR_TUNE_FILE keys
// lighting.muzzle, lighting.muzzle_radius, lighting.muzzle_hold, lighting.muzzle_fade.)
// (60 ms since 2026-11-15, the user: "maybe give it 10ms more in duration"; 30 ms in v27, then
// "give the flash 20ms more on".)
inline constexpr float MuzzleHoldS = 0.06f;
// (2026-11-11, the user: "increase the range ... so when we fire our surroundings light up for a
// bit": twice the light, a softer core (1 / (d^2 + MuzzleSoftM2), was 0.09: the hand and barrel
// no brighter than before), a 20 m reach, a longer fade; it lit some 4 m before.)
// (Then the Grand Cathedral, v25: 60 with a 0.12 s fade turned the whole view white for half a
// second, and the eye adaptation took the flash for daylight and brightened it more; the user:
// "maybe too strong". Now 30 with a 0.08 s fade, and the eye adaptation pauses while a flash
// shines: vk_rasterizer.cpp. A scaling by the area's measured level did nothing: the tone map's
// level is after the game's own exposure, about -6 stops in the Tomb, the cathedral and outside.)
// (Then v26, the user: "needs to be weaker in intensity and a little faster too (so faster/more
// abrupt fade out)": 18 (was 30), held 50 ms (30 for one build), a 0.05 s fade (was 0.08), gone after 0.3 s
// (was 0.6; the eye adaptation's pause too), and a softer core (3 m^2, was 1: the coat and the
// ground right at the gun went white on the first frame; at 5 m about 30% less light than v26).)
// (Then, 2026-11-12, the user: "make the muzzle flash have greater distance (the light)": 30 with
// a 6 m^2 core (was 18 and 3): about as bright at the gun, x1.5 at 6 m, x1.6 at 10-15 m, and a
// 30 m reach (was 20). The core is now the slot's own (dword 7), the torch glow's differs.)
// (Then, 2026-11-15, the user: "increase a bit the range of the light of the gun muzzle (maybe by
// 1.5m or 2m more) ... and make it a tiny bit more strong", then "a tiny bit more": 48 with an
// 8 m^2 core and a 32 m reach (were 30, 6 and 30): 20-24% more light within a metre of the gun,
// the same light 1.6 m further where it was at 6 m, 2.0 m at 8 m, 2.4 m at 10 m.)
inline constexpr float MuzzleFadeS = 0.05f;
inline constexpr float MuzzleLifeS = 0.3f;
inline constexpr float MuzzleRadiusM = 32.0f;
inline constexpr float MuzzleIntensity = 48.0f;
inline constexpr float MuzzleSoftM2 = 8.0f;
// The torch held up (core/vr/torch_glow.h; the muzzle flash's slot when no flash shines): a
// share of the game's own torch light (the listed light nearest its flame, within TorchMatchM)
// at its colour, reaching TorchRange x as far: raised (level 1) and held high (2). The core
// TorchSoftM2 keeps it gentle near the flame. VR_TUNE_FILE lighting.torch_glow (scales the
// shares), lighting.torch_range (scales the reach's growth).
inline constexpr float TorchShareRaised = 0.2f;
inline constexpr float TorchShareHigh = 0.35f;
inline constexpr float TorchRangeRaised = 1.2f;
inline constexpr float TorchRangeHigh = 1.35f;
inline constexpr float TorchSoftM2 = 2.0f;
inline constexpr float TorchMatchM = 0.75f;
inline constexpr float TorchFreshS = 0.25f;
inline constexpr std::array<float, 3> MuzzleColour{1.0f, 0.78f, 0.5f};
inline constexpr u32 MuzzleSteps = 16;

// Shadowed air (2026-11-11: the visible next-gen items, "volumetric fog" part 2): a
// lamp's glow in the air is cut by what stands between it and the air (railings, pillars,
// gravestones, a hunter), so dark shafts run through it. For the AirLamps strongest lamps from
// AirNearM to AirFarM of the eye (the lantern in your hand never: its shafts would sweep the face)
// the composite's air integral is times the share of it that sees the lamp: all the air points on
// a pixel's ray see the lamp across the same line of the screen (the ray and the lamp make one
// plane with the eye), so AirSamples depth samples along that line, each a ray from the eye at an
// angle towards the lamp, mark which parts of the ray they shadow (the surface found there, taken
// to be AirThickM + AirThickPerM per metre thick, blocks the stretch of the ray whose segments to
// the lamp pass behind it) as bits of 32 slices that hold equal light (the falloff's integral is
// an arctangent); lit share = 1 - bits / 32. The samples move every frame (the pixel's gradient
// noise) and the upscaler averages them. A sample off the screen or at the lamp's own fixture
// shadows nothing (the air as before). Light dword 7 is the lamp's shadow share (0..1, fading
// over AirFadeS as a lamp joins or leaves the shadowed ones). VR_AIR_SHADOWS=0: none (compiled
// at the first use).
inline constexpr u32 AirLamps = 3;
inline constexpr float AirNearM = 1.0f;
inline constexpr float AirFarM = 15.0f;
inline constexpr float AirFadeS = 0.3f;
inline constexpr u32 AirSamples = 6;
inline constexpr float AirBiasM = 0.02f;
inline constexpr float AirThickM = 0.2f;
inline constexpr float AirThickPerM = 0.05f;
inline std::array<float, 3> AirColour(const std::array<float, 3>& c) {
    const float top = std::max({c[0], c[1], c[2]});
    const float scale = top > AirCap ? AirCap / top : 1.0f;
    const float grey = scale * (0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]);
    std::array<float, 3> out;
    for (int k = 0; k < 3; ++k) {
        out[k] = std::max(0.0f, grey + (1.0f - AirGrey) * (scale * c[k] - grey));
    }
    return out;
}

} // namespace Shader::Optimization::FogLight
