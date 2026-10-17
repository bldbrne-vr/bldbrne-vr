// SPDX-License-Identifier: GPL-2.0-or-later
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <gtest/gtest.h>
#include "video_core/renderer_vulkan/vr_goods_meshes.h"
namespace O = Vulkan::VrOverlay;

namespace {
std::vector<u8> SkullData() {
    const auto path = std::filesystem::path(__FILE__).parent_path() / "../src/resources/vr/sangiran17.bin";
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}
void ExpectWellFormed(const O::MeshData& m) {
    ASSERT_FALSE(m.vertices.empty());
    ASSERT_EQ(m.indices.size() % 3, 0u);
    for (const u32 i : m.indices) ASSERT_LT(i, m.vertices.size());
    for (const auto& v : m.vertices) {
        ASSERT_TRUE(std::isfinite(v.position.x) && std::isfinite(v.position.y) && std::isfinite(v.position.z));
        ASSERT_NEAR(O::Length(v.normal), 1.0f, 1e-3f);
        ASSERT_TRUE(v.aux.y >= 0.0f && v.aux.y <= 1.0f);
    }
}
} // namespace

// The embedded Sangiran 17 decodes to the approved mesh: life size, closed outward.
TEST(GoodsMeshes, SkullDecodesLifeSize) {
    const auto data = SkullData();
    ASSERT_EQ(data.size(), 65852u);
    const auto m = O::SkullMeshFrom(data);
    ExpectWellFormed(m);
    EXPECT_EQ(m.vertices.size(), 2984u);
    EXPECT_EQ(m.indices.size(), 18000u);
    O::Vec3 lo{1, 1, 1}, hi{-1, -1, -1};
    for (const auto& v : m.vertices) {
        lo = {std::min(lo.x, v.position.x), std::min(lo.y, v.position.y), std::min(lo.z, v.position.z)};
        hi = {std::max(hi.x, v.position.x), std::max(hi.y, v.position.y), std::max(hi.z, v.position.z)};
    }
    EXPECT_NEAR(hi.x - lo.x, .144f, .002f); // breadth
    EXPECT_NEAR(hi.y - lo.y, .168f, .002f); // the vault to the tooth roots
    EXPECT_NEAR(hi.z - lo.z, .221f, .002f); // the face to the occiput
    EXPECT_GT(O::SignedVolume(m), 0.0015); // ~2 litres, wound outward
}

TEST(GoodsMeshes, SkullRejectsBadData) {
    auto data = SkullData();
    EXPECT_TRUE(O::SkullMeshFrom(std::span(data).first(100)).vertices.empty());
    data[0] = 'X';
    EXPECT_TRUE(O::SkullMeshFrom(data).vertices.empty());
}

// Shade::Syringe tells a potion (all glass, gold blood) from the vial's tubes by aux.x.
TEST(GoodsMeshes, LiquidPartsMarkTheirKind) {
    const auto bottle = O::IosefkaBottleMesh();
    ExpectWellFormed(bottle);
    for (const auto& v : bottle.vertices) EXPECT_EQ(v.aux.x, 2.0f);
    EXPECT_GT(O::SignedVolume(bottle), 0.0);
    for (const bool ring : {false, true}) {
        const auto tube = O::BloodTubeMesh(ring);
        ExpectWellFormed(tube);
        for (const auto& v : tube.vertices) EXPECT_LT(v.aux.x, 1.5f);
    }
}

TEST(GoodsMeshes, GoodsPartsAreWellFormed) {
    for (const auto& m : {O::IosefkaLabelMesh(), O::BloodTubeClothMesh(true), O::BloodTubeClothMesh(false),
                          O::ResonantBellMesh(), O::AntidoteMesh(), O::NotebookMesh(), O::ColdbloodMesh(),
                          O::RubPaperMesh(O::GoodsMat::FirePaper), O::RubPaperMesh(O::GoodsMat::BoltPaper),
                          O::PhantasmShellMesh()}) {
        ExpectWellFormed(m);
        for (const auto& v : m.vertices) EXPECT_TRUE(v.aux.x >= 0.0f && v.aux.x <= 18.0f);
    }
    // The rubs are held by their left end: most of each reaches right, toward the blade.
    for (const auto& m : {O::RubPaperMesh(O::GoodsMat::FirePaper), O::PhantasmShellMesh()}) {
        float lo = 1, hi = -1;
        for (const auto& v : m.vertices) lo = std::min(lo, v.position.x), hi = std::max(hi, v.position.x);
        EXPECT_GT(hi, 3.0f * -lo);
    }
}

// The occlusion bake darkens hollows, not open surfaces, and is cheap enough for the build.
TEST(GoodsMeshes, OcclusionFindsHollowsQuickly) {
    const auto t0 = std::chrono::steady_clock::now();
    std::vector<O::MeshData> all{O::BloodTubeClothMesh(true), O::BloodTubeClothMesh(false),
                                 O::ResonantBellMesh(), O::AntidoteMesh(), O::NotebookMesh(),
                                 O::ColdbloodMesh()};
    for (auto& m : all) O::BakeOcclusion(m, &m == &all[2] ? .04f : .012f); // as vr_inframe.cpp
    const auto ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(ms, 1500.0) << "bake took " << ms << " ms";
    std::printf("[ goods ] occlusion bake of all goods meshes: %.0f ms\n", ms);
    // The bell: inside its mouth (normals toward the axis) darker than the outside wall.
    const auto& bell = all[2];
    double inside = 0, outside = 0;
    int ni = 0, no = 0;
    for (const auto& v : bell.vertices) {
        if (v.aux.x != O::GoodsMat::Bronze || v.position.y < .008f || v.position.y > .030f) continue;
        const float radial = v.normal.x * v.position.x + v.normal.z * v.position.z;
        (radial < 0 ? inside : outside) += v.aux.y;
        ++(radial < 0 ? ni : no);
    }
    ASSERT_GT(ni, 0);
    ASSERT_GT(no, 0);
    EXPECT_GT(inside / ni, outside / no + .2);
}
