// SPDX-License-Identifier: GPL-2.0-or-later

// Cinematic flat mode (core/vr/cinematic_state.h): the state machine stepped like the main
// player's update (60 Hz, steady-clock nanoseconds), the fade brightness, and the ladder gate
// (climbs at 30 Hz, the rate the plan's timings were taken at, and at 60 Hz).

#include <algorithm>
#include <cmath>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/cinematic_state.h"

namespace {

namespace C = VR::Cinematic;

constexpr s64 Ms = C::Millis;
constexpr s64 Frame = 16'666'667;   // one main-player update at 60 fps
constexpr s64 Frame30 = 33'333'334; // at 30 fps

C::Signals Nothing() {
    C::Signals s{};
    s.throw_valid = true;
    s.event_valid = true;
    s.ids_valid = true;
    s.hp_valid = true;
    s.hp = 500;
    s.hp_max = 500;
    s.ladder.valid = true; // off the ladder
    s.ladder.object_valid = true;
    return s;
}

C::Signals Event(s32 id) {
    C::Signals s = Nothing();
    s.event = true;
    s.event_id = id;
    return s;
}

C::Signals Throw(u8 state, bool aligning = false) {
    C::Signals s = Nothing();
    s.throw_state = state;
    s.aligning = aligning;
    return s;
}

struct Change {
    s64 t;
    C::Step step;
};

// Steps the machine once per update and records every phase change and ladder event.
struct Driver {
    C::Machine machine{};
    C::Config cfg{};
    s64 t{1000 * Ms};
    s64 frame{Frame};
    bool allowed{true};
    bool can_enter{true}; // first-person VR is being shown
    s64 projection{-1};   // the presenter's word: VR (the projection) shown since, -1 = the screen
    std::vector<Change> changes;
    std::vector<Change> ladder;          // updates with a ladder event (enter, exit, skip)
    std::vector<C::ReturnTimes> returns; // every finished way back (Returning -> Off)
    C::Step last{};

    // The rule before 2026-10-05: back only when every signal has been off 200 ms, the screen
    // dims (FadeBack), VR fades in over the same 150 ms, and Off 0.5 s after that.
    void Legacy() {
        cfg.exit_ns = 200 * Ms;
        cfg.return_fade_ns = -1;
        cfg.release = false;
    }

    C::Step Tick(const C::Signals& s, bool suppress = false) {
        last = machine.Update(s, t, cfg, allowed, suppress, can_enter, projection);
        if (last.returned) {
            returns.push_back(last.times);
        }
        if (last.changed) {
            changes.push_back({t, last});
        }
        if (last.ladder.event != C::LadderEvent::None) {
            ladder.push_back({t, last});
        }
        t += frame;
        return last;
    }
    void Run(const C::Signals& s, s64 duration) {
        const s64 end = t + duration;
        while (t < end) {
            Tick(s);
        }
    }
    C::Phase Phase() const {
        return machine.GetPhase();
    }
    // Time (ms after `from`) of the first change into `phase`, -1 if none.
    double When(C::Phase phase, s64 from) const {
        for (const auto& c : changes) {
            if (c.step.to == phase && c.t >= from) {
                return static_cast<double>(c.t - from) / static_cast<double>(Ms);
            }
        }
        return -1.0;
    }
    size_t Count(C::Phase phase) const {
        size_t n = 0;
        for (const auto& c : changes) {
            n += c.step.to == phase ? 1 : 0;
        }
        return n;
    }
    // The first change into `phase` at or after `from` (nullptr if none).
    const Change* First(C::Phase phase, s64 from = 0) const {
        for (const auto& c : changes) {
            if (c.step.to == phase && c.t >= from) {
                return &c;
            }
        }
        return nullptr;
    }
    // Fades out that a cut turned round before the flat screen showed (a dim blink).
    size_t Aborts() const {
        size_t n = 0;
        for (const auto& c : changes) {
            n += c.step.from == C::Phase::FadeOut && c.step.to == C::Phase::Returning ? 1 : 0;
        }
        return n;
    }
    size_t LadderCount(C::LadderEvent event) const {
        size_t n = 0;
        for (const auto& c : ladder) {
            n += c.step.ladder.event == event ? 1 : 0;
        }
        return n;
    }
    // The first ladder event of a kind at or after `from` (nullptr if none).
    const Change* FirstLadder(C::LadderEvent event, s64 from = 0) const {
        for (const auto& c : ladder) {
            if (c.step.ladder.event == event && c.t >= from) {
                return &c;
            }
        }
        return nullptr;
    }
};

double MsOf(s64 ns) {
    return static_cast<double>(ns) / static_cast<double>(Ms);
}

std::string Reason(const Change* c) {
    return c != nullptr ? std::string(c->step.reason) : std::string("(none)");
}

std::string LadderReason(const Change* c) {
    return c != nullptr ? std::string(c->step.ladder.reason) : std::string("(none)");
}

// A hunter on a ladder as the main-player update sees it: the ladder state, the ladder object
// (rung, top) and the clip each state pushes on its first update. The rung counts at the start
// of a step (one of the two possible timings; the other is one update later).
struct Climber {
    explicit Climber(Driver& driver) : d{driver} {}

    Driver& d;
    s32 rungs{12}; // the ladder's length
    s32 top{0};    // the ladder object's fields
    s32 rung{0};
    s32 object{0}; // 1 on the ladder, 2 getting on, 0 none
    s32 hp{500};
    s32 hp_max{500};
    bool object_valid{true};
    bool ids{true};                // the TimeAct ids can be read (else no ladder clips)
    s32 event{-1};                 // an event animation at the same time (-1 = none)
    std::function<void()> on_tick; // after every update (a presenter sample)

    C::Signals Sample(s8 state, s32 clip) const {
        C::Signals s = event >= 0 ? Event(event) : Nothing();
        s.hp = hp;
        s.hp_max = hp_max;
        s.ladder.state = state;
        s.ladder.object_valid = object_valid;
        s.ladder.object_state = object;
        s.ladder.rung = rung;
        s.ladder.top = top;
        s.ladder.clip = ids ? clip : -1;
        return s;
    }
    // One state for `duration` (whole updates); its clip is pushed on the first update.
    void Hold(s8 state, s32 clip, s64 duration, bool suppress_first = false) {
        const s64 updates = std::max<s64>((duration + d.frame / 2) / d.frame, 1);
        for (s64 i = 0; i < updates; ++i) {
            d.Tick(Sample(state, i == 0 ? clip : -1), i == 0 && suppress_first);
            if (on_tick) {
                on_tick();
            }
        }
    }
    // The prompt moves the hunter onto the ladder (0.2 s), then the mount clip plays; the rung
    // count is set after the move.
    void MountBelow() {
        object = 2;
        rung = -1;
        top = 1;
        Hold(-1, -1, 200 * Ms);
        object = 1;
        rung = 0;
        top = rungs;
        Hold(0, 101010, 900 * Ms);
    }
    void MountAbove() {
        object = 2;
        rung = 2;
        top = 2;
        Hold(-1, -1, 200 * Ms);
        object = 1;
        rung = rungs;
        top = rungs;
        Hold(1, 101020, 2000 * Ms);
    }
    // Steps, alternating hands; the rung counts at the start of each.
    void Up(s32 count) {
        for (s32 i = 0; i < count; ++i) {
            ++rung;
            const bool right = (rung & 1) != 0;
            Hold(right ? 2 : 3, right ? 101011 : 101012, 400 * Ms);
        }
    }
    void FastUp(s32 count) {
        for (s32 i = 0; i < count; ++i) {
            ++rung; // the fast steps do not write the state
            Hold(3, (rung & 1) != 0 ? 101015 : 101016, 300 * Ms);
        }
    }
    void Down(s32 count) {
        for (s32 i = 0; i < count; ++i) {
            --rung;
            const bool right = (rung & 1) != 0;
            Hold(right ? 4 : 5, right ? 101021 : 101022, 400 * Ms);
        }
    }
    void Idle(s64 duration) {
        Hold(8, 101030, duration);
    }
    // Getting off: the extra step over-runs the rung count and resets the ladder object.
    void ExitTop() {
        object = rung = top = 0;
        Hold(-1, 101013, 2000 * Ms);
    }
    void ExitBottom() {
        object = rung = top = 0;
        Hold(-1, 101023, 1067 * Ms);
    }
    void Off(s64 duration) {
        object = rung = top = 0;
        Hold(-1, -1, duration);
    }
};

} // namespace

TEST(CinematicClassify, EventIds) {
    EXPECT_EQ(C::ClassifyEvent(101100), C::Category::Objects); // Open Door
    EXPECT_EQ(C::ClassifyEvent(101121), C::Category::Objects);
    EXPECT_EQ(C::ClassifyEvent(101141), C::Category::Objects); // Open Chest
    EXPECT_EQ(C::ClassifyEvent(101144), C::Category::Objects); // coffin
    EXPECT_EQ(C::ClassifyEvent(101150), C::Category::Objects); // Operate Device
    EXPECT_EQ(C::ClassifyEvent(101151), C::Category::Objects);
    EXPECT_EQ(C::ClassifyEvent(101200), C::Category::Objects); // device and revival
    EXPECT_EQ(C::ClassifyEvent(101230), C::Category::Objects);
    EXPECT_EQ(C::ClassifyEvent(101240), C::Category::Objects);
    EXPECT_EQ(C::ClassifyEvent(101140), C::Category::Pickup);
    EXPECT_EQ(C::ClassifyEvent(101170), C::Category::Lamps);
    EXPECT_EQ(C::ClassifyEvent(101171), C::Category::Lamps);
    EXPECT_EQ(C::ClassifyEvent(101160), C::Category::Lamps);
    EXPECT_EQ(C::ClassifyEvent(101190), C::Category::Lamps);
    EXPECT_EQ(C::ClassifyEvent(100111), C::Category::Scripted);
    EXPECT_EQ(C::ClassifyEvent(101260), C::Category::Scripted);
    EXPECT_EQ(C::ClassifyEvent(101330), C::Category::Scripted);
    EXPECT_EQ(C::ClassifyEvent(103000), C::Category::Scripted);
    EXPECT_EQ(C::ClassifyEvent(101130), C::Category::Scripted); // unknown event animation
    // The Doll's Level Up kneel: down, held (the menu up), up.
    EXPECT_EQ(C::ClassifyEvent(101290), C::Category::LevelUp);
    EXPECT_EQ(C::ClassifyEvent(101270), C::Category::LevelUp);
    EXPECT_EQ(C::ClassifyEvent(101271), C::Category::LevelUp);
    EXPECT_EQ(C::ClassifyEvent(101272), C::Category::LevelUp);
    EXPECT_EQ(C::ClassifyEvent(101273), C::Category::Scripted);
    EXPECT_EQ(C::ClassifyEvent(101280), C::Category::Scripted);
    EXPECT_EQ(C::ClassifyEvent(-1), C::Category::Scripted);
    EXPECT_EQ(C::ClassifyEvent(100033000), C::Category::Scripted); // not an event animation
    EXPECT_FALSE(C::IsEventAnimId(99999));
    EXPECT_TRUE(C::IsEventAnimId(100000));
    EXPECT_TRUE(C::IsEventAnimId(103999));
    EXPECT_FALSE(C::IsEventAnimId(104005)); // gestures
    EXPECT_EQ(C::ThrowCategory(1), C::Category::Visceral);
    EXPECT_EQ(C::ThrowCategory(5), C::Category::Visceral);
    EXPECT_EQ(C::ThrowCategory(2), C::Category::Grabbed);
    EXPECT_EQ(C::ThrowCategory(6), C::Category::Grabbed);
}

TEST(CinematicClassify, PickEventId) {
    const s32 blend[] = {100033000, 3010, 101150};
    EXPECT_EQ(C::PickEventId(blend, 3, 101100), 101150); // the pending event clip wins
    const s32 none[] = {100033000, 3010};
    EXPECT_EQ(C::PickEventId(none, 2, 101100), 101100); // else the request
    EXPECT_EQ(C::PickEventId(nullptr, 0, -1), -1);
}

// The old rule (cinematic_release=0, cinematic_exit_ms=200, cinematic_return_fade_ms=-1).
TEST(CinematicMachine, DoorTimelineOldRule) {
    Driver d;
    d.Legacy();
    d.Run(Nothing(), 200 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    const s64 start = d.t;
    const C::Step first = d.Tick(Event(101100));
    EXPECT_TRUE(first.changed);
    EXPECT_EQ(first.to, C::Phase::FadeOut);
    EXPECT_EQ(first.category, C::Category::Objects);
    d.Run(Event(101100), 2800 * Ms - Frame);
    const s64 end = d.t; // first update without the bit
    d.Run(Nothing(), 2000 * Ms);
    EXPECT_NEAR(d.When(C::Phase::Flat, start), 150.0, 17.0);
    EXPECT_NEAR(d.When(C::Phase::FadeBack, end), 200.0, 20.0);
    EXPECT_NEAR(d.When(C::Phase::Returning, end), 350.0, 34.0);
    EXPECT_NEAR(d.When(C::Phase::Off, end), 1000.0, 50.0);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_EQ(d.changes.size(), 5u);
    // The heading re-sync is requested exactly once, on entering Returning.
    u32 resyncs = 0;
    for (const auto& c : d.changes) {
        if (c.step.resync) {
            ++resyncs;
            EXPECT_EQ(c.step.to, C::Phase::Returning);
        }
    }
    EXPECT_EQ(resyncs, 1u);
}

TEST(CinematicMachine, ShortPickupHoldsMinimum) {
    Driver d;
    const s64 start = d.t;
    d.Run(Event(101140), 300 * Ms);
    d.Run(Nothing(), 2000 * Ms);
    EXPECT_EQ(d.changes.front().step.category, C::Category::Pickup);
    // FadeOut 150 ms, then at least 600 ms flat; back to VR at once (no FadeBack).
    EXPECT_NEAR(d.When(C::Phase::Returning, start), 750.0, 17.0);
    EXPECT_EQ(d.Count(C::Phase::FadeBack), 0u);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
}

TEST(CinematicMachine, PickupToggleOff) {
    Driver d;
    d.cfg.categories[static_cast<size_t>(C::Category::Pickup)] = false;
    d.Run(Event(101140), 500 * Ms);
    EXPECT_TRUE(d.changes.empty());
    EXPECT_EQ(d.last.category, C::Category::Pickup); // detected, not requested
    EXPECT_FALSE(d.last.raw);
    d.Run(Nothing(), 100 * Ms);
    d.Run(Event(101100), 500 * Ms); // a door still goes flat
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
}

// cinematic_levelup=0 (the default): the Doll's kneel (101290, then 101270 while Level Up is up,
// 101272 to rise) never goes flat, so Level Up is on the menu panel in first-person VR; other
// scripted animations still do. cinematic_levelup=1: flat as before.
TEST(CinematicMachine, LevelUpKneelStaysInVr) {
    Driver d;
    d.Run(Event(101290), 300 * Ms);
    d.Run(Event(101270), 3000 * Ms);
    d.Run(Event(101272), 1500 * Ms);
    EXPECT_TRUE(d.changes.empty());
    EXPECT_EQ(d.last.category, C::Category::LevelUp); // detected, not requested
    EXPECT_FALSE(d.last.raw);
    d.Run(Nothing(), 200 * Ms);
    d.Run(Event(101260), 500 * Ms); // another scripted animation still goes flat
    EXPECT_EQ(d.Phase(), C::Phase::Flat);

    Driver flat;
    flat.cfg.categories[C::CategoryIndex(C::Category::LevelUp)] = true;
    flat.Run(Event(101290), 300 * Ms);
    flat.Run(Event(101270), 1000 * Ms);
    EXPECT_EQ(flat.Phase(), C::Phase::Flat);
    EXPECT_EQ(flat.changes.front().step.category, C::Category::LevelUp);
}

TEST(CinematicMachine, MasterSwitchOff) {
    Driver d;
    d.cfg.enabled = false;
    d.Run(Event(101100), 1000 * Ms);
    d.Run(Throw(1), 1000 * Ms);
    EXPECT_TRUE(d.changes.empty());
}

TEST(CinematicMachine, NoIdsCountAsObjects) {
    Driver d;
    d.cfg.categories[static_cast<size_t>(C::Category::Scripted)] = false;
    C::Signals s = Event(-1);
    s.ids_valid = false;
    d.Tick(s);
    EXPECT_EQ(d.last.category, C::Category::Objects);
    EXPECT_EQ(d.Phase(), C::Phase::FadeOut);
}

TEST(CinematicMachine, ThrowNeedsTwoAnimatingUpdates) {
    Driver d;
    // A throw whose alignment fails: state 1 with the alignment flag, then zeroed.
    d.Run(Throw(1, true), 1000 * Ms);
    d.Run(Nothing(), 100 * Ms);
    EXPECT_TRUE(d.changes.empty());
    // One animating update is not enough, two are.
    d.Tick(Throw(1, true));
    d.Tick(Throw(1));
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    d.Tick(Throw(1));
    EXPECT_EQ(d.Phase(), C::Phase::FadeOut);
    EXPECT_EQ(d.last.category, C::Category::Visceral);
    d.Run(Throw(1), 4000 * Ms);
    d.Run(Nothing(), 2000 * Ms);
    EXPECT_EQ(d.Count(C::Phase::Flat), 1u);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
}

TEST(CinematicMachine, GrabbedAndEscape) {
    Driver d;
    d.Run(Throw(2), 3000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    EXPECT_EQ(d.changes.front().step.category, C::Category::Grabbed);
    d.Run(Throw(4), 1500 * Ms); // escaped: still the same throw
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    EXPECT_EQ(d.Count(C::Phase::FadeBack), 0u);
    d.Run(Nothing(), 1500 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
}

TEST(CinematicMachine, UnknownThrowStateFallback) {
    Driver d;
    d.Tick(Throw(9));
    d.Tick(Throw(9));
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    d.Tick(Throw(9));
    EXPECT_EQ(d.Phase(), C::Phase::FadeOut);
    EXPECT_EQ(d.last.category, C::Category::Visceral); // odd: the hunter throws
}

TEST(CinematicMachine, ThrowToggles) {
    Driver d;
    d.cfg.categories[static_cast<size_t>(C::Category::Visceral)] = false;
    d.Run(Throw(1), 2000 * Ms);
    EXPECT_TRUE(d.changes.empty());
    d.Run(Nothing(), 100 * Ms);
    d.Run(Throw(2), 500 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
}

TEST(CinematicMachine, ThrowBeatsEventCategory) {
    Driver d;
    C::Signals s = Throw(1);
    s.event = true;
    s.event_id = 101100;
    d.Run(s, 500 * Ms);
    EXPECT_EQ(d.last.category, C::Category::Visceral);
}

TEST(CinematicMachine, StuckThrowTimesOut) {
    Driver d;
    const s64 start = d.t;
    bool stuck = false;
    while (d.t < start + 25'000 * Ms) {
        stuck = d.Tick(Throw(2)).throw_stuck || stuck;
    }
    EXPECT_TRUE(stuck);
    // Ignored after 20 s: back to VR although the state never cleared.
    EXPECT_NEAR(d.When(C::Phase::Returning, start), 20'050.0, 34.0);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    // The same stuck state never re-enters; a new throw after it clears does.
    d.Run(Throw(2), 2000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    d.Tick(Nothing());
    d.Run(Throw(2), 500 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
}

// cinematic_death_hold=1 (the default before 2026-10-07): a fatal grab stays flat until the respawn.
TEST(CinematicMachine, DeathHoldAfterFatalGrab) {
    Driver d;
    d.cfg.death_hold = true;
    d.Run(Throw(2), 3000 * Ms);
    d.Run(Throw(6), 2000 * Ms);
    C::Signals dead = Nothing();
    dead.hp = 0;
    d.Run(dead, 5000 * Ms); // the throw is over, HP stays 0: still flat
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    EXPECT_EQ(d.Count(C::Phase::FadeBack), 0u);
    d.Run(Nothing(), 1000 * Ms); // respawned (HP back)
    EXPECT_EQ(d.Phase(), C::Phase::Off);

    // Without the hold, or when the period was not a throw, death does not hold.
    Driver e;
    e.cfg.death_hold = false;
    e.Run(Throw(2), 1000 * Ms);
    e.Run(dead, 2000 * Ms);
    EXPECT_EQ(e.Phase(), C::Phase::Off);
    Driver f;
    f.cfg.death_hold = true;
    f.Run(Event(101100), 1000 * Ms);
    f.Run(dead, 2000 * Ms);
    EXPECT_EQ(f.Phase(), C::Phase::Off);
}

TEST(CinematicMachine, ChainedStatesWithGap) {
    Driver d;
    d.Run(Event(101150), 1000 * Ms);
    d.Tick(Nothing()); // one update between two chained Event states
    d.Run(Event(101151), 1000 * Ms);
    d.Run(Nothing(), 1500 * Ms);
    EXPECT_EQ(d.Count(C::Phase::Flat), 1u);
    EXPECT_EQ(d.Count(C::Phase::Returning), 1u);
}

TEST(CinematicMachine, SignalDuringFadeBackAndReturning) {
    Driver d;
    d.Legacy();
    d.Run(Event(101100), 1000 * Ms);
    d.Run(Nothing(), 250 * Ms); // into FadeBack
    EXPECT_EQ(d.Phase(), C::Phase::FadeBack);
    d.Tick(Event(103000));
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    EXPECT_EQ(d.last.category, C::Category::Scripted);
    d.Run(Event(103000), 500 * Ms);
    d.Run(Nothing(), 600 * Ms); // past min, exit and FadeBack: Returning (not yet Off)
    EXPECT_EQ(d.Phase(), C::Phase::Returning);
    d.Tick(Event(101100));
    EXPECT_EQ(d.Phase(), C::Phase::FadeOut);
}

TEST(CinematicMachine, HardCut) {
    Driver d;
    d.cfg.fade_ns = 0;
    d.cfg.return_fade_ns = 0;
    d.Tick(Event(101100));
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    d.Run(Event(101100), 1000 * Ms);
    const s64 end = d.t;
    d.Run(Nothing(), 1000 * Ms);
    EXPECT_NEAR(d.When(C::Phase::Returning, end), 50.0, 17.0);
    EXPECT_EQ(d.Count(C::Phase::FadeBack), 0u);
    bool resync = false;
    for (const auto& c : d.changes) {
        resync = resync || (c.step.to == C::Phase::Returning && c.step.resync);
    }
    EXPECT_TRUE(resync);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
}

TEST(CinematicMachine, NotAllowedIsOffWithoutFade) {
    Driver d;
    d.Run(Event(101100), 500 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    d.allowed = false; // the screen chosen by hand (or VR off)
    d.Tick(Event(101100));
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_EQ(d.changes.back().step.from, C::Phase::Flat);
    d.Run(Event(101100), 500 * Ms); // never enters while not allowed
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    d.allowed = true;
    d.Tick(Event(101100));
    EXPECT_EQ(d.Phase(), C::Phase::FadeOut);
}

TEST(CinematicMachine, EscapeHatch) {
    Driver d;
    d.Run(Event(101110), 1000 * Ms); // a long gate
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    // Menu hold: the screen chosen by hand (Off at once) ...
    d.allowed = false;
    d.Tick(Event(101110), true);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    // ... and back: the rest of this animation stays in VR.
    d.allowed = true;
    d.Run(Event(101110), 3000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_TRUE(d.machine.Suppressed());
    EXPECT_FALSE(d.last.raw);
    // Once every signal has been off for exit_ns the next animation goes flat again.
    d.Run(Nothing(), 300 * Ms);
    EXPECT_FALSE(d.machine.Suppressed());
    d.Tick(Event(101100));
    EXPECT_EQ(d.Phase(), C::Phase::FadeOut);
}

// Menu held during a cinematic (the display mode stays Auto): first-person VR comes back at once,
// and the rest of the animation stays in VR.
TEST(CinematicMachine, EscapeHatchReturnsAtOnce) {
    Driver d;
    d.Run(Event(101110), 1000 * Ms); // a long gate
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    const s64 held = d.t;
    d.Tick(Event(101110), true);
    EXPECT_EQ(d.Phase(), C::Phase::Returning); // the screen cuts to black, VR fades in
    EXPECT_EQ(std::string(d.last.reason), std::string("escape hatch"));
    EXPECT_TRUE(d.last.resync);
    d.Run(Event(101110), 2000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_NEAR(d.When(C::Phase::Returning, held), 0.0, 1.0);
    EXPECT_EQ(d.Count(C::Phase::Flat), 1u);
    EXPECT_TRUE(d.machine.Suppressed());

    // During the fade out: straight back (VR was still driven, so no heading re-sync).
    Driver e;
    e.Tick(Event(101100));
    e.Tick(Event(101100));
    EXPECT_EQ(e.Phase(), C::Phase::FadeOut);
    e.Tick(Event(101100), true);
    EXPECT_EQ(e.Phase(), C::Phase::Returning);
    EXPECT_FALSE(e.last.resync);
    e.Run(Event(101100), 1000 * Ms);
    EXPECT_EQ(e.Phase(), C::Phase::Off);

    // A fatal grab's death hold does not keep it flat either.
    Driver f;
    f.cfg.death_hold = true;
    f.Run(Throw(2), 1000 * Ms);
    C::Signals dead = Nothing();
    dead.hp = 0;
    f.Run(dead, 1000 * Ms);
    EXPECT_EQ(f.Phase(), C::Phase::Flat);
    f.Tick(dead, true);
    EXPECT_EQ(f.Phase(), C::Phase::Returning);
}

TEST(CinematicMachine, SuppressWhileOffIsIgnored) {
    Driver d;
    d.Tick(Nothing(), true);
    d.Tick(Event(101100));
    EXPECT_EQ(d.Phase(), C::Phase::FadeOut);
}

// A flat period only starts from first-person VR that is being shown (with the virtual screen
// already up it would only dim the screen).
TEST(CinematicMachine, StartsOnlyFromVr) {
    Driver d;
    d.can_enter = false;
    d.Run(Event(101100), 500 * Ms);
    EXPECT_TRUE(d.changes.empty());
    d.can_enter = true;
    d.Tick(Event(101100));
    EXPECT_EQ(d.Phase(), C::Phase::FadeOut);
    // Once started it runs its course.
    d.can_enter = false;
    d.Run(Event(101100), 500 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
}

TEST(CinematicMachine, LongGapForcesOff) {
    Driver d;
    d.Run(Event(101100), 1000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    d.t += 6000 * Ms; // loading: no player update
    d.Tick(Event(101100));
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_EQ(std::string(d.last.reason), std::string("no player update"));
    d.Tick(Event(101100)); // still on after the gap: a new rising edge
    EXPECT_EQ(d.Phase(), C::Phase::FadeOut);
}

// A hitch (a shader compile on the first visceral) in the middle of a flat period: no Off, no
// fade, no second flat period, and the death hold survives it.
TEST(CinematicMachine, HitchKeepsFlat) {
    Driver d;
    d.cfg.death_hold = true;
    d.Run(Throw(2), 1000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    const size_t changes = d.changes.size();
    d.t += 1500 * Ms;
    d.Run(Throw(2), 500 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    EXPECT_EQ(d.changes.size(), changes);
    d.Run(Throw(6), 200 * Ms);
    C::Signals dead = Nothing();
    dead.hp = 0;
    d.t += 3000 * Ms;
    d.Run(dead, 2000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    EXPECT_TRUE(d.machine.HeldByThrow());
    EXPECT_EQ(d.changes.size(), changes);
    // The animation ending during a hitch returns normally afterwards.
    d.t += 1500 * Ms;
    d.Run(Nothing(), 1500 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_EQ(d.Count(C::Phase::Flat), 1u);
    EXPECT_EQ(d.Count(C::Phase::Returning), 1u);
}

TEST(CinematicMachine, NewPlayerForcesOff) {
    Driver d;
    d.Run(Event(101100), 1000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    C::Signals s = Event(101100);
    s.new_player = true;
    d.Tick(s);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_EQ(std::string(d.last.reason), std::string("new main player"));
}

// ---- Control back (cinematic_release) and the quick way back ----------------------------------

namespace {

// The game's cancel window: `open` = the clip takes R1 / R2 or a move cancels it.
C::Signals Window(C::Signals s, bool open) {
    s.cancel_valid = true;
    s.control = open;
    return s;
}

// Flat periods that ended at the game's cancel window.
size_t CountControlBack(const Driver& d) {
    size_t n = 0;
    for (const auto& c : d.changes) {
        n += std::string(c.step.reason) == "control back" ? 1 : 0;
    }
    return n;
}

} // namespace

// A door without a cancel window: back 50 ms after its end, straight from the screen (no FadeBack),
// the heading re-synced once; Off once VR has been up for the 80 ms fade (or after 0.58 s without
// the presenter's word).
TEST(CinematicMachine, DoorTimeline) {
    Driver d;
    d.Run(Nothing(), 200 * Ms);
    const s64 start = d.t;
    const C::Step first = d.Tick(Event(101100));
    EXPECT_EQ(first.to, C::Phase::FadeOut);
    d.Run(Event(101100), 2800 * Ms - Frame);
    const s64 end = d.t;
    d.Run(Nothing(), 2000 * Ms);
    EXPECT_NEAR(d.When(C::Phase::Flat, start), 150.0, 17.0);
    EXPECT_NEAR(d.When(C::Phase::Returning, end), 50.0, 17.0);
    EXPECT_EQ(d.Count(C::Phase::FadeBack), 0u);
    EXPECT_NEAR(d.When(C::Phase::Off, end), 50.0 + 80.0 + 500.0, 17.0);
    EXPECT_EQ(std::string(Reason(d.First(C::Phase::Returning))), std::string("signals off"));
    u32 resyncs = 0;
    for (const auto& c : d.changes) {
        resyncs += c.step.resync ? 1 : 0;
    }
    EXPECT_EQ(resyncs, 1u);
    ASSERT_EQ(d.returns.size(), 1u);
    EXPECT_EQ(d.returns[0].control, 0);
    EXPECT_EQ(d.returns[0].source, C::Category::Objects);
    EXPECT_EQ(d.returns[0].return_fade, 80 * Ms);

    // With the presenter's word: Off once VR has shown (two frames later) for the fade.
    Driver e;
    e.Run(Event(101100), 2800 * Ms);
    const s64 end2 = e.t;
    s64 shown = -1;
    while (e.t < end2 + 2000 * Ms) {
        e.Tick(Nothing());
        if (e.Phase() == C::Phase::Returning && shown < 0) {
            shown = e.t + 2 * Frame;
        }
        if (shown >= 0 && e.t >= shown && e.projection < 0) {
            e.projection = shown;
        }
    }
    EXPECT_NEAR(e.When(C::Phase::Off, end2), 50.0 + 33.0 + 80.0, 20.0);
    ASSERT_EQ(e.returns.size(), 1u);
    EXPECT_EQ(e.returns[0].projection, shown);
}

// The arena beast's grab (a000_009490, measured): the cancel window opens 5.95 s into the clip
// while the throw state stays on 0.72 s more. VR comes back once the window has stayed open 50 ms
// (two 30 fps updates), the tail never goes flat again, and the latch clears with the throw state.
TEST(CinematicMachine, GrabReturnsAtControlBack) {
    Driver d;
    d.frame = Frame30;
    d.Run(Window(Throw(2), false), 5950 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    const s64 open = d.t;
    bool control_back = false;
    bool cleared = false;
    s64 latch_age = 0;
    while (d.t < open + 720 * Ms) {
        const C::Step st = d.Tick(Window(Throw(2), true));
        control_back = control_back || st.control_back;
    }
    EXPECT_TRUE(control_back);
    EXPECT_NE(d.Phase(), C::Phase::Flat);
    const double back = d.When(C::Phase::Returning, open);
    EXPECT_GE(back, 50.0);
    EXPECT_LE(back, 50.0 + 34.0); // the exit hold, at most one update late
    EXPECT_EQ(CountControlBack(d), 1u);
    EXPECT_EQ(d.Count(C::Phase::FadeBack), 0u);
    EXPECT_EQ(d.Count(C::Phase::Flat), 1u);
    while (d.t < open + 2000 * Ms) {
        const C::Step st = d.Tick(Window(Nothing(), true)); // the throw state clears
        if (st.latch_cleared) {
            cleared = true;
            latch_age = st.latch_age_ns;
        }
    }
    EXPECT_TRUE(cleared);
    EXPECT_NEAR(MsOf(latch_age), 720.0, 34.0);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u); // never flat again from the tail
    ASSERT_EQ(d.returns.size(), 1u);
    const C::ReturnTimes& r = d.returns[0];
    EXPECT_EQ(r.control, open);
    EXPECT_EQ(r.request_end, open);
    EXPECT_EQ(r.exit, r.returning);
    EXPECT_EQ(std::string(r.why), std::string("control back"));
    EXPECT_EQ(r.source, C::Category::Grabbed);
    EXPECT_EQ(r.signals_off, 0); // still on when VR was back

    // A new grab after it goes flat again.
    d.Run(Window(Throw(2), false), 500 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
}

// The old rule on the same grab, for the log's split: the request ends with the throw state, the
// exit hold is 200 ms, the screen dims 150 ms.
TEST(CinematicMachine, GrabOldRuleTimeline) {
    Driver d;
    d.Legacy();
    d.frame = Frame30;
    d.Run(Window(Throw(2), false), 5950 * Ms);
    const s64 open = d.t;
    d.Run(Window(Throw(2), true), 720 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat); // the old rule waits
    const s64 ts_end = d.t;
    d.Run(Window(Nothing(), true), 2000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_EQ(d.Count(C::Phase::FadeBack), 1u);
    EXPECT_EQ(CountControlBack(d), 0u);
    ASSERT_EQ(d.returns.size(), 1u);
    const C::ReturnTimes& r = d.returns[0];
    EXPECT_EQ(r.control, open); // the window is timed, not acted on
    EXPECT_EQ(r.request_end, ts_end);
    EXPECT_EQ(r.signals_off, ts_end);
    EXPECT_NEAR(MsOf(r.exit - r.request_end), 200.0, 34.0);
    EXPECT_NEAR(MsOf(r.returning - r.exit), 150.0, 34.0);
    EXPECT_EQ(std::string(r.why), std::string("signals off"));
}

// A window open for a single update (less than the exit hold) keeps the flat view; one that stays
// open returns.
TEST(CinematicMachine, WindowGlitchKeepsFlat) {
    Driver d;
    d.frame = Frame30;
    d.Run(Window(Throw(2), false), 2000 * Ms);
    d.Tick(Window(Throw(2), true));
    d.Run(Window(Throw(2), false), 1000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    EXPECT_EQ(CountControlBack(d), 0u);
    d.Run(Window(Throw(2), true), 200 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Returning);
    EXPECT_EQ(CountControlBack(d), 1u);
}

// A window carried over from the clip before (open from the first update, never seen closed) is
// not control back: the animation returns at its end, as before.
TEST(CinematicMachine, WindowCarriedOverIsIgnored) {
    Driver d;
    d.Run(Window(Event(101100), true), 2000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    const s64 end = d.t;
    d.Run(Window(Nothing(), true), 1500 * Ms);
    EXPECT_EQ(CountControlBack(d), 0u);
    EXPECT_NEAR(d.When(C::Phase::Returning, end), 50.0, 17.0);
}

// With the death hold (cinematic_death_hold=1) a fatal grab stays flat: no release with HP at 0,
// and a released grab whose hunter dies before the throw state clears goes flat again and holds.
TEST(CinematicMachine, DeathGrabStaysFlat) {
    Driver d;
    d.cfg.death_hold = true;
    d.frame = Frame30;
    d.Run(Window(Throw(2), false), 3000 * Ms);
    C::Signals dying = Window(Throw(2), true);
    dying.hp = 0;
    d.Run(dying, 1000 * Ms);
    C::Signals dead = Window(Nothing(), true);
    dead.hp = 0;
    d.Run(dead, 3000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    EXPECT_EQ(CountControlBack(d), 0u);
    EXPECT_EQ(d.Count(C::Phase::Returning), 0u);

    Driver e;
    e.cfg.death_hold = true;
    e.frame = Frame30;
    e.Run(Window(Throw(2), false), 3000 * Ms);
    e.Run(Window(Throw(2), true), 200 * Ms);
    EXPECT_EQ(e.Phase(), C::Phase::Returning);
    C::Signals late = Window(Throw(2), true);
    late.hp = 0; // killed during the tail of the released grab
    e.Run(late, 300 * Ms);
    EXPECT_NE(e.Phase(), C::Phase::Off);
    EXPECT_NE(e.Phase(), C::Phase::Returning);
    e.Run(dead, 3000 * Ms);
    EXPECT_EQ(e.Phase(), C::Phase::Flat);
    EXPECT_TRUE(e.machine.HeldByThrow());
}

// The default since 2026-10-07 (cinematic_death_hold=0; the user saw YOU DIED on the flat screen
// after a fatal grab): HP reaching 0 in a grab's flat period returns to VR at once, the way the
// cancel window does (the throw state stays set ~7 s through the beast's kill in run int_b, the
// window never opens), the rest of the grab is ignored, nothing goes flat while HP is 0, and the
// respawn starts afresh.
static size_t CountReason(const Driver& d, const char* reason) {
    size_t n = 0;
    for (const auto& c : d.changes) {
        n += std::string(c.step.reason) == reason ? 1 : 0;
    }
    return n;
}

TEST(CinematicMachine, DeathReturnsToVrForTheDeathScreen) {
    Driver d;
    d.frame = Frame30;
    EXPECT_FALSE(d.cfg.death_hold);
    d.Run(Window(Throw(2), false), 3000 * Ms);
    ASSERT_EQ(d.Phase(), C::Phase::Flat);
    C::Signals dying = Window(Throw(6), false);
    dying.hp = 0;
    const s64 death = d.t;
    d.Tick(dying);
    EXPECT_TRUE(d.last.death_back);
    EXPECT_EQ(d.Phase(), C::Phase::Returning); // at once: no exit hold, no minimum
    EXPECT_TRUE(d.last.resync);
    EXPECT_EQ(std::string(d.last.reason), std::string("death"));
    d.projection = d.t; // the presenter brings VR up
    d.Run(dying, 7000 * Ms); // the kill plays on with the throw state set
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_LT(d.When(C::Phase::Off, death), 200.0);
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u); // no second flat period from its tail
    EXPECT_EQ(CountReason(d, "death"), 1u);
    EXPECT_EQ(CountControlBack(d), 0u);
    ASSERT_EQ(d.returns.size(), 1u);
    EXPECT_EQ(d.returns[0].request_end, death);
    C::Signals dead = Window(Nothing(), false);
    dead.hp = 0;
    C::Signals dead_event = Event(101100);
    dead_event.hp = 0;
    d.Run(dead, 1000 * Ms);
    d.Run(dead_event, 1000 * Ms); // nothing goes flat while HP is 0
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u);
    // The respawn: a new main player, HP back; a grab goes flat again.
    C::Signals respawn = Nothing();
    respawn.new_player = true;
    d.Tick(respawn);
    d.Run(Nothing(), 500 * Ms);
    d.Run(Throw(2), 1000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 2u);

    // A death during the fade to black: aborted, back up from the level reached.
    Driver e;
    e.Tick(Throw(2));
    e.Tick(Throw(2));
    e.Tick(Throw(2));
    ASSERT_EQ(e.Phase(), C::Phase::FadeOut);
    C::Signals dying2 = Throw(2);
    dying2.hp = 0;
    e.Tick(dying2);
    EXPECT_EQ(e.Phase(), C::Phase::Returning);
    EXPECT_EQ(std::string(e.last.reason), std::string("death"));
    EXPECT_GT(e.last.b0, 0.0f);
    e.Run(dying2, 2000 * Ms);
    EXPECT_EQ(e.Phase(), C::Phase::Off);
    EXPECT_EQ(e.Count(C::Phase::Flat), 0u);

    // A grab that kills before its flat period starts never goes flat.
    Driver f;
    f.Run(dying2, 3000 * Ms);
    EXPECT_TRUE(f.changes.empty());

    // A released grab whose hunter dies in its tail stays in VR (the hold would go flat again).
    Driver g;
    g.frame = Frame30;
    g.Run(Window(Throw(2), false), 3000 * Ms);
    g.Run(Window(Throw(2), true), 200 * Ms);
    EXPECT_EQ(CountControlBack(g), 1u);
    C::Signals late = Window(Throw(2), true);
    late.hp = 0;
    g.Run(late, 3000 * Ms);
    EXPECT_EQ(g.Phase(), C::Phase::Off);
    EXPECT_EQ(g.Count(C::Phase::FadeOut), 1u);
    EXPECT_EQ(CountReason(g, "death"), 0u);
}

// An event animation released at its cancel window: the bit stays on (the clip plays on) without
// a second flat period; a new event after the bit drops goes flat again.
TEST(CinematicMachine, EventReleaseIsLatched) {
    Driver d;
    d.Run(Window(Event(101150), false), 2000 * Ms);
    d.Run(Window(Event(101150), true), 800 * Ms);
    EXPECT_EQ(CountControlBack(d), 1u);
    EXPECT_NE(d.Phase(), C::Phase::Flat);
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u);
    d.Run(Window(Event(101150), false), 300 * Ms); // the window closes, the bit still on
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u);
    d.Tick(Window(Nothing(), true));
    d.Run(Window(Event(101100), false), 500 * Ms); // a new event
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 2u);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
}

// A window that opens early still keeps the shortest flat period.
TEST(CinematicMachine, ReleaseKeepsMinimumFlat) {
    Driver d;
    d.Run(Window(Event(101140), false), 250 * Ms); // FadeOut, then 100 ms flat
    const Change* flat = d.First(C::Phase::Flat);
    ASSERT_NE(flat, nullptr);
    d.Run(Window(Event(101140), true), 2000 * Ms);
    EXPECT_NEAR(d.When(C::Phase::Returning, flat->t), 600.0, 17.0);
    EXPECT_EQ(d.Count(C::Phase::Flat), 1u);
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u);
}

// cinematic_release=0: the window is only timed; back at the signals' end.
TEST(CinematicMachine, ReleaseOff) {
    Driver d;
    d.cfg.release = false;
    d.Run(Window(Throw(2), false), 3000 * Ms);
    const s64 open = d.t;
    d.Run(Window(Throw(2), true), 700 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    const s64 end = d.t;
    d.Run(Window(Nothing(), true), 1000 * Ms);
    EXPECT_EQ(CountControlBack(d), 0u);
    EXPECT_NEAR(d.When(C::Phase::Returning, end), 50.0, 17.0);
    ASSERT_EQ(d.returns.size(), 1u);
    EXPECT_EQ(d.returns[0].control, open);
}

// Without the cancel signal (a failed check) nothing is released early.
TEST(CinematicMachine, NoCancelSignal) {
    Driver d;
    C::Signals s = Throw(2);
    s.cancel_valid = false;
    s.control = true;
    d.Run(s, 3000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    EXPECT_EQ(CountControlBack(d), 0u);
}

TEST(CinematicBrightness, Phases) {
    const s64 fade = 150 * Ms;
    EXPECT_NEAR(C::Brightness(C::Phase::Off, 0, fade, true, 0), 1.0f, 1e-6f);
    // FadeOut / FadeBack: down to black in the old view.
    EXPECT_NEAR(C::Brightness(C::Phase::FadeOut, 0, fade, true, 10'000 * Ms), 1.0f, 1e-6f);
    EXPECT_NEAR(C::Brightness(C::Phase::FadeOut, 75 * Ms, fade, true, 10'000 * Ms), 0.5f, 1e-6f);
    EXPECT_NEAR(C::Brightness(C::Phase::FadeOut, 200 * Ms, fade, true, 10'000 * Ms), 0.0f, 1e-6f);
    EXPECT_NEAR(C::Brightness(C::Phase::FadeBack, 75 * Ms, fade, false, 10'000 * Ms), 0.5f, 1e-6f);
    // Flat: black until the screen shows, then it fades in.
    EXPECT_NEAR(C::Brightness(C::Phase::Flat, 30 * Ms, fade, true, 10'000 * Ms), 0.0f, 1e-6f);
    EXPECT_NEAR(C::Brightness(C::Phase::Flat, 60 * Ms, fade, false, 0), 0.0f, 1e-6f);
    EXPECT_NEAR(C::Brightness(C::Phase::Flat, 135 * Ms, fade, false, 75 * Ms), 0.5f, 1e-6f);
    EXPECT_NEAR(C::Brightness(C::Phase::Flat, 2000 * Ms, fade, false, 1900 * Ms), 1.0f, 1e-6f);
    // ... and never held black for more than a second.
    EXPECT_NEAR(C::Brightness(C::Phase::Flat, 1000 * Ms, fade, true, 10'000 * Ms), 1.0f, 1e-6f);
    // Returning: black until VR shows, then it fades in.
    EXPECT_NEAR(C::Brightness(C::Phase::Returning, 20 * Ms, fade, false, 10'000 * Ms), 0.0f, 1e-6f);
    EXPECT_NEAR(C::Brightness(C::Phase::Returning, 50 * Ms, fade, true, 30 * Ms), 0.2f, 1e-6f);
    EXPECT_NEAR(C::Brightness(C::Phase::Returning, 400 * Ms, fade, true, 380 * Ms), 1.0f, 1e-6f);
    EXPECT_NEAR(C::Brightness(C::Phase::Returning, 1000 * Ms, fade, false, 10'000 * Ms), 1.0f,
                1e-6f);
}

// Entered part-way (an interrupted fade): the fade continues from the entry level.
TEST(CinematicBrightness, EntryLevel) {
    const s64 fade = 150 * Ms;
    // FadeBack interrupted at 0.5 by a new signal: Flat with the screen already up goes on up.
    EXPECT_NEAR(C::Brightness(C::Phase::Flat, 0, fade, false, 10'000 * Ms, 0.5f), 0.5f, 1e-6f);
    EXPECT_NEAR(C::Brightness(C::Phase::Flat, 30 * Ms, fade, false, 10'000 * Ms, 0.5f), 0.7f,
                1e-6f);
    EXPECT_NEAR(C::Brightness(C::Phase::Flat, 75 * Ms, fade, false, 10'000 * Ms, 0.5f), 1.0f,
                1e-6f);
    // Returning interrupted at 0.4 by a new signal: FadeOut goes down from there.
    EXPECT_NEAR(C::Brightness(C::Phase::FadeOut, 0, fade, true, 10'000 * Ms, 0.4f), 0.4f, 1e-6f);
    EXPECT_NEAR(C::Brightness(C::Phase::FadeOut, 75 * Ms, fade, true, 10'000 * Ms, 0.4f), 0.2f,
                1e-6f);
    // Returning while the screen is still up and VR is black stays black in FadeOut.
    EXPECT_NEAR(C::Brightness(C::Phase::FadeOut, 10 * Ms, fade, false, 10'000 * Ms, 0.0f), 0.0f,
                1e-6f);
    // Returning when VR cannot come back (no camera updates or no head pose): the screen fades
    // back in instead of staying black.
    EXPECT_NEAR(C::Brightness(C::Phase::Returning, 75 * Ms, fade, false, 10'000 * Ms, 0.0f, false),
                0.5f, 1e-6f);
    EXPECT_NEAR(C::Brightness(C::Phase::Returning, 75 * Ms, fade, false, 10'000 * Ms, 0.0f, true),
                0.0f, 1e-6f);
}

namespace {

// The presenter side: the layer follows the camera a few frames later, and the fader is sampled
// at every update. Returns the largest change of brightness between two updates.
struct Presenter {
    C::Fader fader{};
    bool immersive{true};
    s64 switched{0};
    u32 waiting{0};
    bool vr_available{true}; // VR can come back (else the screen stays up)
    float level{1.0f};
    float max_step{0.0f};
    bool popped{false}; // the level rose while it should only have gone down
    bool started{false};

    void Sample(Driver& d, u32 delay = 2) {
        if (!started) {
            started = true;
            d.projection = immersive ? 0 : -1;
        }
        const C::Phase phase = d.Phase();
        const bool want = vr_available && phase != C::Phase::Flat && phase != C::Phase::FadeBack;
        waiting = want != immersive ? waiting + 1 : 0;
        if (want != immersive && waiting >= delay) {
            immersive = want;
            switched = d.t;
            waiting = 0;
            d.projection = immersive ? switched : -1; // VR::NoteCinematicLayer
        }
        const float b = fader.Update(phase, d.machine.PhaseStart(), d.t, d.machine.PhaseFade(),
                                     immersive, d.t - switched, vr_available);
        max_step = std::max(max_step, std::abs(b - level));
        if ((phase == C::Phase::FadeOut || phase == C::Phase::FadeBack) && b > level + 1e-6f) {
            popped = true;
        }
        level = b;
    }
};

// One update and one presenter sample.
void Step(Driver& d, Presenter& p, const C::Signals& s) {
    d.Tick(s);
    p.Sample(d);
}

void Steps(Driver& d, Presenter& p, const C::Signals& s, s64 duration) {
    const s64 end = d.t + duration;
    while (d.t < end) {
        Step(d, p, s);
    }
}

constexpr float MaxRamp = static_cast<float>(Frame) / (150.0f * Ms) + 1e-4f; // one update's step

} // namespace

// A new signal during FadeBack: the screen goes back up from where it was (no jump to full).
TEST(CinematicBrightness, SignalDuringFadeBackIsContinuous) {
    Driver d;
    d.Legacy();
    Presenter p;
    Steps(d, p, Event(101100), 1000 * Ms);
    Steps(d, p, Nothing(), 200 * Ms);
    while (d.Phase() != C::Phase::FadeBack) {
        Step(d, p, Nothing());
    }
    Steps(d, p, Nothing(), 70 * Ms); // about half way down
    EXPECT_GT(p.level, 0.3f);
    EXPECT_LT(p.level, 0.7f);
    Step(d, p, Event(103000));
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    Steps(d, p, Event(103000), 1000 * Ms);
    Steps(d, p, Nothing(), 2000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_LE(p.max_step, MaxRamp);
    EXPECT_FALSE(p.popped);
    EXPECT_NEAR(p.level, 1.0f, 1e-6f);
}

// A new signal while first-person VR fades back in: it fades down from there (no pop to full).
TEST(CinematicBrightness, SignalDuringReturningIsContinuous) {
    Driver d;
    d.Legacy();
    Presenter p;
    Steps(d, p, Event(101100), 1000 * Ms);
    while (d.Phase() != C::Phase::Returning) {
        Step(d, p, Nothing());
    }
    Steps(d, p, Nothing(), 80 * Ms); // VR shows after two frames and fades in part-way
    EXPECT_GT(p.level, 0.1f);
    EXPECT_LT(p.level, 0.9f);
    Step(d, p, Event(101100));
    EXPECT_EQ(d.Phase(), C::Phase::FadeOut);
    Steps(d, p, Event(101100), 1000 * Ms);
    Steps(d, p, Nothing(), 2000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_LE(p.max_step, MaxRamp);
    EXPECT_FALSE(p.popped);
    EXPECT_NEAR(p.level, 1.0f, 1e-6f);
}

// A new signal in Returning before VR has shown (the screen is still up, black): the screen never
// pops back to full brightness, it comes up again with the next flat period.
TEST(CinematicBrightness, SignalBeforeVrShowsStaysBlack) {
    Driver d;
    d.Legacy();
    Presenter p;
    Steps(d, p, Event(101100), 1000 * Ms);
    while (d.Phase() != C::Phase::Returning) {
        Step(d, p, Nothing());
    }
    d.Tick(Event(101100)); // the very next update, before the projection is back
    p.Sample(d, 10);
    EXPECT_EQ(d.Phase(), C::Phase::FadeOut);
    EXPECT_NEAR(p.level, 0.0f, 1e-6f);
    for (int i = 0; i < 8; ++i) {
        d.Tick(Event(101100));
        p.Sample(d, 10);
        EXPECT_LE(p.level, 1e-6f);
    }
    Steps(d, p, Event(101100), 1000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    EXPECT_NEAR(p.level, 1.0f, 1e-6f);
    EXPECT_LE(p.max_step, MaxRamp);
}

// VR cannot come back after the flat period (the head pose was lost): the screen fades back in
// at once instead of holding black through Returning.
TEST(CinematicBrightness, ReturningWithoutVrBringsTheScreenBack) {
    Driver d;
    d.Legacy();
    Presenter p;
    Steps(d, p, Event(101100), 1000 * Ms);
    p.vr_available = false;
    Steps(d, p, Nothing(), 2000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_FALSE(p.immersive);
    EXPECT_LE(p.max_step, MaxRamp);
    EXPECT_NEAR(p.level, 1.0f, 1e-6f);
}

// VR could not come back (the screen was brought back up in Returning) and a new animation starts:
// the screen is already the shown layer, so it goes flat at once and never dims.
TEST(CinematicBrightness, SignalInReturningWithoutVrKeepsTheScreenUp) {
    Driver d;
    d.Legacy();
    Presenter p;
    Steps(d, p, Event(101100), 1000 * Ms);
    p.vr_available = false;
    d.can_enter = false; // first-person VR is not shown
    while (d.Phase() != C::Phase::Returning) {
        Step(d, p, Nothing());
    }
    Steps(d, p, Nothing(), 300 * Ms);
    EXPECT_NEAR(p.level, 1.0f, 1e-6f);
    Step(d, p, Event(103000));
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    EXPECT_EQ(std::string(d.last.reason), std::string("signal again"));
    float lowest = p.level;
    for (int i = 0; i < 30; ++i) {
        Step(d, p, Event(103000));
        lowest = std::min(lowest, p.level);
    }
    EXPECT_NEAR(lowest, 1.0f, 1e-6f);
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u); // only the first animation's
    Steps(d, p, Nothing(), 2000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_LE(p.max_step, MaxRamp);
    EXPECT_NEAR(p.level, 1.0f, 1e-6f);

    // With VR shown again (can_enter) the same signal fades out as before.
    Driver e;
    e.Legacy();
    e.Run(Event(101100), 1000 * Ms);
    while (e.Phase() != C::Phase::Returning) {
        e.Tick(Nothing());
    }
    e.Tick(Event(101100));
    EXPECT_EQ(e.Phase(), C::Phase::FadeOut);
}

TEST(CinematicBrightness, HardCut) {
    EXPECT_NEAR(C::Brightness(C::Phase::FadeOut, 0, 0, true, 0), 0.0f, 1e-6f);
    EXPECT_NEAR(C::Brightness(C::Phase::Flat, 10 * Ms, 0, true, 10'000 * Ms), 0.0f, 1e-6f);
    EXPECT_NEAR(C::Brightness(C::Phase::Flat, 20 * Ms, 0, false, 0), 1.0f, 1e-6f);
    EXPECT_NEAR(C::Brightness(C::Phase::Returning, 20 * Ms, 0, true, 0), 1.0f, 1e-6f);
}

// The fade as the presenter sees it through a whole door: brightness never rises before the
// layer switches, and is 1 again at the end.
TEST(CinematicBrightness, DoorSequence) {
    Driver d;
    const s64 fade = d.cfg.fade_ns;
    bool immersive = true;
    s64 switched = 0;
    float previous = 1.0f;
    bool dark = false;
    for (int i = 0; i < 400; ++i) {
        const C::Signals s = i < 170 ? Event(101100) : Nothing();
        d.Tick(s);
        // The camera stops being driven in Flat and FadeBack; the layer follows two frames later.
        const bool want_immersive = d.Phase() != C::Phase::Flat && d.Phase() != C::Phase::FadeBack;
        if (want_immersive != immersive && i % 2 == 0) {
            immersive = want_immersive;
            switched = d.t;
        }
        const float b =
            C::Brightness(d.Phase(), d.t - d.machine.PhaseStart(), fade, immersive, d.t - switched);
        EXPECT_GE(b, 0.0f);
        EXPECT_LE(b, 1.0f);
        if (b == 0.0f) {
            dark = true;
        }
        if (d.Phase() == C::Phase::FadeOut || d.Phase() == C::Phase::FadeBack) {
            EXPECT_LE(b, previous + 1e-6f); // only goes down while fading out
        }
        previous = b;
    }
    EXPECT_TRUE(dark);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_NEAR(previous, 1.0f, 1e-6f);
}

// The quick way back as the presenter shows it: the screen cuts to black, first-person VR comes up
// from black two frames later and fades in over 80 ms (only up, never a flash of the screen), and
// the gameplay controls are back (Off) right after the fade.
TEST(CinematicBrightness, QuickReturn) {
    Driver d;
    Presenter p;
    Steps(d, p, Event(101100), 1000 * Ms);
    EXPECT_NEAR(p.level, 1.0f, 1e-6f); // the screen is up
    while (d.Phase() != C::Phase::Returning) {
        Step(d, p, Nothing());
    }
    EXPECT_NEAR(p.level, 0.0f, 1e-6f); // cut to black at once
    const s64 back = d.t;
    float previous = p.level;
    float max_up = 0.0f;
    while (d.Phase() != C::Phase::Off && d.t < back + 2000 * Ms) {
        Step(d, p, Nothing());
        EXPECT_GE(p.level, previous - 1e-6f);
        max_up = std::max(max_up, p.level - previous);
        previous = p.level;
    }
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_NEAR(p.level, 1.0f, 1e-6f);
    EXPECT_LE(max_up, static_cast<float>(Frame) / (80.0f * Ms) + 1e-4f);
    EXPECT_NEAR(MsOf(d.t - back), 33.0 + 80.0, 25.0);
    ASSERT_EQ(d.returns.size(), 1u);
    EXPECT_GT(d.returns[0].projection, d.returns[0].returning);
}

// ---- Ladders ----------------------------------------------------------------------------------

TEST(LadderClassify, Clips) {
    EXPECT_EQ(C::ClassifyLadderClip(101010), C::LadderClip::Mount);
    EXPECT_EQ(C::ClassifyLadderClip(101020), C::LadderClip::Mount);
    EXPECT_EQ(C::ClassifyLadderClip(101011), C::LadderClip::Up);
    EXPECT_EQ(C::ClassifyLadderClip(101016), C::LadderClip::FastUp);
    EXPECT_EQ(C::ClassifyLadderClip(101022), C::LadderClip::Down);
    EXPECT_EQ(C::ClassifyLadderClip(101040), C::LadderClip::Idle);
    EXPECT_EQ(C::ClassifyLadderClip(101000), C::LadderClip::Slide);
    EXPECT_EQ(C::ClassifyLadderClip(101004), C::LadderClip::Slide);
    EXPECT_EQ(C::ClassifyLadderClip(101048), C::LadderClip::Hit);
    EXPECT_EQ(C::ClassifyLadderClip(101036), C::LadderClip::Attack);
    EXPECT_EQ(C::ClassifyLadderClip(101075), C::LadderClip::Attack);
    EXPECT_EQ(C::ClassifyLadderClip(101061), C::LadderClip::Damage);
    EXPECT_EQ(C::ClassifyLadderClip(101014), C::LadderClip::ExitTop);
    EXPECT_EQ(C::ClassifyLadderClip(101024), C::LadderClip::ExitBottom);
    EXPECT_EQ(C::ClassifyLadderClip(101002), C::LadderClip::SlideEnd);
    EXPECT_EQ(C::ClassifyLadderClip(101051), C::LadderClip::Fall);
    EXPECT_EQ(C::ClassifyLadderClip(24710), C::LadderClip::Die);
    EXPECT_EQ(C::ClassifyLadderClip(101070), C::LadderClip::Unexpected);
    EXPECT_EQ(C::ClassifyLadderClip(101100), C::LadderClip::None); // a door
    EXPECT_EQ(C::ClassifyLadderClip(3010), C::LadderClip::None);
    EXPECT_EQ(C::ClassifyLadderClip(-1), C::LadderClip::None);
    EXPECT_TRUE(C::IsLadderEnd(C::LadderClip::ExitTop));
    EXPECT_TRUE(C::IsLadderEnd(C::LadderClip::SlideEnd));
    EXPECT_TRUE(C::IsLadderEnd(C::LadderClip::Die));
    EXPECT_FALSE(C::IsLadderEnd(C::LadderClip::Attack));
    // The newest ladder clip of a behaviour step, one that ends the climb first.
    const s32 step[] = {101011, 3010, 101030};
    EXPECT_EQ(C::PickLadderClip(step, 3), 101030);
    const s32 end[] = {101013, 101030, 3010};
    EXPECT_EQ(C::PickLadderClip(end, 3), 101013);
    const s32 none[] = {3010, 101100};
    EXPECT_EQ(C::PickLadderClip(none, 2), -1);
    EXPECT_EQ(C::PickLadderClip(nullptr, 0), -1);
    EXPECT_TRUE(C::IsKnownLadderState(-1));
    EXPECT_TRUE(C::IsKnownLadderState(19));
    EXPECT_FALSE(C::IsKnownLadderState(7));
    EXPECT_FALSE(C::IsKnownLadderState(15));
}

// The gate on its own: a step's clip and its state byte one update apart are one step; the
// mount sets the direction.
TEST(LadderGate, StepsAndDirection) {
    C::LadderGate g;
    const C::LadderConfig cfg{};
    C::LadderSignals in{};
    in.valid = true;
    s64 t = 1000 * Ms;
    const auto tick = [&](s8 state, s32 clip) {
        in.state = state;
        in.clip = clip;
        const C::LadderOut out = g.Update(t, in, true, 500, 500, cfg, true, true);
        t += Frame30;
        return out;
    };
    tick(-1, -1);
    C::LadderOut out = tick(0, 101010);
    EXPECT_TRUE(out.session);
    EXPECT_TRUE(out.mounting);
    EXPECT_EQ(out.dir, 1);
    for (int i = 0; i < 26; ++i) {
        tick(0, -1);
    }
    out = tick(8, 101030); // the mount ends into idle: armed, no step yet
    EXPECT_FALSE(out.mounting);
    EXPECT_TRUE(out.armed);
    EXPECT_EQ(out.steps, 0u);
    out = tick(8, 101011); // a step's clip ...
    EXPECT_EQ(out.steps, 1u);
    EXPECT_EQ(out.climb_ns, 0);
    out = tick(2, -1); // ... and its state an update later: still one step
    EXPECT_EQ(out.steps, 1u);
    for (int i = 0; i < 10; ++i) {
        tick(2, -1);
    }
    out = tick(3, 101012);
    EXPECT_EQ(out.steps, 2u);
    EXPECT_EQ(out.dir, 1);
    out = tick(4, 101021); // turned round
    EXPECT_EQ(out.steps, 1u);
    EXPECT_EQ(out.dir, -1);
    out = tick(-1, 101023);
    EXPECT_FALSE(out.session);
    EXPECT_EQ(out.clip, 101023);

    // From above: down.
    C::LadderGate h;
    in.state = 1;
    in.clip = 101020;
    out = h.Update(t, in, true, 500, 500, cfg, true, true);
    EXPECT_EQ(out.dir, -1);
    EXPECT_TRUE(out.mounting);
}

namespace {

// A long ladder climbed straight up from the bottom: getting on stays in VR, the climb goes flat
// 0.5 s after the mount, VR comes back one step before the top (before the dismount clip), and
// the dismount does not go flat again. The fade never jumps.
void LongClimbUp(s64 frame) {
    Driver d;
    d.frame = frame;
    Presenter p;
    Climber c{d};
    c.rungs = 12;
    c.on_tick = [&] { p.Sample(d); };
    c.Off(300 * Ms);
    c.MountBelow();
    EXPECT_TRUE(d.changes.empty()); // never flat while getting on
    const s64 mount_end = d.t;
    c.Up(12);
    const s64 end_push = d.t;
    c.ExitTop();
    c.Off(1000 * Ms);

    const double tolerance = MsOf(frame) + 0.01;
    const Change* out = d.First(C::Phase::FadeOut);
    ASSERT_TRUE(out != nullptr);
    EXPECT_NEAR(MsOf(out->t - mount_end), 500.0, tolerance);
    EXPECT_EQ(out->step.category, C::Category::Ladder);
    EXPECT_EQ(out->step.fade_ns, 100 * Ms); // the ladder fade
    EXPECT_NEAR(d.When(C::Phase::Flat, mount_end), 600.0, tolerance);
    const Change* enter = d.FirstLadder(C::LadderEvent::Enter);
    ASSERT_TRUE(enter != nullptr);
    EXPECT_EQ(enter->step.ladder.dir, 1);
    EXPECT_EQ(enter->step.ladder.steps_left, 10);
    EXPECT_EQ(enter->step.ladder.pred_ns, 4000 * Ms);

    // Back one step before the top (rung 11 of 12), at once (no exit_ms / min_ms).
    const Change* back = d.First(C::Phase::FadeBack);
    ASSERT_TRUE(back != nullptr);
    EXPECT_EQ(Reason(back), std::string("lead"));
    EXPECT_FALSE(back->step.ladder.cut);
    EXPECT_EQ(back->step.ladder.steps_left, 1);
    EXPECT_NEAR(MsOf(back->t - mount_end), 4000.0, tolerance);
    const Change* returning = d.First(C::Phase::Returning);
    ASSERT_TRUE(returning != nullptr);
    EXPECT_TRUE(returning->step.resync);
    EXPECT_LT(returning->t, end_push); // VR is back before the dismount
    const Change* off = d.First(C::Phase::Off);
    ASSERT_TRUE(off != nullptr);
    EXPECT_LE(off->t, end_push);

    EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u);
    EXPECT_EQ(d.Count(C::Phase::Flat), 1u);
    EXPECT_EQ(d.LadderCount(C::LadderEvent::Enter), 1u);
    EXPECT_EQ(d.LadderCount(C::LadderEvent::Exit), 1u);
    EXPECT_EQ(d.LadderCount(C::LadderEvent::Skip), 0u);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_FALSE(d.machine.Ladder().InSession());
    // The presenter: no pop, steps no larger than the 100 ms fade allows, full brightness again.
    EXPECT_FALSE(p.popped);
    EXPECT_LE(p.max_step, static_cast<float>(frame) / (100.0f * Ms) + 1e-4f);
    EXPECT_NEAR(p.level, 1.0f, 1e-6f);
}

} // namespace

TEST(LadderMachine, LongClimbUp30Hz) {
    LongClimbUp(Frame30);
}

TEST(LadderMachine, LongClimbUp60Hz) {
    LongClimbUp(Frame);
}

// From the top: the 2 s mount stays in VR, the climb down goes flat, VR is back before the bottom
// dismount (whose drop happens in its first 0.33 s).
TEST(LadderMachine, LongClimbDown) {
    Driver d;
    d.frame = Frame30;
    Climber c{d};
    c.rungs = 12;
    c.Off(300 * Ms);
    c.MountAbove();
    EXPECT_TRUE(d.changes.empty());
    const s64 mount_end = d.t;
    c.Down(12);
    const s64 end_push = d.t;
    c.ExitBottom();
    c.Off(1000 * Ms);
    const double tolerance = MsOf(Frame30) + 0.01;
    EXPECT_NEAR(d.When(C::Phase::FadeOut, mount_end), 500.0, tolerance);
    const Change* enter = d.FirstLadder(C::LadderEvent::Enter);
    ASSERT_TRUE(enter != nullptr);
    EXPECT_EQ(enter->step.ladder.dir, -1);
    const Change* back = d.First(C::Phase::FadeBack);
    ASSERT_TRUE(back != nullptr);
    EXPECT_EQ(Reason(back), std::string("lead"));
    EXPECT_NEAR(MsOf(back->t - mount_end), 4000.0, tolerance); // at rung 1
    const Change* returning = d.First(C::Phase::Returning);
    ASSERT_TRUE(returning != nullptr);
    EXPECT_LT(returning->t, end_push);
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
}

// A short ladder (4 rungs) never goes flat: the predicted flat time is too short, logged once.
// The threshold with the defaults is 7 rungs.
TEST(LadderMachine, ShortLadderStaysInVr) {
    for (const s32 top : {4, 6}) {
        Driver d;
        d.frame = Frame30;
        Climber c{d};
        c.rungs = top;
        c.Off(300 * Ms);
        c.MountBelow();
        c.Up(top);
        c.ExitTop();
        c.Off(500 * Ms);
        EXPECT_TRUE(d.changes.empty());
        EXPECT_EQ(d.LadderCount(C::LadderEvent::Skip), 1u);
        EXPECT_EQ(d.LadderCount(C::LadderEvent::Enter), 0u);
    }
    Driver d;
    d.frame = Frame30;
    Climber c{d};
    c.rungs = 7;
    c.Off(300 * Ms);
    c.MountBelow();
    c.Up(7);
    c.ExitTop();
    c.Off(1000 * Ms);
    EXPECT_EQ(d.Count(C::Phase::Flat), 1u);
    EXPECT_EQ(d.LadderCount(C::LadderEvent::Skip), 0u);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
}

// Getting on and straight off again (both mounts can cancel into the dismount): never flat.
TEST(LadderMachine, MountThenGetOff) {
    Driver d;
    d.frame = Frame30;
    Climber c{d};
    c.rungs = 20;
    c.Off(300 * Ms);
    c.MountBelow();
    c.ExitBottom();
    c.Off(300 * Ms);
    c.rungs = 20;
    c.MountAbove();
    c.ExitTop();
    c.Off(300 * Ms);
    EXPECT_TRUE(d.changes.empty());
    EXPECT_TRUE(d.ladder.empty());
}

// Getting on and waiting: nothing until the hunter climbs, then enter_delay of climbing.
TEST(LadderMachine, MountThenWait) {
    Driver d;
    d.frame = Frame30;
    Climber c{d};
    c.rungs = 20;
    c.Off(300 * Ms);
    c.MountBelow();
    c.Idle(3000 * Ms);
    EXPECT_TRUE(d.changes.empty());
    const s64 climb = d.t;
    c.Up(10);
    EXPECT_NEAR(d.When(C::Phase::FadeOut, climb), 500.0, MsOf(Frame30) + 0.01);
}

// An attack on the ladder while flat: back to VR at once. Idle afterwards does not go flat again;
// the next step does, after enter_delay of climbing and at least reenter after the return.
TEST(LadderMachine, AttackOnLadder) {
    Driver d;
    d.frame = Frame30;
    Climber c{d};
    c.rungs = 30;
    c.Off(300 * Ms);
    c.MountBelow();
    c.Up(3);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    const s64 attack = d.t;
    c.Hold(10, 101035, 1170 * Ms);
    const Change* back = d.First(C::Phase::FadeBack, attack);
    ASSERT_TRUE(back != nullptr);
    EXPECT_EQ(back->t, attack);
    EXPECT_EQ(Reason(back), std::string("attack"));
    EXPECT_TRUE(back->step.ladder.cut);
    c.Idle(2000 * Ms);
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    const s64 climb = d.t;
    c.Up(5);
    EXPECT_NEAR(d.When(C::Phase::FadeOut, climb), 500.0, MsOf(Frame30) + 0.01);

    // Climbing on 0.3 s after the cut: the 1 s re-entry wait decides.
    Driver e;
    e.frame = Frame30;
    Climber f{e};
    f.rungs = 30;
    f.Off(300 * Ms);
    f.MountBelow();
    f.Up(3);
    const s64 cut = e.t;
    f.Hold(12, 101045, 300 * Ms);
    f.Up(10);
    const Change* again = e.First(C::Phase::FadeOut, cut);
    ASSERT_TRUE(again != nullptr);
    EXPECT_NEAR(MsOf(again->t - cut), 1000.0, MsOf(Frame30) + 0.01);

    // A fight before the flat period starts: the gate waits for the next step.
    Driver g;
    g.frame = Frame30;
    Climber h{g};
    h.rungs = 30;
    h.Off(300 * Ms);
    h.MountBelow();
    h.Up(1);
    h.Hold(10, 101035, 1170 * Ms);
    EXPECT_TRUE(g.changes.empty());
    const s64 resume = g.t;
    h.Up(5);
    EXPECT_NEAR(g.When(C::Phase::FadeOut, resume), 500.0, MsOf(Frame30) + 0.01);
}

// Damage (an HP drop, or the damage clip) and a blocked climb return at once with combat_exit;
// without it the climb stays flat.
TEST(LadderMachine, DamageOnLadder) {
    Driver d;
    d.frame = Frame30;
    Climber c{d};
    c.rungs = 30;
    c.Off(300 * Ms);
    c.MountBelow();
    c.Up(3);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    const s64 hit = d.t;
    c.hp = 400;
    c.Up(1);
    const Change* back = d.First(C::Phase::FadeBack, hit);
    ASSERT_TRUE(back != nullptr);
    EXPECT_EQ(back->t, hit);
    EXPECT_EQ(Reason(back), std::string("damage"));
    EXPECT_EQ(back->step.ladder.hp_before, 500);
    // Climbing on: back to flat 1 s after the return (the next step re-arms at +0.4 s).
    c.Up(6);
    const Change* again = d.First(C::Phase::FadeOut, hit);
    ASSERT_TRUE(again != nullptr);
    EXPECT_NEAR(MsOf(again->t - hit), 1000.0, MsOf(Frame30) + 0.01);

    // The damage clip without an HP drop, and a character in the way.
    for (const s32 clip : {101060, 101037}) {
        Driver e;
        e.frame = Frame30;
        Climber f{e};
        f.rungs = 30;
        f.Off(300 * Ms);
        f.MountBelow();
        f.Up(3);
        const s64 at = e.t;
        f.Hold(8, clip, 500 * Ms);
        const Change* cut = e.First(C::Phase::FadeBack, at);
        ASSERT_TRUE(cut != nullptr);
        EXPECT_EQ(cut->t, at);
        EXPECT_EQ(Reason(cut), std::string(clip == 101060 ? "damage" : "hit"));
    }

    // combat_exit=0: damage and attacks stay flat.
    Driver g;
    g.frame = Frame30;
    g.cfg.ladder.combat_exit = false;
    Climber h{g};
    h.rungs = 30;
    h.Off(300 * Ms);
    h.MountBelow();
    h.Up(3);
    h.hp = 300;
    h.Up(1);
    h.Hold(10, 101035, 1170 * Ms);
    h.Up(2);
    EXPECT_EQ(g.Phase(), C::Phase::Flat);
    EXPECT_EQ(g.Count(C::Phase::FadeBack), 0u);
}

// Falling off (stamina gone): back to VR at once, the climb is over.
TEST(LadderMachine, FallOff) {
    Driver d;
    d.frame = Frame30;
    Climber c{d};
    c.rungs = 30;
    c.Off(300 * Ms);
    c.MountBelow();
    c.Up(6);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    const s64 fall = d.t;
    c.object = c.rung = c.top = 0;
    c.Hold(-1, 101050, 300 * Ms);
    c.Hold(-1, 101051, 700 * Ms);
    c.Hold(-1, 101052, 800 * Ms);
    c.Off(1000 * Ms);
    const Change* back = d.First(C::Phase::FadeBack, fall);
    ASSERT_TRUE(back != nullptr);
    EXPECT_EQ(back->t, fall);
    EXPECT_EQ(Reason(back), std::string("fall"));
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_FALSE(d.machine.Ladder().InSession());
}

// Death on a ladder: back to VR at once. The death states never write the ladder state, so after
// the respawn it still reads a climbing value: no flat period until the hunter is on a ladder
// again (dead latch), also across the loading gap.
TEST(LadderMachine, DeathOnLadder) {
    for (const bool loading : {false, true}) {
        Driver d;
        d.frame = Frame30;
        Climber c{d};
        c.rungs = 30;
        c.Off(300 * Ms);
        c.MountBelow();
        c.Up(3);
        EXPECT_EQ(d.Phase(), C::Phase::Flat);
        const s64 died = d.t;
        // The death states keep the state of the last step.
        const s8 stale = (c.rung & 1) != 0 ? 2 : 3;
        c.hp = 0;
        c.Hold(stale, 24700, 4000 * Ms);
        const Change* back = d.First(C::Phase::FadeBack, died);
        ASSERT_TRUE(back != nullptr);
        EXPECT_EQ(back->t, died);
        EXPECT_EQ(Reason(back), std::string("death"));
        EXPECT_TRUE(d.machine.Ladder().Dead());
        if (loading) {
            d.t += 8000 * Ms;
        }
        // Respawned: HP back, the stale state and ladder object, no new clip.
        c.hp = 500;
        c.Hold(stale, -1, 6000 * Ms);
        EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u);
        EXPECT_TRUE(d.machine.Ladder().Dead());
        // The next ladder.
        c.rungs = 20;
        c.MountBelow();
        EXPECT_FALSE(d.machine.Ladder().Dead());
        c.Up(10);
        EXPECT_EQ(d.Count(C::Phase::FadeOut), 2u);
    }
}

// Sliding down: the prediction counts 133 ms a rung, so VR is back before the landing.
TEST(LadderMachine, Slide) {
    for (const s64 lead : {450 * Ms, s64{0}}) {
        Driver d;
        d.frame = Frame30;
        d.cfg.ladder.exit_lead_ns = lead;
        Climber c{d};
        c.rungs = 20;
        c.Off(300 * Ms);
        c.MountAbove();
        c.Down(2);
        EXPECT_EQ(d.Phase(), C::Phase::Flat);
        // Slide_Start, then the loop (the state stays 14), a rung every 133 ms.
        --c.rung;
        c.Hold(14, 101000, 167 * Ms);
        bool first = true;
        while (c.rung > 0) {
            --c.rung;
            c.Hold(14, first ? 101001 : -1, 133 * Ms);
            first = false;
        }
        const s64 landing = d.t;
        c.object = c.rung = c.top = 0;
        c.Hold(-1, 101002, 833 * Ms);
        c.Off(1000 * Ms);
        const Change* back = d.First(C::Phase::FadeBack);
        ASSERT_TRUE(back != nullptr);
        EXPECT_EQ(d.Count(C::Phase::FadeBack), 1u);
        if (lead > 0) {
            EXPECT_EQ(Reason(back), std::string("lead"));
            EXPECT_EQ(back->step.ladder.step_ns, C::LadderSlideStepNs);
            EXPECT_EQ(back->step.ladder.steps_left, 3);
            const Change* returning = d.First(C::Phase::Returning);
            ASSERT_TRUE(returning != nullptr);
            EXPECT_LT(returning->t, landing);
        } else {
            EXPECT_EQ(back->t, landing);
            EXPECT_EQ(Reason(back), std::string("slide_end"));
        }
        EXPECT_EQ(d.Phase(), C::Phase::Off);
    }
}

// Climbing the same ladder again right after getting off: a new climb, flat again.
TEST(LadderMachine, ReMount) {
    Driver d;
    d.frame = Frame30;
    Climber c{d};
    c.rungs = 12;
    c.Off(300 * Ms);
    c.MountBelow();
    c.Up(12);
    c.ExitTop();
    c.Off(300 * Ms);
    c.rungs = 12;
    c.MountAbove();
    const s64 mount_end = d.t;
    c.Down(12);
    c.ExitBottom();
    c.Off(1000 * Ms);
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 2u);
    EXPECT_EQ(d.Count(C::Phase::FadeBack), 2u);
    EXPECT_NEAR(d.When(C::Phase::FadeOut, mount_end), 500.0, MsOf(Frame30) + 0.01);
    const Change* back = d.First(C::Phase::FadeBack, mount_end);
    ASSERT_TRUE(back != nullptr);
    EXPECT_EQ(Reason(back), std::string("lead"));
    EXPECT_EQ(d.Phase(), C::Phase::Off);
}

// A cut during the fade out (getting off within the dim): the fade is aborted and brightens again
// from where it was, without a layer switch and without a heading re-sync.
TEST(LadderMachine, CutDuringFadeOut) {
    Driver d;
    d.frame = Frame30;
    Presenter p;
    Climber c{d};
    c.rungs = 30;
    c.on_tick = [&] { p.Sample(d); };
    c.Off(300 * Ms);
    c.MountBelow();
    c.Up(1);
    ++c.rung;
    while (d.Phase() != C::Phase::FadeOut) {
        d.Tick(c.Sample(3, -1));
        p.Sample(d);
    }
    d.Tick(c.Sample(3, -1)); // a third of the way down
    p.Sample(d);
    EXPECT_EQ(d.Phase(), C::Phase::FadeOut);
    const float dimmed = p.level;
    EXPECT_LT(dimmed, 0.8f);
    d.Tick(c.Sample(10, 101035));
    p.Sample(d);
    EXPECT_EQ(d.Phase(), C::Phase::Returning);
    EXPECT_EQ(std::string(d.last.reason), std::string("attack"));
    EXPECT_FALSE(d.last.resync);
    const float ramp = static_cast<float>(Frame30) / (100.0f * Ms); // one update of the fade
    EXPECT_NEAR(d.last.b0, 1.0f - 2.0f * ramp, 1e-3f);
    // The presenter samples an update later: up from b0 by one update, no jump.
    EXPECT_NEAR(p.level, d.last.b0 + ramp, 1e-3f);
    EXPECT_LE(p.level, dimmed + ramp + 1e-4f);
    float previous = p.level;
    bool rising = true;
    for (int i = 0; i < 60; ++i) {
        d.Tick(c.Sample(10, -1));
        p.Sample(d);
        rising = rising && p.level >= previous - 1e-6f;
        previous = p.level;
    }
    EXPECT_TRUE(rising);
    EXPECT_NEAR(p.level, 1.0f, 1e-6f);
    EXPECT_TRUE(p.immersive); // the screen never came up
    EXPECT_FALSE(p.popped);
    EXPECT_EQ(d.Count(C::Phase::Flat), 0u);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
}

// Without the ladder object (signature or runtime check failed) the length is unknown: flat after
// two steps in one direction, back to VR only on a cut (here the dismount).
TEST(LadderMachine, LengthUnknown) {
    Driver d;
    d.frame = Frame30;
    Climber c{d};
    c.rungs = 12;
    c.object_valid = false;
    c.Off(300 * Ms);
    c.MountBelow();
    const s64 mount_end = d.t;
    c.Up(12);
    const s64 end_push = d.t;
    c.ExitTop();
    c.Off(1000 * Ms);
    EXPECT_NEAR(d.When(C::Phase::FadeOut, mount_end), 500.0, MsOf(Frame30) + 0.01);
    const Change* enter = d.FirstLadder(C::LadderEvent::Enter);
    ASSERT_TRUE(enter != nullptr);
    EXPECT_EQ(enter->step.ladder.pred_ns, -1);
    const Change* back = d.First(C::Phase::FadeBack);
    ASSERT_TRUE(back != nullptr);
    EXPECT_EQ(back->t, end_push);
    EXPECT_EQ(Reason(back), std::string("dismount_top"));
    EXPECT_EQ(d.Count(C::Phase::FadeBack), 1u);

    // One step is not enough.
    Driver e;
    e.frame = Frame30;
    Climber f{e};
    f.object_valid = false;
    f.Off(300 * Ms);
    f.MountBelow();
    f.Up(1);
    f.Idle(2000 * Ms);
    EXPECT_TRUE(e.changes.empty());
    f.Up(1);
    EXPECT_EQ(e.Phase(), C::Phase::Flat);
}

// Another category while on a ladder keeps its own rules (exit_ms, min_ms, its fade), and wins
// over the ladder while it lasts.
TEST(LadderMachine, OtherCategoryOnLadder) {
    // A scripted event during a short climb (never flat by itself).
    {
        Driver d;
        d.frame = Frame30;
        Climber c{d};
        c.rungs = 4;
        c.Off(300 * Ms);
        c.MountBelow();
        c.Up(1);
        const s64 start = d.t;
        c.event = 103000;
        c.Up(1);
        c.event = -1;
        c.Up(2);
        c.ExitTop();
        c.Off(1000 * Ms);
        const Change* out = d.First(C::Phase::FadeOut);
        ASSERT_TRUE(out != nullptr);
        EXPECT_EQ(out->t, start);
        EXPECT_EQ(out->step.category, C::Category::Scripted);
        EXPECT_EQ(out->step.fade_ns, 150 * Ms);
        EXPECT_NEAR(d.When(C::Phase::Returning, start), 750.0, MsOf(Frame30) + 0.01); // min_ms
        EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u);
    }
    // An event over the lead exit of a flat climb: flat until the event ends + exit_ms.
    {
        Driver d;
        d.frame = Frame30;
        Climber c{d};
        c.rungs = 12;
        c.Off(300 * Ms);
        c.MountBelow();
        c.Up(9);
        c.event = 103000;
        c.Up(2);
        c.event = -1;
        const s64 event_end = d.t;
        c.Up(1);
        c.ExitTop();
        c.Off(1000 * Ms);
        const Change* lead = d.FirstLadder(C::LadderEvent::Exit);
        ASSERT_TRUE(lead != nullptr);
        EXPECT_EQ(LadderReason(lead), std::string("lead"));
        EXPECT_FALSE(lead->step.changed); // the event holds the flat period
        const Change* back = d.First(C::Phase::Returning);
        ASSERT_TRUE(back != nullptr);
        EXPECT_NEAR(MsOf(back->t - event_end), 50.0, MsOf(Frame30) + 0.01);
        EXPECT_EQ(Reason(back), std::string("signals off"));
        EXPECT_EQ(back->step.fade_ns, 80 * Ms); // the event's quick way back
        EXPECT_EQ(d.Count(C::Phase::FadeBack), 0u);
        EXPECT_EQ(d.Count(C::Phase::Returning), 1u);
    }
    // An event that ends while the climb is still flat: the ladder holds it again and returns at
    // once on its lead exit.
    {
        Driver d;
        d.frame = Frame30;
        Climber c{d};
        c.rungs = 12;
        c.Off(300 * Ms);
        c.MountBelow();
        const s64 mount_end = d.t;
        c.Up(3);
        c.event = 103000;
        c.Up(1);
        c.event = -1;
        c.Up(8);
        c.ExitTop();
        const Change* back = d.First(C::Phase::FadeBack);
        ASSERT_TRUE(back != nullptr);
        EXPECT_EQ(Reason(back), std::string("lead"));
        EXPECT_NEAR(MsOf(back->t - mount_end), 4000.0, MsOf(Frame30) + 0.01);
        EXPECT_EQ(d.Count(C::Phase::FadeBack), 1u);
    }
}

// Menu held during a flat climb: first-person VR at once, and the rest of this climb stays in VR;
// the next ladder goes flat again.
TEST(LadderMachine, EscapeHatch) {
    Driver d;
    d.frame = Frame30;
    Climber c{d};
    c.rungs = 40;
    c.Off(300 * Ms);
    c.MountBelow();
    c.Up(3);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    const s64 held = d.t;
    ++c.rung;
    c.Hold(2, 101011, 400 * Ms, true);
    const Change* back = d.First(C::Phase::FadeBack, held);
    ASSERT_TRUE(back != nullptr);
    EXPECT_EQ(back->t, held);
    EXPECT_EQ(Reason(back), std::string("escape hatch"));
    c.Up(20);
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u);
    EXPECT_TRUE(d.machine.Ladder().Suppressed());
    c.ExitTop();
    EXPECT_FALSE(d.machine.Ladder().Suppressed());
    c.Off(300 * Ms);
    c.rungs = 20;
    c.MountAbove();
    c.Down(10);
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 2u);
}

// cinematic_ladder_fade_ms=0: a hard cut both ways, the return still re-syncs the heading.
TEST(LadderMachine, HardCut) {
    Driver d;
    d.frame = Frame30;
    d.cfg.ladder.fade_ns = 0;
    Climber c{d};
    c.Off(300 * Ms);
    c.MountBelow();
    const s64 mount_end = d.t;
    c.Up(12);
    c.ExitTop();
    c.Off(1000 * Ms);
    EXPECT_NEAR(d.When(C::Phase::Flat, mount_end), 500.0, MsOf(Frame30) + 0.01);
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 0u);
    EXPECT_EQ(d.Count(C::Phase::FadeBack), 0u);
    const Change* returning = d.First(C::Phase::Returning);
    ASSERT_TRUE(returning != nullptr);
    EXPECT_TRUE(returning->step.resync);
    EXPECT_EQ(Reason(returning), std::string("lead"));
    EXPECT_EQ(d.Phase(), C::Phase::Off);
}

// The toggles, VR not shown, and an unexpected state value.
TEST(LadderMachine, TogglesAndOddInput) {
    for (int which = 0; which < 2; ++which) {
        Driver d;
        d.frame = Frame30;
        if (which == 0) {
            d.cfg.categories[C::CategoryIndex(C::Category::Ladder)] = false;
        } else {
            d.cfg.enabled = false;
        }
        Climber c{d};
        c.Off(300 * Ms);
        c.MountBelow();
        c.Up(12);
        c.ExitTop();
        EXPECT_TRUE(d.changes.empty());
        EXPECT_TRUE(d.ladder.empty());
    }
    // The virtual screen is up (VR not shown): no flat climb and no skip; once VR shows, the
    // climb goes flat.
    {
        Driver d;
        d.frame = Frame30;
        d.can_enter = false;
        Climber c{d};
        c.rungs = 30;
        c.Off(300 * Ms);
        c.MountBelow();
        c.Up(4);
        EXPECT_TRUE(d.changes.empty());
        EXPECT_TRUE(d.ladder.empty());
        d.can_enter = true;
        c.Up(1);
        EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u);
    }
    // An unexpected state value counts as idle and is reported once.
    {
        Driver d;
        d.frame = Frame30;
        Climber c{d};
        c.rungs = 30;
        c.Off(300 * Ms);
        c.MountBelow();
        c.Up(3);
        u32 reported = 0;
        for (int i = 0; i < 2; ++i) {
            for (int k = 0; k < 6; ++k) {
                reported += d.Tick(c.Sample(7, -1)).ladder.unknown_state ? 1 : 0;
            }
            c.Up(1);
        }
        EXPECT_EQ(reported, 1u);
        EXPECT_EQ(d.Phase(), C::Phase::Flat);
    }
    // The ladder signal goes away (a failed runtime check) while flat: back at once.
    {
        Driver d;
        d.frame = Frame30;
        Climber c{d};
        c.rungs = 30;
        c.Off(300 * Ms);
        c.MountBelow();
        c.Up(3);
        C::Signals s = c.Sample(2, -1);
        s.ladder.valid = false;
        d.Tick(s);
        EXPECT_EQ(d.Phase(), C::Phase::FadeBack);
        EXPECT_EQ(std::string(d.last.reason), std::string("invalid"));
    }
}

// The damage threshold: an HP drop of at least damage_pct (3 %) of the maximum in one update.
TEST(LadderGate, DamageThreshold) {
    EXPECT_EQ(C::LadderDamageMin(3.0f, 500, 500), 15);
    EXPECT_EQ(C::LadderDamageMin(3.0f, 1052, 900), 32); // 31.56 rounds up
    EXPECT_EQ(C::LadderDamageMin(3.0f, 0, 400), 12);    // the maximum unknown: the HP before
    EXPECT_EQ(C::LadderDamageMin(3.0f, 200, 500), 15);  // a maximum below the HP is not plausible
    EXPECT_EQ(C::LadderDamageMin(3.0f, 10'000'000, 500), 15);
    EXPECT_EQ(C::LadderDamageMin(3.0f, 500, 0), 15);
    EXPECT_EQ(C::LadderDamageMin(3.0f, 0, 0), 1);
    EXPECT_EQ(C::LadderDamageMin(0.0f, 500, 500), 1); // 0 = any drop
    EXPECT_EQ(C::LadderDamageMin(100.0f, 500, 500), 500);
}

// Poison (or any slow drain) loses a little HP every tick: not damage, so a long poisoned climb is
// one flat period (never flat / VR / flat, never a dim blink). 3 % of the maximum at once cuts.
TEST(LadderMachine, PoisonDrainDoesNotFlicker) {
    struct Drain {
        s64 period;
        s32 hp; // lost each period
    };
    for (const Drain drain :
         {Drain{300 * Ms, 1}, Drain{1000 * Ms, 1}, Drain{1500 * Ms, 1}, Drain{2500 * Ms, 1},
          Drain{4000 * Ms, 1}, Drain{1000 * Ms, 14}, Drain{4000 * Ms, 14}}) {
        Driver d;
        d.frame = Frame30;
        Climber c{d};
        c.rungs = 60;
        c.Off(300 * Ms);
        c.MountBelow();
        s64 next = d.t + drain.period;
        size_t drains = 0;
        c.on_tick = [&] {
            drains += d.last.ladder.drain ? 1 : 0;
            if (d.t >= next) {
                c.hp -= drain.hp;
                next += drain.period;
            }
        };
        c.Up(60);
        c.on_tick = nullptr;
        c.ExitTop();
        c.Off(1000 * Ms);
        EXPECT_GT(c.hp, 0);
        EXPECT_GT(drains, 0u);
        EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u);
        EXPECT_EQ(d.Count(C::Phase::FadeBack), 1u);
        EXPECT_EQ(Reason(d.First(C::Phase::FadeBack)), std::string("lead"));
        EXPECT_EQ(d.Aborts(), 0u);
        EXPECT_EQ(d.Phase(), C::Phase::Off);
    }
    for (const s32 lost : {14, 15}) {
        Driver d;
        d.frame = Frame30;
        Climber c{d};
        c.rungs = 30;
        c.Off(300 * Ms);
        c.MountBelow();
        c.Up(4);
        EXPECT_EQ(d.Phase(), C::Phase::Flat);
        const s64 hit = d.t;
        c.hp -= lost;
        c.Up(1);
        const Change* back = d.First(C::Phase::FadeBack, hit);
        if (lost < 15) {
            EXPECT_TRUE(back == nullptr);
            EXPECT_EQ(d.Phase(), C::Phase::Flat);
        } else {
            ASSERT_TRUE(back != nullptr);
            EXPECT_EQ(back->t, hit);
            EXPECT_EQ(Reason(back), std::string("damage"));
            EXPECT_EQ(back->step.ladder.damage_min, 15);
        }
    }
}

// The ladder object resets a few updates before a slide's landing (or a dismount) shows in the
// state: a ladder too short to go flat must not go flat in that gap (the "length unknown" rule is
// only for an object that cannot be read at all), so nothing dims. On a flat climb with
// exit_lead 0 the reset itself returns to VR (the dismount starts).
TEST(LadderMachine, SlideLandingResetGap) {
    for (const s64 gap : {s64{0}, s64{1}, s64{2}}) {
        Driver d;
        d.frame = Frame30;
        Climber c{d};
        c.rungs = 12;
        c.Off(300 * Ms);
        c.MountAbove();
        --c.rung;
        c.Hold(14, 101000, 167 * Ms);
        bool first = true;
        while (c.rung > 0) {
            --c.rung;
            c.Hold(14, first ? 101001 : -1, 133 * Ms);
            first = false;
        }
        c.object = c.rung = c.top = 0; // the slide code's reset, Slide_End not shown yet
        for (s64 i = 0; i < gap; ++i) {
            d.Tick(c.Sample(14, -1));
        }
        c.Hold(-1, 101002, 833 * Ms);
        c.Off(1000 * Ms);
        EXPECT_TRUE(d.changes.empty());
        EXPECT_EQ(d.LadderCount(C::LadderEvent::Enter), 0u);
    }
    // A short ladder whose animation ids cannot be read, the object reset before the dismount
    // state.
    for (const s64 gap : {s64{0}, s64{1}, s64{2}}) {
        Driver d;
        d.frame = Frame30;
        Climber c{d};
        c.rungs = 6;
        c.ids = false;
        c.Off(300 * Ms);
        c.MountBelow();
        c.Up(6);
        const s8 last = (c.rung & 1) != 0 ? 2 : 3;
        c.object = c.rung = c.top = 0;
        for (s64 i = 0; i < gap; ++i) {
            d.Tick(c.Sample(last, -1));
        }
        c.ExitTop();
        c.Off(1000 * Ms);
        EXPECT_TRUE(d.changes.empty());
        EXPECT_EQ(d.LadderCount(C::LadderEvent::Skip), 1u);
        EXPECT_EQ(d.LadderCount(C::LadderEvent::Enter), 0u);
    }
    // A flat climb with exit_lead 0: back to VR on the reset, ahead of the dismount's state.
    Driver d;
    d.frame = Frame30;
    d.cfg.ladder.exit_lead_ns = 0;
    Climber c{d};
    c.rungs = 12;
    c.Off(300 * Ms);
    c.MountBelow();
    c.Up(12);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    const s8 last = (c.rung & 1) != 0 ? 2 : 3;
    c.object = c.rung = c.top = 0;
    const s64 reset = d.t;
    d.Tick(c.Sample(last, -1));
    c.ExitTop();
    c.Off(1000 * Ms);
    const Change* back = d.First(C::Phase::FadeBack);
    ASSERT_TRUE(back != nullptr);
    EXPECT_EQ(back->t, reset);
    EXPECT_EQ(Reason(back), std::string("reset"));
    EXPECT_FALSE(back->step.ladder.cut);
    EXPECT_EQ(d.Count(C::Phase::FadeBack), 1u);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
}

// The escape hatch lasts for the rest of that climb only. After a death on the ladder (the death
// states never write the ladder state) and a respawn without loading, the next ladder's mount
// writes its state straight over the stale value: a new climb, which goes flat again.
TEST(LadderMachine, EscapeHatchDoesNotLeakPastDeath) {
    for (const bool escape : {false, true}) {
        Driver d;
        d.frame = Frame30;
        Climber c{d};
        c.rungs = 40;
        c.Off(300 * Ms);
        c.MountBelow();
        c.Up(3);
        EXPECT_EQ(d.Phase(), C::Phase::Flat);
        if (escape) {
            ++c.rung;
            c.Hold(2, 101011, 400 * Ms, true);
            EXPECT_TRUE(d.machine.Ladder().Suppressed());
        }
        c.Idle(300 * Ms);
        c.hp = 0;
        c.Hold(8, 24700, 4000 * Ms);
        c.hp = 500;
        c.Hold(8, -1, 3000 * Ms); // respawned: the stale state, no -1
        EXPECT_TRUE(d.machine.Ladder().Dead());
        const size_t before = d.Count(C::Phase::FadeOut);
        // The next ladder, from above: its mount writes 1 over the stale 8.
        c.rungs = 20;
        c.object = 1;
        c.rung = c.top = 20;
        c.Hold(1, 101020, 2000 * Ms);
        EXPECT_FALSE(d.machine.Ladder().Suppressed());
        EXPECT_FALSE(d.machine.Ladder().Dead());
        EXPECT_EQ(d.Count(C::Phase::FadeOut), before); // getting on stays in VR
        c.Down(20);
        c.ExitBottom();
        c.Off(500 * Ms);
        EXPECT_EQ(d.Count(C::Phase::FadeOut), before + 1);
        EXPECT_EQ(d.Phase(), C::Phase::Off);
    }
}

// A step's state byte arriving a few updates after its clip (a slow frame, a late byte), or its
// clip after the byte, is one step: with the length unknown, one real step and a pause must not go
// flat. Two clips are always two steps (the fast steps never write the byte).
TEST(LadderMachine, SlowStepIsOneStep) {
    struct Lag {
        s64 frame;
        s64 updates; // the second sign this many updates after the first
        bool clip_first;
    };
    for (const Lag lag : {Lag{Frame30, 4, true}, Lag{Frame30, 4, false}, Lag{Frame, 8, true},
                          Lag{4 * Frame, 2, true}, Lag{4 * Frame, 3, false}}) {
        Driver d;
        d.frame = lag.frame;
        Climber c{d};
        c.rungs = 20;
        c.object_valid = false;
        c.Off(300 * Ms);
        c.MountBelow();
        c.Idle(300 * Ms);
        ++c.rung;
        const s8 before = lag.clip_first ? 8 : 2;
        d.Tick(c.Sample(before, lag.clip_first ? 101011 : -1));
        for (s64 i = 1; i < lag.updates; ++i) {
            d.Tick(c.Sample(before, -1));
        }
        d.Tick(c.Sample(2, lag.clip_first ? -1 : 101011));
        EXPECT_EQ(d.last.ladder.steps, 1u);
        c.Hold(2, -1, 300 * Ms);
        c.Idle(2000 * Ms);
        EXPECT_TRUE(d.changes.empty());
        // The next step is the second: flat at once (the climb timer runs from the first).
        c.Up(1);
        EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u);
    }
    Driver d;
    d.frame = Frame30;
    Climber c{d};
    c.rungs = 20;
    c.Off(300 * Ms);
    c.MountBelow();
    c.Idle(300 * Ms);
    c.FastUp(3);
    EXPECT_EQ(d.last.ladder.steps, 3u);
    EXPECT_EQ(d.last.ladder.step_ns, C::LadderFastStepNs);
}

// A damage bind: hit on the ladder, the state reads -1 for a moment while the hunter stays on it
// (the ladder object still reads on the ladder). The climb goes on: with combat_exit the flat
// period ends at once and comes back after the next step and reenter; without it the climb stays
// flat; the escape hatch is kept. A bind longer than LadderBindMaxNs, or one that knocks the hunter
// off (the ladder object resets), ends the climb.
TEST(LadderMachine, DamageBind) {
    // With and without the damage clip (whether the game pushes it is not known): the HP drop.
    for (const s32 clip : {101060, -1}) {
        Driver d;
        d.frame = Frame30;
        Climber c{d};
        c.rungs = 30;
        c.Off(300 * Ms);
        c.MountBelow();
        c.Up(6);
        EXPECT_EQ(d.Phase(), C::Phase::Flat);
        const s64 bind = d.t;
        c.hp = 300;
        c.Hold(-1, clip, 600 * Ms);
        const Change* back = d.First(C::Phase::FadeBack, bind);
        ASSERT_TRUE(back != nullptr);
        EXPECT_EQ(back->t, bind);
        EXPECT_EQ(Reason(back), std::string("damage"));
        EXPECT_TRUE(back->step.ladder.bind);
        EXPECT_TRUE(d.machine.Ladder().InSession());
        EXPECT_EQ(d.last.ladder.dir, 1);
        c.Idle(300 * Ms);
        const s64 climb = d.t;
        c.Up(10);
        EXPECT_NEAR(d.When(C::Phase::FadeOut, bind), MsOf(climb - bind) + 500.0,
                    MsOf(Frame30) + 0.01);
        EXPECT_EQ(d.Count(C::Phase::FadeOut), 2u);
    }
    // The escape hatch is kept through a bind: that climb stays in VR.
    {
        Driver d;
        d.frame = Frame30;
        Climber c{d};
        c.rungs = 30;
        c.Off(300 * Ms);
        c.MountBelow();
        c.Up(4);
        ++c.rung;
        c.Hold(2, 101011, 400 * Ms, true);
        EXPECT_TRUE(d.machine.Ladder().Suppressed());
        c.Up(2);
        c.hp = 300;
        c.Hold(-1, 101060, 600 * Ms);
        EXPECT_TRUE(d.machine.Ladder().Suppressed());
        c.Idle(300 * Ms);
        c.Up(15);
        c.ExitTop();
        c.Off(1000 * Ms);
        EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u);
        EXPECT_EQ(d.Phase(), C::Phase::Off);
    }
    // combat_exit=0: the climb stays flat through the bind.
    {
        Driver d;
        d.frame = Frame30;
        d.cfg.ladder.combat_exit = false;
        Climber c{d};
        c.rungs = 30;
        c.Off(300 * Ms);
        c.MountBelow();
        c.Up(6);
        c.hp = 300;
        c.Hold(-1, 101060, 600 * Ms);
        c.Idle(300 * Ms);
        c.Up(10);
        EXPECT_EQ(d.Phase(), C::Phase::Flat);
        EXPECT_EQ(d.Count(C::Phase::FadeBack), 0u);
    }
    // A bind that lasts too long ends the climb (without combat_exit, back to VR then).
    for (const bool combat_exit : {true, false}) {
        Driver d;
        d.frame = Frame30;
        d.cfg.ladder.combat_exit = combat_exit;
        Climber c{d};
        c.rungs = 30;
        c.Off(300 * Ms);
        c.MountBelow();
        c.Up(6);
        const s64 bind = d.t;
        c.hp = 300;
        c.Hold(-1, 101060, 2000 * Ms);
        EXPECT_FALSE(d.machine.Ladder().InSession());
        const Change* back = d.First(C::Phase::FadeBack, bind);
        ASSERT_TRUE(back != nullptr);
        EXPECT_EQ(Reason(back), std::string("damage"));
        EXPECT_NEAR(MsOf(back->t - bind), combat_exit ? 0.0 : 1500.0, MsOf(Frame30) + 0.01);
        EXPECT_EQ(d.Count(C::Phase::FadeBack), 1u);
    }
    // Knocked off (the ladder object resets with the -1): a cut, and the climb is over.
    {
        Driver d;
        d.frame = Frame30;
        Climber c{d};
        c.rungs = 40;
        c.Off(300 * Ms);
        c.MountBelow();
        c.Up(4);
        EXPECT_EQ(d.Phase(), C::Phase::Flat);
        c.hp = 300;
        c.object = c.rung = c.top = 0;
        const s64 at = d.t;
        d.Tick(c.Sample(-1, -1));
        const Change* back = d.First(C::Phase::FadeBack);
        ASSERT_TRUE(back != nullptr);
        EXPECT_EQ(back->t, at);
        EXPECT_EQ(Reason(back), std::string("damage"));
        EXPECT_FALSE(back->step.ladder.bind);
        EXPECT_FALSE(d.machine.Ladder().InSession());
    }
}

// A cut before the flat period (here damage during the first steps) disarms the gate and starts
// the re-entry wait like a return: the climb goes flat no sooner than reenter after it.
TEST(LadderMachine, CutBeforeFlatWaitsForReenter) {
    Driver d;
    d.frame = Frame30;
    Climber c{d};
    c.rungs = 30;
    c.Off(300 * Ms);
    c.MountBelow();
    c.Up(1);
    EXPECT_TRUE(d.changes.empty());
    const s64 cut = d.t;
    c.hp = 400;
    c.Up(1);
    EXPECT_TRUE(d.changes.empty());
    c.Up(8);
    EXPECT_NEAR(d.When(C::Phase::FadeOut, cut), 1000.0, MsOf(Frame30) + 0.01);
}

// ---- Canned item uses (Category::Item: the Shaman Bone Blade's stab from the arc) ----------------

namespace {

// One update's item readings: the action flags read, the item-use bit, the item's clip pending.
C::ItemSignals ItemRead(bool bit, u32 serial, s64 arm_ns, bool clip = false) {
    C::ItemSignals s{};
    s.valid = true;
    s.use = bit;
    s.clip = clip;
    s.arm_serial = serial;
    s.arm_ns = arm_ns;
    return s;
}

// The machine's signals with the gate's answer.
C::Signals ItemSig(bool on, bool window = false) {
    C::Signals s = Window(Nothing(), window);
    s.item_valid = true;
    s.item = on;
    return s;
}

} // namespace

// Our Square arms it; the game's item-use bit rising after it starts the use (timed from the arm),
// the bit's end ends it.
TEST(CinematicItemGate, ArmThenTheBit) {
    C::ItemUseGate g;
    s64 t = 1000 * Ms;
    EXPECT_FALSE(g.Update(t, ItemRead(false, 0, 0)).on);
    const s64 arm = t;
    t += Frame;
    EXPECT_FALSE(g.Update(t, ItemRead(false, 1, arm)).on); // the game has not taken it yet
    EXPECT_TRUE(g.Armed());
    t += 4 * Frame;
    const C::ItemOut start = g.Update(t, ItemRead(true, 1, arm));
    EXPECT_TRUE(start.on);
    EXPECT_TRUE(start.event == C::ItemEvent::Start);
    EXPECT_EQ(start.arm_age_ns, 5 * Frame);
    EXPECT_FALSE(start.by_clip);
    for (int i = 0; i < 60; ++i) {
        t += Frame;
        const C::ItemOut o = g.Update(t, ItemRead(true, 1, arm));
        EXPECT_TRUE(o.on);
        EXPECT_TRUE(o.event == C::ItemEvent::None);
    }
    t += Frame;
    const C::ItemOut end = g.Update(t, ItemRead(false, 1, arm));
    EXPECT_FALSE(end.on);
    EXPECT_TRUE(end.event == C::ItemEvent::End);
    EXPECT_EQ(end.use_age_ns, 61 * Frame);
    // The same arm never starts again: the next item use (the vial) is not ours.
    for (int i = 0; i < 30; ++i) {
        t += Frame;
        EXPECT_FALSE(g.Update(t, ItemRead(i > 10, 1, arm)).on);
    }
}

// No Square of ours: the item-use bit (a vial, a throw, the game's own D-pad use) is never canned.
TEST(CinematicItemGate, NoArmNoUse) {
    C::ItemUseGate g;
    s64 t = 1000 * Ms;
    for (int i = 0; i < 120; ++i) {
        t += Frame;
        const C::ItemOut o = g.Update(t, ItemRead((i / 20) % 2 == 1, 0, 0));
        EXPECT_FALSE(o.on);
        EXPECT_TRUE(o.event == C::ItemEvent::None);
    }
}

// An arm the game never answers (busy, refused) expires after ItemArmNs; a later bit is not ours.
TEST(CinematicItemGate, ArmExpires) {
    C::ItemUseGate g;
    s64 t = 1000 * Ms;
    const s64 arm = t;
    bool expired = false;
    while (t < arm + C::ItemArmNs + 5 * Frame) {
        const C::ItemOut o = g.Update(t, ItemRead(false, 7, arm));
        EXPECT_FALSE(o.on);
        if (o.event == C::ItemEvent::Expired) {
            expired = true;
            EXPECT_GT(o.arm_age_ns, C::ItemArmNs);
            EXPECT_LE(o.arm_age_ns, C::ItemArmNs + Frame);
        }
        t += Frame;
    }
    EXPECT_TRUE(expired);
    EXPECT_FALSE(g.Armed());
    EXPECT_FALSE(g.Update(t, ItemRead(true, 7, arm)).on);
}

// The bit already on at the arm (the tail of another use): not started by it; started when it
// drops and rises again, or at once by the item's own clip.
TEST(CinematicItemGate, BitAlreadyOn) {
    {
        C::ItemUseGate g;
        s64 t = 1000 * Ms;
        const s64 arm = t;
        for (int i = 0; i < 10; ++i) {
            EXPECT_FALSE(g.Update(t, ItemRead(true, 1, arm)).on);
            t += Frame;
        }
        EXPECT_FALSE(g.Update(t, ItemRead(false, 1, arm)).on);
        t += Frame;
        const C::ItemOut o = g.Update(t, ItemRead(true, 1, arm));
        EXPECT_TRUE(o.on);
        EXPECT_FALSE(o.by_clip);
    }
    {
        C::ItemUseGate g;
        s64 t = 1000 * Ms;
        const s64 arm = t;
        EXPECT_FALSE(g.Update(t, ItemRead(true, 1, arm)).on);
        t += Frame;
        const C::ItemOut o = g.Update(t, ItemRead(true, 1, arm, true)); // the clip pushed
        EXPECT_TRUE(o.on);
        EXPECT_TRUE(o.event == C::ItemEvent::Start);
        EXPECT_TRUE(o.by_clip);
    }
}

// A one-update gap in the bit (a state chain) is the same use; a longer one ends it.
TEST(CinematicItemGate, GapRejoins) {
    C::ItemUseGate g;
    s64 t = 1000 * Ms;
    const s64 arm = t;
    g.Update(t, ItemRead(false, 1, arm));
    t += Frame;
    EXPECT_TRUE(g.Update(t, ItemRead(true, 1, arm)).on);
    t += 30 * Frame;
    EXPECT_TRUE(g.Update(t, ItemRead(true, 1, arm)).on);
    t += Frame;
    EXPECT_TRUE(g.Update(t, ItemRead(false, 1, arm)).event == C::ItemEvent::End);
    t += Frame;
    const C::ItemOut back = g.Update(t, ItemRead(true, 1, arm));
    EXPECT_TRUE(back.on);
    EXPECT_TRUE(back.event == C::ItemEvent::Rejoin);
    EXPECT_EQ(back.use_age_ns, 32 * Frame);
    t += Frame;
    g.Update(t, ItemRead(false, 1, arm));
    t += C::ItemRejoinNs + Frame;
    EXPECT_FALSE(g.Update(t, ItemRead(true, 1, arm)).on); // too late: not ours any more
}

// A bit that never clears is no use: timed out after ItemMaxNs, ignored until it clears.
TEST(CinematicItemGate, StuckBitTimesOut) {
    C::ItemUseGate g;
    s64 t = 1000 * Ms;
    const s64 arm = t;
    g.Update(t, ItemRead(false, 1, arm));
    t += Frame;
    EXPECT_TRUE(g.Update(t, ItemRead(true, 1, arm)).on);
    bool timed_out = false;
    const s64 until = t + C::ItemMaxNs + 10 * Frame;
    while (t < until) {
        t += Frame;
        const C::ItemOut o = g.Update(t, ItemRead(true, 1, arm));
        timed_out = timed_out || o.event == C::ItemEvent::TimedOut;
        if (timed_out) {
            EXPECT_FALSE(o.on);
        }
    }
    EXPECT_TRUE(timed_out);
    // A new Square while it is stuck: started only once the bit has cleared and risen again.
    const s64 arm2 = t;
    t += Frame;
    EXPECT_FALSE(g.Update(t, ItemRead(true, 2, arm2, true)).on);
    t += Frame;
    EXPECT_FALSE(g.Update(t, ItemRead(false, 2, arm2)).on);
    t += Frame;
    EXPECT_TRUE(g.Update(t, ItemRead(true, 2, arm2)).on);
}

// The stab in the flat view: FadeOut on the use, flat 150 ms later; the game's cancel window opens
// 1.4 s in and VR comes back once it has stayed open 50 ms (the quick return, as a visceral's),
// the heading re-synced; the use's tail never goes flat again, and its latch clears with the bit.
TEST(CinematicMachine, ItemUseReturnsAtControlBack) {
    Driver d;
    d.Run(ItemSig(false), 200 * Ms);
    const s64 start = d.t;
    const C::Step first = d.Tick(ItemSig(true));
    EXPECT_EQ(first.to, C::Phase::FadeOut);
    EXPECT_EQ(first.category, C::Category::Item);
    d.Run(ItemSig(true), 1400 * Ms - Frame);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    EXPECT_NEAR(d.When(C::Phase::Flat, start), 150.0, 17.0);
    const s64 open = d.t;
    bool control_back = false;
    while (d.t < open + 700 * Ms) {
        control_back = d.Tick(ItemSig(true, true)).control_back || control_back;
    }
    EXPECT_TRUE(control_back);
    const double back = d.When(C::Phase::Returning, open);
    EXPECT_GE(back, 50.0);
    EXPECT_LE(back, 50.0 + 17.0);
    EXPECT_EQ(CountControlBack(d), 1u);
    const Change* ret = d.First(C::Phase::Returning);
    ASSERT_TRUE(ret != nullptr);
    EXPECT_TRUE(ret->step.resync);
    bool cleared = false;
    while (d.t < open + 2000 * Ms) {
        cleared = d.Tick(ItemSig(false, true)).latch_cleared || cleared; // the bit clears
    }
    EXPECT_TRUE(cleared);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u); // never flat again from the tail
    ASSERT_EQ(d.returns.size(), 1u);
    const C::ReturnTimes& r = d.returns[0];
    EXPECT_EQ(r.source, C::Category::Item);
    EXPECT_EQ(r.control, open);
    EXPECT_EQ(std::string(r.why), std::string("control back"));
    EXPECT_EQ(r.signals_off, 0); // still on when VR was back
    // The next use goes flat again.
    d.Run(ItemSig(true), 500 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
}

// Without a cancel window: back 50 ms after the use ends (at least the shortest flat period).
TEST(CinematicMachine, ItemUseWithoutWindowReturnsAtItsEnd) {
    Driver d;
    d.Run(ItemSig(true), 2000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    const s64 end = d.t;
    d.Run(ItemSig(false), 1500 * Ms);
    EXPECT_NEAR(d.When(C::Phase::Returning, end), 50.0, 17.0);
    EXPECT_EQ(std::string(Reason(d.First(C::Phase::Returning))), std::string("signals off"));
    ASSERT_EQ(d.returns.size(), 1u);
    EXPECT_EQ(d.returns[0].source, C::Category::Item);
}

// cinematic_item=0 (or no valid flags): the use stays in VR; the other categories are unchanged.
TEST(CinematicMachine, ItemToggleOff) {
    Driver d;
    d.cfg.categories[C::CategoryIndex(C::Category::Item)] = false;
    d.Run(ItemSig(true), 2000 * Ms);
    EXPECT_TRUE(d.changes.empty());
    EXPECT_EQ(d.machine.GetCategory(), C::Category::Item); // detected (the log), not requested
    d.Run(Event(101100), 400 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);

    Driver e;
    C::Signals s = ItemSig(true);
    s.item_valid = false;
    e.Run(s, 2000 * Ms);
    EXPECT_TRUE(e.changes.empty());
}

// The escape hatch during the stab: VR at once, the rest of that use stays in VR.
TEST(CinematicMachine, ItemEscapeHatch) {
    Driver d;
    d.Run(ItemSig(true), 800 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);
    d.Tick(ItemSig(true), true);
    EXPECT_EQ(d.Phase(), C::Phase::Returning);
    d.Run(ItemSig(true), 1000 * Ms);
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u);
    d.Run(ItemSig(false), 200 * Ms);
    d.Run(ItemSig(true), 400 * Ms); // the next use
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 2u);
}

// The gate and the machine together, as the main player's update steps them: the Square, the
// game's use 5 updates later, its cancel window at 1.4 s, the bit clearing 0.6 s after it.
TEST(CinematicMachine, ItemGateDrivesTheFlatView) {
    Driver d;
    C::ItemUseGate g;
    d.Run(ItemSig(false), 200 * Ms);
    const s64 arm = d.t;
    const auto tick = [&](bool bit, bool window) {
        const C::ItemOut o = g.Update(d.t, ItemRead(bit, 1, arm));
        return d.Tick(ItemSig(o.on, window));
    };
    for (int i = 0; i < 5; ++i) {
        tick(false, false);
    }
    const s64 use = d.t;
    while (d.t < use + 1400 * Ms) {
        tick(true, false);
    }
    const s64 open = d.t;
    while (d.t < open + 600 * Ms) {
        tick(true, true);
    }
    while (d.t < open + 2000 * Ms) {
        tick(false, true);
    }
    EXPECT_NEAR(d.When(C::Phase::FadeOut, arm), 5.0 * MsOf(Frame), 1.0);
    EXPECT_NEAR(d.When(C::Phase::Returning, open), 50.0, 17.0);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_EQ(d.Count(C::Phase::FadeOut), 1u);
    ASSERT_EQ(d.returns.size(), 1u);
    EXPECT_EQ(d.returns[0].source, C::Category::Item);
}


TEST(CinematicMachine, VrGrabsKeepVictimsInVrAndOtherScenesOnScreen) {
    for (const int state : {2, 4, 6}) {
        Driver d;
        d.cfg.categories[static_cast<size_t>(C::Category::Grabbed)] = false;
        d.Run(Throw(state), 3000 * Ms);
        EXPECT_TRUE(d.changes.empty());
        EXPECT_EQ(d.Phase(), C::Phase::Off);
        d.Run(Nothing(), 100 * Ms);
        d.Run(Throw(1), 1000 * Ms);
        EXPECT_EQ(d.Phase(), C::Phase::Flat);
    }
    Driver door;
    door.cfg.categories[static_cast<size_t>(C::Category::Grabbed)] = false;
    door.Run(Event(101100), 1000 * Ms);
    EXPECT_EQ(door.Phase(), C::Phase::Flat);
}

TEST(CinematicMachine, VrGrabOverridesOverlappingEventAndItemOnlyDuringGrab) {
    Driver d;
    d.cfg.vr_grab_attacks = true;
    auto grabbed = Throw(2);
    grabbed.event_valid = true;
    grabbed.event = true;
    grabbed.ids_valid = true;
    grabbed.event_id = 103000;
    grabbed.item_valid = true;
    grabbed.item = true;
    d.Run(grabbed, 2000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Off);
    EXPECT_TRUE(d.changes.empty());
    d.Run(Nothing(), 100 * Ms);
    d.Run(Event(103000), 1000 * Ms);
    EXPECT_EQ(d.Phase(), C::Phase::Flat);

    Driver legacy;
    legacy.Run(grabbed, 1000 * Ms);
    EXPECT_EQ(legacy.Phase(), C::Phase::Flat);
}

TEST(CinematicMachine, VrGrabOptionPreservesInteractiveVisceralsAndObjectScenes) {
    for (const bool vr_grabs : {false, true}) {
        Driver visceral;
        visceral.cfg.vr_grab_attacks = vr_grabs;
        // Runtime policy when the interactive visceral hook is installed.
        visceral.cfg.categories[static_cast<size_t>(C::Category::Visceral)] = false;
        visceral.Run(Throw(1), 2000 * Ms);
        EXPECT_TRUE(visceral.changes.empty());
        for (const int event : {101100, 101141, 101150, 103000}) {
            Driver scene;
            scene.cfg.vr_grab_attacks = vr_grabs;
            scene.Run(Event(event), 1000 * Ms);
            EXPECT_EQ(scene.Phase(), C::Phase::Flat) << event;
        }
        // Physical lever dispatch removes the canned hunter event; it never requests flat.
        Driver physical_lever;
        physical_lever.cfg.vr_grab_attacks = vr_grabs;
        physical_lever.Run(Nothing(), 2000 * Ms);
        EXPECT_TRUE(physical_lever.changes.empty());
    }
}
