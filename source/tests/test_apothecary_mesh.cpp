// SPDX-License-Identifier: GPL-2.0-or-later
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/vr_apothecary_mesh.h"
#include "video_core/renderer_vulkan/vr_syringe_mesh.h"
#include "video_core/renderer_vulkan/vr_molotov_mesh.h"
namespace O=Vulkan::VrOverlay;
TEST(ApothecaryMesh, FilledCaseFitsBudgetWithoutChangingHeldSyringe) {
    const auto c=O::ApothecaryCaseMesh(), stored=O::AntiqueSyringeMesh(18), held=O::AntiqueSyringeMesh();
    const auto triangles=(c.indices.size()+4*stored.indices.size())/3;
    EXPECT_EQ(triangles,8572u);EXPECT_GE(triangles,8000u);EXPECT_LE(triangles,8600u);
    EXPECT_EQ(held.indices.size()/3,4800u);
    for(const auto* mesh:{&c,&stored}) {
        for(auto i:mesh->indices) ASSERT_LT(i,mesh->vertices.size());
        for(const auto& v:mesh->vertices) {
            EXPECT_TRUE(std::isfinite(v.position.x));EXPECT_TRUE(std::isfinite(v.position.y));EXPECT_TRUE(std::isfinite(v.position.z));
        }
    }
    // Same longitudinal profile, chamber and plunger positions; only angular density changes.
    float low=1,high=-1;
    for(const auto& v:stored.vertices){low=std::min(low,v.position.y);high=std::max(high,v.position.y);}
    EXPECT_NEAR(low,0,.00001);EXPECT_NEAR(high,.176,.00001);
}

TEST(ApothecaryMesh, VisibilitySphereContainsCaseAndEveryStoredVial) {
    const auto c=O::ApothecaryCaseMesh(), stored=O::AntiqueSyringeMesh(18);
    const auto check=[](float x,float y,float z){EXPECT_LE(x*x+y*y+z*z,.18f*.18f);};
    for(const auto& v:c.vertices) check(v.position.x,v.position.y,v.position.z);
    for(int slot=0;slot<4;++slot)
        for(const auto& v:stored.vertices)
            check(v.position.x+(slot%2-.5f)*.044f,v.position.y-.105f,
                  v.position.z+(slot/2-.5f)*.046f);
}

TEST(ApothecaryMesh, BottleVisibilitySphereIncludesTheExtendedCloth) {
    const auto bottle=O::VictorianMolotovMesh();
    const float radius=std::sqrt(.106f*.106f+.296f*.296f+.106f*.106f)*.5f+.06f;
    for(const auto& v:bottle.vertices) {
        const float x=v.position.x,y=v.position.y-.052f,z=v.position.z;
        EXPECT_LE(x*x+y*y+z*z,radius*radius);
    }
}

TEST(ApothecaryMesh, FoldedFlapDoesNotForceCaseAwayFromBelt) {
    const auto mesh=O::ApothecaryCaseMesh();
    for(const auto& v:mesh.vertices) EXPECT_LE(v.position.z,.068f);
}
