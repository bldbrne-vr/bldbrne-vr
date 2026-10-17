// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <filesystem>
#include <memory>
#include <string>
#include "video_core/renderer_vulkan/vk_common.h"
#include "video_core/renderer_vulkan/vr_fsr4_session.h"
namespace Vulkan {
class Instance;
class VrFsr4Runtime {
public:
    VrFsr4Runtime() = default;
    ~VrFsr4Runtime();
    VrFsr4Runtime(const VrFsr4Runtime&) = delete;
    VrFsr4Runtime& operator=(const VrFsr4Runtime&) = delete;
    VrFsr4Session* Activate(const VR::Graphics::Config& accepted, const Instance& instance);
    const std::filesystem::path& Assets() const {
        return assets;
    }
    /// bbport's shared-memory post passes (opt/post-lds.json, prepare_fsr4_post_lds.py) are
    /// installed and the bridge accepts provider flags. Checked once, when the bridge loads.
    bool OptimizedPostInstalled() const {
        return optimized_post;
    }
    /// The fenced SPD auto-exposure pass (opt/spd_auto_exposure.spv, packaged with the post
    /// manifest) is installed and the bridge accepts VR_FSR4_FLAG_FENCED_SPD.
    bool FencedSpdInstalled() const {
        return fenced_spd;
    }
    const std::string& Problem() const {
        return problem;
    }
    bool Shutdown(uint64_t completed);

private:
    bool Load();
    void* module{};
    const VrFsr4Api* api{};
    std::unique_ptr<VrFsr4Session> session;
    std::filesystem::path assets;
    std::string problem;
    bool attempted{}, optimized_post{}, fenced_spd{};
};
} // namespace Vulkan
