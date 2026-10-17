// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <array>
#include <cassert>
#include <cstdint>
#include "core/vr/graphics_settings.h"
#include "video_core/renderer_vulkan/vr_fsr4_bridge.h"
namespace Vulkan {
class VrFsr4Session {
public:
    enum class Result { Disabled, Unavailable, Pending, Ready, Failed };
    // bridge_flags: what an ABI 2 bridge accepts (its VrFsr4GetStereoFlags export; a bridge
    // without one knows only VR_FSR4_FLAG_OPTIMIZED_POST). `pair_bridge`: the bridge's optional
    // lockstep entry (VR_FSR4_PAIR_EXPORT, FAST bit 45).
    explicit VrFsr4Session(const VrFsr4Api* bridge,
                           uint32_t bridge_flags = VR_FSR4_FLAG_OPTIMIZED_POST,
                           const VrFsr4PairApi* pair_bridge = nullptr)
        : api(bridge), offered(bridge_flags), pair_api(pair_bridge) {}
    ~VrFsr4Session() {
        assert(!eyes[0].handle && !eyes[1].handle && !twin.handle);
    }
    VrFsr4Session(const VrFsr4Session&) = delete;
    VrFsr4Session& operator=(const VrFsr4Session&) = delete;
    Result Activate(const VR::Graphics::Config& accepted, bool supported) {
        requested = accepted.upscaler == VR::Graphics::Upscaler::Fsr4;
        profile = uint32_t(accepted.fsr4);
        // An ABI 1 bridge reads the first four VrFsr4Desc members and has no flags.
        eligible = supported && api && (api->abi == VR_FSR4_ABI || api->abi == VR_FSR4_ABI_V1) &&
                   api->Create && api->Evaluate && api->Completed && api->Release && api->Problem;
        if (!requested)
            return Result::Disabled;
        if (!eligible)
            return Result::Unavailable;
        return failed ? Result::Failed : Result::Ready;
    }
    void Disable() {
        requested = false;
    }
    bool Active() const {
        return requested && eligible && !failed;
    }
    bool SupportsFlags() const {
        return api && api->abi >= 2;
    }
    bool SupportsFencedSpd() const {
        return SupportsFlags() && (offered & VR_FSR4_FLAG_FENCED_SPD) != 0;
    }
    bool CanProduceMotion() const {
        return Active() && eyes[0].handle && eyes[1].handle && eyes[0].profile == profile &&
               eyes[1].profile == profile;
    }
    Result Create(unsigned eye, VkDevice device, VkPhysicalDevice physical, const wchar_t* assets,
                  const VrFsr4Desc& desc) {
        if (!requested)
            return Result::Disabled;
        if (!Active())
            return Result::Unavailable;
        if (eye >= eyes.size() || eyes[eye].handle || !device || !physical || !assets ||
            (desc.profile != profile && desc.profile != 3))
            return Result::Failed;
        auto* handle = Make(device, physical, assets, desc);
        if (!handle) {
            failed = true;
            return Result::Failed;
        }
        eyes[eye] = {handle, 0, profile};
        return Result::Ready;
    }
    Result Evaluate(unsigned eye, const VrFsr4Frame& frame) {
        if (!requested)
            return Result::Disabled;
        if (!CanProduceMotion())
            return Result::Unavailable;
        if (eye >= eyes.size() || frame.ticket <= completed || frame.ticket < eyes[eye].last_use)
            return Result::Failed;
        const auto result = api->Evaluate(eyes[eye].handle, &frame);
        if (result == 2)
            return Result::Pending; // No commands recorded; retain the previous last-use.
        eyes[eye].last_use = frame.ticket;
        if (result != 1) {
            failed = true;
            return Result::Failed;
        }
        Track(eyes[eye], frame.ticket);
        return Result::Ready;
    }
    // FAST bit 45: the bridge records both eyes' dispatches interleaved under shared barriers
    // (only a bridge with the optional VR_FSR4_PAIR_EXPORT entry).
    bool SupportsPair() const {
        return api && pair_api && pair_api->abi == VR_FSR4_PAIR_ABI && pair_api->EvaluatePair;
    }
    // Both eyes through the bridge's lockstep entry. Its per-eye results get exactly Evaluate's
    // bookkeeping, then the combined result follows eye order like Evaluate(0), Evaluate(1):
    // a pending or failed left eye means the right was not recorded.
    Result EvaluatePair(const std::array<VrFsr4Frame, 2>& frames, VrFsr4PairResult& pair) {
        pair = {};
        pair.eye[0] = pair.eye[1] = -1;
        if (!requested)
            return Result::Disabled;
        if (!CanProduceMotion())
            return Result::Unavailable;
        if (!SupportsPair())
            return Result::Failed;
        for (unsigned eye = 0; eye < 2; ++eye)
            if (frames[eye].ticket <= completed || frames[eye].ticket < eyes[eye].last_use)
                return Result::Failed;
        pair_api->EvaluatePair(eyes[0].handle, eyes[1].handle, &frames[0], &frames[1], &pair);
        for (unsigned eye = 0; eye < 2; ++eye) {
            // 1 recorded, 0 failed after possibly recording part of the frame; 2 and -1
            // recorded nothing, so the previous last use stays.
            if (pair.eye[eye] != 0 && pair.eye[eye] != 1)
                continue;
            eyes[eye].last_use = frames[eye].ticket;
            if (pair.eye[eye] == 1)
                Track(eyes[eye], frames[eye].ticket);
        }
        for (unsigned eye = 0; eye < 2; ++eye) {
            if (pair.eye[eye] == 2)
                return Result::Pending;
            if (pair.eye[eye] != 1) {
                failed = true;
                return Result::Failed;
            }
        }
        return Result::Ready;
    }
    // FAST_UPSCALER_VERIFY (bit 37): a left-eye twin with other provider options, fed the same
    // inputs to compare outputs (FAST_FSR4_INTERLEAVE_VERIFY, bit 45: with the eyes' own
    // options, always evaluated alone). Never affects the pair: creation/evaluation failures
    // and skipped frames only invalidate the twin, which is released with the eyes.
    Result CreateTwin(VkDevice device, VkPhysicalDevice physical, const wchar_t* assets,
                      const VrFsr4Desc& desc) {
        twin_valid = false;
        if (!Active() || twin.handle || !device || !physical || !assets ||
            (desc.profile != profile && desc.profile != 3))
            return Result::Failed;
        auto* handle = Make(device, physical, assets, desc);
        if (!handle)
            return Result::Failed;
        twin = {handle, 0, profile};
        twin_valid = true;
        return Result::Ready;
    }
    bool TwinValid() const {
        return twin_valid && twin.handle && CanProduceMotion();
    }
    Result EvaluateTwin(const VrFsr4Frame& frame) {
        if (!TwinValid())
            return Result::Unavailable;
        if (frame.ticket <= completed || frame.ticket < twin.last_use) {
            twin_valid = false;
            return Result::Failed;
        }
        const auto result = api->Evaluate(twin.handle, &frame);
        if (result == 2) {
            twin_valid = false; // a skipped frame makes its history differ from the eye's
            return Result::Pending;
        }
        twin.last_use = frame.ticket;
        if (result != 1) {
            twin_valid = false;
            return Result::Failed;
        }
        Track(twin, frame.ticket);
        return Result::Ready;
    }
    // The Vulkan FSR bridge has three dispatch slots per eye. Wait before
    // recording either eye, so capacity pressure cannot leave a partial pair.
    uint64_t ReuseTicket() const {
        if (!CanProduceMotion()) return 0;
        uint64_t wait = 0;
        const auto oldest_of = [&](const Eye& eye) {
            uint64_t oldest = eye.in_flight[0];
            for (auto ticket : eye.in_flight) if (ticket < oldest) oldest = ticket;
            if (oldest > completed && oldest > wait) wait = oldest;
        };
        for (const auto& eye : eyes)
            oldest_of(eye);
        if (TwinValid())
            oldest_of(twin);
        return wait;
    }
    void Completed(uint64_t ticket) {
        if (ticket > completed)
            completed = ticket;
        for (auto& eye : eyes)
            if (eye.handle)
                api->Completed(eye.handle, completed);
        if (twin.handle)
            api->Completed(twin.handle, completed);
    }
    bool Retire() {
        for (const auto& eye : eyes)
            if (eye.last_use > completed)
                return false;
        if (twin.last_use > completed)
            return false;
        for (auto& eye : eyes)
            if (eye.handle) {
                if (!api->Release(eye.handle))
                    return false;
                eye = {};
            }
        if (twin.handle) {
            if (!api->Release(twin.handle))
                return false;
            twin = {};
        }
        twin_valid = false;
        failed = false;
        return true;
    }
    const char* Problem() const {
        if (!api || !api->Problem)
            return "FSR bridge unavailable";
        for (const auto& eye : eyes)
            if (eye.handle) {
                const char* text = api->Problem(eye.handle);
                if (text && *text)
                    return text;
            }
        return api->Problem(nullptr);
    }

private:
    struct Eye {
        void* handle{};
        uint64_t last_use{};
        uint32_t profile{};
        std::array<uint64_t, 3> in_flight{};
        uint32_t next_slot{};
    };
    void* Make(VkDevice device, VkPhysicalDevice physical, const wchar_t* assets,
               const VrFsr4Desc& desc) const {
        VrFsr4Desc copy = desc;
        // A bridge refuses flags it does not know (ABI 1 knows none).
        copy.flags = SupportsFlags() ? copy.flags & offered : 0u;
        return api->Create(device, physical, assets, &copy);
    }
    static void Track(Eye& eye, uint64_t ticket) {
        eye.in_flight[eye.next_slot] = ticket;
        eye.next_slot = (eye.next_slot + 1) % eye.in_flight.size();
    }
    const VrFsr4Api* api{};
    uint32_t offered{}; // VR_FSR4_FLAG_* the bridge accepts (ABI 2)
    const VrFsr4PairApi* pair_api{};
    std::array<Eye, 2> eyes{};
    Eye twin{};
    uint64_t completed{};
    uint32_t profile{};
    bool requested{}, eligible{}, failed{}, twin_valid{};
};
} // namespace Vulkan
