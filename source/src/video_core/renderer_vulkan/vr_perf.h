// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

// GPU work between and inside render passes: the
// runtime switches and counters. perf_resync_memo is off by default, perf_depth_one_layout on;
// off, each gives the GPU exactly the work it got before 2026-10-09.
//  - vr.ini perf_resync_memo / VR_TUNE_FILE perf.resync_memo: an image read through a texel buffer
//    is not re-tiled into the buffer again while neither was written since the last time
//    (buffer_cache.cpp SynchronizeBufferFromImage).
//  - vr.ini perf_depth_one_layout / VR_TUNE_FILE perf.depth_one_layout: a draw with depth writes
//    off that does not sample the depth, while the depth is in the writable layout, keeps the
//    write view and that layout (Rasterizer::BeginRendering), so a depth-write toggle no longer
//    ends the render pass; never more view or layout changes than before (vr_perf_logic.h).
//  - VR_PERF_CHECK=1 (env, tests): the shadow check. Skipped resyncs are also done the full way
//    into a scratch buffer and compared with the buffer's bytes on the GPU's result (one at a time:
//    a skip while a comparison is in flight is not checked). Mismatches: the first 20 logged in
//    full, then counted in the 5 s 'VR perf check:' line. Off: no cost.
//  - VR_PERF_PROBE=1 (env, tests): which images are resynced (one line each, the first 12, and a
//    5 s summary by image and reader), what ends the render passes with 4+ colour targets (the
//    G-buffer), every 5 s, and why the copy HLE refused a two-buffer compute shader (once per
//    shader and reason, Rasterizer::ProbeCopyReject).
//  - Always: the presenter's 5 s 'VR perf:' line carries the resync counts (Summary()).
#pragma once

#include <atomic>
#include <cstdlib>
#include <string>
#include "common/types.h"

namespace Vulkan::Perf {

// The switches as they apply now (read on the command processor's thread at every guest submit).
bool ResyncMemo();
bool DepthOneLayout();
// VR_PERF_CHECK=1 / VR_PERF_PROBE=1 (read once at start-up: inline plain loads, several per draw,
// where an out-of-line call paid a function-local static's guard).
inline bool EnvSwitchOn(const char* name) {
    const char* v = std::getenv(name);
    return v != nullptr && v[0] != 0 && v[0] != '0';
}
inline const bool g_perf_check = EnvSwitchOn("VR_PERF_CHECK");
inline const bool g_perf_probe = EnvSwitchOn("VR_PERF_PROBE");
inline bool Check() {
    return g_perf_check;
}
inline bool Probe() {
    return g_perf_probe;
}

// Reads vr.ini and VR_TUNE_FILE again (the guest submit, Rasterizer::OnSubmit): a toggled switch
// invalidates every memo. Also writes the 5 s 'VR perf check:' / 'VR perf probe:' lines.
void Refresh();

// Every memo is checked against this: a toggle or a raw-memory (DMA) shader moves it.
u64 GlobalSeq();
void BumpGlobal();
// The draw (or dispatch) whose bindings are being made: Rasterizer::BindResources moves it.
u64 DrawSerial();
void NextDraw();
// The draw serial of the last raw-memory (DMA) shader's bindings.
u64 DmaDraw();
void NoteDma();
// A new Buffer's identity (never reused).
u64 NextBufferSerial();
// The shader whose bindings are being made (probe only: who reads / wrote a resynced image).
void SetContext(u64 pgm_hash);
u64 Context();

// Counters (the 5 s lines).
struct Counters {
    std::atomic<u64> resyncs{};       // full resyncs (image -> buffer)
    std::atomic<u64> skipped{};       // resyncs the memo skipped
    std::atomic<u64> skippable_off{}; // with the switch off: resyncs it would have skipped
    std::atomic<u64> img_same_buf_written{}; // the image unchanged, the buffer written since
    std::atomic<u64> resync_bytes{};  // bytes of the full resyncs
    std::atomic<u64> skip_bytes{};    // bytes skipped
    std::atomic<u64> checks{};        // skipped resyncs compared (VR_PERF_CHECK)
    std::atomic<u64> mismatches{};    // of those, different bytes
    std::atomic<u64> depth_kept{};    // depth targets bound writable although writes are off
};
Counters& Count();

// The presenter's 5 s 'VR perf:' line: "; resyncs N/s (M MB/s), skipped N/s, depth kept N/s"
// since the last call (empty when nothing happened). Cheap: a few relaxed atomic loads.
std::string Summary(double seconds);

// A mismatch of the shadow check: logged in full for the first 20.
void ReportMismatch(const char* what, const char* details);

// VR_PERF_PROBE: a render pass with 4+ colour targets ends. `reason` 0: an explicit end (the
// caller's stack is kept), 1: the next pass differs in size / count, 2: a colour view, 3: a colour
// layout, 4: a colour clear, 5: the depth view, 6: the depth layout, 7: a depth clear. The 5 s
// 'VR perf probe:' line lists the most frequent (stack frames as offsets in the exe).
void NotePassBreak(u32 reason);

} // namespace Vulkan::Perf
