// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <array>
#include <optional>

#include "common/types.h"

// The sun's or moon's shadow atlas held over frames (on, one frame in two; VR_SHADOW_HOLD=N for
// another count, 0: off; live key lighting.shadow_hold).
//
// The game draws its four cascades into the atlas every frame, and every frame fits them anew to
// the camera. Where what casts never moves (the Forbidden Woods' trees and rocks, which only cast
// at all with VR_WOODS_SHADOWS: about 440 draws and half a millisecond of GPU a frame), most of
// that is the same picture again. So: the atlas is drawn one frame in N, and left as it is in the
// frames between (every draw into it left out, its clear too). The picture then belongs to the
// camera of the frame it was drawn in, and the game's shadow pass asks it with this frame's
// matrices; those are replaced by the held picture's own, brought to this frame's camera:
//
//   the game's (cascades336): M_k = S_k inverse(V) inverse(P)      (screen -> shadow, per cascade)
//   kept at the drawn frame:  W_k = M_k P V                        (world -> shadow)
//   a held frame's:           M'_k = W_k inverse(V_now) inverse(P_now)
//
// V is the scene camera's view (its 864-byte block's rows 8..19; the cascades' view space is the
// same one: checked against the light's own view rows, 2026-11-15). Exact for everything that
// stands still; what moves (the hunter, enemies) casts where it was when the atlas was drawn, at
// most N - 1 frames ago.
// The held picture was fitted to the old camera: the further the camera has gone since, the more
// of this frame's view lies outside it. The atlas is drawn again as soon as a cascade's window
// has moved by more than a small share of its width (Limit), at every artificial turn, and when
// the light itself is another one.
//
// All of it on Stage A (the draw producer), in the game's own order: the draws left out
// (Rasterizer::ShadowCellSkipped), the camera and the matrices at PrepareConstants. Stage B, a
// frame behind, asks by the raw bytes (Lookup) for the same replaced matrices: the moon's light in
// the air and the fog grid read the atlas too.
namespace Vulkan::ShadowHold {

/// On unless VR_SHADOW_HOLD is 0 or 1 (read once).
[[nodiscard]] bool On();

// Steady cascades (2026-11-15; the user in the headset: "moving my head makes the game's shadows
// jump around"). The game fits every cascade's window to the camera anew each frame, anywhere:
// the window slides by fractions of a shadow texel and its size breathes with the view, so every
// frame the casters land on other texels and each shadow's edge crawls (leaves' shadows boil).
// On a screen that shows while the camera moves; a head never stands still.
// So each cascade is drawn a little moved and a little smaller inside its tile: its size brought
// down to the next step of 2% (texels a metre: the same for as long as the fit stays within that
// step), and then moved by less than a texel so that one fixed place of the world near the camera
// (the anchor: a corner of a 16 m grid) lands on a whole texel. The same texels then cover the same
// ground frame after frame, whatever the head does. The matrices the shadows are read with get
// the same change (Cascades), so every shadow stays where it is: only its edge stops crawling.
// Costs nothing: a viewport's numbers. Found per atlas tile from the light's own camera block
// (orthographic, 864 bytes) and the draw's viewport; which tile is which cascade, and which way
// up the atlas is read, is checked against the cascades' own matrices before anything is moved
// (and again every frame: three frames that disagree switch it off until they agree again).
// VR_SHADOW_SNAP=0 (tests) or the live key lighting.shadow_snap=0: as the game fits them.
/// The hooks below are to be called (the hold or the steady cascades are on).
[[nodiscard]] bool Active();
/// Stage A, every regular draw: not into a shadow atlas' tile (until CellDraw says so).
void NoCell();
/// Stage A, a draw into a shadow atlas that is drawn: its cell of 4 x 4, its viewport (x offset,
/// x scale, y offset, y scale; texels) and the atlas' side.
void CellDraw(u32 cell, const std::array<float, 4>& viewport, u32 side);
/// Stage A, after the draw's constants were prepared: how its viewport is to be moved (x, y;
/// texels) and scaled about its centre (x, y), or nothing.
[[nodiscard]] std::optional<std::array<float, 4>> Snap();

/// The nearest cascade (the atlas' cell 1,1 of 4 x 4: its top left quarter) is never held: what
/// moves close by (the hunter's own body, whoever he fights) casts there, and a shadow a frame
/// late on a body that follows the head jittered (the user in the headset, 2026-11-15: "my own
/// shadow was a little glitchy while moving my head ... apply the shadow cache for non actors
/// only"). On a held frame its draws are drawn, with the game's own matrices. The game's clear is
/// left out on a held frame (it only marks the whole atlas as cleared: the next pass would wipe
/// the held cascades too); the first draw into the nearest cascade clears that quarter itself.
///
/// Since 2026-11-18 the second cascade (cell 3,1: the top right quarter, 2 to 8 m ahead) is drawn
/// every frame as well, the user's call: with it held, a carriage's shadow a few metres from the
/// hunter blinked and jumped at the Great Bridge. Only the two far cascades are held; on a held
/// frame the first draw into either near cascade clears the atlas' top half.
inline constexpr u32 NearCell = 5, NearCell2 = 7;
inline constexpr u32 NearCascades = 2;
enum class Verdict { Draw, Skip, NearClear };

/// Stage A, at a draw into a shadow atlas. `whole_atlas`: this draw covers the whole atlas (the
/// game makes one or more a frame, after some or all of its cascades; it draws nothing);
/// otherwise `cell` is the draw's cell of 4 x 4. A frame starts where the cascades' round begins
/// again: a draw into a cell that a whole-atlas draw came after. Skip: left out (the atlas is
/// being held); NearClear: a held frame's draw into a near cascade: drawn, the first of them
/// after a clear of the near cascades' part of the atlas.
[[nodiscard]] Verdict SkipDraw(bool whole_atlas, u32 cell);

/// Stage A: a scene camera block (864 bytes: 216 floats).
void Camera(const float* scene);
/// Stage A: the same block, seen while a draw into an atlas tile is prepared (the light's camera).
void LightCamera(const float* scene);

/// Stage A: a 336-byte lighting buffer. True: `out` (84 floats) holds what to use in its place.
[[nodiscard]] bool Cascades(const float* raw, float* out);

/// Any stage: `values` (84 floats) replaced by what Cascades made of the same raw bytes.
bool Lookup(float* values);
/// The held frame this draw belongs to (Stage A, after SkipDraw gave Verdict::NearClear): the
/// recording side clears the near cascades' part of the atlas once per such frame.
u32 HeldFrame();
/// How many of the nearest cascades are drawn every frame (their quarters of the top half).
u32 NearTiles();

} // namespace Vulkan::ShadowHold
