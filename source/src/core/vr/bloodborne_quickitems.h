// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <array>
#include "common/types.h"

// Bloodborne (v1.09 eboot): quick items in VR, the game side (core/vr/quick_items.h has the
// design and the pure parts).
//
// Site A (the main player's update, core/vr/bloodborne_heal.cpp) reads the six quick-item slots of
// EquipGameData every update and publishes them (goods, count, icon, held effect, usable); it
// carries out the presenter's requests through the game's own functions:
//  - select: the one aligned store SelectNext makes (the selected inventory index) plus the three
//    HUD bytes the D-pad path sets, so the HUD shows the item in your hand;
//  - use: PlayerIns::UseGoods (vt+0x408, what the item animations' event 65 calls) for items whose
//    effect needs no animation;
//  - throw: UseGoods with the left palm's dummy poly (3): the game's own projectile, one item
//    consumed by the game. A hook at the end of the projectile launch (0x19F7835, every projectile
//    passes there) gives that one projectile your release velocity, and with throw_physics a
//    private copy of its BulletParam row with real gravity and no speed clamps.
// The render-camera hook (loop thread) places the arc above the hunter's left palm when it opens,
// decides which card the palm hovers and builds the in-frame draw list (core/vr/inframe.h) in
// that camera's own view.
// Every site and called function is signature-checked; anything missing turns that part off (Y
// stays Square, throws use the game's own throw).
namespace VR::Bloodborne {

// OnEbootLoaded (VR enabled): checks and installs. Logs one line.
void InstallQuickItems(VAddr base, u64 size);

// Site A, main player only: `may_use` = no other UseGoods ran in this update (the vial).
void QuickItemsSiteA(u8* player, bool may_use);

// Render-camera hook (loop thread), after the frame's camera is placed (`rows`: the render
// camera's right / up / forward rows, 4 floats each; `position`: its position).
void QuickItemsRenderTick(u64 record_id, const float* rows, const float* position);

// Installed (the slots can be read).
[[nodiscard]] bool QuickItemsReady();

// Existing hand-pose loop: bounded cosmetic pressure, zero for stale/put-away scenes.
[[nodiscard]] float CallBeyondSqueeze(s64 now_ns);
[[nodiscard]] float AugurSqueeze(s64 now_ns);
[[nodiscard]] float MessengerSqueeze(s64 now_ns);
[[nodiscard]] float GloveHandOpen(s64 now_ns);
[[nodiscard]] float BeastRoarScreenStrength(s64 now_ns);

// The thumb-facing eyepiece of the Monocular, in the current tracking space. Published from
// the same model transform that draws the held item, including the hand's render carry.
[[nodiscard]] bool MonocularEyepiece(u32 space_serial, s64 now_ns,
                                     std::array<float, 3>& position);
// The held item `goods`'s drawn centre (its model's bounds), the same way: the Blacksky Eye held
// to your eye is measured from it (core/vr/quick_items.h ToolGesture::Eye).
[[nodiscard]] bool HeldItemPoint(u32 space_serial, s64 now_ns, s32 goods,
                                 std::array<float, 3>& position);
// Raw protector ID in the player's arm slot, or -1 when unavailable.
[[nodiscard]] s32 EquippedArmArmor(const u8* player);

// The held Blacksky Eye as last drawn: its centre (game world) and radius (world units), its
// pupil turned onto the shot's line (core/vr/blacksky_eye.h): where its meteor leaves.
[[nodiscard]] bool BlackskyPupil(s64 now_ns, std::array<float, 3>& centre, float& radius);

// quick_use_sounds: the game's sound of the item `goods` used at once with goodsUseAnim
// `use_anim` (core/vr/quick_items.h UseSoundsOf; the Blood Vial's injection: 26), played by the
// render ticks that follow. Site A (the player's update).
void QueueQuickUseSound(s32 goods, s32 use_anim);

// molotov_burst: the hunter's gun fired from `start` (world) along the unit `ray` (the emitter,
// core/vr/bloodborne_gun.cpp, the first bullet of a shot). The next render tick bursts a Molotov
// of his in flight within molotov_burst_deg of that ray.
void QuickItemsGunShot(const float* start, const float* ray);

// The Hand Lantern in the hunter's hand carries a light (core/vr/hand_lantern.h Carry; the hip's
// lantern is out meanwhile): core/vr/bloodborne_held.cpp lights the palm. Site A's view.
[[nodiscard]] bool LanternLightInHand();

// ... or was, less than a second ago: the game's lantern model on the palm stays undrawn while
// its effect fades out (core/vr/hand_lantern.h; the renderer's skip).
[[nodiscard]] bool LanternModelHidden();

// ... and the light in the hand went out within the last HandLantern::GlowAloneNs: its effect's
// last frame may still draw the glow card without the model (the renderer's skip). Reads the
// clock: asked only for such a card.
[[nodiscard]] bool LanternJustPutOut();

} // namespace VR::Bloodborne
