// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

// Transitional host include; the canonical interaction implementation belongs to the DLL.
#include "common/types.h"
#include <iosfwd>
#include <string>
#include "physical_interactions/private/core/vr/body_settings.h"

// Host-owned persistence, mutable storage and live presentation settings.
namespace VR::Body {
[[nodiscard]] BodySettings& GetBodySettings();
[[nodiscard]] const char* BodyModeName(BodyMode mode);

// The same for this frame: vr.ini, VR_HEAD_SHADOW (read once) and VR_TUNE_FILE body.head_shadow
// (live A/B tests).
[[nodiscard]] bool HeadHiddenByRenderer();

// The same for this frame (vr.ini, the VR page, VR_TUNE_FILE body.mode / body.arms_draw).
[[nodiscard]] DrawnArms DrawnArmsNow();

// body_mode as it applies this frame: vr.ini / the VR page, or VR_TUNE_FILE body.mode (0 =
// no_head, 1 = arms_only, 2 = full, 3 = hands_only; live A/B tests).
[[nodiscard]] BodyMode EffectiveBodyMode();

// The grip of the global weapon_grip_pitch_deg / _yaw_deg / _roll_deg keys (no offset).
[[nodiscard]] GripTrim DefaultWeaponGrip();
// The grip of a weapon family (core/vr/bb_weapons.h: EquipParamWeapon id / 100000): its
// weapon_grip_<weapon> line, else DefaultWeaponGrip(). `custom` (optional) says which. Any thread.
[[nodiscard]] GripTrim WeaponGrip(s32 family, bool* custom = nullptr);
// Sets a family's grip (nullptr: back to the default) and saves it to vr.ini as
// weapon_grip_<weapon>=pitch,yaw,roll,x_cm,y_cm,z_cm (or =default). Presenter thread.
void SetWeaponGrip(s32 family, const GripTrim* trim);
// Changes with every SetWeaponGrip, so readers know to fetch their grip again.
[[nodiscard]] u32 WeaponGripGeneration();
// The vr.ini key of a family: weapon_grip_saw_cleaver, or weapon_grip_<family> for one without a
// name.
[[nodiscard]] std::string WeaponGripKey(s32 family);
[[nodiscard]] std::string WeaponName(s32 family);

// One vr.ini line. False when `key` is not a body setting. Out-of-range values are clamped and
// logged; std::stof errors propagate (the caller logs the bad line).
bool ParseBodySetting(const std::string& key, const std::string& value);

// The body section of a fresh vr.ini.
void WriteBodyDefaults(std::ostream& out);

} // namespace VR::Body
