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
#include <span>
#include <string_view>
#include <optional>
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

// A DCX file's contents (DFLT: zlib), nullopt when it is not one or does not unpack.
[[nodiscard]] std::optional<std::vector<u8>> DecompressDcx(const std::vector<u8>& file);

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
    enum class Format : u8 { None, Bc1, Bc3, Bc7, Rgba8 }; // diffuse sRGB; surface linear
    Format format{Format::None};
    u32 width{0}; // level 0 (a multiple of 4)
    u32 height{0};
    u32 levels{0};
    std::vector<u32> level_offset; // into blocks
    std::vector<u8> blocks;        // every level's blocks, row by row
    // The same blocks held once for many models (set: `blocks` empty): a severed enemy's pieces
    // share its decoded texture instead of a copy each (the review, 2026-11-16). Never changed
    // once made; read through Bytes().
    std::shared_ptr<const std::vector<u8>> shared_blocks;
    // Nonzero: every model whose texture has this key shares one GPU image (the same content:
    // a severed piece's texture of one enemy, the flesh tile), uploaded once.
    u64 share_key{0};

    const std::vector<u8>& Bytes() const {
        return shared_blocks ? *shared_blocks : blocks;
    }
    // Its blocks moved into one shared buffer (copies of it then share them).
    void Share() {
        if (!shared_blocks) {
            shared_blocks = std::make_shared<const std::vector<u8>>(std::move(blocks));
            blocks = {};
        }
    }
};

struct Model {
    s32 id{0};
    std::vector<ModelVertex> vertices;
    std::vector<u32> indices; // a triangle list
    std::vector<float> animation; // immutable vec4 metadata + baked palette frames
    u32 animation_bones{16};       // decoder-validated, at most 32
    bool single_surface{false};   // animated UVs directly address one repeating material
    ModelTexture surface;         // animated model: tangent normal RGB, roughness A
    ModelTexture diffuse;     // Format::None: none found (the renderer shades it plain)
    float min[3]{};
    float max[3]{};
};
// The model (nullptr until loaded, or when the game files have no such model; the first call
// requests it, and with it every model of the held-item table, from one read of the archive).
[[nodiscard]] std::shared_ptr<const Model> GetModel(s32 model_id);

// Runtime models (core/vr/bloodborne_sever.cpp: a Beast Claw's severed piece and its flesh): put
// under an id from RuntimeModelBase up (never a held item's), served by GetModel like any model
// (the in-frame renderer uploads it on first use), dropped when done. Never queued for loading.
inline constexpr s32 RuntimeModelBase = 0x40000000;
void PutModel(s32 model_id, std::shared_ptr<const Model> model);
void DropModel(s32 model_id);
// A renderer's GPU copies of runtime models: freed as they are dropped. The count of drops so far
// (it checks its copies when it changes), and of `ids` (runtime models' ids), those no longer put
// kept, the rest removed (one lock for all).
[[nodiscard]] u32 DroppedModels();
void KeepDroppedModels(std::vector<s32>& ids);

// A PS4 TPF's texture by name (a character bnd's: "c2000_head_a"; case ignored) with its mip
// chain: BC1, BC3 or BC7, tile mode 13. False and why when it is not there or not handled.
bool DecodeTpfTexture(const std::vector<u8>& tpf, std::string_view name, ModelTexture& out,
                      std::string& why);

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
