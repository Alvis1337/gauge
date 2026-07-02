"""
End-to-end tests of obd.py's ObdClient against a real ELM327 protocol
emulator (see emulator_server.py) instead of a real vehicle/WiFi adapter.
Covers the ELM327 init handshake, Mode 01 PID decoding, the BMW-specific
Mode 22 UDS DIDs, and the reconnect-after-drop path that matters most for
a flaky WiFi OBD adapter in a moving car.

Requires the ELM327-emulator package in .venv-test — see emulator_server.py
docstring for setup. Skipped automatically if that venv isn't present.
"""
import os
import socket
import subprocess
import sys
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
from obd import ObdClient  # noqa: E402


def _free_port() -> int:
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _wait_for_port(port: int, timeout: float = 10.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.5):
                return
        except OSError:
            time.sleep(0.1)
    raise TimeoutError(f"emulator never opened port {port}")


@pytest.fixture
def emulator():
    port = _free_port()
    # stdin=PIPE (not inherited/closed) keeps the emulator's interactive
    # cmd loop blocked on read() instead of hitting EOF and exiting —
    # daemon mode (-d) works too but forking makes cleanup/PID tracking
    # awkward for a test fixture.
    proc = subprocess.Popen(
        [_VENV_PY, _SERVER, "-n", str(port)],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
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


def test_connect_runs_full_elm327_init_handshake(emulator):
    client = ObdClient()
    client.connect("127.0.0.1", emulator)
    assert client.connected
    client.disconnect()


def test_poll_decodes_standard_and_bmw_uds_pids(emulator):
    client = ObdClient()
    client.connect("127.0.0.1", emulator)
    data = client.poll()
    client.disconnect()

    # Mode 01 PIDs: exact values come from the emulator's built-in 'car'
    # scenario (cycling fixture data), so just assert they decoded to
    # plausible numbers instead of None.
    assert data.rpm is not None and data.rpm >= 0
    assert data.coolant_c is not None
    assert data.throttle is not None and 0 <= data.throttle <= 100
    assert data.boost_psi is not None

    # Custom BMW Mode 22 UDS DIDs: emulator_server.py wires these to exact
    # values, so assert the real decode math in parsers.py round-trips.
    assert data.oil_temp_c == pytest.approx(90.0)
    assert data.ethanol == pytest.approx(30.0)


def test_reconnect_after_connection_drop(emulator):
    client = ObdClient()
    client.connect("127.0.0.1", emulator)
    assert client.connected

    # Simulate a WiFi/adapter blip: sever the socket out from under the client.
    client._sock.shutdown(socket.SHUT_RDWR)
    client._sock.close()

    data = client.poll()
    assert client.connected is False
    assert data.rpm is None  # no crash, just empty data — matches _obd_thread's contract

    client.disconnect()
    client.connect("127.0.0.1", emulator)  # emulator is still alive; only our socket died
    assert client.connected
    data = client.poll()
    assert data.rpm is not None
    client.disconnect()


def test_connect_to_dead_port_raises_cleanly():
    port = _free_port()  # nothing listening here
    client = ObdClient()
    with pytest.raises(OSError):
        client.connect("127.0.0.1", port)
    assert client.connected is False
