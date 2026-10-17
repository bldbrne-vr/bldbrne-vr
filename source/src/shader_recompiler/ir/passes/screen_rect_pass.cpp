// SPDX-License-Identifier: GPL-2.0-or-later

// VR: compute shaders that copy or process a pixel rectangle of an image (the heat haze's grab of
// the scene around the fire, the refraction of glass and water). Each image store writes pixel
// (constant + global invocation id) and only below a constant end; image reads at (constant +
// the same id) read the same rectangle. The game gives the rectangle on the flat game's screen, so
// in native stereo each eye's rectangle has to move to where that eye sees the effect
// (NativeStereo::ScreenRectBuffer); this pass only finds the constants (Info::vr_screen_rect).

#include <array>
#include <optional>
#include <string_view>
#include <vector>

#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/program.h"

namespace Shader::Optimization {
namespace {

struct Coord {
    u32 binding = ~0u;
    u32 dword = ~0u;
    u32 axis = ~0u;
    bool operator==(const Coord&) const = default;
};

// value = constant dword + workgroup id * size + local invocation id, along one axis.
std::optional<Coord> MatchCoord(const IR::Value& value) {
    Coord c;
    u32 group_axis = ~0u;
    u32 local_axis = ~0u;
    const auto walk = [&](const auto& self, const IR::Value& v) -> bool {
        if (v.IsImmediate()) {
            return v.U32() == 0;
        }
        const IR::Inst* inst = v.InstRecursive();
        switch (inst->GetOpcode()) {
        case IR::Opcode::IAdd32:
        case IR::Opcode::BitwiseOr32:
            return self(self, inst->Arg(0)) && self(self, inst->Arg(1));
        case IR::Opcode::ReadConstBuffer:
            if (c.binding != ~0u || !inst->Arg(0).IsImmediate() || !inst->Arg(1).IsImmediate()) {
                return false;
            }
            c.binding = inst->Arg(0).U32();
            c.dword = inst->Arg(1).U32();
            return true;
        case IR::Opcode::ShiftLeftLogical32:
        case IR::Opcode::IMul32: {
            if (group_axis != ~0u || !inst->Arg(1).IsImmediate() || inst->Arg(0).IsImmediate()) {
                return false;
            }
            const IR::Inst* id = inst->Arg(0).InstRecursive();
            if (id->GetOpcode() != IR::Opcode::GetAttributeU32 ||
                id->Arg(0).Attribute() != IR::Attribute::WorkgroupId) {
                return false;
            }
            group_axis = id->Arg(1).U32();
            return true;
        }
        case IR::Opcode::GetAttributeU32:
            if (local_axis != ~0u || inst->Arg(0).Attribute() != IR::Attribute::LocalInvocationId) {
                return false;
            }
            local_axis = inst->Arg(1).U32();
            return true;
        default:
            return false;
        }
    };
    if (!walk(walk, value) || c.binding == ~0u || group_axis > 1 || group_axis != local_axis) {
        return std::nullopt;
    }
    c.axis = group_axis;
    return c;
}

// The x and y of an image instruction's coordinates (a composite).
std::optional<std::array<Coord, 2>> MatchCoords(const IR::Value& coords) {
    if (coords.IsImmediate()) {
        return std::nullopt;
    }
    const IR::Inst* inst = coords.InstRecursive();
    switch (inst->GetOpcode()) {
    case IR::Opcode::CompositeConstructU32x2:
    case IR::Opcode::CompositeConstructU32x3:
    case IR::Opcode::CompositeConstructU32x4:
        break;
    default:
        return std::nullopt;
    }
    const auto x = MatchCoord(inst->Arg(0));
    const auto y = MatchCoord(inst->Arg(1));
    if (!x || !y || x->axis != 0 || y->axis != 1 || x->binding != y->binding) {
        return std::nullopt;
    }
    return std::array{*x, *y};
}

// Other image work, or writes to buffers or shared memory: more than a rectangle pass.
bool OtherEffects(IR::Opcode op) {
    const std::string_view name = IR::NameOf(op);
    return name.starts_with("Image") || name.starts_with("StoreBuffer") ||
           name.starts_with("BufferAtomic") || name.starts_with("WriteShared") ||
           name.starts_with("SharedAtomic");
}

} // namespace

void ScreenRectPass(IR::Program& program) {
    auto& info = program.info;
    if (info.stage != Stage::Compute) {
        return;
    }
    std::optional<std::array<Coord, 2>> dst, src;
    u32 image = ~0u;
    std::array<u32, 2> end{~0u, ~0u};
    std::vector<const IR::Inst*> compares;
    for (IR::Block* const block : program.blocks) {
        for (const IR::Inst& inst : block->Instructions()) {
            switch (inst.GetOpcode()) {
            case IR::Opcode::ImageWrite: {
                const auto c = MatchCoords(inst.Arg(1));
                if (!c || (dst && *dst != *c) || !inst.Arg(0).IsImmediate() ||
                    (image != ~0u && image != (inst.Arg(0).U32() & 0xFFFF))) {
                    return;
                }
                dst = c;
                image = inst.Arg(0).U32() & 0xFFFF;
                break;
            }
            case IR::Opcode::ImageRead: {
                const auto c = MatchCoords(inst.Arg(1));
                if (!c || (src && *src != *c)) {
                    return;
                }
                src = c;
                break;
            }
            case IR::Opcode::UGreaterThan32:
            case IR::Opcode::SGreaterThan32:
            case IR::Opcode::ULessThan32:
            case IR::Opcode::SLessThan32:
                compares.push_back(&inst);
                break;
            default:
                if (OtherEffects(inst.GetOpcode())) {
                    return;
                }
                break;
            }
        }
    }
    if (!dst || (src && (*src)[0].binding != (*dst)[0].binding)) {
        return;
    }
    // The bound: end > coordinate (or coordinate < end) on each axis.
    for (const IR::Inst* inst : compares) {
        const bool greater = inst->GetOpcode() == IR::Opcode::UGreaterThan32 ||
                             inst->GetOpcode() == IR::Opcode::SGreaterThan32;
        const IR::Value bound = inst->Arg(greater ? 0 : 1);
        const auto coord = MatchCoord(inst->Arg(greater ? 1 : 0));
        if (!coord || bound.IsImmediate() || *coord != (*dst)[coord->axis]) {
            continue;
        }
        const IR::Inst* read = bound.InstRecursive();
        if (read->GetOpcode() == IR::Opcode::ReadConstBuffer && read->Arg(0).IsImmediate() &&
            read->Arg(1).IsImmediate() && read->Arg(0).U32() == coord->binding) {
            end[coord->axis] = read->Arg(1).U32();
        }
    }
    const auto fits = [](u32 v) { return v < 0xFF; };
    if (end[0] == ~0u || end[1] == ~0u || !fits((*dst)[0].binding) || !fits(image) ||
        !fits((*dst)[0].dword) || !fits((*dst)[1].dword) || !fits(end[0]) || !fits(end[1]) ||
        (src && (!fits((*src)[0].dword) || !fits((*src)[1].dword)))) {
        return;
    }
    auto& rect = info.vr_screen_rect;
    rect.binding = static_cast<u8>((*dst)[0].binding);
    rect.image = static_cast<u8>(image);
    rect.dst = {static_cast<u8>((*dst)[0].dword), static_cast<u8>((*dst)[1].dword)};
    rect.end = {static_cast<u8>(end[0]), static_cast<u8>(end[1])};
    if (src) {
        rect.src = {static_cast<u8>((*src)[0].dword), static_cast<u8>((*src)[1].dword)};
    }
    LOG_INFO(Render_Recompiler,
             "VR screen rect: compute shader {:#x} copies a pixel rectangle (buffer {}, offset "
             "dwords {} {}, end {} {}{}); each eye gets its own",
             info.pgm_hash, rect.binding, rect.dst[0], rect.dst[1], rect.end[0], rect.end[1],
             src ? fmt::format(", read at {} {}", rect.src[0], rect.src[1]) : std::string{});
}

} // namespace Shader::Optimization
