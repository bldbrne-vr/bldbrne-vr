// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/vr/animated_item_asset.h"
#include "core/vr/augur_creature.h"
namespace VR::AugurCreature {
inline std::shared_ptr<Assets::Model> Decode(std::span<const u8> data) {
    return AnimatedItem::Decode<Model,TextureSize,TextureLevels,MaxVertices,MaxIndices,true,Bones>(data);
}
}
