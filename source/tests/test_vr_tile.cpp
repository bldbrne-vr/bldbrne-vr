// SPDX-License-Identifier: GPL-2.0-or-later

// The VR tile in Bloodborne's Options grid: the movie patch (core/vr/gfx_tile.h) and the eboot
// patch sites (core/vr/vr_tile_patch.h). Optional game-data tests use the configured
// game directory and skip validation when external data is unavailable.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#ifdef BBVR_GAME_DATA_TESTS
#include <miniz.h>
#endif

#include "core/vr/gfx_tile.h"
#include "core/vr/gfx_area_name.h"
#include "core/vr/gfx_title.h"
#include "core/vr/tile_icon_compose.h"
#include "core/vr/tpf_patch.h"
#include "core/vr/vr_tile_patch.h"

namespace {

using namespace VR::GfxTile;
using namespace VR::GfxTile::Detail;

#ifndef VR_TILE_GAME_DIR
#define VR_TILE_GAME_DIR "./ROM/"
#endif

std::optional<std::vector<u8>> Load(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("  (skipped: %s not found)\n", path);
        return std::nullopt;
    }
    return std::vector<u8>(std::istreambuf_iterator<char>(f), {});
}

std::optional<std::vector<u8>> RetailMovie() {
    return Load(VR_TILE_GAME_DIR "CUSA03173/dvdroot_ps4/menu/ingametop.gfx");
}

// A bitmap icon payload of our own (AddTile embeds it as given; the emulator itself puts the icon
// into the game's atlas instead).
constexpr u16 TestIconSize = 80;
constexpr std::array<u8, 24> TestIconZlib = {0x78, 0x9c, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
                                             0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e,
                                             0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16};

Icon TheIcon() {
    return Icon{TestIconSize, TestIconSize, TestIconZlib.data(), TestIconZlib.size()};
}

std::size_t HeaderEnd(const std::vector<u8>& d) {
    return 8 + (5 + 4 * (d[8] >> 3) + 7) / 8 + 4;
}

// The placements of a sprite by id.
std::vector<Place> Places(const std::vector<u8>& d, u16 sprite) {
    std::vector<Tag> tags;
    ReadTags(d, HeaderEnd(d), d.size(), tags);
    for (const Tag& tag : tags) {
        if (tag.type == 39 && (d[tag.body] | (d[tag.body + 1] << 8)) == sprite) {
            std::vector<Tag> inner;
            ReadTags(d, tag.body + 4, tag.body + tag.length, inner);
            std::vector<Place> out;
            for (const Tag& t : inner) {
                if (auto p = ReadPlace(d, t)) {
                    out.push_back(*p);
                }
            }
            return out;
        }
    }
    return {};
}

const Place* Named(const std::vector<Place>& places, const std::string& name) {
    for (const Place& p : places) {
        if (p.name == name) {
            return &p;
        }
    }
    return nullptr;
}

} // namespace

TEST(VrTile, BitsRoundTrip) {
    BitWriter w;
    WriteMatrix(w, std::make_pair(s32{0x140000}, s32{-0x8000}), -1234, 5678);
    const auto bytes = w.Bytes();
    BitReader r{bytes, 0};
    const Matrix m = ReadMatrix(r);
    EXPECT_TRUE(r.Ok());
    EXPECT_NEAR(m.sx, 20.0, 1e-9);
    EXPECT_NEAR(m.sy, -0.5, 1e-9);
    EXPECT_EQ(m.tx, -1234);
    EXPECT_EQ(m.ty, 5678);
    EXPECT_EQ(SignedBits({0}), 1);
    EXPECT_EQ(SignedBits({1}), 2);
    EXPECT_EQ(SignedBits({-1}), 2);
    EXPECT_EQ(SignedBits({1620, -1620}), 12);
}

TEST(VrTile, LeaDisplacement) {
    using namespace VR::TilePatch;
    // The game's own lea at 0x1BDE5DF points at L"Debug" 0x49CE5A2.
    const auto own = LeaTo(0x1BDE5DF, 0x49CE5A2);
    ASSERT_TRUE(own.has_value());
    for (std::size_t i = 0; i < LeaLength; ++i) {
        EXPECT_EQ((*own)[i], LabelLeaSig[i]);
    }
    // ... and at 0x1BE29C0 (the stock handler).
    const auto handler = LeaTo(0x1BDE7F2, 0x1BE29C0);
    ASSERT_TRUE(handler.has_value());
    EXPECT_TRUE(std::equal(handler->begin(), handler->end(), HandlerLeaSig.begin()));
    // Backwards and the +-2 GB limits.
    const auto back = LeaTo(0x80000000ull, 0x1000);
    ASSERT_TRUE(back.has_value());
    EXPECT_EQ((*back)[6], 0x80);
    EXPECT_TRUE(LeaTo(0x1000, 0x1000 + 7 + 0x7FFFFFFFull).has_value());
    EXPECT_FALSE(LeaTo(0x1000, 0x1000 + 7 + 0x80000000ull).has_value());
    EXPECT_FALSE(LeaTo(0x100000000ull, 0x1000).has_value());
}

TEST(VrTile, RetailMovieGetsTheTile) {
    const auto movie = RetailMovie();
    if (!movie) {
        return;
    }
    EXPECT_EQ(movie->size(), 11248u);
    EXPECT_EQ(Fnv1a64(*movie), 0x501d498bcf9fb5a8ull);
    const Result r = AddTile(*movie, TheIcon());
    ASSERT_TRUE(r.error.empty());
    ASSERT_GT(r.movie.size(), movie->size() + TestIconZlib.size());
    // Header length follows the file.
    const u32 length = r.movie[4] | (r.movie[5] << 8) | (r.movie[6] << 16) | (u32{r.movie[7]} << 24);
    EXPECT_EQ(length, r.movie.size());
    EXPECT_EQ(r.bitmap_id, 75);
    EXPECT_EQ(r.shape_id, 76);
    EXPECT_EQ(r.sprite_id, 77);
    // Top: Item_0_3 one column right of Item_0_2 (89.35 px), same row, its own depth, our sprite.
    const auto top = Places(r.movie, TopSpriteId);
    const Place* system = Named(top, "Item_0_2");
    const Place* tile = Named(top, "Item_0_3");
    ASSERT_TRUE(system != nullptr && tile != nullptr);
    EXPECT_EQ(tile->depth, NewDepth);
    EXPECT_EQ(*tile->character, r.sprite_id);
    EXPECT_EQ(tile->matrix.ty, system->matrix.ty);
    EXPECT_NEAR((tile->matrix.tx - system->matrix.tx) / 20.0, 89.35, 0.1);
    EXPECT_NEAR(r.x, 704.0, 0.25);
    EXPECT_NEAR(r.y, 416.9, 0.1);
    // Every other placement of Top is unchanged (same count + 1).
    EXPECT_EQ(top.size(), Places(*movie, TopSpriteId).size() + 1);
    // The new tile: icon (depth 1), the game's frame #41 (depth 2), Cursor (depth 3).
    const auto inner = Places(r.movie, r.sprite_id);
    ASSERT_EQ(inner.size(), 3u);
    EXPECT_EQ(inner[0].depth, 1);
    EXPECT_EQ(*inner[0].character, r.shape_id);
    EXPECT_EQ(inner[1].depth, 2);
    EXPECT_EQ(*inner[1].character, 41);
    EXPECT_EQ(inner[2].depth, 3);
    EXPECT_STREQ(inner[2].name.c_str(), "Cursor");
    EXPECT_EQ(*inner[2].character, 44);
    // The game's own tile sprite #45 is untouched (frame still at depth 1).
    const auto own = Places(r.movie, TileSpriteId);
    ASSERT_EQ(own.size(), 2u);
    EXPECT_EQ(own[0].depth, 1);
    // The bitmap tag carries the icon as given.
    std::vector<Tag> tags;
    ASSERT_TRUE(ReadTags(r.movie, HeaderEnd(r.movie), r.movie.size(), tags));
    bool bitmap = false;
    for (const Tag& tag : tags) {
        if (tag.type == 36) {
            bitmap = true;
            EXPECT_EQ(r.movie[tag.body + 2], 5);
            EXPECT_EQ(r.movie[tag.body + 3] | (r.movie[tag.body + 4] << 8), TestIconSize);
            EXPECT_EQ(tag.length, 7 + TestIconZlib.size());
            EXPECT_EQ(
                std::memcmp(&r.movie[tag.body + 7], TestIconZlib.data(), TestIconZlib.size()), 0);
        }
    }
    EXPECT_TRUE(bitmap);
}

TEST(VrTile, ParchmentFallback) {
    const auto movie = RetailMovie();
    if (!movie) {
        return;
    }
    Icon parchment;
    parchment.parchment = true;
    const Result r = AddTile(*movie, parchment);
    ASSERT_TRUE(r.error.empty());
    EXPECT_EQ(r.sprite_id, 75);
    EXPECT_EQ(r.bitmap_id, 0);
    const auto inner = Places(r.movie, r.sprite_id);
    ASSERT_EQ(inner.size(), 3u);
    EXPECT_EQ(*inner[0].character, ParchmentShapeId);
    EXPECT_NEAR(inner[0].matrix.sx, IconSize / 80.0, 1e-4);
    // #46's left / top edge (4.0, 0.2) lands on the icon box's (IconX, IconY).
    EXPECT_NEAR(inner[0].matrix.tx / 20.0 + 4.0 * inner[0].matrix.sx, IconX, 0.05);
    EXPECT_NEAR(inner[0].matrix.ty / 20.0 + 0.2 * inner[0].matrix.sy, IconY, 0.05);
    EXPECT_EQ(*inner[1].character, 41);
    EXPECT_STREQ(inner[2].name.c_str(), "Cursor");
    EXPECT_TRUE(Named(Places(r.movie, TopSpriteId), "Item_0_3") != nullptr);
    EXPECT_LT(r.movie.size(), movie->size() + 160);
}

TEST(VrTile, RefusesWhatIsNotTheGamesMovie) {
    const auto movie = RetailMovie();
    if (!movie) {
        return;
    }
    // Twice: the patched movie already has an Item_0_3.
    const Result once = AddTile(*movie, TheIcon());
    ASSERT_TRUE(once.error.empty());
    const Result twice = AddTile(once.movie, TheIcon());
    EXPECT_TRUE(twice.movie.empty());
    EXPECT_FALSE(twice.error.empty());
    // Truncated, a wrong length, a compressed movie, no icon.
    std::vector<u8> cut(movie->begin(), movie->begin() + 5000);
    EXPECT_TRUE(AddTile(cut, TheIcon()).movie.empty());
    std::vector<u8> bad = *movie;
    bad[4] ^= 1;
    EXPECT_TRUE(AddTile(bad, TheIcon()).movie.empty());
    std::vector<u8> cfx = *movie;
    cfx[0] = 'C';
    EXPECT_TRUE(AddTile(cfx, TheIcon()).movie.empty());
    EXPECT_TRUE(AddTile(*movie, Icon{}).movie.empty());
    // Every truncation point of the tag stream fails cleanly (no crash, no output).
    for (std::size_t n = 30; n < movie->size(); n += 97) {
        std::vector<u8> part(movie->begin(), movie->begin() + n);
        part[4] = static_cast<u8>(n);
        part[5] = static_cast<u8>(n >> 8);
        part[6] = 0;
        part[7] = 0;
        const Result r = AddTile(part, TheIcon());
        EXPECT_TRUE(r.movie.empty());
    }
}

TEST(VrTile, SignaturesMatchTheRealEboot) {
    using namespace VR::TilePatch;
    const auto elf = Load(VR_TILE_GAME_DIR "CUSA03173-mods/eboot.bin");
    if (!elf || elf->size() < 0x40 || (*elf)[0] != 0x7F) {
        return;
    }
    // Lay the PT_LOAD segments out at their virtual addresses (code and data).
    const auto u64at = [&](std::size_t o) {
        u64 v = 0;
        std::memcpy(&v, elf->data() + o, 8);
        return v;
    };
    const u64 phoff = u64at(0x20);
    const u16 phnum = static_cast<u16>((*elf)[0x38] | ((*elf)[0x39] << 8));
    std::vector<u8> module(0x5700000, 0);
    for (u16 i = 0; i < phnum; ++i) {
        const std::size_t p = phoff + 56 * i;
        const u32 type = (*elf)[p] | ((*elf)[p + 1] << 8);
        if (type != 1) {
            continue;
        }
        const u64 off = u64at(p + 8), va = u64at(p + 16), fsz = u64at(p + 32);
        ASSERT_LE(va + fsz, module.size());
        std::memcpy(module.data() + va, elf->data() + off, fsz);
    }
    EXPECT_STREQ(CheckTileSites(module.data(), module.size()).c_str(), "");
    EXPECT_TRUE(CheckRefreshSite(module.data(), module.size()));
    EXPECT_FALSE(TileSitesPatched(module.data(), module.size()));
    // One changed byte anywhere in the builder's branch fails the check.
    module[BranchRva + 0x100] ^= 0x01;
    EXPECT_FALSE(CheckTileSites(module.data(), module.size()).empty());
    module[BranchRva + 0x100] ^= 0x01;
    module[GateRva + 5] ^= 0x01;
    EXPECT_FALSE(CheckTileSites(module.data(), module.size()).empty());
    module[GateRva + 5] ^= 0x01;
    // The patch as installed reads back as patched.
    std::memcpy(module.data() + GateRva, GatePatch.data(), GatePatch.size());
    const u64 near = 0x5ED4000; // module-relative, as the near region
    std::memcpy(module.data() + LabelLeaRva, LeaTo(LabelLeaRva, near)->data(), LeaLength);
    std::memcpy(module.data() + HelpLeaRva, LeaTo(HelpLeaRva, near + 16)->data(), LeaLength);
    std::memcpy(module.data() + HandlerLeaRva, LeaTo(HandlerLeaRva, near + 96)->data(), LeaLength);
    EXPECT_TRUE(TileSitesPatched(module.data(), module.size()));
    EXPECT_FALSE(CheckTileSites(module.data(), module.size()).empty());
    EXPECT_EQ(module.size() >= MinModuleSize, true);
}

// ---- The icon from the game's own atlas (core/vr/tpf_patch.h, Icon::atlas; 2026-10-05) ----------

namespace {

// The first fill style of DefineShape `id`: its type, bitmap id and matrix.
struct Fill {
    u8 type{0};
    u16 bitmap{0};
    Matrix matrix;
};
std::optional<Fill> ShapeFill(const std::vector<u8>& d, u16 id) {
    std::vector<Tag> tags;
    ReadTags(d, HeaderEnd(d), d.size(), tags);
    for (const Tag& tag : tags) {
        if (tag.type != 2 || (d[tag.body] | (d[tag.body + 1] << 8)) != id) {
            continue;
        }
        BitReader b{d, tag.body + 2};
        const int n = static_cast<int>(b.Ub(5));
        for (int i = 0; i < 4; ++i) {
            b.Sb(n);
        }
        std::size_t p = b.Align();
        if (d[p++] != 1) {
            return std::nullopt;
        }
        Fill f;
        f.type = d[p++];
        f.bitmap = static_cast<u16>(d[p] | (d[p + 1] << 8));
        BitReader m{d, p + 2};
        f.matrix = ReadMatrix(m);
        return f;
    }
    return std::nullopt;
}

// A PS4 TPF with two textures: a small one and a BC1 1024 x 1024 atlas named `atlas` (tile mode
// 13, one level), every block black as the retail atlas's unused area (00000000 aaaaaaaa).
std::vector<u8> FakeTpf(const std::u16string& atlas, u8 format = 0) {
    const std::u16string names[2] = {u"KG_OK", atlas};
    const std::size_t entries = 0x10, names_at = entries + 2 * 0x24;
    std::vector<u8> d(names_at);
    std::memcpy(d.data(), "TPF\0", 4);
    d[8] = 2;
    d[0x0C] = 4;
    std::vector<std::size_t> name_offsets;
    for (const auto& n : names) {
        name_offsets.push_back(d.size());
        for (const char16_t c : n) {
            d.push_back(static_cast<u8>(c));
            d.push_back(static_cast<u8>(c >> 8));
        }
        d.push_back(0);
        d.push_back(0);
    }
    while (d.size() % 16 != 0) {
        d.push_back(0);
    }
    const u32 sizes[2] = {1024, 524288};
    const u16 dims[2] = {32, 1024};
    for (int i = 0; i < 2; ++i) {
        const u32 off = static_cast<u32>(d.size());
        for (u32 k = 0; k < sizes[i]; k += 8) {
            const u8 black[8] = {0, 0, 0, 0, 0xaa, 0xaa, 0xaa, 0xaa};
            d.insert(d.end(), black, black + 8);
        }
        u8* e = d.data() + entries + i * 0x24;
        std::memcpy(e, &off, 4);
        std::memcpy(e + 4, &sizes[i], 4);
        e[8] = i == 1 ? format : 102;
        e[10] = 1;
        std::memcpy(e + 12, &dims[i], 2);
        std::memcpy(e + 14, &dims[i], 2);
        const u32 one = 1, mode = 13;
        std::memcpy(e + 0x10, &one, 4);
        std::memcpy(e + 0x14, &mode, 4);
        const u32 name = static_cast<u32>(name_offsets[i]);
        std::memcpy(e + 0x18, &name, 4);
    }
    return d;
}

} // namespace

TEST(VrTile, AtlasIconInTheMovie) {
    const auto movie = RetailMovie();
    if (!movie) {
        return;
    }
    Icon atlas;
    atlas.atlas = true;
    atlas.atlas_x = 160;
    atlas.atlas_y = 640;
    const Result r = AddTile(*movie, atlas);
    ASSERT_TRUE(r.error.empty());
    EXPECT_EQ(r.bitmap_id, 4); // the movie's external image MENU_Common_00100
    EXPECT_EQ(r.shape_id, 75);
    EXPECT_EQ(r.sprite_id, 76);
    // No bitmap of our own: one shape and one sprite more, a few hundred bytes.
    std::vector<Tag> tags, own;
    ASSERT_TRUE(ReadTags(r.movie, HeaderEnd(r.movie), r.movie.size(), tags));
    ASSERT_TRUE(ReadTags(*movie, HeaderEnd(*movie), movie->size(), own));
    EXPECT_EQ(tags.size(), own.size() + 2);
    for (const Tag& tag : tags) {
        EXPECT_TRUE(tag.type != 36 && tag.type != 20 && tag.type != 21 && tag.type != 35);
    }
    EXPECT_LT(r.movie.size(), movie->size() + 200);
    // The shape: the atlas, not smoothed (0x42, as the game's own icons and #46), 1 texel per
    // stage px, the cell's corner on the icon box's.
    const auto fill = ShapeFill(r.movie, r.shape_id);
    ASSERT_TRUE(fill.has_value());
    EXPECT_EQ(fill->type, 0x42);
    EXPECT_EQ(fill->bitmap, 4);
    EXPECT_NEAR(fill->matrix.sx, 20.0, 1e-4);
    EXPECT_NEAR(fill->matrix.sy, 20.0, 1e-4);
    EXPECT_NEAR(fill->matrix.tx / 20.0, IconX - 160.0, 0.05);
    EXPECT_NEAR(fill->matrix.ty / 20.0, IconY - 640.0, 0.05);
    // The game's parchment #46 is filled the same way (its own cell at (720, 160)).
    const auto parchment = ShapeFill(*movie, ParchmentShapeId);
    ASSERT_TRUE(parchment.has_value());
    EXPECT_EQ(parchment->type, fill->type);
    EXPECT_EQ(parchment->bitmap, fill->bitmap);
    EXPECT_NEAR(parchment->matrix.sx, fill->matrix.sx, 1e-4);
    // The tile: our shape at depth 1, the game's frame and Cursor over it; Item_0_3 placed.
    const auto inner = Places(r.movie, r.sprite_id);
    ASSERT_EQ(inner.size(), 3u);
    EXPECT_EQ(*inner[0].character, r.shape_id);
    EXPECT_EQ(*inner[1].character, 41);
    EXPECT_STREQ(inner[2].name.c_str(), "Cursor");
    EXPECT_TRUE(Named(Places(r.movie, TopSpriteId), "Item_0_3") != nullptr);
    // A cell off the atlas, or not on a block boundary, is refused.
    Icon off = atlas;
    off.atlas_x = 1000;
    EXPECT_TRUE(AddTile(*movie, off).movie.empty());
    off = atlas;
    off.atlas_y = 642;
    EXPECT_TRUE(AddTile(*movie, off).movie.empty());
}

TEST(VrTile, AtlasCellWrittenTiled) {
    using namespace VR::TpfPatch;
    // The tiled offset is the inverse of the detiler's (bloodborne_assets.cpp DetileLevel).
    for (u32 e = 0; e < 64; ++e) {
        const u32 x = (e & 1) | ((e >> 1) & 2) | ((e >> 2) & 4);
        const u32 y = ((e >> 1) & 1) | ((e >> 2) & 2) | ((e >> 3) & 4);
        EXPECT_EQ(TiledBlockOffset(8 * 3 + x, 8 * 2 + y, 256, 8),
                  (std::size_t{2 * 32 + 3} * 64 + e) * 8);
    }
    std::vector<u8> tpf = FakeTpf(AtlasTexture);
    const std::vector<u8> before = tpf;
    std::vector<u8> blocks(3200);
    for (std::size_t i = 0; i < blocks.size(); ++i) {
        blocks[i] = static_cast<u8>(i * 7 + 1);
    }
    const std::string written = WriteBc1Cell(tpf, AtlasTexture, CellX, CellY, 80, 80, blocks);
    EXPECT_STREQ(written.c_str(), "");
    const auto t = FindTexture(tpf, AtlasTexture);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->width, 1024);
    EXPECT_EQ(t->tile_mode, 13u);
    EXPECT_TRUE(ReadBc1Cell(tpf, *t, CellX, CellY, 80, 80) == blocks);
    // Exactly 400 blocks changed, nothing else.
    std::size_t changed = 0;
    for (std::size_t i = 0; i < tpf.size(); i += 8) {
        changed += std::memcmp(tpf.data() + i, before.data() + i, 8) != 0;
    }
    EXPECT_EQ(changed, 400u);
    // A second write finds the cell in use; a wrong texture, format or cell is refused unchanged.
    std::vector<u8> again = tpf;
    const std::string refused = WriteBc1Cell(again, AtlasTexture, CellX, CellY, 80, 80, blocks);
    EXPECT_STREQ(refused.c_str(), "the cell is not empty");
    EXPECT_TRUE(again == tpf);
    std::vector<u8> fresh = FakeTpf(AtlasTexture);
    EXPECT_FALSE(WriteBc1Cell(fresh, u"MENU_Common_00101", CellX, CellY, 80, 80, blocks).empty());
    EXPECT_FALSE(WriteBc1Cell(fresh, AtlasTexture, 162, CellY, 80, 80, blocks).empty());
    EXPECT_FALSE(WriteBc1Cell(fresh, AtlasTexture, 960, CellY, 80, 80, blocks).empty());
    EXPECT_FALSE(WriteBc1Cell(fresh, AtlasTexture, CellX, CellY, 80, 80,
                              std::span<const u8>{blocks}.subspan(8))
                     .empty());
    EXPECT_TRUE(fresh == FakeTpf(AtlasTexture));
    std::vector<u8> bc7 = FakeTpf(AtlasTexture, 102);
    EXPECT_FALSE(WriteBc1Cell(bc7, AtlasTexture, CellX, CellY, 80, 80, blocks).empty());
}

// ---- The icon composited at install (core/vr/tile_icon_compose.h) --------------------------------

namespace TI = VR::TileIcon;

// A parchment of our own: a warm ground with a procedural grain, 80 x 80 RGB.
std::vector<u8> TestParchment(int seed) {
    std::vector<u8> rgb(static_cast<std::size_t>(TI::Size) * TI::Size * 3);
    u32 h = 2166136261u + static_cast<u32>(seed);
    for (int y = 0; y < TI::Size; ++y) {
        for (int x = 0; x < TI::Size; ++x) {
            h = (h ^ static_cast<u32>(x * 73 + y * 151)) * 16777619u;
            const int grain = static_cast<int>(h >> 27) - 16; // -16..15
            const int base = 150 + seed * 20 + ((x / 10 + y / 10) % 2) * 12 + grain;
            const std::size_t o = (static_cast<std::size_t>(y) * TI::Size + x) * 3;
            rgb[o] = static_cast<u8>(std::clamp(base + 20, 0, 255));
            rgb[o + 1] = static_cast<u8>(std::clamp(base, 0, 255));
            rgb[o + 2] = static_cast<u8>(std::clamp(base - 30, 0, 255));
        }
    }
    return rgb;
}

double Rms(const std::vector<u8>& a, const std::vector<u8>& b, const std::vector<bool>* mask) {
    double sum = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (mask != nullptr && !(*mask)[i / 3]) {
            continue;
        }
        sum += (static_cast<double>(a[i]) - b[i]) * (static_cast<double>(a[i]) - b[i]);
        ++n;
    }
    return n == 0 ? 0.0 : std::sqrt(sum / static_cast<double>(n));
}

TEST(VrTileIcon, Bc1EncodesAndDecodes) {
    // A flat block is exact (an RGB565 colour); a two-colour block is its two endpoints.
    double px[16][3];
    for (auto& p : px) {
        p[0] = 0x84; // 565-exact: r 16, g 33, b 8
        p[1] = 0x86;
        p[2] = 0x42;
    }
    auto b = TI::EncodeBlock(px);
    u8 out[16][3];
    TI::DecodeBlock(b.data(), out);
    for (const auto& o : out) {
        EXPECT_EQ(o[0], 0x84);
        EXPECT_EQ(o[1], 0x86);
        EXPECT_EQ(o[2], 0x42);
    }
    for (int i = 0; i < 16; ++i) {
        const bool dark = (i % 4) < 2;
        px[i][0] = dark ? 0x10 : 0xF7;
        px[i][1] = dark ? 0x20 : 0xDF;
        px[i][2] = dark ? 0x08 : 0x9C;
    }
    b = TI::EncodeBlock(px);
    TI::DecodeBlock(b.data(), out);
    const u16 c0 = static_cast<u16>(b[0] | (b[1] << 8));
    const u16 c1 = static_cast<u16>(b[2] | (b[3] << 8));
    EXPECT_GT(c0, c1); // 4-colour mode
    for (int i = 0; i < 16; ++i) {
        for (int k = 0; k < 3; ++k) {
            EXPECT_NEAR(out[i][k], px[i][k], 4.0) << "texel " << i << " ch " << k;
        }
    }
    // A cell round trip of a smooth gradient stays close.
    std::vector<u8> grad(static_cast<std::size_t>(TI::Size) * TI::Size * 3);
    for (int y = 0; y < TI::Size; ++y) {
        for (int x = 0; x < TI::Size; ++x) {
            const std::size_t o = (static_cast<std::size_t>(y) * TI::Size + x) * 3;
            grad[o] = static_cast<u8>(40 + 2 * x);
            grad[o + 1] = static_cast<u8>(30 + 2 * y);
            grad[o + 2] = static_cast<u8>(100 + x / 2 + y / 2);
        }
    }
    const auto cell = TI::EncodeCell(grad);
    ASSERT_EQ(cell.size(), TI::CellBytes);
    EXPECT_LT(Rms(TI::DecodeCell(cell), grad, nullptr), 3.0);
}

TEST(VrTileIcon, GogglesOverTheGivenParchment) {
    // Two different parchments: where our layer is opaque the icon is the goggles alone (the
    // same over both); where it is clear it is that parchment; the ground only ever comes in.
    const auto p1 = TestParchment(0);
    const auto p2 = TestParchment(2);
    std::string why;
    const auto i1 = TI::Build(TI::EncodeCell(p1), why);
    ASSERT_EQ(i1.size(), TI::CellBytes) << why;
    const auto i2 = TI::Build(TI::EncodeCell(p2), why);
    ASSERT_EQ(i2.size(), TI::CellBytes) << why;
    std::vector<bool> goggles(static_cast<std::size_t>(TI::Size) * TI::Size);
    std::vector<bool> clear(goggles.size());
    std::size_t n_goggles = 0, n_clear = 0;
    for (std::size_t p = 0; p < goggles.size(); ++p) {
        const double m = VR::GfxTile::GroundLo + VR::GfxTile::GogglesGround[p] / 255.0 *
                                                     (VR::GfxTile::GroundHi - VR::GfxTile::GroundLo);
        // Inside the goggles and away from the edge texels (the round trip's reach).
        const int x = static_cast<int>(p % TI::Size), y = static_cast<int>(p / TI::Size);
        const bool inner = x >= 4 && y >= 4 && x < TI::Size - 4 && y < TI::Size - 4;
        goggles[p] = inner && std::abs(m) < 0.02;
        clear[p] = inner && m > 0.98;
        n_goggles += goggles[p];
        n_clear += clear[p];
    }
    EXPECT_GT(n_goggles, 1000u); // the goggles cover a good part of the tile
    EXPECT_GT(n_clear, 1000u);   // and leave the parchment around them
    const auto d1 = TI::DecodeCell(i1);
    const auto d2 = TI::DecodeCell(i2);
    EXPECT_LT(Rms(d1, d2, &goggles), 6.0); // the goggles: the same over either ground
    EXPECT_GT(Rms(d1, d2, &clear), 25.0);  // around them: the two grounds (40 levels apart)
    EXPECT_LT(Rms(d1, p1, &clear), 12.0);  // ... each its own parchment (grain kept, BC1 error)
    EXPECT_LT(Rms(d2, p2, &clear), 12.0);
    // The composite before BC1: the same split.
    const auto c1 = TI::Compose(p1);
    const auto c2 = TI::Compose(p2);
    EXPECT_LT(Rms(c1, c2, &goggles), 2.5);
}

TEST(VrTileIcon, RefusesWhatIsNotParchment) {
    std::string why;
    // The atlas's unused area: every block black.
    std::vector<u8> black(TI::CellBytes, 0);
    for (std::size_t i = 4; i < black.size(); i += 8) {
        std::memset(&black[i], 0xAA, 4);
    }
    EXPECT_TRUE(TI::Build(black, why).empty());
    EXPECT_FALSE(why.empty());
    // A flat colour (a mod's blank cell).
    std::vector<u8> flat(static_cast<std::size_t>(TI::Size) * TI::Size * 3, 128);
    EXPECT_TRUE(TI::Build(TI::EncodeCell(flat), why).empty());
    // Not a whole cell.
    EXPECT_TRUE(TI::Build(std::span<const u8>{black}.subspan(8), why).empty());
}

// The user's own atlas (game-data tier, skipped without the dump): the icon composited over its
// parchment is the one the emulator puts in (FNV-1a 64 of its 400 blocks: a hash only), every block
// in 4-colour mode, and it goes into the free cell.
TEST(VrTileIcon, ComposedOverTheGamesParchment) {
    using namespace VR::TpfPatch;
    const auto file = Load(VR_TILE_GAME_DIR "CUSA03173/dvdroot_ps4/menu/common.tpf.dcx");
    if (!file) {
        return;
    }
#ifdef BBVR_GAME_DATA_TESTS
    const auto dcx = ParseDcx(*file);
    ASSERT_TRUE(dcx.has_value());
    std::vector<u8> tpf(dcx->unpacked);
    mz_ulong length = dcx->unpacked;
    ASSERT_EQ(mz_uncompress(tpf.data(), &length, file->data() + dcx->stream, dcx->packed), MZ_OK);
    const auto t = FindTexture(tpf, AtlasTexture);
    ASSERT_TRUE(t.has_value());
    std::string why;
    const auto icon =
        TI::Build(ReadBc1Cell(tpf, *t, TI::ParchmentX, TI::ParchmentY, TI::Size, TI::Size), why);
    ASSERT_EQ(icon.size(), TI::CellBytes) << why;
    EXPECT_EQ(Fnv1a64(icon), 0xf33851762c1d6afbull);
    std::size_t four_colour = 0;
    for (std::size_t i = 0; i < icon.size(); i += 8) {
        four_colour += (icon[i] | (icon[i + 1] << 8)) > (icon[i + 2] | (icon[i + 3] << 8));
    }
    EXPECT_EQ(four_colour, 400u);
    const std::string written =
        WriteBc1Cell(tpf, AtlasTexture, CellX, CellY, TI::Size, TI::Size, icon);
    EXPECT_STREQ(written.c_str(), "");
#else
    std::printf("  (the atlas is there: build with BBVR_GAME_DATA_TESTS to check the icon on it)\n");
#endif
}

TEST(VrTile, AtlasDcxHeader) {
    using namespace VR::TpfPatch;
    const auto file = Load(VR_TILE_GAME_DIR "CUSA03173/dvdroot_ps4/menu/common.tpf.dcx");
    if (!file) {
        return;
    }
    EXPECT_EQ(file->size(), AtlasFileSize);
    const auto dcx = ParseDcx(*file);
    ASSERT_TRUE(dcx.has_value());
    EXPECT_EQ(dcx->unpacked, 31276144u);
    EXPECT_EQ(dcx->packed, 9415075u);
    EXPECT_EQ(dcx->stream, 0x4Cu);
    EXPECT_EQ((*file)[dcx->stream], 0x78); // a zlib stream
    const std::vector<u8> stream = {0x78, 0x9c, 1, 2, 3, 4, 5, 6, 7, 8};
    const auto out = RebuildDcx(*file, *dcx, stream, 31276144u);
    ASSERT_EQ(out.size(), 0x4Cu + stream.size());
    const auto again = ParseDcx(out);
    ASSERT_TRUE(again.has_value());
    EXPECT_EQ(again->packed, 10u);
    EXPECT_EQ(again->unpacked, 31276144u);
    // Everything but the packed size is the game's own header.
    for (std::size_t i = 0; i < 0x4C; ++i) {
        if (i < dcx->dcs + 8 || i >= dcx->dcs + 12) {
            EXPECT_EQ(out[i], (*file)[i]);
        }
    }
    std::vector<u8> bad = *file;
    bad[0] = 'X';
    EXPECT_FALSE(ParseDcx(bad).has_value());
}

TEST(VrAreaName, RejectsWhatIsNotTheHudMovie) {
    EXPECT_TRUE(VR::GfxAreaName::ClearDim({}).movie.empty());
    const std::vector<u8> invalid(4096, 0);
    EXPECT_TRUE(VR::GfxAreaName::ClearDim(invalid).movie.empty());
}

TEST(VrAreaName, OnlyTheDimStripsColoursTurnWhite) {
    const auto original = Load(VR_TILE_GAME_DIR "CUSA03173/dvdroot_ps4/menu/fe.gfx");
    if (!original) {
        GTEST_SKIP() << "The HUD movie is not installed";
    }
    const auto result = VR::GfxAreaName::ClearDim(*original);
    ASSERT_FALSE(result.movie.empty()) << result.why;
    ASSERT_EQ(result.movie.size(), original->size());
    EXPECT_EQ(result.colours, 2); // white at the top, black at the bottom
    // Six colour bytes change (two RGB records); everything else is the game's own.
    std::vector<std::size_t> changed;
    for (std::size_t i = 0; i < original->size(); ++i) {
        if (result.movie[i] != (*original)[i]) {
            changed.push_back(i);
            EXPECT_EQ(result.movie[i], 0xFF);
        }
    }
    EXPECT_EQ(changed.size(), 3u); // the top record is white already
    ASSERT_FALSE(changed.empty());
    EXPECT_EQ(changed.back() - changed.front(), 2u);
    // Patching the patched movie changes nothing more.
    const auto again = VR::GfxAreaName::ClearDim(result.movie);
    EXPECT_EQ(again.movie, result.movie);
}

TEST(VrAreaName, LeavesAMovieWithAnotherBlendAlone) {
    const auto original = Load(VR_TILE_GAME_DIR "CUSA03173/dvdroot_ps4/menu/fe.gfx");
    if (!original) {
        GTEST_SKIP() << "The HUD movie is not installed";
    }
    // The multiply byte of MapName's depth-1 PlaceObject3 (flags 0x0e 0x02, depth 1) made NORMAL.
    std::vector<u8> modded = *original;
    const auto first = VR::GfxAreaName::ClearDim(modded);
    ASSERT_FALSE(first.movie.empty());
    bool found = false;
    for (std::size_t i = 0; i + 14 <= modded.size(); ++i) {
        const u32 code = modded[i] | (modded[i + 1] << 8);
        if ((code >> 6) == 70 && (code & 0x3F) == 14 && modded[i + 2] == 0x0e &&
            modded[i + 3] == 0x02 && modded[i + 4] == 1 && modded[i + 5] == 0 &&
            modded[i + 15] == 3) {
            modded[i + 15] = 0;
            found = true;
            break;
        }
    }
    ASSERT_TRUE(found);
    const auto result = VR::GfxAreaName::ClearDim(modded);
    EXPECT_TRUE(result.movie.empty());
}

TEST(VrTitle, RejectsUnknownOrModifiedMovies) {
    EXPECT_TRUE(VR::GfxTitle::AddLabel({}).empty());
    const std::vector<u8> invalid(9099, 0);
    EXPECT_TRUE(VR::GfxTitle::AddLabel(invalid).empty());
}

TEST(VrTitle, RetailVariantsKeepTimelineAndPlaceLabelInsideFadingTop) {
    for (const char* name : {"title.gfx", "title_dlc.gfx", "title_dlc_eu.gfx"}) {
        SCOPED_TRACE(name);
        const auto path = std::string{VR_TILE_GAME_DIR "CUSA03173/dvdroot_ps4/menu/"} + name;
        const auto original = Load(path.c_str());
        if (!original) {
            GTEST_SKIP() << "Retail title movies are not installed";
        }
        const auto patched = VR::GfxTitle::AddLabel(*original);
        ASSERT_FALSE(patched.empty());
        EXPECT_EQ(patched[4] | (patched[5] << 8) | (patched[6] << 16) |
                      (u32{patched[7]} << 24), patched.size());
        EXPECT_TRUE(VR::GfxTitle::AddLabel(patched).empty());
        auto modified = *original;
        modified.back() ^= 1;
        EXPECT_TRUE(VR::GfxTitle::AddLabel(modified).empty());

        std::vector<Tag> before, after;
        ASSERT_TRUE(ReadTags(*original, HeaderEnd(*original), original->size(), before));
        ASSERT_TRUE(ReadTags(patched, HeaderEnd(patched), patched.size(), after));
        ASSERT_EQ(after.size(), before.size() + 1);
        std::size_t source = 0;
        bool text_found = false, label_found = false;
        for (const auto& tag : after) {
            const auto id = tag.length >= 2 ? patched[tag.body] | (patched[tag.body + 1] << 8) : 0;
            if (tag.type == 37 && id == 44) {
                text_found = true;
                const std::string text(patched.begin() + tag.body,
                                       patched.begin() + tag.body + tag.length);
                EXPECT_NE(text.find("MenuFont_01"), std::string::npos);
                EXPECT_NE(text.find("<b>VR</b>"), std::string::npos);
                continue;
            }
            ASSERT_LT(source, before.size());
            const auto& old = before[source++];
            EXPECT_EQ(tag.type, old.type);
            if (tag.type == 39 && id == 43) {
                std::vector<Tag> children, old_children;
                ASSERT_TRUE(ReadTags(patched, tag.body + 4, tag.body + tag.length, children));
                ASSERT_TRUE(ReadTags(*original, old.body + 4, old.body + old.length, old_children));
                ASSERT_EQ(children.size(), old_children.size() + 1);
                std::size_t old_index = 0;
                for (const auto& child : children) {
                    const auto place = ReadPlace(patched, child);
                    if (place && place->name == "VrLabel") {
                        EXPECT_FALSE(label_found);
                        label_found = true;
                        EXPECT_EQ(place->character, 44);
                        EXPECT_EQ(place->depth, 39);
                        EXPECT_EQ(place->matrix.tx, 125 * 20);
                        EXPECT_EQ(place->matrix.ty, 906 * 20);
                        continue;
                    }
                    ASSERT_LT(old_index, old_children.size());
                    const auto& old_child = old_children[old_index++];
                    EXPECT_EQ(std::vector<u8>(patched.begin() + child.start,
                                             patched.begin() + child.body + child.length),
                              std::vector<u8>(original->begin() + old_child.start,
                                             original->begin() + old_child.body + old_child.length));
                }
                EXPECT_EQ(old_index, old_children.size());
            } else {
                EXPECT_EQ(std::vector<u8>(patched.begin() + tag.start,
                                         patched.begin() + tag.body + tag.length),
                          std::vector<u8>(original->begin() + old.start,
                                         original->begin() + old.body + old.length));
            }
        }
        EXPECT_EQ(source, before.size());
        EXPECT_TRUE(text_found);
        EXPECT_TRUE(label_found);
    }
}
