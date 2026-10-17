// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>

#include "common/types.h"
#include "video_core/renderer_vulkan/vr_body_split.h"

namespace Shader {
struct Info;
}

// Fast bit 40's skip-the-draw (vk_pipeline_async.cpp), on by default in native-stereo VR.
// bbport's rule picks the candidates: a pass drawn every frame whose new pipeline is still
// compiling is left out until the pipeline is ready. In VR a candidate is drawn after all (its
// pipeline compiled or waited for, as without the rule) when one of the vetoes below holds, or
// when its compile alone has run for longer than the age cap. The rules are decided only for a
// candidate, never for a draw whose pipeline exists. The pure parts are here, so the unit tests
// (tests/test_pipeline_skip.cpp) cover them.
namespace Vulkan::PipelineSkip {

// Why a candidate is drawn after all (each counted in the 'FAST async_pipelines:' line).
enum Veto : u32 {
    None,
    Writer,  // a stage may write memory, or streamout is on: later passes read what it writes
    Player,  // the hunter's own draw (body, arms, hands, weapon, cape, items): next to the eyes
    Ui,      // after the composite (hud_after_vs), or no composite seen to tell (UiPhase)
    Mono,    // a target that is not an eye pair (a layered view, a block-compressed or the shared
             // shadow atlas), or targets the recording has not drawn yet. The shadow cascades are
             // eye pairs in native stereo (drawn per eye), so their casters can be left out.
    Stencil, // it writes stencil (a skipped mark pass leaks lighting), or it is a DB clear
    Frame,   // not an immersive VR frame (the virtual screen, a flat cinematic, the menu panel)
    NumVetoes,
};

// FAST_ASYNC_PIPELINE_SKIP's value (null: not set): '1' on (in flat too: bbport's rule alone),
// anything else off; not set, on in native stereo (with the vetoes and the age cap).
constexpr bool SkipOn(const char* env, bool native_stereo) {
    return env != nullptr && env[0] != '\0' ? env[0] == '1' : native_stereo;
}

// The age cap from FAST_ASYNC_PIPELINE_SKIP_MS's value, in ns: whole milliseconds, 0 (no cap:
// bbport's rule, left out until it is ready) to 10000. Not set, empty, or not a plain number
// ("100ms", "-1", "off"): the default 640 ms, so a typo never turns the cap off.
inline u64 AgeCapNs(const char* env) {
    constexpr u64 Default = 640;
    constexpr u64 Max = 10'000;
    if (env == nullptr || env[0] < '0' || env[0] > '9') {
        return Default * 1'000'000;
    }
    char* end = nullptr;
    const u64 ms = std::strtoull(env, &end, 10);
    if (end == env || *end != '\0') {
        return Default * 1'000'000;
    }
    return std::min(ms, Max) * 1'000'000;
}

// Past the cap: more than `cap_ns` (0: no cap) since `since_ns` (0: not yet). The rule starts
// two clocks: since a key was first left out (its job then goes to the front of the compile
// queue) and since its compile started (then it is waited for, at most one such wait a frame).
constexpr bool Aged(u64 since_ns, u64 now_ns, u64 cap_ns) {
    return cap_ns != 0 && since_ns != 0 && now_ns > since_ns && now_ns - since_ns > cap_ns;
}

// Where Stage A is in its frame, from the frame the composite (hud_after_vs) was last selected in
// (`composite`, ~0: never) and the current one.
enum class UiPhase : u8 {
    World,   // before this frame's composite, which was drawn in the frame before
    Ui,      // after this frame's composite: the HUD and menus
    Unknown, // no composite in the frame before (hud_after_vs not this game's, or a frame after a
             // loading screen): the UI phase cannot be told, so nothing is left out (Veto::Ui)
};

constexpr UiPhase Phase(u64 composite, u64 frame) {
    if (composite == frame) {
        return UiPhase::Ui;
    }
    return composite != ~u64{0} && composite + 1 == frame ? UiPhase::World : UiPhase::Unknown;
}

// One face's stencil state: DB_STENCIL_CONTROL's ops (AmdGpu::StencilFunc, 0 = Keep) and
// DB_STENCILREFMASK's write mask.
struct StencilFace {
    u32 fail{};
    u32 zpass{};
    u32 zfail{};
    u32 write_mask{};
};

// Does the draw change stencil: the test on (with a valid stencil buffer) and a face whose ops
// change it through its write mask (the back faces have the front's state without
// backface_enable), or a DB depth or stencil clear (DB_RENDER_CONTROL).
constexpr bool WritesStencil(bool test, const StencilFace& front, bool backface,
                             const StencilFace& back, bool db_clear) {
    const auto writes = [](const StencilFace& face) {
        return face.write_mask != 0 && (face.fail | face.zpass | face.zfail) != 0;
    };
    return db_clear || (test && (writes(front) || (backface && writes(back))));
}

// The frame the candidate is drawn in.
struct FrameState {
    bool vr{};             // a VR session (the rasterizer has its stereo warp)
    bool immersive{};      // VR::IsImmersive: the eyes show the game's world
    bool cinematic_flat{}; // VR::CinematicFlat: a cutscene shown on a screen
    bool driven{};         // its camera record was placed by the headset
    bool has_camera{};     // ... with the render camera's position
    bool menu{};           // ... for the menu panel
    bool first_person{};   // ... in the hunter's head
    bool has_player{};     // ... with his model origin
};

// Only an immersive frame (Veto::Frame otherwise): on a screen (the virtual screen, a flat
// cinematic, the menu panel) a stall is a flat game's stutter and the screen stays put, as flat
// mode keeps it; and only where the hunter can be found (PlayersDraw).
constexpr bool ImmersiveFrame(const FrameState& f) {
    return f.vr && f.immersive && !f.cinematic_flat && f.driven && f.has_camera && !f.menu &&
           (f.first_person || f.has_player);
}

// A translation relative to a point, in metres (world axes, y up).
struct Offset {
    float x{};
    float y{};
    float z{};
};

// How far from the hunter a draw's object transform may stand and still be taken for his.
inline constexpr float PieceReachM = 2.0f;  // a tilted piece on his bones, from his origin
                                            // (measured 1.6 m for a raised arm's bandage)
inline constexpr float HeldReachM = 1.2f;   // first person: the VR hands and what they hold, an
                                            // arm's reach from the eyes in any direction
inline constexpr float NearCameraM = 2.5f;  // first person without his origin: anything that close

// Is a draw the hunter's own, by its object transform (vk_rasterizer_vr_draws.cpp
// ObjectPosition)? With his model origin, VrBodySplit::IsPlayersObject (his skinned parts at his
// origin or under it in a duck; a tilted piece on his bones) with its pieces kept within
// PieceReachM, so an enemy's weapon 2 m away is not his. In first person also anything within
// HeldReachM of the camera (a weapon raised overhead, an outstretched arm), or without his origin
// within NearCameraM. A wider net only costs a compile here; a narrower one lets a piece he holds
// vanish while its pipeline compiles.
constexpr bool PlayersDraw(bool first_person, bool has_player, const Offset& to_player,
                           bool upright, const Offset& to_camera) {
    const auto length2 = [](const Offset& o) { return o.x * o.x + o.y * o.y + o.z * o.z; };
    if (has_player &&
        VrBodySplit::IsPlayersObject(to_player.x, to_player.y, to_player.z, upright) &&
        (upright || length2(to_player) <= PieceReachM * PieceReachM)) {
        return true;
    }
    if (!first_person) {
        return false;
    }
    const float reach = has_player ? HeldReachM : NearCameraM;
    return length2(to_camera) <= reach * reach;
}

// A colour or depth view of other slices than the first: never an eye pair (an eye pair is a 2D
// image of one layer, Image::native_stereo).
constexpr bool LayeredView(u32 slice_start, u32 slice_max) {
    return slice_start != 0 || slice_max != 0;
}

// The key of a draw's render targets (TargetViews): from 0, each attachment the recording binds
// added in turn, its colour targets by slot (0..7) and then its depth target (slot 8). Stage B
// adds its bound attachments (cb_descs, db_desc), Stage A the same ones from the registers and
// the pipeline's mrt_mask, so one key always means one set of images. Never 0 once one is added.
constexpr u64 AddTarget(u64 key, u64 address, u32 slot) {
    u64 x = (key ^ address ^ (u64{slot + 1} << 56)) * 0x9E3779B97F4A7C15ull;
    x ^= x >> 29;
    return x | (u64{1} << 63);
}

// The recording thread to Stage A: the view mask each set of render targets was last drawn with
// (the recording's `desired`: 3 when every target is an eye pair, else 0). Keyed by AddTarget's
// key, so a draw with a mono depth or a mono second target never reads its first target's mask.
// Lock-free, direct-mapped and lossy: a newer key takes the slot, and the older one is then
// unknown again (no skip). FAST_CPU_VIEW_PREDICT reads it too (PredictedViewMask; Stage A and the
// preparation workers).
class TargetViews {
public:
    static constexpr u32 SlotBits = 12;
    static constexpr size_t Slots = size_t{1} << SlotBits;
    static constexpr s32 Unknown = -1;

    // FAST_CPU_PASS_MEMO (the rasterizer's switch): Note's last entry. Only Note writes the slots
    // and an entry fixes its slot (Index reads only the bits it keeps), so once Note stored or
    // found an entry, its slot holds it until Note is given another one: noting it again changes
    // nothing. Memo skips such a repeat without loading the slot (a 32 KiB table; most draws note
    // the targets of the draw before); Verify loads and stores as Off does and returns false when
    // the slot did not hold the entry Memo would have skipped.
    enum class NoteMemo : u8 { Off, Memo, Verify };

    // The recording thread, per draw; stores only when the slot changes.
    bool Note(u64 key, u32 view_mask, NoteMemo memo = NoteMemo::Off) {
        if (key == 0) {
            return true;
        }
        const u64 entry = (key & ~u64{0xFF}) | (view_mask == 3 ? 2u : 1u);
        const bool repeat = entry == last_entry;
        if (repeat && memo == NoteMemo::Memo) {
            return true;
        }
        last_entry = entry;
        auto& slot = slots[Index(key)];
        const bool held = slot.load(std::memory_order_relaxed) == entry;
        if (!held) {
            slot.store(entry, std::memory_order_relaxed);
        }
        return held || !repeat || memo != NoteMemo::Verify;
    }

    // Stage A: the view mask last noted for `key` (3 or 0), or Unknown.
    s32 Find(u64 key) const {
        if (key == 0) {
            return Unknown;
        }
        const u64 entry = slots[Index(key)].load(std::memory_order_relaxed);
        if ((entry & ~u64{0xFF}) != (key & ~u64{0xFF})) {
            return Unknown;
        }
        return (entry & 0xFF) == 2 ? 3 : 0;
    }

private:
    static constexpr size_t Index(u64 key) {
        return size_t(((key >> 8) * 0x9E3779B97F4A7C15ull) >> (64 - SlotBits));
    }

    std::array<std::atomic<u64>, Slots> slots{};
    u64 last_entry{}; // Note's, recording thread only (0 is no entry: their low byte is 1 or 2)
};

// FAST_CPU_VIEW_PREDICT's value (null: not set): on in native stereo unless '0'. Off with a debug
// hook that reads Stage A's pipeline before the recording's reselect (`debug_hooks`: the draw
// inspector's stage hashes, VR_DRAW_CTL; the foreground probe's base, VR_FOREGROUND_PROBE).
constexpr bool ViewPredictOn(const char* env, bool native_stereo, bool debug_hooks) {
    return native_stereo && !debug_hooks && !(env != nullptr && env[0] == '0');
}

// What Stage A (or a preparation worker) knows of a draw's render targets before it selects the
// draw's pipeline (vk_pipeline_async.cpp PipelineCache::PredictViewMask).
struct ViewFacts {
    u64 targets{};                   // their AddTarget key, as the recording builds it (0: none)
    s32 noted{TargetViews::Unknown}; // TargetViews::Find(targets)
    bool layered{};                  // a layered colour or depth view among them (LayeredView)
    bool colours_unknown{};          // a colour target is bound and the fragment program was
                                     // never translated: which ones it writes (the key) is unknown
};

// FAST_CPU_VIEW_PREDICT: the view mask a draw's pipeline is selected with in native stereo, the
// recording's `desired` as far as it can be told in advance. Without it Stage A always selects 3
// and the recording reselects 0 when a target is not an eye pair (a layered or 3D view, a
// block-compressed image, the shared shadow atlas, a mono UI target), so every pipeline drawn
// there was built twice (its multiview twin never bound). 0 when the draw has no attachment (the
// recording's `desired` is then 0 whatever the images) or its set of targets was last drawn
// with 0; with no note, 0 for a layered view (never an eye pair: an eye pair is a 2D image of one
// layer) and 3 otherwise, as before. The recording's reselect stays the authority: a wrong
// guess costs that reselect (and a compile of the other mask), never a different image.
constexpr u32 PredictedViewMask(const ViewFacts& f) {
    if (f.colours_unknown) {
        return 3;
    }
    if (f.targets == 0) {
        return 0;
    }
    if (f.noted != TargetViews::Unknown) {
        return f.noted == 3 ? 3u : 0u;
    }
    return f.layered ? 0u : 3u;
}

// A guest stage that may write memory: a written buffer or image (atomics included), an image
// atomic, a storage image, raw (DMA) memory access, or a GDS, fault or BDA page-table buffer
// (vk_pipeline_async.cpp; fast bit 54's no-op draws use it too).
bool StageMayWrite(const Shader::Info& info);

} // namespace Vulkan::PipelineSkip
