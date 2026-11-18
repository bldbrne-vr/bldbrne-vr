// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// VR_DISPATCH_MONO=d (diagnostic, off by default): which native-stereo game dispatches do the same
// work in both eyes. Native stereo records every direct dispatch twice (vr_hud 0 and 1); in a
// compute shader the eye index only selects descriptor 0 or 1 of each pair (EmitContext::
// NativeEyeIndex, NativeBufferPointer / NativeImagePointer), so a dispatch whose pairs name the
// same objects in both entries computes the same thing twice. Classified from the paired writes
// Pipeline::BindResources leaves in set_writes (VR_NATIVE_SWAP_WRITES, on by default):
//   same_all     every pair (read and written) identical: the right run repeats the left one;
//   same_inputs  every read pair identical, a written buffer with a right-eye copy: both outputs
//                hold the same bytes, the right run only fills the right copy;
//   per_eye      a read buffer or image differs between the eyes (the camera, eye targets);
//   storage_image every pair identical and a storage image written through one view in both eyes
//                (both runs write the same image; equal only without reads of what it writes);
//   raw_memory, other: not considered.
// Every 8192 dispatches: `VR dispatch mono:` with the shares and the most frequent shaders.

#include <algorithm>
#include <array>
#include <cstdlib>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>
#include "common/logging/log.h"
#include "common/types.h"
#include "shader_recompiler/info.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::DispatchMono {

inline char Mode() {
    static const char mode = [] {
        const char* v = std::getenv("VR_DISPATCH_MONO");
        return v && v[0] == 'd' ? 'd' : '0';
    }();
    return mode;
}

enum class Class : u32 { SameAll, SameInputs, PerEye, StorageImage, RawMemory, Other, Count };
inline constexpr std::array<const char*, static_cast<size_t>(Class::Count)> ClassNames{
    "same_all", "same_inputs", "per_eye", "storage_image", "raw_memory", "other"};

inline bool SameBuffer(const vk::DescriptorBufferInfo& a, const vk::DescriptorBufferInfo& b) {
    return a.buffer == b.buffer && a.offset == b.offset && a.range == b.range;
}

/// `paired`: the dispatch's native-stereo writes ([left, right] per buffer and image descriptor);
/// `writable`: Pipeline::NativeWritableBindings, indexed by dstBinding.
inline Class Classify(const Shader::Info& info, std::span<const u8> writable,
                      std::span<const vk::WriteDescriptorSet> paired) {
    if (info.uses_dma) {
        return Class::RawMemory;
    }
    for (const auto& buffer : info.buffers) {
        if (buffer.buffer_type != Shader::BufferType::Guest &&
            buffer.buffer_type != Shader::BufferType::Flatbuf &&
            buffer.buffer_type != Shader::BufferType::SharedMemory) {
            return Class::Other;
        }
    }
    bool written_differs = false;
    bool storage_same = false;
    for (const auto& w : paired) {
        switch (w.descriptorType) {
        case vk::DescriptorType::eStorageBuffer:
        case vk::DescriptorType::eUniformBuffer: {
            if (!w.pBufferInfo || w.descriptorCount % 2 != 0) {
                return Class::Other;
            }
            const bool written = w.dstBinding < writable.size() && writable[w.dstBinding];
            for (u32 i = 0; i + 1 < w.descriptorCount; i += 2) {
                if (!SameBuffer(w.pBufferInfo[i], w.pBufferInfo[i + 1])) {
                    if (!written) {
                        return Class::PerEye;
                    }
                    written_differs = true;
                }
            }
            break;
        }
        case vk::DescriptorType::eSampledImage: {
            if (!w.pImageInfo || w.descriptorCount % 2 != 0) {
                return Class::Other;
            }
            for (u32 i = 0; i + 1 < w.descriptorCount; i += 2) {
                if (w.pImageInfo[i].imageView != w.pImageInfo[i + 1].imageView) {
                    return Class::PerEye;
                }
            }
            break;
        }
        case vk::DescriptorType::eSampler:
            break;
        case vk::DescriptorType::eStorageImage: {
            // A layered (native stereo) image has a right view of its own: each eye writes its
            // layer, real per-eye work. One view for both: both runs write the same image.
            if (!w.pImageInfo || w.descriptorCount % 2 != 0) {
                return Class::Other;
            }
            for (u32 i = 0; i + 1 < w.descriptorCount; i += 2) {
                if (w.pImageInfo[i].imageView != w.pImageInfo[i + 1].imageView) {
                    return Class::PerEye;
                }
            }
            storage_same = true;
            break;
        }
        default:
            return Class::Other;
        }
    }
    if (storage_same) {
        return written_differs ? Class::SameInputs : Class::StorageImage;
    }
    return written_differs ? Class::SameInputs : Class::SameAll;
}

/// Per shader: dispatches by class. Stage B's thread only.
class Stats {
public:
    void Note(u64 pgm_hash, u32 groups, Class c) {
        auto& s = shaders[pgm_hash];
        ++s.by_class[static_cast<size_t>(c)];
        s.groups = groups;
        ++totals[static_cast<size_t>(c)];
        if ((++dispatches & 8191) != 0) {
            return;
        }
        const double n = double(dispatches);
        std::string line;
        for (size_t i = 0; i < totals.size(); ++i) {
            line += fmt::format(" {} {:.1f}%", ClassNames[i], 100.0 * double(totals[i]) / n);
        }
        std::vector<std::pair<u64, const Shader*>> order;
        order.reserve(shaders.size());
        for (const auto& [hash, shader] : shaders) {
            order.emplace_back(hash, &shader);
        }
        std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) {
            return a.second->Total() > b.second->Total();
        });
        std::string top;
        for (size_t i = 0; i < order.size() && i < 12; ++i) {
            const auto& s2 = *order[i].second;
            size_t best = 0;
            for (size_t k = 1; k < s2.by_class.size(); ++k) {
                if (s2.by_class[k] > s2.by_class[best]) {
                    best = k;
                }
            }
            top += fmt::format(" | cs {:08x} {:.1f}% {} ({} groups)", order[i].first,
                               100.0 * double(s2.Total()) / n, ClassNames[best], s2.groups);
        }
        LOG_INFO(Render_Vulkan, "VR dispatch mono: {} dispatches:{}{}", dispatches, line, top);
    }

private:
    struct Shader {
        std::array<u64, static_cast<size_t>(Class::Count)> by_class{};
        u32 groups{};
        u64 Total() const {
            u64 t = 0;
            for (const u64 v : by_class) {
                t += v;
            }
            return t;
        }
    };
    std::unordered_map<u64, Shader> shaders;
    std::array<u64, static_cast<size_t>(Class::Count)> totals{};
    u64 dispatches{};
};

inline Stats& Get() {
    static Stats stats;
    return stats;
}

} // namespace Vulkan::DispatchMono
