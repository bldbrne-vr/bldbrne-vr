// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "video_core/texture_cache/image.h"
namespace VideoCore {
class TextureCache;
}
namespace Vulkan {
// Reduced raster storage; guest-size storage stays coherent for unmodified compute/readback.
// Mirrors bbport SceneResolution::Coherence, with a single two-layer native-stereo mip.
class VrSceneProxy {
public:
    VrSceneProxy(VideoCore::Image&, uint32_t width, uint32_t height, VideoCore::TextureCache&);
    ~VrSceneProxy();
    static bool Eligible(const VideoCore::Image&);
    bool Matches(const VideoCore::Image&, uint32_t width, uint32_t height) const;
    void NativeAccess(VideoCore::Image&, vk::AccessFlags2);
    vk::ImageView Attachment(VideoCore::Image&, const VideoCore::ImageViewInfo&, bool discard,
                             bool written = true);
    vk::ImageView Read(VideoCore::Image&, uint32_t eye);
    // Bit 46 (vr_dlss_direct.h): the two-layer image behind Read's views, and its format.
    vk::Image ReadImage() const {
        return image;
    }
    vk::Format ReadFormat() const {
        return image.image_ci.format;
    }
    void NativeWritten() {
        valid = dirty = false;
    }
    bool attachment_scope{};
    uint32_t uploads{}, resolves{};
    vk::ImageLayout Layout() const {
        return layout;
    }

private:
    void Fill(VideoCore::Image&, bool to_native);
    void Transition(vk::ImageLayout, vk::AccessFlags2);
    const Instance& instance;
    Scheduler& scheduler;
    VideoCore::UniqueImage image;
    vk::Image native{};
    vk::ImageAspectFlags aspects;
    vk::ImageLayout layout{vk::ImageLayout::eUndefined};
    std::vector<std::pair<VideoCore::ImageViewInfo, vk::UniqueImageView>> views;
    std::array<vk::UniqueImageView, 2> reads;
    bool valid{}, dirty{}, copying{};
};
} // namespace Vulkan
