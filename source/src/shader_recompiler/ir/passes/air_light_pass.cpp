// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// The air pass (air_light.h): the shadow mask's blur computes the moonlight in the air's
// half-size cells (VrAirCompute, a side effect: its value, 0, is added to the blur's own output so
// the op stays), the copy of the finished scene applies them and the lamps' glow (VrAirApply of
// the scene's colour: colour * (1 + w) + rgb).

#include <algorithm>
#include <array>
#include <cstdlib>
#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/air_light.h"
#include "shader_recompiler/ir/passes/fog_grid.h"
#include "shader_recompiler/ir/passes/fog_light.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/passes/moon_shafts.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

void AirLightPass(IR::Program& program, const RuntimeInfo& runtime_info) {
    auto& info = program.info;
    if (info.stage != Stage::Fragment || program.blocks.empty()) {
        return;
    }
    const bool compute_program =
        AirLight::IsComputeProgram(info.pgm_hash) || AirLight::IsHalfProgram(info.pgm_hash);
    const bool apply_program = AirLight::IsApplyProgram(info.pgm_hash);
    if (!compute_program && !apply_program) {
        return;
    }
    // Tests: VR_AIR_HOSTS, a mask of the programs taken (1 the blur, 2 the reflections pass, 4 the
    // scene's copy; 7 by default).
    static const u32 hosts = [] {
        const char* const env = std::getenv("VR_AIR_HOSTS");
        return env != nullptr ? static_cast<u32>(std::atoi(env)) : 7u;
    }();
    if ((AirLight::IsComputeProgram(info.pgm_hash) && !(hosts & 1u)) ||
        (AirLight::IsHalfProgram(info.pgm_hash) && !(hosts & 2u)) || (apply_program && !(hosts & 4u))) {
        return;
    }
    // The resources: declared for every permutation of the two programs, ours or not (as the
    // composite's, BouncePass); only our ops read them.
    const auto buffer = [&](BufferType type, u32 dwords, bool written) {
        if (std::ranges::find(info.buffers, type, &BufferResource::buffer_type) == info.buffers.end()) {
            info.buffers.push_back({
                .used_types = IR::Type::U32,
                .inline_cbuf = AmdGpu::Buffer::Placeholder(dwords * sizeof(u32)),
                .buffer_type = type,
                .is_written = written,
            });
        }
    };
    const auto image = [&](VrHostImage slot, bool depth) {
        const u32 sharp = VrHostImageBase + static_cast<u32>(slot);
        const auto found = std::ranges::find(info.images, sharp, &ImageResource::sharp_idx);
        if (found != info.images.end()) {
            return static_cast<u32>(std::distance(info.images.begin(), found));
        }
        info.images.push_back({
            .sharp_idx = sharp,
            .is_depth = depth,
        });
        return static_cast<u32>(info.images.size() - 1);
    };
    buffer(BufferType::VrAir, AirLight::ParamDwords, false);
    buffer(BufferType::VrAirCells, 1024, compute_program);
    const u32 depth_image = image(VrHostImage::AirDepth, false);
    if (compute_program) {
        buffer(BufferType::VrMoonShafts, MoonShafts::BufferDwords, false);
        image(VrHostImage::MoonAtlas, true);
    } else {
        buffer(BufferType::VrFogLights, FogLight::BufferDwords, false);
        // The source votes (air_light.h Sources) go to the colour probe's buffer.
        buffer(BufferType::VrAirProbe, AirLight::ProbeSlots * AirLight::ProbeSlotDwords, true);
        // The fog grid (fog_grid.h), in the permutations keyed for it (every one of a session that
        // has it on). Never written here: both eyes read the one buffer, no right-eye twin.
        if (runtime_info.vr_composite_light & RuntimeInfo::VrFogGrid) {
            buffer(BufferType::VrFogGrid, FogGrid::NoneDwords, false);
        }
    }
    const bool on = (runtime_info.vr_composite_light & RuntimeInfo::VrAirPass) != 0;
    if (!on) {
        return;
    }
    std::array<IR::Inst*, 3> colour{};
    u32 own_sample = ~0u; // the program's own image sample handle (its sampler: the handle's high half)
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::SetAttribute &&
                inst.Arg(0).Attribute() == IR::Attribute::RenderTarget0 && inst.Arg(2).IsImmediate() &&
                inst.Arg(2).U32() < 3) {
                colour[inst.Arg(2).U32()] = &inst;
            }
            if ((inst.GetOpcode() == IR::Opcode::ImageSampleImplicitLod ||
                 inst.GetOpcode() == IR::Opcode::ImageSampleExplicitLod) &&
                inst.Arg(0).IsImmediate() && own_sample == ~0u) {
                own_sample = inst.Arg(0).U32();
            }
        }
    }
    if (colour[0] == nullptr || (apply_program && (colour[1] == nullptr || colour[2] == nullptr))) {
        LOG_INFO(Render_Recompiler, "VR air pass: shader {:#x} left as it is (outputs)", info.pgm_hash);
        return;
    }
    IR::Block* const entry = program.blocks.front();
    if (compute_program && (runtime_info.vr_composite_light & RuntimeInfo::VrFogGrid)) {
        // The fog grid (fog_grid.h) holds the mist and the moonlit air: no cells, no march. The
        // program keeps its resources (the rasterizer works the mist's numbers out at its draw).
        LOG_INFO(Render_Recompiler, "VR air pass: shader {:#x} left as it is (the fog grid has the air)",
                 info.pgm_hash);
        return;
    }
    if (compute_program) {
        IR::IREmitter ir{*entry, entry->begin()};
        const IR::F32 nothing = ir.VrAirCompute();
        IR::Inst* const out = colour[0];
        IR::IREmitter at_out{*out->GetParent(), IR::Block::InstructionList::s_iterator_to(*out)};
        out->SetArg(1, at_out.FPAdd(IR::F32{out->Arg(1)}, nothing));
        LOG_INFO(Render_Recompiler, "VR air pass: shader {:#x} computes the moonlight in the air",
                 info.pgm_hash);
        return;
    }
    // (The emitter reads the scene's colour itself, for the source votes: the program's own
    // picture by its own sample handle; ~0: none found.)
    IR::IREmitter ir{*entry, entry->begin()};
    const IR::Value air = ir.VrAirApply(ir.Imm32(own_sample));
    for (u32 c = 0; c < 3; ++c) {
        IR::Inst* const out = colour[c];
        IR::IREmitter at_out{*out->GetParent(), IR::Block::InstructionList::s_iterator_to(*out)};
        const IR::F32 lit{out->Arg(1)};
        const IR::F32 dim{at_out.CompositeExtract(air, 3)};
        out->SetArg(1, at_out.FPAdd(at_out.FPAdd(lit, at_out.FPMul(lit, dim)),
                                    IR::F32{at_out.CompositeExtract(air, c)}));
    }
    // The lamps' glow runs on the VR lighting's setup (emit_spirv_light_shadow.cpp), which samples
    // the depth by this handle: our host depth with the program's own sampler. (The matrix is the
    // emitter's own: no buffer.)
    if (own_sample != ~0u) {
        info.vr_light_shadow = {true, 0, 0, depth_image | (own_sample & 0xFFFF0000u)};
    }
    LOG_INFO(Render_Recompiler, "VR air pass: shader {:#x} applies the air's light{}", info.pgm_hash,
             own_sample != ~0u ? " and the lamps' glow" : " (no sampler: no glow)");
}

} // namespace Shader::Optimization
