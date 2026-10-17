// SPDX-License-Identifier: GPL-2.0-or-later

// FAST_CPU_VIEW_PREDICT (src/video_core/renderer_vulkan/vk_pipeline_skip.h PredictedViewMask):
// the view mask Stage A and the preparation workers select a draw's pipeline with in native
// stereo, from the recording's mask for its set of targets (TargetViews), so a draw into a target
// that is not an eye pair no longer builds a multiview twin the recording never binds. The
// recording's reselect stays the authority; these tests pin what is guessed when.

#include <cstdint>
#include <memory>

#include <gtest/gtest.h>

#include "video_core/renderer_vulkan/vk_pipeline_skip.h"

namespace {

using namespace Vulkan::PipelineSkip;

constexpr std::uint64_t Scene = 0x6'0000'0000ull;
constexpr std::uint64_t SceneDepth = 0x6'2000'0000ull;
constexpr std::uint64_t Atlas = 0x6'1000'0000ull;
constexpr std::uint64_t Hud = 0x6'4000'0000ull;

TEST(ViewPredict, OnInNativeStereoUnlessZeroOrADebugHook) {
    EXPECT_TRUE(ViewPredictOn(nullptr, true, false));
    EXPECT_TRUE(ViewPredictOn("", true, false));
    EXPECT_TRUE(ViewPredictOn("1", true, false));
    EXPECT_FALSE(ViewPredictOn("0", true, false));   // FAST_CPU_VIEW_PREDICT=0
    EXPECT_FALSE(ViewPredictOn(nullptr, false, false)); // flat: never (always mask 0 there)
    EXPECT_FALSE(ViewPredictOn("1", false, false));
    EXPECT_FALSE(ViewPredictOn(nullptr, true, true)); // the draw inspector or foreground probe
}

TEST(ViewPredict, AsBeforeWhenNothingIsKnown) {
    ViewFacts f{};
    f.targets = AddTarget(AddTarget(0, Scene, 0), SceneDepth, 8);
    EXPECT_EQ(PredictedViewMask(f), 3u); // never drawn: the multiview pipeline, as before
}

TEST(ViewPredict, NoAttachmentIsAlwaysMono) {
    // The recording's `desired` is 0 for a draw with no attachment (no colour target written,
    // no depth or stencil test): the full-screen DB passes of fast bit 54, every frame.
    ViewFacts f{};
    EXPECT_EQ(PredictedViewMask(f), 0u);
    f.noted = 3; // cannot be noted for key 0 (Find(0) is Unknown); still mono
    EXPECT_EQ(PredictedViewMask(f), 0u);
}

TEST(ViewPredict, FollowsTheRecordingsLastMask) {
    const auto views = std::make_unique<TargetViews>();
    const std::uint64_t scene = AddTarget(AddTarget(0, Scene, 0), SceneDepth, 8);
    const std::uint64_t atlas = AddTarget(0, Atlas, 8);
    const std::uint64_t hud = AddTarget(0, Hud, 0);
    // The recording thread notes each draw's targets with its `desired`.
    views->Note(scene, 3);
    views->Note(atlas, 0); // the shared shadow atlas
    views->Note(hud, 0);   // a mono UI target
    const auto predict = [&](std::uint64_t targets, bool layered = false) {
        ViewFacts f{};
        f.targets = targets;
        f.noted = views->Find(targets);
        f.layered = layered;
        return PredictedViewMask(f);
    };
    EXPECT_EQ(predict(scene), 3u);
    EXPECT_EQ(predict(atlas), 0u);
    EXPECT_EQ(predict(hud), 0u);
    // The image behind a set of targets changed (an eye pair now): the recording's reselect
    // corrects the one wrong guess and notes 3, which the next draw follows.
    views->Note(hud, 3);
    EXPECT_EQ(predict(hud), 3u);
    // A noted mask wins over the layered pre-filter (the recording found an eye pair there).
    const std::uint64_t layered = AddTarget(0, Scene + 0x100000, 0);
    views->Note(layered, 3);
    EXPECT_EQ(predict(layered, true), 3u);
}

TEST(ViewPredict, LayeredViewsWithoutANoteAreMono) {
    ViewFacts f{};
    f.targets = AddTarget(0, Scene, 0);
    f.layered = LayeredView(1, 1); // a cube face or an array slice
    EXPECT_EQ(PredictedViewMask(f), 0u);
    f.layered = LayeredView(0, 0);
    EXPECT_EQ(PredictedViewMask(f), 3u);
}

TEST(ViewPredict, UnknownFragmentProgramWithColoursIsNotGuessed) {
    // Which colour targets a never-translated fragment program writes is unknown, so the key
    // cannot be built: as before, whatever the table says for the bound set.
    ViewFacts f{};
    f.targets = AddTarget(0, Hud, 0);
    f.noted = 0;
    f.colours_unknown = true;
    EXPECT_EQ(PredictedViewMask(f), 3u);
    // Without a bound colour target the key is the depth target's alone: guessed.
    f.colours_unknown = false;
    f.targets = AddTarget(0, Atlas, 8);
    f.noted = 0;
    EXPECT_EQ(PredictedViewMask(f), 0u);
}

TEST(ViewPredict, SubsetOfTargetsKeysApart) {
    // A pipeline writing only the scene colour of a bound scene + HUD pair is keyed apart from
    // one writing both, so the mono HUD never makes the scene-only draw mono.
    const auto views = std::make_unique<TargetViews>();
    const std::uint64_t both = AddTarget(AddTarget(0, Scene, 0), Hud, 1);
    const std::uint64_t scene_only = AddTarget(0, Scene, 0);
    views->Note(both, 0);
    views->Note(scene_only, 3);
    ViewFacts f{};
    f.targets = scene_only;
    f.noted = views->Find(scene_only);
    EXPECT_EQ(PredictedViewMask(f), 3u);
    f.targets = both;
    f.noted = views->Find(both);
    EXPECT_EQ(PredictedViewMask(f), 0u);
}

} // namespace
