// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// VR_BATCH_CENSUS=1 (diagnostic, off by default): how far a frame's draws could be batched. Each
// stage B draw (DrawRecord, right before its pass begin) is compared with the draw before it in
// the same pass instance (Scheduler::PassBegins unchanged after the begin):
//  - pipeline: the same pipeline;
//  - material: and the same images, views and samplers (every DescriptorImageInfo);
//  - buffers: and the same buffer objects and ranges (offsets may differ: per-object constants
//    rebuilt in a ring);
//  - dynamic: and the same dynamic state (viewports, scissors, depth, stencil, cull, blend);
//  - batch: all of the above and the same indexed-ness and index type: what one multi-draw call
//    with per-draw offsets and user data could take.
// A run is a sequence of draws each matching the one before at a level. Every 240 frames: `VR batch
// census:` with draws and runs per frame for each level (runs = the calls batching would leave),
// the share of draws in runs of 2 or more, the batch runs' length histogram, what differs within
// batch runs (buffer offsets, user data, index address), the instances and indices per draw, and
// the same for the G-buffer passes (6 colour targets). FrameEnd is called from FlushFrameEnd
// (stage B's thread, as every Note).

#include <array>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string>
#include "common/logging/log.h"
#include "common/types.h"

namespace Vulkan::BatchCensus {

inline bool Enabled() {
    static const bool on = [] {
        const char* v = std::getenv("VR_BATCH_CENSUS");
        return v && v[0] == '1';
    }();
    return on;
}

inline u64 Mix(u64 h, u64 v) {
    return (h ^ (v + 0x9E3779B97F4A7C15ull + (h << 6) + (h >> 2))) * 0xFF51AFD7ED558CCDull;
}
inline u64 MixBytes(u64 h, const void* data, size_t size) {
    const auto* p = static_cast<const u8*>(data);
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        u64 w;
        std::memcpy(&w, p + i, 8);
        h = Mix(h, w);
    }
    u64 tail = 0;
    std::memcpy(&tail, p + i, size - i);
    return Mix(h, tail ^ (u64(size) << 56));
}

// One draw's identity at each level (hashes) and the values batching would have to vary.
struct DrawKey {
    u64 pass{};      // PassBegins after the begin
    u64 pipeline{};  // pipeline handle
    u64 material{};  // + images
    u64 buffers{};   // buffer objects and ranges
    u64 offsets{};   // buffer offsets
    u64 dynamic{};   // dynamic state values
    u64 user_data{}; // push data user registers
    u64 index_addr{};
    u32 index_type{}; // 0 non-indexed, 1 + IndexType
    u32 instances{};
    u32 indices{};
    bool gbuffer{};
};

class Census {
public:
    void Note(const DrawKey& k) {
        ++frame_draws;
        const bool same_pass = has_prev && k.pass == prev.pass;
        const bool same_pipe = same_pass && k.pipeline == prev.pipeline;
        const bool same_mat = same_pipe && k.material == prev.material;
        const bool same_buf = same_mat && k.buffers == prev.buffers;
        const bool same_dyn = same_buf && k.dynamic == prev.dynamic;
        const bool same_batch = same_dyn && k.index_type == prev.index_type;
        const std::array<bool, Levels> same{same_pipe, same_mat, same_buf, same_dyn, same_batch};
        for (u32 l = 0; l < Levels; ++l) {
            Account(level[l], same[l], k.gbuffer);
        }
        if (same_batch) {
            diff_offsets += k.offsets != prev.offsets;
            diff_user += k.user_data != prev.user_data;
            diff_index += k.index_addr != prev.index_addr;
            ++batch_pairs;
        }
        // The batch level's run length histogram (closed when the run ends).
        if (same_batch) {
            ++batch_run;
        } else {
            CloseBatchRun();
            batch_run = 1;
            batch_run_gbuffer = k.gbuffer;
        }
        instances += k.instances;
        indices += k.indices;
        draws += 1;
        if (k.gbuffer) {
            ++gbuffer_draws;
        }
        prev = k;
        has_prev = true;
    }

    void FrameEnd() {
        CloseBatchRun();
        batch_run = 0;
        has_prev = false; // runs never cross frames
        frame_draws = 0;
        if (++frames < 240) {
            return;
        }
        const double f = double(frames);
        static constexpr std::array<const char*, Levels> names{"pipeline", "material", "buffers",
                                                               "dynamic", "batch"};
        std::string lv;
        for (u32 l = 0; l < Levels; ++l) {
            const auto& L = level[l];
            lv += fmt::format(" | {} runs {:.0f}/f ({:.0f}% of draws in runs >= 2; G-buffer runs "
                              "{:.0f}/f)",
                              names[l], double(L.runs) / f,
                              draws ? 100.0 * double(L.in_runs) / double(draws) : 0.0,
                              double(L.runs_gbuffer) / f);
        }
        std::string hist;
        static constexpr std::array<const char*, 7> buckets{"1", "2-3", "4-7", "8-15", "16-31",
                                                            "32-63", "64+"};
        for (size_t i = 0; i < buckets.size(); ++i) {
            hist += fmt::format(" {}:{:.1f}/{:.0f}", buckets[i], double(run_count[i]) / f,
                                double(run_draws[i]) / f);
        }
        std::string ghist;
        for (size_t i = 0; i < buckets.size(); ++i) {
            ghist += fmt::format(" {}:{:.1f}/{:.0f}", buckets[i], double(grun_count[i]) / f,
                                 double(grun_draws[i]) / f);
        }
        LOG_INFO(Render_Vulkan,
                 "VR batch census: {:.0f} draws/f ({:.0f} G-buffer), {:.1f} instances and {:.0f} "
                 "indices a draw{}",
                 double(draws) / f, double(gbuffer_draws) / f,
                 draws ? double(instances) / double(draws) : 0.0,
                 draws ? double(indices) / double(draws) : 0.0, lv);
        LOG_INFO(Render_Vulkan,
                 "VR batch census: batch runs by length (runs/f / draws/f):{} | G-buffer:{} | "
                 "within batch runs, the next draw differs in buffer offsets {:.0f}%, user data "
                 "{:.0f}%, index address {:.0f}%",
                 hist, ghist, batch_pairs ? 100.0 * double(diff_offsets) / double(batch_pairs) : 0.0,
                 batch_pairs ? 100.0 * double(diff_user) / double(batch_pairs) : 0.0,
                 batch_pairs ? 100.0 * double(diff_index) / double(batch_pairs) : 0.0);
        level = {};
        run_count = run_draws = grun_count = grun_draws = {};
        diff_offsets = diff_user = diff_index = batch_pairs = 0;
        instances = indices = draws = gbuffer_draws = frames = 0;
    }

private:
    static constexpr u32 Levels = 5;
    struct Level {
        u64 runs{}, in_runs{}, runs_gbuffer{};
        bool open_counted{}; // the open run's first draw already counted in in_runs
    };
    void Account(Level& L, bool same, bool gbuffer) {
        if (!same) {
            ++L.runs;
            L.runs_gbuffer += gbuffer ? 1 : 0;
            L.open_counted = false;
            return;
        }
        // A second draw joins: count the run's first draw once, then this one.
        L.in_runs += L.open_counted ? 1 : 2;
        L.open_counted = true;
    }
    static size_t Bucket(u64 n) {
        if (n <= 1)
            return 0;
        if (n <= 3)
            return 1;
        if (n <= 7)
            return 2;
        if (n <= 15)
            return 3;
        if (n <= 31)
            return 4;
        if (n <= 63)
            return 5;
        return 6;
    }
    void CloseBatchRun() {
        if (batch_run == 0)
            return;
        const size_t b = Bucket(batch_run);
        ++run_count[b];
        run_draws[b] += batch_run;
        if (batch_run_gbuffer) {
            ++grun_count[b];
            grun_draws[b] += batch_run;
        }
    }

    std::array<Level, Levels> level{};
    std::array<u64, 7> run_count{}, run_draws{}, grun_count{}, grun_draws{};
    u64 batch_run{};
    bool batch_run_gbuffer{};
    u64 diff_offsets{}, diff_user{}, diff_index{}, batch_pairs{};
    u64 instances{}, indices{}, draws{}, gbuffer_draws{}, frames{}, frame_draws{};
    DrawKey prev{};
    bool has_prev{};
};

// Stage B's own (one recording thread).
inline Census& Get() {
    static Census census;
    return census;
}

} // namespace Vulkan::BatchCensus
