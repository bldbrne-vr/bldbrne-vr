// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <algorithm>
#include "video_core/renderer_vulkan/vr_overlay_meshes.h"

namespace Vulkan::VrOverlay {
// Victorian apothecary bottle and folded linen wick. Fits within the original 106 x 296 mm envelope of s10010,
// with the neck still centered at the original y=0.11 m grip. Aux.x=2 identifies linen.
inline MeshData VictorianMolotovMesh() {
    std::array<ProfilePoint, 29> profile{{
        {0,-.095f},{.035f,-.095f},{.046f,-.096f},{.050f,-.094f},
        {.0525f,-.089f},{.053f,-.084f},{.053f,-.080f,true},
        {.051f,-.077f,true},{.051f,-.066f},{.052f,-.025f},
        {.052f,.043f},{.0505f,.052f},{.047f,.060f},{.040f,.067f},
        {.029f,.076f},{.018f,.086f},{.014f,.095f},{.0135f,.103f},
        {.0135f,.158f},{.014f,.169f,true},{.017f,.171f,true},
        {.018f,.174f},{.018f,.179f},{.0165f,.182f,true},
        {.010f,.182f,true},{.0095f,.177f},{.0095f,.171f},
        {.007f,.169f},{0,.169f}}};
    // Slim the body 8%, smoothly returning to the unchanged neck and grip radius.
    for (auto& point : profile) {
        const float t = std::clamp((point.y - .043f) / .060f, 0.0f, 1.0f);
        point.r *= .92f + .08f * t * t * (3.0f - 2.0f * t);
    }
    MeshData mesh = Lathe(profile,48);
    for (auto& v : mesh.vertices) v.aux = {0,0};
    // Crumpled plug of linen in the open mouth, and a fuller, draped tail. Part of the same mesh.
    const std::array<ProfilePoint,6> plug{{{0,.173f},{.007f,.173f},
        {.010f,.183f},{.009f,.190f},{.005f,.194f},{0,.194f}}};
    auto knot=Lathe(plug,12);
    for(auto& v:knot.vertices) {
        const float a=std::atan2(v.position.z,v.position.x);
        const float fold=1.0f+.13f*std::sin(a*5.0f+v.position.y*240.0f);
        v.position.x*=fold;v.position.z*=fold;v.aux={2,0};
    }
    AppendMesh(mesh,knot);
    constexpr u32 Rows=28, Cols=10;
    const u32 base=static_cast<u32>(mesh.vertices.size());
    auto point=[](float t,float w) {
        const float x=.004f+.044f*t;
        const float y=.186f+.005f*std::sin(t*Pi)-.027f*t*t;
        const float width=.0105f*(1.0f-.15f*t);
        const float fold=.0025f*std::sin(w*Pi*2.0f+t*6.0f)+.003f*(1.0f-w*w)*std::sin(t*Pi);
        const float fray=.0018f*std::sin(w*15.0f)*std::pow(t,12.0f);
        return Vec3{x+fray,y+w*width*std::sin(t*1.3f)+fold,
                    -.001f-.007f*t+w*width*std::cos(t*1.3f)};
    };
    for(u32 i=0;i<=Rows;++i) for(u32 j=0;j<=Cols;++j) {
        const float t=float(i)/Rows,w=-1.0f+2.0f*float(j)/Cols;
        const auto dt=point(t+.001f,w)-point(t-.001f,w);
        const auto dw=point(t,w+.001f)-point(t,w-.001f);
        mesh.vertices.push_back({point(t,w),Normalize(Cross(dt,dw)),{2+t,w}});
    }
    for(u32 i=0;i<Rows;++i) for(u32 j=0;j<Cols;++j) {
        const u32 a=base+i*(Cols+1)+j,b=a+Cols+1;
        mesh.indices.insert(mesh.indices.end(),{a,b,b+1,a,b+1,a+1});
    }
    mesh.closed=false; // the folded cloth is two-sided
    return mesh;
}
} // namespace Vulkan::VrOverlay
