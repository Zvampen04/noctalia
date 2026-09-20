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

## Implementation coverage

- Implemented: generic visual Quick Settings layout editor with saved grid placement, width/height, add/remove, keyboard adjustments, Undo, Tidy, and automatic-layout reset. Circular action ordering and device capability checks remain service-backed. Oversized layouts scroll.
- Implemented: configurable panel-to-section routing for keyboard/IPC opens and launcher sizing from results, width, row limit and grid columns. Project 1 selects the clock section through recipe data.
- Project 1 recipe: volume and notification activity targets the middle clock section using the existing generic activity presenter.
- Not visually certified: opening/closing shape continuity and notification/volume handoff. The existing retained-source geometry is reused; no new claim of frame-by-frame parity.
- Implemented: right-click size presets and themed layout previews in the editor. Previews do not invoke device actions.
- Implemented: compact detail-page Back/Cancel navigation and month-to-strip navigation reuse the existing panel surface and source anchor.
- Implemented: configurable content-driven panel resizing; launcher results and calendar page transitions use it, including an immediate mode.
- Implemented: section activities expand/collapse independently of their saved compact anchors, retain metric rings, display optional body text, and allow shared/per-kind width, height, track thickness and timeout settings. Repeated level changes retain their current reveal progress.

Testing preference: focused logic/schema/build checks; visual inspection only when needed to resolve a concrete uncertainty. No automatic broad VM or frame-by-frame audit.

Reference archive: `/home/Zvampen04/Pictures/Noctalia rebuild/Noctalia Shell Layouts/Project 1/Video references/`. Visual editor: F043/F051/F052. Launcher: F025–F027. Transients: F032–F039.

## Generic configuration

The native settings window exposes **Quick Settings grid columns** and **Visual Quick Settings layout** under Control Center. Apply stores `control_center.compact_layout` and enables compact sections. Entries use `kind:x:y:width:height` in a 4–12 column, 12 row grid. Supported blocks are `wifi`, `bluetooth`, `volume`, `brightness`, `notifications`, `media`, `tray`, and `actions`. An empty list retains the automatic layout; `["empty"]` deliberately hides all blocks. Invalid, overlapping, or duplicate entries are ignored. Choose the column count before arranging blocks. Brightness, media and tray retain their existing capability/visibility settings. Actions use the existing configurable shortcut list.

Launcher settings now honor width, maximum height, visible result rows and app-grid columns. **Fit launcher to results** shrinks the panel as results narrow. **Panel source sections** under Panels accepts entries such as `launcher=my-center-section`; explicit module clicks retain their own source. Missing sections fall back to ordinary placement. No theme names are interpreted by either renderer.

Project 1's factory recipe enables result fitting and selects `launcher=project1-clock`. Saved user presets are preserved; reselect the factory recipe to adopt revised defaults, or change these settings individually.

## Full requirement checklist

This distinguishes existing implementation from additions in this release. Source presence is not a claim of frame-by-frame visual verification.

| Requirement | Coverage / owner |
| --- | --- |
| Media left, clock middle, updates right | Existing Project 1 recipe; no additional default bar modules added. |
| Independently selectable content and CPU/GPU/RAM/battery/update rings | Existing generic widget presentation; `docs/bar-module-content-and-rings.md`. |
| Symmetric utilization contour, thresholds and theme-linked colors | Existing shared ring renderer and widget settings; retained for expanded panels and now integrated activity. |
| Reorderable circular actions, wallpaper/themes/StoreIt/settings/power/task manager | Existing service-backed shortcut registry and compact action strip. |
| System tray plus newest/oldest selection | Existing shared tray model; compact tray and widget selection use the same items. |
| Update popup automatic/Quick Settings/StoreIt policy | Existing host system-updates plugin and cached status frontend. |
| Scrollable calendar, shared events, full month | Existing CalendarTab/service; this release adds return navigation without tearing down the shell surface. |
| Compact desktop controls, optional brightness and duplicate-media avoidance | Existing compact renderer and capability/home-visibility policies; new grid stores placement/size/visibility. |
| Quick Settings visual editor | New persistent backend grid and drag/resize/keyboard/right-click-size editor, Undo/Tidy/reset/add/remove. |
| Wi-Fi/Bluetooth/audio detail views and Focus | Existing network, Bluetooth, mixer and Do Not Disturb services; new compact Back navigation. |
| Hover hit-area stability and closing to source | Existing stable source bounds/input envelopes; navigation now preserves the same panel surface and source instead of restarting its entrance. |
| Launcher attached to middle capsule, fitting results | New generic source routing and honored size/grid/row settings; configurable resize animation. |
| Notification/volume/brightness integrated with notch or elsewhere | Existing shared routing and queue; new expanding section presentation/body/dimension controls. Attached and standalone choices remain. |
| Consistent utility-panel source anchors | Project 1 recipe assigns launcher, session and wallpaper to the clock section through generic routes. Theme picker remains the requested separate overlay. |
| Basic/Advanced, shared speed/curve, component overrides | Existing native settings and Infinite Desktop manifest/backend; new controls participate in theme capture. |
| Arbitrary bar sections, alignment, edge offset, four edges and background modes | Existing generic section engine; `docs/bar-sections.md`. |
| Per-section/per-widget presets and custom shader files | Existing native material targets, imported custom-effect resources and compositor material protocol; `docs/custom-element-effects.md`. |
| HyprWindowShade and terminal glass | Existing host plugin integration and shared material backend; preserved. |
| Rounded screen corners without black edge bands | Existing separate corner masks; full desktop frame remains disabled for Project 1. |
| Secondary display edge gaps and projected window corner handling | Existing Infinite Desktop per-output spacing and rendering; this release does not replace compositor behavior. |
| Theme switcher replaces Visual Themes in Settings and Quick Settings | Existing theme-picker routing repair, preserved. |
| Wallpaper picker / authentication / fullscreen / displays | Accepted exceptions: thumbnail grid, styled greetd/session lock, Super+F, Super+G. |

The ten notch-app reviews are feature/design references, not a requirement to port every feature of all ten products (including proprietary implementations). This release retains the shared native features exposed by the action catalog; optional future integrations must use that same catalog and backend ownership.

Minimal validation is intentional. Manual checks after activation should cover the new editor, compact back navigation, launcher result sizing, and volume/notification interruption and return. A quantitative frame-by-frame certification remains outside this minimal-testing pass.

`osd.activity` now accepts `width`, `height`, `progress_thickness`, `timeout_ms` and `show_body`; volume, brightness and notification overrides accept the same numeric keys (zero inherits) plus `body = "show" | "hide"` (empty inherits). Widths below 120 and heights below 24 are bounded at presentation. `motion = "off"` applies changes immediately. Notification hover pauses the existing service timeout; left-click activates, right-click dismisses; volume/brightness sliders and wheel changes use their existing device services. Section content and its configured metric ring return to the stable compact bounds when the activity ends.

`control_center.compact_navigation` enables Back and the configured Cancel key on detail pages. The full month returns to the strip; other detail pages return to Quick Settings. `shell.panel.resize_duration_ms` controls content-driven panel resizing across themes. Shared motion disable/speed/curve still applies.
