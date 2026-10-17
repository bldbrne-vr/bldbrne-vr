// SPDX-License-Identifier: GPL-2.0-or-later

// The hunter's face sets in first-person VR (src/video_core/renderer_vulkan/vr_body_split.h and
// the generated vr_body_table.h): the near-camera cutaway, the hidden cape and body_head_shadow
// (the head drawn into the shadow maps, left out of the colour passes), body_cutaway_shoulders (the
// upper arms and shoulders fade with the body), and body_mode=arms_only (only the hands and
// forearms in the colour passes: vr_hands_table.h).

#include <algorithm>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/body_settings.h"
#include "video_core/renderer_vulkan/vr_body_split.h"
#include "video_core/renderer_vulkan/vr_body_table.h"
#include "video_core/renderer_vulkan/vr_hands_table.h"

namespace {

using namespace Vulkan;
using VrBodySplit::Action;
using VrBodySplit::Frame;
using VrBodySplit::KeyShape;
using VrBodySplit::PlanDraw;

// The behaviour before body_head_shadow (vk_rasterizer_vr_draws.cpp, 2026-10-04): with the cutaway
// on a triangle list, the cutaway pipeline is asked for; arms or a hidden cape split the draw.
struct OldPlan {
    bool handled;
    bool split;
    bool cape_hidden;
};
OldPlan Old(const KeyShape& k, bool cut_on, bool cape_hidden, bool triangles) {
    if (!cut_on || !triangles) {
        return {false, false, false};
    }
    return {true, k.keep != 0 || (k.cape != 0 && cape_hidden), cape_hidden};
}

TEST(BodySplit, HeadShownIsExactlyTheOldBehaviour) {
    for (u16 keep : {0, 2}) {
        for (u16 cut : {0, 1, 3}) {
            for (u16 cape : {0, 2}) {
                for (u16 head : {0, 4}) {
                    const KeyShape k{keep, cut, cape, head, false};
                    for (int bits = 0; bits < 16; ++bits) {
                        const bool cut_on = bits & 1, cape_hidden = bits & 2, colour = bits & 4,
                                   tri = bits & 8;
                        const auto p = PlanDraw(k, {cut_on, cape_hidden, false}, colour, tri);
                        const OldPlan o = Old(k, cut_on, cape_hidden, tri);
                        if (!o.handled) {
                            EXPECT_EQ(p.action, Action::Untouched);
                            continue;
                        }
                        EXPECT_EQ(p.action, o.split ? Action::Split : Action::Whole);
                        EXPECT_TRUE(p.cut);
                        EXPECT_EQ(p.draw_cape, !o.cape_hidden);
                        EXPECT_TRUE(p.draw_head); // the head's triangles were cut ones
                    }
                }
            }
        }
    }
}

TEST(BodySplit, HeadPartsActOnlyWhileTheHeadIsHidden) {
    const KeyShape hat{0, 0, 0, 1, true};
    const KeyShape hood{0, 5, 0, 6, true};
    for (int bits = 0; bits < 8; ++bits) {
        const bool cut_on = bits & 1, colour = bits & 2, tri = bits & 4;
        EXPECT_EQ(PlanDraw(hat, {cut_on, true, false}, colour, tri).action, Action::Untouched);
        EXPECT_EQ(PlanDraw(hood, {cut_on, true, false}, colour, tri).action, Action::Untouched);
    }
}

TEST(BodySplit, HiddenHeadLeavesTheColourPassesAndCastsItsShadow) {
    const KeyShape hat{0, 0, 0, 1, true};   // all head (a hat, the face, hair)
    const KeyShape hood{0, 5, 0, 6, true};  // a hood: the shoulders are cut ones
    const KeyShape vest{3, 7, 2, 4, false}; // a body face set: arms, torso, capelet, collar top
    for (bool cut_on : {false, true}) {
        for (bool tri : {false, true}) {
            // Colour pass: the hat is never drawn, whatever the primitive.
            EXPECT_EQ(PlanDraw(hat, {cut_on, true, true}, true, tri).action, Action::Skip);
            // Shadow (depth-only) pass: drawn whole (the cutaway is no shadow pass's business).
            const auto s = PlanDraw(hat, {cut_on, true, true}, false, tri);
            EXPECT_TRUE(s.action == Action::Untouched || s.action == Action::Whole);
        }
        // The hood: only its shoulders in the colour pass, never faded (a head part is never cut,
        // as with body_head_shadow=0).
        const auto c = PlanDraw(hood, {cut_on, true, true}, true, true);
        EXPECT_EQ(c.action, Action::Split);
        EXPECT_FALSE(c.draw_head);
        EXPECT_FALSE(c.cut);
        const auto d = PlanDraw(hood, {cut_on, true, true}, false, true);
        EXPECT_NE(d.action, Action::Skip);
        EXPECT_NE(d.action, Action::Split); // nothing left out: the draw as it is
        // The vest: arms kept, torso cut, cape hidden, collar top left out of the colour pass,
        // all of it but the hidden cape in the shadows.
        const auto v = PlanDraw(vest, {cut_on, true, true}, true, true);
        EXPECT_EQ(v.action, Action::Split);
        EXPECT_FALSE(v.draw_head);
        EXPECT_FALSE(v.draw_cape);
        EXPECT_EQ(v.cut, cut_on);
        const auto vs = PlanDraw(vest, {cut_on, true, true}, false, true);
        if (cut_on) {
            EXPECT_EQ(vs.action, Action::Split);
            EXPECT_TRUE(vs.draw_head);
            EXPECT_FALSE(vs.draw_cape);
        } else {
            EXPECT_EQ(vs.action, Action::Untouched); // the cape table hides the capelet
        }
        // A cape shown: drawn in the colour pass too (cut), only the head left out.
        const auto shown = PlanDraw(vest, {cut_on, false, true}, true, true);
        EXPECT_TRUE(shown.draw_cape);
        EXPECT_FALSE(shown.draw_head);
    }
    // A face set of only head and a hidden cape: nothing of it in the colour pass.
    EXPECT_EQ(PlanDraw({0, 0, 2, 3, false}, {true, true, true}, true, true).action, Action::Skip);
    EXPECT_EQ(PlanDraw({0, 0, 2, 3, false}, {true, false, true}, true, true).action, Action::Split);
    // A body face set without head triangles is unchanged by body_head_shadow.
    for (int bits = 0; bits < 16; ++bits) {
        const bool cut_on = bits & 1, cape_hidden = bits & 2, colour = bits & 4, tri = bits & 8;
        const KeyShape k{2, 3, 1, 0, false};
        const auto a = PlanDraw(k, {cut_on, cape_hidden, false}, colour, tri);
        const auto b = PlanDraw(k, {cut_on, cape_hidden, true}, colour, tri);
        EXPECT_EQ(a.action, b.action);
        EXPECT_EQ(a.cut, b.cut);
        EXPECT_EQ(a.draw_cape, b.draw_cape);
    }
}

TEST(BodySplit, WhoHidesTheHead) {
    using VR::Body::BodyMode;
    using VR::Body::HeadHiddenByRendererFor;
    EXPECT_TRUE(HeadHiddenByRendererFor(true, BodyMode::NoHead, true));
    EXPECT_FALSE(HeadHiddenByRendererFor(true, BodyMode::NoHead, false)); // the old scale
    EXPECT_FALSE(HeadHiddenByRendererFor(false, BodyMode::NoHead, true)); // no body hooks
    EXPECT_FALSE(HeadHiddenByRendererFor(true, BodyMode::ArmsOnly, true)); // the hands filter
    EXPECT_TRUE(HeadHiddenByRendererFor(true, BodyMode::Full, true));
    EXPECT_TRUE(HeadHiddenByRendererFor(true, BodyMode::Full, false)); // never scaled
    EXPECT_FALSE(HeadHiddenByRendererFor(false, BodyMode::Full, true));
    EXPECT_TRUE(VR::Body::BodySettings{}.body_head_shadow); // the default
}

// The generated table: sorted unique keys; each key's ranges are triangles of the face set, in
// order within a class, disjoint, and cover every triangle once; head parts are head parts.
TEST(BodySplit, TableIsConsistent) {
    const auto& keys = VrBodyTable::Keys;
    const auto& ranges = VrBodyTable::Ranges;
    ASSERT_FALSE(keys.empty());
    u32 head_only = 0;
    u32 body_with_head = 0;
    u32 skin = 0;
    u32 skin_with_arms = 0;
    u32 next = 0;
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto& k = keys[i];
        if (i != 0) {
            const auto& p = keys[i - 1];
            ASSERT_TRUE(p.count < k.count || (p.count == k.count && p.hash < k.hash)) << k.name;
        }
        ASSERT_EQ(k.begin, next) << k.name;
        const u32 n = u32(k.keep) + k.shoulder + k.cut + k.cape + k.head;
        next += n;
        ASSERT_LE(next, ranges.size());
        std::vector<std::pair<u32, u32>> all;
        for (u32 r = 0; r < n; ++r) {
            const auto& x = ranges[k.begin + r];
            ASSERT_EQ(x.first % 3, 0u) << k.name;
            ASSERT_EQ(x.count % 3, 0u) << k.name;
            ASSERT_GT(x.count, 0u) << k.name;
            ASSERT_LE(x.first + x.count, k.count) << k.name;
            all.emplace_back(x.first, x.count);
        }
        std::sort(all.begin(), all.end());
        u32 at = 0;
        for (const auto& [first, count] : all) {
            ASSERT_EQ(first, at) << k.name; // disjoint and no gap
            at = first + count;
        }
        ASSERT_EQ(at, k.count - k.count % 3) << k.name;
        const std::string_view name{k.name};
        const bool head_part = name.starts_with("hd_") || name.starts_with("fc_") ||
                               name.starts_with("fg_") || name.starts_with("hr_");
        ASSERT_EQ(k.head_only, head_part) << k.name;
        if (k.head_only) {
            ++head_only;
            // Only the fc_* bare skin (face, neck, torso, arms, hands) keeps arm ranges.
            if (!name.starts_with("fc_")) {
                ASSERT_EQ(k.keep, 0) << k.name;
            } else {
                ++skin;
                skin_with_arms += k.keep != 0;
                // The hands (fc_m_0000 mesh 3: 5018 arm triangles, mesh 2: 716) are never a
                // head key (2026-10-06: the whole part left the colour passes, hands with it).
                ASSERT_FALSE(name.starts_with("fc_m_0000 mesh 2 ") ||
                             name.starts_with("fc_m_0000 mesh 3 "))
                    << k.name;
            }
            ASSERT_EQ(k.cape, 0) << k.name;
            ASSERT_GT(k.head, 0) << k.name;
            ASSERT_EQ(k.shoulder, 0) << k.name; // a head part is never cut
        } else {
            ASSERT_TRUE(name.starts_with("bd_") || name.starts_with("lg_")) << k.name;
            body_with_head += k.head != 0;
            // A body key has something to cut: torso, cape, head or upper arms.
            ASSERT_GT(u32(k.shoulder) + k.cut + k.cape + k.head, 0u) << k.name;
        }
    }
    EXPECT_EQ(next, ranges.size());
    EXPECT_GT(head_only, 1000u);   // hats, faces, face-gen parts, hair
    EXPECT_GT(body_with_head, 50u); // collars on Neck
    EXPECT_GT(skin, 20u);           // the faces' and necks' face sets of the fc_* parts
    EXPECT_GT(skin_with_arms, 0u);  // a few with arm triangles: kept
}

// body_cutaway_shoulders=0: exactly the behaviour before the shoulder class, where the shoulder
// triangles were keep ones (a key's keep then = keep + shoulder now) and a face set of only arms
// had no key (untouched); a key without shoulder ranges acts as before whatever it holds.
TEST(BodySplit, ShouldersKeptIsExactlyTheOldBehaviour) {
    for (u16 keep : {0, 2}) {
        for (u16 shoulder : {0, 3}) {
            for (u16 cut : {0, 1, 3}) {
                for (u16 cape : {0, 2}) {
                    for (u16 head : {0, 4}) {
                        for (bool head_only : {false, true}) {
                            if (head_only && shoulder != 0) {
                                continue; // never generated
                            }
                            const KeyShape now{keep, cut, cape, head, head_only, shoulder};
                            const KeyShape before{u16(keep + shoulder), cut, cape, head,
                                                  head_only};
                            const bool had_key =
                                shoulder == 0 || head_only || cut != 0 || cape != 0 || head != 0;
                            for (int bits = 0; bits < 32; ++bits) {
                                const bool cut_on = bits & 1, cape_hidden = bits & 2,
                                           head_hidden = bits & 4, colour = bits & 8,
                                           tri = bits & 16;
                                const auto p = PlanDraw(
                                    now, {cut_on, cape_hidden, head_hidden, false}, colour, tri);
                                EXPECT_FALSE(p.cut_shoulder);
                                if (!had_key) {
                                    EXPECT_EQ(p.action, Action::Untouched);
                                    continue;
                                }
                                const auto o = PlanDraw(before, {cut_on, cape_hidden, head_hidden},
                                                        colour, tri);
                                EXPECT_EQ(p.action, o.action);
                                EXPECT_EQ(p.cut, o.cut);
                                EXPECT_EQ(p.draw_cape, o.draw_cape);
                                EXPECT_EQ(p.draw_head, o.draw_head);
                            }
                        }
                    }
                }
            }
        }
    }
}

// body_cutaway_shoulders=1 (the default): the upper arms and shoulders go with the cut ranges
// whenever the body is cut; the forearms and hands stay whole; a head part never fades.
TEST(BodySplit, ShouldersFadeWithTheBody) {
    EXPECT_TRUE(VR::Body::BodySettings{}.body_cutaway_shoulders); // the default
    const KeyShape shirt{4, 2, 0, 0, false, 5}; // forearms, a little torso, upper sleeves
    const KeyShape vest{0, 7, 2, 3, false, 1};  // torso, capelet, collar top, a shoulder cap
    const KeyShape sleeve{0, 0, 0, 0, false, 2}; // an upper sleeve on its own (a new key)
    const KeyShape arms{3, 0, 0, 0, false, 2};   // a whole sleeve: forearm and upper arm
    for (bool cape_hidden : {false, true}) {
        for (bool head_hidden : {false, true}) {
            const Frame on{true, cape_hidden, head_hidden, true};
            const auto s = PlanDraw(shirt, on, true, true);
            EXPECT_EQ(s.action, Action::Split); // the forearms stay
            EXPECT_TRUE(s.cut);
            EXPECT_TRUE(s.cut_shoulder);
            const auto v = PlanDraw(vest, on, true, true);
            EXPECT_TRUE(v.cut);
            EXPECT_TRUE(v.cut_shoulder);
            // Nothing kept: one draw with the cutaway, unless something of it is left out.
            EXPECT_EQ(v.action, cape_hidden || head_hidden ? Action::Split : Action::Whole);
            const auto u = PlanDraw(sleeve, on, true, true);
            EXPECT_EQ(u.action, Action::Whole);
            EXPECT_TRUE(u.cut);
            const auto a = PlanDraw(arms, on, true, true);
            EXPECT_EQ(a.action, Action::Split);
            EXPECT_TRUE(a.cut_shoulder);
            // The cutaway off this frame (third person, body_cutaway=0, the pitch's first step):
            // nothing fades, a face set of only arms is untouched.
            const Frame off{false, cape_hidden, head_hidden, true};
            EXPECT_FALSE(PlanDraw(shirt, off, true, true).cut_shoulder);
            EXPECT_EQ(PlanDraw(sleeve, off, true, true).action, Action::Untouched);
            EXPECT_EQ(PlanDraw(arms, off, true, true).action, Action::Untouched);
            // Not a triangle list: as before (drawn whole, the cutaway pipeline asked for).
            EXPECT_EQ(PlanDraw(shirt, on, true, false).action, Action::Untouched);
        }
    }
    // A head part (the fc_* bare skin with its arms) never fades, the shoulder flag or not.
    const auto skin = PlanDraw({2, 3, 0, 4, true}, {true, true, true, true}, true, true);
    EXPECT_FALSE(skin.cut);
    EXPECT_FALSE(skin.cut_shoulder);
    // All of a face set left out (head and hidden cape only) is still skipped; one with an upper
    // sleeve is not.
    EXPECT_EQ(PlanDraw({0, 0, 2, 3, false, 0}, {true, true, true, true}, true, true).action,
              Action::Skip);
    EXPECT_EQ(PlanDraw({0, 0, 2, 3, false, 1}, {true, true, true, true}, true, true).action,
              Action::Split);
}

// The generated classes, checked against the hands table (gen_hands_table.py: a vertex with half
// its weight on the hand / forearm bones): the shoulder class (an arm triangle with less than half
// of its arm weight from the elbow down) is the upper arm: never a hand triangle, almost never
// one of the hands table's hand-or-forearm triangles (only the elbow ring, whose triangles the
// hands table takes with any forearm vertex); the keep class is the forearms and hands. And the
// Foreign Garb (the kit save's coat): the shirt's sleeves split at the elbow, the vest's shoulder
// caps (its only arm triangles) fade.
TEST(BodySplit, ShoulderClassIsTheUpperArm) {
    const auto& keys = VrBodyTable::Keys;
    const auto& ranges = VrBodyTable::Ranges;
    u64 shoulder = 0, shoulder_hand = 0, shoulder_lower = 0;
    u64 keep = 0, keep_hand = 0, keep_lower = 0;
    u64 cut_hand = 0;
    u32 keys_with = 0, arms_only = 0, compared = 0;
    std::vector<u8> cls;
    for (const auto& k : keys) {
        if (k.head_only) {
            continue;
        }
        keys_with += k.shoulder != 0;
        arms_only += k.shoulder != 0 && k.cut == 0 && k.cape == 0 && k.head == 0;
        const auto hk = std::lower_bound(
            VrHandsTable::Keys.begin(), VrHandsTable::Keys.end(), std::pair{k.count, k.hash},
            [](const VrHandsTable::Key& a, const std::pair<u32, u64>& b) {
                return a.count != b.first ? a.count < b.first : a.hash < b.second;
            });
        if (hk == VrHandsTable::Keys.end() || hk->count != k.count || hk->hash != k.hash ||
            (k.keep == 0 && k.shoulder == 0)) {
            continue;
        }
        ++compared;
        // Per triangle: bit 0 hand, bit 1 hand or forearm (hands table).
        cls.assign(k.count / 3, 0);
        for (u32 r = 0; r < u32(hk->hand) + hk->lower; ++r) {
            const auto& x = VrHandsTable::Ranges[hk->begin + r];
            for (u32 t = x.first / 3; t < (x.first + x.count) / 3; ++t) {
                cls[t] |= r < hk->hand ? 1 : 2;
            }
        }
        const auto tally = [&](u32 first, u32 n, u64& all, u64& hand, u64& lower) {
            for (u32 r = first; r < first + n; ++r) {
                const auto& x = ranges[k.begin + r];
                for (u32 t = x.first / 3; t < (x.first + x.count) / 3; ++t) {
                    ++all;
                    hand += (cls[t] & 1) != 0;
                    lower += (cls[t] & 2) != 0;
                }
            }
        };
        u64 ignored_all = 0, ignored_lower = 0;
        tally(0, k.keep, keep, keep_hand, keep_lower);
        tally(k.keep, k.shoulder, shoulder, shoulder_hand, shoulder_lower);
        tally(u32(k.keep) + k.shoulder, k.cut, ignored_all, cut_hand, ignored_lower);
    }
    EXPECT_GT(keys_with, 500u);  // every coat and shirt with sleeves
    EXPECT_GT(arms_only, 20u);   // sleeves of their own (keys that are new with the class)
    EXPECT_GT(compared, 500u);
    EXPECT_GT(shoulder, 400000u);
    EXPECT_EQ(shoulder_hand, 0u);
    EXPECT_LT(shoulder_lower * 100, shoulder);  // under 1% (measured 0.6%: the elbow ring)
    EXPECT_GT(keep_lower * 10, keep * 8);       // over 80% (measured 88%; the rest: the elbow)
    EXPECT_GT(keep_hand, 0u);                   // the hands are keep ones
    EXPECT_EQ(cut_hand, 0u);                    // the torso's never touch a hand
    const auto find = [&](std::string_view prefix) -> const VrBodyTable::Key* {
        for (const auto& k : keys) {
            if (std::string_view{k.name}.starts_with(prefix)) {
                return &k;
            }
        }
        return nullptr;
    };
    const auto* shirt = find("bd_m_8000 mesh 5 ");
    ASSERT_NE(shirt, nullptr);
    EXPECT_GT(shirt->keep, 0);
    EXPECT_GT(shirt->shoulder, 0);
    const auto* vest = find("bd_m_8000 mesh 0 ");
    ASSERT_NE(vest, nullptr);
    EXPECT_EQ(vest->keep, 0);
    EXPECT_GT(vest->shoulder, 0);
    // Legs have no arms.
    for (const auto& k : keys) {
        if (std::string_view{k.name}.starts_with("lg_")) {
            EXPECT_EQ(k.keep, 0) << k.name;
            EXPECT_EQ(k.shoulder, 0) << k.name;
        }
    }
}

// A head part's arm triangles (the fc_* bare skin: the hands) are drawn in the colour passes while
// the head is hidden; in a shadow pass a head part is drawn as before (untouched).
TEST(BodySplit, BareSkinKeepsItsHands) {
    const KeyShape skin{2, 3, 0, 4, true};
    for (bool cut_on : {false, true}) {
        const auto c = PlanDraw(skin, {cut_on, true, true}, true, true);
        EXPECT_EQ(c.action, Action::Split); // keep ranges drawn, head ranges left out
        EXPECT_FALSE(c.draw_head);
        EXPECT_FALSE(c.cut); // the hands and the rest of the skin: the draw's own pipeline
        for (bool tri : {false, true}) {
            EXPECT_EQ(PlanDraw(skin, {cut_on, true, true}, false, tri).action, Action::Untouched);
            EXPECT_EQ(PlanDraw({0, 0, 0, 1, true}, {cut_on, true, true}, false, tri).action,
                      Action::Untouched);
        }
    }
}

// body_mode=arms_only / hands_only: the solver scales nothing; the renderer hides all but the
// hands and forearms / the hands.
TEST(BodySplit, WhoShowsOnlyTheHands) {
    using VR::Body::BodyMode;
    using VR::Body::DrawnArms;
    using VR::Body::DrawnArmsFor;
    EXPECT_EQ(DrawnArmsFor(true, BodyMode::ArmsOnly, true), DrawnArms::Forearms);
    EXPECT_EQ(DrawnArmsFor(true, BodyMode::ArmsOnly, false), DrawnArms::Whole); // old Spine scale
    EXPECT_EQ(DrawnArmsFor(true, BodyMode::HandsOnly, true), DrawnArms::Hands);
    EXPECT_EQ(DrawnArmsFor(true, BodyMode::HandsOnly, false), DrawnArms::Hands); // no other way
    for (const BodyMode m :
         {BodyMode::NoHead, BodyMode::ArmsOnly, BodyMode::Full, BodyMode::HandsOnly}) {
        EXPECT_EQ(DrawnArmsFor(false, m, true), DrawnArms::Whole); // no body hooks
    }
    EXPECT_EQ(DrawnArmsFor(true, BodyMode::NoHead, true), DrawnArms::Whole);
    EXPECT_EQ(DrawnArmsFor(true, BodyMode::Full, true), DrawnArms::Whole);
    EXPECT_FALSE(VR::Body::HeadHiddenByRendererFor(true, BodyMode::HandsOnly, true));
}

// The HUD fades only for parts that are drawn (2026-10-08 review: in hands_only the counters
// faded for the invisible torso looking 53 degrees and more down).
TEST(BodySplit, OnlyDrawnPartsHideTheHud) {
    using VR::Body::DrawnArms;
    using VR::Body::DrawnPartsOf;
    constexpr auto whole = DrawnPartsOf(DrawnArms::Whole);
    EXPECT_TRUE(whole.forearms && whole.upper_arms && whole.torso);
    constexpr auto forearms = DrawnPartsOf(DrawnArms::Forearms);
    EXPECT_TRUE(forearms.forearms);
    EXPECT_FALSE(forearms.upper_arms);
    EXPECT_FALSE(forearms.torso);
    constexpr auto hands = DrawnPartsOf(DrawnArms::Hands);
    EXPECT_FALSE(hands.forearms || hands.upper_arms || hands.torso);
    // The default body_mode (hands_only) draws no torso.
    EXPECT_FALSE(DrawnPartsOf(VR::Body::DrawnArmsFor(true, VR::Body::BodySettings{}.mode, true))
                     .torso);
}

// His own draws: skinned parts at his origin, rigid pieces on his bones within his reach; another
// character's root is never his.
TEST(BodySplit, WhoseObject) {
    using VrBodySplit::IsPlayersObject;
    EXPECT_TRUE(IsPlayersObject(0.0f, -0.001f, 0.0f, true));  // his root (measured)
    EXPECT_TRUE(IsPlayersObject(0.08f, 0.05f, 0.06f, true));  // a frame of lag at a sprint
    EXPECT_FALSE(IsPlayersObject(0.75f, 0.0f, 0.0f, true));   // a hunter NPC beside him
    EXPECT_FALSE(IsPlayersObject(0.5f, 0.6f, 0.0f, true));    // ... one step up the stairs
    // A deep duck: his skinned parts' root sinks under his origin with the UpperPos shift
    // (measured 0.30 m down at a 0.60 m shift, 0.71 m at 0.80, 1-2 cm aside); straight under him
    // it is his, down to 1.25 m.
    EXPECT_TRUE(IsPlayersObject(0.003f, -0.300f, -0.015f, true));
    EXPECT_TRUE(IsPlayersObject(-0.005f, -0.706f, -0.021f, true));
    EXPECT_TRUE(IsPlayersObject(0.12f, -0.45f, 0.20f, true)); // a forward duck
    EXPECT_TRUE(IsPlayersObject(0.0f, -1.25f, 0.0f, true));
    EXPECT_FALSE(IsPlayersObject(0.0f, -1.30f, 0.0f, true));
    EXPECT_FALSE(IsPlayersObject(0.40f, -0.30f, 0.0f, true));  // aside: someone below a step
    EXPECT_FALSE(IsPlayersObject(0.0f, 0.45f, 0.0f, true));    // above him: never
    // The Foreign set's bandage on L_Forearm: its bone transform times the inverse bind pose
    // (measured on a raised arm: 0.90 m left, 1.24 m up, 0.90 m back).
    EXPECT_TRUE(IsPlayersObject(-0.904f, 1.243f, -0.897f, false));
    EXPECT_TRUE(IsPlayersObject(0.3f, 2.2f, -0.4f, false));
    EXPECT_FALSE(IsPlayersObject(2.0f, 1.0f, 1.5f, false)); // a piece on someone farther away
    EXPECT_FALSE(IsPlayersObject(0.0f, 0.0f, 3.0f, false));
}

// Only the camera's colour passes of such a frame lose his parts; the shadows keep them.
TEST(BodySplit, HandsPlan) {
    using VrBodySplit::HandsAction;
    using VrBodySplit::PlanHands;
    for (bool tri : {false, true}) {
        for (u32 ranges : {0u, 1u, 7u}) {
            for (bool all : {false, true}) {
                EXPECT_EQ(PlanHands(false, true, ranges, all, tri), HandsAction::Untouched);
                EXPECT_EQ(PlanHands(true, false, ranges, all, tri), HandsAction::Untouched);
            }
            // All of it kept (arms_only, a sleeve's lower half that is all forearm): as it is.
            EXPECT_EQ(PlanHands(true, true, ranges, true, tri), HandsAction::Untouched);
        }
        EXPECT_EQ(PlanHands(true, true, 0, false, tri), HandsAction::Skip); // nothing kept
    }
    EXPECT_EQ(PlanHands(true, true, 3, false, true), HandsAction::Hands);
    // Kept ranges but not a triangle list: drawn whole (the hands never vanish).
    EXPECT_EQ(PlanHands(true, true, 3, false, false), HandsAction::Untouched);
}

// The generated hands table: sorted unique keys; each key's hand ranges and its hand-or-forearm
// ranges are triangles of its face set, in order and disjoint; every hand range lies inside the
// hand-or-forearm ranges; the hand ranges are never the whole face set (all-hand face sets have
// no key), lower_all exactly when the second list is all of it; every kind of the hunter's parts
// present, no weapon.
TEST(BodySplit, HandsTableIsConsistent) {
    const auto& keys = VrHandsTable::Keys;
    const auto& ranges = VrHandsTable::Ranges;
    ASSERT_FALSE(keys.empty());
    u32 next = 0;
    u32 hidden = 0;
    u32 no_hand = 0;
    u32 lower_all = 0;
    u32 gloves = 0;
    u32 sleeves = 0;
    u32 head_with_hands = 0;
    std::vector<std::string_view> prefixes;
    const auto walk = [&](const VrHandsTable::Key& k, u32 first, u32 n, u32& kept,
                          std::vector<std::pair<u32, u32>>& out) {
        u32 at = 0;
        for (u32 r = 0; r < n; ++r) {
            const auto& x = ranges[first + r];
            ASSERT_EQ(x.first % 3, 0u) << k.name;
            ASSERT_EQ(x.count % 3, 0u) << k.name;
            ASSERT_GT(x.count, 0u) << k.name;
            ASSERT_GE(x.first, at) << k.name; // in order, disjoint
            ASSERT_LE(x.first + x.count, k.count) << k.name;
            at = x.first + x.count;
            kept += x.count;
            out.emplace_back(x.first, x.count);
        }
    };
    for (size_t i = 0; i < keys.size(); ++i) {
        const auto& k = keys[i];
        if (i != 0) {
            const auto& p = keys[i - 1];
            ASSERT_TRUE(p.count < k.count || (p.count == k.count && p.hash < k.hash)) << k.name;
        }
        ASSERT_EQ(k.begin, next) << k.name;
        next += u32(k.hand) + k.lower;
        ASSERT_LE(next, ranges.size());
        u32 hand = 0;
        u32 lower = 0;
        std::vector<std::pair<u32, u32>> hr, lr;
        walk(k, k.begin, k.hand, hand, hr);
        walk(k, k.begin + k.hand, k.lower, lower, lr);
        const u32 whole = k.count - k.count % 3;
        ASSERT_LT(hand, whole) << k.name; // never all hand
        ASSERT_GE(lower, hand) << k.name;
        ASSERT_EQ(k.lower_all, lower == whole) << k.name;
        for (const auto& [first, count] : hr) { // every hand range inside a lower one
            const bool inside = std::any_of(lr.begin(), lr.end(), [&](const auto& l) {
                return first >= l.first && first + count <= l.first + l.second;
            });
            ASSERT_TRUE(inside) << k.name;
        }
        const std::string_view name{k.name};
        const std::string_view prefix = name.substr(0, 3);
        ASSERT_TRUE(prefix == "am_" || prefix == "bd_" || prefix == "lg_" || prefix == "hd_" ||
                    prefix == "fc_" || prefix == "fg_" || prefix == "hr_")
            << k.name;
        if (std::find(prefixes.begin(), prefixes.end(), prefix) == prefixes.end()) {
            prefixes.push_back(prefix);
        }
        hidden += k.lower == 0;
        no_hand += k.hand == 0;
        lower_all += k.lower_all;
        gloves += prefix == "am_" && k.hand != 0;
        sleeves += prefix == "bd_" && k.lower != 0;
        head_with_hands += (prefix == "hd_" || prefix == "fg_" || prefix == "hr_") && k.lower != 0;
    }
    EXPECT_EQ(next, ranges.size());
    EXPECT_EQ(prefixes.size(), 7u);  // all seven kinds of parts
    EXPECT_GT(hidden, 10000u);       // torsos, coats, legs, heads, hair
    EXPECT_GT(no_hand, hidden);      // sleeves and bracers: forearm only
    EXPECT_GT(lower_all, 500u);      // gloves with their cuffs
    EXPECT_GT(gloves, 200u);         // gloves whose cuffs leave with hands_only
    EXPECT_GT(sleeves, 500u);        // coats keep their forearm sleeves with arms_only
    EXPECT_LT(head_with_hands, 20u); // a head part with hand triangles is a shared face set
}

} // namespace

TEST(BodySplit, LegacyFullBodyHidesHeadInEyesAndPreservesShadow) {
    using VR::Body::BodyMode;
    // The holster launcher used legacy full; the menu's Full option uses no_head.
    // Neither may reveal the face/hat when the player leans out of the skull.
    for(auto mode:{BodyMode::Full,BodyMode::NoHead}) {
        const Frame frame{false,false,VR::Body::HeadHiddenByRendererFor(true,mode,true)};
        for(const auto& key:VrBodyTable::Keys) {
            if(!key.head) continue;
            const KeyShape shape{key.keep,key.cut,key.cape,key.head,key.head_only,key.shoulder};
            const auto eye=PlanDraw(shape,frame,true,true);
            EXPECT_TRUE(eye.action==Action::Skip || eye.action==Action::Split)<<key.name;
            if(eye.action==Action::Split) EXPECT_FALSE(eye.draw_head)<<key.name;
            const auto shadow=PlanDraw(shape,frame,false,true);
            EXPECT_EQ(shadow.action,Action::Untouched)<<key.name;
        }
    }
}
