// SPDX-License-Identifier: GPL-2.0-or-later
#include <limits>
#include <string>
#include <gtest/gtest.h>
#include "quick_allocation_probe.h"
using PhysicalInteractions::BoundedText;
TEST(BoundedText, CapacityAndTruncationAreExplicit) {
    BoundedText<8> text{"12345678"};
    EXPECT_EQ(text.size(), 8);
    EXPECT_FALSE(text.Truncated());
    EXPECT_EQ(text.c_str()[8], 0);
    text += "9";
    EXPECT_EQ(std::string_view(text), "12345678");
    EXPECT_TRUE(text.Truncated());
    auto copied = text;
    EXPECT_TRUE(copied.Truncated());
    EXPECT_EQ(copied.size(), 8);
    copied.clear();
    EXPECT_FALSE(copied.Truncated());
    EXPECT_TRUE(copied.empty());
    EXPECT_EQ(copied.c_str()[0], 0);
    BoundedText<0> empty{"x"};
    EXPECT_TRUE(empty.Truncated());
    EXPECT_EQ(empty.size(), 0);
}
TEST(BoundedText, AliasingAndSubstringStayInBounds) {
    BoundedText<8> text{"abc"};
    text += text;
    EXPECT_EQ(std::string_view(text), "abcabc");
    EXPECT_FALSE(text.Truncated());
    text += std::string_view(text).substr(1);
    EXPECT_EQ(std::string_view(text), "abcabcbc");
    EXPECT_TRUE(text.Truncated());
    EXPECT_EQ(std::string_view(text.substr(3, 2)), "ab");
    EXPECT_TRUE(text.substr(99).empty());
    EXPECT_EQ(text.find("cab"), 2);
    EXPECT_EQ(text.find("z"), std::string_view::npos);
}
TEST(BoundedText, NumberFormattingMatchesLegacyWithoutAllocations) {
    for (double value : {-0.0, 1.25, -127.5, 1e-9, 1e20, std::numeric_limits<double>::max()}) {
        BoundedText<512> actual;
        {
            QuickAllocationProbe::Scope scope{"number formatting"};
            actual = BoundedText<512>::NumberText(value);
        }
        EXPECT_EQ(std::string_view(actual), std::to_string(value));
        EXPECT_FALSE(actual.Truncated());
    }
    const auto clipped = BoundedText<8>::NumberText(123456789);
    EXPECT_EQ(std::string_view(clipped), "12345678");
    EXPECT_TRUE(clipped.Truncated());
}

TEST(BoundedText, TruncationSurvivesTransferToLargerTraceStorage) {
    BoundedText<4> small{"abcdef"};
    BoundedText<32> large{small};
    EXPECT_EQ(std::string_view(large), "abcd");
    EXPECT_TRUE(large.Truncated());
    large.clear();
    large += small;
    EXPECT_EQ(std::string_view(large), "abcd");
    EXPECT_TRUE(large.Truncated());
    VR::Quick::ThrowTrace trace;
    EXPECT_TRUE(trace.Lines(0, 1, 1, "literal tag").empty()); // preserve the legacy helper API
}
