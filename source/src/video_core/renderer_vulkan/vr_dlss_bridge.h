// SPDX-FileCopyrightText: Copyright 2026 IFreemz (shadps4_dlss bridge), bbport contributors
// SPDX-License-Identifier: MIT
//
// C interface between bbport and the DLSS bridge (libbbport_dlss.so / bbport_dlss.dll). The port
// contains no NVIDIA code: it loads the bridge at run time when it is present (next to bb-probe,
// with NVIDIA's libnvidia-ngx-dlss.so.<version> / nvngx_dlss.dll) and keeps
// its other upscalers when it is absent. Adapted from IFreemz/shadPS4-Bloodborne-DLSS-FSR.

#pragma once

#include <stdint.h>
#include <vulkan/vulkan.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BBPORT_DLSS_BRIDGE_ABI 2
// Staged stereo ABI: view is 0 (left) or 1 (right); each owns separate NGX state. Bridges built
// from this tree also accept view 2, a diagnostic twin (FAST_DLSS_DIRECT_VERIFY); older ones
// refuse it from CreateFeature without recording anything.
// Calls are serialized by the future render owner; wait for GPU use before release.

typedef void (*BbDlssLogFn)(int warning, const char* message);

typedef struct BbDlssImage {
    VkImage image;
    VkImageView view;
    VkImageSubresourceRange range;
    VkFormat format;
    uint32_t width;
    uint32_t height;
} BbDlssImage;

typedef struct BbDlssFeature {
    uint32_t input_width;
    uint32_t input_height;
    uint32_t output_width;
    uint32_t output_height;
    int32_t quality; // 0 DLAA, 1 Quality, 2 Balanced, 3 Performance, 4 Ultra Performance
    int32_t depth_inverted;
    uint32_t preset; // NVSDK_NGX_DLSS_Hint_Render_Preset value, 0 = driver default
    int32_t hdr;     // 1: linear HDR colour with automatic exposure, 0: tonemapped colour
} BbDlssFeature;

typedef struct BbDlssEvaluate {
    BbDlssImage color;  // sampled
    BbDlssImage depth;  // sampled, hardware depth
    BbDlssImage motion; // sampled, render-resolution pixels, current to previous
    BbDlssImage output; // storage, general layout
    float jitter_x;
    float jitter_y;
    int32_t reset;
    float frame_ms;
    float sharpness; // 0..1; 0 leaves the output unsharpened
} BbDlssEvaluate;

typedef struct BbDlssApi {
    uint32_t abi;
    // wchar_t paths (NGX's type): the directory with NVIDIA's DLSS library, and a writable data
    // directory.
    int32_t (*Configure)(const wchar_t* dll_directory, const wchar_t* data_directory,
                         BbDlssLogFn log);
    // Vulkan extensions NGX needs. The arrays stay valid until Shutdown.
    int32_t (*InstanceExtensions)(uint32_t* count, const VkExtensionProperties** extensions);
    int32_t (*DeviceExtensions)(VkInstance instance, VkPhysicalDevice physical, uint32_t* count,
                                const VkExtensionProperties** extensions);
    int32_t (*Initialize)(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                          PFN_vkGetInstanceProcAddr get_instance_proc,
                          PFN_vkGetDeviceProcAddr get_device_proc);
    int32_t (*CreateFeature)(uint32_t view, VkCommandBuffer command, const BbDlssFeature* feature);
    int32_t (*Evaluate)(uint32_t view, VkCommandBuffer command, const BbDlssEvaluate* evaluate);
    // The caller must have waited for the GPU work that used the feature.
    void (*ReleaseFeature)(uint32_t view);
    void (*Shutdown)(void);
} BbDlssApi;

typedef const BbDlssApi* (*BbDlssGetApiFn)(void);
// Load VrDlssGetStereoApi from vr_dlss_stereo.dll; never load this as the mono ABI.

// Optional export VrDlssSetSrPreset (no ABI bump; older bridges lack it and keep their model):
// the Super Resolution model of every feature created after the call. 0 restores the built-in
// choice (VR_DLSS_SR_PRESET at the bridge's build, else BbDlssFeature.preset),
// VR_DLSS_SR_PRESET_AUTO asks NGX for its per-mode default, and 5..15 are
// NVSDK_NGX_DLSS_Hint_Render_Preset values (E, F, J, K, L, M are accepted). Returns the hint the
// next CreateFeature passes to NGX for a host preset of 0, or -1 (refused; the built-in choice
// is restored). Serialized with the other calls, like CreateFeature.
#define VR_DLSS_SR_PRESET_AUTO 0x100u
typedef int32_t (*VrDlssSetSrPresetFn)(uint32_t selection);

#ifdef __cplusplus
}
#endif
