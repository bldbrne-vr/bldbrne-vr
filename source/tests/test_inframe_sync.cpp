// SPDX-License-Identifier: GPL-2.0-or-later

// inframe_hand_sync (src/core/vr/inframe.h CarryBetween / ReView / RebaseList): the hand items
// carried with your play space from the update the pose was placed for (walking, turning; never
// your head), a view to view move, and which items of a list move (the hunter's hand items) and
// which stay (the arc's cards, labelled items of a mixed list).

#include <cmath>
#include <cstdint>
#include <random>
#include <gtest/gtest.h>
#include "core/vr/inframe.h"

namespace {

using namespace VR::InFrame;
using V3 = std::array<float, 3>;

// A render camera like the game's: rows right, up, forward (yaw + = right, pitch + = down).
ViewOf Camera(float yaw, float pitch, float roll, V3 position) {
    const float cy = std::cos(yaw), sy = std::sin(yaw), cp = std::cos(pitch),
                sp = std::sin(pitch), cr = std::cos(roll), sr = std::sin(roll);
    const V3 fwd{sy * cp, -sp, cy * cp};
    const V3 right0{cy, 0.0f, -sy};
    // up0 = fwd x right0 (y up for a level camera)
    const V3 up0{fwd[1] * right0[2] - fwd[2] * right0[1], fwd[2] * right0[0] - fwd[0] * right0[2],
                 fwd[0] * right0[1] - fwd[1] * right0[0]};
    V3 right{}, up{};
    for (int i = 0; i < 3; ++i) {
        right[i] = cr * right0[i] + sr * up0[i];
        up[i] = cr * up0[i] - sr * right0[i];
    }
    ViewOf v{};
    v.rows = {right[0], right[1], right[2], 0.0f, up[0], up[1], up[2], 0.0f,
              fwd[0],   fwd[1],   fwd[2],   0.0f};
    v.position = position;
    return v;
}

V3 ToView(const ViewOf& v, V3 w) {
    const float d[3] = {w[0] - v.position[0], w[1] - v.position[1], w[2] - v.position[2]};
    V3 out{};
    for (int i = 0; i < 3; ++i) {
        out[i] = v.rows[i * 4 + 0] * d[0] + v.rows[i * 4 + 1] * d[1] + v.rows[i * 4 + 2] * d[2];
    }
    return out;
}

V3 Apply(const std::array<float, 12>& mv, V3 m) {
    V3 out{};
    for (int i = 0; i < 3; ++i) {
        out[i] = mv[i * 4 + 0] * m[0] + mv[i * 4 + 1] * m[1] + mv[i * 4 + 2] * m[2] + mv[i * 4 + 3];
    }
    return out;
}

float Dist(V3 a, V3 b) {
    return std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
                     (a[2] - b[2]) * (a[2] - b[2]));
}

Item HandItem(V3 at, float size) {
    Item item{};
    item.hand = true;
    item.mv = {size, 0.0f, 0.0f, at[0], 0.0f, size, 0.0f, at[1], 0.0f, 0.0f, size, at[2]};
    return item;
}

TEST(InFrameSync, ReViewKeepsEveryWorldPoint) {
    std::mt19937 rng{7};
    std::uniform_real_distribution<float> ang(-3.0f, 3.0f), pit(-1.2f, 1.2f), pos(-50.0f, 50.0f);
    float worst = 0.0f;
    for (int n = 0; n < 2000; ++n) {
        const ViewOf a = Camera(ang(rng), pit(rng), 0.3f * pit(rng), {pos(rng), pos(rng), pos(rng)});
        const ViewOf b = Camera(ang(rng), pit(rng), 0.3f * pit(rng), {pos(rng), pos(rng), pos(rng)});
        const V3 w{pos(rng), pos(rng), pos(rng)};
        const ReView rv = ReView::Between(a, b);
        worst = std::max(worst, Dist(rv.Point(ToView(a, w)), ToView(b, w)));
    }
    EXPECT_LT(worst, 2e-4f); // (game units, points up to 170 away)
}

TEST(InFrameSync, ReViewedItemDrawsTheSameMeshPoints) {
    const ViewOf a = Camera(0.4f, 0.3f, 0.0f, {-199.7f, 10.2f, 174.7f});
    const ViewOf b = Camera(0.45f, 0.28f, 0.02f, {-199.6f, 10.21f, 174.75f});
    // A gauge-like item: 53 x 35 mm, turned, 0.5 m in front of camera a.
    Item item{};
    item.hand = true;
    item.mv = {0.0353f, 0.02f, 0.0f, 0.1f, -0.02f, 0.0353f, 0.0f, -0.2f, 0.0f, 0.0f, 0.04f, 0.45f};
    const ReView rv = ReView::Between(a, b);
    const Item moved = ReViewed(item, rv);
    for (const V3 m : {V3{0, 0, 0}, V3{0.5f, -0.5f, 0.1f}, V3{-0.5f, 0.5f, -0.1f}}) {
        const V3 in_a = Apply(item.mv, m);
        // the world point it is, seen from b
        V3 w{};
        for (int i = 0; i < 3; ++i) {
            w[i] = a.position[i] + a.rows[0 * 4 + i] * in_a[0] + a.rows[1 * 4 + i] * in_a[1] +
                   a.rows[2 * 4 + i] * in_a[2];
        }
        EXPECT_LT(Dist(Apply(moved.mv, m), ToView(b, w)), 1e-5f);
    }
}

PlayFrame Play(V3 eye, V3 head, float yaw) {
    PlayFrame f{};
    f.eye_world = eye;
    f.head = head;
    f.base_yaw = yaw;
    f.scale = 1.0f;
    return f;
}

// The lag this fixes: the VR body places the hunter's pose of update N for update N-1's play
// frame, and the frame shows his arm at your controller against its own camera. The hunter
// strafes right at 4.5 m/s at 45 fps (10 cm per update), his hand 0.45 m in front of the eye:
// placed from his dummies (before) the item is 10 cm to the left of the hand the frame shows;
// carried with the play space (inframe_hand_sync) it is on it.
TEST(InFrameSync, AStrafingHandsItemIsCarriedOntoTheHand) {
    const float step = 4.5f / 45.0f;
    const PlayFrame prev = Play({0.0f, 1.6f, 0.0f}, {0.0f, 1.6f, 0.0f}, 0.0f);
    const PlayFrame now = Play({step, 1.6f, 0.0f}, {0.0f, 1.6f, 0.0f}, 0.0f);
    const ViewOf cam = Camera(3.14159265f, 0.35f, 0.0f, {step, 1.6f, 0.0f});
    const V3 hand_t{-0.12f, 1.35f, -0.4f}; // your controller in tracking space
    const V3 placed = prev.ToWorld(hand_t);   // his dummy (placed for the previous frame)
    const V3 shown = ToView(cam, now.ToWorld(hand_t)); // the arm the frame shows
    EXPECT_NEAR(Dist(ToView(cam, placed), shown), step, 1e-5f);
    List list{};
    list.record = 101;
    list.Add(HandItem(ToView(cam, placed), 0.05f));
    Item card = HandItem(ToView(cam, placed), 0.09f);
    card.hand = false; // an arc card: in your play space already, never carried
    list.Add(card);
    List out{};
    ASSERT_TRUE(RebaseList(list, &list, CarryBetween(prev, now, cam), 101, out));
    ASSERT_EQ(out.count, 2u);
    // (mixed list: the card first, then the carried item)
    EXPECT_LT(Dist({out.items[1].mv[3], out.items[1].mv[7], out.items[1].mv[11]}, shown), 1e-5f);
    EXPECT_LT(Dist({out.items[0].mv[3], out.items[0].mv[7], out.items[0].mv[11]},
                   ToView(cam, placed)),
              1e-6f);
}

TEST(InFrameSync, CarryFollowsWalkingAndTurningButNotYourHead) {
    std::mt19937 rng{3};
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    float worst_head = 0.0f, worst_walk = 0.0f;
    for (int n = 0; n < 1000; ++n) {
        const V3 eye{300.0f * u(rng), 10.0f * u(rng), 300.0f * u(rng)}; // (the game's scale)
        const V3 head{0.3f * u(rng), 1.6f + 0.1f * u(rng), 0.3f * u(rng)};
        const float yaw = 3.0f * u(rng);
        const PlayFrame from = Play(eye, head, yaw);
        const ViewOf cam = Camera(3.0f * u(rng), 0.6f * u(rng), 0.0f, eye);
        const V3 x{eye[0] + 0.5f * u(rng), eye[1] - 0.3f, eye[2] + 0.5f * u(rng)};
        // Only your head moved (the eye with it, the play space stays): nothing moves.
        const V3 head2{head[0] + 0.05f * u(rng), head[1] + 0.03f * u(rng), head[2] + 0.05f * u(rng)};
        const V3 eye2 = from.ToWorld(head2);
        const ReView still = CarryBetween(from, Play(eye2, head2, yaw), cam);
        worst_head = std::max(worst_head, Dist(still.Point(ToView(cam, x)), ToView(cam, x)));
        // Walking and a smooth turn: a point fixed in your play space stays fixed in it.
        const PlayFrame to = Play({eye[0] + 0.1f * u(rng), eye[1] + 0.02f * u(rng),
                                   eye[2] + 0.1f * u(rng)},
                                  head, yaw + 0.05f * u(rng));
        const V3 t{0.2f * u(rng), 1.3f, -0.4f};
        const ReView walk = CarryBetween(from, to, cam);
        worst_walk = std::max(worst_walk, Dist(walk.Point(ToView(cam, from.ToWorld(t))),
                                               ToView(cam, to.ToWorld(t))));
    }
    EXPECT_LT(worst_head, 1e-4f);
    EXPECT_LT(worst_walk, 1e-4f);
}

TEST(InFrameSync, AllHandListIsReplacedWholeWithLabelsAndProbe) {
    const ReView rv = ReView::Between(Camera(0.1f, 0.2f, 0.0f, {1, 2, 3}),
                                      Camera(0.12f, 0.2f, 0.0f, {1.05f, 2, 3}));
    List from{};
    from.record = 11;
    from.producer = Producer::Vitals;
    Item digits = HandItem({0.1f, -0.2f, 0.5f}, 0.01f);
    digits.tex = Tex::Label;
    digits.tex_key = static_cast<std::uint32_t>(from.AddLabel("17"));
    digits.fit_label = 0;
    from.Add(HandItem({0.1f, -0.2f, 0.5f}, 0.05f));
    from.Add(digits);
    from.probe.id = 9;
    from.probe.count = 2;
    from.probe.points[0] = {0.1f, -0.2f, 0.5f};
    from.probe.points[1] = {0.11f, -0.19f, 0.52f};
    List to{};
    to.record = 10;
    to.producer = Producer::Vitals;
    to.Add(HandItem({0.0f, -0.2f, 0.5f}, 0.05f));
    List out{};
    ASSERT_TRUE(RebaseList(from, &to, rv, 10, out));
    EXPECT_EQ(out.record, 10u);
    EXPECT_EQ(out.producer, Producer::Vitals);
    ASSERT_EQ(out.count, 2u);
    EXPECT_EQ(out.label_count, 1u);
    EXPECT_EQ(out.items[1].tex, Tex::Label);
    EXPECT_EQ(out.items[1].fit_label, 0);
    EXPECT_EQ(out.probe.id, 9u);
    ASSERT_EQ(out.probe.count, 2u);
    const V3 p1 = rv.Point(from.probe.points[1]);
    EXPECT_LT(Dist(out.probe.points[1], p1), 1e-6f);
    const V3 c0 = rv.Point({0.1f, -0.2f, 0.5f});
    EXPECT_LT(Dist({out.items[0].mv[3], out.items[0].mv[7], out.items[0].mv[11]}, c0), 1e-6f);
}

TEST(InFrameSync, PlaySpaceListsAreLeftAlone) {
    const ReView rv{};
    List from{};
    from.record = 21;
    Item card{};
    card.mesh = MeshId::Plate; // the arc's card: placed in your play space, not on the hand
    from.Add(card);
    List out{};
    EXPECT_FALSE(RebaseList(from, nullptr, rv, 20, out));
    List to{};
    to.record = 20;
    to.Add(card);
    EXPECT_FALSE(RebaseList(from, &to, rv, 20, out));
}

TEST(InFrameSync, MixedListKeepsItsCardsAndLabelsAndGetsTheNewHandItems) {
    const ViewOf a = Camera(0.0f, 0.3f, 0.0f, {0.1f, 1.6f, 0.0f});
    const ViewOf b = Camera(0.0f, 0.3f, 0.0f, {0.0f, 1.6f, 0.0f});
    const ReView rv = ReView::Between(a, b);
    // The older record: two cards (one with a label) folding and the item as it was.
    List to{};
    to.record = 30;
    Item card{};
    card.mesh = MeshId::Plate;
    card.mv[3] = 0.3f;
    Item name{};
    name.tex = Tex::Label;
    name.tex_key = static_cast<std::uint32_t>(to.AddLabel("Molotov Cocktail"));
    to.Add(card);
    to.Add(name);
    to.Add(HandItem({-0.1f, -0.2f, 0.45f}, 0.3f)); // the bottle from the older pose
    // The newer record: the same cards placed for its camera and the bottle from the newer pose.
    List from{};
    from.record = 31;
    from.AddLabel("Molotov Cocktail");
    Item card_b = card;
    card_b.mv[3] = 0.2f;
    from.Add(card_b);
    from.Add(name);
    from.Add(HandItem({-0.1f, -0.2f, 0.45f}, 0.3f));
    List out{};
    ASSERT_TRUE(RebaseList(from, &to, rv, 30, out));
    ASSERT_EQ(out.count, 3u);
    EXPECT_FLOAT_EQ(out.items[0].mv[3], 0.3f); // the older record's own card, unmoved
    EXPECT_EQ(out.items[1].tex, Tex::Label);   // its label kept
    EXPECT_EQ(out.label_count, 1u);
    EXPECT_TRUE(out.items[2].hand); // the new bottle, re-viewed (0.1 units right of b's eye)
    EXPECT_NEAR(out.items[2].mv[3], -0.1f + 0.1f, 1e-6f);
    // A mixed newer list with nothing for the older record: left alone.
    EXPECT_FALSE(RebaseList(from, nullptr, rv, 30, out));
}

// What is kept from his pose and not drawn as a hand item (the vial's depth proxy, the arc's
// hover point, the use ring's place, the throw's start): CarryPoint moves a world point as
// CarryBetween moves the hand items in the view, and the point lands in the play space his pose
// was placed for (a hand held still in your play space while you walk: the same hover point).
TEST(InFrameSync, CarryPointIsTheViewsCarryAndKeepsTheHandInYourPlaySpace) {
    std::mt19937 rng{11};
    std::uniform_real_distribution<float> u(-1.0f, 1.0f);
    float worst_view = 0.0f, worst_local = 0.0f, worst_world = 0.0f;
    for (int n = 0; n < 1000; ++n) {
        const V3 eye{300.0f * u(rng), 10.0f * u(rng), 300.0f * u(rng)}; // (the game's scale)
        const V3 head{0.3f * u(rng), 1.6f + 0.1f * u(rng), 0.3f * u(rng)};
        const float yaw = 3.0f * u(rng);
        const PlayFrame from = Play(eye, head, yaw);
        const PlayFrame to = Play({eye[0] + 0.15f * u(rng), eye[1] + 0.02f * u(rng),
                                   eye[2] + 0.15f * u(rng)},
                                  {head[0] + 0.02f * u(rng), head[1], head[2] + 0.02f * u(rng)},
                                  yaw + 0.05f * u(rng));
        const ViewOf cam = Camera(3.0f * u(rng), 0.6f * u(rng), 0.0f, to.eye_world);
        const V3 t{0.25f * u(rng), 1.2f + 0.2f * u(rng), -0.45f + 0.1f * u(rng)}; // your hand
        const V3 placed = from.ToWorld(t); // his dummy, placed for the previous update
        const V3 carried = CarryPoint(from, to, placed);
        worst_view = std::max(worst_view, Dist(ToView(cam, carried),
                                               CarryBetween(from, to, cam).Point(ToView(cam, placed))));
        worst_local = std::max(worst_local, Dist(to.ToTracking(carried), t));
        worst_world = std::max(worst_world, Dist(carried, to.ToWorld(t)));
    }
    EXPECT_LT(worst_view, 1e-4f);
    EXPECT_LT(worst_local, 1e-4f);
    EXPECT_LT(worst_world, 1e-4f);
}

// The throw's start and the use ring: a strafe at run speed (4.5 m/s, 45 updates a second): the
// place kept from his pose was one update's strafe (10 cm) from the item the frame showed; carried,
// it is on it.
TEST(InFrameSync, AStrafingThrowStartsWhereTheItemWasDrawn) {
    const float step = 4.5f / 45.0f;
    const PlayFrame prev = Play({200.0f, 1.6f, -50.0f}, {0.1f, 1.6f, 0.0f}, 0.4f);
    const PlayFrame now = Play({200.0f + step * std::cos(0.4f), 1.6f, -50.0f - step * std::sin(0.4f)},
                               {0.1f, 1.6f, 0.0f}, 0.4f);
    const V3 item_t{-0.18f, 1.25f, -0.35f}; // the held item, in your play space
    const V3 kept = prev.ToWorld(item_t);   // from his palm dummy
    const V3 drawn = now.ToWorld(item_t);   // where the frame shows it (the hand sync)
    EXPECT_NEAR(Dist(kept, drawn), step, 1e-4f);
    EXPECT_LT(Dist(CarryPoint(prev, now, kept), drawn), 1e-4f);
    // No carry (standing still): the point stays.
    EXPECT_LT(Dist(CarryPoint(prev, prev, kept), kept), 1e-4f);
}

// The same gate as the hand items: a teleport, a snap turn or a scale change is not carried;
// standing still moves nothing.
TEST(InFrameSync, CarryStepSkipsTeleportsSnapTurnsAndScaleChanges) {
    const PlayFrame a = Play({100.0f, 2.0f, 100.0f}, {0.0f, 1.6f, 0.0f}, 3.1f);
    const auto walk = CarryStepOf(a, Play({100.1f, 2.0f, 100.0f}, {0.0f, 1.6f, 0.0f}, 3.1f));
    EXPECT_TRUE(walk.apply);
    EXPECT_FALSE(walk.still);
    EXPECT_NEAR(walk.moved_m, 0.1f, 1e-3f);
    const auto still = CarryStepOf(a, a);
    EXPECT_TRUE(still.apply);
    EXPECT_TRUE(still.still);
    // Your head alone moved (the eye with it): the play space did not.
    const V3 head2{0.05f, 1.62f, -0.03f};
    const auto head = CarryStepOf(a, Play(a.ToWorld(head2), head2, 3.1f));
    EXPECT_TRUE(head.apply);
    EXPECT_LT(head.moved_m, 1e-4f); // (float at the game's scale)
    EXPECT_FALSE(CarryStepOf(a, Play({101.5f, 2.0f, 100.0f}, {0.0f, 1.6f, 0.0f}, 3.1f)).apply);
    EXPECT_FALSE(CarryStepOf(a, Play({100.0f, 2.0f, 100.0f}, {0.0f, 1.6f, 0.0f}, 3.1f - 0.52f)).apply);
    // A smooth turn across the angle's wrap (3.1 -> -3.1 rad: 4.7 degrees).
    const auto wrap = CarryStepOf(a, Play({100.0f, 2.0f, 100.0f}, {0.0f, 1.6f, 0.0f}, -3.1f));
    EXPECT_TRUE(wrap.apply);
    EXPECT_NEAR(wrap.turn_rad, 6.2831853f - 6.2f, 1e-3f);
    PlayFrame scaled = a;
    scaled.scale = 1.1f;
    EXPECT_FALSE(CarryStepOf(a, scaled).apply);
}


TEST(InFrameVisibility, KeepsEdgesAndUnknownBoundsButRejectsWhollyOutsideSpheres) {
    Visibility view(1.5707963f, 1.f);
    EXPECT_TRUE(view.MaySee({0,0,1,.1f}));
    EXPECT_TRUE(view.MaySee({1.1f,0,1,.1f})); // intersects edge although centre is outside
    EXPECT_FALSE(view.MaySee({1.3f,0,1,.1f}));
    EXPECT_FALSE(view.MaySee({0,-2,1,.1f}));
    EXPECT_FALSE(view.MaySee({0,0,-1,.1f}));
    EXPECT_TRUE(view.MaySee({0,0,-.05f,.1f})); // camera intersects the sphere
    EXPECT_TRUE(view.MaySee({100,100,-1,0})); // legacy / unknown bound
    EXPECT_TRUE(Visibility(0,1).MaySee({100,100,-1,.1f}));
    EXPECT_TRUE(Visibility(1,0).MaySee({100,100,-1,.1f}));
}
TEST(InFrameVisibility, CompactionPreservesOrderLabelsAndMixedLegacyItems) {
    List list{};
    list.AddLabel("kept");
    Item outside{}, visible{}, legacy{};
    outside.cull_sphere = {0,-2,1,.1f}; outside.tex_key = 1;
    visible.cull_sphere = {0,0,1,.1f}; visible.tex_key = 2;
    legacy.tex_key = 3;
    list.Add(outside); list.Add(visible); list.Add(outside); list.Add(legacy);
    EXPECT_EQ(CullInvisible(list,1.5707963f,1),2u);
    EXPECT_EQ(list.items[0].tex_key,2u); EXPECT_EQ(list.items[1].tex_key,3u);
    EXPECT_EQ(list.label_count,1u);
    List hidden{}; hidden.Add(outside); hidden.Add(outside);
    EXPECT_EQ(CullInvisible(hidden,1.5707963f,1),0u);
}
TEST(InFrameVisibility, SphereMovesWithItsItemWhenRebased) {
    Item item{}; item.cull_sphere={1,2,3,.2f};
    ReView rv{}; rv.b={4,-1,2};
    const auto result=ReViewed(item,rv);
    EXPECT_EQ(result.cull_sphere,(std::array<float,4>{5,1,5,.2f}));
}
TEST(InFrameVisibility, NeverDropsASphereWithAnySampleInsideTheProjection) {
    std::mt19937 rng(417);
    std::uniform_real_distribution<float> u(-1,1);
    for(float aspect : {.75f,1.f,1.8f}) {
        const float vfov=1.5f, ty=std::tan(vfov*.5f), tx=ty*aspect;
        Visibility view(vfov,aspect);
        for(int i=0;i<3000;++i) {
            const std::array<float,4> s{u(rng)*2,u(rng)*2,u(rng)*2,.2f};
            for(int point=0;point<24;++point) {
                float x=u(rng),y=u(rng),z=u(rng);
                const float length=std::sqrt(x*x+y*y+z*z);
                if(length<.001f) continue;
                x=s[0]+x*.2f/length; y=s[1]+y*.2f/length; z=s[2]+z*.2f/length;
                if(z>0 && std::abs(x)<=z*tx && std::abs(y)<=z*ty)
                    ASSERT_TRUE(view.MaySee(s));
            }
        }
    }
}

// The central frustum alone must not hide equipment visible at either native eye edge.
TEST(InFrameVisibility, NativeStereoKeepsEitherEyeAndAsymmetricOuterEdges) {
    const float vfov = 1.5707963f;
    Visibility mono(vfov, 1.f);
    Visibility stereo(vfov, 1.f, .032f);
    for (float side : {-1.f, 1.f}) {
        const std::array<float, 4> sphere{side * .13f, 0, .1f, .002f};
        EXPECT_FALSE(mono.MaySee(sphere));
        EXPECT_TRUE(stereo.MaySee(sphere));
    }
    // Eye remap scale .8 / offset +-.2 encloses tangents out to 1.5, not 1.0.
    Visibility asymmetric(vfov, 1.f, .032f, 1.5f, 1.3f);
    EXPECT_TRUE(asymmetric.MaySee({1.45f, 0, 1.f, .01f}));
    EXPECT_TRUE(asymmetric.MaySee({-1.45f, 0, 1.f, .01f}));
    EXPECT_TRUE(asymmetric.MaySee({0, 1.25f, 1.f, .01f}));
    EXPECT_FALSE(asymmetric.MaySee({2.f, 0, 1.f, .01f}));
}

} // namespace
