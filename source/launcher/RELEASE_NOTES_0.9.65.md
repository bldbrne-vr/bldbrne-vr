# Bldbrne-VR 0.9.65

- **Laurence no longer crashes the game.** Walking into his fight could close the game or leave the room gray with only sparks showing. The fight starts and plays.
- **No more stray glow orbs.** Cold blue balls of light hanging in the air with no lamp behind them are gone. Candles, lanterns and fires keep their glow.
- **Hunter tools no longer refuse with "no bullets" when you have them.** This happened with the game's own HUD (not the wrist gauge) and the health or stamina colours on.
- **Blood bullets by syringe** never take your health and then show a refusal.
- **Smoother start.** The shaders of the items in your hands are built before the first frame of the world, not during it.
- Smaller: a little less texture copying, one less wait for the graphics card in the shadow pass.

The release runtime uses ThinLTO with renderer PGO, retrained for this version. Normal play keeps only startup and crash diagnostics.

Updating from 0.9.2 or older, the launcher's update button may call this version 0.9.165: that's 0.9.65. From 0.9.3 it shows 0.9.65.
