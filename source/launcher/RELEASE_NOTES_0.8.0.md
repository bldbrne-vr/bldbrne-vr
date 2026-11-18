# Bldbrne-VR 0.8.0

- **Riposte burst** (VR settings > Damage, launcher > Feedback; on by default): a parry riposte or a backstab now flashes both eyes into a black, blood-red and white look with ink outlines, softer through the lunge and at full strength as the enemy is thrown back.
- **Weapons clash with their real shapes** (VR settings > Weapons > Solid weapons): your two weapons now meet each other where they really are, the folded Saw Cleaver's blade, an axe's head, the scythe's curve, instead of along a thin line. Pressed together they keep to the side they met on, and a gun pushed into the crook of a blade rests there. Walls still meet a line along the blade, as before.
- **DLSS and FSR in DIBR**: DLSS, FSR 4 and FSR 3.1.5 now also work with DIBR stereo, not only native stereo (launcher > Graphics > Upscaler).
- **AMD Radeon**:
  - RX 6000 cards: the right eye no longer turns white once the world loads in native stereo. Each eye is drawn on its own (launcher > Graphics > Native stereo eyes: Auto, Multiview or Layers; Auto picks Layers only on RX 6000).
  - With DLSS or FSR, the HUD is no longer doubled and the wrist display and held items show again.
  - Switching between DIBR and native stereo no longer recompiles the shaders.
  - The launcher recommends FSR 3.1.5 on AMD cards.
- **Lighter HUD**: the HUD's masks are drawn straight into the headset's view instead of being copied before each use (9 copies a frame become 1; most noticeable on AMD cards).
- **Lean dodge**: its invincibility lasts 250 ms (was 200).
- **Molotov fire**: a thrown Molotov's burst and the fire it leaves reach 1.5 times as far; the flames keep their size (vr.ini `molotov_fire_scale`, 1 to 3; 1 is the game's own).
- **PERF SCAN**: the suggested settings stand out in a gold box with a big Apply button right under them. The scan now runs on every supported game version, with or without The Old Hunters.
- Choosing a PKG file (or its folder) as the game now explains that the launcher needs the game's extracted folder, with eboot.bin.

The release runtime uses ThinLTO without PGO. Normal play keeps only startup and crash diagnostics.
