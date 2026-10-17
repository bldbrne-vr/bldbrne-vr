// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#include "common/logging/log.h"
#include "core/vr/bloodborne_layout.h"
#include "core/vr/bloodborne_motion.h"
#include "core/vr/body_channel.h"
#include "core/vr/hook_util.h"
#include "core/vr/motion_camera.h"
#include "core/vr/interaction_native_camera.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

namespace M = VR::Motion;
// Player update and status reads serialize one context under the publication mutex.
// Render owns a separate context. Native reads and publication stay here.
class MotionRuntime {
public:
    VR::Interactions::NativeCameraRuntime& Get() {
        if(!attempted_) {
            attempted_ = true;
            if (VR::Interactions::DllRequested()) {
                module_ = new VR::Interactions::Module();
                module_->EnableFaultBroadcast();
                runtime_.Bind(*module_, fault_);
                if (!module_->LoadBesideExecutable()) {
                    fault_ = true;
                    module_->ReportFault();
                    LOG_ERROR(Core, "VR motion: interaction DLL unavailable: {}",
                              module_->Diagnostic());
                }
            }
        }
        return runtime_;
    }

private:
    VR::Interactions::Module* module_{};
    VR::Interactions::NativeCameraRuntime runtime_;
    bool attempted_{}, fault_{};
};
using Hook::Plausible;
using Hook::Read;
using namespace Layout;

// Engine code behind each signal (module-relative addresses in the 1.09 eboot). Not patched,
// only compared: a mismatch turns that signal off. -1 = any byte.
// HKS env 56 ("has SpEffect"): list [rbx+0x1C8], first node [list+0x08], id +0x40, next +0x58.
constexpr u64 SpWalkRva = 0x1A142F1;
constexpr std::array<s16, 47> SpWalkSig = {
    0x48, 0x8B, 0x8B, 0xC8, 0x01, 0x00, 0x00, // mov rcx, [rbx+0x1C8]
    0x48, 0x85, 0xC9, 0x74, 0x2A,             // test rcx, rcx; jz
    0x48, 0x83, 0xC1, 0x08,                   // add rcx, 8
    0x48, 0x8B, 0x3D, -1,   -1,   -1,   -1,   // mov rdi, [rip+disp]
    0xBE, 0x01, 0x00, 0x00, 0x00,             // mov esi, 1
    0x48, 0x8B, 0x09,                         // mov rcx, [rcx]
    0x48, 0x85, 0xC9,                         // test rcx, rcx
    0x0F, 0x84, -1,   -1,   -1,   -1,         // jz
    0x39, 0x41, 0x40,                         // cmp [rcx+0x40], eax
    0x48, 0x8D, 0x49, 0x58,                   // lea rcx, [rcx+0x58]
};
// HKS env 20 ("is landed"): u8 [[bag+0x68]+0x212].
constexpr u64 GroundedRva = 0x1A13BCC;
constexpr std::array<u8, 18> GroundedSig = {
    0x49, 0x8B, 0x80, 0xB0, 0x03, 0x00, 0x00, // mov rax, [r8+0x3B0]
    0x48, 0x8B, 0x40, 0x68,                   // mov rax, [rax+0x68]
    0x0F, 0xB6, 0xB0, 0x12, 0x02, 0x00, 0x00, // movzx esi, byte [rax+0x212]
};
// The follow camera's pivot comes from the same physics module ([[PlayerIns+0x3B0]+0x68]+0x1E0);
// logged, ties the landed byte to the eye position.
constexpr u64 PivotSrcRva = 0x143B920;
constexpr std::array<u8, 11> PivotSrcSig = {
    0x49, 0x8B, 0x87, 0xB0, 0x03, 0x00, 0x00, // mov rax, [r15+0x3B0]
    0x48, 0x8B, 0x58, 0x68,                   // mov rbx, [rax+0x68]
};
constexpr u64 PivotPosRva = 0x143B974;
constexpr std::array<u8, 8> PivotPosSig = {
    0xC5, 0xF8, 0x28, 0x8B, 0xE0, 0x01, 0x00, 0x00, // vmovaps xmm1, [rbx+0x1E0]
};
// TAE event dispatch (jump table 0x1A246F0): rdx = bag, rbx = the action flags [bag+0x08], rax =
// the HKS action state [[PlayerIns+0x58]+0x38]. Logging only, like the three events below.
constexpr u64 TaeDispatchRva = 0x1A2174D;
constexpr std::array<u8, 23> TaeDispatchSig = {
    0x49, 0x8B, 0x4E, 0x10,                   // mov rcx, [r14+0x10]
    0x48, 0x8B, 0x91, 0xB0, 0x03, 0x00, 0x00, // mov rdx, [rcx+0x3B0]
    0x48, 0x8B, 0x5A, 0x08,                   // mov rbx, [rdx+0x08]
    0x48, 0x8B, 0x41, 0x58,                   // mov rax, [rcx+0x58]
    0x48, 0x8B, 0x40, 0x38,                   // mov rax, [rax+0x38]
};
// TAE event 8 (invincible): or [af+0x1BC], 2 (cleared every frame at 0x1A0DF93).
constexpr u64 IframeRva = 0x1A22B79;
constexpr std::array<u8, 7> IframeSig = {0x80, 0x8B, 0xBC, 0x01, 0x00, 0x00, 0x02};
// TAE event 27 (ignore gravity) / 51 (no fall animation): or byte [X+0x3C], 0x10 / 0x20.
constexpr u64 Event27Rva = 0x1A22CAC;
constexpr std::array<u8, 4> Event27Sig = {0x80, 0x48, 0x3C, 0x10};
constexpr u64 Event51Rva = 0x1A22D8D;
constexpr std::array<u8, 4> Event51Sig = {0x80, 0x48, 0x3C, 0x20};

constexpr u32 OffsetSpEffects = 0x1C8; // PlayerIns -> SpEffect list
constexpr u32 OffsetEffectHead = 0x08;
constexpr u32 OffsetEffectId = 0x40;
constexpr u32 OffsetEffectNext = 0x58;
constexpr u32 MaxEffects = 256;
constexpr s32 EffectDodge = 392;    // quickstep, roll, wake-up roll
constexpr s32 EffectBackstep = 393; // backsteps
// The sprint leap (core/vr/left_locomotion.h): the dash clips' TAE keeps 391 on (0.1 s); when it
// lapses it becomes 390 (0.2 s, replaceSpEffectId), the window in which the HKS's roll request
// is a jump instead (c0000.hks: Arm_Rolling and env GetSpEffectID 390 -> W_Jump).
constexpr s32 EffectJumpWindow = 390;
constexpr s32 EffectDash = 391;
std::atomic<u32> g_dash_bits{0}; // bits: 390, 391, read, 392 or 393
std::atomic<s64> g_dash_ns{0};
// The jump clip (7800) among the pushed TimeAct entries (DashEffects): the last update that read
// them, the last one with the jump, how many had it.
std::atomic<s64> g_jump_read_ns{0};
std::atomic<s64> g_jump_ns{0};
std::atomic<u32> g_jump_updates{0};
constexpr u32 OffsetChrType = 0x78; // PlayerIns (s32, logging)
constexpr u32 OffsetPhysicsModule = 0x68;
constexpr u32 OffsetPhysicsPosition = 0x1E0;
constexpr u32 OffsetLanded = 0x212;
constexpr u32 OffsetInvincible = 0x1BC; // action flags (u32), bit 1
constexpr u32 OffsetMoveFlags = 0x3C;   // HKS action state (u8)
constexpr u8 FlagNoGravity = 0x10;
constexpr u8 FlagNoFallAnim = 0x20;

s64 NowNs() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// VR_DODGE_LOG: 1 = dodge, mode and reset edges with per-dodge statistics; 2 = also a CSV line per
// camera tick around each dodge.
int DodgeLog() {
    static const int level = [] {
        const char* value = std::getenv("VR_DODGE_LOG");
        return value != nullptr ? std::max(std::atoi(value), 1) : 0;
    }();
    return level;
}

// ---- Main-player update (site A: one update at a time, usually on a job worker thread) --------
u8* g_base = nullptr;
u64 g_size = 0;
bool g_sp = false;       // SpEffect 392/393
bool g_ring = false;     // TimeAct motion clips
bool g_grounded = false; // the landed byte
bool g_ladder = false;   // the ladder state byte
bool g_iframe = false;   // logging
bool g_flags = false;    // logging
bool g_first = false;    // the first update was logged
u8* g_phys_vtable = nullptr;
std::atomic<float> g_jump_lift{0.f};
std::atomic<s64> g_jump_lift_ns{0};
s8 g_last_ladder = -1;
bool g_last_sp = false;

// The hand-off to the loop thread.
std::mutex g_signals_mutex;
M::Signals g_signals{};
// Input needs ladder detection while the cinematic screen bypasses the comfort camera.
// Updated at the player hook; published with g_signals under g_signals_mutex.
MotionRuntime g_input_motion_runtime;

M::Signals LoadSignals() {
    std::scoped_lock lock{g_signals_mutex};
    return g_signals;
}

// "+0x5335190" for an address in the eboot, else the pointer.
std::string Rva(const u8* pointer) {
    if (pointer >= g_base && pointer < g_base + g_size) {
        return fmt::format("+{:#x}", static_cast<u64>(pointer - g_base));
    }
    return fmt::format("{}", fmt::ptr(pointer));
}

std::string Vtable(const u8* object) {
    return object != nullptr ? Rva(Read<u8*>(object)) : std::string{"none"};
}

std::string Pos(const VR::Vec3& v) {
    return fmt::format("({:.3f},{:.3f},{:.3f})", v.x, v.y, v.z);
}

std::string Pos(const M::V3& v) {
    return fmt::format("({:.3f},{:.3f},{:.3f})", v.x, v.y, v.z);
}

// The action state (ladder byte, move flags): vtable and owner. A mismatch turns both off.
bool CheckActionState(const u8* state, const u8* ctrl) {
    if (Read<u8*>(state) == g_base + ActionStateVtableRva &&
        Read<u8*>(state + OffsetOwner) == ctrl) {
        return true;
    }
    LOG_WARNING(Core,
                "VR dodge: WARNING HKS action state {} vtable {} (expected +{:#x}) owner {} "
                "(ChrCtrl {}); ladder mode and move flags off",
                fmt::ptr(state), Vtable(state), ActionStateVtableRva,
                fmt::ptr(Read<u8*>(state + OffsetOwner)), fmt::ptr(ctrl));
    g_ladder = g_flags = false;
    return false;
}

// ---- Render-camera hook (loop thread) -----------------------------------------------------------
struct CsvRow {
    s64 t;
    s32 eye;
    bool pair_first;
    M::Mode mode;
    M::Kind kind;
    s32 clip;
    s64 clip_age_ns;
    s32 sp;
    bool grounded;
    M::V3 root;
    M::V3 cam;
    float vy;
    float t_eff;
    float speed;
};

struct CameraSide {
    MotionRuntime runtime;
    M::DodgeStats stats{};
    M::Signals signals{}; // the last tick's copy
    // The dodge being measured.
    M::ClipInfo dodge{};
    s32 dodge_clip{-1};
    const char* end_reason{""};
    s64 dodge_len_ns{0};
    // Logs.
    bool first_logged{false};
    M::Mode gate_mode{M::Mode::Ground}; // the old camera's mode lines
    M::V3 first_eye{};                  // the last first eye's base (pair-lock check)
    s64 epoch_ns{0};
    std::array<CsvRow, 30> preroll{};
    size_t preroll_count{0};
    size_t preroll_next{0};
    s32 csv_left{-1}; // ticks still written after the end (-1 = not writing; large while dodging)
    bool csv_header{false};
    bool ladder_dead{false}; // the gate's ladder dead latch (edges logged)
    u64 rejected{0};         // non-finite camera targets
};

CameraSide& Cam() {
    static CameraSide side;
    return side;
}

M::V3 ToMotion(const VR::Vec3& v) {
    return {v.x, v.y, v.z};
}

VR::Vec3 ToVr(const M::V3& v) {
    return {v.x, v.y, v.z};
}

M::ComfortConfig CurrentConfig() {
    const auto& s = VR::GetSettings();
    M::ComfortConfig cfg{};
    cfg.scale = s.world_scale;
    cfg.follow_s = s.follow_smoothing_ms * 0.001f;
    cfg.ground_s = s.vertical_smoothing_ms * 0.001f;
    cfg.dodge_s = s.dodge_vertical_ms * 0.001f;
    cfg.fall_s = s.fall_vertical_ms * 0.001f;
    cfg.stop_factor = s.vertical_stop_factor;
    cfg.dip_m = s.dodge_dip_m;
    cfg.jump_height_scale = s.jump_height_scale;
    cfg.pair_lock = s.pair_lock_position;
    cfg.pair_max_ns = static_cast<s64>(1.5 * static_cast<double>(VR::GameFrameSeconds()) * 1e9);
    return cfg;
}

void WriteCsv(const CsvRow& row) {
    auto& c = Cam();
    if (!c.csv_header) {
        c.csv_header = true;
        LOG_INFO(Core, "VR dodgecsv,t_ms,eye,pf,mode,kind,clip,clip_age_ms,sp,gr,root_x,root_y,"
                       "root_z,cam_x,cam_y,cam_z,vy,T_eff,spd");
    }
    LOG_INFO(Core,
             "VR dodgecsv,{:.1f},{},{},{},{},{},{},{},{},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},{:.4f},"
             "{:.3f},{:.0f},{:.2f}",
             static_cast<double>(row.t - c.epoch_ns) * 1e-6, row.eye, row.pair_first ? 1 : 0,
             M::ModeName(row.mode), M::KindName(row.kind), row.clip,
             row.clip_age_ns < 0 ? -1 : row.clip_age_ns / M::Millis, row.sp, row.grounded ? 1 : 0,
             row.root.x, row.root.y, row.root.z, row.cam.x, row.cam.y, row.cam.z, row.vy,
             row.t_eff * 1000.0f, row.speed);
}

void FinishDodge(CameraSide& c, const char* cut) {
    const float scale = VR::GetSettings().world_scale;
    const u32 pairs = c.stats.pairs;
    const u32 mismatches = c.stats.mismatches;
    const u32 resets = c.stats.resets;
    const M::DodgeReport r = c.stats.Finish(scale);
    if (!r.valid) {
        LOG_INFO(Core, "VR dodge: END kind={} clip={} ({}) | no camera ticks measured",
                 M::KindName(c.dodge.kind), c.dodge_clip, cut != nullptr ? cut : c.end_reason);
        return;
    }
    const auto ms = [](float value) {
        return value < 0.0f ? std::string{"never"} : fmt::format("{:.0f}", value);
    };
    LOG_INFO(Core,
             "VR dodge: END kind={} dir={} clip={} after {} ms ({}) | root dist={:.2f} cam "
             "dist={:.2f} (x{:.3f}) dir err={:.1f}deg | H lag max={:.3f} over={:.3f} settle={} ms "
             "| V root range={:.3f} cam range={:.3f} cam vmax={:.2f} amax(pairs)={:.0f} tail={} ms "
             "| pairlock mismatches={} resets={} pairs={} ticks={}{}",
             M::KindName(c.dodge.kind), M::DirName(c.dodge.dir), c.dodge_clip,
             c.dodge_len_ns / M::Millis, cut != nullptr ? cut : c.end_reason, r.root_dist,
             r.cam_dist, r.dist_ratio, r.dir_err_deg, r.h_lag_max, r.h_over, ms(r.h_settle_ms),
             r.v_root_range, r.v_cam_range, r.v_cam_vmax, r.v_cam_amax, ms(r.v_tail_ms), mismatches,
             resets, pairs, r.ticks,
             VR::GetSettings().dodge_camera ? "" : " (dodge_camera=0: old camera)");
}

// One driven first-person tick after the camera placed the eye base `base`: the one-time
// consistency line and, with VR_DODGE_LOG, the dodge statistics and lines.
void Observe(const EyeTick& tick, const M::Phase& phase, const M::V3& base,
             const M::ComfortOut* out) {
    auto& c = Cam();
    const s64 now = tick.now_ns;
    if (!c.first_logged) {
        c.first_logged = true;
        c.epoch_ns = now;
        const auto& s = c.signals;
        const VR::Vec3 d{tick.pivot.x - s.phys.x - tick.org.x, tick.pivot.y - s.phys.y - tick.org.y,
                         tick.pivot.z - s.phys.z - tick.org.z};
        LOG_INFO(
            Core,
            "VR dodge: first driven tick pivot={} org={} phys={} |pivot-phys-org|={} target={} "
            "camera={}",
            Pos(tick.pivot), Pos(tick.org), s.phys_valid ? Pos(s.phys) : std::string{"?"},
            s.phys_valid ? fmt::format("{:.1f} mm", std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) *
                                                        1000.0f / VR::GetSettings().world_scale)
                         : std::string{"?"},
            Pos(tick.target), out != nullptr ? "comfort" : "old (dodge_camera=0)");
    }
    const int log = DodgeLog();
    if (log == 0) {
        return;
    }
    const M::V3 root = ToMotion(tick.target);

    // Mode lines: the comfort camera's own mode (Fall is held through the landing), else the
    // gate's.
    const M::Mode mode = out != nullptr ? out->mode : phase.mode;
    const M::Mode from = out != nullptr ? out->mode_from : c.gate_mode;
    if ((out != nullptr ? out->mode_changed : mode != c.gate_mode)) {
        std::string why;
        if (mode == M::Mode::Fall) {
            why = phase.fall_confirmed
                      ? fmt::format(" (ring {})", phase.clip)
                      : fmt::format(" (ungrounded {} ms)", phase.ungrounded_ns / M::Millis);
            if (out != nullptr) {
                why += fmt::format(" takeoff={:.3f}", out->takeoff);
            }
        } else if (from == M::Mode::Fall && out != nullptr) {
            why = fmt::format(" lag={:.3f} m", out->fall_lag_m);
        } else if (mode == M::Mode::Dodge) {
            why = fmt::format(" ({} {})", M::KindName(phase.dodge.kind), phase.dodge_clip);
        }
        LOG_INFO(Core, "VR dodge: MODE {}->{}{}", M::ModeName(from), M::ModeName(mode), why);
    }
    c.gate_mode = phase.mode;

    // Dodges: start, end (+300 ms of tail), statistics.
    if (phase.dodge_start) {
        if (c.stats.Active()) {
            FinishDodge(c, phase.chained ? "chained" : "restarted");
        }
        c.stats.Begin(now);
        c.dodge = phase.dodge;
        c.dodge_clip = phase.dodge_clip;
        c.end_reason = "";
        c.dodge_len_ns = 0;
        const auto& s = c.signals;
        LOG_INFO(Core,
                 "VR dodge: START kind={} dir={} clip={}{} sp={} grounded={} iframe={} f51={} "
                 "f27={} root={} cam={} mode={}",
                 M::KindName(phase.dodge.kind), M::DirName(phase.dodge.dir), phase.dodge_clip,
                 phase.chained ? " (chained)" : "", phase.sp_id,
                 s.grounded_valid ? (s.grounded ? "1" : "0") : "?",
                 s.iframe_valid ? (s.iframe ? "1" : "0") : "?",
                 s.flags_valid ? (s.no_fall_anim ? "1" : "0") : "?",
                 s.flags_valid ? (s.no_gravity ? "1" : "0") : "?", Pos(root), Pos(base),
                 M::ModeName(mode));
        if (log >= 2) {
            // The pre-roll, oldest first.
            const size_t start =
                (c.preroll_next + c.preroll.size() - c.preroll_count) % c.preroll.size();
            for (size_t i = 0; i < c.preroll_count; ++i) {
                WriteCsv(c.preroll[(start + i) % c.preroll.size()]);
            }
            c.preroll_count = 0;
            c.csv_left = 1 << 30;
        }
    }
    if (phase.dodge_end && c.stats.Active()) {
        c.stats.End(now);
        c.end_reason = phase.end_reason;
        c.dodge_len_ns = phase.dodge_len_ns;
        if (c.csv_left > 30) {
            c.csv_left = 30;
        }
    }
    if (c.stats.Active()) {
        if (tick.pair_first) {
            c.stats.Add(now, root, base);
        } else {
            ++c.stats.pairs;
            if (std::memcmp(&base, &c.first_eye, sizeof(base)) != 0) {
                ++c.stats.mismatches;
            }
        }
        if (out != nullptr && out->reset) {
            ++c.stats.resets;
        }
        if (c.stats.Due(now)) {
            FinishDodge(c, nullptr);
        }
    }
    if (tick.pair_first) {
        c.first_eye = base;
    }

    if (log >= 2) {
        const CsvRow row{now,
                         tick.eye,
                         tick.pair_first,
                         mode,
                         phase.dodging ? phase.dodge.kind : phase.sticky.kind,
                         phase.clip,
                         phase.clip_age_ns,
                         phase.sp_id,
                         phase.grounded,
                         root,
                         base,
                         out != nullptr ? out->vy : 0.0f,
                         out != nullptr ? out->t_eff : 0.0f,
                         out != nullptr ? out->speed : 0.0f};
        if (c.csv_left > 0) {
            WriteCsv(row);
            --c.csv_left;
        } else {
            c.preroll[c.preroll_next] = row;
            c.preroll_next = (c.preroll_next + 1) % c.preroll.size();
            c.preroll_count = std::min(c.preroll_count + 1, c.preroll.size());
        }
    }
}

M::Phase StepGate(CameraSide& c, s64 now) {
    c.signals = LoadSignals();
    const M::Phase phase = c.runtime.Get().Gate(c.signals, now);
    if (phase.ladder_dead != c.ladder_dead) {
        c.ladder_dead = phase.ladder_dead;
        LOG_INFO(Core, "VR dodge: ladder dead latch {} (ladder state {}, HP {})",
                 phase.ladder_dead ? "set: a ladder state left over from a death is ignored"
                                   : "released",
                 c.signals.ladder_valid ? static_cast<s32>(c.signals.ladder) : -2,
                 c.signals.hp_valid ? c.signals.hp : -1);
    }
    return phase;
}

} // Anonymous namespace

DashEffects LatestDashEffects() {
    DashEffects d{};
    d.steady_ns = g_dash_ns.load(std::memory_order_acquire);
    const u32 bits = g_dash_bits.load(std::memory_order_relaxed);
    d.valid = (bits & 4) != 0 && d.steady_ns != 0;
    d.window = (bits & 1) != 0;
    d.dash = (bits & 2) != 0;
    d.dodge = (bits & 8) != 0;
    d.jump_read_ns = g_jump_read_ns.load(std::memory_order_acquire);
    d.jump_ns = g_jump_ns.load(std::memory_order_relaxed);
    d.jump_updates = g_jump_updates.load(std::memory_order_relaxed);
    d.jump_valid = d.jump_read_ns != 0;
    return d;
}

AirSample LatestAir() {
    const M::Signals s = LoadSignals();
    AirSample a{};
    a.valid = s.valid && s.grounded_valid && s.phys_valid;
    a.grounded = s.grounded;
    a.y = s.phys.y;
    a.steady_ns = s.update_ns;
    return a;
}

bool LadderActive() {
    std::scoped_lock lock{g_signals_mutex};
    return g_input_motion_runtime.Get().MotionStatus(NowNs()).ladder;
}

s64 LatestLadderStartNs() {
    std::scoped_lock lock{g_signals_mutex};
    return g_input_motion_runtime.Get().MotionStatus(NowNs()).ladder_start_ns;
}

bool InitMotion(VAddr base, u64 size, bool site_ok, bool main_ok) {
    auto* const module = reinterpret_cast<u8*>(base);
    g_base = module;
    g_size = size;
    if (size < ActionStateVtableRva + sizeof(u64)) {
        LOG_INFO(Core, "VR dodge: eboot too small ({:#x}) for Bloodborne 1.09; no motion signals",
                 size);
        return false;
    }
    const auto matches = [module](u64 rva, const auto& signature) {
        return std::memcmp(module + rva, signature.data(), signature.size()) == 0;
    };
    const bool sp_ok = MatchMasked(module + SpWalkRva, SpWalkSig);
    const bool grounded_ok = matches(GroundedRva, GroundedSig);
    const bool pivot_ok = matches(PivotSrcRva, PivotSrcSig) && matches(PivotPosRva, PivotPosSig);
    const bool push_ok = matches(TimeActPushRva, TimeActPushSig);
    const bool act26_ok = matches(Act26Rva, Act26Sig);
    const bool dispatch_ok = matches(TaeDispatchRva, TaeDispatchSig);
    const bool iframe_ok = matches(IframeRva, IframeSig);
    const bool f27_ok = matches(Event27Rva, Event27Sig);
    const bool f51_ok = matches(Event51Rva, Event51Sig);
    const bool hook_ok = site_ok && main_ok;
    g_sp = hook_ok && sp_ok;
    g_ring = hook_ok && push_ok;
    g_grounded = hook_ok && grounded_ok;
    g_ladder = hook_ok && act26_ok;
    g_iframe = hook_ok && dispatch_ok && iframe_ok;
    g_flags = hook_ok && dispatch_ok && act26_ok && f27_ok && f51_ok;
    const auto& s = VR::GetSettings();
    const auto verdict = [](bool ok) { return ok ? "ok" : "MISMATCH"; };
    const auto on = [](bool ok) { return ok ? "on" : "off"; };
    LOG_INFO(Core,
             "VR dodge: siteA={} ismain={} spwalk@1A142F1={} grounded@1A13BCC={} "
             "pivotsrc@143B920={} taPush@1A28173={} act26@1A11A67={} taeDispatch@1A2174D={} "
             "iframe@1A22B79={} f27@1A22CAC={} f51@1A22D8D={} -> sp={} ring={} grounded={} "
             "ladder={} iframe={} flags={}; camera={} follow={:.0f} vert={:.0f}/{:.0f}/{:.0f} "
             "stop={:.2f} dip={:.3f} jump_scale={:.2f} pairlock={}{}",
             verdict(site_ok), verdict(main_ok), verdict(sp_ok), verdict(grounded_ok),
             verdict(pivot_ok), verdict(push_ok), verdict(act26_ok), verdict(dispatch_ok),
             verdict(iframe_ok), verdict(f27_ok), verdict(f51_ok), on(g_sp), on(g_ring),
             on(g_grounded), on(g_ladder), on(g_iframe), on(g_flags),
             s.dodge_camera ? "comfort" : "old (dodge_camera=0)", s.follow_smoothing_ms,
             s.vertical_smoothing_ms, s.dodge_vertical_ms, s.fall_vertical_ms,
             s.vertical_stop_factor, s.dodge_dip_m, s.jump_height_scale, s.pair_lock_position ? 1 : 0,
             DodgeLog() != 0 ? fmt::format(" log={}", DodgeLog()) : std::string{});
    const auto dump = [module](const char* name, u64 rva, size_t length) {
        LOG_WARNING(Core, "VR dodge: {} live bytes at {:#x}: {}", name, rva,
                    Hook::HexBytes(module + rva, length));
    };
    if (!sp_ok) {
        dump("spwalk", SpWalkRva, SpWalkSig.size());
    }
    if (!grounded_ok) {
        dump("grounded", GroundedRva, GroundedSig.size());
    }
    if (!pivot_ok) {
        dump("pivotsrc", PivotSrcRva, PivotSrcSig.size());
        dump("pivotpos", PivotPosRva, PivotPosSig.size());
    }
    if (!push_ok) {
        dump("taPush", TimeActPushRva, TimeActPushSig.size());
    }
    if (!act26_ok) {
        dump("act26", Act26Rva, Act26Sig.size());
    }
    if (!dispatch_ok) {
        dump("taeDispatch", TaeDispatchRva, TaeDispatchSig.size());
    }
    if (!iframe_ok) {
        dump("iframe", IframeRva, IframeSig.size());
    }
    if (!f27_ok) {
        dump("f27", Event27Rva, Event27Sig.size());
    }
    if (!f51_ok) {
        dump("f51", Event51Rva, Event51Sig.size());
    }
    const bool any = g_sp || g_ring || g_grounded || g_ladder;
    if (!any) {
        LOG_WARNING(Core, "VR dodge: no motion signal; the comfort camera uses its Ground time "
                          "constants everywhere");
    }
    return any;
}

void MotionUpdate(u8* player) {
    const s64 now = NowNs();
    M::Signals s{};
    s.valid = true;
    s.update_ns = now;
    u8* const bag = Read<u8*>(player + OffsetChrModules);
    const bool bag_ok = Plausible(bag);
    const auto module = [bag, bag_ok](u32 offset) -> u8* {
        if (!bag_ok) {
            return nullptr;
        }
        u8* const pointer = Read<u8*>(bag + offset);
        return Plausible(pointer) ? pointer : nullptr;
    };
    const bool first = !g_first && bag_ok;

    // SpEffect 392 / 393, walked like HKS env 56 (no flag skip).
    std::string effects;
    if (g_sp) {
        u8* const list = Read<u8*>(player + OffsetSpEffects);
        if (Plausible(list)) {
            s.sp_valid = true;
            u32 dash = 4;
            u8* node = Read<u8*>(list + OffsetEffectHead);
            for (u32 i = 0; i < MaxEffects && Plausible(node); ++i) {
                const s32 id = Read<s32>(node + OffsetEffectId);
                s.sp392 = s.sp392 || id == EffectDodge;
                s.sp393 = s.sp393 || id == EffectBackstep;
                dash |= id == EffectJumpWindow ? 1u : id == EffectDash ? 2u : 0u;
                dash |= id == EffectDodge || id == EffectBackstep ? 8u : 0u;
                if (first && i < 16) {
                    effects += fmt::format("{}{}", effects.empty() ? "" : ",", id);
                }
                node = Read<u8*>(node + OffsetEffectNext);
            }
            g_dash_bits.store(dash, std::memory_order_relaxed);
            g_dash_ns.store(now, std::memory_order_release);
        }
    }
    const bool sp = s.sp392 || s.sp393;

    // Action flags (logging): vtable on the first update and on every SpEffect rising edge.
    u8* const action_flags = module(OffsetActionFlagModule);
    if (g_iframe && action_flags != nullptr) {
        if ((first || (sp && !g_last_sp)) &&
            Read<u8*>(action_flags) != g_base + ActionFlagVtableRva) {
            g_iframe = false;
            LOG_WARNING(Core,
                        "VR dodge: WARNING action-flag module {} has vtable {} (expected +{:#x}); "
                        "invincibility log off",
                        fmt::ptr(action_flags), Vtable(action_flags), ActionFlagVtableRva);
        } else {
            s.iframe_valid = true;
            s.iframe = (Read<u32>(action_flags + OffsetInvincible) & 2) != 0;
        }
    }
    g_last_sp = sp;

    // Copy the native ring in its original order. DLL policy selects clips and
    // asks whether the original first/new-clip vtable proof is required.
    u8* const time_act = module(OffsetTimeActModule);
    PiNativeCameraInput ring_input{};
    ring_input.operation = PI_NC_MOTION_RING;
    ring_input.now_ns = now;
    static_assert(TimeActRingSize == std::size(ring_input.ring.ids));
    ring_input.ring.command = PI_MOTION_RING_BEGIN;
    ring_input.ring.first = first;
    if (g_ring && time_act != nullptr) {
        const s32 write = Read<s32>(time_act + OffsetTimeActWrite);
        const s32 read = Read<s32>(time_act + OffsetTimeActRead);
        if (write >= 0 && write < TimeActRingSize && read >= 0 && read < TimeActRingSize) {
            ring_input.ring.have_ring = 1;
            for (s32 i = read; i != write; i = (i + 1) % TimeActRingSize)
                ring_input.ring.ids[ring_input.ring.count++] =
                    Read<s32>(time_act + OffsetTimeActRing + i * TimeActEntrySize);
        }
    }
    const auto call_ring = [&](const PiNativeCameraInput& input) {
        std::scoped_lock lock{g_signals_mutex};
        return g_input_motion_runtime.Get().Call(input).ring;
    };
    const auto begin = call_ring(ring_input);
    if (!begin.token)
        return;
    bool accepted = ring_input.ring.have_ring;
    if (begin.check_vtable && Read<u8*>(time_act) != g_base + TimeActVtableRva) {
        g_ring = false;
        accepted = false;
        LOG_WARNING(
            Core,
            "VR dodge: WARNING TimeAct module {} has vtable {} (expected +{:#x}); motion clips off",
            fmt::ptr(time_act), Vtable(time_act), TimeActVtableRva);
    }
    ring_input.ring = {};
    ring_input.ring.command = PI_MOTION_RING_COMMIT;
    ring_input.ring.token = begin.token;
    ring_input.ring.accepted = accepted;
    const auto clips = call_ring(ring_input);
    if (!clips.token)
        return;
    s.ring_valid = clips.ring_valid;
    s.clip = clips.clip;
    s.clip_ns = clips.clip_ns;
    s.clip_serial = clips.clip_serial;
    if (clips.ring_valid) {
        g_jump_updates.store(clips.jump_updates, std::memory_order_relaxed);
        g_jump_ns.store(clips.jump_ns, std::memory_order_relaxed);
        g_jump_read_ns.store(clips.jump_read_ns, std::memory_order_release);
    }

    // The physics module: the landed byte and the position (the camera pivot's source). Its
    // vtable is recorded on the first update and must stay the same.
    u8* const physics = module(OffsetPhysicsModule);
    if (g_grounded && physics != nullptr) {
        u8* const vtable = Read<u8*>(physics);
        if (g_phys_vtable == nullptr) {
            g_phys_vtable = vtable;
        }
        if (vtable != g_phys_vtable) {
            g_grounded = false;
            LOG_WARNING(Core,
                        "VR dodge: WARNING physics module {} vtable {} changed (first {}); landed "
                        "byte off",
                        fmt::ptr(physics), Rva(vtable), Rva(g_phys_vtable));
        } else {
            s.grounded_valid = true;
            s.grounded = Read<u8>(physics + OffsetLanded) != 0;
            s.phys_valid = true;
            float position[3];
            std::memcpy(position, physics + OffsetPhysicsPosition, sizeof(position));
            s.phys = {position[0], position[1], position[2]};
        }
    }

    // The HKS action state: the ladder byte and the move flags. Vtable and owner on the first
    // update and whenever the hunter gets on a ladder.
    u8* action_state = nullptr;
    if (g_ladder || g_flags) {
        u8* const ctrl = Read<u8*>(player + OffsetChrCtrl);
        u8* const state = Plausible(ctrl) ? Read<u8*>(ctrl + OffsetActionState) : nullptr;
        if (Plausible(state)) {
            const s8 ladder = Read<s8>(state + OffsetLadderState);
            const bool mount = ladder != -1 && g_last_ladder == -1;
            if (!(first || mount) || CheckActionState(state, ctrl)) {
                action_state = state;
                if (g_ladder) {
                    s.ladder_valid = true;
                    s.ladder = ladder;
                    g_last_ladder = ladder;
                }
                if (g_flags) {
                    const u8 flags = Read<u8>(state + OffsetMoveFlags);
                    s.flags_valid = true;
                    s.no_gravity = (flags & FlagNoGravity) != 0;
                    s.no_fall_anim = (flags & FlagNoFallAnim) != 0;
                }
            }
        }
    }

    // HP: the Gate's ladder dead latch (as the body's animation gate and the cinematic ladder gate
    // read it).
    if (u8* const data = module(OffsetDataModule); data != nullptr) {
        s.hp_valid = true;
        s.hp = Read<s32>(data + OffsetHp);
    }

    if (first) {
        g_first = true;
        LOG_INFO(Core,
                 "VR dodge: first P={} chrType={} phys={} vt={}{} pos={} grounded={} ta vt={} X={} "
                 "vt={} ls={} af vt={} sp=[{}]",
                 fmt::ptr(player), Read<s32>(player + OffsetChrType), fmt::ptr(physics),
                 Vtable(physics),
                 physics != nullptr &&
                         (Read<u8*>(physics) < g_base || Read<u8*>(physics) >= g_base + g_size)
                     ? " (not in the eboot)"
                     : "",
                 s.phys_valid ? Pos(s.phys) : std::string{"?"},
                 s.grounded_valid ? (s.grounded ? "1" : "0") : "?", Vtable(time_act),
                 fmt::ptr(action_state), Vtable(action_state), static_cast<s32>(s.ladder),
                 Vtable(action_flags), effects);
    }

    std::scoped_lock lock{g_signals_mutex};
    g_signals = s;
    g_input_motion_runtime.Get().Gate(s, now);
}

void PublishJumpLift(float lift, s64 now) {
    g_jump_lift.store(lift, std::memory_order_relaxed);
    g_jump_lift_ns.store(now, std::memory_order_release);
}

VR::Vec3 ComfortEye(const EyeTick& tick, const char* reset) {
    auto& c = Cam();
    const M::Phase phase = StepGate(c, tick.now_ns);
    M::ComfortConfig cfg = CurrentConfig();
    const auto lift_ns = g_jump_lift_ns.load(std::memory_order_acquire);
    const float lift = g_jump_lift.load(std::memory_order_relaxed);
    // A second eye's pair lock as the VR body decided it at this frame's pose copy (it placed the
    // hands for that eye): the camera follows it (core/vr/body_frame.h PairLock).
    M::LockDecision lock{};
    if (VR::Body::PairLock decided{}; VR::Body::LatestPairLock(decided)) {
        lock = {true, decided.first_ns, decided.decided_ns, decided.locked};
    }
    // A non-finite target with no recent output to keep: the body's physics position from the
    // main player's last update plus the eye height. The target comes from the follow camera's
    // pivot (which the game derives from that position), so this is read apart from the value
    // that failed.
    const M::Signals& signals = c.signals;
    const M::ComfortOut out = c.runtime.Get().Comfort(
        ToMotion(tick.target), tick.pair_first, tick.now_ns, phase, cfg, reset, lock, nullptr,
        &signals, VR::GetSettings().fp_eye_height_m, lift_ns, lift, &cfg.jump_lift);
    if (out.rejected && c.rejected++ % 600 == 0) {
        const char* shown = out.fallback ? (out.stale ? "the body's last known physics position"
                                                      : "the body's physics position")
                            : out.stale  ? "its last known position (nothing recent)"
                                         : "its last position";
        LOG_WARNING(Core,
                    "VR dodge: non-finite camera target {} ({} so far); the camera shows {} {} and "
                    "starts over at the next finite one",
                    Pos(tick.target), c.rejected, shown, Pos(out.position));
    }
    if (out.reset) {
        const bool jump = std::strcmp(out.reset_reason, "teleport") == 0 ||
                          std::strcmp(out.reset_reason, "lag") == 0;
        // Teleports and lag resets are rare (lamps, respawns) and always logged.
        if (jump || DodgeLog() >= 1) {
            LOG_INFO(Core, "VR dodge: RESET reason={}{} target={} mode={}{}", out.reset_reason,
                     jump ? fmt::format("({:.2f}m)", out.reset_m) : std::string{}, Pos(tick.target),
                     M::ModeName(phase.mode), phase.dodging ? " (during a dodge)" : "");
        }
    }
    if (DodgeLog() >= 2 && tick.pair_first && (phase.dodging || out.dip != 0.0f)) {
        LOG_INFO(Core, "VR motion cue: kind={} clip={} offset_mm={:.2f} animated_lift_mm={:.2f} camera_y={:.4f}",
                 M::KindName(phase.dodge.kind),phase.dodge_clip,
                 out.dip / cfg.scale * 1000.f,cfg.jump_lift / cfg.scale * 1000.f,out.position.y);
    }
    Observe(tick, phase, out.position, &out);
    return ToVr(out.position);
}

void LegacyEye(const EyeTick& tick, const VR::Vec3& base) {
    if (DodgeLog() == 0) {
        return;
    }
    auto& c = Cam();
    const M::Phase phase = StepGate(c, tick.now_ns);
    Observe(tick, phase, ToMotion(base), nullptr);
}

} // namespace VR::Bloodborne
