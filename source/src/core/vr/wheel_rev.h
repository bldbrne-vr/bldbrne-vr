// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

// Logarius' Wheel's spin (2026-11-06: "keep the weapon animation and applied effect, but
// not make the hunter themselves animate"). Its two-handed form's special is the game's L2 chain
// AtkBothL2_01..05 (a110_046000..046400): five spins in a row, each adding a stronger buff
// (SpEffect 6020..6024, 20 s each: arcane attack x1.1 / 1.2 / 1.4 / 1.6 / 1.6, and an HP drain
// that grows with the step). Here each quick left-trigger pull is the next step, played on the
// weapon only (core/vr/bloodborne_transform.cpp): the wheel model's own clip of the step
// (WP_A_1052_2.anibnd), the clip's sounds and sparks, and its buff at the step's time (the clip's
// TAE event 401) through the game's AddSpEffect; the hunter does not move. A pull before the step
// before has added its buff waits for it (one waits; more are dropped, so the buffs come in the
// game's order); a pull after the step's chain window (the hunter's clip in the game) starts again
// at step 1, as in the flat game; at step 5 a pull replays step 5 (its buff again). Pure (no
// OpenXR or game state): tests/test_wheel_rev.cpp; the presenter runs it (openxr_presenter.cpp
// UpdateGun).
namespace VR::WheelRev {

constexpr int Steps = 5;
// The game's chain window: a110_046x00's events end at 3.17 s (the hunter lowering the wheel).
constexpr float ChainS = 3.17f;

struct StepClip {
    std::int32_t clip;     // the weapon parts' clip (a110_<clip % 1000000>_2 on the form-B model)
    float length;          // s (the hkx's duration)
    std::int32_t speffect; // the buff the step adds ...
    float buff_s;          // ... at this clip time (its TAE event 401)
};
inline constexpr std::array<StepClip, Steps> StepClips = {{
    {110046000, 1.667f, 6020, 0.50f},
    {110046100, 1.500f, 6021, 0.33f},
    {110046200, 1.500f, 6022, 0.33f},
    {110046300, 1.500f, 6023, 0.33f},
    {110046400, 1.500f, 6024, 0.33f},
}};

// The clips' own sounds (TAE 128, type 1: c<id>), the same in every step. Not the hunter's: his
// footsteps and cloth (types 8 / 9) and the clack as he lowers the wheel (c8100 at 2.77 s).
struct Sound {
    float t;
    std::int32_t id;
};
inline constexpr std::array<Sound, 4> SpinSounds = {{
    {0.37f, 5600}, {0.37f, 5800}, {0.37f, 853}, {0.40f, 852}}};
// The sparks on the wheel (TAE 96: ffx 252011 at dummy 12100, held 0.43..0.77 s in every step).
constexpr std::int32_t SparkFfx = 252011;
constexpr std::int32_t SparkDummy = 12100;
constexpr float SparkFromS = 0.43f;
constexpr float SparkToS = 0.77f;

constexpr const StepClip& ClipOf(int step) {
    return StepClips[static_cast<std::size_t>(std::clamp(step, 1, Steps) - 1)];
}

struct Pull {
    int step{0};        // a step starts now (1..Steps), else 0
    bool queued{false}; // this frame's pull waits for the step before's buff
};

class Rev {
public:
    // `down`: the trigger now; `dt`: the time since the last call (s).
    Pull Update(bool down, float dt) {
        const bool edge = down && !down_;
        down_ = down;
        if (step_ > 0) {
            since_ += std::max(dt, 0.0f);
            if (since_ > ChainS) {
                step_ = 0; // the window closed: the next pull starts again
                waiting_ = false;
            }
        }
        Pull out{};
        if (edge) {
            if (step_ == 0) {
                return Start(1);
            }
            if (waiting_) {
                return out; // one waits already
            }
            waiting_ = true;
            out.queued = true;
        }
        if (waiting_ && since_ >= ClipOf(step_).buff_s) {
            waiting_ = false;
            return Start(std::min(step_ + 1, Steps));
        }
        return out;
    }
    void Reset() {
        *this = {};
    }
    int Step() const {
        return step_;
    }
    bool Waiting() const {
        return waiting_;
    }

private:
    Pull Start(int step) {
        step_ = step;
        since_ = 0.0f;
        Pull out{};
        out.step = step;
        return out;
    }

    bool down_{false};
    bool waiting_{false};
    int step_{0};
    float since_{0.0f};
};

} // namespace VR::WheelRev
