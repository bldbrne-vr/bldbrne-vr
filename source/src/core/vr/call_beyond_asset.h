// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/vr/animated_item_asset.h"
#include "core/vr/call_beyond.h"
namespace VR::CallBeyond {
inline std::shared_ptr<Assets::Model> Decode(std::span<const u8> data) {
    return AnimatedItem::Decode<Model, TextureSize, TextureLevels, MaxVertices, MaxIndices, true>(
        data);
}
} // namespace VR::CallBeyond
