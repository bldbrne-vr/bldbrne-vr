// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#include "core/vr/s1_dynamic_resolution.h"
#include <algorithm>
#include "video_core/renderer_vulkan/vr_dibr_upscale.h"
#include <cstdlib>
#include <cstring>
#include "common/logging/log.h"
#include "common/path_util.h"
#include "core/vr/dlss5_file.h"
#include "core/vr/graphics_settings.h"
#include <mutex>
#include "video_core/renderer_vulkan/vr_dlss_preset.h"
#include "video_core/renderer_vulkan/vr_dlss_runtime.h"
#ifdef _WIN32
#include <windows.h>
#endif

namespace Vulkan {
namespace {
std::filesystem::path RuntimeDirectory() {
    if (const char* path = std::getenv("VR_DLSS_DIR"); path && *path)
        return std::filesystem::absolute(path);
#ifdef _WIN32
    std::wstring path(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (!n || n >= path.size())
        return {};
    path.resize(n);
    return std::filesystem::path(path).parent_path() / "dlss";
#else
    return {};
#endif
}
// The player's DLSS 5 file: VR_DLSS5_FILE (tests), else user/dlss5 (the launcher copies it there),
// else an older setup's copy beside the DLSS bridge.
std::filesystem::path NeuralCandidate() {
    if (const char* path = std::getenv("VR_DLSS5_FILE"); path && *path)
        return std::filesystem::absolute(path);
    std::error_code error;
    const auto user = VR::Dlss5File::InUser(Common::FS::GetUserPath(Common::FS::PathType::UserDir));
    if (std::filesystem::is_regular_file(user, error))
        return user;
    const auto beside = RuntimeDirectory();
    return beside.empty() ? user : beside / VR::Dlss5File::Name;
}
void Remember(std::vector<std::string>& stored, const std::vector<const char*>& enabled) {
    stored.clear();
    stored.reserve(enabled.size());
    for (const char* name : enabled)
        stored.emplace_back(name);
}
bool Requirements(const VkExtensionProperties* required, uint32_t count,
                  const std::vector<vk::ExtensionProperties>& available,
                  std::vector<std::string>& stored, std::vector<const char*>* append) {
    if (count && !required)
        return false;
    // Validate the entire list before changing enabled extensions.
    for (uint32_t i = 0; i < count; ++i) {
        const auto& r = required[i];
        if (std::none_of(available.begin(), available.end(), [&](const auto& p) {
                return std::strcmp(p.extensionName.data(), r.extensionName) == 0 &&
                       p.specVersion >= r.specVersion;
            }))
            return false;
        const bool enabled =
            std::find(stored.begin(), stored.end(), r.extensionName) != stored.end();
        if (!enabled && !append)
            return false;
    }
    if (append) {
        for (uint32_t i = 0; i < count; ++i) {
            if (std::find(stored.begin(), stored.end(), required[i].extensionName) == stored.end()) {
                LOG_INFO(Render_Vulkan, "VR DLSS: enabling required extension {}", required[i].extensionName);
                stored.emplace_back(required[i].extensionName);
            }
        }
        // The caller consumes these strings during device creation, before the next update.
        append->clear();
        append->reserve(stored.size());
        for (const auto& name : stored)
            append->push_back(name.c_str());
    }
    return true;
}
} // namespace
std::filesystem::path VerifiedNeuralFile() {
    static std::once_flag once;
    static std::filesystem::path verified;
    std::call_once(once, [] {
        const auto file = NeuralCandidate();
        const auto check = VR::Dlss5File::Verify(file);
        if (check.ok) {
            verified = file;
            LOG_INFO(Render_Vulkan, "VR DLSS 5: {} {} (NVIDIA-signed)", file.string(), check.version);
        } else {
            LOG_INFO(Render_Vulkan, "VR DLSS 5 unavailable: {} {}", file.string(), check.problem);
        }
        VR::Graphics::neural_file.store(check.ok ? 1 : 0);
    });
    return verified;
}
VrDlssRuntime& VrDlssRuntime::Get() {
    // Deliberate process lifetime: the bridge must outlive NGX/driver callbacks.
    static auto* runtime = new VrDlssRuntime;
    return *runtime;
}
void VrDlssRuntime::PrepareDriverAddressSpace() {
#ifdef _WIN32
    if (!std::getenv("VR_NATIVE_STEREO") && !DibrUpscale::Requested()) return;
    if (early_attempted || VR::Graphics::Accepted().upscaler != VR::Graphics::Upscaler::Dlss)
        return;
    early_attempted = true;
    VR::Graphics::dlss_early.store(0, std::memory_order_relaxed); // until it succeeds below
    if (!Load()) return;
    // Keep an unused device alive until the real one exists. NVIDIA reserves its
    // binary-import VA here, and the guest memory map then skips those host ranges.
    // A private dispatcher leaves the later renderer/OpenXR setup untouched.
    static vk::detail::DynamicLoader loader;
    const auto get_proc = loader.getProcAddress<PFN_vkGetInstanceProcAddr>("vkGetInstanceProcAddr");
    if (!get_proc) { Fail("cannot load Vulkan for early DLSS initialization"); return; }
    vk::detail::DispatchLoaderDynamic dispatch(get_proc);
    uint32_t count{};
    const VkExtensionProperties* required{};
    if (!api->InstanceExtensions(&count,&required) || (count && !required)) {
        Fail("cannot query early DLSS instance requirements"); return;
    }
    std::vector<const char*> names;
    for (uint32_t i=0;i<count;++i) names.push_back(required[i].extensionName);
    const VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO,nullptr,
        "shadPS4 DLSS address reservation",0,nullptr,0,VK_API_VERSION_1_3};
    const VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,nullptr,0,
        &app,0,nullptr,uint32_t(names.size()),names.data()};
    if (dispatch.vkCreateInstance(&info,nullptr,&early_instance)!=VK_SUCCESS) {
        Fail("early DLSS Vulkan instance creation failed"); return;
    }
    dispatch.init(early_instance, get_proc);
    early_destroy_instance=dispatch.vkDestroyInstance;
    uint32_t physical_count{};
    if (dispatch.vkEnumeratePhysicalDevices(early_instance,&physical_count,nullptr)!=VK_SUCCESS) {
        ReleaseDriverReservation(); Fail("early DLSS GPU enumeration failed"); return;
    }
    std::vector<VkPhysicalDevice> physical(physical_count);
    if (dispatch.vkEnumeratePhysicalDevices(early_instance,&physical_count,physical.data())!=VK_SUCCESS) {
        ReleaseDriverReservation(); Fail("early DLSS GPU enumeration failed"); return;
    }
    // OpenXR may select a different GPU than the desktop preference. Prepare all
    // eligible NVIDIA devices; OFF and FSR never enter this path.
    for (const auto gpu:physical) {
        VkPhysicalDeviceProperties properties{};
        dispatch.vkGetPhysicalDeviceProperties(gpu,&properties);
        if (properties.vendorID!=0x10de) continue;
        count=0; required=nullptr;
        if (!api->DeviceExtensions(early_instance,gpu,&count,&required) || (count && !required)) continue;
        names.clear();
        for (uint32_t i=0;i<count;++i) names.push_back(required[i].extensionName);
        uint32_t family_count{};
        dispatch.vkGetPhysicalDeviceQueueFamilyProperties(gpu,&family_count,nullptr);
        std::vector<VkQueueFamilyProperties> families(family_count);
        dispatch.vkGetPhysicalDeviceQueueFamilyProperties(gpu,&family_count,families.data());
        uint32_t family=0;
        while (family<family_count && !(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT)) ++family;
        if (family==family_count) continue;
        const float priority=1;
        const VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,nullptr,0,family,1,&priority};
        VkPhysicalDeviceVulkan12Features features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
        features.bufferDeviceAddress=VK_TRUE;
        const VkDeviceCreateInfo device_info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,&features,0,
            1,&queue,0,nullptr,uint32_t(names.size()),names.data(),nullptr};
        VkDevice reserve{};
        if (dispatch.vkCreateDevice(gpu,&device_info,nullptr,&reserve)==VK_SUCCESS) {
            early_devices.push_back(reserve);
            early_destroy_device=dispatch.vkDestroyDevice;
        }
    }
    early_ready=!early_devices.empty();
    VR::Graphics::dlss_early.store(early_ready ? 1 : 0, std::memory_order_relaxed);
    if (!early_ready) {
        ReleaseDriverReservation(); Fail("early DLSS driver address reservation failed");
    } else LOG_INFO(Render_Vulkan,"VR DLSS: driver address space prepared before guest reservation");
#endif
}
void VrDlssRuntime::ReleaseDriverReservation() {
    for (const auto d:early_devices) early_destroy_device(d,nullptr);
    early_devices.clear();
    if (early_instance && early_destroy_instance) early_destroy_instance(early_instance,nullptr);
    early_instance=VK_NULL_HANDLE;
}
void VrDlssRuntime::Log(int warning, const char* message) {
    if (warning)
        LOG_WARNING(Render_Vulkan, "VR DLSS: {}", message ? message : "");
    else
        LOG_DEBUG(Render_Vulkan, "VR DLSS: {}", message ? message : "");
}
void VrDlssRuntime::Fail(const char* reason) {
    if (problem != reason)
        LOG_WARNING(Render_Vulkan, "VR DLSS unavailable: {}", reason);
    problem = reason;
}
bool VrDlssRuntime::Load() {
    // DIBR loads NGX only for its centre reconstruction (vr_dibr_upscale.h; VR_DIBR_UPSCALE=0
    // never does).
    if (!std::getenv("VR_NATIVE_STEREO") && !DibrUpscale::Requested()) {
        problem = "Disabled in DIBR mode (VR_DIBR_UPSCALE=0)";
        return false;
    }
    if (attempted)
        return api != nullptr;
    attempted = true;
#ifdef _WIN32
    const auto directory = RuntimeDirectory();
    std::error_code error;
    if (directory.empty() ||
        !std::filesystem::is_regular_file(directory / "nvngx_dlss.dll", error) ||
        !std::filesystem::is_regular_file(directory / "vr_dlss_stereo.dll", error)) {
        Fail("stereo bridge/runtime missing (dlss folder or VR_DLSS_DIR)");
        return false;
    }
    module = LoadLibraryExW((directory / "vr_dlss_stereo.dll").c_str(), nullptr,
                            LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS);
    const auto get = module ? reinterpret_cast<BbDlssGetApiFn>(GetProcAddress(
                                  static_cast<HMODULE>(module), "VrDlssGetStereoApi"))
                            : nullptr;
    const auto* candidate = get ? get() : nullptr;
    if (!candidate || candidate->abi != BBPORT_DLSS_BRIDGE_ABI || !candidate->Configure ||
        !candidate->InstanceExtensions || !candidate->DeviceExtensions || !candidate->Initialize ||
        !candidate->CreateFeature || !candidate->Evaluate || !candidate->ReleaseFeature ||
        !candidate->Shutdown) {
        Fail("stereo bridge ABI mismatch or incomplete API");
        return false;
    }
    const auto data = Common::FS::GetUserPath(Common::FS::PathType::CacheDir) / "vr-dlss";
    std::filesystem::create_directories(data, error);
    if (error || !candidate->Configure(directory.wstring().c_str(), data.wstring().c_str(), Log)) {
        Fail("cannot configure NGX paths/cache");
        return false;
    }
    if (S1Drs::enabled) {
        S1Drs::dlss_region.store(reinterpret_cast<S1Drs::SetRegion>(GetProcAddress(
            static_cast<HMODULE>(module), "VrDlssSetInputReductionPercent")), std::memory_order_release);
    }
    api = candidate;
    session = std::make_unique<VrDlssSession>(api);
    return true;
#else
    Fail("stereo DLSS runtime loader is currently implemented for Windows only");
    return false;
#endif
}
void VrDlssRuntime::PrepareInstance(std::vector<const char*>& enabled, bool requested) {
    Remember(instance_extensions, enabled);
    instance_ok = false;
    device_ok = false;
    checked = false;
    if (!requested || !Load())
        return;
    CheckRequirements(true, &enabled);
}
void VrDlssRuntime::PrepareDevice(vk::Instance instance, vk::PhysicalDevice physical,
                                  std::vector<const char*>& enabled, bool requested) {
    device.instance = instance;
    device.physical = physical;
    Remember(device_extensions, enabled);
    device_ok = false;
    checked = false;
    if (requested && early_attempted && !early_ready) return;
    if (!requested || !Load())
        return;
    if (physical.getProperties().vendorID != 0x10de) {
        Fail("DLSS requires a supported NVIDIA GPU");
        return;
    }
    CheckRequirements(true, nullptr, &enabled);
}
bool VrDlssRuntime::CheckRequirements(bool append, std::vector<const char*>* instance_enabled,
                                      std::vector<const char*>* device_enabled) {
    if (!api)
        return false;
    uint32_t count = 0;
    const VkExtensionProperties* required = nullptr;
    const auto [ir, available_instance] = vk::enumerateInstanceExtensionProperties();
    instance_ok = ir == vk::Result::eSuccess && api->InstanceExtensions(&count, &required) &&
                  Requirements(required, count, available_instance, instance_extensions,
                               append ? instance_enabled : nullptr);
    if (!instance_ok) {
        Fail("required Vulkan instance extensions are not enabled; enable DLSS and restart");
        return false;
    }
    if (!device.physical || instance_enabled)
        return true;
    count = 0;
    required = nullptr;
    const auto [dr, available_device] =
        vk::PhysicalDevice(device.physical).enumerateDeviceExtensionProperties();
    device_ok = dr == vk::Result::eSuccess &&
                api->DeviceExtensions(device.instance, device.physical, &count, &required) &&
                Requirements(required, count, available_device, device_extensions,
                             append ? device_enabled : nullptr);
    if (!device_ok)
        Fail("required DLSS device capabilities/extensions missing; restart after enabling DLSS");
    return device_ok;
}
void VrDlssRuntime::AttachDevice(vk::Instance instance, vk::PhysicalDevice physical,
                                 vk::Device logical) {
    device = {instance, physical, logical, VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr,
              VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr};
    checked = false;
    ReleaseDriverReservation(); // real device now owns the driver reservation
}
VrDlssSession* VrDlssRuntime::Activate(const VR::Graphics::Config& accepted,
                                       uint32_t sr_preset) {
    if (accepted.upscaler != VR::Graphics::Upscaler::Dlss) {
        if (session)
            session->Disable();
        return nullptr;
    }
    if (!Load() || !device.logical)
        return nullptr;
    if (!checked) {
        checked = true;
        CheckRequirements(false);
    }
    if (!instance_ok || !device_ok)
        return nullptr;
#ifdef _WIN32
    using SetNeural = int (*)(int, unsigned, unsigned, unsigned, unsigned);
    const auto neural = reinterpret_cast<SetNeural>(GetProcAddress(
        static_cast<HMODULE>(module), "VrDlssSetNeuralConfig"));
    // DLSS 5 only on an RTX 50 card (the VR page offers it nowhere else; an older vr.ini could).
    if (accepted.dlss5 && VR::Graphics::neural_gpu.load(std::memory_order_relaxed) != 1) {
        Fail("DLSS 5 needs a GeForce RTX 50-series GPU; choose DLSS or FSR");
        return nullptr;
    }
    // The player's own NVIDIA file (never shipped): the bridge loads it from where it was found.
    if (accepted.dlss5) {
        using SetRuntime = int (*)(const wchar_t*);
        const auto set_runtime = reinterpret_cast<SetRuntime>(GetProcAddress(
            static_cast<HMODULE>(module), "VrDlssSetNeuralRuntime"));
        const auto file = VerifiedNeuralFile();
        if (file.empty()) {
            Fail("DLSS 5 needs NVIDIA's signed nvngx_dlssnr.dll (add it in the launcher's DLSS 5 tab)");
            return nullptr;
        }
        if (!set_runtime || !set_runtime(file.c_str())) {
            Fail("DLSS 5 bridge cannot take the player's DLSS 5 file");
            return nullptr;
        }
    }
    // DLSS 5 runs at full strength whatever an older vr.ini kept (the VR page has one DLSS 5
    // setting, on or off; the tester, 2026-10-27).
    if ((accepted.dlss5 && !neural) || (neural && !neural(accepted.dlss5,
            unsigned(accepted.neural), 100u, 100u, 100u))) {
        Fail("DLSS5 bridge unavailable or previous eye features have not retired");
        return nullptr;
    }
    static_assert(VR::Graphics::DlssModelAuto == VR_DLSS_SR_PRESET_AUTO);
    // FAST bit 52 / vr.ini dlss_model: an optional export, so the bridge keeps ABI 2. It is
    // called with 0 too, which restores a bridge's built-in model after another selection. The
    // features this pair creates next (both eyes, any verification twin) use it.
    const auto set_preset = reinterpret_cast<VrDlssSetSrPresetFn>(GetProcAddress(
        static_cast<HMODULE>(module), "VrDlssSetSrPreset"));
    DlssPresetNote(sr_preset, set_preset != nullptr, set_preset ? set_preset(sr_preset) : -1);
#endif
    if (session->Activate(accepted, device, true) != VrDlssSession::Result::Ready) {
        Fail("NGX initialization failed; inspect VR DLSS log");
        return nullptr;
    }
    problem.clear();
    return session.get();
}
bool VrDlssRuntime::Shutdown(uint64_t completed) {
    if (session) {
        session->Completed(completed);
        if (!session->Shutdown())
            return false;
    }
    device = {};
    device_ok = checked = false;
    return true;
}
} // namespace Vulkan
