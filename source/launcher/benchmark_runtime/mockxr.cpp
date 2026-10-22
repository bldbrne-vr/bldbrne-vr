// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// MockXR: a minimal headless OpenXR runtime used to exercise shadPS4's VR presenter
// without a headset. It creates real Vulkan swapchain images on the application's
// device, animates a head pose, validates submitted layers, and dumps the submitted eye
// images to PNG (MOCKXR_DUMP_DIR) so stereo output can be inspected.
//
// Use: set XR_RUNTIME_JSON=<dir>\mockxr.json for the test process only.
// MOCKXR_QUEST_FOV=1: report Quest 3's per-eye FOV (left eye L-54/R+40/U+44/D-55 degrees,
// right eye mirrored) with fovMutable = false, like Meta Link. Every projection layer is then
// checked against it: a submitted FOV that differs from the located one is logged as an error.
// Eye dumps contain only the submitted imageRect of each view.
//
// VR body test tooling (src/core/vr/body_presets.h, shared with the emulator's --vr-sim hands):
// MOCKXR_HANDS=1|poses|all or a comma-separated list of preset names (rest, still, guard,
// blade_level, reach_fwd, out_of_reach, high, cross, over_shoulder, behind_head, roll_sweep,
// wrist_inject, swing, lost): xrLocateSpace returns the scripted grip for the grip and aim spaces
// of both hands (aim = grip), with finite-difference linear and angular velocities, and the
// timeline (3 s a segment, looping) starts at the first projection frame after 1200; before that
// both hands rest. The log says `MOCKXR seg <name>` at every segment. The `swing` segment scripts
// a fast swing; `lost` clears the right grip's position-tracked bit (position inferred) for 0.5 s.
// The presets' right grip squeeze ("weapon drawn" in the old clutch layout) is only applied with
// MOCKXR_PRESET_GRIP=1 (also MOCKXR_INPUT's): the emulator's right grip now transforms the weapon
// (L1), and swings attack without it (vr.ini right_grip=draw restores the clutch). MOCKXR_HAND_BLEND=<s>: each segment moves the grips from where
// the previous one left them over <s> s (minimum jerk) instead of jumping.
// MOCKXR_LEFT_PATH=<t>:<dx>,<dy>,<dz>;...: the left grip moved by these offsets from its scripted
// pose, keyframes at <t> s on the hands timeline with minimum jerk between them (quick-item tests:
// reach for a card, then swing and let go; velocities follow by finite differences).
// MOCKXR_HAND_SHIFT=x,y,z: every scripted grip moved by that many metres (LOCAL space), e.g.
// 0,0.1,0.15 holds the presets 10 cm higher and 15 cm nearer the face (near-field tests).
// Height tests (a player profile): MOCKXR_HAND_SCALE=<k> scales every scripted grip's offset from
// (0, 1.6, 0) by k (the presets are a ~171 cm body), MOCKXR_FLOOR=<y> puts the floor (STAGE and
// LOCAL_FLOOR, listed by xrEnumerateReferenceSpaces) at y in LOCAL; MOCKXR_NO_FLOOR=1 lists none,
// MOCKXR_NO_LOCAL_FLOOR=1 only STAGE (see FloorY below).
// MOCKXR_HEAD=pitch:<deg>[,yaw:<deg>][,roll:<deg>][,swing:<deg>,period:<s>[,neck:1]][,step:<deg>]: a fixed head
// orientation at (0, 1.6, 0) (default: the slow sine motion; MOCKXR_STATIC=1: looking straight
// ahead). roll tilts the head about the view axis (+ = right side up); swing turns it +-<deg> in a
// sine of <period> s, about world up, or with neck:1 about the pitched head's own up (looking down
// and turning the head to a hand on one side: the eyes' baseline tilts). nod:<deg> moves the pitch
// +-<deg> around `pitch` in a sine of the same period (a nod through the fp_near_down_m ramp);
// nodrate:<n> nods n times per period instead (the launcher scan's look-around: swing:25,nod:6,
// nodrate:2,period:5 looks left and right, dipping down and up on the way: a figure of eight).
// MOCKXR_DUMP_SEGMENTS=1: dumps both eyes 1.5 s into each segment of the first pass as
// <MOCKXR_DUMP_DIR>\<segment>_L.png / _R.png. MOCKXR_MARK_HANDS=1: a magenta cross at each grip's
// projection in every dumped eye (the submitted view pose, FOV and image rect).
// MOCKXR_STAMPS=1 (with the emulator's VR_STAMP=1): every projection frame, reads the 16-bit frame
// stamps (rows of 16 black/white 4x4 cells along the bottom-left edge of each view's imageRect) of
// the image each view shows and logs `MOCKXR stamp f=<projection frame> v0=<left> v1=<right>
// img0=.. img1=.. c0=.. c1=.. s0=.. s1=.. x0=..`: v = the camera record the image was submitted
// with (bottom row), c = the record its flip was tagged with (the row above), s = its publish
// serial (the third row), x = the record current at the frame's composite (the top row, left view).
// The readback is queued at xrEndFrame and decoded a few frames later (no stall).
// MOCKXR_ASSOC=1 (with MOCKXR_STAMPS): the stamp line also gets `reg=<x>`: 64 world-fixed directions
// (an 8x8 grid over the central 40x30 degrees of the left eye at the frame's display pose) are looked
// up in the submitted left image through its submitted pose and FOV, like a rotation-only compositor,
// and reg is their mean absolute colour change (0..255) against the previous projection frame. With
// a still head and world it stays near 0 when every frame is submitted with the pose it was rendered
// for; a frame submitted with another pose jumps (emulator VR_LAG_TEST=2: 80 degrees).
// MOCKXR_SIG=1 (with MOCKXR_STAMPS): the stamp line also gets `sig=<128 hex digits>`: the luma of
// 64 image-space samples (an 8x8 grid over the central 60% of the left view's imageRect) of the image
// it shows. With a still head and world, images rendered from the same camera look alike, so the
// emulator's VR_LAG_TEST=4 (four headings by camera record id % 4) lets a script tell from the
// image which camera update rendered it (the signatures cluster by heading), against the record it
// was submitted with.
// MOCKXR_TRACK=1 (judder measurement): every projection frame, three patches of what the left eye
// sees through a rotation-only compositor (the submitted left view reprojected to the eye at the
// frame's display pose, no depth: Link without positional timewarp) are appended to
// <MOCKXR_DUMP_DIR>\track.bin, read back without a stall (collected a few frames later). A record:
// u64 projection frame, i64 display time (ns), then 3 x 160x160 luma bytes (row-major; 0 where the
// patch left the image). The patches, left eye, in tangent space at the eye images' 532 px per unit:
// far (centre, above the horizon: tan x -0.15..0.15, y 0.40..0.10), floor (centre, below it: y
// -0.25..-0.55) and hand (lower right, the hunter's right hand and weapon: x 0.30..0.60, y
// -0.70..-1.00). latewarp/judder.py measures each patch's motion from one refresh to the next.
// MOCKXR_RATES=<hz>[,<hz>...]: the refresh rates offered (default 72,90,120), like Link offering
// only the rate chosen in the Meta Quest Link app; the first frame period is 90 Hz when offered, else
// the highest one listed.
// MOCKXR_LATCH_MS=<ms>: a compositor latch. Every swapchain release submits a fence (signalled when
// all GPU work queued before the release is done); <ms> before each projection frame's display time
// a latch thread checks the fences of the images its views show, and a view whose image is not
// finished then counts as late (the compositor would show the previous image): logged as
// `latch late=<views> of <checked>` with the frame counts.
// MOCKXR_DUMP_EVERY=<n>: the eye dumps (MOCKXR_DUMPS) every <n> projection frames (default 200).
// MOCKXR_DUMP_ASYNC=1: dumps never stall the application (the copy is collected frames later),
// e.g. for dumps of consecutive game frames.
// XR_KHR_composition_layer_depth is listed (MOCKXR_NO_DEPTH=1 hides it) with D32_SFLOAT and
// D16_UNORM depth swapchains. A depth info chained on a projection view is validated (a released
// depth swapchain, imageRect inside it and the colour rect's size, 0 <= minDepth < maxDepth <= 1,
// finite positive nearZ != farZ; ERROR lines) and counted (`depth=` in the frame counts, `MOCKXR
// depth on|off` when it starts or stops). Every eye dump with depth also writes
// eye<N>_depth_L/R.png (white = 0.5 m, black = far) plus the decoded metres as eye<N>_depth_L/R.f32
// (u32 width, u32 height, floats) and a `depth dump` line with the distances at the centre and four
// points around it.
// MOCKXR_STEREO_SWITCH=<seconds>: once the game shows projection frames, every <seconds> holds
// Menu for 0.8 s and pushes the left stick right in the middle of it (the in-VR stereo mode
// switch: mono -> AER -> DIBR -> mono); haptics are logged (`haptic ...`, first 400;
// MOCKXR_HAPTIC_LOG=<n>: the first n in any run, default 40). Every vibration the controller would
// play is logged as it ends (`haptic pulse <action> t= len= peak= calls= end=stop|duration`); the
// calls are checked like a runtime (XR_SESSION_NOT_FOCUSED without the focus) and
// MOCKXR_HAPTIC_FAIL=<XrResult>[,left|right] makes xrApplyHapticFeedback fail (see ApplyHaptic).
// xrEnumerateBoundSourcesForAction answers with the suggested bindings of the controllers in use
// (oculus/touch_controller, or MOCKXR_PROFILE's: see Controller profiles below).
// Menu panel tests: XR_KHR_composition_layer_color_scale_bias and XR_KHR_composition_layer_cylinder
// are listed (MOCKXR_NO_LAYER_EXT=1 hides them). Every change of the submitted layer stack is
// logged (`layers: ...`, first 200). A file <MOCKXR_DUMP_DIR>\dump_now (its first line = a name)
// dumps every layer of the next frame: <name>_L.png / <name>_R.png (projection views),
// <name>_quad.png / <name>_cyl.png (their imageRect), with `layerdump <name> ...` log lines (pose,
// size, radius / angle / aspect, rect, colour scale, head pose); the file is deleted.
// A file <MOCKXR_DUMP_DIR>\input.txt scripts controller input, one event per line, run in order:
// `<action> <seconds> [x [y]]` holds that action (button_a, button_b, button_x, button_y, menu,
// stick_left, stick_right: pressed; trigger_left/right, grip_left/right: x, default 1; move, turn:
// the stick at (x, y)), then 0.25 s idle; `wait <seconds>` idles. The file is deleted when read and
// every event is logged (`MOCKXR input ...`). Hand tests: `lhand <seconds> dx dy dz` moves the left
// grip (minimum jerk over <seconds>) to that offset (metres, LOCAL) from its scripted pose and keeps
// it there until the next lhand (`lhand 0.3 0 0 0`: back), e.g. the palm onto a quick-item card,
// then `grip_left`. A line `@<offset> <action> ...` starts that event <offset> s after the previous
// event's start, alongside it (e.g. `grip_left 1.0` then `@0.7 lhand 0.4 0.05 0.3 -0.45`: a swing
// let go of mid-way, a throw). Throw tests: `lthrow_over <seconds> <speed> <yaw> <pitch>` and
// `lthrow_push ...` script a whole left-hand throw on their own (the grip squeezed from the start,
// let go at the release point): 0.35 s still (Y just before: a quick draw takes the selected
// item), then an elbow-bent OVERHAND throw (the hand back and up beside the head, 12 cm behind
// the shoulder, the controller cocked back 70 degrees, then a curved stroke forward over the
// shoulder whose speed rises to <speed> m/s at the release, the wrist whipping forward through
// it, released 50 cm from the shoulder) or a straight PUSH (from 28 cm in front of the shoulder
// 32 cm straight along the throw, no turn of the controller), in the direction <yaw> degrees
// left and <pitch> degrees up from LOCAL -Z; the hand keeps its peak speed for its last 4-12 cm;
// then a short follow-through and back to the scripted pose (all within a real arm's reach).
// <seconds> is how long the event lasts (at least about 2.5). MOCKXR_THROW_DUMPS=1 dumps every
// layer of each one (dump_now style) at the wind-up's top (throw<N>_windup, then the top is held
// 0.5 s so the dump's stall is over before the stroke), just after the release (_release), at the
// follow-through's end (_follow) and on the way back (_back).
// Melee tests: `rswing_fore <seconds> <speed> <yaw> <pitch>` (and rswing_back, rswing_chop,
// rswing_stab) script one right-hand stroke: eased slowly to its start, held 0.3 s, the stroke
// (the hand on a 0.55 m sphere round the right shoulder, the blade out along the radius, minimum
// jerk peaking at <speed> m/s; fore = out right -> front left, back = the mirror, chop = wound up
// over the shoulder down through the front (170 deg), stab = 35 cm straight ahead), held, eased
// back; the event lasts until it is back (MOCKXR_RSWING_EASE / _HOLD: the ease to the start and the
// still hold before the stroke, default 0.8 / 0.3 s). A stroke that starts while another one still
// runs (an `@<offset>` line) chains on from wherever that one's hand is: a quick combo, e.g.
// `rswing_fore 2 6;@0.45 rswing_back 2 6;@0.45 rswing_fore 2 6` (its ease and hold
// MOCKXR_RSWING_CHAIN_EASE / _HOLD, default 0.12 / 0 s). `rwaggle <seconds> <deg> <hz> <arm m>`: the blade
// waggles +-<deg> at <hz> (the wrist), the hand +-<arm m> sideways with it (0: wrist only).
// Gun tests: `laim <seconds> <yaw> <pitch> [attach]` turns the left grip so that the gun's barrel
// (the emulator's core/vr/gun_aim.h BarrelInGrip at the default grip: attach 347 = the pistols,
// 43.8 degrees below grip -Z; 345 = the Blunderbuss and rifles; 348 = the cannons) points <yaw>
// degrees left and <pitch> degrees up from LOCAL -Z, the grip's up as near world up as it gets,
// the hand 45 cm out from the left shoulder along it; eased there over <seconds> (minimum jerk)
// and held until the next `laim` (`laim <seconds> 999 0`: back to the scripted pose). Logged with
// the barrel direction it gives (`MOCKXR input laim ...`).
// Lean tests (physical evasion): `lean <seconds> <amp m> <period s> [duck m]` sways the head
// sideways (LOCAL x) from the centre at the event's start for <seconds> (MOCKXR_LEAN's sway, but
// starting at phase 0, so a recentre or the emulator's takeover before it is its middle). It runs
// in the background: the next events start right after it starts, not after it ends.
// Head direction tests (view occlusion, looking down at the body): `look <seconds> <pitch> <yaw>`
// turns the head (minimum jerk over <seconds>, from where the last `look` left it) to <pitch>
// degrees up and <yaw> degrees left, replacing MOCKXR_HEAD's / MOCKXR_STATIC's orientation until
// `look <seconds> 999 0` (released at once). dump_now also dumps the depth of eyes that carry it
// (the emulator's xr_depth=1 with late_warp=0) as <name>_depth_L/R.png and .f32.
// MOCKXR_HANDS_FOLLOW=<k> moves both grips by k times the head's offset (drift, lean, MOCKXR_LEAN),
// like hands held in front of a leaning body (default 0: the grips stay where they are).
// Head position tests: `drift <seconds> x z` moves the
// head x, z metres in LOCAL space (the player walked away from the centre) when it starts;
// `head_y <seconds> <dy>` puts the head dy metres above (+) or below (-) its 1.6 m (height tests:
// ducking, sitting down, standing up);
// `recenter <seconds>` puts it back at the centre and sends
// XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING (LOCAL), like holding the Meta button.
// `recenter_pose <seconds> <delay_ms>` is that event the way the spec has it: the head stays
// where it is and LOCAL itself moves to it (the head's heading, yaw only, and its x/z; the old
// LOCAL's height) at a changeTime <delay_ms> ahead; the event carries poseValid and the new
// LOCAL's pose in the old one. From changeTime on, every LOCAL reference space (plain, or an
// application's offset of it) is located and converted relative to the moved LOCAL (by the time
// asked for: a locate before changeTime still gets the old LOCAL), so an application that stamps
// its samples too early shows it.
// Reference spaces created with a poseInReferenceSpace (the emulator's own recenter, Menu held:
// a LOCAL space at the head, facing where it faces) are honoured: xrLocateSpace / xrLocateViews
// return poses relative to such a base space, and layers submitted in one are converted to LOCAL
// before every check and dump (logged once per space: `MOCKXR space: ...`), so dumps, `disp`
// poses and MOCKXR_ASSOC stay in LOCAL as before. Use MOCKXR_HEAD (yaw) and input.txt `drift` to
// put the head somewhere before the recenter.
// VR overlay (renderer_vulkan/vr_overlay.h): a projection layer with
// XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT is the overlay, not a game frame: it is
// counted in `overlay=` (never in `projection=`, so the hands timeline and the eye dumps follow
// the game), checked (located FOV, rect inside its swapchain; ERROR when it is not the last layer,
// carries UNPREMULTIPLIED_ALPHA or comes twice) and its presence changes are logged
// (`MOCKXR overlay on|off frame N t=<s since the hands timeline start>`). Every eye{N} dump also dumps the overlay
// of that frame as ovl{N}_L.png / _R.png (RGBA, alpha kept, with an `ovl dump N eye v ...` line in
// the `dump` format; `ovl dump N: none` without one); segment dumps add <segment>_ovl_L/R.png
// (seg<index>_<name> when a timeline repeats a name, for the game dumps too), quad frame 300 adds
// quad_ovl_L/R.png and dump_now writes <name>_ovl_L/R.png.
// MOCKXR_BUTTONS=<action>@<t>[+<dur>][=<x>[:<y>]],...: actions held for <dur> s (default 0.1) at
// <t> s after the script clock starts (the first game projection frame after 1200, as the hands
// timeline): boolean actions (button_x, menu, stick_right, ...) read pressed, float actions
// (trigger_left/right, grip_left/right) read <x> (default 1), vector actions (move, turn) read
// (<x>, <y>) (default (1, 0)). Presses may overlap (e.g. stick_right@3+0.8,turn@3.3+0.2=1:0:
// the right stick click held while the stick is pushed right). Each press is logged once
// (`MOCKXR button ...`).
// MOCKXR_REJECT_OVERLAY=1: xrEndFrame returns XR_ERROR_LAYER_INVALID once, for the 300th frame
// that carries an overlay layer (robustness test); =all: for every frame with an overlay layer from
// the 300th on (the emulator must turn the overlay off and keep the game layer).
// UI layers (the emulator's game UI on its own layers: quads / cylinders flagged
// XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, premultiplied): dump_now writes each of
// them as <name>_l<layer index>_quad.png / _cyl.png with its alpha kept (opaque quads and
// cylinders keep the old <name>_quad.png / _cyl.png), and their `layers:` / `layerdump` lines also
// carry the full colour scale and bias (`rgba`, `bias`) and the eye visibility. Checked: imageRect
// inside its swapchain, no UNPREMULTIPLIED_ALPHA flag, at most 16 layers in a frame, swapchains
// created with arraySize 1 (ERROR lines). A colour swapchain may have mip levels (mipCount, the UI
// layer's mip chain; logged with the creation, dumps read level 0); a mip chain without
// XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT is an ERROR (it cannot be blitted down). They are counted (`ui=` in the frame
// counts). MOCKXR_REJECT_UI=1: xrEndFrame returns XR_ERROR_LAYER_INVALID once, for the 300th frame
// that carries such a layer (the emulator must drop its UI layers and keep the game). Quads and
// cylinders submitted in a VIEW reference space (a HUD fixed to the view) are converted to LOCAL
// with the head pose of the frame's display time before every check and dump (logged once).
// MOCKXR_REJECT_DEPTH=1: xrEndFrame returns XR_ERROR_VALIDATION_FAILURE once, for the 300th frame
// with the eyes' depth chained; =all: for every such frame from the 300th on (the emulator must
// probe: a frame without depth, then depth again; twice rejected = depth off, the rest kept).
// Frame timing tests: MOCKXR_SKIP_EVERY=<n>: every n-th xrWaitFrame returns one display period
// later than due (the application missed a refresh: its predictedDisplayTime is two periods after
// the last). MOCKXR_THROTTLE=<k>: every xrWaitFrame waits k periods (a runtime running the
// application at refresh / k, like ASW) while still reporting the display's period. Logged once.
// MOCKXR_UNFOCUS=<t>+<dur>[,...]: on the script clock (MOCKXR_BUTTONS'), the session goes to
// VISIBLE at <t> s (input focus lost: the Quest dashboard, the headset taken off) and back to
// FOCUSED <dur> s later; logged (`MOCKXR focus ...`). Held MOCKXR_BUTTONS stay held through it.
// MOCKXR_STOP_AFTER_S=<s>: Quit in the headset's menu, the way the Quest runtime does it: <s> s
// after the session first went FOCUSED (wall clock) it goes VISIBLE, SYNCHRONIZED and STOPPING
// (0.2 s apart), and IDLE once the application has ended it (xrEndSession) - then nothing more
// (no EXITING, no READY). Logged (`MOCKXR stop ...`). With it set, xrEndSession outside STOPPING
// is an ERROR (XR_ERROR_SESSION_NOT_STOPPING) and a frame call after the session ended is an
// ERROR (XR_ERROR_SESSION_NOT_RUNNING), as on a runtime. Built as mockxr_stop.dll / .json.
// Controller profiles (other headsets, core/vr/xr_bindings.h): every xrSuggestInteractionProfile-
// Bindings is checked like a runtime (see SuggestBindings): the profile must be one the spec
// defines (oculus/touch_controller, valve/index_controller, htc/vive_controller,
// microsoft/motion_controller, khr/simple_controller; meta/touch_controller_plus,
// hp/mixed_reality_controller and bytedance/pico4_controller / pico_neo3_controller only with their
// extension enabled) and every binding a component that profile has on that hand (d-pad components
// only with XR_EXT_dpad_binding enabled), else ERROR lines and XR_ERROR_PATH_UNSUPPORTED (the
// profile's bindings are then not taken). The type is checked too (a 2D action on a stick or
// trackpad, poses on poses, vibration on output/haptic). Logged as `suggested <n> bindings for
// <profile> -> ok|<error>`; MOCKXR_BIND_LOG=1 logs each one (`bind <profile> <action> <path>`).
// MOCKXR_EXTS=<ext>[,<ext>...]: further extensions offered (e.g. XR_META_touch_controller_plus as
// Meta's runtime, XR_BD_controller_interaction, XR_EXT_hp_mixed_reality_controller,
// XR_EXT_dpad_binding,XR_KHR_binding_modification as SteamVR); xrCreateInstance refuses one not
// offered (XR_ERROR_EXTENSION_NOT_PRESENT) and logs the enabled ones. MOCKXR_API_1_0=1: a runtime
// that only implements OpenXR 1.0 (xrCreateInstance asking for more -> XR_ERROR_API_VERSION_
// UNSUPPORTED). MOCKXR_PROFILE=<vendor>/<type> (or the whole profile path): the controllers in use
// (default oculus/touch_controller): xrGetCurrentInteractionProfile reports it (none
// when it got no accepted bindings), xrEnumerateBoundSourcesForAction answers with its bindings,
// an input action not bound in it reads inactive, and a vibration on an action not bound in it
// plays nothing (`haptic ... not bound in <profile>`).
// MOCKXR_REFUSE=<text>[,<text>...]: xrSuggestInteractionProfileBindings refuses a suggestion whose
// profile or any binding path contains one of the texts (e.g. `dpad_` or `oculus/touch_controller`).
#define _CRT_SECURE_NO_WARNINGS // std::getenv, std::fopen
#define XR_USE_GRAPHICS_API_VULKAN
// Capability marker inspected without executing the DLL by the launcher.
extern "C" __declspec(dllexport) void BbvrBenchmarkStartupV1() {}
#include "startup_navigation.h"
#include <vulkan/vulkan.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdarg>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>
#include <zlib.h>

#include "core/vr/body_presets.h"
#include "file_io.h"

#define EXPORT extern "C" __declspec(dllexport)

namespace {

std::mutex g_mutex;
std::mutex g_log_mutex;
FILE* g_log = nullptr;

void Log(const char* fmt, ...) {
    std::lock_guard lk{g_log_mutex};
    if (!g_log) {
        const char* dir = BenchmarkFiles::DumpDirectory();
        std::string path = std::string(dir ? dir : ".") + "\\mockxr.log";
        g_log = BenchmarkFiles::Open(path.c_str(), "w");
    }
    if (!g_log) {
        return;
    }
    va_list args;
    va_start(args, fmt);
    std::vfprintf(g_log, fmt, args);
    va_end(args);
    std::fputc('\n', g_log);
    std::fflush(g_log);
}

int64_t NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

struct Swapchain {
    int64_t format{};
    uint32_t width{}, height{};
    std::vector<VkImage> images;
    std::vector<VkDeviceMemory> memory;
    uint32_t next{0};
    int last_released{-1};
    int acquired{-1};
    bool waited{false};
    // MOCKXR_LATCH_MS: per image, the fence submitted at its last release (never destroyed).
    std::vector<VkFence> release_fence;
    bool depth{false}; // a depth format (XR_KHR_composition_layer_depth)
};

bool IsDepthFormat(int64_t format) {
    return format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM ||
           format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

struct Session {
    VkInstance vk_instance{};
    VkPhysicalDevice vk_physical{};
    VkDevice vk_device{};
    uint32_t queue_family{};
    uint32_t queue_index{};
    std::deque<XrSessionState> events;
    XrSessionState state{XR_SESSION_STATE_IDLE};
    bool running{false};
    int64_t next_frame{0};
    uint64_t frames{0};
    uint64_t projection_frames{0};
    uint64_t quad_frames{0};
    uint64_t cylinder_frames{0};
    uint64_t overlay_frames{0};
    uint64_t empty_frames{0};
    int dumps{0};
    int64_t last_report{0};
};

struct Action {
    XrActionType type;
    std::string name;
};

std::atomic<uint64_t> g_handle{1};
template <typename T>
T NewHandle() {
    return reinterpret_cast<T>(g_handle.fetch_add(1));
}

std::map<XrSwapchain, Swapchain> g_swapchains;
Session* g_session = nullptr;
std::map<std::string, XrPath> g_paths;
std::map<XrPath, std::string> g_path_names;
std::map<XrAction, Action> g_actions;
std::map<XrSpace, int> g_spaces; // 0 local, 1 view, 2 action
// Action spaces of the hands: 0 aim_left, 1 aim_right, 2 grip_pose_left, 3 grip_pose_right.
std::map<XrSpace, int> g_space_roles;
// The latest predicted display time handed out by xrWaitFrame (scripted grip values).
std::atomic<int64_t> g_display_time{0};
// Display period; XR_FB_display_refresh_rate switches it (90 Hz at start).
std::atomic<int64_t> g_frame_period{11'111'111};
// MOCKXR_RATES: the refresh rates offered (default 72 / 90 / 120 Hz).
const std::vector<float>& RefreshRates() {
    static const std::vector<float> rates = [] {
        std::vector<float> out;
        if (const char* value = std::getenv("MOCKXR_RATES")) {
            const char* p = value;
            while (*p != '\0') {
                char* end = nullptr;
                const float hz = std::strtof(p, &end);
                if (end == p) {
                    break;
                }
                if (hz > 10.0f) {
                    out.push_back(hz);
                }
                p = *end == ',' ? end + 1 : end;
            }
        }
        if (out.empty()) {
            out = {72.0f, 90.0f, 120.0f};
        }
        std::sort(out.begin(), out.end());
        if (std::find(out.begin(), out.end(), 90.0f) == out.end()) {
            g_frame_period.store(static_cast<int64_t>(1e9 / out.back()));
        }
        return out;
    }();
    return rates;
}

// ---- MOCKXR_REC / MOCKXR_MAXRECT / MOCKXR_MAX_SWAPCHAIN: the runtime's size limits ----------
// MOCKXR_REC=<w>x<h>: each view's recommendedImageRect (default 2064x2208); MOCKXR_MAXRECT=<w>x<h>:
// each view's maxImageRect (default 4096x4096); MOCKXR_MAX_SWAPCHAIN=<w>x<h>: the largest swapchain
// image (XrSystemGraphicsProperties, default 4096x4096). Meta Link (Quest 3, Link slider 1x):
// MOCKXR_REC=1424x1568 MOCKXR_MAXRECT=2848x3136 MOCKXR_MAX_SWAPCHAIN=4096x4096. A swapchain larger
// than the largest fails (XR_ERROR_VALIDATION_FAILURE, an ERROR line); a projection view's
// imageRect larger than maxImageRect is an ERROR line (CheckProjectionView).
struct SizeLimit {
    uint32_t width;
    uint32_t height;
};
SizeLimit SizeFromEnv(const char* name, SizeLimit fallback) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        return fallback;
    }
    unsigned w = 0;
    unsigned h = 0;
    if (std::sscanf(value, "%ux%u", &w, &h) != 2 || w == 0 || h == 0) {
        return fallback;
    }
    return {w, h};
}
SizeLimit RecommendedRect() {
    static const SizeLimit size = SizeFromEnv("MOCKXR_REC", {2064, 2208});
    return size;
}
SizeLimit MaxImageRect() {
    static const SizeLimit size = SizeFromEnv("MOCKXR_MAXRECT", {4096, 4096});
    return size;
}
SizeLimit MaxSwapchain() {
    static const SizeLimit size = SizeFromEnv("MOCKXR_MAX_SWAPCHAIN", {4096, 4096});
    return size;
}

// ---- MOCKXR_LATCH_MS: compositor latch ---------------------------------------------
int64_t LatchNs() {
    static const int64_t ns = [] {
        const char* value = std::getenv("MOCKXR_LATCH_MS");
        return value != nullptr ? static_cast<int64_t>(std::atof(value) * 1e6) : int64_t{0};
    }();
    return ns;
}

// Fence status checks and resets (host access to a fence is externally synchronised).
std::mutex g_fence_mutex;
std::atomic<uint64_t> g_release_serial{0};
std::atomic<uint64_t> g_latch_checked{0};
std::atomic<uint64_t> g_latch_late{0};
// The serial of each release fence's last submission (g_fence_mutex): a later release of the same
// image makes an older latch check moot.
std::map<VkFence, uint64_t> g_fence_serial;

struct LatchCheck {
    int64_t at{};       // steady clock: the latch
    uint64_t frame{};   // projection frame
    VkFence fence[2]{}; // the shown image of each view (null = none)
    uint64_t serial[2]{};
};
std::mutex g_latch_mutex;
std::condition_variable g_latch_cv;
std::deque<LatchCheck> g_latch_queue;

void LatchThread(VkDevice dev) {
    for (;;) {
        LatchCheck check{};
        {
            std::unique_lock lk{g_latch_mutex};
            g_latch_cv.wait(lk, [] { return !g_latch_queue.empty(); });
            check = g_latch_queue.front();
            g_latch_queue.pop_front();
        }
        const int64_t wait = check.at - NowNs();
        if (wait > 0) {
            std::this_thread::sleep_for(std::chrono::nanoseconds(wait));
        }
        for (int v = 0; v < 2; ++v) {
            if (check.fence[v] == VK_NULL_HANDLE) {
                continue;
            }
            std::lock_guard lk{g_fence_mutex};
            if (g_fence_serial[check.fence[v]] != check.serial[v]) {
                continue; // released again since: the image changed, the check is moot
            }
            ++g_latch_checked;
            if (vkGetFenceStatus(dev, check.fence[v]) != VK_SUCCESS) {
                const uint64_t late = ++g_latch_late;
                if (late <= 20) {
                    Log("MOCKXR latch: view %d of projection frame %llu late (its image's GPU "
                        "work unfinished %.1f ms before display)",
                        v, (unsigned long long)check.frame, static_cast<double>(LatchNs()) * 1e-6);
                }
            }
        }
    }
}

// ---- head animation ---------------------------------------------------------------
struct FixedHead {
    bool on{false};
    float yaw{0.0f};    // radians, + = left
    float pitch{0.0f};  // radians, + = up
    float roll{0.0f};   // radians, + = the head's right side up (about the view axis)
    float swing{0.0f};  // radians: yaw swing amplitude (sine), 0 = fixed
    float period{2.0f}; // s, of the swing
    bool neck{false};   // the swing turns about the pitched head's own up (adds roll), not world up
    float nod{0.0f};    // radians: pitch swing amplitude (sine, same period), 0 = fixed
    float nodrate{1.0f}; // nods per period
    float step{0.0f};   // radians: yaw steps by this every half period (square wave), 0 = none
};

// MOCKXR_HEAD=pitch:<deg>[,yaw:<deg>][,roll:<deg>][,swing:<deg>][,period:<s>][,neck:1][,nod:<deg>]
// [,nodrate:<n>]
const FixedHead& HeadOverride() {
    static const FixedHead head = [] {
        FixedHead h{};
        const char* value = std::getenv("MOCKXR_HEAD");
        if (value == nullptr || value[0] == '\0') {
            return h;
        }
        const std::string text{value};
        size_t start = 0;
        while (start <= text.size()) {
            const size_t comma = text.find(',', start);
            const std::string token =
                text.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            const size_t colon = token.find(':');
            if (colon != std::string::npos) {
                const float deg = std::strtof(token.c_str() + colon + 1, nullptr);
                const std::string key = token.substr(0, colon);
                if (key == "pitch") {
                    h.pitch = deg * 3.14159265f / 180.0f;
                } else if (key == "yaw") {
                    h.yaw = deg * 3.14159265f / 180.0f;
                } else if (key == "roll") {
                    h.roll = deg * 3.14159265f / 180.0f;
                } else if (key == "swing") {
                    h.swing = deg * 3.14159265f / 180.0f;
                } else if (key == "period") {
                    h.period = std::max(deg, 0.1f);
                } else if (key == "neck") {
                    h.neck = deg != 0.0f;
                } else if (key == "nod") {
                    h.nod = deg * 3.14159265f / 180.0f;
                } else if (key == "nodrate") {
                    h.nodrate = std::clamp(deg, 0.25f, 8.0f);
                } else if (key == "step") {
                    h.step = deg * 3.14159265f / 180.0f;
                }
            }
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
        h.on = true;
        return h;
    }();
    return head;
}

XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b);

// input.txt `drift` / `recenter`: the head's offset in LOCAL space (metres).
std::atomic<float> g_head_offset_x{0.0f};
std::atomic<float> g_head_offset_z{0.0f};
// input.txt `head_y <seconds> <dy>` (height tests): the head dy metres above (+) or below (-) its 1.6 m.
std::atomic<float> g_head_offset_y{0.0f};
// input.txt `lean`: a sideways sway that starts at the centre when the event starts (phase 0, so
// a recentre taken before it is the sway's middle), replacing MOCKXR_LEAN while it runs.
std::atomic<float> g_lean_amp{0.0f};
std::atomic<float> g_lean_period{4.0f};
std::atomic<float> g_lean_duck{0.0f};
std::atomic<double> g_lean_t0{0.0};
std::atomic<double> g_lean_t1{0.0};
// input.txt `look`: the head turned to a pitch / yaw (minimum jerk from where the last look left
// it), replacing MOCKXR_HEAD's or the default motion's orientation until `look <s> 999 0`.
struct LookState {
    bool active{false};
    float from_pitch{}, from_yaw{}, to_pitch{}, to_yaw{}; // radians
    double t0{}, t1{};
};
std::mutex g_look_mutex;
LookState g_look;

// The look's pitch / yaw at time s (g_look_mutex held).
void LookAt(double s, float& pitch, float& yaw) {
    const double span = g_look.t1 - g_look.t0;
    const double u = span > 0.0 ? std::clamp((s - g_look.t0) / span, 0.0, 1.0) : 1.0;
    const float k = static_cast<float>(u * u * u * (10.0 - 15.0 * u + 6.0 * u * u));
    pitch = g_look.from_pitch + (g_look.to_pitch - g_look.from_pitch) * k;
    yaw = g_look.from_yaw + (g_look.to_yaw - g_look.from_yaw) * k;
}

void HeadPose(int64_t t, XrPosef& pose) {
    const double s = static_cast<double>(t) * 1e-9;
    static const bool frozen = std::getenv("MOCKXR_STATIC") != nullptr;
    const FixedHead& fixed = HeadOverride();
    const float yaw = fixed.on ? fixed.yaw
                      : frozen ? 0.0f
                               : static_cast<float>(0.5 * std::sin(s * 0.6));
    const float nod =
        fixed.on && fixed.nod != 0.0f
            ? fixed.nod * static_cast<float>(std::sin(2.0 * 3.14159265358979 * s * fixed.nodrate /
                                                      fixed.period))
            : 0.0f;
    const float pitch = fixed.on ? fixed.pitch + nod
                        : frozen ? 0.0f
                                 : static_cast<float>(0.15 * std::sin(s * 0.4));
    const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f);
    const float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
    pose.orientation = {cy * sp, sy * cp, -sy * sp, cy * cp};
    if (fixed.on && fixed.step != 0.0f) {
        // step:<deg>: a square wave of yaw (instant turns every half period, latency tests).
        const bool second = std::fmod(s, static_cast<double>(fixed.period)) >= 0.5 * fixed.period;
        const float y = fixed.yaw + (second ? fixed.step : 0.0f);
        const float cy2 = std::cos(y * 0.5f), sy2 = std::sin(y * 0.5f);
        pose.orientation = {cy2 * sp, sy2 * cp, -sy2 * sp, cy2 * cp};
    }
    if (fixed.on && (fixed.swing != 0.0f || fixed.roll != 0.0f)) {
        // q = yaw(world) * pitch * [swing about the head's own up when `neck`] * roll, or the
        // swing added to the world yaw. Turning a pitched head about its own up axis is what a
        // neck does when you look down at a hand on one side: the line between the eyes tilts.
        const auto axis = [](float x, float y, float z, float a) {
            const float s = std::sin(a * 0.5f);
            return XrQuaternionf{x * s, y * s, z * s, std::cos(a * 0.5f)};
        };
        const float swing = fixed.swing * static_cast<float>(std::sin(
                                              2.0 * 3.14159265358979 * s / fixed.period));
        XrQuaternionf q = axis(0, 1, 0, fixed.yaw + (fixed.neck ? 0.0f : swing));
        q = Multiply(q, axis(1, 0, 0, pitch));
        if (fixed.neck) {
            q = Multiply(q, axis(0, 1, 0, swing));
        }
        pose.orientation = Multiply(q, axis(0, 0, 1, fixed.roll));
    }
    {
        std::lock_guard lk{g_look_mutex};
        if (g_look.active) {
            float lp = 0.0f, ly = 0.0f;
            LookAt(s, lp, ly);
            const float cy3 = std::cos(ly * 0.5f), sy3 = std::sin(ly * 0.5f);
            const float cp3 = std::cos(lp * 0.5f), sp3 = std::sin(lp * 0.5f);
            pose.orientation = {cy3 * sp3, sy3 * cp3, -sy3 * sp3, cy3 * cp3};
        }
    }
    pose.position = {g_head_offset_x.load(), 1.6f + g_head_offset_y.load(), g_head_offset_z.load()};
    // MOCKXR_LEAN=<amplitude m>[,<period s>[,<duck m>]]: the head sways sideways (physical
    // evasion tests), optionally dipping by <duck> at the extremes.
    static const std::array<float, 3> lean = [] {
        std::array<float, 3> v{0.0f, 4.0f, 0.0f};
        if (const char* text = std::getenv("MOCKXR_LEAN")) {
            std::sscanf(text, "%f,%f,%f", &v[0], &v[1], &v[2]);
        }
        return v;
    }();
    if (g_lean_t1.load() > g_lean_t0.load() && s >= g_lean_t0.load()) {
        if (s < g_lean_t1.load() && g_lean_period.load() > 0.0f) {
            const double phase =
                2.0 * 3.14159265358979 * (s - g_lean_t0.load()) / g_lean_period.load();
            const float side = static_cast<float>(std::sin(phase));
            pose.position.x += g_lean_amp.load() * side;
            pose.position.y -= g_lean_duck.load() * side * side;
        }
        return;
    }
    if (lean[0] != 0.0f && lean[1] > 0.0f) {
        const double phase = 2.0 * 3.14159265358979 * s / lean[1];
        const float side = static_cast<float>(std::sin(phase));
        pose.position.x += lean[0] * side;
        pose.position.y -= lean[2] * side * side;
    }
}

bool QuestFov() {
    static const bool quest = [] {
        const char* v = std::getenv("MOCKXR_QUEST_FOV");
        return v != nullptr && v[0] != '\0' && v[0] != '0';
    }();
    return quest;
}

// The FOV xrLocateViews reports for `eye` (0 = left).
XrFovf LocatedFov(uint32_t eye) {
    constexpr float D = 3.14159265f / 180.0f;
    if (QuestFov()) {
        return eye == 0 ? XrFovf{-54.0f * D, 40.0f * D, 44.0f * D, -55.0f * D}
                        : XrFovf{-40.0f * D, 54.0f * D, 44.0f * D, -55.0f * D};
    }
    // Default: a Quest 3-like asymmetric FOV that the application may change (fovMutable).
    return eye == 0 ? XrFovf{-0.942f, 0.698f, 0.768f, -0.873f}
                    : XrFovf{-0.698f, 0.942f, 0.768f, -0.873f};
}

XrVector3f Rotate(const XrQuaternionf& q, XrVector3f v) {
    const XrVector3f u{q.x, q.y, q.z};
    const float s = q.w;
    const float duv = u.x * v.x + u.y * v.y + u.z * v.z;
    const float duu = u.x * u.x + u.y * u.y + u.z * u.z;
    const XrVector3f c{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    return {2 * duv * u.x + (s * s - duu) * v.x + 2 * s * c.x,
            2 * duv * u.y + (s * s - duu) * v.y + 2 * s * c.y,
            2 * duv * u.z + (s * s - duu) * v.z + 2 * s * c.z};
}

// ---- scripted hands (MOCKXR_HANDS) --------------------------------------------------
namespace BP = VR::BodyPresets;

const std::vector<const BP::Preset*>& HandsTimeline() {
    static const std::vector<const BP::Preset*> timeline = [] {
        const char* value = std::getenv("MOCKXR_HANDS");
        if (value == nullptr || value[0] == '\0' || std::strcmp(value, "0") == 0) {
            return std::vector<const BP::Preset*>{};
        }
        std::string unknown;
        auto parsed = BP::ParseTimeline(value, &unknown);
        Log("MOCKXR hands '%s': %zu segments of %.0f s, start after 1200 projection frames%s%s",
            value, parsed.size(), BP::SegmentSeconds, unknown.empty() ? "" : "; unknown: ",
            unknown.c_str());
        return parsed;
    }();
    return timeline;
}

// Display time the timeline started at (0 = not yet).
std::atomic<int64_t> g_hands_start{0};

// The timeline position at `time`; false without MOCKXR_HANDS or before it starts.
bool HandsPoint(int64_t time, BP::TimelinePoint& point) {
    const auto& timeline = HandsTimeline();
    const int64_t start = g_hands_start.load();
    if (timeline.empty() || start == 0 || time < start) {
        return false;
    }
    point = BP::Locate(timeline, static_cast<double>(time - start) * 1e-9);
    return point.preset != nullptr;
}

// MOCKXR_HAND_BLEND=<s>: every segment starts by moving both grips from where the previous segment
// left them (minimum jerk over <s> seconds) instead of jumping there, so approaches have real
// speeds (the vial's wrist injection needs the hand to come in moving); unset or 0 = jump.
double HandBlendSeconds() {
    static const double seconds = [] {
        const char* value = std::getenv("MOCKXR_HAND_BLEND");
        return value != nullptr ? std::clamp(std::atof(value), 0.0, BP::SegmentSeconds) : 0.0;
    }();
    return seconds;
}

// MOCKXR_HAND_SHIFT=x,y,z: metres added to both scripted grips (LOCAL space), e.g. 0,0.1,0.15 holds
// every preset 10 cm higher and 15 cm nearer the face (near-field tests).
const std::array<float, 3>& HandShift() {
    static const std::array<float, 3> shift = [] {
        std::array<float, 3> v{0.0f, 0.0f, 0.0f};
        if (const char* value = std::getenv("MOCKXR_HAND_SHIFT")) {
            std::sscanf(value, "%f,%f,%f", &v[0], &v[1], &v[2]);
            Log("MOCKXR hand shift (%.3f %.3f %.3f)", v[0], v[1], v[2]);
        }
        return v;
    }();
    return shift;
}

bool ScriptedHandUnshifted(int side, int64_t time, BP::HandPose& out);
void LiveLeftOffset(int64_t time, float out[3]); // input.txt `lhand` (below)
void LiveThrowPose(int64_t time, BP::HandPose& pose); // input.txt `lthrow_*` (below)
void LiveAimPose(int64_t time, BP::HandPose& pose);   // input.txt `laim` (below)
void LiveRightPose(int64_t time, BP::HandPose& pose); // input.txt `rswing_*` (below)

// MOCKXR_LEFT_PATH=<t>:<dx>,<dy>,<dz>;...: the left grip moved by these offsets (metres, LOCAL)
// from its scripted pose, keyframes at <t> s on the hands timeline, minimum jerk between them
// (0 before the first, the last one held after it). Quick-item tests: reach for a card of the
// arc, then swing and let go (MOCKXR_BUTTONS grip_left).
struct LeftKey {
    double t;
    float d[3];
};
const std::vector<LeftKey>& LeftPath() {
    static const std::vector<LeftKey> path = [] {
        std::vector<LeftKey> keys;
        if (const char* value = std::getenv("MOCKXR_LEFT_PATH")) {
            const char* p = value;
            while (*p != 0) {
                LeftKey k{};
                int used = 0;
                if (std::sscanf(p, "%lf:%f,%f,%f%n", &k.t, &k.d[0], &k.d[1], &k.d[2], &used) != 4) {
                    break;
                }
                keys.push_back(k);
                p += used;
                while (*p == ';' || *p == ' ') {
                    ++p;
                }
            }
            Log("MOCKXR left path: %zu keyframes", keys.size());
        }
        return keys;
    }();
    return path;
}

void LeftPathOffset(double t, float out[3]) {
    const auto& keys = LeftPath();
    out[0] = out[1] = out[2] = 0.0f;
    if (keys.empty() || t <= keys.front().t) {
        return;
    }
    for (size_t i = 1; i < keys.size(); ++i) {
        if (t < keys[i].t) {
            const double span = std::max(keys[i].t - keys[i - 1].t, 1e-6);
            const float k = static_cast<float>(BP::MinJerk((t - keys[i - 1].t) / span));
            for (int j = 0; j < 3; ++j) {
                out[j] = keys[i - 1].d[j] + (keys[i].d[j] - keys[i - 1].d[j]) * k;
            }
            return;
        }
    }
    for (int j = 0; j < 3; ++j) {
        out[j] = keys.back().d[j];
    }
}

// MOCKXR_HANDS_FOLLOW=<k>: both grips move by k times the head's offset from the centre (input.txt
// drift and lean, MOCKXR_LEAN), as hands held in front of a leaning or stepping body do (physical
// evasion tests: otherwise the real hands stay behind and the hunter's IK arms with them). Default
// 0: the grips stay put.
void HandFollowOffset(int64_t time, float out[3]) {
    static const float k = [] {
        const char* value = std::getenv("MOCKXR_HANDS_FOLLOW");
        const float v = value != nullptr ? static_cast<float>(std::atof(value)) : 0.0f;
        if (v != 0.0f) {
            Log("MOCKXR hands follow the head's offset x %.2f", v);
        }
        return v;
    }();
    out[0] = out[1] = out[2] = 0.0f;
    if (k == 0.0f) {
        return;
    }
    XrPosef head{};
    HeadPose(time, head);
    out[0] = k * head.position.x;
    out[1] = k * (head.position.y - 1.6f);
    out[2] = k * head.position.z;
}

// MOCKXR_HAND_SCALE=<k>: a player k times the presets' size (they are a ~171 cm body, the head at
// 1.6 m): every scripted grip moved away from the head's rest point (0, 1.6, 0) k times as far
// (before MOCKXR_HAND_SHIFT), e.g. 1.111 for a 190 cm player, 0.906 for 155 cm (height tests,
// with MOCKXR_FLOOR for the eye height).
float HandScale() {
    static const float k = [] {
        float v = 1.0f;
        if (const char* value = std::getenv("MOCKXR_HAND_SCALE")) {
            v = std::clamp(std::strtof(value, nullptr), 0.5f, 2.0f);
            Log("MOCKXR hand scale %.3f (about (0, 1.6, 0))", v);
        }
        return v;
    }();
    return k;
}

// The scripted grip of `side` (0 = left) at `time`: both hands rest until the timeline starts.
bool ScriptedHand(int side, int64_t time, BP::HandPose& out) {
    if (!ScriptedHandUnshifted(side, time, out)) {
        return false;
    }
    float follow[3];
    HandFollowOffset(time, follow);
    if (const float k = HandScale(); k != 1.0f) {
        static constexpr float rest[3]{0.0f, 1.6f, 0.0f};
        for (int i = 0; i < 3; ++i) {
            out.p[i] = rest[i] + (out.p[i] - rest[i]) * k;
        }
    }
    for (int i = 0; i < 3; ++i) {
        out.p[i] += HandShift()[i] + follow[i];
    }
    return true;
}

bool ScriptedHandUnshifted(int side, int64_t time, BP::HandPose& out) {
    if (HandsTimeline().empty()) {
        return false;
    }
    BP::TimelinePoint point{};
    if (!HandsPoint(time, point)) {
        out = BP::Evaluate(*BP::Find("rest"), side, 0.0);
        return true;
    }
    out = BP::Evaluate(*point.preset, side, point.t);
    if (side == 0) {
        float live[3];
        LiveLeftOffset(time, live); // at the pose's own time: the velocities see the move
        for (int i = 0; i < 3; ++i) {
            out.p[i] += live[i];
        }
    }
    if (side == 0 && !LeftPath().empty()) {
        float d[3];
        LeftPathOffset(static_cast<double>(time - g_hands_start.load()) * 1e-9, d);
        for (int i = 0; i < 3; ++i) {
            out.p[i] += d[i];
        }
    }
    if (side == 0) {
        LiveThrowPose(time, out);
        LiveAimPose(time, out);
    } else {
        LiveRightPose(time, out);
    }
    const double blend = HandBlendSeconds();
    if (blend > 0.0 && point.t < blend) {
        const auto& timeline = HandsTimeline();
        const BP::Preset* previous =
            point.segment == 0 ? BP::Find("rest")
                               : timeline[(point.index + timeline.size() - 1) % timeline.size()];
        const BP::HandPose from = BP::Evaluate(*previous, side, BP::SegmentSeconds);
        const float k = static_cast<float>(BP::MinJerk(point.t / blend));
        float dot = 0.0f;
        for (int i = 0; i < 4; ++i) {
            dot += from.q[i] * out.q[i];
        }
        const float sign = dot < 0.0f ? -1.0f : 1.0f;
        float norm = 0.0f;
        for (int i = 0; i < 3; ++i) {
            out.p[i] = from.p[i] + (out.p[i] - from.p[i]) * k;
        }
        for (int i = 0; i < 4; ++i) {
            out.q[i] = from.q[i] + (sign * out.q[i] - from.q[i]) * k;
            norm += out.q[i] * out.q[i];
        }
        norm = std::sqrt(std::max(norm, 1e-12f));
        for (int i = 0; i < 4; ++i) {
            out.q[i] /= norm;
        }
    }
    return true;
}

XrQuaternionf Multiply(const XrQuaternionf& a, const XrQuaternionf& b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}

// Finite-difference velocities of the script around `time` (base space: LOCAL).
void ScriptedVelocity(int side, int64_t time, XrVector3f& linear, XrVector3f& angular) {
    constexpr int64_t H = 2'000'000; // 2 ms each side
    BP::HandPose a{}, b{};
    ScriptedHand(side, time - H, a);
    ScriptedHand(side, time + H, b);
    const float dt = 2.0f * static_cast<float>(H) * 1e-9f;
    linear = {(b.p[0] - a.p[0]) / dt, (b.p[1] - a.p[1]) / dt, (b.p[2] - a.p[2]) / dt};
    const XrQuaternionf qa{a.q[0], a.q[1], a.q[2], a.q[3]};
    XrQuaternionf dq = Multiply({b.q[0], b.q[1], b.q[2], b.q[3]}, {-qa.x, -qa.y, -qa.z, qa.w});
    if (dq.w < 0.0f) {
        dq = {-dq.x, -dq.y, -dq.z, -dq.w};
    }
    const float n = std::sqrt(dq.x * dq.x + dq.y * dq.y + dq.z * dq.z);
    const float angle = 2.0f * std::atan2(n, dq.w);
    const float k = n > 1e-9f ? angle / (n * dt) : 0.0f;
    angular = {dq.x * k, dq.y * k, dq.z * k};
}

// A LOCAL point in the submitted image rect of `view` (pixels from the rect's top-left).
bool ProjectToRect(const XrCompositionLayerProjectionView& view, const std::array<float, 3>& p,
                   int& px, int& py) {
    const XrQuaternionf& q = view.pose.orientation;
    const XrVector3f d{p[0] - view.pose.position.x, p[1] - view.pose.position.y,
                       p[2] - view.pose.position.z};
    const XrVector3f v = Rotate({-q.x, -q.y, -q.z, q.w}, d);
    if (v.z > -0.01f) {
        return false;
    }
    const float tx = v.x / -v.z, ty = v.y / -v.z;
    const float l = std::tan(view.fov.angleLeft), r = std::tan(view.fov.angleRight);
    const float u = std::tan(view.fov.angleUp), dn = std::tan(view.fov.angleDown);
    const float fx = (tx - l) / (r - l), fy = (u - ty) / (u - dn);
    px = static_cast<int>(
        std::lround(fx * static_cast<float>(view.subImage.imageRect.extent.width)));
    py = static_cast<int>(
        std::lround(fy * static_cast<float>(view.subImage.imageRect.extent.height)));
    return fx >= 0.0f && fx <= 1.0f && fy >= 0.0f && fy <= 1.0f;
}

// MOCKXR_MARK_HANDS: both grips' projections in `view` at `time` (left first).
std::vector<std::array<int, 2>> HandMarks(const XrCompositionLayerProjectionView& view,
                                          int64_t time, std::string& text) {
    static const bool enabled = std::getenv("MOCKXR_MARK_HANDS") != nullptr;
    std::vector<std::array<int, 2>> marks;
    if (!enabled) {
        return marks;
    }
    for (int side = 0; side < 2; ++side) {
        BP::HandPose hand{};
        if (!ScriptedHand(side, time, hand)) {
            break;
        }
        int x = 0, y = 0;
        const bool inside = ProjectToRect(view, hand.p, x, y);
        char line[112];
        std::snprintf(line, sizeof(line), " %s grip (%.2f %.2f %.2f) -> px %d,%d%s",
                      side == 0 ? "L" : "R", hand.p[0], hand.p[1], hand.p[2], x, y,
                      inside ? "" : " (outside)");
        text += line;
        if (inside) {
            marks.push_back({x, y});
        }
    }
    return marks;
}

// A dumped view for offline checks (offline analysis): the submitted pose, FOV and
// rect, and what the compositor and the real eye have at the frame's display time: the head pose
// and this eye's located position (the head's own roll included), and the scripted grips then.
std::string ViewText(const XrCompositionLayerProjectionView& view, uint32_t eye,
                     int64_t display_time) {
    XrPosef head{};
    HeadPose(display_time, head);
    const XrVector3f d = Rotate(head.orientation, {eye == 0 ? -0.032f : 0.032f, 0.0f, 0.0f});
    const auto& q = view.pose.orientation;
    const auto& p = view.pose.position;
    const auto& r = view.subImage.imageRect;
    char text[512];
    std::snprintf(text, sizeof(text),
                  " view sq(%.6f %.6f %.6f %.6f) sp(%.5f %.5f %.5f) fov(%.5f %.5f %.5f %.5f) "
                  "rect(%d %d %d %d) dq(%.6f %.6f %.6f %.6f) dp(%.5f %.5f %.5f) "
                  "eye(%.5f %.5f %.5f) t=%.4f",
                  q.x, q.y, q.z, q.w, p.x, p.y, p.z, view.fov.angleLeft, view.fov.angleRight,
                  view.fov.angleUp, view.fov.angleDown, r.offset.x, r.offset.y, r.extent.width,
                  r.extent.height, head.orientation.x, head.orientation.y, head.orientation.z,
                  head.orientation.w, head.position.x, head.position.y, head.position.z,
                  head.position.x + d.x, head.position.y + d.y, head.position.z + d.z,
                  static_cast<double>(display_time) * 1e-9);
    std::string out{text};
    for (int side = 0; side < 2; ++side) {
        BP::HandPose hand{};
        if (!ScriptedHand(side, display_time, hand)) {
            break;
        }
        char grip[96];
        std::snprintf(grip, sizeof(grip), " %s(%.5f %.5f %.5f)", side == 0 ? "gl" : "gr",
                      hand.p[0], hand.p[1], hand.p[2]);
        out += grip;
    }
    return out;
}

// ---- PNG dump of a swapchain image ------------------------------------------------
// `alpha`: an RGBA PNG (colour type 6) that keeps the image's alpha (the overlay layer).
void WritePng(const std::string& path, uint32_t w, uint32_t h, const uint8_t* bgra, bool rgba,
              bool alpha = false) {
    std::vector<uint8_t> raw;
    raw.reserve(((alpha ? w * 4 : w * 3) + 1) * h);
    for (uint32_t y = 0; y < h; ++y) {
        raw.push_back(0);
        for (uint32_t x = 0; x < w; ++x) {
            const uint8_t* p = bgra + (y * w + x) * 4;
            if (rgba) {
                raw.insert(raw.end(), {p[0], p[1], p[2]});
            } else {
                raw.insert(raw.end(), {p[2], p[1], p[0]});
            }
            if (alpha) {
                raw.push_back(p[3]);
            }
        }
    }
    uLongf size = compressBound(static_cast<uLong>(raw.size()));
    std::vector<uint8_t> z(size);
    compress2(z.data(), &size, raw.data(), static_cast<uLong>(raw.size()), 6);
    FILE* f = BenchmarkFiles::Open(path.c_str(), "wb");
    if (!f) {
        return;
    }
    const auto be = [](uint32_t v) {
        return std::array<uint8_t, 4>{uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)};
    };
    const auto chunk = [&](const char* type, const uint8_t* data, uint32_t len) {
        auto l = be(len);
        std::fwrite(l.data(), 1, 4, f);
        std::fwrite(type, 1, 4, f);
        if (len) {
            std::fwrite(data, 1, len, f);
        }
        uLong crc = crc32(0, reinterpret_cast<const Bytef*>(type), 4);
        if (len) {
            crc = crc32(crc, data, len);
        }
        auto c = be(static_cast<uint32_t>(crc));
        std::fwrite(c.data(), 1, 4, f);
    };
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    std::fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    auto wb = be(w), hb = be(h);
    std::memcpy(ihdr, wb.data(), 4);
    std::memcpy(ihdr + 4, hb.data(), 4);
    ihdr[8] = 8;
    ihdr[9] = alpha ? 6 : 2;
    ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), static_cast<uint32_t>(size));
    chunk("IEND", nullptr, 0);
    std::fclose(f);
}

uint32_t FindMemoryType(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags flags) {
    VkPhysicalDeviceMemoryProperties props;
    vkGetPhysicalDeviceMemoryProperties(pd, &props);
    for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
        if ((bits & (1u << i)) && (props.memoryTypes[i].propertyFlags & flags) == flags) {
            return i;
        }
    }
    return 0;
}

// Clamps a submitted image rectangle to the swapchain (the dump never reads outside it).
XrRect2Di ClampRect(const Swapchain& sc, XrRect2Di rect) {
    const int32_t w = static_cast<int32_t>(sc.width), h = static_cast<int32_t>(sc.height);
    rect.offset.x = rect.offset.x < 0 ? 0 : (rect.offset.x > w - 1 ? w - 1 : rect.offset.x);
    rect.offset.y = rect.offset.y < 0 ? 0 : (rect.offset.y > h - 1 ? h - 1 : rect.offset.y);
    if (rect.extent.width < 1 || rect.offset.x + rect.extent.width > w) {
        rect.extent.width = w - rect.offset.x;
    }
    if (rect.extent.height < 1 || rect.offset.y + rect.extent.height > h) {
        rect.extent.height = h - rect.offset.y;
    }
    return rect;
}

// ---- image readback (eye dumps) ----------------------------------------------------
// MOCKXR_DUMP_ASYNC=1: a dump's GPU copy is queued and collected a few frames later, so a dump never
// stalls the application (its frame thread holds its queue lock inside xrEndFrame); default: the
// copy is waited for in xrEndFrame (the PNG is always written on a worker thread).
bool DumpAsync() {
    static const bool enabled = std::getenv("MOCKXR_DUMP_ASYNC") != nullptr;
    return enabled;
}

struct Readback {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    VkCommandPool pool{};
    VkFence fence{};
    VkDeviceSize size{};
    std::function<void(std::vector<uint8_t>)> done; // on a worker thread, with the bytes
};
std::mutex g_readback_mutex;
std::vector<Readback> g_readbacks;

void FinishReadback(Readback& r) {
    Session& s = *g_session;
    void* data = nullptr;
    vkMapMemory(s.vk_device, r.memory, 0, r.size, 0, &data);
    std::vector<uint8_t> bytes(static_cast<const uint8_t*>(data),
                               static_cast<const uint8_t*>(data) + r.size);
    vkUnmapMemory(s.vk_device, r.memory);
    vkDestroyFence(s.vk_device, r.fence, nullptr);
    vkDestroyCommandPool(s.vk_device, r.pool, nullptr);
    vkDestroyBuffer(s.vk_device, r.buffer, nullptr);
    vkFreeMemory(s.vk_device, r.memory, nullptr);
    std::thread([done = std::move(r.done), bytes = std::move(bytes)]() mutable {
        done(std::move(bytes));
    }).detach();
}

// Collects finished async readbacks (xrEndFrame).
void PollReadbacks() {
    if (!DumpAsync() || g_session == nullptr) {
        return;
    }
    std::lock_guard lk{g_readback_mutex};
    for (size_t i = 0; i < g_readbacks.size();) {
        if (vkGetFenceStatus(g_session->vk_device, g_readbacks[i].fence) == VK_SUCCESS) {
            FinishReadback(g_readbacks[i]);
            g_readbacks.erase(g_readbacks.begin() + static_cast<std::ptrdiff_t>(i));
        } else {
            ++i;
        }
    }
}

// Called inside xrEndFrame (the application holds its queue lock there): copies `rect` of `image`
// (`aspect`, in `layout` before and after) into a host buffer, `bpp` bytes per texel.
void ReadbackImage(VkImage image, VkImageAspectFlags aspect, VkImageLayout layout,
                   const XrRect2Di& rect, uint32_t bpp,
                   std::function<void(std::vector<uint8_t>)> done) {
    Session& s = *g_session;
    VkDevice dev = s.vk_device;
    VkQueue queue;
    vkGetDeviceQueue(dev, s.queue_family, s.queue_index, &queue);
    const uint32_t width = static_cast<uint32_t>(rect.extent.width);
    const uint32_t height = static_cast<uint32_t>(rect.extent.height);
    Readback r{};
    r.size = VkDeviceSize(width) * height * bpp;
    r.done = std::move(done);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = r.size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    vkCreateBuffer(dev, &bi, nullptr, &r.buffer);
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, r.buffer, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = FindMemoryType(s.vk_physical, req.memoryTypeBits,
                                        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(dev, &ai, nullptr, &r.memory);
    vkBindBufferMemory(dev, r.buffer, r.memory, 0);
    VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pi.queueFamilyIndex = s.queue_family;
    vkCreateCommandPool(dev, &pi, nullptr, &r.pool);
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = r.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(dev, &cai, &cmd);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(cmd, &begin);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.oldLayout = layout;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, 1, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy region{};
    region.imageSubresource = {aspect, 0, 0, 1};
    region.imageOffset = {rect.offset.x, rect.offset.y, 0};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.buffer, 1,
                           &region);
    b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    b.newLayout = layout;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    vkEndCommandBuffer(cmd);
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    vkCreateFence(dev, &fi, nullptr, &r.fence);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    vkQueueSubmit(queue, 1, &si, r.fence);
    if (DumpAsync()) {
        std::lock_guard lk{g_readback_mutex};
        g_readbacks.push_back(std::move(r));
        return;
    }
    vkWaitForFences(dev, 1, &r.fence, VK_TRUE, UINT64_MAX);
    FinishReadback(r);
}

// Called inside xrEndFrame (the application holds its queue lock there). Dumps only `rect`,
// the region the compositor would show.
void DumpImage(const Swapchain& sc, int index, XrRect2Di rect, const std::string& path,
               const std::vector<std::array<int, 2>>& marks = {}, bool alpha = false) {
    rect = ClampRect(sc, rect);
    const uint32_t width = static_cast<uint32_t>(rect.extent.width);
    const uint32_t height = static_cast<uint32_t>(rect.extent.height);
    const bool rgba = sc.format == VK_FORMAT_R8G8B8A8_SRGB || sc.format == VK_FORMAT_R8G8B8A8_UNORM;
    // The marks and the PNG (zlib, ~100 ms per eye) are written on a worker thread, so a dump
    // never stalls the application's frame loop for that (a stall of the app's XR frame thread
    // breaks its stereo pairs and the dump would show that, not the steady state).
    ReadbackImage(
        sc.images[index], VK_IMAGE_ASPECT_COLOR_BIT, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, rect,
        4,
        [marks, path, width, height, rgba, alpha, rect, sc_w = sc.width, sc_h = sc.height,
         format = sc.format, index](std::vector<uint8_t> copy) {
            // A 9 px magenta cross at each mark (the same bytes in BGRA and RGBA).
            for (const auto& m : marks) {
                for (int k = -4; k <= 4; ++k) {
                    for (const auto& [x, y] : {std::array<int, 2>{m[0] + k, m[1]},
                                               std::array<int, 2>{m[0], m[1] + k}}) {
                        if (x < 0 || y < 0 || x >= static_cast<int>(width) ||
                            y >= static_cast<int>(height)) {
                            continue;
                        }
                        uint8_t* px = copy.data() + (static_cast<size_t>(y) * width + x) * 4;
                        px[0] = 255;
                        px[1] = 0;
                        px[2] = 255;
                        px[3] = 255;
                    }
                }
            }
            WritePng(path, width, height, copy.data(), rgba, alpha);
            Log("dumped %s (%ux%u at %d,%d of %ux%u, fmt %lld image %d)", path.c_str(), width,
                height, rect.offset.x, rect.offset.y, sc_w, sc_h, (long long)format, index);
        });
}

XrRect2Di FullRect(const Swapchain& sc) {
    return {{0, 0}, {static_cast<int32_t>(sc.width), static_cast<int32_t>(sc.height)}};
}

// Validates one projection view against its swapchain and the located FOV; logs the
// submitted FOV and rectangle whenever they change (and every problem, up to a limit).
void CheckProjectionView(uint32_t v, const XrCompositionLayerProjectionView& view,
                         const Swapchain& sc) {
    static int problems = 0;
    static XrFovf logged_fov[2]{};
    static XrRect2Di logged_rect[2]{};
    static int logs = 0;
    constexpr float R = 57.2957795f;
    if (v >= 2) {
        return;
    }
    const XrRect2Di& r = view.subImage.imageRect;
    const XrFovf& f = view.fov;
    if (problems < 20 &&
        (r.offset.x < 0 || r.offset.y < 0 || r.extent.width < 1 || r.extent.height < 1 ||
         r.offset.x + r.extent.width > static_cast<int32_t>(sc.width) ||
         r.offset.y + r.extent.height > static_cast<int32_t>(sc.height))) {
        ++problems;
        Log("ERROR: view %u imageRect %d,%d %dx%d outside the %ux%u swapchain", v, r.offset.x,
            r.offset.y, r.extent.width, r.extent.height, sc.width, sc.height);
    }
    if (problems < 20 && (r.extent.width > static_cast<int32_t>(MaxImageRect().width) ||
                          r.extent.height > static_cast<int32_t>(MaxImageRect().height))) {
        ++problems;
        Log("ERROR: view %u imageRect %dx%d larger than the max image rect %ux%u", v,
            r.extent.width, r.extent.height, MaxImageRect().width, MaxImageRect().height);
    }
    const XrFovf located = LocatedFov(v);
    const float tolerance = 0.001f;
    const bool differs = std::fabs(f.angleLeft - located.angleLeft) > tolerance ||
                         std::fabs(f.angleRight - located.angleRight) > tolerance ||
                         std::fabs(f.angleUp - located.angleUp) > tolerance ||
                         std::fabs(f.angleDown - located.angleDown) > tolerance;
    if (QuestFov() && differs && problems < 20) {
        ++problems;
        Log("ERROR: view %u submitted fov l%.2f r%.2f u%.2f d%.2f differs from the located "
            "l%.2f r%.2f u%.2f d%.2f (fovMutable=false: the runtime would ignore it)",
            v, f.angleLeft * R, f.angleRight * R, f.angleUp * R, f.angleDown * R,
            located.angleLeft * R, located.angleRight * R, located.angleUp * R,
            located.angleDown * R);
    }
    const bool changed = std::memcmp(&logged_fov[v], &f, sizeof(f)) != 0 ||
                         std::memcmp(&logged_rect[v], &r, sizeof(r)) != 0;
    if (changed && logs < 40) {
        ++logs;
        logged_fov[v] = f;
        logged_rect[v] = r;
        Log("view %u submits fov l%.2f r%.2f u%.2f d%.2f, rect x %d..%d y %d..%d (%dx%d) of %ux%u",
            v, f.angleLeft * R, f.angleRight * R, f.angleUp * R, f.angleDown * R, r.offset.x,
            r.offset.x + r.extent.width, r.offset.y, r.offset.y + r.extent.height, r.extent.width,
            r.extent.height, sc.width, sc.height);
    }
}

// ---- API implementation ------------------------------------------------------------
// The extensions offered: the base list (MOCKXR_NO_LAYER_EXT / MOCKXR_NO_DEPTH shorten it), then
// MOCKXR_EXTS's (other runtimes' controller extensions).
const std::vector<std::string>& OfferedExtensions() {
    static const std::vector<std::string> offered = [] {
        static const char* exts[] = {XR_KHR_VULKAN_ENABLE_EXTENSION_NAME,
                                     XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME,
                                     XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME,
                                     XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME,
                                     XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME};
        const uint32_t base = std::getenv("MOCKXR_NO_LAYER_EXT") ? 2u
                              : std::getenv("MOCKXR_NO_DEPTH")   ? 4u
                                                                 : 5u;
        std::vector<std::string> list(exts, exts + base);
        if (const char* extra = std::getenv("MOCKXR_EXTS"); extra != nullptr && *extra != '\0') {
            std::string all = extra;
            size_t at = 0;
            while (at <= all.size()) {
                const size_t comma = std::min(all.find(',', at), all.size());
                if (comma > at) {
                    list.push_back(all.substr(at, comma - at));
                }
                at = comma + 1;
            }
            std::string text;
            for (size_t i = base; i < list.size(); ++i) {
                text += " " + list[i];
            }
            Log("MOCKXR_EXTS: also offering%s", text.c_str());
        }
        return list;
    }();
    return offered;
}
std::set<std::string> g_enabled_exts; // xrCreateInstance's enabled extensions
XrVersion g_api_version{XR_MAKE_VERSION(1, 0, 0)}; // xrCreateInstance's apiVersion

XrResult XRAPI_CALL EnumerateInstanceExtensionProperties(const char*, uint32_t cap, uint32_t* count,
                                                         XrExtensionProperties* props) {
    const std::vector<std::string>& offered = OfferedExtensions();
    const uint32_t total = static_cast<uint32_t>(offered.size());
    *count = total;
    if (cap == 0) {
        return XR_SUCCESS;
    }
    for (uint32_t i = 0; i < total && i < cap; ++i) {
        std::strncpy(props[i].extensionName, offered[i].c_str(), XR_MAX_EXTENSION_NAME_SIZE - 1);
        props[i].extensionName[XR_MAX_EXTENSION_NAME_SIZE - 1] = '\0';
        props[i].extensionVersion = i == 0 ? 8 : 1;
    }
    return XR_SUCCESS;
}

// XR_FB_display_refresh_rate: 72 / 90 / 120 Hz; a request changes the frame period at once.
XrResult XRAPI_CALL EnumerateDisplayRefreshRates(XrSession, uint32_t cap, uint32_t* count,
                                                 float* rates) {
    const auto& offered = RefreshRates();
    *count = static_cast<uint32_t>(offered.size());
    if (cap == 0) {
        return XR_SUCCESS;
    }
    if (cap < offered.size()) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    std::copy(offered.begin(), offered.end(), rates);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL GetDisplayRefreshRate(XrSession, float* rate) {
    *rate = static_cast<float>(1e9 / static_cast<double>(g_frame_period.load()));
    return XR_SUCCESS;
}
XrResult XRAPI_CALL RequestDisplayRefreshRate(XrSession, float rate) {
    if (rate != 0.0f &&
        std::find(RefreshRates().begin(), RefreshRates().end(), rate) == RefreshRates().end()) {
        Log("ERROR: refresh rate %.1f Hz requested, not supported", rate);
        return XR_ERROR_DISPLAY_REFRESH_RATE_UNSUPPORTED_FB;
    }
    const float hz = rate == 0.0f ? RefreshRates().back() : rate;
    g_frame_period.store(static_cast<int64_t>(1e9 / hz));
    Log("refresh rate %.1f Hz requested -> period %lld ns", rate,
        static_cast<long long>(g_frame_period.load()));
    return XR_SUCCESS;
}

XrResult XRAPI_CALL CreateInstance(const XrInstanceCreateInfo* info, XrInstance* instance) {
    const XrVersion api = info->applicationInfo.apiVersion;
    if (std::getenv("MOCKXR_API_1_0") != nullptr &&
        (XR_VERSION_MAJOR(api) > 1 || (XR_VERSION_MAJOR(api) == 1 && XR_VERSION_MINOR(api) > 0))) {
        Log("xrCreateInstance api %u.%u: MOCKXR_API_1_0, only 1.0 (XR_ERROR_API_VERSION_UNSUPPORTED)",
            static_cast<unsigned>(XR_VERSION_MAJOR(api)),
            static_cast<unsigned>(XR_VERSION_MINOR(api)));
        return XR_ERROR_API_VERSION_UNSUPPORTED;
    }
    std::string enabled;
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        const std::string name = info->enabledExtensionNames[i];
        if (std::ranges::find(OfferedExtensions(), name) == OfferedExtensions().end()) {
            Log("ERROR: xrCreateInstance: extension %s is not offered "
                "(XR_ERROR_EXTENSION_NOT_PRESENT)",
                name.c_str());
            return XR_ERROR_EXTENSION_NOT_PRESENT;
        }
        enabled += " " + name;
    }
    g_enabled_exts.clear();
    for (uint32_t i = 0; i < info->enabledExtensionCount; ++i) {
        g_enabled_exts.insert(info->enabledExtensionNames[i]);
    }
    g_api_version = api;
    *instance = NewHandle<XrInstance>();
    Log("xrCreateInstance app=%s api %u.%u, extensions:%s", info->applicationInfo.applicationName,
        static_cast<unsigned>(XR_VERSION_MAJOR(api)), static_cast<unsigned>(XR_VERSION_MINOR(api)),
        enabled.c_str());
    return XR_SUCCESS;
}
XrResult XRAPI_CALL DestroyInstance(XrInstance) {
    return XR_SUCCESS;
}
XrResult XRAPI_CALL GetInstanceProperties(XrInstance, XrInstanceProperties* p) {
    std::strcpy(p->runtimeName, "MockXR");
    p->runtimeVersion = 1;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL GetSystem(XrInstance, const XrSystemGetInfo*, XrSystemId* id) {
    *id = 1;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL GetSystemProperties(XrInstance, XrSystemId, XrSystemProperties* p) {
    std::strcpy(p->systemName, "MockXR HMD");
    p->graphicsProperties.maxSwapchainImageWidth = MaxSwapchain().width;
    p->graphicsProperties.maxSwapchainImageHeight = MaxSwapchain().height;
    p->trackingProperties.orientationTracking = XR_TRUE;
    p->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

XrResult FillString(const char* s, uint32_t cap, uint32_t* count, char* buf) {
    *count = static_cast<uint32_t>(std::strlen(s) + 1);
    if (cap) {
        std::strcpy(buf, s);
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL GetVulkanInstanceExtensions(XrInstance, XrSystemId, uint32_t cap,
                                                uint32_t* count, char* buf) {
    return FillString("VK_KHR_surface", cap, count, buf);
}
XrResult XRAPI_CALL GetVulkanDeviceExtensions(XrInstance, XrSystemId, uint32_t cap,
                                              uint32_t* count, char* buf) {
    return FillString("VK_KHR_swapchain", cap, count, buf);
}
XrResult XRAPI_CALL GetVulkanGraphicsDevice(XrInstance, XrSystemId, VkInstance vk,
                                            VkPhysicalDevice* pd) {
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(vk, &n, nullptr);
    std::vector<VkPhysicalDevice> devices(n);
    vkEnumeratePhysicalDevices(vk, &n, devices.data());
    *pd = devices.empty() ? VK_NULL_HANDLE : devices[0];
    for (auto d : devices) {
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(d, &props);
        if (props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
            *pd = d;
            break;
        }
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL GetVulkanGraphicsRequirements(XrInstance, XrSystemId,
                                                  XrGraphicsRequirementsVulkanKHR* req) {
    req->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    req->maxApiVersionSupported = XR_MAKE_VERSION(1, 4, 0);
    return XR_SUCCESS;
}

XrResult XRAPI_CALL CreateSession(XrInstance, const XrSessionCreateInfo* info, XrSession* out) {
    const auto* binding = static_cast<const XrGraphicsBindingVulkanKHR*>(info->next);
    if (!binding || binding->type != XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) {
        return XR_ERROR_GRAPHICS_DEVICE_INVALID;
    }
    (void)RefreshRates(); // MOCKXR_RATES: the first period
    g_session = new Session{};
    g_session->vk_instance = binding->instance;
    g_session->vk_physical = binding->physicalDevice;
    g_session->vk_device = binding->device;
    g_session->queue_family = binding->queueFamilyIndex;
    g_session->queue_index = binding->queueIndex;
    g_session->events.push_back(XR_SESSION_STATE_READY);
    *out = NewHandle<XrSession>();
    Log("xrCreateSession ok (queue family %u)", binding->queueFamilyIndex);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL DestroySession(XrSession) {
    Log("xrDestroySession: frames=%llu projection=%llu quad=%llu overlay=%llu empty=%llu",
        (unsigned long long)g_session->frames, (unsigned long long)g_session->projection_frames,
        (unsigned long long)g_session->quad_frames, (unsigned long long)g_session->overlay_frames,
        (unsigned long long)g_session->empty_frames);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL BeginSession(XrSession, const XrSessionBeginInfo*) {
    std::lock_guard lk{g_mutex};
    g_session->running = true;
    g_session->events.push_back(XR_SESSION_STATE_SYNCHRONIZED);
    g_session->events.push_back(XR_SESSION_STATE_VISIBLE);
    g_session->events.push_back(XR_SESSION_STATE_FOCUSED);
    return XR_SUCCESS;
}
// ---- MOCKXR_STOP_AFTER_S: Quit in the headset's menu (the Quest runtime's sequence) ------------
double StopAfterS() { // < 0: off
    static const double after = [] {
        const char* v = std::getenv("MOCKXR_STOP_AFTER_S");
        if (v == nullptr || *v == '\0') {
            return -1.0;
        }
        const double s = std::max(std::atof(v), 0.0);
        Log("MOCKXR stop script: Quit in the headset's menu %.1f s after the first FOCUSED "
            "(VISIBLE, SYNCHRONIZED, STOPPING 0.2 s apart; IDLE after xrEndSession; nothing more)",
            s);
        return s;
    }();
    return after;
}
std::atomic<int64_t> g_first_focused{0}; // NowNs when the session first went FOCUSED (0: not yet)
int g_stop_phase{0};                     // stop states queued (g_mutex): VISIBLE, SYNCHRONIZED, STOPPING
bool g_stop_ended{false};                // xrEndSession after the stop (g_mutex): IDLE queued
std::atomic<int> g_stop_frame_errors{0}; // frame calls after the session ended

void ApplyStopScript() {
    const double after = StopAfterS();
    const int64_t focused = g_first_focused.load();
    if (after < 0.0 || focused == 0 || g_session == nullptr) {
        return;
    }
    static constexpr XrSessionState states[3] = {
        XR_SESSION_STATE_VISIBLE, XR_SESSION_STATE_SYNCHRONIZED, XR_SESSION_STATE_STOPPING};
    static constexpr const char* names[3] = {"VISIBLE", "SYNCHRONIZED", "STOPPING"};
    const double t = static_cast<double>(NowNs() - focused) * 1e-9;
    std::lock_guard lk{g_mutex};
    while (g_stop_phase < 3 && t >= after + 0.2 * g_stop_phase) {
        g_session->events.push_back(states[g_stop_phase]);
        Log("MOCKXR stop: %s queued %.3f s after the first FOCUSED", names[g_stop_phase], t);
        ++g_stop_phase;
    }
}

// With MOCKXR_STOP_AFTER_S: a frame call after the stopped session ended (a runtime's
// XR_ERROR_SESSION_NOT_RUNNING). Returns true when the call must fail.
bool StopFrameRefused(const char* call) {
    if (StopAfterS() < 0.0 || g_session == nullptr || g_session->running) {
        return false;
    }
    bool ended = false;
    {
        std::lock_guard lk{g_mutex};
        ended = g_stop_ended;
    }
    if (!ended) {
        return false;
    }
    if (g_stop_frame_errors.fetch_add(1) < 10) {
        Log("ERROR %s after the session ended (xrEndSession): XR_ERROR_SESSION_NOT_RUNNING", call);
    }
    return true;
}

XrResult XRAPI_CALL EndSession(XrSession) {
    if (StopAfterS() >= 0.0) {
        std::lock_guard lk{g_mutex};
        if (g_session->state != XR_SESSION_STATE_STOPPING) {
            Log("ERROR xrEndSession in state %d (not STOPPING): XR_ERROR_SESSION_NOT_STOPPING",
                static_cast<int>(g_session->state));
            return XR_ERROR_SESSION_NOT_STOPPING;
        }
        g_session->running = false;
        g_stop_ended = true;
        g_session->events.push_back(XR_SESSION_STATE_IDLE);
        Log("MOCKXR stop: xrEndSession %.3f s after the first FOCUSED: IDLE queued (nothing more)",
            static_cast<double>(NowNs() - g_first_focused.load()) * 1e-9);
        return XR_SUCCESS;
    }
    g_session->running = false;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL RequestExitSession(XrSession) {
    return XR_SUCCESS;
}
void ApplyHeadEvents(); // input.txt drift / recenter (below)
// input.txt `recenter_pose`: its events (pose and changeTime) to send (g_space_event_mutex).
std::mutex g_space_event_mutex;
std::deque<XrEventDataReferenceSpaceChangePending> g_space_change_events;
void ApplyFocusScript(); // MOCKXR_UNFOCUS (below)
std::atomic<int> g_space_changes{0}; // REFERENCE_SPACE_CHANGE_PENDING events to send

XrResult XRAPI_CALL PollEvent(XrInstance, XrEventDataBuffer* buffer) {
    ApplyHeadEvents();
    ApplyFocusScript();
    ApplyStopScript();
    std::lock_guard lk{g_mutex};
    if (g_session && g_space_changes.load() > 0) {
        --g_space_changes;
        auto* c = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(buffer);
        *c = {XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING};
        c->referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        c->changeTime = NowNs();
        c->poseValid = XR_FALSE;
        c->poseInPreviousSpace = {{0, 0, 0, 1}, {0, 0, 0}};
        Log("MOCKXR event: REFERENCE_SPACE_CHANGE_PENDING (LOCAL)");
        return XR_SUCCESS;
    }
    if (g_session) {
        std::lock_guard sk{g_space_event_mutex};
        if (!g_space_change_events.empty()) {
            auto* c = reinterpret_cast<XrEventDataReferenceSpaceChangePending*>(buffer);
            *c = g_space_change_events.front();
            g_space_change_events.pop_front();
            Log("MOCKXR event: REFERENCE_SPACE_CHANGE_PENDING (LOCAL, pose given, changeTime "
                "+%.1f ms)",
                static_cast<double>(c->changeTime - NowNs()) * 1e-6);
            return XR_SUCCESS;
        }
    }
    if (!g_session || g_session->events.empty()) {
        return XR_EVENT_UNAVAILABLE;
    }
    auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(buffer);
    e->type = XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED;
    e->next = nullptr;
    e->session = nullptr;
    e->state = g_session->events.front();
    e->time = NowNs();
    g_session->state = e->state;
    g_session->events.pop_front();
    if (e->state == XR_SESSION_STATE_FOCUSED && g_first_focused.load() == 0) {
        g_first_focused = e->time; // MOCKXR_STOP_AFTER_S counts from here
    }
    if (StopAfterS() >= 0.0 && g_stop_phase > 0) {
        static constexpr const char* names[] = {"UNKNOWN", "IDLE",     "READY",
                                                "SYNCHRONIZED", "VISIBLE", "FOCUSED",
                                                "STOPPING", "LOSS_PENDING", "EXITING"};
        const int s = static_cast<int>(e->state);
        Log("MOCKXR stop: delivered %s", s >= 0 && s <= 8 ? names[s] : "?");
    }
    return XR_SUCCESS;
}

// Reference spaces created with a poseInReferenceSpace (an application's own recentred LOCAL or
// STAGE space): their pose in the mock's world (LOCAL). xrLocateSpace / xrLocateViews return poses
// relative to such a base space, and xrEndFrame converts layers submitted in one to the world
// before every check and dump (EndFrameInWorld), so the logs, MOCKXR_ASSOC and the dumps' display
// poses stay comparable. Spaces without a pose (identity) behave exactly as before.
std::mutex g_space_pose_mutex;
std::map<XrSpace, XrPosef> g_space_pose;
// input.txt `recenter_pose` (g_space_pose_mutex): the LOCAL reference spaces, the runtime's LOCAL
// in the world before and from `at` (changeTime) on, and the events to send.
std::set<XrSpace> g_local_spaces;
struct RuntimeLocal {
    bool active{false};
    XrPosef before{{0, 0, 0, 1}, {0, 0, 0}};
    XrPosef after{{0, 0, 0, 1}, {0, 0, 0}};
    int64_t at{0};
};
RuntimeLocal g_runtime_local;
XrPosef ComposePose(const XrPosef& a, const XrPosef& b);

bool IsIdentityPose(const XrPosef& p) {
    return p.orientation.x == 0.0f && p.orientation.y == 0.0f && p.orientation.z == 0.0f &&
           p.orientation.w == 1.0f && p.position.x == 0.0f && p.position.y == 0.0f &&
           p.position.z == 0.0f;
}
// The world pose of an offset reference space (false: none, the world itself) at `time` (0 =
// now): after a `recenter_pose`, a LOCAL space's is the moved LOCAL's from its changeTime on.
bool SpaceOffset(XrSpace space, XrPosef& pose, int64_t time = 0) {
    std::lock_guard lk{g_space_pose_mutex};
    const auto it = g_space_pose.find(space);
    const bool own = it != g_space_pose.end();
    if (g_runtime_local.active && g_local_spaces.count(space) != 0) {
        const int64_t t = time != 0 ? time : NowNs();
        const XrPosef& local = t >= g_runtime_local.at ? g_runtime_local.after
                                                       : g_runtime_local.before;
        pose = own ? ComposePose(local, it->second) : local;
        return true;
    }
    if (!own) {
        return false;
    }
    pose = it->second;
    return true;
}
XrQuaternionf Conjugate(const XrQuaternionf& q) {
    return {-q.x, -q.y, -q.z, q.w};
}
// a * b (b given in a's frame).
XrPosef ComposePose(const XrPosef& a, const XrPosef& b) {
    const XrVector3f p = Rotate(a.orientation, b.position);
    return {Multiply(a.orientation, b.orientation),
            {a.position.x + p.x, a.position.y + p.y, a.position.z + p.z}};
}
// A world pose in the frame `base` (base^-1 * pose).
XrPosef PoseInBase(const XrPosef& base, const XrPosef& pose) {
    const XrQuaternionf inv = Conjugate(base.orientation);
    const XrVector3f d{pose.position.x - base.position.x, pose.position.y - base.position.y,
                       pose.position.z - base.position.z};
    return {Multiply(inv, pose.orientation), Rotate(inv, d)};
}

// Height tests: the floor-level spaces. MOCKXR_FLOOR=<y>: the floor's height in the mock's world
// (the runtime's LOCAL; default 0, the floor the scripted head at 1.6 m stands on), e.g. 0.149 for
// a 155 cm player (eye 1.451 m) or -0.178 for 190 cm (eye 1.778 m). STAGE and LOCAL_FLOOR have their
// origin at (0, y, 0); LOCAL_FLOOR moves with LOCAL (input.txt `recenter_pose`), STAGE never.
// xrEnumerateReferenceSpaces lists VIEW, LOCAL, STAGE and, with an OpenXR 1.1 instance or
// XR_EXT_local_floor enabled, LOCAL_FLOOR (as Meta's runtime does). MOCKXR_NO_FLOOR=1: neither
// floor space (a runtime without one); MOCKXR_NO_LOCAL_FLOOR=1: STAGE only. Creating a space that
// is not listed fails with XR_ERROR_REFERENCE_SPACE_UNSUPPORTED, as the spec says.
float FloorY() {
    static const float y = [] {
        const char* value = std::getenv("MOCKXR_FLOOR");
        const float v = value != nullptr ? std::strtof(value, nullptr) : 0.0f;
        if (value != nullptr) {
            Log("MOCKXR floor at y %.3f (the head's 1.6 m is %.3f m over it)", v, 1.6f - v);
        }
        return v;
    }();
    return y;
}
bool SpaceListed(XrReferenceSpaceType type) {
    switch (type) {
    case XR_REFERENCE_SPACE_TYPE_VIEW:
    case XR_REFERENCE_SPACE_TYPE_LOCAL:
        return true;
    case XR_REFERENCE_SPACE_TYPE_STAGE:
        return std::getenv("MOCKXR_NO_FLOOR") == nullptr;
    case XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR:
        return std::getenv("MOCKXR_NO_FLOOR") == nullptr &&
               std::getenv("MOCKXR_NO_LOCAL_FLOOR") == nullptr &&
               (XR_VERSION_MINOR(g_api_version) >= 1 || XR_VERSION_MAJOR(g_api_version) > 1 ||
                g_enabled_exts.contains("XR_EXT_local_floor"));
    default:
        return false;
    }
}
XrResult XRAPI_CALL EnumerateReferenceSpaces(XrSession, uint32_t cap, uint32_t* count,
                                             XrReferenceSpaceType* types) {
    std::vector<XrReferenceSpaceType> listed;
    for (const XrReferenceSpaceType t :
         {XR_REFERENCE_SPACE_TYPE_VIEW, XR_REFERENCE_SPACE_TYPE_LOCAL,
          XR_REFERENCE_SPACE_TYPE_STAGE, XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR}) {
        if (SpaceListed(t)) {
            listed.push_back(t);
        }
    }
    *count = static_cast<uint32_t>(listed.size());
    if (cap == 0) {
        return XR_SUCCESS;
    }
    if (cap < listed.size()) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    std::string text;
    for (size_t i = 0; i < listed.size(); ++i) {
        types[i] = listed[i];
        text += listed[i] == XR_REFERENCE_SPACE_TYPE_VIEW    ? " VIEW"
                : listed[i] == XR_REFERENCE_SPACE_TYPE_LOCAL ? " LOCAL"
                : listed[i] == XR_REFERENCE_SPACE_TYPE_STAGE ? " STAGE"
                                                             : " LOCAL_FLOOR";
    }
    Log("xrEnumerateReferenceSpaces:%s (floor at y %.3f)", text.c_str(), FloorY());
    return XR_SUCCESS;
}

XrResult XRAPI_CALL CreateReferenceSpace(XrSession, const XrReferenceSpaceCreateInfo* info,
                                         XrSpace* space) {
    if (!SpaceListed(info->referenceSpaceType)) {
        Log("xrCreateReferenceSpace type %d is not listed: XR_ERROR_REFERENCE_SPACE_UNSUPPORTED",
            static_cast<int>(info->referenceSpaceType));
        return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    }
    *space = NewHandle<XrSpace>();
    g_spaces[*space] = info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_VIEW ? 1 : 0;
    if (info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL ||
        info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR) {
        std::lock_guard lk{g_space_pose_mutex};
        g_local_spaces.insert(*space);
    }
    if (info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ||
        info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR) {
        // Always an offset space (its origin is located even at the world's).
        const XrPosef floor{{0.0f, 0.0f, 0.0f, 1.0f}, {0.0f, FloorY(), 0.0f}};
        const XrPosef p = ComposePose(floor, info->poseInReferenceSpace);
        {
            std::lock_guard lk{g_space_pose_mutex};
            g_space_pose[*space] = p;
        }
        Log("MOCKXR space: %s space %llu, origin (%.4f, %.4f, %.4f) m in LOCAL",
            info->referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE ? "STAGE" : "LOCAL_FLOOR",
            (unsigned long long)reinterpret_cast<uintptr_t>(*space), p.position.x, p.position.y,
            p.position.z);
        return XR_SUCCESS;
    }
    if (info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_VIEW &&
        !IsIdentityPose(info->poseInReferenceSpace)) {
        const auto& p = info->poseInReferenceSpace;
        {
            std::lock_guard lk{g_space_pose_mutex};
            g_space_pose[*space] = p;
        }
        const float yaw = 2.0f * std::atan2(p.orientation.y, p.orientation.w) * 57.29578f;
        Log("MOCKXR space: reference space %llu at yaw %.2f deg (q %.5f %.5f %.5f %.5f), "
            "(%.4f, %.4f, %.4f) m in LOCAL",
            (unsigned long long)reinterpret_cast<uintptr_t>(*space), yaw, p.orientation.x,
            p.orientation.y, p.orientation.z, p.orientation.w, p.position.x, p.position.y,
            p.position.z);
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL CreateActionSpace(XrSession, const XrActionSpaceCreateInfo* info,
                                      XrSpace* space) {
    *space = NewHandle<XrSpace>();
    g_spaces[*space] = 2;
    static const std::array<const char*, 4> roles{"aim_left", "aim_right", "grip_pose_left",
                                                  "grip_pose_right"};
    const std::string& name = g_actions[info->action].name;
    for (int i = 0; i < 4; ++i) {
        if (name == roles[static_cast<size_t>(i)]) {
            g_space_roles[*space] = i;
        }
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL DestroySpace(XrSpace space) {
    std::lock_guard lk{g_space_pose_mutex};
    g_space_pose.erase(space);
    g_local_spaces.erase(space);
    return XR_SUCCESS;
}
XrResult LocateSpaceWorld(XrSpace space, XrTime time, XrSpaceLocation* loc);
// The located pose relative to `base` (an offset reference space; the world otherwise).
XrResult XRAPI_CALL LocateSpace(XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* loc) {
    const XrResult result = LocateSpaceWorld(space, time, loc);
    XrPosef own{};
    if (SpaceOffset(space, own, time)) {
        loc->pose = own; // an offset reference space itself: its origin
    }
    XrPosef offset{};
    if (result == XR_SUCCESS && SpaceOffset(base, offset, time)) {
        loc->pose = PoseInBase(offset, loc->pose);
        if (auto* v = static_cast<XrSpaceVelocity*>(const_cast<void*>(loc->next));
            v && v->type == XR_TYPE_SPACE_VELOCITY) {
            const XrQuaternionf inv = Conjugate(offset.orientation);
            v->linearVelocity = Rotate(inv, v->linearVelocity);
            v->angularVelocity = Rotate(inv, v->angularVelocity);
        }
    }
    return result;
}
XrResult LocateSpaceWorld(XrSpace space, XrTime time, XrSpaceLocation* loc) {
    loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT |
                         XR_SPACE_LOCATION_POSITION_VALID_BIT |
                         XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT |
                         XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    const auto role = g_space_roles.find(space);
    BP::HandPose hand{};
    if (role != g_space_roles.end() && ScriptedHand(role->second & 1, time, hand)) {
        // MOCKXR_HANDS: the scripted grip (the aim pose is the same).
        loc->pose = {{hand.q[0], hand.q[1], hand.q[2], hand.q[3]},
                     {hand.p[0], hand.p[1], hand.p[2]}};
        if (!hand.tracked) {
            loc->locationFlags &= ~XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
        }
        if (auto* v = static_cast<XrSpaceVelocity*>(const_cast<void*>(loc->next));
            v && v->type == XR_TYPE_SPACE_VELOCITY) {
            v->velocityFlags =
                XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
            ScriptedVelocity(role->second & 1, time, v->linearVelocity, v->angularVelocity);
        }
        return XR_SUCCESS;
    }
    if (g_spaces[space] == 1) {
        HeadPose(time, loc->pose);
    } else {
        float follow[3];
        HandFollowOffset(time, follow);
        loc->pose = {{0, 0, 0, 1}, {0.2f + follow[0], 1.2f + follow[1], -0.3f + follow[2]}};
    }
    if (auto* v = static_cast<XrSpaceVelocity*>(const_cast<void*>(loc->next));
        v && v->type == XR_TYPE_SPACE_VELOCITY) {
        v->velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
        v->linearVelocity = {0, 0, 0};
        const double ph = std::fmod(static_cast<double>(time) * 1e-9, 6.0);
        if (std::getenv("MOCKXR_INPUT") && g_spaces[space] == 2 && ph >= 2.0 && ph < 2.15) {
            v->linearVelocity = {3.5f, 0, 0};
        }
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL EnumerateEnvironmentBlendModes(XrInstance, XrSystemId, XrViewConfigurationType,
                                                   uint32_t cap, uint32_t* count,
                                                   XrEnvironmentBlendMode* modes) {
    *count = 1;
    if (cap) {
        modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL EnumerateViewConfigurationViews(XrInstance, XrSystemId, XrViewConfigurationType,
                                                    uint32_t cap, uint32_t* count,
                                                    XrViewConfigurationView* views) {
    *count = 2;
    for (uint32_t i = 0; i < cap && i < 2; ++i) {
        views[i].recommendedImageRectWidth = RecommendedRect().width;
        views[i].recommendedImageRectHeight = RecommendedRect().height;
        views[i].maxImageRectWidth = MaxImageRect().width;
        views[i].maxImageRectHeight = MaxImageRect().height;
        views[i].recommendedSwapchainSampleCount = 1;
        views[i].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL EnumerateSwapchainFormats(XrSession, uint32_t cap, uint32_t* count,
                                              int64_t* formats) {
    static const int64_t f[] = {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_SRGB,
                                VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_D16_UNORM,
                                VK_FORMAT_D32_SFLOAT};
    *count = 5;
    for (uint32_t i = 0; i < cap && i < 5; ++i) {
        formats[i] = f[i];
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL CreateSwapchain(XrSession, const XrSwapchainCreateInfo* info,
                                    XrSwapchain* out) {
    Session& s = *g_session;
    Swapchain sc;
    sc.format = info->format;
    sc.width = info->width;
    sc.height = info->height;
    sc.depth = IsDepthFormat(info->format);
    if (info->width > MaxSwapchain().width || info->height > MaxSwapchain().height) {
        Log("ERROR: xrCreateSwapchain %ux%u larger than the largest swapchain %ux%u", info->width,
            info->height, MaxSwapchain().width, MaxSwapchain().height);
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (sc.depth && (info->usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) == 0) {
        Log("ERROR: depth swapchain format %lld without DEPTH_STENCIL_ATTACHMENT usage",
            (long long)info->format);
    }
    if (info->arraySize > 1 || (sc.depth && info->mipCount > 1)) {
        Log("ERROR: swapchain %ux%u with arraySize %u mipCount %u (the mock makes 1 and 1)",
            info->width, info->height, info->arraySize, info->mipCount);
    }
    const uint32_t mips = sc.depth ? 1u : std::max(info->mipCount, 1u);
    if (mips > 1 && (info->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT) == 0) {
        Log("ERROR: swapchain %ux%u with %u mip levels without TRANSFER_SRC usage", info->width,
            info->height, mips);
    }
    for (int i = 0; i < 3; ++i) {
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = static_cast<VkFormat>(info->format);
        ci.extent = {info->width, info->height, 1};
        ci.mipLevels = mips;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.usage = (sc.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                             : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                   VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                   VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VkImage image;
        if (vkCreateImage(s.vk_device, &ci, nullptr, &image) != VK_SUCCESS) {
            Log("vkCreateImage failed");
            return XR_ERROR_RUNTIME_FAILURE;
        }
        VkMemoryRequirements req;
        vkGetImageMemoryRequirements(s.vk_device, image, &req);
        VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        ai.allocationSize = req.size;
        ai.memoryTypeIndex = FindMemoryType(s.vk_physical, req.memoryTypeBits,
                                            VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        VkDeviceMemory mem;
        vkAllocateMemory(s.vk_device, &ai, nullptr, &mem);
        vkBindImageMemory(s.vk_device, image, mem, 0);
        sc.images.push_back(image);
        sc.memory.push_back(mem);
    }
    *out = NewHandle<XrSwapchain>();
    std::lock_guard lk{g_mutex};
    g_swapchains[*out] = std::move(sc);
    Log("xrCreateSwapchain %ux%u format %lld%s", info->width, info->height,
        (long long)info->format, mips > 1 ? (" mips " + std::to_string(mips)).c_str() : "");
    return XR_SUCCESS;
}
XrResult XRAPI_CALL DestroySwapchain(XrSwapchain h) {
    std::lock_guard lk{g_mutex};
    auto& sc = g_swapchains[h];
    for (size_t i = 0; i < sc.images.size(); ++i) {
        vkDestroyImage(g_session->vk_device, sc.images[i], nullptr);
        vkFreeMemory(g_session->vk_device, sc.memory[i], nullptr);
    }
    g_swapchains.erase(h);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL EnumerateSwapchainImages(XrSwapchain h, uint32_t cap, uint32_t* count,
                                             XrSwapchainImageBaseHeader* images) {
    std::lock_guard lk{g_mutex};
    auto& sc = g_swapchains[h];
    *count = static_cast<uint32_t>(sc.images.size());
    auto* vk = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
    for (uint32_t i = 0; i < cap && i < sc.images.size(); ++i) {
        vk[i].image = sc.images[i];
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL AcquireSwapchainImage(XrSwapchain h, const XrSwapchainImageAcquireInfo*,
                                          uint32_t* index) {
    std::lock_guard lk{g_mutex};
    auto& sc = g_swapchains[h];
    if (sc.acquired >= 0) {
        Log("ERROR: acquire while an image is still acquired");
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    sc.acquired = static_cast<int>(sc.next);
    sc.next = (sc.next + 1) % sc.images.size();
    *index = static_cast<uint32_t>(sc.acquired);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL WaitSwapchainImage(XrSwapchain h, const XrSwapchainImageWaitInfo*) {
    std::lock_guard lk{g_mutex};
    g_swapchains[h].waited = true;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL ReleaseSwapchainImage(XrSwapchain h, const XrSwapchainImageReleaseInfo*) {
    std::lock_guard lk{g_mutex};
    auto& sc = g_swapchains[h];
    if (sc.acquired < 0 || !sc.waited) {
        Log("ERROR: release without acquire/wait");
        return XR_ERROR_CALL_ORDER_INVALID;
    }
    sc.last_released = sc.acquired;
    sc.acquired = -1;
    sc.waited = false;
    if (LatchNs() > 0) {
        // The application holds its queue lock across the release (XR_KHR_vulkan_enable): a
        // fence submitted now signals once all GPU work queued before the release is done.
        Session& s = *g_session;
        const size_t i = static_cast<size_t>(sc.last_released);
        std::lock_guard fl{g_fence_mutex};
        if (sc.release_fence.size() != sc.images.size()) {
            sc.release_fence.assign(sc.images.size(), VK_NULL_HANDLE);
        }
        if (sc.release_fence[i] == VK_NULL_HANDLE) {
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            vkCreateFence(s.vk_device, &fi, nullptr, &sc.release_fence[i]);
        } else {
            // A fence still pending from the previous release (the GPU is far behind) is waited
            // for before it can be reused.
            vkWaitForFences(s.vk_device, 1, &sc.release_fence[i], VK_TRUE, UINT64_MAX);
            vkResetFences(s.vk_device, 1, &sc.release_fence[i]);
        }
        VkQueue queue;
        vkGetDeviceQueue(s.vk_device, s.queue_family, s.queue_index, &queue);
        vkQueueSubmit(queue, 0, nullptr, sc.release_fence[i]);
        g_fence_serial[sc.release_fence[i]] = ++g_release_serial;
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL WaitFrame(XrSession, const XrFrameWaitInfo*, XrFrameState* state) {
    if (StopFrameRefused("xrWaitFrame")) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    int64_t now = NowNs();
    if (g_session->next_frame == 0) {
        g_session->next_frame = now;
    }
    const int64_t period = g_frame_period.load();
    // MOCKXR_SKIP_EVERY / MOCKXR_THROTTLE (frame timing tests).
    static const int skip_every = [] {
        const char* v = std::getenv("MOCKXR_SKIP_EVERY");
        return v != nullptr ? std::max(std::atoi(v), 0) : 0;
    }();
    static const int throttle = [] {
        const char* v = std::getenv("MOCKXR_THROTTLE");
        const int k = v != nullptr ? std::atoi(v) : 1;
        if (v != nullptr) {
            Log("MOCKXR wait: every frame waits %d display periods (the app at refresh / %d)",
                std::max(k, 1), std::max(k, 1));
        }
        return std::max(k, 1);
    }();
    static uint64_t waits = 0;
    ++waits;
    int64_t advance = period * throttle;
    if (skip_every > 0 && waits % static_cast<uint64_t>(skip_every) == 0) {
        advance += period; // a refresh the application misses
        static int skip_logs = 0;
        if (skip_logs++ == 0) {
            Log("MOCKXR wait: every %d-th frame skips a display period", skip_every);
        }
    }
    g_session->next_frame += advance;
    if (g_session->next_frame > now) {
        std::this_thread::sleep_for(std::chrono::nanoseconds(g_session->next_frame - now));
    } else {
        g_session->next_frame = now;
    }
    state->predictedDisplayTime = g_session->next_frame + 2 * period;
    state->predictedDisplayPeriod = period;
    g_display_time.store(state->predictedDisplayTime);
    state->shouldRender = XR_TRUE;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL BeginFrame(XrSession, const XrFrameBeginInfo*) {
    if (StopFrameRefused("xrBeginFrame")) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    return XR_SUCCESS;
}
// ---- MOCKXR_STAMPS ------------------------------------------------------------------
// Readback layout: view v's 64x16 stamp strip at v * StampViewBytes (4 rows of 256 bytes per 4
// lines), then the MOCKXR_ASSOC samples (4 bytes each) at AssocOffset.
constexpr uint32_t StampViewBytes = 64 * 16 * 4;
constexpr uint32_t AssocSamples = 64;
constexpr uint32_t AssocOffset = 2 * StampViewBytes;
constexpr uint32_t SigSamples = 64;
constexpr uint32_t SigOffset = AssocOffset + AssocSamples * 4;
constexpr uint32_t StampBufferBytes = SigOffset + SigSamples * 4;

struct StampSlot {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    uint8_t* mapped{};
    VkCommandBuffer cmd{};
    VkFence fence{};
    bool busy{false};
    uint64_t frame{};
    int image[2]{};
    bool assoc[AssocSamples]{}; // the sample landed inside the submitted image
};
struct StampReader {
    bool ready{false};
    VkCommandPool pool{};
    std::array<StampSlot, 8> slots{};
    uint32_t next{0};
};

bool StampsEnabled() {
    static const bool enabled = std::getenv("MOCKXR_STAMPS") != nullptr;
    return enabled;
}

StampReader& Stamps() {
    static StampReader reader;
    return reader;
}

bool AssocEnabled() {
    static const bool enabled = std::getenv("MOCKXR_ASSOC") != nullptr;
    return enabled;
}

bool SigEnabled() {
    static const bool enabled = std::getenv("MOCKXR_SIG") != nullptr;
    return enabled;
}

// 64x16 RGBA/BGRA texels per view; stamp row r (0 = the bottom one) is texel lines 12 - 4r ..
// 15 - 4r, cell k texels 4k..4k+3, read at its centre (G channel).
uint32_t DecodeStamp(const uint8_t* strip, uint32_t row = 0) {
    uint32_t value = 0;
    const uint32_t line = 4 * (3 - row) + 2;
    for (uint32_t k = 0; k < 16; ++k) {
        const uint8_t* px = strip + (line * 64 + 4 * k + 2) * 4;
        if (px[1] > 127) {
            value |= 1u << k;
        }
    }
    return value;
}

void FinishStamp(Session& s, StampSlot& slot, bool wait) {
    if (!slot.busy) {
        return;
    }
    if (wait) {
        vkWaitForFences(s.vk_device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
    } else if (vkGetFenceStatus(s.vk_device, slot.fence) != VK_SUCCESS) {
        return;
    }
    slot.busy = false;
    const uint8_t* left = slot.mapped;
    const uint8_t* right = slot.mapped + StampViewBytes;
    char reg[48] = "";
    if (AssocEnabled()) {
        // Mean absolute colour change of the world-fixed samples against the previous frame.
        static uint8_t previous[AssocSamples * 4]{};
        static bool previous_valid[AssocSamples]{};
        const uint8_t* samples = slot.mapped + AssocOffset;
        double sum = 0.0;
        uint32_t count = 0;
        for (uint32_t i = 0; i < AssocSamples; ++i) {
            if (slot.assoc[i] && previous_valid[i]) {
                for (uint32_t c = 0; c < 3; ++c) {
                    sum += std::abs(int(samples[i * 4 + c]) - int(previous[i * 4 + c]));
                }
                ++count;
            }
            previous_valid[i] = slot.assoc[i];
        }
        std::memcpy(previous, samples, sizeof(previous));
        std::snprintf(reg, sizeof(reg), " reg=%.1f n=%u", count ? sum / (3.0 * count) : -1.0,
                      count);
    }
    char sig[8 + SigSamples * 2] = "";
    if (SigEnabled()) {
        const uint8_t* samples = slot.mapped + SigOffset;
        std::strcpy(sig, " sig=");
        for (uint32_t i = 0; i < SigSamples; ++i) {
            const uint8_t* px = samples + i * 4; // B, G, R or R, G, B: luma close enough either way
            const int luma = (px[0] * 29 + px[1] * 150 + px[2] * 77) >> 8;
            std::snprintf(sig + 5 + i * 2, 3, "%02x", luma & 0xFF);
        }
    }
    Log("MOCKXR stamp f=%llu v0=%u v1=%u img0=%d img1=%d c0=%u c1=%u s0=%u s1=%u x0=%u%s%s",
        (unsigned long long)slot.frame, DecodeStamp(left), DecodeStamp(right), slot.image[0],
        slot.image[1], DecodeStamp(left, 1), DecodeStamp(right, 1), DecodeStamp(left, 2),
        DecodeStamp(right, 2), DecodeStamp(left, 3), reg, sig);
}

// MOCKXR_ASSOC: where world-fixed direction `i` (of the 8x8 grid over the central 40x30 degrees of
// the eye at its display orientation `display`) lands in the submitted view (pixels in the
// swapchain image); false when outside the submitted image.
bool AssocSample(const XrCompositionLayerProjectionView& view, const XrQuaternionf& display,
                 uint32_t i, int32_t& x, int32_t& y) {
    constexpr float D = 3.14159265f / 180.0f;
    const float ax = (-20.0f + 40.0f * ((i % 8) + 0.5f) / 8.0f) * D;
    const float ay = (-15.0f + 30.0f * ((i / 8) + 0.5f) / 8.0f) * D;
    const XrVector3f world = Rotate(display, {std::tan(ax), std::tan(ay), -1.0f});
    const XrQuaternionf& q = view.pose.orientation;
    const XrVector3f v = Rotate({-q.x, -q.y, -q.z, q.w}, world);
    if (v.z > -0.01f) {
        return false;
    }
    const float tx = v.x / -v.z, ty = v.y / -v.z;
    const float l = std::tan(view.fov.angleLeft), r = std::tan(view.fov.angleRight);
    const float u = std::tan(view.fov.angleUp), dn = std::tan(view.fov.angleDown);
    const float fx = (tx - l) / (r - l), fy = (u - ty) / (u - dn);
    if (!(fx >= 0.0f && fx < 1.0f && fy >= 0.0f && fy < 1.0f)) {
        return false;
    }
    const XrRect2Di& rect = view.subImage.imageRect;
    x = rect.offset.x + static_cast<int32_t>(fx * static_cast<float>(rect.extent.width));
    y = rect.offset.y + static_cast<int32_t>(fy * static_cast<float>(rect.extent.height));
    return true;
}

// Called in xrEndFrame (the application holds its queue lock there) for a projection layer.
void QueueStampRead(Session& s, const XrCompositionLayerProjection& p, int64_t display_time) {
    auto& r = Stamps();
    VkDevice dev = s.vk_device;
    if (!r.ready) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = s.queue_family;
        vkCreateCommandPool(dev, &pi, nullptr, &r.pool);
        for (auto& slot : r.slots) {
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = StampBufferBytes;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            vkCreateBuffer(dev, &bi, nullptr, &slot.buffer);
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(dev, slot.buffer, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            ai.memoryTypeIndex = FindMemoryType(s.vk_physical, req.memoryTypeBits,
                                                VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            vkAllocateMemory(dev, &ai, nullptr, &slot.memory);
            vkBindBufferMemory(dev, slot.buffer, slot.memory, 0);
            vkMapMemory(dev, slot.memory, 0, StampBufferBytes, 0,
                        reinterpret_cast<void**>(&slot.mapped));
            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cai.commandPool = r.pool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            vkAllocateCommandBuffers(dev, &cai, &slot.cmd);
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            vkCreateFence(dev, &fi, nullptr, &slot.fence);
        }
        r.ready = true;
    }
    // Decode finished reads in order (oldest first), then take the next slot.
    for (uint32_t i = 0; i < r.slots.size(); ++i) {
        auto& slot = r.slots[(r.next + i) % r.slots.size()];
        if (slot.busy && vkGetFenceStatus(dev, slot.fence) != VK_SUCCESS) {
            break; // keep the log in frame order
        }
        FinishStamp(s, slot, false);
    }
    auto& slot = r.slots[r.next];
    r.next = (r.next + 1) % r.slots.size();
    FinishStamp(s, slot, true);
    vkResetFences(dev, 1, &slot.fence);
    vkResetCommandBuffer(slot.cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(slot.cmd, &begin);
    for (uint32_t v = 0; v < 2 && v < p.viewCount; ++v) {
        Swapchain sc;
        {
            std::lock_guard lk{g_mutex};
            sc = g_swapchains[p.views[v].subImage.swapchain];
        }
        slot.image[v] = sc.last_released;
        const XrRect2Di rect = ClampRect(sc, p.views[v].subImage.imageRect);
        if (v == 0) {
            std::memset(slot.assoc, 0, sizeof(slot.assoc));
        }
        if (sc.last_released < 0 || rect.extent.width < 64 || rect.extent.height < 16) {
            std::memset(slot.mapped + v * StampViewBytes, 0, StampViewBytes);
            continue;
        }
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = sc.images[sc.last_released];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(slot.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        std::vector<VkBufferImageCopy> regions;
        VkBufferImageCopy region{};
        region.bufferOffset = v * StampViewBytes;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {rect.offset.x, rect.offset.y + rect.extent.height - 16, 0};
        region.imageExtent = {64, 16, 1};
        regions.push_back(region);
        if (v == 0 && SigEnabled()) {
            for (uint32_t i = 0; i < SigSamples; ++i) {
                VkBufferImageCopy sample{};
                sample.bufferOffset = SigOffset + i * 4;
                sample.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                sample.imageOffset = {
                    rect.offset.x + static_cast<int32_t>(rect.extent.width *
                                                         (0.2f + 0.6f * ((i % 8) + 0.5f) / 8.0f)),
                    rect.offset.y + static_cast<int32_t>(rect.extent.height *
                                                         (0.2f + 0.6f * ((i / 8) + 0.5f) / 8.0f)),
                    0};
                sample.imageExtent = {1, 1, 1};
                regions.push_back(sample);
            }
        }
        if (v == 0 && AssocEnabled()) {
            XrPosef head{};
            HeadPose(display_time, head);
            for (uint32_t i = 0; i < AssocSamples; ++i) {
                int32_t x = 0, y = 0;
                if (!AssocSample(p.views[0], head.orientation, i, x, y) || x < 0 || y < 0 ||
                    x >= static_cast<int32_t>(sc.width) || y >= static_cast<int32_t>(sc.height)) {
                    continue;
                }
                slot.assoc[i] = true;
                VkBufferImageCopy sample{};
                sample.bufferOffset = AssocOffset + i * 4;
                sample.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                sample.imageOffset = {x, y, 0};
                sample.imageExtent = {1, 1, 1};
                regions.push_back(sample);
            }
        }
        vkCmdCopyImageToBuffer(slot.cmd, b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                               slot.buffer, static_cast<uint32_t>(regions.size()), regions.data());
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(slot.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    vkEndCommandBuffer(slot.cmd);
    VkQueue queue;
    vkGetDeviceQueue(dev, s.queue_family, s.queue_index, &queue);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &slot.cmd;
    vkQueueSubmit(queue, 1, &si, slot.fence);
    slot.busy = true;
    slot.frame = s.projection_frames;
}

// ---- MOCKXR_TRACK ------------------------------------------------------------------
constexpr uint32_t TrackPatch = 160;
constexpr uint32_t TrackPatches = 3;
constexpr float TrackPxPerTan = 532.0f;
// Each patch's top-left corner in the left eye's tangent space (x right, y up).
constexpr std::array<std::array<float, 2>, TrackPatches> TrackOrigin{
    {{-0.15f, 0.40f}, {-0.15f, -0.25f}, {0.30f, -0.70f}}};
constexpr uint32_t TrackRegion = 256; // largest copied region side (submitted image pixels)
constexpr uint32_t TrackRegionBytes = TrackRegion * TrackRegion * 4;

struct TrackSlot {
    VkBuffer buffer{};
    VkDeviceMemory memory{};
    uint8_t* mapped{};
    VkCommandBuffer cmd{};
    VkFence fence{};
    bool busy{false};
    uint64_t frame{};
    int64_t time{};
    XrQuaternionf display{0, 0, 0, 1};
    XrCompositionLayerProjectionView view{};
    std::array<XrRect2Di, TrackPatches> region{};
    std::array<bool, TrackPatches> valid{};
};
struct TrackReader {
    bool ready{false};
    VkCommandPool pool{};
    std::array<TrackSlot, 8> slots{};
    uint32_t next{0};
    std::FILE* file{nullptr};
    uint64_t written{0};
};

bool TrackEnabled() {
    static const bool enabled = std::getenv("MOCKXR_TRACK") != nullptr;
    return enabled;
}

TrackReader& Tracks() {
    static TrackReader reader;
    return reader;
}

// Left-eye tangent (tx, ty) at display orientation `display` -> the submitted view's image
// (continuous pixels), like a rotation-only compositor. False: behind it.
bool TrackMap(const XrQuaternionf& display, const XrCompositionLayerProjectionView& view, float tx,
              float ty, float& x, float& y) {
    const XrVector3f world = Rotate(display, {tx, ty, -1.0f});
    const XrQuaternionf& q = view.pose.orientation;
    const XrVector3f v = Rotate({-q.x, -q.y, -q.z, q.w}, world);
    if (v.z > -0.01f) {
        return false;
    }
    const float sx = v.x / -v.z, sy = v.y / -v.z;
    const float l = std::tan(view.fov.angleLeft), r = std::tan(view.fov.angleRight);
    const float u = std::tan(view.fov.angleUp), dn = std::tan(view.fov.angleDown);
    const XrRect2Di& rect = view.subImage.imageRect;
    x = static_cast<float>(rect.offset.x) + (sx - l) / (r - l) * static_cast<float>(rect.extent.width);
    y = static_cast<float>(rect.offset.y) + (u - sy) / (u - dn) * static_cast<float>(rect.extent.height);
    return true;
}

void FinishTrack(Session& s, TrackSlot& slot, bool wait) {
    if (!slot.busy) {
        return;
    }
    if (wait) {
        vkWaitForFences(s.vk_device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
    } else if (vkGetFenceStatus(s.vk_device, slot.fence) != VK_SUCCESS) {
        return;
    }
    slot.busy = false;
    auto& r = Tracks();
    if (r.file == nullptr) {
        const char* dir = BenchmarkFiles::DumpDirectory();
        const std::string path = std::string(dir ? dir : ".") + "\\track.bin";
        r.file = BenchmarkFiles::Open(path.c_str(), "wb");
        Log("MOCKXR track: %u patches of %ux%u per projection frame into %s", TrackPatches,
            TrackPatch, TrackPatch, path.c_str());
        if (r.file == nullptr) {
            return;
        }
    }
    std::vector<uint8_t> out(TrackPatches * TrackPatch * TrackPatch, 0);
    for (uint32_t k = 0; k < TrackPatches; ++k) {
        if (!slot.valid[k]) {
            continue;
        }
        const XrRect2Di& region = slot.region[k];
        // One sequential copy out of the (possibly uncached) mapped memory, then random reads.
        static std::vector<uint8_t> cached;
        const size_t bytes = static_cast<size_t>(region.extent.width) * region.extent.height * 4;
        cached.resize(bytes);
        std::memcpy(cached.data(), slot.mapped + k * TrackRegionBytes, bytes);
        const uint8_t* base = cached.data();
        const auto luma = [&](int x, int y) {
            x = std::clamp(x, 0, region.extent.width - 1);
            y = std::clamp(y, 0, region.extent.height - 1);
            const uint8_t* px = base + (static_cast<size_t>(y) * region.extent.width + x) * 4;
            return 0.25f * (px[0] + 2.0f * px[1] + px[2]); // B,G,R or R,G,B alike
        };
        for (uint32_t j = 0; j < TrackPatch; ++j) {
            for (uint32_t i = 0; i < TrackPatch; ++i) {
                const float tx = TrackOrigin[k][0] + (static_cast<float>(i) + 0.5f) / TrackPxPerTan;
                const float ty = TrackOrigin[k][1] - (static_cast<float>(j) + 0.5f) / TrackPxPerTan;
                float x{}, y{};
                if (!TrackMap(slot.display, slot.view, tx, ty, x, y)) {
                    continue;
                }
                const float lx = x - static_cast<float>(region.offset.x) - 0.5f;
                const float ly = y - static_cast<float>(region.offset.y) - 0.5f;
                const int x0 = static_cast<int>(std::floor(lx));
                const int y0 = static_cast<int>(std::floor(ly));
                const float fx = lx - static_cast<float>(x0);
                const float fy = ly - static_cast<float>(y0);
                const float top = luma(x0, y0) + (luma(x0 + 1, y0) - luma(x0, y0)) * fx;
                const float bottom = luma(x0, y0 + 1) + (luma(x0 + 1, y0 + 1) - luma(x0, y0 + 1)) * fx;
                out[(k * TrackPatch + j) * TrackPatch + i] =
                    static_cast<uint8_t>(std::clamp(top + (bottom - top) * fy + 0.5f, 0.0f, 255.0f));
            }
        }
    }
    std::fwrite(&slot.frame, sizeof(slot.frame), 1, r.file);
    std::fwrite(&slot.time, sizeof(slot.time), 1, r.file);
    std::fwrite(out.data(), 1, out.size(), r.file);
    if (++r.written % 120 == 0) {
        std::fflush(r.file);
    }
}

// Called in xrEndFrame (the application holds its queue lock there) for the game's projection layer.
void QueueTrackRead(Session& s, const XrCompositionLayerProjection& p, int64_t display_time) {
    auto& r = Tracks();
    VkDevice dev = s.vk_device;
    if (!r.ready) {
        VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pi.queueFamilyIndex = s.queue_family;
        vkCreateCommandPool(dev, &pi, nullptr, &r.pool);
        for (auto& slot : r.slots) {
            VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            bi.size = TrackPatches * TrackRegionBytes;
            bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
            vkCreateBuffer(dev, &bi, nullptr, &slot.buffer);
            VkMemoryRequirements req;
            vkGetBufferMemoryRequirements(dev, slot.buffer, &req);
            VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            ai.allocationSize = req.size;
            constexpr VkMemoryPropertyFlags cached = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT |
                                                     VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            VkPhysicalDeviceMemoryProperties props;
            vkGetPhysicalDeviceMemoryProperties(s.vk_physical, &props);
            ai.memoryTypeIndex = FindMemoryType(s.vk_physical, req.memoryTypeBits, cached);
            if ((props.memoryTypes[ai.memoryTypeIndex].propertyFlags & cached) != cached ||
                (req.memoryTypeBits & (1u << ai.memoryTypeIndex)) == 0) {
                ai.memoryTypeIndex = FindMemoryType(s.vk_physical, req.memoryTypeBits,
                                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                        VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
            }
            vkAllocateMemory(dev, &ai, nullptr, &slot.memory);
            vkBindBufferMemory(dev, slot.buffer, slot.memory, 0);
            vkMapMemory(dev, slot.memory, 0, bi.size, 0, reinterpret_cast<void**>(&slot.mapped));
            VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            cai.commandPool = r.pool;
            cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cai.commandBufferCount = 1;
            vkAllocateCommandBuffers(dev, &cai, &slot.cmd);
            VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
            vkCreateFence(dev, &fi, nullptr, &slot.fence);
        }
        r.ready = true;
    }
    for (uint32_t i = 0; i < r.slots.size(); ++i) {
        auto& slot = r.slots[(r.next + i) % r.slots.size()];
        if (slot.busy && vkGetFenceStatus(dev, slot.fence) != VK_SUCCESS) {
            break; // in frame order
        }
        FinishTrack(s, slot, false);
    }
    auto& slot = r.slots[r.next];
    r.next = (r.next + 1) % r.slots.size();
    FinishTrack(s, slot, true);
    Swapchain sc;
    {
        std::lock_guard lk{g_mutex};
        sc = g_swapchains[p.views[0].subImage.swapchain];
    }
    if (sc.last_released < 0) {
        return;
    }
    XrPosef head{};
    HeadPose(display_time, head);
    slot.display = head.orientation; // the eyes are parallel to the head (LocateViews)
    slot.view = p.views[0];
    slot.frame = s.projection_frames;
    slot.time = display_time;
    std::vector<VkBufferImageCopy> regions;
    for (uint32_t k = 0; k < TrackPatches; ++k) {
        slot.valid[k] = false;
        float lo_x = 1e9f, lo_y = 1e9f, hi_x = -1e9f, hi_y = -1e9f;
        bool ok = true;
        for (uint32_t c = 0; c < 4 && ok; ++c) {
            const float tx = TrackOrigin[k][0] + ((c & 1) ? TrackPatch / TrackPxPerTan : 0.0f);
            const float ty = TrackOrigin[k][1] - ((c & 2) ? TrackPatch / TrackPxPerTan : 0.0f);
            float x{}, y{};
            ok = TrackMap(slot.display, slot.view, tx, ty, x, y);
            lo_x = std::min(lo_x, x);
            lo_y = std::min(lo_y, y);
            hi_x = std::max(hi_x, x);
            hi_y = std::max(hi_y, y);
        }
        const int x0 = std::max(static_cast<int>(std::floor(lo_x)) - 2, 0);
        const int y0 = std::max(static_cast<int>(std::floor(lo_y)) - 2, 0);
        const int x1 = std::min(static_cast<int>(std::ceil(hi_x)) + 2, static_cast<int>(sc.width));
        const int y1 = std::min(static_cast<int>(std::ceil(hi_y)) + 2, static_cast<int>(sc.height));
        if (!ok || x1 - x0 < 8 || y1 - y0 < 8 || x1 - x0 > static_cast<int>(TrackRegion) ||
            y1 - y0 > static_cast<int>(TrackRegion)) {
            continue;
        }
        slot.valid[k] = true;
        slot.region[k] = {{x0, y0}, {x1 - x0, y1 - y0}};
        VkBufferImageCopy region{};
        region.bufferOffset = k * TrackRegionBytes;
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageOffset = {x0, y0, 0};
        region.imageExtent = {static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0), 1};
        regions.push_back(region);
    }
    vkResetFences(dev, 1, &slot.fence);
    vkResetCommandBuffer(slot.cmd, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(slot.cmd, &begin);
    if (!regions.empty()) {
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = sc.images[sc.last_released];
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdPipelineBarrier(slot.cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        vkCmdCopyImageToBuffer(slot.cmd, b.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, slot.buffer,
                               static_cast<uint32_t>(regions.size()), regions.data());
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        vkCmdPipelineBarrier(slot.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    vkEndCommandBuffer(slot.cmd);
    VkQueue queue;
    vkGetDeviceQueue(dev, s.queue_family, s.queue_index, &queue);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &slot.cmd;
    vkQueueSubmit(queue, 1, &si, slot.fence);
    slot.busy = true;
}

// The colour scale chained to a layer (1,1,1,1 without one).
XrColor4f LayerColorScale(const void* next) {
    const auto* header = static_cast<const XrBaseInStructure*>(next);
    while (header != nullptr) {
        if (header->type == XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR) {
            return reinterpret_cast<const XrCompositionLayerColorScaleBiasKHR*>(header)
                ->colorScale;
        }
        header = header->next;
    }
    return {1.0f, 1.0f, 1.0f, 1.0f};
}

// The colour bias chained to a layer (0,0,0,0 without one).
XrColor4f LayerColorBias(const void* next) {
    const auto* header = static_cast<const XrBaseInStructure*>(next);
    while (header != nullptr) {
        if (header->type == XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR) {
            return reinterpret_cast<const XrCompositionLayerColorScaleBiasKHR*>(header)->colorBias;
        }
        header = header->next;
    }
    return {0.0f, 0.0f, 0.0f, 0.0f};
}

// A UI layer: a quad or cylinder blended with its (premultiplied) alpha.
bool IsUiLayer(const XrCompositionLayerBaseHeader* base) {
    return (base->type == XR_TYPE_COMPOSITION_LAYER_QUAD ||
            base->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) &&
           (base->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
}

// The VR overlay: a projection layer blended with its alpha (the game's layers are opaque).
bool IsOverlay(const XrCompositionLayerBaseHeader* base) {
    return base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION &&
           (base->layerFlags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) != 0;
}

// One layer of the submitted stack as text (the `layers:` and `layerdump` lines).
std::string LayerText(const XrCompositionLayerBaseHeader* base) {
    char text[512];
    const XrColor4f c = LayerColorScale(base->next);
    if (IsOverlay(base)) {
        std::snprintf(text, sizeof(text), "overlay scale %.3f flags %llu", c.r,
                      (unsigned long long)base->layerFlags);
    } else if (base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
        std::snprintf(text, sizeof(text), "projection scale %.3f flags %llu", c.r,
                      (unsigned long long)base->layerFlags);
    } else if (base->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
        const auto* q = reinterpret_cast<const XrCompositionLayerQuad*>(base);
        const auto& r = q->subImage.imageRect;
        std::snprintf(text, sizeof(text),
                      "quad q(%.5f %.5f %.5f %.5f) p(%.4f %.4f %.4f) size %.4fx%.4f rect %d,%d "
                      "%dx%d scale %.3f flags %llu",
                      q->pose.orientation.x, q->pose.orientation.y, q->pose.orientation.z,
                      q->pose.orientation.w, q->pose.position.x, q->pose.position.y,
                      q->pose.position.z, q->size.width, q->size.height, r.offset.x, r.offset.y,
                      r.extent.width, r.extent.height, c.r, (unsigned long long)base->layerFlags);
    } else if (base->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
        const auto* y = reinterpret_cast<const XrCompositionLayerCylinderKHR*>(base);
        const auto& r = y->subImage.imageRect;
        std::snprintf(text, sizeof(text),
                      "cylinder q(%.5f %.5f %.5f %.5f) p(%.4f %.4f %.4f) radius %.4f angle %.4f "
                      "aspect %.4f rect %d,%d %dx%d scale %.3f flags %llu",
                      y->pose.orientation.x, y->pose.orientation.y, y->pose.orientation.z,
                      y->pose.orientation.w, y->pose.position.x, y->pose.position.y,
                      y->pose.position.z, y->radius, y->centralAngle, y->aspectRatio, r.offset.x,
                      r.offset.y, r.extent.width, r.extent.height, c.r,
                      (unsigned long long)base->layerFlags);
    } else {
        std::snprintf(text, sizeof(text), "type %d", (int)base->type);
    }
    if (IsUiLayer(base)) {
        const XrColor4f b = LayerColorBias(base->next);
        const int eyes =
            base->type == XR_TYPE_COMPOSITION_LAYER_QUAD
                ? (int)reinterpret_cast<const XrCompositionLayerQuad*>(base)->eyeVisibility
                : (int)reinterpret_cast<const XrCompositionLayerCylinderKHR*>(base)->eyeVisibility;
        char extra[160];
        std::snprintf(extra, sizeof(extra), " rgba %.3f %.3f %.3f %.3f bias %.3f %.3f %.3f %.3f eyes %d",
                      c.r, c.g, c.b, c.a, b.r, b.g, b.b, b.a, eyes);
        return std::string(text) + extra;
    }
    return text;
}

// <dump dir>\dump_now: its first line names the dump; the file is deleted (frame thread).
bool TakeAutoDump(std::string& name); // MOCKXR_THROW_DUMPS (below)
bool TakeDumpRequest(const char* dump_dir, std::string& name) {
    static int64_t last_check = 0;
    const int64_t now = NowNs();
    if (TakeAutoDump(name)) {
        return dump_dir != nullptr;
    }
    if (dump_dir == nullptr || now - last_check < 50'000'000) {
        return false;
    }
    last_check = now;
    const std::string path = std::string(dump_dir) + "\\dump_now";
    FILE* f = BenchmarkFiles::Open(path.c_str(), "r");
    if (f == nullptr) {
        return false;
    }
    char line[128]{};
    if (std::fgets(line, sizeof(line), f) == nullptr) {
        line[0] = '\0';
    }
    std::fclose(f);
    BenchmarkFiles::Remove(path);
    name = line;
    while (!name.empty() && (name.back() == '\n' || name.back() == '\r' || name.back() == ' ')) {
        name.pop_back();
    }
    if (name.empty()) {
        static int n = 0;
        name = "req" + std::to_string(++n);
    }
    return true;
}

const XrCompositionLayerDepthInfoKHR* ViewDepth(const XrCompositionLayerProjectionView& view);
void DumpDepth(const XrCompositionLayerDepthInfoKHR& d, const std::string& path,
               const std::string& label);

// Dumps every layer of this frame (dump_now); eyes that carry their depth also get
// <name>_depth_L/R.png and .f32 (DumpDepth).
void DumpLayers(const XrFrameEndInfo* info, const char* dump_dir, const std::string& name) {
    XrPosef head{};
    HeadPose(info->displayTime, head);
    Log("layerdump %s head q(%.5f %.5f %.5f %.5f) p(%.4f %.4f %.4f) layers %u", name.c_str(),
        head.orientation.x, head.orientation.y, head.orientation.z, head.orientation.w,
        head.position.x, head.position.y, head.position.z, info->layerCount);
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        const auto* base = info->layers[i];
        Log("layerdump %s layer %u %s", name.c_str(), i, LayerText(base).c_str());
        const std::string prefix = std::string(dump_dir) + "\\" + name;
        if (base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(base);
            const bool overlay = IsOverlay(base);
            for (uint32_t v = 0; v < 2 && v < p->viewCount; ++v) {
                Swapchain sc;
                {
                    std::lock_guard lk{g_mutex};
                    sc = g_swapchains[p->views[v].subImage.swapchain];
                }
                Log("layerdump %s layer %u eye %u%s", name.c_str(), i, v,
                    ViewText(p->views[v], v, info->displayTime).c_str());
                if (sc.last_released >= 0) {
                    DumpImage(sc, sc.last_released, p->views[v].subImage.imageRect,
                              prefix + (overlay ? "_ovl" : "") + (v == 0 ? "_L.png" : "_R.png"),
                              {}, overlay);
                }
                if (const auto* d = overlay ? nullptr : ViewDepth(p->views[v])) {
                    DumpDepth(*d, prefix + (v == 0 ? "_depth_L.png" : "_depth_R.png"),
                              "layerdump " + name + " depth eye " + std::to_string(v));
                }
            }
        } else if (base->type == XR_TYPE_COMPOSITION_LAYER_QUAD ||
                   base->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
            const bool quad = base->type == XR_TYPE_COMPOSITION_LAYER_QUAD;
            const XrSwapchainSubImage& sub =
                quad ? reinterpret_cast<const XrCompositionLayerQuad*>(base)->subImage
                     : reinterpret_cast<const XrCompositionLayerCylinderKHR*>(base)->subImage;
            Swapchain sc;
            {
                std::lock_guard lk{g_mutex};
                sc = g_swapchains[sub.swapchain];
            }
            if (sc.last_released >= 0) {
                if (IsUiLayer(base)) {
                    DumpImage(sc, sc.last_released, sub.imageRect,
                              prefix + "_l" + std::to_string(i) +
                                  (quad ? "_quad.png" : "_cyl.png"),
                              {}, true);
                } else {
                    DumpImage(sc, sc.last_released, sub.imageRect,
                              prefix + (quad ? "_quad.png" : "_cyl.png"));
                }
            }
        }
    }
}

// ---- scripted controller input (input.txt) ------------------------------------------
struct InputEvent {
    std::string action;
    double start{};
    double end{};
    float x{1.0f};
    float y{0.0f};
    float z{0.0f};
    bool applied{false}; // drift / recenter / lhand: done
};
std::mutex g_input_mutex;
std::vector<InputEvent> g_input;
// input.txt `lhand`: the left grip's live offset, eased from `from` to `to` over [t0, t1].
struct LiveLeft {
    double t0{0.0};
    double t1{0.0};
    float from[3]{};
    float to[3]{};
};
LiveLeft g_live_left; // under g_input_mutex

void LiveLeftAt(double t, float out[3]) {
    const LiveLeft& l = g_live_left;
    const float k = t >= l.t1 ? 1.0f
                    : t <= l.t0
                        ? 0.0f
                        : static_cast<float>(BP::MinJerk((t - l.t0) / std::max(l.t1 - l.t0, 1e-6)));
    for (int i = 0; i < 3; ++i) {
        out[i] = l.from[i] + (l.to[i] - l.from[i]) * k;
    }
}

void LiveLeftOffset(int64_t time, float out[3]) {
    std::lock_guard lk{g_input_mutex};
    LiveLeftAt(static_cast<double>(time) * 1e-9, out);
}

// input.txt `lthrow_over` / `lthrow_push` (see the top): the plan of one scripted throw.
struct ThrowPlan {
    bool over{false};
    float speed{0.0f};
    double t0{0.0};           // the event's start (the grip squeezed)
    double windup0{0.0};      // the wind-up starts
    double top{0.0};          // ... reaches its top
    double stroke{0.0};       // the stroke starts
    double peak{0.0};         // ... reaches its speed (kept for Tp until the release)
    double release{0.0};      // the grip let go
    double follow_end{0.0};   // the follow-through ends
    double back_end{0.0};     // back at the scripted pose
    float T{0.0f}, Tf{0.0f};  // stroke and follow-through durations
    float Tp{0.03f};          // the speed's plateau before the release (a real hand's peak)
    std::array<float, 3> d{}, right{}, S{}, C{}, R{}, F{};
    std::array<float, 3> V{}; // the wind-up's way round the shoulder (overhand)
    float drop{0.0f};         // the follow-through's drop (overhand)
    float follow{0.0f};       // ... and travel along the throw
    float theta_top{0.0f}, theta_rel{0.0f}, theta_ft{0.0f}; // degrees about `right` (+ = cocked)
    float theta_peak{0.0f};
};

ThrowPlan MakeThrowPlan(const std::string& action, double start, float speed, float yaw_deg,
                        float pitch_deg) {
    constexpr float D = 3.14159265f / 180.0f;
    ThrowPlan t{};
    t.over = action == "lthrow_over";
    t.speed = std::max(speed, 0.3f);
    const float cy = std::cos(yaw_deg * D), sy = std::sin(yaw_deg * D);
    const float cp = std::cos(pitch_deg * D), sp = std::sin(pitch_deg * D);
    const std::array<float, 3> f{-sy, 0.0f, -cy}; // the throw's forward (+yaw = left)
    t.right = {cy, 0.0f, -sy};
    t.d = {f[0] * cp, sp, f[2] * cp};
    // The player's left shoulder with the head at (0, 1.6, 0) (the emulator estimates it 24 cm
    // below the eyes, 17 cm to the left, 7 cm back). Everything stays within a real arm's reach
    // (about 0.6 m from the shoulder to the grip).
    const std::array<float, 3> sh{-0.17f, 1.38f, 0.05f};
    const auto at = [&](float fwd, float up, float side) {
        return std::array<float, 3>{sh[0] + f[0] * fwd + t.right[0] * side, sh[1] + up,
                                    sh[2] + f[2] * fwd + t.right[2] * side};
    };
    // The hand keeps its peak speed over the last few cm before the release (a real hand's
    // peak, and at least one input frame sees it).
    const float plateau = std::clamp(0.03f * t.speed, 0.04f, 0.12f);
    t.Tp = plateau / t.speed;
    float follow = 0.08f; // the follow-through's own travel
    if (t.over) {
        // The elbow bent and the hand back beside the head (reached round the outside of the
        // shoulder), then a curved stroke forward over it, released in front of the face with
        // the arm not yet straight.
        constexpr float L = 0.26f;
        t.V = at(0.20f, 0.25f, 0.16f);
        t.S = at(-0.10f, 0.20f, 0.06f);
        t.R = at(0.48f, 0.14f - 0.10f * sp, 0.0f);
        std::array<float, 3> r0{};
        for (int i = 0; i < 3; ++i) {
            r0[i] = t.R[i] - t.d[i] * plateau;
            t.C[i] = r0[i] - t.d[i] * L;
        }
        t.T = 4.0f * L / t.speed; // the speed u' |B'(1)| = 2 L (2 / T)
        t.theta_top = 70.0f;
        t.theta_rel = -10.0f;
        t.theta_ft = -45.0f;
        follow = 0.12f;
    } else {
        // From in front of the chest (elbow bent ~90 degrees) straight along the throw.
        constexpr float L = 0.32f;
        t.S = at(0.28f, -0.06f, 0.0f);
        for (int i = 0; i < 3; ++i) {
            t.R[i] = t.S[i] + t.d[i] * L;
            const float r0 = t.R[i] - t.d[i] * plateau;
            t.C[i] = 0.5f * (t.S[i] + r0);
            t.V[i] = 0.5f * (t.S[i] + t.R[i]); // unused: a straight wind-up
        }
        t.T = 2.0f * (L - plateau) / t.speed;
    }
    // The follow-through: the hand slows from the release speed to rest over Tf (never faster
    // than at the release), `follow` m on, the overhand arm swinging down `drop` m as it goes.
    t.Tf = 0.2f;
    t.follow = follow;
    t.drop = t.over ? 0.2f : 0.0f;
    for (int i = 0; i < 3; ++i) {
        t.F[i] = t.R[i] + t.d[i] * follow - (i == 1 ? t.drop : 0.0f);
    }
    static const bool dumps = std::getenv("MOCKXR_THROW_DUMPS") != nullptr;
    t.t0 = start;
    t.windup0 = start + 0.35;
    t.top = t.windup0 + (t.over ? 0.5 : 0.35);
    t.stroke = t.top + (dumps ? 0.5 : 0.15); // a dump's stall ends before the stroke
    t.peak = t.stroke + t.T;
    t.release = t.peak + t.Tp;
    // The controller's turn: theta_peak at the stroke's end, then on at the stroke's final rate
    // through the plateau to theta_rel at the release.
    const float k = 2.0f * t.Tp / t.T;
    t.theta_peak = (t.theta_rel + k * t.theta_top) / (1.0f + k);
    t.follow_end = t.release + t.Tf;
    t.back_end = t.follow_end + 0.7;
    return t;
}

std::array<float, 4> AxisAngleQ(const std::array<float, 3>& axis, float deg) {
    const float h = 0.5f * deg * 3.14159265f / 180.0f;
    return {axis[0] * std::sin(h), axis[1] * std::sin(h), axis[2] * std::sin(h), std::cos(h)};
}

std::array<float, 4> QMul4(const std::array<float, 4>& a, const std::array<float, 4>& b) {
    return {a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
            a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
            a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
            a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2]};
}

// The left grip of throw `t` at `time` (s) over the scripted pose `pose`.
void ThrowPoseAt(const ThrowPlan& t, double time, BP::HandPose& pose) {
    if (time < t.windup0 || time >= t.back_end) {
        return;
    }
    std::array<float, 3> p{};
    float theta = 0.0f;
    const auto lerp3 = [](const std::array<float, 3>& a, const std::array<float, 3>& b, float k) {
        return std::array<float, 3>{a[0] + (b[0] - a[0]) * k, a[1] + (b[1] - a[1]) * k,
                                    a[2] + (b[2] - a[2]) * k};
    };
    const std::array<float, 3> base{pose.p[0], pose.p[1], pose.p[2]};
    if (time < t.top) {
        const float k = static_cast<float>(BP::MinJerk((time - t.windup0) / (t.top - t.windup0)));
        if (t.over) {
            for (int i = 0; i < 3; ++i) {
                p[i] = (1 - k) * (1 - k) * base[i] + 2 * k * (1 - k) * t.V[i] + k * k * t.S[i];
            }
        } else {
            p = lerp3(base, t.S, k);
        }
        theta = t.theta_top * k;
    } else if (time < t.stroke) {
        p = t.S;
        theta = t.theta_top;
    } else if (time < t.peak) {
        // The stroke: speeding up all the way to its peak (u = (tau / T)^2), ...
        const float tau = static_cast<float>((time - t.stroke) / t.T);
        const float u = tau * tau;
        for (int i = 0; i < 3; ++i) {
            const float r0 = t.R[i] - t.d[i] * t.speed * t.Tp;
            p[i] = (1 - u) * (1 - u) * t.S[i] + 2 * u * (1 - u) * t.C[i] + u * u * r0;
        }
        if (!t.over) {
            for (int i = 0; i < 3; ++i) { // straight: the speed rises linearly (u = tau^2)
                const float r0 = t.R[i] - t.d[i] * t.speed * t.Tp;
                p[i] = t.S[i] + (r0 - t.S[i]) * u;
            }
        }
        theta = t.theta_top + (t.theta_peak - t.theta_top) * u;
    } else if (time < t.release) {
        // ... then at that speed to the release (the controller still turning as fast).
        const float tau = static_cast<float>(time - t.release); // <= 0
        const float slope = (t.theta_peak - t.theta_top) * 2.0f / t.T; // deg/s
        for (int i = 0; i < 3; ++i) {
            p[i] = t.R[i] + t.d[i] * t.speed * tau;
        }
        theta = t.theta_rel + slope * tau;
    } else if (time < t.follow_end) {
        const float tau = static_cast<float>(time - t.release);
        const float q = tau / t.Tf;
        const float e = std::max(t.speed * t.Tf / t.follow, 1.0f); // s'(0) = the release speed
        const float s = t.follow * (1.0f - std::pow(1.0f - q, e));
        for (int i = 0; i < 3; ++i) {
            p[i] = t.R[i] + t.d[i] * s - (i == 1 ? t.drop * q * q : 0.0f);
        }
        const float k = 1.0f - (1.0f - tau / t.Tf) * (1.0f - tau / t.Tf);
        theta = t.theta_rel + (t.theta_ft - t.theta_rel) * k;
    } else {
        const float k =
            static_cast<float>(BP::MinJerk((time - t.follow_end) / (t.back_end - t.follow_end)));
        p = lerp3(t.F, base, k);
        theta = t.theta_ft * (1.0f - k);
    }
    pose.p = p;
    if (theta != 0.0f) {
        const auto q = QMul4(AxisAngleQ(t.right, theta), pose.q);
        const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        pose.q = {q[0] / n, q[1] / n, q[2] / n, q[3] / n};
    }
}

std::vector<ThrowPlan> g_throws; // under g_input_mutex
int g_throw_count = 0;
struct AutoDump {
    double at;
    std::string name;
};
std::vector<AutoDump> g_auto_dumps; // under g_input_mutex (MOCKXR_THROW_DUMPS)

// MOCKXR_THROW_DUMPS: a scripted throw's phase whose time has come (frame thread).
bool TakeAutoDump(std::string& name) {
    std::lock_guard lk{g_input_mutex};
    const double t = static_cast<double>(NowNs()) * 1e-9;
    for (auto it = g_auto_dumps.begin(); it != g_auto_dumps.end(); ++it) {
        if (t >= it->at) {
            name = it->name;
            Log("MOCKXR throw dump %s (%.3f s late)", name.c_str(), t - it->at);
            g_auto_dumps.erase(it);
            return true;
        }
    }
    return false;
}

void LiveThrowPose(int64_t time, BP::HandPose& pose) {
    std::lock_guard lk{g_input_mutex};
    const double t = static_cast<double>(time) * 1e-9;
    for (const ThrowPlan& plan : g_throws) {
        if (t >= plan.windup0 && t < plan.back_end) {
            ThrowPoseAt(plan, t, pose);
            return;
        }
    }
}

// input.txt `laim` (see the top): the left grip aimed so the gun's barrel points somewhere.
struct LiveAim {
    bool active{false};  // an aim is held (or being eased to)
    bool release{false}; // easing back to the scripted pose
    bool from_set{false};
    double t0{0.0}, t1{0.0};
    std::array<float, 3> from_p{}, to_p{};
    std::array<float, 4> from_q{0, 0, 0, 1}, to_q{0, 0, 0, 1};
};
LiveAim g_live_aim; // under g_input_mutex

// The barrel in OpenXR grip axes at the default grip (core/vr/gun_aim.h BarrelInGrip, trims 0).
std::array<float, 3> BarrelInGrip(int attach) {
    switch (attach) {
    case 345:
        return {-0.0073611f, -0.4168873f, -0.9089284f};
    case 348:
        return {-0.1013157f, -0.8739530f, -0.4753328f};
    default:
        return {-0.0397999f, -0.6906492f, -0.7220939f};
    }
}

std::array<float, 3> Norm3(std::array<float, 3> v) {
    const float n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    return n > 1e-9f ? std::array<float, 3>{v[0] / n, v[1] / n, v[2] / n} : v;
}
std::array<float, 3> Cross3(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
float Dot3(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
// The rotation taking direction b to d whose image of up_g is nearest up_w (both frames' "up"
// made perpendicular to their direction): R = [d u_w s_w] [b u_g s_g]^T, as a quaternion.
std::array<float, 4> AimRotation(const std::array<float, 3>& b, const std::array<float, 3>& d) {
    const auto frame = [](const std::array<float, 3>& f) {
        const std::array<float, 3> up{0.0f, 1.0f, 0.0f};
        const float k = Dot3(up, f);
        std::array<float, 3> u = Norm3({up[0] - f[0] * k, up[1] - f[1] * k, up[2] - f[2] * k});
        if (std::abs(k) > 0.999f) {
            u = Norm3(Cross3(f, {1.0f, 0.0f, 0.0f}));
        }
        return std::array<std::array<float, 3>, 3>{f, u, Cross3(f, u)};
    };
    const auto G = frame(Norm3(b));
    const auto W = frame(Norm3(d));
    float m[3][3]{};
    for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 3; ++c) {
            m[r][c] = W[0][r] * G[0][c] + W[1][r] * G[1][c] + W[2][r] * G[2][c];
        }
    }
    std::array<float, 4> q{};
    const float trace = m[0][0] + m[1][1] + m[2][2];
    if (trace > 0.0f) {
        const float s = 0.5f / std::sqrt(trace + 1.0f);
        q = {(m[2][1] - m[1][2]) * s, (m[0][2] - m[2][0]) * s, (m[1][0] - m[0][1]) * s, 0.25f / s};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        const float s = 2.0f * std::sqrt(std::max(1.0f + m[0][0] - m[1][1] - m[2][2], 1e-12f));
        q = {0.25f * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s, (m[2][1] - m[1][2]) / s};
    } else if (m[1][1] > m[2][2]) {
        const float s = 2.0f * std::sqrt(std::max(1.0f + m[1][1] - m[0][0] - m[2][2], 1e-12f));
        q = {(m[0][1] + m[1][0]) / s, 0.25f * s, (m[1][2] + m[2][1]) / s, (m[0][2] - m[2][0]) / s};
    } else {
        const float s = 2.0f * std::sqrt(std::max(1.0f + m[2][2] - m[0][0] - m[1][1], 1e-12f));
        q = {(m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25f * s, (m[1][0] - m[0][1]) / s};
    }
    const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    return {q[0] / n, q[1] / n, q[2] / n, q[3] / n};
}

std::array<float, 4> Nlerp4(const std::array<float, 4>& a, std::array<float, 4> b, float k) {
    const float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0.0f) {
        b = {-b[0], -b[1], -b[2], -b[3]};
    }
    std::array<float, 4> q{};
    float n = 0.0f;
    for (int i = 0; i < 4; ++i) {
        q[i] = a[i] + (b[i] - a[i]) * k;
        n += q[i] * q[i];
    }
    n = std::sqrt(std::max(n, 1e-12f));
    for (float& c : q) {
        c /= n;
    }
    return q;
}

// The aim's pose at `t` over the scripted `pose` (the frame thread's and the velocities' calls).
void AimPoseAt(LiveAim& a, double t, BP::HandPose& pose) {
    if (!a.active) {
        return;
    }
    if (!a.from_set) {
        a.from_set = true;
        a.from_p = pose.p;
        a.from_q = pose.q;
    }
    const float k = t >= a.t1   ? 1.0f
                    : t <= a.t0 ? 0.0f
                                : static_cast<float>(BP::MinJerk((t - a.t0) / std::max(a.t1 - a.t0, 1e-6)));
    const std::array<float, 3> to_p = a.release ? pose.p : a.to_p;
    const std::array<float, 4> to_q = a.release ? pose.q : a.to_q;
    for (int i = 0; i < 3; ++i) {
        pose.p[i] = a.from_p[i] + (to_p[i] - a.from_p[i]) * k;
    }
    pose.q = Nlerp4(a.from_q, to_q, k);
    if (a.release && k >= 1.0f) {
        a.active = false;
    }
}

void LiveAimPose(int64_t time, BP::HandPose& pose) {
    std::lock_guard lk{g_input_mutex};
    AimPoseAt(g_live_aim, static_cast<double>(time) * 1e-9, pose);
}

// input.txt `rswing_fore` / `rswing_back` / `rswing_chop` / `rswing_stab` / `rwaggle` (see the
// top): one scripted right-hand stroke over the scripted pose. The hand moves on a sphere of
// RightReach around the right shoulder with the blade (grip -Z) pointing out along the radius, so
// the blade turns with the arm like a real swing: eased (slowly, below the swing gate) from the
// scripted pose to the stroke's start, held still, the stroke itself (minimum jerk from the start
// direction to the end direction, timed so the hand peaks at <speed> m/s), held, and eased back.
// fore: from out to the right (75 deg) across the front to the left (45 deg), slightly down;
// back: the mirror (from the left to out right); chop: from wound up over the shoulder (the
// blade 130 deg up and back) down through the front to 40 deg below it (a heavy's arc); stab: the
// hand straight forward 35 cm with the blade level. <yaw> turns the whole stroke (+ = left),
// <pitch> tilts it. rwaggle <seconds> <deg> <hz> <arm m>: the blade waggles +-<deg> about the
// vertical at <hz> from the scripted pose (the wrist), and with <arm m> > 0 the hand also moves
// +-<arm m> sideways at the same rate (an arm waggle).
struct RightStroke {
    int kind{0}; // 0 fore, 1 back, 2 chop, 3 stab, 4 waggle
    double t0{0.0}, ready{0.0}, stroke{0.0}, stroke_end{0.0}, hold_end{0.0}, back_end{0.0};
    float speed{5.0f};
    std::array<float, 4> q0{0, 0, 0, 1}, q1{0, 0, 0, 1}; // the blade's start / end (grip)
    std::array<float, 3> p0{}, p1{};                     // the stab's hand path
    float amp{0.0f}, hz{0.0f}, arm{0.0f};                // waggle
    float T{0.0f};
};
constexpr float RightReach = 0.55f;                                // m, shoulder to grip
constexpr std::array<float, 3> RightShoulder{0.18f, 1.38f, 0.05f}; // with the head at (0, 1.6, 0)
std::vector<RightStroke> g_rstrokes; // under g_input_mutex
int g_rstroke_count = 0;

std::array<float, 3> BladeOf(const std::array<float, 4>& q) {
    const XrVector3f b = Rotate({q[0], q[1], q[2], q[3]}, {0.0f, 0.0f, -1.0f});
    return {b.x, b.y, b.z};
}

std::array<float, 3> HandOnSphere(const std::array<float, 4>& q) {
    const auto b = BladeOf(q);
    return {RightShoulder[0] + b[0] * RightReach, RightShoulder[1] + b[1] * RightReach,
            RightShoulder[2] + b[2] * RightReach};
}

std::array<float, 4> SlerpQ(const std::array<float, 4>& a, std::array<float, 4> b, float k) {
    float dot = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (dot < 0.0f) {
        b = {-b[0], -b[1], -b[2], -b[3]};
        dot = -dot;
    }
    if (dot > 0.9995f) {
        return Nlerp4(a, b, k);
    }
    const float th = std::acos(std::min(dot, 1.0f));
    const float s = std::sin(th);
    const float wa = std::sin((1.0f - k) * th) / s, wb = std::sin(k * th) / s;
    return {a[0] * wa + b[0] * wb, a[1] * wa + b[1] * wb, a[2] * wa + b[2] * wb,
            a[3] * wa + b[3] * wb};
}

RightStroke MakeRightStroke(const std::string& action, double start, float seconds, float x,
                            float y, float z, bool chained = false) {
    RightStroke s{};
    s.kind = action == "rswing_back"   ? 1
             : action == "rswing_chop" ? 2
             : action == "rswing_stab" ? 3
             : action == "rwaggle"     ? 4
                                       : 0;
    // MOCKXR_RSWING_EASE / MOCKXR_RSWING_HOLD: shorter for a fight (default 0.8 / 0.3 s).
    static const double ease = std::getenv("MOCKXR_RSWING_EASE") != nullptr
                                   ? std::max(std::atof(std::getenv("MOCKXR_RSWING_EASE")), 0.05)
                                   : 0.8;
    static const double hold = std::getenv("MOCKXR_RSWING_HOLD") != nullptr
                                   ? std::max(std::atof(std::getenv("MOCKXR_RSWING_HOLD")), 0.0)
                                   : 0.3;
    // A stroke chained on from a running one (a quick combo): from where its hand is.
    static const double chain_ease =
        std::getenv("MOCKXR_RSWING_CHAIN_EASE") != nullptr
            ? std::max(std::atof(std::getenv("MOCKXR_RSWING_CHAIN_EASE")), 0.02)
            : 0.12;
    static const double chain_hold =
        std::getenv("MOCKXR_RSWING_CHAIN_HOLD") != nullptr
            ? std::max(std::atof(std::getenv("MOCKXR_RSWING_CHAIN_HOLD")), 0.0)
            : 0.0;
    s.t0 = start;
    s.ready = start + (chained ? chain_ease : ease); // eased to the start pose (slowly)
    s.stroke = s.ready + (chained ? chain_hold : hold); // held still (the swing gate wants it)
    const auto grip = [&](float yaw, float pitch) {
        const auto g = BP::GripRotation(yaw + y, pitch + z, 0.0f);
        return std::array<float, 4>{g[0], g[1], g[2], g[3]};
    };
    if (s.kind == 4) {
        s.amp = x;
        s.hz = std::max(y, 0.2f);
        s.arm = z;
        s.ready = start;
        s.stroke = start;
        s.stroke_end = start + std::max(seconds, 0.2f);
        s.hold_end = s.stroke_end;
        s.back_end = s.stroke_end;
        return s;
    }
    s.speed = std::max(x, 0.2f);
    float path = 0.0f;
    switch (s.kind) {
    case 0:
        s.q0 = grip(-75.0f, 5.0f);
        s.q1 = grip(45.0f, -10.0f);
        break;
    case 1:
        s.q0 = grip(55.0f, 0.0f);
        s.q1 = grip(-60.0f, -5.0f);
        break;
    case 2:
        s.q0 = grip(0.0f, 130.0f);
        s.q1 = grip(0.0f, -40.0f);
        break;
    default: { // the stab: level, forward
        s.q0 = s.q1 = grip(0.0f, 0.0f);
        const auto b = BladeOf(s.q0);
        for (int i = 0; i < 3; ++i) {
            s.p0[i] = RightShoulder[i] + b[i] * 0.20f;
            s.p1[i] = RightShoulder[i] + b[i] * 0.55f;
        }
        path = 0.35f;
        break;
    }
    }
    if (s.kind != 3) {
        const float dot = std::clamp(s.q0[0] * s.q1[0] + s.q0[1] * s.q1[1] + s.q0[2] * s.q1[2] +
                                         s.q0[3] * s.q1[3],
                                     -1.0f, 1.0f);
        path = 2.0f * std::acos(std::abs(dot)) * RightReach;
    }
    s.T = std::max(1.875f * path / s.speed, 0.05f); // minimum jerk peaks at 1.875 x the mean
    s.stroke_end = s.stroke + s.T;
    s.hold_end = s.stroke_end + 0.3;
    s.back_end = s.hold_end + 0.9;
    return s;
}

// The right grip of stroke `s` at `time` over the scripted pose `pose`.
void RightStrokeAt(const RightStroke& s, double time, BP::HandPose& pose) {
    if (time < s.t0 || time >= s.back_end) {
        return;
    }
    if (s.kind == 4) {
        const float ph = static_cast<float>(2.0 * 3.14159265 * s.hz * (time - s.t0));
        const auto q = QMul4(AxisAngleQ({0.0f, 1.0f, 0.0f}, s.amp * std::sin(ph)), pose.q);
        const float n = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        pose.q = {q[0] / n, q[1] / n, q[2] / n, q[3] / n};
        pose.p[0] += s.arm * std::sin(ph);
        return;
    }
    const auto start_p = s.kind == 3 ? s.p0 : HandOnSphere(s.q0);
    const auto end_p = s.kind == 3 ? s.p1 : HandOnSphere(s.q1);
    std::array<float, 3> p{};
    std::array<float, 4> q{};
    if (time < s.ready) {
        const float k = static_cast<float>(BP::MinJerk((time - s.t0) / (s.ready - s.t0)));
        for (int i = 0; i < 3; ++i) {
            p[i] = pose.p[i] + (start_p[i] - pose.p[i]) * k;
        }
        q = Nlerp4(pose.q, s.q0, k);
    } else if (time < s.stroke) {
        p = start_p;
        q = s.q0;
    } else if (time < s.stroke_end) {
        const float k = static_cast<float>(BP::MinJerk((time - s.stroke) / s.T));
        q = SlerpQ(s.q0, s.q1, k);
        if (s.kind == 3) {
            for (int i = 0; i < 3; ++i) {
                p[i] = s.p0[i] + (s.p1[i] - s.p0[i]) * k;
            }
        } else {
            p = HandOnSphere(q);
        }
    } else if (time < s.hold_end) {
        p = end_p;
        q = s.q1;
    } else {
        const float k =
            static_cast<float>(BP::MinJerk((time - s.hold_end) / (s.back_end - s.hold_end)));
        for (int i = 0; i < 3; ++i) {
            p[i] = end_p[i] + (pose.p[i] - end_p[i]) * k;
        }
        q = Nlerp4(s.q1, pose.q, k);
    }
    pose.p = p;
    pose.q = q;
}

void LiveRightPose(int64_t time, BP::HandPose& pose) {
    std::lock_guard lk{g_input_mutex};
    const double t = static_cast<double>(time) * 1e-9;
    // In start order: a stroke that started while an earlier one runs (a chained combo) takes
    // over from that one's hand.
    for (const RightStroke& s : g_rstrokes) {
        if (t >= s.t0 && t < s.back_end) {
            RightStrokeAt(s, t, pose);
        }
    }
}

// The grip of a scripted throw: squeezed from its start to its release.
bool ThrowGrip(double t) {
    std::lock_guard lk{g_input_mutex};
    for (const ThrowPlan& plan : g_throws) {
        if (t >= plan.t0 && t < plan.release) {
            return true;
        }
    }
    return false;
}

// Reads <dump dir>\input.txt (at most every 100 ms) and queues its events after the current ones.
void PollInputFile() {
    static int64_t last_check = 0;
    const int64_t now = NowNs();
    const char* dir = BenchmarkFiles::DumpDirectory();
    if (dir == nullptr || now - last_check < 100'000'000) {
        return;
    }
    last_check = now;
    const std::string path = std::string(dir) + "\\input.txt";
    FILE* f = BenchmarkFiles::Open(path.c_str(), "r");
    if (f == nullptr) {
        return;
    }
    std::vector<std::string> lines;
    char line[256];
    while (std::fgets(line, sizeof(line), f) != nullptr) {
        lines.emplace_back(line);
    }
    std::fclose(f);
    BenchmarkFiles::Remove(path);
    std::lock_guard lk{g_input_mutex};
    double t = static_cast<double>(now) * 1e-9;
    for (const auto& e : g_input) {
        if (e.action != "lean") { // a sway runs in the background: never holds the queue
            t = std::max(t, e.end + 0.25);
        }
    }
    double previous_start = t;
    for (const auto& text : lines) {
        char action[64]{};
        float seconds = 0.0f, x = 1.0f, y = 0.0f, z = 0.0f;
        const char* body = text.c_str();
        float offset = -1.0f; // `@<offset>`: alongside the previous event
        if (body[0] == '@') {
            int used = 0;
            if (std::sscanf(body + 1, "%f%n", &offset, &used) != 1) {
                continue;
            }
            body += 1 + used;
        }
        const int n = std::sscanf(body, "%63s %f %f %f %f", action, &seconds, &x, &y, &z);
        if (n < 2 || action[0] == '#') {
            continue;
        }
        if (std::string{action} == "wait") {
            t += seconds;
            continue;
        }
        const double start = offset >= 0.0f ? previous_start + offset : t;
        g_input.push_back({action, start, start + seconds, n >= 3 ? x : 1.0f, n >= 4 ? y : 0.0f,
                           n >= 5 ? z : 0.0f});
        if (std::string{action}.rfind("rswing_", 0) == 0 || std::string{action} == "rwaggle") {
            bool chained = false; // starts while another stroke runs: a quick combo
            for (const RightStroke& r : g_rstrokes) {
                chained = chained || (start >= r.t0 && start < r.back_end && r.kind != 4);
            }
            const RightStroke st =
                MakeRightStroke(action, start, seconds, n >= 3 ? x : 5.0f, n >= 4 ? y : 0.0f,
                                n >= 5 ? z : 0.0f, chained && std::string{action} != "rwaggle");
            g_rstrokes.push_back(st);
            const int k = ++g_rstroke_count;
            const auto b0 = BladeOf(st.q0), b1 = BladeOf(st.q1);
            Log("MOCKXR input %s #%d: hand peak %.2f m/s, stroke %.3f s at +%.2f s (blade (%.2f "
                "%.2f %.2f) -> (%.2f %.2f %.2f)), back at +%.2f s%s%s",
                action, k, st.speed, st.T, st.stroke - static_cast<double>(now) * 1e-9, b0[0],
                b0[1], b0[2], b1[0], b1[1], b1[2], st.back_end - static_cast<double>(now) * 1e-9,
                st.kind == 4 ? " (waggle)" : "", chained ? " (chained)" : "");
            if (start + seconds < st.back_end) {
                seconds = static_cast<float>(st.back_end - start); // the next event waits for it
                g_input.back().end = st.back_end;
            }
        }
        if (std::string{action} == "lthrow_over" || std::string{action} == "lthrow_push") {
            const ThrowPlan plan = MakeThrowPlan(action, start, n >= 3 ? x : 5.0f,
                                                 n >= 4 ? y : 0.0f, n >= 5 ? z : 0.0f);
            g_throws.push_back(plan);
            const int k = ++g_throw_count;
            Log("MOCKXR input %s #%d: %.2f m/s yaw %.0f pitch %.0f: wind-up to (%.3f %.3f %.3f) "
                "at +%.2f s, stroke %.3f s to (%.3f %.3f %.3f), release at +%.3f s (dir %.3f %.3f "
                "%.3f), controller turn %.0f -> %.0f deg (%.1f rad/s at the release), "
                "follow-through %.3f s",
                action, k, plan.speed, n >= 4 ? y : 0.0f, n >= 5 ? z : 0.0f, plan.S[0], plan.S[1],
                plan.S[2], plan.top - static_cast<double>(now) * 1e-9, plan.T, plan.R[0],
                plan.R[1], plan.R[2], plan.release - static_cast<double>(now) * 1e-9, plan.d[0],
                plan.d[1], plan.d[2], plan.theta_top, plan.theta_rel,
                std::abs(plan.theta_peak - plan.theta_top) * 3.14159265f / 180.0f * 2.0f / plan.T,
                plan.Tf);
            if (std::getenv("MOCKXR_THROW_DUMPS") != nullptr) {
                const std::string name = "throw" + std::to_string(k);
                g_auto_dumps.push_back({plan.top + 0.05, name + "_windup"});
                g_auto_dumps.push_back({plan.release + 0.01, name + "_release"});
                g_auto_dumps.push_back({plan.follow_end + 0.03, name + "_follow"});
                g_auto_dumps.push_back({plan.follow_end + 0.3, name + "_back"});
            }
        }
        Log("MOCKXR input queued %s %.2f s (%.2f, %.2f, %.2f) at +%.2f s", action, seconds,
            n >= 3 ? x : 1.0f, n >= 4 ? y : 0.0f, n >= 5 ? z : 0.0f,
            start - static_cast<double>(now) * 1e-9);
        previous_start = start;
        if (std::string{action} != "lean") {
            t = std::max(t, start + seconds + 0.25);
        }
    }
}

// input.txt `drift` / `recenter` events that have started: applied once (the app polls events
// every frame).
void ApplyHeadEvents() {
    const double t = static_cast<double>(NowNs()) * 1e-9;
    std::lock_guard lk{g_input_mutex};
    for (auto& e : g_input) {
        if (e.applied || t < e.start ||
            (e.action != "drift" && e.action != "recenter" && e.action != "lhand" &&
             e.action != "recenter_pose" && e.action != "laim" && e.action != "lean" &&
             e.action != "look" && e.action != "head_y")) {
            continue;
        }
        e.applied = true;
        if (e.action == "look") {
            std::lock_guard gk{g_look_mutex};
            if (e.x >= 900.0f) {
                g_look.active = false;
                Log("MOCKXR input look: released (back to the scripted head)");
                continue;
            }
            float p0 = 0.0f, y0 = 0.0f;
            if (g_look.active) {
                LookAt(t, p0, y0);
            }
            constexpr float D = 3.14159265f / 180.0f;
            g_look = {true, p0, y0, e.x * D, e.y * D, e.start, e.end};
            Log("MOCKXR input look: head to pitch %.1f yaw %.1f deg (from %.1f %.1f) over %.2f s",
                e.x, e.y, p0 / D, y0 / D, e.end - e.start);
            continue;
        }
        if (e.action == "laim") {
            // From wherever the grip is now (an aim being eased to: its pose at this moment;
            // none: the scripted pose, taken at the next locate).
            LiveAim& a = g_live_aim;
            const bool was = a.active;
            if (was && a.from_set) {
                BP::HandPose now{};
                now.p = a.to_p;
                now.q = a.to_q;
                AimPoseAt(a, t, now);
                a.from_p = now.p;
                a.from_q = now.q;
            }
            a.from_set = was && a.from_set;
            a.t0 = e.start;
            a.t1 = e.end;
            if (e.x >= 900.0f) {
                a.release = true;
                a.active = was;
                Log("MOCKXR input laim: back to the scripted pose over %.2f s", e.end - e.start);
                continue;
            }
            constexpr float D = 3.14159265f / 180.0f;
            const int attach = e.z > 300.0f ? static_cast<int>(e.z) : 347;
            const float cy = std::cos(e.x * D), sy = std::sin(e.x * D);
            const float cp = std::cos(e.y * D), sp = std::sin(e.y * D);
            const std::array<float, 3> d{-sy * cp, sp, -cy * cp};
            const std::array<float, 3> shoulder{-0.17f, 1.38f, 0.05f};
            a.to_p = {shoulder[0] + d[0] * 0.45f, shoulder[1] + d[1] * 0.45f,
                      shoulder[2] + d[2] * 0.45f};
            a.to_q = AimRotation(BarrelInGrip(attach), d);
            a.release = false;
            a.active = true;
            const XrVector3f check =
                Rotate({a.to_q[0], a.to_q[1], a.to_q[2], a.to_q[3]},
                       {BarrelInGrip(attach)[0], BarrelInGrip(attach)[1], BarrelInGrip(attach)[2]});
            Log("MOCKXR input laim: attach %d barrel to yaw %.1f pitch %.1f (dir %.3f %.3f %.3f, "
                "check %.3f %.3f %.3f) grip at (%.3f %.3f %.3f) q (%.3f %.3f %.3f %.3f) over %.2f s",
                attach, e.x, e.y, d[0], d[1], d[2], check.x, check.y, check.z, a.to_p[0],
                a.to_p[1], a.to_p[2], a.to_q[0], a.to_q[1], a.to_q[2], a.to_q[3], e.end - e.start);
            continue;
        }
        if (e.action == "lhand") {
            float now[3];
            LiveLeftAt(t, now);
            for (int i = 0; i < 3; ++i) {
                g_live_left.from[i] = now[i];
            }
            g_live_left.to[0] = e.x;
            g_live_left.to[1] = e.y;
            g_live_left.to[2] = e.z;
            g_live_left.t0 = e.start;
            g_live_left.t1 = e.end;
            Log("MOCKXR input lhand: left grip to (%.3f, %.3f, %.3f) over %.2f s", e.x, e.y, e.z,
                e.end - e.start);
            continue;
        }
        if (e.action == "recenter_pose") {
            // The head stays; LOCAL moves to it (its heading, the old LOCAL's height) at
            // changeTime = now + e.x ms.
            const int64_t now = NowNs();
            XrPosef head{};
            HeadPose(now, head);
            const XrVector3f f = Rotate(head.orientation, {0.0f, 0.0f, -1.0f});
            const float yaw = std::atan2(-f.x, -f.z);
            std::lock_guard sk{g_space_pose_mutex};
            const XrPosef current =
                !g_runtime_local.active  ? XrPosef{{0, 0, 0, 1}, {0, 0, 0}}
                : now >= g_runtime_local.at ? g_runtime_local.after
                                            : g_runtime_local.before;
            const XrPosef after{{0.0f, std::sin(0.5f * yaw), 0.0f, std::cos(0.5f * yaw)},
                                {head.position.x, current.position.y, head.position.z}};
            const int64_t at = now + static_cast<int64_t>(static_cast<double>(e.x) * 1e6);
            g_runtime_local = {true, current, after, at};
            XrEventDataReferenceSpaceChangePending c{
                XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING};
            c.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
            c.changeTime = at;
            c.poseValid = XR_TRUE;
            c.poseInPreviousSpace = PoseInBase(current, after);
            {
                std::lock_guard ek{g_space_event_mutex};
                g_space_change_events.push_back(c);
            }
            const auto& q = c.poseInPreviousSpace;
            Log("MOCKXR input recenter_pose: LOCAL moves to yaw %.1f deg, (%.3f, %.3f, %.3f) at "
                "changeTime +%.0f ms; its pose in the previous LOCAL: yaw %.1f deg, (%.3f, %.3f, "
                "%.3f)",
                yaw * 57.29578f, after.position.x, after.position.y, after.position.z, e.x,
                2.0f * std::atan2(q.orientation.y, q.orientation.w) * 57.29578f, q.position.x,
                q.position.y, q.position.z);
            continue;
        }
        if (e.action == "lean") {
            g_lean_amp = e.x;
            g_lean_period = e.y > 0.0f ? e.y : 4.0f;
            g_lean_duck = e.z;
            g_lean_t0 = e.start;
            g_lean_t1 = e.end;
            Log("MOCKXR input lean: +-%.2f m sideways, period %.2f s, duck %.2f m, for %.2f s "
                "(from the centre)",
                e.x, g_lean_period.load(), e.z, e.end - e.start);
            continue;
        }
        if (e.action == "head_y") {
            g_head_offset_y = e.x;
            Log("MOCKXR input head_y: head at y %.3f (%.3f m over the floor at %.3f)",
                1.6f + e.x, 1.6f + e.x - FloorY(), FloorY());
            continue;
        }
        if (e.action == "drift") {
            g_head_offset_x = e.x;
            g_head_offset_z = e.y;
            Log("MOCKXR input drift: head at (%.2f, 1.60, %.2f)", e.x, e.y);
        } else {
            g_head_offset_x = 0.0f;
            g_head_offset_z = 0.0f;
            ++g_space_changes;
            Log("MOCKXR input recenter: head back at (0.00, 1.60, 0.00), space change sent");
        }
    }
}

// The scripted event holding `name` now (nullptr: none).
bool ScriptedInput(const std::string& name, float& x, float& y) {
    const double t = static_cast<double>(NowNs()) * 1e-9;
    std::lock_guard lk{g_input_mutex};
    for (auto& e : g_input) {
        if (e.action == name && t >= e.start && t < e.end) {
            x = e.x;
            y = e.y;
            static std::string logged;
            const std::string key = e.action + std::to_string(e.start);
            if (key != logged) {
                logged = key;
                Log("MOCKXR input %s (%.2f, %.2f) for %.2f s", e.action.c_str(), e.x, e.y,
                    e.end - e.start);
            }
            return true;
        }
    }
    return false;
}

// ---- MOCKXR_BUTTONS (scripted boolean presses on the script clock) ---------------------------
struct ButtonPress {
    std::string action;
    double start{};
    double duration{0.1};
    float x{1.0f}; // float / vector actions
    float y{0.0f};
};

const std::vector<ButtonPress>& ButtonScript() {
    static const std::vector<ButtonPress> script = [] {
        std::vector<ButtonPress> out;
        const char* value = std::getenv("MOCKXR_BUTTONS");
        if (value == nullptr) {
            return out;
        }
        const std::string text{value};
        size_t start = 0;
        while (start < text.size()) {
            size_t comma = text.find(',', start);
            if (comma == std::string::npos) {
                comma = text.size();
            }
            const std::string token = text.substr(start, comma - start);
            start = comma + 1;
            const size_t at = token.find('@');
            if (at == std::string::npos || at == 0) {
                continue;
            }
            ButtonPress press{token.substr(0, at)};
            press.start = std::atof(token.c_str() + at + 1);
            if (const size_t plus = token.find('+', at); plus != std::string::npos) {
                press.duration = std::max(std::atof(token.c_str() + plus + 1), 0.01);
            }
            if (const size_t eq = token.find('=', at); eq != std::string::npos) {
                press.x = static_cast<float>(std::atof(token.c_str() + eq + 1));
                if (const size_t colon = token.find(':', eq); colon != std::string::npos) {
                    press.y = static_cast<float>(std::atof(token.c_str() + colon + 1));
                }
            }
            out.push_back(press);
        }
        Log("MOCKXR buttons '%s': %zu presses on the script clock (starts with the game, after "
            "1200 projection frames)",
            value, out.size());
        return out;
    }();
    return script;
}

// Display time the script clock (MOCKXR_BUTTONS) started at (0 = not yet).
std::atomic<int64_t> g_script_start{0};

bool ScriptedButton(const std::string& name, float* x = nullptr, float* y = nullptr) {
    const auto& script = ButtonScript();
    const int64_t start = g_script_start.load();
    if (script.empty() || start == 0) {
        return false;
    }
    const double t = static_cast<double>(g_display_time.load() - start) * 1e-9;
    static std::vector<bool> logged(script.size(), false);
    for (size_t i = 0; i < script.size(); ++i) {
        const ButtonPress& press = script[i];
        if (press.action == name && t >= press.start && t < press.start + press.duration) {
            if (!logged[i]) {
                logged[i] = true;
                Log("MOCKXR button %s pressed at t=%.3f for %.2f s value (%.2f, %.2f) (frame %llu)",
                    name.c_str(), t, press.duration, press.x, press.y,
                    (unsigned long long)(g_session ? g_session->frames : 0));
            }
            if (x != nullptr) {
                *x = press.x;
            }
            if (y != nullptr) {
                *y = press.y;
            }
            return true;
        }
    }
    return false;
}

// MOCKXR_UNFOCUS: VISIBLE at each <t>, FOCUSED again <dur> s later, on the script clock.
void ApplyFocusScript() {
    struct Window {
        double start;
        double duration;
        int phase; // 0 = before, 1 = unfocused, 2 = done
    };
    static std::vector<Window> windows = [] {
        std::vector<Window> out;
        const char* value = std::getenv("MOCKXR_UNFOCUS");
        if (value == nullptr) {
            return out;
        }
        const std::string text{value};
        size_t start = 0;
        while (start < text.size()) {
            size_t comma = text.find(',', start);
            if (comma == std::string::npos) {
                comma = text.size();
            }
            const std::string token = text.substr(start, comma - start);
            start = comma + 1;
            const size_t plus = token.find('+');
            if (token.empty() || plus == std::string::npos) {
                continue;
            }
            out.push_back({std::atof(token.c_str()), std::atof(token.c_str() + plus + 1), 0});
        }
        Log("MOCKXR focus script '%s': %zu windows", value, out.size());
        return out;
    }();
    const int64_t script_start = g_script_start.load();
    if (windows.empty() || script_start == 0 || g_session == nullptr) {
        return;
    }
    const double t = static_cast<double>(g_display_time.load() - script_start) * 1e-9;
    std::lock_guard lk{g_mutex};
    for (Window& w : windows) {
        if (w.phase == 0 && t >= w.start) {
            w.phase = 1;
            g_session->events.push_back(XR_SESSION_STATE_VISIBLE);
            Log("MOCKXR focus lost at t=%.3f (VISIBLE for %.2f s)", t, w.duration);
        } else if (w.phase == 1 && t >= w.start + w.duration) {
            w.phase = 2;
            g_session->events.push_back(XR_SESSION_STATE_FOCUSED);
            Log("MOCKXR focus regained at t=%.3f", t);
        }
    }
}

// MOCKXR_PRESET_GRIP=1: the presets' (and MOCKXR_INPUT's) right grip squeeze is applied.
bool PresetGrip() {
    static const bool on = [] {
        const char* v = std::getenv("MOCKXR_PRESET_GRIP");
        return v != nullptr && std::atoi(v) != 0;
    }();
    return on;
}

// Seconds since the hands timeline (else the script clock) started, -1 before (overlay logs).
double ScriptSeconds(int64_t display_time) {
    int64_t start = g_hands_start.load();
    if (start == 0) {
        start = g_script_start.load();
    }
    return start == 0 ? -1.0 : static_cast<double>(display_time - start) * 1e-9;
}

// Segment dump names: <segment>, or seg<index>_<segment> when the timeline repeats a name.
std::string SegmentDumpName(uint64_t segment) {
    const auto& timeline = HandsTimeline();
    static const bool repeats = [&timeline] {
        for (size_t i = 0; i < timeline.size(); ++i) {
            for (size_t j = i + 1; j < timeline.size(); ++j) {
                if (timeline[i]->name == timeline[j]->name) {
                    return true;
                }
            }
        }
        return false;
    }();
    const std::string name{timeline[segment]->name};
    return repeats ? "seg" + std::to_string(segment) + "_" + name : name;
}

// The overlay layer's views: the located FOV and a rect inside the released swapchain image.
void CheckOverlay(const XrCompositionLayerProjection& p) {
    static int problems = 0;
    constexpr float R = 57.2957795f;
    if (p.viewCount != 2 && problems < 20) {
        ++problems;
        Log("ERROR: overlay viewCount %u", p.viewCount);
    }
    for (uint32_t v = 0; v < p.viewCount && v < 2; ++v) {
        const auto& view = p.views[v];
        std::lock_guard lk{g_mutex};
        auto it = g_swapchains.find(view.subImage.swapchain);
        if (it == g_swapchains.end() || it->second.last_released < 0) {
            if (problems++ < 20) {
                Log("ERROR: overlay view %u references an unreleased swapchain", v);
            }
            continue;
        }
        const XrRect2Di& r = view.subImage.imageRect;
        const Swapchain& sc = it->second;
        if ((r.offset.x < 0 || r.offset.y < 0 || r.extent.width < 1 || r.extent.height < 1 ||
             r.offset.x + r.extent.width > static_cast<int32_t>(sc.width) ||
             r.offset.y + r.extent.height > static_cast<int32_t>(sc.height)) &&
            problems++ < 20) {
            Log("ERROR: overlay view %u imageRect %d,%d %dx%d outside the %ux%u swapchain", v,
                r.offset.x, r.offset.y, r.extent.width, r.extent.height, sc.width, sc.height);
        }
        const XrFovf located = LocatedFov(v);
        const XrFovf& f = view.fov;
        constexpr float tolerance = 0.001f;
        if (QuestFov() &&
            (std::fabs(f.angleLeft - located.angleLeft) > tolerance ||
             std::fabs(f.angleRight - located.angleRight) > tolerance ||
             std::fabs(f.angleUp - located.angleUp) > tolerance ||
             std::fabs(f.angleDown - located.angleDown) > tolerance) &&
            problems++ < 20) {
            Log("ERROR: overlay view %u fov l%.2f r%.2f u%.2f d%.2f differs from the located "
                "l%.2f r%.2f u%.2f d%.2f",
                v, f.angleLeft * R, f.angleRight * R, f.angleUp * R, f.angleDown * R,
                located.angleLeft * R, located.angleRight * R, located.angleUp * R,
                located.angleDown * R);
        }
    }
}

// Dumps both views of the overlay layer (RGBA) as <path_prefix>_L.png / _R.png, with `label` log
// lines in the `dump` format (submitted pose, FOV, rect, display pose, located FOV, hand marks).
void DumpOverlay(const XrCompositionLayerProjection& p, int64_t display_time,
                 const std::string& label, const std::string& path_prefix) {
    for (uint32_t v = 0; v < 2 && v < p.viewCount; ++v) {
        Swapchain sc;
        {
            std::lock_guard lk{g_mutex};
            sc = g_swapchains[p.views[v].subImage.swapchain];
        }
        const auto& pose = p.views[v].pose;
        const auto& fov = p.views[v].fov;
        const auto& rect = p.views[v].subImage.imageRect;
        XrPosef display{};
        HeadPose(display_time, display);
        const XrFovf located = LocatedFov(v);
        Log("%s eye %u pose q(%.5f %.5f %.5f %.5f) p(%.4f %.4f %.4f) fov l%.3f r%.3f u%.3f d%.3f "
            "rect %d,%d %dx%d disp q(%.5f %.5f %.5f %.5f) located l%.3f r%.3f u%.3f d%.3f",
            label.c_str(), v, pose.orientation.x, pose.orientation.y, pose.orientation.z,
            pose.orientation.w, pose.position.x, pose.position.y, pose.position.z,
            fov.angleLeft * 57.2958f, fov.angleRight * 57.2958f, fov.angleUp * 57.2958f,
            fov.angleDown * 57.2958f, rect.offset.x, rect.offset.y, rect.extent.width,
            rect.extent.height, display.orientation.x, display.orientation.y,
            display.orientation.z, display.orientation.w, located.angleLeft * 57.2958f,
            located.angleRight * 57.2958f, located.angleUp * 57.2958f,
            located.angleDown * 57.2958f);
        std::string marks_text;
        const auto marks = HandMarks(p.views[v], display_time, marks_text);
        if (!marks_text.empty()) {
            Log("%s eye %u%s", label.c_str(), v, marks_text.c_str());
        }
        Log("%s eye %u%s", label.c_str(), v, ViewText(p.views[v], v, display_time).c_str());
        if (sc.last_released >= 0) {
            DumpImage(sc, sc.last_released, rect, path_prefix + (v == 0 ? "_L.png" : "_R.png"),
                      marks, true);
        }
    }
}

// The depth info chained on a projection view (nullptr: none).
const XrCompositionLayerDepthInfoKHR* ViewDepth(const XrCompositionLayerProjectionView& view) {
    const auto* header = static_cast<const XrBaseInStructure*>(view.next);
    while (header != nullptr) {
        if (header->type == XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR) {
            return reinterpret_cast<const XrCompositionLayerDepthInfoKHR*>(header);
        }
        header = header->next;
    }
    return nullptr;
}

// Checks a view's depth info; true when it is usable.
bool CheckDepth(uint32_t v, const XrCompositionLayerProjectionView& view,
                const XrCompositionLayerDepthInfoKHR& d) {
    static int problems = 0;
    const auto fail = [&](const char* what) {
        if (problems++ < 20) {
            Log("ERROR: view %u depth: %s (rect %d,%d %dx%d, depth %.3f..%.3f, near %.4f far %.1f)",
                v, what, d.subImage.imageRect.offset.x, d.subImage.imageRect.offset.y,
                d.subImage.imageRect.extent.width, d.subImage.imageRect.extent.height, d.minDepth,
                d.maxDepth, d.nearZ, d.farZ);
        }
        return false;
    };
    Swapchain sc;
    {
        std::lock_guard lk{g_mutex};
        auto it = g_swapchains.find(d.subImage.swapchain);
        if (it == g_swapchains.end()) {
            return fail("unknown swapchain");
        }
        sc = it->second;
    }
    if (!sc.depth) {
        return fail("not a depth swapchain");
    }
    if (sc.last_released < 0) {
        return fail("the swapchain has no released image");
    }
    const XrRect2Di& r = d.subImage.imageRect;
    if (r.offset.x < 0 || r.offset.y < 0 || r.extent.width < 1 || r.extent.height < 1 ||
        r.offset.x + r.extent.width > static_cast<int32_t>(sc.width) ||
        r.offset.y + r.extent.height > static_cast<int32_t>(sc.height)) {
        return fail("imageRect outside the swapchain");
    }
    if (r.extent.width != view.subImage.imageRect.extent.width ||
        r.extent.height != view.subImage.imageRect.extent.height) {
        return fail("imageRect size differs from the colour's");
    }
    if (!(d.minDepth >= 0.0f && d.maxDepth <= 1.0f && d.minDepth < d.maxDepth)) {
        return fail("depth range not 0 <= min < max <= 1");
    }
    if (!(std::isfinite(d.nearZ) && d.nearZ > 0.0f) || !(d.farZ > 0.0f) || d.nearZ == d.farZ) {
        return fail("nearZ / farZ not positive and different");
    }
    return true;
}

// Dumps a depth view as a PNG (white = 0.5 m, black = far) with decoded sample distances.
void DumpDepth(const XrCompositionLayerDepthInfoKHR& d, const std::string& path,
               const std::string& label) {
    Swapchain sc;
    {
        std::lock_guard lk{g_mutex};
        sc = g_swapchains[d.subImage.swapchain];
    }
    if (sc.last_released < 0 || !sc.depth) {
        return;
    }
    const XrRect2Di rect = ClampRect(sc, d.subImage.imageRect);
    const uint32_t width = static_cast<uint32_t>(rect.extent.width);
    const uint32_t height = static_cast<uint32_t>(rect.extent.height);
    const uint32_t bpp = sc.format == VK_FORMAT_D16_UNORM ? 2 : 4;
    ReadbackImage(
        sc.images[sc.last_released], VK_IMAGE_ASPECT_DEPTH_BIT,
        VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, rect, bpp,
        [d, path, label, width, height, bpp](std::vector<uint8_t> data) {
            // d = (1/n - 1/z) / (1/n - 1/f) (minDepth..maxDepth) -> z = 1 / (1/n - d (1/n - 1/f)).
            const float n = d.nearZ, f = d.farZ;
            const auto metres = [&](uint32_t x, uint32_t y) {
                float v = 0.0f;
                if (bpp == 2) {
                    uint16_t u = 0;
                    std::memcpy(&u, data.data() + (size_t(y) * width + x) * 2, 2);
                    v = u / 65535.0f;
                } else {
                    std::memcpy(&v, data.data() + (size_t(y) * width + x) * 4, 4);
                }
                v = (v - d.minDepth) / (d.maxDepth - d.minDepth);
                const float inv = 1.0f / n - v * (1.0f / n - 1.0f / f);
                return inv > 1e-6f ? 1.0f / inv : 1e6f;
            };
            std::vector<uint8_t> gray(size_t(width) * height * 4);
            std::vector<float> raw(size_t(width) * height); // metres, for reprojection checks
            for (uint32_t y = 0; y < height; ++y) {
                for (uint32_t x = 0; x < width; ++x) {
                    const float z = metres(x, y);
                    raw[size_t(y) * width + x] = z;
                    const uint8_t g =
                        static_cast<uint8_t>(std::clamp(0.5f / z, 0.0f, 1.0f) * 255.0f);
                    uint8_t* px = gray.data() + (size_t(y) * width + x) * 4;
                    px[0] = px[1] = px[2] = g;
                    px[3] = 255;
                }
            }
            Log("%s: %ux%u near %.3f far %.1f, metres at centre %.3f, left %.3f, right %.3f, up "
                "%.3f, down %.3f",
                label.c_str(), width, height, n, f, metres(width / 2, height / 2),
                metres(width / 4, height / 2), metres(3 * width / 4, height / 2),
                metres(width / 2, height / 4), metres(width / 2, 3 * height / 4));
            WritePng(path, width, height, gray.data(), true);
            // <name>.f32: width, height (u32), then width * height floats (metres, row-major).
            const std::string raw_path = path.substr(0, path.size() - 4) + ".f32";
            if (FILE* out = BenchmarkFiles::Open(raw_path.c_str(), "wb")) {
                std::fwrite(&width, 4, 1, out);
                std::fwrite(&height, 4, 1, out);
                std::fwrite(raw.data(), sizeof(float), raw.size(), out);
                std::fclose(out);
            }
        });
}

uint64_t g_depth_frames = 0;

XrResult EndFrameInWorld(const XrFrameEndInfo* info);
// Layers submitted in an offset reference space (an application's recentred LOCAL space) are
// converted to the world (LOCAL) first, so every check, log and dump below sees world poses, as
// before. Logged once per space change.
// A quad / cylinder layer's space as a world pose (false: the world itself): an offset reference
// space (SpaceOffset), or a VIEW space: the head at the frame's display time (a head-locked
// layer, e.g. the emulator's HUD fixed to the view), so checks and dumps see it in LOCAL.
bool LayerSpacePose(XrSpace space, XrPosef& pose, int64_t time) {
    if (SpaceOffset(space, pose, time)) {
        return true;
    }
    const auto it = g_spaces.find(space);
    if (space != XR_NULL_HANDLE && it != g_spaces.end() && it->second == 1) {
        HeadPose(time, pose);
        static bool logged = false;
        if (!logged) {
            logged = true;
            Log("MOCKXR space: quad / cylinder layers in VIEW space converted to LOCAL with the "
                "head pose of their frame's display time");
        }
        return true;
    }
    return false;
}

// Owned only by the benchmark process; no input in normal OpenXR runtimes.
std::mutex g_startup_mutex;
BenchmarkStartup::Navigator g_startup_navigation;
bool BenchmarkStartupEnabled() {
    static const bool enabled=[] { const char* v=std::getenv("MOCKXR_BENCHMARK_STARTUP"); return v && std::string_view(v)=="1"; }();
    return enabled;
}

XrResult XRAPI_CALL EndFrame(XrSession, const XrFrameEndInfo* info) {
    if (StopFrameRefused("xrEndFrame")) {
        return XR_ERROR_SESSION_NOT_RUNNING;
    }
    if(BenchmarkStartupEnabled()) {
        bool projection=false, flat=false;
        for(uint32_t i=0;i<info->layerCount;++i) {
            // Menus also submit a projection overlay; only the opaque world counts as gameplay.
            projection |= info->layers[i]->type==XR_TYPE_COMPOSITION_LAYER_PROJECTION && !IsOverlay(info->layers[i]);
            flat |= info->layers[i]->type==XR_TYPE_COMPOSITION_LAYER_QUAD ||
                    info->layers[i]->type==XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR;
        }
        std::lock_guard lock(g_startup_mutex);
        const auto before=g_startup_navigation.attempts;
        const bool was_gameplay=g_startup_navigation.gameplay;
        g_startup_navigation.Observe(double(NowNs())*1e-9,projection,flat);
        if(g_startup_navigation.attempts!=before) Log("Benchmark startup: confirm attempt %u",g_startup_navigation.attempts);
        if(!was_gameplay && g_startup_navigation.gameplay) Log("Benchmark startup: gameplay reached; input permanently stopped");
    }
    bool any = false;
    XrPosef offset{};
    for (uint32_t i = 0; i < info->layerCount && !any; ++i) {
        const auto* base = info->layers[i];
        XrSpace space = XR_NULL_HANDLE;
        if (base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            space = reinterpret_cast<const XrCompositionLayerProjection*>(base)->space;
        } else if (base->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            space = reinterpret_cast<const XrCompositionLayerQuad*>(base)->space;
        } else if (base->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
            space = reinterpret_cast<const XrCompositionLayerCylinderKHR*>(base)->space;
        }
        any = base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION
                  ? SpaceOffset(space, offset, info->displayTime)
                  : LayerSpacePose(space, offset, info->displayTime);
    }
    if (!any) {
        return EndFrameInWorld(info);
    }
    constexpr uint32_t Max = 32;
    std::array<XrCompositionLayerProjection, Max> projections{};
    std::array<std::array<XrCompositionLayerProjectionView, 2>, Max> views{};
    std::array<XrCompositionLayerQuad, Max> quads{};
    std::array<XrCompositionLayerCylinderKHR, Max> cylinders{};
    std::array<const XrCompositionLayerBaseHeader*, Max> layers{};
    const uint32_t count = std::min(info->layerCount, Max);
    static XrSpace logged_space = XR_NULL_HANDLE;
    for (uint32_t i = 0; i < count; ++i) {
        const auto* base = info->layers[i];
        layers[i] = base;
        if (base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(base);
            XrPosef o{};
            if (!SpaceOffset(p->space, o, info->displayTime) || p->viewCount > 2) {
                continue;
            }
            projections[i] = *p;
            for (uint32_t v = 0; v < p->viewCount; ++v) {
                views[i][v] = p->views[v];
                views[i][v].pose = ComposePose(o, p->views[v].pose);
            }
            projections[i].views = views[i].data();
            layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projections[i]);
            if (logged_space != p->space) {
                logged_space = p->space;
                Log("MOCKXR space: layers in reference space %llu converted to LOCAL (view 0 "
                    "submitted p(%.4f %.4f %.4f) -> p(%.4f %.4f %.4f))",
                    (unsigned long long)reinterpret_cast<uintptr_t>(p->space),
                    p->views[0].pose.position.x, p->views[0].pose.position.y,
                    p->views[0].pose.position.z, views[i][0].pose.position.x,
                    views[i][0].pose.position.y, views[i][0].pose.position.z);
            }
        } else if (base->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            const auto* q = reinterpret_cast<const XrCompositionLayerQuad*>(base);
            XrPosef o{};
            if (LayerSpacePose(q->space, o, info->displayTime)) {
                quads[i] = *q;
                quads[i].pose = ComposePose(o, q->pose);
                layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quads[i]);
            }
        } else if (base->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
            const auto* c = reinterpret_cast<const XrCompositionLayerCylinderKHR*>(base);
            XrPosef o{};
            if (LayerSpacePose(c->space, o, info->displayTime)) {
                cylinders[i] = *c;
                cylinders[i].pose = ComposePose(o, c->pose);
                layers[i] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinders[i]);
            }
        }
    }
    XrFrameEndInfo world = *info;
    world.layerCount = count;
    world.layers = layers.data();
    return EndFrameInWorld(&world);
}
XrResult EndFrameInWorld(const XrFrameEndInfo* info) {
    Session& s = *g_session;
    ++s.frames;
    PollReadbacks();
    if (info->layerCount == 0) {
        ++s.empty_frames;
    }
    const char* dump_dir = BenchmarkFiles::DumpDirectory();
    // The overlay layer of this frame (nullptr: none): checked here, never counted as a game frame.
    const XrCompositionLayerProjection* overlay = nullptr;
    {
        static int problems = 0;
        uint32_t overlays = 0;
        for (uint32_t i = 0; i < info->layerCount; ++i) {
            const auto* base = info->layers[i];
            if (!IsOverlay(base)) {
                continue;
            }
            ++overlays;
            overlay = reinterpret_cast<const XrCompositionLayerProjection*>(base);
            if (i + 1 != info->layerCount && problems++ < 20) {
                Log("ERROR: the overlay is layer %u of %u (it must be the last, on top)", i,
                    info->layerCount);
            }
            if ((base->layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) != 0 &&
                problems++ < 20) {
                Log("ERROR: the overlay layer is flagged UNPREMULTIPLIED_ALPHA (it is "
                    "premultiplied)");
            }
        }
        if (overlays > 1 && problems++ < 20) {
            Log("ERROR: %u overlay layers in one frame", overlays);
        }
        static bool overlay_shown = false;
        if ((overlay != nullptr) != overlay_shown) {
            overlay_shown = overlay != nullptr;
            static int transitions = 0;
            if (transitions++ < 400) {
                Log("MOCKXR overlay %s frame %llu t=%.3f", overlay_shown ? "on" : "off",
                    (unsigned long long)s.frames, ScriptSeconds(info->displayTime));
            }
        }
        if (overlay != nullptr) {
            ++s.overlay_frames;
            CheckOverlay(*overlay);
            static const char* reject = std::getenv("MOCKXR_REJECT_OVERLAY");
            static const bool reject_all = reject != nullptr && std::strcmp(reject, "all") == 0;
            static bool rejected = false;
            static int reject_logs = 0;
            if (reject != nullptr && (!rejected || reject_all) && s.overlay_frames >= 300) {
                rejected = true;
                if (reject_logs++ < 20) {
                    Log("MOCKXR reject: XR_ERROR_LAYER_INVALID for frame %llu (overlay frame %llu)",
                        (unsigned long long)s.frames, (unsigned long long)s.overlay_frames);
                }
                return XR_ERROR_LAYER_INVALID;
            }
        }
    }
    {
        // UI layers (quads / cylinders with source alpha): checked, counted, optionally rejected.
        static int ui_problems = 0;
        static uint64_t ui_frames = 0;
        bool has_ui = false;
        if (info->layerCount > 16 && ui_problems++ < 20) {
            Log("ERROR: %u layers in one frame (Quest: at most 16)", info->layerCount);
        }
        for (uint32_t i = 0; i < info->layerCount; ++i) {
            const auto* base = info->layers[i];
            if (!IsUiLayer(base)) {
                continue;
            }
            has_ui = true;
            const XrSwapchainSubImage& sub =
                base->type == XR_TYPE_COMPOSITION_LAYER_QUAD
                    ? reinterpret_cast<const XrCompositionLayerQuad*>(base)->subImage
                    : reinterpret_cast<const XrCompositionLayerCylinderKHR*>(base)->subImage;
            Swapchain sc;
            bool known = false;
            {
                std::lock_guard lk{g_mutex};
                const auto it = g_swapchains.find(sub.swapchain);
                if (it != g_swapchains.end()) {
                    sc = it->second;
                    known = true;
                }
            }
            const XrRect2Di& r = sub.imageRect;
            if (!known && ui_problems++ < 20) {
                Log("ERROR: UI layer %u uses an unknown swapchain", i);
            } else if (known && (r.offset.x < 0 || r.offset.y < 0 || r.extent.width < 1 ||
                                 r.extent.height < 1 ||
                                 r.offset.x + r.extent.width > static_cast<int32_t>(sc.width) ||
                                 r.offset.y + r.extent.height > static_cast<int32_t>(sc.height)) &&
                       ui_problems++ < 20) {
                Log("ERROR: UI layer %u imageRect %d,%d %dx%d outside the %ux%u swapchain", i,
                    r.offset.x, r.offset.y, r.extent.width, r.extent.height, sc.width, sc.height);
            } else if (known && sc.last_released < 0 && ui_problems++ < 20) {
                Log("ERROR: UI layer %u shows a swapchain with no released image", i);
            }
            if ((base->layerFlags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) != 0 &&
                ui_problems++ < 20) {
                Log("ERROR: UI layer %u is flagged UNPREMULTIPLIED_ALPHA", i);
            }
        }
        if (has_ui) {
            ++ui_frames;
            static const bool reject_ui = std::getenv("MOCKXR_REJECT_UI") != nullptr;
            static bool ui_rejected = false;
            if (reject_ui && !ui_rejected && ui_frames >= 300) {
                ui_rejected = true;
                Log("MOCKXR reject: XR_ERROR_LAYER_INVALID for frame %llu (UI layer frame %llu)",
                    (unsigned long long)s.frames, (unsigned long long)ui_frames);
                return XR_ERROR_LAYER_INVALID;
            }
        }
        static uint64_t ui_logged = 0;
        if (has_ui && (ui_frames == 1 || ui_frames - ui_logged >= 600)) {
            ui_logged = ui_frames;
            Log("MOCKXR ui frames %llu (frame %llu)", (unsigned long long)ui_frames,
                (unsigned long long)s.frames);
        }
    }
    {
        // The submitted layer stack, logged when its kind changes (types, scale, panel size).
        static std::string logged_stack;
        static int stack_logs = 0;
        std::string stack;
        for (uint32_t i = 0; i < info->layerCount; ++i) {
            const auto* base = info->layers[i];
            const XrColor4f c = LayerColorScale(base->next);
            char part[160];
            if (base->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
                const auto* y = reinterpret_cast<const XrCompositionLayerCylinderKHR*>(base);
                std::snprintf(part, sizeof(part), "%scylinder r %.2f angle %.1f aspect %.3f",
                              i ? " + " : "", y->radius, y->centralAngle * 57.2958f,
                              y->aspectRatio);
            } else if (base->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
                const auto* q = reinterpret_cast<const XrCompositionLayerQuad*>(base);
                std::snprintf(part, sizeof(part), "%squad %.2fx%.2f m", i ? " + " : "",
                              q->size.width, q->size.height);
            } else if (IsOverlay(base)) {
                std::snprintf(part, sizeof(part), "%soverlay", i ? " + " : "");
            } else {
                std::snprintf(part, sizeof(part), "%sprojection", i ? " + " : "");
            }
            stack += part;
            if (c.r < 0.999f) {
                stack += " (dimmed)"; // the value is in the detail lines (fades change it per frame)
            }
        }
        if (stack != logged_stack && stack_logs < 200) {
            ++stack_logs;
            logged_stack = stack;
            Log("layers: %s (frame %llu)", stack.empty() ? "none" : stack.c_str(),
                (unsigned long long)s.frames);
            for (uint32_t i = 0; i < info->layerCount; ++i) {
                Log("layers:   %s", LayerText(info->layers[i]).c_str());
            }
        }
        std::string dump_name;
        if (TakeDumpRequest(dump_dir, dump_name)) {
            DumpLayers(info, dump_dir, dump_name);
        }
    }
    for (uint32_t i = 0; i < info->layerCount; ++i) {
        const auto* base = info->layers[i];
        if (IsOverlay(base)) {
            continue; // checked above; dumped with the game layer's dumps
        }
        if (base->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            ++s.projection_frames;
            if (s.projection_frames > 1200) {
                int64_t expected = 0;
                g_script_start.compare_exchange_strong(expected, info->displayTime);
            }
            const auto* p = reinterpret_cast<const XrCompositionLayerProjection*>(base);
            if (p->viewCount != 2) {
                Log("ERROR: projection viewCount %u", p->viewCount);
            }
            for (uint32_t v = 0; v < p->viewCount; ++v) {
                std::lock_guard lk{g_mutex};
                auto it = g_swapchains.find(p->views[v].subImage.swapchain);
                if (it == g_swapchains.end() || it->second.last_released < 0) {
                    Log("ERROR: projection view %u references an unreleased swapchain", v);
                } else {
                    CheckProjectionView(v, p->views[v], it->second);
                }
            }
            {
                // XR_KHR_composition_layer_depth: both views or none.
                uint32_t with_depth = 0;
                bool usable = true;
                for (uint32_t v = 0; v < p->viewCount && v < 2; ++v) {
                    if (const auto* d = ViewDepth(p->views[v])) {
                        ++with_depth;
                        usable &= CheckDepth(v, p->views[v], *d);
                    }
                }
                static int mixed = 0;
                if (with_depth == 1 && mixed++ < 10) {
                    Log("ERROR: depth on one view only");
                }
                static bool depth_on = false;
                const bool on = with_depth == 2 && usable;
                g_depth_frames += on ? 1 : 0;
                if (on != depth_on) {
                    depth_on = on;
                    static int transitions = 0;
                    if (transitions++ < 100) {
                        Log("MOCKXR depth %s frame %llu", on ? "on" : "off",
                            (unsigned long long)s.frames);
                    }
                }
                static const char* reject_depth = std::getenv("MOCKXR_REJECT_DEPTH");
                static const bool reject_depth_all =
                    reject_depth != nullptr && std::strcmp(reject_depth, "all") == 0;
                static bool depth_rejected = false;
                static int depth_reject_logs = 0;
                if (with_depth > 0 && reject_depth != nullptr &&
                    (!depth_rejected || reject_depth_all) && g_depth_frames >= 300) {
                    depth_rejected = true;
                    if (depth_reject_logs++ < 20) {
                        Log("MOCKXR reject: XR_ERROR_VALIDATION_FAILURE for frame %llu (depth frame "
                            "%llu)",
                            (unsigned long long)s.frames, (unsigned long long)g_depth_frames);
                    }
                    return XR_ERROR_VALIDATION_FAILURE;
                }
            }
            if (StampsEnabled() && p->viewCount == 2) {
                QueueStampRead(s, *p, info->displayTime);
            }
            if (TrackEnabled() && p->viewCount == 2) {
                QueueTrackRead(s, *p, info->displayTime);
            }
            if (LatchNs() > 0) {
                static std::once_flag started;
                std::call_once(started, [&s] {
                    std::thread(LatchThread, s.vk_device).detach();
                    Log("MOCKXR latch: %.1f ms before display", static_cast<double>(LatchNs()) * 1e-6);
                });
                LatchCheck check{};
                check.at = info->displayTime - LatchNs();
                check.frame = s.projection_frames;
                {
                    std::lock_guard lk{g_mutex};
                    std::lock_guard fl{g_fence_mutex};
                    for (uint32_t v = 0; v < 2 && v < p->viewCount; ++v) {
                        auto it = g_swapchains.find(p->views[v].subImage.swapchain);
                        if (it == g_swapchains.end() || it->second.last_released < 0 ||
                            it->second.release_fence.empty()) {
                            continue;
                        }
                        const size_t i = static_cast<size_t>(it->second.last_released);
                        check.fence[v] = it->second.release_fence[i];
                        check.serial[v] = g_fence_serial[check.fence[v]];
                    }
                }
                std::lock_guard lk{g_latch_mutex};
                g_latch_queue.push_back(check);
                g_latch_cv.notify_one();
            }
            // MOCKXR_HANDS: the timeline starts with the game running, logs every segment and
            // (MOCKXR_DUMP_SEGMENTS) dumps both eyes 1.5 s into each segment of the first pass.
            if (!HandsTimeline().empty()) {
                int64_t expected = 0;
                if (s.projection_frames > 1200) {
                    g_hands_start.compare_exchange_strong(expected, info->displayTime);
                }
                BP::TimelinePoint point{};
                static uint64_t logged_segment = ~uint64_t{0};
                static uint64_t dumped_segment = ~uint64_t{0};
                if (HandsPoint(info->displayTime, point)) {
                    if (point.segment != logged_segment) {
                        logged_segment = point.segment;
                        Log("MOCKXR seg %.*s t=%.2f (%.*s)",
                            static_cast<int>(point.preset->name.size()), point.preset->name.data(),
                            static_cast<double>(info->displayTime - g_hands_start.load()) * 1e-9,
                            static_cast<int>(point.preset->expect.size()),
                            point.preset->expect.data());
                    }
                    static const bool dump_segments =
                        std::getenv("MOCKXR_DUMP_SEGMENTS") != nullptr;
                    if (dump_dir && dump_segments && point.t >= 1.5 &&
                        point.segment != dumped_segment &&
                        point.segment < HandsTimeline().size() && p->viewCount == 2) {
                        dumped_segment = point.segment;
                        const std::string seg_name = SegmentDumpName(point.segment);
                        for (uint32_t v = 0; v < 2; ++v) {
                            Swapchain sc;
                            {
                                std::lock_guard lk{g_mutex};
                                sc = g_swapchains[p->views[v].subImage.swapchain];
                            }
                            std::string marks_text;
                            const auto marks =
                                HandMarks(p->views[v], info->displayTime, marks_text);
                            Log("segment dump %s eye %u%s", seg_name.c_str(), v,
                                marks_text.c_str());
                            Log("segment dump %s eye %u%s", seg_name.c_str(), v,
                                ViewText(p->views[v], v, info->displayTime).c_str());
                            DumpImage(sc, sc.last_released, p->views[v].subImage.imageRect,
                                      std::string(dump_dir) + "\\" + seg_name +
                                          (v == 0 ? "_L.png" : "_R.png"),
                                      marks);
                        }
                        if (overlay != nullptr) {
                            DumpOverlay(*overlay, info->displayTime,
                                        "segment ovl dump " + seg_name,
                                        std::string(dump_dir) + "\\" + seg_name + "_ovl");
                        } else {
                            Log("segment ovl dump %s: none", seg_name.c_str());
                        }
                    }
                }
            }
            // Dump a stereo pair a few times (after the game is running).
            static const int max_dumps =
                std::getenv("MOCKXR_DUMPS") ? std::atoi(std::getenv("MOCKXR_DUMPS")) : 3;
            static const uint64_t dump_every =
                std::getenv("MOCKXR_DUMP_EVERY")
                    ? static_cast<uint64_t>(std::max(std::atoi(std::getenv("MOCKXR_DUMP_EVERY")), 2))
                    : 200;
            if (dump_dir && s.projection_frames % dump_every == dump_every / 2 &&
                s.dumps < max_dumps && s.projection_frames > 1200) {
                ++s.dumps;
                for (uint32_t v = 0; v < 2; ++v) {
                    Swapchain sc;
                    {
                        std::lock_guard lk{g_mutex};
                        sc = g_swapchains[p->views[v].subImage.swapchain];
                    }
                    const auto& pose = p->views[v].pose;
                    const auto& fov = p->views[v].fov;
                    const auto& rect = p->views[v].subImage.imageRect;
                    // What the compositor would use: the head pose at this frame's display
                    // time and the eye's located FOV (for the stereo/compositor simulation).
                    XrPosef display{};
                    HeadPose(info->displayTime, display);
                    const XrFovf located = LocatedFov(v);
                    Log("dump %d eye %u pose q(%.5f %.5f %.5f %.5f) p(%.4f %.4f %.4f) fov l%.3f "
                        "r%.3f u%.3f d%.3f rect %d,%d %dx%d disp q(%.5f %.5f %.5f %.5f) "
                        "located l%.3f r%.3f u%.3f d%.3f",
                        s.dumps, v, pose.orientation.x, pose.orientation.y, pose.orientation.z,
                        pose.orientation.w, pose.position.x, pose.position.y, pose.position.z,
                        fov.angleLeft * 57.2958f, fov.angleRight * 57.2958f, fov.angleUp * 57.2958f,
                        fov.angleDown * 57.2958f, rect.offset.x, rect.offset.y, rect.extent.width,
                        rect.extent.height, display.orientation.x, display.orientation.y,
                        display.orientation.z, display.orientation.w,
                        located.angleLeft * 57.2958f, located.angleRight * 57.2958f,
                        located.angleUp * 57.2958f, located.angleDown * 57.2958f);
                    std::string marks_text;
                    const auto marks = HandMarks(p->views[v], info->displayTime, marks_text);
                    if (!marks_text.empty()) {
                        Log("dump %d eye %u%s", s.dumps, v, marks_text.c_str());
                    }
                    Log("dump %d eye %u%s", s.dumps, v,
                        ViewText(p->views[v], v, info->displayTime).c_str());
                    DumpImage(sc, sc.last_released, rect,
                              std::string(dump_dir) + "\\eye" + std::to_string(s.dumps) + "_" +
                                  (v == 0 ? "L" : "R") + ".png",
                              marks);
                    if (const auto* d = ViewDepth(p->views[v])) {
                        DumpDepth(*d,
                                  std::string(dump_dir) + "\\eye" + std::to_string(s.dumps) +
                                      "_depth_" + (v == 0 ? "L" : "R") + ".png",
                                  "depth dump " + std::to_string(s.dumps) + " eye " +
                                      std::to_string(v));
                    }
                }
                if (overlay != nullptr) {
                    DumpOverlay(*overlay, info->displayTime, "ovl dump " + std::to_string(s.dumps),
                                std::string(dump_dir) + "\\ovl" + std::to_string(s.dumps));
                } else {
                    Log("ovl dump %d: none", s.dumps);
                }
            }
        } else if (base->type == XR_TYPE_COMPOSITION_LAYER_QUAD) {
            ++s.quad_frames;
            if (dump_dir && s.quad_frames == 300) {
                const auto* q = reinterpret_cast<const XrCompositionLayerQuad*>(base);
                Swapchain sc;
                {
                    std::lock_guard lk{g_mutex};
                    sc = g_swapchains[q->subImage.swapchain];
                }
                Log("quad pose p(%.2f %.2f %.2f) size %.2fx%.2f", q->pose.position.x,
                    q->pose.position.y, q->pose.position.z, q->size.width, q->size.height);
                DumpImage(sc, sc.last_released, FullRect(sc), std::string(dump_dir) + "\\quad.png");
                if (overlay != nullptr) {
                    DumpOverlay(*overlay, info->displayTime, "quad ovl dump",
                                std::string(dump_dir) + "\\quad_ovl");
                } else {
                    Log("quad ovl dump: none");
                }
            }
        } else if (base->type == XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR) {
            ++s.cylinder_frames;
            const auto* y = reinterpret_cast<const XrCompositionLayerCylinderKHR*>(base);
            if (!(y->radius > 0.0f) || !(y->centralAngle > 0.0f) || !(y->aspectRatio > 0.0f)) {
                static int bad = 0;
                if (bad++ < 10) {
                    Log("ERROR: cylinder radius %.3f angle %.3f aspect %.3f", y->radius,
                        y->centralAngle, y->aspectRatio);
                }
            }
        } else {
            Log("ERROR: unexpected layer type %d", base->type);
        }
    }
    const int64_t now = NowNs();
    if (now - s.last_report > 2'000'000'000) {
        s.last_report = now;
        char latch[64] = "";
        if (LatchNs() > 0) {
            std::snprintf(latch, sizeof(latch), " latch late=%llu of %llu",
                          (unsigned long long)g_latch_late.load(),
                          (unsigned long long)g_latch_checked.load());
        }
        Log("frames=%llu projection=%llu quad=%llu cylinder=%llu overlay=%llu empty=%llu depth=%llu%s",
            (unsigned long long)s.frames, (unsigned long long)s.projection_frames,
            (unsigned long long)s.quad_frames, (unsigned long long)s.cylinder_frames,
            (unsigned long long)s.overlay_frames, (unsigned long long)s.empty_frames,
            (unsigned long long)g_depth_frames, latch);
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL LocateViews(XrSession, const XrViewLocateInfo* info, XrViewState* state,
                                uint32_t cap, uint32_t* count, XrView* views) {
    *count = 2;
    state->viewStateFlags = XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    XrPosef head;
    HeadPose(info->displayTime, head);
    for (uint32_t i = 0; i < cap && i < 2; ++i) {
        const float offset = i == 0 ? -0.032f : 0.032f;
        const XrVector3f d = Rotate(head.orientation, {offset, 0, 0});
        views[i].pose = {head.orientation,
                         {head.position.x + d.x, head.position.y + d.y, head.position.z + d.z}};
        views[i].fov = LocatedFov(i);
    }
    XrPosef offset{};
    if (SpaceOffset(info->space, offset, info->displayTime)) {
        for (uint32_t i = 0; i < cap && i < 2; ++i) {
            views[i].pose = PoseInBase(offset, views[i].pose);
        }
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL GetViewConfigurationProperties(XrInstance, XrSystemId,
                                                   XrViewConfigurationType type,
                                                   XrViewConfigurationProperties* props) {
    props->viewConfigurationType = type;
    props->fovMutable = QuestFov() ? XR_FALSE : XR_TRUE;
    Log("xrGetViewConfigurationProperties: fovMutable=%d%s", props->fovMutable,
        QuestFov() ? " (Quest 3 FOV)" : "");
    return XR_SUCCESS;
}

// ---- actions ------------------------------------------------------------------------
XrResult XRAPI_CALL StringToPath(XrInstance, const char* s, XrPath* path) {
    std::lock_guard lk{g_mutex};
    auto it = g_paths.find(s);
    if (it == g_paths.end()) {
        const XrPath p = g_paths.size() + 1;
        g_paths[s] = p;
        g_path_names[p] = s;
        *path = p;
    } else {
        *path = it->second;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL PathToString(XrInstance, XrPath p, uint32_t cap, uint32_t* count, char* buf) {
    return FillString(g_path_names[p].c_str(), cap, count, buf);
}
XrResult XRAPI_CALL CreateActionSet(XrInstance, const XrActionSetCreateInfo*, XrActionSet* set) {
    *set = NewHandle<XrActionSet>();
    return XR_SUCCESS;
}
XrResult XRAPI_CALL DestroyActionSet(XrActionSet) {
    return XR_SUCCESS;
}
XrResult XRAPI_CALL CreateAction(XrActionSet set, const XrActionCreateInfo* info, XrAction* action) {
    // Like Meta's runtime: names and localized names must be unique within an action set
    // (a duplicate localized name cost every controller action on the Quest once).
    static std::map<XrActionSet, std::set<std::string>> names, localized;
    if (!names[set].insert(info->actionName).second) {
        Log("ERROR: xrCreateAction: duplicate action name '%s' (XR_ERROR_NAME_DUPLICATED)",
            info->actionName);
        return XR_ERROR_NAME_DUPLICATED;
    }
    if (!localized[set].insert(info->localizedActionName).second) {
        Log("ERROR: xrCreateAction: duplicate localized name '%s' for '%s' "
            "(XR_ERROR_LOCALIZED_NAME_DUPLICATED)",
            info->localizedActionName, info->actionName);
        return XR_ERROR_LOCALIZED_NAME_DUPLICATED;
    }
    *action = NewHandle<XrAction>();
    g_actions[*action] = {info->actionType, info->actionName};
    return XR_SUCCESS;
}
XrResult XRAPI_CALL DestroyAction(XrAction) {
    return XR_SUCCESS;
}
// ---- controller profiles ----------------------------------------------------------------
// The component paths of each interaction profile the spec defines (OpenXR 1.1, 6.4 and the
// controller extensions), per hand ("both", "left", "right"), and the extension it needs ("" =
// core). The emulator's maps (src/core/vr/xr_bindings.h) are checked against the same lists by
// tests/test_xr_bindings.cpp.
struct ProfileSpec {
    std::string extension;
    std::vector<std::pair<std::string, std::string>> components; // {hand, "/input/..."}
};
const std::map<std::string, ProfileSpec>& ProfileSpecs() {
    static const std::map<std::string, ProfileSpec> specs = [] {
        std::map<std::string, ProfileSpec> s;
        const auto add = [&s](const char* profile, const char* hand,
                              std::initializer_list<const char*> paths) {
            for (const char* p : paths) {
                s[profile].components.emplace_back(hand, p);
            }
        };
        for (const char* profile : {"/interaction_profiles/oculus/touch_controller",
                                    "/interaction_profiles/meta/touch_controller_plus"}) {
            add(profile, "left",
                {"/input/x/click", "/input/x/touch", "/input/y/click", "/input/y/touch",
                 "/input/menu/click"});
            add(profile, "right",
                {"/input/a/click", "/input/a/touch", "/input/b/click", "/input/b/touch",
                 "/input/system/click"});
            add(profile, "both",
                {"/input/squeeze/value", "/input/trigger/value", "/input/trigger/touch",
                 "/input/thumbstick/x", "/input/thumbstick/y", "/input/thumbstick/click",
                 "/input/thumbstick/touch", "/input/thumbrest/touch", "/input/grip/pose",
                 "/input/aim/pose", "/output/haptic"});
        }
        s["/interaction_profiles/meta/touch_controller_plus"].extension =
            "XR_META_touch_controller_plus";
        add("/interaction_profiles/meta/touch_controller_plus", "both",
            {"/input/thumb_meta/proximity_meta", "/input/trigger/proximity_meta",
             "/input/trigger/curl_meta", "/input/trigger/slide_meta", "/input/trigger/force"});
        add("/interaction_profiles/valve/index_controller", "both",
            {"/input/system/click", "/input/system/touch", "/input/a/click", "/input/a/touch",
             "/input/b/click", "/input/b/touch", "/input/squeeze/value", "/input/squeeze/force",
             "/input/trigger/click", "/input/trigger/value", "/input/trigger/touch",
             "/input/thumbstick/x", "/input/thumbstick/y", "/input/thumbstick/click",
             "/input/thumbstick/touch", "/input/trackpad/x", "/input/trackpad/y",
             "/input/trackpad/force", "/input/trackpad/touch", "/input/grip/pose",
             "/input/aim/pose", "/output/haptic"});
        add("/interaction_profiles/htc/vive_controller", "both",
            {"/input/system/click", "/input/squeeze/click", "/input/menu/click",
             "/input/trigger/click", "/input/trigger/value", "/input/trackpad/x",
             "/input/trackpad/y", "/input/trackpad/click", "/input/trackpad/touch",
             "/input/grip/pose", "/input/aim/pose", "/output/haptic"});
        add("/interaction_profiles/microsoft/motion_controller", "both",
            {"/input/menu/click", "/input/squeeze/click", "/input/trigger/value",
             "/input/thumbstick/x", "/input/thumbstick/y", "/input/thumbstick/click",
             "/input/trackpad/x", "/input/trackpad/y", "/input/trackpad/click",
             "/input/trackpad/touch", "/input/grip/pose", "/input/aim/pose", "/output/haptic"});
        s["/interaction_profiles/hp/mixed_reality_controller"].extension =
            "XR_EXT_hp_mixed_reality_controller";
        add("/interaction_profiles/hp/mixed_reality_controller", "left",
            {"/input/x/click", "/input/y/click"});
        add("/interaction_profiles/hp/mixed_reality_controller", "right",
            {"/input/a/click", "/input/b/click"});
        add("/interaction_profiles/hp/mixed_reality_controller", "both",
            {"/input/menu/click", "/input/squeeze/value", "/input/trigger/value",
             "/input/thumbstick/x", "/input/thumbstick/y", "/input/thumbstick/click",
             "/input/grip/pose", "/input/aim/pose", "/output/haptic"});
        for (const char* profile : {"/interaction_profiles/bytedance/pico4_controller",
                                    "/interaction_profiles/bytedance/pico_neo3_controller"}) {
            s[profile].extension = "XR_BD_controller_interaction";
            add(profile, "left", {"/input/x/click", "/input/x/touch", "/input/y/click",
                                  "/input/y/touch"});
            add(profile, "right", {"/input/a/click", "/input/a/touch", "/input/b/click",
                                   "/input/b/touch"});
            add(profile, "both",
                {"/input/system/click", "/input/trigger/click", "/input/trigger/value",
                 "/input/trigger/touch", "/input/thumbstick/y", "/input/thumbstick/x",
                 "/input/thumbstick/click", "/input/thumbstick/touch", "/input/squeeze/click",
                 "/input/squeeze/value", "/input/grip/pose", "/input/aim/pose",
                 "/output/haptic"});
        }
        add("/interaction_profiles/bytedance/pico4_controller", "left", {"/input/menu/click"});
        add("/interaction_profiles/bytedance/pico_neo3_controller", "both",
            {"/input/menu/click"});
        add("/interaction_profiles/khr/simple_controller", "both",
            {"/input/select/click", "/input/menu/click", "/input/grip/pose", "/input/aim/pose",
             "/output/haptic"});
        return s;
    }();
    return specs;
}
// Why a suggested binding is not one the profile has, or not one the action's type can take
// ("" = valid).
std::string BindingProblem(const ProfileSpec& spec, XrActionType type, const std::string& path) {
    std::string hand, rest;
    for (const char* h : {"left", "right"}) {
        const std::string prefix = std::string("/user/hand/") + h + "/";
        if (path.rfind(prefix, 0) == 0) {
            hand = h;
            rest = path.substr(prefix.size() - 1);
        }
    }
    if (hand.empty()) {
        return "not a /user/hand/left|right path";
    }
    const auto has = [&](const std::string& component) {
        return std::ranges::any_of(spec.components, [&](const auto& c) {
            return c.second == component && (c.first == "both" || c.first == hand);
        });
    };
    const bool scalar = type == XR_ACTION_TYPE_BOOLEAN_INPUT || type == XR_ACTION_TYPE_FLOAT_INPUT;
    if (const size_t dpad = rest.find("/dpad_"); dpad != std::string::npos) {
        if (!g_enabled_exts.contains("XR_EXT_dpad_binding") ||
            !g_enabled_exts.contains("XR_KHR_binding_modification")) {
            return "a d-pad component without XR_EXT_dpad_binding + XR_KHR_binding_modification";
        }
        const std::string parent = rest.substr(0, dpad);
        const std::string dir = rest.substr(dpad + 6);
        const bool trackpad = parent == "/input/trackpad";
        if (!trackpad && parent != "/input/thumbstick") {
            return "a d-pad on neither a trackpad nor a thumbstick";
        }
        if (dir != "up" && dir != "down" && dir != "left" && dir != "right" &&
            !(trackpad && dir == "center")) {
            return "no such d-pad component";
        }
        if (!has(parent + "/x")) {
            return "the profile has no " + parent + " on this hand";
        }
        return scalar ? "" : "a d-pad component bound to a non-button action";
    }
    if (has(rest)) {
        const bool pose = rest.size() > 5 && rest.substr(rest.size() - 5) == "/pose";
        const bool haptic = rest == "/output/haptic";
        if (type == XR_ACTION_TYPE_POSE_INPUT) {
            return pose ? "" : "a pose action on a non-pose component";
        }
        if (type == XR_ACTION_TYPE_VIBRATION_OUTPUT) {
            return haptic ? "" : "a vibration action on a non-haptic component";
        }
        if (type == XR_ACTION_TYPE_VECTOR2F_INPUT) {
            return "a 2D action on a single component (bind the identifier)";
        }
        return pose || haptic ? "a button / value action on a pose or output" : "";
    }
    if (has(rest + "/x") && has(rest + "/y")) {
        return type == XR_ACTION_TYPE_VECTOR2F_INPUT || scalar ? ""
                                                                : "a pose / vibration on a stick";
    }
    if (scalar && std::ranges::any_of(spec.components, [&](const auto& c) {
            return c.second.rfind(rest + "/", 0) == 0 && (c.first == "both" || c.first == hand);
        })) {
        return ""; // an identifier without its component (the runtime picks one)
    }
    return "not a component of the profile on the " + hand + " hand";
}
// The controllers in use (MOCKXR_PROFILE, default oculus/touch_controller) and the profiles the
// application's suggestions were accepted for.
XrPath CurrentProfilePath() {
    static const XrPath path = [] {
        // "/interaction_profiles/valve/index_controller" or just "valve/index_controller" (a Git
        // Bash shell turns a leading "/" into a Windows path: the profile part is kept).
        const char* v = std::getenv("MOCKXR_PROFILE");
        std::string name = v != nullptr && *v != '\0' ? v : "oculus/touch_controller";
        if (const size_t at = name.find("/interaction_profiles/"); at != std::string::npos) {
            name = name.substr(at);
        } else {
            name = "/interaction_profiles/" + name;
        }
        XrPath p{};
        StringToPath(XR_NULL_HANDLE, name.c_str(), &p);
        if (v != nullptr && *v != '\0') {
            Log("MOCKXR_PROFILE: the controllers in use are %s", name.c_str());
        }
        return p;
    }();
    return path;
}
std::set<XrPath> g_accepted_profiles;
// The suggested bindings per action and interaction profile (xrEnumerateBoundSourcesForAction
// answers with the profile xrGetCurrentInteractionProfile reports: MOCKXR_PROFILE's).
std::map<XrAction, std::map<XrPath, std::vector<XrPath>>> g_bindings;
// Whether the action has a binding in the controllers' profile (MOCKXR_PROFILE).
bool BoundNow(XrAction action) {
    const auto it = g_bindings.find(action);
    if (it == g_bindings.end()) {
        return false;
    }
    const auto p = it->second.find(CurrentProfilePath());
    return p != it->second.end() && !p->second.empty();
}
XrResult XRAPI_CALL SuggestBindings(XrInstance, const XrInteractionProfileSuggestedBinding* s) {
    CurrentProfilePath(); // (resolved here, outside any lock)
    const std::string profile = g_path_names[s->interactionProfile];
    XrResult result = XR_SUCCESS;
    const auto& specs = ProfileSpecs();
    const auto spec = specs.find(profile);
    if (spec == specs.end()) {
        Log("ERROR: xrSuggestInteractionProfileBindings: %s is not an interaction profile "
            "(XR_ERROR_PATH_UNSUPPORTED)",
            profile.c_str());
        result = XR_ERROR_PATH_UNSUPPORTED;
    } else if (!spec->second.extension.empty() &&
               !g_enabled_exts.contains(spec->second.extension)) {
        Log("ERROR: xrSuggestInteractionProfileBindings: %s needs %s, not enabled "
            "(XR_ERROR_PATH_UNSUPPORTED)",
            profile.c_str(), spec->second.extension.c_str());
        result = XR_ERROR_PATH_UNSUPPORTED;
    } else {
        int invalid = 0;
        for (uint32_t i = 0; i < s->countSuggestedBindings; ++i) {
            const XrActionSuggestedBinding& b = s->suggestedBindings[i];
            const std::string& path = g_path_names[b.binding];
            const auto action = g_actions.find(b.action);
            std::string why = path.empty()              ? "an unknown path"
                              : action == g_actions.end() ? "an unknown action"
                                  : BindingProblem(spec->second, action->second.type, path);
            if (!why.empty()) {
                ++invalid;
                Log("ERROR: xrSuggestInteractionProfileBindings %s: binding %u (%s -> %s): %s",
                    profile.c_str(), i,
                    action != g_actions.end() ? action->second.name.c_str() : "?", path.c_str(),
                    why.c_str());
            }
        }
        if (invalid > 0) {
            result = XR_ERROR_PATH_UNSUPPORTED;
        }
    }
    // MOCKXR_REFUSE=<text>[,<text>...]: a runtime refusing what it could take (robustness: the
    // other profiles must stay, a refused d-pad map must fall back): a suggestion whose profile
    // or any binding path contains one of the texts gets XR_ERROR_PATH_UNSUPPORTED.
    if (const char* refuse = std::getenv("MOCKXR_REFUSE"); result == XR_SUCCESS && refuse) {
        std::string all = refuse;
        for (size_t at = 0; at <= all.size() && result == XR_SUCCESS;) {
            const size_t comma = std::min(all.find(',', at), all.size());
            const std::string token = all.substr(at, comma - at);
            at = comma + 1;
            if (token.empty()) {
                continue;
            }
            bool hit = profile.find(token) != std::string::npos;
            for (uint32_t i = 0; i < s->countSuggestedBindings && !hit; ++i) {
                hit = g_path_names[s->suggestedBindings[i].binding].find(token) != std::string::npos;
            }
            if (hit) {
                Log("MOCKXR_REFUSE: %s refused (contains '%s')", profile.c_str(), token.c_str());
                result = XR_ERROR_PATH_UNSUPPORTED;
            }
        }
    }
    Log("suggested %u bindings for %s -> %s", s->countSuggestedBindings, profile.c_str(),
        result == XR_SUCCESS ? "ok" : "XR_ERROR_PATH_UNSUPPORTED (not taken)");
    if (result != XR_SUCCESS) {
        return result; // a runtime keeps none of a refused suggestion
    }
    for (auto& [action, profiles] : g_bindings) {
        profiles.erase(s->interactionProfile); // a later suggestion replaces the profile's
    }
    static const bool log_each = std::getenv("MOCKXR_BIND_LOG") != nullptr;
    for (uint32_t i = 0; i < s->countSuggestedBindings; ++i) {
        const XrActionSuggestedBinding& b = s->suggestedBindings[i];
        g_bindings[b.action][s->interactionProfile].push_back(b.binding);
        if (log_each) {
            Log("bind %s %s %s", profile.c_str(), g_actions[b.action].name.c_str(),
                g_path_names[b.binding].c_str());
        }
    }
    g_accepted_profiles.insert(s->interactionProfile);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL EnumerateBoundSources(XrSession, const XrBoundSourcesForActionEnumerateInfo* info,
                                          uint32_t capacity, uint32_t* count, XrPath* sources) {
    if (info == nullptr || info->type != XR_TYPE_BOUND_SOURCES_FOR_ACTION_ENUMERATE_INFO ||
        count == nullptr || g_actions.find(info->action) == g_actions.end()) {
        Log("ERROR: xrEnumerateBoundSourcesForAction: invalid info, count or action");
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const XrPath profile = CurrentProfilePath();
    std::vector<XrPath> bound;
    if (auto it = g_bindings.find(info->action); it != g_bindings.end()) {
        if (auto p = it->second.find(profile); p != it->second.end()) {
            bound = p->second;
        }
    }
    *count = static_cast<uint32_t>(bound.size());
    if (capacity == 0) {
        return XR_SUCCESS;
    }
    if (capacity < bound.size() || sources == nullptr) {
        return XR_ERROR_SIZE_INSUFFICIENT;
    }
    std::copy(bound.begin(), bound.end(), sources);
    return XR_SUCCESS;
}
XrResult XRAPI_CALL AttachActionSets(XrSession, const XrSessionActionSetsAttachInfo*) {
    return XR_SUCCESS;
}
void ExpireHapticPulses(); // (below)
XrResult XRAPI_CALL SyncActions(XrSession, const XrActionsSyncInfo*) {
    PollInputFile();
    ExpireHapticPulses();
    return XR_SUCCESS;
}
// MOCKXR_STEREO_SWITCH: the phase (seconds) within the switch period, or -1 when off / not yet.
double StereoSwitchPhase() {
    static const double period = [] {
        const char* v = std::getenv("MOCKXR_STEREO_SWITCH");
        return v ? std::max(std::atof(v), 2.0) : 0.0;
    }();
    if (period <= 0.0 || !g_session || g_session->projection_frames < 600) {
        return -1.0;
    }
    return std::fmod(static_cast<double>(NowNs()) * 1e-9, period);
}

XrResult XRAPI_CALL GetBool(XrSession, const XrActionStateGetInfo* info,
                            XrActionStateBoolean* s) {
    s->isActive = XR_TRUE;
    s->currentState = XR_FALSE;
    if (!BoundNow(info->action)) {
        s->isActive = XR_FALSE; // no binding on the controllers in use (MOCKXR_PROFILE)
        return XR_SUCCESS;
    }
    if(BenchmarkStartupEnabled() && g_actions[info->action].name=="button_a") {
        std::lock_guard lock(g_startup_mutex);
        s->currentState=g_startup_navigation.Down(double(NowNs())*1e-9) ? XR_TRUE : XR_FALSE;
        return XR_SUCCESS;
    }
    const double ph = StereoSwitchPhase();
    if (ph >= 0.0 && ph < 0.8 && g_actions[info->action].name == "menu") {
        s->currentState = XR_TRUE;
    }
    float x{}, y{};
    if (ScriptedInput(g_actions[info->action].name, x, y) ||
        ScriptedButton(g_actions[info->action].name)) {
        s->currentState = XR_TRUE;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL GetFloat(XrSession, const XrActionStateGetInfo* info, XrActionStateFloat* s) {
    s->isActive = XR_TRUE;
    s->currentState = 0.0f;
    if (!BoundNow(info->action)) {
        s->isActive = XR_FALSE; // no binding on the controllers in use (MOCKXR_PROFILE)
        return XR_SUCCESS;
    }
    const double ph = std::fmod(static_cast<double>(NowNs()) * 1e-9, 6.0);
    if (std::getenv("MOCKXR_INPUT") && PresetGrip() && g_actions[info->action].name == "grip_right" &&
        ph >= 1.0 && ph < 3.0) {
        s->currentState = 1.0f; // weapon drawn for the scripted swing at 2.0 s (right_grip=draw)
    }
    BP::TimelinePoint point{};
    if (PresetGrip() && g_actions[info->action].name == "grip_right" &&
        HandsPoint(g_display_time.load(), point) && point.preset->grip_right) {
        s->currentState = 1.0f; // MOCKXR_HANDS: this segment holds the weapon drawn
    }
    float x{}, y{};
    if (ScriptedInput(g_actions[info->action].name, x, y) ||
        ScriptedButton(g_actions[info->action].name, &x, &y)) {
        s->currentState = x;
    }
    if (g_actions[info->action].name == "grip_left" &&
        ThrowGrip(static_cast<double>(NowNs()) * 1e-9)) {
        s->currentState = 1.0f; // a scripted throw holds it until its release
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL GetVector(XrSession, const XrActionStateGetInfo* info,
                              XrActionStateVector2f* s) {
    s->isActive = XR_TRUE;
    s->currentState = {0.0f, 0.0f};
    if (!BoundNow(info->action)) {
        s->isActive = XR_FALSE; // no binding on the controllers in use (MOCKXR_PROFILE)
        return XR_SUCCESS;
    }
    const double ph = std::fmod(static_cast<double>(NowNs()) * 1e-9, 6.0);
    if (std::getenv("MOCKXR_INPUT") && g_actions[info->action].name == "turn" && ph >= 4.0 &&
        ph < 4.3) {
        s->currentState = {1.0f, 0.0f};
    }
    const double sw = StereoSwitchPhase();
    if (sw >= 0.25 && sw < 0.55 && g_actions[info->action].name == "move") {
        s->currentState = {1.0f, 0.0f}; // Menu + left stick right: next stereo mode
    }
    float x{}, y{};
    if (ScriptedInput(g_actions[info->action].name, x, y) ||
        ScriptedButton(g_actions[info->action].name, &x, &y)) {
        s->currentState = {x, y};
    }
    return XR_SUCCESS;
}
// Haptics, checked like a runtime: a vibration-output action (else XR_ERROR_ACTION_TYPE_MISMATCH),
// an XrHapticVibration with 0 <= amplitude <= 1 and a duration > 0 or XR_MIN_HAPTIC_DURATION
// (ERROR lines), the session FOCUSED (else XR_SESSION_NOT_FOCUSED and nothing plays). Each call is
// logged as `haptic <action> amp <a> dur <ms>ms t=<s>` (the first MOCKXR_HAPTIC_LOG), and each
// vibration as it ends as `haptic pulse <action> t=<start s> len=<ms> peak=<a> calls=<n>
// end=stop|duration` (a call while one plays continues it, replacing its amplitude and end; first
// 5000): what the controller would play. MOCKXR_HAPTIC_FAIL=<XrResult>[,left|right]: every
// xrApplyHapticFeedback (of that hand only) returns that code (e.g. -1, or 8 =
// XR_SESSION_NOT_FOCUSED) and plays nothing.
struct HapticPulse {
    bool on{false};
    int64_t start{0};
    int64_t until{0};
    float peak{0.0f};
    int calls{0};
};
std::mutex g_haptic_mutex;
std::map<XrAction, HapticPulse> g_pulses;
void EndHapticPulse(XrAction action, HapticPulse& p, int64_t end, const char* how) {
    static int logged = 0;
    if (p.on && logged < 5000) {
        ++logged;
        Log("haptic pulse %s t=%.3f len=%.1fms peak=%.2f calls=%d end=%s",
            g_actions[action].name.c_str(), static_cast<double>(p.start) * 1e-9,
            static_cast<double>(end - p.start) * 1e-6, p.peak, p.calls, how);
    }
    p = {};
}
void ExpireHapticPulses() {
    std::lock_guard lk{g_haptic_mutex};
    const int64_t now = NowNs();
    for (auto& [action, p] : g_pulses) {
        if (p.on && now >= p.until) {
            EndHapticPulse(action, p, p.until, "duration");
        }
    }
}
// The vibration action of a haptic call, or an error result (logged).
XrResult HapticAction(const char* call, const XrHapticActionInfo* info) {
    if (info == nullptr || info->type != XR_TYPE_HAPTIC_ACTION_INFO ||
        g_actions.find(info->action) == g_actions.end()) {
        Log("ERROR: %s: invalid XrHapticActionInfo or action", call);
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (g_actions[info->action].type != XR_ACTION_TYPE_VIBRATION_OUTPUT) {
        Log("ERROR: %s on '%s': not a vibration output (XR_ERROR_ACTION_TYPE_MISMATCH)", call,
            g_actions[info->action].name.c_str());
        return XR_ERROR_ACTION_TYPE_MISMATCH;
    }
    if (g_session == nullptr || g_session->state != XR_SESSION_STATE_FOCUSED) {
        return XR_SESSION_NOT_FOCUSED;
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL ApplyHaptic(XrSession, const XrHapticActionInfo* info,
                                const XrHapticBaseHeader* base) {
    static int logged = 0;
    // MOCKXR_HAPTIC_LOG=<n>: log the first n haptic calls (default 40; 400 with the switch).
    static const int limit = std::getenv("MOCKXR_HAPTIC_LOG") != nullptr
                                 ? std::atoi(std::getenv("MOCKXR_HAPTIC_LOG"))
                             : std::getenv("MOCKXR_STEREO_SWITCH") ? 400
                                                                   : 40;
    static const std::pair<XrResult, std::string> fail = [] {
        const char* v = std::getenv("MOCKXR_HAPTIC_FAIL");
        if (v == nullptr || *v == '\0') {
            return std::pair<XrResult, std::string>{XR_SUCCESS, ""};
        }
        const char* comma = std::strchr(v, ',');
        Log("MOCKXR haptic failures: xrApplyHapticFeedback returns %d (%s)", std::atoi(v),
            comma ? comma + 1 : "both hands");
        return std::pair<XrResult, std::string>{static_cast<XrResult>(std::atoi(v)),
                                                comma ? comma + 1 : ""};
    }();
    XrResult result = HapticAction("xrApplyHapticFeedback", info);
    if (XR_FAILED(result)) {
        return result;
    }
    if (base == nullptr || base->type != XR_TYPE_HAPTIC_VIBRATION) {
        Log("ERROR: xrApplyHapticFeedback: not an XrHapticVibration");
        return XR_ERROR_VALIDATION_FAILURE;
    }
    const auto* v = reinterpret_cast<const XrHapticVibration*>(base);
    const std::string& name = g_actions[info->action].name;
    if (!(v->amplitude >= 0.0f && v->amplitude <= 1.0f) ||
        (v->duration <= 0 && v->duration != XR_MIN_HAPTIC_DURATION)) {
        Log("ERROR: xrApplyHapticFeedback on %s: amplitude %.3f duration %lld ns", name.c_str(),
            v->amplitude, static_cast<long long>(v->duration));
        return XR_ERROR_VALIDATION_FAILURE;
    }
    if (result == XR_SUCCESS && fail.first != XR_SUCCESS &&
        (fail.second.empty() || name.find(fail.second) != std::string::npos)) {
        result = fail.first;
    }
    const int64_t now = NowNs();
    if (logged < limit) {
        ++logged;
        if (result == XR_SUCCESS) {
            Log("haptic %s amp %.2f dur %.1fms t=%.3f", name.c_str(), v->amplitude,
                v->duration * 1e-6, static_cast<double>(now) * 1e-9);
        } else {
            Log("haptic %s amp %.2f dur %.1fms t=%.3f -> %d (nothing played)", name.c_str(),
                v->amplitude, v->duration * 1e-6, static_cast<double>(now) * 1e-9, result);
        }
    }
    if (result != XR_SUCCESS) {
        return result;
    }
    if (!BoundNow(info->action)) {
        // Like a runtime: the call succeeds, but no controller output is bound to the action.
        static int unbound_logged = 0;
        if (unbound_logged++ < 20) {
            Log("haptic %s amp %.2f t=%.3f: not bound in %s, nothing played", name.c_str(),
                v->amplitude, static_cast<double>(now) * 1e-9,
                g_path_names[CurrentProfilePath()].c_str());
        }
        return XR_SUCCESS;
    }
    std::lock_guard lk{g_haptic_mutex};
    auto& p = g_pulses[info->action];
    if (p.on && now >= p.until) {
        EndHapticPulse(info->action, p, p.until, "duration");
    }
    if (!p.on) {
        p.on = true;
        p.start = now;
    }
    p.until = now + (v->duration == XR_MIN_HAPTIC_DURATION ? 5'000'000 : v->duration);
    p.peak = std::max(p.peak, v->amplitude);
    ++p.calls;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL StopHaptic(XrSession, const XrHapticActionInfo* info) {
    const XrResult result = HapticAction("xrStopHapticFeedback", info);
    if (result != XR_SUCCESS) {
        return result;
    }
    std::lock_guard lk{g_haptic_mutex};
    auto& p = g_pulses[info->action];
    const int64_t now = NowNs();
    if (p.on) {
        EndHapticPulse(info->action, p, std::min(now, p.until), now < p.until ? "stop" : "duration");
    }
    return XR_SUCCESS;
}
// The controllers in use (MOCKXR_PROFILE), or none when the application suggested no accepted
// bindings for them (a runtime cannot use a controller it has no bindings for).
XrResult XRAPI_CALL GetCurrentProfile(XrSession, XrPath, XrInteractionProfileState* state) {
    const XrPath current = CurrentProfilePath();
    state->interactionProfile = g_accepted_profiles.contains(current) ? current : XR_NULL_PATH;
    static bool logged = false;
    if (state->interactionProfile == XR_NULL_PATH && !logged) {
        logged = true;
        Log("MOCKXR profile %s: no accepted bindings, reported as none",
            g_path_names[current].c_str());
    }
    return XR_SUCCESS;
}
XrResult XRAPI_CALL GetPose(XrSession, const XrActionStateGetInfo* info, XrActionStatePose* s) {
    s->isActive = info != nullptr && BoundNow(info->action) ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}
XrResult XRAPI_CALL ResultToString(XrInstance, XrResult r, char buf[XR_MAX_RESULT_STRING_SIZE]) {
    // The names a log is read for (the rest as XR_RESULT_<n>).
    const char* name = r == XR_SUCCESS                    ? "XR_SUCCESS"
                       : r == XR_SESSION_NOT_FOCUSED      ? "XR_SESSION_NOT_FOCUSED"
                       : r == XR_SESSION_LOSS_PENDING     ? "XR_SESSION_LOSS_PENDING"
                       : r == XR_ERROR_VALIDATION_FAILURE ? "XR_ERROR_VALIDATION_FAILURE"
                       : r == XR_ERROR_RUNTIME_FAILURE    ? "XR_ERROR_RUNTIME_FAILURE"
                       : r == XR_ERROR_HANDLE_INVALID     ? "XR_ERROR_HANDLE_INVALID"
                       : r == XR_ERROR_ACTION_TYPE_MISMATCH ? "XR_ERROR_ACTION_TYPE_MISMATCH"
                       : r == XR_ERROR_SESSION_NOT_RUNNING  ? "XR_ERROR_SESSION_NOT_RUNNING"
                       : r == XR_ERROR_PATH_UNSUPPORTED     ? "XR_ERROR_PATH_UNSUPPORTED"
                                                            : nullptr;
    if (name != nullptr) {
        std::snprintf(buf, XR_MAX_RESULT_STRING_SIZE, "%s", name);
    } else {
        std::snprintf(buf, XR_MAX_RESULT_STRING_SIZE, "XR_RESULT_%d", r);
    }
    return XR_SUCCESS;
}

XrResult XRAPI_CALL GetInstanceProcAddr(XrInstance, const char* name, PFN_xrVoidFunction* fn) {
#define F(n, f)                                                                                    \
    if (std::strcmp(name, n) == 0) {                                                               \
        *fn = reinterpret_cast<PFN_xrVoidFunction>(f);                                             \
        return XR_SUCCESS;                                                                         \
    }
    F("xrGetInstanceProcAddr", GetInstanceProcAddr)
    F("xrEnumerateInstanceExtensionProperties", EnumerateInstanceExtensionProperties)
    F("xrCreateInstance", CreateInstance)
    F("xrDestroyInstance", DestroyInstance)
    F("xrGetInstanceProperties", GetInstanceProperties)
    F("xrGetSystem", GetSystem)
    F("xrGetSystemProperties", GetSystemProperties)
    F("xrGetVulkanInstanceExtensionsKHR", GetVulkanInstanceExtensions)
    F("xrGetVulkanDeviceExtensionsKHR", GetVulkanDeviceExtensions)
    F("xrGetVulkanGraphicsDeviceKHR", GetVulkanGraphicsDevice)
    F("xrGetVulkanGraphicsRequirementsKHR", GetVulkanGraphicsRequirements)
    F("xrCreateSession", CreateSession)
    F("xrDestroySession", DestroySession)
    F("xrBeginSession", BeginSession)
    F("xrEndSession", EndSession)
    F("xrRequestExitSession", RequestExitSession)
    F("xrPollEvent", PollEvent)
    F("xrCreateReferenceSpace", CreateReferenceSpace)
    F("xrEnumerateReferenceSpaces", EnumerateReferenceSpaces)
    F("xrCreateActionSpace", CreateActionSpace)
    F("xrDestroySpace", DestroySpace)
    F("xrLocateSpace", LocateSpace)
    F("xrEnumerateEnvironmentBlendModes", EnumerateEnvironmentBlendModes)
    F("xrEnumerateViewConfigurationViews", EnumerateViewConfigurationViews)
    F("xrEnumerateSwapchainFormats", EnumerateSwapchainFormats)
    F("xrCreateSwapchain", CreateSwapchain)
    F("xrDestroySwapchain", DestroySwapchain)
    F("xrEnumerateSwapchainImages", EnumerateSwapchainImages)
    F("xrAcquireSwapchainImage", AcquireSwapchainImage)
    F("xrWaitSwapchainImage", WaitSwapchainImage)
    F("xrReleaseSwapchainImage", ReleaseSwapchainImage)
    F("xrWaitFrame", WaitFrame)
    F("xrBeginFrame", BeginFrame)
    F("xrEndFrame", EndFrame)
    F("xrLocateViews", LocateViews)
    F("xrGetViewConfigurationProperties", GetViewConfigurationProperties)
    F("xrStringToPath", StringToPath)
    F("xrPathToString", PathToString)
    F("xrCreateActionSet", CreateActionSet)
    F("xrDestroyActionSet", DestroyActionSet)
    F("xrCreateAction", CreateAction)
    F("xrDestroyAction", DestroyAction)
    F("xrSuggestInteractionProfileBindings", SuggestBindings)
    F("xrAttachSessionActionSets", AttachActionSets)
    F("xrSyncActions", SyncActions)
    F("xrApplyHapticFeedback", ApplyHaptic)
    F("xrStopHapticFeedback", StopHaptic)
    F("xrEnumerateBoundSourcesForAction", EnumerateBoundSources)
    F("xrGetCurrentInteractionProfile", GetCurrentProfile)
    F("xrGetActionStateBoolean", GetBool)
    F("xrGetActionStateFloat", GetFloat)
    F("xrGetActionStateVector2f", GetVector)
    F("xrGetActionStatePose", GetPose)
    F("xrResultToString", ResultToString)
    F("xrEnumerateDisplayRefreshRatesFB", EnumerateDisplayRefreshRates)
    F("xrGetDisplayRefreshRateFB", GetDisplayRefreshRate)
    F("xrRequestDisplayRefreshRateFB", RequestDisplayRefreshRate)
#undef F
    Log("unsupported function requested: %s", name);
    *fn = nullptr;
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}

} // namespace

EXPORT XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo* info,
                                                             XrNegotiateRuntimeRequest* request) {
    request->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    request->runtimeApiVersion = XR_CURRENT_API_VERSION;
    request->getInstanceProcAddr = GetInstanceProcAddr;
    (void)info;
    return XR_SUCCESS;
}
