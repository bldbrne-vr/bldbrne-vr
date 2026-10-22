// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/logging/log.h"
#include "core/vr/arm_ik.h"
#include "core/vr/bloodborne_vitals.h"
#include "core/vr/c0000_skeleton.h"
#include "core/vr/body_channel.h"
#include "core/vr/combat_channel.h"
#include "core/vr/hook_util.h"
#include "core/vr/inframe.h"
#include "core/vr/live_tune.h"
#include "core/vr/vitals.h"
#include "core/vr/vitals_channel.h"
#include "core/vr/vr_session.h"

namespace VR::Bloodborne {

namespace {

namespace V = VR::Vitals;
namespace F = VR::InFrame;
using Hook::Plausible;
using Hook::Read;
using Vec3 = VR::Body::Vec3;

// Module-relative addresses in the 1.09 eboot (as bloodborne_gun.cpp / bloodborne_heal.cpp).
constexpr u64 WorldChrManRva = 0x553E878;
// FindItemIndex(inventory, const u32* item): the inventory index or -1. Pure.
constexpr u64 FindItemRva = 0x14D9F60;
constexpr std::array<u8, 32> FindItemSig = {
    0x8B, 0x06, 0x83, 0xF8, 0xFF, 0x0F, 0x84, 0xA6, 0x00, 0x00, 0x00, 0x89, 0xC1, 0x48, 0x69, 0xC9,
    0x15, 0x3F, 0xEF, 0x03, 0x48, 0xC1, 0xE9, 0x20, 0x89, 0xC2, 0x29, 0xCA, 0xD1, 0xEA, 0x01, 0xCA,
};
// ChrIns::GetDmyPolyMtx, `s32(ChrIns*, Mtx34* out, const s32* id, u32 max)`: world 3x4 rows.
constexpr u64 DmyPolyRva = 0x18C3BF0;
constexpr std::array<u8, 32> DmyPolySig = {
    0x55, 0x48, 0x89, 0xE5, 0x41, 0x57, 0x41, 0x56, 0x41, 0x55, 0x41, 0x54, 0x53, 0x48, 0x83, 0xEC,
    0x28, 0x41, 0x89, 0xCF, 0x48, 0x89, 0xD3, 0x48, 0x89, 0x75, 0xB0, 0x4C, 0x8B, 0x35, 0xCE, 0xFF,
};

constexpr u32 OffsetMainPlayer = 0x60;  // WorldChrMan -> the main PlayerIns
constexpr u32 OffsetChrModules = 0x3B0; // PlayerIns -> modules, +0x20 = ChrDataModule
constexpr u32 OffsetDataModule = 0x20;
// ChrDataModule (the authoritative values; the HUD's own orange end is HP + (int)pool, 0x1A0D3F0).
constexpr u32 OffsetHp = 0xF8;
constexpr u32 OffsetMaxHp = 0xFC;
constexpr u32 OffsetRallyPool = 0x108;
constexpr u32 OffsetRallyFloor = 0x10C;
constexpr u32 OffsetRallyTimer = 0x118;
constexpr u32 OffsetRallyPending = 0x120;
constexpr u32 OffsetStamina = 0x134;
constexpr u32 OffsetMaxStamina = 0x138;
constexpr u32 OffsetPlayerGameData = 0x3C0;
constexpr u32 OffsetInventory = 0x328; // PlayerGameData: the inventory FindItemIndex takes
// Inventory: 16-byte entries {handle (0 = gone), item, qty, sort}; key items first.
constexpr u32 OffsetInvCapacity = 0x10;
constexpr u32 OffsetInvKeyCap = 0x24;
constexpr u32 OffsetInvNormalList = 0x48;
constexpr u32 OffsetInvKeyList = 0x58;
constexpr u32 OffsetInvHighest = 0x88;
constexpr u32 GoodsCategory = 0x40000000u;
constexpr s32 QuicksilverBullets = 900;
constexpr s32 BloodBullets = 901;

// The hunter's dummies the gauge is placed from (c0000.flver): the palm (L_Hand; its +Z out of
// the palm, +Y out on the thumb's side, as the held items use it), the wrist (L_Hand, its frame
// in L_Hand's: the skeleton read at runtime, core/vr/c0000_skeleton.h Skeleton::left_hand) and
// near the elbow (L_Forearm, 37 mm from the joint).
constexpr s32 PalmDummy = 3;
constexpr s32 WristDummy = VR::Body::C0000::DmyIdLeftHand;
constexpr s32 ElbowDummy = 153;

using FindItemFn = s32(PS4_SYSV_ABI*)(u8* inventory, const u32* item);
using DmyPolyFn = s32(PS4_SYSV_ABI*)(u8* chr, float* out, const s32* id, u32 max);

s64 NowNs() {
    return VR::Body::SteadyNowNs();
}

// ---- Site A -------------------------------------------------------------------------------------
struct Site {
    bool checked{false};
    FindItemFn find{nullptr};
    DmyPolyFn dmy{nullptr};
    u8* base{nullptr};
    V::Status last{};
    u64 serial{0};
    s32 seen_max_bullets{0};
    u32 rally_logs{0};
    s32 logged_hp{-1};
    s32 logged_pool{-1};
    u32 fail_logs{0};
};
Site g_site{};

// The goods count (Quicksilver / Blood Bullets) through the game's own lookup, -1 if unreadable.
s32 GoodsCount(const u8* player, s32 goods) {
    auto& g = g_site;
    if (g.find == nullptr) {
        return -1;
    }
    u8* const pgd = Read<u8*>(player + OffsetPlayerGameData);
    if (!Plausible(pgd)) {
        return -1;
    }
    u8* const inv = pgd + OffsetInventory;
    const u32 item = GoodsCategory | static_cast<u32>(goods);
    const s32 index = g.find(inv, &item);
    if (index < 0 || Read<u32>(inv + OffsetInvCapacity) == 0 ||
        static_cast<u32>(index) > Read<u32>(inv + OffsetInvHighest)) {
        return 0;
    }
    const u32 key_cap = Read<u32>(inv + OffsetInvKeyCap);
    const u32 i = static_cast<u32>(index);
    u8* const list = Read<u8*>(inv + (i < key_cap ? OffsetInvKeyList : OffsetInvNormalList));
    if (!Plausible(list)) {
        return -1;
    }
    const u8* e = list + static_cast<size_t>(i < key_cap ? i : i - key_cap) * 16;
    return Read<u32>(e) != 0 && Read<u32>(e + 4) == item ? Read<s32>(e + 8) : 0;
}

void Check(u8* module) {
    auto& g = g_site;
    g.checked = true;
    g.base = module;
    const bool find_ok = std::memcmp(module + FindItemRva, FindItemSig.data(), FindItemSig.size()) == 0;
    const bool dmy_ok = std::memcmp(module + DmyPolyRva, DmyPolySig.data(), DmyPolySig.size()) == 0;
    g.find = find_ok ? reinterpret_cast<FindItemFn>(module + FindItemRva) : nullptr;
    g.dmy = dmy_ok ? reinterpret_cast<DmyPolyFn>(module + DmyPolyRva) : nullptr;
    LOG_INFO(Core,
             "VR vitals: installed (hud_vitals={}): FindItemIndex {}, GetDmyPolyMtx {}{}",
             VR::GetSettings().hud_vitals == 0 ? "classic" : "wrist", find_ok ? "ok" : "MISMATCH",
             dmy_ok ? "ok" : "MISMATCH",
             find_ok && dmy_ok ? "" : ": the wrist gauge falls back to the classic HUD");
}

} // Anonymous namespace

void VitalsSiteA(u8* module, u8* player) {
    auto& g = g_site;
    if (!g.checked) {
        Check(module);
    }
    const auto& settings = VR::GetSettings();
    if ((settings.hud_vitals == static_cast<s32>(V::Mode::Classic) && !settings.health_color &&
         !settings.stamina_color) ||
        player == nullptr) {
        return;
    }
    V::Status s{};
    s.steady_ns = NowNs();
    u8* const modules = Read<u8*>(player + OffsetChrModules);
    u8* const data = Plausible(modules) ? Read<u8*>(modules + OffsetDataModule) : nullptr;
    float pool = 0.0f;
    if (Plausible(data)) {
        s.hp = Read<s32>(data + OffsetHp);
        s.max_hp = Read<s32>(data + OffsetMaxHp);
        pool = Read<float>(data + OffsetRallyPool);
        s.stamina = Read<s32>(data + OffsetStamina);
        s.max_stamina = Read<s32>(data + OffsetMaxStamina);
        s.rally_hp = V::RallyEnd(s.hp, s.max_hp, pool);
        if (settings.hud_vitals != static_cast<s32>(V::Mode::Classic)) {
            s.qs_bullets = GoodsCount(player, QuicksilverBullets);
            s.blood_bullets = GoodsCount(player, BloodBullets);
            g.seen_max_bullets = std::max(g.seen_max_bullets, s.qs_bullets);
            s.bullet_cap = V::BulletCap(g.seen_max_bullets);
        }
        s.ok = s.max_hp > 0 && s.hp >= 0 && s.hp <= s.max_hp && s.max_stamina > 0 &&
               s.qs_bullets >= 0 && s.blood_bullets >= 0;
    }
    // VR_VITALS_TEST_FAIL=<s> (tests only): every read fails from <s> seconds after the first one
    // (the fallback to the classic HUD).
    static const char* const test_fail = std::getenv("VR_VITALS_TEST_FAIL");
    static const s64 first_ns = s.steady_ns;
    // VR_VITALS_TEST_RALLY=<percent>[@<s>] (tests only): once, <s> seconds (default 20) after the
    // first read, a hit as the game's own damage leaves it (SetHp 0x1A0D470): HP down by <percent>
    // of max HP, the rally pool and its floor that much, its timer 4 s; the game then drains it as
    // after a real hit (the gauge's amber layer, the edges' rim).
    static const char* const test_rally = std::getenv("VR_VITALS_TEST_RALLY");
    static bool rally_done = false;
    if (test_rally != nullptr && !rally_done && Plausible(data) && s.ok && s.hp > 1) {
        float pct = 0.0f, after_s = 20.0f;
        const int n = std::sscanf(test_rally, "%f@%f", &pct, &after_s);
        if (n >= 1 && s.steady_ns - first_ns >= static_cast<s64>(after_s * 1e9)) {
            rally_done = true;
            const s32 loss = std::clamp(static_cast<s32>(pct * 0.01f * static_cast<float>(s.max_hp)),
                                        1, s.hp - 1);
            const s32 hp = s.hp - loss;
            const float pool = static_cast<float>(loss);
            const float timer = 4.0f;
            std::memcpy(data + OffsetHp, &hp, sizeof(hp));
            std::memcpy(data + OffsetRallyPool, &pool, sizeof(pool));
            std::memcpy(data + OffsetRallyFloor, &pool, sizeof(pool));
            std::memcpy(data + OffsetRallyTimer, &timer, sizeof(timer));
            LOG_INFO(Core, "VR vitals: TEST rally: HP {} -> {} of {}, rally pool / floor {:.0f}",
                     s.hp, hp, s.max_hp, pool);
        }
    }
    if (test_fail != nullptr &&
        s.steady_ns - first_ns >= static_cast<s64>(std::atof(test_fail) * 1e9)) {
        s.ok = false;
    }
    if (!s.ok && g.fail_logs < 5) {
        ++g.fail_logs;
        LOG_WARNING(Core,
                    "VR vitals: read failed (data module {}, hp {}/{}, stamina {}/{}, bullets {} "
                    "+ {})",
                    fmt::ptr(data), s.hp, s.max_hp, s.stamina, s.max_stamina, s.qs_bullets,
                    s.blood_bullets);
    }
    const V::Status& o = g.last;
    const bool changed = s.ok != o.ok || s.hp != o.hp || s.max_hp != o.max_hp ||
                         s.rally_hp != o.rally_hp || s.stamina != o.stamina ||
                         s.max_stamina != o.max_stamina || s.qs_bullets != o.qs_bullets ||
                         s.blood_bullets != o.blood_bullets || s.bullet_cap != o.bullet_cap;
    if (changed) {
        ++g.serial;
    }
    s.serial = g.serial;
    // The rally's semantics, checked in the log against the game's own orange segment: every
    // change of HP or of the pool (the first 400).
    if (Plausible(data) && s.ok && g.rally_logs < 400 &&
        (s.hp != g.logged_hp || static_cast<s32>(pool) != g.logged_pool)) {
        ++g.rally_logs;
        LOG_INFO(Core,
                 "VR vitals: HP {}/{} (was {}) rally pool {:.1f} floor {:.1f} timer {:.2f} "
                 "pending {:.1f} -> the orange segment to {} (+{}) | stamina {}/{} bullets {}+{} "
                 "| t {:.3f}",
                 s.hp, s.max_hp, g.logged_hp, pool, Read<float>(data + OffsetRallyFloor),
                 Read<float>(data + OffsetRallyTimer), Read<float>(data + OffsetRallyPending),
                 s.rally_hp, s.rally_hp - s.hp, s.stamina, s.max_stamina, s.qs_bullets,
                 s.blood_bullets, static_cast<double>(s.steady_ns) * 1e-9);
        g.logged_hp = s.hp;
        g.logged_pool = static_cast<s32>(pool);
    }
    // VR_VITALS_LOG=1 (tests): stamina as the game spends and regains it, a line when it moved by
    // 5 or more since the last one, or reached empty or full (the first 300).
    static const bool stamina_log = std::getenv("VR_VITALS_LOG") != nullptr;
    static s32 logged_stamina = -1;
    static u32 stamina_logs = 0;
    if (stamina_log && s.ok && stamina_logs < 300 && s.stamina != logged_stamina &&
        (std::abs(s.stamina - logged_stamina) >= 5 || s.stamina == 0 ||
         s.stamina == s.max_stamina)) {
        ++stamina_logs;
        LOG_INFO(Core, "VR vitals: stamina {}/{} (was {}) | t {:.3f}", s.stamina, s.max_stamina,
                 logged_stamina, static_cast<double>(s.steady_ns) * 1e-9);
        logged_stamina = s.stamina;
    }
    g.last = s;
    V::PublishStatus(s);
}

// ---- The Blood Gauge (render-camera hook) -------------------------------------------------------
namespace {

struct Render {
    V::GaugeAnim anim{};
    V::ChangeTracker changes{};
    V::FitFilter fit{};
    V::FitMotionGate fit_motion{};
    u64 motion_rejected{};
    // The straps' sides: (hand strap, elbow strap) x (round +v, round -v), as the probe's lines.
    std::array<V::StrapFilter, V::StrapLines> straps{};
    std::array<float, V::StrapLines> strap_exits{};
    V::GlanceState glance{};
    s64 last_ns{0};
    u32 probe_id{0};
    u32 result_id{0};
    int digit_up{-1};
    s64 anchor_ns{0};
    s64 drawn_ns{0};
    s64 stats_ns{0};
    u64 drawn{0};
    u64 skipped{0};
    u64 measured{0};
    u64 no_fit{0};
    float radius_sum{0.0f};
    bool placed_logged{false};
    bool anchor_logged{false};
    u32 fit_logs{0};
    u64 fit_seen{0};
};
Render g_r{};

Vec3 Col(const std::array<float, 12>& m, int c) {
    return {m[static_cast<size_t>(c)], m[static_cast<size_t>(4 + c)],
            m[static_cast<size_t>(8 + c)]};
}

Vec3 Pos(const std::array<float, 12>& m) {
    return {m[3], m[7], m[11]};
}

// The camera record's view (rows and position, as the quick items' Mapping).
struct View {
    std::array<float, 12> rows{};
    Vec3 position{};
    Vec3 Point(Vec3 w) const {
        const Vec3 d = w - position;
        return {rows[0] * d.x + rows[1] * d.y + rows[2] * d.z,
                rows[4] * d.x + rows[5] * d.y + rows[6] * d.z,
                rows[8] * d.x + rows[9] * d.y + rows[10] * d.z};
    }
    Vec3 Dir(Vec3 d) const {
        return {rows[0] * d.x + rows[1] * d.y + rows[2] * d.z,
                rows[4] * d.x + rows[5] * d.y + rows[6] * d.z,
                rows[8] * d.x + rows[9] * d.y + rows[10] * d.z};
    }
};

// The straps' places along the plate (mm from its elbow end, as vr_inframe.frag's G_STRAP1/2):
// the hand strap, the elbow strap (the order of V::StrapLines' pairs).
constexpr std::array<float, 2> StrapUMm{42.85f, 10.05f};

// An item from a centre and three (view) columns already scaled.
F::Item MakeItem(F::MeshId mesh, Vec3 c, Vec3 x, Vec3 y, Vec3 z) {
    F::Item item{};
    item.mesh = mesh;
    item.mv = {x.x, y.x, z.x, c.x, x.y, y.y, z.y, c.y, x.z, y.z, z.z, c.z};
    return item;
}

// The screen-space direction (x right, y up) of a view-space vector `v` at view point `p`.
void ScreenDir(Vec3 p, Vec3 v, float& sx, float& sy) {
    const float z = std::max(p.z, 1e-4f);
    sx = (v.x - p.x * v.z / z) / z;
    sy = (v.y - p.y * v.z / z) / z;
}

// The forearm's frame at the gauge (world): the wrist joint, toward the elbow, the back of the
// wrist (dorsal), the thumb's side.
struct ArmFrame {
    Vec3 wrist{};
    Vec3 axis{};   // unit, toward the elbow
    Vec3 dorsal{}; // unit, out of the back of the wrist
    Vec3 thumb{};  // unit, round the arm toward the thumb
    float forearm_m{};
    float palm_m{};
};

// `dmy`: dummy 344 in L_Hand's frame (position, forward, up): the wrist joint (L_Hand's origin)
// from it.
bool FrameOf(const std::array<float, 12>& palm, const std::array<float, 12>& wrist,
             const std::array<float, 12>& elbow, const VR::Body::C0000::DummyPoly& dmy,
             ArmFrame& out) {
    namespace B = VR::Body;
    // L_Hand's rotation from dummy 344's forward (column 2) and up (column 1), then its origin.
    const Vec3 fw = B::Normalize(Col(wrist, 2));
    const Vec3 uw = B::Normalize(B::Perp(Col(wrist, 1), fw));
    const Vec3 fl = B::Normalize(Vec3{dmy.forward[0], dmy.forward[1], dmy.forward[2]});
    const Vec3 ul = B::Normalize(B::Perp(Vec3{dmy.up[0], dmy.up[1], dmy.up[2]}, fl));
    const Vec3 sw = B::Cross(fw, uw);
    const Vec3 sl = B::Cross(fl, ul);
    if (B::LengthSq(fw) < 0.5f || B::LengthSq(uw) < 0.5f) {
        return false;
    }
    // R = [fw uw sw] [fl ul sl]^T.
    const auto rotate = [&](Vec3 v) {
        return fw * B::Dot(fl, v) + uw * B::Dot(ul, v) + sw * B::Dot(sl, v);
    };
    out.wrist = Pos(wrist) - rotate(Vec3{dmy.position[0], dmy.position[1], dmy.position[2]});
    const Vec3 to_elbow = Pos(elbow) - out.wrist;
    out.forearm_m = B::Length(to_elbow);
    out.palm_m = B::Length(Pos(palm) - out.wrist);
    if (out.forearm_m < 0.12f || out.forearm_m > 0.40f || out.palm_m > 0.15f) {
        return false;
    }
    out.axis = to_elbow * (1.0f / out.forearm_m);
    const Vec3 volar = B::Normalize(Col(palm, 2));
    out.dorsal = B::Normalize(B::Perp(volar * -1.0f, out.axis));
    if (B::LengthSq(out.dorsal) < 0.5f) {
        return false;
    }
    Vec3 thumb = B::Perp(B::Perp(B::Normalize(Col(palm, 1)), out.axis), out.dorsal);
    thumb = B::Normalize(thumb, B::Cross(out.axis, out.dorsal));
    out.thumb = thumb;
    return true;
}

std::array<float, 4> LinearOf(V::Srgb c, float a = 1.0f) {
    const auto l = V::Linear(c);
    return {l[0], l[1], l[2], a};
}

} // Anonymous namespace

void VitalsRenderTick(u64 record_id, const float* rows, const float* position) {
    auto& g = g_site;
    auto& r = g_r;
    const auto& settings = VR::GetSettings();
    if (!g.checked || g.dmy == nullptr || g.base == nullptr || rows == nullptr ||
        position == nullptr || settings.hud_vitals != static_cast<s32>(V::Mode::Wrist)) {
        return;
    }
    VR::CameraRecord rec{};
    if (!VR::GetRecord(record_id, rec) || !rec.driven || rec.menu || !rec.first_person) {
        return;
    }
    const s64 now = NowNs();
    const float dt = r.last_ns == 0 ? 0.0f
                                    : std::clamp(static_cast<float>(now - r.last_ns) * 1e-9f,
                                                 0.0f, 0.25f);
    r.last_ns = now;
    V::GaugeState state{};
    V::LatestGauge(state);
    state.tick_ns = now;
    V::Status st{};
    if (!V::LatestStatus(st) || !st.ok || now - st.steady_ns > 1'000'000'000) {
        V::PublishGauge(state);
        return;
    }
    u8* const world = Read<u8*>(g.base + WorldChrManRva);
    u8* const chr = Plausible(world) ? Read<u8*>(world + OffsetMainPlayer) : nullptr;
    if (!Plausible(chr)) {
        V::PublishGauge(state);
        return;
    }
    // The wrist dummy's frame on L_Hand comes from the hunter's skeleton, read at runtime: no
    // gauge until it is (or when it is unavailable: core/vr/bloodborne_skeleton.cpp logs why).
    const VR::Body::C0000::Skeleton* const sk = VR::Body::C0000::Active();
    if (sk == nullptr) {
        V::PublishGauge(state);
        return;
    }
    alignas(16) std::array<float, 8 * 12> buf{};
    std::array<std::array<float, 12>, 3> m{};
    const std::array<s32, 3> ids{PalmDummy, WristDummy, ElbowDummy};
    for (size_t i = 0; i < ids.size(); ++i) {
        if (g.dmy(chr, buf.data(), &ids[i], 1) < 1) {
            if (!r.anchor_logged) {
                r.anchor_logged = true;
                LOG_WARNING(Core, "VR vitals: no dummy {} on the hunter: no wrist gauge", ids[i]);
            }
            V::PublishGauge(state);
            return;
        }
        std::memcpy(m[i].data(), buf.data(), sizeof(m[i]));
    }
    ArmFrame arm{};
    if (!FrameOf(m[0], m[1], m[2], sk->left_hand, arm)) {
        if (!r.anchor_logged) {
            r.anchor_logged = true;
            LOG_WARNING(Core,
                        "VR vitals: the forearm's dummies make no frame (wrist -> elbow {:.3f} m, "
                        "palm {:.3f} m): no wrist gauge",
                        arm.forearm_m, arm.palm_m);
        }
        V::PublishGauge(state);
        return;
    }
    r.anchor_ns = now;
    state.anchor_ns = now;
    View view{};
    std::memcpy(view.rows.data(), rows, sizeof(view.rows));
    view.position = {position[0], position[1], position[2]};

    const Vec3 wrist_view = view.Point(arm.wrist);
    const Vec3 axis_view = view.Dir(arm.axis);
    const Vec3 dorsal_view = view.Dir(arm.dorsal);
    r.fit_motion.Step({arm.wrist.x, arm.wrist.y, arm.wrist.z, wrist_view.x, wrist_view.y,
                       wrist_view.z, axis_view.x, axis_view.y, axis_view.z, dorsal_view.x,
                       dorsal_view.y, dorsal_view.z},
                      dt);

    // The fit: the newest depth probe's answer (a frame or two old) -> the arm's radius here.
    const float scale = settings.vitals_gauge_scale;
    V::FitConfig fit{};
    fit.standoff_m = VR::Tune::Get("vitals.standoff_m", settings.vitals_gauge_standoff_m);
    float measured = -1.0f;
    bool strap_seen = false;
    std::array<float, V::ProbeLines> exits{};
    F::ProbeResult pr{};
    if (F::LatestProbe(pr) && pr.id != r.result_id && pr.id != 0 &&
        pr.count == static_cast<u32>(V::ProbePoints)) {
        r.result_id = pr.id;
        const bool fit_allowed = r.fit_motion.Accept(pr.id);
        r.motion_rejected += fit_allowed ? 0 : 1;
        if (fit_allowed) {
            strap_seen = true;
            for (int line = 0; line < V::ProbeLines; ++line) {
                std::array<float, V::FitSteps> gap{};
                std::array<bool, V::FitSteps> ok{};
                for (int i = 0; i < V::FitSteps; ++i) {
                    const size_t k = static_cast<size_t>(line * V::FitSteps + i);
                    ok[static_cast<size_t>(i)] = pr.scene_z[k] > 0.0f;
                    gap[static_cast<size_t>(i)] = pr.point_z[k] - pr.scene_z[k];
                }
                exits[static_cast<size_t>(line)] =
                    V::LineExit(gap.data(), ok.data(), V::FitSteps, fit);
            }
            for (int i = 0; i < V::StrapLines; ++i) {
                r.strap_exits[static_cast<size_t>(i)] = exits[static_cast<size_t>(V::FitLines + i)];
            }
            // (the plate: its own lines only)
            measured = V::CombineExits(exits.data(), V::FitLines);
            if (measured > 0.0f) {
                ++r.measured;
                r.radius_sum += measured;
            } else {
                ++r.no_fit;
            }
            static const bool fit_log = std::getenv("VR_VITALS_LOG") != nullptr;
            ++r.fit_seen;
            if (fit_log && r.fit_logs < 1000 && (r.fit_seen % 300) == 1) {
                // Every 20th: each line's gaps (mm; x = no depth there).
                for (int line = 0; line < V::ProbeLines; ++line) {
                    std::string text;
                    for (int i = 0; i < V::FitSteps; ++i) {
                        const size_t k = static_cast<size_t>(line * V::FitSteps + i);
                        text += pr.scene_z[k] > 0.0f
                                    ? fmt::format(" {:.0f}", (pr.point_z[k] - pr.scene_z[k]) * 1e3f)
                                    : std::string{" x"};
                    }
                    LOG_INFO(Core, "VR vitals: fit #{} line {} gaps (mm, r 12..96):{}", pr.id, line,
                             text);
                }
            }
            if (fit_log && r.fit_logs < 1000 && (r.fit_seen % 30) == 1) {
                ++r.fit_logs;
                const auto side = [&](int i) {
                    const auto& s = r.straps[static_cast<size_t>(i)];
                    return fmt::format("{:.1f}{}",
                                       exits[static_cast<size_t>(V::FitLines + i)] * 1e3f,
                                       s.valid ? fmt::format(" (b/a {:.2f} end {:.0f})", s.b_over_a,
                                                             s.end * 57.29578f)
                                               : std::string{" (round)"});
                };
                LOG_INFO(
                    Core,
                    "VR vitals: fit #{} exits {:.1f} {:.1f} {:.1f} {:.1f} {:.1f} mm -> {:.1f} mm "
                    "(plate at {:.1f} mm); straps' sides at {:.0f} deg: hand {} / {}, elbow {} / "
                    "{}",
                    pr.id, exits[0] * 1e3f, exits[1] * 1e3f, exits[2] * 1e3f, exits[3] * 1e3f,
                    exits[4] * 1e3f, measured * 1e3f, r.fit.radius * 1e3f,
                    V::StrapPolarRad * 57.29578f, side(0), side(1), side(2), side(3));
            }
        }
    }
    const float radius = r.fit.Step(measured, dt, fit);
    state.radius_m = radius;
    state.fits = r.fit.measured;

    // The gauge's frame (world): along the arm toward the hand (+u), out of the wrist (normal),
    // round it toward the thumb (+v).
    const float from_wrist = VR::Tune::Get("vitals.from_wrist_m", settings.vitals_gauge_from_wrist_m);
    const Vec3 axis_point = arm.wrist + arm.axis * from_wrist;
    const Vec3 u_dir = arm.axis * -1.0f;
    const Vec3 n_dir = arm.dorsal;
    // (round the arm toward the little finger: the reference look's layout, the blood tube on
    // the thumb's side, the counter's window on the other)
    const Vec3 v_dir = arm.thumb * -1.0f;
    const Vec3 centre = axis_point + n_dir * radius;

    // The animation: changes, the glance, the heartbeat (the presenter's clock).
    const u32 changes = r.changes.Step(st);
    const Vec3 c_view = view.Point(centre);
    const Vec3 n_view = view.Dir(n_dir);
    const bool glanced = V::StepGlance(r.glance, {c_view.x, c_view.y, c_view.z},
                                       {n_view.x, n_view.y, n_view.z}, {0.0f, 0.0f, 0.0f},
                                       {0.0f, 0.0f, 1.0f}, dt);
    float pulse = 0.0f;
    V::Beat beat{};
    if (V::LatestBeat(beat) && beat.beat_ns != 0) {
        pulse = V::BeatEnvelope(static_cast<float>(now - beat.beat_ns) * 1e-9f);
    }
    const V::GaugeLook look = r.anim.Step(st, changes, dt, glanced, st.hp > 0, pulse);
    if (look.alpha <= 0.004f) {
        V::PublishGauge(state);
        return;
    }
    // On the frame at all (else no list: the in-frame pass costs nothing then).
    const float tan_half = std::tan(0.5f * std::max(rec.proj_vfov, 0.1f));
    const float aspect = rec.proj_aspect > 0.0f ? rec.proj_aspect : 4.0f / 3.0f;
    // (the plate's centre within the frame, a little margin for its half size)
    const bool on_frame = c_view.z > 0.05f &&
                          std::abs(c_view.x) < 1.08f * tan_half * aspect * c_view.z &&
                          std::abs(c_view.y) < 1.08f * tan_half * c_view.z;
    if (!on_frame) {
        ++r.skipped;
        V::PublishGauge(state);
        return;
    }

    F::List list{};
    list.record = record_id;
    list.steady_ns = now;
    list.producer = F::Producer::Vitals;
    list.exposure = 1.0f;
    const float pull = VR::Tune::Get("vitals.pull_m", settings.vitals_gauge_pull_m);
    const Vec3 uv_view = view.Dir(u_dir);
    const Vec3 vv_view = view.Dir(v_dir);
    const float mm = 1e-3f * scale;
    const float plate_u = (V::GaugeLengthMm + 4.0f) * mm; // the mesh's margin round the plate
    const float plate_v = (V::GaugeWidthMm + 6.0f) * mm;
    // The straps first (under the plate's lugs): leather round the arm on the sleeve, at the
    // plate's level on top and each side down onto the sleeve where its probe line came out of it
    // (V::FitStrapSide; round until the first fit), each side its own half ring.
    const float strap_a = std::max(radius - fit.standoff_m + 0.0012f, 0.01f);
    for (int i = 0; i < V::StrapLines; ++i) {
        auto& sf = r.straps[static_cast<size_t>(i)];
        if (strap_seen) {
            sf.Step(V::FitStrapSide(strap_a, r.strap_exits[static_cast<size_t>(i)]), strap_a, dt);
        }
        const float u_mm = StrapUMm[static_cast<size_t>(i / 2)];
        const float along = (u_mm - 0.5f * V::GaugeLengthMm) * mm;
        const float strap_b = sf.valid ? sf.b_over_a * strap_a : strap_a;
        const Vec3 at = view.Point(axis_point + u_dir * along);
        F::Item strap = MakeItem(F::MeshId::GaugeStrap, at, uv_view * (6.3f * mm),
                                 n_view * strap_a, vv_view * strap_b);
        strap.shade = F::Shade::Gauge;
        strap.color = {look.hp, look.rally, look.stamina, look.bullets};
        // (uv: alpha, the side of the ring drawn (+1: round +v, -1: round -v), part 1 = a strap,
        // where it fades out)
        strap.uv = {look.alpha, (i % 2) == 0 ? 1.0f : -1.0f, 1.0f,
                    sf.valid ? sf.end : V::StrapEndCircle};
        strap.depth_pull = pull;
        list.Add(strap);
    }
    F::Item plate = MakeItem(F::MeshId::GaugePlate, c_view, uv_view * plate_u, n_view * scale,
                             vv_view * plate_v);
    plate.shade = F::Shade::Gauge;
    plate.color = {look.hp, look.rally, look.stamina, look.bullets};
    plate.emissive = {look.glow, look.pulse * (st.hp < st.max_hp * settings.vitals_low_hp ? 1.0f : 0.0f),
                      look.fog, look.bubbles};
    plate.uv = {look.alpha, 5.0f / static_cast<float>(std::max(st.bullet_cap, 5)), 0.0f,
                look.shake_mm};
    plate.depth_pull = pull;
    list.Add(plate);

    // The counter: the number on the drum window at the hand end of the quicksilver tube, upright
    // as you see it (DigitUp), rolling to a new one.
    constexpr float WinU = 0.5f * (36.1f + 49.7f);
    constexpr float WinV = 0.5f * (16.45f + 32.3f);
    const float wx = (WinU - 0.5f * V::GaugeLengthMm) * mm;
    const float wz = (WinV - 0.5f * V::GaugeWidthMm) * mm;
    // The shell's sag there (vr_inframe.cpp GaugePlate: a 45 mm radius over 40.5 mm), lifted a
    // little off the drum.
    const float zf = wz / plate_v;
    const float sag = 0.045f * (1.0f - std::cos(0.5f * 0.0405f / 0.045f)) / 0.25f * zf * zf * scale;
    const Vec3 win_world = centre + u_dir * wx + v_dir * wz + n_dir * (0.0006f * scale - sag);
    const Vec3 win_view = view.Point(win_world);
    float ux{}, uy{}, vx{}, vy{};
    ScreenDir(win_view, uv_view, ux, uy);
    ScreenDir(win_view, vv_view, vx, vy);
    r.digit_up = V::DigitUp(ux, uy, vx, vy, r.digit_up);
    const Vec3 up = r.digit_up == 0   ? uv_view
                    : r.digit_up == 1 ? uv_view * -1.0f
                    : r.digit_up == 2 ? vv_view
                                      : vv_view * -1.0f;
    Vec3 right = VR::Body::Cross(up, n_view);
    {
        // Never mirrored on the screen.
        float rx{}, ry{}, px{}, py{};
        ScreenDir(win_view, right, rx, ry);
        ScreenDir(win_view, up, px, py);
        if (rx * py - ry * px < 0.0f) {
            right = right * -1.0f;
        }
    }
    const float text_h = V::GaugeDigitMm / 0.53f * mm; // the label row: digits ~53% of it
    const auto add_digits = [&](s32 number, float shift, float alpha) {
        if (alpha <= 0.01f) {
            return;
        }
        const std::string text = std::to_string(std::clamp(number, 0, 99));
        const int label = list.AddLabel(text.c_str(), 48);
        if (label < 0) {
            return;
        }
        const Vec3 at = win_view + up * (shift * V::GaugeDigitMm * mm);
        F::Item digits = MakeItem(F::MeshId::Quad, at, right * text_h, up * text_h, n_view);
        digits.shade = F::Shade::Text;
        digits.tex = F::Tex::Label;
        digits.tex_key = static_cast<u32>(label);
        digits.fit_label = static_cast<s8>(label);
        digits.color = LinearOf(V::ToneColor(look.tone), alpha * look.alpha);
        digits.emissive = LinearOf(V::ColorSoot, 0.85f);
        digits.depth_pull = pull;
        list.Add(digits);
    };
    if (look.roll < 1.0f) {
        add_digits(look.digits_old, 0.7f * look.roll, 1.0f - look.roll);
        add_digits(look.digits, -0.7f * (1.0f - look.roll), look.roll);
    } else {
        add_digits(look.digits, 0.0f, 1.0f);
    }

    // The depth probe for the next frames' fit: lines out of the axis through the plate, then the
    // straps' sides (each strap's place along the arm, V::StrapPolarRad round either way).
    list.probe.id = ++r.probe_id == 0 ? ++r.probe_id : r.probe_id;
    r.fit_motion.Submitted(list.probe.id);
    list.probe.count = static_cast<u32>(V::ProbePoints);
    constexpr float AroundRad = 23.0f * 3.14159265f / 180.0f;
    constexpr float HandStrapM = (StrapUMm[0] - 0.5f * V::GaugeLengthMm) * 1e-3f;
    constexpr float ElbowStrapM = (StrapUMm[1] - 0.5f * V::GaugeLengthMm) * 1e-3f;
    const std::array<std::array<float, 2>, V::ProbeLines> lines{{
        {0.0f, 0.0f},
        {0.022f, 0.0f},
        {-0.022f, 0.0f},
        {0.0f, AroundRad},
        {0.0f, -AroundRad},
        {HandStrapM, V::StrapPolarRad},
        {HandStrapM, -V::StrapPolarRad},
        {ElbowStrapM, V::StrapPolarRad},
        {ElbowStrapM, -V::StrapPolarRad},
    }};
    for (int line = 0; line < V::ProbeLines; ++line) {
        const auto& l = lines[static_cast<size_t>(line)];
        const Vec3 dir = n_dir * std::cos(l[1]) + v_dir * std::sin(l[1]);
        const Vec3 base = axis_point + u_dir * (l[0] * scale);
        for (int i = 0; i < V::FitSteps; ++i) {
            const Vec3 p = view.Point(base + dir * V::FitRadius(i));
            list.probe.points[static_cast<size_t>(line * V::FitSteps + i)] = {p.x, p.y, p.z};
        }
    }
    // All of it sits on his forearm (inframe_hand_sync carries it, F::Carry).
    for (u32 i = 0; i < list.count; ++i) {
        list.items[i].hand = true;
    }
    F::Submit(list);
    ++r.drawn;
    r.drawn_ns = now;
    state.drawn_ns = now;
    V::PublishGauge(state);

    if (!r.placed_logged) {
        r.placed_logged = true;
        LOG_INFO(Core,
                 "VR vitals: wrist gauge on the hunter's left forearm: wrist -> elbow dummy {:.3f} "
                 "m, palm {:.3f} m from the wrist; plate {:.1f} x {:.1f} mm, {:.2f} m from the "
                 "eye, radius {:.1f} mm ({}), digits {:.1f} mm",
                 arm.forearm_m, arm.palm_m, V::GaugeLengthMm * scale, V::GaugeWidthMm * scale,
                 VR::Body::Length(c_view), radius * 1e3f,
                 r.fit.valid ? "fitted" : "default, not measured yet", V::GaugeDigitMm * scale);
    }
    if (now - r.stats_ns >= 10'000'000'000LL) {
        if (r.stats_ns != 0) {
            LOG_INFO(Core,
                     "VR vitals: STATS fit motion rejected {}; gauge drawn {} (off the frame {}), "
                     "fits {} measured / {} "
                     "none (mean {:.1f} mm), plate at {:.1f} mm, HP {}/{} (orange to {}) stamina "
                     "{}/{} bullets {}+{} (cap {}), glance {}, eye {:.2f} m",
                     r.motion_rejected, r.drawn, r.skipped, r.measured, r.no_fit,
                     r.measured > 0 ? r.radius_sum / static_cast<float>(r.measured) * 1e3f : 0.0f,
                     radius * 1e3f, st.hp, st.max_hp, st.rally_hp, st.stamina, st.max_stamina,
                     st.qs_bullets, st.blood_bullets, st.bullet_cap, glanced ? "yes" : "no",
                     VR::Body::Length(c_view));
        }
        r.stats_ns = now;
        r.drawn = r.skipped = r.measured = r.no_fit = r.motion_rejected = 0;
        r.radius_sum = 0.0f;
    }
}

} // namespace VR::Bloodborne
