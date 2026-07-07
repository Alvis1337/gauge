"""
Integration tests for main._obd_thread against fake_ble_transport.py's
in-process ELM327 protocol fake — covers the force-reconnect (UI
"Reconnect OBD" button) and auto-discovery fallback paths.

Note: importing main.py runs its module-level setup (gauge_logging.setup(),
settings.load()) as a side effect, same as it would on a real run — it
does not touch any hardware (SPI/GPIO/display) at import time, only
inside main() itself, which this file never calls.
"""
import os
import sys
import threading
import time

import pytest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import main  # noqa: E402
import obd  # noqa: E402
import settings  # noqa: E402
import bt_discovery  # noqa: E402
from obd import ObdClient, GaugeData  # noqa: E402
from fake_ble_transport import FakeBleTransport  # noqa: E402


@pytest.fixture
def fake_transport(monkeypatch):
    monkeypatch.setattr(obd, "BleTransport", FakeBleTransport)


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


def test_obd_thread_connects_and_polls(fake_transport, saved_settings):
    saved_settings["obd_bt_address"] = "AA:BB:CC:11:22:33"

    client = ObdClient()
    data_ref = [GaugeData()]
    stop_evt = threading.Event()
    force_reconnect_evt = threading.Event()
    address_ref = [""]

    t = threading.Thread(
        target=main._obd_thread,
        args=(client, data_ref, stop_evt, address_ref, force_reconnect_evt),
        daemon=True,
    )
    t.start()
    assert _wait_until(lambda: data_ref[0].rpm is not None, timeout=10.0)

    stop_evt.set()
    t.join(timeout=5)
    client.disconnect()


def test_obd_thread_force_reconnect_gets_immediate_response(fake_transport, saved_settings):
    saved_settings["obd_bt_address"] = "AA:BB:CC:11:22:33"

    client = ObdClient()
    data_ref = [GaugeData()]
    stop_evt = threading.Event()
    force_reconnect_evt = threading.Event()
    address_ref = [""]

    t = threading.Thread(
        target=main._obd_thread,
        args=(client, data_ref, stop_evt, address_ref, force_reconnect_evt),
        daemon=True,
    )
    t.start()
    assert _wait_until(lambda: client.connected, timeout=10.0)

    old_transport = client._transport
    force_reconnect_evt.set()
    assert _wait_until(
        lambda: client._transport is not None and client._transport is not old_transport and client.connected,
        timeout=10.0,
    )

    stop_evt.set()
    t.join(timeout=5)
    client.disconnect()


def test_obd_thread_falls_back_to_discovery_after_repeated_failures(fake_transport, saved_settings, monkeypatch):
    # Point the thread at an address the fake transport will refuse...
    saved_settings["obd_bt_address"] = "AA:BB:CC:DE:AD:00"
    monkeypatch.setattr(obd, "BleTransport", lambda: FakeBleTransport(alive=False))

    # ...but make discovery "find" a live address instead, simulating the
    # adapter having moved / a fresh pairing being needed.
    monkeypatch.setattr(bt_discovery, "discover", lambda: "AA:BB:CC:11:22:33")
    monkeypatch.setattr(main, "_DISCOVERY_AFTER_FAILURES", 1)

    client = ObdClient()
    data_ref = [GaugeData()]
    stop_evt = threading.Event()
    force_reconnect_evt = threading.Event()
    address_ref = [""]

    t = threading.Thread(
        target=main._obd_thread,
        args=(client, data_ref, stop_evt, address_ref, force_reconnect_evt),
        daemon=True,
    )
    t.start()

    def _switch_to_live_transport():
        # Once the thread saves the discovered address, flip BleTransport
        # back to the working fake so the next connect attempt succeeds —
        # mirrors the discovered adapter actually being reachable.
        if settings.get("obd_bt_address") == "AA:BB:CC:11:22:33":
            monkeypatch.setattr(obd, "BleTransport", FakeBleTransport)
            return True
        return False

    assert _wait_until(_switch_to_live_transport, timeout=10.0)
    assert _wait_until(lambda: data_ref[0].rpm is not None, timeout=10.0)

    stop_evt.set()
    t.join(timeout=5)

    assert data_ref[0].rpm is not None  # eventually got real data via the discovered address
    assert saved_settings["obd_bt_address"] == "AA:BB:CC:11:22:33"  # discovery's result got persisted
    client.disconnect()
