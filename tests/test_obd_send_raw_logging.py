"""
Unit tests for the wire-level TX/RX byte logging added to obd.py's
_send_raw()/connect() — no real BLE adapter needed, just a fake transport
standing in so the exact bytes logged can be asserted on.
See tests/test_obd_ble_integration.py for the full-protocol integration tests.
"""
import logging
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import obd  # noqa: E402
from obd import ObdClient  # noqa: E402


class _FakeTransport:
    def __init__(self, response: bytes = b""):
        self._response = response
        self.sent = None

    def send(self, data):
        self.sent = data

    def recv_until(self, marker, timeout):
        chunk, self._response = self._response, b""
        return chunk


def test_send_raw_logs_tx_and_rx_bytes(caplog):
    client = ObdClient()
    client._transport = _FakeTransport(b"41 0C 1A F8\r\r>")

    with caplog.at_level(logging.DEBUG, logger="obd"):
        result = client._send_raw("010C")

    # strip() only trims whitespace, not the trailing ">" prompt char — the
    # regex in _extract_payload() is what actually discards non-hex noise.
    assert result == "41 0C 1A F8\r\r>"
    tx_lines = [r.message for r in caplog.records if "TX" in r.message]
    rx_lines = [r.message for r in caplog.records if "RX" in r.message]
    assert any(r"b'010C\r'" in m for m in tx_lines)
    assert any("41 0C 1A F8" in m for m in rx_lines)


def test_send_raw_logs_error_when_transport_send_fails(caplog):
    client = ObdClient()

    class _FailsOnSend(_FakeTransport):
        def send(self, data):
            raise ConnectionResetError("peer reset")

    client._transport = _FailsOnSend()

    with caplog.at_level(logging.DEBUG, logger="obd"):
        result = client._send_raw("010C")

    assert result is None
    assert client.connected is False
    error_lines = [r.message for r in caplog.records if "BLE error" in r.message]
    assert any("ConnectionResetError" in m for m in error_lines)


def test_send_raw_returns_none_when_transport_already_closed():
    client = ObdClient()
    client._transport = None
    assert client._send_raw("010C") is None


def test_connect_logs_ble_timing_on_failure(caplog, monkeypatch):
    class _RaisesOnConnect:
        def connect(self, address, timeout=None):
            raise TimeoutError("timed out")

    monkeypatch.setattr(obd, "BleTransport", _RaisesOnConnect)

    client = ObdClient()
    with caplog.at_level(logging.DEBUG, logger="obd"):
        try:
            client.connect("AA:BB:CC:11:22:33")
        except TimeoutError:
            pass
        else:
            assert False, "expected TimeoutError to propagate"

    warn_lines = [r.message for r in caplog.records if "BLE connect" in r.message and "failed" in r.message]
    assert any("AA:BB:CC:11:22:33" in m and "TimeoutError" in m for m in warn_lines)
