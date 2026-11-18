// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#include "video_core/texture_cache/sky_detail.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <fmt/format.h>
#include <vk_mem_alloc.h>
#include <xxhash.h>

#include "common/div_ceil.h"
#include "common/logging/log.h"
#include "common/path_util.h"
#include "common/singleton.h"
#include "common/stb.h"
#include "common/thread.h"
#include "core/file_sys/fs.h"
#include "core/vr/bc7_decode.h"
#include "core/vr/bloodborne_assets.h"
#include "core/vr/sky_textures.h"
#include "core/vr/vr_session.h"
#include "video_core/buffer_cache/buffer.h"
#include "video_core/host_shaders/vr_sky_encode_comp.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_shader_util.h"

namespace VideoCore::SkyDetail {

// A sharper level 0 made outside the emulator (an AI upscale of the player's own texture: the
// launcher's sharper skies, launcher/sky_upscale.cpp): 2, 4 or 8 times the game's size, RGBA8
// holding the same sRGB-encoded values as the game's texture. Its
// pixels wait in an upload buffer the reading thread filled (the copy of a 4096 x 2048 image
// took 7.8 ms on the recording thread, 2026-10-30): plain VMA, which locks internally, not a
// UniqueBuffer (those register with native stereo, on the recording thread).
struct Replacement {
    Replacement() = default;
    Replacement(const Replacement&) = delete;
    Replacement& operator=(const Replacement&) = delete;
    ~Replacement() {
        if (buffer) {
            vmaDestroyBuffer(allocator, buffer, allocation);
        }
    }
    u32 width{}, height{};
    VmaAllocator allocator{};
    VkBuffer buffer{};
    VmaAllocation allocation{};
};

struct Entry {
    std::string name;
    u64 key{}; // XXH3 of the top level's first KeyBytes
    u32 width{}, height{};
    // The same bytes under other names (m24_00_moon_red_a is also m21_01_m24_00_moon_red_a): an
    // image under any of them is used.
    std::vector<std::string> aliases;
    // Your image for it (empty: none): <sky folder>/<name>.<key as 16 hex digits>.png, else
    // <name>.png. Read when the texture is first uploaded (Builder::Request).
    std::filesystem::path image;
};

namespace {

using VR::SkyTextures::KeyBytes;
// The textures the launcher makes images for (launcher/sky_upscale.h MaxSize): those it can
// upscale at least 2x within 4096 wide (a 2048 x 512 panorama -> 4096 x 1024).
constexpr u32 MaxSize = 4096;

struct Catalog {
    std::once_flag once;
    std::atomic<bool> ready{false};
    std::unordered_map<u64, Entry> entries; // written before `ready`, read-only after
    std::vector<std::pair<u32, u32>> sizes;
    std::jthread thread;
};
Catalog& Cat() {
    static Catalog catalog;
    return catalog;
}

std::optional<std::vector<u8>> ReadRange(std::ifstream& file, u64 offset, u64 size) {
    if (size == 0 || size > (64u << 20)) {
        return std::nullopt;
    }
    std::vector<u8> out(size);
    file.clear();
    file.seekg(static_cast<std::streamoff>(offset));
    if (!file.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(size))) {
        return std::nullopt;
    }
    return out;
}

// The folder of the player's own sharper images: VR_SKY_DIR, else <user folder>/sky_detail (where
// the launcher's sharper skies are made).
std::filesystem::path ReplacementFolder() {
    if (const char* dir = std::getenv("VR_SKY_DIR"); dir && dir[0]) {
        return dir;
    }
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "sky_detail";
}

// The player's sharper weapons (the launcher's, launcher/sky_upscale.h Kind::Weapons): VR_WEAPON_DIR,
// else <user folder>/weapon_detail.
std::filesystem::path WeaponFolder() {
    if (const char* dir = std::getenv("VR_WEAPON_DIR"); dir && dir[0]) {
        return dir;
    }
    return Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "weapon_detail";
}

// The size a PNG says it is (its header only), nullopt for anything else.
std::optional<std::pair<u32, u32>> PngSize(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    std::array<char, 64> head{};
    if (!file || !file.read(head.data(), head.size())) {
        return std::nullopt;
    }
    int w = 0, h = 0, channels = 0;
    if (!stbi_info_from_memory(reinterpret_cast<const stbi_uc*>(head.data()),
                               static_cast<int>(head.size()), &w, &h, &channels)) {
        return std::nullopt;
    }
    return std::pair<u32, u32>{static_cast<u32>(w), static_cast<u32>(h)};
}

// Each catalog texture's image in the folder, if it has one: <name>.<key>.png (the launcher's,
// named by the bytes: m12_sky_a is two different skies in m21 and m24) under any of its names,
// else <name>.png. Only the headers are read here; an image whose size is not 2, 4 or 8 times the
// game's is skipped (logged). The number of textures that have one.
u32 FindImages() {
    const auto folder = ReplacementFolder();
    std::error_code ec;
    if (!std::filesystem::is_directory(folder, ec)) {
        return 0;
    }
    u32 count = 0;
    for (auto& [key, entry] : Cat().entries) {
        std::vector<std::string> names{entry.name};
        names.insert(names.end(), entry.aliases.begin(), entry.aliases.end());
        std::vector<std::filesystem::path> candidates;
        for (const auto& name : names) {
            candidates.push_back(folder / fmt::format("{}.{:016x}.png", name, key));
        }
        for (const auto& name : names) {
            candidates.push_back(folder / (name + ".png"));
        }
        for (const auto& path : candidates) {
            if (!std::filesystem::is_regular_file(path, ec)) {
                continue;
            }
            const auto size = PngSize(path);
            const u32 scale = size ? size->first / entry.width : 0;
            if (!size || scale < 2 || scale > 8 || !std::has_single_bit(scale) ||
                size->first != entry.width * scale || size->second != entry.height * scale) {
                LOG_WARNING(Render_Vulkan,
                            "VR sky detail: {} is not a PNG 2, 4 or 8 times the game's {}x{} "
                            "(skipped)",
                            path.string(), entry.width, entry.height);
                continue;
            }
            entry.image = path;
            ++count;
            break;
        }
    }
    if (count) {
        LOG_INFO(Render_Vulkan, "VR sky detail: {} of your own images in {} (read when first seen)",
                 count, folder.string());
    }
    return count;
}

// Whether `folder` holds any PNG (else nothing is read: every texture stays the game's).
bool HasImages(const std::filesystem::path& folder) {
    std::error_code ec;
    for (auto it = std::filesystem::directory_iterator(folder, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        if (it->path().extension() == ".png" && it->is_regular_file(ec)) {
            return true;
        }
    }
    return false;
}

// The sharper weapons: each image names its texture and that texture's bytes
// (<name>.<key as 16 hex digits>.png, made at 2x), so they need no scan of the game's files: an
// image whose texture the game changed (an update, a mod) simply never matches. The number used.
u32 AddWeaponImages() {
    const auto folder = WeaponFolder();
    auto& cat = Cat();
    std::error_code ec;
    u32 count = 0, skipped = 0;
    for (auto it = std::filesystem::directory_iterator(folder, ec);
         !ec && it != std::filesystem::directory_iterator(); it.increment(ec)) {
        const auto& path = it->path();
        if (path.extension() != ".png" || !it->is_regular_file(ec)) {
            continue;
        }
        const std::string stem = path.stem().string();
        const auto dot = stem.rfind('.');
        u64 key = 0;
        const bool named = dot != std::string::npos && dot > 0 && stem.size() - dot - 1 == 16 &&
                           std::from_chars(stem.data() + dot + 1, stem.data() + stem.size(), key, 16)
                                   .ptr == stem.data() + stem.size();
        const auto size = named ? PngSize(path) : std::nullopt;
        if (!size || size->first % 2 || size->second % 2 || size->first < 512 ||
            size->second < 256 || size->first > 2 * MaxSize || size->second > 2 * MaxSize) {
            ++skipped;
            continue;
        }
        Entry entry{stem.substr(0, dot), key, size->first / 2, size->second / 2};
        entry.image = path;
        if (cat.entries.try_emplace(key, std::move(entry)).second) {
            ++count;
        }
    }
    LOG_INFO(Render_Vulkan, "VR sky detail: {} of your sharper weapons in {}{}", count,
             folder.string(), skipped ? fmt::format(" ({} other files skipped)", skipped) : "");
    return count;
}

void Scan() {
    Common::SetCurrentThreadName("VR sky detail");
    auto& cat = Cat();
    // Your images or the game's own textures, never a filtered copy (2026-10-30).
    const bool skies = HasImages(ReplacementFolder());
    const bool weapons = HasImages(WeaponFolder());
    if (!skies && !weapons) {
        LOG_INFO(Render_Vulkan, "VR sky detail: no images in {} or {} (the launcher's Sharper "
                                "skies and Sharper weapons make them): the game's own textures",
                 ReplacementFolder().string(), WeaponFolder().string());
        cat.ready.store(true, std::memory_order_release);
        return;
    }
    if (!skies) {
        AddWeaponImages();
        for (const auto& [key, entry] : cat.entries) {
            const std::pair<u32, u32> size{entry.width, entry.height};
            if (std::ranges::find(cat.sizes, size) == cat.sizes.end()) {
                cat.sizes.push_back(size);
            }
        }
        cat.ready.store(true, std::memory_order_release);
        return;
    }
    const auto start = std::chrono::steady_clock::now();
    auto* mnt = Common::Singleton<Core::FileSys::MntPoints>::Instance();
    // Every folder the game's files can come from: the base game, its update and a mods folder
    // (the plain lookup gives only the first that has the map folder: a mods folder with one
    // map file in it hid the whole base game, 2026-10-29).
    using HostPathType = Core::FileSys::MntPoints::HostPathType;
    std::vector<std::filesystem::path> roots;
    std::error_code ec;
    for (const auto type : {HostPathType::Base, HostPathType::Patch, HostPathType::Mod}) {
        auto root = mnt->GetHostPath("/app0/dvdroot_ps4/map", nullptr, type);
        if (!root.empty() && std::filesystem::is_directory(root, ec) &&
            std::ranges::find(roots, root) == roots.end()) {
            roots.push_back(std::move(root));
        }
    }
    u32 lists = 0, read = 0, skipped = 0;
    if (roots.empty()) {
        LOG_WARNING(Render_Vulkan, "VR sky detail: no map folder in the game; off");
        cat.ready.store(true, std::memory_order_release);
        return;
    }
    for (const auto& root : roots) {
    for (auto it = std::filesystem::recursive_directory_iterator(root, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) {
            continue;
        }
        // m24_0000.tpfbhd, or the game update's m24_0000.tpfbhd.patch
        const auto data = VR::SkyTextures::DataFile(it->path());
        if (!data) {
            continue;
        }
        std::ifstream bhd_file(it->path(), std::ios::binary);
        std::ifstream bdt(*data, std::ios::binary);
        if (!bhd_file || !bdt) {
            continue;
        }
        const std::vector<u8> bhd{std::istreambuf_iterator<char>(bhd_file), {}};
        ++lists;
        for (const auto& e : VR::SkyTextures::ReadList(bhd)) {
            if (!VR::SkyTextures::Wanted(e.name)) {
                continue;
            }
            const auto packed = ReadRange(bdt, e.offset, e.size);
            const auto tpf = packed ? VR::Assets::DecompressDcx(*packed) : std::nullopt;
            const auto texture = tpf ? VR::SkyTextures::FirstTexture(*tpf) : std::nullopt;
            if (!texture) {
                ++skipped;
                continue;
            }
            ++read;
            const u32 w = texture->width, h = texture->height;
            if (VR::SkyTextures::ScaleFor(w, h, MaxSize) < 2) {
                continue;
            }
            const u64 key = XXH3_64bits(tpf->data() + texture->offset,
                                        std::min<size_t>(texture->size, KeyBytes));
            std::string name = VR::SkyTextures::TextureName(e.name);
            auto [it, fresh] = cat.entries.try_emplace(key, Entry{name, key, w, h});
            if (!fresh && it->second.name != name &&
                std::ranges::find(it->second.aliases, name) == it->second.aliases.end()) {
                it->second.aliases.push_back(std::move(name));
            }
        }
    }
    }
    const size_t textures = cat.entries.size();
    const u32 images = FindImages();
    // Only the textures with an image get a copy; the rest stay the game's own.
    std::erase_if(cat.entries, [](const auto& e) { return e.second.image.empty(); });
    if (weapons) {
        AddWeaponImages();
    }
    for (const auto& [key, entry] : cat.entries) {
        const std::pair<u32, u32> size{entry.width, entry.height};
        if (std::ranges::find(cat.sizes, size) == cat.sizes.end()) {
            cat.sizes.push_back(size);
        }
    }
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    LOG_INFO(Render_Vulkan,
             "VR sky detail: {} sky, moon and cloud textures ({} read, {} unreadable) in {} "
             "texture lists of the game ({} folders), {} of them with your own images (the rest "
             "stay the game's), {} ms",
             textures, read, skipped, lists, roots.size(), images, ms);
    cat.ready.store(true, std::memory_order_release);
}

// Your image's pixels in an upload buffer (a worker thread); nullptr when it cannot be read, is
// not the size the catalog checked, or there is no memory for it.
std::shared_ptr<const Replacement> ReadImage(const std::filesystem::path& path, u32 width,
                                             u32 height, VmaAllocator allocator) {
    const auto start = std::chrono::steady_clock::now();
    std::ifstream file(path, std::ios::binary);
    const std::vector<u8> png{std::istreambuf_iterator<char>(file), {}};
    int w = 0, h = 0;
    u8* pixels = png.empty() ? nullptr
                             : stbi_load_from_memory(png.data(), static_cast<int>(png.size()), &w,
                                                     &h, nullptr, 4);
    const u32 scale = pixels ? static_cast<u32>(w) / width : 0;
    if (!pixels || scale < 2 || static_cast<u32>(w) != width * scale ||
        static_cast<u32>(h) != height * scale) {
        LOG_WARNING(Render_Vulkan, "VR sky detail: {} could not be read", path.string());
        if (pixels) {
            stbi_image_free(pixels);
        }
        return nullptr;
    }
    auto image = std::make_shared<Replacement>();
    image->width = static_cast<u32>(w);
    image->height = static_cast<u32>(h);
    image->allocator = allocator;
    const VkBufferCreateInfo buffer_ci{.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                                       .size = static_cast<VkDeviceSize>(w) * h * 4,
                                       .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT};
    const VmaAllocationCreateInfo alloc_ci{
        .flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                 VMA_ALLOCATION_CREATE_MAPPED_BIT,
        .usage = VMA_MEMORY_USAGE_AUTO_PREFER_HOST,
    };
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(allocator, &buffer_ci, &alloc_ci, &image->buffer, &image->allocation,
                        &info) != VK_SUCCESS ||
        !info.pMappedData) {
        LOG_WARNING(Render_Vulkan, "VR sky detail: no upload memory for {}",
                    path.filename().string());
        stbi_image_free(pixels);
        return nullptr;
    }
    std::memcpy(info.pMappedData, pixels, buffer_ci.size);
    vmaFlushAllocation(allocator, image->allocation, 0, VK_WHOLE_SIZE);
    stbi_image_free(pixels);
    LOG_INFO(Render_Vulkan, "VR sky detail: {} read ({}x{}, {} ms)", path.filename().string(), w,
             h,
             std::chrono::duration_cast<std::chrono::milliseconds>(
                 std::chrono::steady_clock::now() - start)
                 .count());
    return image;
}

bool IsSrgb(vk::Format format) {
    switch (format) {
    case vk::Format::eBc1RgbSrgbBlock:
    case vk::Format::eBc1RgbaSrgbBlock:
    case vk::Format::eBc2SrgbBlock:
    case vk::Format::eBc3SrgbBlock:
    case vk::Format::eBc7SrgbBlock:
        return true;
    default:
        return false;
    }
}

bool IsBlockColour(vk::Format format) {
    switch (format) {
    case vk::Format::eBc1RgbUnormBlock:
    case vk::Format::eBc1RgbSrgbBlock:
    case vk::Format::eBc1RgbaUnormBlock:
    case vk::Format::eBc1RgbaSrgbBlock:
    case vk::Format::eBc2UnormBlock:
    case vk::Format::eBc2SrgbBlock:
    case vk::Format::eBc3UnormBlock:
    case vk::Format::eBc3SrgbBlock:
    case vk::Format::eBc7UnormBlock:
    case vk::Format::eBc7SrgbBlock:
        return true;
    default:
        return false;
    }
}

// The catalog's entry for this image's bytes (nullptr: not a sky or moon, or the catalog is not
// read yet: `not_ready`).
const Entry* Find(const Image& image, u64& not_ready) {
    const auto& info = image.info;
    if (info.type != AmdGpu::ImageType::Color2D || info.resources.layers != 1 ||
        info.num_samples > 1 || info.props.is_depth || !IsBlockColour(info.pixel_format) ||
        info.guest_address == 0 || info.guest_size < 4096 ||
        True(image.flags & ImageFlagBits::GpuModified)) {
        return nullptr;
    }
    auto& cat = Cat();
    if (!cat.ready.load(std::memory_order_acquire)) {
        ++not_ready;
        return nullptr;
    }
    const std::pair<u32, u32> size{info.size.width, info.size.height};
    if (std::ranges::find(cat.sizes, size) == cat.sizes.end()) {
        return nullptr;
    }
    const u64 key = XXH3_64bits(std::bit_cast<const u8*>(info.guest_address),
                                std::min<size_t>(info.guest_size, KeyBytes));
    const auto it = cat.entries.find(key);
    if (it == cat.entries.end() || it->second.width != size.first ||
        it->second.height != size.second) {
        return nullptr;
    }
    return &it->second;
}

struct EncodePush {
    s32 src_size[2];
    s32 dst_size[2];
    u32 src_level;
    u32 first_block;
    u32 blocks_x;
    u32 blocks_y;
};

constexpr u32 QueryCount = 64;

// VR_SKY_DUMP=1: each copy's top level and its level at the game's size, as PNGs in the log folder
// (decoded from the BC7 blocks, so they show exactly what the shaders sample).
const bool g_dump = [] {
    const char* value = std::getenv("VR_SKY_DUMP");
    return value && value[0] == '1';
}();

void WritePng(const std::string& name, const u8* blocks, u32 w, u32 h) {
    const u32 bx = Common::DivCeil(w, 4u), by = Common::DivCeil(h, 4u);
    std::vector<u8> rgba(static_cast<size_t>(bx) * by * 64);
    VR::Bc7::DecodeImage(blocks, static_cast<int>(bx), static_cast<int>(by), rgba.data());
    const auto path = Common::FS::GetUserPath(Common::FS::PathType::LogDir) / (name + ".png");
    stbi_write_png(path.string().c_str(), static_cast<int>(w), static_cast<int>(h), 4, rgba.data(),
                   static_cast<int>(bx * 4 * 4));
}

} // namespace

void StartCatalog() {
    if (!VR::GetSettings().sky_detail) {
        return;
    }
    auto& cat = Cat();
    std::call_once(cat.once, [&cat] { cat.thread = std::jthread([] { Scan(); }); });
}

Twin::Twin(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_},
      image{instance_.GetDevice(), instance_.GetAllocator()} {}

Twin::~Twin() {
    scheduler.DeferOperation(
        [img = std::move(image), v = std::move(views)]() mutable {
            v.clear();
            img.Destroy();
        });
}

ImageView* Twin::View(const ImageViewInfo& info) {
    if (info.type != AmdGpu::ImageType::Color2D || info.is_storage || info.min_lod != 0 ||
        info.range.base.layer != 0 || info.range.extent.layers != 1 ||
        !IsBlockColour(info.format)) {
        return nullptr;
    }
    for (const auto& view : views) {
        if (view->info == info) {
            return view.get();
        }
    }
    const u32 base = info.range.base.level == 0 ? 0 : info.range.base.level + shift;
    const u32 count = info.range.base.level == 0 ? info.range.extent.levels + shift
                                                 : info.range.extent.levels;
    if (base >= levels) {
        return nullptr;
    }
    const vk::ImageViewUsageCreateInfo usage{.usage = vk::ImageUsageFlagBits::eSampled};
    auto [result, view] = instance.GetDevice().createImageViewUnique({
        .pNext = &usage,
        .image = image.image,
        .viewType = vk::ImageViewType::e2D,
        .format = IsSrgb(info.format) ? vk::Format::eBc7SrgbBlock : vk::Format::eBc7UnormBlock,
        .components = info.mapping,
        .subresourceRange = {vk::ImageAspectFlagBits::eColor, base, std::min(count, levels - base),
                             0, 1},
    });
    if (result != vk::Result::eSuccess) {
        return nullptr;
    }
    LOG_INFO(Render_Vulkan,
             "VR sky detail: {} sampled through its copy (the game's levels {}..{} as {}..{})",
             name, info.range.base.level, info.range.base.level + info.range.extent.levels - 1,
             base, base + std::min(count, levels - base) - 1);
    views.push_back(std::make_unique<ImageView>(info, std::move(view)));
    return views.back().get();
}

Builder::Builder(const Vulkan::Instance& instance_, Vulkan::Scheduler& scheduler_)
    : instance{instance_}, scheduler{scheduler_} {}

Builder::~Builder() {
    if (sampler || encode_pipeline || timestamps) {
        scheduler.DeferOperation([s = std::move(sampler), es = std::move(encode_set),
                                  el = std::move(encode_layout), ep = std::move(encode_pipeline),
                                  q = std::move(timestamps)]() mutable {
            ep.reset();
            el.reset();
            es.reset();
            s.reset();
            q.reset();
        });
    }
}

bool Builder::Init() {
    const auto device = instance.GetDevice();
    const auto features =
        instance.GetPhysicalDevice().getFormatProperties(vk::Format::eBc7SrgbBlock).optimalTilingFeatures;
    if (!(features & vk::FormatFeatureFlagBits::eSampledImage) ||
        !(features & vk::FormatFeatureFlagBits::eTransferDst)) {
        LOG_WARNING(Render_Vulkan, "VR sky detail: this GPU cannot sample BC7; off");
        return false;
    }
    auto s = device.createSamplerUnique({.magFilter = vk::Filter::eNearest,
                                         .minFilter = vk::Filter::eNearest,
                                         .mipmapMode = vk::SamplerMipmapMode::eNearest,
                                         .addressModeU = vk::SamplerAddressMode::eClampToEdge,
                                         .addressModeV = vk::SamplerAddressMode::eClampToEdge,
                                         .addressModeW = vk::SamplerAddressMode::eClampToEdge,
                                         .maxLod = VK_LOD_CLAMP_NONE});
    if (s.result != vk::Result::eSuccess) {
        return false;
    }
    sampler = std::move(s.value);
    const auto make = [&](vk::DescriptorType second, u32 push_size, std::string_view code,
                          vk::UniqueDescriptorSetLayout& set, vk::UniquePipelineLayout& layout,
                          vk::UniquePipeline& pipeline) {
        const std::array<vk::DescriptorSetLayoutBinding, 2> bindings{{
            {0, vk::DescriptorType::eCombinedImageSampler, 1, vk::ShaderStageFlagBits::eCompute},
            {1, second, 1, vk::ShaderStageFlagBits::eCompute},
        }};
        auto d = device.createDescriptorSetLayoutUnique(
            {.flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
             .bindingCount = static_cast<u32>(bindings.size()),
             .pBindings = bindings.data()});
        if (d.result != vk::Result::eSuccess) {
            return false;
        }
        set = std::move(d.value);
        const vk::PushConstantRange push{vk::ShaderStageFlagBits::eCompute, 0, push_size};
        auto l = device.createPipelineLayoutUnique({.setLayoutCount = 1,
                                                    .pSetLayouts = &*set,
                                                    .pushConstantRangeCount = 1,
                                                    .pPushConstantRanges = &push});
        if (l.result != vk::Result::eSuccess) {
            return false;
        }
        layout = std::move(l.value);
        const auto module = Vulkan::Compile(code, vk::ShaderStageFlagBits::eCompute, device);
        if (!module) {
            return false;
        }
        auto p = device.createComputePipelineUnique(
            {}, {.stage = {.stage = vk::ShaderStageFlagBits::eCompute,
                           .module = module,
                           .pName = "main"},
                 .layout = *layout});
        device.destroyShaderModule(module);
        if (p.result != vk::Result::eSuccess) {
            return false;
        }
        pipeline = std::move(p.value);
        return true;
    };
    if (!make(vk::DescriptorType::eStorageBuffer, sizeof(EncodePush),
              HostShaders::VR_SKY_ENCODE_COMP, encode_set, encode_layout, encode_pipeline)) {
        LOG_WARNING(Render_Vulkan, "VR sky detail: pipelines failed; off");
        return false;
    }
    if (instance.GetPhysicalDevice().getProperties().limits.timestampPeriod > 0.0f) {
        auto q = device.createQueryPoolUnique(
            {.queryType = vk::QueryType::eTimestamp, .queryCount = QueryCount});
        if (q.result == vk::Result::eSuccess) {
            timestamps = std::move(q.value);
        }
    }
    return true;
}

bool Builder::AfterUpload(Image& image) {
    if (!VR::GetSettings().sky_detail || failed) {
        return false;
    }
    // From the first upload on: the game's folders are mounted by then, and its first sky is
    // seconds away (a save loading).
    StartCatalog();
    const Entry* entry = Find(image, not_ready);
    if (not_ready == 1) {
        ++not_ready; // logged once
        LOG_INFO(Render_Vulkan, "VR sky detail: textures uploaded before the catalog was read "
                                "get no copy");
    }
    const bool had = image.sky_twin != nullptr;
    if (!entry || unusable.contains(entry)) {
        if (had) {
            LOG_INFO(Render_Vulkan, "VR sky detail: {:#x} no longer holds {}: copy dropped",
                     image.info.guest_address, image.sky_twin->name);
            image.sky_twin.reset();
        }
        return had;
    }
    if (had && image.sky_twin->name == entry->name) {
        return false; // the same texture uploaded again
    }
    image.sky_twin.reset();
    if (!tried) {
        tried = true;
        failed = !Init();
        if (failed) {
            return had;
        }
    }
    // Your image, once read. A worker thread reads it; until then the game's own texture is
    // sampled (a stand-in with no view, waiting for Upgrade), never a filtered copy.
    const auto ready = loaded.find(entry);
    if (ready == loaded.end()) {
        Request(*entry);
        auto wait = std::make_shared<Twin>(instance, scheduler);
        wait->name = entry->name;
        wait->entry = entry;
        wait->waiting = true;
        image.sky_twin = std::move(wait);
        return had;
    }
    return Build(image, *entry, ready->second) || had;
}

void Builder::Request(const Entry& entry) {
    if (loading.contains(&entry)) {
        return;
    }
    loading.emplace(&entry,
                    std::async(std::launch::async, [&entry, allocator = instance.GetAllocator()] {
                        Common::SetCurrentThreadName("VR sky image");
                        return ReadImage(entry.image, entry.width, entry.height, allocator);
                    }));
}

bool Builder::Poll() {
    bool done = false;
    for (auto it = loading.begin(); it != loading.end();) {
        if (it->second.wait_for(std::chrono::seconds(0)) != std::future_status::ready) {
            ++it;
            continue;
        }
        if (auto image = it->second.get()) {
            loaded.emplace(it->first, std::move(image));
        } else {
            unusable.insert(it->first);
        }
        it = loading.erase(it);
        done = true;
    }
    return done;
}

bool Builder::Upgrade(Image& image) {
    const auto& twin = image.sky_twin;
    if (!twin || !twin->waiting || !image.backing) {
        return false;
    }
    const auto ready = loaded.find(twin->entry);
    if (ready == loaded.end()) {
        if (unusable.contains(twin->entry)) {
            image.sky_twin.reset(); // unreadable: the game's own texture for good
        }
        return false; // else still reading
    }
    // The copy from your image in place of the game's texture.
    if (!Build(image, *twin->entry, ready->second)) {
        image.sky_twin.reset();
        return false;
    }
    return true;
}

void Builder::EndUpgrades() {
    loaded.clear(); // the copies hold the pixels now; a later upload reads the file again
}

bool Builder::Build(Image& image, const Entry& entry,
                    const std::shared_ptr<const Replacement>& own) {
    const auto cpu_start = std::chrono::steady_clock::now();
    const auto device = instance.GetDevice();
    const std::string& name = entry.name;
    // The copy is your image (an AI upscale) and its mips, encoded as they are.
    const u32 w = image.info.size.width, h = image.info.size.height;
    const u32 scale = own->width / w;
    const u32 shift = static_cast<u32>(std::countr_zero(scale));
    const u32 out_w = w * scale, out_h = h * scale;
    const u32 full_chain = static_cast<u32>(std::bit_width(std::max(out_w, out_h)));
    const u32 levels = std::min(image.info.resources.levels + shift, full_chain);
    // `clean`: your image at the copy's size (its level k is the copy's k).
    const vk::Format clean_format = vk::Format::eR8G8B8A8Unorm;
    const vk::Format src_format = image.backing->image.image_ci.format;
    const bool srgb = IsSrgb(src_format);

    auto twin = std::make_shared<Twin>(instance, scheduler);
    twin->format = srgb ? vk::Format::eBc7SrgbBlock : vk::Format::eBc7UnormBlock;
    twin->shift = shift;
    twin->levels = levels;
    twin->name = name;
    twin->entry = &entry;
    if (!twin->image.TryCreate({
            .flags = vk::ImageCreateFlagBits::eMutableFormat,
            .imageType = vk::ImageType::e2D,
            .format = twin->format,
            .extent = {out_w, out_h, 1},
            .mipLevels = levels,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        })) {
        LOG_WARNING(Render_Vulkan, "VR sky detail: no memory for {}'s {}x{} copy", name, out_w,
                    out_h);
        return false;
    }
    UniqueImage clean{device, instance.GetAllocator()};
    if (!clean.TryCreate({
            .imageType = vk::ImageType::e2D,
            .format = clean_format,
            .extent = {out_w, out_h, 1},
            .mipLevels = levels,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferSrc |
                     vk::ImageUsageFlagBits::eTransferDst,
            .initialLayout = vk::ImageLayout::eUndefined,
        })) {
        return false;
    }
    // The copy's levels: blocks per row and column, first block in the buffer.
    struct Level {
        u32 w, h, bx, by, first;
    };
    std::vector<Level> layout;
    u32 total = 0;
    for (u32 m = 0; m < levels; ++m) {
        const u32 lw = std::max(out_w >> m, 1u), lh = std::max(out_h >> m, 1u);
        const Level l{lw, lh, Common::DivCeil(lw, 4u), Common::DivCeil(lh, 4u), total};
        total += l.bx * l.by;
        layout.push_back(l);
    }
    const vk::DeviceSize total_bytes = vk::DeviceSize{total} * 16;
    auto blocks = std::make_unique<UniqueBuffer>(device, instance.GetAllocator());
    blocks->Create({.size = total_bytes,
                    .usage = vk::BufferUsageFlagBits::eStorageBuffer |
                             vk::BufferUsageFlagBits::eTransferSrc},
                   MemoryUsage::DeviceLocal, nullptr);

    const auto make_view = [&](vk::Image img, vk::Format format, u32 level, u32 count,
                               vk::ImageUsageFlags usage) {
        const vk::ImageViewUsageCreateInfo usage_ci{.usage = usage};
        auto [result, view] = device.createImageViewUnique({
            .pNext = &usage_ci,
            .image = img,
            .viewType = vk::ImageViewType::e2D,
            .format = format,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, level, count, 0, 1},
        });
        return result == vk::Result::eSuccess ? std::move(view) : vk::UniqueImageView{};
    };
    auto clean_read =
        make_view(clean.image, clean_format, 0, levels, vk::ImageUsageFlagBits::eSampled);
    if (!clean_read) {
        return false;
    }

    scheduler.EndRendering();
    const auto cmd = scheduler.Commands();
    u32 query = ~0u;
    if (timestamps) {
        query = next_query;
        next_query = (next_query + 2) % QueryCount;
        cmd.resetQueryPool(*timestamps, query, 2);
        cmd.writeTimestamp2(vk::PipelineStageFlagBits2::eAllCommands, *timestamps, query);
    }
    const auto barrier = [&](vk::Image img, vk::ImageLayout from, vk::ImageLayout to,
                             vk::PipelineStageFlags2 src_stage, vk::AccessFlags2 src_access,
                             vk::PipelineStageFlags2 dst_stage, vk::AccessFlags2 dst_access,
                             u32 level, u32 count) {
        const vk::ImageMemoryBarrier2 b{
            .srcStageMask = src_stage,
            .srcAccessMask = src_access,
            .dstStageMask = dst_stage,
            .dstAccessMask = dst_access,
            .oldLayout = from,
            .newLayout = to,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = img,
            .subresourceRange = {vk::ImageAspectFlagBits::eColor, level, count, 0, 1},
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.imageMemoryBarrierCount = 1,
                                                .pImageMemoryBarriers = &b});
    };
    using Stage = vk::PipelineStageFlagBits2;
    using Access = vk::AccessFlagBits2;
    using Layout = vk::ImageLayout;

    // Pass 1: your image copied into `clean` level 0. `first`: level 0's layout, stage and
    // access after it.
    const Layout first = Layout::eTransferDstOptimal;
    const vk::PipelineStageFlags2 first_stage = Stage::eTransfer;
    const vk::AccessFlags2 first_access = Access::eTransferWrite;
    barrier(clean.image, Layout::eUndefined, first, Stage::eNone, Access::eNone, Stage::eTransfer,
            Access::eTransferWrite, 0, levels);
    {
        const vk::BufferImageCopy copy{
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, 0, 0, 1},
            .imageExtent = {out_w, out_h, 1},
        };
        cmd.copyBufferToImage(vk::Buffer{own->buffer}, clean.image, first, copy);
    }

    // Its mips (2x2 averages), then all of it read by pass 2.
    if (levels > 1) {
        barrier(clean.image, first, Layout::eTransferSrcOptimal, first_stage, first_access,
                Stage::eTransfer, Access::eTransferRead, 0, 1);
        barrier(clean.image, first, Layout::eTransferDstOptimal, Stage::eNone, Access::eNone,
                Stage::eTransfer, Access::eTransferWrite, 1, levels - 1);
        for (u32 m = 1; m < levels; ++m) {
            const auto size = [&](u32 level) {
                return vk::Offset3D{static_cast<s32>(std::max(out_w >> level, 1u)),
                                    static_cast<s32>(std::max(out_h >> level, 1u)), 1};
            };
            const vk::ImageBlit blit{
                .srcSubresource = {vk::ImageAspectFlagBits::eColor, m - 1, 0, 1},
                .srcOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0}, size(m - 1)},
                .dstSubresource = {vk::ImageAspectFlagBits::eColor, m, 0, 1},
                .dstOffsets = std::array<vk::Offset3D, 2>{vk::Offset3D{0, 0, 0}, size(m)},
            };
            cmd.blitImage(clean.image, Layout::eTransferSrcOptimal, clean.image,
                          Layout::eTransferDstOptimal, blit, vk::Filter::eLinear);
            barrier(clean.image, Layout::eTransferDstOptimal, Layout::eTransferSrcOptimal,
                    Stage::eTransfer, Access::eTransferWrite, Stage::eTransfer,
                    Access::eTransferRead, m, 1);
        }
        barrier(clean.image, Layout::eTransferSrcOptimal, Layout::eShaderReadOnlyOptimal,
                Stage::eTransfer, Access::eTransferWrite | Access::eTransferRead,
                Stage::eComputeShader, Access::eShaderSampledRead, 0, levels);
    } else {
        barrier(clean.image, first, Layout::eShaderReadOnlyOptimal, first_stage, first_access,
                Stage::eComputeShader, Access::eShaderSampledRead, 0, 1);
    }

    // Pass 2: every level of the copy as BC7 blocks.
    cmd.bindPipeline(vk::PipelineBindPoint::eCompute, *encode_pipeline);
    {
        const vk::DescriptorImageInfo src{*sampler, *clean_read, Layout::eShaderReadOnlyOptimal};
        const vk::DescriptorBufferInfo dst{*blocks, 0, total_bytes};
        const std::array<vk::WriteDescriptorSet, 2> writes{{
            {.dstBinding = 0,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eCombinedImageSampler,
             .pImageInfo = &src},
            {.dstBinding = 1,
             .descriptorCount = 1,
             .descriptorType = vk::DescriptorType::eStorageBuffer,
             .pBufferInfo = &dst},
        }};
        cmd.pushDescriptorSetKHR(vk::PipelineBindPoint::eCompute, *encode_layout, 0, writes);
    }
    for (u32 m = 0; m < levels; ++m) {
        const auto& l = layout[m];
        const u32 src_level = m;
        const EncodePush push{
            .src_size = {static_cast<s32>(std::max(out_w >> src_level, 1u)),
                         static_cast<s32>(std::max(out_h >> src_level, 1u))},
            .dst_size = {static_cast<s32>(l.w), static_cast<s32>(l.h)},
            .src_level = src_level,
            .first_block = l.first,
            .blocks_x = l.bx,
            .blocks_y = l.by,
        };
        cmd.pushConstants(*encode_layout, vk::ShaderStageFlagBits::eCompute, 0, sizeof(push),
                          &push);
        cmd.dispatch(Common::DivCeil(l.bx, 8u), Common::DivCeil(l.by, 8u), 1);
    }
    // Our pipelines and push constants replaced the game's: its next dispatch sets them.
    ScratchPush::Invalidate();

    // The blocks into the copy, which then stays sampled for good.
    {
        const vk::BufferMemoryBarrier2 b{
            .srcStageMask = Stage::eComputeShader,
            .srcAccessMask = Access::eShaderStorageWrite,
            .dstStageMask = Stage::eTransfer,
            .dstAccessMask = Access::eTransferRead,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .buffer = *blocks,
            .offset = 0,
            .size = VK_WHOLE_SIZE,
        };
        cmd.pipelineBarrier2(vk::DependencyInfo{.bufferMemoryBarrierCount = 1,
                                                .pBufferMemoryBarriers = &b});
    }
    barrier(twin->image.image, Layout::eUndefined, Layout::eTransferDstOptimal, Stage::eNone,
            Access::eNone, Stage::eTransfer, Access::eTransferWrite, 0, levels);
    std::vector<vk::BufferImageCopy> copies;
    for (u32 m = 0; m < levels; ++m) {
        copies.push_back({
            .bufferOffset = vk::DeviceSize{layout[m].first} * 16,
            .imageSubresource = {vk::ImageAspectFlagBits::eColor, m, 0, 1},
            .imageExtent = {layout[m].w, layout[m].h, 1},
        });
    }
    cmd.copyBufferToImage(*blocks, twin->image.image, Layout::eTransferDstOptimal, copies);
    barrier(twin->image.image, Layout::eTransferDstOptimal, Layout::eShaderReadOnlyOptimal,
            Stage::eTransfer, Access::eTransferWrite,
            Stage::eAllGraphics | Stage::eComputeShader, Access::eShaderSampledRead, 0, levels);
    if (query != ~0u) {
        cmd.writeTimestamp2(Stage::eAllCommands, *timestamps, query + 1);
    }

    // VR_SKY_DUMP=1: the top level and the level at the game's size, read back.
    std::unique_ptr<UniqueBuffer> readback;
    u8* readback_data = nullptr;
    if (g_dump) {
        const vk::DeviceSize top = vk::DeviceSize{layout[0].bx} * layout[0].by * 16;
        const vk::DeviceSize same = vk::DeviceSize{layout[shift].bx} * layout[shift].by * 16;
        VmaAllocationInfo alloc{};
        readback = std::make_unique<UniqueBuffer>(device, instance.GetAllocator());
        readback->Create({.size = top + same, .usage = vk::BufferUsageFlagBits::eTransferDst},
                         MemoryUsage::Download, &alloc);
        readback_data = static_cast<u8*>(alloc.pMappedData);
        const std::array<vk::BufferCopy, 2> regions{{
            {0, 0, top},
            {vk::DeviceSize{layout[shift].first} * 16, top, same},
        }};
        cmd.copyBuffer(*blocks, *readback, regions);
    }

    const vk::DeviceSize twin_bytes = total_bytes;
    ++made;
    bytes += twin_bytes;
    const auto cpu_us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - cpu_start)
                            .count();
    const float period = instance.GetPhysicalDevice().getProperties().limits.timestampPeriod;
    scheduler.DeferOperation(
        [device, name, w, h, out_w, out_h, levels, twin_bytes, cpu_us, period, query,
         top_level = layout[0], same_level = layout[shift], made = made, total = bytes,
         pool = timestamps ? *timestamps : vk::QueryPool{}, c = std::move(clean),
         b = std::move(blocks), v = std::move(clean_read), r = std::move(readback), s = own,
         readback_data]() mutable {
            double gpu_ms = -1.0;
            if (pool && query != ~0u) {
                std::array<u64, 2> stamps{};
                if (device.getQueryPoolResults(pool, query, 2, sizeof(stamps), stamps.data(),
                                               sizeof(u64), vk::QueryResultFlagBits::e64) ==
                    vk::Result::eSuccess) {
                    gpu_ms = static_cast<double>(stamps[1] - stamps[0]) * period / 1e6;
                }
            }
            LOG_INFO(Render_Vulkan,
                     "VR sky detail: {} {}x{} -> {}x{} BC7 (your image), {} levels, {:.1f} MiB "
                     "(copies {}, {:.1f} MiB), GPU {:.2f} ms, CPU {} us",
                     name, w, h, out_w, out_h, levels, twin_bytes / 1048576.0, made,
                     total / 1048576.0, gpu_ms, cpu_us);
            if (readback_data) {
                const u64 top = u64{top_level.bx} * top_level.by * 16;
                WritePng(fmt::format("sky_{}_x{}", name, out_w / w), readback_data, top_level.w,
                         top_level.h);
                WritePng(fmt::format("sky_{}_x1", name), readback_data + top, same_level.w,
                         same_level.h);
            }
            v.reset();
            c.Destroy();
            b.reset();
            r.reset();
            s.reset();
        });
    image.sky_twin = std::move(twin);
    return true;
}

} // namespace VideoCore::SkyDetail
