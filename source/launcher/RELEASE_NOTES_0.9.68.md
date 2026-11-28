# Bldbrne-VR 0.9.68

- **Tilting your head no longer splits the picture.** Until now the game's camera stayed level when you tilted your head, so near things (your hands, your weapon) sat at different heights in the two eyes. That could feel cross-eyed or make you queasy, most of all when looking to the side. The camera now rolls with your head. The HUD tilts with you. It switches on by itself once; to go back, set `camera_roll=0` in vr.ini after the first start.
- **Foveated upscaling covers the view more evenly up and down.** On headsets that see farther down than up (Quest 3), the sharp area sat against the top of the view and left the whole soft strip at the bottom. It is the same size, placed evenly now.

The release runtime uses ThinLTO with renderer PGO. Normal play keeps only startup and crash diagnostics.

Updating from 0.9.2 or older, the launcher's update button may call this version 0.9.168: that's 0.9.68. From 0.9.3 it shows 0.9.68.
