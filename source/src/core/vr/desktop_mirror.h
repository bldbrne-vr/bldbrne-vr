// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <algorithm>
#include <string_view>

#include "common/types.h"

// The desktop window while the headset shows the game (vr.ini desktop_mirror,
// desktop_mirror_view; Presenter::Present). The window stays open and live, but costs as little
// as possible: it shows one eye the headset is given (the RIGHT one by default; the frame's DIBR
// eye, already made), centre-cropped to the window and scaled with one blit, and is presented only
// every Nth frame
// that went to the headset. Frames that did not go to the headset (mono, cinematics, held menu
// frames, a pause) present exactly as before. Pure helpers here (unit-tested).
namespace VR::DesktopMirror {

// vr.ini desktop_mirror_view (game | left | right).
enum class View : u8 {
    Game = 0,  // the flat game frame with its HUD (the window before 2026-10-09)
    Left = 1,  // the left eye as the headset gets it (without the HUD and menus: their own layers)
    Right = 2, // the right eye, the same way (the default)
};

// The eye a view shows (0 left, 1 right); Game shows none.
[[nodiscard]] constexpr u32 EyeOf(View view) {
    return view == View::Left ? 0u : 1u;
}

// vr.ini's word (or VR_TUNE mirror.view's number) for a view: game / 0, left / 1, right / 2 (and
// anything else: the default, right).
[[nodiscard]] constexpr View ParseView(std::string_view v) {
    if (v == "game" || v == "0") {
        return View::Game;
    }
    if (v == "left" || v == "1") {
        return View::Left;
    }
    return View::Right;
}
[[nodiscard]] constexpr std::string_view ViewName(View v) {
    return v == View::Game ? "game" : v == View::Left ? "left" : "right";
}
[[nodiscard]] constexpr View ViewFromNumber(float v) {
    return v < 0.5f ? View::Game : v < 1.5f ? View::Left : View::Right;
}

// vr.ini desktop_mirror: is the n-th frame published to the headset (n counted from 1) also
// presented on the desktop? 1 = every frame, N > 1 = every Nth (2 = 30 Hz at the game's 60),
// 0 (or less) = never while the headset shows the game.
[[nodiscard]] constexpr bool Presents(s32 every, u64 n) {
    if (every == 1) {
        return true;
    }
    if (every <= 0) {
        return false;
    }
    return n % static_cast<u64>(every) == 0;
}

// The present queue's priority hint (Instance: {graphics, present}). Anything but the window of
// before 2026-10-09 (every frame, the game view) gives the desktop present the lowest priority.
[[nodiscard]] constexpr bool LowPresentPriority(s32 every, View view) {
    return every != 1 || view != View::Game;
}

struct Rect {
    s32 x{0};
    s32 y{0};
    u32 width{0};
    u32 height{0};
    constexpr bool operator==(const Rect&) const = default;
};

// The largest rectangle of a src_w x src_h image with the aspect ratio of dst_w x dst_h, centred
// (what the window shows of the eye: no bars, no stretch). The whole image when a size is zero.
[[nodiscard]] constexpr Rect CentreCrop(u32 src_w, u32 src_h, u32 dst_w, u32 dst_h) {
    if (src_w == 0 || src_h == 0 || dst_w == 0 || dst_h == 0) {
        return {0, 0, src_w, src_h};
    }
    const u64 sw = src_w;
    const u64 sh = src_h;
    const u64 dw = dst_w;
    const u64 dh = dst_h;
    if (sw * dh > dw * sh) {
        // The image is wider than the window: its full height, the middle of its width.
        const u32 w = static_cast<u32>(std::clamp<u64>((sh * dw + dh / 2) / dh, 1, sw));
        return {static_cast<s32>((src_w - w) / 2), 0, w, src_h};
    }
    // Taller (or the same shape): its full width, the middle of its height.
    const u32 h = static_cast<u32>(std::clamp<u64>((sw * dh + dw / 2) / dw, 1, sh));
    return {0, static_cast<s32>((src_h - h) / 2), src_w, h};
}

// The byte-order trick of the eye blit. The eye images are R8G8B8A8_UNORM images holding the
// headset swapchain's bytes (BGRA when it is B8G8R8A8_SRGB). A blit converts by channel name, a
// copy keeps the bytes. So the eye is blitted (scaled) into an intermediate image and that is
// copied into the window's swapchain image: declare the intermediate RGBA when the eye's bytes
// and the window's format have the same order (the bytes go through unchanged), BGRA when they
// differ (the blit swaps R and B once). True: the intermediate is B8G8R8A8.
[[nodiscard]] constexpr bool IntermediateBgra(bool eye_bytes_bgra, bool window_bgra) {
    return eye_bytes_bgra != window_bgra;
}

} // namespace VR::DesktopMirror
