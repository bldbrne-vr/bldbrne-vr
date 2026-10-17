// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "common/thread.h"
#include "core/vr/bloodborne_iframes.h"
#include "core/vr/bloodborne_layout.h"
#include "core/vr/hook_util.h"
#include "core/vr/iframe_extend.h"
#include "core/vr/interaction_native_timing.h"
#include "core/vr/motion_camera.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

namespace I = VR::Iframes;
namespace M = VR::Motion;
using Hook::Plausible;
using Hook::Read;
using namespace Layout;

// Module-relative addresses in the 1.09 eboot (patch file address - 0x400000).
// PlayerIns vt+0xE8 ("update post", 0x538F310 slot +0xE8; the subclass override 0x19085D0 calls
// it first). Its caller runs it after the behaviour step, so the action flags hold this update's
// TAE events. The 17-byte prologue is replayed (position-independent); the rest is proof.
constexpr u64 PostUpdateRva = 0x18F7FB0;
constexpr size_t PostUpdateLen = 17;
constexpr std::array<u8, 27> PostUpdateSig = {
    0x55,                                     // push rbp
    0x48, 0x89, 0xE5,                         // mov rbp, rsp
    0x41, 0x57, 0x41, 0x56, 0x41, 0x55,       // push r15; push r14; push r13
    0x41, 0x54, 0x53,                         // push r12; push rbx
    0x48, 0x83, 0xEC, 0x68,                   // sub rsp, 0x68  <- resume after this
    0x49, 0x89, 0xFC,                         // mov r12, rdi
    0x4C, 0x8B, 0x3D, 0x15, 0xBC, 0xAE, 0x03, // mov r15, [rip+0x3AEBC15] (stack guard)
};
// The frame time it reads: vmovss xmm0, [r12+0xE0].
constexpr u64 FrameTimeRva = 0x18F7FD2;
constexpr std::array<u8, 10> FrameTimeSig = {0xC4, 0xC1, 0x7A, 0x10, 0x84,
                                             0x24, 0xE0, 0x00, 0x00, 0x00};
constexpr u32 OffsetFrameTime = 0xE0; // PlayerIns, f32 seconds
// The subclass override calls it: call 0x18F7FB0.
constexpr u64 SubclassCallRva = 0x19085D9;
constexpr std::array<u8, 5> SubclassCallSig = {0xE8, 0xD2, 0xF9, 0xFE, 0xFF};
// TAE event 8 (the only writer of the bit): or byte [af+0x1BC], 2.
constexpr u64 Event8Rva = 0x1A22B79;
constexpr std::array<u8, 7> Event8Sig = {0x80, 0x8B, 0xBC, 0x01, 0x00, 0x00, 0x02};
// The per-update reset (0x1A0DF40, called from 0x1514380 at the end of PlayerIns::Update).
constexpr u64 ResetRva = 0x1A0DF93;
constexpr std::array<u8, 10> ResetSig = {0xC7, 0x83, 0xBC, 0x01, 0x00,
                                         0x00, 0x00, 0x00, 0x00, 0x00};
constexpr u64 ResetCallRva = 0x18F6CCA; // call 0x1514380
constexpr std::array<u8, 5> ResetCallSig = {0xE8, 0xB1, 0xD6, 0xC1, 0xFF};
// Hit validity (0x1A29C40): bit 1 set -> the hit is ignored unless the attack ignores i-frames.
constexpr u64 HitCheckRva = 0x1A29CFD;
constexpr std::array<u8, 5> HitCheckSig = {0xF6, 0xC1, 0x02, 0x74, 0x08}; // test cl, 2; jz
// PlayerIns IsInvincible (vt+0x4B0, a pure leaf; called by the log only): bit 0 or 1, the
// +0x1E1 flags, the +0x510 timer.
constexpr u64 IsInvincibleRva = 0x1904930;
constexpr std::array<u8, 124> IsInvincibleSig = {
    0x48, 0x8B, 0x4F, 0x18, 0x8A, 0x51, 0x20, 0xB0, 0x01, 0xF6, 0xC2, 0x01, 0x75, 0x6D, 0x40, 0x84,
    0xF6, 0x74, 0x16, 0x48, 0x8B, 0x8F, 0xB0, 0x03, 0x00, 0x00, 0x48, 0x8B, 0x49, 0x08, 0xF6, 0x81,
    0xBC, 0x01, 0x00, 0x00, 0x01, 0x75, 0x54, 0xEB, 0x3F, 0x8B, 0xB7, 0xE1, 0x01, 0x00, 0x00, 0xF7,
    0xC6, 0x00, 0x40, 0x00, 0x04, 0x75, 0x44, 0xF6, 0xC2, 0x02, 0x74, 0x0F, 0x48, 0x8B, 0x49, 0x30,
    0x48, 0x85, 0xC9, 0x74, 0x36, 0xF6, 0x41, 0x18, 0x01, 0x74, 0x30, 0x48, 0x8B, 0x8F, 0xB0, 0x03,
    0x00, 0x00, 0x48, 0x8B, 0x49, 0x08, 0x8B, 0x89, 0xBC, 0x01, 0x00, 0x00, 0x89, 0xCA, 0x83, 0xE2,
    0x01, 0x83, 0xE1, 0x02, 0x09, 0xD1, 0x75, 0x13, 0xC5, 0xFA, 0x10, 0x87, 0x10, 0x05, 0x00, 0x00,
    0xC5, 0xF0, 0x57, 0xC9, 0xC5, 0xF8, 0x2E, 0xC1, 0x0F, 0x97, 0xC0, 0xC3,
};
// ChrIns::IsMainPlayer: its lea r14, [rip+disp] proves the WorldChrMan global.
constexpr u64 IsMainPlayerRva = 0x18C2D00;
constexpr std::array<u8, 24> IsMainPlayerSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xFB, 0x4C, 0x8D,
    0x35, 0x67, 0xBB, 0xC7, 0x03, 0x49, 0x8B, 0x06, 0x48, 0x85, 0xC0, 0x75,
};
constexpr size_t IsMainPlayerLeaDisp = 13;
constexpr size_t IsMainPlayerLeaEnd = 17;
constexpr u64 WorldChrManRva = 0x553E878;
constexpr u32 OffsetMainPlayer = 0x60;

constexpr u32 OffsetFlagBits = 0x1BC; // action flag module: u32, bit 0 perfect, bit 1 i-frames
constexpr u8 BitIframe = 0x02;
constexpr u32 OffsetSpEffects = 0x1C8; // PlayerIns -> SpEffect list (walked like HKS env 56)
constexpr u32 OffsetEffectHead = 0x08;
constexpr u32 OffsetEffectId = 0x40;
constexpr u32 OffsetEffectNext = 0x58;
constexpr u32 MaxEffects = 256;
constexpr s32 EffectDodge = 392;    // quicksteps, rolls, wake-up rolls (TAE event 66)
constexpr s32 EffectBackstep = 393; // backsteps
constexpr u32 OffsetPhysicsModule = 0x68;
constexpr u32 OffsetLanded = 0x212;

constexpr u32 OffsetInvincibleState = 0x18; // PlayerIns -> the state IsInvincible reads first
using IsInvincibleFn = bool(PS4_SYSV_ABI*)(u8* chr, bool perfect_only);

u8* g_base = nullptr;
u64 g_size = 0;
std::unique_ptr<Xbyak::CodeGenerator> g_code;
bool g_installed = false;
int g_log = 0;
bool g_dry = false;
IsInvincibleFn g_is_invincible = nullptr;

// Counters (the hook and site A may run on different threads, one at a time).
std::atomic<u64> g_updates{0};
std::atomic<u64> g_f8_updates{0};
std::atomic<u64> g_windows{0};
std::atomic<u64> g_extended{0};
std::atomic<u64> g_not_extended{0};
std::atomic<u64> g_asserts{0};
std::atomic<u64> g_asserts_without_392{0};
std::atomic<u64> g_stale_clears{0};
std::atomic<u64> g_backsteps{0};
std::atomic<u32> g_last_total{0}; // F8 + extension updates of the last finished window
std::atomic<u64> g_site_updates{0};
std::atomic<u64> g_site_bit{0};
std::atomic<u64> g_site_inv{0};
std::atomic<u64> g_site_inv_without_bit{0};
// IframesAttackCut: the newest request (taken by the next hook call) and its text.
std::atomic<bool> g_cut_request{false};
std::atomic<const char*> g_cut_what{"attack"};
std::atomic<u32> g_cut_serial{0};
std::atomic<s64> g_cut_ns{0};
std::atomic<u64> g_cuts{0};
std::atomic<u64> g_cut_clears{0};
std::atomic<u64> g_cut_pending{0};    // cuts that arrived before their dodge's window opened
std::atomic<u64> g_cut_pending_hit{0}; // ... whose window then opened cut

using Clock = std::chrono::steady_clock;

double Ms(Clock::duration d) {
    return std::chrono::duration<double, std::milli>(d).count();
}

std::string Rva(const u8* pointer) {
    if (pointer >= g_base && pointer < g_base + g_size) {
        return fmt::format("+{:#x}", static_cast<u64>(pointer - g_base));
    }
    return fmt::format("{}", fmt::ptr(pointer));
}

u8* MainPlayer() {
    u8* const world = Read<u8*>(g_base + WorldChrManRva);
    if (!Plausible(world)) {
        return nullptr;
    }
    u8* const player = Read<u8*>(world + OffsetMainPlayer);
    return Plausible(player) ? player : nullptr;
}

// The action flag module of `player`, its vtable and owner checked.
u8* ActionFlags(u8* player, u8* bag) {
    u8* const af = Read<u8*>(bag + OffsetActionFlagModule);
    if (!Plausible(af) || Read<u8*>(af) != g_base + ActionFlagVtableRva ||
        Read<u8*>(af + OffsetOwner) != player) {
        return nullptr;
    }
    return af;
}

const char* ClipName(s32 id) {
    if (id < 0) {
        return "unknown";
    }
    switch (M::ClassifyMotionClip(id).kind) {
    case M::Kind::Backstep:
        return "backstep";
    case M::Kind::Quickstep:
        return "quickstep";
    case M::Kind::QuickstepOhb:
        return "quickstep_ohb";
    case M::Kind::Roll:
        return "roll";
    case M::Kind::RollOhb:
        return "roll_ohb";
    case M::Kind::WakeRoll:
        return "wake_roll";
    case M::Kind::Jump:
        return "jump";
    case M::Kind::Land:
        return "land";
    default:
        return "other";
    }
}

// --- The hook (main player, PlayerIns vt+0xE8 entry) ---------------------------------------

// One TimeAct ring entry: every behaviour step pushes each clip it advanced, with the clip's
// animation time before and after the step and a third value (logged).
struct Push {
    s32 id{-1};
    float t0{};
    float t1{};
    float f2{};
};

struct HookState {
    u8* player{};
    bool first_logged{};
    bool af_warned{};
    u8* phys_vtable{};
    // TimeAct ring cursor.
    u8* ring_module{};
    s32 ring_write{-1};
    // The window being measured: its clip and the clip's animation times when the clip is in the
    // ring every update (anim), the window's own game time otherwise.
    s32 win_clip{-1};
    bool anim{};
    double win_start{};
    double win_end{};
    double ext_end{};
    double game_t{}; // game time since the window started
    double win_game{};
    Clock::time_point win_wall0{};
    Clock::time_point ext_wall0{};
    u32 vanilla_updates{};
    u32 ext_updates{};
    double ext_time{};
    float dt_end{};
    // Backsteps (SpEffect 393).
    bool sp393{};
    bool bs_f8{};
    u32 bs_asserts{};
    u32 bs_updates{};
    s32 bs_clip{-1};
    // Level 2: per-update lines after the last dodge-related update.
    u32 trailing{};
    Clock::time_point last_stats{};

};
HookState g_hook{};
VR::Interactions::NativeIframesRuntime g_iframes; // Main player update owner only.
VR::Interactions::Module* g_iframes_module{};
bool g_iframes_fault{};
void InitIframesDll() {
    if (!VR::Interactions::DllRequested() || g_iframes_module)
        return;
    g_iframes_module = new VR::Interactions::Module();
    g_iframes_module->EnableFaultBroadcast();
    g_iframes.Bind(*g_iframes_module, g_iframes_fault);
    if (!g_iframes_module->LoadBesideExecutable()) {
        g_iframes_fault = true;
        g_iframes_module->ReportFault();
        LOG_ERROR(Core, "VR iframes: interaction DLL unavailable: {}",
                  g_iframes_module->Diagnostic());
    }
}

struct Sample {
    bool tae{};
    bool reset_ran{};
    bool sp392{};
    bool sp393{};
    bool grounded{};
    bool phys_ok{};
    s32 hp{-1};
    float dt{};
    bool interrupted{};
    s32 interrupt_clip{-1};
    std::array<Push, TimeActRingSize> pushes{};
    s32 push_count{};
};

void ReadRing(HookState& h, u8* bag, Sample& s) {
    u8* const ta = Read<u8*>(bag + OffsetTimeActModule);
    if (!Plausible(ta) || Read<u8*>(ta) != g_base + TimeActVtableRva) {
        return;
    }
    const s32 write = Read<s32>(ta + OffsetTimeActWrite);
    if (write < 0 || write >= TimeActRingSize) {
        return;
    }
    if (ta != h.ring_module || h.ring_write < 0) {
        h.ring_module = ta;
        h.ring_write = write;
        return;
    }
    const s32 count = (write - h.ring_write + TimeActRingSize) % TimeActRingSize;
    for (s32 k = 0; k < count; ++k) {
        const s32 index = (h.ring_write + k) % TimeActRingSize;
        const u8* const entry = ta + OffsetTimeActRing + index * TimeActEntrySize;
        Push& p = s.pushes[s.push_count++];
        p.id = Read<s32>(entry);
        p.t0 = Read<float>(entry + 4);
        p.t1 = Read<float>(entry + 8);
        p.f2 = Read<float>(entry + 12);
    }
    h.ring_write = write;
}

const Push* FindPush(const Sample& s, s32 id) {
    for (s32 i = s.push_count - 1; i >= 0; --i) {
        if (s.pushes[i].id == id) {
            return &s.pushes[i];
        }
    }
    return nullptr;
}

// The clip a window belongs to: a dodge clip, else a jump / fall / damage clip, else the newest.
const Push* PickClip(const Sample& s) {
    const Push* other = nullptr;
    for (s32 i = s.push_count - 1; i >= 0; --i) {
        if (I::IsDodgeClip(s.pushes[i].id)) {
            return &s.pushes[i];
        }
        if (other == nullptr && I::IsInterruptClip(s.pushes[i].id)) {
            other = &s.pushes[i];
        }
    }
    return other != nullptr ? other : (s.push_count > 0 ? &s.pushes[s.push_count - 1] : nullptr);
}

Sample ReadSample(HookState& h, u8* player, u8* bag, u8* af) {
    Sample s{};
    s.tae = (Read<u8>(af + OffsetFlagBits) & BitIframe) != 0;
    u8* const behavior = Read<u8*>(bag + OffsetBehaviorModule);
    s.reset_ran = Plausible(behavior) && Read<u8>(behavior + OffsetBehaviorReady) != 0;
    u8* const list = Read<u8*>(player + OffsetSpEffects);
    if (Plausible(list)) {
        u8* node = Read<u8*>(list + OffsetEffectHead);
        for (u32 i = 0; i < MaxEffects && Plausible(node); ++i) {
            const s32 id = Read<s32>(node + OffsetEffectId);
            s.sp392 = s.sp392 || id == EffectDodge;
            s.sp393 = s.sp393 || id == EffectBackstep;
            node = Read<u8*>(node + OffsetEffectNext);
        }
    }
    u8* const physics = Read<u8*>(bag + OffsetPhysicsModule);
    if (Plausible(physics)) {
        u8* const vtable = Read<u8*>(physics);
        if (h.phys_vtable == nullptr) {
            h.phys_vtable = vtable;
        }
        s.phys_ok = vtable == h.phys_vtable;
        s.grounded = s.phys_ok && Read<u8>(physics + OffsetLanded) != 0;
    }
    u8* const data = Read<u8*>(bag + OffsetDataModule);
    if (Plausible(data)) {
        s.hp = Read<s32>(data + OffsetHp);
    }
    s.dt = Read<float>(player + OffsetFrameTime);
    ReadRing(h, bag, s);
    return s;
}

std::string ClipText(s32 id) {
    return id >= 0 ? fmt::format("{} {}", id, ClipName(id)) : std::string{"unknown"};
}

void LogWindowEnd(const HookState& h, const I::Output& o, bool sp392, Clock::time_point now) {
    const char* const clock = h.anim ? "anim" : "game";
    if (o.window_end && !o.extended) {
        LOG_INFO(Core,
                 "VR iframes: F8 window clip={} {} t={:.3f}-{:.3f} s ({} updates, {:.1f} ms game, "
                 "{:.1f} ms wall) NOT extended ({}) sp392={} dt={:.1f} ms",
                 ClipText(h.win_clip), clock, h.win_start, h.win_end, h.vanilla_updates,
                 h.win_game * 1000.0, Ms(now - h.win_wall0), I::EndName(o.end), sp392 ? 1 : 0,
                 h.dt_end * 1000.0f);
        return;
    }
    LOG_INFO(Core,
             "VR iframes: DODGE clip={} {} i-frames vanilla t={:.3f}-{:.3f} s ({} updates, {:.1f} "
             "ms game, {:.1f} ms wall) -> extended end t={:.3f} s (+{} updates, +{:.1f} ms game, "
             "+{:.1f} ms wall; target +{:.1f} ms) end={}{} dt={:.1f} ms",
             ClipText(h.win_clip), clock, h.win_start, h.win_end, h.vanilla_updates,
             h.win_game * 1000.0, Ms(h.ext_wall0 - h.win_wall0), h.ext_end, h.ext_updates,
             h.ext_time * 1000.0, Ms(now - h.ext_wall0), g_iframes.Extra() * 1000.0f, I::EndName(o.end),
             g_dry ? " DRYRUN" : "", h.dt_end * 1000.0f);
}

void StatsLine(HookState& h, Clock::time_point now) {
    if (now - h.last_stats < std::chrono::seconds(10)) {
        return;
    }
    h.last_stats = now;
    const u64 f8 = g_f8_updates.load(std::memory_order_relaxed);
    const u64 asserts = g_asserts.load(std::memory_order_relaxed);
    const u64 site_bit = g_site_bit.load(std::memory_order_relaxed);
    LOG_INFO(
        Core,
        "VR iframes: STATS updates={} f8_updates={} windows={} extended={} not_extended={} "
        "ext_updates={} ext_without_392={} stale_clears={} backsteps={} cuts={} (pending {}, "
        "took their window {}) cut_clears={} | site A: updates={} bit={} (f8+ext={}{}) "
        "IsInvincible={} inv_without_bit={}",
        g_updates.load(std::memory_order_relaxed), f8, g_windows.load(std::memory_order_relaxed),
        g_extended.load(std::memory_order_relaxed), g_not_extended.load(std::memory_order_relaxed),
        asserts, g_asserts_without_392.load(std::memory_order_relaxed),
        g_stale_clears.load(std::memory_order_relaxed), g_backsteps.load(std::memory_order_relaxed),
        g_cuts.load(std::memory_order_relaxed), g_cut_pending.load(std::memory_order_relaxed),
        g_cut_pending_hit.load(std::memory_order_relaxed),
        g_cut_clears.load(std::memory_order_relaxed),
        g_site_updates.load(std::memory_order_relaxed), site_bit, g_dry ? f8 : f8 + asserts,
        g_dry ? " dry" : "", g_site_inv.load(std::memory_order_relaxed),
        g_site_inv_without_bit.load(std::memory_order_relaxed));
}

void PS4_SYSV_ABI IframesHook(u8* player, u8* /*saved_ymm*/) {
    if (player == nullptr || player != MainPlayer()) {
        return;
    }
    HookState& h = g_hook;
    u8* const bag = Read<u8*>(player + OffsetChrModules);
    if (!Plausible(bag)) {
        return;
    }
    u8* const af = ActionFlags(player, bag);
    if (af == nullptr) {
        if (!h.af_warned) {
            h.af_warned = true;
            u8* const raw = Read<u8*>(bag + OffsetActionFlagModule);
            LOG_WARNING(
                Core,
                "VR iframes: action-flag module {} has vtable {} owner {} (expected +{:#x}, "
                "{}); skipped while it lasts",
                fmt::ptr(raw), Plausible(raw) ? Rva(Read<u8*>(raw)) : "none",
                Plausible(raw) ? fmt::ptr(Read<u8*>(raw + OffsetOwner)) : nullptr,
                ActionFlagVtableRva, fmt::ptr(player));
        }
        return;
    }
    const auto now = Clock::now();
    if (player != h.player) {
        // A new player instance (load): the old one and any bit on it are gone.
        const bool logged = h.first_logged;
        h = HookState{};
        g_iframes.Reset();
        h.first_logged = logged;
        h.player = player;
        h.last_stats = now;
    }
    g_updates.fetch_add(1, std::memory_order_relaxed);

    Sample s = ReadSample(h, player, bag, af);
    const float dt = std::isfinite(s.dt) ? std::clamp(s.dt, 0.0f, I::MaxDt) : I::GameFrame;
    if (!h.first_logged) {
        h.first_logged = true;
        LOG_INFO(Core,
                 "VR iframes: first main-player update player={} af={} (vtable ok, owner ok) "
                 "thread '{}' dt={:.2f} ms behaviour={} sp392={} grounded={} hp={}",
                 fmt::ptr(player), fmt::ptr(af), Common::GetCurrentThreadName(), s.dt * 1000.0f,
                 s.reset_ran ? 1 : 0, s.sp392 ? 1 : 0, s.grounded ? 1 : 0, s.hp);
    }

    I::Input in{};
    in.tae = s.tae;
    in.reset_ran = s.reset_ran;
    in.dodge = s.sp392;
    in.grounded = s.grounded;
    in.alive = s.hp > 0;
    in.dt = s.dt;
    in.cut = g_cut_request.exchange(false, std::memory_order_acq_rel);
    static_assert(TimeActRingSize == PI_TIMING_CLIP_CAPACITY);
    auto facts = PhysicalInteractions::NativeTiming::Pack(in);
    facts.sp393 = s.sp393;
    facts.push_count = static_cast<uint32_t>(s.push_count);
    for (s32 i = 0; i < s.push_count; ++i) {
        facts.pushes[i] = {s.pushes[i].id, s.pushes[i].t0};
    }
    const I::Output o = g_iframes.StepRaw(facts);
    const auto& frame = g_iframes.Frame();
    const bool was_pending = frame.was_pending;
    const s32 dodge_id = frame.dodge_id;
    in.dodge_clip = frame.dodge_clip;
    in.dodge_start = frame.dodge_start;
    in.interrupted = frame.interrupted;
    s.interrupted = frame.interrupted;
    s.interrupt_clip = frame.interrupt_clip;
    if (o.window_start && was_pending && o.cut_bit) {
        g_cut_pending_hit.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO(Core,
                 "VR iframes: the pending cut takes the dodge's i-frame window as it opens (clip "
                 "{}): no i-frames{}",
                 dodge_id >= 0 ? ClipText(dodge_id) : std::string{"-"}, g_dry ? " DRYRUN" : "");
    }

    if (!g_dry) {
        std::atomic_ref<u8> bits(af[OffsetFlagBits]);
        if (o.assert_bit) {
            bits.fetch_or(BitIframe, std::memory_order_relaxed);
        } else if (o.clear_bit || o.cut_bit) {
            bits.fetch_and(static_cast<u8>(~BitIframe), std::memory_order_relaxed);
        }
    }
    if (o.cut_bit) {
        g_cut_clears.fetch_add(1, std::memory_order_relaxed);
    }
    if (in.cut && in.interrupted) {
        LOG_INFO(Core,
                 "VR iframes: the {} #{} came with a damage, grab, fall or death clip ({}): the "
                 "dodge is over, no cut (the clip keeps its own i-frames)",
                 g_cut_what.load(std::memory_order_relaxed),
                 g_cut_serial.load(std::memory_order_relaxed), ClipText(s.interrupt_clip));
    }
    if (o.cut_start) {
        g_cuts.fetch_add(1, std::memory_order_relaxed);
        const bool pending = frame.pending_wait;
        if (pending) {
            g_cut_pending.fetch_add(1, std::memory_order_relaxed);
        }
        const Push* const p = PickClip(s);
        const s64 lag = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count() - g_cut_ns.load(std::memory_order_relaxed);
        LOG_INFO(Core,
                 "VR iframes: CUT by the {} #{} ({} ms after it): the dodge's invulnerability "
                 "ends (clip {}, {} i-frame window, sp392={} sp393={}){}",
                 g_cut_what.load(std::memory_order_relaxed),
                 g_cut_serial.load(std::memory_order_relaxed), lag / 1'000'000,
                 p != nullptr ? ClipText(p->id) : std::string{"-"},
                 g_iframes.InWindow() ? fmt::format("in its ({} updates)", g_iframes.WindowUpdates())
                 : o.extension_end ? std::string{"in its extension (ended)"}
                 : pending         ? std::string{"pending: waits for its"}
                 : !in.dodge_clip  ? std::string{"before any dodge clip: waits an update for its"}
                                   : std::string{"past its"},
                 s.sp392 ? 1 : 0, s.sp393 ? 1 : 0, g_dry ? " DRYRUN" : "");
    }

    // Bookkeeping and logs, in the order the window lives: its end, the extension, a new start.
    if (o.clear_bit) {
        g_stale_clears.fetch_add(1, std::memory_order_relaxed);
        if (g_log >= 1) {
            LOG_INFO(Core, "VR iframes: STALE our bit survived a skipped reset; cleared");
        }
    }
    if (o.window_end) {
        g_last_total.store(h.vanilla_updates, std::memory_order_relaxed);
        if (o.extended) {
            g_extended.fetch_add(1, std::memory_order_relaxed);
            h.ext_wall0 = now;
            h.ext_end = h.win_end;
            h.ext_updates = 0;
            h.ext_time = 0.0;
        } else {
            g_not_extended.fetch_add(1, std::memory_order_relaxed);
            if (g_log >= 1) {
                LogWindowEnd(h, o, s.sp392, now);
            }
        }
    }
    if (o.assert_bit) {
        g_asserts.fetch_add(1, std::memory_order_relaxed);
        if (!s.sp392) {
            g_asserts_without_392.fetch_add(1, std::memory_order_relaxed);
        }
        ++h.ext_updates;
        h.ext_time += dt;
        h.game_t += dt;
        const Push* const p = h.anim ? FindPush(s, h.win_clip) : nullptr;
        h.ext_end = p != nullptr ? p->t1 : h.win_end + h.ext_time;
        if (h.sp393) {
            ++h.bs_asserts;
        }
    }
    if (o.extension_end) {
        g_last_total.store(h.vanilla_updates + h.ext_updates, std::memory_order_relaxed);
        if (g_log >= 1) {
            LogWindowEnd(h, o, s.sp392, now);
        }
    }
    if (o.window_start) {
        g_windows.fetch_add(1, std::memory_order_relaxed);
        const Push* const p = PickClip(s);
        h.win_clip = p != nullptr ? p->id : -1;
        h.anim = p != nullptr;
        h.win_start = h.anim ? p->t0 : 0.0;
        h.game_t = 0.0;
        h.win_wall0 = now;
        if (h.sp393) {
            h.bs_f8 = true;
        }
    }
    if (g_iframes.InWindow() && s.tae) {
        g_f8_updates.fetch_add(1, std::memory_order_relaxed);
        h.game_t += dt;
        const Push* const p = h.anim ? FindPush(s, h.win_clip) : nullptr;
        if (h.anim && p == nullptr) {
            // The clip left the ring: the rest of the window in game time from its start.
            h.anim = false;
            h.win_start = 0.0;
        }
        h.win_end = h.anim ? p->t1 : h.game_t;
        h.win_game = h.game_t;
        h.vanilla_updates = g_iframes.WindowUpdates();
        h.dt_end = dt;
    }

    // Backsteps: SpEffect 393 without any F8 window.
    if (s.sp393 && !h.sp393) {
        const Push* const p = PickClip(s);
        h.bs_f8 = g_iframes.InWindow();
        h.bs_asserts = 0;
        h.bs_updates = 0;
        h.bs_clip = p != nullptr ? p->id : -1;
    }
    if (s.sp393) {
        ++h.bs_updates;
    }
    if (!s.sp393 && h.sp393) {
        g_backsteps.fetch_add(1, std::memory_order_relaxed);
        if (g_log >= 1) {
            LOG_INFO(Core,
                     "VR iframes: BACKSTEP clip={} (sp393 for {} updates): vanilla i-frames {}, "
                     "extension updates {}",
                     ClipText(h.bs_clip), h.bs_updates, h.bs_f8 ? "SEEN" : "none", h.bs_asserts);
        }
    }
    h.sp393 = s.sp393;

    if (g_log >= 2) {
        const bool busy =
            s.tae || o.assert_bit || o.window_end || o.extension_end || s.sp392 || s.sp393;
        h.trailing = busy ? 6 : (h.trailing > 0 ? h.trailing - 1 : 0);
        if (busy || h.trailing > 0) {
            std::string ring;
            for (s32 i = 0; i < s.push_count; ++i) {
                const Push& p = s.pushes[i];
                ring += fmt::format(" {}({:.3f},{:.3f},{:.3f})", p.id, p.t0, p.t1, p.f2);
            }
            LOG_INFO(Core,
                     "VR iframes: U dt={:.2f} tae={} rr={} sp392={} sp393={} gr={} hp={} {}{}{} "
                     "left={:.1f} ms win={} ext={} ring:{}",
                     dt * 1000.0f, s.tae ? 1 : 0, s.reset_ran ? 1 : 0, s.sp392 ? 1 : 0,
                     s.sp393 ? 1 : 0, s.grounded ? 1 : 0, s.hp,
                     o.assert_bit ? "ASSERT" : (o.clear_bit ? "CLEAR" : "-"),
                     s.interrupted ? fmt::format(" interrupt={}", s.interrupt_clip) : "",
                     o.window_start ? " START" : "", g_iframes.Left() * 1000.0f, g_iframes.WindowUpdates(),
                     g_iframes.ExtensionUpdates(), ring);
        }
    }
    if (g_log >= 1) {
        StatsLine(h, now);
    }
}

// --- Site A (inside PlayerIns::Update, before its reset): what the game sees ------------------

struct SiteState {
    u8* player{};
    bool in_run{};
    u32 run_updates{};
    u32 run_inv{};
    double run_game{};
    Clock::time_point run_wall0{};
};
SiteState g_site{};

bool ProvesWorldChrMan(const u8* module) {
    if (std::memcmp(module + IsMainPlayerRva, IsMainPlayerSig.data(), IsMainPlayerSig.size()) !=
        0) {
        return false;
    }
    s32 displacement{};
    std::memcpy(&displacement, module + IsMainPlayerRva + IsMainPlayerLeaDisp,
                sizeof(displacement));
    return IsMainPlayerRva + IsMainPlayerLeaEnd + static_cast<s64>(displacement) == WorldChrManRva;
}

// jmp qword [rip+0]; dq target
void EmitAbsoluteJump(Xbyak::CodeGenerator& c, u64 target) {
    c.db(0xFF);
    c.db(0x25);
    c.dd(0);
    c.dq(target);
}

int EnvLevel(const char* name, const char* alias) {
    const char* value = std::getenv(name);
    if (value == nullptr) {
        value = std::getenv(alias);
    }
    if (value == nullptr) {
        return 0;
    }
    return std::clamp(std::atoi(value), 1, 2);
}

} // Anonymous namespace

void InstallIframes(VAddr base, u64 size) {
    if (g_code || g_installed) {
        return;
    }
    auto* const module = reinterpret_cast<u8*>(base);
    g_base = module;
    g_size = size;
    g_log = EnvLevel("VR_IFRAMES_LOG", "VR_IFRAME_LOG");
    g_dry = std::getenv("VR_IFRAMES_DRYRUN") != nullptr;
    const auto& settings = VR::GetSettings();
    const s32 frames = settings.dodge_iframes_extra;
    // dodge_attack with dodge_attack_iframes=0 needs the hook to end a dodge's i-frames.
    const bool cut = settings.dodge_attack && !settings.dodge_attack_iframes;
    if (frames <= 0 && !cut) {
        LOG_INFO(Core, "VR iframes: dodge_iframes_extra=0, vanilla dodge i-frames (not installed)");
        return;
    }
    if (size <= WorldChrManRva + 8) {
        LOG_WARNING(Core, "VR iframes: eboot too small ({:#x}); not installed", size);
        return;
    }
    const auto matches = [module](u64 rva, const auto& signature) {
        return std::memcmp(module + rva, signature.data(), signature.size()) == 0;
    };
    const bool site_ok = matches(PostUpdateRva, PostUpdateSig);
    const bool dt_ok = matches(FrameTimeRva, FrameTimeSig);
    const bool sub_ok = matches(SubclassCallRva, SubclassCallSig);
    const bool f8_ok = matches(Event8Rva, Event8Sig);
    const bool reset_ok = matches(ResetRva, ResetSig) && matches(ResetCallRva, ResetCallSig);
    const bool hit_ok = matches(HitCheckRva, HitCheckSig);
    const bool inv_ok = matches(IsInvincibleRva, IsInvincibleSig);
    const bool wcm_ok = ProvesWorldChrMan(module);
    const auto verdict = [](bool ok) { return ok ? "ok" : "MISMATCH"; };
    const std::string verdicts =
        fmt::format("site {}, frame time {}, subclass call {}, event 8 {}, reset {}, hit check {}, "
                    "IsInvincible {}, WorldChrMan {}",
                    verdict(site_ok), verdict(dt_ok), verdict(sub_ok), verdict(f8_ok),
                    verdict(reset_ok), verdict(hit_ok), verdict(inv_ok), verdict(wcm_ok));
    if (!site_ok || !dt_ok || !sub_ok || !f8_ok || !reset_ok || !hit_ok || !inv_ok || !wcm_ok) {
        LOG_WARNING(Core, "VR iframes: not installed ({}); live bytes at {:#x}: {}", verdicts,
                    PostUpdateRva, Hook::HexBytes(module + PostUpdateRva, PostUpdateSig.size()));
        return;
    }
    u8* const site = module + PostUpdateRva;
    const u64 resume = reinterpret_cast<u64>(site + PostUpdateLen);
    try {
        g_code = std::make_unique<Xbyak::CodeGenerator>(1024);
        using namespace Xbyak::util;
        auto& c = *g_code;
        void* const host = Hook::EmitHostCallStub(c, reinterpret_cast<void*>(&IframesHook),
                                                  PostUpdateSig.data(), PostUpdateLen, resume, rdi);
        // Entry: only the main player goes through the host call (rax and the flags are dead at
        // a function entry); everyone else replays the prologue and goes on.
        c.align(16);
        auto* const entry = const_cast<u8*>(c.getCurr());
        Xbyak::Label skip;
        c.mov(rax, reinterpret_cast<u64>(module + WorldChrManRva));
        c.mov(rax, ptr[rax]);
        c.test(rax, rax);
        c.jz(skip);
        c.cmp(rdi, ptr[rax + OffsetMainPlayer]);
        c.jne(skip);
        c.jmp(host, Xbyak::CodeGenerator::T_NEAR);
        c.L(skip);
        for (size_t i = 0; i < PostUpdateLen; ++i) {
            c.db(PostUpdateSig[i]);
        }
        EmitAbsoluteJump(c, resume);
        c.ready();
        if (!Hook::MakeWritable(site, PostUpdateLen)) {
            LOG_ERROR(Core, "VR iframes: could not make {:#x} writable; not installed",
                      PostUpdateRva);
            return;
        }
        InitIframesDll();
        g_iframes.SetExtra(static_cast<float>(frames) * I::GameFrame);
        g_is_invincible = reinterpret_cast<IsInvincibleFn>(module + IsInvincibleRva);
        Hook::WriteJump(site, PostUpdateLen, entry);
        g_installed = true;
    } catch (const Xbyak::Error& error) {
        LOG_ERROR(Core, "VR iframes: code emission failed ({}); not installed", error.what());
        return;
    }
    LOG_INFO(Core,
             "VR iframes: installed at {:#x} (PlayerIns vt+0xE8 entry) extra={} frames ({:.1f} ms) "
             "after quickstep/roll i-frames, an attack during a dodge {} ({}) log={}{}",
             PostUpdateRva, frames, static_cast<float>(std::max(frames, 0)) * I::GameFrame * 1000.0f,
             !settings.dodge_attack      ? "waits for its window (dodge_attack=0)"
             : cut                       ? "ends its i-frames (dodge_attack_iframes=0)"
                                         : "keeps them (dodge_attack_iframes=1)",
             verdicts, g_log, g_dry ? " DRYRUN (the bit is never written)" : "");
}

void IframesSiteA(u8* player) {
    if (!g_installed || g_log == 0 || player == nullptr) {
        return;
    }
    u8* const bag = Read<u8*>(player + OffsetChrModules);
    if (!Plausible(bag)) {
        return;
    }
    u8* const af = ActionFlags(player, bag);
    if (af == nullptr) {
        return;
    }
    SiteState& st = g_site;
    if (player != st.player) {
        st = SiteState{};
        st.player = player;
    }
    const auto now = Clock::now();
    const bool bit = (Read<u8>(af + OffsetFlagBits) & BitIframe) != 0;
    // IsInvincible dereferences [chr+0x18] unconditionally.
    const bool inv =
        Plausible(Read<u8*>(player + OffsetInvincibleState)) && g_is_invincible(player, false);
    const float raw_dt = Read<float>(player + OffsetFrameTime);
    const float dt = std::isfinite(raw_dt) ? std::clamp(raw_dt, 0.0f, I::MaxDt) : 0.0f;
    g_site_updates.fetch_add(1, std::memory_order_relaxed);
    if (bit) {
        g_site_bit.fetch_add(1, std::memory_order_relaxed);
    }
    if (inv) {
        g_site_inv.fetch_add(1, std::memory_order_relaxed);
        if (!bit) {
            g_site_inv_without_bit.fetch_add(1, std::memory_order_relaxed);
        }
    }
    if (bit) {
        if (!st.in_run) {
            st.in_run = true;
            st.run_updates = 0;
            st.run_inv = 0;
            st.run_game = 0.0;
            st.run_wall0 = now;
        }
        ++st.run_updates;
        st.run_inv += inv ? 1 : 0;
        st.run_game += dt;
    } else if (st.in_run) {
        st.in_run = false;
        const u32 expected = g_last_total.load(std::memory_order_relaxed);
        LOG_INFO(Core,
                 "VR iframes: SITE-A protected run {} updates ({:.1f} ms game, {:.1f} ms wall), "
                 "game IsInvincible {}/{}, after the run {}; hook window+extension {} updates {}",
                 st.run_updates, st.run_game * 1000.0, Ms(now - st.run_wall0), st.run_inv,
                 st.run_updates, inv ? "STILL INVINCIBLE" : "not invincible", expected,
                 expected == st.run_updates ? "(match)" : "(MISMATCH)");
    }
}

void IframesAttackCut(const char* what, u32 serial) {
    const auto& settings = VR::GetSettings();
    if (!settings.dodge_attack || settings.dodge_attack_iframes) {
        return; // dodge_attack_iframes=1: the dodge keeps its i-frames (pure fun)
    }
    if (!g_installed) {
        static std::atomic<bool> logged{false};
        if (!logged.exchange(true)) {
            LOG_WARNING(Core, "VR iframes: an attack during a dodge cannot end its i-frames (the "
                              "hook is not installed)");
        }
        return;
    }
    g_cut_what.store(what != nullptr ? what : "attack", std::memory_order_relaxed);
    g_cut_serial.store(serial, std::memory_order_relaxed);
    g_cut_ns.store(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now().time_since_epoch()).count(), std::memory_order_relaxed);
    g_cut_request.store(true, std::memory_order_release);
}

bool IsIframesInstalled() {
    return g_installed;
}

} // namespace VR::Bloodborne
