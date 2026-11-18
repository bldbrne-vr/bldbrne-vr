// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Layer draws (NativeStereo::LayerDraws): native stereo's both-eye draws without the driver's
// multiview. The pass renders both layers of its attachments (Scheduler::BeginRendering), the
// pipeline has no view mask, and each draw is recorded once per eye: the right eye's copy pushes
// vr_hud with Shader::PushData::NativeRightEyeBit, which its shaders read as their eye
// (EmitContext::NativeEyeIndex) and their last vertex stage writes to gl_Layer. Then the left
// eye's vr_hud is pushed back, so the command buffer holds the draw's own push again (the push
// memos and FAST_CPU_PUSH_SPLIT leave out bytes it already holds).
#pragma once

#include <bit>
#include <cstddef>
#include "shader_recompiler/resource.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan {

struct EyeDraws {
    vk::PipelineLayout layout{}; // the draw's (a cut pipeline bound mid-draw shares its range)
    float hud{};                 // the vr_hud the draw pushed
    bool both{};                 // a both-eye draw with layer draws on
};

// Records `draw` (the draw's commands, its pipeline bound) for the left eye and, for a both-eye
// draw, for the right eye too. `cmdbuf`: the RecorderProxy the draw records through.
template <class Commands, class Draw>
inline void DrawEyes(const Commands& cmdbuf, const EyeDraws& eyes, Draw&& draw) {
    draw();
    if (!eyes.both) {
        return;
    }
    // Every graphics layout's one range covers these stages (Pipeline::push_range).
    constexpr auto stages =
        vk::ShaderStageFlagBits::eVertex | vk::ShaderStageFlagBits::eTessellationControl |
        vk::ShaderStageFlagBits::eTessellationEvaluation | vk::ShaderStageFlagBits::eGeometry |
        vk::ShaderStageFlagBits::eFragment;
    constexpr u32 offset = offsetof(Shader::PushData, vr_hud);
    const float right =
        std::bit_cast<float>(std::bit_cast<u32>(eyes.hud) | Shader::PushData::NativeRightEyeBit);
    cmdbuf.pushConstants(eyes.layout, stages, offset, u32(sizeof(float)), &right);
    draw();
    cmdbuf.pushConstants(eyes.layout, stages, offset, u32(sizeof(float)), &eyes.hud);
}

} // namespace Vulkan
