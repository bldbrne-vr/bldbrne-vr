// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <vector>

#include "core/vr/whip_chain.h"

namespace VR::AmygdalanArm {
namespace B = Body;
namespace W = Whip;

// wp_a_1701_2: handle -> Dummy001 -> Dummy002 -> Dummy003. Only the two
// tentacle joints move, including the last bone carrying the bladed tip.
// No payout or new damage windows; only the lash (below) stretches the two middle links.
// wp_a_1701_2 dummy 100 transformed into Dummy003's bind frame (from the user's mesh).
inline constexpr B::Vec3 TipLocal{-0.00442427f, 0.00472402f, -0.90037f};
// The lash (2026-11-06: "extend wildly ... at 3.5m when trigger is pressed ... if we
// are mid swing make it be affected by that swing"; then, on seeing it held out straight: "the
// trigger swing... it makes it expand, if it doesn't hit anything it goes back to its regular
// size. If it does hit something then it stays extended and we can pull the enemy close, or
// swing it around"; then: "wiggles wayyy too much, it needs to feel more.. visceral, like an
// insect leg that curves at its joints, not like jelly or rubber"): with the right trigger held in
// physical form B (core/vr/amygdalan_hook.h OwnsTrigger) a swing throws the tentacle out along the
// arm, unfolding at its joints and reaching ReachM in LashOutS, carried by the swing.
// It hangs out while the hand keeps swinging (the tester: "delay like 150-200 ms or so before it
// snaps back so that it feels a little more dynamic"): HangS after the swing slows under HangMps,
// it folds back into the idle shape in LashInS, held or not, unless its
// tip hooked an enemy: then it stays out to it for as long as the hook holds, its two middle links
// stretching up to TautStretch when the enemy is farther (pulled taut). Folding back 0.3 s after
// the throw, it was barely ever out at full length (the game's strikes hold it there 0.2-0.4 s).
// The reach (the user, after the headset, 2026-11-06: "very weak. I thought it would extend
// further, and would be easier to extend (like less force in a swing for it to move and flap)"):
// 3.5 m, the middle links stretched for it (its own 3.0 m felt short), a lighter swing throws it
// and keeps it out (FireMps, HangMps) and it flaps more while out (HangConfig).
inline constexpr float ReachM = 3.5f;
inline constexpr float TautStretch = 1.5f; // x its own middle links, hooked
// The spent lash droops first, then recalls quickly from that pose.
inline constexpr float LashOutS = 0.12f, LashInS = 0.22f;
inline constexpr float NoSagRecallS = LashInS + 0.06f;
// Let the spent lash sag for 0.42 s before the quick recall.
inline constexpr float HangS = 0.42f, HangMps = 1.0f;
// Deterministic follow-through: a spent tip recalls directly; a lively tip gets the full sag.
// Speed is relative to the chain root, so walking alone does not prolong the delay. Only a big
// swing sags (2026-11-07: "if we don't have a lot of momentum it shouldn't trigger as
// right now it happens too much"): none up to SagFromMps, the full HangS from SagFullMps.
// SagFromMps 2 (3 until 2026-11-10, the user's choice: it "retracts back too soon"; judged on the
// hand's own swing since then, a stroke under 3 m/s came straight back).
inline constexpr float SagFromMps = 2.0f, SagFullMps = 6.0f;
inline float SagSeconds(float relative_speed) {
    if (!std::isfinite(relative_speed)) return 0.0f;
    const float t =
        std::clamp((relative_speed - SagFromMps) / (SagFullMps - SagFromMps), 0.0f, 1.0f);
    return HangS * t * t * (3.0f - 2.0f * t);
}
// One bounded sample per slowdown, never per frame. Local state keeps replays reproducible
// and avoids a shared random generator, locks, or allocations in the pose hook.
inline float SagVariation(std::uint32_t& state) {
    if (!state) state = 0x9e3779b9u;
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    const float sample = static_cast<float>(state >> 8) / 16777215.0f;
    // One decision for this eligible slowdown: 15% recall directly, otherwise retain
    // the gentle +/-10% timing variation. A zero uses the existing no-sag recall.
    return sample < 0.15f ? 0.0f : 0.9f + 0.2f * (sample - 0.15f) / 0.85f;
}
// A skipped sag still waits out a reversal (the hand slow this long) before its recall: drawn at
// every slowdown, a zero recalled at once snapped the tentacle back mid-swing at 15% of the
// reversals of a sustained back-and-forth (0.9.4 review). No hang and no droop meanwhile.
inline constexpr float SkipHoldS = 0.12f;
inline constexpr float JabToLashS = 0.12f;
// A swing that throws it: the hand this fast, or turning as fast HandTurnM out (m/s). It leaves
// at the swing's peak, the hand back under ReleaseShare of its fastest (the arm then about the
// swing's middle: a chop's throw goes forward, not up and back from the wind-up), or once the hand
// has swung that fast FireHoldS. The next throw waits for the tentacle back in and the hand
// slowed under RearmShare of FireMps.
// (2.5 m/s needed a hard swing in the headset.)
inline constexpr float FireMps = 1.6f, HandTurnM = 0.5f, RearmShare = 0.6f;
inline constexpr float ReleaseShare = 0.85f, FireHoldS = 0.15f;
// Free lashes need deliberate travel as well as speed: one short tracking/wrist spike
// followed by a slow sample must not launch the whole extension-and-sag sequence.
inline constexpr float FreeStrokeS = 0.03f, FreeStrokeM = 0.10f;
// The push out along the arm at the throw (the tip's share; the links nearer the hand less).
inline constexpr float LaunchMps = 16.0f;
// The lash's reach plus a margin bounds the tip at 3.6 m. This is a
// marker-validity ceiling, NOT a larger hit capsule or a damage/reach multiplier;
// it only ever raises the caller's own limit.
constexpr float MarkerLimit(int variation, bool form_b, float ordinary) {
    return variation == 2500 && form_b && ordinary < 3.6f ? 3.6f : ordinary;
}
// An attack's first frames: the bend the idle left fades out over this, not at once.
inline constexpr float FadeS = 0.12f;
// The most the hand may move in one frame and still swing the tentacle (more is a jump: a load,
// a tracking glitch): this far or this fast, whichever is more.
inline constexpr float JumpM = 0.4f, JumpMps = 15.0f;
inline constexpr float JumpDeg = 45.0f, JumpDegS = 2500.0f;
// wp_a_1701_2's B->A clip, also listed in transform_clips.h. Never infer a
// transform from drive=false: that also means tracking/body/physics was disabled.
inline constexpr std::int32_t ClosingClip = 19550;
using Pose = std::array<B::QsTransform, 4>;
// The bladed tip of `pose` (the part's frame).
inline B::Vec3 TipOf(const Pose& pose) {
    B::QsTransform m = pose[0];
    for (int i = 1; i <= 3; ++i) m = B::Compose(m, pose[i]);
    return m.t + B::Rotate(B::Normalize(m.q), TipLocal);
}

// The snap (2026-11-07, of the thrown tentacle: "it needs to snap... like check the
// game's own animations when it's at whip form, it looks really cool in base game"): a throw plays
// the game's own strike for the tentacle, one of the whip form's attack clips read from the user's
// files at start (core/vr/amygdalan_strike.h; none: the reach-out below). In it the tentacle loops
// back, unfolds, cracks at full reach (the tip at 50-75 m/s, the joints whipping back and forth),
// carries on round past the crack, comes back partway and refolds, the joint at the hand first.
// It starts as the swing does (the hand at FireMps), its loop trailing the hand through the swing
// in WindS and its unfold in UnfoldS (the game's take about 0.45 and 0.3 s), so it cracks about as
// the hand stops, like a whip (both sped up for a fast swing: SwingRateMps); the last CrackLeadS
// of the unfold, the crack and the carry on at the game's own speed (sped up, it ran at 120 m/s). It
// is held there while the hand swings on (the hang: HangS, HangMps, straight out in HangBlendS),
// then refolded in RefoldS. Turned about the arm so that it cracks the way the hand swings (a
// forehand, a backhand, a chop), aimed until CrackLeadS before the crack (AimS below), turning no
// faster than TurnDegS.
// (Thrown at the swing's peak instead, the 0.45 s loop had 0.1 s: a whirl at 90 m/s; skipped, the
// unfold starts 1 m from the idle shape.)
inline constexpr float WindS = 0.16f, UnfoldS = 0.10f, CrackLeadS = 0.10f, RefoldS = 0.6f;
inline constexpr float HangBlendS = 0.15f, TurnDegS = 2000.0f;
inline constexpr float SwingRateMps = 2.5f, MinSwingRate = 0.85f, MaxSwingRate = 1.25f;
// Lashed about without the trigger (2026-11-07: "if I am not pressing TRIGGER but I am
// in whip mode, and want to just lash it around... it doesn't expand at all, or if it does it comes
// back immediately ... it doesn't feel like I am swinging around an alien leg"): any swing in
// physical form B plays the strike, from a little harder a swing without the trigger (FreeFireMps:
// walking and small moves of the hand throw nothing); the trigger only lets its tip latch on an
// enemy (core/vr/amygdalan_hook.h Catch). It stays out while the hand swings on, the trigger held
// or not, and each new stroke while it is out cracks it again from CrackLeadS before the crack (the
// hang let go and each link swung from where it was onto the strike in RecrackBlendS: the strike's
// pull is hard, and put straight onto the new stroke's pose the tentacle jumped across in a frame),
// so lashed back and forth it snaps every way it is swung. 2.0 m/s: 1.8 was tried on 2026-11-11 (a
// left-to-right swipe, a backhand, sometimes stayed folded) and taken back the same day (the user:
// "keep the amygdala to 2m/s"): in the headset it felt as if the arm flew too late, and on the
// user's recorded swings 4 overhead chops stopped slamming (thrown 0.15-0.67 s before the chop, as
// the arm still rose, so it was already out when the chop came).
inline constexpr float FreeFireMps = 2.0f, RecrackBlendS = 0.08f;
// Aimed (the tester: "sometimes I swing and it flies in a weird direction") by the hand's way over
// the last AimS, its fastest motion counting most, until CrackLeadS before the crack. Kept from the
// swing's first peak on, a wind-up's way (back, before the swing) sent it the wrong way.
inline constexpr float AimS = 0.05f;
// The jab (the tester: "if I am holding the weapon still, and press trigger to make it fly forward
// 3m for a brief moment"): the trigger pressed with the hand under JabStillMps for JabCalmS, and no
// swing starting in JabWaitS, throws it straight out along the arm to JabM (its own length) in
// LashOutS and holds it there JabHoldS (or for as long as its tip latched on an enemy), then it is
// back in in LashInS. A swing while it is out cracks it from there (as a new stroke does).
inline constexpr float JabM = 3.0f, JabStillMps = 0.6f, JabCalmS = 0.1f, JabWaitS = 0.06f;
inline constexpr float JabHoldS = 0.12f;

// The lash as the game's limb moves (2026-11-10, approving the study page that set the
// game's own form-B attacks and his recorded swings beside ours: "implement your changes"). Our
// reach and our crack stay as they are (3.5 m, as fast as before: the user's choice); the rest comes
// AFTER the crack:
// - Coil, then crack: a free throw only on a stroke that does not point behind you, the
//   controller's line (level) under acos(CoilAheadCos) from the hunter's facing: drawn right back
//   behind you (a wind-up) it stays folded for the strike. Every other stroke throws as it always
//   did, at its peak (2026-11-10: "I would have expected it to be delayed compared to
//   ours today": the study's proposal also threw any stroke ahead as it started, earlier than
//   today). The game's limb only loops on its wind-up and unfolds for the hit. No heading known:
//   as before. The user's "mostly
//   today" value (2026-11-10, about 35% of the way from today toward the study's proposal: "so
//   mostly how it is today with just slight adjustments"; the full one "extends too much delayed"
//   in some attacks): refused only past 152 degrees (today: never; the proposal: past 101, where
//   the Beast Cutter's throw, CutterThrowShare, starts to count). Softer since 2026-11-11 (the
//   user: keep it, but softer, more than the swing detector's wind-up rule for the other
//   weapons): refused only past 165 degrees, the controller within 15 of straight behind you.
//   162 since 2026-11-12 (the tester: "let's do the amygdala 162 degrees"): within 18 of behind.
inline constexpr float CoilAheadCos = -0.951f;
// Refused on a wind-up, the stroke ahead within CoilHoldS throws as it gets going, not at its
// peak: out in LashOutS, the limb is at full reach for the stroke's fastest moment (thrown at the
// peak of a stroke after a wind-up, the crack came as the hand slowed: the hunter's fourth R1
// cracked at 64 m/s, not 146). Only after a refused wind-up, so the throw is never sooner than
// today's.
inline constexpr float CoilHoldS = 0.4f;
// - The crack, held, then drawn in: full length for CrackHoldS once it is out, and while the hand
//   swings at HardSwingMps or more (and CrackHoldS after); otherwise it springs in to a bent leg
//   WorkReachM from the handle (the game's: 90% of its length for 0.07-0.25 s per light attack,
//   then drawn in, springing back out 2-3 times; ours held its full 3.5 m 0.7-1.0 s, and on the
//   user's swings for seconds on end, since a hand over 1 m/s kept it there). The draw is a spring
//   (DrawHz, DrawZeta: one or two soft bounces, like the game's); back out at LashOutS's pace.
// The user's "mostly today" values (2026-11-10, about 35% toward the study's proposal; the full
// one "retracts back too soon" in some cases): held 0.6 s (today about 0.85 before its sag and
// recall; the proposal 0.2), at full reach from a 1.7 m/s swing (today out from 1; the proposal
// 3), drawn in only to 3.0 m (today never, 3.5; the proposal 2.0).
inline constexpr float CrackHoldS = 0.6f, HardSwingMps = 1.7f, WorkReachM = 3.0f;
// The hold counts from the limb itself reaching CrackSpanShare of its full span (or CrackWaitS
// after its rest shape did: a crack against a wall).
inline constexpr float CrackSpanShare = 0.97f, CrackWaitS = 0.25f;
inline constexpr float DrawHz = 3.0f, DrawZeta = 0.4f;
// - Held up like a leg: live (the hand moving, HangMps or more) it carries LegGravity of its
//   weight, the joints holding it up; only a spent lash (the hand slowed: the sag) hangs its full
//   weight. (On the hunter's first R1 ours dragged its tip on the floor 77% of its time out; the
//   game's sweeps never touch it.) The user's "mostly today" value (2026-11-10, about 35% toward
//   the study's proposal): 2.7 (today the hang's 3.6; the proposal 1.0).
inline constexpr float LegGravity = 2.7f;
// ... and aimed no lower than LegMinPitchDeg below level while it lashes (a sweep along the floor
// was the controller pointing down: the game's sweeps hold the tip 0.9-1.3 m up). "Mostly today":
// 35 degrees (today none; the proposal 10). 35% of the way there would barely act; 35 degrees
// still keeps a lash from being driven steeply into the floor, though from hand height its tip
// meets the ground about 1.9 m out.
inline constexpr float LegMinPitchDeg = 35.0f;
// - The sag judged on your swing, not the tip (a 3.5 m tip nearly always runs 13-17 m/s against
//   the hand at a slowdown, so the 3-6 m/s cut-off refused only 54 of the user's 685): the
//   hand's fastest over the last SagPeakS through SagSeconds (none to 3 m/s, all of HangS from 6).
inline constexpr float SagPeakS = 0.3f;
// - The slam (the game's R2 and charged R2 smash the limb into the ground 3 m ahead): a chop, the
//   controller's line raised SlamRaisedY or more during the stroke and the hand going down (its
//   motion SlamDownShare downward, SlamMps or faster), throws it up along the raised arm and brings
//   it down flat on the ground ahead, held there SlamHoldS after SlamLandS before the usual sag or
//   recall. The game's charged triple slam (two more slams with the hand still) is not ours: the
//   limb never moves on its own.
inline constexpr float SlamRaisedY = 0.5f, SlamDownShare = 0.6f, SlamMps = 3.0f;
inline constexpr float SlamLandS = 0.25f, SlamHoldS = 0.3f;
// A chop's throw can go before the hand is coming down (at an earlier peak of the stroke, or as the
// stroke after a refused wind-up gets going): a lash thrown no more than SlamLateS before the chop
// is turned into the slam (2026-11-10: "Apply both"; with every throw at the stroke's
// start, as the study's proposal had it, none of his recorded chops slammed).
inline constexpr float SlamLateS = 0.15f;
// ... and only once your hand itself has come SlamHandDropM down from its highest in the stroke
// (walking left out). The user, 2026-11-10, "Yes, measure it": a wrist flick with the controller
// raised drives the controller's point down without the hand, and with the throw at the stroke's
// peak the old gate on that point (0.23 m) let 21 flicks slam on his recorded swings beside 12
// real chops. Of the 41 throws there a chop could slam in its SlamLateS (14 real overhead chops,
// the hand dropping 0.67-1.75 m over the stroke; 27 others, under 0.5 m), the hand had come
// down 0.11-1.19 m by then on the chops, 0-0.35 m on the others: at 0.2 m 13 of the 14 chops slam
// and 1 of the others (a 0.49 m stroke).
inline constexpr float SlamHandDropM = 0.2f;
// Whether a throw along `direction` (world) goes ahead of `heading` (world; zero: unknown).
inline bool ThrowAhead(B::Vec3 direction, B::Vec3 heading) {
    direction.y = 0.0f;
    heading.y = 0.0f;
    const float d = B::Length(direction), h = B::Length(heading);
    if (!(h > 0.5f) || !(d > 0.1f) || !std::isfinite(d) || !std::isfinite(h)) return true;
    return B::Dot(direction, heading) / (d * h) >= CoilAheadCos;
}

// The strike: the three joints' local rotations every SampleS, and when in it (s) the unfold
// starts, it cracks (the tip fastest), the carry on is over, the refold starts and it is back in.
struct Strike {
    static constexpr float SampleS = 1.0f / 60.0f;
    std::vector<std::array<B::Quat, 3>> q; // Dummy001..003, local
    std::vector<float> out;                // 0 folded .. 1 at its farthest, by its reach
    float unfold{0.0f}, crack{0.0f}, hold{0.0f}, refold{0.0f}, end{0.0f};
    B::Vec3 sweep{}; // the tip's way across the arm through the crack (part model space, unit)

    bool Valid() const {
        return q.size() >= 8 && out.size() == q.size() && unfold < crack && crack < hold &&
               hold <= refold && refold < end && end <= Length() + 1e-4f &&
               B::LengthSq(sweep) > 0.5f;
    }
    float Length() const { return q.empty() ? 0.0f : static_cast<float>(q.size() - 1) * SampleS; }
    std::array<B::Quat, 3> At(float s) const {
        const float x = std::clamp(s / SampleS, 0.0f, static_cast<float>(q.size() - 1));
        const int k = std::min(static_cast<int>(x), static_cast<int>(q.size()) - 2);
        const float f = x - static_cast<float>(k);
        std::array<B::Quat, 3> r{};
        for (int i = 0; i < 3; ++i)
            r[i] = B::Normalize(B::Slerp(q[static_cast<std::size_t>(k)][i],
                                         q[static_cast<std::size_t>(k + 1)][i], f));
        return r;
    }
    float OutAt(float s) const {
        const float x = std::clamp(s / SampleS, 0.0f, static_cast<float>(out.size() - 1));
        const int k = std::min(static_cast<int>(x), static_cast<int>(out.size()) - 2);
        const float f = x - static_cast<float>(k);
        return out[static_cast<std::size_t>(k)] +
               (out[static_cast<std::size_t>(k + 1)] - out[static_cast<std::size_t>(k)]) * f;
    }
    // The times and `out` from `q`, with the part's root `handle` and Dummy001..003's local
    // translations `t` (the clip's). The clip starts in the idle shape. The unfold starts where
    // the tip was last within 0.25 m of its idle reach before the crack; the carry on is over once
    // the tip is under 6 m/s (0.1 s past the crack at least); the refold starts once it has come
    // 0.1 m nearer than that; it is back in once every joint is within 12 degrees of the idle. Its
    // sweep: where the tip goes across the arm from CrackLeadS before the crack to the carry on's end
    // (at the crack itself it mostly whips in and out along its own length).
    bool Analyze(const B::QsTransform& handle, const std::array<B::Vec3, 3>& t) {
        const int n = static_cast<int>(q.size());
        if (n < 8) return false;
        const auto at = [](const std::vector<float>& v, int k) {
            return v[static_cast<std::size_t>(k)];
        };
        std::vector<B::Vec3> tip(static_cast<std::size_t>(n));
        std::vector<float> reach(static_cast<std::size_t>(n)), speed(static_cast<std::size_t>(n));
        for (int k = 0; k < n; ++k) {
            B::QsTransform m = handle;
            B::Vec3 root{};
            for (int i = 0; i < 3; ++i) {
                B::QsTransform local{};
                local.t = t[i];
                local.q = q[static_cast<std::size_t>(k)][i];
                m = B::Compose(m, local);
                if (i == 0) root = m.t;
            }
            tip[static_cast<std::size_t>(k)] = m.t + B::Rotate(B::Normalize(m.q), TipLocal);
            reach[static_cast<std::size_t>(k)] = B::Length(tip[static_cast<std::size_t>(k)] - root);
        }
        for (int k = 0; k < n; ++k) {
            const int a = std::max(k - 1, 0), c = std::min(k + 1, n - 1);
            speed[static_cast<std::size_t>(k)] =
                B::Length(tip[static_cast<std::size_t>(c)] - tip[static_cast<std::size_t>(a)]) /
                (static_cast<float>(c - a) * SampleS);
        }
        int c = 0;
        for (int k = 1; k < n; ++k)
            if (at(speed, k) > at(speed, c)) c = k;
        int u = c;
        while (u > 0 && at(reach, u) > at(reach, 0) + 0.25f) --u;
        int h = std::min(c + static_cast<int>(0.1f / SampleS + 0.5f), n - 1);
        while (h < n - 1 && at(speed, h) >= 6.0f) ++h;
        int r = h;
        while (r < n - 1 && at(reach, r) >= at(reach, h) - 0.1f) ++r;
        const auto off = [&](int k) {
            float most = 0.0f;
            for (int i = 0; i < 3; ++i) {
                const float d = std::abs(B::Dot(q[static_cast<std::size_t>(k)][i], q[0][i]));
                most = std::max(most, 2.0f * std::acos(std::min(d, 1.0f)));
            }
            return most;
        };
        int e = r + 1;
        while (e < n - 1 && off(e) > 12.0f * B::Deg) ++e;
        float most = at(reach, 0);
        for (int k = u; k <= r; ++k) most = std::max(most, at(reach, k));
        out.resize(static_cast<std::size_t>(n));
        for (int k = 0; k < n; ++k) {
            const float x = std::clamp((at(reach, k) - at(reach, 0)) /
                                           std::max(most - at(reach, 0), 1e-3f),
                                       0.0f, 1.0f);
            out[static_cast<std::size_t>(k)] = x * x * (3.0f - 2.0f * x);
        }
        const B::Vec3 arm = B::Normalize(
            B::Compose(handle, B::QsTransform{t[0], q[0][0], {1.0f, 1.0f, 1.0f}}).t - handle.t);
        const int a = std::max(c - static_cast<int>(CrackLeadS / SampleS + 0.5f), 0);
        const B::Vec3 swept = tip[static_cast<std::size_t>(h)] - tip[static_cast<std::size_t>(a)];
        sweep = B::Normalize(swept - arm * B::Dot(swept, arm));
        unfold = static_cast<float>(u) * SampleS;
        crack = static_cast<float>(c) * SampleS;
        hold = static_cast<float>(h) * SampleS;
        refold = static_cast<float>(r) * SampleS;
        end = static_cast<float>(std::min(e, n - 1)) * SampleS;
        return Valid();
    }
};

// Already mapped into game-world axes by the adapter. The rendered hand remains the
// attachment, but cannot inject its animation's angular velocity into a launch.
struct LaunchInput {
    B::Vec3 direction{}, velocity{};
    B::Vec3 heading{}; // the way you face (world; zero: unknown, a throw goes any way, as before)
};

class Motion {
public:
    // A new part or hunter: everything again, the neutral pose too.
    void Clear() {
        const Strike* const strike = strike_;
        *this = {};
        strike_ = strike;
    }
    // The game's strike a throw plays (null or not valid: the reach-out); kept by Clear.
    void SetStrike(const Strike* strike) {
        strike = strike != nullptr && strike->Valid() ? strike : nullptr;
        if (strike != strike_) strike_stretch_ = 0.0f;
        strike_ = strike;
    }
    // A gap (a stall, the body not driven): the chain starts again; the neutral pose is kept.
    // The game may still be showing our last bend, and capturing that would keep it bent.
    void Pause() {
        running_ = tracked_ = have_last_ = have_track_ = out_ = playing_ = false;
        jab_ = latched_ = trigger_ = false;
        fade_ = lash_ = hang_w_ = 0.0f;
        jab_wait_ = -1.0f;
        was_out_ = false;
        recall_lash_ = droop_ = 0.0f;
        have_sag_ = skip_sag_ = false;
        sag_s_ = HangS;
        recall_s_ = LashInS;
        stroke_s_ = stroke_m_ = peak_ = fast_ = 0.0f;
        stroke_samples_ = 0;
        extended_attack_ = false;
        since_full_ = -1.0f;
        hard_calm_ = open_v_ = hand_peak_ = hand_peak_age_ = full_wait_ = 0.0f;
        open_ = 1.0f;
        slam_ = false;
        have_leg_heading_ = false;
        stroke_raise_ = -1.0f;
        coiled_ = stroke_hand_y_ = stroke_hand_top_ = 0.0f;
    }
    // 0 folded .. 1 out at full reach (the lash).
    float Lash() const {
        return playing_ ? std::max({strike_->OutAt(tau_), hang_w_, 0.01f}) : lash_;
    }
    // A throw playing the game's strike, and how far into it (s of the clip).
    bool Striking() const { return playing_; }
    // Slammed down onto the ground ahead (a chop), until it has lain there SlamHoldS.
    bool Slamming() const { return slam_; }
    // Out or on its way (a strike, a jab, a hook or folding back): its arm is ours, not a clip's.
    bool Out() const { return playing_ || out_ || lash_ > 0.0f; }
    bool HasNeutralPose() const { return have_neutral_; }
    float StrikeTime() const { return tau_; }
    float StrikeTurn() const { return turn_now_; } // rad about the arm
    // Locomotion/turning moves the reference frame, not the player's physical swing.
    void Carry(B::Quat rotation, B::Vec3 from, B::Vec3 to) {
        if (!running_ || !B::IsFinite(rotation) || !B::IsFinite(from) || !B::IsFinite(to)) return;
        rotation = B::Normalize(rotation);
        chain_.Carry(rotation, from, to);
        previous_.t = to + B::Rotate(rotation, previous_.t - from);
        previous_.q = B::Normalize(rotation * previous_.q);
    }

    // The idle clip: the tentacle swings on the hand, a swing throws it out (the strike; an
    // attack's clip too: the throw takes the arm over from it); `lash`: the trigger is held (a
    // lighter swing throws it, pressed with the hand still it jabs); `hooked`: the point its tip
    // holds on an enemy (world; null: none), which keeps it out that far. `floor_y`: the
    // ground under the hunter (world y; below -1e8: none), which the lashing tentacle never goes
    // through. `out`: the pose to write (false: none).
    bool Update(const Pose& sampled, const W::Frame& grip, float dt, Pose& out, bool lash = false,
                float floor_y = -1e9f, const B::Vec3* hooked = nullptr,
                float controller_speed = -1.0f, const LaunchInput* controller_launch = nullptr) {
        out = sampled;
        if (!Valid(sampled, grip, dt)) {
            Clear();
            return false;
        }
        // Captured once per part, never again after a gap or an attack (above).
        if (!have_neutral_ && !Capture(sampled)) return false;
        out = neutral_;
        if (!running_) {
            // From the attack that just ended (its follow-through, with its motion), else at rest
            // on the hand.
            std::array<B::Vec3, 4> points{};
            running_ = true;
            if (tracked_ && Points(tracked_pose_, tracked_grip_, points)) {
                chain_.ResetTo(points.data(), 1.0f);
                for (int i = 1; i <= 3; ++i) chain_.SetVelocity(i, tracked_velocity_[i]);
                previous_ = tracked_grip_;
            } else {
                chain_.Reset(grip);
                previous_ = grip;
            }
            tracked_ = false;
            lash_ = rest_w_ = 0.0f;
            out_ = false;
            chain_.SetRest(fold_.data());
        }
        // Loads, tracking jumps and snap turns carry the arm rather than flinging it. The jump
        // allowed grows with the frame's time: at 30 fps a fast wrist flick (1400 deg/s) turns
        // the hand 47 degrees in one frame and must still swing the tentacle.
        const bool jumped =
            dt <= 0.0f || dt > 0.1f ||
            B::Length(grip.t - previous_.t) > std::max(JumpM, JumpMps * dt) ||
            B::AngleBetween(grip.q, previous_.q) > std::max(JumpDeg, JumpDegS * dt) * B::Deg;
        // The hand's swing this frame (the hunter's walking and turning are carried, not swung).
        float swing = 0.0f;
        B::Vec3 spin{}, hand_v{};
        if (!jumped) {
            hand_v = (grip.t - previous_.t) * (1.0f / dt);
            const B::Quat turn = B::Normalize(grip.q * B::Conjugate(previous_.q));
            const float angle = B::AngleOf(turn);
            const B::Vec3 axis{turn.x, turn.y, turn.z};
            if (angle > 1e-4f && B::Length(axis) > 1e-6f)
                spin = B::Normalize(axis) * ((turn.w < 0.0f ? -angle : angle) / dt);
            // Rolling the handle about its own shaft is not a whipping stroke.
            const B::Vec3 shaft = B::Rotate(grip.q, along_);
            swing = std::max(B::Length(hand_v), B::Length(B::Cross(spin, shaft)) * HandTurnM);
        }
        // Runtime decisions use tracked controller intent, never a twitch of the rendered hand.
        // The default keeps stand-alone motion simulations independent of the presenter.
        if (controller_speed >= 0.0f || !std::isfinite(controller_speed))
            swing = !jumped && std::isfinite(controller_speed) ? controller_speed : 0.0f;
        const bool launch_valid = controller_launch && B::IsFinite(controller_launch->direction) &&
                                  B::IsFinite(controller_launch->velocity) &&
                                  B::LengthSq(controller_launch->direction) > 0.25f &&
                                  B::LengthSq(controller_launch->direction) < 2.25f &&
                                  B::LengthSq(controller_launch->velocity) <= 225.3f;
        const bool can_launch = !controller_launch || launch_valid;
        const auto launch_direction = [&] {
            return launch_valid ? B::Normalize(controller_launch->direction)
                                : B::Rotate(grip.q, along_);
        };
        const float stroke_peak = std::max(peak_, swing);
        if (!jumped && swing >= FireMps * RearmShare) {
            stroke_s_ = std::min(FreeStrokeS, stroke_s_ + dt);
            stroke_m_ = std::min(FreeStrokeM, stroke_m_ + swing * dt);
            stroke_samples_ = std::min(stroke_samples_ + 1u, 2u);
        }
        const bool deliberate = lash || (!jumped && stroke_samples_ >= 2 &&
                                         stroke_s_ >= FreeStrokeS && stroke_m_ >= FreeStrokeM);
        if (swing < FireMps * RearmShare) {
            rearmed_ = true;
            peak_ = fast_ = 0.0f;
            stroke_s_ = stroke_m_ = 0.0f;
            stroke_samples_ = 0;
        } else {
            peak_ = std::max(peak_, swing);
            if (swing >= FireMps) fast_ += dt;
        }
        if (!jumped) {
            calm_ = swing < HangMps ? calm_ + dt : 0.0f;
            still_ = swing < JabStillMps ? still_ + dt : 0.0f;
            hard_calm_ = swing >= HardSwingMps ? 0.0f : hard_calm_ + dt;
            // The hand's fastest over about the last SagPeakS (held, then let go to the hand's now).
            hand_peak_age_ += dt;
            if (swing >= hand_peak_ || hand_peak_age_ > SagPeakS) {
                hand_peak_ = swing;
                hand_peak_age_ = 0.0f;
            }
            // How high the controller's line has been in this stroke (a chop starts raised), and
            // your hand's height in it from where it started (walking is carried, so left out) and
            // its highest.
            if (swing < FireMps * RearmShare) {
                stroke_raise_ = -1.0f;
                stroke_hand_y_ = stroke_hand_top_ = 0.0f;
            } else {
                if (launch_valid)
                    stroke_raise_ =
                        std::max(stroke_raise_, B::Normalize(controller_launch->direction).y);
                if (dt > 0.0f && dt <= 0.1f && std::isfinite(hand_v.y)) {
                    stroke_hand_y_ += hand_v.y * dt;
                    stroke_hand_top_ = std::max(stroke_hand_top_, stroke_hand_y_);
                }
            }
        }
        const bool release =
            stroke_peak >= FireMps && (swing <= ReleaseShare * stroke_peak || fast_ >= FireHoldS);
        const bool on = lash && hooked != nullptr && B::IsFinite(*hooked);
        // The hand's way: its motion carrying a point 2 m out along the arm, across the arm (grip
        // space), over the last AimS (the faster, the more it counts).
        if (!jumped) {
            const B::Vec3 reach_at = W::Apply(grip, fold_[0] + along_ * 2.0f);
            B::Vec3 way =
                B::Rotate(B::Conjugate(grip.q), hand_v + B::Cross(spin, reach_at - grip.t));
            way = way - along_ * B::Dot(way, along_);
            way_ = way_ + (way - way_) * (1.0f - std::exp(-dt / AimS));
        }
        // The turn about the arm so that the strike cracks that way (the tip's way through it).
        const auto aim = [&](float& turn) {
            B::Vec3 crack = strike_->sweep - along_ * B::Dot(strike_->sweep, along_);
            if (B::Length(way_) < 0.3f || B::Length(crack) < 1e-3f) return;
            const B::Vec3 way = B::Normalize(way_);
            crack = B::Normalize(crack);
            turn = std::atan2(B::Dot(B::Cross(crack, way), along_), B::Dot(crack, way));
        };
        // The trigger pressed with the hand still: a jab, unless a swing starts in JabWaitS.
        const bool pressed = lash && !trigger_;
        trigger_ = lash;
        if (can_launch && !jumped && pressed && !out_ && !playing_ && lash_ <= 0.0f &&
            still_ >= JabCalmS)
            jab_wait_ = JabWaitS;
        if (jab_wait_ >= 0.0f) {
            if (jumped || !can_launch || !lash || on || swing >= FireMps * RearmShare) {
                jab_wait_ = -1.0f;
            } else if (dt > 0.0f && dt <= 0.1f && (jab_wait_ -= dt) < 0.0f) {
                out_ = jab_ = true;
                latched_ = false;
                thrown_ = 0.0f;
                reach_ = jab_stretch_;
                const B::Vec3 along = launch_direction();
                for (int i = 1; i <= 3; ++i)
                    chain_.SetVelocity(i, chain_.Velocity(i) + along * (LaunchMps * i / 3.0f));
            }
        }
        const float fire = lash ? FireMps : FreeFireMps;
        if (can_launch && deliberate && strike_ == nullptr && !jumped && !on && rearmed_ &&
            swing >= fire && (jab_ || (!out_ && lash_ > 0.0f))) {
            // A deliberate swing after a jab carries the existing extension on as a lash.
            // It must not finish the jab's timer and fold in during the new stroke.
            jab_ = false;
            out_ = true;
            rearmed_ = false;
            // Preserve the current length; grow the extra half metre over JabToLashS below.
            thrown_ = calm_ = 0.0f;
        }
        const float lead = strike_ != nullptr ? std::max(strike_->crack - CrackLeadS, strike_->unfold)
                                              : 0.0f;
        if (deliberate && !on && !out_ && lash_ <= 0.0f && rearmed_ &&
            strike_ != nullptr && swing >= fire) {
            // The game's strike, from the swing's start.
            out_ = playing_ = true;
            rearmed_ = aimed_ = jab_ = latched_ = false;
            thrown_ = calm_ = tau_ = hang_w_ = turn_ = 0.0f;
            rate_ = MinSwingRate;
            aim(turn_);
            turn_now_ = turn_;
            if (strike_stretch_ <= 0.0f) strike_stretch_ = StrikeStretch();
        } else if (deliberate && !on && rearmed_ && strike_ != nullptr && swing >= fire &&
                   ((playing_ && tau_ >= strike_->hold) || (jab_ && lash_ >= 0.5f))) {
            // A new stroke with it out (carried on round, or jabbed): cracked again from there,
            // aimed anew (out straight, its turn about itself unseen).
            if (!playing_) hang_w_ = lash_;
            out_ = playing_ = true;
            rearmed_ = aimed_ = jab_ = false;
            thrown_ = calm_ = 0.0f;
            tau_ = lead;
            lash_ = blend_ = 0.0f;
            for (int i = 0; i < 4; ++i) from_[i] = W::ApplyInverse(previous_, chain_.Point(i));
            aim(turn_);
            if (hang_w_ >= 0.9f) turn_now_ = turn_;
            if (strike_stretch_ <= 0.0f) strike_stretch_ = StrikeStretch();
        }
        if (playing_ && !aimed_ && !jumped) {
            // Aimed until CrackLeadS before the crack, its speed taken from the swing's fastest.
            aim(turn_);
            rate_ = std::max(rate_, std::clamp(peak_ / SwingRateMps, MinSwingRate, MaxSwingRate));
            aimed_ = tau_ >= lead;
        }
        if (playing_ && dt > 0.0f && dt <= 0.1f) {
            const float gap = std::remainder(turn_ - turn_now_, 2.0f * B::Pi);
            const float most = TurnDegS * B::Deg * dt;
            turn_now_ = std::remainder(turn_now_ + std::clamp(gap, -most, most), 2.0f * B::Pi);
        }
        // A chop: the controller's line raised in this stroke, the hand going down hard (a slam).
        const float launch_speed = launch_valid ? B::Length(controller_launch->velocity) : 0.0f;
        const bool chop = launch_valid && stroke_raise_ >= SlamRaisedY && launch_speed >= SlamMps &&
                          controller_launch->velocity.y <= -SlamDownShare * launch_speed &&
                          stroke_hand_top_ - stroke_hand_y_ >= SlamHandDropM;
        // Coiled on a wind-up: thrown only ahead of you (a chop lands ahead, whatever its start).
        const bool ahead = !launch_valid || chop ||
                           ThrowAhead(controller_launch->direction, controller_launch->heading);
        const bool could = can_launch && strike_ == nullptr && !jumped && !on && !out_ &&
                           lash_ <= 0.0f && rearmed_ && deliberate &&
                           (lash || stroke_peak >= FreeFireMps);
        // A free throw at the stroke's peak (release), as always. Coiled: a wind-up refused, the
        // stroke ahead after it throws as it gets going (not at its peak), so the limb is out for
        // that stroke's fastest moment.
        if (dt > 0.0f && dt <= 0.1f) coiled_ = std::max(0.0f, coiled_ - dt);
        if (could && release && !ahead) coiled_ = CoilHoldS;
        if (could && ahead && (release || (coiled_ > 0.0f && swing >= fire && !chop))) {
            coiled_ = 0.0f;
            // Physical swing: reach out with the hand's momentum, without a canned loop.
            out_ = true;
            rearmed_ = jab_ = latched_ = false;
            thrown_ = calm_ = 0.0f;
            reach_ = throw_stretch_;
            since_full_ = -1.0f;
            open_ = 1.0f;
            open_v_ = 0.0f;
            slam_ = chop;
            slam_t_ = 0.0f;
            if (chop)
                slam_dir_ = SlamDirection(launch_direction(), controller_launch->heading, grip,
                                          floor_y);
            const B::Vec3 along = launch_direction();
            const B::Vec3 root = W::Apply(grip, straight_[0]);
            for (int i = 1; i <= 3; ++i) {
                const B::Vec3 swung = controller_launch
                    ? (launch_valid ? controller_launch->velocity * (i / 3.0f) : B::Vec3{})
                    : B::Cross(spin, W::Apply(grip, straight_[i]) - root);
                chain_.SetVelocity(i, chain_.Velocity(i) + along * (LaunchMps * i / 3.0f) + swung);
            }
        }
        // A lash thrown just before its stroke became a chop: the slam from here.
        if (chop && strike_ == nullptr && out_ && !slam_ && !jab_ && !latched_ && !on &&
            !playing_ && !jumped && thrown_ <= SlamLateS) {
            slam_ = true;
            slam_t_ = 0.0f;
            slam_dir_ = SlamDirection(launch_direction(), controller_launch->heading, grip, floor_y);
        }
        if (strike_ == nullptr && out_ && !jab_ && !latched_ && dt > 0 && dt <= 0.1f)
            reach_ = std::min(throw_stretch_, reach_ +
                (throw_stretch_ - jab_stretch_) * dt / JabToLashS);
        if (on && playing_) {
            // Hooked mid-strike: held out to it from here (below), straight.
            playing_ = false;
            hang_w_ = 0.0f;
            lash_ = 1.0f;
        }
        // The payout timer is not proof that the physical chain actually straightened.
        // Only a lash that reached near its full root-to-tip span may have a sag phase.
        if (!out_ || jab_ || on || latched_ || jumped) {
            extended_attack_ = false;
        } else if (!playing_ && lash_ >= 1.0f &&
                   B::Length(chain_.Point(3) - chain_.Point(0)) >=
                       0.85f * B::Length(straight_[3] - straight_[0])) {
            extended_attack_ = true;
        }
        // Capture remaining motion once at the slowdown, rather than chasing a changing
        // deadline as the spring settles. Jabs and attached enemies keep their existing rules.
        if (!out_ || jab_ || on || latched_ || swing >= HangMps) {
            have_sag_ = skip_sag_ = false;
            sag_s_ = HangS;
        } else if (!playing_ && lash_ >= 1.0f && !have_sag_ && !jumped && !slam_) {
            // Judged on how hard you just swung (SagPeakS), not on the limb's own tip.
            sag_s_ = extended_attack_ ? SagSeconds(hand_peak_) : 0.0f;
            if (sag_s_ > 0.0f) {
                const float variation = SagVariation(sag_random_);
                skip_sag_ = variation <= 0.0f;
                sag_s_ = skip_sag_ ? SkipHoldS : sag_s_ * variation;
            }
            have_sag_ = true;
        }
        if (out_)
            recall_s_ = !jab_ && !on && have_sag_ && (sag_s_ <= 0.0f || skip_sag_)
                ? NoSagRecallS : LashInS;
        if (on) {
            // Hooked: held out to the enemy; farther than its own length, the middle links stretch
            // to the hook's distance from the handle (taut, up to TautStretch).
            out_ = latched_ = true;
            const float d = B::Length(W::ApplyInverse(grip, *hooked) - neutral_[0].t);
            if (std::isfinite(d)) reach_ = std::clamp((d - ends_) / middle_, 1.0f, TautStretch);
        } else if (!playing_ && out_ &&
                   (jab_ ? thrown_ >= LashOutS + JabHoldS
                         : latched_ || (lash_ >= 1.0f && calm_ > 0.0f && calm_ >= sag_s_ &&
                                        !slam_ && have_sag_))) {
            // The jab held, the hook let go, or it missed and the swing is over: back in (a jab
            // still one until in, for a swing to crack it from there).
            out_ = latched_ = false;
        }
        if (dt > 0.0f && dt <= 0.1f) {
            if (out_) thrown_ += dt;
            if (!playing_)
                lash_ = out_ ? std::min(1.0f, lash_ + dt / LashOutS)
                             : std::max(0.0f, lash_ - dt / recall_s_);
            if (!out_ && lash_ <= 0.0f) jab_ = false;
            // The slam lands in SlamLandS and lies there SlamHoldS.
            if (slam_ && (!out_ || on || jab_ || (slam_t_ += dt) >= SlamLandS + SlamHoldS))
                slam_ = false;
            // The crack held CrackHoldS, then drawn in to the bent leg unless swung hard: a spring
            // in (DrawHz, DrawZeta), back out at the throw's pace.
            const bool lashing = out_ && !jab_ && !on && !latched_ && !playing_;
            // Held from when the limb itself is out at its full reach (or CrackWaitS after its rest
            // shape got there), so the draw in never cuts the crack short.
            full_wait_ = lashing && lash_ >= 1.0f ? full_wait_ + dt : 0.0f;
            const bool cracked = B::Length(chain_.Point(3) - chain_.Point(0)) >=
                                     CrackSpanShare * B::Length(straight_[3] - straight_[0]) ||
                                 full_wait_ >= CrackWaitS;
            since_full_ = !lashing ? -1.0f
                : since_full_ >= 0.0f ? since_full_ + dt : lash_ >= 1.0f && cracked ? 0.0f : -1.0f;
            // (Folding back in it keeps the shape it had; a jab or a hook is out straight.)
            const bool full =
                !lashing || slam_ || since_full_ < CrackHoldS || hard_calm_ < CrackHoldS;
            if (!out_) {
                open_v_ = 0.0f;
            } else if (full) {
                open_ = std::min(1.0f, open_ + (1.0f - work_open_) * dt / LashOutS);
                open_v_ = 0.0f;
            } else {
                const float wn = 2.0f * B::Pi * DrawHz;
                for (float left = dt; left > 1e-6f;) {
                    const float h = std::min(left, 1.0f / 240.0f);
                    open_v_ += (wn * wn * (work_open_ - open_) - 2.0f * DrawZeta * wn * open_v_) * h;
                    open_ = std::clamp(open_ + open_v_ * h, std::max(0.0f, work_open_ - 0.25f),
                                       1.0f);
                    left -= h;
                }
            }
        }
        if (playing_ && !Play(dt, calm_ >= HangS)) {
            // Back in: the idle again.
            playing_ = out_ = false;
            lash_ = rest_w_ = hang_w_ = 0.0f;
            reach_ = rest_reach_ = 1.0f;
            chain_.SetRest(fold_.data());
        }
        const float w = lash_ * lash_ * (3.0f - 2.0f * lash_);
        const float droop = out_ && !jab_ && !on && extended_attack_ &&
                            have_sag_ && sag_s_ > 0.0f && !skip_sag_ && lash_ >= 1.0f
                                ? std::clamp((calm_ - 0.04f) / std::max(sag_s_ - 0.04f, 0.001f),
                                             0.0f, 1.0f)
                                : 0.0f;
        // Slamming: at the ground ahead (world), else along the controller's line; a lash (not a
        // jab or a hook) held up like a leg, never aimed more than LegMinPitchDeg below level (its
        // sag, the droop below, still takes a spent one down).
        B::Vec3 line = launch_valid ? B::Normalize(controller_launch->direction) : B::Vec3{};
        if (launch_valid && out_ && !jab_ && !latched_) {
            const float lowest = -std::sin(LegMinPitchDeg * B::Deg);
            // The line's heading, kept from the last frame it pointed clearly out (the controller
            // nearly straight down, after a chop that did not slam, has no heading: noise, and
            // through vertical it flips behind you; review, 2026-11-10).
            const B::Vec3 flat{line.x, 0.0f, line.z};
            const float flat_len = B::Length(flat);
            if (flat_len >= LegHeadingMinFlat) {
                leg_heading_ = flat * (1.0f / flat_len);
                have_leg_heading_ = true;
            }
            if (line.y < lowest) {
                const B::Vec3 level = have_leg_heading_
                                          ? leg_heading_
                                          : B::Normalize(flat, B::Vec3{1, 0, 0});
                line = level * std::sqrt(1.0f - lowest * lowest);
                line.y = lowest;
            }
        }
        const B::Vec3 aim_axis = slam_ && out_ && !on
            ? B::Rotate(B::Conjugate(grip.q), slam_dir_)
            : launch_valid && out_ && !on ? B::Rotate(B::Conjugate(grip.q), line) : along_;
        // How far it is open: out (w) and, once the crack is held, drawn in to the bent leg (open_).
        const float oe = w * open_;
        if (!playing_ && (w != rest_w_ || oe != rest_oe_ || reach_ != rest_reach_ ||
                         droop != droop_ || B::LengthSq(aim_axis - rest_aim_) > 1e-10f)) {
            auto rest = RestAt(oe, reach_, oe > rest_oe_ && rest_oe_ > 0.0f ? &rest_seed_ : nullptr);
            const auto seed = rest; // next unfold starts before controller aim and sag rotations
            // Extend toward the tracked weapon axis. Only the root stays attached to the
            // rendered grip; a rig twist must not redirect the jab's target or spring pull.
            const B::Quat aim_turn = B::ScaleRotation(B::FromTo(along_, aim_axis), w);
            const B::Vec3 aimed = B::Rotate(aim_turn, along_);
            for (int i = 1; i < 4; ++i)
                rest[i] = rest[0] + B::Rotate(aim_turn, rest[i] - rest[0]);
            // Let the spent lash tip down before its quick recall. This moves the spring's
            // target, never the visible bones directly; gravity and damping still do the work.
            if (droop > 0.0f) {
                const B::Vec3 down = B::Rotate(B::Conjugate(grip.q), {0, -1, 0});
                B::Quat bend = B::ScaleRotation(B::FromTo(aimed, down), droop * 0.45f);
                // Do not keep pulling the rest target through the floor. That fights the
                // chain's ground constraint and can fold the rendered tip underneath it.
                const float length = B::Length(rest[3] - rest[0]);
                const auto root = W::Apply(grip, rest[0]);
                B::Vec3 direction = B::Rotate(grip.q, B::Rotate(bend, aimed));
                const float min_y = std::clamp((floor_y + 0.10f - root.y) /
                                                  std::max(length, 0.001f), -1.0f, 1.0f);
                if (direction.y < min_y) {
                    const auto horizontal = B::Normalize(B::Vec3{direction.x, 0, direction.z},
                                                         B::Vec3{1, 0, 0});
                    direction = horizontal * std::sqrt(std::max(0.0f, 1.0f - min_y * min_y));
                    direction.y = min_y;
                    bend = B::FromTo(aimed, B::Rotate(B::Conjugate(grip.q), direction));
                }
                for (int i = 1; i < 4; ++i)
                    rest[i] = rest[0] + B::Rotate(bend, rest[i] - rest[0]);
            }
            if (!out_ && was_out_) {
                for (int i = 0; i < 4; ++i) recall_[i] = W::ApplyInverse(grip, chain_.Point(i));
                recall_lash_ = std::max(lash_, 0.001f);
            }
            if (!out_ && lash_ > 0.0f && recall_lash_ > 0.0f) {
                const float t = 1.0f - std::clamp(lash_ / recall_lash_, 0.0f, 1.0f);
                const float blend = t * t * (3.0f - 2.0f * t);
                rest[0] = fold_[0];
                for (int i = 1; i < 4; ++i) {
                    const B::Vec3 a = recall_[i] - recall_[i - 1];
                    const B::Vec3 b = fold_[i] - fold_[i - 1];
                    const float length =
                        B::Length(b) * (i < 3 ? 1.0f + (reach_ - 1.0f) * oe : 1.0f);
                    rest[i] = rest[i - 1] + B::Rotate(B::ScaleRotation(B::FromTo(a, b), blend),
                                                     B::Normalize(a, along_)) * length;
                }
            }
            if (chain_.SetRest(rest.data())) {
                rest_ = rest;
                rest_w_ = w;
                rest_oe_ = oe;
                rest_reach_ = reach_;
                rest_aim_ = aim_axis;
                rest_seed_ = seed;
            }
        }
        droop_ = droop;
        was_out_ = out_;
        if (jumped) {
            chain_.Reset(grip);
            previous_ = grip;
            Keep(out);
            return true;
        }
        if (playing_) {
            // The strike followed closely; swung round out straight, loose (the hang).
            W::ChainConfig c = hang_w_ >= 0.5f ? HangConfig() : StrikeConfig();
            c.floor_y = floor_y;
            chain_.Step(previous_, grip, dt, 1.0f, c);
        } else if (lash_ > 0.0f) {
            // Fully out and swung round (not hooked), its joints give a little more; live (your
            // hand moving) it holds itself up like a leg, spent it hangs its weight (the sag).
            // Slamming, it is pulled hard onto the ground ahead.
            W::ChainConfig c = !slam_ && out_ && !on && lash_ >= 1.0f
                ? (calm_ <= 0.0f ? LegConfig() : HangConfig()) : LashConfig();
            c.floor_y = floor_y;
            chain_.Step(previous_, grip, dt, 1.0f, c);
        } else {
            chain_.Step(previous_, grip, dt, 1.0f, Config());
        }
        previous_ = grip;

        if (playing_) {
            // Drawn around the strike's own pose (its twist too), each joint turned onto the chain.
            B::QsTransform parent = neutral_[0];
            for (int i = 1; i <= 3; ++i) {
                const B::Vec3 wanted = W::ApplyInverse(grip, chain_.Point(i)) -
                                       W::ApplyInverse(grip, chain_.Point(i - 1));
                const auto current = B::Compose(parent, ref_[i]);
                const auto direction = B::Rotate(current.q, i < 3 ? ref_[i + 1].t : TipLocal);
                B::Quat turn = B::FromTo(direction, wanted);
                const float angle = B::AngleOf(turn);
                if (angle > 175.0f * B::Deg) turn = B::ScaleRotation(turn, 175.0f * B::Deg / angle);
                out[i].t = ref_[i].t;
                out[i].q = B::Normalize(B::Conjugate(parent.q) * turn * current.q);
                parent = B::Compose(parent, out[i]);
            }
            for (const auto& bone : out) {
                if (!B::IsFinite(bone)) {
                    Pause();
                    out = neutral_;
                    return false;
                }
            }
            Keep(out);
            return true;
        }
        // Out, the joints straighten the fold (each turned up to 175 degrees from the idle) and
        // the two middle links stretch as the chain's do.
        // (Drawn in, the middle links ease back toward their own length with the chain's.)
        const float stretch = 1.0f + (reach_ - 1.0f) * oe;
        out[2].t = neutral_[2].t * stretch;
        out[3].t = neutral_[3].t * stretch;
        B::QsTransform parent = neutral_[0];
        for (int i = 1; i <= 3; ++i) {
            const B::Vec3 wanted = W::ApplyInverse(grip, chain_.Point(i)) -
                                   W::ApplyInverse(grip, chain_.Point(i - 1));
            const auto current = B::Compose(parent, neutral_[i]);
            const auto direction = B::Rotate(current.q, i < 3 ? neutral_[i + 1].t : TipLocal);
            B::Quat turn = B::FromTo(direction, wanted);
            const float angle = B::AngleOf(turn);
            const float idle = (i == 1 ? 65.0f : i == 2 ? 90.0f : 100.0f) * B::Deg;
            const float limit = idle + (175.0f * B::Deg - idle) * w;
            if (angle > limit) turn = B::ScaleRotation(turn, limit / angle);
            out[i].q = B::Normalize(B::Conjugate(parent.q) * turn * current.q);
            parent = B::Compose(parent, out[i]);
        }
        for (const auto& bone : out) {
            if (!B::IsFinite(bone)) {
                Pause();
                out = neutral_;
                return false;
            }
        }
        Keep(out);
        return true;
    }

    // An attack's frames (the game's clip has the arm): the bend the idle left fades out over
    // FadeS on top of the clip instead of snapping, and the clip's pose is kept for the chain to
    // take over from when the idle comes back. `out`: the pose to write (false: the clip's own).
    bool Attack(const Pose& sampled, const W::Frame& grip, float dt, Pose& out) {
        out = sampled;
        if (!Valid(sampled, grip, dt)) {
            Pause();
            return false;
        }
        if (running_) {
            // The idle just ended: our bend as offsets from the neutral pose, at full (and a
            // lash's stretch, reeled into the clip's lengths with it).
            for (int i = 1; i <= 3; ++i)
                fade_q_[i - 1] = B::Normalize(last_[i].q * B::Conjugate(neutral_[i].q));
            fade_t_ = {last_[2].t, last_[3].t};
            fade_stretch_ = lash_ > 0.0f; // else the clip's own lengths at once (the game's)
            fade_ = have_last_ ? 1.0f : 0.0f;
            running_ = have_last_ = false;
            have_track_ = false;
            lash_ = hang_w_ = 0.0f;
            out_ = playing_ = jab_ = latched_ = false;
            jab_wait_ = -1.0f;
        } else if (dt > 0.0f) {
            fade_ = std::max(0.0f, fade_ - dt / FadeS);
        }
        // The clip's motion, for a lash or the follow-through taking over from it mid-swing.
        std::array<B::Vec3, 4> points{};
        if (Points(sampled, grip, points)) {
            for (int i = 0; i < 4; ++i)
                tracked_velocity_[i] = have_track_ && dt > 0.0f && dt <= 0.1f
                                           ? (points[i] - track_points_[i]) * (1.0f / dt)
                                           : B::Vec3{};
            track_points_ = points;
            have_track_ = true;
        }
        tracked_pose_ = sampled;
        tracked_grip_ = grip;
        tracked_ = true;
        if (fade_ <= 0.0f) return false;
        const float w = fade_ * fade_ * (3.0f - 2.0f * fade_);
        for (int i = 1; i <= 3; ++i)
            out[i].q = B::Normalize(B::ScaleRotation(fade_q_[i - 1], w) * sampled[i].q);
        if (fade_stretch_) {
            out[2].t = sampled[2].t + (fade_t_[0] - sampled[2].t) * w;
            out[3].t = sampled[3].t + (fade_t_[1] - sampled[3].t) * w;
        }
        return true;
    }

    // Only the confirmed B->A clip may finish a bend after the form flag changes.
    // Its folded pose must not seed the simulation when the arm unfolds again.
    bool Close(const Pose& sampled, const W::Frame& grip, float dt, Pose& out) {
        if (!std::isfinite(dt) || dt < 0.0f || dt > 0.1f) {
            Pause();
            out = sampled;
            return false;
        }
        const bool write = Attack(sampled, grip, dt, out);
        tracked_ = false;
        return write;
    }

private:
    static bool Valid(const Pose& sampled, const W::Frame& grip, float dt) {
        if (!B::IsFinite(grip.t) || !B::IsFinite(grip.q) || !std::isfinite(dt)) return false;
        for (const auto& bone : sampled)
            if (!B::IsFinite(bone)) return false;
        return true;
    }
    // Built once (the per-frame step reads it).
    static const W::ChainConfig& Config() {
        static const W::ChainConfig c = [] {
            W::ChainConfig k{};
            // Organic, like the game's own form-B clips (wp_a_1701_2's a142_04xxxx, sampled from
            // the user's files, 2026-11-06): the joints move in long smooth arcs, the one at the
            // hand first and the tip's after (0.05-0.15 s), and settle with one soft overshoot
            // (about 10 percent, still in 0.3-0.9 s: a joint spring near 1.6 Hz, damping ratio about
            // 0.6). So a soft pull (the gap closed per 1/240 s; about 15 rad/s at the root, 10 at
            // the tip) with the motion damped toward the hand's own (idle_velocity_damp) rather
            // than in the world: probed, a 90 degree swing at 300 degrees/s bends the joints 3-4
            // degrees, the outer one 0.25 s after the first, one follow-through, still in 0.25 s.
            // 0.018 / 0.004 with world drag only buzzed at 5 Hz (damping ratio about 0.1: three
            // swings back and forth, jelly); 0.08 / 0.03 with heavy drag (11 Hz) barely bent and
            // snapped still in one frame (a stick).
            k.k_root = 0.006f;
            k.k_tip = 0.002f;
            k.damp = 2.0f;
            k.gravity = 3.0f;
            k.idle_velocity_damp = 18.0f;
            // The idle holds the tentacle folded (its links turn 161 and 145 degrees at the two
            // joints): no straightening pass, and links may turn up to 170 degrees, so the chain
            // can rest in that shape (2026-11-06: at 110 degrees with a 0.03 pass it unfolded
            // about a metre off it with the hand still, the middle joint at its 90 degree clamp).
            k.bend = 0.0f;
            k.bend_iters = 0;
            k.max_bend = 170.0f * B::Deg;
            k.max_dt = 0.1f;
            k.max_substeps = 24;
            return k;
        }();
        return c;
    }
    // Out (and folding back): firm enough to follow the reach out in LashOutS, its motion damped
    // toward the hand's so the fast unfold leaves no one-frame jags (with 6 they were 30-50
    // degrees); folding back the joints go in the game's order with one soft overshoot.
    static const W::ChainConfig& LashConfig() {
        static const W::ChainConfig c = [] {
            W::ChainConfig k = Config();
            k.k_root = 0.12f;
            k.k_tip = 0.04f;
            k.damp = 4.0f;
            k.idle_velocity_damp = 25.0f;
            k.ftl_damp = 0.4f;
            return k;
        }();
        return c;
    }
    // Hanging out, swung round (HangS): the joints give a lot more and its motion is damped far
    // less toward the hand's, so it flaps and the tip curls after the swing and carries on past a
    // stop (the headset, 2026-11-06: "less force in a swing for it to move and flap"; 0.05 /
    // 0.015 with 4 toward the hand moved it as a stiff spoke).
    static const W::ChainConfig& HangConfig() {
        static const W::ChainConfig c = [] {
            W::ChainConfig k = LashConfig();
            // Keep the limb's restoring force: weight should not make it floppy.
            k.k_root = 0.03f;
            k.k_tip = 0.008f;
            k.gravity = 3.6f;
            k.damp = 2.0f;
            k.idle_velocity_damp = 2.0f;
            // As on Beast Cutter, dissipate relative link wobble rather than all swing
            // momentum. Much gentler here: only three moving links, no extra constraints.
            k.link_velocity_damp = 6.0f;
            return k;
        }();
        return c;
    }
    // The slam's way down (world): flat on the ground ahead, the way the controller points (`line`,
    // level), else your heading, pitched down for the straight limb to reach the ground from the
    // hand (`floor_y` below -1e8: none known, a metre down).
    static B::Vec3 SlamDirection(B::Vec3 line, B::Vec3 heading, const W::Frame& grip,
                                 float floor_y) {
        B::Vec3 level{line.x, 0.0f, line.z};
        if (B::Length(level) < 0.3f) level = B::Vec3{heading.x, 0.0f, heading.z};
        if (B::Length(level) < 0.1f) level = B::Vec3{0.0f, 0.0f, -1.0f};
        level = B::Normalize(level);
        const float high = floor_y > -1e8f ? std::clamp(grip.t.y - floor_y, 0.0f, 0.9f * ReachM)
                                           : 1.0f;
        return B::Normalize(level * std::sqrt(ReachM * ReachM - high * high) -
                            B::Vec3{0.0f, high, 0.0f});
    }
    // Out and live (your hand moving): the hang's joints, carrying LegGravity of its weight.
    static const W::ChainConfig& LegConfig() {
        static const W::ChainConfig c = [] {
            W::ChainConfig k = HangConfig();
            k.gravity = LegGravity;
            return k;
        }();
        return c;
    }
    // Playing the strike: pulled hard onto it, so its crack stays a crack (LashConfig's pull lags
    // 0.1 s at the tip and smoothed it away), its motion its own (not damped toward the hand's).
    // Probed: 0.5 / 0.35 overshot the crack (84-91 m/s against the clip's 66 stretched) with more
    // one-frame bends; 0.85 / 0.8 barely lags at all.
    static const W::ChainConfig& StrikeConfig() {
        static const W::ChainConfig c = [] {
            W::ChainConfig k = LashConfig();
            k.k_root = 0.7f;
            k.k_tip = 0.6f;
            k.damp = 2.0f;
            k.idle_velocity_damp = 0.0f;
            k.max_bend = 178.0f * B::Deg;
            return k;
        }();
        return c;
    }
    // The middle links' stretch at the strike's farthest (its out 1, less on the way) for the tip at
    // most ReachM from the handle there: its crack curls the tentacle, so stretched only as far as
    // straight out needs, it reached 3.3 m (2026-11-07: "make it expand nicely.. before
    // it felt too short"). The turn about the arm keeps the reach.
    float StrikeStretch() const {
        const Strike& k = *strike_;
        const auto farthest = [&](float s) {
            float most = 0.0f;
            for (std::size_t i = 0; i < k.q.size(); ++i) {
                const float st = 1.0f + (s - 1.0f) * k.out[i];
                B::QsTransform m = neutral_[0];
                for (int j = 0; j < 3; ++j) {
                    B::QsTransform local = neutral_[static_cast<std::size_t>(j + 1)];
                    local.q = k.q[i][static_cast<std::size_t>(j)];
                    if (j > 0) local.t = local.t * st;
                    m = B::Compose(m, local);
                }
                most = std::max(most, B::Length(m.t + B::Rotate(m.q, TipLocal) - neutral_[0].t));
            }
            return most;
        };
        float lo = 1.0f, hi = TautStretch;
        if (!(farthest(hi) > ReachM)) return hi;
        for (int it = 0; it < 16; ++it) {
            const float mid = 0.5f * (lo + hi);
            (farthest(mid) < ReachM ? lo : hi) = mid;
        }
        return lo;
    }
    // The strike on (tau_ s into it): its time on (each part at its own rate; held at the refold
    // until `let_go`: the swing over HangS), the hang (out straight
    // while the hand swings on past the carry on), its pose ref_ and the chain's rest shape.
    // False once it is back in.
    bool Play(float dt, bool let_go) {
        const Strike& k = *strike_;
        if (dt > 0.0f && dt <= 0.1f) {
            blend_ = std::min(1.0f, blend_ + dt / RecrackBlendS);
            if (tau_ < k.hold) // cracked again: from out straight onto the strike
                hang_w_ = std::max(0.0f, hang_w_ - dt / RecrackBlendS);
            else if (let_go)
                hang_w_ = std::max(0.0f, hang_w_ - dt / HangBlendS);
            else if (calm_ <= 0.0f)
                hang_w_ = std::min(1.0f, hang_w_ + dt / HangBlendS);
            float left = dt;
            for (int part = 0; part < 4 && left > 0.0f && tau_ < k.end; ++part) {
                if (tau_ >= k.refold && (!let_go || hang_w_ > 0.0f)) break;
                float rate = 1.0f, until = k.end;
                const float lead = std::max(k.crack - CrackLeadS, k.unfold);
                if (tau_ < k.unfold) {
                    rate = k.unfold / WindS * rate_;
                    until = k.unfold;
                } else if (tau_ < lead) {
                    rate = (lead - k.unfold) / UnfoldS * rate_;
                    until = lead;
                } else if (tau_ < k.refold) {
                    until = k.refold;
                } else {
                    rate = (k.end - k.refold) / RefoldS;
                }
                const float need = (until - tau_) / rate;
                if (need > left) {
                    tau_ += left * rate;
                    left = 0.0f;
                } else {
                    tau_ = until;
                    left -= need;
                }
            }
            tau_ = std::min(tau_, k.end);
        }
        if (tau_ >= k.end && hang_w_ <= 0.0f) return false;
        // Turned toward the swing over the loop, back over the refold (the idle is never turned).
        float turned = 1.0f;
        if (tau_ < k.unfold) {
            const float x = tau_ / k.unfold;
            turned = x * x * (3.0f - 2.0f * x);
        } else if (tau_ > k.refold) {
            const float x = std::min((tau_ - k.refold) / (k.end - k.refold), 1.0f);
            turned = 1.0f - x * x * (3.0f - 2.0f * x);
        }
        const float hang = hang_w_ * hang_w_ * (3.0f - 2.0f * hang_w_);
        const float struck = 1.0f + (strike_stretch_ - 1.0f) * k.OutAt(tau_);
        stretch_ = struck + (throw_stretch_ - struck) * hang;
        const auto q = k.At(tau_);
        const B::Quat r = B::AxisAngle(along_, turn_now_ * turned);
        // Drawn around the strike's shape, out straight as far as it hangs (turned from the
        // strike's curl onto a chain hanging out straight, a joint needed more than its 175
        // degrees: the tentacle flipped across for a frame as it hung and as it cracked again).
        std::array<B::Quat, 3> shape = q;
        if (hang > 0.0f)
            for (int i = 0; i < 3; ++i) shape[i] = B::Normalize(B::Slerp(q[i], straight_q_[i], hang));
        ref_ = neutral_;
        ref_[1].q = B::Normalize(B::Conjugate(neutral_[0].q) * r * neutral_[0].q * shape[0]);
        ref_[2].q = shape[1];
        ref_[3].q = shape[2];
        ref_[2].t = neutral_[2].t * stretch_;
        ref_[3].t = neutral_[3].t * stretch_;
        std::array<B::Vec3, 4> rest{};
        if (!Points(ref_, {}, rest)) return false;
        if (hang > 0.0f) {
            // Out straight: the tip reached for along the arm, the joints following (FABRIK from
            // the strike's shape).
            std::array<float, 3> l{};
            for (int i = 0; i < 3; ++i) l[i] = B::Length(rest[i + 1] - rest[i]);
            const B::Vec3 root = rest[0];
            const B::Vec3 target = B::Lerp(rest[3], root + along_ * (l[0] + l[1] + l[2]), hang);
            for (int pass = 0; pass < 12; ++pass) {
                rest[3] = target;
                for (int i = 2; i >= 0; --i)
                    rest[i] = rest[i + 1] + B::Normalize(rest[i] - rest[i + 1], along_) * l[i];
                rest[0] = root;
                for (int i = 1; i < 4; ++i)
                    rest[i] = rest[i - 1] + B::Normalize(rest[i] - rest[i - 1], along_) * l[i - 1];
            }
        }
        if (blend_ < 1.0f) {
            // Cracked again: each link turned from where it was onto the strike (lengths kept).
            const float x = blend_ * blend_ * (3.0f - 2.0f * blend_);
            std::array<B::Vec3, 4> mixed = rest;
            for (int i = 1; i < 4; ++i) {
                const B::Vec3 a = B::Normalize(from_[i] - from_[i - 1], along_);
                const B::Vec3 c = rest[i] - rest[i - 1];
                const B::Vec3 to = B::Normalize(c, along_);
                mixed[i] = mixed[i - 1] + B::Rotate(B::ScaleRotation(B::FromTo(a, to), x), a) *
                                              B::Length(c);
            }
            rest = mixed;
        }
        if (chain_.SetRest(rest.data())) rest_ = rest;
        return true;
    }
    // The rest shape `w` of the way from the idle fold to straight out, reached for like a leg: the
    // tip goes `w` of the way along the straight line from where the fold holds it to straight out
    // and the joints open to follow it (FABRIK from the fold, so they bend the way it is folded;
    // the middle links lengthened to `s` times their own). Opening the joints by angle instead
    // swung the last links through half a circle (the fold bends 161 and 145 degrees): up over the
    // hand, down to the floor and out. Unfolding, each solve starts from the shape before (`from`):
    // started from the fold every frame, the last joint flipped for a frame early on. Folding back
    // it starts from the fold, which refolds the joints in the game's own order (the clips': the
    // one at the hand first, the tip's last). The chain's pull and links follow it.
    std::array<B::Vec3, 4> RestAt(float w, float s,
                                  const std::array<B::Vec3, 4>* from = nullptr) const {
        std::array<float, 3> l{};
        float total = 0.0f;
        for (int i = 0; i < 3; ++i) {
            const float own = B::Length(fold_[i + 1] - fold_[i]);
            l[i] = i < 2 ? own * (1.0f + (s - 1.0f) * w) : own;
            total += l[i];
        }
        std::array<B::Vec3, 4> rest = from ? *from : fold_;
        if (w <= 0.0f) return fold_;
        if (w <= 0.0f) return rest;
        const B::Vec3 root = fold_[0];
        if (w >= 1.0f) {
            for (int i = 1; i < 4; ++i) rest[i] = rest[i - 1] + along_ * l[i - 1];
            return rest;
        }
        const B::Vec3 target = B::Lerp(fold_[3], root + along_ * total, w);
        for (int pass = 0; pass < 12; ++pass) {
            rest[3] = target;
            for (int i = 2; i >= 0; --i)
                rest[i] = rest[i + 1] + B::Normalize(rest[i] - rest[i + 1], along_) * l[i];
            rest[0] = root;
            for (int i = 1; i < 4; ++i)
                rest[i] = rest[i - 1] + B::Normalize(rest[i] - rest[i - 1], along_) * l[i - 1];
        }
        return rest;
    }
    // The chain's points of `pose` (the joints and the tip) in the world, through `grip`.
    static bool Points(const Pose& pose, const W::Frame& grip, std::array<B::Vec3, 4>& points) {
        B::QsTransform m = pose[0];
        for (int i = 1; i <= 3; ++i) {
            m = B::Compose(m, pose[i]);
            points[i - 1] = m.t;
        }
        points[3] = m.t + B::Rotate(B::Normalize(m.q), TipLocal);
        for (auto& p : points) {
            p = W::Apply(grip, p);
            if (!B::IsFinite(p)) return false;
        }
        return true;
    }
    bool Capture(const Pose& sampled) {
        std::array<B::Vec3, 4> rest{};
        if (!Points(sampled, {}, rest) || !chain_.Init(rest.data(), 4)) return false;
        // Straight out: on along the arm's first segment (the handle to Dummy001), the middle
        // links stretched so the tip is ReachM from the handle.
        const B::Vec3 first = rest[0] - sampled[0].t;
        const float l0 = B::Length(first), l1 = B::Length(rest[1] - rest[0]),
                    l2 = B::Length(rest[2] - rest[1]), l3 = B::Length(rest[3] - rest[2]);
        if (!(l0 > 1e-3f) || !(l1 + l2 > 1e-3f)) return false;
        along_ = first * (1.0f / l0);
        throw_stretch_ = std::max(1.0f, (ReachM - l0 - l3) / (l1 + l2));
        jab_stretch_ = std::max(1.0f, (JabM - l0 - l3) / (l1 + l2));
        reach_ = 1.0f;
        ends_ = l0 + l3;
        middle_ = l1 + l2;
        fold_ = rest;
        straight_[0] = rest[0];
        straight_[1] = straight_[0] + along_ * (l1 * throw_stretch_);
        straight_[2] = straight_[1] + along_ * (l2 * throw_stretch_);
        straight_[3] = straight_[2] + along_ * l3;
        rest_w_ = rest_oe_ = 0.0f;
        neutral_ = sampled;
        for (auto& bone : neutral_) bone.q = B::Normalize(bone.q);
        // The joints' local rotations with every link out along the arm (the hang's drawn shape).
        B::Quat parent = neutral_[0].q;
        const std::array<B::Vec3, 3> next{neutral_[2].t, neutral_[3].t, TipLocal};
        for (std::size_t i = 0; i < 3; ++i) {
            const B::Quat model = B::Normalize(parent * neutral_[i + 1].q);
            straight_q_[i] = B::Normalize(B::Conjugate(parent) *
                                          B::FromTo(B::Rotate(model, next[i]), along_) * model);
            parent = B::Normalize(parent * straight_q_[i]);
        }
        // The bent leg it is drawn in to: open so far that its tip is WorkReachM from the handle.
        work_open_ = 1.0f;
        const auto tip_at = [&](float o) {
            return B::Length(RestAt(o, throw_stretch_)[3] - sampled[0].t);
        };
        if (tip_at(0.0f) < WorkReachM && tip_at(1.0f) > WorkReachM) {
            float lo = 0.0f, hi = 1.0f;
            for (int it = 0; it < 24; ++it) {
                const float mid = 0.5f * (lo + hi);
                (tip_at(mid) < WorkReachM ? lo : hi) = mid;
            }
            work_open_ = 0.5f * (lo + hi);
        }
        have_neutral_ = true;
        return true;
    }
    void Keep(const Pose& out) {
        last_ = out;
        have_last_ = true;
    }

    W::Chain chain_{};
    Pose neutral_{}, last_{}, tracked_pose_{};
    std::array<B::Quat, 3> fade_q_{};
    std::array<B::Vec3, 2> fade_t_{};
    bool fade_stretch_{false}; // the idle ended with the lash out: its stretch reels in too
    W::Frame previous_{}, tracked_grip_{};
    float fade_{0.0f};
    // The lash: the fold and straight-out rest shapes (grip space), the arm's line, the middle
    // links' stretch now (a taut hook), where it is (0..1) and the rest shape the chain has now;
    // the arm's first segment and tip (ends_) and middle links' lengths.
    std::array<B::Vec3, 4> fold_{}, straight_{}, rest_{}, rest_seed_{};
    std::array<B::Quat, 3> straight_q_{}; // Dummy001..003 local, out straight along the arm
    B::Vec3 along_{0.0f, -1.0f, 0.0f};
    B::Vec3 rest_aim_{};
    float reach_{1.0f}, lash_{0.0f}, rest_w_{0.0f}, rest_reach_{1.0f};
    float throw_stretch_{1.0f}; // the middle links' stretch thrown, for ReachM
    float ends_{0.0f}, middle_{1.0f};
    std::array<B::Vec3, 4> recall_{};
    float recall_lash_{0.0f}, droop_{0.0f};
    bool was_out_{false};
    float thrown_{0.0f}; // s since the throw
    float calm_{0.0f};   // s the hand has swung slower than HangMps
    float sag_s_{HangS};
    float recall_s_{LashInS}; // captured before folding, stable through the whole recall
    bool have_sag_{false};
    bool skip_sag_{false}; // this slowdown's sag skipped (SagVariation 0): SkipHoldS, no droop
    std::uint32_t sag_random_{0x9e3779b9u};
    bool out_{false};     // thrown (or hooked) and not yet reeled back in
    bool rearmed_{true};  // the hand slowed since the last throw
    // The jab: the trigger last frame, s the hand has been still (under JabStillMps), s left before
    // a press jabs (below 0: none), the middle links' stretch for JabM; out as a jab, or latched on.
    bool trigger_{false}, jab_{false}, latched_{false};
    float still_{0.0f}, jab_wait_{-1.0f}, jab_stretch_{1.0f};
    B::Vec3 way_{}; // the hand's way over AimS (grip space, across the arm; m/s)
    float peak_{0.0f}, fast_{0.0f}; // this swing's fastest (m/s) and time at FireMps or more (s)
    float stroke_s_{0.0f}, stroke_m_{0.0f}; // controller-equivalent travel of this stroke
    // The crack held and drawn in: s at full length (-1: not yet), s since the hand swung at
    // HardSwingMps, how far open (1 straight .. work_open_ the bent leg) and its spring's speed.
    float since_full_{-1.0f}, hard_calm_{0.0f}, open_{1.0f}, open_v_{0.0f}, work_open_{1.0f};
    float full_wait_{0.0f}; // s its rest shape has been at full reach
    float rest_oe_{0.0f};
    // The hand's fastest over about SagPeakS (m/s) and its age; the stroke's highest controller
    // line (its y).
    float hand_peak_{0.0f}, hand_peak_age_{0.0f}, stroke_raise_{-1.0f};
    float coiled_{0.0f}; // s a refused wind-up still lets the stroke ahead throw at once
    // m: your hand's height in this stroke from where it started, and its highest in it.
    float stroke_hand_y_{0.0f}, stroke_hand_top_{0.0f};
    // The slam: on, s since it was thrown, the way down to the ground ahead (world, unit).
    bool slam_{false};
    float slam_t_{0.0f};
    B::Vec3 slam_dir_{0.0f, -1.0f, 0.0f};
    // The leg-hold's heading (LegHeadingMinFlat): the controller's last clearly-out line, flat.
    static constexpr float LegHeadingMinFlat = 0.3f; // its flat share: ~17 degrees off vertical
    B::Vec3 leg_heading_{1.0f, 0.0f, 0.0f};
    bool have_leg_heading_{false};
    unsigned stroke_samples_{0};
    bool extended_attack_{false}; // this lash actually reached near-full physical extension
    // The game's strike (null: none read; the reach-out instead) and the one playing: s into it,
    // its turn about the arm (rad: aimed, and turned so far), the swing's rate, the hang (0 its
    // shape .. 1 out straight),
    // the middle links' stretch and its pose now (drawn around).
    const Strike* strike_{nullptr};
    bool playing_{false};
    float tau_{0.0f}, turn_{0.0f}, turn_now_{0.0f}, rate_{1.0f}, hang_w_{0.0f}, stretch_{1.0f};
    float strike_stretch_{0.0f}; // StrikeStretch, once (0: not yet)
    bool aimed_{false}; // CrackLeadS before the crack: turn_ kept
    // Cracked again: the chain's shape then (grip space) and how far onto the strike (0..1).
    std::array<B::Vec3, 4> from_{};
    float blend_{1.0f};
    Pose ref_{};
    // An attack clip's points (world) and their motion, for a lash or the idle taking over.
    std::array<B::Vec3, 4> track_points_{}, tracked_velocity_{};
    bool have_track_{false};
    bool have_neutral_{false}, running_{false}, tracked_{false}, have_last_{false};
};

// Keep the drawn tip on a physical hook point without stretching the model or
// moving its handle. Coordinates are the weapon's model frame. Unreachable
// targets stay at the chain's reach; the native pull moves the enemy toward it.
inline bool AimTip(Pose& pose, B::Vec3 target) {
    if (!B::IsFinite(target))
        return false;
    for (const auto& bone : pose)
        if (!B::IsFinite(bone))
            return false;
    const Pose original = pose;
    const auto models = [](const Pose& local) {
        Pose model = local;
        for (int i = 1; i < 4; ++i)
            model[i] = B::Compose(model[i - 1], local[i]);
        return model;
    };
    for (int iteration = 0; iteration < 8; ++iteration) {
        for (int joint = 3; joint >= 1; --joint) {
            const auto model = models(pose);
            const auto tip = model[3].t + B::Rotate(model[3].q, TipLocal);
            if (B::LengthSq(tip - target) < .000025f)
                return true;
            const auto a = tip - model[joint].t, b = target - model[joint].t;
            if (B::LengthSq(a) < 1e-8f || B::LengthSq(b) < 1e-8f)
                continue;
            const auto turn = B::FromTo(a, b);
            auto q = B::Normalize(B::Conjugate(model[joint - 1].q) * turn * model[joint].q);
            const auto delta = B::Normalize(q * B::Conjugate(original[joint].q));
            const float angle = B::AngleOf(delta);
            const float limit = (joint == 1 ? 65.f : joint == 2 ? 90.f : 100.f) * B::Deg;
            if (angle > limit)
                q = B::Normalize(B::ScaleRotation(delta, limit / angle) * original[joint].q);
            pose[joint].q = q;
        }
    }
    for (const auto& bone : pose) {
        if (!B::IsFinite(bone)) {
            pose = original;
            return false;
        }
    }
    return true;
}
} // namespace VR::AmygdalanArm
