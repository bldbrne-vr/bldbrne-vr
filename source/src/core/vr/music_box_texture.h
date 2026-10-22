// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace VR::MusicBox {
// Four atlas tiles: lid, side panel, crank socket panel, brass. Coordinates belong to
// s11012; no new surfaces, draw calls, normals or collision dimensions are added.
struct RegalFace { int tile, axis; };
inline RegalFace RegalSurface(const float* a, const float* b, const float* c) {
    const std::array<float,3> ab{b[0]-a[0],b[1]-a[1],b[2]-a[2]},
                              ac{c[0]-a[0],c[1]-a[1],c[2]-a[2]};
    const std::array<float,3> n{std::abs(ab[1]*ac[2]-ab[2]*ac[1]),
                              std::abs(ab[2]*ac[0]-ab[0]*ac[2]),
                              std::abs(ab[0]*ac[1]-ab[1]*ac[0])};
    const int axis = static_cast<int>(std::max_element(n.begin(),n.end())-n.begin());
    const float x=(a[0]+b[0]+c[0])/3, y=(a[1]+b[1]+c[1])/3, z=(a[2]+b[2]+c[2])/3;
    if (axis==0 && x>.051f) return {0,axis}; // lid's broad face
    if (x>.050f || ((y<-.020f || y>.060f) && (z<.017f || z>.067f)))
        return {3,axis}; // rolled lid rim and native corner furniture
    if (axis==0) return {4,axis}; // underside uses an unadorned wood patch
    if (axis==1 && y>0) return {2,axis}; // socket only on the right wall
    return {1,axis};
}
inline std::array<float,2> RegalUV(RegalFace face, const float* p) {
    float u{},v{};
    if (face.axis==0) { u=(.078761f-p[1])/.116022f; v=(p[2]+.001792f)/.086719f; }
    else if (face.axis==1) { u=(p[2]-.001532f)/.081469f; v=(.055423f-p[0])/.0682f; }
    else { u=(p[1]+.032797f)/.107278f; v=(.055423f-p[0])/.0682f; }
    // The socket tile is authored separately. Align its dark centre to the actual
    // crank axis. This affine mapping stays inside the tile and interpolates
    // correctly across the original two-triangle panels.
    if (face.tile==2) {
        return {.249f+.400f*(u-.49674f), .665f+.374f*(v-.42482f)};
    }
    if (face.tile==4) return {.19f+.12f*std::clamp(u,0.f,1.f),
                              .065f+.025f*std::clamp(v,0.f,1.f)};
    // Inset inside each tile so filtering cannot sample an adjacent material.
    return {(face.tile%2)*.5f+.006f+.488f*std::clamp(u,0.f,1.f),
            (face.tile/2)*.5f+.006f+.488f*std::clamp(v,0.f,1.f)};
}
template<class Vertex, class Index>
bool ApplyRegalUVs(std::vector<Vertex>& vertices, std::vector<Index>& indices) {
    if (indices.empty() || indices.size()%3) return false;
    for(auto i:indices) if(i>=vertices.size()) return false;
    std::vector<Vertex> mapped;
    mapped.reserve(indices.size());
    for(std::size_t i=0;i<indices.size();i+=3) {
        const auto face=RegalSurface(vertices[indices[i]].position,vertices[indices[i+1]].position,
                                     vertices[indices[i+2]].position);
        for(int j=0;j<3;j++) {
            auto vertex=vertices[indices[i+j]];
            const auto uv=RegalUV(face,vertex.position);
            vertex.uv[0]=uv[0]; vertex.uv[1]=uv[1];
            mapped.push_back(vertex);
        }
    }
    // Split existing vertices at atlas seams; retain every triangle and normal.
    for(std::size_t i=0;i<indices.size();i++) indices[i]=static_cast<Index>(i);
    vertices.swap(mapped);
    return true;
}
inline constexpr std::uint32_t RegalSize=1024, RegalLevels=11;
inline constexpr std::size_t RegalBytes=699064;
inline bool RegalDdsValid(std::span<const char> bytes) {
    if(bytes.size()!=128+RegalBytes) return false;
    const auto word=[&](std::size_t at) {
        std::uint32_t v=0;
        for(int i=0;i<4;i++) v|=std::uint32_t(static_cast<unsigned char>(bytes[at+i]))<<(8*i);
        return v;
    };
    return word(0)==0x20534444 && word(4)==124 && word(12)==RegalSize &&
           word(16)==RegalSize && word(28)==RegalLevels && word(76)==32 && word(84)==0x31545844;
}
} // namespace VR::MusicBox
