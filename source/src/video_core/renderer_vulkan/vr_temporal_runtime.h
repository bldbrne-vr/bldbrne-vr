// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <memory>
#include <future>
#include <string>
#include "video_core/renderer_vulkan/vr_temporal_pair.h"
namespace Vulkan {
class Instance;
class Scheduler;
class VrFsr4Runtime;
// Vulkan adapter for the tested stereo transaction. The scene owner supplies canonical,
// stable single-eye resources and publishes the result only when Resolve returns Ready.
class VrTemporalRuntime {
public:
    VrTemporalRuntime(const Instance& instance, Scheduler& scheduler);
    ~VrTemporalRuntime();
    VrTemporalPair::Result Begin(const VR::Graphics::Config& accepted,
                                 const VrTemporalPair::Plan& plan);
    VrTemporalPair::Result Resolve(uint64_t frame_id, const VrTemporalPair::Frame& frame);
    bool Preparing() const { return initialization.valid(); }
    bool CanProduceMotion() const { return !initialization.valid() && pair.CanProduceMotion(); }
    uint64_t Epoch() const { return epoch; }
    void ResetHistory() { if (!initialization.valid()) pair.ResetHistory(); }
    // FAST_UPSCALER_VERIFY: the FSR twin has followed every left-eye evaluation so far.
    bool VerifyTwinValid() const { return !initialization.valid() && pair.VerifyTwinValid(); }
    // FAST bit 45: the last resolve recorded its FSR eyes interleaved.
    // Like VerifyTwinValid, never reads `pair` while the initialization worker owns it.
    bool FsrPairInterleaved() const {
        const auto* paired = initialization.valid() ? nullptr : pair.FsrPairResult();
        return paired && paired->interleaved;
    }
    // FAST_DLSS_DIRECT_VERIFY: the DLSS twin ran in step with its eye in the last Resolve.
    bool DlssTwinCurrent() const { return !initialization.valid() && pair.DlssTwinCurrent(); }
    const std::string& Problem() const { return problem; }
private:
    const Instance& instance;
    Scheduler& scheduler;
    std::unique_ptr<VrFsr4Runtime> fsr;
    VrTemporalPair pair;
    using Result = VrTemporalPair::Result;
    struct InitializationResult { Result result; std::string problem; };
    InitializationResult Initialize(const VR::Graphics::Config&, const VrTemporalPair::Plan&,
                                    uint64_t ticket, uint64_t completed);
    Result BeginPair(const VR::Graphics::Config&, const VrTemporalPair::Plan&,
                     VkCommandBuffer, uint64_t ticket, uint64_t completed);
    std::future<InitializationResult> initialization;
    VR::Graphics::Config initialization_config{};
    VrTemporalPair::Plan initialization_plan{};
    bool initialization_failed{};
    uint64_t epoch{1};
    bool touched{};
    std::string problem;
    std::wstring fsr_assets;
};
} // namespace Vulkan
