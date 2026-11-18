// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <future>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "common/types.h"
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/texture_cache/image.h"

namespace Vulkan {
class Instance;
class Scheduler;
} // namespace Vulkan

namespace VideoCore::SkyDetail {

// Sky detail (vr.ini sky_detail): the game's sky
// panoramas are 2048 x 512 BC1 around the whole horizon (about 5.7 texels a degree, 4-5x
// magnified in a headset: block seams and steps in the gradients), its moons and cloud rings 512
// or 1024 square. The player's own sharper images of them (the launcher's sharper skies: AI
// upscales made on their PC from their copy, <user folder>/sky_detail) become a copy the shaders
// sample instead, stored as BC7 with its mips (vr_sky_encode.comp), made once on the GPU when the
// game first uploads the texture. A texture with no image stays the game's own: never a filtered
// or plainly upscaled copy (2026-10-30). The game's own image stays as it is (copies,
// downloads and overlaps never see the copy). Which images: those whose first 64 KiB match a sky,
// moon or cloud albedo texture (name *sky*_a / *moon*_a / *cloud*_a / *clond*_a, the game's own
// spelling of most cloud layers) of the map texture lists in the player's own dump
// (dvdroot_ps4/map/*/m*.tpfbhd + .tpfbdt), read on a thread at start-up when the folder has
// images: nothing of the game is in this repository.

// A sky, moon or cloud texture of the game, and the pixels of your image for one (sky_detail.cpp).
struct Entry;
struct Replacement;

// The copy of one image made from your image. Lives with its image (Image::sky_twin); its Vulkan objects are
// destroyed once the GPU is past the frame that drops it.
struct Twin {
    Twin(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler);
    ~Twin();
    Twin(const Twin&) = delete;
    Twin& operator=(const Twin&) = delete;

    // The copy's view for a view the game asked of the image (the same levels, `shift` finer at
    // the top when it starts at level 0); nullptr: use the game's image (not a plain 2D view of
    // the colour blocks, or a minimum LOD).
    ImageView* View(const ImageViewInfo& info);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    UniqueImage image;
    vk::Format format{};  // BC7, sRGB when the game's image is
    u32 shift{};          // levels above the game's level 0 (log2 of the scale)
    u32 levels{};
    std::string name;
    const Entry* entry{}; // the catalog's (it never changes once read)
    bool waiting{};       // no copy yet (the game's image is sampled) while your image is read
    std::vector<std::unique_ptr<ImageView>> views;
};

// Starts reading the dump's texture lists (vr.ini sky_detail=0, or no images: nothing). Once per
// process; the builder starts it at the first upload.
void StartCatalog();

// Makes the copies (owned by the texture cache; the recording thread only).
class Builder {
public:
    Builder(const Vulkan::Instance& instance, Vulkan::Scheduler& scheduler);
    ~Builder();
    // After the game's bytes were uploaded into `image` (outside a render pass): a copy for a sky
    // or moon texture with your image (once read), none for anything else (an image whose bytes
    // changed loses its copy).
    // True when the image's copy changed (made or dropped): bound views must be looked up again.
    bool AfterUpload(Image& image);
    // Once a frame (the texture cache's garbage collector): true when images were read since the
    // last call. Then Upgrade every image (true: its copy changed, look views up again), then
    // EndUpgrades.
    bool Poll();
    bool Upgrade(Image& image);
    void EndUpgrades();

private:
    bool Init();
    // `own`: your image's pixels (kept until the GPU has copied them).
    bool Build(Image& image, const Entry& entry, const std::shared_ptr<const Replacement>& own);
    void Request(const Entry& entry);

    const Vulkan::Instance& instance;
    Vulkan::Scheduler& scheduler;
    bool tried{}, failed{};
    vk::UniqueSampler sampler;
    vk::UniqueDescriptorSetLayout encode_set;
    vk::UniquePipelineLayout encode_layout;
    vk::UniquePipeline encode_pipeline;
    vk::UniqueQueryPool timestamps;
    u32 next_query{};
    u64 made{}, bytes{}, not_ready{};
    // Your images being read, read (until the next EndUpgrades) and unreadable ones.
    std::unordered_map<const Entry*, std::future<std::shared_ptr<const Replacement>>> loading;
    std::unordered_map<const Entry*, std::shared_ptr<const Replacement>> loaded;
    std::unordered_set<const Entry*> unusable;
};

} // namespace VideoCore::SkyDetail
