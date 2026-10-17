// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <xbyak/xbyak.h>

#include "common/logging/log.h"
#include "common/thread.h"
#include "core/vr/bloodborne_cinematic.h"
#include "core/vr/bloodborne_enemy_bars.h"
#include "core/vr/bloodborne_givekit.h"
#include "core/vr/bloodborne_heal.h"
#include "core/vr/bloodborne_held.h"
#include "core/vr/bloodborne_iframes.h"
#include "core/vr/bloodborne_melee.h"
#include "core/vr/bloodborne_menu.h"
#include "core/vr/bloodborne_lamps.h"
#include "core/vr/bloodborne_motion.h"
#include "core/vr/bloodborne_pose.h"
#include "core/vr/bloodborne_quickitems.h"
#include "core/vr/bloodborne_status.h"
#include "core/vr/bloodborne_transform.h"
#include "core/vr/bloodborne_tuning.h"
#include "core/vr/bloodborne_vitals.h"
#include "core/vr/body_settings.h"
#include "core/vr/hook_util.h"
#include "core/vr/pose_gestures.h"
#include "core/vr/quick_settings.h"
#include "core/vr/quick_channel.h"
#include "core/vr/transform_settings.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

// Module-relative addresses in the 1.09 eboot (patch file address - 0x400000).
// PlayerIns::Update (vtable +0xD0 = 0x18F4FF0), right before the IsMainPlayer check that
// guards the per-frame HP/stamina mirror sync (0x18F7880): every path of the update passes
// here once, before that frame's sync and outside any SpEffect-list walk. r13 = PlayerIns*.
// The stub replays the 15 patched bytes and returns to the jnz at 0x18F6C18, which reads the
// replayed flags; every branch into this block lands on the first patched byte.
constexpr u64 PlayerUpdateRva = 0x18F6C09;
constexpr size_t PlayerUpdateLead = 19; // signature bytes before the site
constexpr size_t PlayerUpdateLen = 15;
constexpr std::array<u8, 34> PlayerUpdateSig = {
    0x49, 0x8B, 0x85, 0xB0, 0x03, 0x00, 0x00, // mov rax, [r13+0x3B0]
    0x48, 0x8B, 0xB8, 0xB0, 0x00, 0x00, 0x00, // mov rdi, [rax+0xB0]
    0xE8, 0xC7, 0x9E, 0x15, 0x00,             // call 0x1A50AD0
    0x49, 0x8B, 0x45, 0x00,                   // mov rax, [r13]   <- hook site
    0x4C, 0x89, 0xEF,                         // mov rdi, r13
    0xFF, 0x90, 0xB8, 0x01, 0x00, 0x00,       // call [rax+0x1B8] (IsMainPlayer)
    0x84, 0xC0,                               // test al, al
};
// PlayerIns::UseGoods (vtable +0x408): what the vial animation's event (TAE 319,
// "ConsumeBloodVial") and the debug menu call. Base UseGoods applies the item's effect
// (SpEffect 3010: 40% of max HP, rune multipliers and rally trimming by the engine), then one
// vial is consumed through EquipGameData and the HUD refresh flags are set. Returns 0 only
// when the character type forbids items. The effect is applied BEFORE the inventory lookup,
// so at zero vials it would heal for free: the count is always checked first.
constexpr u64 UseGoodsRva = 0x18F9720;
constexpr std::array<u8, 24> UseGoodsSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54,
    0x53, 0x48, 0x83, 0xEC, 0x18, 0x41, 0x89, 0xD5, 0x41, 0x89, 0xF7, 0x48,
};
// PlayerIns::GetBloodVialCount: a pure read (inventory lookup of 0x400003E8 -> quantity,
// 0 without an entry). The game never calls it.
constexpr u64 VialCountRva = 0x18FA300;
constexpr std::array<u8, 20> VialCountSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x53, 0x48,
    0x83, 0xEC, 0x18, 0x4C, 0x8B, 0x3D, 0xCC, 0x98, 0xAE, 0x03,
};
// ChrIns::IsMainPlayer ([WorldChrMan+0x60] == this). Not called: its lea r14, [rip+disp]
// proves the WorldChrMan global.
constexpr u64 IsMainPlayerRva = 0x18C2D00;
constexpr std::array<u8, 24> IsMainPlayerSig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x56, 0x53, 0x48, 0x89, 0xFB, 0x4C, 0x8D,
    0x35, 0x67, 0xBB, 0xC7, 0x03, 0x49, 0x8B, 0x06, 0x48, 0x85, 0xC0, 0x75,
};
constexpr size_t IsMainPlayerLeaDisp = 13;
constexpr size_t IsMainPlayerLeaEnd = 17;
constexpr u64 WorldChrManRva = 0x553E878;
// Inventory quantity store (EquipInventoryData::AddQuantity). The GoldHEN "Infinite Items"
// cheat patches it so counts never change. Read at injection time: cheats can be toggled.
constexpr u64 QuantityStoreRva = 0x14D9556;
constexpr std::array<u8, 7> QuantityStoreSig = {0x41, 0x89, 0xC4, 0x44, 0x89, 0x63, 0x08};
// Both PlayerIns vtables hold UseGoods at +0x408.
constexpr u64 PlayerVtableRva = 0x538F310;
constexpr u64 PlayerVtableRva2 = 0x538FA80;
constexpr u32 OffsetVtableUseGoods = 0x408;
// Logging only.
constexpr u64 GameDataManRva = 0x553B130; // +0x08: PlayerGameData (expected == the player's)
constexpr u64 MaxVialsRva = 0x5593038;    // cached max carried vials (s32)

constexpr u32 OffsetMainPlayer = 0x60;  // WorldChrMan -> main PlayerIns*
constexpr u32 OffsetChrModules = 0x3B0; // PlayerIns -> modules, +0x20 = ChrDataModule
constexpr u32 OffsetDataModule = 0x20;
constexpr u32 OffsetPlayerGameData = 0x3C0; // PlayerIns -> PlayerGameData
constexpr u32 OffsetSpEffects = 0x1C8;      // PlayerIns -> SpEffect list (a field)
// PlayerIns quick-item use (s32 each): the state machine PlayerIns::Update steps through
// 2/3/5/7 (0x18F6D0D..0x18F74DF), 0 = idle, and the item being used (-1 = none). 0x18F9A30
// starts a use (state 1, item id); UseGoods always resets the state to 0 (0x18F98B4), so an
// injection during another item's use would cut that use short.
constexpr u32 OffsetItemUseItem = 0x47C;
constexpr u32 OffsetItemUseState = 0x490;
// ChrDataModule: the authoritative values.
constexpr u32 OffsetHp = 0xF8;
constexpr u32 OffsetMaxHp = 0xFC;
constexpr u32 OffsetRallyPool = 0x108;
constexpr u32 OffsetRallyFloor = 0x10C;
constexpr u32 OffsetRallyTimer = 0x118;
constexpr u32 OffsetRallyPending = 0x120;
constexpr u32 OffsetStamina = 0x134;
constexpr u32 OffsetMaxStamina = 0x138;
constexpr u32 OffsetDebugFlags = 0x200;
constexpr u16 FlagNoGoodsConsume = 0x200;
// PlayerGameData: HUD/save mirror, rewritten by the per-frame sync (never written here).
constexpr u32 OffsetMirrorHp = 0x14;
constexpr u32 OffsetMirrorMaxHp = 0x18;
constexpr u32 OffsetMirrorStamina = 0x30;
constexpr u32 OffsetMirrorMaxStamina = 0x34;
// Inventory (PlayerGameData+0x328): 16-byte entries {handle, item id, quantity, sort}.
constexpr u32 OffsetInventory = 0x328;
constexpr u32 OffsetInvNormalCapacity = 0x14;
constexpr u32 OffsetInvKeyCapacity = 0x24;
constexpr u32 OffsetInvNormalList = 0x48;
constexpr u32 OffsetInvKeyList = 0x58;
constexpr u32 OffsetInvHighest = 0x88;
// SpEffect list: first node at +0x08; node flags +0x1C, id +0x40 (what HKS env 56 at 0x1A142F1
// and the AddSpEffect helper at 0x1A24922 compare), param row +0x48, next +0x58.
constexpr u32 OffsetEffectHead = 0x08;
constexpr u32 OffsetNodeFlags = 0x1C;
constexpr u32 OffsetNodeId = 0x40;
constexpr u32 OffsetNodeRow = 0x48;
constexpr u32 OffsetNodeNext = 0x58;
constexpr u32 NodeSkipMask = 0x800C0003; // inactive entries (the game's own walks skip them)
constexpr u32 OffsetRowStateInfo = 0x156;
constexpr u32 OffsetRowRecoverRate = 0x208; // transfusionHpRecoverRate (heal multiplier)
// SpEffects with this stateInfo (2150, 3954) make the game play the vial's fail animation.
constexpr u16 StateInfoNoVial = 154;

constexpr s32 BloodVialGoodsId = 1000;
constexpr u32 BloodVialItemId = 0x400003E8;
constexpr s32 VialEffectId = 3010;
constexpr float VialHealRate = -40.0f; // SpEffect 3010 transfusionHpRate (% of max HP)
constexpr u32 WatchUpdates = 10;       // player updates the heal is watched for
// The heal vignette's item context: SpEffect 207 is added by every item animation's TAE (with
// 200, "standby action disabled"); the heal lands within an update or two of UseGoods, which
// ends the item use, so an HP rise this long after the context still counts.
constexpr s32 ItemAnimEffectId = 207;
constexpr auto ItemHealGrace = std::chrono::milliseconds(600);
// Goods whose use cannot heal but whose hit can win HP back (rally): Molotovs, throwing knives,
// pebbles, oil urns, bone marrow ash and the like (1200-1299), the lantern (1410). The goods in
// use is [player+0x1C0] (the item animation's latch, read by HKS GetItemAnimType).
constexpr u32 OffsetItemAnimGoods = 0x1C0;

using UseGoodsFn = u8(PS4_SYSV_ABI*)(void* player, s32 goods_id, s32 param);
using VialCountFn = s32(PS4_SYSV_ABI*)(void* player);

u8* g_base = nullptr;
bool g_installed = false; // the player update hook
bool g_heal = false;      // instant injection installed
bool g_cinematic = false; // cinematic detection runs in the hook
bool g_motion = false;    // the dodge comfort camera's motion signals are read in the hook
UseGoodsFn g_use_goods = nullptr;
VialCountFn g_vial_count = nullptr;
std::unique_ptr<Xbyak::CodeGenerator> g_code;
std::atomic_bool g_busy{false}; // re-entrancy guard (main player only)
std::atomic<u64> g_updates{0};  // every PlayerIns update seen (VR_HEAL_LOG)
// The Blood Vials carried (-1 unknown), every main-player update (CarriedBloodVials).
std::atomic<s32> g_carried_vials{-1};
bool g_log = false;             // VR_HEAL_LOG: state lines and per-update heal watch
bool g_dry_run = false;         // VR_HEAL_DRYRUN: every check, no UseGoods call
bool g_autotest = false;        // VR_HEAL_AUTOTEST: one injection 3 s after the first hit

// Main-player state: site A runs one main-player update at a time, usually on a job worker
// thread (not the loop thread that places the camera; the hand-offs to other threads are the
// atomics in vr_session).
bool g_disabled = false;
bool g_first_logged = false;
u64 g_main_updates = 0;
struct Watch {
    u32 left{};    // updates still watched
    u32 updates{}; // updates since the injection
    s32 hp_before{};
    s32 expected{}; // expected HP gain (logging only; the game's value is authoritative)
};
Watch g_watch{};

// The heal vignette's signal (HealSignalUpdate).
struct HealSignal {
    const u8* player{};
    s32 hp{-1};
    s32 max_hp{-1};
    bool item_seen{false};                           // an item context since this player appeared
    std::chrono::steady_clock::time_point item_at{}; // ... last seen
    bool context{false};                             // VR_HEAL_LOG: the item context last update
};
HealSignal g_signal{};

// The hit vignette's signal (HitSignalUpdate): an HP drop is a hit when the game's damage
// calculation hit the main player (PlayerDamageEvents, the damage hook in bloodborne_tuning.cpp)
// since the last drop it was paired with, at most HitEventGrace before the drop or HitEventWait
// after it (the drop waits that long for its event). Without the damage hook installed, a drop of
// at least HitFallbackFraction of max HP in one update counts. Anything else is a drain (poison
// and toxic ticks, HP spent rather than lost to an attack): no vignette.
constexpr auto HitEventGrace = std::chrono::milliseconds(500);
constexpr auto HitEventWait = std::chrono::milliseconds(150);
constexpr float HitFallbackFraction = 0.04f;
struct HitSignal {
    bool synced{false}; // events_used set for this player
    u32 events_used{0}; // the damage events already paired with a drop (or too old)
    s32 pending{0};     // HP lost by drops still waiting for their event
    s32 pending_from{0};
    std::chrono::steady_clock::time_point pending_at{};
    std::chrono::steady_clock::time_point line_window{}; // the log's rate limit (10 lines/s)
    u32 lines{0};
};
HitSignal g_hit{};

using Hook::Plausible;
using Hook::Read;

u8* DataModule(const u8* player) {
    u8* const modules = Read<u8*>(player + OffsetChrModules);
    if (!Plausible(modules)) {
        return nullptr;
    }
    u8* const module = Read<u8*>(modules + OffsetDataModule);
    return Plausible(module) ? module : nullptr;
}

// Visits the active SpEffects (node, param row or nullptr), like the game's own walks.
template <typename F>
void ForEachEffect(const u8* player, F&& visit) {
    u8* const list = Read<u8*>(player + OffsetSpEffects);
    if (!Plausible(list)) {
        return;
    }
    u8* node = Read<u8*>(list + OffsetEffectHead);
    for (u32 i = 0; i < 512 && Plausible(node); ++i) {
        if ((Read<u32>(node + OffsetNodeFlags) & NodeSkipMask) == 0) {
            u8* const row = Read<u8*>(node + OffsetNodeRow);
            visit(node, Plausible(row, 1) ? row : nullptr);
        }
        node = Read<u8*>(node + OffsetNodeNext);
    }
}

bool HasStateInfo(const u8* player, u16 state) {
    bool found = false;
    ForEachEffect(player, [&](u8*, u8* row) {
        if (row != nullptr && Read<u16>(row + OffsetRowStateInfo) == state) {
            found = true;
        }
    });
    return found;
}

// Product of the active heal multipliers (Caryll runes and the like).
float RecoverRate(const u8* player) {
    float rate = 1.0f;
    ForEachEffect(player, [&](u8*, u8* row) {
        if (row != nullptr) {
            rate *= Read<float>(row + OffsetRowRecoverRate);
        }
    });
    return rate;
}

// The vial's heal with the game's float steps (0x17E40B0), for the log only. Separate
// statements so the steps are never contracted into an FMA.
s32 ExpectedHeal(s32 max_hp, float rate) {
    const float percent = VialHealRate * rate;
    const float scaled = static_cast<float>(max_hp) * percent;
    const float negated = -scaled;
    const float amount = negated * 0.01f;
    return static_cast<s32>(amount);
}

bool InfiniteItemsActive() {
    return std::memcmp(g_base + QuantityStoreRva, QuantityStoreSig.data(),
                       QuantityStoreSig.size()) != 0;
}

VR::InjectResult Disable(const std::string& reason) {
    g_disabled = true;
    VR::SetInstantInjectAvailable(false);
    LOG_WARNING(Core, "VR heal: DISABLED ({}); stabs are now refused (inject=0: X is the game's "
                      "vial button)",
                reason);
    return VR::InjectResult::Disabled;
}

VR::InjectResult Deny(VR::InjectResult result, const char* reason, s32 hp, s32 max_hp, s32 vials) {
    LOG_INFO(Core, "VR heal: DENY {} hp={}/{} vials={}", reason, hp, max_hp, vials);
    return result;
}

// Runs inside the main player's update (site A, usually a job worker thread), like the vial's
// animation event.
VR::InjectResult TryInject(u8* player) {
    if (g_disabled) {
        return VR::InjectResult::Disabled;
    }
    u8* const vtable = Read<u8*>(player);
    u8* const use_goods = Plausible(vtable) ? Read<u8*>(vtable + OffsetVtableUseGoods) : nullptr;
    if ((vtable != g_base + PlayerVtableRva && vtable != g_base + PlayerVtableRva2) ||
        use_goods != g_base + UseGoodsRva) {
        return Disable(fmt::format("unexpected player vtable {} (+0x408 = {})", fmt::ptr(vtable),
                                   fmt::ptr(use_goods)));
    }
    u8* const module = DataModule(player);
    if (module == nullptr || !Plausible(Read<u8*>(player + OffsetPlayerGameData))) {
        LOG_INFO(Core, "VR heal: DENY no_player");
        return VR::InjectResult::NoPlayer;
    }
    const s32 hp = Read<s32>(module + OffsetHp);
    const s32 max_hp = Read<s32>(module + OffsetMaxHp);
    const s32 vials = g_vial_count(player);
    if (hp <= 0) {
        return Deny(VR::InjectResult::Dead, "dead", hp, max_hp, vials);
    }
    if (hp >= max_hp && !VR::GetSettings().inject_full_hp) {
        return Deny(VR::InjectResult::FullHp, "full_hp", hp, max_hp, vials);
    }
    if (HasStateInfo(player, StateInfoNoVial)) {
        return Deny(VR::InjectResult::Blocked, "state154", hp, max_hp, vials);
    }
    if (vials <= 0) {
        return Deny(VR::InjectResult::NoVials, "no_vials", hp, max_hp, vials); // never free
    }
    if (vials > 999) {
        return Disable(fmt::format("implausible vial count {}", vials));
    }
    const s32 item_state = Read<s32>(player + OffsetItemUseState);
    const s32 item_id = Read<s32>(player + OffsetItemUseItem);
    if (item_state != 0) {
        // Another quick item (pebble, Molotov, ...) is being used: the game never interrupts
        // that with a vial either.
        LOG_INFO(Core, "VR heal: DENY busy (item use state={} item={}) hp={}/{} vials={}",
                 item_state, item_id, hp, max_hp, vials);
        return VR::InjectResult::Busy;
    }

    const float rate = RecoverRate(player);
    const s32 heal = ExpectedHeal(max_hp, rate);
    const s32 expected_hp = std::min(hp + heal, max_hp);
    const float pool = Read<float>(module + OffsetRallyPool);
    if (g_dry_run) {
        LOG_INFO(Core,
                 "VR heal: DRY RUN hp={}/{} vials={} pool={:.0f} R={:.3f} expectHeal={} "
                 "expectHp={} (UseGoods not called)",
                 hp, max_hp, vials, pool, rate, heal, expected_hp);
        return VR::InjectResult::DryRun;
    }

    const auto start = std::chrono::steady_clock::now();
    const bool used = g_use_goods(player, BloodVialGoodsId, -1) != 0;
    const s32 after = g_vial_count(player);
    const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - start)
                            .count();
    if (!used) {
        // The character type forbids item use: nothing was applied or consumed.
        return Deny(VR::InjectResult::Blocked, "chrtype", hp, max_hp, vials);
    }
    LOG_INFO(Core,
             "VR heal: injected hp={}/{} vials {}->{} pool={:.0f} R={:.3f} expectHeal={} "
             "expectHp={} item use state={} item={} -> state={} item={} ({} us)",
             hp, max_hp, vials, after, pool, rate, heal, expected_hp, item_state, item_id,
             Read<s32>(player + OffsetItemUseState), Read<s32>(player + OffsetItemUseItem), micros);
    // quick_use_sounds: the Blood Vial clip's own stab and blood (goodsUseAnim 26: c100080, c5480),
    // skipped with the clip (the injection was silent before).
    QueueQuickUseSound(BloodVialGoodsId, 26);
    g_watch = {WatchUpdates, 0, hp, expected_hp - hp};
    if (after != vials - 1) {
        const bool frozen = InfiniteItemsActive() ||
                            (Read<u16>(module + OffsetDebugFlags) & FlagNoGoodsConsume) != 0;
        if (frozen) {
            LOG_INFO(Core, "VR heal: vial count unchanged (Infinite Items or no-consume flag)");
        } else {
            // The item use ran but the count did not drop for no known reason: stop using it.
            Disable(fmt::format("vial count {} -> {} after UseGoods", vials, after));
        }
    }
    return VR::InjectResult::Healed;
}

bool HasEffect(const u8* player, s32 id) {
    bool found = false;
    ForEachEffect(player, [&](u8* node, u8*) {
        if (Read<s32>(node + OffsetNodeId) == id) {
            found = true;
        }
    });
    return found;
}

// Reports when the heal lands (the SpEffect HP tick applies it within an update or two).
void UpdateWatch(const u8* player, const u8* module) {
    if (g_watch.left == 0) {
        return;
    }
    --g_watch.left;
    ++g_watch.updates;
    const s32 hp = Read<s32>(module + OffsetHp);
    if (g_log) {
        u8* const pgd = Read<u8*>(player + OffsetPlayerGameData);
        LOG_INFO(Core, "VR heal: WATCH +{} hp={} pool={:.0f} mirror={} has{}={}", g_watch.updates,
                 hp, Read<float>(module + OffsetRallyPool),
                 Plausible(pgd) ? Read<s32>(pgd + OffsetMirrorHp) : -1, VialEffectId,
                 HasEffect(player, VialEffectId) ? "y" : "n");
    }
    const s32 gained = hp - g_watch.hp_before;
    if (gained > 0) {
        LOG_INFO(Core, "VR heal: RESULT ok heal={} (expected {}) after {} updates", gained,
                 g_watch.expected, g_watch.updates);
        g_watch.left = 0;
    } else if (g_watch.left == 0) {
        LOG_INFO(Core, "VR heal: RESULT no HP change within {} updates (expected +{})",
                 WatchUpdates, g_watch.expected);
    }
}

// The hit vignette (VR::NoteHitTaken): pairs the main player's HP drops with the damage
// calculations that hit him (HitSignal). `before` / `max_before`: the last update's HP and max HP
// (-1 for a new player). A drop with a change of max HP, from 0 HP or to a new player never
// counts. Main player's update, every update.
void HitSignalUpdate(s32 before, s32 max_before, s32 hp, s32 max_hp,
                     std::chrono::steady_clock::time_point now) {
    auto& w = g_hit;
    s64 event_ns = 0;
    const u32 events = PlayerDamageEvents(event_ns);
    const s64 now_ns =
        std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    if (!w.synced) {
        w = {};
        w.synced = true;
        w.events_used = events; // a hit before this player appeared is not his
    }
    const bool hook = IsOffhandDamageInstalled();
    const bool fresh = events != w.events_used &&
                       now_ns - event_ns <= std::chrono::nanoseconds(HitEventGrace).count();
    const auto line_allowed = [&] {
        if (now - w.line_window >= std::chrono::seconds(1)) {
            w.line_window = now;
            w.lines = 0;
        }
        return ++w.lines <= 10;
    };
    const auto hit = [&](s32 from, s32 to, s32 max, const char* how) {
        const float fraction = static_cast<float>(from - to) / static_cast<float>(max);
        VR::NoteHitTaken(fraction);
        if (line_allowed()) {
            LOG_INFO(Core, "VR hit: HP {} -> {} (-{} = {:.1f}% of max {}) {}: hit vignette", from,
                     to, from - to, 100.0f * fraction, max, how);
        }
    };
    const auto drain = [&](s32 from, s32 to, const char* how) {
        if (g_log && line_allowed()) {
            LOG_INFO(Core, "VR hit: HP {} -> {} (-{}) {}: no vignette", from, to, from - to, how);
        }
    };
    const bool drop = before > 0 && hp < before && max_hp == max_before && max_hp > 0;
    if (drop && !hook) {
        if (static_cast<float>(before - hp) >= HitFallbackFraction * static_cast<float>(max_hp)) {
            hit(before, hp, max_hp, "(no damage hook: a large drop)");
        } else {
            drain(before, hp, "without the damage hook, a small drop");
        }
    } else if (drop) {
        if (w.pending == 0) {
            w.pending_from = before;
            w.pending_at = now;
        }
        w.pending += before - hp;
    } else if (max_hp != max_before && w.pending > 0) {
        w.pending = 0; // max HP changed: what is pending cannot be compared any more
    }
    if (hook && w.pending > 0) {
        if (fresh) {
            w.events_used = events;
            hit(w.pending_from, w.pending_from - w.pending, max_hp, "after a hit on the hunter");
            w.pending = 0;
        } else if (now - w.pending_at > HitEventWait) {
            drain(w.pending_from, w.pending_from - w.pending, "without a hit (poison, toxic, ...)");
            w.pending = 0;
        }
    }
    // An event that took no HP (or whose drop came long before) never pairs with a later drain.
    if (events != w.events_used &&
        now_ns - event_ns > std::chrono::nanoseconds(HitEventGrace).count()) {
        w.events_used = events;
    }
}

// The heal vignette (VR::NoteItemHeal): HP that rises while an item is used or just after
// (ItemHealGrace), or while the vial's heal effect (SpEffect 3010) is active: the instant
// injection, the game's own Blood Vial and the other healing items. HP won back by hitting enemies
// (rally) has no item context, unless a thrown item's hit wins it (the goods in use is a
// throwable: not counted). A revival (from 0 HP), a change of max HP and a new player (loading)
// never count. Main player's update, every update.
void HealSignalUpdate(const u8* player) {
    const u8* const module = DataModule(player);
    if (module == nullptr) {
        return;
    }
    auto& h = g_signal;
    if (player != h.player) {
        h = {};
        h.player = player;
        g_hit.synced = false;
    }
    const s32 hp = Read<s32>(module + OffsetHp);
    const s32 max_hp = Read<s32>(module + OffsetMaxHp);
    bool item_anim = false;
    bool vial_effect = false;
    ForEachEffect(player, [&](u8* node, u8*) {
        const s32 id = Read<s32>(node + OffsetNodeId);
        item_anim = item_anim || id == ItemAnimEffectId;
        vial_effect = vial_effect || id == VialEffectId;
    });
    const s32 use_state = Read<s32>(player + OffsetItemUseState);
    const s32 goods = Read<s32>(player + OffsetItemAnimGoods);
    const bool context = use_state != 0 || item_anim || vial_effect;
    const auto now = std::chrono::steady_clock::now();
    if (context) {
        h.item_seen = true;
        h.item_at = now;
    }
    if (g_log && context != h.context) {
        LOG_INFO(Core,
                 "VR heal: item context {} (use state={} item={} anim goods={} 207={} 3010={})",
                 context ? "on" : "off", use_state, Read<s32>(player + OffsetItemUseItem), goods,
                 item_anim ? "y" : "n", vial_effect ? "y" : "n");
    }
    h.context = context;
    if (h.hp > 0 && hp > h.hp && max_hp == h.max_hp) {
        const bool throwable = (goods >= 1200 && goods < 1300) || goods == 1410;
        const bool recent = h.item_seen && now - h.item_at <= ItemHealGrace;
        const bool item = vial_effect || (recent && !throwable);
        if (item) {
            VR::NoteItemHeal();
        }
        if (g_log || item) {
            LOG_INFO(Core,
                     "VR heal: HP {} -> {} (+{}) {} (use state={} item={} anim goods={} 207={} "
                     "3010={})",
                     h.hp, hp, hp - h.hp,
                     item ? "from an item: heal vignette"
                          : (recent ? "during a throwable's use: not an item heal"
                                    : "without an item: not an item heal (rally, lamp, ...)"),
                     use_state, Read<s32>(player + OffsetItemUseItem), goods, item_anim ? "y" : "n",
                     vial_effect ? "y" : "n");
        }
    }
    HitSignalUpdate(h.hp, h.max_hp, hp, max_hp, now); // the hit vignette's hits
    h.hp = hp;
    h.max_hp = max_hp;
}

// VR_HEAL_LOG: the vial entry found by scanning the inventory (cross-checks the count).
std::string ScanVialEntry(const u8* pgd) {
    const u8* const inventory = pgd + OffsetInventory;
    const u32 highest = std::min(Read<u32>(inventory + OffsetInvHighest), 4096u);
    const u32 key_capacity = Read<u32>(inventory + OffsetInvKeyCapacity);
    const u32 normal_capacity = Read<u32>(inventory + OffsetInvNormalCapacity);
    u8* const key_list = Read<u8*>(inventory + OffsetInvKeyList);
    u8* const normal_list = Read<u8*>(inventory + OffsetInvNormalList);
    for (u32 index = 0; index <= highest; ++index) {
        const bool key = index < key_capacity;
        u8* const list = key ? key_list : normal_list;
        const u32 slot = key ? index : index - key_capacity;
        if (!Plausible(list) || (!key && slot >= normal_capacity)) {
            continue;
        }
        const u8* const entry = list + static_cast<size_t>(slot) * 16;
        if (Read<u32>(entry) != 0 && Read<u32>(entry + 4) == BloodVialItemId) {
            return fmt::format("idx={} handle={:#x} id={:#x} qty={}", index, Read<u32>(entry),
                               Read<u32>(entry + 4), Read<u32>(entry + 8));
        }
    }
    return "none";
}

// VR_HEAL_LOG: every second for the first 20 s, then every 10 s.
void LogState(u8* player, const u8* module) {
    using Clock = std::chrono::steady_clock;
    static const Clock::time_point start = Clock::now();
    static Clock::time_point last = start;
    static u64 last_main = 0;
    static u64 last_all = 0;
    const auto now = Clock::now();
    const auto interval =
        now - start < std::chrono::seconds(20) ? std::chrono::seconds(1) : std::chrono::seconds(10);
    if (now - last < interval) {
        return;
    }
    const float seconds = std::max(std::chrono::duration<float>(now - last).count(), 0.001f);
    last = now;
    const u64 all = g_updates.load(std::memory_order_relaxed);
    const float main_rate = static_cast<float>(g_main_updates - last_main) / seconds;
    const float all_rate = static_cast<float>(all - last_all) / seconds;
    last_main = g_main_updates;
    last_all = all;

    u8* const vtable = Read<u8*>(player);
    u8* const use_goods = Plausible(vtable) ? Read<u8*>(vtable + OffsetVtableUseGoods) : nullptr;
    u8* const pgd = Read<u8*>(player + OffsetPlayerGameData);
    u8* const game_data = Read<u8*>(g_base + GameDataManRva);
    u8* const pgd2 = Plausible(game_data) ? Read<u8*>(game_data + 8) : nullptr;
    std::string effects;
    u32 count = 0;
    ForEachEffect(player, [&](u8* node, u8*) {
        if (++count <= 16) {
            effects +=
                fmt::format("{}{}", effects.empty() ? "" : ",", Read<s32>(node + OffsetNodeId));
        }
    });
    LOG_INFO(Core,
             "VR heal: STATE upd/s main={:.0f} all={:.0f} player={} vt=+{:#x} vt408=+{:#x} "
             "pgd={} pgd2={} hp={}/{} mirror={}/{} st={}/{} mirrorSt={}/{} vials=(fn){} "
             "(scan {}) maxVials={} pool/floor/timer/pend={:.0f}/{:.0f}/{:.2f}/{:.1f} R={:.3f} "
             "st154={} itemUse={}/{} effects=[{}]{}",
             main_rate, all_rate, fmt::ptr(player), static_cast<u64>(vtable - g_base),
             static_cast<u64>(use_goods - g_base), fmt::ptr(pgd), fmt::ptr(pgd2),
             Read<s32>(module + OffsetHp), Read<s32>(module + OffsetMaxHp),
             Plausible(pgd) ? Read<s32>(pgd + OffsetMirrorHp) : -1,
             Plausible(pgd) ? Read<s32>(pgd + OffsetMirrorMaxHp) : -1,
             Read<s32>(module + OffsetStamina), Read<s32>(module + OffsetMaxStamina),
             Plausible(pgd) ? Read<s32>(pgd + OffsetMirrorStamina) : -1,
             Plausible(pgd) ? Read<s32>(pgd + OffsetMirrorMaxStamina) : -1,
             Plausible(pgd) ? g_vial_count(player) : -1,
             Plausible(pgd) ? ScanVialEntry(pgd) : "no pgd", Read<s32>(g_base + MaxVialsRva),
             Read<float>(module + OffsetRallyPool), Read<float>(module + OffsetRallyFloor),
             Read<float>(module + OffsetRallyTimer), Read<float>(module + OffsetRallyPending),
             RecoverRate(player), HasStateInfo(player, StateInfoNoVial) ? 1 : 0,
             Read<s32>(player + OffsetItemUseState), Read<s32>(player + OffsetItemUseItem), effects,
             count > 16 ? ",..." : "");
}

// VR_HEAL_AUTOTEST: one injection 3 s after HP is first seen below max (no gesture needed).
void AutoTest(s32 hp, s32 max_hp) {
    static bool done = false;
    static bool hurt = false;
    static std::chrono::steady_clock::time_point since{};
    if (done) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (!hurt) {
        if (hp > 0 && hp < max_hp) {
            hurt = true;
            since = now;
            LOG_INFO(Core, "VR heal: AUTOTEST hp {}/{}, injecting in 3 s", hp, max_hp);
        }
        return;
    }
    if (now - since >= std::chrono::seconds(3)) {
        done = true;
        LOG_INFO(Core, "VR heal: AUTOTEST request {}", VR::RequestInject() ? "queued" : "busy");
    }
}

// VR_HEAL_TEST_HURT=<percent> (mock tests of the heal vignette with the game's own vial): once,
// 10 s after the first main-player update, takes that much of max HP (never below 1), as a hit
// without its animation would.
void TestHurt(u8* player) {
    static const s32 percent = [] {
        const char* value = std::getenv("VR_HEAL_TEST_HURT");
        return value != nullptr ? std::clamp(std::atoi(value), 0, 99) : 0;
    }();
    static bool done = false;
    static std::chrono::steady_clock::time_point first{};
    if (percent == 0 || done) {
        return;
    }
    u8* const module = DataModule(player);
    if (module == nullptr) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if (first == std::chrono::steady_clock::time_point{}) {
        first = now;
    }
    if (now - first < std::chrono::seconds(10)) {
        return;
    }
    done = true;
    const s32 hp = Read<s32>(module + OffsetHp);
    const s32 max_hp = Read<s32>(module + OffsetMaxHp);
    const s32 hurt = std::max(1, hp - max_hp * percent / 100);
    std::memcpy(module + OffsetHp, &hurt, sizeof(hurt));
    LOG_INFO(Core, "VR heal: TEST hurt hp {} -> {} of {}", hp, hurt, max_hp);
}

void OnMainPlayerUpdate(u8* player) {
    // PlayerUpdateHook validated this main-player pointer. Keep its address/handle in
    // the host; snapshot consumers see only a generation tied to this actor lifetime.
    VR::Quick::ObserveInteractionActor(reinterpret_cast<uintptr_t>(player), Read<u32>(player + 0x08));
    VR::NotePlayerUpdate();
    ++g_main_updates;
    if (!g_first_logged) {
        g_first_logged = true;
        LOG_INFO(Core, "VR: first main-player update, player={} thread '{}'", fmt::ptr(player),
                 Common::GetCurrentThreadName());
    }
    // The cinematic and motion signals first: they only read the state the last behaviour step
    // left.
    if (g_cinematic) {
        CinematicUpdate(player);
    }
    if (g_motion) {
        MotionUpdate(player);
    }
    TuningUpdate(player);     // logs and the HP autotest only
    IframesSiteA(player);     // the dodge i-frame log's view of the game (VR_IFRAMES_LOG only)
    MenuUpdate(player);       // in-game menu state for the presenter
    LampsSiteA(player);       // lamp requests: native game calls on the player update thread
    EnemyBarsUpdate();        // the enemy HP bars switch's log (ui_enemy_hp_bars)
    TestHurt(player);         // VR_HEAL_TEST_HURT only
    HealSignalUpdate(player); // the heal vignette's item heals
    VitalsSiteA(g_base, player); // HP, rally, stamina, bullets for the wrist gauge (hud_vitals)
    StatusSiteA(player);         // build-ups drain a little faster (status_drain_pct)
    // The Blood Vials carried (vial_inject_drain: a vial armed with none left is an empty one,
    // bloodborne_held.cpp): one inventory lookup per update, as the game's own HUD count. Read
    // every update so the vial's first frame in the hand already knows.
    g_carried_vials.store(g_heal && !g_disabled &&
                                  (VR::GetSettings().vial_inject_drain ||
                                   VR::Quick::GetQuickSettings().holster_mode != 0) &&
                                  Plausible(Read<u8*>(player + OffsetPlayerGameData))
                              ? g_vial_count(player)
                              : -1,
                          std::memory_order_relaxed);
    HeldSiteATick(player);    // the vial in the hunter's hand, without the body's pose hook
    // The vial's injection first; a quick item's use or throw waits for the next update when it
    // took this one (one UseGoods per update).
    bool injected = false;
    if (g_heal) {
        if (const u8* const module = DataModule(player); module != nullptr) {
            if (g_log) {
                LogState(player, module);
            }
            UpdateWatch(player, module);
            if (g_autotest) {
                AutoTest(Read<s32>(module + OffsetHp), Read<s32>(module + OffsetMaxHp));
            }
        }
        bool expired = false;
        if (VR::TakeInjectRequest(expired)) {
            if (expired) {
                LOG_INFO(Core, "VR heal: DENY expired (request older than the time-out)");
            }
            VR::FinishInjectRequest(expired ? VR::InjectResult::Expired : TryInject(player));
            injected = true;
        }
    }
    GiveKitSiteA(g_base, player);       // TEST/DEBUG kit (vr.ini give_kit, off by default)
    QuickItemsSiteA(player, !injected); // the quick-item slots and the arc's requests
    MeleeSiteA(player); // physical melee: the telegraph's AI notices, a new swing's visceral try
    TransformSiteA(player); // the weapon-only transform's form switch (the game's event 32)
    PoseSiteA(player); // physical gestures: the Brain's and the Doll's markers (pose_gestures)
}

// Runs for every PlayerIns update (first argument: r13 = PlayerIns*).
void PS4_SYSV_ABI PlayerUpdateHook(u8* player, u8* /*saved_ymm*/) {
    if (g_log) {
        g_updates.fetch_add(1, std::memory_order_relaxed);
    }
    u8* const world = Read<u8*>(g_base + WorldChrManRva);
    if (!Plausible(world) || player == nullptr || Read<u8*>(world + OffsetMainPlayer) != player) {
        return;
    }
    if (g_busy.exchange(true, std::memory_order_acquire)) {
        return; // re-entered from inside the game's item use
    }
    OnMainPlayerUpdate(player);
    g_busy.store(false, std::memory_order_release);
}

} // Anonymous namespace

void InstallPlayerHook(VAddr base, u64 size) {
    if (g_installed) {
        return;
    }
    VR::SetInstantInjectAvailable(false);
    const auto& settings = VR::GetSettings();
    if (!settings.inject) {
        LOG_INFO(Core, "VR heal: inject=0, instant Blood Vial injection not installed");
    }
    if (!settings.cinematic) {
        LOG_INFO(Core, "VR cine: cinematic=0, scripted animations stay in VR");
    }
    // Motion signals also time the ladder coat fade when the dodge comfort camera is off.
    // Keep sampling when the cutaway is hidden: its in-game switch can enable it live.
    const auto& body = VR::Body::GetBodySettings();
    const bool motion = VR::IsFirstPerson() &&
                        (settings.dodge_camera || body.body_cutaway_boost ||
                         std::getenv("VR_DODGE_LOG") != nullptr);
    // The in-game menu detection (core/vr/bloodborne_menu.h) samples in this hook too.
    const bool menu = settings.menu_mode;
    // Quick items in VR (core/vr/bloodborne_quickitems.h) read the slots and use items here.
    const bool quick = VR::Quick::GetQuickSettings().quick_items;
    // The weapon-only transform switches the form here (core/vr/bloodborne_transform.h).
    const bool transform = VR::Transform::GetTransformSettings().native;
    // Physical gestures (core/vr/bloodborne_pose.h) apply their markers here.
    const bool pose = VR::PoseGesture::GetPoseSettings().enabled;
    if (!settings.inject && !settings.cinematic && !motion && !menu && !quick && !transform &&
        !pose) {
        return;
    }
    auto* const module = reinterpret_cast<u8*>(base);
    if (size < MaxVialsRva + sizeof(s32)) {
        LOG_INFO(Core, "VR heal: eboot too small ({:#x}) for Bloodborne 1.09; not installed", size);
        return;
    }
    const auto matches = [module](u64 rva, const auto& signature) {
        return std::memcmp(module + rva, signature.data(), signature.size()) == 0;
    };
    const bool site_ok = matches(PlayerUpdateRva - PlayerUpdateLead, PlayerUpdateSig);
    const bool use_goods_ok = matches(UseGoodsRva, UseGoodsSig);
    const bool vial_count_ok = matches(VialCountRva, VialCountSig);
    bool main_ok = matches(IsMainPlayerRva, IsMainPlayerSig);
    if (main_ok) {
        s32 displacement{};
        std::memcpy(&displacement, module + IsMainPlayerRva + IsMainPlayerLeaDisp,
                    sizeof(displacement));
        main_ok =
            IsMainPlayerRva + IsMainPlayerLeaEnd + static_cast<s64>(displacement) == WorldChrManRva;
    }
    const bool quantity_original = matches(QuantityStoreRva, QuantityStoreSig);
    const auto verdict = [](bool ok) { return ok ? "ok" : "MISMATCH"; };
    const auto dump = [module](const char* name, u64 rva, size_t length) {
        LOG_WARNING(Core, "VR heal: {} live bytes at {:#x}: {}", name, rva,
                    Hook::HexBytes(module + rva, length));
    };
    // The hook itself needs the site and the main-player check; the injection also needs the
    // two functions it calls, the cinematic detection its own signatures (it logs them).
    const bool hook_ok = site_ok && main_ok;
    const bool heal_ok = settings.inject && hook_ok && use_goods_ok && vial_count_ok;
    if (settings.inject && !heal_ok) {
        LOG_WARNING(Core,
                    "VR heal: base={:#x} siteA sig={} usegoods={} vialcount={} ismain={} "
                    "installed=0; stabs are refused (inject=0: X is the game's vial button)",
                    base, verdict(site_ok), verdict(use_goods_ok), verdict(vial_count_ok),
                    verdict(main_ok));
        if (!use_goods_ok) {
            dump("UseGoods", UseGoodsRva, UseGoodsSig.size());
        }
        if (!vial_count_ok) {
            dump("GetBloodVialCount", VialCountRva, VialCountSig.size());
        }
    }
    if (!site_ok) {
        dump("siteA", PlayerUpdateRva - PlayerUpdateLead, PlayerUpdateSig.size());
    }
    if (!main_ok) {
        dump("IsMainPlayer", IsMainPlayerRva, IsMainPlayerSig.size());
    }
    const bool cinematic_ok = settings.cinematic && InitCinematic(base, size, site_ok, main_ok);
    const bool motion_ok = motion && InitMotion(base, size, site_ok, main_ok);
    const bool menu_ok = menu && hook_ok;
    const bool quick_ok = quick && hook_ok;
    const bool transform_ok = transform && hook_ok;
    const bool pose_ok = pose && hook_ok;
    if (!heal_ok && !cinematic_ok && !motion_ok && !menu_ok && !quick_ok && !transform_ok &&
        !pose_ok) {
        return;
    }

    u8* const site = module + PlayerUpdateRva;
    if (!Hook::MakeWritable(site, PlayerUpdateLen)) {
        LOG_ERROR(Core, "VR: could not make the player update writable; injection, cinematic "
                        "detection and dodge signals not installed");
        return;
    }
    // Everything the hook reads is set before the jump is written.
    g_base = module;
    g_heal = heal_ok;
    g_cinematic = cinematic_ok;
    g_motion = motion_ok;
    if (heal_ok) {
        g_use_goods = reinterpret_cast<UseGoodsFn>(module + UseGoodsRva);
        g_vial_count = reinterpret_cast<VialCountFn>(module + VialCountRva);
        g_log = std::getenv("VR_HEAL_LOG") != nullptr;
        g_dry_run = std::getenv("VR_HEAL_DRYRUN") != nullptr;
        g_autotest = std::getenv("VR_HEAL_AUTOTEST") != nullptr;
    }
    try {
        g_code = std::make_unique<Xbyak::CodeGenerator>(1024);
        // The replay comes from the constant signature, never from the live site.
        void* const stub =
            Hook::EmitHostCallStub(*g_code, reinterpret_cast<void*>(&PlayerUpdateHook),
                                   PlayerUpdateSig.data() + PlayerUpdateLead, PlayerUpdateLen,
                                   reinterpret_cast<u64>(site + PlayerUpdateLen), Xbyak::util::r13);
        Hook::WriteJump(site, PlayerUpdateLen, stub);
    } catch (const Xbyak::Error& e) {
        g_heal = g_cinematic = g_motion = false;
        LOG_ERROR(Core,
                  "VR: failed to generate the player update hook ({}); stabs are refused, "
                  "scripted animations stay in VR, no dodge signals",
                  e.what());
        return;
    }
    g_installed = true;
    VR::SetInstantInjectAvailable(heal_ok);
    if (heal_ok) {
        LOG_INFO(Core,
                 "VR heal: base={:#x} siteA sig=ok usegoods=ok vialcount=ok ismain=ok "
                 "(WorldChrMan RVA {:#x}) quantity store {} installed=1{}{}{}",
                 base, WorldChrManRva, quantity_original ? "original" : "patched (Infinite Items?)",
                 g_log ? " log" : "", g_dry_run ? " DRY-RUN" : "", g_autotest ? " autotest" : "");
    }
    LOG_INFO(Core,
             "VR: player update hook installed at {:#x} (instant injection {}, cinematic {}, "
             "dodge signals {}, menu detection {})",
             PlayerUpdateRva, heal_ok ? "on" : "off", cinematic_ok ? "on" : "off",
             motion_ok ? "on" : "off", menu_ok ? "on" : "off");
}

bool IsHealInstalled() {
    return g_heal;
}

bool IsPlayerHookInstalled() {
    return g_installed;
}

s32 CarriedBloodVials() {
    return g_carried_vials.load(std::memory_order_relaxed);
}

} // namespace VR::Bloodborne
