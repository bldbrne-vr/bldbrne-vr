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

#include "core/vr/body_frame.h"
#include "core/vr/body_settings.h"

// VR body: the live weapon grip adjust (the body-feel design's "live weapon adjust", UEVR's
// Adjust workflow). Hold A+B for 1.2 s standing still (the A+B chord, core/vr/roll_chord.h, feeds
// it: the stick pushed while held makes the chord a roll instead): the weapon freezes where it is
// (the presenter keeps publishing the right grip's pose of that moment, so the weapon stays put in
// tracking space while the hunter's arm holds it there), you line your controller up with the
// frozen weapon the way you want to hold it, and on releasing A or B the weapon stays where it was
// frozen relative to the controller's new pose: the new trim and offset (AdjustTrim) are saved for
// the weapon family in hand (vr.ini weapon_grip_<weapon>, body_settings). Pulling the right trigger
// while adjusting puts that weapon back on the default grip instead. With roll_chord=1 neither A
// nor B reaches the game during the chord; with roll_chord=0 they do, as before. The presenter
// keeps the hunter still while frozen.
//
// Pure (no OpenXR or emulator state): the presenter runs it on the OpenXR frame thread, the unit
// tests (tests/test_body_feedback.cpp) on scripted inputs.
namespace VR::Body {

// ---- Grip math ------------------------------------------------------------------------------

// An OpenXR (tracking space) rotation in the game's controller frame: P conjugation (body_frame.h
// TrackingToWorldRotation without the camera's yaw).
inline Quat ToControllerFrame(const Quat& q) {
    return {-q.x, -q.y, q.z, q.w};
}

// The yaw, pitch and roll (degrees) of a trim rotation: the inverse of GripTrimRotation (exact
// while |pitch| < 90). For M = Ry(a) Rx(b) Rz(c): M12 = -sin b, (M02, M22) = cos b (sin a, cos a),
// (M10, M11) = cos b (sin c, cos c).
inline void TrimAngles(int side, const Quat& trim, float& yaw_deg, float& pitch_deg,
                       float& roll_deg) {
    const Quat q = Normalize(trim);
    const Vec3 x = Rotate(q, {1.0f, 0.0f, 0.0f});
    const Vec3 y = Rotate(q, {0.0f, 1.0f, 0.0f});
    const Vec3 z = Rotate(q, {0.0f, 0.0f, 1.0f});
    const float b = std::asin(std::clamp(-z.y, -1.0f, 1.0f));
    const float a = std::atan2(z.x, z.z);
    const float c = std::atan2(x.y, y.y);
    const float mirror = side == C0000::Left ? -1.0f : 1.0f;
    yaw_deg = mirror * a / Deg;
    pitch_deg = -b / Deg;
    roll_deg = mirror * c / Deg;
}

inline Quat TrimRotation(int side, const GripTrim& t) {
    return GripTrimRotation(side, t.yaw_deg, t.pitch_deg, t.roll_deg);
}

inline Vec3 TrimOffset(const GripTrim& t) {
    return {t.offset_m[0], t.offset_m[1], t.offset_m[2]};
}

struct AdjustResult {
    GripTrim trim{};
    float turn_deg{0.0f}; // how far the controller turned against the frozen weapon
    float move_m{0.0f};   // how far the grip moved
};

// The grip that puts the weapon of the frozen grip pose (p0, q0) with `old` on the grip pose
// (p1, q1) (tracking space): TW(q1) T1 = TW(q0) T0, i.e. T1 = P(q1^-1 q0) T0 (the camera's yaw
// cancels), and p1 + R(q1) o1 = p0 + R(q0) o0. Not limited (SetWeaponGrip clamps).
inline AdjustResult AdjustTrim(int side, const GripTrim& old, Vec3 p0, const Quat& q0, Vec3 p1,
                               const Quat& q1) {
    const Quat a = Normalize(q0);
    const Quat b = Normalize(q1);
    const Quat relative = Conjugate(b) * a;
    const Quat trim = Normalize(ToControllerFrame(relative) * TrimRotation(side, old));
    AdjustResult r{};
    TrimAngles(side, trim, r.trim.yaw_deg, r.trim.pitch_deg, r.trim.roll_deg);
    const Vec3 weapon = p0 + Rotate(a, TrimOffset(old));
    const Vec3 offset = Rotate(Conjugate(b), weapon - p1);
    r.trim.offset_m = {offset.x, offset.y, offset.z};
    r.turn_deg = AngleOf(relative) / Deg;
    r.move_m = Length(p1 - p0);
    return r;
}

// ---- The A+B gesture ------------------------------------------------------------------------

enum class AdjustPhase : std::uint8_t {
    Idle,
    Chord,     // A and B held, counting to HoldS
    Adjusting, // the weapon is frozen
    Wait,      // done (saved, reset, cancelled or denied): until A and B are both released
};

enum class AdjustEvent : std::uint8_t {
    None,
    Enter,  // frozen at p0 / q0
    Save,   // released: keep the weapon frozen at p0 / q0 with the grip at p1 / q1
    Reset,  // right trigger while adjusting: back to the default grip
    Cancel, // tracking lost, 60 s, or Cancel()
    Deny,   // HoldS held but not `ready` or not tracked
};

struct AdjustInput {
    float dt{0.0f};
    bool a{false};
    bool b{false};
    bool trigger{false};
    bool ready{false};   // the body drives the right arm and the weapon in it is known
    bool tracked{false}; // the right grip's pose (tracking space) this frame
    Vec3 p{};
    Quat q{};
};

struct AdjustOutput {
    AdjustEvent event{AdjustEvent::None};
    const char* why{""}; // Cancel / Deny
    Vec3 p1{};           // Save: the grip pose to keep
    Quat q1{};
};

class GripAdjust {
public:
    // The second button within this of the first (the presenter feeds A and B together from the
    // chord, core/vr/roll_chord.h, which has its own window).
    static constexpr float ChordWindowS = 0.30f;
    static constexpr float HoldS = 1.2f;
    static constexpr float TimeoutS = 60.0f;
    // The pose kept on release is the newest one at least this old: lifting the thumb off A or B
    // jolts the controller.
    static constexpr float SettleS = 0.06f;

    AdjustOutput Update(const AdjustInput& in) {
        AdjustOutput out{};
        const float dt = std::max(in.dt, 0.0f);
        const bool pressed = (in.a && a_time < 0.0f) || (in.b && b_time < 0.0f);
        a_time = in.a ? (a_time < 0.0f ? 0.0f : a_time + dt) : -1.0f;
        b_time = in.b ? (b_time < 0.0f ? 0.0f : b_time + dt) : -1.0f;
        const bool both = in.a && in.b;
        const bool trigger_edge = in.trigger && !trigger_down;
        trigger_down = in.trigger;
        switch (phase) {
        case AdjustPhase::Idle:
            // A chord starts on the press that completes it, the other one at most
            // ChordWindowS earlier (A held to sprint and then B is not one).
            if (both && pressed && std::abs(a_time - b_time) <= ChordWindowS) {
                phase = AdjustPhase::Chord;
                hold = 0.0f;
            }
            break;
        case AdjustPhase::Chord:
            if (!both) {
                phase = AdjustPhase::Idle;
                break;
            }
            hold += dt;
            if (hold < HoldS) {
                break;
            }
            if (!in.ready || !in.tracked) {
                phase = AdjustPhase::Wait;
                out.event = AdjustEvent::Deny;
                out.why = !in.tracked ? "right controller not tracked" : "not ready";
                break;
            }
            phase = AdjustPhase::Adjusting;
            p0 = in.p;
            q0 = in.q;
            time = 0.0f;
            history_count = 0;
            Push(in.p, in.q, 0.0f);
            out.event = AdjustEvent::Enter;
            break;
        case AdjustPhase::Adjusting:
            time += dt;
            if (!in.tracked) {
                phase = AdjustPhase::Wait;
                out.event = AdjustEvent::Cancel;
                out.why = "right controller lost";
                break;
            }
            Push(in.p, in.q, dt);
            if (trigger_edge) {
                phase = AdjustPhase::Wait;
                out.event = AdjustEvent::Reset;
                break;
            }
            if (time >= TimeoutS) {
                phase = AdjustPhase::Wait;
                out.event = AdjustEvent::Cancel;
                out.why = "held for 60 s";
                break;
            }
            if (!both) {
                phase = in.a || in.b ? AdjustPhase::Wait : AdjustPhase::Idle;
                out.event = AdjustEvent::Save;
                Settled(out.p1, out.q1);
            }
            break;
        case AdjustPhase::Wait:
            if (!in.a && !in.b) {
                phase = AdjustPhase::Idle;
            }
            break;
        }
        return out;
    }

    // From outside (a context switch, the body off): ends an adjust without saving. True when one
    // was running.
    bool Cancel() {
        const bool was = phase == AdjustPhase::Adjusting;
        if (phase != AdjustPhase::Idle) {
            phase = AdjustPhase::Wait;
        }
        return was;
    }

    AdjustPhase Phase() const {
        return phase;
    }
    bool Frozen() const {
        return phase == AdjustPhase::Adjusting;
    }
    float Time() const {
        return time;
    }

    Vec3 p0{}; // the frozen grip pose (tracking space)
    Quat q0{};

private:
    struct Sample {
        float age{0.0f};
        Vec3 p{};
        Quat q{};
    };

    void Push(Vec3 p, const Quat& q, float dt) {
        for (std::uint32_t i = 0; i < history_count; ++i) {
            history[i].age += dt;
        }
        if (history_count == history.size()) {
            std::copy(history.begin() + 1, history.end(), history.begin());
            --history_count;
        }
        history[history_count++] = {0.0f, p, q};
    }

    // The newest sample at least SettleS old, else the oldest one.
    void Settled(Vec3& p, Quat& q) const {
        std::uint32_t pick = 0;
        for (std::uint32_t i = 0; i < history_count; ++i) {
            if (history[i].age >= SettleS) {
                pick = i;
            }
        }
        p = history[pick].p;
        q = history[pick].q;
    }

    AdjustPhase phase{AdjustPhase::Idle};
    float a_time{-1.0f}; // seconds held, -1 = up
    float b_time{-1.0f};
    bool trigger_down{false};
    float hold{0.0f};
    float time{0.0f};
    std::array<Sample, 24> history{}; // oldest first
    std::uint32_t history_count{0};
};

} // namespace VR::Body
