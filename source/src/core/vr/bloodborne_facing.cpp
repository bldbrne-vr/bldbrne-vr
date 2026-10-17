// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/thread.h"
#include "core/vr/bloodborne_facing.h"
#include "core/vr/body_channel.h"
#include "core/vr/body_facing.h"
#include "core/vr/body_settings.h"
#include "core/vr/hook_util.h"
#include "core/vr/interaction_native_facing.h"
#include "core/vr/live_tune.h"
#include "core/vr/vr_session.h"
#include "input/controller.h"

namespace VR::Bloodborne {

namespace {

namespace B = VR::Body;
using Hook::Plausible;
using Hook::Read;

// Module-relative addresses in the 1.09 eboot (patch file address - 0x400000).
// LockTgtMan::Update(LockTgtMan* [0x553E860], PlayerIns*, float dt): its prologue.
constexpr u64 LockUpdateRva = 0x188E900;
constexpr std::array<u8, 20> LockUpdateSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, // push rbp; mov rbp, rsp; push ...
    0x41, 0x54, 0x53, 0x48, 0x81, 0xEC, 0x78, 0x03, 0x00, 0x00, // ...; sub rsp, 0x378
};
// Its two callers set up the arguments and call it; each site is the setup plus the call (no
// branch lands inside either range). The stub replays the setup, calls the original, then
// OnLockUpdated, and jumps back after the call.
struct CallSite {
    u64 rva;
    std::array<u8, 23> bytes;
    size_t setup; // bytes before the 5-byte call
};
constexpr std::array<CallSite, 2> CallSites = {{
    {0x1917014,
     {
         0x48, 0x8B, 0x38,                               // mov rdi, [rax]
         0xC5, 0xFA, 0x10, 0x85, 0x6C, 0xFE, 0xFF, 0xFF, // vmovss xmm0, [rbp-0x194]
         0x48, 0x8B, 0xB5, 0x60, 0xFE, 0xFF, 0xFF,       // mov rsi, [rbp-0x1A0]
         0xE8, 0xD5, 0x78, 0xF7, 0xFF,                   // call 0x188E900
     },
     18},
    {0x1918C69,
     {
         0x48, 0x8B, 0x38,                         // mov rdi, [rax]
         0xC5, 0xFA, 0x10, 0x45, 0xAC,             // vmovss xmm0, [rbp-0x54]
         0x48, 0x8B, 0xB5, 0x70, 0xFF, 0xFF, 0xFF, // mov rsi, [rbp-0x90]
         0xE8, 0x83, 0x5C, 0xF7, 0xFF,             // call 0x188E900
     },
     15},
}};
// Inside it, the locked branch (0x1890B19): PlayerIns+0xF8 = 1, ChrCtrl (+0x58) +0x245 = 0 (after
// resetting the pad manipulator's +0x194 when it changes) and the lock point PlayerIns+0x100 =
// the target's (r15) float4. The layout the writes below copy.
constexpr u64 LockedBranchRva = 0x1890B19;
constexpr std::array<u8, 40> LockedBranchSig = {
    0xC6, 0x80, 0xF8, 0x00, 0x00, 0x00, 0x01,       // mov byte [rax+0xF8], 1
    0x48, 0x8B, 0x58, 0x58,                         // mov rbx, [rax+0x58]
    0x49, 0x89, 0xC5,                               // mov r13, rax
    0x80, 0xBB, 0x45, 0x02, 0x00, 0x00, 0x00,       // cmp byte [rbx+0x245], 0
    0x74, 0x21,                                     // jz
    0x48, 0x8B, 0x03, 0x48, 0x89, 0xDF, 0xFF, 0x50, // mov rax, [rbx]; mov rdi, rbx; call [rax+0x48]
    0x48, 0x48, 0x85, 0xC0, 0x74, 0x13, 0x48, 0x8B, // test rax, rax; jz; ...
    0x03,
};
constexpr u64 LockPointStoreRva = 0x1890B51;
constexpr std::array<u8, 20> LockPointStoreSig = {
    0xC6, 0x83, 0x45, 0x02, 0x00, 0x00, 0x00, // mov byte [rbx+0x245], 0
    0xC4, 0xC1, 0x78, 0x28, 0x07,             // vmovaps xmm0, [r15]
    0xC4, 0xC1, 0x78, 0x29, 0x85, 0x00, 0x01, // vmovaps [r13+0x100], xmm0
    0x00,
};
// HKS env 18 ("locked on") for players: !ChrCtrl+0x245 (0x1A13B55).
constexpr u64 EnvLockRva = 0x1A13B55;
constexpr std::array<u8, 35> EnvLockSig = {
    0x49, 0x8B, 0x00,                         // mov rax, [r8]
    0x4C, 0x89, 0xC7,                         // mov rdi, r8
    0x4C, 0x89, 0xC3,                         // mov rbx, r8
    0xFF, 0x90, 0xC0, 0x01, 0x00, 0x00,       // call [rax+0x1C0] (a player character)
    0x84, 0xC0,                               // test al, al
    0x0F, 0x84, 0x4D, 0x1A, 0x00, 0x00,       // jz (others: PlayerIns+0xF8)
    0x48, 0x8B, 0x43, 0x58,                   // mov rax, [rbx+0x58]
    0x0F, 0xB6, 0xB0, 0x45, 0x02, 0x00, 0x00, // movzx esi, byte [rax+0x245]
    0x83,                                     // xor esi, 1
};
// The pad manipulator turns him toward the lock point while +0xF8 is set (0x1526B35).
constexpr u64 TurnToPointRva = 0x1526B35;
constexpr std::array<u8, 11> TurnToPointSig = {
    0x49, 0x8B, 0x47, 0x08,                   // mov rax, [r15+0x08]
    0x80, 0xB8, 0xF8, 0x00, 0x00, 0x00, 0x00, // cmp byte [rax+0xF8], 0
};

constexpr u64 WorldChrManRva = 0x553E878;
constexpr u32 OffsetMainPlayer = 0x60;
constexpr u32 OffsetLockChosen = 0xF8;
constexpr u32 OffsetLockPoint = 0x100;
constexpr u32 OffsetChrCtrl = 0x58;
constexpr u32 OffsetCtrlOwner = 0x08;
constexpr u32 OffsetCtrlWorld = 0x90; // W: three float4 rows {R row i, t_i}
constexpr u32 OffsetCtrlFreeMove = 0x245;
// The module bag ([PlayerIns+0x3B0]): the action flags (+0x08, flags +0x10: 0x01 = an idle state,
// HKS env 17; 0x40 = an item in use) and the physics module (+0x68, its owner at +0x08): his
// orientation as angles at +0x1D0 (pitch, yaw + pi, 0, 0; the pad manipulator turns them toward the
// lock point, 0x1526D35: atan2(dx, dz) + pi).
constexpr u32 OffsetChrModules = 0x3B0;
constexpr u32 OffsetActionFlagModule = 0x08;
constexpr u32 OffsetActionFlags = 0x10;
constexpr u8 FlagIdle = 0x01;
// While +0xF8 is set the game's own bullets head for the lock point (0x18C7800): a Molotov would be
// lobbed toward the point 50 m ahead (9.5 deg up instead of 2.5). Using an item (a throw, a hunter
// tool) only the strafe mode is written, so his own throws fly as unlocked.
constexpr u8 FlagItemUse = 0x40;
constexpr u32 OffsetPhysicsModule = 0x68;
constexpr u32 OffsetModuleOwner = 0x08;
constexpr u32 OffsetPhysicsAngles = 0x1D0;
constexpr u64 TurnAnglesRva = 0x1526D08; // call atan2f; vaddss xmm0, [pi]; ...; vsubps [rbx+0x1D0]
constexpr std::array<u8, 52> TurnAnglesSig = {
    0xE8, 0x8B, 0x79, 0x69, 0x01,                   // call atan2f
    0xC5, 0xFA, 0x58, 0x05, 0x93, 0xF7, 0x3F, 0x03, // vaddss xmm0, [pi]
    0xC5, 0xF8, 0x28, 0x8D, 0x90, 0xFE, 0xFF, 0xFF, // vmovaps xmm1, [rbp-0x170]
    0xC4, 0xE3, 0x71, 0x21, 0xC0, 0x10,             // vinsertps xmm0, xmm1, xmm0, 0x10
    0xC5, 0xF0, 0x57, 0xC9,                         // vxorps xmm1, xmm1, xmm1
    0xC5, 0x78, 0x28, 0x25, 0xB1, 0xF2, 0x3C, 0x03, // vmovaps xmm12, [mask]
    0xC4, 0xE3, 0x79, 0x4A, 0xC1, 0xC0,             // vblendvps xmm0, xmm0, xmm1, xmm12
    0xC5, 0xF8, 0x5C, 0x8B, 0xD0, 0x01, 0x00,       // vsubps xmm1, xmm0, [rbx+0x1D0]
};

constexpr s64 Ms = 1'000'000;
constexpr float Pi = 3.14159265f;

enum class Mode { Point, Strafe, Probe };

// VR_FACING_MODE=point (default: the lock point ahead along the VR heading and the strafe mode),
// strafe (only ChrCtrl+0x245: the game's own turn target), probe (log only, no writes).
Mode FacingMode() {
    static const Mode mode = [] {
        const char* value = std::getenv("VR_FACING_MODE");
        const std::string text = value != nullptr ? value : "point";
        if (text == "strafe") {
            return Mode::Strafe;
        }
        if (text == "probe") {
            return Mode::Probe;
        }
        return Mode::Point;
    }();
    return mode;
}

const char* ModeName(Mode mode) {
    switch (mode) {
    case Mode::Point:
        return "point";
    case Mode::Strafe:
        return "strafe";
    case Mode::Probe:
        return "probe";
    }
    return "?";
}

// VR_FACING_LOG=1: a line every 0.5 s and every change of state.
bool LogOn() {
    static const bool enabled = std::getenv("VR_FACING_LOG") != nullptr;
    return enabled;
}

u8* g_base = nullptr;
bool g_installed = false;
bool g_idle_turn = false; // the physics module's angles were matched (TurnAnglesSig)
std::unique_ptr<Xbyak::CodeGenerator> g_code;
std::atomic_bool g_active{false};
std::atomic<s64> g_hold_until{0}; // HoldGameFacing
std::atomic_flag g_busy;
std::mutex g_reading_mutex;
FacingReading g_reading{}; // LatestFacing

struct State {
    bool was_active{false};
    bool was_locked{false};
    const char* last_why{nullptr};
    u64 updates{0};
    u64 writes{0};
    u64 activations{0};
    s64 log_ns{0};
    float max_error{0.0f}; // |heading - his yaw| while active, since the last line
    u64 idle_turns{0};     // frames turned directly while idle
    u64 item_frames{0};    // frames of an item in use (no lock point)
    float probe_yaw{0.0f}; // logs: the physics yaw - pi
    bool probe_ok{false};
};

State& S() {
    static State s;
    return s;
}

float Deg(float radians) {
    return radians * 57.2957795f;
}

u8* MainPlayer() {
    u8* const world = Read<u8*>(g_base + WorldChrManRva);
    u8* const chr = Plausible(world) ? Read<u8*>(world + OffsetMainPlayer) : nullptr;
    return Plausible(chr) ? chr : nullptr;
}

VR::Interactions::NativeFacingRuntime g_facing;
VR::Interactions::Module* g_facing_module{};
bool g_facing_fault{};
void InitFacingDll() {
    if (!VR::Interactions::DllRequested() || g_facing_module)
        return;
    // Calls remain serialized by g_busy. Installed hooks retain the module through exit.
    g_facing_module = new VR::Interactions::Module();
    g_facing_module->EnableFaultBroadcast();
    g_facing.Bind(*g_facing_module, g_facing_fault);
    if (!g_facing_module->LoadBesideExecutable()) {
        g_facing_fault = true;
        g_facing_module->ReportFault();
        LOG_ERROR(Core, "VR facing: interaction DLL unavailable: {}",
                  g_facing_module->Diagnostic());
    }
}
void CopyStick(PiFacingFacts& facts) {
    auto* const pads = Common::Singleton<Input::GameControllers>::Instance();
    if (!pads)
        return;
    const auto state = (*pads)[0]->ReadState();
    facts.have_pad = 1;
    facts.pad_x = state.axes[static_cast<size_t>(Input::Axis::LeftX)];
    facts.pad_y = state.axes[static_cast<size_t>(Input::Axis::LeftY)];
}
const char* FacingReason(uint32_t reason, const char* body) {
    switch (reason) {
    case PI_FB_NONE:
        return nullptr;
    case PI_FB_DISABLED:
        return "body_facing=0";
    case PI_FB_TUNE:
        return "facing.off (tune)";
    case PI_FB_ROLL:
        return "the A+B roll";
    case PI_FB_FRAME:
        return "W is not a rotation";
    case PI_FB_THIRD:
        return "third person";
    case PI_FB_ANCHOR:
        return "no first-person view";
    case PI_FB_CINEMATIC:
        return "cinematic flat";
    case PI_FB_DISPLAY:
        return "virtual screen";
    case PI_FB_STATUS:
        return "no body status";
    case PI_FB_BODY:
        return body;
    default:
        return "interaction DLL fault";
    }
}

void LogLine(State& s, const char* what, float chr_yaw, float want, const B::Anchor& anchor,
             bool locked, bool moving, const char* why) {
    LOG_INFO(Core,
             "VR facing: {} | his yaw {:.1f} heading {:.1f} view {:.1f} (off {:.1f}) deg{}{} | "
             "locked {} moving {} | updates {} writes {} idle turns {} item frames {} max off "
             "{:.1f} deg | physics yaw-pi {}{}",
             what, Deg(chr_yaw), Deg(want), Deg(anchor.view_yaw),
             Deg(B::FacingWrap(anchor.view_yaw - want)), g_facing.Catching() ? " catching" : "",
             why != nullptr ? fmt::format(" | game's own: {}", why) : std::string{}, locked ? 1 : 0,
             moving ? 1 : 0, s.updates, s.writes, s.idle_turns, s.item_frames, Deg(s.max_error),
             s.probe_ok ? fmt::format("{:.1f}", Deg(s.probe_yaw)) : std::string{"-"},
             FacingMode() != Mode::Point ? fmt::format(" mode {}", ModeName(FacingMode()))
                                         : std::string{});
    s.max_error = 0.0f;
}

// The module at `offset` of his module bag, when it names him as its owner.
u8* OwnModule(u8* chr, u32 offset) {
    u8* const mods = Read<u8*>(chr + OffsetChrModules);
    u8* const module = Plausible(mods) ? Read<u8*>(mods + offset) : nullptr;
    return Plausible(module) && Read<const u8*>(module + OffsetModuleOwner) == chr ? module
                                                                                   : nullptr;
}

// Right after LockTgtMan::Update (the game's lock-on state for this frame is written).
void PS4_SYSV_ABI OnLockUpdated() {
    if (g_busy.test_and_set(std::memory_order_acquire)) {
        return;
    }
    auto& s = S();
    const s64 now = B::SteadyNowNs();
    ++s.updates;
    u8* const chr = MainPlayer();
    u8* const ctrl = chr != nullptr ? Read<u8*>(chr + OffsetChrCtrl) : nullptr;
    if (chr == nullptr || !Plausible(ctrl) || Read<const u8*>(ctrl + OffsetCtrlOwner) != chr) {
        g_active.store(false, std::memory_order_relaxed);
        s.was_active = false;
        g_busy.clear(std::memory_order_release);
        return;
    }
    PiFacingFacts facts{};
    std::memcpy(facts.world_rows, ctrl + OffsetCtrlWorld, sizeof(facts.world_rows));
    const bool locked = Read<u8>(chr + OffsetLockChosen) != 0;
    facts.locked = locked;
    B::Anchor anchor{};
    facts.have_anchor = B::LatestAnchor(anchor);
    facts.anchor_valid = anchor.valid;
    facts.anchor_ns = anchor.steady_ns;
    facts.base_yaw = anchor.base_yaw;
    facts.view_yaw = anchor.view_yaw;
    facts.world_scale = anchor.world_scale;
    const auto& settings = B::GetBodySettings();
    facts.enabled = settings.body_facing;
    facts.deadzone_deg = settings.body_facing_deadzone_deg;
    facts.moving_deg = settings.body_facing_moving_deg;
    facts.follow_ms = settings.body_facing_ms;
    facts.tune_off = VR::Tune::Get("facing.off", 0.0f) != 0.0f;
    facts.strafe_only = VR::Tune::Get("facing.strafe_only", 0.0f) != 0.0f;
    facts.hold_until_ns = g_hold_until.load(std::memory_order_relaxed);
    facts.first_person = VR::IsFirstPerson();
    facts.cinematic_flat = VR::CinematicFlat();
    facts.auto_display = VR::GetDisplayMode() == VR::DisplayMode::Auto;
    B::BodyStatus status{};
    facts.have_body = B::LatestBodyStatus(status);
    facts.body_ns = status.steady_ns;
    facts.body_blocked = status.facing_block != nullptr;
    facts.mode = static_cast<uint32_t>(FacingMode());
    facts.idle_turn_enabled = g_idle_turn;
    CopyStick(facts);
    u8* const flags = OwnModule(chr, OffsetActionFlagModule);
    if (flags) {
        const u8 bits = Read<u8>(flags + OffsetActionFlags);
        facts.idle = (bits & FlagIdle) != 0;
        facts.item_use = (bits & FlagItemUse) != 0;
    }
    u8* const physics = OwnModule(chr, OffsetPhysicsModule);
    facts.have_physics = physics != nullptr;
    if (physics) {
        facts.physics_yaw = Read<float>(physics + OffsetPhysicsAngles + 4);
        s.probe_yaw = B::FacingWrap(facts.physics_yaw - Pi);
        s.probe_ok = true;
    }
    const auto decision = g_facing.Step(facts, now);
    const char* why = FacingReason(decision.reason, status.facing_block);
    const bool moving = decision.moving, active = decision.active;
    const float chr_yaw = decision.chr_yaw, want = decision.want;
    if (decision.write_point) {
        const std::array<float, 4> point{decision.point[0], decision.point[1], decision.point[2],
                                         1.0f};
        std::memcpy(chr + OffsetLockPoint, point.data(), sizeof(point));
        chr[OffsetLockChosen] = 1;
    }
    if (decision.write_strafe) {
        ctrl[OffsetCtrlFreeMove] = 0;
        ++s.writes;
    }
    if (decision.write_yaw) {
        std::memcpy(physics + OffsetPhysicsAngles + 4, &decision.physics_yaw,
                    sizeof(decision.physics_yaw));
        ++s.idle_turns;
    }
    s.item_frames += active && facts.item_use ? 1 : 0;
    if (decision.admitted)
        s.max_error = std::max(s.max_error, decision.error);
    g_active.store(active, std::memory_order_relaxed);
    {
        // The stick's reference is the view's yaw while the first-person VR view gives the game
        // camera its orientation (core/vr/bloodborne_camera.cpp), else the game's own camera.
        std::scoped_lock lock{g_reading_mutex};
        g_reading.valid = decision.view_ok;
        g_reading.heading = active;
        g_reading.hunter_yaw = active ? want : chr_yaw;
        g_reading.stick_yaw = anchor.view_yaw;
        g_reading.steady_ns = now;
    }
    if (active && !s.was_active) {
        ++s.activations;
    }
    const bool edge = active != s.was_active || locked != s.was_locked ||
                      (why != s.last_why && (why == nullptr || s.last_why == nullptr ||
                                             std::strcmp(why, s.last_why) != 0));
    if (edge && (LogOn() || s.activations <= 3)) {
        LogLine(s, active ? "VR heading drives him" : "the game's facing", chr_yaw, want, anchor,
                locked, moving, why);
    } else if (LogOn() && now - s.log_ns >= 500 * Ms) {
        s.log_ns = now;
        LogLine(s, active ? "on" : "off", chr_yaw, want, anchor, locked, moving, why);
    }
    s.was_active = active;
    s.was_locked = locked;
    s.last_why = why;
    g_busy.clear(std::memory_order_release);
}

// setup; mov r11, LockTgtMan::Update; call r11; mov rax, OnLockUpdated; call rax; jmp back. At
// the site rsp is 16-byte aligned for the call (it is reached by a jump), so it is for both calls;
// after the original returns only callee-saved registers are live in the caller.
void* EmitSite(Xbyak::CodeGenerator& c, const CallSite& site, u64 callee, u64 back) {
    using namespace Xbyak::util;
    c.align(16);
    void* const entry = const_cast<u8*>(c.getCurr());
    for (size_t i = 0; i < site.setup; ++i) {
        c.db(site.bytes[i]);
    }
    c.mov(r11, callee);
    c.call(r11);
    c.mov(rax, reinterpret_cast<u64>(&OnLockUpdated));
    c.call(rax);
    c.db(0xFF); // jmp qword [rip+0]; dq back
    c.db(0x25);
    c.dd(0);
    c.dq(back);
    return entry;
}

} // Anonymous namespace

void InstallFacing(VAddr base, u64 size) {
    if (g_installed || base == 0) {
        return;
    }
    auto* const module = reinterpret_cast<u8*>(base);
    g_base = module;
    const auto& b = B::GetBodySettings();
    if (!b.body_facing) {
        LOG_INFO(Core, "VR facing: off (vr.ini body_facing=0): the game turns the hunter as in the "
                       "flat game");
        return;
    }
    if (size < WorldChrManRva + sizeof(u64)) {
        LOG_INFO(Core, "VR facing: eboot too small for Bloodborne 1.09; off");
        return;
    }
    const auto matches = [module](u64 rva, const auto& sig, size_t length) {
        return std::memcmp(module + rva, sig.data(), length) == 0;
    };
    std::string bad;
    if (!matches(LockUpdateRva, LockUpdateSig, LockUpdateSig.size())) {
        bad += " LockTgtMan::Update";
    }
    for (const CallSite& site : CallSites) {
        if (!matches(site.rva, site.bytes, site.setup + 5)) {
            bad += fmt::format(" call site {:#x}", site.rva);
        }
    }
    if (!matches(LockedBranchRva, LockedBranchSig, LockedBranchSig.size()) ||
        !matches(LockPointStoreRva, LockPointStoreSig, LockPointStoreSig.size())) {
        bad += " lock writes";
    }
    if (!matches(EnvLockRva, EnvLockSig, EnvLockSig.size())) {
        bad += " HKS env 18";
    }
    if (!matches(TurnToPointRva, TurnToPointSig, TurnToPointSig.size())) {
        bad += " turn to the lock point";
    }
    // The idle turn (his orientation's angles) only with its own match; the rest works without.
    g_idle_turn = matches(TurnAnglesRva, TurnAnglesSig, TurnAnglesSig.size()) &&
                  std::getenv("VR_FACING_NO_IDLE_TURN") == nullptr;
    if (!bad.empty()) {
        LOG_WARNING(Core,
                    "VR facing: signature MISMATCH ({}); the game turns the hunter as in "
                    "the flat game",
                    bad.substr(1));
        return;
    }
    for (const CallSite& site : CallSites) {
        if (!Hook::MakeWritable(module + site.rva, site.setup + 5)) {
            LOG_ERROR(Core, "VR facing: could not make {:#x} writable; off", site.rva);
            return;
        }
    }
    try {
        g_code = std::make_unique<Xbyak::CodeGenerator>(512);
        std::array<void*, CallSites.size()> stubs{};
        for (size_t i = 0; i < CallSites.size(); ++i) {
            const CallSite& site = CallSites[i];
            stubs[i] = EmitSite(*g_code, site, reinterpret_cast<u64>(module + LockUpdateRva),
                                reinterpret_cast<u64>(module + site.rva + site.setup + 5));
        }
        InitFacingDll();
        for (size_t i = 0; i < CallSites.size(); ++i) {
            Hook::WriteJump(module + CallSites[i].rva, CallSites[i].setup + 5, stubs[i]);
        }
    } catch (const Xbyak::Error& e) {
        LOG_ERROR(Core, "VR facing: failed to generate the call-site stubs ({}); off", e.what());
        return;
    }
    g_installed = true;
    LOG_INFO(Core,
             "VR facing: installed after LockTgtMan::Update at {:#x} and {:#x} (deadzone {:.0f} "
             "deg, {:.0f} while moving, follow {:.0f} ms, idle turn {}, mode {}{})",
             CallSites[0].rva, CallSites[1].rva, b.body_facing_deadzone_deg,
             b.body_facing_moving_deg, b.body_facing_ms, g_idle_turn ? "on" : "off (no match)",
             ModeName(FacingMode()), LogOn() ? ", log" : "");
}

bool FacingActive() {
    return g_active.load(std::memory_order_relaxed);
}

void HoldGameFacing(s64 until_ns) {
    g_hold_until.store(until_ns, std::memory_order_relaxed);
}

FacingReading LatestFacing() {
    std::scoped_lock lock{g_reading_mutex};
    return g_reading;
}

} // namespace VR::Bloodborne
