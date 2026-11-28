// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <cstdint>

#include "core/vr/torch_glow.h"

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
// The light while it lasts. Until 2026-11-23 a game effect with a light of its own on the head,
// asked every update in a private slot (the lantern's is 30030): 202010 (the Augur's projectile:
// its tentacles too), then 200007 (a dark octagonal model with it: the user, "tonitrus sprouts a
// second head"). Now no game effect (the tester: "if we can make a faint blue light that doesn't
// tank performance I would be cool with that"): the head is published every update while the
// charge holds (HeadGlow) and the renderer lights it in the muzzle flash's slot (vk_rasterizer.cpp
// VrFogUpload, when no flash shines; before the torch's glow): GlowColour times GlowIntensity over
// (d^2 + GlowSoftM2), nothing past GlowRadiusM. This atmospheric spill is unshadowed. VR_TUNE_FILE
// lighting.tonitrus (scales it; 0: none), lighting.tonitrus_radius. It needs the VR lighting (the
// composite's lamp memory). VR_TONITRUS_FFX=<id>: a game effect there as before (tests).
inline constexpr std::int32_t LightFfx = 0;
inline constexpr std::int32_t LightSlot = 30033;
// (Seen in the mock facing a dark wall in Pthumeru, with the user's graphics (DLSS: without an
// upscaler the lamp glow that carries it is off by the card's defaults): 7 lit the wall a pale
// grey and 70 turned the room white; 1, a deep blue, is a faint tint on what is near. Then the
// user, 2026-11-24: "a tiny bit more faint and less reach", then "3m is good": 0.8 to 3 m,
// were 1 to 6 m.)
inline constexpr std::array<float, 3> GlowColour{0.3f, 0.5f, 1.0f};
inline constexpr float GlowIntensity = 0.8f;
inline constexpr float GlowSoftM2 = 1.0f;
inline constexpr float GlowRadiusM = 3.0f;
// Keep the tuned lighting origin slightly towards the eye from the centre of the iron head.
inline constexpr float GlowTowardEyeM = 0.25f;
inline constexpr float GlowFreshS = 0.25f; // published every update: gone this soon after the charge
inline VR::TorchGlow::Spot HeadGlow;
// When it shines (2026-11-23: "the faint blue light needs to go off once the charge is
// done (7 seconds). teleporting or resting or dying should also remove it"): while the game's
// charge holds (6032 on the hunter, gone after its 7 s), and never past ChargeS after the strike
// whatever the game keeps, nor while the hunter is dead, nor once a lamp's travel has begun (a
// rest at a lamp is the trip to the Dream), nor after any arrival since the strike (a warp, a
// respawn, a load: the next charge lights it again).
inline constexpr float ChargeS = 7.5f; // SpEffectParam 6032: 7 s (7.3 by the updates in the mock)
inline bool GlowLit(float age_s, bool alive, bool travelling, bool arrived_since) {
    return age_s >= 0.0f && age_s < ChargeS && alive && !travelling && !arrived_since;
}
// Its strength (2026-11-23: "make it weaken at second 5 and off by second 7"): full
// until FadeFromS after the strike, then down smoothly to nothing at FadeOutS.
inline constexpr float FadeFromS = 5.0f;
inline constexpr float FadeOutS = 7.0f;
inline float GlowFade(float age_s) {
    if (!(age_s < FadeOutS)) {
        return 0.0f;
    }
    if (age_s <= FadeFromS) {
        return 1.0f;
    }
    const float t = (age_s - FadeFromS) / (FadeOutS - FadeFromS);
    return 1.0f - t * t * (3.0f - 2.0f * t);
}
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

// The game thread publishes charge edges and changes in retry eligibility.
struct State {
    std::uint64_t serial{0};  // charges begun this session
    std::int64_t start_ns{0}; // the last one's start (steady clock)
    bool active{false};       // the charge holds now
    // A verified stretch without the buff, ending in an interrupted Tonitrus pose.
    std::int64_t retry_from_ns{};
    std::int64_t interrupted_ns{};

    bool MayRetryLift(std::int64_t request_ns) const {
        return !active && retry_from_ns > 0 && request_ns >= retry_from_ns &&
               request_ns < interrupted_ns;
    }
};
void Publish(const State& state);
bool Latest(State& out);

enum class Observation { Unknown, Absent, Present };

// One per hunter update: the charge's edges. True on the update it begins.
struct Tracker {
    State state{};
    std::int64_t uncharged_since_ns{};
    bool was_interrupted{};

    // Called with the same effect-list observation as Step. Publish only an interruption
    // edge or withdrawal of an earlier retry. An unreadable list proves nothing.
    bool ObserveLift(bool verified_uncharged, bool interrupted, std::int64_t now) {
        if (!verified_uncharged && !uncharged_since_ns) {
            return false;
        }
        const auto previous = state.interrupted_ns;
        if (!verified_uncharged) {
            uncharged_since_ns = state.retry_from_ns = state.interrupted_ns = 0;
        } else {
            if (uncharged_since_ns == 0) {
                uncharged_since_ns = now;
            }
            if (interrupted && !was_interrupted) {
                state.retry_from_ns = uncharged_since_ns;
                state.interrupted_ns = now;
            }
        }
        was_interrupted = verified_uncharged && interrupted;
        return state.interrupted_ns != previous;
    }

    bool Step(Observation effect, std::int64_t now) {
        if (effect == Observation::Unknown) {
            return false;
        }
        const bool charged = effect == Observation::Present;
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
