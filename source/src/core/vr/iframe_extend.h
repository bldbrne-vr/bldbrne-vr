// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <cmath>

#include "common/types.h"

// Dodge i-frame extension (vr.ini dodge_iframes_extra): the state machine behind
// core/vr/bloodborne_iframes.cpp, pure so tests/test_iframes.cpp can drive it.
//
// The game marks the hunter invulnerable with bit 1 of the action flags (TAE event 8, "F8"),
// re-set by every behaviour step whose animation window overlaps the event and zeroed at the end
// of every PlayerIns::Update. Step() runs once per main-player update, after the behaviour step
// (PlayerIns vt+0xE8 entry): there the bit holds exactly this update's F8, because the reset
// already ran. When a dodge's F8 window ends, the bit is set again for `extra_s` more game time
// (rounded to whole updates: 4 at 60 fps, 2 at 30 fps for the default 2/30 s), as long as the
// dodge goes on (SpEffect 392 present, on the ground, alive, no damage, grab or fall clip).
// A bit set here lives exactly like an F8 bit: until the next reset.
//
// Attacks through dodges (vr.ini dodge_attack, dodge_attack_iframes=0): a swing or a shot during a
// dodge ends its invulnerability (Input::cut): from that update on bit 1 is cleared (Output::
// cut_bit: the F8 window's own bit too) and no extension follows, for the rest of that dodge. The
// cut belongs to the dodge clip (Input::dodge_clip / dodge_start, from the TimeAct ring), not to
// its F8 window: one that arrives before the window opens (the swing and this hook run on
// different threads) waits for it (CutPending). It is over when the clip ends (an update without
// a dodge clip, no window or extension running) or the next dodge clip starts (a chained dodge
// has its own i-frames). A cut taken while no dodge clip shows yet binds to the next one that
// starts (it ends after one update without one). A damage, grab, fall or death clip (Input::
// interrupted) ends the dodge and the cut with it, and the cut only ever clears bits while a dodge
// clip plays: a knockdown that lands through a cut dodge keeps its own get-up i-frames.
namespace VR::Iframes {

constexpr float GameFrame = 1.0f / 30.0f; // the setting's unit: the game's 30 fps frame
constexpr float MaxDt = 0.1f;             // longer updates count as this
constexpr u32 MaxExtensionUpdates = 64;   // safety cap per window

// Why a window was not extended, or why an extension ended.
enum class End : u8 {
    None,
    Timer,       // the extra time is used up (the normal end)
    NoDodge,     // no SpEffect 392: not a quickstep / roll (jump, knockdown, event...)
    Airborne,    // not on the ground (fall damage stays vanilla)
    Dead,        // HP 0
    Interrupted, // a damage, grab, fall or death animation started
    NewWindow,   // a chained dodge's own F8 window started
    Stale,       // the reset did not run, our bit was still set: cleared
    Cap,         // MaxExtensionUpdates reached (dt stuck at 0)
    Disabled,    // extra_s = 0
    Attack,      // an attack during the dodge ended its invulnerability (Input::cut)
};

constexpr const char* EndName(End e) {
    switch (e) {
    case End::None:
        return "none";
    case End::Timer:
        return "timer";
    case End::NoDodge:
        return "no392";
    case End::Airborne:
        return "airborne";
    case End::Dead:
        return "dead";
    case End::Interrupted:
        return "interrupted";
    case End::NewWindow:
        return "new_window";
    case End::Stale:
        return "stale";
    case End::Cap:
        return "cap";
    case End::Disabled:
        return "disabled";
    case End::Attack:
        return "attack";
    }
    return "?";
}

struct Input {
    bool tae{};         // action flags bit 1 at the hook
    bool reset_ran{};   // the behaviour module is active: the reset zeroed the flags this update
    bool dodge{};       // SpEffect 392 ("PC rolling judgment": quicksteps, rolls, wake-up rolls)
    bool grounded{};    // the physics module's landed byte
    bool alive{};       // HP > 0
    bool interrupted{}; // a damage, grab, fall, landing or death clip started this update
    float dt{};         // this update's frame time (s)
    bool cut{};         // a swing or a shot this update ends the dodge's invulnerability
    bool dodge_clip{};  // a dodge clip (backstep, quickstep, roll) plays this update
    bool dodge_start{}; // ... and one started this update (a new id, or the same one from 0)
};

struct Output {
    bool assert_bit{};    // set bit 1 now: an extension update
    bool clear_bit{};     // clear bit 1 now: our own bit survived a skipped reset
    bool window_start{};  // this update is the first of an F8 window
    bool window_end{};    // the F8 window ended with the last update
    bool extended{};      // with window_end: an extension starts this update
    bool extension_end{}; // an extension ended (end says why)
    End end{End::None};   // with window_end && !extended, or extension_end
    bool cut_start{};     // Input::cut took effect this update (the dodge's i-frames end)
    bool cut_bit{};       // clear bit 1 now: an attack ended this dodge's invulnerability
};

class Extender {
public:
    explicit Extender(float extra_s = 2.0f * GameFrame) {
        SetExtra(extra_s);
    }

    void SetExtra(float extra_s) {
        extra_s_ = std::isfinite(extra_s) ? std::max(extra_s, 0.0f) : 0.0f;
    }
    float Extra() const {
        return extra_s_;
    }

    // Forget everything (a new player instance). The bit is left alone.
    void Reset() {
        state_ = State::Idle;
        ours_prev_ = false;
        cut_ = false;
        cut_bound_ = false;
        cut_pending_ = false;
        left_ = 0.0f;
        window_updates_ = 0;
        window_time_ = 0.0f;
        ext_updates_ = 0;
        ext_time_ = 0.0f;
    }

    Output Step(const Input& in) {
        Output o{};
        const float dt = std::isfinite(in.dt) ? std::clamp(in.dt, 0.0f, MaxDt) : GameFrame;
        // The cut lives as long as its dodge clip: the next dodge clip's start ends one taken in
        // an earlier clip; one taken before any clip showed binds to the clip that shows next. A
        // damage, grab, fall or death clip ends the dodge and the cut (a request arriving with it
        // was for the dodge it ended): a knockdown keeps its own get-up i-frames.
        if (cut_ && ((in.dodge_start && cut_bound_) || in.interrupted)) {
            cut_ = false;
        }
        cut_bound_ = cut_ && (cut_bound_ || in.dodge_clip);
        const bool fresh = in.cut && !cut_ && !in.interrupted;
        if (fresh) {
            cut_ = true;
            cut_bound_ = in.dodge_clip;
            o.cut_start = true;
        }
        Advance(in, dt, o);
        // Its clip is over (no dodge clip, no window or extension): so is the cut. One taken this
        // update without a clip waits one update for its clip to show.
        if (cut_ && !in.dodge_clip && state_ == State::Idle && !(fresh && !cut_bound_)) {
            cut_ = false;
        }
        cut_pending_ = cut_ && state_ == State::Idle;
        return o;
    }

    bool InWindow() const {
        return state_ == State::Window;
    }
    bool Extending() const {
        return state_ == State::Extend;
    }
    // The current or last window: its F8 updates and their game time, and the extension's.
    u32 WindowUpdates() const {
        return window_updates_;
    }
    float WindowTime() const {
        return window_time_;
    }
    u32 ExtensionUpdates() const {
        return ext_updates_;
    }
    float ExtensionTime() const {
        return ext_time_;
    }
    float Left() const {
        return left_;
    }
    // An attack ended the current dodge's invulnerability (Input::cut): until its clip ends or the
    // next dodge clip starts.
    bool Cut() const {
        return cut_;
    }
    // ... and no window of it runs now: the cut waits for the window (or outlives it).
    bool CutPending() const {
        return cut_pending_;
    }

private:
    enum class State : u8 { Idle, Window, Extend };

    // The window and its extension (the cut ends both while a dodge clip plays: never another
    // clip's F8 window, a get-up's or an event's, should the cut outlive its dodge).
    void Advance(const Input& in, float dt, Output& o) {
        const bool cut = cut_ && in.dodge_clip;
        if (in.tae && ours_prev_ && !in.reset_ran) {
            // No reset ran since our last write: the bit is ours, not this update's F8.
            o.clear_bit = true;
            if (state_ == State::Extend) {
                o.extension_end = true;
                o.end = End::Stale;
            }
            state_ = State::Idle;
            ours_prev_ = false;
            return;
        }
        const bool f8 = in.tae && (in.reset_ran || !ours_prev_);
        if (state_ == State::Extend && f8) {
            o.extension_end = true;
            o.end = End::NewWindow;
            state_ = State::Idle;
        }
        if (state_ == State::Extend && cut) {
            o.extension_end = true;
            o.end = End::Attack;
            state_ = State::Idle;
            ours_prev_ = false;
        }
        if (state_ == State::Idle) {
            if (f8) {
                state_ = State::Window;
                window_extra_s_ = extra_s_;
                window_updates_ = 1;
                window_time_ = dt;
                ext_updates_ = 0;
                ext_time_ = 0.0f;
                o.window_start = true;
            }
            ours_prev_ = false;
            o.cut_bit = cut && f8;
            return;
        }
        if (state_ == State::Window) {
            if (f8) {
                ++window_updates_;
                window_time_ += dt;
                ours_prev_ = false;
                o.cut_bit = cut;
                return;
            }
            o.window_end = true;
            const End why = cut ? End::Attack : window_extra_s_ > 0.0f ? Check(in) : End::Disabled;
            if (why != End::None) {
                o.end = why;
                state_ = State::Idle;
                ours_prev_ = false;
                return;
            }
            o.extended = true;
            state_ = State::Extend;
            left_ = window_extra_s_;
        }
        // Extend: one more update while the time lasts (to the nearest update) and the dodge runs.
        End why = cut ? End::Attack : left_ > 0.5f * dt ? Check(in) : End::Timer;
        if (why == End::None && ext_updates_ >= MaxExtensionUpdates) {
            why = End::Cap;
        }
        if (why != End::None) {
            o.extension_end = true;
            o.end = why;
            state_ = State::Idle;
            ours_prev_ = false;
            return;
        }
        o.assert_bit = true;
        left_ -= dt;
        ++ext_updates_;
        ext_time_ += dt;
        ours_prev_ = true;
    }

    static End Check(const Input& in) {
        if (!in.dodge) {
            return End::NoDodge;
        }
        if (!in.grounded) {
            return End::Airborne;
        }
        if (!in.alive) {
            return End::Dead;
        }
        if (in.interrupted) {
            return End::Interrupted;
        }
        return End::None;
    }

    float extra_s_{};
    float window_extra_s_{};
    State state_{State::Idle};
    bool ours_prev_{false};
    bool cut_{false};
    bool cut_bound_{false};   // the cut's dodge clip has shown (the next clip's start ends it)
    bool cut_pending_{false}; // the cut waits for its window (or outlived it)
    float left_{0.0f};
    u32 window_updates_{0};
    float window_time_{0.0f};
    u32 ext_updates_{0};
    float ext_time_{0.0f};
};

// Lean i-frames (vr.ini lean_iframes_ms, 200 by default): a quick lean of the head (the lean
// detector, core/vr/lean_dodge.h; nothing is pressed, no dodge animation plays) asks for
// `request` ms of bit 1: that much game time at any frame rate (100 ms: 3 updates at 30 fps, 9 at
// 90), to the nearest update, as an extension's. The bit is set
// in each update the reset ran (it then lives exactly that update); an update the reset skips
// (the behaviour idle) waits, up to MaxWaits in a row. It ends with death, a damage, grab, fall
// or death clip, or an attack ending a dodge's i-frames (Input::cut, or a cut dodge). An update
// that finds the game's own bit set (a dodge's F8) just counts. Our bit seen again in an update
// the reset skipped is cleared (it is not that update's F8).
struct LeanInput {
    u32 request{};      // a lean asked for this many ms now (0: none)
    float dt{};         // this update's frame time (s), as Input::dt
    bool tae{};         // bit 1 at the hook, as Input::tae
    bool reset_ran{};   // as Input::reset_ran
    bool alive{};       // HP > 0
    bool interrupted{}; // as Input::interrupted
    bool cut{};         // as Input::cut
};

struct LeanOutput {
    bool assert_bit{}; // set bit 1 now
    bool clear_bit{};  // clear bit 1 now: our bit survived a skipped reset (not an F8)
    bool start{};      // a request was taken this update
    bool covered{};    // an update of the grant the game's own bit already covered
    const char* end{}; // the grant ended this update: "done", "dead", "hit", "attack", "no_reset",
                       // "stale"
};

class LeanGrant {
public:
    static constexpr u32 MaxMs = 300;     // per request
    static constexpr u32 MaxUpdates = 64; // per grant, whatever dt says
    static constexpr u32 MaxWaits = 3;    // updates in a row without the reset

    LeanOutput Step(const LeanInput& in) {
        LeanOutput o{};
        const float dt = std::isfinite(in.dt) ? std::clamp(in.dt, 0.0f, MaxDt) : GameFrame;
        if (ours_prev_ && in.tae && !in.reset_ran) {
            // Our bit outlived its update. A request that came with it starts next update.
            ours_prev_ = false;
            o.clear_bit = true;
            pending_ = std::max(pending_, in.request);
            return o;
        }
        ours_prev_ = false;
        const u32 request = std::max(in.request, pending_);
        pending_ = 0;
        if (request > 0) {
            left_ = static_cast<float>(std::min(request, MaxMs)) * 0.001f;
            updates_ = 0;
            waits_ = 0;
            active_ = true;
            o.start = true;
        }
        if (!active_) {
            return o;
        }
        const char* why = !in.alive        ? "dead"
                          : in.interrupted ? "hit"
                          : in.cut         ? "attack"
                                           : nullptr;
        if (why == nullptr && !in.reset_ran) {
            // The behaviour is idle (an event, a load): a bit set now would outlive the update.
            if (++waits_ <= MaxWaits) {
                return o;
            }
            why = "no_reset";
        }
        // The time is used up (to the nearest update), or the cap.
        if (why == nullptr && (!(left_ > 0.5f * dt) || updates_ >= MaxUpdates)) {
            why = "done";
        }
        if (why != nullptr) {
            Stop();
            o.end = why;
            return o;
        }
        waits_ = 0;
        left_ -= dt;
        ++updates_;
        if (in.tae) {
            o.covered = true;
        } else {
            o.assert_bit = true;
            ours_prev_ = true;
        }
        return o;
    }

    void Reset() {
        Stop();
        pending_ = 0;
        ours_prev_ = false;
    }
    bool Active() const {
        return active_;
    }
    float Left() const {
        return left_;
    }
    // Our bit is set from the last update (Input::tae is ours, not an F8, until a reset runs).
    bool OursPrev() const {
        return ours_prev_;
    }

private:
    void Stop() {
        active_ = false;
        left_ = 0.0f;
        updates_ = 0;
        waits_ = 0;
    }

    bool active_{false};
    float left_{0.0f}; // game time still to cover (s)
    u32 updates_{0};
    u32 waits_{0};
    u32 pending_{0}; // a request taken in a stale update
    bool ours_prev_{false};
};

// Animation ids (TimeAct ring: group * 1000000 + animation; groups above 99 are throws and event
// animations).
constexpr s32 ClipBase(s32 id) {
    return id >= 0 && id / 1'000'000 <= 99 ? id % 1'000'000 : -1;
}
// Backsteps, quicksteps, rolls, wake-up rolls (7000-7799; 7800 is the jump).
constexpr bool IsDodgeClip(s32 id) {
    const s32 base = ClipBase(id);
    return base >= 7000 && base < 7800;
}
// Ends a dodge: the jump, falls and landings, grabs, guard reactions, damage, knockdowns, death
// (7800-24999).
constexpr bool IsInterruptClip(s32 id) {
    const s32 base = ClipBase(id);
    return base >= 7800 && base < 25000;
}

} // namespace VR::Iframes
