// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: AGPL-3.0-or-later
// Copyright (C) 2026 BB-VR contributors. Part of Bldbrne-VR (https://github.com/bldbrne-vr/bldbrne-vr).
// Licensed under the GNU Affero General Public License v3.0 or later (LICENSES/AGPL-3.0-or-later.txt).
// You may copy, modify and redistribute this file, in source or compiled form, ONLY under that licence:
// keep every copyright notice, credit Bldbrne-VR, and publish your complete source under the same
// licence. Any other use is copyright infringement and terminates your rights under it (AGPL-3.0 s. 8).

#pragma once

#include <iosfwd>
#include <string>

// The weapon-only transform (core/vr/transform.h, core/vr/bloodborne_transform.h): its vr.ini keys.
// Parsed by VR::LoadSettings, which hands every key it does not know to ParseTransformSetting, and
// written with the other defaults when vr.ini is created. Read-only after loading; `transform` is
// decided at install (a restart changes it).
namespace VR::Transform {

struct TransformSettings {
    // native: the right grip transforms only the weapon (its own clip in the hand, the game's form
    // switch; no L1, no body animation); vanilla: the right grip holds L1 (the game's transform
    // animation on the whole hunter). Weapons without a native clip keep L1 either way.
    bool native{true};
    // Trick weapons whose native transform has not been seen working yet (core/vr/transform_clips.h
    // Untested) transform natively too; 0 = they keep L1.
    bool untested{true};
    float speed{1.0f}; // the clip's speed (1 = the game's timing)
    // Seconds of the clip's lead-in skipped; < 0 (auto): its measured still part (in vanilla the
    // hunter's own wind-up), so the weapon moves at the squeeze.
    float skip_s{-1.0f};
    // The retract (form B -> A): its start, up to the weapon's form switch, plays this much faster
    // (nothing skipped: the weapon moves through the whole clip and its sounds fire in order, just
    // sooner), the rest at transform_speed. 1.75 brings the Saw Cleaver's switch from 0.47 s to
    // 0.27 s, as soon as its extend's; 1 = the game's timing.
    float retract_speed{1.75f};
    bool cues{true};    // the clip's own sounds and effects at the weapon
    bool haptics{true}; // the latch tick at the squeeze, a detent per mechanical sound, the lock
    // A squeeze the game cannot start at once (a hit, an item, the transform before still
    // locking) waits this long. A dodge never holds it up: the weapon transforms through the roll.
    float buffer_ms{400.0f};
    // A squeeze during a swing or with the weapon hand moving transforms at once, as a flick: the
    // weapon snaps into its new form mid-stroke (core/vr/transform.h FlickClock). 0 = such a
    // squeeze is ignored (a reflex clench at the hit never transforms).
    bool flick{true};
    float flick_speed{2.5f};     // a flick's clip speed at least (clip seconds per second)
    float flick_lead_ms{100.0f}; // ... faster where the form would switch later than this
    // A flick during a physical swing makes the rest of that stroke the game's own transform
    // attack (the trick attack's row: its damage, poise and hit on the new form's blade); 0 = the
    // new form's own light / heavy row.
    bool attack{true};
};

TransformSettings& GetTransformSettings();

// VR::LoadSettings: true if `key` is one of ours (and it was applied).
bool ParseTransformSetting(const std::string& key, const std::string& value);
// The defaults block written into a new vr.ini.
void WriteTransformDefaults(std::ostream& out);
// "transform=native transform_untested=1 ..." for the install line.
std::string TransformSettingsText();

} // namespace VR::Transform
