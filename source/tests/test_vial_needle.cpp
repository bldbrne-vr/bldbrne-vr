// SPDX-License-Identifier: GPL-2.0-or-later

// The needle on the Blood Vial (src/core/vr/vial_needle.h): the stabbing end and the axis found
// from a model's vertices, the needle's and the hub's rows from the vial's (where the point lands,
// how thick, out of which end, through any pose), the fist in their meshes, its least width on
// screen and how round it is shaded there, and when it is drawn (exactly while our vial is:
// held_item.h's decision included).

#include <array>
#include <cmath>
#include <vector>
#include <gtest/gtest.h>
#include "core/vr/held_item.h"
#include "core/vr/vial_needle.h"

namespace {

using namespace VR::Held;

// A synthetic vial (not the game's): a cylinder of radius `r` round (cx, cz) from y0 to y1, a
// rounded shoulder of radius r / 2 at the bottom, `n` vertices round, stride 8 floats (x y z,
// normal, uv) like the renderer's model vertices.
struct Synthetic {
    std::vector<float> data;
    std::array<float, 3> min{1e9f, 1e9f, 1e9f};
    std::array<float, 3> max{-1e9f, -1e9f, -1e9f};
    std::size_t count{0};
    void Add(float x, float y, float z) {
        data.insert(data.end(), {x, y, z, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f});
        const float p[3] = {x, y, z};
        for (int i = 0; i < 3; ++i) {
            min[i] = std::min(min[i], p[i]);
            max[i] = std::max(max[i], p[i]);
        }
        ++count;
    }
};

Synthetic Cylinder(float cx, float cz, float r, float y0, float y1, int n = 16) {
    Synthetic s{};
    for (int i = 0; i < n; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / static_cast<float>(n);
        const float c = std::cos(a), d = std::sin(a);
        s.Add(cx + 0.5f * r * c, y0, cz + 0.5f * r * d); // the bottom face's rim (half width)
        s.Add(cx + r * c, y0 + 0.01f, cz + r * d);       // the shoulder
        s.Add(cx + r * c, y1, cz + r * d);               // the top
    }
    return s;
}

Rows Identity() {
    return {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
}

// Rows [R | t] of a rotation about the axis (unit) by `deg`, a uniform scale and a translation.
Rows Pose(std::array<float, 3> axis, float deg, float scale, std::array<float, 3> t) {
    const float a = deg * 3.14159265f / 180.0f;
    const float c = std::cos(a), s = std::sin(a), k = 1.0f - c;
    const float l = std::hypot(axis[0], axis[1], axis[2]);
    const float x = axis[0] / l, y = axis[1] / l, z = axis[2] / l;
    const float r[9] = {c + x * x * k,     x * y * k - z * s, x * z * k + y * s,
                        y * x * k + z * s, c + y * y * k,     y * z * k - x * s,
                        z * x * k - y * s, z * y * k + x * s, c + z * z * k};
    return {r[0] * scale, r[1] * scale, r[2] * scale, t[0], r[3] * scale, r[4] * scale,
            r[5] * scale, t[1],          r[6] * scale, r[7] * scale, r[8] * scale, t[2]};
}

float Dist(const std::array<float, 3>& a, const std::array<float, 3>& b) {
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}

std::array<float, 3> Column(const Rows& m, int c) {
    return {m[c], m[4 + c], m[8 + c]};
}

float Length(const std::array<float, 3>& v) {
    return std::hypot(v[0], v[1], v[2]);
}

TEST(VialNeedle, TheRootIsTheBottomFaceOnTheAxis) {
    const Synthetic s = Cylinder(0.002f, -0.003f, 0.019f, -0.060f, 0.116f);
    const NeedleRoot r = NeedleRootOf(s.data.data(), s.count, 8, s.min, s.max);
    ASSERT_TRUE(r.valid);
    EXPECT_NEAR(r.x, 0.002f, 1e-5f);
    EXPECT_NEAR(r.z, -0.003f, 1e-5f);
    EXPECT_NEAR(r.y, -0.060f, 1e-6f);
    EXPECT_NEAR(r.face_r, 0.0095f, 1e-4f); // the face only, not the shoulder 1 cm up
    EXPECT_NEAR(r.length, 0.176f, 1e-5f);
    // The top end (end = +1).
    const NeedleRoot top = NeedleRootOf(s.data.data(), s.count, 8, s.min, s.max, 1.0f);
    EXPECT_NEAR(top.y, 0.116f, 1e-6f);
    EXPECT_NEAR(top.face_r, 0.019f, 1e-4f);
}

TEST(VialNeedle, NoModelNoRoot) {
    EXPECT_FALSE(NeedleRootOf(nullptr, 0, 8, {0, 0, 0}, {1, 1, 1}).valid);
    const float one[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    EXPECT_FALSE(NeedleRootOf(one, 1, 8, {0, 0, 0}, {0, 0, 0}).valid); // flat: no height
}

TEST(VialNeedle, ThePointLandsPastTheHubAlongTheAxis) {
    const Synthetic s = Cylinder(0.0f, 0.0f, 0.019f, -0.060f, 0.116f);
    const NeedleRoot root = NeedleRootOf(s.data.data(), s.count, 8, s.min, s.max);
    const NeedleLook look{};
    const Rows needle = NeedlePartRows(Identity(), root, look, NeedlePart::Needle);
    const Rows hub = NeedlePartRows(Identity(), root, look, NeedlePart::Hub);
    // The hub starts inside the bottom face; the needle shows `length_m` past the hub's tip.
    EXPECT_NEAR(hub[7], -0.060f + look.inset_m, 1e-6f);
    const auto hub_tip = RowsPoint(hub, {0.0f, -1.0f, 0.0f});
    EXPECT_NEAR(hub_tip[1], -0.060f + look.inset_m - look.hub_length_m, 1e-6f);
    const auto point = NeedlePoint(needle);
    EXPECT_NEAR(point[0], 0.0f, 1e-6f);
    EXPECT_NEAR(point[2], 0.0f, 1e-6f);
    EXPECT_NEAR(point[1], hub_tip[1] - look.length_m, 1e-6f);
    // Its root is inside the hub (no seam at the hub's tip).
    EXPECT_GT(needle[7], hub_tip[1]);
    EXPECT_LT(needle[7], hub[7]);
    // Thin and small: 1 mm across (radius 0.5 of the unit mesh), the hub 4.5 mm.
    EXPECT_NEAR(Length(Column(needle, 0)) * 0.5f, 0.0005f, 1e-7f);
    EXPECT_NEAR(Length(Column(needle, 2)) * 0.5f, 0.0005f, 1e-7f);
    EXPECT_NEAR(Length(Column(hub, 0)) * 0.5f, 0.00225f, 1e-7f);
    // The sizes asked for: 0.8 .. 1.2 mm thin, 1.5 .. 2.5 cm long.
    EXPECT_GE(look.diameter_m, 0.0008f);
    EXPECT_LE(look.diameter_m, 0.0012f);
    EXPECT_GE(look.length_m, 0.015f);
    EXPECT_LE(look.length_m, 0.025f);
}

// A vial whose end tapers to a point (as the game's does): the hub's root goes in to where the
// end is as wide as it, so it sits flush on the taper instead of floating round its tip.
TEST(VialNeedle, TheHubSitsFlushOnATaperedEnd) {
    Synthetic s{};
    const float y0 = -0.073f;
    for (int i = 0; i < 12; ++i) {
        const float a = 6.2831853f * static_cast<float>(i) / 12.0f;
        const float c = std::cos(a), d = std::sin(a);
        s.Add(0.0f, y0, 0.0f);                          // the point
        s.Add(0.004f * c, y0 + 0.005f, 0.004f * d);     // 8 mm across 5 mm in
        s.Add(0.019f * c, y0 + 0.020f, 0.019f * d);     // full width 2 cm in
        s.Add(0.019f * c, 0.103f, 0.019f * d);          // the top
    }
    const NeedleRoot root = NeedleRootOf(s.data.data(), s.count, 8, s.min, s.max);
    ASSERT_TRUE(root.valid);
    EXPECT_NEAR(root.face_r, 0.0f, 1e-6f);
    // Along the taper's edge: 2.25 mm of radius is 2.8 mm in.
    EXPECT_NEAR(root.InsetFor(0.00225f), 0.005f * 0.00225f / 0.004f, 1e-6f);
    EXPECT_NEAR(root.InsetFor(0.0f), 0.0f, 1e-7f);
    EXPECT_NEAR(root.InsetFor(0.019f), 0.020f, 1e-6f);
    // Wider than the model anywhere near its end: the profile's span.
    EXPECT_NEAR(root.InsetFor(0.05f), NeedleRoot::ProfileStep * NeedleRoot::ProfileCount, 1e-6f);
    const NeedleLook look{};
    const Rows hub = NeedlePartRows(Identity(), root, look, NeedlePart::Hub);
    EXPECT_NEAR(hub[7], y0 + root.InsetFor(0.5f * look.hub_diameter_m) + look.inset_m, 1e-6f);
    // The needle still shows its full length past the hub's tip.
    const auto hub_tip = RowsPoint(hub, {0.0f, -1.0f, 0.0f});
    const auto point = NeedlePoint(NeedlePartRows(Identity(), root, look, NeedlePart::Needle));
    EXPECT_NEAR(hub_tip[1] - point[1], look.length_m, 1e-6f);
}

TEST(VialNeedle, TheTopEndPointsUp) {
    const Synthetic s = Cylinder(0.0f, 0.0f, 0.019f, -0.060f, 0.116f);
    NeedleLook look{};
    look.end = 1.0f;
    const NeedleRoot root = NeedleRootOf(s.data.data(), s.count, 8, s.min, s.max, look.end);
    const auto point = NeedlePoint(NeedlePartRows(Identity(), root, look, NeedlePart::Needle));
    EXPECT_NEAR(point[1], 0.116f - look.inset_m + look.hub_length_m + look.length_m, 1e-6f);
}

TEST(VialNeedle, ItFollowsTheVialThroughAnyPose) {
    const Synthetic s = Cylinder(0.001f, 0.002f, 0.019f, -0.060f, 0.116f);
    const NeedleRoot root = NeedleRootOf(s.data.data(), s.count, 8, s.min, s.max);
    const NeedleLook look{};
    const Rows ident = NeedlePartRows(Identity(), root, look, NeedlePart::Needle);
    const auto point0 = NeedlePoint(ident);
    const auto base0 = RowsPoint(ident, {0.0f, 0.0f, 0.0f});
    for (const float deg : {0.0f, 37.0f, 90.0f, 181.0f, -120.0f}) {
        const std::array<float, 3> axis{0.267f, 0.535f, 0.802f};
        const Rows vial = Pose(axis, deg, 1.0f, {0.3f, -0.4f, 0.55f});
        const Rows needle = NeedlePartRows(vial, root, look, NeedlePart::Needle);
        // The point is the vial's transform of the point on the unposed vial: it is rigidly on it.
        const auto expect = RowsPoint(vial, point0);
        const auto point = NeedlePoint(needle);
        EXPECT_LT(Dist(point, expect), 1e-5f) << deg;
        // Along the vial's axis (its -Y), the needle's length away from its root.
        const auto base = RowsPoint(needle, {0.0f, 0.0f, 0.0f});
        EXPECT_LT(Dist(base, RowsPoint(vial, base0)), 1e-5f) << deg;
        const auto down = Column(vial, 1);
        const float along = (point[0] - base[0]) * -down[0] + (point[1] - base[1]) * -down[1] +
                            (point[2] - base[2]) * -down[2];
        EXPECT_NEAR(along, Dist(point, base), 1e-5f) << deg;
    }
    // The vial's scale (drain.scale) scales the needle with it.
    const Rows big = Pose({0.0f, 1.0f, 0.0f}, 0.0f, 2.0f, {0.0f, 0.0f, 0.0f});
    const Rows needle = NeedlePartRows(big, root, look, NeedlePart::Needle);
    EXPECT_NEAR(Length(Column(needle, 0)), 2.0f * look.diameter_m, 1e-7f);
}

TEST(VialNeedle, TheFistInThePartsMesh) {
    const Synthetic s = Cylinder(0.0f, 0.0f, 0.019f, -0.060f, 0.116f);
    const NeedleRoot root = NeedleRootOf(s.data.data(), s.count, 8, s.min, s.max);
    const NeedleLook look{};
    const Rows vial = Pose({0.0f, 0.6f, 0.8f}, 70.0f, 1.0f, {0.1f, -0.3f, 0.4f});
    for (const NeedlePart part : {NeedlePart::Hub, NeedlePart::Needle}) {
        const Rows rows = NeedlePartRows(vial, root, look, part);
        const auto fist = VialPointInPart(vial, rows, {0.0f, 0.0f, 0.0f});
        // Mapped back through the part's rows it is the vial's origin (the grip) in view.
        EXPECT_LT(Dist(RowsPoint(rows, fist), RowsPoint(vial, {0.0f, 0.0f, 0.0f})), 1e-5f);
    }
}

TEST(VialNeedle, ThinOnScreenIsDrawnWiderAndFainter) {
    // A 2560 x 1440 frame, a 100 degree vertical field: one pixel at 0.4 m is about 0.66 mm.
    const float p11 = 1.0f / std::tan(0.5f * 100.0f * 3.14159265f / 180.0f);
    // At 40 cm the 0.5 mm radius is over 0.7 pixels: as is.
    EXPECT_FLOAT_EQ(NeedleWiden(0.0005f, 0.4f, p11, 1440.0f, 0.7f), 1.0f);
    // At 1.5 m it is under: widened to 0.7 pixels, its opacity 1 / k.
    const float k = NeedleWiden(0.0005f, 1.5f, p11, 1440.0f, 0.7f);
    EXPECT_GT(k, 1.5f);
    const float pixel = 2.0f * 1.5f / (p11 * 1440.0f);
    EXPECT_NEAR(0.0005f * k, 0.7f * pixel, 1e-7f);
    // Nothing to widen without a size.
    EXPECT_FLOAT_EQ(NeedleWiden(0.0f, 1.0f, p11, 1440.0f, 0.7f), 1.0f);
    EXPECT_FLOAT_EQ(NeedleWiden(0.0005f, 1.0f, p11, 0.0f, 0.7f), 1.0f);
}

// Over Link (the mock at the user's limits): the game's frame is 1808 high, its eye crop 1353 rows
// for a 99 degree field (44 up, 55 down): p11 = 2 / (2.394 x 1808 / 1353). At arm's length (45 cm)
// the 1 mm needle is about 0.63 pixels round its axis: drawn 2 pixels wide at 0.63 of its opacity.
// At 15 cm (the needle at the forearm, close to the eye) it is its own width.
TEST(VialNeedle, AtArmsLengthOverLinkItIsTheLeastWidth) {
    const float span = (std::tan(44.0f * 3.14159265f / 180.0f) +
                        std::tan(55.0f * 3.14159265f / 180.0f)) * 1808.0f / 1353.0f;
    const float p11 = 2.0f / span;
    const NeedleLook look{};
    const float r = 0.5f * look.diameter_m;
    const float pixel = 2.0f * 0.45f / (p11 * 1808.0f);
    EXPECT_NEAR(r / pixel, 0.63f, 0.03f);
    const float k = NeedleWiden(r, 0.45f, p11, 1808.0f, NeedleMinHalfPx);
    EXPECT_NEAR(r * k, NeedleMinHalfPx * pixel, 1e-7f);
    EXPECT_NEAR(1.0f / k, 0.63f, 0.03f); // its opacity: the coverage it really has
    EXPECT_FLOAT_EQ(NeedleWiden(r, 0.15f, p11, 1808.0f, NeedleMinHalfPx), 1.0f);
    // At least 2 pixels wide, never wider than the needle where it is wider.
    EXPECT_GE(NeedleMinHalfPx, 1.0f);
}

// The highlight's line is only drawn round where the needle is wide enough to show it: an even
// line up to 1 pixel of half width (the least drawn), fully round from 3, a blend in between.
TEST(VialNeedle, ANarrowNeedleIsOneEvenLine) {
    EXPECT_FLOAT_EQ(NeedleRoundness(NeedleMinHalfPx), 0.0f);
    EXPECT_FLOAT_EQ(NeedleRoundness(0.5f), 0.0f);
    EXPECT_FLOAT_EQ(NeedleRoundness(2.0f), 0.5f);
    EXPECT_FLOAT_EQ(NeedleRoundness(3.0f), 1.0f);
    EXPECT_FLOAT_EQ(NeedleRoundness(12.0f), 1.0f);
    float last = 0.0f;
    for (float w = 0.0f; w < 5.0f; w += 0.25f) {
        EXPECT_GE(NeedleRoundness(w), last) << w;
        last = NeedleRoundness(w);
    }
}

TEST(VialNeedle, ShownExactlyWhileOurVialIs) {
    NeedleInputs in{};
    in.model = true;
    // Nothing in the hand, or the game's own vial (the drain off: it does not sit where our model
    // does, so a needle from our model would float beside it): none.
    EXPECT_FALSE(NeedleVisible(in));
    // Our vial (the drain on: armed, drawn, draining, the empty hold).
    in.ours = true;
    EXPECT_TRUE(NeedleVisible(in));
    // vial_needle=0, the test key, a menu's camera, the model not loaded: none.
    in.setting = false;
    EXPECT_FALSE(NeedleVisible(in));
    in.setting = true;
    in.tuned_off = true;
    EXPECT_FALSE(NeedleVisible(in));
    in.tuned_off = false;
    in.menu = true;
    EXPECT_FALSE(NeedleVisible(in));
    in.menu = false;
    in.model = false;
    EXPECT_FALSE(NeedleVisible(in));
}

// The vial's own rules decide the needle's: whenever the held decision does not draw our vial
// (put away, the gun's trigger, a two-handed attack, a menu, a cinematic, an event, death, an
// item's own animation), the needle is not drawn either.
TEST(VialNeedle, HiddenWheneverTheHeldDecisionHidesTheVial) {
    const Policy p{};
    Inputs base{};
    base.request = {Kind::Vial, FlagDrain, 1000};
    base.age_ms = 5;
    const auto needle = [&](const Inputs& in) {
        NeedleInputs n{};
        n.model = true;
        n.ours = Decide(in, p).ours;
        return NeedleVisible(n);
    };
    EXPECT_TRUE(needle(base)); // armed: shown
    Inputs in = base;
    in.request.kind = Kind::None; // put away
    EXPECT_FALSE(needle(in));
    in = base;
    in.request.flags |= FlagGun; // the left trigger: the gun is in his hand
    EXPECT_FALSE(needle(in));
    in = base;
    in.gate_valid = true;
    in.held = true;
    in.owner = VR::Body::AnimClass::AttackBoth; // two-handed: that hand is on the hilt
    EXPECT_FALSE(needle(in));
    in.owner = VR::Body::AnimClass::AttackLeft; // a gun attack
    EXPECT_FALSE(needle(in));
    in = base;
    in.menu = true;
    EXPECT_FALSE(needle(in));
    in = base;
    in.cinematic = true;
    EXPECT_FALSE(needle(in));
    in = base;
    in.gate_valid = true;
    in.reason = VR::Body::GateReason::Event;
    EXPECT_FALSE(needle(in));
    in = base;
    in.hp = 0;
    EXPECT_FALSE(needle(in));
    in = base;
    in.item_use_state = 1;
    EXPECT_FALSE(needle(in));
    in = base;
    in.age_ms = 1000; // the presenter stopped asking
    EXPECT_FALSE(needle(in));
}

} // namespace

TEST(VialNeedle, NeedleLengthChangesTheLookButNeverHubContact) {
    const auto model=Cylinder(.002f,-.003f,.019f,-.060f,.116f);
    const auto root=NeedleRootOf(model.data.data(),model.count,8,model.min,model.max);
    for(float angle : {0.f,45.f,90.f,180.f}) for(float scale : {.75f,1.f,1.4f}) {
        const auto rows=Pose({1,2,3},angle,scale,{.2f,1.1f,-.4f});
        NeedleLook old_look{}, look{};
        old_look.length_m=.028f; look.length_m=AntiqueNeedleLength;
        const auto old_tip=NeedlePoint(NeedlePartRows(rows,root,old_look,NeedlePart::Needle));
        const auto tip=NeedlePoint(NeedlePartRows(rows,root,look,NeedlePart::Needle));
        EXPECT_NEAR(Dist(tip,old_tip),(AntiqueNeedleLength-.028f)*scale,1e-6f);
        const auto old_hub=RowsPoint(NeedlePartRows(rows,root,old_look,NeedlePart::Hub),{0,-1,0});
        const auto hub=RowsPoint(NeedlePartRows(rows,root,look,NeedlePart::Hub),{0,-1,0});
        EXPECT_NEAR(Dist(old_hub,hub),0.f,1e-7f);
        const auto plunger=RowsPoint(rows,{root.x,model.max[1],root.z});
        EXPECT_GT(Dist(tip,plunger),.21f*scale);
    }
}
