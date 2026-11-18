// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/program.h"

namespace Shader {
struct Profile;
struct SrtSnapshot;
void InjectClipDistanceAttributes(IR::Program& program, RuntimeInfo& runtime_info);
} // namespace Shader

namespace Shader::Optimization {

void SsaRewritePass(IR::Program& program);
void SsaDestroyPass(IR::Program& program);
void IdentityRemovalPass(IR::BlockList& program);
void DeadCodeEliminationPass(IR::Program& program);
void ConstantPropagationPass(IR::BlockList& program);
void FlattenExtendedUserdataPass(IR::Program& program, SrtSnapshot* snapshot = nullptr);
void ReadLaneEliminationPass(IR::Program& program);
void ResourceTrackingPass(IR::Program& program, const Profile& profile);
void CollectShaderInfoPass(IR::Program& program, const Profile& profile);
void LowerBufferFormatToRaw(IR::Program& program);
void LowerUserClipPlanes(IR::Program& program, const RuntimeInfo& runtime_info);
void BillboardFaceEyePass(IR::Program& program, const RuntimeInfo& runtime_info);
void ScreenRectPass(IR::Program& program);
void WetBloodPass(IR::Program& program, const RuntimeInfo& runtime_info);
void ParticleAirPass(IR::Program& program, const RuntimeInfo& runtime_info);
void ParallaxPass(IR::Program& program, const RuntimeInfo& runtime_info);
void BumpHeightPass(IR::Program& program, const RuntimeInfo& runtime_info);
void SpecularAaPass(IR::Program& program, const RuntimeInfo& runtime_info);
void MoonShadowPass(IR::Program& program, const RuntimeInfo& runtime_info);
void ReflectionsPass(IR::Program& program, const RuntimeInfo& runtime_info);
void LightShadowPass(IR::Program& program, const RuntimeInfo& runtime_info);
void AmbientOcclusionPass(IR::Program& program, const RuntimeInfo& runtime_info);
void BouncePass(IR::Program& program, const RuntimeInfo& runtime_info);
void AirLightPass(IR::Program& program, const RuntimeInfo& runtime_info);
void LowerFp64ToFp32(IR::Program& program);
void RingAccessElimination(const IR::Program& program, const RuntimeInfo& runtime_info);
void TessellationPreprocess(IR::Program& program, RuntimeInfo& runtime_info);
void HullShaderTransform(IR::Program& program, const RuntimeInfo& runtime_info);
void DomainShaderTransform(const IR::Program& program, const RuntimeInfo& runtime_info);
void SharedMemoryBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info,
                             const Profile& profile);
void SharedMemorySimplifyPass(IR::Program& program, const Profile& profile);
void SharedMemoryToStoragePass(IR::Program& program, const RuntimeInfo& runtime_info,
                               const Profile& profile);

} // namespace Shader::Optimization
