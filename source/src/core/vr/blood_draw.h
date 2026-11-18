// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
// Blood bullets by hand (the tester's 2026-11-04 "Right hand -> left shoulder + grip button = right
// hand grabs an empty injection vial. Using it (by stabbing like a blood vial), should make it
// fill with blood, edges of screen pulsate white - and we get 5 quicksilver bullets"): the right
// hand reaches over to the left shoulder and squeezes its grip: an empty syringe in the fist (the
// right weapon put away while it is there). Stabbed into the left arm, the body or a leg as a
// Blood Vial is, it draws the hunter's blood up into it and the game's own blood bullets are
// asked for (the effect its blood bullets clip applies, SpEffect 2012: 30% of max HP, which a
// rally wins back, for five Blood Bullets; without the clip, its arm or its blood spray). As the
// game takes it (Output::made) the presenter gives it a Blood Vial's feel: the THUMP on both hands
// and the hands held where the needle went in. Once the bullets came and the blood is all drawn
// up, the view's edges pulse white twice and the syringe,
// full, is put away; refused (too little HP for the cost, the five already there), the blood runs
// back down and nothing is spent. Let go of the grip before a stab: put back. Pure logic, no
// allocation; the presenter feeds it every XR frame.
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace VR::BloodDraw {

inline constexpr float GripPress = .75f, GripRelease = .45f; // the right squeeze (hysteresis)
inline constexpr std::int64_t FillNs = 800'000'000;      // drawn up into it (0.55 s until 2026-11-04)
inline constexpr std::int64_t AnswerNs = 1'800'000'000;  // the game's bullets within this
inline constexpr std::int64_t DrainNs = 450'000'000;     // refused: the blood runs back
inline constexpr std::int64_t ShowFullNs = 700'000'000;  // full, then put away
inline constexpr std::int64_t PutAwayNs = 220'000'000;   // its fade as it goes
inline constexpr std::int64_t LostNs = 300'000'000;      // untracked / not gameplay this long: away
inline constexpr std::int64_t StabGraceNs = 200'000'000; // no stab right after the draw
// The game's D-pad up instead: site A counts its Blood Bullets this long (past AnswerNs).
inline constexpr std::int64_t FallbackCountNs = 2'500'000'000;
// The vial's hold where the needle went in only for an answer this soon after the stab (the
// D-pad's clip answers about 0.9 s later, the hand long gone from there).
inline constexpr std::int64_t HoldAnswerNs = 250'000'000;
// The white pulses at the edges: two beats (a heartbeat's), the second softer.
inline constexpr std::int64_t PulseRiseNs = 55'000'000, PulseFallNs = 300'000'000,
                              PulseGapNs = 290'000'000;

enum class Phase : std::uint8_t { Idle, Held, Drawing, Full, Refused, Away };
constexpr const char* PhaseName(Phase p) {
    switch (p) {
    case Phase::Idle: return "idle";
    case Phase::Held: return "held";
    case Phase::Drawing: return "drawing";
    case Phase::Full: return "full";
    case Phase::Refused: return "refused";
    case Phase::Away: return "put away";
    }
    return "?";
}
enum class Feel : std::uint8_t { None, Draw, PutBack, Stab, Filled, Refused };
// The game's answer to a stab's ask (core/vr/blood_draw_channel.h; the main player's update):
// made (its own blood bullets effect, SpEffect 2012, took); refused by the game's own rules (the
// five Blood Bullets already there; too little HP for their cost; another item in use; the effect
// not taken); or the direct path not there (Unavailable: the game's own D-pad up instead).
enum class Answer : std::uint8_t { None, Made, Full, LowHp, Busy, Rejected, Unavailable, Forbidden };
constexpr bool Refusal(Answer a) {
    return a == Answer::Full || a == Answer::LowHp || a == Answer::Busy || a == Answer::Rejected ||
           a == Answer::Forbidden;
}
constexpr const char* RefusalLog(Answer a) {
    switch (a) {
    case Answer::Full: return "refused: the five Blood Bullets are already there";
    case Answer::LowHp: return "refused: too little HP for their cost";
    case Answer::Busy: return "refused: another item is in use";
    case Answer::Rejected: return "refused: the game did not take its effect";
    case Answer::Forbidden: return "refused: no items here";
    default: return "no blood bullets came (the game did not answer)";
    }
}

struct Input {
    std::int64_t now{};
    bool gameplay{};    // in the world, not a menu / cinematic / the screen
    bool tracked{};     // the right controller's pose
    bool at_shoulder{}; // the right hand at the left shoulder (its zone, settled)
    float grip{};       // the right squeeze, 0..1
    bool busy{};        // the right hand is the weapon's or another act's (a swing, two hands...)
    bool stab{};        // the syringe's stab gate fired this frame (the presenter's)
    bool bullets_valid{};
    std::int32_t bullets{}; // the Blood Bullets now (VR::Vitals blood_bullets)
    Answer answer{};        // the game's answer to this stab's ask (None until it comes)
};
struct Output {
    Phase phase{Phase::Idle};
    bool shown{};        // the syringe in the right fist (draw it; hide the right weapon)
    float fill{};        // the blood in it, 0..1
    float fade{1.0f};    // its opacity (the put-away)
    bool ask_bullets{};  // this frame: ask the game for its blood bullets
    bool made{};         // this frame: the game took the stab (the Blood Vial's thump, its hold)
    bool stab_too_soon{}; // this frame: a stab inside StabGraceNs of the draw, ignored (re-arm)
    bool owns_grip{};    // the right grip is the syringe's (no transform, no L1)
    Feel feel{Feel::None};
    const char* log{nullptr};
};

inline float Smooth(float s) {
    s = std::clamp(s, 0.0f, 1.0f);
    return s * s * (3.0f - 2.0f * s);
}
// How hard blood is coming in through the needle (0..1): all of it while drawing, easing off as
// the syringe tops up (the shader's swirl at the needle end, vr_inframe.frag ShadeSyringeFn).
inline float Inflow(Phase phase, float fill) {
    if (phase == Phase::Drawing) return 1.0f - 0.45f * Smooth(fill / .92f);
    if (phase == Phase::Full) return 0.55f * (1.0f - Smooth((fill - .92f) / .08f));
    return 0.0f;
}

// The white edge pulse at `ns` after the bullets came (0..1; 0 outside).
inline float Pulse(std::int64_t ns) {
    const auto beat = [](std::int64_t t) {
        if (t < 0) return 0.0f;
        if (t < PulseRiseNs) return Smooth(float(t) / float(PulseRiseNs));
        const float f = float(t - PulseRiseNs) / float(PulseFallNs);
        return f >= 1.0f ? 0.0f : (1.0f - f) * (1.0f - f);
    };
    return std::max(beat(ns), 0.6f * beat(ns - PulseGapNs));
}

class Machine {
public:
    void Reset() { *this = {}; }
    // A context switch (a menu, a load): the syringe gone at once; a grip still held is no new
    // squeeze (only a press after letting go draws again) and stays the syringe's until let go.
    void PutAway() {
        const bool owns = owns_;
        *this = {};
        grip_ = true;
        owns_ = owns;
    }
    Phase State() const { return phase_; }
    std::int64_t BulletsAt() const { return bullets_ns_; }
    std::int64_t StabAt() const { return stab_ns_; } // the last accepted stab

    Output Update(const Input& in) {
        Output out{};
        const bool grip = grip_ ? in.grip > GripRelease : in.grip >= GripPress;
        const bool press = grip && !grip_;
        grip_ = grip;
        const bool live = in.gameplay && in.tracked;
        lost_ns_ = live ? 0 : (lost_ns_ == 0 ? in.now : lost_ns_);
        const bool lost = !live && in.now - lost_ns_ >= LostNs;
        switch (phase_) {
        case Phase::Idle:
            // A fresh squeeze at the shoulder (never one already held as the hand arrived).
            if (press && live && in.at_shoulder && !in.busy) {
                Enter(Phase::Held, in.now);
                out.feel = Feel::Draw;
                out.log = "an empty syringe drawn from the left shoulder: stab it to draw blood";
            }
            break;
        case Phase::Held:
            if (lost) {
                Enter(Phase::Idle, in.now);
                out.log = "the syringe put away (tracking or gameplay lost)";
                break;
            }
            if (!grip) {
                Enter(Phase::Idle, in.now);
                out.feel = Feel::PutBack;
                out.log = "the syringe put back (the grip let go)";
                break;
            }
            // The grace period keeps the model visible through a brief tracking gap; it
            // must never authorize spending HP from an invalid pose or gameplay context.
            if (!live) break;
            if (in.stab && in.now - since_ >= StabGraceNs) {
                Enter(Phase::Drawing, in.now);
                stab_ns_ = in.now;
                bullets_before_ = in.bullets_valid ? in.bullets : -1;
                made_ = false;
                out.ask_bullets = true;
                out.feel = Feel::Stab;
                out.log = "STAB: drawing blood; the game's blood bullets asked for";
            } else if (in.stab) {
                // The hand still coming away from the shoulder: not this one, the next.
                out.stab_too_soon = true;
                out.log = "a stab right after the draw: ignored (stab again)";
            }
            break;
        case Phase::Drawing:
            // The game's answer: its effect took, or more Blood Bullets than at the stab (the
            // game's own D-pad up when the direct path is not there). The blood is drawn up all
            // the way before the syringe is full, however soon they came.
            if (!made_ && (in.answer == Answer::Made ||
                           (in.bullets_valid && bullets_before_ >= 0 &&
                            in.bullets > bullets_before_))) {
                made_ = true;
                out.made = true;
            }
            if (made_ && (in.now - since_ >= FillNs || lost)) {
                Enter(Phase::Full, in.now);
                bullets_ns_ = in.now;
                out.feel = Feel::Filled;
                out.log = "the game's blood bullets came: the syringe full, the edges pulse";
            } else if (!made_ && (Refusal(in.answer) || in.now - since_ >= AnswerNs || lost)) {
                Enter(Phase::Refused, in.now);
                out.feel = Feel::Refused;
                out.log = lost ? "the syringe lost before the game answered" : RefusalLog(in.answer);
            }
            break;
        case Phase::Full:
            if (in.now - since_ >= ShowFullNs || lost) Enter(Phase::Away, in.now);
            break;
        case Phase::Refused:
            if (in.now - since_ >= DrainNs) {
                // Back to the empty syringe in the fist while the grip holds it (stab again).
                Enter(grip && live ? Phase::Held : Phase::Idle, in.now);
                if (phase_ == Phase::Held) since_ = in.now - StabGraceNs;
            }
            break;
        case Phase::Away:
            if (in.now - since_ >= PutAwayNs) Enter(Phase::Idle, in.now);
            break;
        }
        out.phase = phase_;
        const float t = float(in.now - since_);
        switch (phase_) {
        case Phase::Held: out.fill = 0.0f; break;
        case Phase::Drawing: {
            // A gush the moment the needle goes in, slowing as it fills (2026-11-04:
            // "the moment the needle collides, it needs to start filling with blood").
            const float s = 1.0f - std::clamp(t / float(FillNs), 0.0f, 1.0f);
            out.fill = (1.0f - s * s) * .92f;
            break;
        }
        case Phase::Full:
            out.fill = .92f + .08f * Smooth(t / 120'000'000.0f);
            break;
        case Phase::Refused: out.fill = fill_at_ * (1.0f - Smooth(t / float(DrainNs))); break;
        case Phase::Away:
            out.fill = 1.0f;
            out.fade = 1.0f - Smooth(t / float(PutAwayNs));
            break;
        case Phase::Idle: break;
        }
        if (phase_ != Phase::Refused) fill_at_ = out.fill;
        out.shown = phase_ != Phase::Idle;
        // The grip is the syringe's from the draw until it is let go after it went away.
        if (phase_ != Phase::Idle) owns_ = true;
        else if (!grip) owns_ = false;
        out.owns_grip = owns_;
        return out;
    }

private:
    void Enter(Phase p, std::int64_t now) {
        phase_ = p;
        since_ = now;
    }
    Phase phase_{Phase::Idle};
    std::int64_t since_{}, lost_ns_{}, bullets_ns_{}, stab_ns_{};
    std::int32_t bullets_before_{-1};
    float fill_at_{};
    bool grip_{}, owns_{}, made_{};
};

} // namespace VR::BloodDraw
