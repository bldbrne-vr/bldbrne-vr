# Bldbrne-VR 0.8.6

- **Hunter tools, reborn.** Grab a tool and it stays in your hand, then use it for real: aim the Tonitrus and the Beast Claw with a swing, raise the Blacksky Eye to your eye, blow the Madaras Whistle, gather A Call Beyond with the trigger and lift it to the stars, throw the Accursed Brew like a Molotov. No canned animations: the act is the use. Out of bullets? You can't pick the tool up: you hear the empty-gun click and your wrist counter flashes red.
- **Parry bullets with your blade.** Swing into an enemy's shot and knock it away in a shower of sparks. Angle it right and it flies straight back at the shooter.
- **Bigger, bloodier viscerals.** The red-and-black burst hits harder. The first flash holds until you rip your hand back out, then the world breathes for a moment before the second one lands.
- **Heavy attacks feel earned.** A heavy now takes a real wind-up (blade cocked back, or a wide sweep) and comes at most once every 4 seconds. Hold the trigger for a heavy any time you're willing to pay its full stamina. Swing speed counts a little (5% up or down), and early enemies no longer melt in one hit.
- **Dodging by leaning is easier.** A smaller, gentler lean or duck now gives you the dodge.
- **Smoother frames.** Both eyes now share more of the GPU's work, plus a stack of CPU and GPU trims across the renderer.
- **FSR 4.1.2 for AMD cards** (experimental): AMD's own newest upscaler. It falls back to FSR 3.1.5 by itself if it can't start.
- **Foveated rendering in the VR menu**, live while you play, with a new Light level. Picture > Performance adds Water reflections, Far shadows and Fewer shadows when busy for extra speed.
- **Angled-screen headsets** (Pimax and friends) render each eye over its whole view.
- **Fixes**: faces no longer burst into triangles; the bow's release never groans; soft saves show when they were made; FSR 3.1.5 failing to start no longer crashes; the launcher's VR settings now match the in-game menu.

The release runtime uses ThinLTO without PGO. Normal play keeps only startup and crash diagnostics.
