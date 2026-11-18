# Bldbrne-VR 0.7.0

- **Smooth busy areas: Frame overlap** (VR settings > Graphics, on by default). The game now prepares its next frame while the renderer finishes the current one, instead of waiting for it. The constant jitter when looking around in busy places such as Old Yharnam and the Sewers is gone. Turning it off applies at the next start.
- **Fewer shadows when busy** (VR settings > Graphics, off by default): in very busy views it also skips the next-farthest shadows, and brings them back once the view is calmer. Far shadows stays on Reduced by default.
- **Solid weapons hit walls**: your weapons now stop at walls and other level geometry, not only at each other (VR settings > Weapons > Solid weapons: On). "Weapons only" brings back the old behaviour.
- **Quick items stay put**: the item cards and the item name no longer turn and roll with your head as you look around; they stay upright where the menu opened.
- Upscaling (DLSS / FSR): scene draws whose size the GPU decides are now jittered like the others, so none are left out of the reconstruction.

The release runtime uses ThinLTO without PGO. Normal play keeps only startup and crash diagnostics.
