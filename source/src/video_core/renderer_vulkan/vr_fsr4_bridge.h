// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <vulkan/vulkan.h>
#ifdef __cplusplus
extern "C" {
#endif
// ABI 2 appends VrFsr4Desc::flags; an ABI 1 bridge (no flags) is still accepted by the host,
// which then never requests a flag. A bridge never reads flags from an ABI 1 host: the host
// rejects any ABI it does not know. A bridge refuses flags it does not know: one without the
// VrFsr4GetStereoFlags export knows only VR_FSR4_FLAG_OPTIMIZED_POST, and the export lists
// the rest (still ABI 2, so older hosts keep loading newer bridges).
#define VR_FSR4_ABI 2
#define VR_FSR4_ABI_V1 1
// bbport's shared-memory post pass (fsr4_shaders/opt/*_post.spv, prepare_fsr4_post_lds.py).
#define VR_FSR4_FLAG_OPTIMIZED_POST 1u
// SPD auto-exposure with each tile store fenced before the workgroup counter
// (fsr4_shaders/opt/spd_auto_exposure.spv, prepare_fsr4_post_lds.py).
#define VR_FSR4_FLAG_FENCED_SPD 2u
// Why EvaluatePair recorded the eyes one after the other instead of interleaved.
#define VR_FSR4_PAIR_INTERLEAVED 0u
#define VR_FSR4_PAIR_DESC 1u        // the contexts differ in size, profile, depth or flags
#define VR_FSR4_PAIR_FRAME 2u       // render size, reset, command buffer or ticket differ, or bad input
#define VR_FSR4_PAIR_STATE 3u       // an eye failed earlier or has no free dispatch slot
#define VR_FSR4_PAIR_SHAPE 4u       // the eyes' captured command streams differ in shape
#define VR_FSR4_PAIR_RECORD 5u      // an eye did not record completely (replayed in eye order)
#define VR_FSR4_PAIR_UNSUPPORTED 6u // a captured command cannot be replayed; pairs stay per-eye
#define VR_FSR4_PAIR_REASONS 7u
// Single owner; host resources must remain alive through last submitted use.
typedef struct VrFsr4Image {
    VkImage image;
    VkImageView view;
    uint32_t width, height;
} VrFsr4Image;
typedef struct VrFsr4Desc {
    uint32_t width, height; // output dimensions
    uint32_t profile;       // 0 Performance, 1 Balanced, 2 Quality, 3 Native AA
    uint32_t depth_inverted;
    uint32_t flags;         // ABI 2: VR_FSR4_FLAG_*
} VrFsr4Desc;
typedef struct VrFsr4Frame {
    VkCommandBuffer command;
    VrFsr4Image color, depth, motion, output; // GENERAL, single eye, mip 0
    float jitter_x, jitter_y, frame_ms, near_plane, far_plane, vertical_fov;
    uint32_t reset;
    uint64_t ticket; // the caller's real GPU timeline; strictly newer than completed
} VrFsr4Frame;
// What VrFsr4PairApi::EvaluatePair did.
typedef struct VrFsr4PairResult {
    // Per eye, as Evaluate would return it (1 recorded, 0 failed, 2 pending with nothing
    // recorded), or -1 when the eye was not attempted (the right eye after a left eye that did
    // not record).
    int32_t eye[2];
    uint32_t interleaved;         // 1: the eyes' dispatches were recorded in lockstep
    uint32_t fallback;            // VR_FSR4_PAIR_* when not interleaved
    uint32_t barriers;            // pipeline barriers recorded (captured frames only)
    uint32_t sequential_barriers; // what the per-eye order records for the same work
    uint32_t dispatches;          // dispatches recorded for both eyes (captured frames only)
} VrFsr4PairResult;
typedef struct VrFsr4Api {
    uint32_t abi;
    void* (*Create)(VkDevice, VkPhysicalDevice, const wchar_t* assets, const VrFsr4Desc*);
    // 1 recorded, 0 failed, 2 pending retirement (nothing recorded).
    int32_t (*Evaluate)(void*, const VrFsr4Frame*);
    // No waits: owner reports actual GPU completion, then retries if Evaluate returns 2.
    void (*Completed)(void*, uint64_t);
    int32_t (*Release)(void*); // 0: still in flight; retain pointer and retry later
    const char* (*Problem)(void*);
} VrFsr4Api;
typedef const VrFsr4Api* (*VrFsr4GetApiFn)(void);
// Optional export "VrFsr4GetStereoFlags": every VR_FSR4_FLAG_* the bridge accepts.
typedef uint32_t (*VrFsr4GetFlagsFn)(void);
// FAST bit 45: the lockstep pair entry is a separate, optional export (VR_FSR4_PAIR_EXPORT), so
// VrFsr4Api and VR_FSR4_ABI stay unchanged. An older host loads a bridge that has it and keeps the
// per-eye Evaluate; a host that looks for it treats a bridge without it the same way. Bridges are
// packaged into one shared runtime folder (temporal-staging/local/runtimes/fsr4), so a version
// bump here would make every older exe run against that folder lose FSR.
#define VR_FSR4_PAIR_ABI 1
#define VR_FSR4_PAIR_EXPORT "VrFsr4GetStereoPairApi"
typedef struct VrFsr4PairApi {
    uint32_t abi; // VR_FSR4_PAIR_ABI
    // Both eyes of one frame into one command buffer, with the contract of Evaluate(left)
    // followed, if the left recorded, by Evaluate(right). When the two contexts and frames
    // match, the eyes' dispatches are interleaved under shared barriers; the output is
    // bit-identical to the per-eye order. Returns the right eye's result when it was attempted,
    // otherwise the left eye's.
    int32_t (*EvaluatePair)(void* left, void* right, const VrFsr4Frame* left_frame,
                            const VrFsr4Frame* right_frame, VrFsr4PairResult* result);
} VrFsr4PairApi;
typedef const VrFsr4PairApi* (*VrFsr4GetPairApiFn)(void);
#ifdef __cplusplus
}
#endif
