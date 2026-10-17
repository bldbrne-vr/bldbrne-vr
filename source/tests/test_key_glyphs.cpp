// SPDX-License-Identifier: GPL-2.0-or-later

// The game's button icons as the VR controllers' buttons (src/core/vr/key_glyphs.h): the badges,
// the BC7 encoder (decoded back with core/vr/bc7_decode.h) and the atlas pass. The pass on the
// game's own files runs when BBVR_KEYGLYPH_TPF (an unpacked game asset) and
// BBVR_KEYGLYPH_FONT (game asset) are set; BBVR_KEYGLYPH_DUMP=<dir> also writes every
// redrawn icon there, decoded (32 x 32 RGBA8, <name>.rgba).

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <random>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "core/vr/bc7_decode.h"
#include "core/vr/key_glyphs.h"

namespace {

using namespace VR;
using u8 = std::uint8_t;

std::vector<u8> Decode(const std::vector<u8>& blocks, int w, int h) {
    std::vector<u8> out(static_cast<std::size_t>(w) * h * 4);
    Bc7::DecodeImage(blocks.data(), w / 4, h / 4, out.data());
    return out;
}

// Mean and max absolute error over every channel, colour weighted by alpha (what is seen).
void Compare(const std::vector<u8>& a, const std::vector<u8>& b, double& mean, int& max) {
    double sum = 0.0;
    max = 0;
    for (std::size_t i = 0; i < a.size(); i += 4) {
        for (int k = 0; k < 4; ++k) {
            int d = std::abs(a[i + k] - b[i + k]);
            if (k < 3) {
                d = d * a[i + 3] / 255;
            }
            sum += d;
            max = std::max(max, d);
        }
    }
    mean = sum / static_cast<double>(a.size());
}

std::vector<u8> ReadFile(const char* path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

TEST(KeyGlyphs, MenuBadgeNeedsNoFont) {
    const auto rgba = KeyGlyphs::Draw(KeyGlyphs::Glyphs.back(), nullptr);
    ASSERT_EQ(rgba.size(), 32u * 32 * 4);
    // A disc 26 texels across: opaque at the centre, clear in the corners, light bars inside.
    EXPECT_EQ(rgba[(16 * 32 + 16) * 4 + 3], 255);
    EXPECT_EQ(rgba[(1 * 32 + 1) * 4 + 3], 0);
    EXPECT_GT(rgba[(16 * 32 + 16) * 4 + 0], 200); // the middle bar
    EXPECT_LT(rgba[(18 * 32 + 16) * 4 + 0], 90);  // between the bars: the dark body
    // Lettered badges need the font.
    EXPECT_TRUE(KeyGlyphs::Draw(KeyGlyphs::Glyphs.front(), nullptr).empty());
}

TEST(KeyGlyphs, Bc7RoundTripOfABadge) {
    const auto rgba = KeyGlyphs::Draw(KeyGlyphs::Glyphs.back(), nullptr);
    const auto blocks = KeyGlyphs::EncodeBc7(rgba, 32, 32);
    ASSERT_EQ(blocks.size(), 64u * 16);
    double mean;
    int max;
    Compare(rgba, Decode(blocks, 32, 32), mean, max);
    std::printf("  menu badge: mean %.2f, max %d\n", mean, max);
    EXPECT_LT(mean, 1.5);
    EXPECT_LT(max, 40);
}

TEST(KeyGlyphs, Bc7RoundTripOfNoiseAndFlats) {
    std::mt19937 rng{7};
    std::vector<u8> rgba(16 * 16 * 4);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            u8* p = &rgba[(y * 16 + x) * 4];
            if (y < 4) { // flat blocks, one of them fully clear
                p[0] = 10, p[1] = 200, p[2] = 90, p[3] = x < 4 ? 0 : 255;
            } else if (y < 8) { // a smooth gradient along x with an alpha ramp
                p[0] = static_cast<u8>(x * 16), p[1] = static_cast<u8>(255 - x * 12), p[2] = 128;
                p[3] = static_cast<u8>(x * 17);
            } else { // noise: lossy, but sane
                for (int k = 0; k < 4; ++k) {
                    p[k] = static_cast<u8>(rng() & 0xFF);
                }
            }
        }
    }
    const auto back = Decode(KeyGlyphs::EncodeBc7(rgba, 16, 16), 16, 16);
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 16; ++x) {
            for (int k = 0; k < 4; ++k) {
                const int i = (y * 16 + x) * 4 + k;
                if (y < 4 && x < 4 && k < 3) {
                    continue; // the clear block's colour is free
                }
                EXPECT_LE(std::abs(rgba[i] - back[i]), y < 4 ? 2 : 12) << x << "," << y << "," << k;
            }
        }
    }
    double mean;
    int max;
    Compare(rgba, back, mean, max);
    EXPECT_LT(mean, 30.0);
}

TEST(KeyGlyphs, GameAtlas) {
    const char* tpf_path = std::getenv("BBVR_KEYGLYPH_TPF");
    const char* font_path = std::getenv("BBVR_KEYGLYPH_FONT");
    if (tpf_path == nullptr || font_path == nullptr) {
        GTEST_SKIP() << "BBVR_KEYGLYPH_TPF / BBVR_KEYGLYPH_FONT not set";
    }
    auto tpf = ReadFile(tpf_path);
    const auto font_file = ReadFile(font_path);
    ASSERT_FALSE(tpf.empty());
    std::string why;
    const auto font = GameText::ParseGameFont(font_file, why);
    ASSERT_NE(font, nullptr) << why;
    const auto before = tpf;
    int written = 0;
    const std::string refused = KeyGlyphs::Apply(tpf, font.get(), written);
    EXPECT_EQ(refused, "");
    EXPECT_EQ(written, static_cast<int>(KeyGlyphs::Glyphs.size()));
    // Only the redrawn icons' bytes changed.
    std::size_t changed = 0;
    for (std::size_t i = 0; i < tpf.size(); ++i) {
        changed += tpf[i] != before[i];
    }
    EXPECT_LE(changed, KeyGlyphs::Glyphs.size() * 1024);
    const char* dump = std::getenv("BBVR_KEYGLYPH_DUMP");
    for (const auto& g : KeyGlyphs::Glyphs) {
        const auto t = TpfPatch::FindTexture(tpf, g.texture);
        ASSERT_TRUE(t.has_value());
        std::vector<u8> blocks(1024);
        for (std::uint32_t by = 0; by < 8; ++by) {
            for (std::uint32_t bx = 0; bx < 8; ++bx) {
                std::memcpy(blocks.data() + (by * 8 + bx) * 16,
                            tpf.data() + t->offset + TpfPatch::TiledBlockOffset(bx, by, 8, 16), 16);
            }
        }
        const auto decoded = Decode(blocks, 32, 32);
        const auto drawn = KeyGlyphs::Draw(g, font.get());
        double mean;
        int max;
        Compare(drawn, decoded, mean, max);
        std::string name(g.texture.begin(), g.texture.end());
        std::printf("  %s (%s): mean %.2f, max %d\n", name.c_str(), g.text, mean, max);
        EXPECT_LT(mean, 2.0) << name;
        if (dump != nullptr) {
            std::ofstream(std::string{dump} + "/" + name + ".rgba", std::ios::binary)
                .write(reinterpret_cast<const char*>(decoded.data()),
                       static_cast<std::streamsize>(decoded.size()));
        }
    }
}

TEST(KeyGlyphs, ScaledBadgeFillsItsCell) {
    const KeyGlyphs::Glyph menu{u"", KeyGlyphs::Shape::Menu, ""};
    const auto rgba = KeyGlyphs::Draw(menu, nullptr, 26, 25.5f / 26.0f);
    ASSERT_EQ(rgba.size(), 26u * 26 * 4);
    EXPECT_EQ(rgba[(13 * 26 + 13) * 4 + 3], 255); // the centre
    EXPECT_GT(rgba[(13 * 26 + 0) * 4 + 3], 100);  // the disc reaches the cell's edges
    EXPECT_EQ(rgba[(0 * 26 + 0) * 4 + 3], 0);     // not its corners
}

TEST(KeyGlyphs, GameHudAtlases) {
    const char* tpf_path = std::getenv("BBVR_KEYGLYPH_TPF");
    const char* font_path = std::getenv("BBVR_KEYGLYPH_FONT");
    if (tpf_path == nullptr || font_path == nullptr) {
        GTEST_SKIP() << "BBVR_KEYGLYPH_TPF / BBVR_KEYGLYPH_FONT not set";
    }
    auto tpf = ReadFile(tpf_path);
    const auto font_file = ReadFile(font_path);
    std::string why;
    const auto font = GameText::ParseGameFont(font_file, why);
    ASSERT_NE(font, nullptr) << why;
    const auto before = tpf;
    int written = 0;
    EXPECT_EQ(KeyGlyphs::ApplyHud(tpf, font.get(), false, written), "");
    EXPECT_EQ(written, static_cast<int>(KeyGlyphs::HudGlyphs.size()));
    // Only the blocks under the four cells changed (each at most 8 x 8 blocks).
    std::size_t changed = 0;
    for (std::size_t i = 0; i < tpf.size(); ++i) {
        changed += tpf[i] != before[i];
    }
    EXPECT_GT(changed, 0u);
    EXPECT_LE(changed, KeyGlyphs::HudGlyphs.size() * 64 * 16);
    const char* dump = std::getenv("BBVR_KEYGLYPH_DUMP");
    int n = 0;
    for (const auto& h : KeyGlyphs::HudGlyphs) {
        const auto t = TpfPatch::FindTexture(tpf, h.texture);
        ASSERT_TRUE(t.has_value());
        // The cell, decoded: our badge (opaque in the middle, a light letter on a dark disc).
        const std::uint32_t bw = t->width / 4;
        std::vector<u8> cell(static_cast<std::size_t>(h.size) * h.size * 4);
        for (std::uint32_t y = 0; y < h.size; ++y) {
            for (std::uint32_t x = 0; x < h.size; ++x) {
                const std::uint32_t X = h.x + x, Y = h.y + y;
                u8 texels[64];
                Bc7::DecodeBlock(tpf.data() + t->offset +
                                     TpfPatch::TiledBlockOffset(X / 4, Y / 4, bw, 16),
                                 texels);
                std::memcpy(&cell[(y * h.size + x) * 4], texels + ((Y % 4) * 4 + X % 4) * 4, 4);
            }
        }
        const std::size_t mid = (h.size / 2 * h.size + h.size / 2) * 4;
        EXPECT_EQ(cell[mid + 3], 255);
        int light = 0;
        for (std::size_t i = 0; i < cell.size(); i += 4) {
            light += cell[i + 3] > 200 && cell[i] > 180;
        }
        EXPECT_GT(light, 15) << h.game; // the letter
        if (dump != nullptr) {
            std::ofstream(std::string{dump} + "/hud" + std::to_string(n) + "_" +
                              std::to_string(h.size) + ".rgba",
                          std::ios::binary)
                .write(reinterpret_cast<const char*>(cell.data()),
                       static_cast<std::streamsize>(cell.size()));
        }
        ++n;
    }
    if (dump != nullptr) {
        std::ofstream(std::string{dump} + "/common_hud.tpf", std::ios::binary)
            .write(reinterpret_cast<const char*>(tpf.data()),
                   static_cast<std::streamsize>(tpf.size()));
    }
    // swap_sticks: the face buttons trade hands.
    EXPECT_STREQ(Thumbs::LabelFor(KeyGlyphs::HudGlyphs[1].logical, true), "Y");
}

} // namespace
