// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "core/vr/weapon_pose_motor.h"

namespace VR::Body::Weight {
// Private opt-in candidate. Existing filter supplies translation and aerodynamic
// load; the independent rotational motor replaces its orientation output.
class Handling {
public:
    const Profile& ProfileFor(const Input& in) { return legacy_.ProfileFor(in); }
    Vec3 NativeCenter() const { return legacy_.NativeCenter(); }
    void ConfigureMotor(bool enabled,const TwoHand::BoneOnGrip& grip,Vec3 shift,float scale) {
        requested_=enabled;grip_=grip;shift_=shift;scale_=scale;
    }
    bool Step(std::int64_t time,std::uint32_t space,TrackedHand& hand,const Input& in) {
        const auto& profile=legacy_.ProfileFor(in);
        const bool use=requested_&&in.measured_shape&&profile.measured&&IsFinite(grip_.grip)&&
            IsFinite(grip_.offset)&&IsFinite(shift_)&&std::isfinite(scale_)&&scale_>1e-6f;
        if(use!=mode_){Clear();mode_=use;}
        const Quat target=Normalize(hand.q);
        const bool active=legacy_.Step(time,space,hand,in);
        if(!use){motor_.have=false;return active;}
        if(!active){motor_.have=false;have_output_=false;previous_output_=target;return false;}
        // Include the support-hand grip shift in the same native bone mapping used
        // by the renderer. Cache eigendecomposition when calibration is unchanged.
        auto effective=grip_;
        effective.offset=TwoHand::BoneToGripLocal(grip_,shift_,scale_);
        if(!cached_||family_!=in.family||form_!=in.form_b||cached_scale_!=scale_||
           Turn(cached_grip_.grip,effective.grip)>1e-6f||
           LengthSq(cached_grip_.offset-effective.offset)>1e-12f){
            motor_.shape=MotorDetail::Calibrate(in.family,in.form_b,profile.mass,effective,scale_);
            cached_=true;family_=in.family;form_=in.form_b;cached_scale_=scale_;cached_grip_=effective;
        }
        const Quat previous=previous_output_;
        const float gravity=std::isfinite(in.gravity)&&IsFinite(in.gravity_down)&&LengthSq(in.gravity_down)>.5f ?
            std::clamp(in.gravity,0.f,1.f)*legacy_.LiftScale():0;
        const auto driven=motor_.Step(double(time)*1e-9,target,profile,motor_.shape.center,
                                     std::clamp(in.support,0.f,1.f),gravity,legacy_.DragCue(),in.gravity_down);
        const float strength=std::clamp(in.strength,0.f,1.f);
        hand.q=Normalize(Rotation(RotationVector(driven*Conjugate(target))*strength)*target);
        const double dt=double(time-previous_time_)*1e-9;
        if(have_output_&&dt>0&&dt<=.05){
            hand.w=RotationVector(hand.q*Conjugate(previous))*float(1/dt);hand.angular_valid=true;
        }else{hand.angular_valid=false;hand.w={};}
        previous_time_=time;previous_output_=hand.q;have_output_=true;
        gravity_sag_=motor_.gravity_sag*strength;limit_=14*Deg*strength;
        return true;
    }
    void Clear(){legacy_.Clear();motor_.have=false;have_output_=false;gravity_sag_=0;}
    float Limit()const{return mode_?limit_:legacy_.Limit();}
    float GravitySag()const{return mode_?gravity_sag_:legacy_.GravitySag();}
    Vec3 PositionOffset()const{return legacy_.PositionOffset();}
private:
    Filter legacy_;
    MotorDetail::Motor motor_;
    TwoHand::BoneOnGrip grip_{},cached_grip_{};
    Vec3 shift_{};
    float scale_{1},cached_scale_{},gravity_sag_{},limit_{};
    int family_{};
    bool form_{},cached_{},requested_{},mode_{},have_output_{};
    std::int64_t previous_time_{};
    Quat previous_output_{};
};
}
