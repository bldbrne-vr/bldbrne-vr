// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#ifdef ENABLE_OPENXR

#include <array>
#include <vector>

#include <openxr/openxr.h>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"

VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

namespace Vulkan {

// Canted displays (core/vr/eye_cant.h; a Pimax user's report, 2026-11-01): each finished parallel
// eye image turned into its eye's own view (vr_eye_cant.comp: a pure turn about the eye's centre,
// exact for every depth), to be submitted with the eye's own orientation and located FOV.
//   - Record: one eye from a storage view to a storage view (both GENERAL, the caller's barriers;
//     the late warp's own images).
//   - TurnCopy: the game-rate pair (RecordStereoCopy): one eye image (TRANSFER_SRC_OPTIMAL)
//     into its swapchain image (TRANSFER_DST_OPTIMAL). The DIBR and native eyes have an RGBA8
//     UNORM storage view (`src_view`): read in place (back in TRANSFER_SRC_OPTIMAL after), into
//     this eye's turned working image, then copied; without one (any 32-bit colour format, sRGB
//     or not) its bytes are copied into a working image first.
// Both bind a compute pipeline in the caller's command buffer (ScratchPush::Invalidate after).
class VrEyeCant {
public:
    struct Eye {
        vk::ImageView src;  // the parallel eye: storage, RGBA8 UNORM (stored bytes)
        vk::Rect2D src_rect;
        XrFovf src_fov{};   // the FOV of exactly src_rect's pixels (the head's frame)
        vk::ImageView dst;  // the turned eye: storage, RGBA8 UNORM
        vk::Rect2D dst_rect;
        XrFovf dst_fov{};   // the eye's located FOV (its own frame)
        XrQuaternionf cant{0.0f, 0.0f, 0.0f, 1.0f}; // the eye's turn from the head (eye -> head)
    };

    VrEyeCant() = default;
    ~VrEyeCant();
    VrEyeCant(const VrEyeCant&) = delete;
    VrEyeCant& operator=(const VrEyeCant&) = delete;

    // Compiles the pipeline. False (logged): a canted display gets the parallel eyes.
    bool Init(vk::Device device, VmaAllocator allocator);
    // The caller has waited for its GPU work.
    void Shutdown();
    [[nodiscard]] bool Ready() const {
        return static_cast<bool>(pipeline);
    }

    void Record(vk::CommandBuffer cmd, const Eye& eye) const;
    bool TurnCopy(vk::CommandBuffer cmd, u32 eye, vk::Image src, vk::ImageView src_view,
                  vk::Extent2D src_extent, const XrFovf& src_fov, vk::Image dst,
                  vk::Extent2D dst_extent, const XrFovf& dst_fov, const XrQuaternionf& cant);

    // The turned eye's image size: its located FOV `eye` at the density of its parallel image
    // (`cover_px` pixels over `cover`; core/vr/eye_cant.h Extent).
    [[nodiscard]] static vk::Extent2D Extent(const XrFovf& eye, const XrFovf& cover,
                                             vk::Extent2D cover_px);

private:
    struct Image {
        vk::Image image;
        VmaAllocation allocation{};
        vk::ImageView view;
        vk::Extent2D extent{};
    };
    struct Retired {
        Image image;
        u64 at{0};
    };
    bool EnsureImage(Image& image, vk::Extent2D extent);
    void Destroy(Image& image);
    // A resized working image may still be read by the GPU: destroyed RetireCalls TurnCopy calls
    // later (a few game frames), or at Shutdown.
    void Collect(bool all);
    static constexpr u64 RetireCalls = 16;

    vk::Device device;
    VmaAllocator allocator{};
    vk::DescriptorSetLayout set_layout;
    vk::PipelineLayout layout;
    vk::Pipeline pipeline;
    std::array<std::array<Image, 2>, 2> work{}; // per eye: the parallel eye's bytes, the turned eye
    std::vector<Retired> retired;
    u64 calls{0};
};

} // namespace Vulkan

#endif // ENABLE_OPENXR
