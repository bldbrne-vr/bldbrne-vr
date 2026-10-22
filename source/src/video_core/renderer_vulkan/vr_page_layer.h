// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#ifdef ENABLE_OPENXR

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <openxr/openxr.h>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vr_overlay.h"

VK_DEFINE_HANDLE(VmaAllocation)

namespace Vulkan {

// The VR settings page's swapchain (vr.ini vr_menu=1, core/vr/vr_page.h): one OpenXR swapchain,
// `width` x 2 `height`, the page image on top and its selection glow below (two layers of one
// image, the glow pulsed with the colour scale). The CPU-drawn images (core/vr/vr_page_render.h,
// premultiplied RGBA8 in the sRGB domain, like the game's UI) are copied in through a staging
// buffer only when they change; a layer keeps showing the last image released.
//
// Frame thread, with the queue lock held (the presenter's Submit): Upload. Any Vulkan or OpenXR
// error disables it for the session (one log line); the page then is not shown and its input is
// handed back (the presenter closes it).
//
// A second mip level (half size, a linear blit after each copy) like the UI layer's chain: the
// page is drawn at the canvas's 24.6 px per degree and minified 1.2-2x on the headset, where its
// thin rules and text shimmered under head motion with one level (offline, at 12 px per degree:
// the error's frame-to-frame change p99 17.6 -> 12.4 of 255, the canvas's 12.5). The halves split
// cleanly at level 1 (the page image's height is even). One level when the format cannot be
// blitted with linear filtering, the runtime refuses the chain, VR_UI_NO_MIPS is set, or on any
// runtime but Meta's (DeviceContext::swapchain_mips; VR_SWAPCHAIN_MIPS=1 for the second level there
// too).
//
// The queue: the overlay's DeviceContext (the graphics queue, or the runtime's own queue with its
// lock when the session has one: vr_xr_queue.h). The "queue lock" below is that queue's.
class VrPageLayer {
public:
    using DeviceContext = VrOverlayRenderer::DeviceContext;

    VrPageLayer() = default;
    ~VrPageLayer();
    VrPageLayer(const VrPageLayer&) = delete;
    VrPageLayer& operator=(const VrPageLayer&) = delete;

    // Creates the swapchain, the staging buffer and the command buffer (once). `formats`: the
    // runtime's swapchain formats (an RGBA8 or BGRA8 sRGB one is used).
    bool Init(const DeviceContext& device, XrSession session, std::span<const s64> formats,
              u32 width, u32 height);
    void Shutdown();

    // Without the queue lock (it is a 14 MB copy): both images (width x height RGBA8 each) into
    // the staging buffer. False while the previous copy is still on the GPU (try again next
    // frame) or after a failure (Failed()).
    bool Stage(const u8* page_rgba, const u8* glow_rgba);
    // Queue lock held: the staged images into the next swapchain image, released. False when it
    // could not this frame (nothing staged, the image not ready: try again) or after a failure.
    bool Upload();
    [[nodiscard]] bool Staged() const {
        return staged;
    }

    [[nodiscard]] bool Ready() const {
        return initialized && !failed;
    }
    [[nodiscard]] bool Failed() const {
        return failed;
    }
    [[nodiscard]] bool HasImage() const {
        return has_image;
    }
    [[nodiscard]] XrSwapchain Swapchain() const {
        return swapchain;
    }
    [[nodiscard]] u32 Width() const {
        return width;
    }
    [[nodiscard]] u32 Height() const {
        return height;
    }
    [[nodiscard]] u32 Uploads() const {
        return uploads;
    }
    [[nodiscard]] u32 MipLevels() const {
        return mips;
    }

private:
    bool Fail(std::string_view what);

    DeviceContext dev{};
    XrSession session{XR_NULL_HANDLE};
    bool initialized{false};
    bool failed{false};
    bool has_image{false};
    bool acquired{false};
    bool waited{false};
    bool pending{false}; // a copy submitted, its fence not yet seen
    bool staged{false};  // the staging buffer holds images not yet copied
    bool bgra{false};
    u32 width{0};
    u32 height{0};
    u32 image_index{0};
    u32 uploads{0};
    u32 mips{1};
    vk::Format format{vk::Format::eUndefined};
    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::vector<vk::Image> images;
    std::vector<bool> image_initialized;
    vk::Buffer staging;
    VmaAllocation staging_allocation{};
    void* staging_mapped{nullptr};
    vk::CommandPool pool;
    vk::CommandBuffer cmd;
    vk::Fence fence;
};

} // namespace Vulkan

#endif // ENABLE_OPENXR
