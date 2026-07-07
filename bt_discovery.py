"""
Auto-discovery for BLE (Bluetooth Low Energy) ELM327-compatible OBD-II
adapters — the Bluetooth counterpart to what obd_discovery.py used to do
for WiFi adapters, now that the adapter itself is BLE-only.

BLE devices broadcast their name before any GATT connection is made, so
unlike the WiFi version this can filter candidates up front from the
advertised name alone, without opening a connection to every device in
range. Each candidate discover() considers is still verified with a real
ELM327 ATZ handshake before being reported as found — an unrelated BLE
device (headphones, a phone) whose name happens to match one of the hints
below isn't enough on its own.
"""
import logging
from typing import Optional

from bleak import BleakScanner

from bt_transport import BleTransport, run_coro

log = logging.getLogger("bt_discovery")

# Substrings seen in real ELM327/OBD BLE adapter advertised names —
# matched case-insensitively. Not exhaustive; a device that doesn't match
# any of these can still be picked manually from the full scan list in
# the settings UI.
_NAME_HINTS = ("obd", "elm327", "icar", "vgate", "vlinker", "obdii", "obd2")

SCAN_TIMEOUT = 6.0
VERIFY_TIMEOUT = 5.0


def _looks_like_obd_name(name: Optional[str]) -> bool:
    if not name:
        return False
    lname = name.lower()
    return any(hint in lname for hint in _NAME_HINTS)


def scan(timeout: float = SCAN_TIMEOUT) -> list[tuple[str, str]]:
    """Every nearby BLE device, as (address, display_name) pairs, with
    name-hinted OBD-looking devices sorted first. No GATT connection is
    made here — this is what backs the settings UI's device list, where a
    person's judgment substitutes for verifying every device seen."""
    try:
        devices = run_coro(BleakScanner.discover(timeout=timeout), timeout + 5)
    except Exception:
        log.exception("BLE scan failed")
        return []
    results = [(d.address, d.name or d.address) for d in devices]
    results.sort(key=lambda r: not _looks_like_obd_name(r[1]))
    return results


def verify(address: str, timeout: float = VERIFY_TIMEOUT) -> bool:
    """Open a real GATT connection and confirm an ELM327 ATZ handshake
    actually comes back — the BLE equivalent of the old
    obd_discovery._looks_like_elm327(), so a device that merely has a
    plausible name isn't mistaken for a working adapter."""
    transport = BleTransport()
    try:
        transport.connect(address, timeout=timeout)
        transport.send(b"ATZ\r")
        buf = transport.recv_until(b">", timeout)
        text = buf.decode(errors="ignore").upper()
        found = "ELM327" in text or ">" in text
        if not found:
            log.debug("%s: no ELM327 handshake (raw=%r)", address, buf)
        return found
    except Exception as e:
        log.debug("%s: verify failed: %s: %s", address, type(e).__name__, e)
        return False
    finally:
        transport.close()


def discover(timeout: float = SCAN_TIMEOUT) -> Optional[str]:
    """Scan for nearby BLE devices whose advertised name hints at being an
    OBD adapter, verify each via a real ATZ handshake in order, and return
    the first one that actually replies like an ELM327 — or None.

    This is the automatic fallback _obd_thread reaches for once the
    configured address goes unreachable; the interactive "Scan for
    Bluetooth adapters" list in the settings UI uses scan() instead and
    lets a person pick, since walking a GATT handshake against every
    nearby BLE device unattended is slow and unnecessary when a human is
    right there to judge which device is the right one.
    """
    candidates = [addr for addr, name in scan(timeout) if _looks_like_obd_name(name)]
    for address in candidates:
        log.debug("verifying candidate %s", address)
        if verify(address):
            log.info("discovered OBD adapter at %s", address)
            return address
    log.info("BLE OBD auto-discovery found nothing")
    return None
