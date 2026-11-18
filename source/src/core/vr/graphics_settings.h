// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <array>
#include <atomic>
#include <cstdlib>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace VR::Graphics {
// Persisted IDs: keep existing providers stable when adding FSR 3.1.5. Fsr41: AMD's own FSR
// Upscaling 4.1 (FSR SDK 2.4.0's signed DLLs, the fsr41 bridge; AMD Radeon RX 6000 and newer),
// which runs the FSR 3.1.5 bridge where AMD offers no 4.1 (2026-10-31).
enum class Upscaler : int { Off = 0, Dlss = 1, Fsr4 = 2, Fsr31 = 3, Fsr41 = 4 };
inline constexpr bool IsFsr(Upscaler provider) {
    return provider == Upscaler::Fsr4 || provider == Upscaler::Fsr31 ||
           provider == Upscaler::Fsr41;
}
inline constexpr const char* UpscalerName(Upscaler provider) {
    switch (provider) {
    case Upscaler::Dlss: return "DLSS";
    case Upscaler::Fsr4: return "FSR 4";
    case Upscaler::Fsr31: return "FSR 3.1.5";
    case Upscaler::Fsr41: return "FSR 4.1.2";
    default: return "Off";
    }
}
// Persisted IDs: Ultra Performance and Ultra Quality were added after the first three, so
// they take 3 and 4; Balanced Plus (Balanced's size with DLSS model M) came later still: 5.
enum class Profile : int { Performance, Balanced, Quality, UltraPerformance, UltraQuality, BalancedPlus };
inline constexpr int ProfileCount = 6;
// Only ACCEPT publishes these preferences. Renderer changes are latched at frame boundaries.
struct Config {
    int scale_percent{};
    Upscaler upscaler{Upscaler::Off};
    Profile dlss{Profile::Balanced}, fsr4{Profile::Balanced}; // the defaults: Balanced (2026-10-17)
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
// geometry and art direction; headset tuning must precede enabling the model. Balanced renders
// at DLSS Balanced's 0.58 (2026-10-27: measured 88-90 fps at 2000x2140 per eye with the
// model on the centre, NeuralCentre).
inline constexpr std::array<NeuralProfile, 2> NeuralProfiles{
    {{0.50f, 100, 85, 50}, {0.58f, 100, 80, 85}}};
// The model works on the centre of each eye's render: this share of its width and height, at the
// render's own resolution, a multiple of 4 (the DLSS bridge's VR_DLSS5_FOVEA default; its own
// image there, fading into the game's over the region's edge, then DLSS Super Resolution).
inline constexpr float NeuralCentre = 0.66f;
inline constexpr uint32_t NeuralCentreSide(uint32_t full) {
    const uint32_t side = (uint32_t(float(full) * NeuralCentre) + 3u) & ~3u;
    return side < full ? side : full;
}
// The render scale of each preset (Profile order). Ultra Performance renders 40%, above the usual
// third: VR shows every pixel up close and these presets use the lighter DLSS model K
// (2026-10-22). Ultra Quality: the Quality scale with DLSS model M; FSR has no model to change,
// so its Ultra Quality renders 75% instead (Ratio; 77% until 2026-11-04). Balanced Plus: the
// Balanced scale with model M; FSR's renders 62% instead (about 14% more pixels than Balanced,
// halfway to Quality).
inline constexpr std::array<float, ProfileCount> UpscaleRatios{0.5f, 0.58f, 2.0f / 3.0f,
                                                              0.4f, 2.0f / 3.0f, 0.58f};
inline constexpr float FsrUltraQualityRatio = 0.75f;
inline constexpr float FsrBalancedPlusRatio = 0.62f;
// The DLSS bridge's quality (vr_dlss_bridge.h): 1 Quality, 2 Balanced, 3 Performance, 4 Ultra
// Performance (0, DLAA, is chosen by the caller for native input). Ultra Quality is Quality,
// Balanced Plus Balanced.
inline constexpr int DlssQuality(Profile p) {
    return p == Profile::UltraPerformance ? 4
           : p == Profile::UltraQuality   ? 1
           : p == Profile::BalancedPlus   ? 2
                                          : 3 - int(p);
}
// The FSR bridges' profile (vr_fsr4_bridge.h): 0 Performance, 1 Balanced, 2 Quality. The shipped
// FSR 4 runtime has no Ultra Performance model, so that preset uses the Performance one at its
// own render size, Ultra Quality the Quality one and Balanced Plus the Balanced one (FSR 3.1.5
// sizes from the images and ignores the profile).
inline constexpr uint32_t FsrProfile(Profile p) {
    return p == Profile::UltraPerformance ? 0u
           : p == Profile::UltraQuality   ? 2u
           : p == Profile::BalancedPlus   ? 1u
                                          : uint32_t(p);
}
// The DLSS model of a preset when vr.ini sets none (core/vr/dlss_model.h; NGX hint numbers):
// K (11, the 1st-generation transformer) everywhere, M (13, the 2nd generation) for Ultra
// Quality and Balanced Plus. Measured at 2000x2160 per eye on an RTX 5080: M 2.88 ms, K 1.70 ms
//.
inline constexpr uint32_t DlssModelForProfile(Profile p) {
    return p == Profile::UltraQuality || p == Profile::BalancedPlus ? 13u : 11u;
}
inline constexpr Profile NeuralPreset(Profile p) {
    return p == Profile::Performance ? p : Profile::Balanced;
}
inline constexpr Profile EffectiveProfile(const Config& c) {
    return IsFsr(c.upscaler) ? c.fsr4 : c.dlss5 ? NeuralPreset(c.neural) : c.dlss;
}
// A preset's render scale for a provider.
inline constexpr float Ratio(Upscaler provider, Profile p) {
    const unsigned i = unsigned(p);
    if (p == Profile::UltraQuality && IsFsr(provider))
        return FsrUltraQualityRatio;
    if (p == Profile::BalancedPlus && IsFsr(provider))
        return FsrBalancedPlusRatio;
    return UpscaleRatios[i < UpscaleRatios.size() ? i : 2];
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
// the game's own pixels; DLSS and FSR 4 use their preset ratio. VR_DLSS5_RENDER_SCALE=0.2..1
// (test only, with the bridge's VR_DLSS5_ORDER=overlay) renders DLSS 5 at another scale, e.g.
// 0.58 for DLSS Balanced's size.
inline float NeuralRenderScaleOverride() {
    static const float scale = [] {
        const char* v = std::getenv("VR_DLSS5_RENDER_SCALE");
        const float s = v ? float(std::atof(v)) : 0.0f;
        return s >= 0.2f && s <= 1.0f ? s : 0.0f;
    }();
    return scale;
}
inline float WholeFrameRatio(const Config& c) {
    const unsigned p = unsigned(EffectiveProfile(c));
    if (Neural(c) && NeuralRenderScaleOverride() > 0)
        return NeuralRenderScaleOverride();
    return Neural(c) ? NeuralProfiles[unsigned(NeuralPreset(c.neural))].working_scale
                     : Ratio(c.upscaler, Profile(p));
}
// In memory only (Serialize is what vr.ini keeps): 3 bits per preset since Ultra Quality. The
// upscaler's low two bits stay at 4 and its third (FSR 4.1) is bit 37, so 0..3 pack as before.
inline constexpr uint64_t Pack(const Config& c) {
    return uint64_t((c.scale_percent + 50) / 10) | (uint64_t(int(c.upscaler) & 3) << 4) |
           (uint64_t(c.dlss) << 6) | (uint64_t(c.fsr4) << 9) | (uint64_t(c.dlss5) << 12) |
           (uint64_t(NeuralPreset(c.neural)) << 13) | (uint64_t(c.intensity) << 16) |
           (uint64_t(c.detail) << 23) | (uint64_t(c.lighting) << 30) |
           (uint64_t((int(c.upscaler) >> 2) & 1) << 37);
}
inline Config Unpack(uint64_t v) {
    return {int(v & 15) * 10 - 50, Upscaler(((v >> 4) & 3) | (((v >> 37) & 1) << 2)),
            Profile((v >> 6) & 7),
            Profile((v >> 9) & 7), bool((v >> 12) & 1),    NeuralPreset(Profile((v >> 13) & 7)),
            int((v >> 16) & 127),  int((v >> 23) & 127),   int((v >> 30) & 127)};
}
// `accepted`: what the renderer runs. `saved`: what vr.ini and the VR page hold (its ACCEPT). The
// same until the game's resolution is applied at start (SetAppliedResolution, `held`); after that
// a change waits for SAVE AND RESTART, so the picture is always a whole setup (the upscaler had
// switched live on the start's input size, which only a restart changes; the tester, 2026-10-24).
// DLSS 5's intensity, detail and lighting stay live.
inline constinit std::atomic<uint64_t> accepted{Pack(Config{})};
inline constinit std::atomic<uint64_t> saved{Pack(Config{})};
inline std::atomic<bool> held{};
// DLSS 5 is offered on GeForce RTX 50-series cards only (2026-10-27; NVIDIA lists it
// for RTX 50): NVIDIA's vendor id and "RTX 50" with a model 5050..5090 in the name ("NVIDIA
// GeForce RTX 5080", "RTX 5070 Ti Laptop GPU"; not the workstation "RTX 5000 Ada Generation").
inline bool Rtx50(uint32_t vendor_id, std::string_view name) {
    if (vendor_id != 0x10de)
        return false;
    for (size_t at = name.find("RTX 50"); at != std::string_view::npos;
         at = name.find("RTX 50", at + 1)) {
        const size_t m = at + 6; // "RTX 50" then the model's last two digits
        if (m + 1 < name.size() && name[m] >= '5' && name[m] <= '9' && name[m + 1] == '0' &&
            (m + 2 == name.size() || name[m + 2] < '0' || name[m + 2] > '9'))
            return true;
    }
    return false;
}
// The renderer's GPU (vk_instance.cpp): -1 not yet known, 0 no DLSS 5, 1 an RTX 50 card.
inline std::atomic<int> neural_gpu{-1};
// The renderer's GPU is AMD's (vk_instance.cpp): -1 not yet known, 0 no, 1 yes. FSR 4.1 is
// offered on AMD cards only; AMD's loader then decides whether 4.1 runs (RX 6000 and newer) or
// the bridge's FSR 3.1.5 does.
inline std::atomic<int> amd_gpu{-1};
// The player's DLSS 5 file (vr_dlss_runtime.cpp VerifiedNeuralFile): -1 not checked yet, 0 none or
// refused, 1 NVIDIA's signed file.
inline std::atomic<int> neural_file{-1};
inline std::atomic<bool> restart_requested{};
inline std::atomic<const char*> restart_failure{};
inline Config Accepted() {
    return Unpack(accepted.load(std::memory_order_relaxed));
}
inline Config Saved() {
    return Unpack(saved.load(std::memory_order_relaxed));
}
// `c` with `live`'s DLSS 5 sliders (the part of a change that applies at once).
inline Config WithLiveSliders(Config c, const Config& live) {
    c.intensity = live.intensity;
    c.detail = live.detail;
    c.lighting = live.lighting;
    return c;
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
    if (n[0] != 1 || n[1] < -50 || n[1] > 50 || n[1] % 10 || n[2] < 0 || n[2] > 4 || n[3] < 0 ||
        n[3] >= ProfileCount || n[4] < 0 || n[4] >= ProfileCount || n[5] < 0 || n[5] > 1 || n[6] < 0 || n[6] > 2 ||
        n[7] < 0 || n[7] > 100 || n[8] < 0 || n[8] > 100 || n[9] < 0 || n[9] > 100)
        return {};
    return Config{n[1],          Upscaler(n[2]), Profile(n[3]), Profile(n[4]), bool(n[5]),
                  NeuralPreset(Profile(n[6])), n[7],           n[8],          n[9]};
}
inline bool Publish(std::string_view text) {
    const auto c = Parse(text);
    if (!c)
        return false;
    saved.store(Pack(*c), std::memory_order_relaxed);
    const Config live = held.load(std::memory_order_relaxed) ? WithLiveSliders(Accepted(), *c) : *c;
    accepted.store(Pack(live), std::memory_order_relaxed);
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
// The game's own render size as its resolution patch set it at start, headset-sized or the
// template's fixed size (0: no VR resolution patch). The HUD movie's slots follow its shape
// (core/vr/ui_slots.h).
inline std::atomic<uint64_t> game_extent{};
inline void SetGameExtent(Extent extent) {
    game_extent.store(uint64_t(extent.width) << 32 | extent.height, std::memory_order_relaxed);
}
inline Extent GameExtent() {
    const auto v = game_extent.load(std::memory_order_relaxed);
    return {uint32_t(v >> 32), uint32_t(v)};
}
// S1, FAST bit 51: on by default whenever DLSS, DLSS 5 or FSR 4 is selected (vr.ini
// upscale_whole_frame=0 or FAST_S1=0 turns it off; FAST_S1=1 or bit 51 in FAST_TOGGLES forces it):
// the generated VR patch is applied at the upscaler's input size and the renderer reconstructs
// the whole tonemapped frame to s1_output (vk_rasterizer_temporal.cpp VrS1Reconstruct). Read once,
// before the game's first instruction. 2000x2160 per eye: DLSS Performance 7.3 ms GPU vs 10.6
// on the scene-only path and 10.1 with SMAA.
inline std::atomic<bool> whole_frame_requested{true};
// vr.ini foveation (video_core/renderer_vulkan/vr_foveation.h): -1 not set (Auto: on where the card supports it), 0 off,
// 1 on, 2 performance, 3 light. Live: the next eye-sized pass takes it (vk_scheduler.cpp).
inline std::atomic<int> foveation{-1};
// DIBR (stereo_mode 2, one centre render) with DLSS, FSR 4 or FSR 3.1.5: the same whole-frame
// patch as S1 (SetS1), reconstructed once for the centre before the DIBR warp (renderer_vulkan/
// vr_dibr_upscale.cpp). DLSS 5 is not offered there. VR_DIBR_UPSCALE=0 (tests, A/B) keeps the old
// DIBR: the full-size render and its spatial AA, no provider loaded.
inline bool DibrUpscaleAllowed() {
    static const bool allowed = [] {
        const char* v = std::getenv("VR_DIBR_UPSCALE");
        return !(v && v[0] == '0');
    }();
    return allowed;
}
inline constexpr bool DibrUpscaleSupports(const Config& c) {
    return c.upscaler != Upscaler::Off && !(c.upscaler == Upscaler::Dlss && c.dlss5);
}
// DLSS's early start (VrDlssRuntime::PrepareDriverAddressSpace, before the game's patches): -1
// not tried, 0 failed (no NVIDIA RTX device, or its files missing), 1 ready. The DIBR patch only
// shrinks the render for a provider that can start, so a failed one leaves the full-size render.
inline std::atomic<int> dlss_early{-1};
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
// S1's texture mip bias: log2 of its render scale (-1.0 at DLSS Performance's 0.50), as NVIDIA's
// and AMD's upscaler guides ask, so material textures keep the output size's detail instead of
// the smaller input's. 0 when the game started without S1 (no upscaler: textures as before), or
// with VR_S1_MIP_BIAS=0. Set once at boot (SetS1), before the game makes its samplers.
inline float S1MipBias() {
    static const bool off = [] {
        const char* v = std::getenv("VR_S1_MIP_BIAS");
        return v && v[0] == '0';
    }();
    if (off || !S1Output().width)
        return 0.0f;
    const float ratio = WholeFrameRatio(Unpack(s1_config.load(std::memory_order_relaxed)));
    return ratio > 0.0f && ratio < 1.0f ? std::log2(ratio) : 0.0f;
}
inline bool ResolutionRestartRequired() {
    // Held: a saved choice the renderer does not run yet (Publish).
    if (held.load(std::memory_order_relaxed) && WithLiveSliders(Accepted(), Saved()) != Saved())
        return true;
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
    held.store(true, std::memory_order_relaxed); // later choices wait for a restart (Publish)
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
