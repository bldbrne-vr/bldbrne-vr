// SPDX-FileCopyrightText: Copyright 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <limits>
#include <optional>
#include "common/types.h"

// Ported from Supermedo/bloodborne_pc texture_cache.cpp, commit
// 5f56ed6a7ca47c37049cbf737a244707a3ecc4d2 (GPL-2.0-or-later).
namespace VideoCore::TextureMemory {

// bbport's collector: at most 256 visits, counting only actual evictions against the deletion
// budget. The caller moves skipped images to the back of the LRU, so each pass makes progress.
template <class Lru, class CanEvict, class Evict>
void Collect(Lru& lru, u64 cutoff, size_t& remaining, CanEvict&& can_evict, Evict&& evict) {
    u32 visited = 0;
    lru.ForEachItemBelow(cutoff, [&](auto id) {
        if (remaining == 0 || ++visited > 256) {
            return true;
        }
        if (can_evict(id)) {
            --remaining;
            evict(id);
        }
        return false;
    });
}

// bbport records the GC tick at the start of each second. Uses older than the start of the
// second `ago` seconds back are old enough (allocation recovery: 2; the collector: IdleSeconds).
// No clock read is added to texture binding.
class AgeCutoff {
public:
    void Record(u64 second, u64 tick) {
        if (second != recorded_second) {
            recorded_second = second;
            samples[second % samples.size()] = {second, tick};
        }
    }
    u64 Before(u64 second, u64 ago = 2) const {
        if (ago >= samples.size() || second < ago) {
            return 0;
        }
        const auto& sample = samples[(second - ago) % samples.size()];
        return sample.second <= second - ago ? sample.tick : 0;
    }

private:
    struct Sample {
        u64 second{}, tick{};
    };
    std::array<Sample, 64> samples{};
    u64 recorded_second{};
};

// bbport 5f56ed6a: the collector's age is in seconds, not GC ticks (one per frame here). An age
// of 16 ticks (a fifth of a second at 90 fps) evicted every texture briefly off screen and
// uploaded it again when it came back: ~300 evictions a second while above the trigger, the
// chest's quick items blinking as the camera moved (2026-10-18). Below the pressure mark an image
// goes once unused for 24 s (an area left behind), under pressure 5 s, over the critical mark
// 1 s; still over it after that pass, by ticks as before. Below pressure 24 s here (bbport: 20,
// design choice for this version).
inline u64 IdleSeconds(bool pressured, bool aggressive) {
    return aggressive ? 1 : pressured ? 5 : 24;
}

// A transition requests four passes, two seconds apart. Only a confirmed map change bypasses
// the pressure threshold; death, same-map travel and unknown destinations never do.
// Freeze the LRU cutoff at the request: textures touched in the destination survive even if
// briefly off screen. The normal two-second age and GPU/binding checks still apply.
class TransitionCleanup {
public:
    template <class IsPressured>
    std::optional<u64> Step(u64 second, u64 request, u64 tick, IsPressured&& is_pressured) {
        const u64 sequence = request >> 1;
        if (sequence != seen) {
            seen = sequence;
            area_changed = (request & 1) != 0;
            cutoff = tick;
            next = second;
            expires = second + 12;
            remaining = 4;
        }
        if (!remaining || second > expires || second < next) {
            if (second > expires)
                remaining = 0;
            return std::nullopt;
        }
        --remaining;
        next = second + 2;
        // Recheck each pass: a pressure-only transition stops reclaiming once pressure subsides.
        // Skipped passes still consume the budget; no per-frame polling or delayed flush later.
        if (!area_changed && !is_pressured())
            return std::nullopt;
        return cutoff;
    }

private:
    u64 seen{}, cutoff{}, next{}, expires{};
    unsigned remaining{};
    bool area_changed{};
};

struct Candidate {
    u64 lru_tick{}, submission_tick{};
    bool gpu_valid_data{}, retained{}, overlaps{};
};

inline bool CanReclaim(const Candidate& image, u64 cutoff, u64 open_tick) {
    return image.lru_tick < cutoff && image.submission_tick < open_tick && !image.gpu_valid_data &&
           !image.retained && !image.overlaps;
}

// Called only after an allocation really failed. Three bounded rounds, as in bbport's
// 5f56ed6a recovery; normal allocation does not scan, wait or evict anything.
template <class Reclaim, class Retry>
bool Recover(u64 request_bytes, Reclaim&& reclaim, Retry&& retry) {
    const u64 twice = request_bytes > std::numeric_limits<u64>::max() / 2
                          ? std::numeric_limits<u64>::max()
                          : request_bytes * 2;
    for (const u64 target : {u64{64} << 20, u64{256} << 20, u64{1024} << 20}) {
        reclaim(std::max(target, twice));
        if (retry()) {
            return true;
        }
    }
    return false;
}

} // namespace VideoCore::TextureMemory
