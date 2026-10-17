// SPDX-License-Identifier: GPL-2.0-or-later

#include <gtest/gtest.h>
#include "core/vr/grip_preview.h"
#include "core/vr/body_presets.h"
#include "core/vr/body_frame.h"

TEST(GripPreview, CatalogInspectionNeverUsesOrEquipsInventory) {
    for (const int goods : VR::Quick::QuickUsableGoods) {
        const auto out = VR::GripPreview::Display(goods, 1'000'000'000);
        EXPECT_FALSE(out.send) << goods;
        EXPECT_FALSE(out.square_hold) << goods;
        EXPECT_FALSE(out.square_press) << goods;
        EXPECT_FALSE(out.l2_press) << goods;
        EXPECT_FALSE(out.stab_item) << goods;
        EXPECT_FALSE(out.draw_vial) << goods;
        EXPECT_EQ(out.haptic_count, 0) << goods;
        EXPECT_TRUE(out.block_l2) << goods;
        EXPECT_EQ(out.scene.held_slot, -1) << goods;
        EXPECT_EQ(out.scene.hold_from_ns, 0) << goods;
        EXPECT_FALSE(out.scene.holsters_enabled) << goods;
        EXPECT_EQ(out.crank_box, goods == 700) << goods;
        if (goods != 1000) {
            EXPECT_EQ(out.held_goods, goods);
            EXPECT_EQ(out.scene.held_goods, goods);
            EXPECT_EQ(out.scene.state, VR::Quick::SceneState::Holding);
        }
    }
}

TEST(GripPreview, VialUsesItsDedicatedPathAndEmptySelectionDrawsNothing) {
    for (const int goods : {-1, 1000}) {
        const auto out = VR::GripPreview::Display(goods, 42);
        EXPECT_EQ(out.scene.state, VR::Quick::SceneState::Closed);
        EXPECT_EQ(out.scene.held_goods, -1);
        EXPECT_EQ(out.held_goods, -1);
        EXPECT_FALSE(out.send);
    }
}

TEST(GripPreview, InspectionPosesKeepTheSameHandPositionAndAreExplicitOnly) {
    namespace P = VR::BodyPresets;
    const auto standard = P::ParseTimeline("all");
    EXPECT_EQ(standard.size(), P::Presets.size());
    const auto origin = P::Evaluate(P::GripPreviews[0], 0, 0);
    for (const auto& preset : P::GripPreviews) {
        ASSERT_EQ(P::Find(preset.name), &preset);
        const auto hand = P::Evaluate(preset, 0, 0);
        EXPECT_EQ(hand.p, origin.p);
        float length = 0;
        for (const float v : hand.q) length += v*v;
        EXPECT_NEAR(length, 1.0f, 1e-5f);
        EXPECT_FALSE(preset.grip_right);
        EXPECT_EQ(P::Evaluate(preset, 1, 0).p, P::Evaluate(P::GripPreviews[0], 1, 0).p);
        for (const auto* p : standard) EXPECT_NE(p, &preset);
    }
}

#include "core/vr/item_attachment.h"
#include "core/vr/hand_grip.h"
#include "core/vr/music_box.h"

TEST(ReviewedGrips, ItemShapesDoNotLeakAcrossTheSharedGripKind) {
    namespace G = VR::Body::Grip;
    const auto book = G::ShapeForGoods(120, G::Kind::Book);
    const auto register_book = G::ShapeForGoods(130, G::Kind::Book);
    EXPECT_EQ(book.bend, register_book.bend);
    EXPECT_NE(book.bend, G::ShapeOf(G::Kind::Book).bend);
    const auto vial = G::ShapeForGoods(1000, G::Kind::Vial);
    EXPECT_EQ(vial.bend, G::ShapeOf(G::Kind::Vial).bend);
    EXPECT_EQ(vial.thumb_oppose, G::ShapeOf(G::Kind::Vial).thumb_oppose);
    const auto sedative = G::ShapeForGoods(1101, G::Kind::Wide);
    EXPECT_NE(sedative.bend, G::ShapeOf(G::Kind::Wide).bend);
    EXPECT_EQ(G::ShapeForGoods(-1, G::Kind::Handle).bend, G::ShapeOf(G::Kind::Handle).bend);
}

TEST(ReviewedGrips, ApprovedPivotStaysInPalmThroughWristTurnsAndScaling) {
    namespace Q = VR::Quick;
    // Rotated, translated and scaled game dummy; axes must not scale the item twice.
    const std::array<float,12> palm{0,-2,0,3, 0,0,2,4, -2,0,0,5};
    for (const int id : Q::QuickUsableGoods) {
        const auto look = Q::HeldLookOf(id);
        const auto f = Q::ItemFrameOf(palm, look, 1.25f);
        const auto pivot = f.origin + (f.x*look.hold_x+f.y*look.hold_y+f.z*look.hold_z)*
                                      (look.scale*1.25f);
        EXPECT_NEAR(pivot.x, 3-(Q::GripCentre[1]+look.lift_m), 1e-5) << id;
        EXPECT_NEAR(pivot.y, 4+(Q::GripCentre[2]+look.out_m), 1e-5) << id;
        EXPECT_NEAR(pivot.z, 5-(Q::GripCentre[0]+look.side_m), 1e-5) << id;
        EXPECT_NEAR(VR::Body::Length(f.x), 1, 1e-5) << id;
    }
}

TEST(ReviewedGrips, MusicBoxContactFollowsTheRenderedAxesAndRejectsStaleFrames) {
    namespace M = VR::MusicBox;
    M::Geometry g{};
    const auto f = M::FrameFromAxes(g, {1,2,3}, {0,0,1}, {1,0,0});
    M::Crank crank; M::Config cfg;
    EXPECT_TRUE(crank.TryGrab(cfg,g,f,M::KnobAt(g,f,0)));
    EXPECT_FALSE(M::Crank{}.TryGrab(cfg,g,M::Frame{},M::KnobAt(g,f,0)));
    M::AttachmentChannel().Store({1'000'000'000,f,g});
    M::Attachment attachment;
    EXPECT_TRUE(M::LatestAttachment(attachment,1'010'000'000));
    EXPECT_FALSE(M::LatestAttachment(attachment,1'300'000'000));
    // A concurrent render may publish just after the presenter's clock sample.
    EXPECT_TRUE(M::LatestAttachment(attachment,999'000'000));
    EXPECT_FALSE(M::LatestAttachment(attachment,700'000'000));
}

TEST(ReviewedGrips, MusicBoxCrankIsWearersRightWhenLeftPalmFacesUp) {
    namespace Q = VR::Quick;
    namespace B = VR::Body;
    namespace M = VR::MusicBox;
    // Game-space palm normal +Y, fingers +Z (tracking -Z). Across-palm -X
    // completes the native basis. Looking down, the wearer's right is tracking +X.
    const std::array<float,12> palm{0,-1,0,-.18f, 1,0,0,1.f, 0,0,1,.25f};
    const auto look=Q::HeldLookOf(M::Goods);
    const auto item=Q::ItemFrameOf(palm,look);
    const auto track=[](B::Vec3 p) {const auto t=B::WorldToTracking(p,0,1);return Q::V3{t.x,t.y,t.z};};
    const auto origin=track(item.origin);
    const auto f=M::FrameForModel(origin,track(item.x),track(item.y),track(item.z),look.scale,
                                  {.0175745f,.01475f,.033567f});
    EXPECT_GT(f.axis.x,.99f);
    EXPECT_GT(f.hub.x,f.box.x);
    M::Geometry g{};g.arm_m*=look.scale;g.knob_out_m*=look.scale;
    for(int step=0;step<24;step++) EXPECT_GT(M::KnobAt(g,f,M::TwoPi*step/24).x,f.box.x);
}
