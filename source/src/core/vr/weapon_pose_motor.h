// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once
#include "core/vr/weapon_weight.h"
#include "core/vr/weapon_grip_inertia.h"

// Private opt-in orientation motor. Authored equilibrium load; no collision solver.
namespace VR::Body::Weight::MotorDetail {
namespace B=VR::Body;
namespace W=B::Weight;
struct Motor {
    B::Quat q{}, last_target{};
    B::Vec3 omega{}, target_velocity{};
    bool have{};
    double last_time{};
    int resets{};
    float peak_error{}, peak_torque{}, gravity_sag{};
    // Partial target-velocity damping avoids treating all intentional movement as
    // a disturbance. Keep this tunable: full feedforward can pass tracking noise.
    float feedforward=.60f;
    float torque_scale=8;
    int saturated_steps{}, bounded_steps{};
    GripInertia shape;
    B::Quat Step(double time, B::Quat target, const W::Profile& p,
                 B::Vec3 local_arm, float support=0, float gravity=1, B::Vec3 external_bias={}, B::Vec3 down={0,-1,0}) {
        target=B::Normalize(target);
        const double elapsed=time-last_time;
        if(!have || elapsed<=0 || elapsed>.05 ||
           W::Turn(last_target,target)>45*B::Deg) {
            have=true;last_time=time;q=last_target=target;
            omega=target_velocity={};++resets;return q;
        }
        const float dt=float(elapsed);
        auto velocity=W::RotationVector(target*B::Conjugate(last_target))*(1/dt);
        target_velocity+=(velocity-target_velocity)*(1-std::exp(-dt/.035f));
        const float control=1+1.6f*support;
        const float max_torque=(40+18*p.mass)*control*torque_scale;
        const float load=p.mass*B::Length(local_arm);
        // An authored grip equilibrium represents active human support. This is
        // deliberately separate from motor bandwidth, not physical free-fall.
        const auto torque_direction=B::Cross(B::Rotate(target,local_arm),B::Normalize(down,{0,-1,0}));
        const float arm=std::max(B::Length(local_arm),1e-6f);
        const float sag=gravity*(10*load/(load+.6f))*B::Deg/control;
        gravity_sag=B::Length(torque_direction)*(sag/arm);
        const auto goal=B::Normalize(W::Rotation(torque_direction*(sag/arm)+external_bias)*target);
        // Backward-Euler PD velocity update, bounded fixed cost. Cap actual torque
        // after solving, so a tracking discontinuity cannot invent infinite force.
        constexpr int steps=2;
        const float h=dt/steps;
        for(int i=0;i<steps;++i){
            auto error=W::RotationVector(goal*B::Conjugate(q));
            auto desired=target_velocity*feedforward;
            B::Vec3 impulse{},torque{};
            for(int axis=0;axis<3;axis++){
                const auto direction=B::Rotate(q,shape.axes[axis]);
                const float inertia=std::max(.025f,shape.values[axis]);
                const float stiffness=1.5f*(180+220*std::sqrt(inertia))*control;
                const float damping=2.2f*std::sqrt(stiffness*inertia);
                const float current=B::Dot(omega,direction);
                const float next=(current+(B::Dot(error,direction)*stiffness+B::Dot(desired,direction)*damping)*(h/inertia)) /
                    (1+h*damping/inertia+h*h*stiffness/inertia);
                impulse+=direction*(next-current);
                torque+=direction*((next-current)*inertia/h);
            }
            const float norm=B::Length(torque);
            if(norm>max_torque){impulse=impulse*(max_torque/norm);++saturated_steps;}
            peak_torque=std::max(peak_torque,std::min(norm,max_torque));
            omega+=impulse;
            q=B::Normalize(W::Rotation(omega*h)*q);
        }
        // Smooth displayed compliance prevents a hard projection discontinuity.
        // Internal motion remains independent; measure its recovery separately.
        auto relative=W::RotationVector(q*B::Conjugate(target));
        const float length=B::Length(relative), limit=14*B::Deg;
        if(length>limit)++bounded_steps;
        if(length>1e-6f)relative=relative*(limit*std::tanh(length/limit)/length);
        const auto output=B::Normalize(W::Rotation(relative)*target);
        peak_error=std::max(peak_error,W::Turn(target,output));
        last_time=time;last_target=target;return output;
    }
};
}
