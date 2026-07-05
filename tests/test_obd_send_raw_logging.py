"""
Unit tests for the wire-level TX/RX byte logging added to obd.py's
_send_raw()/connect() — no real socket or ELM327 emulator needed, just a
fake socket standing in so the exact bytes logged can be asserted on.
See tests/test_obd_emulator.py for the real-protocol integration tests.
"""
import logging
import os
import socket
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

from obd import ObdClient


class _FakeSocket:
    def __init__(self, response: bytes = b"", raise_after: Exception = None):
        self._response = response
        self._raise_after = raise_after
        self.sent = None

    def settimeout(self, t):
        pass

    def sendall(self, data):
        self.sent = data

    def recv(self, n):
        if self._raise_after is not None and not self._response:
            raise self._raise_after
        chunk, self._response = self._response, b""
        return chunk


def test_send_raw_logs_tx_and_rx_bytes(caplog):
    client = ObdClient()
    client._sock = _FakeSocket(b"41 0C 1A F8\r\r>")

    with caplog.at_level(logging.DEBUG, logger="obd"):
        result = client._send_raw("010C")

    # strip() only trims whitespace, not the trailing ">" prompt char — the
    # regex in _extract_payload() is what actually discards non-hex noise.
    assert result == "41 0C 1A F8\r\r>"
    tx_lines = [r.message for r in caplog.records if "TX" in r.message]
    rx_lines = [r.message for r in caplog.records if "RX" in r.message]
    assert any(r"b'010C\r'" in m for m in tx_lines)
    assert any("41 0C 1A F8" in m for m in rx_lines)


def test_send_raw_logs_partial_rx_before_a_socket_error(caplog):
    client = ObdClient()

    class _DropsMidStream(_FakeSocket):
        def recv(self, n):
            if not hasattr(self, "_served"):
                self._served = True
                return b"41 0C"  # partial data, then the connection dies
            raise ConnectionResetError("peer reset")

    client._sock = _DropsMidStream()

    with caplog.at_level(logging.DEBUG, logger="obd"):
        result = client._send_raw("010C")

    assert result is None
    assert client.connected is False
    partial_lines = [r.message for r in caplog.records if "partial RX before error" in r.message]
    assert any("41 0C" in m for m in partial_lines)


def test_send_raw_returns_none_when_socket_already_closed():
    client = ObdClient()
    client._sock = None
    assert client._send_raw("010C") is None


def test_connect_logs_tcp_timing_on_failure(caplog, monkeypatch):
    def raise_timeout(addr, timeout=None):
        raise TimeoutError("timed out")
    monkeypatch.setattr(socket, "create_connection", raise_timeout)

    client = ObdClient()
    with caplog.at_level(logging.DEBUG, logger="obd"):
        try:
            client.connect("10.0.0.5", 35000)
        except TimeoutError:
            pass
        else:
            assert False, "expected TimeoutError to propagate"

    warn_lines = [r.message for r in caplog.records if "TCP connect" in r.message and "failed" in r.message]
    assert any("10.0.0.5:35000" in m and "TimeoutError" in m for m in warn_lines)
