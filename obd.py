import logging
import re
import threading
import time
from dataclasses import dataclass, field
from typing import Optional

from parsers import (
    parse_rpm, parse_kpa, parse_coolant,
    parse_throttle, parse_oil_temp_4402, parse_ethanol,
)
from bt_transport import BleTransport
import config

log = logging.getLogger("obd")

_ADAPTER_ERRORS = ("NO DATA", "STOPPED", "UNABLE TO CONNECT", "CAN ERROR", "BUS INIT")


def _response_echo(cmd: str) -> str:
    """Positive-response echo per ISO 14229/SAE J1979: service_id + 0x40,
    followed by the PID/DID requested. "010C" -> "410C", "224402" -> "624402"."""
    mode = int(cmd[0:2], 16)
    return f"{mode + 0x40:02X}{cmd[2:].upper()}"


def _extract_payload(raw: str, cmd: str) -> Optional[str]:
    """Find the response echo anywhere in the hex stream and return the
    data bytes after it. Robust to whatever the adapter prepends (CAN
    header, PCI/length byte, spacing) since it searches for the actual
    marker instead of assuming a fixed strip length."""
    s = re.sub(r'[^0-9A-Fa-f]', '', raw).upper()
    echo = _response_echo(cmd)
    idx = s.find(echo)
    if idx == -1:
        return None
    return s[idx + len(echo):] or None

@dataclass
class GaugeData:
    boost_psi:  Optional[float] = None
    rpm:        Optional[float] = None
    coolant_c:  Optional[float] = None
    oil_temp_c: Optional[float] = None
    throttle:   Optional[float] = None
    ethanol:    Optional[float] = None
    ts:         float = field(default_factory=time.monotonic)

    def rpm_display(self)     -> str: return f"{self.rpm:.0f}"     if self.rpm      is not None else "--"
    def boost_display(self)   -> str: return f"{self.boost_psi:.1f} psi" if self.boost_psi is not None else "--"
    def coolant_display(self) -> str: return f"{self.coolant_c:.0f} °C"  if self.coolant_c is not None else "--"
    def oil_display(self)     -> str: return f"{self.oil_temp_c:.0f} °C" if self.oil_temp_c is not None else "--"


class ObdClient:
    def __init__(self):
        self._transport: Optional[BleTransport] = None
        self._lock = threading.Lock()
        self.connected = False

    def connect(self, address: str = None):
        address = address or config.OBD_BT_ADDRESS
        log.info("connecting to %s (timeout=%.1fs)", address, config.OBD_TIMEOUT)
        t0 = time.monotonic()
        transport = BleTransport()
        try:
            transport.connect(address, timeout=config.OBD_TIMEOUT)
        except Exception as e:
            # Distinguishing "failed instantly" from "timed out waiting the
            # full config.OBD_TIMEOUT" from the elapsed time alone is often
            # enough to tell "adapter out of range/off" apart from "found
            # it but the GATT handshake itself didn't negotiate" without
            # anything else to go on.
            log.warning("BLE connect to %s failed after %.1fs: %s: %s",
                        address, time.monotonic() - t0, type(e).__name__, e)
            raise
        log.debug("BLE connect to %s took %.0fms", address, (time.monotonic() - t0) * 1000)
        self._transport = transport
        if not self._init_elm():
            # A timed-out init command leaves whatever the adapter sends
            # late still sitting unread in the receive queue — every read
            # after that is misaligned with what it's actually replying to
            # (we've seen this over TCP: ATZ/ATE0 time out, then
            # ATL0/ATS0/ATSP0/ATH1 all come back as empty strings — stale
            # bytes, not real acks). Don't limp forward on a desynced
            # stream; close and let the caller retry with a genuinely
            # fresh connection instead.
            self._transport.close()
            self._transport = None
            raise ConnectionError(
                f"ELM327 init failed on {address} — see debug log for which AT command got no response"
            )
        self.connected = True
        log.info("connected to %s", address)

    def disconnect(self):
        # Holds the same lock _send_raw() uses so a disconnect() called from
        # another thread (e.g. a UI "reconnect" button) can't null out
        # self._transport while _send_raw() is mid-flight using it — that
        # raced an AttributeError before this fix.
        with self._lock:
            self.connected = False
            try:
                if self._transport:
                    self._transport.close()
            except Exception:
                pass
            self._transport = None

    def _init_elm(self) -> bool:
        # ATZ triggers a full ELM327 chip reset, which needs real recovery
        # time — the previous 3s default was too tight, causing ATZ (and
        # then ATE0, still waiting on the reboot) to time out with the
        # adapter's real replies arriving late and desyncing everything
        # that followed.
        atz_timeout = max(config.OBD_TIMEOUT, 5.0)
        for cmd, timeout, delay in (
            ("ATZ", atz_timeout, 2.0), ("ATE0", None, 0), ("ATL0", None, 0),
            ("ATS0", None, 0), ("ATSP0", None, 0), ("ATH1", None, 0),
        ):
            resp = self._send_raw(cmd, timeout=timeout)
            log.debug("init %-6s -> %r", cmd, resp)
            if resp is None:
                log.warning("init %s got no response — aborting connect", cmd)
                return False
            if delay:
                time.sleep(delay)
        return True

    def poll(self) -> GaugeData:
        t0 = time.monotonic()
        map_kpa  = self._query("010B", parse_kpa)
        baro_kpa = self._query("0133", parse_kpa)
        boost = ((map_kpa - baro_kpa) * 0.145038) if (map_kpa and baro_kpa) else None
        data = GaugeData(
            boost_psi  = boost,
            rpm        = self._query("010C", parse_rpm),
            coolant_c  = self._query("0105", parse_coolant),
            oil_temp_c = self._query("224402", parse_oil_temp_4402),
            throttle   = self._query("0111", parse_throttle),
            ethanol    = self._query("224010", parse_ethanol),
        )
        log.debug(
            "poll %.0fms boost=%s rpm=%s coolant=%s oil=%s throttle=%s ethanol=%s",
            (time.monotonic() - t0) * 1000,
            data.boost_psi, data.rpm, data.coolant_c,
            data.oil_temp_c, data.throttle, data.ethanol,
        )
        return data

    def _query(self, cmd: str, parser):
        raw = self._send_raw(cmd)
        if raw is None:
            log.warning("PID %s: no response (send/recv failed or timed out)", cmd)
            return None
        payload = _extract_payload(raw, cmd)
        if payload is None:
            if any(err in raw for err in _ADAPTER_ERRORS):
                log.warning("PID %s: adapter/ECU error: %r", cmd, raw.strip())
            else:
                log.warning("PID %s: response missing expected echo %r: %r",
                            cmd, _response_echo(cmd), raw)
            return None
        value = parser(payload)
        if value is None:
            log.warning("PID %s: unparseable payload %r (raw=%r)", cmd, payload, raw)
        return value

    def _send_raw(self, cmd: str, timeout: float = None) -> Optional[str]:
        with self._lock:
            # Captured once under the lock rather than re-read via
            # self._transport throughout — disconnect() (also lock-held)
            # can't swap it to None out from under an in-progress call
            # anymore, but a local reference keeps this method correct even
            # if that invariant ever changes.
            transport = self._transport
            if transport is None:
                return None
            eff_timeout = timeout if timeout is not None else config.OBD_TIMEOUT
            buf = b""
            try:
                payload = (cmd + "\r").encode()
                # Raw bytes (not the decoded/stripped string) so anything a
                # nonstandard adapter prepends or sends non-ASCII — framing
                # bytes, NULs, a proprietary preamble — shows up instead of
                # being silently eaten by decode(errors="ignore").
                log.debug("PID %s: TX %r", cmd, payload)
                t0 = time.monotonic()
                transport.send(payload)
                buf = transport.recv_until(b">", eff_timeout)
                log.debug("PID %s: RX %r (%.0fms)", cmd, buf, (time.monotonic() - t0) * 1000)
                return buf.decode(errors="ignore").strip()
            except Exception as e:
                log.debug("PID %s: BLE error: %r (partial RX before error: %r)", cmd, e, buf)
                # Without this, a dropped connection (adapter reset, out of
                # BLE range) never gets noticed: the poll loop keeps
                # hammering the dead transport at full speed forever
                # instead of backing off and reconnecting.
                self.connected = False
                return None
