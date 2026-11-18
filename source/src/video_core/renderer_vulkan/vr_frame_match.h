// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// VR_FRAME_MATCH=1 (diagnostic, off by default): how many of a frame's draws repeat a draw of the
// previous frame, i.e. whether stage B could reuse last frame's resolved work. Each draw (stage B,
// DrawRecord) gets three signatures, each one the previous plus more inputs:
//   pipe     the bound pipeline and the draw's render targets (image ids);
//   tex      + every stage's T# and S# (the raw sharps);
//   all      + every stage's V# (address, records, stride, format): all bound inputs except the
//            bytes the buffers hold (constants change per object and frame anyway).
// A draw counts as `ordinal` when the draw at the same position of the previous frame had the same
// signature, and `seen` when any draw of the previous frame had it. Every 240 frames:
// `VR frame match:` with the draws per frame and those shares (stage B's thread, FlushFrameEnd).

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <xxhash.h>
#include "common/logging/log.h"
#include "common/types.h"
#include "shader_recompiler/info.h"

namespace Vulkan::FrameMatch {

inline bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("VR_FRAME_MATCH");
        return v && *v == '1';
    }();
    return on;
}

struct Signatures {
    u64 pipe{}, tex{}, all{};
};

class Tracker {
public:
    template <typename Stages>
    void Note(const void* pipeline, const Stages& stages, const u32* targets, size_t target_count) {
        XXH3_state_t* st = state;
        XXH3_64bits_reset(st);
        XXH3_64bits_update(st, &pipeline, sizeof(pipeline));
        XXH3_64bits_update(st, targets, target_count * sizeof(u32));
        Signatures s;
        s.pipe = XXH3_64bits_digest(st);
        for (const auto* stage : stages) {
            if (!stage)
                continue;
            for (const auto& desc : stage->images) {
                const auto sharp = desc.GetSharp(*stage);
                XXH3_64bits_update(st, &sharp, sizeof(sharp));
            }
            for (const auto& desc : stage->samplers) {
                const auto sharp = desc.GetSharp(*stage);
                XXH3_64bits_update(st, &sharp, sizeof(sharp));
            }
        }
        s.tex = XXH3_64bits_digest(st);
        for (const auto* stage : stages) {
            if (!stage)
                continue;
            for (const auto& desc : stage->buffers) {
                const auto sharp = desc.GetSharp(*stage);
                const u64 key[3] = {sharp.base_address, u64(sharp.num_records) << 32 | sharp.stride,
                                    u64(sharp.GetDataFmt()) << 32 | u32(sharp.GetNumberFmt())};
                XXH3_64bits_update(st, key, sizeof(key));
            }
        }
        s.all = XXH3_64bits_digest(st);
        // The same `tex` signature can be drawn several times a frame (one mesh, other constants):
        // the k-th draw of a signature is the twin of last frame's k-th.
        const u64 key = s.tex ^ (u64(occurrence[s.tex]++ + 1) * 0x9E3779B97F4A7C15ull);
        // The buffer slots of a draw whose `tex` signature was drawn last frame: kept (same address
        // and size in the same slot), moved (same size, other address) or resized, by size.
        auto& now = cur_bufs[key];
        now.clear();
        for (const auto* stage : stages) {
            if (!stage)
                continue;
            for (const auto& desc : stage->buffers) {
                const auto sharp = desc.GetSharp(*stage);
                const u64 bytes =
                    sharp.stride ? u64(sharp.num_records) * sharp.stride : u64(sharp.num_records);
                now.push_back({u64(sharp.base_address), bytes});
            }
        }
        if (const auto it = prev_bufs.find(key); it != prev_bufs.end()) {
            const auto& was = it->second;
            for (size_t b = 0; b < now.size(); ++b) {
                ++buf_slots;
                const u32 bucket = now[b][1] <= 256 ? 0 : now[b][1] <= 4096 ? 1 : now[b][1] <= 65536 ? 2 : 3;
                if (b < was.size() && was[b] == now[b]) {
                    ++buf_kept;
                } else if (b < was.size() && was[b][1] == now[b][1]) {
                    ++buf_moved[bucket];
                } else {
                    ++buf_other[bucket];
                }
            }
            u32 moved_here = 0;
            for (size_t b = 0; b < now.size(); ++b)
                moved_here += !(b < was.size() && was[b] == now[b]);
            draws_moved_le[std::min<u32>(moved_here, 3)]++;
        }
        pending_tex = key;
        pending = true;
        const size_t i = cur.size();
        cur.push_back(s);
        ++draws;
        if (i < prev.size()) {
            ord_pipe += prev[i].pipe == s.pipe;
            ord_tex += prev[i].tex == s.tex;
            ord_all += prev[i].all == s.all;
        }
        seen_pipe += std::binary_search(prev_pipe.begin(), prev_pipe.end(), s.pipe);
        seen_tex += std::binary_search(prev_tex.begin(), prev_tex.end(), s.tex);
        seen_all += std::binary_search(prev_all.begin(), prev_all.end(), s.all);
        // Two and three frames back (a rotating per-frame constant area repeats its addresses).
        const bool in2 = std::binary_search(older_all[0].begin(), older_all[0].end(), s.all);
        const bool in3 = std::binary_search(older_all[1].begin(), older_all[1].end(), s.all);
        seen_all2 += in2;
        seen_all3 += in3;
        seen_all_any += in2 || in3 || std::binary_search(prev_all.begin(), prev_all.end(), s.all);
        if (i < prev2.size())
            ord_all2 += prev2[i].all == s.all;
        // Where the moved slots live: 1 MiB regions touched this frame.
        if (const auto it = prev_bufs.find(key); it != prev_bufs.end()) {
            for (size_t b = 0; b < now.size(); ++b) {
                if (b < it->second.size() && it->second[b] == now[b])
                    continue;
                moved_regions.push_back(now[b][0] >> 20);
            }
        }
    }

    // The recorder packets the draw last given to Note committed since `from` (DrawRecord's end):
    // compared with its twin's (the same `tex` signature) of the previous frame, 8 bytes at a time.
    template <typename Recorder>
    void Blob(const Recorder& recorder, u64 from, const u64* spent = nullptr, size_t categories = 0) {
        if (!pending)
            return;
        pending = false;
        // FAST_B_PROFILE: the draw's stage B ticks by category, all draws and eligible ones.
        bool eligible_now = false;
        struct Credit {
            Tracker& t;
            const u64* spent;
            size_t n;
            const bool& eligible;
            ~Credit() {
                if (!spent)
                    return;
                for (size_t i = 0; i < n && i < t.ticks_all.size(); ++i) {
                    t.ticks_all[i] += spent[i];
                    if (eligible)
                        t.ticks_eligible[i] += spent[i];
                }
            }
        } credit{*this, spent, categories, eligible_now};
        auto& bytes = cur_blobs[pending_tex];
        recorder.CopyOut(from, recorder.WritePosition(), bytes);
        blob_bytes += bytes.size();
        ++blob_draws;
        const auto it = prev_blobs.find(pending_tex);
        if (it == prev_blobs.end())
            return;
        ++blob_twins;
        const auto& was = it->second;
        // Eligible with re-encoding: the packets a hit does not re-encode (render pass, barriers,
        // copies, labels, timestamps, ...) are byte-identical, in order. Re-encoded: push
        // constants 1, descriptors 2, blend constants 6, binds 10/11/31/32, draws 12/13 and the
        // dynamic state 33..56 (emitted whatever the bind filter held).
        const auto structural = [](const std::vector<std::byte>& b, std::vector<std::byte>& out) {
            out.clear();
            for (size_t o = 0; o + 8 <= b.size();) {
                u32 op, size;
                std::memcpy(&op, b.data() + o, 4);
                std::memcpy(&size, b.data() + o + 4, 4);
                if (size < 8 || o + size > b.size())
                    return false;
                const u32 k = op & 63;
                const bool re = k == 1 || k == 2 || k == 6 || k == 10 || k == 11 || k == 12 ||
                                k == 13 || k == 31 || k == 32 || (k >= 33 && k <= 56);
                if (!re && k != 0)
                    out.insert(out.end(), b.begin() + o, b.begin() + o + size);
                o += size;
            }
            return true;
        };
        if (structural(bytes, struct_now) && structural(was, struct_was) && struct_now == struct_was) {
            ++blob_eligible2;
            eligible_now = true;
        }
        // Packets: an 8-byte header (u32 opcode, u32 size including the header), then the body.
        const auto opcodes = [](const std::vector<std::byte>& b, std::array<u32, 64>& count) {
            count = {};
            for (size_t o = 0; o + 8 <= b.size();) {
                u32 op, size;
                std::memcpy(&op, b.data() + o, 4);
                std::memcpy(&size, b.data() + o + 4, 4);
                ++count[op & 63];
                if (size < 8)
                    break;
                o += size;
            }
        };
        if (was.size() != bytes.size()) {
            std::array<u32, 64> a, b;
            opcodes(bytes, a);
            opcodes(was, b);
            for (u32 op = 0; op < 64; ++op) {
                if (a[op] > b[op])
                    extra_by_op[op] += a[op] - b[op];
                else
                    missing_by_op[op] += b[op] - a[op];
            }
            return;
        }
        ++blob_same_len;
        u64 diff = 0;
        u32 op = 0;
        size_t next = 0;
        // Eligible for copy-and-patch: the same packet sequence, and every differing word inside
        // a packet the hit re-encodes (PushDescriptors 2, bindIndexBuffer 11, draw 12,
        // drawIndexed 13, bindVertexBuffers 31, bindVertexBuffers2 32).
        bool eligible = true;
        for (size_t o = 0; o + 8 <= bytes.size(); o += 8) {
            if (o == next) {
                u32 size, was_op, was_size;
                std::memcpy(&op, bytes.data() + o, 4);
                std::memcpy(&size, bytes.data() + o + 4, 4);
                std::memcpy(&was_op, was.data() + o, 4);
                std::memcpy(&was_size, was.data() + o + 4, 4);
                eligible &= op == was_op && size == was_size;
                next = o + (size >= 8 ? size : 8);
            }
            if (std::memcmp(bytes.data() + o, was.data() + o, 8) != 0) {
                ++diff;
                ++diff_by_op[op & 63];
                const u32 k = op & 63;
                eligible &= k == 2 || k == 11 || k == 12 || k == 13 || k == 31 || k == 32;
            }
        }
        blob_eligible += eligible;
        blob_diff_words += diff;
        ++blob_hist[diff == 0 ? 0 : diff <= 4 ? 1 : diff <= 16 ? 2 : 3];
    }

    void FrameEnd() {
        occurrence.clear();
        prev_blobs.swap(cur_blobs);
        cur_blobs.clear();
        if (cur.empty())
            return;
        older_all[1].swap(older_all[0]);
        older_all[0].swap(prev_all); // prev_all is rebuilt below
        prev2.swap(prev);
        prev.swap(cur);
        cur.clear();
        std::sort(moved_regions.begin(), moved_regions.end());
        moved_regions.erase(std::unique(moved_regions.begin(), moved_regions.end()),
                            moved_regions.end());
        for (const u64 r : moved_regions) {
            region_lo = std::min(region_lo, r);
            region_hi = std::max(region_hi, r);
        }
        regions_sum += moved_regions.size();
        moved_regions.clear();
        prev_bufs.swap(cur_bufs);
        cur_bufs.clear();
        const auto sorted = [&](auto field, std::vector<u64>& out) {
            out.clear();
            for (const auto& s : prev)
                out.push_back(s.*field);
            std::sort(out.begin(), out.end());
        };
        sorted(&Signatures::pipe, prev_pipe);
        sorted(&Signatures::tex, prev_tex);
        sorted(&Signatures::all, prev_all);
        if (++frames < 240)
            return;
        const double d = draws ? double(draws) : 1.0;
        const auto pct = [&](u64 n) { return 100.0 * double(n) / d; };
        LOG_INFO(Render_Vulkan,
                 "VR frame match: draws/f={:.0f} ordinal pipe {:.1f}% tex {:.1f}% all {:.1f}% | "
                 "seen pipe {:.1f}% tex {:.1f}% all {:.1f}%",
                 d / double(frames), pct(ord_pipe), pct(ord_tex), pct(ord_all), pct(seen_pipe),
                 pct(seen_tex), pct(seen_all));
        const double slots = buf_slots ? double(buf_slots) : 1.0;
        const auto bp = [&](u64 n) { return 100.0 * double(n) / slots; };
        const double rd = double(draws_moved_le[0] + draws_moved_le[1] + draws_moved_le[2] +
                                 draws_moved_le[3]);
        const auto dp = [&](u64 n) { return rd ? 100.0 * double(n) / rd : 0.0; };
        LOG_INFO(Render_Vulkan,
                 "VR frame match buffers: slots/f={:.0f} kept {:.1f}% | moved <=256B {:.1f}% "
                 "<=4K {:.1f}% <=64K {:.1f}% big {:.1f}% | resized/new <=256B {:.1f}% <=4K {:.1f}% "
                 "<=64K {:.1f}% big {:.1f}% | draws with 0/1/2/3+ moved slots {:.1f}/{:.1f}/{:.1f}/"
                 "{:.1f}%",
                 slots / double(frames), bp(buf_kept), bp(buf_moved[0]), bp(buf_moved[1]),
                 bp(buf_moved[2]), bp(buf_moved[3]), bp(buf_other[0]), bp(buf_other[1]),
                 bp(buf_other[2]), bp(buf_other[3]), dp(draws_moved_le[0]), dp(draws_moved_le[1]),
                 dp(draws_moved_le[2]), dp(draws_moved_le[3]));
        LOG_INFO(Render_Vulkan,
                 "VR frame match history: all ordinal N-2 {:.1f}% | seen N-1 {:.1f}% N-2 {:.1f}% "
                 "N-3 {:.1f}% any of 3 {:.1f}% | moved slots in {:.1f} MiB regions/f, "
                 "{:#x}..{:#x} MiB",
                 pct(ord_all2), pct(seen_all), pct(seen_all2), pct(seen_all3), pct(seen_all_any),
                 double(regions_sum) / double(frames), region_lo, region_hi);
        const double bd = blob_draws ? double(blob_draws) : 1.0;
        const double same = blob_same_len ? double(blob_same_len) : 1.0;
        LOG_INFO(Render_Vulkan,
                 "VR frame match packets: {:.0f} B/draw | twin found {:.1f}% same length {:.1f}% | "
                 "of same length: identical {:.1f}% 1-4 words differ {:.1f}% 5-16 {:.1f}% 17+ "
                 "{:.1f}%, {:.1f} words differ on average | eligible for copy-and-patch {:.1f}% "
                 "of all draws, with re-encoded binds and state {:.1f}%",
                 double(blob_bytes) / bd, 100.0 * double(blob_twins) / bd,
                 100.0 * double(blob_same_len) / bd, 100.0 * double(blob_hist[0]) / same,
                 100.0 * double(blob_hist[1]) / same, 100.0 * double(blob_hist[2]) / same,
                 100.0 * double(blob_hist[3]) / same, double(blob_diff_words) / same,
                 100.0 * double(blob_eligible) / bd, 100.0 * double(blob_eligible2) / bd);
        blob_eligible = blob_eligible2 = 0;
        // The opcodes (RecorderOpcode numbers) behind the differences, most first.
        const auto top = [](const std::array<u64, 64>& c) {
            std::array<u32, 64> order;
            for (u32 i = 0; i < 64; ++i)
                order[i] = i;
            std::sort(order.begin(), order.end(), [&](u32 a, u32 b) { return c[a] > c[b]; });
            std::string s;
            for (u32 i = 0; i < 8 && c[order[i]]; ++i)
                s += fmt::format(" {}:{}", order[i], c[order[i]]);
            return s;
        };
        LOG_INFO(Render_Vulkan,
                 "VR frame match opcodes (per 240 frames): differing words by opcode{} | extra "
                 "packets{} | missing packets{}",
                 top(diff_by_op), top(extra_by_op), top(missing_by_op));
        // Stage B ticks per frame by FastRender::BProfile category index (all draws | eligible).
        std::string all_s, elig_s;
        for (size_t c = 0; c < ticks_all.size(); ++c) {
            if (ticks_all[c] == 0)
                continue;
            all_s += fmt::format(" {}:{:.0f}", c, double(ticks_all[c]) / double(frames));
            elig_s += fmt::format(" {}:{:.0f}", c, double(ticks_eligible[c]) / double(frames));
        }
        if (!all_s.empty())
            LOG_INFO(Render_Vulkan, "VR frame match ticks/f: all draws{} | eligible draws{}", all_s,
                     elig_s);
        ticks_all = {};
        ticks_eligible = {};
        diff_by_op = {};
        extra_by_op = {};
        missing_by_op = {};
        blob_bytes = blob_draws = blob_twins = blob_same_len = blob_diff_words = 0;
        blob_hist = {};
        frames = draws = 0;
        ord_pipe = ord_tex = ord_all = seen_pipe = seen_tex = seen_all = 0;
        ord_all2 =seen_all2 = seen_all3 = seen_all_any = regions_sum = 0;
        region_lo = ~u64{0};
        region_hi = 0;
        buf_slots = buf_kept = 0;
        buf_moved = {};
        buf_other = {};
        draws_moved_le = {};
    }

    Tracker() : state{XXH3_createState()} {}
    ~Tracker() {
        XXH3_freeState(state);
    }

private:
    XXH3_state_t* state;
    std::vector<Signatures> cur, prev;
    std::vector<u64> prev_pipe, prev_tex, prev_all;
    u64 frames{}, draws{};
    u64 ord_pipe{}, ord_tex{}, ord_all{}, seen_pipe{}, seen_tex{}, seen_all{};
    std::unordered_map<u64, std::vector<std::array<u64, 2>>> cur_bufs, prev_bufs;
    u64 buf_slots{}, buf_kept{};
    std::array<u64, 4> buf_moved{}, buf_other{}, draws_moved_le{};
    std::vector<Signatures> prev2;
    std::array<std::vector<u64>, 2> older_all; // sorted `all` of frames N-2 and N-3
    std::vector<u64> moved_regions;
    u64 ord_all2{}, seen_all2{}, seen_all3{}, seen_all_any{}, regions_sum{};
    u64 region_lo{~u64{0}}, region_hi{};
    u64 pending_tex{};
    bool pending{};
    std::unordered_map<u64, std::vector<std::byte>> cur_blobs, prev_blobs;
    u64 blob_bytes{}, blob_draws{}, blob_twins{}, blob_same_len{}, blob_diff_words{},
        blob_eligible{}, blob_eligible2{};
    std::unordered_map<u64, u32> occurrence; // this frame's draws per `tex` signature
    std::vector<std::byte> struct_now, struct_was;
    std::array<u64, 4> blob_hist{};
    std::array<u64, 64> diff_by_op{}, extra_by_op{}, missing_by_op{};
    std::array<u64, 32> ticks_all{}, ticks_eligible{};
};

// Stage B's own (one recording thread).
inline Tracker& Get() {
    static Tracker tracker;
    return tracker;
}

} // namespace Vulkan::FrameMatch
