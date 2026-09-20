# Project 1 implementation status

Updated 2026-09-20. Source audit, not a visual certification.

## Accepted existing behavior — do not replace

- Keep Noctalia’s wallpaper thumbnail grid; do not implement the video’s carousel.
- Keep our styled lock screen/greetd authentication presentation.
- Super+F already hides the bar when a window becomes fullscreen; no video Game Mode transformation is required.
- Super+G owns display configuration; do not duplicate it in Quick Settings.
- Keep the requested Super+E image-card theme overlay and Quick Settings link to it.
- Keep media/CPU left, clock/GPU middle, updates/RAM right and configurable theme-linked utilization rings.

## Existing foundations

Media and calendar panels, shared event backend, connectivity, audio mixer, notification history, power actions, utility shortcuts and system tray exist. Generic section/attached routing exists for OSD and notifications. These need integration/polish where they differ from the approved reference; they do not need replacement backends.

## Implementation queue

- Implemented: generic visual Quick Settings layout editor with saved grid placement, width/height, add/remove, keyboard adjustments, Undo, Tidy, and automatic-layout reset. Circular action ordering and device capability checks remain service-backed. Oversized layouts scroll.
- Implemented: configurable panel-to-section routing for keyboard/IPC opens and launcher sizing from results, width, row limit and grid columns. Project 1 selects the clock section through recipe data.
- Project 1 recipe: volume and notification activity targets the middle clock section using the existing generic activity presenter.
- Not visually certified: opening/closing shape continuity and notification/volume handoff. The existing retained-source geometry is reused; no new claim of frame-by-frame parity.
- Remaining editor refinement: right-click size presets and miniature live service previews; this version provides labeled blocks and pointer/keyboard sizing.

Testing preference: focused logic/schema/build checks; visual inspection only when needed to resolve a concrete uncertainty. No automatic broad VM or frame-by-frame audit.

Reference archive: `/home/Zvampen04/Pictures/Noctalia rebuild/Noctalia Shell Layouts/Project 1/Video references/`. Visual editor: F043/F051/F052. Launcher: F025–F027. Transients: F032–F039.

## Generic configuration

The native settings window exposes **Quick Settings grid columns** and **Visual Quick Settings layout** under Control Center. Apply stores `control_center.compact_layout` and enables compact sections. Entries use `kind:x:y:width:height` in a 4–12 column, 12 row grid. Supported blocks are `wifi`, `bluetooth`, `volume`, `brightness`, `notifications`, `media`, `tray`, and `actions`. An empty list retains the automatic layout; `["empty"]` deliberately hides all blocks. Invalid, overlapping, or duplicate entries are ignored. Choose the column count before arranging blocks. Brightness, media and tray retain their existing capability/visibility settings. Actions use the existing configurable shortcut list.

Launcher settings now honor width, maximum height, visible result rows and app-grid columns. **Fit launcher to results** shrinks the panel as results narrow. **Panel source sections** under Panels accepts entries such as `launcher=my-center-section`; explicit module clicks retain their own source. Missing sections fall back to ordinary placement. No theme names are interpreted by either renderer.

Project 1's factory recipe enables result fitting and selects `launcher=project1-clock`. Saved user presets are preserved; reselect the factory recipe to adopt revised defaults, or change these settings individually.
