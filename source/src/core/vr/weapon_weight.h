// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include "core/vr/body_frame.h"
#include "core/vr/weapon_mass_geometry.h"

// Optional weapon handling, downstream of tremor filtering and support-hand steering.
// Geometry-based rotational inertia and speed-dependent drag provide visual resistance, not force
// feedback. Optional gravity compliance loads the blade at rest. The dominant grip position
// stays exact unless optional load compliance is enabled; this is not an independent rigidbody or collision solver.
namespace VR::Body::Weight {

struct Profile {
    float mass{}, length{}, balance{}; // mass is authored; geometry optionally uses measured surface proxies
    float air{.6f}; // authored drag coefficient, not extra mass
    float width{.07f}, thickness{.025f}, area{}; // coarse shape, metres / square metres
    // Parallel-axis inertia: moving the balance point outward costs mass * distance^2.
    // Fallback dimensions are authored; measured profiles use triangle-area second moments.
    Vec3 center{}; // measured uniform-surface centroid in native R_Weapon coordinates
    float transverse_per_mass{}, axial_per_mass{};
    bool measured{};
    float Inertia() const {
        if (measured) return mass * transverse_per_mass;
        return mass * ((length*length + width*width) / 12.0f + balance*balance);
    }
    float TwistRatio() const {
        const float polar = measured ? mass*axial_per_mass : mass * (width*width + thickness*thickness) / 12.0f;
        return std::clamp(std::sqrt(polar / std::max(Inertia(), 1e-6f)), .12f, .60f);
    }
    float Area() const { return area > 0 ? area : length*width; }
};

inline Profile AuthoredWeapon(int family, bool form_b) {
    // Explicit rigid-weapon profiles. Independent dual blades, flexible transformed weapons,
    // bows, firearms, bare hands and unknown families stay on their existing motion path.
    switch (family) {
    case 20: return form_b ? Profile{1.5f, 1.05f, .42f} : Profile{1.2f, .9f, .32f};
    case 50: return form_b ? Profile{2.4f, 1.35f, .80f, .85f, .22f, .04f, .085f} : Profile{2.4f, .65f, .40f, .65f, .22f, .04f, .060f}; // Hunter Axe
    case 51: return form_b ? Profile{2.8f, 1.5f, .85f} : Profile{1.3f, .85f, .32f};
    case 70: return form_b ? Profile{1.552f, .95f, .53f, .75f, .13f, .035f, .085f} : Profile{1.552f, .55f, .30f, .65f, .13f, .035f, .055f}; // Saw Cleaver
    case 71: return form_b ? Profile{1.455f, 1.0f, .50f, .60f, .10f, .025f, .070f} : Profile{1.455f, .55f, .30f, .50f, .10f, .025f, .045f}; // Saw Spear
    case 80: return form_b ? Profile{6.0f, 1.0f, .72f, 1.0f, .25f, .25f, .180f} : Profile{1.1f, .85f, .30f, .35f, .045f, .012f, .035f}; // Kirkhammer
    case 81: return form_b ? Profile{3.2f, 1.3f, .60f, .80f, .14f, .05f, .130f} : Profile{1.1f, .85f, .30f, .35f, .045f, .012f, .035f}; // Ludwig's
    case 100: return form_b ? Profile{2.2f, 1.35f, .60f} : Profile{1.6f, .90f, .40f};
    case 101: return form_b ? Profile{} : Profile{1.1f, .9f, .30f}; // firing form stays exact
    case 120: return {4.5f, .65f, .40f};
    case 130: return {1.6f, .65f, .43f};
    case 220: return form_b ? Profile{} : Profile{.8f, .8f, .28f};
    case 230: return form_b ? Profile{1.3f, .6f, .28f} : Profile{1.3f, .95f, .48f};
    case 240: return form_b ? Profile{} : Profile{2.8f, .9f, .55f, .95f, .16f, .05f, .120f}; // Beast Cutter; B uses its chain solver
    case 250: return form_b ? Profile{} : Profile{3.5f, 1.0f, .65f};
    case 260: return form_b ? Profile{3.0f, 1.4f, .65f} : Profile{2.0f, 1.1f, .45f};
    case 280: return {3.5f, .9f, .65f};
    case 290: return form_b ? Profile{3.5f, 1.25f, .80f} : Profile{1.6f, .65f, .43f};
    case 300: return form_b ? Profile{2.0f, 1.25f, .65f} : Profile{1.2f, .85f, .32f};
    case 310: return form_b ? Profile{4.0f, 1.35f, .85f} : Profile{1.8f, .7f, .43f};
    case 320: return form_b ? Profile{} : Profile{1.1f, .85f, .30f};
    default: return {};
    }
}

inline Profile ForWeapon(int family, bool form_b, bool mesh=false, float scale=1.0f) {
    auto p = AuthoredWeapon(family,form_b);
    if (!mesh || p.mass <= 0 || !std::isfinite(scale) || scale <= 0) return p;
    for (const auto& g : Measured::Shapes) {
        if (g.family != family || g.form_b != form_b) continue;
        p.length=g.length*scale; p.width=g.width*scale; p.thickness=g.thickness*scale;
        p.area=g.area*scale*scale; p.center=g.center*scale; p.balance=Length(p.center);
        p.transverse_per_mass=g.transverse_per_mass*scale*scale;
        p.axial_per_mass=g.axial_per_mass*scale*scale; p.measured=true;
        break;
    }
    return p;
}

inline Vec3 RotationVector(Quat q) {
    q = Normalize(q);
    if (q.w < 0.0f) q = {-q.x, -q.y, -q.z, -q.w};
    const Vec3 v{q.x, q.y, q.z};
    const float n = Length(v);
    return v * (n > 1e-8f ? 2.0f * std::atan2(n, q.w) / n : 2.0f);
}
inline Quat Rotation(Vec3 v) {
    const float n = Length(v);
    if (n < 1e-8f) return Normalize(Quat{v.x*.5f, v.y*.5f, v.z*.5f, 1.0f});
    const float s = std::sin(n*.5f) / n;
    return {v.x*s, v.y*s, v.z*s, std::cos(n*.5f)};
}
inline float Turn(Quat a, Quat b) { return Length(RotationVector(b * Conjugate(a))); }

struct Input {
    int family{-1};
    bool form_b{}, active{};
    float strength{}; // 0 off, 1 the full authored preset; fractional values scale it
    float support{}; // actual attachment blend, not merely the game's two-handed form
    float separation{}; // metres between hands
    Vec3 blade_axis{0, 0, -1}; // tracking-space blade direction
    Vec3 reference_position{}; // head at the SAME prediction time, for relative hand movement
    bool reference_valid{}; // absent: no translational drag; rotation still sweeps the blade
    float gravity{}; // independently opt-in 0..1; master strength still gates all handling
    Vec3 gravity_down{0,-1,0}; // tracking-space gravity, independent of head tilt
    bool measured_shape{};
    float model_to_metres{1};
    float load_response{}; // experimental 0..1 compliance, independently opt-in
    float lift_response{}; // experimental acceleration load, requires load_response
    Vec3 gravity_arm{}; // optional grip-to-centroid vector in tracking space, incl. grip trims
};

class Filter {
public:
    // Weapon/form/scale changes are rare. Keep mesh lookup and derived inertia off
    // the steady frame path; this cache is owned by the existing frame-thread filter.
    const Profile& ProfileFor(const Input& in) {
        if (!profile_valid_ || profile_family_ != in.family || profile_form_ != in.form_b ||
            profile_mesh_ != in.measured_shape || profile_scale_ != in.model_to_metres) {
            profile_ = ForWeapon(in.family,in.form_b,in.measured_shape,in.model_to_metres);
            native_center_ = profile_.measured ? ForWeapon(in.family,in.form_b,true).center : Vec3{};
            inertia_ = profile_.Inertia();
            profile_twist_ = profile_.TwistRatio();
            profile_family_ = in.family; profile_form_ = in.form_b;
            profile_mesh_ = in.measured_shape; profile_scale_ = in.model_to_metres;
            profile_valid_ = true;
        }
        return profile_;
    }
    Vec3 NativeCenter() const { return native_center_; }

    // Returns true only when a continuous, weighted sample (including angular velocity) exists.
    // The unweighted pose is retained on reset, so gaps cannot invent weapon momentum.
    bool Step(std::int64_t time, std::uint32_t space, TrackedHand& hand, const Input& in) {
        if (!in.active || !std::isfinite(in.strength) || in.strength <= 0 ||
            !IsTracked(hand) || !IsFinite(in.blade_axis) ||
            !std::isfinite(in.support) || !std::isfinite(in.separation)) {
            Clear();
            return false;
        }
        const auto& profile = ProfileFor(in);
        if (profile.mass == 0) { Clear(); return false; }
        const Quat target = Normalize(hand.q);
        const float strength = std::clamp(in.strength, 0.0f, 1.0f);
        const float support = std::clamp(in.support, 0.0f, 1.0f);
        const float leverage = Smoothstep(.06f, .45f, in.separation);
        const float inertia = inertia_;
        const float control = 1.0f + support * (.35f + 1.25f * leverage);
        const float desired_tau = strength * (.008f + .020f * inertia / (inertia + .6f)) / control;
        // Drag torque depends on exposed area AND its lever from the grip. Keep
        // extension separate from mass (e.g. both axe forms have the same mass).
        const float desired_air = strength * profile.air * (profile.Area()/.05f) *
                                  (profile.balance/.45f) / control;
        const float desired_lever = profile.balance;
        const float desired_twist = profile_twist_;
        const float desired_limit = strength * (3.0f + 7.0f * inertia / (inertia + .8f)) *
                                    (1.0f - support * (.20f + .40f * leverage)) * Deg;
        const bool gravity_enabled = std::isfinite(in.gravity) && in.gravity > 0 &&
                                     IsFinite(in.gravity_down) && LengthSq(in.gravity_down) > .5f;
        const Vec3 desired_arm = in.measured_shape && profile.measured && IsFinite(in.gravity_arm) &&
            LengthSq(in.gravity_arm)>1e-8f ? in.gravity_arm : Normalize(in.blade_axis,{0,0,-1})*profile.balance;
        const Vec3 local_arm = Rotate(Conjugate(target),desired_arm);
        const float load = profile.mass * (in.measured_shape && profile.measured ? Length(desired_arm) : profile.balance);
        const float load_response = gravity_enabled && std::isfinite(in.load_response) ?
            std::clamp(in.load_response,0.0f,1.0f) : 0.0f;
        const float legacy_gravity = gravity_enabled ? strength * std::clamp(in.gravity,0.0f,1.0f) *
            (8.0f * load/(load+1.0f)) * Deg / (1.0f+support*(.4f+2.6f*leverage)) : 0.0f;
        const float loaded_gravity = strength * std::clamp(in.gravity,0.0f,1.0f) *
            (14.0f*load/(load+.6f))*Deg / (1.0f+support*(.25f+1.15f*leverage));
        const float desired_gravity = load_response > 0 ?
            legacy_gravity+(loaded_gravity-legacy_gravity)*load_response : legacy_gravity;
        const bool relative_valid = in.reference_valid && IsFinite(in.reference_position);
        const Vec3 relative = relative_valid ? hand.p - in.reference_position : Vec3{};
        const double elapsed = have_ ? static_cast<double>(time - time_) * 1e-9 : 0;
        // The reset guard and angular velocity use the same shortest-arc rotation.
        // Calculate its normalized quaternion/log once, retaining the exact guard.
        const Vec3 delta = have_ ? RotationVector(target * Conjugate(raw_)) : Vec3{};
        if (!have_ || in.measured_shape != measured_shape_ || space != space_ || in.family != family_ || elapsed <= 0 ||
            elapsed > .05 || Length(hand.p - position_) > .25f ||
            Length(delta) > 45.0f * Deg) {
            have_ = true; time_ = time; space_ = space; family_ = in.family;
            raw_ = q_ = target; position_ = hand.p;
            tau_ = desired_tau; air_ = desired_air; limit_ = desired_limit;
            lever_ = desired_lever; twist_ = desired_twist;
            gravity_limit_ = desired_gravity; gravity_state_ = {}; gravity_sag_ = 0;
            local_arm_ = local_arm; measured_shape_ = in.measured_shape;
            relative_ = relative; relative_valid_ = relative_valid; linear_ = {}; turning_ = {};
            load_offset_ = {}; lift_velocity_ = {}; lift_acceleration_ = {}; lift_scale_ = 1; force_drag_ = {};
            hand.q = target;
            return false;
        }
        const float dt = static_cast<float>(elapsed);
        // Form and grip changes blend their parameters, never the tracked input or clock.
        const float blend = 1.0f - std::exp(-dt / .24f);
        tau_ += (desired_tau - tau_) * blend;
        air_ += (desired_air - air_) * blend;
        limit_ += (desired_limit - limit_) * blend;
        lever_ += (desired_lever - lever_) * blend;
        twist_ += (desired_twist - twist_) * blend;
        if (gravity_enabled) gravity_limit_ += (desired_gravity-gravity_limit_) * blend;
        else { gravity_limit_ = 0; gravity_state_ = {}; }
        gravity_sag_ = 0;
        local_arm_ += (local_arm-local_arm_)*blend;
        const Quat previous = q_;
        const Vec3 target_velocity = delta * (1.0f/dt);
        const Vec3 axis = Normalize(in.blade_axis, {0,0,-1});
        // Differentiate already stabilized poses. Runtime/raw angular velocity must not
        // reopen this response with tracking noise after the hand has actually settled.
        turning_ = turning_ + (target_velocity-turning_) * (1.0f-std::exp(-dt/.05f));
        const float speed = Length(turning_);
        // Saturating quadratic drag: nearly absent for precise aiming, substantial for a
        // deliberate swing. The signed turn estimate suppresses rapid alternating noise;
        // no raw controller velocity or acceleration is differentiated a second time.
        const float response = tau_ * speed / (speed + 80.0f * Deg);
        if (relative_valid && relative_valid_ && Length(relative-relative_) < .25f) {
            const Vec3 velocity = (relative-relative_) * (1.0f/dt);
            linear_ = linear_ + (velocity-linear_) * (1.0f-std::exp(-dt/(load_response>0?.06f:.015f)));
        } else {
            linear_ = {};
        }
        // Estimate deliberate lifting from stabilized, head-relative motion. Two low-pass
        // stages keep pose differentiation from reintroducing post-stop tracking noise.
        // Common locomotion cancels; invalid reference data cannot invent an inertial load.
        float desired_lift_scale = 1;
        const float lift = load_response > 0 && std::isfinite(in.lift_response) ?
            std::clamp(in.lift_response,0.0f,1.0f) : 0.0f;
        if (lift > 0 && relative_valid && relative_valid_ && Length(relative-relative_) < .25f) {
            const Vec3 velocity = (relative-relative_)*(1.0f/dt);
            const Vec3 previous_velocity = lift_velocity_;
            lift_velocity_ += (velocity-lift_velocity_)*(1.0f-std::exp(-dt/.10f));
            const Vec3 acceleration = (lift_velocity_-previous_velocity)*(1.0f/dt);
            lift_acceleration_ += (acceleration-lift_acceleration_)*(1.0f-std::exp(-dt/.12f));
            const float deliberate_lift = Smoothstep(.12f,.35f,Length(lift_velocity_));
            desired_lift_scale += .8f*lift*deliberate_lift*std::clamp(
                -Dot(lift_acceleration_,Normalize(in.gravity_down,{0,-1,0}))/9.81f,-.30f,.30f);
        } else { lift_velocity_ = {}; lift_acceleration_ = {}; }
        if (lift > 0) lift_scale_ += (desired_lift_scale-lift_scale_)*(1.0f-std::exp(-dt/.06f));
        else lift_scale_ = 1;
        // Air velocity at the effective blade area, including the sweep of a long
        // blade around a stationary grip. Common head/hand translation still cancels.
        // Axial thrust and shaft twist make no transverse air torque.
        // Fade rotational air torque near rest so residual tracking motion cannot
        // keep a large blade loaded after the deliberate swing has stopped.
        const float deliberate = std::max(0.0f, speed - 60.0f*Deg) /
                                 std::max(speed, 1e-6f);
        const Vec3 swept = Cross(turning_,axis*lever_) * (.25f*deliberate*deliberate);
        const Vec3 transverse = Perp(linear_ + swept,axis);
        Vec3 drag = Cross(axis,transverse) * (-air_ * .8f * Deg * Length(transverse));
        const float drag_magnitude = Length(drag), drag_cap = limit_ * .30f;
        if (drag_magnitude > drag_cap) drag = drag * (drag_cap/drag_magnitude);
        // Smooth the visual force, never the tracked position. Quadratic drag otherwise
        // amplifies differentiated sub-millimetre noise while a fast movement stops.
        if (load_response > 0) {
            force_drag_ += (drag-force_drag_)*(1.0f-std::exp(-dt/.09f));
            drag = force_drag_;
        } else { force_drag_ = {}; }
        drag_cue_ = drag;
        relative_ = relative; relative_valid_ = relative_valid;
        // Angular deflection is a bounded opposing torque cue. It integrates no independent
        // weapon momentum; on a clean stop the deflection decays back to the tracked target.
        // Axial rotation has less inertia than swinging the blade across its length.
        Vec3 next_error = (Perp(turning_,axis) + axis*Dot(turning_,axis)*twist_) *
                          (-response) + drag;
        if (gravity_enabled) {
            // Gravitational torque is proportional to the horizontal moment arm.
            // Smooth the load, then project it onto the CURRENT downward torque direction:
            // stale gravity history must never raise the tip or roll a vertical shaft.
            const Vec3 gravity_axis = in.measured_shape && profile.measured ?
                Rotate(target,Normalize(local_arm_,{0,0,-1})) : axis;
            const Vec3 torque = Cross(gravity_axis,Normalize(in.gravity_down,{0,-1,0}));
            const Vec3 desired = torque * (gravity_limit_*lift_scale_);
            gravity_state_ += (desired-gravity_state_) * (1.0f-std::exp(-dt/.07f));
            const float t2 = LengthSq(torque);
            const Vec3 sag = t2 > 1e-8f ? torque *
                std::clamp(Dot(gravity_state_,torque)/t2,0.0f,
                    std::max(gravity_limit_*lift_scale_,Length(gravity_state_))) : Vec3{};
            gravity_sag_ = Length(sag);
            next_error += sag;
        }
        if (load_response > 0) {
            // A compliant weapon has finite rotational response even near rest. Damping
            // the driven orientation prevents gravity/drag from amplifying tiny target
            // changes. This affects the weapon only; tracking and hand filtering stay exact.
            const float drive_tau = load_response*(.012f+.008f*inertia/(inertia+.6f))/control;
            const float follow = 1.0f-std::exp(-dt/std::max(drive_tau,1e-5f));
            const Vec3 previous_error = RotationVector(previous*Conjugate(target));
            next_error = previous_error+(next_error-previous_error)*follow;
        }
        const float error_length = Length(next_error), total_limit = Limit();
        if (error_length > total_limit && error_length > 1e-8f) {
            const Vec3 n = next_error * (1.0f/error_length);
            next_error = n * total_limit;
        }
        q_ = Normalize(Rotation(next_error) * target);
        time_ = time; raw_ = target; position_ = hand.p;
        // Compliance adds a bounded vertical load cue without delaying tracked translation.
        // Store the unweighted position above so this displacement never feeds back into itself.
        const Vec3 old_offset = load_offset_;
        if (load_response > 0) {
            const float drop = strength * std::clamp(in.gravity,0.0f,1.0f) * load_response *
                (.020f*profile.mass/(profile.mass+1.0f)) /
                (1.0f+support*(.25f+.75f*leverage));
            const Vec3 desired_offset = Normalize(in.gravity_down,{0,-1,0})*std::min(.020f,drop*lift_scale_);
            load_offset_ += (desired_offset-load_offset_)*(1.0f-std::exp(-dt/.10f));
        } else { load_offset_ = {}; }
        if (LengthSq(load_offset_) > 0 || LengthSq(old_offset) > 0) {
            hand.p += load_offset_;
            if (hand.velocity_valid) hand.v += (load_offset_-old_offset)*(1.0f/dt);
        }
        hand.q = q_;
        hand.w = RotationVector(q_ * Conjugate(previous)) * (1.0f/dt);
        hand.angular_valid = true;
        return true;
    }
    void Clear() {
        have_ = false;
        load_offset_ = {}; lift_velocity_ = {}; lift_acceleration_ = {}; lift_scale_ = 1;
        gravity_sag_ = gravity_limit_ = 0;
        drag_cue_ = {};
    }
    Vec3 DragCue() const { return drag_cue_; }
    float LiftScale() const { return lift_scale_; }
    Vec3 PositionOffset() const { return load_offset_; }
    float Limit() const { return std::min(14.0f*Deg,limit_+gravity_limit_); }
    float GravitySag() const { return gravity_sag_; }
private:
    Profile profile_{};
    Vec3 native_center_{};
    float inertia_{}, profile_twist_{}, profile_scale_{};
    int profile_family_{};
    bool profile_valid_{}, profile_form_{}, profile_mesh_{};
    bool have_{};
    std::int64_t time_{};
    std::uint32_t space_{};
    int family_{-1};
    Quat raw_{}, q_{};
    Vec3 position_{}, relative_{}, linear_{}, turning_{};
    bool relative_valid_{};
    float tau_{}, air_{}, limit_{}, lever_{}, twist_{};
    float gravity_limit_{}, gravity_sag_{}, lift_scale_{1};
    Vec3 lift_velocity_{}, lift_acceleration_{}, force_drag_{};
    Vec3 gravity_state_{}, local_arm_{}, load_offset_{}, drag_cue_{};
    bool measured_shape_{};
};

// Preserve the attached off-hand's grip relationship when the shared weapon rotates.
inline void CarrySupport(HandsSample& sample, Quat previous_right, Vec3 position_shift = {}) {
    if (sample.two_hand <= 0 || !IsTracked(sample.hand[0])) return;
    const auto& r = sample.hand[1];
    auto& l = sample.hand[0];
    const Quat turn = ScaleRotation(Normalize(r.q * Conjugate(previous_right)),
                                    std::clamp(sample.two_hand, 0.0f, 1.0f));
    l.p = r.p + Rotate(turn, l.p-r.p+position_shift) -
        position_shift*(1.0f-std::clamp(sample.two_hand,0.0f,1.0f));
    l.q = Normalize(turn*l.q);
    // The adjusted linear velocity is not the controller's; shared combat differentiates
    // the final poses in BladeMotion after this operation.
    l.velocity_valid = l.angular_valid = false;
}
} // namespace VR::Body::Weight
