// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
namespace VR::Bloodborne {
// Opt-in grip lab snapshot, read on the game's render thread after GetDmyPolyMtx.
struct GripLabPose {
    std::array<float, 12> world{};
    // translation xyz, quaternion xyzw, scale xyz, in Havok's 108-bone order.
    std::array<std::array<float, 10>, 108> local{}, model{};
};
bool ReadGripLabPose(GripLabPose& out);
}
