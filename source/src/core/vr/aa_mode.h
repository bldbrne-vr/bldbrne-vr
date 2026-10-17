// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The VR eyes' anti-aliasing (vr.ini aa_mode, the VR page's Graphics > Anti-aliasing), header-only
// and pure (unit tested in tests/test_aa_mode.cpp). The game's own AA is FXAA 3.11 "console": two
// full-screen draws at the end of its post chain, a copy of the scene colour A into B and the FXAA
// draw (fragment shader 0x30a78adffc8, vr.ini aa_fxaa_fs) from B back into A, which the composite,
// the HUD and our DIBR capture then read. The rasterizer (vk_rasterizer_vr_aa.cpp) decides at that
// draw, every frame, so a change applies at the next frame (no restart, no game patch):
//   fxaa: the game's FXAA as it is (exactly the build before aa_mode),
//   smaa: the FXAA draw is replaced by SMAA 1x (renderer_vulkan/vr_smaa.h) from B into A (the
//         default since 2026-10-07: the player's pick, sharper; a draw SMAA refuses, or a Vulkan
//         failure, draws the game's FXAA instead and logs why),
//   taa:  temporal AA in the game frame (2026-10-08; renderer_vulkan/vr_render_taa.h,
//         core/vr/render_taa.h): the game's render camera is jittered by a sub-pixel step every
//         frame (Halton 2,3, in the vertex epilogue's clip space: the game's matrices, the camera
//         record, the DIBR and the late warp stay unjittered), and the FXAA draw is replaced by a
//         resolve from B into A that reprojects its history with the frame's depth and the exact
//         previous / current render cameras, clips it to the current 3x3 neighbourhood (YCoCg
//         variance box) and blends: the edges converge to a steady anti-aliased result whether
//         the head is still or moving. A frame it refuses gets SMAA (or the FXAA).
//   off:  the FXAA draw is skipped: A keeps the scene as rendered (the same pixels as the preset's
//         "Disable AA" patch) and the eyes' own temporal AA (eye_taa) does the work. vr.ini only
//         (experiments; until 2026-10-08 this was aa_mode=taa): the VR page shows it as
//         "Eyes' TAA only".
// Only in immersive DIBR frames: the flat screen, cinematic screens and a session without a
// headset keep the game's FXAA whatever the setting.
namespace VR::Aa {

enum class Mode : std::uint8_t { Fxaa = 0, Smaa = 1, Taa = 2, Off = 3 };

inline constexpr std::uint64_t DefaultFxaaFs = 0x30a78adffc8; // Bloodborne 1.09's FXAA draw

// The code default (vr.ini without aa_mode, the VR page's Defaults): SMAA; aa_mode=fxaa restores
// the game's FXAA.
inline constexpr Mode DefaultMode = Mode::Smaa;

// The vr.ini word of a mode.
inline const char* Name(Mode mode) {
    switch (mode) {
    case Mode::Fxaa:
        return "fxaa";
    case Mode::Smaa:
        return "smaa";
    case Mode::Taa:
        return "taa";
    case Mode::Off:
        return "off";
    }
    return "fxaa";
}

// vr.ini aa_mode: fxaa (or game) | smaa | taa (the game frame's temporal AA) | off (or none: the
// game's AA off, the eyes' TAA only); case and surrounding spaces ignored. Anything else: nullopt
// (the setting is unchanged).
inline std::optional<Mode> ParseMode(std::string_view text) {
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
        text.remove_prefix(1);
    }
    while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
        text.remove_suffix(1);
    }
    std::string word{text};
    std::ranges::transform(word, word.begin(),
                           [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (word == "fxaa" || word == "game" || word == "0") {
        return Mode::Fxaa;
    }
    if (word == "smaa" || word == "1") {
        return Mode::Smaa;
    }
    if (word == "taa" || word == "2") {
        return Mode::Taa;
    }
    if (word == "off" || word == "none" || word == "3") {
        return Mode::Off;
    }
    return std::nullopt;
}

// What the rasterizer does with a draw (Decide).
enum class Action : std::uint8_t {
    Draw, // draw it as the game does (not the FXAA draw, or aa_mode=fxaa, or not immersive)
    Skip, // the FXAA draw is not drawn (aa_mode=off)
    Smaa, // the FXAA draw is replaced by SMAA from its input into its target (aa_mode=smaa)
    Taa,  // the FXAA draw is replaced by the render TAA's resolve (aa_mode=taa)
};

struct DrawFacts {
    std::uint64_t fs_hash{}; // the draw's fragment shader (pipeline key stage hash)
    std::uint64_t fxaa_fs{DefaultFxaaFs};
    bool immersive{false}; // immersive DIBR or native stereo frame
    bool upscaler{false}; // this frame was reconstructed by DLSS/FSR: bypass spatial AA, preserve the saved mode
};

// The decision for one draw before its resources are bound: SMAA still needs Usable() on the
// draw's images (else the FXAA draw is drawn after all).
inline Action Decide(Mode mode, const DrawFacts& d) {
    if (d.fs_hash == 0 || d.fs_hash != d.fxaa_fs || !d.immersive) {
        return Action::Draw;
    }
    if (d.upscaler) return Action::Skip;
    switch (mode) {
    case Mode::Fxaa:
        return Action::Draw;
    case Mode::Smaa:
        return Action::Smaa;
    case Mode::Taa:
        return Action::Taa;
    case Mode::Off:
        return Action::Skip;
    }
    return Action::Draw;
}

// The FXAA draw's images as SMAA would use them: its one texture (B) and its colour target (A).
struct DrawImages {
    bool has_texture{false};
    bool has_target{false};
    bool same_image{false}; // B is A (a feedback loop: never in Bloodborne)
    std::uint32_t texture_w{}, texture_h{};
    std::uint32_t target_w{}, target_h{};
    std::uint32_t texture_bytes_per_texel{}; // 4: an 8-bit RGBA / BGRA format
    std::uint32_t target_bytes_per_texel{};
    std::uint32_t target_samples{1};
};

// Whether SMAA can stand in for the FXAA draw: one 8-bit 4-channel texture and target of the same
// size, single-sampled, distinct. `why` says what refused it (for the log).
inline bool Usable(const DrawImages& d, std::string* why = nullptr) {
    const auto no = [&](const char* what) {
        if (why != nullptr) {
            *why = what;
        }
        return false;
    };
    if (!d.has_texture || !d.has_target) {
        return no("the FXAA draw has no texture or no colour target");
    }
    if (d.same_image) {
        return no("the FXAA draw reads its own target");
    }
    if (d.texture_bytes_per_texel != 4 || d.target_bytes_per_texel != 4) {
        return no("the FXAA images are not 8-bit RGBA");
    }
    if (d.texture_w != d.target_w || d.texture_h != d.target_h || d.target_w == 0 ||
        d.target_h == 0) {
        return no("the FXAA texture and target differ in size");
    }
    if (d.target_samples != 1) {
        return no("the FXAA target is multisampled");
    }
    return true;
}

// ---- A CPU mirror of SMAA's luma edge detection (SMAALumaEdgeDetectionPS, SMAA.hlsl) -----------
// For the tests and the docs: the shader runs the reference code itself (host_shaders/smaa).

inline constexpr float LumaR = 0.2126f, LumaG = 0.7152f, LumaB = 0.0722f;
inline constexpr float SmaaThreshold = 0.1f;       // SMAA_PRESET_HIGH
inline constexpr float LocalContrastFactor = 2.0f; // SMAA_LOCAL_CONTRAST_ADAPTATION_FACTOR

// Luma of gamma-encoded (sRGB) values 0..1 in R, G, B order; `swapped`: the values are B, G, R
// (a B8G8R8A8 image read as R8G8B8A8, vr_smaa.comp's flag 1).
inline float Luma(float r, float g, float b, bool swapped = false) {
    return swapped ? LumaR * b + LumaG * g + LumaB * r : LumaR * r + LumaG * g + LumaB * b;
}

struct Edges {
    bool left{false}; // an edge between this pixel and the one to its left (SMAA's edges.r)
    bool top{false};  // ... and the one above it (edges.g)
    bool operator==(const Edges&) const = default;
};

// One pixel's edges from the lumas around it, exactly as the shader: the threshold on the left /
// top differences, then the local contrast adaptation (an edge is dropped when a neighbouring
// difference - right, bottom, left-left, top-top - is more than twice as large).
inline Edges LumaEdges(float l, float left, float top, float right, float bottom, float leftleft,
                       float toptop, float threshold = SmaaThreshold) {
    const float dl = std::abs(l - left);
    const float dt = std::abs(l - top);
    bool e_left = dl >= threshold; // step(threshold, delta)
    bool e_top = dt >= threshold;
    if (!e_left && !e_top) {
        return {};
    }
    float max_x = std::max(dl, std::abs(l - right));
    float max_y = std::max(dt, std::abs(l - bottom));
    max_x = std::max(max_x, std::abs(left - leftleft));
    max_y = std::max(max_y, std::abs(top - toptop));
    const float final_delta = std::max(max_x, max_y);
    e_left = e_left && LocalContrastFactor * dl >= final_delta;
    e_top = e_top && LocalContrastFactor * dt >= final_delta;
    return {e_left, e_top};
}

// ---- The log line of a mode change ----

inline std::string Describe(Mode mode) {
    switch (mode) {
    case Mode::Fxaa:
        return "the game's FXAA";
    case Mode::Smaa:
        return "SMAA 1x in place of the game's FXAA";
    case Mode::Taa:
        return "temporal AA in the game frame (jittered) in place of the game's FXAA";
    case Mode::Off:
        return "the game's FXAA off, the eyes' TAA only";
    }
    return {};
}

} // namespace VR::Aa
