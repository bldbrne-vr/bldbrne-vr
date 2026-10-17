// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <iosfwd>
#include <string>

// Quick items in VR (core/vr/quick_items.h, core/vr/bloodborne_quickitems.h) and the in-frame
// renderer that draws them into the game's own frame (renderer_vulkan/vr_inframe.h): their vr.ini
// keys. Parsed by VR::LoadSettings, which hands every key it does not know to ParseQuickSetting,
// and written with the other defaults when vr.ini is created. Read-only after loading.
namespace VR::Quick {

struct QuickSettings {
    // Game side: the quick-item slots are read every update, and selecting, using and throwing
    // them goes through the game's own functions. 0 = nothing installed (Y is Square).
    bool quick_items{true};
    // Y opens the arc of your quick items above the hunter's left hand. 0 = Y is the game's Square
    // exactly as before (the fallback).
    bool quick_arc{true};
    // A tap of the left stick click (not sprinting, no combo) opens and closes the arc exactly
    // like Y, so an item can be picked with the thumb still on the stick, walking and dodging on
    // (the user's request, 2026-10-05). Sprinting it still jumps and every hold combo stays.
    // 0 = the tap is L3 to the game as before. A release later than l3_tap_ms is no tap (L3).
    bool arc_l3{true};
    float l3_tap_ms{500.0f};
    // left_scheme=2 (vr.ini; the left grip opens and closes the arc, the left trigger takes the
    // card the hand is on): the trigger takes only a card lit this long (ms), so sweeping across
    // the arc with the trigger down never takes or uses anything (a pull on a card lit for less
    // waits for it while the trigger stays down and the hand on that card) ...
    float dwell_ms{100.0f};
    // ... a throwable taken with the trigger stays in the hand while the trigger is held past this
    // (0..1; its pull is the gun's own, 0.45) and flies when it is let go below it, through the
    // same release as the grip's (a slow one puts it back on the arc) ...
    float trigger_release{0.35f};
    // ... and after a throw the arc closes (the gun is back at once). Opt-in: the arc is back where
    // it was for the next one this long (s; a card is taken from it only once lit 0.25 s, and it
    // closes at its time whatever the hand lit; quick_arc_stack=0: off too). Off by default since
    // the review of 2026-10-05: the hand coming back to rest after a throw lands on a card, and
    // the next trigger pull, meant for the gun, took another item.
    float reopen_s{0.0f};
    // left_scheme=2: the left grip squeezed with the hand at the left hip (beside the lap seated)
    // draws a Blood Vial, as X arms one (core/vr/belt_vial.h); 0 = the grip opens the arc there
    // too. belt_vial_zone scales the hip's box (1 = core/vr/belt_vial.h Standing / Seated).
    // Physical inventory: 0 retains the menu / legacy belt, 1 slots (Y still opens the
    // menu), 2 both. Vials stay right; pocket/chest follow equipped quick slots 1/2.
    int holster_mode{2};
    bool holster_haptics{true};
    float holster_height_cm{0.0f}, holster_width_cm{0.0f}, holster_forward_cm{0.0f};
    float holster_reach_cm{12.0f}; // (10 until 2026-10-14: the user, "+2 cm")
    bool belt_vial{true};
    float belt_vial_zone{1.0f};
    float radius_m{0.18f};    // arc radius around the hand (seated: x0.875)
    // The card diameter (seated: x0.9); its icon and count scale with it.
    float card_m{0.064f};
    float span_deg{150.0f};   // end card to end card (six cards 9.3 cm apart)
    // The hover volume around each card: x 6 cm round the fist's centre in the card's plane and
    // 10 cm along the line of sight (6.7 / 11.2 cm). It widens where a card is reached for (out,
    // up, in front, behind); between two neighbours the nearer one is lit and the other takes
    // over 1.5 cm nearer whatever this is (quick_items.h PickHover), so switching stays as easy.
    // A little easier to pick (the user's 2026-10-08 "a tiny bit bigger area of selection" and
    // "a tiny tiny bit bigger icons"): card_m 0.060 -> 0.064, hover_scale 1.0 -> 1.12.
    float hover_scale{1.12f};
    // How the hand picks a card: hovering it with the fist (0), or pointing the hand from where
    // the arc opened toward it, from 7 cm out (1, a pie: no need to reach the card).
    bool pick_sector{false};
    // A squeeze just after the hand left a card still takes it (ms) ...
    float grace_ms{150.0f};
    // ... and one with no card lit takes the card clearly nearest the hand, up to this x the
    // hover volume (10 cm in the card's plane at 1.12; 0 = off).
    float nearest{1.5f};
    // The left weapon is hidden while the arc is open (it covers the cards the hand reaches for).
    // 0: the gun stays in the hand (the cards it covers show through it, hidden_xray / the
    // hovered card hover_xray) and the trigger closes the arc and fires it at the pull.
    bool hide_gun_open{false};
    // The hovered card stays readable where the hunter's own fist covers it: shown through the
    // fingers (faded in with the hover); 0 = hidden behind the hand like the other cards.
    bool hover_xray{true};
    // Every card (and the item name) where the scene hides it - a wall or pillar the arc opened
    // into, the hunter's own arm - is shown through it at this opacity; 0 = hidden.
    float hidden_xray{0.5f};
    bool quick_draw{true};    // Y then the grip at once (no reach) takes the game's selected item
    bool select{true};        // a grab selects that item in the game too (its HUD slot)
    // A use is a deliberate pull of the left trigger, held this long; a shorter tap does nothing
    // (with trigger_parry: the item goes away and the gun gets the press).
    float use_commit_ms{100.0f};
    float use_cooldown_ms{500.0f}; // between two uses
    // Opt-in: the left trigger with a throwable in hand (a deliberate pull), or a tap of it with a
    // usable, puts the item away and fires the gun (L2), a reflex parry. 0 (the user's rule): the
    // trigger never fires the gun while an item is in the hand.
    bool trigger_parry{false};
    bool hide_gun{true};           // the left weapon is hidden while an item is in the hand
    bool labels{true};             // the hovered item's name and the counts
    bool icons{true};              // the game's own item icons on the cards (0 = plain cards)
    float idle_close_s{6.0f};      // an open arc nobody reaches for closes (0 = never)
    // After a throw or a use, a new squeeze of the left grip within regrab_s takes the next one
    // of the same item, without the arc.
    bool stack{true};
    float regrab_s{1.0f};
    // The held item's size: the game's own models are life size (x1); the per-item sizes of
    // core/vr/quick_items.h HeldLookOf are multiplied by this.
    float held_scale{1.0f};
    // The hunter's left fingers close round what the hand holds (a quick item, the vial), per
    // item; 0 = the animation's own hand.
    bool held_grip{true};
    float haptics{1.0f};    // strength of the quick-item haptics (0 = none)
    // Usable items whose effect the game applies at once (Blue Elixir, pellets, coldbloods,
    // papers, ash, NPC bloods, antidotes) are used without the animation, through the game's own
    // UseGoods; 0 = every use plays the game's animation (select + Square).
    bool instant{true};
    // The items the hunter eats or drinks (Beast Blood Pellet, Antidote: eaten; Blue Elixir,
    // Sedative, Lead Elixir: drunk; core/vr/quick_items.h MouthOf) stay in his hand when taken
    // from the arc and are used when you bring them to your mouth (a ball of mouth_radius_m round a
    // point mouth_down_m below and mouth_ahead_m in front of your eyes, kept there mouth_dwell_ms
    // with the hand slowing down); let go of the trigger (left_scheme=1: the grip) away from the
    // mouth and it goes back, nothing used. A gulp or a bite on the left hand, and a drink plays
    // the game's own drinking sound (mouth_sound). 0 = used at the pull, as before.
    bool use_mouth{true};
    float mouth_radius_m{0.10f};
    float mouth_down_m{0.08f};
    float mouth_ahead_m{0.08f};
    float mouth_dwell_ms{150.0f};
    bool mouth_sound{true};
    // Use sounds (the user's 2026-10-06 "and they should play a sound when activated"): an item
    // used at once, without the game's use animation (eaten, drunk, wiped, crushed, held to use,
    // stabbed, the Blood Vial), plays the game's own sound of that use from its use clip, or the
    // closest one where the clip has none (core/vr/quick_items.h UseSoundsOf). Items used with
    // the game's animation (bells, marks, the Music Box) keep its own. 0 = only the drink at the
    // mouth (mouth_sound), as before.
    bool use_sounds{true};
    float use_whoosh{1.0f}; // quiet rising air during hold/crush; 0 = off, 2 = twice as loud
    // Wipe to apply (the user's 2026-10-06 request: "having to apply the firepaper on the weapon
    // itself"): Fire Paper, Bolt Paper and the Empty Phantasm Shell (core/vr/quick_items.h WipeOf)
    // stay in the hunter's left hand when taken from the arc (the trigger held; left_scheme=1: the
    // grip) and are applied when you wipe them along your weapon's blade: the paper within
    // wipe_reach_cm of the blade and moving along it for wipe_share of its length within
    // wipe_window_ms (either way; not a tap, not a cut across it): the game's own buff at once, no
    // animation. A rasp on both hands while it touches, rising with the stroke, and a thump when
    // it takes. Let go away from the blade: back, nothing used. 0 = used at the pull.
    // Easier (the user's 2026-10-08 "make applying the fire paper a little easier (like easier to
    // find the spot where we need to rub)"): a wider zone (12 cm of the blade's axis), a shorter
    // stroke (30% of the blade, at least wipe_min_cm, within 1 s), the paper's own move at least
    // wipe_own of the stroke (a thrust or a swing of the blade past a still paper is no wipe), and
    // a find cue while a paper is in the hand: soft ticks on the paper hand quickening as it
    // nears the blade from wipe_cue_cm away (wipe_cue; on/off: the older values 2 and 3 read as
    // on). The first version: wipe_reach_cm=7 wipe_share=0.4
    // wipe_window_ms=800 wipe_min_cm=8 wipe_own=0 wipe_cue=0. The merge's skeptic: a stroke at
    // most wipe_max_cm (the long forms asked 25 cm; the user's strokes were 20-21) and the blade
    // turning at most wipe_turn_deg over it (a forehand with the paper by the hilt went through:
    // a swing turns the blade, a wipe holds it); the first version: wipe_max_cm=25
    // wipe_turn_deg=0.
    bool use_wipe{true};
    float wipe_reach_cm{12.0f};
    float wipe_share{0.30f};
    float wipe_window_ms{1000.0f};
    float wipe_min_cm{7.0f};
    float wipe_own{0.40f};
    float wipe_max_cm{20.0f};
    float wipe_turn_deg{35.0f};
    bool wipe_cue{true};
    float wipe_cue_cm{30.0f};
    // The rub (the user's 2026-10-08 "I don't want it to trigger when I am not rubbing the actual
    // blade -- I want it to trigger when I RUB the blade"; core/vr/quick_items.h WipeConfig): the
    // paper you see (its card in the fingers) within wipe_touch_cm of the blade's surface (its
    // axis with wipe_blade_cm round it), rubbed along it back and forth: strokes of at least
    // wipe_stroke_cm adding up to wipe_rub_cm within wipe_rub_ms, the blade itself never moving
    // faster than wipe_blade_mps under the paper (a swing or a thrust past a still paper); one long
    // stroke (wipe_share of the blade, wipe_min_cm .. wipe_max_cm) still applies at once.
    // wipe_reach_cm and wipe_window_ms are the one-way stroke's (wipe_rub=0: the morning's).
    bool wipe_rub{true};
    float wipe_blade_cm{4.0f};
    float wipe_touch_cm{3.0f};
    float wipe_rub_cm{18.0f};
    float wipe_rub_ms{1500.0f};
    float wipe_stroke_cm{4.0f};
    float wipe_blade_mps{0.8f};
    // Hold to use (the user's 2026-10-06 request): the misc items whose use cannot be taken back
    // (the coldbloods, Madman's Knowledge, the Blood Dreg, the NPC bloods, the Hunter's Marks, the
    // bells, the Tiny Music Box, ...; core/vr/quick_items.h HoldOf) stay in the hunter's hand when
    // taken from the arc while you keep the trigger (left_scheme=1: the grip) held: a thin ring
    // fills round the item over hold_use_s with a rising vibration, and full the item is used (a
    // small flash and a solid thump). Let go before: it goes back, nothing used (a soft put-back
    // tick). Getting hit, a dodge or a guard meanwhile cancels it the same way. 0 = used at the
    // pull, as before.
    bool hold_use{true};
    float hold_use_s{1.5f};
    // Natural gestures, the faster way for some held items (the user's 2026-10-06 "yes and yes";
    // the hold above stays their fallback; core/vr/quick_items.h GestureOf, StabOf):
    //  - crush: the coldbloods, Madman's Knowledge, Great One's Wisdom, the Blood Dreg and Vermin
    //    are crushed in the fist: squeeze the other button hard (left_scheme=2: the grip, which
    //    then never puts the item back: let go of the trigger for that; 1: the trigger) at least
    //    crush_press for crush_hold_ms. Only a fresh squeeze (that button let go since the take).
    //  - shake: the bells (Beckoning, Small / Sinister Resonant, the Silencing Blank) ring with
    //    shake_strokes back-and-forth strokes within shake_window_ms (a wrist flick of at least
    //    shake_wrist rad/s, or a forearm shake of shake_arm m/s); a clack per stroke, then the
    //    toll and the game's own ring. shake_choir_bell=1 holds and rings the Choir Bell (a
    //    hunter tool, otherwise used at the pull: 5 Quicksilver Bullets safe from a stray pull,
    //    ~0.5 s slower in a fight).
    // Queenly Blood uses crush exclusively (no timed hold fallback). Bottled NPC
    // blood and Iosefka's vial use the mouth. The legacy stab option has no mapped
    // quick-slot items; the regular Blood Vial's dedicated injection is unchanged.
    bool use_crush{true};
    float crush_press{0.90f};
    float crush_hold_ms{150.0f};
    bool use_shake{true};
    bool shake_choir_bell{true};
    int shake_strokes{3};
    float shake_window_ms{900.0f};
    float shake_wrist{5.0f};
    float shake_arm{0.9f};
    bool use_stab{true};
    // The Tiny Music Box played by hand (the user's 2026-10-06 request; core/vr/music_box.h): taken
    // from the arc it stays in the left hand while the trigger (left_scheme=1: the grip) is held
    // and the RIGHT hand is free: the right weapon is put away (no animation), swings and the
    // right grip's transform are off. Squeeze the right grip on the box's little crank (within
    // music_box_grab_cm of its knob) and turn it: a ratchet tick every quarter turn, and after
    // one full turn the box plays: the game's own use of it (what makes Father Gascoigne stagger)
    // and its tune, no animation, you keep moving. Turning on plays it again once the tune is over
    // (music_box_replay_s). Let go of the box (the trigger) and the weapon is back in his right
    // hand at once. 0 = held to use (the ring, then the game's own animation), as before.
    bool music_box_crank{true};
    float music_box_grab_cm{6.0f};
    float music_box_turns{1.0f};
    float music_box_replay_s{4.9f};
    // Throwables: swing and let go of the grip; the game's own projectile flies with your hand's
    // release velocity. 0 = they are used like the rest (select + Square: the game's own throw).
    bool throws{true};
    float throw_min_speed{1.5f}; // m/s hand speed at release to throw (slower = put back)
    // Real ballistics (your direction, real gravity) with a VR boost: launch speed per hand speed
    // (small class factors on top, no minimum: a weak flick drops short). A throw in the headset
    // moves the hand far slower than a real one (no weight to drive; the user's first real
    // throw, 2026-10-04: 2.5 m/s with a full wind-up and wrist snap, where a real overhand leaves
    // at 10-15 m/s): at 1.2 it fell 1.7 m away, at 4 it leaves at ~10 m/s ...
    float throw_gain{4.0f};
    // ... through this power curve about 3 m/s (1 = linear; 0.8: a natural 2 m/s flick +8%, a
    // hard 5 m/s throw -10%, so a relaxed throw carries and a hard one has room under the cap) ...
    float throw_gain_curve{0.8f};
    // ... read from the grip located at each input frame's own time (the runtime's freshest
    // estimate; 0 = the poses predicted for the display time, 41-50 ms ahead over Link, which
    // read the user's real throws about 30% slow) ...
    bool throw_fresh_pose{true};
    // ... and when the grip opens while the hand still speeds up, the throw waits up to this long
    // (ms, the item still in the hand) for its peak (Valve's rule; 0 = at the release) ...
    float throw_rise_wait_ms{25.0f};
    // ... using the hand's strongest velocity of this long before the grip opened when it beats
    // the release velocity (the hand is already slowing at full extension; 0 = release only),
    // its direction at most throw_peak_turn_deg from the release's
    float throw_peak_window_s{0.30f};
    float throw_peak_turn_deg{20.0f};
    // A throw from flat up to 15 degrees (and down to 30) leaves this much higher, fading out by
    // 40 up and 60 down: in the headset a throw aimed at something at eye level drops short (no
    // weight in the hand, real gravity, the distance underestimated); the game's own throw leaves
    // 10 degrees above the hunter's facing for the same reason. 0 = your exact direction.
    float throw_lift_deg{6.0f};
    // ... a straight push from in front of the chest gets this share of it, a wind-up from the
    // shoulder, a whip of the wrist or an arm swung round all of it (blended)
    float throw_push_factor{0.85f};
    // ... taken at the item, not the controller: its spin (angular velocity x the item's offset
    // from the grip, ReleaseOffsetOf) counts this much (at 1 a wrist snap lifts a Molotov held by
    // its neck ~15 degrees above the hand's own direction; the throw plan's 0.6 keeps the item's
    // whip without throwing every snap high)
    float throw_spin{0.6f};
    // ... bent softly (SoftCap) toward the speed whose arc at the throw's own pitch lands this many
    // times as far as the game's own throw of the item (its BulletParam row's flight, ThrowCapAt):
    // a lob is held to the best arc's speed, a flat throw may leave faster ...
    float throw_range_factor{1.6f};
    // ... up to the speed at this pitch (degrees; flatter and downward throws keep it; 45 = the
    // best arc's speed at every pitch, as before 2026-10-05) ...
    float throw_cap_flat_deg{10.0f};
    // ... from this share of that speed up (a lower knee bends earlier and longer)
    float throw_cap_knee{0.7f};
    // Locked on: the throw's yaw (only) is turned this share toward the target when it points
    // within throw_assist_deg of it (knives throw_assist_knife_deg), fading out toward the edge.
    // (The user's 30%, 2026-10-07 morning; 0.26 the night before, 0.27 on 10-06, 0.22 before.)
    float throw_assist{0.30f};
    float throw_assist_deg{9.0f};
    float throw_assist_knife_deg{15.0f};
    // Not locked on, every throw but the knives (Molotovs, oil, pebbles, ...): the same yaw-only
    // turn, this share, toward the game's lock-on candidate whose heading is nearest the throw's
    // (PickYawTarget: the knife assist's list), within throw_assist_deg; 0 = lock-on only. (The
    // user's 2026-10-07 answer: "the regular throws yes"; the gun stays lock-on only.)
    float throw_assist_unlocked{0.26f};
    // Throwing and Poison Knives (KnifeAssist; 0 = the yaw assist above, lock-on only): the
    // throw's direction, yaw AND pitch, is turned this share of the way toward the direction that
    // drops onto the target's lock-on point at the throw's own speed (gravity compensated), at full
    // strength while it points within knife_assist_deg of that, fading to none at 4/3 of it. The
    // target: the lock-on target, else the game's lock-on candidate nearest the throw's line
    // (locked on or not). The speed is never changed. (0.85 since 2026-10-07: "much stronger just
    // for the throwing knives"; 0.70 on 10-06 still missed a knife 14 degrees off at 7.5 m.)
    float knife_assist{0.85f};
    float knife_assist_deg{15.0f};
    // ... plus the hunter's own horizontal velocity (1 = a throw while running carries his speed)
    float throw_inherit_motion{1.0f};
    bool throw_physics{true};    // real gravity and no speed clamps for the thrown item
    // The thrown item starts where the one in the hand was (the held model's origin), not at the
    // palm (the game's spawn point, up to ~12 cm away for a bottle held by its neck).
    bool throw_from_hand{true};
    // The game's own throw whoosh (the sound its throw animations play as the arm comes through,
    // c751) at the hand when a bottle, knife or pebble leaves it; 0 = silent (the projectile's
    // own sounds only).
    bool throw_sound{true};
    // A Molotov of yours in flight bursts in the air when your gun fires toward it (its centre
    // within molotov_burst_deg of the barrel's ray; no hit test), and with molotov_burst_ground
    // the bottle then drops straight down and lights the ground below as well.
    bool molotov_burst{true};
    float molotov_burst_deg{12.0f};
    // The air burst's bullet: 701 = the Cannon shell's explosion (its effect, sound and damage;
    // BehaviorParam 101500800 -> bullet 700 -> HitBulletID 701), <= 0 = the Molotov's own burst.
    int molotov_burst_bullet{701};
    // ... and its hit radius (m; the Cannon's own is 1.8; the effect keeps its size), 0 = its own.
    float molotov_burst_radius_m{2.0f};
    bool molotov_burst_ground{true};
    // The release pulse on the left hand scales with the launch speed (a lob a light tick, a hard
    // throw a long strong one); 0 = the same pulse for every throw.
    bool throw_haptic_scale{true};
    float throw_gravity{9.81f};  // m/s^2 with throw_physics
    // In-frame renderer: the arc, the cards, the labels and held fallbacks are drawn into the game
    // frame with the game's camera and depth (before the DIBR capture), 0 = not drawn at all.
    bool inframe{true};
    float inframe_exposure{1.0f}; // brightness of the in-frame items
    // What sits on the hunter's hand or arm (the held item, the Blood Gauge, the vial's drain) is
    // carried with your play space from the update his pose was placed for (the VR body places
    // it for the previous update's view anchor) to the frame's own: it moves with his hand while
    // you walk, strafe or turn instead of trailing one game frame behind (your head's own motion
    // moves nothing). 0 = placed from his dummies as they are, as before (2026-10-06).
    bool inframe_hand_sync{true};
};

QuickSettings& GetQuickSettings();

// VR::LoadSettings: true if `key` is one of ours (and it was applied).
bool ParseQuickSetting(const std::string& key, const std::string& value);
// The defaults block written into a new vr.ini.
void WriteQuickDefaults(std::ostream& out);

} // namespace VR::Quick
