// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Lit smoke (particle_air.h): in the game's soft particle programs (RuntimeInfo::VrParticleAir,
// set for their hashes only), recognized from IR:
//  - the distance fog's sqrt(x^2 + z^2) (an FPSqrt of an FPAdd of two squares, each an FPMul of
//    a value by itself, either through an FPNeg or FPAbs), x and z interpolations of one
//    attribute's chans 0 and 2 (expressions of its GetAttribute reads and the barycentrics only):
//    the vector from the fragment to the camera in world axes;
//  - render target 0's last colour writes, all four in one block.
// After those writes, the vector is built again from fresh reads (chans 0, 1 and 2: the fog sits
// inside a branch, its own values would not dominate the writes), VrParticleAir gives the light
// the smoke takes, and render target 0's red, green and blue are written again with it added
// (times the alpha for a premultiplied blend, RuntimeInfo::VrParticleAirPremul). Anything else is
// left as it is, with a log line. Every permutation of these programs declares the lights' buffer
// (they share one Info and so one descriptor layout).

#include <algorithm>
#include <array>
#include <optional>
#include <utility>

#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/passes/particle_air.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {
namespace {

// The value under any FPNeg / FPAbs (a square does not see them).
IR::Value Unsigned(IR::Value v) {
    for (int k = 0; k < 4 && !v.IsImmediate(); ++k) {
        const IR::Inst* const inst = v.InstRecursive();
        if (inst->GetOpcode() != IR::Opcode::FPNeg32 && inst->GetOpcode() != IR::Opcode::FPAbs32) {
            break;
        }
        v = inst->Arg(0);
    }
    return v;
}

bool SameValue(const IR::Value& a, const IR::Value& b) {
    if (a.IsImmediate() || b.IsImmediate()) {
        return false;
    }
    return a.InstRecursive() == b.InstRecursive();
}

// v * v (through any sign), else nothing: v.
std::optional<IR::Value> SquareOf(const IR::Value& v) {
    if (v.IsImmediate()) {
        return std::nullopt;
    }
    const IR::Inst* const m = v.InstRecursive();
    if (m->GetOpcode() != IR::Opcode::FPMul32) {
        return std::nullopt;
    }
    const IR::Value a = Unsigned(m->Arg(0));
    const IR::Value b = Unsigned(m->Arg(1));
    if (!SameValue(a, b)) {
        return std::nullopt;
    }
    return a;
}

bool IsBarycentric(IR::Attribute attribute) {
    return attribute >= IR::Attribute::BaryCoordNoPersp &&
           attribute <= IR::Attribute::BaryCoordPullModel;
}

// The (attribute, chan) every attribute read of `v` is, when `v` is an interpolation: only
// GetAttribute reads of one parameter's chan, the barycentrics, and FPFma / FPSub / FPAdd /
// FPMul of them (a few levels deep). Nothing: something else.
struct Source {
    IR::Attribute attribute;
    u32 chan;
};
bool Collect(const IR::Value& v, std::optional<Source>& found, int depth) {
    if (v.IsImmediate()) {
        return true;
    }
    if (depth > 8) {
        return false;
    }
    const IR::Inst* const inst = v.InstRecursive();
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetAttribute: {
        if (!inst->Arg(1).IsImmediate() || !inst->Arg(2).IsImmediate()) {
            return false;
        }
        const IR::Attribute attribute = inst->Arg(0).Attribute();
        if (IsBarycentric(attribute)) {
            return true;
        }
        if (!IR::IsParam(attribute)) {
            return false;
        }
        const Source here{attribute, inst->Arg(1).U32()};
        if (found && (found->attribute != here.attribute || found->chan != here.chan)) {
            return false;
        }
        found = here;
        return true;
    }
    case IR::Opcode::FPFma32:
    case IR::Opcode::FPSub32:
    case IR::Opcode::FPAdd32:
    case IR::Opcode::FPMul32:
        for (size_t k = 0; k < inst->NumArgs(); ++k) {
            if (!Collect(inst->Arg(k), found, depth + 1)) {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
}

std::optional<Source> SourceOf(const IR::Value& v) {
    std::optional<Source> found;
    if (!Collect(v, found, 0) || !found) {
        return std::nullopt;
    }
    return found;
}

// `v` built again at `ir` (an interpolation Collect accepted), its parameter's reads moved to
// chan `chan`.
IR::F32 Rebuild(IR::IREmitter& ir, const IR::Value& v, const Source& source, u32 chan) {
    if (v.IsImmediate()) {
        return IR::F32{v};
    }
    const IR::Inst* const inst = v.InstRecursive();
    const auto arg = [&](size_t k) { return Rebuild(ir, inst->Arg(k), source, chan); };
    switch (inst->GetOpcode()) {
    case IR::Opcode::GetAttribute: {
        const IR::Attribute attribute = inst->Arg(0).Attribute();
        const u32 index = inst->Arg(2).U32();
        return ir.GetAttribute(attribute, attribute == source.attribute ? chan : inst->Arg(1).U32(),
                               index);
    }
    case IR::Opcode::FPFma32:
        return IR::F32{ir.FPFma(arg(0), arg(1), arg(2))};
    case IR::Opcode::FPSub32:
        return IR::F32{ir.FPSub(arg(0), arg(1))};
    case IR::Opcode::FPAdd32:
        return IR::F32{ir.FPAdd(arg(0), arg(1))};
    case IR::Opcode::FPMul32:
        return IR::F32{ir.FPMul(arg(0), arg(1))};
    default:
        return IR::F32{v}; // not reached: Collect accepted only the cases above
    }
}

// The last write of render target 0's component `component`.
IR::Inst* LastColourWrite(IR::Program& program, u32 component) {
    IR::Inst* write = nullptr;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::SetAttribute &&
                inst.Arg(0).Attribute() == IR::Attribute::RenderTarget0 &&
                inst.Arg(2).IsImmediate() && inst.Arg(2).U32() == component) {
                write = &inst;
            }
        }
    }
    return write;
}

} // namespace

void ParticleAirPass(IR::Program& program, const RuntimeInfo& runtime_info) {
    auto& info = program.info;
    if (info.stage != Stage::Fragment || program.blocks.empty() ||
        !ParticleAir::IsParticleProgram(info.pgm_hash)) {
        return;
    }
    if (std::ranges::find(info.buffers, BufferType::VrParticleLights,
                          &BufferResource::buffer_type) == info.buffers.end()) {
        info.buffers.push_back({
            .used_types = IR::Type::U32,
            .inline_cbuf = AmdGpu::Buffer::Placeholder(ParticleAir::BufferDwords * sizeof(u32)),
            .buffer_type = BufferType::VrParticleLights,
        });
    }
    if ((runtime_info.vr_composite_light & RuntimeInfo::VrParticleAir) == 0) {
        return;
    }
    const auto skip = [&](const char* why) {
        LOG_INFO(Render_Recompiler, "VR lit smoke: shader {:#x} left as it is ({})",
                 info.pgm_hash, why);
    };
    // The fog's horizontal distance: sqrt(x^2 + z^2) of one attribute's chans 0 and 2.
    std::optional<std::pair<IR::Value, Source>> found;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (found || inst.GetOpcode() != IR::Opcode::FPSqrt || inst.Arg(0).IsImmediate()) {
                continue;
            }
            const IR::Inst* const sum = inst.Arg(0).InstRecursive();
            if (sum->GetOpcode() != IR::Opcode::FPAdd32) {
                continue;
            }
            const auto a = SquareOf(sum->Arg(0));
            const auto b = SquareOf(sum->Arg(1));
            if (!a || !b) {
                continue;
            }
            const auto sa = SourceOf(*a);
            const auto sb = SourceOf(*b);
            if (!sa || !sb || sa->attribute != sb->attribute ||
                std::min(sa->chan, sb->chan) != 0 || std::max(sa->chan, sb->chan) != 2) {
                continue;
            }
            found = std::pair{*a, *sa};
        }
    }
    if (!found) {
        return skip("no fog distance");
    }
    // Render target 0's last writes, in one block; ours go after the last of them.
    std::array<IR::Inst*, 4> writes{};
    for (u32 c = 0; c < 4; ++c) {
        writes[c] = LastColourWrite(program, c);
        if (!writes[c] || (c > 0 && writes[c]->GetParent() != writes[0]->GetParent())) {
            return skip("colour writes");
        }
    }
    IR::Block* const block = writes[0]->GetParent();
    IR::Inst* last = nullptr;
    for (IR::Inst& inst : block->Instructions()) {
        if (std::ranges::find(writes, &inst) != writes.end()) {
            last = &inst;
        }
    }
    IR::IREmitter ir{*block, std::next(IR::Block::InstructionList::s_iterator_to(*last))};
    const auto& [x, source] = *found;
    const IR::Value e = ir.CompositeConstruct(Rebuild(ir, x, source, 0), Rebuild(ir, x, source, 1),
                                              Rebuild(ir, x, source, 2));
    const IR::F32 r{writes[0]->Arg(1)};
    const IR::F32 g{writes[1]->Arg(1)};
    const IR::F32 b{writes[2]->Arg(1)};
    const IR::F32 alpha{writes[3]->Arg(1)};
    const IR::Value air = ir.VrParticleAir(e, ir.CompositeConstruct(r, g, b));
    const bool premul = (runtime_info.vr_composite_light & RuntimeInfo::VrParticleAirPremul) != 0;
    const std::array<IR::F32, 3> colour{r, g, b};
    for (u32 c = 0; c < 3; ++c) {
        IR::F32 add{ir.CompositeExtract(air, c)};
        if (premul) {
            add = IR::F32{ir.FPMul(add, IR::F32{ir.FPSaturate(alpha)})};
        }
        ir.SetAttribute(IR::Attribute::RenderTarget0, IR::F32{ir.FPAdd(colour[c], add)}, c);
    }
    LOG_INFO(Render_Recompiler,
             "VR lit smoke: shader {:#x}: lamps light its smoke (fog vector: param {} chans 0..2{})",
             info.pgm_hash, static_cast<u32>(source.attribute) - static_cast<u32>(IR::Attribute::Param0),
             premul ? ", premultiplied" : "");
}

} // namespace Shader::Optimization
