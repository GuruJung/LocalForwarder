#!/usr/bin/env python3
"""Real OpenSSH integration tests; no third-party Python packages or user SSH files.

The native test entry point runs the application's production session controller.
All server keys, client configuration, profiles and endpoints belong to this run.
An owned TCP relay supplies repeatable connection loss without touching system sshd.
"""

from __future__ import annotations

import argparse
import base64
import contextlib
import ctypes
import getpass
import json
import os
from pathlib import Path
import platform
import shutil
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import traceback
import unittest


WINDOWS = os.name == "nt"
PROFILE_ID = "fixture"


def run(command: list[str], *, timeout: float = 30, **kwargs) -> subprocess.CompletedProcess:
    result = subprocess.run(command, capture_output=True, text=True, timeout=timeout, **kwargs)
    if result.returncode:
        raise RuntimeError(f"{command!r} exited {result.returncode}\n{result.stdout}\n{result.stderr}")
    return result


def ports(count: int) -> list[int]:
    """Reserve distinct ephemeral ports together, then return them to the OS."""
    with contextlib.ExitStack() as stack:
        sockets = [stack.enter_context(socket.socket()) for _ in range(count)]
        for item in sockets:
            item.bind(("127.0.0.1", 0))
        return [item.getsockname()[1] for item in sockets]


def ssh_path(path: Path) -> str:
    return '"' + str(path.absolute()).replace("\\", "/").replace('"', '\\"') + '"'


def find_tool(name: str, explicit: str | None = None) -> str:
    candidates = [explicit]
    if WINDOWS:
        candidates.extend([
            str(Path(os.environ.get("SystemRoot", r"C:\Windows")) / "System32/OpenSSH" / (name + ".exe")),
        ])
    else:
        candidates.extend(["/usr/sbin/" + name, "/usr/bin/" + name])
    candidates.append(shutil.which(name))
    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return str(Path(candidate).absolute())
    raise RuntimeError(f"Required executable {name} is missing. Install it on the disposable CI runner.")


class EchoServer:
    def __init__(self):
        self.socket = socket.socket()
        self.socket.bind(("127.0.0.1", 0))
        self.socket.listen()
        self.port = self.socket.getsockname()[1]
        self.closed = threading.Event()
        self.connections: set[socket.socket] = set()
        self.lock = threading.Lock()
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while not self.closed.is_set():
            try:
                connection, _ = self.socket.accept()
            except OSError:
                return
            with self.lock:
                self.connections.add(connection)
            threading.Thread(target=self._echo, args=(connection,), daemon=True).start()

    def _echo(self, connection):
        try:
            with connection:
                while chunk := connection.recv(65536):
                    connection.sendall(chunk)
        except OSError:
            pass
        finally:
            with self.lock:
                self.connections.discard(connection)

    def close(self):
        self.closed.set()
        self.socket.close()
        with self.lock:
            for connection in self.connections:
                with contextlib.suppress(OSError):
                    connection.shutdown(socket.SHUT_RDWR)
                connection.close()


def socks_echo(proxy_port: int, destination_port: int):
    """Verify an inherited DynamicForward with actual SOCKS5 data delivery."""
    def receive(connection, count):
        result = b""
        while len(result) < count:
            chunk = connection.recv(count - len(result))
            if not chunk:
                raise AssertionError("SOCKS5 stream ended early")
            result += chunk
        return result
    with socket.create_connection(("127.0.0.1", proxy_port), timeout=4) as connection:
        connection.settimeout(4)
        connection.sendall(b"\x05\x01\x00")
        if receive(connection, 2) != b"\x05\x00":
            raise AssertionError("SOCKS5 authentication negotiation failed")
        connection.sendall(b"\x05\x01\x00\x01" + socket.inet_aton("127.0.0.1") +
                           destination_port.to_bytes(2, "big"))
        header = receive(connection, 4)
        if header[:3] != b"\x05\x00\x00":
            raise AssertionError(f"SOCKS5 connection failed: {header!r}")
        if header[3] == 1:
            receive(connection, 4)
        elif header[3] == 4:
            receive(connection, 16)
        elif header[3] == 3:
            receive(connection, receive(connection, 1)[0])
        else:
            raise AssertionError("Unknown SOCKS5 address type")
        receive(connection, 2)
        payload = b"LocalForwarder dynamic forwarding\n"
        connection.sendall(payload)
        if receive(connection, len(payload)) != payload:
            raise AssertionError("DynamicForward corrupted data")


class Relay:
    def __init__(self, destination_port: int):
        self.destination_port = destination_port
        self.socket = socket.socket()
        self.socket.bind(("127.0.0.1", 0))
        self.socket.listen()
        self.port = self.socket.getsockname()[1]
        self.lock = threading.Lock()
        self.connections: set[socket.socket] = set()
        self.enabled = True
        self.reject_count = 0
        self.reject_gracefully = False
        self.accepted: list[float] = []
        self.closed = False
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while not self.closed:
            try:
                incoming, _ = self.socket.accept()
            except OSError:
                return
            with self.lock:
                self.accepted.append(time.monotonic())
                allowed = self.enabled and self.reject_count == 0
                if self.reject_count:
                    self.reject_count -= 1
                graceful = self.reject_gracefully
            if not allowed:
                if graceful:
                    # Consume the client identification before sending FIN. Closing
                    # with unread data can produce a reset whose Windows OpenSSH
                    # errno is only "Unknown error", not classifiable evidence.
                    incoming.settimeout(2)
                    with contextlib.suppress(OSError):
                        banner = b""
                        while not banner.endswith(b"\n") and len(banner) < 1024:
                            data = incoming.recv(1)
                            if not data:
                                break
                            banner += data
                        incoming.shutdown(socket.SHUT_WR)
                        while incoming.recv(1024):
                            pass
                incoming.close()
                continue
            try:
                outgoing = socket.create_connection(("127.0.0.1", self.destination_port), timeout=5)
                outgoing.settimeout(None)
            except OSError:
                incoming.close()
                continue
            with self.lock:
                if not self.enabled or self.closed:
                    incoming.close()
                    outgoing.close()
                    continue
                self.connections.update((incoming, outgoing))
            threading.Thread(target=self._copy, args=(incoming, outgoing), daemon=True).start()
            threading.Thread(target=self._copy, args=(outgoing, incoming), daemon=True).start()

    def _copy(self, source, destination):
        try:
            while data := source.recv(65536):
                destination.sendall(data)
        except OSError:
            pass
        finally:
            for connection in (source, destination):
                with contextlib.suppress(OSError):
                    connection.shutdown(socket.SHUT_RDWR)
                connection.close()
            with self.lock:
                self.connections.discard(source)
                self.connections.discard(destination)

    def disconnect(self, *, block: bool):
        with self.lock:
            self.enabled = not block
            for connection in list(self.connections):
                with contextlib.suppress(OSError):
                    connection.shutdown(socket.SHUT_RDWR)
                connection.close()
            self.connections.clear()

    def allow(self):
        with self.lock:
            self.enabled = True

    def close(self):
        self.closed = True
        self.disconnect(block=True)
        self.socket.close()


class Fixture:
    def __init__(self, sshd: str | None = None):
        self.temporary = tempfile.TemporaryDirectory(prefix="lf-integration-")
        self.root = Path(self.temporary.name)
        self.sshd = find_tool("sshd", sshd)
        self.ssh = find_tool("ssh")
        self.keygen = find_tool("ssh-keygen")
        self.server_port = ports(1)[0]
        self.user = getpass.getuser()
        self.host_key = self.root / "host_key"
        self.client_key = self.root / "client_key"
        self.home = self.root / "remote-home"
        (self.home / ".ssh").mkdir(parents=True)
        self.authorized = self.home / ".ssh/authorized_keys"
        self.server = None
        self.relay = None
        self.echo = None
        self.server_log = None
        try:
            for path in (self.host_key, self.client_key):
                run([self.keygen, "-q", "-t", "ed25519", "-N", "", "-f", str(path)])
            self.authorized.write_text("# fixture key: preserve this comment\n" +
                                       self.client_key.with_suffix(".pub").read_text(), encoding="utf-8")
            if not WINDOWS:
                self.authorized.chmod(0o600)
            self.server_config = self.root / "sshd_config"
            lines = [
                f"Port {self.server_port}", "ListenAddress 127.0.0.1",
                f"HostKey {ssh_path(self.host_key)}",
                f"PidFile {ssh_path(self.root / 'sshd.pid')}",
                f"AuthorizedKeysFile {ssh_path(self.authorized)}",
                "PasswordAuthentication no", "KbdInteractiveAuthentication no",
                "PubkeyAuthentication yes", "StrictModes no", "AllowTcpForwarding yes",
                "PermitRootLogin prohibit-password", "LogLevel VERBOSE",
            ]
            if not WINDOWS:
                lines.append("UsePAM no")
                # This redirects only this daemon's remote command environment. It lets
                # auth setup tests append to a disposable HOME, never the real user home.
                wrapper = self.root / "remote-command.sh"
                wrapper.write_text("#!/bin/sh\nexport HOME=" + shell_quote(str(self.home)) +
                                   '\nexec /bin/sh -c "$SSH_ORIGINAL_COMMAND"\n', encoding="utf-8")
                wrapper.chmod(0o700)
                lines.append("ForceCommand /bin/sh " + shell_quote(str(wrapper)))
            self.server_config.write_text("\n".join(lines) + "\n", encoding="utf-8")
            self.server_log = (self.root / "sshd.log").open("w", encoding="utf-8")
            command = [self.sshd, "-D", "-e", "-f", str(self.server_config)]
            # Apple sshd needs privilege to create sessions for the runner account.
            # Only this owned daemon uses sudo; Remote Login service is untouched.
            self.sudo = platform.system() == "Darwin" and os.geteuid() != 0
            if self.sudo:
                command = ["sudo", "-n", *command]
            self.server = subprocess.Popen(command, stdout=self.server_log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 15
            while True:
                if self.server.poll() is not None:
                    raise RuntimeError("Isolated sshd exited:\n" + self.server_diagnostics())
                try:
                    with socket.create_connection(("127.0.0.1", self.server_port), timeout=.3):
                        break
                except OSError:
                    if time.monotonic() >= deadline:
                        raise RuntimeError("Isolated sshd did not listen:\n" + self.server_diagnostics())
                    time.sleep(.1)
            self.echo = EchoServer()
            self.relay = Relay(self.server_port)
            self.known_hosts = self.root / "known_hosts"
            host_fields = self.host_key.with_suffix(".pub").read_text().split()
            self.known_hosts.write_text(
                f"[127.0.0.1]:{self.relay.port} {host_fields[0]} {host_fields[1]}\n", encoding="utf-8")
            self.ssh_config = self.root / "ssh_config"
            self.inherited_port, self.remote_port = ports(2)
            self.write_ssh_config()
            # Fail with the real server log before app behavior is blamed for fixture setup.
            probe = subprocess.run([self.ssh, "-F", str(self.ssh_config), "-o", "BatchMode=yes",
                                    "-o", "ClearAllForwardings=yes", "lf-fixture", "echo fixture-ready"],
                                   capture_output=True, text=True, timeout=20)
            if probe.returncode or "fixture-ready" not in probe.stdout:
                raise RuntimeError(f"Fixture SSH authentication failed:\n{probe.stdout}\n{probe.stderr}\n" +
                                   self.server_diagnostics())
        except BaseException:
            self.close()
            raise

    def write_ssh_config(self):
        self.ssh_config.write_text("\n".join([
            "Host lf-fixture", "  HostName 127.0.0.1", f"  Port {self.relay.port}",
            f"  User {self.user}", f"  IdentityFile {ssh_path(self.client_key)}",
            f"  UserKnownHostsFile {ssh_path(self.known_hosts)}", "  GlobalKnownHostsFile none",
            "  StrictHostKeyChecking yes", "  IdentitiesOnly yes", "  LogLevel DEBUG1",
            f"  LocalForward 127.0.0.1:{self.inherited_port} 127.0.0.1:{self.echo.port}",
            f"  RemoteForward 127.0.0.1:{self.remote_port} 127.0.0.1:{self.echo.port}",
        ]) + "\n", encoding="utf-8")

    def profile(self, local_ports=None, *, target_ports=None, name="fixture"):
        local_ports = local_ports or ports(2)
        target_ports = target_ports or [self.echo.port] * len(local_ports)
        host = {"id": PROFILE_ID, "name": "테스트 " + name, "target": "lf-fixture",
                "identityFile": str(self.client_key), "forwards": []}
        for index, (local, remote) in enumerate(zip(local_ports, target_ports)):
            host["forwards"].append({"id": f"rule-{index}", "enabled": True,
                                     "localPort": local, "remoteHost": "127.0.0.1", "remotePort": remote})
        return {"version": 2, "hosts": [host]}

    def config_directory(self, name, config):
        directory = self.root / ("설정 " + name)
        directory.mkdir()
        self.write_config(directory, config)
        return directory

    @staticmethod
    def write_config(directory, config):
        temporary = directory / "config.tmp"
        temporary.write_text(json.dumps(config, ensure_ascii=False, indent=2), encoding="utf-8")
        temporary.replace(directory / "config.json")

    def server_diagnostics(self):
        if self.server_log:
            self.server_log.flush()
        path = self.root / "sshd.log"
        return path.read_text(encoding="utf-8", errors="replace")[-16000:] if path.exists() else ""

    def close(self):
        if self.relay:
            self.relay.close()
        if self.echo:
            self.echo.close()
        if self.server and self.server.poll() is None:
            # PidFile is created by the owned daemon, not a global service.
            if getattr(self, "sudo", False):
                pid_file = self.root / "sshd.pid"
                if pid_file.exists():
                    pid = pid_file.read_text().strip()
                    if pid.isdecimal():
                        subprocess.run(["sudo", "-n", "/bin/kill", "-TERM", pid], capture_output=True)
            else:
                self.server.terminate()
            try:
                self.server.wait(timeout=8)
            except subprocess.TimeoutExpired:
                self.server.kill()
                self.server.wait(timeout=5)
        if self.server_log:
            self.server_log.close()
        self.temporary.cleanup()


def shell_quote(value: str) -> str:
    return "'" + value.replace("'", "'\\''") + "'"


def echo(port: int, payload: bytes = b"LocalForwarder real SSH\n"):
    with socket.create_connection(("127.0.0.1", port), timeout=4) as connection:
        connection.settimeout(4)
        connection.sendall(payload)
        answer = b""
        while len(answer) < len(payload):
            chunk = connection.recv(len(payload) - len(answer))
            if not chunk:
                break
            answer += chunk
        if answer != payload:
            raise AssertionError(f"Forward {port} returned {answer!r}, expected {payload!r}")


def wait_port_closed(port: int, timeout: float = 5):
    deadline = time.monotonic() + timeout
    while True:
        try:
            connection = socket.create_connection(("127.0.0.1", port), timeout=.2)
            connection.close()
        except OSError:
            return
        if time.monotonic() >= deadline:
            raise AssertionError(f"Local port {port} still listening after app exit")
        time.sleep(.05)


def process_metrics(pid: int) -> dict:
    if WINDOWS:
        from ctypes import wintypes

        class Counters(ctypes.Structure):
            _fields_ = [("cb", wintypes.DWORD), ("PageFaultCount", wintypes.DWORD),
                        ("PeakWorkingSetSize", ctypes.c_size_t), ("WorkingSetSize", ctypes.c_size_t),
                        ("QuotaPeakPagedPoolUsage", ctypes.c_size_t), ("QuotaPagedPoolUsage", ctypes.c_size_t),
                        ("QuotaPeakNonPagedPoolUsage", ctypes.c_size_t), ("QuotaNonPagedPoolUsage", ctypes.c_size_t),
                        ("PagefileUsage", ctypes.c_size_t), ("PeakPagefileUsage", ctypes.c_size_t)]

        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.GetProcessTimes.argtypes = [wintypes.HANDLE] + [ctypes.POINTER(wintypes.FILETIME)] * 4
        handle = kernel.OpenProcess(0x0400 | 0x0010, False, pid)
        if not handle:
            raise ctypes.WinError(ctypes.get_last_error())
        try:
            times = [wintypes.FILETIME() for _ in range(4)]
            if not kernel.GetProcessTimes(handle, *[ctypes.byref(item) for item in times]):
                raise ctypes.WinError(ctypes.get_last_error())
            cpu = sum((item.dwHighDateTime << 32) + item.dwLowDateTime for item in times[2:]) / 10_000_000
            counters = Counters()
            counters.cb = ctypes.sizeof(counters)
            psapi = ctypes.WinDLL("psapi", use_last_error=True)
            psapi.GetProcessMemoryInfo.argtypes = [wintypes.HANDLE, ctypes.POINTER(Counters), wintypes.DWORD]
            if not psapi.GetProcessMemoryInfo(handle, ctypes.byref(counters), counters.cb):
                raise ctypes.WinError(ctypes.get_last_error())
            return {"cpu_seconds": cpu, "rss_bytes": counters.WorkingSetSize}
        finally:
            kernel.CloseHandle(handle)
    output = run(["ps", "-p", str(pid), "-o", "time=", "-o", "rss="]).stdout.strip().split()
    time_value, rss = output[-2:]
    days = 0
    if "-" in time_value:
        day_text, time_value = time_value.split("-", 1)
        days = int(day_text)
    fields = [float(value) for value in time_value.split(":")]
    seconds = 0.0
    for value in fields:
        seconds = seconds * 60 + value
    return {"cpu_seconds": seconds + days * 86400, "rss_bytes": int(rss) * 1024}


class ManagerSession:
    """JSON control adapter around the production multi-host ConnectionManager."""
    def __init__(self, app, fixture, directory, ssh_configuration, *, start=True):
        command = [app, "--test-manager", str(directory), str(ssh_configuration)]
        if not start:
            command.append("--no-start")
        self.process = subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=subprocess.PIPE, text=True, encoding="utf-8",
                                        errors="replace", bufsize=1)
        self.condition = threading.Condition()
        self.host_events = []
        self.snapshot_serial = 0
        self.output = []
        self.errors = []
        self.readers = [threading.Thread(target=self._read, args=(self.process.stdout, False), daemon=True),
                        threading.Thread(target=self._read, args=(self.process.stderr, True), daemon=True)]
        for reader in self.readers:
            reader.start()

    def _read(self, stream, error):
        for line in stream:
            with self.condition:
                (self.errors if error else self.output).append(line.rstrip())
                if line.startswith("LF_HOST "):
                    self.host_events.append((time.monotonic(), json.loads(line[len("LF_HOST "):])))
                self.condition.notify_all()

    def send(self, command, **values):
        payload = {"command": command, **values}
        self.process.stdin.write(json.dumps(payload) + "\n")
        self.process.stdin.flush()

    def wait_host(self, host_id, state, *, after=0, timeout=30, **values):
        deadline = time.monotonic() + timeout
        with self.condition:
            while True:
                for index, (when, host) in enumerate(self.host_events[after:], start=after):
                    if host["id"] == host_id and host["state"] == state and all(
                            host.get(key) == value for key, value in values.items()):
                        return index, when, host
                self.assert_alive()
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AssertionError(f"Timed out waiting for {host_id}: {state} {values}:\n{self.diagnostics()}")
                self.condition.wait(min(remaining, .2))

    def snapshot(self, host_id):
        after = len(self.host_events)
        self.snapshot_serial += 1
        request_id = f"snapshot-{self.snapshot_serial}"
        self.send("snapshot", requestID=request_id)
        deadline = time.monotonic() + 5
        with self.condition:
            while True:
                for _, host in self.host_events[after:]:
                    if host["id"] == host_id and host.get("requestID") == request_id:
                        return host
                self.assert_alive()
                if time.monotonic() >= deadline:
                    raise AssertionError(self.diagnostics())
                self.condition.wait(.1)

    def assert_alive(self):
        if self.process.poll() is not None:
            raise AssertionError(f"App unexpectedly exited {self.process.returncode}:\n{self.diagnostics()}")

    def diagnostics(self):
        return "\n".join(self.output[-100:] + ["--- stderr ---"] + self.errors[-100:])

    def wait_exit(self, timeout):
        try:
            result = self.process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            error.add_note("Application output at timeout:\n" + self.diagnostics()[-24000:])
            raise
        for reader in self.readers:
            reader.join(timeout=2)
        return result

    def close(self):
        if self.process.poll() is None:
            with contextlib.suppress(OSError, BrokenPipeError):
                self.send("quit")
            try:
                self.wait_exit(timeout=8)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.wait_exit(timeout=5)
        for stream in (self.process.stdin, self.process.stdout, self.process.stderr):
            stream.close()


class MultiHostSSHTests(unittest.TestCase):
    """Production multi-host behavior shared by macOS and Windows."""
    fixture: Fixture
    app: str
    report: dict

    def setUp(self):
        self.fixture.relay.allow()
        self.sessions: list[ManagerSession] = []
        self.report["tests_started"].append(self.id())

    def tearDown(self):
        try:
            for session in self.sessions:
                session.close()
            self.fixture.relay.disconnect(block=False)
            wait_port_closed(self.fixture.inherited_port)
            wait_port_closed(self.fixture.remote_port)
        finally:
            with self.fixture.relay.lock:
                self.fixture.relay.reject_count = 0

    @unittest.skipIf(WINDOWS, "Registration targets a POSIX server; native Windows sshd is not that environment")
    def test_public_key_registration(self):
        fixture = self.fixture
        selected_key = fixture.root / "registration_key"
        run([fixture.keygen, "-q", "-t", "ed25519", "-N", "", "-f", str(selected_key)])
        config = fixture.profile()
        config["hosts"][0]["identityFile"] = str(selected_key)
        directory = fixture.config_directory("auth-registration", config)
        original = fixture.authorized.read_text(encoding="utf-8")
        expected_key = selected_key.with_suffix(".pub").read_text(encoding="utf-8").split()[1]
        command = [self.app, "--test-setup-auth", str(directory), PROFILE_ID, str(fixture.ssh_config)]
        # Baseline -F IdentityFile can authenticate the registration. Explicit selected
        # key is deliberately not yet authorized; the helper must actually append it.
        run(command, timeout=45, input="\n")
        updated = fixture.authorized.read_text(encoding="utf-8")
        self.assertTrue(updated.startswith(original), updated)
        self.assertEqual(updated.count(expected_key), 1)
        run(command, timeout=45, input="\n")
        self.assertEqual(fixture.authorized.read_text(encoding="utf-8").count(expected_key), 1)
        # Existing key options must survive registration. Adding an unrestricted
        # duplicate would silently bypass restrictions even though the key is the same.
        current = fixture.authorized.read_text(encoding="utf-8")
        selected_lines = [line for line in current.splitlines(keepends=True)
                          if expected_key in line.split()]
        self.assertEqual(len(selected_lines), 1)
        restricted_line = "no-agent-forwarding,no-X11-forwarding " + selected_lines[0]
        restricted = current.replace(selected_lines[0], restricted_line, 1)
        fixture.authorized.write_text(restricted, encoding="utf-8")
        run(command, timeout=45, input="\n")
        after_registration = fixture.authorized.read_text(encoding="utf-8")
        self.assertEqual(after_registration.count(expected_key), 1)
        self.assertIn(restricted_line, after_registration)
        self.assertEqual(after_registration, restricted)
        selected_config = fixture.root / "registered_key_config"
        selected_config.write_text(fixture.ssh_config.read_text(encoding="utf-8").replace(
            ssh_path(fixture.client_key), ssh_path(selected_key)), encoding="utf-8")
        output = run([fixture.ssh, "-F", str(selected_config), "-o", "BatchMode=yes",
                      "-o", "ClearAllForwardings=yes", "lf-fixture", "echo registered-key-works"])
        self.assertIn("registered-key-works", output.stdout)

    def profile(self, count=1):
        hosts = []
        for index in range(count):
            host = self.fixture.profile(ports(1))["hosts"][0]
            host.update(id=f"host-{index}", autoConnect=True, exitOnForwardFailure=True,
                        retryOnConnectionRefused=True)
            hosts.append(host)
        return {"version": 2, "hosts": hosts}

    def start(self, name, config=None, *, start=True, inherited=False, dynamic_port=None):
        config = config or self.profile()
        directory = self.fixture.config_directory(name, config)
        # Shared inherited listeners would make independently valid hosts conflict.
        # Test inheritance separately with one host, and leave the fixture itself unchanged.
        private = directory / "ssh_config"
        lines = self.fixture.ssh_config.read_text(encoding="utf-8").splitlines()
        if not inherited:
            lines = [line for line in lines if not line.strip().startswith(("LocalForward ", "RemoteForward "))]
        if dynamic_port is not None:
            lines.append(f"  DynamicForward 127.0.0.1:{dynamic_port}")
        private.write_text("\n".join(lines) + "\n", encoding="utf-8")
        session = ManagerSession(self.app, self.fixture, directory, private, start=start)
        self.sessions.append(session)
        return session, directory, config

    def test_01_independent_hosts_manual_intent_and_whole_cleanup(self):
        config = self.profile(3)
        config["hosts"][2]["autoConnect"] = False
        session, _, _ = self.start("multi-independent", config)
        for host in config["hosts"][:2]:
            session.wait_host(host["id"], "connected")
            echo(host["forwards"][0]["localPort"])
        session.wait_host("host-2", "disconnected", intent=False)
        session.send("connect", id="host-2")
        session.wait_host("host-2", "connected", intent=True)
        echo(config["hosts"][2]["forwards"][0]["localPort"])
        before = len(session.host_events)
        session.send("disconnect", id="host-0")
        session.wait_host("host-0", "disconnected", after=before, intent=False)
        wait_port_closed(config["hosts"][0]["forwards"][0]["localPort"])
        session.send("wake")
        time.sleep(6)
        self.assertFalse(session.snapshot("host-0")["intent"])
        self.assertEqual(session.snapshot("host-0")["state"], "disconnected")
        for host in config["hosts"][1:]:
            echo(host["forwards"][0]["localPort"])
        session.send("quit")
        self.assertEqual(session.wait_exit(8), 0, session.diagnostics())
        for host in config["hosts"]:
            wait_port_closed(host["forwards"][0]["localPort"])

    def test_02_event_recovery_debounces_and_leaves_healthy_host_alone(self):
        config = self.profile(2)
        # One direct connection remains healthy while the relay disconnects its peer.
        config["hosts"][1]["port"] = self.fixture.server_port
        direct_key = self.fixture.known_hosts.read_text().replace(
            f"]:{self.fixture.relay.port} ", f"]:{self.fixture.server_port} ")
        with self.fixture.known_hosts.open("a") as file:
            file.write(direct_key)
        session, _, _ = self.start("multi-recovery", config)
        for host in config["hosts"]:
            session.wait_host(host["id"], "connected")
        before = len(session.host_events)
        self.fixture.relay.disconnect(block=False)
        lost, _, _ = session.wait_host("host-0", "disconnected", after=before, intent=True)
        session.send("network", available=False)
        session.send("network", available=True)
        time.sleep(2)
        last_event = time.monotonic()
        session.send("wake")
        recovered, when, _ = session.wait_host("host-0", "connected", after=lost + 1, timeout=20)
        self.assertGreaterEqual(when - last_event, 4.8)
        self.assertLess(when - last_event, 15)
        healthy_events = [host["state"] for _, host in session.host_events[lost + 1:recovered + 1]
                          if host["id"] == "host-1"]
        self.assertTrue(all(state == "connected" for state in healthy_events), session.diagnostics())
        echo(config["hosts"][1]["forwards"][0]["localPort"])
        self.report["recovery_delay_after_last_event_seconds"] = when - last_event

    def test_03_temporary_override_survives_recovery_and_save_clears_it(self):
        session, directory, config = self.start("multi-temporary")
        session.wait_host("host-0", "connected")
        original = config["hosts"][0]["forwards"][0]["localPort"]
        temporary = json.loads(json.dumps(config["hosts"][0]))
        temporary["forwards"][0]["localPort"] = ports(1)[0]
        before = len(session.host_events)
        session.send("temporary", host=temporary)
        session.wait_host("host-0", "connected", after=before, temporary=True)
        echo(temporary["forwards"][0]["localPort"])
        wait_port_closed(original)
        self.assertEqual(json.loads((directory / "config.json").read_text(encoding="utf-8")), config)
        before = len(session.host_events)
        self.fixture.relay.disconnect(block=False)
        lost, _, _ = session.wait_host("host-0", "disconnected", after=before)
        session.send("wake")
        session.wait_host("host-0", "connected", after=lost + 1, temporary=True)
        echo(temporary["forwards"][0]["localPort"])
        # Simulate a successful per-host save notification: it changes the next
        # configuration, but must not tear down this live temporary connection.
        session.send("saved", id="host-0")
        state = session.snapshot("host-0")
        self.assertFalse(state["temporary"])
        self.assertEqual(state["state"], "connected")
        echo(temporary["forwards"][0]["localPort"])
        before = len(session.host_events)
        session.send("connect", id="host-0")
        # Late diagnostics can still report the old connected session after our
        # event cursor. Require the new attempt before accepting connected.
        connecting, _, _ = session.wait_host("host-0", "connecting", after=before, temporary=False)
        session.wait_host("host-0", "connected", after=connecting + 1, temporary=False)
        echo(original)
        wait_port_closed(temporary["forwards"][0]["localPort"])

    def test_04_retry_only_once_and_option_off(self):
        relay = self.fixture.relay
        with relay.lock:
            relay.reject_gracefully = True
        def restore_rejection():
            with relay.lock:
                relay.reject_gracefully = False
        self.addCleanup(restore_rejection)
        for retry in (True, False):
            with self.subTest(retry=retry):
                config = self.profile()
                config["hosts"][0]["retryOnConnectionRefused"] = retry
                with relay.lock:
                    relay.reject_count = 1
                    before_attempts = len(relay.accepted)
                session, _, _ = self.start("multi-refusal-" + str(retry), config)
                if retry:
                    session.wait_host("host-0", "connected")
                else:
                    trying, _, _ = session.wait_host("host-0", "connecting")
                    session.wait_host("host-0", "disconnected", after=trying + 1)
                    time.sleep(2)
                attempts = relay.accepted[before_attempts:]
                self.assertEqual(len(attempts), 2 if retry else 1, session.diagnostics())
                if retry:
                    self.assertGreaterEqual(attempts[1] - attempts[0], .8)
                session.assert_alive()
                session.close()
        with relay.lock:
            relay.reject_count = 10
            before_attempts = len(relay.accepted)
        session, _, _ = self.start("multi-refusal-exhausted")
        session.wait_host("host-0", "connecting")
        deadline = time.monotonic() + 10
        while len(relay.accepted) - before_attempts < 2:
            self.assertLess(time.monotonic(), deadline, session.diagnostics())
            time.sleep(.1)
        time.sleep(3)
        self.assertEqual(len(relay.accepted) - before_attempts, 2)
        self.assertEqual(session.snapshot("host-0")["state"], "disconnected")
        session.assert_alive()
        with relay.lock:
            relay.reject_count = 0

    def test_05_partial_forward_failure_option_and_peer_isolation(self):
        for fail_all in (True, False):
            with self.subTest(exitOnForwardFailure=fail_all), socket.socket() as occupied:
                if WINDOWS:
                    occupied.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
                occupied.bind(("127.0.0.1", 0))
                occupied.listen()
                config = self.profile(2)
                host = config["hosts"][1]
                host["exitOnForwardFailure"] = fail_all
                host["forwards"].append({"id": "occupied", "enabled": True,
                    "localPort": occupied.getsockname()[1], "remoteHost": "127.0.0.1",
                    "remotePort": self.fixture.echo.port})
                session, _, _ = self.start("multi-partial-" + str(fail_all), config)
                session.wait_host("host-0", "connected")
                if fail_all:
                    trying, _, _ = session.wait_host("host-1", "connecting")
                    session.wait_host("host-1", "disconnected", after=trying + 1)
                    wait_port_closed(host["forwards"][0]["localPort"])
                else:
                    _, _, state = session.wait_host("host-1", "connected")
                    echo(host["forwards"][0]["localPort"])
                    self.assertEqual({rule["id"]: rule["state"] for rule in state["forwards"]},
                                     {"rule-0": "ready", "occupied": "failed"}, session.diagnostics())
                echo(config["hosts"][0]["forwards"][0]["localPort"])
                session.assert_alive()
                session.close()

    def test_06_no_unsolicited_reconnect_or_exit_and_idle_evidence(self):
        config = self.profile(2)
        config["hosts"][1]["autoConnect"] = False
        session, _, _ = self.start("multi-persistence", config)
        session.wait_host("host-0", "connected")
        before = len(session.host_events)
        self.fixture.relay.disconnect(block=False)
        index, _, _ = session.wait_host("host-0", "disconnected", after=before, intent=True)
        baseline = process_metrics(session.process.pid)
        before_attempts = len(self.fixture.relay.accepted)
        start = time.monotonic()
        time.sleep(65)
        elapsed = time.monotonic() - start
        session.assert_alive()
        final = process_metrics(session.process.pid)
        self.assertEqual(len(self.fixture.relay.accepted), before_attempts)
        self.assertFalse(any(host["state"] in ("connecting", "connected", "reconnecting")
                             for _, host in session.host_events[index + 1:]))
        self.report["idle"] = {"duration_seconds": elapsed,
            "cpu_seconds": final["cpu_seconds"] - baseline["cpu_seconds"],
            "cpu_percent_one_core": 100 * (final["cpu_seconds"] - baseline["cpu_seconds"]) / elapsed,
            "rss_bytes": final["rss_bytes"],
            "scope": "headless production multi-host manager after disconnection; excludes native UI"}

    def test_07_cancel_start_and_temporary_host_full_reconnect(self):
        session, _, config = self.start("multi-cancel", start=False)
        session.send("wake")
        time.sleep(6)
        state = session.snapshot("host-0")
        self.assertEqual(state["state"], "disconnected")
        self.assertFalse(state["intent"])
        temporary = self.profile()["hosts"][0]
        temporary["id"] = "temporary-new"
        session.send("temporary", host=temporary)
        session.wait_host("temporary-new", "connected", temporary=True)
        before = len(session.host_events)
        session.send("reconnect-all")
        session.wait_host("host-0", "connected", after=before, intent=True)
        wait_port_closed(temporary["forwards"][0]["localPort"])
        echo(config["hosts"][0]["forwards"][0]["localPort"])
        session.send("delete", id="host-0")
        wait_port_closed(config["hosts"][0]["forwards"][0]["localPort"])
        session.assert_alive()

    def test_07b_disconnect_all_preserves_settings_and_blocks_recovery(self):
        config = self.profile(2)
        config["hosts"][1]["autoConnect"] = False
        session, directory, _ = self.start("multi-disconnect-all", config)
        original = (directory / "config.json").read_bytes()
        session.wait_host("host-0", "connected")
        session.send("connect", id="host-1")
        session.wait_host("host-1", "connected", intent=True)
        temporary = self.profile()["hosts"][0]
        temporary["id"] = "temporary-new"
        session.send("temporary", host=temporary)
        session.wait_host("temporary-new", "connected", temporary=True)
        hosts = config["hosts"] + [temporary]
        for host in hosts:
            echo(host["forwards"][0]["localPort"])
        # Cancel an already scheduled recovery as well as later recovery events.
        session.send("wake")
        before = len(session.host_events)
        session.send("disconnect-all")
        for host in hosts:
            session.wait_host(host["id"], "disconnected", after=before, intent=False)
            wait_port_closed(host["forwards"][0]["localPort"])
        deadline = time.monotonic() + 5
        while True:
            with self.fixture.relay.lock:
                remaining_transports = len(self.fixture.relay.connections)
            if not remaining_transports or time.monotonic() >= deadline:
                break
            time.sleep(.05)
        self.assertEqual(remaining_transports, 0, "Stopped SSH transports remain open")
        baseline = len(self.fixture.relay.accepted)
        session.send("disconnect-all")
        session.send("network", available=False)
        session.send("network", available=True)
        session.send("wake")
        time.sleep(6)
        self.assertEqual(len(self.fixture.relay.accepted), baseline)
        for host in hosts:
            state = session.snapshot(host["id"])
            self.assertEqual(state["state"], "disconnected")
            self.assertFalse(state["intent"])
        self.assertEqual((directory / "config.json").read_bytes(), original)
        self.assertTrue(session.snapshot("temporary-new")["temporary"])
        # Manual connect retains the unsaved host's execution settings.
        before = len(session.host_events)
        session.send("connect", id="temporary-new")
        session.wait_host("temporary-new", "connected", after=before, temporary=True)
        echo(temporary["forwards"][0]["localPort"])
        # Whole reconnect still applies only saved auto-connect hosts.
        before = len(session.host_events)
        session.send("reconnect-all")
        session.wait_host("host-0", "connected", after=before, intent=True)
        for host in (config["hosts"][1], temporary):
            self.assertFalse(session.snapshot(host["id"])["intent"])
            wait_port_closed(host["forwards"][0]["localPort"])
        echo(config["hosts"][0]["forwards"][0]["localPort"])
        self.assertEqual((directory / "config.json").read_bytes(), original)
        session.assert_alive()

    def test_08_inherited_rules_destination_failure_and_disabled_rule(self):
        dead_destination = ports(1)[0]
        config = self.profile()
        host = config["hosts"][0]
        host["forwards"].extend([
            {"id": "destination-down", "enabled": True, "localPort": ports(1)[0],
             "remoteHost": "127.0.0.1", "remotePort": dead_destination},
            {"id": "disabled", "enabled": False, "localPort": ports(1)[0],
             "remoteHost": "127.0.0.1", "remotePort": self.fixture.echo.port}])
        if WINDOWS:
            # Native Windows sshd can confirm a failed asynchronous TCP connect,
            # then send EOF without SSH_CHANNEL_OPEN_FAILURE. Keep the closed-port
            # check and separately provoke a synchronous destination lookup error.
            host["forwards"].append({"id": "destination-unresolved", "enabled": True,
                "localPort": ports(1)[0], "remoteHost": "localforwarder-destination.invalid.",
                "remotePort": 80})
        dynamic_port = ports(1)[0]
        session, _, _ = self.start("multi-inherited", config, inherited=True, dynamic_port=dynamic_port)
        session.wait_host("host-0", "connected")
        socks_echo(dynamic_port, self.fixture.echo.port)
        for port in (host["forwards"][0]["localPort"], self.fixture.inherited_port, self.fixture.remote_port):
            echo(port)
        wait_port_closed(host["forwards"][2]["localPort"])
        with socket.create_connection(("127.0.0.1", host["forwards"][1]["localPort"]), timeout=4) as connection:
            connection.settimeout(4)
            with contextlib.suppress(ConnectionResetError):
                self.assertEqual(connection.recv(1), b"")
        if WINDOWS:
            observed = session.snapshot("host-0")
            self.assertEqual(observed["state"], "connected")
            self.assertEqual(next(rule for rule in observed["forwards"]
                                  if rule["id"] == "destination-down")["state"], "ready")
            self.report["closed_destination"] = {
                "transport": "EOF or reset", "ssh_state": observed["state"],
                "reported_errors": observed.get("recentErrors", []),
                "policy": "Only emitted SSH errors are reported; no service probe or inferred error."}
            with socket.create_connection(("127.0.0.1", host["forwards"][3]["localPort"]), timeout=4) as connection:
                connection.settimeout(10)
                with contextlib.suppress(ConnectionResetError):
                    self.assertEqual(connection.recv(1), b"")
        deadline = time.monotonic() + 5
        while True:
            state = session.snapshot("host-0")
            if state.get("recentErrors"):
                break
            if time.monotonic() >= deadline:
                # Preserve the actual collected SSH diagnostics on failure. A
                # normal relay disconnect makes the app report its final log;
                # do not weaken the destination-error assertion.
                before = len(session.host_events)
                self.fixture.relay.disconnect(block=False)
                session.wait_host("host-0", "disconnected", after=before)
                self.fail(f"Destination error was not recorded: {state}\n{session.diagnostics()}")
            time.sleep(.1)
        self.assertEqual(state["state"], "connected")
        self.assertIn("time", state["recentErrors"][-1])
        self.assertTrue(any("open failed:" in error["message"] or "connect failed:" in error["message"]
                            for error in state["recentErrors"]))
        self.assertGreaterEqual(len(state["forwards"]), 4)
        session.send("quit")
        self.assertEqual(session.wait_exit(8), 0)
        for port in (self.fixture.inherited_port, self.fixture.remote_port, dynamic_port):
            wait_port_closed(port)


    @unittest.skipUnless(sys.platform == "darwin", "macOS Foundation query-worker shutdown regression")
    def test_09_quit_interrupts_inflight_ssh_configuration_inspection(self):
        fixture = self.fixture
        marker = fixture.root / "inspection-shell.pid"
        child_marker = fixture.root / "inspection-child.pid"
        script = fixture.root / "blocking-inspection.sh"
        script.write_text(
            "#!/bin/sh\nprintf '%s\\n' \"$$\" > " + shell_quote(str(marker)) +
            "\nsleep 30 &\nprintf '%s\\n' \"$!\" > " + shell_quote(str(child_marker)) + "\nwait\n",
            encoding="utf-8",
        )
        private_config = fixture.root / "blocking-inspection-config"
        command = "/bin/sh " + shell_quote(str(script))
        escaped = command.replace("\\", "\\\\").replace('"', '\\"')
        private_config.write_text(fixture.ssh_config.read_text(encoding="utf-8") +
                                  '\nMatch exec "' + escaped + '"\n', encoding="utf-8")
        directory = fixture.config_directory("multi-quit-inspection", fixture.profile())
        session = ManagerSession(self.app, fixture, directory, ssh_configuration=private_config)
        self.sessions.append(session)
        owned_group = None
        pids = []
        try:
            deadline = time.monotonic() + 8
            while not marker.exists() or not child_marker.exists() or not child_marker.read_text().strip():
                session.assert_alive()
                self.assertLess(time.monotonic(), deadline, session.diagnostics())
                time.sleep(.02)
            pids = [int(marker.read_text().strip()), int(child_marker.read_text().strip())]
            owned_group = os.getpgid(pids[0])
            self.assertNotEqual(owned_group, os.getpgrp(), "Inspection must not share the test driver's group")
            start = time.monotonic()
            session.send("quit")
            self.assertEqual(session.wait_exit(timeout=3), 0, session.diagnostics())
            self.assertLess(time.monotonic() - start, 3)
            self.assertNotIn("connected", [host["state"] for _, host in session.host_events])
            for pid in pids:
                result = subprocess.run(["/bin/ps", "-p", str(pid), "-o", "stat="], capture_output=True, text=True)
                self.assertTrue(result.returncode != 0 or result.stdout.strip().startswith("Z"),
                                f"Owned inspection descendant {pid} remains live: {result.stdout}")
        finally:
            # Cleanup only the group identified from this test's own marker process.
            if owned_group is not None and owned_group != os.getpgrp():
                for pid in pids:
                    try:
                        if os.getpgid(pid) == owned_group:
                            os.killpg(owned_group, signal.SIGKILL)
                            break
                    except ProcessLookupError:
                        pass
            for path in (marker, child_marker, script, private_config):
                path.unlink(missing_ok=True)

    def test_10_proxyjump_requires_target_forwarding_readiness(self):
        fixture = self.fixture
        # A jump ssh -W process emits its own authentication and interactive-session
        # messages on the parent's stderr before the target SSH handshake completes.
        # No inherited RemoteForward is present: a remote-ack gate would mask the bug.
        known_hosts = fixture.root / "proxyjump-known-hosts"
        host_fields = fixture.host_key.with_suffix(".pub").read_text().split()
        known_hosts.write_text(
            f"[127.0.0.1]:{fixture.server_port} {host_fields[0]} {host_fields[1]}\n",
            encoding="utf-8",
        )
        private_config = fixture.root / "proxyjump-config"

        def write_jump_config(target_port):
            private_config.write_text("\n".join([
                "Host jump-fixture target-fixture", f"  User {fixture.user}",
                f"  IdentityFile {ssh_path(fixture.client_key)}",
                f"  UserKnownHostsFile {ssh_path(known_hosts)}", "  GlobalKnownHostsFile none",
                "  StrictHostKeyChecking yes", "  IdentitiesOnly yes", "  BatchMode yes",
                "Host jump-fixture", "  HostName 127.0.0.1", f"  Port {fixture.server_port}",
                "Host target-fixture", "  HostName 127.0.0.1", f"  Port {target_port}",
                "  ProxyJump jump-fixture",
            ]) + "\n", encoding="utf-8")

        def start_jump(name):
            profile = fixture.profile(name=name)
            profile["hosts"][0]["target"] = "target-fixture"
            directory = fixture.config_directory(name, profile)
            session = ManagerSession(self.app, fixture, directory, ssh_configuration=private_config)
            self.sessions.append(session)
            return session, profile

        with socket.socket() as sink:
            sink.bind(("127.0.0.1", 0))
            sink.listen()
            sink.settimeout(.2)
            stop = threading.Event()
            accepted = []

            def accept_without_ssh_banner():
                while not stop.is_set():
                    try:
                        connection, _ = sink.accept()
                        accepted.append(connection)
                    except socket.timeout:
                        continue
                    except OSError:
                        return

            worker = threading.Thread(target=accept_without_ssh_banner, daemon=True)
            worker.start()
            failed = None
            try:
                write_jump_config(sink.getsockname()[1])
                failed, _ = start_jump("multi-proxyjump-blocked")
                trying, _, _ = failed.wait_host(PROFILE_ID, "connecting")
                failed.wait_host(PROFILE_ID, "disconnected", after=trying + 1, timeout=30)
                failed.assert_alive()
                self.assertNotIn("connected", [host["state"] for _, host in failed.host_events], failed.diagnostics())
                self.assertEqual(len(accepted), 1, "Timeout must reach the target once without retry")
            finally:
                if failed:
                    failed.close()
                stop.set()
                worker.join(timeout=1)
                for connection in accepted:
                    connection.close()

        # The same jump configuration must work once the target is an actual SSH
        # server; it must not solve the regression by rejecting ProxyJump entirely.
        write_jump_config(fixture.server_port)
        succeeded, profile = start_jump("multi-proxyjump-success")
        succeeded.wait_host(PROFILE_ID, "connected", timeout=35)
        for rule in profile["hosts"][0]["forwards"]:
            echo(rule["localPort"])
        succeeded.send("quit")
        self.assertEqual(succeeded.wait_exit(timeout=10), 0, succeeded.diagnostics())
        for rule in profile["hosts"][0]["forwards"]:
            wait_port_closed(rule["localPort"])
        self.report["proxyjump"] = "blocked target never connected; authenticated target forwards data"


    def test_11_authentication_and_host_key_errors_do_not_retry(self):
        fixture = self.fixture
        rejected_key = fixture.root / "rejected-client-key"
        run([fixture.keygen, "-q", "-t", "ed25519", "-N", "", "-f", str(rejected_key)])
        for failure in ("authentication", "host-key"):
            with self.subTest(failure=failure):
                config = self.profile()
                directory = fixture.config_directory("multi-" + failure, config)
                private = directory / "ssh_config"
                lines = [line for line in fixture.ssh_config.read_text(encoding="utf-8").splitlines()
                         if not line.strip().startswith(("LocalForward ", "RemoteForward "))]
                contents = "\n".join(lines) + "\n"
                if failure == "authentication":
                    config["hosts"][0]["identityFile"] = str(rejected_key)
                    fixture.write_config(directory, config)
                    contents = contents.replace(ssh_path(fixture.client_key), ssh_path(rejected_key))
                else:
                    empty = directory / "empty-known-hosts"
                    empty.write_text("")
                    contents = contents.replace(ssh_path(fixture.known_hosts), ssh_path(empty))
                private.write_text(contents, encoding="utf-8")
                before = len(fixture.relay.accepted)
                session = ManagerSession(self.app, fixture, directory, private)
                self.sessions.append(session)
                trying, _, _ = session.wait_host("host-0", "connecting")
                session.wait_host("host-0", "disconnected", after=trying + 1)
                time.sleep(2)
                self.assertEqual(len(fixture.relay.accepted) - before, 1, session.diagnostics())
                session.assert_alive()
                self.assertTrue(session.snapshot("host-0")["error"])
                session.close()


    def test_12_native_duplicate_bind_preserves_existing_host(self):
        first, _, config = self.start("multi-native-first")
        first.wait_host("host-0", "connected")
        duplicate = json.loads(json.dumps(config["hosts"][0]))
        duplicate["id"] = "duplicate"
        before = len(first.host_events)
        first.send("temporary", host=duplicate)
        deadline = time.monotonic() + 30
        with first.condition:
            while True:
                events = [host for _, host in first.host_events[before:] if host["id"] == "duplicate"]
                settled = next((host for host in reversed(events)
                                if host["state"] == "connected" or
                                (host["state"] == "disconnected" and host["error"])), None)
                if settled:
                    break
                first.assert_alive()
                self.assertLess(time.monotonic(), deadline, first.diagnostics())
                first.condition.wait(.1)
        self.report["native_duplicate_bind"] = {
            "result": settled["state"], "error": settled["error"],
            "scope": "observed native SSH/OS behavior, not a guaranteed collision rejection"}
        self.assertEqual(first.snapshot("host-0")["state"], "connected")
        # Test delivery only after removing the ambiguous duplicate listener.
        first.send("delete", id="duplicate")
        deadline = time.monotonic() + 5
        while True:
            try:
                echo(config["hosts"][0]["forwards"][0]["localPort"])
                break
            except OSError:
                self.assertLess(time.monotonic(), deadline, first.diagnostics())
                time.sleep(.05)
        first.assert_alive()

    @unittest.skipUnless(WINDOWS, "Windows Job Object query-descendant cleanup")
    def test_13_windows_quit_interrupts_configuration_inspection(self):
        marker = self.fixture.root / "windows-query.pid"
        descendant = self.fixture.root / "windows-query-child.pid"
        quote = lambda value: "'" + str(value).replace("'", "''") + "'"
        body = (
            "$child = Start-Process -FilePath (Join-Path $PSHOME 'powershell.exe') "
            "-ArgumentList '-NoProfile -NonInteractive -Command Start-Sleep -Seconds 90' -PassThru; "
            f"[IO.File]::WriteAllText({quote(marker)}, [string]$PID); "
            f"[IO.File]::WriteAllText({quote(descendant)}, [string]$child.Id); "
            "$child.WaitForExit()"
        )
        encoded = base64.b64encode(body.encode("utf-16-le")).decode("ascii")
        private = self.fixture.root / "windows-blocking-query-config"
        private.write_text(self.fixture.ssh_config.read_text(encoding="utf-8") +
            '\nMatch exec "powershell.exe -NoProfile -NonInteractive -EncodedCommand ' + encoded + '"\n',
            encoding="utf-8")
        directory = self.fixture.config_directory("windows-query-cleanup", self.profile())
        session = ManagerSession(self.app, self.fixture, directory, private)
        self.sessions.append(session)
        from ctypes import wintypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
        kernel.OpenProcess.restype = wintypes.HANDLE
        kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
        kernel.WaitForSingleObject.restype = wintypes.DWORD
        kernel.CloseHandle.argtypes = [wintypes.HANDLE]
        kernel.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
        handles = []
        pids = []
        try:
            deadline = time.monotonic() + 12
            while not all(path.exists() and path.read_text().strip().isdecimal()
                          for path in (marker, descendant)):
                session.assert_alive()
                self.assertLess(time.monotonic(), deadline, session.diagnostics())
                time.sleep(.05)
            pids = [int(path.read_text().strip()) for path in (marker, descendant)]
            for pid in pids:
                handle = kernel.OpenProcess(0x00100001, False, pid)  # SYNCHRONIZE | PROCESS_TERMINATE
                self.assertTrue(handle, f"Cannot observe fixture-owned child {pid}")
                handles.append(handle)
            started = time.monotonic()
            session.send("quit")
            self.assertEqual(session.wait_exit(timeout=5), 0, session.diagnostics())
            self.assertLess(time.monotonic() - started, 5)
            self.assertNotIn("connected", [host["state"] for _, host in session.host_events])
            for handle in handles:
                self.assertEqual(kernel.WaitForSingleObject(handle, 1000), 0,
                                 "Owned SSH configuration descendant remains alive")
        finally:
            for handle in handles:
                # Retained handles identify the fixture process even if a PID is reused.
                if kernel.WaitForSingleObject(handle, 0) == 258:
                    kernel.TerminateProcess(handle, 1)
                kernel.CloseHandle(handle)


    def test_14_connected_with_no_or_all_failed_forwards(self):
        empty = self.profile()
        empty["hosts"][0]["forwards"] = []
        session, _, _ = self.start("multi-no-forwards", empty)
        session.wait_host("host-0", "connected")
        self.assertEqual(session.snapshot("host-0")["forwards"], [])
        session.close()
        with socket.socket() as occupied:
            if WINDOWS:
                occupied.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            occupied.bind(("127.0.0.1", 0))
            occupied.listen()
            config = self.profile()
            host = config["hosts"][0]
            host["exitOnForwardFailure"] = False
            host["forwards"][0]["localPort"] = occupied.getsockname()[1]
            session, _, _ = self.start("multi-all-forwards-failed", config)
            session.wait_host("host-0", "connected")
            state = session.snapshot("host-0")
            self.assertEqual([rule["state"] for rule in state["forwards"]], ["failed"])
            self.assertEqual(state["state"], "connected")
            session.assert_alive()


    def test_15_shared_configuration_validation_corpus(self):
        cases = [
            ("defaults", {}, True),
            ("description-null", {"description": None}, True),
            ("description-empty", {"description": ""}, True),
            ("description-korean", {"description": "개발 DB · 15432 → 5432"}, True),
            ("description-number", {"description": 42}, False),
            ("description-lf", {"description": "first\nsecond"}, False),
            ("description-cr", {"description": "first\rsecond"}, False),
            ("description-nel", {"description": "first\u0085second"}, False),
            ("description-ls", {"description": "first\u2028second"}, False),
            ("description-ps", {"description": "first\u2029second"}, False),
            ("description-nul", {"description": "first\u0000second"}, False),
            ("nullable", {"user": None, "identityFile": None, "port": None,
                          "autoConnect": None, "exitOnForwardFailure": None,
                          "retryOnConnectionRefused": None}, True),
            ("empty-user", {"user": ""}, False),
            ("empty-key", {"identityFile": ""}, False),
            ("spaced-name", {"name": "  서버  "}, True),
            ("blank-name", {"name": " \t\u00a0\u3000"}, False),
            ("unicode-target", {"target": "server\u2009name"}, False),
            ("format-target", {"target": "server\u200bname"}, False),
            ("bidi-user", {"user": "user\u202ename"}, False),
            ("bom-key", {"identityFile": "key\ufeffpath"}, False),
            ("spaced-key", {"identityFile": "키 경로 with spaces"}, True),
            ("port-fraction", {"port": 22.5}, False),
            ("port-integral", {"port": 22.0}, True),
            ("bad-option", {"autoConnect": 1}, False),
            ("false-options", {"autoConnect": False, "exitOnForwardFailure": False,
                               "retryOnConnectionRefused": False}, True),
        ]
        # Optional descriptions remain compatible within v2. Malformed values and
        # unsupported file versions must preserve the original file.
        forward_descriptions = [
            ("missing", {}, True), ("null", {"description": None}, True),
            ("empty", {"description": ""}, True),
            ("korean", {"description": "개발 DB · 읽기 전용"}, True),
            ("number", {"description": 42}, False),
            ("boolean", {"description": True}, False),
            ("object", {"description": {}}, False),
            ("array", {"description": []}, False),
        ]
        for label, character in [("lf", "\n"), ("cr", "\r"), ("nel", "\u0085"),
                                 ("ls", "\u2028"), ("ps", "\u2029"), ("nul", "\u0000")]:
            forward_descriptions.append((label, {"description": "앞" + character + "뒤"}, False))
        for label, changes, accepted in forward_descriptions:
            rule = {"id": "rule", "enabled": True, "localPort": 15432,
                    "remoteHost": "localhost", "remotePort": 5432}
            rule.update(changes)
            cases.append((f"forward-description-{label}", {"forwards": [rule]}, accepted))
        for version in (1, 99):
            cases.append((f"unsupported-version-{version}", {}, False))
        observed = []
        for label, changes, accepted in cases:
            with self.subTest(case=label):
                config = {"version": (int(label.rsplit("-", 1)[1]) if label.startswith("unsupported-version-") else
                                      2.0 if label == "port-integral" else 2),
                          "hosts": [{"id": "corpus", "name": "서버", "target": "server", "forwards": []}]}
                config["hosts"][0].update(changes)
                directory = self.fixture.config_directory("corpus-" + label, config)
                original = (directory / "config.json").read_bytes()
                session = ManagerSession(self.app, self.fixture, directory, self.fixture.ssh_config,
                                         start=label.startswith("unsupported-version-"))
                self.sessions.append(session)
                deadline = time.monotonic() + 8
                with session.condition:
                    while not any(line == "LF_READY" for line in session.output):
                        session.assert_alive()
                        self.assertLess(time.monotonic(), deadline, session.diagnostics())
                        session.condition.wait(.05)
                    errors = [line for line in session.output if line.startswith("LF_ERROR ")]
                self.assertEqual(not errors, accepted, session.diagnostics())
                if accepted:
                    state = session.snapshot("corpus")
                    self.assertEqual(state["state"], "disconnected")
                    self.assertFalse(state["intent"])
                else:
                    self.assertEqual(session.host_events, [])
                self.assertEqual((directory / "config.json").read_bytes(), original)
                session.close()
                observed.append({"case": label, "accepted": accepted})
        self.report["configuration_validation"] = observed


def check_fixture(fixture: Fixture):
    """Independently verify the daemon, relay and echo endpoint without an app build."""
    log_path = fixture.root / "fixture-client.log"
    log = log_path.open("w", encoding="utf-8")
    tunnel = subprocess.Popen([fixture.ssh, "-F", str(fixture.ssh_config), "-v", "-N", "-n", "-T",
                               "-o", "BatchMode=yes", "-o", "ExitOnForwardFailure=yes", "lf-fixture"],
                              stdout=subprocess.DEVNULL, stderr=log)
    try:
        deadline = time.monotonic() + 10
        while True:
            if tunnel.poll() is not None:
                raise RuntimeError(log_path.read_text(encoding="utf-8", errors="replace")[-12000:])
            try:
                echo(fixture.inherited_port)
                echo(fixture.remote_port)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(.1)
        fixture.relay.disconnect(block=False)
        tunnel.wait(timeout=10)
        wait_port_closed(fixture.inherited_port)
        wait_port_closed(fixture.remote_port)
    finally:
        if tunnel.poll() is None:
            tunnel.terminate()
            tunnel.wait(timeout=10)
        log.close()
    return log_path.read_text(encoding="utf-8", errors="replace")[-12000:]


class EvidenceResult(unittest.TextTestResult):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.test_records = []
        self._started = {}
        self._status = {}

    def startTest(self, test):
        self._started[test.id()] = time.monotonic()
        self._status[test.id()] = "running"
        super().startTest(test)

    def addSuccess(self, test):
        self._status[test.id()] = "passed"
        super().addSuccess(test)

    def addFailure(self, test, error):
        self._status[test.id()] = "failed"
        super().addFailure(test, error)

    def addError(self, test, error):
        self._status[test.id()] = "error"
        super().addError(test, error)

    def addSkip(self, test, reason):
        self._status[test.id()] = "skipped"
        super().addSkip(test, reason)

    def stopTest(self, test):
        self.test_records.append({"name": test.id(), "status": self._status[test.id()],
                                  "duration_seconds": time.monotonic() - self._started[test.id()]})
        super().stopTest(test)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--app")
    parser.add_argument("--sshd", default=os.environ.get("LOCALFORWARDER_TEST_SSHD"))
    parser.add_argument("--report", default="dist/integration-evidence.json")
    parser.add_argument("--only", help="Run matching unittest method names, e.g. test_02")
    parser.add_argument("--fixture-check", action="store_true", help="Validate isolated OpenSSH fixture only")
    options = parser.parse_args()
    if not options.fixture_check and not options.app:
        parser.error("--app is required unless --fixture-check is used")
    started = time.monotonic()
    report = {"behavior": "multi-host-v2", "platform": platform.platform(), "architecture": platform.machine(),
              "tests_started": [], "status": "failed", "limitations": [
                  "Headless integration does not verify native window/tray appearance or external terminal UI.",
              ]}
    if WINDOWS:
        report["limitations"].extend([
            "Windows-to-POSIX authentication registration is not exercised by the native Windows sshd fixture.",
            "Physical sleep, LAN/VPN changes, DPI and Explorer restart require separate desktop verification.",
        ])
    fixture = None
    try:
        fixture = Fixture(options.sshd)
        version = run([fixture.ssh, "-V"])
        report["ssh_version"] = (version.stdout + version.stderr).strip()
        report["ssh_path"] = fixture.ssh
        report["ssh_keygen_path"] = fixture.keygen
        report["sshd_path"] = fixture.sshd
        report["fixture_tunnel_diagnostics"] = check_fixture(fixture)
        if options.fixture_check:
            report.update(status="fixture-validated", app_tests_run=False)
            return 0
        test_class = MultiHostSSHTests
        test_class.fixture = fixture
        test_class.app = str(Path(options.app).absolute())
        test_class.report = report
        names = unittest.defaultTestLoader.getTestCaseNames(test_class)
        if options.only:
            names = [name for name in names if options.only in name]
            if not names:
                raise ValueError("--only did not match any tests")
        suite = unittest.TestSuite(test_class(name) for name in names)
        result = unittest.TextTestRunner(verbosity=2, resultclass=EvidenceResult).run(suite)
        report.update(status="passed" if result.wasSuccessful() else "failed",
                      tests_run=result.testsRun,
                      cases=result.test_records,
                      skipped=[{"test": str(test), "reason": reason} for test, reason in result.skipped],
                      failures=[{"test": str(test), "traceback": details} for test, details in result.failures],
                      errors=[{"test": str(test), "traceback": details} for test, details in result.errors])
        if not result.wasSuccessful():
            report["sshd_log"] = fixture.server_diagnostics()
        return 0 if result.wasSuccessful() else 1
    except BaseException:
        report["error"] = traceback.format_exc()
        print(report["error"], file=sys.stderr)
        return 1
    finally:
        if fixture:
            fixture.close()
        report["duration_seconds"] = time.monotonic() - started
        output = Path(options.report)
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps(report, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
        print("Integration report:", output, flush=True)


if __name__ == "__main__":
    raise SystemExit(main())
