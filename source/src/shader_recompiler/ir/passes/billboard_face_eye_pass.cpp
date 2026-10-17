// SPDX-License-Identifier: GPL-2.0-or-later

// Eye-facing sprites (on in native stereo; VR_BILLBOARD_FACE_EYE=0 off, =1 on everywhere): the
// game's camera-facing sprites face the eye instead of the view plane (billboard_face_eye.h), so
// turning your head no longer turns them. Recognized from IR only, never by shader hash, in two
// forms:
//  - a corner p_k = c_k + s_r right_k + s_u up_k (k = x, y, z) where right/up are read from the
//    camera block, the same s_r (s_u) scales all three components, and the sum is built by adds
//    and fmas alone;
//  - the tessellated particles (smoke, fire): p_k = sum_j a_kj s_j from a per-particle matrix read
//    from the patch, two s_j moving with the tessellation point, projected by VP rows from one
//    constant buffer (RewriteParticleAxes).
//  - the geometry-stage sprites (sparks, item glows): points expanded along the camera's right
//    and up read from the effect camera block (RewriteGeometrySprites).
// Anything else is left untouched and logged.

#include <algorithm>
#include <cstdlib>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/billboard_face_eye.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {
namespace {

using namespace BillboardFaceEye;

// The camera block (864 bytes, NativeStereo::CameraBuffer), in dwords: the world-to-view matrix's
// rows right (8..10) and up (12..14) with translations 11, 15, 19; the view-to-world matrix's
// columns right (180, 184, 188) and up (181, 185, 189) with the eye at 183, 187, 191.
struct Source {
    const char* name;
    u32 right[3], up[3], translation[3];
    bool eye_direct; // translation is the eye itself (view-to-world)
};
constexpr Source Sources[] = {
    {"view rows", {8, 9, 10}, {12, 13, 14}, {11, 15, 19}, false},
    {"inverse view columns", {180, 184, 188}, {181, 185, 189}, {183, 187, 191}, true},
};

// Scalar adapter: FaceEye's math emitted as IR. Constants carry no emitter.
struct F {
    IR::IREmitter* ir{};
    IR::F32 v;
    F(float x = 0.0f) : v{IR::Value{x}} {}
    F(IR::IREmitter* ir_, IR::F32 v_) : ir{ir_}, v{v_} {}
};
IR::IREmitter* Emitter(const F& a, const F& b) {
    return a.ir ? a.ir : b.ir;
}
// Two constants fold here: the emitter is only needed once an IR value is involved.
F operator+(const F& a, const F& b) {
    auto* ir = Emitter(a, b);
    if (!ir) {
        return F{a.v.F32() + b.v.F32()};
    }
    return {ir, IR::F32{ir->FPAdd(a.v, b.v)}};
}
F operator-(const F& a, const F& b) {
    auto* ir = Emitter(a, b);
    if (!ir) {
        return F{a.v.F32() - b.v.F32()};
    }
    return {ir, IR::F32{ir->FPSub(a.v, b.v)}};
}
F operator*(const F& a, const F& b) {
    auto* ir = Emitter(a, b);
    if (!ir) {
        return F{a.v.F32() * b.v.F32()};
    }
    return {ir, IR::F32{ir->FPMul(a.v, b.v)}};
}
F Step(const F& a) {
    if (!a.ir) {
        return F{a.v.F32() > 0.0f ? 1.0f : 0.0f};
    }
    const IR::F32 zero{IR::Value{0.0f}}, one{IR::Value{1.0f}};
    return {a.ir, IR::F32{a.ir->Select(a.ir->FPGreaterThan(a.v, zero), one, zero)}};
}
F RecipSqrt(const F& a) {
    return {a.ir, IR::F32{a.ir->FPRecipSqrt(a.v)}};
}
F Recip(const F& a) {
    return {a.ir, IR::F32{a.ir->FPRecip(a.v)}};
}
F Max(const F& a, const F& b) {
    auto* ir = Emitter(a, b);
    return {ir, IR::F32{ir->FPMax(a.v, b.v)}};
}

struct Read {
    u32 binding, offset;
    IR::Inst* inst;
};
std::optional<Read> CameraRead(IR::Value v) {
    v = v.Resolve();
    if (v.IsImmediate()) {
        return {};
    }
    IR::Inst* i = v.InstRecursive();
    if (i->GetOpcode() == IR::Opcode::BitCastF32U32) {
        v = i->Arg(0).Resolve();
        if (v.IsImmediate()) {
            return {};
        }
        i = v.InstRecursive();
    }
    if (i->GetOpcode() != IR::Opcode::ReadConstBuffer || !i->Arg(0).IsImmediate() ||
        !i->Arg(1).IsImmediate()) {
        return {};
    }
    return Read{i->Arg(0).U32(), i->Arg(1).U32(), i};
}

// s * axis_k, as an FPMul32 or the product of an FPFma32 (then the term includes its addend).
struct Term {
    IR::Inst* inst{};
    IR::Value axis, scale;
};
struct Axis {
    u32 binding;
    const Source* source;
    bool up;
    IR::Value scale;
    std::array<Term, 3> terms{};
    IR::Inst* read{};
    bool ambiguous{};
};

// Whether `target` is one of the summed terms of v (adds, fma addends, the minuend of a sub).
bool Contains(const IR::Value& value, const IR::Inst* target, u32 depth = 0) {
    const IR::Value v = value.Resolve();
    if (v.IsImmediate() || depth > 24) {
        return false;
    }
    const IR::Inst* i = v.InstRecursive();
    if (i == target) {
        return true;
    }
    switch (i->GetOpcode()) {
    case IR::Opcode::FPAdd32:
        return Contains(i->Arg(0), target, depth + 1) || Contains(i->Arg(1), target, depth + 1);
    case IR::Opcode::FPFma32:
        return Contains(i->Arg(2), target, depth + 1);
    case IR::Opcode::FPSub32:
        return Contains(i->Arg(0), target, depth + 1);
    default:
        return false;
    }
}

// The smallest sum holding both terms: p_k.
IR::Inst* CornerSum(IR::Program& program, const IR::Inst* a, const IR::Inst* b) {
    const auto both = [&](const IR::Value& v) { return Contains(v, a) && Contains(v, b); };
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& i : *block) {
            const auto op = i.GetOpcode();
            if (op != IR::Opcode::FPAdd32 && op != IR::Opcode::FPFma32 &&
                op != IR::Opcode::FPSub32) {
                continue;
            }
            if (!both(IR::Value{&i})) {
                continue;
            }
            const bool child = op == IR::Opcode::FPAdd32 ? both(i.Arg(0)) || both(i.Arg(1))
                               : op == IR::Opcode::FPFma32 ? both(i.Arg(2))
                                                           : both(i.Arg(0));
            if (!child) {
                return &i;
            }
        }
    }
    return nullptr;
}

std::vector<Axis> FindAxes(IR::Program& program) {
    std::vector<Axis> axes;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& i : *block) {
            if (i.GetOpcode() != IR::Opcode::FPMul32 && i.GetOpcode() != IR::Opcode::FPFma32) {
                continue;
            }
            for (u32 slot = 0; slot < 2; ++slot) {
                const auto read = CameraRead(i.Arg(slot));
                if (!read) {
                    continue;
                }
                for (const Source& source : Sources) {
                    for (u32 k = 0; k < 3; ++k) {
                        for (const bool up : {false, true}) {
                            if ((up ? source.up : source.right)[k] != read->offset) {
                                continue;
                            }
                            const IR::Value scale = i.Arg(1 - slot).Resolve();
                            auto it = std::ranges::find_if(axes, [&](const Axis& a) {
                                return a.binding == read->binding && a.source == &source &&
                                       a.up == up && a.scale == scale;
                            });
                            if (it == axes.end()) {
                                it = axes.insert(axes.end(),
                                                 Axis{read->binding, &source, up, scale});
                            }
                            it->ambiguous |= it->terms[k].inst != nullptr;
                            it->terms[k] = {&i, i.Arg(slot), scale};
                            it->read = read->inst;
                        }
                    }
                }
            }
        }
    }
    std::erase_if(axes, [](const Axis& a) {
        return a.ambiguous ||
               std::ranges::any_of(a.terms, [](const Term& t) { return t.inst == nullptr; });
    });
    return axes;
}

bool Rewrite(IR::Program& program, const Axis& right, const Axis& up) {
    const auto hash = program.info.pgm_hash;
    std::array<IR::Inst*, 3> sums{};
    for (u32 k = 0; k < 3; ++k) {
        sums[k] = CornerSum(program, right.terms[k].inst, up.terms[k].inst);
        if (!sums[k] || (k > 0 && sums[k]->GetParent() != sums[0]->GetParent())) {
            LOG_INFO(Render_Recompiler, "VR billboard: shader {:#x} ({}) corner {} not a plain sum",
                     hash, right.source->name, k);
            return false;
        }
    }
    // The new corners go right after the last of the three sums; every use of a sum must follow.
    IR::Block& block = *sums[0]->GetParent();
    std::unordered_map<const IR::Inst*, u32> order;
    u32 index = 0;
    for (IR::Inst& i : block) {
        order[&i] = index++;
    }
    IR::Inst* last = *std::ranges::max_element(sums, {}, [&](IR::Inst* i) { return order[i]; });
    for (IR::Inst* sum : sums) {
        for (const auto& use : sum->Uses()) {
            if (use.user->GetParent() == &block && order[use.user] <= order[last]) {
                LOG_INFO(Render_Recompiler,
                         "VR billboard: shader {:#x} ({}) uses a corner before all three exist",
                         hash, right.source->name);
                return false;
            }
        }
    }

    const auto next = std::next(IR::Block::InstructionList::s_iterator_to(*last));
    IR::IREmitter ir{block, next};
    const auto info = right.read->Flags<IR::BufferInstInfo>();
    const auto camera = [&](u32 offset) {
        return F{&ir, ir.BitCast<IR::F32>(
                          ir.ReadConstBuffer(ir.Imm32(right.binding), ir.Imm32(offset), info))};
    };
    Vec3<F> r, u, t, c;
    for (u32 k = 0; k < 3; ++k) {
        r[k] = F{&ir, IR::F32{right.terms[k].axis}};
        u[k] = F{&ir, IR::F32{up.terms[k].axis}};
        t[k] = camera(right.source->translation[k]);
    }
    const F sr{&ir, IR::F32{right.scale}}, su{&ir, IR::F32{up.scale}};
    for (u32 k = 0; k < 3; ++k) {
        c[k] = F{&ir, IR::F32{IR::Value{sums[k]}}} - sr * r[k] - su * u[k];
    }
    const Vec3<F> eye = right.source->eye_direct ? t : EyeFromView(r, u, t);
    const auto [r2, u2] = FaceEye(r, u, c, eye);
    std::array<F, 3> corner;
    for (u32 k = 0; k < 3; ++k) {
        corner[k] = c[k] + sr * r2[k] + su * u2[k];
    }

    std::unordered_set<const IR::Inst*> added;
    for (auto it = std::next(IR::Block::InstructionList::s_iterator_to(*last)); it != next; ++it) {
        added.insert(&*it);
    }
    for (u32 k = 0; k < 3; ++k) {
        for (const auto& use : sums[k]->Uses()) {
            if (!added.contains(use.user)) {
                use.user->SetArg(use.operand, corner[k].v);
            }
        }
    }
    LOG_INFO(Render_Recompiler, "VR billboard: shader {:#x} faces the eye (binding {}, {})", hash,
             right.binding, right.source->name);
    return true;
}

// ---- Per-particle axes (the tessellated smoke and fire sprites) ----
// Each patch is one particle: corner p_k = sum_j a_kj s_j (k = x, y, z) with a_kj the particle
// matrix read from the patch, s_j shared by the three rows, two of the s_j moving with the
// tessellation point (the sprite's axes j0, j1), and p projected by VP rows read from one constant
// buffer. The rewrite adds (a'_kj - a_kj) s_j for both axes, a' from FaceEyeUpright.

// A sum flattened through FPAdd / FPFma / FPMul; anything else is a lone addend.
struct Product {
    IR::Value a, b;
};
struct SumOf {
    std::vector<Product> products;
    std::vector<IR::Value> addends;
};
void Flatten(const IR::Value& value, SumOf& out, u32 depth = 0) {
    const IR::Value v = value.Resolve();
    if (!v.IsImmediate() && depth < 16) {
        IR::Inst* i = v.InstRecursive();
        switch (i->GetOpcode()) {
        case IR::Opcode::FPAdd32:
            Flatten(i->Arg(0), out, depth + 1);
            Flatten(i->Arg(1), out, depth + 1);
            return;
        case IR::Opcode::FPFma32:
            out.products.push_back({i->Arg(0).Resolve(), i->Arg(1).Resolve()});
            Flatten(i->Arg(2), out, depth + 1);
            return;
        case IR::Opcode::FPMul32:
            out.products.push_back({i->Arg(0).Resolve(), i->Arg(1).Resolve()});
            return;
        default:
            break;
        }
    }
    out.addends.push_back(v);
}

// One VP row applied to the corner: r0 p0 + r1 p1 + r2 p2 + r3, r_c at four consecutive offsets.
struct Row {
    u32 binding{}, base{};
    std::array<IR::Value, 3> p;
    IR::Inst* read{};
};
std::optional<Row> ProjectionRow(const IR::Value& value) {
    SumOf sum;
    Flatten(value, sum);
    if (sum.products.size() != 3 || sum.addends.size() != 1) {
        return {};
    }
    const auto t = CameraRead(sum.addends[0]);
    if (!t || t->offset < 3) {
        return {};
    }
    Row row{t->binding, t->offset - 3, {}, t->inst};
    std::array<bool, 3> seen{};
    for (const Product& product : sum.products) {
        const auto ra = CameraRead(product.a), rb = CameraRead(product.b);
        if (ra.has_value() == rb.has_value()) {
            return {};
        }
        const Read& r = ra ? *ra : *rb;
        if (r.binding != row.binding || r.offset < row.base || r.offset > row.base + 2 ||
            seen[r.offset - row.base]) {
            return {};
        }
        seen[r.offset - row.base] = true;
        row.p[r.offset - row.base] = ra ? product.b : product.a;
    }
    return row;
}

// Whether a value moves with the tessellation point (memoized; a phi cycle counts as no).
bool MovesWithCorner(const IR::Value& value, std::unordered_map<const IR::Inst*, bool>& memo,
                     u32 depth = 0) {
    const IR::Value v = value.Resolve();
    if (v.IsImmediate() || depth > 48) {
        return false;
    }
    const IR::Inst* i = v.InstRecursive();
    if (const auto it = memo.find(i); it != memo.end()) {
        return it->second;
    }
    memo[i] = false;
    bool moves = false;
    if (i->GetOpcode() == IR::Opcode::GetAttribute) {
        const auto attr = i->Arg(0).Attribute();
        moves = attr == IR::Attribute::TessellationEvaluationPointU ||
                attr == IR::Attribute::TessellationEvaluationPointV;
    }
    for (size_t n = 0; !moves && n < i->NumArgs(); ++n) {
        moves = MovesWithCorner(i->Arg(n), memo, depth + 1);
    }
    memo[i] = moves;
    return moves;
}

// Per-particle data: the matrix entries come straight from the patch (never from constants).
bool FromPatch(const IR::Value& value) {
    const IR::Value v = value.Resolve();
    return !v.IsImmediate() &&
           v.InstRecursive()->GetOpcode() == IR::Opcode::GetTessGenericAttribute;
}

bool RewriteParticleAxes(IR::Program& program) {
    const auto hash = program.info.pgm_hash;
    std::array<IR::Value, 4> position{};
    std::array<u32, 4> writes{};
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& i : *block) {
            if (i.GetOpcode() == IR::Opcode::SetAttribute &&
                i.Arg(0).Attribute() == IR::Attribute::Position0 && i.Arg(2).U32() < 4) {
                position[i.Arg(2).U32()] = i.Arg(1);
                ++writes[i.Arg(2).U32()];
            }
        }
    }
    if (writes[0] != 1 || writes[1] != 1 || writes[3] != 1) {
        return false;
    }
    const auto rx = ProjectionRow(position[0]), ry = ProjectionRow(position[1]),
               rw = ProjectionRow(position[3]);
    if (!rx || !ry || !rw || ry->binding != rx->binding || rw->binding != rx->binding) {
        return false;
    }
    std::array<IR::Inst*, 3> corner{};
    for (u32 k = 0; k < 3; ++k) {
        if (rx->p[k] != ry->p[k] || rx->p[k] != rw->p[k] || rx->p[k].IsImmediate()) {
            return false;
        }
        corner[k] = rx->p[k].InstRecursive();
    }
    // The shared factors s_j, taken from the first row; each must appear once in every row.
    std::array<SumOf, 3> rows;
    for (u32 k = 0; k < 3; ++k) {
        Flatten(rx->p[k], rows[k]);
        if (!rows[k].addends.empty() || rows[k].products.size() != rows[0].products.size()) {
            return false;
        }
    }
    const size_t columns = rows[0].products.size();
    if (columns < 3 || columns > 4) {
        return false;
    }
    std::vector<IR::Value> s(columns);
    std::vector<std::array<IR::Value, 3>> a(columns);
    const auto other = [](const Product& p, const IR::Value& x) {
        return p.a == x ? std::optional{p.b} : p.b == x ? std::optional{p.a} : std::nullopt;
    };
    for (size_t j = 0; j < columns; ++j) {
        const Product& p = rows[0].products[j];
        for (const IR::Value candidate : {p.a, p.b}) {
            bool everywhere = true;
            for (u32 k = 1; k < 3 && everywhere; ++k) {
                everywhere = std::ranges::count_if(rows[k].products, [&](const Product& q) {
                                 return other(q, candidate).has_value();
                             }) == 1;
            }
            if (everywhere && s[j].IsEmpty()) {
                s[j] = candidate;
            }
        }
        if (s[j].IsEmpty()) {
            return false;
        }
        for (u32 k = 0; k < 3; ++k) {
            for (const Product& q : rows[k].products) {
                if (const auto x = other(q, s[j])) {
                    a[j][k] = *x;
                }
            }
        }
    }
    std::unordered_map<const IR::Inst*, bool> memo;
    std::vector<size_t> axes;
    for (size_t j = 0; j < columns; ++j) {
        for (u32 k = 0; k < 3; ++k) {
            if (a[j][k].IsEmpty() || MovesWithCorner(a[j][k], memo)) {
                return false;
            }
        }
        if (MovesWithCorner(s[j], memo)) {
            axes.push_back(j);
        }
    }
    if (axes.size() != 2) {
        return false;
    }
    for (const size_t j : axes) {
        for (u32 k = 0; k < 3; ++k) {
            if (!FromPatch(a[j][k])) {
                LOG_INFO(Render_Recompiler,
                         "VR billboard: shader {:#x} sprite axes are not per-particle data", hash);
                return false;
            }
        }
    }
    // The particle's centre takes each moving factor's fixed part: s = base + offset(corner).
    std::vector<IR::Value> base(columns);
    for (size_t j = 0; j < columns; ++j) {
        base[j] = s[j];
        if (std::ranges::find(axes, j) == axes.end()) {
            continue;
        }
        base[j] = IR::Value{0.0f};
        const IR::Value v = s[j].Resolve();
        if (!v.IsImmediate() && v.InstRecursive()->GetOpcode() == IR::Opcode::FPAdd32) {
            const IR::Inst* add = v.InstRecursive();
            const bool m0 = MovesWithCorner(add->Arg(0), memo);
            const bool m1 = MovesWithCorner(add->Arg(1), memo);
            if (m0 != m1) {
                base[j] = (m0 ? add->Arg(1) : add->Arg(0)).Resolve();
            }
        }
    }

    // The new corner is built from the particle data alone, right after the last of its inputs:
    // the game's shaders use one corner component before the others exist, so the new code
    // cannot wait for all three. Every use of a corner must follow that point.
    IR::Block& block = *corner[0]->GetParent();
    if (corner[1]->GetParent() != &block || corner[2]->GetParent() != &block) {
        LOG_INFO(Render_Recompiler, "VR billboard: shader {:#x} corner spans blocks", hash);
        return false;
    }
    std::unordered_map<const IR::Inst*, u32> order;
    u32 index = 0;
    for (IR::Inst& i : block) {
        order[&i] = index++;
    }
    IR::Inst* anchor = nullptr;
    const auto consider = [&](const IR::Value& v) {
        if (v.IsImmediate()) {
            return;
        }
        IR::Inst* i = v.InstRecursive();
        if (i->GetParent() == &block && (!anchor || order[i] > order[anchor])) {
            anchor = i;
        }
    };
    for (size_t j = 0; j < columns; ++j) {
        consider(s[j]);
        consider(base[j]);
        for (u32 k = 0; k < 3; ++k) {
            consider(a[j][k]);
        }
    }
    for (IR::Inst* c : corner) {
        for (const auto& use : c->Uses()) {
            if (use.user->GetParent() == &block && anchor && order[use.user] <= order[anchor]) {
                LOG_INFO(Render_Recompiler,
                         "VR billboard: shader {:#x} uses a corner before its inputs exist", hash);
                return false;
            }
        }
    }
    auto start = block.begin();
    if (anchor) {
        start = std::next(IR::Block::InstructionList::s_iterator_to(*anchor));
    } else {
        while (start != block.end() && start->GetOpcode() == IR::Opcode::Phi) {
            ++start;
        }
    }
    const auto first_added = start == block.begin() ? block.end() : std::prev(start);
    IR::IREmitter ir{block, start};
    const auto info = rx->read->Flags<IR::BufferInstInfo>();
    const auto camera = [&](u32 offset) {
        return F{&ir, ir.BitCast<IR::F32>(
                          ir.ReadConstBuffer(ir.Imm32(rx->binding), ir.Imm32(offset), info))};
    };
    const auto value = [&](const IR::Value& v) {
        return v.IsImmediate() ? F{v.F32()} : F{&ir, IR::F32{v}};
    };
    Vec3<F> row0, row1, row3;
    for (u32 c = 0; c < 3; ++c) {
        row0[c] = camera(rx->base + c);
        row1[c] = camera(ry->base + c);
        row3[c] = camera(rw->base + c);
    }
    const auto view = ViewFromViewProjection(row0, camera(rx->base + 3), row1,
                                             camera(ry->base + 3), row3, camera(rw->base + 3));
    Vec3<F> a0, a1, center;
    for (u32 k = 0; k < 3; ++k) {
        a0[k] = value(a[axes[0]][k]);
        a1[k] = value(a[axes[1]][k]);
        F sum{0.0f};
        for (size_t j = 0; j < columns; ++j) {
            sum = sum + value(a[j][k]) * value(base[j]);
        }
        center[k] = sum;
    }
    F weight;
    const auto [b0, b1] = FaceEyeUpright(view, a0, a1, center, weight);
    // p'_k = sum_j a'_kj s_j, with a' = a + weight (b - a) on the sprite's two axes.
    std::array<F, 3> moved;
    for (u32 k = 0; k < 3; ++k) {
        F sum{0.0f};
        for (size_t j = 0; j < columns; ++j) {
            F entry = value(a[j][k]);
            if (j == axes[0]) {
                entry = entry + weight * (b0[k] - a0[k]);
            } else if (j == axes[1]) {
                entry = entry + weight * (b1[k] - a1[k]);
            }
            sum = sum + entry * value(s[j]);
        }
        moved[k] = sum;
    }

    std::unordered_set<const IR::Inst*> added;
    for (auto it = first_added == block.end() ? block.begin() : std::next(first_added);
         it != start; ++it) {
        added.insert(&*it);
    }
    for (u32 k = 0; k < 3; ++k) {
        for (const auto& use : corner[k]->Uses()) {
            if (!added.contains(use.user)) {
                use.user->SetArg(use.operand, moved[k].v);
            }
        }
    }
    LOG_INFO(Render_Recompiler,
             "VR billboard: shader {:#x} faces the eye (per-particle axes, binding {}, VP rows {} "
             "{} {})",
             hash, rx->binding, rx->base, ry->base, rw->base);
    return true;
}

// ---- Geometry-stage sprites (the effect system's points expanded to quads: sparks, item glows) ----
// Each point is one particle: corner = centre + M (x', y', 0, 1), (x', y') its corner rotated by
// the particle's spin, M's first two columns the camera's right (dwords 36, 40, 44) and up
// (37, 41, 45) from the effect camera block, which also holds the camera position (20..22) and the
// view-projection rows (52..67; row 3, the forward axis, at 64..66). Every read of those six
// dwords takes the eye-facing pair instead (FaceEyeUpright with the camera's own axes: spin and
// size kept). Trails built from the camera position alone read none of them and are left alone.
constexpr u32 GsRight[3] = {36, 40, 44};
constexpr u32 GsUp[3] = {37, 41, 45};
constexpr u32 GsEye[3] = {20, 21, 22};
constexpr u32 GsForward[3] = {64, 65, 66};

void RewriteGeometrySprites(IR::Program& program) {
    std::unordered_map<u32, std::vector<Read>> reads; // per binding
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& i : *block) {
            if (i.GetOpcode() == IR::Opcode::ReadConstBuffer && i.Arg(0).IsImmediate() &&
                i.Arg(1).IsImmediate()) {
                reads[i.Arg(0).U32()].push_back({i.Arg(0).U32(), i.Arg(1).U32(), &i});
            }
        }
    }
    const auto has = [](const std::vector<Read>& r, u32 offset) {
        return std::ranges::any_of(r, [&](const Read& x) { return x.offset == offset; });
    };
    for (auto& [binding, r] : reads) {
        bool all = true;
        for (const u32 o : {GsRight[0], GsRight[1], GsRight[2], GsUp[0], GsUp[1], GsUp[2],
                            GsEye[0], GsEye[1], GsEye[2], GsForward[0], GsForward[1],
                            GsForward[2]}) {
            all = all && has(r, o);
        }
        if (!all) {
            continue;
        }
        IR::Block& entry = *program.blocks.front();
        IR::IREmitter ir{entry, entry.begin()};
        const auto info = r.front().inst->Flags<IR::BufferInstInfo>();
        const auto load = [&](u32 offset) {
            return F{&ir, ir.BitCast<IR::F32>(
                              ir.ReadConstBuffer(ir.Imm32(binding), ir.Imm32(offset), info))};
        };
        ViewFrame<F> view;
        Vec3<F> forward, centre;
        for (u32 k = 0; k < 3; ++k) {
            view.right[k] = load(GsRight[k]);
            view.up[k] = load(GsUp[k]);
            view.eye[k] = load(GsEye[k]);
            forward[k] = load(GsForward[k]);
            centre[k] = F{&ir, ir.GetAttribute(IR::Attribute::Position0, k, 0)};
        }
        const F inv_f = RecipSqrt(Dot(forward, forward));
        for (u32 k = 0; k < 3; ++k) {
            view.forward[k] = forward[k] * inv_f;
        }
        F weight;
        const auto [r2, u2] = FaceEyeUpright(view, view.right, view.up, centre, weight);
        std::array<IR::U32, 3> right_bits, up_bits;
        for (u32 k = 0; k < 3; ++k) {
            right_bits[k] = ir.BitCast<IR::U32>(r2[k].v);
            up_bits[k] = ir.BitCast<IR::U32>(u2[k].v);
        }
        u32 replaced = 0;
        for (const Read& x : r) {
            for (u32 k = 0; k < 3; ++k) {
                const bool is_right = x.offset == GsRight[k];
                if (!is_right && x.offset != GsUp[k]) {
                    continue;
                }
                const IR::Value value{is_right ? right_bits[k] : up_bits[k]};
                std::vector<std::pair<IR::Inst*, u32>> uses;
                for (const auto& use : x.inst->Uses()) {
                    uses.emplace_back(use.user, use.operand);
                }
                for (const auto& [user, operand] : uses) {
                    user->SetArg(operand, value);
                }
                ++replaced;
            }
        }
        LOG_INFO(Render_Recompiler,
                 "VR billboard: shader {:#x} faces the eye (geometry sprites, binding {}, {} axis "
                 "reads)",
                 program.info.pgm_hash, binding, replaced);
        return;
    }
}

} // namespace

void BillboardFaceEyePass(IR::Program& program, const RuntimeInfo& runtime_info) {
    // In native stereo every camera-facing sprite faces the eye; VR_BILLBOARD_FACE_EYE=0 turns it
    // off (with a fresh shader cache), any other value turns it on outside native stereo too.
    static const char* setting = std::getenv("VR_BILLBOARD_FACE_EYE");
    const bool enabled = setting ? setting[0] != '0' : runtime_info.native_stereo;
    if (enabled && program.info.stage == Stage::Geometry) {
        RewriteGeometrySprites(program);
        return;
    }
    if (!enabled || program.info.stage != Stage::Vertex) {
        return;
    }
    if (program.info.l_stage == LogicalStage::TessellationEval && RewriteParticleAxes(program)) {
        return;
    }
    const auto axes = FindAxes(program);
    for (const Axis& right : axes) {
        if (right.up) {
            continue;
        }
        for (const Axis& up : axes) {
            if (up.up && up.binding == right.binding && up.source == right.source &&
                Rewrite(program, right, up)) {
                return;
            }
        }
    }
}

} // namespace Shader::Optimization
