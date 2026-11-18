# Bldbrne-VR 0.8.1

- **New quick item menu** (VR settings > Inventory > Quick item menu, launcher > Inventory): your items on a fan round the left hand. Move the hand a little toward an item to light it (no need to reach it) and pull the hand away to close. Fan A (gothic panes of candle-lit glass) is the new default, also for existing installs that had the old one; Fan B has an engraved brass rim; Boxes is the old arc of cards.
- **Shoulder vial** (VR settings > Inventory, launcher > Inventory; on by default): reach over your right shoulder with the left hand and squeeze the grip to draw a Blood Vial. It uses the same broad shoulder zone as the weapon switch, so it is easy to find, and gives no touch vibration there.
- **Duck**: ducking now lowers the hunter like a crouch, so you can duck under a swing at your belly or hips. A quick duck (your head down fast, at least 18 cm) gives 125 ms of invincibility. Looking down or walking down stairs does not count.
- **Lean dodge** is a little easier to trigger (1.18 m/s, was 1.3). Settings you changed yourself are kept.
- **Healing**: drawing a vial from the pouch right after a heal now gives a full one, not the empty one. X waits 150 ms after a heal before it raises the next vial, so tapping it no longer heals over and over without a pause.
- **Item pickups**: items on the ground show their prompt from a little further away (10%).
- **HUD in VR**: the HP and items, echoes, insight and the build-up meter now sit in their own rows above the view, and the banners and area name below it, so they never overlap the prompts, item pop-ups or each other.
- **Fixed**: in Central Yharnam, the Tomb of Oedon and the Great Bridge, later in the story, the prompts, item pop-ups, the HUD (echoes and insight) and the game menu could all be missing. The game draws an extra effect there that the VR HUD capture mistook for the picture.
- **Fixed**: the game menu darkens the world behind it again.
- **Fixed**: after many lamp trips (Central Yharnam and the Sickroom, for example) the street could smear out to the horizon. Video memory left over from earlier areas is now freed when the card runs low.
- **Fixed**: an NPC standing close to you (Eileen in the Crow set, for example) no longer loses her mask, hood or more in Hands only or Arms only.
- **Foveation** is now even at the top and bottom of the view (it was strong at the bottom and weak at the top).
- **AMD Radeon RX 400, RX 500 and RX 5000** cards start in DIBR stereo by default. Native stereo on these cards draws each eye separately.
- **Steam Frame**: the left d-pad's down or right also works as X (Blood Vial), and up or left as Y.
- Grabbing from the body slots is more forgiving of a squeeze that comes a little early or late.
- Slightly lower CPU cost per frame in the renderer.

The release runtime uses ThinLTO without PGO. Normal play keeps only startup and crash diagnostics.
