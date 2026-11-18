// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include "common/logging/log.h"
#include "core/vr/body_channel.h"
#include "core/vr/bloodborne_deflect.h"
#include "core/vr/bloodborne_layout.h"
#include "core/vr/combat_channel.h"
#include "core/vr/hook_util.h"
#include "core/vr/melee_settings.h"
#include "core/vr/trick_weapons.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

using Hook::Plausible;
using Hook::Read;
namespace D = VR::Deflect;
namespace B = VR::Body;

constexpr s64 Ms = 1'000'000;

constexpr u64 WorldChrManRva = 0x553E878;
constexpr u32 OffsetMainPlayer = 0x60;
constexpr u32 OffsetHandle = 0x08;
// A character's handle (kind 1, the hunter's and the enemies' alike) in WorldChrMan, as the hit
// resolve reads it (0x18647B8): its block (handle >> 14 & 0x3F) below [[mgr+0x08]+0x18] + 2, the
// block's chr set [mgr + 0x850 + 8 x block] {s32 count, +0x08 entries of 0x38 bytes}, the entry
// (handle & 0x3FFF) starting with the ChrIns.
constexpr u32 HandleKindChr = 1;
constexpr u32 OffsetMgrBlocks = 0x08;
constexpr u32 OffsetBlocksCount = 0x18;
constexpr u32 OffsetMgrChrSets = 0x850;
constexpr u32 OffsetSetEntries = 0x08;
constexpr u32 ChrEntrySize = 0x38;

// Projectile P (as bloodborne_quickitems.cpp): its pool handle, position, orientation (hkQuaternion
// x, y, z, w), velocity and the one the ballistic update read last (units / s), the mode (1..3)
// and its objects (+0x08 the bullet id, +0x10 its BulletParam row), the bullet id, the owner's
// handle, the alive flag and the mode's life left (s).
constexpr u32 OffsetPHandle = 0x08;
constexpr u32 OffsetPPos = 0x10;
constexpr u32 OffsetPQuat = 0x30;
constexpr u32 OffsetPVel = 0x40;
constexpr u32 OffsetPPrevVel = 0x50;
constexpr u32 OffsetPMode = 0x2E0;
constexpr u32 OffsetPBullet = 0x2F4;
constexpr u32 OffsetPOwner = 0x2FC;
constexpr u32 OffsetPFlags = 0x7F1; // bit 0: alive
constexpr u32 OffsetPLife = 0x734;
constexpr std::array<u32, 3> OffsetPModes = {0x7F8, 0x820, 0x848};
constexpr u32 OffsetModeRow = 0x10;
// BulletParam row: gravity in range (units / s^2, down), the speed limits the ballistic update
// clamps to (0x19FA34D), the hit radius.
constexpr u32 RowGravityIn = 0x1C;
constexpr u32 RowMaxSpeed = 0x34;
constexpr u32 RowHitRadius = 0x44;
// A handle's kind is its top nibble; a bullet's attack resolves its owner through P+0x2FC while
// the kind is 3 (0x185DE30), so an owner must be another kind.
constexpr u32 HandleKindBullet = 3;

// The deflection's clink: the game's PlaySound (TAE event 128's call, as bloodborne_melee.cpp),
// sword-x-sword (blade_sounds.h's tap), c-prefixed (SoundType 1) as the blade sounds play it: the
// animation sound-type flag's g namespace has no g000009000.
constexpr u64 PlaySoundRva = 0x13E0290;
constexpr std::array<u8, 24> PlaySoundSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48, 0x81, 0xEC,
    0x28, 0x01, 0x00, 0x00, 0x49, 0x89, 0xF6, 0x48, 0x89, 0xFB, 0x4C, 0x8B,
};
constexpr s32 ClinkSound = 9000;
// A blast sent back rings too: iron-cut-iron (blade_sounds.h's other tap), over the clink.
constexpr s32 ReturnSound = 935;
using PlaySoundFn = void(PS4_SYSV_ABI*)(u64* handle, const float* pos, s32 type, s32 id);

constexpr s64 BladeMaxAgeNs = 100 * Ms;  // an older blade (the body stopped): nothing deflects
constexpr s64 TrackNs = 15'000 * Ms;     // a bullet is followed this long at most
constexpr float StrikeSpeedMps = 6.0f;   // the weapon wall's strike for a deflection (a hard one)
constexpr float SnapLiftMax = 2.0f;      // the drop compensation never raises the aim more (units)
// The height his gun fired from over the shooter's feet (0.2 .. 5 units): a bullet sent back aims
// at a share of it where he is now (ReturnAimShare). A shooter no longer found (dead, unloaded) is
// never sent one; any other bounce keeps clear of him there, or of his muzzle at the launch.
constexpr float GunHeightMin = 0.2f;
constexpr float GunHeightMax = 5.0f;
// A bullet sent back heads for the shooter's body (this share of his gun's height over his feet)
// and is steered there at most HomeTurnDegS a second, until it is within HomeNear of him (the
// game's own hit takes it there), he is behind it (D::Steer) or HomeMaxNs have passed. Its blast's
// damage on him counts until ReturnGraceNs after its flight back should be over.
constexpr float ReturnAimShare = 0.8f;
constexpr float HomeTurnDegS = 360.0f;
constexpr float HomeNear = 1.0f;
constexpr s64 HomeMaxNs = 3'000 * Ms;
constexpr s64 ReturnGraceNs = 500 * Ms;
constexpr float Deg = 0.0174532925f;
// A character's frame W ([[chr+0x58]+0x90], three float4 rows {R row i | t_i}): the shooter's
// feet, and VR_DEFLECT_TEST's wall in front of the hunter's chest.
constexpr u32 OffsetChrCtrl = 0x58;
constexpr u32 OffsetCtrlWorld = 0x90;

// The same proofs as physical melee's stamina/TAE-225 paths. Unknown game code never
// authorizes a stamina write (or a free passive block when it should have cost stamina).
constexpr u64 StaminaRva = 0x1A297CB;
constexpr std::array<u8, 37> StaminaSig = {
    0x49, 0x8B, 0x45, 0x08, 0x48, 0x8B, 0x80, 0xB0, 0x03, 0x00, 0x00, 0x4C, 0x8B,
    0x78, 0x20, 0x41, 0x8B, 0x9F, 0x34, 0x01, 0x00, 0x00, 0x45, 0x85, 0xE4, 0x7E,
    0x44, 0x41, 0xF6, 0x87, 0x00, 0x02, 0x00, 0x00, 0x80, 0x75, 0x5C,
};
constexpr u64 RegenRva = 0x1A22200;
constexpr std::array<u8, 15> RegenSig = {
    0x48, 0x8B, 0x40, 0x58, 0x48, 0x8B, 0x40, 0x38, 0x41, 0x8A, 0x0C, 0x24, 0x88, 0x48, 0x34,
};
constexpr u32 OffsetHp = 0xF8, OffsetStamina = 0x134, OffsetMaxStamina = 0x138;
constexpr u32 OffsetDataFlags = 0x200, OffsetRegenPercent = 0x34;

struct Tracked {
    u8* p{nullptr};
    u32 handle{0};
    u32 owner{0};
    s32 bullet{-1};
    s64 ns{0};
    std::array<float, 3> from{}; // the muzzle at the launch (the shooter's line runs through it)
    float gun_h{-1.0f};          // ... its height over the shooter's feet (< 0: not found)
    // Sent back at the shooter and steered at him on its way (Home); the bullet's owner is now
    // the hunter's, `owner` stays the shooter's.
    bool returning{false};
    s64 back_ns{0};
};

std::array<Tracked, 32> g_tracked{};
std::mutex g_tracked_mutex; // also g_returns, g_rolls, g_rng

// A blast sent back at its shooter (its pellets: one shooter, one bullet type, launched within
// 10 ms): the first damage one of them does him is the return's hit, felt in the hand; none by
// `until`, a miss. Logged when it ends (EndReturns).
struct Return {
    u32 shooter{0};
    const u8* chr{nullptr}; // his ChrIns when it was sent back (the damage hook's target)
    s32 bullet{-1};
    s64 launch{0};
    s64 until{0}; // 0: a free slot
    int sent{0};
    int hits{0};
    float damage{0.0f};
    float chance{0.0f}; // its swing's chance
};
std::array<Return, 8> g_returns{};
std::atomic<bool> g_returns_live{false};
D::Rolls g_rolls;
u64 g_rng = 0x9E3779B97F4A7C15ull;

struct BladeState {
    D::Blade was{};
    D::Blade now{};
    float dt{0.0f};
    float scale{1.0f};
    s64 ns{0};
    bool have{false};
};
BladeState g_blade; // Game:Main (the body and the render tick)

DeflectStrike g_strike{};
bool g_strike_pending = false;

u8* g_base = nullptr;
bool g_ready = false;
PlaySoundFn g_play_sound = nullptr;
int g_log = 0;       // VR_DEFLECT_LOG: 1 the deflections, 2 every enemy launch too
// VR_DEFLECT_TEST: every enemy bullet within 1.5 m deflects (no blade); 2: off a wall turned
// 15 degrees, as a swing's (its bounce 30 degrees off the shooter: a swing's return's chance).
int g_test = 0;
u32 g_count = 0;
bool g_guard_stamina = false, g_guard_regen = false;
D::GuardCharges g_guard;
const u8* g_guard_player = nullptr;
u32 g_guard_hunter = 0;
// Payment runs on Game:Main; the pose-copy worker refreshes regen. Publish the character's
// identity before its expiry, including the handle in case a later load reuses the address.
std::atomic<const u8*> g_regen_player{nullptr};
std::atomic<u32> g_regen_hunter{0};
std::atomic<s64> g_regen_until{0};

std::array<float, 3> Read3(const u8* p) {
    return {Read<float>(p), Read<float>(p + 4), Read<float>(p + 8)};
}

B::Vec3 V(const std::array<float, 3>& a) {
    return {a[0], a[1], a[2]};
}

void Write4(u8* p, B::Vec3 v, float w) {
    const std::array<float, 4> out{v.x, v.y, v.z, w};
    std::memcpy(p, out.data(), sizeof(out));
}

u8* Player() {
    if (g_base == nullptr) {
        return nullptr;
    }
    u8* const man = Read<u8*>(g_base + WorldChrManRva);
    if (!Plausible(man)) {
        return nullptr;
    }
    u8* const player = Read<u8*>(man + OffsetMainPlayer);
    return Plausible(player) ? player : nullptr;
}

// Called only after contact, on Game:Main. Read the authoritative pool again for each paid
// impact, so two shooters in one update cannot both spend the same cached stamina.
int PayGuard(u8* player, u32 hunter, const Tracked& t, s64 now) {
    const int cost =
        g_guard.Cost(t.owner, t.bullet, t.ns, now, VR::Melee::GetMeleeSettings().deflect_stamina);
    if (cost == 0) {
        return 0;
    }
    if (!g_guard_stamina || !g_guard_regen) {
        return -1;
    }
    u8* const modules = Read<u8*>(player + Layout::OffsetChrModules);
    u8* const data = Plausible(modules) ? Read<u8*>(modules + Layout::OffsetDataModule) : nullptr;
    if (!Plausible(data) || Read<s32>(data + OffsetHp) <= 0) {
        return -1;
    }
    if ((Read<u8>(data + OffsetDataFlags) & 0x80) != 0) {
        return 0; // game's infinite-stamina flag
    }
    int stamina = Read<s32>(data + OffsetStamina);
    if (!D::SpendGuard(stamina, Read<s32>(data + OffsetMaxStamina), cost)) {
        return -1;
    }
    std::memcpy(data + OffsetStamina, &stamina, sizeof(stamina));
    g_guard.Paid(t.owner, t.bullet, t.ns, now);
    g_regen_player.store(player, std::memory_order_relaxed);
    g_regen_hunter.store(hunter, std::memory_order_relaxed);
    g_regen_until.store(now + D::GuardRegenNs, std::memory_order_release);
    return cost;
}

// The character with this handle (null: none, or not a character's).
const u8* ChrByHandle(u32 handle) {
    if (g_base == nullptr || (handle >> 28) != HandleKindChr) {
        return nullptr;
    }
    const u8* const mgr = Read<const u8*>(g_base + WorldChrManRva);
    const u8* const blocks = Plausible(mgr) ? Read<const u8*>(mgr + OffsetMgrBlocks) : nullptr;
    if (!Plausible(blocks)) {
        return nullptr;
    }
    const u32 block = (handle >> 14) & 0x3F;
    if (static_cast<s32>(block) >= Read<s32>(blocks + OffsetBlocksCount) + 2) {
        return nullptr;
    }
    const u8* const set = Read<const u8*>(mgr + OffsetMgrChrSets + 8 * block);
    const u32 slot = handle & 0x3FFF;
    if (!Plausible(set) || static_cast<s32>(slot) >= Read<s32>(set)) {
        return nullptr;
    }
    const u8* const entries = Read<const u8*>(set + OffsetSetEntries);
    const u8* const chr =
        Plausible(entries) ? Read<const u8*>(entries + ChrEntrySize * slot) : nullptr;
    return Plausible(chr) && Read<u32>(chr + OffsetHandle) == handle ? chr : nullptr;
}

// The character's feet (its frame W's translation); false when unreadable.
bool FeetOf(const u8* chr, B::Vec3& out) {
    const u8* const ctrl = chr != nullptr ? Read<const u8*>(chr + OffsetChrCtrl) : nullptr;
    if (!Plausible(ctrl)) {
        return false;
    }
    const u8* const w = ctrl + OffsetCtrlWorld;
    out = {Read<float>(w + 0x0C), Read<float>(w + 0x1C), Read<float>(w + 0x2C)};
    return B::IsFinite(out);
}

const u8* RowOf(const u8* p) {
    const s32 mode = Read<s32>(p + OffsetPMode);
    if (mode < 1 || mode > 3) {
        return nullptr;
    }
    const u8* const row =
        Read<const u8*>(p + OffsetPModes[static_cast<size_t>(mode - 1)] + OffsetModeRow);
    return Plausible(row, 4) ? row : nullptr;
}

// The game's sound `id` (SoundType c) at `at`.
void Play(B::Vec3 at, s32 id) {
    if (g_play_sound == nullptr) {
        return;
    }
    alignas(16) std::array<float, 4> p{at.x, at.y, at.z, 1.0f};
    u64 handle = 0;
    constexpr s32 type = 1;
    g_play_sound(&handle, p.data(), type, id);
}

// 0 .. 1 (xorshift64*; g_tracked_mutex).
float Random01() {
    g_rng ^= g_rng >> 12;
    g_rng ^= g_rng << 25;
    g_rng ^= g_rng >> 27;
    return static_cast<float>((g_rng * 0x2545F4914F6CDD1Dull) >> 40) * (1.0f / 16777216.0f);
}

// Turn the bullet's line onto `dir` (unit) at `speed`: its orientation by the same turn (the game
// draws and falls back to it) and the velocity it keeps (the ballistic update integrates its own
// velocity and clamps the speed).
void Turn(u8* p, B::Vec3 dir, float speed) {
    const B::Vec3 v_old = V(Read3(p + OffsetPVel));
    const B::Quat q_old{Read<float>(p + OffsetPQuat), Read<float>(p + OffsetPQuat + 4),
                        Read<float>(p + OffsetPQuat + 8), Read<float>(p + OffsetPQuat + 12)};
    const B::Quat q_new =
        B::Normalize(B::FromTo(B::Normalize(v_old, dir), dir) * B::Normalize(q_old));
    const std::array<float, 4> q{q_new.x, q_new.y, q_new.z, q_new.w};
    std::memcpy(p + OffsetPQuat, q.data(), sizeof(q));
    Write4(p + OffsetPVel, dir * speed, 0.0f);
    Write4(p + OffsetPPrevVel, dir * speed, 0.0f);
}

// Where a bullet sent back heads: the shooter's body where he is now (ReturnAimShare of his gun's
// height over his feet). False: he is not found (dead, unloaded) or his gun's height was not.
bool ShooterAim(const Tracked& t, B::Vec3& out) {
    B::Vec3 feet{};
    if (t.gun_h < 0.0f || !FeetOf(ChrByHandle(t.owner), feet)) {
        return false;
    }
    out = feet + B::Vec3{0.0f, t.gun_h * ReturnAimShare, 0.0f};
    return true;
}

// The aim point a bullet at `from` flying `speed` and falling `gravity` takes to reach `target`:
// raised by its drop, never more than SnapLiftMax.
B::Vec3 AimAt(B::Vec3 from, B::Vec3 target, float speed, float gravity) {
    B::Vec3 aim = D::AimFor(from, target, speed, gravity);
    aim.y = std::min(aim.y, target.y + SnapLiftMax);
    return aim;
}

// A bullet sent back, on its way: turned toward its shooter where he is now. False: no longer
// followed (he is gone, it is at him or past him, or it has flown long enough); it flies on.
bool Home(u8* p, const Tracked& t, const u8* row, s64 now, float dt) {
    B::Vec3 target{};
    if (now - t.back_ns > HomeMaxNs || !ShooterAim(t, target)) {
        return false;
    }
    const B::Vec3 at = V(Read3(p + OffsetPPos));
    const B::Vec3 v = V(Read3(p + OffsetPVel));
    const float speed = B::Length(v);
    if (!(speed > 1.0f) || !(B::Length(target - at) > HomeNear)) {
        return false;
    }
    const float gravity = std::max(Read<float>(row + RowGravityIn), 0.0f);
    B::Vec3 dir = v * (1.0f / speed);
    const B::Vec3 want = B::Normalize(AimAt(at, target, speed, gravity) - at, dir);
    if (!D::Steer(dir, want, HomeTurnDegS * Deg * dt)) {
        return false;
    }
    Turn(p, dir, speed);
    return true;
}

// A pellet sent back at its shooter: its blast's return (a new one the first time: the ring and
// the hand's crack), counted until its flight back is over.
void NoteReturn(const Tracked& t, B::Vec3 at, float flight_s, float chance, s64 now) {
    const s64 until = now + static_cast<s64>(std::clamp(flight_s, 0.0f, 10.0f) * 1e9f) +
                      ReturnGraceNs;
    for (Return& r : g_returns) {
        const s64 gap = r.launch >= t.ns ? r.launch - t.ns : t.ns - r.launch;
        if (r.until != 0 && r.shooter == t.owner && r.bullet == t.bullet && gap <= 10 * Ms) {
            ++r.sent;
            r.until = std::max(r.until, until);
            return;
        }
    }
    Return* slot = &*std::min_element(g_returns.begin(), g_returns.end(),
                                      [](const Return& a, const Return& b) {
                                          return a.until < b.until;
                                      });
    *slot = {t.owner, ChrByHandle(t.owner), t.bullet, t.ns, until, 1, 0, 0.0f, chance};
    g_returns_live.store(true, std::memory_order_release);
    Play(at, ReturnSound);
    Combat::PushMeleeEvent({Combat::MeleeEventKind::Ricochet, 0, 0.0f, chance, now, ""});
}

// The returns whose time is up: logged (hit or missed) and freed.
void EndReturns(s64 now) {
    bool live = false;
    for (Return& r : g_returns) {
        if (r.until == 0) {
            continue;
        }
        if (now <= r.until) {
            live = true;
            continue;
        }
        if (r.hits > 0) {
            LOG_INFO(Core,
                     "VR deflect: the blast sent back at {:#x} (its chance {:.0f}%) hit him: {} of "
                     "{} pellets, {:.0f} damage",
                     r.shooter, r.chance * 100.0f, r.hits, r.sent, r.damage);
        } else {
            LOG_INFO(Core,
                     "VR deflect: the blast sent back at {:#x} (its chance {:.0f}%) missed: {} "
                     "pellets, no damage on him",
                     r.shooter, r.chance * 100.0f, r.sent);
        }
        r = {};
    }
    g_returns_live.store(live, std::memory_order_release);
}

// Turn the bullet onto `dir` (unit) from `at` (Turn), the life to come back the way it came, and
// it is the hunter's.
void Redirect(u8* p, const Tracked& t, B::Vec3 at, B::Vec3 dir, float speed, u32 hunter, s64 now) {
    Turn(p, dir, speed);
    Write4(p + OffsetPPos, at, Read<float>(p + OffsetPPos + 12));
    const float flown = static_cast<float>(now - t.ns) * 1e-9f;
    const float life = std::max(Read<float>(p + OffsetPLife), flown * 1.5f + 0.5f);
    std::memcpy(p + OffsetPLife, &life, sizeof(life));
    std::memcpy(p + OffsetPOwner, &hunter, sizeof(hunter));
}

} // Anonymous namespace

void InstallDeflect(VAddr base, u64 size) {
    std::scoped_lock lock{g_tracked_mutex};
    g_tracked = {};
    g_blade = {};
    g_guard = {};
    g_guard_player = nullptr;
    g_guard_hunter = 0;
    g_regen_player.store(nullptr, std::memory_order_relaxed);
    g_regen_hunter.store(0, std::memory_order_relaxed);
    g_regen_until.store(0, std::memory_order_release);
    g_strike_pending = false;
    g_count = 0;
    g_returns = {};
    g_returns_live.store(false, std::memory_order_relaxed);
    g_rolls = {};
    g_rng ^= static_cast<u64>(B::SteadyNowNs()) * 0xBF58476D1CE4E5B9ull;
    g_rng = g_rng != 0 ? g_rng : 0x9E3779B97F4A7C15ull;
    g_play_sound = nullptr;
    g_base = reinterpret_cast<u8*>(base);
    const auto& s = VR::Melee::GetMeleeSettings();
    if (const char* value = std::getenv("VR_DEFLECT_LOG"); value != nullptr) {
        g_log = std::clamp(std::atoi(value), 0, 2);
    }
    g_test = 0;
    if (const char* value = std::getenv("VR_DEFLECT_TEST"); value != nullptr) {
        g_test = std::max(std::atoi(value), 1);
    }
    const auto matches = [&](u64 rva, const auto& sig) {
        return size > rva + sig.size() && std::memcmp(g_base + rva, sig.data(), sig.size()) == 0;
    };
    if (matches(PlaySoundRva, PlaySoundSig)) {
        g_play_sound = reinterpret_cast<PlaySoundFn>(g_base + PlaySoundRva);
    }
    g_guard_stamina = matches(StaminaRva, StaminaSig);
    g_guard_regen = matches(RegenRva, RegenSig);
    LOG_INFO(Core,
             "VR deflect: passive blocks cost {} stamina (0.35 s regen pause), swats free; "
             "stamina {}, regen {}",
             s.deflect_stamina, g_guard_stamina ? "ok" : "MISMATCH (paid blocks refused)",
             g_guard_regen ? "ok" : "MISMATCH (paid blocks refused)");
    g_ready = s.deflect;
    // The blade is the solid weapons' (bloodborne_body.cpp SolidWeapons): without them none.
    const bool solid = VR::Tricks::GetTrickSettings().weapon_wall;
    LOG_INFO(Core, "VR deflect: {}{}{}{}{}",
             g_ready ? "on (its bullets from the launch hook: the VR quick line says if it is in)"
                     : "off (melee_deflect=0)",
             g_ready && !solid ? " but weapon_wall=0: no blade to deflect with" : "",
             g_ready ? fmt::format(" (a swing's return: {:.0f}% within {:.0f} deg of the "
                                   "shooter, none from {:.0f} deg; margin {:.0f} cm, at most "
                                   "{:.2f} m wide)",
                                   D::ReturnOdds[0] * 100.0f, s.deflect_snap_deg,
                                   s.deflect_return_deg, s.deflect_margin_cm,
                                   s.deflect_max_radius_m)
                     : std::string{},
             g_play_sound == nullptr ? ", clink MISMATCH" : "",
             std::string{g_log > 0 ? fmt::format(" log={}", g_log) : ""} +
                 (g_test == 1   ? " TEST (no blade: every enemy bullet within 1.5 m)"
                  : g_test >= 2 ? " TEST 2 (no blade: every enemy bullet within 1.5 m, as a "
                                  "swing's, 30 deg off its shooter)"
                                : ""));
}

bool DeflectWanted() {
    return VR::Melee::GetMeleeSettings().deflect;
}

void DeflectLaunch(u8* p) {
    if (!g_ready || p == nullptr || !Plausible(p, 16)) {
        return;
    }
    const u32 owner = Read<u32>(p + OffsetPOwner);
    u8* const player = Player();
    if (owner == 0xFFFFFFFFu || owner == 0 || player == nullptr ||
        owner == Read<u32>(player + OffsetHandle) || (owner >> 28) == HandleKindBullet) {
        return; // the hunter's own, or a child of a bullet (its fire, its burst)
    }
    const u8* const row = RowOf(p);
    const float radius = row != nullptr ? Read<float>(row + RowHitRadius) : -1.0f;
    const auto from = Read3(p + OffsetPPos);
    const B::Vec3 v = V(Read3(p + OffsetPVel));
    const s32 bullet = Read<s32>(p + OffsetPBullet);
    if (g_log >= 2) {
        LOG_INFO(Core,
                 "VR deflect: launch bullet {} mode {} owner {:#x} at ({:.2f},{:.2f},{:.2f}) "
                 "{:.1f} u/s radius {:.2f}{}",
                 bullet, Read<s32>(p + OffsetPMode), owner, from[0], from[1], from[2],
                 B::Length(v), radius,
                 radius > VR::Melee::GetMeleeSettings().deflect_max_radius_m ? " (too wide)" : "");
    }
    if (row == nullptr || !(radius >= 0.0f) ||
        radius > VR::Melee::GetMeleeSettings().deflect_max_radius_m || B::Length(v) < 1.0f) {
        return;
    }
    const s64 now = B::SteadyNowNs();
    std::scoped_lock lock{g_tracked_mutex};
    Tracked* slot = nullptr;
    for (Tracked& t : g_tracked) {
        if (t.p == p || t.p == nullptr) {
            slot = &t;
            break;
        }
    }
    if (slot == nullptr) {
        slot = &*std::min_element(g_tracked.begin(), g_tracked.end(),
                                  [](const Tracked& a, const Tracked& b) { return a.ns < b.ns; });
    }
    B::Vec3 feet{};
    const bool found = FeetOf(ChrByHandle(owner), feet);
    *slot = {p, Read<u32>(p + OffsetPHandle), owner, bullet, now, from,
             found ? std::clamp(from[1] - feet.y, GunHeightMin, GunHeightMax) : -1.0f};
}

void DeflectBlade(const D::Blade& blade, float scale, bool fresh, bool snap, s64 now) {
    if (!g_ready) {
        return;
    }
    BladeState& b = g_blade;
    if (!fresh && b.have && now - b.ns < BladeMaxAgeNs) {
        return; // a stereo pair's second eye: its hands reused
    }
    const bool sweep = b.have && !snap && now - b.ns < BladeMaxAgeNs && b.now.n == blade.n;
    b.was = sweep ? b.now : blade;
    b.dt = sweep ? static_cast<float>(now - b.ns) * 1e-9f : 0.0f;
    b.now = blade;
    b.scale = std::max(scale, 1e-3f);
    b.ns = now;
    b.have = true;
}

void DeflectNoBlade() {
    g_blade.have = false;
}

bool TakeDeflectStrike(DeflectStrike& out) {
    if (!g_strike_pending) {
        return false;
    }
    g_strike_pending = false;
    out = g_strike;
    return true;
}

void DeflectRegenTick(u8* player, s64 now) {
    s64 until = g_regen_until.load(std::memory_order_acquire);
    if (until == 0) {
        return; // normal play: one atomic load, no clock or guest-memory read
    }
    if (now == 0) {
        now = B::SteadyNowNs();
    }
    if (now >= until) {
        g_regen_until.compare_exchange_strong(until, 0, std::memory_order_relaxed);
        return;
    }
    if (player == nullptr || player != g_regen_player.load(std::memory_order_relaxed) ||
        Read<u32>(player + OffsetHandle) != g_regen_hunter.load(std::memory_order_relaxed)) {
        return;
    }
    // The installation proved this exact pointer chain and byte store. Only pending paid
    // blocks reach here; holding a weapon alone neither drains nor pauses regeneration.
    u8* const ctrl = Read<u8*>(player + Layout::OffsetChrCtrl);
    u8* const hks = Plausible(ctrl) ? Read<u8*>(ctrl + Layout::OffsetActionState) : nullptr;
    if (Plausible(hks)) {
        hks[OffsetRegenPercent] = 0;
    }
}

void DeflectTick(s64 now) {
    if (!g_ready) {
        return;
    }
    std::scoped_lock lock{g_tracked_mutex};
    if (g_returns_live.load(std::memory_order_relaxed)) {
        EndReturns(now);
    }
    bool any = false;
    for (const Tracked& t : g_tracked) {
        any = any || t.p != nullptr;
    }
    if (!any) {
        return;
    }
    u8* const player = Player();
    const BladeState& b = g_blade;
    const bool blade = b.have && now - b.ns < BladeMaxAgeNs;
    if (player == nullptr) {
        return;
    }
    const u32 hunter = Read<u32>(player + OffsetHandle);
    if (g_guard_player != player || g_guard_hunter != hunter) {
        g_guard = {};
        g_guard_player = player;
        g_guard_hunter = hunter;
    }
    const auto& s = VR::Melee::GetMeleeSettings();
    const float dt = std::clamp(VR::GameFrameSeconds(), 1.0f / 120.0f, 1.0f / 15.0f);
    const float scale = blade ? b.scale : 1.0f;
    // The same for every bullet: where the blade goes on over a bullet's next step, and the sphere
    // around its whole sweep that every bullet not at the blade misses.
    const float margin = s.deflect_margin_cm * 0.01f * scale;
    D::Blade ahead{};
    D::Bound bound{};
    if (blade) {
        ahead = D::Ahead(b.was, b.now, b.dt, dt);
        bound = D::BoundOf(b.was, b.now, ahead);
    }
    bool swat_known = false;
    bool swat = false;
    u32 swing_serial = 0;
    const auto read_swat = [&] {
        if (!swat_known) {
            Combat::MeleeWindow swing{};
            swat = Combat::LatestMeleeWindow(swing) && D::Swatting(swing, now);
            swing_serial = swat ? swing.serial : 0;
            swat_known = true;
        }
    };
    for (Tracked& t : g_tracked) {
        if (t.p == nullptr) {
            continue;
        }
        u8* const p = t.p;
        const bool alive = now - t.ns < TrackNs && Plausible(p, 16) &&
                           Read<u32>(p + OffsetPHandle) == t.handle &&
                           (Read<u8>(p + OffsetPFlags) & 1) != 0 &&
                           Read<u32>(p + OffsetPOwner) == (t.returning ? hunter : t.owner) &&
                           Read<s32>(p + OffsetPBullet) == t.bullet;
        const u8* const row = alive ? RowOf(p) : nullptr;
        if (row == nullptr) {
            t = {};
            continue;
        }
        if (t.returning) {
            if (!Home(p, t, row, now, dt)) {
                t = {}; // it flies on as it is
            }
            continue;
        }
        if (!blade && !g_test) {
            continue;
        }
        const B::Vec3 at = V(Read3(p + OffsetPPos));
        const B::Vec3 v = V(Read3(p + OffsetPVel));
        const float speed = B::Length(v);
        if (!(speed > 1.0f)) {
            continue;
        }
        const B::Vec3 dir = v * (1.0f / speed);
        // Its next step (a quarter of one back: a step the last update just took past the blade).
        const B::Vec3 p0 = at - v * (0.25f * dt);
        const B::Vec3 p1 = at + v * (1.25f * dt);
        const float rb = std::max(Read<float>(row + RowHitRadius), 0.0f);
        D::Contact c{};
        bool wall = false;
        if (blade && D::Near(bound, p0, p1, rb + margin + D::SwingAssistM * scale)) {
            // Only nearby bullets read the swing channel, once per tick. The broad phase
            // includes assisted hits; passive guards retain their exact old reach.
            read_swat();
            const float contact_margin = margin + (swat ? D::SwingAssistM * scale : 0.0f);
            // Where the blade swept since the last update, and where it goes on its way over the
            // bullet's next step.
            if (swat || D::Near(bound, p0, p1, rb + contact_margin)) {
                c = D::Earlier(D::Touch(p0, p1, rb, contact_margin, b.was, b.now, b.dt),
                               D::Touch(p0, p1, rb, contact_margin, b.now, ahead, dt));
            }
        }
        if (!c.hit && g_test) {
            // VR_DEFLECT_TEST: a wall 1.5 m out from the hunter's chest, facing the bullet.
            if (B::Vec3 root{}; FeetOf(player, root)) {
                const B::Vec3 chest = root + B::Vec3{0.0f, 1.2f, 0.0f};
                const B::Vec3 to = chest - at;
                const float ahead = B::Dot(to, dir);
                if (ahead > 0.0f && ahead < 1.5f + speed * dt &&
                    B::Length(to - dir * ahead) < 1.0f) {
                    c.hit = true;
                    c.point = at;
                    c.normal = -dir;
                    if (g_test >= 2) {
                        // Turned 15 degrees about the world's up: the mirror bounce 30 off.
                        constexpr float Cos15 = 0.96592583f, Sin15 = 0.25881905f;
                        c.normal = B::Normalize(B::Vec3{-dir.x * Cos15 - dir.z * Sin15, -dir.y,
                                                        dir.x * Sin15 - dir.z * Cos15},
                                                -dir);
                    }
                    wall = true;
                }
            }
        }
        if (!c.hit) {
            continue;
        }
        // Also covers the diagnostic wall without a blade; never reread mid-loop.
        read_swat();
        const int spent = wall || swat ? 0 : PayGuard(player, hunter, t, now);
        if (spent < 0) {
            if (g_log >= 1) {
                LOG_INFO(Core, "VR deflect: bullet {} passes (passive block cannot pay stamina)",
                         t.bullet);
            }
            t = {}; // no redirect, ownership change, sparks or later retry of this contact
            continue;
        }
        // The bounce in real metres per second of the blade (the swing turns the bullet).
        B::Vec3 out = D::Bounce(dir, c.normal, c.blade_v * (1.0f / scale));
        const float gravity = std::max(Read<float>(row + RowGravityIn), 0.0f);
        const float max_speed = Read<float>(row + RowMaxSpeed);
        const float fly = max_speed > 1.0f ? std::min(speed, max_speed) : speed;
        B::Vec3 target = V(t.from);
        const bool live = ShooterAim(t, target);
        const B::Vec3 aim = AimAt(c.point, target, fly, gravity);
        const B::Vec3 bounced = out;
        const B::Vec3 at_him = B::Normalize(aim - c.point, bounced);
        const float off_deg = B::Angle(bounced, at_him) * 57.2957795f;
        // Only a swing sends it back (a still blade is a shield): by its chance, the likelier the
        // closer its bounce heads at him, one roll for its whole blast in this swing.
        const bool as_swing = swat || (wall && g_test >= 2);
        const float chance =
            as_swing && live ? D::ReturnChance(off_deg, s.deflect_snap_deg, s.deflect_return_deg)
                             : 0.0f;
        bool returned = false;
        bool shared = false;
        if (chance > 0.0f) {
            shared = g_rolls.Find(t.owner, t.bullet, t.ns, swing_serial, returned);
            if (!shared) {
                returned = Random01() < chance;
                g_rolls.Note(t.owner, t.bullet, t.ns, swing_serial, returned);
            }
        }
        // Any other goes its own way, softer, and never at him.
        out = returned ? at_him
                       : D::AwayFrom(D::Soften(out, dir, c.normal), at_him, s.deflect_snap_deg,
                                     c.normal);
        Redirect(p, t, c.point, out, speed, hunter, now);
        if (returned) {
            // Life for the whole way back; steered at him on its way (Home).
            const float back_s = B::Length(target - c.point) / std::max(speed, 1.0f);
            const float life = std::max(Read<float>(p + OffsetPLife), back_s + 1.0f);
            std::memcpy(p + OffsetPLife, &life, sizeof(life));
            NoteReturn(t, c.point, back_s, chance, now);
            t.returning = true;
            t.back_ns = now;
        }
        ++g_count;
        g_strike = {c.point, c.normal, StrikeSpeedMps};
        g_strike_pending = true;
        Play(c.point, ClinkSound);
        if (g_log >= 1 || g_count <= 3) {
            LOG_INFO(
                Core,
                "VR deflect #{}: bullet {} (owner {:#x}) at ({:.2f},{:.2f},{:.2f}) {:.1f} u/s, "
                "{:.0f} ms after its launch: off the {} (blade {:.1f} m/s), the bounce {:.0f} "
                "deg from the shooter ({}) -> {} ({:.2f},{:.2f},{:.2f}), stamina {}{}",
                g_count, t.bullet, t.owner, c.point.x, c.point.y, c.point.z, speed,
                static_cast<float>(now - t.ns) * 1e-6f, wall ? "test wall" : "blade",
                B::Length(c.blade_v) / scale,
                B::Angle(bounced, B::Normalize(aim - c.point, bounced)) * 57.2957795f,
                live ? fmt::format("where he is, {:.1f} m up", t.gun_h) : "his muzzle",
                returned
                    ? fmt::format("back at the shooter (its chance {:.0f}%{})", chance * 100.0f,
                                  shared ? ", its blast's roll" : "")
                    : fmt::format("its own way{}, {:.0f} deg off him",
                                  chance > 0.0f
                                      ? fmt::format(" (missed its {:.0f}% chance{})",
                                                    chance * 100.0f,
                                                    shared ? ", its blast's roll" : "")
                                      : std::string{},
                                  B::Angle(out, at_him) * 57.2957795f),
                out.x, out.y, out.z, spent,
                std::string{swat ? " (swat)" : ""} + (t.returning ? ", steered at him" : ""));
        }
        if (!t.returning) {
            t = {};
        }
    }
}

void DeflectOnDamage(const u8* target, s32 bullet, float damage) {
    if (!g_returns_live.load(std::memory_order_acquire) || target == nullptr ||
        !(damage > 0.0f)) {
        return;
    }
    std::scoped_lock lock{g_tracked_mutex};
    const s64 now = B::SteadyNowNs();
    for (Return& r : g_returns) {
        // Its shooter, by any bullet of the hunter's: the game's result may name the hit's
        // bullet otherwise than the projectile does (logged when it differs).
        if (r.until == 0 || r.chr != target || now > r.until) {
            continue;
        }
        r.damage += damage;
        if (++r.hits == 1) {
            Combat::PushMeleeEvent({Combat::MeleeEventKind::Ricochet, 0, 1.0f, damage, now, ""});
            LOG_INFO(Core, "VR deflect: a bullet sent back hit its shooter {:#x}: {:.0f} damage{}",
                     r.shooter, damage,
                     bullet != r.bullet ? fmt::format(" (the hit's bullet {}, the blast's {})",
                                                      bullet, r.bullet)
                                        : std::string{});
        }
        return;
    }
}

} // namespace VR::Bloodborne
