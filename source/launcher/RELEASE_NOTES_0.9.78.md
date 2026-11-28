# Bldbrne-VR 0.9.78

- **Lock-on follow (new, off by default).** Turn it on in Combat > Two hands & guns, in the VR menu or the launcher. When you lock on, your view smoothly keeps the target in front of you, and the left stick circles it like flat lock-on. Look away from the target to let go.
- **Hit a lever to work it.** In the Chalice Dungeons, smack a lever with your weapon or shoot it and it moves, whenever you could pull it by hand. An enemy in the way takes the shot instead. Tall and turning levers still use B.
- **Honed steel that really shines.** Rub the Blades of Mercy or the Rakuyo together and the blades' own steel takes the polish: a flash of light runs from guard to tip, and the steel stays bright until the hit that spends it. A faint grip mark on the joined hilt shows where to grab to split them.
- **Foveation changes on the fly.** Change foveation or the upscaler's focus in the VR menu and it applies at once, no restart.
- **No more black corners.** The black stair-step wedges at the far corners of your view with foveation On or Performance are gone, at no cost.
- **Status warnings show sooner.** Poison, frenzy and the rest show at the edges of your view earlier and darken faster as they build.
- **Quicker ladders.** The fast climb kicks in a little sooner again.
- **Chest items sit a little higher.** Whatever you keep on your chest now shows 2 cm higher on the coat. You grab it in the same spot as before.
- **And fixes:** the cutscene frame cap after Rom now holds all the way to Yahar'gul, resizing the desktop window is safer, a draw missing its vertex data can no longer crash the game, graphics memory from areas you've left is freed before the card fills up, and finished game threads no longer leak memory.

The release runtime uses ThinLTO with renderer PGO. Normal play keeps only startup and crash diagnostics.

Updating from 0.9.2 or older, the launcher's update button may call this version 0.9.178: that's 0.9.78. From 0.9.3 it shows 0.9.78.
