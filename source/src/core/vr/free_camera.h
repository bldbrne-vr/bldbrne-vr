// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <cmath>
#include <atomic>
#include <cstdint>

namespace VR::FreeCamera {
struct Point {
    float x{}, y{}, z{};
};
struct Input {
    float right{}, forward{}, up{};
    int speed{}; // speed buttons held (the right trigger, a grip, the left stick pressed): 0, 1, 2+
    bool home{};
};
// m/s at the game's scale: no speed button; one (fast); two at once (faster, to cross an area;
// 2026-10-30, the user).
inline constexpr float FlySpeed = 1.5f, FastSpeed = 6.0f, FasterSpeed = 18.0f;
inline float SpeedOf(int speed) {
    return speed >= 2 ? FasterSpeed : speed == 1 ? FastSpeed : FlySpeed;
}
// Y in the free camera (the presenter): a tap is back to the hunter, on its release. Where Y does
// Menu's jobs (y_menu: SteamVR, whose dashboard owns Menu) holding it is the recenter, as Menu's
// hold (core/vr/y_menu.h, 0.7 s), and that press is not a way back; elsewhere every release is.
class YButton {
public:
    enum class Event : std::uint8_t { None, Back, Recenter };
    static constexpr float HoldS = 0.7f; // Menu's hold (VR::YMenu::Gesture::hold_s)
    Event Update(bool down, float dt, bool y_menu) {
        if (down) {
            if (!held) {
                held = true;
                fired = false;
                time = 0.0f;
            }
            time += dt;
            if (y_menu && !fired && time >= HoldS) {
                fired = true;
                return Event::Recenter;
            }
            return Event::None;
        }
        if (!held)
            return Event::None;
        held = false;
        return fired ? Event::None : Event::Back;
    }
    // Free camera not taking Y this frame (off, a menu, a screen): a press it never saw end is
    // forgotten, so it cannot end the next free camera on its first frame.
    void Reset() {
        held = fired = false;
        time = 0.0f;
    }

private:
    bool held{false}, fired{false};
    float time{0.0f};
};
// When a free camera turned on in the game ends: at the first load after it went on (a death, a
// travel, a quit). The launcher's whole-run one (VR_FREE_CAMERA=1) is never armed. Set from the XR
// thread (the page, Y), read on the game camera thread.
class LoadEnd {
public:
    void Set(bool on, std::uint64_t load_serial) {
        at.store(load_serial, std::memory_order_relaxed);
        armed.store(on, std::memory_order_release);
    }
    bool Ends(std::uint64_t load_serial) const {
        return armed.load(std::memory_order_acquire) &&
               load_serial != at.load(std::memory_order_relaxed);
    }

private:
    std::atomic<bool> armed{false};
    std::atomic<std::uint64_t> at{0};
};
// Non-aggro with the free camera (bloodborne_camera.cpp writes the byte): the byte is 1 while the
// free camera is on, and goes back to what it was when it goes off (the launcher's non-aggro, set
// at load, stays). A byte that is neither 0 nor 1 is not the flag this expects: nothing is written
// (Refused, once).
class StealthSwitch {
public:
    enum class Step : std::uint8_t { None, On, Back, Refused };
    // `current`: the byte now; `write`: what to store for On / Back.
    Step Update(bool free_camera, std::uint8_t current, std::uint8_t& write) {
        if (free_camera == on)
            return Step::None;
        if (free_camera) {
            if (current > 1) {
                if (refused)
                    return Step::None;
                refused = true;
                return Step::Refused;
            }
            on = true;
            before = current;
            write = 1;
            return Step::On;
        }
        on = false;
        write = before;
        return Step::Back;
    }
    std::uint8_t Before() const {
        return before;
    }

private:
    bool on{false}, refused{false};
    std::uint8_t before{0};
};
inline float Axis(float v) {
    if (!std::isfinite(v))
        return 0;
    const float a = std::clamp(std::abs(v), 0.0f, 1.0f);
    return a <= .2f ? 0.0f : std::copysign((a - .2f) / .8f, v);
}
// Game-camera-thread state. Head motion and stereo separation are added by the caller,
// never integrated into this shared eye-pair position. No player/world writes.
class Controller {
public:
    void Reset() {
        ready = false;
    }
    Point Step(Point origin, Point anchor, float yaw, float scale, double now, std::uint64_t load,
               bool first_eye, Input input, double input_time) {
        const float dx = anchor.x - last_anchor.x, dy = anchor.y - last_anchor.y,
                    dz = anchor.z - last_anchor.z;
        const bool reset = !ready || load != last_load || now < last_time ||
                           now - last_time > 1.0 || dx * dx + dy * dy + dz * dz > 9 * scale * scale;
        if (reset) {
            position = origin;
            last_time = now;
            ready = true;
        }
        last_anchor = anchor;
        last_load = load;
        if (!first_eye && !reset)
            return position;
        const float dt = static_cast<float>(std::clamp(now - last_time, 0.0, .05));
        last_time = now;
        // Missing/frozen XR input must never keep flying.
        if (now < input_time || now - input_time > .15)
            input = {};
        if (input.home) {
            position = origin;
            return position;
        }
        float r = Axis(input.right), f = Axis(input.forward), u = Axis(input.up);
        const float length = std::sqrt(r * r + f * f + u * u);
        if (length > 1) {
            r /= length;
            f /= length;
            u /= length;
        }
        const float step = dt * scale * SpeedOf(input.speed);
        position.x += (std::cos(yaw) * r + std::sin(yaw) * f) * step;
        position.y += u * step;
        position.z += (-std::sin(yaw) * r + std::cos(yaw) * f) * step;
        return position;
    }

private:
    Point position{}, last_anchor{};
    double last_time{};
    std::uint64_t last_load{};
    bool ready{};
};
} // namespace VR::FreeCamera
