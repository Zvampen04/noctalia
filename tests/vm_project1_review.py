"""Project 1 integration review, executed by the NixOS WM test driver.

Run against the normal Hyprland VM with matching shell and Infinite Desktop
inputs.
Captures are review evidence; successful IPC alone is not visual certification.
"""
import json
import os
import shlex
import time

if "machine" not in globals():
    raise SystemExit("Use the disposable NixOS WM test driver")

machine.start()
machine.wait_for_unit("home-manager-Zvampen04.service", timeout=180)
machine.wait_for_unit("greetd.service", timeout=180)
machine.succeed("test $(cat /etc/wm-audit-session) = hyprland")
uid = machine.succeed("id -u Zvampen04").strip()
machine.wait_until_succeeds(f"pgrep -u {uid} -f noctalia", timeout=180)
signature = machine.succeed(
    f"find /run/user/{uid}/hypr -mindepth 1 -maxdepth 1 -type d | head -1 | xargs basename"
).strip()
display = machine.succeed(
    f'for name in /run/user/{uid}/wayland-*; do test -S "$name" && basename "$name" && break; done'
).strip()
env = {
    "XDG_RUNTIME_DIR": f"/run/user/{uid}", "WAYLAND_DISPLAY": display,
    "HYPRLAND_INSTANCE_SIGNATURE": signature,
    "DBUS_SESSION_BUS_ADDRESS": f"unix:path=/run/user/{uid}/bus",
    "XDG_DATA_DIRS": "/etc/profiles/per-user/Zvampen04/share:/run/current-system/sw/share",
    "XCURSOR_PATH": "/home/Zvampen04/.icons:/home/Zvampen04/.local/share/icons:/etc/profiles/per-user/Zvampen04/share/icons",
    "PATH": "/etc/profiles/per-user/Zvampen04/bin:/run/current-system/sw/bin",
}

def user(*args):
    return machine.succeed(shlex.join([
        "runuser", "-u", "Zvampen04", "--", "env",
        *(f"{key}={value}" for key, value in env.items()), *args,
    ]) + " 2>&1", timeout=120).strip()

def shell(*args):
    return user("noctalia", "msg", *args)

def backend(command, payload=None):
    args = ["hyprset-backend", command]
    if payload is not None:
        args.append(json.dumps(payload))
    return json.loads(user(*args))

def wait_panel(panel=None, context=None):
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        status = json.loads(shell("status"))
        if status["activePanelId"] == panel and (context is None or status["activePanelContext"] == context):
            return
        time.sleep(.05)
    raise AssertionError(status)

captures = []
def capture(name):
    user("grim", "-o", "Virtual-1", f"/tmp/{name}.png")
    machine.copy_from_machine(f"/tmp/{name}.png")
    captures.append(name)

def close():
    shell("panel-close")
    wait_panel()
    time.sleep(.6)

# These background fixtures distract from shell pixels. Stop them only in this
# disposable guest; they remain part of the normal production configuration.
user("systemctl", "--user", "stop", "hyprland-canvas-app-codex.service",
     "codex-wallpaper-terminal-theme.path")
user("hyprctl-lua", "keyword", "monitor", "Virtual-1,1600x1000@60,0x0,1")
user("hyprctl-lua", "keyword", "monitor", "DVI-I-1,1024x768@60,1600x0,1")
user("hyprctl-lua", "dispatch", "focusmonitor", "Virtual-1")
user("hyprctl-lua", "dispatch", "movecursor", "800", "500")

gallery = backend("theme-gallery")
assert [entry["name"] for entry in gallery["entries"]] == [
    "Neumorphism", "Project 1", "Coder", "Canvas", "Sonder"]
assert not gallery["dirty"], "VM baseline has uncommitted appearance edits"
backend("theme-preview", {"token": gallery["token"], "selected": "Project 1"})
backend("theme-finish", {"token": gallery["token"], "action": "commit"})
time.sleep(2)

# Record the ordinary feature pass and close tails, plus a bounded interrupted
# replacement check. Keep real elapsed times; do not alter animation speed.
user("sh", "-c", "wf-recorder -o Virtual-1 -r 60 -f /tmp/project1-review.mp4 > /tmp/project1-recorder.log 2>&1 & echo $! > /tmp/project1-recorder.pid")
try:
    for mode in ("dark", "light"):
        shell("theme-mode-set", mode)
        time.sleep(1)
        capture(f"project1-{mode}-desktop")
        cases = [
            ("launcher", ""), ("control-center", "home"),
            ("control-center", "network"), ("control-center", "bluetooth"),
            ("control-center", "audio"), ("control-center", "monitor"),
            ("control-center", "system"), ("control-center", "notifications"),
            ("control-center", "calendar-strip"), ("control-center", "calendar-month"),
            ("control-center", "media"), ("control-center", "weather"),
            ("clipboard", ""), ("wallpaper", ""), ("session", ""),
        ]
        for panel, context in cases:
            shell("panel-open", panel, *([context] if context else []))
            wait_panel(panel, context)
            time.sleep(1.2)
            capture(f"project1-{mode}-{context or panel}")
            close()
        for value in (0, 5, 50, 100):
            shell("volume-osd", str(value))
            time.sleep(.5)
            capture(f"project1-{mode}-volume-{value}")
        time.sleep(3)
        shell("notification-show", "Project 1 review", "Long notification text exercises the clock activity without overlapping the compact clock.")
        time.sleep(.6)
        capture(f"project1-{mode}-notification")
        time.sleep(6)
        shell("settings-open", "appearance")
        time.sleep(1)
        capture(f"project1-{mode}-settings")
        shell("settings-close")
        time.sleep(.6)

    shell("panel-open", "launcher")
    shell("panel-open", "control-center", "home")
    shell("panel-open", "control-center", "calendar-strip")
    wait_panel("control-center", "calendar-strip")
    time.sleep(1)
    capture("project1-latest-panel-request")
    close()
    shell("panel-open", "launcher")
    shell("panel-close")
    shell("panel-open", "launcher")
    wait_panel("launcher")
    time.sleep(.7)
    close()

    shell("panel-open", "launcher")
    wait_panel("launcher")
    time.sleep(.6)
    machine.send_key("esc")
    wait_panel()
    time.sleep(.6)

    # Color-mode changes are real edits. Discard these deliberate test edits
    # before checking that theme-only switches never trigger dirty state.
    gallery = backend("theme-gallery")
    if gallery["dirty"]:
        backend("theme-select", {"token": gallery["token"], "selected": "Project 1", "action": "discard"})
    else:
        backend("theme-finish", {"token": gallery["token"], "action": "cancel"})

    shell("panel-open", "infinite-desktop/settings:theme-picker")
    time.sleep(1)
    capture("project1-theme-picker")
    shell("panel-close")
    time.sleep(1)
    # Test theme-only transactions and usable-area round trips using the real
    # backend; every commit must leave the gallery clean.
    for name in ("Canvas", "Neumorphism", "Coder", "Sonder", "Project 1"):
        gallery = backend("theme-gallery")
        assert not gallery["dirty"], (name, gallery)
        backend("theme-preview", {"token": gallery["token"], "selected": name})
        backend("theme-finish", {"token": gallery["token"], "action": "commit"})
        time.sleep(.5)
        capture("theme-roundtrip-" + name.replace(" ", "-"))
    gallery = backend("theme-gallery")
    assert not gallery["dirty"] and gallery["selected"] == "Project 1"
    backend("theme-finish", {"token": gallery["token"], "action": "cancel"})
finally:
    machine.execute("kill -INT $(cat /tmp/project1-recorder.pid)")
    machine.wait_until_succeeds("! kill -0 $(cat /tmp/project1-recorder.pid) 2>/dev/null", timeout=30)
    machine.copy_from_machine("/tmp/project1-review.mp4")

machine.succeed("journalctl -b -t noctalia --no-pager > /tmp/project1-shell.log")
machine.copy_from_machine("/tmp/project1-shell.log")
machine.succeed("cat > /tmp/project1-review.json <<'JSON'\n" + json.dumps({
    "captures": captures, "theme_catalog": 5, "modes": ["dark", "light"],
    "limitations": ["VM has no Bluetooth radio or display brightness control", "Calendar and weather network access are offline"],
}, indent=2) + "\nJSON")
machine.copy_from_machine("/tmp/project1-review.json")
