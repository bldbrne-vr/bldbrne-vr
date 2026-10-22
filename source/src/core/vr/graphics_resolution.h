// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <algorithm>
#include <bit>
#include <cmath>
#include <iomanip>
#include <sstream>
#include "core/vr/graphics_settings.h"

namespace VR::Graphics {
// Bloodborne's generated VR patch allocates all scene, post and light-grid targets together.
// Keep the full-resolution chain at the output target; only scene raster uses the preset ratio.
// S1 (FAST bit 51, opt-in) reverses this: the whole chain at WholeFrameResolution, reconstructed
// after the composite (bbport's default for every output except 1080p).
inline Extent OutputResolution(Extent left, Extent right, int scale_percent,
                               Extent limit = {8192, 8192}) {
    if (!left.width || !left.height || !right.width || !right.height || limit.width < 64 ||
        limit.height < 64)
        return {};
    const auto dimension = [&](uint32_t a, uint32_t b, uint32_t maximum) {
        const double scale = (100. + std::clamp(scale_percent, -50, 50)) / 100.;
        const auto rounded = uint64_t(std::llround(std::max(a, b) * scale / 8.)) * 8;
        return uint32_t(std::clamp<uint64_t>(rounded, 64, maximum / 8 * 8));
    };
    return {dimension(left.width, right.width, limit.width),
            dimension(left.height, right.height, limit.height)};
}
inline Extent InputResolution(Extent output, Upscaler provider, Profile profile) {
    if (provider == Upscaler::Off)
        return output;
    const auto ratio = UpscaleRatios[std::min(unsigned(profile), 2u)];
    const auto even = [&](uint32_t x) {
        return x ? std::min(x, std::max(2u, uint32_t(std::lround(x * ratio * .5f)) * 2)) : 0u;
    };
    return {even(output.width), even(output.height)};
}
// S1 (graphics_settings.h): the guest's whole-frame size for an upscaled output. The preset's
// ratio (DLSS 5: the neural working scale, WholeFrameRatio), rounded to the 8-pixel alignment
// OutputResolution gives the template.
inline Extent WholeFrameResolution(Extent output, const Config& config) {
    if (config.upscaler == Upscaler::Off || !output.width || !output.height)
        return output;
    const double ratio = WholeFrameRatio(config);
    const auto dimension = [&](uint32_t x) {
        const auto rounded = uint32_t(std::lround(x * ratio / 8.)) * 8;
        return std::clamp<uint32_t>(rounded, std::min(x, 64u), x);
    };
    return {dimension(output.width), dimension(output.height)};
}
inline std::string Hex32(uint32_t value, bool bytes = false) {
    std::ostringstream text;
    if (bytes)
        value = (value >> 24) | ((value >> 8) & 0xFF00) | ((value << 8) & 0xFF0000) | (value << 24);
    else
        text << "0x";
    text << std::uppercase << std::hex << std::setfill('0') << std::setw(8) << value;
    return text.str();
}
// Only values in the audited Kyo-derived 01.00 VR template. No guest instructions are
// synthesized outside that template, and the caller validates the complete group first.
inline std::optional<std::string> ResolutionPatchValue(uint32_t address, std::string_view old,
                                                       Extent output) {
    const uint32_t ui_height = uint32_t(std::lround(1920. * output.height / output.width));
    const auto mov = [](uint32_t n, uint32_t opcode) { return Hex32((n << 8) | opcode); };
    switch (address) {
    case 0x0183A35D:
        return Hex32(std::bit_cast<uint32_t>(float(output.width) / output.height), true);
    case 0x02196A6B:
    case 0x02358554:
        return mov(output.width, 0xB8);
    case 0x02196A7A:
        return mov(output.height, 0xB8);
    case 0x0235855D:
        return mov(output.height, 0xB9);
    case 0x01FFD4D1:
    case 0x01A44365:
    case 0x01A44C63:
    case 0x01A452D5:
    case 0x02438BC7:
    case 0x02438F67:
        return mov(ui_height, 0xB8);
    case 0x0212C6A4:
        return mov(ui_height, 0xBE);
    case 0x02418498:
        return mov(ui_height, 0xB9);
    case 0x04D26EA8:
        return Hex32(std::bit_cast<uint32_t>(float(ui_height)));
    case 0x04CF9A14:
    case 0x04CF9A74:
    case 0x04CF9AE4:
        return Hex32(std::bit_cast<uint32_t>(540.f / ui_height));
    case 0x02695CB6:
        return "C783A8780000" + Hex32(output.width / 2, true);
    case 0x02695CC0:
        return "C783AC780000" + Hex32(output.height / 2, true);
    case 0x02594AA6: {
        // Two immediate dimensions within the template's display-buffer allocation stub.
        std::string value(old);
        if (value.size() < 64 || !value.starts_with("418D4DFF83F97F7718C78424A0000000") ||
            value.substr(40, 14) != "C78424A4000000")
            return {};
        value.replace(32, 8, Hex32(output.width, true));
        value.replace(54, 8, Hex32(output.height, true));
        return value;
    }
    default:
        return {};
    }
}
} // namespace VR::Graphics
