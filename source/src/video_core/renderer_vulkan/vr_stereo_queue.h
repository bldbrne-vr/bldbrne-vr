// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// vr.ini stereo_adopt_finished: the DIBR pairs waiting to go up (OpenXRPresenter RecordStereoCopy,
// PublishStereo, AdoptStereo). Header-only (tests/test_stereo_queue.cpp).
//
// A game frame's eyes are copied into a DIBR swapchain pair in its own draw stream and published;
// the frame thread puts a published pair up once the GPU has finished it. With one waiting pair
// (0, before 2026-10-07) each new frame replaces the waiting one, so while the GPU is more than a
// game frame behind (after a stutter) every pair is replaced before it is finished and the world
// stands still until the GPU has caught up, then lurches forward (0.3-0.4 s at 3200 in the mock).
// With more pairs the newest pair the GPU has finished goes up while newer ones are still being
// drawn: the picture keeps moving at the GPU's own pace during a backlog.

#include <algorithm>
#include <span>

#include "common/types.h"

namespace Vulkan::VrStereoQueue {

// At most this many published pairs wait (stereo_adopt_finished 2); one more pair is on display.
constexpr u32 MaxWaiting = 3;
constexpr u32 MaxPairs = MaxWaiting + 1;

// stereo_adopt_finished (0 = one waiting pair, as before; 1 = two; 2 = three) -> waiting pairs.
constexpr u32 Waiting(s32 setting) {
    return static_cast<u32>(std::clamp<s32>(setting, 0, static_cast<s32>(MaxWaiting) - 1)) + 1;
}
// The swapchain pairs: the waiting ones and the one on display.
constexpr u32 Pairs(u32 waiting) {
    return waiting + 1;
}
// The late warp's capture slots (VrStereoWarp): the shown pair's, the waiting pairs' and the one
// the next frame is captured into (3 with one waiting pair, as before).
constexpr u32 LateSlots(u32 waiting) {
    return waiting + 2;
}

// UI images must survive independently of eye-pair reuse: an adopted frame can retain an
// older UI (AER, a refused copy). Reserve one chain per waiting frame plus the shown UI.
// RecordStereoCopy evicts a frame before acquiring its replacement. A concurrent present-
// thread copy may exhaust the pool temporarily; keep the shown UI rather than wait or grow.
// The pool is sized once for the session; selection uses only bounded stack storage.
constexpr u32 MaxUiChains = MaxPairs;
constexpr u32 UiChains(u32 waiting) {
    return std::clamp(waiting, 1u, MaxWaiting) + 1;
}

inline s32 ChooseUiChain(u32 count, s32 shown, std::span<const s32> queued,
                         std::span<const bool> acquired) {
    count = std::min(count, static_cast<u32>(acquired.size()));
    for (u32 i = 0; i < count; ++i) {
        const s32 chain = static_cast<s32>(i);
        if (!acquired[i] && chain != shown &&
            std::ranges::find(queued, chain) == queued.end()) {
            return chain;
        }
    }
    return -1; // never overwrite a live UI just to make progress
}

// A published pair waiting to go up. The queue is ordered oldest first (frames are published in
// draw order, so a frame whose GPU work is done means every older one's is too).
struct Entry {
    u32 pair{0};
    u64 seq{0};       // FrameTag::seq
    bool done{false}; // its GPU work has finished
};

// The newest waiting pair whose GPU work is done (-1: none). The ones before it are dropped when
// it goes up; newer ones keep waiting.
inline s32 ChooseAdopt(std::span<const Entry> queue) {
    for (s32 i = static_cast<s32>(queue.size()) - 1; i >= 0; --i) {
        if (queue[i].done) {
            return i;
        }
    }
    return -1;
}

// The waiting pair a frame thread waits for a little before it submits (WaitForStereoGpu): none
// when one is already done, else the oldest (the first to finish). -1: none.
inline s32 ChooseWait(std::span<const Entry> queue) {
    if (queue.empty() || ChooseAdopt(queue) >= 0) {
        return -1;
    }
    return 0;
}

// Which waiting pair gives way when the queue is full and a new frame comes: one older than a
// finished pair is never shown (the finished one goes up first), so the oldest goes; else the
// newest (the new frame supersedes it, and the older ones finish first). With one waiting pair
// that is always it (as before).
inline s32 ChooseVictim(std::span<const Entry> queue) {
    if (queue.empty()) {
        return -1;
    }
    return ChooseAdopt(queue) > 0 ? 0 : static_cast<s32>(queue.size()) - 1;
}

struct RecordChoice {
    u32 pair{0};
    s32 drop{-1}; // the waiting entry (queue index) the new frame replaces; -1: none
};

// The pair a new frame is copied into: with the queue full, the victim's (ChooseVictim); else the
// lowest pair neither on display (`shown`, -1: none) nor waiting. With one waiting pair: the pair
// not on display (pair 0 with none shown), replacing the waiting one, as before.
inline RecordChoice ChooseRecordPair(s32 shown, u32 waiting, std::span<const Entry> queue) {
    waiting = std::clamp<u32>(waiting, 1, MaxWaiting);
    if (queue.size() >= waiting) {
        const s32 victim = ChooseVictim(queue);
        return {queue[victim].pair, victim};
    }
    const u32 pairs = Pairs(waiting);
    for (u32 p = 0; p < pairs; ++p) {
        if (static_cast<s32>(p) == shown) {
            continue;
        }
        bool used = false;
        for (const Entry& e : queue) {
            used |= e.pair == p;
        }
        if (!used) {
            return {p, -1};
        }
    }
    // Unreachable with a consistent queue (pairs = waiting + 1); the oldest gives way.
    return queue.empty() ? RecordChoice{} : RecordChoice{queue[0].pair, 0};
}

} // namespace Vulkan::VrStereoQueue
