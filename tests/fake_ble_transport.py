"""
A pure-Python fake standing in for bt_transport.BleTransport, so obd.py's
ObdClient can be exercised end-to-end (real ELM327 init handshake, Mode 01
PID decoding, BMW UDS DIDs, drop/reconnect) without a real BLE radio or
adapter. Replaces the old subprocess-based ELM327-emulator TCP fixture
(emulator_server.py), which depended on obd.py's since-removed socket
transport.

Test code monkeypatches obd.BleTransport with a factory returning instances
of this class — obd.py's connect() instantiates BleTransport() with no
arguments, so `monkeypatch.setattr(obd, "BleTransport", FakeBleTransport)`
is enough to intercept every connection ObdClient makes.
"""

# raw=0xB8 (184): 184 * 191.25 / 255 - 48 = 90.0 degC  (parsers.parse_oil_temp_4402)
# raw=0x1E (30):  30.0 percent                          (parsers.parse_ethanol)
_AT_OK = {"ATE0", "ATL0", "ATS0", "ATSP0", "ATH1"}
_PID_RESPONSES = {
    "010B": "41 0B 64",       # MAP 100 kPa
    "0133": "41 33 65",       # baro 101 kPa
    "010C": "41 0C 1A F8",    # RPM
    "0105": "41 05 5A",       # coolant
    "0111": "41 11 80",       # throttle
    "224402": "62 44 02 B8",  # BMW oil temp
    "224010": "62 40 10 1E",  # BMW ethanol
}


class FakeBleTransport:
    """Stands in for bt_transport.BleTransport. `alive=False` (or flipping
    .alive mid-test) simulates the adapter going out of range/resetting."""

    def __init__(self, alive: bool = True):
        self.address = None
        self.alive = alive
        self.sent: list[bytes] = []

    def connect(self, address: str, timeout: float = None):
        if not self.alive:
            raise ConnectionError(f"{address}: fake transport is not alive")
        self.address = address

    def send(self, payload: bytes):
        if not self.alive:
            raise ConnectionError("send on dead fake transport")
        self.sent.append(payload)

    def recv_until(self, marker: bytes, timeout: float) -> bytes:
        if not self.alive:
            return b""
        cmd = self.sent[-1].decode().strip()
        if cmd == "ATZ":
            body = "ELM327 v1.5"
        elif cmd in _AT_OK:
            body = "OK"
        else:
            body = _PID_RESPONSES.get(cmd, "NO DATA")
        return f"{body}\r\r>".encode()

    @property
    def connected(self) -> bool:
        return self.alive

    def close(self):
        self.alive = False
