// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cmath>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include "core/vr/amygdalan_arm.h"
#include "core/vr/whip_idle.h"

// The Amygdalan Arm's strike (core/vr/amygdalan_arm.h Strike), read from the user's own game files
// at start: the whip form's part anibnd (parts/wp_a_1701.partsbnd.dcx -> WP_A_1701_2.anibnd), its
// skeleton_2.hkx (the tentacle's bones Dummy001..003 under the handle) and one attack clip, sampled
// every Strike::SampleS over its length (core/vr/whip_idle.h's clip reader). Nothing of the game's
// is in the mod. Header only, no emulator dependencies (the unit tests).
namespace VR::AmygdalanArm {

// The part's model and the clip: the whip form's first R1, a142_040000 (its loop, unfold, crack
// at full reach, carry on round, partway back and refold; the others crack the same way).
inline constexpr std::string_view StrikePart = "wp_a_1701";
inline constexpr std::string_view StrikeClip = "a142_040000_2.hkx";

inline std::optional<Strike> StrikeFromAnibnd(std::span<const std::uint8_t> anibnd,
                                              std::string& why) {
    namespace I = Whip::Idle;
    const auto skel = I::R::Bnd4Find(anibnd, "skeleton_2.hkx");
    const auto file = I::R::Bnd4Find(anibnd, StrikeClip);
    if (!skel || !file) {
        why = !skel ? "no skeleton_2.hkx" : "no " + std::string(StrikeClip);
        return std::nullopt;
    }
    const auto bones = I::ParseSkeleton(*skel, why);
    if (!bones) {
        why = "skeleton_2.hkx: " + why;
        return std::nullopt;
    }
    const auto clip = I::ParseClip(*file, why);
    if (!clip) {
        why = std::string(StrikeClip) + ": " + why;
        return std::nullopt;
    }
    const auto find = [&](std::string_view name) {
        for (std::size_t i = 0; i < bones->size(); ++i)
            if ((*bones)[i].name == name) return static_cast<int>(i);
        return -1;
    };
    const std::array<int, 3> chain = {find("Dummy001"), find("Dummy002"), find("Dummy003")};
    if (chain[0] < 0 || chain[1] < 0 || chain[2] < 0 ||
        (*bones)[static_cast<std::size_t>(chain[0])].parent != 0 ||
        (*bones)[static_cast<std::size_t>(chain[1])].parent != chain[0] ||
        (*bones)[static_cast<std::size_t>(chain[2])].parent != chain[1]) {
        why = "not the tentacle's skeleton (handle -> Dummy001 -> Dummy002 -> Dummy003)";
        return std::nullopt;
    }
    const auto quat = [](const std::array<double, 4>& q) {
        return B::Normalize(B::Quat{static_cast<float>(q[0]), static_cast<float>(q[1]),
                                    static_cast<float>(q[2]), static_cast<float>(q[3])});
    };
    const auto vec = [](const std::array<double, 3>& t) {
        return B::Vec3{static_cast<float>(t[0]), static_cast<float>(t[1]), static_cast<float>(t[2])};
    };
    // Each bone's pose at `t`: the clip's track, else the skeleton's reference.
    std::array<I::TrackPose, 4> first{};
    Strike s;
    const int samples = static_cast<int>(std::floor(clip->duration / Strike::SampleS + 1e-3f)) + 1;
    if (samples < 8 || samples > 2000) {
        why = "a clip of " + std::to_string(clip->duration) + " s";
        return std::nullopt;
    }
    s.q.reserve(static_cast<std::size_t>(samples));
    for (int k = 0; k < samples; ++k) {
        const auto tracks = I::SampleClip(*clip, static_cast<double>(k) * Strike::SampleS);
        if (!tracks) {
            why = std::string(StrikeClip) + ": data out of bounds or of an unknown quantisation";
            return std::nullopt;
        }
        std::array<I::TrackPose, 4> pose{};
        pose[0] = {(*bones)[0].t, (*bones)[0].q};
        for (int i = 0; i < 3; ++i) {
            const auto& b = (*bones)[static_cast<std::size_t>(chain[static_cast<std::size_t>(i)])];
            pose[static_cast<std::size_t>(i + 1)] = {b.t, b.q};
        }
        for (std::size_t ti = 0; ti < tracks->size(); ++ti) {
            const int bone = ti < clip->track_to_bone.size() ? clip->track_to_bone[ti]
                                                             : static_cast<int>(ti);
            if (bone == 0) pose[0] = (*tracks)[ti];
            for (int i = 0; i < 3; ++i)
                if (bone == chain[static_cast<std::size_t>(i)])
                    pose[static_cast<std::size_t>(i + 1)] = (*tracks)[ti];
        }
        if (k == 0) first = pose;
        s.q.push_back({quat(pose[1].q), quat(pose[2].q), quat(pose[3].q)});
    }
    B::QsTransform handle{};
    handle.t = vec(first[0].t);
    handle.q = quat(first[0].q);
    if (!s.Analyze(handle, {vec(first[1].t), vec(first[2].t), vec(first[3].t)})) {
        why = std::string(StrikeClip) + ": no crack found in it";
        return std::nullopt;
    }
    return s;
}

} // namespace VR::AmygdalanArm
