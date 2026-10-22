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
#include <cstddef>

#include "common/types.h"
#include "core/vr/cinematic_state.h"

// Dodge comfort camera (own first person in the follow camera). The eye is the character's physics
// position plus a fixed eye height: every dodge clip has zero vertical root motion, and the head
// bone's dips (0.45-1.4 m in a roll) are never followed, so a roll or a quickstep moves the eye
// only along the dodge. On top of that:
//  - a horizontal critically damped follow (SmoothDamp, `follow_s`) rounds off the 30 Hz steps of
//    the root motion;
//  - the second eye of a stereo pair uses the first eye's position (pair lock), so moving at up to
//    10 m/s never pulls the two eyes apart or together by the 16.7 ms of travel between them.
//    Whether a second eye is locked is decided once: by the VR body at that frame's pose copy,
//    where it places the hands for that eye (LockDecision), else by the camera's own window;
//  - a critically damped vertical filter whose time constant follows the mode: Ground (`ground_s`,
//    times `stop_factor` while the body stands), Dodge (`dodge_s`), Fall (`fall_s`, the slower of
//    `fall_s` and `dodge_s` while a dodge is not landed without a fall clip: stairs, a slope; no
//    rise above the height where the fall was confirmed, held through the landing) and Ladder
//    (`ground_s`). The time constant blends over 100 ms (only a confirmed fall snaps to it), so a
//    release never jerks; no hold, no slew limit. Going up on the ground or a ladder the eye may
//    not trail far below the body (`rise_s`, `rise_lag_m`); going down only a sustained fast
//    drop (a dodge down the stairs, a fall) stiffens the spring (`drop_soft_m`);
//  - teleport detection on every tick (a lamp, a respawn: the camera jumps, it never glides);
//  - a roll-only dip and the jump animation's positive Master lift;
//  - a non-finite target (a bad position read) is never filtered and never output: the last output
//    while it is recent and no reset was asked for, else the caller's fallback (the body's physics
//    position plus the eye height, read apart from the follow camera's pivot) while it is recent,
//    else the newest position known; the next finite target starts over.
// The mode only tunes time constants: with no signal at all every tick is Ground, and the view
// still travels with the body without any animation vertical.
//
// Pure (no emulator state): the game integration (core/vr/bloodborne_motion.cpp) publishes the
// Signals from the main player's update and steps Gate and ComfortCamera in the render-camera
// hook, so unit tests can drive them (tests/test_motion_camera.cpp). Positions are game units
// (`scale` = world_scale per meter; every threshold below is in meters and scaled), times are
// steady-clock nanoseconds.
namespace VR::Motion {

constexpr s64 Millis = 1'000'000; // nanoseconds

struct V3 {
    float x{}, y{}, z{};
};

inline V3 operator+(V3 a, V3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline V3 operator-(V3 a, V3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline float Length(V3 a) {
    return std::sqrt(a.x * a.x + a.y * a.y + a.z * a.z);
}
inline float LengthXZ(V3 a) {
    return std::sqrt(a.x * a.x + a.z * a.z);
}

// ---- Animation clips ---------------------------------------------------------------------------

enum class Kind : u8 {
    None, // not a motion clip (attacks, layered upper-body clips, idle, walking)
    Backstep,
    Quickstep,
    QuickstepOhb, // off-hand bullet quickstep
    Roll,
    RollOhb,
    WakeRoll, // getting up with a roll
    Jump,
    FallStart,
    JumpFall,
    Falling,
    Land,
    Sprint,
    Knockdown,
    Damage,
    Ladder,
};

struct ClipInfo {
    Kind kind{Kind::None};
    s32 dir{-1};         // quickstep: 0 F, 1 B, 2 L, 3 R, 4 FL, 5 FR, 6 BL, 7 BR
    s32 committed_ms{0}; // a dodge counts this long after the clip without SpEffect 392/393
    s32 dip_ms{0};       // the optional dip's length (0 = no dip)
    float dip_weight{0.0f};
    bool ladder_end{false}; // a ladder clip that ends the climb (dismount, slide landing, fall)
};

// The motion family of an animation id (TimeAct entry). Weapon variants are category * 1000000
// + base; categories above 99 (throws 100033xxx, event animations) are never motion clips. Times
// from the TAE (cancel window F11) and the root-motion tables.
constexpr ClipInfo ClassifyMotionClip(s32 id) {
    ClipInfo c{};
    if (id < 0 || id / 1'000'000 > 99) {
        return c;
    }
    const s32 base = id % 1'000'000;
    const auto make = [](Kind kind, s32 committed, s32 dip, float weight, s32 dir = -1) {
        ClipInfo info{};
        info.kind = kind;
        info.dir = dir;
        info.committed_ms = committed;
        info.dip_ms = dip;
        info.dip_weight = weight;
        return info;
    };
    if (base == 7000) {
        return make(Kind::Backstep, 700, 550, 0.5f);
    }
    if (base == 7050) {
        return make(Kind::Backstep, 600, 550, 0.5f);
    }
    if (base == 7300 || base == 7350) {
        return make(Kind::Backstep, 800, 600, 0.5f); // from lying
    }
    if (base >= 7100 && base <= 7107) {
        return make(Kind::Quickstep, 730, 600, 0.7f, base - 7100);
    }
    if (base >= 7150 && base <= 7157) {
        return make(Kind::QuickstepOhb, 630, 500, 0.7f, base - 7150);
    }
    if (base == 7200) {
        return make(Kind::Roll, 870, 700, 1.0f);
    }
    if (base == 7250) {
        return make(Kind::RollOhb, 670, 550, 1.0f);
    }
    if (base >= 7400 && base <= 7457) {
        return make(Kind::WakeRoll, 1000, 900, 1.0f);
    }
    if (base == 7800) {
        return make(Kind::Jump, 1600, 0, 0.0f);
    }
    if (base >= 8000 && base <= 8040) {
        return make(Kind::FallStart, 0, 0, 0.0f);
    }
    if (base == 8050) {
        return make(Kind::JumpFall, 0, 0, 0.0f);
    }
    if (base >= 8100 && base <= 8140) {
        return make(Kind::Falling, 0, 0, 0.0f);
    }
    if (base >= 8200 && base <= 8250) {
        return make(Kind::Land, 300, 0, 0.0f);
    }
    if (base == 5000 || base == 5200 || base == 6700) {
        return make(Kind::Sprint, 0, 0, 0.0f);
    }
    // Knockdowns: the gate's fallback length per family, within the 1.6-3.4 s of their cancel
    // points (approximate; SpEffect 392/393 do not cover them).
    if (base >= 20400 && base <= 20430) {
        return make(Kind::Knockdown, 3400, 0, 0.0f);
    }
    if (base >= 20500 && base <= 20590 && base % 10 == 0) {
        return make(Kind::Knockdown, 2400, 0, 0.0f);
    }
    if (base == 20700) {
        return make(Kind::Knockdown, 1600, 0, 0.0f);
    }
    if (base >= 21000 && base <= 21090 && base % 10 == 0) {
        return make(Kind::Knockdown, 3000, 0, 0.0f);
    }
    if (base >= 20000 && base <= 21300) {
        return make(Kind::Damage, 0, 0, 0.0f);
    }
    if (base >= 101000 && base <= 101075) {
        ClipInfo info = make(Kind::Ladder, 0, 0, 0.0f);
        info.ladder_end = Cinematic::IsLadderEnd(Cinematic::ClassifyLadderClip(base));
        return info;
    }
    return c;
}

// Dodges for the gate: SpEffect 392/393 or, without them, the clip's committed time.
constexpr bool IsDodgeKind(Kind kind) {
    return kind == Kind::Backstep || kind == Kind::Quickstep || kind == Kind::QuickstepOhb ||
           kind == Kind::Roll || kind == Kind::RollOhb || kind == Kind::WakeRoll ||
           kind == Kind::Jump || kind == Kind::Knockdown;
}

constexpr bool IsFallKind(Kind kind) {
    return kind == Kind::FallStart || kind == Kind::JumpFall || kind == Kind::Falling;
}

// A ladder clip that starts or goes on with a climb (getting on, a step up or down): it releases
// the ladder dead latch.
constexpr bool IsClimbClip(s32 id) {
    using Cinematic::LadderClip;
    const LadderClip clip = Cinematic::ClassifyLadderClip(id % 1'000'000);
    return clip == LadderClip::Mount || clip == LadderClip::Up || clip == LadderClip::FastUp ||
           clip == LadderClip::Down;
}

inline const char* KindName(Kind kind) {
    switch (kind) {
    case Kind::None:
        return "none";
    case Kind::Backstep:
        return "backstep";
    case Kind::Quickstep:
        return "quickstep";
    case Kind::QuickstepOhb:
        return "quickstep_ohb";
    case Kind::Roll:
        return "roll";
    case Kind::RollOhb:
        return "roll_ohb";
    case Kind::WakeRoll:
        return "wake_roll";
    case Kind::Jump:
        return "jump";
    case Kind::FallStart:
        return "fall_start";
    case Kind::JumpFall:
        return "jump_fall";
    case Kind::Falling:
        return "falling";
    case Kind::Land:
        return "land";
    case Kind::Sprint:
        return "sprint";
    case Kind::Knockdown:
        return "knockdown";
    case Kind::Damage:
        return "damage";
    case Kind::Ladder:
        return "ladder";
    }
    return "?";
}

inline const char* DirName(s32 dir) {
    constexpr std::array<const char*, 8> names{"F", "B", "L", "R", "FL", "FR", "BL", "BR"};
    return dir >= 0 && dir < 8 ? names[static_cast<size_t>(dir)] : "-";
}

// ---- Signals (published from the main player's update) ---------------------------------------

struct Signals {
    bool valid{false}; // published at least once
    s64 update_ns{0};  // the main-player update that published them
    // SpEffect 392 (quickstep, roll, wake-up roll) / 393 (backsteps): "dodge active".
    bool sp_valid{false};
    bool sp392{false};
    bool sp393{false};
    // The newest motion clip the behaviour steps pushed (ClassifyMotionClip not None). A new push
    // increments the serial; the same id again within 100 ms is a layered duplicate (not counted).
    bool ring_valid{false};
    s32 clip{-1};
    s64 clip_ns{0};
    u32 clip_serial{0};
    // Physics module "landed" byte (HKS env 20).
    bool grounded_valid{false};
    bool grounded{true};
    // HKS ladder action state (-1 off the ladder).
    bool ladder_valid{false};
    s8 ladder{-1};
    // HP (the ladder dead latch).
    bool hp_valid{false};
    s32 hp{1};
    // Logging only.
    bool iframe_valid{false};
    bool iframe{false};
    bool flags_valid{false};
    bool no_fall_anim{false}; // F51
    bool no_gravity{false};   // F27
    bool phys_valid{false};
    V3 phys{}; // physics position (the camera pivot's source)
};

// ---- Gate: the mode for the camera -----------------------------------------------------------

enum class Mode : u8 {
    Ground,
    Dodge,
    Fall,
    Ladder,
};

inline const char* ModeName(Mode mode) {
    switch (mode) {
    case Mode::Ground:
        return "Ground";
    case Mode::Dodge:
        return "Dodge";
    case Mode::Fall:
        return "Fall";
    case Mode::Ladder:
        return "Ladder";
    }
    return "?";
}

struct Phase {
    Mode mode{Mode::Ground};
    bool stale{true}; // no fresh signals (loading, a hitch, no hook): Ground
    // Dodge.
    bool dodging{false};
    bool dodge_start{false}; // rising edge, or a chained dodge restarting it
    bool chained{false};
    bool dodge_end{false};
    const char* end_reason{""}; // "sp off", "timer", "cap", "stale"
    ClipInfo dodge{};           // the dodge's clip (kind None: SpEffect only)
    s32 dodge_clip{-1};
    s64 dodge_ns{0};     // since the dodge (or its chained restart) started
    s64 dodge_len_ns{0}; // dodge_end: how long it lasted
    bool sp{false};      // SpEffect 392/393 present
    s32 sp_id{0};
    // The last motion clip.
    ClipInfo sticky{};
    s32 clip{-1};
    s64 clip_age_ns{-1};
    // Fall.
    bool fall_confirmed{false}; // by a fall clip (FallStart, JumpFall, Falling)
    s64 ungrounded_ns{0};       // how long the landed byte has read 0
    bool grounded{true};
    bool ladder{false};
    s64 ladder_start_ns{0};  // observed mount for the coat fade; 0 = no active mount window
    bool ladder_dead{false}; // the dead latch ignores the ladder state (left over from a death)
};

// Decides the camera mode from the published signals on every camera tick:
//   ladder (state != -1, or a climbing clip until the climb ends) > fall (a fall clip, or not
//   landed for 100 ms) > dodge (SpEffect 392/393, or a dodge clip within its committed time;
//   capped at 3.5 s) > ground.
// Signals older than 250 ms carry no information: Ground. The ladder state has a dead latch (as
// cinematic_state.h's LadderGate): the death states never write it, so after a death on a ladder
// it keeps a climbing value through the respawn. HP <= 0 ends the climb and latches; while latched
// the state is ignored until it changes or a climbing clip arrives while alive. The latch and the
// last state survive stale signals (the respawn's loading screen).
class Gate {
public:
    static constexpr s64 StaleNs = 250 * Millis;
    static constexpr s64 DodgeCapNs = 3500 * Millis;
    static constexpr s64 UngroundedFallNs = 100 * Millis;
    static constexpr s64 FallGroundedNs = 200 * Millis; // landed this long ends a confirmed fall
    static constexpr s64 LadderOffNs = 1000 * Millis;   // state -1 this long ends a climbing clip
    static constexpr s64 AdoptNs = 200 * Millis; // a clip after an SpEffect start names the dodge

    Phase Step(const Signals& s, s64 now) {
        Phase out{};
        const bool fresh = s.valid && now - s.update_ns <= StaleNs && s.update_ns - now <= StaleNs;
        if (!fresh) {
            out.dodge_end = dodging;
            out.end_reason = "stale";
            out.dodge = dodge;
            out.dodge_clip = dodge_clip;
            out.dodge_len_ns = now - dodge_start;
            // Start over (loading, no hook), but never take the last published clip as new, and
            // keep the ladder dead latch with the state it was set on.
            const bool keep_have = have_serial;
            const u32 keep_serial = serial;
            const bool keep_dead = ladder_dead;
            const bool keep_have_ladder = have_ladder;
            const s8 keep_ladder = last_ladder;
            const bool keep_mount_clip = last_clip_was_mount;
            *this = Gate{};
            have_serial = keep_have;
            serial = keep_serial;
            ladder_dead = keep_dead;
            have_ladder = keep_have_ladder;
            last_ladder = keep_ladder;
            last_clip_was_mount = keep_mount_clip;
            out.ladder_dead = ladder_dead;
            return out;
        }
        out.stale = false;

        // The ladder dead latch (the first state read is the baseline, not a change).
        const bool new_clip =
            s.ring_valid && (!have_serial || s.clip_serial != serial) && now - s.clip_ns <= StaleNs;
        const bool state_changed = s.ladder_valid && have_ladder && s.ladder != last_ladder;
        const auto ladder_clip = new_clip && ClassifyMotionClip(s.clip).kind == Kind::Ladder
                                     ? Cinematic::ClassifyLadderClip(s.clip % 1'000'000)
                                     : Cinematic::LadderClip::None;
        // The coat fade starts on a mount, not on a resumed rung after a damage bind, a missing
        // signal, or a loading gap. State/clip observations of the same mount share one timer.
        const bool ladder_mount =
            (s.ladder_valid && Cinematic::IsLadderMountState(s.ladder) &&
             (!have_ladder || !Cinematic::IsLadderMountState(last_ladder))) ||
            (ladder_clip == Cinematic::LadderClip::Mount && !last_clip_was_mount);
        if (s.hp_valid && s.hp <= 0) {
            ladder_dead = true;
            ladder_sticky = false; // death ends the climb
        } else if (ladder_dead && (state_changed || (new_clip && IsClimbClip(s.clip)))) {
            ladder_dead = false;
        }

        // Ladder edges first: a clip pushed in the same update decides after them.
        if (s.ladder_valid) {
            if (s.ladder == -1) {
                if (last_ladder != -1) {
                    ladder_sticky = false; // off the ladder
                }
                if (ladder_off_since < 0) {
                    ladder_off_since = now;
                }
                if (now - ladder_off_since >= LadderOffNs) {
                    ladder_sticky = false;
                }
            } else {
                ladder_off_since = -1;
            }
            last_ladder = s.ladder;
            have_ladder = true;
        }

        // A new motion clip. Clips older than the staleness limit (the last one before a
        // loading screen) only update the serial.
        bool new_dodge_clip = false;
        if (s.ring_valid && (!have_serial || s.clip_serial != serial)) {
            have_serial = true;
            serial = s.clip_serial;
            const ClipInfo info = ClassifyMotionClip(s.clip);
            if (info.kind != Kind::None && now - s.clip_ns <= StaleNs) {
                last_clip_was_mount = ladder_clip == Cinematic::LadderClip::Mount;
                sticky = info;
                sticky_id = s.clip;
                sticky_ns = s.clip_ns;
                ladder_sticky = info.kind == Kind::Ladder && !info.ladder_end;
                if (IsFallKind(info.kind)) {
                    if (!fall_confirmed) {
                        fall_clip_ns = now;
                    }
                    fall_confirmed = true;
                } else {
                    fall_confirmed = false; // a landing or any other motion clip
                }
                new_dodge_clip = IsDodgeKind(info.kind);
            }
        }
        if (ladder_dead) {
            ladder_sticky = false; // no climb while the latch holds
            last_clip_was_mount = false; // death cannot consume the next live mount
        }

        // Landed byte.
        if (s.grounded_valid) {
            if (s.grounded) {
                ungrounded_since = -1;
                if (grounded_since < 0) {
                    grounded_since = now;
                }
            } else {
                grounded_since = -1;
                if (ungrounded_since < 0) {
                    ungrounded_since = now;
                }
            }
        } else {
            grounded_since = ungrounded_since = -1;
        }
        if (fall_confirmed && grounded_since >= 0 &&
            now - std::max(grounded_since, fall_clip_ns) >= FallGroundedNs) {
            fall_confirmed = false;
        }
        const s64 ungrounded = ungrounded_since >= 0 ? now - ungrounded_since : 0;

        // Dodge.
        const bool sp = s.sp_valid && (s.sp392 || s.sp393);
        const bool timer = IsDodgeKind(sticky.kind) &&
                           now - sticky_ns < static_cast<s64>(sticky.committed_ms) * Millis;
        const bool raw = sp || timer;
        if (!raw) {
            capped = false;
        }
        if (raw && !dodging && !capped) {
            dodging = true;
            dodge_start = now;
            out.dodge_start = true;
            const bool named = IsDodgeKind(sticky.kind) && now - sticky_ns <= AdoptNs;
            dodge = named ? sticky : ClipInfo{};
            dodge_clip = named ? sticky_id : -1;
        } else if (dodging && new_dodge_clip) {
            if (dodge.kind == Kind::None && now - dodge_start <= AdoptNs) {
                dodge = sticky; // the SpEffect came first: the clip names this dodge
                dodge_clip = sticky_id;
            } else {
                dodge_start = now; // chained (a roll out of a roll)
                dodge = sticky;
                dodge_clip = sticky_id;
                out.dodge_start = true;
                out.chained = true;
            }
        } else if (dodging && (!raw || now - dodge_start >= DodgeCapNs)) {
            out.dodge_end = true;
            out.end_reason = !raw ? (last_sp && !last_timer   ? "sp off"
                                     : !last_sp && last_timer ? "timer"
                                                              : "sp off+timer")
                                  : "cap";
            out.dodge_len_ns = now - dodge_start;
            out.dodge = dodge;
            out.dodge_clip = dodge_clip;
            capped = raw;
            dodging = false;
        }
        last_sp = sp;
        last_timer = timer;

        out.ladder = (s.ladder_valid && s.ladder != -1 && !ladder_dead) || ladder_sticky;
        out.ladder_dead = ladder_dead;
        if (!out.ladder || Cinematic::IsLadderEnd(ladder_clip)) {
            ladder_start_ns = 0;
        } else if (ladder_start_ns == 0 && ladder_mount) {
            ladder_start_ns = now;
        }
        out.ladder_start_ns = ladder_start_ns;
        const bool fall = fall_confirmed || (s.grounded_valid && ungrounded >= UngroundedFallNs);
        out.mode = out.ladder ? Mode::Ladder
                   : fall     ? Mode::Fall
                   : dodging  ? Mode::Dodge
                              : Mode::Ground;
        out.dodging = dodging;
        if (dodging) {
            out.dodge = dodge;
            out.dodge_clip = dodge_clip;
            out.dodge_ns = now - dodge_start;
        }
        out.sp = sp;
        out.sp_id = s.sp_valid ? (s.sp392 ? 392 : s.sp393 ? 393 : 0) : 0;
        out.sticky = sticky;
        out.clip = sticky_id;
        out.clip_age_ns = sticky_id >= 0 ? now - sticky_ns : -1;
        out.fall_confirmed = fall_confirmed;
        out.ungrounded_ns = ungrounded;
        out.grounded = !s.grounded_valid || s.grounded;
        return out;
    }

    void Reset() {
        *this = Gate{};
    }

private:
    bool have_serial{false};
    u32 serial{0};
    ClipInfo sticky{}; // the last motion clip
    s32 sticky_id{-1};
    s64 sticky_ns{0};
    bool ladder_sticky{false}; // a climbing clip, until the climb ends
    s64 ladder_start_ns{0};
    bool last_clip_was_mount{false}; // survives stale samples: a duplicate mount cannot re-arm
    s8 last_ladder{-1};
    bool have_ladder{false};
    bool ladder_dead{false};
    s64 ladder_off_since{-1};
    bool fall_confirmed{false};
    s64 fall_clip_ns{0};
    s64 ungrounded_since{-1};
    s64 grounded_since{-1};
    bool dodging{false};
    bool capped{false}; // the cap ended a dodge: wait until both signals are off
    s64 dodge_start{0};
    ClipInfo dodge{};
    s32 dodge_clip{-1};
    bool last_sp{false};
    bool last_timer{false};
};

// ---- Comfort camera --------------------------------------------------------------------------

// Critically damped spring toward `target` (Game Programming Gems 4, as Unity's SmoothDamp
// without its clamp): no overshoot for a step, a few mm with a moving target. `t` = time constant
// in seconds; 0 or less follows at once.
inline void SmoothDamp(float& value, float& velocity, float target, float t, float dt) {
    if (t <= 0.0f) {
        value = target;
        velocity = 0.0f;
        return;
    }
    if (dt <= 0.0f) {
        return;
    }
    const float omega = 2.0f / t;
    const float x = omega * dt;
    const float e = 1.0f / (1.0f + x + 0.48f * x * x + 0.235f * x * x * x);
    const float change = value - target;
    const float temp = (velocity + omega * change) * dt;
    velocity = (velocity - omega * temp) * e;
    value = target + (change + temp) * e;
}

struct ComfortConfig {
    float scale{1.0f};      // game units per meter (world_scale)
    float follow_s{0.025f}; // horizontal time constant
    float ground_s{0.120f}; // vertical: Ground and Ladder
    float dodge_s{0.150f};
    float fall_s{0.060f};
    float stop_factor{0.5f};      // Ground while the body stands
    float rise_s{0.025f};         // Ground and Ladder while the target is above (0 = ground_s)
    float rise_lag_m{0.04f};      // ... and the eye at most this far under it (0 = no limit)
    // Going down fast for a while (2026-10-16, the tester: a dodge down the stairs, a fall: the view
    // ended far above the body): while the target's speed down, smoothed over `drop_speed_s` (long
    // enough that single stair steps never read as fast: walking down the stairs or a slope is
    // untouched), is v, the spring going down is at most drop_soft_m / v, so the lag stays near
    // drop_soft_m; never stiffer than `drop_soft_min_s` (stairs taken fast, a dodge down them: no
    // step comes through) nor, in a confirmed fall (in the air: no steps), `fall_soft_min_s`.
    // No hard stop (0 = off).
    float drop_soft_m{0.18f}; // (0.21 until 2026-10-16 evening: the tester, a tiny bit closer)
    float drop_speed_s{0.40f};
    float drop_soft_min_s{0.09f};
    float fall_soft_min_s{0.05f};
    float dip_m{0.0f};            // roll-only dip depth (0 = off)
    float jump_height_scale{1.f}; // real animated lift, 0 = off
    float jump_lift{0.f};         // positive Master lift above reference, game units
    bool pair_lock{true};         // the second eye of a pair uses the first eye's position
    s64 pair_max_ns{25 * Millis}; // a second eye later than this is not locked (1.5 frames)
};

// A stereo pair's second-eye lock as the VR body decided it at that frame's pose copy, before the
// camera tick (core/vr/body_frame.h PairLock; the body places the hands for that eye there). Used
// when it belongs to this pair: decided after the pair's first eye (`first_ns`, the first eye's
// anchor, at or after its tick) and not after this tick. Otherwise (no body, a stale pose copy)
// the camera's own window (pair_max_ns) decides.
struct LockDecision {
    bool valid{false};
    s64 first_ns{0};
    s64 decided_ns{0};
    bool locked{false};
};

// A position the caller knows for a tick apart from its target, and when it was read (the body's
// physics position plus the eye height, from the main player's last update): what a non-finite
// target shows when there is no recent output to keep.
struct Fallback {
    V3 position{};
    s64 ns{0};
};

struct ComfortOut {
    V3 position{};        // the eye base: before the forward offset, the stereo eye and 6DoF
    bool reset{false};    // the camera jumped to the target this tick
    bool rejected{false}; // a non-finite target: nothing filtered (see `fallback`, `stale`)
    bool fallback{false}; // rejected with nothing recent to keep: the caller's fallback position
    bool stale{false};    // rejected and nothing recent known: the newest position known (the
                          // origin only when nothing finite ever was)
    const char* reset_reason{""}; // the caller's reason, "init", "teleport" or "lag"
    float reset_m{0.0f};          // teleport / lag: meters
    bool locked{false};           // second eye: the first eye's position, bit for bit
    bool unlocked{false};         // second eye too late for the pair lock (moves with the body)
    bool decided{false};          // second eye: the lock came from the LockDecision
    Mode mode{Mode::Ground};      // the filter's mode (Fall is held through the landing)
    bool mode_changed{false};
    Mode mode_from{Mode::Ground};
    float fall_lag_m{0.0f}; // leaving Fall: height still to settle (meters)
    float takeoff{0.0f};    // Fall: the camera height at entry, again when it is confirmed
    float t_eff{0.0f};      // seconds: the vertical time constant in effect
    float vy{0.0f};         // game units per second
    float speed{0.0f};      // m/s: horizontal body speed between pairs
    float dip{0.0f};        // signed camera cue in game units
};

class ComfortCamera {
public:
    static constexpr float BlendS = 0.100f;         // time-constant blend (the smooth release)
    static constexpr float StopSpeed = 0.2f;        // m/s
    static constexpr u32 StopPairs = 2;             // ... for this many pairs = standing
    static constexpr float TeleportTickM = 1.5f;    // the target jumped this far in one tick
    static constexpr float TeleportLagM = 3.0f;     // or is this far from the camera
    static constexpr float FallMarginM = 0.02f;     // a confirmed fall stays below take-off + this
    static constexpr float FallExitM = 0.05f;       // Fall is held after landing until within this
    static constexpr s64 FallExitNs = 300 * Millis; // ... or for this long
    static constexpr float RiseDeadM = 0.001f;      // the target this far above = going up
    static constexpr s64 DipStartNs = 150 * Millis; // a dip starts only this early in a dodge
    static constexpr s64 KeepNs = 250 * Millis; // a position this recent is shown for a bad target
    static constexpr float MaxDt = 0.1f;

    // One render-camera tick. `target` = physics position + eye height; `reset` = the caller's
    // reason to jump to the target (nullptr = none); `lock`: a second eye's lock as the VR body
    // decided it (nullptr = none); `fallback`: a position for this tick to show if the target is
    // not finite and there is no recent output to keep (nullptr = none).
    ComfortOut Step(V3 target, bool pair_first, s64 now, const Phase& phase,
                    const ComfortConfig& cfg, const char* reset = nullptr,
                    const LockDecision* lock = nullptr, const Fallback* fallback = nullptr) {
        ComfortOut out{};
        const float s = std::max(cfg.scale, 1e-3f);
        if (!IsFinite(target)) {
            // Never into the filter (NaN would stay in it for good, and the teleport and lag
            // checks would never fire) and never out. In order: the last output while the last
            // finite target is under KeepNs old and no reset was asked for (a reset follows a gap:
            // that output is from before it); the caller's fallback while it is that recent; else
            // the newest position known (`stale`: the fallback or the last finite target, whichever
            // was read last); the origin only when nothing finite ever was. The next finite target
            // starts over.
            out.rejected = true;
            const auto recent = [now](s64 ns) { return now - ns <= KeepNs && ns - now <= KeepNs; };
            const bool have_fallback = fallback != nullptr && IsFinite(fallback->position);
            if (initialized && reset == nullptr && recent(finite_ns)) {
                out.position = cam_out;
            } else if (have_fallback && recent(fallback->ns)) {
                out.position = fallback->position;
                out.fallback = true;
            } else if (have_fallback && (!have_finite || fallback->ns >= finite_ns)) {
                out.position = fallback->position;
                out.fallback = true;
                out.stale = true;
            } else {
                out.position = have_finite ? last_finite : V3{};
                out.stale = !have_finite || !recent(finite_ns);
            }
            restart = initialized;
            Fill(out, mode);
            return out;
        }
        have_finite = true;
        last_finite = target;
        finite_ns = now;
        const char* reason = reset;
        if (reason == nullptr && restart) {
            reason = "non-finite";
        }
        if (reason == nullptr) {
            if (!initialized) {
                reason = "init";
            } else if (const float tick = Length(target - last_target) / s; tick > TeleportTickM) {
                reason = "teleport";
                out.reset_m = tick;
            } else if (const float lag = Length(target - cam) / s; lag > TeleportLagM) {
                reason = "lag";
                out.reset_m = lag;
            }
        }
        last_target = target;
        if (reason != nullptr) {
            const Mode from = mode;
            initialized = true;
            restart = false;
            cam = target;
            vel = {};
            cam_out = target;
            pair_target = target;
            pair_ns = now;
            in_fall = phase.mode == Mode::Fall;
            was_confirmed = in_fall && phase.fall_confirmed;
            takeoff = target.y;
            fall_left_ns = -1;
            mode = phase.mode;
            t_eff = TargetT(mode, false, phase, cfg);
            stopped_pairs = 0;
            t_rise = t_eff;
            rise_room = 0.0f;
            drop_prev_y = target.y;
            drop_speed = 0.0f;
            dip_active = false;
            jump_offset = jump_velocity = 0.f;
            out.reset = true;
            out.reset_reason = reason;
            Fill(out, from);
            out.position = target;
            return out;
        }

        if (!pair_first) {
            // Second eye: the pair's position, unless it comes too late (1.5 game frames) or the
            // lock is off: then it moves with the body as far as the body moved since the first
            // eye. The VR body's decision for this eye, when there is one, decides instead (it
            // placed the hands for it).
            bool locked = cfg.pair_lock && now - pair_ns <= cfg.pair_max_ns;
            if (lock != nullptr && lock->valid && lock->first_ns >= pair_ns &&
                lock->decided_ns > pair_ns && lock->decided_ns <= now) {
                locked = lock->locked;
                out.decided = true;
            }
            if (locked) {
                out.position = cam_out;
                out.locked = true;
            } else {
                out.position = cam_out + (target - pair_target);
                out.unlocked = cfg.pair_lock;
            }
            Fill(out, mode);
            return out;
        }

        const float dt = std::clamp(static_cast<float>(now - pair_ns) * 1e-9f, 0.0f, MaxDt);
        pair_ns = now;
        SmoothDamp(cam.x, vel.x, target.x, cfg.follow_s, dt);
        SmoothDamp(cam.z, vel.z, target.z, cfg.follow_s, dt);
        if (dt > 0.0f) {
            speed = LengthXZ(target - pair_target) / dt / s;
            stopped_pairs = speed < StopSpeed ? std::min(stopped_pairs + 1, 1000u) : 0;
        }
        pair_target = target;

        // Fall: entered at once (a confirmed fall's time constant too), held after the gate leaves
        // it until the landing has settled. The no-rise clamp starts where the fall is confirmed: a
        // jump is "not landed" (an unconfirmed Fall) while it still rises toward its apex.
        const Mode from = mode;
        bool entering_fall = false;
        float fall_lag = 0.0f;
        if (phase.mode == Mode::Fall) {
            if (!in_fall) {
                in_fall = true;
                entering_fall = true;
                takeoff = cam.y;
            }
            if (phase.fall_confirmed && !was_confirmed) {
                takeoff = cam.y;
            }
            fall_left_ns = -1;
        } else if (in_fall) {
            if (fall_left_ns < 0) {
                fall_left_ns = now;
            }
            const float lag = std::abs(cam.y - target.y) / s;
            if (phase.mode == Mode::Ladder || lag < FallExitM || now - fall_left_ns >= FallExitNs) {
                in_fall = false;
                fall_lag = lag;
            }
        }
        was_confirmed = in_fall && phase.fall_confirmed;
        mode = in_fall ? Mode::Fall : phase.mode;
        const float t_target = TargetT(mode, stopped_pairs >= StopPairs, phase, cfg);
        if (entering_fall && phase.fall_confirmed) {
            t_eff = t_target;
        } else {
            t_eff += (t_target - t_eff) * (1.0f - std::exp(-dt / BlendS));
        }
        // Only a confirmed fall clamps: a false "not landed" (stairs, a ladder) never freezes it.
        float target_y = target.y;
        if (in_fall && phase.fall_confirmed) {
            target_y = std::min(target_y, takeoff + FallMarginM * s);
        }
        // Going up on the ground or a ladder (stairs, a slope, a climb) the eye must not trail
        // below the hunter: his body is drawn at his own height (body_height_from_head), so an
        // eye 10-25 cm under it (1-2 m/s up x 120 ms) looked out through his collar and chest.
        // While the target is above, the spring is quicker (`rise_s`, the horizontal follow's
        // 25 ms; blended in over BlendS like every other time constant) and the eye never falls
        // further than `rise_lag_m` under it. A larger lag carried in (a dodge up the stairs, a
        // landing on a higher floor) is never cut: it may only shrink, and the quicker spring
        // takes it down. Going down the eye trails above him, where his head is hidden: the same
        // filter as always (under RiseDeadM it is float noise of a settled spring), but for a
        // sustained fast drop (`drop_soft_m`, below).
        // Dodges and falls (rolls, jumps, landings) keep their own springs and no limit, bit for
        // bit, and so does Ground once the landed byte drops (a take-off, the 100 ms before the
        // gate calls it a fall); after one the quicker spring blends in from theirs. A ladder
        // reads "not landed": it keeps the rules.
        const bool rise_mode = mode == Mode::Ladder || (mode == Mode::Ground && phase.grounded);
        if (rise_mode && cfg.rise_s > 0.0f) {
            const float t_up = std::min(t_eff, cfg.rise_s);
            t_rise = std::min(t_rise + (t_up - t_rise) * (1.0f - std::exp(-dt / BlendS)), t_eff);
        } else {
            t_rise = t_eff;
        }
        // Going down fast for a while (`drop_soft_m`): the spring at most drop_soft_m / v, v the
        // target's speed down smoothed over `drop_speed_s` (a dodge down the stairs ended 63 cm
        // above the body, a 12 m fall 64 cm; with this 51 and 54). Stair steps one by one never
        // reach it (walking down the stairs is the same filter bit for bit, running within 6 mm,
        // as smooth), and it is never stiffer than 90 ms on the ground (a sprint down them: 25 ->
        // 28 m/s^2 at most); in the air 50 ms (a 12 m landing 150 -> 133 m/s^2, a 3 m one 68 ->
        // 76). Going up is untouched (the rise rules above).
        {
            const float v_down = dt > 0.0f ? std::max((drop_prev_y - target.y) / s / dt, 0.0f)
                                           : 0.0f;
            drop_prev_y = target.y;
            if (cfg.drop_speed_s > 0.0f) {
                drop_speed += (v_down - drop_speed) * (1.0f - std::exp(-dt / cfg.drop_speed_s));
            }
        }
        float t_down = t_eff;
        if (cfg.drop_soft_m > 0.0f && drop_speed > 1e-3f) {
            const float floor_s =
                in_fall && phase.fall_confirmed ? cfg.fall_soft_min_s : cfg.drop_soft_min_s;
            t_down = std::min(t_eff, std::max(cfg.drop_soft_m / drop_speed, floor_s));
        }
        SmoothDamp(cam.y, vel.y, target_y,
                   target_y - cam.y > RiseDeadM * s   ? t_rise
                   : cam.y - target_y > RiseDeadM * s ? t_down
                                                      : t_eff,
                   dt);
        if (rise_mode && cfg.rise_lag_m > 0.0f) {
            const float room = std::max(cfg.rise_lag_m * s, rise_room);
            if (target_y - cam.y > room) {
                cam.y = target_y - room;
            }
        }
        rise_room = std::max(target_y - cam.y, 0.0f);

        // A roll-only downward pulse. Sidesteps and backsteps do
        // not move the eye vertically. Latch amplitude/duration for both eyes and run
        // to zero even if the clip hands control back early.
        float dip = 0.0f;
        if (cfg.dip_m > 0.0f) {
            const bool roll = phase.dodge.kind == Kind::Roll || phase.dodge.kind == Kind::RollOhb;
            const float amplitude = roll ? -cfg.dip_m : 0.0f;
            const s32 duration = phase.dodge.dip_ms;
            if (!dip_active && phase.dodging && amplitude != 0.0f && duration > 0 &&
                phase.dodge_ns >= 0 && phase.dodge_ns <= DipStartNs) {
                dip_active = true;
                dip_start = now - phase.dodge_ns;
                dip_ms = duration;
                dip_weight = amplitude;
            }
            if (dip_active) {
                const float t = static_cast<float>(now - dip_start) * 1e-9f;
                const float length = static_cast<float>(dip_ms) * 1e-3f;
                if (t < 0.0f || t >= length) {
                    dip_active = false;
                } else {
                    constexpr float TwoPi = 6.28318530718f;
                    dip =
                        s * dip_weight * 0.5f * (1.0f - std::cos(TwoPi * t / length));
                }
            }
        } else {
            dip_active = false;
        }
        // Only the jump's upward Master lift, not the crouch or landing tumble.
        const float jump_target=phase.dodging && phase.dodge.kind==Kind::Jump ?
            std::max(0.f,cfg.jump_lift)*cfg.jump_height_scale : 0.f;
        SmoothDamp(jump_offset,jump_velocity,jump_target,.025f,dt);
        if(jump_target==0.f && std::abs(jump_offset)<.00001f) jump_offset=jump_velocity=0.f;
        dip+=jump_offset;
        cam_out = {cam.x, cam.y + dip, cam.z};
        out.position = cam_out;
        out.dip = dip;
        out.fall_lag_m = fall_lag;
        Fill(out, from);
        return out;
    }

    void Reset() {
        initialized = false;
        restart = false;
    }
    bool Initialized() const {
        return initialized;
    }

private:
    static bool IsFinite(V3 v) {
        return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
    }

    static float TargetT(Mode mode, bool stopped, const Phase& phase, const ComfortConfig& cfg) {
        switch (mode) {
        case Mode::Ground:
            return cfg.ground_s * (stopped ? cfg.stop_factor : 1.0f);
        case Mode::Dodge:
            return cfg.dodge_s;
        case Mode::Fall:
            // A dodge not landed without a fall clip (down stairs or a slope): no faster than the
            // dodge's own time constant.
            return phase.dodging && !phase.fall_confirmed ? std::max(cfg.fall_s, cfg.dodge_s)
                                                          : cfg.fall_s;
        case Mode::Ladder:
            return cfg.ground_s;
        }
        return cfg.ground_s;
    }

    void Fill(ComfortOut& out, Mode from) const {
        out.mode = mode;
        out.mode_from = from;
        out.mode_changed = from != mode;
        out.takeoff = takeoff;
        out.t_eff = t_eff;
        out.vy = vel.y;
        out.speed = speed;
    }

    bool initialized{false};
    bool restart{false}; // a non-finite target came: the next finite one resets
    bool have_finite{false};
    V3 last_finite{}; // the last finite target (either eye, kept across resets)
    s64 finite_ns{0};
    V3 cam{};     // filter state (no dip)
    V3 vel{};     // game units per second
    V3 cam_out{}; // the first eye's output, shared by the second eye
    float t_eff{0.12f};
    s64 pair_ns{0};
    V3 pair_target{}; // the first eye's target
    V3 last_target{}; // the last tick's target (either eye)
    float speed{0.0f};
    u32 stopped_pairs{0};
    Mode mode{Mode::Ground};
    bool in_fall{false};
    bool was_confirmed{false}; // the last first eye was in a confirmed fall
    float takeoff{0.0f};
    s64 fall_left_ns{-1};
    float t_rise{0.12f};   // seconds: the vertical time constant while the target is above
    float rise_room{0.0f}; // the last first eye's height under its target (game units, >= 0)
    float drop_prev_y{0.0f}; // the last first eye's target height (game units)
    float drop_speed{0.0f};  // its speed down, smoothed (m/s)
    float jump_offset{},jump_velocity{};
    bool dip_active{false};
    s64 dip_start{0};
    s32 dip_ms{0};
    float dip_weight{0.0f};
};

// ---- Per-dodge statistics (VR_DODGE_LOG) -----------------------------------------------------

struct DodgeReport {
    bool valid{false};     // at least two first-eye ticks
    u32 ticks{0};          // first-eye ticks
    float root_dist{0.0f}; // m, horizontal: first to last tick
    float cam_dist{0.0f};
    float dist_ratio{1.0f};   // camera / body
    float dir_err_deg{0.0f};  // between the two displacements
    float h_lag_max{0.0f};    // m
    float h_over{0.0f};       // m: camera ahead of the body along the dodge
    float h_settle_ms{-1.0f}; // body stopped -> lag under 1 cm for good (-1 = never)
    float v_root_range{0.0f}; // m
    float v_cam_range{0.0f};
    float v_cam_vmax{0.0f}; // m/s
    float v_cam_amax{0.0f}; // m/s^2 over the first-eye sequence (30 Hz in stereo at 60 fps)
    float v_tail_ms{-1.0f}; // the body's last vertical move -> within 1 cm for good (-1 = never)
};

// Accumulates the first-eye sequence of one dodge, from its start to 300 ms after its end.
class DodgeStats {
public:
    static constexpr size_t Capacity = 600;
    static constexpr s64 TailNs = 300 * Millis;
    static constexpr float SettleM = 0.01f;
    static constexpr float MovingSpeed = 0.2f; // m/s
    static constexpr float VerticalStepM = 0.001f;

    void Begin(s64 now) {
        count = 0;
        active = true;
        start_ns = now;
        end_ns = -1;
        pairs = mismatches = resets = 0;
    }
    void Add(s64 now, V3 root, V3 camera) {
        if (active && count < Capacity) {
            samples[count++] = {now, root, camera};
        }
    }
    void End(s64 now) {
        if (active && end_ns < 0) {
            end_ns = now;
        }
    }
    bool Active() const {
        return active;
    }
    bool Ended() const {
        return active && end_ns >= 0;
    }
    bool Due(s64 now) const {
        return Ended() && now - end_ns >= TailNs;
    }

    DodgeReport Finish(float scale) {
        DodgeReport r{};
        active = false;
        r.ticks = static_cast<u32>(count);
        if (count < 2) {
            return r;
        }
        r.valid = true;
        const float inv = 1.0f / std::max(scale, 1e-3f);
        const Sample& first = samples[0];
        const Sample& last = samples[count - 1];
        const float rdx = (last.root.x - first.root.x) * inv;
        const float rdz = (last.root.z - first.root.z) * inv;
        const float cdx = (last.cam.x - first.cam.x) * inv;
        const float cdz = (last.cam.z - first.cam.z) * inv;
        r.root_dist = std::hypot(rdx, rdz);
        r.cam_dist = std::hypot(cdx, cdz);
        if (r.root_dist > 0.01f) {
            r.dist_ratio = r.cam_dist / r.root_dist;
        }
        if (r.root_dist > 0.01f && r.cam_dist > 0.01f) {
            const float c = (rdx * cdx + rdz * cdz) / (r.root_dist * r.cam_dist);
            r.dir_err_deg = std::acos(std::clamp(c, -1.0f, 1.0f)) * 57.2957795f;
        }
        const float ux = r.root_dist > 0.01f ? rdx / r.root_dist : 0.0f;
        const float uz = r.root_dist > 0.01f ? rdz / r.root_dist : 0.0f;
        float root_min = first.root.y, root_max = first.root.y;
        float cam_min = first.cam.y, cam_max = first.cam.y;
        size_t last_move = 0;
        size_t last_vmove = 0;
        float prev_vy = 0.0f;
        bool have_vy = false;
        for (size_t i = 0; i < count; ++i) {
            const Sample& p = samples[i];
            r.h_lag_max = std::max(r.h_lag_max, HorizontalError(p) * inv);
            r.h_over =
                std::max(r.h_over, ((p.cam.x - p.root.x) * ux + (p.cam.z - p.root.z) * uz) * inv);
            root_min = std::min(root_min, p.root.y);
            root_max = std::max(root_max, p.root.y);
            cam_min = std::min(cam_min, p.cam.y);
            cam_max = std::max(cam_max, p.cam.y);
            if (i == 0) {
                continue;
            }
            const Sample& q = samples[i - 1];
            const float dt = static_cast<float>(p.t - q.t) * 1e-9f;
            if (dt <= 0.0f) {
                continue;
            }
            if (LengthXZ(p.root - q.root) * inv / dt > MovingSpeed) {
                last_move = i;
            }
            if (std::abs(p.root.y - q.root.y) * inv > VerticalStepM) {
                last_vmove = i;
            }
            const float vy = (p.cam.y - q.cam.y) * inv / dt;
            r.v_cam_vmax = std::max(r.v_cam_vmax, std::abs(vy));
            if (have_vy) {
                r.v_cam_amax = std::max(r.v_cam_amax, std::abs(vy - prev_vy) / dt);
            }
            prev_vy = vy;
            have_vy = true;
        }
        r.v_root_range = (root_max - root_min) * inv;
        r.v_cam_range = (cam_max - cam_min) * inv;
        r.h_settle_ms = SettleMs(last_move, inv, false);
        r.v_tail_ms = SettleMs(last_vmove, inv, true);
        return r;
    }

    u32 pairs{0};      // second-eye ticks
    u32 mismatches{0}; // second-eye ticks not locked to their first eye
    u32 resets{0};

private:
    struct Sample {
        s64 t;
        V3 root;
        V3 cam;
    };

    static float HorizontalError(const Sample& p) {
        return LengthXZ(p.cam - p.root);
    }

    // From sample `from`: how long until the error stays under SettleM (-1 = not at the end).
    float SettleMs(size_t from, float inv, bool vertical) const {
        size_t settled = count;
        while (settled > 0) {
            const Sample& p = samples[settled - 1];
            const float error = vertical ? std::abs(p.cam.y - p.root.y) : HorizontalError(p);
            if (error * inv >= SettleM) {
                break;
            }
            --settled;
        }
        if (settled == count) {
            return -1.0f;
        }
        const size_t at = std::max(settled, from);
        return static_cast<float>(samples[at].t - samples[from].t) * 1e-6f;
    }

    std::array<Sample, Capacity> samples{};
    size_t count{0};
    bool active{false};
    s64 start_ns{0};
    s64 end_ns{-1};
};

} // namespace VR::Motion
