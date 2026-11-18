// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>

namespace VR::PoisonKnife {
// Separate cache entry: the ordinary throwing knife keeps its native material.
inline constexpr int Model = 91016, NativeModel = 10016;
inline constexpr std::uint32_t Size = 512, Levels = 10;
inline constexpr std::size_t Bytes = 174776;
constexpr int SourceModel(int id) { return id == Model ? NativeModel : id; }

inline bool DdsValid(std::span<const char> bytes) {
    if (bytes.size() != 128 + Bytes) return false;
    const auto word = [&](std::size_t at) {
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i)
            v |= std::uint32_t(static_cast<unsigned char>(bytes[at+i])) << (8*i);
        return v;
    };
    return word(0) == 0x20534444 && word(4) == 124 && word(12) == Size &&
           word(16) == Size && word(28) == Levels && word(76) == 32 &&
           word(80) == 4 && word(84) == 0x31545844 && word(112) == 0;
}

// Projection baked once on the asset worker. No changed geometry, vertex splits,
// shader branches or per-frame work. UV v=0 is the atlas top.
inline std::array<float, 2> UV(const float* p) {
    constexpr float top = .310635f, seam = .03157f, bottom = -.096966f;
    const float v = p[1] >= seam ? .70f * (top-p[1]) / (top-seam)
                                : .70f + .30f * (seam-p[1]) / (seam-bottom);
    // Give the narrow blade most of the atlas width instead of wasting its detail
    // on the wide crossguard. Smooth the transition along the blade's root.
    const float blend = std::clamp((p[1]-.085f)/.025f, 0.f, 1.f);
    const float width = p[1] < seam ? .030f : .120f + (.035f-.120f)*blend;
    return {std::clamp(.5f + p[0]/width, .002f, .998f), std::clamp(v, .002f, .998f)};
}

template<class Vertices>
bool ApplyUVs(Vertices& vertices) {
    if (vertices.empty()) return false;
    std::array<float, 3> lo{1,1,1}, hi{-1,-1,-1};
    for (const auto& v : vertices) {
        for (int i = 0; i < 3; ++i) {
            if (!std::isfinite(v.position[i])) return false;
            lo[i] = std::min(lo[i], v.position[i]);
            hi[i] = std::max(hi[i], v.position[i]);
        }
    }
    // Reject an unfamiliar layout before touching UVs; keep the native texture.
    constexpr std::array<float,3> expected_lo{-.055441f,-.096966f,-.014280f};
    constexpr std::array<float,3> expected_hi{ .055462f, .310635f, .014525f};
    for (int i = 0; i < 3; ++i)
        if (std::abs(lo[i]-expected_lo[i]) > .003f ||
            std::abs(hi[i]-expected_hi[i]) > .003f) return false;
    for (auto& v : vertices) {
        const auto uv = UV(v.position);
        v.uv[0] = uv[0]; v.uv[1] = uv[1];
    }
    return true;
}
} // namespace VR::PoisonKnife
