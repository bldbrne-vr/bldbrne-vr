// SPDX-License-Identifier: GPL-2.0-or-later

// Use sounds (quick_use_sounds, the user's 2026-10-06 "and they should play a sound when
// activated"): an item used at once, without the game's use animation, plays that animation's own
// sound (or the closest game sound where it has none). core/vr/quick_items.h: UseClipOf (the
// behaviour's UseItem_<anim> selectors), UseSoundsOf (the a00.tae PlaySound events), checked
// against the goods tables of the paths that skip the animation.

#include <cstdint>

#include <gtest/gtest.h>

#include "core/vr/quick_items.h"

namespace {

namespace Q = VR::Quick;

TEST(QuickUseSounds, Clips) {
    // The behaviour's selectors: eat, drink, the throws, the vials, the crush, the bells.
    EXPECT_EQ(Q::UseClipOf(0), 100000);
    EXPECT_EQ(Q::UseClipOf(1), 100130);
    EXPECT_EQ(Q::UseClipOf(3), 100030);
    EXPECT_EQ(Q::UseClipOf(9), 100020);
    EXPECT_EQ(Q::UseClipOf(10), 100050);
    EXPECT_EQ(Q::UseClipOf(14), 100110);
    EXPECT_EQ(Q::UseClipOf(15), 100090);
    EXPECT_EQ(Q::UseClipOf(18), 100300);
    EXPECT_EQ(Q::UseClipOf(26), 100080);
    EXPECT_EQ(Q::UseClipOf(28), 100081);
    EXPECT_EQ(Q::UseClipOf(33), 100180);
    EXPECT_EQ(Q::UseClipOf(34), 100033);
    EXPECT_EQ(Q::UseClipOf(35), 100021);
    EXPECT_EQ(Q::UseClipOf(11), -1);
    EXPECT_EQ(Q::UseClipOf(-1), -1);
    EXPECT_EQ(Q::UseSoundsOf(10).clip, 100050);
}

TEST(QuickUseSounds, OwnSounds) {
    // The drink: the swallow at the lips, at once.
    const Q::UseSounds drink = Q::UseSoundsOf(10);
    EXPECT_TRUE(drink.own);
    ASSERT_EQ(drink.count, 1);
    EXPECT_EQ(drink.sound[0].id, 100050);
    EXPECT_EQ(drink.sound[0].after_ms, 0);
    // The injection (the Blood Vial's clip and the NPC bloods' copy of it): the stab, then the
    // blood 33 ms later (0.833 s / 0.867 s in the clip).
    for (const int anim : {26, 28}) {
        const Q::UseSounds inject = Q::UseSoundsOf(anim);
        EXPECT_TRUE(inject.own);
        ASSERT_EQ(inject.count, 2);
        EXPECT_EQ(inject.sound[0].id, Q::InjectSound);
        EXPECT_EQ(inject.sound[0].after_ms, 0);
        EXPECT_EQ(inject.sound[1].id, Q::BloodSound);
        EXPECT_EQ(inject.sound[1].after_ms, 33);
    }
    // Vermin: the crush, then 0.667 s later the second.
    const Q::UseSounds vermin = Q::UseSoundsOf(33);
    EXPECT_TRUE(vermin.own);
    ASSERT_EQ(vermin.count, 2);
    EXPECT_EQ(vermin.sound[0].id, 100180);
    EXPECT_EQ(vermin.sound[1].id, 100181);
    EXPECT_EQ(vermin.sound[1].after_ms, 667);
    // The ash: its pour and click.
    const Q::UseSounds ash = Q::UseSoundsOf(5);
    EXPECT_TRUE(ash.own);
    ASSERT_EQ(ash.count, 2);
    EXPECT_EQ(ash.sound[0].id, 710);
    EXPECT_EQ(ash.sound[1].id, 793);
}

TEST(QuickUseSounds, ClosestWhereTheClipHasNone) {
    // Eaten: the drink's swallow; wiped: the vial clip's arm whoosh; crushed (and the cords, the
    // crush's clip): the Vermin's crush.
    const Q::UseSounds eat = Q::UseSoundsOf(0);
    EXPECT_FALSE(eat.own);
    ASSERT_EQ(eat.count, 1);
    EXPECT_EQ(eat.sound[0].id, Q::SwallowSound);
    const Q::UseSounds wipe = Q::UseSoundsOf(1);
    EXPECT_FALSE(wipe.own);
    ASSERT_EQ(wipe.count, 1);
    EXPECT_EQ(wipe.sound[0].id, Q::ArmWhooshSound);
    for (const int anim : {9, 35}) {
        const Q::UseSounds crush = Q::UseSoundsOf(anim);
        EXPECT_FALSE(crush.own);
        ASSERT_EQ(crush.count, 1);
        EXPECT_EQ(crush.sound[0].id, Q::CrushSound);
    }
}

TEST(QuickUseSounds, NothingAddedToTheGamesOwnAnimation) {
    // The bells, the Silencing Blank, the Marks, the Choir Bell, the Music Box (select + Square:
    // the clip plays its own), the throws (throw_sound), the lantern, the notebook: nothing.
    for (const int anim : {2, 3, 8, 13, 14, 15, 16, 17, 18, 27, 34, 255, -1, 200}) {
        EXPECT_EQ(Q::UseSoundsOf(anim).count, 0) << anim;
    }
}

TEST(QuickUseSounds, EveryAtOncePathIsHeard) {
    // Every goods whose use skips the game's animation has a sound for its goodsUseAnim (the
    // 1.09 params): the mouth's (0 eat, 10 drink), the wipe's (1), the crush's (9, 33), the
    // stab's (28), the hold's consumables (1, 5, 9, 28, 33, 35) and the Blood Vial (26).
    struct Use {
        std::int32_t goods;
        std::int32_t anim;
    };
    for (const Use u : {Use{1100, 0}, Use{1110, 0}, Use{1101, 10}, Use{1120, 10}, Use{2030, 10}}) {
        EXPECT_TRUE(Q::MouthOf(u.goods) == Q::MouthKindOfAnim(u.anim)) << u.goods;
        EXPECT_GT(Q::UseSoundsOf(u.anim).count, 0) << u.goods;
    }
    for (const Use u : {Use{1300, 1}, Use{1310, 1}, Use{1320, 1}}) {
        EXPECT_TRUE(Q::WipeOf(u.goods) && Q::WipeOfAnim(u.anim)) << u.goods;
        EXPECT_GT(Q::UseSoundsOf(u.anim).count, 0) << u.goods;
    }
    for (const Use u : {Use{1510, 9}, Use{1500, 9}, Use{704, 9}, Use{1700, 33}}) {
        EXPECT_TRUE(Q::GestureOf(u.goods) == Q::GestureKind::Crush) << u.goods;
        EXPECT_TRUE(Q::GestureAnimAgrees(Q::GestureKind::Crush, u.anim)) << u.goods;
        EXPECT_GT(Q::UseSoundsOf(u.anim).count, 0) << u.goods;
    }
    for (int goods : {701,702,703,706}) {
        const auto sounds=Q::UseSoundsForGoods(goods,28);
        EXPECT_EQ(sounds.count,1);
        EXPECT_EQ(sounds.sound[0].id,Q::SwallowSound);
        EXPECT_NE(sounds.sound[0].id,Q::InjectSound);
    }
    EXPECT_EQ(Q::UseSoundsForGoods(705,28).sound[0].id,Q::CrushSound);
    EXPECT_EQ(Q::UseSoundsForGoods(1000,26).sound[0].id,Q::InjectSound);
    EXPECT_EQ(Q::UseSoundsForGoods(1000,26).sound[1].id,Q::BloodSound);
    for (const std::int32_t anim : {1, 5, 9, 28, 33, 35}) {
        EXPECT_TRUE(Q::HoldAnimAgrees(Q::HoldKind::Consume, anim)) << anim;
        EXPECT_GT(Q::UseSoundsOf(anim).count, 0) << anim;
    }
    // The rites (the game's own act) are never given a second sound.
    for (const std::int32_t anim : {14, 15, 16, 27}) {
        EXPECT_TRUE(Q::HoldAnimAgrees(Q::HoldKind::Rite, anim)) << anim;
        EXPECT_EQ(Q::UseSoundsOf(anim).count, 0) << anim;
    }
    EXPECT_EQ(Q::UseSoundsOf(26).sound[0].id, Q::InjectSound); // the Blood Vial's X / belt
}

TEST(QuickUseSounds, Offsets) {
    // The offsets rise (the render tick plays them in order) and stay under a second.
    for (int anim = -1; anim <= 255; ++anim) {
        const Q::UseSounds u = Q::UseSoundsOf(anim);
        ASSERT_LE(u.count, u.sound.size());
        for (std::uint8_t i = 0; i < u.count; ++i) {
            EXPECT_GT(u.sound[i].id, 0) << anim;
            EXPECT_GE(u.sound[i].after_ms, i == 0 ? 0 : u.sound[i - 1].after_ms) << anim;
            EXPECT_LT(u.sound[i].after_ms, 1000) << anim;
        }
        if (u.count > 0) {
            EXPECT_EQ(u.sound[0].after_ms, 0) << anim;
            EXPECT_NE(u.clip, -1) << anim;
        }
    }
}

} // namespace
