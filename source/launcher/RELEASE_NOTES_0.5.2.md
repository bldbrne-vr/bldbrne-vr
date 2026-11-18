# Bldbrne-VR 0.5.2

- Faster on Intel CPUs: Windows no longer moves the game to power-saving cores, and the instructions Intel CPUs lack (SSE4a) are replaced in advance instead of being caught one by one while you play.
- Intel black tonemap fix: applied automatically on Intel CPUs.
- Larger per-frame GPU command heap (1 MiB) for everyone.
- Upscaler presets: Ultra Performance (40%), Performance, Balanced, Balanced Plus, Quality and Ultra Quality (67%). DLSS uses its faster model (K) for most presets and its best model (M) for Balanced Plus and Ultra Quality. Balanced Plus renders at Balanced size; with FSR it is the same as Balanced.
- The performance scan no longer fixes the DLSS model in your settings, so each preset uses its own model. A model fixed by an earlier scan is reset when you launch.
- Fixed foveated rendering is on by default where the GPU supports it: the edges of each eye are drawn at lower detail.
- Lower CPU cost on the render thread: faster uploads, fewer redundant waits, and the next draw is fetched ahead of time.
- On SteamVR (for example PSVR2), HUD pieces partly hidden by your hands now fade smoothly instead of disappearing whole. The echoes and insight counters fully hide when something is in front of them.
- Less graphics memory: about 1 GB less at 2000 x 2160 per eye with DLSS Quality (5.8 to 4.8 GB). DLSS reads the scene in place and writes a lighter output, large images get their own memory, the lock-on marker's images are made at your first lock-on, and the hands, controllers and vial overlay use 2x anti-aliasing (was 4x; overlay_msaa=4 in vr.ini restores it).
- Faster render threads: the game sees GPU work finish sooner, fewer per-draw calculations, a memory leak fixed, and on Intel hybrid CPUs the render threads prefer the fast cores.
- Foveated rendering applies only while you are in the world, not on the title screen, menus or the virtual screen.
- Fixed: the body slots, pouch vials, quick items and wrist vitals could all disappear at some per-eye sizes (for example PSVR2 on SteamVR).
- Launcher: the per-eye size and an estimate of the graphics memory are shown under Scaling, with a warning when the size is above 2240 pixels wide or may not fit your card. A headset list (Quest, PS VR2, Index, Steam Frame, Pico, Vive, Reverb, Beyond and more) shows your headset's own panel size, and "Use panel size" sets the scaling to it. Shorter text throughout.

The release runtime uses ThinLTO without PGO. Normal play keeps only startup and crash diagnostics.
