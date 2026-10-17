// SPDX-License-Identifier: GPL-2.0-or-later

// vr.ini melee_kill_ragdoll / melee_ragdoll_impulse / melee_ragdoll_vanilla_pct
// (core/vr/ragdoll_kill.h): which blow counts as a right-hand melee kill, which death animation
// ends in the ragdoll (the generated table of the game's TAE event 228), what each of the enemy's
// updates after the kill does, the push, and the chance a kill keeps the game's own death.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/ragdoll_kill.h"

namespace {

namespace R = VR::Ragdoll;
using R::Vec3;

R::BlowIn Swing(std::int32_t hp, float damage) {
    R::BlowIn b{};
    b.attacker_is_player = true;
    b.bullet = R::NoBullet;
    b.hand = R::HandRight;
    b.category = R::CategoryRightWeapon;
    b.hp = hp;
    b.damage = damage;
    return b;
}

float Len(Vec3 v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

// ---- The blow ------------------------------------------------------------------------------------

TEST(RagdollKill, ALethalRightHandSwingIsAKill) {
    // The mock's kill: atk 700000 bid 100700000 hand -1 bullet -1 cat 1, hp 146.
    EXPECT_EQ(R::ClassifyBlow(Swing(146, 147.0f)), R::Blow::Kill);
    EXPECT_EQ(R::ClassifyBlow(Swing(146, 146.0f)), R::Blow::Kill); // exactly the HP left
    EXPECT_EQ(R::ClassifyBlow(Swing(146, 146.9f)), R::Blow::Kill);
}

TEST(RagdollKill, TheGameTruncatesTheDamage) {
    EXPECT_EQ(R::ClassifyBlow(Swing(146, 145.99f)), R::Blow::NotLethal);
    EXPECT_EQ(R::ClassifyBlow(Swing(146, 34.3f)), R::Blow::NotLethal);
    EXPECT_EQ(R::ClassifyBlow(Swing(146, NAN)), R::Blow::NotLethal);
    EXPECT_EQ(R::ClassifyBlow(Swing(146, -5.0f)), R::Blow::NotLethal);
    EXPECT_EQ(R::ClassifyBlow(Swing(146, INFINITY)), R::Blow::NotLethal);
    EXPECT_EQ(R::ClassifyBlow(Swing(2000000000, 3.0e9f)), R::Blow::Kill); // clamped, not UB
}

TEST(RagdollKill, BulletsFireThrownItemsAndTheLeftHandKeepTheDeathAnimation) {
    R::BlowIn b = Swing(10, 500.0f);
    b.bullet = 1000; // a pistol's bullet, a Molotov's fire, bolt paper's shot, a thrown knife
    EXPECT_EQ(R::ClassifyBlow(b), R::Blow::Bullet);
    b = Swing(10, 500.0f);
    b.hand = -2; // the left hand (the torch, a left-hand weapon)
    EXPECT_EQ(R::ClassifyBlow(b), R::Blow::LeftHand);
    b = Swing(10, 500.0f);
    b.attacker_is_player = false; // an enemy's blow, a trap, a fall
    EXPECT_EQ(R::ClassifyBlow(b), R::Blow::NotPlayer);
    b = Swing(10, 500.0f);
    b.target_is_player = true;
    EXPECT_EQ(R::ClassifyBlow(b), R::Blow::NotPlayer);
}

TEST(RagdollKill, KicksVisceralsAndBackstabsAreNotTheRightWeapon) {
    R::BlowIn b = Swing(10, 500.0f);
    b.category = 9; // a kick
    EXPECT_EQ(R::ClassifyBlow(b), R::Blow::NotWeapon);
    b.category = 5; // the common PC behaviours: the visceral / backstab rows (bid 504, atk 304)
    EXPECT_EQ(R::ClassifyBlow(b), R::Blow::NotWeapon);
    b.category = 2; // the left weapon's attack
    EXPECT_EQ(R::ClassifyBlow(b), R::Blow::NotWeapon);
    b.category = 0; // an item (no behaviour)
    EXPECT_EQ(R::ClassifyBlow(b), R::Blow::NotWeapon);
    b = Swing(10, 500.0f);
    b.target_in_throw = true; // in a visceral / riposte / backstab: the game's own kill
    EXPECT_EQ(R::ClassifyBlow(b), R::Blow::Throw);
}

TEST(RagdollKill, AnEnemyAlreadyDyingIsNotKilledAgain) {
    EXPECT_EQ(R::ClassifyBlow(Swing(0, 50.0f)), R::Blow::Dead);
    EXPECT_EQ(R::ClassifyBlow(Swing(-3, 50.0f)), R::Blow::Dead);
}

TEST(RagdollKill, EveryBlowAndStepHasAName) {
    for (int i = 0; i <= static_cast<int>(R::Blow::Dead); ++i) {
        EXPECT_NE(std::string(R::BlowName(static_cast<R::Blow>(i))), std::string("?"));
    }
    for (int i = 0; i <= static_cast<int>(R::Step::KeepNoDeath); ++i) {
        EXPECT_NE(std::string(R::StepName(static_cast<R::Step>(i))), std::string("?"));
    }
}

// ---- The death animation ---------------------------------------------------------------------------

TEST(RagdollKill, TheScourgeBeastsDeaths) {
    // c1000 a2200: event 228 at 1.70 s (the mock's vanilla death went limp 1.5 s after the kill).
    const R::DeathAnim a2200 = R::ClassifyDeath(1000, 2200);
    EXPECT_EQ(a2200.kind, R::Death::Ragdoll);
    EXPECT_NEAR(a2200.ragdoll_at_s, 1.70f, 1e-3f);
    EXPECT_EQ(R::ClassifyDeath(1000, 2205).kind, R::Death::Ragdoll);
    EXPECT_NEAR(R::ClassifyDeath(1000, 2205).ragdoll_at_s, 1.133f, 1e-3f);
    EXPECT_EQ(R::ClassifyDeath(1000, 2250).kind, R::Death::Ragdoll); // limp from 0 already
    EXPECT_NEAR(R::ClassifyDeath(1000, 2250).ragdoll_at_s, 0.0f, 1e-6f);
    // a2202 / a2203 have no events of their own: they import a2200 / a2205 (TAE ImportOtherAnim)
    // and end as those do.
    EXPECT_EQ(R::ClassifyDeath(1000, 2202).kind, R::Death::Ragdoll);
    EXPECT_NEAR(R::ClassifyDeath(1000, 2202).ragdoll_at_s, 1.70f, 1e-3f);
    EXPECT_EQ(R::ClassifyDeath(1000, 2203).kind, R::Death::Ragdoll);
    EXPECT_NEAR(R::ClassifyDeath(1000, 2203).ragdoll_at_s, 1.133f, 1e-3f);
    // The hit reaction before the death (3003, 3011) is no death.
    EXPECT_EQ(R::ClassifyDeath(1000, 3003).kind, R::Death::Unknown);
    EXPECT_EQ(R::ClassifyDeath(1000, 3011).kind, R::Death::Unknown);
    // The visceral's own deaths (a2501 / a2511 after the throw) are not in the table: kept.
    EXPECT_EQ(R::ClassifyDeath(1000, 2501).kind, R::Death::Unknown);
}

TEST(RagdollKill, AGroupTheChrDoesNotHaveFallsBackToGroupZero) {
    // c1000's TAE has group 0 only.
    EXPECT_EQ(R::ClassifyDeath(1000, 2200 + 1000000).kind, R::Death::Ragdoll);
    EXPECT_EQ(R::ClassifyDeath(1000, 2202 + 3000000).kind, R::Death::Ragdoll);
    EXPECT_EQ(R::ClassifyDeath(1000, -1).kind, R::Death::Unknown);
    EXPECT_EQ(R::ClassifyDeath(1000, 12200).kind, R::Death::Unknown);
    EXPECT_EQ(R::ClassifyDeath(1000, 100002200).kind, R::Death::Unknown);
}

TEST(RagdollKill, EveryTaeGroupsVariantIsItsOwn) {
    // Variants whose own events have no 228 although another group's copy has it (the skeptic's
    // scan of every chr TAE): never cut short.
    struct Case {
        std::int32_t chr, anim;
        R::Death kind;
    };
    const Case cases[] = {
        {4050, 2200, R::Death::Ragdoll},
        {4050, 3002200, R::Death::Animated}, // 20 events of its own, none 228
        {4050, 3002205, R::Death::Animated}, // imports a003_002200
        {4120, 2205, R::Death::Ragdoll},
        {4120, 3002205, R::Death::Animated},
        {4120, 3002207, R::Death::Animated},
        {3030, 1610, R::Death::Ragdoll},
        {3030, 1001610, R::Death::Animated},
        {3030, 1001620, R::Death::Animated},
        {4020, 2209, R::Death::Ragdoll},
        {4020, 3002209, R::Death::Animated},
        {4020, 3002211, R::Death::Animated},
        {3010, 2209, R::Death::Ragdoll},
        {3010, 1002209, R::Death::Animated},
        {1170, 2211, R::Death::Ragdoll},
        {1170, 1002211, R::Death::Animated},
        {1040, 2250, R::Death::Animated}, // group 0 has no 228, only group 1
        {1040, 1002250, R::Death::Ragdoll},
    };
    for (const Case& c : cases) {
        EXPECT_EQ(R::ClassifyDeath(c.chr, c.anim).kind, c.kind) << c.chr << " a" << c.anim;
    }
}

TEST(RagdollKill, AnImportingVariantTakesItsTargetsEnd) {
    // c1020 a001_002252: no events, imports a001_002250 (228 at 0): limp at once.
    EXPECT_EQ(R::ClassifyDeath(1020, 1002252).kind, R::Death::Ragdoll);
    EXPECT_EQ(R::ClassifyDeath(1020, 1002250).kind, R::Death::Ragdoll);
    // a000_002252 imports a000_002250 but has an event of its own without 228: when unsure, the
    // game's own.
    EXPECT_EQ(R::ClassifyDeath(1020, 2252).kind, R::Death::Animated);
}

TEST(RagdollKill, BossesDissolvesAndNpcHuntersNeverRagdoll) {
    for (const std::int32_t boss : {5000, 5010, 5020}) {
        EXPECT_FALSE(R::ChrCanRagdoll(boss)) << boss;
        EXPECT_EQ(R::ClassifyDeath(boss, 2200).kind, R::Death::Animated) << boss;
    }
    EXPECT_FALSE(R::ChrCanRagdoll(1110)); // no event 228 in any of its deaths
    EXPECT_EQ(R::ClassifyDeath(1110, 2200).kind, R::Death::Animated);
    EXPECT_FALSE(R::ChrCanRagdoll(0)); // the hunter, NPC hunters (c0000): no table rows at all
    EXPECT_EQ(R::ClassifyDeath(0, 2200).kind, R::Death::Unknown);
    EXPECT_FALSE(R::ChrCanRagdoll(-1));
    EXPECT_FALSE(R::ChrCanRagdoll(10000));
    EXPECT_TRUE(R::ChrCanRagdoll(1000));
    EXPECT_TRUE(R::ChrCanRagdoll(1010));
}

TEST(RagdollKill, TheTablesAreSortedAndDisjoint) {
    for (std::size_t i = 1; i < R::RagdollDeaths.size(); ++i) {
        ASSERT_LT(R::RagdollDeaths[i - 1].key, R::RagdollDeaths[i].key);
    }
    for (std::size_t i = 1; i < R::AnimatedDeaths.size(); ++i) {
        ASSERT_LT(R::AnimatedDeaths[i - 1], R::AnimatedDeaths[i]);
    }
    for (const R::RagdollDeath& d : R::RagdollDeaths) {
        EXPECT_FALSE(std::binary_search(R::AnimatedDeaths.begin(), R::AnimatedDeaths.end(), d.key));
    }
}

// ---- Each update after the kill ---------------------------------------------------------------------

TEST(RagdollKill, ItWaitsForTheDeathAnimationThenCutsIt) {
    // The mock: the death animation in the ring 2 updates after the kill.
    EXPECT_EQ(R::DecideStep(1, 0, true, R::Death::Unknown), R::Step::Wait);
    EXPECT_EQ(R::DecideStep(2, 0, true, R::Death::Ragdoll), R::Step::Ragdoll);
    EXPECT_EQ(R::DecideStep(2, 0, true, R::Death::Animated), R::Step::KeepAnimated);
}

TEST(RagdollKill, AnHpStoreLaterThanTheUpdateIsWaitedFor) {
    EXPECT_EQ(R::DecideStep(1, 12, true, R::Death::Unknown), R::Step::Wait);
    EXPECT_EQ(R::DecideStep(2, 12, true, R::Death::Unknown), R::Step::Wait);
    EXPECT_EQ(R::DecideStep(R::AliveUpdates, 1, true, R::Death::Unknown), R::Step::KeepAlive);
}

TEST(RagdollKill, OnlyTheHuntersKill) {
    EXPECT_EQ(R::DecideStep(2, 0, false, R::Death::Ragdoll), R::Step::KeepKiller);
}

TEST(RagdollKill, NoDeathAnimationInTimeKeepsTheGames) {
    for (int u = 1; u < R::DeathWaitUpdates; ++u) {
        EXPECT_EQ(R::DecideStep(u, 0, true, R::Death::Unknown), R::Step::Wait) << u;
    }
    EXPECT_EQ(R::DecideStep(R::DeathWaitUpdates, 0, true, R::Death::Unknown),
              R::Step::KeepNoDeath);
}

// ---- The push ----------------------------------------------------------------------------------------

TEST(RagdollKill, TheBladesVelocityAtTheHit) {
    const Vec3 base{0, 1, 0}, tip{0, 2, 0};
    const Vec3 vb{0, 0, 2}, vt{0, 0, 10};
    const Vec3 mid = R::BladeVelocityAt(base, tip, vb, vt, {0.1f, 1.5f, 0});
    EXPECT_NEAR(mid.z, 6.0f, 1e-4f);
    EXPECT_NEAR(R::BladeVelocityAt(base, tip, vb, vt, {0, 3, 0}).z, 10.0f, 1e-4f); // past the tip
    EXPECT_NEAR(R::BladeVelocityAt(base, tip, vb, vt, {0, 0, 0}).z, 2.0f, 1e-4f);  // below the grip
    EXPECT_NEAR(R::BladeVelocityAt(base, base, vb, vt, {0, 5, 0}).z, 10.0f, 1e-4f); // no length
}

// The arena's Scourge Beast as the mock read it: 25 bodies, 154.8 kg; the hit 0.12 m from body 19.
struct Beast {
    std::vector<R::PushBody> bodies;
    R::PushIn in;
};

Beast ScourgeBeast() {
    Beast b{};
    for (int i = 0; i < 25; ++i) {
        R::PushBody body{};
        body.mass = i < 3 ? 5.0f : (154.8f - 15.0f) / 22.0f;
        // Bodies 0-2 at the hit, the rest 1 .. 2 m off it.
        body.com = i < 3 ? Vec3{0.05f * static_cast<float>(i), 1.2f, 2.0f}
                         : Vec3{1.0f + 0.05f * static_cast<float>(i), 1.0f, 2.5f};
        body.dynamic = true;
        b.bodies.push_back(body);
    }
    b.in.hit = {0.0f, 1.2f, 2.0f};
    b.in.hunter = {0.0f, 1.0f, 0.0f};
    b.in.target = {0.0f, 0.0f, 2.3f};
    b.in.blade_ok = true;
    b.in.blade_vel = {6.2f, 0.0f, 0.0f}; // a level swing to the right, across the beast
    return b;
}

TEST(RagdollKill, TheHitsSpeedTakesThePresentersTip) {
    // i2 (the mock): the game's blade 3.4 m/s at the hit, the presenter's tip 10.1 m/s.
    EXPECT_NEAR(R::HitSpeed(3.4f, 6.8f, 10.1f), 5.05f, 1e-3f); // struck halfway along the blade
    EXPECT_NEAR(R::HitSpeed(3.4f, 3.4f, 10.1f), 10.1f, 1e-3f); // at the tip
    EXPECT_NEAR(R::HitSpeed(6.0f, 5.0f, 4.0f), 6.0f, 1e-3f);   // never below the game's own
    EXPECT_NEAR(R::HitSpeed(3.4f, 0.2f, 10.1f), 3.4f, 1e-3f);  // no tip motion: the game's
    EXPECT_NEAR(R::HitSpeed(3.4f, 6.8f, 0.0f), 3.4f, 1e-3f);   // no presenter tip: the game's
    EXPECT_NEAR(R::HitSpeed(NAN, 6.8f, 10.0f), 0.0f, 1e-6f);
    Beast b = ScourgeBeast();
    b.in.hit_speed = 9.0f; // the blade's direction, this speed
    EXPECT_NEAR(R::PlanPush(b.in, b.bodies.data(), 25, R::PushConfig{}).speed, 9.0f, 1e-4f);
}

TEST(RagdollKill, TheMockRunsPushIsTheDefault) {
    // r3_direct_cap: p 194 kg m/s at 6.2 m/s -> 0.63 m/s on the whole body, +3 m/s (the cap) on
    // the 15 kg struck, which felt physical.
    Beast b = ScourgeBeast();
    const R::PushPlan p = R::PlanPush(b.in, b.bodies.data(), 25, R::PushConfig{});
    EXPECT_TRUE(p.from_blade);
    EXPECT_NEAR(p.speed, 6.2f, 1e-4f);
    EXPECT_NEAR(p.momentum, 193.75f, 0.01f);
    EXPECT_NEAR(p.total_mass, 154.8f, 0.01f);
    EXPECT_EQ(p.dynamic, 25);
    EXPECT_EQ(p.local, 3);
    EXPECT_NEAR(p.local_mass, 15.0f, 1e-3f);
    EXPECT_NEAR(p.dv_all, 0.5f * 193.75f / 154.8f, 1e-3f); // 0.63
    EXPECT_NEAR(p.dv_local, 3.0f, 1e-4f);
    // Along the blade (across the beast, 90 deg off "away"), turned to 60 deg off it (2.3 m off).
    EXPECT_NEAR(p.distance, 2.3f, 1e-4f);
    EXPECT_NEAR(p.away_max_deg, 60.0f, 1e-4f);
    EXPECT_NEAR(p.dir.x, std::sin(60.0f * 0.0174533f), 1e-4f);
    EXPECT_NEAR(p.dir.z, 0.5f, 1e-4f);
    // dv = J / m, per body.
    EXPECT_NEAR(Len(p.impulse[0]) / 5.0f, p.dv_all + p.dv_local, 1e-4f);
    EXPECT_NEAR(Len(p.impulse[10]) / b.bodies[10].mass, p.dv_all, 1e-4f);
    // Never more than the blow's momentum.
    float sum = 0.0f;
    for (int i = 0; i < 25; ++i) {
        sum += Len(p.impulse[i]);
    }
    EXPECT_LE(sum, p.momentum + 1e-2f);
}

TEST(RagdollKill, AFasterSwingPushesHarderUpToTheCaps) {
    Beast b = ScourgeBeast();
    b.in.blade_vel = {3.0f, 0.0f, 0.0f};
    const R::PushPlan slow = R::PlanPush(b.in, b.bodies.data(), 25, R::PushConfig{});
    b.in.blade_vel = {9.0f, 0.0f, 0.0f};
    const R::PushPlan fast = R::PlanPush(b.in, b.bodies.data(), 25, R::PushConfig{});
    EXPECT_GT(fast.dv_all, slow.dv_all * 2.5f);
    b.in.blade_vel = {40.0f, 0.0f, 0.0f}; // a tracking glitch
    const R::PushPlan glitch = R::PlanPush(b.in, b.bodies.data(), 25, R::PushConfig{});
    EXPECT_NEAR(glitch.speed, R::PushConfig{}.speed_max, 1e-4f);
    EXPECT_LE(glitch.dv_all, R::PushConfig{}.dv_all_max + 1e-4f);
    EXPECT_LE(glitch.dv_local, R::PushConfig{}.dv_local_max + 1e-4f);
}

TEST(RagdollKill, ALightEnemyIsCappedNotLaunched) {
    std::vector<R::PushBody> bodies(12);
    for (int i = 0; i < 12; ++i) {
        bodies[i] = {{0.1f * static_cast<float>(i), 1.0f, 2.0f}, {}, 2.0f, true}; // 24 kg
    }
    R::PushIn in{};
    in.hit = {0, 1, 2};
    in.hunter = {0, 1, 0};
    in.target = {0, 0, 2};
    in.blade_ok = true;
    in.blade_vel = {0, 0, 12};
    const R::PushPlan p = R::PlanPush(in, bodies.data(), 12, R::PushConfig{});
    EXPECT_NEAR(p.dv_all, 1.5f, 1e-4f);
    EXPECT_NEAR(p.dv_local, 3.0f, 1e-4f);
    for (int i = 0; i < 12; ++i) {
        EXPECT_LE(Len(p.impulse[i]) / 2.0f, 4.5f + 1e-3f);
    }
}

TEST(RagdollKill, ThePushNeverGoesTowardTheHunter) {
    Beast b = ScourgeBeast();
    b.in.blade_vel = {3.0f, 0.0f, -6.0f}; // a backhand pulling toward the hunter (and right)
    const R::PushPlan p = R::PlanPush(b.in, b.bodies.data(), 25, R::PushConfig{});
    EXPECT_GE(p.dir.x * p.away.x + p.dir.z * p.away.z, 0.5f - 1e-4f); // at most 60 deg off away
    EXPECT_GT(p.dir.x, 0.85f); // its sideways part is kept
    b.in.blade_vel = {0.0f, 0.0f, -8.0f}; // straight at the hunter
    const R::PushPlan q = R::PlanPush(b.in, b.bodies.data(), 25, R::PushConfig{});
    EXPECT_NEAR(q.dir.z, q.away.z, 1e-4f); // then away from him
}

TEST(RagdollKill, AChopIntoTheFloorKeepsSomeForwardPush) {
    Beast b = ScourgeBeast();
    b.in.blade_vel = {0.0f, -9.0f, 0.0f};
    const R::PushPlan p = R::PlanPush(b.in, b.bodies.data(), 25, R::PushConfig{});
    EXPECT_NEAR(p.dir.y, -R::PushConfig{}.down_max, 1e-4f);
    EXPECT_NEAR(Len(p.dir), 1.0f, 1e-4f);
    EXPECT_GT(p.dir.x * p.away.x + p.dir.z * p.away.z, 0.7f);
    b.in.blade_vel = {0.0f, 9.0f, 0.0f}; // an uppercut is not a launch either
    const R::PushPlan q = R::PlanPush(b.in, b.bodies.data(), 25, R::PushConfig{});
    EXPECT_NEAR(q.dir.y, R::PushConfig{}.up_max, 1e-4f);
}

TEST(RagdollKill, WithoutTheBladeItPushesAwayAndALittleUp) {
    Beast b = ScourgeBeast();
    b.in.blade_ok = false;
    const R::PushPlan p = R::PlanPush(b.in, b.bodies.data(), 25, R::PushConfig{});
    EXPECT_FALSE(p.from_blade);
    EXPECT_NEAR(p.speed, R::PushConfig{}.default_speed, 1e-4f);
    EXPECT_GT(p.dir.z, 0.9f);
    EXPECT_GT(p.dir.y, 0.1f);
    b.in.fallback_speed = 7.0f; // the presenter's tip speed
    EXPECT_NEAR(R::PlanPush(b.in, b.bodies.data(), 25, R::PushConfig{}).speed, 7.0f, 1e-4f);
    b.in.blade_ok = true;
    b.in.blade_vel = {0.1f, 0.0f, 0.1f}; // too slow to give a direction
    EXPECT_FALSE(R::PlanPush(b.in, b.bodies.data(), 25, R::PushConfig{}).from_blade);
}

TEST(RagdollKill, ALungeTowardTheHunterIsStopped) {
    Beast b = ScourgeBeast();
    for (R::PushBody& body : b.bodies) {
        body.vel = {0.0f, 0.0f, -1.8f}; // r1: the beast lunging at the hunter at 1.8 m/s
    }
    b.bodies[5].vel = {0.0f, 0.0f, -5.0f};
    const R::PushPlan p = R::PlanPush(b.in, b.bodies.data(), 25, R::PushConfig{});
    // body 10: the whole body's push (dir) and its lunge taken out (+z).
    EXPECT_NEAR(p.impulse[10].z / b.bodies[10].mass, 1.8f + p.dir.z * p.dv_all, 1e-3f);
    EXPECT_NEAR(p.impulse[5].z / b.bodies[5].mass,
                R::PushConfig{}.cancel_max + p.dir.z * p.dv_all, 1e-3f);
    EXPECT_NEAR(p.cancelled, R::PushConfig{}.cancel_max, 1e-4f);
    // Moving away already: nothing taken out.
    for (R::PushBody& body : b.bodies) {
        body.vel = {0.0f, 0.0f, 1.0f};
    }
    const R::PushPlan q = R::PlanPush(b.in, b.bodies.data(), 25, R::PushConfig{});
    EXPECT_NEAR(q.impulse[10].z / b.bodies[10].mass, q.dir.z * q.dv_all, 1e-4f);
    EXPECT_NEAR(q.cancelled, 0.0f, 1e-6f);
}

TEST(RagdollKill, AFlailingLimbIsCalmed) {
    // i1 (the mock): the death animation's start swung one of the beast's limbs at 11.8 m/s; the
    // game's own switch at the animation's end sees a slow body.
    Beast b = ScourgeBeast();
    b.bodies[10].vel = {12.0f, 0.0f, 0.0f};  // sideways
    b.bodies[11].vel = {0.0f, -9.0f, 0.0f};  // down
    b.bodies[12].vel = {0.0f, 0.0f, -12.0f}; // at the hunter
    b.bodies[13].vel = {0.0f, 0.0f, 4.0f};   // away, slow: kept
    const R::PushPlan p = R::PlanPush(b.in, b.bodies.data(), 25, R::PushConfig{});
    const float cap = R::PushConfig{}.inherit_max;
    EXPECT_NEAR(p.impulse[10].x / b.bodies[10].mass, cap - 12.0f + p.dir.x * p.dv_all, 1e-3f);
    EXPECT_NEAR(p.impulse[11].y / b.bodies[11].mass, 9.0f - cap, 1e-3f);
    // capped, then stopped
    EXPECT_NEAR(p.impulse[12].z / b.bodies[12].mass, 12.0f + p.dir.z * p.dv_all, 1e-3f);
    EXPECT_NEAR(p.impulse[13].z / b.bodies[13].mass, p.dir.z * p.dv_all, 1e-4f);
    EXPECT_EQ(p.calmed_bodies, 3);
    EXPECT_NEAR(p.calmed, 12.0f - cap, 1e-4f);
    // Without a push (melee_ragdoll_impulse=0) the motion is still calmed.
    R::PushConfig none{};
    none.scale = 0.0f;
    const R::PushPlan q = R::PlanPush(b.in, b.bodies.data(), 25, none);
    EXPECT_NEAR(q.impulse[10].x / b.bodies[10].mass, cap - 12.0f, 1e-3f);
    EXPECT_NEAR(Len(q.impulse[0]), 0.0f, 1e-6f);
}

TEST(RagdollKill, AtArmsLengthThePushTurnsFurtherAway) {
    // v1 / i2 (the mock): the beast killed 0.7 m off, a swing across it (or a backhand coming
    // back): the corpse tipped onto the hunter. Nearer than 1.5 m the limit narrows to 35 deg.
    const R::PushConfig c{};
    EXPECT_NEAR(R::AwayLimitDeg(3.0f, c), 60.0f, 1e-4f);
    EXPECT_NEAR(R::AwayLimitDeg(1.5f, c), 60.0f, 1e-4f);
    EXPECT_NEAR(R::AwayLimitDeg(1.15f, c), 47.5f, 1e-3f);
    EXPECT_NEAR(R::AwayLimitDeg(0.8f, c), 35.0f, 1e-4f);
    EXPECT_NEAR(R::AwayLimitDeg(0.2f, c), 35.0f, 1e-4f);
    Beast b = ScourgeBeast();
    b.in.target = {0.0f, 0.0f, 0.7f};
    b.in.blade_vel = {6.0f, 0.0f, -2.0f}; // v1: across and back toward the hunter
    const R::PushPlan p = R::PlanPush(b.in, b.bodies.data(), 25, c);
    EXPECT_NEAR(p.away_max_deg, 35.0f, 1e-4f);
    EXPECT_NEAR(p.dir.x * p.away.x + p.dir.z * p.away.z, std::cos(35.0f * 0.0174533f), 1e-4f);
    EXPECT_GT(p.dir.x, 0.5f); // still to the blade's side
    b.in.blade_vel = {-6.0f, 0.0f, -2.0f}; // v2: the other side
    const R::PushPlan q = R::PlanPush(b.in, b.bodies.data(), 25, c);
    EXPECT_LT(q.dir.x, -0.5f);
    // A blow within the limit is untouched.
    b.in.blade_vel = {2.0f, 0.0f, 6.0f};
    const R::PushPlan r = R::PlanPush(b.in, b.bodies.data(), 25, c);
    EXPECT_NEAR(r.dir.x, 2.0f / std::sqrt(40.0f), 1e-4f);
    EXPECT_NEAR(r.dir.z, 6.0f / std::sqrt(40.0f), 1e-4f);
}

TEST(RagdollKill, TheGuardKeepsTheCorpseOffTheHunter) {
    const Vec3 hunter{0.0f, 0.0f, 0.0f};
    const float radius = R::GuardConfig{}.radius;
    float removed = -1.0f;
    // 0.5 m off, tipping toward him at 3.8 m/s (v1 at u32) and falling: the horizontal part toward
    // him is taken out, the fall and the sideways motion are kept.
    R::PushBody b{{0.0f, 0.8f, 0.5f}, {1.0f, -2.0f, -3.8f}, 10.0f, true};
    const Vec3 j = R::GuardImpulse(b, hunter, radius, &removed);
    EXPECT_NEAR(removed, 3.8f, 1e-4f);
    EXPECT_NEAR(j.z, 38.0f, 1e-3f);
    EXPECT_NEAR(j.x, 0.0f, 1e-5f);
    EXPECT_NEAR(j.y, 0.0f, 1e-5f);
    // Moving away from him, or beyond the radius, or keyframed, or broken: nothing.
    b.vel = {0.0f, -2.0f, 1.0f};
    EXPECT_NEAR(Len(R::GuardImpulse(b, hunter, radius, &removed)), 0.0f, 1e-6f);
    EXPECT_NEAR(removed, 0.0f, 1e-6f);
    b.vel = {0.0f, 0.0f, -3.0f};
    b.com = {0.0f, 0.8f, 1.2f};
    EXPECT_NEAR(Len(R::GuardImpulse(b, hunter, radius)), 0.0f, 1e-6f);
    b.com = {0.0f, 0.8f, 0.5f};
    b.dynamic = false;
    EXPECT_NEAR(Len(R::GuardImpulse(b, hunter, radius)), 0.0f, 1e-6f);
    b.dynamic = true;
    b.vel = {NAN, 0.0f, 0.0f};
    EXPECT_NEAR(Len(R::GuardImpulse(b, hunter, radius)), 0.0f, 1e-6f);
    // Diagonal: only the part along the line to him.
    b.com = {0.3f, 1.0f, 0.4f};
    b.vel = {-3.0f, 0.0f, 0.0f};
    const Vec3 k = R::GuardImpulse(b, hunter, radius, &removed);
    EXPECT_NEAR(removed, 3.0f * 0.6f, 1e-4f);
    const Vec3 after{b.vel.x + k.x / b.mass, 0.0f, b.vel.z + k.z / b.mass};
    EXPECT_NEAR(after.x * -0.6f + after.z * -0.8f, 0.0f, 1e-4f);
}

TEST(RagdollKill, TheGuardLastsItsGameTime) {
    const R::GuardConfig g{};
    EXPECT_TRUE(R::Guarding(g, 1, 0.0f));
    EXPECT_TRUE(R::Guarding(g, 36, 0.6f));   // 60 fps: 0.6 s
    EXPECT_FALSE(R::Guarding(g, 37, 0.62f));
    EXPECT_TRUE(R::Guarding(g, 30, 2.0f));   // a slowed game (the mock's dumps): 36 updates
    EXPECT_TRUE(R::Guarding(g, 50, 0.5f));   // a fast one: 0.6 s
    EXPECT_FALSE(R::Guarding(g, 91, 0.1f));  // never more than 90 updates
}

TEST(RagdollKill, KeyframedBodiesAreNotPushed) {
    Beast b = ScourgeBeast();
    b.bodies[0].dynamic = false;
    b.bodies[1].mass = 0.0f;
    const R::PushPlan p = R::PlanPush(b.in, b.bodies.data(), 25, R::PushConfig{});
    EXPECT_EQ(p.dynamic, 23);
    EXPECT_NEAR(Len(p.impulse[0]), 0.0f, 1e-6f);
    EXPECT_NEAR(Len(p.impulse[1]), 0.0f, 1e-6f);
    EXPECT_EQ(p.nearest, 2);
}

TEST(RagdollKill, TheImpulseScale) {
    Beast b = ScourgeBeast();
    R::PushConfig c{};
    c.scale = 0.0f; // melee_ragdoll_impulse=0: the game's ragdoll as it falls
    const R::PushPlan none = R::PlanPush(b.in, b.bodies.data(), 25, c);
    for (int i = 0; i < 25; ++i) {
        EXPECT_NEAR(Len(none.impulse[i]), 0.0f, 1e-6f);
    }
    c.scale = 2.0f;
    const R::PushPlan twice = R::PlanPush(b.in, b.bodies.data(), 25, c);
    const R::PushPlan once = R::PlanPush(b.in, b.bodies.data(), 25, R::PushConfig{});
    EXPECT_NEAR(twice.dv_all, 2.0f * once.dv_all, 1e-4f);
    EXPECT_NEAR(twice.dv_local, 6.0f, 1e-4f); // the cap scales too
}

// ---- The chance of the game's own death (melee_ragdoll_vanilla_pct) ---------------------------------

// `kills` kills, each with its own roll, from one die seeded once: how many keep the game's death.
int KeptOf(int kills, std::int32_t pct, std::uint64_t seed) {
    R::KillDice dice{seed};
    int kept = 0;
    for (int k = 0; k < kills; ++k) {
        R::VanillaRoll roll{};
        kept += R::RollVanilla(roll, pct, dice) ? 1 : 0;
    }
    return kept;
}

TEST(RagdollKill, TheChanceZeroNeverKeepsTheDeath) {
    // melee_ragdoll_vanilla_pct=0: every kill that would go limp does (the behaviour before it).
    EXPECT_EQ(KeptOf(10000, 0, 1), 0);
    EXPECT_EQ(KeptOf(10000, 0, 0xDEADBEEFull), 0);
    EXPECT_EQ(KeptOf(1000, -20, 7), 0); // clamped to 0
}

TEST(RagdollKill, TheChanceHundredAlwaysKeepsTheDeath) {
    EXPECT_EQ(KeptOf(10000, 100, 1), 10000);
    EXPECT_EQ(KeptOf(10000, 100, 0xDEADBEEFull), 10000);
    EXPECT_EQ(KeptOf(1000, 250, 7), 1000); // clamped to 100
}

TEST(RagdollKill, TheDefaultChanceIsTwentyTwoPercent) {
    // 22% since 2026-10-09 (about one kill in five; 15% the day before).
    EXPECT_EQ(R::DefaultVanillaPct, 22);
    // 10000 seeded kills at 22%: within +-1.5% (150 kills; one sigma is 41), for several seeds
    // (seed 0 too: splitmix64 has no bad seed).
    const std::uint64_t seeds[] = {0ull, 1ull, 42ull, 0x123456789ABCDEFull, ~0ull};
    for (const std::uint64_t seed : seeds) {
        const int kept = KeptOf(10000, R::DefaultVanillaPct, seed);
        EXPECT_GE(kept, 2050) << "seed " << seed;
        EXPECT_LE(kept, 2350) << "seed " << seed;
    }
}

TEST(RagdollKill, TheChanceFifteenKeepsFifteenPercent) {
    // The default until 2026-10-09: 10000 seeded kills at 15% within +-1.5% (150 kills; one sigma
    // is 36), for several seeds.
    const std::uint64_t seeds[] = {0ull, 1ull, 42ull, 0x123456789ABCDEFull, ~0ull};
    for (const std::uint64_t seed : seeds) {
        const int kept = KeptOf(10000, 15, seed);
        EXPECT_GE(kept, 1350) << "seed " << seed;
        EXPECT_LE(kept, 1650) << "seed " << seed;
    }
    // Other chances land where they say.
    EXPECT_NEAR(KeptOf(10000, 50, 3) / 10000.0, 0.50, 0.02);
    EXPECT_NEAR(KeptOf(10000, 1, 3) / 10000.0, 0.01, 0.005);
    EXPECT_NEAR(KeptOf(10000, 99, 3) / 10000.0, 0.99, 0.005);
}

TEST(RagdollKill, TheDieIsUniformAndSeeded) {
    R::KillDice a{9};
    R::KillDice b{9};
    R::KillDice c{10};
    std::vector<int> counts(100, 0);
    int same_b = 0;
    int same_c = 0;
    for (int k = 0; k < 100000; ++k) {
        const std::int32_t x = a.Percent();
        ASSERT_GE(x, 0);
        ASSERT_LT(x, 100);
        ++counts[x];
        same_b += x == b.Percent() ? 1 : 0;
        same_c += x == c.Percent() ? 1 : 0;
    }
    EXPECT_EQ(same_b, 100000); // the same seed: the same kills
    EXPECT_LT(same_c, 1500);   // another seed: another stream (~1% equal by chance)
    for (int v = 0; v < 100; ++v) {
        EXPECT_GT(counts[v], 850) << v; // 1000 expected each
        EXPECT_LT(counts[v], 1150) << v;
    }
}

TEST(RagdollKill, OneRollPerKill) {
    R::KillDice dice{5};
    // Asked again and again for the same kill (each of its updates, a second blow of a multi-hit
    // attack carrying the first one's roll): one number drawn, the same answer every time.
    for (int k = 0; k < 200; ++k) {
        R::VanillaRoll roll{};
        const std::uint64_t before = dice.Draws();
        const bool first = R::RollVanilla(roll, 15, dice);
        EXPECT_TRUE(roll.rolled);
        EXPECT_EQ(roll.pct, 15);
        R::VanillaRoll carried = roll; // the second blow's slot takes the first one's roll
        for (int again = 0; again < 10; ++again) {
            EXPECT_EQ(R::RollVanilla(roll, 15, dice), first);
            EXPECT_EQ(R::RollVanilla(carried, again % 2 == 0 ? 0 : 100, dice), first);
        }
        EXPECT_EQ(dice.Draws(), before + 1);
        EXPECT_EQ(roll.pct, 15); // the chance it was rolled with, for the log
    }
    EXPECT_EQ(dice.Draws(), 200u);
    // 0 and 100 draw too (one per kill whatever the chance: the stream does not depend on it).
    R::VanillaRoll zero{};
    R::VanillaRoll hundred{};
    EXPECT_FALSE(R::RollVanilla(zero, 0, dice));
    EXPECT_TRUE(R::RollVanilla(hundred, 100, dice));
    EXPECT_EQ(dice.Draws(), 202u);
}

TEST(RagdollKill, NoBodiesNoPush) {
    Beast b = ScourgeBeast();
    const R::PushPlan p = R::PlanPush(b.in, b.bodies.data(), 0, R::PushConfig{});
    EXPECT_EQ(p.dynamic, 0);
    EXPECT_EQ(p.nearest, -1);
    std::vector<R::PushBody> many(80, b.bodies[10]);
    const R::PushPlan q = R::PlanPush(b.in, many.data(), 80, R::PushConfig{}); // clamped
    EXPECT_EQ(q.bodies, R::MaxBodies);
    EXPECT_EQ(q.dynamic, R::MaxBodies);
}

} // namespace
