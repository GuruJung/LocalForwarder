#!/usr/bin/env python3
"""Real OpenSSH integration tests; no third-party Python packages or user SSH files.

The native test entry point runs the application's production session controller.
All server keys, client configuration, profiles and endpoints belong to this run.
An owned TCP relay supplies repeatable connection loss without touching system sshd.
"""

from __future__ import annotations

import argparse
import contextlib
import ctypes
import getpass
import json
import os
from pathlib import Path
import platform
import shutil
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
            if not allowed:
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
        return {"version": 1, "hosts": [host]}

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


class AppSession:
    def __init__(self, app: str, fixture: Fixture, directory: Path):
        self.process = subprocess.Popen(
            [app, "--test-session", str(directory), PROFILE_ID, str(fixture.ssh_config)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8", errors="replace", bufsize=1,
        )
        self.condition = threading.Condition()
        self.events: list[tuple[float, str]] = []
        self.output: list[str] = []
        self.errors: list[str] = []
        self.readers = [threading.Thread(target=self._read, args=(self.process.stdout, False), daemon=True),
                        threading.Thread(target=self._read, args=(self.process.stderr, True), daemon=True)]
        for reader in self.readers:
            reader.start()

    def _read(self, stream, error):
        for line in stream:
            with self.condition:
                (self.errors if error else self.output).append(line.rstrip())
                if len(self.output) > 2000:
                    del self.output[:1000]
                if len(self.errors) > 2000:
                    del self.errors[:1000]
                if line.startswith("LF_STATE "):
                    self.events.append((time.monotonic(), line.split()[1]))
                self.condition.notify_all()
        with self.condition:
            self.condition.notify_all()

    def wait_state(self, state: str, *, after: int = 0, timeout: float = 30):
        deadline = time.monotonic() + timeout
        with self.condition:
            while True:
                for index, event in enumerate(self.events[after:], start=after):
                    if event[1] == state:
                        return index, event[0]
                if self.process.poll() is not None:
                    raise AssertionError(f"App exited {self.process.returncode} before {state}:\n{self.diagnostics()}")
                remaining = deadline - time.monotonic()
                if remaining <= 0:
                    raise AssertionError(f"Timed out waiting for {state}:\n{self.diagnostics()}")
                self.condition.wait(min(remaining, .2))

    def send(self, command: str):
        self.process.stdin.write(command + "\n")
        self.process.stdin.flush()

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


class RealSSHTests(unittest.TestCase):
    fixture: Fixture
    app: str
    report: dict

    def setUp(self):
        self.fixture.relay.allow()
        self.sessions: list[AppSession] = []
        self.report["tests_started"].append(self.id())

    def tearDown(self):
        for session in self.sessions:
            session.close()
        self.fixture.relay.disconnect(block=False)
        wait_port_closed(self.fixture.inherited_port)
        wait_port_closed(self.fixture.remote_port)

    def start(self, name, config=None):
        config = config or self.fixture.profile(name=name)
        directory = self.fixture.config_directory(name, config)
        session = AppSession(self.app, self.fixture, directory)
        self.sessions.append(session)
        return session, directory, config

    def test_01_multiple_forwards_and_60_second_idle(self):
        dead_port = ports(1)[0]
        local = ports(3)
        config = self.fixture.profile(local, target_ports=[self.fixture.echo.port, self.fixture.echo.port, dead_port])
        disabled_port = ports(1)[0]
        config["hosts"][0]["forwards"].append({"id": "disabled", "enabled": False,
            "localPort": disabled_port, "remoteHost": "127.0.0.1", "remotePort": self.fixture.echo.port})
        session, _, _ = self.start("idle", config)
        session.wait_state("connected")
        for port in local[:2] + [self.fixture.inherited_port, self.fixture.remote_port]:
            echo(port)
        wait_port_closed(disabled_port)
        # A destination-side failure must not be confused with tunnel setup failure.
        with socket.create_connection(("127.0.0.1", local[2]), timeout=4) as connection:
            connection.settimeout(4)
            with contextlib.suppress(ConnectionResetError, BrokenPipeError):
                self.assertEqual(connection.recv(1), b"")
        baseline = process_metrics(session.process.pid)
        start = time.monotonic()
        time.sleep(60)
        elapsed = time.monotonic() - start
        session.assert_alive()
        final = process_metrics(session.process.pid)
        self.assertEqual(session.events[-1][1], "connected", session.diagnostics())
        self.report["idle"] = {
            "duration_seconds": elapsed,
            "cpu_seconds": final["cpu_seconds"] - baseline["cpu_seconds"],
            "cpu_percent_one_core": 100 * (final["cpu_seconds"] - baseline["cpu_seconds"]) / elapsed,
            "rss_bytes": final["rss_bytes"],
            "scope": "headless production session controller; excludes SSH child and native UI",
        }
        session.send("quit")
        self.assertEqual(session.wait_exit(timeout=10), 0, session.diagnostics())
        for port in local + [self.fixture.inherited_port, self.fixture.remote_port]:
            wait_port_closed(port)

    def test_02_first_transport_close_is_retried(self):
        relay = self.fixture.relay
        with relay.lock:
            relay.reject_count = 1
            before = len(relay.accepted)
        session, _, _ = self.start("first-close")
        session.wait_state("connected", timeout=35)
        attempts = relay.accepted[before:]
        self.assertEqual(len(attempts), 2, session.diagnostics())
        self.assertGreaterEqual(attempts[1] - attempts[0], .8)
        self.assertLess(attempts[1] - attempts[0], 8)

    def test_03_port_collision_fails_all_forwards(self):
        with socket.socket() as occupied:
            occupied.bind(("127.0.0.1", 0))
            occupied.listen()
            free = ports(1)[0]
            config = self.fixture.profile([free, occupied.getsockname()[1]])
            session, _, _ = self.start("collision", config)
            self.assertNotEqual(session.wait_exit(timeout=35), 0, session.diagnostics())
            self.assertNotIn("connected", [item[1] for item in session.events])
            self.assertTrue(any(line.startswith("LF_ERROR ") for line in session.output), session.diagnostics())
            wait_port_closed(free)
            wait_port_closed(self.fixture.inherited_port)

    def test_04_manual_reconnect_reads_latest_config(self):
        session, directory, config = self.start("manual")
        session.wait_state("connected")
        old_port = config["hosts"][0]["forwards"][0]["localPort"]
        new_port = ports(1)[0]
        config["hosts"][0]["forwards"][0]["localPort"] = new_port
        self.fixture.write_config(directory, config)
        self.fixture.relay.disconnect(block=False)
        index, lost_at = session.wait_state("waitingReconnect")
        session.send("reconnect")
        _, recovered_at = session.wait_state("connected", after=index + 1)
        self.assertLess(recovered_at - lost_at, 20)
        echo(new_port)
        wait_port_closed(old_port)

    def test_05_automatic_reconnect_after_real_60_seconds(self):
        session, directory, config = self.start("auto-success")
        session.wait_state("connected")
        self.fixture.relay.disconnect(block=False)
        index, lost_at = session.wait_state("waitingReconnect")
        old_port = config["hosts"][0]["forwards"][0]["localPort"]
        new_port = next(port for port in ports(2) if port != old_port)
        config["hosts"][0]["forwards"][0]["localPort"] = new_port
        self.fixture.write_config(directory, config)
        reconnect_index, retry_at = session.wait_state("reconnecting", after=index + 1, timeout=80)
        self.assertGreaterEqual(retry_at - lost_at, 58)
        self.assertLess(retry_at - lost_at, 75)
        session.wait_state("connected", after=reconnect_index + 1)
        echo(new_port)
        wait_port_closed(old_port)
        self.report["automatic_reconnect_delay_seconds"] = retry_at - lost_at

    def test_06_failed_automatic_reconnect_then_real_60_second_exit(self):
        session, _, config = self.start("auto-failure")
        session.wait_state("connected")
        self.fixture.relay.disconnect(block=True)
        index, lost_at = session.wait_state("waitingReconnect")
        reconnect_index, retry_at = session.wait_state("reconnecting", after=index + 1, timeout=80)
        self.assertGreaterEqual(retry_at - lost_at, 58)
        waiting_index, failed_at = session.wait_state("waitingExit", after=reconnect_index + 1, timeout=45)
        session.wait_exit(timeout=80)
        exit_events = [item for item in session.events[waiting_index + 1:] if item[1] == "exiting"]
        self.assertTrue(exit_events, session.diagnostics())
        exited_at = exit_events[-1][0]
        self.assertGreaterEqual(exited_at - failed_at, 58)
        self.assertLess(exited_at - failed_at, 75)
        self.assertNotIn("reconnecting", [item[1] for item in session.events[waiting_index + 1:]])
        wait_port_closed(config["hosts"][0]["forwards"][0]["localPort"])
        self.report["failed_reconnect_exit_delay_seconds"] = exited_at - failed_at

    def test_07_quit_cancels_pending_reconnect(self):
        session, _, _ = self.start("cancel")
        session.wait_state("connected")
        self.fixture.relay.disconnect(block=False)
        session.wait_state("waitingReconnect")
        start = time.monotonic()
        session.send("quit")
        self.assertEqual(session.wait_exit(timeout=8), 0, session.diagnostics())
        self.assertLess(time.monotonic() - start, 8)
        self.assertNotIn("reconnecting", [item[1] for item in session.events])

    def test_08_collision_does_not_stop_existing_instance(self):
        first, _, config = self.start("independent-first")
        first.wait_state("connected")
        # OpenSSH enables SO_REUSEADDR. Windows may permit two SSH listeners on
        # the same address/port; POSIX normally rejects them. Observe that native
        # behavior without introducing a wrapper-side port or process precheck.
        duplicate, _, _ = self.start("duplicate-bind-observation", config)
        deadline = time.monotonic() + 45
        while True:
            with duplicate.condition:
                connected = any(state == "connected" for _, state in duplicate.events)
                ended = duplicate.process.poll() is not None
                if connected or ended:
                    break
                self.assertLess(time.monotonic(), deadline, duplicate.diagnostics())
                duplicate.condition.wait(.1)
        if connected:
            self.report["native_duplicate_bind"] = "allowed by native SSH/OS socket binding"
            duplicate.send("quit")
            self.assertEqual(duplicate.wait_exit(timeout=10), 0, duplicate.diagnostics())
        else:
            self.report["native_duplicate_bind"] = "rejected by native SSH/OS socket binding"
            self.assertNotEqual(duplicate.wait_exit(timeout=5), 0, duplicate.diagnostics())
        first.assert_alive()
        echo(config["hosts"][0]["forwards"][0]["localPort"])

        # Force a real bind failure independently of the OS's duplicate-SSH
        # listener semantics, and prove failure cleanup leaves the first alive.
        with socket.socket() as occupied:
            if WINDOWS:
                occupied.setsockopt(socket.SOL_SOCKET, socket.SO_EXCLUSIVEADDRUSE, 1)
            occupied.bind(("127.0.0.1", 0))
            occupied.listen()
            second_config = json.loads(json.dumps(config))
            second_config["hosts"][0]["forwards"].append({
                "id": "exclusive-conflict", "enabled": True,
                "localPort": occupied.getsockname()[1],
                "remoteHost": "127.0.0.1", "remotePort": self.fixture.echo.port,
            })
            second, _, _ = self.start("independent-second", second_config)
            self.assertNotEqual(second.wait_exit(timeout=35), 0, second.diagnostics())
            self.assertNotIn("connected", [state for _, state in second.events])
        first.assert_alive()
        echo(config["hosts"][0]["forwards"][0]["localPort"])

    @unittest.skipIf(WINDOWS, "Registration targets a POSIX server; native Windows sshd is not that environment")
    def test_09_public_key_registration_preserves_and_deduplicates(self):
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
    report = {"platform": platform.platform(), "architecture": platform.machine(),
              "tests_started": [], "status": "failed", "limitations": [
                  "Headless integration does not verify native window/tray appearance or external terminal UI.",
              ]}
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
        RealSSHTests.fixture = fixture
        RealSSHTests.app = str(Path(options.app).absolute())
        RealSSHTests.report = report
        names = unittest.defaultTestLoader.getTestCaseNames(RealSSHTests)
        if options.only:
            names = [name for name in names if options.only in name]
            if not names:
                raise ValueError("--only did not match any tests")
        suite = unittest.TestSuite(RealSSHTests(name) for name in names)
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
