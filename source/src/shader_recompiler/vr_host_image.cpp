// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

// VR host images (resource.h): the T#s the rasterizer captured for shaders that read an image of
// another draw. Read by GetSharp on the recording thread, by preparation workers and by pipeline
// compiles (their specialization), so a lock: a T# is 32 bytes.

#include <array>
#include <cstring>
#include <mutex>

#include "shader_recompiler/resource.h"

namespace Shader {
namespace {

std::mutex g_mutex;
std::array<AmdGpu::Image, static_cast<size_t>(VrHostImage::Count)> g_images{};
std::array<AmdGpu::Buffer, static_cast<size_t>(VrHostBuffer::Count)> g_buffers{};

} // namespace

AmdGpu::Buffer GetVrHostBuffer(VrHostBuffer slot) {
    const auto index = static_cast<size_t>(slot);
    if (index >= g_buffers.size()) {
        return {};
    }
    std::scoped_lock lock{g_mutex};
    return g_buffers[index];
}

bool SetVrHostBuffer(VrHostBuffer slot, const AmdGpu::Buffer& buffer) {
    const auto index = static_cast<size_t>(slot);
    if (index >= g_buffers.size()) {
        return false;
    }
    std::scoped_lock lock{g_mutex};
    if (std::memcmp(&g_buffers[index], &buffer, sizeof(buffer)) == 0) {
        return false;
    }
    g_buffers[index] = buffer;
    return true;
}

AmdGpu::Image GetVrHostImage(VrHostImage slot) {
    const auto index = static_cast<size_t>(slot);
    if (index >= g_images.size()) {
        return {};
    }
    std::scoped_lock lock{g_mutex};
    return g_images[index];
}

bool SetVrHostImage(VrHostImage slot, const AmdGpu::Image& image) {
    const auto index = static_cast<size_t>(slot);
    if (index >= g_images.size()) {
        return false;
    }
    std::scoped_lock lock{g_mutex};
    if (std::memcmp(&g_images[index], &image, sizeof(image)) == 0) {
        return false;
    }
    g_images[index] = image;
    return true;
}

} // namespace Shader
