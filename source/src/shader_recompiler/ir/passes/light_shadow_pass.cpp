// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// Light shadows (light_shadow.h): in a light rectangle's fragment stage, recognized from IR only:
//  - a matrix buffer read at dwords 0..15 whose rows 12, 13 and 14 multiply the pixel's ndc x, y
//    and its depth (an image sample's x): the pixel's view position;
//  - another buffer with lights of 16 dwords: k's position at 16k..16k+2 and colours at
//    16k+4..6 and 16k+8..10.
// Each light's colours (as floats) are multiplied by VrLightVisibility, computed in the entry
// block once the pixel's ndc, coordinates and depth are known. Anything else is left untouched.

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
#include "shader_recompiler/ir/passes/ambient_occlusion.h"
#include "shader_recompiler/ir/passes/body_shadow.h"
#include "shader_recompiler/ir/passes/bounce_light.h"
#include "shader_recompiler/ir/passes/fog_light.h"
#include "shader_recompiler/ir/passes/cinematic_look.h"
#include "shader_recompiler/ir/passes/ir_passes.h"
#include "shader_recompiler/ir/passes/light_shadow.h"
#include "shader_recompiler/ir/passes/air_light.h"
#include "shader_recompiler/ir/passes/moon_shafts.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/runtime_info.h"

namespace Shader::Optimization {
namespace {

struct Read {
    u32 handle;
    u32 dword;
};

// A constant buffer dword read with immediate handle and index, seen through its float bitcast.
std::optional<Read> ReadOf(const IR::Value& v) {
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
    return Read{i->Arg(0).U32(), i->Arg(1).U32()};
}

// The value that `handle`'s dword `dword` multiplies (FPMul32, or an FPFma32's product).
std::optional<IR::Value> Factor(IR::Block* block, u32 handle, u32 dword) {
    for (IR::Inst& inst : block->Instructions()) {
        const auto op = inst.GetOpcode();
        if (op != IR::Opcode::FPMul32 && op != IR::Opcode::FPFma32) {
            continue;
        }
        for (u32 a = 0; a < 2; ++a) {
            const auto r = ReadOf(inst.Arg(a));
            if (r && r->handle == handle && r->dword == dword) {
                return inst.Arg(1 - a);
            }
        }
    }
    return std::nullopt;
}

} // namespace

void LightShadowPass(IR::Program& program, const RuntimeInfo& runtime_info) {
    auto& info = program.info;
    if (info.stage != Stage::Fragment || program.blocks.empty()) {
        return;
    }
    // A light the game shadows itself (its own shadow map, compared: the cube-shadowed point
    // lights): left as it is (2026-11-12: ours was added on top, on light 0 only).
    if (!LightShadow::OldShadows()) {
        for (IR::Block* block : program.blocks) {
            for (IR::Inst& inst : block->Instructions()) {
                const auto op = inst.GetOpcode();
                if (op == IR::Opcode::ImageSampleDrefImplicitLod ||
                    op == IR::Opcode::ImageSampleDrefExplicitLod ||
                    op == IR::Opcode::ImageGatherDref) {
                    LOG_INFO(Render_Recompiler,
                             "VR light shadows: shader {:#x} left as it is (the game shadows its "
                             "lights itself)",
                             info.pgm_hash);
                    return;
                }
            }
        }
    }
    // Every immediate constant buffer read, by handle and dword.
    std::map<u32, std::map<u32, std::vector<IR::Inst*>>> reads;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::ReadConstBuffer && inst.Arg(0).IsImmediate() &&
                inst.Arg(1).IsImmediate()) {
                reads[inst.Arg(0).U32()][inst.Arg(1).U32()].push_back(&inst);
            }
        }
    }
    const auto has = [&](u32 handle, u32 dword) {
        const auto it = reads.find(handle);
        return it != reads.end() && it->second.contains(dword);
    };
    IR::Block* const entry = program.blocks.front();
    for (const auto& [matrix, matrix_reads] : reads) {
        bool rows = true;
        for (u32 d = 0; d < 16; ++d) {
            rows &= matrix_reads.contains(d);
        }
        if (!rows) {
            continue;
        }
        const auto ndc_x = Factor(entry, matrix, 12);
        const auto ndc_y = Factor(entry, matrix, 13);
        const auto depth = Factor(entry, matrix, 14);
        if (!ndc_x || !ndc_y || !depth || depth->IsImmediate() || ndc_x->IsImmediate() ||
            ndc_y->IsImmediate()) {
            continue;
        }
        // The depth: an image sample's x; its coordinates are the pixel's.
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
        const IR::Value uv = sample->Arg(1);
        // The lights: another buffer with k's position and colours.
        for (const auto& [lights, light_reads] : reads) {
            if (lights == matrix) {
                continue;
            }
            u32 count = 0;
            while (count < 16 && has(lights, 16 * count) && has(lights, 16 * count + 1) &&
                   has(lights, 16 * count + 2) && has(lights, 16 * count + 4) &&
                   has(lights, 16 * count + 5) && has(lights, 16 * count + 6)) {
                ++count;
            }
            if (count == 0) {
                continue;
            }
            // Every light block reads the same dwords (a spot light's second block is its cone).
            const auto block = [&](u32 k) {
                std::set<u32> used;
                for (const auto& [dword, list] : light_reads) {
                    if (dword >= 16 * k && dword < 16 * k + 16) {
                        used.insert(dword - 16 * k);
                    }
                }
                return used;
            };
            bool same = true;
            for (u32 k = 1; k < count; ++k) {
                same &= block(k) == block(0);
            }
            // Spot lights (2026-11-12, the user: "let's get this fixed too"): 32 dwords a light,
            // its block (position at 0..2, reach at 3, colours at 4..6 and 8..10, as a point
            // light's) then its cone's (the view-to-spot rows); the game's cone falloff stays.
            u32 per_light = 1;
            if (!same) {
                bool spots = count % 2 == 0 && block(1) != block(0);
                for (u32 k = 2; spots && k < count; ++k) {
                    spots = block(k) == block(k % 2);
                }
                if (!spots) {
                    LOG_INFO(Render_Recompiler,
                             "VR light shadows: shader {:#x} left as it is (its light blocks differ)",
                             info.pgm_hash);
                    return;
                }
                per_light = 2;
                count /= 2;
            }
            // The game's own shadowed lamps (GXLightAcc LegacyDPointLightShadowed1..4): each lamp
            // has a shadow map, a 2D array image (a cube's six faces). Their records are 24
            // dwords (the first 16 a point light's, then the cube's), so the 16-dword blocks
            // above found only the first lamp: the others by 24 (2026-11-12 night: the
            // game marks them "shadowed", ours shadow every one of them, its maps go).
            std::vector<IR::Inst*> shadow_samples;
            for (IR::Block* b : program.blocks) {
                for (IR::Inst& inst : b->Instructions()) {
                    const auto op = inst.GetOpcode();
                    if ((op == IR::Opcode::ImageSampleImplicitLod ||
                         op == IR::Opcode::ImageSampleExplicitLod) &&
                        inst.Arg(0).IsImmediate() && &inst != sample) {
                        // An array image (a cube's faces as layers): its resource says so, or
                        // its coordinates have the layer as a third component (the instruction's
                        // own flag is not always set for them).
                        const u32 image = inst.Arg(0).U32() & 0xFFFFu;
                        const bool layered =
                            (image < info.images.size() && info.images[image].is_array) ||
                            inst.Flags<IR::TextureInstInfo>().is_array ||
                            (!inst.Arg(1).IsImmediate() && inst.Arg(1).Type() == IR::Type::F32x3);
                        if (layered) {
                            shadow_samples.push_back(&inst);
                        }
                    }
                }
            }
            const bool game_shadowed = !shadow_samples.empty();
            u32 stride = 16 * per_light;
            if (game_shadowed && count == 1) {
                u32 n = 0;
                while (n < 4 && has(lights, 24 * n) && has(lights, 24 * n + 1) &&
                       has(lights, 24 * n + 2) && has(lights, 24 * n + 4) &&
                       has(lights, 24 * n + 5) && has(lights, 24 * n + 6)) {
                    ++n;
                }
                if (n >= 2) {
                    stride = 24;
                    count = n;
                }
            }
            // Every input must be in the entry block (they then dominate every colour use).
            std::vector<const IR::Inst*> inputs{extract, sample};
            for (const IR::Value* v : {&*ndc_x, &*ndc_y, &uv}) {
                if (!v->IsImmediate()) {
                    inputs.push_back(v->InstRecursive());
                }
            }
            if (std::ranges::any_of(inputs,
                                    [&](const IR::Inst* i) { return i->GetParent() != entry; })) {
                LOG_INFO(Render_Recompiler,
                         "VR light shadows: shader {:#x} left as it is (inputs outside the entry)",
                         info.pgm_hash);
                return;
            }
            // The hunter's capsules (body_shadow.h): declared in every permutation of a lamp
            // program (they share one Info, so one descriptor layout), read only with the lamp
            // shadows on.
            if (std::ranges::find(info.buffers, BufferType::VrBodyCapsules,
                                  &BufferResource::buffer_type) == info.buffers.end()) {
                info.buffers.push_back({
                    .used_types = IR::Type::U32,
                    .inline_cbuf = AmdGpu::Buffer::Placeholder(BodyShadow::BufferDwords * sizeof(u32)),
                    .buffer_type = BufferType::VrBodyCapsules,
                });
            }
            // ... and his body maps (vr_body_maps.h: MaxMaps maps of MapTexels a side). (With the
            // shadow mask the AO pass reads them and the grid for every lamp; since 2026-11-17 the
            // lamp programs declare them and the grid all the same: High's hero lamps march here
            // (light_shadow.h High), and Low and High keep one descriptor layout a program.)
            if (std::ranges::find(info.buffers, BufferType::VrBodyMaps,
                                  &BufferResource::buffer_type) == info.buffers.end()) {
                info.buffers.push_back({
                    .used_types = IR::Type::U32,
                    .inline_cbuf = AmdGpu::Buffer::Placeholder(
                        BodyShadow::MaxMaps * BodyShadow::MapTexels * BodyShadow::MapTexels *
                        sizeof(u32)),
                    .buffer_type = BufferType::VrBodyMaps,
                });
            }
            // ... and the blocker grid (light_shadow.h Grid), which they fill and read (with the
            // mask: read only, by High's hero lamps; the AO pass fills it).
            if (LightShadow::Grid::Enabled() &&
                std::ranges::find(info.buffers, BufferType::VrShadowGrid,
                                  &BufferResource::buffer_type) == info.buffers.end()) {
                info.buffers.push_back({
                    .used_types = IR::Type::U32,
                    .inline_cbuf =
                        AmdGpu::Buffer::Placeholder(LightShadow::Grid::BufferWords * sizeof(u32)),
                    .buffer_type = BufferType::VrShadowGrid,
                    .is_written = !LightShadow::SceneMaps::Mask(),
                });
            }
            // ... and the scene maps (light_shadow.h SceneMaps), which they read.
            if (LightShadow::SceneMaps::Enabled() && !LightShadow::SceneMaps::Mask() &&
                std::ranges::find(info.buffers, BufferType::VrSceneMaps,
                                  &BufferResource::buffer_type) == info.buffers.end()) {
                info.buffers.push_back({
                    .used_types = IR::Type::U32,
                    .inline_cbuf = AmdGpu::Buffer::Placeholder(LightShadow::SceneMaps::Texels *
                                                               sizeof(u32)),
                    .buffer_type = BufferType::VrSceneMaps,
                });
            }
            if (LightShadow::SceneMaps::Mask() &&
                std::ranges::find(info.buffers, BufferType::VrShadowMask,
                                  &BufferResource::buffer_type) == info.buffers.end()) {
                info.buffers.push_back({
                    .used_types = IR::Type::U32,
                    .inline_cbuf = AmdGpu::Buffer::Placeholder(1024 * sizeof(u32)),
                    .buffer_type = BufferType::VrShadowMask,
                });
            }
            if (!runtime_info.vr_light_shadows) {
                return;
            }
            // After the last of them.
            auto at = entry->Instructions().begin();
            for (auto it = entry->Instructions().begin(); it != entry->Instructions().end();
                 ++it) {
                if (std::ranges::find(inputs, &*it) != inputs.end()) {
                    at = std::next(it);
                }
            }
            IR::IREmitter ir{*entry, at};
            const IR::Value ndc = ir.CompositeConstruct(IR::F32{*ndc_x}, IR::F32{*ndc_y});
            std::vector<IR::F32> visibility;
            // (The light's first dword is 8 times this: a point's 16 k, a spot's 32 k, a shadowed
            // lamp's 24 k.)
            for (u32 k = 0; k < count; ++k) {
                visibility.push_back(
                    ir.VrLightVisibility(ir.Imm32(k * stride / 8), ndc, uv, IR::F32{*depth}));
            }
            // Tests (LightShadow::SceneMaps::Tint): a visibility a channel, the channel in the
            // light immediate's high bits; colour dword c takes channel (c - 4) % 4.
            const bool tint = LightShadow::SceneMaps::Tint() && LightShadow::SceneMaps::Mask();
            std::vector<std::array<IR::F32, 3>> tinted;
            if (tint) {
                for (u32 k = 0; k < count; ++k) {
                    std::array<IR::F32, 3> v{};
                    for (u32 ch = 0; ch < 3; ++ch) {
                        v[ch] = ir.VrLightVisibility(
                            ir.Imm32(k * stride / 8 + ((ch + 1) << LightShadow::SceneMaps::TintChannelShift)), ndc, uv,
                            IR::F32{*depth});
                    }
                    tinted.push_back(v);
                }
            }
            IR::Inst* const last_added = tint ? tinted.back()[2].InstRecursive() : visibility.back().InstRecursive();
            // Each colour read's float uses get light k's visibility.
            u32 scaled = 0;
            for (u32 k = 0; k < count; ++k) {
                for (const u32 c : {4u, 5u, 6u, 8u, 9u, 10u}) {
                    const auto it = light_reads.find(stride * k + c);
                    if (it == light_reads.end()) {
                        continue;
                    }
                    for (IR::Inst* read : it->second) {
                        std::vector<IR::Inst*> casts;
                        for (const auto& use : read->Uses()) {
                            if (use.user->GetOpcode() == IR::Opcode::BitCastF32U32) {
                                casts.push_back(use.user);
                            }
                        }
                        for (IR::Inst* cast : casts) {
                            // After both the cast and the visibility (all in the entry block,
                            // or the cast's own block, which the entry dominates).
                            IR::Block* const block = cast->GetParent();
                            auto pos = std::next(IR::Block::InstructionList::s_iterator_to(*cast));
                            if (block == entry) {
                                for (IR::Inst& i : entry->Instructions()) {
                                    if (&i == last_added) {
                                        break;
                                    }
                                    if (&i == cast) {
                                        // The cast comes first: after the visibility instead.
                                        pos = std::next(
                                            IR::Block::InstructionList::s_iterator_to(*last_added));
                                        break;
                                    }
                                }
                            }
                            IR::IREmitter at_cast{*block, pos};
                            const IR::F32 lit = at_cast.FPMul(IR::F32{IR::Value{cast}},
                                                              tint ? tinted[k][(c - 4) % 4] : visibility[k]);
                            IR::Inst* const mul = lit.InstRecursive();
                            std::vector<std::pair<IR::Inst*, u32>> users;
                            for (const auto& use : cast->Uses()) {
                                if (use.user != mul) {
                                    users.emplace_back(use.user, use.operand);
                                }
                            }
                            for (const auto& [user, operand] : users) {
                                user->SetArg(operand, lit);
                            }
                            ++scaled;
                        }
                    }
                }
            }
            // The game's shadow maps read as lit: a variance shadow map's moments (x, y) taken
            // as far beyond anything (x >= the pixel's depth: its Chebyshev bound is 1).
            u32 unshadowed = 0;
            for (IR::Inst* shadow : shadow_samples) {
                std::vector<std::pair<IR::Inst*, f32>> moments;
                for (const auto& use : shadow->Uses()) {
                    IR::Inst* const user = use.user;
                    if (user->GetOpcode() == IR::Opcode::CompositeExtractF32x4 &&
                        user->Arg(1).IsImmediate() && user->Arg(1).U32() < 2) {
                        moments.emplace_back(user, user->Arg(1).U32() == 0 ? 1e9f : 2e18f);
                    }
                }
                // (Each use rewritten in place: ReplaceUsesWith would leave an Identity, which
                // this late in the passes nothing removes and the emitter cannot take.)
                for (const auto& [extract_inst, value] : moments) {
                    std::vector<std::pair<IR::Inst*, u32>> users;
                    for (const auto& use : extract_inst->Uses()) {
                        users.emplace_back(use.user, use.operand);
                    }
                    for (const auto& [user, operand] : users) {
                        user->SetArg(operand, IR::Value{value});
                    }
                    ++unshadowed;
                }
            }
            info.vr_light_shadow = {true, matrix, lights, sample->Arg(0).U32(), game_shadowed,
                                    count, stride};
            LOG_INFO(Render_Recompiler,
                     "VR light shadows: shader {:#x}: {} {} light(s), {} colour reads "
                     "(matrix {}, lights {}, depth image {:#x}){}",
                     info.pgm_hash, count,
                     per_light == 2 ? "spot" : stride == 24 ? "game-shadowed" : "point", scaled,
                     matrix, lights, sample->Arg(0).U32(),
                     game_shadowed ? fmt::format(", the game's {} shadow lookups lit ({} moments)",
                                                 shadow_samples.size(), unshadowed)
                                   : std::string{});
            return;
        }
    }
}

} // namespace Shader::Optimization

namespace Shader::Optimization {

// Ambient occlusion (ambient_occlusion.h): the game's AO pass (RuntimeInfo::vr_ambient_occlusion,
// set for its programs only) writes VrAmbientOcclusion to its red channel; recognized from IR as
// the light shadows' view position (matrix rows 12, 13, 14 times ndc x, y and an image sample's
// x in the entry block) and a red channel beside a packed-depth green (a fraction).
void AmbientOcclusionPass(IR::Program& program, const RuntimeInfo& runtime_info) {
    auto& info = program.info;
    if (info.stage != Stage::Fragment ||
        (runtime_info.vr_ambient_occlusion & RuntimeInfo::VrAmbientOcclusionBit) == 0 ||
        program.blocks.empty()) {
        return;
    }
    // Our AO off, the lamp shadows' mask on (RuntimeInfo::VrAoMaskOnly): the mask only.
    const bool mask_only = (runtime_info.vr_ambient_occlusion & RuntimeInfo::VrAoMaskOnly) != 0;
    if (mask_only && !LightShadow::SceneMaps::Mask()) {
        return;
    }
    IR::Block* const entry = program.blocks.front();
    // Red and green of render target 0.
    IR::Inst* red = nullptr;
    IR::Inst* green = nullptr;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::SetAttribute &&
                inst.Arg(0).Attribute() == IR::Attribute::RenderTarget0 &&
                inst.Arg(2).IsImmediate()) {
                const u32 component = inst.Arg(2).U32();
                if (component == 0) {
                    red = &inst;
                } else if (component == 1) {
                    green = &inst;
                }
            }
        }
    }
    if (!red || !green || green->Arg(1).IsImmediate() ||
        green->Arg(1).InstRecursive()->GetOpcode() != IR::Opcode::FPFract32) {
        LOG_INFO(Render_Recompiler, "VR ambient occlusion: shader {:#x} left as it is (outputs)",
                 info.pgm_hash);
        return;
    }
    // The view position's matrix: a buffer read at 0..15 whose rows 12, 13, 14 take ndc and depth.
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
        const IR::Value uv = sample->Arg(1);
        std::vector<const IR::Inst*> inputs{extract, sample};
        for (const IR::Value* v : {&*ndc_x, &*ndc_y, &uv}) {
            if (!v->IsImmediate()) {
                inputs.push_back(v->InstRecursive());
            }
        }
        if (std::ranges::any_of(inputs,
                                [&](const IR::Inst* i) { return i->GetParent() != entry; })) {
            LOG_INFO(Render_Recompiler,
                     "VR ambient occlusion: shader {:#x} left as it is (inputs outside the entry)",
                     info.pgm_hash);
            return;
        }
        auto at = entry->Instructions().begin();
        for (auto it = entry->Instructions().begin(); it != entry->Instructions().end(); ++it) {
            if (std::ranges::find(inputs, &*it) != inputs.end()) {
                at = std::next(it);
            }
        }
        // The scene maps (light_shadow.h SceneMaps): this pass writes them (EmitVrAmbientOcclusion),
        // from the slots in the body buffer's header.
        if (LightShadow::SceneMaps::Enabled()) {
            if (std::ranges::find(info.buffers, BufferType::VrBodyCapsules,
                                  &BufferResource::buffer_type) == info.buffers.end()) {
                info.buffers.push_back({
                    .used_types = IR::Type::U32,
                    .inline_cbuf = AmdGpu::Buffer::Placeholder(BodyShadow::BufferDwords * sizeof(u32)),
                    .buffer_type = BufferType::VrBodyCapsules,
                });
            }
            if (std::ranges::find(info.buffers, BufferType::VrSceneMaps,
                                  &BufferResource::buffer_type) == info.buffers.end()) {
                info.buffers.push_back({
                    .used_types = IR::Type::U32,
                    .inline_cbuf = AmdGpu::Buffer::Placeholder(LightShadow::SceneMaps::Texels *
                                                               sizeof(u32)),
                    .buffer_type = BufferType::VrSceneMaps,
                    .is_written = true,
                });
            }
            // ... and the blocker grid (stage 2: its voxels into the cubes; with the mask its
            // upkeep too, written).
            if (LightShadow::SceneMaps::GridSplat() && LightShadow::Grid::Enabled() &&
                std::ranges::find(info.buffers, BufferType::VrShadowGrid,
                                  &BufferResource::buffer_type) == info.buffers.end()) {
                info.buffers.push_back({
                    .used_types = IR::Type::U32,
                    .inline_cbuf =
                        AmdGpu::Buffer::Placeholder(LightShadow::Grid::BufferWords * sizeof(u32)),
                    .buffer_type = BufferType::VrShadowGrid,
                    .is_written = LightShadow::SceneMaps::Mask(),
                });
            }
            // ... and the shadow mask it writes, and his body maps (his shadow goes in it), and
            // the other casters (enemies and NPCs out of view).
            if (LightShadow::SceneMaps::Mask()) {
                if (std::ranges::find(info.buffers, BufferType::VrShadowCasters,
                                      &BufferResource::buffer_type) == info.buffers.end()) {
                    info.buffers.push_back({
                        .used_types = IR::Type::U32,
                        .inline_cbuf = AmdGpu::Buffer::Placeholder(LightShadow::SceneMaps::CasterDwords *
                                                                   sizeof(u32)),
                        .buffer_type = BufferType::VrShadowCasters,
                    });
                }
                if (std::ranges::find(info.buffers, BufferType::VrShadowMask,
                                      &BufferResource::buffer_type) == info.buffers.end()) {
                    info.buffers.push_back({
                        .used_types = IR::Type::U32,
                        .inline_cbuf = AmdGpu::Buffer::Placeholder(1024 * sizeof(u32)),
                        .buffer_type = BufferType::VrShadowMask,
                        .is_written = true,
                    });
                }
                if (BodyShadow::MapsEnabled() &&
                    std::ranges::find(info.buffers, BufferType::VrBodyMaps,
                                      &BufferResource::buffer_type) == info.buffers.end()) {
                    info.buffers.push_back({
                        .used_types = IR::Type::U32,
                        .inline_cbuf = AmdGpu::Buffer::Placeholder(
                            BodyShadow::MaxMaps * BodyShadow::MapTexels * BodyShadow::MapTexels *
                            sizeof(u32)),
                        .buffer_type = BufferType::VrBodyMaps,
                    });
                }
                // ... and the depth pyramid its march reads (light_shadow.h SceneMaps::Pyramid).
                if (LightShadow::SceneMaps::MarchMode() && LightShadow::SceneMaps::Pyramid() &&
                    std::ranges::find(info.buffers, BufferType::VrDepthPyramid,
                                      &BufferResource::buffer_type) == info.buffers.end()) {
                    info.buffers.push_back({
                        .used_types = IR::Type::U32,
                        .inline_cbuf = AmdGpu::Buffer::Placeholder(
                            LightShadow::SceneMaps::PyramidHeaderWords * sizeof(u32)),
                        .buffer_type = BufferType::VrDepthPyramid,
                    });
                }
            }
        }
        IR::IREmitter ir{*entry, at};
        const IR::Value ndc = ir.CompositeConstruct(IR::F32{*ndc_x}, IR::F32{*ndc_y});
        const IR::F32 ao = ir.VrAmbientOcclusion(ndc, uv, IR::F32{*depth});
        if (mask_only) {
            // (Ours gives 1: the game's AO, past the mask's writes.)
            IR::IREmitter at_red{*red->GetParent(), IR::Block::InstructionList::s_iterator_to(*red)};
            red->SetArg(1, at_red.FPMin(IR::F32{red->Arg(1)}, ao));
        } else {
            red->SetArg(1, ao);
        }
        info.vr_light_shadow = {true, matrix, 0, sample->Arg(0).U32()};
        // The game's normal buffer (ambient_occlusion.h BufferNormals): the one other image the
        // program samples whose x, y and z it reads (the depth gives x, the rotation noise x and
        // y), with the view's rotation among the matrix buffer's dwords.
        if (AmbientOcclusion::BufferNormals()) {
            bool rotation = true;
            for (const u32 first : AmbientOcclusion::RotationDwords) {
                for (u32 d = first; d < first + 3; ++d) {
                    rotation &= used.contains(d);
                }
            }
            std::map<u32, u32> read; // image handle -> components read (bits)
            for (IR::Block* block : program.blocks) {
                for (IR::Inst& inst : block->Instructions()) {
                    if (inst.GetOpcode() != IR::Opcode::CompositeExtractF32x4 || inst.Arg(0).IsImmediate() ||
                        !inst.Arg(1).IsImmediate()) {
                        continue;
                    }
                    const IR::Inst* const from = inst.Arg(0).InstRecursive();
                    if ((from->GetOpcode() == IR::Opcode::ImageSampleImplicitLod ||
                         from->GetOpcode() == IR::Opcode::ImageSampleExplicitLod) &&
                        from->Arg(0).IsImmediate()) {
                        read[from->Arg(0).U32()] |= 1u << inst.Arg(1).U32();
                    }
                }
            }
            u32 found = ~0u, count = 0;
            for (const auto& [handle, components] : read) {
                if (handle != sample->Arg(0).U32() && (components & 7u) == 7u) {
                    found = handle;
                    ++count;
                }
            }
            if (rotation && count == 1) {
                info.vr_light_shadow.normal_image = found;
            }
            LOG_INFO(Render_Recompiler,
                     "VR ambient occlusion: shader {:#x}: the game's normal buffer {} (rotation dwords {}, {} "
                     "image(s) read in x y z)",
                     info.pgm_hash,
                     info.vr_light_shadow.normal_image != ~0u ? fmt::format("is image {:#x}", found) : "not found",
                     rotation ? "read" : "not read", count);
        }
        LOG_INFO(Render_Recompiler,
                 "VR ambient occlusion: shader {:#x}: ours in red (matrix {}, depth image {:#x})",
                 info.pgm_hash, matrix, sample->Arg(0).U32());
        return;
    }
    LOG_INFO(Render_Recompiler, "VR ambient occlusion: shader {:#x} left as it is (no position)",
             info.pgm_hash);
}

} // namespace Shader::Optimization

namespace Shader::Optimization {

static void CinematicPass(IR::Program& program) {
    auto& info = program.info;
    if (info.stage != Stage::Fragment || program.blocks.empty() ||
        !CinematicLook::IsGradingProgram(info.pgm_hash)) {
        return;
    }
    if (std::ranges::find(info.buffers, BufferType::VrGrade, &BufferResource::buffer_type) ==
        info.buffers.end()) {
        info.buffers.push_back({
            .used_types = IR::Type::U32,
            .inline_cbuf = AmdGpu::Buffer::Placeholder(CinematicLook::BufferDwords * sizeof(u32)),
            .buffer_type = BufferType::VrGrade,
        });
    }
    std::array<IR::Inst*, 3> colour{};
    IR::Inst* last = nullptr;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::SetAttribute &&
                inst.Arg(0).Attribute() == IR::Attribute::RenderTarget0 &&
                inst.Arg(2).IsImmediate() && inst.Arg(2).U32() < 3) {
                colour[inst.Arg(2).U32()] = &inst;
                last = &inst;
            }
        }
    }
    if (std::ranges::any_of(colour, [](const IR::Inst* i) { return i == nullptr; }) ||
        std::ranges::any_of(colour, [&](const IR::Inst* i) {
            return i->GetParent() != last->GetParent();
        })) {
        LOG_INFO(Render_Recompiler, "VR cinematic look: shader {:#x} left as it is (outputs)",
                 info.pgm_hash);
        return;
    }
    IR::Block* const block = last->GetParent();
    IR::IREmitter ir{*block, std::next(IR::Block::InstructionList::s_iterator_to(*last))};
    const IR::Value graded = ir.VrCinematic(ir.CompositeConstruct(
        IR::F32{colour[0]->Arg(1)}, IR::F32{colour[1]->Arg(1)}, IR::F32{colour[2]->Arg(1)}));
    for (u32 c = 0; c < 3; ++c) {
        ir.SetAttribute(IR::Attribute::RenderTarget0, IR::F32{ir.CompositeExtract(graded, c)}, c);
    }
    LOG_INFO(Render_Recompiler, "VR cinematic look: shader {:#x} graded", info.pgm_hash);
}

// The tone map's floor max(ToneMapFloor, x) within `depth` steps up `v`'s arguments.
static IR::Inst* ToneMapFloorOf(const IR::Value& v, int depth) {
    if (v.IsImmediate() || depth < 0) {
        return nullptr;
    }
    IR::Inst* const inst = v.InstRecursive();
    if (inst->GetOpcode() == IR::Opcode::FPMax32) {
        for (size_t a = 0; a < 2; ++a) {
            const IR::Value arg = inst->Arg(a);
            if (arg.IsImmediate() && arg.Type() == IR::Type::F32 &&
                arg.F32() == CinematicLook::ToneMapFloor) {
                return inst;
            }
        }
    }
    if (inst->GetOpcode() == IR::Opcode::FPLog2) {
        return nullptr; // the gamma's exp2(g log2(t)) after the curve: not a channel's curve
    }
    for (size_t a = 0; a < inst->NumArgs(); ++a) {
        if (IR::Inst* const found = ToneMapFloorOf(inst->Arg(a), depth - 1)) {
            return found;
        }
    }
    return nullptr;
}

// Filmic highlights (cinematic_look.h): the tone map's three exponentials e = exp2(-k x log2 e)
// of the floored channels x, each one's uses moved to VrFilmic's.
static void FilmicPass(IR::Program& program, const RuntimeInfo& runtime_info) {
    auto& info = program.info;
    if (info.stage != Stage::Fragment || program.blocks.empty() ||
        !CinematicLook::IsToneMapProgram(info.pgm_hash)) {
        return;
    }
    std::array<IR::Inst*, 3> exps{};
    std::array<IR::Inst*, 3> floors{};
    u32 found = 0;
    bool extra = false;
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() != IR::Opcode::FPExp2) {
                continue;
            }
            IR::Inst* const floor = ToneMapFloorOf(inst.Arg(0), 4);
            if (!floor) {
                continue;
            }
            if (found == 3) {
                extra = true;
                continue;
            }
            exps[found] = &inst;
            floors[found] = floor;
            ++found;
        }
    }
    const auto same_block = [&] {
        return std::ranges::all_of(exps, [&](const IR::Inst* i) {
                   return i->GetParent() == exps[2]->GetParent();
               }) &&
               std::ranges::all_of(floors, [&](const IR::Inst* i) {
                   return i->GetParent() == exps[2]->GetParent();
               });
    };
    if (found != 3 || extra || floors[0] == floors[1] || floors[1] == floors[2] ||
        floors[0] == floors[2] || !same_block()) {
        LOG_INFO(Render_Recompiler,
                 "VR filmic highlights: shader {:#x} left as it is ({} channel curves{})",
                 info.pgm_hash, found, extra ? ", more than three" : "");
        return;
    }
    if (std::ranges::find(info.buffers, BufferType::VrGrade, &BufferResource::buffer_type) ==
        info.buffers.end()) {
        info.buffers.push_back({
            .used_types = IR::Type::U32,
            .inline_cbuf = AmdGpu::Buffer::Placeholder(CinematicLook::BufferDwords * sizeof(u32)),
            .buffer_type = BufferType::VrGrade,
        });
    }
    // Right after the last of its inputs (the three floors, the exponentials' arguments), which
    // must come before every use of an exponential (2026-11-11, the night's tone map 0xccb08339:
    // exp0's first use sits between the exponentials, so placing VrFilmic after the last
    // exponential used its result before defining it and the SPIR-V emitter crashed).
    IR::Block* const block = exps[2]->GetParent();
    const auto in_block = [&](const IR::Value& v) -> const IR::Inst* {
        if (v.IsImmediate()) {
            return nullptr;
        }
        const IR::Inst* const inst = v.InstRecursive();
        return inst->GetParent() == block ? inst : nullptr;
    };
    std::vector<const IR::Inst*> inputs{floors.begin(), floors.end()};
    for (const IR::Inst* exp : exps) {
        if (const IR::Inst* const arg = in_block(exp->Arg(0))) {
            inputs.push_back(arg);
        }
    }
    const auto is_exp = [&](const IR::Value& v) {
        return !v.IsImmediate() && std::ranges::find(exps, v.InstRecursive()) != exps.end();
    };
    auto at = block->Instructions().begin();
    bool have_inputs = false;
    for (auto it = block->Instructions().begin(); it != block->Instructions().end(); ++it) {
        if (std::ranges::find(inputs, &*it) != inputs.end()) {
            at = std::next(it);
            have_inputs = true;
        }
    }
    // No exponential's use before `at` (the exponentials themselves go).
    bool used_early = false;
    for (auto it = block->Instructions().begin(); it != at && !used_early; ++it) {
        for (size_t a = 0; a < it->NumArgs(); ++a) {
            used_early |= is_exp(it->Arg(a));
        }
    }
    if (!have_inputs || used_early) {
        LOG_INFO(Render_Recompiler,
                 "VR filmic highlights: shader {:#x} left as it is (a curve used before the "
                 "others are made)",
                 info.pgm_hash);
        return;
    }
    IR::IREmitter ir{*block, at};
    // Eye adaptation (cinematic_look.h): its state buffer in every permutation (one Info); the
    // exposure scales each exponential's argument (-k x: the scene light times the exposure).
    if (std::ranges::find(info.buffers, BufferType::VrAdaptation, &BufferResource::buffer_type) ==
        info.buffers.end()) {
        info.buffers.push_back({
            .used_types = IR::Type::U32,
            .inline_cbuf = AmdGpu::Buffer::Placeholder(CinematicLook::AdaptDwords * sizeof(u32)),
            .buffer_type = BufferType::VrAdaptation,
            .is_written = true,
        });
    }
    const bool adapt = (runtime_info.vr_ambient_occlusion & RuntimeInfo::VrEyeAdaptation) != 0;
    // Fresh copies of the exponentials feed VrFilmic (the originals' uses move to its result).
    std::array<IR::F32, 3> e;
    std::array<IR::F32, 3> x;
    for (u32 c = 0; c < 3; ++c) {
        x[c] = IR::F32{IR::Value{floors[c]}};
    }
    const IR::F32 exposure =
        adapt ? ir.VrExposure(ir.CompositeConstruct(x[0], x[1], x[2])) : ir.Imm32(1.0f);
    for (u32 c = 0; c < 3; ++c) {
        e[c] = adapt ? ir.FPExp2(IR::F32{ir.FPMul(IR::F32{exps[c]->Arg(0)}, exposure)})
                     : ir.FPExp2(IR::F32{exps[c]->Arg(0)});
    }
    const IR::Value filmic = ir.VrFilmic(ir.CompositeConstruct(x[0], x[1], x[2]),
                                         ir.CompositeConstruct(e[0], e[1], e[2]));
    for (u32 c = 0; c < 3; ++c) {
        exps[c]->ReplaceUsesWithAndRemove(ir.CompositeExtract(filmic, c));
    }
    LOG_INFO(Render_Recompiler, "VR filmic highlights: shader {:#x} tone map", info.pgm_hash);
}

// The light composite adds VrBounceLight to its colour when the bounce variant is enabled.
void BouncePass(IR::Program& program, const RuntimeInfo& runtime_info) {
    CinematicPass(program);
    FilmicPass(program, runtime_info);
    auto& info = program.info;
    if (info.stage != Stage::Fragment || program.blocks.empty()) {
        return;
    }
    // The lights in the air's buffer (fog_light.h): declared for every permutation of the
    // composite, ours or not, VR or not (the permutations of a program share one Info and so one
    // descriptor layout, as LowerUserClipPlanes' clip planes); only VrFogLight reads it.
    if (BounceLight::IsCompositeProgram(info.pgm_hash) &&
        std::ranges::find(info.buffers, BufferType::VrFogLights, &BufferResource::buffer_type) ==
            info.buffers.end()) {
        info.buffers.push_back({
            .used_types = IR::Type::U32,
            .inline_cbuf = AmdGpu::Buffer::Placeholder(FogLight::BufferDwords * sizeof(u32)),
            .buffer_type = BufferType::VrFogLights,
        });
    }
    // The moonlight in the air's (moon_shafts.h) buffer and the moon's shadow atlas (a host image:
    // the rasterizer's T# from the shadow pass), declared the same way.
    if (BounceLight::IsCompositeProgram(info.pgm_hash)) {
        if (std::ranges::find(info.buffers, BufferType::VrMoonShafts,
                              &BufferResource::buffer_type) == info.buffers.end()) {
            info.buffers.push_back({
                .used_types = IR::Type::U32,
                .inline_cbuf = AmdGpu::Buffer::Placeholder(MoonShafts::BufferDwords * sizeof(u32)),
                .buffer_type = BufferType::VrMoonShafts,
            });
        }
        const u32 atlas = VrHostImageBase + static_cast<u32>(VrHostImage::MoonAtlas);
        if (std::ranges::find(info.images, atlas, &ImageResource::sharp_idx) == info.images.end()) {
            info.images.push_back({
                .sharp_idx = atlas,
                .is_depth = true,
            });
        }
        // And the directional light's light-set table (a host buffer: that pass's own guest
        // buffer, written by the GPU), for the moon's colour.
        // The air pass's colour probe (air_light.h): its sums.
        if (std::ranges::find(info.buffers, BufferType::VrAirProbe,
                              &BufferResource::buffer_type) == info.buffers.end()) {
            info.buffers.push_back({
                .used_types = IR::Type::U32,
                .inline_cbuf = AmdGpu::Buffer::Placeholder(AirLight::ProbeSlots * AirLight::ProbeSlotDwords *
                                                           sizeof(u32)),
                .buffer_type = BufferType::VrAirProbe,
                .is_written = true,
            });
        }
        const u32 sets = VrHostBufferBase + static_cast<u32>(VrHostBuffer::MoonSets);
        if (std::ranges::none_of(info.buffers, [&](const BufferResource& buffer) {
                return buffer.IsVrHost() && buffer.sharp_idx == sets;
            })) {
            info.buffers.push_back({
                .sharp_idx = sets,
                .used_types = IR::Type::U32,
                .buffer_type = BufferType::Guest,
            });
        }
    }
    const bool bounce = (runtime_info.vr_composite_light & RuntimeInfo::VrCompositeBounce) != 0;
    const bool fog = (runtime_info.vr_composite_light & RuntimeInfo::VrCompositeFog) != 0;
    const bool shafts = (runtime_info.vr_composite_light & RuntimeInfo::VrCompositeShafts) != 0;
    if (!bounce && !fog && !shafts) {
        return;
    }
    IR::Block* const entry = program.blocks.front();
    std::array<IR::Inst*, 3> colour{};
    for (IR::Block* block : program.blocks) {
        for (IR::Inst& inst : block->Instructions()) {
            if (inst.GetOpcode() == IR::Opcode::SetAttribute &&
                inst.Arg(0).Attribute() == IR::Attribute::RenderTarget0 &&
                inst.Arg(2).IsImmediate() && inst.Arg(2).U32() < 3) {
                colour[inst.Arg(2).U32()] = &inst;
            }
        }
    }
    if (std::ranges::any_of(colour, [](const IR::Inst* i) { return i == nullptr; })) {
        LOG_INFO(Render_Recompiler, "VR bounce light: shader {:#x} left as it is (outputs)",
                 info.pgm_hash);
        return;
    }
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
        if (!ndc_x || !ndc_y || !depth || depth->IsImmediate() || ndc_x->IsImmediate() ||
            ndc_y->IsImmediate()) {
            continue;
        }
        IR::Inst* const extract = depth->InstRecursive();
        if (extract->GetOpcode() != IR::Opcode::CompositeExtractF32x4 ||
            extract->Arg(0).IsImmediate()) {
            continue;
        }
        IR::Inst* const sample = extract->Arg(0).InstRecursive();
        if ((sample->GetOpcode() != IR::Opcode::ImageSampleImplicitLod &&
             sample->GetOpcode() != IR::Opcode::ImageSampleExplicitLod) ||
            !sample->Arg(0).IsImmediate() ||
            sample->Arg(0).U32() != BounceLight::DepthImage) {
            continue;
        }
        const IR::Value uv = sample->Arg(1);
        std::vector<const IR::Inst*> inputs{extract, sample};
        // The moon's light sets (moon_shafts.h): the image whose w the composite reads as a
        // light-set id (255 * w, then times 6, its own table's stride); its sample handle, or
        // ~0 when there is none.
        u32 sets_image = ~0u;
        if (shafts) {
            for (IR::Inst& inst : entry->Instructions()) {
                if (inst.GetOpcode() != IR::Opcode::FPMul32 || sets_image != ~0u) {
                    continue;
                }
                for (u32 k = 0; k < 2; ++k) {
                    const IR::Value a = inst.Arg(k), b = inst.Arg(1 - k);
                    if (!a.IsImmediate() || a.F32() != 255.0f || b.IsImmediate()) {
                        continue;
                    }
                    IR::Inst* const w = b.InstRecursive();
                    if (w->GetOpcode() != IR::Opcode::CompositeExtractF32x4 ||
                        !w->Arg(1).IsImmediate() || w->Arg(1).U32() != 3 ||
                        w->Arg(0).IsImmediate()) {
                        continue;
                    }
                    const IR::Inst* const s = w->Arg(0).InstRecursive();
                    if ((s->GetOpcode() == IR::Opcode::ImageSampleImplicitLod ||
                         s->GetOpcode() == IR::Opcode::ImageSampleExplicitLod) &&
                        s->Arg(0).IsImmediate()) {
                        sets_image = s->Arg(0).U32();
                        break;
                    }
                }
            }
        }
        for (const IR::Value* v : {&*ndc_x, &*ndc_y, &uv}) {
            if (!v->IsImmediate()) {
                inputs.push_back(v->InstRecursive());
            }
        }
        if (std::ranges::any_of(inputs,
                                [&](const IR::Inst* i) { return i->GetParent() != entry; })) {
            LOG_INFO(Render_Recompiler,
                     "VR bounce light: shader {:#x} left as it is (inputs outside the entry)",
                     info.pgm_hash);
            return;
        }
        auto at = entry->Instructions().begin();
        for (auto it = entry->Instructions().begin(); it != entry->Instructions().end(); ++it) {
            if (std::ranges::find(inputs, &*it) != inputs.end()) {
                at = std::next(it);
            }
        }
        IR::IREmitter ir{*entry, at};
        const IR::Value ndc = ir.CompositeConstruct(IR::F32{*ndc_x}, IR::F32{*ndc_y});
        std::vector<IR::Value> added;
        if (bounce) {
            added.push_back(ir.VrBounceLight(ndc, uv, IR::F32{*depth}));
        }
        if (fog) {
            added.push_back(ir.VrFogLight(ndc, uv, IR::F32{*depth}));
        }
        // The moonlight in the air also dims what lies behind it: its w is the air's
        // transmittance minus one (colour * (1 + w) + rgb).
        const std::optional<IR::Value> moon =
            !shafts ? std::nullopt
            : AirLight::Pass()
                ? std::optional{ir.VrAirProbe(ndc, uv, IR::F32{*depth}, ir.Imm32(sets_image))}
                : std::optional{ir.VrMoonShafts(ndc, uv, IR::F32{*depth}, ir.Imm32(sets_image))};
        for (u32 c = 0; c < 3; ++c) {
            IR::Inst* const out = colour[c];
            IR::IREmitter at_out{*out->GetParent(), IR::Block::InstructionList::s_iterator_to(*out)};
            IR::F32 lit{out->Arg(1)};
            for (const IR::Value& light : added) {
                lit = at_out.FPAdd(lit, IR::F32{at_out.CompositeExtract(light, c)});
            }
            if (moon) {
                const IR::F32 dim{at_out.CompositeExtract(*moon, 3)};
                lit = at_out.FPAdd(at_out.FPAdd(lit, at_out.FPMul(lit, dim)),
                                   IR::F32{at_out.CompositeExtract(*moon, c)});
            }
            out->SetArg(1, lit);
        }
        info.vr_light_shadow = {true, matrix, 0, sample->Arg(0).U32()};
        LOG_INFO(Render_Recompiler,
                 "VR composite light: shader {:#x}: {}{}{}{} (matrix {})", info.pgm_hash,
                 bounce ? "one bounce " : "", fog ? "lights in the air " : "",
                 shafts ? "moonlight in the air" : "",
                 shafts && sets_image == ~0u ? " (no light-set id: no moon colour)" : "",
                 matrix);
        return;
    }
    LOG_INFO(Render_Recompiler, "VR bounce light: shader {:#x} left as it is (no position)",
             info.pgm_hash);
}

} // namespace Shader::Optimization
