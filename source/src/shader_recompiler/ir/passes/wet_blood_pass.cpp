// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Wet, glistening blood (wet_blood.h): in the game's _Bloody_ G-buffer programs
// (RuntimeInfo::VrWetBlood, set for their hashes only), recognized from IR:
//  - render target 3's red, green and blue are sqrt(F0) of three blends f = B + A (X - B) (an
//    FPAdd of an FPMul, or an FPFma), A a constant-buffer float (FC_BloodAmount), X - B an FPSub
//    whose B is the blend's own, X = W B + Y with W an image sample's alpha (the painted blood);
//  - the gloss: a blend of the same A and the same blood texture within a few steps of render
//    target 1's red (the gloss the lights read).
// Each blend's result is raised towards the wet value by the blood's coverage A (1 - W) right
// after it is computed, so all its uses (the G-buffer, the material's own reflection's mip and
// Fresnel, its ambient term) take the wet one. Anything else is left as it is, with a log line.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <set>
#include <utility>
#include <vector>

#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/passes/wet_blood.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {
namespace {

namespace Tune = WetBlood;

bool IsSample(IR::Opcode op) {
    return op == IR::Opcode::ImageSampleImplicitLod || op == IR::Opcode::ImageSampleExplicitLod;
}

IR::Inst* As(const IR::Value& v, IR::Opcode op) {
    if (v.IsImmediate()) {
        return nullptr;
    }
    IR::Inst* const inst = v.InstRecursive();
    return inst->GetOpcode() == op ? inst : nullptr;
}

// `v` is component `component` of an image sample: that extract.
IR::Inst* SampleExtract(const IR::Value& v, u32 component) {
    IR::Inst* const e = As(v, IR::Opcode::CompositeExtractF32x4);
    if (!e || e->Arg(0).IsImmediate() || !e->Arg(1).IsImmediate() ||
        e->Arg(1).U32() != component) {
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

// One blood blend: its amount A (the constant-buffer float, and which dword it is) and the blood
// texture's alpha W (an extract).
struct Blend {
    std::pair<u32, u32> read;
    IR::Value amount;
    IR::Inst* mask;
};

// f = B + A (X - B) (FPAdd of FPMul either way round, or FPFma(A, X - B, B)), X = W B + Y
// (FPAdd of FPMul either way round, or FPFma(W, B, Y)).
std::optional<Blend> BloodBlend(IR::Inst* f) {
    IR::Value amount, difference, base;
    bool found = false;
    const auto product = [&](const IR::Inst* m, const IR::Value& b) {
        for (u32 j = 0; j < 2; ++j) {
            if (ReadOf(m->Arg(j))) {
                amount = m->Arg(j);
                difference = m->Arg(1 - j);
                base = b;
                return true;
            }
        }
        return false;
    };
    if (f->GetOpcode() == IR::Opcode::FPAdd32) {
        for (u32 k = 0; k < 2 && !found; ++k) {
            if (const IR::Inst* const m = As(f->Arg(k), IR::Opcode::FPMul32)) {
                found = product(m, f->Arg(1 - k));
            }
        }
    } else if (f->GetOpcode() == IR::Opcode::FPFma32) {
        found = product(f, f->Arg(2));
    }
    if (!found || base.IsImmediate()) {
        return std::nullopt;
    }
    const IR::Inst* const b = base.InstRecursive();
    const IR::Inst* const sub = As(difference, IR::Opcode::FPSub32);
    if (!sub || sub->Arg(1).IsImmediate() || sub->Arg(1).InstRecursive() != b) {
        return std::nullopt;
    }
    // W B, either way round.
    const auto mask_of = [&](const IR::Inst* m) -> IR::Inst* {
        for (u32 j = 0; j < 2; ++j) {
            IR::Inst* const w = SampleExtract(m->Arg(j), 3);
            if (w && !m->Arg(1 - j).IsImmediate() && m->Arg(1 - j).InstRecursive() == b) {
                return w;
            }
        }
        return nullptr;
    };
    IR::Inst* mask = nullptr;
    const IR::Value x = sub->Arg(0);
    if (const IR::Inst* const add = As(x, IR::Opcode::FPAdd32)) {
        for (u32 k = 0; k < 2 && !mask; ++k) {
            if (const IR::Inst* const m = As(add->Arg(k), IR::Opcode::FPMul32)) {
                mask = mask_of(m);
            }
        }
    } else if (const IR::Inst* const fma = As(x, IR::Opcode::FPFma32)) {
        mask = mask_of(fma);
    }
    if (!mask) {
        return std::nullopt;
    }
    return Blend{*ReadOf(amount), amount, mask};
}

bool SameBlood(const Blend& a, const Blend& b) {
    return a.read == b.read && a.mask->Arg(0).InstRecursive() == b.mask->Arg(0).InstRecursive();
}

// A blend like `want` within `depth` steps up `v`'s arguments.
IR::Inst* FindBlend(const IR::Value& v, const Blend& want, int depth) {
    if (v.IsImmediate() || depth < 0) {
        return nullptr;
    }
    IR::Inst* const inst = v.InstRecursive();
    if (const auto blend = BloodBlend(inst); blend && SameBlood(*blend, want)) {
        return inst;
    }
    for (size_t k = 0; k < inst->NumArgs(); ++k) {
        if (IR::Inst* const found = FindBlend(inst->Arg(k), want, depth - 1)) {
            return found;
        }
    }
    return nullptr;
}

// The last write of render target `target`'s component `component`.
IR::Inst* LastWrite(IR::Program& program, IR::Attribute target, u32 component) {
    IR::Inst* write = nullptr;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::SetAttribute && inst.Arg(0).Attribute() == target &&
                inst.Arg(2).IsImmediate() && inst.Arg(2).U32() == component) {
                write = &inst;
            }
        }
    }
    return write;
}

// x's exponent bits as integers: a number (without SignedZeroInfNanPreserve a driver may fold a
// float NaN or Inf test away).
IR::U1 IsNumber(IR::IREmitter& ir, const IR::F32& x) {
    const IR::U32 bits = ir.BitCast<IR::U32, IR::F32>(x);
    return ir.INotEqual(IR::U32{ir.BitwiseAnd(bits, ir.Imm32(0x7f800000u))},
                        ir.Imm32(0x7f800000u));
}

// The blood's coverage of the pixel: A (1 - W), A within 0..1 (a constant that is not a number
// counts as no blood).
IR::F32 Coverage(IR::IREmitter& ir, const Blend& blend) {
    const IR::F32 amount{blend.amount};
    const IR::F32 safe{ir.Select(IsNumber(ir, amount), amount, ir.Imm32(0.0f))};
    return ir.FPMul(ir.FPSaturate(safe),
                    ir.FPSub(ir.Imm32(1.0f), IR::F32{IR::Value{blend.mask}}));
}

// `a` and `b` in one block, `a` first; or `a` in the entry (which dominates every block).
bool Dominates(const IR::Inst* a, const IR::Inst* b, const IR::Block* entry) {
    if (a->GetParent() == entry && b->GetParent() != entry) {
        return true;
    }
    if (a->GetParent() != b->GetParent()) {
        return false;
    }
    for (const IR::Inst& inst : a->GetParent()->Instructions()) {
        if (&inst == a) {
            return true;
        }
        if (&inst == b) {
            return false;
        }
    }
    return false;
}

} // namespace

void WetBloodPass(IR::Program& program, const RuntimeInfo& runtime_info) {
    auto& info = program.info;
    if (info.stage != Stage::Fragment || program.blocks.empty() ||
        (runtime_info.vr_composite_light & RuntimeInfo::VrWetBlood) == 0) {
        return;
    }
    const auto skip = [&](const char* why) {
        LOG_INFO(Render_Recompiler, "VR wet blood: shader {:#x} left as it is ({})",
                 info.pgm_hash, why);
    };
    // F0: render target 3's colour, sqrt of a blend each.
    std::vector<std::pair<IR::Inst*, float>> sites;
    std::optional<Blend> blood;
    for (u32 c = 0; c < 3; ++c) {
        IR::Inst* const write = LastWrite(program, IR::Attribute::RenderTarget3, c);
        IR::Inst* const root = write ? As(write->Arg(1), IR::Opcode::FPSqrt) : nullptr;
        if (!root || root->Arg(0).IsImmediate()) {
            return skip("no sqrt(F0) target");
        }
        IR::Inst* const f = root->Arg(0).InstRecursive();
        const auto blend = BloodBlend(f);
        if (!blend || (blood && !SameBlood(*blend, *blood))) {
            return skip("F0 blend");
        }
        blood = blend;
        if (std::ranges::none_of(sites, [&](const auto& s) { return s.first == f; })) {
            sites.emplace_back(f, Tune::F0Wet);
        }
    }
    // The gloss: the same blood's blend near render target 1's red.
    IR::Inst* const gloss_write = LastWrite(program, IR::Attribute::RenderTarget1, 0);
    IR::Inst* const gloss = gloss_write ? FindBlend(gloss_write->Arg(1), *blood, 4) : nullptr;
    if (!gloss) {
        return skip("no gloss blend");
    }
    sites.emplace_back(gloss, Tune::GlossWet);
    // Each blend raised towards its wet value by the coverage, right after it.
    for (const auto& [site, target] : sites) {
        const auto blend = BloodBlend(site); // its own amount and mask (they dominate it)
        const auto uses = site->Uses();      // the game's, before ours are added
        IR::IREmitter ir{*site->GetParent(),
                         std::next(IR::Block::InstructionList::s_iterator_to(*site))};
        const IR::F32 value{IR::Value{site}};
        const IR::F32 cover = ir.FPMul(Coverage(ir, *blend), ir.Imm32(Tune::Strength));
        const IR::F32 wet = ir.FPMax(value, ir.Imm32(target)); // only ever raises it
        const IR::F32 raised = ir.FPFma(cover, ir.FPSub(wet, value), value);
        // A blend that is not a number stays the game's (FPMax saw it only on that path).
        const IR::F32 out{ir.Select(IsNumber(ir, value), raised, value)};
        for (const IR::Use& use : uses) {
            use.user->SetArg(use.operand, out);
        }
    }
    // The albedo (render target 2): a little darker where wet; VR_WET_BLOOD_DEBUG=1 (tests): the
    // coverage painted green instead. Only where the blood's inputs come before the writes.
    static const bool debug = [] {
        const char* const env = std::getenv("VR_WET_BLOOD_DEBUG");
        return env != nullptr && env[0] == '1';
    }();
    const IR::Block* const entry = program.blocks.front();
    const IR::Inst* const amount_inst = blood->amount.InstRecursive();
    u32 darkened = 0;
    for (u32 c = 0; c < 3; ++c) {
        IR::Inst* const write = LastWrite(program, IR::Attribute::RenderTarget2, c);
        if (!write || !Dominates(amount_inst, write, entry) ||
            !Dominates(blood->mask, write, entry)) {
            continue;
        }
        IR::IREmitter at{*write->GetParent(), IR::Block::InstructionList::s_iterator_to(*write)};
        const IR::F32 albedo{write->Arg(1)};
        const IR::F32 cover = Coverage(at, *blood);
        const IR::F32 out =
            debug ? IR::F32{at.FPFma(cover, at.FPSub(at.Imm32(c == 1 ? 1.0f : 0.0f), albedo),
                                     albedo)}
                  : IR::F32{at.FPMul(albedo, at.FPFma(cover, at.Imm32(-Tune::WetDarken),
                                                      at.Imm32(1.0f)))};
        write->SetArg(1, out);
        ++darkened;
    }
    LOG_INFO(Render_Recompiler,
             "VR wet blood: shader {:#x}: F0 and gloss wet where its blood lies (amount buffer {} "
             "dword {}, {} blend(s), albedo {}){}",
             info.pgm_hash, blood->read.first, blood->read.second, sites.size(),
             darkened == 3 ? "darkened" : "as it is", debug ? " [coverage debug]" : "");
}

} // namespace Shader::Optimization
