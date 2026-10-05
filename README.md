# Noctalia

Noctalia is a native Wayland desktop shell for people who want a polished, configurable Linux desktop without stitching
together a separate bar, launcher, notification daemon, lock screen, wallpaper tool, and settings UI.

It provides the shell layer around your compositor: bars, widgets, dock, launcher, control center, notifications,
wallpaper, lock screen, session actions, clipboard history, OSDs, tray integration, and desktop widgets. The project is
built directly on Wayland and OpenGL ES with no Qt or GTK dependency, so the UI, rendering, configuration, and IPC model
are designed as one cohesive shell instead of a collection of unrelated panels and scripts.

<p><br/></p>

<p align="center">
  <img src="https://assets.noctalia.dev/noctalia-logo.svg?v=2" alt="Noctalia Logo" style="width: 192px" />
</p>

<p align="center">
  <a href="https://docs.noctalia.dev/noctalia/getting-started/installation/">
    <img
      src="https://img.shields.io/badge/Install_Noctalia-FFF59B?style=for-the-badge&labelColor=FFF59B"
      alt="Install Noctalia"
      style="height: 50px"
    />
  </a>
</p>

<p><br/></p>

<p align="center">
  <a href="https://github.com/noctalia-dev/noctalia/commits">
    <img src="https://img.shields.io/github/last-commit/noctalia-dev/noctalia?style=for-the-badge&labelColor=FFF59B&color=FFF59B&logo=git&logoColor=070722&label=commit" alt="Last commit" />
  </a>
  <a href="https://github.com/noctalia-dev/noctalia/stargazers">
    <img src="https://img.shields.io/github/stars/noctalia-dev/noctalia?style=for-the-badge&labelColor=FFF59B&color=FFF59B&logo=github&logoColor=070722" alt="GitHub stars" />
  </a>
  <a href="https://docs.noctalia.dev">
    <img src="https://img.shields.io/badge/docs-FFF59B?style=for-the-badge&logo=gitbook&logoColor=070722&labelColor=FFF59B" alt="Documentation" />
  </a>
  <a href="https://discord.noctalia.dev">
    <img src="https://img.shields.io/badge/discord-FFF59B?style=for-the-badge&labelColor=FFF59B&logo=discord&logoColor=070722" alt="Discord" />
  </a>
</p>

## Why Noctalia?

Most Wayland setups leave the desktop shell to a stack of small tools: one bar, another launcher, another notification
daemon, a lock screen, a wallpaper daemon, scripts for session actions, and separate config formats for each piece. That
can be flexible, but it also makes a complete desktop feel fragile and hard to keep visually consistent.

Noctalia solves that by providing one configurable shell layer that owns the common desktop surfaces and services while
still fitting into compositor-driven Wayland workflows. It is meant for users who want the control of a custom desktop
environment with fewer moving parts and a consistent UI.

To understand the values and philosophy guiding the project, read our [ethos](https://noctalia.dev/ethos).

## What It Includes

- Multi-monitor bars with configurable widgets, taskbar, workspaces, system tray, media, network, battery, brightness,
  weather, clipboard, and custom script-backed widgets.
- Dock, launcher, control center, notification toasts/history, wallpaper picker, OSD overlays, lock screen, session
  panel, and desktop widgets.
- TOML configuration with hot reload, GUI-managed overrides, theme/palette support, template application, and IPC for
  runtime control.
- Direct Wayland integration for layer-shell, session lock, idle behavior, clipboard, foreign toplevels, workspaces,
  fractional scaling, and compositor-specific workspace backends where needed.

## Wayland Compositor Support

Noctalia supports Wayland compositors that provide the layer-shell protocols it needs for shell surfaces. Workspace
integration works through compositor-native backends where needed, or through `ext-workspace-v1` on compositors that
implement it.

Current compositor integrations include Niri, Hyprland, Sway, Scroll, Mango, Labwc, Triad, dwl, and other compatible
Wayland compositors. Other compositors may run Noctalia but can have reduced workspace, window, output, or
session-action integration depending on the protocols and IPC they expose.

## Scope

Noctalia is a desktop shell, not a full desktop environment. It provides the visual and service layer around your
Wayland compositor: bars, panels, launcher, notifications, dock, lock screen, idle behavior, OSDs, theming, wallpapers,
desktop widgets, and multi-monitor shell surfaces.

Window management, tiling, file management, removable-drive mounting, printers management and screen mirroring/casting
belong to the compositor, dedicated desktop applications, or system services.

Display/login greeter support lives in the separate [Noctalia Greeter](https://github.com/noctalia-dev/noctalia-greeter)
project. Noctalia may integrate with those pieces when useful, but it does not replace them.

The plugin system is available for user-installed extensions. Features that are useful to some users but not essential
to the core shell can live there: extra bar widgets, launcher providers, desktop widgets, panels, shortcuts, background
services, compositor-specific extras, hardware-specific controls, and third-party service integrations.

Application catalog updates and launcher icon metadata run on owned background workers. Opening the launcher uses the
last published catalog immediately, with text and fallback glyphs while icon lookup completes. Visible tiles and
overscan request icons; the icon worker retains at most 128 current-generation pending requests (including completed
results awaiting adoption), plus at most one obsolete executing lookup, and 512 cached positive or negative results.
Theme and catalog updates invalidate this cache. Closing the launcher cancels pending adoption, and callbacks cannot
access a destroyed view. Catalog refresh requests coalesce, and
Nix profile changes, desktop-file filtering and language changes remain part of background catalog refresh.
Missing absolute icon paths can be retried on a later visible-tile bind after one second, including when a generic
application icon was used as their fallback. Failed metadata lookups also retry on a later bind after one second;
ordinary missing named icons stay cached until theme or catalog invalidation.

Launcher usage history also loads and saves on one owned worker. Getters read cached state; early activations merge
aggregated count changes and at most 20 recent identifiers per provider with the original history before saving.
Clearing history suppresses late initial data. Saves retain one active and one latest pending snapshot; shutdown
attempts to flush the latest accepted state before joining. Existing unreadable history is retained rather than
replaced with an empty baseline. Each JSON file is replaced atomically with private permissions; the two files are
not a joint transaction, and failed IO can still prevent persistence.

Periodic icon-theme metadata checks also run off the main event loop. Shared theme state is published under a short
lock after filesystem work completes. Each check owns a separate GSettings instance and private event context;
cleanup dispatches outstanding settings callbacks until the instance is finalized before releasing that context.
Workers are joined during shutdown; an already executing kernel filesystem operation can delay that join. These
changes keep catalog refresh and launcher icon lookup off the main event loop, but do not guarantee responsiveness
during every kernel, compositor or storage failure.

## Build from source

Source dependencies, distro-specific package commands, build modes, and install layouts live in
[BUILDING.md](BUILDING.md).

## Configuration

A ready-to-use starting config with all defaults is at [example.toml](example.toml). The full configuration reference
lives in the [documentation site](https://docs.noctalia.dev/noctalia/). The source MDX files are in
[`docs/user/`](docs/user/); sync them to a local docs checkout with `tools/sync-docs.sh`.

## Contributing

Developer notes, architecture overview, code style, project layout, and debugging commands live in
[CONTRIBUTING.md](CONTRIBUTING.md).

Bug reports, fixes, documentation updates, themes, and configuration examples are welcome. For general help and design
discussion, join the community on [Discord](https://discord.noctalia.dev).

## Credits

Thank you to the [contributors](https://github.com/noctalia-dev/noctalia/graphs/contributors) and community
members who test Noctalia, report issues, share configurations, and help shape the project.

## Donations

Donations are appreciated but completely optional.

<p>
  <a href="https://www.buymeacoffee.com/noctalia">
    <img src="https://img.shields.io/badge/Buy_Me_a_Coffee-FFF59B?style=for-the-badge&logo=buymeacoffee&logoColor=070722&labelColor=FFF59B" alt="Buy Me a Coffee">
  </a>
  <a href="https://ko-fi.com/noctaliadev">
    <img src="https://img.shields.io/badge/Ko--fi-FFF59B?style=for-the-badge&logo=kofi&logoColor=070722&labelColor=FFF59B" alt="Ko-fi">
  </a>
</p>

## License

MIT License. See [LICENSE](LICENSE) for details.

## Packaging

Distro packaging notes (description, deps, install layout, Meson options) live in
[PACKAGING.md](PACKAGING.md).

## Star History

<p align="center">
  <a href="https://github.com/noctalia-dev/noctalia/stargazers">
    <img src="https://api.noctalia.dev/stars" alt="Star History" />
  </a>
</p>
