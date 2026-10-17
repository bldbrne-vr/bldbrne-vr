// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include "common/logging/log.h"
#include "core/vr/vr_session.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/texture_cache/sampler.h"

namespace VideoCore {

namespace {

// VR picture quality (vr.ini texture_aniso / texture_lod_bias, documents/bloodborne-vr.md
// "Sharpness"): what the game's sampler gets on top of its own settings.
struct VrFiltering {
    bool force_aniso{false};
    float aniso{1.0f};
    float lod_bias{0.0f};
};

// A filtered texture the game samples with screen-space derivatives: a linear minification
// filter, normalized coordinates, no depth compare (shadow maps), a plain blend reduction (not a
// min/max depth pyramid). Point-sampled buffers keep their exact texels.
bool IsFilteredTexture(const AmdGpu::Sampler& sampler) {
    const auto min = sampler.xy_min_filter.Value();
    return (min == AmdGpu::Filter::Bilinear || min == AmdGpu::Filter::AnisoLinear) &&
           sampler.force_unnormalized == 0 &&
           sampler.depth_compare_func == AmdGpu::DepthCompare::Never &&
           sampler.filter_mode == AmdGpu::FilterMode::Blend;
}

// A material texture: mipmapped and tiling (wrap or mirror on both axes). The game's render
// targets and screen-space buffers clamp.
bool IsTilingMipmapped(const AmdGpu::Sampler& sampler) {
    const auto tiles = [](AmdGpu::ClampMode m) {
        return m == AmdGpu::ClampMode::Wrap || m == AmdGpu::ClampMode::Mirror;
    };
    return sampler.mip_filter != AmdGpu::MipFilter::None && tiles(sampler.clamp_x) &&
           tiles(sampler.clamp_y);
}

VrFiltering VrFilteringFor(const AmdGpu::Sampler& sampler) {
    VrFiltering out{};
    if (!VR::IsEnabled() || !IsFilteredTexture(sampler)) {
        return out;
    }
    const auto& s = VR::GetSettings();
    if (s.texture_aniso >= 2) {
        out.force_aniso = true;
        out.aniso = static_cast<float>(s.texture_aniso);
    }
    if (s.texture_lod_bias != 0.0f && IsTilingMipmapped(sampler)) {
        out.lod_bias = s.texture_lod_bias;
    }
    return out;
}

// Counted for the log (at 1, 2, 4, 8, ... samplers, then every 256th).
std::atomic<u32> made_count{0};
std::atomic<u32> filtered_count{0};
std::atomic<u32> game_aniso_count{0};
std::atomic<u32> forced_count{0};
std::atomic<u32> biased_count{0};

} // Anonymous namespace

Sampler::Sampler(const Vulkan::Instance& instance, const AmdGpu::Sampler& sampler,
                 const AmdGpu::BorderColorBuffer border_color_base) {
    using namespace Vulkan;
    const VrFiltering vr = VrFilteringFor(sampler);
    const bool game_aniso = AmdGpu::IsAnisoFilter(sampler.xy_mag_filter) ||
                            AmdGpu::IsAnisoFilter(sampler.xy_min_filter);
    const bool anisotropy_enable =
        instance.IsAnisotropicFilteringSupported() && (game_aniso || vr.force_aniso);
    const float wanted = std::max(game_aniso ? sampler.MaxAniso() : 1.0f, vr.aniso);
    const float max_anisotropy =
        anisotropy_enable ? std::clamp(wanted, 1.0f, instance.MaxSamplerAnisotropy()) : 1.0f;
    if (VR::IsEnabled()) {
        const u32 n = made_count.fetch_add(1) + 1;
        filtered_count += IsFilteredTexture(sampler) ? 1 : 0;
        game_aniso_count += game_aniso ? 1 : 0;
        forced_count += vr.force_aniso && anisotropy_enable &&
                                max_anisotropy > (game_aniso ? sampler.MaxAniso() : 1.0f)
                            ? 1
                            : 0;
        biased_count += vr.lod_bias != 0.0f ? 1 : 0;
        static const bool each = std::getenv("VR_SAMPLER_LOG") != nullptr;
        if (each) {
            // VR_SAMPLER_LOG=1: every sampler the game makes, as the guest describes it.
            LOG_INFO(Render_Vulkan,
                     "VR sampler {}: clamp {}/{}/{}, filter min {} mag {} mip {}, aniso ratio {}{}, "
                     "lod bias {:.2f} (+{:.2f}), lod {:.2f}..{:.2f}, compare {}, mode {}{}",
                     n, static_cast<u32>(sampler.clamp_x.Value()),
                     static_cast<u32>(sampler.clamp_y.Value()),
                     static_cast<u32>(sampler.clamp_z.Value()),
                     static_cast<u32>(sampler.xy_min_filter.Value()),
                     static_cast<u32>(sampler.xy_mag_filter.Value()),
                     static_cast<u32>(sampler.mip_filter.Value()),
                     static_cast<u32>(sampler.max_aniso.Value()), // 0..4 = 1x..16x
                     game_aniso ? " (on)" : "", sampler.LodBias(), vr.lod_bias, sampler.MinLod(),
                     sampler.MaxLod(), static_cast<u32>(sampler.depth_compare_func.Value()),
                     static_cast<u32>(sampler.filter_mode.Value()),
                     anisotropy_enable ? fmt::format(", made with {}x", max_anisotropy) : "");
        }
        if ((n & (n - 1)) == 0 || n % 256 == 0) { // 1, 2, 4, 8, ... and every 256th
            LOG_INFO(Render_Vulkan,
                     "VR texture filtering: {} samplers made, {} filtered textures ({} with the "
                     "game's own anisotropy), {} raised to {}x anisotropic, {} with the mip bias "
                     "{:+.2f} (vr.ini texture_aniso={} texture_lod_bias={})",
                     n, filtered_count.load(), game_aniso_count.load(), forced_count.load(),
                     VR::GetSettings().texture_aniso, biased_count.load(),
                     VR::GetSettings().texture_lod_bias, VR::GetSettings().texture_aniso,
                     VR::GetSettings().texture_lod_bias);
        }
    }
    auto border_color = LiverpoolToVK::BorderColor(sampler.border_color_type);
    if (border_color == vk::BorderColor::eFloatCustomEXT &&
        !instance.IsCustomBorderColorSupported()) {
        LOG_WARNING(Render_Vulkan, "Custom border color is not supported, falling back to black");
        border_color = vk::BorderColor::eFloatOpaqueBlack;
    }

    const auto custom_color = [&]() -> std::optional<vk::SamplerCustomBorderColorCreateInfoEXT> {
        if (border_color == vk::BorderColor::eFloatCustomEXT) {
            const auto border_color_index = sampler.border_color_ptr.Value();
            const auto border_color_buffer = border_color_base.Address<std::array<float, 4>*>();
            const auto custom_border_color_array = border_color_buffer[border_color_index];

            const vk::SamplerCustomBorderColorCreateInfoEXT ret{
                .customBorderColor =
                    vk::ClearColorValue{
                        .float32 = custom_border_color_array,
                    },
                .format = vk::Format::eR32G32B32A32Sfloat,
            };
            return ret;
        } else {
            return std::nullopt;
        }
    }();

    const vk::SamplerCreateInfo sampler_ci = {
        .pNext = custom_color ? &*custom_color : nullptr,
        .magFilter = LiverpoolToVK::Filter(sampler.xy_mag_filter),
        .minFilter = LiverpoolToVK::Filter(sampler.xy_min_filter),
        .mipmapMode = LiverpoolToVK::MipFilter(sampler.mip_filter),
        .addressModeU = LiverpoolToVK::ClampMode(sampler.clamp_x),
        .addressModeV = LiverpoolToVK::ClampMode(sampler.clamp_y),
        .addressModeW = LiverpoolToVK::ClampMode(sampler.clamp_z),
        .mipLodBias = std::min(sampler.LodBias() + vr.lod_bias, instance.MaxSamplerLodBias()),
        .anisotropyEnable = anisotropy_enable,
        .maxAnisotropy = max_anisotropy,
        .compareEnable = sampler.depth_compare_func != AmdGpu::DepthCompare::Never,
        .compareOp = LiverpoolToVK::DepthCompare(sampler.depth_compare_func),
        .minLod = sampler.MinLod(),
        .maxLod = sampler.MaxLod(),
        .borderColor = border_color,
        .unnormalizedCoordinates = false, // Handled in shader due to Vulkan limitations.
    };
    auto [sampler_result, smplr] = instance.GetDevice().createSamplerUnique(sampler_ci);
    ASSERT_MSG(sampler_result == vk::Result::eSuccess, "Failed to create sampler: {}",
               vk::to_string(sampler_result));
    handle = std::move(smplr);
}

Sampler::~Sampler() = default;

} // namespace VideoCore
