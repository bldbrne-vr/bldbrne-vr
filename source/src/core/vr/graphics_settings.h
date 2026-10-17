// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <atomic>
#include <cstdlib>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace VR::Graphics {
enum class Upscaler : int { Off, Dlss, Fsr4 };
enum class Profile : int { Performance, Balanced, Quality };
// Only ACCEPT publishes these preferences. Renderer changes are latched at frame boundaries.
struct Config {
    int scale_percent{};
    Upscaler upscaler{Upscaler::Off};
    Profile dlss{Profile::Balanced}, fsr4{Profile::Balanced};
    bool dlss5{};
    Profile neural{Profile::Balanced};
    int intensity{100}, detail{80}, lighting{85}; // DLSS 5 Enhancement at full strength by default
    bool operator==(const Config&) const = default;
};
struct NeuralProfile {
    float working_scale;
    int intensity, detail, lighting;
};
// Starting points, not measured quality/performance claims. Preserve Bloodborne's
// geometry and art direction; headset tuning must precede enabling the model.
inline constexpr std::array<NeuralProfile, 2> NeuralProfiles{
    {{0.50f, 100, 85, 50}, {0.67f, 100, 80, 85}}};
inline constexpr std::array<float, 3> UpscaleRatios{0.5f, 0.58f, 2.0f / 3.0f};
inline constexpr Profile NeuralPreset(Profile p) {
    return p == Profile::Performance ? p : Profile::Balanced;
}
inline constexpr Profile EffectiveProfile(const Config& c) {
    return c.upscaler == Upscaler::Fsr4 ? c.fsr4 : c.dlss5 ? NeuralPreset(c.neural) : c.dlss;
}
inline void SetNeuralProfile(Config& c, Profile p) {
    p = NeuralPreset(p);
    c.neural = p;
    const auto v = NeuralProfiles[static_cast<unsigned>(p)];
    c.intensity = v.intensity;
    c.detail = v.detail;
    c.lighting = v.lighting;
}
inline constexpr bool Neural(const Config& c) {
    return c.upscaler == Upscaler::Dlss && c.dlss5;
}
// S1 (FAST bit 51): the guest's whole-frame scale of the output. DLSS 5 renders at its neural
// profile's working scale (0.50 / 0.67), the size of the chain's model input, so the model sees
// the game's own pixels; DLSS and FSR 4 use their preset ratio.
inline constexpr float WholeFrameRatio(const Config& c) {
    const unsigned p = unsigned(EffectiveProfile(c));
    return Neural(c) ? NeuralProfiles[unsigned(NeuralPreset(c.neural))].working_scale
                     : UpscaleRatios[p > 2 ? 2 : p];
}
inline constexpr uint64_t Pack(const Config& c) {
    return uint64_t((c.scale_percent + 50) / 10) | (uint64_t(c.upscaler) << 4) |
           (uint64_t(c.dlss) << 6) | (uint64_t(c.fsr4) << 8) | (uint64_t(c.dlss5) << 10) |
           (uint64_t(NeuralPreset(c.neural)) << 11) | (uint64_t(c.intensity) << 13) | (uint64_t(c.detail) << 20) |
           (uint64_t(c.lighting) << 27);
}
inline Config Unpack(uint64_t v) {
    return {int(v & 15) * 10 - 50, Upscaler((v >> 4) & 3), Profile((v >> 6) & 3),
            Profile((v >> 8) & 3), bool((v >> 10) & 1),    NeuralPreset(Profile((v >> 11) & 3)),
            int((v >> 13) & 127),  int((v >> 20) & 127),   int((v >> 27) & 127)};
}
inline constinit std::atomic<uint64_t> accepted{Pack(Config{})};
inline std::atomic<bool> restart_requested{};
inline std::atomic<const char*> restart_failure{};
inline Config Accepted() {
    return Unpack(accepted.load(std::memory_order_relaxed));
}
inline std::string Serialize(const Config& c) {
    return "1," + std::to_string(c.scale_percent) + "," + std::to_string(int(c.upscaler)) + "," +
           std::to_string(int(c.dlss)) + "," + std::to_string(int(c.fsr4)) + "," +
           std::to_string(c.dlss5) + "," + std::to_string(int(NeuralPreset(c.neural))) + "," +
           std::to_string(c.intensity) + "," + std::to_string(c.detail) + "," +
           std::to_string(c.lighting);
}
inline std::optional<Config> Parse(std::string_view text) {
    std::array<int, 10> n{};
    for (size_t i = 0; i < n.size(); ++i) {
        const size_t end = text.find(',');
        if ((i + 1 == n.size()) != (end == text.npos))
            return {};
        const auto token = text.substr(0, end);
        const auto [p, e] = std::from_chars(token.data(), token.data() + token.size(), n[i]);
        if (e != std::errc{} || p != token.data() + token.size())
            return {};
        if (end != text.npos)
            text.remove_prefix(end + 1);
    }
    if (n[0] != 1 || n[1] < -50 || n[1] > 50 || n[1] % 10 || n[2] < 0 || n[2] > 2 || n[3] < 0 ||
        n[3] > 2 || n[4] < 0 || n[4] > 2 || n[5] < 0 || n[5] > 1 || n[6] < 0 || n[6] > 2 ||
        n[7] < 0 || n[7] > 100 || n[8] < 0 || n[8] > 100 || n[9] < 0 || n[9] > 100)
        return {};
    return Config{n[1],          Upscaler(n[2]), Profile(n[3]), Profile(n[4]), bool(n[5]),
                  NeuralPreset(Profile(n[6])), n[7],           n[8],          n[9]};
}
inline bool Publish(std::string_view text) {
    const auto c = Parse(text);
    if (!c)
        return false;
    accepted.store(Pack(*c), std::memory_order_relaxed);
    return true;
}
struct Extent {
    uint32_t width{}, height{};
};
inline std::array<std::atomic<uint64_t>, 2> headset{};
inline std::atomic<uint64_t> headset_limit{uint64_t(8192) << 32 | 8192};
inline void SetHeadsetLimit(uint32_t width, uint32_t height) {
    headset_limit.store(uint64_t(width) << 32 | height, std::memory_order_relaxed);
}
inline Extent HeadsetLimit() {
    const auto v = headset_limit.load(std::memory_order_relaxed);
    return {uint32_t(v >> 32), uint32_t(v)};
}
inline std::atomic<uint64_t> applied_resolution{};
inline std::atomic<int> applied_scale{1000};
// S1, FAST bit 51: on by default whenever DLSS, DLSS 5 or FSR 4 is selected (vr.ini
// upscale_whole_frame=0 or FAST_S1=0 turns it off; FAST_S1=1 or bit 51 in FAST_TOGGLES forces it):
// the generated VR patch is applied at the upscaler's input size and the renderer reconstructs
// the whole tonemapped frame to s1_output (vk_rasterizer_temporal.cpp VrS1Reconstruct). Read once,
// before the game's first instruction. 2000x2160 per eye: DLSS Performance 7.3 ms GPU vs 10.6
// on the scene-only path and 10.1 with SMAA.
inline std::atomic<bool> whole_frame_requested{true};
inline std::atomic<uint64_t> s1_output{};
inline std::atomic<uint64_t> s1_config{};
inline void SetS1(Extent output, const Config& c) {
    s1_config.store(Pack(c), std::memory_order_relaxed);
    s1_output.store(uint64_t(output.width) << 32 | output.height, std::memory_order_relaxed);
}
inline Extent S1Output() {
    const auto v = s1_output.load(std::memory_order_relaxed);
    return {uint32_t(v >> 32), uint32_t(v)};
}
inline bool ResolutionRestartRequired() {
    const int scale = applied_scale.load(std::memory_order_relaxed);
    if (scale != 1000 && scale != Accepted().scale_percent)
        return true;
    // S1: the guest's input size was patched for this provider's scale (WholeFrameRatio); OFF or
    // another scale (a preset change, or DLSS 5 <-> DLSS/FSR 4 at Balanced) keeps that size until
    // a restart. The renderer still reconstructs from the patched size meanwhile.
    if (S1Output().width) {
        const auto boot = Unpack(s1_config.load(std::memory_order_relaxed)), now = Accepted();
        return now.upscaler == Upscaler::Off || WholeFrameRatio(now) != WholeFrameRatio(boot);
    }
    // Booted without an upscaler (no S1 patch): picking DLSS, DLSS 5 or FSR 4 runs the
    // scene-only path until a restart applies S1, so say a restart is needed.
    const char* forced = std::getenv("FAST_S1");
    return Accepted().upscaler != Upscaler::Off && applied_scale.load(std::memory_order_relaxed) != 1000 &&
           whole_frame_requested.load(std::memory_order_relaxed) && !(forced && forced[0] == '0');
}
inline void SetAppliedResolution(Extent extent, int scale) {
    applied_resolution.store(uint64_t(extent.width) << 32 | extent.height, std::memory_order_relaxed);
    applied_scale.store(scale, std::memory_order_relaxed);
}
inline Extent AppliedResolution() {
    const auto v = applied_resolution.load(std::memory_order_relaxed);
    return {uint32_t(v >> 32), uint32_t(v)};
}
inline void SetHeadset(unsigned eye, uint32_t width, uint32_t height) {
    if (eye < 2)
        headset[eye].store(uint64_t(width) << 32 | height, std::memory_order_relaxed);
}
inline Extent Headset(unsigned eye) {
    if (eye >= 2)
        return {};
    const auto v = headset[eye].load(std::memory_order_relaxed);
    return {uint32_t(v >> 32), uint32_t(v)};
}
inline std::string ResolutionText() {
    const auto l = Headset(0), r = Headset(1);
    if (!l.width || !l.height || !r.width || !r.height)
        return "Headset not connected";
    const auto size = [](Extent e) {
        return std::to_string(e.width) + " x " + std::to_string(e.height);
    };
    return l.width == r.width && l.height == r.height ? size(l) + " / eye"
                                                      : "L " + size(l) + "; R " + size(r);
}
} // namespace VR::Graphics
