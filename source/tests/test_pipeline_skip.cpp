// SPDX-License-Identifier: GPL-2.0-or-later

// Fast bit 40's skip-the-draw in native-stereo VR (src/video_core/renderer_vulkan/
// vk_pipeline_skip.h): when it is on, the vetoes that draw a candidate after all (stencil writes,
// the immersive-frame gate, the UI phase, the hunter's own draws, layered targets), the age cap,
// and the lock-free table of the view mask each set of targets was last drawn with.

#include <cstdint>
#include <memory>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vk_pipeline_skip.h"

namespace {

using namespace Vulkan::PipelineSkip;

constexpr std::uint64_t Ms = 1'000'000;

TEST(PipelineSkip, OnByDefaultInNativeStereoOnly) {
    EXPECT_TRUE(SkipOn(nullptr, true));
    EXPECT_FALSE(SkipOn(nullptr, false)); // flat: as before, off
    EXPECT_TRUE(SkipOn("", true));        // set but empty: as not set
    EXPECT_FALSE(SkipOn("0", true));      // FAST_ASYNC_PIPELINE_SKIP=0 turns it off
    EXPECT_TRUE(SkipOn("1", false));      // =1 in flat too, as before
    EXPECT_FALSE(SkipOn("2", true));      // anything but '1' is off, as before
}

TEST(PipelineSkip, AgeCapFromTheEnvironment) {
    EXPECT_EQ(AgeCapNs(nullptr), 640 * Ms);
    EXPECT_EQ(AgeCapNs(""), 640 * Ms);
    EXPECT_EQ(AgeCapNs("250"), 250 * Ms);
    EXPECT_EQ(AgeCapNs("0"), 0u); // no cap
    // Not a plain number: the default, never a silent "no cap" or a wrapped huge one.
    EXPECT_EQ(AgeCapNs("100ms"), 640 * Ms);
    EXPECT_EQ(AgeCapNs("off"), 640 * Ms);
    EXPECT_EQ(AgeCapNs("-1"), 640 * Ms);
    EXPECT_EQ(AgeCapNs(" 50"), 640 * Ms);
    EXPECT_EQ(AgeCapNs("+50"), 640 * Ms);
    // At most 10 s, also past what fits in 64 bits.
    EXPECT_EQ(AgeCapNs("10000"), 10'000 * Ms);
    EXPECT_EQ(AgeCapNs("50000"), 10'000 * Ms);
    EXPECT_EQ(AgeCapNs("99999999999999999999999"), 10'000 * Ms);
}

TEST(PipelineSkip, AgedOnlyPastTheCap) {
    const std::uint64_t first = 5'000 * Ms;
    EXPECT_FALSE(Aged(first, first, 100 * Ms));
    EXPECT_FALSE(Aged(first, first + 100 * Ms, 100 * Ms)); // exactly the cap: still left out
    EXPECT_TRUE(Aged(first, first + 100 * Ms + 1, 100 * Ms));
    EXPECT_FALSE(Aged(0, first + 10'000 * Ms, 100 * Ms));   // not started / never left out
    EXPECT_FALSE(Aged(first, first + 10'000 * Ms, 0));      // no cap: bbport's rule
    EXPECT_FALSE(Aged(first, first - 1, 100 * Ms));         // a clock read before it
}

TEST(PipelineSkip, UiPhaseFromTheComposite) {
    constexpr std::uint64_t Never = ~std::uint64_t{0};
    EXPECT_EQ(Phase(41, 42), UiPhase::World); // composite last frame: before this frame's
    EXPECT_EQ(Phase(42, 42), UiPhase::Ui);    // after this frame's composite
    // No composite in the frame before (a loading screen, or hud_after_vs not the game's): the
    // phase cannot be told.
    EXPECT_EQ(Phase(40, 42), UiPhase::Unknown);
    EXPECT_EQ(Phase(Never, 0), UiPhase::Unknown);
    EXPECT_EQ(Phase(Never, 42), UiPhase::Unknown);
}

TEST(PipelineSkip, StencilWrites) {
    constexpr std::uint32_t Keep = 0, Replace = 3, Incr = 5;
    const StencilFace keep{Keep, Keep, Keep, 0xFF};
    const StencilFace marks{Keep, Replace, Keep, 0xFF};
    const StencilFace masked{Keep, Replace, Keep, 0x00};
    EXPECT_FALSE(WritesStencil(false, marks, false, marks, false)); // test off
    EXPECT_FALSE(WritesStencil(true, keep, false, keep, false));    // only tests
    EXPECT_FALSE(WritesStencil(true, masked, false, masked, false)); // write mask 0
    EXPECT_TRUE(WritesStencil(true, marks, false, keep, false));
    // The back faces count only with backface_enable (else they have the front's state).
    EXPECT_FALSE(WritesStencil(true, keep, false, StencilFace{Incr, Keep, Keep, 0x01}, false));
    EXPECT_TRUE(WritesStencil(true, keep, true, StencilFace{Incr, Keep, Keep, 0x01}, false));
    EXPECT_TRUE(WritesStencil(true, keep, true, StencilFace{Keep, Keep, Incr, 0x80}, false));
    // A DB clear, whatever its stencil state.
    EXPECT_TRUE(WritesStencil(false, keep, false, keep, true));
}

FrameState Gameplay() {
    return {.vr = true,
            .immersive = true,
            .cinematic_flat = false,
            .driven = true,
            .has_camera = true,
            .menu = false,
            .first_person = true,
            .has_player = true};
}

TEST(PipelineSkip, OnlyImmersiveFrames) {
    EXPECT_TRUE(ImmersiveFrame(Gameplay()));
    auto f = Gameplay();
    f.vr = false; // no VR session
    EXPECT_FALSE(ImmersiveFrame(f));
    f = Gameplay();
    f.immersive = false; // the virtual screen
    EXPECT_FALSE(ImmersiveFrame(f));
    f = Gameplay();
    f.cinematic_flat = true;
    EXPECT_FALSE(ImmersiveFrame(f));
    f = Gameplay();
    f.menu = true; // the menu panel
    EXPECT_FALSE(ImmersiveFrame(f));
    f = Gameplay();
    f.driven = false; // the game's own camera
    EXPECT_FALSE(ImmersiveFrame(f));
    f = Gameplay();
    f.has_camera = false;
    EXPECT_FALSE(ImmersiveFrame(f));
    // Third person: only with his origin (the camera is not his head then).
    f = Gameplay();
    f.first_person = false;
    EXPECT_TRUE(ImmersiveFrame(f));
    f.has_player = false;
    EXPECT_FALSE(ImmersiveFrame(f));
    f = Gameplay();
    f.has_player = false; // first person without his origin: what is near the camera is his
    EXPECT_TRUE(ImmersiveFrame(f));
}

TEST(PipelineSkip, PlayersDraws) {
    const Offset far_from_camera{10.0f, -1.6f, 10.0f};
    // His skinned parts at his origin, or under it in a deep duck.
    EXPECT_TRUE(PlayersDraw(true, true, {0.0f, 0.0f, 0.0f}, true, far_from_camera));
    EXPECT_TRUE(PlayersDraw(true, true, {0.1f, -0.6f, 0.0f}, true, far_from_camera));
    // A tilted piece on his bones within PieceReachM (measured 1.6 m on a raised arm).
    EXPECT_TRUE(PlayersDraw(true, true, {1.2f, 1.0f, 0.8f}, false, far_from_camera));
    EXPECT_TRUE(PlayersDraw(false, true, {0.0f, 1.6f, 0.0f}, false, far_from_camera));
    // An enemy's weapon 2.2 m from his origin, away from his head: not his (IsPlayersObject
    // alone would take it, up to 2.5 m).
    EXPECT_FALSE(PlayersDraw(true, true, {2.2f, 0.3f, 0.0f}, false, far_from_camera));
    EXPECT_FALSE(PlayersDraw(true, true, {2.0f, 1.0f, 2.0f}, false, far_from_camera));
    // A character root 1.5 m away is someone else's.
    EXPECT_FALSE(PlayersDraw(true, true, {1.5f, 0.0f, 0.0f}, true, {1.5f, -1.6f, 0.0f}));
    // First person: whatever his VR hands hold, within an arm's reach of the eyes in any
    // direction (a weapon raised overhead, an outstretched arm), upright or tilted.
    EXPECT_TRUE(PlayersDraw(true, true, {5.0f, 0.0f, 5.0f}, false, {0.1f, 0.7f, 0.2f}));
    EXPECT_TRUE(PlayersDraw(true, true, {5.0f, 0.0f, 5.0f}, false, {1.1f, -0.2f, 0.0f}));
    EXPECT_TRUE(PlayersDraw(true, true, {5.0f, 0.0f, 5.0f}, true, {0.0f, -0.5f, 0.6f}));
    EXPECT_FALSE(PlayersDraw(true, true, {5.0f, 0.0f, 5.0f}, false, {1.3f, -0.2f, 0.0f}));
    // ... not in third person, where the camera is not his head.
    EXPECT_FALSE(PlayersDraw(false, true, {5.0f, 0.0f, 5.0f}, false, {0.1f, 0.7f, 0.2f}));
    // First person without his origin: anything within 2.5 m of the camera.
    EXPECT_TRUE(PlayersDraw(true, false, {}, true, {0.3f, -1.5f, 0.2f}));
    EXPECT_TRUE(PlayersDraw(true, false, {}, false, {0.0f, 0.8f, 0.0f}));  // raised overhead
    EXPECT_TRUE(PlayersDraw(true, false, {}, false, {1.6f, -0.4f, 0.0f})); // arm outstretched
    EXPECT_FALSE(PlayersDraw(true, false, {}, true, {1.5f, -2.0f, 1.5f}));
    EXPECT_FALSE(PlayersDraw(true, false, {}, true, {0.0f, -3.0f, 0.0f})); // a floor below
}

TEST(PipelineSkip, LayeredViewsAreMono) {
    EXPECT_FALSE(LayeredView(0, 0));
    EXPECT_TRUE(LayeredView(1, 1)); // a cube face or array slice
    EXPECT_TRUE(LayeredView(0, 5)); // a layered target
}

TEST(PipelineSkip, TargetKeys) {
    const std::uint64_t x = 0x6'0000'0000ull;
    const std::uint64_t depth = 0x6'2000'0000ull;
    const std::uint64_t mono_depth = 0x6'3000'0000ull;
    // Every attachment counts: the same first target with another depth or second target is
    // another key; the same attachments always give the same one, never 0.
    const std::uint64_t with_depth = AddTarget(AddTarget(0, x, 0), depth, 8);
    EXPECT_NE(with_depth, 0u);
    EXPECT_EQ(with_depth, AddTarget(AddTarget(0, x, 0), depth, 8));
    EXPECT_NE(with_depth, AddTarget(AddTarget(0, x, 0), mono_depth, 8));
    EXPECT_NE(with_depth, AddTarget(0, x, 0));
    EXPECT_NE(AddTarget(AddTarget(0, x, 0), x + 0x1000, 1), AddTarget(0, x, 0));
    // The slot matters: one image as colour target 1 is not colour target 0.
    EXPECT_NE(AddTarget(0, x, 0), AddTarget(0, x, 1));
    EXPECT_NE(AddTarget(0, 0, 8), 0u); // even a zero address
}

TEST(PipelineSkip, TargetViews) {
    const auto views = std::make_unique<TargetViews>();
    const std::uint64_t x = 0x6'0000'0000ull;
    const std::uint64_t scene = AddTarget(AddTarget(0, x, 0), 0x6'2000'0000ull, 8);
    const std::uint64_t mono = AddTarget(AddTarget(0, x, 0), 0x6'3000'0000ull, 8);
    const std::uint64_t shadow = AddTarget(0, 0x6'1000'0000ull, 8);
    EXPECT_EQ(views->Find(scene), TargetViews::Unknown); // never drawn: no skip
    views->Note(scene, 3);
    views->Note(shadow, 0);
    EXPECT_EQ(views->Find(scene), 3);
    EXPECT_EQ(views->Find(shadow), 0);
    // A draw into the same first target with a mono depth has its own entry: the eye-pair
    // draws into X after it never make it read 3.
    views->Note(mono, 0);
    views->Note(scene, 3);
    EXPECT_EQ(views->Find(mono), 0);
    EXPECT_EQ(views->Find(scene), 3);
    views->Note(scene, 0); // the recording drew these mono since
    EXPECT_EQ(views->Find(scene), 0);
    views->Note(scene, 3);
    EXPECT_EQ(views->Find(scene), 3);
    // No attachments (key 0): never kept.
    EXPECT_EQ(views->Find(0), TargetViews::Unknown);
    views->Note(0, 3);
    EXPECT_EQ(views->Find(0), TargetViews::Unknown);
    // A key sharing the slot takes it: the older one is unknown again, never misread.
    std::uint64_t other = 0;
    for (std::uint64_t address = x + 0x100; other == 0; address += 0x100) {
        const std::uint64_t candidate = AddTarget(0, address, 0);
        views->Note(candidate, 0);
        if (views->Find(scene) == TargetViews::Unknown) {
            other = candidate;
        } else {
            EXPECT_EQ(views->Find(scene), 3);
        }
    }
    EXPECT_EQ(views->Find(other), 0);
    EXPECT_EQ(views->Find(scene), TargetViews::Unknown);
}

} // namespace
