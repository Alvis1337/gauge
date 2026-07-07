"""
On-demand Bluetooth diagnostics, captured automatically whenever the OBD
adapter fails to connect over BLE.

The point is to have enough evidence sitting in the session log after a
failed test drive to tell apart "the Pi's BT radio is off/blocked" or "the
adapter isn't in range/isn't powered" (nothing to connect to at all) from
"the adapter is right there advertising but the GATT handshake itself
didn't negotiate" (a protocol-level problem) — without needing to have
been watching live.
"""
import logging
import subprocess

from bleak import BleakScanner

from bt_transport import run_coro

log = logging.getLogger("netdiag")


def adapter_powered() -> str:
    """The Pi's own BT radio power state, as bluetoothctl sees it right now."""
    try:
        out = subprocess.run(
            ["bluetoothctl", "show"], timeout=5, capture_output=True, text=True, check=False,
        ).stdout
        for line in out.splitlines():
            line = line.strip()
            if line.startswith("Powered:"):
                return line
        return "<Powered: unknown — bluetoothctl output unrecognized>"
    except Exception as e:
        return f"<bluetoothctl failed: {e!r}>"


def bt_powered() -> bool:
    """Plain bool version of adapter_powered(), for the status bar."""
    return adapter_powered().strip() == "Powered: yes"


def wifi_connected() -> bool:
    """Whether NetworkManager currently has an active WiFi connection."""
    try:
        out = subprocess.run(
            ["nmcli", "-t", "-f", "ACTIVE,SSID", "dev", "wifi"],
            timeout=5, capture_output=True, text=True, check=False,
        ).stdout
        return any(line.startswith("yes:") for line in out.splitlines())
    except Exception as e:
        log.debug("wifi_connected() check failed: %s: %s", type(e).__name__, e)
        return False


def device_visible(address: str, timeout: float = 5.0) -> bool:
    """A quick targeted BLE scan for the configured adapter's address —
    tells apart "adapter is off/out of range/not advertising" from "it's
    right there but the GATT handshake itself didn't negotiate", the same
    role the old WiFi-association + ping checks played for the TCP path."""
    try:
        device = run_coro(BleakScanner.find_device_by_address(address, timeout=timeout), timeout + 5)
        return device is not None
    except Exception as e:
        log.debug("device_visible(%s) scan failed: %s: %s", address, type(e).__name__, e)
        return False


def snapshot(address: str) -> str:
    """One line summarizing BT adapter power state + whether the
    configured device is currently advertising, logged at WARNING so it
    survives a session log at default verbosity."""
    power = adapter_powered()
    visible = device_visible(address)
    return f"{power} device({address})={'visible' if visible else 'NOT SEEN'}"
