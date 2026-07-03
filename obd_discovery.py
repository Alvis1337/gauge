"""
Auto-discovery for WiFi OBD-II (ELM327-compatible) adapters.

Most cheap WiFi ELM327/MHD-style adapters run their own DHCP AP and listen
on a small, well-known set of TCP ports at either their own gateway
address (they *are* the router once you're on their WiFi) or one of a
handful of fixed factory-default IPs. discover() probes those candidates
with a real ELM327 ATZ handshake — not just "is the port open" — so some
unrelated device happening to have an open port on the LAN can't produce
a false positive.
"""
import logging
import socket
import subprocess
import time
from concurrent.futures import ThreadPoolExecutor, as_completed

log = logging.getLogger("obd_discovery")

# Fixed factory-default IPs used by common WiFi ELM327/MHD-style adapters.
KNOWN_HOSTS = ["192.168.0.10", "192.168.4.1", "192.168.1.10"]
# Common ports these adapters listen on for a raw ELM327 AT-command socket.
KNOWN_PORTS = [35000, 23, 6801, 2000, 3333]

PROBE_TIMEOUT = 1.0


def _default_gateway() -> str | None:
    """Most of these adapters act as their own AP/router, so the current
    default gateway is usually the adapter itself when connected to its
    WiFi — try it first since it's the most likely hit."""
    try:
        out = subprocess.check_output(
            ["ip", "-4", "route", "show", "default"],
            timeout=3, text=True, stderr=subprocess.DEVNULL,
        )
        # e.g. "default via 192.168.4.1 dev wlan0 proto dhcp ..."
        parts = out.split()
        if "via" in parts:
            return parts[parts.index("via") + 1]
    except Exception:
        log.debug("could not determine default gateway", exc_info=True)
    return None


def _looks_like_elm327(host: str, port: int, timeout: float = PROBE_TIMEOUT) -> bool:
    """Open a raw socket and check for a real ELM327 ATZ handshake."""
    try:
        with socket.create_connection((host, port), timeout=timeout) as sock:
            sock.settimeout(timeout)
            sock.sendall(b"ATZ\r")
            buf = b""
            deadline = time.monotonic() + timeout
            while b">" not in buf and time.monotonic() < deadline:
                chunk = sock.recv(256)
                if not chunk:
                    break
                buf += chunk
            text = buf.decode(errors="ignore").upper()
            return "ELM327" in text or ">" in text
    except OSError:
        return False


def _probe_host(host: str, ports: list[int], timeout: float) -> int | None:
    """Probe every candidate port on one host in parallel; bounded to
    roughly `timeout` seconds regardless of how many ports are checked."""
    with ThreadPoolExecutor(max_workers=len(ports)) as pool:
        futures = {pool.submit(_looks_like_elm327, host, p, timeout): p for p in ports}
        for future in as_completed(futures):
            if future.result():
                return futures[future]
    return None


def discover(candidate_ports: list[int] = None, timeout: float = PROBE_TIMEOUT) -> tuple[str, int] | None:
    """Probe likely host:port combinations for a live ELM327 adapter.

    Tries the current default gateway first (most likely to be the
    adapter itself), then falls back to known factory-default IPs.
    Returns (host, port) of the first responder, or None if nothing
    answered like a real ELM327 adapter.
    """
    ports = candidate_ports or KNOWN_PORTS
    gateway = _default_gateway()

    hosts = []
    if gateway:
        hosts.append(gateway)
    hosts += [h for h in KNOWN_HOSTS if h != gateway]

    for host in hosts:
        log.debug("probing %s on ports %s", host, ports)
        port = _probe_host(host, ports, timeout)
        if port is not None:
            log.info("discovered OBD adapter at %s:%s", host, port)
            return host, port

    log.info("OBD auto-discovery found nothing")
    return None
