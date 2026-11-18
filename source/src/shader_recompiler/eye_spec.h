// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// VR_EYE_SPEC (native stereo; on by default since 2026-11-07, the user's go): eye-shared buffer
// variants.
// Every storage buffer of a native stereo graphics stage is an array of two descriptors, the left
// and the right eye's, and each read picks one by the view index. The driver cannot know that
// most of them hold the same descriptor twice (85% of a draw's buffer pairs: the right eye reads
// the left eye's buffer), and the dynamic index costs: on an RTX 5080 at the user's settings
// constant indices took 0.36 ms off the scene's 3.47 a frame, 0.23 of it outside the cameras and
// lights (2026-11-07).
//
// The shader side (EmitContext::NativeBufferPointer): the eye index of buffer binding b is
// select(marker_b == 1, 0, eye), marker_b a constant made a specialization constant (SpecId
// SpecBase + b) after the module is assembled (PatchEyeSpec). Its default value is never 1, so
// the module behaves exactly as before; a pipeline specialized with marker_b = 1 reads slot 0
// for both eyes, which is the same descriptor when the draw's two are equal. A marker the patch
// cannot prove is read only by its own compares stays a plain constant (that binding keeps its
// dynamic index whatever the specialization says).
//
// The host side (vk_pipeline_common.cpp, GraphicsPipeline::EyeHandle, vr_eye_spec_variants.h):
// after pairing, a draw's mask is the buffer bindings whose two descriptors are equal; a hot
// pipeline gets a variant with the bindings shared over its first draws, built in the background;
// a draw binds it only when its own mask holds every binding the variant shares.
//   unset or 1     on: bindings whose two descriptors are equal (+3.2% fps at the user's
//                  settings on an RTX 5080, GPU -0.25 ms a frame, eye captures within noise);
//   VR_EYE_SPEC=2  also camera/light pairs by stride (built and checked, but measured no faster
//                  than 1 on an RTX 5080, 2026-11-07: the eyes' reads still differ per lane, and
//                  the stride and range check cost about what the descriptor saved);
//   VR_EYE_SPEC=0  off (modules as before, no variants).

#include <cstdlib>
#include "common/types.h"

namespace Shader::EyeSpec {

inline char Mode() {
    static const char mode = [] {
        const char* v = std::getenv("VR_EYE_SPEC");
        if (!v || !v[0]) {
            return '1';
        }
        return v[0] == '0' ? '0' : v[0] == '2' ? '2' : '1';
    }();
    return mode;
}
inline bool Enabled() {
    return Mode() != '0';
}
/// VR_EYE_SPEC=2: variants also read camera/light pairs by stride.
inline bool Strides() {
    return Mode() == '2';
}

/// SpecId of buffer binding b's shared flag: SpecBase + b. 1: both eyes read descriptor 0.
constexpr u32 SpecBase = 0x4E00;
/// Read-only bindings also take a stride and a size (bytes, SpecBaseDelta / SpecBaseSize + b):
/// with the flag set and a stride, descriptor 0 covers both eyes' copies of a pair made in one
/// allocation (cameras, lights: right = left + stride) and an eye reads index + eye * stride, an
/// index at or past the size reading out of range (0, as the eye's own descriptor gives). Both 0
/// for a binding whose two descriptors are equal.
constexpr u32 SpecBaseDelta = 0x4E40;
constexpr u32 SpecBaseSize = 0x4E80;
/// The markers' values for binding b: Magic + b, MagicDelta + b, MagicSize + b (NaN bits as
/// floats; any other use of the same value in a module makes the patch leave that binding alone).
/// Unpatched, the flag is never 1: the binding reads as it always did.
constexpr u32 Magic = 0x7FBA0000;
constexpr u32 MagicDelta = 0x7FBB0000;
constexpr u32 MagicSize = 0x7FBC0000;
/// The bindings a mask can name (u64 masks).
constexpr u32 MaxBindings = 64;

/// VR_EXP_EYE_INDEX / VR_EXP_CULL (GPU timing experiments, never defaults; some modes draw a
/// wrong image on purpose) change the modules they make. Mixed into every cached binary's word
/// (ShaderBinaryVersionOf): a session with one neither loads an ordinary session's modules nor
/// leaves its own for one, and each mode keeps its own. 0 with neither set. Its mode and cull bits
/// fall in the word's revision fields (pipeline_cache_policy.h, since 2026-11-16): an experiment
/// session after a revision bump may, for one particular other mode, read the modules an earlier
/// one wrote. Timing experiments only (never a default, never a player's session); a full
/// separation would need a wider word (a cache format change).
inline u32 ExperimentWord() {
    static const u32 word = [] {
        const char* index = std::getenv("VR_EXP_EYE_INDEX");
        const char* cull = std::getenv("VR_EXP_CULL");
        const u32 mode = index && index[0] ? u32(index[0]) & 0x7Fu : 0u;
        const u32 culled = cull && cull[0] == '1' ? 1u : 0u;
        return mode != 0 || culled != 0 ? 0x4000'0000u | mode << 16 | culled << 23 : 0u;
    }();
    return word;
}

} // namespace Shader::EyeSpec
