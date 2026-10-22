// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Scratch prerequisite for ordered draw recording on a separate thread.
#pragma once
#include "input_scope_gate.h"
#include <array>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <span>
#include <vector>

namespace ScratchDraw {
struct RegisterRead {
  std::uint32_t pointer_index{}, dword_offset{};
  std::array<std::uint32_t, 4> words{};
};
struct Inputs {
  const void *owner{};
  std::span<const std::uint32_t> user;
  std::span<const std::uint32_t> flat;
  std::uint64_t program_base{};
  std::span<const RegisterRead> register_reads;
};
// The packet owns the words until the recording worker consumes it. Never copy
// the live Shader::Info vector/span from the consumer thread.
struct OwnedInputs {
  const void *owner{};
  std::vector<std::uint32_t> user, flat;
  std::uint64_t program_base{};
  void Capture(const void *key, std::span<const std::uint32_t> ud,
               std::span<const std::uint32_t> flattened, std::uint64_t base) {
    owner = key;
    user.assign(ud.begin(), ud.end());
    flat.assign(flattened.begin(), flattened.end());
    program_base = base;
  }
  Inputs View() const { return {owner, user, flat, program_base}; }
};
inline ScopeUseGate input_scope_gate;
inline thread_local std::span<const Inputs> active_inputs;
inline const Inputs *Find(const void *owner) noexcept {
  if (input_scope_gate.CanSkip())
    return nullptr;
  for (const auto &item : active_inputs)
    if (item.owner == owner)
      return &item;
  return nullptr;
}
class Scope {
  std::span<const Inputs> previous;

public:
  explicit Scope(std::span<const Inputs> inputs) : previous(active_inputs) {
    for (std::size_t i = 0; i < inputs.size(); ++i) {
      assert(inputs[i].owner);
      for (std::size_t j = 0; j < i; ++j)
        assert(inputs[j].owner != inputs[i].owner);
    }
    input_scope_gate.MarkUsed();
    active_inputs = inputs;
  }
  ~Scope() { active_inputs = previous; }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
};
} // namespace ScratchDraw
