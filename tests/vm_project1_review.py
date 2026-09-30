"""Project 1 integration review, executed by the NixOS WM test driver.

Run against the normal Hyprland VM with matching shell and Infinite Desktop
inputs.
Set PROJECT1_EVDEV_PYTHON to the VM's packaged Python with evdev.
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
    requested_context = context
    context = {"calendar-strip": "calendar", "calendar-month": "calendar"}.get(context, context)
    deadline = time.monotonic() + 60
    while time.monotonic() < deadline:
        status = json.loads(shell("status"))
        if status["activePanelId"] == panel and (context is None or status["activePanelContext"] == context):
            if requested_context == "calendar-strip":
                layer = panel_layer()
                if layer is not None and 80 <= layer["h"] <= 260:
                    return
            elif requested_context == "calendar-month":
                layer = panel_layer()
                if layer is not None and layer["h"] >= 300:
                    return
            else:
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

def panel_layer():
    layers = json.loads(user("hyprctl-lua", "-j", "layers"))["Virtual-1"]["levels"]
    return next((layer for group in layers.values() for layer in group
                 if layer["namespace"] == "noctalia-attached-panel"), None)

def wait_layer_height(address, check, timeout=60):
    deadline = time.monotonic() + timeout
    last = None
    stable_since = None
    previous_height = None
    while time.monotonic() < deadline:
        last = panel_layer()
        if last is not None and last["address"] == address and check(last["h"]):
            now = time.monotonic()
            if previous_height is not None and abs(last["h"] - previous_height) <= 1:
                stable_since = stable_since or now
                if now - stable_since >= .6:
                    return last
            else:
                stable_since = None
            previous_height = last["h"]
        else:
            stable_since = None
            previous_height = None
        time.sleep(.1)
    raise AssertionError(last)

def camera_now():
    return json.loads(user("hyprland-canvas-viewport", "status"))["outputs"]["Virtual-1"]["camera"]

def wait_camera_settled(timeout=30):
    deadline = time.monotonic() + timeout
    started = time.monotonic()
    previous = None
    stable_since = None
    camera = None
    while time.monotonic() < deadline:
        camera = camera_now()
        now = time.monotonic()
        if previous is not None and all(abs(camera[key] - previous[key]) < .05
                                        for key in ("x", "y", "width", "height")):
            stable_since = stable_since or now
            if now - stable_since >= 1.25 and now - started >= 1.5:
                return camera
        else:
            stable_since = None
        previous = camera
        time.sleep(.2)
    raise AssertionError(("camera did not settle", camera))

input_python = os.environ["PROJECT1_EVDEV_PYTHON"]
machine.succeed("cat > /tmp/project1-pointer.py <<'PY'\n" + '''
import subprocess, sys, time
from evdev import UInput, ecodes as e
assert subprocess.run(['systemd-detect-virt', '--vm', '--quiet']).returncode == 0
with UInput({e.EV_KEY:[e.BTN_LEFT], e.EV_REL:[e.REL_X,e.REL_Y]}, name='Project1 VM pointer') as mouse:
    time.sleep(.4)
    subprocess.run(['hyprctl-lua','dispatch','movecursor',str(int(sys.argv[1])-1),sys.argv[2]],check=True)
    mouse.write(e.EV_REL,e.REL_X,1);mouse.syn();time.sleep(.12)
    if len(sys.argv)>3:
        mouse.write(e.EV_KEY,e.BTN_LEFT,1);mouse.syn();time.sleep(.06)
        mouse.write(e.EV_KEY,e.BTN_LEFT,0);mouse.syn()
    time.sleep(.12)
''' + "\nPY")

def pointer(x, y, click=False):
    machine.succeed(shlex.join(["env", *(f"{key}={value}" for key,value in env.items()),
        input_python, "/tmp/project1-pointer.py", str(x), str(y), *(["click"] if click else [])]))

# These background fixtures distract from shell pixels. Stop them only in this
# disposable guest; they remain part of the normal production configuration.
user("systemctl", "--user", "stop", "hyprland-canvas-app-codex.service",
     "codex-wallpaper-terminal-theme.path")
user("hyprctl-lua", "keyword", "monitor", "Virtual-1,1600x1000@60,0x0,1")
user("hyprctl-lua", "keyword", "monitor", "DVI-I-1,1024x768@60,1600x0,1")
user("hyprctl-lua", "dispatch", "focusmonitor", "Virtual-1")
user("hyprctl-lua", "dispatch", "movecursor", "800", "500")
user("pactl", "load-module", "module-null-sink", "sink_name=project1_vm",
     "sink_properties=device.description=Project1_VM_Audio")
user("pactl", "set-default-sink", "project1_vm")
machine.wait_until_succeeds(shlex.join(["runuser", "-u", "Zvampen04", "--", "env",
    *(f"{key}={value}" for key, value in env.items()), "noctalia", "msg", "volume-osd", "50"]), timeout=30)
time.sleep(3)

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

    # Open from the hovered clock through physical guest input, then navigate
    # the strip to month without replacing the retained layer surface.
    pointer(800, 27)
    time.sleep(.3)
    pointer(800, 27, True)
    wait_panel("control-center", "calendar")
    time.sleep(1)
    compact = panel_layer()
    assert compact is not None
    pointer(800, 105, True)
    expanded = wait_layer_height(compact["address"], lambda height: height > compact["h"] + 80)
    capture("project1-calendar-retained-month")
    machine.send_key("esc")
    returned = wait_layer_height(compact["address"], lambda height: height < expanded["h"] - 80)
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
    initial_camera = wait_camera_settled()
    for name in ("Canvas", "Neumorphism", "Coder", "Sonder", "Project 1"):
        gallery = backend("theme-gallery")
        assert not gallery["dirty"], (name, gallery)
        backend("theme-preview", {"token": gallery["token"], "selected": name})
        backend("theme-finish", {"token": gallery["token"], "action": "commit"})
        camera = wait_camera_settled()
        assert abs(camera["width"] / camera["height"] - 1.6) < .001, (name, camera)
        capture("theme-roundtrip-" + name.replace(" ", "-"))
    assert all(abs(camera[key] - initial_camera[key]) < 1 for key in initial_camera), (initial_camera, camera)
    gallery = backend("theme-gallery")
    assert not gallery["dirty"] and gallery["selected"] == "Project 1"
    backend("theme-finish", {"token": gallery["token"], "action": "cancel"})

    # The maintained Infinite Desktop base prewarms the editor during region
    # selection. Cancellation must retire it and restore selection state.
    user("sh", "-c", "hyprland-infinite-screenshot > /tmp/project1-screenshot.log 2>&1 & echo $! > /tmp/project1-screenshot.pid")
    editor = f"pgrep -u {uid} -f -- '[-][-]capture=/run/user/{uid}/hyprland-canvas-viewports/canvas-screenshot'"
    machine.wait_until_succeeds(editor, timeout=30)
    machine.send_key("esc")
    machine.wait_until_succeeds("! kill -0 $(cat /tmp/project1-screenshot.pid) 2>/dev/null", timeout=30)
    machine.wait_until_succeeds(f"! {editor}", timeout=30)
    capture("project1-screenshot-cancelled")
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
