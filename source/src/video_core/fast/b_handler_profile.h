// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
// Opt-in exclusive stage-B handler timings. No queue spin/sleep is included.
#pragma once
#include "common/rdtsc.h"
#include "lean_scopes.h"
#include <array>
#include <atomic>
#include <cstdlib>
namespace FastRender::BProfile {
enum Category : unsigned {
  Other,
  Replay,
  Resources,
  Buffers,
  NativePairing,
  VertexIndex,
  BeginRendering,
  Recording,
  OrderedTasks,
  RenderTargets,
  UploadCopy,
  DrawSetup,
  Maintenance,
  Reselect,
  Submit,
  Special,
  ComputeHle,
  Count
};
inline constexpr std::array<const char *, Count> names{
    "other",        "replay",     "resources",   "buffers",  "native",
    "vertex_index", "begin",      "recording",   "tasks",    "targets",
    "upload_copy",  "draw_setup", "maintenance", "reselect", "submit",
    "special",      "compute_hle"};
inline std::array<std::atomic<u64>, Count> totals{};
// Read once at start-up: a plain load per handler and per scope, no guard.
inline const bool g_enabled = [] {
  const char *v = std::getenv("FAST_B_PROFILE");
  return v && v[0] != '0';
}();
// FAST_CPU_LEAN_SCOPES: scopes and End read the thread's state only while profiling (it is never
// active otherwise); '0': always, as before.
inline const bool g_reads_state = g_enabled || !g_lean_scopes;
inline bool Enabled() { return g_enabled; }
struct State {
  bool active{}, task{};
  Category current{Other};
  u64 last{};
  std::array<u64, Count> ticks{};
};
inline thread_local State state;
inline void Begin(u64 now, bool task) {
  if (!Enabled())
    return;
  state = {};
  state.active = true;
  state.task = task;
  state.current = task ? OrderedTasks : Other;
  state.last = now;
}
inline void End(u64 now) {
  if (!g_reads_state || !state.active)
    return;
  state.ticks[state.current] += now - state.last;
  state.active = false;
  // B is the sole writer. Publish only once per handler, outside the measured
  // interval.
  for (unsigned i = 0; i < Count; ++i)
    totals[i].store(totals[i].load(std::memory_order_relaxed) + state.ticks[i],
                    std::memory_order_relaxed);
}
struct Scope {
  bool active;
  Category previous{};
  explicit Scope(Category category)
      : active(g_reads_state && state.active && !state.task) {
    if (!active)
      return;
    const auto now = Common::FencedRDTSC();
    previous = state.current;
    state.ticks[previous] += now - state.last;
    state.current = category;
    state.last = now;
  }
  ~Scope() {
    if (!active)
      return;
    const auto now = Common::FencedRDTSC();
    state.ticks[state.current] += now - state.last;
    state.current = previous;
    state.last = now;
  }
  Scope(const Scope &) = delete;
  Scope &operator=(const Scope &) = delete;
};
} // namespace FastRender::BProfile
