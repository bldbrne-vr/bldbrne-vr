// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "common/logging/log.h"
#include "core/vr/live_tune.h"
#include "core/vr/vr_session.h"
#include "video_core/renderer_vulkan/vr_perf.h"
#include "video_core/renderer_vulkan/vr_perf_logic.h"
#include "video_core/renderer_vulkan/vr_perf_probe.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOGDI
#define NOGDI
#endif
#include <windows.h>
#endif

namespace Vulkan::Perf {

namespace {

std::atomic<bool> g_resync_memo{false};
std::atomic<bool> g_depth_one_layout{false};
std::atomic<u64> g_global_seq{1};
std::atomic<u64> g_draw_serial{1};
std::atomic<u64> g_dma_draw{0};
std::atomic<u64> g_buffer_serial{0};
std::atomic<u64> g_context{0};
std::atomic<u32> g_reports{0};
bool g_logged_switches = false; // the first Refresh logs the switches once
Counters g_counters;

// The 5 s lines: the counters' change since the last one.
struct Window {
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    u64 resyncs = 0, skipped = 0, skippable = 0, img_same = 0, rbytes = 0, sbytes = 0, checks = 0,
        mism = 0;
};

// VR_PERF_PROBE: pass ends by reason and stack.
struct BreakRec {
    u64 count = 0;
    u32 reason = 0;
    std::array<u64, 8> rva{};
};
std::mutex g_break_mutex;
std::unordered_map<u64, BreakRec> g_breaks;
u64 g_break_total = 0;

void ReportBreaks() {
    std::vector<BreakRec> v;
    u64 total = 0;
    {
        std::scoped_lock lock{g_break_mutex};
        for (const auto& [k, r] : g_breaks) {
            v.push_back(r);
        }
        total = g_break_total;
        g_breaks.clear();
        g_break_total = 0;
    }
    std::sort(v.begin(), v.end(), [](const BreakRec& a, const BreakRec& b) {
        return a.count > b.count;
    });
    static const char* const names[] = {"end",        "size/count",  "colour view",
                                        "colour layout", "colour clear", "depth view",
                                        "depth layout",  "depth clear"};
    std::string top;
    for (std::size_t i = 0; i < std::min<std::size_t>(v.size(), 12); ++i) {
        top += fmt::format("; x{} {}:", v[i].count, names[std::min<u32>(v[i].reason, 7)]);
        for (const u64 rva : v[i].rva) {
            if (rva != 0) {
                top += fmt::format(" {:x}", rva);
            }
        }
    }
    LOG_INFO(Render_Vulkan, "VR perf probe: G-buffer-sized pass ends {} (5 s){}", total, top);
}

// VR_PERF_PROBE: resyncs per image and reader.
struct ResyncRec {
    ResyncProbe p{};
    u64 n[4]{};
    u64 draws_ago_sum = 0;
};
std::mutex g_resync_mutex;
std::unordered_map<u64, ResyncRec> g_resync_recs;

void ReportResyncs() {
    std::vector<ResyncRec> v;
    {
        std::scoped_lock lock{g_resync_mutex};
        for (const auto& [k, r] : g_resync_recs) {
            v.push_back(r);
        }
        g_resync_recs.clear();
    }
    std::sort(v.begin(), v.end(), [](const ResyncRec& a, const ResyncRec& b) {
        return a.n[0] + a.n[1] + a.n[2] + a.n[3] > b.n[0] + b.n[1] + b.n[2] + b.n[3];
    });
    std::string top;
    for (std::size_t i = 0; i < std::min<std::size_t>(v.size(), 8); ++i) {
        const auto& r = v[i];
        const u64 all = r.n[0] + r.n[1] + r.n[2] + r.n[3];
        top += fmt::format("; image {:#x} {}x{} fmt {} {} bits {} B tiled {} reader {:#x} writer {:#x}: "
                           "full {} (image same, buffer written {}), skipped {}, skippable(off) {}, "
                           "draws since its write avg {:.0f}",
                           r.p.address, r.p.width, r.p.height, r.p.format, r.p.bits, r.p.size,
                           r.p.tiled ? 1 : 0, r.p.reader, r.p.writer, r.n[0] + r.n[3], r.n[3], r.n[1],
                           r.n[2], all ? double(r.draws_ago_sum) / double(all) : 0.0);
    }
    LOG_INFO(Render_Vulkan, "VR perf probe: resyncs by image and reader (5 s){}", top);
}

u64 Take(std::atomic<u64>& c, u64& last) {
    const u64 v = c.load(std::memory_order_relaxed);
    const u64 d = v - last;
    last = v;
    return d;
}

} // Anonymous namespace

bool ResyncMemo() {
    return g_resync_memo.load(std::memory_order_relaxed);
}
bool DepthOneLayout() {
    return g_depth_one_layout.load(std::memory_order_relaxed);
}

u64 GlobalSeq() {
    return g_global_seq.load(std::memory_order_relaxed);
}
void BumpGlobal() {
    g_global_seq.fetch_add(1, std::memory_order_relaxed);
}
u64 DrawSerial() {
    return g_draw_serial.load(std::memory_order_relaxed);
}
void NextDraw() {
    g_draw_serial.fetch_add(1, std::memory_order_relaxed);
}
u64 DmaDraw() {
    return g_dma_draw.load(std::memory_order_relaxed);
}
void NoteDma() {
    g_dma_draw.store(DrawSerial(), std::memory_order_relaxed);
    BumpGlobal();
}
u64 NextBufferSerial() {
    return g_buffer_serial.fetch_add(1, std::memory_order_relaxed) + 1;
}
void SetContext(u64 pgm_hash) {
    g_context.store(pgm_hash, std::memory_order_relaxed);
}
u64 Context() {
    return g_context.load(std::memory_order_relaxed);
}

Counters& Count() {
    return g_counters;
}

void ReportMismatch(const char* what, const char* details) {
    if (g_reports.fetch_add(1, std::memory_order_relaxed) < 20) {
        LOG_ERROR(Render_Vulkan, "VR perf check MISMATCH ({}): {}", what, details);
    }
}

void NoteResync(const ResyncProbe& p, ResyncOutcome outcome) {
    std::scoped_lock lock{g_resync_mutex};
    auto& r = g_resync_recs[p.address * 0x9E3779B97F4A7C15ull ^ p.reader];
    r.p = p;
    ++r.n[static_cast<u32>(outcome)];
    r.draws_ago_sum += p.draws_ago;
}

void NotePassBreak(u32 reason) {
    BreakRec rec{};
    rec.reason = reason;
    u64 key = reason + 1;
#ifdef _WIN32
    if (reason == 0) {
        void* frames[10]{};
        const USHORT n = RtlCaptureStackBackTrace(2, 10, frames, nullptr);
        const u64 base = reinterpret_cast<u64>(GetModuleHandleW(nullptr));
        for (USHORT i = 0; i < n && i < rec.rva.size(); ++i) {
            const u64 a = reinterpret_cast<u64>(frames[i]);
            rec.rva[i] = a >= base ? a - base : a;
            key = key * 0x100000001B3ull ^ rec.rva[i];
        }
    }
#endif
    std::scoped_lock lock{g_break_mutex};
    auto& r = g_breaks[key];
    if (r.count == 0) {
        r = rec;
    }
    ++r.count;
    ++g_break_total;
}

void Refresh() {
    // The switches: vr.ini (the VR page writes it), VR_TUNE_FILE over it (A/B in one launch).
    static VideoCore::PerfLogic::Switch memo_switch, depth_switch;
    const auto& s = VR::GetSettings();
    bool memo = s.perf_resync_memo;
    bool depth = s.perf_depth_one_layout;
    if (VR::Tune::Active()) {
        memo = VR::Tune::Get("perf.resync_memo", memo ? 1.0f : 0.0f) != 0.0f;
        depth = VR::Tune::Get("perf.depth_one_layout", depth ? 1.0f : 0.0f) != 0.0f;
    }
    const bool memo_changed = memo_switch.Update(memo);
    const bool depth_changed = depth_switch.Update(depth);
    if (memo_changed) {
        BumpGlobal(); // every memo is invalid from here
    }
    if (memo_changed || depth_changed || !g_logged_switches) {
        g_logged_switches = true;
        LOG_INFO(Render_Vulkan, "VR perf: resync_memo {} depth_one_layout {}", memo ? 1 : 0,
                 depth ? 1 : 0);
    }
    g_resync_memo.store(memo, std::memory_order_relaxed);
    g_depth_one_layout.store(depth, std::memory_order_relaxed);

    if (!Check() && !Probe()) {
        return;
    }
    static Window w;
    const auto now = std::chrono::steady_clock::now();
    if (now - w.start < std::chrono::seconds(5)) {
        return;
    }
    const double secs = std::chrono::duration<double>(now - w.start).count();
    w.start = now;
    if (Probe()) {
        ReportBreaks();
        ReportResyncs();
    }
    auto& c = g_counters;
    const u64 resyncs = Take(c.resyncs, w.resyncs), skipped = Take(c.skipped, w.skipped),
              skippable = Take(c.skippable_off, w.skippable),
              img_same = Take(c.img_same_buf_written, w.img_same),
              rbytes = Take(c.resync_bytes, w.rbytes), sbytes = Take(c.skip_bytes, w.sbytes),
              checks = Take(c.checks, w.checks), mism = Take(c.mismatches, w.mism);
    LOG_INFO(Render_Vulkan,
             "VR perf check: {:.1f} s; resync_memo {}: resyncs {} ({:.1f} MB), skipped {} "
             "({:.1f} MB), would skip (off) {}, image same but buffer written {}; checked {}, "
             "MISMATCHES {} (total {}); depth_one_layout {}",
             secs, ResyncMemo() ? 1 : 0, resyncs, double(rbytes) / (1 << 20), skipped,
             double(sbytes) / (1 << 20), skippable, img_same, checks, mism, c.mismatches.load(),
             DepthOneLayout() ? 1 : 0);
}

std::string Summary(double seconds) {
    // Its own baselines (the VR_PERF_CHECK line keeps others). The presenter's thread only.
    static u64 last_resyncs = 0, last_bytes = 0, last_skipped = 0, last_depth = 0;
    auto& c = g_counters;
    const u64 resyncs = Take(c.resyncs, last_resyncs), bytes = Take(c.resync_bytes, last_bytes),
              skipped = Take(c.skipped, last_skipped), depth = Take(c.depth_kept, last_depth);
    if (seconds <= 0.0 || (resyncs == 0 && skipped == 0 && depth == 0)) {
        return {};
    }
    return fmt::format("; resyncs {:.1f}/s ({:.1f} MB/s), skipped {:.1f}/s, depth kept {:.0f}/s",
                       resyncs / seconds, double(bytes) / (1 << 20) / seconds, skipped / seconds,
                       depth / seconds);
}

} // namespace Vulkan::Perf
