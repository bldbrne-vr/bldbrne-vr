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

#include "core/vr/melee_moves.h"
#include "core/vr/swing_detector.h"

// Physical melee: the swing IS the attack (melee=physical). No attack animation plays: the hunter
// stays in his idle or locomotion state, the VR body's arm IK keeps the weapon on the controller,
// and while the real blade moves fast the game's own attack is opened on the weapon's dummy polys
// (the TAE's InvokeAttack, called from the pose-copy wrapper; core/vr/bloodborne_melee.h). The
// game sweeps that capsule along the blade's path every frame and does the rest: damage with the
// row's motion value, stamina, poise, rally, blood, sounds.
//
// This header is the pure part, unit-tested in tests/test_melee_window.cpp:
//  - ChooseMove: which of the weapon's rows (core/vr/melee_moves.h) a swing opens: light, heavy
//    (a wound-up stroke, or the right trigger held 0.35 s) or charged (the trigger held
//    melee_charge_s), in the weapon's current form; a missing heavier move falls back to a lighter
//    one of the same form.
//  - WindowTracker (the presenter, every XR frame): when a swing's window opens and closes. It
//    opens at the stroke's start (melee_open=onset: the blade turning fast with the hand moving
//    fast, not toward the body, not upward, not a recovery stroke bringing the weapon back) so
//    point-blank hits land, and must then commit through
//    the swing classifier's gate (2 m/s for 40 ms, 15 cm, the blade turning 15 deg or a stab)
//    within 0.15 s, or it is aborted (its stamina is given back if it hit nothing). It stays open
//    while the blade still turns fast (max(1.5 rad/s, 30% of the stroke's peak)) or the hand
//    moves (1.4 m/s), and closes 30 ms after both slowed, or after 0.6 s.
//  - WindowLatch (the game's pose-copy wrapper, every game frame): one attack per window, created
//    once (stamina once, one hit list), kept alive every frame while the window is open (at least
//    two game frames, so a window that opened and closed between two frames still sweeps), and
//    ended for good when a gate fails mid-swing (a dodge, a hit taken: never re-created).
//  - ComboBudget (melee_budget, the default): the swing rate. Each weapon has two quick charges
//    that come back at the weapon's own pace in the game (core/vr/melee_moves.h Paces); a swing
//    with too few left does not register (no attack, a dull buzz). Transforms never use them.
//  - DamageFactor: hits scaled by the blade tip's speed (0.75 at a gentle tap .. 1.10 at full
//    speed); with melee_budget=0 (and for a flick's transform attack, as before the budget) also
//    by the swing cadence (swinging faster than the game's own recovery lowers each hit, down to
//    0.5), so waggling never beats the game's own damage per second.
//  - BiteFollow: the blade "bites" on a hit: for a moment the hunter's hand follows the controller
//    only halfway (a felt hit stop on our side, the game never freezes), then catches up.
//  - TelegraphTracker (the presenter): which swings the enemies may read (melee_telegraph): a
//    wind-up, a heavy, charged, long or wide swing sends the game's own "the target attacks"
//    notice to the AIs, as a vanilla attack does; a quick light flick never does.
namespace VR::Melee {

enum class MoveKind : std::uint8_t { Light, Heavy, Charged };

inline const char* MoveKindName(MoveKind k) {
    switch (k) {
    case MoveKind::Light:
        return "light";
    case MoveKind::Heavy:
        return "heavy";
    case MoveKind::Charged:
        return "charged";
    }
    return "?";
}

// ---- Which row a swing opens --------------------------------------------------------------------

struct MoveChoice {
    const MoveRow* row{nullptr}; // null: no physical move for this weapon and form
    MoveKind kind{MoveKind::Light};
    bool lighter{false}; // a lighter move than asked for (the heavier one does not exist)
};

inline MoveChoice ChooseMove(const WeaponMoves& moves, bool form_b, MoveKind want) {
    const std::size_t base = form_b ? 3 : 0;
    for (int k = static_cast<int>(want); k >= 0; --k) {
        const MoveRow& row = moves.rows[base + static_cast<std::size_t>(k)];
        if (row.Valid()) {
            return {&row, static_cast<MoveKind>(k), k != static_cast<int>(want)};
        }
    }
    return {};
}

// Resolve stamina before creating the hit list: an unaffordable strong swing must use
// the light row itself (damage, poise, rally, capsule and sound), not just a smaller bill.
inline MoveChoice AffordableMove(MoveChoice choice, const MoveRow* light,
                                 std::int32_t stamina) {
    if (choice.row && choice.kind != MoveKind::Light && stamina < choice.row->stamina)
        return {light, MoveKind::Light, true};
    return choice;
}

// ---- Damage -------------------------------------------------------------------------------------

struct DamageConfig {
    bool speed{true};     // melee_speed_damage
    bool cadence{true};   // ... and by the swing rate (melee_budget=0, a flick's attack)
    float tip_slow{3.5f}; // m/s blade tip: the floor's speed ...
    float tip_fast{9.0f}; // ... and full speed
    float floor{0.75f};   // the factor of a gentle tap that still committed
    float full{1.10f};    // ... and of a full-speed swing
    float min{0.5f};      // the total factor's bounds
    float max{1.10f};
    float max_heavy{1.15f};
};

// The game's own recovery after each move (its R1 cancel start: the Saw Cleaver's 0.57 s, its
// heavy 0.80 s, its charged 0.93 s; TAE ChrActionFlag 4).
inline float RecoverSeconds(MoveKind k) {
    switch (k) {
    case MoveKind::Light:
        return 0.57f;
    case MoveKind::Heavy:
        return 0.80f;
    case MoveKind::Charged:
        return 0.93f;
    }
    return 0.57f;
}

inline float SpeedFactor(float tip_speed, const DamageConfig& c) {
    return c.floor + (c.full - c.floor) * Smoothstep(c.tip_slow, c.tip_fast, tip_speed);
}

// `since_s`: from the previous window that counted (it committed or hit) to this one's opening.
// Proportional, so hits per second x damage per hit never beats the game's own rate (the swing
// gate allows a swing every 0.35 s at most: x0.61 for a light's 0.57 s; the 0.5 floor only bounds
// the heavier rows).
inline float CadenceFactor(float since_s, float recover_s) {
    if (recover_s <= 0.0f) {
        return 1.0f;
    }
    return std::clamp(since_s / recover_s, 0.5f, 1.0f);
}

inline float DamageFactor(float tip_speed, float since_s, MoveKind kind, const DamageConfig& c) {
    if (!c.speed) {
        return 1.0f;
    }
    const float f = SpeedFactor(tip_speed, c) *
                    (c.cadence ? CadenceFactor(since_s, RecoverSeconds(kind)) : 1.0f);
    return std::clamp(f, c.min, kind == MoveKind::Light ? c.max : c.max_heavy);
}

// ---- The hit capsule's radius -------------------------------------------------------------------
//
// The game's capsule radii are fat for its canned swings (0.3-0.5 m round a 2-3 cm sword blade):
// in VR they hit far from the blade you see. melee_radius_scale (0.7) trims every row; never below
// the row's own radius up to `floor` (0.2 m: a thin capsule stays the game's, the Kirkhammer's
// hammer 0.20 m rather than 0.14). With melee_radius_fit the radius also never exceeds the blade's
// own reach off the capsule's axis (MoveRow::blade, from the weapon model) + melee_radius_margin_cm,
// at least `min` (a thin sword 0.12 m instead of 0.28; the Saw Cleaver's wide blade keeps 0.28).
// The fit holds only for a capsule row while the part in the hand has the model's shape: its
// dmy1 -> dmy2 span within `span_tol` of MoveRow::span (the Saw Cleaver's form B unfolds its blade:
// no fit there). A sphere row has no span to check it by (the Hunter Axe's head is a sphere on a
// haft its form A shortens): never fitted.

struct RadiusConfig {
    float scale{0.7f};      // melee_radius_scale
    bool fit{true};         // melee_radius_fit
    float margin{0.10f};    // melee_radius_margin_cm / 100
    float min{0.12f};       // the fit never below
    float floor{0.20f};     // the scale never below min(the row's radius, this)
    float span_tol{0.03f};  // m: the live span must match the model's this well
};

// `span_now`: the live dmy1 -> dmy2 distance (< 0: unknown, no fit).
inline float SwingRadius(float row_radius, float blade, float span, float span_now,
                         const RadiusConfig& c) {
    if (!(row_radius > 0.0f)) {
        return row_radius;
    }
    float r = row_radius * std::clamp(c.scale, 0.0f, 1.0f);
    r = std::max(r, std::min(row_radius, c.floor));
    if (c.fit && blade > 0.0f && span > 0.0f && span_now >= 0.0f &&
        std::abs(span_now - span) <= c.span_tol) {
        r = std::min(r, std::max(blade + c.margin, c.min));
    }
    return std::min(r, row_radius);
}

// ---- The combo budget (melee_budget) ------------------------------------------------------------
//
// The swing rate, so flicking the wrist never kills everything: each weapon has `charges` (2) quick
// charges. A light swing uses one, a heavy two, a charged one all of them; a swing with too few
// left does not register (no attack: the game plays its whoosh and trail only, the presenter a dull
// "empty" buzz), and every swing that registers does the row's full damage (the tip-speed factor
// only). The charges a swing used come back together, after that move's own recovery in the game
// (WeaponPace: its TAE's R1 cancel, the Saw Cleaver's light 0.567 s, heavy 0.867, charged 0.933)
// x melee_budget_scale + melee_budget_offset_ms (-80 ms: 0.487 s), counted from the swing or from
// the previous block's return, whichever is later. So swings at the game's own pace always
// register, two quick ones in a row do too, and flailing gets the game's own rate (plus the banked
// charge); a light then a heavy waits for the light's charge, as the game's R1 -> R2. A wound-up
// stroke (not trigger-armed) with one charge left is a light swing. Swings that never committed
// (and hit nothing), the game refused, or that became a flick's transform attack before their own
// attack hit anything give theirs back. Transforms (a squeeze, a flick and its transform attack)
// never use or need charges. melee_budget=0: the old rules (TrackerConfig::min_interval between
// swings, the cadence damage factor).

inline constexpr int BudgetMaxCharges = 8;

// Quick weapons (2026-10-15, the tester: their real combos are three quick hits): a weapon whose
// light swing recovers within `quick_s` in its form in the game's own TAE (TrackerInput::
// light_pace: the Saw Cleaver's 0.567 s, the Blade of Mercy's, the Beast Claw's, the
// Reiterpallasch's, the Rakuyo's twin blades' 0.633...; never a form without a pace of its own,
// nor a whip form) has `quick_charges` (3). The third swing in a row (two charges already out)
// comes a moment later and slower back: it waits `quick_gap_s` after the swing before it, and its
// block takes `quick_last_scale` x its refill.
struct BudgetConfig {
    bool enabled{true};      // melee_budget
    int charges{2};          // melee_budget_charges (1 .. BudgetMaxCharges)
    float scale{1.0f};       // melee_budget_scale: x the game's recovery
    float offset_s{-0.080f}; // melee_budget_offset_ms: + this (s)
    float min_s{0.05f};      // a block never comes back sooner
    // The charges a heavy swing uses (BudgetCost). The whip forms: 1 (2026-10-17, the tester: a
    // lash is nearly always heavy, and at 2 every lash emptied the budget for its whole refill,
    // so the next one's chain passed through enemies unregistered).
    int heavy_cost{2};
    int quick_charges{2};    // melee_budget_quick_charges (the settings' 3; <= charges: off)
    float quick_s{0.64f};    // melee_budget_quick_s: a light recovery this short is quick
    float quick_gap_s{0.10f};       // the third charge waits this long after the swing before
    float quick_last_scale{1.25f};  // ... and comes back this much slower
};

// The charges of a weapon whose light swing recovers in `light_recover_s` in the game (its form's
// own pace; 0: none, not quick).
inline int BudgetCapacity(const BudgetConfig& c, float light_recover_s) {
    const bool quick = c.quick_charges > c.charges && light_recover_s > 0.0f &&
                       light_recover_s <= c.quick_s;
    return std::clamp(quick ? c.quick_charges : c.charges, 1, BudgetMaxCharges);
}

// The charges a move uses: light 1, heavy `heavy_cost` (2; the whip forms 1), charged all.
inline int BudgetCost(MoveKind k, int charges, int heavy_cost = 2) {
    charges = std::clamp(charges, 1, BudgetMaxCharges);
    switch (k) {
    case MoveKind::Light:
        return 1;
    case MoveKind::Heavy:
        return std::clamp(heavy_cost, 1, charges);
    case MoveKind::Charged:
        return charges;
    }
    return 1;
}

// The game's recovery of `kind` for the weapon in its form (s): its WeaponPace, else the generic
// RecoverSeconds.
inline float PaceRecover(const WeaponPace* pace, bool form_b, MoveKind kind) {
    if (pace != nullptr) {
        const float r = pace->recover[(form_b ? 3u : 0u) + static_cast<std::size_t>(kind)];
        if (r > 0.0f) {
            return r;
        }
    }
    return RecoverSeconds(kind);
}

// How long a move's charges take to come back (s).
inline float BudgetRefill(float recover_s, const BudgetConfig& c) {
    return std::max(recover_s * c.scale + c.offset_s, c.min_s);
}

class ComboBudget {
public:
    int Capacity() const {
        return cap;
    }
    // A new charge count empties the queue (all charges free; a transform between a quick form
    // and a slower one too: fine, the tester 2026-10-15).
    void SetCapacity(int n) {
        n = std::clamp(n, 1, BudgetMaxCharges);
        if (n != cap) {
            cap = n;
            count = 0;
        }
    }
    void Clear() {
        count = 0;
    }
    // The charges free at `now` (s, the caller's clock).
    int Available(double now) const {
        int used = 0;
        for (int i = 0; i < count; ++i) {
            if (blocks[i].ready > now) {
                used += blocks[i].charges;
            }
        }
        return std::max(cap - used, 0);
    }
    // Seconds since the last swing that used charges still out (large: none out).
    double SinceLastUse(double now) const {
        return count > 0 ? now - blocks[count - 1].used : 1e9;
    }
    // Seconds until the next block comes back (0: all free).
    float NextIn(double now) const {
        for (int i = 0; i < count; ++i) {
            if (blocks[i].ready > now) {
                return static_cast<float>(blocks[i].ready - now);
            }
        }
        return 0.0f;
    }
    // The charges window `serial` holds now (0: none, or back already).
    int Held(std::uint32_t serial, double now) const {
        for (int i = 0; i < count; ++i) {
            if (blocks[i].serial == serial && blocks[i].ready > now) {
                return blocks[i].charges;
            }
        }
        return 0;
    }
    // Window `serial` uses `charges`; they come back `refill_s` after now or after the previous
    // block, whichever is later. False (nothing used) when fewer are free.
    bool Spend(std::uint32_t serial, int charges, float refill_s, double now) {
        Prune(now);
        if (charges <= 0 || Available(now) < charges || count >= BudgetMaxCharges) {
            return false;
        }
        Block& b = blocks[count];
        b.serial = serial;
        b.charges = charges;
        b.refill = std::max(refill_s, 0.0f);
        b.used = now;
        b.start = count > 0 ? std::max(now, blocks[count - 1].ready) : now;
        b.ready = b.start + b.refill;
        ++count;
        return true;
    }
    // Window `serial`'s charges come back now, as if it never used them: the blocks after it move
    // up (the time a returning block had already waited goes to the next one). False when it held
    // none.
    bool Refund(std::uint32_t serial, double now) {
        Prune(now);
        for (int i = 0; i < count; ++i) {
            if (blocks[i].serial != serial) {
                continue;
            }
            const double anchor = i > 0 ? blocks[i - 1].ready : blocks[i].start;
            for (int j = i; j + 1 < count; ++j) {
                blocks[j] = blocks[j + 1];
            }
            --count;
            Chain(i, anchor);
            return true;
        }
        return false;
    }
    // Window `serial` uses `charges` with `refill_s` instead, from when it was used (a heavy the
    // classifier judged light). False when it held none.
    bool Rebill(std::uint32_t serial, int charges, float refill_s, double now) {
        Prune(now);
        for (int i = 0; i < count; ++i) {
            if (blocks[i].serial != serial) {
                continue;
            }
            blocks[i].charges = std::max(charges, 1);
            blocks[i].refill = std::max(refill_s, 0.0f);
            Chain(i, i > 0 ? blocks[i - 1].ready : blocks[i].start);
            return true;
        }
        return false;
    }

private:
    struct Block {
        std::uint32_t serial{0};
        int charges{0};
        float refill{0.0f};
        double used{0.0};  // when the swing used them
        double start{0.0}; // when they started coming back
        double ready{0.0}; // ... and are back
    };
    // Blocks back already leave the queue (they are always its head: `ready` only grows).
    void Prune(double now) {
        int k = 0;
        while (k < count && blocks[k].ready <= now) {
            ++k;
        }
        if (k > 0) {
            for (int j = k; j < count; ++j) {
                blocks[j - k] = blocks[j];
            }
            count -= k;
        }
    }
    void Chain(int from, double anchor) {
        for (int j = from; j < count; ++j) {
            blocks[j].start = std::max(blocks[j].used, anchor);
            blocks[j].ready = blocks[j].start + blocks[j].refill;
            anchor = blocks[j].ready;
        }
    }
    std::array<Block, BudgetMaxCharges> blocks{};
    int count{0};
    int cap{2};
};

// ---- The presenter's window ---------------------------------------------------------------------

struct TrackerConfig {
    bool onset{true};            // melee_open=onset (else the window opens at the commit)
    float onset_rate{3.0f};      // rad/s: the blade turning this fast (the stroke's start) ...
    float onset_hand{2.0f};      // m/s: ... with the hand this fast (head-relative)
    float confirm_s{0.15f};      // an onset window must commit within this
    float commit_tip{0.0f};      // m/s: a window whose far end got this fast commits when it
                                 //   would be aborted (the whip forms: the chain flung out is the
                                 //   swing; the gate still vetoes raises and wind-ups; 0 = off)
    // s: a wound-up stroke opened within this of the last committed one, when that one was a
    // heavy, is a light swing (the Beast Cutter's whip, 0.8; 0 = off). In a flurry the backswing
    // is the last lash coming back, not a wind-up: with a heavy costing one charge
    // (BudgetConfig::heavy_cost) every such lash was a 50-stamina heavy, two of them a whole bar
    // (2026-10-18). The one after a flurry's light may be heavy again (feedback: heavy,
    // light, heavy...). Trigger-armed swings are never changed.
    float flurry_s{0.0f};
    float keep_rate{1.5f};       // open while the blade turns faster than max(keep_rate,
    float keep_frac{0.30f};      //   keep_frac x the stroke's peak) ...
    float keep_hand{1.4f};       // ... or the hand moves this fast
    float end_hold{0.03f};       // both slower this long: closed
    float max_open{0.6f};        // s
    float min_interval{0.35f};   // between windows that counted (melee_budget=0 only)
    float merge_s{0.5f};         // a commit this soon after a window that hit stays that window
    float charge_heavy_s{0.35f}; // right trigger held: heavy
    float charge_full_s{1.0f};   // ... charged (melee_charge_s)
    float charge_keep_s{1.5f};   // armed this long after it is let go
    BudgetConfig budget{};       // the swing rate (melee_budget; replaces min_interval)
    float hold_s{0.15f};         // melee_left's counter-motion window (TrackerInput::hold_ago)
};

struct TrackerInput {
    float dt{0.0f};
    bool swings{false}; // swings may attack now (SwingGate, talk guard, ...)
    // The classifier this frame (core/vr/swing_detector.h).
    bool stroke_live{false};
    unsigned stroke{0};
    bool cocked{false};
    bool recovery{false};    // the stroke brings the weapon back (Classifier::Recovery)
    float blade_rate{0.0f};  // rad/s
    float stroke_peak{0.0f}; // rad/s
    bool fired{false};       // the commit gate passed (for fired_stroke)
    unsigned fired_stroke{0};
    Kind kind{Kind::None};   // a decision: Light / Heavy ...
    unsigned kind_stroke{0}; // ... for this stroke (Output::stats.id)
    Kind gate{Kind::None};   // Ignored / Rejected
    // The weapon hand (head-relative) and the horizontal view.
    float hand_speed{0.0f};
    V3 hand_v{};
    V3 fwd_flat{0.0f, 0.0f, -1.0f};
    float tip_speed{0.0f}; // m/s, the blade's far end
    bool trigger{false};   // the right trigger held
    // The game: the newest window serial that hit an enemy (Hit events).
    std::uint32_t hit_serial{0};
    // The weapon in hand (its current form): the game's recovery of its light, heavy and charged
    // move (s; PaceRecover), the pace of the combo budget.
    std::array<float, 3> recover{RecoverSeconds(MoveKind::Light), RecoverSeconds(MoveKind::Heavy),
                                 RecoverSeconds(MoveKind::Charged)};
    // ... and its light's recovery in the game's own TAE (WeaponPace; 0: no pace of its own in
    // this form, or a whip form): what makes it a quick weapon (BudgetCapacity).
    float light_pace{0.0f};
    // ... and the heaviest move it has in that form (ChooseMove's fallback: a heavy swing of a
    // weapon without a heavy row opens its light row, and costs a light's charge).
    MoveKind top{MoveKind::Charged};
    // melee_left: an off-hand stroke that holds a charge started this long ago (s; < 0: none).
    // Within TrackerConfig::hold_s of a swing's own start (either first) it may be that swing's
    // counter-motion: a wound-up swing one short waits (unpaid) for its commit or that charge back
    // (PayLate) before it settles for a light. Measured against each window's own start (a swing
    // replacing an open window is held by its own).
    float hold_ago{-1.0f};
};

enum class Charge : std::uint8_t { None, Heavy, Full };

struct Window {
    std::uint32_t serial{0};
    MoveKind kind{MoveKind::Light};
    bool open{false};
    bool committed{false}; // through the swing gate (an onset window until then)
    bool aborted{false};   // closed without committing: no stamina for it unless it hit
    bool cocked{false};    // the stroke started wound up
    // The classifier's light / heavy for a heavy window opened on a wound-up stroke (a lighter
    // verdict: the game's update gives the stamina difference back if nothing was hit yet).
    bool verdict_known{false};
    MoveKind verdict{MoveKind::Light};
    unsigned stroke{0};
    float open_t{0.0f}; // tracker clock (s)
    float close_t{0.0f};
    float tip_speed{0.0f};      // now
    float tip_peak{0.0f};       // the window's peak so far
    float since_counted{10.0f}; // from the last window that counted to this one's opening (s)
    bool hit{false};            // the game reported an enemy hit by it
    const char* why{""};        // how it opened / closed
    bool armed{false};          // the right trigger chose its kind (heavy / charged)
    bool powered{false};        // a released Stake Driver charge: independent of hand speed
    // The combo budget (melee_budget): a swing it could not pay does not register: no attack
    // (the game plays its whoosh and trail only), unless a transform flick joins its stroke.
    bool unpaid{false};
    bool late{false};         // paid at its commit (nothing was free at the stroke's start)
    bool downgraded{false};   // wound up with one charge left: a light swing
    bool flurry{false};       // wound up right after the last lash (TrackerConfig::flurry_s): light
    std::uint8_t charges{0};  // the charges it holds (0: none, given back, or no budget)
    std::uint8_t left{0};     // free after it was paid (or when it could not be)
    std::uint8_t capacity{0}; // melee_budget_charges (0: no budget)
    float refill_s{0.0f};     // its charges come back this long after they are used
    float next_s{0.0f};       // not paid: the next charge back in this long (s)
    // A transform flick was squeezed during it or just before it opened (NoteFlick): its stroke
    // is the transform's. Opened after the squeeze: never paid; open at the squeeze: never paid
    // at its commit, no empty buzz (what it paid at its start comes back from the game when the
    // transform attack takes over and its own attack had hit nothing).
    bool flick{false};
};

struct TrackerOutput {
    bool opened{false};
    bool committed{false}; // this frame
    bool closed{false};
    bool aborted{false};
    bool verdict{false};      // the verdict arrived this frame
    bool charge_heavy{false}; // the trigger charge reached heavy / full this frame
    bool charge_full{false};
    Charge charge{Charge::None};
    bool empty{false};    // a swing the combo budget could not pay committed (the dull buzz)
    bool paid_late{false}; // ... or it could at the commit (it registers after all)
    bool refunded{false};  // the window's charges came back this frame (aborted, judged light)
};

class WindowTracker {
public:
    const Window& Current() const {
        return w;
    }
    Charge ChargeState() const {
        return charge;
    }
    float ChargeHeld() const {
        return charge_hold;
    }
    // The combo budget (its clock: BudgetClock, the real time of the steps).
    const ComboBudget& Budget() const {
        return Pool();
    }
    double BudgetClock() const {
        return Clock();
    }
    int BudgetFree() const {
        return Pool().Available(Clock());
    }

    // melee_left: both hands swing at one pace. This tracker pays from `owner`'s combo budget, on
    // the owner's clock (the right hand's), so a left swing takes the weapon's next swing slot; its
    // windows' charges are kept apart from the owner's own (PooledKey). nullptr: its own budget.
    void PayFrom(WindowTracker* owner) {
        pool = owner;
    }

    // A charge came back while this window was open and could not be paid (melee_left: the off
    // hand's counter-motion that took it was dropped): paid now if it can be, as at its commit.
    // True when it was.
    bool PayLate(const TrackerConfig& cfg) {
        if (!w.open || !w.unpaid || w.flick) {
            return false;
        }
        hold_ago = -1.0f; // (the charge it waited for is back, or never will be)
        Bill(cfg);
        w.late = !w.unpaid;
        return !w.unpaid;
    }

    // The open window's stroke is no swing after all (melee_left: the off hand's counter-motion to
    // a right swing): it closes as aborted, committed or not, and its charges come back (the game
    // gives its stamina back the same way). Nothing when it hit something.
    TrackerOutput Drop(const char* why) {
        TrackerOutput out{};
        if (w.open && !w.hit) {
            w.committed = false;
            Close(out, why);
        }
        return out;
    }

    // A transform flick squeezed now: the open window, else the next one opening within
    // `attach_s`, is its stroke (one window, as the game's FlickJoins joins one per transform).
    void NoteFlick(float attach_s = 0.35f) {
        flick_attach = attach_s;
        if (w.open) {
            w.flick = true;
            flick_t = -10.0f;
        } else {
            flick_t = t;
        }
    }

    // The game refused window `serial`, or its stroke became a flick's transform attack before
    // its own attack hit anything: its charges come back. True when it held any.
    bool RefundWindow(std::uint32_t serial) {
        if (!Pool().Refund(Key(serial), Clock())) {
            return false;
        }
        if (w.serial == serial) {
            w.charges = 0;
            w.left = static_cast<std::uint8_t>(Pool().Available(Clock()));
        }
        return true;
    }

    // The game-side stamina check can downgrade after the XR thread reserved a strong
    // swing. Rebill that same window as a light, without creating another attack.
    bool ResolveLightAttack(std::uint32_t serial, const BudgetConfig& cfg) {
        if (!serial || w.serial != serial || w.kind == MoveKind::Light)
            return false;
        w.kind = MoveKind::Light;
        w.verdict = MoveKind::Light;
        w.verdict_known = true;
        w.downgraded = true;
        const float refill = BudgetRefill(w_recover[0], cfg) * w_refill_mult;
        if (w.charges > 0 && Pool().Rebill(Key(serial), 1, refill, Clock())) {
            w.charges = 1;
            w.refill_s = refill;
            w.left = static_cast<std::uint8_t>(Pool().Available(Clock()));
        }
        return true;
    }

    // Time that passed with no Step (a game menu, the virtual screen, focus loss, the R1 / R2
    // route of the whip forms): the combo budget refills and the cadence / flick clock runs on
    // real time whatever the route. The presenter adds the gap since its last Step (seconds).
    void Advance(double seconds) {
        if (!(seconds > 0.0)) {
            return;
        }
        bt += std::min(seconds, 86400.0);
        if (!w.open) {
            // Past every interval the clock times. An open window keeps the steps' clamped
            // clock (a slow frame never closes it early).
            t += static_cast<float>(std::min(seconds, 30.0));
        }
    }

    // A new revolution of a spun whip (core/vr/whip_spin.h SpinCounter, the presenter): a swing
    // of its own, as a committed window (the open one closes: a new hit list), paid from the combo
    // budget like any other, so continuous spinning costs a charge per revolution. Call after Step
    // in a frame where Step opened nothing; `in` as given to Step (its stroke is the window's). A
    // whip's crack (core/vr/whip_spin.h CrackGate: the chain lashed by the wrist alone) opens the
    // same way, `why` "a whip crack".
    TrackerOutput OpenRevolution(const TrackerInput& in, const TrackerConfig& cfg,
                                 const char* why = "a whip revolution") {
        TrackerOutput out{};
        if (!in.swings) {
            return out;
        }
        if (w.open) {
            Close(out, "a new revolution");
        }
        TrackerInput r = in;
        r.cocked = false; // a revolution is light (heavy / charged: the trigger's charge)
        Open(r, cfg, out, true, in.stroke, why);
        out.charge = charge;
        return out;
    }

    // A charged mechanism discharged on trigger release. The caller owns charge/cooldown
    // gating; this window still pays the ordinary charged-attack combo and stamina costs.
    TrackerOutput OpenPowered(const TrackerInput& in, const TrackerConfig& cfg) {
        TrackerOutput out{};
        if (!in.swings || in.top != MoveKind::Charged) {
            return out;
        }
        if (w.open) {
            Close(out, "a powered discharge");
        }
        flick_t = -10.0f; // a discharge cannot inherit a free transform-flick attack
        charge = Charge::Full;
        TrackerInput release = in;
        release.trigger = false;
        Open(release, cfg, out, true, 0, "charged trigger release");
        w.powered = true;
        charge_used = Charge::None; // a refused discharge never arms a later ordinary swing
        return out;
    }

    // Context switch, recenter, focus loss: closes an open window (aborted if it had not
    // committed) and drops the charge.
    TrackerOutput Reset(const char* why) {
        TrackerOutput out{};
        if (w.open) {
            Close(out, why);
        }
        charge = Charge::None;
        charge_hold = 0.0f;
        return out;
    }

    TrackerOutput Step(const TrackerInput& in, const TrackerConfig& cfg) {
        TrackerOutput out{};
        hold_ago = in.hold_ago;
        hold_s = cfg.hold_s;
        t += std::clamp(in.dt, 0.0f, 0.1f);
        bt += static_cast<double>(std::clamp(in.dt, 0.0f, 10.0f)); // the budget's: real time
        // The pool's size is its owner's (a quick weapon's 3): a tracker paying from another's
        // (PayFrom: the left hand's) never sets it, or the two would flip it every frame (a whip
        // form's left reads the game's recovery, the right its own) and each flip empties it.
        if (cfg.budget.enabled && pool == nullptr) {
            Pool().SetCapacity(BudgetCapacity(cfg.budget, in.light_pace));
        }
        StepCharge(in, cfg, out);
        if (in.hit_serial != 0 && in.hit_serial == w.serial && !w.hit) {
            w.hit = true;
            if (!w_counted) {
                w_counted = true;
                last_counted_t = w.open_t;
            }
        }
        if (w.open) {
            w.tip_speed = in.tip_speed;
            w.tip_peak = std::max(w.tip_peak, in.tip_speed);
        }
        if (w.open && !in.swings) {
            Close(out, "swings off");
        }
        if (w.powered) {
            // Give the stationary capsule time to reach the game thread. Motion during this
            // short impulse cannot replace it with a second attack or extend its lifetime.
            if (w.open && t - w.open_t >= 0.20f) {
                Close(out, "powered discharge ended");
            }
            if (w.open || out.closed) {
                out.charge = charge;
                return out;
            }
        }
        // An onset window commits or is aborted. One whose blade already hit something at speed
        // is a swing whatever the swing gate says later (the gate is often still measuring).
        if (w.open && !w.committed) {
            if (w.hit || (in.fired && (in.fired_stroke == w.stroke || in.fired_stroke == 0))) {
                Commit(cfg, out);
            } else if (in.gate == Kind::Ignored ||
                       (in.gate == Kind::Rejected && cfg.commit_tip <= 0.0f)) {
                // A whip's "too soon" (the detector's 0.35 s since its last fire) is left to
                // the chain's tip below: a quick backhand after a forehand is a lash of its own
                // (2026-10-16: 99% of them never registered).
                Close(out, in.gate == Kind::Ignored ? "not a swing (ignored)" : "too soon");
            } else if (const bool ended = !in.stroke_live || in.stroke != w.stroke;
                       ended || t - w.open_t > cfg.confirm_s) {
                if (cfg.commit_tip > 0.0f && w.tip_peak >= cfg.commit_tip) {
                    // A whip's lash: the chain is out and flying whatever the swing detector (it
                    // reads a blade's stroke) made of the arm.
                    Commit(cfg, out);
                } else {
                    Close(out, ended ? "the stroke ended before the commit" : "no commit");
                }
            }
        }
        // A commit with no window of its own: a new one (the previous stroke's ends). A commit
        // that follows a window which hit, in the same motion (the classifier split it into two
        // strokes), stays that window: one hit list, one stamina cost for one motion.
        if (cfg.commit_tip <= 0.0f && in.fired && in.swings && w.open && w.hit &&
            w.stroke != in.fired_stroke && t - w.open_t < cfg.merge_s) {
            // (A whip's next stroke is a lash of its own, never the last one's.)
            w.stroke = in.fired_stroke != 0 ? in.fired_stroke : w.stroke;
        }
        if (in.fired && in.swings && !(w.open && w.committed && w.stroke == in.fired_stroke)) {
            if (!(w.open && !w.committed)) { // (an onset window of this stroke committed above)
                if (w.open) {
                    Close(out, "a new swing");
                }
                Open(in, cfg, out, true, in.fired_stroke != 0 ? in.fired_stroke : in.stroke,
                     "commit");
            }
        }
        // A stroke's start (the combo budget, not a minimum interval, sets the rate).
        const float min_interval = cfg.budget.enabled ? 0.0f : cfg.min_interval;
        // A whip's new stroke takes over from a committed window of the last one still winding
        // down (a quick reversal never stays slow long enough for it to close).
        const bool whip_takeover =
            cfg.commit_tip > 0.0f && w.open && w.committed && in.stroke != w.stroke;
        if ((!w.open || whip_takeover) && cfg.onset && in.swings && in.stroke_live && !in.recovery &&
            in.stroke != onset_stroke && in.blade_rate >= cfg.onset_rate &&
            in.hand_speed >= cfg.onset_hand && t - last_counted_t >= min_interval &&
            DirectionOk(in)) {
            onset_stroke = in.stroke;
            if (w.open) {
                Close(out, "a new stroke");
            }
            Open(in, cfg, out, false, in.stroke, "onset");
        }
        // Open while the blade (or the hand) is still fast.
        if (w.open) {
            const bool fast = in.blade_rate >= std::max(cfg.keep_rate, cfg.keep_frac * w_peak) ||
                              in.hand_speed >= cfg.keep_hand;
            if (in.stroke_live && in.stroke == w.stroke) {
                w_peak = std::max(w_peak, in.stroke_peak);
            }
            w_peak = std::max(w_peak, in.blade_rate);
            slow = fast ? 0.0f : slow + std::clamp(in.dt, 0.0f, 0.1f);
            if (slow >= cfg.end_hold) {
                Close(out, w.committed ? "slowed" : "slowed before the commit");
            } else if (t - w.open_t >= cfg.max_open) {
                Close(out, w.committed ? "maximum duration" : "maximum duration before the commit");
            }
        }
        // The classifier's light / heavy for this window's stroke (a wound-up stroke's decision
        // comes up to 0.3 s after its commit, or with it).
        if ((in.kind == Kind::Light || in.kind == Kind::Heavy) && w.serial != 0 &&
            !w.verdict_known && (in.kind_stroke == w.stroke || in.kind_stroke == 0) &&
            (w.open || t - w.close_t < 0.35f)) {
            w.verdict_known = true;
            w.verdict = in.kind == Kind::Heavy ? MoveKind::Heavy : MoveKind::Light;
            out.verdict = true;
            // A wound-up swing judged light holds a light's charges and refill (from when it was
            // used; a heavy of one charge too, BudgetConfig::heavy_cost 1: its refill is a light's).
            if (cfg.budget.enabled && w.kind == MoveKind::Heavy && w.verdict == MoveKind::Light &&
                !w.armed && w.charges > 0) {
                const float refill = BudgetRefill(w_recover[0], cfg.budget) * w_refill_mult;
                if (Pool().Rebill(Key(w.serial), 1, refill, Clock())) {
                    w.charges = 1;
                    w.refill_s = refill;
                    w.left = static_cast<std::uint8_t>(Pool().Available(Clock()));
                    out.refunded = true;
                }
            }
        }
        out.charge = charge;
        return out;
    }

private:
    // Not a pull-back toward the body, not a raise (the swing gate's own direction tests).
    static bool DirectionOk(const TrackerInput& in) {
        const V3 v = in.hand_v;
        const float speed = Length(v);
        if (speed < 1e-3f) {
            return false;
        }
        if (v.y > 0.75f * speed) {
            return false;
        }
        const float flat = std::sqrt(v.x * v.x + v.z * v.z);
        const float forward =
            std::sqrt(in.fwd_flat.x * in.fwd_flat.x + in.fwd_flat.z * in.fwd_flat.z);
        if (flat > 0.1f * speed && forward > 0.1f && -v.y < flat &&
            (v.x * in.fwd_flat.x + v.z * in.fwd_flat.z) / (flat * forward) < -0.8f) {
            return false;
        }
        return true;
    }

    void StepCharge(const TrackerInput& in, const TrackerConfig& cfg, TrackerOutput& out) {
        const float dt = std::clamp(in.dt, 0.0f, 0.1f);
        if (in.trigger) {
            charge_hold += dt;
            if (charge_hold >= cfg.charge_full_s && charge != Charge::Full) {
                charge = Charge::Full;
                out.charge_full = true;
            } else if (charge_hold >= cfg.charge_heavy_s && charge == Charge::None) {
                charge = Charge::Heavy;
                out.charge_heavy = true;
            }
            released_t = t;
        } else {
            charge_hold = 0.0f;
            if (charge != Charge::None && t - released_t > cfg.charge_keep_s) {
                charge = Charge::None;
            }
        }
    }

    void Open(const TrackerInput& in, const TrackerConfig& cfg, TrackerOutput& out, bool committed,
              unsigned stroke, const char* why) {
        const float since = t - last_counted_t;
        if (w_counted) {
            // The last counted lash, as it ended (flurry_s): a wound-up one the classifier
            // judged light is a light.
            last_counted_kind = w.verdict_known && w.verdict == MoveKind::Light && !w.armed
                                    ? MoveKind::Light
                                    : w.kind;
        }
        w = {};
        w.serial = ++serial;
        w.open = true;
        w.committed = committed;
        w.stroke = stroke;
        w.cocked = in.cocked;
        w.open_t = t;
        w.tip_speed = in.tip_speed;
        w.tip_peak = in.tip_speed;
        w.since_counted = std::min(since, 10.0f);
        w.why = why;
        w.kind = charge == Charge::Full                 ? MoveKind::Charged
                 : charge == Charge::Heavy || in.cocked ? MoveKind::Heavy
                                                        : MoveKind::Light;
        if (charge != Charge::None) {
            // Trigger-armed: the classifier's light / heavy does not change it.
            w.armed = true;
            w.verdict_known = true;
            w.verdict = w.kind;
            if (!in.trigger) {
                charge_used = charge;
                charge = Charge::None; // used up (held: every swing keeps it)
            }
        } else if (!in.cocked) {
            w.verdict_known = true; // a stroke that did not start wound up is light at once
            w.verdict = MoveKind::Light;
        } else if (cfg.flurry_s > 0.0f && since < cfg.flurry_s &&
                   last_counted_kind != MoveKind::Light) {
            // Wound up right after a heavy lash (a whip form's flurry): light at once, and the
            // classifier's verdict does not make it heavy again.
            w.kind = MoveKind::Light;
            w.verdict_known = true;
            w.verdict = MoveKind::Light;
            w.flurry = true;
        }
        w_peak = in.stroke_peak;
        w_counted = false;
        slow = 0.0f;
        w_recover = in.recover;
        w_light_pace = in.light_pace;
        w_top = in.top;
        w.flick = t - flick_t <= flick_attach;
        if (w.flick) {
            flick_t = -10.0f; // that transform's stroke (one)
        }
        Bill(cfg);
        out.opened = true;
        if (committed) {
            Commit(cfg, out);
        }
    }

    // The combo budget pays for the window (its kind's charges), or the window is unpaid.
    void Bill(const TrackerConfig& cfg) {
        const BudgetConfig& b = cfg.budget;
        w.unpaid = false;
        if (!b.enabled) {
            return;
        }
        ComboBudget& pay = Pool();
        const double now = Clock();
        if (pool == nullptr) {
            pay.SetCapacity(BudgetCapacity(b, w_light_pace)); // (the owner's, as in Step)
        }
        w.capacity = static_cast<std::uint8_t>(pay.Capacity());
        const int free = pay.Available(now);
        if (w.flick) {
            w.left = static_cast<std::uint8_t>(free); // the transform's stroke: free
            return;
        }
        // The move the game will open (ChooseMove: no heavier row, the lighter one): its cost. A
        // wound-up swing the classifier already judged light (paid late: nothing opened yet) is
        // the light it is.
        if (w.verdict_known && w.verdict == MoveKind::Light && !w.armed) {
            w.kind = MoveKind::Light;
        }
        MoveKind kind = std::min(w.kind, w_top);
        int need = BudgetCost(kind, pay.Capacity(), b.heavy_cost);
        const bool hold = hold_ago >= 0.0f && std::abs(hold_ago - (t - w.open_t)) < hold_s;
        if (free < need && kind == MoveKind::Heavy && !w.armed && free >= 1 && !hold) {
            kind = MoveKind::Light; // wound up with one charge left: a light swing
            need = 1;
        }
        // A quick weapon's charges past the first two: the third waits a moment after the swing
        // before it, and comes back slower.
        const bool past_two = pay.Capacity() > std::max(b.charges, 1) &&
                              pay.Capacity() - free >= std::max(b.charges, 1);
        const double since = pay.SinceLastUse(now);
        if (free >= need && past_two && since < b.quick_gap_s) {
            w.unpaid = true;
            w.charges = 0;
            w.left = static_cast<std::uint8_t>(free);
            w.next_s = static_cast<float>(b.quick_gap_s - since);
            return;
        }
        if (free < need) {
            w.unpaid = true;
            w.charges = 0;
            w.left = static_cast<std::uint8_t>(free);
            w.next_s = pay.NextIn(now);
            return;
        }
        if (kind != std::min(w.kind, w_top)) {
            w.downgraded = true;
            w.kind = kind;
            w.verdict_known = true;
            w.verdict = kind;
        }
        w_refill_mult = past_two ? std::max(b.quick_last_scale, 1.0f) : 1.0f;
        w.refill_s = BudgetRefill(w_recover[static_cast<std::size_t>(kind)], b) * w_refill_mult;
        pay.Spend(Key(w.serial), need, w.refill_s, now);
        w.charges = static_cast<std::uint8_t>(need);
        w.left = static_cast<std::uint8_t>(free - need);
        w.next_s = 0.0f;
    }

    void Commit(const TrackerConfig& cfg, TrackerOutput& out) {
        w.committed = true;
        out.committed = true;
        if (!w_counted) {
            w_counted = true;
            last_counted_t = w.open_t;
        }
        if (w.unpaid && !w.flick) {
            // Opened at the stroke's start with nothing free: a charge back since pays for it.
            Bill(cfg);
            w.late = !w.unpaid;
            out.paid_late = !w.unpaid;
            out.empty = w.unpaid;
        }
    }

    void Close(TrackerOutput& out, const char* why) {
        w.open = false;
        w.close_t = t;
        w.why = why;
        w.aborted = !w.committed;
        out.closed = true;
        out.aborted = w.aborted;
        if (w.aborted && !w.hit && w.charges > 0 && Pool().Refund(Key(w.serial), Clock())) {
            // Not a swing (a wind-up, a reach, a gesture): its charges come back.
            w.charges = 0;
            w.left = static_cast<std::uint8_t>(Pool().Available(Clock()));
            out.refunded = true;
        }
        if ((w.aborted || w.unpaid) && !w.hit && charge_used != Charge::None &&
            charge == Charge::None) {
            // A wind-up the gate refused (or a swing the budget could not pay) does not use up
            // the trigger's charge: the strike does.
            charge = charge_used;
            released_t = t;
        }
        charge_used = Charge::None;
    }

    // The budget this tracker pays from (PayFrom), its clock, and a window's key in it: a pooled
    // tracker's serials get the top bit (the owner's own never get that high).
    static constexpr std::uint32_t PooledKey = 0x80000000u;
    ComboBudget& Pool() {
        return pool != nullptr ? pool->budget : budget;
    }
    const ComboBudget& Pool() const {
        return pool != nullptr ? pool->budget : budget;
    }
    double Clock() const {
        return pool != nullptr ? pool->bt : bt;
    }
    std::uint32_t Key(std::uint32_t s) const {
        return pool != nullptr ? s | PooledKey : s;
    }

    Window w{};
    std::uint32_t serial{0};
    float t{0.0f};
    float last_counted_t{-10.0f};
    MoveKind last_counted_kind{MoveKind::Light}; // the kind of that window (flurry_s)
    bool w_counted{false};
    float w_peak{0.0f};
    float slow{0.0f};
    unsigned onset_stroke{0};
    Charge charge{Charge::None};
    Charge charge_used{Charge::None}; // the charge the open window used up
    float charge_hold{0.0f};
    float released_t{-10.0f};
    ComboBudget budget{};
    double bt{0.0}; // the budget's clock (s)
    WindowTracker* pool{nullptr}; // PayFrom: the tracker whose budget this one pays from
    float hold_ago{-1.0f};        // TrackerInput::hold_ago, this step's ...
    float hold_s{0.15f};          // ... and TrackerConfig::hold_s
    float flick_t{-10.0f}; // the newest transform flick's squeeze (tracker clock)
    float flick_attach{0.35f};
    MoveKind w_top{MoveKind::Charged};
    float w_refill_mult{1.0f}; // the window's refill factor (a quick weapon's third charge)
    float w_light_pace{0.0f};  // TrackerInput::light_pace at the window's opening
    std::array<float, 3> w_recover{RecoverSeconds(MoveKind::Light), RecoverSeconds(MoveKind::Heavy),
                                   RecoverSeconds(MoveKind::Charged)};
};

// ---- The telegraph: enemies may read a big swing ------------------------------------------------
//
// A physical swing plays no attack animation, so by itself it gives the enemies nothing to read.
// The game tells its AIs about an attack in two messages, both sent when a vanilla attack starts
// (core/vr/bloodborne_melee.cpp): "the target attacks" (PlayerIns vt+0x440, AI message 0: the AI's
// FindAttack interrupt 0.2-0.7 s later, which input-reading enemies answer with a step, a guard or
// a counter) and "parry timing" (vt+0x448, message 7: INTERUPT_ParryTiming). melee_telegraph=heavy
// sends them for the swings a player can see coming: a wind-up (the blade raised, cocked back or
// taken out wide over melee_telegraph_arc_deg in one stroke), a strike that starts wound up or is
// trigger-armed (heavy / charged), a long one (its window still open after melee_telegraph_long_s)
// and a wide one (the stroke sweeping the classifier's heavy arc). A quick light flick from guard
// never tells (a deliberate small advantage). `all`: every swing tells, as every vanilla attack
// does; `off`: none. One telegraph covers the swing it announces: no other for TelegraphGap and
// while that swing's window is still open (a vanilla attack chain notifies once, and re-arming the
// AI's reaction timer every few frames would keep it from ever firing).

enum class TelegraphMode : std::uint8_t { Off, Heavy, All };

inline const char* TelegraphModeName(TelegraphMode m) {
    switch (m) {
    case TelegraphMode::Off:
        return "off";
    case TelegraphMode::Heavy:
        return "heavy";
    case TelegraphMode::All:
        return "all";
    }
    return "?";
}

struct TelegraphConfig {
    TelegraphMode mode{TelegraphMode::Heavy};
    float windup_arc{60.0f * Deg}; // rad: a wind-up stroke this long (melee_telegraph_arc_deg)
    float wide_arc{140.0f * Deg};  // rad: a strike sweeping this much (the classifier's heavy arc)
    float long_s{0.40f};           // a window still open after this long (melee_telegraph_long_s)
    float gap_s{0.80f};            // TelegraphGap: no new telegraph sooner
};

struct TelegraphInput {
    float dt{0.0f};
    bool swings{false}; // swings may attack now, and are the game's attack (physical)
    // The classifier this frame (core/vr/swing_detector.h).
    bool stroke_live{false};
    unsigned stroke{0};
    const char* windup{nullptr}; // the stroke takes the blade away from the front (a wind-up)
    float stroke_arc{0.0f};      // rad, swept so far
    float blade_angle{0.0f};     // rad, the blade from horizontal-forward now
    bool wound_up{false};        // the weapon is held wound up (WoundUpPose)
    // The window after this frame's WindowTracker::Step.
    const Window* window{nullptr};
    bool opened{false}; // it opened this frame
    bool spin{false};   // ... as a spun whip's revolution (OpenRevolution): a wide swing
};

struct TelegraphOutput {
    bool fire{false};
    const char* why{""}; // a static string
};

class TelegraphTracker {
public:
    void Reset() {
        active = false;
        covers = 0;
        pose_prev = false;
        swings_prev = false;
    }

    TelegraphOutput Step(const TelegraphInput& in, const TelegraphConfig& cfg) {
        t += std::clamp(in.dt, 0.0f, 0.1f);
        const Window* w = in.window;
        const bool open = w != nullptr && w->open;
        if (open && w->serial != seen_serial) {
            seen_serial = w->serial;
            seen_open_t = t;
        }
        // Raised slowly (no stroke): a stroke's own follow-through ending out wide is not one.
        const bool pose_entered =
            in.wound_up && !pose_prev && swings_prev && !in.stroke_live && !open;
        pose_prev = in.wound_up;
        swings_prev = in.swings;
        if (cfg.mode == TelegraphMode::Off || !in.swings) {
            active = false;
            covers = 0;
            return {};
        }
        if (active) {
            // A telegraph made before its swing's window covers the next one to open.
            if (covers == 0 && open && seen_open_t >= fired_t) {
                covers = w->serial;
            }
            const bool covered_open = covers != 0 && open && w->serial == covers;
            if (t - fired_t >= cfg.gap_s && !covered_open) {
                active = false;
                covers = 0;
            } else {
                return {};
            }
        }
        const char* why = nullptr;
        // A wind-up takes the blade high, back or out wide (WindupPoseAngle from the front): the
        // weapon brought back up to guard after a chop is not one.
        if (in.stroke_live && in.windup != nullptr && in.stroke_arc >= cfg.windup_arc &&
            (in.wound_up || in.blade_angle >= WindupPoseAngle)) {
            why = in.windup[0] == 'r'   ? "a wind-up (raised)"
                  : in.windup[0] == 'c' ? "a wind-up (cocked back)"
                                        : "a wind-up (out wide)";
        } else if (pose_entered) {
            why = "the weapon raised into a wound-up hold";
        } else if (in.opened && open && w->kind != MoveKind::Light) {
            why = w->kind == MoveKind::Charged ? "a charged swing" : "a heavy swing";
        } else if (in.opened && open && in.spin) {
            why = "a whip spin (a wide swing)";
        } else if (open && t - seen_open_t >= cfg.long_s) {
            why = "a long swing";
        } else if (open && in.stroke_live && in.stroke == w->stroke &&
                   in.stroke_arc >= cfg.wide_arc) {
            why = "a wide swing";
        } else if (cfg.mode == TelegraphMode::All && in.opened && open) {
            why = "a swing";
        }
        if (why == nullptr) {
            return {};
        }
        active = true;
        fired_t = t;
        covers = open ? w->serial : 0;
        return {true, why};
    }

private:
    float t{0.0f};
    bool active{false};
    float fired_t{-10.0f};
    std::uint32_t covers{0}; // the window the telegraph announced (0: the next one to open)
    std::uint32_t seen_serial{0};
    float seen_open_t{0.0f}; // when the newest window was first seen open
    bool pose_prev{false};
    bool swings_prev{false};
};

// ---- The game's side: one attack per window ----------------------------------------------------

enum class LatchStep : std::uint8_t {
    Idle,    // nothing to do this frame
    Wait,    // a new window waits (the visceral check first)
    Create,  // call InvokeAttack: creates the attack (stamina, a new hit list)
    Refresh, // call it again: keeps it alive this frame
    End,     // stop calling: the game ends it this frame
    Deny,    // a new window the gates refuse
};

inline const char* LatchStepName(LatchStep s) {
    switch (s) {
    case LatchStep::Idle:
        return "idle";
    case LatchStep::Wait:
        return "wait";
    case LatchStep::Create:
        return "create";
    case LatchStep::Refresh:
        return "refresh";
    case LatchStep::End:
        return "end";
    case LatchStep::Deny:
        return "deny";
    }
    return "?";
}

struct LatchState {
    std::uint32_t serial{0}; // the window served (or the last one)
    bool active{false};      // our attack is alive
    bool done{false};        // this window is over (ended, denied, a gap): never again
    std::uint32_t frames{0}; // game frames it has been alive
    std::uint32_t waited{0}; // game frames a new window has waited (Wait)
    bool waiting_for_payment{false};
};

struct LatchInput {
    bool have{false}; // a window was published (and is fresh)
    std::uint32_t serial{0};
    bool open{false};
    bool stale{false}; // closed long ago (a window from before a load or a stall)
    bool gates{false}; // everything allows an attack now
    bool ready{true};  // the visceral check saw this window (else wait up to max_wait)
    std::uint32_t min_frames{2};
    std::uint32_t max_wait{2};
    bool paid{true}; // false: wait for the combo budget, without the ready/max_wait timeout
};

inline LatchStep StepLatch(LatchState& s, const LatchInput& in) {
    if (!in.have) {
        if (s.active) {
            s.active = false;
            s.done = true;
            return LatchStep::End;
        }
        return LatchStep::Idle;
    }
    if (in.serial != s.serial) {
        // A new window: the old attack (if any) ends by not being called again (its slot is not
        // refreshed: the game ends it this frame); the new one uses the other slot.
        s = {};
        s.serial = in.serial;
        if (in.stale || (!in.open && in.min_frames == 0)) {
            s.done = true;
            return LatchStep::Idle;
        }
    }
    if (s.done) {
        return LatchStep::Idle;
    }
    if (!s.active) {
        if (in.stale && s.waiting_for_payment) {
            s.done = true;
            return LatchStep::Idle;
        }
        if (!in.ready && s.waited < in.max_wait) {
            ++s.waited;
            return LatchStep::Wait;
        }
        if (!in.gates) {
            s.done = true;
            return LatchStep::Deny;
        }
        if (!in.paid) {
            s.waiting_for_payment = true;
            return LatchStep::Wait;
        }
        s.waiting_for_payment = false;
        s.active = true;
        s.frames = 1;
        return LatchStep::Create;
    }
    if (!in.gates || !in.paid) {
        s.active = false;
        s.done = true; // a gap ends it for good: never a second attack (stamina, hit list)
        return LatchStep::End;
    }
    if (in.open || s.frames < in.min_frames) {
        ++s.frames;
        return LatchStep::Refresh;
    }
    s.active = false;
    s.done = true;
    return LatchStep::End;
}

// ---- The swing buffer (vr.ini swing_buffer_ms) ------------------------------------------------
//
// A new swing the gates refuse only because the game takes no attack yet (a kept stagger's attack
// window about to open: MeleeBody::attack_wait) waits up to `buffer_ns` for it (the latch's Wait),
// as the game buffers R1 and as the gun (gun_buffer_ms) and the transform (transform_buffer_ms)
// wait: the moment the game takes attacks again its attack opens on the swing still under way
// (the motion went on; its damage starts then). Past the time, once the swing's window has closed,
// or for any other refusal (a menu, a grab, a cinematic, death, the game's arm...): refused as
// before, with the deny tick. A swing already attacking is never held (a gap still ends it).
struct SwingBuffer {
    std::uint32_t serial{0};  // the window held (0: none yet)
    std::int64_t since_ns{0}; // when it was first held
    // Whether window `window` waits this tick. `pending`: its attack is neither created nor
    // refused yet; `only_wait`: the gates' one refusal is "the game takes no attack yet".
    bool Hold(std::uint32_t window, bool pending, bool only_wait, bool open, std::int64_t now,
              std::int64_t buffer_ns) {
        if (!pending || !only_wait || !open || buffer_ns <= 0) {
            return false;
        }
        if (window != serial) {
            serial = window;
            since_ns = now;
        }
        return now - since_ns < buffer_ns;
    }
    // How long window `window` has been held (0: never).
    std::int64_t HeldNs(std::uint32_t window, std::int64_t now) const {
        return serial != 0 && window == serial ? now - since_ns : 0;
    }
};

// ---- A flick mid-swing: the stroke becomes the transform attack ---------------------------------
//
// The weapon-only transform (core/vr/transform.h) snaps the weapon into its other form when the
// right grip is squeezed during a swing or with the hand moving (a flick). The window of that
// stroke ends its old-form attack as the switch is handed to the game (before the old part's
// dummies leave the blade) and, once the new form is in the hand, opens the game's own transform
// attack (core/vr/melee_moves.h TransformMoves) for the rest of the window: its damage, poise and
// hit capsule on the new form's blade, one new hit list.
//
// FlickJoins: whether window `window` is the flick's stroke: open at the squeeze, or opened while
// the transform had not switched yet, or within `attach_ns` of the squeeze (the stroke's start
// came a moment after the squeeze).
struct FlickJoinInput {
    bool enabled{false};      // physical melee, the transform's flick on
    std::uint32_t window{0};  // the newest window (0: none)
    bool open{false};         // ... open now
    std::int64_t open_ns{0};  // when it opened (steady clock, the presenter's = the game's)
    std::int64_t close_ns{0}; // when it closed (0: open)
    std::uint32_t run{0};     // the newest transform run (0: none)
    bool run_active{false};
    bool switched{false};     // its form switch happened
    std::int64_t press_ns{0}; // its squeeze
    std::int64_t now{0};
    std::int64_t attach_ns{350'000'000};
    std::int64_t max_age_ns{1'500'000'000}; // a run this old joins nothing
};

inline bool FlickJoins(const FlickJoinInput& in) {
    if (!in.enabled || in.window == 0 || in.run == 0 || in.now - in.press_ns > in.max_age_ns) {
        return false;
    }
    if (in.close_ns != 0 && in.close_ns < in.press_ns) {
        return false; // over before the squeeze
    }
    if (in.open_ns <= in.press_ns) {
        return true; // open at the squeeze
    }
    // Opened after it: while the transform had not switched yet, or right after the squeeze.
    return (in.run_active && !in.switched) || in.open_ns - in.press_ns <= in.attach_ns;
}

// ---- The blade's bite ---------------------------------------------------------------------------

// How much of the controller's motion the hunter's hand follows `since_s` after a hit: `depth` for
// `hold_s`, then back to 1 over `ease_s` (smoothstep). 1 = no bite.
inline float BiteFollow(float since_s, float hold_s, float ease_s, float depth) {
    if (since_s < 0.0f) {
        return 1.0f;
    }
    if (since_s < hold_s) {
        return depth;
    }
    if (ease_s <= 0.0f || since_s >= hold_s + ease_s) {
        return 1.0f;
    }
    return depth + (1.0f - depth) * Smoothstep(0.0f, 1.0f, (since_s - hold_s) / ease_s);
}

// ---- The stamina of a physical swing (melee_stamina_pct) ----------------------------------------
//
// A physical swing (light, heavy, charged, form B, a flick's transform attack, the physical whip)
// Light attacks cost `pct` percent (melee_stamina_pct, 50 by default) of what the game's own attack costs:
// - 1 .. 99: InvokeAttack's CreateAttack takes the row's full stamina, exactly as for the game's own
//   attack (down to -50), and in the same call all but pct% of that drop goes back (PartialSwing;
//   nothing reads the stamina in between, so the gauge only ever shows the pct% dip). A swing at 0
//   or below is refused ("no stamina"), as the game refuses its own; the regen is not paused (a
//   tenth of the cost with the game's whole pause would not feel like a tenth).
// - 100: the game's own cost; with `regen_pause` (melee_regen_pause) no stamina comes back for the
//   move's recovery.
// - 0: free. InvokeAttack runs with the game's own per-character no-stamina-consumption flag
//   (ChrData +0x200 bit 0x80, which every stamina subtraction of the game checks) set for the call
//   and put back after it, so no cost is created at all; nothing is refused for stamina and the
//   regen is never paused. Should the stamina still drop (the flag's bytes did not match), the
//   value before goes back in the same call: the gauge never dips.
// Dodges, sprint, jumps, the gun and the game's own canned attacks are not physical swings and keep
// the game's stamina either way.
struct SwingStamina {
    int pct{100};            // the share of the game's cost a swing pays (0 .. 100)
    bool charge{true};       // InvokeAttack may take the row's stamina (pct > 0; else the flag)
    bool refuse_empty{true}; // a swing at 0 or below is refused (pct > 0)
    bool regen_pause{true};  // the regen paused for the move's recovery (pct 100 only)
};

constexpr SwingStamina SwingStaminaOf(int pct, bool regen_pause) {
    const int p = pct < 0 ? 0 : pct > 100 ? 100 : pct;
    return {p, p > 0, p > 0, p >= 100 && regen_pause};
}
constexpr SwingStamina AttackStaminaOf(MoveKind kind, int light_pct, bool regen_pause) {
    return SwingStaminaOf(kind == MoveKind::Light ? light_pct : 100, regen_pause);
}

// (A bool would be 0 or 1 percent: the old melee_stamina=1 is 100, see MeleeSettings::stamina_pct.)
SwingStamina SwingStaminaOf(bool, bool) = delete;

// The game's floor of the stamina (InvokeAttack's cost block clamps it to -50 .. max).
inline constexpr std::int32_t StaminaFloor = -50;

// pct% of a full cost: rounded to the nearest and at least 1 of a cost above 0 (10% of a 7 is 1:
// a partial swing is never free); nothing of a cost of 0 or at pct 0; the whole cost at 100.
constexpr std::int32_t PartialCost(std::int32_t full, int pct) {
    if (full <= 0 || pct <= 0) {
        return 0;
    }
    if (pct >= 100) {
        return full;
    }
    const std::int32_t part = (full * pct + 50) / 100;
    return part < 1 ? 1 : part;
}

// What a paying swing's InvokeAttack leaves (PartialSwing).
struct SwingPaid {
    std::int32_t stamina{0}; // the stamina to leave after the call
    std::int32_t full{0};    // the game's full cost of the call
    std::int32_t paid{0};    // what the swing pays of it (stamina before - `stamina`)
};

// The game took its full cost (`before` -> `after`); all but pct% of it goes back. The floor (-50)
// may have clipped the game's drop: then the row's own cost `row_cost` (BehaviorParam stamina) is
// the full one when it is larger. pct 100 = the game's own (`after`), pct 0 = nothing (`before`,
// as a free swing). Nothing taken (or the stamina rose meanwhile): left as it is.
constexpr SwingPaid PartialSwing(std::int32_t before, std::int32_t after, int pct,
                                 std::int32_t row_cost) {
    if (after >= before) {
        return {after, 0, 0};
    }
    std::int32_t full = before - after;
    if (after <= StaminaFloor && row_cost > full) {
        full = row_cost;
    }
    if (pct >= 100) {
        return {after, full, before - after};
    }
    std::int32_t left = before - PartialCost(full, pct);
    left = left < StaminaFloor ? StaminaFloor : left;
    return {left, full, before - left};
}

// Whether a swing is refused for the game's stamina now.
constexpr bool StaminaRefuses(const SwingStamina& s, std::int32_t stamina) {
    return s.refuse_empty && stamina <= 0;
}

// ChrData +0x200: the no-stamina-consumption bit, set for a free swing's InvokeAttack ...
inline constexpr std::uint8_t NoStaminaBit = 0x80;
constexpr std::uint8_t FreeSwingFlags(std::uint8_t flags) {
    return static_cast<std::uint8_t>(flags | NoStaminaBit);
}
// ... and after it the bit as it was before the call (the other bits as the call left them).
constexpr std::uint8_t RestoredFlags(std::uint8_t now, std::uint8_t before) {
    return static_cast<std::uint8_t>((now & ~NoStaminaBit) | (before & NoStaminaBit));
}

// The stamina a free swing's InvokeAttack leaves: never less than `before`.
constexpr std::int32_t FreeSwingStamina(std::int32_t before, std::int32_t after) {
    return after < before ? before : after;
}

} // namespace VR::Melee
