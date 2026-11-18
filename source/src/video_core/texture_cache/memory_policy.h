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

// The critical mark (2026-11-03): within a small margin of the budget (5%, at least 256 MiB),
// never below `floor` (the pressure mark). Upstream's, 20% of it below, sat where VR play is on
// 6-8 GB cards: most of the memory is not the caches' to free (the buffers the GPU wrote, the
// driver's, the VR images), so the critical passes ran every frame and only uploaded again what
// had just been on screen (the stress test at the Old Yharnam horizon on a 6 GB card's budget:
// 41,000 textures evicted in 5 minutes, 9% of the frame rate). Both caches use it.
constexpr s64 CriticalMark(s64 budget, s64 floor) {
    const s64 margin = std::max<s64>(s64{256} << 20, budget / 20);
    return std::max(budget - margin, floor);
}

// A load's revision of the budget the marks come from (2026-11-03): Windows' budget for the game
// moves as other programs take or give back memory (the VR runtime settling in after the start,
// a recording started). That budget is what the game may use in all, its own use included: the
// marks compare the game's use with it, so it is never reduced by that use. A change under 5% (at
// least 256 MiB) is noise and leaves the marks; the revised budget stays within half the start's
// (a passing dip, the window out of focus) and the card's size (`card`). Nothing when the marks
// stay, or when the budget is not known (0).
constexpr std::optional<s64> RevisedBudget(s64 current, s64 start, s64 live, s64 card) {
    if (live <= 0 || start <= 0 || current <= 0) {
        return std::nullopt;
    }
    const s64 revised = std::clamp(live, start / 2, std::max(card, start));
    const s64 change = revised > current ? revised - current : current - revised;
    if (change < std::max<s64>(s64{256} << 20, current / 20)) {
        return std::nullopt;
    }
    return revised;
}

// The critical passes rest only when they cannot win (2026-11-03). Each eviction of theirs belongs
// to the WindowSeconds window it happened in (its cohort); an image created again at its address
// within RefaultWindows windows came back to its cohort (uploaded again: nothing was freed). As a
// window closes, with the passes having run in every second of it and of the one before: they
// rest BackoffSeconds when, of the bytes those two cohorts evicted, at least half came back and
// the newer cohort is not clearly winning (a quarter back at least), or when they evicted nothing.
// The gentle passes go on meanwhile. While what they evict stays gone (an area left behind) they
// never rest: over the budget a real card pages, worse than any upload (a rest after any 3 s over
// the mark let the sprint between lamps on a 6 GB card's budget climb to 4.8 GB, the old build
// 4.2). An allocation that fails still reclaims (Recover). Caller holds the cache's lock; each
// pass calls Advance before its evictions (a code review: a pass in a window's first frame, where
// the 1 s cutoff moves, was counted in the window before) and Ran after its critical passes.
class CriticalBackoff {
public:
    static constexpr u64 WindowSeconds = 3;
    static constexpr u64 RefaultWindows = 2;
    static constexpr u64 BackoffSeconds = 10;
    // At the start of each pass, in `second`. True when this starts a rest.
    bool Advance(u64 second) {
        const u64 now = second / WindowSeconds;
        if (now == window) {
            return false;
        }
        const bool consecutive = now == window + 1;
        full_windows = consecutive && ran_seconds == WindowSeconds ? full_windows + 1 : 0;
        const Cohort& newer = cohorts[window % Cohorts];
        const Cohort& older = cohorts[(window + Cohorts - 1) % Cohorts];
        last_evicted = newer.evicted + older.evicted;
        last_returned = newer.returned + older.returned;
        const bool rest = full_windows >= 2 &&
                          (last_evicted == 0 || (last_returned * 2 >= last_evicted &&
                                                 newer.returned * 4 >= newer.evicted));
        if (rest) {
            backoff_until = second + BackoffSeconds;
            full_windows = 0;
        }
        if (consecutive) {
            cohorts[now % Cohorts] = {}; // the slot of the cohort past RefaultWindows
        } else {
            cohorts = {};
        }
        window = now;
        ran_seconds = 0;
        return rest;
    }
    // May the critical passes run in this second?
    bool Allowed(u64 second) const {
        return second >= backoff_until;
    }
    // A critical pass evicted an image of `bytes` at `address`.
    void Evicted(u64 address, u64 bytes) {
        ghosts[Slot(address)] = {address, bytes, window};
        cohorts[window % Cohorts].evicted += bytes;
    }
    // An image is created at `address`: one the critical passes evicted lately came back.
    void Created(u64 address) {
        Ghost& ghost = ghosts[Slot(address)];
        if (ghost.bytes != 0 && ghost.address == address && ghost.window + RefaultWindows >= window) {
            cohorts[ghost.window % Cohorts].returned += ghost.bytes;
            ghost.bytes = 0;
        }
    }
    // After the pass, when its critical passes ran in `second`.
    void Ran(u64 second) {
        if (second != ran_second) {
            ran_second = second;
            ++ran_seconds;
        }
    }
    // The two cohorts Advance last judged: what they evicted, and what of it came back.
    u64 LastEvicted() const {
        return last_evicted;
    }
    u64 LastReturned() const {
        return last_returned;
    }

private:
    struct Ghost {
        u64 address{}, bytes{}, window{};
    };
    struct Cohort {
        u64 evicted{}, returned{};
    };
    static constexpr size_t Ghosts = 4096;
    static constexpr size_t Cohorts = RefaultWindows + 1;
    static size_t Slot(u64 address) {
        return static_cast<size_t>(((address >> 8) * 0x9E3779B97F4A7C15ull) >> 52) & (Ghosts - 1);
    }
    std::array<Ghost, Ghosts> ghosts{};
    std::array<Cohort, Cohorts> cohorts{};
    u64 window{}, ran_second{~u64{0}}, ran_seconds{}, full_windows{};
    u64 last_evicted{}, last_returned{}, backoff_until{};
};

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

// The marks follow Windows' budget during play too (2026-11-05), not only at a load: another
// program taking VRAM mid-play (the VR runtime, a browser, a recording) lowered the game's budget
// and Windows paged the game into system memory while the marks, set at the last load, saw no
// pressure. A reading every PeriodSeconds; a change RevisedBudget accepts applies when the next
// reading gives the same one (within its dead zone), so a passing dip moves nothing.
class LiveBudget {
public:
    static constexpr u64 PeriodSeconds = 2;

    // Is a reading due this second?
    bool Due(u64 second) {
        if (second < next) {
            return false;
        }
        next = second + PeriodSeconds;
        return true;
    }

    // RevisedBudget's answer for this reading: the change to apply now, when the last reading
    // gave the same answer.
    std::optional<s64> Confirm(std::optional<s64> revised) {
        if (!revised) {
            pending.reset();
            return std::nullopt;
        }
        if (pending) {
            const s64 change = *pending > *revised ? *pending - *revised : *revised - *pending;
            if (change < std::max<s64>(s64{256} << 20, *pending / 20)) {
                pending.reset();
                return revised;
            }
        }
        pending = revised;
        return std::nullopt;
    }

private:
    u64 next{};
    std::optional<s64> pending;
};

// Windows can move part of the game's VRAM into system memory while the budget it reports stays
// high (2026-11-05: another program held 12.5 GB of a 16 GB card at top residency priority; the
// game's budget read 12.2 GB throughout, 0.5-0.9 GB of it went to system memory, fps fell to 58-64
// and no cleanup ran). The spill (Instance::QueryMemoryBudget, from Windows' counters): about 0
// normally. Spilled EnterBytes or more in
// ConfirmReadings readings in a row: the marks drop to what Windows keeps resident (the cleanup
// then frees the spilled amount and the critical margin, and Windows can bring the rest back);
// lower again as the spill grows; held until ReleaseReadings in a row stay under LeaveBytes, then
// Windows' budget sets them again. Never under FloorBytes (render targets and the VR images
// cannot go).
class SpillWatch {
public:
    static constexpr s64 EnterBytes = s64{192} << 20;
    static constexpr s64 LeaveBytes = s64{96} << 20;
    static constexpr s64 StepBytes = s64{64} << 20;
    static constexpr s64 FloorBytes = s64{2048} << 20;
    static constexpr u32 ConfirmReadings = 2;
    static constexpr u32 ReleaseReadings = 30; // a minute of LiveBudget readings

    struct Answer {
        bool hold{};               // the marks stay where the spill put them (LiveBudget waits)
        std::optional<s64> budget; // the marks' budget to apply now
    };

    // One reading: the game's VRAM use as Vulkan counts it, of it what Windows moved to system
    // memory, the marks' budget now.
    Answer Step(s64 usage, s64 spilled, s64 current) {
        const s64 resident = usage - spilled;
        if (spilled >= EnterBytes) {
            quiet = 0;
            if (!active && ++over < ConfirmReadings) {
                return {};
            }
            active = true;
            const s64 target = std::max(FloorBytes, resident);
            if (target + StepBytes <= current) {
                return {true, target};
            }
            return {true, std::nullopt};
        }
        over = 0;
        if (!active) {
            return {};
        }
        if (spilled >= LeaveBytes) {
            quiet = 0; // the release needs ReleaseReadings in a row under LeaveBytes
        } else if (++quiet >= ReleaseReadings) {
            active = false;
            quiet = 0;
            return {};
        }
        return {true, std::nullopt};
    }

    [[nodiscard]] bool Active() const noexcept {
        return active;
    }

private:
    u32 over{};
    u32 quiet{};
    bool active{};
};

} // namespace VideoCore::TextureMemory
