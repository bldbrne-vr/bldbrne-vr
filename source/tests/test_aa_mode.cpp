// SPDX-License-Identifier: GPL-2.0-or-later

// vr.ini aa_mode (core/vr/aa_mode.h): the word parser, the per-draw decision at the game's FXAA
// draw, the checks SMAA makes of that draw's images, and the CPU mirror of SMAA's luma edge test
// (SMAALumaEdgeDetectionPS, which host_shaders/vr_smaa.comp runs from the reference itself).

#include <array>
#include <cmath>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/aa_mode.h"

namespace VR::Aa {
// For the failure messages.
inline std::ostream& operator<<(std::ostream& os, Mode m) {
    return os << Name(m);
}
inline std::ostream& operator<<(std::ostream& os, const std::optional<Mode>& m) {
    return m ? os << *m : os << "(none)";
}
inline std::ostream& operator<<(std::ostream& os, Action a) {
    return os << (a == Action::Draw   ? "draw"
                  : a == Action::Skip ? "skip"
                  : a == Action::Taa  ? "taa"
                                      : "smaa");
}
inline std::ostream& operator<<(std::ostream& os, const Edges& e) {
    return os << "{left " << e.left << ", top " << e.top << "}";
}
} // namespace VR::Aa

namespace {

using namespace VR::Aa;

TEST(AaMode, ParsesTheWordsAndKeepsTheSettingOnAnythingElse) {
    EXPECT_EQ(ParseMode("fxaa"), Mode::Fxaa);
    EXPECT_EQ(ParseMode("game"), Mode::Fxaa);
    EXPECT_EQ(ParseMode("0"), Mode::Fxaa);
    EXPECT_EQ(ParseMode("smaa"), Mode::Smaa);
    EXPECT_EQ(ParseMode(" SMAA "), Mode::Smaa);
    EXPECT_EQ(ParseMode("1"), Mode::Smaa);
    EXPECT_EQ(ParseMode("taa"), Mode::Taa);
    EXPECT_EQ(ParseMode(" TAA"), Mode::Taa);
    EXPECT_EQ(ParseMode("2"), Mode::Taa);
    // The game's AA off, the eyes' TAA only (aa_mode=taa until 2026-10-08).
    EXPECT_EQ(ParseMode("Off"), Mode::Off);
    EXPECT_EQ(ParseMode("none"), Mode::Off);
    EXPECT_EQ(ParseMode("3"), Mode::Off);
    EXPECT_FALSE(ParseMode("").has_value());
    EXPECT_FALSE(ParseMode("msaa").has_value());
    EXPECT_FALSE(ParseMode("smaa2x").has_value());
    EXPECT_FALSE(ParseMode("4").has_value());
    // Every mode's vr.ini word reads back as itself (the page writes Name, the parser reads it).
    for (const Mode m : {Mode::Fxaa, Mode::Smaa, Mode::Taa, Mode::Off}) {
        EXPECT_EQ(ParseMode(Name(m)), m) << Name(m);
        EXPECT_FALSE(Describe(m).empty());
    }
    EXPECT_STREQ(Name(Mode::Fxaa), "fxaa");
    EXPECT_STREQ(Name(Mode::Smaa), "smaa");
    // The default since 2026-10-07: SMAA (aa_mode=fxaa restores the game's FXAA).
    EXPECT_EQ(DefaultMode, Mode::Smaa);
    EXPECT_STREQ(Name(DefaultMode), "smaa");
    EXPECT_STREQ(Name(Mode::Taa), "taa");
    EXPECT_STREQ(Name(Mode::Off), "off");
}

TEST(AaMode, OnlyTheFxaaDrawOfAnImmersiveFrameChanges) {
    const DrawFacts fxaa_vr{DefaultFxaaFs, DefaultFxaaFs, true};
    const DrawFacts fxaa_flat{DefaultFxaaFs, DefaultFxaaFs, false};
    const DrawFacts other_vr{0xf7221902253, DefaultFxaaFs, true}; // the copy before it
    const DrawFacts no_hash{0, 0, true};
    // aa_mode=fxaa: exactly the build before aa_mode.
    for (const DrawFacts& d : {fxaa_vr, fxaa_flat, other_vr, no_hash}) {
        EXPECT_EQ(Decide(Mode::Fxaa, d), Action::Draw);
    }
    EXPECT_EQ(Decide(Mode::Smaa, fxaa_vr), Action::Smaa);
    EXPECT_EQ(Decide(Mode::Taa, fxaa_vr), Action::Taa);
    EXPECT_EQ(Decide(Mode::Off, fxaa_vr), Action::Skip);
    // The flat screen (no headset, cinematic screens) keeps the game's FXAA.
    EXPECT_EQ(Decide(Mode::Smaa, fxaa_flat), Action::Draw);
    EXPECT_EQ(Decide(Mode::Taa, fxaa_flat), Action::Draw);
    EXPECT_EQ(Decide(Mode::Off, fxaa_flat), Action::Draw);
    // Every other draw is the game's, whatever the mode.
    for (const Mode m : {Mode::Smaa, Mode::Taa, Mode::Off}) {
        EXPECT_EQ(Decide(m, other_vr), Action::Draw);
        EXPECT_EQ(Decide(m, no_hash), Action::Draw);
    }
    // aa_fxaa_fs names the draw (another game version, another hash).
    const DrawFacts custom{0x1234, 0x1234, true};
    EXPECT_EQ(Decide(Mode::Off, custom), Action::Skip);
    EXPECT_EQ(Decide(Mode::Taa, custom), Action::Taa);
    EXPECT_EQ(Decide(Mode::Off, {DefaultFxaaFs, 0x1234, true}), Action::Draw);
    EXPECT_EQ(Decide(Mode::Taa, {DefaultFxaaFs, 0x1234, true}), Action::Draw);
}

TEST(AaMode, UpscalersBypassSpatialAaWithoutMutatingTheSavedMode) {
    for (const auto mode : {Mode::Fxaa, Mode::Smaa, Mode::Off, Mode::Taa}) {
        const auto saved = mode;
        EXPECT_EQ(Decide(mode, {DefaultFxaaFs, DefaultFxaaFs, true, true}), Action::Skip);
        EXPECT_EQ(mode, saved);
        EXPECT_EQ(Decide(mode, {DefaultFxaaFs, DefaultFxaaFs, true, false}),
                  Decide(saved, {DefaultFxaaFs, DefaultFxaaFs, true}));
        EXPECT_EQ(Decide(mode, {123, DefaultFxaaFs, true, true}), Action::Draw);
        EXPECT_EQ(Decide(mode, {DefaultFxaaFs, DefaultFxaaFs, false, true}), Action::Draw);
    }
}

TEST(AaMode, SwitchingIsPerFrameAndNeedsNoRestart) {
    // A session's FXAA draws while the VR page switches the mode: each frame follows the setting
    // it reads (the rasterizer asks at every FXAA draw; nothing is latched).
    const std::array<Mode, 8> setting{Mode::Fxaa, Mode::Smaa, Mode::Smaa, Mode::Off,
                                      Mode::Fxaa, Mode::Taa,  Mode::Taa,  Mode::Smaa};
    const std::array<Action, 8> expected{Action::Draw, Action::Smaa, Action::Smaa,
                                         Action::Skip, Action::Draw, Action::Taa,
                                         Action::Taa,  Action::Smaa};
    for (size_t f = 0; f < setting.size(); ++f) {
        EXPECT_EQ(Decide(setting[f], {DefaultFxaaFs, DefaultFxaaFs, true}), expected[f]) << f;
    }
}

TEST(AaMode, SmaaTakesOnlyTheImagesItCanReplaceTheDrawIn) {
    DrawImages ok{};
    ok.has_texture = ok.has_target = true;
    ok.texture_w = ok.target_w = 2560;
    ok.texture_h = ok.target_h = 1920;
    ok.texture_bytes_per_texel = ok.target_bytes_per_texel = 4;
    std::string why;
    EXPECT_TRUE(Usable(ok, &why));
    EXPECT_TRUE(why.empty());

    auto refused = [&](DrawImages d) {
        std::string w;
        const bool usable = Usable(d, &w);
        EXPECT_FALSE(w.empty());
        return !usable;
    };
    DrawImages d = ok;
    d.has_texture = false;
    EXPECT_TRUE(refused(d));
    d = ok;
    d.has_target = false;
    EXPECT_TRUE(refused(d));
    d = ok;
    d.same_image = true;
    EXPECT_TRUE(refused(d));
    d = ok;
    d.texture_bytes_per_texel = 8; // an RGBA16F scene: not this FXAA
    EXPECT_TRUE(refused(d));
    d = ok;
    d.target_bytes_per_texel = 0;
    EXPECT_TRUE(refused(d));
    d = ok;
    d.texture_w = 1280; // a half-size texture
    EXPECT_TRUE(refused(d));
    d = ok;
    d.target_samples = 4;
    EXPECT_TRUE(refused(d));
    d = ok;
    d.texture_w = d.target_w = 0;
    EXPECT_TRUE(refused(d));
    // 3200x2400 (the 3200 preset) is the same thing larger.
    d = ok;
    d.texture_w = d.target_w = 3200;
    d.texture_h = d.target_h = 2400;
    EXPECT_TRUE(Usable(d));
}

// ---- SMAA's luma edge test --------------------------------------------------------------------

TEST(AaMode, LumaIsRec709InGammaSpaceAndHonoursTheByteOrder) {
    EXPECT_NEAR(Luma(1.0f, 1.0f, 1.0f), 1.0f, 1e-6f);
    EXPECT_NEAR(Luma(1.0f, 0.0f, 0.0f), 0.2126f, 1e-6f);
    EXPECT_NEAR(Luma(0.0f, 1.0f, 0.0f), 0.7152f, 1e-6f);
    EXPECT_NEAR(Luma(0.0f, 0.0f, 1.0f), 0.0722f, 1e-6f);
    // B G R A bytes read as R G B A: red is in the third channel.
    EXPECT_NEAR(Luma(0.0f, 0.0f, 1.0f, true), 0.2126f, 1e-6f);
    EXPECT_NEAR(Luma(1.0f, 0.0f, 0.0f, true), 0.0722f, 1e-6f);
}

TEST(AaMode, EdgesAtTheThreshold) {
    // A step of 0.1 is an edge (step(threshold, delta): >=), 0.09 is not (lumas in exact binary
    // fractions where it matters: 0.5 - 0.4 is 0.099999994 in float, below the threshold, on the
    // GPU as here).
    EXPECT_EQ(LumaEdges(0.5f, 0.375f, 0.5f, 0.5f, 0.5f, 0.375f, 0.5f), (Edges{true, false}));
    EXPECT_EQ(LumaEdges(0.5f, 0.5f, 0.375f, 0.5f, 0.5f, 0.5f, 0.375f), (Edges{false, true}));
    EXPECT_EQ(LumaEdges(0.5f, 0.4f, 0.5f, 0.5f, 0.5f, 0.4f, 0.5f), Edges{});
    EXPECT_EQ(LumaEdges(0.5f, 0.6f, 0.5f, 0.5f, 0.5f, 0.6f, 0.5f), (Edges{true, false}));
    EXPECT_EQ(LumaEdges(0.5f, 0.41f, 0.41f, 0.5f, 0.5f, 0.41f, 0.41f), Edges{});
    EXPECT_EQ(LumaEdges(0.5f, 0.2f, 0.9f, 0.5f, 0.5f, 0.2f, 0.9f), (Edges{true, true}));
    // Flat: nothing.
    EXPECT_EQ(LumaEdges(0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f, 0.3f), Edges{});
}

TEST(AaMode, LocalContrastAdaptationDropsAWeakEdgeBesideAStrongOne) {
    // Left delta 0.15 (an edge), but the right neighbour differs by 0.5: more than twice, so the
    // weak edge is dropped (SMAA's "a bigger contrast nearby hides this one").
    EXPECT_EQ(LumaEdges(0.5f, 0.35f, 0.5f, 1.0f, 0.5f, 0.35f, 0.5f), Edges{});
    // Exactly twice still keeps it (step: >=; 0.125 and 0.25, exact in float).
    EXPECT_EQ(LumaEdges(0.5f, 0.375f, 0.5f, 0.75f, 0.5f, 0.375f, 0.5f), (Edges{true, false}));
    EXPECT_EQ(LumaEdges(0.5f, 0.375f, 0.5f, 0.765625f, 0.5f, 0.375f, 0.5f), Edges{});
    // The left-left difference counts too.
    EXPECT_EQ(LumaEdges(0.5f, 0.35f, 0.5f, 0.5f, 0.5f, 0.0f, 0.5f), Edges{});
    // The top edge survives while the left one is dropped (each against the same maximum).
    EXPECT_EQ(LumaEdges(0.5f, 0.38f, 0.0f, 0.5f, 0.5f, 0.38f, 0.0f), (Edges{false, true}));
}

// The whole edge pass over a small image (the shader's clamp-to-edge reads): an aliased diagonal
// of a bright and a dark half gives edges only along the stair, every step of it.
TEST(AaMode, EdgePassFollowsAnAliasedDiagonal) {
    constexpr int W = 16, H = 16;
    std::vector<float> luma(W * H);
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            luma[y * W + x] = (x > y / 2) ? 0.9f : 0.1f; // a 1-in-2 stair
        }
    }
    const auto at = [&](int x, int y) {
        x = std::clamp(x, 0, W - 1);
        y = std::clamp(y, 0, H - 1);
        return luma[y * W + x];
    };
    int left_edges = 0, top_edges = 0;
    for (int y = 0; y < H; ++y) {
        for (int x = 0; x < W; ++x) {
            const Edges e = LumaEdges(at(x, y), at(x - 1, y), at(x, y - 1), at(x + 1, y),
                                      at(x, y + 1), at(x - 2, y), at(x, y - 2));
            const bool boundary_left = (at(x, y) > 0.5f) != (at(x - 1, y) > 0.5f);
            const bool boundary_top = (at(x, y) > 0.5f) != (at(x, y - 1) > 0.5f);
            EXPECT_EQ(e.left, boundary_left) << x << "," << y;
            EXPECT_EQ(e.top, boundary_top) << x << "," << y;
            left_edges += e.left;
            top_edges += e.top;
        }
    }
    EXPECT_EQ(left_edges, H); // one vertical boundary per row
    EXPECT_EQ(top_edges, 7);  // a horizontal step every second row (x = 1 .. 7)
}

} // namespace
