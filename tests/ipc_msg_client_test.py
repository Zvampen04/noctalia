"""Exercise both real IPC executables against isolated Unix socket peers."""

import contextlib
import json
import os
from pathlib import Path
import queue
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest


MSG = str(Path(sys.argv[1]).resolve())
FULL = str(Path(sys.argv[2]).resolve())
del sys.argv[1:3]


class MsgClientTest(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="noctalia-msg-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cwd = self.root / "cwd with spaces å"
        self.cwd.mkdir()
        self.runtime = self.root / "runtime"
        self.runtime.mkdir(mode=0o700)
        self.environment = dict(os.environ)
        self.environment.update(
            XDG_RUNTIME_DIR=str(self.runtime),
            WAYLAND_DISPLAY="wayland-private",
            HOME=str(self.root),
            LANG="C.UTF-8",
            LC_ALL="C.UTF-8",
        )

    def clients(self):
        return [(MSG, []), (FULL, ["msg"])]

    def invoke(self, executable, prefix, args, environment=None):
        started = time.monotonic()
        result = subprocess.run(
            [executable, *prefix, *args],
            cwd=self.cwd,
            env=environment or self.environment,
            capture_output=True,
            timeout=6,
            check=False,
        )
        return result, time.monotonic() - started

    def exchange(self, executable, prefix, args, reply=b"ok\n", hold=0, environment=None):
        environment = environment or self.environment
        display = environment.get("WAYLAND_DISPLAY") or "wayland-0"
        path = self.runtime / f"noctalia-{display}.sock"
        observations = queue.Queue(maxsize=1)
        failures = queue.Queue(maxsize=1)
        with contextlib.closing(socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)) as listener:
            listener.bind(str(path))
            listener.listen(1)
            listener.settimeout(5)

            def serve():
                try:
                    with listener.accept()[0] as peer:
                        peer.settimeout(5)
                        credentials = struct.unpack(
                            "3i", peer.getsockopt(socket.SOL_SOCKET, socket.SO_PEERCRED, 12)
                        )
                        request = bytearray()
                        while True:
                            chunk = peer.recv(4096)
                            if not chunk:
                                break
                            request.extend(chunk)
                            if len(request) > 1024 * 1024:
                                raise AssertionError("request exceeded contract bound")
                        observations.put((bytes(request), credentials))
                        if hold:
                            time.sleep(hold)
                        for offset in range(0, len(reply), 1021):
                            peer.sendall(reply[offset : offset + 1021])
                except BrokenPipeError:
                    # The shared client closes after its existing socket timeout.
                    if not hold:
                        failures.put(sys.exc_info()[1])
                except BaseException as error:
                    failures.put(error)

            thread = threading.Thread(target=serve, name="private-ipc-peer")
            thread.start()
            try:
                result, elapsed = self.invoke(executable, prefix, args, environment)
            finally:
                thread.join(6)
                listener.close()
                path.unlink(missing_ok=True)
            self.assertFalse(thread.is_alive(), "private peer must finish and close")
            if not failures.empty():
                raise failures.get_nowait()
            request, credentials = observations.get_nowait()
            self.assertGreater(credentials[0], 0)
            self.assertEqual(credentials[1:], (os.getuid(), os.getgid()))
            return result, elapsed, request

    def compare_exchange(self, args, reply=b"ok\n", environment=None):
        outcomes = [
            self.exchange(executable, prefix, args, reply=reply, environment=environment)
            for executable, prefix in self.clients()
        ]
        left, right = outcomes
        self.assertEqual(left[2], right[2])
        self.assertEqual(
            (left[0].returncode, left[0].stdout, left[0].stderr),
            (right[0].returncode, right[0].stdout, right[0].stderr),
        )
        return left

    def test_status_cwd_and_chunked_response(self):
        reply = ("status å\n" * 2000).encode()
        result, _, request = self.compare_exchange(["status"], reply=reply)
        self.assertEqual(request, str(self.cwd).encode() + b"\x1estatus")
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, reply)
        self.assertEqual(result.stderr, b"")

    def test_argument_text_and_server_error(self):
        args = ["panel-open", "name with spaces", "", "$PWD\n%u", "quoted'\"value"]
        result, _, request = self.compare_exchange(args, reply=b"error: rejected\n")
        self.assertEqual(
            request,
            str(self.cwd).encode() + b"\x1epanel-open name with spaces  $PWD\n%u quoted'\"value",
        )
        self.assertEqual(result.returncode, 1)
        self.assertEqual(result.stdout, b"error: rejected\n")

    def test_notification_json(self):
        result, _, request = self.compare_exchange(
            ["notification-show", "title å ' \"", "body\nline", "", "$1"]
        )
        cwd, command = request.split(b"\x1e", 1)
        self.assertEqual(cwd, str(self.cwd).encode())
        self.assertTrue(command.startswith(b"notification-show "))
        self.assertEqual(
            json.loads(command[len(b"notification-show ") :]),
            {"summary": "title å ' \"", "body": "body\nline  $1"},
        )
        self.assertEqual(result.returncode, 0)

    def test_default_display_environment(self):
        environment = dict(self.environment)
        environment.pop("WAYLAND_DISPLAY")
        result, _, request = self.compare_exchange(["status"], environment=environment)
        self.assertEqual(request, str(self.cwd).encode() + b"\x1estatus")
        self.assertEqual(result.returncode, 0)

    def test_help_and_missing_command(self):
        for args in (["--help"], ["panel-open", "--help"], []):
            outcomes = [self.invoke(executable, prefix, args)[0] for executable, prefix in self.clients()]
            self.assertEqual(
                (outcomes[0].returncode, outcomes[0].stdout, outcomes[0].stderr),
                (outcomes[1].returncode, outcomes[1].stdout, outcomes[1].stderr),
            )
            self.assertEqual(outcomes[0].returncode, 0 if args else 1)
            if args:
                self.assertIn(b"Usage: noctalia msg", outcomes[0].stdout)
            else:
                self.assertIn(b"msg requires a command", outcomes[0].stderr)

    def test_absent_socket(self):
        for executable, prefix in self.clients():
            result, _ = self.invoke(executable, prefix, ["status"])
            self.assertEqual(result.returncode, 1)
            self.assertEqual(result.stdout, b"")
            self.assertEqual(result.stderr, b"error: noctalia is not running\n")

    def test_overlong_socket_path(self):
        environment = dict(self.environment, WAYLAND_DISPLAY="x" * 120)
        for executable, prefix in self.clients():
            result, _ = self.invoke(executable, prefix, ["status"], environment)
            self.assertEqual(result.returncode, 1)
            self.assertEqual(result.stderr, b"error: socket path too long\n")

    def test_empty_response_compatibility(self):
        result, _, _ = self.compare_exchange(["status"], reply=b"")
        self.assertEqual((result.returncode, result.stdout, result.stderr), (0, b"", b""))

    def test_existing_two_second_receive_timeout(self):
        for executable, prefix in self.clients():
            result, elapsed, request = self.exchange(
                executable, prefix, ["status"], reply=b"late\n", hold=2.6
            )
            self.assertEqual(request, str(self.cwd).encode() + b"\x1estatus")
            self.assertGreater(elapsed, 1.8)
            self.assertLess(elapsed, 2.6)
            # Preserve the current shared client's EOF/error interpretation.
            # This is not a claim that an empty timeout response proves success.
            self.assertEqual((result.returncode, result.stdout, result.stderr), (0, b"", b""))


if __name__ == "__main__":
    unittest.main(verbosity=2)
