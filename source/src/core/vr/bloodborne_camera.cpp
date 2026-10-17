// SPDX-License-Identifier: GPL-2.0-or-later

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
#include "core/vr/interaction_native_camera.h"
#include "core/vr/bloodborne_enemy_bars.h"
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
VR::Interactions::Module* g_camera_module{};
bool g_camera_fault{};
VR::Interactions::NativeCameraRuntime g_native_camera;
bool InitNativeCameraDll() {
    if (!VR::Interactions::DllRequested())
        return true;
    if (g_camera_module)
        return !g_camera_fault && !g_native_camera.Faulted();
    g_camera_module = new VR::Interactions::Module();
    g_camera_module->EnableFaultBroadcast();
    if (!g_camera_module->LoadBesideExecutable()) {
        g_camera_fault = true;
        LOG_ERROR(Core, "VR camera: interaction DLL unavailable: {}",
                  g_camera_module->Diagnostic());
        return false;
    }
    g_native_camera.Bind(*g_camera_module, g_camera_fault);
    return true;
}
template <class V>
void CameraPut(float (&out)[3], const V& v) {
    out[0] = v.x;
    out[1] = v.y;
    out[2] = v.z;
}
VR::Body::Vec3 CameraVec(const float (&v)[3]) {
    return {v[0], v[1], v[2]};
}
// The render hook ran without driving the camera: the next driven frame restarts the camera
// filters, height and (comfort camera) body position, so after a lever ride, an elevator or a
// cinematic the eye never glides.

// Which first-person camera the last driven frame used (the comfort camera, dodge_camera=1, or the
// old one); the other restarts when it takes over.


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

bool CameraInteractionFaulted() {
    if (!g_native_camera.Faulted())
        return false;
    // Same neutral publication as a missing/inactive camera, without a local policy retry.
    BodyRenderTick(nullptr, nullptr, false);
    EvasionRenderTick(nullptr, false);
    return true;
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
void PS4_SYSV_ABI RenderCameraHook(u8* field_camera, u8* /*saved_ymm*/, u8* render) {
    if (CameraInteractionFaulted())
        return;
    u8* manager = nullptr;
    std::memcpy(&manager, field_camera + OffsetFieldManager, sizeof(manager));
    const bool follow = g_follow_seen;
    g_follow_seen = false;
    if (manager == nullptr || render == nullptr) {
        g_native_camera.Path(PI_CP_MISSING);
        BodyRenderTick(nullptr, nullptr, false);
        EvasionRenderTick(nullptr, false);
        return;
    }
    if (RemoCutscene()) {
        g_native_camera.Path(PI_CP_INACTIVE);
        // PI_CP_INACTIVE also resets the DLL-owned root-height hold.
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
            g_native_camera.Path(PI_CP_INACTIVE);
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
        g_native_camera.Path(PI_CP_INACTIVE);
        BodyRenderTick(nullptr, nullptr, false);
        EvasionRenderTick(nullptr, false);
        return; // cutscene or scripted camera: left to the game (virtual screen)
    }
    g_last = result;
    ++g_render_ticks;
    if (!result.active) {
        g_native_camera.Path(PI_CP_INACTIVE);
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
    PiNativeCameraInput geometry_input{};
    geometry_input.operation = PI_NC_GEOMETRY;
    geometry_input.scale = scale;
    auto& geometry_facts = geometry_input.geometry;
    geometry_facts.fp_follow = fp_follow;
    geometry_facts.view_yaw = result.view_yaw;
    geometry_facts.view_pitch = result.view_pitch;
    geometry_facts.forward_m = settings.fp_forward_m;
    geometry_facts.near_m = settings.fp_near_m;
    if (fp_follow) {
        geometry_facts.ahead_m = VR::Tune::Get("fp.ahead_m", settings.fp_ahead_m);
        geometry_facts.ahead_down_m = VR::Tune::Get("fp.ahead_down_m", settings.fp_ahead_down_m);
        if (settings.fp_near_m > 0.f)
            geometry_facts.near_down_m = VR::Tune::Get("fp.near_down_m", settings.fp_near_down_m);
    }
    const auto geometry = g_native_camera.Call(geometry_input).geometry;
    if (CameraInteractionFaulted())
        return;
    const float eye_forward_m = geometry.eye_forward_m;
    const float forward_x = geometry.forward_x, forward_z = geometry.forward_z;
    EyeTick eye{};
    eye.now_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    bool roomscale_updated = false;
    VR::GrabCamera::Sample grab_sample{};
    const bool sampled = fp_follow && BodyGrabEye(eye.now_ns, grab_sample, g_native_camera);
    const bool own_throw = sampled && grab_sample.own_throw;
    const VR::Vec3 grab_eye{grab_sample.eye.x, grab_sample.eye.y, grab_sample.eye.z};
    const bool grab = sampled && !own_throw;
    const auto path = g_native_camera.Path(PI_CP_SELECT, fp_follow, grab, settings.dodge_camera);
    if (CameraInteractionFaulted())
        return;
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

        const bool have_model = BodyRootHeight(root_y, &root_yaw);
        root_y = g_native_camera.RootHeight(phys_root, root_y, root_yaw, scale, have_model, hold_on)
                     .height;
        if (CameraInteractionFaulted())
            return;
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
        if (geometry.write_near)
            std::memcpy(render + OffsetNear, &geometry.near_plane, sizeof(geometry.near_plane));
    }

    if (path.path == PI_CAMERA_GRAB) {
        // Follow the animated body even when the physics capsule stays on the ground.
        // Head look, physical movement and stereo remain live.
        p[0] = grab_eye.x + forward_x + result.render_offset.x;
        p[1] = grab_eye.y + result.render_offset.y;
        p[2] = grab_eye.z + forward_z + result.render_offset.z;
    } else if (path.path == PI_CAMERA_COMFORT) {
        // Dodge comfort camera (core/vr/motion_camera.h): the body position is followed with a
        // critically damped spring horizontally and a mode-dependent one vertically, pair-locked
        // for both eyes of a stereo pair; the forward offset, the stereo eye and real head
        // movement are added after it, unfiltered.
        const char* reset = path.reset_reason
                                ? PhysicalInteractions::MotionPackets::Reason(path.reset_reason)
                                : nullptr;
        const VR::Vec3 base = ComfortEye(eye, reset);
        // Resolve physical walking before the throw clearance check so it sees the same
        // tracked offset that will actually be rendered.
        VR::UpdateRoomscaleWalk(result, base, fp_follow);
        roomscale_updated = true;
        // A freehand throw (a visceral, riposte, backstab): the view holds your place, your head
        // and hands tracked, and keeps off the victim; it glides back onto him when the throw
        // ends (the same for both eyes of a pair; grab_camera.h ThrowCamera). Check the actual
        // rendered head centre: include the forward lead and exclude only stereo separation.
        const VR::Body::Vec3 head{result.render_offset.x - result.eye_offset.x + forward_x,
                                  result.render_offset.y - result.eye_offset.y,
                                  result.render_offset.z - result.eye_offset.z + forward_z};
        const VR::Body::Vec3 base_b{base.x, base.y, base.z};
        PiNativeCameraInput throw_input{};
        throw_input.operation = PI_NC_THROW;
        throw_input.throwing = own_throw;
        throw_input.first = result.pair_first;
        throw_input.now_ns = eye.now_ns;
        throw_input.scale = scale;
        throw_input.victim_valid = grab_sample.victim_valid;
        CameraPut(throw_input.base, base_b);
        CameraPut(throw_input.facing, grab_sample.facing);
        CameraPut(throw_input.head, head);
        CameraPut(throw_input.victim, grab_sample.victim);
        const auto throw_state = g_native_camera.Call(throw_input);
        if (CameraInteractionFaulted())
            return;
        const auto hold = CameraVec(throw_state.hold);
        const auto push = CameraVec(throw_state.push);
        result.throw_offset = {hold.x + push.x, hold.y + push.y, hold.z + push.z};
        // One log line per throw: how far the game moved the hunter under your held view (the
        // most, and when it ended: the glide back), and how far it kept you off the victim.
        static bool throw_logging = false;
        static float held_peak = 0.0f, held_end = -1.0f, push_peak = 0.0f; // game units
        if (throw_state.cut) {
            LOG_INFO(Core, "VR camera: a throw moved the hunter over {:.0f} m: the view cut to him",
                     VR::GrabCamera::ThrowCamera::MaxM);
        }
        if (throw_state.running) {
            throw_logging = true;
            const float held = VR::Body::Length(CameraVec(throw_state.held));
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
                     "{:.0f} cm off the victim",
                     held_peak * cm, std::max(held_end, 0.0f) * cm,
                     throw_state.glide_s * 1000.0f, push_peak * cm);
            throw_logging = false;
            held_peak = 0.0f;
            held_end = -1.0f;
            push_peak = 0.0f;
        }
        p[0] = base.x + result.throw_offset.x + forward_x + result.render_offset.x;
        p[1] = base.y + result.throw_offset.y + result.render_offset.y;
        p[2] = base.z + result.throw_offset.z + forward_z + result.render_offset.z;
    } else {
        // The old camera (third person, the aim camera, dodge_camera=0). Comfort: low-pass the
        // camera height so rolls, jumps and stairs do not bob the view. The height only advances
        // on the first eye of a stereo pair (with or without smoothing): the second eye keeps it,
        // so the pair has no vertical offset.
        PiNativeCameraInput legacy{};
        legacy.operation = PI_NC_LEGACY_OWNED;
        legacy.now_ns =
            std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
        legacy.first = result.pair_first;
        legacy.smoothing_ms = settings.vertical_smoothing_ms;
        std::copy_n(p, 3, legacy.base);
        const auto legacy_state = g_native_camera.Call(legacy);
        if (CameraInteractionFaulted() || !legacy_state.applied)
            return;
        std::copy_n(legacy_state.position, 3, p);
        const float height = p[1];
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
        PiNativeCameraInput pair{};
        pair.operation = PI_NC_PAIR;
        pair.now_ns = eye.now_ns;
        pair.max_age_ns = max_age;
        pair.grabbing = grab;
        pair.first = result.pair_first;
        pair.lock_enabled = settings.pair_lock_position;
        pair.have_decision = have_decision;
        if (have_decision) {
            pair.first_ns = decision.first_ns;
            pair.decided_ns = decision.decided_ns;
            pair.decision_locked = decision.locked;
        }
        CameraPut(pair.base, base);
        const auto locked = g_native_camera.Call(pair);
        if (CameraInteractionFaulted())
            return;
        if (locked.applied) {
            p[0] = locked.position[0] + forward_x + result.render_offset.x;
            p[1] = locked.position[1] + result.render_offset.y;
            p[2] = locked.position[2] + forward_z + result.render_offset.z;
        }
    }
    if (CameraInteractionFaulted())
        return;
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
    if (!InitNativeCameraDll()) return;
    InstallVitalsVisual();
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
        g_code = std::make_unique<Xbyak::CodeGenerator>(4096);
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
