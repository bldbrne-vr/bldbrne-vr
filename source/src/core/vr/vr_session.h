// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <array>
#include <optional>
#include <string>
#include "common/types.h"
#include "core/vr/aa_mode.h"
#include "core/vr/assist_settings.h"
#include "core/vr/cinematic_state.h"
#include "core/vr/space_shift.h"

// Game-agnostic VR state shared between the OpenXR frame thread, the game's camera
// hook (guest thread), the GPU command thread and the presenter.
//
// Stereo models:
// - stereo_mode 2 (DIBR): every guest frame is rendered once from the centre of the head and both
//   eyes are synthesised from it on the GPU with the scene depth (renderer_vulkan/vr_stereo_warp),
//   so both eyes show the same game frame, every frame.
// - Alternate-eye rendering: each guest frame is rendered for one eye, either by moving the game's
//   render camera half the IPD left or right (stereo_mode 1, AER: lighting, shadows and decals
//   get correct parallax) or by shifting perspective geometry in clip space (stereo_mode 0).
// Every presented frame carries a FrameTag that records which eye it was rendered for (or that it
// is a centre frame) and the head pose the game camera was driven with, so the compositor can
// reproject each eye independently.
namespace VR {

struct Vec3 {
    float x, y, z;
};

struct Quat {
    float x, y, z, w;
};

struct Pose {
    Quat orientation{0.0f, 0.0f, 0.0f, 1.0f};
    Vec3 position{0.0f, 0.0f, 0.0f};
};

struct Fov {
    float angle_left, angle_right, angle_up, angle_down; // radians, OpenXR convention
};

// Head state published by the OpenXR frame thread (tracking space = LOCAL).
struct HeadState {
    bool valid{false};
    s64 time{};  // XrTime the pose was predicted for
    Pose head{}; // VIEW space pose in LOCAL space
    // Each eye's FOV as located by the runtime (asymmetric on most headsets). The eye images
    // are submitted with exactly these angles, cropped out of the symmetric game render.
    std::array<Fov, 2> eye_fov{};
    std::array<Pose, 2> eye_pose{}; // each eye relative to the head (VIEW space)
    float ipd{0.064f};
    // The runtime takes a submitted FOV other than the located one (fovMutable). False (Meta Link,
    // or unknown): the game FOV is never narrower than the headset's.
    bool fov_mutable{false};
    // The tracking space the sample was located in (ShiftTrackingSpace; stamped by PublishHead):
    // changes on every recenter.
    u32 space_serial{0};
    // The floor's height in the same space (m), located from a floor-level reference space
    // (LOCAL_FLOOR, else STAGE) only while Height is As the player or a seated eye height is set
    // (player_body.h).
    bool floor_valid{false};
    float floor_y{0.0f};
};

// Orientation the game camera was driven with for one camera update.
struct CameraRecord {
    u64 id{};
    bool driven{false};   // true when the camera was driven by the headset
    float head_yaw{};     // radians, tracking space, + = turned left
    float head_pitch{};   // radians, tracking space, + = looking up
    float head_roll{};    // radians about the view axis, + = the head's right side up (camera_roll)
    // render_pitch_bias_deg (core/vr/render_tilt.h): head_yaw / head_pitch / head_roll above are
    // the RENDER camera's orientation (what re-sees the frame from your head starts from them:
    // the late warp, the eye TAA, the UI's rest quad, the compositor), which looked this much
    // lower than your head (radians, + = lower; 0 = the head's own). Your head's pitch at this
    // update: head_pitch + pitch_bias (TrueHeadPitch).
    float pitch_bias{0.0f};
    [[nodiscard]] float TrueHeadPitch() const {
        return head_pitch + pitch_bias;
    }
    Vec3 head_position{}; // tracking space
    // The tracking space the head pose above is in (HeadState::space_serial): a frame still in
    // flight through a recenter is moved into the new space with SpaceShiftBetween.
    u32 space_serial{0};
    float vfov{};         // vertical game FOV in radians used for this update
    float aspect{};       // the game's render aspect (render camera +0x54), 0 = unknown
    float ipd{};          // eye distance in meters the cameras were placed with (EffectiveIpd)
    u32 turn_serial{};    // changes on every artificial turn / camera takeover
    s32 eye{-1};          // stereo_mode 1: eye the render camera was placed for (-1 = mono)
    // stereo_mode 0: the eye frame this update was placed for (0 = a pair's left frame, which
    // advances the whole view; 1 = the right frame after it, which keeps it; -1 = other modes).
    s32 shift_eye{-1};
    // stereo_mode 1: the stereo pair of this update. The left eye's update starts a pair; the
    // right eye's update after it renders with the same head sample, turns and heading, and the
    // presenter shows the two images together (0 = mono).
    u64 pair{};
    // Located per-eye FOV of the head sample that drove this update; each eye image is
    // submitted with it.
    std::array<Fov, 2> eye_fov{};
    // Pair integrity (stereo_mode 1): the presenter only pairs two eye images whose updates are
    // close in time and share the game heading.
    s64 tick_ns{};           // steady clock time of this camera update
    s64 head_time{};         // XrTime the head sample of this update was predicted for
    float base_yaw{};        // radians: game heading of tracking-space forward (turns, lock-on)
    float world_scale{1.0f}; // game units per meter of the offsets below
    Vec3 eye_offset{};       // game units: the stereo eye offset in the render camera position
    bool has_render_position{false};
    Vec3 render_position{}; // game units: final render camera position (render-camera hook)
    // How the render camera's position follows the head (the late warp re-sees the frame from the
    // head at display time with them, renderer_vulkan/vr_late_warp_math.h CameraPoint): the 6DoF
    // origin (tracking space), the head offset's cap (0: positional off, the head never moves the
    // camera) and its room below the origin (core/vr/head_room.h DownLimit; 0: the sphere of the
    // cap all round) and the first-person eye in front of the head centre along the view's yaw
    // (metres; 0 outside first person; the render-camera hook).
    Vec3 head_origin{};
    bool roomscale{false}; // horizontal tracking is free; vertical limits still apply
    float head_offset_limit_m{0.0f};
    float head_offset_down_m{0.0f};
    float eye_forward_m{0.0f};
    // The render-camera hook placed the first-person eye (the camera follows the hunter's body:
    // late_warp_motion moves the world with it between frames; in third person it does not).
    bool first_person{false};
    // The camera's base height over the hunter's feet minus the nominal fp_eye_height_m (metres,
    // negative = lower: the comfort camera's vertical smoothing on stairs and slopes, broken
    // floors; the body's anchor camera_dip). late_warp_motion puts the floor plane under his body
    // by it (the render-camera hook, while the VR body runs).
    bool has_floor_dip{false};
    float floor_dip_m{0.0f};
    // The hunter's model origin (his ChrCtrl world translation, game units) in the same render
    // tick (the render-camera hook, while the VR body runs): the renderer knows his own parts'
    // draws by their object transform standing exactly there (vk_rasterizer_vr_draws.cpp), so a
    // hunter NPC standing close is never taken for him.
    bool has_player_position{false};
    Vec3 player_position{};
    // stereo_mode 2 (DIBR) was in effect for this update: the camera sits at the head centre and
    // both eyes are synthesised from its frame.
    bool dibr{false};
    // Placed for the menu panel (MenuPanelActive): the frozen mono view at the panel's FOV. Its
    // frame always goes to the panel, never to the eyes.
    bool menu{false};
    // The projection the game rendered this update with, after our FOV / near-plane writes (render
    // camera +0x50 / +0x54 / +0x58 / +0x5C, render-camera hook). DIBR turns depth into disparity
    // with exactly these values.
    bool has_projection{false};
    float proj_vfov{};   // radians
    float proj_aspect{}; // P00 = cot(vfov / 2) / aspect
    float near_z{};      // game units
    float far_z{};       // game units
    // The game's vial in the hunter's hand (core/vr/bloodborne_held.h, render-camera hook): its
    // glass writes no depth, so DIBR stamps this capsule into the frame's disparity instead. View
    // space of this update's render camera, game units (x right, y up, z forward along the view).
    bool has_held_proxy{false};
    Vec3 held_a{};
    Vec3 held_b{};
    float held_radius{};
};

// Metadata attached to every guest frame at the GPU frame boundary.
struct FrameTag {
    bool valid{false};
    Aa::Mode aa_mode{Aa::DefaultMode}; // mode used to render this frame
    bool immersive{false}; // false: show on the virtual screen
    s32 eye{-1};           // 0 = left, 1 = right, -1 = mono (both eyes)
    // The stereo pair of an eye frame: the camera record's (stereo_mode 1), or a left frame and
    // the right frame after it (stereo_mode 0). Both eyes of a pair are shown together.
    u64 pair{};
    float stereo_shift{}; // clip-space x shift used for this frame
    // stereo_mode 2: an immersive centre frame whose eyes are synthesised from its depth (DIBR).
    bool dibr{false};
    // Drawn while the menu panel was up (MenuPanelActive): a mono frame with the HUD and menus at
    // full size, shown on the menu panel (never immersive).
    bool menu{false};
    // Not shown in the headset (the last image stays up): at a menu's open or close the frame was
    // drawn for the panel with a camera placed for the eyes, or the other way round.
    bool hold{false};
    // The UI layer (vr.ini ui_layer) while the frame was drawn: 0 = none (the UI is in the eye
    // images, today's path), 1 = the UI shadowed onto its own layer, 2 = shadowed and checked only.
    u8 ui_layer{0};
    // The game menu open while the frame was drawn (UiClass): where the presenter shows its UI.
    u8 ui_class{0};
    // The camera update the frame's draws used (the one current at its first draw), which every
    // frame is submitted with: its pose, an AER frame's eye and pair, the DIBR capture's camera.
    CameraRecord record{};
    // The camera update the flip was queued with (the newest when the game submitted it), which
    // can be one newer than `record`; kept for the statistics and the test stamps.
    u64 flip_record{};
    // VR_STAMP tests (DIBR frames): the newest update when the GPU thread reached the composite.
    u64 aux_record{};
    // Flipped-frame order (1, 2, ...): the presenter never replaces a shown frame with an older
    // one (DIBR frames reach the headset from the GPU thread, the others from the present thread).
    u64 seq{};
};

enum class DisplayMode : u32 {
    Auto = 0,   // immersive while the gameplay camera is active, screen otherwise
    Screen = 1, // always show the virtual screen
};

// vr.ini eye_size (Settings::eye_size; the same order as Vulkan::VrEyeScale::Mode).
enum class EyeSize : u8 {
    Auto = 0,   // the runtime's recommended eye size (with Meta Link the Link app's slider)
    Panel = 1,  // the headset's own panel pixels per eye (Quest 3: 2064x2208)
    Factor = 2, // eye_upscale times the render's crop
};

// vr.ini values. `stereo` and `stereo_mode` are only the starting mode: the mode in effect is
// GetStereo() (switched live in VR).
struct Settings {
    bool stereo{true};          // stereo; false = mono immersive
    // 2 = DIBR (one centre frame, both eyes from its depth; the default), 1 = per-eye camera
    // position (AER), 0 = clip-space shift
    s32 stereo_mode{2};
    s32 first_person{1};        // 1 = camera in the hunter's head, 0 = third person
    float fp_eye_height_m{1.55f}; // first person: eye height above the feet (game units)
    float fp_forward_m{0.1f};     // first person: eye in front of the head centre
    // First person: near clip (0.2 would hide more of the hunter's collar and sleeves under the
    // eye; 0.1 is what the tester verified as comfortable).
    float fp_near_m{0.1f};
    // First person, looking down (ramped in from 20 to 40 deg below the horizon): the near clip
    // moves out to this, hiding the collar, chest and shoulder caps right under the eye (0 = off).
    float fp_near_down_m{0.0f};
    // First person: the camera this much further ahead of the hunter's eyes (along your view's
    // heading), always (fp_ahead_m) plus, looking down (ramped in from 25 to 60 deg below the
    // horizon), fp_ahead_down_m. Unlike fp_forward_m his body does not follow it: the whole body,
    // legs included, ends up that much behind the camera (nothing is bent), so less of his collar
    // and chest is right under your eyes; your hands stay on your controllers.
    float fp_ahead_m{0.0f};
    float fp_ahead_down_m{0.03f}; // with the body cutaway: the chosen design (option c)
    // The render camera rolls with your head about the view axis, so the eye cameras (AER) and
    // the warp's baseline (DIBR) lie on the line between your eyes. 0 = a level camera (the old
    // behaviour: with the head tilted, near objects get vertical disparity, about 1.5 deg on a hand
    // at 0.5 m per 12 deg of tilt).
    bool camera_roll{false};
    bool positional{true};      // real head movement moves the game camera (6DoF)
    float positional_limit_m{0.5f}; // max distance the head may move the camera
    // ...and below the recentered head (core/vr/head_room.h: the sphere above, a cylinder of its
    // radius this deep below; a duck that leans forward is followed all the way; 0 = the sphere;
    // 0.65 until the evening of 2026-10-08, the tester: "15cm more? (so up to 30 in total)")
    float positional_limit_down_m{0.80f};
    bool roomscale_walk{true}; // standing first person: the feet follow physical steps
    float roomscale_start_m{0.28f};
    float roomscale_stop_m{0.12f};
    float roomscale_max_stick{0.45f};
    float world_scale{1.0f};    // game units per meter
    float ipd_scale{1.0f};      // multiplies the runtime IPD
    float ipd_mm{0.0f};         // eye distance for the game cameras; 0 = the headset's IPD
    float vfov_deg{0.0f};       // 0 = derive from headset FOV
    float fov_margin_deg{3.0f}; // extra game FOV around the headset's (each eye is cropped)
    // render_pitch_bias_deg (core/vr/render_tilt.h): the game's render camera looks this many
    // degrees lower than your head, so the symmetric render is centred on your eyes' 44-up /
    // 55-down range instead of on your head: with the same fov_margin_deg the render is less tall
    // (more pixels per degree) and the late warp's room is shared between the top and the bottom
    // (vertical half = max(up + bias, down - bias) + fov_margin_deg). Only with DIBR while the late
    // warp shows the eyes (it undoes the bias exactly: nothing you see moves or tilts), never on
    // the menu panel. 0 = off (as before 2026-10-07); sized from your head motion: 5 with a margin
    // of 8, the default since the TILT 2560 review
    // (2026-10-07). -15 .. 15.
    float render_pitch_bias_deg{5.0f};
    // render_pitch_min_margin_deg: while the bias applies, the margin is at least this
    // (Tilt::MarginDeg: max(fov_margin_deg, this)); a larger fov_margin_deg is kept. Without it a
    // margin sized for the unbiased render (yours: 6) would leave 7 up / 6 down; with 8: 9 / 8.
    // Without the bias (menu panel, AER, no late warp) fov_margin_deg alone, as before. 0 .. 20.
    float render_pitch_min_margin_deg{8.0f};
    // render_steady (core/vr/render_steady.h): the game's render camera holds its pose while your
    // head stays within render_steady_deg / render_steady_mm of it (standing "still": sway,
    // tremor), is pulled along at the band's edge beyond it, and follows your head exactly while it
    // turns faster than render_steady_follow_deg_s or moves faster than render_steady_follow_mm_s.
    // The game then renders the same view while you stand, so its aliasing (twigs, railings,
    // glints) stops blinking; the late warp re-sees every frame from your true latest head pose, so
    // nothing you see lags or moves. Only with DIBR while the late warp shows the eyes, never on
    // the menu panel. 0 = off (the render camera follows every head sample, as before 2026-10-08).
    // Native stereo: optional angular-only hold. The compositor uses the recorded
    // render orientation; positional tracking stays exact. No extra GPU pass.
    // Opt in until headset comfort/outer-edge checks are accepted.
    bool native_render_steady{false};
    float native_render_steady_deg{0.2f}; // 0 .. 0.5, independent of DIBR's wider band
    bool render_steady{true};
    float render_steady_deg{0.8f};          // 0 .. 3
    float render_steady_mm{5.0f};           // 0 .. 20
    float render_steady_follow_deg_s{8.0f}; // 1 .. 60
    float render_steady_follow_mm_s{50.0f}; // 5 .. 500
    // late_warp_steady (with late_warp; the same filter, core/vr/render_steady.h): the late warp
    // re-sees the frame for a held head pose while your head stays within late_warp_steady_deg /
    // late_warp_steady_mm of it (faster than render_steady_follow_deg_s / _mm_s: your head
    // exactly), and the eyes go to the headset with that pose: the runtime's own reprojection
    // turns them to your head at display time (exact for rotation; translation too with
    // xr_depth). Standing still, every refresh then samples the frame at the same place, so the
    // late warp's own resampling (thin twigs, nearest texels at depth edges) stops blinking.
    // 0 = the late warp re-sees every refresh for the head at its display time (as before).
    bool late_warp_steady{false};
    float late_warp_steady_deg{0.25f}; // 0 .. 1
    float late_warp_steady_mm{2.0f};   // 0 .. 10
    float pitch_limit_up_deg{80.0f};
    float pitch_limit_down_deg{80.0f};
    float snap_turn_deg{30.0f}; // 0 = smooth turn
    float smooth_turn_deg_per_s{120.0f};
    s32 pose_lag_frames{0}; // camera updates between camera tick and GPU frame boundary
    float camera_stale_ms{500.0f};
    // A gap longer than camera_stale_ms where the game camera stayed put (a hitch: a shader
    // compile, a stall) resumes the view: the heading and the 6DoF origin are kept, so a duck
    // through a hitch is still a duck afterwards (core/vr/camera_gap.h). 0 = every gap takes over
    // again (the origin from your head at that moment: the behaviour before 2026-10-08).
    bool camera_gap_resume{true};
    float pair_max_age_ms{50.0f}; // stereo pair eyes further apart than this go mono
    float screen_distance_m{2.6f};
    float screen_width_m{3.4f};
    float pose_prediction_ms{25.0f}; // extra head prediction for the game camera
    float refresh_hz{120.0f};        // Hz to request (highest supported <=), 0 = default
    // How a close ends the process (core/vr/exit_sequence.h, emulator.cpp EndAfterClose; the
    // user's 2026-10-09 log: after Quit in the Meta Link menu the process never ended - the
    // runtime's DLL hung its exit). OpenXR is torn down in order first (the frame thread, the GPU
    // idle, the session, the instance), within xr_exit_teardown_ms (0 = not torn down), and on
    // Windows the process then ends with TerminateProcess once the play time, the shader cache, a
    // save being written and the log are done (xr_exit_terminate; no DLL detach can hang it, the
    // exit code kept; 0 = quick_exit's ExitProcess as before).
    float xr_exit_teardown_ms{1500.0f};
    bool xr_exit_terminate{true};
    // DIBR eyes are submitted with their depth (XR_KHR_composition_layer_depth), so the compositor
    // can correct head translation too (positional timewarp), not only rotation. Off by default
    // until it is confirmed in the headset (it also moves the submitted eyes to the headset's IPD).
    bool xr_depth{false};
    float xr_depth_debug_m{0.0f}; // > 0: every pixel at this distance instead (proof test)
    // Late warp (renderer_vulkan/vr_late_warp.h): every XR frame the DIBR pair on display is warped
    // again to the head at that frame's display time (rotation and translation, both eyes from the
    // game frame's capture), so the view is smooth at the headset's rate whatever the game's frame
    // rate. 0 = off (the game-rate pair as it is, rotation corrected by the runtime), 1 = on.
    s32 late_warp{1};
    // late_warp_motion (with late_warp): a game frame shown for 2-3 refreshes also moves on with
    // the game camera's own motion (walking, smooth and lock-on turns) between its updates, the
    // hunter's own body (arms, weapons, legs) staying put; none across snap turns, camera changes,
    // teleports, recenters, menus and cinematics, never past one frame. 0 = head motion only.
    bool late_warp_motion{true};
    // late_warp_edge_blend (with late_warp): at a depth edge of the game frame the late warp takes
    // the nearest texel (no smearing across silhouettes), which flips by a whole texel as the view
    // slides over the frame every refresh (the late warp's own crawl on fine detail: trees,
    // railings). Where the edge's two sides move apart by less than this many pixels between the
    // game frame and the view, the colour is blended instead (VrLate::EdgeBlendWeight: fully below
    // half of it); larger parallax keeps the nearest texel. Off while the world moves with the game
    // camera. 0 = the nearest texel everywhere (as before 2026-10-07), 0 .. 4.
    float late_warp_edge_blend{0.0f};
    // late_warp_fetch_once (with late_warp; VR_TUNE late.fetch_once): the late warp's eye pass
    // fetches each depth and colour of its sharp (Catmull-Rom) filter once instead of up to 44
    // times per pixel; the same tests on the same values, the same image (-0.05 ms per refresh on
    // the mock). 0 = as before 2026-10-09.
    bool late_warp_fetch_once{true};
    // stereo_adopt_finished (DIBR): after a stutter the GPU runs more than a game frame behind for
    // a while. 0 (before 2026-10-07): one published pair waits to go up and each new frame
    // replaces it, so every one is replaced before the GPU has finished it and the world stands
    // still until the GPU has caught up (0.3-0.4 s at 3200), then lurches forward. 1: two may
    // wait, and the newest one the GPU has finished goes up while a newer one is drawn, so the
    // world keeps moving at the GPU's pace (one more DIBR swapchain pair and late-warp capture
    // slot); 2: three. Read at startup (renderer_vulkan/vr_stereo_queue.h).
    s32 stereo_adopt_finished{1};
    // Picture quality.
    // texture_aniso: the game's filtered textures (bilinear or trilinear, not shadow maps or
    // point-sampled buffers) are sampled with at least this anisotropic filtering: sharp floors and
    // walls at grazing angles instead of the blur of the coarser mip (texture_cache/sampler.cpp).
    // 0 = the game's own sampler settings; 2, 4, 8 or 16. Read when a sampler is first made.
    s32 texture_aniso{16};
    // texture_lod_bias: added to the game's mip bias on its tiling (wrap/mirror) mipmapped textures
    // (-2 .. 1; negative = a finer mip: sharper, with more shimmer in motion). 0 = the game's own.
    float texture_lod_bias{0.0f};
    // aa_mode (core/vr/aa_mode.h; the VR page's Graphics > Anti-aliasing, live): SMAA 1x in place
    // of the game's FXAA (smaa, the default; renderer_vulkan/vr_smaa.h; a draw it refuses draws
    // the FXAA), the game's FXAA (fxaa), temporal AA in the game frame (taa: a sub-pixel jitter
    // of the render camera and a resolve with a reprojected history, core/vr/render_taa.h,
    // renderer_vulkan/vr_render_taa.h) or neither, the eyes' TAA alone (off: vr.ini only).
    // Immersive DIBR frames only; the flat screen keeps the game's FXAA.
    Aa::Mode aa_mode{Aa::DefaultMode}; // startup only; live readers use GetRequested/ActiveAaMode
    u64 aa_fxaa_fs{Aa::DefaultFxaaFs}; // the game's FXAA draw (fragment shader hash)
    // lod_bias (the VR page's Display > Distant detail): the Model LOD patch's bias, set as the
    // patches apply; a change in game is a store to its data slot (memory_patcher.cpp
    // SetLodBiasLive), at the next start when there is no slot. 0 Performance, the game's own
    // distant models
    // (the default since 2026-10-17: +11% frame rate in Old Yharnam); -1 Balanced; -2 Quality,
    // every object's full model at any distance (the default before).
    int lod_bias{0};
    // aa_mode=taa's knobs (core/vr/render_taa.h):
    //  taa_jitter: the Halton (2, 3) jitter's amplitude, x +-0.5 render px (0 = none: a temporal
    //    filter of the head's own motion only). 0 .. 1.
    //  taa_phases: the jitter cycle's length, 8 or 16.
    //  taa_alpha_min: the current frame's least share of the result (the history's length:
    //    1 / taa_alpha_min frames). 1/32 .. 0.5. 0.06 (round 2; round 1: 0.1): with the plain
    //    blend (taa_luma_weight 0) the longer history keeps a 1-in-8 glint's swing below round
    //    1's and halves the static background's jitter residual (0.50 -> 0.33 levels).
    //  taa_clip_gamma: the history is clipped to the current 3x3's YCoCg mean +- this many sigma
    //    (smaller: less ghosting, more flicker). 0.5 .. 2.
    //  taa_depth_reject: a history surface nearer than the reprojected point by more than this
    //    (relative 1/z) is a disocclusion: the history restarts there. 0.001 .. 0.2.
    //  taa_stable_depth: the DIBR and the late warp read the resolve's stable depth (an edge pixel
    //    keeps its side while the jitter moves the edge); 0 = the raw jittered depth.
    //  taa_sharpen: contrast-adaptive sharpening (RCAS) of the resolved frame, 0 (off) .. 1; the
    //    history itself stays unsharpened. Round 1: 0.25 (0.88x SMAA's sharpness in the Dream,
    //    0.67x in the dark clinic hall). Round 2 (taa2:tune, 2026-10-08): 0.7 with
    //    taa_sharpen_lift 0.5 and the eyes' TAA at 0.9 (see the docs' round-2 table).
    //  taa_filter_px: the current frame's reconstruction filter (a Gaussian, sigma in render px,
    //    0.2 .. 0.55 in 0.05 steps; 0.45 ~ UE4's Blackman-Harris fit). Round 1: 0.45 (0.3
    //    blinked ~2x more standing with alpha 0.1); round 2: 0.35 with alpha 0.06.
    //  taa_near_m / taa_near_alpha: within taa_near_m of the eyes (fading out to 1.5x it) the
    //    current frame keeps at least taa_near_alpha of the result: the hunter's weapon and hands
    //    move without motion vectors (the camera reprojection is wrong for them), so a long
    //    history smears them. 0 m = off. 1.2 m / 0.8: a swing's ghost ring 8.7 -> 6.4 (SMAA
    //    8.0); a waggled blade 8.3 -> 7.8 (SMAA 5.8).
    //  taa_eye_taa: the eyes' TAA weight while the render TAA runs (-1 = eye_taa's own). 0.9
    //    (round 2): what is left of the standing blink is the eyes' pipeline resampling held
    //    frames (the late warp every refresh), which only the eyes' TAA reaches: 0.9 halves the
    //    Dream's standing blink against 0.85 for ~5% of moving sharpness; 0.7 doubled it.
    //  taa_luma_weight: the blend's anti-flicker weighting 1 / (1 + k luma), k 0 .. 1 (1 = round
    //    1's; it also dims glints that are bright in a few jitter phases; 0 = a plain blend).
    //  taa_occluder: the occluder test (vr_render_taa.comp step 4b): where the whole history 2x2 is
    //    farther than all of this frame's 3x3, something without motion vectors moved in front
    //    (an enemy's leading edge): the history restarts there. 0 / 1.
    //  taa_edge_blend: late_warp_edge_blend while the render TAA runs (-1 = its own): the game
    //    frame's edges are anti-aliased and steady then, so sliding them with the head is safe.
    //  taa_eye_sharpen: eye_sharpen while the render TAA runs (-1 = its own).
    //  taa_sharpen_lift: taa_sharpen's black floor moved down by this for RCAS's limit, so dark
    //    textures are sharpened too (RCAS keeps its output above black). 0 .. 0.5.
    //  taa_debug (tests): 1 = the history map (red = rejected, green = the count), 2 = the current
    //    frame unjittered (no history), 3 = the raw jittered frame (no resolve).
    float taa_jitter{1.0f};
    s32 taa_phases{8};
    float taa_alpha_min{0.06f};
    float taa_clip_gamma{1.0f};
    float taa_depth_reject{0.01f};
    bool taa_stable_depth{true};
    float taa_sharpen{0.7f};
    float taa_filter_px{0.35f};
    float taa_near_m{1.2f};
    float taa_near_alpha{0.8f};
    float taa_eye_taa{0.9f};
    float taa_luma_weight{0.0f};
    bool taa_occluder{true};
    float taa_edge_blend{1.0f};
    float taa_eye_sharpen{-1.0f};
    float taa_sharpen_lift{0.5f};
    s32 taa_debug{0};
    // eye_size (eye_upscale is the older name of the same key): each eye enlarged with FSR 1's
    // EASU before the headset, so the compositor no longer stretches the render's pixels
    // bilinearly (vr_eye_upscale.h, vr_eye_scale.h): auto (the default) = to the runtime's
    // recommended eye size (the Link app's slider), panel = to the headset's own panel pixels per
    // eye (Quest 3: 2064x2208; Meta Link encodes ~2048 per eye whatever its slider says), a number
    // = `eye_upscale` (1 = off, 1.1 .. 2) times the render's pixels. panel is not the default:
    // through a model of Link's chain it was 0-5% sharper at the panel than auto (not visible),
    // for +0.11-0.14 ms of late warp per refresh (perf/eyesize, 2026-10-06). eye_sharpen: FSR 1's
    // RCAS sharpening after it, 0 (off) .. 1 (strongest). Not with xr_depth (the depth stays at
    // the render's pixels).
    EyeSize eye_size{EyeSize::Auto};
    float eye_upscale{1.0f};
    float eye_sharpen{0.5f};
    // eye_size_min_factor: with eye_size=auto, the eyes are enlarged at least this much (never
    // past the headset's panel or the runtime's largest image), so a render whose crop is already
    // the recommended size or larger (3200x2400 at Link 1x) still gets EASU instead of RCAS on
    // its raw pixels (x1.0: +80% stair-steps in the Dream; x1.13: +29%, +0.13 ms of late warp;
    // VrEyeScale::DefaultMinFactor). 1 = as before (1 .. 2).
    float eye_size_min_factor{1.13f};
    // eye_taa: the eyes' temporal anti-aliasing with the late warp (vr_eye_taa.comp): each refresh
    // keeps this share of the previous refreshes, moved exactly by the eye's motion since and
    // clamped to what the new one shows nearby, so edges stop crawling while you look around
    // (2026-10-06, mock: -77% crawl on the Dream's trees, a still view unchanged). 0 = off,
    // 0.3 .. 0.9.
    float eye_taa{0.85f};
    // DIBR-only eye history override: -1 inherits, 0 disables, 0..0.9 blends history.
    float dibr_eye_taa{-1.0f};
    // eye_taa_follow: with eye_taa, the history also follows the game camera (walking, smooth
    // turns: vr_eye_taa_math.h Choose; the original eye TAA started it again on every such refresh, two
    // thirds of real play), and still views are moved by the displayed camera (the 0.1 m forward
    // lever included). While the world moves the history is depth-checked: a sample the new view
    // just uncovered, the body and held items get none. 0 = exactly the original eye TAA.
    bool eye_taa_follow{true};
    // eye_taa_gate_px: while the world moves, a history sample nearer than this pixel's surface is
    // dropped when the two would move apart by more than this many eye pixels in one refresh (a
    // disocclusion); slopes and farther samples are kept (0.01 .. 1).
    float eye_taa_gate_px{0.05f};
    // eye_taa_edge_cap: while the world moves, at most this share of history where the 3x3 holds
    // surfaces that move apart by more than eye_taa_gate_px: there the history lags the edge
    // (synthetic walk on aliased rails: 0.5 has the least smear, a third of the shimmer of no
    // history; 1 = eye_taa everywhere, 0.1 .. 1).
    float eye_taa_edge_cap{0.5f};
    // eye_taa_jitter: with eye_taa, every camera update looks a sub-pixel step off the head (an
    // 8-step pattern, +- this many half render pixels, in its record too: the late warp puts each
    // frame exactly where it belongs), so the temporal AA gathers samples between the render's
    // pixels and a still view's edges smooth out as well (0 = off, 1 = +-0.5 px). An option: 0.5
    // with eye_taa 0.9 takes about a third of the still steps off, a little softer.
    float eye_taa_jitter{0.0f};
    // gpu_submit_ms: the game frame's GPU work is handed to the GPU during the frame, every this
    // many ms of recording, at a render-pass switch, instead of all at once at the flip: the GPU
    // finishes the frame sooner after the command processor does, and the late warp (the head
    // tracking) no longer queues behind a whole frame on the GPU (clinic hall, mock: its GPU work
    // done p99 9.9 -> 3.7 ms after xrEndFrame; ~6 more submits per frame, +0.6 ms of the command
    // processor). 0 = all at the flip (before 2026-10-06).
    float gpu_submit_ms{1.5f};
    // Per-draw CPU shortcuts of the command processor (video_core/renderer_vulkan/vk_perf_memo.h;
    // the image is the same, VR_PERF_CHECK=1 compares them with the full work). On by default
    // since 2026-10-09 (requested on): correct (0 mismatches in ~30 M
    // shadow-checked draws), saving ~0.15-0.33 ms of the command processor per game frame in the
    // clinic hall (within the A/B noise on the developer's PC). 0 = off (the full work per draw).
    // perf_spec_memo: the pipeline cache remembers which program permutation the same inputs (the
    // stage's registers, the fetch shader, the layout of the bound resources) resolved to, instead
    // of building and comparing a specialization for every stage of every draw.
    bool perf_spec_memo{true};
    // perf_rt_memo: the render targets' image descriptions are remembered by their CB / DB
    // register bytes instead of being built again for every draw.
    bool perf_rt_memo{true};
    // perf_bind_memo: a shader stage's image bindings (the texture cache's image, view and range
    // for each T#) are remembered by the bytes of its T#s while no image is registered or
    // unregistered, instead of being looked up again for every draw; the per-draw work (CPU-write
    // tracking, uploads of CPU-written textures, layout transitions) still runs.
    bool perf_bind_memo{true};
    // perf_copy_memo: the copy shader's HLE (vk_shader_hle.cpp) reuses the buffers and offsets it
    // obtained for the same source and destination ranges while no buffer was created or removed
    // and no page of either range changed its CPU or GPU write-tracking state since (the buffer
    // cache's epochs), instead of synchronizing both ranges again for every batch. Off by default:
    // correct, but it never hit in the clinic hall, the fight or the menus (0 of 1.34 M batches,
    // 2026-10-09): 60% of the batches read a small range through the stream buffer (a fresh copy,
    // nothing to reuse) and the others' pages always changed since the batch before (the game
    // writes the copy's source every frame). 1 = on (for tests in other scenes).
    bool perf_copy_memo{false};
    // GPU shortcuts between and inside render passes (video_core/renderer_vulkan/vr_perf.h; the
    // image is the same).
    // perf_resync_memo: a GPU-written image the game reads through a buffer is copied (re-tiled)
    // into that buffer only when the image or the buffer was written since the last copy, not at
    // every binding (identical bytes; VR_TUNE_FILE perf.resync_memo for A/B, VR_PERF_CHECK=1
    // compares every skipped copy with a fresh one).
    bool perf_resync_memo{false};
    // perf_depth_one_layout: a draw with depth writes off that does not sample the depth, while
    // the depth is in the writable layout, keeps the depth-write view and that layout (the writes
    // stay off through the dynamic depth-write state), so a depth-write toggle no longer ends the
    // render pass and transitions the depth image (VR_TUNE_FILE perf.depth_one_layout). On by
    // default since 2026-10-09: the same image, sync validation clean, 3 to 5 fewer render
    // passes per frame in the clinic hall (mock).
    bool perf_depth_one_layout{true};
    // The desktop window while the headset shows the game (core/vr/desktop_mirror.h; VR_TUNE
    // mirror.every / mirror.view). desktop_mirror: present every Nth frame that went to the
    // headset (1 = every frame as before 2026-10-09, 2 = 30 Hz, 0 = none: the window keeps its
    // last image). desktop_mirror_view (VR::DesktopMirror::View): right (2) / left (1) = that eye
    // as the headset gets it (one blit; the window-only post-process pass is skipped), game (0) =
    // the flat game frame with its HUD. Frames not sent to the headset (mono, cinematics, menus
    // held, a pause) always present as before.
    s32 desktop_mirror{2};
    u8 desktop_mirror_view{2};
    // Frame cap: the game's frame time a whole number of headset refreshes, so every game frame
    // stays up equally long (an even cadence). -1 = auto (120 Hz: 60 fps, 40 when 60 cannot be
    // held; 90 Hz: 45; 80 Hz: 40; 72 Hz: no cap), 0 = off (the game's own 60; the default until
    // auto is confirmed in the headset), N = N fps.
    s32 frame_cap{0};
    bool sim_motion{true};           // --vr-sim: animate the synthetic head
    bool gestures{true};             // swing the right controller to attack
    bool controls_classic{false};    // controls=classic: buttons only (no swings)
    // menu_hold: what holding the left Menu button (0.7 s) does. recenter (default): recenter the
    // view (the head becomes forward and the 6DoF origin); screen: the old binding, toggle the
    // virtual screen (Tab on the keyboard toggles it either way).
    bool menu_hold_screen{false};
    // y_menu: Y does the left Menu button's jobs in the motion layout's left_scheme=2, wherever
    // Menu does them (a tap = Options, held 0.7 s = the recenter, held 2.5 s on a cinematic's or
    // Tab's screen = back to VR; hold Y + B stays the gestures; core/vr/y_menu.h). 0 = auto (on where the runtime
    // owns Menu: SteamVR, whose dashboard takes the left Menu button), 1 = on, 2 = off (Y's tap is
    // the quick-item arc, Menu the menu). vr.ini and the env VR_Y_MENU (over it) take the same
    // words (VR::YMenu::ParseSetting: auto / on, 1, true, yes / off, 0, false, no).
    s32 y_menu{0};
    // Motion controls, the right grip: transform (default: the right grip transforms the weapon,
    // L1, and swings attack on their own; the left grip is free) or draw (right_grip=draw, the
    // old clutch: the right grip held draws the weapon, only a drawn weapon swings, the left grip
    // transforms).
    bool right_grip_draw{false};
    bool sticky_grip{false};         // right_grip=draw: a quick right-grip tap toggles the weapon
    // The left hand's layout (motion controls, 2026-10-05). 2 (the default since the evening of
    // 2026-10-05, after the review's fixes): the left stick click latches the sprint and A leaps
    // while sprinting (the locomotion half; Y held is the modifier, its tap the arc or, y_menu,
    // Options), the left grip
    // opens and closes the arc, the left trigger takes the card the hand is on (a throwable flies
    // when the trigger is let go, a usable is used at the pull), the gun fires off the cards, and
    // the grip at the left hip draws a Blood Vial (core/vr/quick_items.h, core/vr/belt_vial.h,
    // core/vr/left_locomotion.h). 1: the old left hand, Y opens the quick-item arc, the left grip
    // takes a card, the trigger uses it, the left stick click's tap opens the arc.
    s32 left_scheme{2};
    bool heavy_button{false};        // heavy=button: the right trigger holds R2 directly
    bool heavy_arc{true};            // a big swing from a wound-up start is a heavy attack
    float heavy_arc_deg{140.0f};     // blade sweep for a heavy (seated: x0.85)
    float heavy_windup_deg{100.0f};  // blade angle from forward that counts as wound up
    bool seated{false};              // seated play: smaller heavy arc and wind-up
    bool heavy_ready_tick{true};     // soft click when the weapon is wound up
    float blade_pitch_deg{0.0f};     // blade relative to the grip -Z axis (+ = toward grip +Y)
    // Overhead swings (core/vr/swing_detector.h, QA 2026-10-07): a wind-up looped into its strike
    // splits where the blade turns back toward the front by this much (0 = one stroke) ...
    float swing_windup_turn_deg{25.0f};
    // ... a stroke whose blade comes this much closer to the front is never a wind-up (0 = off) ...
    float swing_toward_front_deg{10.0f};
    // ... a stroke ignored as a wind-up is judged again while it lasts (a chop whose blade lagged)
    bool swing_windup_rejudge{true};
    // The weapon hand counts as at the face only once it is slower than this there (m/s): a stroke
    // passing the face at speed is no hand at the headset (0 = any speed, the old rule).
    float swing_face_enter_speed{1.5f};
    bool inject{true};               // X arms a Blood Vial, touching the wrist or body injects it
    bool inject_full_hp{false};      // allow an injection at full HP (wastes the vial, as vanilla)
    float vial_timeout_s{30.0f};     // an armed vial disarms itself after this long (0 = never)
    // The Blood Vial's stab zones (core/vr/vial_zones.h): their reach beyond the body and the arm
    // (the room the vial hand's controller needs), x this (0.5 .. 2; 1 = as designed).
    float vial_zone_scale{1.0f};
    // TEST / DEBUG kit (core/vr/bloodborne_givekit.h; env VR_GIVE_KIT / VR_GIVE_KIT_EQUIP
    // override): "0" = off, "1" = the default kit, or "<id>[:<qty>],..." top-up targets.
    std::string give_kit{"0"};
    bool give_kit_equip{true}; // with the kit: fill empty weapon and quick-item slots
    bool talk_guard{true};           // while an NPC is talking a swing never attacks (buttons do)
    // VR overlay (renderer_vulkan/vr_overlay.h): small 3D objects drawn at the headset rate in
    // their own layer on top of the game: the armed Blood Vial in the left hand, the controllers
    // on the virtual screen and in menus, ghost hands where the game's animation has an arm (body
    // ghost_hands / ghost_alpha) and the gun laser.
    bool overlay{true};             // false: none of them (no overlay swapchain is created)
    float overlay_scale{0.70f};     // eye image size relative to the recommended size, 0.5 .. 1
    s32 overlay_msaa{4};            // 1, 2, 4 or 8 samples
    float overlay_brightness{1.0f}; // exposure of the overlay's lighting
    bool vial_visual{true};         // the armed Blood Vial in the left hand
    std::array<float, 3> vial_offset_mm{0.0f, 0.0f, -8.0f}; // in the left grip (-Z = thumb side)
    std::array<float, 3> vial_rot_deg{0.0f, 0.0f, 0.0f};    // in the grip: X, then Y, then Z
    bool vial_holdout{false};    // the hunter's fingers cut out of the vial (only if they line up)
    // vial_in_hand=game: the armed vial is the game's own (the Blood Bullet vial, FFX 173) in the
    // hunter's left hand, following it through every animation, lit like the scene and hidden by
    // his fingers (core/vr/bloodborne_held.h); the overlay vial above is only the fallback when
    // that is unavailable. overlay: always the overlay vial at your left controller.
    bool vial_in_hand_game{true};
    // While it shows: 0 keep the left weapon, 1 hide it (as the game's own clip), 2 all its parts.
    s32 vial_hide_left{1};
    bool shoulder_switch{true}; // settled right shoulder + B / left shoulder + Y
    bool health_color{true};
    bool stamina_color{true};
    float health_color_strength{0.5f};
    float stamina_color_strength{0.7f}; // 0.5, then 0.9, 0.8, 0.7 (2026-10-15)
    bool vial_hide_two_handed{true}; // no vial during two-handed attacks (that hand is on the hilt)
    float vial_inject_hold_s{0.3f};  // the game's vial stays this long after a stab (at the wrist)
    // Fixed wall-clock pause, followed by vial_inject_release_ms of easing back to tracking.
    s32 vial_inject_hold_ms{210};
    // Compatibility only: legacy frames=0 disables the pause; positive values no longer time it.
    s32 vial_inject_hold_frames{10};
    bool vial_inject_hold_both{true}; // vial_inject_hold_hands=both (true) | vial
    s32 vial_inject_release_ms{80};
    // A healed stab: the vial in the hunter's hand empties (core/vr/vial_drain.h): its blood drops
    // from full to empty over vial_inject_drain_ms (a plunger: soft start, steady pour, soft stop),
    // then the empty vial stays vial_inject_empty_ms (200: put away 620 ms after the THUMP; 80
    // until 2026-10-09, gone at 500 ms; 0 .. 1000) and goes away as before. 0 = no drain (the
    // game's full vial to the end).
    bool vial_inject_drain{true};
    s32 vial_inject_drain_ms{420};
    s32 vial_inject_empty_ms{200};
    // A thin steel needle (and its small hub) out of the stabbing end of the Blood Vial in the
    // hunter's hand, drawn on the vial in the game's frame, exactly while the vial is shown
    // (core/vr/vial_needle.h). Visual only: the injection's zones are unchanged. 0 = none.
    bool vial_needle{true};
    // DIBR: the game's vial glass writes no depth, so a capsule at the vial is stamped into the
    // disparity (else the vial warps at the depth of what is behind it, visibly behind his hand).
    bool vial_dibr_proxy{true};
    s32 controller_models{1};    // controllers drawn: 0 never, 1 virtual screen and menus, 2 always
    bool laser{false};           // gun laser from the left hand while it is raised to aim
    bool overlay_debug{false};   // grip and aim axes on both controllers (VR_OVERLAY_DEBUG=1 too)
    // Heal vignette (VR overlay): a soft glow at the edges of the view, in over 0.15 s and out
    // over 1.2 s, whenever an item heals you (an injection, the game's own Blood Vial or another
    // healing item; never the HP won back by hitting enemies).
    bool heal_vignette{true};
    float heal_vignette_strength{0.35f}; // opacity at the edge of the view, 0 .. 1
    std::array<float, 3> heal_vignette_color{0.35f, 0.95f, 0.45f}; // display colour (sRGB) 0 .. 1
    // Hit vignette (VR overlay): a small soft red flash at the very edges of the view when an
    // attack takes HP (in over 60 ms, out over hit_vignette_fade_ms; 70% .. 100% of the strength by the HP lost,
    // full from a quarter of max HP). Never for HP lost without a hit (poison, toxic, ...).
    bool hit_vignette{true};
    float hit_vignette_strength{0.3f}; // opacity at the edge of the view, 0 .. 1
    std::array<float, 3> hit_vignette_color{0.8f, 0.05f, 0.04f}; // display colour (sRGB) 0 .. 1
    float hit_vignette_fade_ms{500.0f}; // how long it takes to fade out after the flash
    // Where the red starts (core/vr/hit_vignette.h): clear inside this fraction of the displayed
    // field from the centre, full strength at the visible edge; for the flash, its wedge and the
    // hit-stun hold (0.3 .. 0.95; 0.464 = the ring until 2026-10-09, 0.53 its first default that
    // day, larger = a thinner red rim).
    float hit_vignette_inner{0.58f};
    // Directional hit feedback (core/vr/hit_direction.h): the hit vignette weighted toward where
    // the blow came from relative to your head (a wedge on that edge of the view: ahead = top,
    // behind = bottom), and the controller on that side vibrating stronger than the other.
    bool hit_vignette_directional{true};
    // The vignettes (hit flash and wedge, hit-stun, stagger / knockdown black, heal, low-HP edge,
    // rally rim) drawn into the eyes by the late warp's last pass (renderer_vulkan/
    // vr_vignette_warp.h; VR_TUNE vig.warp) instead of the VR overlay's own full-view layer,
    // which the compositor blended every refresh. The same look per eye (ring, colour, depth);
    // the UI layers and the overlay's items (vial, controllers, hands) now sit over them. Only
    // while the late warp runs and enlarges or sharpens the eyes (the default), else on the
    // overlay as before. 0 = always on the overlay (before 2026-10-09).
    bool vignette_in_warp{true};
    bool hit_haptics_directional{true};
    // Stagger vignette (core/vr/daze.h): the edges of the view darken while a blow's stagger or a
    // knockdown holds the hunter (in over 90 ms, out over 150 ms as the game's attack window opens),
    // under the red hit flash; edge opacity 0.55 (stagger) / 0.75 (knockdown) times the strength.
    bool stagger_vignette{true};
    float stagger_vignette_strength{1.0f}; // 0 .. 1.5
    // A clearing fades to a faint rest at once and holds it this long (VR::Daze::MergeFloor), so a
    // blow that staggers again inside it does not flash the view clear and dark; 0 = full fade.
    float stagger_vignette_merge_ms{400.0f}; // 0 .. 1000
    // 1: the stagger vignette (and the red hit flash, hit_vignette_fit) fitted to what the
    // headset shows (the eyes' displayed field from the runtime, not the wider render): dark over
    // the outer ~35% of the view, 0.75 at the visible edge for a stagger, 0.9 for a knockdown,
    // with a darker onset dip over stagger_vignette_dip_ms; 0 = the old fixed degrees (0.55 /
    // 0.75, full from 58 / 52 degrees, mostly beyond a Quest 3's view).
    bool stagger_vignette_fit{true};
    float stagger_vignette_dip_ms{120.0f}; // 0 .. 400 (0 = no dip)
    bool hit_vignette_fit{true};
    // The red hit flash over the stagger tunnel (VR::Daze::RedScale): it keeps this share of its
    // opacity at the tunnel's full darkness, thinner, and fades out over at most 220 ms, so the
    // tunnel reads black and the blow's side as a brief red rim; 1 = the flash as before (0 .. 1).
    float stagger_vignette_red{0.35f};
    // Hit-stun vignette (core/vr/hit_stun.h): the red hit flash held while a blow's on-feet hit
    // reaction refuses swings (before the game takes R1 again), gone within 120 ms of the window.
    // 1 = a light stagger shows the red alone, a heavy one the black tunnel with the red rim held
    // over it; 2 = every on-feet stagger shows the red alone (black only for knockdowns); 0 = as
    // before (the black tunnel, the red flash brief). Needs hit_vignette.
    s32 hitstun_vignette{1};
    // The held red's opacity: a factor on the blow's flash peak (1 = held at the flash's own
    // peak, 70 .. 100% of hit_vignette_strength by the HP it took), 0 .. 1.5.
    float hitstun_vignette_strength{1.0f};
    // Shorter hit staggers (core/vr/stagger_speed.h): the hunter's hit staggers (a000_020000 ..
    // 020399; not knockdowns, grabs or death) play faster from their start until the game frees
    // him this much sooner (every window of the clip and its end); 0 = the game's.
    float stagger_shorten_ms{150.0f}; // 0 .. 400
    // Immersive vitals (core/vr/vitals.h): 1 wrist = the Blood Gauge on the hunter's left wrist
    // (HP with the rally, stamina, bullets; drawn into the game's frame, fitted to whatever he
    // wears) and the game's HP cluster only when you look down; 0 classic = the game's HUD as
    // before. Any read failure falls back to classic (one log line).
    s32 hud_vitals{1};
    float vitals_gauge_scale{1.0f};      // the gauge's size (1 = 52.9 x 34.5 mm)
    // its centre this far up the forearm from the wrist (0.048 until 2026-10-06: requested
    // for it a tiny bit closer to the wrist)
    float vitals_gauge_from_wrist_m{0.035f};
    float vitals_gauge_standoff_m{0.003f};   // its underside this far above the measured surface
    // It is depth-tested this much nearer than it is (the sleeve or a cuff bulging past it never
    // hides it; a hand or weapon further in front still does).
    float vitals_gauge_pull_m{0.03f};
    // The low-HP safety net (hud_vitals=wrist only): below vitals_low_hp of max HP a heartbeat in
    // the left controller (faster as HP falls, quiet vitals_heartbeat_mute_s after the last HP
    // lost) ...
    bool vitals_heartbeat{true};
    float vitals_low_hp{0.30f};
    float vitals_heartbeat_mute_s{20.0f};
    float vitals_heartbeat_gain{1.0f};
    // ... and the edges of the view darkening with dried blood below 60% HP (0.18 at 30%, 0.42 near
    // 5%, clear inside 30 degrees, swelling with the heartbeat), a faint amber rim (0.05,
    // core/vr/vitals.h RallyRimAlpha) while the game's rally can win HP back.
    // vitals_edge_strength scales both.
    bool vitals_edge{true};
    float vitals_edge_strength{1.0f};
    float haptic_gain{1.0f};         // 0 = off .. 1.5
    // Controller vibration (openxr_presenter.cpp UpdateHaptics): the weakest tick or pulse (the
    // feedback patterns' amplitudes 0 .. 1 are played as haptic_floor .. 1; the continuous hums
    // and the game's own rumble are not raised) and the shortest burst in ms (a tick shorter
    // than this lasts this long): Quest Touch motors hardly show less than about 0.3 for 20 ms.
    float haptic_floor{0.3f};    // 0 .. 0.8
    float haptic_min_ms{25.0f};  // 0 .. 100
    // A test buzz the first time gameplay has the headset's focus: the left controller, then the
    // right one 0.3 s later (logged with the runtime's answers).
    bool haptic_startup_test{true};
    // Combat vibration (core/vr/haptic_output.h Haptics::Combat: the hierarchy), each 0 = none ..
    // 1.5: a melee hit's impact on the weapon hand (both with the two-handed grip; by the hit's
    // damage and weight), the small knock on both controllers when a blow hits the hunter (a
    // little stronger toward it), the light tick when one of his bullets or thrown items hits a
    // character, and the injection's THUMP. The gun's recoil is gun_recoil_gain.
    float haptic_melee_hit{1.0f};
    // A landed melee hit's full-strength part (ms, + 15 x its weight): 65 = full (1.0) for
    // 65 .. 80 ms, then a decay (about 150 .. 170 ms in all), guarded for 200 ms (no wall clank,
    // swing tick, refused-swing buzz, arc hum or rumble cuts it). 0 = the old 20 .. 30 ms crack.
    float haptic_melee_hit_ms{65.0f};
    // 1 = a heavy landed hit (a heavy attack, a two-handed one, the axe) adds a second short blow,
    // the crunch, 40 ms after the first one; 0 = one blow for every hit.
    bool haptic_melee_crunch{true};
    float haptic_hit_taken{1.0f};
    float haptic_gun_hit{1.0f};
    float haptic_throw_hit{1.0f};
    float haptic_inject{1.0f};
    // The game's own rumble for a blow on the hunter (0.8 for up to 1.6 s; moved toward the blow
    // with hit_haptics_directional, else even on both hands) x this, 0 .. 1: the hit stays a small
    // vibration that never masks the weapon hand's feedback. 1 = as the game sends it. 0.35
    // (0.28 played): the blow's rumble runs 0.3 .. 1.5 s, and at 0.5 (0.40) the swing's tick
    // (0.44) and a bullet's hit tick (0.55) were lost under it (the 2026-10-06 mock).
    float haptic_hit_rumble{0.35f};
    float hud_scale{0.3125f};        // HUD/menu panel size relative to the game image
    float hud_depth_m{2.0f};         // stereo depth of the HUD panel
    // In-game menus (core/vr/bloodborne_menu.h): while the game has a menu open (inventory,
    // notes, Yes/No boxes, shops, the Doll, the lamp) both sticks step through it, A/X confirm
    // and B/Y go back.
    bool menu_mode{true};
    // The frozen menu panel (menu_present=frozen, and the UI layer's fallback): the game image,
    // menu included at full resolution, is shown on a panel fixed in space where you looked when
    // the menu opened (placed again on every open). The game view behind it is frozen (mono, the
    // head sample of the open) and the last stereo view around it is dimmed. Without a
    // menu_present line, 0 means menu_present=hud.
    bool menu_panel{true};
    float menu_width_deg{60.0f};    // horizontal size of the panel (a 16:9 menu is ~34 deg tall)
    float menu_distance_m{1.4f};    // distance of the panel
    float menu_dim{0.35f};          // brightness of the frozen world around it (1 = not dimmed)
    bool menu_curved{true};         // a curved panel (same text size at its edges), if supported
    float menu_follow_deg{50.0f};   // looking this far off the panel for 1 s moves it (0 = never)
    float menu_dialog_zoom{1.6f};   // the game's small OK boxes this much larger (1 = off)
    // The menus (the log's `VR menu: opening '<name>'`) that are such small dialogs; zoomed only
    // when no other menu is open under them. Comma-separated.
    std::string menu_zoom_menus{"MessageBox"};
    // The VR tile in the Options grid (core/vr/bloodborne_vrtile.h): 1 (the default since
    // 2026-10-05) = a 4th tile "VR" right of System that opens the VR settings page
    // (signature-checked eboot patches, the emulator's own copies of the grid's movie and of the
    // icon atlas) and the grid cursor probe in the log; 0 = nothing patched, the game's own grid.
    int vr_menu{1};
    // Your height (core/vr/player_body.h; the VR page's General > Body, live): vr.ini height_base
    // (hunter = false, the default: standing straight your eye is the hunter's, fp_eye_height_m;
    // player = true: 1:1 with the real world, your eye over the headset's floor) and
    // height_adjust_cm (-10..+10: the eye that much higher or lower, from either). The older
    // builds' game_height_cm and player_height_cm are read and ignored.
    bool height_player{false};
    s32 height_adjust_cm{0};
    // A seated eye height over the floor (cm, by hand; an older build measured it): with seated=1
    // and a floor, the vial's lap zone sits under it. 0 = none.
    s32 seated_eye_cm{0};
    // Set on the VR settings page (core/vr/vr_page.h) or by hand: the sticks swapped (movement on
    // the right stick, core/vr/thumb_layout.h).
    bool swap_sticks{false};
    float menu_hud_scale{0.25f};
    u64 hud_after_vs{0x34f1f46b160}; // Bloodborne: vertex shader of the final composite
    // The game's UI on its own compositor layers (renderer_vulkan/vr_ui_layer.h): every UI draw is
    // also drawn into two VR-owned targets, which give the UI alone, exactly as the game blends it,
    // shown sharp over the live stereo world: menus and dialogs on a canvas fixed in space, the HUD
    // fixed to the view's lower corners and lower middle (ui_hud_anchor; or on a band that follows
    // the head lazily), lock-on markers and banners where the game puts them. 0 = off (the HUD panel in the eye images, menus on the frozen menu
    // panel), 1 = on (the default, as requested: the UI on its own layer; hud_scale then no
    // longer sizes the gameplay HUD: ui_hud_scale_deg does; when the runtime refuses the layers the
    // session falls back to 0), 2 = check only (the UI is shadowed and compared, nothing visible
    // changes; `VR ui:` statistics).
    s32 ui_layer{1};
    bool ui_compose_eyes{true}; // ui_compose=eyes|layers; native stereo, read at start
    bool overlay_compose_eyes{true}; // overlay_compose=eyes|layer
    // Where menus go: 0 layer (the UI layer's canvas over the live world, when ui_layer=1), 1
    // frozen (the menu panel over a frozen, dimmed world), 2 hud (the HUD panel at menu_hud_scale).
    // Without the key, menu_panel=0 means hud; anything else layer.
    s32 menu_present{0};
    bool menu_present_set{false};   // menu_present was in vr.ini (not saved)
    float ui_menu_distance_m{1.8f}; // menus and dialogs: the canvas's distance
    float ui_menu_width_deg{68.0f}; // full menus: the canvas's width (body text = width / 80 deg)
    float ui_dialog_em_deg{1.3f};   // small dialogs and notes: their text size at most (cropped canvas)
    // Small dialogs and notes: their box at most this wide (deg; VR::Ui::DialogDegPerPx: the game's
    // boxes are ~1000 stage px wide, so this sets their size: text 0.73-0.80 deg at 34). 0 = no
    // cap (ui_dialog_em_deg alone: 55-61 deg wide, the size before 2026-10-08).
    float ui_dialog_max_deg{34.0f};
    // Full menus cropped to their content and shown as large as fits (VR::Ui::StepMenuCrop,
    // CanvasDegPerPx): ui_dialog_em_deg text at most, never smaller than the whole menu at
    // ui_menu_width_deg, and (when set, > 0) at most ui_menu_max_width_deg wide and
    // ui_menu_max_height_deg tall. 0 = the whole 16:9 menu as before.
    bool ui_menu_fit{true};
    float ui_menu_max_width_deg{0.0f};
    float ui_menu_max_height_deg{0.0f};
    // false: once a full menu's crop is placed, a box that spans the band's whole width (a
    // screen-wide dim: the shop's quantity box, a sub-page's fade, an NPC's subtitle band as a
    // talk list closes) does not grow it (VR::Ui::StepMenuCrop); true: it does (the old way: the
    // list shrank ~8% while the quantity box was up).
    bool ui_menu_fit_screen_wide{false};
    // A page over the whole screen (Status opened from the Options grid: its book, opaque, far
    // outside the grid's crop) grows the crop even with ui_menu_fit_screen_wide=0
    // (VR::Ui::IsScreenWidePage); false: kept like a dim (the builds of 2026-10-07 to 2026-10-09:
    // Status shown cut down to the grid's 39 x 27 degrees).
    bool ui_menu_page{true};
    // A full menu's canvas takes a new crop (a sub-page that grew it, the page you went back to)
    // at once: the page appears at its size and place in one frame (VR::Ui::StepMenuCrop's
    // shrink too: once the smaller page holds still). false: the builds before 2026-10-09 (the
    // canvas eased over ~0.3 s, zooming and sliding into place; shrinking 0.6 s later).
    bool ui_menu_snap{true};
    // The headstone's travel list (WarpMenu): its canvas this many degrees below where you look
    // (+ = above). Its entries sit at the top of a tall, mostly empty panel (17-20 degrees up when
    // centred); 0 = centred like every other menu.
    float ui_travel_pitch_deg{-10.0f};
    // Every canvas (a full menu, a dialog or note, the travel list) keeps its corners within this
    // many degrees of its centre, across and up / down (VR::Ui::SharedViewDegPerPx): inside the
    // view both eyes share (Quest 3 over Link: +-40 degrees across) with a margin. Sizes above
    // give way to it (its text gets smaller rather than a corner out at the edge of one eye).
    float ui_menu_corner_yaw_deg{34.0f};
    float ui_menu_corner_pitch_deg{30.0f};
    bool ui_curved{true}; // curved canvas and HUD band (cylinders), if supported
    // The gameplay HUD: 0 belt (the status clusters and the lower group - prompts, pickups,
    // subtitles, the boss bar - as pieces fixed to the view or on a band that follows the head
    // lazily, ui_hud_anchor; VR::Ui::ComputeHudLayout), 1 game (everything on one quad where the
    // game draws it, at hud_scale: today's look, sharp).
    s32 ui_hud_layout{0};
    // What the belt is fixed to (VR::Ui::HudAnchor): 0 view (the HP / items cluster in the view's
    // lower-left corner, echoes / insight lower-right, prompts and subtitles lower middle, where
    // you look, as on the flat screen), 1 band (the lazy-follow band: ui_hud_follow_*).
    s32 ui_hud_anchor{0};
    // View anchor: the clusters' outer content edges this far left / right of the view's centre
    // and their rows this far below (-) or above (+) it.
    float ui_hud_corner_yaw_deg{30.0f};
    float ui_hud_corner_pitch_deg{-21.0f};
    float ui_hud_distance_m{2.0f};  // the HUD's distance
    // The lower group (subtitles, action prompts, item pickups) this much further than the HUD
    // (2026-10-17, the tester: NPC dialogue at 2 m felt slightly cross-eyed); its size in degrees
    // stays.
    float ui_lower_push_m{0.30f};
    float ui_hud_scale_deg{0.042f}; // degrees per 1080p screen pixel (body text 24 px = 1.0 deg)
    float ui_hud_pitch_deg{0.0f};   // moves every HUD row up (+) or down
    float ui_hud_follow_deg{28.0f};       // band anchor: it follows the head's yaw beyond this
    float ui_hud_follow_pitch_deg{15.0f}; // ... its pitch beyond this looking up
    // ... and looking down beyond this (below the band's lowest row: reading it never moves it)
    float ui_hud_follow_pitch_down_deg{32.0f};
    float ui_hud_follow_ms{300.0f}; // ... with this time constant
    // A HUD piece a hand, the arm or the held weapon is in front of fades (it is drawn over them:
    // a far layer cutting through a near hand), the HUD content itself, not its whole rect: to
    // 15% for a hand, its forearm or upper arm, the gun; to ui_hud_blade_alpha when only the
    // blade is (a right-handed guard holds it across the lower right corner most of the time).
    bool ui_hand_fade{true};
    float ui_hud_blade_alpha{0.45f};
    // The lower group (action prompts, item pickups, subtitles) never fades below this for your
    // hands, arms or body while it holds something to read (VR::IsHudTextUp; 1: no fade; 0: their
    // old 15%; the blade and the gun still fade it, ui_hud_blade_alpha; VR::Ui::HudFadeFloor).
    float ui_hud_lower_alpha{1.0f};
    // The echoes / insight counters (the HUD's right cluster) in gameplay: VR::Ui::CountersMode
    // (0 look_down: hidden, faded in while you physically look down more than
    // ui_counters_pitch_deg below the horizon; 1 always; 2 hidden). Menus, small dialogs and notes
    // show them as before.
    s32 ui_counters{0};
    float ui_counters_pitch_deg{40.0f};
    // Where they appear when you look down (VR::Ui::CountersPos): 0 lower_middle (the "tray":
    // centred below the view's centre, with the HP / stamina / items cluster over them when
    // ui_status_left shows it by looking down), 1 corner (their HUD corner), 2 raised
    // (ui_counters_raise_deg higher than their corner, right of the centre).
    s32 ui_counters_pos{0};
    // The tray: its top this many degrees below (-) the view's centre, its size x
    // ui_hud_scale_deg (1.3: text 1.3 degrees), the gap between its two clusters, and 1 = one
    // narrow column (the HP cluster over the counters) / 0 = side by side.
    float ui_counters_tray_top_deg{-14.0f};
    float ui_counters_scale{1.3f};
    float ui_counters_gap_deg{1.5f};
    bool ui_counters_tray_stack{true};
    // ui_counters_pos=raised: this much higher than their corner (degrees).
    float ui_counters_raise_deg{28.0f};
    // The HP / stamina / vials / bullets / quick item cluster (the HUD's top left, status_left)
    // in gameplay (VR::Ui::StatusLeftMode): -1 auto (look_down with hud_vitals=wrist, else
    // always), 0 look_down (with the counters, in the tray), 1 always (as the flat game), 2 hidden
    // (never in gameplay, even looking down). Menus, small dialogs and notes show it as before.
    s32 ui_status_left{-1};
    // A full menu (the Options grid, the inventory, Status, shops, Level Up): the HP, stamina and
    // item cluster and the echoes / insight shown under its canvas, fixed to it (the game draws
    // its HUD under the menu and does not pause; VR::Ui::MenuHudLayout). 0 = only the menu.
    bool ui_menu_hud{true};
    // With ui_menu_hud: the HUD the game drew before the menu is taken out of the menu's own
    // image (K and W reset in the HUD's rows right after they are kept for the HUD under the
    // canvas, VrUiLayer::SnapshotHud), so none of it shows inside a large menu's crop (the quick
    // item's name "Molotov Cocktail" next to the travel list and the Controls page). 0 = the
    // canvas keeps whatever HUD its crop reaches (the old way).
    bool ui_menu_hud_off_canvas{true};
    // Lock-on markers, enemy tags, banners (the rest quad): its scale, 0 = auto (with the belt 1,
    // lined up with the world; ui_hud_layout=game: hud_scale) ...
    float ui_rest_scale{0.0f};
    // ... and distance, 0 = auto (with the belt VR::Ui::RestDepthDefaultM, a typical combat
    // distance; ui_hud_layout=game: hud_depth_m)
    float ui_rest_depth_m{0.0f};
    // ... on a cylinder around the head (core/vr/ui_rest_curve.h; gameplay frames with the belt,
    // the runtime's cylinder layers): every enemy tag and the lock-on cursor faces you wherever it
    // is in the view; on the flat quad they turned with your head. 0 = the flat quad.
    bool ui_rest_curved{true};
    // The lock-on marker (the glow on the locked-on target; ui_layer=1): drawn this much smaller
    // about its own centre (so it stays on the target) and this much fainter (its alpha and glow),
    // on the UI layer only (the desktop mirror keeps the game's). 1 / 1 = as the game draws it.
    // Nothing else of the UI changes (VR::Ui::LockMarker, vr_ui_resolve.comp).
    float ui_lock_marker_scale{0.88f};
    float ui_lock_marker_alpha{0.82f};
    // The bars over the enemies you hit or lock on to, with their damage numbers (the HUD movie's
    // 8 EnemyTag clips, core/vr/bloodborne_enemy_bars.h); 0 = hidden, live (the VR page's Graphics >
    // Show enemy health bars), on every view of the UI. The boss bar, the lock-on dot and the
    // hunter's own HUD always show.
    bool ui_enemy_hp_bars{false};
    // The boss's health bar (the HUD movie's 3 BossTop items, core/vr/bloodborne_enemy_bars.h):
    // 1 = shown near the top of the view instead of the bottom (the default since 2026-10-18,
    // the tester: boss bars on, enemy bars off; hidden by default 2026-10-15 .. 10-18), 0 = hidden;
    // live (the VR page's Graphics > Display > Show boss health bar).
    bool ui_boss_hp_bar{true};
    // The game's button icons (menus' key guide, dialogs, messages) show the VR controllers'
    // buttons: cross A, circle B, square LG, triangle RG, L1 LT, R1 RT, OPTIONS the Menu button;
    // the gameplay HUD's action prompts by the gameplay layout: cross B, circle A, triangle X
    // (swap_sticks: Y, X, A) (core/vr/key_glyphs.h, redrawn in the game's icon atlas at load);
    // 0 = the DualShock 4's. At the start only.
    bool ui_vr_buttons{true};
    // The dark box behind the NPCs' subtitles (core/vr/gfx_talk_window.h, the HUD movie patched at
    // load): 0 = none (the words alone, the tester's 2026-10-16 choice), 1 = faint, 2 = the game's.
    // At the start only.
    int ui_dialogue_backing{0};
    // The game's status build-up meter (poison, rapid poison, bleed, frenzy, beasthood;
    // VR::Ui::Conditions, ui_layer=1 with the belt): 0 = view, a small piece of its own under the
    // view's centre (its row 0 ui_conditions_pitch_deg from it, ui_conditions_scale x the HUD's
    // size), only while the game shows a build-up; 1 = game, where the game draws it (top centre:
    // out of view in VR, the builds before 2026-10-09).
    s32 ui_conditions{0};
    float ui_conditions_pitch_deg{-6.5f};
    float ui_conditions_scale{1.0f};
    // The main player's slow poison, rapid poison, bleed and frenzy build-ups drain this many
    // percent faster than the game's own recovery (5 points / s), only while falling, never
    // beasthood, never enemies (core/vr/bloodborne_status.h); 0 = the game's rate.
    s32 status_drain_pct{10};
    // The death screen as a title (VR::Ui::StepDeathTitle): YOU DIED fixed in space where you
    // looked, this far away (0 = off: the whole screen lined up with the eyes as other banners),
    // its centre this far from the horizon (+ = up) and this wide (degrees).
    float ui_death_distance_m{4.5f};
    float ui_death_pitch_deg{-5.0f};
    float ui_death_text_deg{40.0f};
    // Every banner the game plays in the middle of the screen (VR::LatestBanner: PREY
    // SLAUGHTERED, NIGHTMARE SLAIN, the blood echoes regained, YOU DIED, ...) shown as that title
    // (where you look when it comes up, ui_death_* above); 0 = only the death screen (while the
    // hunter's HP is 0), the other banners where the game draws them. Their size: the widest
    // (PREY SLAUGHTERED) this many degrees across, every banner at that scale.
    bool ui_banner_title{true};
    float ui_banner_text_deg{50.0f};
    bool ui_veil{true};          // the game's full-screen dims and fades darken the world itself
    bool ui_backdrop_keep{false}; // UI draws that read the scene stay in the layer (else skipped)
    float ui_grey{0.5f};          // the W target's clear value
    // A UI-layer pixel no UI drew on (K and W still at their clear values to within one 8-bit
    // step) is exactly transparent (vr_ui_resolve.comp EmptyPixel, VR::Ui::Empty). 0 = the
    // grey's 8-bit rounding leaves a black alpha of 2/255 over every pixel without UI (the rest
    // quad's whole area: the view 0.8% darker; the builds before 2026-10-09).
    bool ui_empty_snap{true};
    float ui_fade_ms{120.0f};     // crossfade when a menu opens or closes
    // DIBR stereo (stereo_mode 2, renderer_vulkan/vr_stereo_warp.h).
    float dibr_knee_m{0.6f};         // near-field depth compression starts here (0 = off)
    float dibr_near_clamp_m{0.3f};   // nearest apparent distance (0 = no compression)
    s32 dibr_quality{1};             // 0 = linear resampling, 1 = Catmull-Rom (2: reserved)
    float dibr_baseline_scale{1.0f}; // multiplies the eye baseline (0 = mono control, tests)
    s32 dibr_debug_holes{0};         // 1 = disocclusion holes green, 2 = also the HUD magenta
    bool dibr_seen{false};           // the one-time stereo_mode 1 -> 2 migration ran
    bool decoupled{true};            // head rotates the view only; the orbit follows turning
    float orbit_pitch_deg{12.0f};    // camera orbit pitch while decoupled (+ = above)
    float vertical_smoothing_ms{120.0f}; // camera height smoothing (stairs, rolls); 0 = off
    // Dodge comfort camera (first person, core/vr/motion_camera.h): the view travels with the body
    // when you dodge; height changes from stairs and falls arrive smoothed. 0 = the old camera.
    bool dodge_camera{true};
    // Horizontal follow of the body (0 = rigid, 2026-10-18: the hands stay on the view while the
    // game eases him into and out of a walk; a spring here trailed the body and the arms jumped).
    float follow_smoothing_ms{0.0f};
    bool follow_rigid_seen{false}; // the one-time follow_smoothing_ms 25 -> 0 migration ran
    // The hands' tremor damped while they are nearly still (core/vr/hand_tremor_filter.h).
    bool hand_tremor_filter{true};
    // Weighted weapons, ON by default (2026-10-21). An explicit choice overrides all
    // legacy controls, independent of their order in vr.ini. Absent: the private legacy tuning
    // (weapon_weight= above 0) when vr.ini sets it, else the default, on.
    // Serialized values preserve old On=1 / Off=0 preferences; 2 selects Light.
    std::optional<int> weighted_weapons{};
    bool weapon_weight_legacy{false}; // vr.ini set weapon_weight= above 0 (private tuning)
    float WeaponWeightStrength() const {
        return weighted_weapons ? (*weighted_weapons == 2 ? .35f : *weighted_weapons ? 1.0f : 0.0f)
                                : weapon_weight_legacy ? weapon_weight : 1.0f;
    }
    bool WeightedWeaponsPreset() const { return weighted_weapons.value_or(!weapon_weight_legacy); }
    float weapon_weight{0.0f}; // legacy tuning: weapon inertia + drag, 0 off .. 1 full
    bool weapon_mesh_weight{false}; // opt-in measured geometry; requires weapon_weight > 0
    bool weapon_pose_motor{false}; // private experimental rotational motor; measured rigid shapes only
    float weapon_lift_response{0.0f}; // opt-in acceleration load; requires load_response
    float weapon_load_response{0.0f}; // opt-in bounded load compliance; requires gravity
    float weapon_gravity{0.0f}; // opt-in gravitational sag, requires weapon_weight > 0
    bool pair_lock_position{true};    // both eyes of a stereo pair use one body position
    float dodge_vertical_ms{150.0f};  // height smoothing during a dodge
    float vertical_stop_factor{0.5f}; // multiplies vertical_smoothing_ms while standing
    float fall_vertical_ms{60.0f};    // height smoothing while falling and landing
    float jump_height_scale{1.0f}; // follow the actual jump animation lift (0 = off)
    float dodge_dip_m{0.13f}; // smooth 13 cm duck during a roll (0 = off)
    // The A+B chord (core/vr/roll_chord.h, motion controls): tapped, or held while moving, the
    // game's real roll toward the stick (centred: back); held 1.2 s standing still, the weapon
    // grip adjust. 0 = A and B go straight through (the grip adjust on A+B held, as before).
    bool roll_chord{true};
    // Lean to dodge (experimental, standing, motion controls): a quick sideways or backward lean
    // of the head dodges in that direction.
    bool lean_dodge{false};
    float lean_dodge_speed{1.3f};         // m/s head speed
    float lean_dodge_dist_m{0.15f};       // head travel within 250 ms
    float lean_dodge_cooldown_ms{600.0f}; // between two lean dodges
    // Physical evasion (core/vr/bloodborne_evasion.h): your real lean, step and duck move the
    // hunter's hurtboxes (times evasion_gain), no dodge animation. Turns lean_dodge off.
    bool physical_evasion{true};
    float evasion_gain{1.3f};          // hurtbox offset per metre of lean
    float evasion_max_m{0.40f};        // horizontal lean cap (<= positional_limit_m)
    float evasion_lean_m{0.30f};       // lean range pivoting at the ankles; beyond = a step
    float evasion_duck_m{0.75f};       // counted head drop cap (<= the camera's down room)
    bool evasion_wall{true};           // the hurtboxes never go into walls (the game's own casts)
    float evasion_wall_margin_m{0.03f}; // gap they keep from a wall
    float evasion_wall_rise_mps{1.5f};  // how fast a wall limit is released (m/s)
    float evasion_hold_ms{1500.0f};    // a lean counts fully this long, then fades (0 = no limit)
    float evasion_hold_tau_ms{500.0f};
    float evasion_settle_ms{1000.0f};  // small drift held still this long settles (0 = no settle)
    float evasion_settle_tau_ms{1000.0f};
    bool evasion_pelvis{true};         // false: the pelvis capsule never moves
    bool evasion_feedback{true};       // haptics: a knock at a wall, a whoosh on a near miss
    s32 player_hp_bonus_pct{Assists::DefaultHpBonus}; // extra max HP for the hunter, %
    s32 outgoing_damage_pct{0};       // player outgoing HP damage adjustment, -30..30 percent
    s32 offhand_damage_pct{10};        // extra damage of the left-hand weapon (gun etc.), %
    // Instant ragdoll on a right-hand melee kill (core/vr/bloodborne_ragdoll.h): an enemy whose
    // death animation ends in the ragdoll goes limp the moment your blade kills it, pushed along
    // the blade's velocity at the hit x melee_ragdoll_impulse (0 = no push). 0 = the game's death
    // animation for every kill.
    bool melee_kill_ragdoll{true};
    float melee_ragdoll_impulse{1.0f};
    // The chance (%) that such a kill keeps the game's own death anyway (its death animation, the
    // game's ragdoll at its end, no push): one roll per kill (core/vr/ragdoll_kill.h RollVanilla).
    // 22 by default (about one kill in five; 15 before 2026-10-09), 0 = always at once, 100 =
    // never. Read at each kill; vr.ini only (no VR page row).
    s32 melee_ragdoll_vanilla_pct{22};
    s32 dodge_iframes_extra{Assists::DefaultDodgeFrames};
    // Lean i-frames (core/vr/bloodborne_iframes.h IframesLeanGrant): a quick sideways or backward
    // lean of the head (the lean_dodge_speed / _dist_m thresholds) makes the hunter invulnerable
    // for this many ms of game time at any frame rate (0 .. 300, 0 = off), at most once per
    // lean_iframes_cooldown_ms. No dodge is pressed and no animation plays (standing, motion
    // controls; lean_dodge=1 dodges for real instead). The tester, 2026-10-15: 100 ms, then 180 ms
    // with a 480 ms cooldown.
    s32 lean_iframes_ms{200}; // (180 until the tester's +20 ms, 2026-10-15 night)
    float lean_iframes_cooldown_ms{480.0f};
    // Attacks through dodges (core/vr/anim_classes.h GateOut::dodge_attack): a physical swing or a
    // gun shot works at any moment of a backstep, quickstep or roll (the dodge plays on; 0 = they
    // wait for the dodge's own attack window, as the game). dodge_attack_iframes=0: the dodge's
    // invulnerability ends the moment you swing or shoot (core/vr/bloodborne_iframes.h); 1 = it
    // stays (pure fun).
    bool dodge_attack{true};
    bool dodge_attack_iframes{false};
    // Cinematic flat mode: scripted full-body animations on the virtual screen.
    bool cinematic{true};
    bool cinematic_visceral{true};          // your viscerals, ripostes, backstabs
    bool vr_grab_attacks{false};            // enemy grabs stay in VR and follow the animated body
    bool cinematic_grabbed{true};           // enemy grabs
    bool cinematic_objects{true};           // doors, gates, levers, elevators, chests, coffins
    bool cinematic_lamps{true};             // lighting / using lamps
    bool cinematic_pickup{true};            // picking up items (only with pickup_instant=0)
    // Item pickups in first-person VR: granted at once with the game's pop-up and sound, without
    // the pickup animation (core/vr/bloodborne_pickup.h); false = the game's animation.
    bool pickup_instant{true};
    // A new hunter's name (core/libraries/ime/ime_dialog.cpp): the system keyboard is a desktop
    // window the headset never shows, so its name box is answered at once with a random name, as
    // if typed and confirmed (2026-10-16); 0 = the keyboard as before.
    bool auto_name{true};
    // The light sets' camera-attached fill lights (core/vr/bloodborne_lights.h): 1 = they follow
    // the hunter's heading instead of your head (no sliding highlights, but less of the game's
    // sheen when you look sideways); 0 (the default, the tester 2026-10-20: keep the original look)
    // = they follow the camera, your head, as the game makes them. Live.
    bool body_fill_lights{false};
    bool cinematic_scripted{true};          // NPC talk, scripted event animations, revival
    bool cinematic_item{true}; // a canned item use from the arc (the Shaman Bone Blade's stab)
    // The Doll's Level Up kneel (core/vr/cinematic_state.h IsLevelUpKneel): false = it stays in
    // first-person VR and Level Up is on the menu panel; true = flat (the virtual screen).
    bool cinematic_levelup{false};
    // Walking through a fog gate (core/vr/cinematic_state.h Category::FogGate): false = it stays
    // in first-person VR (2026-10-18); true = flat (the virtual screen), as before.
    bool cinematic_foggate{false};
    float cinematic_fade_ms{150.0f};        // fade to black on the way in; 0 = hard cut
    float cinematic_return_fade_ms{80.0f};  // VR fades in on the way back; -1 = the old way
    float cinematic_exit_ms{50.0f};         // signals off (or control back) this long first
    float cinematic_min_ms{600.0f};         // shortest flat period
    bool cinematic_release{true};           // back to VR at the game's cancel window
    // 0 (since 2026-10-07): a death ends a flat period at once (the death screen in VR); 1: a
    // fatal grab stays flat while HP <= 0 (until the respawn).
    bool cinematic_death_hold{false};
    float cinematic_throw_timeout_s{20.0f}; // a throw state this old is ignored
    // Ladders: the climb is flat, getting on and off stay in VR.
    bool cinematic_ladder{true};
    float cinematic_ladder_enter_delay_ms{500.0f}; // climbing after the mount before going flat
    float cinematic_ladder_min_flat_ms{1500.0f};   // shorter predicted flat periods stay in VR
    float cinematic_ladder_exit_lead_ms{450.0f};   // back to VR before the dismount (0 = at it)
    float cinematic_ladder_reenter_ms{1000.0f};    // no re-entry sooner after a return
    float cinematic_ladder_fade_ms{100.0f};        // fade of ladder transitions (0 = hard cut)
    bool cinematic_ladder_combat_exit{true};       // attack / damage / blocked returns to VR
    float cinematic_ladder_damage_pct{3.0f};       // smallest damage, % of max HP (0 = any drop)
};

Settings& GetSettings();
void LoadSettings();

void SetEnabled(bool enabled);
[[nodiscard]] bool IsEnabled();

// The kind of game menu that is open, for the UI layer (FrameTag::ui_class).
enum class UiClass : u8 {
    None = 0,   // gameplay: the HUD band and the rest quad
    Full = 1,   // a full-screen menu: the whole UI on the canvas
    Dialog = 2, // a small OK / YES-NO box: the middle of the screen on the canvas, enlarged
    Note = 3,   // a messenger's note
};
enum class MenuPresent : s32 { Layer = 0, Frozen = 1, Hud = 2 };
[[nodiscard]] const char* UiClassName(UiClass cls);

// In-game menu state (game thread: the main player's update publishes it every frame). False
// when no sample came for 0.5 s (loading, title, no player).
// `zoom`: the open menu is a small dialog the menu panel shows this much larger (1 = a menu that
// must fit the panel). `cls`: how the UI layer shows it. `pitch_deg`: its canvas this much above
// (+) or below (-) where you look (the travel list, ui_travel_pitch_deg). `grid_cell`: the open
// menu is the Options grid (IngameTop) and its cursor is on this cell (0 Inventory, 1 Stats,
// 2 System, 3 VR, ...), -1 with the cursor not known (vr_menu=0), -2 for any other menu
// (VR::Ui::GridNone ..., ui_layout.h: Status and System from the grid, at once).
void PublishGameMenu(bool open, float zoom = 1.0f, UiClass cls = UiClass::Full,
                     float pitch_deg = 0.0f, s32 grid_cell = -2);
[[nodiscard]] bool IsGameMenuOpen();
// The interactive visceral (core/vr/bloodborne_visceral_pull.h) is installed: the hunter's throws
// play in first person whatever cinematic_visceral says.
void SetVisceralFirstPerson(bool on);
// The open menu's class (None when no menu is open).
[[nodiscard]] UiClass GameMenuClass();
// The open menu's canvas pitch offset in degrees (0 when no menu is open).
[[nodiscard]] float GameMenuPitchDeg();
// The Options grid's cursor cell while it is the open menu (PublishGameMenu's `grid_cell`; -2
// when no menu or another menu is open).
[[nodiscard]] s32 GameMenuGridCell();
// GPU thread, during a frame's draws: the game menu class the frame being drawn will be tagged
// with (FrameTag::ui_class).
[[nodiscard]] UiClass DrawnFrameUiClass();
// Where menus are shown (vr.ini menu_present, menu_panel).
[[nodiscard]] MenuPresent MenuPresentMode();

// ---- UI layer (renderer_vulkan/vr_ui_layer.h) ----------------------------------------
// The presenter can show it (its compositor layers work): false at the start and after a runtime
// refused a frame with them (today's path for the rest of the session).
void SetUiLayerAvailable(bool available);
[[nodiscard]] bool UiLayerAvailable();
// GPU thread: the UI layer for the frame being drawn (0 none, 1 on, 2 check), decided at the frame
// boundary like DibrFrameActive.
[[nodiscard]] s32 UiLayerFrameMode();
// GPU thread (PrepareFrame), for an immersive UI-layer frame: whether its draws had the
// composite. A menu whose frames have none (3 in a row) goes to the frozen menu panel until it
// closes.
void NoteUiMarker(bool seen);
// An NPC conversation (Bloodborne: SprjWorldTalkMan, sampled with the menu state): `talking` = a
// line is playing, `talk_ui` = the conversation's UI is still up (an NPC subtitle, the talk input
// cooldown). IsNpcTalking: a line is playing, or the talk UI of a line from the last 30 s is up,
// or either was less than TalkGraceNs ago (the gaps between lines, the return to first person
// facing the NPC). False when no sample came for 0.5 s, and a line flag stuck for 60 s in a row
// (a flag that never clears) no longer counts.
void PublishNpcTalking(bool talking, bool talk_ui = false);
void PublishNpcSubtitle(bool shown);
[[nodiscard]] bool IsNpcSubtitleUp();
[[nodiscard]] bool IsNpcTalking();
// The gameplay UI is the HUD alone (Bloodborne: no action prompt, text window, NPC talk or item
// pop-up up; sampled with the menu state). IsPlainHud: so for the last second, no lock-on, no NPC
// talk, and a sample less than 0.5 s old. The UI layer learns the HUD's plain translucent rects
// only then: a prompt's or a subtitle's box is drawn like a menu's dim and must never count as
// the HUD's (Rasterizer::VrUiCommitHud).
// The action prompt owns its HUD region from its first game-state sample through
// its fade-out. Rendering snapshots this once into the resolved frame.
// The area name's dark strip was taken out of the HUD movie as it loaded (core/vr/gfx_area_name.h):
// the UI layer then leaves the name's corner as the game draws it (its smoke kept) instead of
// keying its dark backing out (vr_ui_resolve.comp KeyAreaName).
void PublishAreaDimRemoved(bool removed);
[[nodiscard]] bool IsAreaDimRemoved();
// A lantern warp (Rest / Travel on the VR lamp page, core/vr/bloodborne_lamps.cpp): it leaves
// through the game's warp effect in full VR, without the lamp's own cutscene, so the departure's
// fade and the arrival's queued text effects must not read as an area name or a banner.
// PublishLampWarp(false) as it starts, (true) once the new player is there.
void PublishLampWarp(bool arrived);
// Unknown/same-map transitions permit extra cleanup only under memory pressure. The lamp
// adapter may confirm a cross-map arrival from a verified source and accepted destination.
void RequestTransitionTextureCleanup(bool area_changed = false);
// One render-thread consumer: sequence in bits 1..63, coalesced area-change hint in bit 0.
[[nodiscard]] u64 TakeTransitionTextureCleanupRequest();
// The game's own lantern box ("Return to the Hunter's Dream?" YES / NO) while the VR lamp page
// answers it: never shown (it flashed up half-drawn before the page). PublishLampBoxHidden(true)
// tentatively as a lamp-type box opens (bloodborne_menu.cpp) and on every lamp-site tick that
// owns the box; false once it is not the lantern's, refused or done. Lapses 0.5 s after the last
// true, whatever happens.
void PublishLampBoxHidden(bool hidden);
[[nodiscard]] bool IsLampBoxHidden();
// Started and not arrived yet (at most 30 s).
[[nodiscard]] bool LampWarpDeparting();
// Arrived less than `seconds` ago.
[[nodiscard]] bool LampWarpArrivedWithin(float seconds);
// Any load's arrival (the game's start, a lamp warp, a respawn): a new main player, noted by the
// main player's update (bloodborne_heal.cpp OnMainPlayerUpdate). The text effects the game queues
// as it arrives are no banners either.
void NotePlayerArrival();
// A new main player less than `seconds` ago.
[[nodiscard]] bool PlayerArrivedWithin(float seconds);
void PublishItemPickup(bool shown);
[[nodiscard]] bool IsItemPickupUp();
void PublishActionPrompt(bool shown);
[[nodiscard]] bool IsActionPromptUp();
void PublishPlainHud(bool plain);
[[nodiscard]] bool IsPlainHud();
// The game shows something to read in the HUD's lower group, or did less than a second ago: an
// action prompt, a text window (event text, an NPC subtitle), an NPC line or the talk cooldown,
// an item pop-up or a menu (a PublishPlainHud(false) sample); also when that is not known (no
// sample for 0.5 s: the menu detection is off, or the game is not updating it). Your hands, arms
// and body fade the lower group only when not (VR::Ui::HudFadeFloor `reading`): the boss bar
// alone fades as it always did. HudTextUp: the rule on PublishPlainHud's stamps (steady ns; unit
// tested); IsHudTextUp: now.
constexpr bool HudTextUp(s64 now_ns, s64 sample_ns, s64 busy_ns) {
    if (sample_ns == 0 || now_ns - sample_ns >= 500'000'000) {
        return true;
    }
    return busy_ns != 0 && now_ns - busy_ns <= 1'000'000'000;
}
[[nodiscard]] bool IsHudTextUp();
// A banner the game started in the middle of the screen (Bloodborne: a text effect the HUD played,
// sampled with the menu state: PREY SLAUGHTERED, YOU DIED, the blood echoes regained, ...).
// `type`: the game's text-effect type (0 = not known). LatestBanner: the newest one (serial +1 per
// banner, 0 = none yet) and when it started (steady ns, NowNs).
struct BannerEvent {
    u64 serial{0};
    u32 type{0};
    s64 ns{0};
};
void PublishBanner(u32 type);
[[nodiscard]] BannerEvent LatestBanner();
// The frozen menu panel is up: a game menu is open and menus go to it (menu_present=frozen, or the
// UI layer cannot show them: off, unavailable, or a menu without the composite), or one closed
// less than 150 ms ago (a menu that chains into the next keeps the panel and the frozen view).
// Game frames are then not immersive (the HUD and menus draw at full size), the camera tick keeps
// the head sample of the open (mono), and the presenter shows the frames on the menu panel
// (FrameTag::menu). Never on the virtual screen (display mode Screen, a cinematic's flat view):
// menus stay on it.
[[nodiscard]] bool MenuPanelActive();
// The menu panel is placed at the head's yaw and its pitch clamped to this range (a note on the
// ground puts it below eye level); the frozen view behind it looks the same way.
constexpr float MenuPanelPitchMinDeg = -30.0f;
constexpr float MenuPanelPitchMaxDeg = 15.0f;
// The menu panel's horizontal size in degrees: menu_width_deg, times menu_dialog_zoom while the
// open menu is a small dialog in the middle of the screen (its corners only hold the HUD).
[[nodiscard]] float MenuPanelWidthDeg();
// Presenter (frame thread): where the menu panel faces (tracking space, yaw + = left, pitch + =
// up), every frame while it is shown; `shown` false once it is gone. The frozen view looks the same
// way, so the game image on the panel lines up with the world around it (exactly on the flat
// panel, within about 2 deg on the curved one), also after the panel moved to where you look
// (menu_follow_deg). The aim is in the current tracking space (ShiftTrackingSpace): the camera
// tick only takes it with a head sample of the same space (a recenter re-places the panel).
void SetMenuPanelAim(bool shown, float yaw = 0.0f, float pitch = 0.0f);

// Debug: drives a synthetic head instead of a headset (no OpenXR needed).
void SetSimulatedHead(bool enabled);
[[nodiscard]] bool IsSimulatedHead();

// ---- OpenXR thread ---------------------------------------------------------------
void PublishHead(const HeadState& state);
[[nodiscard]] HeadState GetHead();
// Opt-in A2 tracking-loss candidate; headset acceptance is still required.
[[nodiscard]] bool KeepEyeFov();
// Seconds since the first driven gameplay camera tick; -1 before gameplay.
[[nodiscard]] double GameplaySeconds();

// ---- Input -----------------------------------------------------------------------
void AddTurn(float radians); // + = turn right
// The arms' pose copy (core/vr/bloodborne_body.cpp Gather), before this game frame's camera
// update: the turn queued so far is claimed for this frame (`claim`; a stereo pair's second eye
// takes no new one), and the camera update applies exactly the claimed turn, so the arms are
// placed with the heading the camera will have (2026-10-18: the arms were a turn behind for the
// frame of every snap turn, a frame's turn behind all through a smooth one). Returns the claimed
// turn not yet applied (radians, + = right).
float ClaimTurnForArms(bool claim);
// Tab on the keyboard (sdl_window.cpp), or the Menu hold with menu_hold=screen: toggles the virtual
// screen. While a cinematic is going flat or flat (FadeOut, Flat) it is the escape hatch instead:
// first-person VR comes back at once and the rest of that animation (on a ladder: that climb)
// stays in VR; the display mode and the controls are left as they are. `why`: what asked for it
// (the log).
void ToggleScreenMode(const char* why = "Tab");
void SetDisplayMode(DisplayMode mode);
[[nodiscard]] DisplayMode GetDisplayMode();

// ---- Stereo mode -----------------------------------------------------------------
struct StereoConfig {
    bool stereo{true}; // false = mono
    s32 mode{2};       // stereo_mode: 2 = DIBR, 1 = per-eye camera (AER), 0 = clip-space shift
};
// The stereo mode in effect. Both values are read as one word, so no thread ever sees a new
// `stereo` with an old `stereo_mode`. Starts from vr.ini.
[[nodiscard]] StereoConfig GetStereo();
// Live switch (input thread): steps through the live modes, 0 = mono, 1 = AER (stereo_mode 1) and
// 2 = DIBR (stereo_mode 2), in `direction` (+1 / -1). The camera tick applies it between eye pairs,
// never inside one. Returns the new live mode.
s32 CycleStereoMode(s32 direction);
[[nodiscard]] const char* StereoModeName(s32 live_mode);

// ---- Guest camera hook (game thread) ---------------------------------------------
struct CameraOverride {
    bool active{false};
    float pitch{};      // orbit pitch, game convention: radians, + = down
    float yaw{};        // orbit yaw, game convention: radians, + = turned right
    // The view's pitch (game convention, + = down): your head's (decoupled from the orbit). What
    // follows your head uses it (the first-person eye's lead, the near clip, the VR body).
    float view_pitch{};
    // The render camera's pitch (+ = down): view_pitch plus render_pitch_bias_deg while the bias
    // applies (core/vr/render_tilt.h), else view_pitch. Only the render camera's orientation.
    float render_pitch{};
    float view_yaw{};
    // Render camera roll about its forward axis (radians, + = the camera's right side up): the
    // head's own roll, so the stereo baseline is the line between your eyes (camera_roll).
    float view_roll{};
    bool decoupled{false};
    bool orbit{false}; // write the orbit pitch/yaw (false: the game drives it, e.g. lock-on)
    float vfov{};      // radians, vertical
    Vec3 render_offset{}; // added to the render camera position only (eye + head movement),
                          // game world axes and units
    // The part of the render camera position that holds your place through a freehand throw and
    // keeps you off its victim (game world axes and units; bloodborne_camera.cpp,
    // GrabCamera::ThrowCamera). The whole-body rig follows it, separately from leaning.
    Vec3 throw_offset{};
    Vec3 throw_root{}; // presented root from the same pose decision as the camera
    bool throw_shared{};
    float throw_weight{}; // the hold/recovery blend shared by the whole hunter
    bool throw_holding{}; // lock the presented root through native paired positioning
    // The camera record of this update (SetRecordRenderPosition).
    u64 record_id{};
    // False on the second eye of a stereo pair: the head sample and the state that changes the
    // whole view (turns, the lock-on heading, recenter, camera height) only advance on the first
    // eye, so both eyes of a pair share them.
    bool pair_first{true};
    // The VR body's anchor (core/vr/bloodborne_body.cpp): the stereo part of render_offset, the
    // head position the camera was placed for (tracking space), the tracking -> game mapping
    // (game heading of tracking -Z, game units per metre) and the stereo eye of this update.
    Vec3 eye_offset{};
    Vec3 head_position{};
    float base_yaw{};
    float world_scale{1.0f};
    s32 eye{-1}; // stereo_mode 1 eye (-1 = mono or stereo_mode 0)
    bool aer{false};
    // Physical evasion: the neck pivot's offset from its recentre origin (game world axes, metres,
    // before world_scale; turning or nodding the head does not move it). Valid while 6DoF drives
    // the camera.
    bool neck_valid{false};
    bool head_tracked{false};
    Vec3 neck_offset{};
};
// Called once per rendered game frame. `game_orbit`: the game steers the camera itself
// (lock-on, or a camera mode we do not drive such as the first-person aim camera); the view
// then follows the game's heading with the head on top, and the right stick goes to the game.
// `game_aspect`: the render camera's aspect ratio (0 = unknown: the frame's W/H is used).
// `render_camera`: called from the render-camera hook, which writes the result's render_pitch
// into the render camera (render_pitch_bias_deg applies only then). `game_anchor`: the follow
// camera's pivot on the hunter (world, game units; nullptr unknown): after a gap in the updates
// the view resumes only if the game camera stayed put (core/vr/camera_gap.h).
CameraOverride OnCameraTick(float game_pitch, float game_yaw, float game_vfov, bool lock_on,
                            bool game_orbit = false, float game_aspect = 0.0f,
                            bool render_camera = false, const Vec3* game_anchor = nullptr);
// The render-camera hook's final position for camera record `id` (game units).
void SetRecordRenderPosition(u64 id, const Vec3& position);
// Presenter: eligibility plus consumption of a fresh camera-relative walking stick (x right,
// z forward). False cancels immediately. Never call with true when other controls own movement.
[[nodiscard]] Vec3 RoomscaleWalkInput(bool allowed, const char* why = "");
// Render hook: after comfort smoothing, before writing the eye. Consumes only actual horizontal
// base travel and updates the tracking origins, this result and its late-warp record together.
void UpdateRoomscaleWalk(CameraOverride& camera, const Vec3& base, bool first_person);
// The render-camera hook: the first-person eye's distance in front of the head centre this update
// was placed with (CameraRecord::eye_forward_m), and whether it placed the first-person eye at all
// (CameraRecord::first_person).
void SetRecordEyeForward(u64 id, float forward_m, bool first_person);
// The render-camera hook's projection for camera record `id`: the vertical FOV (radians), aspect,
// near and far plane (game units) the game renders this update with.
void SetRecordProjection(u64 id, float vfov, float aspect, float near_z, float far_z);
// The render-camera hook's held-vial depth proxy for camera record `id` (CameraRecord::held_*).
void SetRecordHeldProxy(u64 id, const Vec3& a, const Vec3& b, float radius);
// The render-camera hook (the VR body's anchor): the camera's dip under the nominal eye height over
// the hunter's feet for camera record `id` (CameraRecord::floor_dip_m, metres).
void SetRecordFloorDip(u64 id, float dip_m);
// The render-camera hook (the VR body's anchor): the hunter's model origin for camera record `id`
// (CameraRecord::player_position, game units).
void SetRecordPlayerPosition(u64 id, const Vec3& position);

// ---- Flip association ------------------------------------------------------------
// Guest thread, when the game submits a flip (sceVideoOutSubmitFlip, sceVideoOutSubmitEopFlip,
// which sceGnmSubmitAndFlipCommandBuffers goes through): queues the newest camera record for the
// frame being flipped. Returns the flip's ticket for OnGpuFrameBoundary (0 = VR off).
[[nodiscard]] u64 OnFlipSubmitted();

// ---- GPU command thread ----------------------------------------------------------
// At the flip of the frame whose draws just completed: returns its tag (the camera record its
// first draw used; without draws the one queued with `flip`, the latest record when there is
// none) and advances the eye.
// finish_rendering drains the draw worker, recorder and GPU; called only for an AA change.
FrameTag OnGpuFrameBoundary(u64 flip, void (*finish_rendering)());
Aa::Mode GetActiveAaMode();
Aa::Mode GetRequestedAaMode();
// Per-draw stereo parameters for the vertex shader (see ApplyStereoShift).
struct DrawStereo {
    float shift{}; // clip-space x shift for perspective geometry
    float hud{};   // HUD panel transform, 0 = none
};
// vertex_shader_hash identifies the game's final scene composite; every draw after it
// in the same frame is HUD or menu and is turned into a panel.
[[nodiscard]] DrawStereo StereoForDraw(u32 target_width, u32 target_height, bool has_depth,
                                       u64 vertex_shader_hash);
void SetMainAspect(float aspect);
[[nodiscard]] float MainAspect(); // width / height of the flipped game frames
// True while the frame being drawn is an immersive stereo_mode 2 (DIBR) frame: the rasterizer
// captures its scene colour and depth at the composite.
[[nodiscard]] bool DibrFrameActive();
// aa_mode=taa: the sub-pixel jitter of the frame being drawn (set at its start, the same for all
// its scene draws; StereoForDraw encodes it, the resolve undoes it). `on` false: unjittered.
struct TaaJitter {
    bool on{false};
    float ndc_x{0.0f}, ndc_y{0.0f}; // clip-space offset per w (NDC)
    float px_x{0.0f}, px_y{0.0f};   // the same in render pixels (+y = up in NDC)
    u32 phase{0};
    u32 width{0}, height{0}; // the render size the NDC offset was made for
};
[[nodiscard]] TaaJitter FrameTaaJitter();
// The render TAA's resolve ran (or refused) for the frame being drawn at this render size: the
// next frames jitter only while resolves keep coming (a jittered frame is never left unresolved
// for long).
void NoteTaaResolve(u32 width, u32 height, bool resolved);
// The camera record of the frame being drawn (the same one StereoForDraw and the frame tag use);
// when its projection is not known yet, the newest record that has one.
[[nodiscard]] CameraRecord DrawFrameRecord();
// A queued draw owns its camera inputs. Null leaves ordinary serial selection unchanged.
class DrawCameraScope {
public:
    explicit DrawCameraScope(const CameraRecord* record);
    ~DrawCameraScope();
    DrawCameraScope(const DrawCameraScope&) = delete;
    DrawCameraScope& operator=(const DrawCameraScope&) = delete;

private:
    const CameraRecord* previous;
};
// The record a DrawCameraScope installed on this thread (null = none): what DrawFrameRecord
// returns then, read in place without record_mutex or a copy (fast toggle bit 36).
[[nodiscard]] const CameraRecord* DrawCameraOverride();
// DrawFrameRecord's id at the frame's first draw (0 = no draw yet): the update the frame is built
// with and tagged with (FrameTag::record).
[[nodiscard]] u64 FirstDrawRecord();
// The rasterizer found the camera update the frame's draws really carry in their constants
// (Rasterizer::VrMatchCamera): the frame is built with and tagged with it, not FirstDrawRecord's
// (GPU command processor thread, before the frame's flip).
void SetFrameRecord(u64 id);
// Test switch VR_TAG_SOURCE=flip: frames are tagged with the flip's camera update (the newest when
// the game submitted the flip) instead of their first draw's, for before/after measurements.
[[nodiscard]] bool FlipTagTest();
// Camera record `id` while it is still in the record ring (the last 64 camera updates).
[[nodiscard]] bool GetRecord(u64 id, CameraRecord& record);
// The newest camera update's head yaw (radians, tracking space, + = turned left) while the
// headset drives the camera; false otherwise (menus, cinematics, the virtual screen).
[[nodiscard]] bool NewestHeadYaw(float& yaw);
// Latest camera tick, without draw-frame lag; used to wait for a blink turn's new eyes.
[[nodiscard]] bool NewestCameraRecord(CameraRecord& record);
[[nodiscard]] bool DebugDrawLog(); // true for a few frames when VR_DEBUG_DRAWS is set
// Debug (VR_LAG_TEST): the camera alternates heading per record; see the presenter overlay.
// VR_LAG_TEST=2 also puts that heading into the record's head yaw, so a frame submitted with the
// record it was rendered with stays still in the compositor and one sent with a neighbour jumps
// 80 degrees (the mock runtime's MOCKXR_ASSOC check).
[[nodiscard]] bool LagTestEnabled();
[[nodiscard]] s32 LagTestMode(); // 0 = off, 1 or 2

// ---- Presenter / status ----------------------------------------------------------
[[nodiscard]] bool IsCameraActive();
// Frame-based: true while gameplay frames are being rendered immersively (robust to hitches).
[[nodiscard]] bool IsImmersive();
// True when the game itself steers the camera (lock-on, virtual screen, cutscenes); the
// right stick then goes to the game instead of turning.
[[nodiscard]] bool GameOwnsCamera();
// The presenter publishes, the VR page's controls view reads: y_menu in effect (Settings::y_menu
// resolved against the runtime: Y is the menu button) and whether the runtime owns the left Menu
// button (SteamVR: its dashboard).
void SetYMenu(bool y_menu, bool menu_runtime);
[[nodiscard]] bool YMenuActive();
[[nodiscard]] bool MenuOwnedByRuntime();
[[nodiscard]] float CurrentVfov();// radians, the FOV the game camera is being driven with
// Seconds per game frame (smoothed camera-update interval; 1/60 until known).
[[nodiscard]] float GameFrameSeconds();
// Your height (core/vr/player_body.h, the page's Height and Adjust height): the camera's standing
// eye over the hunter's feet minus fp_eye_height_m (metres, before world_scale; + = higher) as the
// newest camera tick placed it (before the first, As the hunter's: Adjust height). Exactly 0 for As
// the hunter + 0 cm. The camera's floor guard, evasion's duck cap and the late warp's soles use it.
[[nodiscard]] float HeightEyeOffsetM();
// The body's offset (player_body.h BodyOffsetM): HeightEyeOffsetM, except As the player your head
// above the 6DoF origin adds to it (standing up after a recenter taken bent fits him to your
// standing eye again). His stance and arms, the vial zones, the belt and evasion's counted lean.
[[nodiscard]] float HeightBodyOffsetM();
// eye_taa: the late warp's eyes' temporal AA blended a history on its last rendered warp (the
// presenter sets it every warp; with eye_taa_follow also while the game camera walks or turns).
// The camera's sub-pixel jitter (eye_taa_jitter) runs only then: after a reset the history starts
// again, and a jitter would only add shimmer.
void SetEyeTaaAccumulating(bool on);
[[nodiscard]] bool EyeTaaAccumulating();
// The late warp shows the DIBR eyes (presenter: true after every warp it renders, false once it
// fails or is off for the session; a frame it skips for its content, a menu panel or a missing
// head pose leaves it). render_pitch_bias_deg applies only while it is true: the late warp is what
// undoes the bias.
void SetLateWarpAvailable(bool on);
[[nodiscard]] bool LateWarpAvailable();
// Eye distance (meters, before world_scale) used for the game cameras.
[[nodiscard]] float EffectiveIpd(const HeadState& head);
// Live eye-distance adjustment from the controllers: +/- millimeters, 0 = back to the
// headset's IPD. Saved to vr.ini. Returns the new eye distance in millimeters.
float AdjustIpd(float delta_mm);
// Rewrites (or appends) one key=value line in vr.ini, keeping the rest of the file (comments,
// unknown keys, the order; core/vr/vr_ini.h). Queued: a worker thread writes it 0.5 s after the
// last change (or at FlushSettings), to <vr.ini>.tmp and then renames it over vr.ini, under a lock
// file shared with other emulator instances. Any thread.
void SaveSetting(const std::string& key, const std::string& value);
// Takes `key` out of vr.ini (queued like SaveSetting): its code default applies at the next start.
void RemoveSetting(const std::string& key);
// Writes the queued SaveSetting / RemoveSetting edits now (on the worker; returns at once).
void FlushSettings();
// Main-thread restart path: do not exit unless the accepted graphics record reached disk.
bool SaveGraphicsForRestart();
// Applies one vr.ini line to the live settings, exactly as the loader parses it (the VR settings
// page: a change applies at once). False (logged) when the value is refused.
bool ApplySetting(const std::string& key, const std::string& value);
// The vr.ini file in use (the user directory's, or VR_INI's).
[[nodiscard]] std::string SettingsFilePath();
// The current head position becomes the neutral position for 6DoF movement (recenter).
void RecenterPosition();
// Presenter (OpenXR frame thread), before it publishes the first head sample located in a new
// tracking space (a recenter: the app's own, Menu held, or the runtime's): `shift` is the new space
// in the old one (core/vr/space_shift.h). Every HeadState published from now on carries the new
// serial; the camera tick that first sees it carries the view over (the game heading of tracking
// forward turns with the space, the 6DoF and neck origins and the frozen head samples are moved
// into it, so nothing the game shows jumps) and then takes the 6DoF origin from the live head
// (RecenterPosition; during a menu once it closes). An unknown shift only recenters the position.
// Returns the new serial.
u32 ShiftTrackingSpace(const Space::Shift& shift);
// The shift from tracking space `from` to the later space `to` (identity when equal): something
// placed in `from` (a camera record's head pose, a held hand) moved into `to`. False, with an
// unknown shift, when a change between them is unknown (a runtime recenter without its pose) or
// too old (more than 32 changes ago). Any thread.
bool SpaceShiftBetween(u32 from, u32 to, Space::Shift& shift);
// Presenter: whether the camera tick has carried the view over into space `serial` (or a later
// one), and the first camera record placed in that space (frames from it on are in the new space).
[[nodiscard]] bool SpaceShiftApplied(u32 serial, u64& first_record);
// True while the game camera is locked on to a target.
[[nodiscard]] bool IsLockedOn();
// Game rumble (scePadSetVibration), forwarded to the controllers as haptics.
void SetPadRumble(u8 small_motor, u8 large_motor);
// Frame cap (vr.ini frame_cap): the emulated vblank follows the headset. The game waits for the
// vblank to flip, so it renders at most one frame per vblank: one vblank every K headset refreshes
// paces it at refresh / K fps, in step with the display (no drift through it).
// OpenXR frame thread, after every xrEndFrame: K (0 = not pacing: the vblank timer runs free) and
// the display refreshes since the last call (the XR frame's display slot step: 1 normally, 2+
// when the loop missed a refresh or the runtime throttles it, 0 for a second frame on the same
// refresh). A vblank comes every K refreshes, on the same refreshes whatever the loop missed.
void PaceVblank(u32 refreshes_per_vblank, u32 refreshes_elapsed = 1);
// Video out present thread, once per vblank: waits for the next paced vblank. False when not
// pacing, or when none came within `timeout_ns` (the headset loop stalled): the caller's own timer
// is the vblank then.
[[nodiscard]] bool WaitPacedVblank(s64 timeout_ns);
// The refreshes per vblank PaceVblank set (0 = not pacing).
[[nodiscard]] u32 PacedVblankRefreshes();
void GetPadRumble(float& small_motor, float& large_motor); // 0..1
// Set by the game integration when a first-person camera mod is detected.
void SetFirstPersonDetected(bool detected);
[[nodiscard]] bool IsFirstPerson();
// First person: how far (m) the camera is placed ahead of the hunter's eyes for a view pitch
// (radians, + = down): fp_ahead_m + fp_ahead_down_m ramped in from 25 to 60 deg down
// (VR_TUNE_FILE fp.ahead_m / fp.ahead_down_m: live A/B). The camera adds it to fp_forward_m; the
// VR body keeps the hunter's eyes that much behind the camera.
[[nodiscard]] float FirstPersonAheadM(float view_pitch);

// ---- Instant item use (Bloodborne: Blood Vial self-injection) ------------------------
// The OpenXR input thread asks for one injection; the player update hook in
// core/vr/bloodborne_heal.cpp (site A, usually a job worker thread, not the loop thread) takes the
// request during the main player's update, runs the game's own item use and answers. A request is
// owned by whichever side wins the hand-off (taken by the player update, or expired by the input
// thread), so one gesture can never heal twice, and a request the game does not pick up quickly
// (loading, hitch) never fires late.
enum class InjectResult : u32 {
    Pending = 0, // not answered yet
    Healed,      // the game's item use ran (one vial consumed, heal applied by the game)
    DryRun,      // VR_HEAL_DRYRUN: every check passed, nothing was used
    FullHp,      // denied: HP is full (inject_full_hp=0)
    NoVials,     // denied: no Blood Vials carried
    Dead,        // denied
    Blocked,     // denied: a status effect or the character type forbids item use
    NoPlayer,    // denied: no main player data
    Expired,     // the player update did not take the request in time
    Busy,        // another request is still outstanding, or another item is being used
    Disabled,    // the hook disabled itself (sanity check failed); later gestures fall back
};
[[nodiscard]] const char* InjectResultName(InjectResult result);
// Input thread: queues one injection. False while another request is outstanding.
[[nodiscard]] bool RequestInject();
// Input thread: the answer to the last request (Pending until there is one). A request the
// player update has not taken within the timeout is expired here and answered with Expired.
[[nodiscard]] InjectResult PollInjectResult();
// True while the instant-injection hook is installed and has not disabled itself. When false
// a stab is refused (never the game's own vial with its animation; inject=0: X is that button).
[[nodiscard]] bool InstantInjectAvailable();
// The main player's update (heal hook, site A: usually a job worker thread).
void SetInstantInjectAvailable(bool available);
// The main player's update ran (hook liveness for the injection, freshness of the cinematic
// phase).
void NotePlayerUpdate();
// When it last ran (steady_clock ns; 0: never). A cutscene that stops the player's updates shows
// as this standing still (bloodborne_camera.cpp RemoCutscene). Any thread.
[[nodiscard]] s64 LastPlayerUpdateNs();
// A remo cutscene plays (bloodborne_camera.cpp RemoCutscene; the game's camera): its frames are
// not first person (the head, the cape and the body are drawn). Any thread.
void SetRemoCutscene(bool playing);
[[nodiscard]] bool RemoCutscenePlaying();
// Takes the pending request; `expired` = it is older than the timeout (answer Expired).
[[nodiscard]] bool TakeInjectRequest(bool& expired);
void FinishInjectRequest(InjectResult result);

// ---- Heal feedback (the heal vignette) ------------------------------------------------
// The player update hook (core/vr/bloodborne_heal.cpp) counts the heals an item gave (HP that
// rises while the player uses an item, or with the Blood Vial's heal effect); the presenter shows
// the heal vignette once per new count.
void NoteItemHeal();
[[nodiscard]] u32 ItemHealCount();

// ---- Hit feedback (the hit vignette) -----------------------------------------------------
// The player update hook counts the hits that took HP (an HP drop right after the game's damage
// calculation hit the main player), with the share of max HP the last one took (0 .. 1); the
// presenter flashes the hit vignette once per new count.
void NoteHitTaken(float fraction_of_max_hp);
[[nodiscard]] u32 HitTakenCount();
[[nodiscard]] float LastHitFraction();

// ---- Held items (Bloodborne: the armed vial in the hunter's own hand) -------------------------
// vial_in_hand=game: the presenter (input thread) publishes what the hunter's left hand should
// hold every input frame (VR::Held::PackRequest, core/vr/held_item.h); the main player's update
// (core/vr/bloodborne_held.cpp) shows the game's own vial there and publishes its status; the VR
// overlay draws its own vial only while the game's is not available (failed, not installed, or no
// status for 250 ms).
void PublishHeldRequest(u64 word);
[[nodiscard]] u64 LatestHeldRequest();
// The injection's drain (presenter -> the in-frame renderer): VR::Held::PackDrain's word
// (core/vr/vial_drain.h; 0 = none).
void PublishVialDrain(u64 word);
[[nodiscard]] u64 LatestVialDrain();
struct HeldStatus {
    s64 steady_ns{0};       // when the main player's update published it
    u8 state{0};            // VR::Held::State
    bool left_hidden{false}; // the left weapon was hidden this update
    s32 slot{-1};           // the effect slot in use
    const char* reason{""}; // why it is not shown (static text)
};
// The main player's update (one writer).
void PublishHeldStatus(const HeldStatus& status);
// False before the first status.
[[nodiscard]] bool LatestHeldStatus(HeldStatus& out);

// ---- Cinematic flat mode (Bloodborne) ----------------------------------------------
// Scripted full-body animations (viscerals, grabs, doors, levers, lamps, ...) and long ladder
// climbs show the game's own third-person camera on the virtual screen, with a fade to black on
// both sides; first-person VR resumes when they end, re-synced to the head. The signals are read
// in the main player's update (core/vr/bloodborne_cinematic.cpp) and decided by
// Cinematic::Machine (core/vr/cinematic_state.h). The gameplay controls stay as they are
// throughout (no menu layout, held buttons are kept).
// Main-player update (site A, usually a job worker thread): steps the state machine and
// publishes the phase atomically for the camera, presenter and input threads. Returns the step
// for the log.
Cinematic::Step UpdateCinematic(const Cinematic::Signals& signals);
// The game's camera is shown (Flat, FadeBack): the camera tick does not drive the camera.
[[nodiscard]] bool CinematicFlat();
// Any phase but Off, with a main-player update within the last 5 s (a hitch keeps the phase;
// loading ends it).
[[nodiscard]] bool CinematicActive();
// Presenter frame thread (one caller): brightness of the submitted layer, 0..1 (1 outside
// cinematics and on the screen chosen by hand); continuous when a new signal interrupts a fade.
// `layer_switch_ns`: steady-clock time the shown content last switched between the projection
// and the virtual screen.
[[nodiscard]] float CinematicBrightness(bool content_immersive, s64 layer_switch_ns);
// The phase in effect (Off once the player updates stop) and how long it has lasted, for logs.
[[nodiscard]] Cinematic::Phase CinematicPhase(s64* age_ms = nullptr);
// Presenter frame thread: the shown content switched to first-person VR (`immersive`, at the
// steady-clock time `switch_ns`) or to the virtual screen. The way back of a flat period ends
// once VR has been up for its fade (the gameplay controls come back then).
void NoteCinematicLayer(bool immersive, s64 switch_ns);

// Math helpers shared with the presenter.
Quat QuatFromYawPitch(float yaw, float pitch);
// q_yaw * q_pitch * q_roll: the roll turns about the view axis, + = the right side up.
Quat QuatFromYawPitchRoll(float yaw, float pitch, float roll);
Vec3 Rotate(const Quat& q, const Vec3& v);
void YawPitchFromQuat(const Quat& q, float& yaw, float& pitch);
// The roll of `q` about its own view axis against the level frame QuatFromYawPitch(yaw, pitch) of
// the same view direction (YawPitchFromQuat's yaw and pitch, unclamped).
float RollFromQuat(const Quat& q, float yaw, float pitch);

} // namespace VR
