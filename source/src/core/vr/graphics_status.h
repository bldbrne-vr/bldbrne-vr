// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/vr/graphics_settings.h"
#include <cmath>

namespace VR::Graphics {
// One atomic snapshot: no locks, allocations or UI calls on the rendering thread.
// Dimensions use 14 bits each (the runtime caps targets at 8192).
enum class RunState : uint64_t { Waiting, Active, Retrying, Unavailable, Unsupported, Preparing };
struct RunStatus {
    Extent input{}, output{};
    RunState state{RunState::Waiting};
    Upscaler provider{Upscaler::Off};
    Profile profile{Profile::Quality};
    bool neural{};
};
inline constexpr uint64_t PackStatus(RunStatus s) {
    return (uint64_t(s.input.width) & 16383) | ((uint64_t(s.input.height) & 16383) << 14) |
           ((uint64_t(s.output.width) & 16383) << 28) |
           ((uint64_t(s.output.height) & 16383) << 42) | (uint64_t(s.state) << 56) |
           (uint64_t(s.provider) << 59) | (uint64_t(s.profile) << 61) | (uint64_t(s.neural) << 63);
}
inline constexpr RunStatus UnpackStatus(uint64_t v) {
    return {{uint32_t(v & 16383), uint32_t((v >> 14) & 16383)},
            {uint32_t((v >> 28) & 16383), uint32_t((v >> 42) & 16383)},
            RunState((v >> 56) & 7),
            Upscaler((v >> 59) & 3),
            Profile((v >> 61) & 3),
            bool(v >> 63)};
}
inline std::atomic<uint64_t> run_status{PackStatus({})};
static_assert(std::atomic<uint64_t>::is_always_lock_free);
inline void PublishStatus(const Config& c, RunState state, Extent input = {}, Extent output = {}) {
    run_status.store(PackStatus({input, output, state, c.upscaler,
                                 EffectiveProfile(c), c.dlss5}),
                     std::memory_order_relaxed);
}
inline std::string SizeText(Extent e) {
    return std::to_string(e.width) + "x" + std::to_string(e.height);
}
inline std::string StatusText(Config c, RunStatus s, Extent applied, bool restart) {
    const std::string name = c.upscaler == Upscaler::Fsr4 ? "FSR 4"
                             : c.dlss5                    ? "DLSS 5"
                                                          : "DLSS";
    const auto profile = EffectiveProfile(c);
    const bool matches = s.provider == c.upscaler && s.profile == profile && s.neural == c.dlss5;
    if (c.upscaler != Upscaler::Off && matches) {
        if (s.state == RunState::Unavailable)
            return name + " unavailable | Native fallback. SAVE AND RESTART to retry.";
        if (s.state == RunState::Unsupported)
            return name + " is not supported by this rendering mode or device.";
    }
    if (restart)
        return "Restart required | Current output " + SizeText(applied) + " / eye";
    if (c.upscaler == Upscaler::Off)
        return applied.width ? "Native rendering | " + SizeText(applied) + " / eye"
                             : "Upscaling Off";
    if (matches && s.state == RunState::Preparing)
        return name + " preparing | Native rendering until ready";
    if (!matches || s.state == RunState::Waiting)
        return name + " saved | Waiting for a complete scene";
    if (s.state == RunState::Retrying)
        return name + " | Reconstruction skipped: incomplete scene data";
    if (s.state == RunState::Active) {
        auto text = name + " active | " + SizeText(s.input) + " -> " + SizeText(s.output) + " / eye";
        if (c.dlss5 && c.upscaler == Upscaler::Dlss) {
            const float scale = NeuralProfiles[unsigned(NeuralPreset(c.neural))].working_scale;
            const Extent neural{uint32_t(std::lround(s.output.width * scale)),
                                uint32_t(std::lround(s.output.height * scale))};
            text += " | Neural " + SizeText(neural) + " / eye";
        }
        return text;
    }
    return name + " saved | Waiting for a complete scene";
}
inline std::string RuntimeStatusText() {
    return StatusText(Accepted(), UnpackStatus(run_status.load(std::memory_order_relaxed)),
                      AppliedResolution(), ResolutionRestartRequired());
}
} // namespace VR::Graphics
