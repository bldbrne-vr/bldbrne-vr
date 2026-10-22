// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <array>
#include <iosfwd>
#include <string>

#include "common/types.h"

// VR body (tracked hunter arms) settings in vr.ini. Parsed by VR::LoadSettings, which hands every
// key it does not know to ParseBodySetting, and written with the other defaults when vr.ini is
// created. Read-only after loading, like VR::Settings, except the per-weapon grips, which the
// live grip adjust changes (mutex inside; WeaponGrip / SetWeaponGrip).
namespace VR::Body {

enum class BodyMode : u32 {
    NoHead = 0, // the head (neck and head bones) hidden; torso, legs, arms shown
    // Only the hands and forearms (and everything they hold) in the first-person view: the
    // renderer leaves every other triangle of the hunter out of the colour passes
    // (vk_rasterizer_vr_draws.cpp, vr_hands_table.h); nothing is scaled, so the shadow maps keep
    // his whole body. VR_TUNE_FILE body.arms_draw=0: the old way (Spine scaled to 0.01; A/B only).
    ArmsOnly = 1,
    Full = 2, // legacy full-body mode; head omitted in first person, preserved in shadows
    // Only the hands (and everything they hold), the same way: not even the forearms.
    HandsOnly = 3,
};

// What the renderer keeps of the hunter in the colour passes of first-person frames.
enum class DrawnArms : u8 {
    Whole,    // no filter (no_head, full, the old arms_only scaling, no body hooks)
    Forearms, // arms_only: his hands and forearms
    Hands,    // hands_only: his hands
};

struct BodySettings {
    bool body{true}; // master switch: false = no body hooks at all
    // Full body in first person, with the head omitted.
    BodyMode mode{BodyMode::NoHead};
    // The hunter's cape is not drawn in first-person VR (the renderer leaves out the player's
    // cape meshes, vk_rasterizer_vr_draws.cpp); cinematics, the virtual screen and third person
    // show it. Independent of `body` and `mode`.
    bool hide_cape{true};
    // Near-camera cutaway: in first-person VR the hunter's own body draws (vk_rasterizer_vr_draws
    // .cpp) fade out where they come nearer to the camera than body_cutaway_far_m (a screen-space
    // dither; nothing is drawn nearer than body_cutaway_near_m), so looking down you see past the
    // collar and chest right under your eyes without moving or bending anything. On by default
    // (2026-10-04: the body must not block the view looking down, and must not look
    // distorted: the upper-body shift bent the waist, so it is off and this replaces it).
    bool body_cutaway{true};
    // A gentle cross-fade (chosen 2026-10-04: option c, cross-fading rather than cut, then
    // "even softer than softest"): fully gone only nearer than 3 cm, fading over 3 .. 22 cm.
    float body_cutaway_near_m{0.03f};
    float body_cutaway_far_m{0.22f};
    // Only while looking down more than this (degrees below the horizon; half the radius from
    // here, the full one 15 degrees further down): looking ahead or slightly down the body stays
    // whole. 0 = always on.
    float body_cutaway_pitch_deg{0.0f};
    // false: while the cutaway is on, the inside of the body (its back faces: the coat's lining,
    // the far side of the torso) is never drawn, so the opening shows the legs and the floor
    // beyond, not a hollow; true: the inside is drawn (the look of a cut-open shell).
    bool body_cutaway_inside{true};
    // The boost (core/vr/body_cut_boost.h; the tester, 2026-10-08: "when enemies hit me or I move my
    // head a tiny bit or down, I see a bit through the shoulders/body ... fade it out a bit more in
    // those cases"): through a hit reaction, a stagger or a kept dodge the body fades from
    // body_cutaway_boost_far_m to body_cutaway_boost_near_m (past the 10 cm near clip, so nothing
    // is left there to cut open), with the camera 4 cm behind or 4.5 cm below his eye halfway
    // between that and the normal fade; the inside is never drawn while boosted; held 0.4 s. Only
    // with the whole body drawn (no_head / full). A ladder mount also boosts for up to 3 s,
    // ending at dismount or a flat screen. false = the cutaway as before.
    bool body_cutaway_boost{true};
    float body_cutaway_boost_near_m{0.11f};
    float body_cutaway_boost_far_m{0.35f};
    // The upper arms and shoulders (the shirt's and coat's upper sleeves, the shoulder caps: the
    // vr_body_table.h shoulder ranges, every arm triangle above the elbow) fade with the rest of
    // the body near the camera, the normal fade and the boost alike; the forearms, hands and
    // anything held are never cut. The tester, 2026-10-08: through the shoulders after a hit or a
    // small head drop; the height merge's look-down at 177 cm: two solid white triangles of the
    // shirt's shoulder in the faded coat. Only with the whole body drawn (no_head / full; arms_only
    // and hands_only draw no upper arm). false = the arms kept whole as before 2026-10-09.
    bool body_cutaway_shoulders{true};
    // With body_mode=no_head (and the legacy full alias): true (the default since 2026-10-06, the tester: the hunter had no head
    // shadow) = the head (Neck, Head and the hat, face and hair on them) keeps its full size, so
    // the shadow maps draw it and it casts its shadow (on the ground, and on his shoulders), and
    // the renderer leaves it out of every colour pass of first-person VR frames instead
    // (vk_rasterizer_vr_draws.cpp, vr_body_table.h head ranges): nothing of it reaches the colour
    // or the depth the stereo is made from. false = the old way: local scale 0.01 on Neck and Head
    // (no shadow). VR_HEAD_SHADOW=0|1 / VR_TUNE_FILE body.head_shadow override it.
    bool body_head_shadow{true};
    bool arm_ik{true};        // false = body alignment and hiding only, the game's arms
    bool arm_pair_sync{true}; // both eyes of a stereo pair use one hand sample
    // Torso yaw: follows the head outside a deadzone (time constant torso_follow_ms), at most this
    // far from the legs.
    bool torso_follow{true};
    float torso_deadzone_deg{25.0f};
    float torso_max_deg{75.0f};
    float torso_follow_ms{250.0f};
    // The hunter's heading (core/vr/body_facing.h, core/vr/bloodborne_facing.cpp): only the turn
    // stick and your real heading turn him, never locomotion (he strafes, backpedals and steps in
    // every direction). It follows the view once you look more than body_facing_deadzone_deg away
    // (body_facing_moving_deg while the left stick moves him), easing in body_facing_ms.
    bool body_facing{true};
    float body_facing_deadzone_deg{35.0f};
    float body_facing_moving_deg{15.0f};
    float body_facing_ms{250.0f};
    // The upper body moved under the eye (low-pass body_align_ms), body_back_m behind it, and
    // looking down further back, up to body_back_down_m more (from 25 degrees below the horizon,
    // all of it from 60): the coat and collar right under the eye move out of the view instead of
    // filling it (QA: 25-42% of the view within 0.2 m at 45-85 degrees down).
    bool body_align{true};
    // How (body_posture): true = lean, the default since 2026-10-05 (feedback: "the torso feels
    // squished walking forward, the waist bends oddly"): the trunk leans from the waist to put the
    // eye over yours and keeps its length; a height difference within body_vertical_deadband_m is
    // left alone (the walk's bob, the hunter's eye a few cm above yours), only the rest moves the
    // upper body down (a duck; never up: walking and running carry his eye 3-20 cm lower, and
    // pulling the trunk up to the camera stretched it). false = shift: the upper body slides
    // against the hips (the waist shears and squeezes; body_align_max_m sideways).
    bool body_posture_lean{true};
    float body_lean_side_deg{20.0f};
    bool body_comfort{false}; // lateral clipping protection and staged recovery (opt-in)
    float body_vertical_deadband_m{0.08f};
    // A duck bends at the hips and comes all the way down (body_duck_lean_deg, since 2026-10-08:
    // in the tester's forward-leaning duck the 35-degree lean left his shoulders 10 cm behind your
    // head and the right hand 10-12 cm short of its controller): as your eyes go 0.10 .. 0.35 m
    // below his, the trunk's forward limit rises from 35 degrees to this and the 8 cm height
    // deadband fades out (core/vr/body_solver.h DuckWeight). 35 = no duck posture (the standing
    // limit and deadband everywhere, as before).
    float body_duck_lean_deg{70.0f};
    // Arms (body_shoulder_reach_deg): a controller still beyond the arm's reach with its 10%
    // stretch turns the shoulder (the clavicle) further toward it by what closes the gap, at most
    // this much (core/vr/arm_ik.h ArmParams::clavicleCatchUp). 0 = off (as before).
    float body_shoulder_reach_deg{35.0f};
    // Natural arms (body_natural_arms, 2026-10-08; the tester: crossed hands and hands brought to
    // the chest "don't fold like real human hands"): the forearm turns the hand by itself up to
    // body_forearm_pronation_deg palm down and body_forearm_supination_deg palm up before the
    // elbow moves (only the roll beyond them turns the elbow out and up / in and down, like a
    // shoulder taking over), the forearm's twist helpers follow the twist continuously up to
    // body_wrist_twist_max_deg (no snap or pinched wrist past 120 degrees), the elbows of arms
    // folded across the chest hang lower, and an elbow never goes into his trunk (it turns out
    // to the ribs instead). The hands and the weapon stay exactly on the controllers. false = the
    // arms before it (core/vr/arm_ik.h ArmParams::naturalRoll, body_solver.h TrunkKeepOut).
    bool body_natural_arms{true};
    float body_forearm_pronation_deg{75.0f};
    float body_forearm_supination_deg{90.0f};
    float body_wrist_twist_max_deg{150.0f};
    // The hunter's height follows your real head over his feet (fp_eye_height_m plus your head's
    // rise or drop), not the first-person camera's own vertical smoothing on stairs and slopes
    // (which lags 10 cm and more behind his steps and squeezed or stretched his trunk).
    bool body_height_from_head{true};
    float body_align_max_m{0.20f};
    float body_back_m{0.0f};
    float body_back_down_m{0.0f};
    float body_align_ms{50.0f};
    // Hand-over between your arms and the game's animation.
    float arm_blend_in_ms{300.0f};
    float arm_blend_out_ms{80.0f};
    // The game's animations take the arms (attacks, dodges, hits, items, doors, gestures: the
    // phantom-hand gating, core/vr/anim_classes.h). false = only throws, ladders, event animations
    // and death do.
    bool arm_anim_gate{true};
    // Your arms stay on the controllers through a hit stagger (arm_keep_on_hit) and through a
    // dodge, quickstep or backstep (arm_keep_on_dodge) while the body plays the game's clip; swings,
    // the transform and shots still wait for the game's cancel window (core/vr/anim_classes.h
    // KeepsArms). false = the game's animation takes both arms until that window (as before).
    bool arm_keep_on_hit{true};
    bool arm_keep_on_dodge{true};
    // Ghost hands where your controllers are while the game owns an arm.
    bool ghost_hands{false};
    float ghost_alpha{0.35f};
    // Weapon grip calibration on top of the default grip (core/vr/body_frame.h DefaultGrip).
    float weapon_grip_pitch_deg{0.0f};
    float weapon_grip_yaw_deg{0.0f};
    float weapon_grip_roll_deg{0.0f};
    // Live weapon grip adjust (presenter): hold A+B for 1 s, the weapon freezes, line the
    // controller up with it, release; saved per weapon family (weapon_grip_<weapon>).
    bool grip_adjust{true};
    // Body haptics: a tick when a hand leaves the hunter's reach, a deny pattern on both hands
    // when the body turns itself off.
    bool body_haptics{true};
    // Time budget of the body's work per main-player pose copy (microseconds, averaged over 5 s;
    // 0 = no guard). Two windows over it in a row turn the body off for the session.
    float body_budget_us{1000.0f};
};

// A weapon grip trim: the rotation in degrees in the controller frame (pitch + = the blade up,
// yaw + = the blade outward, roll + = the grip's top inward; mirrored for the left hand,
// core/vr/body_frame.h GripTrimRotation) and the weapon's offset from the controller grip in
// metres, in OpenXR grip axes (-Z through the fist from the little finger to the thumb, +X out of
// the left palm and into the right one).
struct GripTrim {
    float pitch_deg{0.0f};
    float yaw_deg{0.0f};
    float roll_deg{0.0f};
    std::array<float, 3> offset_m{};
};

// Limits of a per-weapon grip (the live adjust and hand-edited weapon_grip_<weapon> lines).
constexpr float GripPitchLimitDeg = 60.0f;
constexpr float GripYawLimitDeg = 45.0f;
constexpr float GripRollLimitDeg = 60.0f;
constexpr float GripOffsetLimitM = 0.05f;

[[nodiscard]] BodySettings& GetBodySettings();
[[nodiscard]] const char* BodyModeName(BodyMode mode);

// body_head_shadow applies: the solver leaves the head its size and the renderer hides it in the
// colour passes of first-person frames (both sides decide with the same inputs).
[[nodiscard]] constexpr bool HeadHiddenByRendererFor(bool body, BodyMode mode, bool head_shadow) {
    // Legacy full configurations must not expose the head in first-person VR.
    // Full never shrinks the skeleton, so always use the renderer for that mode.
    return body && (mode == BodyMode::Full || (mode == BodyMode::NoHead && head_shadow));
}
// The same for this frame: vr.ini, VR_HEAD_SHADOW (read once) and VR_TUNE_FILE body.head_shadow
// (live A/B tests).
[[nodiscard]] bool HeadHiddenByRenderer();

// body_mode=arms_only / hands_only by draw: the solver scales nothing and the renderer draws only
// the hands and forearms / the hands of the hunter in the colour passes of first-person frames.
// `by_draw` false: arms_only the old way, the Spine scaled (VR_TUNE_FILE body.arms_draw=0, A/B
// tests only; hands_only has no other way).
[[nodiscard]] constexpr DrawnArms DrawnArmsFor(bool body, BodyMode mode, bool by_draw) {
    if (!body) {
        return DrawnArms::Whole;
    }
    if (mode == BodyMode::HandsOnly) {
        return DrawnArms::Hands;
    }
    return mode == BodyMode::ArmsOnly && by_draw ? DrawnArms::Forearms : DrawnArms::Whole;
}
// The same for this frame (vr.ini, the VR page, VR_TUNE_FILE body.mode / body.arms_draw).
[[nodiscard]] DrawnArms DrawnArmsNow();

// Which of the hunter's parts besides the hands are drawn in first person (and so can stand in
// front of a HUD piece: the presenter's UiPieceOccluded, ui_hand_fade). The hands (and what they
// hold) are drawn in every mode. 2026-10-08 review: in hands_only the HUD faded for the invisible
// forearm, upper arm and torso (the counters faded looking 53 degrees and more down).
struct DrawnParts {
    bool forearms{true};
    bool upper_arms{true};
    bool torso{true};
};
[[nodiscard]] constexpr DrawnParts DrawnPartsOf(DrawnArms arms) {
    switch (arms) {
    case DrawnArms::Hands:
        return {false, false, false};
    case DrawnArms::Forearms:
        return {true, false, false};
    case DrawnArms::Whole:
        break;
    }
    return {true, true, true};
}

// body_mode as it applies this frame: vr.ini / the VR page, or VR_TUNE_FILE body.mode (0 =
// no_head, 1 = arms_only, 2 = full, 3 = hands_only; live A/B tests).
[[nodiscard]] BodyMode EffectiveBodyMode();

// The grip of the global weapon_grip_pitch_deg / _yaw_deg / _roll_deg keys (no offset).
[[nodiscard]] GripTrim DefaultWeaponGrip();
// The grip of a weapon family (core/vr/bb_weapons.h: EquipParamWeapon id / 100000): its
// weapon_grip_<weapon> line, else DefaultWeaponGrip(). `custom` (optional) says which. Any thread.
[[nodiscard]] GripTrim WeaponGrip(s32 family, bool* custom = nullptr);
// Sets a family's grip (nullptr: back to the default) and saves it to vr.ini as
// weapon_grip_<weapon>=pitch,yaw,roll,x_cm,y_cm,z_cm (or =default). Presenter thread.
void SetWeaponGrip(s32 family, const GripTrim* trim);
// Changes with every SetWeaponGrip, so readers know to fetch their grip again.
[[nodiscard]] u32 WeaponGripGeneration();
// The vr.ini key of a family: weapon_grip_saw_cleaver, or weapon_grip_<family> for one without a
// name.
[[nodiscard]] std::string WeaponGripKey(s32 family);
[[nodiscard]] std::string WeaponName(s32 family);

// One vr.ini line. False when `key` is not a body setting. Out-of-range values are clamped and
// logged; std::stof errors propagate (the caller logs the bad line).
bool ParseBodySetting(const std::string& key, const std::string& value);

// The body section of a fresh vr.ini.
void WriteBodyDefaults(std::ostream& out);

} // namespace VR::Body
