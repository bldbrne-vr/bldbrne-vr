// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include <array>
#include "core/vr/body_frame.h"
// Generated from local FLVER LOD0 triangles posed by each part's a999 default clip.
// Coordinates: R_Weapon frame, game metres. Uniform surface-density centroid and
// second moments are geometry proxies, NOT measured material densities or mass.
// Generated offline by a tool not included here.
namespace VR::Body::Weight::Measured {
struct Geometry {
    int family; bool form_b;
    float length, width, thickness, area;
    Vec3 center;
    float transverse_per_mass, axial_per_mass;
};
inline constexpr std::array<Geometry,11> Shapes{{
    {50,false,1.27868488f,0.48609078f,0.11502406f,0.15440200f,{-0.04489641f,-0.43568044f,0.00104812f},0.29544206f,0.01493727f},
    {50,true,2.31524879f,0.52221187f,0.11497031f,0.19722400f,{0.01257781f,-0.84235364f,0.00124370f},1.11983913f,0.01138985f},
    {70,false,0.81931278f,0.41871486f,0.08677829f,0.10659800f,{-0.08592753f,-0.17625422f,-0.00276461f},0.08123756f,0.02096684f},
    {70,true,1.45604593f,0.37281596f,0.08681547f,0.12035200f,{0.02191780f,-0.74550934f,0.01157062f},0.69556581f,0.00619011f},
    {71,false,1.13152704f,0.30347001f,0.11360669f,0.12037000f,{-0.13269391f,-0.12820720f,0.00019675f},0.09946616f,0.02494896f},
    {71,true,1.94280447f,0.24292179f,0.11364387f,0.13835200f,{-0.07972144f,-0.60363370f,0.00190282f},0.56959455f,0.00977942f},
    {80,false,1.28131532f,0.24586629f,0.03949462f,0.03875800f,{-0.00033688f,-0.28678543f,0.00116730f},0.17987228f,0.00083867f},
    {80,true,1.47250462f,0.79501150f,0.30113155f,0.26965800f,{0.30482530f,-0.74740469f,0.00052159f},0.70095993f,0.12845088f},
    {81,false,1.42175697f,0.24586629f,0.03949462f,0.04187000f,{-0.00041907f,-0.33357185f,0.00132436f},0.23722347f,0.00079642f},
    {81,true,1.66090840f,0.64528662f,0.07365652f,0.15387200f,{0.22289118f,-0.53272800f,-0.00132164f},0.44057236f,0.06952983f},
    {240,false,1.11529239f,0.32887560f,0.15479861f,0.11575200f,{-0.08767019f,-0.52728482f,0.00151828f},0.34793312f,0.01211731f},
}};
} // namespace VR::Body::Weight::Measured
