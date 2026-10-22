// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "core/vr/weapon_shape_moments.h"
#include "core/vr/two_hand_grip.h"
namespace VR::Body::Weight::MotorDetail {
struct GripInertia {
    std::array<VR::Body::Vec3,3> axes{{{1,0,0},{0,1,0},{0,0,1}}};
    std::array<float,3> values{{1,1,1}};
    VR::Body::Vec3 center{};
};
// Setup only: rotate and translate second moments into the actual grip frame,
// then diagonalize the full symmetric inertia tensor. Steady steps use 3 axes.
inline GripInertia Calibrate(int family,bool form,float mass,
                            const VR::TwoHand::BoneOnGrip& grip,float scale=1) {
    namespace B=VR::Body;
    const ShapeMoments* shape=nullptr;
    for(const auto& s:Moments)if(s.family==family&&s.form==form)shape=&s;
    GripInertia out;if(!shape)return out;
    auto map=[&](B::Vec3 p){return VR::TwoHand::BoneToGripLocal(grip,p,scale);};
    auto offset=map({});auto center=map(shape->center);out.center=center;
    std::array<B::Vec3,3> cols{{map({1,0,0})-offset,map({0,1,0})-offset,map({0,0,1})-offset}};
    auto component=[](B::Vec3 v,int i){return i==0?v.x:i==1?v.y:v.z;};
    double second[3][3]{},a[3][3]{},v[3][3]{};
    for(int i=0;i<3;i++)for(int j=0;j<3;j++){
        for(int k=0;k<3;k++)for(int l=0;l<3;l++)
            second[i][j]+=component(cols[k],i)*shape->second[k*3+l]*component(cols[l],j);
        // E[(Rx+b)(Rx+b)^T], including COM/offset cross terms.
        second[i][j]+=component(center-offset,i)*component(offset,j)+
                      component(offset,i)*component(center-offset,j)+component(offset,i)*component(offset,j);
    }
    const double trace=second[0][0]+second[1][1]+second[2][2];
    for(int i=0;i<3;i++)for(int j=0;j<3;j++){a[i][j]=mass*((i==j?trace:0)-second[i][j]);v[i][j]=i==j;}
    // Cyclic Jacobi; bounded work off the steady frame path.
    for(int sweep=0;sweep<12;sweep++)for(int p=0;p<2;p++)for(int q=p+1;q<3;q++){
        if(std::abs(a[p][q])<1e-12)continue;
        const double angle=.5*std::atan2(2*a[p][q],a[q][q]-a[p][p]);
        const double c=std::cos(angle),s=std::sin(angle);
        const double pp=a[p][p],qq=a[q][q],pq=a[p][q];
        a[p][p]=c*c*pp-2*s*c*pq+s*s*qq;
        a[q][q]=s*s*pp+2*s*c*pq+c*c*qq;a[p][q]=a[q][p]=0;
        for(int k=0;k<3;k++)if(k!=p&&k!=q){
            const double kp=a[k][p],kq=a[k][q];
            a[k][p]=a[p][k]=c*kp-s*kq;a[k][q]=a[q][k]=s*kp+c*kq;
        }
        for(int k=0;k<3;k++){
            const double kp=v[k][p],kq=v[k][q];v[k][p]=c*kp-s*kq;v[k][q]=s*kp+c*kq;
        }
    }
    for(int i=0;i<3;i++){
        out.values[i]=float(a[i][i]);
        out.axes[i]=B::Normalize(B::Vec3{float(v[0][i]),float(v[1][i]),float(v[2][i])});
    }
    return out;
}
}
