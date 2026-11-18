// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Deeper stones (parallax.h): in the game's three parallax programs (RuntimeInfo::VrParallax, set
// for their hashes only), recognized from IR:
//  - the layer count's float: a ConvertS32F32 whose result is compared by an SGreaterThan32 (the
//    loop's bound) and converted back to a float by a ConvertF32S32 that an FPRecip32 inverts
//    (the layer's step). Its operand is multiplied by StepScale (at most MaxLayers) right before;
//  - the walk: the step's FPMul32 users whose other operand is FPNeg32(X) (X: the offset of one
//    texture coordinate per unit of depth), each added to its coordinate's Phi (through its
//    bits), and the step's FPSub32 user taking it off the layer's Phi (whose start is 1.0);
//  - the height: the loop's image sample whose component the next layer is compared with
//    (FPOrdGreaterThan32), and the program's other sample of the same texture before its
//    render target 2 and 3 writes (the walk's final spot, the bump map's read);
//  - the fade: the FPOrdGreaterThanEqual32 of FadeEndLevel and the texture detail level (an
//    FPMax32), in the entry block.
// Deeper bumps scale each X; the random start moves the layer's and the coordinates' starts in
// the loop's preheader; the darker gaps scale the albedo and reflectance writes. Each part is
// left out, with a log line, when its shape is not found.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <optional>
#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/passes/bump_probe.h"
#include "shader_recompiler/ir/passes/parallax.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {

namespace {

namespace Tune = Parallax;

IR::Inst* InstOf(const IR::Value& value) {
    return value.IsImmediate() ? nullptr : value.InstRecursive();
}

IR::Inst* As(const IR::Value& value, IR::Opcode op) {
    IR::Inst* const inst = InstOf(value);
    return inst != nullptr && inst->GetOpcode() == op ? inst : nullptr;
}

// The Phi behind a coordinate's or layer's bits (BitCastF32U32 of the Phi).
IR::Inst* PhiOfBits(const IR::Value& value) {
    IR::Inst* const cast = As(value, IR::Opcode::BitCastF32U32);
    return cast != nullptr ? As(cast->Arg(0), IR::Opcode::Phi) : nullptr;
}

bool IsImageSample(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::ImageSampleImplicitLod:
    case IR::Opcode::ImageSampleExplicitLod:
    case IR::Opcode::ImageSampleDrefImplicitLod:
    case IR::Opcode::ImageSampleDrefExplicitLod:
    case IR::Opcode::ImageGradient:
        return true;
    default:
        return false;
    }
}

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

// x's exponent bits: a number (a driver may fold a float NaN test away).
IR::U1 IsNumber(IR::IREmitter& ir, const IR::F32& x) {
    const IR::U32 bits = ir.BitCast<IR::U32, IR::F32>(x);
    return ir.INotEqual(IR::U32{ir.BitwiseAnd(bits, ir.Imm32(0x7f800000u))},
                        ir.Imm32(0x7f800000u));
}

float EnvFloat(const char* name, float fallback, float lo, float hi) {
    const char* const env = std::getenv(name);
    return env != nullptr ? std::clamp(static_cast<float>(std::atof(env)), lo, hi) : fallback;
}

// Where to add instructions at a block's end: before a trailing ConditionRef.
IR::Block::iterator EndOf(IR::Block& block) {
    auto& list = block.Instructions();
    if (!list.empty() && list.back().GetOpcode() == IR::Opcode::ConditionRef) {
        return std::prev(list.end());
    }
    return list.end();
}

// Block dominance for the cheap parallax (iterative sets: a few dozen blocks).
class Dominance {
public:
    explicit Dominance(const IR::Program& program) : blocks{program.blocks} {
        const size_t n = blocks.size();
        for (size_t i = 0; i < n; ++i) {
            index[blocks[i]] = i;
        }
        dom.assign(n, std::vector<bool>(n, true));
        if (n == 0) {
            return;
        }
        dom[0].assign(n, false);
        dom[0][0] = true;
        for (bool changed = true; changed;) {
            changed = false;
            for (size_t i = 1; i < n; ++i) {
                std::vector<bool> d(n, true);
                bool any = false;
                for (const IR::Block* pred : blocks[i]->ImmPredecessors()) {
                    const auto it = index.find(pred);
                    if (it == index.end()) {
                        continue;
                    }
                    any = true;
                    for (size_t k = 0; k < n; ++k) {
                        d[k] = d[k] && dom[it->second][k];
                    }
                }
                if (!any) {
                    d.assign(n, false);
                }
                d[i] = true;
                if (d != dom[i]) {
                    dom[i] = std::move(d);
                    changed = true;
                }
            }
        }
    }
    // `a` comes before `b` on every path (its block dominates b's; first in a shared block).
    bool Before(const IR::Inst* a, const IR::Inst* b) const {
        const auto ia = index.find(a->GetParent());
        const auto ib = index.find(b->GetParent());
        if (ia == index.end() || ib == index.end() || !dom[ib->second][ia->second]) {
            return false;
        }
        if (ia->second != ib->second) {
            return true;
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

    bool Dominates(const IR::Block* a, const IR::Block* b) const {
        const auto ia = index.find(a);
        const auto ib = index.find(b);
        return ia != index.end() && ib != index.end() && dom[ib->second][ia->second];
    }
    // Whether `to` can run after `from` (following successors).
    static bool Reaches(const IR::Block* from, const IR::Block* to) {
        std::vector<const IR::Block*> stack{from};
        std::vector<const IR::Block*> seen;
        while (!stack.empty()) {
            const IR::Block* const b = stack.back();
            stack.pop_back();
            if (b == to) {
                return true;
            }
            if (std::ranges::find(seen, b) != seen.end()) {
                continue;
            }
            seen.push_back(b);
            for (const IR::Block* next : b->ImmSuccessors()) {
                stack.push_back(next);
            }
        }
        return false;
    }
    const IR::BlockList& List() const {
        return blocks;
    }

private:
    const IR::BlockList& blocks;
    std::unordered_map<const IR::Block*, size_t> index;
    std::vector<std::vector<bool>> dom;
};

// A constant-buffer float read with immediate operands (BitCastF32U32 of ReadConstBuffer), or a
// float immediate: a value that can be read again anywhere.
bool ConstLike(const IR::Value& v) {
    if (v.IsImmediate()) {
        return v.Type() == IR::Type::F32;
    }
    IR::Inst* const cast = As(v, IR::Opcode::BitCastF32U32);
    IR::Inst* const read = cast != nullptr ? As(cast->Arg(0), IR::Opcode::ReadConstBuffer) : nullptr;
    return read != nullptr && read->Arg(0).IsImmediate() && read->Arg(1).IsImmediate();
}

IR::F32 ReadAgain(IR::IREmitter& ir, const IR::Value& v) {
    if (v.IsImmediate()) {
        return IR::F32{v};
    }
    IR::Inst* const read = InstOf(InstOf(v)->Arg(0));
    return ir.BitCast<IR::F32, IR::U32>(
        ir.ReadConstBuffer(read->Arg(0), IR::U32{read->Arg(1)}, read->Flags<IR::BufferInstInfo>()));
}

// One texture coordinate as base * scale + offset (scale and offset constant-like), or the base.
struct Axis {
    IR::Inst* base{};
    IR::Value scale{};
    IR::Value offset{};
};

std::optional<Axis> AxisOf(const IR::Value& v) {
    IR::Inst* const inst = InstOf(v);
    if (inst == nullptr) {
        return std::nullopt;
    }
    const auto split = [](const IR::Value& a, const IR::Value& b,
                          const IR::Value& off) -> std::optional<Axis> {
        for (const auto& [x, sc] : {std::pair{a, b}, std::pair{b, a}}) {
            if (!ConstLike(sc) || ConstLike(x) || InstOf(x) == nullptr || !ConstLike(off)) {
                continue;
            }
            return Axis{InstOf(x), sc, off};
        }
        return std::nullopt;
    };
    if (inst->GetOpcode() == IR::Opcode::FPFma32) {
        return split(inst->Arg(0), inst->Arg(1), inst->Arg(2));
    }
    if (inst->GetOpcode() == IR::Opcode::FPAdd32) {
        for (u32 k = 0; k < 2; ++k) {
            if (IR::Inst* const m = As(inst->Arg(k), IR::Opcode::FPMul32)) {
                if (auto axis = split(m->Arg(0), m->Arg(1), inst->Arg(1 - k))) {
                    return axis;
                }
            }
        }
        return std::nullopt;
    }
    if (ConstLike(v)) {
        return std::nullopt;
    }
    return Axis{inst, IR::Value{}, IR::Value{}};
}

// The program's camera-minus-pixel vector: the three values one FPRecipSqrt32 normalizes (its
// operand a sum of their squares, as x * x or a select of it) that each add a constant-buffer
// float (the camera) to the rest, ordered by that float's dword.
std::optional<std::array<IR::Inst*, 3>> FindView(IR::Program& program) {
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::FPRecipSqrt32) {
                continue;
            }
            std::vector<IR::Inst*> squared;
            std::vector<IR::Value> stack{inst.Arg(0)};
            for (u32 guard = 0; !stack.empty() && guard < 16; ++guard) {
                const IR::Value v = stack.back();
                stack.pop_back();
                IR::Inst* const i = InstOf(v);
                if (i == nullptr) {
                    continue;
                }
                if (i->GetOpcode() == IR::Opcode::FPAdd32) {
                    stack.push_back(i->Arg(0));
                    stack.push_back(i->Arg(1));
                } else if (i->GetOpcode() == IR::Opcode::SelectF32) {
                    stack.push_back(i->Arg(2));
                } else if (i->GetOpcode() == IR::Opcode::FPMul32 && !i->Arg(0).IsImmediate() &&
                           InstOf(i->Arg(0)) == InstOf(i->Arg(1))) {
                    squared.push_back(InstOf(i->Arg(0)));
                }
            }
            if (squared.size() != 3) {
                continue;
            }
            std::array<std::pair<u32, IR::Inst*>, 3> parts{};
            bool ok = true;
            for (u32 c = 0; c < 3 && ok; ++c) {
                IR::Inst* const add = squared[c];
                ok = add->GetOpcode() == IR::Opcode::FPAdd32;
                u32 dword = ~0u;
                for (u32 k = 0; ok && k < 2; ++k) {
                    if (ConstLike(add->Arg(k)) && !add->Arg(k).IsImmediate() &&
                        !ConstLike(add->Arg(1 - k))) {
                        dword = InstOf(InstOf(add->Arg(k))->Arg(0))->Arg(1).U32();
                    }
                }
                ok = ok && dword != ~0u;
                parts[c] = {dword, add};
            }
            if (!ok) {
                continue;
            }
            std::ranges::sort(parts);
            if (parts[0].first == parts[1].first || parts[1].first == parts[2].first) {
                continue;
            }
            return std::array{parts[0].second, parts[1].second, parts[2].second};
        }
    }
    return std::nullopt;
}

// What CopyBefore may copy: arithmetic, constant reads and the stage's inputs (no phi, no memory
// or image access, nothing with an effect).
bool Copyable(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::FPAdd32:
    case IR::Opcode::FPSub32:
    case IR::Opcode::FPMul32:
    case IR::Opcode::FPFma32:
    case IR::Opcode::FPNeg32:
    case IR::Opcode::FPAbs32:
    case IR::Opcode::FPRecip32:
    case IR::Opcode::FPRecipSqrt32:
    case IR::Opcode::FPSqrt:
    case IR::Opcode::FPMax32:
    case IR::Opcode::FPMin32:
    case IR::Opcode::FPSaturate32:
    case IR::Opcode::FPOrdEqual32:
    case IR::Opcode::FPOrdGreaterThan32:
    case IR::Opcode::FPOrdLessThan32:
    case IR::Opcode::SelectF32:
    case IR::Opcode::LogicalOr:
    case IR::Opcode::LogicalAnd:
    case IR::Opcode::LogicalNot:
    case IR::Opcode::BitCastF32U32:
    case IR::Opcode::BitCastU32F32:
    case IR::Opcode::ReadConstBuffer:
    case IR::Opcode::GetAttribute:
    case IR::Opcode::GetAttributeU32:
        return true;
    default:
        return false;
    }
}

// `v` before `at`: itself when it already comes before it on every path, else a copy of its
// arithmetic placed right before `at` (2026-11-11: some programs compute their view vector after
// their first texture read). Empty when something on the way cannot be copied.
std::optional<IR::Value> CopyBefore(const Dominance& dom, const IR::Value& v, IR::Inst* at,
                                    std::unordered_map<const IR::Inst*, IR::Inst*>& copies,
                                    u32 depth) {
    if (v.IsImmediate()) {
        return v;
    }
    IR::Inst* const inst = v.InstRecursive();
    if (inst != at && dom.Before(inst, at)) {
        return IR::Value{inst};
    }
    if (const auto it = copies.find(inst); it != copies.end()) {
        return IR::Value{it->second};
    }
    if (depth > 48 || inst == at || !Copyable(inst->GetOpcode())) {
        return std::nullopt;
    }
    std::vector<IR::Value> args;
    for (size_t i = 0; i < inst->NumArgs(); ++i) {
        auto arg = CopyBefore(dom, inst->Arg(i), at, copies, depth + 1);
        if (!arg) {
            return std::nullopt;
        }
        args.push_back(*arg);
    }
    IR::Inst* const copy = &*at->GetParent()->PrependNewInst(
        IR::Block::InstructionList::s_iterator_to(*at), *inst);
    for (size_t i = 0; i < args.size(); ++i) {
        copy->SetArg(i, args[i]);
    }
    copies[inst] = copy;
    return IR::Value{copy};
}

// The image reads whose coordinates come from the base coordinates (parallax.h BumpGaps),
// found before ours exist, and every instruction on the way to them.
struct Dependents {
    std::unordered_set<const IR::Inst*> insts;
    std::vector<IR::Inst*> reads;
    const char* why{};
};

bool IsImageRead(IR::Opcode op) {
    return IsImageSample(op) || op == IR::Opcode::ImageGather ||
           op == IR::Opcode::ImageGatherDref || op == IR::Opcode::ImageQueryLod;
}

Dependents FindDependents(IR::Inst* bu, IR::Inst* bv) {
    Dependents d;
    std::vector<IR::Inst*> stack{bu, bv};
    d.insts.insert(bu);
    d.insts.insert(bv);
    while (!stack.empty()) {
        IR::Inst* const inst = stack.back();
        stack.pop_back();
        for (const IR::Use& use : inst->Uses()) {
            IR::Inst* const user = use.user;
            const IR::Opcode op = user->GetOpcode();
            if (op == IR::Opcode::Phi) {
                d.why = "base read by a phi";
                return d;
            }
            if (IsImageRead(op)) {
                if (use.operand != 1) {
                    d.why = "base not a read's coordinates";
                    return d;
                }
                if (std::ranges::find(d.reads, user) == d.reads.end()) {
                    d.reads.push_back(user);
                }
                continue;
            }
            if (d.insts.insert(user).second) {
                stack.push_back(user);
            }
        }
    }
    return d;
}

// What a moved read's coordinates may be made of (copied with the base moved): CopyBefore's,
// composites and the rounding a tiled coordinate takes.
bool Clonable(IR::Opcode op) {
    switch (op) {
    case IR::Opcode::CompositeConstructF32x2:
    case IR::Opcode::CompositeConstructF32x3:
    case IR::Opcode::CompositeConstructF32x4:
    case IR::Opcode::CompositeExtractF32x2:
    case IR::Opcode::CompositeExtractF32x3:
    case IR::Opcode::CompositeExtractF32x4:
    case IR::Opcode::FPFloor32:
    case IR::Opcode::FPFract32:
    case IR::Opcode::FPCeil32:
    case IR::Opcode::FPTrunc32:
    case IR::Opcode::FPDiv32:
    case IR::Opcode::FPClamp32:
        return true;
    default:
        return Copyable(op);
    }
}

bool CanClone(const IR::Value& v, const Dependents& d, const IR::Inst* bu, const IR::Inst* bv,
              std::unordered_set<const IR::Inst*>& seen, u32 depth) {
    if (v.IsImmediate()) {
        return true;
    }
    const IR::Inst* const inst = v.InstRecursive();
    if (inst == bu || inst == bv || !d.insts.contains(inst) || seen.contains(inst)) {
        return true;
    }
    if (depth > 48 || !Clonable(inst->GetOpcode())) {
        return false;
    }
    seen.insert(inst);
    for (size_t i = 0; i < inst->NumArgs(); ++i) {
        if (!CanClone(inst->Arg(i), d, bu, bv, seen, depth + 1)) {
            return false;
        }
    }
    return true;
}

// `v` with the base coordinates moved: its arithmetic from them copied before `at`.
IR::Value CloneMoved(const IR::Value& v, IR::Inst* at, const Dependents& d, const IR::Inst* bu,
                     const IR::Inst* bv, const IR::Value& mu, const IR::Value& mv,
                     std::unordered_map<const IR::Inst*, IR::Inst*>& memo) {
    if (v.IsImmediate()) {
        return v;
    }
    IR::Inst* const inst = v.InstRecursive();
    if (inst == bu) {
        return mu;
    }
    if (inst == bv) {
        return mv;
    }
    if (!d.insts.contains(inst)) {
        return v;
    }
    if (const auto it = memo.find(inst); it != memo.end()) {
        return IR::Value{it->second};
    }
    std::vector<IR::Value> args;
    for (size_t i = 0; i < inst->NumArgs(); ++i) {
        args.push_back(CloneMoved(inst->Arg(i), at, d, bu, bv, mu, mv, memo));
    }
    IR::Inst* const copy = &*at->GetParent()->PrependNewInst(
        IR::Block::InstructionList::s_iterator_to(*at), *inst);
    for (size_t i = 0; i < args.size(); ++i) {
        copy->SetArg(i, args[i]);
    }
    memo[inst] = copy;
    return IR::Value{copy};
}

// Every read of the material at the moved spot (parallax.h BumpGaps): its coordinates copied with
// the base moved. A 2D implicit-detail read becomes VrBumpSample: for a draw that moves, a
// gradient read there with its own unmoved coordinates' change across the pixel (the joints keep
// their detail level); for every other draw the game's own read, untouched.
void MoveReads(const Dependents& d, const IR::Inst* bu, const IR::Inst* bv, const IR::Value& mu,
               const IR::Value& mv) {
    for (IR::Inst* const read : d.reads) {
        std::unordered_map<const IR::Inst*, IR::Inst*> memo;
        const IR::Value original = read->Arg(1);
        const IR::Value moved = CloneMoved(original, read, d, bu, bv, mu, mv, memo);
        IR::Inst* const coords = InstOf(original);
        const bool two_d = coords != nullptr &&
                           coords->GetOpcode() == IR::Opcode::CompositeConstructF32x2;
        if (read->GetOpcode() != IR::Opcode::ImageSampleImplicitLod || !two_d) {
            read->SetArg(1, moved);
            continue;
        }
        // In place (the caller keeps using the bump map's read): handle, coordinates, bias,
        // offset becomes handle, coordinates, moved coordinates, bias, offset.
        const IR::Value bias = read->Arg(2);
        const IR::Value offset = read->Arg(3);
        read->SetArg(2, IR::Value{});
        read->SetArg(3, IR::Value{});
        read->ReplaceOpcode(IR::Opcode::VrBumpSample);
        read->SetArg(2, moved);
        read->SetArg(3, bias);
        read->SetArg(4, offset);
    }
}

// Parallax occlusion (parallax.h BumpGaps): `why` null when done, else why not; the effect's
// fade at the pixel (VrBumpSeed's), for the darker joints to reuse.
struct ParallaxResult {
    const char* why{};
    IR::Value near{};
    std::vector<IR::Inst*> reads{}; // every read made from the layer's coordinates
};

// `writes`: the albedo's and reflectance's writes (BumpHeightPass's): the march goes where every
// pixel that writes them passes, else nowhere (its screen derivatives are undefined in a branch some
// pixels of a quad skip).
// `others`: a layered material's other bump maps. Each layer has its own coordinates in the
// game's two-layer ground materials (cobbles under dirt: Param4's first and second pair), so
// moving this layer's reads leaves the other layer where it lies; a program whose layers share
// their coordinates is left alone (one layer's height would drag the other's colour).
ParallaxResult OffsetMap(IR::Program& program, IR::Inst* bump, std::span<IR::Inst* const> others,
                         std::span<IR::Inst* const> writes) {
    IR::Inst* const coords = As(bump->Arg(1), IR::Opcode::CompositeConstructF32x2);
    if (coords == nullptr) {
        return {"coordinates"};
    }
    const auto au = AxisOf(coords->Arg(0));
    const auto av = AxisOf(coords->Arg(1));
    if (!au || !av || au->base == av->base) {
        return {"coordinates' form"};
    }
    const auto view = FindView(program);
    if (!view) {
        return {"no view vector"};
    }
    const Dominance dom{program};
    // Ours go before the first read of the base coordinates, which must come before all the
    // others.
    std::vector<IR::Inst*> users;
    for (IR::Inst* base : {au->base, av->base}) {
        for (const IR::Use& use : base->Uses()) {
            if (use.user->GetOpcode() == IR::Opcode::Phi) {
                return {"base read by a phi"};
            }
            users.push_back(use.user);
        }
    }
    IR::Inst* first = nullptr;
    for (IR::Inst* const u : users) {
        if (std::ranges::all_of(users, [&](IR::Inst* w) { return w == u || dom.Before(u, w); })) {
            first = u;
            break;
        }
    }
    if (first == nullptr) {
        return {"no first read"};
    }
    // Where every pixel that writes the material passes (2026-11-13 night, the hunter's coat drawn
    // as flat black, green or magenta shapes in some frames: its near-camera dither skips the
    // material's reads in every other pixel, so a quad's derivatives there are undefined and the
    // march's move was any number, every read then landing on one texel). The joints still darken.
    for (IR::Inst* const w : writes) {
        if (w != nullptr && !dom.Before(first, w)) {
            return {"its coordinates first read in a branch some pixels skip"};
        }
    }
    // Every read the coordinates reach must be movable on its own; else nothing moves (until
    // 2026-11-12 such a program slid its coordinates whole by one step: the look the user turned
    // down, on nobody knew which materials).
    const Dependents dependents = FindDependents(au->base, av->base);
    if (dependents.why != nullptr) {
        return {dependents.why};
    }
    if (dependents.reads.empty()) {
        return {"no reads"};
    }
    for (IR::Inst* const other : others) {
        if (std::ranges::find(dependents.reads, other) != dependents.reads.end()) {
            return {"its layers share their coordinates"};
        }
    }
    {
        std::unordered_set<const IR::Inst*> seen;
        for (IR::Inst* const read : dependents.reads) {
            if (!CanClone(read->Arg(1), dependents, au->base, av->base, seen, 0)) {
                return {"a read's coordinates cannot be copied"};
            }
        }
    }
    // Each base before the first read: itself, else its arithmetic copied there (2026-11-12: a
    // program bit-casting u before computing v was skipped though nothing depends on the order).
    std::unordered_map<const IR::Inst*, IR::Inst*> copies;
    std::array<IR::Value, 2> bases;
    for (u32 k = 0; k < 2; ++k) {
        IR::Inst* const need = k == 0 ? au->base : av->base;
        if (need != first && dom.Before(need, first)) {
            bases[k] = IR::Value{need};
            continue;
        }
        const auto copy = CopyBefore(dom, IR::Value{need}, first, copies, 0);
        if (!copy) {
            return {"coordinates after their first read"};
        }
        bases[k] = *copy;
    }
    // The view vector: the program's own when it comes first, else its arithmetic copied there.
    std::array<IR::Value, 3> eye_parts;
    for (u32 c = 0; c < 3; ++c) {
        const auto part = CopyBefore(dom, IR::Value{(*view)[c]}, first, copies, 0);
        if (!part) {
            return {"view vector not copyable"};
        }
        eye_parts[c] = *part;
    }
    IR::IREmitter ir{*first->GetParent(), IR::Block::InstructionList::s_iterator_to(*first)};
    const auto coordinate = [&](const Axis& a, const IR::Value& base_value) {
        const IR::F32 base{base_value};
        if (a.scale.IsEmpty()) {
            return base;
        }
        return IR::F32{ir.FPFma(base, ReadAgain(ir, a.scale), ReadAgain(ir, a.offset))};
    };
    const IR::Value at = ir.CompositeConstruct(coordinate(*au, bases[0]), coordinate(*av, bases[1]));
    // The bump map's blue here and the fade (read only for a draw whose blue is a height map).
    const IR::U32 handle = ir.Imm32(bump->Arg(0).U32());
    const IR::Value seed = ir.VrBumpSeed(handle, at);
    const IR::F32 height{ir.CompositeExtract(seed, 0)};
    const IR::F32 near{ir.CompositeExtract(seed, 1)};
    const IR::Value eye = ir.CompositeConstruct(IR::F32{eye_parts[0]}, IR::F32{eye_parts[1]},
                                                IR::F32{eye_parts[2]});
    const IR::Value base_uv = ir.CompositeConstruct(IR::F32{bases[0]}, IR::F32{bases[1]});
    // From the surface (the faces) down the draw's depth, faded like the darker joints.
    const IR::Value move = ir.VrBumpMarch(handle, eye, at, base_uv, height, near);
    const IR::F32 mu{ir.FPAdd(IR::F32{bases[0]}, IR::F32{ir.CompositeExtract(move, 0)})};
    const IR::F32 mv{ir.FPAdd(IR::F32{bases[1]}, IR::F32{ir.CompositeExtract(move, 1)})};
    MoveReads(dependents, au->base, av->base, mu, mv);
    return {nullptr, IR::Value{near}, dependents.reads};
}

// A layered material's darker joints (the user's first street pictures, 2026-11-12: darkened at
// the albedo write, the cobbles' joints showed through the dirt lying over them). The layer's own
// colour read takes them instead, before the game blends its layers: where the dirt covers the
// cobbles nothing of them shows, darker or not. The colour read: one of the layer's reads whose
// red, green and blue each go into the albedo write of their own channel and of no other (a mask
// or a gloss goes into all three, or none). False: none found, nothing darkened.
bool DarkenLayer(const Dominance& dom, const IR::Inst* bump, std::span<IR::Inst* const> reads,
                 const std::array<IR::Inst*, 6>& writes, const IR::F32& keep) {
    IR::Inst* const keep_inst = InstOf(keep);
    if (keep_inst == nullptr) {
        return false;
    }
    std::array<std::unordered_set<const IR::Inst*>, 3> feeds;
    for (u32 c = 0; c < 3; ++c) {
        std::vector<const IR::Inst*> stack;
        if (const IR::Inst* const first = InstOf(writes[c]->Arg(1))) {
            stack.push_back(first);
        }
        while (!stack.empty()) {
            const IR::Inst* const inst = stack.back();
            stack.pop_back();
            if (!feeds[c].insert(inst).second) {
                continue;
            }
            for (size_t k = 0; k < inst->NumArgs(); ++k) {
                if (const IR::Inst* const arg = InstOf(inst->Arg(k))) {
                    stack.push_back(arg);
                }
            }
        }
    }
    bool done = false;
    for (IR::Inst* const read : reads) {
        if (read == bump) {
            continue;
        }
        std::array<IR::Inst*, 3> channel{};
        bool plain = true;
        for (const IR::Use& use : read->Uses()) {
            IR::Inst* const user = use.user;
            if (user->GetOpcode() != IR::Opcode::CompositeExtractF32x4 ||
                !user->Arg(1).IsImmediate()) {
                plain = false;
                break;
            }
            const u32 k = user->Arg(1).U32();
            if (k < 3) {
                plain = plain && channel[k] == nullptr;
                channel[k] = user;
            }
        }
        if (!plain || channel[0] == nullptr || channel[1] == nullptr || channel[2] == nullptr) {
            continue;
        }
        bool colour = true;
        for (u32 k = 0; k < 3; ++k) {
            for (u32 c = 0; c < 3; ++c) {
                colour = colour && feeds[c].contains(channel[k]) == (c == k);
            }
        }
        if (!colour || !std::ranges::all_of(channel, [&](const IR::Inst* e) {
                return dom.Before(keep_inst, e);
            })) {
            continue;
        }
        for (IR::Inst* const e : channel) {
            std::vector<std::pair<IR::Inst*, u32>> uses;
            for (const IR::Use& use : e->Uses()) {
                uses.emplace_back(use.user, use.operand);
            }
            IR::IREmitter at{*e->GetParent(),
                             std::next(IR::Block::InstructionList::s_iterator_to(*e))};
            const IR::F32 dark{at.FPMul(IR::F32{IR::Value{e}}, keep)};
            for (const auto& [user, operand] : uses) {
                user->SetArg(operand, dark);
            }
        }
        done = true;
    }
    return done;
}

struct Coord {
    IR::Inst* x;   // the offset per unit of depth (an FPMul32)
    IR::Inst* phi; // the coordinate's Phi (bits)
};

} // namespace

void ParallaxPass(IR::Program& program, const RuntimeInfo& runtime_info) {
    auto& info = program.info;
    if (info.stage != Stage::Fragment || program.blocks.empty() ||
        (runtime_info.vr_composite_light & RuntimeInfo::VrParallax) == 0 ||
        !Parallax::IsParallaxProgram(info.pgm_hash)) {
        return;
    }
    static const float depth_scale = EnvFloat("VR_PARALLAX_DEPTH", Tune::DepthScale, 0.5f, 3.0f);
    static const float cavity = EnvFloat("VR_PARALLAX_CAVITY", Tune::CavityStrength, 0.0f, 1.0f);
    static const bool jitter = [] {
        const char* const env = std::getenv("VR_PARALLAX_JITTER");
        return env != nullptr ? env[0] != '0' : Tune::Jitter;
    }();

    // ---- The layer count ----
    u32 scaled = 0;
    IR::Inst* step = nullptr;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::ConvertS32F32 || inst.Arg(0).IsImmediate()) {
                continue;
            }
            bool bounds_loop = false;
            IR::Inst* recip = nullptr;
            for (const IR::Use& use : inst.Uses()) {
                const IR::Opcode op = use.user->GetOpcode();
                if (op == IR::Opcode::SGreaterThan32) {
                    bounds_loop = true;
                } else if (op == IR::Opcode::ConvertF32S32) {
                    for (const IR::Use& u : use.user->Uses()) {
                        if (u.user->GetOpcode() == IR::Opcode::FPRecip32) {
                            recip = u.user;
                        }
                    }
                }
            }
            if (!bounds_loop || recip == nullptr) {
                continue;
            }
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            const IR::F32 layers{inst.Arg(0)};
            const IR::F32 more{ir.FPMin(ir.FPMul(layers, ir.Imm32(Parallax::StepScale)),
                                        ir.Imm32(Parallax::MaxLayers))};
            inst.SetArg(0, more);
            step = recip;
            ++scaled;
        }
    }
    if (scaled != 1) {
        LOG_INFO(Render_Recompiler, "VR deeper stones: shader {:#x}: {} layer counts scaled{}",
                 info.pgm_hash, scaled, scaled == 0 ? " (left as it is)" : "");
        return;
    }

    // ---- The walk: its coordinates' offsets and Phis, the layer's Phi and next layer ----
    std::vector<Coord> coords;
    IR::Inst* layer_phi = nullptr;
    IR::Inst* next_layer = nullptr;
    for (const IR::Use& use : step->Uses()) {
        IR::Inst* const user = use.user;
        if (user->GetOpcode() == IR::Opcode::FPMul32) {
            IR::Inst* const neg = As(user->Arg(use.operand == 0 ? 1 : 0), IR::Opcode::FPNeg32);
            IR::Inst* const x = neg != nullptr ? As(neg->Arg(0), IR::Opcode::FPMul32) : nullptr;
            if (x == nullptr) {
                continue;
            }
            for (const IR::Use& u : user->Uses()) {
                if (u.user->GetOpcode() != IR::Opcode::FPAdd32) {
                    continue;
                }
                if (IR::Inst* const phi = PhiOfBits(u.user->Arg(u.operand == 0 ? 1 : 0))) {
                    coords.push_back({x, phi});
                }
            }
        } else if (user->GetOpcode() == IR::Opcode::FPSub32 && use.operand == 1) {
            if (IR::Inst* const phi = PhiOfBits(user->Arg(0))) {
                layer_phi = phi;
                next_layer = user;
            }
        }
    }

    // ---- Deeper bumps ----
    u32 deeper = 0;
    if (depth_scale != 1.0f) {
        std::vector<IR::Inst*> done;
        for (const Coord& c : coords) {
            if (std::ranges::find(done, c.x) != done.end()) {
                continue;
            }
            IR::IREmitter ir{*c.x->GetParent(), IR::Block::InstructionList::s_iterator_to(*c.x)};
            c.x->SetArg(0, ir.FPMul(IR::F32{c.x->Arg(0)}, ir.Imm32(depth_scale)));
            done.push_back(c.x);
            ++deeper;
        }
    }

    // ---- The random start (in the preheader: the block the layer's 1.0 comes from) ----
    bool started = false;
    if (jitter && layer_phi != nullptr && !coords.empty()) {
        IR::Block* pre = nullptr;
        size_t layer_arg = 0;
        for (size_t i = 0; i < layer_phi->NumArgs(); ++i) {
            const IR::Value v = layer_phi->Arg(i);
            if (v.IsImmediate() && v.Type() == IR::Type::U32 && v.U32() == 0x3f800000u) {
                pre = layer_phi->PhiBlock(i);
                layer_arg = i;
            }
        }
        std::vector<std::pair<const Coord*, size_t>> starts;
        for (const Coord& c : coords) {
            for (size_t i = 0; pre != nullptr && i < c.phi->NumArgs(); ++i) {
                if (c.phi->PhiBlock(i) == pre) {
                    starts.emplace_back(&c, i);
                }
            }
        }
        if (pre != nullptr && starts.size() == coords.size() &&
            step->GetParent() == pre) {
            IR::IREmitter ir{*pre, EndOf(*pre)};
            // A hash of the first two coordinates' start bits: per pixel, and per frame through
            // the upscaler's sub-pixel jitter (it moves the interpolated coordinates).
            const IR::U32 a{starts[0].first->phi->Arg(starts[0].second)};
            const IR::U32 b{starts[std::min<size_t>(1, starts.size() - 1)].first->phi->Arg(
                starts[std::min<size_t>(1, starts.size() - 1)].second)};
            IR::U32 h{ir.BitwiseXor(IR::U32{ir.IMul(a, ir.Imm32(0x9e3779b1u))},
                                    IR::U32{ir.IMul(b, ir.Imm32(0x85ebca77u))})};
            h = ir.BitwiseXor(h, IR::U32{ir.ShiftRightLogical(h, ir.Imm32(16u))});
            h = IR::U32{ir.IMul(h, ir.Imm32(0x7feb352du))};
            h = ir.BitwiseXor(h, IR::U32{ir.ShiftRightLogical(h, ir.Imm32(15u))});
            const IR::F32 j{ir.FPMul(
                IR::F32{ir.ConvertUToF(32, 32, ir.ShiftRightLogical(h, ir.Imm32(8u)))},
                ir.Imm32(1.0f / 16777216.0f))};
            const IR::F32 shift = ir.FPMul(j, IR::F32{IR::Value{step}});
            // Lower by `shift`: the layer from 1 - shift, each coordinate from its start - shift X.
            layer_phi->SetArg(layer_arg,
                              ir.BitCast<IR::U32, IR::F32>(ir.FPSub(ir.Imm32(1.0f), shift)));
            for (const auto& [c, i] : starts) {
                const IR::F32 from = ir.BitCast<IR::F32, IR::U32>(IR::U32{c->phi->Arg(i)});
                const IR::F32 moved{ir.FPFma(ir.FPNeg(shift), IR::F32{IR::Value{c->x}}, from)};
                c->phi->SetArg(i, ir.BitCast<IR::U32, IR::F32>(moved));
            }
            started = true;
        }
    }

    // ---- Darker gaps ----
    bool darkened = false;
    const char* cavity_why = "off";
    if (cavity > 0.0f) {
        cavity_why = "no height compare";
        // The loop's height: the next layer compared with a component of an image sample.
        IR::Inst* loop_sample = nullptr;
        u32 channel = 0;
        for (const IR::Use& use : next_layer != nullptr ? next_layer->Uses() : step->Uses()) {
            if (next_layer == nullptr || use.user->GetOpcode() != IR::Opcode::FPOrdGreaterThan32 ||
                use.operand != 1) {
                continue;
            }
            IR::Inst* const extract = As(use.user->Arg(0), IR::Opcode::CompositeExtractF32x4);
            IR::Inst* const sample = extract != nullptr ? InstOf(extract->Arg(0)) : nullptr;
            if (sample != nullptr && IsImageSample(sample->GetOpcode()) &&
                sample->Arg(0).IsImmediate() && sample->Arg(0).Type() == IR::Type::U32 &&
                extract->Arg(1).IsImmediate() && extract->Arg(1).Type() == IR::Type::U32) {
                loop_sample = sample;
                channel = extract->Arg(1).U32();
            }
        }
        // The fade: FadeEndLevel >= the detail level, in the entry block.
        const IR::Block* const entry = program.blocks.front();
        IR::Inst* level = nullptr;
        for (IR::Inst& inst : program.blocks.front()->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::FPOrdGreaterThanEqual32 &&
                inst.Arg(0).IsImmediate() && inst.Arg(0).Type() == IR::Type::F32 &&
                inst.Arg(0).F32() == Tune::FadeEndLevel &&
                As(inst.Arg(1), IR::Opcode::FPMax32) != nullptr) {
                level = InstOf(inst.Arg(1));
            }
        }
        std::array<IR::Inst*, 6> writes{};
        bool all_writes = true;
        for (u32 c = 0; c < 3; ++c) {
            writes[c] = LastWrite(program, IR::Attribute::RenderTarget2, c);
            writes[3 + c] = LastWrite(program, IR::Attribute::RenderTarget3, c);
        }
        for (IR::Inst* w : writes) {
            all_writes = all_writes && w != nullptr && w->GetParent() == writes[0]->GetParent();
        }
        // The final spot's sample of the same texture, before the writes.
        IR::Inst* final_sample = nullptr;
        if (loop_sample != nullptr && level != nullptr && all_writes) {
            cavity_why = "no final sample";
            for (IR::Inst& inst : writes[0]->GetParent()->Instructions()) {
                if (&inst != loop_sample && IsImageSample(inst.GetOpcode()) &&
                    inst.Arg(0).IsImmediate() && inst.Arg(0).Type() == IR::Type::U32 &&
                    inst.Arg(0).U32() == loop_sample->Arg(0).U32()) {
                    final_sample = &inst;
                    break;
                }
            }
        } else if (loop_sample != nullptr) {
            cavity_why = level == nullptr ? "no fade level" : "render targets 2, 3";
        }
        if (final_sample != nullptr &&
            std::ranges::all_of(writes, [&](IR::Inst* w) {
                return Dominates(final_sample, w, entry) && Dominates(level, w, entry);
            })) {
            // Before the first of the writes in their block.
            IR::Inst* first = nullptr;
            for (IR::Inst& inst : writes[0]->GetParent()->Instructions()) {
                if (std::ranges::find(writes, &inst) != writes.end()) {
                    first = &inst;
                    break;
                }
            }
            IR::IREmitter ir{*first->GetParent(), IR::Block::InstructionList::s_iterator_to(*first)};
            const IR::F32 height{ir.CompositeExtract(IR::Value{final_sample}, channel)};
            const IR::F32 depth = ir.FPSub(ir.Imm32(1.0f), IR::F32{ir.FPSaturate(height)});
            const IR::F32 lod{IR::Value{level}};
            const IR::F32 walk{ir.Select(
                IsNumber(ir, lod),
                IR::F32{ir.FPSaturate(ir.FPSub(ir.Imm32(Tune::FadeEndLevel), lod))},
                ir.Imm32(0.0f))};
            const IR::F32 keep{ir.FPSaturate(IR::F32{ir.FPFma(
                ir.FPMul(walk, depth), ir.Imm32(-cavity), ir.Imm32(1.0f))})};
            const IR::F32 keep_f0 = ir.FPSqrt(keep);
            for (u32 c = 0; c < 6; ++c) {
                IR::Inst* const w = writes[c];
                IR::IREmitter at{*w->GetParent(), IR::Block::InstructionList::s_iterator_to(*w)};
                w->SetArg(1, at.FPMul(IR::F32{w->Arg(1)}, c < 3 ? keep : keep_f0));
            }
            darkened = true;
        } else if (final_sample != nullptr) {
            cavity_why = "order";
        }
    }

    LOG_INFO(Render_Recompiler,
             "VR deeper stones: shader {:#x}: layers x{}, {} coordinate(s), depth x{} ({}), random "
             "start {}, darker gaps {}",
             info.pgm_hash, Parallax::StepScale, coords.size(), depth_scale, deeper,
             started ? "on" : (jitter ? "not found" : "off"),
             darkened ? "on" : cavity_why);
}


// Raised brickwork (parallax.h BumpGaps), in a G-buffer program (RuntimeInfo::VrBumpGaps): the bump
// map is the one sample whose red, green and alpha are each decoded as fma(2, s, -1), its blue
// read nowhere; the albedo and reflectance writes (render targets 2 and 3) come after it (same
// block, or it in the entry). A program with two such samples is a layered material: left as it
// is. Tests: VR_BUMP_HEIGHT_DEBUG=1 (any G-buffer program, with or without the setting) paints
// the blue as the albedo instead; VR_BUMP_CLASS_DEBUG=1 the draw's class.
void BumpHeightPass(IR::Program& program, const RuntimeInfo& runtime_info) {
    namespace Gaps = BumpGaps;
    static const bool debug = [] {
        const char* const env = std::getenv("VR_BUMP_HEIGHT_DEBUG");
        return env != nullptr && env[0] == '1';
    }();
    auto& info = program.info;
    const bool on = (runtime_info.vr_composite_light & RuntimeInfo::VrBumpGaps) != 0;
    // (The map's programs only, whatever the key says: parallax.h BumpGaps::MapPrograms.)
    if ((!debug && !on) || info.stage != Stage::Fragment || program.blocks.empty() ||
        !Gaps::IsMapProgram(info.pgm_hash) ||
        LastWrite(program, IR::Attribute::RenderTarget5, 0) == nullptr) {
        return;
    }
    std::array<IR::Inst*, 6> writes{};
    for (u32 c = 0; c < 3; ++c) {
        writes[c] = LastWrite(program, IR::Attribute::RenderTarget2, c);
        writes[3 + c] = LastWrite(program, IR::Attribute::RenderTarget3, c);
        if (writes[c] == nullptr) {
            return;
        }
    }
    const bool f0 = std::ranges::all_of(writes.begin() + 3, writes.end(),
                                        [](IR::Inst* w) { return w != nullptr; });
    struct Decoded {
        IR::Inst* sample;
        std::array<bool, 4> channels;
    };
    std::vector<Decoded> decodes;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::FPFma32 || !inst.Arg(2).IsImmediate() ||
                inst.Arg(2).Type() != IR::Type::F32 || inst.Arg(2).F32() != -1.0f) {
                continue;
            }
            for (u32 a = 0; a < 2; ++a) {
                const IR::Value two = inst.Arg(a);
                IR::Inst* const e = As(inst.Arg(1 - a), IR::Opcode::CompositeExtractF32x4);
                if (!two.IsImmediate() || two.Type() != IR::Type::F32 || two.F32() != 2.0f ||
                    e == nullptr || !e->Arg(1).IsImmediate()) {
                    continue;
                }
                IR::Inst* const sample = InstOf(e->Arg(0));
                if (sample == nullptr || !IsImageSample(sample->GetOpcode()) ||
                    !sample->Arg(0).IsImmediate()) {
                    continue;
                }
                if (e->Arg(1).U32() >= 4) {
                    continue;
                }
                auto it = std::ranges::find(decodes, sample, &Decoded::sample);
                if (it == decodes.end()) {
                    decodes.push_back({sample, {}});
                    it = std::prev(decodes.end());
                }
                it->channels[e->Arg(1).U32()] = true;
            }
        }
    }
    // The bump maps: the samples decoded in red, green and alpha. The first is ours; a layered
    // material has others.
    IR::Inst* bump = nullptr;
    std::vector<IR::Inst*> others;
    for (const Decoded& d : decodes) {
        if (d.channels[0] && d.channels[1] && d.channels[3]) {
            if (bump == nullptr) {
                bump = d.sample;
            } else {
                others.push_back(d.sample);
            }
        }
    }
    const auto skip = [&](const char* why) {
        LOG_INFO(Render_Recompiler, "VR raised bricks: shader {:#x} left as it is ({})",
                 info.pgm_hash, why);
    };
    if (bump == nullptr) {
        return skip("no red, green, alpha bump decode");
    }
    if (BumpProbe::Dir() != nullptr) {
        BumpProbe::Note(info.pgm_hash, bump->Arg(0).U32() & 0xffffu); // (the handle: image | sampler << 16)
    }
    // Its blue: read nowhere (free for us, as in the parallax materials).
    for (const IR::Use& use : bump->Uses()) {
        const IR::Inst* const u = use.user;
        if (u->GetOpcode() != IR::Opcode::CompositeExtractF32x4 || !u->Arg(1).IsImmediate() ||
            u->Arg(1).U32() == 2) {
            return skip("its blue is read");
        }
    }
    // The parallax first (the bump map's own read then takes the moved coordinates).
    ParallaxResult parallax{"off"};
    if (!debug) {
        parallax = OffsetMap(program, bump, others, writes);
    }
    const char* const parallax_why = parallax.why;
    // A layered material (dirt over cobbles, plaster over bricks) whose first layer cannot move
    // on its own: left as it is (darker joints alone would show through the layer over them).
    if (!others.empty() && parallax_why != nullptr && !debug) {
        LOG_INFO(Render_Recompiler, "VR raised bricks: shader {:#x}: a layered material, parallax {}",
                 info.pgm_hash, parallax_why);
        return skip("a layered material whose first layer cannot move alone");
    }
    // The program's bump map, for the per-draw class (VrPushData::bump_depth / bump_scale).
    const u32 bump_handle = bump->Arg(0).U32();
    if ((bump_handle & 0xffffu) < 0xffu) {
        info.vr_bump_image = static_cast<u8>(bump_handle & 0xffffu);
    }
    const Dominance dom{program};
    // Writes the bump's block does not come before on every path (the game's alpha test jumps
    // past the bump read for a cut-out texel): the darkening meets them through a phi where both
    // paths join, 1 (no darkening) on the path without it. That join: a block the bump's does
    // not dominate, before every write, each of whose predecessors either comes after the bump
    // (dominated by its block) or cannot follow it at all.
    IR::Block* const bump_block = bump->GetParent();
    IR::Block* merge = nullptr;
    if (!std::ranges::all_of(writes, [&](IR::Inst* w) { return w == nullptr || dom.Before(bump, w); })) {
        for (IR::Block* x : dom.List()) {
            if (dom.Dominates(bump_block, x) ||
                !std::ranges::all_of(writes, [&](IR::Inst* w) {
                    return w == nullptr || dom.Dominates(x, w->GetParent());
                })) {
                continue;
            }
            bool through = false, clean = true;
            for (const IR::Block* pred : x->ImmPredecessors()) {
                if (dom.Dominates(bump_block, pred)) {
                    through = true;
                } else if (Dominance::Reaches(bump_block, pred)) {
                    clean = false;
                }
            }
            if (through && clean) {
                merge = x;
                break;
            }
        }
        if (merge == nullptr) {
            LOG_INFO(Render_Recompiler, "VR raised bricks: shader {:#x}: parallax {}",
                     info.pgm_hash, parallax_why ? parallax_why : "on");
            return skip("order");
        }
    }
    IR::IREmitter ir{*bump->GetParent(),
                     std::next(IR::Block::InstructionList::s_iterator_to(*bump))};
    const IR::F32 height{ir.CompositeExtract(IR::Value{bump}, 2)};
    // Tests (VR_BUMP_CLASS_DEBUG=1): the albedo shows the draw's class (green: its blue is a
    // height map that moves; red: flat, grey, mask or unknown: left as the game draws it).
    static const bool class_debug = [] {
        const char* const env = std::getenv("VR_BUMP_CLASS_DEBUG");
        return env != nullptr && env[0] == '1';
    }();
    if (class_debug && merge == nullptr) {
        const IR::F32 on{ir.FPSaturate(ir.VrBumpDepth(ir.Imm32(-1.0f)))};
        writes[0]->SetArg(1, ir.FPSub(ir.Imm32(1.0f), on));
        writes[1]->SetArg(1, on);
        writes[2]->SetArg(1, ir.Imm32(0.0f));
        LOG_INFO(Render_Recompiler, "VR bump class test: shader {:#x}: albedo = the class",
                 info.pgm_hash);
        return;
    }
    if (debug) {
        if (merge != nullptr) {
            return skip("test views need the bump read first");
        }
        for (u32 c = 0; c < 3; ++c) {
            writes[c]->SetArg(1, height);
        }
        LOG_INFO(Render_Recompiler, "VR bump height test: shader {:#x}: albedo = the bump map's blue",
                 info.pgm_hash);
        return;
    }
    // The fade: the parallax's when it ran (the same texture, the unmoved spot), else read here.
    IR::F32 near{};
    if (parallax.why == nullptr) {
        near = IR::F32{parallax.near};
    } else {
        near = IR::F32{ir.CompositeExtract(
            ir.VrBumpSeed(ir.Imm32(bump->Arg(0).U32()), bump->Arg(1)), 1)};
    }
    // How deep the spot found lies below the faces, 0..1 (0 for a draw whose blue is no height
    // map: nothing darkens).
    const IR::F32 gap{
        ir.FPSaturate(ir.FPMul(ir.VrBumpDepth(height), ir.Imm32(Gaps::JointGain)))};
    const IR::F32 keep{
        ir.FPFma(ir.FPMul(near, gap), ir.Imm32(-Gaps::Strength), ir.Imm32(1.0f))};
    // Tests (VR_BUMP_GAPS_DEBUG=1): the albedo shows the fade (red), the joint's depth (green)
    // and the blue at the spot found (blue).
    static const bool gaps_debug = [] {
        const char* const env = std::getenv("VR_BUMP_GAPS_DEBUG");
        return env != nullptr && env[0] != '0';
    }();
    if (gaps_debug && merge == nullptr) {
        writes[0]->SetArg(1, near);
        writes[1]->SetArg(1, gap);
        writes[2]->SetArg(1, height);
        LOG_INFO(Render_Recompiler, "VR raised bricks test: shader {:#x}: albedo = fade, joint",
                 info.pgm_hash);
        return;
    }
    if (!others.empty()) {
        const bool layer = DarkenLayer(dom, bump, parallax.reads, writes, keep);
        LOG_INFO(Render_Recompiler,
                 "VR raised bricks: shader {:#x}: a layered material, its first layer moves; its "
                 "joints {}",
                 info.pgm_hash,
                 layer ? "darker in its own colour read" : "not darkened (no colour read found)");
        return;
    }
    IR::F32 keep_out = keep;
    IR::F32 keep_f0{};
    if (merge != nullptr) {
        // (Phis carry U1 or U32 here, as the game's own floats do: the bits, then back.)
        const IR::U32 keep_bits = ir.BitCast<IR::U32, IR::F32>(keep);
        IR::Inst* const phi = &*merge->PrependNewInst(merge->begin(), IR::Opcode::Phi);
        phi->SetFlags(IR::Type::U32);
        for (IR::Block* pred : merge->ImmPredecessors()) {
            phi->AddPhiOperand(pred, dom.Dominates(bump_block, pred) ? IR::Value{keep_bits}
                                                                     : IR::Value{0x3f800000u});
        }
        auto after = merge->begin();
        while (after != merge->end() && after->GetOpcode() == IR::Opcode::Phi) {
            ++after;
        }
        IR::IREmitter at{*merge, after};
        keep_out = at.BitCast<IR::F32, IR::U32>(IR::U32{IR::Value{phi}});
        keep_f0 = at.FPSqrt(keep_out);
    } else {
        keep_f0 = ir.FPSqrt(keep);
    }
    for (u32 c = 0; c < 6; ++c) {
        IR::Inst* const w = writes[c];
        if (w == nullptr) {
            continue;
        }
        IR::IREmitter at{*w->GetParent(), IR::Block::InstructionList::s_iterator_to(*w)};
        w->SetArg(1, at.FPMul(IR::F32{w->Arg(1)}, c < 3 ? keep_out : keep_f0));
    }
    LOG_INFO(Render_Recompiler,
             "VR raised bricks: shader {:#x}: albedo{} darker in the joints{}; parallax {}{}",
             info.pgm_hash, f0 ? " and reflectance" : "", merge != nullptr ? " (through a phi)" : "",
             parallax_why ? parallax_why : "on", others.empty() ? "" : " (its first layer)");
}

} // namespace Shader::Optimization
