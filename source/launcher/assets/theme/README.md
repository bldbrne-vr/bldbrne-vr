# Hunter launcher theme

Original artwork generated with the built-in image generation tool for this launcher;
not extracted from Bloodborne or another game's website. The approved look is a dark,
tactile Victorian gothic concept, with restrained ornament and crimson controls.

Runtime assets (embedded in the executable, no network access):

* `yharnam.jpg`: 1024 x 342 static panoramic illustration, JPEG quality 85.
* `leather.png`: 256 x 256 reusable subtle leather texture.
* `frame.png`: 512 x 512 transparent gunmetal frame, drawn in eight perimeter slices.
* `button-frame.png`: 512 x 81 transparent notched plaque border, independently sliced
  to preserve thin rails and small corners on buttons of different proportions.
* `launch-plaque.png`: 1024 x 259 crimson leather plaque with embossed vines and
  sculpted metal end caps. The caps retain their proportions as the center widens.
* `cabinet-frame.png`: 512 x 384 transparent outer cabinet frame. The central
  divider samples its straight iron rail so the whole launcher shares one frame.
* `hardware.png`: 512 x 192 transparent atlas with the unlettered riveted
  utility button, the crest, and the bottom clasp, generated from the mock.
* `scrollbar-eye.png`: 40 x 408 transparent scrollbar thumb with tarnished brass,
  scratched gunmetal and an engraved eye. The caps and grip retain their proportions.

Source prompts, built-in image generation (2026-11-05):

* Banner: production background illustration, wide 3:1, no text/UI/border. Dark
  Victorian gothic city under moonlight, blue-black and charcoal, restrained warm
  gas lamps and fog. Quiet left half for the UI headline; a small hunter silhouette
  at a railing; cathedral spires and a clouded pale moon on the right.
* Leather: front-on scan of charcoal black antique bookbinding leather, fine
  irregular pebbled grain, subdued pewter highlights, low contrast, seamless square,
  uniform lighting; no objects, seams, letters, border, perspective or vignette.
* Frame: front-on square Victorian gothic worn gunmetal frame, transparent center
  and outside, narrow straight rails, modest engraved corner fittings and rivets,
  restrained pewter edge catches, suitable for nine-slice scaling. No gold, text,
  objects, leather inside or large scroll ornaments.

* Button border: thin layered aged gunmetal and restrained brass edges, notched
  corners, transparent center and outside, no text or symbols; cropped and reduced
  to a compact perimeter texture.

* Launch plaque: layered aged gunmetal and embossed oxblood leather with a blank
  center for live text. Cabinet frame: dark iron rails with rounded, engraved
  corner plates based on the launcher mock,
  restrained pewter highlights and rivets. Both are front-on with transparent exteriors.

Scrollbar prompt, built-in image generation (2026-11-06):

* Use case: stylized-concept. Asset type: a single vertical scrollbar thumb sprite for
  a Bloodborne-inspired desktop launcher. One straight-on orthographic Victorian
  artifact, an extremely long narrow chamfered metal bar with parallel sides,
  approximately 1:6 width to height, centered upright on fully transparent background.
  The border is worn antique brass with thin oxidized teal filigree, the interior
  battered gunmetal with tactile deep blue-black, warm pewter scratches and cool pale
  metal specks. At the exact midpoint, one small but stark ice-grey eldritch eye
  etched into the metal, almond shape almost spanning the bar's interior width, black
  vertical pupil with a light ring, bordered above and below by short transverse grip
  grooves. Long stretches of dark textured metal above and below the centered eye with
  very little ornament, small squared beveled end caps. A hard crisp graphic
  silhouette and strong contrast to read when scaled to 12 pixels across. Clean alpha
  outside the piece. Flat even front lighting, no perspective, no cast shadows, no
  text, no arrows, no scene, no other objects. This is a game UI sprite, not a mockup.

The eight immutable RGBA8 textures occupy 5,183,232 bytes (about 4.94 MiB) before
driver overhead. They load once per launcher instance. No extra render targets,
animation timers, custom shader passes or continuous repainting were added.

Actual controls retain ImGui navigation, disabled handling and activation behavior.
Preview-only options: `BBVR_PREVIEW_THEME=1` omits the canned update/new-player
notices, `BBVR_PREVIEW_POINTER=hover|pressed|keyboard` exercises native states,
`BBVR_PREVIEW_SMALL=1` checks a small client area. These require `--preview` and use
its isolated profile. `--idle-smoke` preserves the existing idle measurement path.
