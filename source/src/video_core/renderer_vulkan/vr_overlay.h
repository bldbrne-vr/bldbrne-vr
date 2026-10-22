// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#ifdef ENABLE_OPENXR

#include <array>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <openxr/openxr.h>

#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vr_overlay_meshes.h"

VK_DEFINE_HANDLE(VmaAllocator)
VK_DEFINE_HANDLE(VmaAllocation)

namespace Vulkan {

// VR overlay renderer: draws small lit meshes (vr_overlay_meshes.h: the Blood Vial, controllers,
// ghost hands, the laser) for both eyes at the headset rate into its own OpenXR swapchain, and
// returns a projection layer the presenter submits on top of the game layer
// (XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, premultiplied). The layer carries the
// exact eye poses and FOVs it was rendered with, so the compositor reprojects it independently
// of the game layer.
//
// One double-wide swapchain (left eye | right eye) is rendered in a single dynamic-rendering
// pass with 4x MSAA (resolved into the swapchain image) and reversed-Z depth; the draw list is
// recorded once per eye with that eye's viewport. No device features beyond Vulkan 1.3 core
// (dynamic rendering, synchronization2) are needed.
//
// Threading (all calls on the OpenXR frame thread, once per XR frame, after xrBeginFrame):
//     BeginFrame(display_time)  locates both eyes (LOCAL, at the predicted display time)
//     Draw(...) / Items()       collects this frame's draw list
//     EndFrame()                waits for the acquired image (1 ms cap) and records; no locks,
//                               never waits on the GPU (a busy slot reuses the last image)
//     SubmitLayer(next)         with the queue lock held (the presenter's Submit, right before
//                               xrEndFrame): queue submit, xrReleaseSwapchainImage, acquire of
//                               the next image; returns the layer or nullptr
// An empty draw list submits no layer and costs nothing. Any Vulkan or OpenXR error disables the
// overlay for the session (one log line) so it can never take the game layer down with it.
class VrOverlayRenderer {
public:
    // Device handles shared with the game renderer.
    struct DeviceContext {
        vk::PhysicalDevice physical_device;
        vk::Device device;
        VmaAllocator allocator{};
        vk::Queue queue;           // the queue the OpenXR session was created with
        u32 queue_family{};
        std::mutex* queue_mutex{}; // guards `queue` (Scheduler::submit_mutex, or the runtime's own
                                   // queue's lock: Instance::GetXrQueueMutex)
        // VrPageLayer: its swapchain may have a second mip level (false on every runtime but
        // Meta's, vr_xr_queue_policy.h SwapchainMips). The overlay's has one level either way.
        bool swapchain_mips{true};
    };

    // From the emulator's Instance: VrOverlayRenderer::FromInstance(instance,
    // Scheduler::submit_mutex). A template so this header needs neither vk_instance.h nor
    // vk_scheduler.h.
    template <typename InstanceT>
    static DeviceContext FromInstance(const InstanceT& instance, std::mutex& queue_mutex) {
        return {instance.GetPhysicalDevice(),       instance.GetDevice(),
                instance.GetAllocator(),            instance.GetGraphicsQueue(),
                instance.GetGraphicsQueueFamilyIndex(), &queue_mutex};
    }

    struct SessionContext {
        XrInstance instance{XR_NULL_HANDLE};
        XrSystemId system{XR_NULL_SYSTEM_ID};
        XrSession session{XR_NULL_HANDLE};
        XrSpace space{XR_NULL_HANDLE}; // layer space (LOCAL)
        std::array<XrViewConfigurationView, 2> views{};
        std::span<const s64> formats; // xrEnumerateSwapchainFormats (only read during Init)
        // The runtime drops a premultiplied layer's alpha-0 pixels instead of adding their light
        // (SteamVR): the additive pass (sparks, the laser, the vial's glow) also writes its alpha,
        // so they show there as bright streaks over what is behind. Meta adds them as before.
        bool additive_alpha{false};
    };

    struct Settings {
        bool enabled{true};
        bool offscreen{false}; // private sampled target, no XR swapchain
        float scale{0.70f}; // eye size relative to the recommended size, 0.5 .. 1
        u32 msaa{4};        // 1, 2, 4 or 8 (lowered to what the device supports)
        float exposure{1.0f};
        float near_m{0.03f};
    };

    struct Stats {
        u32 frames{};        // BeginFrame calls with a non-empty draw list
        u32 drawn{};         // images rendered and released
        u32 layers{};        // layers returned
        u32 reused{};        // frames that showed the previous image (no pose / GPU / wait)
        u32 gpu_late{};      // ... because the GPU had not finished the image two frames back
        u32 wait_timeouts{}; // ... because xrWaitSwapchainImage timed out (1 ms)
        double gpu_ms{};     // summed GPU time of finished images
        u32 gpu_samples{};
    };

    struct EyeViews {
        std::array<XrPosef, 2> pose{};
        std::array<XrFovf, 2> fov{};
    };

    // The stereo of the game layer under the overlay (the presenter sets it before BeginFrame).
    // The game layer's eyes are `baseline_m` apart (vr.ini ipd_mm, DIBR's baseline), not the
    // runtime's: the overlay is rendered and submitted with that eye distance too, so the vial
    // looks as far away as the hunter's fist it sits in. knee_m / min_m: the DIBR near-field
    // compression (dibr_knee_m / dibr_near_clamp_m) applied to each item's disparity. All 0 (the
    // virtual screen, the menu panel): the runtime's own eyes.
    struct StereoMatch {
        float baseline_m{0.0f};
        float knee_m{0.0f};
        float min_m{0.0f};
        bool operator==(const StereoMatch&) const = default;
    };

    VrOverlayRenderer();
    ~VrOverlayRenderer();
    VrOverlayRenderer(const VrOverlayRenderer&) = delete;
    VrOverlayRenderer& operator=(const VrOverlayRenderer&) = delete;

    // Creates the swapchain, the render targets, the pipelines (GLSL compiled here, not on the
    // frame thread) and uploads the meshes. Takes queue_mutex briefly (mesh upload, first
    // acquire): do not hold it when calling. False (logged) leaves the overlay disabled.
    bool Init(const DeviceContext& device, const SessionContext& session,
              const Settings& settings, const VrOverlayRenderer* reuse = nullptr);
    // Waits for the overlay's own GPU work (not the queue), destroys everything including the
    // swapchain. Call before xrDestroySession.
    void Shutdown();

    [[nodiscard]] bool IsActive() const {
        return initialized && !failed;
    }
    void Disable(std::string_view reason);

    // Locates both eyes in the layer space at `display_time` (no extra prediction).
    bool LocateEyes(XrTime display_time, EyeViews& views) const;
    // A recenter changed the presenter's tracking space (frame thread, between frames; `space` is
    // the same handle when the runtime moved LOCAL itself): the eyes are located in and the layer
    // submitted to `space` from the next frame on. The last image (shown again while a frame
    // cannot be drawn) was placed in the old space: it is dropped.
    void SetSpace(XrSpace space) {
        xr.space = space;
        has_image = false;
    }
    // After BeginFrame: the head of this frame's eyes (their midpoint, their mean orientation),
    // for head-locked items such as the heal vignette. False without valid views.
    bool HeadPose(VrOverlay::Pose& head) const;
    // Before BeginFrame: the game layer's stereo the next frames match (see StereoMatch).
    void SetStereoMatch(const StereoMatch& match) {
        stereo_match = match;
    }
    // Starts a frame: clears the draw list. The eye views are what the layer will carry.
    void BeginFrame(const EyeViews& views, bool views_valid, bool should_render);
    // Locates the eyes and moves them to the StereoMatch baseline (about their midpoint).
    void BeginFrame(XrTime display_time, bool should_render);

    // Optional per frame (default: DefaultLighting at the eyes' yaw, Settings::exposure).
    void SetLighting(const VrOverlay::Lighting& lighting);
    void Draw(const VrOverlay::DrawItem& item);
    void Draw(VrOverlay::MeshId mesh, VrOverlay::Pass pass, VrOverlay::Shade shade,
              const VrOverlay::Mat4& world_from_model, const VrOverlay::Material& material);
    // What the headset shows this frame (FrameConstants::field, what the fitted vignettes are
    // drawn against): the union of the eyes' located FOV, degrees left, right, up, down; zeros
    // before the eyes are located.
    [[nodiscard]] std::array<float, 4> FieldDeg() const;
    // The draw list, for the VrOverlay::Append* helpers.
    std::vector<VrOverlay::DrawItem>& Items() {
        return items;
    }

    void EndFrame();
    // External frame-thread command stream; caller fences uniform slot reuse and teardown.
    bool RecordOffscreen(vk::CommandBuffer cmd, u32 slot, const EyeViews& eyes,
                         std::span<const VrOverlay::DrawItem> draw_items);
    vk::Image OffscreenImage() const { return offscreen.image; }
    vk::ImageView OffscreenView() const { return offscreen.view; }
    vk::Extent2D OffscreenExtent() const { return {2 * eye_extent.width, eye_extent.height}; }

    // Queue lock held. `layer_next` is chained into the layer (e.g. the base layer's
    // XrCompositionLayerColorScaleBiasKHR during cinematic fades). The returned pointer stays
    // valid until the next BeginFrame.
    const XrCompositionLayerBaseHeader* SubmitLayer(const void* layer_next = nullptr);

    // Stats since the last call.
    Stats TakeStats();
    // e.g. "1440x1536 x2, B8G8R8A8_SRGB, 4x MSAA, 196 MB".
    [[nodiscard]] std::string Describe() const;

private:
    struct Slot {
        vk::CommandBuffer cmd;
        vk::Fence fence;
        vk::DescriptorSet set;
        bool pending{false}; // submitted, fence not yet seen signaled
    };
    struct ImageResource {
        vk::Image image;
        VmaAllocation allocation{};
        vk::ImageView view;
    };
    struct BufferResource {
        vk::Buffer buffer;
        VmaAllocation allocation{};
        void* mapped{};
    };

    bool Fail(std::string_view what);
    bool CreateSwapchain();
    bool CreateTargets(const VrOverlayRenderer* reuse);
    bool CreateImage(ImageResource& out, vk::Format format, vk::ImageUsageFlags usage,
                     vk::ImageAspectFlags aspect, vk::SampleCountFlagBits samples,
                     std::string_view name);
    void DestroyImage(ImageResource& image);
    bool CreateBuffer(BufferResource& out, vk::DeviceSize size, vk::BufferUsageFlags usage,
                      bool host_visible);
    void DestroyBuffer(BufferResource& buffer);
    bool UploadMeshes();
    bool CreatePipelines();
    bool CreateFrameResources();
    bool AcquireImage();
    bool CanReuse() const;
    void ReadTimestamps(u32 slot);
    void PrepareDraw(u32 slot_index);
    void Record(Slot& slot, u32 slot_index, vk::CommandBuffer external = {});

    DeviceContext dev{};
    SessionContext xr{};
    Settings settings{};
    bool initialized{false};
    bool failed{false};

    // Swapchain (double wide: left eye at x = 0, right eye at x = eye_extent.width).
    XrSwapchain swapchain{XR_NULL_HANDLE};
    std::vector<vk::Image> swapchain_images;
    std::vector<vk::ImageView> swapchain_views;
    vk::Format color_format{vk::Format::eUndefined};
    vk::Format depth_format{vk::Format::eUndefined};
    vk::Extent2D eye_extent{};
    vk::SampleCountFlagBits samples{vk::SampleCountFlagBits::e1};
    u32 image_index{0};
    bool acquired{false};
    bool waited{false};
    bool has_image{false};
    u64 shown_frame{0};
    EyeViews shown{};

    // Borrowed immutable meshes may cross queues in the same family. Transient targets
    // are shared only on the same queue, where Record barriers serialize their reuse.
    // The owner must outlive this renderer and all externally submitted commands.
    bool borrowed_targets{false};
    bool borrowed_meshes{false};
    u64 shared_memory_bytes{0};
    ImageResource offscreen{};
    ImageResource msaa_color{};
    ImageResource depth{};
    BufferResource vertices{};
    BufferResource indices{};
    BufferResource uniforms{};
    vk::DeviceSize uniform_stride{};
    std::array<VrOverlay::MeshRange, VrOverlay::MeshCount> ranges{};

    vk::CommandPool command_pool;
    std::array<Slot, 2> slots{};
    u32 slot_index{0};
    vk::DescriptorPool descriptor_pool;
    vk::DescriptorSetLayout set_layout;
    vk::PipelineLayout pipeline_layout;
    std::array<vk::Pipeline, VrOverlay::PassCount> pipelines{}; // by Pass (Holdout = DepthOnly)
    vk::QueryPool query_pool;
    float timestamp_period_ns{0.0f};
    u64 timestamp_mask{~u64{0}};

    // Current frame.
    u64 frame{0};
    bool frame_open{false};
    bool views_valid{false};
    bool recorded{false};
    bool want_layer{false};
    bool lighting_set{false};
    EyeViews pending{};
    StereoMatch stereo_match{};
    // The near-field compression of this frame's views (Record): each item moves with each eye
    // by (1 - VrOverlay::NearDisparityScale) of half the baseline.
    bool knee_active{false};
    VrOverlay::Vec3 knee_centre{};  // midpoint of the eyes
    VrOverlay::Vec3 knee_right{};   // unit, left eye -> right eye
    VrOverlay::Vec3 knee_forward{}; // the left eye's view direction
    float knee_half_m{0.0f};        // half the eye distance
    VrOverlay::Lighting lighting{};
    std::vector<VrOverlay::DrawItem> items;
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    std::array<XrCompositionLayerProjectionView, 2> layer_views{};
    Stats stats{};
    u64 memory_bytes{0};
};

} // namespace Vulkan

#endif // ENABLE_OPENXR
