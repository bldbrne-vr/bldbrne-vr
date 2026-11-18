// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/vr/animated_item_asset.h"
#include "core/vr/beast_roar.h"
namespace VR::BeastRoar {
inline std::shared_ptr<Assets::Model> Decode(std::span<const u8> data) {
    return AnimatedItem::Decode<Model, AtlasSize, AtlasLevels, MaxVertices, MaxIndices, true>(data);
}
} // namespace VR::BeastRoar
