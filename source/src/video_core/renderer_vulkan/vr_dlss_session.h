// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include "core/vr/graphics_settings.h"
#include "video_core/renderer_vulkan/vr_dlss_bridge.h"

namespace Vulkan {
// Single renderer owner. Tickets belong to its scheduler timeline. No waits, allocation,
// Vulkan submissions or settings reads in Evaluate. Image ownership remains with the caller.
class VrDlssSession {
public:
    struct Device {
        VkInstance instance{};
        VkPhysicalDevice physical{};
        VkDevice logical{};
        PFN_vkGetInstanceProcAddr instance_proc{};
        PFN_vkGetDeviceProcAddr device_proc{};
    };
    enum class Result { Disabled, Unavailable, Pending, Ready, Failed };
    explicit VrDlssSession(const BbDlssApi* bridge) : api(bridge) {}
    VrDlssSession(const VrDlssSession&) = delete;
    VrDlssSession& operator=(const VrDlssSession&) = delete;
    ~VrDlssSession() {
        assert(!initialized && "Retire and shut down DLSS before its device");
    }

    // Called at a frame boundary with ACCEPTED settings, never the menu draft.
    Result Activate(const VR::Graphics::Config& accepted, const Device& device, bool eligible) {
        requested = accepted.upscaler == VR::Graphics::Upscaler::Dlss;
        neural = requested && accepted.dlss5; // the pair's key includes it: a change re-activates
        if (!requested)
            return Result::Disabled;
        supported = eligible;
        if (!eligible || !api || api->abi != BBPORT_DLSS_BRIDGE_ABI || !api->Initialize ||
            !api->CreateFeature || !api->Evaluate || !api->ReleaseFeature || !api->Shutdown ||
            !device.instance || !device.physical || !device.logical || !device.instance_proc ||
            !device.device_proc)
            return Result::Unavailable;
        if (failed)
            return Result::Failed;
        if (!initialized) {
            if (!api->Initialize(device.instance, device.physical, device.logical,
                                 device.instance_proc, device.device_proc)) {
                failed = true;
                return Result::Failed;
            }
            initialized = true;
        }
        return Result::Ready;
    }
    void Disable() {
        requested = false;
    }
    void Completed(uint64_t ticket) {
        if (ticket > completed)
            completed = ticket;
    }
    bool Initialized() const {
        return initialized;
    }
    bool Active() const {
        return requested && supported && initialized && !failed;
    }
    bool HasFeatures(unsigned views = 2) const {
        return (views == 1 || views == 2) && eyes[0].created &&
               (views == 1 || eyes[1].created);
    }
    bool CanProduceMotion(unsigned views = 2) const {
        return Active() && HasFeatures(views) && completed >= eyes[0].creation &&
               (views == 1 || completed >= eyes[1].creation);
    }
    // No feature replacement while commands using it are in flight. Reconfiguration is
    // transactional at the frame owner: retire BOTH eyes, then create both new features.
    Result Create(unsigned eye, VkCommandBuffer command, const BbDlssFeature& feature,
                  uint64_t ticket) {
        if (!Active())
            return Result::Disabled;
        if (eye >= eyes.size() || !command || ticket <= completed || eyes[eye].created ||
            !feature.input_width || !feature.input_height || !feature.output_width ||
            !feature.output_height)
            return Result::Failed;
        eyes[eye].last_use = ticket; // Creation failures can still record uploads.
        // S1 (FAST bit 51, boot-time): the input is the tonemapped frame after the composite.
        // DLSS 5 keeps HDR + auto exposure: its chain composes in linear light (compose.comp), so
        // S1 feeds it X decoded through sRGB views (linear 0..1) and encodes its output again
        // (vk_rasterizer_temporal.cpp VrS1Reconstruct).
        BbDlssFeature created = feature;
        if (VR::Graphics::S1Output().width && !neural)
            created.hdr = 0;
        if (!api->CreateFeature(eye, command, &created)) {
            failed = true;
            return Result::Failed;
        }
        eyes[eye] = {true, ticket, ticket};
        return Result::Ready;
    }
    Result Evaluate(unsigned eye, VkCommandBuffer command, const BbDlssEvaluate& frame,
                    uint64_t ticket) {
        if (!Active())
            return Result::Disabled;
        if (eye >= eyes.size() || !eyes[eye].created || !command || ticket <= completed ||
            ticket < eyes[eye].last_use)
            return Result::Failed;
        if (completed < eyes[eye].creation)
            return Result::Pending;
        eyes[eye].last_use = ticket; // Failure may still have recorded commands.
        if (!api->Evaluate(eye, command, &frame)) {
            failed = true;
            return Result::Failed;
        }
        return Result::Ready;
    }
    // FAST_DLSS_DIRECT_VERIFY (bit 46, vr_dlss_direct.h): bridge view 2, a twin of one eye fed
    // the old path's inputs. Diagnostic only: its failures never mark the session failed or touch
    // the eyes, and a bridge built with two views refuses view 2 without recording anything.
    static constexpr unsigned TwinView = 2;
    bool CreateTwin(VkCommandBuffer command, const BbDlssFeature& feature, uint64_t ticket) {
        // A failed attempt can still own GPU work. Preserve its ticket until Retire,
        // and do not disable an existing twin when rejecting a duplicate request.
        if (!Active() || twin.last_use || !command || ticket <= completed)
            return false;
        twin_valid = twin_synced = false;
        twin.last_use = ticket; // Creation failures can still record uploads.
        if (!api->CreateFeature(TwinView, command, &feature))
            return false;
        twin = {true, ticket, ticket};
        twin_valid = true;
        return true;
    }
    bool HasTwin() const {
        return twin_valid && twin.created && Active();
    }
    // The twin has seen every evaluation of its eye since both last restarted their history.
    bool TwinSynced() const {
        return HasTwin() && twin_synced;
    }
    // Its eye was evaluated without it: out of step until the next restart of both.
    void MissTwin() {
        twin_synced = false;
    }
    Result EvaluateTwin(VkCommandBuffer command, const BbDlssEvaluate& frame, uint64_t ticket) {
        if (!HasTwin())
            return Result::Disabled;
        if (!command || ticket <= completed || ticket < twin.last_use ||
            completed < twin.creation) {
            twin_synced = false;
            return Result::Pending;
        }
        twin.last_use = ticket; // Failure may still have recorded commands.
        if (!api->Evaluate(TwinView, command, &frame)) {
            twin_valid = twin_synced = false;
            return Result::Failed;
        }
        if (frame.reset) // Both histories start again here.
            twin_synced = true;
        return Result::Ready;
    }
    bool Retire() {
        // Do not partly release a stereo pair.
        for (const auto& eye : eyes)
            if (completed < eye.last_use)
                return false;
        if (completed < twin.last_use)
            return false;
        for (unsigned i = 0; i < eyes.size(); ++i) {
            if (eyes[i].last_use) // Also retire partially created features after failed uploads.
                api->ReleaseFeature(i);
            eyes[i] = {};
        }
        if (twin.last_use)
            api->ReleaseFeature(TwinView);
        twin = {};
        twin_valid = twin_synced = false;
        failed = false; // Owner retries only after a new accepted configuration.
        return true;
    }
    bool Shutdown() {
        Disable();
        if (!Retire())
            return false;
        if (initialized)
            api->Shutdown();
        initialized = false;
        failed = false;
        return true;
    }

private:
    struct Eye {
        bool created{};
        uint64_t creation{}, last_use{};
    };
    const BbDlssApi* api{};
    std::array<Eye, 2> eyes{};
    Eye twin{}; // TwinView, FAST_DLSS_DIRECT_VERIFY only
    uint64_t completed{};
    bool requested{}, supported{}, initialized{}, failed{}, twin_valid{}, twin_synced{};
    bool neural{}; // DLSS 5 (accepted.dlss5) at the last Activate
};
} // namespace Vulkan
