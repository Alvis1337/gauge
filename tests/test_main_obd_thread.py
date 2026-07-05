"""
Integration tests for main._obd_thread against the ELM327 emulator —
covers the force-reconnect (UI "Reconnect OBD" button) and auto-discovery
fallback paths, which are new and not exercised anywhere else.

Note: importing main.py runs its module-level setup (gauge_logging.setup(),
settings.load()) as a side effect, same as it would on a real run — it
does not touch any hardware (SPI/GPIO/display) at import time, only
inside main() itself, which this file never calls.

Requires the ELM327-emulator package in .venv-test — see emulator_server.py
docstring for setup. Skipped automatically if that venv isn't present.
"""
import os
import subprocess
import sys
import threading
import time

import pytest

_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
_VENV_PY = os.path.join(_ROOT, ".venv-test", "bin", "python3")
_SERVER = os.path.join(_ROOT, "tests", "emulator_server.py")

pytestmark = pytest.mark.skipif(
    not os.path.exists(_VENV_PY),
    reason="ELM327-emulator not installed — see emulator_server.py docstring",
)

sys.path.insert(0, _ROOT)
from tests.test_obd_emulator import _free_port, _wait_for_port  # noqa: E402

import main  # noqa: E402
import settings  # noqa: E402
import obd_discovery  # noqa: E402
from obd import ObdClient, GaugeData  # noqa: E402


@pytest.fixture
def emulator():
    port = _free_port()
    proc = subprocess.Popen(
        [_VENV_PY, _SERVER, "-n", str(port)],
        stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True,
    )
    try:
        _wait_for_port(port)
        yield port
    finally:
        proc.terminate()
        try:
            proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            proc.kill()


@pytest.fixture
def saved_settings(monkeypatch):
    """Isolates settings.py's module-level _data from the real settings.json."""
    fake_data = {}
    monkeypatch.setattr(settings, "_data", fake_data)
    monkeypatch.setattr(settings, "save", lambda: None)
    return fake_data


def _wait_until(predicate, timeout=10.0, interval=0.1):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return predicate()


def test_obd_thread_connects_and_polls(emulator, saved_settings):
    # obd.py's ELM327 init deliberately waits out a ~2.6s ATZ chip-reset
    # delay (see obd.py's _init_elm) — the emulator reproduces that timing
    # faithfully, so this needs a real wait, not a fixed short sleep.
    saved_settings["obd_host"] = "127.0.0.1"
    saved_settings["obd_port"] = emulator

    client = ObdClient()
    data_ref = [GaugeData()]
    stop_evt = threading.Event()
    force_reconnect_evt = threading.Event()
    host_ref = [""]

    t = threading.Thread(
        target=main._obd_thread,
        args=(client, data_ref, stop_evt, host_ref, force_reconnect_evt),
        daemon=True,
    )
    t.start()
    assert _wait_until(lambda: data_ref[0].rpm is not None, timeout=10.0)

    stop_evt.set()
    t.join(timeout=5)
    client.disconnect()


def test_obd_thread_force_reconnect_gets_immediate_response(emulator, saved_settings):
    saved_settings["obd_host"] = "127.0.0.1"
    saved_settings["obd_port"] = emulator

    client = ObdClient()
    data_ref = [GaugeData()]
    stop_evt = threading.Event()
    force_reconnect_evt = threading.Event()
    host_ref = [""]

    t = threading.Thread(
        target=main._obd_thread,
        args=(client, data_ref, stop_evt, host_ref, force_reconnect_evt),
        daemon=True,
    )
    t.start()
    assert _wait_until(lambda: client.connected, timeout=10.0)

    old_sock = client._sock
    force_reconnect_evt.set()
    # client._sock goes non-None again as soon as the raw TCP connect for
    # the new attempt succeeds, well before client.connected flips True at
    # the end of the ELM327 init handshake — wait for the real signal
    # (connected, post a fresh socket) rather than the earlier one.
    assert _wait_until(
        lambda: client._sock is not None and client._sock is not old_sock and client.connected,
        timeout=10.0,
    )

    stop_evt.set()
    t.join(timeout=5)
    client.disconnect()


def test_obd_thread_falls_back_to_discovery_after_repeated_failures(emulator, saved_settings, monkeypatch):
    # Point the thread at a host nothing is listening on...
    dead_port = _free_port()
    saved_settings["obd_host"] = "127.0.0.1"
    saved_settings["obd_port"] = dead_port

    # ...but make discovery "find" the real emulator instead, simulating
    # the adapter having moved to a different IP/port than configured.
    monkeypatch.setattr(obd_discovery, "discover", lambda: ("127.0.0.1", emulator))
    monkeypatch.setattr(main, "_DISCOVERY_AFTER_FAILURES", 1)

    client = ObdClient()
    data_ref = [GaugeData()]
    stop_evt = threading.Event()
    force_reconnect_evt = threading.Event()
    host_ref = [""]

    t = threading.Thread(
        target=main._obd_thread,
        args=(client, data_ref, stop_evt, host_ref, force_reconnect_evt),
        daemon=True,
    )
    t.start()
    deadline = time.monotonic() + 10.0
    while data_ref[0].rpm is None and time.monotonic() < deadline:
        time.sleep(0.1)
    stop_evt.set()
    t.join(timeout=5)

    assert data_ref[0].rpm is not None  # eventually got real data via the discovered host
    assert saved_settings["obd_host"] == "127.0.0.1"
    assert saved_settings["obd_port"] == emulator  # discovery's result got persisted
    client.disconnect()
