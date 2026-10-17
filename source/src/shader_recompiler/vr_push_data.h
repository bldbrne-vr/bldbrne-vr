// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include "common/types.h"

namespace Shader {

// The VR body cutaway of a draw (FragmentRuntimeInfo::vr_cut's code: near radius in millimetres
// of game units in bits 0..15, far radius in bits 16..30, bit 31 = back faces cut too) as the
// cutaway shader uses it: fragments nearer than `near_units` are cut, from there they fade in over
// 1 / `scale` game units; the far radius is at least 1 mm beyond the near one. The baked
// permutations (EmitVrCutaway) and the pushed values (VrPushData::SetCut) both decode it here, so
// they are the same floats bit for bit.
struct VrCut {
    float near_units{};
    float scale{};
    bool back{};

    static VrCut Decode(u32 code) {
        const float near_units = static_cast<float>(code & 0xFFFFu) * 0.001f;
        const float far_units =
            std::max(static_cast<float>((code >> 16) & 0x7FFFu) * 0.001f, near_units + 0.001f);
        return {near_units, 1.0f / (far_units - near_units), (code >> 31) != 0};
    }
};

// Native stereo graphics draws on a device with VrPushData::End bytes of push constants
// (Profile::SupportsVrPushData): the per-draw VR values that follow PushData in the push block
// (members 13..22), read at run time with the eye chosen by the view index. One shader
// permutation then serves every camera, light tile, cut radius and motion-buffer allocation;
// before, each of them was baked into its own permutation (RuntimeInfo::native_projection /
// native_light_bounds / vr_motion's addresses, FragmentRuntimeInfo::vr_cut), which are now only
// flags. Without that limit they are baked as before and only scene_inverse is pushed (136 bytes,
// fragment stages).
// Every graphics pipeline layout has the one range 0..End (push constants survive the binds
// between pipelines, VrDrawCapeSplit's cut pipeline in the middle of a draw included).
struct VrPushData {
    static constexpr u32 Begin = 128; // sizeof(PushData)
    static constexpr u32 End = 224;
    // The push block's SPIR-V members after PushData's 13 (a pair: left, right = left + 1).
    static constexpr u32 SceneInverseIndex = 13; // x, then y at 14
    static constexpr u32 CutNearIndex = 15;
    static constexpr u32 CutScaleIndex = 16;
    static constexpr u32 ProjectionIndex = 17; // left 17, right 18
    static constexpr u32 LightIndex = 19;      // left 19, right 20
    static constexpr u32 MotionIndex = 21;     // the parameters' address 21, the positions' 22
    static constexpr u32 CutNearOffset = 136, CutScaleOffset = 140, ProjectionOffset = 144,
                         LightOffset = 176, MotionOffset = 208;
    // A light test that keeps every pixel (a draw without a tile of its own).
    static constexpr float Unbounded = std::numeric_limits<float>::max();
    static constexpr std::array<float, 4> AllPixels{-Unbounded, -Unbounded, Unbounded, Unbounded};

    // FragCoord in render pixels times this = guest pixels (the temporal input scale).
    std::array<float, 2> scene_inverse{1.0f, 1.0f};
    // The body cutaway (a cut permutation reads them): VrCut's near radius and its scale, negative
    // when back faces are cut too. The default keeps every fragment: a fragment's depth (> 0) is
    // more than 1 unit past a near radius of -1, so the fade is always complete.
    float cut_near{-1.0f};
    float cut_scale{1.0f};
    // Each eye's asymmetric remap {x_scale, y_scale, x_offset, y_offset}
    // (NativeStereo::DrawProjection, RuntimeInfo::native_projection's order); x_scale 0: none.
    std::array<std::array<float, 4>, 2> projection{};
    // Each eye's light tile in guest pixels, half-open [x0, y0, x1, y1)
    // (NativeStereo::LightScissors).
    std::array<std::array<float, 4>, 2> light{AllPixels, AllPixels};
    // A motion variant's object-motion buffers (RuntimeInfo::vr_motion, VrObjectMotion): the
    // device addresses of its per-draw parameters and of its position history; 0 for every other
    // draw. The shader reads each as a uvec2 {low word, high word} and bitcasts it to a u64.
    u64 motion_params{};
    u64 motion_positions{};

    // The cut of the draw's code (0: none, the default).
    void SetCut(u32 code) {
        if (code == 0) {
            cut_near = -1.0f;
            cut_scale = 1.0f;
            return;
        }
        const VrCut cut = VrCut::Decode(code);
        cut_near = cut.near_units;
        cut_scale = cut.back ? -cut.scale : cut.scale;
    }
};
static_assert(sizeof(VrPushData) == VrPushData::End - VrPushData::Begin);
static_assert(offsetof(VrPushData, cut_near) == VrPushData::CutNearOffset - VrPushData::Begin);
static_assert(offsetof(VrPushData, cut_scale) == VrPushData::CutScaleOffset - VrPushData::Begin);
static_assert(offsetof(VrPushData, projection) == VrPushData::ProjectionOffset - VrPushData::Begin);
static_assert(offsetof(VrPushData, light) == VrPushData::LightOffset - VrPushData::Begin);
static_assert(offsetof(VrPushData, motion_params) == VrPushData::MotionOffset - VrPushData::Begin);
static_assert(offsetof(VrPushData, motion_positions) ==
              VrPushData::MotionOffset + 8 - VrPushData::Begin);
static_assert(VrPushData::ProjectionOffset % 16 == 0 && VrPushData::LightOffset % 16 == 0 &&
              VrPushData::MotionOffset % 8 == 0);

} // namespace Shader
