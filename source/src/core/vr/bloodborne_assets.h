// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "common/types.h"

// Bloodborne's own item names and icons for the quick-item arc, read from the game's files (never
// shipped with the emulator):
//  - names: dvdroot_ps4/msg/<language>/item.msgbnd.dcx (DCX DFLT -> BND4 -> the goods-name FMG,
//    file id 10), the console language's folder (engus when it has none);
//  - icons: dvdroot_ps4/menu/menu_knowledge_<iconId>.tpf.dcx (DCX -> PS4 TPF -> one 400 x 400
//    BC7 texture, padded to 416 x 416 and micro-tiled (tile mode 13: 8 x 8 blocks in Morton
//    order, tiles row by row); detiled here, uploaded as BC7 by the in-frame renderer);
//  - text: labels rasterised from the game's own font outlines (menu/<language>/font.gfx), or
//    from the emulator's embedded Noto Sans (CJK for Japanese, Korean and Chinese) when the game
//    font lacks a character, with a soft halo, as a two-channel (fill, halo) bitmap;
//  - held items: the game's own item models and textures (sfx archive, see GetModel).
// Everything loads on one background thread; until something is ready the callers get nothing
// (a card without its icon, no label) and ask again later.
namespace VR::Assets {

// The goods name in the console language ("" until loaded, or unknown).
[[nodiscard]] std::string GoodsName(s32 goods);

struct Icon {
    u32 width{0};  // texels (a multiple of 4, the padded size)
    u32 height{0};
    u32 used{0};   // the icon's own size inside (400 of 416)
    std::vector<u8> bc7; // linear blocks, row by row (16 bytes per 4 x 4 block)
};
// The icon's BC7 data (nullptr until decoded or when it failed; the first call requests it).
[[nodiscard]] std::shared_ptr<const Icon> GetIcon(s32 icon_id);

struct LabelBitmap {
    u32 width{0};
    u32 height{0};
    std::vector<u8> rg; // two bytes per texel: fill coverage, halo coverage (soft, for the shadow)
};
// Rasterises `utf8` at `px` (glyph em size) into `out`: in the game's own HUD and menu font when
// every character has a glyph there (dvdroot_ps4/menu/<language>/font.gfx: a Scaleform font
// library, one DefineFont3 tag of quadratic glyph outlines, "TT-Reimin Y10 Bold" in the western
// languages), else in Noto Sans. False until a font is ready (call again later) or when the text
// is empty. Any thread; at most a millisecond or two.
bool RasterizeLabel(const char* utf8, u32 px, LabelBitmap& out);

// A held item's model from the game's own effect library (dvdroot_ps4/sfx/
// common and DLC effect archives, DCX -> BND4): sNNNNN.flver (FLVER2, all meshes,
// each mesh's first non-LOD face set, the default bone's transform applied: metres, the model's own axes, +Y along
// the item as authored) and its diffuse texture sNNNNN.tpf (PS4 TPF, BC1 / BC3 / BC7 with the
// game's mip chain, micro-tiled: detiled here to linear blocks, level after level).
struct ModelVertex {
    float position[3];
    float normal[3];
    float uv[2];
};
static_assert(sizeof(ModelVertex) == 32); // the in-frame renderer's vertex layout

struct ModelTexture {
    enum class Format : u8 { None, Bc1, Bc3, Bc7 }; // all sRGB colour
    Format format{Format::None};
    u32 width{0}; // level 0 (a multiple of 4)
    u32 height{0};
    u32 levels{0};
    std::vector<u32> level_offset; // into blocks
    std::vector<u8> blocks;        // every level's blocks, row by row
};

struct Model {
    s32 id{0};
    std::vector<ModelVertex> vertices;
    std::vector<u32> indices; // a triangle list
    ModelTexture diffuse;     // Format::None: none found (the renderer shades it plain)
    float min[3]{};
    float max[3]{};
};
// The model (nullptr until loaded, or when the game files have no such model; the first call
// requests it, and with it every model of the held-item table, from one read of the archive).
[[nodiscard]] std::shared_ptr<const Model> GetModel(s32 model_id);

// Starts the loader (names, font) once; GoodsName / GetIcon start it too.
void Start();

// A texture of the game's menu atlas file menu/common.tpf.dcx (the VR settings page draws the
// game's own frames, ornaments and key-guide glyphs from it): BC7 decoded to RGBA8, straight
// alpha, row by row. nullptr until loaded (the first call requests it; RequestMenuTextures asks
// for several from one read of the file), or when the file has no such texture (logged).
struct MenuTexture {
    u32 width{0};
    u32 height{0};
    std::vector<u8> rgba;
};
[[nodiscard]] std::shared_ptr<const MenuTexture> GetMenuTexture(const std::string& name);
void RequestMenuTextures(const std::vector<std::string>& names);

} // namespace VR::Assets

namespace VR::GameText {
struct GameFont;
}

namespace VR::Assets {

// The game's own menu font once loaded (core/vr/game_font.h; never freed), else nullptr (the
// loader is started).
[[nodiscard]] const GameText::GameFont* GetGameFont();
// The VR pages' font (vr_menu=1; their text is English): the English menu font when the console
// language's font is another file (Korean, Chinese), else GetGameFont()'s. Ready with it.
[[nodiscard]] const GameText::GameFont* GetPageFont();

} // namespace VR::Assets
