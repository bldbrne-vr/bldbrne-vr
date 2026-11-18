# Bldbrne-VR 0.8.2

- **Accurate game timing**: the game now measures every frame and moves by its real length (bbport's frame-rate hooks) instead of the old 60 FPS patch, which assumed a fixed frame. Walking, dodges, attacks, camera smoothing, cloth and effects stay right at any frame rate, through dips and frame-rate caps; the old patch was only exact while the game held its rate. On for everyone, existing installs too. The frame rate is the same as before.
- Our own VR timings (the gun punch check, HUD fades, the leap's hold, the blade wipe) follow the game's frame rate too.
- **FSR Balanced Plus** renders at 62% (it was the same as Balanced, 58%): a step between Balanced and Quality. The launcher's preset description and the VR page list every preset's size.
- **DLSS** is a little lighter on the GPU: with the game's 8-bit picture, the colour it reads is copied at 8 bits.
- **PERF SCAN** asks for 13.8% headroom above your headset's rate (was 13.5%).

The release runtime uses ThinLTO without PGO. Normal play keeps only startup and crash diagnostics.
