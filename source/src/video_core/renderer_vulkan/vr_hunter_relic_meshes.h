// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include "video_core/renderer_vulkan/vr_goods_meshes.h"

namespace Vulkan::VrOverlay {
// Original, code-authored hunter relics. Generated once, never in the frame loop.
// Material channels: 6 iron, 7 brass, 9 leather, 19 embroidered cloth, 20 charged inlay.
namespace HunterRelic {
// Five strips (the trunk and four forks), built once. The shader bends them and faces their
// width to each eye. 382 vertices / 346 triangles including three flashes and ten analytic
// sparks. No mutable vertex buffer or particle simulation.
inline MeshData TonitrusBridge() {
    MeshData mesh{};mesh.closed=false;
    mesh.vertices.reserve(382);mesh.indices.reserve(1038);
    for(u32 branch=0;branch<5;++branch) {
        const u32 base=static_cast<u32>(mesh.vertices.size());
        for(u32 i=0;i<=32;++i)
            for(float side:{-1.0f,1.0f})
                mesh.vertices.push_back({{0,float(i)/32,float(branch)},{0,0,1},{side,float(branch)}});
        for(u32 i=0;i<32;++i) {
            const u32 a=base+2*i;
            mesh.indices.insert(mesh.indices.end(),{a,a+1,a+3,a,a+3,a+2});
        }
    }
    // 5: source flash, 6: contact flash, 7: the contact's light pool, 8..17: contact sparks.
    for(u32 code=5;code<18;++code) {
        const u32 a=static_cast<u32>(mesh.vertices.size());
        for(const auto corner:std::array<Vec2,4>{{{-1,-1},{1,-1},{1,1},{-1,1}}})
            mesh.vertices.push_back({{corner.x,corner.y,float(code)},{0,0,1},{corner.x,float(code)}});
        mesh.indices.insert(mesh.indices.end(),{a,a+1,a+2,a,a+2,a+3});
    }
    return mesh;
}
inline void Renormalize(MeshData& mesh) {
    for(auto& v:mesh.vertices)v.normal={};
    for(size_t i=0;i+2<mesh.indices.size();i+=3) {
        auto& a=mesh.vertices[mesh.indices[i]];
        auto& b=mesh.vertices[mesh.indices[i+1]];
        auto& c=mesh.vertices[mesh.indices[i+2]];
        const auto n=Cross(b.position-a.position,c.position-a.position);
        a.normal=a.normal+n;b.normal=b.normal+n;c.normal=c.normal+n;
    }
    for(auto& v:mesh.vertices)v.normal=Normalize(v.normal);
}
inline void Part(MeshData& dst, MeshData mesh, float material, const Mat4& pose = {}) {
    SetMaterial(mesh, material);
    TransformMesh(mesh, pose);
    AppendMesh(dst, mesh);
}
inline void Ring(MeshData& dst, float radius, float y, float wire, float material,
                 float flatten = 1.0f, u32 segments = 24) {
    std::array<ProfilePoint,5> profile{};
    for(size_t i=0;i<profile.size();++i) {
        const float a=2*Pi*float(i)/4;
        profile[i]={radius+wire*std::cos(a),y+wire*std::sin(a)};
    }
    Part(dst,Lathe(profile,segments),material,Mat4::Scale({1,1,flatten}));
}
inline MeshData Tonitrus() {
    MeshData out;
    // A substantial insulated grip and a forged terminal: proportions authored in metres.
    const std::array<ProfilePoint,12> shaft{{{0,-.145f},{.007f,-.132f},
        {.009f,-.105f},{.0105f,-.087f},{.011f,-.078f},{.011f,.030f},
        {.009f,.040f},{.007f,.051f},{.009f,.057f},{.013f,.061f},
        {.013f,.068f},{0,.069f}}};
    Part(out,Lathe(shaft,24),6);
    const std::array<ProfilePoint,6> grip{{{.0108f,-.079f},{.0122f,-.075f},
        {.0120f,-.065f},{.0114f,.016f},{.0118f,.026f},{.0108f,.030f}}};
    Part(out,Lathe(grip,24),9);
    // Low relief winding: a single continuous tube, not a stack of floating bands.
    std::array<Vec3,145> winding{};
    for(size_t i=0;i<winding.size();++i) {
        const float t=float(i)/float(winding.size()-1),a=t*2*Pi*9;
        const float r=.0123f-.0004f*t;
        winding[i]={r*std::cos(a),-.073f+t*.099f,r*std::sin(a)};
    }
    Part(out,Sweep(winding,.0007f,.0007f,5),7);
    for(float y:{-.082f,-.078f,.031f,.035f,.057f,.064f})Ring(out,.0122f,y,.0015f,7);
    auto orb=Sphere(.0365f,32,16);
    // Hammered metal is deliberately subtle: retain a clean round silhouette in VR.
    for(auto& v:orb.vertices) v.position=v.position*(1+.008f*GoodsNoise(v.position*700));
    Part(out,std::move(orb),6,Mat4::Translation({0,.098f,0}));
    Ring(out,.0367f,.098f,.0008f,7);
    // Recess-like meridian inlays with small anchor studs.
    for(int k=0;k<8;++k) {
        std::array<Vec3,13> arc{};
        const float az=float(k)*Pi/4;
        for(size_t i=0;i<arc.size();++i) {
            const float a=.22f+(Pi-.44f)*float(i)/float(arc.size()-1);
            arc[i]={.0368f*std::sin(a)*std::cos(az),.098f+.0368f*std::cos(a),
                    .0368f*std::sin(a)*std::sin(az)};
        }
        Part(out,Sweep(arc,.00065f,.00065f,4),20);
        Part(out,Sphere(.0015f,6,3),7,Mat4::Translation(arc[2]));
        Part(out,Sphere(.0015f,6,3),7,Mat4::Translation(arc[10]));
    }
    // Match the original item's long axis (-Z toward the terminal).
    TransformMesh(out,Mat4::RotationX(-Pi*.5f));
    return out;
}

inline MeshData Gloves() {
    MeshData out;
    // A collapsed gauntlet, cradled by the cuff. +Z runs toward its fingers,
    // +Y is the visible embroidered back, +X across the palm.
    std::array<LoftRing,22> palm{};
    const std::array<float,8> width{.027f,.026f,.027f,.031f,.035f,.035f,.032f,.027f};
    const auto palmFold=[](float x,float z) {
        // A few compressed leather creases, not inflated sinusoidal lobes.
        const float a=z-(.038f+x*.32f),b=z-(.068f-x*.23f),c=z-(.087f+x*.17f);
        const auto crease=[](float d) {
            return .0006f*std::exp(-std::pow((d-.003f)/.0024f,2.0f))-
                   .0010f*std::exp(-std::pow(d/.0018f,2.0f));
        };
        return crease(a)+crease(b)+.6f*crease(c);
    };
    // Match the embroidered face to the actual folded palm instead of floating
    // flat decorations above it. z is in the translated, cuff-relative frame.
    const auto backSurface=[&](float x,float z) {
        const float local_z=z-.040f;
        const float t=std::clamp((local_z-.012f)/.094f,0.0f,1.0f);
        const float section=t*7.0f;
        const size_t a=std::min(size_t(section),size_t(6));
        const float f=section-float(a);
        const float w=width[a]+(width[a+1]-width[a])*f*f*(3-2*f);
        const float across=std::abs(x-.0015f*std::sin(t*Pi))/w;
        const float y=.0015f*std::sin(t*Pi)+(.006f+.0015f*std::sin(t*Pi))*
            std::pow(std::max(0.0f,1-std::pow(across,3.0f)),1.0f/3.0f);
        return y+palmFold(x,local_z)*std::clamp(y/.006f,0.0f,1.0f);
    };
    for(size_t i=0;i<palm.size();++i) {
        const float t=float(i)/float(palm.size()-1);
        const float section=t*7.0f;
        const size_t a=std::min(size_t(section),size_t(6));
        const float blend=section-float(a);
        const float w=width[a]+(width[a+1]-width[a])*blend*blend*(3-2*blend);
        palm[i]={{.0015f*std::sin(t*Pi),.0015f*std::sin(t*Pi),.012f+t*.094f},
                 {1,0,0},{0,1,0},w,.006f+.0015f*std::sin(t*Pi),3.0f};
    }
    auto hide=Loft(palm,24,{0,0,.010f},{0,0,.109f});
    for(auto& v:hide.vertices) {
        const float face=std::clamp(std::abs(v.position.y)/.006f,0.0f,1.0f);
        // Broad compression folds and diagonal tension toward the thumb seam.
        // Built once, so these keep their silhouette without per-frame tessellation.
        const float z=v.position.z,x=v.position.x;
        const float fold=palmFold(x,z);
        v.position.y+=(v.position.y>=0.0f?1.0f:-1.0f)*fold*face;
    }
    Renormalize(hide);
    Part(out,std::move(hide),9);
    // Four slightly curled empty fingers; their joint creases remain visible when inert.
    for(int finger=0;finger<4;++finger) {
        const float x=(float(finger)-1.5f)*.017f;
        const float length= finger==3?.049f:finger==0?.061f:finger==1?.073f:.068f;
        std::array<LoftRing,16> rings{};
        for(size_t j=0;j<rings.size();++j) {
            const float t=float(j)/float(rings.size()-1),y=-(.030f+.004f*float(finger))*t*t;
            const float joint=1-.18f*std::exp(-std::pow((t-.45f)*15,2))-
                                    .12f*std::exp(-std::pow((t-.76f)*18,2));
            const float collapsed=.74f-.18f*std::sin(t*Pi);
            const float tip=std::sqrt(1-.90f*std::pow(t,10));
            rings[j]={{x+(x*.12f)*t,y,.096f+length*(t-.15f*t*t)},
                      {1,0,0},{0,1,0},(.0083f-.0023f*t)*joint*tip,
                      (.009f-.003f*t)*joint*collapsed*tip,3.0f};
        }
        Part(out,Loft(rings,14,{x,0,.094f},
             {x*1.12f,-(.030f+.004f*float(finger)),.097f+length*.85f}),9);
        std::array<Vec3,9> seam{};
        for(size_t j=0;j<seam.size();++j) {
            const float t=float(j)/float(seam.size()-1);
            const float joint=1-.18f*std::exp(-std::pow((t-.45f)*15,2))-
                                    .12f*std::exp(-std::pow((t-.76f)*18,2));
            const float top=(.009f-.003f*t)*joint*(.74f-.18f*std::sin(t*Pi))*
                             std::sqrt(1-.90f*std::pow(t,10));
            seam[j]={x+x*.12f*t,-(.030f+.004f*float(finger))*t*t+top+.0002f,
                     .096f+length*(t-.15f*t*t)};
        }
        Part(out,Sweep(seam,.00065f,.00045f,5),19);
    }
    const std::array<Vec3,6> thumb{{{-.028f,0,.040f},{-.041f,-.002f,.050f},
        {-.052f,-.007f,.063f},{-.057f,-.014f,.076f},{-.052f,-.024f,.086f},
        {-.044f,-.030f,.086f}}};
    auto thumb_hide=Sweep(thumb,.011f,.005f,12);
    TransformMesh(thumb_hide,Mat4::Scale({1,.62f,1}));
    Part(out,std::move(thumb_hide),9);
    // A raised, sewn thumb gusset gives the join a continuous leather silhouette.
    const std::array<Vec3,7> gusset{{{-.020f,.010f,.022f},{-.029f,.011f,.035f},
        {-.036f,.009f,.047f},{-.042f,.006f,.056f},{-.046f,.002f,.066f},
        {-.046f,-.004f,.074f},{-.043f,-.012f,.080f}}};
    auto thumb_seam=Sweep(gusset,.00075f,.00055f,6);
    TransformMesh(thumb_seam,Mat4::Scale({1,.62f,1}));
    Part(out,std::move(thumb_seam),19);
    // The glove hangs below the clenched fingers; only its gathered cuff occupies the fist.
    TransformMesh(out,Mat4::Translation({0,0,.040f}));
    // A flared hollow cuff with a ragged rim. Inner surface is explicit, avoiding backface tricks.
    const std::array<ProfilePoint,10> cuff{{{.023f,-.055f},{.020f,-.034f},
        {.019f,-.010f},{.021f,.029f},{.030f,.061f},{.027f,.061f},
        {.018f,.029f},{.016f,-.010f},{.017f,-.034f},{.020f,-.055f}}};
    auto cloth=Lathe(cuff,40,true);
    TransformMesh(cloth,Mat4::Translation({0,-.003f,0})*
        Mat4::Scale({1,.48f,1})*Mat4::RotationX(Pi*.5f));
    for(auto& v:cloth.vertices) {
        const float edge=std::clamp((-.020f-v.position.z)/.035f,0.0f,1.0f);
        const float angle=std::atan2(v.position.y+.003f,v.position.x);
        v.position.z+=edge*(.003f*std::sin(angle*7)+.002f*std::sin(angle*13));
        v.position.y+=.0024f*std::sin(v.position.z*180+angle*4)*edge;
    }
    Renormalize(cloth);
    Part(out,std::move(cloth),19);
    // Sparse hand stitching on the cuff edge, merged into the same opaque mesh.
    for(int i=0;i<9;++i) {
        const float x=-.020f+float(i)*.005f;
        const float y=.0112f*std::sqrt(std::max(0.0f,1-x*x/(.024f*.024f)))-.003f;
        const std::array<Vec3,3> stitch{{{x,y,-.044f},{x+.0012f,y+.0008f,-.041f},{x+.002f,y,-.038f}}};
        Part(out,Sweep(stitch,.00055f,.00055f,4),7);
    }
    // Double piping and broken cross stitches make the cuff read as sewn cloth,
    // rather than a smooth tube. They are batched into this one static mesh.
    for(float side:{-1.0f,1.0f}) {
        const std::array<Vec3,5> piping{{{side*.020f,.006f,-.042f},
            {side*.017f,.007f,-.015f},{side*.019f,.009f,.015f},
            {side*.025f,.012f,.042f},{side*.030f,.010f,.064f}}};
        Part(out,Sweep(piping,.00085f,.0006f,5),7);
        for(int i=0;i<8;++i) {
            const float z=.066f+float(i)*.008f;
            const float x=side*(.024f+.003f*std::sin(float(i)*.7f));
            const std::array<Vec3,3> stitch{{{x-.0014f,backSurface(x-.0014f,z-.0015f),z-.0015f},
                {x,backSurface(x,z)+.0006f,z},
                {x+.0014f,backSurface(x+.0014f,z+.0015f),z+.0015f}}};
            Part(out,Sweep(stitch,.00045f,.00035f,4),19);
        }
    }
    // Gold scroll embroidery: geometry at close range, no large texture or alpha overdraw.
    for(float side:{-1.0f,1.0f}) {
        std::array<Vec3,33> scroll{};
        for(size_t i=0;i<scroll.size();++i) {
            const float t=float(i)/32,angle=t*Pi*3;
            scroll[i]={side*(.017f+.010f*(1-t)*std::cos(angle)),
                       .016f,.085f+.026f*(1-t)*std::sin(angle)};
            scroll[i].y=backSurface(scroll[i].x,scroll[i].z)+.00045f;
        }
        Part(out,Sweep(scroll,.0008f,.00045f,5),7);
    }
    Part(out,Sphere(1,12,6),20,Mat4::Translation({0,backSurface(0,.087f),.087f})*
         Mat4::Scale({.005f,.0012f,.009f}));
    // Native glove frame: its back is toward -Y, with the cuff near -Z.
    TransformMesh(out,Mat4::Scale({1,-1,-1}));
    // A gathered cuff, not a long rigid handle. Shorten the empty sleeve and
    // keep the palm/finger proportions close to a wearable leather glove.
    for(auto& v:out.vertices) {
        const float shorten=v.position.z>0.0f?.55f:.88f;
        v.position.z*=shorten;
        const float t=std::clamp((v.position.z+.055f)/.040f,0.0f,1.0f);
        const float gather=t*t*(3.0f-2.0f*t);
        v.position.x*=1.0f-.36f*gather;
        v.position.y*=1.0f-.32f*gather;
    }
    Renormalize(out);
    return out;
}
// The Blacksky Eye (core/vr/blacksky_eye.h): a human eyeball, 24 mm, its centre at the origin
// and its front (the cornea) toward -Z. Built along +Y (the front up) and turned. Materials: 21
// the sclera, 22 the iris (its pupil drawn by the shader: it dilates), 24 the stub of optic
// nerve behind; the cornea is its own mesh (BlackskyCornea, 23), drawn after it over the
// recessed iris. Anatomy at this size: the limbus 30 degrees off the front (an iris 12 mm
// across), the iris ~2.4 mm behind the cornea's apex.
inline constexpr float EyeR = .012f;
inline constexpr float EyeLimbusY = EyeR * .8660254f, EyeLimbusR = EyeR * .5f;
inline constexpr float CorneaR = .0078f;
inline MeshData BlackskyEye() {
    MeshData out;
    const Mat4 front = Mat4::RotationX(-Pi * .5f); // +Y (built) -> -Z (front)
    // The optic nerve: a short severed stub, its cut face flat, flaring into the globe.
    {
        const std::array<ProfilePoint, 6> nerve{{{0.0f, -.0162f}, {.0016f, -.0162f, true},
                                                 {.00175f, -.0150f}, {.0019f, -.0132f},
                                                 {.0024f, -.0122f}, {.0034f, -.01145f}}};
        Part(out, Lathe(nerve, 24), 24, front);
    }
    // The globe, from just inside the nerve's flare round to the limbus.
    {
        std::vector<ProfilePoint> globe;
        const float a0 = std::asin(.0030f / EyeR), a1 = Pi - Pi / 6.0f;
        for (int i = 0; i <= 30; ++i) {
            const float a = a0 + (a1 - a0) * float(i) / 30.0f; // from the back (-Y)
            globe.push_back({EyeR * std::sin(a), -EyeR * std::cos(a)});
        }
        globe.back().crease = true; // the iris meets it here: no step to alias (its shade blends)
        Part(out, Lathe(globe, 64), 21, front);
    }
    // The iris: from the limbus a shallow dome rising toward the pupil (the lens behind it), to
    // its centre.
    {
        std::vector<ProfilePoint> iris;
        for (int i = 0; i <= 12; ++i) {
            const float t = float(i) / 12.0f;
            const float r = EyeLimbusR * (1.0f - t);
            iris.push_back({r, EyeLimbusY + .00012f * (1.0f - (1.0f - t) * (1.0f - t)), i == 0});
        }
        Part(out, Lathe(iris, 64), 22, front);
    }
    out.closed = false; // the globe's back is open where the nerve's flare covers it
    return out;
}
// Its cornea: the clear dome over the iris, from just inside the limbus (its seam hidden in
// the globe) to the apex 1.2 mm proud of the globe.
inline MeshData BlackskyCornea() {
    MeshData out;
    const float cy = EyeLimbusY - std::sqrt(CorneaR * CorneaR - EyeLimbusR * EyeLimbusR);
    std::vector<ProfilePoint> dome;
    const float b0 = std::asin(std::min(1.0f, (EyeLimbusR + .00006f) / CorneaR));
    for (int i = 0; i <= 14; ++i) {
        const float b = b0 * (1.0f - float(i) / 14.0f);
        dome.push_back({CorneaR * std::sin(b), cy + CorneaR * std::cos(b)});
    }
    Part(out, Lathe(dome, 64), 23, Mat4::RotationX(-Pi * .5f));
    out.closed = false;
    return out;
}
} // namespace HunterRelic
} // namespace Vulkan::VrOverlay
