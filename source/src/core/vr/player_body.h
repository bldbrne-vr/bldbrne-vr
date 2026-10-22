// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <string>
#include <string_view>

// Your height: two rows on the VR page's General > Body (2026-10-09, the tester's design: "2
// settings. First option has 2 options 'As the player -- As the hunter' toggling between the two
// sets the base height. The rest is adjustment, that goes -10+10"). Pure (no OpenXR or emulator
// state): the camera tick (vr_session.cpp), the body, the vial zones, the belt and physical evasion
// read it, tests/test_player_body.cpp checks it.
//
//  - Height (vr.ini height_base): where your eye is in the game when you stand straight.
//      As the hunter (hunter, the default): the hunter's own first-person eye (fp_eye_height_m,
//        1.58 m over his feet). "Standing straight" is your head at the 6DoF origin: where it was
//        at the recenter (or the camera's takeover after a load), as in every build before
//        2026-10-09's height rows. No height is typed or measured: nothing to get wrong.
//      As the player (player): 1:1 with the real world: the camera's eye over his feet is your
//        real eye over the headset's floor (OpenXR LOCAL_FLOOR, else STAGE). Without a floor (or
//        Stance: Seated, where your eye over the floor is not your height) it works as the hunter.
//  - Adjust height (vr.ini height_adjust_cm, -10..+10, step 1): added to the eye of either: +5 is
//    the view exactly 5 cm higher. In As the player it also corrects a headset floor that is a
//    few cm off (the tester's Quest floor read about 9 cm low).
//
// Both give one number, the eye offset: the camera's standing eye over the hunter's feet minus
// fp_eye_height_m (metres, before world_scale; EyeOffsetM). The camera tick adds it after the
// room's clamp (your ducking, leaning and the duck room stay 1:1 around the origin, as always);
// As the hunter + 0 adds exactly nothing (the camera bit for bit as before). The hunter's body is
// fitted to it (bloodborne_body.cpp): below his eye his upper body comes down to yours
// (BodyParams::stand_drop, StandDrop); above it he cannot grow, so his arms reach further toward
// hands his shoulders are below (BodyParams::rise_stretch, RiseStretch), and his arms, the vial's
// stab zones, the belt's draw zone and your counted lean scale with the body scale k
// (BodyScaleFor: 1 + the offset over a 167 cm person's eye height, exactly 1 at 0, clamped
// 0.90 .. 1.15).
//
// vr.ini from the builds before (2026-10-09 02:54 .. this one): game_height_cm (the old absolute
// Height row) and player_height_cm (its Auto / Real height calibration) are read and ignored (a
// typed or mis-measured height never moves the camera); the page never writes them.
namespace VR::PlayerBody {

constexpr float EyeToStature = 0.936f;    // standing eye height / stature (adults)
constexpr float HunterStatureCm = 167.0f; // the hunter's arms are a 167 cm person's
constexpr float ScaleMin = 0.90f;
constexpr float ScaleMax = 1.15f;
// The hunter's arm, shoulder to the grip (model metres): 0.634 m (tests/test_player_body.cpp).
constexpr float HunterArmM = 0.634f;
// The most the arms may stretch for an eye above the hunter's, on top of the IK's own 10%.
constexpr float RiseStretchMax = 0.40f;

// ---- The two rows -------------------------------------------------------------------------------

enum class HeightBase : unsigned char { Hunter, Player };

constexpr int AdjustMinCm = -10;
constexpr int AdjustMaxCm = 10;

struct HeightSetting {
    HeightBase base{HeightBase::Hunter};
    int adjust_cm{0};
};

inline const char* BaseIni(HeightBase base) {
    return base == HeightBase::Player ? "player" : "hunter";
}
inline const char* BaseText(HeightBase base) {
    return base == HeightBase::Player ? "As the player" : "As the hunter";
}

inline int ClampAdjustCm(long cm) {
    return static_cast<int>(std::clamp<long>(cm, AdjustMinCm, AdjustMaxCm));
}

// Adjust height in metres (exactly 0 for 0).
inline float AdjustM(int adjust_cm) {
    return static_cast<float>(ClampAdjustCm(adjust_cm)) * 0.01f;
}

// The row's text: "+3 cm", "0 cm", "-4 cm".
inline std::string AdjustText(int adjust_cm) {
    const int cm = ClampAdjustCm(adjust_cm);
    return (cm > 0 ? "+" : "") + std::to_string(cm) + " cm";
}

// vr.ini's height keys (vr_session.cpp ParseSettingsLine).
enum class HeightKey : unsigned char {
    Other,  // not a height key
    Set,    // height_base / height_adjust_cm, applied
    Legacy, // game_height_cm / player_height_cm: an older build's rows, ignored
    Bad,    // a height key with a value it cannot take (nothing changed)
};

inline bool IsHeightKey(std::string_view key) {
    return key == "height_base" || key == "height_adjust_cm" || key == "game_height_cm" ||
           key == "player_height_cm";
}

inline HeightKey ParseHeightSetting(HeightSetting& h, std::string_view key,
                                    std::string_view value) {
    if (key == "height_base") {
        if (value == "hunter") {
            h.base = HeightBase::Hunter;
        } else if (value == "player") {
            h.base = HeightBase::Player;
        } else {
            return HeightKey::Bad;
        }
        return HeightKey::Set;
    }
    if (key == "height_adjust_cm") {
        const std::string text{value};
        char* end = nullptr;
        const double v = std::strtod(text.c_str(), &end);
        if (text.empty() || end == text.c_str() || *end != '\0' || !std::isfinite(v)) {
            return HeightKey::Bad;
        }
        h.adjust_cm = ClampAdjustCm(std::lround(std::clamp(v, -1000.0, 1000.0)));
        return HeightKey::Set;
    }
    if (key == "game_height_cm" || key == "player_height_cm") {
        return HeightKey::Legacy;
    }
    return HeightKey::Other;
}

// ---- The eye offset -----------------------------------------------------------------------------

// As the player counts (else the hunter's mapping): a floor is known and you play standing.
inline bool PlayerInEffect(HeightBase base, bool floor_valid, bool seated) {
    return base == HeightBase::Player && floor_valid && !seated;
}

// As the hunter, his eye is this much higher than fp_eye_height_m (2026-10-13: "up the
// camera +2cm by default ... where we compute the hunter's height"); Adjust height goes on top.
constexpr float HunterEyeLiftM = 0.02f;

// The camera's eye offset from the hunter's eye (metres, before world_scale; + = higher): As the
// hunter, HunterEyeLiftM plus Adjust height; As the player (with a floor, standing), your eye
// over the floor at the 6DoF origin (`origin_over_floor_m`) minus his eye (`fp_eye_m`) plus Adjust
// height, so the camera's eye over his feet is your eye over the floor plus the adjustment.
inline float EyeOffsetM(HeightBase base, int adjust_cm, float fp_eye_m, bool floor_valid,
                        float origin_over_floor_m, bool seated) {
    const float adjust = AdjustM(adjust_cm);
    if (!PlayerInEffect(base, floor_valid, seated) || !std::isfinite(origin_over_floor_m)) {
        return HunterEyeLiftM + adjust;
    }
    return (origin_over_floor_m - fp_eye_m) + adjust;
}

// The body's eye offset: what his stance, his arms' k, the vial's zones, the belt and the counted
// lean are fitted to (the camera, its floor guard and the late warp's soles keep EyeOffsetM). As
// the player (`player_in_effect`: a floor known, standing) your head above the 6DoF origin
// (`head_above_origin_m`, after the room's clamp) raises it with you: after a recenter (or a camera
// takeover) taken while you were bent, standing up fits him to your standing eye again instead of
// leaving his stance as low and his arms as short as the bent height (the camera is 1:1 with your
// eye over the floor either way; below the origin is a duck, as before). As the hunter, and As the
// player without a floor or seated: the eye offset itself, bit for bit.
inline float BodyOffsetM(float eye_offset_m, bool player_in_effect, float head_above_origin_m) {
    if (!player_in_effect || !(head_above_origin_m > 0.0f) || !std::isfinite(head_above_origin_m)) {
        return eye_offset_m;
    }
    return eye_offset_m + head_above_origin_m;
}

// The camera's standing eye over the hunter's feet (metres, before world_scale): fp_eye_height_m
// plus the offset (fp itself for 0, bit for bit).
inline float StandingEyeM(float eye_offset_m, float fp_eye_m) {
    return fp_eye_m + eye_offset_m;
}

// k for a stature (cm; <= 0: 1, the hunter's own): stature / 167, clamped to 0.90 .. 1.15.
inline float BodyScale(int height_cm) {
    if (height_cm <= 0) {
        return 1.0f;
    }
    return std::clamp(static_cast<float>(height_cm) / HunterStatureCm, ScaleMin, ScaleMax);
}

// k for an eye offset: the hunter's stature plus the offset as stature (offset / 0.936), over his
// stature: 1 + offset / 1.563 m, clamped; exactly 1 for 0 (or a non-finite offset).
inline float BodyScaleFor(float eye_offset_m) {
    if (eye_offset_m == 0.0f || !std::isfinite(eye_offset_m)) {
        return 1.0f;
    }
    const float stature_cm = HunterStatureCm + eye_offset_m / EyeToStature * 100.0f;
    return std::clamp(stature_cm / HunterStatureCm, ScaleMin, ScaleMax);
}

// How tall a person with the camera's standing eye is (cm, for the logs).
inline float StatureCm(float eye_offset_m, float fp_eye_m) {
    return StandingEyeM(eye_offset_m, fp_eye_m) / EyeToStature * 100.0f;
}

// The body for an eye `offset_m` above the hunter's (model units: the eye offset x world_scale).
// Below him (a negative offset), his upper body comes down by all of it while you stand
// (BodyParams::stand_drop: no deadband on that part, a duck on top as before). Above him his
// trunk cannot stretch, so his shoulders stay that much below where yours are: his arms may
// stretch that much further toward a hand out of reach (BodyParams::rise_stretch, a fraction of
// the arm `arm_scale` x HunterArmM; at most RiseStretchMax). Only a pose that needs it stretches:
// hanging and forward hands keep the arm's own length.
inline float StandDrop(float offset_m) {
    return offset_m < 0.0f && std::isfinite(offset_m) ? -offset_m : 0.0f;
}
inline float RiseStretch(float offset_m, float arm_scale) {
    if (!(offset_m > 0.0f) || !std::isfinite(offset_m) || !(arm_scale > 0.0f)) {
        return 0.0f;
    }
    return std::min(offset_m / (HunterArmM * arm_scale), RiseStretchMax);
}

// The IK's own stretch toward a hand out of reach (arm_ik.h ArmLimits::maxStretch).
constexpr float IkStretch = 0.10f;
// Arms shorter than his (k < 1: the camera's eye below his) may still stretch out to his own full
// reach (his arm plus the IK's stretch) toward a hand out of their reach: your real arms can be
// longer than the eye says (Adjust height down, or a headset floor read low), and the mock's 177 cm
// player's overhead hands As the hunter -10 cm were 15 mm short (As the player -10: 20 mm). Only a
// hand out of reach stretches: hanging, bent and relaxed arms keep the shorter look. 0 for k >= 1
// (As the hunter + 0 bit for bit). On top of RiseStretch (0 below his eye) in rise_stretch.
inline float ShortStretch(float arm_scale) {
    if (!(arm_scale > 0.0f) || !(arm_scale < 1.0f)) {
        return 0.0f;
    }
    return (1.0f + IkStretch) * (1.0f / arm_scale - 1.0f);
}

} // namespace VR::PlayerBody
