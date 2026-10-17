// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <optional>
#include <string>
#include "common/logging/log.h"
#include "common/types.h"
#include "core/vr/dlss_model.h"
#include "core/vr/graphics_settings.h"
#include "video_core/fast_render.h"

namespace Vulkan {
// FAST bit 52 (DlssPreset): the DLSS Super Resolution model as a knob (research_scaled.md #6).
// The model comes from vr.ini dlss_model (core/vr/dlss_model.h; default builtin = the bridge's
// own choice, as before). With bit 52 on, FAST_DLSS_PRESET=<builtin|auto|E|F|J|K|L|M> replaces it,
// so one run can A/B the two models (--fast-ab <mask>,<mask|1<<52>:N). The selection rides in
// VrTemporalPair::Plan, so a change retires both DLSS eyes and recreates them on the provider's
// asynchronous initialization thread (native frames meanwhile, then history restarts). FSR 4
// and OFF never see it. The bridge takes it through its optional VrDlssSetSrPreset export
// (vr_dlss_bridge.h, ABI 2 kept): a bridge without the export keeps its built-in model and the
// counter line says bridge=fixed.
//
// No pixel verification: a different model is meant to produce a different image, and with
// the bit off and no vr.ini key the host passes 0 (built-in) and records the same commands as
// before. What can go wrong is the plumbing, so every activation logs the model asked for and
// the NGX hint the bridge says it will pass, and "FAST dlss_preset:" counts both.
static constexpr auto DlssPreset = static_cast<FastRender::Bit>(52);

// FAST_DLSS_PRESET, parsed once. nullopt: unset or invalid (an invalid value is logged).
inline std::optional<u32> DlssPresetEnv() {
    static const std::optional<u32> value = []() -> std::optional<u32> {
        const char* text = std::getenv("FAST_DLSS_PRESET");
        if (!text || !*text)
            return std::nullopt;
        if (const auto v = VR::Graphics::ParseDlssModel(text))
            return v;
        LOG_WARNING(Render_Vulkan,
                    "FAST dlss_preset: FAST_DLSS_PRESET={} ignored (builtin, auto, E, F, J, K, L "
                    "or M)",
                    text);
        return std::nullopt;
    }();
    return value;
}

// Bridge answers, written by VrDlssRuntime::Activate (the provider's initialization thread),
// read by the counter line (rendering thread).
enum : u32 { DlssPresetBridgeUnknown, DlssPresetBridgeApplied, DlssPresetBridgeFixed,
             DlssPresetBridgeRefused };
struct DlssPresetStats {
    std::atomic<u32> requested{};  // the selection of the last activation
    std::atomic<s32> hint{-1};     // the NGX hint the bridge reported for it (-1: none)
    std::atomic<u32> bridge{};     // DlssPresetBridge*
    std::atomic<u64> activations{}, refused{}, fixed{};
};
inline DlssPresetStats g_dlss_preset;

// The Plan's dlss_preset for this frame. Called once per frame on the rendering thread (the
// temporal frame start); also prints the counter line every 300 frames while the bit is on, the
// environment asks for a model or vr.ini selects one.
inline u32 DlssPresetForFrame(const VR::Graphics::Config& accepted, u64 frame) {
    const u32 ini = VR::Graphics::dlss_model.load(std::memory_order_relaxed);
    const auto env = DlssPresetEnv();
    const bool on = FastRender::On(DlssPreset);
    const u32 selected = on && env ? *env : ini;
    static u64 last_frame{};
    if (frame - last_frame >= 300) {
        last_frame = frame;
        const auto& s = g_dlss_preset;
        if (on || env || ini != VR::Graphics::DlssModelBuiltIn) {
            constexpr std::array<const char*, 4> bridges{"unknown", "applied", "fixed",
                                                         "refused"};
            LOG_INFO(Render_Vulkan,
                     "FAST dlss_preset: on={} env={} ini={} selected={} dlss={} active={} hint={} "
                     "bridge={} activations={} refused={} fixed={}",
                     on, env ? VR::Graphics::DlssModelName(*env) : std::string{"unset"},
                     VR::Graphics::DlssModelName(ini), VR::Graphics::DlssModelName(selected),
                     accepted.upscaler == VR::Graphics::Upscaler::Dlss,
                     VR::Graphics::DlssModelName(s.requested.load(std::memory_order_relaxed)),
                     s.hint.load(std::memory_order_relaxed),
                     bridges[std::min<u32>(s.bridge.load(std::memory_order_relaxed), 3)],
                     s.activations.load(std::memory_order_relaxed),
                     s.refused.load(std::memory_order_relaxed),
                     s.fixed.load(std::memory_order_relaxed));
        }
    }
    // Only DLSS carries it, so switching models never recreates FSR 4.
    return accepted.upscaler == VR::Graphics::Upscaler::Dlss ? selected
                                                             : VR::Graphics::DlssModelBuiltIn;
}

// VrDlssRuntime::Activate: what the bridge said about `selection` (its export's return value:
// the NGX hint, -1 refused; `exported` false: no export, the built-in model stays).
inline void DlssPresetNote(u32 selection, bool exported, s32 hint) {
    auto& s = g_dlss_preset;
    s.requested.store(selection, std::memory_order_relaxed);
    s.hint.store(exported ? hint : -1, std::memory_order_relaxed);
    const u32 bridge = !exported ? DlssPresetBridgeFixed
                       : hint < 0 ? DlssPresetBridgeRefused
                                  : DlssPresetBridgeApplied;
    s.bridge.store(bridge, std::memory_order_relaxed);
    s.activations.fetch_add(1, std::memory_order_relaxed);
    if (bridge == DlssPresetBridgeRefused)
        s.refused.fetch_add(1, std::memory_order_relaxed);
    if (bridge == DlssPresetBridgeFixed && selection != VR::Graphics::DlssModelBuiltIn)
        s.fixed.fetch_add(1, std::memory_order_relaxed);
    if (bridge == DlssPresetBridgeApplied)
        LOG_INFO(Render_Vulkan, "VR DLSS: model {} -> NGX preset hint {}",
                 VR::Graphics::DlssModelName(selection), hint);
    else if (selection != VR::Graphics::DlssModelBuiltIn)
        LOG_WARNING(Render_Vulkan, "VR DLSS: model {} not applied ({}); the bridge's built-in "
                                   "model stays",
                    VR::Graphics::DlssModelName(selection),
                    bridge == DlssPresetBridgeFixed ? "vr_dlss_stereo.dll has no VrDlssSetSrPreset"
                                                    : "the bridge refused it");
}
} // namespace Vulkan
