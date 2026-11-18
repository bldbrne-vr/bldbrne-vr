// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Moon shadows (moon_shadow.h): the game's screen-space cascaded shadow pass (the flag
// RuntimeInfo::VrMoonShadows is set for its programs only), recognized from IR:
//  - the view position: a buffer read at dwords 0..15 whose rows 12, 13 and 14 multiply the
//    pixel's ndc x, y and its depth (an image sample's x) in the entry block, the same buffer's
//    cascade splits read at 80..82;
//  - the atlas taps: depth-compare samples (or gathers) of one image and sampler.
// VrMoonShadow takes the taps' place: each tap's result becomes ours, so the game's sum of its
// taps is ours and its distance fade and depth packing stay as they are (the taps' own
// coordinates then feed nothing and the driver drops them).

#include <algorithm>
#include <cstdlib>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <fmt/format.h>
#include "common/logging/log.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/basic_block.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/passes/moon_shadow.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {
namespace {

// The value that `handle`'s dword `dword` multiplies (FPMul32, or an FPFma32's product), through
// a float bitcast of the read.
std::optional<IR::Value> MoonFactor(IR::Block* block, u32 handle, u32 dword) {
    const auto read_of = [&](const IR::Value& v) {
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
    };
    for (IR::Inst& inst : block->Instructions()) {
        const auto op = inst.GetOpcode();
        if (op != IR::Opcode::FPMul32 && op != IR::Opcode::FPFma32) {
            continue;
        }
        for (u32 a = 0; a < 2; ++a) {
            if (read_of(inst.Arg(a))) {
                return inst.Arg(1 - a);
            }
        }
    }
    return std::nullopt;
}

bool IsTap(IR::Opcode op) {
    return op == IR::Opcode::ImageSampleDrefExplicitLod ||
           op == IR::Opcode::ImageSampleDrefImplicitLod || op == IR::Opcode::ImageGatherDref;
}

} // namespace

void MoonShadowPass(IR::Program& program, const RuntimeInfo& runtime_info) {
    auto& info = program.info;
    if (info.stage != Stage::Fragment ||
        (runtime_info.vr_ambient_occlusion & RuntimeInfo::VrMoonShadows) == 0 ||
        program.blocks.empty()) {
        return;
    }
    IR::Block* const entry = program.blocks.front();
    const auto skip = [&](const char* why) {
        LOG_INFO(Render_Recompiler, "VR moon shadows: shader {:#x} left as it is ({})",
                 info.pgm_hash, why);
    };
    // The atlas taps: one image and sampler, all in the entry block.
    std::vector<IR::Inst*> taps;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (IsTap(inst.GetOpcode())) {
                taps.push_back(&inst);
            }
        }
    }
    if (taps.empty() || !taps.front()->Arg(0).IsImmediate()) {
        return skip("no atlas taps");
    }
    const u32 atlas = taps.front()->Arg(0).U32();
    if (std::ranges::any_of(taps, [&](const IR::Inst* t) {
            return t->GetParent() != entry || !t->Arg(0).IsImmediate() || t->Arg(0).U32() != atlas;
        })) {
        return skip("taps of several images or outside the entry");
    }
    // Every immediate constant buffer read, by handle.
    std::map<u32, std::set<u32>> dwords;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::ReadConstBuffer && inst.Arg(0).IsImmediate() &&
                inst.Arg(1).IsImmediate()) {
                dwords[inst.Arg(0).U32()].insert(inst.Arg(1).U32());
            }
        }
    }
    for (const auto& [cascades, used] : dwords) {
        bool rows = used.contains(80) && used.contains(81) && used.contains(82);
        for (u32 d = 0; d < 16; ++d) {
            rows &= used.contains(d);
        }
        if (!rows) {
            continue;
        }
        const auto ndc_x = MoonFactor(entry, cascades, 12);
        const auto ndc_y = MoonFactor(entry, cascades, 13);
        const auto depth = MoonFactor(entry, cascades, 14);
        if (!ndc_x || !ndc_y || !depth || depth->IsImmediate() || ndc_x->IsImmediate() ||
            ndc_y->IsImmediate()) {
            continue;
        }
        IR::Inst* const extract = depth->InstRecursive();
        if (extract->GetOpcode() != IR::Opcode::CompositeExtractF32x4 ||
            !extract->Arg(1).IsImmediate() || extract->Arg(1).U32() != 0 ||
            extract->Arg(0).IsImmediate()) {
            continue;
        }
        IR::Inst* const sample = extract->Arg(0).InstRecursive();
        if ((sample->GetOpcode() != IR::Opcode::ImageSampleImplicitLod &&
             sample->GetOpcode() != IR::Opcode::ImageSampleExplicitLod) ||
            !sample->Arg(0).IsImmediate()) {
            continue;
        }
        const u32 depth_image = sample->Arg(0).U32();
        const IR::Value uv = sample->Arg(1);
        // Every input before the first tap, in the entry block (it is ours then).
        std::vector<const IR::Inst*> inputs{extract, sample};
        for (const IR::Value* v : {&*ndc_x, &*ndc_y, &uv}) {
            if (!v->IsImmediate()) {
                inputs.push_back(v->InstRecursive());
            }
        }
        if (std::ranges::any_of(inputs,
                                [&](const IR::Inst* i) { return i->GetParent() != entry; })) {
            return skip("inputs outside the entry");
        }
        std::set<const IR::Inst*> pending(inputs.begin(), inputs.end());
        IR::Inst* first_tap = nullptr;
        for (IR::Inst& inst : entry->Instructions()) {
            if (IsTap(inst.GetOpcode())) {
                first_tap = &inst;
                break;
            }
            pending.erase(&inst);
        }
        if (first_tap == nullptr || !pending.empty()) {
            return skip("inputs after the taps");
        }
        // The distance fade's buffer (dwords 72, 73: the fade's end and slope), if any: no
        // shadow past it, nothing to compute there.
        u32 params = 0xFFFFFFFFu;
        for (const auto& [handle, read] : dwords) {
            if (handle != cascades && read.contains(72) && read.contains(73)) {
                params = handle;
            }
        }
        // The atlas's compare direction (the game's sampler): a depth farther from the moon is
        // larger unless it compares Greater (reversed).
        const u32 compare_sampler = atlas >> 16;
        bool reversed = false;
        if (compare_sampler < info.samplers.size()) {
            const auto func = info.samplers[compare_sampler].GetSharp(info).depth_compare_func.Value();
            reversed = func == AmdGpu::DepthCompare::Greater ||
                       func == AmdGpu::DepthCompare::GreaterEqual;
            LOG_INFO(Render_Recompiler, "VR moon shadows: shader {:#x}: the atlas compares {}",
                     info.pgm_hash, static_cast<u32>(func));
        }
        // VR_MOON_SHADOW_DUMP=1 (tests): the buffers as this draw has them (the cascades' layout
        // in the atlas, the game's tap spacing and fade).
        static const bool dump = std::getenv("VR_MOON_SHADOW_DUMP") != nullptr;
        if (dump) {
            for (const u32 handle : {cascades, params}) {
                if (handle >= info.buffers.size()) {
                    continue;
                }
                const auto sharp = info.buffers[handle].GetSharp(info);
                const u32 count = std::min<u32>(u32(sharp.GetSize() / 4), 96u);
                if (sharp.base_address == 0 || count == 0) {
                    continue;
                }
                const auto* words = static_cast<const float*>(ScratchDraw::MemorySource(
                    reinterpret_cast<const void*>(sharp.base_address), count * 4));
                for (u32 at = 0; at < count; at += 8) {
                    std::string line;
                    for (u32 k = at; k < std::min(count, at + 8); ++k) {
                        line += fmt::format(" {:.6g}", words[k]);
                    }
                    LOG_INFO(Render_Recompiler, "VR moon shadows: buffer {} dwords {}..:{}",
                             handle, at, line);
                }
            }
        }
        IR::IREmitter ir{*entry, IR::Block::InstructionList::s_iterator_to(*first_tap)};
        const IR::Value ndc = ir.CompositeConstruct(IR::F32{*ndc_x}, IR::F32{*ndc_y});
        const IR::F32 lit = ir.VrMoonShadow(ndc, uv, IR::F32{*depth});
        const IR::F32 zero = ir.Imm32(0.0f);
        const IR::Value one_tap = ir.CompositeConstruct(lit, zero, zero, zero);
        const IR::Value four = ir.CompositeConstruct(lit, lit, lit, lit);
        for (IR::Inst* tap : taps) {
            tap->ReplaceUsesWithAndRemove(tap->GetOpcode() == IR::Opcode::ImageGatherDref ? four
                                                                                        : one_tap);
        }
        info.vr_moon_shadow = {
            .on = true,
            .reversed = reversed,
            .cascades_buffer = cascades,
            .params_buffer = params,
            .depth_image = depth_image,
            .shadow_compare = atlas,
            // The atlas with the depth's plain sampler: its raw depths (the blocker search).
            .shadow_raw = (atlas & 0xFFFFu) | (depth_image & 0xFFFF0000u),
        };
        LOG_INFO(Render_Recompiler,
                 "VR moon shadows: shader {:#x}: {} atlas taps replaced (cascades {}, fade {}, "
                 "depth image {:#x}, atlas {:#x}{})",
                 info.pgm_hash, taps.size(), cascades, params, depth_image, atlas,
                 reversed ? ", reversed" : "");
        return;
    }
    skip("no cascades");
}

} // namespace Shader::Optimization
