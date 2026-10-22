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
#include <cstdint>

#include "core/vr/transform_clips.h"

// The weapon-only transform (vr.ini transform=native): the right grip changes the trick weapon's
// form without the hunter's transform animation. No L1 and no player clip: the weapon model plays
// its own transform clip (the one vanilla plays on it alongside the hunter's a000_19xxx) on our
// clock, and at the clip's SetWeaponStyle time the game's own TAE listener gets a synthetic event
// 32, so the form, the moveset (physical melee's form B rows), the save and the menus change
// exactly as vanilla does. The arm stays on the controller and the hunter keeps walking
// (core/vr/bloodborne_transform.h has the game side).
//
// This header is the pure part, unit-tested in tests/test_transform.cpp:
//  - Choose: the clip a squeeze plays for the right weapon's family and arm style (form A -> its
//    A->B clip, form B -> its B->A clip), or why the game's L1 stays (core/vr/transform_clips.h).
//  - ClipTime: the clip time on our clock (transform_speed; transform_skip_s: the clip's lead-in
//    skipped, by default its measured still part, so the weapon starts moving at the squeeze: in
//    vanilla the hunter's own wind-up fills it). NormalClock adds the retract's quick start
//    (transform_retract_speed: a B->A clip runs faster up to its switch, nothing skipped).
//  - StepCues: the clip's sounds and one-shot effects once each as the time passes them (those in
//    the skipped lead-in at the start), and its held effects every update inside their window.
//  - PullLatch: a squeeze the game cannot start at once (a hit, an item, the transform before
//    still locking) waits up to transform_buffer_ms, then is refused (felt).
//  - Clock / the flick: a squeeze during a swing (or a swing joining a transform before its
//    switch) snaps the weapon into its new form mid-stroke (FlickClock, FlickFrom).
namespace VR::Transform {

// The arm styles of TAE event 32 (PlayerIns equip data +0x08).
constexpr std::int32_t StyleFormA = 1;
constexpr std::int32_t StyleLeftTwoHanded = 2;
constexpr std::int32_t StyleFormBTwoHanded = 3;
constexpr std::int32_t StyleFormB = 5;

constexpr bool IsFormB(std::int32_t style) {
    return style == StyleFormBTwoHanded || style == StyleFormB;
}

// ---- Which clip -------------------------------------------------------------------------------

struct Choice {
    const Clip* clip{nullptr}; // null: the game's L1 (why)
    const char* why{nullptr};
};

// The clip a squeeze plays now for the right weapon `family` (EquipParamWeapon id / 100000) in arm
// style `style`. `untested`: rows not yet seen working may be used (transform_untested).
inline Choice Choose(std::int32_t family, std::int32_t style, bool untested) {
    if (family < 0) {
        return {nullptr, "no right weapon"};
    }
    if (style == StyleLeftTwoHanded) {
        return {nullptr, "the left weapon is held two-handed"};
    }
    if (style != StyleFormA && !IsFormB(style)) {
        return {nullptr, "unknown weapon style"};
    }
    const bool to_b = style == StyleFormA;
    bool known = false;
    for (const Clip& c : Clips) {
        if (c.family != family) {
            continue;
        }
        known = true;
        if (c.to_b != to_b) {
            continue;
        }
        if (c.support == Support::Vanilla) {
            return {nullptr, c.why != nullptr ? c.why : "kept on L1"};
        }
        if (c.style == style || c.length <= 0.0f || c.t_style <= 0.0f || c.t_style >= c.length) {
            return {nullptr, "the clip does not change this form"};
        }
        if (c.support == Support::Untested && !untested) {
            return {nullptr, "not tested yet (transform_untested=0)"};
        }
        return {&c, nullptr};
    }
    return {nullptr, known ? "no clip in this direction" : "not a trick weapon"};
}

// ---- The clock --------------------------------------------------------------------------------

struct Timing {
    float speed{1.0f}; // transform_speed: clip seconds per second
    float skip{-1.0f}; // transform_skip_s: the clip's lead-in skipped at the start (< 0: its still)
    // transform_retract_speed: a B->A clip (the retract) plays its start, up to the form switch,
    // this much faster, the rest at `speed` (nothing skipped; its sounds in order, just sooner).
    // 1 = the game's timing (here; the setting's default is 1.75).
    float retract{1.0f};
};

// The lead-in actually skipped: the clip's measured still part (auto) or the setting, never past
// the form switch (at least 0.1 s of the clip stays before it).
inline float SkipFor(const Clip& c, const Timing& tm) {
    const float want = tm.skip < 0.0f ? c.still : tm.skip;
    return std::clamp(want, 0.0f, std::max(c.t_style - 0.1f, 0.0f));
}

// The clip time `elapsed_s` after the start.
inline float ClipTime(float elapsed_s, const Clip& c, const Timing& tm) {
    const float speed = std::clamp(tm.speed, 0.25f, 4.0f);
    return std::clamp(SkipFor(c, tm) + std::max(elapsed_s, 0.0f) * speed, 0.0f, c.length);
}

// Seconds from the start to the form switch, and to the clip's end.
inline float SecondsToSwitch(const Clip& c, const Timing& tm) {
    return (c.t_style - SkipFor(c, tm)) / std::clamp(tm.speed, 0.25f, 4.0f);
}
inline float SecondsToEnd(const Clip& c, const Timing& tm) {
    return (c.length - SkipFor(c, tm)) / std::clamp(tm.speed, 0.25f, 4.0f);
}

// ---- The run's clock ----------------------------------------------------------------------------

constexpr float MinClipSpeed = 0.25f;
constexpr float MaxClipSpeed = 4.0f;

// Clip time `base_t` at the clock's base instant, then `speed` clip seconds per second. A squeeze
// starts a run on a fresh clock; a flick (below) may re-base it mid-run. The retract's quick
// start (transform_retract_speed): up to clip time `lead_end` (its form switch) the clock runs
// `lead_speed`, from there `speed` (no lead when `lead_speed` is not faster than `speed`).
struct Clock {
    float base_t{0.0f};
    float speed{1.0f};
    float lead_end{0.0f};
    float lead_speed{0.0f};
};

// Seconds the clock spends in its quick lead (0: none).
inline float ClockLeadSeconds(const Clock& k) {
    const float speed = std::clamp(k.speed, MinClipSpeed, MaxClipSpeed);
    const float lead = std::clamp(k.lead_speed, MinClipSpeed, MaxClipSpeed);
    if (lead <= speed || k.lead_end <= k.base_t) {
        return 0.0f;
    }
    return (k.lead_end - k.base_t) / lead;
}

// The clock's pace at clip time `t` (clip seconds per second).
inline float ClockSpeedAt(const Clock& k, float t) {
    if (ClockLeadSeconds(k) > 0.0f && t < k.lead_end) {
        return std::clamp(k.lead_speed, MinClipSpeed, MaxClipSpeed);
    }
    return std::clamp(k.speed, MinClipSpeed, MaxClipSpeed);
}

inline float ClockTime(const Clock& k, float since_base_s, const Clip& c) {
    const float speed = std::clamp(k.speed, MinClipSpeed, MaxClipSpeed);
    const float since = std::max(since_base_s, 0.0f);
    const float lead_s = ClockLeadSeconds(k);
    float t = k.base_t + since * speed;
    if (lead_s > 0.0f) {
        t = since < lead_s
                ? k.base_t + since * std::clamp(k.lead_speed, MinClipSpeed, MaxClipSpeed)
                : k.lead_end + (since - lead_s) * speed;
    }
    return std::clamp(t, 0.0f, c.length);
}

// Seconds from the clock's base to clip time `t` (0 if it is already past).
inline float ClockSecondsTo(const Clock& k, float t, const Clip& c) {
    const float speed = std::clamp(k.speed, MinClipSpeed, MaxClipSpeed);
    const float target = std::min(t, c.length);
    const float lead_s = ClockLeadSeconds(k);
    if (lead_s > 0.0f) {
        if (target <= k.lead_end) {
            return std::max(target - k.base_t, 0.0f) /
                   std::clamp(k.lead_speed, MinClipSpeed, MaxClipSpeed);
        }
        return lead_s + (target - k.lead_end) / speed;
    }
    return std::max(target - k.base_t, 0.0f) / speed;
}

// Whether a plain squeeze's run of `c` gets the retract's quick start: a B->A clip with
// transform_retract_speed above 1 and some clip left before its switch.
inline bool RetractLead(const Clip& c, const Timing& tm) {
    return !c.to_b && tm.retract > 1.0f && c.t_style > SkipFor(c, tm);
}

// A plain squeeze: from the skipped lead-in at transform_speed; a retract (B->A) runs up to its
// form switch transform_retract_speed times faster, then at transform_speed (the whole clip
// plays: the weapon moves and its sounds fire in order, the start just takes less time).
inline Clock NormalClock(const Clip& c, const Timing& tm) {
    Clock k{SkipFor(c, tm), std::clamp(tm.speed, MinClipSpeed, MaxClipSpeed)};
    if (RetractLead(c, tm)) {
        k.lead_end = c.t_style;
        k.lead_speed = std::clamp(k.speed * tm.retract, MinClipSpeed, MaxClipSpeed);
    }
    return k;
}

// ---- The flick ----------------------------------------------------------------------------------
//
// A squeeze during a swing or with the weapon hand moving fast (the presenter's flick flag), or a
// swing that starts while a transform runs before its switch: the weapon snaps into its new form
// mid-stroke. The clip starts at its flick start (Clip::flick: the latest point before the switch
// at which the weapon still sits in its rest pose, so nothing jumps; the Saw Cleaver's B->A unlatch
// wiggle is skipped) and plays at transform_flick_speed, faster still where the switch would come
// later than transform_flick_lead_ms after the squeeze (clips that move from their first frame,
// such as the Hunter Axe's and the Threaded Cane's B->A); the rest of the clip (the lock settling)
// follows at the same speed. Physical melee turns that stroke into the game's transform attack
// after the switch (core/vr/bloodborne_melee.cpp).

struct FlickTiming {
    float speed{2.5f};   // transform_flick_speed: clip seconds per second at least
    float lead_s{0.10f}; // transform_flick_lead_ms: the switch at most this long after the start
};

// Where a flick starts in the clip.
inline float FlickStart(const Clip& c) {
    return std::clamp(std::max(c.flick, c.still), 0.0f, std::max(c.t_style, 0.0f));
}

// The clip speed of a flick going on from clip time `from_t`.
inline float FlickSpeed(const Clip& c, float from_t, const FlickTiming& f) {
    const float to_switch = std::max(c.t_style - from_t, 0.0f);
    const float need = to_switch / std::max(f.lead_s, 0.02f);
    return std::clamp(std::max(f.speed, need), MinClipSpeed, MaxClipSpeed);
}

// A flick squeeze: from the flick start at the flick's pace.
inline Clock FlickClock(const Clip& c, const FlickTiming& f) {
    const float start = FlickStart(c);
    return {start, FlickSpeed(c, start, f)};
}

// A running transform that a swing joined at clip time `t` (before its switch): it goes on from
// there at the flick's pace, never slower than it was going.
inline Clock FlickFrom(const Clip& c, float t, float speed_now, const FlickTiming& f) {
    return {std::clamp(t, 0.0f, c.length),
            std::clamp(std::max(speed_now, FlickSpeed(c, t, f)), MinClipSpeed, MaxClipSpeed)};
}

// ---- Cues -------------------------------------------------------------------------------------

constexpr bool IsHeld(const Cue& q) {
    return q.kind == CueKind::Ffx && q.c >= 0;
}

struct CueStep {
    std::uint32_t fire{0}; // bit i: play cue i now (a sound, a one-shot effect)
    std::uint32_t held{0}; // bit i: ask for held effect i this update (its slot lives while asked)
};

// One update at clip time `t` of a run that started at clip time `start_t`. `fired` keeps which
// one-shot cues are done; those before `start_t` (the skipped lead-in: the mechanism's unlatch
// sound, say) play on the first update.
inline CueStep StepCues(const Clip& c, float start_t, float t, std::uint32_t& fired) {
    CueStep s{};
    const std::uint32_t n = std::min<std::uint32_t>(c.cue_count, 32);
    for (std::uint32_t i = 0; i < n; ++i) {
        const Cue& q = c.cues[i];
        const std::uint32_t bit = 1u << i;
        if (IsHeld(q)) {
            if (q.t0 <= t && t < q.t1 && q.t1 > start_t) {
                s.held |= bit;
            }
            continue;
        }
        if ((fired & bit) == 0 && q.t0 <= std::max(t, start_t)) {
            fired |= bit;
            s.fire |= bit;
        }
    }
    return s;
}

// The chain weapons require one deliberate hold in either form. This runs before both the
// native request and L1 fallback, so neither can leak a tap through. A blocked squeeze or
// changed weapon is spent until release; time in menus/tracking gaps never completes a hold.
constexpr bool GripHoldRequired(std::int32_t family) {
    return family == 220 || family == 240;
}
class GripHold {
public:
    bool Required(std::int32_t family) const {
        // Changing weapon during a pending hold must not expose the direct L1 path.
        return family < 0 || GripHoldRequired(family) ||
               (down_ && (family_ < 0 || GripHoldRequired(family_)));
    }
    bool Step(bool down, std::int32_t family, bool eligible, float dt) {
        if (!down) {
            Reset(false);
            return false;
        }
        if (!down_) {
            down_ = true;
            family_ = family;
            spent_ = !eligible;
            elapsed_ = 0.0f;
            return false; // the press sample contains time from before the press
        }
        if (!eligible || family != family_ || !std::isfinite(dt) || dt < 0 || dt > .1f) {
            spent_ = true;
            accepted_ = false;
        }
        if (spent_)
            return false;
        elapsed_ += dt;
        if (elapsed_ + 1e-6f < .08f)
            return false;
        accepted_ = spent_ = true;
        return true;
    }
    bool Accepted() const {
        return accepted_;
    }
    void Reset(bool down = true) {
        down_ = spent_ = down;
        accepted_ = false;
        elapsed_ = 0;
        family_ = -1;
    }

private:
    bool down_{false}, spent_{false}, accepted_{false};
    float elapsed_{0};
    std::int32_t family_{-1};
};

// ---- The squeeze ------------------------------------------------------------------------------

enum class PullStep : std::uint8_t {
    Idle,  // nothing waiting
    Start, // start the transform now
    Wait,  // a squeeze waits for the gates
    Deny,  // the squeeze waited too long: refused
};

inline const char* PullStepName(PullStep s) {
    switch (s) {
    case PullStep::Idle:
        return "idle";
    case PullStep::Start:
        return "start";
    case PullStep::Wait:
        return "wait";
    case PullStep::Deny:
        return "deny";
    }
    return "?";
}

struct PullLatch {
    std::uint32_t handled{0}; // the newest squeeze serial seen
    std::uint32_t pending{0}; // the squeeze waiting (0: none)
    std::int64_t pending_ns{0};
};

struct PullInput {
    bool have{false};         // a squeeze was published
    std::uint32_t serial{0};  // its serial
    std::int64_t press_ns{0}; // when it was squeezed (steady clock)
    std::int64_t now{0};
    std::int64_t buffer_ns{400'000'000};
    bool can_start{false}; // every gate passes now
};

// One game update. A new squeeze replaces one still waiting; one older than the buffer when first
// seen (a stale publication) is refused at once.
inline PullStep StepPull(PullLatch& l, const PullInput& in) {
    if (in.have && in.serial != l.handled) {
        l.handled = in.serial;
        l.pending = in.serial;
        l.pending_ns = in.press_ns != 0 ? in.press_ns : in.now;
    }
    if (l.pending == 0) {
        return PullStep::Idle;
    }
    const bool expired = in.now - l.pending_ns > in.buffer_ns;
    if (in.can_start && !expired) {
        l.pending = 0;
        return PullStep::Start;
    }
    if (expired) {
        l.pending = 0;
        return PullStep::Deny;
    }
    return PullStep::Wait;
}

// A running transform a new squeeze may cut short: only once its form has switched and the lock
// has settled (`settle_s` of clip time after the switch).
inline bool Interruptible(const Clip& c, float t, bool switched, float settle_s = 0.1f) {
    return switched && t >= c.t_style + settle_s;
}

} // namespace VR::Transform
