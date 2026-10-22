// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <algorithm>
#include <cmath>
#include <vector>

// Used only by the launcher probe process. No global Vulkan dispatcher, rendering,
// swapchains, game state, or refresh-rate request is involved.
namespace Vulkan::BenchmarkRefresh {
template<class T> T Function(XrInstance instance,const char* name) {
    T function{};
    if(XR_FAILED(xrGetInstanceProcAddr(instance,name,reinterpret_cast<PFN_xrVoidFunction*>(&function))))return nullptr;
    return function;
}
inline float Query(XrInstance instance,XrSystemId system) {
#ifdef _WIN32
    struct Resources {
        HMODULE library{};
        VkInstance vk_instance{};
        VkDevice device{};
        XrSession session{};
        PFN_vkDestroyInstance destroy_instance{};
        PFN_vkDestroyDevice destroy_device{};
        ~Resources() {
            if(session)xrDestroySession(session);
            if(device && destroy_device)destroy_device(device,nullptr);
            if(vk_instance && destroy_instance)destroy_instance(vk_instance,nullptr);
            if(library)FreeLibrary(library);
        }
    } resources;
    const auto requirements=Function<PFN_xrGetVulkanGraphicsRequirements2KHR>(instance,"xrGetVulkanGraphicsRequirements2KHR");
    const auto create_instance=Function<PFN_xrCreateVulkanInstanceKHR>(instance,"xrCreateVulkanInstanceKHR");
    const auto get_device=Function<PFN_xrGetVulkanGraphicsDevice2KHR>(instance,"xrGetVulkanGraphicsDevice2KHR");
    const auto create_device=Function<PFN_xrCreateVulkanDeviceKHR>(instance,"xrCreateVulkanDeviceKHR");
    const auto get_rate=Function<PFN_xrGetDisplayRefreshRateFB>(instance,"xrGetDisplayRefreshRateFB");
    if(!requirements || !create_instance || !get_device || !create_device || !get_rate)return 0;
    XrGraphicsRequirementsVulkanKHR required{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR};
    if(XR_FAILED(requirements(instance,system,&required)))return 0;
    const auto version=std::max(required.minApiVersionSupported,XR_MAKE_VERSION(1,1,0));
    if(version>required.maxApiVersionSupported)return 0;
    resources.library=LoadLibraryW(L"vulkan-1.dll");
    if(!resources.library)return 0;
    const auto get_proc=reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(resources.library,"vkGetInstanceProcAddr"));
    if(!get_proc)return 0;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName="Bldbrne-VR refresh probe";
    app.apiVersion=VK_MAKE_VERSION(XR_VERSION_MAJOR(version),XR_VERSION_MINOR(version),XR_VERSION_PATCH(version));
    VkInstanceCreateInfo vk_create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};vk_create.pApplicationInfo=&app;
    XrVulkanInstanceCreateInfoKHR create{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
    create.systemId=system;create.pfnGetInstanceProcAddr=get_proc;create.vulkanCreateInfo=&vk_create;
    VkResult result{};
    if(XR_FAILED(create_instance(instance,&create,&resources.vk_instance,&result)) || result!=VK_SUCCESS)return 0;
    resources.destroy_instance=reinterpret_cast<PFN_vkDestroyInstance>(get_proc(resources.vk_instance,"vkDestroyInstance"));
    if(!resources.destroy_instance)return 0;
    XrVulkanGraphicsDeviceGetInfoKHR device_info{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
    device_info.systemId=system;device_info.vulkanInstance=resources.vk_instance;
    VkPhysicalDevice physical{};
    if(XR_FAILED(get_device(instance,&device_info,&physical)))return 0;
    const auto queues=reinterpret_cast<PFN_vkGetPhysicalDeviceQueueFamilyProperties>(get_proc(resources.vk_instance,"vkGetPhysicalDeviceQueueFamilyProperties"));
    if(!queues)return 0;
    uint32_t count{};queues(physical,&count,nullptr);
    std::vector<VkQueueFamilyProperties> families(count);queues(physical,&count,families.data());
    uint32_t family=0;while(family<count && !(families[family].queueFlags&VK_QUEUE_GRAPHICS_BIT))++family;
    if(family==count)return 0;
    const float priority=1;
    VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    queue.queueFamilyIndex=family;queue.queueCount=1;queue.pQueuePriorities=&priority;
    VkDeviceCreateInfo device_create{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    device_create.queueCreateInfoCount=1;device_create.pQueueCreateInfos=&queue;
    XrVulkanDeviceCreateInfoKHR xr_device{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
    xr_device.systemId=system;xr_device.pfnGetInstanceProcAddr=get_proc;
    xr_device.vulkanPhysicalDevice=physical;xr_device.vulkanCreateInfo=&device_create;
    // Resolve cleanup before creating a device, including partial/error returns.
    resources.destroy_device=reinterpret_cast<PFN_vkDestroyDevice>(get_proc(resources.vk_instance,"vkDestroyDevice"));
    if(!resources.destroy_device)return 0;
    if(XR_FAILED(create_device(instance,&xr_device,&resources.device,&result)) || result!=VK_SUCCESS)return 0;
    XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
    binding.instance=resources.vk_instance;binding.physicalDevice=physical;binding.device=resources.device;
    binding.queueFamilyIndex=family;binding.queueIndex=0;
    XrSessionCreateInfo session{XR_TYPE_SESSION_CREATE_INFO};session.next=&binding;session.systemId=system;
    if(XR_FAILED(xrCreateSession(instance,&session,&resources.session)))return 0;
    float rate{};
    if(XR_FAILED(get_rate(resources.session,&rate)) || !std::isfinite(rate) || rate<30 || rate>240)return 0;
    return rate;
#else
    return 0;
#endif
}
} // namespace Vulkan::BenchmarkRefresh
