// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Wet streets (reflections.h): the game's trace (RuntimeInfo::VrReflections, its program only)
// and its composite (RuntimeInfo::VrReflectionComposite), recognized from IR.
//  - The trace: a buffer read at dwords 0..15 (ndc to view: rows times ndc x, y and the depth's
//    x in the entry block), 16..27 (view to clip), 32..42 (the normals' rotation), 48..63 (the
//    reprojection) and 64..79 (the trace's parameters); two samples in the entry (the depth, the
//    G-buffer normals) and gradient samples in the march (the depth again, last frame's colour).
//    Dword 64 (the game's "fade to viewer" limit) becomes 1e30 for the game's own code, so its
//    march is never entered; VrReflection's result is written to render target 0 after the
//    game's own (the last write counts).
//  - The composite: the one explicit-lod sample whose lod is min(6, ...) (the reflection's mip by
//    the surface's gloss): its colour divided by its weight (the trace's output is weighted).

#include <algorithm>
#include <array>
#include <map>
#include <optional>
#include <set>
#include <vector>

#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ambient_occlusion.h"
#include "shader_recompiler/ir/passes/cinematic_look.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/passes/reflections.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {
namespace {

namespace Tune = Reflections;

bool IsSample(IR::Opcode op) {
    return op == IR::Opcode::ImageSampleImplicitLod || op == IR::Opcode::ImageSampleExplicitLod;
}

// `v` is the float read of `handle`'s dword `dword` (through a bitcast).
bool IsRead(const IR::Value& v, u32 handle, u32 dword) {
    if (v.IsImmediate()) {
        return false;
    }
    const IR::Inst* i = v.InstRecursive();
    if (i->GetOpcode() == IR::Opcode::BitCastF32U32) {
        if (i->Arg(0).IsImmediate()) {
            return false;
        }
        i = i->Arg(0).InstRecursive();
    }
    return i->GetOpcode() == IR::Opcode::ReadConstBuffer && i->Arg(0).IsImmediate() &&
           i->Arg(1).IsImmediate() && i->Arg(0).U32() == handle && i->Arg(1).U32() == dword;
}

// The value `handle`'s dword `dword` multiplies first (FPMul32, or an FPFma32's product), in
// any block (the game computes the view position inside its `depth < 1` branch).
std::optional<IR::Value> Factor(const IR::Program& program, u32 handle, u32 dword) {
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            const auto op = inst.GetOpcode();
            if (op != IR::Opcode::FPMul32 && op != IR::Opcode::FPFma32) {
                continue;
            }
            for (u32 a = 0; a < 2; ++a) {
                if (IsRead(inst.Arg(a), handle, dword)) {
                    return inst.Arg(1 - a);
                }
            }
        }
    }
    return std::nullopt;
}

// `v` where VrReflection goes (before `at` in `block`): the entry's own as it is (the entry
// dominates the output block), else its arithmetic copied there from interpolated inputs (the
// game interpolates its ndc inside its `depth < 1` branch; an input reads the same anywhere in
// a fragment shader). Only pure arithmetic, extracts and inputs, a few levels deep.
std::optional<IR::Value> CopyTo(IR::Block* block, IR::Block::iterator at, const IR::Value& v,
                                IR::Block* entry, u32 depth = 0) {
    if (v.IsImmediate()) {
        return v;
    }
    IR::Inst* const inst = v.InstRecursive();
    if (inst->GetParent() == entry) {
        return v;
    }
    const std::string_view name = IR::NameOf(inst->GetOpcode());
    const bool pure = name.starts_with("FP") || name.starts_with("Composite") ||
                      name.starts_with("BitCast") || name.starts_with("GetAttribute");
    if (depth > 12 || !pure || inst->GetOpcode() == IR::Opcode::Phi ||
        inst->MayHaveSideEffects()) {
        return std::nullopt;
    }
    std::vector<IR::Value> args;
    for (size_t k = 0; k < inst->NumArgs(); ++k) {
        const auto arg = CopyTo(block, at, inst->Arg(k), entry, depth + 1);
        if (!arg) {
            return std::nullopt;
        }
        args.push_back(*arg);
    }
    IR::Inst* const copy = &*block->PrependNewInst(at, *inst);
    for (size_t k = 0; k < args.size(); ++k) {
        copy->SetArg(k, args[k]);
    }
    return IR::Value{copy};
}

// Every immediate constant-buffer read, by handle.
std::map<u32, std::set<u32>> ReadDwords(const IR::Program& program) {
    std::map<u32, std::set<u32>> dwords;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::ReadConstBuffer && inst.Arg(0).IsImmediate() &&
                inst.Arg(1).IsImmediate()) {
                dwords[inst.Arg(0).U32()].insert(inst.Arg(1).U32());
            }
        }
    }
    return dwords;
}

bool TraceDwords(const std::set<u32>& used) {
    const auto all = [&](u32 from, u32 to) {
        for (u32 d = from; d <= to; ++d) {
            if (!used.contains(d)) {
                return false;
            }
        }
        return true;
    };
    return all(0, 27) && all(32, 34) && all(36, 38) && all(40, 42) && all(48, 55) &&
           all(60, 64) && all(68, 69) && used.contains(72) && all(76, 79);
}

void TracePass(IR::Program& program, const RuntimeInfo& runtime_info) {
    auto& info = program.info;
    IR::Block* const entry = program.blocks.front();
    const auto skip = [&](const char* why) {
        LOG_INFO(Render_Recompiler, "VR reflections: shader {:#x} left as it is ({})",
                 info.pgm_hash, why);
    };
    // Render target 0's four writes, in one block.
    std::array<IR::Inst*, 4> out{};
    IR::Inst* last = nullptr;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::SetAttribute &&
                inst.Arg(0).Attribute() == IR::Attribute::RenderTarget0 &&
                inst.Arg(2).IsImmediate() && inst.Arg(2).U32() < 4) {
                out[inst.Arg(2).U32()] = &inst;
                last = &inst;
            }
        }
    }
    if (std::ranges::any_of(out, [](const IR::Inst* i) { return i == nullptr; }) ||
        std::ranges::any_of(out, [&](const IR::Inst* i) {
            return i->GetParent() != last->GetParent();
        })) {
        return skip("outputs");
    }
    for (const auto& [buffer, used] : ReadDwords(program)) {
        if (!TraceDwords(used)) {
            LOG_INFO(Render_Recompiler, "VR reflections: shader {:#x} buffer {}: {} dwords read, "
                     "not the trace's", info.pgm_hash, buffer, used.size());
            continue;
        }
        const auto ndc_x = Factor(program, buffer, 0);
        const auto ndc_y = Factor(program, buffer, 1);
        const auto depth = Factor(program, buffer, 2);
        if (!ndc_x || !ndc_y || !depth || ndc_x->IsImmediate() || ndc_y->IsImmediate() ||
            depth->IsImmediate()) {
            LOG_INFO(Render_Recompiler, "VR reflections: shader {:#x} buffer {}: no ndc / depth "
                     "factors ({} {} {})", info.pgm_hash, buffer, ndc_x.has_value(),
                     ndc_y.has_value(), depth.has_value());
            continue;
        }
        IR::Inst* const extract = depth->InstRecursive();
        if (extract->GetOpcode() != IR::Opcode::CompositeExtractF32x4 ||
            !extract->Arg(1).IsImmediate() || extract->Arg(1).U32() != 0 ||
            extract->Arg(0).IsImmediate()) {
            LOG_INFO(Render_Recompiler, "VR reflections: shader {:#x} buffer {}: depth from {}",
                     info.pgm_hash, buffer, IR::NameOf(extract->GetOpcode()));
            continue;
        }
        IR::Inst* const depth_sample = extract->Arg(0).InstRecursive();
        if (!IsSample(depth_sample->GetOpcode()) || !depth_sample->Arg(0).IsImmediate() ||
            depth_sample->GetParent() != entry) {
            LOG_INFO(Render_Recompiler, "VR reflections: shader {:#x} buffer {}: depth sample {} "
                     "(in the entry: {})", info.pgm_hash, buffer,
                     IR::NameOf(depth_sample->GetOpcode()), depth_sample->GetParent() == entry);
            continue;
        }
        const u32 depth_image = depth_sample->Arg(0).U32();
        // The entry's other sample: the G-buffer normals (one image).
        std::optional<u32> normal_image;
        for (IR::Inst& inst : entry->Instructions()) {
            if (&inst == depth_sample || !IsSample(inst.GetOpcode()) ||
                !inst.Arg(0).IsImmediate()) {
                continue;
            }
            const u32 h = inst.Arg(0).U32();
            if ((h & 0xFFFFu) == (depth_image & 0xFFFFu) ||
                (normal_image && *normal_image != h)) {
                return skip("more than two images in the entry");
            }
            normal_image = h;
        }
        // The march's gradient samples: the depth and last frame's colour (one image).
        std::optional<u32> colour_image;
        for (IR::Block* block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() != IR::Opcode::ImageGradient || !inst.Arg(0).IsImmediate()) {
                    continue;
                }
                const u32 h = inst.Arg(0).U32();
                if ((h & 0xFFFFu) == (depth_image & 0xFFFFu)) {
                    continue;
                }
                if (colour_image && *colour_image != h) {
                    return skip("more than one colour image");
                }
                colour_image = h;
            }
        }
        if (!normal_image || !colour_image) {
            return skip("no normals or no colour");
        }
        // The game's march off: its limit on the reflected ray's view z (dword 64) made 1e30.
        std::vector<IR::Inst*> limit_casts;
        for (IR::Block* block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() != IR::Opcode::ReadConstBuffer || !inst.Arg(0).IsImmediate() ||
                    !inst.Arg(1).IsImmediate() || inst.Arg(0).U32() != buffer ||
                    inst.Arg(1).U32() != 64) {
                    continue;
                }
                for (const IR::Use& use : inst.Uses()) {
                    if (use.user->GetOpcode() != IR::Opcode::BitCastF32U32) {
                        return skip("dword 64 read as an integer");
                    }
                    limit_casts.push_back(use.user);
                }
            }
        }
        if (limit_casts.empty()) {
            return skip("no march limit");
        }
        IR::Block* const block = last->GetParent();
        const auto at = std::next(IR::Block::InstructionList::s_iterator_to(*last));
        // The ndc where VrReflection goes (copied there; checked before anything is changed).
        const auto x_here = CopyTo(block, at, *ndc_x, entry);
        const auto y_here = x_here ? CopyTo(block, at, *ndc_y, entry) : std::nullopt;
        if (!x_here || !y_here) {
            LOG_INFO(Render_Recompiler, "VR reflections: shader {:#x}: ndc from {} / {}",
                     info.pgm_hash, IR::NameOf(ndc_x->InstRecursive()->GetOpcode()),
                     IR::NameOf(ndc_y->InstRecursive()->GetOpcode()));
            return skip("ndc not copyable");
        }
        IR::IREmitter ir{*block, at};
        const IR::Value ndc = ir.CompositeConstruct(IR::F32{*x_here}, IR::F32{*y_here});
        const IR::Value ours = ir.VrReflection(ndc, depth_sample->Arg(1), IR::F32{*depth});
        for (u32 c = 0; c < 4; ++c) {
            ir.SetAttribute(IR::Attribute::RenderTarget0, IR::F32{ir.CompositeExtract(ours, c)}, c);
        }
        for (IR::Inst* cast : limit_casts) {
            cast->ReplaceUsesWithAndRemove(IR::Value{1e30f});
        }
        info.vr_reflection = {
            .on = true,
            .jitter = (runtime_info.vr_ambient_occlusion & RuntimeInfo::VrReflectionsJitter) != 0,
            .buffer = buffer,
            .depth_image = depth_image,
            .normal_image = *normal_image,
            .colour_image = *colour_image,
        };
        LOG_INFO(Render_Recompiler,
                 "VR reflections: shader {:#x}: ours in place of the game's march (buffer {}, "
                 "depth {:#x}, normals {:#x}, colour {:#x})",
                 info.pgm_hash, buffer, depth_image, *normal_image, *colour_image);
        return;
    }
    skip("no trace buffer");
}

bool IsSix(const IR::Value& v) {
    return v.IsImmediate() && v.Type() == IR::Type::F32 && v.F32() == 6.0f;
}

// The composite's reflection: the explicit-lod sample whose lod is min(6, ...) or min(..., 6);
// null (with the reason) when there is none or there are two.
IR::Inst* ReflectionSample(IR::Program& program, const char*& why) {
    IR::Inst* sample = nullptr;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::ImageSampleExplicitLod || inst.Arg(2).IsImmediate()) {
                continue;
            }
            const IR::Inst* lod = inst.Arg(2).InstRecursive();
            if (lod->GetOpcode() == IR::Opcode::FPMin32 && (IsSix(lod->Arg(0)) || IsSix(lod->Arg(1)))) {
                if (sample) {
                    why = "two reflection samples";
                    return nullptr;
                }
                sample = &inst;
            }
        }
    }
    if (!sample) {
        why = "no reflection sample";
    }
    return sample;
}

IR::Inst* As(const IR::Value& v, IR::Opcode op) {
    if (v.IsImmediate()) {
        return nullptr;
    }
    IR::Inst* const inst = v.InstRecursive();
    return inst->GetOpcode() == op ? inst : nullptr;
}

// s.x * s.y of one image sample, either way round (the composite's AO mask).
bool IsAoProduct(const IR::Inst* m) {
    if (!m || m->GetOpcode() != IR::Opcode::FPMul32) {
        return false;
    }
    const IR::Inst* const a = As(m->Arg(0), IR::Opcode::CompositeExtractF32x4);
    const IR::Inst* const b = As(m->Arg(1), IR::Opcode::CompositeExtractF32x4);
    if (!a || !b || a->Arg(0).IsImmediate() || b->Arg(0).IsImmediate() ||
        !a->Arg(1).IsImmediate() || !b->Arg(1).IsImmediate()) {
        return false;
    }
    const IR::Inst* const s = a->Arg(0).InstRecursive();
    const u32 ia = a->Arg(1).U32(), ib = b->Arg(1).U32();
    return s == b->Arg(0).InstRecursive() && IsSample(s->GetOpcode()) &&
           ((ia == 0 && ib == 1) || (ia == 1 && ib == 0));
}

// Shine stays out of cracks (ambient_occlusion.h SpecOcclusion). The composite's second target
// (its shine), per colour: M * spec, M = ao * L (ao the AO mask's product, L its other light
// term; the first target shares M). Its N.V: Schlick's exp2(5 log2(x)), x = 1 - N.V. Its gloss:
// the reflection's lod, min(6, 6.5 - 6.5 gloss). M becomes so * L, so Lagarde's occlusion.
void SpecularOcclusionPass(IR::Program& program) {
    namespace AO = AmbientOcclusion;
    auto& info = program.info;
    const auto skip = [&](const char* why) {
        LOG_INFO(Render_Recompiler, "VR specular occlusion: composite {:#x} left as it is ({})",
                 info.pgm_hash, why);
    };
    std::array<IR::Inst*, 3> rt1{};
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::SetAttribute &&
                inst.Arg(0).Attribute() == IR::Attribute::RenderTarget1 &&
                inst.Arg(2).IsImmediate() && inst.Arg(2).U32() < 3) {
                rt1[inst.Arg(2).U32()] = &inst;
            }
        }
    }
    if (std::ranges::any_of(rt1, [](const IR::Inst* i) { return i == nullptr; })) {
        return skip("no shine target");
    }
    IR::Inst* ao = nullptr;
    std::array<IR::Inst*, 3> shine{};
    std::array<size_t, 3> slot{};
    std::array<IR::Value, 3> rest;
    for (u32 c = 0; c < 3; ++c) {
        shine[c] = As(rt1[c]->Arg(1), IR::Opcode::FPMul32);
        if (!shine[c]) {
            return skip("shine shape");
        }
        bool found = false;
        for (size_t k = 0; k < 2 && !found; ++k) {
            IR::Inst* const m = As(shine[c]->Arg(k), IR::Opcode::FPMul32);
            if (!m) {
                continue;
            }
            for (size_t j = 0; j < 2; ++j) {
                IR::Inst* const a = As(m->Arg(j), IR::Opcode::FPMul32);
                if (!IsAoProduct(a)) {
                    continue;
                }
                if (ao && ao != a) {
                    return skip("two AO products");
                }
                ao = a;
                slot[c] = k;
                rest[c] = m->Arg(1 - j);
                found = true;
                break;
            }
        }
        if (!found) {
            return skip("no AO factor on the shine");
        }
    }
    // Schlick's (1 - N.V)^5: exp2(5 log2(x)).
    IR::Inst* schlick_x = nullptr;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::FPExp2) {
                continue;
            }
            IR::Inst* const mul = As(inst.Arg(0), IR::Opcode::FPMul32);
            if (!mul) {
                continue;
            }
            for (size_t j = 0; j < 2; ++j) {
                const IR::Value five = mul->Arg(j);
                IR::Inst* const log = As(mul->Arg(1 - j), IR::Opcode::FPLog2);
                if (five.IsImmediate() && five.Type() == IR::Type::F32 && five.F32() == 5.0f &&
                    log) {
                    if (schlick_x && schlick_x != log) {
                        return skip("two Fresnel terms");
                    }
                    schlick_x = log;
                }
            }
        }
    }
    if (!schlick_x || schlick_x->Arg(0).IsImmediate()) {
        return skip("no Fresnel term");
    }
    const char* why = nullptr;
    IR::Inst* const sample = ReflectionSample(program, why);
    if (!sample) {
        return skip(why);
    }
    const IR::Inst* const lod_min = sample->Arg(2).InstRecursive();
    const IR::Value raw = IsSix(lod_min->Arg(0)) ? lod_min->Arg(1) : lod_min->Arg(0);
    // One block, every input before the first shine product.
    IR::Block* const block = shine[0]->GetParent();
    const IR::Inst* const x_inst = schlick_x->Arg(0).InstRecursive();
    if (std::ranges::any_of(shine, [&](const IR::Inst* i) { return i->GetParent() != block; }) ||
        ao->GetParent() != block || x_inst->GetParent() != block ||
        (!raw.IsImmediate() && raw.InstRecursive()->GetParent() != block)) {
        return skip("blocks");
    }
    IR::Inst* first = nullptr;
    bool inputs_before = true;
    u32 seen_inputs = 0;
    for (IR::Inst& inst : block->Instructions()) {
        if (&inst == ao || &inst == x_inst || (!raw.IsImmediate() && &inst == raw.InstRecursive())) {
            ++seen_inputs;
        }
        if (&inst == shine[0] || &inst == shine[1] || &inst == shine[2]) {
            if (!first) {
                first = &inst;
                inputs_before = seen_inputs == (raw.IsImmediate() ? 2u : 3u);
            }
        }
    }
    if (!first || !inputs_before) {
        return skip("order");
    }
    IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(*first)};
    const IR::F32 one = ir.Imm32(1.0f);
    const IR::F32 occlusion{IR::Value{ao}};
    const IR::F32 ndv = ir.FPSub(one, IR::F32{schlick_x->Arg(0)});
    const IR::F32 rough = ir.FPSaturate(ir.FPMul(IR::F32{raw}, ir.Imm32(1.0f / 6.5f)));
    const IR::F32 e = ir.FPExp2(ir.FPFma(ir.FPMul(rough, rough), ir.Imm32(-16.0f), ir.Imm32(-1.0f)));
    const IR::F32 base = ir.FPMax(ir.FPAdd(ndv, occlusion), ir.Imm32(1e-4f));
    IR::F32 so = ir.FPSaturate(
        ir.FPAdd(ir.FPExp2(ir.FPMul(e, ir.FPLog2(base))), ir.FPSub(occlusion, one)));
    if constexpr (AO::SpecDarkenOnly) {
        so = ir.FPMin(so, occlusion);
    }
    so = ir.FPFma(ir.FPSub(so, occlusion), ir.Imm32(AO::SpecOcclusion), occlusion);
    // A degenerate normal's N.V (not a number) or gloss: the game's own AO. (The exponent's bits
    // as integers: without SignedZeroInfNanPreserve a driver may fold a float test away.)
    const auto finite = [&](const IR::F32& v) {
        const IR::U32 bits = ir.BitCast<IR::U32, IR::F32>(v);
        return ir.INotEqual(IR::U32{ir.BitwiseAnd(bits, ir.Imm32(0x7f800000u))},
                            ir.Imm32(0x7f800000u));
    };
    so = IR::F32{ir.Select(ir.LogicalAnd(finite(ndv), finite(rough)), so, occlusion)};
    for (u32 c = 0; c < 3; ++c) {
        IR::IREmitter at{*block, IR::Block::InstructionList::s_iterator_to(*shine[c])};
        shine[c]->SetArg(slot[c], at.FPMul(so, IR::F32{rest[c]}));
    }
    LOG_INFO(Render_Recompiler, "VR specular occlusion: composite {:#x}: Lagarde on the shine",
             info.pgm_hash);
}

// The composite's view-depth rows (a buffer read at dwords 10, 11, 14, 15) and its full-size
// depth: the sample whose red the buffer's dword 14 multiplies, before the reflection's sample
// in its block (VrReflectionUpsample).
struct UpsampleIn {
    u32 buffer;
    u32 depth_image;
    IR::Value depth;
};
std::optional<UpsampleIn> UpsampleInputs(const IR::Program& program, const IR::Inst* sample) {
    for (const auto& [handle, used] : ReadDwords(program)) {
        if (!used.contains(10) || !used.contains(11) || !used.contains(14) || !used.contains(15)) {
            continue;
        }
        const auto factor = Factor(program, handle, 14);
        if (!factor || factor->IsImmediate()) {
            continue;
        }
        const IR::Inst* const red = factor->InstRecursive();
        if (red->GetOpcode() != IR::Opcode::CompositeExtractF32x4 || red->Arg(0).IsImmediate() ||
            !red->Arg(1).IsImmediate() || red->Arg(1).U32() != 0) {
            continue;
        }
        const IR::Inst* const depth_sample = red->Arg(0).InstRecursive();
        if (!IsSample(depth_sample->GetOpcode()) || !depth_sample->Arg(0).IsImmediate() ||
            red->GetParent() != sample->GetParent()) {
            continue;
        }
        bool before = false;
        for (const IR::Inst& inst : sample->GetParent()->Instructions()) {
            if (&inst == red) {
                before = true;
                break;
            }
            if (&inst == sample) {
                break;
            }
        }
        if (before) {
            return UpsampleIn{handle, depth_sample->Arg(0).U32(), *factor};
        }
    }
    return std::nullopt;
}

// The composite's surface colour: render target 0 is c * M per colour, c a sample's channel
// (the G-buffer's albedo; M the AO terms the shine shares). That sample, or null.
IR::Inst* SurfaceColourSample(IR::Program& program) {
    IR::Inst* found = nullptr;
    for (u32 c = 0; c < 3; ++c) {
        IR::Inst* write = nullptr;
        for (IR::Block* block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                if (inst.GetOpcode() == IR::Opcode::SetAttribute &&
                    inst.Arg(0).Attribute() == IR::Attribute::RenderTarget0 &&
                    inst.Arg(2).IsImmediate() && inst.Arg(2).U32() == c) {
                    write = &inst;
                }
            }
        }
        if (!write) {
            return nullptr;
        }
        // Channel c of an implicit-lod sample, the nearest one up the written value (breadth
        // first, a few steps: the colour is a factor of the product, the AO mask's samples lie
        // deeper inside M).
        IR::Inst* s = nullptr;
        std::vector<IR::Value> level{write->Arg(1)};
        for (u32 depth = 0; depth < 4 && !s && !level.empty(); ++depth) {
            std::vector<IR::Value> next;
            for (const IR::Value& v : level) {
                if (v.IsImmediate()) {
                    continue;
                }
                IR::Inst* const inst = v.InstRecursive();
                if (inst->GetOpcode() == IR::Opcode::CompositeExtractF32x4 &&
                    !inst->Arg(0).IsImmediate() && inst->Arg(1).IsImmediate() &&
                    inst->Arg(1).U32() == c &&
                    inst->Arg(0).InstRecursive()->GetOpcode() ==
                        IR::Opcode::ImageSampleImplicitLod) {
                    s = inst->Arg(0).InstRecursive();
                    break;
                }
                for (size_t k = 0; k < inst->NumArgs(); ++k) {
                    next.push_back(inst->Arg(k));
                }
            }
            level = std::move(next);
        }
        if (!s || (found && found != s)) {
            return nullptr;
        }
        found = s;
    }
    return found;
}

// The game's own read of `colour` moved right before `sample` when it comes later in the same
// block (a pure read: its uses all follow; no texture is read twice), with the pure steps between
// that build its arguments (its uv's construct): everything else it reads must come before
// `sample` or from another block (which then dominates this one: SSA). False otherwise (nothing
// moved).
bool MoveBefore(IR::Inst* colour, IR::Inst* sample) {
    IR::Block* const block = sample->GetParent();
    if (colour->GetParent() != block) {
        return false;
    }
    std::set<const IR::Inst*> before;
    for (const IR::Inst& inst : block->Instructions()) {
        if (&inst == sample) {
            break;
        }
        before.insert(&inst);
    }
    if (before.contains(colour)) {
        return true; // already before it
    }
    // The instructions to move, arguments first (a few pure levels).
    std::vector<IR::Inst*> order;
    std::set<const IR::Inst*> taken;
    const auto collect = [&](auto&& self, IR::Inst* inst, u32 depth) -> bool {
        for (size_t k = 0; k < inst->NumArgs(); ++k) {
            const IR::Value arg = inst->Arg(k);
            if (arg.IsImmediate()) {
                continue;
            }
            IR::Inst* const a = arg.InstRecursive();
            if (before.contains(a) || taken.contains(a) || a->GetParent() != block) {
                continue;
            }
            const std::string_view name = IR::NameOf(a->GetOpcode());
            const bool pure = name.starts_with("FP") || name.starts_with("Composite") ||
                              name.starts_with("BitCast") || name.starts_with("GetAttribute");
            if (depth >= 3 || !pure || a->GetOpcode() == IR::Opcode::Phi ||
                a->MayHaveSideEffects() || !self(self, a, depth + 1)) {
                return false;
            }
        }
        order.push_back(inst);
        taken.insert(inst);
        return true;
    };
    if (!collect(collect, colour, 0)) {
        return false;
    }
    auto& list = block->Instructions();
    for (IR::Inst* inst : order) {
        list.erase(list.iterator_to(*inst));
        list.insert(list.iterator_to(*sample), *inst);
    }
    return true;
}

// Blood on glossy floors (reflections.h Blood*): the surface colour's channels (at least 0), its
// red's reciprocal and how much it is blood, 0..1: red well above green and blue, and dark.
// Linear ramps (an FMA and a saturate each) and one reciprocal.
struct BloodIn {
    std::array<IR::F32, 3> rgb;
    IR::F32 inv_red;
    IR::F32 share;
};
BloodIn BloodShare(IR::IREmitter& ir, const IR::Value& a) {
    namespace R = Reflections;
    BloodIn in;
    for (u32 k = 0; k < 3; ++k) {
        in.rgb[k] = IR::F32{ir.FPMax(IR::F32{ir.CompositeExtract(a, k)}, ir.Imm32(0.0f))};
    }
    in.inv_red = IR::F32{ir.FPRecip(IR::F32{ir.FPMax(in.rgb[0], ir.Imm32(1e-4f))})};
    // chroma = 1 - max(g, b) / r; red = saturate((chroma - lo) / (hi - lo)), as one FMA on
    // max(g, b) / r. Grey and brown give exactly 0: their reflection stays bit for bit.
    constexpr float s = 1.0f / (R::BloodChromaHi - R::BloodChromaLo);
    const IR::F32 gb = ir.FPMul(IR::F32{ir.FPMax(in.rgb[1], in.rgb[2])}, in.inv_red);
    const IR::F32 red =
        ir.FPSaturate(ir.FPFma(gb, ir.Imm32(-s), ir.Imm32((1.0f - R::BloodChromaLo) * s)));
    // dark = saturate((hi - r) / (hi - lo)).
    constexpr float d = 1.0f / (R::BloodDarkHi - R::BloodDarkLo);
    const IR::F32 dark =
        ir.FPSaturate(ir.FPFma(in.rgb[0], ir.Imm32(-d), ir.Imm32(R::BloodDarkHi * d)));
    in.share = ir.FPMul(red, dark);
    return in;
}

void CompositePass(IR::Program& program, const RuntimeInfo& runtime_info) {
    auto& info = program.info;
    const auto skip = [&](const char* why) {
        LOG_INFO(Render_Recompiler, "VR reflections: composite {:#x} left as it is ({})",
                 info.pgm_hash, why);
    };
    const char* why = nullptr;
    IR::Inst* const sample = ReflectionSample(program, why);
    if (!sample) {
        return skip(why);
    }
    const auto uses = sample->Uses(); // the game's, before ours are added
    IR::Block* const block = sample->GetParent();
    // Blood on glossy floors (reflections.h Blood*; RuntimeInfo::VrBloodFloors): the game's own
    // read of the surface colour moved before the reflection's sample; where it is blood, that
    // sample's mip a little blurrier. (Before the emitter below takes its place after the
    // sample: a moved instruction may have been the one after it.)
    std::optional<BloodIn> blood;
    if ((runtime_info.vr_ambient_occlusion & RuntimeInfo::VrBloodFloors) != 0) {
        IR::Inst* const albedo = SurfaceColourSample(program);
        if (albedo && MoveBefore(albedo, sample)) {
            IR::IREmitter pre{*block, IR::Block::InstructionList::s_iterator_to(*sample)};
            blood = BloodShare(pre, IR::Value{albedo});
            sample->SetArg(2, pre.FPMin(pre.FPFma(blood->share, pre.Imm32(Tune::BloodLodAdd),
                                                  IR::F32{sample->Arg(2)}),
                                        pre.Imm32(6.0f)));
        }
        LOG_INFO(Render_Recompiler, "VR reflections: composite {:#x}: blood on glossy floors {}",
                 info.pgm_hash,
                 blood ? "seen through the blood"
                       : "left as it is (no surface colour before the reflection)");
    }
    IR::IREmitter ir{*block, std::next(IR::Block::InstructionList::s_iterator_to(*sample))};
    IR::Value c{sample};
    // Sharper wet streets (reflections.h UpsampleLodMax): the sample depth-aware at edges.
    if (const auto in = UpsampleInputs(program, sample)) {
        c = ir.VrReflectionUpsample(c, sample->Arg(1), IR::F32{in->depth},
                                    IR::F32{sample->Arg(2)});
        info.vr_reflection_upsample = {
            .on = true,
            .buffer = in->buffer,
            .depth_image = in->depth_image,
            .colour_image = sample->Arg(0).U32(),
        };
        LOG_INFO(Render_Recompiler,
                 "VR reflections: composite {:#x}: depth-aware upsample (buffer {}, depth {:#x})",
                 info.pgm_hash, in->buffer, in->depth_image);
    } else {
        LOG_INFO(Render_Recompiler,
                 "VR reflections: composite {:#x}: no depth rows, the plain bilinear sample",
                 info.pgm_hash);
    }
    std::array<IR::F32, 4> v;
    for (u32 k = 0; k < 4; ++k) {
        v[k] = IR::F32{ir.CompositeExtract(c, k)};
    }
    // Finite and weighted at all: otherwise no reflection (the scene's own stays).
    IR::U1 ok = ir.FPGreaterThan(v[3], ir.Imm32(Tune::MinWeight));
    for (const IR::F32& x : v) {
        // (The exponent's bits as integers: without SignedZeroInfNanPreserve a driver may fold
        // a float NaN or Inf test away.)
        const IR::U32 bits = ir.BitCast<IR::U32, IR::F32>(x);
        ok = ir.LogicalAnd(ok, ir.INotEqual(IR::U32{ir.BitwiseAnd(bits, ir.Imm32(0x7f800000u))},
                                            ir.Imm32(0x7f800000u)));
    }
    const IR::F32 inv = IR::F32{ir.FPDiv(ir.Imm32(1.0f), IR::F32{ir.FPMax(v[3], ir.Imm32(Tune::MinWeight))})};
    std::array<IR::Value, 4> result;
    // Seen through the blood: green and blue times 1 + share Tint (c / r - 1), c / r the blood's
    // own colour against its red (a crimson sheen, not a clear mirror).
    const IR::F32 tint_share =
        !blood ? ir.Imm32(0.0f)
        : Tune::BloodTint == 1.0f ? blood->share
                                  : IR::F32{ir.FPMul(blood->share, ir.Imm32(Tune::BloodTint))};
    for (u32 k = 0; k < 3; ++k) {
        IR::F32 colour = IR::F32{ir.FPClamp(IR::F32{ir.FPMul(v[k], inv)}, ir.Imm32(0.0f),
                                            ir.Imm32(Tune::MaxRadiance))};
        if (blood && k > 0) {
            const IR::F32 ratio = ir.FPSaturate(ir.FPMul(blood->rgb[k], blood->inv_red));
            colour = ir.FPMul(colour, ir.FPFma(tint_share, ir.FPSub(ratio, ir.Imm32(1.0f)),
                                               ir.Imm32(1.0f)));
        }
        result[k] = ir.Select(ok, colour, ir.Imm32(0.0f));
    }
    IR::F32 weight = ir.FPMul(v[3], ir.Imm32(Tune::Strength));
    if (blood) {
        weight = ir.FPMul(weight, ir.FPFma(blood->share, ir.Imm32(-Tune::BloodWeightCut),
                                           ir.Imm32(1.0f)));
    }
    result[3] = ir.Select(ok, IR::F32{ir.FPClamp(weight, ir.Imm32(0.0f), ir.Imm32(1.0f))},
                          ir.Imm32(0.0f));
    const IR::Value ours = ir.CompositeConstruct(result[0], result[1], result[2], result[3]);
    for (const IR::Use& use : uses) {
        use.user->SetArg(use.operand, ours);
    }
    LOG_INFO(Render_Recompiler, "VR reflections: composite {:#x}: the reflection counted once",
             info.pgm_hash);
}

// The ambient light's level (cinematic_look.h AmbientFloat): the ambient composite's first target
// times a float of the grade's buffer, which the rasterizer fills at every draw (1: the game's).
void AmbientLevelPass(IR::Program& program) {
    auto& info = program.info;
    std::array<IR::Inst*, 3> rt0{};
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::SetAttribute &&
                inst.Arg(0).Attribute() == IR::Attribute::RenderTarget0 &&
                inst.Arg(2).IsImmediate() && inst.Arg(2).U32() < 3) {
                rt0[inst.Arg(2).U32()] = &inst;
            }
        }
    }
    if (std::ranges::any_of(rt0, [](const IR::Inst* i) { return i == nullptr; })) {
        LOG_INFO(Render_Recompiler, "VR ambient level: composite {:#x} left as it is (no first target)",
                 info.pgm_hash);
        return;
    }
    auto found = std::ranges::find(info.buffers, BufferType::VrGrade, &BufferResource::buffer_type);
    if (found == info.buffers.end()) {
        info.buffers.push_back({
            .used_types = IR::Type::U32,
            .inline_cbuf = AmdGpu::Buffer::Placeholder(CinematicLook::BufferDwords * sizeof(u32)),
            .buffer_type = BufferType::VrGrade,
        });
        found = std::prev(info.buffers.end());
    }
    const u32 binding = static_cast<u32>(std::distance(info.buffers.begin(), found));
    for (IR::Inst* write : rt0) {
        IR::Block* const block = write->GetParent();
        IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(*write)};
        const IR::F32 level = ir.BitCast<IR::F32>(
            ir.ReadConstBuffer(ir.Imm32(binding), ir.Imm32(CinematicLook::AmbientFloat), IR::BufferInstInfo{}));
        write->SetArg(1, ir.FPMul(IR::F32{write->Arg(1)}, level));
    }
    LOG_INFO(Render_Recompiler, "VR ambient level: composite {:#x}: its first target times the level (buffer {})",
             info.pgm_hash, binding);
}

} // namespace

void ReflectionsPass(IR::Program& program, const RuntimeInfo& runtime_info) {
    if (program.info.stage != Stage::Fragment || program.blocks.empty()) {
        return;
    }
    const u8 flags = runtime_info.vr_ambient_occlusion;
    if ((flags & RuntimeInfo::VrReflections) != 0 &&
        Reflections::IsTraceProgram(program.info.pgm_hash)) {
        TracePass(program, runtime_info);
    }
    if ((flags & RuntimeInfo::VrSpecularOcclusion) != 0 &&
        Reflections::IsCompositeProgram(program.info.pgm_hash)) {
        SpecularOcclusionPass(program);
    }
    if ((flags & RuntimeInfo::VrReflectionComposite) != 0 &&
        Reflections::IsCompositeProgram(program.info.pgm_hash)) {
        CompositePass(program, runtime_info);
    }
    // (Last: the others match the first target's own shape.)
    if (Reflections::IsCompositeProgram(program.info.pgm_hash)) {
        AmbientLevelPass(program);
    }
}

} // namespace Shader::Optimization
