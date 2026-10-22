// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>
#include <Zydis/Zydis.h>
#include <xbyak/xbyak.h>

#include "common/arch.h"
#include "common/logging/log.h"
#include "common/thread.h"
#include "core/file_sys/file_override.h"
#include "core/vr/bloodborne_body.h"
#include "core/vr/bloodborne_camera.h"
#include "core/vr/body_channel.h"
#include "core/vr/grab_camera.h"
#include "core/vr/root_height_hold.h"
#include "core/vr/bloodborne_enemy_bars.h"
#include "core/vr/bloodborne_lights.h"
#include "core/vr/bloodborne_evasion.h"
#include "core/vr/bloodborne_facing.h"
#include "core/vr/bloodborne_gun.h"
#include "core/vr/bloodborne_melee.h"
#include "core/vr/bloodborne_heal.h"
#include "core/vr/bloodborne_held.h"
#include "core/vr/bloodborne_quickitems.h"
#include "core/vr/bloodborne_iframes.h"
#include "core/vr/bloodborne_shoulder.h"
#include "core/vr/bloodborne_stagger.h"
#include "core/vr/bloodborne_status.h"
#include "core/vr/bloodborne_menu.h"
#include "core/vr/bloodborne_lamps.h"
#include "core/vr/bloodborne_motion.h"
#include "core/vr/bloodborne_levers.h"
#include "core/vr/bloodborne_pickup.h"
#include "core/vr/bloodborne_ragdoll.h"
#include "core/vr/bloodborne_pose.h"
#include "core/vr/bloodborne_transform.h"
#include "core/vr/bloodborne_tuning.h"
#include "core/vr/bloodborne_visceral_pull.h"
#include "core/vr/bloodborne_vitals.h"
#include "core/vr/bloodborne_whip.h"
#include "core/vr/camera_shake.h"
#include "core/vr/hook_util.h"
#include "core/vr/bloodborne_visceral_stage.h"
#include "core/vr/visceral_stage.h"
#include "core/vr/inframe.h"
#include "core/vr/live_tune.h"
#include "core/vr/quick_settings.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

using Hook::EmitHostCallStub;
using Hook::MakeWritable;
using Hook::WriteJump;

// Module-relative addresses in the 1.09 eboot (patch file address - 0x400000).
// Camera update: the instruction that loads the final pitch right before the camera
// orientation is built. Both the smoothed and the unsmoothed paths reach it.
constexpr u64 CameraHookRva = 0x143CF2E;
constexpr std::array<u8, 17> CameraHookSig = {
    0xC4, 0xC1, 0x7A, 0x10, 0x85, 0x40, 0x01, 0x00, 0x00, // vmovss xmm0, [r13+0x140]
    0xC5, 0xFA, 0x11, 0x85, 0x10, 0xFD, 0xFF, 0xFF,       // vmovss [rbp-0x2F0], xmm0
};
// FOV smoothing: xmm1 holds the target FOV (radians) before it is blended into +0x50.
constexpr u64 FovHookRva = 0x143AF5E;
constexpr std::array<u8, 24> FovHookSig = {
    0xC4, 0xC1, 0x7A, 0x10, 0x55, 0x50, // vmovss xmm2, [r13+0x50]
    0xC5, 0xF2, 0x5C, 0xCA,             // vsubss xmm1, xmm1, xmm2
    0xC5, 0xF2, 0x59, 0xCC,             // vmulss xmm1, xmm1, xmm4
    0xC5, 0xEA, 0x58, 0xC9,             // vaddss xmm1, xmm2, xmm1
    0xC4, 0xC1, 0x7A, 0x11, 0x4D, 0x50, // vmovss [r13+0x50], xmm1
};
constexpr size_t FovHookLen = 14;
// The two call sites of the camera update; right after the call the manager copies the
// camera's world matrix (+0x10..+0x4F) for rendering. The render orientation is replaced
// there, so the view follows the head exactly while the position keeps the game's orbit.
constexpr u64 PostHookRva1 = 0x1436C05;
constexpr std::array<u8, 14> PostHookSig1 = {
    0x48, 0x8B, 0x43, 0x60,       // mov rax, [rbx+0x60]
    0xC5, 0xFA, 0x10, 0x40, 0x50, // vmovss xmm0, [rax+0x50]
    0xC5, 0xFA, 0x11, 0x43, 0x50, // vmovss [rbx+0x50], xmm0
};
constexpr u64 PostHookRva2 = 0x1436E1A;
constexpr std::array<u8, 16> PostHookSig2 = {
    0xC5, 0xFA, 0x10, 0x83, 0x88, 0x00, 0x00, 0x00, // vmovss xmm0, [rbx+0x88]
    0xC5, 0xF0, 0x57, 0xC9,                         // vxorps xmm1, xmm1, xmm1
    0xC5, 0xF8, 0x2E, 0xC1,                         // vucomiss xmm0, xmm1
};
constexpr u32 OffsetManagerCamera = 0x60;
constexpr u32 OffsetMatrix = 0x10;
constexpr u32 OffsetPosition = 0x40;
// The camera update function; stores to pitch/yaw inside it are disabled while the
// headset drives the camera.
constexpr u64 CameraFuncStart = 0x143AC60;
constexpr u64 CameraFuncEnd = 0x143FAD0;
// Field camera step: after the camera manager (r13+0x128) has been copied into the render
// camera (r15), right before the optional debug-camera override. Every camera mode (follow,
// first-person aim camera, blends) passes here once per rendered frame, so this is where
// the head pose, FOV and eye position are written.
constexpr u64 RenderHookRva = 0x193BEC5;
// After the optional override has copied its entire camera over r15. The first
// hook runs before that copy; holding its position alone cannot constrain it.
constexpr u64 RenderFinalRva = 0x193BF61;
constexpr std::array<u8, 14> RenderFinalSig = {
    0x49,0x8B,0x3E, 0x48,0x85,0xFF, 0xC5,0xFA,0x10,0x85,0xD4,0xFE,0xFF,0xFF};
// Native camera-effects pass: immediately before multiplying the camera by the
// accumulated effect transform at [rbp-0x80]. Both output cameras use this matrix.
constexpr u64 CameraEffectRva = 0x1571C9D;
constexpr std::array<u8, 15> CameraEffectSig = {
    0xC5,0x78,0x28,0x63,0x10, 0xC5,0xF8,0x28,0x4B,0x20, 0xC5,0xF8,0x28,0x53,0x30};
std::atomic<u8*> visceral_effect_controller{};
std::atomic<u64> visceral_effect_record{};
// Per-effect animation output, before the native evaluator composes it into the
// shared matrix. Three complete non-relative instructions; modifies stack output only.
constexpr u64 CameraEffectSourceRva = 0x17E067E;
constexpr std::array<u8, 14> CameraEffectSourceSig = {
    0xC5,0xF8,0x57,0xC0, 0xC5,0xF8,0x5C,0x45,0xB0,
    0xC5,0xF9,0x6F,0x4D,0xC0};
constexpr std::array<u8, 14> RenderHookSig = {
    0x49, 0x8B, 0x85, 0x10, 0x01, 0x00, 0x00, // mov rax, [r13+0x110]
    0x83, 0xB8, 0xC4, 0x00, 0x00, 0x00, 0x00, // cmp dword ptr [rax+0xC4], 0
};
constexpr u32 OffsetFieldManager = 0x128;
constexpr u32 OffsetManagerMode = 0x84;   // 0 follow, 1 follow->aim, 2 aim, 3 aim->follow
constexpr u32 OffsetManagerAimCam = 0x68; // first-person mods render through the aim camera
constexpr u32 OffsetManagerFollowCam = 0x60;
// Follow camera: unsmoothed orbit point (character position + chrOrgOffset, written every
// update at 0x143BAE1) and the chrOrgOffset vector itself (LockCamParam, y = 1.42 m).
constexpr u32 OffsetFollowPivot = 0xA0;
constexpr u32 OffsetFollowOrgOffset = 0x160;
constexpr u32 OffsetAspect = 0x54; // render camera: width / height of the projection
constexpr u32 OffsetNear = 0x58;
constexpr u32 OffsetFar = 0x5C;
// fp_near_down_m ramps in between these view pitches (radians, + = down).
constexpr float NearDownStart = 20.0f * 0.0174532925f;
constexpr float NearDownFull = 40.0f * 0.0174532925f;
// First-person camera mod (Nexus bloodborne/287): NOPs `mov [rbx+0x84], rax` here.
constexpr u64 FirstPersonModRva = 0x143709B;
constexpr std::array<u8, 8> FirstPersonModSig = {0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0xE9};

constexpr u32 OffsetFov = 0x50;
constexpr u32 OffsetPitch = 0x140;
constexpr u32 OffsetYaw = 0x144;
constexpr u32 OffsetPitchCopy = 0x150;
constexpr u32 OffsetLockOn = 0x154; // 1.0f while locked on (BBMCC)

struct StoreSite {
    u8* address;
    u8 length;
    std::array<u8, 16> original;
};

std::vector<StoreSite> g_store_sites;
bool g_nops_active = false; // only touched on the camera thread
bool g_installed = false;
bool g_post_hooks = false;
std::atomic<float> g_fov_override{0.0f};
// Render orientation for the post hook (camera thread only).
bool g_view_active = false;
float g_view_yaw = 0.0f;
float g_view_pitch = 0.0f;
// Follow-camera state seen this frame (camera thread only), consumed by the render hook.
bool g_follow_seen = false;
float g_follow_pitch = 0.0f;
float g_follow_yaw = 0.0f;
float g_follow_fov = 0.0f;
bool g_follow_lock = false;
// Last tick result: drives the follow-camera orbit on the next update.
VR::CameraOverride g_last{};
bool g_render_hook = false;
u64 g_render_ticks = 0;     // render-hook ticks so far
u64 g_seen_render_ticks = 0; // value seen by the last follow update
u32 g_follow_without_render = 0;
// Cinematic flat mode (camera thread): the flat period seen by the last follow update, and the
// one-shot FOV snap for the next FOV hook call.
bool g_cinematic_flat = false;
bool g_flat_handoff = false; // flat started, the game camera not handed back yet
bool g_fov_snap = false;
// The render hook ran without driving the camera: the next driven frame restarts the camera
// filters, height and (comfort camera) body position, so after a lever ride, an elevator or a
// cinematic the eye never glides.
bool g_comfort_reset = true;
// The first-person eye's root height through turns in place (core/vr/root_height_hold.h).
VR::RootHeightHold g_root_hold;
// Which first-person camera the last driven frame used (the comfort camera, dodge_camera=1, or the
// old one); the other restarts when it takes over.
bool g_comfort_path = false;
bool g_legacy_reset = true;
std::unique_ptr<Xbyak::CodeGenerator> g_code;
u64 g_hits = 0;
std::chrono::steady_clock::time_point g_last_log{};

// VR_SHAKE_LOG=1 (test tooling): where the game's camera shake goes. Every 0.5 s one line with the
// window's maxima (camera thread only):
//  follow  - the follow camera's matrix as the game built it this update, against the orientation
//            its own pitch/yaw give (the shake the game adds to its camera), and its position off
//            the orbit axis (min..max, metres);
//  copy    - the render camera as the game copied it (before the VR writes), against the follow
//            camera: anything the camera manager adds on the way;
//  after   - the render camera at the next camera update, against what the VR camera wrote:
//            anything the game changes in it after the VR writes, while the frame is built;
//  eye     - the VR eye: view angle and position change per frame.
struct ShakeProbe {
    bool on = false;
    std::chrono::steady_clock::time_point window_start{};
    u32 frames = 0;
    float follow_rot = 0.0f, follow_roll = 0.0f;
    float follow_off_min = 1e9f, follow_off_max = -1e9f;
    // The follow camera's own jitter: the change of its per-update step (rows in degrees, position
    // in metres). A camera that lags or turns smoothly stays small, a shake does not.
    float follow_rjerk = 0.0f, follow_pjerk = 0.0f;
    bool have_follow = false;
    VR::Shake::Rows follow_rows{};
    float follow_p[3]{}, follow_fv[6]{}, follow_v[3]{};
    float copy_rot = 0.0f, copy_pos = 0.0f;
    float after_rot = 0.0f, after_pos = 0.0f;
    float eye_rot = 0.0f, eye_pos = 0.0f, eye_jerk = 0.0f, eye_rjerk = 0.0f;
    // The follow camera's own pitch/yaw fields: the largest change from one update to the next.
    float field_step = 0.0f;
    bool have_field = false;
    float field_pitch = 0.0f, field_yaw = 0.0f;
    // The last VR write into the render camera, checked at the next camera update.
    u8* written_render = nullptr;
    float written_m[12]{};
    float written_p[3]{};
    bool have_eye = false;
    VR::Shake::Rows eye_rows{};
    float eye_p[3]{};
    float eye_v[3]{}; // the last frame's eye step
    float eye_fv[6]{}; // the last frame's step of the forward and up rows
};
ShakeProbe g_shake{std::getenv("VR_SHAKE_LOG") != nullptr};

void ShakeFlush() {
    auto& s = g_shake;
    const auto now = std::chrono::steady_clock::now();
    if (s.window_start.time_since_epoch().count() == 0) {
        s.window_start = now;
        return;
    }
    if (now - s.window_start < std::chrono::milliseconds(500)) {
        return;
    }
    s.window_start = now;
    static const auto t0 = now;
    // wall: the wall clock (seconds mod 100000), to line the windows up with a test scenario.
    const double wall = std::fmod(
        std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(),
        100000.0);
    LOG_INFO(Core,
             "VR shake: t={:.1f} wall={:.1f} frames={} follow rot={:.3f} roll={:.3f} deg "
             "off={:.3f}..{:.3f} m rjerk={:.3f} deg pjerk={:.4f} m | copy rot={:.3f} deg "
             "pos={:.4f} m | after rot={:.3f} deg pos={:.4f} m | eye rot={:.3f} deg pos={:.4f} "
             "jerk={:.4f} m rjerk={:.3f} deg | fields step={:.3f} deg",
             std::chrono::duration<float>(now - t0).count(), wall, s.frames, s.follow_rot,
             s.follow_roll, s.follow_off_min > 1e8f ? 0.0f : s.follow_off_min,
             s.follow_off_max < -1e8f ? 0.0f : s.follow_off_max, s.follow_rjerk, s.follow_pjerk,
             s.copy_rot, s.copy_pos, s.after_rot, s.after_pos, s.eye_rot, s.eye_pos, s.eye_jerk,
             s.eye_rjerk, s.field_step);
    s.frames = 0;
    s.follow_rot = s.follow_roll = 0.0f;
    s.follow_off_min = 1e9f;
    s.follow_off_max = -1e9f;
    s.copy_rot = s.copy_pos = s.after_rot = s.after_pos = s.eye_rot = s.eye_pos = 0.0f;
    s.eye_jerk = s.eye_rjerk = s.field_step = 0.0f;
    s.follow_rjerk = s.follow_pjerk = 0.0f;
}

// At the follow camera's update (before the next copy into the render camera): the render camera
// still holds what the last frame was built with.
void ShakeAfterCheck() {
    auto& s = g_shake;
    if (s.written_render == nullptr) {
        return;
    }
    const auto* m = reinterpret_cast<const float*>(s.written_render + OffsetMatrix);
    const auto* p = reinterpret_cast<const float*>(s.written_render + OffsetPosition);
    s.after_rot = std::max(s.after_rot, VR::Shake::AngleDeg(VR::Shake::FromMatrix(m),
                                                            VR::Shake::FromMatrix(s.written_m)));
    s.after_pos = std::max(s.after_pos, VR::Shake::Distance(p, s.written_p));
    s.written_render = nullptr;
}

// Right after the follow camera's update, before the VR orientation replaces its matrix.
void ShakeFollow(const u8* camera) {
    auto& s = g_shake;
    float pitch{}, yaw{};
    std::memcpy(&pitch, camera + OffsetPitch, sizeof(pitch));
    std::memcpy(&yaw, camera + OffsetYaw, sizeof(yaw));
    const auto game = VR::Shake::FromMatrix(reinterpret_cast<const float*>(camera + OffsetMatrix));
    const auto own = VR::Shake::FromYawPitch(yaw, pitch);
    s.follow_rot = std::max(s.follow_rot, VR::Shake::AngleDeg(game, own));
    s.follow_roll = std::max(s.follow_roll, std::abs(VR::Shake::RollDeg(game)));
    const float off =
        VR::Shake::OffAxis(reinterpret_cast<const float*>(camera + OffsetPosition),
                           reinterpret_cast<const float*>(camera + OffsetFollowPivot), game);
    s.follow_off_min = std::min(s.follow_off_min, off);
    s.follow_off_max = std::max(s.follow_off_max, off);
    const auto* pos = reinterpret_cast<const float*>(camera + OffsetPosition);
    if (s.have_follow) {
        float fv[6], v[3];
        for (int i = 0; i < 3; ++i) {
            fv[i] = game.r[2][i] - s.follow_rows.r[2][i];
            fv[3 + i] = game.r[1][i] - s.follow_rows.r[1][i];
            v[i] = pos[i] - s.follow_p[i];
        }
        s.follow_rjerk = std::max(s.follow_rjerk,
                                  std::max(VR::Shake::Distance(fv, s.follow_fv),
                                           VR::Shake::Distance(fv + 3, s.follow_fv + 3)) *
                                      57.2957795f);
        s.follow_pjerk = std::max(s.follow_pjerk, VR::Shake::Distance(v, s.follow_v));
        std::memcpy(s.follow_fv, fv, sizeof(fv));
        std::memcpy(s.follow_v, v, sizeof(v));
    }
    s.follow_rows = game;
    std::memcpy(s.follow_p, pos, sizeof(s.follow_p));
    s.have_follow = true;
    if (s.have_field) {
        const float dy = std::remainder(yaw - s.field_yaw, 6.2831853f);
        s.field_step = std::max(
            s.field_step, std::max(std::abs(pitch - s.field_pitch), std::abs(dy)) * 57.2957795f);
    }
    s.field_pitch = pitch;
    s.field_yaw = yaw;
    s.have_field = true;
}

void SetStoreNops(bool enable) {
    for (const auto& site : g_store_sites) {
        if (enable) {
            std::memset(site.address, 0x90, site.length);
        } else {
            std::memcpy(site.address, site.original.data(), site.length);
        }
    }
    g_nops_active = enable;
}

void PS4_SYSV_ABI CameraHook(u8* camera, u8* /*saved_ymm*/) {
    auto* pitch = reinterpret_cast<float*>(camera + OffsetPitch);
    auto* yaw = reinterpret_cast<float*>(camera + OffsetYaw);
    auto* pitch_copy = reinterpret_cast<float*>(camera + OffsetPitchCopy);
    const float fov = *reinterpret_cast<float*>(camera + OffsetFov);

    if (g_shake.on) {
        ShakeAfterCheck();
    }
    float lock_flag{};
    std::memcpy(&lock_flag, camera + OffsetLockOn, sizeof(lock_flag));
    g_follow_seen = true;
    g_follow_pitch = *pitch;
    g_follow_yaw = *yaw;
    g_follow_fov = fov;
    g_follow_lock = lock_flag == 1.0f;

    // The tick normally happens in the render hook (one per rendered frame); this applies
    // its last result. Without the render hook (signature mismatch) the tick happens here.
    if (!g_render_hook) {
        g_last = VR::OnCameraTick(*pitch, *yaw, fov, g_follow_lock);
    } else if (g_render_ticks != g_seen_render_ticks) {
        g_seen_render_ticks = g_render_ticks;
        g_follow_without_render = 0;
    } else if (g_follow_without_render < 1000) {
        ++g_follow_without_render;
    }
    // A follow camera that is no longer rendered (cutscene, menu scene) is left to the game.
    static const VR::CameraOverride inactive{};
    const VR::CameraOverride& result =
        (g_render_hook && g_follow_without_render > 4) ? inactive : g_last;
    // Cinematic flat mode starts: the game's own camera takes over from here. In first person
    // the orbit pitch was the head's, so a downward glance would start the flat view from
    // overhead: it starts at the normal orbit pitch instead. The FOV eases from the VR FOV to the
    // game's over many frames: the next FOV hook call snaps it.
    const bool flat = VR::CinematicFlat();
    if (flat && !g_cinematic_flat) {
        g_flat_handoff = true;
    }
    g_cinematic_flat = flat;
    if (!flat) {
        g_flat_handoff = false;
    } else if (g_flat_handoff && !result.active) {
        // The first update the game drives again (the stores are still disabled from the last
        // driven frame).
        g_flat_handoff = false;
        if (g_nops_active && VR::IsFirstPerson()) {
            const float orbit_pitch = VR::GetSettings().orbit_pitch_deg * 0.0174532925f;
            *pitch = orbit_pitch;
            *pitch_copy = orbit_pitch;
        }
        g_fov_snap = true;
    }
    if (result.active) {
        if (g_nops_active != result.orbit) {
            SetStoreNops(result.orbit);
        }
        if (result.orbit) {
            *pitch = result.pitch;
            *pitch_copy = result.pitch;
            *yaw = result.yaw;
        }
        g_fov_override.store(result.vfov, std::memory_order_relaxed);
        g_view_active = result.decoupled;
        g_view_yaw = result.view_yaw;
        // The manager's camera keeps your head's pitch: render_pitch_bias_deg is the render
        // camera's alone (like camera_roll).
        g_view_pitch = result.view_pitch;
    } else {
        if (g_nops_active) {
            SetStoreNops(false);
        }
        g_fov_override.store(0.0f, std::memory_order_relaxed);
        g_view_active = false;
    }

    ++g_hits;
    static const bool dump = std::getenv("VR_CAM_DUMP") != nullptr;
    if (dump && (g_hits % 15) == 0) {
        std::string line = fmt::format("CAMDUMP hit={} yaw={:.4f} pitch={:.4f} |", g_hits,
                                       result.yaw, result.pitch);
        const auto* f = reinterpret_cast<const float*>(camera);
        for (int i = 0; i < 128; ++i) {
            line += fmt::format(" {:.4f}", f[i]);
        }
        LOG_INFO(Core, "{}", line);
    }
}

// Game convention (rows: right, up, forward), yaw + = right, pitch + = down, roll about forward
// + = the right side up (camera_roll; 0 = level, the rows below exactly).
void WriteOrientation(u8* camera, float yaw, float pitch, float roll = 0.0f) {
    const float sy = std::sin(yaw), cy = std::cos(yaw);
    const float sp = std::sin(pitch), cp = std::cos(pitch);
    float rows[12] = {
        cy,      0.0f, -sy,     0.0f, // right
        sy * sp, cp,   cy * sp, 0.0f, // up
        sy * cp, -sp,  cy * cp, 0.0f, // forward
    };
    if (roll != 0.0f) {
        // right' = right cos r + up sin r, up' = up cos r - right sin r.
        const float sr = std::sin(roll), cr = std::cos(roll);
        for (int i = 0; i < 3; ++i) {
            const float right = rows[i], up = rows[4 + i];
            rows[i] = right * cr + up * sr;
            rows[4 + i] = up * cr - right * sr;
        }
    }
    std::memcpy(camera + OffsetMatrix, rows, sizeof(rows));
}

// Runs right after the camera update returns (first argument: the camera manager).
void PS4_SYSV_ABI CameraPostHook(u8* manager, u8* /*saved_ymm*/) {
    if (g_shake.on) {
        u8* follow = nullptr;
        std::memcpy(&follow, manager + OffsetManagerCamera, sizeof(follow));
        if (follow != nullptr) {
            ShakeFollow(follow);
        }
    }
    if (!g_view_active) {
        return;
    }
    u8* camera = nullptr;
    std::memcpy(&camera, manager + OffsetManagerCamera, sizeof(camera));
    if (camera == nullptr) {
        return;
    }
    WriteOrientation(camera, g_view_yaw, g_view_pitch);
}

// VR_BODY_SPECTATOR=<right_m>,<up_m>,<back_m>[,<look_down_m>] (test tooling, never set for
// play): after the first-person frame is set up (the VR body's anchor, the arms, the evasion all
// from the first-person eye as always), the render camera alone moves to that offset from the eye
// (along the view's heading: right, up, back) and looks at the point look_down_m (default 0.6)
// under the eye, so the eye dumps show the hunter's body as the first-person VR body poses it,
// from outside (the waist and trunk while walking). Use with late_warp=0. With VR_TUNE_FILE the
// keys spec.on (0/1), spec.right, spec.up, spec.back and spec.down switch and move it live.
// inframe_hand_sync (core/vr/quick_settings.h): the hunter's pose of this update was placed by
// the VR body for the previous update's view anchor (BodyRenderTick publishes it for the next
// pose copy), and the frame shows his arm where your controllers are against the camera it is
// drawn with. Our hand items (the held item, the wrist gauge, the vial's drain) placed in world
// from his dummies therefore sat one update of your motion behind the arm while you walk, strafe
// or turn (the play space moved on between the two updates: a 4.5 m/s strafe at 45 fps, 10 cm).
// Right after the producers placed them, they are carried with your play space from the previous
// update's frame to this one's (VR::InFrame::CarryBetween; your head's own motion is in both
// frames alike and moves nothing; standing still nothing moves).
struct HandSync {
    u64 record{0};
    VR::InFrame::PlayFrame play{};
    VR::InFrame::HandCarry carry{}; // this update's (HandSyncBegin), applied by HandSyncTick
    u64 prev{0};
    float moved{0.0f};
    float turn{0.0f};
    u64 ticks{0};
    u64 carried{0};
    u64 items{0};
    u64 skipped{0};
    double moved_sum{0.0};
    float moved_max{0.0f};
    bool logged{false};
};
HandSync g_hand_sync{};

// Before the producers (HeldRenderTick, VitalsRenderTick, QuickItemsRenderTick): this update's
// carry, published for them (VR::InFrame::SetHandCarry: the vial's depth proxy, the arc's hover
// point, the use ring's place and the throw's start move with it, as the hand items do).
void HandSyncBegin(u64 record_id, const float* rows, const float* position) {
    auto& h = g_hand_sync;
    h.carry = {};
    VR::InFrame::SetHandCarry({});
    VR::CameraRecord rec{};
    // First person only (the VR body places his arms for the anchor: BodyRenderTick wants
    // fp_follow): third person and the aim camera keep their dummies as they are (a camera orbit
    // there is not his hand's move).
    const bool have = VR::GetRecord(record_id, rec) && rec.driven && !rec.menu && rec.first_person;
    const u64 prev = h.record;
    const VR::InFrame::PlayFrame from = h.play;
    VR::InFrame::PlayFrame to{};
    if (have) {
        to.eye_world = {position[0] - rec.eye_offset.x, position[1] - rec.eye_offset.y,
                        position[2] - rec.eye_offset.z};
        to.head = {rec.head_position.x, rec.head_position.y, rec.head_position.z};
        to.base_yaw = rec.base_yaw;
        to.scale = rec.world_scale > 0.0f ? rec.world_scale : 1.0f;
    }
    h.record = have ? record_id : 0;
    h.play = to;
    if (!have || !VR::Quick::GetQuickSettings().inframe_hand_sync || prev == 0 ||
        record_id != prev + 1) {
        return;
    }
    ++h.ticks;
    // Your play space's move between the two updates (its origin, and its turn).
    const VR::InFrame::CarryStep step = VR::InFrame::CarryStepOf(from, to);
    if (!step.apply) {
        ++h.skipped; // a teleport, a snap turn, a scale change: nothing to carry across
        return;
    }
    if (step.still) {
        return; // standing still: nothing moves
    }
    VR::InFrame::ViewOf view{};
    std::memcpy(view.rows.data(), rows, sizeof(float) * 12);
    std::memcpy(view.position.data(), position, sizeof(float) * 3);
    h.carry = {record_id, from, to, VR::InFrame::CarryBetween(from, to, view)};
    h.prev = prev;
    h.moved = step.moved_m;
    h.turn = step.turn_rad;
    VR::InFrame::SetHandCarry(h.carry);
}

// After the producers submitted this update's lists: their hand items carried (HandSyncBegin).
void HandSyncTick(u64 record_id) {
    auto& h = g_hand_sync;
    if (h.carry.record != record_id || record_id == 0) {
        return;
    }
    const float moved = h.moved;
    const u32 n = VR::InFrame::Carry(record_id, h.carry.rv);
    if (n > 0) {
        ++h.carried;
        h.items += n;
        h.moved_sum += moved;
        h.moved_max = std::max(h.moved_max, moved);
        if (!h.logged) {
            h.logged = true;
            LOG_INFO(Core,
                     "VR inframe: hand sync on (inframe_hand_sync=1): {} items on the hunter's hand "
                     "carried {:.3f} m / {:.2f} deg with your play space (update {} -> {})",
                     n, moved, h.turn * 57.29578f, h.prev, record_id);
        }
    }
    if (h.ticks % 3600 == 0) {
        LOG_INFO(Core,
                 "VR inframe: hand sync: {} of {} updates carried {} hand items (avg {:.3f} m, max "
                 "{:.3f} m), {} not (teleport / snap turn)",
                 h.carried, h.ticks, h.items,
                 h.carried > 0 ? h.moved_sum / static_cast<double>(h.carried) : 0.0, h.moved_max,
                 h.skipped);
    }
}

void SpectatorView(u8* render, const VR::CameraOverride& result, const float* eye, float scale) {
    static const std::array<float, 5> env = [] {
        std::array<float, 5> o{0.0f, 2.2f, 0.0f, 0.0f, 0.6f};
        if (const char* value = std::getenv("VR_BODY_SPECTATOR"); value != nullptr) {
            o[0] = 1.0f;
            std::sscanf(value, "%f,%f,%f,%f", &o[1], &o[2], &o[3], &o[4]);
            LOG_INFO(Core,
                     "VR_BODY_SPECTATOR: the render camera sits {:.2f} m right, {:.2f} m up, "
                     "{:.2f} m back of the first-person eye, looking {:.2f} m under it (test "
                     "view)",
                     o[1], o[2], o[3], o[4]);
        }
        return o;
    }();
    if (env[0] == 0.0f && !VR::Tune::Active()) {
        return;
    }
    if (VR::Tune::Get("spec.on", env[0]) == 0.0f) {
        return;
    }
    const std::array<float, 4> offset = {
        VR::Tune::Get("spec.right", env[1]), VR::Tune::Get("spec.up", env[2]),
        VR::Tune::Get("spec.back", env[3]), VR::Tune::Get("spec.down", env[4])};
    const float sy = std::sin(result.view_yaw), cy = std::cos(result.view_yaw);
    const float right[3] = {cy, 0.0f, -sy};
    const float forward[3] = {sy, 0.0f, cy};
    float p[3];
    float target[3];
    for (int i = 0; i < 3; ++i) {
        p[i] = eye[i] + (right[i] * offset[0] - forward[i] * offset[2]) * scale;
        target[i] = eye[i];
    }
    p[1] += offset[1] * scale;
    target[1] -= offset[3] * scale;
    const float d[3] = {target[0] - p[0], target[1] - p[1], target[2] - p[2]};
    const float length = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (!(length > 1e-4f)) {
        return;
    }
    WriteOrientation(render, std::atan2(d[0], d[2]),
                     std::asin(std::clamp(-d[1] / length, -1.0f, 1.0f)));
    std::memcpy(render + OffsetPosition, p, sizeof(p));
}

// A remo cutscene (FromSoftware's in-engine cutscenes: a boss's entrance, Father Gascoigne's
// 2026-10-15) renders through the follow camera, so it read as gameplay and played from the head
// in first person. While one plays the main player is not updated (VR::LastPlayerUpdateNs stands
// still): a cutscene file opened in the last RemoWindowNs and no player update for RemoStillNs
// (none ever seen: never). The camera is then left to the game, as for any cutscene: the virtual
// screen shows it, and first-person VR comes back with the player's updates (the camera gap's
// resume).
constexpr s64 RemoWindowNs = 300'000'000'000; // a cutscene file is opened as it starts
constexpr s64 RemoStillNs = 400'000'000;      // gameplay updates the player every frame
bool g_remo_playing{};

bool RemoCutscene() {
    const s64 opened = Core::FileSys::LastCutsceneOpenNs();
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    const s64 updated = VR::LastPlayerUpdateNs();
    const bool playing = opened != 0 && now - opened >= 0 && now - opened <= RemoWindowNs &&
                         updated != 0 && now - updated > RemoStillNs;
    if (playing != g_remo_playing) {
        g_remo_playing = playing;
        VR::SetRemoCutscene(playing); // the renderer draws the hunter's head meanwhile
        LOG_INFO(Core,
                 "VR camera: {} (a cutscene file opened {:.1f} s ago, no player update for "
                 "{:.1f} s)",
                 playing ? "a remo cutscene plays: the camera is the game's (the virtual screen)"
                         : "the remo cutscene ended: first person again",
                 static_cast<double>(now - opened) * 1e-9,
                 static_cast<double>(now - updated) * 1e-9);
    }
    return playing;
}

// The render camera (a per-frame copy of the camera manager) is the last write before
// rendering; the one VR tick per rendered frame happens here.
struct FinalVisceralView {
    u8* camera{};
    u64 record{};
    std::array<u8, 80> view{}; // +0x10 through +0x5F: transform and projection
    bool hold{};
};
thread_local FinalVisceralView final_visceral_view;

void PS4_SYSV_ABI VisceralCameraEffectSourceHook(const u8* instance, float* transform) {
    VR::GrabCamera::ThrowPlacement held{};
    if (!VisceralStagePlacement(VR::Body::SteadyNowNs(), VR::GetSettings().world_scale, held)) return;
    // Native code has already read resource+4/+8 and instance+0x38 here.
    // Keep the raw resource identity: its first word is not yet a verified ID.
    const u8* resource{};
    float elapsed{}, duration{};
    std::memcpy(&resource, instance+0x30, sizeof(resource));
    std::memcpy(&elapsed, instance+0x38, sizeof(elapsed));
    std::memcpy(&duration, resource+4, sizeof(duration));
    const float before[3]{transform[12],transform[13],transform[14]};
    const bool middle_reduced = VR::VisceralStage::AttenuateMiddleKick(transform, elapsed, duration);
    if (middle_reduced) TraceVisceralView("middle_kick_reduced",
        visceral_effect_record.load(std::memory_order_relaxed), before, transform+12);
    const bool reduced = VR::VisceralStage::AttenuateLateKick(transform, elapsed, duration);
    if (reduced) TraceVisceralView("late_kick_reduced",
        visceral_effect_record.load(std::memory_order_relaxed), before, transform+12);
    const auto kind = fmt::format("effect_source_{:x}", reinterpret_cast<uintptr_t>(resource));
    const float metadata[3]{elapsed,duration,0};
    TraceVisceralView(kind.c_str(), visceral_effect_record.load(std::memory_order_relaxed),
                      metadata, transform+12);
}

void PS4_SYSV_ABI VisceralCameraEffectHook(u8* controller, float* effect) {
    if (controller != visceral_effect_controller.load(std::memory_order_acquire)) return;
    VR::GrabCamera::ThrowPlacement held{};
    const bool active = VisceralStagePlacement(VR::Body::SteadyNowNs(), VR::GetSettings().world_scale, held);
    // This is the accumulated native effect during the visceral/finishing clip,
    // not an identified effect ID. Headset pose is not part of this matrix.
    // Keep native timing; reduce lunge separately from rotation/other translation.
    const float before[3]{effect[12], effect[13], effect[14]};
    const float rotation_before[3]{effect[0], effect[5], effect[10]};
    const bool softened = active && VR::VisceralStage::SoftenShake(effect);
    if (active) VR::VisceralStage::ReduceForwardLunge(effect);
    const float rotation_after[3]{effect[0], effect[5], effect[10]};
    TraceVisceralView(!active ? "effect_rotation_idle" : softened ? "effect_rotation" : "effect_rotation_skipped",
                     visceral_effect_record.load(std::memory_order_relaxed),
                     rotation_before, rotation_after);
    TraceVisceralView(active ? "native_effect" : "native_effect_idle", visceral_effect_record.load(std::memory_order_relaxed),
                      before, effect + 12);
}

void PS4_SYSV_ABI RenderFinalHook(u8* render, u8*) {
    auto& view = final_visceral_view;
    if (view.camera != render) return;
    float expected[3], actual[3];
    std::memcpy(expected, view.view.data() + OffsetPosition - OffsetMatrix, sizeof(expected));
    std::memcpy(actual, render + OffsetPosition, sizeof(actual));
    TraceVisceralView("late_camera", view.record, expected, actual);
    // Observational: this earlier copy was ruled out by the headset trace.
    view.camera = nullptr;
}

void PS4_SYSV_ABI RenderCameraHook(u8* field_camera, u8* /*saved_ymm*/, u8* render) {
    final_visceral_view = {};
    static VR::GrabCamera::PairPosition grab_position;
    static VR::GrabCamera::ThrowCamera throw_camera;
    u8* manager = nullptr;
    std::memcpy(&manager, field_camera + OffsetFieldManager, sizeof(manager));
    const bool follow = g_follow_seen;
    g_follow_seen = false;
    if (manager == nullptr || render == nullptr) {
        grab_position.Reset();
        throw_camera.Reset();
        BodyRenderTick(nullptr, nullptr, false);
        EvasionRenderTick(nullptr, false);
        return;
    }
    if (RemoCutscene()) {
        g_comfort_reset = true;
        grab_position.Reset();
        throw_camera.Reset();
        BodyRenderTick(nullptr, nullptr, false);
        EvasionRenderTick(nullptr, false);
        return; // the cutscene's camera: left to the game (virtual screen)
    }
    u32 mode{};
    std::memcpy(&mode, manager + OffsetManagerMode, sizeof(mode));
    // Never written by us: the presenter crops each eye out of the image with it.
    float aspect{};
    std::memcpy(&aspect, render + OffsetAspect, sizeof(aspect));
    if (g_shake.on) {
        ShakeFlush();
        u8* follow_cam = nullptr;
        std::memcpy(&follow_cam, manager + OffsetManagerFollowCam, sizeof(follow_cam));
        if (mode == 0 && follow && follow_cam != nullptr) {
            ++g_shake.frames;
            const auto* rm = reinterpret_cast<const float*>(render + OffsetMatrix);
            const auto* fm = reinterpret_cast<const float*>(follow_cam + OffsetMatrix);
            g_shake.copy_rot =
                std::max(g_shake.copy_rot, VR::Shake::AngleDeg(VR::Shake::FromMatrix(rm),
                                                               VR::Shake::FromMatrix(fm)));
            g_shake.copy_pos = std::max(
                g_shake.copy_pos,
                VR::Shake::Distance(reinterpret_cast<const float*>(render + OffsetPosition),
                                    reinterpret_cast<const float*>(follow_cam + OffsetPosition)));
        }
    }

    // The follow camera's pivot (on the hunter): after a gap in the camera updates the view
    // resumes only if it stayed put (core/vr/camera_gap.h; a load or teleport moves it). Only
    // from the follow camera that ran this frame (the aim camera: the yaw decides alone).
    VR::Vec3 anchor{};
    const VR::Vec3* anchor_ptr = nullptr;
    if (follow && mode != 1 && mode != 2) {
        u8* follow_cam = nullptr;
        std::memcpy(&follow_cam, manager + OffsetManagerFollowCam, sizeof(follow_cam));
        if (follow_cam != nullptr) {
            float pivot[3];
            std::memcpy(pivot, follow_cam + OffsetFollowPivot, sizeof(pivot));
            if (std::isfinite(pivot[0]) && std::isfinite(pivot[1]) && std::isfinite(pivot[2])) {
                anchor = {pivot[0], pivot[1], pivot[2]};
                anchor_ptr = &anchor;
            }
        }
    }

    VR::CameraOverride result{};
    if (mode == 1 || mode == 2) {
        // Aim camera (first-person mods): the game steers it; read its heading.
        u8* aim = nullptr;
        std::memcpy(&aim, manager + OffsetManagerAimCam, sizeof(aim));
        if (aim == nullptr) {
            g_comfort_reset = true;
            grab_position.Reset();
            throw_camera.Reset();
            BodyRenderTick(nullptr, nullptr, false);
            EvasionRenderTick(nullptr, false);
            return;
        }
        float f[3];
        std::memcpy(f, aim + OffsetMatrix + 0x20, sizeof(f)); // forward row
        float fov{};
        std::memcpy(&fov, aim + OffsetFov, sizeof(fov));
        const float yaw = std::atan2(f[0], f[2]);
        const float pitch = std::asin(std::clamp(-f[1], -1.0f, 1.0f));
        result = VR::OnCameraTick(pitch, yaw, fov, false, true, aspect, true, anchor_ptr);
    } else if (follow) {
        result = VR::OnCameraTick(g_follow_pitch, g_follow_yaw, g_follow_fov, g_follow_lock, false,
                                  aspect, true, anchor_ptr);
    } else {
        g_comfort_reset = true;
        grab_position.Reset();
        throw_camera.Reset();
        BodyRenderTick(nullptr, nullptr, false);
        EvasionRenderTick(nullptr, false);
        return; // cutscene or scripted camera: left to the game (virtual screen)
    }
    g_last = result;
    ++g_render_ticks;
    if (!result.active) {
        g_comfort_reset = true; // cinematic flat mode, virtual screen, VR off
        grab_position.Reset();
        throw_camera.Reset();
        BodyRenderTick(nullptr, nullptr, false);
        EvasionRenderTick(nullptr, false);
        return;
    }

    // camera_roll: only the render camera rolls; the manager's camera (CameraPostHook) stays
    // level, so nothing but the rendered view changes. render_pitch_bias_deg likewise: only the
    // render camera looks lower than your head (result.render_pitch; the camera record says so).
    WriteOrientation(render, result.view_yaw, result.render_pitch, result.view_roll);
    std::memcpy(render + OffsetFov, &result.vfov, sizeof(result.vfov));

    float p[3];
    std::memcpy(p, render + OffsetPosition, sizeof(p));
    const auto& settings = VR::GetSettings();
    u8* follow_cam = nullptr;
    std::memcpy(&follow_cam, manager + OffsetManagerFollowCam, sizeof(follow_cam));
    const auto now = std::chrono::steady_clock::now();
    const float scale = settings.world_scale;
    const bool fp_follow = VR::IsFirstPerson() && mode != 1 && mode != 2 && follow_cam != nullptr;
    // The eye in front of the head centre along the view (head yaw: never filtered), plus the
    // first-person camera's lead over the hunter's eyes (fp_ahead_m / fp_ahead_down_m: the VR
    // body does not follow that part, bloodborne_body.cpp).
    const float eye_forward_m =
        settings.fp_forward_m + (fp_follow ? VR::FirstPersonAheadM(result.view_pitch) : 0.0f);
    const float forward_x = std::sin(result.view_yaw) * eye_forward_m * scale;
    const float forward_z = std::cos(result.view_yaw) * eye_forward_m * scale;
    EyeTick eye{};
    if (fp_follow) {
        // First person: the eye sits at head height above the character, a little in front
        // of the head centre along the view, so the hunter's own head stays behind the
        // camera; the near plane clips what is left (hat brims, hair). The character root
        // moves smoothly, so attack and walk animations never bob the view.
        float pivot[3];
        float org_offset[3];
        std::memcpy(pivot, follow_cam + OffsetFollowPivot, sizeof(pivot));
        std::memcpy(org_offset, follow_cam + OffsetFollowOrgOffset, sizeof(org_offset));
        // The follow pivot can remain above the model on stairs. Use the same
        // unanimated root as the body/holsters, then apply the existing comfort
        // smoothing. Bone animation and physical head movement stay separate.
        // A turn in place keeps the model's offset over the physics root (core/vr/
        // root_height_hold.h: the game refits his model to the ground by his facing, which
        // bobbed the view 2-3 cm; VR_ROOT_HEIGHT_HOLD=0: the model's height as before).
        static const bool hold_on = [] {
            const char* env = std::getenv("VR_ROOT_HEIGHT_HOLD");
            return env == nullptr || env[0] != '0';
        }();
        const float phys_root[3] = {pivot[0], pivot[1] - org_offset[1], pivot[2]};
        float root_y = phys_root[1];
        float root_yaw = 0.0f;
        if (g_comfort_reset) {
            g_root_hold.Reset();
        }
        if (BodyRootHeight(root_y, &root_yaw) && hold_on) {
            root_y = g_root_hold.Step(phys_root, root_y, root_yaw, scale);
        } else {
            g_root_hold.Reset();
        }
        eye.target = {pivot[0], root_y + settings.fp_eye_height_m * scale, pivot[2]};
        eye.pivot = {pivot[0], pivot[1], pivot[2]};
        eye.org = {org_offset[0], org_offset[1], org_offset[2]};
        eye.pair_first = result.pair_first;
        eye.eye = VR::GetStereo().stereo ? (result.pair_first ? 0 : 1) : -1;
        eye.now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        p[0] = eye.target.x + forward_x;
        p[1] = eye.target.y;
        p[2] = eye.target.z + forward_z;
        if (settings.fp_near_m > 0.0f) {
            float near_m = settings.fp_near_m;
            // fp_near_down_m: looking down (from 20 deg below the horizon, all of it from 40 deg)
            // the near clip moves out to it, hiding the hunter's collar, chest and shoulder caps
            // right under the eye; hands further away than it stay.
            // VR_TUNE_FILE fp.near_down_m: live A/B in tests.
            const float near_down = VR::Tune::Get("fp.near_down_m", settings.fp_near_down_m);
            if (near_down > near_m) {
                const float t =
                    std::clamp((result.view_pitch - NearDownStart) / (NearDownFull - NearDownStart),
                               0.0f, 1.0f);
                near_m += (near_down - near_m) * t * t * (3.0f - 2.0f * t);
            }
            const float near_plane = near_m * scale;
            std::memcpy(render + OffsetNear, &near_plane, sizeof(near_plane));
        }
    }

    VR::GrabCamera::ThrowPlacement stage{};
    const bool staged = fp_follow && VisceralStagePlacement(eye.now_ns, scale, stage);
    static bool stage_was_active = false;
    if (stage_was_active && !staged) g_comfort_reset = true;
    stage_was_active = staged;
    bool roomscale_updated = false;
    VR::GrabCamera::Sample grab_sample{};
    const bool sampled = fp_follow && BodyGrabEye(eye.now_ns, grab_sample);
    const bool own_throw = sampled && grab_sample.own_throw;
    const VR::Vec3 grab_eye{grab_sample.eye.x, grab_sample.eye.y, grab_sample.eye.z};
    const bool grab = sampled && !own_throw;
    if (grab) {
        // Follow the animated body even when the physics capsule stays on the ground.
        // Head look, physical movement and stereo remain live.
        p[0] = grab_eye.x + forward_x + result.render_offset.x;
        p[1] = grab_eye.y + result.render_offset.y;
        p[2] = grab_eye.z + forward_z + result.render_offset.z;
        g_comfort_reset = true;
        g_legacy_reset = true;
        throw_camera.Reset();
    } else if (fp_follow && (settings.dodge_camera || staged)) {
        // Dodge comfort camera (core/vr/motion_camera.h): the body position is followed with a
        // critically damped spring horizontally and a mode-dependent one vertically, pair-locked
        // for both eyes of a stereo pair; the forward offset, the stereo eye and real head
        // movement are added after it, unfiltered.
        const char* reset = g_comfort_reset ? "inactive" : !g_comfort_path ? "path" : nullptr;
        g_comfort_reset = false;
        g_comfort_path = true;
        g_legacy_reset = true;
        const VR::Vec3 base = ComfortEye(eye, reset);
        // Resolve physical walking before the throw clearance check so it sees the same
        // tracked offset that will actually be rendered.
        VR::UpdateRoomscaleWalk(result, staged ? VR::Vec3{stage.base.x, stage.base.y, stage.base.z} : base,
                                fp_follow && !staged);
        roomscale_updated = true;
        // A freehand throw (a visceral, riposte, backstab): the view holds your place, your head
        // and hands tracked, and keeps off the victim; it glides back onto him when the throw
        // ends (the same for both eyes of a pair; grab_camera.h ThrowCamera). Check the actual
        // rendered head centre: include the forward lead and exclude only stereo separation.
        const VR::Body::Vec3 head{result.render_offset.x - result.eye_offset.x + forward_x,
                                  result.render_offset.y - result.eye_offset.y,
                                  result.render_offset.z - result.eye_offset.z + forward_z};
        const VR::Body::Vec3 base_b{base.x, base.y, base.z};
        VR::GrabCamera::ThrowPlacement shared{};
        const bool stage_enabled = VisceralStageEnabled();
        const bool have_shared = stage_enabled ? staged : BodyThrowPlacement(eye.now_ns, scale, shared);
        if (stage_enabled && staged) shared = stage;
        static bool shared_logging = false;
        static unsigned shared_hits = 0, shared_silhouette = 0, shared_missing = 0;
        static float shared_near_cm = 0.0f, shared_far_cm = 0.0f;
        if (!stage_enabled && have_shared && shared.holding && result.pair_first) {
            if (!shared_logging) {
                LOG_INFO(Core, "VR visceral: shared camera/body clearance target {:.0f} cm",
                         VR::GrabCamera::ThrowCamera::KeepOutM * 100.0f);
            }
            shared_logging = true;
            if (shared.surface.hit) {
                // This is the pose's ray BEFORE retreat, not the final rendered
                // head gap. Silhouette hits are projected collision geometry.
                const float distance_cm = shared.surface.distance * 100.0f / shared.scale;
                if (shared_hits + shared_silhouette == 0) {
                    shared_near_cm = shared_far_cm = distance_cm;
                } else {
                    shared_near_cm = std::min(shared_near_cm, distance_cm);
                    shared_far_cm = std::max(shared_far_cm, distance_cm);
                }
            }
            if (!shared.surface.hit) ++shared_missing;
            else if (shared.surface.projected) ++shared_silhouette;
            else ++shared_hits;
        } else if (!have_shared && shared_logging) {
            LOG_INFO(Core, "VR visceral: shared camera/body placement ended; surface rays: "
                           "{} eye-level, {} silhouette, {} unavailable",
                     shared_hits, shared_silhouette, shared_missing);
            if (shared_hits + shared_silhouette > 0) {
                LOG_INFO(Core, "VR visceral: sampled collision distance BEFORE retreat "
                               "{:.0f} to {:.0f} cm (negative = inside; includes silhouette hits); "
                               "clearance target {:.0f} cm; final rendered head gap not measured",
                         shared_near_cm, shared_far_cm,
                         VR::GrabCamera::ThrowCamera::KeepOutM * 100.0f);
            }
            shared_logging = false;
            shared_hits = shared_silhouette = shared_missing = 0;
        }
        if (have_shared) {
            // The pose already used this base/root. Add only live tracked head
            // movement here; another ray or blend would detach the body again.
            result.throw_offset = {shared.base.x - base.x, shared.base.y - base.y,
                                   shared.base.z - base.z};
            result.throw_root = {shared.root.x, shared.root.y, shared.root.z};
            result.throw_shared = true;
            result.throw_weight = shared.weight;
            result.throw_holding = shared.holding;
            throw_camera.Reset();
        } else if (stage_enabled) {
            // Native placement owns separation. No camera retreat, fallback re-anchor or glide.
            throw_camera.Reset();
        } else {
            const auto hold = throw_camera.Step(own_throw, base_b, grab_sample.facing, scale,
                                                result.pair_first, eye.now_ns);
            // Measure from the proposed eye centre before its keep-out correction,
            // avoiding feedback that would alternate between pushing and pulling in.
            VR::GrabCamera::SurfaceRay surface{};
            if (result.pair_first && own_throw && grab_sample.victim_valid &&
                VR::Body::Length(VR::Body::Vec3{base_b.x - grab_sample.victim.x, 0.0f,
                                               base_b.z - grab_sample.victim.z}) <= 3.0f * scale) {
                surface = CastVisceralSurface(static_cast<const u8*>(grab_sample.victim_character),
                                               base_b + hold + head, grab_sample.victim,
                                               grab_sample.facing, scale);
            }
            const auto push = throw_camera.KeepOut(base_b + hold + head, base_b,
                                                   own_throw && grab_sample.victim_valid,
                                                   grab_sample.victim, scale, result.pair_first, surface);
            result.throw_offset = {hold.x + push.x, hold.y + push.y, hold.z + push.z};
            result.throw_weight = throw_camera.Weight();
            result.throw_holding = throw_camera.Holding();
            // One log line per throw: how far the game moved the hunter under your held view (the
            // most, and when it ended: the glide back), and how far it kept you off the victim.
            static bool throw_logging = false;
            static float held_peak = 0.0f, held_end = -1.0f, push_peak = 0.0f; // game units
            static unsigned surface_hits = 0, silhouette_hits = 0, surface_missing = 0;
            if (own_throw && result.pair_first) {
                if (!surface.hit) ++surface_missing;
                else if (surface.projected) ++silhouette_hits;
                else ++surface_hits;
            }
            if (throw_camera.TakeCut()) {
                LOG_INFO(Core, "VR camera: a throw moved the hunter over {:.0f} m: the view cut to him",
                         VR::GrabCamera::ThrowCamera::MaxM);
            }
            if (throw_camera.Running()) {
                throw_logging = true;
                const float held = VR::Body::Length(throw_camera.Held());
                held_peak = std::max(held_peak, held);
                push_peak = std::max(push_peak, VR::Body::Length(push));
                if (!own_throw && held_end < 0.0f) {
                    held_end = held;
                }
            } else if (throw_logging) {
                const float cm = 100.0f / std::max(scale, 1e-3f);
                LOG_INFO(Core,
                         "VR camera: held your place through a throw (the hunter moved up to {:.0f} "
                         "cm under it, {:.0f} cm when it ended: glided over {:.0f} ms), kept up to "
                         "{:.0f} cm off the victim; surface rays: {} eye-level, {} silhouette, {} unavailable",
                         held_peak * cm, std::max(held_end, 0.0f) * cm,
                         throw_camera.GlideTime() * 1000.0f, push_peak * cm,
                         surface_hits, silhouette_hits, surface_missing);
                throw_logging = false;
                held_peak = 0.0f;
                held_end = -1.0f;
                push_peak = 0.0f;
                surface_hits = silhouette_hits = surface_missing = 0;
            }
        }
        p[0] = base.x + result.throw_offset.x + forward_x + result.render_offset.x;
        p[1] = base.y + result.throw_offset.y + result.render_offset.y;
        p[2] = base.z + result.throw_offset.z + forward_z + result.render_offset.z;
    } else {
        // The old camera (third person, the aim camera, dodge_camera=0). Comfort: low-pass the
        // camera height so rolls, jumps and stairs do not bob the view. The height only advances
        // on the first eye of a stereo pair (with or without smoothing): the second eye keeps it,
        // so the pair has no vertical offset.
        g_comfort_path = false;
        throw_camera.Reset();
        const bool restart = g_legacy_reset; // the comfort camera drove the last frame
        static bool have_height = false;
        static float height = 0.0f;
        static auto last = std::chrono::steady_clock::now();
        const float tau = settings.vertical_smoothing_ms * 0.001f;
        if (!have_height || g_comfort_reset || restart || std::abs(p[1] - height) > 2.0f) {
            // First frame, the first driven frame after the game's own camera (a lever ride, an
            // elevator, a cinematic) or the comfort camera, or a teleport (lantern, cutscene,
            // fall): on either eye, a pair across a teleport is broken anyway.
            height = p[1];
            have_height = true;
            g_comfort_reset = false;
            g_legacy_reset = false;
            last = now;
        } else if (result.pair_first) {
            const float dt =
                std::clamp(std::chrono::duration<float>(now - last).count(), 0.0f, 0.1f);
            last = now;
            height = tau <= 0.0f ? p[1] : height + (p[1] - height) * (1.0f - std::exp(-dt / tau));
        }
        // The horizontal body position is pair-locked too: the second eye of a stereo pair uses
        // the first eye's, so moving (strafing, dodging at up to ~10 m/s) never pulls the two eyes
        // apart or together by the 16.7 ms of travel between them. A teleport resets it.
        static bool have_pair_xz = false;
        static float pair_x = 0.0f, pair_z = 0.0f;
        if (!have_pair_xz || restart || result.pair_first || std::abs(p[0] - pair_x) > 2.0f ||
            std::abs(p[2] - pair_z) > 2.0f) {
            pair_x = p[0];
            pair_z = p[2];
            have_pair_xz = true;
        } else {
            p[0] = pair_x;
            p[2] = pair_z;
        }
        if (fp_follow) {
            // VR_DODGE_LOG: the same dodge statistics for the old camera (A/B runs).
            LegacyEye(eye, VR::Vec3{p[0] - forward_x, height, p[2] - forward_z});
        }
        // Eye position (stereo) and real head movement (6DoF). The render camera is copied
        // from the manager every frame, so nothing has to be undone afterwards.
        p[0] += result.render_offset.x;
        p[1] = height + result.render_offset.y;
        p[2] += result.render_offset.z;
    }
    if (fp_follow) {
        VR::Body::PairLock decision{};
        const bool have_decision = VR::Body::LatestPairLock(decision);
        const VR::Body::Vec3 base{p[0] - forward_x - result.render_offset.x,
                                 p[1] - result.render_offset.y,
                                 p[2] - forward_z - result.render_offset.z};
        const s64 max_age = static_cast<s64>(1.5 * VR::GameFrameSeconds() * 1e9);
        if (const auto locked = grab_position.Step(base, grab, result.pair_first, eye.now_ns,
                                                   settings.pair_lock_position, max_age,
                                                   have_decision ? &decision : nullptr)) {
            p[0] = locked->x + forward_x + result.render_offset.x;
            p[1] = locked->y + result.render_offset.y;
            p[2] = locked->z + forward_z + result.render_offset.z;
        }
    } else {
        grab_position.Reset();
        throw_camera.Reset();
    }
    // Use the actual comfort-smoothed base: compensating the raw root would make its spring
    // drag the view behind a physical step. The game still owns walking and all collision.
    const VR::Vec3 old_offset = result.render_offset;
    const VR::Vec3 walk_base{p[0] - old_offset.x - forward_x - result.throw_offset.x,
                             p[1] - old_offset.y - result.throw_offset.y,
                             p[2] - old_offset.z - forward_z - result.throw_offset.z};
    if (!roomscale_updated)
        VR::UpdateRoomscaleWalk(result, walk_base, fp_follow);
    p[0] += result.render_offset.x - old_offset.x;
    p[2] += result.render_offset.z - old_offset.z;
    g_last = result;
    std::memcpy(render + OffsetPosition, p, sizeof(p));
    VR::SetRecordRenderPosition(result.record_id, VR::Vec3{p[0], p[1], p[2]});
    const float stage_tracked[3]{forward_x + result.render_offset.x, result.render_offset.y,
                                 forward_z + result.render_offset.z};
    TraceVisceralCamera(result.record_id, p, stage_tracked);
    // The late warp: the eye forward of the head centre went in (first person only).
    VR::SetRecordEyeForward(result.record_id, fp_follow ? eye_forward_m : 0.0f, fp_follow);
    // What the game projects this frame with (after our FOV and near-plane writes): DIBR stereo
    // turns the frame's depth into disparity with exactly these values.
    float proj[4];
    std::memcpy(&proj[0], render + OffsetFov, sizeof(float));
    std::memcpy(&proj[1], render + OffsetAspect, sizeof(float));
    std::memcpy(&proj[2], render + OffsetNear, sizeof(float));
    std::memcpy(&proj[3], render + OffsetFar, sizeof(float));
    VR::SetRecordProjection(result.record_id, proj[0], proj[1], proj[2], proj[3]);
    // inframe_hand_sync: this update's carry, for the producers below (HandSyncBegin).
    HandSyncBegin(result.record_id, reinterpret_cast<const float*>(render + OffsetMatrix), p);
    // The game's vial in the hunter's hand: its depth proxy in this frame's view (DIBR).
    HeldRenderTick(result.record_id, reinterpret_cast<const float*>(render + OffsetMatrix), p);
    // The Blood Gauge on the hunter's left wrist (hud_vitals=wrist): its own in-frame list.
    VitalsRenderTick(result.record_id, reinterpret_cast<const float*>(render + OffsetMatrix), p);
    // Quick items: the arc's placement and hover, and its in-frame draw list for this camera.
    QuickItemsRenderTick(result.record_id, reinterpret_cast<const float*>(render + OffsetMatrix),
                         p);
    // inframe_hand_sync: what sits on his hand moves on with your play space since the update his
    // pose was placed for.
    HandSyncTick(result.record_id);
    // VR body: the anchor of this frame's view (the final position, after comfort smoothing) for
    // the next frame's pose copy.
    BodyRenderTick(&result, p, fp_follow);
    EvasionRenderTick(&result, fp_follow);
    if (fp_follow) {
        SpectatorView(render, result, p, scale);
    }
    final_visceral_view.camera = render;
    final_visceral_view.record = result.record_id;
    u8* effect_controller{};
    std::memcpy(&effect_controller, field_camera + 0x110, sizeof(effect_controller));
    visceral_effect_record.store(result.record_id, std::memory_order_relaxed);
    visceral_effect_controller.store(effect_controller, std::memory_order_release);
    final_visceral_view.hold = staged;
    std::memcpy(final_visceral_view.view.data(), render + OffsetMatrix,
                final_visceral_view.view.size());
    // Read-only orientation trace, including after stage release. Correlate with
    // the actual draw camera by record, rather than inferring it from an effect.
    const float* basis = reinterpret_cast<const float*>(render + OffsetMatrix);
    TraceVisceralView("render_right", result.record_id, basis, basis);
    TraceVisceralView("render_up", result.record_id, basis+4, basis+4);
    TraceVisceralView("render_forward", result.record_id, basis+8, basis+8);
    VR::CameraRecord trace_record{};
    static const bool trace_orientation = std::getenv("VR_VISCERAL_STAGE_TRACE") != nullptr;
    if (trace_orientation && VR::GetRecord(result.record_id, trace_record)) {
        const float head_angles[3]{trace_record.head_yaw,trace_record.head_pitch,trace_record.head_roll};
        const float view_angles[3]{result.view_yaw,result.render_pitch,result.view_roll};
        TraceVisceralView("head_view_angles", result.record_id, head_angles, view_angles);
        const float heading[3]{result.base_yaw,result.yaw,result.vfov};
        const float game_heading[3]{g_follow_yaw,g_follow_pitch,g_follow_fov};
        TraceVisceralView("heading", result.record_id, heading, game_heading);
    }
    const float head_position[3]{result.head_position.x,result.head_position.y,result.head_position.z};
    const float head_offset[3]{result.render_offset.x,result.render_offset.y,result.render_offset.z};
    TraceVisceralView("head", result.record_id, head_position, head_offset);
    if (g_shake.on) {
        auto& s = g_shake;
        s.written_render = render;
        std::memcpy(s.written_m, render + OffsetMatrix, sizeof(s.written_m));
        std::memcpy(s.written_p, render + OffsetPosition, sizeof(s.written_p));
        const auto rows = VR::Shake::FromMatrix(s.written_m);
        if (s.have_eye) {
            s.eye_rot = std::max(s.eye_rot, VR::Shake::AngleDeg(rows, s.eye_rows));
            s.eye_pos = std::max(s.eye_pos, VR::Shake::Distance(s.written_p, s.eye_p));
            // The change of the eye's step from one frame to the next: smooth travel (a knock-back,
            // walking) stays small, a shake does not.
            const float v[3] = {s.written_p[0] - s.eye_p[0], s.written_p[1] - s.eye_p[1],
                                s.written_p[2] - s.eye_p[2]};
            s.eye_jerk = std::max(s.eye_jerk, VR::Shake::Distance(v, s.eye_v));
            std::memcpy(s.eye_v, v, sizeof(v));
            // The same for the view direction (forward and up rows): a smooth turn (lock-on, a snap
            // turn's glide) stays small, a shake does not. Degrees (small angles).
            float fv[6];
            for (int i = 0; i < 3; ++i) {
                fv[i] = rows.r[2][i] - s.eye_rows.r[2][i];
                fv[3 + i] = rows.r[1][i] - s.eye_rows.r[1][i];
            }
            s.eye_rjerk = std::max(s.eye_rjerk,
                                   std::max(VR::Shake::Distance(fv, s.eye_fv),
                                            VR::Shake::Distance(fv + 3, s.eye_fv + 3)) *
                                       57.2957795f);
            std::memcpy(s.eye_fv, fv, sizeof(fv));
        }
        s.eye_rows = rows;
        std::memcpy(s.eye_p, s.written_p, sizeof(s.eye_p));
        s.have_eye = true;
    }

    if (now - g_last_log > std::chrono::seconds(5)) {
        g_last_log = now;
        float near_plane{};
        float far_plane{};
        std::memcpy(&near_plane, render + OffsetNear, sizeof(near_plane));
        std::memcpy(&far_plane, render + OffsetFar, sizeof(far_plane));
        LOG_INFO(Core,
                 "VR camera: mode={} follow hits={} view yaw={:.2f} pitch={:.2f} roll={:.2f} "
                 "render pitch={:.2f} fov={:.1f}deg aspect={:.4f} near={:.2f} far={:.1f} first "
                 "person={}",
                 mode, g_hits, result.view_yaw, result.view_pitch, result.view_roll,
                 result.render_pitch, result.vfov * 57.2957795f, aspect, near_plane, far_plane,
                 VR::IsFirstPerson());
    }
}

void PS4_SYSV_ABI FovHook(u8* camera, u8* saved_ymm) {
    static bool logged = false;
    if (!logged) {
        logged = true;
        LOG_INFO(Core, "VR: first FOV hook call, camera={} thread '{}'", fmt::ptr(camera),
                 Common::GetCurrentThreadName());
    }
    const float fov = g_fov_override.load(std::memory_order_relaxed);
    if (fov > 0.0f) {
        // Target FOV lives in xmm1 (saved at ymm slot 1); snapping the current value too
        // removes the game's FOV easing so the projection matches the headset at once.
        std::memcpy(saved_ymm + 32 * 1, &fov, sizeof(fov));
        std::memcpy(camera + OffsetFov, &fov, sizeof(fov));
    } else if (g_fov_snap) {
        // Cinematic flat mode: the game's FOV at once instead of a slow zoom out of the VR FOV.
        g_fov_snap = false;
        std::memcpy(camera + OffsetFov, saved_ymm + 32 * 1, sizeof(float));
    }
}

std::vector<StoreSite> FindStoreSites(u8* start, u8* end, u8* skip_begin, u8* skip_end) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);

    std::vector<StoreSite> sites;
    u8* p = start;
    while (p < end) {
        ZydisDecodedInstruction instruction;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, p, static_cast<ZyanUSize>(end - p),
                                                 &instruction, operands))) {
            ++p;
            continue;
        }
        for (u32 i = 0; i < instruction.operand_count_visible; ++i) {
            const auto& op = operands[i];
            if (op.type != ZYDIS_OPERAND_TYPE_MEMORY ||
                !(op.actions & ZYDIS_OPERAND_ACTION_MASK_WRITE) ||
                op.mem.base != ZYDIS_REGISTER_R13 || op.mem.index != ZYDIS_REGISTER_NONE ||
                op.size != 32) {
                continue;
            }
            const auto disp = op.mem.disp.value;
            if (disp != OffsetPitch && disp != OffsetYaw && disp != OffsetPitchCopy) {
                continue;
            }
            if (p >= skip_begin && p < skip_end) {
                continue;
            }
            StoreSite site{p, instruction.length, {}};
            std::memcpy(site.original.data(), p, instruction.length);
            sites.push_back(site);
        }
        p += instruction.length;
    }
    return sites;
}

} // Anonymous namespace

void OnEbootLoaded(VAddr base, u64 size) {
    if (!VR::IsEnabled() || g_installed) {
        return;
    }
    // Instant Blood Vial injection and the cinematic detection share their own hook and do not
    // depend on the camera. The body's pose hook is installed after relocation (its vtable slots
    // are filled then); only its VR_BODY_HOOK=code variant patches code here.
    InstallPlayerHook(base, size);
    InstallHeld(base, size); // the armed vial in the hunter's own hand (checks only, no patch)
    InstallQuickItems(base, size); // quick items in VR (the arc, uses, throws)
    InstallMelee(base, size); // physical melee: the swing is the attack (before the damage hook)
    InstallTransform(base, size); // the weapon-only transform (after the player hook: site A)
    InstallPose(base, size); // physical gestures: the Brain's and the Doll's markers (site A)
    InstallWhip(base, size);      // the physical whip (whip=physical: a whip form's chain)
    InstallTuning(base, size);
    InstallRagdoll(base, size); // the kill ragdoll (after the damage detour it rides on)
    InstallGun(base, size); // the gun: shots along the barrel, the bang at the pull
    InstallIframes(base, size);
    InstallShoulderEquipment(base, size);
    InitVisceralPull(base, size); // the interactive visceral (rides on the stagger detour)
    InstallStaggerSpeed(base, size); // shorter hit staggers (stagger_shorten_ms)
    ConfirmVisceralPull(IsStaggerSpeedInstalled(), IsOffhandDamageInstalled());
    InstallFacing(base, size); // the hunter strafes: only turns and your heading turn him
    InstallLeverProbe(base, size); // physical levers; live setting controls hand interaction
    InstallPickup(base, size); // item pickups: the pop-up without the pickup animation
    MenuEbootLoaded(base, size);
    LampsEbootLoaded(base, size);
    InstallEnemyBars(base, size); // ui_enemy_hp_bars: the enemies' HP bars on / off, live
    InstallLights(base, size); // body_fill_lights: camera-attached fill lights follow the hunter
    InstallStatus(base, size); // status build-ups: the faster drain and the meter's rows
    EvasionEbootLoaded(base, size);
    BodyEbootLoaded(base, size);
    if (size < CameraFuncEnd) {
        return;
    }

    auto* const module = reinterpret_cast<u8*>(base);
    u8* const camera_site = module + CameraHookRva;
    u8* const fov_site = module + FovHookRva;
    if (std::memcmp(camera_site, CameraHookSig.data(), CameraHookSig.size()) != 0 ||
        std::memcmp(fov_site, FovHookSig.data(), FovHookSig.size()) != 0) {
        LOG_INFO(Core, "VR: Bloodborne 1.09 camera signatures not found; camera bridge disabled");
        return;
    }

    auto sites = FindStoreSites(module + CameraFuncStart, module + CameraFuncEnd, camera_site,
                                camera_site + CameraHookSig.size());
    if (sites.size() < 16 || sites.size() > 64) {
        LOG_ERROR(Core, "VR: unexpected number of camera store sites ({}); bridge disabled",
                  sites.size());
        return;
    }

    if (!MakeWritable(module + CameraFuncStart, CameraFuncEnd - CameraFuncStart)) {
        LOG_ERROR(Core, "VR: could not make the camera code writable; bridge disabled");
        return;
    }

    try {
        g_code = std::make_unique<Xbyak::CodeGenerator>(8192);
        using Xbyak::util::r13;
        using Xbyak::util::rbx;
        void* camera_stub = EmitHostCallStub(
            *g_code, reinterpret_cast<void*>(&CameraHook), CameraHookSig.data(),
            CameraHookSig.size(), reinterpret_cast<u64>(camera_site + CameraHookSig.size()), r13);
        void* fov_stub =
            EmitHostCallStub(*g_code, reinterpret_cast<void*>(&FovHook), FovHookSig.data(),
                             FovHookLen, reinterpret_cast<u64>(fov_site + FovHookLen), r13);
        g_store_sites = std::move(sites);
        WriteJump(camera_site, CameraHookSig.size(), camera_stub);
        WriteJump(fov_site, FovHookLen, fov_stub);

        // Decoupled view: replace the render orientation after the update returns.
        u8* const post1 = module + PostHookRva1;
        u8* const post2 = module + PostHookRva2;
        if (std::memcmp(post1, PostHookSig1.data(), PostHookSig1.size()) == 0 &&
            std::memcmp(post2, PostHookSig2.data(), PostHookSig2.size()) == 0 &&
            MakeWritable(post1, 0x400)) {
            void* stub1 = EmitHostCallStub(*g_code, reinterpret_cast<void*>(&CameraPostHook),
                                           PostHookSig1.data(), PostHookSig1.size(),
                                           reinterpret_cast<u64>(post1 + PostHookSig1.size()), rbx);
            void* stub2 = EmitHostCallStub(*g_code, reinterpret_cast<void*>(&CameraPostHook),
                                           PostHookSig2.data(), PostHookSig2.size(),
                                           reinterpret_cast<u64>(post2 + PostHookSig2.size()), rbx);
            WriteJump(post1, PostHookSig1.size(), stub1);
            WriteJump(post2, PostHookSig2.size(), stub2);
            g_post_hooks = true;

            u8* const render = module + RenderHookRva;
            if (size > RenderHookRva + 0x100 &&
                std::memcmp(render, RenderHookSig.data(), RenderHookSig.size()) == 0 &&
                MakeWritable(render, RenderHookSig.size())) {
                using Xbyak::util::r15;
                void* render_stub = EmitHostCallStub(
                    *g_code, reinterpret_cast<void*>(&RenderCameraHook), RenderHookSig.data(),
                    RenderHookSig.size(), reinterpret_cast<u64>(render + RenderHookSig.size()),
                    r13, &r15);
                WriteJump(render, RenderHookSig.size(), render_stub);
                g_render_hook = true;
                u8* const final_site = module + RenderFinalRva;
                if (std::memcmp(final_site, RenderFinalSig.data(), RenderFinalSig.size()) == 0 &&
                    MakeWritable(final_site, RenderFinalSig.size())) {
                    void* final_stub = EmitHostCallStub(*g_code,
                        reinterpret_cast<void*>(&RenderFinalHook), RenderFinalSig.data(),
                        RenderFinalSig.size(), reinterpret_cast<u64>(final_site + RenderFinalSig.size()), r15);
                    WriteJump(final_site, RenderFinalSig.size(), final_stub);
                    LOG_INFO(Core, "VR visceral: final camera guard installed after native override copy");
                } else {
                    LOG_ERROR(Core, "VR visceral: final camera guard signature mismatch");
                }
                u8* const effect_site = module + CameraEffectRva;
                {
                    u8* const source_site = module + CameraEffectSourceRva;
                    if (std::memcmp(source_site, CameraEffectSourceSig.data(), CameraEffectSourceSig.size()) == 0 &&
                        MakeWritable(source_site, CameraEffectSourceSig.size())) {
                        void* source_stub = Hook::EmitHostCallStubWith(*g_code,
                            reinterpret_cast<void*>(&VisceralCameraEffectSourceHook),
                            CameraEffectSourceSig.data(), CameraEffectSourceSig.size(),
                            reinterpret_cast<u64>(source_site + CameraEffectSourceSig.size()),
                            [](Xbyak::CodeGenerator& c) {
                                using namespace Xbyak::util;
                                c.mov(rdi, rbx);
                                c.lea(rsi, ptr[rbp - 0x60]);
                            });
                        WriteJump(source_site, CameraEffectSourceSig.size(), source_stub);
                        LOG_INFO(Core, "VR visceral: middle and late native kick attenuation installed (0.1% excess)");
                    } else {
                        LOG_ERROR(Core, "VR visceral: individual camera-effect trace signature mismatch");
                    }
                }
                if (std::memcmp(effect_site, CameraEffectSig.data(), CameraEffectSig.size()) == 0 &&
                    MakeWritable(effect_site, CameraEffectSig.size())) {
                    void* effect_stub = Hook::EmitHostCallStubWith(*g_code,
                        reinterpret_cast<void*>(&VisceralCameraEffectHook), CameraEffectSig.data(),
                        CameraEffectSig.size(), reinterpret_cast<u64>(effect_site + CameraEffectSig.size()),
                        [](Xbyak::CodeGenerator& c) {
                            using namespace Xbyak::util;
                            c.mov(rdi, r12);
                            c.lea(rsi, ptr[rbp - 0x80]);
                        });
                    WriteJump(effect_site, CameraEffectSig.size(), effect_stub);
                    LOG_INFO(Core, "VR visceral: native lunge 20%, matrix shake 50% installed at 0x1571C9D");
                } else {
                    LOG_ERROR(Core, "VR visceral: native camera-effect signature mismatch");
                }
            } else {
                LOG_WARNING(Core, "VR: render-camera signature not found; first-person mods "
                                  "will not follow the headset");
            }
        } else {
            LOG_WARNING(Core, "VR: camera post-update signatures not found; view stays coupled");
        }
    } catch (const Xbyak::Error& e) {
        LOG_ERROR(Core, "VR: failed to generate camera hooks: {}", e.what());
        return;
    }

    g_installed = true;
    const bool first_person =
        std::memcmp(module + FirstPersonModRva, FirstPersonModSig.data(),
                    FirstPersonModSig.size()) == 0;
    VR::SetFirstPersonDetected(first_person);
    LOG_INFO(Core, "VR: first-person camera mod {} (first person {})",
             first_person ? "detected" : "not detected", VR::IsFirstPerson() ? "on" : "off");
    LOG_INFO(Core,
             "VR: Bloodborne camera bridge installed ({} store sites, camera {}, fov {}, "
             "decoupled view {}, render camera {})",
             g_store_sites.size(), fmt::ptr(camera_site), fmt::ptr(fov_site), g_post_hooks,
             g_render_hook);
}

bool IsInstalled() {
    return g_installed;
}

} // namespace VR::Bloodborne
