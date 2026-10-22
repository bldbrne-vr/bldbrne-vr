// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Scratch prerequisite: recording reads a packet's state, never the decoder's
// live state.
#pragma once
#include "video_core/amdgpu/liverpool.h"
#include "input_scope_gate.h"
#include <cstdlib>
#include <memory>
#include <optional>

namespace ScratchDraw {
struct RegisterInputs {
  const AmdGpu::Liverpool *owner;
  AmdGpu::Regs regs;
  std::array<AmdGpu::CbDbExtent, AmdGpu::NUM_COLOR_BUFFERS> cb_extent;
  AmdGpu::CbDbExtent db_extent;
  AmdGpu::ComputeProgram compute;
  // FAST_CPU_SLIM_PACKET_VERIFY (stage B): the packet's slim form carried no
  // compute registers (a draw: `compute` would be the last dispatch's), and the
  // reads of them ComputeRegisters counted while it was consumed.
  bool compute_unsent{};
  mutable std::uint32_t compute_unsent_reads{};
  explicit RegisterInputs(AmdGpu::Liverpool &live)
      : owner(&live), regs(live.regs), cb_extent(live.last_cb_extent),
        db_extent(live.last_db_extent), compute(live.GetCsRegs()) {}
};
inline ScopeUseGate register_scope_gate;
inline thread_local const RegisterInputs *active_registers{};
class RegisterScope {
  const RegisterInputs *previous;

public:
  explicit RegisterScope(const RegisterInputs &inputs)
      : previous(active_registers) {
    register_scope_gate.MarkUsed();
    active_registers = &inputs;
  }
  ~RegisterScope() { active_registers = previous; }
  RegisterScope(const RegisterScope &) = delete;
  RegisterScope &operator=(const RegisterScope &) = delete;
};
inline const RegisterInputs *RegistersFor(const AmdGpu::Liverpool *owner) {
  if (register_scope_gate.CanSkip())
    return nullptr;
  return active_registers && active_registers->owner == owner ? active_registers
                                                              : nullptr;
}
inline const AmdGpu::Regs &Registers(AmdGpu::Liverpool *live) {
  if (const auto *packet = RegistersFor(live))
    return packet->regs;
  return live->regs;
}
inline const auto &ColorExtents(AmdGpu::Liverpool *live) {
  if (const auto *packet = RegistersFor(live))
    return packet->cb_extent;
  return live->last_cb_extent;
}
inline const auto &DepthExtent(AmdGpu::Liverpool *live) {
  if (const auto *packet = RegistersFor(live))
    return packet->db_extent;
  return live->last_db_extent;
}
inline const AmdGpu::ComputeProgram &ComputeRegisters(AmdGpu::Liverpool *live) {
  if (const auto *packet = RegistersFor(live)) {
    if (packet->compute_unsent) [[unlikely]]
      ++packet->compute_unsent_reads;
    return packet->compute;
  }
  return live->GetCsRegs();
}
// Serial validation only. The worker will reuse packet storage instead of
// allocating here.
class CapturedRegisters {
  std::unique_ptr<RegisterInputs> owned;
  std::optional<RegisterScope> scope;

public:
  explicit CapturedRegisters(AmdGpu::Liverpool *live) {
    static const bool enabled = [] {
      const char *value = std::getenv("VR_DRAW_REGISTER_SNAPSHOTS");
      return value && value[0] == '1';
    }();
    if (!enabled || RegistersFor(live))
      return;
    owned = std::make_unique<RegisterInputs>(*live);
    scope.emplace(*owned);
  }
};
} // namespace ScratchDraw
