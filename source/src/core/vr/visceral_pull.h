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
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>

// Freehand mode (visceral_freehand): tracked hands from entry to recovery. You stab, the enemy
// takes it (its entry plays), and just before the rip the world holds still: the hunter, his
// victim and every other character (Out::world) until you pull your hand back out, as late as you
// like (FreehandTimeoutS is only a safety), no damage penalty. Your pull counts from the catch on
// (as legacy: the recoil of the thrust never does); then the rip, and the enemy is thrown back.
// (Until 2026-10-14 the pair caught at the entry and the thrust's own recoil was the pull.)
// Legacy interactive visceral: a faster entry, then the catch just before the rip: the throw all but
// stops with the enemy's heart beating in your fist (faster the longer you wait). From there your
// hand drives it: drawing it back out scrubs the throw toward the rip (the tension rising in your
// hand, the heart failing), and the yank that completes the pull (12 cm) tears it out: the game's
// own rip, a brief slow-motion beat on the blood (impact_s), then its original extraction and
// enemy reaction. Only the pull from the catch on counts (the recoil of the thrust never does).
// No pull before the timeout (plus 200 ms of grace, by the XR detection time) still rips, at 80%
// damage. Both partners share the speed. Menu, damage to the hunter and lost partners release the
// speed override immediately.
//
// The game teaches each clip's rip time on its first throw (full damage, no timing judgment).
// Later throws catch lead_s before that time. Learned times persist in vr.ini.
//
// Pure (no emulator dependencies; tests/test_visceral_pull.cpp).
namespace VR::Melee::Pull {

constexpr std::int64_t Ms = 1'000'000;
constexpr float RipMarginS = 0.03f;         // a held clip stays this far short of the rip
constexpr float LateGraceS = 0.20f;         // extra time past the nominal deadline
constexpr float MistimedDamageScale = 0.8f; // keep a mistimed visceral useful
constexpr float MinRate = 0.002f;           // nominal creep; the held clip budget may require less
constexpr float MaxTearRate = 2.0f;         // the hand scrubs the held throw at most this fast
constexpr float FreehandRate = 4.0f;       // bounded catch-up; never seek past animation events
constexpr float ImpactRate = 0.2f;          // the rip's slow-motion beat
constexpr float FreehandTimeoutS = 20.0f;   // freehand's catch: a safety, not a deadline
// While a visceral is open its staggered target runs this much slower (unseen): the gun parry's
// 3.6 s stagger, and the visceral window with it, lasts about 0.40 s longer (2026-10-14:
// another 250 ms on top of the existing 150 ms). Only the enemy takes this factor.
constexpr float StaggerStretchRate = 3.6f / 4.0f;

struct Config {
    bool enabled{true};
    bool freehand{false}; // legacy model by default; runtime visceral_freehand selects the new path
    float rate{0.04f};       // the throw's speed while it waits for the pull
    float lead_s{0.10f};     // waits this long (clip time) before the learned rip
    float at_s{0.0f};        // a fixed hold point in every throw clip (s); 0 = the learned rip
    float timeout_s{3.0f};   // waits at most this long (real time), then softens the rip (plus LateGraceS)
    float min_clip_s{0.15f}; // never holds earlier in the clip than this
    float catch_s{0.10f};    // the catch lasts at least this long before the yank tears
    float entry_rate{1.15f}; // a known throw's entry (both partners)
    float rip_rate{1.4f};    // its original extraction, without skipping animation events
    float release_s{0.06f};  // ease from the catch (or the rip's beat) to extraction speed
    float recover_s{0.25f};  // clip seconds after damage before the arms blend back
    float impact_s{0.15f};   // the rip's beat at ImpactRate (real time, from the damage; 0: none)
};

// ---- The hold (the main player's frame time, once per update) ----------------------------------

struct In {
    std::int64_t now{0};
    bool throwing{false};       // the hunter throws (ThrowState odd: a visceral, riposte, backstab)
    std::int32_t clip{-1};      // his newest clip (TimeAct)
    float clip_time{0.0f};      // its time (s)
    float progress{0.0f};       // the hand's pull since the catch (0 .. 1: pull_cm), yank or not
    bool pulled{false};         // the yank that completes it
    std::int64_t pulled_ns{0};  // actual XR detection time, not the game acknowledgement
    bool ripped{false};         // this throw already dealt its first verified damage
    float dt{1.0f / 30.0f};     // upcoming game update, before scaling
    const char* abort{nullptr}; // let go at once (a menu, a hit, no partner; a static string)
};

enum class Phase : std::uint8_t { Idle, Playing, Held, Released };

struct Out {
    float rate{1.0f};         // the frame time x this (the hunter and his target)
    float world{1.0f};        // ... every other character's (freehand's catch: MinRate)
    bool held_now{false};     // the hold began this update
    bool released_now{false}; // ... ended this update
    bool impact_now{false};   // the rip's slow-motion beat began this update
    const char* why{""};      // how it ended (a static string)
};

class Hold {
public:
    Phase State() const {
        return phase;
    }
    std::uint32_t Serial() const {
        return serial;
    }
    std::int64_t HeldNs() const {
        return held_ns;
    }
    std::int32_t Clip() const {
        return clip;
    }

    bool Failed() const {
        return failed;
    }

    // Keep serials monotonic across loads and character changes (old pulls stay old).
    void Cancel() {
        phase = Phase::Idle;
        pull_ns = 0;
        impact_ns = 0;
        failed = false;
        accelerated = false;
    }

    // Cache each clip's timing when it first appears. Learning during this throw only affects
    // the next throw.
    Out Step(const In& in, float at_s, const Config& c) {
        Out out{};
        if (!c.enabled || !in.throwing) {
            out.released_now = phase == Phase::Held;
            out.why = !c.enabled ? "turned off" : "the throw ended";
            Cancel();
            return out;
        }
        if (phase == Phase::Idle) {
            phase = Phase::Playing;
            ++serial;
            held_ns = 0;
            clip = -1;
            pull_ns = 0;
            impact_ns = 0;
            failed = false;
            accelerated = false;
        }
        // Only a pull from the catch on counts (the thrust's own recoil never does).
        if (phase == Phase::Held && in.pulled && pull_ns == 0) {
            const std::int64_t at = in.pulled_ns > 0 ? std::min(in.pulled_ns, in.now) : in.now;
            pull_ns = at >= held_ns ? at : 0;
        }
        if (in.abort != nullptr || !std::isfinite(in.clip_time) || in.clip < 0 ||
            (phase != Phase::Playing && in.clip != clip)) {
            out.released_now = phase == Phase::Held;
            out.why = in.abort != nullptr ? in.abort : "the clip changed";
            phase = Phase::Released;
            accelerated = false;
            return out;
        }
        if (phase == Phase::Playing && clip != in.clip) {
            clip = in.clip;
            rip_s = c.at_s > 0.0f ? c.at_s + c.lead_s : at_s;
        }
        if (in.ripped && phase != Phase::Released) {
            // A learning throw or a late damage update must never catch after the rip.
            out = Release(in, c, "the rip", 1.0f);
        }
        if (in.ripped && accelerated && impact_ns == 0) {
            impact_ns = in.now; // the rip landed: its beat (ReleaseRate)
            out.impact_now = c.impact_s > 0.0f;
        }
        switch (phase) {
        case Phase::Playing: {
            if (!(rip_s > 0.0f)) {
                return out; // learn this clip at normal speed, no hold
            }
            const float hold_at = std::max(rip_s - c.lead_s, c.min_clip_s);
            if (in.clip_time >= rip_s - RipMarginS) {
                phase = Phase::Released; // too late to catch this throw
                return out;
            }
            if (in.clip_time < hold_at) {
                // Do not step past the catch during a long update.
                out.rate =
                    std::min(c.entry_rate,
                             std::max((hold_at - in.clip_time) / std::max(in.dt, 0.001f), MinRate));
                return out;
            }
            phase = Phase::Held;
            held_ns = in.now;
            catch_clip = in.clip_time;
            out.held_now = true;
            out.rate = HeldRate(in, c);
            out.world = WorldRate(c);
            return out;
        }
        case Phase::Held: {
            const float age = static_cast<float>(in.now - held_ns) * 1e-9f;
            const float rate = std::max(HeldRate(in, c), TearRate(in, c));
            if (pull_ns != 0) {
                if (static_cast<float>(pull_ns - held_ns) * 1e-9f > TimeoutS(c) + LateGraceS) {
                    failed = !c.freehand;
                    return Release(in, c, c.freehand ? "timeout" : "too late", rate);
                }
                if (c.freehand || age >= c.catch_s) {
                    return Release(in, c, "pulled", rate);
                }
            }
            if (age >= TimeoutS(c) + LateGraceS) {
                failed = !c.freehand;
                return Release(in, c, c.freehand ? "timeout" : "too late", rate);
            }
            out.rate = rate;
            out.world = WorldRate(c);
            return out;
        }
        case Phase::Released:
            out.rate = ExtractionRate(in, c);
            return out;
        case Phase::Idle:
            return out;
        }
        return out;
    }

private:
    static float TimeoutS(const Config& c) {
        return c.freehand ? FreehandTimeoutS : c.timeout_s;
    }
    // Freehand's catch holds the world still too (never a hard stop: MinRate); legacy's does not.
    static float WorldRate(const Config& c) {
        return c.freehand ? MinRate : 1.0f;
    }

    Out Release(const In& in, const Config& c, const char* why, float from) {
        const bool was_held = phase == Phase::Held;
        phase = Phase::Released;
        accelerated = true;
        release_ns = in.now;
        release_from = from;
        Out out{};
        out.rate = ExtractionRate(in, c);
        out.released_now = was_held;
        out.why = why;
        return out;
    }

    float ExtractionRate(const In& in, const Config& c) const {
        // Catch up through the game's update, preserving event order and the victim's timing.
        // No position/animation-time writes and no damage fabricated by the gesture detector.
        if (c.freehand && accelerated && !in.ripped && rip_s > in.clip_time) {
            return FreehandRate;
        }
        return ReleaseRate(in.now, c);
    }

    // Eases to the extraction's speed from the release, or from the rip's beat: the hunter and
    // his target all but still for impact_s while its blood flies.
    float ReleaseRate(std::int64_t now, const Config& c) const {
        if (!accelerated) {
            return 1.0f;
        }
        float from = release_from;
        std::int64_t since = release_ns;
        if (impact_ns != 0 && c.impact_s > 0.0f) {
            const auto beat = static_cast<std::int64_t>(static_cast<double>(c.impact_s) * 1e9);
            if (now - impact_ns < beat) {
                return ImpactRate;
            }
            from = ImpactRate;
            since = impact_ns + beat;
        }
        const float t =
            c.release_s > 0.0f
                ? std::clamp(static_cast<float>(now - since) * 1e-9f / c.release_s, 0.0f, 1.0f)
                : 1.0f;
        return from + (c.rip_rate - from) * t * t * (3.0f - 2.0f * t);
    }

    float WaitLeft(const In& in, const Config& c) const {
        return std::max(TimeoutS(c) + LateGraceS -
                            static_cast<float>(static_cast<double>(in.now - held_ns) * 1e-9),
                        0.0f);
    }

    float HeldRate(const In& in, const Config& c) const {
        const float room = rip_s - RipMarginS - in.clip_time;
        // Budget the remaining clip time over the entire wait, including this update.
        // A minimum speed must never override that budget near the damage event.
        const float wait = WaitLeft(in, c) + std::max(in.dt, 0.001f);
        return std::clamp(room / wait, 0.0f, std::max(c.rate, MinRate));
    }

    // Your pull scrubs the held throw from the catch toward the rip (RipMarginS short of it): the
    // speed that brings the clip to the pull's point by the next update. Never back (a hand going
    // back in leaves the clip where it is: HeldRate's creep), never past the margin.
    float TearRate(const In& in, const Config& c) const {
        const float p = std::isfinite(in.progress) ? std::clamp(in.progress, 0.0f, 1.0f) : 0.0f;
        // Leave room for the tiny positive timestep until release. Without this reserve,
        // a nearly complete pull spends the margin, then the minimum creep reaches damage.
        const float reserve = MinRate * (WaitLeft(in, c) + std::max(in.dt, 0.001f));
        const float target =
            catch_clip + p * std::max(rip_s - RipMarginS - reserve - catch_clip, 0.0f);
        return std::clamp((target - in.clip_time) / std::max(in.dt, 0.001f), 0.0f,
                          c.freehand ? FreehandRate : MaxTearRate);
    }

    Phase phase{Phase::Idle};
    std::uint32_t serial{0};
    std::int64_t held_ns{0};
    std::int64_t release_ns{0};
    std::int64_t impact_ns{0}; // the update that saw the rip's damage (0: not yet)
    std::int32_t clip{-1};
    float rip_s{0.0f};
    float catch_clip{0.0f}; // the clip time the catch began at
    float release_from{1.0f};
    std::int64_t pull_ns{0};
    bool failed{false};
    bool accelerated{false};
};

// Faster, stronger heartbeats as the catch lasts, with a bounded rate.
inline float HeartbeatInterval(float held_s) {
    return 0.75f - 0.51f * std::clamp(held_s / 1.5f, 0.0f, 1.0f);
}

// The enemy's heart in your fist (the right hand) during the catch: racing the longer you hold
// it, failing as you tear it out (`progress`: slower and fainter, all but stopped at the end).
struct Beat {
    float interval_s; // until the next one
    float strength;   // a pattern amplitude (0 .. 1, before haptic_floor)
};
inline Beat HeartbeatAt(float held_s, float progress) {
    const float p = std::isfinite(progress) ? std::clamp(progress, 0.0f, 1.0f) : 0.0f;
    const float racing = std::clamp(held_s / 1.5f, 0.0f, 1.0f);
    return {HeartbeatInterval(held_s) * (1.0f + 2.0f * p),
            (0.45f + 0.20f * racing) * (1.0f - 0.7f * p)};
}

// The blade lodged in him from the catch until you pull (the same continuous level, a little
// above where a Touch motor shows it): the hand's "now pull". TearTension rises from it.
inline constexpr float LodgedTension = 0.14f;

// The tear's tension in that hand while you pull (a continuous level; not raised by
// haptic_floor, so it starts where a Touch motor shows it): rising to 0.55 at the full pull.
inline float TearTension(float progress) {
    const float p = std::isfinite(progress) ? std::clamp(progress, 0.0f, 1.0f) : 0.0f;
    return p > 0.02f ? 0.12f + 0.43f * p * p : 0.0f;
}

inline bool RecoveryReady(bool ripped, std::int32_t clip, std::int32_t damage_clip, float time,
                          float damage_time, const Config& c) {
    return ripped && clip >= 0 && clip == damage_clip && std::isfinite(time) &&
           time >= damage_time + c.recover_s;
}

// ---- The rip's clip time per throw clip (taught by the game) -----------------------------------

class Learned {
public:
    static constexpr std::size_t Max = 16;

    float At(std::int32_t clip) const {
        for (std::size_t i = 0; i < count; ++i) {
            if (clips[i] == clip) {
                return times[i];
            }
        }
        return 0.0f;
    }

    // The first damage of a throw at `t` into `clip`. True when it was new (save it).
    bool Note(std::int32_t clip, float t) {
        if (clip < 0 || !(t > 0.05f) || t > 20.0f || At(clip) > 0.0f) {
            return false;
        }
        if (count == Max) {
            std::move(clips.begin() + 1, clips.end(), clips.begin());
            std::move(times.begin() + 1, times.end(), times.begin());
            --count;
        }
        clips[count] = clip;
        times[count] = t;
        ++count;
        return true;
    }

    // "100033000:1.120,100033100:0.953"
    std::string Text() const {
        std::string out;
        char item[48];
        for (std::size_t i = 0; i < count; ++i) {
            std::snprintf(item, sizeof(item), "%s%d:%.3f", out.empty() ? "" : ",", clips[i],
                          static_cast<double>(times[i]));
            out += item;
        }
        return out;
    }

    // The Text() form; malformed items are skipped.
    void Parse(std::string_view text) {
        count = 0;
        while (!text.empty()) {
            const std::size_t comma = text.find(',');
            const std::string item{text.substr(0, comma)};
            text = comma == std::string_view::npos ? std::string_view{} : text.substr(comma + 1);
            const std::size_t colon = item.find(':');
            if (colon == std::string::npos) {
                continue;
            }
            char* end = nullptr;
            const long clip = std::strtol(item.c_str(), &end, 10);
            if (end != item.c_str() + colon) {
                continue;
            }
            const float t = std::strtof(item.c_str() + colon + 1, &end);
            if (end == item.c_str() + colon + 1) {
                continue;
            }
            Note(static_cast<std::int32_t>(clip), t);
        }
    }

    std::size_t Count() const {
        return count;
    }

private:
    std::array<std::int32_t, Max> clips{};
    std::array<float, Max> times{};
    std::size_t count{0};
};

// ---- The pull (the presenter, every XR frame of the catch) ------------------------------------
//
// OpenXR LOCAL space (+x right, +y up, +z back, metres). Production tear input includes both the
// head-relative hand and its tracking-space position: progress requires withdrawal in both,
// so neither stepping with the hand nor head-only leaning counts. The two-argument Arm and
// three-argument Step retain the head-relative kernel for callers without tracked input.
// The pull's progress is the hand leaving its deepest insertion since
// the catch, in any direction but further forward (into the enemy), up to `min_cm`; it completes
// with a peak speed of `min_speed` on the way: a yank back, up or to the side (a slow drift
// fills the progress, and then waits for the yank).

struct V3 {
    float x{0.0f}, y{0.0f}, z{0.0f};
};
inline V3 Sub(V3 a, V3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline float Dot(V3 a, V3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline float Length(V3 a) {
    return std::sqrt(Dot(a, a));
}

struct PullConfig {
    float min_cm{12.0f};
    float min_speed{0.9f};    // m/s, the hand's peak speed relative to the head
    float forward_max{0.35f}; // the motion's share along the head's forward may be at most this
};

class PullDetector {
public:
    // At the catch: the hand (relative to the head) and the head's forward.
    void Arm(V3 hand_rel, V3 head_forward) {
        Arm(hand_rel, head_forward, hand_rel);
    }
    // Tracked input: require displacement in both tracking space and relative to the head.
    // Head-only leaning and moving the whole body with the hand cannot withdraw the blade.
    void Arm(V3 hand_rel, V3 head_forward, V3 hand_tracking) {
        start = hand_rel;
        tracking_start = hand_tracking;
        const float h =
            std::sqrt(head_forward.x * head_forward.x + head_forward.z * head_forward.z);
        forward = h > 1e-4f ? V3{head_forward.x / h, 0.0f, head_forward.z / h} : V3{0, 0, -1};
        peak = 0.0f;
        travel = 0.0f;
        armed = true;
    }
    void Disarm() {
        armed = false;
    }
    bool Armed() const {
        return armed;
    }
    float Travel() const {
        return travel;
    }
    float Peak() const {
        return peak;
    }
    // How far the pull has come as of the last Step (0 .. 1: min_cm out), yank or not.
    float Progress(const PullConfig& c) const {
        return std::clamp(travel * 100.0f / std::max(c.min_cm, 1.0f), 0.0f, 1.0f);
    }

    // The hand relative to the head and its velocity relative to the head's. True once pulled.
    bool Step(V3 hand_rel, V3 vel_rel, const PullConfig& c) {
        return Step(hand_rel, vel_rel, c, hand_rel);
    }
    bool Step(V3 hand_rel, V3 vel_rel, const PullConfig& c, V3 hand_tracking) {
        if (!armed) {
            return false;
        }
        // Follow the continued insertion; a natural recoil is measured from its deepest point,
        // not from where the thrust detector happened to fire.
        if (Dot(Sub(hand_rel, start), forward) > 0.0f) {
            start = hand_rel;
            tracking_start = hand_tracking;
            peak = 0.0f;
        }
        const V3 d = Sub(hand_rel, start);
        const float relative_travel = Length(d);
        // Only actual controller displacement along the same withdrawal counts. Use the
        // smaller distance for feedback too, so a lean cannot scrub the animation either.
        const V3 tracked = Sub(hand_tracking, tracking_start);
        if (tracked.x == d.x && tracked.y == d.y && tracked.z == d.z) {
            // Preserve the original distance exactly when the head has not moved. In
            // particular, do not round a fibre threshold down through dot(d,d)/length(d).
            travel = relative_travel;
        } else {
            travel = relative_travel > 1e-6f
                         ? std::clamp(Dot(tracked, d) / relative_travel, 0.0f, relative_travel)
                         : 0.0f;
        }
        const float speed = Length(vel_rel);
        // Speed only counts while the hand moves outward (not pushing in further).
        if (speed > 1e-3f && Dot(vel_rel, forward) <= c.forward_max * speed) {
            peak = std::max(peak, speed);
        }
        if (travel * 100.0f < c.min_cm || peak < c.min_speed) {
            return false;
        }
        return Dot(d, forward) <= c.forward_max * relative_travel;
    }

private:
    V3 start{};
    V3 tracking_start{};
    V3 forward{0.0f, 0.0f, -1.0f};
    float peak{0.0f};
    float travel{0.0f};
    bool armed{false};
};

} // namespace VR::Melee::Pull
