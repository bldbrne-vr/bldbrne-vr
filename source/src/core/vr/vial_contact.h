// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "core/vr/vial_zones.h"

// Syringe hub contact, with room for the real skin, controllers and headset.
// The needle is visual only. Fixed-size targets are checked at the headset rate.
namespace VR::VialContact {
using Body::Vec3;
namespace C = Body::C0000;
inline constexpr std::array<int, 27> Bones{C::Pelvis,C::Spine,C::Spine1,C::Spine2,C::Neck,
    C::Head,C::R_UpperArm,C::R_Forearm,C::R_Hand,C::R_Finger1,C::L_UpperArm,C::L_Forearm,
    C::L_Thigh,C::L_Calf,C::L_Foot,C::L_Toe0,C::R_Thigh,C::R_Calf,C::R_Foot,C::L_Hand,C::R_Toe0,
    C::R_Finger11,C::R_Finger21,C::R_Finger31,C::R_Finger41,C::R_Finger01,C::L_Finger02};
struct TorsoAxes { Vec3 x{1,0,0}, y{0,1,0}, z{0,0,1}; };
struct Anatomy {
    bool valid{};
    std::int64_t ns{};
    std::uint32_t space{};
    float scale{1};
    bool grip_valid{};
    Vec3 grip_p{};
    Body::Quat grip_q{};
    bool right_grip_valid{};
    Vec3 right_grip_p{};
    Body::Quat right_grip_q{};
    bool seated{};
    bool torso_axes_valid{};
    TorsoAxes torso_axes{};
    // Four inferred seated hip/knee points follow the skeleton points.
    std::array<Vec3, Bones.size()+4> p{};
    // Zero means the game anatomy timestamp; live anchors use their XR timestamp.
    std::array<std::int64_t, Bones.size()+4> point_ns{};
};
struct Sample {
    bool valid{};
    std::int64_t ns{};
    std::uint32_t space{};
    Vec3 tip{};
    bool grip_local{};
    Vec3 grip_offset{};
    Anatomy body{};
};
struct Capsule { int a,b; float radius; VialZones::Zone zone; const char* label; float width{}, depth{}; float margin{.02f}; };
inline constexpr std::array<Capsule, 23> Capsules{{
    {0,1,.10f,VialZones::Zone::Body,"hips",.16f,.10f},
    {1,2,.10f,VialZones::Zone::Body,"belly",.15f,.10f},
    {2,3,.11f,VialZones::Zone::Body,"chest",.17f,.11f},
    {3,4,.095f,VialZones::Zone::Body,"collar"},
    {5,5,.075f,VialZones::Zone::Head,"head / headset",0,0,.045f},
    {6,7,.055f,VialZones::Zone::UpperArm,"upper arm",0,0,.03f},
    {7,8,.045f,VialZones::Zone::Forearm,"forearm",0,0,.03f},
    {8,9,.035f,VialZones::Zone::Wrist,"hand / wrist",0,0,.05f},
    // Cover the palm fan and curled knuckles using the actual posed finger joints.
    {8,21,.023f,VialZones::Zone::Wrist,"index knuckle",0,0,.05f},
    {8,22,.023f,VialZones::Zone::Wrist,"middle knuckle",0,0,.05f},
    {8,23,.023f,VialZones::Zone::Wrist,"ring knuckle",0,0,.05f},
    {8,24,.023f,VialZones::Zone::Wrist,"outer palm",0,0,.05f},
    {8,25,.023f,VialZones::Zone::Wrist,"thumb side",0,0,.05f},
    {12,13,.075f,VialZones::Zone::LeftLeg,"left thigh"},
    {13,14,.055f,VialZones::Zone::LeftLeg,"left shin"},
    {14,15,.045f,VialZones::Zone::LeftLeg,"left foot"},
    {16,17,.075f,VialZones::Zone::RightLeg,"right thigh"},
    {17,18,.055f,VialZones::Zone::RightLeg,"right shin"},
    {18,20,.045f,VialZones::Zone::RightLeg,"right foot"},
    {12,16,.10f,VialZones::Zone::Body,"hips"},
    {3,6,.065f,VialZones::Zone::UpperArm,"right shoulder / clavicle",0,0,.03f},
    {27,28,.08f,VialZones::Zone::Lap,"seated left thigh / knee"},
    {29,30,.08f,VialZones::Zone::Lap,"seated right thigh / knee"},
}};
inline Vec3 Nearest(Vec3 p, Vec3 a, Vec3 b) {
    const Vec3 d=b-a;
    return a+d*std::clamp(Body::Dot(p-a,d)/std::max(Body::Dot(d,d),1e-9f),0.f,1.f);
}
// Closest point on the moving tip segment to a bone segment (including degenerate bones).
inline Vec3 SweepPoint(Vec3 p, Vec3 q, Vec3 a, Vec3 b) {
    const Vec3 d=q-p, e=b-a, r=p-a;
    const float dd=Body::Dot(d,d), ee=Body::Dot(e,e), de=Body::Dot(d,e);
    const float dr=Body::Dot(d,r), er=Body::Dot(e,r);
    float s=0, t=0;
    if (dd>1e-9f) {
        const float denom=dd*ee-de*de;
        s=denom>1e-9f?std::clamp((de*er-dr*ee)/denom,0.f,1.f):0.f;
        t=ee>1e-9f?(de*s+er)/ee:0.f;
        if(t<0 || ee<=1e-9f) s=std::clamp(-dr/dd,0.f,1.f);
        else if(t>1) s=std::clamp((de-dr)/dd,0.f,1.f);
    }
    return p+d*s;
}
inline constexpr std::int64_t GraceNs = 250'000'000;
inline bool Fresh(const Sample& s, std::int64_t now, std::uint32_t space) {
    if (!std::isfinite(s.body.scale) || s.body.scale<=0) return false;
    for (const auto p : s.body.p) if (!Body::IsFinite(p)) return false;
    return s.valid && s.body.valid && s.space==space && s.body.space==space &&
        s.ns>0 && now>=s.ns && now-s.ns<=GraceNs &&
        s.body.ns>0 && now>=s.body.ns && now-s.body.ns<=GraceNs && Body::IsFinite(s.tip);
}
// Rendering publishes one stable attachment offset. Each XR frame applies it to
// the current controller, so a fast press cannot fall between two game frames.
inline Sample AtGrip(const Sample& source, const Anatomy& body, Vec3 p, Body::Quat q,
                     std::int64_t now, std::uint32_t space) {
    Sample out=source;
    out.valid=source.valid && source.grip_local && source.space==space &&
        now>=source.ns && now-source.ns<=GraceNs && Body::IsFinite(p) && Body::IsFinite(q);
    out.ns=now; out.space=space; out.body=body;
    out.tip=p+Body::Rotate(Body::Normalize(q),source.grip_offset);
    return out;
}
inline TorsoAxes GetTorsoAxes(const Anatomy& body) {
    if(body.torso_axes_valid) return body.torso_axes;
    const Vec3 x=Body::Normalize(body.p[6]-body.p[10],{1,0,0});
    const Vec3 y=Body::Normalize(Body::Perp(body.p[3]-body.p[1],x),{0,1,0});
    return {x,y,Body::Normalize(Body::Cross(x,y),{0,0,1})};
}
// Use the adjusted grip published to the body, not the raw controller. Keep the
// shoulder on the game-pose torso; wrist twist must never rotate chest geometry.
inline void AtRightGrip(Anatomy& body, Vec3 p, Body::Quat q, std::int64_t now) {
    if(!body.right_grip_valid || !Body::IsFinite(p) || !Body::IsFinite(q)) return;
    body.torso_axes=GetTorsoAxes(body); // capture before re-anchoring any arm points
    body.torso_axes_valid=true;
    const Vec3 old_wrist=body.p[8];
    const auto rotation=Body::Normalize(q)*Body::Conjugate(Body::Normalize(body.right_grip_q));
    for(int i:{8,9,21,22,23,24,25}) {
        body.p[i]=p+Body::Rotate(rotation,body.p[i]-body.right_grip_p);
        body.point_ns[i]=now;
    }
    // Approximate the intervening elbow until the next game IK sample. Only the
    // wrist displacement contributes, not a rigid rotation about the controller.
    body.p[7]=body.p[7]+(body.p[8]-old_wrist)*.5f;
    body.point_ns[7]=now;
}
inline void SeatedLap(Anatomy& body, const VialZones::Geometry& g, std::int64_t now) {
    body.seated=g.body_ok && g.params.seated;
    if(!body.seated) return;
    for(int side=0;side<2;++side) {
        body.p[27+side*2]=g.hips[side];
        body.p[28+side*2]=g.knees[side];
        body.point_ns[27+side*2]=body.point_ns[28+side*2]=now;
    }
}
// Different anatomy points arrive on different clocks. Repeated game samples
// retain their measured velocity; a new sample uses its own interval, never XR dt.
class AnatomyVelocity {
public:
    void Update(const Anatomy& body, bool reset) {
        for(std::size_t i=0;i<p_.size();++i) {
            const auto ns=body.point_ns[i]>0?body.point_ns[i]:body.ns;
            if(reset || ns<ns_[i] || ns-ns_[i]>GraceNs) v_[i]={};
            else if(ns>ns_[i]) v_[i]=(body.p[i]-p_[i])*(1e9f/float(ns-ns_[i]));
            if(reset || ns!=ns_[i]) { p_[i]=body.p[i]; ns_[i]=ns; }
        }
    }
    Vec3 operator[](std::size_t i) const { return v_[i]; }
private:
    std::array<Vec3,Bones.size()+4> p_{},v_{};
    std::array<std::int64_t,Bones.size()+4> ns_{};
};
class Gate {
public:
    void Reset() { const auto until=cooldown_until_; *this={}; cooldown_until_=until; }
    void StartMoved(Vec3) { Reset(); }
    // The stab Update just reported is not taken (the presenter's grace right after a draw):
    // stab again, but only after leaving the zones (needs_exit_ stays) and without the cooldown.
    void Unfire() { fired_=false; cooldown_until_=0; }
    VialZones::Result Update(const Sample& s, std::int64_t now, std::uint32_t space,
                            bool tracked, bool weapon_swinging, float margin_scale=1.f) {
        VialZones::Result out{};
        if(!tracked || !Fresh(s,now,space)) {
            if(last_.valid && (space!=last_.space || now-last_.ns>GraceNs)) Reset();
            interrupted_=true;
            return out;
        }
        if(s.ns==last_.ns) return out;
        const float dt=float(s.ns-last_.ns)*1e-9f;
        const bool initialize=!last_.valid || dt<=0 || dt>.25f || last_.space!=s.space ||
                              Body::Length(s.tip-last_.tip)>.5f;
        const bool resume=interrupted_ && !initialize;
        interrupted_=false;
        if(initialize) { needs_exit_={}; outside_={}; approach_ns_={}; }
        if(resume) { outside_={}; approach_ns_={}; }
        const Vec3 motion=initialize||resume ? Vec3{} : s.tip-last_.tip;
        velocity_.Update(s.body,initialize||resume);
        const float inv_dt=dt>0 ? 1.f/dt : 0.f;
        const Vec3 own=motion*inv_dt-velocity_[4];
        const float speed=Body::Length(own);
        const auto [x,y,z]=GetTorsoAxes(s.body);
        std::array<bool,VialZones::ZoneCount> outside_zone{};
        outside_zone.fill(true);
        margin_scale=std::clamp(margin_scale,.5f,2.f);
        for(std::size_t i=0;i<Capsules.size();++i) {
            const auto& c=Capsules[i];
            if(c.zone==VialZones::Zone::Lap && !s.body.seated) continue;
            const float margin=c.margin*margin_scale;
            // Skin padding and the headset/lap envelope are measured in room metres.
            const float anatomical_scale=c.zone==VialZones::Zone::Head ||
                c.zone==VialZones::Zone::Lap ? 1.f : s.body.scale;
            const float radius=c.radius*anatomical_scale+margin;
            const float sx=c.width>0?radius/(c.width*anatomical_scale+margin):1.f;
            const float sz=c.depth>0?radius/(c.depth*anatomical_scale+margin):1.f;
            const auto shape=[&](Vec3 p) {
                return c.width>0?Vec3{Body::Dot(p,x)*sx,Body::Dot(p,y),Body::Dot(p,z)*sz}:p;
            };
            const Vec3 a=shape(s.body.p[c.a]), b=shape(s.body.p[c.b]);
            const Vec3 previous=shape(initialize||resume ? s.tip : last_.tip), current=shape(s.tip);
            const float before=Body::Length(previous-Nearest(previous,a,b))-radius;
            const float after=Body::Length(current-Nearest(current,a,b))-radius;
            const auto zone=static_cast<std::size_t>(c.zone);
            auto& measured=out.m[zone];
            if(!measured.valid || after<measured.d) { measured.valid=true; measured.d=after; measured.label=c.label; }
            outside_zone[zone]=outside_zone[zone] && after>.002f;
            if(initialize && after<=0) needs_exit_[zone]=true;
            if(before>.002f) {
                if(outside_[i]==0 || s.ns-outside_[i]>600'000'000)
                    origin_[i]=initialize||resume ? s.tip : last_.tip;
                outside_[i]=s.ns;
            }
            if(initialize || resume || fired_ || needs_exit_[zone] || s.ns<cooldown_until_) continue;
            const Vec3 point=SweepPoint(previous,current,a,b);
            const bool touch=Body::Length(point-Nearest(point,a,b))<=radius+1e-6f;
            const Vec3 normal=Nearest(previous,a,b)-previous;
            const Vec3 inward=Body::Normalize(c.width>0?
                x*(normal.x*sx)+y*normal.y+z*(normal.z*sz):normal);
            const float approach=Body::Dot(own,inward);
            const float travel=Body::Length(s.tip-origin_[i]);
            const float room_approach=Body::Dot(motion*inv_dt,inward);
            const Vec3 target_velocity=(velocity_[c.a]+velocity_[c.b])*.5f;
            const float target_closing=-Body::Dot(target_velocity,inward);
            const bool target_only=target_closing>.04f &&
                (room_approach<.06f || (weapon_swinging && VialZones::IsArm(c.zone) &&
                                       target_closing>room_approach));
            if(approach>=.06f && approach>=.35f*speed && room_approach>=.06f && !target_only) {
                approach_ns_[i]=s.ns; approach_speed_[i]=approach;
            }
            const bool deliberate=approach_ns_[i]>0 && s.ns-approach_ns_[i]<=GraceNs;
            const bool withdrawing=room_approach<-.02f || approach<-.02f;
            const bool accepted=outside_[i]>0 && s.ns-outside_[i]<=600'000'000 &&
                travel>=.01f && deliberate && !withdrawing && !target_only;
            if(touch && before>0 && !accepted && !out.refused) {
                out.refused=true;
                out.miss.zone=c.zone; out.miss.label=c.label;
                out.miss.inside_m=std::max(-after,0.f);
                out.miss.approach=approach; out.miss.speed=speed; out.miss.travel=travel;
                out.miss.why=withdrawing||target_only ? VialZones::Refusal::Glancing :
                    !deliberate ? VialZones::Refusal::Slow : VialZones::Refusal::Short;
            }
            if(touch && accepted && !out.stab) {
                out.stab=true; out.hit.zone=c.zone; out.hit.label=c.label;
                out.hit.inside_m=std::max(-after,0.f);
                out.hit.approach=approach; out.hit.best=approach_speed_[i]; out.hit.speed=speed;
                out.hit.travel=travel;
            }
        }
        for(std::size_t i=0;i<outside_zone.size();++i)
            if(outside_zone[i]) needs_exit_[i]=false;
        last_=s;
        if(out.stab) {
            fired_=true; out.refused=false; outside_={}; approach_ns_={}; needs_exit_.fill(true);
            cooldown_until_=s.ns+500'000'000;
        }
        return out;
    }
private:
    Sample last_{};
    AnatomyVelocity velocity_{};
    bool interrupted_{};
    bool fired_{}; // one request until the next explicit draw/reset
    std::array<bool,VialZones::ZoneCount> needs_exit_{};
    std::int64_t cooldown_until_{};
    std::array<std::int64_t,Capsules.size()> outside_{};
    std::array<Vec3,Capsules.size()> origin_{};
    std::array<std::int64_t,Capsules.size()> approach_ns_{};
    std::array<float,Capsules.size()> approach_speed_{};
};
} // namespace VR::VialContact
