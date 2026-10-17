// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>

// VR body cutaway boost (vr.ini body_cutaway_boost; the user, 2026-10-08: "sometimes when enemies
// hit me or I move my head a tiny bit or down, I see a bit through the shoulders/body of the
// hunter, was wondering if in those cases we can fade it out a bit more").
//
// With the whole body drawn (body_mode no_head / full) the cutaway fades his body out from
// body_cutaway_far_m (22 cm) to body_cutaway_near_m (3 cm) from the camera, its inside (the coat's
// lining) drawn. The game camera's near clip is 10 cm, so at the clip 31% of the body's pixels are
// still drawn and the rest of it nearer is cut open with a hard edge, showing the lining. Two
// things put the camera that close to his collar and shoulders:
//  - a hit reaction, a stagger or a kept dodge: the game's clip moves his upper body under the
//    head-locked camera (the trunk alignment lets go in a damage clip); the user's log measured the
//    camera 23 cm (median, max 80) from his eye in damage windows, 18 cm in dodges, 7 cm otherwise;
//  - your head a little behind or below his eye: his trunk leans back at most 20 degrees, and a
//    head drop within the 8 cm height deadband leaves his upper body where it is.
// Then the fade is stronger for a moment:
//  - level 2 (a kept hit or dodge, a hit-stun clip, a stagger or knockdown): the body fades from
//    body_cutaway_boost_far_m (35 cm) to body_cutaway_boost_near_m (11 cm: past the near clip, so
//    nothing is left there to cut open);
//  - level 1 (the camera >= 4 cm behind or >= 4.5 cm below his eye): halfway between the normal
//    fade and level 2's;
//  - both never draw his inside (back faces), so what shows through is the floor beyond, not a
//    hollow coat.
// Starting a ladder climb also selects level 2 for up to 3 s, ending on dismount or when the
// first-person view gives way to the flat screen. Its timer comes from the player update so
// climbing rungs and returning from the screen cannot restart it.
// Each hit/head-position level is held 0.4 s after its cause ends (a flinch or head bob does not
// flicker it), and never makes the fade weaker than the normal one. Each cut is a pipeline variant
// of the body's draws (FragmentRuntimeInfo::vr_cut), so only these three exist and the renderer
// builds them together when a body pipeline first shows (no compile on the first hit of a fight). With
// Shader::VrPushData (native stereo) the radii are pushed per draw: one variant for all levels.
// Pure logic (no renderer or game types) for tests/test_body_cut_boost.cpp.
namespace VR::Body {

struct CutBoostInput {
    bool fresh{false}; // a body status from the pose hook, the body driven (else nothing new)
    bool hit{false};   // a kept hit or dodge, a hit-stun clip or a stagger / knockdown
    // The camera against his eye after the body's alignment (body_solver.h BodyOutput): ahead (+) /
    // behind (-) along his facing, above (+) / below (-).
    float cam_ahead_m{0.0f};
    float cam_above_m{0.0f};
    float cam_side_m{0.0f};
    // Independently fresh motion signal, even when ladder animations release body IK.
    std::int64_t ladder_start_ns{0}; // current climb start; 0 = off ladder / unavailable
};

struct CutBoostParams {
    float behind_m{0.04f}; // level 1: the camera at least this far behind his eye
    float below_m{0.045f}; // ... or at least this far below it
    float side_m{0.0f};    // zero disables lateral protection
    float recovery_s{0.0f}; // level 2 returns through level 1, using existing pipeline variants
    float hold_s{0.4f};     // each level is held this long after its cause ends
};

enum class CutBoostWhy : std::uint8_t { None, Hit, Behind, Below, Side, Ladder };

class CutBoost {
public:
    static constexpr std::int64_t LadderBoostNs = 3'000'000'000;

    // A flat-screen frame cancels every mount up to its own time. This also catches a mount
    // whose player sample was stale/missing on that frame and arrives after returning to VR.
    void CancelLadder(std::int64_t now_ns) {
        ladder_cancelled_through_ns_ = std::max(ladder_cancelled_through_ns_, now_ns);
    }

    // The level now: 0 the normal cutaway, 1 half the boost, 2 the full boost.
    std::uint32_t Update(const CutBoostInput& in, std::int64_t now_ns, const CutBoostParams& p) {
        const auto hold =
            static_cast<std::int64_t>(static_cast<double>(std::max(p.hold_s, 0.0f)) * 1e9);
        const bool full = in.fresh && in.hit;
        const bool ladder =
            in.ladder_start_ns > ladder_cancelled_through_ns_ &&
            now_ns >= in.ladder_start_ns && now_ns - in.ladder_start_ns < LadderBoostNs;
        const bool behind = in.fresh && in.cam_ahead_m <= -std::max(p.behind_m, 0.0f);
        const bool below = in.fresh && in.cam_above_m <= -std::max(p.below_m, 0.0f);
        const bool side = in.fresh && p.side_m > 0.0f && std::abs(in.cam_side_m) >= p.side_m;
        if (full) {
            full_until_ = now_ns + hold;
            full_why_ = CutBoostWhy::Hit;
            if (p.recovery_s > 0.0f) {
                half_until_ = std::max(half_until_,
                                       full_until_ + static_cast<std::int64_t>(p.recovery_s * 1e9));
                half_why_ = CutBoostWhy::Hit;
            }
        }
        if (behind || below || side) {
            half_until_ = std::max(half_until_, now_ns + hold);
            half_why_ = behind  ? CutBoostWhy::Behind
                        : below ? CutBoostWhy::Below
                                : CutBoostWhy::Side;
        }
        level_ = ladder || full || now_ns < full_until_            ? 2u
                 : behind || below || side || now_ns < half_until_ ? 1u
                                                                   : 0u;
        why_ = level_ == 2   ? (ladder ? CutBoostWhy::Ladder : full_why_)
               : level_ == 1 ? half_why_
                             : CutBoostWhy::None;
        return level_;
    }

    [[nodiscard]] std::uint32_t Level() const {
        return level_;
    }
    [[nodiscard]] CutBoostWhy Why() const {
        return why_;
    }
    [[nodiscard]] static const char* WhyText(CutBoostWhy why) {
        switch (why) {
        case CutBoostWhy::Ladder:
            return "starting a ladder climb";
        case CutBoostWhy::Hit:
            return "a hit, stagger or dodge";
        case CutBoostWhy::Behind:
            return "the camera behind his eye";
        case CutBoostWhy::Below:
            return "the camera below his eye";
        case CutBoostWhy::Side:
            return "the camera beside his eye";
        case CutBoostWhy::None:
            break;
        }
        return "none";
    }

private:
    std::int64_t ladder_cancelled_through_ns_{0};
    std::int64_t full_until_{0};
    std::int64_t half_until_{0};
    CutBoostWhy full_why_{CutBoostWhy::None};
    CutBoostWhy half_why_{CutBoostWhy::None};
    CutBoostWhy why_{CutBoostWhy::None};
    std::uint32_t level_{0};
};

// A cutaway: the body fades out from far_m to near_m from the camera; `inside`: its back faces are
// drawn.
struct CutRadii {
    float near_m{0.0f};
    float far_m{0.0f};
    bool inside{true};
};

// The cutaway at boost `level` (0: `base` itself): level 2 the boost's radii, level 1 halfway
// between them and `base`'s; neither nearer than `base`'s nor drawing the inside.
[[nodiscard]] constexpr CutRadii BoostedRadii(const CutRadii& base, float boost_near_m,
                                              float boost_far_m, std::uint32_t level) {
    if (level == 0) {
        return base;
    }
    const float near_m = std::max(boost_near_m, base.near_m);
    const float far_m = std::max(std::max(boost_far_m, base.far_m), near_m);
    if (level >= 2) {
        return {near_m, far_m, false};
    }
    const float half_near = 0.5f * (base.near_m + near_m);
    const float half_far = std::max(0.5f * (base.far_m + far_m), half_near);
    return {half_near, half_far, false};
}

} // namespace VR::Body
