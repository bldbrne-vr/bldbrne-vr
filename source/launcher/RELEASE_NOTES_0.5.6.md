# Bldbrne-VR 0.5.6

- **Far shadows** (VR settings > Graphics): Reduced, the new default, skips the farthest shadows. That means about 100 fewer draws a frame in big open areas such as the Sewers' halls, for more FPS where the game is busiest. Full draws them all, as before; the change applies at once.
- Sharper shadows with foveated rendering: the shadow map is no longer drawn at lower detail along with the edges of the view.

The release runtime uses ThinLTO without PGO. Normal play keeps only startup and crash diagnostics.
