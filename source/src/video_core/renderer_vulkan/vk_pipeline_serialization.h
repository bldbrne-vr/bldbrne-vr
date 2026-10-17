// SPDX-FileCopyrightText: Copyright 2025 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "shader_recompiler/frontend/fetch_shader.h"
#include "video_core/renderer_vulkan/vk_pipeline_cache.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace Vulkan {

void RegisterPipelineData(const ComputePipelineKey& key,
                          ComputePipeline::SerializationSupport& sdata);
void RegisterPipelineData(const GraphicsPipelineKey& key, u64 hash,
                          GraphicsPipeline::SerializationSupport& sdata);
// Bloodborne VR fork: FAST_CPU_PERM_IDS (off unless '1', read once at start-up;
// vk_pipeline_serialization.cpp): true when a permutation is named by its id (PermutationId),
// false for HashCombine(program, its index), as before (pipeline_cache_policy.h Naming).
bool PermutationIds();
// A permutation's id (pipeline_cache_policy.h), the hash of its serialized specialization seeded
// with the program's hash: never 0.
u64 PermutationId(u64 pgm_hash, const Shader::StageSpecialization& spec);
// `perm_id` is the permutation's name (the pipeline keys' stage hash: its id, or
// HashCombine(program, perm_idx)), the meta's file name; the binary's with the ids, else
// "<pgm_hash>_<perm_idx>". `perm_idx` is where the warm-up puts the permutation in its program's
// list (where this session has it).
void RegisterShaderMeta(const Shader::Info& info,
                        const std::optional<Shader::Gcn::FetchShaderData>& fetch_shader_data,
                        const Shader::StageSpecialization& spec, u64 perm_id, size_t perm_idx);
void RegisterShaderBinary(std::vector<u32>&& spv, u64 perm_id, u64 pgm_hash, size_t perm_idx);

} // namespace Vulkan
