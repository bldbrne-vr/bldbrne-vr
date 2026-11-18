// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "core/vr/augur.h"
#include "video_core/renderer_vulkan/vr_overlay_meshes.h"

namespace Vulkan::VrOverlay {
// The crushed Augur's splatter (core/vr/augur.h Drops): one quad per particle, its corner in xy,
// its kind (DropKind) in z, its velocity as the normal, its radius and stretch as aux.
inline MeshData AugurSprayMesh() {
    MeshData mesh{};mesh.closed=false;
    const auto& drops=VR::Augur::Drops();
    mesh.vertices.reserve(drops.size()*4);
    mesh.indices.reserve(drops.size()*6);
    for(const auto& drop:drops) {
        const u32 base=static_cast<u32>(mesh.vertices.size());
        for(const auto c:std::array<Vec2,4>{{{-1,-1},{1,-1},{1,1},{-1,1}}})
            mesh.vertices.push_back({{c.x,c.y,static_cast<float>(drop.kind)},
                {drop.velocity[0],drop.velocity[1],drop.velocity[2]},
                {drop.radius,drop.stretch}});
        mesh.indices.insert(mesh.indices.end(),{base,base+1,base+2,base,base+2,base+3});
    }
    return mesh;
}
} // namespace Vulkan::VrOverlay
