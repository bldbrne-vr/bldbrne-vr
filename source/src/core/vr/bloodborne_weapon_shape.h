// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <vector>

#include "common/types.h"
#include "core/vr/weapon_fit.h"
#include "core/vr/weapon_mesh.h"

// Authored layouts in weapon_authored_data.h take priority for the reviewed forms.
// weapon_wall_shape=0 restores the single line without loading these shapes.
// The solid weapons' simple fitted shapes (three capsules normally, four for the Beast Cutter; weapon_simple.h)
// Built on the model reader and detailed fitting diagnostics below.
// The solid weapons' fitted shapes (vr.ini weapon_wall_shape=1, the default; core/vr/weapon_fit.h;
// what the other weapon meets, the walls meeting the line): the right weapon's model, read from
// the user's own game files (parts/wp_a_XXXX.partsbnd.dcx, the
// EquipParamWeapon row's equipModelId; never shipped), posed as the hand holds it (each part's
// live skeleton, captured on the game thread by bloodborne_melee.cpp WeaponShapeTick once the
// weapon and its form have settled), fitted on a worker thread ("VR:WeaponShape"), kept per
// weapon, form and parts for the session. Until a fit is ready, and for the whips, the body keeps
// the 0.7.0 line (bloodborne_body.cpp HandShape).
namespace VR::Bloodborne::WeaponShape {

// A cached shape still waits for one uninterrupted run of the same eligible form.
// Reset when eligibility is lost (whip, missing parts, disabled setting or character change).
struct SettleState {
    static constexpr s64 DelayNs = 1'800'000'000;

    void Reset() {
        key = 0;
        since = 0;
    }

    bool Ready(u64 eligible_key, s64 now) {
        if (eligible_key == 0) {
            Reset();
            return false;
        }
        if (eligible_key != key) {
            key = eligible_key;
            since = now;
        }
        return now - since >= DelayNs;
    }

private:
    u64 key{0};
    s64 since{0};
};

struct Part {
    int index{0};                    // the weapon's part (0..3: WP_A_XXXX[_k].flver)
    std::array<float, 12> to_grip{}; // its model space -> the grip's frame (3 x 4, [R | t])
    std::vector<VR::WeaponMesh::LiveBone> bones; // its skeleton as posed now (model space; none:
                                                 // a part without a pose of its own)
    std::vector<VR::WeaponMesh::Marker> markers; // its dummies where the game has them (grip)
};

struct Request {
    u64 key{0}; // weapon, form, hand and the attached parts
    s32 weapon{-1};
    s32 model{-1}; // EquipParamWeapon equipModelId
    s32 style{-1};
    bool form_b{false}; // explicit trick form; style and part index are not form identity
    bool left{false}; // paired blade; its own grip frame and cache key
    // The split blades (core/vr/dual_blades.h): also a model of the blade's steel as posed
    // (core/vr/blade_shine_mesh.h; the grip's frame), put for the in-frame renderer (GetShine):
    // what shines when the blade is honed. Made whether its capsules fit or not.
    bool shine{false};
    std::vector<Part> parts;
};

// Game thread: fit `request` on the worker (ignored when its key was asked for before).
void Ask(Request request);
// Whether `key` was asked for (fitted, waiting, or failed).
bool Asked(u64 key);
// `key`'s capsules (the grip's frame, game units): false while not fitted, or when it failed.
bool Get(u64 key, VR::WeaponFit::Fit& out);
// `key`'s shine (Request::shine): its model (VR::Assets::GetModel's id; 0: none could be made) and
// the blade's line in the grip's frame (the guard end, the tip). False while it is not done yet.
struct Shine {
    s32 model{0};
    std::array<float, 3> base{};
    std::array<float, 3> tip{};
};
bool GetShine(u64 key, Shine& out);

} // namespace VR::Bloodborne::WeaponShape
