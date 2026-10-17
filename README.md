<!--
SPDX-FileCopyrightText: 2026 shadPS4 Emulator Project
SPDX-FileCopyrightText: 2026 BB-VR contributors
SPDX-License-Identifier: GPL-2.0-or-later
-->

# Night of the Hunt VR

An unofficial, independent VR mod for Bloodborne. No game files are included.

## Download

- [Installer (Windows)](https://github.com/bldbrne-vr/bldbrne-vr/releases/latest/download/BloodborneVR-vr-win-Setup.exe)
- [Portable (zip)](https://github.com/bldbrne-vr/bldbrne-vr/releases/latest/download/BloodborneVR-vr-win-Portable.zip)

## Supported game version

Bloodborne for PS4, version 1.09. Tested with CUSA03173 and CUSA00900.

## How to use

1. Install the mod (or extract the portable ZIP) and open `BloodborneVR.exe`.
2. Connect your headset and start your VR software.
3. Choose your **Game folder** — the Bloodborne 1.09 folder containing `eboot.bin`.
4. **Have an existing save?** Choose your shadPS4 `savedata` folder under **Save home folder**. Otherwise, skip this step.
5. Launch the game, put on your headset, and enjoy the hunt!

The first launch may stutter while shaders are built. Existing saves are used in place; use **Save backups** in the launcher to back them up before playing.

## Controls

These are the default **Motion** controls with **Move with: Left stick**, using Touch-style button names (X/Y left, A/B right). For your headset's bindings, choose **Show VR controls** on the in-game VR page.

### LEFT CONTROLLER

- **Move:** push the stick. **Run:** click it while moving; centre it to stop running.
- **Jump:** while running forward, press **A** on the right controller or click the left stick again. Steering away from your run direction makes A dodge instead.
- **Shoot / parry:** aim your gun and press the trigger.
- **Quick items:** squeeze the grip to open the item arc; use the trigger on a card to take or use it. With body slots enabled, grip at a pouch to take its item.
- **Heal:** press **X**, then bring the vial to your wrist or body. Press X again to put it away. With **Blood Vial: Button**, X heals directly.
- **Y:** open the item arc. On SteamVR's menu layout, Y opens the game menu instead; hold it to recenter.
- **Menu:** open the game menu; hold to recenter. On SteamVR, this button opens the dashboard instead.

### RIGHT CONTROLLER

- **Attack:** swing your weapon. **Heavy attack:** hold the trigger to charge, then swing.
- **Grip:** transform or prepare a physical weapon action; see the examples below.
- **Turn:** move the stick left/right. **Target / lock on:** click the stick.
- **Switch weapons:** hold the stick click and push left/right for that hand's equipped weapons.
- **Next quick item:** flick the stick down. **Blood bullets:** hold it up.
- **Dodge:** press **A**, steering with the movement stick. While running forward, A jumps.
- **Interact:** press **B** to talk, open doors or pick up items.
- **Roll:** while moving, press **A+B together**, or **B then A in quick succession**, to roll in the movement-stick direction. While locked on, this becomes a quickstep.

### Physical actions

- **Soft dodge:** lean, step or duck to move the hunter out of a blow. This physical evasion does not play a roll animation; the separate lean-to-roll option is off by default.
- **Switch at the shoulder:** with **Shoulder weapon switch** enabled, tap **B at your right shoulder** or **Y at your left shoulder**. Hold there for about **0.7 seconds** to go unarmed; tap again to draw your weapon back.
- **Two-hand a weapon:** squeeze the right grip to free your left hand, bring the left hand to the handle, then squeeze the left grip. Release **both grips** to let go. The Whirligig Saw skips the initial right-grip squeeze.
- **Split Blade of Mercy / Rakuyo:** squeeze the right grip, grip the hilt with your left hand, then pull apart. To rejoin, bring the blades together side by side and squeeze the left grip again.
- **Physical gestures:** near the Doll, bow, point/wave, or raise a hand for rally/approval. Make Contact is also recognised; see the guide below for its pose and encounter. The corresponding gesture must be learned. Hold **Y**, then press **B** for the normal gesture menu.
- **Levers:** with **Physical levers** enabled, grip and pull with either hand.

[Full controls and weapon guide](CONTROLS.md) — weapon-specific transformations, bow use, paired blades, gestures and alternate controls.

**Using different controls?** Hold both stick clicks for one second to switch Motion / Buttons. **Move with: Right stick** swaps sticks, stick clicks and face-button actions; triggers, grips and weapon hands stay in place.

## Options

The sections below explain the supported launcher options. Expand only the category you need. Choose **Save settings**, or launch to apply changes. Many options also appear in game; **Scenes on a screen** and **Camera** are launcher-only.

Defaults below are for a fresh configuration of this version; existing saved settings take precedence. Advanced `vr.ini` tuning is outside this quick guide.

<details>
<summary>General</summary>

| Option | Default | What it does |
| --- | --- | --- |
| **Height** | As the hunter | As the hunter: his eye height. As the player: your real height (from the headset's floor). |
| **Adjust height** | 0 cm | Taller or shorter, from the choice above. Also corrects a floor that is a few cm off. |
| **Move with** | Left stick | Choose the movement stick. Also swaps stick clicks and face-button actions; weapon hands stay the same. |
| **Turning** | Snap 30° | Turn in fixed steps (snap) or continuously (smooth). |
| **Vibration** | 7 / 10 | Strength of every vibration. 0 turns it off. |

</details>

<details>
<summary>Controls</summary>

| Option | Default | What it does |
| --- | --- | --- |
| **Control style** | Motion | Motion: swing to attack. Buttons: the classic layout. |
| **Blood Vial** | Stab to inject | Stab the vial into your wrist, or use a button. |
| **Gun laser** | Off | A thin line from the gun's barrel. |
| **Grip adjust** | On | Stand still and hold A+B for about 1.2 seconds. Align your controller with the frozen weapon, then release to save the grip. |
| **Stance** | Standing | Seated: smaller swings and throws, the lap counts for the vial. |

</details>

<details>
<summary>Comfort</summary>

| Option | Default | What it does |
| --- | --- | --- |
| **Body near your eyes** | Fade | Full body only: fade his coat where it comes close to your eyes. |
| **Hunter's body** | Full | See the hunter's whole body, only his hands and forearms, or only his hands. |
| **World size (eye distance)** | Standard (64 mm) | A smaller eye distance makes the world look larger. |
| **Scenes on a screen** | On | Show grabs, viscerals, doors and ladders on a virtual screen. |

</details>

<details>
<summary>HUD</summary>

| Option | Default | What it does |
| --- | --- | --- |
| **Health & stamina** | Wrist gauge | On your wrist, or in the corner like the game. |
| **Low-health warning** | On | A heartbeat in both hands and dark edges when your health is low. |
| **Echoes & Insight** | Look down | Look down: they show when you look down at your feet. |
| **Hit flash** | On | The edges of your view flash red when you are hit. |
| **HUD size** | Medium | The size of the health bars, items and prompts. |

</details>

<details>
<summary>Assists</summary>

| Option | Default | What it does |
| --- | --- | --- |
| **Extra health** | +10% | More maximum health for the hunter. |
| **Longer dodge invincibility** | +1 frame | Extra invincibility after a quickstep or roll, in 1/30-second increments. Does not apply to backsteps. |
| **Instant ragdoll** | On | An enemy your blade kills goes limp at once. Off: its death animation. |
| **Ragdoll push** | Normal | How hard your swing throws the body. None: it just falls. |
| **Light attack stamina** | 80% | Light swing cost. Strong swings cost 100%; low stamina makes them light. |

</details>

<details>
<summary>Display</summary>

| Option | Default | What it does |
| --- | --- | --- |
| **Anti-aliasing** | SMAA | Game FXAA, dual-eye SMAA, or None. |
| **Distant detail** | Performance | Performance uses simpler distant models; Balanced adds detail; Quality keeps full models at a higher rendering cost. The in-game page indicates whether a change applies immediately or at the next start. |
| **Show enemy health bars** | Off | The bars over enemies you hit or lock on to. Boss bars always show. |
| **Camera** | First person | Choose the initial perspective. |

</details>

<details>
<summary>Body</summary>

| Option | Default | What it does |
| --- | --- | --- |
| **Tracked posture** | Lean | Lean keeps the spine's length as you move your head. |
| **Side lean limit** | 20° | How far the hunter leans sideways. More lean lets you move farther before he steps. |
| **Extra clipping protection** | Off | Protect sideways leans and restore the body in stages after a roll or hit. |
| **Posture smoothing** | 50 ms | Less smoothing follows faster; more softens small movements. |

</details>

<details>
<summary>Inventory</summary>

| Option | Default | What it does |
| --- | --- | --- |
| **Item access** | Menu | Choose the quick-item menu, body pouches, or both. Body pouches use the left grip; pocket/chest items follow equipped quick slots 1/2. |
| **Pouch touch feedback** | On | A small vibration when your hand finds a pouch. |

</details>

<details>
<summary>Feedback</summary>

| Option | Default | What it does |
| --- | --- | --- |
| **Low-health red edges** | On | A restrained red edge below 30% health. Takes priority over stamina color feedback. |
| **Health intensity** | 50% | Strength of the low-health red edge. |
| **Exhaustion desaturation** | On | Briefly soften colors at exhausted stamina, then fade back. Never full grayscale. |
| **Stamina intensity** | 50% | Strength of the brief exhaustion effect. |
| **Shoulder weapon switch** | On | B at right shoulder / Y at left: tap to switch, hold about 0.7 seconds for unarmed. Tap unarmed to restore the remembered weapon. |

</details>

<details>
<summary>Weapons</summary>

| Option | Default | What it does |
| --- | --- | --- |
| **Solid weapons** | Weapons only | Stop at walls and each other. Weapons only: just each other. |
| **Grip guide** | On | Hold the right grip to show where your left hand can grab. |
| **Swing feel** | 100% | Faint feedback during fast swings with both hands on one weapon. |
| **Wrist-flick reload** | On | After a shot, release the trigger and flick your left wrist firmly in any direction to reload. |
| **Blade sounds** | On | Your weapons ring when they meet; draw one along the other to sharpen it. |

</details>

<details>
<summary>Interactions</summary>

| Option | Default | What it does |
| --- | --- | --- |
| **Physical levers** | On | Grip a lever with either hand and pull. Off: original interaction. |
| **Lamp rest & travel** | On (experimental) | Rest, refill and travel between lit lamps. Offline only. Experimental. |
| **VR Grab attacks** | Off | Stay in VR during enemy grabs. The view follows the hunter. Off: existing behavior. |

</details>

<details>
<summary>Color</summary>

| Option | Default | What it does |
| --- | --- | --- |
| **Contrast** | 0% | Light and dark separation. 0 is unchanged. |
| **Saturation** | 0% | Color intensity. -100 is monochrome; 0 is unchanged. |
| **Gamma** | 0% | Midtone brightness. Higher is brighter; 0 is unchanged. |

</details>

<details>
<summary>Graphics</summary>

| Option | Default | What it does |
| --- | --- | --- |
| **Scaling** | 0% (headset resolution) | Changes resolution relative to what the headset requests. Higher values give a sharper image at a higher rendering cost. |

</details>

The in-game page also has **Show VR controls** and a read-only headset resolution display. Its **Smooth motion** option is inactive with the bundled launcher preset.

---

## Source

A source snapshot is in [source](source).

## Affiliation and rights

Night of the Hunt VR is an independent fan project. It is not affiliated with, endorsed, sponsored, or approved by Sony Interactive Entertainment, Sony Group Corporation, FromSoftware, or the shadPS4 project.

Bloodborne and PlayStation are trademarks of Sony Interactive Entertainment. Rights in Bloodborne and its game content remain with Sony and the respective rights holders. Other names and trademarks belong to their respective owners. No ownership of those rights is claimed by this project.

No game files are included. You must provide your own lawfully obtained copy of Bloodborne. Please do not request or share game files here.

For software licences, third-party notices and attribution, see [Notices](source/NOTICE.md), [Licence](source/LICENSE) and [Credits](source/CREDITS.md).
