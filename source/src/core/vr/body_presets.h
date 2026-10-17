// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Scripted controller poses for testing the VR body without a headset. The mock OpenXR runtime
// (the test harness, MOCKXR_HANDS) returns them from xrLocateSpace for the grip and aim spaces,
// and --vr-sim builds the same hands relative to its synthetic head (VR_BODY_SIM_HANDS). Standard
// library only, so the mock includes it as it is.
//
// LOCAL space, metres, a standing user with the head at (0, 1.6, 0) looking down -Z. A grip's
// orientation is given by its blade (grip -Z): yaw (+ = left), pitch (+ = up), then roll about the
// blade, degrees. A timeline plays presets for SegmentSeconds each, in order, and loops.
namespace VR::BodyPresets {

constexpr double SegmentSeconds = 3.0;
constexpr float HeadHeight = 1.6f;

struct GripPose {
    float x, y, z;          // metres
    float yaw, pitch, roll; // degrees
};

enum class Motion : unsigned char {
    Hold,      // both grips hold their pose
    RollSweep, // the right grip rolls -90 -> +90 degrees over the segment
    Swing,     // the right grip swings from `right` to `swing_end` (minimum jerk, 0.25 s at 1.0 s)
    Lost,      // the right grip loses tracking from 1.25 s to 1.75 s (position inferred)
};

struct Preset {
    std::string_view name;
    GripPose right;
    GripPose left;
    Motion motion;
    bool grip_right; // the right grip is squeezed (weapon drawn) during the segment
    std::string_view expect;
};

inline constexpr GripPose LeftRest{-0.20f, 1.00f, -0.20f, 0.0f, -20.0f, 0.0f};
inline constexpr GripPose SwingStart{0.35f, 1.50f, 0.00f, -30.0f, 100.0f, 0.0f};
inline constexpr GripPose SwingEnd{-0.20f, 1.10f, -0.45f, 30.0f, -20.0f, 0.0f};
constexpr double SwingAt = 1.0;
constexpr double SwingSeconds = 0.25;
constexpr double LostFrom = 1.25;
constexpr double LostTo = 1.75;

// clang-format off
inline constexpr std::array<Preset, 14> Presets = {{
    {"rest", {0.20f, 1.00f, -0.20f, 0.0f, -20.0f, 0.0f}, LeftRest, Motion::Hold, false,
     "both reached, relaxed elbows"},
    {"still", {0.20f, 1.20f, -0.35f, 0.0f, 10.0f, 0.0f}, {-0.20f, 1.20f, -0.35f, 0.0f, 10.0f, 0.0f},
     Motion::Hold, false, "both hands held still in front: no jitter, no drift"},
    {"guard", {0.18f, 1.25f, -0.40f, 0.0f, 45.0f, 0.0f}, {-0.22f, 1.10f, -0.30f, 0.0f, 0.0f, 0.0f},
     Motion::Hold, true, "reached, blade up"},
    {"blade_level", {0.15f, 1.30f, -0.40f, 0.0f, 0.0f, 0.0f}, LeftRest, Motion::Hold, true,
     "blade horizontal, forward"},
    {"reach_fwd", {0.17f, 1.38f, -0.50f, 0.0f, 0.0f, 0.0f}, LeftRest, Motion::Hold, false,
     "reach forward: near straight, stretch <= 5%"},
    {"out_of_reach", {0.17f, 1.38f, -0.90f, 0.0f, 0.0f, 0.0f}, LeftRest, Motion::Hold, false,
     "detached, stretch 1.10, the arm points at the target"},
    {"high", {0.25f, 2.05f, -0.20f, 0.0f, 70.0f, 0.0f}, LeftRest, Motion::Hold, false,
     "reach up: reached or slightly detached, elbow out"},
    {"cross", {-0.25f, 1.30f, -0.35f, 45.0f, 0.0f, 0.0f}, LeftRest, Motion::Hold, false,
     "cross-body: reached, no elbow flip"},
    {"over_shoulder", {0.15f, 1.75f, 0.12f, 0.0f, 150.0f, 0.0f}, LeftRest, Motion::Hold, false,
     "over the shoulder: elbow up, no flip"},
    {"behind_head", {0.02f, 1.70f, 0.16f, 0.0f, 160.0f, 0.0f}, LeftRest, Motion::Hold, false,
     "behind the head: elbow up, no flip"},
    {"roll_sweep", {0.18f, 1.25f, -0.40f, 0.0f, 45.0f, -90.0f}, LeftRest, Motion::RollSweep, true,
     "no candy-wrapped forearm, the elbow turns <= 22 deg"},
    {"wrist_inject", {0.15f, 1.15f, -0.30f, 0.0f, 0.0f, 0.0f},
     {0.10f, 1.17f, -0.28f, -60.0f, -10.0f, 0.0f}, Motion::Hold, false,
     "the left glove on the right wrist"},
    {"swing", SwingStart, LeftRest, Motion::Swing, true,
     "a fast swing: R1, the right arm animated, then back"},
    {"lost", {0.18f, 1.25f, -0.40f, 0.0f, 45.0f, 0.0f}, LeftRest, Motion::Lost, false,
     "right tracking lost 0.5 s: held 0.3 s, then the animated arm"},
}};
// clang-format on

// Explicit-only inspection poses, excluded from the normal "all" test route.
inline constexpr std::array<Preset, 4> GripPreviews = {{
    {"grip_front", {0.25f, 1.00f, -0.20f, 0, -20, 0},
     {-0.12f, 1.30f, -0.38f, 0, 0, 0}, Motion::Hold, false, "held item, neutral wrist"},
    {"grip_palm", {0.25f, 1.00f, -0.20f, 0, -20, 0},
     {-0.12f, 1.30f, -0.38f, 0, 0, 90}, Motion::Hold, false, "held item, wrist rolled +90"},
    {"grip_back", {0.25f, 1.00f, -0.20f, 0, -20, 0},
     {-0.12f, 1.30f, -0.38f, 0, 0, -90}, Motion::Hold, false, "held item, wrist rolled -90"},
    {"grip_side", {0.25f, 1.00f, -0.20f, 0, -20, 0},
     {-0.12f, 1.30f, -0.38f, -60, 0, 0}, Motion::Hold, false, "held item, wrist yaw -60"},
}};

inline const Preset* Find(std::string_view name) {
    for (const Preset& preset : GripPreviews) {
        if (preset.name == name) return &preset;
    }
    for (const Preset& preset : Presets) {
        if (preset.name == name) {
            return &preset;
        }
    }
    return nullptr;
}

// MOCKXR_HANDS / VR_BODY_SIM_HANDS: "1", "poses" or "all" = every preset in order; otherwise a
// comma-separated list of preset names (one name holds that preset). Unknown names are skipped and
// listed in `unknown`.
inline std::vector<const Preset*> ParseTimeline(std::string_view spec,
                                                std::string* unknown = nullptr) {
    std::vector<const Preset*> timeline;
    if (spec == "1" || spec == "poses" || spec == "all") {
        for (const Preset& preset : Presets) {
            timeline.push_back(&preset);
        }
        return timeline;
    }
    while (!spec.empty()) {
        const std::size_t comma = spec.find(',');
        const std::string_view name = spec.substr(0, comma);
        if (!name.empty()) {
            if (const Preset* preset = Find(name)) {
                timeline.push_back(preset);
            } else if (unknown != nullptr) {
                *unknown += (unknown->empty() ? "" : ",") + std::string{name};
            }
        }
        spec = comma == std::string_view::npos ? std::string_view{} : spec.substr(comma + 1);
    }
    return timeline;
}

struct TimelinePoint {
    const Preset* preset{nullptr};
    std::size_t index{0};     // into the timeline
    std::uint64_t segment{0}; // segments played so far (index + loops * size)
    double t{0.0};            // seconds into the segment
};

inline TimelinePoint Locate(const std::vector<const Preset*>& timeline, double seconds) {
    TimelinePoint point{};
    if (timeline.empty()) {
        return point;
    }
    const double clamped = seconds > 0.0 ? seconds : 0.0;
    point.segment = static_cast<std::uint64_t>(clamped / SegmentSeconds);
    point.index = static_cast<std::size_t>(point.segment % timeline.size());
    point.preset = timeline[point.index];
    point.t = clamped - static_cast<double>(point.segment) * SegmentSeconds;
    return point;
}

struct HandPose {
    std::array<float, 3> p{};
    std::array<float, 4> q{0.0f, 0.0f, 0.0f, 1.0f}; // x, y, z, w
    bool tracked{true}; // false: position inferred (tracking lost), orientation still tracked
};

// Blade yaw, pitch, roll -> grip orientation: q = q_y(yaw) * q_x(pitch) * q_z(roll).
inline std::array<float, 4> GripRotation(float yaw_deg, float pitch_deg, float roll_deg) {
    constexpr float D = 3.14159265358979f / 180.0f;
    const float cy = std::cos(0.5f * yaw_deg * D), sy = std::sin(0.5f * yaw_deg * D);
    const float cp = std::cos(0.5f * pitch_deg * D), sp = std::sin(0.5f * pitch_deg * D);
    const float cr = std::cos(0.5f * roll_deg * D), sr = std::sin(0.5f * roll_deg * D);
    // q_y * q_x = (cy sp, sy cp, -sy sp, cy cp); then * q_z = (0, 0, sr, cr).
    const float x = cy * sp, y = sy * cp, z = -sy * sp, w = cy * cp;
    return {x * cr + y * sr, y * cr - x * sr, z * cr + w * sr, w * cr - z * sr};
}

// Minimum-jerk progress 0..1.
inline double MinJerk(double s) {
    s = s < 0.0 ? 0.0 : (s > 1.0 ? 1.0 : s);
    return s * s * s * (10.0 - 15.0 * s + 6.0 * s * s);
}

// The scripted grip of `side` (0 = left, 1 = right) `t` seconds into a segment of `preset`.
inline HandPose Evaluate(const Preset& preset, int side, double t) {
    GripPose g = side == 0 ? preset.left : preset.right;
    HandPose out{};
    if (side == 1) {
        switch (preset.motion) {
        case Motion::Hold:
            break;
        case Motion::RollSweep:
            g.roll = static_cast<float>(-90.0 + 180.0 * (t / SegmentSeconds));
            break;
        case Motion::Swing: {
            const float k = static_cast<float>(MinJerk((t - SwingAt) / SwingSeconds));
            const auto mix = [k](float a, float b) { return a + (b - a) * k; };
            g = {mix(SwingStart.x, SwingEnd.x),         mix(SwingStart.y, SwingEnd.y),
                 mix(SwingStart.z, SwingEnd.z),         mix(SwingStart.yaw, SwingEnd.yaw),
                 mix(SwingStart.pitch, SwingEnd.pitch), mix(SwingStart.roll, SwingEnd.roll)};
            break;
        }
        case Motion::Lost:
            out.tracked = !(t >= LostFrom && t < LostTo);
            break;
        }
    }
    out.p = {g.x, g.y, g.z};
    out.q = GripRotation(g.yaw, g.pitch, g.roll);
    return out;
}

} // namespace VR::BodyPresets
