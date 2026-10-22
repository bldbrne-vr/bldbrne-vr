// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vr_dlss_session.h"

namespace Vulkan {
// The bridge/NGX API is process-wide. Vulkan setup and the renderer owner serialize
// these calls. No file lookup, DLL load or NGX initialization occurs per draw.
class VrDlssRuntime {
public:
    static VrDlssRuntime& Get();
    // Windows NVX binary import reserves host VA before the guest claims its ranges.
    // Does nothing unless DLSS is already accepted; no features or GPU work are created.
    void PrepareDriverAddressSpace();
    void PrepareInstance(std::vector<const char*>& enabled, bool requested);
    void PrepareDevice(vk::Instance instance, vk::PhysicalDevice physical,
                       std::vector<const char*>& enabled, bool requested);
    void AttachDevice(vk::Instance instance, vk::PhysicalDevice physical, vk::Device device);
    // Provider initialization remains lazy; called ONLY for an accepted DLSS request.
    // `sr_preset`: the Super Resolution model for the features created next (vr_dlss_preset.h;
    // 0 = the bridge's built-in model).
    VrDlssSession* Activate(const VR::Graphics::Config& accepted, uint32_t sr_preset = 0);
    const std::string& Problem() const {
        return problem;
    }
    // Caller retires scheduler work before tearing down the Vulkan device.
    bool NeedsShutdown() const {
        return session && session->Initialized();
    }
    bool Shutdown(uint64_t completed);

private:
    void ReleaseDriverReservation();
    bool Load();
    bool CheckRequirements(bool append, std::vector<const char*>* instance_enabled = nullptr,
                           std::vector<const char*>* device_enabled = nullptr);
    void Fail(const char* reason);
    static void Log(int warning, const char* message);
    VkInstance early_instance{};
    std::vector<VkDevice> early_devices;
    PFN_vkDestroyInstance early_destroy_instance{};
    PFN_vkDestroyDevice early_destroy_device{};
    bool early_attempted{}, early_ready{};
    void* module{};
    const BbDlssApi* api{};
    std::unique_ptr<VrDlssSession> session;
    VrDlssSession::Device device{};
    std::vector<std::string> instance_extensions, device_extensions;
    std::string problem;
    bool attempted{}, instance_ok{}, device_ok{}, checked{};
};
} // namespace Vulkan
