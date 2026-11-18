# Bldbrne-VR 0.9.4

- **Play with a gamepad.** A new Play style on the launcher's Play page: Motion controls, Gamepad first person or Gamepad third person. The gamepad styles switch off everything that needs your hands, and your own motion settings come back the moment you switch back. Third-person camera work by ID-Gamer.
- **The gamepad, all the way.** Options opens the VR menu (hold it to recenter) and the menu works with the pad. Xbox pads get Xbox button icons, pull the right stick back to turn around in first person, and hold L3 with the right stick to zoom the third-person camera. Show VR controls has a gamepad page.
- **The Beast Cutter swings like a heavy chain.** In whip form the chain flies along your swing and its reach follows your speed: big wide swings throw it all the way out, a lazy one keeps it short. Stop and it reels back in, slowly at first; swing again and it stays out. The trigger or a flick back pulls it in. It no longer folds into your hand on fast back-and-forth swings, and walls no longer bounce you.
- **One sweep, many hits.** A Beast Cutter whip swing stays one attack until the chain lands, and it can catch several enemies in its path.
- **Hear the full charge.** A fully charged attack now rings with the game's own charge sound, with the three pulses, only on weapons that have a charged attack.
- **Leaning out of the way, a little easier.** The lean dodge triggers a touch sooner and keeps you invincible a little longer. Longer dodge invincibility is now off by default; turn it back on under Difficulty if you want it. A value you picked yourself stays.
- **Knock on doors in VR.** With VR interactions on, knocking on a resident's door no longer plays the hunter's animation on a flat screen: you stay in your eyes, and they answer.
- **More frames.** The two eyes share more of the GPU's work: about 3% faster in native stereo on our test PC. The first start after updating rebuilds the VR shaders once.
- **No freeze on arrival.** The 1.5-2 second hitch right after you arrive in the world is gone.
- **Fewer stutters in The Old Hunters.** If you own the add-on, its areas' shaders are now built at startup too.
- **Fixes**: the Amygdalan Arm only hangs after a deliberate full lash, and not every time; the VR Settings scrollbar drags while the page is locked; No Grip item access works on every headset; the long-session slowdown fix waits on the GPU even less.

The release runtime uses ThinLTO with renderer PGO, retrained for this version. Normal play keeps only startup and crash diagnostics.

Updating from 0.9.2 or older, the launcher's update button may call this version 0.9.140: that's 0.9.4. From 0.9.3 it shows 0.9.4.
