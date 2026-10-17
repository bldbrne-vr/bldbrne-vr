// SPDX-License-Identifier: GPL-2.0-or-later
#include <limits>
#include <string>
#include <gtest/gtest.h>
#include "core/vr/interaction_mailbox.h"
#include "physical_interactions/quick_action_packets.h"
namespace A = PhysicalInteractions::QuickActions;
namespace Q = VR::Quick;
namespace {
PiQuickRequest Request() {
    Q::Request r{};
    r.serial = 11;
    r.steady_ms = 123456;
    r.action = Q::Action::Throw;
    r.goods = 100;
    r.slot = 2;
    r.dmy = 17;
    r.v_world = {1.25f, -2.5f, 3.75f};
    r.throw_class = 2;
    return A::Pack(r, 3, 400, 5);
}
} // namespace
TEST(QuickActionPackets, CopiesEveryActionFieldWithoutNarrowingLoss) {
    auto p = Request();
    p.select = 5;
    p.mouth = 2;
    p.wipe = p.stab = p.crank = 1;
    Q::Request r{};
    ASSERT_TRUE(A::Unpack(p, r));
    auto copy = A::Pack(r, p.session, p.request_id, p.character_generation);
    EXPECT_EQ(copy.controller_serial, p.controller_serial);
    EXPECT_EQ(copy.steady_ms, p.steady_ms);
    EXPECT_EQ(copy.action, p.action);
    EXPECT_EQ(copy.select, p.select);
    EXPECT_EQ(copy.goods, p.goods);
    EXPECT_EQ(copy.slot, p.slot);
    EXPECT_EQ(copy.dmy, p.dmy);
    EXPECT_EQ(copy.throw_class, p.throw_class);
    EXPECT_EQ(copy.mouth, p.mouth);
    EXPECT_EQ(copy.wipe, p.wipe);
    EXPECT_EQ(copy.stab, p.stab);
    EXPECT_EQ(copy.crank, p.crank);
    for (int i = 0; i < 3; ++i)
        EXPECT_EQ(copy.velocity[i], p.velocity[i]);
}
TEST(QuickActionPackets, MalformedInputCannotPartiallyOverwriteDestination) {
    for (int failure = 0; failure < 14; ++failure) {
        auto p = Request();
        switch (failure) {
        case 0:
            p.session = 0;
            break;
        case 1:
            p.request_id = 0;
            break;
        case 2:
            p.character_generation = 0;
            break;
        case 3:
            p.controller_serial = 0;
            break;
        case 4:
            p.action = 3;
            break;
        case 5:
            p.select = 6;
            break;
        case 6:
            p.slot = -2;
            break;
        case 7:
            p.dmy = 128;
            break;
        case 8:
            p.throw_class = 256;
            break;
        case 9:
            p.mouth = 3;
            break;
        case 10:
            p.wipe = 2;
            break;
        case 11:
            p.stab = 2;
            break;
        case 12:
            p.crank = 2;
            break;
        case 13:
            p.velocity[1] = std::numeric_limits<float>::quiet_NaN();
            break;
        }
        Q::Request r{};
        r.serial = 99;
        r.goods = 123;
        EXPECT_FALSE(A::Unpack(p, r)) << failure;
        EXPECT_EQ(r.serial, 99);
        EXPECT_EQ(r.goods, 123);
    }
}
TEST(QuickActionPackets, ResultOwnsTextAndReportsTruncation) {
    auto request = Request();
    for (size_t size : {0u, 126u, 127u, 128u, 8000u}) {
        std::string reason(size, 'x');
        Q::Result r{11, Q::Outcome::Denied, reason.c_str(), 100, 2, 2};
        PiQuickResult out{};
        ASSERT_TRUE(A::PackResult(request, r, out));
        reason.assign(size, 'y');
        EXPECT_EQ(std::string(out.reason), std::string(std::min(size, size_t{127}), 'x'));
        EXPECT_EQ(out.reason_truncated, size > 127);
        EXPECT_TRUE(A::Matches(request, out));
    }
    PiQuickResult out{};
    Q::Result wrong{12, Q::Outcome::Thrown};
    EXPECT_FALSE(A::PackResult(request, wrong, out));
    EXPECT_EQ(out.request_id, 0);
}
TEST(QuickActionPackets, IdentityAndTerminatedTextAreRequired) {
    const auto request = Request();
    Q::Result r{11, Q::Outcome::Thrown, "used", 100, 2, 1};
    PiQuickResult result{};
    ASSERT_TRUE(A::PackResult(request, r, result));
    for (int failure = 0; failure < 7; ++failure) {
        auto bad = result;
        switch (failure) {
        case 0:
            ++bad.session;
            break;
        case 1:
            ++bad.request_id;
            break;
        case 2:
            ++bad.character_generation;
            break;
        case 3:
            ++bad.controller_serial;
            break;
        case 4:
            bad.outcome = 0;
            break;
        case 5:
            bad.reason_truncated = 2;
            break;
        case 6:
            std::fill(std::begin(bad.reason), std::end(bad.reason), 'x');
            break;
        }
        EXPECT_FALSE(A::Matches(request, bad)) << failure;
    }
}
TEST(QuickActionPackets, ClaimedActionRetainsOwnedResultThroughCancellationAndSessionChange) {
    VR::Interactions::Mailbox<PiQuickRequest, PiQuickResult> mailbox;
    auto request = Request();
    ASSERT_TRUE(mailbox.Submit(1, request));
    PiQuickRequest claimed{};
    ASSERT_TRUE(mailbox.Claim(claimed));
    EXPECT_FALSE(mailbox.Cancel(1));
    auto next = request;
    ++next.session;
    ++next.request_id;
    EXPECT_FALSE(mailbox.Submit(2, next));
    Q::Result r{11, Q::Outcome::Thrown, "used", 100, 2, 1};
    PiQuickResult result{};
    ASSERT_TRUE(A::PackResult(claimed, r, result));
    ASSERT_TRUE(mailbox.Finish(1, result));
    EXPECT_FALSE(mailbox.Submit(2, next));
    EXPECT_FALSE(mailbox.Finish(1, result));
    PiQuickResult received{};
    ASSERT_TRUE(mailbox.Poll(received));
    EXPECT_TRUE(A::Matches(request, received));
    EXPECT_FALSE(A::Matches(next, received));
    EXPECT_FALSE(mailbox.Poll(received));
    ASSERT_TRUE(mailbox.Submit(2, next));
    ASSERT_TRUE(mailbox.Cancel(2));
    EXPECT_FALSE(mailbox.Claim(claimed));
    EXPECT_FALSE(mailbox.Submit(2, next));
}
