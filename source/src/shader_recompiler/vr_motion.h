// SPDX-License-Identifier: GPL-2.0-or-later
// Adapted from bbport's object-motion path (see scratchpad/temporal-staging/IMPORT.json).
#pragma once
#include <cstdint>

namespace Shader {
struct VrMotion {
    // Leave sixteen components for built-in interfaces on 128-component hardware.
    // Locations 30/31 exceed that limit once Position/FragCoord/ViewIndex are included.
    static constexpr uint32_t CurrentLocation = 26, PreviousLocation = 27, Output = 7;
    static_assert((PreviousLocation + 1) * 4 + 16 <= 128);
    static constexpr uint32_t FlagStore = 1, FlagLoad = 2;
    // Native vr_hud is an integer encoded as a float. Low bits belong to stereo
    // world/camera/projection flags; upper bits carry the adjacent eye parameter pair.
    // Stay below 2^24 so both fields survive float conversion exactly.
    static constexpr uint32_t ParameterShift = 3, NativeFlagsMask = 7;
    static constexpr uint32_t MaxParameterIndex = (1u << (24 - ParameterShift)) - 1;
    static constexpr float PackPush(uint32_t native_flags, uint32_t pair_index) {
        return float((native_flags & NativeFlagsMask) |
                     ((pair_index <= MaxParameterIndex ? pair_index : 0) << ParameterShift));
    }
    static constexpr uint32_t ParameterIndex(float packed) {
        return uint32_t(packed) >> ParameterShift;
    }
    // Two adjacent entries per draw: left then right; entries 0 and 1 are disabled.
    struct alignas(16) Params {
        uint32_t store_base{}, load_base{}, vertices{}, flags{};
        uint32_t first_vertex{}, first_instance{}, instances{}, reserved{};
    };
    // VrObjectMotion's buffers. With Shader::VrPushData each draw pushes their addresses and a
    // permutation keeps only `enabled` (Variant), cached on disk like any other. Without it the
    // addresses are part of the specialization, baked into the shader: such a permutation is
    // never written to disk (cached shaders cannot silently retain the addresses of a retired
    // motion-buffer allocation).
    uint64_t params_address{}, positions_address{};
    bool enabled{};
    // The addresses are baked into this permutation (no VrPushData): never cached on disk.
    bool Baked() const {
        return params_address != 0 || positions_address != 0;
    }
    // What a motion variant is built and kept with (PipelineCache::VrMotionVariant): with the
    // addresses pushed (`pushed`) only the flag, so every allocation finds the same permutation
    // (and the one cached on disk); otherwise the addresses too.
    VrMotion Variant(bool pushed) const {
        if (!enabled || !pushed) {
            return *this;
        }
        VrMotion variant{};
        variant.enabled = true;
        return variant;
    }
    bool operator==(const VrMotion&) const = default;
};
static_assert(sizeof(VrMotion::Params) == 32);
} // namespace Shader
