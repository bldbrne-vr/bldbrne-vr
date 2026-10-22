// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>
#include <utility>
#include "core/vr/body_frame.h"
#include "core/vr/visceral_channel.h"
#include "core/vr/visceral_clearance.h"

namespace VR::GrabCamera {

// Only known victim states take the eye itself. An outgoing freehand throw instead gives the
// hunter's facing and the victim's root (ThrowCamera).
inline bool Victim(bool valid, unsigned state) {
    return valid && (state == 2 || state == 4 || state == 6);
}

struct Sample {
    Body::Vec3 eye{}; // world position (a grab; unused for a throw)
    std::int64_t ns{0};
    bool valid{false};
    bool own_throw{false}; // the hunter's own freehand throw (ThrowCamera), not a grab
    // A throw only: the hunter's forward (world) and his victim's root (world), if it was read.
    Body::Vec3 facing{};
    Body::Vec3 victim{};
    bool victim_valid{false};
    const void* victim_character{nullptr}; // refreshed and consumed synchronously by BodyGrabEye on the loop thread
};

// The camera through the hunter's freehand throw (a visceral, riposte or backstab; 2026-10-14,
// the tester: "freeze our movement into place (except for hands and leaning)"): your view stays
// where it was when the throw began, whatever the game's paired positioning and his root motion
// do (the enemy is brought to you; his animated lunge is not followed: it went low and into the
// enemy), your head and hands tracked one to one. When the throw ends the view glides onto him
// over GlideS (longer for a long way: at most GlideMps on average). Advanced on the first eye of a
// stereo pair; the second eye reuses it.
//
// KeepOut enforces KeepOutM from the victim surface along an eye-level horizontal ray.
// Retreat is immediate to prevent entry clipping; return motion is limited to PushMps.
// With no readable collision body, retain the older root-distance fallback.
class ThrowCamera {
public:
    static constexpr float GlideS = 0.3f;
    static constexpr float GlideMps = 2.0f;
    static constexpr float MaxM = 3.0f; // the base moved further (a load, a warp): a cut
    static constexpr float KeepOutM = 1.25f;
    static constexpr float PushMps = 1.5f;

    // `base`: the camera's base this frame (his root at eye height, comfort-smoothed);
    // `facing`: his forward. The offset to add to the base (the base back to where it was).
    Body::Vec3 Step(bool throwing, Body::Vec3 base, Body::Vec3 facing, float scale, bool advance,
                    std::int64_t now) {
        if (!advance) {
            return applied_;
        }
        const bool recent = last_ns_ != 0 && now >= last_ns_ &&
                            now - last_ns_ <= 150'000'000;
        const float dt = last_ns_ != 0 && now >= last_ns_
                             ? std::min(static_cast<float>(now - last_ns_) * 1e-9f, 0.1f)
                             : 0.0f;
        last_ns_ = now;
        dt_ = dt;
        throwing = throwing && Body::IsFinite(base);
        if (throwing && !holding_) {
            // Pair alignment may already have moved the hunter on this first throw frame.
            // Hold the last rendered base, not that newly staged position. A stale view or
            // a real warp must not pull the camera back across the world.
            const bool keep_view = recent && have_view_ &&
                Body::LengthSq(view_ - base) <= MaxM * MaxM * scale * scale;
            anchor_ = keep_view ? view_ : base + applied_;
            have_surface_ = false;
            surface_push_ = {};
            facing_ = Body::Normalize(Body::Vec3{facing.x, 0.0f, facing.z});
            holding_ = true;
            running_ = true;
        } else if (!throwing && holding_) {
            holding_ = false;
            glide_s_ = std::max(GlideS, Body::Length(hold_) / (GlideMps * scale));
        }
        if (holding_) {
            hold_ = anchor_ - base;
            if (Body::LengthSq(hold_) > MaxM * MaxM * scale * scale) {
                // Cut to him and hold from there (the victim is read afresh).
                anchor_ = base;
                hold_ = {};
                push_ = {};
                victim_valid_ = false;
                have_surface_ = false;
                surface_push_ = {};
                cut_ = true;
            }
            w_ = 1.0f;
        } else {
            w_ = std::max(w_ - dt / glide_s_, 0.0f);
        }
        if (w_ <= 0.0f && !holding_) {
            running_ = false;
            victim_valid_ = false;
        }
        applied_ = hold_ * Eased();
        have_view_ = Body::IsFinite(base);
        if (have_view_)
            view_ = base + applied_;
        return applied_;
    }

    // After Step. `camera`: the camera's head centre with the hold applied, `staged`: the
    // hunter's eye on his root (a victim over 3 m from it is not the victim). The push that keeps
    // the camera out (to add to it); the victim holds while the view glides back.
    Body::Vec3 KeepOut(Body::Vec3 camera, Body::Vec3 staged, bool victim_valid, Body::Vec3 victim,
                       float scale, bool advance, SurfaceRay surface = {}) {
        if (!advance) {
            return push_;
        }
        const auto flat = [](Body::Vec3 v) { return Body::Vec3{v.x, 0.0f, v.z}; };
        if (victim_valid && running_ && Body::IsFinite(victim) && Body::IsFinite(staged) &&
            Body::Length(flat(staged - victim)) <= 3.0f * scale) {
            victim_ = victim;
            victim_valid_ = true;
        }
        Body::Vec3 want{};
        const float clear = KeepOutM * scale;
        if (victim_valid_ && Body::IsFinite(camera)) {
            const Body::Vec3 d = flat(camera - victim_);
            const float dist = Body::Length(d);
            if (dist < clear) {
                const Body::Vec3 out = Body::Normalize(d, facing_ * -1.0f);
                want = out * ((clear - dist) * Eased());
            }
        }
        if (holding_ && victim_valid_ && surface.hit && std::isfinite(surface.distance) &&
            surface.distance >= -MaxM * scale && surface.distance <= 6.0f * scale &&
            Body::IsFinite(surface.direction) && std::abs(surface.direction.y) < 1e-5f &&
            std::abs(Body::LengthSq(surface.direction) - 1.0f) < 0.001f) {
            const auto out = surface.direction * -1.0f;
            const float required = std::max(clear - surface.distance, 0.0f);
            want = out * required;
            // Waiting for the old 1.5 m/s easing is exactly what allowed the first
            // stab frames to clip. Both eyes reuse this first-eye correction.
            if (Body::Dot(push_, out) < required) push_ = want;
            have_surface_ = true;
        } else if (have_surface_) {
            // A missed body read must not draw the eye forward into the victim.
            // After the throw, fade the last correction with the existing glide.
            want = surface_push_ * Eased();
        }
        // Ease only the return (or the legacy root-only fallback).
        const Body::Vec3 change = want - push_;
        const float step = PushMps * scale * dt_;
        const float length = Body::Length(change);
        push_ = length > step ? push_ + change * (step / length) : want;
        if (holding_ && have_surface_) surface_push_ = push_;
        if (!running_ && Body::LengthSq(push_) < 1e-8f) have_surface_ = false;
        return push_;
    }

    void Reset() {
        *this = ThrowCamera{};
    }
    [[nodiscard]] bool Holding() const { return holding_; }
    [[nodiscard]] float Weight() const { return Eased(); }
    [[nodiscard]] bool Running() const {
        return running_;
    }
    [[nodiscard]] Body::Vec3 Held() const {
        return hold_;
    }
    [[nodiscard]] float GlideTime() const {
        return glide_s_;
    }
    // A warp past MaxM let go of the hold (once).
    [[nodiscard]] bool TakeCut() {
        return std::exchange(cut_, false);
    }

private:
    [[nodiscard]] float Eased() const {
        return w_ * w_ * (3.0f - 2.0f * w_);
    }

    float w_{0.0f};
    bool holding_{false}; // the throw runs
    bool running_{false}; // ... or the view still glides back from it
    Body::Vec3 view_{};   // last rendered base before head/eye offsets and keep-out
    bool have_view_{false};
    Body::Vec3 anchor_{}; // the view's base before the throw's paired positioning
    Body::Vec3 hold_{};   // the anchor - the base (kept as the throw ended, for the glide)
    Body::Vec3 applied_{};
    Body::Vec3 facing_{}; // his, horizontal, when it began
    Body::Vec3 victim_{};
    bool victim_valid_{false};
    Body::Vec3 push_{};
    Body::Vec3 surface_push_{};
    bool have_surface_{false};
    float dt_{0.0f};
    float glide_s_{GlideS};
    bool cut_{false};
    std::int64_t last_ns_{0};
};

// Preserve the pre-throw relationship between the camera base and the whole
// hunter, including the legs. Merely applying the camera's hold offset is not
// enough: its comfort spring can lag a full metre behind native paired positioning.
class ThrowBodyRig {
public:
    Body::Vec3 Step(Body::Vec3 root, Body::Vec3 base, Body::Vec3 camera_offset,
                   float weight, float scale, std::int64_t now) {
        if (!Body::IsFinite(root) || !Body::IsFinite(base) ||
            !Body::IsFinite(camera_offset) || !std::isfinite(weight) ||
            !std::isfinite(scale) || scale <= 0.0f) {
            Reset();
            return {};
        }
        const bool recent = have_ && now >= ns_ && now - ns_ <= 250'000'000;
        const bool active = weight > 0.0f || Body::LengthSq(camera_offset) > 1e-10f;
        // An active camera hold survives a frame hitch. Its body reference must too.
        // Freshness only rejects an old idle reference on a new entry.
        if (!active || !have_ || now < ns_ || (!active_ && !recent)) reference_ = root - base;
        active_ = active;
        ns_ = now;
        have_ = true;
        const auto offset = camera_offset +
            (base + reference_ - root) * std::clamp(weight, 0.0f, 1.0f);
        if (Body::LengthSq(offset) > 36.0f * scale * scale) {
            Reset(); // a load or warp is never a six-metre body displacement
            return {};
        }
        return offset;
    }
    [[nodiscard]] bool HasReference() const { return have_; }
    void Reset() { *this = {}; }
private:
    Body::Vec3 reference_{}; // native root minus the pre-throw, untracked eye base
    std::int64_t ns_{};
    bool have_{};
    bool active_{};
};

// One immutable pose decision, copied to the renderer through BodyStatus. All
// positions are game-world units. The renderer adds live head motion and stereo;
// it never recomputes this artificial displacement from a different ray origin.
struct ThrowPlacement {
    Body::Vec3 base{}; // corrected, untracked camera base
    Body::Vec3 root{}; // presented whole-body root
    SurfaceRay surface{};
    std::int64_t ns{};
    float weight{};
    float scale{1.0f};
    bool valid{};
    bool holding{};
};

class ThrowPlacementController {
public:
    template <typename Cast>
    ThrowPlacement Step(Body::Vec3 root, Body::Vec3 previous_root, Body::Vec3 base,
                        Body::Vec3 head, const Sample& sample, float scale,
                        bool advance, std::int64_t now, Cast&& cast) {
        if (!Body::IsFinite(root) || !Body::IsFinite(previous_root) ||
            !Body::IsFinite(base) || !Body::IsFinite(head) ||
            !std::isfinite(scale) || scale <= 0 || now < last_ns_) {
            Reset();
            return {};
        }
        if (last_ns_ && scale != scale_) {
            Reset();
            advance = true;
        }
        // A native teleport invalidates both halves, even if the comfort base lags.
        if (Body::LengthSq(root - previous_root) > 9.0f * scale * scale) {
            base += root - previous_root;
            previous_root = root;
            Reset();
            advance = true;
        }
        scale_ = scale;
        last_ns_ = now;
        if (!advance) {
            auto paired = placement_;
            paired.ns = now;
            return paired;
        }
        const bool was_active = active_;
        if (!was_active) {
            // Entry is anchored to the last rendered frame, not to a comfort
            // sample from another pose earlier than that frame.
            camera_.Reset();
            camera_.Step(false, base, sample.facing, scale, true, now - 1);
            rig_.Reset();
            rig_.Step(previous_root, base, {}, 0, scale, now - 1);
        }
        const bool throwing = sample.valid && sample.own_throw;
        const auto hold = camera_.Step(throwing, base, sample.facing, scale, true, now);
        if (camera_.TakeCut()) {
            rig_.Reset();
            rig_.Step(root, base, {}, 0, scale, now);
        }
        const auto eye = base + hold + head;
        SurfaceRay surface{};
        if (throwing && sample.victim_valid &&
            Body::Length(Body::Vec3{base.x - sample.victim.x, 0, base.z - sample.victim.z}) <= 3 * scale)
            surface = cast(eye);
        const auto push = camera_.KeepOut(eye, base, throwing && sample.victim_valid,
                                          sample.victim, scale, true, surface);
        const auto offset = hold + push;
        const float weight = camera_.Weight();
        const auto rig_offset = rig_.Step(root, base, offset, weight, scale, now);
        if (!rig_.HasReference()) {
            // A rejected body displacement must cut the camera in the same
            // decision, rather than leaving one half of the rig held behind.
            Reset();
            placement_ = {base, root, {}, now, 0, scale, true, false};
            return placement_;
        }
        const bool active = camera_.Running() || Body::LengthSq(push) > 1e-10f;
        // Cache the final recovery pose for its second eye as well. Whether
        // the next first eye starts idle is separate from this pair's validity.
        active_ = active;
        placement_ = {base + offset, root + rig_offset, surface, now, weight,
                      scale, active || was_active, camera_.Holding()};
        return placement_;
    }
    void Reset() { *this = {}; }
private:
    ThrowCamera camera_{};
    ThrowBodyRig rig_{};
    ThrowPlacement placement_{};
    bool active_{};
    std::int64_t last_ns_{};
    float scale_{1.0f};
};

inline bool Fresh(const Sample& s, std::int64_t now) {
    return s.valid && Body::IsFinite(s.eye) && now >= s.ns && now - s.ns <= 150'000'000;
}

// A held animation can stop publishing fresh poses. The game-thread throw is updated
// independently: it owns entry, hold and extraction, even with no body pose at all.
// Pull release only resumes the animation. Cancellation/end takes precedence over an older
// outgoing pose; an incoming grab still uses its fresh animated eye.
inline Sample SelectSample(const Sample& pose, const Combat::VisceralHold& hold,
                           bool freehand, std::int64_t now) {
    const bool fresh = Fresh(pose, now);
    if (fresh && !pose.own_throw)
        return pose;
    if (freehand && hold.ns > 0 && now >= hold.ns && now - hold.ns <= 250'000'000) {
        if (hold.camera_cancelled)
            return {};
        if (hold.camera_valid ? hold.camera_active : hold.active) {
            Sample out = fresh ? pose : Sample{};
            out.eye = {};
            out.ns = now;
            out.valid = out.own_throw = true;
            return out;
        }
        // A body pose from before this end receipt cannot restart the camera hold.
        // A newer pose can announce the next throw before its game-thread receipt.
        if (!fresh || hold.ns >= pose.ns)
            return {};
    }
    return fresh ? pose : Sample{};
}

// Position from the untouched animation; headset orientation is deliberately independent.
inline Body::Vec3 Eye(const Body::ChrFrame& frame, const Body::QsTransform& head,
                       Body::Vec3 on_head) {
    return Body::ToWorld(frame, head.t + Body::Rotate(Body::Normalize(head.q), on_head));
}

// Cache the ordinary camera too: a grab beginning or ending on the second eye
// must use the same base as the first eye. Never lock across a long frame gap.
class PairPosition {
public:
    std::optional<Body::Vec3> Step(Body::Vec3 eye, bool grabbing, bool first,
                                   std::int64_t now, bool lock_enabled,
                                   std::int64_t max_age, const Body::PairLock* decision = nullptr) {
        if (!Body::IsFinite(eye)) {
            Reset();
            return std::nullopt;
        }
        const bool recent = active && now >= pair_ns && now - pair_ns <= max_age;
        bool locked = lock_enabled && recent && !first;
        if (recent && !first && decision && decision->first_ns >= pair_ns &&
            decision->decided_ns > pair_ns && decision->decided_ns <= now) {
            locked = decision->locked;
        }
        if (locked && (grabbing || pair_grab)) {
            return position;
        }
        if (!locked) {
            position = eye;
            pair_ns = now;
            pair_grab = grabbing;
            active = true;
        }
        return grabbing ? std::optional{eye} : std::nullopt;
    }
    void Reset() { active = false; }
private:
    bool active{false};
    bool pair_grab{false};
    std::int64_t pair_ns{0};
    Body::Vec3 position{};
};

} // namespace VR::GrabCamera
