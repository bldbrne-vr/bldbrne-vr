# Bldbrne-VR 0.8.5

- **A simpler VR settings menu**: nine tabs (You, Controls, Combat, Items & world, HUD & feedback, Picture, Upscaling, Difficulty, Save). Settings that were two rows are one choice now: Low-health edges, Exhaustion color, Health bars (Off / Bosses only / All), Ragdoll, Two-handed controls and a single Upscaler row (Off / DLSS / FSR 4 / FSR 3.1.5 / DLSS 5). A tab with more settings than fit has a second page: pick it on the strip under the tab names (left / right or the triggers). Long help lines wrap instead of shrinking. Side lean, posture smoothing, clipping protection, pouch touch feedback and swing feel are set in the launcher.
- **Soft save** (experimental): on the menu's Save tab, save the moment you are in. After a death you choose: back to your soft save or to the last lantern. Using a lantern, choosing Last lantern or slaying a boss clears it. It is on for your own save when the launcher has backed it up at that launch (it does at every launch), and every reload first backs your save up again.
- **Monocular**: a true zoomed view through its eyepiece in each eye. The trigger is a coarse zoom ring (up to 6x) and turning your wrist zooms too (up to 5x), with a tick as the lens turns (clockwise zooms in). The zoomed view is stabilized, moves at real-world speed, stays at your eye if the controller loses tracking, and works with DIBR.
- **Free camera**: VR menu > You > Free camera. The view flies while the hunter stays put and enemies ignore him. Y brings you back to the hunter. The grips and the left stick click change the speed. It is also in the launcher.
- **Hand Lantern**: carried in your hand, it swings as you move and lights the world with the game's own light, a candle burning in its cage. Switch it on and off in your hand; stowed, it hangs from your belt.
- **New players start with an upscaler picked for their graphics card** (DLSS on RTX 30-series and newer with 10 GB or more, FSR 3.1.5 on stronger Radeons), for a clean image from the first launch. Players who already have Bldbrne-VR keep their own settings.
- **The resolution warning is hard to miss**: a framed banner on the Play page with a RUN PERF SCAN button that finds smooth settings for your PC.
- **Controls page in the launcher**: Quest, PSVR and Frame tabs list every control as the game's Show VR controls page has it, with your own settings, and below it how to play.
- **Sharper skies** (launcher Play page): the launcher makes AI-upscaled sky panoramas, moons and cloud rings from your own game files (bundled Real-ESRGAN). Remove them to go back to the game's own skies.
- **Summon bell**: ringing the bell with your hand no longer plays the game's ringing animation.
- **Levers**: the scripted lever in Mensis is pulled by hand too, and levers are easier to grab as your hand arrives.
- **Viscerals**: a 7 cm stab starts an open visceral (was 8 cm).
- **Lights**: the camera's fill lights follow lazily by default, and a light's glow sits on the light in both eyes and no longer on your hands.
- **Performance**: NVIDIA cards are asked for maximum performance for the game. Waiting threads stop spinning sooner (a big gain on 4-core CPUs). Cloth stiffness follows the frame's time, and a render-job slot leak is fixed.
- **Fixes**: right-hand projectile aiming was reversed; blade contact sounds used the wrong sounds; opening a chest in Mensis could open the lamp page; the launcher could not turn DLSS 5 off in DIBR; the Steam Frame's menu help named X and Y instead of its d-pad; grip adjust's help now describes the real gesture (hold A+B 1.2 s standing still). The boss health bar sits 2 px lower.

The release runtime uses ThinLTO without PGO. Normal play keeps only startup and crash diagnostics.
