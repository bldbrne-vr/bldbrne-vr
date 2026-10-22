# Bloodborne VR: the weapons guide

How every weapon works in the VR mod: how to hold it, swing it, fire it and transform it, and what
is different from the flat game. Checked against the code (2026-10-12). Names in `backticks` are keys in `%APPDATA%\shadPS4\vr.ini`.

> **Heads-up.** A lot of the trick-weapon work was added on 2026-10-12 and **has not been tried in
> the game yet**. Those parts are marked *(new, untested)*. The full list is in
> [section 5](#5-not-yet-tried-in-the-game).

1. [How every weapon works](#1-how-every-weapon-works)
2. [Right-hand trick weapons](#2-right-hand-trick-weapons)
3. [The left hand](#3-the-left-hand)
4. [Settings quick reference](#4-settings-quick-reference)
5. [Not yet tried in the game](#5-not-yet-tried-in-the-game)

---

## 1. How every weapon works

### Your swing is the attack

With `melee=physical` (the default) no attack animation plays. Swing the right controller and the
game's own attack rides on the blade you see: it hits whatever the blade passes through while it
moves fast. Damage, poise, blood, the rally and staggers are the game's.

| Attack | How to do it |
|---|---|
| **Light** | Any real swing from guard. |
| **Heavy** | Start wound up (blade behind your head or shoulder, raised high, or arm out wide; a soft click says you are wound up) and sweep a big arc, about 140° (seated about 120°). Stop short and it is a light. Or hold the right trigger 0.35 s (a tick), then swing. |
| **Charged** | Hold the right trigger 1 s (`melee_charge_s`): a buzz builds, then a triple buzz. The next swing is the fully charged attack, which can stagger an enemy from behind for a visceral. |

- The trigger stays armed for 1.5 s after you let go, and arms every swing while you hold it. The
  trigger alone never attacks.
- Wind-ups, slow swings, wrist waggles, reaching out and raising the weapon never attack.
- Faster swings hit harder: ×0.75 for a swing that only just counts, up to ×1.10 at full speed
  (`melee_speed_damage`).
- The hit area is trimmed to the blade, so a thin sword hits where you see it (`melee_radius_scale`,
  `melee_radius_fit`).
- A form without a heavy or charged move of its own uses its next lighter move instead.

### The pace: two quick charges

Each weapon has **2 quick charges** (`melee_budget_charges`). A light swing uses 1, a heavy 2, a
charged swing all of them. Used charges come back at the weapon's own pace in the game (its
recovery after that move, minus 80 ms: `melee_budget_offset_ms`, with extra reductions for physical
whips). Two quick lights can use the two stored charges. Used charges refill in sequence: the
second waits for the first to refill. The refill duration does not increase with the combo count;
continued rapid attacks use up the initial reserve. Each weapon's light pace is in the
[section 2 table](#2-right-hand-trick-weapons).

- With no charge left a swing **does not register**: the whoosh plays, the blade passes through,
  and you feel a **dull buzz** instead of the usual tick.
- A wound-up swing with one charge left becomes a light. A trigger-armed heavy with too few charges
  does not register, and the trigger stays armed.
- Transforms never use charges.
- Your left hand shares the same charges ([left-hand swings](#left-hand-swings-new-untested)).

### Stamina

- Light swings cost **50%** of the game's stamina cost (`melee_stamina_pct`; VR page: Assists >
  Light attack stamina, Original / 80% / 50% / 10%). That includes form B, whip lashes and left-hand
  swings.
- Heavy and charged swings cost the full amount. With less stamina than a heavy costs, it becomes a
  light.
- At 0 or below a swing is refused (a double tick), as in the game. A half-cost swing does not
  pause your stamina regen.
- Dodges, sprinting, jumps and the gun keep the game's own costs.

### Transforming

- **Squeeze the right grip** with your hand still: only the weapon transforms (`transform=native`).
  No body animation, and you keep walking. A tick at the squeeze, a detent for each mechanical
  click, a clack when the new form locks. You can swing again 0.1 s after the lock.
- **Flick it** (`transform_flick=1`): squeeze while you swing and the weapon snaps into its other
  form within about 0.1 s. If the weapon has a transform attack that way, the rest of the stroke is
  the game's transform attack (its damage and poise); otherwise it is the new form's own swing.
  Section 2 lists each weapon's flick attacks.
- If the game has your arm (a hit, an item, a door), the squeeze waits up to 0.4 s
  (`transform_buffer_ms`), then you get a double tick: squeeze again. A roll never holds it up.
- **Weapons on the game's own transform (L1):** the Chikage, the Bloodletter, Logarius' Wheel,
  Tonitrus, the Kos Parasite and the Boom Hammer's closing play the game's whole-body transform
  animation (the game's buff or effect lives in it, or there is no weapon-only clip for it). For
  these, hold your hand still and then squeeze. A squeeze mid-swing, within 0.4 s of an attack or
  with the hand moving faster than 1.5 m/s is ignored.
- **By hand** *(new, untested, `hand_transform=1`)*: for some weapons the right grip does
  something else (by design, 2026-10-12). See each weapon in section 2:
  - **Frees your left hand** (a click in both hands; for 5 s, a second squeeze takes it back): the
    Chikage, Hunter Axe, Burial Blade, Kirkhammer, Ludwig's Holy Blade and Rifle Spear, then
    **grab the handle with the left grip**: the two-handed form, **only while both hands hold
    it**; the Blade of Mercy and Rakuyo, then take the second hilt and pull apart.
  - **Arms the Bloodletter while held**: keep holding and stab it into your chest.
  - **Held, charges**: Tonitrus (lift it up over your head), the Boom Hammer (1.5 s), the Beast
    Claw (2 s). Tonitrus and the Boom Hammer then cool down 10 s (`charge_cooldown_s`).

  The right grip transforms nothing within 1 s of a hand transform. `hand_transform=0`: the
  squeeze transforms them all.

### Two hands on the weapon

In forms the game holds with both hands (it puts your gun on your hip), **put your left hand on the
handle and squeeze the left grip** (`two_hand_grip=1`):

- Grab anywhere along the glowing grip region. The left hand attaches there and slides along
  it as you move, without another button. The region runs from a hand's width off your right
  fist to a fist past the game's own left grip (the end of the handle). On polearms and hafted
  weapons (an axe, a scythe, a spear, a hammer) it also runs past the right fist toward the
  head, up to 30 cm and stopping a hand's width short of the striking part, so either hand can
  be in front; passing the right fist does not reverse the blade. On swords the blade side is
  never a grip. That reach is learned from each weapon's two-handed form the first time you
  hold it (until then only the handle side). A left squeeze right on top of your right hand is
  no grab: it stays the quick items'. The right controller keeps its fixed weapon grip and
  position.
- **Both hands slide, no button:** the hand you move along the weapon slides on it, the one you
  keep still holds it. Move your left hand along the shaft and it slides (the weapon stays in your
  right hand, as before); move your right hand and it slides (the weapon stays where it is in the
  room and moves in the hunter's right fist, his hand staying on your controller; its hits follow
  it). Move both together (a thrust, a step) and nothing slides: the weapon goes with them. Each
  hand takes its share of a change in proportion to how much it moved, so there is no switching
  and nothing to flicker, and tracking jitter never creeps the weapon through your fist.
- **Walls, not springs:** both hands stay on the handle (never onto a sword's blade, never past
  the pommel end); the upper hand always leaves 10 cm for the lower one at the pommel end (no
  holding it with both hands at its very bottom); on a sword the right hand goes no lower than
  where the game's own left hand holds it. A hand at a wall stops sliding there (the right hand
  then carries the weapon on). Releasing the weapon (both grips open) eases it back to its normal
  place in the right fist.
- **While you swing, nothing slides:** both hands keep their places on the weapon from the
  moment it moves fast (0.8 m/s, 30 cm out along the blade) until it has been slow for 0.15 s;
  the hands' catch-up after it is the left hand's (the weapon never creeps in the fist).
- **Moving the left hand past the right one:** the controllers cannot pass through each other,
  so your hand goes round your right hand. Within about 10 cm of it the weapon holds its angle
  (from 14 cm on, steering is normal again), so the detour steers nothing; go round on whichever
  side is free (lean the weapon to the left to pass on the left). Steering takes over from where
  your hand comes out, keeping the weapon's angle. With the hands that close together, swings
  follow your right hand as before.
- Movement along the shaft slides; movement across it steers with speed-adaptive One Euro
  smoothing: steady aiming filters jitter, fast push-pull swings use less smoothing. Steering
  pauses near overlapping hands to avoid flips. Tracking recovery clears the speed estimate
  so a tracking jump is not treated as a swing. Your left wrist keeps its tracked rotation.
  The filter runs on the headset's frame clock, so frame-rate changes do not change the feel.
  During transformation or first-time calibration the hand follows the controller until the
  rail is ready, then attaches at your current position rather than the animation's grip.
- Wrist roll around the handle blends 70% right / 30% left, relative to the grab pose, the left
  share at most 25 degrees (turning the left wrist back undoes it at once, however far it went).
  The right grip stays fixed; bending your left wrist sideways does not directly tilt the blade.
  Re-grabs (also one while the last hold still eases out) and tracking recovery do not snap the
  blade to a new angle.
- With a shared handle attached, swings follow the steered blade, so a push-pull swing registers
  even with the right controller nearly still. The swing detector reads a point 30 cm out along
  the blade (its thresholds were tuned on the hand: a 3-5 rad/s wrist flick stays no attack); its
  wound-up pose is still judged at your hand. Damage speed and the swing hum read the blade 80 cm
  out, the same scale as one-handed swings, whatever the weapon's length. The direction comes
  from the weapon's own hit geometry when known (an off-axis head included), fixed during a hold.
  The left controller hidden behind the right hand does not interrupt a swing (the weapon keeps
  its turn on the right hand). A frame without blade motion (the attach blend, a hitch, the right
  controller lost) is a tracking gap the swing rides out, not a cancelled attack. Single-handed
  use, untransformed forms and separate dual weapons keep their controller-based combat paths.
- Fast swings give both attached hands a faint hum, beneath hits, charges and other feedback.
  **VR Settings > Weapons > Swing feel** adjusts it (0% disables it; `two_hand_swing_hum=0`).
  The existing `two_hand_haptics` and melee haptics switches also disable it.
- Swings with the grip held and your left hand moving with them (0.8 m/s or more) hit ×1.05 harder
  (`two_hand_damage`) and thump in both hands.
- Hold the right grip to show a faint glow along the available grip region. Disable it under **VR
  Settings > Weapons > Grip guide** (`two_hand_guide=0`). It disappears once the sliding grip attaches.
  On an already transformed shared-handle weapon, hold right grip to aim the support hand;
  a short tap (under 0.25 seconds) transforms on release. Grabbing cancels that tap.
  This gesture stays the same when the guide is hidden; weapon-specific grip actions still apply.
- Grabbing a two-handed handle or the dual blades' hilt automatically closes the quick-item
  menu. An item already in the hand must be put away first.
- To let go, release **both grip buttons**. Either grip keeps an acquired hold attached, even
  during wide swings or wrist rotation. Re-squeezing the right grip while attached does not
  transform the weapon. Menus, weapon changes and prolonged tracking loss still clear the hold.
- The rail is straight, along the native grip's axis; it is not fitted to every mesh. Curved and
  bulky weapons (Logarius' Wheel) still need headset testing. Reaching an end does not pull the
  right controller or release the weapon. Grip spacing never scales the weapon.
- The cue uses the learned rail even before transformation; its calibration stays fixed during
  a hold while your left contact point slides. The right contact point stays fixed in this version.
- It only grabs if your left hand is within 9 cm of the bounded rail at the moment you squeeze
  (`two_hand_zone_cm`). A squeeze anywhere else still opens the quick-item arc or draws a vial at
  your hip. In one-handed forms the handle does nothing.
- Two-handed forms with a handle: the Chikage, Hunter Axe, Burial Blade, Kirkhammer, Ludwig's Holy
  Blade, Rifle Spear, Logarius' Wheel, Holy Moonlight Sword, Bloodletter, Church Pick and Whirligig
  Saw, all in form B. [unverified: this list comes from the game's data; only Ludwig's Holy Blade
  was tried, in the mock runtime]
- **The grab group** *(new, untested)*: the Chikage, Hunter Axe, Burial Blade, Kirkhammer, Ludwig's
  Holy Blade and Rifle Spear are in form B only while both hands hold the handle. In form A,
  squeeze the right grip (your left hand is free: no gun meanwhile, a soft tick if you pull the
  left trigger), then grab the handle with the left grip (or squeeze both grips together with the
  left hand on the handle): a heavy
  clunk through both hands and it transforms. Release both grips and after 0.35 s it turns back
  to form A, with one heavy drop in the sword hand. During a swing that time only starts once
  the swing is over. Wide swings and off-hand tracking occlusion keep the attachment.
  If something else takes the hand off it (a menu, a vial, the hand out of the cameras'
  view), it turns back when you release both grips. A trigger held through the grab never fires the
  gun, the game's special or the saw: let go of it first. A squeeze that just misses the handle
  (within 15 cm) never opens the quick items; the hand gets a faint tick as it comes onto the
  handle when a squeeze there would take it. In third person (or with the VR body off) the right
  grip transforms these weapons as before.
- **The left trigger in a two-handed form** *(new, untested)*: nothing, unless the weapon is listed
  in `l2_canned` (default: the Bloodletter and the Kos Parasite): then it is the game's own special
  with its animation. Add a weapon's key (such as `holy_moonlight_sword`, `whirligig_saw`) to try
  its special.

### Viscerals

- After a **gun parry** or a **charged swing's back-stagger**, a soft heartbeat on the right
  controller says a visceral is open (`visceral_cue`).
- **Thrust** the blade straight at the enemy: 10 cm or more along the blade at 1.5 m/s or more
  (`visceral_thrust_cm`, `visceral_thrust_speed`). Face the enemy within the game's visceral reach
  (about 1.5 m); lock-on helps.
- **A good stab with the blade off its line counts too** (a wrist turned, a hammer grip): the
  hand itself moving 10 cm or more straight ahead at 1.5 m/s or more, within 30 degrees of where
  you look, keeping within 8 degrees of the direction it started in, with the blade within 60
  degrees of that path and still pointing ahead. A swing's hand arcs off its starting direction
  at once, so a swing still hits as a swing (only a swing held with the blade far ahead of the
  arm can pass for a stab, and only while a visceral is open does that matter).
- **Your view holds its place** (with `dodge_camera=1`, the default). The game's visceral moves
  the hunter into the enemy; your view stays where it was (the enemy is brought to you), your
  head and hands tracked, and it is kept out of the enemy (no closer than 55 cm to its centre).
  When the visceral ends, or you cancel into another move, the view glides onto him over 0.3 s
  (longer if he ended up far from your view).
- A normal swing at the staggered enemy just hits it, and that ends the stagger. After a parry:
  thrust, don't swing.
- **Stab and pull out** (`visceral_pull=1`, `visceral_freehand=1`; prototype, headset validation
  pending): both hands and your torso stay tracked. The stab lands with a hard thud, the enemy
  takes it, and just before the rip the whole world holds still (every character), the blade
  lodged in your hand and the enemy's heart beating in it. Pull the right hand 12 cm back out
  (`visceral_pull_cm`; back, up or sideways, as slowly as you like) whenever you like: the world
  resumes, the rip lands and the enemy is thrown back. The thrust's own recoil is not the pull.
  There is no deadline (a 20 s safety completes it at full damage). Each animation's first use
  plays through to learn timing.
  `visceral_freehand=0` restores the older animation takeover and speed-qualified yank.

### The plunge *(new, untested)*

Swing while you fall, or just **hold the blade point down** as you fall (after 0.5 m of falling,
the fist no lower than 45 cm below your eyes; a tick when the point is set; once per fall). Either
way it is paid from your charges like a swing. The attack stays live until you land (+0.15 s), so
the blade hits what you land on. After a drop of 1.2 m or more
(`melee_plunge_min_drop_m`) its hits deal ×1.5 (`melee_plunge_damage`, a placeholder until the
game's own plunge values are known). No plunge animation. It needs `dodge_camera=1` (the default).

### Solid weapons *(new, untested)*

Off by default. With `weapon_wall=1` (VR page: Weapons > Solid weapons: On), your weapons are
solid: the blade in your right hand, the gun in your left, or a bare hand **stops at walls, floors
and pillars and never goes through them**, however far your controller goes. The hunter's hand stays
at the wall; bring your hand back and the weapon comes with it at once. Your two weapons stop each
other too: cross the blade and the gun and they push apart, each keeping to its side.

- **Drive it into a wall** (1.2 m/s or more into it): it stops there with a solid thud in that
  hand, harder the faster; steel throws a little spray of sparks.
- **Slide it along the wall** (0.3 m/s or more), **or turn it along the wall**: its tip or edge
  slides along the surface with a buzz rising with the speed and a grit of short ticks; steel
  moving fast (0.5 m/s or more) throws a trickle of sparks, more the faster.
- **Rest it there:** nothing.
- **Rub your two weapons together** (the blade and the gun; Rakuyo's or the Blade of Mercy's two
  blades; the Beast Claw's claws): both hands feel it, with sparks. The hand pressing into the
  other weapon yields; a resting hand stays put. If both press, they share the correction by
  their inward motion when contact begins. That split stays fixed while they touch, including
  further pressing, so small wrist movements cannot jerk the other hand. Separating releases it.
  The approach determines which blade stays above, including rotation at a stationary grip; neither
  hand is assigned the lower side. The first time you split the blades, hold your left hand
  still a moment (the left blade is measured then).
- **Caught on something** you walked past (a door frame, a pole) and your hand already far away
  (45 cm): it is back in your hand at once, never past a wall between you and your hand.
- **A long blade hanging while you walk:** its tip rests on the floor and drags along it, with a
  light buzz and a few sparks.

The sparks are small and short: white-hot streaks that cool to orange and dull red and are gone
within half a second, like flint on steel. A bare hand never sparks. It works only while your arm
is yours (not while the game has it: an item, a visceral, a ladder, a door), whatever the controls.
With the two-handed grip your left hand stays on the handle and feels the right weapon at 60%.
Limits: the torch and the shield count as the bare hand, and so does the right weapon with
`melee=vanilla`; a weapon is a line along its blade (an axe's or a
hammer's head touches only where that line does). Off (the default), none of it runs.

### Also good to know

- **Kills go limp** (`melee_kill_ragdoll=1`): an enemy your blade kills falls the way your blow
  sent it, with no death animation. About one kill in five (22%, `melee_ragdoll_vanilla_pct`) still
  plays the game's death. VR page: Assists > Instant ragdoll, Ragdoll push.
- **Enemies read big swings** (`melee_telegraph=heavy`): wind-ups, heavies, and long or wide swings
  warn the enemy AI as the game's own attacks do. A quick flick from guard does not.
- **Attacks out of a dodge** (`dodge_attack=1`): you can swing or shoot at any moment of a
  backstep, quickstep or roll, but doing so ends the dodge's invulnerability
  (`dodge_attack_iframes=0`).
- **Switching weapons:** hold the right stick click and push the right stick right (right-hand
  weapon) or left (left-hand weapon). Or use the shoulder switch (`shoulder_switch=1`, VR page
  "Shoulder weapon switch"): hold your right hand still at your right shoulder and press B for the
  next right-hand weapon, or your left hand at your left shoulder and press Y for the next left-hand
  weapon.
- **Grip adjust:** standing still, hold A+B for 1.2 s. The weapon freezes; hold the controller the
  way you like and let go of A or B to save that grip for the weapon.
- **Playing seated:** set Stance to Seated on the VR page (`seated=1`) for a smaller heavy arc and
  wind-up.

### What the vibrations tell you

| You feel | It means |
|---|---|
| A light tick in the weapon hand | the swing counted |
| A soft click | you are wound up: the next big swing is a heavy |
| A tick, or a building buzz then a triple buzz, while you hold the right trigger | heavy armed / fully charged |
| A dull buzz instead of the tick | no charge left: that swing did nothing |
| A strong thump (heavy hits add a second "crunch") | your hit landed |
| A dull knock | the hit did no damage (guarded, parried, immune) |
| A clank, or a thud | the blade hit a wall or the floor (with `weapon_wall`: it stops there, and a buzz while it scrapes along) |
| A double tick | refused: the game has your arm, you are out of stamina, or a transform was refused |
| A soft tick or a soft "no" | a hand-transform gesture did not take (the hands never met, a strike stopped short) |
| A soft heartbeat | a visceral is open: thrust |
| A tick, detents, then a clack | the transform started, clicked through, locked |
| A click on both hands / a soft tick on both | two-handed grab / release |
| A small knock on both hands | you were hit |

The gun's feedback is in [section 3](#guns).

---

## 2. Right-hand trick weapons

"Form A" is the weapon as you first hold it, "form B" its transformed form. Everything in section 1
applies to all of them; this section is what differs. **Light pace** is how long a light swing's
charge takes to come back, in seconds (form A / form B). **Flick** lists the directions in which a
flick turns the rest of the stroke into the game's transform attack. (DLC) marks The Old Hunters.

| Weapon | Form B held | Transform | Flick attack | Special in VR | Light pace A / B |
|---|---|---|---|---|---|
| Saw Cleaver | one hand | grip | both ways | none | 0.49 / 0.69 |
| Saw Spear | one hand | grip | both ways | none | 0.49 / 0.72 |
| Hunter Axe | two hands | right grip, then grab the handle; let go: back | B→A only | two-handed grip | 0.69 / 0.89 |
| Threaded Cane | one hand | grip | A→B | form B is a physical whip | 0.49 / 0.47 |
| Beast Cutter (DLC) | one hand | grip | both ways | form B is a physical whip | 0.69 / 0.83 |
| Kirkhammer | two hands | right grip, then grab the handle; let go: back | none | two-handed grip | 0.59 / 1.05 |
| Ludwig's Holy Blade | two hands | right grip, then grab the handle; let go: back | none | two-handed grip | 0.59 / 0.92 |
| Holy Moonlight Sword (DLC) | two hands | grip | A→B | moonlight wave (charged swing); two-handed grip | 0.89 / 0.75 |
| Blade of Mercy | a blade in each hand | right grip, then by hand: split / join | B→A | left blade swings | 0.42 / 0.39 |
| Rakuyo (DLC) | a blade in each hand | right grip, then by hand: split / join | B→A | left blade swings | 0.59 / 0.55 |
| Burial Blade | two hands | right grip, then grab the handle; let go: back | none | two-handed grip | 0.59 / 0.72 |
| Church Pick (DLC) | two hands | grip | both ways | two-handed grip | 0.55 / 0.72 |
| Beasthunter Saif (DLC) | one hand | grip | both ways | none | 0.72 / 0.62 |
| Amygdalan Arm (DLC) | one hand | grip | both ways | none | 0.89 / 0.95 |
| Rifle Spear | two hands | right grip, then grab the handle; let go: back | B→A | right trigger held, let go: blast (B); two-handed grip | 0.59 / 0.62 |
| Reiterpallasch | one hand | grip | B→A | right trigger fires (B) | 0.52 / 0.42 |
| Stake Driver | one hand | grip | both ways | hold right trigger 1 s, release (B) | 0.52 / 0.62 |
| Whirligig Saw (DLC) | two hands | grip | A→B | hum (B); two-handed grip | 0.59 / 0.89 |
| Boom Hammer (DLC) | one hand | hold the grip 1.5 s to open (cooldown), game's L1 to close | none | hum (B) | 0.82 / 1.32 |
| Simon's Bowblade (DLC) | the bow | grip | B→A | form B is a real bow | 0.55 / bow |
| Chikage | two hands | right grip, then grab the handle (game's L1); let go: back | (L1) | two-handed grip | 0.55 / 0.59 |
| Bloodletter (DLC) | two hands | hold right grip, stab into chest (game's L1); grip again: back | (L1) | two-handed grip | 0.65 / 0.85 |
| Tonitrus | no form B | hold the grip and lift it up (game's L1; cooldown) | (L1) | none | 0.65 / – |
| Logarius' Wheel | two hands | game's L1 | (L1) | two-handed grip | 1.09 / 1.22 |
| Beast Claw | claws on both hands | hold the grip 2 s | none | left claw swings | 0.49 / 0.45 |
| Kos Parasite (DLC) | parasite on the left arm | game's L1 | (L1) | B: game's own attacks; left-hand parasite swings | 0.49 / game's |

"(L1)": the transform is the game's own animation, so there is no flick. A weapon you transform by
grabbing, holding or lifting has no flick into the form that takes it (its trick attack that way is
lost: accepted, 2026-10-12).

### Plain trick weapons

The **Saw Cleaver**, **Saw Spear**, **Hunter Axe**, **Burial Blade**, **Church Pick (DLC)**,
**Beasthunter Saif (DLC)** and **Amygdalan Arm (DLC)** follow section 1 with nothing extra.

- The Hunter Axe and Burial Blade are in the grab group: right grip, then grab the handle; form B
  lasts while both hands hold it ([two hands](#two-hands-on-the-weapon)). The Church Pick is
  two-handed in form B too, but transforms with the grip as before. The Hunter Axe's haft shifts
  about 5 cm in your hand as form B locks (the game's grip point).
- The Saw Cleaver's transform locks 0.27 s after the squeeze (0.47 s in the game).
- The Beasthunter Saif's and the Amygdalan Arm's tip may jump at the squeeze when opening (their
  transform clips start in another pose). [unverified: the doc lists it as not checked]

### Kirkhammer, Ludwig's Holy Blade, Holy Moonlight Sword (DLC)

Form A is the sword in one hand; form B is the sword slotted into the hammer or the sheath.

- **Kirkhammer, Ludwig's Holy Blade** *(new, untested)*: the grab group. Squeeze the right grip
  (your left hand is free), then grab the handle with the left grip: form B, while both hands hold
  it. Let go and it turns back. No flick attacks.
- **Holy Moonlight Sword:** stays one-handed in both forms (by design: "it's fun"); the grip
  transforms it, with a flick attack into form B. **The moonlight wave** *(new, untested,
  `moon_wave=1`)*: only a **charged** swing in form B (the right trigger held the full 1 s, the
  triple buzz, then swing) lets the wave go just after the stroke's fastest point, from the middle
  of the blade. A heavy no longer sends it.
  - It flies **where you look** (where your head points) if the blade's middle swept within 20° of
    your line of sight during the swing. Otherwise it flies out from your chest through the blade
    as it was at its fastest, so a swing beside you keeps its own line.
  - It never goes more than 20° up or down. Not on a flick's stroke (that one is the transform
    attack), and not on a swing that had no charge left. You feel a swell out of the blade.

### Dual blades: Blade of Mercy, Rakuyo (DLC)

Form A is one blade; form B is a blade in each hand. By hand *(new, untested)*:

- **Split (A→B):** squeeze the right grip (your left hand is free, for 5 s), then bring your left
  hand to your right (within 18 cm) and squeeze the left grip: you take the second hilt. A squeeze begun within 25 cm still takes hold if the hands meet within
  0.3 s. Pull the hands 18 cm further apart (within 1.5 s): the blade splits, a ring of steel in
  both hands.
- **Join (B→A):** bring the blades together side by side (hands within 22 cm, blades within 35° of
  parallel) and squeeze the left grip, up to 0.3 s before they meet: they lock back into one with a
  clack.
- **Form B:** your right hand swings the long blade; **your left hand swings the second blade**
  ([left-hand swings](#left-hand-swings-new-untested)). Both count, and swinging both at once (an X)
  spends both charges.
- **Blade of Mercy** form B has a light and a heavy (its charged swing is the heavy). **Rakuyo**
  form B has only a light on the right hand: its heavy and charged swings are the light, because the
  game's form-B heavy strikes with the small blade in the left hand.

### Whips: Threaded Cane, Beast Cutter (DLC)

Form A is a blade. Form B is a whip, and with `whip=physical` (the default) it is a chain simulated
on your hand: what you see is what hits.

- **Hold the transform grip for 0.08 s**, in either direction: right grip in the default motion
  layout, left grip in Classic or `right_grip=draw`. A tap does nothing; release before transforming
  again. Menus, tracking loss, frame stalls and changing weapons cancel a pending hold.
- **The Cane picks up each new stroke**, including a backhand before it has reeled in. A short,
  tapered pull travels down the chain, then releases it so the light tip lashes past the stopped
  hand. Continuous circles do not keep restarting that pull. The first throw keeps its existing assist.
- **It pays out with your speed.** It starts to extend at about 2.5 m/s at the weapon's far end and
  requests full payout at 5.5 m/s (`whip_payout_speed`). With `whip_reach_scale=1`, the Beast
  Cutter's reach caps from the grip are **4.7 m light, 6.6 m heavy, and 7.4 m charged**. The
  Threaded Cane's caps are approximately **4.15 / 3.17 / 3.60 m** respectively. These are reach
  limits; the tip's actual distance depends on the swing and how straight the chain is.
- **Keep it moving** (2 m/s or faster) and it stays out. With `whip_hold_s=0.6`, the Cane starts
  reeling in after 0.42 s of slow motion; the Beast Cutter waits 1.4 s. A new stroke resets the pause.
- **Flick it back** (the Beast Cutter's whip; the Threaded Cane has none): with the chain out, a
  small quick snap of the wrist that lifts the weapon back (its far end up, or back over your
  shoulder), your hand itself still, reels it straight back in, about 0.1 s after the wrist stops.
  The same snap thrown out (down, forward or sideways) is a crack: it lashes and hits, and the
  chain stays out. Cocking the wrist back and then snapping it straight out harder is a crack too,
  and so is the wrist coming back up just after a crack; the wrist easing back to where it was after
  a flick back keeps the recall. Aimed steeply down at the chain on the floor, raising the weapon
  is still the flick back. A flick back never costs a swing.
  `whip_flick_recall=any` brings back the older snap in any direction (a crack then reels the
  chain in too); `whip_flick_recall=0` turns the flick recall off.
- **Attack window:** extended whip attacks stay open for up to 0.72 s while you keep swinging,
  with up to another 0.30 s for the chain to finish its lash while still extended and fast.
  Slowing down can end an attack sooner; a new swing can start before that limit.
- **Attack recovery:** with default settings, the Cane refills a light attack charge in 0.317 s;
  heavy/charged attacks refill their charges together in 0.853 / 0.787 s (2026-10-15: about a
  quarter sooner than before). The Beast Cutter takes
  0.744 / 1.563 / 1.596 s respectively (2026-10-15: light 5%, heavy / charged 10% sooner). Two stored charges allow quick light follow-ups; the refill
  duration stays constant through a combo. These times apply only to physical whip forms.
- **Damage keeps the game's pace** (`whip_pace=1`, since 2026-10-15): the quicker refills make
  lashes register sooner, not hurt more. The game starts a move only once the move before it has
  recovered (Threaded Cane light 0.767 s, heavy 1.333 s, charged 1.267 s; Beast Cutter 1.233 /
  2.067 / 2.100 s). A lash after a pause, lashes at that pace and a heavy right after a light's
  recovery (the game's own R1 then R2) hit in full. A lash that comes sooner loses the share of its
  own move's recovery it is early by: at twice the game's pace, half each. A lash that hits nothing
  costs nothing, and every enemy one lash hits takes the same share. So no flurry, spin or crack
  deals more damage per second than the game's own attacks. A light at the soonest the charges
  allow after a heavy or charged one deals less (the Cane about x0.37, the Beast Cutter x0.59):
  the game counts the heavy's long wind-up too. Every lash that lands still feels the same on the
  controller (the impact keeps the damage before the pace). `whip_pace=0` lets every registered
  lash hit in full, as before (the Cane's quick lashes and spins then deal up to about 2.4 times the
  game's rate).
- **Spin it** overhead and it circles like a helicopter blade. Each revolution is a swing paid from
  your charges, so spinning uses the same refill timing as other whip attacks, and its hits keep the
  game's pace.
- **Crack it:** a sharp flick of the wrist lashes the chain out and hits, paid like a swing.
- Only a moving chain hurts (its tip at 4 m/s or more). Heavy and charged swings work as usual.
- **Flick into the whip:** finish the grip hold during a swing to lash the new chain out with your
  stroke. The Beast Cutter also has a flick attack back into form A; the Threaded Cane does not.
- **Feel:** a light hum rising with the payout, a tick when it locks out, a soft click when it reels
  in, the thump on hits.
- **Limits:** the chain passes through walls, and lies on a flat floor at your feet's level
  (wrong on stairs and slopes). `whip=canned` gives the game's own whip animations instead.
- The Threaded Cane's whip comes out of the front of your hand.

### Gun-blades: Reiterpallasch, Rifle Spear

*(new, untested, `gun_blade=1`)* No animation for either.

- **Reiterpallasch**, form B: **squeeze the right trigger** to fire from the tip along the blade.
  0.87 s between shots; the kick is the Hunter Pistol's, and the hunter's sword hand kicks too:
  the tip flips up about 9° and the hand jumps back 3 cm, settling within about a quarter second
  (`gun_recoil_visual` scales it, 0 turns it off).
- **Rifle Spear**, form B: **hold the right trigger** (a build in the hand, a triple buzz at 0.3 s:
  charged), then **let go**: the blast from the tip along the blade, with a strong kick: a long hard
  slam in the sword hand and a heavy one in the left hand on the shaft (when you hold it there), and
  the hunter's arms thrown
  back (the tip up 22°, the hand back 8 cm).
  Let go before it is charged and nothing fires (a soft tick). 1 s before the next (a squeeze
  before then: a double tick). Its form B is the grab group's: right grip, then grab the handle.
- Bullets and costs are the game's. No bullets: a double click.
- Form B has no heavy or charged swing, so the trigger arms nothing there and every swing is a
  light.
- Sighting down the blade (your hand at your face) still shoots. No shot while swings are held back
  (an NPC talking, Menu held: a double tick) or while the vial is at your wrist.
- Flick attack only back into form A.

### Stake Driver

*(updated 2026-10-13; headset verification pending, `stake_blast=1`)* Squeeze right grip to
transform. In form B, **hold the right trigger 1 s**, wait for the charged buzz, then **release**.
The discharge works with the controller held still; no punch is required. A rising rumble and
triple ready buzz precede it, with the native blast effect at the tip, a strong thump and
weapon-hand recoil when the game accepts it.
The native melee blast sphere follows the tracked weapon tip and keeps its 1 m radius.
A melee attack row opens a short powered contact attack using the normal melee stamina and
combo budget, without swing-speed damage reduction. A projectile row fires on the same release
(one bullet). Both paths wait 3 s between discharges. Early release cancels; changing context or
losing permission to attack cancels the charge. Normal form remains ordinary melee. Flick
attacks work both ways.

### Humming weapons: Whirligig Saw, Boom Hammer (DLC)

*(new, untested, `weapon_hum=1`)* In form B (the Whirligig's spinning blade, the Boom Hammer's lit
furnace) the weapon hums gently in your hand while you hold it, and revs up as you swing (stronger
the faster the far end moves, from 1 to 5 m/s), winding down over about a quarter of a second. The
Whirligig hums a little stronger. Any other feedback in that hand takes over and hands the hum
back when it ends.

- **Whirligig Saw:** the right grip does nothing. Grab the handle with the left hand to
  transform it into the two-handed saw; releasing the grab returns it to form A after the
  normal release grace. With both hands holding it, **hold either trigger** to spin it up over
  0.3 s and grind enemies. Holding both triggers and then releasing one keeps it running;
  a trigger held through the grab also works. Release both triggers or end the grab to stop
  contact damage immediately; the blade coasts down over 0.5 s. Only the weapon animates:
  its native `a148_052910_2.hkx` loop and attack judges 931/932 run without L2 or a player clip.
  Native blade sparks follow the spin, including spin-up and coast-down.
  Each grinding interval costs the native 10 stamina; empty stamina requires releasing the
  trigger before restarting. `l2_canned` cannot enable a character animation for this saw.
  Implemented and covered by control/timing regressions; headset verification is still pending.
- **Boom Hammer:** *(new, untested)* **hold the right grip 1.5 s** to light the furnace (a build in
  the hand, then a thump): the weapon-only transform into form B. Then 10 s before it lights again
  (`charge_cooldown_s`). Closing is the right grip and the game's own L1 animation (the game keeps
  no data for that clip).

### The bow: Simon's Bowblade (DLC)

Form A is a curved blade (light and heavy; its charged swing is the heavy). Form B is **a real bow**
*(new, untested, `bow=1`)*:

| You | What happens |
|---|---|
| Transform into the bow | the bow in your left hand, **an arrow ready in your right** (the game's own arrow is hidden; ours shows) |
| Bring it to the bow (within 27 cm of the arrow rest, `bow_nock_cm`: round the string) and squeeze the right trigger | nocked: a click in both hands. No arrow in hand: a soft "no" |
| Keep squeezing and pull back | the draw, full at 60 cm (`bow_draw_cm`). The hunter's hand rides the string: on the arrow's line, never past full draw, however far your controller goes. A tick each third, the string's tension building in both hands, and **the wall** at full draw |
| Hold full draw for 0.6 s (`bow_charge_ms`) | charged: a triple buzz |
| Let go | the quick shot, or the charged shot once charged: the string snaps on your fingers, the bow kicks and buzzes in your other hand. Below 15% draw (`bow_min_draw`) the arrow goes back into your fingers, no shot |
| After a shot, reach over your right shoulder and squeeze the right trigger | the next arrow from the quiver in your fingers. It stays there until you shoot it |

- **Which hand:** the bow in the left hand, the arrow in the right (a right-handed archer; the
  game's bow form puts it there and the log confirms it).
- **The bow** is your hand's: it turns freely with your wrist, held as when shooting (fist on its
  outer, left side, the handle pointing left, the bow 1.5 cm toward its right of the fist). Its grip
  on your hand is the one of your last full draw (before any shot, facing where your hand points).
  Placing an arrow never turns it; drawing back eases it onto the arrow, and after the release it
  keeps that grip.
- **The arrow** is shown at 90% of its size. In your hand it points where your hand points
  (forward), held round the shaft with its nock 7 cm behind your fist (`bow_arrow_pitch_deg`, 45:
  how far below a sword's line). **On the string** it runs along the bow's right side: 2 cm above
  the fist and 4.5 cm across, clear of it; the draw, the nocking zone and the shot's line all start
  from that rest.
- **Aim:** along the line from your string hand to your bow hand, as it was just before you let go.
- **Stamina:** a quick arrow costs `melee_stamina_pct` (50%) of the game's cost, a charged arrow the
  full cost. With none left there is no arrow (a double tick).
- **The left trigger does nothing** while the bow is out (never the game's bow animation). No
  swings while the bow is up. The right grip turns it back into the blade (flick attack B→A), but
  not with an arrow on the string or held at the bow (move your hand away first).
- `bow_quiver=0`: no quiver; the bow comes out empty and a squeeze at the bow nocks an arrow.
- Still to come (they need the game, on Windows): the game's own bow bending and its string drawn
  back with your draw (until then its string stays straight; our arrow is a plain 78 cm shaft), the
  bow turning to face the arrow, and the first shots of a fresh install leaving at once (until the
  mod has learned the bow's release sound, `bow_twang`, each shot is the game's own: your arms stay
  yours, but the arrow leaves a moment after you let go). `bow=0` always gives the game's own shots.

### Weapons the game transforms with L1

These keep the game's whole-body transform animation underneath (their buff or effect lives in it;
removing it needs work on the Windows build). What starts it *(new, untested)*:

- **Tonitrus** (one form): **hold the right grip and lift the weapon up**, over your head and
  pointing up (the fist 25 cm above its lowest since the squeeze, 5 cm above your eyes, within 60°
  of straight up). A build rises as you lift; at the top a crack, then crackling: its charge. Then
  10 s before the next (`charge_cooldown_s`; a squeeze before then: a double tick). Let go early:
  a soft tick.
- **Chikage:** the grab group: right grip, then grab the handle with the left grip (its L1); let go
  and it turns back. Form B is two-handed.
- **Bloodletter (DLC):** **hold the right grip** to keep it armed (a heartbeat in both hands, no timeout),
  then drive either end into yourself: its back, the hilt end (held as it is, up to 25 cm behind
  your fist), into your chest or face, or turn it round and thrust its head at least 10 cm toward
  your chest (its head reaching within 16 cm of a point 32 cm
  below your eyes, at 0.6 m/s along its line): a deep thud through both hands and its self-wound
  buff (its L1). The right grip in form B turns it back. Form B is two-handed. [missing: the game's
  sound at the arming; the mod has no way yet to play a game sound from the controls]
  The stab needs only the right arm; losing left-controller tracking does not block it.
- **Logarius' Wheel:** no gesture. It keeps L1 because an effect lives in its closing animation.
  Form B is two-handed. It is the slowest weapon (light pace 1.09 s / 1.22 s).

### Claws and the parasite: Beast Claw, Kos Parasite (DLC)

- **Beast Claw:** form A is a claw on your right hand. **Hold the right grip 2 s** to transform
  *(new, untested)*: a build, then a thump. In form B your right hand swings the right
  claw (light and heavy; the charged swing is the heavy) and **your left hand swings the left claw**
  ([left-hand swings](#left-hand-swings-new-untested)). No flick attacks.
- **Kos Parasite:** form A strikes with your right hand. In form B your right-hand swings play the
  game's own attack animations (all its form-B hits are the left arm's parasite), and the parasite
  also attacks when you swing your left hand. Its transform is the game's L1. Its pace is a stand-in
  (the Saw Cleaver's), because its own data was not in the extracted files.
- **No right-hand weapon (bare fists):** there are no physical moves for it, so your swings press
  the game's own punches (R1 / R2, with their animations).

---

## 3. The left hand

### Guns

**Pull the left trigger** and the gun in the hunter's hand fires at once, from its muzzle, along its
visible barrel: where the gun points, wherever he faces (`gun_aim=1`, `gun_fire=direct`).

- **Wrist-flick reload** (VR Settings > Weapons, `gun_wrist_reload=1`, on by default):
  after a confirmed shot, release the left trigger and flick in any direction once the 100 ms
  post-shot guard has passed. No still-wrist pause is required. Within a rolling 180 ms window,
  turn at least 26 degrees and reach 5.5 radians/second. **Close to the body** (the hand within
  about 38 cm of your neck, horizontally, scaled by your height; or its position only inferred
  by the headset), the flick must reach **6.5 radians/second**, with the same 26-degree turn.
  These current thresholds have automated coverage but still need headset validation.
  Only the controller's orientation must be tracked. Motion before the post-shot guard does
  not count; lost orientation and tracking jumps reset the gesture history. A moving hand can
  reload without first returning to rest.
  In builds with logging enabled, reload diagnostics include the measured peak speed, required
  speed, turn, duration, and why the hand counted as near the body. Failed attempts are rate
  limited to four lines per second. These per-attempt logs are disabled in no-logs releases.
  A short mechanical reload sound confirms it is ready; pulling before reloading plays an empty
  click and does not fire or spend bullets. The game's ammunition costs and firing cadence still
  apply. The setting applies immediately and is saved in `vr.ini`.
  It applies to both direct and vanilla left-hand firearm shots, including cannons.
  Continuous-fire Gatling/sprayer weapons are exempt from the chamber requirement. Bows, shields,
  quick items and two-handed specials retain their controls. Turning it off clears the requirement.
  The embedded sounds are CC0 recordings by Brian MacIntosh and LFA; source links and edits are in
  [the sound credits](../src/resources/vr/GUN_RELOAD_AUDIO_LICENSE.txt).
  Regression coverage includes brief pauses, wind-ups, twitches and quick aim changes before a
  flick, slow finishes, expired flicks, settling time, small shakes, tracking loss and gesture
  replays at 60, 72, 90 and 120 Hz. Headset feel remains to be tested.

- **Locked on**, a gentle nudge: within 4° of the target the shot turns 27% of the way toward it
  (at most about 1.2°), fading to nothing at 8° (`gun_assist`, `gun_assist_deg`). Not locked on:
  no help (`gun_assist_unlocked=1` changes that). The laser is off by default (VR page "Gun laser",
  `laser=1`).
- **Rate** is the gun's own. A pull in the last 0.12 s before the gun is ready fires the moment it
  is; an earlier pull gets a light tick. **Empty:** a double click.
- **Bullets** as in the game: Blood Bullets first, then Quicksilver Bullets. Bone Marrow Ash gives
  its empowered shot.
- **The gun parry** is the game's. With no draw animation it is easier to time; then thrust for the
  visceral.
- **During a hit or a weapon switch** the game has the arm: your pull waits up to 0.25 s
  (`gun_buffer_ms`), then becomes the game's own L2 shot (with its draw), still along your barrel.
  During a backstep, quickstep or roll your arm stays yours, so the shot fires at once
  (`dodge_attack`) and ends the dodge's invulnerability.
- **With the vial armed**, the pull puts it away and fires.
- **In two-handed forms** the gun is on your hip and the left trigger does nothing, unless the
  weapon is in `l2_canned` ([two hands](#two-hands-on-the-weapon)); then it goes to the game as L2 (in
  the mock survey nothing fired).
- Left-hand damage is +10% (`offhand_damage_pct`).
- **The kick:** your controller stays put; the hunter's hand flips muzzle-up and back
  (`gun_recoil_visual`) and your controller thumps (`gun_recoil_gain` scales it, 0 to 1.5).

| Gun | Fires | Shot to shot | Hand kick | Thump |
|---|---|---|---|---|
| Hunter Pistol, Evelyn | at the pull | 0.87 s | 10°, 2.5 cm | sharp 25 ms, fading over 90 ms |
| Repeating Pistol | at the pull (2 bullets a shot) | 0.87 s | 12°, 3 cm | as the pistol |
| Hunter Blunderbuss | at the pull, its fan of pellets centred on the barrel | 1.17 s | 14°, 4 cm | 40 ms, fading over 140 ms |
| Ludwig's Rifle | at the pull | 1.4 s | 14°, 4 cm | 40 ms, fading over 140 ms |
| Piercing Rifle (DLC) | the game's L2 with its draw, along the barrel | the game's | 14°, 4 cm | 40 ms, fading over 140 ms |
| Cannon | the game's L2 with its draw, along the barrel | the game's | 22°, 6 cm | the heaviest: 90 ms, fading over 300 ms |
| Church Cannon (DLC) | the game's L2 with its draw; along the barrel when the mod finds its mount point in the game files, else the game's aim | the game's | 16°, 4.5 cm | 60 ms, fading over 180 ms |
| Gatling Gun (DLC) | the game's L2 held: a stream, along the barrel | the game's | 2.5°, 6 mm | a rattle: 18 ms a shot |
| Flamesprayer, Rosmarinus | the game's L2 held: a stream, along the barrel | the game's | none | [unverified: they get the pistol's thump on each shot the game reports; how often was not measured] |

### The pistol punch *(new, untested)*

With the Hunter Pistol, Evelyn or the Repeating Pistol in your left hand, **punch** with it: the
bare fist's attack, thrown with the gun in your hand.

- It needs a real strike (your hand at 3 m/s). Not with the trigger pulled past 20%, not within
  0.5 s of a shot, and a punch followed by the trigger within 150 ms counts as aiming (dropped,
  free).
- First, **punch once with an empty left hand** (the game's own L2 punch) so the mod learns the
  fist's attack; it is kept in vr.ini (`melee_left_rows`, the `1000:` entry).
- The first gun punch of a session checks that its hit lands at your left fist; if not, gun punches
  stay off for that session (logged).
- [unverified: whose damage the punch uses, the gun's or the right weapon's]

### Shields *(new, untested)*

With the Wooden Shield or the Loch Shield (DLC), **hold your left hand up in front of you** for a
tenth of a second and the game guards (L2 held) until you lower it (`shield_raise=1`).

| | To raise it | Once it is up |
|---|---|---|
| In front of your eyes | 18 cm or more (`shield_raise_cm`) | 3 cm or more |
| Height | from your upper chest (38 cm below your eyes) to 15 cm above them | 15 cm more room each way |
| Sideways | from 45 cm to your left to 30 cm across your body | 15 cm more room each way |

- It only drops after 0.2 s outside that room, so a duck, a lean or a twitch never flickers it.
  "In front" means in front of your body, not where you look. It stays up through a tracking dropout
  of up to 0.3 s (your other hand crossing in front).
- A firm tick when it comes up, a soft one when it goes down. Blows on it thud by their size; a
  guard break is a long shudder through the arm.
- Squeezing the left grip while it is up (bracing) never opens the quick-item arc. The left trigger
  is still the game's L2.
- Not with the quick-item arc open, an item or vial in your left hand, the right weapon in a
  two-handed form (the shield is put away), or classic controls.
- An older vr.ini that says `shield_raise_cm=20` keeps the old, stricter reach: change it to 18 or
  delete the line.

### Torches: Torch, Hunter's Torch (DLC)

- **Swing your left hand** to attack with it (see
  [left-hand swings](#left-hand-swings-new-untested)). The left trigger is still the game's own
  torch attack, with its animation.
- **Hold it up for more light** *(new, untested, `torch_lift=1`)*: raised to your shoulder (within
  30 cm below your eyes) it burns with one more copy of its flame; held at or above your eyes, two.
  A level drops only after the torch has been 10 cm lower for 0.4 s, so swinging it never flickers.
  [unverified: the mod must find the torch's flame among the game's effects; if it cannot, nothing
  is added and the log says so]

### Fists: bare fist, Fist of Gratia (DLC)

**Swing your left hand to punch.** First do one of the game's own punches with the left trigger, so
the mod learns the attack (kept in `melee_left_rows`). The left trigger keeps the game's own punch.

### Left-hand swings *(new, untested)*

`melee_left=1`: anything that attacks from your left hand attacks when you swing your left hand, as
your right hand does, with no animation: the second blade of the transformed Blade of Mercy and
Rakuyo, the transformed Beast Claw's left claw, the transformed Kos Parasite's parasite, the
torches, the Fist of Gratia, the bare fist and the pistol punch. Not in a two-handed form.

- **Light swings only**, paid from the right hand's charges (one pace for both hands), at
  `melee_stamina_pct`. No charge left: the dull buzz.
- **Your off hand swinging along** with a big right swing is ignored (free and silent) when the left
  hand has its own weapon (torch, fists, pistol punch): a left swing within 150 ms of a right one,
  either first. A left swing more than 150 ms after is a combo and counts. With the dual blades, the
  claw and the parasite both hands always count.
- **Learning:** each left attack is learned once and kept in vr.ini (`melee_left_rows`; delete the
  line to learn again). The dual blades' second blade, the Beast Claw's left claw and the Kos
  Parasite's parasite learn from **your first left swing** *(new, untested)*: it tries the form's
  attacks in turn and keeps the one whose hitbox is at your left hand. Torches and fists: one press
  of the left trigger (the game's own attack shows it). Pistol punch: one empty-handed punch. Until
  then a left swing gets a double tick.
- Your left hand at your face (the strap, the headset) holds left swings back until it is away and
  slow again.

### Hunter tools: the Augur of Ebrietas *(new, untested)*

`tool_aim=1`: use the Augur from the quick-item arc as usual (it plays the game's item animation;
the burst leaves about 0.5 s later). The burst flies **where you look** (where your head points) if
your left hand pointed there (within 20°) during the 0.8 s before it leaves, so thrust your hand at
what you look at. Otherwise it flies where your left hand points as it leaves; with neither, the
game's aim. You feel a heavy push out of your palm. A Call Beyond and the Blacksky Eye keep the
game's own aim (not recognised yet).

### Papers and the Phantasm Shell: wipe them along your blade

`quick_use_wipe=1`: Fire Paper, Bolt Paper and the Empty Phantasm Shell stay in your left hand when
you take them from the arc. **Rub them along your weapon's blade** (short strokes adding up to
18 cm within 1.5 s, or one long wipe) and the game's buff takes at once, with no animation: a rasp
in both hands while it touches, a thump when it takes. Ticks quicken as the paper nears the blade.
Let go of the trigger away from the blade and the item goes back unused. Bone Marrow Ash goes on the
gun and is used at the pull.

In physical whip form, the Threaded Cane and Beast Cutter accept paper along their posed blade
chain, including the reachable blades below the handle. Follow the curve with short strokes;
crossing a link joint keeps counting. The bare handle and empty space across a bend do not count.
Hold the weapon roughly still: swinging it past stationary paper does not apply the buff.

Papers still use the game's weapon restrictions (Blade of Mercy cannot take paper; Rakuyo can).
Applying paper and transforming both go through the game's native functions. The second blade
of a split weapon also attacks as part of the right-hand weapon, retaining its buff ownership;
the two-blade flame display after a split still needs a headset check.

The Blood Vial and the other quick items (thrown by hand, eaten at your mouth) are not weapons:
see the [controls guide](controls.md).

---

## 4. Settings quick reference

Only the weapon-related keys; edit them in vr.ini. A few are also on the in-game VR page
(Options > VR): Stance (`seated`), Gun laser (`laser`), Light attack stamina
(`melee_stamina_pct`), Instant ragdoll and Ragdoll push, Shoulder weapon switch, and Solid weapons
(`weapon_wall`).

**Swings and pace**

| Key | Default | Meaning |
|---|---|---|
| `melee` | physical | `physical`: the swing is the attack; `vanilla`: a swing presses R1 / R2 (the game's animations). Restart to apply |
| `melee_open` | onset | the hit opens as the stroke starts (point-blank hits land); `commit`: only once the swing is certain |
| `heavy_arc` / `heavy_arc_deg` / `heavy_windup_deg` | 1 / 140 / 100 | a big wound-up swing is a heavy / its sweep (°) / how far back the blade must start (°) |
| `heavy_ready_tick` | 1 | the soft click when you are wound up |
| `seated` | 0 | 1 = seated play: smaller heavy arc, wind-up and arm travel |
| `blade_pitch_deg` | 0 | the blade's angle on the controller (−60 to 60); adjust if wind-ups are misread |
| `melee_charge_s` | 1 | right trigger held this long: charged (0.35 s: heavy) |
| `melee_speed_damage` | 1 | damage ×0.75 to ×1.10 by blade speed; 0 = the game's damage |
| `melee_radius_scale` / `melee_radius_fit` | 0.7 / 1 | trim the game's hit area round the blade |
| `melee_budget` / `melee_budget_charges` | 1 / 2 | the quick charges and how many |
| `melee_budget_scale` / `melee_budget_offset_ms` | 1 / −80 | when charges come back: the game's recovery × scale + offset (larger = slower) |
| `melee_stamina_pct` | 50 | light swings' share of the game's stamina cost (heavy and charged always 100); 0 = free |
| `melee_regen_pause` | 1 | no stamina regen after a full-cost swing |
| `melee_telegraph` | heavy | which swings enemies can read: `heavy`, `all`, `off` |
| `melee_haptics` / `melee_bite` / `melee_whoosh` / `melee_trail` | 1 / 1 / 1 / 1 | swing feedback / the blade's bite on a hit / swing sound / blade trail |
| `dodge_attack` / `dodge_attack_iframes` | 1 / 0 | swing or shoot during a dodge / keep the dodge's invulnerability when you do |

**Viscerals, plunge, kills**

| Key | Default | Meaning |
|---|---|---|
| `visceral_thrust` / `visceral_swing` | 1 / 0 | the visceral is a thrust / a swing starts it too |
| `visceral_thrust_speed` / `visceral_thrust_cm` / `visceral_thrust_align_deg` | 1.5 / 10 / 45 | the thrust's speed (m/s), travel (cm) and how straight (°) |
| `visceral_cue` | 1 | the heartbeat when a visceral opens: 0 off, 1 once, 2 repeated |
| `visceral_pull` / `visceral_freehand` | 1 / 1 | tracked stab-and-rip (headset validation pending); freehand=0 restores the legacy handover |
| `visceral_pull_cm` / `visceral_pull_mps` / `visceral_pull_timeout_s` | 12 / 0.9 / 3 | withdrawal distance; speed applies only to legacy mode; timeout auto-rips (full damage in freehand) |
| `melee_plunge` / `melee_plunge_min_drop_m` / `melee_plunge_damage` | 1 / 1.2 / 1.5 | the plunge; the drop (m) that counts; its damage × |
| `melee_kill_ragdoll` / `melee_ragdoll_impulse` / `melee_ragdoll_vanilla_pct` | 1 / 1 / 22 | blade kills go limp / how hard they are pushed / % that keep the game's death |

**Transform and two hands**

| Key | Default | Meaning |
|---|---|---|
| `transform` | native | `native`: only the weapon transforms; `vanilla`: the game's L1 animation. Restart to apply |
| `transform_flick` / `transform_attack` | 1 / 1 | a squeeze mid-swing transforms at once / and gives the transform attack |
| `transform_retract_speed` / `transform_speed` | 1.75 / 1 | folding back plays faster up to the switch / the whole clip's speed |
| `transform_buffer_ms` | 400 | a squeeze while the game has your arm waits this long |
| `transform_cues` / `transform_haptics` | 1 / 1 | the clip's sounds and sparks / the tick, detents and clack |
| `hand_transform` / `hand_transform_pull_cm` | 1 / 18 | the right grip's uses and the transforms by hand (new, untested) / how far a split pulls (cm) |
| `charge_cooldown_s` | 10 | Tonitrus' charge and the Boom Hammer's ignition: none again for this long (s) |
| `l2_canned` | bloodletter,kos_parasite | the weapons whose two-handed form keeps the game's own left-trigger special |
| `two_hand_grip` / `two_hand_zone_cm` | 1 / 9 | the two-handed grip / how close to the handle the left hand must be (cm) |
| `two_hand_guide` | 1 | faint off-hand target while holding right grip; Weapons > Grip guide |
| `two_hand_release_cm` / `two_hand_twist_deg` | 25 / 75 | legacy keys, no longer release a held grip |
| `two_hand_damage` / `two_hand_relative` | 1.05 / 1 | two-handed swings' damage × / steer from the grab's angle (no jump) |

**Special weapons**

| Key | Default | Meaning |
|---|---|---|
| `whip` | physical | the whips' form B is a simulated chain; `canned`: the game's animations |
| `whip_payout_speed` / `whip_hold_s` / `whip_reach_scale` | 5.5 / 0.6 / 1 | swing speed for full payout (m/s) / slow this long to reel in (s) / reach × |
| `whip_flick_recall` | 1 | the Beast Cutter's whip (not the Threaded Cane): a snap of the wrist lifting the weapon back, with the chain out, reels it straight in (a snap thrown out is a crack); `any`: a snap in any direction; `0`: off |
| `whip_pace` | 1 | whip lashes register quickly but deal at most the game's own damage per second (quicker lashes hit for less each; the controller's impact stays full); 0 = every registered lash hits in full |
| `bow` / `bow_quiver` / `bow_direct` | 1 / 1 / 1 | the real bow / arrows from over your shoulder / the arrow leaves at the release |
| `bow_draw_cm` / `bow_nock_cm` | 60 / 27 | full draw (cm) / nocking distance from the arrow rest (cm) |
| `bow_arrow_pitch_deg` | 45 | the arrow carried in the string hand points this far below a sword's line (forward) |
| `bow_min_draw` / `bow_full_draw` / `bow_charge_ms` | 0.15 / 0.85 / 600 | below this draw no shot / full draw / held this long: charged |
| `gun_blade` / `moon_wave` / `stake_blast` | 1 / 1 / 1 | the Reiterpallasch's shot and the Rifle Spear's blast / the moonlight wave / the stake blast |
| `saw_spin` | 1 | the Whirligig held with both hands: either trigger spins it |
| `weapon_hum` / `weapon_wall` | 1 / 0 | the Whirligig and Boom Hammer hum / solid weapons: they stop at walls and each other, with a thud, a buzz and a few sparks (off: none of it runs) |
| `trick_haptics` | 1 | the feel of all the trick-weapon features |

**Left hand**

| Key | Default | Meaning |
|---|---|---|
| `gun_aim` / `gun_fire` | 1 / direct | shots along the barrel / fire at the pull (`vanilla`: the game's L2 with its draw). Restart to apply |
| `gun_assist` / `gun_assist_deg` / `gun_assist_unlocked` | 0.27 / 8 / 0 | the locked-on nudge / its cone (°) / also when not locked on |
| `gun_busy` / `gun_buffer_ms` | vanilla / 250 | a pull while the game has the arm waits this long, then the game's own L2 shot |
| `gun_rate_scale` | 1 | gun cadence (down to 0.6 = faster) |
| `gun_recoil_visual` / `gun_haptics` / `gun_recoil_gain` | 1 / 1 / 1 | the hand's kick / gun feedback / thump strength |
| `laser` | 0 | a laser along the barrel |
| `offhand_damage_pct` | 10 | extra damage (%) for the left-hand weapon |
| `shield_raise` / `shield_raise_cm` | 1 / 18 | the shield held up blocks / how far in front of your eyes (cm) |
| `torch_lift` / `torch_lift_copies` | 1 / 2 | the torch burns brighter held up / at most this many extra flames |
| `melee_left` / `melee_left_rows` | 1 / (empty) | left-hand swings / their learned attacks (delete to relearn) |
| `tool_aim` | 1 | the Augur aimed by your hand and gaze |
| `quick_use_wipe` | 1 | papers and the shell are wiped along the blade |
| `shoulder_switch` | 1 | hand at a shoulder + B / Y switches that hand's weapon |

---

## 5. Not yet tried in the game

Added on 2026-10-12 and not yet run in the game (designed and unit-tested, some checked in the mock
runtime):

- The right grip's uses: freeing the left hand and the grab group (form B while both hands hold
  it), the dual blades' split and join, the Bloodletter armed and stabbed, Tonitrus lifted, the
  Boom Hammer and Beast Claw held, the cooldowns; the left trigger's `l2_canned`.
- The shield held up, and its slightly more forgiving reach.
- The Whirligig Saw's spin (either trigger with both hands on it), with weapon animation,
  native grinding attacks and blade sparks.
- The right hand's shots: the Reiterpallasch, the Rifle Spear's and the Stake Driver's charged
  blasts, the moonlight wave on a charged swing (and its aim where you look).
- Left-hand swings: the dual blades' second blade, the claw, the parasite, torches, fists, the
  pistol punch, and the shared pace.
- The Whirligig Saw and Boom Hammer hum; the Augur aimed where you look.
- Simon's Bowblade as a real bow: the quiver, the visible arrow, arrow stamina.
- The torch held up for more light.
- The plunge; solid weapons; pulling your hand out of a visceral (not even in the mock).

Open questions the docs list: which hand the game hangs Simon's bow on; whether the shield's guard
stance takes over the hunter's left arm; which of the game's rows are the moonlight wave and the
stake blast; whether the default handle point suits each grab weapon's form A; which rows the left
parts attack with and how the left-hand damage bonus treats them; the game's own plunge
values; whether solid weapons hold well at a wall and their sparks look right. And for everything:
how it feels on a real headset.
