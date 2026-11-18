// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Less shimmer on shiny details (specular_aa.h): in the game's light programs and its ambient
// composite (RuntimeInfo::VrSpecularAa, set for their hashes only), recognized from IR:
//  - the gloss: the one image sample's red that the immediate 13 multiplies (the lights'
//    specular power exp2(13 g), the composite's reflection mip 6.5 - 0.5 * 13 g);
//  - the G-buffer normal: the sample whose x, y and z are each decoded as fma(2, s, -1);
//  - the pixel's depth: an image sample's red that dword 14 of a buffer read at dwords 0..15
//    multiplies in the entry block (the ndc-to-view matrix, as the light shadows find it; its
//    dwords 12 and 13 take the ndc).
// Every use of the gloss becomes VrSpecularAa's, computed in the entry block before the gloss's
// first use there (every input is before it; no branch of the game's has run yet). Anything else
// is left as it is, with a log line saying why.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/passes/specular_aa.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {
namespace {

namespace Tune = SpecularAa;

bool IsSample(IR::Opcode op) {
    return op == IR::Opcode::ImageSampleImplicitLod || op == IR::Opcode::ImageSampleExplicitLod;
}

bool IsImm(const IR::Value& v, float f) {
    return v.IsImmediate() && v.Type() == IR::Type::F32 && v.F32() == f;
}

IR::Inst* As(const IR::Value& v, IR::Opcode op) {
    if (v.IsImmediate()) {
        return nullptr;
    }
    IR::Inst* const inst = v.InstRecursive();
    return inst->GetOpcode() == op ? inst : nullptr;
}

// `v` is component `component` of an image sample (any component when ~0): that extract.
IR::Inst* SampleExtract(const IR::Value& v, u32 component = ~0u) {
    IR::Inst* const e = As(v, IR::Opcode::CompositeExtractF32x4);
    if (!e || e->Arg(0).IsImmediate() || !e->Arg(1).IsImmediate() ||
        (component != ~0u && e->Arg(1).U32() != component)) {
        return nullptr;
    }
    return IsSample(e->Arg(0).InstRecursive()->GetOpcode()) ? e : nullptr;
}

// The float read of a constant buffer's dword, with immediate handle and index (through its
// bitcast): {handle, dword}.
std::optional<std::pair<u32, u32>> ReadOf(const IR::Value& v) {
    if (v.IsImmediate()) {
        return std::nullopt;
    }
    const IR::Inst* i = v.InstRecursive();
    if (i->GetOpcode() == IR::Opcode::BitCastF32U32) {
        if (i->Arg(0).IsImmediate()) {
            return std::nullopt;
        }
        i = i->Arg(0).InstRecursive();
    }
    if (i->GetOpcode() != IR::Opcode::ReadConstBuffer || !i->Arg(0).IsImmediate() ||
        !i->Arg(1).IsImmediate()) {
        return std::nullopt;
    }
    return std::pair{i->Arg(0).U32(), i->Arg(1).U32()};
}

// The value `handle`'s dword `dword` multiplies (FPMul32, or an FPFma32's product) in `block`.
std::optional<IR::Value> Factor(IR::Block* block, u32 handle, u32 dword) {
    for (IR::Inst& inst : block->Instructions()) {
        const auto op = inst.GetOpcode();
        if (op != IR::Opcode::FPMul32 && op != IR::Opcode::FPFma32) {
            continue;
        }
        for (u32 a = 0; a < 2; ++a) {
            const auto r = ReadOf(inst.Arg(a));
            if (r && r->first == handle && r->second == dword) {
                return inst.Arg(1 - a);
            }
        }
    }
    return std::nullopt;
}

struct Position {
    u32 matrix;
    IR::Value depth;
};

// The ndc-to-view matrix (a buffer read at dwords 0..15) whose dwords 12, 13 and 14 multiply the
// pixel's ndc x, y and an image sample's red (its depth) in the entry block.
std::optional<Position> FindPosition(const IR::Program& program, IR::Block* entry) {
    std::map<u32, std::set<u32>> dwords;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::ReadConstBuffer && inst.Arg(0).IsImmediate() &&
                inst.Arg(1).IsImmediate()) {
                dwords[inst.Arg(0).U32()].insert(inst.Arg(1).U32());
            }
        }
    }
    for (const auto& [matrix, used] : dwords) {
        bool rows = true;
        for (u32 d = 0; d < 16; ++d) {
            rows &= used.contains(d);
        }
        if (!rows) {
            continue;
        }
        const auto ndc_x = Factor(entry, matrix, 12);
        const auto ndc_y = Factor(entry, matrix, 13);
        const auto depth = Factor(entry, matrix, 14);
        if (!ndc_x || !ndc_y || !depth || ndc_x->IsImmediate() || ndc_y->IsImmediate() ||
            !SampleExtract(*depth, 0)) {
            continue;
        }
        return Position{matrix, *depth};
    }
    return std::nullopt;
}

} // namespace

void SpecularAaPass(IR::Program& program, const RuntimeInfo& runtime_info) {
    auto& info = program.info;
    if (info.stage != Stage::Fragment || program.blocks.empty() ||
        (runtime_info.vr_ambient_occlusion & RuntimeInfo::VrSpecularAa) == 0) {
        return;
    }
    const auto skip = [&](const char* why) {
        LOG_INFO(Render_Recompiler, "VR specular AA: shader {:#x} left as it is ({})",
                 info.pgm_hash, why);
    };
    IR::Block* const entry = program.blocks.front();
    // The gloss: g * 13, exactly once.
    IR::Inst* gloss = nullptr;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::FPMul32) {
                continue;
            }
            for (u32 a = 0; a < 2; ++a) {
                IR::Inst* const g = SampleExtract(inst.Arg(1 - a), 0);
                if (!IsImm(inst.Arg(a), 13.0f) || !g) {
                    continue;
                }
                if (gloss && gloss != g) {
                    return skip("two gloss reads");
                }
                gloss = g;
            }
        }
    }
    if (!gloss) {
        return skip("no gloss read");
    }
    if (gloss->GetParent() != entry) {
        return skip("gloss outside the entry");
    }
    // The normal: fma(2, s.c, -1) for c = 0, 1, 2 of one sample.
    IR::Inst* normal = nullptr;
    std::array<bool, 3> decoded{};
    for (IR::Inst& inst : entry->Instructions()) {
        if (inst.GetOpcode() != IR::Opcode::FPFma32 || !IsImm(inst.Arg(2), -1.0f)) {
            continue;
        }
        for (u32 a = 0; a < 2; ++a) {
            IR::Inst* const e = SampleExtract(inst.Arg(1 - a));
            if (!IsImm(inst.Arg(a), 2.0f) || !e || e->Arg(1).U32() > 2) {
                continue;
            }
            IR::Inst* const s = e->Arg(0).InstRecursive();
            if (normal && normal != s) {
                return skip("two normal decodes");
            }
            normal = s;
            decoded[e->Arg(1).U32()] = true;
        }
    }
    if (!normal || !std::ranges::all_of(decoded, [](bool d) { return d; })) {
        return skip("no G-buffer normal");
    }
    const auto position = FindPosition(program, entry);
    if (!position) {
        return skip("no view position");
    }
    // Where: before the gloss's first use in the entry, after every input.
    const auto uses = gloss->Uses(); // the game's, before ours is added
    std::set<const IR::Inst*> users;
    for (const IR::Use& use : uses) {
        users.insert(use.user);
    }
    std::vector<const IR::Inst*> inputs{gloss, normal, position->depth.InstRecursive()};
    if (std::ranges::any_of(inputs,
                            [&](const IR::Inst* i) { return i->GetParent() != entry; })) {
        return skip("inputs outside the entry");
    }
    IR::Inst* first_use = nullptr;
    size_t seen_inputs = 0;
    for (IR::Inst& inst : entry->Instructions()) {
        if (users.contains(&inst)) {
            first_use = &inst;
            break;
        }
        if (std::ranges::find(inputs, &inst) != inputs.end()) {
            ++seen_inputs;
        }
    }
    if (!first_use) {
        return skip("no use of the gloss in the entry");
    }
    std::ranges::sort(inputs);
    const auto distinct = std::ranges::unique(inputs).begin() - inputs.begin();
    if (seen_inputs != static_cast<size_t>(distinct)) {
        return skip("order");
    }
    IR::IREmitter ir{*entry, IR::Block::InstructionList::s_iterator_to(*first_use)};
    const IR::F32 game{IR::Value{gloss}};
    const IR::F32 widened = ir.VrSpecularAa(game, IR::Value{normal}, IR::F32{position->depth},
                                            ir.Imm32(position->matrix));
    for (const IR::Use& use : uses) {
        use.user->SetArg(use.operand, widened);
    }
    // Tests (VR_SPEC_AA_DEBUG=1): a light's specular red gains DebugScale * (g - g'), red where
    // the gloss was widened. Not in the composite: its shine target is the specular occlusion's.
    static const bool debug = [] {
        const char* const env = std::getenv("VR_SPEC_AA_DEBUG");
        return env != nullptr && env[0] == '1';
    }();
    if (debug && info.pgm_hash != 0x3fe9e964u) {
        IR::Inst* red = nullptr;
        for (IR::Block* block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() == IR::Opcode::SetAttribute &&
                    inst.Arg(0).Attribute() == IR::Attribute::RenderTarget1 &&
                    inst.Arg(2).IsImmediate() && inst.Arg(2).U32() == 0) {
                    red = &inst;
                }
            }
        }
        if (red) {
            IR::IREmitter at{*red->GetParent(), IR::Block::InstructionList::s_iterator_to(*red)};
            const IR::F32 mark = at.FPSaturate(
                at.FPMul(at.FPSub(game, widened), at.Imm32(Tune::DebugScale)));
            red->SetArg(1, at.FPAdd(IR::F32{red->Arg(1)}, mark));
        }
    }
    LOG_INFO(Render_Recompiler,
             "VR specular AA: shader {:#x}: gloss widened by the normal's variance (matrix {}, "
             "{} gloss use(s))",
             info.pgm_hash, position->matrix, uses.size());
}

} // namespace Shader::Optimization
