// SPDX-FileCopyrightText: Copyright 2026 IFreemz (shadps4_dlss bridge), bbport contributors
// SPDX-License-Identifier: MIT
//
// libbbport_dlss.so / bbport_dlss.dll: the only part of the project that uses the NVIDIA DLSS
// (NGX) SDK; the port loads it at run time. Linux: build.sh with DLSS_SDK_ROOT; Windows: MSVC
// (the SDK's Windows libraries are MSVC only), see CMakeLists.txt.
// Adapted from IFreemz/shadPS4-Bloodborne-DLSS-FSR (dlss_bridge).

#include <algorithm>
#include <array>
#include <cstdio>
#include <cmath>
#include <string>

#include "bbport_dlss_bridge.h" // Vulkan first: the NGX headers expect its types

#include <nvsdk_ngx_helpers.h>
#include <nvsdk_ngx_vk.h>
#include <nvsdk_ngx_helpers_vk.h>
#ifdef VR_DLSSNR_CHAIN
#include "../dlss5_bridge/chain.h"
#endif

namespace {

#ifndef VR_DLSS_PROJECT_ID
#error "Supply VR_DLSS_PROJECT_ID for your NGX project when building the standalone bridge"
#endif
constexpr char ProjectId[] = VR_DLSS_PROJECT_ID;
#ifdef _WIN32
constexpr char EngineVersion[] = "bloodborne-vr-staged-windows";
#else
constexpr char EngineVersion[] = "bloodborne-vr-staged-linux";
#endif

struct ViewState {
    NVSDK_NGX_Parameter* parameters{};
    NVSDK_NGX_Handle* feature{};
    BbDlssFeature feature_desc{};
    bool ready{}; // Partial creation stays owned until explicit host retirement.
};

struct State {
    BbDlssLogFn log{};
    uint32_t sr_preset{}; // VrDlssSetSrPreset: 0 built-in, VR_DLSS_SR_PRESET_AUTO or a hint
    std::wstring dll_directory;
    std::wstring data_directory;
    const wchar_t* dll_path{};
    NVSDK_NGX_FeatureCommonInfo common{};
    NVSDK_NGX_FeatureDiscoveryInfo discovery{};
    NVSDK_NGX_Parameter* capabilities{};
    // Views 0 and 1 are the eyes. View 2 is the host's FAST_DLSS_DIRECT_VERIFY twin (a third
    // feature with its own parameters, never DLSS 5); hosts check that it is accepted.
    std::array<ViewState, 3> views{};
    VkDevice device{};
    bool configured{};
    bool initialized{};
} state;

template <typename... Args>
void Log(int warning, const char* format, Args... args) {
    if (!state.log)
        return;
    if constexpr (sizeof...(Args) == 0) {
        state.log(warning, format); // a plain message (no format string without arguments)
    } else {
        std::array<char, 1024> text{};
        std::snprintf(text.data(), text.size(), format, args...);
        state.log(warning, text.data());
    }
}

bool Check(const char* operation, NVSDK_NGX_Result result) {
    if (NVSDK_NGX_FAILED(result)) {
        Log(1, "%s failed: 0x%08x", operation, static_cast<unsigned>(result));
        return false;
    }
    return true;
}

void NVSDK_CONV NgxLog(const char* message, NVSDK_NGX_Logging_Level, NVSDK_NGX_Feature) {
    Log(0, "NGX: %s", message ? message : "");
}

int32_t Configure(const wchar_t* dll_directory, const wchar_t* data_directory, BbDlssLogFn log) {
    if (state.initialized) return 0; // Paths must stay alive until NGX shuts down.
    state.log = log;
    state.dll_directory = dll_directory ? dll_directory : L".";
    state.data_directory = data_directory ? data_directory : L".";
    state.dll_path = state.dll_directory.c_str();
    state.common.PathListInfo = {&state.dll_path, 1};
    state.common.LoggingInfo = {NgxLog, NVSDK_NGX_LOGGING_LEVEL_OFF, false};
    state.discovery.SDKVersion = NVSDK_NGX_Version_API;
    state.discovery.FeatureID = NVSDK_NGX_Feature_SuperSampling;
    state.discovery.Identifier.IdentifierType = NVSDK_NGX_Application_Identifier_Type_Project_Id;
    state.discovery.Identifier.v.ProjectDesc = {ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM,
                                                EngineVersion};
    state.discovery.ApplicationDataPath = state.data_directory.c_str();
    state.discovery.FeatureInfo = &state.common;
    state.configured = true;
    return 1;
}

int32_t InstanceExtensions(uint32_t* count, const VkExtensionProperties** extensions) {
    VkExtensionProperties* required{};
    if (!state.configured || !count || !extensions ||
        !Check("Instance extension query",
               NVSDK_NGX_VULKAN_GetFeatureInstanceExtensionRequirements(&state.discovery, count,
                                                                       &required)))
        return 0;
    *extensions = required;
    return 1;
}

int32_t DeviceExtensions(VkInstance instance, VkPhysicalDevice physical, uint32_t* count,
                         const VkExtensionProperties** extensions) {
    if (!state.configured || !count || !extensions)
        return 0;
    NVSDK_NGX_FeatureRequirement support{};
    if (!Check("Feature requirement query",
               NVSDK_NGX_VULKAN_GetFeatureRequirements(instance, physical, &state.discovery,
                                                       &support)))
        return 0;
    if (support.FeatureSupported != NVSDK_NGX_FeatureSupportResult_Supported) {
        Log(1, "DLSS is not supported on this GPU or driver (code 0x%x)",
            static_cast<unsigned>(support.FeatureSupported));
        return 0;
    }
    VkExtensionProperties* required{};
    if (!Check("Device extension query",
               NVSDK_NGX_VULKAN_GetFeatureDeviceExtensionRequirements(
                   instance, physical, &state.discovery, count, &required)))
        return 0;
    *extensions = required;
    return 1;
}

void Shutdown();

int32_t Initialize(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                   PFN_vkGetInstanceProcAddr get_instance_proc,
                   PFN_vkGetDeviceProcAddr get_device_proc) {
    if (!state.configured || state.initialized || !instance || !physical || !device)
        return 0;
    if (!Check("NGX initialization",
               NVSDK_NGX_VULKAN_Init_with_ProjectID(
                   ProjectId, NVSDK_NGX_ENGINE_TYPE_CUSTOM, EngineVersion,
                   state.data_directory.c_str(), instance, physical, device, get_instance_proc,
                   get_device_proc, &state.common)))
        return 0;
    state.initialized = true;
    state.device = device;
#ifdef VR_DLSSNR_CHAIN
    VrNeural::Attach(instance, physical, device, state.dll_directory.c_str(),
                     state.data_directory.c_str(), state.log);
#endif
    if (!Check("Capability query", NVSDK_NGX_VULKAN_GetCapabilityParameters(&state.capabilities)) ||
        !state.capabilities) {
        Shutdown();
        return 0;
    }
    int available{}, needs_driver{};
    NVSDK_NGX_Parameter_GetI(state.capabilities, NVSDK_NGX_Parameter_SuperSampling_Available,
                             &available);
    NVSDK_NGX_Parameter_GetI(state.capabilities,
                             NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needs_driver);
    if (needs_driver) {
        Log(1, "DLSS needs a newer NVIDIA driver");
        Shutdown();
        return 0;
    }
    if (!available) {
        Log(1, "DLSS Super Resolution is not available on this system");
        Shutdown();
        return 0;
    }
    return 1;
}

void ReleaseFeature(uint32_t view) {
    if (view >= state.views.size()) return;
    auto& eye = state.views[view];
#ifdef VR_DLSSNR_CHAIN
    if (view < 2) // the neural chain has the two eyes only
        VrNeural::Release(view);
#endif
    if (eye.feature) {
        NVSDK_NGX_VULKAN_ReleaseFeature(eye.feature);
        eye.feature = nullptr;
    }
    if (eye.parameters) {
        NVSDK_NGX_VULKAN_DestroyParameters(eye.parameters);
        eye.parameters = nullptr;
    }
    eye.feature_desc = {};
    eye.ready = false;
}

int32_t CreateFeature(uint32_t view, VkCommandBuffer command, const BbDlssFeature* desc) {
    if (view >= state.views.size()) return 0;
    auto& eye = state.views[view];
    if (!state.initialized || !state.capabilities || !command || !desc ||
        !desc->input_width || !desc->input_height || !desc->output_width || !desc->output_height ||
        desc->input_width > desc->output_width || desc->input_height > desc->output_height ||
        desc->quality < 0 || desc->quality > 4)
        return 0;
    // A failed creation may have recorded uploads. Only explicit ReleaseFeature after
    // host completion may discard that state; a retry must not release it implicitly.
    if (eye.parameters && !eye.ready) return 0;
    ReleaseFeature(view);
    if (!Check("Parameter allocation", NVSDK_NGX_VULKAN_AllocateParameters(&eye.parameters)) ||
        !eye.parameters) {
        ReleaseFeature(view);
        return 0;
    }
    unsigned preset = desc->preset;
#ifdef VR_DLSS_SR_PRESET
    if (VR_DLSS_SR_PRESET != 0) preset = VR_DLSS_SR_PRESET;
#endif
    // The host's model (VrDlssSetSrPreset) replaces the built-in choice above.
    if (state.sr_preset)
        preset = state.sr_preset == VR_DLSS_SR_PRESET_AUTO ? 0u : state.sr_preset;
    for (const char* key : {NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                            NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality})
        NVSDK_NGX_Parameter_SetUI(eye.parameters, key, preset);
    static constexpr std::array qualities{
        NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality,
        NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf,
        NVSDK_NGX_PerfQuality_Value_UltraPerformance};
    NVSDK_NGX_DLSS_Create_Params settings{};
    settings.Feature.InWidth = desc->input_width;
    settings.Feature.InHeight = desc->input_height;
    settings.Feature.InTargetWidth = desc->output_width;
    settings.Feature.InTargetHeight = desc->output_height;
    settings.Feature.InPerfQualityValue = qualities[desc->quality];
    settings.InFeatureCreateFlags =
        NVSDK_NGX_DLSS_Feature_Flags_MVLowRes |
        (desc->depth_inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0) |
        (desc->hdr ? NVSDK_NGX_DLSS_Feature_Flags_IsHDR | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure
                   : 0);
    const auto result = NGX_VULKAN_CREATE_DLSS_EXT1(state.device, command, 1, 1, &eye.feature,
                                                   eye.parameters, &settings);
    if (!Check("DLSS feature creation", result) || !eye.feature) {
        // The host submits and waits even after failure. Keep any partial feature
        // and its parameters alive until its completion-gated ReleaseFeature call.
        return 0;
    }
    eye.feature_desc = *desc;
#ifdef VR_DLSSNR_CHAIN
    // Retire any recorded uploads first.
    if (view < 2 && !VrNeural::Create(view, command, *desc)) return 0;
#endif
    eye.ready = true;
    Log(0, "DLSS %ux%u -> %ux%u, quality %d, preset %u%s", desc->input_width, desc->input_height,
        desc->output_width, desc->output_height, desc->quality, preset,
        desc->hdr ? ", HDR input" : "");
    return 1;
}

NVSDK_NGX_Resource_VK Wrap(const BbDlssImage& image, bool writable) {
    return NVSDK_NGX_Create_ImageView_Resource_VK(image.view, image.image, image.range,
                                                  image.format, image.width, image.height,
                                                  writable);
}

bool ValidImage(const BbDlssImage& image, uint32_t width, uint32_t height) {
    return image.image && image.view && image.format != VK_FORMAT_UNDEFINED &&
           image.width >= width && image.height >= height && image.range.levelCount == 1 &&
           image.range.layerCount == 1 && image.range.aspectMask != 0;
}

int32_t Evaluate(uint32_t view, VkCommandBuffer command, const BbDlssEvaluate* evaluate) {
    if (view >= state.views.size()) return 0;
    auto& eye = state.views[view];
    if (!eye.ready || !eye.feature || !eye.parameters || !command || !evaluate)
        return 0;
    const auto& desc = eye.feature_desc;
    if (!ValidImage(evaluate->color, desc.input_width, desc.input_height) ||
        !ValidImage(evaluate->depth, desc.input_width, desc.input_height) ||
        !ValidImage(evaluate->motion, desc.input_width, desc.input_height) ||
        !ValidImage(evaluate->output, desc.output_width, desc.output_height) ||
        !std::isfinite(evaluate->jitter_x) || !std::isfinite(evaluate->jitter_y) ||
        !std::isfinite(evaluate->frame_ms) || evaluate->frame_ms < 0 ||
        !std::isfinite(evaluate->sharpness) || evaluate->sharpness < 0 || evaluate->sharpness > 1)
        return 0;
    auto color = Wrap(evaluate->color, false);
    auto depth = Wrap(evaluate->depth, false);
    auto motion = Wrap(evaluate->motion, false);
#ifdef VR_DLSSNR_CHAIN
    auto output = Wrap(view < 2 ? VrNeural::Output(view, command, evaluate->output)
                                : evaluate->output, true);
#else
    auto output = Wrap(evaluate->output, true);
#endif
    NVSDK_NGX_VK_DLSS_Eval_Params parameters{};
    parameters.Feature.pInColor = &color;
    parameters.Feature.pInOutput = &output;
    parameters.Feature.InSharpness = evaluate->sharpness;
    parameters.pInDepth = &depth;
    parameters.pInMotionVectors = &motion;
    parameters.InRenderSubrectDimensions = {eye.feature_desc.input_width,
                                            eye.feature_desc.input_height};
    parameters.InJitterOffsetX = evaluate->jitter_x;
    parameters.InJitterOffsetY = evaluate->jitter_y;
    parameters.InReset = evaluate->reset;
    parameters.InMVScaleX = parameters.InMVScaleY = 1.0f;
    parameters.InPreExposure = parameters.InExposureScale = 1.0f;
    parameters.InFrameTimeDeltaInMsec = evaluate->frame_ms;
    if (!Check("DLSS evaluation", NGX_VULKAN_EVALUATE_DLSS_EXT(command, eye.feature,
                                                              eye.parameters, &parameters))) return 0;
#ifdef VR_DLSSNR_CHAIN
    if (view < 2 && !VrNeural::Evaluate(view, command, *evaluate)) return 0;
#endif
    return 1;
}

void Shutdown() {
    for (uint32_t view=0; view<state.views.size(); ++view) ReleaseFeature(view);
    if (state.capabilities) {
        NVSDK_NGX_VULKAN_DestroyParameters(state.capabilities);
        state.capabilities = nullptr;
    }
    if (state.initialized) {
#ifdef VR_DLSSNR_CHAIN
        VrNeural::Shutdown();
#endif
        NVSDK_NGX_VULKAN_Shutdown1(state.device);
        state.initialized = false;
        state.device = VK_NULL_HANDLE;
    }
}

const BbDlssApi api{BBPORT_DLSS_BRIDGE_ABI, Configure,      InstanceExtensions, DeviceExtensions,
                    Initialize,             CreateFeature,  Evaluate,           ReleaseFeature,
                    Shutdown};

} // namespace

#ifdef _WIN32
#define BBPORT_DLSS_EXPORT __declspec(dllexport)
#else
#define BBPORT_DLSS_EXPORT __attribute__((visibility("default")))
#endif

extern "C" BBPORT_DLSS_EXPORT const BbDlssApi* VrDlssGetStereoApi() {
    return &api;
}

// Optional host model selection (bbport_dlss_bridge.h). Hosts without a model setting never call
// it, and then nothing changes. Accepted: 0, AUTO, and the SDK's live presets E, F (5, 6: CNN,
// deprecated), J, K (10, 11: transformer), L, M (12, 13: 2nd-generation transformer); removed
// (A-D) and "do not use" values are refused.
extern "C" BBPORT_DLSS_EXPORT int32_t VrDlssSetSrPreset(uint32_t selection) {
    const bool valid = selection == 0 || selection == VR_DLSS_SR_PRESET_AUTO || selection == 5 ||
                       selection == 6 || (selection >= 10 && selection <= 13);
    state.sr_preset = valid ? selection : 0;
    if (!valid) {
        Log(1, "DLSS model %u refused; the built-in model stays", selection);
        return -1;
    }
    if (selection)
        return selection == VR_DLSS_SR_PRESET_AUTO ? 0 : int32_t(selection);
#ifdef VR_DLSS_SR_PRESET
    if (VR_DLSS_SR_PRESET != 0) return VR_DLSS_SR_PRESET;
#endif
    return 0; // the host's BbDlssFeature.preset, 0 from this project's hosts
}
