#include "../../../scratchpad/dual-eye-perf/asymmetric_projection.h"
#include "../../../scratchpad/dual-eye-perf/capture_marker.h"
// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "../../../scratchpad/dual-eye-perf/native_stereo.h"
#include "common/debug.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "core/debug_state.h"
#include "core/devtools/layer.h"
#include "core/emulator_settings.h"
#include "core/libraries/system/systemservice.h"
#include "core/vr/desktop_mirror.h"
#include "core/vr/graphics_settings.h"
#include "core/vr/live_tune.h"
#include "imgui/friends_layer.h"
#include "imgui/invitation_prompt_layer.h"
#include "imgui/notifications_layer.h"
#include "imgui/renderer/imgui_core.h"
#include "imgui/renderer/imgui_impl_vulkan.h"
#include "imgui/shadnet_notifications_layer.h"
#include "sdl_window.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/renderdoc.h"
#include "video_core/renderer_vulkan/openxr_presenter.h"
#include "video_core/renderer_vulkan/vk_gpu_frame_timer.h"
#include "video_core/renderer_vulkan/vk_gpu_pass_profiler.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "core/vr/vr_page_runtime.h"
#include "video_core/renderer_vulkan/vk_presenter.h"
#include "video_core/renderer_vulkan/vk_rasterizer.h"
#include "video_core/renderer_vulkan/vk_submit_lock_trace.h"
#include "video_core/texture_cache/image.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <csetjmp>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <span>
#include <sstream>
#include <system_error>
#include <vector>
#include <imgui.h>
#include <stb_image_write.h>
#include <vk_mem_alloc.h>

namespace Vulkan {

bool CanBlitToSwapchain(const vk::PhysicalDevice physical_device, vk::Format format) {
    const vk::FormatProperties props{physical_device.getFormatProperties(format)};
    return static_cast<bool>(props.optimalTilingFeatures & vk::FormatFeatureFlagBits::eBlitDst);
}

[[nodiscard]] vk::ImageSubresourceLayers MakeImageSubresourceLayers() {
    return vk::ImageSubresourceLayers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlit(s32 frame_width, s32 frame_height, s32 dst_width,
                                          s32 dst_height, s32 offset_x, s32 offset_y) {
    return vk::ImageBlit{
        .srcSubresource = MakeImageSubresourceLayers(),
        .srcOffsets =
            std::array{
                vk::Offset3D{
                    .x = 0,
                    .y = 0,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = frame_width,
                    .y = frame_height,
                    .z = 1,
                },
            },
        .dstSubresource = MakeImageSubresourceLayers(),
        .dstOffsets =
            std::array{
                vk::Offset3D{
                    .x = offset_x,
                    .y = offset_y,
                    .z = 0,
                },
                vk::Offset3D{
                    .x = offset_x + dst_width,
                    .y = offset_y + dst_height,
                    .z = 1,
                },
            },
    };
}

[[nodiscard]] vk::ImageBlit MakeImageBlitStretch(s32 frame_width, s32 frame_height,
                                                 s32 swapchain_width, s32 swapchain_height) {
    return MakeImageBlit(frame_width, frame_height, swapchain_width, swapchain_height, 0, 0);
}

static vk::Rect2D FitImage(s32 frame_width, s32 frame_height, s32 swapchain_width,
                           s32 swapchain_height) {
    float frame_aspect = static_cast<float>(frame_width) / frame_height;
    float swapchain_aspect = static_cast<float>(swapchain_width) / swapchain_height;

    u32 dst_width = swapchain_width;
    u32 dst_height = swapchain_height;

    if (frame_aspect > swapchain_aspect) {
        dst_height = static_cast<s32>(swapchain_width / frame_aspect);
    } else {
        dst_width = static_cast<s32>(swapchain_height * frame_aspect);
    }

    const s32 offset_x = (swapchain_width - dst_width) / 2;
    const s32 offset_y = (swapchain_height - dst_height) / 2;

    return vk::Rect2D{{offset_x, offset_y}, {dst_width, dst_height}};
}

[[nodiscard]] vk::ImageBlit MakeImageBlitFit(s32 frame_width, s32 frame_height, s32 swapchain_width,
                                             s32 swapchain_height) {
    const auto& dst_rect = FitImage(frame_width, frame_height, swapchain_width, swapchain_height);

    return MakeImageBlit(frame_width, frame_height, dst_rect.extent.width, dst_rect.extent.height,
                         dst_rect.offset.x, dst_rect.offset.y);
}

enum class ScreenshotKind : u8 {
    GameOnly,
    WithOverlays,
};

struct ScreenshotReadback {
    ScreenshotKind kind{};
    std::vector<std::filesystem::path> paths{};
    VideoCore::Buffer buffer;
    u32 width{};
    u32 height{};
    vk::Format format{};
    bool hdr_encoded{};

    ScreenshotReadback(const Instance& instance, Scheduler& scheduler, ScreenshotKind kind_,
                       std::vector<std::filesystem::path> paths_, const u32 width_,
                       const u32 height_, const vk::Format format_, const bool hdr_encoded_)
        : kind{kind_}, paths{std::move(paths_)},
          buffer{instance,
                 scheduler,
                 VideoCore::MemoryUsage::Download,
                 0,
                 vk::BufferUsageFlagBits::eTransferDst,
                 static_cast<u64>(width_) * static_cast<u64>(height_) * 4},
          width{width_}, height{height_}, format{format_}, hdr_encoded{hdr_encoded_} {}
};

static std::string SanitizeFilenameComponent(std::string value) {
    for (char& c : value) {
        const unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isalnum(uc) && c != '_' && c != '-') {
            c = '_';
        }
    }
    if (value.empty()) {
        return "UNKNOWN";
    }
    return value;
}

static std::vector<std::filesystem::path> BuildScreenshotPaths(const ScreenshotKind kind,
                                                               const u32 count) {
    static std::atomic<u64> screenshot_sequence{0};
    std::vector<std::filesystem::path> paths{};
    if (count == 0) {
        return paths;
    }

    const auto& screenshots_dir = Common::FS::GetUserPath(Common::FS::PathType::ScreenshotsDir);
    std::filesystem::create_directories(screenshots_dir);

    const auto game_id =
        SanitizeFilenameComponent(std::string(Common::ElfInfo::Instance().GameSerial()));
    const auto now = std::chrono::system_clock::now();
    const auto now_time = std::chrono::system_clock::to_time_t(now);
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count() %
        1000;

    std::tm local_tm{};
#ifdef _WIN32
    localtime_s(&local_tm, &now_time);
#else
    localtime_r(&now_time, &local_tm);
#endif

    std::ostringstream stamp;
    stamp << std::put_time(&local_tm, "%Y%m%d_%H%M%S") << '_' << std::setw(3) << std::setfill('0')
          << ms;

    const char* suffix = kind == ScreenshotKind::GameOnly ? "game" : "hud";
    const auto first_sequence = screenshot_sequence.fetch_add(count, std::memory_order_relaxed);

    paths.reserve(count);
    const auto stamp_str = stamp.str();
    for (u32 i = 0; i < count; ++i) {
        paths.emplace_back(screenshots_dir / fmt::format("{}_{}_{}_{:06}.png", game_id, stamp_str,
                                                         suffix, first_sequence + i));
    }

    return paths;
}

static float PqToNits(const float encoded) {
    // ST.2084 inverse EOTF
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;

    const float v = std::clamp(encoded, 0.0f, 1.0f);
    const float vp = std::pow(v, 1.0f / m2);
    const float num = std::max(vp - c1, 0.0f);
    const float den = std::max(c2 - c3 * vp, 1e-6f);
    return 10000.0f * std::pow(num / den, 1.0f / m1);
}

static float ToneMapToSdrLinear(const float nits) {
    // Map absolute HDR luminance into SDR [0,1], preserving 100-nit white.
    constexpr float sdr_white_nits = 100.0f;
    const float x = std::max(nits, 0.0f) / sdr_white_nits;
    const float mapped = (2.0f * x) / (1.0f + x);
    return std::clamp(mapped, 0.0f, 1.0f);
}

static float LinearToSrgb(const float linear) {
    const float x = std::clamp(linear, 0.0f, 1.0f);
    if (x <= 0.0031308f) {
        return 12.92f * x;
    }
    return 1.055f * std::pow(x, 1.0f / 2.4f) - 0.055f;
}

static const std::array<float, 1024>& GetPqDecodeNitsLut() {
    static const std::array<float, 1024> lut = [] {
        std::array<float, 1024> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = PqToNits(static_cast<float>(i) / 1023.0f);
        }
        return values;
    }();
    return lut;
}

static const std::array<u8, 1024>& GetUnorm10ToU8Lut() {
    static const std::array<u8, 1024> lut = [] {
        std::array<u8, 1024> values{};
        for (size_t i = 0; i < values.size(); ++i) {
            values[i] = static_cast<u8>((i * 255u + 511u) / 1023u);
        }
        return values;
    }();
    return lut;
}

static void CopyImageToReadback(const vk::CommandBuffer& cmdbuf, const vk::Image image,
                                const vk::ImageLayout layout, ScreenshotReadback& readback, u32 layer = 0) {
    const vk::BufferImageCopy copy_region = {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource =
            {
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .mipLevel = 0,
                .baseArrayLayer = layer,
                .layerCount = 1,
            },
        .imageOffset = {0, 0, 0},
        .imageExtent = {readback.width, readback.height, 1},
    };
    cmdbuf.copyImageToBuffer(image, layout, readback.buffer.Handle(), copy_region);
}

static bool ConvertReadbackToRgba8(const ScreenshotReadback& readback, std::vector<u8>& out_rgba) {
    const u64 pixel_count = static_cast<u64>(readback.width) * static_cast<u64>(readback.height);
    const u64 byte_size = pixel_count * 4;
    if (readback.buffer.mapped_data.size() < byte_size) {
        LOG_ERROR(Render_Vulkan, "Screenshot readback buffer size mismatch (have {}, need {})",
                  readback.buffer.mapped_data.size(), byte_size);
        return false;
    }

    const auto src =
        std::span<const u8>{readback.buffer.mapped_data.data(), static_cast<size_t>(byte_size)};
    out_rgba.resize(static_cast<size_t>(byte_size));

    switch (readback.format) {
    case vk::Format::eR8G8B8A8Unorm:
    case vk::Format::eR8G8B8A8Srgb:
        std::memcpy(out_rgba.data(), src.data(), out_rgba.size());
        for (u64 i = 0; i < pixel_count; ++i) {
            out_rgba[static_cast<size_t>(i) * 4 + 3] = 255;
        }
        return true;
    case vk::Format::eB8G8R8A8Unorm:
    case vk::Format::eB8G8R8A8Srgb:
        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            out_rgba[o + 0] = src[o + 2];
            out_rgba[o + 1] = src[o + 1];
            out_rgba[o + 2] = src[o + 0];
            out_rgba[o + 3] = 255;
        }
        return true;
    case vk::Format::eA2R10G10B10UnormPack32: {
        const auto& pq_decode_lut = GetPqDecodeNitsLut();
        const auto& unorm10_to_u8 = GetUnorm10ToU8Lut();

        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            const u32 packed = static_cast<u32>(src[o + 0]) | (static_cast<u32>(src[o + 1]) << 8) |
                               (static_cast<u32>(src[o + 2]) << 16) |
                               (static_cast<u32>(src[o + 3]) << 24);
            const u32 b = (packed >> 0) & 0x3FF;
            const u32 g = (packed >> 10) & 0x3FF;
            const u32 r = (packed >> 20) & 0x3FF;

            if (readback.hdr_encoded) {
                // Rec.2020 + PQ. Convert to SDR Rec.709 for PNG output.
                const float r2020 = pq_decode_lut[r];
                const float g2020 = pq_decode_lut[g];
                const float b2020 = pq_decode_lut[b];

                const float r709_nits = 1.6605f * r2020 - 0.5876f * g2020 - 0.0728f * b2020;
                const float g709_nits = -0.1246f * r2020 + 1.1329f * g2020 - 0.0083f * b2020;
                const float b709_nits = -0.0182f * r2020 - 0.1006f * g2020 + 1.1187f * b2020;

                const float r_srgb = LinearToSrgb(ToneMapToSdrLinear(r709_nits));
                const float g_srgb = LinearToSrgb(ToneMapToSdrLinear(g709_nits));
                const float b_srgb = LinearToSrgb(ToneMapToSdrLinear(b709_nits));

                out_rgba[o + 0] = static_cast<u8>(std::clamp(r_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 1] = static_cast<u8>(std::clamp(g_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 2] = static_cast<u8>(std::clamp(b_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
            } else {
                out_rgba[o + 0] = unorm10_to_u8[r];
                out_rgba[o + 1] = unorm10_to_u8[g];
                out_rgba[o + 2] = unorm10_to_u8[b];
            }
            out_rgba[o + 3] = 255;
        }
        return true;
    }
    case vk::Format::eA2B10G10R10UnormPack32: {
        const auto& pq_decode_lut = GetPqDecodeNitsLut();
        const auto& unorm10_to_u8 = GetUnorm10ToU8Lut();

        for (u64 i = 0; i < pixel_count; ++i) {
            const size_t o = static_cast<size_t>(i) * 4;
            const u32 packed = static_cast<u32>(src[o + 0]) | (static_cast<u32>(src[o + 1]) << 8) |
                               (static_cast<u32>(src[o + 2]) << 16) |
                               (static_cast<u32>(src[o + 3]) << 24);
            const u32 r = (packed >> 0) & 0x3FF;
            const u32 g = (packed >> 10) & 0x3FF;
            const u32 b = (packed >> 20) & 0x3FF;

            if (readback.hdr_encoded) {
                // HDR swapchain path is Rec.2020 + PQ. Convert to SDR Rec.709 for PNG output.
                const float r2020 = pq_decode_lut[r];
                const float g2020 = pq_decode_lut[g];
                const float b2020 = pq_decode_lut[b];

                const float r709_nits = 1.6605f * r2020 - 0.5876f * g2020 - 0.0728f * b2020;
                const float g709_nits = -0.1246f * r2020 + 1.1329f * g2020 - 0.0083f * b2020;
                const float b709_nits = -0.0182f * r2020 - 0.1006f * g2020 + 1.1187f * b2020;

                const float r_srgb = LinearToSrgb(ToneMapToSdrLinear(r709_nits));
                const float g_srgb = LinearToSrgb(ToneMapToSdrLinear(g709_nits));
                const float b_srgb = LinearToSrgb(ToneMapToSdrLinear(b709_nits));

                out_rgba[o + 0] = static_cast<u8>(std::clamp(r_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 1] = static_cast<u8>(std::clamp(g_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
                out_rgba[o + 2] = static_cast<u8>(std::clamp(b_srgb, 0.0f, 1.0f) * 255.0f + 0.5f);
            } else {
                out_rgba[o + 0] = unorm10_to_u8[r];
                out_rgba[o + 1] = unorm10_to_u8[g];
                out_rgba[o + 2] = unorm10_to_u8[b];
            }
            out_rgba[o + 3] = 255;
        }
        return true;
    }
    default:
        LOG_WARNING(Render_Vulkan, "Unsupported screenshot format: {}",
                    vk::to_string(readback.format));
        return false;
    }
}

static bool WritePng(const std::filesystem::path& path, const std::span<const u8> rgba,
                     const u32 width, const u32 height) {
    Common::FS::IOFile file(path, Common::FS::FileAccessMode::Create);
    if (!file.IsOpen()) {
        return false;
    }

    auto callback = [](void* context, void* data, int size) {
        const auto* f = static_cast<Common::FS::IOFile*>(context);
        f->WriteRaw<u8>(data, size);
    };
    return stbi_write_png_to_func(callback, &file, width, height, 4, rgba.data(), 0);
}

static void SavePendingScreenshots(const std::vector<ScreenshotReadback>& readbacks) {
    for (const auto& readback : readbacks) {
        if (readback.paths.empty()) {
            continue;
        }

        std::vector<u8> rgba;
        if (!ConvertReadbackToRgba8(readback, rgba)) {
            continue;
        }

        const auto& primary_path = readback.paths.front();
        if (!WritePng(primary_path, rgba, readback.width, readback.height)) {
            LOG_ERROR(Render_Vulkan, "Failed saving screenshot to {}", primary_path.string());
            continue;
        }

        LOG_INFO(Render_Vulkan, "Saved screenshot: {}", primary_path.string());

        std::ifstream file(primary_path, std::ios::binary);
        std::vector<u8> imgdata;
        if (file) {
            imgdata = std::vector<u8>(std::istreambuf_iterator<char>(file),
                                      std::istreambuf_iterator<char>());
        }
        shadNotifications::QueueNotification("Saved screenshot:\n" + primary_path.string(), 3.0f,
                                             shadNotifications::position::BottomRight, imgdata);

        for (size_t i = 1; i < readback.paths.size(); ++i) {
            const auto& path = readback.paths[i];
            std::error_code ec{};
            std::filesystem::copy_file(primary_path, path, std::filesystem::copy_options::none, ec);
            if (ec) {
                // Fallback for platforms/filesystems where copy_file can fail for transient
                // reasons.
                if (!WritePng(path, rgba, readback.width, readback.height)) {
                    LOG_ERROR(Render_Vulkan, "Failed saving screenshot to {}", path.string());
                    continue;
                }
            }

            LOG_INFO(Render_Vulkan, "Saved screenshot: {}", path.string());
            std::ifstream file(path, std::ios::binary);
            std::vector<u8> imgdata;
            if (file) {
                imgdata = std::vector<u8>(std::istreambuf_iterator<char>(file),
                                          std::istreambuf_iterator<char>());
            }
            shadNotifications::QueueNotification("Saved screenshot:\n" + path.string(), 3.0f,
                                                 shadNotifications::position::BottomRight, imgdata);
        }
    }
}

Presenter::Presenter(Frontend::WindowSDL& window_, AmdGpu::Liverpool* liverpool_)
    : window{window_}, liverpool{liverpool_},
      instance{window, EmulatorSettings.GetGpuId(), EmulatorSettings.IsVkValidationEnabled(),
               EmulatorSettings.IsVkCrashDiagnosticEnabled()},
      draw_scheduler{instance, true}, present_scheduler{instance}, flip_scheduler{instance},
      swapchain{instance, window},
      rasterizer{std::make_unique<Rasterizer>(instance, draw_scheduler, liverpool)},
      texture_cache{rasterizer->GetTextureCache()} {
    const u32 num_images = swapchain.GetImageCount();
    const vk::Device device = instance.GetDevice();

    // Create presentation frames.
    present_frames.resize(num_images);
    for (u32 i = 0; i < num_images; i++) {
        Frame& frame = present_frames[i];
        frame.id = i;
        auto fence = Check<"create present done fence">(
            device.createFence({.flags = vk::FenceCreateFlagBits::eSignaled}));
        frame.present_done = fence;
        free_queue.push(&frame);
    }

    fsr_settings.enable = EmulatorSettings.IsFsrEnabled();
    fsr_settings.use_rcas = EmulatorSettings.IsRcasEnabled();
    fsr_settings.rcas_attenuation =
        static_cast<float>(EmulatorSettings.GetRcasAttenuation() / 1000.f);

    fsr_pass.Create(device, instance.GetAllocator(), num_images);
    pp_pass.Create(device, swapchain.GetSurfaceFormat().format);

    if (OpenXRPresenter::IsRequested()) {
        if (OpenXRPresenter::IsRuntimeAvailable()) {
            openxr_presenter =
                std::make_unique<OpenXRPresenter>(instance, swapchain.GetSurfaceFormat().format);
            if (!openxr_presenter->IsReady()) {
                LOG_ERROR(Render_Vulkan, "OpenXR presenter could not start; VR is off");
                openxr_presenter.reset();
            } else {
                // DIBR stereo (stereo_mode 2): captured in the draw stream, warped at the flip.
                vr_warp = std::make_unique<VrStereoWarp>(instance, draw_scheduler);
                if (std::getenv("VR_DIBR_TIMING") != nullptr) {
                    vr_warp->EnableTiming(instance); // GPU times in the `VR dibr gpu:` line
                }
                vr_eye_images.resize(num_images);
                if (openxr_presenter->LateWarpEnabled()) {
                    // The late warp re-sees each frame's capture from the head at every XR frame's
                    // display time: the captures rotate through slots it keeps while it shows them.
                    vr_warp->EnableLateRing(openxr_presenter->LateBusyMask(),
                                            openxr_presenter->LateSlotsWanted());
                }
                rasterizer->SetVrStereoWarp(vr_warp.get());
                LOG_INFO(Render_Vulkan, "VR dibr: warp pipelines created");
                // The game's UI on its own layer: shadowed in the draw stream, resolved at the
                // flip.
                vr_ui_layer = std::make_unique<VrUiLayer>(instance, draw_scheduler);
                if (std::getenv("VR_UI_TIMING") != nullptr) {
                    vr_ui_layer->EnableTiming(instance); // GPU times in the `VR ui:` line
                }
                rasterizer->SetVrUiLayer(vr_ui_layer.get());
            }
        } else {
            LOG_ERROR(Render_Vulkan, "OpenXR runtime unavailable; VR is off");
        }
    }

    ImGui::Layer::AddLayer(Common::Singleton<Core::Devtools::Layer>::Instance());
    ImGui::Friends::Register();
    ImGui::ShadNetNotify::Register();
    ImGui::InvitationPrompt::Register();
}

Presenter::~Presenter() {
    ImGui::InvitationPrompt::Unregister();
    ImGui::ShadNetNotify::Unregister();
    ImGui::Friends::Unregister();
    ImGui::Layer::RemoveLayer(Common::Singleton<Core::Devtools::Layer>::Instance());

    if (openxr_presenter) {
        openxr_presenter->ShutdownLateWarp(); // the frame thread stops reading the captures
    }
    draw_scheduler.Finish();
    present_scheduler.Finish();
    flip_scheduler.Finish();
    rasterizer->SetVrStereoWarp(nullptr);
    rasterizer->SetVrUiLayer(nullptr);
    Check(draw_scheduler.CommandBuffer().reset());
    Check(present_scheduler.CommandBuffer().reset());
    Check(flip_scheduler.CommandBuffer().reset());

    const vk::Device device = instance.GetDevice();
    for (auto& frame : present_frames) {
        vmaDestroyImage(instance.GetAllocator(), frame.image, frame.allocation);
        device.destroyImageView(frame.image_view);
        device.destroyFence(frame.present_done);
    }
}

bool Presenter::IsVideoOutSurface(const AmdGpu::ColorBuffer& color_buffer) const {
    return std::ranges::find(vo_buffers_addr, color_buffer.Address()) != vo_buffers_addr.cend();
}

void Presenter::RecreateFrame(Frame* frame, u32 width, u32 height) {
    const vk::Device device = instance.GetDevice();
    if (frame->imgui_texture) {
        ImGui::Vulkan::RemoveTexture(frame->imgui_texture);
    }
    if (frame->image_view) {
        device.destroyImageView(frame->image_view);
    }
    if (frame->image) {
        vmaDestroyImage(instance.GetAllocator(), frame->image, frame->allocation);
    }

    const vk::Format format = swapchain.GetSurfaceFormat().format;
    const vk::ImageCreateInfo image_info = {
        .flags = vk::ImageCreateFlagBits::eMutableFormat,
        .imageType = vk::ImageType::e2D,
        .format = format,
        .extent = {width, height, 1},
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = vk::SampleCountFlagBits::e1,
        .usage = vk::ImageUsageFlagBits::eColorAttachment | vk::ImageUsageFlagBits::eTransferDst |
                 vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eSampled,
    };

    const VmaAllocationCreateInfo alloc_info = {
        .flags = VMA_ALLOCATION_CREATE_WITHIN_BUDGET_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE,
        .requiredFlags = 0,
        .preferredFlags = 0,
        .pool = VK_NULL_HANDLE,
        .pUserData = nullptr,
    };

    VkImage unsafe_image{};
    VkImageCreateInfo unsafe_image_info = static_cast<VkImageCreateInfo>(image_info);

    VkResult result = vmaCreateImage(instance.GetAllocator(), &unsafe_image_info, &alloc_info,
                                     &unsafe_image, &frame->allocation, nullptr);
    if (result != VK_SUCCESS) [[unlikely]] {
        LOG_CRITICAL(Render_Vulkan, "Failed allocating texture with error {}",
                     vk::to_string(vk::Result{result}));
        UNREACHABLE();
    }
    frame->image = vk::Image{unsafe_image};
    SetObjectName(device, frame->image, "Frame image #{}", frame->id);

    const vk::ImageViewCreateInfo view_info = {
        .image = frame->image,
        .viewType = vk::ImageViewType::e2D,
        .format = format,
        .subresourceRange{
            .aspectMask = vk::ImageAspectFlagBits::eColor,
            .baseMipLevel = 0,
            .levelCount = 1,
            .baseArrayLayer = 0,
            .layerCount = 1,
        },
    };
    auto view = Check<"create frame image view">(device.createImageView(view_info));
    frame->image_view = view;
    frame->width = width;
    frame->height = height;

    frame->imgui_texture = ImGui::Vulkan::AddTexture(view, vk::ImageLayout::eShaderReadOnlyOptimal);
    frame->is_hdr = swapchain.GetHDR();
}

Frame* Presenter::PrepareLastFrame() {
    if (last_submit_frame == nullptr) {
        return nullptr;
    }

    Frame* frame = last_submit_frame;

    while (true) {
        vk::Result result = instance.GetDevice().waitForFences(frame->present_done, false,
                                                               std::numeric_limits<u64>::max());
        if (result == vk::Result::eSuccess) {
            break;
        }
        if (result == vk::Result::eTimeout) {
            continue;
        }
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
    }

    auto& scheduler = flip_scheduler;
    scheduler.EndRendering();
    const auto cmdbuf = scheduler.CommandBuffer();

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier =
        vk::ImageMemoryBarrier2{.srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                                .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                                .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
                                .oldLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                                .newLayout = vk::ImageLayout::eGeneral,
                                .image = frame->image,
                                .subresourceRange{frame_subresources}};

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

static vk::Format GetFrameViewFormat(const Libraries::VideoOut::PixelFormat format) {
    switch (format) {
    case Libraries::VideoOut::PixelFormat::A8B8G8R8Srgb:
        return vk::Format::eR8G8B8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A8R8G8B8Srgb:
        return vk::Format::eB8G8R8A8Srgb;
    case Libraries::VideoOut::PixelFormat::A2R10G10B10:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Srgb:
    case Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq:
        return vk::Format::eA2R10G10B10UnormPack32;
    default:
        break;
    }
    UNREACHABLE_MSG("Unknown format={}", static_cast<u32>(format));
    return {};
}

#include "../../../scratchpad/dual-eye-perf/native_present.inl"

Frame* Presenter::PrepareFrame(const Libraries::VideoOut::BufferAttributeGroup& attribute,
                               VAddr cpu_address, const VR::FrameTag& vr_tag) {
    rasterizer->DrainDrawPipe();
    auto desc = VideoCore::TextureCache::ImageDesc{attribute, cpu_address};
    const auto image_id = texture_cache.FindImage(desc);
    texture_cache.UpdateImage(image_id);

    Frame* frame = GetRenderFrame();
    frame->vr_tag = vr_tag;
    frame->vr_stereo = false;
    frame->vr_published = false;
    frame->vr_ui = {};
    frame->vr_scene_image = vk::Image{};
    frame->vr_desktop_eye = false;

    const auto frame_subresources = vk::ImageSubresourceRange{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = VK_REMAINING_ARRAY_LAYERS,
    };

    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange{frame_subresources},
    };

    draw_scheduler.EndRendering();
    const auto cmdbuf = draw_scheduler.CommandBuffer();
    GpuPassProfiler::MarkAt(draw_scheduler, 0xF11F'0000'0001ull, GpuPassProfiler::Group::VrHost,
                            "flip: frame setup (barriers, screenshots)");
    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    VideoCore::ImageViewInfo view_info{};
    view_info.format = GetFrameViewFormat(attribute.attrib.pixel_format);
    // Exclude alpha from output frame to avoid blending with UI.
    view_info.mapping.a = vk::ComponentSwizzle::eOne;

    auto& image = texture_cache.GetImage(image_id);
    auto image_view = *image.FindView(view_info).image_view;
    const vk::Extent2D image_size = {image.info.size.width, image.info.size.height};
    expected_ratio = static_cast<float>(image_size.width) / static_cast<float>(image_size.height);
    VR::SetMainAspect(expected_ratio);
    game_extent = image_size;

    const u32 capture_game_only_count = VideoCore::ConsumeGameOnlyScreenshotRequests();
    std::vector<ScreenshotReadback> pending_screenshots;
    if (capture_game_only_count > 0) {
        pending_screenshots.reserve(1);
        const bool hdr_encoded =
            attribute.attrib.pixel_format == Libraries::VideoOut::PixelFormat::A2R10G10B10Bt2020Pq;
        pending_screenshots.emplace_back(
            instance, draw_scheduler, ScreenshotKind::GameOnly,
            BuildScreenshotPaths(ScreenshotKind::GameOnly, capture_game_only_count),
            image_size.width, image_size.height, view_info.format, hdr_encoded);
        auto& readback = pending_screenshots.back();

        // Capture the guest output before any host-side scaling (FSR/PP) is applied.
        image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {},
                      cmdbuf);
        CopyImageToReadback(cmdbuf, image.GetImage(), vk::ImageLayout::eTransferSrcOptimal,
                            readback);
    }

    if (NativeStereo::Enabled()) {
        const char* dir = std::getenv("VR_NATIVE_CAPTURE_DIR");
        if (!dir)
            dir = std::getenv("VR_NATIVE_AUDIT_DIR");
        if (dir && ScratchCapture::Consume(std::filesystem::path(dir)/"native_dump")) {
            // Test-only labelled native layers remain capturable when tracking loss
            // makes the compositor show a mono quad instead of a projection layer.
            std::string dump_label;
            const auto label_path = std::filesystem::path(dir) / "native_dump_label";
            {
                std::ifstream label_file(label_path);
                label_file >> dump_label;
            }
            ScratchCapture::Consume(label_path);
            if (dump_label != "tracking_before" && dump_label != "tracking_active" &&
                dump_label != "tracking_after")
                dump_label.clear();
            if (!dump_label.empty()) {
                const auto& record = vr_tag.record;
                LOG_INFO(Render_Vulkan,
                         "VR tracking native: label={} gameplay_s={:.3f} driven={} vfov={:.5f} "
                         "eye_left={:.5f}/{:.5f}",
                         dump_label, VR::GameplaySeconds(), record.driven, record.vfov,
                         record.eye_fov[0].angle_left, record.eye_fov[1].angle_left);
            }
            if (dump_label.empty())
                VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::WithOverlays);
            ASSERT_MSG(image.native_stereo, "Native final output is not layered");
            image.Transit(vk::ImageLayout::eTransferSrcOptimal, vk::AccessFlagBits2::eTransferRead, {}, cmdbuf);
            for (u32 eye=0; eye<2; ++eye) {
                std::vector<std::filesystem::path> paths{
                    std::filesystem::path(dir) /
                    (dump_label.empty() ? fmt::format("native_eye_{}.png", eye)
                                        : fmt::format("{}_native_eye_{}.png", dump_label, eye))};
                pending_screenshots.emplace_back(instance, draw_scheduler, ScreenshotKind::GameOnly,
                    std::move(paths), image_size.width, image_size.height, view_info.format, false);
                CopyImageToReadback(cmdbuf, image.GetImage(), vk::ImageLayout::eTransferSrcOptimal, pending_screenshots.back(), eye);
            }
            LOG_INFO(Render_Vulkan, "VR native stereo: captured both final colour layers {}x{}", image_size.width, image_size.height);
        }
    }

    // Continue with host-side passes that draw the displayed (scaled) frame.
    image.Transit(vk::ImageLayout::eShaderReadOnlyOptimal, vk::AccessFlagBits2::eShaderRead, {},
                  cmdbuf);

    GpuPassProfiler::MarkAt(draw_scheduler, 0xF11F'0000'0002ull, GpuPassProfiler::Group::VrHost,
                            "flip: FSR 1 / scaling pass (desktop)");
    image_view = fsr_pass.Render(cmdbuf, image_view, image_size, {frame->width, frame->height},
                                 fsr_settings, frame->is_hdr);
    // vr.ini desktop_mirror_view right / left: the window shows that eye of a frame that goes to
    // the headset, so its post-process pass (it only feeds the window then) waits until that is
    // known.
    const auto mirror_view = VR::DesktopMirror::ViewFromNumber(VR::Tune::Get(
        "mirror.view", static_cast<float>(VR::GetSettings().desktop_mirror_view)));
    const u32 mirror_index = VR::DesktopMirror::EyeOf(mirror_view);
    const bool mirror_eye = vr_warp && openxr_presenter && !frame->is_hdr &&
                            !VR::LagTestEnabled() && mirror_view != VR::DesktopMirror::View::Game;
    auto frame_settings = pp_settings;
    if (vr_tag.immersive && !frame->is_hdr) {
        frame_settings.color_adjust = VR::Color::Get().Push();
    }
    if (!mirror_eye) {
        GpuPassProfiler::MarkAt(draw_scheduler, 0xF11F'0000'0003ull,
                                GpuPassProfiler::Group::VrHost,
                                "flip: post-process pass (desktop)");
        pp_pass.Render(cmdbuf, image_view, image_size, *frame, frame_settings);
    }
    if (vr_warp) {
        // The UI layer first (its veil goes into the scene the eyes are made from), then DIBR:
        // both eyes of this frame from its capture; the captures never outlive the flip.
        GpuPassProfiler::MarkAt(draw_scheduler, 0xF11F'0000'0004ull,
                                GpuPassProfiler::Group::VrHost, "VR UI layer resolve");
        RecordVrUi(cmdbuf, *frame, image_size);
        if (NativeStereo::Enabled()) {
            RecordNativeStereo(cmdbuf, *frame, image, image_size, view_info.format);
        } else {
            GpuPassProfiler::MarkAt(draw_scheduler, 0xF11F'0000'0005ull,
                                    GpuPassProfiler::Group::VrHost, "VR DIBR stereo warp");
            RecordVrStereo(cmdbuf, *frame, image_size);
            GpuPassProfiler::MarkAt(draw_scheduler, 0xF11F'0000'0006ull,
                                    GpuPassProfiler::Group::VrHost, "VR UI eyes");
            RecordVrUiEyes(cmdbuf, *frame, image_size);
        }
        if (mirror_eye) {
            if (frame->vr_published && frame->vr_stereo && frame->vr_eye_image[mirror_index] &&
                frame->vr_eye_extent[mirror_index].width != 0 &&
                frame->vr_eye_extent[mirror_index].height != 0) {
                // Its image is never drawn: from the attachment layout of the barrier above to
                // the one Present takes it in (its contents undefined; Present shows the eye).
                frame->vr_desktop_eye = true;
                frame->vr_desktop_eye_index = static_cast<u8>(mirror_index);
                const vk::ImageMemoryBarrier2 to_general{
                    .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                    .srcAccessMask = vk::AccessFlagBits2::eNone,
                    .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
                    .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
                    .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
                    .newLayout = vk::ImageLayout::eGeneral,
                    .image = frame->image,
                    .subresourceRange{frame_subresources},
                };
                cmdbuf.pipelineBarrier2(vk::DependencyInfo{
                    .imageMemoryBarrierCount = 1,
                    .pImageMemoryBarriers = &to_general,
                });
            } else {
                // Not a published pair (mono, AER, a held or refused frame): post-processed as
                // always. The VR passes may have moved the game's image (the HUD capture).
                texture_cache.GetImage(image_id).Transit(vk::ImageLayout::eShaderReadOnlyOptimal,
                                                         vk::AccessFlagBits2::eShaderRead, {},
                                                         cmdbuf);
                GpuPassProfiler::MarkAt(draw_scheduler, 0xF11F'0000'0003ull,
                                        GpuPassProfiler::Group::VrHost,
                                        "flip: post-process pass (desktop)");
                pp_pass.Render(cmdbuf, image_view, image_size, *frame, frame_settings);
            }
        }
        vr_warp->BeginFrame(0);
        vr_ui_layer->EndFrame();
        rasterizer->VrEndFrame();
    }

    DebugState.game_resolution = {image_size.width, image_size.height};
    DebugState.output_resolution = {frame->width, frame->height};

    std::shared_ptr<std::vector<ScreenshotReadback>> deferred_screenshots{};
    if (!pending_screenshots.empty()) {
        deferred_screenshots =
            std::make_shared<std::vector<ScreenshotReadback>>(std::move(pending_screenshots));
        draw_scheduler.DeferPriorityOperation(
            [deferred_screenshots]() { SavePendingScreenshots(*deferred_screenshots); });
    }

    // Flush frame creation commands.
    frame->ready_semaphore = draw_scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = draw_scheduler.CurrentTick();
    GpuPassProfiler::EndFrameAt(draw_scheduler); // FAST_GPU_PASS_PROFILE: the frame's last timestamp
    SubmitInfo info{};
    draw_scheduler.Flush(info);
    GpuFrameTimerMark(draw_scheduler, instance); // VR_GPU_TIMING only: the frame's GPU time
    draw_scheduler.FlushRecorder("presenter_handoff");
    if (frame->vr_published) {
        // DIBR: the eyes copied in this submission go to the headset once the GPU is done.
        openxr_presenter->PublishStereo(draw_scheduler.GetMasterSemaphore(), frame->ready_tick);
    }
    return frame;
}

Frame* Presenter::PrepareBlankFrame(bool present_thread) {
    rasterizer->DrainDrawPipe();
    // Request a free presentation frame.
    Frame* frame = GetRenderFrame();
    frame->vr_stereo = false;
    frame->vr_published = false;
    frame->vr_ui = {};
    frame->vr_scene_image = vk::Image{};
    frame->vr_desktop_eye = false;
    if (vr_warp && !present_thread) {
        // A blank flip ends the frame being drawn too (GPU thread).
        vr_warp->BeginFrame(0);
        vr_ui_layer->EndFrame();
        rasterizer->VrEndFrame();
    }

    auto& scheduler = present_thread ? present_scheduler : draw_scheduler;
    scheduler.EndRendering();

    const auto cmdbuf = scheduler.CommandBuffer();

    constexpr vk::ImageSubresourceRange simple_subresource = {
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .levelCount = 1,
        .layerCount = 1,
    };
    const auto pre_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentRead,
        .dstStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .dstAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .oldLayout = vk::ImageLayout::eUndefined,
        .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const auto post_barrier = vk::ImageMemoryBarrier2{
        .srcStageMask = vk::PipelineStageFlagBits2::eColorAttachmentOutput,
        .srcAccessMask = vk::AccessFlagBits2::eColorAttachmentWrite,
        .dstStageMask = vk::PipelineStageFlagBits2::eFragmentShader,
        .dstAccessMask = vk::AccessFlagBits2::eShaderRead,
        .oldLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .newLayout = vk::ImageLayout::eGeneral,
        .image = frame->image,
        .subresourceRange = simple_subresource,
    };

    const vk::RenderingAttachmentInfo attachment = {
        .imageView = frame->image_view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eClear,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    const vk::RenderingInfo rendering_info = {
        .renderArea =
            {
                .extent = {frame->width, frame->height},
            },
        .layerCount = 1,
        .colorAttachmentCount = 1u,
        .pColorAttachments = &attachment,
    };

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &pre_barrier,
    });

    cmdbuf.beginRendering(rendering_info);
    cmdbuf.endRendering();

    cmdbuf.pipelineBarrier2(vk::DependencyInfo{
        .imageMemoryBarrierCount = 1,
        .pImageMemoryBarriers = &post_barrier,
    });

    // Flush frame creation commands.
    frame->ready_semaphore = scheduler.GetMasterSemaphore()->Handle();
    frame->ready_tick = scheduler.CurrentTick();
    GpuPassProfiler::EndFrameAt(scheduler); // FAST_GPU_PASS_PROFILE: draw scheduler only
    SubmitInfo info{};
    scheduler.Flush(info);
    return frame;
}

void Presenter::Present(Frame* frame, bool is_reusing_frame) {
    // Free the frame for reuse
    const auto free_frame = [&] {
        if (!is_reusing_frame) {
            last_submit_frame = frame;
            std::scoped_lock fl{free_mutex};
            free_queue.push(frame);
            free_cv.notify_one();
        }
    };

    // Swapchain::Recreate waits for the device to be idle: with the present queue's lock
    // (Instance::GetPresentQueueMutex; the graphics queue's is not taken here, as before) and the
    // OpenXR runtime's own queue's (Instance::GetXrQueueMutex: its xrEndFrame submits there
    // without the graphics queue's lock; never taken by anyone else without the runtime's own
    // queue). No other lock is held here.
    const auto recreate = [&] {
        std::scoped_lock present_lock{instance.GetXrQueueMutex(), instance.GetPresentQueueMutex()};
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    };

    // vr.ini desktop_mirror (core/vr/desktop_mirror.h): of the frames that went to the headset
    // only every Nth is presented on the desktop. The others end here, freed exactly as after a
    // present (present_done signalled once the frame's GPU work is done, its image left in
    // eShaderReadOnlyOptimal as below: PrepareLastFrame expects that), and the window keeps its
    // last image. A pending screenshot is never skipped. Neither the swapchain nor the present
    // queue is touched.
    if (openxr_presenter && frame->vr_published && !is_reusing_frame) {
        const s32 every = static_cast<s32>(std::lround(VR::Tune::Get(
            "mirror.every", static_cast<float>(VR::GetSettings().desktop_mirror))));
        ++mirror_published;
        if (mirror_published % 600 == 0) {
            LOG_INFO(Render_Vulkan,
                     "VR desktop mirror: {} frames to the headset, {} not presented on the desktop "
                     "(desktop_mirror {}), {} shown as an eye",
                     mirror_published, mirror_skipped, every, mirror_eye_shown);
        }
        if (!VR::DesktopMirror::Presents(every, mirror_published) &&
            VideoCore::PeekWithOverlaysScreenshotRequests() == 0) {
            const auto reset = instance.GetDevice().resetFences(frame->present_done);
            ASSERT_MSG(reset == vk::Result::eSuccess,
                       "Unexpected error resetting present done fence: {}", vk::to_string(reset));
            const auto skip_cmdbuf = present_scheduler.CommandBuffer();
            const vk::ImageMemoryBarrier to_read{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };
            skip_cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                        vk::PipelineStageFlagBits::eColorAttachmentOutput,
                                        vk::DependencyFlagBits::eByRegion, {}, {}, to_read);
            SubmitInfo skip_info{};
            skip_info.AddWait(frame->ready_semaphore, frame->ready_tick);
            skip_info.AddSignal(frame->present_done);
            present_scheduler.Flush(skip_info);
            ++mirror_skipped;
            free_frame();
            DebugState.IncFlipFrameNum();
            return;
        }
    }

    // Recreate the swapchain if the window was resized.
    if (window.GetWidth() != swapchain.GetWidth() || window.GetHeight() != swapchain.GetHeight()) {
        recreate();
    }

    if (!swapchain.AcquireNextImage()) {
        recreate();
        if (!swapchain.AcquireNextImage()) {
            // User resizes the window too fast and GPU can't keep up. Skip this frame.
            LOG_WARNING(Render_Vulkan, "Skipping frame!");
            free_frame();
            return;
        }
    }

    // Reset fence for queue submission. Do it here instead of GetRenderFrame() because we may
    // skip frame because of slow swapchain recreation. If a frame skip occurs, we skip signal
    // the frame's present fence and future GetRenderFrame() call will hang waiting for this frame.
    const auto reset_result = instance.GetDevice().resetFences(frame->present_done);
    ASSERT_MSG(reset_result == vk::Result::eSuccess,
               "Unexpected error resetting present done fence: {}", vk::to_string(reset_result));

    ImGuiID dockId = ImGui::Core::NewFrame(is_reusing_frame);

    const vk::Image swapchain_image = swapchain.Image();
    const vk::ImageView swapchain_image_view = swapchain.ImageView();

    auto& scheduler = present_scheduler;
    const auto cmdbuf = scheduler.CommandBuffer();
    // A held frame (VR::FrameTag::hold, a menu's open or close) is not given to the headset; a
    // DIBR frame handed over in the draw stream (Frame::vr_published) is there already.
    const bool vr_copy = openxr_presenter && !is_reusing_frame && frame->vr_tag.valid &&
                         !frame->vr_tag.hold && !frame->vr_published &&
                         frame->width == game_extent.width && frame->height == game_extent.height;
    const u32 capture_with_overlays_count = VideoCore::ConsumeWithOverlaysScreenshotRequests();
    std::vector<ScreenshotReadback> pending_screenshots;
    if (capture_with_overlays_count > 0) {
        pending_screenshots.reserve(1);
    }

    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.beginDebugUtilsLabelEXT(vk::DebugUtilsLabelEXT{
            .pLabelName = "Present",
        });
    }

    {
        auto* profiler_ctx = instance.GetProfilerContext();
        TracyVkNamedZoneC(profiler_ctx, renderer_gpu_zone, cmdbuf, "Host frame",
                          MarkersPalette::GpuMarkerColor, profiler_ctx != nullptr);

        const vk::Extent2D extent = swapchain.GetExtent();
        const std::array pre_barriers{
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eNone,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .oldLayout = vk::ImageLayout::eUndefined,
                .newLayout = vk::ImageLayout::eColorAttachmentOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
            vk::ImageMemoryBarrier{
                .srcAccessMask = vk::AccessFlagBits::eColorAttachmentWrite,
                .dstAccessMask = vk::AccessFlagBits::eColorAttachmentRead,
                .oldLayout = vk::ImageLayout::eGeneral,
                .newLayout = vk::ImageLayout::eShaderReadOnlyOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = frame->image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            },
        };

        bool swapchain_copied_for_screenshot = false;

        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eColorAttachmentOutput,
                               vk::PipelineStageFlagBits::eColorAttachmentOutput,
                               vk::DependencyFlagBits::eByRegion, {}, {}, pre_barriers);

        // vr.ini desktop_mirror_view right / left: the window is that eye (the frame image holds
        // nothing); ImGui's frame ends without being drawn (the eye covers the whole window).
        // (An HDR window cannot take it: black. Its frame image was not drawn either way.)
        const bool mirror_eye = frame->vr_desktop_eye;
        if (mirror_eye) {
            ImGui::Render();
            RecordMirrorEye(cmdbuf, *frame, swapchain_image, extent);
            ++mirror_eye_shown;
        } else { // Draw the game
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2{0.0f});
            ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.0f, 0.0f, 0.0f, 1.0f));
            ImGui::SetNextWindowDockID(dockId, ImGuiCond_Once);
            if (ImGui::Begin("Display##game_display", nullptr, ImGuiWindowFlags_NoNav)) {
                auto game_texture = frame->imgui_texture;
                auto game_width = frame->width;
                auto game_height = frame->height;

                if (Libraries::SystemService::IsSplashVisible()) { // draw splash
                    if (!splash_img.has_value()) {
                        splash_img.emplace();
                        const auto& splash_data = Common::ElfInfo::Instance().GetSplashData();
                        if (!splash_data.empty()) {
                            splash_img = ImGui::RefCountedTexture::DecodePngTexture(splash_data);
                        }
                    }
                    if (auto& splash_image = this->splash_img.value()) {
                        auto [im_id, width, height] = splash_image.GetTexture();
                        game_texture = im_id;
                        game_width = width;
                        game_height = height;
                    }
                }

                ImVec2 contentArea = ImGui::GetContentRegionAvail();
                SetExpectedGameSize((s32)contentArea.x, (s32)contentArea.y);

                const auto imgRect =
                    FitImage(game_width, game_height, (s32)contentArea.x, (s32)contentArea.y);
                ImVec2 offset{
                    static_cast<float>(imgRect.offset.x),
                    static_cast<float>(imgRect.offset.y),
                };
                ImVec2 size{
                    static_cast<float>(imgRect.extent.width),
                    static_cast<float>(imgRect.extent.height),
                };

                ImGui::SetCursorPos(ImGui::GetCursorStartPos() + offset);
                ImGui::Image(game_texture, size);
                if (VR::LagTestEnabled() && frame->vr_tag.valid) {
                    const auto id = frame->vr_tag.record.id;
                    const std::string text =
                        fmt::format("rec {} expects {}", id, (id & 1) ? "RIGHT" : "LEFT");
                    ImGui::GetForegroundDrawList()->AddText(nullptr, 48.0f, ImVec2{40.0f, 120.0f},
                                                            IM_COL32(255, 255, 0, 255),
                                                            text.c_str());
                }

                if (EmulatorSettings.IsNullGPU()) {
                    Core::Devtools::Layer::DrawNullGpuNotice();
                }
            }
            ImGui::End();
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor();
            ImGui::Core::Render(cmdbuf, swapchain_image_view, swapchain.GetExtent());
        }
        // Where the swapchain image is now: as ImGui drew it, or the eye's copy.
        const vk::ImageLayout drawn_layout = mirror_eye ? vk::ImageLayout::eTransferDstOptimal
                                                        : vk::ImageLayout::eColorAttachmentOptimal;
        const vk::AccessFlags drawn_access = mirror_eye ? vk::AccessFlagBits::eTransferWrite
                                                        : vk::AccessFlagBits::eColorAttachmentWrite;
        const vk::PipelineStageFlags drawn_stage =
            mirror_eye ? vk::PipelineStageFlagBits::eTransfer
                       : vk::PipelineStageFlagBits::eColorAttachmentOutput;

        // A UI-layer frame's eyes are the scene without its UI (the UI goes on its layer): the
        // warped eyes, or the mono eye image made from the scene copy. Without either (the mono
        // eye could not be made, or DIBR eyes that do not fit fall back to the frame's own image)
        // the eyes carry the UI themselves, so the frame's UI layers are left out (never twice).
        bool ui_free_eyes = frame->vr_scene_image;
        if (vr_copy && frame->vr_stereo) {
            // DIBR: both eyes of this one game frame.
            OpenXRPresenter::StereoEyes eyes{};
            for (u32 i = 0; i < 2; ++i) {
                eyes.image[i] = frame->vr_eye_image[i];
                eyes.extent[i] = frame->vr_eye_extent[i];
                eyes.crop[i] = {frame->vr_eye_fov[i], frame->vr_eye_rect[i]};
            }
            eyes.eye_offset_m = frame->vr_eye_offset_m;
            if (openxr_presenter->CopyStereoFrame(
                    cmdbuf, eyes, frame->vr_scene_image ? frame->vr_scene_image : frame->image,
                    {frame->width, frame->height}, frame->vr_tag)) {
                ui_free_eyes = true;
            }
        } else if (vr_copy) {
            openxr_presenter->CopyFrame(
                cmdbuf, frame->vr_scene_image ? frame->vr_scene_image : frame->image,
                {frame->width, frame->height}, frame->vr_tag);
        }
        if (vr_copy) {
            openxr_presenter->CopyUiFrame(
                cmdbuf,
                frame->vr_ui.valid && !ui_free_eyes ? OpenXRPresenter::UiFrame{} : frame->vr_ui,
                frame->vr_tag);
        }

        if (capture_with_overlays_count > 0) {
            pending_screenshots.emplace_back(
                instance, scheduler, ScreenshotKind::WithOverlays,
                BuildScreenshotPaths(ScreenshotKind::WithOverlays, capture_with_overlays_count),
                extent.width, extent.height,
                swapchain.GetHDR() ? vk::Format::eA2B10G10R10UnormPack32
                                   : swapchain.GetSurfaceFormat().format,
                swapchain.GetHDR());
            auto& readback = pending_screenshots.back();

            const vk::ImageMemoryBarrier to_transfer{
                .srcAccessMask = drawn_access,
                .dstAccessMask = vk::AccessFlagBits::eTransferRead,
                .oldLayout = drawn_layout,
                .newLayout = vk::ImageLayout::eTransferSrcOptimal,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = swapchain_image,
                .subresourceRange{
                    .aspectMask = vk::ImageAspectFlagBits::eColor,
                    .baseMipLevel = 0,
                    .levelCount = 1,
                    .baseArrayLayer = 0,
                    .layerCount = VK_REMAINING_ARRAY_LAYERS,
                },
            };

            cmdbuf.pipelineBarrier(drawn_stage, vk::PipelineStageFlagBits::eTransfer,
                                   vk::DependencyFlagBits::eByRegion, {}, {}, to_transfer);
            CopyImageToReadback(cmdbuf, swapchain_image, vk::ImageLayout::eTransferSrcOptimal,
                                readback);
            swapchain_copied_for_screenshot = true;
        }

        const vk::AccessFlags post_src_access_mask =
            swapchain_copied_for_screenshot ? vk::AccessFlagBits::eTransferRead : drawn_access;
        const vk::ImageLayout post_old_layout =
            swapchain_copied_for_screenshot ? vk::ImageLayout::eTransferSrcOptimal : drawn_layout;
        const vk::ImageMemoryBarrier post_barrier{
            .srcAccessMask = post_src_access_mask,
            .dstAccessMask = vk::AccessFlagBits::eNone,
            .oldLayout = post_old_layout,
            .newLayout = vk::ImageLayout::ePresentSrcKHR,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = swapchain_image,
            .subresourceRange{
                .aspectMask = vk::ImageAspectFlagBits::eColor,
                .baseMipLevel = 0,
                .levelCount = 1,
                .baseArrayLayer = 0,
                .layerCount = VK_REMAINING_ARRAY_LAYERS,
            },
        };
        cmdbuf.pipelineBarrier(vk::PipelineStageFlagBits::eAllCommands,
                               vk::PipelineStageFlagBits::eAllCommands,
                               vk::DependencyFlagBits::eByRegion, {}, {}, post_barrier);

        if (profiler_ctx) {
            TracyVkCollect(profiler_ctx, cmdbuf);
        }
    }
    if (EmulatorSettings.IsVkHostMarkersEnabled()) {
        cmdbuf.endDebugUtilsLabelEXT();
    }

    // Flush vulkan commands.
    std::shared_ptr<std::vector<ScreenshotReadback>> deferred_screenshots{};
    if (!pending_screenshots.empty()) {
        deferred_screenshots =
            std::make_shared<std::vector<ScreenshotReadback>>(std::move(pending_screenshots));
        scheduler.DeferPriorityOperation(
            [deferred_screenshots]() { SavePendingScreenshots(*deferred_screenshots); });
    }

    SubmitInfo info{};
    info.AddWait(swapchain.GetImageAcquiredSemaphore());
    info.AddWait(frame->ready_semaphore, frame->ready_tick);
    info.AddSignal(swapchain.GetPresentReadySemaphore());
    info.AddSignal(frame->present_done);
    const u64 present_tick = scheduler.CurrentTick();
    scheduler.Flush(info);
    if (vr_copy) {
        openxr_presenter->ReleaseFrame(scheduler.GetMasterSemaphore(), present_tick);
    }

    // Present to swapchain.
    bool presented = false;
    if (instance.HasOwnPresentQueue()) {
        // The present queue is its own (VR): only its lock. The driver may block here for a vblank
        // or more (up to 30 ms measured); the game's submits and the headset's frames on the
        // graphics queue no longer wait behind it. Its wait semaphore was signalled by the submit
        // above on the graphics queue (same family: no ownership transfer).
        const auto start = LockTrace::Now(); // VR_LOCK_TRACE only: no clock read without it
        {
            std::scoped_lock present_lock{instance.GetPresentQueueMutex()};
            presented = swapchain.Present();
        }
        if (LockTrace::Enabled()) {
            const double ms =
                std::chrono::duration<double, std::milli>(LockTrace::Now() - start).count();
            if (ms >= 2.0) {
                LOG_INFO(Render_Vulkan,
                         "VR lock: desktop present on its own queue {:.2f} ms (submit_mutex not "
                         "held)",
                         ms);
            }
        }
    } else {
        const Common::Log::DeferScope log_defer; // its log lines: once the lock is released
        const LockTrace::Start lock_start;
        std::scoped_lock submit_lock{Scheduler::submit_mutex};
        LockTrace::Hold trace{"desktop present", lock_start}; // VR_LOCK_TRACE only
        LockTrace::Phase phase{"vkQueuePresentKHR"};
        presented = swapchain.Present();
    }
    if (!presented) {
        // Swapchain::Recreate waits for the device to be idle: every queue's lock (the OpenXR
        // runtime's own queue's too, when it has one: Instance::GetXrQueueMutex; one std::lock,
        // no other lock held, so its order with the frame thread's cannot deadlock).
        const Common::Log::DeferScope log_defer; // its log lines: once the lock is released
        const LockTrace::Start lock_start;
        std::scoped_lock lk{Scheduler::submit_mutex, instance.GetXrQueueMutex(),
                            instance.GetPresentQueueMutex()};
        LockTrace::Hold trace{"desktop present", lock_start}; // VR_LOCK_TRACE only
        LockTrace::Phase phase{"desktop swapchain recreate"};
        swapchain.Recreate(window.GetWidth(), window.GetHeight());
    }

    free_frame();
    if (!is_reusing_frame) {
        DebugState.IncFlipFrameNum();
    }
}

void Presenter::RecordMirrorEye(vk::CommandBuffer cmdbuf, const Frame& frame, vk::Image target,
                                vk::Extent2D extent) {
    using S = vk::PipelineStageFlagBits;
    using A = vk::AccessFlagBits;
    using L = vk::ImageLayout;
    constexpr vk::ImageSubresourceRange color{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .baseMipLevel = 0,
        .levelCount = 1,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    constexpr vk::ImageSubresourceLayers color_layers{
        .aspectMask = vk::ImageAspectFlagBits::eColor,
        .mipLevel = 0,
        .baseArrayLayer = 0,
        .layerCount = 1,
    };
    const auto barrier = [&](vk::Image image, L from, L to, A src_access, A dst_access) {
        return vk::ImageMemoryBarrier{
            .srcAccessMask = src_access,
            .dstAccessMask = dst_access,
            .oldLayout = from,
            .newLayout = to,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = color,
        };
    };
    // The window's image, whatever it held (ImGui did not draw it this frame).
    cmdbuf.pipelineBarrier(S::eColorAttachmentOutput | S::eTransfer, S::eTransfer, {}, {}, {},
                           barrier(target, L::eUndefined, L::eTransferDstOptimal, A::eNone,
                                   A::eTransferWrite));
    const auto format = swapchain.GetSurfaceFormat().format;
    const bool window_bgra = format == vk::Format::eB8G8R8A8Unorm;
    const bool window_rgba = format == vk::Format::eR8G8B8A8Unorm;
    const u32 e = std::min<u32>(frame.vr_desktop_eye_index, 1u);
    const bool typed_srgb = frame.native_eyes[e] &&
                            frame.vr_eye_image[e] == frame.native_eyes[e]->image;
    // Native eyes are typed sRGB images, unlike DIBR's RGBA_UNORM byte containers.
    // Blitting them decodes sRGB and already understands their channel order. Use an
    // sRGB intermediate in the window's order to encode again before the raw copy.
    // The old UNORM intermediate both lost that encoding and swapped R/B twice.
    const vk::Format mid_format = typed_srgb
        ? (window_bgra ? vk::Format::eB8G8R8A8Srgb : vk::Format::eR8G8B8A8Srgb)
        : (VR::DesktopMirror::IntermediateBgra(frame.vr_eye_bgra, window_bgra)
               ? vk::Format::eB8G8R8A8Unorm
               : vk::Format::eR8G8B8A8Unorm);
    const vk::Extent2D eye = frame.vr_eye_extent[e];
    if ((!window_bgra && !window_rgba) || extent.width == 0 || extent.height == 0) {
        // Not an 8-bit window (never in VR): black.
        cmdbuf.clearColorImage(target, L::eTransferDstOptimal,
                               vk::ClearColorValue{std::array{0.0f, 0.0f, 0.0f, 1.0f}}, color);
        return;
    }
    if (!mirror_image || mirror_extent != extent || mirror_format != mid_format) {
        if (mirror_image) {
            // In use by the window's last presents: freed once they are done.
            present_scheduler.DeferOperation(
                [old = std::move(mirror_image)]() mutable { old.Destroy(); });
        }
        mirror_image = VideoCore::UniqueImage(instance.GetDevice(), instance.GetAllocator());
        mirror_image.Create(vk::ImageCreateInfo{
            .imageType = vk::ImageType::e2D,
            .format = mid_format,
            .extent{.width = extent.width, .height = extent.height, .depth = 1},
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eTransferSrc | vk::ImageUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
            .initialLayout = vk::ImageLayout::eUndefined,
        });
        SetObjectName(instance.GetDevice(), static_cast<vk::Image>(mirror_image),
                      "VR desktop mirror {}x{}", extent.width, extent.height);
        mirror_extent = extent;
        mirror_format = mid_format;
        LOG_INFO(Render_Vulkan,
                 "VR desktop mirror: the {} eye ({}x{}, {} bytes) into the {}x{} {} window",
                 e == 0 ? "left" : "right", eye.width, eye.height,
                 frame.vr_eye_bgra ? "BGRA" : "RGBA", extent.width, extent.height,
                 vk::to_string(format));
    }
    // The eye: written by the warp and left in eTransferSrcOptimal for the headset copy in the
    // same queue's earlier submission (whose barrier covers this read too).
    const auto crop = VR::DesktopMirror::CentreCrop(eye.width, eye.height, extent.width,
                                                    extent.height);
    cmdbuf.pipelineBarrier(S::eTransfer, S::eTransfer, {}, {}, {},
                           barrier(mirror_image, L::eUndefined, L::eTransferDstOptimal, A::eNone,
                                   A::eTransferWrite));
    const vk::ImageBlit blit{
        .srcSubresource = color_layers,
        .srcOffsets = std::array{
            vk::Offset3D{crop.x, crop.y, 0},
            vk::Offset3D{crop.x + static_cast<s32>(crop.width),
                         crop.y + static_cast<s32>(crop.height), 1}},
        .dstSubresource = color_layers,
        .dstOffsets = std::array{
            vk::Offset3D{0, 0, 0},
            vk::Offset3D{static_cast<s32>(extent.width), static_cast<s32>(extent.height), 1}},
    };
    cmdbuf.blitImage(frame.vr_eye_image[e], L::eTransferSrcOptimal, mirror_image,
                     L::eTransferDstOptimal, blit, vk::Filter::eLinear);
    cmdbuf.pipelineBarrier(S::eTransfer, S::eTransfer, {}, {}, {},
                           barrier(mirror_image, L::eTransferDstOptimal, L::eTransferSrcOptimal,
                                   A::eTransferWrite, A::eTransferRead));
    // The bytes as they are (core/vr/desktop_mirror.h: the blit put them in the window's order).
    const vk::ImageCopy copy{
        .srcSubresource = color_layers,
        .dstSubresource = color_layers,
        .extent{.width = extent.width, .height = extent.height, .depth = 1},
    };
    cmdbuf.copyImage(mirror_image, L::eTransferSrcOptimal, target, L::eTransferDstOptimal, copy);
}

Frame* Presenter::GetRenderFrame() {
    // Wait for free presentation frames
    Frame* frame;
    {
        std::unique_lock lock{free_mutex};
        free_cv.wait(lock, [this] { return !free_queue.empty(); });
        LOG_DEBUG(Render_Vulkan, "Got render frame, remaining {}", free_queue.size() - 1);

        // Take the frame from the queue
        frame = free_queue.front();
        free_queue.pop();
    }

    const vk::Device device = instance.GetDevice();
    vk::Result result{};

    const auto wait = [&]() {
        result = device.waitForFences(frame->present_done, false, std::numeric_limits<u64>::max());
        return result;
    };

    // Wait for the presentation to be finished so all frame resources are free
    while (wait() != vk::Result::eSuccess) {
        ASSERT_MSG(result != vk::Result::eErrorDeviceLost,
                   "Device lost during waiting for a frame");
        // Retry if the waiting times out
        if (result == vk::Result::eTimeout) {
            continue;
        }
    }

    if (frame->width != expected_frame_width || frame->height != expected_frame_height ||
        frame->is_hdr != swapchain.GetHDR()) {
        RecreateFrame(frame, expected_frame_width, expected_frame_height);
    }

    return frame;
}

void Presenter::SetExpectedGameSize(s32 width, s32 height) {
    if (openxr_presenter && game_extent.width > 0 && game_extent.height > 0) {
        // VR: keep presentation frames at the game's resolution for the headset.
        expected_frame_width = game_extent.width;
        expected_frame_height = game_extent.height;
        return;
    }
    const float ratio = (float)width / (float)height;

    expected_frame_height = height;
    expected_frame_width = width;
    if (ratio > expected_ratio) {
        expected_frame_width = static_cast<s32>(height * expected_ratio);
    } else {
        expected_frame_height = static_cast<s32>(width / expected_ratio);
    }
}

} // namespace Vulkan
