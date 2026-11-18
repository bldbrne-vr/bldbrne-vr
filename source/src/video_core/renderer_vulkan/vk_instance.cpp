#include "common/render_diagnostic.h"
// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#include "video_core/native_stereo/native_stereo.h"
#include <cstdlib>
#include <thread>
#include <fmt/format.h>
#include <fmt/ranges.h>

#include "common/assert.h"
#include "common/debug.h"
#include "common/types.h"
#include "core/vr/desktop_mirror.h"
#include "core/vr/vr_session.h"
#include "imgui/renderer/imgui_core.h"
#include "sdl_window.h"
#include "video_core/renderer_vulkan/liverpool_to_vk.h"
#include "video_core/renderer_vulkan/openxr_presenter.h"
#include "video_core/renderer_vulkan/vk_gpu_crash.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_platform.h"
#include "video_core/renderer_vulkan/vk_residency.h"
#include "video_core/renderer_vulkan/vr_dlss_runtime.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vr_foveation.h"
#include "video_core/renderer_vulkan/vr_xr_queue_policy.h"

#include <vk_mem_alloc.h>

namespace Vulkan {

namespace {

// VR_VRAM_BUDGET_MB=<MB> (tests only, read once): the budget a smaller card would give, so the
// texture and buffer caches set their eviction thresholds as they would there (with a VRAM
// ballast holding the rest of the GPU, a 4 GB card is simulated on a larger one). 0: none.
u64 TestBudgetCap() {
    static const u64 cap = [] {
        const char* test = std::getenv("VR_VRAM_BUDGET_MB");
        return test && *test ? std::strtoull(test, nullptr, 10) << 20 : u64{0};
    }();
    return cap;
}

std::vector<vk::PhysicalDevice> EnumeratePhysicalDevices(vk::UniqueInstance& instance) {
    auto [devices_result, devices] = instance->enumeratePhysicalDevices();
    ASSERT_MSG(devices_result == vk::Result::eSuccess, "Failed to enumerate physical devices: {}",
               vk::to_string(devices_result));
    return std::move(devices);
}

std::vector<std::string> GetSupportedExtensions(vk::PhysicalDevice physical) {
    const auto [extensions_result, extensions] = physical.enumerateDeviceExtensionProperties();
    if (extensions_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Could not query supported extensions: {}",
                  vk::to_string(extensions_result));
        return {};
    }
    std::vector<std::string> supported_extensions;
    supported_extensions.reserve(extensions.size());
    for (const auto& extension : extensions) {
        supported_extensions.emplace_back(extension.extensionName.data());
    }
    return supported_extensions;
}

vk::FormatProperties3 GetFormatProperties(vk::PhysicalDevice physical, vk::Format format) {
    vk::FormatProperties3 properties3{};
    vk::FormatProperties2 properties2 = {
        .pNext = &properties3,
    };
    physical.getFormatProperties2(format, &properties2);
    return properties3;
}

std::unordered_map<vk::Format, vk::FormatProperties3> GetFormatProperties(
    vk::PhysicalDevice physical) {
    std::unordered_map<vk::Format, vk::FormatProperties3> format_properties;
    for (const auto& format_info : LiverpoolToVK::SurfaceFormats()) {
        const auto format = format_info.vk_format;
        if (!format_properties.contains(format)) {
            format_properties.emplace(format, GetFormatProperties(physical, format));
        }
    }
    for (const auto& format_info : LiverpoolToVK::DepthFormats()) {
        const auto format = format_info.vk_format;
        if (!format_properties.contains(format)) {
            format_properties.emplace(format, GetFormatProperties(physical, format));
        }
    }
    // Other miscellaneous formats, e.g. for color buffers, swizzles, or compatibility
    static constexpr std::array misc_formats = {
        vk::Format::eA2R10G10B10UnormPack32,
        vk::Format::eB8G8R8A8Unorm,
        vk::Format::eB8G8R8A8Srgb,
        vk::Format::eD24UnormS8Uint,
    };
    for (const auto& format : misc_formats) {
        if (!format_properties.contains(format)) {
            format_properties.emplace(format, GetFormatProperties(physical, format));
        }
    }
    return format_properties;
}

std::string GetReadableVersion(u32 version) {
    return fmt::format("{}.{}.{}", VK_VERSION_MAJOR(version), VK_VERSION_MINOR(version),
                       VK_VERSION_PATCH(version));
}

} // Anonymous namespace

Instance::Instance(bool enable_validation, bool enable_crash_diagnostic)
    : instance{CreateInstance(Frontend::WindowSystemType::Headless, enable_validation,
                              enable_crash_diagnostic)},
      physical_devices{EnumeratePhysicalDevices(instance)} {}

Instance::Instance(Frontend::WindowSDL& window, s32 physical_device_index,
                   bool enable_validation /*= false*/, bool enable_crash_diagnostic /*= false*/)
    : instance{CreateInstance(window.GetWindowInfo().type, enable_validation,
                              enable_crash_diagnostic)},
      physical_devices{EnumeratePhysicalDevices(instance)} {
    if (enable_validation) {
        debug_callback = CreateDebugCallback(*instance);
    }
    // Before the device: its objects' GPU address ranges, for the crash report (vk_gpu_crash.h).
    address_callback = GpuCrash::AddressMessenger(*instance);
    const std::size_t num_physical_devices = static_cast<u16>(physical_devices.size());
    ASSERT_MSG(num_physical_devices > 0, "No physical devices found");
    LOG_INFO(Render_Vulkan, "Found {} physical devices", num_physical_devices);

    const vk::PhysicalDevice openxr_device =
        OpenXRPresenter::RequiredVulkanPhysicalDevice(*instance);
    if (openxr_device) {
        const auto selected = std::ranges::find(physical_devices, openxr_device);
        if (selected == physical_devices.end()) {
            OpenXRPresenter::Disable(
                "the runtime-selected Vulkan device is not visible to shadPS4");
        } else {
            physical_device = openxr_device;
            if (physical_device_index >= 0 && physical_device_index < num_physical_devices &&
                physical_devices[physical_device_index] != openxr_device) {
                LOG_WARNING(Render_Vulkan,
                            "OpenXR overrides the configured GPU because the runtime requires a "
                            "different Vulkan physical device");
            }
        }
    }

    if (!physical_device && physical_device_index < 0) {
        std::vector<
            std::tuple<size_t, vk::PhysicalDeviceProperties2, vk::PhysicalDeviceMemoryProperties>>
            properties2{};
        for (auto const& physical : physical_devices) {
            properties2.emplace_back(properties2.size(), physical.getProperties2(),
                                     physical.getMemoryProperties());
        }
        std::sort(properties2.begin(), properties2.end(), [](const auto& left, const auto& right) {
            const vk::PhysicalDeviceProperties& left_prop = std::get<1>(left).properties;
            const vk::PhysicalDeviceProperties& right_prop = std::get<1>(right).properties;
            const bool left_supports_api = left_prop.apiVersion >= TargetVulkanApiVersion;
            const bool right_supports_api = right_prop.apiVersion >= TargetVulkanApiVersion;
            if (left_supports_api != right_supports_api) {
                return left_supports_api;
            }

            const bool left_is_discrete =
                left_prop.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
            const bool right_is_discrete =
                right_prop.deviceType == vk::PhysicalDeviceType::eDiscreteGpu;
            if (left_is_discrete != right_is_discrete) {
                return left_is_discrete;
            }

            constexpr auto get_mem = [](const vk::PhysicalDeviceMemoryProperties& mem) -> size_t {
                size_t max = 0;
                for (u32 i = 0; i < mem.memoryHeapCount; i++) {
                    const vk::MemoryHeap& heap = mem.memoryHeaps[i];
                    if (heap.flags & vk::MemoryHeapFlagBits::eDeviceLocal && heap.size > max) {
                        max = heap.size;
                    }
                }
                return max;
            };
            size_t left_mem_size = get_mem(std::get<2>(left));
            size_t right_mem_size = get_mem(std::get<2>(right));
            return left_mem_size > right_mem_size;
        });
        physical_device = physical_devices[std::get<0>(properties2[0])];
    } else if (!physical_device) {
        ASSERT_MSG(physical_device_index < num_physical_devices,
                   "Invalid physical device index {} provided when only {} devices exist",
                   physical_device_index, num_physical_devices);

        physical_device = physical_devices[physical_device_index];
    }

    available_extensions = GetSupportedExtensions(physical_device);
    format_properties = GetFormatProperties(physical_device);
    properties = physical_device.getProperties();
    memory_properties = physical_device.getMemoryProperties();
    VR::Graphics::neural_gpu.store(
        VR::Graphics::Rtx50(properties.vendorID, properties.deviceName.data()) ? 1 : 0);
    VR::Graphics::amd_gpu.store(properties.vendorID == 0x1002 ? 1 : 0);
    // The player's DLSS 5 file is checked off this thread; the VR page offers DLSS 5 once it passes.
    if (VR::Graphics::neural_gpu.load() == 1)
        std::thread([] { VerifiedNeuralFile(); }).detach();
    else
        VR::Graphics::neural_file.store(0);
    CollectDeviceParameters();
    RENDER_DIAGNOSTIC_ONCE("GPU name={} vendor={:#x} device={:#x} driver={} Vulkan={}",
        properties.deviceName.data(), properties.vendorID, properties.deviceID,
        GetDriverVersionName(), properties.apiVersion);
    ASSERT_MSG(properties.apiVersion >= TargetVulkanApiVersion,
               "Vulkan {}.{} is required, but only {}.{} is supported by device!",
               VK_VERSION_MAJOR(TargetVulkanApiVersion), VK_VERSION_MINOR(TargetVulkanApiVersion),
               VK_VERSION_MAJOR(properties.apiVersion), VK_VERSION_MINOR(properties.apiVersion));

    CreateDevice();
    CollectPhysicalMemoryInfo();
    CollectImageFormatInfo();
    CollectToolingInfo();
}

Instance::~Instance() {
    auto& dlss = VrDlssRuntime::Get();
    if (device && dlss.NeedsShutdown()) {
        // Teardown only. Never a wait in the off path or a per-frame DLSS evaluation.
        std::scoped_lock lock{Scheduler::submit_mutex, present_queue_mutex, xr_queue_mutex};
        const auto idle = device->waitIdle();
        if (idle == vk::Result::eSuccess || idle == vk::Result::eErrorDeviceLost)
            dlss.Shutdown(UINT64_MAX);
    }
    ImGui::Core::Shutdown(GetDevice());
    vmaDestroyAllocator(allocator);
}

std::string Instance::GetDriverVersionName() {
    // Extracted from
    // https://github.com/SaschaWillems/vulkan.gpuinfo.org/blob/5dddea46ea1120b0df14eef8f15ff8e318e35462/functions.php#L308-L314
    const u32 version = properties.driverVersion;
    if (driver_id == vk::DriverId::eNvidiaProprietary) {
        const u32 major = (version >> 22) & 0x3ff;
        const u32 minor = (version >> 14) & 0x0ff;
        const u32 secondary = (version >> 6) & 0x0ff;
        const u32 tertiary = version & 0x003f;
        return fmt::format("{}.{}.{}.{}", major, minor, secondary, tertiary);
    }
    if (driver_id == vk::DriverId::eIntelProprietaryWindows) {
        const u32 major = version >> 14;
        const u32 minor = version & 0x3fff;
        return fmt::format("{}.{}", major, minor);
    }
    return GetReadableVersion(version);
}

bool Instance::CreateDevice() {
    const vk::StructureChain feature_chain =
        physical_device
            .getFeatures2<vk::PhysicalDeviceFeatures2, vk::PhysicalDeviceVulkan11Features,
                          vk::PhysicalDeviceVulkan12Features, vk::PhysicalDeviceVulkan13Features,
                          vk::PhysicalDeviceRobustness2FeaturesEXT,
                          vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT,
                          vk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT,
                          vk::PhysicalDeviceShaderAtomicFloat2FeaturesEXT,
                          vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR,
                          vk::PhysicalDeviceImage2DViewOf3DFeaturesEXT,
                          vk::PhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT>();
    features = feature_chain.get().features;

    const vk::StructureChain properties_chain = physical_device.getProperties2<
        vk::PhysicalDeviceProperties2, vk::PhysicalDeviceVulkan11Properties,
        vk::PhysicalDeviceVulkan12Properties, vk::PhysicalDeviceVulkan13Properties,
        vk::PhysicalDevicePushDescriptorPropertiesKHR>();
    vk11_props = properties_chain.get<vk::PhysicalDeviceVulkan11Properties>();
    if (vk11_props.deviceLUIDValid) {
        std::array<u8, 8> luid{};
        std::copy_n(vk11_props.deviceLUID.begin(), luid.size(), luid.begin());
        residency = std::make_unique<Residency::Reader>(luid);
        LOG_INFO(Render_Vulkan, "Residency: {}", residency->Found()
                     ? "reading Windows' GPU memory counters every 2 s (the spill watch is on)"
                     : "no pdh.dll (the spill watch is off)");
    }
    vk12_props = properties_chain.get<vk::PhysicalDeviceVulkan12Properties>();
    vk13_props = properties_chain.get<vk::PhysicalDeviceVulkan13Properties>();
    push_descriptor_props = properties_chain.get<vk::PhysicalDevicePushDescriptorPropertiesKHR>();
    LOG_INFO(Render_Vulkan, "Physical device subgroup size {}", vk11_props.subgroupSize);

    if (available_extensions.empty()) {
        LOG_CRITICAL(Render_Vulkan, "No extensions supported by device.");
        return false;
    }

    std::vector<const char*> enabled_extensions;
    enabled_extensions.reserve(48);
    const auto add_extension = [&](std::string_view extension) -> bool {
        const auto result =
            std::find_if(available_extensions.begin(), available_extensions.end(),
                         [&](const std::string& name) { return name == extension; });

        if (result != available_extensions.end()) {
            LOG_INFO(Render_Vulkan, "Enabling extension: {}", extension);
            enabled_extensions.push_back(extension.data());
            return true;
        }

        LOG_WARNING(Render_Vulkan, "Extension {} unavailable.", extension);
        return false;
    };

    // Required
    ASSERT_MSG(add_extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME),
               "Required Vulkan extension unavailable: {}", VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    ASSERT_MSG(add_extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME),
               "Required Vulkan extension unavailable: {}", VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
    ASSERT_MSG(add_extension(VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME),
               "Required Vulkan extension unavailable: {}",
               VK_EXT_VERTEX_ATTRIBUTE_DIVISOR_EXTENSION_NAME);
    ASSERT_MSG(add_extension(VK_EXT_ROBUSTNESS_2_EXTENSION_NAME),
               "Required Vulkan extension unavailable: {}", VK_EXT_ROBUSTNESS_2_EXTENSION_NAME);

    for (const auto& extension : OpenXRPresenter::RequiredVulkanDeviceExtensions()) {
        if (!add_extension(extension)) {
            OpenXRPresenter::Disable(
                fmt::format("required Vulkan device extension {} is unavailable", extension));
        }
    }

    const auto robustness2_features = feature_chain.get<vk::PhysicalDeviceRobustness2FeaturesEXT>();
    ASSERT_MSG(robustness2_features.robustBufferAccess2,
               "Required Vulkan feature unavailable: robustBufferAccess2");
    ASSERT_MSG(robustness2_features.robustImageAccess2,
               "Required Vulkan feature unavailable: robustImageAccess2");
    ASSERT_MSG(robustness2_features.nullDescriptor,
               "Required Vulkan feature unavailable: nullDescriptor");

    // Optional
    maintenance_8 = add_extension(VK_KHR_MAINTENANCE_8_EXTENSION_NAME);
    attachment_feedback_loop = add_extension(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_LAYOUT_EXTENSION_NAME);
    if (attachment_feedback_loop) {
        attachment_feedback_loop =
            add_extension(VK_EXT_ATTACHMENT_FEEDBACK_LOOP_DYNAMIC_STATE_EXTENSION_NAME);
        if (!attachment_feedback_loop) {
            // We want both extensions so remove the first if the second isn't available
            enabled_extensions.pop_back();
        }
    }
    depth_range_unrestricted = add_extension(VK_EXT_DEPTH_RANGE_UNRESTRICTED_EXTENSION_NAME);
    dynamic_state_3 = add_extension(VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME);
    if (dynamic_state_3) {
        dynamic_state_3_features =
            feature_chain.get<vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT>();
        LOG_INFO(Render_Vulkan, "- extendedDynamicState3ColorWriteMask: {}",
                 dynamic_state_3_features.extendedDynamicState3ColorWriteMask);
    }
    custom_border_color = add_extension(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
    depth_clip_control = add_extension(VK_EXT_DEPTH_CLIP_CONTROL_EXTENSION_NAME);
    depth_clip_enable = add_extension(VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME);
    vertex_input_dynamic_state = add_extension(VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME);
    list_restart = add_extension(VK_EXT_PRIMITIVE_TOPOLOGY_LIST_RESTART_EXTENSION_NAME);
    if (list_restart) {
        list_restart_features =
            feature_chain.get<vk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT>();
        LOG_INFO(Render_Vulkan, "- primitiveTopologyListRestart: {}",
                 list_restart_features.primitiveTopologyListRestart);
        LOG_INFO(Render_Vulkan, "- primitiveTopologyPatchListRestart: {}",
                 list_restart_features.primitiveTopologyPatchListRestart);
    }
    amd_shader_explicit_vertex_parameter =
        add_extension(VK_AMD_SHADER_EXPLICIT_VERTEX_PARAMETER_EXTENSION_NAME);
    if (!amd_shader_explicit_vertex_parameter) {
        fragment_shader_barycentric =
            add_extension(VK_KHR_FRAGMENT_SHADER_BARYCENTRIC_EXTENSION_NAME);
    }
    provoking_vertex = add_extension(VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME);
    shader_stencil_export = add_extension(VK_EXT_SHADER_STENCIL_EXPORT_EXTENSION_NAME);
    image_load_store_lod = add_extension(VK_AMD_SHADER_IMAGE_LOAD_STORE_LOD_EXTENSION_NAME);
    amd_gcn_shader = add_extension(VK_AMD_GCN_SHADER_EXTENSION_NAME);
    amd_shader_trinary_minmax = add_extension(VK_AMD_SHADER_TRINARY_MINMAX_EXTENSION_NAME);
    nv_framebuffer_mixed_samples = add_extension(VK_NV_FRAMEBUFFER_MIXED_SAMPLES_EXTENSION_NAME);
    amd_mixed_attachment_samples = add_extension(VK_AMD_MIXED_ATTACHMENT_SAMPLES_EXTENSION_NAME);
    shader_atomic_float = add_extension(VK_EXT_SHADER_ATOMIC_FLOAT_EXTENSION_NAME);
    shader_atomic_float2 = add_extension(VK_EXT_SHADER_ATOMIC_FLOAT_2_EXTENSION_NAME);
    if (shader_atomic_float2) {
        shader_atomic_float2_features =
            feature_chain.get<vk::PhysicalDeviceShaderAtomicFloat2FeaturesEXT>();
        LOG_INFO(Render_Vulkan, "- shaderBufferFloat32AtomicMinMax: {}",
                 shader_atomic_float2_features.shaderBufferFloat32AtomicMinMax);
        LOG_INFO(Render_Vulkan, "- shaderImageFloat32AtomicMinMax: {}",
                 shader_atomic_float2_features.shaderImageFloat32AtomicMinMax);
    }
    workgroup_memory_explicit_layout =
        add_extension(VK_KHR_WORKGROUP_MEMORY_EXPLICIT_LAYOUT_EXTENSION_NAME);
    if (workgroup_memory_explicit_layout) {
        workgroup_memory_explicit_layout_features =
            feature_chain.get<vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR>();
        LOG_INFO(Render_Vulkan, "- workgroupMemoryExplicitLayout: {}",
                 workgroup_memory_explicit_layout_features.workgroupMemoryExplicitLayout);
        LOG_INFO(Render_Vulkan, "- workgroupMemoryExplicitLayoutScalarBlockLayout: {}",
                 workgroup_memory_explicit_layout_features
                     .workgroupMemoryExplicitLayoutScalarBlockLayout);
        LOG_INFO(
            Render_Vulkan, "- workgroupMemoryExplicitLayout16BitAccess: {}",
            workgroup_memory_explicit_layout_features.workgroupMemoryExplicitLayout16BitAccess);
    }
    image_2d_view_of_3d = add_extension(VK_EXT_IMAGE_2D_VIEW_OF_3D_EXTENSION_NAME);
    if (image_2d_view_of_3d) {
        image_2d_view_of_3d_features =
            feature_chain.get<vk::PhysicalDeviceImage2DViewOf3DFeaturesEXT>();
        LOG_INFO(Render_Vulkan, "- image2DViewOf3D: {}",
                 image_2d_view_of_3d_features.image2DViewOf3D);
        LOG_INFO(Render_Vulkan, "- sampler2DViewOf3D: {}",
                 image_2d_view_of_3d_features.sampler2DViewOf3D);
    }
    image_view_min_lod = add_extension(VK_EXT_IMAGE_VIEW_MIN_LOD_EXTENSION_NAME);
    // FAST bit 43 part 2 (vk_pass_breaks.h). Enabling it only relaxes validity rules (pipeline
    // and render pass colour attachment counts may differ); nothing is drawn differently until
    // the scheduler continues a pass with it.
    dynamic_rendering_unused_attachments =
        feature_chain.get<vk::PhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT>()
            .dynamicRenderingUnusedAttachments &&
        add_extension(VK_EXT_DYNAMIC_RENDERING_UNUSED_ATTACHMENTS_EXTENSION_NAME);
    // Fixed foveated rendering (vr_foveation.h), with the shading rate taken from an attachment.
    // 16x16 texels where the card allows them (else its smallest), and 4x4 fragments only where
    // the card lists them. Enabled on every card that can, whatever vr.ini foveation says at the
    // start: the level is live (the VR page), Off only leaves the rate map out of the passes.
    // VR_FOVEATED (tests) fixes it for the run instead, and VR_FOVEATED=0 keeps the extension out.
    const char* const foveation_env = std::getenv("VR_FOVEATED");
    foveation_fixed = Foveation::Fixed(foveation_env);
    foveation_settings = Foveation::Choose(foveation_env,
                                           VR::Graphics::foveation.load(std::memory_order_relaxed));
    if ((foveation_settings.on || !foveation_fixed) &&
        std::ranges::find(available_extensions, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) !=
            available_extensions.end()) {
        const auto rate_features =
            physical_device
                .getFeatures2<vk::PhysicalDeviceFeatures2,
                              vk::PhysicalDeviceFragmentShadingRateFeaturesKHR>()
                .get<vk::PhysicalDeviceFragmentShadingRateFeaturesKHR>();
        const auto rate_props =
            physical_device
                .getProperties2<vk::PhysicalDeviceProperties2,
                                vk::PhysicalDeviceFragmentShadingRatePropertiesKHR>()
                .get<vk::PhysicalDeviceFragmentShadingRatePropertiesKHR>();
        // Coarse shading may be restricted for fragment shaders that write depth/stencil or the
        // sample mask (AMD RDNA 2 and Intel Arc report both on Windows, NVIDIA neither): those
        // pipelines then ignore the rate map (vk_graphics_pipeline.cpp), the rest are foveated.
        // A pipeline sample mask is allowed by every card reported so far; a card that restricts
        // it keeps full rate everywhere.
        foveation_shader_depth = rate_props.fragmentShadingRateWithShaderDepthStencilWrites;
        foveation_shader_mask = rate_props.fragmentShadingRateWithShaderSampleMask;
        if (!rate_props.fragmentShadingRateWithSampleMask) {
            LOG_WARNING(Render_Vulkan,
                        "VR foveation: off, the card restricts coarse shading with a sample mask");
        }
        if (rate_features.attachmentFragmentShadingRate &&
            rate_props.fragmentShadingRateWithSampleMask &&
            add_extension(VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME)) {
            foveation = true;
            const auto lo = rate_props.minFragmentShadingRateAttachmentTexelSize;
            const auto hi = rate_props.maxFragmentShadingRateAttachmentTexelSize;
            foveation_texel = {std::clamp(16u, lo.width, hi.width),
                               std::clamp(16u, lo.height, hi.height)};
            foveation_layered = rate_props.layeredShadingRateAttachments;
            const auto [rates_result, rates] = physical_device.getFragmentShadingRatesKHR();
            if (rates_result == vk::Result::eSuccess)
                for (const auto& rate : rates)
                    foveation_4x4 |= rate.fragmentSize == vk::Extent2D{4, 4};
        }
        LOG_INFO(Render_Vulkan,
                 "VR foveation: {} (attachment rates {}, texel {}x{}, layered {}, 4x4 {}, coarse "
                 "with shader depth/stencil {}, with shader sample mask {}); at the start {} "
                 "(inner {:.2f}, outer {:.2f}){}",
                 foveation ? "available" : "unavailable",
                 bool(rate_features.attachmentFragmentShadingRate), foveation_texel.width,
                 foveation_texel.height, foveation_layered, foveation_4x4, foveation_shader_depth,
                 foveation_shader_mask, foveation_settings.on ? "on" : "off",
                 foveation_settings.inner, foveation_settings.outer,
                 foveation_fixed ? ", fixed by VR_FOVEATED" : ", live (vr.ini foveation)");
    }
    supports_memory_budget = add_extension(VK_EXT_MEMORY_BUDGET_EXTENSION_NAME);
    // GPU crash reports (vk_gpu_crash.h): the driver's own fault report (a query made only after
    // a lost device) wherever the device has it, the checkpoint markers when wanted (NVIDIA).
    bool device_fault = false;
    if (std::ranges::find(available_extensions, VK_EXT_DEVICE_FAULT_EXTENSION_NAME) !=
        available_extensions.end()) {
        device_fault = physical_device
                           .getFeatures2<vk::PhysicalDeviceFeatures2,
                                         vk::PhysicalDeviceFaultFeaturesEXT>()
                           .get<vk::PhysicalDeviceFaultFeaturesEXT>()
                           .deviceFault &&
                       add_extension(VK_EXT_DEVICE_FAULT_EXTENSION_NAME);
    }
    gpu_checkpoints = GpuCrash::WantCheckpoints() &&
                      add_extension(VK_NV_DEVICE_DIAGNOSTIC_CHECKPOINTS_EXTENSION_NAME);
    // Every object's GPU address range, told to the instance's address messenger.
    bool address_report = false;
    if (address_callback &&
        std::ranges::find(available_extensions,
                          VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME) !=
            available_extensions.end()) {
        address_report = physical_device
                             .getFeatures2<vk::PhysicalDeviceFeatures2,
                                           vk::PhysicalDeviceAddressBindingReportFeaturesEXT>()
                             .get<vk::PhysicalDeviceAddressBindingReportFeaturesEXT>()
                             .reportAddressBinding &&
                         add_extension(VK_EXT_DEVICE_ADDRESS_BINDING_REPORT_EXTENSION_NAME);
    }
    const bool calibrated_timestamps =
        TRACY_GPU_ENABLED ? add_extension(VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME) : false;

    const auto family_properties = physical_device.getQueueFamilyProperties();
    if (family_properties.empty()) {
        LOG_CRITICAL(Render_Vulkan, "Physical device reported no queues.");
        return false;
    }

    bool graphics_queue_found = false;
    for (std::size_t i = 0; i < family_properties.size(); i++) {
        const u32 index = static_cast<u32>(i);
        if (family_properties[i].queueFlags & vk::QueueFlagBits::eGraphics) {
            queue_family_index = index;
            graphics_queue_found = true;
        }
    }

    if (!graphics_queue_found) {
        LOG_CRITICAL(Render_Vulkan, "Unable to find graphics and/or present queues.");
        return false;
    }

    // The buffer arenas (FAST_BUFFER_ARENA, buffer_cache/sparse_pool.h): sparse buffers, with the
    // graphics queue binding their memory. On by default (2026-11-05); FAST_BUFFER_ARENA=0 turns
    // them off. A device without sparse buffers keeps ordinary ones, as before.
    {
        const char* arena = std::getenv("FAST_BUFFER_ARENA");
        const bool arena_on = arena == nullptr || arena[0] != '0';
        // Aliased too: a merge binds the old views' pieces in the new view, and a twin the
        // view's pieces (VkBufferCreateInfo: SPARSE_ALIASED needs sparseResidencyAliased).
        sparse_buffers = arena_on && features.sparseBinding &&
                         features.sparseResidencyBuffer && features.sparseResidencyAliased &&
                         (family_properties[queue_family_index].queueFlags &
                          vk::QueueFlagBits::eSparseBinding);
        if (arena_on) {
            LOG_INFO(Render_Vulkan, "Buffer arenas: sparse buffers {} (sparseBinding={} "
                     "sparseResidencyBuffer={} sparseResidencyAliased={} queue={})",
                     sparse_buffers ? "on" : "UNSUPPORTED", bool(features.sparseBinding),
                     bool(features.sparseResidencyBuffer), bool(features.sparseResidencyAliased),
                     bool(family_properties[queue_family_index].queueFlags &
                          vk::QueueFlagBits::eSparseBinding));
        }
    }

    // VR: the desktop present gets a second queue of the family when it has one
    // (HasOwnPresentQueue). Queue 0 stays the graphics queue: the game's, the VR presenter's and,
    // unless it has its own (below), the OpenXR session's. VR_PRESENT_OWN_QUEUE=0 turns
    // it off: the present then takes submit_mutex on queue 0, exactly as before 2026-10-09.
    const char* own_present_env = std::getenv("VR_PRESENT_OWN_QUEUE");
    const bool own_present_off = own_present_env != nullptr && own_present_env[0] == '0';
    const bool own_present = OpenXRPresenter::IsRequested() && !own_present_off &&
                             family_properties[queue_family_index].queueCount >= 2;
    // The desktop present's queue only a hint of the lowest priority while the desktop window is
    // not the one of before 2026-10-09 (desktop_mirror != 1 or desktop_mirror_view an eye):
    // the headset's work goes first when both are ready. A per-queue global priority is not
    // possible here (one VkDeviceQueueCreateInfo for the family, VUID-...-02802).
    const auto& vr_settings = VR::GetSettings();
    const bool low_present = own_present && VR::DesktopMirror::LowPresentPriority(
                                                vr_settings.desktop_mirror,
                                                static_cast<VR::DesktopMirror::View>(
                                                    vr_settings.desktop_mirror_view));
    // VR: the OpenXR runtime gets a queue of the family of its own too when there is one more
    // (VR_XR_OWN_QUEUE, vr_xr_queue_policy.h: by default on every runtime but Meta's): its
    // xrEndFrame, which SteamVR spends most of a refresh in, then never holds the game's submits.
    // Unset on Meta's runtime, or VR_XR_OWN_QUEUE off ("0", "off", "false", "no"): the session
    // shares queue 0, as before.
    const u32 queues_before = own_present ? 2u : 1u;
    VrXrQueuePolicy::OwnQueue own_xr{};
    if (OpenXRPresenter::IsRequested() && OpenXRPresenter::IsRuntimeAvailable()) {
        own_xr = VrXrQueuePolicy::ChooseOwnQueue(
            std::getenv("VR_XR_OWN_QUEUE"),
            VrXrQueuePolicy::Classify(OpenXRPresenter::RuntimeName()),
            family_properties[queue_family_index].queueCount, queues_before);
    }
    // Queue 0 the graphics queue, then the desktop present's, then the runtime's. The runtime's
    // has the graphics queue's priority (1.0, the highest; priorities are only hints, and with
    // equal ones neither goes first); the desktop present's may be lower (low_present).
    std::array<float, 4> queue_priorities{1.0f, 1.0f, 1.0f, 1.0f};
    if (own_present) {
        queue_priorities[1] = low_present ? 0.0f : 1.0f;
    }
    // Sparse buffers: their binds on a queue of the family of their own when it has one more
    // (FAST_SPARSE_OWN_QUEUE, '0' off): a bind on the graphics queue waits for the frames ahead of
    // it and the GPU idles behind it (30-40 ms load-in frames, 2026-11-05); on its own queue only
    // the submit that needs the new memory waits for it (SparsePool's semaphore).
    const u32 queues_with_xr = queues_before + (own_xr.own ? 1u : 0u);
    {
        const char* own = std::getenv("FAST_SPARSE_OWN_QUEUE");
        own_sparse_queue = sparse_buffers && !(own && own[0] == '0') &&
                           family_properties[queue_family_index].queueCount > queues_with_xr;
    }
    const vk::DeviceQueueCreateInfo queue_info = {
        .queueFamilyIndex = queue_family_index,
        .queueCount = queues_with_xr + (own_sparse_queue ? 1u : 0u),
        .pQueuePriorities = queue_priorities.data(),
    };

    // Optional compute derivatives for FSR 4. Enabling a feature adds no passes or
    // allocations; provider construction stays gated by accepted graphics settings.
    const bool derivative_extension =
        std::ranges::find(available_extensions, VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME) != available_extensions.end();
    const bool derivative_nv = !derivative_extension &&
        std::ranges::find(available_extensions, VK_NV_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME) != available_extensions.end();
    vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR derivatives{};
    if (derivative_extension || derivative_nv) {
        derivatives = physical_device.getFeatures2<vk::PhysicalDeviceFeatures2,
            vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR>()
            .get<vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR>();
        if (derivatives.computeDerivativeGroupLinear)
            add_extension(derivative_extension ? VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME
                                               : VK_NV_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME);
    }
    derivatives.pNext = nullptr;

    VrDlssRuntime::Get().PrepareDevice(
        *instance, physical_device, enabled_extensions,
        VR::Graphics::Accepted().upscaler == VR::Graphics::Upscaler::Dlss);

    // FSR 4.1 (AMD's FSR SDK 2.4.0 upscaler, the fsr41 bridge): what AMD's Vulkan backend and its
    // FP8 / INT8 models use (the extensions its upscaler DLL names), enabled for an FSR 4.1
    // session only and only where the card has them, so no other session's device changes.
    // AMD's backend picks AMD device-coherent memory types and loads the KHR-named
    // memory-requirements command whenever the card offers them, so those are enabled too.
    const bool fsr41 = VR::Graphics::Accepted().upscaler == VR::Graphics::Upscaler::Fsr41;
    const auto available = [&](std::string_view extension) {
        return std::ranges::find(available_extensions, extension) != available_extensions.end();
    };
    const auto add_once = [&](std::string_view extension) {
        if (std::ranges::find_if(enabled_extensions, [&](const char* e) {
                return extension == e;
            }) != enabled_extensions.end())
            return true;
        return add_extension(extension);
    };
    vk::PhysicalDeviceCooperativeMatrixFeaturesKHR fsr41_matrix{};
    vk::PhysicalDeviceShaderFloat8FeaturesEXT fsr41_float8{};
    vk::PhysicalDeviceCoherentMemoryFeaturesAMD fsr41_coherent{};
    if (fsr41) {
        add_once(VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME);
        add_once(VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME);
        if (available(VK_AMD_BUFFER_MARKER_EXTENSION_NAME))
            add_once(VK_AMD_BUFFER_MARKER_EXTENSION_NAME);
        if (available(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME)) {
            fsr41_matrix = physical_device.getFeatures2<vk::PhysicalDeviceFeatures2,
                vk::PhysicalDeviceCooperativeMatrixFeaturesKHR>()
                .get<vk::PhysicalDeviceCooperativeMatrixFeaturesKHR>();
            if (!fsr41_matrix.cooperativeMatrix || !add_once(VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME))
                fsr41_matrix = {};
        }
        if (available(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME)) {
            fsr41_float8 = physical_device.getFeatures2<vk::PhysicalDeviceFeatures2,
                vk::PhysicalDeviceShaderFloat8FeaturesEXT>()
                .get<vk::PhysicalDeviceShaderFloat8FeaturesEXT>();
            // Its cooperative-matrix use needs the matrix feature itself.
            fsr41_float8.shaderFloat8CooperativeMatrix &= fsr41_matrix.cooperativeMatrix;
            if (!fsr41_float8.shaderFloat8 || !add_once(VK_EXT_SHADER_FLOAT8_EXTENSION_NAME))
                fsr41_float8 = {};
        }
        if (available(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME)) {
            fsr41_coherent = physical_device.getFeatures2<vk::PhysicalDeviceFeatures2,
                vk::PhysicalDeviceCoherentMemoryFeaturesAMD>()
                .get<vk::PhysicalDeviceCoherentMemoryFeaturesAMD>();
            if (!fsr41_coherent.deviceCoherentMemory ||
                !add_once(VK_AMD_DEVICE_COHERENT_MEMORY_EXTENSION_NAME))
                fsr41_coherent = {};
        }
        LOG_INFO(Render_Vulkan,
                 "FSR 4.1 device: cooperative matrix {}, float8 {} (matrix {}), AMD coherent "
                 "memory {}",
                 bool(fsr41_matrix.cooperativeMatrix), bool(fsr41_float8.shaderFloat8),
                 bool(fsr41_float8.shaderFloat8CooperativeMatrix),
                 bool(fsr41_coherent.deviceCoherentMemory));
    }
    fsr41_matrix.pNext = fsr41_float8.pNext = fsr41_coherent.pNext = nullptr;

    const auto vk11_features = feature_chain.get<vk::PhysicalDeviceVulkan11Features>();
    vk12_features = feature_chain.get<vk::PhysicalDeviceVulkan12Features>();
    vk13_features = feature_chain.get<vk::PhysicalDeviceVulkan13Features>();
    vr_fsr4_supported = derivatives.computeDerivativeGroupLinear && features.shaderInt16 &&
        features.shaderStorageImageExtendedFormats && vk11_features.storageBuffer16BitAccess &&
        vk12_features.storageBuffer8BitAccess && vk12_features.shaderInt8 &&
        vk12_features.shaderFloat16 && vk13_features.shaderIntegerDotProduct;
    if (NativeStereo::Enabled()) {
        // Native stereo draws both eyes with multiview or, on AMD RX 6000, layer draws;
        // every report then says what the card offers (a right eye left gray on some AMD cards,
        // 2026-10-21).
        RENDER_DIAGNOSTIC_ONCE("Native stereo multiview: multiview={} geometry={} tessellation={} "
                               "vendor={:#x} device={:#x} driver={:#x}",
                               bool(vk11_features.multiview),
                               bool(vk11_features.multiviewGeometryShader),
                               bool(vk11_features.multiviewTessellationShader),
                               properties.vendorID, properties.deviceID, properties.driverVersion);
    }
    // Layer draws or multiview, before any pipeline or pass is made, in DIBR sessions too: the
    // shader cache's profile carries the card's mode (NativeStereo::LayerDrawsMode).
    NativeStereo::ChooseLayerDraws(properties.vendorID, properties.deviceID,
                                   properties.deviceName.data(), vk12_features.shaderOutputLayer);
    vk::StructureChain device_chain = {
        vk::DeviceCreateInfo{
            .queueCreateInfoCount = 1u,
            .pQueueCreateInfos = &queue_info,
            .enabledExtensionCount = static_cast<u32>(enabled_extensions.size()),
            .ppEnabledExtensionNames = enabled_extensions.data(),
        },
        vk::PhysicalDeviceFeatures2{
            .features{
                .robustBufferAccess = features.robustBufferAccess,
                .imageCubeArray = features.imageCubeArray,
                .independentBlend = features.independentBlend,
                .geometryShader = features.geometryShader,
                .tessellationShader = features.tessellationShader,
                .sampleRateShading = features.sampleRateShading,
                .dualSrcBlend = features.dualSrcBlend,
                .logicOp = features.logicOp,
                .multiDrawIndirect = features.multiDrawIndirect,
                .depthClamp = features.depthClamp,
                .depthBiasClamp = features.depthBiasClamp,
                .fillModeNonSolid = features.fillModeNonSolid,
                .depthBounds = features.depthBounds,
                .wideLines = features.wideLines,
                .multiViewport = features.multiViewport,
                .samplerAnisotropy = features.samplerAnisotropy,
                .vertexPipelineStoresAndAtomics = features.vertexPipelineStoresAndAtomics,
                .fragmentStoresAndAtomics = features.fragmentStoresAndAtomics,
                .shaderImageGatherExtended = features.shaderImageGatherExtended,
                .shaderStorageImageExtendedFormats = features.shaderStorageImageExtendedFormats,
                .shaderStorageImageMultisample = features.shaderStorageImageMultisample,
                .shaderSampledImageArrayDynamicIndexing = features.shaderSampledImageArrayDynamicIndexing,
                .shaderStorageBufferArrayDynamicIndexing = features.shaderStorageBufferArrayDynamicIndexing,
                .shaderStorageImageArrayDynamicIndexing = features.shaderStorageImageArrayDynamicIndexing,
                .shaderClipDistance = features.shaderClipDistance,
                .shaderFloat64 = features.shaderFloat64,
                .shaderInt64 = features.shaderInt64,
                .shaderInt16 = features.shaderInt16,
                .sparseBinding = sparse_buffers,
                .sparseResidencyBuffer = sparse_buffers,
                .sparseResidencyAliased = sparse_buffers,
            },
        },
        vk::PhysicalDeviceVulkan11Features{
            .storageBuffer16BitAccess = vk11_features.storageBuffer16BitAccess,
            .multiview = (std::getenv("VR_FOREGROUND_PROBE") || NativeStereo::Enabled()) && vk11_features.multiview,
            .multiviewGeometryShader = NativeStereo::Enabled() && vk11_features.multiviewGeometryShader,
            .multiviewTessellationShader = NativeStereo::Enabled() && vk11_features.multiviewTessellationShader,
            .shaderDrawParameters = vk11_features.shaderDrawParameters,
        },
        vk::PhysicalDeviceVulkan12Features{
            .samplerMirrorClampToEdge = vk12_features.samplerMirrorClampToEdge,
            .drawIndirectCount = vk12_features.drawIndirectCount,
            .storageBuffer8BitAccess = vk12_features.storageBuffer8BitAccess,
            .shaderBufferInt64Atomics = vk12_features.shaderBufferInt64Atomics,
            .shaderSharedInt64Atomics = vk12_features.shaderSharedInt64Atomics,
            .shaderFloat16 = vk12_features.shaderFloat16,
            .shaderInt8 = vk12_features.shaderInt8,
            // FSR 4.1: the descriptor indexing its backend names (VK_EXT_descriptor_indexing).
            .descriptorIndexing = fsr41 && vk12_features.descriptorIndexing,
            .shaderUniformBufferArrayNonUniformIndexing = fsr41 && vk12_features.shaderUniformBufferArrayNonUniformIndexing,
            .shaderSampledImageArrayNonUniformIndexing = (NativeStereo::Enabled() || fsr41) && vk12_features.shaderSampledImageArrayNonUniformIndexing,
            .shaderStorageBufferArrayNonUniformIndexing = (NativeStereo::Enabled() || fsr41) && vk12_features.shaderStorageBufferArrayNonUniformIndexing,
            .shaderStorageImageArrayNonUniformIndexing = (NativeStereo::Enabled() || fsr41) && vk12_features.shaderStorageImageArrayNonUniformIndexing,
            .descriptorBindingSampledImageUpdateAfterBind = fsr41 && vk12_features.descriptorBindingSampledImageUpdateAfterBind,
            .descriptorBindingStorageImageUpdateAfterBind = fsr41 && vk12_features.descriptorBindingStorageImageUpdateAfterBind,
            .descriptorBindingStorageBufferUpdateAfterBind = fsr41 && vk12_features.descriptorBindingStorageBufferUpdateAfterBind,
            .descriptorBindingUpdateUnusedWhilePending = fsr41 && vk12_features.descriptorBindingUpdateUnusedWhilePending,
            .descriptorBindingPartiallyBound = fsr41 && vk12_features.descriptorBindingPartiallyBound,
            .descriptorBindingVariableDescriptorCount = fsr41 && vk12_features.descriptorBindingVariableDescriptorCount,
            .runtimeDescriptorArray = fsr41 && vk12_features.runtimeDescriptorArray,
            .scalarBlockLayout = vk12_features.scalarBlockLayout,
            .uniformBufferStandardLayout = vk12_features.uniformBufferStandardLayout,
            .separateDepthStencilLayouts = vk12_features.separateDepthStencilLayouts,
            .hostQueryReset = vk12_features.hostQueryReset,
            .timelineSemaphore = vk12_features.timelineSemaphore,
            .bufferDeviceAddress = vk12_features.bufferDeviceAddress,
            .shaderOutputLayer = vk12_features.shaderOutputLayer,
        },
        vk::PhysicalDeviceVulkan13Features{
            .robustImageAccess = vk13_features.robustImageAccess,
            .shaderDemoteToHelperInvocation = vk13_features.shaderDemoteToHelperInvocation,
            .subgroupSizeControl = vk13_features.subgroupSizeControl,
            .computeFullSubgroups = fsr41 && vk13_features.computeFullSubgroups,
            .synchronization2 = vk13_features.synchronization2,
            .dynamicRendering = vk13_features.dynamicRendering,
            .shaderIntegerDotProduct = vk13_features.shaderIntegerDotProduct,
            .maintenance4 = vk13_features.maintenance4,
        },
        vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR{
            .computeDerivativeGroupLinear = derivatives.computeDerivativeGroupLinear,
        },
        fsr41_matrix,
        fsr41_float8,
        fsr41_coherent,
        // Extensions
        vk::PhysicalDeviceCustomBorderColorFeaturesEXT{
            .customBorderColors = true,
            .customBorderColorWithoutFormat = true,
        },
        vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT{
            .extendedDynamicState3ColorWriteMask =
                dynamic_state_3_features.extendedDynamicState3ColorWriteMask,
        },
        vk::PhysicalDeviceDepthClipControlFeaturesEXT{
            .depthClipControl = true,
        },
        vk::PhysicalDeviceDepthClipEnableFeaturesEXT{
            .depthClipEnable = true,
        },
        vk::PhysicalDeviceRobustness2FeaturesEXT{
            .robustBufferAccess2 = true,
            .robustImageAccess2 = true,
            .nullDescriptor = true,
        },
        vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT{
            .vertexInputDynamicState = true,
        },
        vk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT{
            .primitiveTopologyListRestart = list_restart_features.primitiveTopologyListRestart,
            .primitiveTopologyPatchListRestart =
                list_restart_features.primitiveTopologyPatchListRestart,
        },
        vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR{
            .fragmentShaderBarycentric = true,
        },
        vk::PhysicalDeviceProvokingVertexFeaturesEXT{
            .provokingVertexLast = true,
        },
        vk::PhysicalDeviceVertexAttributeDivisorFeatures{
            .vertexAttributeInstanceRateDivisor = true,
        },
        vk::PhysicalDeviceMaintenance8FeaturesKHR{
            .maintenance8 = true,
        },
        vk::PhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT{
            .attachmentFeedbackLoopLayout = true,
        },
        vk::PhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT{
            .attachmentFeedbackLoopDynamicState = true,
        },
        vk::PhysicalDeviceShaderAtomicFloat2FeaturesEXT{
            .shaderBufferFloat32AtomicMinMax =
                shader_atomic_float2_features.shaderBufferFloat32AtomicMinMax,
            .shaderImageFloat32AtomicMinMax =
                shader_atomic_float2_features.shaderImageFloat32AtomicMinMax,
        },
        vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR{
            .workgroupMemoryExplicitLayout =
                workgroup_memory_explicit_layout_features.workgroupMemoryExplicitLayout,
            .workgroupMemoryExplicitLayoutScalarBlockLayout =
                workgroup_memory_explicit_layout_features
                    .workgroupMemoryExplicitLayoutScalarBlockLayout,
            .workgroupMemoryExplicitLayout16BitAccess =
                workgroup_memory_explicit_layout_features.workgroupMemoryExplicitLayout16BitAccess,
        },
        vk::PhysicalDeviceImage2DViewOf3DFeaturesEXT{
            .image2DViewOf3D = image_2d_view_of_3d_features.image2DViewOf3D,
            .sampler2DViewOf3D = image_2d_view_of_3d_features.sampler2DViewOf3D,
        },
        vk::PhysicalDeviceImageViewMinLodFeaturesEXT{
            .minLod = true,
        },
        vk::PhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT{
            .dynamicRenderingUnusedAttachments = true,
        },
        vk::PhysicalDeviceFragmentShadingRateFeaturesKHR{
            // Only the attachment rate: the pipelines keep a 1x1 fragment size (vr_foveation.h).
            .attachmentFragmentShadingRate = true,
        },
        vk::PhysicalDeviceFaultFeaturesEXT{
            .deviceFault = true,
        },
        vk::PhysicalDeviceAddressBindingReportFeaturesEXT{
            .reportAddressBinding = true,
        },
    };

    if (!derivatives.computeDerivativeGroupLinear) {
        device_chain.unlink<vk::PhysicalDeviceComputeShaderDerivativesFeaturesKHR>();
    }
    if (!fsr41_matrix.cooperativeMatrix) {
        device_chain.unlink<vk::PhysicalDeviceCooperativeMatrixFeaturesKHR>();
    }
    if (!fsr41_float8.shaderFloat8) {
        device_chain.unlink<vk::PhysicalDeviceShaderFloat8FeaturesEXT>();
    }
    if (!fsr41_coherent.deviceCoherentMemory) {
        device_chain.unlink<vk::PhysicalDeviceCoherentMemoryFeaturesAMD>();
    }
    if (!custom_border_color) {
        device_chain.unlink<vk::PhysicalDeviceCustomBorderColorFeaturesEXT>();
    }
    if (!dynamic_state_3) {
        device_chain.unlink<vk::PhysicalDeviceExtendedDynamicState3FeaturesEXT>();
    }
    if (!depth_clip_control) {
        device_chain.unlink<vk::PhysicalDeviceDepthClipControlFeaturesEXT>();
    }
    if (!depth_clip_enable) {
        device_chain.unlink<vk::PhysicalDeviceDepthClipEnableFeaturesEXT>();
    }
    if (!vertex_input_dynamic_state) {
        device_chain.unlink<vk::PhysicalDeviceVertexInputDynamicStateFeaturesEXT>();
    }
    if (!list_restart) {
        device_chain.unlink<vk::PhysicalDevicePrimitiveTopologyListRestartFeaturesEXT>();
    }
    if (!fragment_shader_barycentric) {
        device_chain.unlink<vk::PhysicalDeviceFragmentShaderBarycentricFeaturesKHR>();
    }
    if (!provoking_vertex) {
        device_chain.unlink<vk::PhysicalDeviceProvokingVertexFeaturesEXT>();
    }
    if (!maintenance_8) {
        device_chain.unlink<vk::PhysicalDeviceMaintenance8FeaturesKHR>();
    }
    if (!attachment_feedback_loop) {
        device_chain.unlink<vk::PhysicalDeviceAttachmentFeedbackLoopLayoutFeaturesEXT>();
        device_chain.unlink<vk::PhysicalDeviceAttachmentFeedbackLoopDynamicStateFeaturesEXT>();
    }
    if (!shader_atomic_float2) {
        device_chain.unlink<vk::PhysicalDeviceShaderAtomicFloat2FeaturesEXT>();
    }
    if (!workgroup_memory_explicit_layout) {
        device_chain.unlink<vk::PhysicalDeviceWorkgroupMemoryExplicitLayoutFeaturesKHR>();
    }
    if (!image_2d_view_of_3d) {
        device_chain.unlink<vk::PhysicalDeviceImage2DViewOf3DFeaturesEXT>();
    }
    if (!image_view_min_lod) {
        device_chain.unlink<vk::PhysicalDeviceImageViewMinLodFeaturesEXT>();
    }
    if (!dynamic_rendering_unused_attachments) {
        device_chain.unlink<vk::PhysicalDeviceDynamicRenderingUnusedAttachmentsFeaturesEXT>();
    }
    if (!foveation) {
        device_chain.unlink<vk::PhysicalDeviceFragmentShadingRateFeaturesKHR>();
    }
    if (!device_fault) {
        device_chain.unlink<vk::PhysicalDeviceFaultFeaturesEXT>();
    }
    if (!address_report) {
        device_chain.unlink<vk::PhysicalDeviceAddressBindingReportFeaturesEXT>();
    }

    auto [device_result, dev] = physical_device.createDeviceUnique(device_chain.get());
    if (device_result != vk::Result::eSuccess) {
        LOG_CRITICAL(Render_Vulkan, "Failed to create device: {}", vk::to_string(device_result));
        return false;
    }
    device = std::move(dev);

    VULKAN_HPP_DEFAULT_DISPATCHER.init(*device);
    VrDlssRuntime::Get().AttachDevice(*instance, physical_device, *device);

    graphics_queue = device->getQueue(queue_family_index, 0);
    own_present_queue = own_present;
    present_queue = device->getQueue(queue_family_index, own_present ? 1 : 0);
    if (own_present) {
        LOG_INFO(Render_Vulkan,
                 "VR: the desktop present has a queue of its own (queue 1 of family {}): a "
                 "blocked present no longer holds up the game's submits or the headset's frames "
                 "(priority {})",
                 queue_family_index, queue_priorities[1]);
    } else if (own_present_off && OpenXRPresenter::IsRequested()) {
        LOG_INFO(Render_Vulkan, "VR: the desktop present shares the graphics queue "
                                "(VR_PRESENT_OWN_QUEUE=0)");
    }
    own_xr_queue = own_xr.own;
    xr_queue_index = own_xr.own ? queues_before : 0u;
    xr_queue = device->getQueue(queue_family_index, xr_queue_index);
    sparse_queue = own_sparse_queue ? device->getQueue(queue_family_index, queues_with_xr)
                                    : graphics_queue;
    {
        const std::array<GpuCrash::NamedQueue, 4> queues{{
            {graphics_queue, "graphics (game)"},
            {present_queue, "desktop present"},
            {xr_queue, "OpenXR runtime"},
            {sparse_queue, "sparse binds"},
        }};
        GpuCrash::Init(physical_device, *device, device_fault, gpu_checkpoints,
                       supports_memory_budget, queues);
        GpuCrash::SetAddressReport(address_report);
    }
    if (sparse_buffers) {
        LOG_INFO(Render_Vulkan, "Buffer arenas: sparse binds on {}", own_sparse_queue
                 ? fmt::format("a queue of their own (index {})", queues_with_xr)
                 : std::string{"the graphics queue"});
    }
    if (OpenXRPresenter::IsRequested()) {
        // Once: which queue the OpenXR runtime submits its work on (OpenXRPresenter's binding).
        LOG_INFO(Render_Vulkan,
                 "VR: the OpenXR runtime ({}) {} (queue {} of family {}, {} queues there): {}",
                 OpenXRPresenter::RuntimeName(),
                 own_xr.own ? "has a queue of its own: its xrEndFrame and swapchain calls no "
                              "longer hold the game's submits"
                            : "shares the graphics queue, as before",
                 xr_queue_index, queue_family_index,
                 family_properties[queue_family_index].queueCount, own_xr.reason);
    }

    if (calibrated_timestamps) {
        const auto [time_domains_result, time_domains] =
            physical_device.getCalibrateableTimeDomainsEXT();
        if (time_domains_result == vk::Result::eSuccess) {
#if _WIN64
            const bool has_host_time_domain =
                std::find(time_domains.cbegin(), time_domains.cend(),
                          vk::TimeDomainEXT::eQueryPerformanceCounter) != time_domains.cend();
#elif __linux__
            const bool has_host_time_domain =
                std::find(time_domains.cbegin(), time_domains.cend(),
                          vk::TimeDomainEXT::eClockMonotonicRaw) != time_domains.cend();
#else
            // Tracy limitation means only Windows and Linux can use host time domain.
            // https://github.com/shadps4-emu/tracy/blob/c6d779d78508514102fbe1b8eb28bda10d95bb2a/public/tracy/TracyVulkan.hpp#L384-L389
            const bool has_host_time_domain = false;
#endif
            if (has_host_time_domain) {
                static constexpr std::string_view context_name{"vk_rasterizer"};
                profiler_context = TracyVkContextHostCalibrated(
                    *instance, physical_device, *device,
                    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
                    VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr);
                TracyVkContextName(profiler_context, context_name.data(), context_name.size());
            }
        } else {
            LOG_WARNING(Render_Vulkan, "Could not query calibrated time domains for profiling: {}",
                        vk::to_string(time_domains_result));
        }
    }

    CreateAllocator();
    return true;
}

void Instance::CreateAllocator() {
    const VmaVulkanFunctions functions = {
        .vkGetInstanceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
        .vkGetDeviceProcAddr = VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr,
    };

    // Experiment (2026-10-18, bbport uses 64): VR_VMA_BLOCK_MB=N sets VMA's large-heap block size
    // (default 0: VMA's own, 256 MiB). A block goes back to the driver only once empty.
    static const VkDeviceSize block_size = [] {
        const char* env = std::getenv("VR_VMA_BLOCK_MB");
        const unsigned long mb = env ? std::strtoul(env, nullptr, 10) : 0;
        return mb >= 8 && mb <= 1024 ? VkDeviceSize{mb} << 20 : VkDeviceSize{0};
    }();
    if (block_size != 0) {
        LOG_INFO(Render_Vulkan, "VMA large-heap block size {} MiB (VR_VMA_BLOCK_MB)",
                 block_size >> 20);
    }
    const VmaAllocatorCreateInfo allocator_info = {
        .flags = VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT,
        .physicalDevice = physical_device,
        .device = *device,
        .preferredLargeHeapBlockSize = block_size,
        .pVulkanFunctions = &functions,
        .instance = *instance,
        .vulkanApiVersion = TargetVulkanApiVersion,
    };

    const VkResult result = vmaCreateAllocator(&allocator_info, &allocator);
    if (result != VK_SUCCESS) {
        UNREACHABLE_MSG("Failed to initialize VMA with error {}",
                        vk::to_string(vk::Result{result}));
    }
}

void Instance::CollectDeviceParameters() {
    const vk::StructureChain property_chain =
        physical_device
            .getProperties2<vk::PhysicalDeviceProperties2, vk::PhysicalDeviceDriverProperties>();
    const vk::PhysicalDeviceDriverProperties driver =
        property_chain.get<vk::PhysicalDeviceDriverProperties>();

    driver_id = driver.driverID;
    vendor_name = driver.driverName.data();

    const std::string model_name{GetModelName()};
    const std::string driver_version = GetDriverVersionName();
    const std::string driver_name = fmt::format("{} {}", vendor_name, driver_version);
    const std::string api_version = GetReadableVersion(properties.apiVersion);
    const std::string extensions = fmt::format("{}", fmt::join(available_extensions, ", "));

    LOG_INFO(Render_Vulkan, "GPU_Vendor: {}", vendor_name);
    LOG_BENCHMARK_INFO(Render_Vulkan, "GPU_Model: {}", model_name);
    LOG_INFO(Render_Vulkan, "GPU_Integrated: {}", IsIntegrated() ? "Yes" : "No");
    LOG_INFO(Render_Vulkan, "GPU_Vulkan_Driver: {}", driver_name);
    LOG_INFO(Render_Vulkan, "GPU_Vulkan_Version: {}", api_version);
    LOG_INFO(Render_Vulkan, "GPU_Vulkan_Extensions: {}", extensions);
}

void Instance::CollectPhysicalMemoryInfo() {
    vk::PhysicalDeviceMemoryBudgetPropertiesEXT budget{};
    vk::PhysicalDeviceMemoryProperties2 props = {
        .pNext = supports_memory_budget ? &budget : nullptr,
    };
    physical_device.getMemoryProperties2(&props);
    const auto& memory_props = props.memoryProperties;
    const size_t num_props = memory_props.memoryHeapCount;
    total_memory_budget = 0;
    u64 device_initial_usage = 0;
    u64 local_memory = 0;
    for (size_t i = 0; i < num_props; ++i) {
        const bool is_device_local =
            (memory_props.memoryHeaps[i].flags & vk::MemoryHeapFlagBits::eDeviceLocal) !=
            vk::MemoryHeapFlags{};
        if (!IsIntegrated() && !is_device_local) {
            // Ignore non-device local memory on discrete GPUs.
            continue;
        }
        valid_heaps.push_back(i);
        if (is_device_local) {
            local_memory += memory_props.memoryHeaps[i].size;
        }
        if (supports_memory_budget) {
            device_initial_usage += budget.heapUsage[i];
            total_memory_budget += budget.heapBudget[i];
            continue;
        }
        // If memory budget is not supported, use the size of the heap as the budget.
        total_memory_budget += memory_props.memoryHeaps[i].size;
    }
    device_local_cap = local_memory - std::min<u64>(local_memory / 8, 1_GB);
    if (!IsIntegrated()) {
        // We reserve some memory for the system.
        const u64 system_memory = std::min<u64>(total_memory_budget / 8, 1_GB);
        total_memory_budget -= system_memory;
        if (const u64 cap = TestBudgetCap(); cap != 0 && cap < total_memory_budget) {
            LOG_WARNING(Render_Vulkan,
                        "VR_VRAM_BUDGET_MB: a memory budget of {} MB for this test (the card "
                        "gives {} MB)",
                        cap >> 20, total_memory_budget >> 20);
            total_memory_budget = cap;
        }
        return;
    }
    // Leave at least 8 GB for the system on integrated GPUs.
    const s64 available_memory = static_cast<s64>(total_memory_budget - device_initial_usage);
    total_memory_budget =
        static_cast<u64>(std::max<s64>(available_memory - 8_GB, static_cast<s64>(local_memory)));
}

void Instance::CollectImageFormatInfo() {
    // Check for block texel view support using basic image format info.
    const vk::PhysicalDeviceImageFormatInfo2 block_texel_view_info{
        .format = vk::Format::eBc1RgbaUnormBlock,
        .type = vk::ImageType::e2D,
        .tiling = vk::ImageTiling::eOptimal,
        .usage = vk::ImageUsageFlagBits::eSampled,
        .flags = vk::ImageCreateFlagBits::eBlockTexelViewCompatible,
    };
    const auto block_texel_view_props =
        physical_device.getImageFormatProperties2(block_texel_view_info);
    supports_block_texel_view = block_texel_view_props.result == vk::Result::eSuccess;
    LOG_INFO(Render_Vulkan, "Block Texel View support: {}",
             supports_block_texel_view ? "Yes" : "No");

    // Check and log format support details.
    for (const auto& format : LiverpoolToVK::SurfaceFormats()) {
        if (!IsFormatSupported(format.vk_format, format.flags)) {
            LOG_WARNING(Render_Vulkan,
                        "Surface format data_format={}, number_format={} is not fully supported "
                        "(vk_format={}, missing features={})",
                        static_cast<u32>(format.data_format),
                        static_cast<u32>(format.number_format), vk::to_string(format.vk_format),
                        vk::to_string(format.flags & ~GetFormatFeatureFlags(format.vk_format)));
        }
    }
    for (const auto& format : LiverpoolToVK::DepthFormats()) {
        if (!IsFormatSupported(format.vk_format, format.flags)) {
            LOG_WARNING(Render_Vulkan,
                        "Depth format z_format={}, stencil_format={} is not fully supported "
                        "(vk_format={}, missing features={})",
                        static_cast<u32>(format.z_format), static_cast<u32>(format.stencil_format),
                        vk::to_string(format.vk_format),
                        vk::to_string(format.flags & ~GetFormatFeatureFlags(format.vk_format)));
        }
    }
}

void Instance::CollectToolingInfo() const {
    if (driver_id == vk::DriverId::eAmdProprietary ||
        driver_id == vk::DriverId::eIntelProprietaryWindows) {
        // AMD: Causes issues with Reshade.
        // Intel: Causes crash on start.
        return;
    }
    const auto [tools_result, tools] = physical_device.getToolProperties();
    if (tools_result != vk::Result::eSuccess) {
        LOG_ERROR(Render_Vulkan, "Could not get Vulkan tool properties: {}",
                  vk::to_string(tools_result));
        return;
    }
    for (const vk::PhysicalDeviceToolProperties& tool : tools) {
        const std::string_view name = tool.name;
        LOG_INFO(Render_Vulkan, "Attached debugging tool: {}", name);
    }
}

Instance::MemoryBudgetNow Instance::QueryMemoryBudget() const {
    if (!supports_memory_budget || IsIntegrated()) {
        return {};
    }
    vk::PhysicalDeviceMemoryBudgetPropertiesEXT memory_budget_props{};
    vk::PhysicalDeviceMemoryProperties2 props = {
        .pNext = &memory_budget_props,
    };
    physical_device.getMemoryProperties2(&props);
    MemoryBudgetNow now;
    for (const size_t heap : valid_heaps) {
        now.budget += memory_budget_props.heapBudget[heap];
        now.usage += memory_budget_props.heapUsage[heap];
    }
    // Windows' counters (Residency::Reader): memory it moved to system memory still counts in
    // Vulkan's use but is not resident, and adds to the system-memory use beyond what the game
    // allocated there itself. Both must show it: memory allocated at a load counts in Vulkan before
    // its first use makes it resident (1.7 GB "missing" at a load, 2026-11-05), with no system
    // memory growth.
    const auto& heaps = props.memoryProperties;
    for (u32 heap = 0; heap < heaps.memoryHeapCount; ++heap) {
        if (!(heaps.memoryHeaps[heap].flags & vk::MemoryHeapFlagBits::eDeviceLocal)) {
            now.host += memory_budget_props.heapUsage[heap];
        }
    }
    now.resident = now.usage;
    if (const auto windows = residency ? residency->Read() : std::nullopt) {
        now.resident = std::min<u64>(now.usage, windows->local);
        now.non_local = windows->non_local;
        // FAST_SPILL_WATCH=0 (A/B tests only): the counters are still read and logged, but no
        // reading reaches the spill watch, so the marks follow Windows' budget alone.
        static const bool spill_watch = [] {
            const char* value = std::getenv("FAST_SPILL_WATCH");
            return value == nullptr || value[0] != '0';
        }();
        now.residency_sample = spill_watch ? windows->sample : 0;
        const u64 missing = now.usage - now.resident;
        const u64 extra = windows->non_local > now.host ? windows->non_local - now.host : 0;
        now.spilled = std::min(missing, extra);
    }
    // As at the start (CollectPhysicalMemoryInfo): the system's share off, a test's cap on.
    now.budget -= std::min<u64>(now.budget / 8, 1_GB);
    if (const u64 cap = TestBudgetCap(); cap != 0) {
        now.budget = std::min(now.budget, cap);
    }
    return now;
}

u64 Instance::GetDeviceMemoryUsage() const {
    vk::PhysicalDeviceMemoryBudgetPropertiesEXT memory_budget_props{};
    vk::PhysicalDeviceMemoryProperties2 props = {
        .pNext = &memory_budget_props,
    };
    physical_device.getMemoryProperties2(&props);

    u64 total_usage = 0;
    for (const size_t heap : valid_heaps) {
        total_usage += memory_budget_props.heapUsage[heap];
    }
    return total_usage;
}

vk::FormatFeatureFlags2 Instance::GetFormatFeatureFlags(vk::Format format) const {
    const auto it = format_properties.find(format);
    if (it == format_properties.end()) {
        UNIMPLEMENTED_MSG("Properties of format {} have not been queried.", vk::to_string(format));
    }

    return it->second.optimalTilingFeatures | it->second.bufferFeatures;
}

bool Instance::IsFormatSupported(const vk::Format format,
                                 const vk::FormatFeatureFlags2 flags) const {
    if (format == vk::Format::eUndefined) [[unlikely]] {
        return true;
    }
    return (GetFormatFeatureFlags(format) & flags) == flags;
}

vk::Format Instance::GetSupportedFormat(const vk::Format format,
                                        const vk::FormatFeatureFlags2 flags) const {
    if (!IsFormatSupported(format, flags)) [[unlikely]] {
        switch (format) {
        case vk::Format::eD16UnormS8Uint:
            if (IsFormatSupported(vk::Format::eD24UnormS8Uint, flags)) {
                return vk::Format::eD24UnormS8Uint;
            }
            if (IsFormatSupported(vk::Format::eD32SfloatS8Uint, flags)) {
                return vk::Format::eD32SfloatS8Uint;
            }
            break;
        case vk::Format::eR8Srgb:
            if (IsFormatSupported(vk::Format::eR8Unorm, flags)) {
                return vk::Format::eR8Unorm;
            }
            break;
        default:
            break;
        }
    }
    return format;
}

} // namespace Vulkan
