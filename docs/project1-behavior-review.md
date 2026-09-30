# Project 1 behavior review

Project 1 is a configuration recipe in the Infinite Desktop repository. Its
behavior uses the same shell controls, panel manager, materials, motion settings
and settings menu as other themes. No renderer branches on the recipe name.

The September 2026 audit traced the reference walkthrough, panel lifecycle,
bar attachment geometry, OSD and notification activities, and shared controls.
The resulting fixes address:

- Immediate destruction when another panel opens: replacement now waits for
  the visible close, and the latest requested panel wins.
- Interrupted exits: travel and duration start from the current frame. A
  zero-reveal panel does not delay a pending replacement.
- Hover handoff: opening uses the painted opener bounds, including its shadow;
  closing returns to the canonical compact bounds.
- Named bar sections: start, center and end anchors retain their actual role,
  so media and quick settings expand in the reference direction.
- Premature content clipping: controls reveal late in island expansion. A
  resized context page also reveals after its retained body begins resizing.
- Usage rings: the compact ring fades at its opener rather than stretching into
  a partial border around the expanded panel.
- Activity churn: compatible volume updates change the mounted activity in
  place instead of withdrawing and reconstructing it for every tick.
- Persistent panels and switcher cancellation: shared fades preserve current
  opacity when reversed. Closed compositor surfaces retire safely.
- Slider and card consistency: a configured zero thumb stays hidden; compact
  slider glyphs center vertically and retain contrast on both sides of the
  fill. Segmented controls retain all four paddings. Card opacity updates reach
  nested media and notification cards without rebuilding them.
- Compact month calendars have public width and height settings and a dense
  month layout. The recipe uses a single calendar card instead of squeezing an
  events column into the same space. Session action labels can wrap onto two
  centered lines rather than truncating the action name.
- Settings titles use the space left by their controls and wrap onto two lines,
  so long labels do not overlap override badges or reset actions.

## Verification

The focused native tests are `control_variants`, `bar_island_morph_geometry`
and `attached_panel_scene`. Existing package checks also cover slider and
material previews.

`tests/vm_project1_review.py` is an external test script for the consuming
NixOS `wm-session-hyprland-vm-driver`. Use matching pinned shell and Infinite
Desktop inputs. It applies the actual recipe through Hyprset, captures dark
and light feature states and close tails, checks latest-request replacement
and reopen behavior, and round-trips the maintained five-theme catalog.
It emits `project1-review.mp4`, PNG captures and `project1-review.json` into
the driver's output directory. Review the pixels and intermediate frames;
IPC assertions alone do not prove animation quality.

The offline VM has no Bluetooth radio or display-brightness hardware and
cannot certify online calendar or weather content. Those modules still have
their empty/unavailable states and panel geometry exercised.

Native application settings windows use the compositor's close animation.
Grabbed tray/context menus release their native grab immediately. Window
switcher selection likewise retires the overlay before activating the selected
client, as required by native compositor activation. These are intentional
lifecycle exceptions, not missing ordinary panel close animations.
