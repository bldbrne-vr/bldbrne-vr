# Controller artwork

Quest, Steam Frame and Index source SVGs are the controller diagrams used by
SteamVR's binding interface. The source files are retained alongside the
512-pixel-high PNGs embedded in the launcher.
Index line weights and Frame detail contrast are increased for legibility at
small display sizes.

- Quest — Valve / Meta, SteamVR Oculus driver.
- Steam Frame and Index — Valve, SteamVR controller drivers.
- PS VR2 — Sony Interactive Entertainment, *CFI-ZVR1 Instruction Manual*,
  page 16. Black vector outlines extracted from the top and side diagrams,
  with the manual's labels removed and outlines changed to white.

Original artwork remains copyright of its respective owners. The SteamVR
mirror is a source reference, not a grant of a new artwork license.

Sources retrieved 2026-11-05

- https://github.com/SteamDatabase/GameTracking-SteamVR/tree/master/drivers/oculus/resources/icons
- https://github.com/SteamDatabase/GameTracking-SteamVR/tree/master/drivers/frame_controller/resources/icons
- https://github.com/SteamDatabase/GameTracking-SteamVR/tree/master/drivers/indexcontroller/resources/icons
- https://www.playstation.com/content/dam/global_pdc/en/corporate/support/manuals/psvr2-docs/CFI-ZVR1_PS%20VR2_Instruction_Manual_Web%24en.pdf

The launcher decodes only the selected pair using Windows Imaging Component,
then uses its existing Direct3D renderer. No SVG parser, network request or
animation runs in the launcher.

The extracted PSVR2 filled outlines also have a 0.12-unit light stroke to keep
the original fine lines readable at the launcher's small display size.
