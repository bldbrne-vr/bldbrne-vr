// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Scratch speculative scanner: register effects only; never execute guest writes or waits.
#pragma once
#include "video_core/amdgpu/regs.h"
#include "video_core/amdgpu/pm4_cmds.h"
#include <span>
namespace ScratchDraw {
template<class Mark>
inline bool ReplayGraphicsPacket(AmdGpu::Regs& regs, std::span<const u32> packet,
                                 size_t& words, Mark mark) {
    using namespace AmdGpu;
    words = 0;
    if (packet.empty()) return false;
    const auto* header = reinterpret_cast<const PM4Header*>(packet.data());
    if (header->type == 2) { words = 1; return true; }
    if (header->type != 3) return false;
    const size_t count = header->type3.NumWords();
    if (count == 0) return false;
    words = count + 1;
    if (words > packet.size()) return false;
    const auto fits = [&](size_t size) { return size <= words * sizeof(u32); };
    const auto field = [&](const auto& value) {
        const auto first = (reinterpret_cast<const char*>(&value) -
                            reinterpret_cast<const char*>(&regs)) / sizeof(u32);
        mark(first, (sizeof(value) + sizeof(u32) - 1) / sizeof(u32));
    };
    switch (header->type3.opcode) {
    case PM4ItOpcode::ClearState: regs.SetDefaults(); mark(0, Regs::NumRegs); break;
    case PM4ItOpcode::SetConfigReg:
    case PM4ItOpcode::SetContextReg:
    case PM4ItOpcode::SetShReg:
    case PM4ItOpcode::SetUconfigReg: {
        if (!fits(sizeof(PM4CmdSetData))) return false;
        const auto* data = reinterpret_cast<const PM4CmdSetData*>(header);
        u32 base = Regs::ConfigRegWordOffset;
        if (header->type3.opcode == PM4ItOpcode::SetContextReg) base = Regs::ContextRegWordOffset;
        if (header->type3.opcode == PM4ItOpcode::SetUconfigReg) base = Regs::UconfigRegWordOffset;
        if (header->type3.opcode == PM4ItOpcode::SetShReg) {
            if (data->reg_offset >= 0x200 && data->reg_offset <= 0x200 + sizeof(ComputeProgram) / 4)
                break; // Separate compute queue state, not graphics registers.
            base = Regs::ShRegWordOffset;
        }
        const size_t at = size_t(base) + data->reg_offset;
        if (at > Regs::NumRegs || count - 1 > Regs::NumRegs - at) return false;
        std::memcpy(regs.reg_array.data() + at, packet.data() + 2, (count - 1) * sizeof(u32));
        mark(at, count - 1);
        break;
    }
    case PM4ItOpcode::IndexType:
        if (!fits(sizeof(PM4CmdDrawIndexType))) return false;
        regs.index_buffer_type.raw = reinterpret_cast<const PM4CmdDrawIndexType*>(header)->raw;
        field(regs.index_buffer_type);
        break;
    case PM4ItOpcode::DrawIndex2: {
        if (!fits(sizeof(PM4CmdDrawIndex2))) return false;
        const auto& d = *reinterpret_cast<const PM4CmdDrawIndex2*>(header);
        regs.max_index_size = d.max_size;
        field(regs.max_index_size);
        regs.index_base_address.base_addr_lo = d.index_base_lo;
        field(regs.index_base_address);
        regs.index_base_address.base_addr_hi = d.index_base_hi;
        field(regs.index_base_address);
        regs.num_indices = d.index_count;
        field(regs.num_indices);
        regs.draw_initiator = d.draw_initiator;
        field(regs.draw_initiator);
        break;
    }
    case PM4ItOpcode::DrawIndexOffset2: {
        if (!fits(sizeof(PM4CmdDrawIndexOffset2))) return false;
        const auto& d = *reinterpret_cast<const PM4CmdDrawIndexOffset2*>(header);
        regs.max_index_size = d.max_size;
        field(regs.max_index_size);
        regs.num_indices = d.index_count;
        field(regs.num_indices);
        regs.draw_initiator = d.draw_initiator;
        field(regs.draw_initiator);
        break;
    }
    case PM4ItOpcode::DrawIndexAuto: {
        if (!fits(sizeof(PM4CmdDrawIndexAuto))) return false;
        const auto& d = *reinterpret_cast<const PM4CmdDrawIndexAuto*>(header);
        regs.num_indices = d.index_count;
        field(regs.num_indices);
        regs.draw_initiator = d.draw_initiator;
        field(regs.draw_initiator);
        break;
    }
    case PM4ItOpcode::NumInstances:
        if (!fits(sizeof(PM4CmdDrawNumInstances))) return false;
        regs.num_instances.num_instances = reinterpret_cast<const PM4CmdDrawNumInstances*>(header)->num_instances;
        field(regs.num_instances);
        break;
    case PM4ItOpcode::IndexBase: {
        if (!fits(sizeof(PM4CmdDrawIndexBase))) return false;
        const auto& d = *reinterpret_cast<const PM4CmdDrawIndexBase*>(header);
        regs.index_base_address.base_addr_lo = d.addr_lo;
        field(regs.index_base_address);
        regs.index_base_address.base_addr_hi = d.addr_hi;
        field(regs.index_base_address);
        break;
    }
    case PM4ItOpcode::IndexBufferSize:
        if (!fits(sizeof(PM4CmdDrawIndexBufferSize))) return false;
        regs.num_indices = reinterpret_cast<const PM4CmdDrawIndexBufferSize*>(header)->num_indices;
        field(regs.num_indices);
        break;
    case PM4ItOpcode::EventWrite:
        if (count < 1) return false;
        if (reinterpret_cast<const PM4CmdEventWrite*>(header)->event_type.Value() == EventType::SoVgtStreamoutFlush) {
            regs.cp_strmout_cntl.offset_update_done = 1;
            field(regs.cp_strmout_cntl);
        }
        break;
    case PM4ItOpcode::Nop: case PM4ItOpcode::ContextControl: case PM4ItOpcode::SetPredication:
    case PM4ItOpcode::SetBase: case PM4ItOpcode::AcquireMem: case PM4ItOpcode::DmaData:
    case PM4ItOpcode::WriteData: case PM4ItOpcode::CopyData: case PM4ItOpcode::MemSemaphore:
    case PM4ItOpcode::WaitRegMem: case PM4ItOpcode::EventWriteEop: case PM4ItOpcode::EventWriteEos:
    case PM4ItOpcode::IncrementDeCounter: case PM4ItOpcode::WaitOnCeCounter: case PM4ItOpcode::PfpSyncMe:
    case PM4ItOpcode::StrmoutBufferUpdate: case PM4ItOpcode::GetLodStats:
    case PM4ItOpcode::DispatchDirect: case PM4ItOpcode::DispatchIndirect:
    case PM4ItOpcode::DrawIndirect: case PM4ItOpcode::DrawIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirect: case PM4ItOpcode::DrawIndexIndirectMulti:
    case PM4ItOpcode::DrawIndexIndirectCountMulti:
        break; // No graphics register assignments in the normal decoder.
    default:
        // Nested buffers, conditional execution, rewinds and unknown commands need
        // separate starting-state/control-flow treatment. Never guess their effects.
        return false;
    }
    return true;
}
inline bool ReplayGraphicsPacket(AmdGpu::Regs& regs, std::span<const u32> packet, size_t& words) {
    return ReplayGraphicsPacket(regs, packet, words, [](size_t, size_t) {});
}
}
