// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
//
// Guest signals (Scheduler::SignalGuest: EOP/EOS/RELEASE_MEM labels and their interrupts,
// semaphore signals) and the recorder: when a sync point must submit for them, and
// FAST_CPU_ASYNC_IDLE's count of the guest submission being retired. Header-only and free of
// Vulkan, so tests/test_signal_policy.cpp models the protocol with the same functions.
#pragma once

#include <cstdint>

namespace Vulkan::SignalPolicy {

// How an encoded guest signal becomes guest-visible, and what a sync point submits for it
// (Liverpool::SyncRecorder, Scheduler::FlushGuestSignals, Scheduler::CheckEmpty).
enum class Mode : std::uint8_t {
    // The replay writes it right after the next submission it reaches (CompleteGuestSignals). A
    // sync point that finds a signal unwritten submits, even when a submission already encoded
    // after the signal will write it once replayed.
    Old,
    // FAST_CPU_ASYNC_IDLE: written as in Old, but a sync point submits only for signals encoded
    // since the last submission; the replay of that submission writes the earlier ones, so a
    // drain suffices for them (no second, near-empty submission).
    Precise,
    // FAST_CPU_SIGNAL_AT_REPLAY: the replay writes it when it reaches its packet, before any
    // submission (bbport's RecorderFences). Sync points still submit the work encoded before
    // signals that no submission follows yet, as Precise does.
    AtReplay,
};

// The producer's view: signals encoded so far, those the replay has written, and `encoded` when
// the last submission was encoded.
struct Counts {
    std::uint64_t encoded{}, completed{}, at_submit{};
};

// Encoded and not written yet.
constexpr bool Pending(const Counts& c) {
    return c.completed < c.encoded;
}
// Encoded after the last submission (no submission encoded so far writes them in Old).
constexpr bool SinceSubmit(const Counts& c) {
    return c.encoded != c.at_submit;
}
// Whether a sync point submits before it drains the recorder for the signals.
constexpr bool NeedsFlush(Mode mode, const Counts& c) {
    return mode == Mode::Old ? Pending(c) : SinceSubmit(c);
}
// Old and Precise write a signal only at a submission's replay, so one encoded since the last
// submission cannot be written yet. Precise relies on this (FAST_CPU_ASYNC_IDLE_VERIFY checks it).
constexpr bool SubmitOrderHolds(const Counts& c) {
    return !SinceSubmit(c) || Pending(c);
}
// FAST_CPU_ASYNC_IDLE: whether the guest submission being retired is the last one outstanding,
// as the old path's `num_submits == 1` saw it. The old path decremented num_submits before it
// went on; the async path decrements it when the recorder reaches the retirement, so the ones
// queued and not yet run (`async_retires`) are subtracted. Both counts under one lock.
constexpr bool LastSubmission(std::uint32_t num_submits, std::uint32_t async_retires) {
    return num_submits - async_retires == 1;
}

} // namespace Vulkan::SignalPolicy
