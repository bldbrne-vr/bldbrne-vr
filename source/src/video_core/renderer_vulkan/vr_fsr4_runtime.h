// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
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
    explicit VrFsr4Runtime(VR::Graphics::Upscaler provider = VR::Graphics::Upscaler::Fsr4)
        : provider(provider) {}
    VR::Graphics::Upscaler Provider() const { return provider; }
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
    const VR::Graphics::Upscaler provider;
    void* module{};
    const VrFsr4Api* api{};
    std::unique_ptr<VrFsr4Session> session;
    std::filesystem::path assets;
    std::string problem;
    bool attempted{}, optimized_post{}, fenced_spd{};
};
} // namespace Vulkan
