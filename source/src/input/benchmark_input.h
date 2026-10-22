// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).
#pragma once

#include <cstdlib>
#include <SDL3/SDL_events.h>

namespace Input {
// Set only on private PERF SCAN children. Never blocks MockXR's startup actions.
inline bool BenchmarkInputLocked() {
    static const bool locked = [] {
        const char* value = std::getenv("BBVR_BENCHMARK_INPUT_LOCK");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return locked;
}

inline bool IsBenchmarkPlayerEvent(Uint32 type) {
    // Retain device hotplug, window lifecycle and quit events.
    return (type >= SDL_EVENT_KEY_DOWN && type <= SDL_EVENT_KEYMAP_CHANGED) ||
           (type >= SDL_EVENT_MOUSE_MOTION && type <= SDL_EVENT_MOUSE_WHEEL) ||
           (type >= SDL_EVENT_JOYSTICK_AXIS_MOTION && type <= SDL_EVENT_JOYSTICK_BUTTON_UP) ||
           (type >= SDL_EVENT_GAMEPAD_AXIS_MOTION && type <= SDL_EVENT_GAMEPAD_BUTTON_UP) ||
           (type >= SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN && type <= SDL_EVENT_GAMEPAD_SENSOR_UPDATE) ||
           (type >= SDL_EVENT_FINGER_DOWN && type <= SDL_EVENT_FINGER_CANCELED);
}
} // namespace Input
