// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <cmath>
#include <cstdint>

// Tonitrus' charge (2026-11-09: lifted up to charge it, a quick blue and white flash
// like "a thunder hit next to us" as a custom shader (A Call Beyond's kind, no overlay), the Tiny
// Tonitrus' own thunder and a quick strong bump, a bolt "from above [that] hits the head", and for
// the 7 seconds it lasts "a blue light that lights up surroundings"). The game's charge is SpEffect
// 6032 on the hunter (SpEffectParam: 7 s, lightning +40 and x1.7: its only bonus), added by the
// game's own animation however it began (the lift, a squeeze with hand_transform=0, a gamepad's
// L1). The game thread (bloodborne_held.cpp) watches for it, keeps a light on the mace's head while
// it lasts and a burst of the game's lightning for the strike, and hides the Tonitrus' idle crackle
// outside it; its start strikes a bolt from the sky onto the mace and flashes the world (the
// in-frame renderer, bloodborne_quickitems.cpp: before the upscaler, whose history restarts for the
// flash, vr_temporal_scene.cpp) and the presenter claps the thunder and bumps the weapon hand.
namespace VR::TonitrusCharge {

inline constexpr std::int32_t ChargeEffect = 6032;
// The mace's tip: the model's dummy 100 (0.78 m out from the grip), as the right weapon's part 0.
inline constexpr std::int32_t HeadDummy = 10100;
// The light while it lasts: a game effect with a light of its own, on the head (asked every
// update, so it ends with the charge), in a private slot (the lantern's is 30030). 202010 (a
// lightning bullet's): a soft blue light with next to nothing drawn of its own (the 2026-11-09
// probe of 250 effects at the hand in a dark hall: +12 on the floor, blue over red).
inline constexpr std::int32_t LightFfx = 202010;
inline constexpr std::int32_t LightSlot = 30033;
// The bolt: from this high above the head, the Tiny Tonitrus bridge's strike and two restrikes
// (HunterRelics::BridgeStrikeAt), a little wider than the tool's.
inline constexpr float SkyM = 6.5f;
inline constexpr float BoltWidthM = 0.10f;
// The strike's own light (2026-11-09: "like a thunder hit next to us, very quick"):
// 250010, a lightning burst's (arcs leaping round and a hard blue-white light: the probe's
// brightest, +51 on the floor), on the head for FlashNs; the game's own light, so what is near
// turns white and what is far stays dark.
inline constexpr std::int32_t FlashFfx = 250010;
inline constexpr std::int32_t FlashSlot = 30034;
inline constexpr std::int64_t FlashNs = 400'000'000;
// ... and the world flashes with it before the upscaler (the in-frame backdrop A Call Beyond's
// gray world uses, InFrame::List world_storm at Blink's flicker; vr_inframe.frag StormFlash):
// added over the scene, which keeps its detail, the world lit a cold blue-white, the dark a deep
// blue, the bright (near the strike) white, with a glow round every bright thing.

// The Tonitrus' idle crackle (2026-11-09: "hide it"): little arcs on its head that never
// stop while it is in the hand (even through a loading screen). The game plays its resident
// SpEffect 6033's state 33 as SpEffectVfxParam row 33: FFX 7211 on the head's dummies 206; the
// charge's state 30 is row 30, FFX 7210, kept. Row 33's effect is blanked in the params the game
// loads (bloodborne_kos_lights.cpp PrepareParams; VR_TONITRUS_IDLE=1, tests: kept). Charged or
// not changes nothing else: 6033 has no stats, the charge's bonus is 6032's alone.
inline constexpr std::int32_t IdleCrackleVfxRow = 33;
inline constexpr std::int32_t IdleCrackleFfx = 7211;

// The game thread's view of the charge (Publish every hunter update; any thread reads it).
struct State {
    std::uint64_t serial{0};  // charges begun this session
    std::int64_t start_ns{0}; // the last one's start (steady clock)
    bool active{false};       // the charge holds now
};
void Publish(const State& state);
bool Latest(State& out);

// One per hunter update: the charge's edges. True on the update it begins.
struct Tracker {
    State state{};
    bool Step(bool charged, std::int64_t now) {
        const bool began = charged && !state.active;
        if (began) {
            ++state.serial;
            state.start_ns = now;
        }
        state.active = charged;
        return began;
    }
};

// The strike's age (s) at `now` for a charge begun at `start`.
inline float StrikeAgeS(std::int64_t now, std::int64_t start) {
    return static_cast<float>(now - start) * 1e-9f;
}

// The world flash's strength `t` seconds after the start: a lightning flicker, full at once, a
// dip, a second flash, then fading out by BlinkS (2026-11-09: "up a bit the time of the
// glow": 0.32 s before).
inline constexpr float BlinkS = 0.5f;
inline float Blink(float t) {
    if (!(t >= 0.0f) || t >= BlinkS) {
        return 0.0f;
    }
    if (t < 0.06f) {
        return 1.0f;
    }
    if (t < 0.11f) {
        return 0.35f;
    }
    if (t < 0.17f) {
        return 0.85f;
    }
    const float k = 1.0f - (t - 0.17f) / (BlinkS - 0.17f);
    return 0.85f * k * k;
}

// The temporal upscaler and Foveated upscale's TAA (vr_temporal_scene.cpp) start their history
// afresh on a frame where the flash jumps (the flicker's on, its dip, its second flash): kept,
// they would average the jump away, and unevenly between the centre (the upscaler) and the edges
// (the TAA): a ring. The fade after it is smooth (at most ~0.09 a frame at 60 fps): their
// history follows it, and the image keeps its detail. (Until 2026-11-09 every frame for 0.56 s,
// some 50 frames each upscaled from itself alone: the edges shimmered through the fade.) The
// level is the frame's own (InFrame::StormFor its record: what its list asks to be drawn, with
// the strike's game lighting in the same frame), never the clock's at the resolve, which runs a
// frame or two after the game built the frame: a restart there landed on the frame before the
// flash, and the first bright frame was blended with the dark one.
inline constexpr float FlashJump = 0.2f;
inline bool FlashJumped(float previous, float now) {
    return std::abs(now - previous) > FlashJump;
}

} // namespace VR::TonitrusCharge
