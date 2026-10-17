// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "core/vr/test_stats.h"
namespace S = VR::TestStats;
TEST(TestStats, AcceptsStartingHunterAndMaximum) {
    EXPECT_TRUE(S::Valid({11, 10, 12, 10, 9, 8}, 10));
    EXPECT_TRUE(S::Valid({10, 9, 10, 9, 7, 9}, 4));
    const S::Request request{};
    EXPECT_TRUE(S::Valid(request.stats, request.level));
    EXPECT_TRUE(S::Maxed(request.stats, request.level));
}
TEST(TestStats, RefusesPartialOrUnexpectedPlayerData) {
    EXPECT_FALSE(S::Valid({}, 0));
    EXPECT_FALSE(S::Valid({99, 99, 99, 99, 99, 99}, 10));
    EXPECT_FALSE(S::Valid({100, 10, 12, 10, 9, 8}, 99));
    EXPECT_FALSE(S::Valid({-1, 10, 12, 10, 9, 8}, 10));
    EXPECT_FALSE(S::Maxed({99, 99, 99, 99, 99, 98}, 543));
}
