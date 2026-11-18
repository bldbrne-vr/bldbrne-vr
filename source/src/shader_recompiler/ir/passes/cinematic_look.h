// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdlib>
#include <array>
#include <bit>

#include "common/types.h"

// The cinematic look (2026-11-09: "add the cinematic look and the darker fill ... make
// it configurable"): a grade on the game's last colour step, gritty rather than glossy (deeper
// blacks, a filmic contrast curve round a low pivot, quieter shadows, a faint cool-shadow /
// warm-highlight split; no teal and orange). The game's ColorGrading pass (`0xed4c7668`) looks
// its tone-mapped scene up in a 16^3 LUT; CinematicPass (light_shadow_pass.cpp) passes the
// result through VrCinematic, whose parameters come from a buffer of its own
// (Shader::BufferType::VrGrade, declared in every permutation of the program) that the
// rasterizer fills at every draw. The live-tuning key lighting.grade defaults to 0.55 in VR;
// strength 0 preserves the game's grade.
namespace Shader::Optimization::CinematicLook {

inline constexpr bool IsGradingProgram(u64 hash) {
    return hash == 0xed4c7668u;
}

// Filmic highlights (2026-11-11: "fire, lamps and the moon fade smoothly into colour
// instead of clipping to flat yellow-white"). The game's tone map (`0x5e67d866`, HDR scene ->
// 8-bit, before the grade) squeezes each channel on its own: t = 1 - exp(-k max(2^-14, x)). A
// bright orange fire (x = 10, 3, 0.5) comes out t ~ 1, 0.95, 0.39: yellow-white. FilmicPass
// (light_shadow_pass.cpp) hands the three exponentials e = 1 - t to VrFilmic, which moves them
// towards the hue-keeping curve (the brightest channel's t, every channel in its own share of
// it: 1, 0.3, 0.05) by the share in this buffer's float 15; the game's shaping, glow and gamma
// after it are untouched. Share 0 is the game's tone map exactly. Cost: a dozen ALU per pixel.
// Three variants, all drawn by vs 0x1ea5a07e (2026-11-11, found with FAST_GPU_PASS_PROFILE: with
// only the Dream's, filmic highlights and eye adaptation never ran elsewhere): the Dream's
// `0x5e67d866`; `0xccb08339` in Old Yharnam and the Grand Cathedral (the same curve and samples,
// more after it); `0x5c02eaf8` at the Tomb of Oedon. FilmicPass matches the shape (ToneMapFloor's
// three exponentials) and leaves anything else as it is.
inline constexpr bool IsToneMapProgram(u64 hash) {
    return hash == 0x5e67d866u || hash == 0xccb08339u || hash == 0x5c02eaf8u;
}
// The floor the tone map puts under each channel (the match's anchor in FilmicPass).
inline constexpr float ToneMapFloor = 6.103515625e-05f;
// The share of the hue-keeping curve when filmic_highlights is on (VR_TUNE_FILE lighting.filmic
// overrides it, 0..1).
inline constexpr float FilmicShare = 0.5f;
// Only the bright end takes it: none while the brightest channel's tone-mapped value is below
// FilmicFrom, all of it from FilmicFull (a smoothstep between). The hue-keeping curve is darker in
// the other channels, so on midtones and haze it would only deepen the picture.
inline constexpr float FilmicFrom = 0.6f;
inline constexpr float FilmicFull = 0.95f;

// The buffer, floats: 0 strength (0..1: the grade's share), 1 contrast (the curve's exponent - 1),
// 2 black level (crushed to 0), 3 pivot (the curve's fixed point), 4 / 5 saturation in the
// shadows / highlights, 6..8 the shadows' tint, 9..11 the highlights' tint, 12..14 the luma
// weights, 15 the filmic highlights' share (read by the tone map, not the grade). Colours in the
// pass's output order: blue, green, red (the LUT's result leaves the shader swapped, as the
// game's colour buffer stores it).
// Eye adaptation (2026-11-11: "going in a dark room after a bright room and vice
// versa"): the game sets its exposure per area and never follows how bright the view is. Ours
// keeps the game's look once the eye has adapted and adds only the adapting: the tone map counts
// the frame's brightness (the log2 luminance of every SampleEvery-th pixel in both directions,
// atomically summed into the state buffer, BufferType::VrAdaptation), and one pixel a frame turns
// the last frame's sum into the current level (a fast follower, FastS) and the adapted level (a
// slow one, SlowS); the exposure is 2^(AdaptStrength (current - adapted)) within +-MaxStops. Into
// a dark room: the current level drops, the eye still adapted to the light, so the room is darker
// than the game's at first and opens up over a second or two; out into the moonlight: bright,
// then settling. Exactly the game's exposure once adapted. Floats 16..19 of this buffer: the
// strength (0: off), the fast and slow followers' share this frame (1 - exp(-dt / S)), the
// stops; dword 20 the frame's number (its parity picks the state slot, mod 3 the sum's).
// vr.ini eye_adaptation (a tone map permutation: off costs nothing), VR_EYE_ADAPT=0|1 overrides
// it (compiled; tests). (For a day the tone map measured always, for the muzzle flash's area
// scaling: dropped, the level is after the game's own exposure.)
inline bool AdaptEnabled(bool vr, bool setting, const char* override_value) {
    return override_value ? override_value[0] != '0' : vr && setting;
}
// (0.85 and 2.5 s since the first headset run, 0.7 and 1.5 s: unnoticed while walking.)
// Then (2026-11-11, after the Old Yharnam church walk: "we probably need to bump the
// eye adaptation by much because I didn't even notice it. And only make it when brightness
// changes a lot between dark and light"): a dead zone of ThresholdStops (a gap smaller than that
// between the current and the adapted level does nothing: no pumping as you look around), above
// it StrengthAbove times the rest, up to MaxStops; and two slow followers like an eye's: into
// the light quickly (SlowUpS), into the dark slowly (SlowDownS: the dark stays dark for a while).
// VR_TUNE_FILE keys lighting.adapt (the strength), lighting.adapt_threshold,
// lighting.adapt_stops, lighting.adapt_up_s, lighting.adapt_down_s.
inline constexpr float AdaptStrength = 1.6f;
inline constexpr float ThresholdStops = 0.6f;
inline constexpr float FastS = 0.12f;
inline constexpr float SlowS = 1.8f;     // into the light
inline constexpr float SlowDownS = 3.0f; // into the dark (exponential time constant, seconds)
inline constexpr float MaxStops = 2.5f;
inline constexpr unsigned SampleEvery = 16;
inline constexpr float SumScale = 256.0f; // the sum's fixed point (log2 units)
// The state buffer, dwords: the current and adapted levels (log2) of the even frames' slot (0, 1)
// and the odd frames' (2, 3), 4 set once they are valid, then three sums of (log2 x SumScale as a
// signed integer, the count) at 8, 10, 12.
inline constexpr unsigned AdaptDwords = 16;

inline constexpr u32 BufferDwords = 24;
// Float AmbientFloat: the ambient light's level (2026-11-14: "reduce the ambient light
// of forbidden woods a click"; "I want forbiden woods to be darker... have the light from the
// moon, torches etc to have bigger impact"). The game's ambient composite (reflections.h
// IsCompositeProgram) declares this buffer too and AmbientLevelPass (reflections_pass.cpp)
// multiplies its first target by it. 1: the game's. Live key lighting.ambient.
inline constexpr u32 AmbientFloat = 23;
// The Forbidden Woods' own level where the live key is not set (2026-11-17: "make the
// forbidden woods a tiny bit darker (ambient color) just a tiny bit"): at 0 the woods' lamp view
// is some 14% darker, so 0.6 is a twentieth. VR_WOODS_AMBIENT=level (1: the game's).
inline constexpr float WoodsAmbient = 0.6f;
inline float WoodsAmbientNow() {
    static const float v = [] {
        const char* const env = std::getenv("VR_WOODS_AMBIENT");
        return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), 0.0f, 2.0f) : WoodsAmbient;
    }();
    return v;
}
struct Adaptation {
    float strength{};
    float fast{};
    float slow{};
    float stops{MaxStops};
    u32 frame{};
    float slow_down{};
    float threshold{ThresholdStops};
};
inline std::array<float, BufferDwords> Params(float strength, float filmic, const Adaptation& adapt = {}) {
    const float s = std::clamp(strength, 0.0f, 1.0f);
    return {s,
            0.45f,                   // contrast: midtones 1.45x steeper
            0.035f,                  // black level
            0.32f,                   // pivot (a dark game's mid-grey)
            0.75f,  1.06f,           // saturation: shadows / highlights
            0.012f, 0.002f, -0.008f, // shadows a little cooler (b g r)
            -0.008f, 0.004f, 0.012f, // highlights a little warmer (b g r)
            0.0722f, 0.7152f, 0.2126f, // luma (b g r)
            std::clamp(filmic, 0.0f, 1.0f),
            std::clamp(adapt.strength, 0.0f, 3.0f),
            std::clamp(adapt.fast, 0.0f, 1.0f),
            std::clamp(adapt.slow, 0.0f, 1.0f),
            std::clamp(adapt.stops, 0.0f, 4.0f),
            std::bit_cast<float>(adapt.frame),
            std::clamp(adapt.slow_down, 0.0f, 1.0f),
            std::clamp(adapt.threshold, 0.0f, 4.0f),
            1.0f}; // AmbientFloat (the rasterizer sets it)
}

} // namespace Shader::Optimization::CinematicLook
