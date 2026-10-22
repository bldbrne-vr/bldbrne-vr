// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "core/vr/lamp_travel.h"

namespace VR::Lamps {
// Site A owns the sequence. A pulse cannot expire before the presenter actually applies it.
class Sequence {
  public:
    enum class Result { Waiting, Warp, Finished, Cancelled };
    struct Output {
        Result result{Result::Waiting};
        Pad pad{Pad::Neutral};
        const char* reason{};
    };
    void Start(Action value, int selection, std::int64_t now) {
        ++command;
        action = value;
        select_yes = selection == 1 && value == Action::Dream;
        stage = Stage::Neutral;
        started = since = now;
        clear_since = 0;
        response_delivered = false;
    }
    Output Tick(std::int64_t now, DialogState dialog, int result, bool talking,
                std::optional<PadReceipt> receipt = {}) {
        if (stage == Stage::Done)
            return {Result::Finished};
        if (now - started > 8'000'000'000)
            return Stop("action timed out; no retry");
        if (dialog == DialogState::Changed)
            return Stop("lamp dialog replaced or serial changed");
        const bool applied = receipt && receipt->pad == Desired() && receipt->applied_ns >= since &&
                             receipt->applied_ns <= now;
        if (stage == Stage::Respond && applied)
            response_delivered = true;
        // Rest / Travel answer the box with Back (No): any answer but Yes (1) then, -1 included.
        // The game leaves its answer unset or other than 0 / 2 after a Back (the 2026-10-13 run:
        // every Rest stopped "without the expected native result"); only Yes acts (the Dream).
        const bool expected = action == Action::Dream ? result == 1 : result != 1;
        if (result != -1 && !expected)
            return Stop("unexpected native dialog result");
        if (dialog == DialogState::Closed && stage != Stage::Clear) {
            if ((stage != Stage::Respond && stage != Stage::AwaitClose) || !response_delivered)
                return Stop("dialog closed before our response was delivered");
            if (!expected)
                return Stop("dialog closed without the expected native result");
            Next(Stage::Clear, now);
            return Waiting(); // publish a distinct neutral command and await its receipt
        }
        const auto held = applied ? now - receipt->applied_ns : 0;
        switch (stage) {
        case Stage::Neutral:
            if (applied && held >= 150'000'000)
                Next(select_yes ? Stage::SelectYes : Stage::Respond, now);
            break;
        case Stage::SelectYes:
            if (applied && held >= 120'000'000)
                Next(Stage::ReleaseYes, now);
            break;
        case Stage::ReleaseYes:
            if (applied && held >= 150'000'000)
                Next(Stage::Respond, now);
            break;
        case Stage::Respond:
            if (applied && held >= 120'000'000)
                Next(Stage::AwaitClose, now);
            break;
        case Stage::AwaitClose:
            break; // no repeats; only the matching native result can complete this response
        case Stage::Clear:
            if (dialog != DialogState::Closed || !expected)
                return Stop("dialog completion changed");
            if (!applied)
                break; // release must reach the pad before input is handed back
            if (action == Action::Dream || action == Action::Leave) {
                stage = Stage::Done;
                return {Result::Finished};
            }
            if (talking) {
                clear_since = 0;
                break;
            }
            if (!clear_since)
                clear_since = now;
            if (now - clear_since >= 150'000'000) {
                stage = Stage::Done;
                return {Result::Warp};
            }
            break;
        case Stage::Done:
            return {Result::Finished};
        }
        return Waiting();
    }
    // Used to renew the neutral command even when AwaitClose -> Clear keeps the same pad value.
    std::uint64_t Command() const { return command; }

  private:
    enum class Stage { Neutral, SelectYes, ReleaseYes, Respond, AwaitClose, Clear, Done };
    Stage stage{Stage::Done};
    Action action{Action::None};
    bool select_yes{}, response_delivered{};
    std::int64_t started{}, since{}, clear_since{};
    std::uint64_t command{};
    Pad Desired() const {
        if (stage == Stage::SelectYes)
            return Pad::Left;
        if (stage == Stage::Respond)
            return action == Action::Dream ? Pad::Yes : Pad::Back;
        return Pad::Neutral;
    }
    Output Waiting() const { return {Result::Waiting, Desired()}; }
    void Next(Stage value, std::int64_t now) {
        stage = value;
        since = now;
        ++command;
    }
    Output Stop(const char* reason) {
        stage = Stage::Done;
        return {Result::Cancelled, Pad::Neutral, reason};
    }
};
} // namespace VR::Lamps
