// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>

#include "core/vr/melee_moves.h"

// The Bloodletter's blood explosion, two ways (2026-11-21: "A tap of the trigger =
// plays the animation. A long hold of the trigger + smashing the weapon down = interactive blood
// explosion ... releasing the trigger = plays the animation. But holding the trigger and smashing
// down = does the blood explosion without the animation. And the right next trigger release is
// ignored"; a long hold let go without a smash: "Cancel it"; the hold "0.52s"):
//  - a tap (let go before ArmS): the game's own L2, its animation, as a press of TapPulseS;
//  - held ArmS: armed (a heartbeat in the hand); a downward strike with the weapon while
//    armed: the game's own blast without its animation (its attack 930 round the flail's head,
//    its self-bleed 5286 on the hunter, its stamina), then nothing until the trigger is let go;
//  - armed and let go without a smash: nothing.
// Its form B only (the flail); the trigger is the special's (the left one, or the right one in
// the optimized two-handed controls). Pure: no game state.
namespace VR::BloodBlast {

inline constexpr std::int32_t Family = 290;

inline constexpr float ArmS = 0.52f;       // held this long: armed
inline constexpr float TapPulseS = 0.15f;  // a tap's L2 press to the game
inline constexpr float CooldownS = 2.0f;   // between two blasts (the animation's own pace)
// The smash: the hand down at SmashDownMps or faster, at least SmashTravelM of it, then stopped
// (down to StopShare of its fastest) within StopWithinS of first going that fast (a blow, not a long
// sweep down). The hand's speed relative to the head: ducking is no blow. (2026-11-21, the user:
// "a little bit less gentle ... but I don't want it to feel frustrating": 1.85 m/s, 14 cm, 0.55;
// the gentle slam's were 1.65, 12 cm, 0.60; the first 2.6, 20 cm, 0.35.)
inline constexpr float SmashDownMps = 1.85f;
inline constexpr float SmashTravelM = 0.14f;
inline constexpr float StopShare = 0.55f;
inline constexpr float StopWithinS = 0.42f;

// The game's blast (AtkParam 2900930: a 2.2 m sphere at dummy 12130, the flail's head, x2.0, 90
// stamina in its behaviour) and its cost on the hunter (2900931's SpEffect 5286: blood damage
// every 0.1 s for 5 s), as the game's L2 makes them.
inline constexpr std::int32_t BlastJudge = 930;
inline constexpr std::int32_t BlastDummy = 12130;
inline constexpr float BlastRadiusM = 2.2f;
inline constexpr std::int32_t SelfSpEffect = 5286;
inline constexpr Melee::MoveRow Row{BlastJudge, 90, 200, BlastDummy, -1, BlastRadiusM, 0.0f, false,
                                    -1, -1, -1, -1, 0.0f, 0.0f};
// Its blood: the game's own, as its L2 clip spawns it (c0000 a146.tae, animation 46000, TAE
// event 96 SpawnFFX at 1.033 s: FFX 1030002 for 0.1 s and FFX 252060 for 0.83 s, both at dummy
// 12100), in slots of our own. Slot effects live while they are asked for each update: asked for
// their times from the blast on (BloodBlastFfxTick).
inline constexpr std::int32_t FlashFfx = 1030002, BurstFfx = 252060;
inline constexpr std::int32_t FfxDummy = 12100;
inline constexpr std::int32_t FlashSlot = 30340, BurstSlot = 30341;
inline constexpr float FlashS = 0.10f, BurstS = 0.83f;

// The blow must hit something (2026-11-21: "downward blow that hits something (doesn't
// matter if it hits a floor or an enemy or a wall)"): the map (the weapon's strike against it,
// core/vr/weapon_wall.h) or a live enemy's hurt capsule within EnemyTouchM of the flail's head
// (the game's side: g_enemy_touch_ns). A touch counts from the stroke's start to ContactGraceS
// after the hand stopped, including when the trigger was released on impact.
inline constexpr float ContactGraceS = 0.15f;
inline constexpr float EnemyTouchM = 0.25f;
// Game -> presenter, steady clock. Map contact comes from the actual weapon sweep.
// Enemy capsule queries run only during a candidate stroke and its contact grace.
inline std::atomic<std::int64_t> g_map_touch_ns{0};
inline std::atomic<std::int64_t> g_enemy_touch_ns{0};
inline std::atomic<bool> g_check_enemy{false};

struct ContactHistory {
    std::int64_t seen_ns{};
    bool Consume(std::int64_t touch, std::int64_t now) {
        if (touch <= seen_ns || touch > now) return false;
        seen_ns = touch;
        return now - touch <= 150'000'000;
    }
};

struct Out {
    bool game_press{false}; // the game's L2 this frame (a tap's press)
    bool armed{false};      // armed this frame (the edge)
    bool fired{false};      // the blast this frame (the edge)
    bool cancelled{false};  // armed, let go without a smash (the edge)
    bool holding{false};    // the trigger is the gate's (never the gun's) while it is held
};

class Gate {
public:
    void Reset() {
        state_ = State::Idle;
        prev_ = true; // A trigger held across a menu or focus switch needs a fresh press.
        pulse_ = 0.0f;
        ClearStroke();
    }
    bool Armed() const {
        return state_ == State::Armed;
    }
    bool WatchingImpact() const {
        return Armed() || state_ == State::ReleasedSlam;
    }
    // The trigger held for the blast, for the hold's hum in the hand: its share of ArmS while it
    // arms, 1 armed, 0 otherwise (let go, a tap's release, after the blast, not ready).
    float HoldShare() const {
        if (state_ == State::Armed) {
            return 1.0f;
        }
        return state_ == State::Holding ? std::min(held_ / ArmS, 1.0f) : 0.0f;
    }
    bool NeedsEnemyContact() const {
        return WatchingImpact() && !spent_ && (smash_.travel > 0.0f || await_ >= 0.0f);
    }
    void ConfirmBlast() {
        cooldown_ = CooldownS;
    }
    // `trigger`: the special's trigger now; `hand_vy`: the sword hand's speed up relative to the
    // head (m/s; down is negative); `ready`: the flail in hand, nothing else owning the trigger.
    // `contact`: the flail touched something this frame (the map or an enemy).
    Out Step(bool trigger, float dt, float hand_vy, bool ready, bool contact, bool tracked = true) {
        Out out{};
        dt = std::clamp(dt, 0.0f, 0.1f);
        cooldown_ = std::max(cooldown_ - dt, 0.0f);
        if (pulse_ > 0.0f) {
            out.game_press = true;
            pulse_ = std::max(pulse_ - dt, 0.0f);
        }
        if (!ready) {
            // (A held trigger stays the gate's until let go: no shot from a squeeze it took. A
            // tap's press still to go is dropped: it was the flail's, never another weapon's.)
            state_ = trigger && state_ != State::Idle ? State::WaitRelease : State::Idle;
            pulse_ = 0.0f;
            out.game_press = false;
            out.holding = state_ != State::Idle;
            ClearStroke();
            prev_ = trigger;
            return out;
        }
        switch (state_) {
        case State::Idle:
            if (trigger && !prev_) {
                state_ = State::Holding;
                held_ = 0.0f;
            }
            break;
        case State::Holding:
            if (!trigger) {
                state_ = State::Idle;
                pulse_ = TapPulseS;
                out.game_press = true;
                break;
            }
            held_ += dt;
            if (held_ >= ArmS) {
                state_ = State::Armed;
                ClearStroke();
                out.armed = true;
            }
            break;
        case State::Armed:
            if (!tracked) {
                ClearStroke();
            } else if (Smashed(hand_vy, dt, contact) && cooldown_ <= 0.0f) {
                // A release sampled on the impact frame still finishes that slam.
                state_ = trigger ? State::WaitRelease : State::Idle;
                out.fired = true;
                break;
            }
            if (!trigger) {
                // Contact and controller deceleration may land on different XR samples.
                // Carry only a stopped stroke awaiting contact or a qualified impact awaiting its stop.
                const bool impact = touched_ && smash_.since >= 0.0f && smash_.travel >= SmashTravelM;
                release_left_ = ContactGraceS;
                state_ = await_ >= 0.0f || impact ? State::ReleasedSlam : State::Idle;
                out.cancelled = state_ == State::Idle;
            }
            break;
        case State::ReleasedSlam:
            if (trigger) {
                ClearStroke();
                state_ = State::Holding;
                held_ = 0.0f;
            } else {
                release_left_ -= dt;
                out.fired = tracked && release_left_ >= 0.0f &&
                            Smashed(hand_vy, dt, contact) && cooldown_ <= 0.0f;
                if (!tracked || release_left_ < 0.0f || (await_ < 0.0f && smash_.since < 0.0f)) {
                    state_ = State::Idle;
                    out.cancelled = !out.fired;
                }
            }
            break;
        case State::WaitRelease:
            if (!trigger) {
                state_ = State::Idle;
            }
            break;
        }
        prev_ = trigger;
        out.holding = trigger && state_ != State::Idle;
        return out;
    }

private:
    enum class State : std::uint8_t { Idle, Holding, Armed, ReleasedSlam, WaitRelease };
    void ClearStroke() {
        smash_ = {};
        touched_ = false;
        await_ = -1.0f;
        spent_ = false;
    }
    struct Smash {
        float peak{0.0f};   // m/s down at the stroke's fastest
        float travel{0.0f}; // m down since it began
        float since{-1.0f}; // s since the stroke first went fast enough (-1: none)
        float early_touch_s{}; // brief contact on the approach, before reaching the speed threshold
    };
    bool Smashed(float vy, float dt, bool contact) {
        // (A blow that stopped: it counts once it touched something, until ContactGraceS on.)
        if (await_ >= 0.0f) {
            await_ += dt;
            if (await_ <= ContactGraceS && contact) {
                await_ = -1.0f;
                return true;
            }
            if (await_ > ContactGraceS) {
                await_ = -1.0f; // it hit nothing: no blast (still armed)
            }
            return false;
        }
        const float down = -vy;
        if (spent_) {
            // (A long sweep down was turned away: a new blow only once the hand has slowed.)
            spent_ = down >= SmashDownMps * 0.5f;
            return false;
        }
        if (down >= SmashDownMps * 0.5f) {
            smash_.travel += down * dt;
        }
        if (smash_.since < 0.0f) {
            if (down < SmashDownMps) {
                if (down < SmashDownMps * 0.5f) {
                    ClearStroke();
                } else {
                    // A slow approach may lead into a slam, but cannot bank all its travel.
                    smash_.travel = std::min(smash_.travel, SmashTravelM * 0.33f);
                    smash_.early_touch_s = contact ? 0.05f : std::max(0.0f, smash_.early_touch_s - dt);
                }
                return false;
            }
            smash_.since = 0.0f;
            touched_ = smash_.early_touch_s >= dt && smash_.early_touch_s > 0.0f;
        } else {
            smash_.since += dt;
        }
        if (smash_.since > StopWithinS) {
            ClearStroke();
            spent_ = down >= SmashDownMps * 0.5f;
            return false;
        }
        smash_.peak = std::max(smash_.peak, down);
        touched_ |= contact;
        if (down <= StopShare * smash_.peak) {
            const bool blow = smash_.travel >= SmashTravelM;
            const bool touched = touched_;
            ClearStroke();
            if (blow && !touched) {
                await_ = 0.0f;
                return false;
            }
            return blow;
        }
        return false;
    }

    State state_{State::Idle};
    bool prev_{false};
    float held_{0.0f};
    float pulse_{0.0f};
    float cooldown_{0.0f};
    Smash smash_{};
    bool spent_{false};
    bool touched_{false}; // the stroke touched something
    float await_{-1.0f};  // s since a blow stopped untouched (-1: none)
    float release_left_{}; // bounded follow-through after releasing an already-qualified stroke
};

} // namespace VR::BloodBlast
