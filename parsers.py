"""
Parse OBD-II response payloads. Callers pass in just the hex data bytes
*after* the response echo (e.g. "1AF8" for a "410C" RPM response) — see
obd.py's _extract_payload for how that's isolated from the raw adapter
text (CAN header / PCI length byte / echo / spacing all stripped there).
"""

def parse_rpm(h: str | None) -> float | None:
    if not h or len(h) < 4:
        return None
    try:
        a = int(h[0:2], 16)
        b = int(h[2:4], 16)
        return (a * 256 + b) / 4.0
    except ValueError:
        return None

def parse_kpa(h: str | None) -> float | None:
    if not h:
        return None
    try:
        return float(int(h[0:2], 16))
    except ValueError:
        return None

def parse_coolant(h: str | None) -> float | None:
    if not h:
        return None
    try:
        return float(int(h[0:2], 16) - 40)
    except ValueError:
        return None

def parse_throttle(h: str | None) -> float | None:
    if not h:
        return None
    try:
        return int(h[0:2], 16) * 100.0 / 255.0
    except ValueError:
        return None

def parse_oil_temp_4402(h: str | None) -> float | None:
    # mode 22 DID 0x4402: raw * 191.25 / 255 - 48 = °C
    # Source: github.com/TomWis97/bmw-obd2-display
    if not h:
        return None
    try:
        raw = int(h[0:2], 16)
        return raw * 191.25 / 255.0 - 48.0
    except ValueError:
        return None

def parse_ethanol(h: str | None) -> float | None:
    if not h:
        return None
    try:
        return float(int(h[0:2], 16))
    except ValueError:
        return None
