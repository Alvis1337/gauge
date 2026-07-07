"""
Unit tests for bt_discovery.py — all BLE scanning/GATT calls are mocked so
this runs fast and deterministically without any real Bluetooth hardware.
"""
import asyncio
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from bleak import BleakScanner

import bt_discovery
from fake_ble_transport import FakeBleTransport


class _FakeDevice:
    def __init__(self, address, name):
        self.address = address
        self.name = name


def _run_sync(coro, timeout):
    # Stand-in for bt_transport.run_coro that just runs the coroutine on a
    # plain asyncio.run() — no need for the real background event-loop
    # thread when the coroutine itself is a test fake with no real I/O.
    return asyncio.run(coro)


def test_looks_like_obd_name_matches_known_hints():
    assert bt_discovery._looks_like_obd_name("OBDII") is True
    assert bt_discovery._looks_like_obd_name("Vgate iCar Pro") is True
    assert bt_discovery._looks_like_obd_name("ELM327-BLE") is True
    assert bt_discovery._looks_like_obd_name("HC-05") is False
    assert bt_discovery._looks_like_obd_name(None) is False


def test_scan_sorts_obd_looking_names_first(monkeypatch):
    async def fake_discover(timeout=None):
        return [_FakeDevice("11:22:33:44:55:66", "HC-05"),
                _FakeDevice("AA:BB:CC:11:22:33", "OBDII")]

    monkeypatch.setattr(bt_discovery, "run_coro", _run_sync)
    monkeypatch.setattr(BleakScanner, "discover", fake_discover)

    results = bt_discovery.scan()
    assert results == [("AA:BB:CC:11:22:33", "OBDII"), ("11:22:33:44:55:66", "HC-05")]


def test_scan_falls_back_to_address_when_name_missing(monkeypatch):
    async def fake_discover(timeout=None):
        return [_FakeDevice("11:22:33:44:55:66", None)]

    monkeypatch.setattr(bt_discovery, "run_coro", _run_sync)
    monkeypatch.setattr(BleakScanner, "discover", fake_discover)

    assert bt_discovery.scan() == [("11:22:33:44:55:66", "11:22:33:44:55:66")]


def test_scan_returns_empty_list_on_failure(monkeypatch):
    def raise_error(coro, timeout):
        coro.close()
        raise RuntimeError("adapter not powered")

    monkeypatch.setattr(bt_discovery, "run_coro", raise_error)
    assert bt_discovery.scan() == []


def test_verify_true_on_real_elm327_handshake(monkeypatch):
    monkeypatch.setattr(bt_discovery, "BleTransport", FakeBleTransport)
    assert bt_discovery.verify("AA:BB:CC:11:22:33") is True


def test_verify_false_when_transport_cannot_connect(monkeypatch):
    monkeypatch.setattr(bt_discovery, "BleTransport", lambda: FakeBleTransport(alive=False))
    assert bt_discovery.verify("AA:BB:CC:DE:AD:00") is False


def test_discover_verifies_only_name_hinted_candidates_in_order(monkeypatch):
    monkeypatch.setattr(
        bt_discovery, "scan",
        lambda timeout=bt_discovery.SCAN_TIMEOUT: [
            ("11:22:33:44:55:66", "HC-05"),          # no name hint, skipped
            ("AA:BB:CC:11:22:33", "OBDII"),           # hinted, verified first
            ("AA:BB:CC:99:88:77", "Vgate iCar Pro"),  # hinted, but never reached
        ],
    )
    checked = []

    def fake_verify(address, timeout=bt_discovery.VERIFY_TIMEOUT):
        checked.append(address)
        return address == "AA:BB:CC:11:22:33"

    monkeypatch.setattr(bt_discovery, "verify", fake_verify)

    assert bt_discovery.discover() == "AA:BB:CC:11:22:33"
    assert checked == ["AA:BB:CC:11:22:33"]  # stopped at the first verified hit


def test_discover_returns_none_when_nothing_verifies(monkeypatch):
    monkeypatch.setattr(
        bt_discovery, "scan",
        lambda timeout=bt_discovery.SCAN_TIMEOUT: [("AA:BB:CC:11:22:33", "OBDII")],
    )
    monkeypatch.setattr(bt_discovery, "verify", lambda address, timeout=bt_discovery.VERIFY_TIMEOUT: False)
    assert bt_discovery.discover() is None


def test_discover_returns_none_when_scan_finds_nothing(monkeypatch):
    monkeypatch.setattr(bt_discovery, "scan", lambda timeout=bt_discovery.SCAN_TIMEOUT: [])
    assert bt_discovery.discover() is None
