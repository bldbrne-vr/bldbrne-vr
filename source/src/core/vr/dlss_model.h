// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <atomic>
#include <cctype>
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The DLSS Super Resolution model (vr.ini dlss_model; FAST_DLSS_PRESET with FAST bit 52 for
// A/B, video_core/renderer_vulkan/vr_dlss_preset.h). Header-only and pure apart from the
// published value. A change recreates both DLSS eyes through the asynchronous provider path
// (history restarts; FSR 4 and OFF never reconfigure).
//
// Values: 0 = BuiltIn, the bridge's own choice (VR_DLSS_SR_PRESET at its build: 13 = M in the
// dlss45 runtime) and the default, so a vr.ini without the key renders as before. Auto = NGX's
// per-mode default (the "Default" hint: K for DLAA/Quality/Balanced, M for Performance, L for
// Ultra Performance with the 310.5 DLL; NVIDIA may change it with a DLL update). 5..15 are
// NVSDK_NGX_DLSS_Hint_Render_Preset values; the selectable ones are the SDK's live presets:
// E, F (CNN, deprecated), J, K (1st-gen transformer), L, M (2nd-gen transformer).
namespace VR::Graphics {
inline constexpr uint32_t DlssModelBuiltIn = 0;
inline constexpr uint32_t DlssModelAuto = 0x100; // = VR_DLSS_SR_PRESET_AUTO in vr_dlss_bridge.h

inline constexpr bool DlssModelValid(uint32_t v) {
    return v == DlssModelBuiltIn || v == DlssModelAuto || v == 5 || v == 6 ||
           (v >= 10 && v <= 13);
}

// vr.ini / environment spelling: builtin | auto | E F J K L M (or the hint number). Case and
// surrounding spaces are ignored; anything else is nullopt.
inline std::optional<uint32_t> ParseDlssModel(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
        text.remove_prefix(1);
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
        text.remove_suffix(1);
    std::string word;
    for (const char c : text)
        word.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    if (word == "builtin" || word == "built-in" || word == "bridge")
        return DlssModelBuiltIn;
    if (word == "auto" || word == "default")
        return DlssModelAuto;
    uint32_t v{};
    if (word.size() == 1 && word[0] >= 'a' && word[0] <= 'z') {
        v = uint32_t(word[0] - 'a') + 1;
    } else {
        const auto [end, error] = std::from_chars(word.data(), word.data() + word.size(), v);
        if (error != std::errc{} || end != word.data() + word.size())
            return std::nullopt;
    }
    // A hint only (1..15): "0" and "256" are not spellings of builtin and auto.
    return v >= 1 && v <= 15 && DlssModelValid(v) ? std::optional<uint32_t>{v} : std::nullopt;
}

inline std::string DlssModelName(uint32_t v) {
    if (v == DlssModelBuiltIn)
        return "builtin";
    if (v == DlssModelAuto)
        return "auto";
    if (v >= 1 && v <= 26)
        return std::string(1, char('A' + v - 1));
    return std::to_string(v);
}

// vr.ini dlss_model, published by the settings parser; the renderer reads it at frame start.
inline std::atomic<uint32_t> dlss_model{DlssModelBuiltIn};
} // namespace VR::Graphics
