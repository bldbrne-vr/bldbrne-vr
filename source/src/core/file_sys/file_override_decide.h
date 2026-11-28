// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <chrono>
#include <optional>

namespace Core::FileSys {

// A switched layer's choice at each touch of its path (file_override.cpp OpenLayer, under the
// overrides' lock): whether an open or a stat sees the layer's file or the one under it. A stat and
// the open it goes with must see the same file, whatever the switch does between them (the layer's
// file is packed again: its size is not the game's), and the switch must still reach the file at
// the next load (a code reviews and the independent one, 2026-11-21). The game opens a map file,
// then stats it, then closes it (the mock's kernel log), within milliseconds; it touches no map file
// between loads. So:
// - a burst: touches of the path within HoldFor of the last one share its decision; the switch is
//   asked afresh at the next load;
// - a stat after that, within TrailFor of the path's last open, while that open has had no stat
//   (none before it, none after it), is that open's stat: the file that open got, arming nothing
//   (a measurement, 2026-11-21: a stat 5 s or more after its open started a burst of its own; it told the
//   size of the file the switch said then, and armed the next visit's open with it). The window
//   counts from the open's decision, before any wait for its build;
// - any other stat made before its burst's first open answers for the next open, however late
//   (taken by that open, which then has its stat); one made after an open of its burst arms nothing.
// Left open (never seen: every map stat in the 2026-11-21 mock logs came while its open's file was
// held): an open with no stat at all, then within TrailFor a load that stats before it opens.
struct LayerDecisions {
    using Clock = std::chrono::steady_clock;
    static constexpr Clock::duration HoldFor = std::chrono::seconds(5);
    static constexpr Clock::duration TrailFor = std::chrono::seconds(60);

    std::optional<bool> stat_answer; // a stat's answer, waiting for the next open
    bool held_on{false};             // the burst's decision
    bool burst_opened{false};        // the burst has an open (its stats arm nothing)
    Clock::time_point last_touch{};
    Clock::time_point last_open{};
    bool last_open_on{false}; // the last open's decision
    bool open_paired{true};   // the last open has had its stat (or there is no open yet)

    // This touch's decision where one applies without asking the switch (a stat's answer waiting
    // for this open, the burst it continues, the open a stat comes after); with `fresh` (the
    // switch, asked outside the lock) always one, a new burst on it when nothing else applies.
    // Recorded at once, so a touch on another thread meanwhile sees it.
    std::optional<bool> Decide(Clock::time_point now, bool stat, std::optional<bool> fresh) {
        const bool in_burst = last_touch != Clock::time_point{} && now - last_touch < HoldFor;
        const bool trails_open = stat && !in_burst && !open_paired &&
                                 last_open != Clock::time_point{} && now - last_open < TrailFor;
        std::optional<bool> on;
        bool took_answer = false;
        if (!stat && stat_answer) {
            on = stat_answer; // (the size the game was told: this open's file)
            stat_answer.reset();
            took_answer = true;
        } else if (in_burst) {
            on = held_on; // (the same load's)
        } else if (trails_open) {
            on = last_open_on; // (the file the open it comes after got)
        } else if (fresh) {
            on = fresh;
        } else {
            return std::nullopt;
        }
        if (!in_burst) {
            burst_opened = trails_open; // (a new burst; one a late stat starts is its open's load)
        }
        held_on = *on;
        last_touch = std::max(last_touch, now);
        if (stat) {
            if (!burst_opened) {
                stat_answer = *on; // (a size asked before the burst's open: the next open's)
            }
            open_paired = true; // (the last open has had its stat)
        } else {
            burst_opened = true;
            last_open = std::max(last_open, now);
            last_open_on = *on;
            open_paired = took_answer; // (its stat came before it, or is still to come)
        }
        return on;
    }
};

} // namespace Core::FileSys
