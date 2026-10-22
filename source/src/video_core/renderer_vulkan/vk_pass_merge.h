// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <array>
#include "common/types.h"

// FAST_GPU_PASS_MERGE (on unless '0'; read once at start-up): fewer render passes for the VR UI
// layer's shadow draws (Rasterizer::VrUiShadowDraw, vk_rasterizer_vr.cpp).
//
// Every UI draw into the composite X is drawn three times: into W and K (the shadow pair, each in
// its own pass: X's state with colour attachment 0 swapped for the target, depth and stencil
// writes masked) and into X. Each draw went W, K, X, and each shadow pass began after an explicit
// end, so a run of n UI draws on one X pass made 3n passes (research_gpu_passes.md 3.1: ~120 of
// the ~277 passes of a frame, ~40 UI draws). With the switch:
//  1 A draw whose X pass is still open (the draw before ended with it) is drawn into X first and
//    into its shadows after, in the other order (K, then W): X's pass goes on. The next draw
//    starts with W, whose pass the last one left open, and ends with X again (Order). Every
//    boundary between two draws then continues a pass: about 2n passes instead of 3n.
//  2 A shadow pass is not ended explicitly when its target is in the attachment layout already
//    (VrUiLayer::TargetBound: TargetView records nothing then) and the shadow state asks for no
//    clear: Scheduler::BeginRendering ends the open pass when the state differs (the same commands
//    as the explicit end), or goes on with it when it is the same pass (bit 43's rules).
//  3 The begin-state memo (bit 2, Rasterizer::BeginRendering; BeginMemo, or PassMemo with
//    FAST_CPU_PASS_MEMO, the same rule in both) asks for X's pass to be open. After
//    a swapped draw the open pass is its shadows' instead, so the next draw would miss it: it also
//    holds while that shadow pass is the one begun right after X's pass ended (Scheduler::
//    PassBegins unchanged; Rasterizer::VrUiMemoResumes). K's and W's passes touch none of X's
//    targets (no transition; the depth/stencil an attachment in its layout, only tested), and the
//    hit's own checks (layouts, dirty images, meta clears, registry epoch, tick) run as always.
// Why the images come out the same, bit for bit: K, W and X are three images, and each still gets
// its draws in the same order with the same state. Drawing X before its own shadows changes only
// the order between the images, which matters only where one of them reads what another writes:
//  - the depth/stencil attachment, which the shadows test and X's draw may write: kept in the old
//    order when X's draw can write it (depth test and write on; stencil test on with a write mask
//    and an op other than KEEP);
//  - a texture the draw samples that is one of its own targets (the shadows would read this draw's
//    output): kept;
//  - memory the shaders write (buffers, images, GDS, DMA, streamout): kept;
//  - clears: a state that asks for any clear is kept, and a shadow pass whose state asks for one is
//    always ended explicitly (a clear asked again in a continued pass would not run);
//  - draws another VR hook records into (the lock-on marker's pair, the HUD snapshot before the
//    first menu draw, the foreground probe, the hunter's split draws, an AA replacement, object
//    motion) and the HUD at S1's output size (bits 58/59: its depth twin may copy, outside a pass)
//    keep the old order; the scaled HUD's shadow passes keep their explicit ends too.
// Continuing a pass is bit 43's case (vk_pass_breaks.h): vkCmdEndRendering stores, the next begin
// loads the same views, so going on gives the same memory.
//
// FAST_GPU_PASS_MERGE_VERIFY=1: every swap is decided a second way (the depth/stencil writes from
// the guest registers instead of the committed dynamic state, a target bound as a texture from the
// bound image list instead of the binding flags), every shadow pass kept open checks that
// TargetView recorded no command (it would be a barrier inside a pass), and every memo hit of 3
// builds the full render state too and compares. A disagreement is logged (LOG_ERROR, the first
// 32 and then every 1024th); that draw keeps the old order, or takes the full state. Every 8192
// UI draws a 'FAST pass_merge:' line (LOG_INFO) gives the counts.
namespace Vulkan::PassMerge {

/// FAST_GPU_PASS_MERGE: on unless set and starting with '0'.
[[nodiscard]] constexpr bool SwitchOn(const char* value) {
    return value == nullptr || value[0] != '0';
}

/// FAST_GPU_PASS_MERGE_VERIFY: on only when it starts with '1'.
[[nodiscard]] constexpr bool VerifyOn(const char* value) {
    return value != nullptr && value[0] == '1';
}

/// One stencil face as the draw records it.
struct StencilFace {
    u32 write_mask{};
    bool keeps{true}; ///< its fail, depth-fail and pass ops are all KEEP
};

/// A draw's depth/stencil use: its pass's attachments and its tests and writes (the committed
/// dynamic state, or the guest registers it came from).
struct DepthStencilUse {
    bool has_depth{};   ///< the pass has a depth attachment
    bool has_stencil{}; ///< ... a stencil attachment
    bool depth_test{};
    bool depth_write{};
    bool stencil_test{};
    StencilFace front{};
    StencilFace back{};
};

/// Whether the draw may write its depth/stencil attachment (which the shadows test). Depth is
/// written only with the test on; stencil only with the test on, a write mask and an op that is
/// not KEEP.
[[nodiscard]] constexpr bool WritesDepthStencil(const DepthStencilUse& u) {
    const bool depth = u.has_depth && u.depth_test && u.depth_write;
    const auto face = [](const StencilFace& f) { return f.write_mask != 0 && !f.keeps; };
    const bool stencil = u.has_stencil && u.stencil_test && (face(u.front) || face(u.back));
    return depth || stencil;
}

/// Why a UI draw keeps the old order (its shadows first), in the order they are asked.
enum Fallback : u32 {
    Off,          ///< FAST_GPU_PASS_MERGE=0
    NotOpen,      ///< its X pass is not open (the draw begins one anyway)
    Hook,         ///< a VR hook records into it (the lock-on marker's pair, the HUD snapshot,
                  ///< the foreground probe, the hunter's split draws, AA, object motion)
    Scaled,       ///< the HUD at S1's output size (bits 58/59)
    Clears,       ///< its state asks for a clear
    Feedback,     ///< it samples one of its own targets
    Writes,       ///< a stage may write memory, or streamout is on
    DepthStencil, ///< it may write the depth/stencil the shadows test
    Disagree,     ///< FAST_GPU_PASS_MERGE_VERIFY decided otherwise
    NumFallbacks, ///< none: the draw goes first, its shadows after
};

inline constexpr std::array<const char*, NumFallbacks> FallbackNames{
    "off",      "not_open", "hook",          "scaled",  "clears",
    "feedback", "writes",   "depth_stencil", "disagree"};

/// What Decide asks about one UI draw.
struct Inputs {
    bool on{};
    bool pass_open{};
    bool hook{};
    bool scaled{};
    bool clears{};
    bool feedback{};
    bool writes{};
    bool depth_stencil{};
};

/// NumFallbacks: the draw goes into X first and its shadows after; else the first reason it keeps
/// the old order.
[[nodiscard]] constexpr Fallback Decide(const Inputs& in) {
    if (!in.on) {
        return Off;
    }
    if (!in.pass_open) {
        return NotOpen;
    }
    if (in.hook) {
        return Hook;
    }
    if (in.scaled) {
        return Scaled;
    }
    if (in.clears) {
        return Clears;
    }
    if (in.feedback) {
        return Feedback;
    }
    if (in.writes) {
        return Writes;
    }
    if (in.depth_stencil) {
        return DepthStencil;
    }
    return NumFallbacks;
}

/// Whether a shadow pass may go without its explicit end (Scheduler::BeginRendering then ends the
/// open pass if it differs): the switch on, a pass open, the target in its attachment layout
/// already (TargetView records nothing) and the HUD not at S1's output size (its depth twin may
/// record a copy). A state that asks for a clear still ends it explicitly (the caller checks).
[[nodiscard]] constexpr bool KeepOpen(bool on, bool rendering, bool target_bound, bool scaled) {
    return on && rendering && target_bound && !scaled;
}

/// The three targets of one UI draw (K and W: VrUiLayer::Black and Grey, or the lock-on marker's
/// pair).
enum class Target : u8 { X, K, W };

/// The order of one UI draw's three passes: W, K, then X (the draw itself), as always; or X first,
/// then K and W (`after_draw`), so that the next draw's first pass, W, is the one this draw left
/// open, and its last, X, the one the draw after it goes on with.
[[nodiscard]] constexpr std::array<Target, 3> Order(bool after_draw) {
    return after_draw ? std::array{Target::X, Target::K, Target::W}
                      : std::array{Target::W, Target::K, Target::X};
}

} // namespace Vulkan::PassMerge
