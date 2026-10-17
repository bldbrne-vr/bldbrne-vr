// SPDX-License-Identifier: GPL-2.0-or-later

// The per-draw VR values in the push constants (shader_recompiler/vr_push_data.h): the layout the
// SPIR-V push block declares, the body cutaway's radii decoded exactly as the baked permutations
// had them, the motion buffers' addresses as the shader reads them (and the motion variant kept
// by its flag alone), and the defaults a draw without a projection, a light tile, a cut or object
// motion pushes.

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "shader_recompiler/vr_motion.h"
#include "shader_recompiler/vr_push_data.h"

namespace {

using Shader::VrCut;
using Shader::VrMotion;
using Shader::VrPushData;

std::uint32_t Bits(float v) {
    return std::bit_cast<std::uint32_t>(v);
}

std::uint32_t Code(std::uint32_t near_mm, std::uint32_t far_mm, bool back) {
    return near_mm | (far_mm << 16) | (back ? 0x80000000u : 0u);
}

// Codes as EncodeCut makes them (vk_rasterizer_vr_draws.cpp) plus the field limits. 0 is no cut
// (no cut permutation is ever used with it; the push then keeps every fragment).
std::vector<std::uint32_t> Codes() {
    std::vector<std::uint32_t> codes;
    for (const std::uint32_t near_mm : {0u, 1u, 30u, 70u, 110u, 999u, 1000u, 32766u, 65535u}) {
        for (const std::uint32_t far_mm : {0u, 1u, 2u, 220u, 285u, 350u, 1001u, 32767u}) {
            if (near_mm != 0 || far_mm != 0) {
                codes.push_back(Code(near_mm, far_mm, false));
            }
            codes.push_back(Code(near_mm, far_mm, true));
        }
    }
    return codes;
}

// The cutaway shader's arithmetic (EmitVrCutaway) in floats: is the fragment cut?
bool Cut(float near_units, float scale, bool back, float w, float noise, bool front) {
    const float depth = 1.0f / w;
    const float t = std::clamp((depth - near_units) * scale, 0.0f, 1.0f);
    const float keep = t * t * (3.0f - 2.0f * t);
    return keep <= noise || (back && !front);
}

TEST(VrPushData, TheLayoutTheShadersDeclare) {
    // DefinePushDataBlock: members 13..22 at these offsets, after the 128 bytes of PushData; the
    // graphics pipeline layouts' one range ends at End (NVIDIA, AMD and Intel give 256).
    EXPECT_EQ(sizeof(VrPushData), 96u);
    EXPECT_EQ(VrPushData::Begin, 128u);
    EXPECT_EQ(VrPushData::End, 224u);
    EXPECT_EQ(offsetof(VrPushData, scene_inverse), 0u);
    EXPECT_EQ(VrPushData::Begin + offsetof(VrPushData, cut_near), VrPushData::CutNearOffset);
    EXPECT_EQ(VrPushData::Begin + offsetof(VrPushData, cut_scale), VrPushData::CutScaleOffset);
    EXPECT_EQ(VrPushData::Begin + offsetof(VrPushData, projection), VrPushData::ProjectionOffset);
    EXPECT_EQ(VrPushData::Begin + offsetof(VrPushData, light), VrPushData::LightOffset);
    EXPECT_EQ(VrPushData::Begin + offsetof(VrPushData, motion_params), VrPushData::MotionOffset);
    EXPECT_EQ(VrPushData::Begin + offsetof(VrPushData, motion_positions),
              VrPushData::MotionOffset + 8u);
    EXPECT_EQ(VrPushData::CutNearOffset, 136u);
    EXPECT_EQ(VrPushData::CutScaleOffset, 140u);
    EXPECT_EQ(VrPushData::ProjectionOffset, 144u); // left 144, right 160
    EXPECT_EQ(VrPushData::LightOffset, 176u);      // left 176, right 192
    EXPECT_EQ(VrPushData::MotionOffset, 208u);     // parameters 208, positions 216 (uvec2 each)
    EXPECT_EQ(VrPushData::LightOffset + 32u, VrPushData::MotionOffset);
    EXPECT_EQ(VrPushData::MotionOffset + 16u, VrPushData::End);
    // Members: scene_inverse x 13, y 14, then one member per float or per eye's vec4.
    EXPECT_EQ(VrPushData::SceneInverseIndex, 13u);
    EXPECT_EQ(VrPushData::CutNearIndex, VrPushData::SceneInverseIndex + 2);
    EXPECT_EQ(VrPushData::CutScaleIndex, VrPushData::CutNearIndex + 1);
    EXPECT_EQ(VrPushData::ProjectionIndex, VrPushData::CutScaleIndex + 1);
    EXPECT_EQ(VrPushData::LightIndex, VrPushData::ProjectionIndex + 2);
    EXPECT_EQ(VrPushData::MotionIndex, VrPushData::LightIndex + 2);
}

TEST(VrPushData, TheMotionAddressesReadAsTheShaderReadsThem) {
    // EmitVertexMotion loads each address as a uvec2 and bitcasts it to a u64: component 0 is the
    // low word. The bytes BindResources pushes are this struct's (little-endian, like SPIR-V's
    // bitcast of a vector to a wider scalar).
    VrPushData push{};
    push.motion_params = 0x0000'1234'5678'9ABCull;
    push.motion_positions = 0xFEDC'BA98'7654'3210ull;
    std::array<std::uint8_t, sizeof(VrPushData)> bytes{};
    std::memcpy(bytes.data(), &push, sizeof(push));
    for (const auto& [offset, value] :
         {std::pair{VrPushData::MotionOffset, push.motion_params},
          std::pair{VrPushData::MotionOffset + 8u, push.motion_positions}}) {
        std::array<std::uint32_t, 2> uvec2{};
        std::memcpy(uvec2.data(), bytes.data() + (offset - VrPushData::Begin), sizeof(uvec2));
        EXPECT_EQ(uvec2[0], static_cast<std::uint32_t>(value));
        EXPECT_EQ(uvec2[1], static_cast<std::uint32_t>(value >> 32));
        EXPECT_EQ(std::bit_cast<std::uint64_t>(uvec2), value);
    }
}

TEST(VrMotion, OnlyBakedAddressesStayOffTheDiskCache) {
    // VrPushData: a motion permutation keeps only the flag (PipelineCache::VrMotionVariant) and is
    // cached; one with an address baked in (without VrPushData) never is, nor loaded.
    VrMotion pushed{};
    pushed.enabled = true;
    EXPECT_FALSE(pushed.Baked());
    EXPECT_FALSE(VrMotion{}.Baked());
    VrMotion baked = pushed;
    baked.params_address = 0x1000;
    EXPECT_TRUE(baked.Baked());
    baked = pushed;
    baked.positions_address = 0x2000;
    EXPECT_TRUE(baked.Baked());
}

TEST(VrMotion, APushedVariantKeepsOnlyTheFlag) {
    // VrObjectMotion's buffers before and after a Release/Init (new allocations).
    VrMotion first{};
    first.params_address = 0x7F00'0001'0000ull;
    first.positions_address = 0x7F00'0002'0000ull;
    first.enabled = true;
    VrMotion second = first;
    second.params_address = 0x7F00'0040'0000ull;
    second.positions_address = 0x7F00'0080'0000ull;
    // With VrPushData: the same variant for every allocation, one the disk cache keeps.
    const VrMotion variant = first.Variant(true);
    EXPECT_TRUE(variant.enabled);
    EXPECT_EQ(variant.params_address, 0u);
    EXPECT_EQ(variant.positions_address, 0u);
    EXPECT_FALSE(variant.Baked());
    EXPECT_TRUE(second.Variant(true) == variant);
    EXPECT_TRUE(variant.Variant(true) == variant);
    // Without it the addresses stay in the variant (baked: never cached), one per allocation.
    EXPECT_TRUE(first.Variant(false) == first);
    EXPECT_FALSE(second.Variant(false) == first.Variant(false));
    EXPECT_TRUE(first.Variant(false).Baked());
    // No motion: nothing to strip either way.
    EXPECT_TRUE(VrMotion{}.Variant(true) == VrMotion{});
    EXPECT_TRUE(VrMotion{}.Variant(false) == VrMotion{});
}

TEST(VrCut, DecodesExactlyAsTheBakedPermutations) {
    // The constants EmitVrCutaway baked into a cut permutation before the radii were pushed.
    for (const std::uint32_t code : Codes()) {
        const float near_units = static_cast<float>(code & 0xFFFFu) * 0.001f;
        const float far_units =
            std::max(static_cast<float>((code >> 16) & 0x7FFFu) * 0.001f, near_units + 0.001f);
        const VrCut cut = VrCut::Decode(code);
        EXPECT_EQ(Bits(cut.near_units), Bits(near_units)) << code;
        EXPECT_EQ(Bits(cut.scale), Bits(1.0f / (far_units - near_units))) << code;
        EXPECT_EQ(cut.back, (code >> 31) != 0) << code;
        EXPECT_GT(cut.scale, 0.0f) << code; // the pushed sign carries `back` unambiguously
    }
}

TEST(VrPushData, ACutPushesTheDecodedRadii) {
    for (const std::uint32_t code : Codes()) {
        VrPushData push{};
        push.SetCut(code);
        const VrCut cut = VrCut::Decode(code);
        EXPECT_EQ(Bits(push.cut_near), Bits(cut.near_units)) << code;
        EXPECT_EQ(Bits(std::abs(push.cut_scale)), Bits(cut.scale)) << code;
        EXPECT_EQ(push.cut_scale < 0.0f, cut.back) << code;
    }
    VrPushData push{};
    push.SetCut(Code(30, 220, true));
    push.SetCut(0); // no cut: the default again
    EXPECT_EQ(Bits(push.cut_near), Bits(VrPushData{}.cut_near));
    EXPECT_EQ(Bits(push.cut_scale), Bits(VrPushData{}.cut_scale));
}

TEST(VrPushData, ThePushedCutCutsTheSameFragments) {
    // The shader takes |scale| and scale < 0: the same decision as the baked constants.
    const std::array<float, 9> ws{0.01f, 0.02f, 0.05f, 0.1f, 0.25f, 1.0f, 3.0f, 20.0f, 1000.0f};
    for (const std::uint32_t code : Codes()) {
        const VrCut baked = VrCut::Decode(code);
        VrPushData push{};
        push.SetCut(code);
        for (const float w : ws) {
            for (int n = 0; n < 16; ++n) {
                const float noise = static_cast<float>(n) / 16.0f;
                for (const bool front : {true, false}) {
                    EXPECT_EQ(Cut(push.cut_near, std::abs(push.cut_scale), push.cut_scale < 0.0f, w,
                                  noise, front),
                              Cut(baked.near_units, baked.scale, baked.back, w, noise, front))
                        << code << " w " << w;
                }
            }
        }
    }
}

TEST(VrPushData, TheDefaultsChangeNothing) {
    const VrPushData push{};
    // No cut: every fragment in front of the camera (w > 0), front or back face, is kept.
    for (const float w : {1e-6f, 0.001f, 0.5f, 1.0f, 2.0f, 1e6f}) {
        for (int n = 0; n < 64; ++n) {
            const float noise = static_cast<float>(n) / 64.0f;
            EXPECT_FALSE(Cut(push.cut_near, std::abs(push.cut_scale), push.cut_scale < 0.0f, w,
                             noise, false))
                << w;
        }
    }
    // No projection: x_scale 0, the remap's validity test fails and the shader keeps its values.
    EXPECT_FALSE(push.projection[0][0] > 0.0f && push.projection[1][0] > 0.0f);
    // No light tile: every pixel passes the half-open per-eye test.
    for (const auto& eye : push.light) {
        for (const float pixel : {-1e30f, -1.0f, 0.0f, 0.5f, 1456.0f, 16384.0f, 1e30f}) {
            EXPECT_FALSE(pixel < eye[0] || pixel >= eye[2]) << pixel;
            EXPECT_FALSE(pixel < eye[1] || pixel >= eye[3]) << pixel;
        }
    }
    EXPECT_EQ(Bits(push.scene_inverse[0]), Bits(1.0f));
    EXPECT_EQ(Bits(push.scene_inverse[1]), Bits(1.0f));
    // No motion variant: no buffers (no shader without one reads them).
    EXPECT_EQ(push.motion_params, 0u);
    EXPECT_EQ(push.motion_positions, 0u);
}

} // namespace
