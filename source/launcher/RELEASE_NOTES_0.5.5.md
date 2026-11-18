# Bldbrne-VR 0.5.5

0.5.5 includes everything from 0.5.2, which was never published, plus the fixes below.

New in 0.5.5:

- Fewer and shorter hitches when new areas stream in: memory for the game's buffers grows in larger steps instead of being rebuilt again and again (one Sewers hitch went from 90 to 51 ms).
- The game's copy shader no longer forces a full GPU flush about once a frame.
- Freed graphics memory is released in the background instead of on the render thread, and small buffers share memory blocks instead of each taking its own.
- In-game menus are sharp again with foveated rendering on: the pause and equipment menus, echoes and insight are drawn at full detail, only the 3D world is foveated.
- Stronger foveated rendering by default (full detail in the centre, lower detail sooner towards the edges) for more GPU headroom.
- Foveated rendering now also works in DIBR mode.
- The HUD is drawn in the game's own render pass (about 4% less GPU time per frame).
- Slightly stronger aim help for thrown items (Molotovs, knives, pebbles and the rest), locked on or not.
- Melee: retuned charge timing, and heavy swings need a more deliberate motion.
- Graphics settings: a new upscaler, preset or scale waits for SAVE AND RESTART instead of half-applying live (DLSS 5's sliders still apply at once).
- Fixed: with Height set to As the player, a recenter from the headset's own button (Meta Link) could make you very short. The floor now comes from the headset's room floor when its two floor readings disagree.

Everything from 0.5.2:

- Faster on Intel CPUs: Windows no longer moves the game to power-saving cores, and the instructions Intel CPUs lack (SSE4a) are replaced in advance instead of being caught one by one while you play.
- Intel black tonemap fix: applied automatically on Intel CPUs.
- Larger per-frame GPU command heap (1 MiB) for everyone.
- Upscaler presets: Ultra Performance (40%), Performance, Balanced, Balanced Plus, Quality and Ultra Quality (67%). DLSS uses its faster model (K) for most presets and its best model (M) for Balanced Plus and Ultra Quality. Balanced Plus renders at Balanced size; with FSR it is the same as Balanced.
- The performance scan no longer fixes the DLSS model in your settings, so each preset uses its own model. A model fixed by an earlier scan is reset when you launch.
- Fixed foveated rendering is on by default where the GPU supports it: the edges of each eye are drawn at lower detail. It applies only while you are in the world, not on the title screen, menus or the virtual screen.
- Lower CPU cost on the render thread: faster uploads, fewer redundant waits, and the next draw is fetched ahead of time.
- On SteamVR (for example PSVR2), HUD pieces partly hidden by your hands now fade smoothly instead of disappearing whole. The echoes and insight counters fully hide when something is in front of them.
- Less graphics memory: about 1 GB less at 2000 x 2160 per eye with DLSS Quality. DLSS reads the scene in place and writes a lighter output, large images get their own memory, the lock-on marker's images are made at your first lock-on, and the hands, controllers and vial overlay use 2x anti-aliasing (was 4x; overlay_msaa=4 in vr.ini restores it).
- Faster render threads: the game sees GPU work finish sooner, fewer per-draw calculations, a memory leak fixed, and on Intel hybrid CPUs the render threads prefer the fast cores.
- Fixed: the body slots, pouch vials, quick items and wrist vitals could all disappear at some per-eye sizes (for example PSVR2 on SteamVR).
- Launcher: the per-eye size and an estimate of the graphics memory are shown under Scaling, with a warning when the size is above 2240 pixels wide or may not fit your card. A headset list (Quest, PS VR2, Index, Steam Frame, Pico, Vive, Reverb, Beyond and more) shows your headset's own panel size, and "Use panel size" sets the scaling to it. Shorter text throughout.

The release runtime uses ThinLTO without PGO. Normal play keeps only startup and crash diagnostics.
