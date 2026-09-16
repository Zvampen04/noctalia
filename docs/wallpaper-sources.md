# Wallpaper sources

The native wallpaper picker offers a source selector when `noctalia/wallhaven`
is enabled. **Wallpaper** shows the existing local library; **Wallhaven** opens
the plugin browser. The browser has the same selector to return to the library.
The wallpaper panel IPC command continues to open the local library.

`src/shell/wallpaper/panel/wallpaper_source_switch.h` owns the shared selector.
Panel replacement is deferred until the input callback returns and retains the
current output and source bar. Other plugin panels do not receive this control.

Leave the plugin's `download_dir` empty to use Noctalia's configured wallpaper
directory. The plugin downloads `wallhaven-<id>.<extension>` there before applying
it. The native scanner checks the directory modification time when reopening,
so downloaded images appear in the local library without restarting the shell.

Store optional API keys in a private user configuration file, never in the Nix
store or repository. Authentication does not change the selected purity filters.
