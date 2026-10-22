// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>

#include "core/vr/arm_ik.h"
#include "core/vr/ragdoll_deaths.h"

// Instant ragdoll on a right-hand melee kill (vr.ini melee_kill_ragdoll; the game side is
// core/vr/bloodborne_ragdoll.cpp). The pure parts, unit-tested (tests/test_ragdoll_kill.cpp):
//  - which blow counts: a lethal hit of the main player's own right-hand weapon attack (the
//    physical swing, a stab, a charged one, R1 / R2 of a weapon without physical moves). Bullets,
//    explosions, thrown items, the left hand, kicks, viscerals / ripostes / backstabs (their
//    BehaviorParam category is not the right weapon's, and the target is in a throw) do not;
//  - which death animation ends in the ragdoll (core/vr/ragdoll_deaths.h, the game's own TAE
//    event 228): only those are cut short; one that ends animated, a boss's, a dissolve, an NPC
//    hunter's (c0000) stays as the game plays it;
//  - what to do on each of the enemy's updates after the kill (wait for its death animation, cut
//    it, or keep it);
//  - the chance (melee_ragdoll_vanilla_pct, 22% by default) that a kill which would go limp keeps
//    the game's own death animation and ragdoll anyway: one roll per kill;
//  - the push: the blade's velocity at the hit, as momentum, spread over the ragdoll's bodies by
//    their real masses (half on the whole body, half on the part struck), clamped, never toward
//    the hunter (at most 60 deg off "away" from him, 35 deg at arm's length); then for 0.6 s no
//    body near him moves toward him (the corpse never tips onto the hunter);
//  - the death table is keyed by the full TimeAct id (each TAE group's variant on its own).
namespace VR::Ragdoll {

using Vec3 = VR::Body::Vec3;

// ---- The blow ------------------------------------------------------------------------------------

constexpr std::int32_t HandRight = -1;             // damage result +0x44 (-2 = the left hand)
constexpr std::uint8_t CategoryRightWeapon = 1;    // +0x13E BehaviorParam.category
constexpr std::int32_t NoBullet = -1;              // +0x14C BulletParam id

enum class Blow : std::uint8_t {
    Kill,      // the main player's right-hand weapon attack, lethal
    NotLethal, // it leaves HP
    NotPlayer, // another attacker, or the hunter himself was hit
    Bullet,    // a bullet, an explosion, a thrown item (Molotov, knife, pebble), bolt paper's shot
    LeftHand,  // the left hand's weapon (the gun, the torch, a left-hand blade)
    NotWeapon, // not a right-hand weapon attack: a kick, a visceral / riposte / backstab, an item
    Throw,     // the target is in a throw (a visceral, riposte or backstab: the game's own kill)
    Dead,      // the target had no HP left (already dying)
};

struct BlowIn {
    bool attacker_is_player{false};
    bool target_is_player{false};
    std::int32_t bullet{NoBullet};
    std::int32_t hand{HandRight};
    std::uint8_t category{0};
    bool target_in_throw{false};
    std::int32_t hp{0};   // the target's HP before this hit
    float damage{0.0f}; // the hit's final damage (the game truncates it into the HP store)
};

// Retail Stake Driver fully charged transformed R2; do not infer explosions from
// the equipped weapon or treat unrelated bullets / Molotovs as this attack.
inline bool IsStakeBlast(int behavior, int attack, int bullet, int hand, int category) {
    return behavior == 101100930 && attack == 1100930 && bullet == NoBullet &&
           hand == HandRight && category == CategoryRightWeapon;
}

// The HP the hit takes (the game truncates the final float).
inline std::int64_t HitDamage(float damage) {
    if (!std::isfinite(damage) || damage <= 0.0f) {
        return 0;
    }
    return static_cast<std::int64_t>(std::min(damage, 2.0e9f));
}

inline Blow ClassifyBlow(const BlowIn& b) {
    if (!b.attacker_is_player || b.target_is_player) {
        return Blow::NotPlayer;
    }
    if (b.bullet != NoBullet) {
        return Blow::Bullet;
    }
    if (b.hand != HandRight) {
        return Blow::LeftHand;
    }
    if (b.category != CategoryRightWeapon) {
        return Blow::NotWeapon;
    }
    if (b.target_in_throw) {
        return Blow::Throw;
    }
    if (b.hp <= 0) {
        return Blow::Dead;
    }
    return HitDamage(b.damage) >= b.hp ? Blow::Kill : Blow::NotLethal;
}

inline const char* BlowName(Blow b) {
    switch (b) {
    case Blow::Kill:
        return "a right-hand melee kill";
    case Blow::NotLethal:
        return "not lethal";
    case Blow::NotPlayer:
        return "not the hunter's";
    case Blow::Bullet:
        return "a bullet / explosion / thrown item";
    case Blow::LeftHand:
        return "the left hand";
    case Blow::NotWeapon:
        return "not a right-hand weapon attack (kick, visceral, backstab, item)";
    case Blow::Throw:
        return "the target is in a throw (visceral, riposte, backstab)";
    case Blow::Dead:
        return "the target was already dead";
    }
    return "?";
}

// ---- The death animation ---------------------------------------------------------------------------

enum class Death : std::uint8_t {
    Ragdoll,  // ends in the ragdoll (event 228): cut short
    Animated, // ends animated: kept
    Unknown,  // not a death animation of this chr's (yet)
};

struct DeathAnim {
    Death kind{Death::Unknown};
    float ragdoll_at_s{0.0f}; // where the game would have switched to the ragdoll
};

// One table lookup: `id` is the full TimeAct id.
inline bool FindDeath(std::int32_t chr_id, std::int32_t id, DeathAnim& out) {
    const std::uint64_t key =
        static_cast<std::uint64_t>(chr_id) * DeathKeyChr + static_cast<std::uint64_t>(id);
    const auto it =
        std::lower_bound(RagdollDeaths.begin(), RagdollDeaths.end(), key,
                         [](const RagdollDeath& d, std::uint64_t k) { return d.key < k; });
    if (it != RagdollDeaths.end() && it->key == key) {
        out = {Death::Ragdoll, static_cast<float>(it->start_ms) * 0.001f};
        return true;
    }
    if (std::binary_search(AnimatedDeaths.begin(), AnimatedDeaths.end(), key)) {
        out = {Death::Animated, 0.0f};
        return true;
    }
    return false;
}

// `anim_id` as the TimeAct ring holds it (TAE group * 1000000 + animation). Every TAE group's entry
// is its own: a variant whose own events have no 228 stays animated although another group's copy
// of the animation ends in the ragdoll (c4050 a003_002200, c1040 a000_002250, ...). Only an id of a
// group the chr's TAE does not have at all falls back to group 0's entry (what the game plays then).
inline DeathAnim ClassifyDeath(std::int32_t chr_id, std::int32_t anim_id) {
    if (chr_id <= 0 || chr_id > 9999 || anim_id < 0 ||
        static_cast<std::uint64_t>(anim_id) >= DeathKeyChr) {
        return {};
    }
    DeathAnim d{};
    if (FindDeath(chr_id, anim_id, d)) {
        return d;
    }
    if (anim_id >= 1000000 && FindDeath(chr_id, anim_id % 1000000, d)) {
        return d;
    }
    return {};
}

// True when the chr has any death animation that ends in the ragdoll.
inline bool ChrCanRagdoll(std::int32_t chr_id) {
    if (chr_id <= 0 || chr_id > 9999) {
        return false;
    }
    const std::uint64_t lo = static_cast<std::uint64_t>(chr_id) * DeathKeyChr;
    const auto it =
        std::lower_bound(RagdollDeaths.begin(), RagdollDeaths.end(), lo,
                         [](const RagdollDeath& d, std::uint64_t k) { return d.key < k; });
    return it != RagdollDeaths.end() && it->key < lo + DeathKeyChr;
}

// ---- Each of the enemy's updates after the kill ----------------------------------------------------

enum class Step : std::uint8_t {
    Wait,         // nothing decided yet
    Ragdoll,      // switch it to the ragdoll now
    KeepAlive,    // its HP is not 0 (an immortal or scripted enemy): the game's
    KeepKiller,   // the game credits someone else with the kill
    KeepAnimated, // its death animation ends animated
    KeepNoDeath,  // no death animation of its own came in time (a dissolve, an event death)
};

// The HP store follows the damage step in the same frame; an update that ran before it may still
// see the HP.
constexpr int AliveUpdates = 3;
// The death animation was in the ring 2 updates after the kill in every mock run; 10 at 30 Hz is
// 0.33 s.
constexpr int DeathWaitUpdates = 10;

// `updates`: the enemy's ragdoll updates since the kill, counting this one (1, 2, ...).
inline Step DecideStep(int updates, std::int32_t hp, bool killer_is_player, Death death) {
    if (hp > 0) {
        return updates >= AliveUpdates ? Step::KeepAlive : Step::Wait;
    }
    if (!killer_is_player) {
        return Step::KeepKiller;
    }
    switch (death) {
    case Death::Ragdoll:
        return Step::Ragdoll;
    case Death::Animated:
        return Step::KeepAnimated;
    case Death::Unknown:
        break;
    }
    return updates >= DeathWaitUpdates ? Step::KeepNoDeath : Step::Wait;
}

inline const char* StepName(Step s) {
    switch (s) {
    case Step::Wait:
        return "wait";
    case Step::Ragdoll:
        return "ragdoll at once";
    case Step::KeepAlive:
        return "it did not die (its HP came back: an immortal or scripted enemy)";
    case Step::KeepKiller:
        return "the game credits another attacker with the kill";
    case Step::KeepAnimated:
        return "its death animation ends animated (no ragdoll in it)";
    case Step::KeepNoDeath:
        return "no death animation of its own (a dissolve, an event death, a boss)";
    }
    return "?";
}

// ---- The chance of the game's own death (melee_ragdoll_vanilla_pct) ---------------------------------

// A kill that would go limp at once (the step above says Ragdoll) keeps the game's own death with
// this chance instead: its death animation plays to its end and the game's own ragdoll takes over
// there (event 228), nothing switched, no push. One roll per kill: drawn the first time the kill
// would go limp and kept for it (a second blow of a multi-hit attack on the same enemy, or another
// of its updates, never rolls again). 0 = always at once, 100 = never. 22 by default (about one
// kill in five; 15 from 2026-10-08, 22 since 2026-10-09).
constexpr std::int32_t DefaultVanillaPct = 22;

// splitmix64: small, fast, good enough for a die (every seed, 0 too, gives a full-period stream).
// The game side seeds one once at install; the tests seed their own.
class KillDice {
public:
    explicit KillDice(std::uint64_t seed = 0) : state_{seed} {}

    void Seed(std::uint64_t seed) {
        state_ = seed;
    }

    std::uint64_t Next() {
        ++draws_;
        std::uint64_t z = (state_ += 0x9E3779B97F4A7C15ull);
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }

    // Uniform in [0, 100) (the top 32 bits scaled: a bias below 1e-7).
    std::int32_t Percent() {
        return static_cast<std::int32_t>(((Next() >> 32) * 100ull) >> 32);
    }

    // How many numbers were drawn (the tests: one per kill).
    std::uint64_t Draws() const {
        return draws_;
    }

private:
    std::uint64_t state_;
    std::uint64_t draws_{0};
};

// One kill's roll.
struct VanillaRoll {
    bool rolled{false};
    bool vanilla{false}; // the game's own death
    std::int32_t pct{0}; // the chance it was rolled with (clamped)
};

// True when this kill keeps the game's own death. The first call draws (one number, whatever the
// chance); every later call for the same kill returns that answer.
inline bool RollVanilla(VanillaRoll& roll, std::int32_t pct, KillDice& dice) {
    if (!roll.rolled) {
        roll.rolled = true;
        roll.pct = std::clamp(pct, 0, 100);
        roll.vanilla = dice.Percent() < roll.pct;
    }
    return roll.vanilla;
}

// ---- The push ---------------------------------------------------------------------------------------

// The blade's velocity at `point` (world): the two ends' velocities, interpolated along the blade
// by where the point falls on it (a swing turning about the grip moves its tip fastest).
inline Vec3 BladeVelocityAt(Vec3 base, Vec3 tip, Vec3 base_vel, Vec3 tip_vel, Vec3 point) {
    const Vec3 axis = tip - base;
    const float len2 = VR::Body::LengthSq(axis);
    const float s =
        len2 > 1e-6f ? std::clamp(VR::Body::Dot(point - base, axis) / len2, 0.0f, 1.0f) : 1.0f;
    return base_vel + (tip_vel - base_vel) * s;
}

// The blade's speed at the hit. The game's blade is sampled once per game frame (a chord of the
// arc, and the arm's IK can lag the controller); the presenter measures the controller's tip at the
// headset's rate. The presenter's tip speed, scaled by the hit's share of the game blade's tip speed
// (where on the blade it struck), never below the game's own speed at the hit.
inline float HitSpeed(float game_hit, float game_tip, float presenter_tip) {
    if (!std::isfinite(game_hit) || game_hit < 0.0f) {
        return 0.0f;
    }
    if (!std::isfinite(game_tip) || game_tip < 0.5f || !std::isfinite(presenter_tip) ||
        presenter_tip <= 0.0f) {
        return game_hit;
    }
    const float share = std::clamp(game_hit / game_tip, 0.0f, 1.0f);
    return std::max(game_hit, presenter_tip * share);
}

// A conservative heavy-weapon bonus, captured at the killing hit. Transform attacks
// may strike with the previous form, so they deliberately keep ordinary momentum.
inline float HeavyTwoHandMomentum(int variation, bool form_b, bool two_handed, bool transform) {
    if (!form_b || !two_handed || transform) return 1.0f;
    switch (variation) {
    case 500: // Hunter Axe
    case 800: // Kirkhammer
    case 810: // Ludwig's Holy Blade
        return 1.5f;
    default: return 1.0f;
    }
}

struct PushConfig {
    float scale{1.0f};           // melee_ragdoll_impulse (0 = no push at all)
    float momentum_kg{31.25f};   // momentum = this x the blade's speed (250 kg m/s at 8 m/s)
    float speed_min{1.5f};       // the blade's speed used, m/s
    float speed_max{12.0f};
    float default_speed{5.0f};   // a blow whose blade was not tracked (R1 / R2 of the game's)
    float blade_min_speed{0.5f}; // a blade velocity this slow gives no direction
    float local_share{0.5f};     // of the momentum to the part struck (the rest to the whole body)
    float local_radius{0.6f};    // m: the bodies this near the hit are the part struck
    float dv_all_max{1.5f};      // m/s (x scale): the whole body's push at most
    float dv_local_max{3.0f};    // m/s (x scale): the part struck's extra at most
    // The motion the bodies carry into the ragdoll. The game switches at its death animation's end,
    // when the body is slow; at the animation's start (the mock's Scourge Beast: one limb at 11.8
    // m/s) a body faster than this is slowed to it, and its motion toward the hunter (a lunge or a
    // swipe it died in) is taken out, up to cancel_max: the corpse never falls onto the hunter.
    float inherit_max{5.0f};
    float cancel_max{5.0f};
    float fallback_lift{0.25f};  // the fallback direction's upward part (hunter -> enemy, a bit up)
    float down_max{0.6f};        // the direction's downward part at most (a chop into the floor)
    float up_max{0.8f};          // ... its upward part at most
    // The direction's horizontal part at most this far off "away from the hunter": a swing across
    // the enemy, or a backhand coming back toward him, still pushes it mostly sideways, but never
    // so far sideways that the falling corpse tips onto him. At range away_max_deg; an enemy
    // nearer than close_far_m narrows it, to away_max_close_deg at close_near_m and nearer.
    float away_max_deg{60.0f};
    float away_max_close_deg{35.0f};
    float close_far_m{1.5f};
    float close_near_m{0.8f};
};

enum class PushAttack { Normal, HeavyTwoHand, StakeBlast };

// These exceptions raise only the added-velocity caps. Direction, lift, inherited
// motion limits and the post-push player guard retain the normal rules.
inline PushConfig AttackPushConfig(float scale, PushAttack attack) {
    PushConfig cfg{};
    cfg.scale = scale;
    float cap = 1.0f;
    if (attack == PushAttack::HeavyTwoHand) {
        cfg.momentum_kg *= 1.5f;
        cap = 1.25f;
    } else if (attack == PushAttack::StakeBlast) {
        cfg.momentum_kg *= 1.75f;
        cap = 1.5f;
    }
    cfg.dv_all_max *= cap;
    cfg.dv_local_max *= cap;
    return cfg;
}

struct PushBody {
    Vec3 com{};
    Vec3 vel{};
    float mass{0.0f}; // kg (0: keyframed / unknown)
    bool dynamic{false};
};

struct PushIn {
    Vec3 hit{};    // where the blade hit (the damage result)
    Vec3 hunter{}; // the hunter's position at the hit
    Vec3 target{}; // the enemy's position at the hit
    bool blade_ok{false};
    Vec3 blade_vel{};          // the blade's velocity at the hit (m/s, world)
    float hit_speed{0.0f};     // its speed to use (HitSpeed; 0 = |blade_vel|)
    float fallback_speed{0.0f}; // without the blade: the swing's speed if known (0 = the default)
};

constexpr int MaxBodies = 64;

struct PushPlan {
    Vec3 dir{};          // the push's direction (unit)
    Vec3 away{};         // the hunter -> the enemy, horizontal (unit)
    bool from_blade{false};
    float distance{0.0f};    // the hunter -> the enemy, horizontal (m)
    float away_max_deg{0.0f}; // the direction's limit off "away" used
    float speed{0.0f};    // the blade's speed used (m/s)
    float momentum{0.0f}; // kg m/s
    float total_mass{0.0f};
    float local_mass{0.0f};
    float dv_all{0.0f};
    float dv_local{0.0f};
    int bodies{0};
    int dynamic{0};
    int local{0};
    int nearest{-1};
    float nearest_m{0.0f};
    float cancelled{0.0f}; // the largest motion toward the hunter taken out (m/s)
    float calmed{0.0f};    // the largest speed above inherit_max taken out (m/s)
    int calmed_bodies{0};
    std::array<Vec3, MaxBodies> impulse{}; // per body (kg m/s); zero for the ones not pushed
};

// The hunter -> the enemy, horizontal (unit).
inline Vec3 AwayFromHunter(const PushIn& in) {
    Vec3 away{in.target.x - in.hunter.x, 0.0f, in.target.z - in.hunter.z};
    if (VR::Body::LengthSq(away) < 0.05f * 0.05f || !VR::Body::IsFinite(away)) {
        away = {in.hit.x - in.hunter.x, 0.0f, in.hit.z - in.hunter.z};
    }
    return VR::Body::Normalize(away, Vec3{0.0f, 0.0f, 1.0f});
}

// The hunter -> the enemy, horizontal (m).
inline float HunterDistance(const PushIn& in) {
    const Vec3 d{in.target.x - in.hunter.x, 0.0f, in.target.z - in.hunter.z};
    return VR::Body::IsFinite(d) ? VR::Body::Length(d) : 0.0f;
}

// The limit of the push's horizontal direction off "away" (degrees) for an enemy this near.
inline float AwayLimitDeg(float distance, const PushConfig& c) {
    if (!std::isfinite(distance) || c.close_far_m <= c.close_near_m) {
        return c.away_max_close_deg;
    }
    const float t = std::clamp((distance - c.close_near_m) / (c.close_far_m - c.close_near_m),
                               0.0f, 1.0f);
    return c.away_max_close_deg + (c.away_max_deg - c.away_max_close_deg) * t;
}

// `h` (horizontal, unit) turned toward `away` until it is at most `max_deg` off it.
inline Vec3 LimitOffAway(Vec3 h, Vec3 away, float max_deg) {
    const float rad = std::clamp(max_deg, 0.0f, 180.0f) * 0.017453292519943295f;
    const float cmax = std::cos(rad);
    const float c = VR::Body::Dot(h, away);
    if (c >= cmax) {
        return h;
    }
    const Vec3 side = h - away * c;
    const float sl = VR::Body::Length(side);
    if (sl < 1e-4f) {
        return away; // straight at the hunter (or no side at all)
    }
    return away * cmax + side * (std::sin(rad) / sl);
}

// The push's direction: along the blade's velocity at the hit (else away from the hunter, a little
// up), never toward the hunter (that horizontal part is taken out) and at most AwayLimitDeg off
// "away", its downward / upward part limited.
inline Vec3 PushDirection(const PushIn& in, const PushConfig& c, Vec3 away, bool& from_blade,
                          float away_max_deg = 180.0f) {
    from_blade = in.blade_ok && VR::Body::IsFinite(in.blade_vel) &&
                 VR::Body::Length(in.blade_vel) >= c.blade_min_speed;
    Vec3 d = from_blade ? VR::Body::Normalize(in.blade_vel)
                        : VR::Body::Normalize(away + Vec3{0.0f, c.fallback_lift, 0.0f}, away);
    Vec3 h{d.x, 0.0f, d.z};
    const float toward = VR::Body::Dot(h, away);
    if (toward < 0.0f) {
        h = h - away * toward;
    }
    const float hl = VR::Body::Length(h);
    const float n = std::sqrt(hl * hl + d.y * d.y);
    if (n < 1e-3f) {
        return away; // the blade moved straight at the hunter
    }
    const float y = std::clamp(d.y / n, -c.down_max, c.up_max);
    const Vec3 hn = LimitOffAway(hl > 1e-3f ? h * (1.0f / hl) : away, away, away_max_deg);
    return hn * std::sqrt(std::max(0.0f, 1.0f - y * y)) + Vec3{0.0f, y, 0.0f};
}

inline PushPlan PlanPush(const PushIn& in, const PushBody* bodies, int count, const PushConfig& c) {
    PushPlan p{};
    count = std::clamp(count, 0, MaxBodies);
    p.bodies = count;
    p.away = AwayFromHunter(in);
    p.distance = HunterDistance(in);
    p.away_max_deg = AwayLimitDeg(p.distance, c);
    p.dir = PushDirection(in, c, p.away, p.from_blade, p.away_max_deg);
    const float raw_speed = p.from_blade ? (in.hit_speed > 0.0f ? in.hit_speed
                                                                : VR::Body::Length(in.blade_vel))
                            : in.fallback_speed > 0.0f ? in.fallback_speed
                                                       : c.default_speed;
    p.speed = std::clamp(std::isfinite(raw_speed) ? raw_speed : c.default_speed, c.speed_min,
                         c.speed_max);
    const float scale = std::isfinite(c.scale) ? std::max(c.scale, 0.0f) : 0.0f;
    p.momentum = c.momentum_kg * p.speed * scale;
    float nearest_d2 = 1e30f;
    for (int i = 0; i < count; ++i) {
        const PushBody& b = bodies[i];
        if (!b.dynamic || !(b.mass > 0.0f) || !std::isfinite(b.mass) || !VR::Body::IsFinite(b.com)) {
            continue;
        }
        ++p.dynamic;
        p.total_mass += b.mass;
        const float d2 = VR::Body::LengthSq(b.com - in.hit);
        if (d2 < nearest_d2) {
            nearest_d2 = d2;
            p.nearest = i;
        }
    }
    if (p.dynamic == 0) {
        return p; // nothing to push
    }
    p.nearest_m = std::sqrt(nearest_d2);
    std::array<bool, MaxBodies> local{};
    for (int i = 0; i < count; ++i) {
        const PushBody& b = bodies[i];
        if (!b.dynamic || !(b.mass > 0.0f) || !std::isfinite(b.mass) || !VR::Body::IsFinite(b.com)) {
            continue;
        }
        local[i] = i == p.nearest ||
                   VR::Body::LengthSq(b.com - in.hit) < c.local_radius * c.local_radius;
        if (local[i]) {
            ++p.local;
            p.local_mass += b.mass;
        }
    }
    // melee_ragdoll_impulse=0: no push (the motion it carries is still calmed).
    p.dv_all = scale > 0.0f ? std::min((1.0f - c.local_share) * p.momentum / p.total_mass,
                                       c.dv_all_max * scale)
                            : 0.0f;
    p.dv_local = scale > 0.0f && p.local_mass > 0.0f
                     ? std::min(c.local_share * p.momentum / p.local_mass, c.dv_local_max * scale)
                     : 0.0f;
    for (int i = 0; i < count; ++i) {
        const PushBody& b = bodies[i];
        if (!b.dynamic || !(b.mass > 0.0f) || !std::isfinite(b.mass) || !VR::Body::IsFinite(b.com)) {
            continue;
        }
        Vec3 dv = p.dir * (p.dv_all + (local[i] ? p.dv_local : 0.0f));
        // The motion it carries: slowed to inherit_max, its part toward the hunter (an enemy
        // killed mid-lunge hands its lunge to the ragdoll) taken out: the blow stopped it.
        if (VR::Body::IsFinite(b.vel)) {
            Vec3 v = b.vel;
            const float speed = VR::Body::Length(v);
            if (speed > c.inherit_max && speed > 0.0f) {
                v = v * (c.inherit_max / speed);
                p.calmed = std::max(p.calmed, speed - c.inherit_max);
                ++p.calmed_bodies;
            }
            const float vu = VR::Body::Dot(Vec3{v.x, 0.0f, v.z}, p.away);
            if (vu < 0.0f) {
                const float cancel = std::min(-vu, c.cancel_max);
                v += p.away * cancel;
                p.cancelled = std::max(p.cancelled, cancel);
            }
            dv += v - b.vel;
        }
        p.impulse[i] = dv * b.mass;
    }
    return p;
}

// ---- After the push: the corpse never falls onto the hunter ------------------------------------------

// For a short while after the push (the corpse tipping over under gravity: a beast killed at arm's
// length, leaning in, would fall onto the hunter and fill his view with fur), each of the enemy's
// updates takes the horizontal motion toward the hunter out of every body within `radius` of him.
struct GuardConfig {
    float radius{0.9f};     // m, horizontal: the bodies this near the hunter
    float duration_s{0.6f}; // after the push, and
    int min_updates{36};    // ... at least this many of the enemy's updates (0.6 s at 60 fps; a
                            // slowed game, e.g. the mock's dumps, still gets its game time)
    int max_updates{90};    // ... at most this many
};

// Whether the guard still runs `updates` updates / `seconds` after the push.
inline bool Guarding(const GuardConfig& g, int updates, float seconds) {
    return updates <= g.max_updates && (updates <= g.min_updates || seconds < g.duration_s);
}

// The impulse (kg m/s) that takes out the body's horizontal motion toward `hunter` when it is
// within `radius` of him (horizontally); zero otherwise. `removed`: that speed (m/s).
inline Vec3 GuardImpulse(const PushBody& b, Vec3 hunter, float radius, float* removed = nullptr) {
    if (removed != nullptr) {
        *removed = 0.0f;
    }
    if (!b.dynamic || !(b.mass > 0.0f) || !std::isfinite(b.mass) || !VR::Body::IsFinite(b.com) ||
        !VR::Body::IsFinite(b.vel) || !VR::Body::IsFinite(hunter)) {
        return {};
    }
    const Vec3 d{hunter.x - b.com.x, 0.0f, hunter.z - b.com.z};
    const float dist = VR::Body::Length(d);
    if (!(dist < radius) || dist < 1e-3f) {
        return {};
    }
    const Vec3 u = d * (1.0f / dist); // toward the hunter
    const float toward = VR::Body::Dot(Vec3{b.vel.x, 0.0f, b.vel.z}, u);
    if (!(toward > 0.0f)) {
        return {};
    }
    if (removed != nullptr) {
        *removed = toward;
    }
    return u * (-toward * b.mass);
}

} // namespace VR::Ragdoll
