# Bldbrne-VR 0.9.6

- **New graphics.** Yharnam, relit: lanterns, torches and gunshots cast real shadows, fog that stays where it hangs and is lit by lamps and the moon, moonlight in the air with rays through the trees, deeper ambient shading, bounced light, water that mirrors the scene and wet streets. The game picks what your graphics card has room for (12 GB and 16 GB cards get more; cards under 10 GB keep the game's own look), and the parts have their own switches on the Picture page, in the game and in the launcher (Fog needs Moon glow on). Needs DLSS or FSR.
- **A film look for everyone.** Softer highlights, a tone filter, eyes that adjust when you step from light into dark, and cobbles and bricks with real depth.
- **Dismemberment.** The Beast Claw, the Burial Blade's scythe and a hard enough blow take limbs off: enemies come apart along their own bones, the pieces fly, land and stay.
- **Open viscerals you can feel.** Draw your hand back, up or aside before the stab; the Beast Claw punches.
- **Cutscenes framed as the game shot them,** then the camera is handed back to your head.
- **The blood moon changes the night.** Where the game hangs it, the moon's glow turns with it.
- **Sharper weapons.** A honed split blade shines, the Holy Moonlight Sword's wave tilts with your slash, Simon's Bowblade gets a charged sword swing, the Boom Hammer explodes where it lands, and a swing that hits nothing gives its quick charges back.
- **Faster.** Less work for the processor and the graphics card in every frame, smoother arrivals, and a shader cache that survives more.
- **Headsets.** Meta Link starts more reliably and recovers when the link drops; the game waits for your headset's runtime to finish starting.
- **Fixes**: Ebrietas' runaway trail, Call Beyond's sky, menus that confirm with the right button, ladders, status effects that reach the lenses, and many small ones.
- On upgrade, the old **Water reflections** setting resets once to your graphics card’s default. To choose your own, open **Picture → New graphics → Wetness** in the launcher or headset.

The new lighting builds on ideas from LanternEngine (GPL-3); our code, with thanks.

The release runtime uses ThinLTO with renderer PGO, retrained for this version on the game's own graphics and the new ones. Normal play keeps only startup and crash diagnostics.

Updating from 0.9.2 or older, the launcher's update button may call this version 0.9.160: that's 0.9.6. From 0.9.3 it shows 0.9.6.
