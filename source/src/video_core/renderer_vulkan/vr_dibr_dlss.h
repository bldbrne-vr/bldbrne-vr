// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <cstdlib>
#include <memory>
#include "core/vr/graphics_settings.h"
#include "video_core/renderer_vulkan/vk_common.h"

namespace Vulkan::DibrDlss {
// Explicit opt-in while this path is being validated. Zero disables it completely;
// native stereo never enters it. Keep DIBR-specific policy out of the shared provider.
inline bool Enabled() {
    const char* value = std::getenv("VR_DIBR_DLSS");
    return !std::getenv("VR_NATIVE_STEREO") && value && value[0] == '1' && value[1] == '\0';
}
inline bool Supports(const VR::Graphics::Config& config) {
    return config.upscaler == VR::Graphics::Upscaler::Dlss && !config.dlss5;
}
inline bool Requested() { return Enabled() && Supports(VR::Graphics::Accepted()); }
} // namespace Vulkan::DibrDlss

namespace VR { struct CameraRecord; }
namespace VideoCore { struct Image; }
namespace Vulkan {
class Instance;
class Scheduler;
// Experimental single-center reconstruction. No allocations or provider activation when off.
// Camera motion only: this is a memory experiment, not a moving-object quality certification.
class VrDibrDlss {
public:
    VrDibrDlss(const Instance&, Scheduler&);
    ~VrDibrDlss();
    struct Output { vk::Image image{}; vk::Extent2D extent{}; };
    bool Ready() const;
    vk::Extent2D DesiredSize(vk::Extent2D input) const;
    Output Record(VideoCore::Image& color, VideoCore::Image& depth,
                  vk::ComponentMapping components, const VR::CameraRecord& camera,
                  float xsign, float ysign, bool already_resolved);
private:
    struct State;
    std::unique_ptr<State> state;
};
}
