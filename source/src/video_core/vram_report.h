// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include "video_core/renderer_vulkan/vk_common.h" // the Vulkan defines, then VMA
#include <vk_mem_alloc.h>
#include "common/types.h"
#include <fmt/format.h>
#include "common/path_util.h"

// VR_VRAM_REPORT=1 (diagnostic, off by default): where the GPU memory goes. Allocations are named
// as they are made (texture cache images with size, format, layers and guest address; others by
// what made them, where named), and every 10 s VMA's detailed map of every allocation is written
// to user/log/vram-<n>.json. Memory the
// OpenXR runtime or NGX allocate themselves is not in VMA's map.
namespace VideoCore {
// texture_cache/image.cpp: the images living in memory of their own (VR_VMA_DEDICATED_MB).
u32 DedicatedImages();
} // namespace VideoCore

namespace VideoCore::VramReport {

inline bool Enabled() {
    static const bool on = [] {
        const char* value = std::getenv("VR_VRAM_REPORT");
        return value && *value && *value != '0';
    }();
    return on;
}

inline void Name(VmaAllocator allocator, VmaAllocation allocation, const std::string& name) {
    if (Enabled() && allocator && allocation)
        vmaSetAllocationName(allocator, allocation, name.c_str());
}

// Every 10 s at most; keeps the 60 newest dumps (10 minutes).
inline void MaybeDump(VmaAllocator allocator) {
    if (!Enabled() || !allocator)
        return;
    static auto last = std::chrono::steady_clock::time_point{};
    static unsigned index = 0;
    const auto now = std::chrono::steady_clock::now();
    if (last.time_since_epoch().count() != 0 && now - last < std::chrono::seconds(10))
        return;
    last = now;
    char* text = nullptr;
    vmaBuildStatsString(allocator, &text, VK_TRUE);
    if (!text)
        return;
    try {
        const auto dir = Common::FS::GetUserPath(Common::FS::PathType::LogDir);
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        const auto path = dir / fmt::format("vram-{:02}.json", index++ % 60);
#ifdef _WIN32
        FILE* file = _wfopen(path.c_str(), L"wb");
#else
        FILE* file = std::fopen(path.c_str(), "wb");
#endif
        if (file) {
            std::fputs(text, file);
            std::fclose(file);
        }
    } catch (...) {
    }
    vmaFreeStatsString(allocator, text);
}

} // namespace VideoCore::VramReport
