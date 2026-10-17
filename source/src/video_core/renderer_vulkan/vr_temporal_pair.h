// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include "video_core/renderer_vulkan/vr_dlss_session.h"
#include "video_core/renderer_vulkan/vr_fsr4_session.h"

namespace Vulkan {
// One render owner, both eyes as one transaction. Real completion tickets only; no
// sleeps, GPU waits or allocations in the steady-state path. Image ownership is external.
class VrTemporalPair {
public:
    enum class Result { Disabled, Invalid, Unavailable, Pending, Ready, Failed };
    struct Size {
        uint32_t input_width{}, input_height{}, output_width{}, output_height{};
        bool operator==(const Size&) const = default;
    };
    // Provider options carried by the plan, so a change retires and recreates the pair.
    enum ProviderFlag : uint32_t {
        Fsr4OptimizedPost = 1, // bbport's shared-memory post pass, when installed
        Fsr4VerifyTwin = 2,    // plus a left-eye twin with the original post (diagnostic)
        Fsr4VerifySame = 4,    // the twin runs the eyes' own post pass instead (A/A control)
        Fsr4FencedSpd = 8,     // fenced SPD auto-exposure for every context, when installed
        // FAST_FSR4_INTERLEAVE_VERIFY (bit 45): a left-eye twin with the eyes' own options,
        // evaluated alone after the pair (diagnostic). Replaces Fsr4VerifyTwin's twin. 64: 4 and 8 are bit
        // 37's Fsr4VerifySame and Fsr4FencedSpd, 16 and 32 the DLSS twin's (bit 46).
        Fsr4VerifyInterleave = 64,
        // FAST_DLSS_DIRECT_VERIFY (bit 46): a DLSS twin (bridge view 2) of the left eye, or of the
        // right one with DlssVerifyTwinRight, fed that eye's canonical copies (diagnostic).
        // Kept clear of the low bits other FSR options take (4, 8 in the w5 FSR ports):
        // VrTemporalImages::Allocate tests DlssVerifyTwin without knowing the provider.
        DlssVerifyTwin = 16,
        DlssVerifyTwinRight = 32,
    };
    static uint32_t DlssTwinEye(uint32_t provider_flags) {
        return (provider_flags & DlssVerifyTwinRight) != 0 ? 1u : 0u;
    }
    struct Plan {
        std::array<Size, 2> eyes{};
        bool inverted{};
        uint32_t provider_flags{}; // ProviderFlag
        // DLSS model (vr_dlss_preset.h, FAST bit 52); 0 = the bridge's own. DLSS only.
        uint32_t dlss_preset{};
        bool operator==(const Plan&) const = default;
    };
    struct Device { VkDevice logical{}; VkPhysicalDevice physical{}; };
    struct FsrActivation {
        VrFsr4Session* session{};
        const wchar_t* assets{};
        bool optimized_post{}; // opt/*_post.spv and their manifest are installed
        bool fenced_spd{};     // opt/spd_auto_exposure.spv too, and the bridge takes its flag
    };
    struct Frame {
        std::array<BbDlssEvaluate, 2> eyes{}; // RGBA16F/R32F/RG16F/RGBA16F, GENERAL
        std::array<float, 2> vertical_fov{};
        float near_plane{}, far_plane{};
        // Fsr4VerifyTwin/DlssVerifyTwin: the twin's output (its eye's size, RGBA16F, GENERAL);
        // else empty.
        BbDlssImage verify_output{};
        // FAST bit 45: evaluate both FSR eyes through the bridge's lockstep entry when it has
        // one (identical output, interleaved dispatches). Read every frame; no recreation.
        bool fsr4_pair{};
        // DlssVerifyTwin: canonical colour/depth of verify_eye and the twin's own motion vectors.
        BbDlssEvaluate verify_inputs{};
        uint32_t verify_eye{};
        // Bit 46: the eyes' colour/depth are the scene's own layer views (layer = eye, RGBA16F
        // colour, D32/D32S8 depth aspect, GENERAL) instead of canonical copies. DLSS only.
        bool direct_inputs{};
    };
    // Activators are called only on a new accepted configuration, after the previous pair
    // retires. OFF never calls either. The runtime owns sessions and outlives this object.
    template<class ActivateDlss, class ActivateFsr>
    Result Begin(const VR::Graphics::Config& accepted, const Plan& plan, const Device& device,
                 VkCommandBuffer command, uint64_t ticket, uint64_t completed,
                 ActivateDlss&& activate_dlss, ActivateFsr&& activate_fsr) {
        Complete(completed);
        const auto next = MakeKey(accepted, plan);
        if (!configured || next != key) {
            ready = false;
            if (!Retire(completed)) return Result::Pending;
            dlss = nullptr;
            fsr = nullptr;
            key = next;
            configured = true;
            failed = false;
            reset = true;
            last_frame = 0;
            seen_frame = false;
            fsr_post_state = 0;
            fsr_spd_state = 0;
            dlss_twin_state = 0;
            dlss_twin_current = false;
            if (key.provider == VR::Graphics::Upscaler::Off) return Result::Disabled;
            if (!Valid(plan) || uint32_t(key.profile) > 2 || !device.logical || !device.physical ||
                !command || ticket <= completed) {
                failed = true;
                return Result::Invalid;
            }
            if (key.provider == VR::Graphics::Upscaler::Dlss) {
                dlss = activate_dlss();
                if (!dlss) { failed = true; return Result::Unavailable; }
                dlss->Completed(completed_tick);
                const auto feature = [&](unsigned eye) {
                    const auto& s = plan.eyes[eye];
                    // Native input/output is DLAA. Reduced input uses the selected SR profile.
                    const int quality = s.input_width == s.output_width &&
                                                s.input_height == s.output_height
                                            ? 0 : 3 - int(key.profile);
                    return BbDlssFeature{s.input_width, s.input_height, s.output_width,
                                         s.output_height, quality, int(plan.inverted), 0, 1};
                };
                for (unsigned eye = 0; eye < 2; ++eye) {
                    if (dlss->Create(eye, command, feature(eye), ticket) != VrDlssSession::Result::Ready) {
                        failed = true;
                        return Result::Failed;
                    }
                }
                // Diagnostic only: a bridge without a third view refuses the twin and the pair
                // runs unverified. DLSS 5's chain has no twin.
                if ((plan.provider_flags & DlssVerifyTwin) && !key.neural)
                    dlss_twin_state =
                        dlss->CreateTwin(command, feature(DlssTwinEye(plan.provider_flags)), ticket)
                            ? 1u : 2u;
            } else if (key.provider == VR::Graphics::Upscaler::Fsr4) {
                const auto activated = activate_fsr();
                fsr = activated.session;
                if (!fsr) { failed = true; return Result::Unavailable; }
                fsr->Completed(completed_tick);
                const bool requested = (plan.provider_flags & Fsr4OptimizedPost) != 0;
                const bool post = requested && activated.optimized_post && fsr->SupportsFlags();
                const bool fence_requested = (plan.provider_flags & Fsr4FencedSpd) != 0;
                const bool fence =
                    fence_requested && activated.fenced_spd && fsr->SupportsFencedSpd();
                const uint32_t flags = (post ? VR_FSR4_FLAG_OPTIMIZED_POST : 0u) |
                                       (fence ? VR_FSR4_FLAG_FENCED_SPD : 0u);
                fsr_post_state = post ? 1u : requested ? 2u : 0u;
                fsr_spd_state = fence ? 2u : fence_requested ? 3u : 1u;
                const auto describe = [&](unsigned eye, uint32_t desc_flags) {
                    const auto& s = plan.eyes[eye];
                    const bool native = s.input_width == s.output_width &&
                                        s.input_height == s.output_height;
                    return VrFsr4Desc{s.output_width, s.output_height,
                                      native ? 3u : uint32_t(key.profile), uint32_t(plan.inverted),
                                      desc_flags};
                };
                for (unsigned eye = 0; eye < 2; ++eye) {
                    if (fsr->Create(eye, device.logical, device.physical, activated.assets,
                                    describe(eye, flags)) != VrFsr4Session::Result::Ready) {
                        failed = true;
                        return Result::Failed;
                    }
                }
                // Diagnostic only: a failed twin leaves the pair usable, without verification.
                // It always shares the eyes' SPD pass, so only the post pass can differ. Bit
                // 45's twin has the eyes' own options (A/A) and takes the place of bit 37's.
                const bool same = (plan.provider_flags & Fsr4VerifySame) != 0;
                const uint32_t twin_flags = same ? flags : flags & ~VR_FSR4_FLAG_OPTIMIZED_POST;
                if (plan.provider_flags & Fsr4VerifyInterleave)
                    fsr->CreateTwin(device.logical, device.physical, activated.assets,
                                    describe(0, flags));
                else if ((post || same) && (plan.provider_flags & Fsr4VerifyTwin))
                    fsr->CreateTwin(device.logical, device.physical, activated.assets,
                                    describe(0, twin_flags));
            } else { failed = true; return Result::Invalid; }
        }
        if (key.provider == VR::Graphics::Upscaler::Off) return Result::Disabled;
        if (failed) return Result::Failed;
        ready = dlss ? dlss->CanProduceMotion() : fsr && fsr->CanProduceMotion();
        return ready ? Result::Ready : Result::Pending;
    }
    bool NeedsConfiguration(const VR::Graphics::Config& c, const Plan& p) const {
        return !configured || MakeKey(c, p) != key;
    }
    bool CanProduceMotion() const { return ready && !failed; }
    uint64_t Epoch() const { return epoch; }
    // The caller publishes output only on Ready. A failure/pending right eye cannot expose
    // an updated left eye alongside stale right-eye history. Reset both on the next attempt.
    Result Evaluate(uint64_t frame_id, VkCommandBuffer command, uint64_t ticket, const Frame& frame) {
        fsr_pair_used = false;
        dlss_twin_current = false;
        if (key.provider == VR::Graphics::Upscaler::Off) return Result::Disabled;
        if (!CanProduceMotion()) return failed ? Result::Failed : Result::Pending;
        if (!command || ticket <= completed_tick || (seen_frame && frame_id <= last_frame) ||
            !Validate(frame)) { reset = true; return Result::Invalid; }
        const bool restart = reset || !seen_frame || frame_id != last_frame + 1;
        last_frame = frame_id;
        seen_frame = true;
        reset = true;
        if (fsr && frame.fsr4_pair && fsr->SupportsPair()) {
            // FAST bit 45: the same two eye frames as the loop below, recorded by the bridge in
            // lockstep; the twin follows the left eye whenever it recorded, as in the loop.
            const auto image = [](const BbDlssImage& i) {
                return VrFsr4Image{i.image, i.view, i.width, i.height};
            };
            std::array<VrFsr4Frame, 2> frames{};
            for (unsigned eye = 0; eye < 2; ++eye) {
                auto f = frame.eyes[eye];
                f.reset = f.reset || restart;
                frames[eye] = VrFsr4Frame{command, image(f.color), image(f.depth), image(f.motion),
                                          image(f.output), f.jitter_x, f.jitter_y, f.frame_ms,
                                          frame.near_plane, frame.far_plane, frame.vertical_fov[eye],
                                          uint32_t(f.reset), ticket};
            }
            fsr_pair_used = true;
            const auto result = fsr->EvaluatePair(frames, fsr_pair);
            if (fsr_pair.eye[0] == 1 && frame.verify_output.image && fsr->TwinValid()) {
                auto twin = frames[0];
                twin.output = image(frame.verify_output);
                fsr->EvaluateTwin(twin);
            }
            if (result == VrFsr4Session::Result::Pending) return Result::Pending;
            if (result != VrFsr4Session::Result::Ready) {
                failed = true; ready = false; return Result::Failed;
            }
            reset = false;
            return Result::Ready;
        }
        for (unsigned eye = 0; eye < 2; ++eye) {
            auto f = frame.eyes[eye];
            f.reset = f.reset || restart;
            if (dlss) {
                const auto result = dlss->Evaluate(eye, command, f, ticket);
                if (result == VrDlssSession::Result::Pending) return Result::Pending;
                if (result != VrDlssSession::Result::Ready) {
                    failed = true; ready = false; return Result::Failed;
                }
                // Verification twin: this eye again on the old path's inputs, same jitter, reset
                // and frame time. Its failure or a missed frame only pauses verification.
                if (eye == frame.verify_eye && dlss->HasTwin()) {
                    if (frame.verify_output.image && frame.verify_inputs.color.image) {
                        auto twin = f;
                        twin.color = frame.verify_inputs.color;
                        twin.depth = frame.verify_inputs.depth;
                        twin.motion = frame.verify_inputs.motion;
                        twin.output = frame.verify_output;
                        const auto twin_result = dlss->EvaluateTwin(command, twin, ticket);
                        if (twin_result == VrDlssSession::Result::Failed)
                            dlss_twin_state = 3; // the counter line says twin=failed
                        dlss_twin_current = twin_result == VrDlssSession::Result::Ready &&
                                            dlss->TwinSynced();
                    } else {
                        dlss->MissTwin();
                    }
                }
            } else {
                const auto image = [](const BbDlssImage& i) {
                    return VrFsr4Image{i.image, i.view, i.width, i.height};
                };
                const VrFsr4Frame fsr_frame{command, image(f.color), image(f.depth), image(f.motion),
                                            image(f.output), f.jitter_x, f.jitter_y, f.frame_ms,
                                            frame.near_plane, frame.far_plane, frame.vertical_fov[eye],
                                            uint32_t(f.reset), ticket};
                const auto result = fsr->Evaluate(eye, fsr_frame);
                if (result == VrFsr4Session::Result::Pending) return Result::Pending;
                if (result != VrFsr4Session::Result::Ready) {
                    failed = true; ready = false; return Result::Failed;
                }
                // Verification twin: identical left-eye inputs, original post pass. Its
                // failure or skipped frame only ends verification, never the pair.
                if (eye == 0 && frame.verify_output.image && fsr->TwinValid()) {
                    auto twin = fsr_frame;
                    twin.output = image(frame.verify_output);
                    fsr->EvaluateTwin(twin);
                }
            }
        }
        reset = false;
        return Result::Ready;
    }
    // 0 original FSR post pass, 1 optimized, 2 optimized requested but not installed.
    uint32_t FsrPostState() const { return fsr_post_state; }
    // 0 no FSR context, 1 original SPD pass, 2 fenced, 3 fenced requested but not installed.
    uint32_t FsrSpdState() const { return fsr_spd_state; }
    // FAST bit 45: 0 no FSR session, 1 per-eye bridge only (no pair export), 2 lockstep entry.
    uint32_t FsrPairSupport() const { return !fsr ? 0u : fsr->SupportsPair() ? 2u : 1u; }
    // What the bridge's lockstep entry did in the last Evaluate, or null if it was not used.
    const VrFsr4PairResult* FsrPairResult() const { return fsr_pair_used ? &fsr_pair : nullptr; }
    bool VerifyTwinValid() const { return fsr && fsr->TwinValid(); }
    // DlssVerifyTwin: 0 none, 1 created, 2 refused, 3 failed after creation (verification
    // stopped; the eyes go on); and whether the last Evaluate ran the twin
    // in step with its eye (its output is then comparable).
    uint32_t DlssTwinState() const { return dlss_twin_state; }
    bool DlssTwinCurrent() const { return dlss && dlss_twin_current; }
    void RefreshCompletion(uint64_t completed) { Complete(completed); }
    uint64_t ReuseTicket() const { return fsr ? fsr->ReuseTicket() : 0; }
    void ResetHistory() { reset = true; }
    bool Retire(uint64_t completed) {
        configured = false;
        ready = false;
        if (dlss) dlss->Disable();
        if (fsr) fsr->Disable();
        Complete(completed);
        const bool d = !dlss || dlss->Retire();
        const bool f = !fsr || fsr->Retire();
        if (!d || !f) return false;
        if (dlss || fsr) ++epoch;
        dlss = nullptr;
        fsr = nullptr;
        return true;
    }
private:
    struct Key {
        VR::Graphics::Upscaler provider{VR::Graphics::Upscaler::Off};
        VR::Graphics::Profile profile{VR::Graphics::Profile::Quality};
        Plan plan{};
        uint64_t neural{}; // Accepted neural settings participate in pair retirement/recreation.
        bool operator==(const Key&) const = default;
    } key;
    static Key MakeKey(const VR::Graphics::Config& c, const Plan& p) {
        Key next{c.upscaler, VR::Graphics::EffectiveProfile(c), p};
        if (c.upscaler == VR::Graphics::Upscaler::Dlss && c.dlss5)
            next.neural = VR::Graphics::Pack(c) & ~uint64_t(1023);
        return c.upscaler == VR::Graphics::Upscaler::Off ? Key{} : next;
    }
    static bool Valid(const Plan& plan) {
        for (const auto& s : plan.eyes)
            if (!s.input_width || !s.input_height || s.input_width > s.output_width ||
                s.input_height > s.output_height || s.output_width > 16384 ||
                s.output_height > 16384) return false;
        return true;
    }
    bool Validate(const Frame& frame) const {
        const auto valid = [](const BbDlssImage& i, VkFormat format, uint32_t w, uint32_t h) {
            return i.image && i.view && i.format == format && i.width == w && i.height == h &&
                   i.range.aspectMask == VK_IMAGE_ASPECT_COLOR_BIT && i.range.baseMipLevel == 0 &&
                   i.range.levelCount == 1 && i.range.baseArrayLayer == 0 && i.range.layerCount == 1;
        };
        // Bit 46 scene views go to plain DLSS only: FSR's backend assumes COLOR aspect, layer 0,
        // and DLSS 5's chain hands the depth input on to its own feature.
        if (frame.direct_inputs && (!dlss || key.neural)) return false;
        for (unsigned eye = 0; eye < 2; ++eye) {
            const auto& f = frame.eyes[eye];
            const auto& size = key.plan.eyes[eye];
            // A direct input is this eye's array layer of the scene, in a format whose values
            // the canonical copy kept exactly: RGBA16F colour, D32 depth aspect; or, for an input
            // FAST_DLSS_DIRECT_LAYER_COPY keeps copied (the right eye's depth by default), its
            // canonical image.
            const auto layer = [eye](const BbDlssImage& i, VkImageAspectFlags aspect, uint32_t w,
                                     uint32_t h) {
                return i.image && i.view && i.width == w && i.height == h &&
                       i.range.aspectMask == aspect && i.range.baseMipLevel == 0 &&
                       i.range.levelCount == 1 && i.range.baseArrayLayer == eye &&
                       i.range.layerCount == 1;
            };
            const bool inputs = frame.direct_inputs
                ? ((layer(f.color, VK_IMAGE_ASPECT_COLOR_BIT, size.input_width, size.input_height) &&
                    f.color.format == VK_FORMAT_R16G16B16A16_SFLOAT) ||
                   valid(f.color, VK_FORMAT_R16G16B16A16_SFLOAT, size.input_width, size.input_height)) &&
                  ((layer(f.depth, VK_IMAGE_ASPECT_DEPTH_BIT, size.input_width, size.input_height) &&
                    (f.depth.format == VK_FORMAT_D32_SFLOAT ||
                     f.depth.format == VK_FORMAT_D32_SFLOAT_S8_UINT)) ||
                   valid(f.depth, VK_FORMAT_R32_SFLOAT, size.input_width, size.input_height))
                : valid(f.color, VK_FORMAT_R16G16B16A16_SFLOAT, size.input_width, size.input_height) &&
                      valid(f.depth, VK_FORMAT_R32_SFLOAT, size.input_width, size.input_height);
            if (!inputs ||
                !valid(f.motion, VK_FORMAT_R16G16_SFLOAT, size.input_width, size.input_height) ||
                !valid(f.output, VK_FORMAT_R16G16B16A16_SFLOAT, size.output_width, size.output_height) ||
                !std::isfinite(f.jitter_x) || !std::isfinite(f.jitter_y) ||
                !std::isfinite(f.frame_ms) || f.frame_ms <= 0 ||
                !std::isfinite(f.sharpness) || f.sharpness < 0 || f.sharpness > 1) return false;
            if (fsr && (!std::isfinite(frame.near_plane) || frame.near_plane <= 0 ||
                        !std::isfinite(frame.far_plane) || frame.far_plane <= frame.near_plane ||
                        !std::isfinite(frame.vertical_fov[eye]) || frame.vertical_fov[eye] <= 0 ||
                        frame.vertical_fov[eye] >= 3.141593f)) return false;
        }
        // Canonical single-layer inputs and outputs belong to one eye only; disallow aliasing
        // writes with either eye's inputs, and any accidental shared motion/depth/color history.
        for (unsigned i = 0; i < 2; ++i) for (unsigned j = 0; j < 2; ++j) {
            const auto& out = frame.eyes[i].output;
            const auto& other = frame.eyes[j];
            if (out.image == other.color.image || out.image == other.depth.image ||
                out.image == other.motion.image || (i != j && out.image == other.output.image))
                return false;
        }
        if (const auto& verify = frame.verify_output; verify.image) {
            if (frame.verify_eye > 1) return false;
            const auto& size = key.plan.eyes[frame.verify_eye];
            if (!verify.view || verify.format != VK_FORMAT_R16G16B16A16_SFLOAT ||
                verify.width != size.output_width || verify.height != size.output_height)
                return false;
            for (const auto& other : frame.eyes)
                if (verify.image == other.color.image || verify.image == other.depth.image ||
                    verify.image == other.motion.image || verify.image == other.output.image)
                    return false;
            // DlssVerifyTwin inputs: canonical single-layer images that no output aliases.
            if (const auto& twin = frame.verify_inputs; twin.color.image) {
                if (!valid(twin.color, VK_FORMAT_R16G16B16A16_SFLOAT, size.input_width, size.input_height) ||
                    !valid(twin.depth, VK_FORMAT_R32_SFLOAT, size.input_width, size.input_height) ||
                    !valid(twin.motion, VK_FORMAT_R16G16_SFLOAT, size.input_width, size.input_height))
                    return false;
                for (const auto* input : {&twin.color, &twin.depth, &twin.motion}) {
                    if (input->image == verify.image) return false;
                    for (const auto& other : frame.eyes)
                        if (input->image == other.output.image) return false;
                }
                for (const auto& other : frame.eyes)
                    if (twin.motion.image == other.motion.image) return false;
            }
        }
        if (frame.direct_inputs) {
            // One scene image per kind for both eyes: distinct layers and views instead.
            const auto distinct = [](const BbDlssImage& a, const BbDlssImage& b) {
                return a.view != b.view &&
                       (a.image != b.image || a.range.baseArrayLayer != b.range.baseArrayLayer);
            };
            return distinct(frame.eyes[0].color, frame.eyes[1].color) &&
                   distinct(frame.eyes[0].depth, frame.eyes[1].depth) &&
                   frame.eyes[0].motion.image != frame.eyes[1].motion.image;
        }
        return frame.eyes[0].color.image != frame.eyes[1].color.image &&
               frame.eyes[0].depth.image != frame.eyes[1].depth.image &&
               frame.eyes[0].motion.image != frame.eyes[1].motion.image;
    }
    void Complete(uint64_t completed) {
        completed_tick = std::max(completed_tick, completed);
        if (dlss) dlss->Completed(completed_tick);
        if (fsr) fsr->Completed(completed_tick);
    }
    VrDlssSession* dlss{};
    VrFsr4Session* fsr{};
    uint64_t completed_tick{}, last_frame{}, epoch{1};
    uint32_t fsr_post_state{}, fsr_spd_state{}, dlss_twin_state{};
    VrFsr4PairResult fsr_pair{};
    bool fsr_pair_used{};
    bool configured{}, ready{}, failed{}, reset{true}, seen_frame{}, dlss_twin_current{};
};
} // namespace Vulkan
