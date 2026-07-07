"""
End-to-end tests of obd.py's ObdClient against fake_ble_transport.py's
in-process ELM327 protocol fake, standing in for a real BLE adapter.
Covers the ELM327 init handshake, Mode 01 PID decoding, the BMW-specific
Mode 22 UDS DIDs, and the reconnect-after-drop path that matters most for
an adapter that can go out of BLE range mid-drive.
"""
import os
import sys
import threading
import time

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import obd  # noqa: E402
from obd import ObdClient  # noqa: E402
from fake_ble_transport import FakeBleTransport  # noqa: E402


@pytest.fixture
def fake_transport(monkeypatch):
    monkeypatch.setattr(obd, "BleTransport", FakeBleTransport)


def test_connect_runs_full_elm327_init_handshake(fake_transport):
    client = ObdClient()
    client.connect("AA:BB:CC:11:22:33")
    assert client.connected
    client.disconnect()


def test_poll_decodes_standard_and_bmw_uds_pids(fake_transport):
    client = ObdClient()
    client.connect("AA:BB:CC:11:22:33")
    data = client.poll()
    client.disconnect()

    assert data.rpm is not None and data.rpm >= 0
    assert data.coolant_c is not None
    assert data.throttle is not None and 0 <= data.throttle <= 100
    assert data.boost_psi is not None

    # Custom BMW Mode 22 UDS DIDs: fake_ble_transport.py wires these to
    # exact values, so assert the real decode math in parsers.py round-trips.
    assert data.oil_temp_c == pytest.approx(90.0)
    assert data.ethanol == pytest.approx(30.0)


def test_reconnect_after_connection_drop(fake_transport):
    client = ObdClient()
    client.connect("AA:BB:CC:11:22:33")
    assert client.connected

    # Simulate the adapter going out of BLE range mid-poll.
    client._transport.alive = False

    data = client.poll()
    assert client.connected is False
    assert data.rpm is None  # no crash, just empty data — matches _obd_thread's contract

    client.disconnect()
    client.connect("AA:BB:CC:11:22:33")  # a fresh transport reconnects fine
    assert client.connected
    data = client.poll()
    assert data.rpm is not None
    client.disconnect()


def test_concurrent_disconnect_during_poll_does_not_raise(fake_transport):
    # Stress test for a race where disconnect() (e.g. the UI's "Reconnect
    # OBD" button) could null out self._transport while _send_raw() on the
    # OBD poll thread was mid-flight using it, raising AttributeError. GIL
    # scheduling makes this timing-dependent — it's not a guaranteed repro
    # of the old bug, but it does exercise the interleaving that caused it.
    client = ObdClient()
    client.connect("AA:BB:CC:11:22:33")

    errors = []
    stop = threading.Event()

    def _poll_loop():
        try:
            while not stop.is_set():
                client.poll()
        except Exception as e:  # noqa: BLE001 - the whole point is nothing should raise here
            errors.append(e)

    t = threading.Thread(target=_poll_loop, daemon=True)
    t.start()
    for _ in range(20):
        client.disconnect()
        time.sleep(0.01)
    stop.set()
    t.join(timeout=5)

    assert errors == []


def test_connect_to_dead_adapter_raises_cleanly(monkeypatch):
    monkeypatch.setattr(obd, "BleTransport", lambda: FakeBleTransport(alive=False))
    client = ObdClient()
    with pytest.raises(OSError):
        client.connect("AA:BB:CC:DE:AD:00")
    assert client.connected is False
