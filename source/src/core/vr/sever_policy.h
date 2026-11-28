// SPDX-FileCopyrightText: 2026 BB-VR contributors
// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <span>
#include <string_view>

#include "core/vr/sever_mesh.h"

// Dismemberment rules. Pure: no guest
// memory, no allocation. Which hit severs, which limb, and where its cut goes:
//  - Beast Claw heavies/charged hits in either form, lights in form B; Burial Blade in scythe
//    form only, with a lower chance for lights (including light-judged wind-ups). Never a
//    transform attack, and only a kill as the instant ragdoll counts
//    one (core/vr/ragdoll_kill.h ClassifyBlow == Kill: the hunter's right-hand weapon, lethal,
//    the target not in a visceral). A corpse is cut once;
//  - the limb is the struck collision body's (its ragdoll body name, core/vr/headshot.h
//    Resolve): the head (head or neck: cut through the neck), an arm or a leg of a known side.
//    Torso/pelvis: in half, subject to MaxHalfKg. Clavicles, tails and unnamed bodies: no cut.
//    A creature's own names (2026-11-16: six legs a side, "Ragdoll_L_leg3_002"; a head hung off
//    the body, no neck) are cut by the struck body's own bone (Site::key, FindChain's fallback);
//  - the cut crosses the limb's bone between its joints (never at one, where skin weights
//    blend and the contour gets ragged), at the contact, its plane square to the bone and
//    turned toward the claw's own slash (its edge swept along its motion) by up to the site's
//    tilt: the cut follows your swing, the stump stays a clean convex section (sever_mesh.h).
//    A short thick bone tilts less (the neck: 12 degrees; at 30 a neck's cut would leave its
//    span and catch the shoulders or the jaw, and the cutter would refuse it), and given the
//    limb's thickness the tilt never lets the cut reach a joint.
namespace VR::Sever {

inline constexpr std::int32_t BeastClawVariation = 900;
inline constexpr std::int32_t BurialBladeVariation = 510;
// The Chikage's quick draw out of its scabbard (core/vr/chikage.h), either form: a kill cuts
// ChikageDrawChance of the time (2026-11-20: "if it kills an enemy if it has a 50%
// chance to dismember"; 2026-11-26: "can we up its dismemberment for the sheath attack change to
// 58%?"). Its other swings never cut.
inline constexpr std::int32_t ChikageVariation = 200;
inline constexpr float ChikageDrawChance = 0.58f;

// The hit policy: the folded Burial Blade never cuts; the Chikage only with its quick draw.
constexpr bool CanSever(std::int32_t variation, bool form_b, bool chikage_draw = false) {
    return variation == BeastClawVariation || (variation == BurialBladeVariation && form_b) ||
           (variation == ChikageVariation && chikage_draw);
}

// The equipment wake-up: the weapon in either form. The Burial Blade folds and unfolds all fight
// long; waking by form stopped the worker and freed what it read at every fold, so the next cut
// read its model from disk again and the game thread started a new thread at every unfold (the
// review, 2026-11-16). Each hit still checks CanSever.
constexpr bool CanWake(std::int32_t variation) {
    return variation == BeastClawVariation || variation == BurialBladeVariation ||
           variation == ChikageVariation;
}
inline constexpr float MaxTiltDeg = 30.0f;  // a long bone's (upper arm, forearm, thigh, calf)
inline constexpr float NeckTiltDeg = 12.0f; // the neck: short and thick
inline constexpr float EndTiltDeg = 20.0f;  // the wrist and the ankle: near the joint
inline constexpr float MinBoneM = 0.02f;    // a shorter bone (or none): no cut
// Cut in half only up to this weight (its hit capsules as flesh, the live adapter's weighing; the
// user, 2026-11-13: 180 kg). A heavier enemy's torso blow takes its head or the limb nearest the
// blow instead: every enemy loses something.
inline constexpr float MaxHalfKg = 180.0f;
// The share of the way to the nearer joint a tilted cut may reach at the limb's surface.
inline constexpr float JointClearance = 0.8f;

// Torso: cut in half at the waist (the upper body, arms and head with it, comes off the legs).
enum class Limb : std::uint8_t { None, Head, LeftArm, RightArm, LeftLeg, RightLeg, Torso };
// The bone a limb's cut crosses (the live adapter finds it by name in the enemy's skeleton;
// DetachedMask gives the detached piece's bones).
// Segment: a creature's limb bone by its own name (Site::key), "L_leg3_2" or "L_Arm_1".
enum class Bone : std::uint8_t { None, Neck, UpperArm, Forearm, Thigh, Calf, Spine, Segment };

// A struck body's own bone name, as a key (Detail::KeyOf): lower case, ' ' as '_', its ragdoll
// prefix and copy counter off ("Ragdoll_L_leg3_002" -> "l_leg3_2", "Ragdoll_head001" -> "head").
inline constexpr std::size_t KeyChars = 32;
using Key = std::array<char, KeyChars>;

struct Site {
    Limb limb{Limb::None};
    Bone bone{Bone::None};
    float t_min{0.0f}, t_max{0.0f}; // along the bone, its parent joint 0 -> its child joint 1
    float max_tilt_deg{0.0f};       // how far the slash may turn the cut off square
    // The struck body's own bone (FindChain's fallback where the skeleton has none of the names
    // the sites expect: a creature's). Empty: none known.
    Key key{};
    // The number its name had glued on (Detail::GluedNumber: "Neck003" 3), dropped from the key as
    // a copy counter ("Head001" is bone "Head") but a segment's for some (c5510's "Neck003" is bone
    // "Neck3", c1210's "L_Leg004" bone "L_Leg04"): tried after the key, exactly, as written and
    // zero-padded (KeyChain). -1: none.
    std::int16_t key_number{-1};
};

namespace Detail {
constexpr char Lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}
// ASCII case-insensitive substring.
constexpr bool Has(std::string_view name, std::string_view word) {
    if (word.empty() || word.size() > name.size()) {
        return false;
    }
    for (std::size_t i = 0; i + word.size() <= name.size(); ++i) {
        bool match = true;
        for (std::size_t j = 0; j < word.size() && match; ++j) {
            match = Lower(name[i + j]) == word[j];
        }
        if (match) {
            return true;
        }
    }
    return false;
}
// -1 left, 1 right, 0 unknown. Bloodborne's skeletons (the game files, 2026-11-12: c2000's)
// name sides as a word of its own: "L UpperArm", "R Calf", "Collidable_L UpperArm001",
// "Support_Knee_L", or glued to a capital: "LUpArmTwist"; also "L_Hand", "LeftHand".
constexpr int Side(std::string_view name) {
    bool left = Has(name, "left");
    bool right = Has(name, "right");
    for (std::size_t i = 0; i < name.size(); ++i) {
        const char c = name[i];
        const bool word = i == 0 || name[i - 1] == ' ' || name[i - 1] == '_';
        if (!word || (Lower(c) != 'l' && Lower(c) != 'r')) {
            continue;
        }
        const char next = i + 1 < name.size() ? name[i + 1] : '\0';
        const bool alone = next == '\0' || next == ' ' || next == '_';
        const bool glued = (c == 'L' || c == 'R') && next >= 'A' && next <= 'Z';
        if (alone || glued) {
            (Lower(c) == 'l' ? left : right) = true;
        }
    }
    return left == right ? 0 : left ? -1 : 1;
}
// Helper bones that are no limb's own segment (twists, supports, ends, collision proxies count
// only where asked for).
constexpr bool Helper(std::string_view name) {
    return Has(name, "twist") || Has(name, "support") || Has(name, "nub");
}
// A struck body's name as its bone's key (Key). A trailing number after '_' is a segment's, its
// zeros dropped ("_002" -> "_2"); one glued to a word is the ragdoll's copy counter, its last
// three digits dropped ("head001" -> "head"). Too long for a key: empty (no fallback).
constexpr Key KeyOf(std::string_view name) {
    if (name.size() >= 8 && Has(name.substr(0, 8), "ragdoll_")) {
        name.remove_prefix(8);
    }
    Key out{};
    if (name.empty() || name.size() >= KeyChars) {
        return out;
    }
    std::size_t n = 0;
    for (const char ch : name) {
        out[n++] = ch == ' ' ? '_' : Lower(ch);
    }
    std::size_t d = n;
    while (d > 0 && out[d - 1] >= '0' && out[d - 1] <= '9') {
        --d;
    }
    if (d < n && d > 0 && out[d - 1] == '_') {
        std::size_t z = d;
        while (z + 1 < n && out[z] == '0') {
            ++z;
        }
        std::size_t w = d;
        for (std::size_t r = z; r < n; ++r) {
            out[w++] = out[r];
        }
        for (std::size_t r = w; r < n; ++r) {
            out[r] = 0;
        }
    } else if (d < n && d > 0 && n - d >= 3) {
        out[n - 1] = out[n - 2] = out[n - 3] = 0;
    }
    return out;
}
// A bone's whole name as a key (lower case, ' ' as '_', nothing dropped: "Head001" stays
// "head001", where KeyOf reads a struck name's "001" as its counter). Too long: empty.
constexpr Key ExactKey(std::string_view name) {
    Key out{};
    if (name.empty() || name.size() >= KeyChars) {
        return out;
    }
    for (std::size_t i = 0; i < name.size(); ++i) {
        out[i] = name[i] == ' ' ? '_' : Lower(name[i]);
    }
    return out;
}
// The number glued to a struck body's name's last word, its zeros dropped ("Ragdoll_Neck003" 3,
// "Ragdoll_head001" 1, "Ragdoll_L_Leg000" 0); -1 none, or a segment's after '_' (KeyOf keeps that
// one in the key).
constexpr std::int16_t GluedNumber(std::string_view name) {
    std::size_t d = name.size();
    while (d > 0 && name[d - 1] >= '0' && name[d - 1] <= '9') {
        --d;
    }
    if (d == name.size() || d == 0 || name[d - 1] == '_' || name[d - 1] == ' ' ||
        name.size() - d < 3 || name.size() - d > 4) {
        return -1;
    }
    std::int16_t n = 0;
    for (std::size_t i = d; i < name.size(); ++i) {
        n = static_cast<std::int16_t>(n * 10 + (name[i] - '0'));
    }
    return n;
}
// How a bone's name meets a key: 2 the same, 1 a key ending in a segment's number then another
// digit ("l_arm4_1" meets "L_Arm4_11": a model numbering its segments 11, 12, 2; "l_arm" never
// meets "L_Arm4_Collarbone"), 0 not.
constexpr int KeyMatch(std::string_view bone, const Key& key, bool any_number = false) {
    std::size_t k = 0;
    while (k < KeyChars && key[k] != 0) {
        ++k;
    }
    if (k == 0 || bone.size() < k) {
        return 0;
    }
    for (std::size_t i = 0; i < k; ++i) {
        if ((bone[i] == ' ' ? '_' : Lower(bone[i])) != key[i]) {
            return 0;
        }
    }
    if (bone.size() == k) {
        return 2;
    }
    const bool numbered = any_number || (key[k - 1] >= '0' && key[k - 1] <= '9');
    return numbered && bone[k] >= '0' && bone[k] <= '9' ? 1 : 0;
}
// A chain of another limb (c1400's arms hung off its neck): not a helper, an ornament or a strand.
constexpr bool LimbChain(std::string_view name) {
    return Has(name, "arm") || Has(name, "leg") || Has(name, "wing");
}
} // namespace Detail

// The cut site of a struck ragdoll body, by its name. Unverified per enemy: the live adapter
// still checks the bone exists in that enemy's skeleton before anything is cut.
constexpr Site SiteOf(std::string_view body) {
    using Detail::Has;
    const Key key = Detail::KeyOf(body);
    const std::int16_t number = Detail::GluedNumber(body);
    const auto keyed = [&](Site s) {
        s.key = key;
        s.key_number = number;
        return s;
    };
    if (Has(body, "head") || (Has(body, "neck") && !Has(body, "necklace"))) {
        // Clear of the shoulders' and clavicles' skin at the neck's root (0) and of the jaw,
        // which hangs below the head's joint (1).
        return keyed({Limb::Head, Bone::Neck, 0.3f, 0.6f, NeckTiltDeg});
    }
    if (Has(body, "spine") || Has(body, "waist") || Has(body, "pelvis") || Has(body, "hips")) {
        // In half: the spine's lowest segment, clear of the pelvis' skin and of the chest's.
        return keyed({Limb::Torso, Bone::Spine, 0.3f, 0.7f, MaxTiltDeg});
    }
    if (Has(body, "clavicle") || Has(body, "collarbone") || Has(body, "shoulder") ||
        Has(body, "tail")) {
        return {};
    }
    const int side = Detail::Side(body);
    if (side == 0) {
        return {};
    }
    const Limb arm = side < 0 ? Limb::LeftArm : Limb::RightArm;
    const Limb leg = side < 0 ? Limb::LeftLeg : Limb::RightLeg;
    if (Has(body, "upperarm")) {
        return keyed({arm, Bone::UpperArm, 0.3f, 0.7f, MaxTiltDeg});
    }
    if (Has(body, "forearm")) {
        return keyed({arm, Bone::Forearm, 0.3f, 0.7f, MaxTiltDeg});
    }
    if (Has(body, "hand") || Has(body, "wrist") || Has(body, "finger")) {
        return keyed({arm, Bone::Forearm, 0.75f, 0.8f, EndTiltDeg}); // the wrist: the hand comes off
    }
    if (Has(body, "thigh")) {
        return keyed({leg, Bone::Thigh, 0.3f, 0.7f, MaxTiltDeg});
    }
    if (Has(body, "calf") || Has(body, "shin") || Has(body, "knee")) {
        return keyed({leg, Bone::Calf, 0.3f, 0.7f, MaxTiltDeg});
    }
    if (Has(body, "foot") || Has(body, "toe") || Has(body, "ankle")) {
        return keyed({leg, Bone::Calf, 0.75f, 0.8f, EndTiltDeg}); // the ankle
    }
    // A creature's own limb names ("L_leg3_002", "L_Leg_A_001", "L_Arm_002"): its struck
    // segment, by its bone (Site::key).
    if (Has(body, "leg")) {
        return keyed({leg, Bone::Segment, 0.3f, 0.7f, MaxTiltDeg});
    }
    if (Has(body, "arm")) {
        return keyed({arm, Bone::Segment, 0.3f, 0.7f, MaxTiltDeg});
    }
    return {};
}

// The facts of one hit, gathered by the damage detour (no reads here).
struct Hit {
    std::int32_t weapon_variation{-1}; // the swing's, captured when it opened
    bool swing{false};      // a physical swing's attack (bloodborne_melee.h MeleeHitInfoOf)
    bool heavy{false};      // its row: heavy or charged ...
    bool judged_light{false}; // ... not a wind-up the classifier judged light
    bool transform{false};  // not a flick's transform attack
    bool kill{false};       // ClassifyBlow == Kill
    float tip_mps{0.0f};    // its blade tip's peak speed (m/s; 0 unknown: the weakest's chance)
    float arc_deg{0.0f};    // how far its stroke swept the blade (deg; 0 unknown: as tip_mps)
    float roll{0.0f};       // a uniform draw in [0, 1): it cuts below its chance (0: it always does)
    bool form_b{false};     // the form captured when this attack opened
    Site site{};            // SiteOf(the struck body)
    bool supported{false};  // this enemy's skeleton and mesh are verified for cutting
    bool severed{false};    // this corpse was cut (or tried) before
    bool charged{false};    // a genuine charged attack, not a light-judged wind-up
    bool chikage_draw{false}; // the Chikage's quick draw (CanSever, ChikageDrawChance)
};

// A heavy or charged kill cuts by chance, the stronger the swing the surer (the user,
// 2026-11-13: a cut wants "a heavy/charged attack that is wide and fast. And is a killing blow
// always"; then "based on chance if the swing is weaker (like 15%, 25% etc, and make it be almost
// sure the stronger and faster it is)"). Its strength: the blade tip's peak speed (melee_window.h
// DamageConfig: an ordinary swing's 6 m/s, full speed 12) and how wide its stroke had swept when
// it struck (a heavy's whole stroke, the swing classifier's, is 140 deg; the mock's scripted
// heavy chop strikes at 83), each on a ramp, their geometric mean (both count: a fast stab or a
// slow wide push is weak). The chance runs from the weakest's to the strongest's, in a line.
// (Then "make it a little easier": the weakest 25% (15%), the strongest from 10 m/s and 90 deg
// (12, 110); an ordinary heavy about half (a third).)
inline constexpr float SeverTipLowMps = 5.0f;
inline constexpr float SeverTipHighMps = 10.0f;
inline constexpr float SeverArcLowDeg = 30.0f;
inline constexpr float SeverArcHighDeg = 90.0f;
inline constexpr float SeverChanceMin = 0.25f;
inline constexpr float SeverChanceMax = 0.98f;
// The transformed claw's lights cut too (the tester: "in transformed mode, make regular light
// attacks also trigger it (with some percentage over how strong the swing is)"), less often: the
// same strength, from 20% to 80% (the user, later: "up the chances", then "a tiny bit" more;
// 5% to 50% at first).
inline constexpr float SeverLightChanceMin = 0.2f;
inline constexpr float SeverLightChanceMax = 0.8f;
// The scythe, by the same strength (2026-11-16: "more fair, but I don't mind it
// dismembering lots"): the swing decides, as with the claw, and it still cuts a lot. A weak flick
// to a fast wide swing: lights 20% to 60%, heavies 50% to 98%, charged 75% to 99% (the first
// odds were 5-15%, 70-98%, 90-99%: a heavy hardly cared how it was swung, a light hardly ever cut).
inline constexpr float ScytheLightChanceMin = 0.20f, ScytheLightChanceMax = 0.60f;
inline constexpr float ScytheHeavyChanceMin = 0.50f, ScytheHeavyChanceMax = 0.98f;
inline constexpr float ScytheChargedChanceMin = 0.75f, ScytheChargedChanceMax = 0.99f;

// 0 (a weak swing, or unknown) .. 1 (fast and wide).
inline float SwingStrength(float tip_mps, float arc_deg) {
    const auto ramp = [](float v, float lo, float hi) {
        return std::isfinite(v) ? std::clamp((v - lo) / (hi - lo), 0.0f, 1.0f) : 0.0f;
    };
    return std::sqrt(ramp(tip_mps, SeverTipLowMps, SeverTipHighMps) *
                     ramp(arc_deg, SeverArcLowDeg, SeverArcHighDeg));
}

inline float SeverLightChance(float tip_mps, float arc_deg) {
    return SeverLightChanceMin +
           (SeverLightChanceMax - SeverLightChanceMin) * SwingStrength(tip_mps, arc_deg);
}

// A light hit (a light row, or a wind-up the classifier judged light).
inline bool LightHit(const Hit& h) {
    return !h.heavy || h.judged_light;
}

inline float SeverChance(float tip_mps, float arc_deg) {
    return SeverChanceMin + (SeverChanceMax - SeverChanceMin) * SwingStrength(tip_mps, arc_deg);
}

// This hit's chance: a heavy's or charged's, or (form B) a light's.
inline float HitChance(const Hit& h) {
    if (h.weapon_variation == ChikageVariation && h.chikage_draw) {
        return ChikageDrawChance;
    }
    if (h.weapon_variation == BurialBladeVariation) {
        const bool light = LightHit(h);
        const float lo = light ? ScytheLightChanceMin
                               : h.charged ? ScytheChargedChanceMin : ScytheHeavyChanceMin;
        const float hi = light ? ScytheLightChanceMax
                               : h.charged ? ScytheChargedChanceMax : ScytheHeavyChanceMax;
        return lo + (hi - lo) * SwingStrength(h.tip_mps, h.arc_deg);
    }
    return LightHit(h) ? SeverLightChance(h.tip_mps, h.arc_deg) : SeverChance(h.tip_mps, h.arc_deg);
}

enum class Verdict : std::uint8_t {
    Sever,
    NotWeapon,
    NotSwing,
    NotHeavy,
    NotKill,
    NoLimb,
    Unsupported,
    Severed,
    Spared, // its roll lost (SeverChance)
};

inline Verdict Decide(const Hit& h) {
    if (!CanSever(h.weapon_variation, h.form_b, h.chikage_draw)) {
        return Verdict::NotWeapon;
    }
    if (!h.swing) {
        return Verdict::NotSwing;
    }
    // A transform's own attack never cuts; lights only in transformed form.
    if (h.transform || (LightHit(h) && !h.form_b)) {
        return Verdict::NotHeavy;
    }
    if (!h.kill) {
        return Verdict::NotKill;
    }
    if (h.site.limb == Limb::None) {
        return Verdict::NoLimb;
    }
    if (!h.supported) {
        return Verdict::Unsupported;
    }
    if (h.severed) {
        return Verdict::Severed;
    }
    return h.roll < HitChance(h) ? Verdict::Sever : Verdict::Spared;
}

constexpr const char* VerdictName(Verdict v) {
    switch (v) {
    case Verdict::Sever:
        return "sever";
    case Verdict::NotWeapon:
        return "not a dismembering weapon/form";
    case Verdict::NotSwing:
        return "not a physical swing";
    case Verdict::NotHeavy:
        return "not a heavy or charged hit";
    case Verdict::NotKill:
        return "not a kill";
    case Verdict::NoLimb:
        return "no limb struck";
    case Verdict::Unsupported:
        return "enemy not verified for cutting";
    case Verdict::Severed:
        return "already cut";
    case Verdict::Spared:
        return "spared (the roll)";
    }
    return "?";
}

constexpr const char* LimbName(Limb l) {
    switch (l) {
    case Limb::None:
        return "none";
    case Limb::Head:
        return "head";
    case Limb::LeftArm:
        return "left arm";
    case Limb::RightArm:
        return "right arm";
    case Limb::LeftLeg:
        return "left leg";
    case Limb::RightLeg:
        return "right leg";
    case Limb::Torso:
        return "upper body";
    }
    return "?";
}

constexpr const char* BoneName(Bone b) {
    switch (b) {
    case Bone::None:
        return "none";
    case Bone::Neck:
        return "neck";
    case Bone::UpperArm:
        return "upper arm";
    case Bone::Forearm:
        return "forearm";
    case Bone::Thigh:
        return "thigh";
    case Bone::Calf:
        return "calf";
    case Bone::Spine:
        return "waist";
    case Bone::Segment:
        return "limb";
    }
    return "?";
}

// What a cut goes through, for the logs: the bone's kind, a creature's own bone by its key.
inline std::string_view CutName(const Site& s) {
    if (s.bone == Bone::Segment && s.key[0] != 0) {
        std::size_t n = 0;
        while (n < KeyChars && s.key[n] != 0) {
            ++n;
        }
        return {s.key.data(), n};
    }
    return BoneName(s.bone);
}

// One struck body of one model as a word, never 0 (0: no character): FNV-1a over its character
// and what decides its cut (the site's limb, bone, key and key number). The failed-hit table's
// (Cache::FailedHits): two struck names with one site are one body.
constexpr std::uint64_t StruckId(std::int32_t chr, const Site& s) {
    if (chr <= 0) {
        return 0;
    }
    std::uint64_t h = 0xCBF29CE484222325ull;
    const auto add = [&h](std::uint64_t byte) { h = (h ^ (byte & 0xFFu)) * 0x100000001B3ull; };
    for (int i = 0; i < 4; ++i) {
        add(static_cast<std::uint32_t>(chr) >> (8 * i));
    }
    add(static_cast<std::uint8_t>(s.limb));
    add(static_cast<std::uint8_t>(s.bone));
    add(static_cast<std::uint16_t>(s.key_number));
    add(static_cast<std::uint16_t>(s.key_number) >> 8);
    for (const char ch : s.key) {
        if (ch == 0) {
            break;
        }
        add(static_cast<unsigned char>(ch));
    }
    return h == 0 ? 1 : h;
}

// One bone of the enemy's skeleton (the live adapter's: its model's bone names and parents).
struct BoneRef {
    std::string_view name;
    std::int32_t parent{-1};
};
inline constexpr std::size_t MaxBones = 2048; // sever_mesh.h's bone mask (the game's largest: 1,490)

// The cut's two joints in the skeleton: the bone it crosses and the next one down the limb.
struct Chain {
    std::int32_t cut{-1};
    std::int32_t child{-1};
    constexpr bool Valid() const {
        return cut >= 0 && child >= 0;
    }
};

namespace Detail {
constexpr bool Own(std::string_view name) {
    return !Helper(name) && !Has(name, "collidable");
}
constexpr bool IsBone(std::string_view name, Bone b) {
    switch (b) {
    case Bone::Neck:
        return Own(name) && Has(name, "neck") && !Has(name, "necklace"); // (c1110's: an ornament)
    case Bone::UpperArm:
        return Own(name) && Has(name, "upperarm");
    case Bone::Forearm:
        return Own(name) && Has(name, "forearm");
    case Bone::Thigh:
        return Own(name) && Has(name, "thigh");
    case Bone::Calf:
        return Own(name) && Has(name, "calf");
    case Bone::Spine:
        return Own(name) && Has(name, "spine");
    case Bone::Segment:
    case Bone::None:
        break;
    }
    return false;
}
constexpr bool IsChild(std::string_view name, Bone b) {
    switch (b) {
    case Bone::Neck:
        return Own(name) && Has(name, "head");
    case Bone::UpperArm:
        return Own(name) && Has(name, "forearm");
    case Bone::Forearm:
        return Own(name) && Has(name, "hand");
    case Bone::Thigh:
        return Own(name) && Has(name, "calf");
    case Bone::Calf:
        return Own(name) && Has(name, "foot");
    case Bone::Spine:
        return Own(name) && Has(name, "spine"); // the next segment ("Spine" -> "Spine1")
    case Bone::Segment:
    case Bone::None:
        break;
    }
    return false;
}
// A twist bone skinning the cut segment, wherever it hangs ("LUpArmTwist" off the clavicle,
// "L ForeTwist" off the upper arm, "LCalfTwist" off the thigh, "LThighTwist" off the spine).
constexpr bool IsTwist(std::string_view name, Bone b) {
    if (!Has(name, "twist")) {
        return false;
    }
    switch (b) {
    case Bone::UpperArm:
        return Has(name, "uparm") || Has(name, "upperarm");
    case Bone::Forearm:
        return Has(name, "fore");
    case Bone::Thigh:
        return Has(name, "thigh");
    case Bone::Calf:
        return Has(name, "calf");
    case Bone::Neck:
    case Bone::Spine:
    case Bone::Segment:
    case Bone::None:
        break;
    }
    return false;
}
// Another limb's root below the cut (Bloodborne hangs the clavicles off the neck): left out.
constexpr bool LimbRoot(std::string_view name) {
    return Has(name, "clavicle") || Has(name, "shoulder") ||
           (Own(name) && (Has(name, "upperarm") || Has(name, "thigh")));
}
constexpr int SideOf(Limb l) {
    return l == Limb::LeftArm || l == Limb::LeftLeg     ? -1
           : l == Limb::RightArm || l == Limb::RightLeg ? 1
                                                        : 0;
}
} // namespace Detail

// A creature's cut by the struck body's own bone (Site::key; for a head, else the bone named
// head: a skull hung straight off the body, no neck): through it to its child down the limb
// (its side's, the most of its name in common, no helper), its own subtree detached. A bone at a
// limb's end is cut from its parent when that is a plain segment (its only own child): never
// from a root with every limb on it. Invalid: no such bone, or none of them gives a chain.
inline Chain KeyChain(const Site& site, std::span<const BoneRef> bones) {
    using namespace Detail;
    const auto n = static_cast<std::int32_t>(bones.size());
    const int side = SideOf(site.limb);
    // exact: the name alone; loose: a number after it too, whatever the key ("head": "Head1").
    const auto find = [&](const Key& key, bool exact, bool loose = false) {
        std::int32_t best = -1;
        int score = 0;
        for (std::int32_t i = 0; i < n && score < 2; ++i) {
            const int s = Own(bones[i].name) ? KeyMatch(bones[i].name, key, loose) : 0;
            if (s > score && (s == 2 || !exact)) {
                best = i;
                score = s;
            }
        }
        return best;
    };
    const auto common = [](std::string_view x, std::string_view y) {
        std::size_t k = 0;
        while (k < x.size() && k < y.size() && Lower(x[k]) == Lower(y[k])) {
            ++k;
        }
        return k;
    };
    const auto chain_at = [&](std::int32_t b) -> Chain {
        if (b < 0 || Side(bones[b].name) != side) {
            return {};
        }
        std::int32_t child = -1;
        std::size_t best = 0;
        for (std::int32_t i = 0; i < n; ++i) {
            if (bones[i].parent != b || !Own(bones[i].name) || Side(bones[i].name) != side) {
                continue;
            }
            const std::size_t k = common(bones[i].name, bones[b].name);
            if (child < 0 || k > best) {
                child = i;
                best = k;
            }
        }
        if (child >= 0) {
            return {b, child};
        }
        const std::int32_t p = bones[b].parent;
        if (p < 0 || p >= n || !Own(bones[p].name) || Side(bones[p].name) != side) {
            return {};
        }
        for (std::int32_t i = 0; i < n; ++i) {
            if (i != b && bones[i].parent == p && Own(bones[i].name)) {
                return {}; // (a joint of several: not a plain segment)
            }
        }
        return {p, b};
    };
    if (site.key[0] != 0) {
        const std::int32_t named = find(site.key, false);
        if (const Chain c = chain_at(named); c.Valid()) {
            return c;
        }
        // No bone of its name: its glued number as the segment's, exactly (never a prefix:
        // "l_leg1" is not "L_Leg10"), as written, then zero-padded to two and three digits ("neck"
        // 3: "Neck3"; "l_leg" 4: "L_Leg04"). Never another bone when its own is there and gives
        // no cut (the review, 2026-11-16: "L_Leg" struck, "L_Leg1" cut).
        if (named < 0 && site.key_number >= 0) {
            std::size_t k = 0;
            while (k < KeyChars && site.key[k] != 0) {
                ++k;
            }
            char digits[6]{};
            std::size_t d = 0;
            std::int32_t v = site.key_number;
            do {
                digits[d++] = static_cast<char>('0' + v % 10);
                v /= 10;
            } while (v > 0 && d < 5);
            for (std::size_t width = d; width <= 3; ++width) {
                if (width < d || k + width >= KeyChars || (width > d && width < 2)) {
                    continue;
                }
                Key numbered = site.key;
                for (std::size_t i = 0; i < width; ++i) {
                    numbered[k + i] = i < width - d ? '0' : digits[width - 1 - i];
                }
                if (const Chain c = chain_at(find(numbered, true)); c.Valid()) {
                    return c;
                }
            }
        }
    }
    if (site.limb == Limb::Head) {
        return chain_at(find(KeyOf("head"), false, true));
    }
    return {};
}

// The site's bone in this skeleton (its side's; the neck has none) and its next joint, a child
// of it. Every bone of the kind is tried, so a chain of them ("Neck", "Neck1" -> "Head") is cut
// on the one the next joint hangs from. None: the struck body's own bone (KeyChain). Invalid
// when there is neither: that enemy is not cut there.
inline Chain FindChain(const Site& site, std::span<const BoneRef> bones) {
    if (site.bone == Bone::None || bones.size() > MaxBones) {
        return {};
    }
    const int side = Detail::SideOf(site.limb);
    bool named = false; // a bone of the kind in this skeleton, joined to it: a broken chain, no cut
    for (std::size_t c = 0; c < bones.size(); ++c) {
        if (!Detail::IsBone(bones[c].name, site.bone) || Detail::Side(bones[c].name) != side) {
            continue;
        }
        named = named || bones[c].parent >= 0;
        for (std::size_t i = 0; i < bones.size(); ++i) {
            if (bones[i].parent == static_cast<std::int32_t>(c)) {
                named = true;
                if (Detail::IsChild(bones[i].name, site.bone)) {
                    return {static_cast<std::int32_t>(c), static_cast<std::int32_t>(i)};
                }
            }
        }
    }
    // None of its kind (a creature's skeleton; a stray unjoined one, c1300's "neck", counts as
    // none): the struck body's own bone. A head always may (c5510's neck of five, no head bone
    // joined to it: the struck neck's own bone); a limb with a broken chain is not cut. Never in
    // half by a key: a creature's pelvis is its root, the whole of it (the review, 2026-11-16).
    if (site.limb == Limb::Torso || (named && site.limb != Limb::Head)) {
        return {};
    }
    return KeyChain(site, bones);
}

// Whether the struck body's own bone (Site::key) decides this site's cut in this skeleton: then
// another body of the same limb and kind (another leg's segment, another neck) may cut where this
// one did not, and a failure says nothing of the model's other hits there (the uncuttable table
// keeps one bit a limb and kind: it is not marked; the review, 2026-11-16).
inline bool KeyDecides(const Site& site, std::span<const BoneRef> bones) {
    if (site.bone == Bone::Segment || site.limb == Limb::Head) {
        return true;
    }
    if (site.limb == Limb::Torso || bones.size() > MaxBones) {
        return false;
    }
    const int side = Detail::SideOf(site.limb);
    for (std::size_t c = 0; c < bones.size(); ++c) {
        if (!Detail::IsBone(bones[c].name, site.bone) || Detail::Side(bones[c].name) != side) {
            continue;
        }
        if (bones[c].parent >= 0) {
            return false;
        }
        for (std::size_t i = 0; i < bones.size(); ++i) {
            if (bones[i].parent == static_cast<std::int32_t>(c)) {
                return false;
            }
        }
    }
    return true;
}

// Whether every head hit on this skeleton cuts where this one did (`got`; invalid: no cut): the
// plain head's, and each bone's a hit could name (by its key; a struck name that names no bone
// ends at the plain head's). Only then is a head failure the model's and not the struck bone's:
// the uncuttable table keeps one bit for every head hit (c1190's heads cut at "HeadK001", never at
// "HeadK001_autoTerminal"; numbered necks, one named by no bone; a code review, 2026-11-17).
inline bool EveryHeadHitCutsAs(const Chain& got, std::span<const BoneRef> bones) {
    const auto same = [&](const Chain& c) {
        return c.Valid() ? got.Valid() && c.cut == got.cut && c.child == got.child : !got.Valid();
    };
    if (!same(FindChain(SiteOf("Head"), bones))) {
        return false;
    }
    for (const BoneRef& b : bones) {
        Site s = SiteOf(b.name);
        if (s.limb != Limb::Head) {
            continue;
        }
        if (!same(FindChain(s, bones))) {
            return false;
        }
        // ... and by its whole name: a struck "Head001001" is bone "Head001", which SiteOf reads
        // as "Head" and its counter (the review, 2026-11-17).
        s.key = Detail::ExactKey(b.name);
        s.key_number = -1;
        if (s.key[0] != 0 && !same(FindChain(s, bones))) {
            return false;
        }
    }
    return true;
}

// What a failure every kill here would repeat (no bones to cut through; no mesh blending into them
// in any look) says, for the kill hook's tables (no bite where no cut can come): Model, the
// model's at this limb and kind (Cache::Uncuttable: every hit there ends alike); Body, this struck
// body's alone (Cache::FailedHits: its own bone decided, another of the limb may cut); None, a
// heavy blow's (its site the worker's choice, not the struck one: never remembered).
enum class FailScope : std::uint8_t { None, Model, Body };
inline FailScope FailureScope(const Site& site, bool heavy, const Chain& got,
                              std::span<const BoneRef> bones) {
    if (heavy) {
        return FailScope::None;
    }
    if (!KeyDecides(site, bones)) {
        return FailScope::Model;
    }
    return site.limb == Limb::Head && EveryHeadHitCutsAs(got, bones) ? FailScope::Model
                                                                     : FailScope::Body;
}

// The detached piece's bones (sever_mesh.h Split's mask, 0 / 1 per bone): the cut bone and
// everything below it, except another limb hanging from it (the arms off the neck: a head comes
// off alone; cut in half, the arms go with the upper body and only a leg's bones stay), and the
// segment's own twist bones of its side wherever they hang. Every bone is
// walked up at most once per bone count: a broken (looping) skeleton ends. False, the mask all 0,
// without a valid chain or room.
inline bool DetachedMask(const Site& site, std::span<const BoneRef> bones, const Chain& chain,
                         std::span<std::uint8_t> mask) {
    std::fill(mask.begin(), mask.end(), std::uint8_t{0});
    const auto n = static_cast<std::int32_t>(bones.size());
    if (!chain.Valid() || chain.cut >= n || chain.child >= n || bones.size() > MaxBones ||
        mask.size() < bones.size()) {
        return false;
    }
    const int side = Detail::SideOf(site.limb);
    // A head cut through a neck leaves another limb hung off it on the body; one through the head's
    // own bone (a creature's, by its key) takes everything on the head (ears, mandibles).
    const bool neck_cut = site.limb == Limb::Head && Detail::IsBone(bones[chain.cut].name, Bone::Neck);
    // (Bit 2: the bone has a child, for the head's rule below; cleared at the end.)
    for (std::int32_t i = 0; i < n; ++i) {
        if (bones[i].parent >= 0 && bones[i].parent < n) {
            mask[static_cast<std::size_t>(bones[i].parent)] |= 2;
        }
    }
    for (std::int32_t i = 0; i < n; ++i) {
        const std::string_view name = bones[i].name;
        bool in = Detail::IsTwist(name, site.bone) && Detail::Side(name) == side;
        std::int32_t at = i;
        for (std::int32_t steps = 0; !in && at >= 0 && at < n && steps <= n; ++steps) {
            if (at == chain.cut) {
                in = true;
            } else if (site.limb == Limb::Torso ? Detail::Has(bones[at].name, "thigh")
                                                : Detail::LimbRoot(bones[at].name)) {
                break;
            } else if (neck_cut && bones[at].parent == chain.cut &&
                       Detail::Side(bones[at].name) != 0 && (mask[at] & 2) != 0 &&
                       Detail::LimbChain(bones[at].name)) {
                break; // another limb hung off the neck (c1400's arms): not the head's
            } else {
                at = bones[at].parent;
            }
        }
        mask[i] = static_cast<std::uint8_t>((mask[i] & 2) | (in ? 1 : 0));
    }
    for (std::int32_t i = 0; i < n; ++i) {
        mask[i] &= 1;
    }
    return true;
}

// The claw's slash at the hit: its edge (base -> tip) and its motion (the tip's velocity).
struct Slash {
    bool known{false};
    Vec3 edge{}, motion{};
};

struct Plane {
    bool ok{false};
    Vec3 origin{}, normal{}; // the normal points to the detached side (sever_mesh.h Cut)
    float t{0.0f};           // where along the bone
    float tilt_deg{0.0f};    // how far the slash turned it off square
};

namespace Detail {
inline Vec3 Sub(Vec3 a, Vec3 b) {
    return {a.x - b.x, a.y - b.y, a.z - b.z};
}
inline Vec3 Add(Vec3 a, Vec3 b) {
    return {a.x + b.x, a.y + b.y, a.z + b.z};
}
inline Vec3 Mul(Vec3 a, float s) {
    return {a.x * s, a.y * s, a.z * s};
}
inline float Dot(Vec3 a, Vec3 b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}
inline Vec3 Cross(Vec3 a, Vec3 b) {
    return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}
inline bool Finite(Vec3 a) {
    return std::isfinite(a.x) && std::isfinite(a.y) && std::isfinite(a.z);
}
// Unit `a`, or false when it is too short or not finite.
inline bool Unit(Vec3 a, float min_len, Vec3& out) {
    const float len = std::sqrt(Dot(a, a));
    if (!Finite(a) || !std::isfinite(len) || len < min_len) {
        return false;
    }
    out = Mul(a, 1.0f / len);
    return true;
}
} // namespace Detail

// The native corpse shrinks the limb onto a joint. Translate a wound only far enough along
// its own bone for its depth range to reach that joint. Its lateral skin outline stays put.
struct StumpSection {
    Vec3 joint{NAN, NAN, NAN};
    Vec3 axis{};
    float low{INFINITY}, high{-INFINITY};

    void Include(Vec3 vertex) {
        const float depth = Detail::Dot(Detail::Sub(vertex, joint), axis);
        low = std::min(low, depth);
        high = std::max(high, depth);
    }

    Vec3 Place(Vec3 middle) const {
        if (!Detail::Finite(joint)) return middle;
        if (low > high) return joint; // unrelated fallback root: no known cut axis
        return Detail::Sub(middle, Detail::Mul(axis, std::clamp(0.0f, low, high)));
    }
};

// The tilt a cut at `t` along a bone of `length` may take on a limb `radius_m` thick (both m):
// at the limb's surface the tilted plane may reach JointClearance of the way to the nearer
// joint, never past it. No radius known (0): the site's tilt alone.
inline float TiltRoom(float t, float length, float radius_m, float site_deg) {
    if (!(radius_m > 0.0f) || !std::isfinite(radius_m)) {
        return site_deg;
    }
    const float room = std::min(t, 1.0f - t) * length * JointClearance;
    const float deg = std::atan(std::max(room, 0.0f) / radius_m) * 57.29578f;
    return std::min(site_deg, deg);
}

// The cut across a bone from `joint0` (its parent end) to `joint1` (toward the detached side),
// at `contact` projected onto it (kept within the site's span), square to the bone and turned
// toward the slash's plane (the edge swept along its motion) by up to the site's tilt, less on a
// limb too thick for it (`radius_m`, the live adapter's measure of the mesh round the bone; 0
// unknown). All in one frame (the live adapter's: the frozen pose's model space, metres). Not ok
// for a missing or degenerate bone; a degenerate slash just leaves it square.
inline Plane CutPlane(Vec3 joint0, Vec3 joint1, Vec3 contact, const Site& site, const Slash& slash,
                      float radius_m = 0.0f) {
    using namespace Detail;
    Plane out;
    Vec3 axis{};
    const Vec3 bone = Sub(joint1, joint0);
    if (site.bone == Bone::None || !Finite(joint0) || !Unit(bone, MinBoneM, axis) ||
        !(site.t_min >= 0.0f && site.t_min <= site.t_max && site.t_max <= 1.0f)) {
        return out;
    }
    const float length = std::sqrt(Dot(bone, bone));
    float t = Finite(contact) ? Dot(Sub(contact, joint0), axis) / length : 0.5f;
    t = std::clamp(std::isfinite(t) ? t : 0.5f, site.t_min, site.t_max);
    out.ok = true;
    out.t = t;
    out.origin = Add(joint0, Mul(bone, t));
    out.normal = axis;
    // The slash's plane holds the edge and its motion; its normal, onto the detached side.
    Vec3 edge{}, motion{}, slash_n{};
    if (!slash.known || !Unit(slash.edge, 1e-3f, edge) || !Unit(slash.motion, 1e-3f, motion) ||
        !Unit(Cross(edge, motion), 0.05f, slash_n)) {
        return out; // parallel edge and motion (a stab), or no slash: square
    }
    if (Dot(slash_n, axis) < 0.0f) {
        slash_n = Mul(slash_n, -1.0f);
    }
    const float cos_tilt = std::clamp(Dot(slash_n, axis), -1.0f, 1.0f);
    const float tilt = std::acos(cos_tilt) * 57.29578f;
    const float limit =
        std::clamp(TiltRoom(t, length, radius_m, site.max_tilt_deg), 0.0f, 89.0f);
    if (tilt <= limit) {
        out.normal = slash_n;
        out.tilt_deg = tilt;
        return out;
    }
    // Turned only `limit` from square, toward the slash.
    Vec3 toward{};
    if (!Unit(Sub(slash_n, Mul(axis, cos_tilt)), 1e-4f, toward)) {
        return out;
    }
    const float r = limit / 57.29578f;
    out.normal = Add(Mul(axis, std::cos(r)), Mul(toward, std::sin(r)));
    out.tilt_deg = limit;
    return out;
}

} // namespace VR::Sever
