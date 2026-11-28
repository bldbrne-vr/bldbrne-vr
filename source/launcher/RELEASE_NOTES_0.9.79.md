# Bldbrne-VR 0.9.79

- **Antivirus false alarm fixed.** Windows Defender and some other antivirus programs flagged the small Bldbrne-VR.exe in the install folder as a virus. It isn't one: it's the standard starter from Velopack, the installer we use, which only opens the launcher. But it also carried a spare copy of the launcher's pictures that it never uses, and that looks suspicious to antivirus scanners. It doesn't carry them any more, and updating replaces it.
- **If your antivirus already removed it:** the Start menu shortcut still starts the game, and updating puts the starter back.
- Everything else is the same as 0.9.78: lock-on follow, hitting levers, honed steel, and the rest.

The release runtime uses ThinLTO with renderer PGO. Normal play keeps only startup and crash diagnostics.

Updating from 0.9.2 or older, the launcher's update button may call this version 0.9.179: that's 0.9.79. From 0.9.3 it shows 0.9.79.
