// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <cstdint>

// VR body: the feedback rules (body plan S12/S13), pure (no OpenXR or emulator state) so the unit
// tests (tests/test_body_feedback.cpp) can drive them:
//  - DetachWatch, per arm on the pose hook (loop thread): the hand is off the controller (the
//    arm is yours but cannot reach it), with hysteresis, and when to tick the hand for it.
//  - GhostTarget / GhostFade, on the presenter: how opaque the ghost hand at a controller should
//    be while the game owns that arm or the hand is detached.
//  - BudgetGuard, on the pose hook: the body's time budget per frame.
namespace VR::Body {

// ---- Detached hand --------------------------------------------------------------------------

// One arm's "detached" state: the arm is yours (weight >= OwnedWeight), its solve did not reach
// the target and the weapon bone is more than 3 cm from the controller; it clears below 2 cm, on
// reaching, or when the game takes the arm. A tick is asked for on each new detach, at most once
// per `min_gap_ns`, so a hand hovering at the edge of the hunter's reach does not buzz.
struct DetachWatch {
    static constexpr float OwnedWeight = 0.9f;
    static constexpr float EnterM = 0.03f;
    static constexpr float LeaveM = 0.02f;
    static constexpr std::int64_t DefaultGapNs = 500'000'000;

    bool detached{false};
    bool ticked{false}; // a tick has been asked for (last_tick_ns valid)
    std::int64_t last_tick_ns{0};
    std::uint32_t count{0}; // detaches so far

    // `solved`: the arm was solved this frame (not skipped or failed). Returns true when a tick
    // should play now.
    bool Update(float weight, bool solved, bool reached, float error_m, std::int64_t now_ns,
                std::int64_t min_gap_ns = DefaultGapNs) {
        const bool was = detached;
        if (!(weight >= OwnedWeight) || !solved || reached) {
            detached = false;
        } else {
            detached = error_m > (was ? LeaveM : EnterM);
        }
        if (!detached || was) {
            return false;
        }
        ++count;
        if (ticked && now_ns - last_tick_ns < min_gap_ns) {
            return false;
        }
        ticked = true;
        last_tick_ns = now_ns;
        return true;
    }
    void Reset() {
        detached = false;
    }
};

// ---- Ghost hands ----------------------------------------------------------------------------

// Opacity of the ghost hand at a controller (body plan S12): 0 while that arm's weight is at
// least 0.9 (the hunter's hand is on the controller), rising linearly to `alpha` as the game takes
// the arm (weight 0), and `alpha` while the hand is detached. None for an untracked controller or
// while the body is not driven (cinematic flat mode, the virtual screen).
constexpr float GhostOwnedWeight = 0.9f;
constexpr float GhostFadeSeconds = 0.08f;

inline float GhostTarget(bool driven, bool tracked, float weight, bool detached, float alpha) {
    if (!driven || !tracked || !(alpha > 0.0f)) {
        return 0.0f;
    }
    if (detached) {
        return alpha;
    }
    const float w = std::clamp(weight, 0.0f, 1.0f);
    return w >= GhostOwnedWeight ? 0.0f : alpha * (GhostOwnedWeight - w) / GhostOwnedWeight;
}

// `current` moved toward `target`: a full fade (0 <-> alpha) takes GhostFadeSeconds.
inline float GhostFade(float current, float target, float alpha, float dt) {
    const float step = std::max(alpha, 0.01f) * std::max(dt, 0.0f) / GhostFadeSeconds;
    return current < target ? std::min(current + step, target) : std::max(current - step, target);
}

// ---- Time budget ----------------------------------------------------------------------------

// The body's work per main-player pose copy (the gate, the solve and the write; not the logs or
// the recording), averaged over 5 s windows. A window whose average is over the budget is
// reported; WindowsToDisable of them in a row ask to turn the body off for the session (the game
// keeps its own pose). Single slow calls (the thread preempted) are only counted. A window with
// fewer than MinCalls calls (loading, a pause) is never judged.
struct BudgetWindow {
    bool closed{false};   // this call ended a window (the fields below are that window's)
    bool over{false};     // its average was over the budget
    bool exceeded{false}; // WindowsToDisable windows over the budget in a row
    std::uint32_t calls{0};
    std::uint32_t over_calls{0}; // single calls over the budget
    std::uint32_t over_windows{0};
    double avg_us{0.0};
    double max_us{0.0};
};

class BudgetGuard {
public:
    static constexpr std::int64_t WindowNs = 5'000'000'000;
    static constexpr std::uint32_t WindowsToDisable = 2;
    static constexpr std::uint32_t MinCalls = 30;

    // `budget_us` <= 0: measured, never over.
    BudgetWindow Add(std::int64_t work_ns, std::int64_t now_ns, float budget_us) {
        BudgetWindow out{};
        if (!started) {
            started = true;
            start_ns = now_ns;
        }
        const std::int64_t work = std::max<std::int64_t>(work_ns, 0);
        ++calls;
        sum_ns += work;
        max_ns = std::max(max_ns, work);
        const bool judged = budget_us > 0.0f;
        if (judged && static_cast<double>(work) > static_cast<double>(budget_us) * 1000.0) {
            ++over_calls;
        }
        if (now_ns - start_ns < WindowNs) {
            return out;
        }
        out.closed = true;
        out.calls = calls;
        out.over_calls = over_calls;
        out.avg_us = static_cast<double>(sum_ns) / static_cast<double>(calls) * 1e-3;
        out.max_us = static_cast<double>(max_ns) * 1e-3;
        out.over = judged && calls >= MinCalls && out.avg_us > static_cast<double>(budget_us);
        over_windows = out.over ? over_windows + 1 : 0;
        out.over_windows = over_windows;
        out.exceeded = over_windows >= WindowsToDisable;
        start_ns = now_ns;
        calls = over_calls = 0;
        sum_ns = max_ns = 0;
        return out;
    }

private:
    bool started{false};
    std::int64_t start_ns{0};
    std::int64_t sum_ns{0};
    std::int64_t max_ns{0};
    std::uint32_t calls{0};
    std::uint32_t over_calls{0};
    std::uint32_t over_windows{0};
};

} // namespace VR::Body
