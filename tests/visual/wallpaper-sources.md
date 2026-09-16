# Wallpaper source navigation

Owning sources:
- `src/shell/wallpaper/panel/wallpaper_source_switch.h`
- `src/shell/wallpaper/panel/wallpaper_panel.cpp`
- `src/shell/panel/plugin_panel.cpp`

Required release checks:
- Local library: local source selected; existing filter, monitor, sort, favorites
  and theme controls fit below the source selector.
- Wallhaven: online source selected; search, filters, thumbnails and pagination
  remain visible below the selector.
- Switch both ways with pointer and keyboard; retain the output and source bar.
- Reopen the local library after a download; find the new image without a shell
  restart and without changing the wallpaper directory.
- Disable Wallhaven and reopen the native picker; no unavailable source appears.
- Inspect narrow/fractional-scale output layouts and both palette modes.

2026-09-16 focused inspection: a release build based on the installed shell
source was loaded into the active session. Directly reviewed captures of both
views on DP-1 at scale 1 with the current dark theme. Selected segments, local
picker controls, online search filters, thumbnails and pagination were visible
and legible. No separate Wallhaven widget remained in the current bar.
Authenticated search succeeded and a downloaded image was present in the shared
wallpaper directory. Wider release matrix and automated pointer/keyboard routing
remain pending; this inspection does not approve untested contexts.
