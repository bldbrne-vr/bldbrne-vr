// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

// The buffer cache's eviction policy (BufferCache::EvictIdleBuffers). Upstream's part 1 never ran
// its collector: a buffer lived until a bigger one swallowed it, so every load left the last
// area's buffers in VRAM (the 2026-10-26 soak, 19 lamp warps between Central Yharnam and the
// Sickroom: the cache 592 -> 1231 MiB, their right-eye twins 72 -> 872 MiB; standing still
// added nothing). bbport's 0.6 rule, for what is safe here: under VRAM pressure a buffer unused
// for IdleSeconds, or unused since a warp, death or new map (the passes after a transition), goes
// when the game's memory holds all of its bytes, so that a buffer made there later uploads the
// same ones. Never one that:
// - the GPU wrote (no FAST_GPU_SETTLE: the game's copy is stale; the face glitch);
// - has a right-eye twin (the right eye's bytes exist nowhere else): its twin alone goes
//   instead (NativeStereo::ReleaseRight; a later per-eye write makes it again from the left),
//   and during play only when the GPU did not write the buffer (TwinReleasable);
// - holds an image's data (a resync: image_data);
// nor anything while DMA is on (shaders reach any buffer by address, bound or not).
// The same soak with all of it, 100 s per place on an 8 GB budget: VRAM 3.9 GB after 8 warps
// (5.0 GB without the twins' release), the twins 35 MiB on each arrival, hitches as before.
namespace VideoCore::BufferEviction {

// Below the pressure mark nothing goes; under it a minute unused (an area left behind), over the
// critical mark 5 s (bbport: the same, its BB_VRAM_IDLE_SECONDS and pressure age).
inline std::uint64_t IdleSeconds(bool critical) {
    return critical ? 5 : 60;
}

// A pass a second at most, with a bound on what it frees: every eviction is a vkFreeMemory and,
// for the next user, an upload. The biggest first: fewest frees for the memory.
inline constexpr std::size_t MaxPerPass = 8;
inline constexpr std::size_t MaxPerCriticalPass = 32;
inline constexpr std::uint64_t MaxBytesPerPass = 256ull << 20;
// A warp, a death or a new map (TextureMemory::TransitionCleanup's request, as for textures): what
// the area left behind used goes in the passes after it, by the request's tick instead of an
// age; the load hides the frees and the uploads, so a pass may take more.
// Right-eye twins (released alone, their buffer kept): an idle pass releases them only in a batch
// of at least this much (each release rebuilds the binding memos once).
inline constexpr std::uint64_t MinTwinBytesPerIdlePass = 16ull << 20;
inline constexpr std::size_t MaxPerTransitionPass = 128;
inline constexpr std::uint64_t MaxBytesPerTransitionPass = 1024ull << 20;

struct Candidate {
    std::uint32_t id{};
    std::uint64_t bytes{}, last_used_tick{};
    bool gpu_written{}, twin{}, image_data{};
};

inline bool Evictable(const Candidate& c, std::uint64_t cutoff_tick) {
    return c.last_used_tick < cutoff_tick && !c.gpu_written && !c.twin && !c.image_data;
}

// May a pass release this buffer's right-eye twin alone? A twin of a buffer the GPU wrote holds the
// right eye's picture, which exists nowhere else: released during play, the right eye showed the
// left eye's picture there until the game drew it again (2026-11-05: on a 3.8 GB test budget the
// critical passes released 16 such twins in 150 s, 8.5 MiB each, and no other). The passes after a
// transition (a warp, a death, a new map) still release them: the load draws what the area left
// behind again, and without it the twins grew 72 -> 872 MiB over 19 warps (2026-10-26).
inline bool TwinReleasable(const Candidate& c, std::uint64_t cutoff_tick, bool transition) {
    return c.twin && c.last_used_tick < cutoff_tick && (transition || !c.gpu_written);
}

// The ids to evict, biggest first, within the pass's bounds (one buffer over the byte bound
// still goes when it is the first: a single large buffer must be able to leave).
inline std::vector<std::uint32_t> Pick(std::vector<Candidate> candidates, std::uint64_t cutoff_tick,
                                       std::size_t max_count, std::uint64_t max_bytes) {
    std::erase_if(candidates, [&](const Candidate& c) { return !Evictable(c, cutoff_tick); });
    std::ranges::sort(candidates, [](const Candidate& a, const Candidate& b) {
        return a.bytes != b.bytes ? a.bytes > b.bytes : a.id < b.id;
    });
    std::vector<std::uint32_t> out;
    std::uint64_t bytes = 0;
    for (const auto& c : candidates) {
        if (out.size() == max_count || (!out.empty() && bytes + c.bytes > max_bytes)) {
            break;
        }
        out.push_back(c.id);
        bytes += c.bytes;
    }
    return out;
}

// The idle rule's bounds: critical passes take more.
inline std::vector<std::uint32_t> Pick(std::vector<Candidate> candidates, std::uint64_t cutoff_tick,
                                       bool critical) {
    return Pick(std::move(candidates), cutoff_tick, critical ? MaxPerCriticalPass : MaxPerPass,
                critical ? MaxBytesPerPass * 4 : MaxBytesPerPass);
}

} // namespace VideoCore::BufferEviction
