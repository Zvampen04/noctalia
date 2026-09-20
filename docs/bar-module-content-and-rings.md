# Module content, rings and actions

Every bar widget, including plugin widgets, has independent content and a metric ring. Choose the widget type for its content in the bar editor; enable **Ring** and choose **Ring source** in that widget's presentation settings. Ring sources are battery, RAM, CPU, GPU and current system update item progress. GPU usage uses the shared monitor and is requested only while a GPU ring exists. Unsupported metrics show an empty ring. Update progress uses the host's published per-item progress, not an estimated total.

Rings follow the content's outline: circular for square content, capsule-shaped for wide content such as a clock. Content retains its click targets and tooltip. The ring adds no independent pointer target. These settings apply to all themes, bar positions and named sections.

```toml
[widget.music]
type = "media"
ring = true
ring_source = "cpu"

[widget.time]
type = "clock"
ring = true
ring_source = "gpu"

[widget.status]
type = "control-center"
icon_source = "system_updates"
ring = true
ring_source = "ram"

[widget.latest-app]
type = "tray"
selection = "newest" # all, newest, oldest
ring = true
ring_source = "cpu"
```

Tray ordering means registration order within the current shell session. Restarted apps register again. Existing apps discovered at shell startup are ordered by discovery; this does not infer their process start time. Hidden/passive filters apply before selecting the newest or oldest item. Hover shows the app's tooltip, left-click activates the app and right-click opens its tray menu. Apps must publish a StatusNotifier tray item.

`control_center.show_tray` enables the shared tray in either normal or compact Quick Settings. It includes passive background apps and scrolls horizontally when space is limited. It uses the same icons and menus as the bar tray.

The Quick Settings shortcut picker includes power, settings, wallpaper, themes, StoreIt, system updates, task manager, both calendar views, notifications, clipboard and existing service toggles. Shortcuts can be reordered. In compact mode they appear as circular buttons at the bottom; hover shows the action name. Click performs the named action. Media's existing shortcut toggles playback and right-click opens the player; other native shortcut behavior is unchanged.

The shared bar action picker offers ready-made media, rolling calendar, month calendar, Quick Settings, power, wallpaper, themes, settings, StoreIt, updates, task manager, notifications, clipboard, audio, weather, tray and launcher actions, plus the existing registered shell commands and custom commands. Configure each gesture independently. Rolling calendar explicitly opens the week strip; month calendar explicitly opens the full calendar. Both use the same calendar service and events. Themes opens Appearance settings, including the integrated preset controls. System updates respects the configured popup policy.

Existing module gesture defaults and per-module hover behavior are documented under `docs/user/bar/actions.mdx` and `docs/user/bar/widgets/`. A ring does not change those actions. Section hover/click settings remain independent of widget bindings.
