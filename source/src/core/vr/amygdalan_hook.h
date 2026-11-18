// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include "core/vr/arm_ik.h"
#include "core/vr/corpse_hands.h"
#include "core/vr/seqlock.h"

// Living-enemy hook policy. Enabled by default for every enemy (pulled when it has a ragdoll and
// is light enough, latched otherwise); VR_AMYGDALA_HOOK=0 disables it. Native signature checks
// still gate activation.
namespace VR::AmygdalanHook {
using Body::Vec3;
inline constexpr std::int64_t MaxAgeNs = 100'000'000;
inline constexpr std::uint32_t NoTarget = 0xffffffffu;
// One small, nonlethal puncture on an accepted catch. Never heal or finish an enemy:
// killing and its native reward/death processing remain with ordinary attacks.
inline constexpr std::int32_t CatchDamage = 5;
inline constexpr std::int32_t CatchHp(std::int32_t hp) {
    return hp > 1 ? hp - std::min(CatchDamage, hp - 1) : hp;
}
// The lash's reach (core/vr/amygdalan_arm.h ReachM 3.5 m): the longest rope a catch takes, and
// the most the tether stretches before it breaks (world metres; the adapter removes world scale).
inline constexpr float MaxRopeM = 3.55f;
inline constexpr float MaxTetherM = 3.6f;
// Retail form-B hit3 is the rigid club, between the two handle-bone dummies.
// The outer blade (100 -> 130) and middle section (130 -> 110) only catch while squeezing.
inline bool ClubHit(std::int32_t first, std::int32_t second) {
    return (first == 12110 && second == 12120) || (first == 12120 && second == 12110);
}
// A tip that moves farther than this in one sample (and more than half a metre) jumped (a load, a
// recentre): never a sweep through the world between. The thrown tip runs 40-45 m/s in a gentle
// swing (probed at 30-90 fps), faster in a hard one; at 45 m/s it read as a jump and disarmed
// the catch for the rest of the squeeze (the mock, 2026-11-06: no hook at all).
inline constexpr float TipJumpMps = 120.0f;
// The enemy's ragdoll heavier than this (kg, its bodies' masses at the catch): the tip latches on
// but it neither goes limp nor moves (2026-11-06: "the heaviest of heaviest shouldn't be
// throwable/movable"); lighter ones are pulled by the Strength-capped tether, a heavy one slowly.
inline constexpr float AnchoredKg = 400.0f;
// The yank (2026-11-06: "the enemy shouldn't be ragdolled immediately, but only when
// it gets pulled forward or swung away"): every hit latches, the enemy still on its feet. One
// light enough to move goes limp and is pulled once the hand, slowed under YankArmMps after the
// throw, moves at YankMps or more other than toward it (pulled or swung away), or the tether is
// stretched YankM past its length (a step back). World m and m/s.
// (YankM was 0.3: the hand easing back after a chop stretched it that much within 0.2 s.)
inline constexpr float YankMps = 2.5f, YankArmMps = 1.2f, YankM = 0.6f;
inline constexpr float YankAfterS = 0.15f; // nothing sooner after the latch
inline constexpr float YankCalmS = 0.5f;   // the throw's swing over by then, slowed or not
// The pulled enemy's rope takes up the slack as it comes nearer, down to this (m).
inline constexpr float ReelMinM = 1.0f;
// The enemy's weight before the catch, from its hit capsules' volume (world metres): a living
// ragdoll's bodies still have no mass of their own. The capsules overlap and run wide, so this is
// matched to the game's own ragdolls: the Scourge Beast's (c1000) hold 0.84 m^3 and its ragdoll
// weighs 155 kg; c1170 0.44 m^3, 201 kg. 400 kg is then 2.6 Scourge Beasts' bulk.
inline constexpr float FleshKgPerM3 = 185.0f;

// An enemy too heavy to move, or with no ragdoll: the tip latched on and follows it, the enemy
// itself untouched (the main-player adapter keeps it, core/vr/bloodborne_melee.cpp; the part hook
// aims the tip at `point`, in the game's world units as ArmHookStatus's). Latest value.
struct LatchStatus {
    std::int64_t ns{};
    std::uint64_t player{};
    Vec3 point{};
    bool attached{};
};
inline SeqLock<LatchStatus> latch_status{};
inline LatchStatus LatchNow() {
    LatchStatus s{};
    return latch_status.Load(s) ? s : LatchStatus{};
}

// The Arm part's turn in the world (the hand's), from the part hook (core/vr/bloodborne_whip.cpp)
// every update, for the caught enemy's lever (LeverWay). Latest value.
struct HandTurn {
    std::int64_t ns{};
    std::uint64_t player{};
    Body::Quat q{};
};
inline SeqLock<HandTurn> hand_turn{};
inline HandTurn HandTurnNow() {
    HandTurn s{};
    return hand_turn.Load(s) ? s : HandTurn{};
}

// The caught enemy's swing (2026-11-06: "can you try flinging it around?"): the
// tentacle is a lever from the hand, not a rope. The enemy is pulled toward its tip: the hand plus
// the way it went to the enemy at the yank (`way0`), turned as the hand has turned since (`q0` ->
// `q`), the rope's length out. A quarter turn of the arm drags it a quarter of the way round (with
// a 2.5 m rope the tip moves 4 m), where pulling at the hand moved it the hand's half metre (the
// mock: the limp beast lay still through three swings). Let go mid-swing and it flies on.
inline Vec3 LeverWay(Body::Quat q0, Body::Quat q, Vec3 way0) {
    if (!Body::IsFinite(q0) || !Body::IsFinite(q) || !Body::IsFinite(way0) ||
        Body::LengthSq(way0) < .25f)
        return {};
    return Body::Normalize(Body::Rotate(Body::Normalize(q * Body::Conjugate(q0)), way0));
}
// Pulled toward the lever's tip, the rope slack this much (m) and still pulling this far past it
// (a fast swing runs the tip on ahead of the enemy), with LeverForceScale x the Strength cap up to
// LeverMaxMps (the headset, 2026-11-06: "I wasn't able to make enemies fly, only to drag them near
// me": at 1x, at most 1.4 kN, a 230 kg enemy could not even be lifted, and 7 m/s was a drag).
inline constexpr float LeverSlackM = 0.25f, LeverReachM = 3.5f;
inline constexpr float LeverForceScale = 4.f, LeverMaxMps = 14.f;
// Let go mid-swing: the body thrown on at LaunchShare of the lever tip's speed, up to LaunchMaxMps,
// when the tip went LaunchMinMps or faster (a still hand throws nothing).
inline constexpr float LaunchShare = 0.85f, LaunchMinMps = 3.f, LaunchMaxMps = 14.f;

inline bool EnabledValue(const char* value) {
    return value == nullptr || std::strcmp(value, "1") == 0;
}

inline bool Enabled() {
    static const bool enabled = EnabledValue(std::getenv("VR_AMYGDALA_HOOK"));
    return enabled;
}

// Published only after both native detours and the reset helper pass their
// signature checks. A failed install must not consume the normal heavy trigger.
inline std::atomic<bool> backend_ready{false};
inline bool Active() {
    return Enabled() && backend_ready.load(std::memory_order_acquire);
}

inline bool ProbeEnabled() {
    static const bool enabled = [] {
        const char* value = std::getenv("VR_AMYGDALA_HOOK_PROBE");
        return value && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

// When testing the hook, its squeeze must not also charge a heavy attack.
// Folded Arm, other weapons and the non-physical fallback retain their controls.
constexpr bool OwnsTrigger(bool enabled, bool physical, int variation, bool transformed) {
    return enabled && physical && variation == 2500 && transformed;
}

// The producer and the native enemy update run at different rates. Once any
// input cancels a hold, later samples must not revive it before the enemy gets
// its next update. BeginArmHook creates a new hold after release/re-grab.
struct HoldInput {
    std::int64_t ns{};
    std::uint64_t player{};
    std::uint32_t space{};
    std::uint32_t target{NoTarget};
    Vec3 grip{}, velocity{};
    Vec3 lever{}; // the tentacle's way out from the hand (unit; zero: the plain rope), LeverWay
    float scale{1.f};
    int strength{};
    bool held{}, clear{};
};

inline bool ContinueHold(HoldInput& current, const HoldInput& next, std::int64_t now) {
    const bool valid = current.held && current.clear && next.held && next.clear &&
                       next.player == current.player && next.space == current.space &&
                       next.target == current.target && next.scale == current.scale &&
                       std::isfinite(next.scale) && next.scale >= .25f && next.scale <= 4.f &&
                       next.ns > 0 && next.ns >= current.ns &&
                       next.ns - current.ns <= MaxAgeNs && now >= next.ns &&
                       now - next.ns <= MaxAgeNs && Body::IsFinite(next.grip) &&
                       Body::IsFinite(next.velocity) && Body::IsFinite(next.lever);
    if (!valid) {
        current.held = false;
        return false;
    }
    current = next;
    return true;
}

struct Input {
    std::int64_t now{}, sample{};
    std::uint64_t player{}; // session identity supplied by the adapter
    std::uint32_t space{};
    int variation{-1};
    bool transformed{}, ready{}, tracked{};
    float trigger{};
    Vec3 tip{}; // world metres (adapter removes world scale)
};

// One catch per squeeze. A load, tracking loss, blocked path or broken tether
// requires release before another catch; holding through a transform cannot grab.
class Catch {
public:
    bool Step(const Input& in) {
        searching_ = false;
        const bool fresh = in.sample > 0 && in.now >= in.sample && in.now - in.sample <= MaxAgeNs;
        const bool valid = fresh && in.player != 0 && in.variation == 2500 && in.transformed &&
                           in.ready && in.tracked && std::isfinite(in.trigger) &&
                           Body::IsFinite(in.tip);
        const bool changed = in.player != player_ || in.space != space_;
        if (!valid || changed) {
            Cancel();
            last_ = 0;
            player_ = in.player;
            space_ = in.space;
            // Even a valid first sample must see an open trigger before arming.
            if (!valid)
                return false;
        }
        if (last_ && (in.sample < last_ || in.sample - last_ > MaxAgeNs)) {
            Cancel();
            last_ = 0;
        }
        if (in.trigger <= .35f) {
            target_ = NoTarget;
            armed_ = true;
            held_ = false;
        }
        if (in.sample == last_)
            return false; // stereo duplicate: no second sweep
        const float dt = last_ ? static_cast<float>(in.sample - last_) * 1e-9f : 0.0f;
        previous_ = last_ ? tip_ : in.tip;
        tip_ = in.tip;
        last_ = in.sample;
        if (Body::Length(tip_ - previous_) > std::max(.5f, TipJumpMps * dt)) {
            // Discard this sweep, never trace through a discontinuity. Before a catch, a fast
            // animated tip must not poison the whole squeeze: resume from this new position on
            // the next continuous sample. An attached enemy still releases and needs a re-grab.
            const bool searching = armed_ && target_ == NoTarget;
            const bool held = held_;
            Cancel();
            armed_ = searching;
            held_ = searching && held;
            return false;
        }
        if (armed_ && in.trigger >= .55f)
            held_ = true;
        searching_ = armed_ && held_ && target_ == NoTarget;
        return searching_;
    }

    bool Contact(std::uint32_t target, bool eligible, bool clear_path) {
        if (!searching_ || !eligible || !clear_path || (target >> 28) != 1)
            return false;
        target_ = target;
        searching_ = false;
        return true;
    }
    void Cancel() {
        target_ = NoTarget;
        armed_ = held_ = searching_ = false;
    }
    std::uint32_t Target() const {
        return target_;
    }
    Vec3 PreviousTip() const {
        return previous_;
    }

private:
    std::uint64_t player_{};
    std::uint32_t space_{}, target_{NoTarget};
    std::int64_t last_{};
    Vec3 previous_{}, tip_{};
    bool armed_{}, held_{}, searching_{};
};

struct Pull {
    Vec3 anchor{}, point{}, anchor_velocity{}, velocity{};
    float length{}; // rope length captured at contact; slack does not push
    float mass{};   // affected body's native mass, kg
    float dt{};
    int strength{};
    bool path_clear{};
    float reach{1.5f};       // m past `length` it still pulls
    float force_scale{1.f};  // x the Strength cap
    float max_speed{7.f};    // m/s: the body no faster
};

// A tension-only spring: no position writes, no infinite-strength hand target.
// Force is capped by Strength and resulting body speed by 7 m/s.
// Gravity, constraints and map collisions remain the game's own.
inline Vec3 PullImpulse(const Pull& p) {
    if (!p.path_clear || !Body::IsFinite(p.anchor) || !Body::IsFinite(p.point) ||
        !Body::IsFinite(p.anchor_velocity) || !Body::IsFinite(p.velocity) ||
        !std::isfinite(p.length) || p.length < 0 || p.length > MaxTetherM ||
        !std::isfinite(p.mass) ||
        p.mass < .01f || p.mass > 10000.f || !std::isfinite(p.dt) || p.dt <= 0 || p.dt > .05f)
        return {};
    const Vec3 delta = p.anchor - p.point;
    const float distance = Body::Length(delta);
    if (!std::isfinite(distance) || distance <= p.length || !std::isfinite(p.reach) ||
        distance > p.length + p.reach)
        return {};
    const Vec3 direction = delta * (1.0f / distance);
    const float relative = Body::Dot(p.velocity - p.anchor_velocity, direction);
    if (!std::isfinite(relative))
        return {};
    const float stiffness = 900.f;
    const float damping = 2.f * std::sqrt(stiffness * p.mass);
    if (!std::isfinite(p.force_scale) || p.force_scale <= 0.f || !std::isfinite(p.max_speed) ||
        p.max_speed <= 0.f)
        return {};
    const float force = std::clamp(stiffness * (distance - p.length) - damping * relative, 0.f,
                                   Corpse::HoldForce(p.strength) * p.force_scale);
    const float speed2 = Body::LengthSq(p.velocity);
    const float MaxSpeed = p.max_speed;
    if (!std::isfinite(speed2) || speed2 >= MaxSpeed * MaxSpeed)
        return {};
    // Largest positive delta-v along the tether that stays inside the speed sphere.
    const float along = Body::Dot(p.velocity, direction);
    const float room = std::max(
        0.f, -along + std::sqrt(std::max(0.f, along * along + MaxSpeed * MaxSpeed - speed2)));
    return direction * std::min(force * p.dt, room * p.mass);
}

// A latched enemy's yank (YankMps), world metres (the adapter removes world scale): `hand` the
// grip's velocity, `to` the latch point from the grip, `rope` the latch's length, `age` seconds
// since it took hold. `settled` carries whether the throw's own swing has slowed since.
struct Yank {
    Vec3 hand{}, to{};
    float rope{}, age{};
    bool movable{};
};
inline bool Yanked(const Yank& y, bool& settled) {
    if (!Body::IsFinite(y.hand) || !Body::IsFinite(y.to) || !std::isfinite(y.rope) ||
        !std::isfinite(y.age))
        return false;
    const float speed = Body::Length(y.hand), stretch = Body::Length(y.to);
    if (speed < YankArmMps)
        settled = true;
    // Never the throw's own swing: a forehand that hooked on its way and swung on stretched the
    // tether 0.3 m within 0.2 s (the mock, 2026-11-06), the enemy limp at once. The hand slowed,
    // or YankCalmS gone (walking back keeps the hand fast), it is over.
    if (!y.movable || y.age < YankAfterS || !(settled || y.age >= YankCalmS))
        return false;
    if (stretch > y.rope + YankM)
        return true; // a step back
    // Pulled or swung away; never the hand coming toward it (within about 45 degrees).
    const bool toward = stretch > 0.f && Body::Dot(y.hand, y.to) > .7f * speed * stretch;
    return speed >= YankMps && !toward;
}
// The impulse (kg m/s) that throws a body of `mass` moving at `body` on along the lever tip's
// velocity `tip` (LaunchShare of it, LaunchMaxMps at most); none along the rest, none when the
// body is already that fast that way or the tip was slow.
inline Vec3 Launch(Vec3 tip, Vec3 body, float mass) {
    const float speed = Body::Length(tip);
    if (!Body::IsFinite(tip) || !Body::IsFinite(body) || !std::isfinite(mass) || mass <= 0.f ||
        mass > 10000.f || !(speed >= LaunchMinMps))
        return {};
    const Vec3 way = tip * (1.f / speed);
    const float want = std::min(speed * LaunchShare, LaunchMaxMps);
    const float add = want - Body::Dot(body, way);
    return add > 0.f ? way * (add * mass) : Vec3{};
}
// The pulled enemy's rope as it comes `nearer`: the slack taken up, never under ReelMinM.
inline float Reel(float length, float nearer) {
    return std::isfinite(nearer) ? std::min(length, std::max(nearer, ReelMinM)) : length;
}
} // namespace VR::AmygdalanHook
