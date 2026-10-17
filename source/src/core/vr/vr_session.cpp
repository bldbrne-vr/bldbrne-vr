// SPDX-License-Identifier: GPL-2.0-or-later

#include "core/vr/lever_settings.h"
#include "core/vr/y_menu.h"
#include "core/vr/lamp_channel.h"
#include <algorithm>
#include <atomic>
#include "core/vr/color_settings.h"
#include <bit>
#include <cctype>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/vr/audio_settings.h"
#include "core/vr/blade_sounds.h"
#include "core/vr/body_settings.h"
#include "core/vr/camera_gap.h"
#include "core/vr/desktop_mirror.h"
#include "core/vr/dlss_model.h"
#include "core/vr/evasion.h"
#include "core/vr/eye_fov_cache.h"
#include "core/vr/graphics_settings.h"
#include "core/vr/gun_settings.h"
#include "core/vr/head_room.h"
#include "core/vr/body_channel.h"
#include "core/vr/roomscale_walk.h"
#include "core/vr/interaction_roomscale.h"
#include "core/vr/live_tune.h"
#include "core/vr/melee_settings.h"
#include "core/vr/bow.h"
#include "core/vr/torch_lift.h"
#include "core/vr/trick_weapons.h"
#include "core/vr/player_body.h"
#include "core/vr/pose_gestures.h"
#include "core/vr/quick_settings.h"
#include "core/vr/render_steady.h"
#include "core/vr/render_taa.h"
#include "core/vr/render_tilt.h"
#include "core/vr/seqlock.h"
#include "core/vr/transform_settings.h"
#include "core/vr/two_hand_settings.h"
#include "core/vr/vr_ini.h"
#include "core/vr/vr_session.h"
#include "core/vr/whip_settings.h"
#include "input/controller.h"
#include "video_core/fast_render.h"

namespace VR {

namespace {

constexpr float Pi = 3.14159265358979323846f;
constexpr float DegToRad = Pi / 180.0f;

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

float WrapAngle(float a) {
    while (a > Pi) {
        a -= 2.0f * Pi;
    }
    while (a < -Pi) {
        a += 2.0f * Pi;
    }
    return a;
}

// Stereo mode as one word: bit 8 = stereo on, low byte = stereo_mode.
constexpr u32 PackStereo(bool stereo, s32 mode) {
    return (stereo ? 0x100u : 0u) | (static_cast<u32>(mode) & 0xFFu);
}

StereoConfig UnpackStereo(u32 word) {
    return {(word & 0x100u) != 0, static_cast<s32>(static_cast<s8>(word & 0xFFu))};
}

// Live modes of the in-VR switch: 0 = mono, 1 = AER (stereo_mode 1), 2 = DIBR (stereo_mode 2).
constexpr s32 LiveModeCount = 3;

s32 LiveMode(StereoConfig config) {
    if (!config.stereo) {
        return 0;
    }
    return config.mode == 1 ? 1 : config.mode == 2 ? 2 : 0;
}

struct State {
    Settings settings{};
    std::atomic_bool enabled{false};
    std::atomic_bool simulated{false};
    std::atomic<u32> display_mode{static_cast<u32>(DisplayMode::Auto)};
    // The stereo mode in effect (game thread writes, any thread reads) and the one the live
    // switch asked for (input thread writes); the camera tick applies the request between pairs.
    std::atomic<u32> stereo_word{PackStereo(true, 2)};
    std::atomic<u32> stereo_request{PackStereo(true, 2)};

    std::mutex head_mutex;
    HeadState head{};

    std::atomic<float> pending_turn{0.0f};

    // Game thread.
    bool driving{false};
    s32 tick_eye{1};       // last stereo_mode 1 eye; 1 = the next update starts a pair
    s32 shift_tick{1};     // last stereo_mode 0 frame parity; 1 = the next update starts a pair
    s64 pair_tick_ns{0};   // last first-eye update
    u64 pair_serial{0};    // stereo_mode 1 pairs started
    HeadState pair_head{}; // head sample of the pair's first eye (the second eye renders with it)
    bool vfov_raised_logged{false};
    // render_pitch_bias_deg: the bias of the last driven update (radians) and how many times it
    // switched on or off (logged, the first few).
    float tilt_bias{0.0f};
    u32 tilt_switches{0};
    // render_steady (core/vr/render_steady.h): the held render pose, and its statistics for the
    // periodic log line (camera updates held / dragged / followed, the render-vs-head offset).
    Steady::State steady{};
    bool steady_on{false};
    u32 steady_switches{0};
    u64 steady_counts[4]{};
    double steady_off_deg_sum{0.0};
    double steady_off_mm_sum{0.0};
    float steady_off_deg_max{0.0f};
    float steady_off_mm_max{0.0f};
    std::atomic_bool first_person_detected{false};
    bool pad_turn_armed{true};
    u32 turn_serial{0};
    float orbit_pitch{0.0f};
    std::atomic_bool game_owns{true};
    float base_yaw{};
    std::mutex roomscale_mutex;
    Interactions::RoomscaleController roomscale_walk{}; // camera caller only
    Interactions::Module* roomscale_module{};
    bool roomscale_fault{};
    Roomscale::Output roomscale_out{};      // under roomscale_mutex
    s64 roomscale_input_ns{}, roomscale_output_ns{};
    bool roomscale_allowed{}, roomscale_applied{};
    bool roomscale_logged_walking{}; // camera thread: transition diagnostics
    const char* roomscale_why{""};   // the presenter's reason it gives no stick (a literal)
    float roomscale_far_s{};         // camera thread: the neck out past the start, not walking
    s64 roomscale_why_ns{};          // ... the last such log
    Vec3 head_origin{};               // head position the 6DoF offset is measured from
    Vec3 neck_origin{};               // neck pivot at the same moment (physical evasion)
    std::atomic_bool recenter{false}; // recapture head_origin on the next tick
    // A gap in the camera updates (camera_stale_ms, core/vr/camera_gap.h), game thread: what the
    // last driven update left the game camera at, whether a gap interrupted driving (the next
    // takeover may resume), its length, and how many gaps were logged.
    CameraGap::Last gap_last{};
    bool gap_pending{false};
    s64 gap_ns{0};
    u32 gap_count{0};
    bool recenter_deferred{false};    // game thread: logged that a menu holds it back
    // Height (player_body.h), game thread: the floor's height in the space the view was last
    // carried into (shift_applied), from the newest head sample that had it (moved with a
    // recenter, dropped on a change without its pose).
    bool floor_known{false};
    float floor_y{0.0f};
    // As the player: the eye offset of the last tick that knew the floor (kept while a recenter's
    // unknown shift drops it for a sample or two, instead of falling back to the hunter's).
    bool player_offset_known{false};
    float player_offset{0.0f};
    // The eye offset the newest tick used (HeightEyeOffsetM; any thread) and what was logged last.
    std::atomic<float> eye_offset_now{0.0f};
    std::atomic<float> body_offset_now{0.0f}; // HeightBodyOffsetM (any thread)
    std::atomic<bool> eye_offset_set{false};
    float eye_offset_logged{-1000.0f};
    int height_mode_logged{-1}; // 0 hunter, 1 player, 2 player without a floor, 3 player seated
    int height_adjust_logged{-1000};
    // Tracking space changes (ShiftTrackingSpace, presenter frame thread; shift_mutex): the space
    // the published heads are located in and the last changes by serial (SpaceShiftBetween: the
    // camera tick carries the view from the space it last saw into its head sample's, several
    // changes composed; frames in flight and held hands are moved the same way).
    std::mutex shift_mutex;
    std::atomic<u32> space_serial{0};
    Space::ShiftLog<32> shift_log{};
    // Game thread: the space the view was last carried into; for the presenter its serial and the
    // first camera record placed in it.
    u32 shift_applied{0};
    std::atomic<u32> shift_applied_serial{0};
    std::atomic<u64> shift_first_record{0};
    bool shift_log_pending{false}; // log the first driven view after the shift
    float last_view_yaw{0.0f};     // the last driven tick's view yaw (that log)
    std::atomic<s64> last_tick_ns{0};
    std::atomic<s64> first_gameplay_tick_ns{0};
    std::array<Fov, 2> last_valid_eye_fov{}; // camera thread only
    std::atomic<float> frame_seconds{1.0f / 60.0f};
    std::atomic_bool locked_on{false};
    std::atomic_bool game_menu{false};
    // The interactive visceral's hold is installed (SetVisceralFirstPerson): the hunter's throws
    // play in first person.
    std::atomic_bool visceral_first_person{false};
    std::atomic<s64> game_menu_ns{0};
    std::atomic<s64> game_menu_open_ns{0}; // the last sample with a menu open
    std::atomic<float> game_menu_zoom{1.0f};
    std::atomic<u8> game_menu_class{0}; // UiClass of the open menu
    std::atomic<float> game_menu_pitch{0.0f}; // its canvas's pitch offset (degrees)
    std::atomic<s32> game_menu_grid{-2};      // the Options grid's cursor cell (-2: not the grid)
    // UI layer: the presenter can show it; a menu whose frames have no composite (the UI layer
    // cannot take it) goes to the frozen panel until it closes.
    std::atomic_bool ui_layer_available{false};
    std::atomic_bool ui_marker_fallback{false};
    u32 ui_marker_missing{
        0}; // GPU thread: immersive UI-layer frames in a row without the composite
    std::atomic_bool npc_talking{false};
    std::atomic<s64> npc_subtitle_ns{0};     // latest visible subtitle (includes fade tail)
    std::atomic<s64> npc_talk_ns{0};         // the last sample
    std::atomic<s64> npc_talk_since_ns{0};   // the start of the current talking stretch
    std::atomic<s64> npc_line_ns{0};         // the last sample with a line playing (not stuck)
    std::atomic<s64> npc_conversation_ns{0}; // ... or with that line's talk UI still up
    std::atomic<s64> plain_hud_ns{0};        // the last PublishPlainHud sample
    std::atomic<s64> item_pickup_ns{0};
    std::atomic<s64> action_prompt_ns{0};   // last sample with an action prompt up
    std::atomic<s64> busy_ui_ns{0};          // ... the last one with more than the HUD up
    // The shown menu panel's yaw and pitch, whether it is set and the tracking space it is in
    // (menu_aim_mutex: the four together, so a tick never takes an aim from another space).
    struct MenuAim {
        bool valid{false};
        float yaw{0.0f};
        float pitch{0.0f};
        u32 space{0};
    };
    std::mutex menu_aim_mutex;
    MenuAim menu_aim{};
    // Game thread: the menu panel froze the view at this head sample (menu_frozen).
    bool menu_frozen{false};
    HeadState menu_head{};
    std::atomic<u32> rumble{0}; // small << 8 | large
    std::atomic<float> current_vfov{0.0f};
    std::atomic<float> camera_aspect{0.0f}; // last render camera aspect read (0 = none yet)

    // Camera records (game thread writes, GPU thread reads).
    static constexpr size_t RecordCount = 64;
    std::mutex record_mutex;
    std::array<CameraRecord, RecordCount> records{};
    u64 next_record_id{1};
    std::string camera_thread; // name of the thread of the camera updates

    // Flip association (record_mutex): one entry per submitted flip that the GPU thread has not
    // reached yet, oldest first. The game builds the next frame while the GPU thread still
    // draws the last one, so the newest record at the flip is often already the next frame's.
    struct FlipEntry {
        u64 ticket;
        u64 record;
    };
    static constexpr size_t FlipQueueSize = 32;
    std::array<FlipEntry, FlipQueueSize> flip_queue{};
    size_t flip_head{0};
    size_t flip_count{0};
    u64 next_flip_ticket{1};
    std::string submit_thread; // name of the thread of the first flip submission
    // Statistics, logged every 600 flips.
    u32 flips{0};
    u32 flips_older{0};     // the queued record was older than the newest at the flip
    u64 flips_older_max{0}; // ... by this many records at most
    u32 flips_unmatched{0}; // no queued entry for the flip: the newest record was used
    u32 flips_dropped{0};   // entries discarded (never flipped, or the queue overflowed)
    u32 flips_redrawn{0};   // tagged with their first draw's update, not the flip's (older)
    size_t flip_depth_max{0};

    // GPU thread.
    s32 current_eye{0};
    float current_draw_shift{0.0f};
    float current_hud{0.0f};
    bool current_frame_immersive{false};
    bool current_frame_dibr{false}; // the frame being drawn is immersive stereo_mode 2
    // aa_mode=taa: the frame being drawn's jitter, the phase counter, the last resolve's render
    // size, whether this frame was resolved and how many jittered frames in a row were not.
    TaaJitter current_taa_jitter{};
    u32 taa_phase{0};
    u32 taa_width{0}, taa_height{0};
    bool taa_resolved_frame{false};
    u32 taa_unresolved{0};
    bool current_frame_menu{false}; // the frame being drawn goes to the menu panel
    s32 current_frame_ui{0};        // the UI layer of the frame being drawn (UiLayerFrameMode)
    u8 current_frame_ui_class{0};   // ... and the menu class while it is drawn
    u32 menu_held_frames{0};        // FrameTag::hold in a row
    bool hud_phase{false};
    // The camera update the frame being drawn was built with: DrawFrameRecord at its first draw
    // (StereoForDraw), 0 = no draw yet. The frame is tagged with it (OnGpuFrameBoundary).
    u64 frame_first_record{0};
    bool frame_drawn{false};
    u64 last_boundary_record{0};
    u32 boundaries_without_tick{0};
    s32 last_tag_eye{-1};
    u32 eye_repeats{0};
    u64 boundary_count{0};
    u64 frame_seq{0}; // FrameTag::seq
    u64 shift_pair{0};           // stereo_mode 0 pairs (a left frame and the right frame after it)
    bool shift_realigned{false}; // the last boundary repeated an eye to follow the updates
    u32 shift_realigns{0};       // ... how often since the last log line
    std::atomic_bool immersive_now{false};
    std::atomic<float> main_aspect{16.0f / 9.0f};

    // Instant injection hand-off (input thread <-> game thread). Request word:
    // sequence << 48 | request time in ms << 2 | phase (idle, pending, taken).
    std::atomic<u64> inject_request{0};
    std::atomic<u64> inject_awaited{0}; // the request word the input thread waits for
    std::atomic<u32> inject_result{0};  // sequence << 8 | InjectResult
    std::atomic<u32> inject_sequence{0};
    std::atomic_bool inject_available{false};
    std::atomic<s64> player_alive_ns{0}; // last main-player update seen by the hook
    u32 inject_dead_expiries{0};         // input thread
    std::atomic<u32> item_heals{0};      // NoteItemHeal (the heal vignette)
    std::atomic<u32> hits_taken{0};      // NoteHitTaken (the hit vignette)
    std::atomic<float> hit_fraction{0.0f}; // ... the last one's share of max HP
    std::atomic<u64> held_request{0};    // PublishHeldRequest (VR::Held::PackRequest)
    std::atomic<u64> vial_drain{0};      // PublishVialDrain (VR::Held::PackDrain)
    SeqLock<HeldStatus> held_status{};   // PublishHeldStatus (the main player's update)

    // Cinematic flat mode. The machine runs in the main-player update (site A: one update at a
    // time, usually on a job worker thread, not the loop thread that places the camera); the
    // phase is published as one word: start time in ms << 20 | the phase's fade in ms << 8 |
    // phase.
    Cinematic::Machine cinematic{};
    std::atomic<u64> cinematic_word{0};
    std::atomic_bool cinematic_suppress{false}; // Tab during a cinematic (escape hatch)
    std::atomic_bool resync_to_head{false};     // the next camera takeover returns from one
    Cinematic::Fader cinematic_fader{};         // presenter frame thread (CinematicBrightness)
    // When the presenter last brought first-person VR (the projection) up, -1 while the virtual
    // screen is shown (NoteCinematicLayer): the way back ends once VR has faded in.
    std::atomic<s64> cinematic_projection_ns{-1};
};

// FAST_CPU_LEAN_SCOPES: the state's address once it is constructed, a plain load per S() (both
// stages call it several times per draw); the function-local static's guard reads thread-local
// storage on every call. '0': it is never published and every call takes the guard, as before.
std::atomic<State*> g_state{};
SHAD_NO_INLINE State& FirstS() {
    static State state;
    if (FastRender::g_lean_scopes) {
        g_state.store(&state, std::memory_order_release);
    }
    return state;
}
State& S() {
    if (State* const state = g_state.load(std::memory_order_acquire)) [[likely]] {
        return *state;
    }
    return FirstS();
}

// The camera record a frame is tagged with (record_mutex held): `frame_record` is the frame's own
// camera update; pose_lag_frames moves it earlier. Falls back to the frame's own record, then to
// the newest, when the wanted one was already overwritten. The frame tag and the HUD depth both
// use this, so they always agree on the eye.
const CameraRecord& LaggedRecord(const State& st, u64 frame_record, u64 latest) {
    const u64 lag = static_cast<u64>(std::max(st.settings.pose_lag_frames, 0));
    const u64 wanted = frame_record > lag ? frame_record - lag : frame_record;
    const auto& candidate = st.records[wanted % State::RecordCount];
    const auto& own = st.records[frame_record % State::RecordCount];
    return candidate.id == wanted   ? candidate
           : own.id == frame_record ? own
                                    : st.records[latest % State::RecordCount];
}

// The camera record of the frame the GPU thread is drawing (record_mutex held): the oldest flip
// it has not reached yet; with none queued the frame's flip is not submitted yet and it is the
// newest camera update. The frame is tagged with the same record (pose_lag_frames included), so
// the HUD depth and the DIBR capture always agree with the tag.
thread_local const CameraRecord* draw_camera_override{};
const CameraRecord& FrameRecordForDrawsLocked(const State& st) {
    if (draw_camera_override)
        return *draw_camera_override;
    const u64 latest = st.next_record_id - 1;
    const u64 id = st.flip_count != 0 ? st.flip_queue[st.flip_head].record : latest;
    return LaggedRecord(st, id, latest);
}

// DrawFrameRecord (record_mutex held): FrameRecordForDrawsLocked, or, while the render-camera hook
// has not reported its projection yet (the game thread already placed the camera for its next
// frame), the newest update that has one. The presenter checks that its projection is the one of
// the frame's own update.
const CameraRecord& DrawFrameRecordLocked(const State& st) {
    if (draw_camera_override)
        return *draw_camera_override;
    const CameraRecord& record = FrameRecordForDrawsLocked(st);
    if (record.has_projection) {
        return record;
    }
    const u64 latest = st.next_record_id - 1;
    for (u64 back = 0; back < State::RecordCount && back < latest; ++back) {
        const auto& older = st.records[(latest - back) % State::RecordCount];
        if (older.id == latest - back && older.has_projection) {
            return older;
        }
    }
    return record;
}

constexpr u64 InjectIdle = 0;
constexpr u64 InjectPending = 1;
constexpr u64 InjectTaken = 2;
constexpr u64 InjectPhaseMask = 3;
constexpr u64 InjectTimeMask = (u64{1} << 46) - 1;
constexpr u64 InjectTimeoutMs = 300;
// Requests expired while the hook saw no player update for this long count as "hook dead".
constexpr s64 InjectHookDeadNs = 1'000'000'000;

u64 InjectNowMs() {
    return static_cast<u64>(NowNs() / 1'000'000) & InjectTimeMask;
}

u64 InjectAgeMs(u64 word) {
    return (InjectNowMs() - ((word >> 2) & InjectTimeMask)) & InjectTimeMask;
}

HeadState SimulatedHead() {
    const double t = S().settings.sim_motion ? static_cast<double>(NowNs()) * 1e-9 : 0.0;
    const float yaw = 0.6f * static_cast<float>(std::sin(t * 0.7));
    const float pitch = 0.25f * static_cast<float>(std::sin(t * 0.45));
    HeadState head{};
    head.valid = true;
    head.head.orientation = QuatFromYawPitch(yaw, pitch);
    head.head.position = {0.0f, 0.0f, 0.0f};
    const float h = 50.0f * DegToRad;
    const float v = 48.0f * DegToRad;
    head.eye_fov[0] = {-h, 0.8f * h, v, -v};
    head.eye_fov[1] = {-0.8f * h, h, v, -v};
    head.ipd = 0.064f;
    head.eye_pose[0].position = {-0.032f, 0.0f, 0.0f};
    head.eye_pose[1].position = {0.032f, 0.0f, 0.0f};
    return head;
}

// The vertical game FOV for the head's eye FOVs and the render aspect: both eyes plus the margin
// (each eye is cropped out of it). A fixed vfov_deg is used as given, except where the runtime
// always shows its own FOV (fovMutable false, Meta Link): a render narrower than the headset
// would be stretched over the eye there, so it is never narrower than the headset.
// `bias` (radians, + = the render camera looks lower than the head; render_pitch_bias_deg): the
// eyes as the pitched render sees them (core/vr/render_tilt.h CoverHalfVfov; 0 = as before).
float ComputeVfov(const HeadState& head, float aspect, float bias = 0.0f) {
    const auto& s = S().settings;
    float half_v = 0.0f;
    float half_h = 0.0f;
    for (const auto& fov : head.eye_fov) {
        half_v = std::max({half_v, fov.angle_up, -fov.angle_down});
        half_h = std::max({half_h, fov.angle_right, -fov.angle_left});
    }
    const bool located = half_v > 0.0f && half_h > 0.0f;
    // Vertical half-angle that covers both eyes widened by `extra`.
    const auto cover = [&](float extra) {
        return Tilt::CoverHalfVfov(head.eye_fov, aspect, bias, extra);
    };
    // A5 is image-affecting and opt-in. Derive a culling frustum from the eye
    // union with one degree per edge for near objects at the offset eye origins.
    // Both modes keep asymmetric projection; this changes only the game camera.
    static const bool tight_env = [] {
        const char* value = std::getenv("VR_NATIVE_TIGHT_FRUSTUM");
        return value && value[0] == '1';
    }();
    static const bool native_asymmetric = std::getenv("VR_NATIVE_STEREO") != nullptr &&
                                          std::getenv("VR_NATIVE_ASYMMETRIC") != nullptr;
    if (located && native_asymmetric && s.late_warp == 0 &&
        (tight_env || FastRender::On(FastRender::TightNativeFrustum))) {
        return 2.0f * cover(1.0f * DegToRad);
    }
    if (s.vfov_deg > 0.0f) {
        const float fixed = s.vfov_deg * DegToRad;
        return located && !head.fov_mutable ? std::max(fixed, 2.0f * cover(0.0f)) : fixed;
    }
    if (!located) {
        return 100.0f * DegToRad;
    }
    // While the bias applies the margin is at least render_pitch_min_margin_deg (Tilt::MarginDeg).
    return 2.0f *
           cover(Tilt::MarginDeg(s.fov_margin_deg, s.render_pitch_min_margin_deg, bias) * DegToRad);
}

// Clip-space x shift of `eye` (stereo_mode 0, HUD disparity): half the eye distance through the
// game's projection. Its horizontal scale comes from the render camera's aspect (`aspect`, else
// the last one the camera hook read, else the frame's W/H), as in the game's own projection.
float ComputeShift(s32 eye, float vfov, float aspect = 0.0f) {
    auto& st = S();
    const auto& s = st.settings;
    const HeadState head = GetHead();
    if (!(aspect > 0.0f)) {
        aspect = st.camera_aspect.load(std::memory_order_relaxed);
    }
    if (!(aspect > 0.0f)) {
        aspect = st.main_aspect.load(std::memory_order_relaxed);
    }
    const float p00 = 1.0f / (aspect * std::tan(vfov * 0.5f));
    const float half_ipd = 0.5f * EffectiveIpd(head) * s.world_scale;
    // Left eye = camera moved left = geometry moves right in clip space.
    const float sign = eye == 0 ? 1.0f : -1.0f;
    return sign * p00 * half_ipd;
}

// HUD panel parameter for the vertex shader: round(v) = scale * 16, fraction = the NDC
// disparity of a point at hud_depth_m for `eye` (-1 = mono). `aspect`: the frame's render camera
// aspect (0 = unknown).
float HudValue(s32 eye, float vfov, float aspect = 0.0f) {
    const auto& s = S().settings;
    const float scale = IsGameMenuOpen() ? s.menu_hud_scale : s.hud_scale;
    const float steps = std::clamp(std::round(scale * 16.0f), 1.0f, 16.0f);
    const float offset = (eye >= 0 && vfov > 0.0f)
                             ? ComputeShift(eye, vfov, aspect) / std::max(s.hud_depth_m, 0.25f)
                             : 0.0f;
    return steps + std::clamp(offset, -0.45f, 0.45f);
}

void ParseSettingsLine(Settings& s, const std::string& key, const std::string& value) {
    const auto f = [&] { return std::stof(value); };
    if (key == "graphics_config") {
        if (!Graphics::Publish(value))
            throw std::invalid_argument("Invalid graphics profile");
    } else if (key == "dlss_model") {
        // core/vr/dlss_model.h. vr.ini is read at start; ApplySetting (a VR page) is live: the
        // renderer recreates both DLSS eyes at the next frame.
        const auto model = Graphics::ParseDlssModel(value);
        if (!model)
            throw std::invalid_argument("dlss_model: builtin, auto, E, F, J, K, L or M");
        Graphics::dlss_model.store(*model, std::memory_order_relaxed);
    } else if (key == "upscale_whole_frame") {
        // S1 (FAST bit 51, graphics_settings.h): read once, when the game's patch is applied.
        Graphics::whole_frame_requested.store(value == "1" || value == "true",
                                              std::memory_order_relaxed);
    } else if (key == "stereo") {
        s.stereo = value == "1" || value == "true";
    } else if (key == "world_scale") {
        s.world_scale = f();
    } else if (key == "ipd_scale") {
        s.ipd_scale = f();
    } else if (key == "ipd_mm") {
        s.ipd_mm = std::clamp(f(), 0.0f, 120.0f);
    } else if (key == "vfov_deg") {
        s.vfov_deg = f();
    } else if (key == "fov_margin_deg") {
        s.fov_margin_deg = f();
    } else if (key == "render_pitch_bias_deg") {
        s.render_pitch_bias_deg = std::clamp(f(), -Tilt::MaxBiasDeg, Tilt::MaxBiasDeg);
    } else if (key == "render_pitch_min_margin_deg") {
        s.render_pitch_min_margin_deg = std::clamp(f(), 0.0f, 20.0f);
    } else if (key == "pitch_limit_up_deg") {
        s.pitch_limit_up_deg = f();
    } else if (key == "pitch_limit_down_deg") {
        s.pitch_limit_down_deg = f();
    } else if (key == "snap_turn_deg") {
        s.snap_turn_deg = f();
    } else if (key == "smooth_turn_deg_per_s") {
        s.smooth_turn_deg_per_s = f();
    } else if (key == "pose_lag_frames") {
        s.pose_lag_frames = static_cast<s32>(f());
    } else if (key == "camera_stale_ms") {
        s.camera_stale_ms = f();
    } else if (key == "camera_gap_resume") {
        s.camera_gap_resume = f() != 0.0f;
    } else if (key == "pair_max_age_ms") {
        s.pair_max_age_ms = std::clamp(f(), 20.0f, 200.0f);
    } else if (key == "screen_distance_m") {
        s.screen_distance_m = f();
    } else if (key == "screen_width_m") {
        s.screen_width_m = f();
    } else if (key == "pose_prediction_ms") {
        s.pose_prediction_ms = f();
    } else if (key == "refresh_hz") {
        s.refresh_hz = std::max(f(), 0.0f);
    } else if (key == "xr_exit_on_stop_s") {
        // Retired 2026-10-09 (core/vr/session_exit.h): a session stop never closes the emulator
        // (the headset taken off while holding the controllers closed the game), only the
        // runtime's EXITING or the window's close. An old vr.ini line is accepted and ignored.
        LOG_INFO(Core, "VR: xr_exit_on_stop_s={} ignored (retired: taking the headset off never "
                 "closes the game; Quit in Meta Link or closing the window does)",
                 value);
    } else if (key == "xr_exit_teardown_ms") {
        s.xr_exit_teardown_ms = std::clamp(f(), 0.0f, 10000.0f);
    } else if (key == "xr_exit_terminate") {
        s.xr_exit_terminate = value == "1" || value == "true";
    } else if (key == "frame_cap") {
        s.frame_cap = value == "auto" ? -1
                      : value == "off" ? 0
                                       : std::clamp(static_cast<s32>(std::lround(f())), 0, 120);
        if (s.frame_cap > 0 && s.frame_cap < 20) {
            s.frame_cap = 20;
        }
    } else if (key == "late_warp") {
        s.late_warp = std::clamp(static_cast<s32>(f()), 0, 1);
    } else if (key == "late_warp_motion") {
        s.late_warp_motion = value == "1" || value == "true";
    } else if (key == "late_warp_fetch_once") {
        s.late_warp_fetch_once = value == "1" || value == "true";
    } else if (key == "late_warp_edge_blend") {
        s.late_warp_edge_blend = std::clamp(f(), 0.0f, 4.0f);
    } else if (key == "stereo_adopt_finished") {
        s.stereo_adopt_finished = std::clamp(static_cast<s32>(f()), 0, 2);
    } else if (key == "texture_aniso") {
        const s32 v = std::clamp(static_cast<s32>(std::lround(f())), 0, 16);
        s.texture_aniso = v <= 1 ? 0 : v <= 2 ? 2 : v <= 4 ? 4 : v <= 8 ? 8 : 16;
    } else if (key == "texture_lod_bias") {
        s.texture_lod_bias = std::clamp(f(), -2.0f, 1.0f);
    } else if (key == "aa_mode") {
        if (const auto mode = Aa::ParseMode(value)) {
            s.aa_mode = *mode;
        }
    } else if (key == "aa_fxaa_fs") {
        s.aa_fxaa_fs = std::stoull(value, nullptr, 0);
    } else if (key == "taa_jitter") {
        s.taa_jitter = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "taa_phases") {
        s.taa_phases = f() >= 12.0f ? 16 : 8;
    } else if (key == "taa_alpha_min") {
        s.taa_alpha_min = std::clamp(f(), 1.0f / 32.0f, 0.5f);
    } else if (key == "taa_clip_gamma") {
        s.taa_clip_gamma = std::clamp(f(), 0.5f, 2.0f);
    } else if (key == "taa_depth_reject") {
        s.taa_depth_reject = std::clamp(f(), 0.001f, 0.2f);
    } else if (key == "taa_stable_depth") {
        s.taa_stable_depth = value == "1" || value == "true";
    } else if (key == "taa_sharpen") {
        s.taa_sharpen = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "taa_filter_px") {
        s.taa_filter_px = std::clamp(f(), 0.2f, 0.55f);
    } else if (key == "taa_near_m") {
        s.taa_near_m = std::clamp(f(), 0.0f, 12.0f);
    } else if (key == "taa_near_alpha") {
        s.taa_near_alpha = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "taa_eye_taa") {
        const float v = f();
        s.taa_eye_taa = v < 0.0f ? -1.0f : v <= 0.0f ? 0.0f : std::clamp(v, 0.3f, 0.9f);
    } else if (key == "taa_luma_weight") {
        s.taa_luma_weight = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "taa_occluder") {
        s.taa_occluder = value == "1" || value == "true";
    } else if (key == "taa_edge_blend") {
        const float v = f();
        s.taa_edge_blend = v < 0.0f ? -1.0f : std::min(v, 4.0f);
    } else if (key == "taa_sharpen_lift") {
        s.taa_sharpen_lift = std::clamp(f(), 0.0f, 0.5f);
    } else if (key == "taa_eye_sharpen") {
        const float v = f();
        s.taa_eye_sharpen = v < 0.0f ? -1.0f : std::min(v, 1.0f);
    } else if (key == "taa_debug") {
        s.taa_debug = std::clamp(static_cast<s32>(f()), 0, 3);
    } else if (key == "eye_size" || key == "eye_upscale") {
        // auto | panel | a factor (1 = off, 1.1 .. 2); 0 or less = auto (eye_upscale's old 0).
        std::string word{value};
        std::ranges::transform(word, word.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        if (word == "auto") {
            s.eye_size = EyeSize::Auto;
        } else if (word == "panel" || word == "native") {
            s.eye_size = EyeSize::Panel;
        } else if (const float v = f(); v <= 0.0f) {
            s.eye_size = EyeSize::Auto;
        } else {
            s.eye_size = EyeSize::Factor;
            s.eye_upscale = std::clamp(v, 1.0f, 2.0f);
        }
    } else if (key == "color_contrast") {
        Color::Set(Color::Control::Contrast, value);
    } else if (key == "color_saturation") {
        Color::Set(Color::Control::Saturation, value);
    } else if (key == "color_gamma") {
        Color::Set(Color::Control::Gamma, value);
    } else if (key == "eye_sharpen") {
        s.eye_sharpen = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "eye_size_min_factor") {
        s.eye_size_min_factor = std::clamp(f(), 1.0f, 2.0f);
    } else if (key == "eye_taa") {
        const float v = f();
        s.eye_taa = v <= 0.0f ? 0.0f : std::clamp(v, 0.3f, 0.9f);
    } else if (key == "gpu_submit_ms") {
        const float v = f();
        s.gpu_submit_ms = v <= 0.0f ? 0.0f : std::clamp(v, 0.5f, 16.0f);
    } else if (key == "perf_spec_memo") {
        s.perf_spec_memo = value == "1" || value == "true";
    } else if (key == "perf_rt_memo") {
        s.perf_rt_memo = value == "1" || value == "true";
    } else if (key == "perf_bind_memo") {
        s.perf_bind_memo = value == "1" || value == "true";
    } else if (key == "perf_copy_memo") {
        s.perf_copy_memo = value == "1" || value == "true";
    } else if (key == "perf_resync_memo") {
        s.perf_resync_memo = value == "1" || value == "true";
    } else if (key == "perf_depth_one_layout") {
        s.perf_depth_one_layout = value == "1" || value == "true";
    } else if (key == "desktop_mirror") {
        s.desktop_mirror = std::clamp(static_cast<s32>(std::lround(f())), 0, 60);
    } else if (key == "desktop_mirror_view") {
        s.desktop_mirror_view = static_cast<u8>(DesktopMirror::ParseView(value));
    } else if (key == "eye_taa_follow") {
        s.eye_taa_follow = value == "1" || value == "true";
    } else if (key == "eye_taa_gate_px") {
        s.eye_taa_gate_px = std::clamp(f(), 0.01f, 1.0f);
    } else if (key == "eye_taa_edge_cap") {
        s.eye_taa_edge_cap = std::clamp(f(), 0.1f, 1.0f);
    } else if (key == "eye_taa_jitter") {
        s.eye_taa_jitter = value == "true" ? 1.0f : std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "native_render_steady") {
        s.native_render_steady = value == "1" || value == "true";
    } else if (key == "native_render_steady_deg") {
        s.native_render_steady_deg = std::clamp(f(), 0.0f, 0.5f);
    } else if (key == "render_steady") {
        s.render_steady = value == "1" || value == "true";
    } else if (key == "render_steady_deg") {
        s.render_steady_deg = std::clamp(f(), 0.0f, 3.0f);
    } else if (key == "render_steady_mm") {
        s.render_steady_mm = std::clamp(f(), 0.0f, 20.0f);
    } else if (key == "render_steady_follow_deg_s") {
        s.render_steady_follow_deg_s = std::clamp(f(), 1.0f, 60.0f);
    } else if (key == "render_steady_follow_mm_s") {
        s.render_steady_follow_mm_s = std::clamp(f(), 5.0f, 500.0f);
    } else if (key == "late_warp_steady") {
        s.late_warp_steady = value == "1" || value == "true";
    } else if (key == "late_warp_steady_deg") {
        s.late_warp_steady_deg = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "late_warp_steady_mm") {
        s.late_warp_steady_mm = std::clamp(f(), 0.0f, 10.0f);
    } else if (key == "xr_depth") {
        s.xr_depth = value == "1" || value == "true";
    } else if (key == "xr_depth_debug_m") {
        s.xr_depth_debug_m = std::clamp(f(), 0.0f, 100.0f);
    } else if (key == "hud_scale") {
        s.hud_scale = f();
    } else if (key == "menu_mode") {
        s.menu_mode = value == "1" || value == "true";
    } else if (key == "menu_hud_scale") {
        s.menu_hud_scale = std::clamp(f(), 0.0625f, 1.0f);
    } else if (key == "menu_panel") {
        s.menu_panel = value == "1" || value == "true";
    } else if (key == "menu_present") {
        s.menu_present = value == "frozen" ? static_cast<s32>(MenuPresent::Frozen)
                         : value == "hud"  ? static_cast<s32>(MenuPresent::Hud)
                                           : static_cast<s32>(MenuPresent::Layer);
        s.menu_present_set = true;
    } else if (key == "ui_layer") {
        s.ui_layer = std::clamp(static_cast<s32>(f()), 0, 2);
    } else if (key == "ui_menu_distance_m") {
        s.ui_menu_distance_m = std::clamp(f(), 0.5f, 10.0f);
    } else if (key == "ui_menu_width_deg") {
        s.ui_menu_width_deg = std::clamp(f(), 30.0f, 150.0f);
    } else if (key == "ui_dialog_em_deg") {
        s.ui_dialog_em_deg = std::clamp(f(), 0.5f, 3.0f);
    } else if (key == "ui_dialog_max_deg") {
        // 0 = no cap of its own (the text size and the corners decide)
        s.ui_dialog_max_deg = f() <= 0.0f ? 0.0f : std::clamp(f(), 15.0f, 80.0f);
    } else if (key == "ui_menu_fit") {
        s.ui_menu_fit = value == "1" || value == "true";
    } else if (key == "ui_menu_max_width_deg") {
        // 0 = no cap of its own (the corners decide)
        s.ui_menu_max_width_deg = f() <= 0.0f ? 0.0f : std::clamp(f(), 30.0f, 150.0f);
    } else if (key == "ui_menu_max_height_deg") {
        s.ui_menu_max_height_deg = f() <= 0.0f ? 0.0f : std::clamp(f(), 20.0f, 90.0f);
    } else if (key == "ui_menu_fit_screen_wide") {
        s.ui_menu_fit_screen_wide = value == "1" || value == "true";
    } else if (key == "ui_menu_page") {
        s.ui_menu_page = value == "1" || value == "true";
    } else if (key == "ui_menu_snap") {
        s.ui_menu_snap = value == "1" || value == "true";
    } else if (key == "ui_travel_pitch_deg") {
        s.ui_travel_pitch_deg = std::clamp(f(), -30.0f, 30.0f);
    } else if (key == "ui_menu_corner_yaw_deg") {
        s.ui_menu_corner_yaw_deg = std::clamp(f(), 10.0f, 75.0f);
    } else if (key == "ui_menu_corner_pitch_deg") {
        s.ui_menu_corner_pitch_deg = std::clamp(f(), 10.0f, 75.0f);
    } else if (key == "ui_curved") {
        s.ui_curved = value == "1" || value == "true";
    } else if (key == "ui_hud_layout") {
        s.ui_hud_layout = value == "game" ? 1 : 0;
    } else if (key == "ui_hud_anchor") {
        s.ui_hud_anchor = value == "band" ? 1 : 0;
    } else if (key == "ui_hud_corner_yaw_deg") {
        s.ui_hud_corner_yaw_deg = std::clamp(f(), 5.0f, 60.0f);
    } else if (key == "ui_hud_corner_pitch_deg") {
        s.ui_hud_corner_pitch_deg = std::clamp(f(), -45.0f, 45.0f);
    } else if (key == "ui_hud_distance_m") {
        s.ui_hud_distance_m = std::clamp(f(), 0.5f, 10.0f);
    } else if (key == "ui_hud_scale_deg") {
        s.ui_hud_scale_deg = std::clamp(f(), 0.01f, 0.1f);
    } else if (key == "ui_hud_pitch_deg") {
        s.ui_hud_pitch_deg = std::clamp(f(), -30.0f, 30.0f);
    } else if (key == "ui_hud_follow_deg") {
        s.ui_hud_follow_deg = std::clamp(f(), 0.0f, 90.0f);
    } else if (key == "ui_hud_follow_pitch_deg") {
        s.ui_hud_follow_pitch_deg = std::clamp(f(), 0.0f, 90.0f);
    } else if (key == "ui_hud_follow_pitch_down_deg") {
        s.ui_hud_follow_pitch_down_deg = std::clamp(f(), 0.0f, 90.0f);
    } else if (key == "ui_hud_follow_ms") {
        s.ui_hud_follow_ms = std::clamp(f(), 0.0f, 5000.0f);
    } else if (key == "ui_hand_fade") {
        s.ui_hand_fade = value == "1" || value == "true";
    } else if (key == "ui_hud_blade_alpha") {
        s.ui_hud_blade_alpha = std::clamp(f(), 0.05f, 1.0f);
    } else if (key == "ui_hud_lower_alpha") {
        s.ui_hud_lower_alpha = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "ui_counters") {
        if (value == "look_down" || value == "0") {
            s.ui_counters = 0;
        } else if (value == "always" || value == "1") {
            s.ui_counters = 1;
        } else if (value == "hidden" || value == "2") {
            s.ui_counters = 2;
        } else {
            throw std::invalid_argument{"look_down, always or hidden expected"};
        }
    } else if (key == "ui_counters_pitch_deg") {
        s.ui_counters_pitch_deg = std::clamp(f(), 5.0f, 80.0f);
    } else if (key == "ui_counters_raise_deg") {
        s.ui_counters_raise_deg = std::clamp(f(), 0.0f, 30.0f);
    } else if (key == "ui_counters_pos") {
        if (value == "lower_middle" || value == "tray" || value == "0") {
            s.ui_counters_pos = 0;
        } else if (value == "corner" || value == "1") {
            s.ui_counters_pos = 1;
        } else if (value == "raised" || value == "2") {
            s.ui_counters_pos = 2;
        } else {
            throw std::invalid_argument{"lower_middle, corner or raised expected"};
        }
    } else if (key == "ui_counters_tray_top_deg") {
        s.ui_counters_tray_top_deg = std::clamp(f(), -40.0f, 10.0f);
    } else if (key == "ui_counters_tray_stack") {
        s.ui_counters_tray_stack = value == "1" || value == "true";
    } else if (key == "ui_counters_scale") {
        s.ui_counters_scale = std::clamp(f(), 0.5f, 2.5f);
    } else if (key == "ui_counters_gap_deg") {
        s.ui_counters_gap_deg = std::clamp(f(), 0.0f, 20.0f);
    } else if (key == "ui_status_left") {
        if (value == "auto" || value == "-1") {
            s.ui_status_left = -1;
        } else if (value == "look_down" || value == "0") {
            s.ui_status_left = 0;
        } else if (value == "always" || value == "1") {
            s.ui_status_left = 1;
        } else if (value == "hidden" || value == "2") {
            s.ui_status_left = 2;
        } else {
            throw std::invalid_argument{"auto, look_down, always or hidden expected"};
        }
    } else if (key == "shoulder_switch") {
        s.shoulder_switch = value == "1" || value == "true";
    } else if (key == "health_color") {
        s.health_color = value == "1" || value == "true";
    } else if (key == "stamina_color") {
        s.stamina_color = value == "1" || value == "true";
    } else if (key == "health_color_strength") {
        s.health_color_strength = std::clamp(f(), 0.f, 1.f);
    } else if (key == "stamina_color_strength") {
        s.stamina_color_strength = std::clamp(f(), 0.f, 1.f);
    } else if (key == "hud_vitals") {
        if (value == "classic" || value == "0") {
            s.hud_vitals = 0;
        } else if (value == "wrist" || value == "1") {
            s.hud_vitals = 1;
        } else {
            throw std::invalid_argument{"classic or wrist expected"};
        }
    } else if (key == "vitals_gauge_scale") {
        s.vitals_gauge_scale = std::clamp(f(), 0.5f, 2.0f);
    } else if (key == "vitals_gauge_from_wrist_m") {
        s.vitals_gauge_from_wrist_m = std::clamp(f(), 0.0f, 0.15f);
    } else if (key == "vitals_gauge_standoff_m") {
        s.vitals_gauge_standoff_m = std::clamp(f(), 0.0f, 0.03f);
    } else if (key == "vitals_gauge_pull_m") {
        s.vitals_gauge_pull_m = std::clamp(f(), 0.0f, 0.15f);
    } else if (key == "vitals_heartbeat") {
        s.vitals_heartbeat = value == "1" || value == "true";
    } else if (key == "vitals_low_hp") {
        s.vitals_low_hp = std::clamp(f(), 0.05f, 0.6f);
    } else if (key == "vitals_heartbeat_mute_s") {
        s.vitals_heartbeat_mute_s = std::clamp(f(), 0.0f, 600.0f);
    } else if (key == "vitals_heartbeat_gain") {
        s.vitals_heartbeat_gain = std::clamp(f(), 0.0f, 2.0f);
    } else if (key == "vitals_edge") {
        s.vitals_edge = value == "1" || value == "true";
    } else if (key == "vitals_edge_strength") {
        s.vitals_edge_strength = std::clamp(f(), 0.0f, 2.0f);
    } else if (key == "ui_menu_hud") {
        s.ui_menu_hud = value == "1" || value == "true";
    } else if (key == "ui_menu_hud_off_canvas") {
        s.ui_menu_hud_off_canvas = value == "1" || value == "true";
    } else if (key == "ui_rest_scale") {
        s.ui_rest_scale = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "ui_rest_depth_m") {
        s.ui_rest_depth_m = std::clamp(f(), 0.0f, 20.0f);
    } else if (key == "ui_rest_curved") {
        s.ui_rest_curved = value == "1" || value == "true";
    } else if (key == "ui_lock_marker_scale") {
        s.ui_lock_marker_scale = std::clamp(f(), 0.25f, 2.0f);
    } else if (key == "ui_lock_marker_alpha") {
        s.ui_lock_marker_alpha = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "ui_enemy_hp_bars") {
        s.ui_enemy_hp_bars = value == "1" || value == "true";
    } else if (key == "ui_boss_hp_bar") {
        s.ui_boss_hp_bar = value == "1" || value == "true";
    } else if (key == "ui_vr_buttons") {
        s.ui_vr_buttons = value == "1" || value == "true";
    } else if (key == "ui_conditions") {
        if (value == "view" || value == "0") {
            s.ui_conditions = 0;
        } else if (value == "game" || value == "1") {
            s.ui_conditions = 1;
        } else {
            throw std::invalid_argument{"view or game expected"};
        }
    } else if (key == "ui_conditions_pitch_deg") {
        s.ui_conditions_pitch_deg = std::clamp(f(), -40.0f, 40.0f);
    } else if (key == "ui_conditions_scale") {
        s.ui_conditions_scale = std::clamp(f(), 0.5f, 2.5f);
    } else if (key == "status_drain_pct") {
        s.status_drain_pct = std::clamp(static_cast<s32>(std::lround(f())), 0, 100);
    } else if (key == "ui_death_distance_m") {
        s.ui_death_distance_m = std::clamp(f(), 0.0f, 20.0f);
    } else if (key == "ui_death_pitch_deg") {
        s.ui_death_pitch_deg = std::clamp(f(), -45.0f, 45.0f);
    } else if (key == "ui_death_text_deg") {
        s.ui_death_text_deg = std::clamp(f(), 10.0f, 90.0f);
    } else if (key == "ui_banner_title") {
        s.ui_banner_title = value == "1" || value == "true";
    } else if (key == "ui_banner_text_deg") {
        s.ui_banner_text_deg = std::clamp(f(), 10.0f, 90.0f);
    } else if (key == "ui_veil") {
        s.ui_veil = value == "1" || value == "true";
    } else if (key == "ui_backdrop") {
        s.ui_backdrop_keep = value == "keep";
    } else if (key == "ui_grey") {
        s.ui_grey = std::clamp(f(), 0.25f, 0.9f);
    } else if (key == "ui_empty_snap") {
        s.ui_empty_snap = value == "1" || value == "true";
    } else if (key == "ui_fade_ms") {
        s.ui_fade_ms = std::clamp(f(), 0.0f, 2000.0f);
    } else if (key == "menu_width_deg") {
        s.menu_width_deg = std::clamp(f(), 20.0f, 120.0f);
    } else if (key == "menu_distance_m") {
        s.menu_distance_m = std::clamp(f(), 0.5f, 10.0f);
    } else if (key == "menu_dim") {
        s.menu_dim = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "menu_curved") {
        s.menu_curved = value == "1" || value == "true";
    } else if (key == "menu_follow_deg") {
        s.menu_follow_deg = std::clamp(f(), 0.0f, 180.0f);
    } else if (key == "menu_dialog_zoom") {
        s.menu_dialog_zoom = std::clamp(f(), 1.0f, 2.5f);
    } else if (key == "menu_zoom_menus") {
        s.menu_zoom_menus = value;
    } else if (key == "vr_menu") {
        s.vr_menu = value == "1" || value == "true" ? 1 : 0;
    } else if (key == "seated_eye_cm") {
        const s32 cm = static_cast<s32>(std::lround(f()));
        s.seated_eye_cm = cm <= 0 ? 0 : std::clamp(cm, 50, 180);
    } else if (PlayerBody::IsHeightKey(key)) {
        // Your height (player_body.h): height_base, height_adjust_cm (clamped to -10..+10); the
        // older builds' game_height_cm / player_height_cm are ignored (said once each).
        PlayerBody::HeightSetting h{s.height_player ? PlayerBody::HeightBase::Player
                                                    : PlayerBody::HeightBase::Hunter,
                                    s.height_adjust_cm};
        switch (PlayerBody::ParseHeightSetting(h, key, value)) {
        case PlayerBody::HeightKey::Set:
            s.height_player = h.base == PlayerBody::HeightBase::Player;
            s.height_adjust_cm = h.adjust_cm;
            break;
        case PlayerBody::HeightKey::Legacy: {
            static bool said_game = false, said_player = false;
            bool& said = key == "game_height_cm" ? said_game : said_player;
            if (!said) {
                said = true;
                LOG_INFO(Core,
                         "VR: {}={} ignored (an older build's height row; Height and Adjust height "
                         "on the VR page replace it)",
                         key, value);
            }
            break;
        }
        case PlayerBody::HeightKey::Bad:
            throw std::invalid_argument(key == "height_base"
                                            ? "height_base is hunter or player"
                                            : "height_adjust_cm is a number of cm");
        case PlayerBody::HeightKey::Other:
            break;
        }
    } else if (key == "swap_sticks") {
        s.swap_sticks = value == "1" || value == "true";
    } else if (key == "hud_depth_m") {
        s.hud_depth_m = f();
    } else if (key == "hud_after_vs") {
        s.hud_after_vs = std::stoull(value, nullptr, 0);
    } else if (key == "decoupled") {
        s.decoupled = value == "1" || value == "true";
    } else if (key == "orbit_pitch_deg") {
        s.orbit_pitch_deg = f();
    } else if (key == "vertical_smoothing_ms") {
        // Longer would let the height drift in the 0.2-0.4 Hz band where motion sickness peaks.
        s.vertical_smoothing_ms = std::clamp(f(), 0.0f, 300.0f);
        if (f() > 300.0f) {
            LOG_WARNING(Core, "VR: vertical_smoothing_ms={} is above the 300 ms limit; using 300",
                        value);
        }
    } else if (key == "dodge_camera") {
        s.dodge_camera = value == "1" || value == "true";
    } else if (key == "follow_smoothing_ms") {
        s.follow_smoothing_ms = std::clamp(f(), 0.0f, 100.0f);
    } else if (key == "pair_lock_position") {
        s.pair_lock_position = value == "1" || value == "true";
    } else if (key == "dodge_vertical_ms") {
        s.dodge_vertical_ms = std::clamp(f(), 0.0f, 300.0f);
    } else if (key == "vertical_stop_factor") {
        s.vertical_stop_factor = std::clamp(f(), 0.1f, 1.0f);
    } else if (key == "fall_vertical_ms") {
        s.fall_vertical_ms = std::clamp(f(), 0.0f, 200.0f);
    } else if (key == "jump_height_scale") {
        s.jump_height_scale = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "jump_rise_m") {
        // Legacy cue: retain off, otherwise use the real animated height.
        s.jump_height_scale = f() > 0.f ? 1.f : 0.f;
    } else if (key == "dodge_dip_m") {
        s.dodge_dip_m = std::clamp(f(), 0.0f, 0.13f);
    } else if (key == "roll_chord") {
        s.roll_chord = value == "1" || value == "true";
    } else if (key == "lean_dodge") {
        s.lean_dodge = value == "1" || value == "true";
    } else if (key == "lean_dodge_speed") {
        s.lean_dodge_speed = std::clamp(f(), 0.5f, 5.0f);
    } else if (key == "lean_dodge_dist_m") {
        s.lean_dodge_dist_m = std::clamp(f(), 0.05f, 0.6f);
    } else if (key == "lean_dodge_cooldown_ms") {
        s.lean_dodge_cooldown_ms = std::clamp(f(), 200.0f, 5000.0f);
    } else if (key == "physical_evasion") {
        s.physical_evasion = value == "1" || value == "true";
    } else if (key == "evasion_gain") {
        s.evasion_gain = std::clamp(f(), 1.0f, 2.0f);
    } else if (key == "evasion_max_m") {
        s.evasion_max_m = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "evasion_lean_m") {
        s.evasion_lean_m = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "evasion_duck_m") {
        s.evasion_duck_m = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "evasion_wall") {
        s.evasion_wall = value == "1" || value == "true";
    } else if (key == "evasion_wall_margin_m") {
        s.evasion_wall_margin_m = std::clamp(f(), 0.0f, 0.3f);
    } else if (key == "evasion_wall_rise_mps") {
        s.evasion_wall_rise_mps = std::clamp(f(), 0.2f, 10.0f);
    } else if (key == "evasion_hold_ms") {
        s.evasion_hold_ms = std::clamp(f(), 0.0f, 10000.0f);
    } else if (key == "evasion_hold_tau_ms") {
        s.evasion_hold_tau_ms = std::clamp(f(), 50.0f, 5000.0f);
    } else if (key == "evasion_settle_ms") {
        s.evasion_settle_ms = std::clamp(f(), 0.0f, 10000.0f);
    } else if (key == "evasion_settle_tau_ms") {
        s.evasion_settle_tau_ms = std::clamp(f(), 50.0f, 10000.0f);
    } else if (key == "evasion_pelvis") {
        s.evasion_pelvis = value == "1" || value == "true";
    } else if (key == "evasion_feedback") {
        s.evasion_feedback = value == "1" || value == "true";
    } else if (key == "player_hp_bonus_pct") {
        s.player_hp_bonus_pct = std::clamp(static_cast<s32>(std::lround(f())), 0, 100);
    } else if (key == "offhand_damage_pct") {
        s.offhand_damage_pct = std::clamp(static_cast<s32>(std::lround(f())), 0, 100);
    } else if (key == "melee_kill_ragdoll") {
        s.melee_kill_ragdoll = value == "1" || value == "true";
    } else if (key == "melee_ragdoll_impulse") {
        s.melee_ragdoll_impulse = std::clamp(f(), 0.0f, 3.0f);
    } else if (key == "melee_ragdoll_vanilla_pct") {
        s.melee_ragdoll_vanilla_pct = std::clamp(static_cast<s32>(std::lround(f())), 0, 100);
    } else if (key == "dodge_iframes_extra") {
        s.dodge_iframes_extra = std::clamp(static_cast<s32>(std::lround(f())), 0, 10);
    } else if (key == "dodge_attack") {
        s.dodge_attack = value == "1" || value == "true";
    } else if (key == "dodge_attack_iframes") {
        s.dodge_attack_iframes = value == "1" || value == "true";
    } else if (key == "stereo_mode") {
        s.stereo_mode = std::clamp(static_cast<s32>(f()), 0, 2);
    } else if (key == "dibr_knee_m") {
        s.dibr_knee_m = std::clamp(f(), 0.0f, 2.0f);
    } else if (key == "dibr_near_clamp_m") {
        s.dibr_near_clamp_m = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "dibr_quality") {
        s.dibr_quality = std::clamp(static_cast<s32>(f()), 0, 2);
    } else if (key == "dibr_baseline_scale") {
        s.dibr_baseline_scale = std::clamp(f(), 0.0f, 2.0f);
    } else if (key == "dibr_debug_holes") {
        s.dibr_debug_holes = std::clamp(static_cast<s32>(f()), 0, 2);
    } else if (key == "dibr_seen") {
        s.dibr_seen = value == "1" || value == "true";
    } else if (key == "first_person") {
        s.first_person = static_cast<s32>(f());
    } else if (key == "fp_eye_height_m") {
        s.fp_eye_height_m = f();
    } else if (key == "fp_forward_m") {
        s.fp_forward_m = f();
    } else if (key == "fp_near_m") {
        s.fp_near_m = f();
    } else if (key == "fp_near_down_m") {
        s.fp_near_down_m = std::clamp(f(), 0.0f, 0.5f);
    } else if (key == "fp_ahead_m") {
        s.fp_ahead_m = std::clamp(f(), 0.0f, 0.2f);
    } else if (key == "fp_ahead_down_m") {
        s.fp_ahead_down_m = std::clamp(f(), 0.0f, 0.2f);
    } else if (key == "camera_roll") {
        s.camera_roll = value == "1" || value == "true";
    } else if (key == "positional") {
        s.positional = value == "1" || value == "true";
    } else if (key == "positional_limit_m") {
        s.positional_limit_m = f();
    } else if (key == "roomscale_walk") {
        s.roomscale_walk = value == "1" || value == "true";
    } else if (key == "roomscale_start_m") {
        s.roomscale_start_m = std::clamp(f(), 0.15f, 0.75f);
    } else if (key == "roomscale_stop_m") {
        s.roomscale_stop_m = std::clamp(f(), 0.03f, 0.70f);
    } else if (key == "roomscale_max_stick") {
        s.roomscale_max_stick = std::clamp(f(), 0.25f, 0.60f);
    } else if (key == "positional_limit_down_m") {
        s.positional_limit_down_m = std::clamp(f(), 0.0f, 1.5f);
    } else if (key == "controls") {
        s.controls_classic = value == "classic";
    } else if (key == "menu_hold") {
        s.menu_hold_screen = value == "screen";
    } else if (key == "y_menu") {
        // The env VR_Y_MENU's words too (one parser): auto / on, 1, true, yes / off, 0, false, no.
        const int parsed = VR::YMenu::ParseSetting(value);
        if (parsed < 0) {
            LOG_WARNING(Core, "VR: y_menu={} is not auto, on or off (or 1 / 0, true / false, yes / "
                              "no); using auto",
                        value);
        }
        s.y_menu = parsed < 0 ? 0 : parsed;
    } else if (key == "right_grip") {
        s.right_grip_draw = value == "draw";
    } else if (key == "sticky_grip") {
        s.sticky_grip = value == "1" || value == "true";
    } else if (key == "left_scheme") {
        s.left_scheme = std::clamp(static_cast<s32>(std::lround(f())), 1, 2);
    } else if (key == "heavy") {
        s.heavy_button = value == "button";
    } else if (key == "heavy_arc") {
        s.heavy_arc = value == "1" || value == "true";
    } else if (key == "heavy_arc_deg") {
        s.heavy_arc_deg = std::clamp(f(), 90.0f, 220.0f);
    } else if (key == "heavy_windup_deg") {
        s.heavy_windup_deg = std::clamp(f(), 80.0f, 150.0f);
    } else if (key == "seated") {
        s.seated = value == "1" || value == "true";
    } else if (key == "heavy_ready_tick") {
        s.heavy_ready_tick = value == "1" || value == "true";
    } else if (key == "blade_pitch_deg") {
        s.blade_pitch_deg = std::clamp(f(), -60.0f, 60.0f);
    } else if (key == "swing_windup_turn_deg") {
        s.swing_windup_turn_deg = std::clamp(f(), 0.0f, 90.0f);
    } else if (key == "swing_toward_front_deg") {
        s.swing_toward_front_deg = std::clamp(f(), 0.0f, 45.0f);
    } else if (key == "swing_windup_rejudge") {
        s.swing_windup_rejudge = value == "1" || value == "true";
    } else if (key == "swing_face_enter_speed") {
        s.swing_face_enter_speed = std::clamp(f(), 0.0f, 10.0f);
    } else if (key == "inject") {
        s.inject = value == "1" || value == "true";
    } else if (key == "inject_full_hp") {
        s.inject_full_hp = value == "1" || value == "true";
    } else if (key == "give_kit") {
        s.give_kit = value; // test / debug only (bloodborne_givekit.h)
    } else if (key == "give_kit_equip") {
        s.give_kit_equip = value == "1" || value == "true";
    } else if (key == "talk_guard") {
        s.talk_guard = value == "1" || value == "true";
    } else if (key == "vial_timeout_s") {
        s.vial_timeout_s = std::clamp(f(), 0.0f, 600.0f);
    } else if (key == "vial_zone_scale") {
        s.vial_zone_scale = std::clamp(f(), 0.5f, 2.0f);
    } else if (key == "overlay") {
        s.overlay = value == "1" || value == "true";
    } else if (key == "overlay_scale") {
        s.overlay_scale = std::clamp(f(), 0.5f, 1.0f);
    } else if (key == "overlay_msaa") {
        s.overlay_msaa = std::clamp(static_cast<s32>(std::lround(f())), 1, 8);
    } else if (key == "overlay_brightness") {
        s.overlay_brightness = std::clamp(f(), 0.1f, 4.0f);
    } else if (key == "vial_visual") {
        s.vial_visual = value == "1" || value == "true";
    } else if (key == "vial_offset_mm" || key == "vial_rot_deg") {
        // Three comma-separated values (grip X, Y, Z).
        std::array<float, 3> v{};
        if (std::sscanf(value.c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]) != 3) {
            throw std::invalid_argument{"three values expected"};
        }
        const float limit = key == "vial_offset_mm" ? 50.0f : 180.0f;
        for (float& c : v) {
            c = std::clamp(c, -limit, limit);
        }
        (key == "vial_offset_mm" ? s.vial_offset_mm : s.vial_rot_deg) = v;
    } else if (key == "vial_holdout") {
        s.vial_holdout = value == "1" || value == "true";
    } else if (key == "vial_in_hand") {
        if (value != "game" && value != "overlay" && value != "controller") {
            throw std::invalid_argument{"game or overlay expected"};
        }
        s.vial_in_hand_game = value == "game";
    } else if (key == "vial_hide_left") {
        s.vial_hide_left = std::clamp(static_cast<s32>(std::lround(f())), 0, 2);
    } else if (key == "vial_hide_two_handed") {
        s.vial_hide_two_handed = value == "1" || value == "true";
    } else if (key == "vial_inject_hold_s") {
        s.vial_inject_hold_s = std::clamp(f(), 0.0f, 2.0f);
    } else if (key == "vial_inject_hold_frames") {
        s.vial_inject_hold_frames = std::clamp(static_cast<s32>(std::lround(f())), 0, 15);
    } else if (key == "vial_inject_hold_hands") {
        if (value != "both" && value != "vial") {
            throw std::invalid_argument{"both or vial expected"};
        }
        s.vial_inject_hold_both = value == "both";
    } else if (key == "vial_inject_hold_ms") {
        s.vial_inject_hold_ms = std::clamp(static_cast<s32>(std::lround(f())), 0, 1000);
    } else if (key == "vial_inject_release_ms") {
        s.vial_inject_release_ms = std::clamp(static_cast<s32>(std::lround(f())), 0, 1000);
    } else if (key == "vial_inject_drain") {
        s.vial_inject_drain = value == "1" || value == "true";
    } else if (key == "vial_inject_drain_ms") {
        s.vial_inject_drain_ms = std::clamp(static_cast<s32>(std::lround(f())), 50, 1000);
    } else if (key == "vial_inject_empty_ms") {
        s.vial_inject_empty_ms = std::clamp(static_cast<s32>(std::lround(f())), 0, 1000);
    } else if (key == "vial_needle") {
        s.vial_needle = value == "1" || value == "true";
    } else if (key == "vial_dibr_proxy") {
        s.vial_dibr_proxy = value == "1" || value == "true";
    } else if (key == "controller_models") {
        s.controller_models = std::clamp(static_cast<s32>(std::lround(f())), 0, 2);
    } else if (key == "laser") {
        s.laser = value == "1" || value == "true";
    } else if (key == "overlay_debug") {
        s.overlay_debug = value == "1" || value == "true";
    } else if (key == "heal_vignette") {
        s.heal_vignette = value == "1" || value == "true";
    } else if (key == "heal_vignette_strength") {
        s.heal_vignette_strength = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "heal_vignette_color") {
        // Three comma-separated values (r, g, b), display colour 0 .. 1.
        std::array<float, 3> v{};
        if (std::sscanf(value.c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]) != 3) {
            throw std::invalid_argument{"three values expected"};
        }
        for (float& c : v) {
            c = std::clamp(c, 0.0f, 1.0f);
        }
        s.heal_vignette_color = v;
    } else if (key == "hit_vignette") {
        s.hit_vignette = value == "1" || value == "true";
    } else if (key == "hit_vignette_strength") {
        s.hit_vignette_strength = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "hit_vignette_fade_ms") {
        s.hit_vignette_fade_ms = std::clamp(f(), 50.0f, 5000.0f);
    } else if (key == "hit_vignette_inner") {
        s.hit_vignette_inner = std::clamp(f(), 0.3f, 0.95f);
    } else if (key == "hit_vignette_directional") {
        s.hit_vignette_directional = value == "1" || value == "true";
    } else if (key == "vignette_in_warp") {
        s.vignette_in_warp = value == "1" || value == "true";
    } else if (key == "hit_haptics_directional") {
        s.hit_haptics_directional = value == "1" || value == "true";
    } else if (key == "stagger_vignette") {
        s.stagger_vignette = value == "1" || value == "true";
    } else if (key == "stagger_vignette_strength") {
        s.stagger_vignette_strength = std::clamp(f(), 0.0f, 1.5f);
    } else if (key == "stagger_vignette_merge_ms") {
        s.stagger_vignette_merge_ms = std::clamp(f(), 0.0f, 1000.0f);
    } else if (key == "stagger_vignette_fit") {
        s.stagger_vignette_fit = value == "1" || value == "true";
    } else if (key == "stagger_vignette_dip_ms") {
        s.stagger_vignette_dip_ms = std::clamp(f(), 0.0f, 400.0f);
    } else if (key == "hit_vignette_fit") {
        s.hit_vignette_fit = value == "1" || value == "true";
    } else if (key == "stagger_vignette_red") {
        s.stagger_vignette_red = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "hitstun_vignette") {
        s.hitstun_vignette =
            value == "true" ? 1 : std::clamp(static_cast<s32>(std::lround(f())), 0, 2);
    } else if (key == "hitstun_vignette_strength") {
        s.hitstun_vignette_strength = std::clamp(f(), 0.0f, 1.5f);
    } else if (key == "stagger_shorten_ms") {
        s.stagger_shorten_ms = std::clamp(f(), 0.0f, 400.0f);
    } else if (key == "hit_vignette_color") {
        // Three comma-separated values (r, g, b), display colour 0 .. 1.
        std::array<float, 3> v{};
        if (std::sscanf(value.c_str(), "%f,%f,%f", &v[0], &v[1], &v[2]) != 3) {
            throw std::invalid_argument{"three values expected"};
        }
        for (float& c : v) {
            c = std::clamp(c, 0.0f, 1.0f);
        }
        s.hit_vignette_color = v;
    } else if (key == "haptic_gain") {
        s.haptic_gain = std::clamp(f(), 0.0f, 1.5f);
    } else if (key == "haptic_floor") {
        s.haptic_floor = std::clamp(f(), 0.0f, 0.8f);
    } else if (key == "haptic_min_ms") {
        s.haptic_min_ms = std::clamp(f(), 0.0f, 100.0f);
    } else if (key == "haptic_startup_test") {
        s.haptic_startup_test = value == "1" || value == "true";
    } else if (key == "haptic_melee_hit") {
        s.haptic_melee_hit = std::clamp(f(), 0.0f, 1.5f);
    } else if (key == "haptic_melee_hit_ms") {
        s.haptic_melee_hit_ms = std::clamp(f(), 0.0f, 300.0f);
    } else if (key == "haptic_melee_crunch") {
        s.haptic_melee_crunch = value == "1" || value == "true";
    } else if (key == "haptic_hit_taken") {
        s.haptic_hit_taken = std::clamp(f(), 0.0f, 1.5f);
    } else if (key == "haptic_gun_hit") {
        s.haptic_gun_hit = std::clamp(f(), 0.0f, 1.5f);
    } else if (key == "haptic_throw_hit") {
        s.haptic_throw_hit = std::clamp(f(), 0.0f, 1.5f);
    } else if (key == "haptic_inject") {
        s.haptic_inject = std::clamp(f(), 0.0f, 1.5f);
    } else if (key == "haptic_hit_rumble") {
        s.haptic_hit_rumble = std::clamp(f(), 0.0f, 1.0f);
    } else if (key == "gestures") {
        s.gestures = value == "1" || value == "true";
    } else if (key == "sim_motion") {
        s.sim_motion = value == "1" || value == "true";
    } else if (key == "cinematic") {
        s.cinematic = value == "1" || value == "true";
    } else if (key == "cinematic_visceral") {
        s.cinematic_visceral = value == "1" || value == "true";
    } else if (key == "vr_grab_attacks") {
        s.vr_grab_attacks = value == "1" || value == "true";
    } else if (key == "cinematic_grabbed") {
        s.cinematic_grabbed = value == "1" || value == "true";
    } else if (key == "cinematic_objects") {
        s.cinematic_objects = value == "1" || value == "true";
    } else if (key == "cinematic_lamps") {
        s.cinematic_lamps = value == "1" || value == "true";
    } else if (key == "cinematic_pickup") {
        s.cinematic_pickup = value == "1" || value == "true";
    } else if (key == "lamp_travel") {
        Lamps::SetEnabled(value == "1" || value == "true");
    } else if (key == "physical_levers") {
        Lever::SetEnabled(value == "1" || value == "true");
    } else if (key == "pickup_instant") {
        s.pickup_instant = value == "1" || value == "true";
    } else if (key == "cinematic_scripted") {
        s.cinematic_scripted = value == "1" || value == "true";
    } else if (key == "cinematic_item") {
        s.cinematic_item = value == "1" || value == "true";
    } else if (key == "cinematic_levelup") {
        s.cinematic_levelup = value == "1" || value == "true";
    } else if (key == "cinematic_fade_ms") {
        s.cinematic_fade_ms = std::clamp(f(), 0.0f, 2000.0f);
    } else if (key == "cinematic_return_fade_ms") {
        const float v = f();
        s.cinematic_return_fade_ms = v < 0.0f ? -1.0f : std::min(v, 2000.0f);
    } else if (key == "cinematic_exit_ms") {
        s.cinematic_exit_ms = std::clamp(f(), 0.0f, 5000.0f);
    } else if (key == "cinematic_min_ms") {
        s.cinematic_min_ms = std::clamp(f(), 0.0f, 10000.0f);
    } else if (key == "cinematic_release") {
        s.cinematic_release = value == "1" || value == "true";
    } else if (key == "cinematic_death_hold") {
        s.cinematic_death_hold = value == "1" || value == "true";
    } else if (key == "cinematic_throw_timeout_s") {
        s.cinematic_throw_timeout_s = std::clamp(f(), 1.0f, 120.0f);
    } else if (key == "cinematic_ladder") {
        s.cinematic_ladder = value == "1" || value == "true";
    } else if (key == "cinematic_ladder_enter_delay_ms") {
        s.cinematic_ladder_enter_delay_ms = std::clamp(f(), 0.0f, 10000.0f);
    } else if (key == "cinematic_ladder_min_flat_ms") {
        s.cinematic_ladder_min_flat_ms = std::clamp(f(), 0.0f, 60000.0f);
    } else if (key == "cinematic_ladder_exit_lead_ms") {
        s.cinematic_ladder_exit_lead_ms = std::clamp(f(), 0.0f, 5000.0f);
    } else if (key == "cinematic_ladder_reenter_ms") {
        s.cinematic_ladder_reenter_ms = std::clamp(f(), 0.0f, 10000.0f);
    } else if (key == "cinematic_ladder_fade_ms") {
        s.cinematic_ladder_fade_ms = std::clamp(f(), 0.0f, 2000.0f);
    } else if (key == "cinematic_ladder_combat_exit") {
        s.cinematic_ladder_combat_exit = value == "1" || value == "true";
    } else if (key == "cinematic_ladder_damage_pct") {
        s.cinematic_ladder_damage_pct = std::clamp(f(), 0.0f, 100.0f);
    } else if (!Body::ParseBodySetting(key, value) && !Quick::ParseQuickSetting(key, value) &&
               !Gun::ParseGunSetting(key, value) && !Melee::ParseMeleeSetting(key, value) &&
               !Transform::ParseTransformSetting(key, value) &&
               !PoseGesture::ParsePoseSetting(key, value) && !Whip::ParseWhipSetting(key, value) &&
               !TwoHand::ParseTwoHandSetting(key, value) && !Sound::ParseAudioSetting(key, value) &&
               !Bow::ParseBowSetting(key, value) && !Torch::ParseTorchSetting(key, value) &&
               !Tricks::ParseTrickSetting(key, value) &&
               !BladeSound::ParseBladeSoundSetting(key, value)) {
        LOG_WARNING(Core, "VR: unknown setting '{}'", key);
    }
}

// The neck pivot of a head pose (LOCAL space): physical evasion measures leans from it.
Vec3 NeckPivot(const Pose& head) {
    const auto& q = head.orientation;
    const Evade::Vec3 neck = Evade::NeckFromEye({head.position.x, head.position.y, head.position.z},
                                                q.x, q.y, q.z, q.w);
    return {neck.x, neck.y, neck.z};
}

} // Anonymous namespace

bool LagTestEnabled();

Settings& GetSettings() {
    return S().settings;
}

namespace {
// The vr.ini stereo values become the mode in effect (and the live switch starts from them).
void PublishStereoSettings() {
    auto& st = S();
    const u32 word = PackStereo(st.settings.stereo, st.settings.stereo_mode);
    st.stereo_word.store(word);
    st.stereo_request.store(word);
}
// vr.ini in the user directory, or the file VR_INI names (test runs use their own copy and never
// touch the player's settings).
std::filesystem::path SettingsPath() {
    if (const char* override_path = std::getenv("VR_INI");
        override_path != nullptr && override_path[0] != '\0') {
        return std::filesystem::path{override_path};
    }
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "vr.ini";
}
} // Anonymous namespace

void LoadSettings() {
    auto& room_state=S();
    if(VR::Interactions::DllRequested() && !room_state.roomscale_module){
        room_state.roomscale_module=new Interactions::Module();room_state.roomscale_module->EnableFaultBroadcast();
        if(!room_state.roomscale_module->LoadBesideExecutable()){
            room_state.roomscale_fault=true;LOG_ERROR(Core,"VR roomscale: DLL unavailable: {}",room_state.roomscale_module->Diagnostic());
        }
        room_state.roomscale_walk.Bind(*room_state.roomscale_module,room_state.roomscale_fault);
    }

    const auto path = SettingsPath();
    std::ifstream file{path};
    if (!file) {
        PublishStereoSettings();
        if (std::error_code ec; std::filesystem::exists(path, ec) || ec) {
            // There but unreadable (another program holds it, its permissions): the defaults
            // apply for this session and the player's file is left as it is.
            LOG_WARNING(Core, "VR: cannot read {}; the default settings apply and the file is "
                              "left as it is",
                        Common::FS::PathToUTF8String(path));
            return;
        }
        std::ofstream out{path};
        if (out) {
            const Settings d{};
            out << "# Bloodborne VR settings. Delete this file to restore defaults.\n"
                << "# starting stereo mode (in VR: Menu + left stick left/right steps mono / AER / "
                   "DIBR)\n"
                << "stereo=" << (d.stereo ? 1 : 0) << "\n"
                << "# 2 = depth-based stereo (one centre frame, both eyes every frame), 1 = "
                   "alternate-eye\n"
                << "# (each eye rendered from its own camera position), 0 = clip-space shift\n"
                << "stereo_mode=" << d.stereo_mode << "\n"
                << "# stereo_mode 2: nearer than dibr_knee_m (m) the depth is compressed so "
                   "nothing looks\n"
                << "# nearer than dibr_near_clamp_m (m); 0 = off\n"
                << "dibr_knee_m=" << d.dibr_knee_m << "\n"
                << "dibr_near_clamp_m=" << d.dibr_near_clamp_m << "\n"
                << "# stereo_mode 2 resampling: 1 = sharp (Catmull-Rom), 0 = linear\n"
                << "dibr_quality=" << d.dibr_quality << "\n"
                << "# stereo_mode 2 eye distance multiplier (0 = mono control, for tests)\n"
                << "dibr_baseline_scale=" << d.dibr_baseline_scale << "\n"
                << "# stereo_mode 2 debug: 1 = disocclusion holes green, 2 = also the HUD "
                   "magenta\n"
                << "dibr_debug_holes=" << d.dibr_debug_holes << "\n"
                << "# the one-time switch of an older stereo_mode=1 to 2 already happened\n"
                << "dibr_seen=1\n"
                << "# 1 = first person (camera in the hunter's head), 0 = third person\n"
                << "first_person=" << d.first_person << "\n"
                << "fp_eye_height_m=" << d.fp_eye_height_m << "\n"
                << "fp_forward_m=" << d.fp_forward_m << "\n"
                << "fp_near_m=" << d.fp_near_m << "\n"
                << "# looking down, the near clip moves out to this (m; hides the collar under "
                   "your eyes), 0 = off\n"
                << "fp_near_down_m=" << d.fp_near_down_m << "\n"
                << "# the camera this much ahead of the hunter's eyes (m; his body stays behind "
                   "it), always / looking down\n"
                << "fp_ahead_m=" << d.fp_ahead_m << "\n"
                << "fp_ahead_down_m=" << d.fp_ahead_down_m << "\n"
                << "# 1 = the camera rolls with your head (the stereo baseline is the line "
                   "between your eyes)\n"
                << "camera_roll=" << (d.camera_roll ? 1 : 0) << "\n"
                << "# 1 = leaning/crouching moves the game camera (6DoF)\n"
                << "positional=" << (d.positional ? 1 : 0) << "\n"
                << "positional_limit_m=" << d.positional_limit_m << "\n"
                << "# ...and below your recentered head (a duck, leaning or not; 0 = "
                   "positional_limit_m)\n"
                << "positional_limit_down_m=" << d.positional_limit_down_m << "\n"
                << "# standing room-scale walking: start / stop distance (m), maximum walking stick\n"
                << "roomscale_walk=" << (d.roomscale_walk ? 1 : 0) << "\n"
                << "roomscale_start_m=" << d.roomscale_start_m << "\n"
                << "roomscale_stop_m=" << d.roomscale_stop_m << "\n"
                << "roomscale_max_stick=" << d.roomscale_max_stick << "\n"
                << "world_scale=" << d.world_scale << "\n"
                << "ipd_scale=" << d.ipd_scale << "\n"
                << "# eye distance in mm for the game cameras (0 = headset IPD). In VR: hold\n"
                << "# Menu + right stick up/down to change it, Menu + right stick click = reset\n"
                << "ipd_mm=" << d.ipd_mm << "\n"
                << "# 0 = derive the game FOV from the headset\n"
                << "vfov_deg=" << d.vfov_deg << "\n"
                << "# extra game FOV around the headset's; each eye is cropped to the headset FOV\n"
                << "fov_margin_deg=" << d.fov_margin_deg << "\n"
                << "# the game's render looks this many degrees below your head (DIBR with the "
                   "late warp,\n# which undoes it: nothing you see moves), so the late warp's "
                   "room for fast head\n# turns is shared between the top and the bottom of your "
                   "view; 0 = off\n"
                << "render_pitch_bias_deg=" << d.render_pitch_bias_deg << "\n"
                << "# while the bias applies, fov_margin_deg is at least this (a larger one is "
                   "kept)\n"
                << "render_pitch_min_margin_deg=" << d.render_pitch_min_margin_deg << "\n"
                << "# 1 = the game's render camera holds still while your head stays within "
                   "render_steady_deg /\n# _mm of it (standing still: no blinking aliasing), "
                   "follows at once faster than\n# render_steady_follow_deg_s / _mm_s; the late "
                   "warp shows your true head pose (DIBR)\n"
                << "render_steady=" << (d.render_steady ? 1 : 0) << "\n"
                << "# Native angular-only stabilization; position stays live. Experimental.\n"
                << "native_render_steady=" << (d.native_render_steady ? 1 : 0) << "\n"
                << "native_render_steady_deg=" << d.native_render_steady_deg << "\n"
                << "render_steady_deg=" << d.render_steady_deg << "\n"
                << "render_steady_mm=" << d.render_steady_mm << "\n"
                << "render_steady_follow_deg_s=" << d.render_steady_follow_deg_s << "\n"
                << "render_steady_follow_mm_s=" << d.render_steady_follow_mm_s << "\n"
                << "# 1 = standing still the late warp keeps one head pose (within "
                   "late_warp_steady_deg /\n# _mm), the headset turns the eyes to your head "
                   "(no blinking resampling)\n"
                << "late_warp_steady=" << (d.late_warp_steady ? 1 : 0) << "\n"
                << "late_warp_steady_deg=" << d.late_warp_steady_deg << "\n"
                << "late_warp_steady_mm=" << d.late_warp_steady_mm << "\n"
                << "pitch_limit_up_deg=" << d.pitch_limit_up_deg << "\n"
                << "pitch_limit_down_deg=" << d.pitch_limit_down_deg << "\n"
                << "# 0 = smooth turning\n"
                << "snap_turn_deg=" << d.snap_turn_deg << "\n"
                << "smooth_turn_deg_per_s=" << d.smooth_turn_deg_per_s << "\n"
                << "pose_lag_frames=" << d.pose_lag_frames << "\n"
                << "camera_stale_ms=" << d.camera_stale_ms << "\n"
                << "# 1 = after a hitch longer than camera_stale_ms the view keeps its heading and "
                   "6DoF\n# centre (a duck through a hitch stays a duck); 0 = taken again from your "
                   "head\n"
                << "camera_gap_resume=" << (d.camera_gap_resume ? 1 : 0) << "\n"
                << "pair_max_age_ms=" << d.pair_max_age_ms << "\n"
                << "screen_distance_m=" << d.screen_distance_m << "\n"
                << "screen_width_m=" << d.screen_width_m << "\n"
                << "pose_prediction_ms=" << d.pose_prediction_ms << "\n"
                << "# headset refresh rate to request in Hz (the highest supported rate at or "
                   "below it); 0 = runtime default\n"
                << "refresh_hz=" << d.refresh_hz << "\n"
                << "# A close first ends OpenXR (the session, the instance) within this many ms "
                   "(0 = not)\n"
                << "xr_exit_teardown_ms=" << d.xr_exit_teardown_ms << "\n"
                << "# 1 = then the process ends at once once the play time, the shader cache, a "
                   "save being\n# written and the log are done (Windows: no DLL detach can hang "
                   "it); 0 = the old exit\n"
                << "xr_exit_terminate=" << (d.xr_exit_terminate ? 1 : 0) << "\n"
                << "# 1 = every headset refresh the stereo view is re-made for where your head "
                   "is then\n# (turning and leaning smooth at the headset's rate); 0 = the game's "
                   "frames as they are\n"
                << "late_warp=" << d.late_warp << "\n"
                << "# 1 = with late_warp, a game frame shown for 2-3 refreshes also moves on with "
                   "your walking\n# and turning in the game (your own body stays put); 0 = head "
                   "motion only\n"
                << "late_warp_motion=" << (d.late_warp_motion ? 1 : 0) << "\n"
                << "# with late_warp: depth edges that move apart by less than this many pixels "
                   "between the\n# game frame and your view are blended, not stepped (less edge "
                   "crawl); 0 = off\n"
                << "late_warp_edge_blend=" << d.late_warp_edge_blend << "\n"
                << "# 1 = the late warp's eye pass fetches each texel of its sharp filter once "
                   "(the same\n# image, less GPU per refresh)\n"
                << "late_warp_fetch_once=" << (d.late_warp_fetch_once ? 1 : 0) << "\n"
                << "# 1 = after a stutter the newest finished game frame goes up while the GPU "
                   "catches up\n# (no half-second freeze and lurch); 0 = wait for the newest one "
                   "(before)\n"
                << "stereo_adopt_finished=" << d.stereo_adopt_finished << "\n"
                << "# picture quality: anisotropic filtering forced on the game's textures (0 = "
                   "the game's own,\n# 2 / 4 / 8 / 16) and a mip bias for its tiling textures "
                   "(negative = sharper, more shimmer)\n"
                << "texture_aniso=" << d.texture_aniso << "\n"
                << "texture_lod_bias=" << d.texture_lod_bias << "\n"
                << "# anti-aliasing of the game frame the eyes are made from: smaa (SMAA 1x "
                   "in place of the\n# game's FXAA: crisper edges), fxaa (the game's own), taa "
                   "(temporal AA in the game\n# frame: a sub-pixel jitter and a reprojected "
                   "history, the steadiest edges in motion),\n# off (the game's AA off: the eyes' "
                   "temporal AA alone; vr.ini only)\n"
                << "aa_mode=" << Aa::Name(d.aa_mode) << "\n"
                << "# with aa_mode=taa: the jitter (0 .. 1 = +-half a render pixel), its cycle "
                   "(8 or 16),\n# the current frame's least share (the history's length: 1 / "
                   "it frames), the history's\n# clip box in sigma, the disocclusion test "
                   "(relative 1/z), the stable depth for the\n# DIBR (1 = on), the "
                   "sharpening of the result (0 .. 1), the current frame's filter (sigma\n# in "
                   "render px), the near field (m; 0 = off) and its least current-frame share\n# "
                   "(the weapon and hands), the eyes' TAA weight while it runs (-1 = eye_taa's "
                   "own),\n# the blend's luma weight (0 .. 1), the occluder test (1 = on), "
                   "late_warp_edge_blend and eye_sharpen while it runs (-1 = their own),\n"
                   "# the sharpening's lift below black for dark scenes (0 .. 0.5)\n"
                << "taa_jitter=" << d.taa_jitter << "\n"
                << "taa_phases=" << d.taa_phases << "\n"
                << "taa_alpha_min=" << d.taa_alpha_min << "\n"
                << "taa_clip_gamma=" << d.taa_clip_gamma << "\n"
                << "taa_depth_reject=" << d.taa_depth_reject << "\n"
                << "taa_stable_depth=" << (d.taa_stable_depth ? 1 : 0) << "\n"
                << "taa_sharpen=" << d.taa_sharpen << "\n"
                << "taa_filter_px=" << d.taa_filter_px << "\n"
                << "taa_near_m=" << d.taa_near_m << "\n"
                << "taa_near_alpha=" << d.taa_near_alpha << "\n"
                << "taa_eye_taa=" << d.taa_eye_taa << "\n"
                << "taa_luma_weight=" << d.taa_luma_weight << "\n"
                << "taa_occluder=" << (d.taa_occluder ? 1 : 0) << "\n"
                << "taa_edge_blend=" << d.taa_edge_blend << "\n"
                << "taa_eye_sharpen=" << d.taa_eye_sharpen << "\n"
                << "taa_sharpen_lift=" << d.taa_sharpen_lift << "\n"
                << "# the eyes enlarged with FSR 1 before the headset: auto (the runtime's "
                   "recommended size\n# = the Link app's render resolution), panel (to the "
                   "headset's own panel, Quest 3\n# 2064x2208 per eye, whatever the Link app's "
                   "render resolution), 1 = off, 1.1 .. 2 =\n# factor; then sharpened 0 (off) .. "
                   "1\n"
                << "eye_size="
                << (d.eye_size == EyeSize::Panel  ? std::string{"panel"}
                    : d.eye_size == EyeSize::Auto ? std::string{"auto"}
                                                  : fmt::format("{}", d.eye_upscale))
                << "\n"
                << "eye_sharpen=" << d.eye_sharpen << "\n"
                << "color_contrast=0\ncolor_saturation=0\ncolor_gamma=0\n"
                << "# with eye_size=auto: the eyes enlarged at least this much (never past the "
                   "panel), so a\n# large render still gets FSR's upscale, not only sharpening "
                   "(1 = off, 1 .. 2)\n"
                << "eye_size_min_factor=" << d.eye_size_min_factor << "\n"
                << "# the eyes' temporal anti-aliasing while you look around: how much of the "
                   "previous\n# refreshes each one keeps (0 = off, 0.5 .. 0.9)\n"
                << "eye_taa=" << d.eye_taa << "\n"
                << "# with eye_taa: 1 = it also follows the game camera (walking, smooth turns), "
                   "0 = only\n# while the world stands still\n"
                << "eye_taa_follow=" << (d.eye_taa_follow ? 1 : 0) << "\n"
                << "# with eye_taa_follow, while the world moves: a history sample nearer than "
                   "the surface is\n# dropped above this parallax (eye px per refresh), and depth "
                   "edges keep at most\n# eye_taa_edge_cap of history (1 = no cap)\n"
                << "eye_taa_gate_px=" << d.eye_taa_gate_px << "\n"
                << "eye_taa_edge_cap=" << d.eye_taa_edge_cap << "\n"
                << "# with eye_taa: each game frame a sub-pixel step off, so still views smooth "
                   "out too\n# (0 = off, 1 = +-half a render pixel)\n"
                << "eye_taa_jitter=" << d.eye_taa_jitter << "\n"
                << "# the game frame's GPU work goes to the GPU every this many ms of the frame "
                   "(the late\n# warp then never waits behind a whole frame); 0 = all at the "
                   "flip\n"
                << "gpu_submit_ms=" << d.gpu_submit_ms << "\n"
                << "# command-processor shortcuts, the image is the same (1 = on, 0 = off): the "
                   "pipeline\n# cache remembers each program's permutation for the same inputs, "
                   "the render\n# targets' descriptions by their registers, each shader's "
                   "texture bindings\n# by their T#s, and the copy shader's buffers while their "
                   "pages are unchanged\n"
                << "perf_spec_memo=" << (d.perf_spec_memo ? 1 : 0) << "\n"
                << "perf_rt_memo=" << (d.perf_rt_memo ? 1 : 0) << "\n"
                << "perf_bind_memo=" << (d.perf_bind_memo ? 1 : 0) << "\n"
                << "perf_copy_memo=" << (d.perf_copy_memo ? 1 : 0) << "\n"
                << "# GPU shortcuts, the image is the same (1 = on, 0 = off): a game image read "
                   "through a\n# buffer is copied into it again only after a write; the depth "
                   "target keeps one\n# view and layout when the game toggles depth writes\n"
                << "perf_resync_memo=" << (d.perf_resync_memo ? 1 : 0) << "\n"
                << "perf_depth_one_layout=" << (d.perf_depth_one_layout ? 1 : 0) << "\n"
                << "# the desktop window while the headset shows the game: present every Nth "
                   "frame (1 = every\n# frame, 2 = 30 Hz, 0 = none), showing the right or left "
                   "eye (right, left) or the flat\n# game frame (game)\n"
                << "desktop_mirror=" << d.desktop_mirror << "\n"
                << "desktop_mirror_view="
                << DesktopMirror::ViewName(static_cast<DesktopMirror::View>(d.desktop_mirror_view))
                << "\n"
                << "# 1 = the eyes go to the headset with their depth, so it can correct head "
                   "movement\n# too (positional timewarp; untested on Link: try it); 0 = "
                   "rotation only\n"
                << "xr_depth=" << (d.xr_depth ? 1 : 0) << "\n"
                << "# game frame rate cap for an even cadence on the headset: auto (120 Hz: 60, "
                   "40 when\n# 60 cannot be held; 90 Hz: 45; 72 Hz: none), off (the game's own "
                   "60), or fps\n"
                << "frame_cap="
                << (d.frame_cap < 0    ? std::string{"auto"}
                    : d.frame_cap == 0 ? std::string{"off"}
                                       : std::to_string(d.frame_cap))
                << "\n"
                << "# swing the right controller to attack\n"
                << "gestures=" << (d.gestures ? 1 : 0) << "\n"
                << "# big swing = heavy attack: start wound up (blade behind your head or "
                   "shoulder, or raised\n"
                << "# high / arm out wide - a soft click tells you) and sweep through a large "
                   "arc. 0 = every swing is light\n"
                << "heavy_arc=" << (d.heavy_arc ? 1 : 0) << "\n"
                << "# blade sweep in degrees for a heavy (seated uses 85%)\n"
                << "heavy_arc_deg=" << d.heavy_arc_deg << "\n"
                << "# how far back the blade must start (degrees from pointing forward) to count "
                   "as wound up\n"
                << "heavy_windup_deg=" << d.heavy_windup_deg << "\n"
                << "# 1 = playing seated (smaller heavy arc and wind-up)\n"
                << "seated=" << (d.seated ? 1 : 0) << "\n"
                << "# 1 = soft click when the weapon is wound up for a heavy\n"
                << "heavy_ready_tick=" << (d.heavy_ready_tick ? 1 : 0) << "\n"
                << "# blade angle relative to the controller grip; adjust if wind-ups are "
                   "misread\n"
                << "blade_pitch_deg=" << d.blade_pitch_deg << "\n"
                << "# overhead swings: a wind-up looped into its chop splits where the blade "
                   "turns back toward\n# the front by this many degrees (0 = off)\n"
                << "swing_windup_turn_deg=" << d.swing_windup_turn_deg << "\n"
                << "# a stroke whose blade comes this many degrees closer to the front is never a "
                   "wind-up (0 = off)\n"
                << "swing_toward_front_deg=" << d.swing_toward_front_deg << "\n"
                << "# 1 = a stroke ignored as a wind-up may still become the chop while it lasts\n"
                << "swing_windup_rejudge=" << (d.swing_windup_rejudge ? 1 : 0) << "\n"
                << "# the weapon hand turns swings off at your face only once slower than this "
                   "(m/s) there;\n# a swing passing your face is not touching the headset (0 = "
                   "any speed)\n"
                << "swing_face_enter_speed=" << d.swing_face_enter_speed << "\n"
                << "# motion = swing the right controller to attack; classic = buttons only.\n"
                << "# Hold both stick clicks for 1 s to switch in game.\n"
                << "controls=" << (d.controls_classic ? "classic" : "motion") << "\n"
                << "# holding the left Menu button (0.7 s): recenter = recenter the view (where "
                   "you face becomes\n# forward, your head the neutral position); screen = "
                   "toggle the virtual screen (the old\n# binding). Tab on the keyboard toggles "
                   "the virtual screen either way\n"
                << "menu_hold=" << (d.menu_hold_screen ? "screen" : "recenter") << "\n"
                << "# Y as the menu button (motion layout, left_scheme=2), wherever Menu would "
                   "be: a Y tap opens\n# the game's menu, Y held 0.7 s recenters the view, held "
                   "2.5 s on a flat screen brings VR\n# back (hold Y + B stays the gestures; in "
                   "the game's menus B alone goes back). auto = only\n# where the runtime owns "
                   "the Menu button (SteamVR: its dashboard); on (1, true, yes) =\n# everywhere; "
                   "off (0, false, no) = never (Y's tap is the quick-item arc, Menu the menu)\n"
                << "y_menu=" << VR::YMenu::SettingName(d.y_menu) << "\n"
                << "# 1 = the VR tile in the game's Options grid (right of System) opens the VR "
                   "settings page;\n# 0 = the game's own grid, nothing patched\n"
                << "vr_menu=" << d.vr_menu << "\n"
                << "# motion: transform = the right grip transforms the weapon (L1) and swings "
                   "attack on their own;\n"
                << "# draw = the old clutch (hold the right grip to swing, the left grip "
                   "transforms)\n"
                << "right_grip=" << (d.right_grip_draw ? "draw" : "transform") << "\n"
                << "# right_grip=draw: 1 = a quick right-grip tap toggles the drawn weapon\n"
                << "sticky_grip=" << (d.sticky_grip ? 1 : 0) << "\n"
                << "# the left hand: 2 = the stick click sprints (A leaps while sprinting), the "
                   "left grip opens the\n# quick items, the trigger takes / throws on its release "
                   "/ uses, the grip at the left hip draws a\n# Blood Vial, Y's tap is the arc and "
                   "held Y the modifier; 1 = the old one: Y opens the quick items,\n# the grip "
                   "takes, the trigger uses, the stick click's tap opens them\n"
                << "left_scheme=" << d.left_scheme << "\n"
                << "# swing = right trigger tap / + swing = heavy, hold = charge; button = R2\n"
                << "heavy=" << (d.heavy_button ? "button" : "swing") << "\n"
                << "# X arms a Blood Vial in the left hand; touch your right wrist or stab your "
                   "thigh/belly\n"
                << "# to inject it at once (no animation). 0 = X is the normal vial button\n"
                << "inject=" << (d.inject ? 1 : 0) << "\n"
                << "# 1 = allow injecting at full HP (the vial is wasted, as in the game)\n"
                << "inject_full_hp=" << (d.inject_full_hp ? 1 : 0) << "\n"
                << "# 1 = while an NPC is talking a swing does not attack (the trigger and "
                   "buttons still do)\n"
                << "talk_guard=" << (d.talk_guard ? 1 : 0) << "\n"
                << "# an armed vial disarms itself after this long (s); X disarms it any time, "
                   "0 = never\n"
                << "vial_timeout_s=" << d.vial_timeout_s << "\n"
                << "# the Blood Vial's stab zones (right arm, body, lap): how far they reach "
                   "beyond you, 0.5 .. 2\n"
                << "vial_zone_scale=" << d.vial_zone_scale << "\n"
                << "# 3D objects drawn on top of the game: the armed vial in your left hand, the "
                   "controllers\n"
                << "# (virtual screen, menus), ghost hands, the gun laser. 0 = none of them\n"
                << "overlay=" << (d.overlay ? 1 : 0) << "\n"
                << "# their image size (0.5 .. 1 of the headset's recommended size) and "
                   "anti-aliasing samples\n"
                << "overlay_scale=" << d.overlay_scale << "\n"
                << "overlay_msaa=" << d.overlay_msaa << "\n"
                << "overlay_brightness=" << d.overlay_brightness << "\n"
                << "# 1 = the armed Blood Vial is shown in your left hand\n"
                << "vial_visual=" << (d.vial_visual ? 1 : 0) << "\n"
                << "# where the vial sits in the left hand: offset in mm and rotation in degrees "
                   "(x,y,z)\n"
                << "vial_offset_mm=" << d.vial_offset_mm[0] << "," << d.vial_offset_mm[1] << ","
                << d.vial_offset_mm[2] << "\n"
                << "vial_rot_deg=" << d.vial_rot_deg[0] << "," << d.vial_rot_deg[1] << ","
                << d.vial_rot_deg[2] << "\n"
                << "# 1 = the hunter's fingers hide the vial where they wrap it\n"
                << "vial_holdout=" << (d.vial_holdout ? 1 : 0) << "\n"
                << "# game = the armed vial is the game's own, in the hunter's left hand (it\n"
                << "# follows his hand, his fingers hide it); overlay = a vial at your controller\n"
                << "vial_in_hand=" << (d.vial_in_hand_game ? "game" : "overlay") << "\n"
                << "# while the game's vial shows: 0 = keep the left weapon, 1 = hide it, 2 = hide "
                   "all its parts\n"
                << "vial_hide_left=" << d.vial_hide_left << "\n"
                << "# 1 = no vial during two-handed attacks\n"
                << "vial_hide_two_handed=" << (d.vial_hide_two_handed ? 1 : 0) << "\n"
                << "# the game's vial stays this long (s) after a stab\n"
                << "vial_inject_hold_s=" << d.vial_inject_hold_s << "\n"
                << "# a healed stab: fixed hand pause in milliseconds, independent of frame rate; "
                   "0 = off, followed by vial_inject_release_ms of easing; both = both hands, "
                   "vial = only the vial's\n"
                << "vial_inject_hold_ms=" << d.vial_inject_hold_ms << "\n"
                << "vial_inject_hold_hands=" << (d.vial_inject_hold_both ? "both" : "vial") << "\n"
                << "vial_inject_release_ms=" << d.vial_inject_release_ms << "\n"
                << "# a healed stab: 1 = the vial in his hand empties (its blood drops over "
                   "vial_inject_drain_ms, 50 .. 1000), then it goes away; 0 = it stays full\n"
                << "vial_inject_drain=" << (d.vial_inject_drain ? 1 : 0) << "\n"
                << "vial_inject_drain_ms=" << d.vial_inject_drain_ms << "\n"
                << "# then the empty vial stays this long (ms, 0 .. 1000) before it is put away\n"
                << "vial_inject_empty_ms=" << d.vial_inject_empty_ms << "\n"
                << "# 1 = a thin steel needle on the stabbing end of the vial in his hand (visual "
                   "only); 0 = none\n"
                << "vial_needle=" << (d.vial_needle ? 1 : 0) << "\n"
                << "# DIBR: 1 = the game's vial is given the depth of the hand (its glass has "
                   "none)\n"
                << "vial_dibr_proxy=" << (d.vial_dibr_proxy ? 1 : 0) << "\n"
                << "# controller models: 0 = never, 1 = on the virtual screen and in menus, 2 = "
                   "always\n"
                << "controller_models=" << d.controller_models << "\n"
                << "# 1 = a laser from the gun hand while you raise it to aim\n"
                << "laser=" << (d.laser ? 1 : 0) << "\n"
                << "# 1 = axes on both controllers (to check that the models sit in your hands)\n"
                << "overlay_debug=" << (d.overlay_debug ? 1 : 0) << "\n"
                << "# 1 = a soft green glow at the edges of the view whenever an item heals you\n"
                << "heal_vignette=" << (d.heal_vignette ? 1 : 0) << "\n"
                << "# its opacity at the edge of the view (0 .. 1) and colour (r,g,b 0 .. 1)\n"
                << "heal_vignette_strength=" << d.heal_vignette_strength << "\n"
                << "heal_vignette_color=" << d.heal_vignette_color[0] << ","
                << d.heal_vignette_color[1] << "," << d.heal_vignette_color[2] << "\n"
                << "# 1 = a small red flash at the edges of the view when a hit takes HP\n"
                << "hit_vignette=" << (d.hit_vignette ? 1 : 0) << "\n"
                << "# its opacity at the edge of the view (0 .. 1) and colour (r,g,b 0 .. 1)\n"
                << "hit_vignette_strength=" << d.hit_vignette_strength << "\n"
                << "hit_vignette_color=" << d.hit_vignette_color[0] << ","
                << d.hit_vignette_color[1] << "," << d.hit_vignette_color[2] << "\n"
                << "# how long the red flash takes to fade out (ms)\n"
                << "hit_vignette_fade_ms=" << d.hit_vignette_fade_ms << "\n"
                << "# where the red starts: clear inside this share of the view from its centre, "
                   "full at the\n# edge (0.3 .. 0.95; 0.464 = the wider red before 2026-10-09, "
                   "larger = a thinner rim)\n"
                << "hit_vignette_inner=" << d.hit_vignette_inner << "\n"
                << "# 1 = the red flash shows on the edge toward where the blow came from (ahead = "
                   "top, behind = bottom)\n"
                << "hit_vignette_directional=" << (d.hit_vignette_directional ? 1 : 0) << "\n"
                << "# 1 = the vignettes are drawn into the eyes by the late warp (no extra "
                   "full-view layer\n# for the headset to blend); 0 = on the VR overlay's layer\n"
                << "vignette_in_warp=" << (d.vignette_in_warp ? 1 : 0) << "\n"
                << "# 1 = the controller on the side the blow came from vibrates stronger\n"
                << "hit_haptics_directional=" << (d.hit_haptics_directional ? 1 : 0) << "\n"
                << "# 1 = the edges of the view darken while a blow staggers or knocks the hunter "
                   "down (dizzy),\n# clearing as you can act again\n"
                << "stagger_vignette=" << (d.stagger_vignette ? 1 : 0) << "\n"
                << "# its darkness (1 = 0.55 at the edge for a stagger, 0.75 for a knockdown; 0 .. "
                   "1.5)\n"
                << "stagger_vignette_strength=" << d.stagger_vignette_strength << "\n"
                << "# a clearing fades to a faint rest at once and holds it this long (ms), so a "
                   "blow that staggers\n# again inside it reads as one daze (no flashing clear and "
                   "dark); 0 = the full fade at once\n"
                << "stagger_vignette_merge_ms=" << d.stagger_vignette_merge_ms << "\n"
                << "# 1 = the stagger vignette and the red hit flash fitted to what the headset "
                   "shows (darker:\n# 0.75 / 0.9 at the visible edge); 0 = the old fixed "
                   "degrees (mostly beyond a Quest 3's view)\n"
                << "stagger_vignette_fit=" << (d.stagger_vignette_fit ? 1 : 0) << "\n"
                << "hit_vignette_fit=" << (d.hit_vignette_fit ? 1 : 0) << "\n"
                << "# a stagger's first moment is darker for this long (ms; 0 = none)\n"
                << "stagger_vignette_dip_ms=" << d.stagger_vignette_dip_ms << "\n"
                << "# the red hit flash over the stagger's black keeps this share at full "
                   "darkness (thinner,\n# gone within 220 ms: a black tunnel, the blow's side a "
                   "brief red rim); 1 = as before\n"
                << "stagger_vignette_red=" << d.stagger_vignette_red << "\n"
                << "# the red hit flash held while a hit's stagger refuses your swings, gone as "
                   "the game takes\n# attacks again: 1 = a light stagger red, a heavy one black "
                   "with a red rim; 2 = every\n# stagger red (black only for knockdowns); 0 = "
                   "off (the black tunnel, a brief red flash)\n"
                << "hitstun_vignette=" << d.hitstun_vignette << "\n"
                << "# the held red's opacity (1 = the hit flash's own peak; 0 .. 1.5)\n"
                << "hitstun_vignette_strength=" << d.hitstun_vignette_strength << "\n"
                << "# the hunter's hit staggers end this much sooner (ms; played faster from "
                   "their start;\n# 0 = the game's)\n"
                << "stagger_shorten_ms=" << d.stagger_shorten_ms << "\n"
                << "# immersive vitals: wrist = the Blood Gauge on the left wrist (HP, rally, "
                   "stamina, bullets),\n# the game's HP cluster only when you look down; "
                   "classic = the game's HUD as before\n"
                << "shoulder_switch=" << d.shoulder_switch << "\n"
                << "health_color=" << d.health_color << "\n"
                << "stamina_color=" << d.stamina_color << "\n"
                << "health_color_strength=" << d.health_color_strength << "\n"
                << "stamina_color_strength=" << d.stamina_color_strength << "\n"
                << "hud_vitals=" << (d.hud_vitals == 0 ? "classic" : "wrist") << "\n"
                << "# the gauge: size, centre up the forearm from the wrist (m), stand-off above "
                   "the measured\n# sleeve (m), depth pull (m: the sleeve never hides it)\n"
                << "vitals_gauge_scale=" << d.vitals_gauge_scale << "\n"
                << "vitals_gauge_from_wrist_m=" << d.vitals_gauge_from_wrist_m << "\n"
                << "vitals_gauge_standoff_m=" << d.vitals_gauge_standoff_m << "\n"
                << "vitals_gauge_pull_m=" << d.vitals_gauge_pull_m << "\n"
                << "# low HP (wrist): a heartbeat in the left controller below vitals_low_hp of "
                   "max HP (quiet\n# vitals_heartbeat_mute_s after the last HP lost) and the "
                   "edges of the view darkening below 60%\n"
                << "vitals_heartbeat=" << (d.vitals_heartbeat ? 1 : 0) << "\n"
                << "vitals_low_hp=" << d.vitals_low_hp << "\n"
                << "vitals_heartbeat_mute_s=" << d.vitals_heartbeat_mute_s << "\n"
                << "vitals_heartbeat_gain=" << d.vitals_heartbeat_gain << "\n"
                << "vitals_edge=" << (d.vitals_edge ? 1 : 0) << "\n"
                << "vitals_edge_strength=" << d.vitals_edge_strength << "\n"
                << "haptic_gain=" << d.haptic_gain << "\n"
                << "# the weakest vibration tick (0 .. 0.8) and the shortest one (ms)\n"
                << "haptic_floor=" << d.haptic_floor << "\n"
                << "haptic_min_ms=" << d.haptic_min_ms << "\n"
                << "# 1 = a test buzz (left, then right controller) when gameplay starts\n"
                << "haptic_startup_test=" << (d.haptic_startup_test ? 1 : 0) << "\n"
                << "# combat vibration strengths (0 = none .. 1.5): a melee hit's impact, the "
                   "knock when you are hit, a bullet / a thrown item hitting, the injection\n"
                << "haptic_melee_hit=" << d.haptic_melee_hit << "\n"
                << "# a landed melee hit: full strength this long (ms, 0 = the old short crack); "
                   "1 = a heavy hit adds a second blow (the crunch)\n"
                << "haptic_melee_hit_ms=" << d.haptic_melee_hit_ms << "\n"
                << "haptic_melee_crunch=" << (d.haptic_melee_crunch ? 1 : 0) << "\n"
                << "haptic_hit_taken=" << d.haptic_hit_taken << "\n"
                << "haptic_gun_hit=" << d.haptic_gun_hit << "\n"
                << "haptic_throw_hit=" << d.haptic_throw_hit << "\n"
                << "haptic_inject=" << d.haptic_inject << "\n"
                << "# the game's own rumble for a blow on you x this (0 .. 1; 1 = as the game "
                   "sends it)\n"
                << "haptic_hit_rumble=" << d.haptic_hit_rumble << "\n"
                << "# HUD and menus become a panel of this size (0.0625 .. 1) at this depth\n"
                << "hud_scale=" << d.hud_scale << "\n"
                << "# in-game menus (inventory, notes, shops): menu controls (both sticks, A/X "
                   "confirm,\n"
                << "# B/Y back) while a menu is open\n"
                << "menu_mode=" << (d.menu_mode ? 1 : 0) << "\n"
                << "# 1 = menus on a sharp panel fixed where you looked when they opened (the "
                   "world around it\n"
                << "# frozen and dimmed); 0 = menus on the HUD panel at menu_hud_scale\n"
                << "menu_panel=" << (d.menu_panel ? 1 : 0) << "\n"
                << "# panel width in degrees, distance (m), brightness of the world around it (1 = "
                   "no dim)\n"
                << "menu_width_deg=" << d.menu_width_deg << "\n"
                << "menu_distance_m=" << d.menu_distance_m << "\n"
                << "menu_dim=" << d.menu_dim << "\n"
                << "# 1 = curved panel (same text size at the edges)\n"
                << "menu_curved=" << (d.menu_curved ? 1 : 0) << "\n"
                << "# looking this far (deg) away from the panel for 1 s brings it to you (0 = "
                   "never)\n"
                << "menu_follow_deg=" << d.menu_follow_deg << "\n"
                << "# small dialogs in the middle of the screen (the game's OK boxes) are shown "
                   "this much\n"
                << "# larger on the panel (1 = the same size as menus)\n"
                << "menu_dialog_zoom=" << d.menu_dialog_zoom << "\n"
                << "# the menus that are such dialogs (names from the log's 'VR menu: opening' "
                   "lines, comma-separated)\n"
                << "menu_zoom_menus=" << d.menu_zoom_menus << "\n"
                << "menu_hud_scale=" << d.menu_hud_scale << "\n"
                << "hud_depth_m=" << d.hud_depth_m << "\n"
                << "# the game's UI on its own layers, sharp over the live stereo world: menus "
                   "on a canvas fixed\n"
                << "# where you looked, the HUD on a band low in the view (hud_scale then no "
                   "longer applies to it).\n"
                << "# 0 = off (the HUD panel above, menus on the frozen panel), 1 = on, 2 = check "
                   "only\n"
                << "ui_layer=" << d.ui_layer << "\n"
                << "# where menus go: layer (the UI layer's canvas), frozen (the menu panel over a "
                   "frozen,\n"
                << "# dimmed world), hud (the HUD panel)\n"
                << "menu_present=layer\n"
                << "# menu canvas distance (m) and width (deg; text = width / 80 deg), small "
                   "dialogs' text size (deg)\n"
                << "ui_menu_distance_m=" << d.ui_menu_distance_m << "\n"
                << "ui_menu_width_deg=" << d.ui_menu_width_deg << "\n"
                << "ui_dialog_em_deg=" << d.ui_dialog_em_deg << "\n"
                << "# small dialogs and notes: the box at most this wide (deg; 0 = no cap)\n"
                << "ui_dialog_max_deg=" << d.ui_dialog_max_deg << "\n"
                << "# 1 = full menus cropped to their content and shown as large as fits (the "
                   "dialogs' text size\n"
                << "# at most, never smaller than the whole menu; when > 0 at most this wide and "
                   "tall, deg); 0 = the\n# whole menu\n"
                << "ui_menu_fit=" << (d.ui_menu_fit ? 1 : 0) << "\n"
                << "ui_menu_max_width_deg=" << d.ui_menu_max_width_deg << "\n"
                << "ui_menu_max_height_deg=" << d.ui_menu_max_height_deg << "\n"
                << "# 0 = a screen-wide dim over a menu (the shop's quantity box, a fade) does "
                   "not grow its crop; 1 = it does (the old way)\n"
                << "ui_menu_fit_screen_wide=" << (d.ui_menu_fit_screen_wide ? 1 : 0) << "\n"
                << "# 1 = a page over the whole screen (Status from the Options grid) grows "
                   "the crop; 0 = kept like a dim\n"
                << "ui_menu_page=" << (d.ui_menu_page ? 1 : 0) << "\n"
                << "# 1 = a menu's new page is shown at its size and place at once; 0 = eased "
                   "(zooms into place)\n"
                << "ui_menu_snap=" << (d.ui_menu_snap ? 1 : 0) << "\n"
                << "# the headstone's travel list: this many degrees below (-) where you look "
                   "(its entries are at the top of a tall panel); 0 = centred\n"
                << "ui_travel_pitch_deg=" << d.ui_travel_pitch_deg << "\n"
                << "# every menu, dialog and note keeps its corners within this many degrees of "
                   "its centre\n"
                << "# (across / up and down): inside the view both eyes share, with a margin\n"
                << "ui_menu_corner_yaw_deg=" << d.ui_menu_corner_yaw_deg << "\n"
                << "ui_menu_corner_pitch_deg=" << d.ui_menu_corner_pitch_deg << "\n"
                << "# 1 = curved canvas and HUD band\n"
                << "ui_curved=" << (d.ui_curved ? 1 : 0) << "\n"
                << "# HUD: belt (HP, items, echoes, prompts as pieces of their own) or game "
                   "(where the game\n"
                << "# draws it, at hud_scale)\n"
                << "ui_hud_layout=belt\n"
                << "# the belt is fixed to: view (HP and items in the view's lower-left corner, "
                   "echoes lower\n"
                << "# right, prompts lower middle, as on the flat screen) or band (a band that "
                   "follows your\n"
                << "# head lazily)\n"
                << "ui_hud_anchor=view\n"
                << "# view: the HUD's outer edges this far left / right of the view's centre and "
                   "its rows this\n"
                << "# far below (-) it (deg)\n"
                << "ui_hud_corner_yaw_deg=" << d.ui_hud_corner_yaw_deg << "\n"
                << "ui_hud_corner_pitch_deg=" << d.ui_hud_corner_pitch_deg << "\n"
                << "# HUD distance (m), size (deg per 1080p pixel: 0.042 = text 1 deg), height (+ "
                   "= up, deg)\n"
                << "ui_hud_distance_m=" << d.ui_hud_distance_m << "\n"
                << "ui_hud_scale_deg=" << d.ui_hud_scale_deg << "\n"
                << "ui_hud_pitch_deg=" << d.ui_hud_pitch_deg << "\n"
                << "# band: it follows your head beyond these angles (deg: sideways, up, down) "
                   "with this time\n# constant (ms)\n"
                << "ui_hud_follow_deg=" << d.ui_hud_follow_deg << "\n"
                << "ui_hud_follow_pitch_deg=" << d.ui_hud_follow_pitch_deg << "\n"
                << "ui_hud_follow_pitch_down_deg=" << d.ui_hud_follow_pitch_down_deg << "\n"
                << "ui_hud_follow_ms=" << d.ui_hud_follow_ms << "\n"
                << "# 1 = a HUD piece fades while a hand, the arm or the weapon is in front of "
                   "it\n"
                << "ui_hand_fade=" << (d.ui_hand_fade ? 1 : 0) << "\n"
                << "# ... to this opacity when only the blade is in front of it\n"
                << "ui_hud_blade_alpha=" << d.ui_hud_blade_alpha << "\n"
                << "# ... but the action prompts, pickups and subtitles never below this for "
                   "your hands, arms or body\n# (1 = they do not fade for them; the blade "
                   "and the gun still fade them)\n"
                << "ui_hud_lower_alpha=" << d.ui_hud_lower_alpha << "\n"
                << "# echoes / insight counters in gameplay: look_down (shown while you look "
                   "down more than\n# ui_counters_pitch_deg below the horizon), always or "
                   "hidden (menus always show them)\n"
                << "ui_counters="
                << (d.ui_counters == 1   ? "always"
                    : d.ui_counters == 2 ? "hidden"
                                         : "look_down")
                << "\n"
                << "ui_counters_pitch_deg=" << d.ui_counters_pitch_deg << "\n"
                << "# ... where they appear looking down: lower_middle (the tray, below the "
                   "view's centre),\n# corner (their HUD corner) or raised (right of the "
                   "centre, ui_counters_raise_deg higher)\n"
                << "ui_counters_pos="
                << (d.ui_counters_pos == 1   ? "corner"
                    : d.ui_counters_pos == 2 ? "raised"
                                             : "lower_middle")
                << "\n"
                << "# the tray: its top this far below the view's centre (deg), its size, the "
                   "gap between its clusters (deg),\n# 1 = one column (the HP cluster over the "
                   "counters), 0 = side by side\n"
                << "ui_counters_tray_top_deg=" << d.ui_counters_tray_top_deg << "\n"
                << "ui_counters_scale=" << d.ui_counters_scale << "\n"
                << "ui_counters_gap_deg=" << d.ui_counters_gap_deg << "\n"
                << "ui_counters_tray_stack=" << (d.ui_counters_tray_stack ? 1 : 0) << "\n"
                << "# ui_counters_pos=raised: this much higher than their corner (deg)\n"
                << "ui_counters_raise_deg=" << d.ui_counters_raise_deg << "\n"
                << "# HP / stamina / items cluster in gameplay: auto (look_down with "
                   "hud_vitals=wrist, else always),\n# look_down (with the counters, in the "
                   "tray), always or hidden\n"
                << "ui_status_left="
                << (d.ui_status_left == 0   ? "look_down"
                    : d.ui_status_left == 1 ? "always"
                    : d.ui_status_left == 2 ? "hidden"
                                            : "auto")
                << "\n"
                << "# 1 = HP, stamina, items and echoes shown under a full menu (the game does "
                   "not pause)\n"
                << "ui_menu_hud=" << (d.ui_menu_hud ? 1 : 0) << "\n"
                << "# 1 = that HUD only under the menu, never inside its canvas (no stray item "
                   "name in a large menu's crop); 0 = the old way\n"
                << "ui_menu_hud_off_canvas=" << (d.ui_menu_hud_off_canvas ? 1 : 0) << "\n"
                << "# lock-on markers, enemy tags, banners: scale (0 = auto: lined up with the "
                   "world on the belt)\n"
                << "# and distance (m, 0 = auto: 3 m on the belt)\n"
                << "ui_rest_scale=" << d.ui_rest_scale << "\n"
                << "ui_rest_depth_m=" << d.ui_rest_depth_m << "\n"
                << "# ... on a curve around you, so enemy tags face you as you turn (0 = flat)\n"
                << "ui_rest_curved=" << (d.ui_rest_curved ? 1 : 0) << "\n"
                << "# the lock-on marker (the glow on the locked-on target): its size and "
                   "brightness (1 / 1 = as the game draws it)\n"
                << "ui_lock_marker_scale=" << d.ui_lock_marker_scale << "\n"
                << "ui_lock_marker_alpha=" << d.ui_lock_marker_alpha << "\n"
                << "# the bars over the enemies you hit or lock on to (and their damage "
                   "numbers): 1 = shown, 0 = hidden\n# (the boss bar: ui_boss_hp_bar)\n"
                << "ui_enemy_hp_bars=" << (d.ui_enemy_hp_bars ? 1 : 0) << "\n"
                << "# the boss's health bar: 0 = hidden, 1 = shown near the top of the view\n"
                << "ui_boss_hp_bar=" << (d.ui_boss_hp_bar ? 1 : 0) << "\n"
                << "# the game's button icons as the VR controllers' (cross A, circle B, square "
                   "LG, triangle RG,\n# L1 LT, R1 RT, OPTIONS Menu; the action prompts: cross B, "
                   "circle A, triangle X);\n# 0 = the DualShock 4's (at the start)\n"
                << "ui_vr_buttons=" << (d.ui_vr_buttons ? 1 : 0) << "\n"
                << "# the status build-up meter (poison, bleed, frenzy, ...): view = a small "
                   "piece under the view's centre\n# while a build-up shows, game = where the "
                   "game draws it (top centre, out of view in VR)\n"
                << "ui_conditions=" << (d.ui_conditions == 1 ? "game" : "view") << "\n"
                << "# ... its first row this many degrees from the view's centre (+ = up), its "
                   "size x the HUD's\n"
                << "ui_conditions_pitch_deg=" << d.ui_conditions_pitch_deg << "\n"
                << "ui_conditions_scale=" << d.ui_conditions_scale << "\n"
                << "# poison, rapid poison, bleed and frenzy build-ups drain this many percent "
                   "faster (0 = the game's rate;\n# only while falling, never beasthood, never "
                   "enemies)\n"
                << "status_drain_pct=" << d.status_drain_pct << "\n"
                << "# YOU DIED as a title fixed where you looked: distance (m, 0 = off: lined up "
                   "with the eyes),\n# its centre's pitch from the horizon and its width "
                   "(degrees)\n"
                << "ui_death_distance_m=" << d.ui_death_distance_m << "\n"
                << "ui_death_pitch_deg=" << d.ui_death_pitch_deg << "\n"
                << "ui_death_text_deg=" << d.ui_death_text_deg << "\n"
                << "# 1 = every banner (PREY SLAUGHTERED, the echoes regained, ...) as that title; "
                   "0 = only YOU DIED\n"
                << "ui_banner_title=" << (d.ui_banner_title ? 1 : 0) << "\n"
                << "# the banners' size: the widest (PREY SLAUGHTERED) this many degrees across\n"
                << "ui_banner_text_deg=" << d.ui_banner_text_deg << "\n"
                << "# 1 = the game's menu dim and fades darken the world itself\n"
                << "ui_veil=" << (d.ui_veil ? 1 : 0) << "\n"
                << "# 1 = head turns only the view (comfortable); 0 = head swings the orbit\n"
                << "decoupled=" << (d.decoupled ? 1 : 0) << "\n"
                << "orbit_pitch_deg=" << d.orbit_pitch_deg << "\n"
                << "# smooths camera height on stairs, slopes and landings (ms, 0 = off, max "
                   "300)\n"
                << "vertical_smoothing_ms=" << d.vertical_smoothing_ms << "\n"
                << "# first person: the view travels with the body when you dodge; height "
                   "changes from\n"
                << "# stairs/falls are smoothed (0 = old camera)\n"
                << "dodge_camera=" << (d.dodge_camera ? 1 : 0) << "\n"
                << "# horizontal follow of the body (ms, 0 = rigid; 40 = softer)\n"
                << "follow_smoothing_ms=" << d.follow_smoothing_ms << "\n"
                << "# both eyes of a stereo pair use one body position (correct depth while "
                   "moving)\n"
                << "pair_lock_position=" << (d.pair_lock_position ? 1 : 0) << "\n"
                << "# height smoothing during a dodge (ms)\n"
                << "dodge_vertical_ms=" << d.dodge_vertical_ms << "\n"
                << "# multiplies vertical_smoothing_ms while you stand still (0.1 .. 1)\n"
                << "vertical_stop_factor=" << d.vertical_stop_factor << "\n"
                << "# height smoothing while falling and landing (ms)\n"
                << "fall_vertical_ms=" << d.fall_vertical_ms << "\n"
                << "# jump_height_scale: fraction of actual animated lift (1 = full, 0 = off)\n"
                << "# dodge_dip_m: roll duck in metres; 0 disables, sidesteps/backsteps stay level\n"
                << "jump_height_scale=" << d.jump_height_scale << "\n"
                << "dodge_dip_m=" << d.dodge_dip_m << "\n"
                << "# A+B (motion controls): tap, or hold while moving = the game's roll toward "
                   "the stick\n"
                << "# (centred: back); hold 1.2 s standing still = the weapon grip adjust; 0 = "
                   "A and B as before\n"
                << "roll_chord=" << (d.roll_chord ? 1 : 0) << "\n"
                << "# experimental, standing only (motion controls): a quick sideways or "
                   "backward lean or\n"
                << "# step of your head dodges that way (1 = on)\n"
                << "lean_dodge=" << (d.lean_dodge ? 1 : 0) << "\n"
                << "# head speed (m/s) and head travel within 250 ms (m) that count as a lean\n"
                << "lean_dodge_speed=" << d.lean_dodge_speed << "\n"
                << "lean_dodge_dist_m=" << d.lean_dodge_dist_m << "\n"
                << "# shortest time between two lean dodges (ms)\n"
                << "lean_dodge_cooldown_ms=" << d.lean_dodge_cooldown_ms << "\n"
                << "# physical evasion: your real lean, step and duck move the hunter's "
                   "hurtboxes\n"
                << "# (no dodge animation, no i-frames); turns lean_dodge off\n"
                << "physical_evasion=" << (d.physical_evasion ? 1 : 0) << "\n"
                << "# hurtbox offset per metre of lean (a little extra for the game's coarse "
                   "hits)\n"
                << "evasion_gain=" << d.evasion_gain << "\n"
                << "# horizontal lean cap (m), the ankle-pivot lean range (m), the duck cap (m; "
                   "at most\n# positional_limit_down_m)\n"
                << "evasion_max_m=" << d.evasion_max_m << "\n"
                << "evasion_lean_m=" << d.evasion_lean_m << "\n"
                << "evasion_duck_m=" << d.evasion_duck_m << "\n"
                << "# the hurtboxes never go into walls (1 = on), the gap they keep (m) and how "
                   "fast\n# a wall's limit lets go (m/s)\n"
                << "evasion_wall=" << (d.evasion_wall ? 1 : 0) << "\n"
                << "evasion_wall_margin_m=" << d.evasion_wall_margin_m << "\n"
                << "evasion_wall_rise_mps=" << d.evasion_wall_rise_mps << "\n"
                << "# a lean counts fully this long (ms), then fades with this time constant "
                   "(0 = no limit)\n"
                << "evasion_hold_ms=" << d.evasion_hold_ms << "\n"
                << "evasion_hold_tau_ms=" << d.evasion_hold_tau_ms << "\n"
                << "# small drift held still this long (ms) slowly stops counting (0 = never "
                   "settle at all)\n"
                << "evasion_settle_ms=" << d.evasion_settle_ms << "\n"
                << "evasion_settle_tau_ms=" << d.evasion_settle_tau_ms << "\n"
                << "evasion_pelvis=" << (d.evasion_pelvis ? 1 : 0) << "\n"
                << "# haptics: a faint knock when a wall stops your lean, a whoosh when a blow "
                   "misses\n# you because you leaned (1 = on)\n"
                << "evasion_feedback=" << (d.evasion_feedback ? 1 : 0) << "\n"
                << "# extra max HP for the hunter in % (makes VR a little easier), 0 = vanilla\n"
                << "player_hp_bonus_pct=" << d.player_hp_bonus_pct << "\n"
                << "# extra damage in % for the left-hand weapon (gun etc.), 0 = vanilla\n"
                << "offhand_damage_pct=" << d.offhand_damage_pct << "\n"
                << "# 1 = an enemy your blade kills goes limp at once (its death animation cut "
                   "short) if its death ends in the\n# ragdoll; bullets, fire bombs, the left hand, "
                   "viscerals and backstabs keep the death animation; 0 = always the game's\n"
                << "melee_kill_ragdoll=" << (d.melee_kill_ragdoll ? 1 : 0) << "\n"
                << "# the push your blade gives that ragdoll (1 = the tuned default, 0 = none, "
                   "up to 3)\n"
                << "melee_ragdoll_impulse=" << d.melee_ragdoll_impulse << "\n"
                << "# the chance in % that such a kill plays the game's own death animation "
                   "anyway (and its ragdoll at the end,\n# no push): one roll per kill; 22 = "
                   "about one in five (the default), 0 = always limp at once, 100 = never\n"
                << "melee_ragdoll_vanilla_pct=" << d.melee_ragdoll_vanilla_pct << "\n"
                << "# extra invincibility after a quickstep's or roll's own i-frames, in 1/30 s "
                   "frames (0 .. 10; backsteps have none), 0 = vanilla\n"
                << "dodge_iframes_extra=" << d.dodge_iframes_extra << "\n"
                << "# 1 = a swing or a gun shot works at any moment of a dodge (the dodge goes on), "
                   "0 = they wait for the dodge's attack window\n# (needs arm_keep_on_dodge=1: "
                   "a dodge that takes the arms takes no attack)\n"
                << "dodge_attack=" << (d.dodge_attack ? 1 : 0) << "\n"
                << "# 0 = attacking during a dodge ends its invincibility at once (fair), 1 = it "
                   "stays (pure fun)\n"
                << "dodge_attack_iframes=" << (d.dodge_attack_iframes ? 1 : 0) << "\n"
                << "# flat screen (the game's own camera) during scripted full-body animations\n"
                << "cinematic=" << (d.cinematic ? 1 : 0) << "\n"
                << "# your viscerals, ripostes, backstabs\n"
                << "cinematic_visceral=" << (d.cinematic_visceral ? 1 : 0) << "\n"
                << "# enemy grabs (a fatal one returns to VR at the death, see "
                   "cinematic_death_hold)\n"
                << "cinematic_grabbed=" << (d.cinematic_grabbed ? 1 : 0) << "\n"
                << "# enemy grabs in VR, following the animated body (0 = existing behavior)\n"
                << "vr_grab_attacks=" << (d.vr_grab_attacks ? 1 : 0) << "\n"
                << "# doors, gates, levers, elevators, chests, coffins\n"
                << "cinematic_objects=" << (d.cinematic_objects ? 1 : 0) << "\n"
                << "# lighting/using lamps\n"
                << "cinematic_lamps=" << (d.cinematic_lamps ? 1 : 0) << "\n"
                << "# the game's pickup animation (only with pickup_instant=0; short; 0 if it "
                   "flickers)\n"
                << "cinematic_pickup=" << (d.cinematic_pickup ? 1 : 0) << "\n"
                << "# offline lamp rest / travel menu (0 = vanilla lamp menu)\n"
                << "lamp_travel=1\n"
                << "# grab and pull supported levers with either hand (0 = original interaction)\n"
                << "physical_levers=1\n"
                << "# item pickups in first-person VR: the game's 'picked up' pop-up at once, no "
                   "pickup animation (0 = the game's animation)\n"
                << "pickup_instant=" << (d.pickup_instant ? 1 : 0) << "\n"
                << "# NPC talk, scripted event animations, revival\n"
                << "cinematic_scripted=" << (d.cinematic_scripted ? 1 : 0) << "\n"
                << "# a canned item use from the quick-item arc (the Shaman Bone Blade's stab): "
                   "flat while the game plays it, back at its cancel window\n"
                << "cinematic_item=" << (d.cinematic_item ? 1 : 0) << "\n"
                << "# the Doll's Level Up: 0 = the hunter's kneel stays in first-person VR and "
                   "Level Up is on the menu panel; 1 = flat (the game's camera, the menu on the "
                   "virtual screen)\n"
                << "cinematic_levelup=" << (d.cinematic_levelup ? 1 : 0) << "\n"
                << "# fade to black when the flat screen comes (ms, 0 = hard cut)\n"
                << "cinematic_fade_ms=" << d.cinematic_fade_ms << "\n"
                << "# the way back: the screen cuts to black, first-person VR fades in (ms, 0 = "
                   "hard cut; -1 = the old way: the screen dims over cinematic_fade_ms first)\n"
                << "cinematic_return_fade_ms=" << d.cinematic_return_fade_ms << "\n"
                << "# back to VR as soon as the game hands control back (its cancel window: the "
                   "grab's get-up), not at the end of the animation; 0 = at its end\n"
                << "cinematic_release=" << (d.cinematic_release ? 1 : 0) << "\n"
                << "# the cancel window open, or all signals off, this long before returning (ms)\n"
                << "cinematic_exit_ms=" << d.cinematic_exit_ms << "\n"
                << "# shortest flat period (ms)\n"
                << "cinematic_min_ms=" << d.cinematic_min_ms << "\n"
                << "# 0 = a death ends the flat view at once (YOU DIED in VR), 1 = a fatal grab "
                   "stays flat while HP is 0 (until respawn)\n"
                << "cinematic_death_hold=" << (d.cinematic_death_hold ? 1 : 0) << "\n"
                << "# a throw state this old (s) is ignored until it clears\n"
                << "cinematic_throw_timeout_s=" << d.cinematic_throw_timeout_s << "\n"
                << "# flat screen while climbing long ladders; getting on and off stay in VR\n"
                << "cinematic_ladder=" << (d.cinematic_ladder ? 1 : 0) << "\n"
                << "# climbing this long after getting on before going flat (ms)\n"
                << "cinematic_ladder_enter_delay_ms=" << d.cinematic_ladder_enter_delay_ms << "\n"
                << "# ladders whose predicted flat time is shorter stay in VR (ms)\n"
                << "cinematic_ladder_min_flat_ms=" << d.cinematic_ladder_min_flat_ms << "\n"
                << "# back to VR this long before the predicted dismount (ms, 0 = at it)\n"
                << "cinematic_ladder_exit_lead_ms=" << d.cinematic_ladder_exit_lead_ms << "\n"
                << "# no flat climb sooner than this after returning (ms)\n"
                << "cinematic_ladder_reenter_ms=" << d.cinematic_ladder_reenter_ms << "\n"
                << "# fade of ladder transitions (ms, 0 = hard cut)\n"
                << "cinematic_ladder_fade_ms=" << d.cinematic_ladder_fade_ms << "\n"
                << "# an attack, damage or a blocked climb on a ladder returns to VR\n"
                << "cinematic_ladder_combat_exit=" << (d.cinematic_ladder_combat_exit ? 1 : 0)
                << "\n"
                << "# an HP drop on a ladder is damage from this % of max HP (poison stays "
                   "below; 0 = any drop)\n"
                << "cinematic_ladder_damage_pct=" << d.cinematic_ladder_damage_pct << "\n"
                << "# vertex shader hash of the last scene pass; later draws are HUD/menus\n"
                << "hud_after_vs=0x" << std::hex << d.hud_after_vs << std::dec << "\n"
                << "# fragment shader hash of the game's FXAA draw (aa_mode)\n"
                << "aa_fxaa_fs=0x" << std::hex << d.aa_fxaa_fs << std::dec << "\n";
            Body::WriteBodyDefaults(out);
            Quick::WriteQuickDefaults(out);
            Gun::WriteGunDefaults(out);
            Melee::WriteMeleeDefaults(out);
            Transform::WriteTransformDefaults(out);
            PoseGesture::WritePoseDefaults(out);
            Whip::WriteWhipDefaults(out);
            TwoHand::WriteTwoHandDefaults(out);
            Sound::WriteAudioDefaults(out);
            Bow::WriteBowDefaults(out);
            Torch::WriteTorchDefaults(out);
            Tricks::WriteTrickDefaults(out);
            BladeSound::WriteBladeSoundDefaults(out);
        }
        LOG_INFO(Core, "VR: wrote default settings to {}", Common::FS::PathToUTF8String(path));
        return;
    }
    std::string line;
    while (std::getline(file, line)) {
        // `key = value` with spaces (or a CR) around either side reads like `key=value`.
        const auto kv = Ini::ParseLine(line);
        if (!kv) {
            continue;
        }
        try {
            ParseSettingsLine(S().settings, kv->key, kv->value);
        } catch (const std::exception&) {
            LOG_WARNING(Core, "VR: bad setting line '{}'", line);
        }
    }
    auto& loaded = S().settings;
    if (loaded.stereo_mode == 1 && !loaded.dibr_seen) {
        // One time: settings written before depth-based stereo existed start in it (both eyes
        // from every game frame). AER stays one Menu + left stick press away, and a stereo_mode=1
        // set after this is kept.
        loaded.stereo_mode = 2;
        loaded.dibr_seen = true;
        SaveSetting("stereo_mode", "2");
        SaveSetting("dibr_seen", "1");
        LOG_INFO(Core, "VR: stereo_mode 1 -> 2 (DIBR is the new default; Menu + left stick "
                       "switches back)");
    }
    if (loaded.physical_evasion && loaded.lean_dodge) {
        // A quick lean would both roll and move the hurtboxes.
        loaded.lean_dodge = false;
        LOG_WARNING(Core, "VR: lean_dodge=1 is ignored while physical_evasion=1 (leaning moves "
                          "your hurtboxes instead of dodging)");
    }
    if (loaded.camera_roll) {
        LOG_INFO(Core, "VR: camera_roll=1: the render camera rolls with your head");
    }
    PublishStereoSettings();
    LOG_INFO(Core, "VR: loaded settings from {}", Common::FS::PathToUTF8String(path));
}

const char* UiClassName(UiClass cls) {
    switch (cls) {
    case UiClass::None:
        return "none";
    case UiClass::Full:
        return "menu";
    case UiClass::Dialog:
        return "dialog";
    case UiClass::Note:
        return "note";
    }
    return "?";
}

void PublishGameMenu(bool open, float zoom, UiClass cls, float pitch_deg, s32 grid_cell) {
    auto& st = S();
    const s64 now = NowNs();
    st.game_menu.store(open, std::memory_order_relaxed);
    st.game_menu_zoom.store(open ? std::clamp(zoom, 1.0f, 3.0f) : 1.0f, std::memory_order_relaxed);
    st.game_menu_pitch.store(open ? std::clamp(pitch_deg, -30.0f, 30.0f) : 0.0f,
                             std::memory_order_relaxed);
    st.game_menu_class.store(static_cast<u8>(open ? cls : UiClass::None),
                             std::memory_order_relaxed);
    st.game_menu_grid.store(open ? std::max(grid_cell, -2) : -2, std::memory_order_relaxed);
    if (open) {
        st.game_menu_open_ns.store(now, std::memory_order_relaxed);
    }
    st.game_menu_ns.store(now, std::memory_order_release);
}

namespace {
constexpr s64 TalkStuckNs = 60'000'000'000; // a line flag on this long in a row is not trusted
constexpr s64 TalkUiNs = 30'000'000'000;    // the talk UI counts this long after the last line
constexpr s64 TalkGraceNs = 3'000'000'000;  // the conversation lasts this long after both end
} // Anonymous namespace

void PublishNpcSubtitle(bool shown) {
    if (shown) {
        S().npc_subtitle_ns.store(NowNs(), std::memory_order_relaxed);
    }
}

bool IsNpcSubtitleUp() {
    const s64 stamp = S().npc_subtitle_ns.load(std::memory_order_relaxed);
    const s64 now = NowNs();
    return stamp != 0 && now >= stamp && now - stamp < 500'000'000;
}

void PublishNpcTalking(bool talking, bool talk_ui) {
    auto& st = S();
    const s64 now = NowNs();
    if (talking && !st.npc_talking.load(std::memory_order_relaxed)) {
        st.npc_talk_since_ns.store(now, std::memory_order_relaxed);
    }
    const bool line =
        talking && now - st.npc_talk_since_ns.load(std::memory_order_relaxed) < TalkStuckNs;
    if (line) {
        st.npc_line_ns.store(now, std::memory_order_relaxed);
    }
    const s64 last_line = st.npc_line_ns.load(std::memory_order_relaxed);
    if (line || (talk_ui && last_line != 0 && now - last_line < TalkUiNs)) {
        st.npc_conversation_ns.store(now, std::memory_order_relaxed);
    }
    st.npc_talking.store(talking, std::memory_order_relaxed);
    st.npc_talk_ns.store(now, std::memory_order_release);
}

namespace {
std::atomic<bool> g_area_dim_removed{false};
} // Anonymous namespace

void PublishAreaDimRemoved(bool removed) {
    g_area_dim_removed.store(removed, std::memory_order_release);
}

bool IsAreaDimRemoved() {
    return g_area_dim_removed.load(std::memory_order_acquire);
}

namespace {
std::atomic<s64> g_lamp_warp_ns{0};
std::atomic<s64> g_lamp_arrival_ns{0};
} // Anonymous namespace

namespace {
std::atomic<s64> g_lamp_box_hidden_ns{0};
} // Anonymous namespace

void PublishLampBoxHidden(bool hidden) {
    g_lamp_box_hidden_ns.store(hidden ? NowNs() : 0, std::memory_order_release);
}

bool IsLampBoxHidden() {
    const s64 at = g_lamp_box_hidden_ns.load(std::memory_order_acquire);
    return at != 0 && NowNs() - at < 500'000'000;
}

void PublishLampWarp(bool arrived) {
    (arrived ? g_lamp_arrival_ns : g_lamp_warp_ns).store(NowNs(), std::memory_order_release);
}

bool LampWarpDeparting() {
    const s64 start = g_lamp_warp_ns.load(std::memory_order_acquire);
    const s64 now = NowNs();
    return start != 0 && g_lamp_arrival_ns.load(std::memory_order_acquire) < start &&
           now - start < 30'000'000'000;
}

bool LampWarpArrivedWithin(float seconds) {
    const s64 arrival = g_lamp_arrival_ns.load(std::memory_order_acquire);
    return arrival != 0 && arrival >= g_lamp_warp_ns.load(std::memory_order_acquire) &&
           NowNs() - arrival < static_cast<s64>(seconds * 1e9f);
}

void PublishItemPickup(bool shown) {
    if (shown) {
        S().item_pickup_ns.store(NowNs(), std::memory_order_relaxed);
    }
}

bool IsItemPickupUp() {
    const s64 stamp = S().item_pickup_ns.load(std::memory_order_relaxed);
    const s64 now = NowNs();
    return stamp != 0 && now >= stamp && now - stamp < 1'000'000'000;
}

void PublishActionPrompt(bool shown) {
    if (shown) {
        S().action_prompt_ns.store(NowNs(), std::memory_order_relaxed);
    }
}

bool IsActionPromptUp() {
    const s64 stamp = S().action_prompt_ns.load(std::memory_order_relaxed);
    const s64 now = NowNs();
    // The game clears its flag before the movie finishes fading the prompt out.
    return stamp != 0 && now >= stamp && now - stamp < 1'000'000'000;
}

void PublishPlainHud(bool plain) {
    auto& st = S();
    const s64 now = NowNs();
    if (!plain) {
        st.busy_ui_ns.store(now, std::memory_order_relaxed);
    }
    st.plain_hud_ns.store(now, std::memory_order_release);
}

namespace {
std::atomic<u64> g_banner_serial{0};
std::atomic<u32> g_banner_type{0};
std::atomic<s64> g_banner_ns{0};
} // Anonymous namespace

void PublishBanner(u32 type) {
    g_banner_type.store(type, std::memory_order_relaxed);
    g_banner_ns.store(NowNs(), std::memory_order_relaxed);
    g_banner_serial.fetch_add(1, std::memory_order_release);
}

BannerEvent LatestBanner() {
    BannerEvent e{};
    e.serial = g_banner_serial.load(std::memory_order_acquire);
    e.type = g_banner_type.load(std::memory_order_relaxed);
    e.ns = g_banner_ns.load(std::memory_order_relaxed);
    return e;
}

bool IsPlainHud() {
    auto& st = S();
    const s64 now = NowNs();
    const s64 sample = st.plain_hud_ns.load(std::memory_order_acquire);
    // A second after the last prompt, text or talk: the frames drawn around it are not counted.
    return sample != 0 && now - sample < 500'000'000 &&
           now - st.busy_ui_ns.load(std::memory_order_relaxed) > 1'000'000'000 && !IsLockedOn() &&
           !IsNpcTalking();
}

bool IsHudTextUp() {
    auto& st = S();
    const s64 sample = st.plain_hud_ns.load(std::memory_order_acquire);
    return HudTextUp(NowNs(), sample, st.busy_ui_ns.load(std::memory_order_relaxed));
}

bool IsNpcTalking() {
    auto& st = S();
    const s64 stamp = st.npc_talk_ns.load(std::memory_order_acquire);
    const s64 now = NowNs();
    if (stamp == 0 || now - stamp >= 500'000'000) {
        return false;
    }
    const s64 conversation = st.npc_conversation_ns.load(std::memory_order_relaxed);
    return conversation != 0 && now - conversation < TalkGraceNs;
}

void SetVisceralFirstPerson(bool on) {
    S().visceral_first_person.store(on, std::memory_order_relaxed);
}

bool IsGameMenuOpen() {
    auto& st = S();
    const s64 stamp = st.game_menu_ns.load(std::memory_order_acquire);
    const s64 now = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count();
    return st.game_menu.load(std::memory_order_relaxed) && stamp != 0 &&
           now - stamp < 500'000'000;
}

UiClass DrawnFrameUiClass() {
    return static_cast<UiClass>(S().current_frame_ui_class);
}

UiClass GameMenuClass() {
    return IsGameMenuOpen()
               ? static_cast<UiClass>(S().game_menu_class.load(std::memory_order_relaxed))
               : UiClass::None;
}

float GameMenuPitchDeg() {
    return IsGameMenuOpen() ? S().game_menu_pitch.load(std::memory_order_relaxed) : 0.0f;
}

s32 GameMenuGridCell() {
    return IsGameMenuOpen() ? S().game_menu_grid.load(std::memory_order_relaxed) : -2;
}

MenuPresent MenuPresentMode() {
    const auto& s = S().settings;
    if (s.menu_present_set) {
        return static_cast<MenuPresent>(s.menu_present);
    }
    return s.menu_panel ? MenuPresent::Layer : MenuPresent::Hud;
}

void SetUiLayerAvailable(bool available) {
    S().ui_layer_available.store(available, std::memory_order_release);
}

bool UiLayerAvailable() {
    return S().ui_layer_available.load(std::memory_order_acquire);
}

s32 UiLayerFrameMode() {
    return S().current_frame_ui;
}

void NoteUiMarker(bool seen) {
    auto& st = S();
    if (seen || !IsGameMenuOpen()) {
        st.ui_marker_missing = 0;
        return;
    }
    if (++st.ui_marker_missing == 3 && !st.ui_marker_fallback.load(std::memory_order_relaxed)) {
        st.ui_marker_fallback.store(true, std::memory_order_relaxed);
        LOG_WARNING(Core, "VR ui: the open menu draws no composite (3 frames); it is shown on the "
                          "frozen menu panel until it closes");
    }
}

bool MenuPanelActive() {
    auto& st = S();
    const auto& s = st.settings;
    if (!s.menu_mode || !IsEnabled()) {
        return false;
    }
    switch (MenuPresentMode()) {
    case MenuPresent::Hud:
        return false;
    case MenuPresent::Layer:
        // The UI layer shows menus over the live world; the frozen panel only when it cannot
        // (off, unavailable, or a menu without the composite).
        if (st.ui_marker_fallback.load(std::memory_order_relaxed)) {
            if (!IsGameMenuOpen()) {
                st.ui_marker_fallback.store(false, std::memory_order_relaxed);
            }
        }
        if (const StereoConfig stereo = GetStereo();
            s.ui_layer == 1 && UiLayerAvailable() && !(stereo.stereo && stereo.mode == 0) &&
            !st.ui_marker_fallback.load(std::memory_order_relaxed)) {
            return false; // (the clip-shift mode has no UI layer)
        }
        break;
    case MenuPresent::Frozen:
        break;
    }
    // The virtual screen (chosen with Menu, or a cinematic's flat view) shows the game's own camera:
    // its menus stay on the screen, which is as large, instead of a panel over a frozen view that
    // is not being driven.
    if (GetDisplayMode() != DisplayMode::Auto || CinematicFlat()) {
        return false;
    }
    if (IsGameMenuOpen()) {
        return true;
    }
    // Chained menus (a talk list into a shop, Level Up) close and reopen within a frame or two.
    const s64 open = st.game_menu_open_ns.load(std::memory_order_relaxed);
    return open != 0 && NowNs() - open < 150'000'000;
}

float MenuPanelWidthDeg() {
    auto& st = S();
    return std::min(st.settings.menu_width_deg * st.game_menu_zoom.load(std::memory_order_relaxed),
                    150.0f);
}

void SetMenuPanelAim(bool shown, float yaw, float pitch) {
    auto& st = S();
    std::scoped_lock lk{st.menu_aim_mutex};
    if (shown) {
        // The presenter's thread also changes the space (ShiftTrackingSpace): this is its space.
        st.menu_aim = {true, yaw, pitch, st.space_serial.load(std::memory_order_acquire)};
    } else {
        st.menu_aim.valid = false;
    }
}

void SetEnabled(bool enabled) {
    S().enabled.store(enabled);
}

bool IsEnabled() {
    return S().enabled.load(std::memory_order_relaxed);
}

void SetSimulatedHead(bool enabled) {
    S().simulated.store(enabled);
}

bool IsSimulatedHead() {
    return S().simulated.load(std::memory_order_relaxed);
}

void PublishHead(const HeadState& state) {
    auto& st = S();
    std::scoped_lock lk{st.head_mutex};
    st.head = state;
    // The presenter publishes every sample after the ShiftTrackingSpace of the space it was
    // located in (same thread), so the current serial is that space's.
    st.head.space_serial = st.space_serial.load(std::memory_order_acquire);
}

HeadState GetHead() {
    auto& st = S();
    if (st.simulated.load(std::memory_order_relaxed)) {
        return SimulatedHead();
    }
    std::scoped_lock lk{st.head_mutex};
    return st.head;
}

void AddTurn(float radians) {
    if (!IsImmersive() || GameOwnsCamera()) {
        return; // loading screens, cutscenes, lock-on: never queue turns
    }
    auto& turn = S().pending_turn;
    float expected = turn.load();
    while (!turn.compare_exchange_weak(expected, expected + radians)) {
    }
}

void ToggleScreenMode(const char* why) {
    const auto phase = CinematicPhase();
    if (phase == Cinematic::Phase::FadeOut || phase == Cinematic::Phase::Flat) {
        // Escape hatch (taken at the next player update): first-person VR comes back at once and
        // the rest of this animation (on a ladder: this climb) stays in VR. The display mode
        // stays Auto, so the gameplay controls stay and VR is back for good when it ends.
        S().cinematic_suppress.store(true);
        LOG_INFO(Core,
                 "VR cine: {} during a cinematic; back to first-person VR, this animation (or "
                 "ladder climb) stays in VR",
                 why);
        return;
    }
    const auto mode = GetDisplayMode();
    SetDisplayMode(mode == DisplayMode::Auto ? DisplayMode::Screen : DisplayMode::Auto);
}

void SetDisplayMode(DisplayMode mode) {
    S().display_mode.store(static_cast<u32>(mode));
    LOG_INFO(Core, "VR: display mode -> {}", mode == DisplayMode::Auto ? "auto" : "screen");
}

DisplayMode GetDisplayMode() {
    return static_cast<DisplayMode>(S().display_mode.load(std::memory_order_relaxed));
}

StereoConfig GetStereo() {
    return UnpackStereo(S().stereo_word.load(std::memory_order_acquire));
}

s32 CycleStereoMode(s32 direction) {
    auto& st = S();
    const s32 current = LiveMode(UnpackStereo(st.stereo_request.load()));
    const s32 next =
        ((current + (direction < 0 ? -1 : 1)) % LiveModeCount + LiveModeCount) % LiveModeCount;
    st.stereo_request.store(next == 0 ? PackStereo(false, st.settings.stereo_mode)
                                      : PackStereo(true, next));
    LOG_INFO(Core, "VR: stereo mode -> {}", StereoModeName(next));
    return next;
}

const char* StereoModeName(s32 live_mode) {
    switch (live_mode) {
    case 0:
        return "mono";
    case 1:
        return "AER (stereo_mode 1)";
    case 2:
        return "DIBR (stereo_mode 2)";
    default:
        return "unknown";
    }
}

// Gamepad / keyboard right stick turns the view (the game's own camera input is disabled
// while the headset drives the camera). VR controllers turn through AddTurn directly.
void PollPadTurn(float dt) {
    auto& st = S();
    const auto state = (*Common::Singleton<Input::GameControllers>::Instance())[0]->ReadState();
    const float x =
        (static_cast<float>(state.axes[static_cast<size_t>(Input::Axis::RightX)]) - 128.0f) /
        127.0f;
    const auto& s = st.settings;
    if (std::abs(x) < 0.3f) {
        st.pad_turn_armed = true;
        return;
    }
    if (s.snap_turn_deg > 0.0f) {
        if (st.pad_turn_armed && std::abs(x) > 0.7f) {
            AddTurn((x > 0.0f ? 1.0f : -1.0f) * s.snap_turn_deg * DegToRad);
            st.pad_turn_armed = false;
        }
    } else {
        AddTurn(x * s.smooth_turn_deg_per_s * DegToRad * dt);
    }
}

namespace {

// Game thread, every camera tick before the head is used, with the one head sample this tick
// uses: once it is located in a new tracking space (a recenter), carry the view over into that
// space (exactly its, not the newest: a change noted after the sample is carried with the next
// sample). The game heading of tracking forward turns by the space's yaw, exactly as much as
// every head yaw drops, so the view yaw (base - head), the hands' mapping into the game and the
// 6DoF offset stay as they were; the 6DoF and neck origins and the head samples the view holds (a
// stereo pair's, the menu's frozen one) are moved into the new space. Then the 6DoF origin is
// taken from the live head (RecenterPosition: the camera returns over the hunter; during a menu
// once it closes).
void ApplySpaceShift(State& st, const HeadState& sample) {
    if (!sample.valid) {
        return;
    }
    const u32 serial = sample.space_serial;
    // The same space, or an older one (the simulated head is always in space 0).
    if (static_cast<s32>(serial - st.shift_applied) <= 0) {
        return;
    }
    Space::Shift shift{};
    if (!SpaceShiftBetween(st.shift_applied, serial, shift)) {
        shift.known = false; // more than the log holds, or a change without its pose
    }
    const float base_before = st.base_yaw;
    if (shift.known) {
        st.base_yaw = Space::BaseYawAfter(st.base_yaw, shift);
        st.head_origin = Space::PointToNew(shift, st.head_origin);
        st.neck_origin = Space::PointToNew(shift, st.neck_origin);
        for (HeadState* held : {&st.pair_head, &st.menu_head}) {
            held->head.orientation = Space::RotationToNew(shift, held->head.orientation);
            held->head.position = Space::PointToNew(shift, held->head.position);
            held->floor_y = Space::PointToNew(shift, Vec3{0.0f, held->floor_y, 0.0f}).y;
        }
        st.floor_y = Space::PointToNew(shift, Vec3{0.0f, st.floor_y, 0.0f}).y;
    } else {
        st.floor_known = false; // the next sample's floor (in the new space) is taken
    }
    st.pair_head.space_serial = serial; // (unknown: they stay where they are)
    st.menu_head.space_serial = serial;
    st.recenter.store(true);
    ++st.turn_serial; // the eyes never pair images from the two spaces
    st.shift_applied = serial;
    st.shift_log_pending = st.driving;
    st.shift_first_record.store(st.next_record_id, std::memory_order_relaxed);
    st.shift_applied_serial.store(serial, std::memory_order_release);
    if (shift.known) {
        LOG_INFO(Core,
                 "VR: tracking space {} (recenter): forward turned {:.1f} deg, origin moved "
                 "({:.3f}, {:.3f}, {:.3f}) m; game heading of forward {:.1f} -> {:.1f} deg (the "
                 "view keeps its direction){}",
                 serial, shift.yaw / DegToRad, shift.x, shift.y, shift.z, base_before / DegToRad,
                 st.base_yaw / DegToRad, st.menu_frozen ? "; the menu's frozen view moved with it" : "");
    } else {
        LOG_INFO(Core, "VR: tracking space {} (recenter, its shift unknown: the runtime's without "
                       "its pose): only the 6DoF origin is taken again",
                 serial);
    }
}

} // namespace

bool KeepEyeFov() {
    static const bool enabled = [] {
        const char* value = std::getenv("VR_KEEP_EYE_FOV");
        return value && value[0] == '1';
    }();
    return enabled || FastRender::On(FastRender::KeepEyeFov);
}

double GameplaySeconds() {
    const s64 first = S().first_gameplay_tick_ns.load(std::memory_order_acquire);
    return first ? static_cast<double>(NowNs() - first) * 1e-9 : -1.0;
}

CameraOverride OnCameraTick(float game_pitch, float game_yaw, float game_vfov, bool lock_on,
                            bool game_orbit_mode, float game_aspect, bool render_camera,
                            const Vec3* game_anchor) {
    auto& st = S();
    // VR_TUNE_FILE cam.stall=<n> (tests): each new non-zero value stalls this camera tick once by
    // cam.stall_ms (default 300 ms): a game hitch on demand.
    if (Tune::Active()) {
        static float stall_seen = 0.0f;
        const float stall = Tune::Get("cam.stall", 0.0f);
        if (stall != stall_seen) {
            stall_seen = stall;
            if (stall != 0.0f) {
                const float ms = std::clamp(Tune::Get("cam.stall_ms", 300.0f), 0.0f, 5000.0f);
                LOG_INFO(Core, "VR: test stall of the camera tick, {:.0f} ms (cam.stall={})", ms,
                         stall);
                std::this_thread::sleep_for(std::chrono::microseconds(static_cast<s64>(ms * 1000.0f)));
            }
        }
    }
    // The render camera's own aspect sets the horizontal FOV of the projection; an implausible
    // value (not a render camera) falls back to the frame's W/H.
    const bool aspect_known = game_aspect > 0.25f && game_aspect < 5.0f;
    if (aspect_known) {
        st.camera_aspect.store(game_aspect, std::memory_order_relaxed);
    }
    const s64 now = NowNs();
    const s64 last = st.last_tick_ns.exchange(now);
    const float dt = std::clamp(static_cast<float>(now - last) * 1e-9f, 0.0f, 0.1f);
    if (dt > 0.0f) {
        // Game frame time for input timing (the game samples the pad once per frame).
        const float previous = st.frame_seconds.load(std::memory_order_relaxed);
        const float smoothed =
            previous + (std::clamp(dt, 1.0f / 60.0f, 1.0f / 15.0f) - previous) *
                           (1.0f - std::exp(-dt / 0.5f));
        st.frame_seconds.store(smoothed, std::memory_order_relaxed);
    }
    st.locked_on.store(lock_on, std::memory_order_relaxed);
    if (IsEnabled() && st.driving && !game_orbit_mode && !(lock_on && st.settings.decoupled)) {
        PollPadTurn(dt);
    }
    const float stale_ns = st.settings.camera_stale_ms * 1e6f;
    if (static_cast<float>(now - last) > stale_ns) {
        // The camera was not updated for a while (a hitch, loading, a cutscene's own camera): the
        // takeover below resumes the view if the game camera stayed put (core/vr/camera_gap.h),
        // else takes it over again. Only a gap that interrupted driving may resume.
        st.gap_pending = st.driving;
        st.gap_ns = last != 0 ? now - last : 0;
        st.driving = false;
    }
    // One head sample for the whole tick: a recenter is carried into exactly its space before
    // anything below reads it (a second read could already be in a newer one).
    const HeadState live = GetHead();
    if (KeepEyeFov())
        EyeFov::Update(st.last_valid_eye_fov, live.eye_fov);
    ApplySpaceShift(st, live);
    // Height: the floor of the newest sample that has it (the presenter locates it only while
    // Height is As the player or a seated eye height is set), in the space the view is now in.
    if (live.valid && live.floor_valid && live.space_serial == st.shift_applied) {
        st.floor_y = live.floor_y;
        st.floor_known = true;
    }

    CameraRecord record{};
    record.space_serial = st.shift_applied;
    CameraOverride result{};
    // Cinematic flat mode: the game's third-person camera is shown on the virtual screen, exactly
    // like a cutscene (not driven, the right stick goes to the game).
    const bool want = IsEnabled() && GetDisplayMode() == DisplayMode::Auto && !CinematicFlat();
    HeadState sampled = want ? live : HeadState{};
    // Menu panel: the view stays where you looked when the menu opened (mono), so the game image
    // on the panel holds still while you look around the panel.
    const bool menu_hold = want && sampled.valid && MenuPanelActive();
    if (menu_hold) {
        if (!st.menu_frozen) {
            st.menu_frozen = true;
            st.menu_head = sampled;
            // Looking the way the panel faces (the presenter places it with the same clamp).
            float yaw{}, pitch{};
            YawPitchFromQuat(sampled.head.orientation, yaw, pitch);
            const float panel_pitch = std::clamp(pitch, MenuPanelPitchMinDeg * DegToRad,
                                                 MenuPanelPitchMaxDeg * DegToRad);
            st.menu_head.head.orientation = QuatFromYawPitch(yaw, panel_pitch);
            LOG_INFO(Core, "VR menu: view frozen for the menu panel (head yaw {:.1f} pitch {:.1f} "
                           "deg, view pitch {:.1f}, mono)",
                     yaw / DegToRad, pitch / DegToRad, panel_pitch / DegToRad);
        }
        State::MenuAim aim{};
        {
            std::scoped_lock lk{st.menu_aim_mutex};
            aim = st.menu_aim;
        }
        if (aim.valid && aim.space == st.menu_head.space_serial) {
            // The panel is up: look where it faces (it may have moved to where you look). An aim
            // from another tracking space (a recenter re-places the panel within the next XR
            // frame) is not taken: the frozen view keeps its last aim, moved with the space.
            st.menu_head.head.orientation = QuatFromYawPitch(aim.yaw, aim.pitch);
        }
        sampled = st.menu_head;
    } else if (st.menu_frozen) {
        st.menu_frozen = false;
        st.tick_eye = 1; // the eye pairs start again from the left eye
        st.shift_tick = 1;
        LOG_INFO(Core, "VR menu: view follows the head again");
    }

    if (want && sampled.valid) {
        if (render_camera && !menu_hold &&
            !st.first_gameplay_tick_ns.load(std::memory_order_relaxed))
            st.first_gameplay_tick_ns.store(now, std::memory_order_release);
        const auto& s = st.settings;
        const bool decoupled = s.decoupled;
        const bool takeover = !st.driving;
        if (takeover) {
            st.tick_eye = 1; // a takeover starts a new pair
            st.shift_tick = 1;
        }
        // Live stereo switch: applied only between two pairs (after the right eye), so a pair
        // never mixes modes. AER starts each pair with the left eye.
        StereoConfig stereo = UnpackStereo(st.stereo_word.load(std::memory_order_relaxed));
        const bool aer_before = stereo.stereo && stereo.mode == 1;
        const bool shift_before = stereo.stereo && stereo.mode == 0;
        if ((!aer_before || st.tick_eye == 1) && (!shift_before || st.shift_tick == 1)) {
            const u32 request = st.stereo_request.load();
            if (request != st.stereo_word.load(std::memory_order_relaxed)) {
                st.stereo_word.store(request, std::memory_order_release);
                stereo = UnpackStereo(request);
                st.tick_eye = 1;
                st.shift_tick = 1;
            }
        }
        // The menu panel shows mono frames (no eye offset).
        const bool aer = stereo.stereo && stereo.mode == 1 && !menu_hold;
        // Per-eye stereo: alternate the eye every camera update.
        const s32 eye = aer ? (st.tick_eye ^ 1) : -1;
        if (aer) {
            st.tick_eye = eye;
        }
        // stereo_mode 0 shifts alternate frames for the two eyes and shows a left frame and the
        // right frame after it together: the same pairing by update parity (the GPU boundary
        // follows it, see OnGpuFrameBoundary).
        const bool shift = stereo.stereo && stereo.mode == 0 && !menu_hold;
        const s32 shift_eye = shift ? (st.shift_tick ^ 1) : -1;
        if (shift) {
            st.shift_tick = shift_eye;
        }
        // Everything that changes the whole view advances only on the first eye of a pair
        // (every update in mono), so the two eyes of a pair share the heading and the 6DoF
        // origin: an eye pair with different headings is a uniform vergence error. The second
        // eye also renders with the first eye's head sample, like the two eyes of a real head at
        // one moment: no vertical or rotational disparity from head motion between the two
        // updates (the compositor reprojects both from that one pose).
        const bool pair_first = (aer ? eye : shift_eye) <= 0;
        // Native stereo uses the same startup environment switch as NativeStereo::Enabled.
        // Its compositor receives the render orientation from CameraRecord, but there is
        // no depth-aware positional correction: only the tiny angular band is held.
        static const bool native_stereo = std::getenv("VR_NATIVE_STEREO") != nullptr;
        const bool native_steady = native_stereo && s.native_render_steady;
        const bool steady = render_camera && stereo.stereo && stereo.mode == 2 && !menu_hold &&
                            (native_stereo ? native_steady : s.render_steady && LateWarpAvailable());
        const Steady::Config steady_cfg{
            native_stereo ? s.native_render_steady_deg : s.render_steady_deg,
            native_stereo ? 0.0f : s.render_steady_mm,
            s.render_steady_follow_deg_s, s.render_steady_follow_mm_s};
        if (steady != st.steady_on) {
            st.steady_on = steady;
            st.steady.valid = false;
            if (st.steady_switches++ < 8) {
                LOG_INFO(Core,
                         "VR steady: {} {} (band {:.2f} deg / {:.1f} mm, follow above {:.1f} "
                         "deg/s / {:.0f} mm/s; menu {}, late warp {})",
                         native_stereo ? "native angular-only" : "DIBR render camera",
                         steady ? "on" : "off", steady_cfg.band_deg, steady_cfg.band_mm,
                         steady_cfg.follow_deg_s, steady_cfg.follow_mm_s, menu_hold,
                         LateWarpAvailable());
            }
        }
        if (pair_first) {
            st.pair_head = sampled;
            if (steady) {
                if (takeover || st.recenter.load()) {
                    st.steady.valid = false; // the 6DoF origin and the heading take the true head
                }
                const Quat& o = sampled.head.orientation;
                const Vec3& hp = sampled.head.position;
                Steady::Q q_out{};
                Steady::V p_out{};
                const Steady::Q input_q{o.x, o.y, o.z, o.w};
                const Steady::V input_p{hp.x, hp.y, hp.z};
                const Steady::Step step = native_steady
                    ? Steady::UpdateRotation(st.steady, input_q, input_p, sampled.time,
                                             steady_cfg, q_out, p_out)
                    : Steady::Update(st.steady, input_q, input_p, sampled.time,
                                     steady_cfg, q_out, p_out);
                st.pair_head.head.orientation = {q_out.x, q_out.y, q_out.z, q_out.w};
                st.pair_head.head.position = {p_out.x, p_out.y, p_out.z};
                const float off_deg = Steady::AngleDeg({o.x, o.y, o.z, o.w}, q_out);
                const float off_mm = Steady::DistMm({hp.x, hp.y, hp.z}, p_out);
                ++st.steady_counts[static_cast<u32>(step)];
                st.steady_off_deg_sum += off_deg;
                st.steady_off_mm_sum += off_mm;
                st.steady_off_deg_max = std::max(st.steady_off_deg_max, off_deg);
                st.steady_off_mm_max = std::max(st.steady_off_mm_max, off_mm);
                const u64 n = st.steady_counts[0] + st.steady_counts[1] + st.steady_counts[2] +
                              st.steady_counts[3];
                if (n >= (native_steady ? 600u : 1800u)) {
                    LOG_INFO(Core,
                             "VR steady: {} camera updates: {} held, {} pulled at the band's edge, "
                             "{} followed the head, {} started; render camera vs your head mean "
                             "{:.2f} deg / {:.1f} mm, max {:.2f} deg / {:.1f} mm",
                             n, st.steady_counts[2], st.steady_counts[3], st.steady_counts[1],
                             st.steady_counts[0], st.steady_off_deg_sum / static_cast<double>(n),
                             st.steady_off_mm_sum / static_cast<double>(n), st.steady_off_deg_max,
                             st.steady_off_mm_max);
                    std::fill(std::begin(st.steady_counts), std::end(st.steady_counts), 0);
                    st.steady_off_deg_sum = st.steady_off_mm_sum = 0.0;
                    st.steady_off_deg_max = st.steady_off_mm_max = 0.0f;
                }
            } else {
                st.steady.valid = false;
            }
            if (aer) {
                ++st.pair_serial;
            }
        }
        const HeadState& head = st.pair_head;
        float head_yaw{}, head_pitch{};
        YawPitchFromQuat(head.head.orientation, head_yaw, head_pitch);
        // camera_roll: the head's roll about its view axis (against the level frame of its own
        // view direction, before the pitch limit). The render camera rolls with it and the eyes sit
        // on the rolled baseline, so near objects get no vertical disparity when you tilt your
        // head. The menu panel's frozen view stays level.
        const float head_roll = s.camera_roll && !menu_hold
                                    ? RollFromQuat(head.head.orientation, head_yaw, head_pitch)
                                    : 0.0f;
        head_pitch = std::clamp(head_pitch, -s.pitch_limit_down_deg * DegToRad,
                                s.pitch_limit_up_deg * DegToRad);

        if (takeover) {
            const bool resync = st.resync_to_head.exchange(false);
            // A gap in the updates where only time passed (a hitch): the view resumes with its
            // heading, orbit and 6DoF origin, so your head's offset (a duck, a lean) is still
            // measured from the recentered head. The game camera moved (a load, a teleport), the
            // first update, a cinematic's return or the bridge coming back: taken over.
            const bool from_gap = st.gap_pending;
            st.gap_pending = false;
            const CameraGap::Decision gap =
                from_gap ? CameraGap::Decide(st.gap_last, game_yaw,
                                             game_anchor != nullptr ? &game_anchor->x : nullptr,
                                             s.world_scale)
                         : CameraGap::Decision{};
            const bool resume = from_gap && gap.Resume() && s.camera_gap_resume && !resync;
            if (from_gap && st.gap_last.valid && (st.gap_count < 40 || st.gap_count % 50 == 0)) {
                LOG_INFO(Core,
                         "VR: camera updates back after a {:.0f} ms gap (camera_stale_ms={:.0f}): "
                         "{} (the game camera moved {} turned {:.1f} deg); {} (head at ({:.3f}, "
                         "{:.3f}, {:.3f}), origin ({:.3f}, {:.3f}, {:.3f}))",
                         static_cast<double>(st.gap_ns) * 1e-6, s.camera_stale_ms,
                         CameraGap::VerdictName(gap.verdict),
                         gap.moved_m >= 0.0f ? fmt::format("{:.2f} m", gap.moved_m)
                                             : std::string{"? m"},
                         gap.turned_rad / DegToRad,
                         resume              ? "heading and 6DoF origin kept"
                         : !s.camera_gap_resume ? "taken over (camera_gap_resume=0)"
                         : resync            ? "taken over (a cinematic's return)"
                                             : "taken over: heading and 6DoF origin from now",
                         head.head.position.x, head.head.position.y, head.head.position.z,
                         st.head_origin.x, st.head_origin.y, st.head_origin.z);
            }
            if (from_gap && st.gap_last.valid) {
                ++st.gap_count;
            }
            if (!resume) {
                // Keep the game's current heading (and orbit pitch) when we take over. In first
                // person (and coupled) the game camera's yaw is the view's, base - head, so the
                // head is added back: the view continues where the game camera looked instead of
                // jumping by the head's room yaw. Returning from a cinematic always re-syncs that
                // way.
                st.base_yaw = resync || !decoupled || IsFirstPerson()
                                  ? WrapAngle(game_yaw + head_yaw)
                                  : game_yaw;
                if (resync) {
                    LOG_INFO(Core, "VR cine: return base={:.3f} game={:.3f} head={:.3f}",
                             st.base_yaw, game_yaw, head_yaw);
                }
                st.orbit_pitch = game_pitch;
                st.roomscale_walk.Reset();
                st.head_origin = head.head.position; // leaning is measured from here
                st.neck_origin = NeckPivot(head.head);
            }
            st.driving = true;
            ++st.turn_serial;
            st.pair_tick_ns = now;
        }
        const float pair_dt =
            std::clamp(static_cast<float>(now - st.pair_tick_ns) * 1e-9f, 0.0f, 0.1f);
        if (pair_first) {
            st.pair_tick_ns = now;
            // A recenter during a menu waits for the live head: the frozen menu sample is still
            // in the tracking space from before the recenter (the view inside the panel keeps
            // its offset meanwhile).
            if (!menu_hold && st.recenter.exchange(false)) {
                st.roomscale_walk.Reset();
                st.head_origin = head.head.position;
                st.neck_origin = NeckPivot(head.head);
                st.recenter_deferred = false;
                LOG_INFO(Core, "VR: recentered, 6DoF origin at the head ({:.3f}, {:.3f}, {:.3f})",
                         st.head_origin.x, st.head_origin.y, st.head_origin.z);
            } else if (menu_hold && !st.recenter_deferred && st.recenter.load()) {
                st.recenter_deferred = true;
                LOG_INFO(Core, "VR: recenter during a menu: applied when the menu closes");
            }
            if (const float turn = st.pending_turn.exchange(0.0f); turn != 0.0f) {
                st.base_yaw = WrapAngle(st.base_yaw + turn);
                if (std::abs(turn) > 0.1f) {
                    ++st.turn_serial; // the eyes must not mix pre- and post-turn images
                }
            }
        }
        // Lock-on: the game keeps steering the orbit toward the target and the view
        // follows it; the head still looks around freely on top.
        const bool game_orbit = game_orbit_mode || (decoupled && lock_on);
        const bool first_person_lock = lock_on && !game_orbit_mode && IsFirstPerson();
        if (!pair_first) {
            // Second eye: same heading and orbit as the first.
        } else if (game_orbit && !first_person_lock) {
            st.base_yaw = game_yaw;
            st.orbit_pitch = game_pitch;
        } else {
            // Ease back to the comfortable orbit pitch (e.g. after lock-on on a tall boss).
            const float target = s.orbit_pitch_deg * DegToRad;
            st.orbit_pitch += (target - st.orbit_pitch) * (1.0f - std::exp(-pair_dt / 0.35f));
        }

        const float render_aspect =
            aspect_known ? game_aspect : st.main_aspect.load(std::memory_order_relaxed);
        // render_pitch_bias_deg (core/vr/render_tilt.h): the render camera looks this much lower
        // than the head, the vertical FOV covers the eyes as that render sees them. Only with
        // DIBR while the late warp shows the eyes (it undoes the bias), never on the menu panel.
        const bool dibr_mode = stereo.stereo && stereo.mode == 2;
        const float bias =
            render_camera &&
                    Tilt::BiasWanted(s.render_pitch_bias_deg, dibr_mode, menu_hold,
                                     LateWarpAvailable())
                ? s.render_pitch_bias_deg * DegToRad
                : 0.0f;
        if (bias != st.tilt_bias) {
            if (st.tilt_switches++ < 8) {
                LOG_INFO(Core,
                         "VR: render pitch bias {:.1f} deg{} (vr.ini render_pitch_bias_deg={:.1f}; "
                         "DIBR {}, menu panel {}, late warp showing the eyes {}); margin {:.1f} deg "
                         "(fov_margin_deg={:.1f}, render_pitch_min_margin_deg={:.1f} while the "
                         "bias applies)",
                         bias / DegToRad, bias != 0.0f ? ": the render looks below your head" : "",
                         s.render_pitch_bias_deg, dibr_mode ? "yes" : "no",
                         menu_hold ? "yes" : "no", LateWarpAvailable() ? "yes" : "no",
                         Tilt::MarginDeg(s.fov_margin_deg, s.render_pitch_min_margin_deg, bias),
                         s.fov_margin_deg, s.render_pitch_min_margin_deg);
            }
            st.tilt_bias = bias;
        }
        float vfov = ComputeVfov(head, render_aspect, bias);
        if (menu_hold) {
            // The menu panel is a window onto the frozen view at the world's own scale: the
            // frame's width spans the panel's menu_width_deg (instead of a 130 deg view squeezed
            // into it), so the view inside the panel lines up with the frozen view around it
            // (exactly on the flat panel; the curved one bends this flat render onto a cylinder,
            // within about 2 deg). The presenter sizes the panel from this FOV.
            vfov = 2.0f * std::atan(std::tan(0.5f * MenuPanelWidthDeg() * DegToRad) /
                                    std::max(render_aspect, 0.1f));
        }
        if (s.vfov_deg > 0.0f && vfov > s.vfov_deg * DegToRad + 1e-4f && !st.vfov_raised_logged) {
            st.vfov_raised_logged = true;
            LOG_WARNING(Core,
                        "VR: vfov_deg={} is narrower than the headset's FOV, and this runtime "
                        "always shows its own FOV (fovMutable false), which would stretch the "
                        "image; rendering {:.1f} deg instead",
                        s.vfov_deg, vfov / DegToRad);
        }
        float view_yaw = WrapAngle(st.base_yaw - head_yaw);
        float lag_test_yaw = 0.0f; // VR_LAG_TEST=2: also in the record's head yaw (below)
        if (LagTestEnabled()) {
            // Odd records look 40 degrees right, even records 40 degrees left. VR_LAG_TEST=4: four
            // headings by record id % 4 (-34, -11, +11, +34 degrees), so the image shows which
            // update it was rendered with (or a blend of two).
            static constexpr std::array<float, 4> Quarter{-0.6f, -0.2f, 0.2f, 0.6f};
            // VR_LAG_TEST_DEG: the +- offset of modes 1 and 2 in degrees (default 40).
            static const float amount = std::getenv("VR_LAG_TEST_DEG")
                                            ? std::strtof(std::getenv("VR_LAG_TEST_DEG"), nullptr) *
                                                  DegToRad
                                            : 0.7f;
            const float offset = LagTestMode() == 4 ? Quarter[st.next_record_id % 4]
                                 : (st.next_record_id & 1) ? amount
                                                           : -amount;
            view_yaw = WrapAngle(view_yaw + offset);
            lag_test_yaw = LagTestMode() >= 2 ? offset : 0.0f;
        }
        // eye_taa_jitter (DIBR): this update looks a sub-pixel step off the head, and its record
        // says so (the late warp and the compositor put the frame back exactly), so the eyes'
        // temporal AA (eye_taa) gathers samples between the render's pixels. Halton (2, 3), 8
        // steps, +-0.5 px (x eye_taa_jitter) at the centre of a 1920-high render (other sizes:
        // proportionally). Only while the TAA accumulates with the world standing still
        // (EyeTaaAccumulating): walking or turning, the history restarts every refresh (with
        // eye_taa_follow: on the body, held items and the near field) and a jitter would only add
        // shimmer.
        float jitter_yaw = 0.0f;
        // (Never with aa_mode=taa: the render TAA jitters the frame itself, in clip space.)
        if (s.eye_taa > 0.0f && s.eye_taa_jitter > 0.0f && stereo.stereo && stereo.mode == 2 &&
            !menu_hold && EyeTaaAccumulating() && s.aa_mode != Aa::Mode::Taa) {
            static constexpr std::array<std::array<float, 2>, 8> Halton{{
                {0.5f, 1.0f / 3.0f},
                {0.25f, 2.0f / 3.0f},
                {0.75f, 1.0f / 9.0f},
                {0.125f, 4.0f / 9.0f},
                {0.625f, 7.0f / 9.0f},
                {0.375f, 2.0f / 9.0f},
                {0.875f, 5.0f / 9.0f},
                {0.0625f, 8.0f / 9.0f},
            }};
            const auto& h = Halton[st.next_record_id % Halton.size()];
            const float per_px = s.eye_taa_jitter * 2.0f * std::tan(0.5f * vfov) / 1920.0f;
            jitter_yaw = (h[0] - 0.5f) * per_px;
            view_yaw = WrapAngle(view_yaw + jitter_yaw);
            head_pitch -= (h[1] - 0.5f) * per_px;
        }
        static const bool fov_test = std::getenv("VR_FOV_TEST") != nullptr;
        if (fov_test) {
            // FOV calibration: odd records look exactly 10 degrees right.
            view_yaw = WrapAngle(view_yaw + ((st.next_record_id & 1) ? 10.0f * DegToRad : 0.0f));
        }
        if (pair_first && st.shift_log_pending) {
            // A recenter's evidence: the view keeps its direction across the change of space.
            st.shift_log_pending = false;
            LOG_INFO(Core,
                     "VR: recenter: view yaw {:.2f} deg on the first update in the new space (the "
                     "last before it {:.2f}), head yaw {:.2f} deg at ({:.3f}, {:.3f}, {:.3f}), "
                     "game heading of forward {:.2f} deg",
                     view_yaw / DegToRad, st.last_view_yaw / DegToRad, head_yaw / DegToRad,
                     head.head.position.x, head.head.position.y, head.head.position.z,
                     st.base_yaw / DegToRad);
        }
        st.last_view_yaw = view_yaw;
        // render_pitch_bias_deg: the render camera's pitch, after the pitch limit (and the
        // jitter); the view (your head) keeps its own.
        const float render_head_pitch =
            bias != 0.0f ? Tilt::RenderPitch(head_pitch, bias) : head_pitch;
        result.active = true;
        result.decoupled = decoupled;
        result.view_yaw = view_yaw;
        result.view_pitch = -head_pitch;
        result.render_pitch = -render_head_pitch;
        result.view_roll = head_roll;
        // Decoupled: the orbit only follows turning, so head motion never moves the camera.
        // First person: the camera sits in the head, so the orbit follows the view without
        // swinging the camera around, and the character walks where the player looks.
        result.orbit = !game_orbit;
        const bool orbit_follows_view = !decoupled || (IsFirstPerson() && !game_orbit);
        result.yaw = orbit_follows_view ? view_yaw : st.base_yaw;
        result.pitch = orbit_follows_view ? -head_pitch : st.orbit_pitch;
        // Where this update leaves the game camera (a gap's check, core/vr/camera_gap.h): the
        // yaw written into its orbit, or its own while the game steers it.
        st.gap_last.valid = true;
        st.gap_last.yaw = result.orbit ? result.yaw : game_yaw;
        st.gap_last.anchor_valid = game_anchor != nullptr;
        if (game_anchor != nullptr) {
            st.gap_last.anchor[0] = game_anchor->x;
            st.gap_last.anchor[1] = game_anchor->y;
            st.gap_last.anchor[2] = game_anchor->z;
        }
        result.vfov = vfov;
        result.pair_first = pair_first;
        st.game_owns.store(game_orbit, std::memory_order_relaxed);

        // AER: move the render camera half the IPD along the view's right axis (game right =
        // (cos y, 0, -sin y)). The presenter declares the eye poses with this same distance.
        // camera_roll: along the rolled right axis, right * cos r + up * sin r (game up =
        // (sin y sin p, cos p, cos y sin p), view pitch p + = down).
        const float ipd = EffectiveIpd(head);
        Vec3 offset{0.0f, 0.0f, 0.0f};
        if (eye >= 0) {
            const float half = (eye == 0 ? -0.5f : 0.5f) * ipd * s.world_scale;
            const float cr = std::cos(head_roll), sr = std::sin(head_roll);
            const float cy = std::cos(view_yaw), sy = std::sin(view_yaw);
            const float cp = std::cos(result.view_pitch), sp = std::sin(result.view_pitch);
            offset.x += (cy * cr + sy * sp * sr) * half;
            offset.y += cp * sr * half;
            offset.z -= (sy * cr - cy * sp * sr) * half;
        }
        record.eye_offset = offset;
        // Your height (core/vr/player_body.h; the page's General > Body: Height and Adjust
        // height). The 6DoF origin is the recenter's head (where your head was at the recenter or
        // the camera's takeover; your ducking, leaning and the room are measured from it, as in
        // every build), and the camera's eye moves by the height's eye offset, added after the
        // room's clamp below:
        //  - As the hunter: Adjust height alone (+5 = the view exactly 5 cm higher). + 0 adds
        //    nothing: the camera bit for bit as before the height rows ("game height not set").
        //  - As the player: your eye over the headset's floor at the origin minus the hunter's eye,
        //    plus Adjust height: the camera's eye over his feet is your real eye over the floor
        //    (+ the adjustment), standing or ducking, whatever your head did at the recenter. No
        //    floor known yet, or Stance: Seated: as the hunter.
        // The floor guard keeps the camera FloorMarginM over his feet from the standing eye, and
        // the late warp (which re-sees the frame by the head's change from the origin) never adds
        // the offset twice.
        const Vec3 origin = st.head_origin;
        const Vec3 neck_origin = st.neck_origin;
        const PlayerBody::HeightBase height_base =
            s.height_player ? PlayerBody::HeightBase::Player : PlayerBody::HeightBase::Hunter;
        float eye_offset = PlayerBody::EyeOffsetM(height_base, s.height_adjust_cm,
                                                  s.fp_eye_height_m, st.floor_known,
                                                  origin.y - st.floor_y, s.seated);
        int height_mode = 0;
        if (s.height_player) {
            if (s.seated) {
                height_mode = 3;
            } else if (st.floor_known) {
                height_mode = 1;
                st.player_offset_known = true;
                st.player_offset = eye_offset;
            } else if (st.player_offset_known) {
                height_mode = 1; // the floor of the last tick that had it (a recenter's sample)
                eye_offset = st.player_offset;
            } else {
                height_mode = 2;
            }
        }
        st.eye_offset_now.store(eye_offset, std::memory_order_relaxed);
        const float stand_eye = PlayerBody::StandingEyeM(eye_offset, s.fp_eye_height_m);
        // The late warp re-sees the frame from the head at display time the same way.
        record.head_origin = origin;
        record.roomscale = s.roomscale_walk && s.positional && !s.seated && IsFirstPerson() &&
                           render_camera && !game_orbit_mode;
        record.head_offset_limit_m = s.positional ? std::max(s.positional_limit_m, 0.0f) : 0.0f;
        record.head_offset_down_m =
            s.positional ? HeadRoom::DownLimit(s.positional_limit_m, s.positional_limit_down_m,
                                               stand_eye)
                         : 0.0f;
        float head_dy = 0.0f; // your head's height from the origin, after the room's clamp (m)
        if (s.positional) {
            // Real head movement (LOCAL space, origin at the recentered head). Tracking -Z
            // is the game heading base_yaw: right = (cos b, 0, -sin b), fwd = (sin b, 0, cos b).
            // The room (core/vr/head_room.h): positional_limit_m up, sideways and forward, down
            // to positional_limit_down_m below the origin.
            const Vec3& h = head.head.position;
            Vec3 d{h.x - origin.x, h.y - origin.y, h.z - origin.z};
            HeadRoom::Clamp(d.x, d.y, d.z, record.head_offset_limit_m, record.head_offset_down_m,
                            record.roomscale);
            const float cb = std::cos(st.base_yaw), sb = std::sin(st.base_yaw);
            const float r = d.x * s.world_scale, f = -d.z * s.world_scale;
            offset.x += cb * r + sb * f;
            offset.y += d.y * s.world_scale;
            offset.z += -sb * r + cb * f;
            head_dy = d.y;
        }
        if (eye_offset != 0.0f) {
            offset.y += eye_offset * s.world_scale;
        }
        // The body's offset (HeightBodyOffsetM, player_body.h BodyOffsetM): As the player your head
        // above the origin raises it with you (standing up after a recenter taken bent); else the
        // eye offset itself (As the hunter bit for bit).
        st.body_offset_now.store(PlayerBody::BodyOffsetM(eye_offset, height_mode == 1, head_dy),
                                 std::memory_order_relaxed);
        st.eye_offset_set.store(true, std::memory_order_release);
        // The log: a change of the rows, of As the player's floor, or of the offset by 5 mm.
        if (pair_first && (height_mode != st.height_mode_logged ||
                           s.height_adjust_cm != st.height_adjust_logged ||
                           std::abs(eye_offset - st.eye_offset_logged) >= 0.005f)) {
            st.height_mode_logged = height_mode;
            st.height_adjust_logged = s.height_adjust_cm;
            st.eye_offset_logged = eye_offset;
            static constexpr std::array<const char*, 4> ModeText{
                "As the hunter", "As the player",
                "As the player (no floor known yet: as the hunter)",
                "As the player (Stance: Seated: as the hunter)"};
            LOG_INFO(Core,
                     "VR: height: {} {}: {}; standing straight the camera's eye is {:.3f} m over "
                     "the hunter's feet (his own eye {:.3f} m: {:+.3f} m, a {:.0f} cm person's); "
                     "now {:.3f} m (your head {:+.3f} m from the origin)",
                     ModeText[static_cast<size_t>(height_mode)],
                     PlayerBody::AdjustText(s.height_adjust_cm),
                     height_mode == 1
                         ? fmt::format("your eye {:.3f} m over the headset's floor at the 6DoF "
                                       "origin (floor at y {:.3f}, origin y {:.3f} in tracking "
                                       "space)",
                                       origin.y - st.floor_y, st.floor_y, origin.y)
                         : std::string{"standing straight = your head at the 6DoF origin (the "
                                       "recenter's)"},
                     stand_eye * s.world_scale, s.fp_eye_height_m * s.world_scale,
                     eye_offset * s.world_scale,
                     PlayerBody::StatureCm(eye_offset, s.fp_eye_height_m),
                     (stand_eye + head_dy) * s.world_scale, head_dy);
        }
        // VR_HEIGHT_AB=1 (tests): every 600 ticks, how many moved the camera by an eye offset (0
        // with As the hunter + 0 cm: the camera of the builds before the height rows, bit for bit)
        // and how many head samples had a floor, and the camera's eye over his feet now.
        static const bool height_ab = std::getenv("VR_HEIGHT_AB") != nullptr;
        if (height_ab) {
            static u32 ticks = 0, moved = 0, floors = 0;
            ++ticks;
            moved += eye_offset != 0.0f ? 1 : 0;
            floors += live.floor_valid ? 1 : 0;
            if (ticks % 600 == 0) {
                LOG_INFO(Core,
                         "VR height A/B: {} camera ticks, {} with an eye offset, {} head samples "
                         "with a floor; {} {}: the camera's eye {:.4f} m over his feet (standing "
                         "{:.4f}, your head {:+.4f})",
                         ticks, moved, floors, PlayerBody::BaseText(height_base),
                         PlayerBody::AdjustText(s.height_adjust_cm), stand_eye + head_dy,
                         stand_eye, head_dy);
            }
        }
        result.render_offset = offset;
        // Physical evasion: the neck pivot's offset from its origin in game world axes (metres),
        // so turning or nodding the head never moves the hurtboxes. From this tick's own head
        // sample: in AER the camera's pair_head only changes on a pair's first eye, which would
        // update the lean every second game frame.
        const HeadState& neck_head = sampled.valid ? sampled : head;
        result.neck_valid = s.positional && neck_head.valid;
        result.head_tracked = neck_head.valid;
        if (result.neck_valid) {
            const Vec3 neck = NeckPivot(neck_head.head);
            const Vec3 n{neck.x - neck_origin.x, neck.y - neck_origin.y, neck.z - neck_origin.z};
            const float cb = std::cos(st.base_yaw), sb = std::sin(st.base_yaw);
            const float r = n.x, f = -n.z;
            result.neck_offset = {cb * r + sb * f, n.y, -sb * r + cb * f};
        }
        result.eye_offset = record.eye_offset;
        result.head_position = head.head.position;
        result.base_yaw = st.base_yaw;
        result.world_scale = s.world_scale;
        result.eye = eye;
        result.aer = aer;
        record.eye = eye;
        record.shift_eye = shift_eye;
        record.pair = aer ? st.pair_serial : 0;
        // DIBR: no eye offset (eye = -1, so every update starts a "pair" like mono); the centre
        // camera's frame is warped into both eyes. Menu panel frames are never warped.
        record.dibr = stereo.stereo && stereo.mode == 2 && !menu_hold;
        record.menu = menu_hold;

        record.driven = true;
        // VR_LAG_TEST=2: the view turned right by the offset is a head turned right by it (head
        // yaw + = left), so the frame is submitted where it looks.
        record.head_yaw = WrapAngle(head_yaw - lag_test_yaw - jitter_yaw);
        // render_pitch_bias_deg: the render camera's pitch (what re-sees the frame starts from it:
        // the late warp undoes the bias), and the bias that the head's own pitch adds back.
        record.head_pitch = render_head_pitch;
        record.pitch_bias = head_pitch - render_head_pitch;
        record.head_roll = head_roll;
        record.head_position = head.head.position;
        record.head_time = head.time;
        record.space_serial = head.space_serial;
        record.vfov = vfov;
        record.ipd = ipd;
        // The eye FOVs of this head sample: each eye image is submitted with them.
        record.eye_fov = head.eye_fov;
        if (KeepEyeFov()) {
            EyeFov::Update(st.last_valid_eye_fov, head.eye_fov);
            record.eye_fov = st.last_valid_eye_fov;
        }
        record.turn_serial = st.turn_serial;
        record.base_yaw = st.base_yaw;
        record.world_scale = s.world_scale;
        st.current_vfov.store(vfov, std::memory_order_relaxed);
    } else {
        st.driving = false;
        st.gap_pending = false; // not a gap: the bridge stopped driving (the takeover is full)
        st.pending_turn.store(0.0f);
        st.game_owns.store(true, std::memory_order_relaxed);
        // Not driving: no pair is open, so a live mode switch applies at once.
        st.stereo_word.store(st.stereo_request.load(), std::memory_order_release);
        record.vfov = game_vfov;
        if (KeepEyeFov())
            record.eye_fov = st.last_valid_eye_fov;
    }
    record.aspect = aspect_known ? game_aspect : 0.0f;
    record.tick_ns = now;

    {
        std::scoped_lock lk{st.record_mutex};
        record.id = st.next_record_id++;
        st.records[record.id % State::RecordCount] = record;
        if (st.camera_thread.empty()) {
            st.camera_thread = Common::GetCurrentThreadName();
        }
    }
    result.record_id = record.id;
    return result;
}

Vec3 RoomscaleWalkInput(bool allowed, const char* why) {
    auto& st = S();
    const s64 now = NowNs();
    std::scoped_lock lk{st.roomscale_mutex};
    allowed=allowed && !Interactions::ProcessFaultLatch().Tripped();
    st.roomscale_allowed = allowed;
    st.roomscale_why = why != nullptr ? why : "";
    st.roomscale_input_ns = now;
    st.roomscale_applied = allowed && now >= st.roomscale_output_ns &&
                          now - st.roomscale_output_ns < 150'000'000 && st.roomscale_out.walking;
    if (!allowed) st.roomscale_out = {};
    return st.roomscale_applied
               ? Vec3{st.roomscale_out.right, 0.0f, st.roomscale_out.forward} : Vec3{};
}

void UpdateRoomscaleWalk(CameraOverride& camera, const Vec3& base, bool first_person) {
    auto& st = S();
    const auto& s = st.settings;
    const s64 now = NowNs();
    std::scoped_lock lk{st.roomscale_mutex};
    Body::BodyStatus body{};
    const bool body_ok = Body::LatestBodyStatus(body) && now >= body.steady_ns &&
                         now - body.steady_ns < 150'000'000 && body.roomscale_ok;
    if(st.roomscale_module && Interactions::ProcessFaultLatch().Tripped())st.roomscale_fault=true;
    const bool allowed = !st.roomscale_fault && s.roomscale_walk && s.positional && !s.seated && first_person &&
                         camera.active && camera.head_tracked && !IsGameMenuOpen() &&
                         !MenuPanelActive() && !CinematicActive() && body_ok &&
                         st.roomscale_allowed && now >= st.roomscale_input_ns &&
                         now - st.roomscale_input_ns < 150'000'000;
    // Origins only advance on the first eye; the second eye already used that origin.
    if (!camera.pair_first && allowed) return;
    Roomscale::Frame f{};
    f.allowed = allowed;
    f.applied = st.roomscale_applied;
    f.time = static_cast<double>(now) * 1e-9;
    f.space = st.shift_applied;
    f.turn = st.turn_serial;
    f.scale = s.world_scale;
    f.base_yaw = camera.base_yaw;
    f.view_yaw = camera.view_yaw;
    f.base = {base.x, base.z};
    f.head = {camera.head_position.x, camera.head_position.z};
    const auto& posture=Body::GetBodySettings();
    const auto decision=st.roomscale_walk.Update(f,{s.roomscale_start_m,s.roomscale_stop_m,s.roomscale_max_stick},
        {camera.neck_offset.x,camera.neck_offset.z},body.roomscale_yaw,posture.body_lean_side_deg,posture.body_align,posture.body_posture_lean,posture.body_align_max_m);
    f.offset=decision.tracking_offset;const auto& out=decision.motion;const float lean_scale=decision.lean_scale;
    if (out.walking != st.roomscale_logged_walking) {
        LOG_INFO(Core, "VR roomscale: {} (offset {:.2f} m, lean allowance x{:.2f}, allowed {})",
                 out.walking ? "walking" : "stopped", Roomscale::Length(f.offset - out.consume),
                 lean_scale, allowed);
        st.roomscale_logged_walking = out.walking;
    }
    // Out past the start for a second without walking: why (at most every 3 s).
    const float far = Roomscale::Length(f.offset);
    const float start=decision.start_distance;
    st.roomscale_far_s = !out.walking && far >= start
                             ? st.roomscale_far_s + GameFrameSeconds()
                             : 0.0f;
    if (st.roomscale_far_s >= 1.0f && now - st.roomscale_why_ns >= 3'000'000'000) {
        st.roomscale_why_ns = now;
        const bool fresh = now >= st.roomscale_input_ns && now - st.roomscale_input_ns < 150'000'000;
        const char* why = allowed               ? "allowed: the step is not continuous"
                          : !s.roomscale_walk   ? "roomscale_walk=0"
                          : !s.positional       ? "positional=0"
                          : s.seated            ? "seated"
                          : !first_person       ? "not first person"
                          : !camera.active      ? "the camera is not driven"
                          : !camera.head_tracked ? "the head is not tracked"
                          : IsGameMenuOpen()    ? "a game menu is open"
                          : MenuPanelActive()   ? "the menu panel is up"
                          : CinematicActive()   ? "a cinematic"
                          : !body_ok            ? "the body"
                          : !fresh              ? "the presenter's input is stale"
                                                : "the presenter";
        const char* detail = allowed ? ""
                             : !body_ok ? body.roomscale_why
                             : body_ok && fresh ? st.roomscale_why
                                                : "";
        LOG_INFO(Core, "VR roomscale: not walking, {:.2f} m out (starts at {:.2f}): {}{}{}", far,
                 start, why, detail[0] != '\0' ? ": " : "", detail);
    }
    st.roomscale_out = out;
    st.roomscale_output_ns = now;
    const auto delta=decision.world_consume;
    st.head_origin.x += out.consume.x;
    st.head_origin.z += out.consume.z;
    st.neck_origin.x += out.consume.x;
    st.neck_origin.z += out.consume.z;
    camera.render_offset.x -= delta.x * s.world_scale;
    camera.render_offset.z -= delta.z * s.world_scale;
    camera.neck_offset.x -= delta.x;
    camera.neck_offset.z -= delta.z;
    // The late warp must use the origin that placed this frame, even with frames in flight.
    std::scoped_lock record_lk{st.record_mutex};
    auto& record = st.records[camera.record_id % State::RecordCount];
    if (record.id == camera.record_id) record.head_origin = st.head_origin;
}

void SetRecordRenderPosition(u64 id, const Vec3& position) {
    auto& st = S();
    std::scoped_lock lk{st.record_mutex};
    auto& record = st.records[id % State::RecordCount];
    if (record.id == id) {
        record.render_position = position;
        record.has_render_position = true;
    }
}

void SetRecordEyeForward(u64 id, float forward_m, bool first_person) {
    auto& st = S();
    std::scoped_lock lk{st.record_mutex};
    auto& record = st.records[id % State::RecordCount];
    if (record.id == id) {
        record.eye_forward_m = forward_m;
        record.first_person = first_person;
    }
}

void SetRecordFloorDip(u64 id, float dip_m) {
    auto& st = S();
    std::scoped_lock lk{st.record_mutex};
    auto& record = st.records[id % State::RecordCount];
    if (record.id == id && std::isfinite(dip_m)) {
        record.floor_dip_m = dip_m;
        record.has_floor_dip = true;
    }
}

void SetRecordPlayerPosition(u64 id, const Vec3& position) {
    auto& st = S();
    std::scoped_lock lk{st.record_mutex};
    auto& record = st.records[id % State::RecordCount];
    if (record.id == id && std::isfinite(position.x) && std::isfinite(position.y) &&
        std::isfinite(position.z)) {
        record.player_position = position;
        record.has_player_position = true;
    }
}

void SetRecordHeldProxy(u64 id, const Vec3& a, const Vec3& b, float radius) {
    auto& st = S();
    std::scoped_lock lk{st.record_mutex};
    auto& record = st.records[id % State::RecordCount];
    if (record.id == id) {
        record.held_a = a;
        record.held_b = b;
        record.held_radius = radius;
        record.has_held_proxy = true;
    }
}

void SetRecordProjection(u64 id, float vfov, float aspect, float near_z, float far_z) {
    auto& st = S();
    std::scoped_lock lk{st.record_mutex};
    auto& record = st.records[id % State::RecordCount];
    if (record.id == id) {
        record.proj_vfov = vfov;
        record.proj_aspect = aspect;
        record.near_z = near_z;
        record.far_z = far_z;
        record.has_projection = true;
    }
}

u64 OnFlipSubmitted() {
    if (!IsEnabled()) {
        return 0;
    }
    auto& st = S();
    std::scoped_lock lk{st.record_mutex};
    if (st.flip_count == State::FlipQueueSize) {
        // Never flipped (a failed submission): the oldest entry goes.
        st.flip_head = (st.flip_head + 1) % State::FlipQueueSize;
        --st.flip_count;
        ++st.flips_dropped;
    }
    const u64 ticket = st.next_flip_ticket++;
    st.flip_queue[(st.flip_head + st.flip_count) % State::FlipQueueSize] = {ticket,
                                                                            st.next_record_id - 1};
    ++st.flip_count;
    st.flip_depth_max = std::max(st.flip_depth_max, st.flip_count);
    if (st.submit_thread.empty()) {
        st.submit_thread = Common::GetCurrentThreadName();
    }
    return ticket;
}

bool LagTestEnabled() {
    return LagTestMode() != 0;
}

bool FlipTagTest() {
    static const bool flip = [] {
        const char* value = std::getenv("VR_TAG_SOURCE");
        return value != nullptr && std::string_view{value} == "flip";
    }();
    return flip;
}

s32 LagTestMode() {
    static const s32 mode = [] {
        const char* value = std::getenv("VR_LAG_TEST");
        const s32 mode = value == nullptr ? 0 : std::atoi(value);
        return value == nullptr ? 0 : (mode == 2 || mode == 4) ? mode : 1;
    }();
    return mode;
}

bool GetRecord(u64 id, CameraRecord& record) {
    auto& st = S();
    std::scoped_lock lk{st.record_mutex};
    const auto& stored = st.records[id % State::RecordCount];
    if (id == 0 || stored.id != id) {
        return false;
    }
    record = stored;
    return true;
}

// Debug: VR_DEBUG_DRAWS=1 logs the display-buffer draws of two gameplay frames; =menu also logs
// two frames 40 frames into every menu (once it has faded in).
static int g_debug_frames = -1;
static int g_debug_menu_frames = -1; // frames since a menu opened (-1 = none, VR_DEBUG_DRAWS=menu)
static bool g_debug_menu = false;
static bool g_debug_menu_open = false;
bool DebugDrawLog() {
    return (g_debug_frames >= 300 && g_debug_frames < 302) ||
           (g_debug_menu_frames >= 40 && g_debug_menu_frames < 42);
}

FrameTag OnGpuFrameBoundary(u64 flip) {
    auto& st = S();
    FrameTag tag{};
    tag.valid = IsEnabled();
    if (!tag.valid) {
        return tag;
    }
    const StereoConfig stereo_config = GetStereo();

    bool latest_driven = false;
    {
        std::scoped_lock lk{st.record_mutex};
        const u64 latest = st.next_record_id - 1;
        latest_driven = st.records[latest % State::RecordCount].driven;
        // A hitch produces no flips, so counting flips without a camera update (instead of
        // wall time) keeps shader-compile stalls from flashing the virtual screen, while
        // cutscenes and loading screens (which keep flipping) still switch within ~4 frames.
        if (latest != st.last_boundary_record) {
            st.last_boundary_record = latest;
            st.boundaries_without_tick = 0;
        } else if (st.boundaries_without_tick < 1000) {
            ++st.boundaries_without_tick;
        }

        // The frame's own camera update: the one queued when the game submitted this flip
        // (the game thread may already have placed the camera for the next frame). Older
        // entries were never flipped and are dropped.
        u64 frame_record = latest;
        bool matched = false;
        while (flip != 0 && st.flip_count != 0) {
            const State::FlipEntry entry = st.flip_queue[st.flip_head];
            if (entry.ticket > flip) {
                break;
            }
            st.flip_head = (st.flip_head + 1) % State::FlipQueueSize;
            --st.flip_count;
            if (entry.ticket == flip) {
                frame_record = entry.record;
                matched = true;
                break;
            }
            ++st.flips_dropped;
        }
        if (!matched) {
            ++st.flips_unmatched;
        } else if (frame_record != latest) {
            ++st.flips_older;
            st.flips_older_max = std::max(st.flips_older_max, latest - frame_record);
        }
        // pose_lag_frames: a constant correction on top (measured 0).
        tag.record = LaggedRecord(st, frame_record, latest);
        tag.flip_record = tag.record.id;
        tag.seq = ++st.frame_seq;
        // The update the frame's draws used: the one current at its first draw (StereoForDraw).
        // The flip's is the newest when the game submitted the flip, by then often already the
        // next frame's (Game:Main places the next camera while GXRenderThread still builds this
        // frame): measured with VR_LAG_TEST=4, one too new on 0-100% of the frames depending on
        // the session, the first draw's on none. Every frame goes out with it: the DIBR capture
        // and its submitted pose, an AER frame's eye and pair (with the flip's they swap: reversed
        // stereo), a mono frame's pose. VR_TAG_SOURCE=flip keeps the flip's (tests).
        const u64 drawn = std::exchange(st.frame_first_record, 0);
        st.frame_drawn = false;
        if (drawn != 0 && drawn != tag.record.id && !FlipTagTest()) {
            const CameraRecord& record = st.records[drawn % State::RecordCount];
            if (record.id == drawn) {
                tag.record = record;
                ++st.flips_redrawn;
            }
        }
        if (++st.flips >= 600) {
            LOG_INFO(Core,
                     "VR flips: {} of the last {} used an older camera update than the newest at "
                     "the flip (by up to {}), {} had no queued update (newest used), {} queue "
                     "entries dropped, queue depth max {}, {} tagged with their first draw's "
                     "update instead of the flip's{}; flips submitted on '{}', camera updates on "
                     "'{}'",
                     st.flips_older, st.flips, st.flips_older_max, st.flips_unmatched,
                     st.flips_dropped, st.flip_depth_max, st.flips_redrawn,
                     FlipTagTest() ? " (VR_TAG_SOURCE=flip: the flip's kept)" : "",
                     st.submit_thread, st.camera_thread);
            st.flips = st.flips_older = st.flips_unmatched = st.flips_dropped = 0;
            st.flips_redrawn = 0;
            st.flips_older_max = 0;
            st.flip_depth_max = st.flip_count;
        }
    }

    // Drawn for the menu panel (not immersive, the HUD at full size), or rendered with the frozen
    // menu view (the first frame after a menu opened may still have been drawn as an immersive
    // frame; its camera says it belongs on the panel, not in the eyes).
    tag.menu = st.current_frame_menu || tag.record.menu;
    // The GPU runs about a frame behind the game, so at a menu's open and close a frame or two
    // were drawn one way (panel: full-size HUD, or immersive) with a camera placed the other way
    // (the frozen panel view, or the live wide one): squeezed into the panel or with its menu
    // shrunk. They are not shown in the headset (the last panel image or eye images stay up), at
    // most 4 in a row so a camera that stopped updating can never hold the headset's image.
    if (tag.record.driven && st.current_frame_menu != tag.record.menu &&
        st.menu_held_frames < 4) {
        ++st.menu_held_frames;
        tag.hold = true;
        LOG_INFO(Core, "VR menu: frame not shown in the headset (drawn for the {}, camera placed "
                       "for the {})",
                 st.current_frame_menu ? "panel" : "eyes", tag.record.menu ? "panel" : "eyes");
    } else {
        st.menu_held_frames = 0;
    }
    tag.immersive = st.current_frame_immersive && tag.record.driven && !tag.menu;
    tag.ui_layer = tag.immersive ? static_cast<u8>(st.current_frame_ui) : 0;
    tag.ui_class = st.current_frame_ui_class;
    // DIBR: the frame was rendered with the centre camera in stereo_mode 2 (the record says so,
    // whatever the mode is now), so its eyes are synthesised; tag.eye stays -1.
    tag.dibr = tag.immersive && tag.record.dibr;
    if (tag.record.eye >= 0) {
        // stereo_mode 1: the eye was chosen when the camera was placed for this frame.
        tag.eye = tag.immersive ? tag.record.eye : -1;
        tag.pair = tag.record.pair;
        tag.stereo_shift = 0.0f;
    } else {
        tag.eye = (tag.immersive && st.current_draw_shift != 0.0f) ? st.current_eye : -1;
        tag.stereo_shift = tag.eye >= 0 ? st.current_draw_shift : 0.0f;
        // stereo_mode 0: a pair is a left frame and the right frame after it.
        if (tag.eye == 0) {
            ++st.shift_pair;
        }
        tag.pair = tag.eye >= 0 ? st.shift_pair : 0;
    }
    if (tag.record.eye >= 0 && tag.eye >= 0 && tag.eye == st.last_tag_eye) {
        ++st.eye_repeats; // two frames in a row for one eye: camera ticks != frames
    }
    st.last_tag_eye = tag.record.eye >= 0 ? tag.eye : -1;
    if (++st.boundary_count % 600 == 0) {
        if (st.eye_repeats != 0) {
            LOG_WARNING(Core, "VR: {} repeated-eye frames in the last 600", st.eye_repeats);
            st.eye_repeats = 0;
        }
        if (st.shift_realigns != 0) {
            LOG_INFO(Core,
                     "VR: stereo_mode 0 repeated an eye {} times in the last 600 frames to stay in "
                     "step with the camera updates' pairs",
                     st.shift_realigns);
            st.shift_realigns = 0;
        }
    }

    if (g_debug_frames == -1) {
        const char* draws = std::getenv("VR_DEBUG_DRAWS");
        g_debug_frames = draws ? 0 : -2;
        g_debug_menu = draws != nullptr && std::string_view{draws} == "menu";
    }
    if (g_debug_frames >= 0 && IsCameraActive() && ++g_debug_frames > 400) {
        g_debug_frames = -2;
    }
    if (g_debug_menu) {
        const bool open = IsGameMenuOpen();
        if (open && !g_debug_menu_open) {
            g_debug_menu_frames = 0;
            LOG_INFO(Core, "VR draws: a menu opened ({}); its draws are logged in 40 frames",
                     UiClassName(GameMenuClass()));
        } else if (g_debug_menu_frames >= 0 && ++g_debug_menu_frames > 60) {
            g_debug_menu_frames = -1;
        }
        g_debug_menu_open = open;
    }

    // Prepare the next frame's draws: the other eye. stereo_mode 0 keeps the eye in step with the
    // camera updates' parity (only a left frame's update moves the whole view, so a pair is a left
    // frame and the right frame after it): when they disagree, the next frame repeats this eye,
    // at most every other frame.
    st.hud_phase = false;
    s32 next_eye = st.current_eye ^ 1;
    if (tag.record.driven && tag.record.shift_eye >= 0) {
        const s32 follow = tag.record.shift_eye ^ 1; // the next update's
        if (follow != next_eye && !st.shift_realigned) {
            next_eye = follow;
            st.shift_realigned = true;
            ++st.shift_realigns;
        } else {
            st.shift_realigned = false;
        }
    }
    st.current_eye = next_eye;
    const s64 last_tick = st.last_tick_ns.load(std::memory_order_relaxed);
    const bool ticking = st.boundaries_without_tick < 4 && last_tick != 0 &&
                         NowNs() - last_tick < 1'000'000'000; // wall-clock backstop
    // A menu on the panel: the next frame is mono with its HUD and menus at full size.
    st.current_frame_menu = MenuPanelActive();
    st.current_frame_immersive = GetDisplayMode() == DisplayMode::Auto && ticking &&
                                 latest_driven && !st.current_frame_menu;
    st.immersive_now.store(st.current_frame_immersive, std::memory_order_relaxed);
    const float vfov = st.current_vfov.load(std::memory_order_relaxed);
    // Only stereo_mode 0 shifts geometry in clip space; stereo_mode 1 moves the camera and
    // stereo_mode 2 renders the centre (the HUD gets its disparity in the DIBR warp).
    const bool shift_mode = stereo_config.mode == 0;
    const bool stereo = st.current_frame_immersive && stereo_config.stereo && vfov > 0.0f;
    st.current_draw_shift = (stereo && shift_mode) ? ComputeShift(st.current_eye, vfov) : 0.0f;
    st.current_hud = 0.0f;
    if (st.current_frame_immersive) {
        // stereo_mode 1 refines the eye when the HUD phase starts (see StereoForDraw).
        st.current_hud = HudValue(stereo && shift_mode ? st.current_eye : -1, vfov);
    }
    st.current_frame_dibr =
        st.current_frame_immersive && stereo_config.stereo && stereo_config.mode == 2;
    // aa_mode=taa: the next frame's sub-pixel jitter (Halton 2, 3) while the resolve keeps
    // running (a jittered frame it did not resolve counts; 8 in a row stop the jitter until a
    // resolve comes again). Its NDC step is made for the last resolve's render size.
    {
        const auto& ts = st.settings;
        if (st.current_taa_jitter.on) {
            st.taa_unresolved = st.taa_resolved_frame ? 0 : st.taa_unresolved + 1;
        }
        st.taa_resolved_frame = false;
        st.current_taa_jitter = {};
        if (st.current_frame_dibr && ts.aa_mode == Aa::Mode::Taa && ts.taa_jitter > 0.0f &&
            st.taa_width > 0 && st.taa_height > 0 && st.taa_unresolved < 8) {
            RenderTaa::Jitter px =
                RenderTaa::Halton(++st.taa_phase, static_cast<std::uint32_t>(ts.taa_phases));
            px.x *= ts.taa_jitter;
            px.y *= ts.taa_jitter;
            const RenderTaa::Jitter ndc = RenderTaa::JitterNdc(
                px, static_cast<float>(st.taa_width), static_cast<float>(st.taa_height));
            st.current_taa_jitter = {
                .on = true,
                .ndc_x = ndc.x,
                .ndc_y = ndc.y,
                .px_x = px.x,
                .px_y = px.y,
                .phase = st.taa_phase,
                .width = st.taa_width,
                .height = st.taa_height,
            };
        }
    }
    // The UI layer: immersive frames in DIBR, AER and mono (the clip-shift mode keeps the HUD
    // panel); check mode needs no presenter layer.
    const s32 ui = st.settings.ui_layer;
    st.current_frame_ui = st.current_frame_immersive &&
                                  (!stereo_config.stereo || stereo_config.mode != 0) &&
                                  (ui == 2 || (ui == 1 && UiLayerAvailable()))
                              ? ui
                              : 0;
    st.current_frame_ui_class = static_cast<u8>(GameMenuClass());
    return tag;
}

DrawStereo StereoForDraw(u32 target_width, u32 target_height, bool has_depth,
                         u64 vertex_shader_hash) {
    auto& st = S();
    if (!st.frame_drawn && IsEnabled()) {
        // The frame's first draw: the camera update it is built with, which it is tagged with at
        // its flip (OnGpuFrameBoundary) and the DIBR capture uses (Rasterizer::VrTrackTargets).
        st.frame_drawn = true;
        std::scoped_lock lk{st.record_mutex};
        st.frame_first_record = DrawFrameRecordLocked(st).id;
    }
    if (!st.current_frame_immersive || target_height == 0) {
        return {};
    }
    const bool hud = st.hud_phase;
    if (vertex_shader_hash == st.settings.hud_after_vs) {
        st.hud_phase = true; // the following draws are HUD and menus
        // stereo_mode 1: the HUD depth follows the eye of the frame being drawn: the camera
        // update of its first draw, which the frame is tagged with (pose_lag_frames included),
        // so the HUD disparity is always for the eye whose image it ends up in. (The update
        // current here, at the composite, is already the next frame's on 8-34% of the frames.)
        s32 eye = -1;
        float frame_vfov = 0.0f;
        float frame_aspect = 0.0f;
        {
            std::scoped_lock lk{st.record_mutex};
            const CameraRecord* record = &FrameRecordForDrawsLocked(st);
            const CameraRecord& first = st.records[st.frame_first_record % State::RecordCount];
            if (st.frame_first_record != 0 && first.id == st.frame_first_record &&
                !FlipTagTest()) {
                record = &first;
            }
            eye = record->eye;
            frame_vfov = record->vfov;
            frame_aspect = record->aspect;
        }
        if (eye >= 0) {
            st.current_hud = HudValue(eye, frame_vfov, frame_aspect);
        }
    }
    const float aspect = static_cast<float>(target_width) / static_cast<float>(target_height);
    const float main = st.main_aspect.load(std::memory_order_relaxed);
    if (std::abs(aspect - main) > 0.02f * main) {
        return {}; // shadow maps, reflection probes, atlases
    }
    if (hud) {
        return {st.current_draw_shift, st.current_hud};
    }
    if (st.current_taa_jitter.on) {
        // aa_mode=taa (DIBR frames only, so there is no stereo shift): the scene's jitter.
        const auto push = RenderTaa::EncodePush(
            {st.current_taa_jitter.ndc_x, st.current_taa_jitter.ndc_y});
        return {push.shift, push.hud};
    }
    return {has_depth ? st.current_draw_shift : 0.0f, 0.0f};
}

void SetMainAspect(float aspect) {
    if (aspect > 0.1f && aspect < 10.0f) {
        S().main_aspect.store(aspect, std::memory_order_relaxed);
    }
}

float MainAspect() {
    return S().main_aspect.load(std::memory_order_relaxed);
}

bool DibrFrameActive() {
    return S().current_frame_dibr;
}

TaaJitter FrameTaaJitter() {
    return S().current_taa_jitter;
}

void NoteTaaResolve(u32 width, u32 height, bool resolved) {
    auto& st = S();
    if (width > 0 && height > 0) {
        st.taa_width = width;
        st.taa_height = height;
    }
    if (resolved) {
        st.taa_resolved_frame = true;
        st.taa_unresolved = 0;
    }
}

DrawCameraScope::DrawCameraScope(const CameraRecord* record) : previous(draw_camera_override) {
    if (record)
        draw_camera_override = record;
}
DrawCameraScope::~DrawCameraScope() {
    draw_camera_override = previous;
}
const CameraRecord* DrawCameraOverride() {
    return draw_camera_override; // DrawFrameRecordLocked returns *draw_camera_override when set
}
CameraRecord DrawFrameRecord() {
    // The thread's own draw camera needs no shared state (and no record_mutex round trip).
    if (draw_camera_override)
        return *draw_camera_override;
    auto& st = S();
    std::scoped_lock lk{st.record_mutex};
    return DrawFrameRecordLocked(st);
}

u64 FirstDrawRecord() {
    return S().frame_first_record;
}

void SetFrameRecord(u64 id) {
    auto& st = S();
    if (id != 0 && !FlipTagTest()) {
        st.frame_first_record = id;
        st.frame_drawn = true;
    }
}

bool IsCameraActive() {
    auto& st = S();
    const s64 last = st.last_tick_ns.load(std::memory_order_relaxed);
    return last != 0 && static_cast<float>(NowNs() - last) < st.settings.camera_stale_ms * 1e6f;
}

bool IsImmersive() {
    return S().immersive_now.load(std::memory_order_relaxed);
}

bool GameOwnsCamera() {
    auto& st = S();
    return st.game_owns.load(std::memory_order_relaxed) || !IsImmersive();
}

namespace {
std::atomic<bool> y_menu_active{false};
std::atomic<bool> menu_runtime_owned{false};
} // namespace

void SetYMenu(bool y_menu, bool menu_runtime) {
    y_menu_active.store(y_menu, std::memory_order_relaxed);
    menu_runtime_owned.store(menu_runtime, std::memory_order_relaxed);
}

bool YMenuActive() {
    return y_menu_active.load(std::memory_order_relaxed);
}

bool MenuOwnedByRuntime() {
    return menu_runtime_owned.load(std::memory_order_relaxed);
}

float CurrentVfov() {
    return S().current_vfov.load(std::memory_order_relaxed);
}

namespace {
std::atomic<bool> g_eye_taa_accumulating{false};
} // namespace

void SetEyeTaaAccumulating(bool on) {
    g_eye_taa_accumulating.store(on, std::memory_order_relaxed);
}

bool EyeTaaAccumulating() {
    return g_eye_taa_accumulating.load(std::memory_order_relaxed);
}

namespace {
std::atomic<bool> g_late_warp_available{false};
} // namespace

void SetLateWarpAvailable(bool on) {
    g_late_warp_available.store(on, std::memory_order_relaxed);
}

bool LateWarpAvailable() {
    return g_late_warp_available.load(std::memory_order_relaxed);
}

float GameFrameSeconds() {
    return S().frame_seconds.load(std::memory_order_relaxed);
}

float HeightEyeOffsetM() {
    auto& st = S();
    if (st.eye_offset_set.load(std::memory_order_acquire)) {
        return st.eye_offset_now.load(std::memory_order_relaxed);
    }
    return PlayerBody::AdjustM(st.settings.height_adjust_cm); // As the hunter's until a tick
}

float HeightBodyOffsetM() {
    auto& st = S();
    if (st.eye_offset_set.load(std::memory_order_acquire)) {
        return st.body_offset_now.load(std::memory_order_relaxed);
    }
    return PlayerBody::AdjustM(st.settings.height_adjust_cm); // As the hunter's until a tick
}

float EffectiveIpd(const HeadState& head) {
    const auto& s = S().settings;
    if (s.ipd_mm > 0.0f) {
        return s.ipd_mm * 0.001f;
    }
    return head.ipd * s.ipd_scale;
}

namespace {
// The vr.ini writer (core/vr/vr_ini.h): one worker thread, edits debounced 0.5 s, written through
// <vr.ini>.tmp + rename under <vr.ini>.lock. Never destroyed (it may outlive the logger at exit);
// the VR page flushes it when it closes.
Ini::Writer& SettingsWriter() {
    static Ini::Writer* writer = new Ini::Writer(
        [] { return SettingsPath(); },
        [](const std::string& line) { LOG_INFO(Core, "{}", line); });
    return *writer;
}
} // Anonymous namespace

void SaveSetting(const std::string& key, const std::string& value) {
    SettingsWriter().Set(key, value);
}

void RemoveSetting(const std::string& key) {
    SettingsWriter().Remove(key);
}

void FlushSettings() {
    SettingsWriter().Flush();
}

bool SaveGraphicsForRestart() {
    auto& writer = SettingsWriter();
    const auto record = Graphics::Accepted();
    const auto dropped = writer.GetStats().dropped;
    writer.Set("graphics_config", Graphics::Serialize(record));
    writer.Flush();
    return writer.WaitIdle(5000) && writer.GetStats().dropped == dropped &&
           Graphics::Accepted() == record;
}

bool ApplySetting(const std::string& key, const std::string& value) {
    try {
        ParseSettingsLine(S().settings, std::string{Ini::Trim(key)}, std::string{Ini::Trim(value)});
        return true;
    } catch (const std::exception& e) {
        LOG_WARNING(Core, "VR: setting {}={} refused ({})", key, value, e.what());
        return false;
    }
}

std::string SettingsFilePath() {
    // UTF-8 (path::string() throws for characters outside the ANSI code page).
    return Common::FS::PathToUTF8String(SettingsPath());
}

float AdjustIpd(float delta_mm) {
    auto& s = S().settings;
    if (delta_mm == 0.0f) {
        s.ipd_mm = 0.0f;
    } else {
        const float current = s.ipd_mm > 0.0f ? s.ipd_mm : GetHead().ipd * s.ipd_scale * 1000.0f;
        s.ipd_mm = std::clamp(std::round(current + delta_mm), 40.0f, 120.0f);
    }
    const float now_mm = EffectiveIpd(GetHead()) * 1000.0f;
    LOG_INFO(Core, "VR: eye distance {:.0f} mm{}", now_mm,
             s.ipd_mm > 0.0f ? "" : " (headset IPD)");
    SaveSetting("ipd_mm", fmt::format("{:.0f}", s.ipd_mm));
    return now_mm;
}

void RecenterPosition() {
    S().recenter.store(true);
}

u32 ShiftTrackingSpace(const Space::Shift& shift) {
    auto& st = S();
    std::scoped_lock lk{st.shift_mutex};
    const u32 serial = st.space_serial.load() + 1;
    st.shift_log.Add(serial, shift);
    st.space_serial.store(serial, std::memory_order_release);
    return serial;
}

bool SpaceShiftBetween(u32 from, u32 to, Space::Shift& shift) {
    auto& st = S();
    std::scoped_lock lk{st.shift_mutex};
    return st.shift_log.Between(from, to, shift);
}

bool SpaceShiftApplied(u32 serial, u64& first_record) {
    auto& st = S();
    const u32 applied = st.shift_applied_serial.load(std::memory_order_acquire);
    // Serials only grow (wrap-safe difference).
    if (static_cast<s32>(applied - serial) < 0) {
        return false;
    }
    first_record = st.shift_first_record.load(std::memory_order_relaxed);
    return true;
}

bool IsLockedOn() {
    // A cinematic keeps the state (no lost / gained tick on entry and return).
    return S().locked_on.load(std::memory_order_relaxed) && (IsImmersive() || CinematicActive());
}

namespace {
// The paced vblank (frame cap): the frame thread produces a tick every K refreshes, the present
// thread consumes them (a backlog is dropped, never a burst of vblanks).
struct VblankPacer {
    std::mutex mutex;
    std::condition_variable cv;
    u64 produced{0};
    u64 consumed{0};
    u32 refreshes{0}; // K (0 = not pacing)
    u32 count{0};     // refreshes since the last tick
};
VblankPacer& Pacer() {
    static VblankPacer pacer;
    return pacer;
}
} // Anonymous namespace

void PaceVblank(u32 refreshes_per_vblank, u32 refreshes_elapsed) {
    auto& p = Pacer();
    {
        std::scoped_lock lk{p.mutex};
        if (refreshes_per_vblank != p.refreshes) {
            p.refreshes = refreshes_per_vblank;
            p.count = 0;
        }
        if (p.refreshes == 0) {
            p.cv.notify_all(); // a waiting present thread goes back to its timer
            return;
        }
        // Counted in display refreshes, not calls: a refresh the headset loop missed (or a loop
        // the runtime throttles to every other refresh) still counts, so the vblank stays on the
        // same refreshes (the one for a missed refresh comes late, never the ones after it).
        p.count += std::min(refreshes_elapsed, 64u);
        if (p.count < p.refreshes) {
            return;
        }
        p.count %= p.refreshes;
        ++p.produced;
    }
    p.cv.notify_all();
}

bool WaitPacedVblank(s64 timeout_ns) {
    auto& p = Pacer();
    std::unique_lock lk{p.mutex};
    if (p.refreshes == 0) {
        return false;
    }
    const bool ticked = p.cv.wait_for(lk, std::chrono::nanoseconds(timeout_ns), [&p] {
        return p.produced != p.consumed || p.refreshes == 0;
    });
    if (!ticked || p.produced == p.consumed) {
        return false;
    }
    p.consumed = p.produced;
    return true;
}

u32 PacedVblankRefreshes() {
    auto& p = Pacer();
    std::scoped_lock lk{p.mutex};
    return p.refreshes;
}

void SetPadRumble(u8 small_motor, u8 large_motor) {
    S().rumble.store((static_cast<u32>(small_motor) << 8) | large_motor, std::memory_order_relaxed);
}

void GetPadRumble(float& small_motor, float& large_motor) {
    const u32 value = S().rumble.load(std::memory_order_relaxed);
    small_motor = static_cast<float>((value >> 8) & 0xFF) / 255.0f;
    large_motor = static_cast<float>(value & 0xFF) / 255.0f;
}

void SetFirstPersonDetected(bool detected) {
    S().first_person_detected.store(detected);
}

bool IsFirstPerson() {
    auto& st = S();
    const s32 mode = st.settings.first_person;
    return mode < 0 || mode != 0;
}

float FirstPersonAheadM(float view_pitch) {
    constexpr float Start = 25.0f * 0.0174532925f;
    constexpr float Full = 60.0f * 0.0174532925f;
    const Settings& settings = S().settings;
    const float always = Tune::Get("fp.ahead_m", settings.fp_ahead_m);
    const float down = Tune::Get("fp.ahead_down_m", settings.fp_ahead_down_m);
    const float t = std::clamp((view_pitch - Start) / (Full - Start), 0.0f, 1.0f);
    const float ahead = always + down * t * t * (3.0f - 2.0f * t);
    return std::isfinite(ahead) ? std::clamp(ahead, 0.0f, 0.4f) : 0.0f;
}

const char* InjectResultName(InjectResult result) {
    switch (result) {
    case InjectResult::Pending:
        return "pending";
    case InjectResult::Healed:
        return "healed";
    case InjectResult::DryRun:
        return "dry run";
    case InjectResult::FullHp:
        return "denied (full HP)";
    case InjectResult::NoVials:
        return "denied (no Blood Vials)";
    case InjectResult::Dead:
        return "denied (dead)";
    case InjectResult::Blocked:
        return "denied (item use blocked)";
    case InjectResult::NoPlayer:
        return "denied (no player)";
    case InjectResult::Expired:
        return "expired (the game did not take it)";
    case InjectResult::Busy:
        return "busy";
    case InjectResult::Disabled:
        return "denied (instant injection disabled)";
    }
    return "unknown";
}

bool RequestInject() {
    auto& st = S();
    u64 current = st.inject_request.load(std::memory_order_acquire);
    if ((current & InjectPhaseMask) != InjectIdle) {
        return false;
    }
    const u32 sequence = st.inject_sequence.fetch_add(1, std::memory_order_relaxed) % 0xFFFF + 1;
    const u64 word = (static_cast<u64>(sequence) << 48) | (InjectNowMs() << 2) | InjectPending;
    if (!st.inject_request.compare_exchange_strong(current, word, std::memory_order_acq_rel)) {
        return false;
    }
    st.inject_awaited.store(word, std::memory_order_release);
    return true;
}

void CancelPendingInject() {
    auto& st = S();
    u64 awaited = st.inject_awaited.load(std::memory_order_acquire);
    if (awaited == 0 || (awaited & InjectPhaseMask) != InjectPending) return;
    // This CAS is the cancellation/execution ownership boundary. A taken request
    // cannot be withdrawn, and remains outstanding until its definitive answer.
    if (st.inject_request.compare_exchange_strong(awaited, InjectIdle, std::memory_order_acq_rel)) {
        st.inject_result.store((static_cast<u32>(awaited >> 48) << 8) |
                               static_cast<u32>(InjectResult::Expired), std::memory_order_release);
    }
}

InjectResult PollInjectResult() {
    auto& st = S();
    u64 awaited = st.inject_awaited.load(std::memory_order_acquire);
    if (awaited == 0) {
        return InjectResult::Pending;
    }
    const u32 result = st.inject_result.load(std::memory_order_acquire);
    if ((result >> 8) == (awaited >> 48) && (result & 0xFF) != 0) {
        st.inject_awaited.store(0, std::memory_order_relaxed);
        st.inject_dead_expiries = 0;
        return static_cast<InjectResult>(result & 0xFF);
    }
    if (InjectAgeMs(awaited) <= InjectTimeoutMs) {
        return InjectResult::Pending;
    }
    // Not taken in time: withdraw it. If the game thread took it meanwhile the exchange
    // fails and its answer arrives with that update.
    if (!st.inject_request.compare_exchange_strong(awaited, InjectIdle,
                                                   std::memory_order_acq_rel)) {
        return InjectResult::Pending;
    }
    st.inject_awaited.store(0, std::memory_order_relaxed);
    const s64 alive = st.player_alive_ns.load(std::memory_order_relaxed);
    if (IsImmersive() && NowNs() - alive > InjectHookDeadNs) {
        // Gameplay is running but the player update hook is not: the site or the player
        // pointer is wrong. After a few tries stabs are refused (never the game's animated vial).
        if (++st.inject_dead_expiries >= 3 && st.inject_available.exchange(false)) {
            LOG_WARNING(Core, "VR heal: DISABLED (no player update seen during gameplay); stabs "
                              "are now refused (inject=0: X is the game's vial button)");
        }
    } else {
        st.inject_dead_expiries = 0;
    }
    return InjectResult::Expired;
}

bool InstantInjectAvailable() {
    return S().inject_available.load(std::memory_order_relaxed);
}

void SetInstantInjectAvailable(bool available) {
    S().inject_available.store(available, std::memory_order_relaxed);
}

void NotePlayerUpdate() {
    S().player_alive_ns.store(NowNs(), std::memory_order_relaxed);
}

s64 LastPlayerUpdateNs() {
    return S().player_alive_ns.load(std::memory_order_relaxed);
}

bool TakeInjectRequest(bool& expired) {
    auto& st = S();
    u64 current = st.inject_request.load(std::memory_order_acquire);
    if ((current & InjectPhaseMask) != InjectPending) {
        return false;
    }
    const u64 taken = (current & ~InjectPhaseMask) | InjectTaken;
    if (!st.inject_request.compare_exchange_strong(current, taken, std::memory_order_acq_rel)) {
        return false; // withdrawn by the input thread
    }
    expired = InjectAgeMs(current) > InjectTimeoutMs;
    return true;
}

void NoteItemHeal() {
    S().item_heals.fetch_add(1, std::memory_order_relaxed);
}

u32 ItemHealCount() {
    return S().item_heals.load(std::memory_order_relaxed);
}

void NoteHitTaken(float fraction_of_max_hp) {
    auto& st = S();
    st.hit_fraction.store(std::clamp(fraction_of_max_hp, 0.0f, 1.0f), std::memory_order_relaxed);
    st.hits_taken.fetch_add(1, std::memory_order_release);
}

u32 HitTakenCount() {
    return S().hits_taken.load(std::memory_order_acquire);
}

float LastHitFraction() {
    return S().hit_fraction.load(std::memory_order_relaxed);
}

void PublishHeldRequest(u64 word) {
    S().held_request.store(word, std::memory_order_release);
}

u64 LatestHeldRequest() {
    return S().held_request.load(std::memory_order_acquire);
}

void PublishVialDrain(u64 word) {
    S().vial_drain.store(word, std::memory_order_release);
}

u64 LatestVialDrain() {
    return S().vial_drain.load(std::memory_order_acquire);
}

void PublishHeldStatus(const HeldStatus& status) {
    S().held_status.Store(status);
}

bool LatestHeldStatus(HeldStatus& out) {
    return S().held_status.Load(out);
}

void FinishInjectRequest(InjectResult result) {
    auto& st = S();
    const u64 current = st.inject_request.load(std::memory_order_acquire);
    if ((current & InjectPhaseMask) != InjectTaken) {
        return;
    }
    const u32 sequence = static_cast<u32>(current >> 48);
    st.inject_result.store((sequence << 8) | (static_cast<u32>(result) & 0xFF),
                           std::memory_order_release);
    st.inject_request.store(InjectIdle, std::memory_order_release);
}

namespace {

s64 MsToNs(float ms) {
    return static_cast<s64>(std::max(ms, 0.0f) * 1e6f);
}

Cinematic::Config CinematicConfig(const Settings& s) {
    Cinematic::Config cfg{};
    cfg.enabled = s.cinematic;
    cfg.vr_grab_attacks = s.vr_grab_attacks;
    // The interactive visceral (visceral_pull, its hold installed) plays the hunter's throws in
    // first person.
    cfg.categories = {false,
                      s.cinematic_visceral &&
                          !S().visceral_first_person.load(std::memory_order_relaxed),
                      s.cinematic_grabbed && !s.vr_grab_attacks,
                      s.cinematic_objects,
                      s.cinematic_lamps,
                      s.cinematic_pickup,
                      s.cinematic_scripted,
                      s.cinematic_ladder,
                      s.cinematic_item,
                      s.cinematic_levelup};
    cfg.fade_ns = MsToNs(s.cinematic_fade_ms);
    cfg.return_fade_ns =
        s.cinematic_return_fade_ms < 0.0f ? -1 : MsToNs(s.cinematic_return_fade_ms);
    cfg.exit_ns = MsToNs(s.cinematic_exit_ms);
    cfg.min_ns = MsToNs(s.cinematic_min_ms);
    cfg.release = s.cinematic_release;
    cfg.death_hold = s.cinematic_death_hold;
    cfg.throw_timeout_ns = MsToNs(s.cinematic_throw_timeout_s * 1000.0f);
    cfg.ladder.combat_exit = s.cinematic_ladder_combat_exit;
    cfg.ladder.damage_pct = s.cinematic_ladder_damage_pct;
    cfg.ladder.enter_delay_ns = MsToNs(s.cinematic_ladder_enter_delay_ms);
    cfg.ladder.min_flat_ns = MsToNs(s.cinematic_ladder_min_flat_ms);
    cfg.ladder.exit_lead_ns = MsToNs(s.cinematic_ladder_exit_lead_ms);
    cfg.ladder.reenter_ns = MsToNs(s.cinematic_ladder_reenter_ms);
    cfg.ladder.fade_ns = MsToNs(s.cinematic_ladder_fade_ms);
    return cfg;
}

// The published phase word: start time in ms << 20 | the phase's fade in ms << 8 | phase.
constexpr u64 CinematicFadeMask = 0xFFF;

// The published phase; Off once the main player's updates have stopped for good (loading, a hook
// that no longer runs), so a stale phase never holds the camera or the fade. A shorter gap (a
// shader-compile hitch) keeps it, as the machine does: the controls and the screen do not switch
// during a hitch in a flat period.
Cinematic::Phase LoadCinematic(s64 now, s64& start_ns, s64* fade_ns = nullptr) {
    auto& st = S();
    const u64 word = st.cinematic_word.load(std::memory_order_acquire);
    start_ns = static_cast<s64>(word >> 20) * Cinematic::Millis;
    if (fade_ns != nullptr) {
        *fade_ns = static_cast<s64>((word >> 8) & CinematicFadeMask) * Cinematic::Millis;
    }
    const auto phase = static_cast<Cinematic::Phase>(word & 0xFF);
    if (phase == Cinematic::Phase::Off ||
        now - st.player_alive_ns.load(std::memory_order_relaxed) > Cinematic::LongGapNs) {
        return Cinematic::Phase::Off;
    }
    return phase;
}

// Debug (VR_CINE_TEST=1): a synthetic 3 s scripted event every 10 s (mock runtime tests; needs
// the player update hook, i.e. in game).
bool CinematicTest(s64 now, Cinematic::Signals& signals) {
    static const bool enabled = std::getenv("VR_CINE_TEST") != nullptr;
    if (!enabled) {
        return false;
    }
    static const s64 epoch = now;
    constexpr s64 Period = 10'000 * Cinematic::Millis;
    if ((now - epoch) % Period < 7'000 * Cinematic::Millis) {
        return false;
    }
    if (!(signals.event_valid && signals.event)) {
        signals.event_valid = true;
        signals.event = true;
        signals.ids_valid = true;
        signals.event_id = 103000; // a scripted event animation
    }
    return true;
}

} // Anonymous namespace

Cinematic::Step UpdateCinematic(const Cinematic::Signals& signals) {
    auto& st = S();
    const s64 now = NowNs();
    Cinematic::Signals in = signals;
    CinematicTest(now, in);
    const bool allowed = IsEnabled() && GetDisplayMode() == DisplayMode::Auto;
    // A flat period only starts from first-person VR that is being shown. With the virtual
    // screen already up (no head pose, no camera bridge, a camera the headset does not drive) it
    // would only dim the screen and black it out on the way back.
    const bool can_enter = IsImmersive();
    const Cinematic::Step step =
        st.cinematic.Update(in, now, CinematicConfig(st.settings), allowed,
                            st.cinematic_suppress.exchange(false, std::memory_order_acq_rel),
                            can_enter, st.cinematic_projection_ns.load(std::memory_order_acquire));
    if (step.changed) {
        if (step.resync) {
            st.resync_to_head.store(true);
        } else if (step.to == Cinematic::Phase::Off) {
            st.resync_to_head.store(false); // no camera takeover consumed it
        }
        const u64 start_ms = static_cast<u64>(st.cinematic.PhaseStart() / Cinematic::Millis);
        const u64 fade_ms = std::min(
            static_cast<u64>(st.cinematic.PhaseFade() / Cinematic::Millis), CinematicFadeMask);
        st.cinematic_word.store((start_ms << 20) | (fade_ms << 8) | static_cast<u64>(step.to),
                                std::memory_order_release);
    }
    return step;
}

bool CinematicFlat() {
    s64 start{};
    const auto phase = LoadCinematic(NowNs(), start);
    return phase == Cinematic::Phase::Flat || phase == Cinematic::Phase::FadeBack;
}

bool CinematicActive() {
    s64 start{};
    return LoadCinematic(NowNs(), start) != Cinematic::Phase::Off;
}

float CinematicBrightness(bool content_immersive, s64 layer_switch_ns) {
    auto& st = S();
    if (!IsEnabled() || GetDisplayMode() == DisplayMode::Screen) {
        st.cinematic_fader.Reset();
        return 1.0f; // the screen chosen by hand is never dimmed
    }
    const s64 now = NowNs();
    s64 start{};
    s64 fade{};
    const auto phase = LoadCinematic(now, start, &fade);
    // Returning: first-person VR can only come back while the camera is updated and the head is
    // tracked; otherwise the screen is brought back up instead of staying black.
    const bool projection_expected =
        phase != Cinematic::Phase::Returning || (IsCameraActive() && GetHead().valid);
    // Each phase with its own fade (a ladder period has a shorter one).
    return st.cinematic_fader.Update(phase, start, now, fade, content_immersive,
                                     now - layer_switch_ns, projection_expected);
}

Cinematic::Phase CinematicPhase(s64* age_ms) {
    const s64 now = NowNs();
    s64 start{};
    const auto phase = LoadCinematic(now, start);
    if (age_ms != nullptr) {
        *age_ms = (now - start) / Cinematic::Millis;
    }
    return phase;
}

void NoteCinematicLayer(bool immersive, s64 switch_ns) {
    S().cinematic_projection_ns.store(immersive ? switch_ns : -1, std::memory_order_release);
}

Quat QuatFromYawPitch(float yaw, float pitch) {
    // q = yaw about +Y, then pitch about +X (local).
    const float cy = std::cos(yaw * 0.5f), sy = std::sin(yaw * 0.5f);
    const float cp = std::cos(pitch * 0.5f), sp = std::sin(pitch * 0.5f);
    // q_yaw = (0, sy, 0, cy), q_pitch = (sp, 0, 0, cp); q = q_yaw * q_pitch
    return Quat{cy * sp, sy * cp, -sy * sp, cy * cp};
}

Quat QuatFromYawPitchRoll(float yaw, float pitch, float roll) {
    const Quat q = QuatFromYawPitch(yaw, pitch);
    if (roll == 0.0f) {
        return q; // exactly the level frame
    }
    // q * q_roll, q_roll = (0, 0, sr, cr) about +Z (the view looks down -Z): + turns +X toward +Y.
    const float cr = std::cos(roll * 0.5f), sr = std::sin(roll * 0.5f);
    return Quat{q.x * cr + q.y * sr, q.y * cr - q.x * sr, q.z * cr + q.w * sr, q.w * cr - q.z * sr};
}

Vec3 Rotate(const Quat& q, const Vec3& v) {
    // v' = q * v * q^-1
    const Vec3 u{q.x, q.y, q.z};
    const float s = q.w;
    const float dot_uv = u.x * v.x + u.y * v.y + u.z * v.z;
    const float dot_uu = u.x * u.x + u.y * u.y + u.z * u.z;
    const Vec3 cross{u.y * v.z - u.z * v.y, u.z * v.x - u.x * v.z, u.x * v.y - u.y * v.x};
    return Vec3{2.0f * dot_uv * u.x + (s * s - dot_uu) * v.x + 2.0f * s * cross.x,
                2.0f * dot_uv * u.y + (s * s - dot_uu) * v.y + 2.0f * s * cross.y,
                2.0f * dot_uv * u.z + (s * s - dot_uu) * v.z + 2.0f * s * cross.z};
}

void YawPitchFromQuat(const Quat& q, float& yaw, float& pitch) {
    const Vec3 f = Rotate(q, Vec3{0.0f, 0.0f, -1.0f});
    yaw = std::atan2(-f.x, -f.z);
    pitch = std::asin(std::clamp(f.y, -1.0f, 1.0f));
}

float RollFromQuat(const Quat& q, float yaw, float pitch) {
    // The head's right axis in the level frame of the same view direction.
    const Quat level = QuatFromYawPitch(yaw, pitch);
    const Vec3 right = Rotate(q, Vec3{1.0f, 0.0f, 0.0f});
    const Vec3 level_right = Rotate(level, Vec3{1.0f, 0.0f, 0.0f});
    const Vec3 level_up = Rotate(level, Vec3{0.0f, 1.0f, 0.0f});
    const float x = right.x * level_right.x + right.y * level_right.y + right.z * level_right.z;
    const float y = right.x * level_up.x + right.y * level_up.y + right.z * level_up.z;
    return std::atan2(y, x);
}

} // namespace VR
