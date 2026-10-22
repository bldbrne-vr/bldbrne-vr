// SPDX-FileCopyrightText: 2026 BB-VR contributors
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
#include <cstdint>

// Recenter: a change of the tracking space (LOCAL) the head and both hands are located in, and the
// fade that hides it. Header-only, no emulator dependencies (tests/test_space_shift.cpp).
//
// The view recenter (Menu held, renderer_vulkan/openxr_presenter.cpp) does what the headset's own
// recenter does, inside the application (OpenXR has no call that asks the runtime for one): a new
// LOCAL space whose origin is the head's position at that moment and whose -Z is the head's heading
// (yaw only, +Y stays up). A point x' of the new space is the point Ry(yaw) x' + origin of the old
// one. The camera tick (core/vr/vr_session.cpp) carries the game view over into the new space: the
// game heading of tracking -Z (base_yaw) turns by -yaw, exactly as much as every head yaw does, so
// the view, the hands' mapping into the game and the 6DoF offsets are unchanged; then the 6DoF
// origin is taken from the head (the camera returns over the hunter).
//
// Conventions: VR::Vec3 / VR::Quat (x, y, z[, w]) of core/vr/vr_session.h, or any type with those
// members; yaw in radians about +Y, + = turned left (YawPitchFromQuat's).
namespace VR::Space {

struct Shift {
    float yaw{0.0f};                 // the new space's -Z in the old space: radians, + = left
    float x{0.0f}, y{0.0f}, z{0.0f}; // the new space's origin in the old space (metres)
    // False: the space changed in a way nobody told us (a runtime recenter without its pose);
    // nothing can be carried over.
    bool known{true};
};

inline float WrapPi(float a) {
    return std::remainder(a, 6.283185307179586f);
}

// Ry(a) v: a turned left by `a` about +Y.
template <typename V>
V RotateY(float a, const V& v) {
    const float c = std::cos(a), s = std::sin(a);
    V out = v;
    out.x = c * v.x + s * v.z;
    out.z = -s * v.x + c * v.z;
    return out;
}

// A point of the old space in the new space's coordinates.
template <typename V>
V PointToNew(const Shift& s, const V& p) {
    V d = p;
    d.x = p.x - s.x;
    d.y = p.y - s.y;
    d.z = p.z - s.z;
    return RotateY(-s.yaw, d);
}

// A point of the new space in the old space's coordinates (inverse of PointToNew).
template <typename V>
V PointToOld(const Shift& s, const V& p) {
    V out = RotateY(s.yaw, p);
    out.x += s.x;
    out.y += s.y;
    out.z += s.z;
    return out;
}

// A direction or velocity of the old space in the new one.
template <typename V>
V VectorToNew(const Shift& s, const V& v) {
    return RotateY(-s.yaw, v);
}

// An orientation of the old space in the new one: Ry(-yaw) * q.
template <typename Q>
Q RotationToNew(const Shift& s, const Q& q) {
    const float h = -0.5f * s.yaw;
    const float c = std::cos(h), sn = std::sin(h);
    Q out = q;
    out.x = c * q.x + sn * q.z;
    out.y = c * q.y + sn * q.w;
    out.z = c * q.z - sn * q.x;
    out.w = c * q.w - sn * q.y;
    return out;
}

// The heading (yaw, + = left) a head orientation faces: its forward's (YawPitchFromQuat's yaw)
// unless it looks within about 20 degrees of straight up or down, where the forward's heading is
// unreliable. There the up vector takes part (looking straight down, the top of the head points
// where the face would look level), so the heading never flips at the poles; for a head without
// roll that blend is exactly the forward's heading too.
template <typename Q>
float Heading(const Q& q) {
    // forward = R(q) (0, 0, -1), up = R(q) (0, 1, 0)
    const float fx = -2.0f * (q.x * q.z + q.w * q.y);
    const float fy = -2.0f * (q.y * q.z - q.w * q.x);
    const float fz = -(1.0f - 2.0f * (q.x * q.x + q.y * q.y));
    if (fx * fx + fz * fz >= 0.35f * 0.35f) {
        return std::atan2(-fx, -fz);
    }
    const float ux = 2.0f * (q.x * q.y - q.w * q.z);
    const float uy = 1.0f - 2.0f * (q.x * q.x + q.z * q.z);
    const float uz = 2.0f * (q.y * q.z + q.w * q.x);
    // f_xz cos(pitch) - u_xz sin(pitch), with cos(pitch) ~ up.y and sin(pitch) = forward.y.
    const float hx = fx * uy - ux * fy;
    const float hz = fz * uy - uz * fy;
    if (hx * hx + hz * hz < 1e-8f) {
        return 0.0f; // degenerate (upside down and sideways): keep the old forward
    }
    return std::atan2(-hx, -hz);
}

// The recenter on a head pose given in the current space: the new space starts at the head and
// looks where it faces.
template <typename Q, typename V>
Shift FromHead(const Q& orientation, const V& position) {
    return Shift{Heading(orientation), position.x, position.y, position.z, true};
}

// `first`, then `second` (given in first's new space): the shift from the space before `first` to
// the one after `second`. Unknown if either is.
inline Shift Compose(const Shift& first, const Shift& second) {
    if (!first.known || !second.known) {
        return Shift{0.0f, 0.0f, 0.0f, 0.0f, false};
    }
    struct P {
        float x, y, z;
    };
    const P o = RotateY(first.yaw, P{second.x, second.y, second.z});
    return Shift{WrapPi(first.yaw + second.yaw), o.x + first.x, o.y + first.y, o.z + first.z, true};
}

// The old space as seen from the new one.
inline Shift Inverse(const Shift& s) {
    struct P {
        float x, y, z;
    };
    const P o = RotateY(-s.yaw, P{s.x, s.y, s.z});
    return Shift{WrapPi(-s.yaw), -o.x, -o.y, -o.z, s.known};
}

inline bool IsIdentity(const Shift& s) {
    return s.known && std::abs(WrapPi(s.yaw)) < 1e-6f && std::abs(s.x) < 1e-6f &&
           std::abs(s.y) < 1e-6f && std::abs(s.z) < 1e-6f;
}

// The game heading of tracking -Z after the shift, so that every view yaw (base - head yaw) stays
// what it was: a head yaw drops by exactly `yaw` in the new space.
inline float BaseYawAfter(float base_yaw, const Shift& s) {
    return s.known ? WrapPi(base_yaw - s.yaw) : base_yaw;
}

// The last N changes of the tracking space by serial (space `serial` is `shift` in space
// serial - 1; space 0 is the first one), so anything placed in a recent space (a head sample, a
// camera record's frame still in flight, a held hand) can be moved into a later one. One owner
// (the caller's lock).
template <std::uint32_t N>
struct ShiftLog {
    struct Entry {
        std::uint32_t serial{0};
        Shift shift{};
    };
    std::array<Entry, N> entries{};

    void Add(std::uint32_t serial, const Shift& shift) {
        entries[serial % N] = {serial, shift};
    }

    // The shift from space `from` to the later space `to` (identity when they are the same).
    // False (and an unknown shift) when one of the changes between them is unknown or no longer
    // in the log, or `to` is not later.
    bool Between(std::uint32_t from, std::uint32_t to, Shift& out) const {
        out = Shift{};
        const std::int32_t steps = static_cast<std::int32_t>(to - from); // wrap-safe
        if (steps == 0) {
            return true;
        }
        if (steps < 0 || steps > static_cast<std::int32_t>(N)) {
            out.known = false;
            return false;
        }
        for (std::uint32_t s = from + 1; s != to + 1; ++s) {
            const Entry& e = entries[s % N];
            if (e.serial != s) {
                out.known = false;
                return false;
            }
            out = Compose(out, e.shift);
        }
        return out.known;
    }
};

// The recenter's fade (presenter frame thread, one owner): the view dims to black over OutS, the
// space changes at black (Step returns true once), it stays black until frames placed in the new
// space are on display (the caller says so; at most MaxBlackS) and comes back over InS. A Start
// during the fade-in goes out again from the brightness it had; during the fade-out or at black it
// changes nothing (the running recenter takes it).
struct Fade {
    enum class Phase : unsigned char { Idle, Out, Black, In };
    static constexpr float OutS = 0.06f;
    static constexpr float InS = 0.09f;
    static constexpr float MaxBlackS = 0.6f;

    Phase phase{Phase::Idle};
    float t{0.0f}; // seconds in the phase

    // False when a recenter is already fading out or black (it takes this one too).
    bool Start() {
        if (phase == Phase::Out || phase == Phase::Black) {
            return false;
        }
        t = phase == Phase::In ? OutS * (1.0f - std::clamp(t / InS, 0.0f, 1.0f)) : 0.0f;
        phase = Phase::Out;
        return true;
    }

    // The space changed some other way (the runtime's own recenter): straight to black from any
    // phase, without a change of our own (a fade-out under way gives its change up), and back in
    // once the new space's frames are shown, like ours.
    void CutToBlack() {
        phase = Phase::Black;
        t = 0.0f;
    }

    // Advances by `dt` seconds. True exactly once, when the fade-out reaches black: change the
    // space now. `new_frames_shown`: the frames on display are placed in the new space (or there
    // is nothing old to hide), checked while black.
    bool Step(float dt, bool new_frames_shown) {
        dt = std::isfinite(dt) ? std::max(dt, 0.0f) : 0.0f;
        switch (phase) {
        case Phase::Idle:
            return false;
        case Phase::Out:
            t += dt;
            if (t >= OutS) {
                phase = Phase::Black;
                t = 0.0f;
                return true;
            }
            return false;
        case Phase::Black:
            t += dt;
            if (new_frames_shown || t >= MaxBlackS) {
                phase = Phase::In;
                t = 0.0f;
            }
            return false;
        case Phase::In:
            t += dt;
            if (t >= InS) {
                phase = Phase::Idle;
                t = 0.0f;
            }
            return false;
        }
        return false;
    }

    // Brightness of every submitted layer, 0..1.
    float Brightness() const {
        switch (phase) {
        case Phase::Out:
            return std::clamp(1.0f - t / OutS, 0.0f, 1.0f);
        case Phase::Black:
            return 0.0f;
        case Phase::In:
            return std::clamp(t / InS, 0.0f, 1.0f);
        default:
            return 1.0f;
        }
    }
};

} // namespace VR::Space
