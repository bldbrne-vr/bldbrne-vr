// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include "core/vr/arm_ik.h"
#include "core/vr/gun_aim.h"

namespace VR::Gun {

// Third person uses ordinary firing. Keep the saved preference for first person;
// applying this through SetEnabled also clears stale chamber and gesture state.
constexpr bool WristReloadEnabled(bool preference, bool first_person) {
    return preference && first_person;
}

// How a reload attempt (a flick that started from a still wrist) ended: the presenter's log.
enum class ReloadEnd : std::uint8_t {
    Reloaded,
    TooSlow,         // ended (its deadline, or a still wrist again) below the flick's peak speed
    TooSmall,        // fast enough, but it never turned MinExcursion from where it started
    Expired,         // fast and far enough, but not within one flick's time
    OrientationLost, // the controller's orientation was lost (or not a rotation)
    Blocked,         // the gun was not the hand's (an item, the arc, two hands, a recenter)
    Trigger,         // the trigger was pulled during it
    TrackingJump,    // a sample faster than MaxSpeed (a tracking jump)
};

constexpr const char* ReloadEndName(ReloadEnd e) {
    switch (e) {
    case ReloadEnd::Reloaded:
        return "reloaded";
    case ReloadEnd::TooSlow:
        return "too slow";
    case ReloadEnd::TooSmall:
        return "too small";
    case ReloadEnd::Expired:
        return "expired";
    case ReloadEnd::OrientationLost:
        return "orientation lost";
    case ReloadEnd::Blocked:
        return "blocked";
    case ReloadEnd::Trigger:
        return "trigger";
    case ReloadEnd::TrackingJump:
        return "tracking jump";
    }
    return "?";
}

// One attempt, as it ended (WristReload::TakeAttempt).
struct ReloadAttempt {
    ReloadEnd end{ReloadEnd::TooSlow};
    bool near_body{false}; // the near-body thresholds applied (the hand was near at some point)
    float peak{0.0f};      // the wrist's fastest sample, rad/s
    float turn{0.0f};      // the largest net turn from where it started, radians
    float ms{0.0f};        // from the still wrist it started from to its end
    float need{0.0f};      // the peak it needed, rad/s
    // Why it was near (either or both, over the attempt's frames): a position the runtime only
    // inferred, or the tracked hand close to the neck (NearBody's distance, with its hysteresis).
    bool near_inferred{false};
    bool near_close{false};
    float closest{-1.0f}; // the closest tracked distance from the neck during it (m; -1: none)
};

// One frame of NearBody (the hand near the body, and why) for WristReload::Step.
struct NearSample {
    bool near{false};
    bool inferred{false};  // near because the hand's or the head's position was not tracked (also
                           // the tracked frames after it inside NearBody's band)
    float distance{-1.0f}; // the hand's horizontal distance from the neck (m; -1: not tracked)
    constexpr NearSample() = default;
    constexpr NearSample(bool near_body) : near(near_body) {} // the verdict alone
    constexpr NearSample(bool near_body, bool inferred_position, float distance_m)
        : near(near_body), inferred(inferred_position), distance(distance_m) {}
};

// Cache only the reload exemption through a missing/stale status sample. An empty pistol's
// chamber is retained while a stream weapon is selected, but cannot block that stream.
class WristReloadMode {
public:
    void Observe(bool fresh, Family family) {
        if (fresh) continuous = ContinuousFire(family);
    }
    bool UsesChamber() const { return !continuous; }
private:
    bool continuous{false};
};

// Chamber and gesture state. Only confirmed shots empty the chamber.
// A flick reloads (2026-10-16, the tester: no "still first", any strong flick once PostShotNs has
// passed): over the last FlickNs of samples since the shot's PostShotNs, the wrist's fastest
// sample reached FlickSpeed and its net turn from any of those samples to now reached
// MinExcursion. Motion before PostShotNs (the recoil, the wrist settling) never counts, and a
// moving hand needs no pause first. Near the body (Step's `near`: the hand close to the torso, or
// its position only inferred) the peak must be NearFlickSpeed, so a slow turn there (the wrist
// gauge, the hip, a pouch) never reloads while a quick ordinary flick does.
class WristReload {
public:
    void Reset() {
        const bool enabled = enabled_;
        *this = {};
        enabled_ = enabled;
    }
    // Changing modes clears an empty chamber, pending shot, rejected hold and gesture history.
    // Disabled reloads also ignore later shot notifications and requests.
    void SetEnabled(bool enabled) {
        if (enabled_ != enabled) {
            Reset();
            enabled_ = enabled;
        }
    }
    bool Empty() const {
        return empty_;
    }
    bool Pending() const {
        return pending_ != 0;
    }
    bool BlocksHeldTrigger() const {
        return empty_ || rejected_hold_;
    }
    bool RejectPull() {
        if (!Blocked())
            return false;
        rejected_hold_ = true;
        return true;
    }
    bool Blocked() const {
        return empty_ || Pending();
    }
    void Request(std::uint32_t serial, std::int64_t now) {
        if (!enabled_) {
            return;
        }
        pending_ = serial;
        request_ns_ = now;
    }
    void Denied(std::uint32_t serial) {
        if (serial == pending_)
            pending_ = 0;
    }
    void Shot(std::int64_t now) {
        if (!enabled_) {
            return;
        }
        pending_ = 0;
        empty_ = true;
        shot_ns_ = now;
        InvalidateMotion();
    }
    void InvalidateMotion() {
        previous_ns_ = 0;
        count_ = 0;
        burst_ = false;
    }
    // The last attempt that ended (once): a burst of wrist motion while the chamber was empty
    // (from its first sample over QuietSpeed to its reload or its slowing again), and how it ended.
    bool TakeAttempt(ReloadAttempt& out) {
        if (!has_attempt_)
            return false;
        out = attempt_;
        has_attempt_ = false;
        return true;
    }
    static constexpr float Need(bool near_body) {
        return near_body ? NearFlickSpeed : FlickSpeed;
    }
    // valid: gameplay eligibility, a tracked orientation and an unchanged tracking space (only the
    // orientation: the gesture is the wrist's, and a position the runtime only infers near the
    // body must not cancel it); `invalid` says which failed for the attempt log. `near_sample`: the
    // hand near the body this frame (NearBody::Sample; an inferred position counts as near), and
    // why, for the attempt log.
    bool Step(std::int64_t now, Body::Quat q, bool valid, bool trigger, NearSample near_sample = {},
              ReloadEnd invalid = ReloadEnd::Blocked) {
        if (!enabled_) {
            return false;
        }
        if (!trigger)
            rejected_hold_ = false;
        // A game animation can reject vanilla L2 without sending a shot event.
        if (pending_ && now - request_ns_ > 2'000'000'000)
            pending_ = 0;
        const float norm = Body::Dot(q, q);
        const bool rotation = Body::IsFinite(q) && norm >= 0.5f && norm <= 1.5f;
        if (!valid || !rotation || trigger) {
            End(now, trigger ? ReloadEnd::Trigger
                     : !valid ? invalid
                              : ReloadEnd::OrientationLost);
            InvalidateMotion();
            return false;
        }
        q = Body::Normalize(q);
        const auto delta = now - previous_ns_;
        if (!previous_ns_ || delta <= 0 || delta > 100'000'000) {
            End(now, ReloadEnd::Blocked); // no samples for a while (a menu, the screen)
            previous_ = q;
            previous_ns_ = now;
            count_ = 0;
            return false;
        }
        const float speed = Turn(q, previous_) / (static_cast<float>(delta) * 1e-9f);
        const Body::Quat before = previous_;
        const std::int64_t before_ns = previous_ns_;
        previous_ = q;
        previous_ns_ = now;
        if (!empty_ || now - shot_ns_ < PostShotNs) {
            count_ = 0;
            return false;
        }
        // A tracking jump empties the window: nothing before it is part of a flick.
        if (speed > MaxSpeed) {
            End(now, ReloadEnd::TrackingJump);
            count_ = 0;
            return false;
        }
        // The window: the samples of the last FlickNs since PostShotNs (the first one is where
        // the wrist was when the window opened, its speed not counted).
        // A burst (one movement) runs from its first sample over QuietSpeed until it slows again.
        if (speed >= QuietSpeed && !burst_) {
            burst_ = true;
            burst_id_ = burst_id_ % 255 + 1;
            burst_near_[burst_id_] = false;
            attempt_ns_ = before_ns;
            peak_all_ = turn_max_ = 0.0f;
            near_ = near_inferred_ = near_close_ = false;
            closest_ = -1.0f;
        }
        const std::uint8_t id = burst_ ? burst_id_ : 0;
        if (burst_) {
            Note(near_sample);
            peak_all_ = std::max(peak_all_, speed);
            burst_near_[id] = burst_near_[id] || near_sample.near;
        }
        // The window's first sample: the previous one when it is past PostShotNs too; otherwise
        // this one, its speed not counted (its step began before the cutoff: the recoil would
        // leak in).
        const bool straddles = count_ == 0 && before_ns - shot_ns_ < PostShotNs;
        if (count_ == 0 && !straddles)
            Push({before_ns, before, 0.0f, near_sample.near, id});
        Push({now, q, straddles ? 0.0f : speed, near_sample.near, id});
        while (count_ > 1 && now - samples_[first_].ns > FlickNs)
            Drop();
        // Near: a sample near the body in the window, or one of a movement that was near at any
        // moment (a slow turn at the hip that leaves the body before it ends never reloads).
        float peak = 0.0f, turn = 0.0f;
        bool near = false;
        for (std::size_t i = 0; i < count_; ++i) {
            const Sample& s = samples_[(first_ + i) % samples_.size()];
            peak = std::max(peak, s.speed);
            turn = std::max(turn, Turn(q, s.q));
            near = near || s.near || (s.burst != 0 && burst_near_[s.burst]);
        }
        if (burst_)
            turn_max_ = std::max(turn_max_, turn);
        if (peak >= Need(near) && turn >= MinExcursion) {
            End(now, ReloadEnd::Reloaded);
            empty_ = false;
            // The trigger is already released: accept its next press, even next frame.
            InvalidateMotion();
            return true;
        }
        if (burst_ && speed < QuietSpeed)
            End(now, Verdict()); // the burst slowed down without a reload
        return false;
    }
    // A flick counts from PostShotNs after the shot (120 ms with a still wrist first until
    // 2026-10-16, then 200; now 100 ms, no pause needed), 5.5 rad/s and 26 degrees within
    // FlickNs; near the body 6.5 rad/s, the same turn.
    static constexpr std::int64_t PostShotNs = 100'000'000;
    static constexpr std::int64_t FlickNs = 180'000'000;
    static constexpr float QuietSpeed = 1.5f;                // radians/second (the attempt log)
    static constexpr float FlickSpeed = 5.5f;                // peak, radians/second
    static constexpr float NearFlickSpeed = 6.5f;            // ... near the body
    static constexpr float MaxSpeed = 25.0f;                 // faster is a tracking jump
    static constexpr float MinExcursion = 26.0f * Body::Deg; // net wrist turn, near or far

private:
    struct Sample {
        std::int64_t ns{};
        Body::Quat q{};
        float speed{};
        bool near{};
        std::uint8_t burst{}; // the movement it is part of (0: none)
    };
    static float Turn(Body::Quat a, Body::Quat b) {
        return 2.0f * std::acos(std::clamp(std::abs(Body::Dot(a, b)), 0.0f, 1.0f));
    }
    void Push(const Sample& s) {
        if (count_ == samples_.size())
            Drop();
        samples_[(first_ + count_) % samples_.size()] = s;
        ++count_;
    }
    void Drop() {
        first_ = (first_ + 1) % samples_.size();
        --count_;
    }
    // Why a burst that did not reload ended: by its peak and turn.
    ReloadEnd Verdict() const {
        return peak_all_ < Need(near_)       ? ReloadEnd::TooSlow
               : turn_max_ < MinExcursion ? ReloadEnd::TooSmall
                                          : ReloadEnd::Expired;
    }
    // A burst ends: kept for TakeAttempt.
    void End(std::int64_t now, ReloadEnd why) {
        if (!burst_)
            return;
        burst_ = false;
        attempt_ = {why,         near_, peak_all_, turn_max_,
                    static_cast<float>(static_cast<double>(now - attempt_ns_) * 1e-6),
                    Need(near_), near_inferred_, near_close_, closest_};
        has_attempt_ = true;
    }
    // A frame of the burst: near once is near for the whole burst; why, and the closest tracked
    // distance, for the attempt log.
    void Note(const NearSample& s) {
        near_ = near_ || s.near;
        near_inferred_ = near_inferred_ || (s.near && s.inferred);
        near_close_ = near_close_ || (s.near && !s.inferred);
        if (s.distance >= 0.0f && (closest_ < 0.0f || s.distance < closest_))
            closest_ = s.distance;
    }
    std::array<Sample, 64> samples_{};
    std::array<bool, 256> burst_near_{}; // per movement id: near the body at some moment
    std::uint8_t burst_id_{};
    std::size_t first_{}, count_{};
    Body::Quat previous_{};
    bool burst_{}, near_{}, near_inferred_{}, near_close_{};
    float closest_{-1.0f};
    float peak_all_{}, turn_max_{};
    std::int64_t previous_ns_{}, shot_ns_{}, request_ns_{}, attempt_ns_{};
    std::uint32_t pending_{};
    bool enabled_{true};
    bool empty_{}, rejected_hold_{};
    ReloadAttempt attempt_{};
    bool has_attempt_{};
};

// Why an attempt counted as near the body, for the log line ("extended" when it did not).
constexpr const char* NearWhy(const ReloadAttempt& a) {
    if (!a.near_body)
        return "extended";
    if (a.near_inferred && a.near_close)
        return "near the body (close to the neck, and its position inferred)";
    if (a.near_inferred)
        return "near the body (its position inferred)";
    return a.near_close ? "near the body (close to the neck)" : "near the body";
}

// Which attempts the presenter's LogReloadAttempt writes: a reload always (never counted); a
// failed attempt only when it was a try (MinPeak or MinTurn reached) and at most PerS a second,
// the rest counted for the next line (TakeDropped).
class ReloadLogGate {
public:
    bool Admit(const ReloadAttempt& a, std::int64_t now) {
        if (a.end == ReloadEnd::Reloaded)
            return true;
        if (a.peak < MinPeak && a.turn < MinTurn)
            return false;
        if (now - window_ns_ >= 1'000'000'000) {
            window_ns_ = now;
            lines_ = 0;
        }
        if (lines_ >= PerS) {
            ++dropped_;
            return false;
        }
        ++lines_;
        return true;
    }
    // The failed attempts not logged since the last line (then zero).
    std::uint32_t TakeDropped() {
        const std::uint32_t d = dropped_;
        dropped_ = 0;
        return d;
    }
    static constexpr std::uint32_t PerS = 4;
    static constexpr float MinPeak = 2.5f;             // rad/s
    static constexpr float MinTurn = 10.0f * Body::Deg; // radians

private:
    std::int64_t window_ns_{0};
    std::uint32_t lines_{0};
    std::uint32_t dropped_{0};
};

// Is the gun hand near the body rather than extended? The horizontal distance from the hand to the
// neck (7.5 cm under and 8 cm behind the eyes, in the head's own axes, as the body zones' pivot),
// by the player's height (k): near under NearM k, far again over FarM k (hysteresis). A hand or
// head position that is not tracked (only inferred: a controller held low or against the torso)
// counts as near. An arm reaching out to aim is about 0.5-0.6 m from the neck; a hand at the
// hip, the chest or the belly 0.2-0.35 m.
class NearBody {
public:
    bool Update(bool hand_tracked, Body::Vec3 hand, bool head_valid, Body::Vec3 head,
                Body::Quat head_q, float k) {
        if (!hand_tracked || !head_valid || !Body::IsFinite(hand) || !Body::IsFinite(head) ||
            !Body::IsFinite(head_q) || Body::Dot(head_q, head_q) < 0.5f) {
            near_ = inferred_ = true;
            distance_ = -1.0f;
            return near_;
        }
        k = std::isfinite(k) ? std::clamp(k, 0.6f, 1.5f) : 1.0f;
        const Body::Vec3 neck =
            head + Body::Rotate(Body::Normalize(head_q), Body::Vec3{0.0f, -0.075f, 0.08f} * k);
        const float dx = hand.x - neck.x;
        const float dz = hand.z - neck.z;
        distance_ = std::sqrt(dx * dx + dz * dz);
        // Inside the band (NearM k .. FarM k) the verdict is carried over, and so is its cause: a
        // near that an untracked frame set stays "inferred" until a tracked frame is really close
        // (then near by distance) or far.
        if (distance_ < NearM * k) {
            near_ = true;
            inferred_ = false;
        } else if (distance_ > FarM * k) {
            near_ = false;
            inferred_ = false;
        }
        return near_;
    }
    [[nodiscard]] bool Near() const {
        return near_;
    }
    // The last horizontal distance from the neck (m; -1: not tracked).
    [[nodiscard]] float Distance() const {
        return distance_;
    }
    // The last Update as WristReload::Step takes it: near, why, the distance.
    [[nodiscard]] NearSample Sample() const {
        return {near_, inferred_, distance_};
    }
    static constexpr float NearM = 0.38f;
    static constexpr float FarM = 0.42f;

private:
    bool near_{false};
    bool inferred_{false}; // near because a position was not tracked (kept through the band)
    float distance_{-1.0f};
};

} // namespace VR::Gun
