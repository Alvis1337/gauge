"""
Auto-discovery for WiFi OBD-II (ELM327-compatible) adapters.

Most cheap WiFi ELM327/MHD-style adapters run their own DHCP AP and listen
on a small, well-known set of TCP ports at either their own gateway
address (they *are* the router once you're on their WiFi) or one of a
handful of fixed factory-default IPs. Those two fast checks come first.

But plenty of real-world setups don't match that shape — the adapter can
be a client on an existing WiFi network rather than an AP of its own, or
just use a non-default IP — so if nothing answers there, discover() falls
back to scanning every host on the subnet(s) the Pi actually has a DHCP
lease on (read from `ip addr`, not assumed), rather than giving up. Every
candidate is verified with a real ELM327 ATZ handshake — not just "is the
port open" — so some unrelated device with an open port on the LAN can't
produce a false positive.
"""
import ipaddress
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

# A real ELM327 chip needs real recovery time after ATZ triggers a full
# reset — obd.py's own _init_elm() waits up to 5s for exactly this reason
# (a shorter default here used to spuriously report a genuine, reachable
# adapter as "not found" during a real test drive: it never had time to
# reply before we gave up on it).
PROBE_TIMEOUT = 5.0
# Subnet scanning hits many more hosts than the fast-path checks, so each
# probe uses a shorter timeout than PROBE_TIMEOUT — but still long enough
# to catch a real ELM327's ATZ reset delay in the common case, not just
# the instrumented emulator's near-instant replies.
SCAN_TIMEOUT = 2.0
SCAN_WORKERS = 256
# A misconfigured/bridged interface could hand us something huge (e.g. a
# /8) — cap how many hosts we're willing to sweep so a single discover()
# call can't hang for minutes.
MAX_SCAN_HOSTS = 512


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
            found = "ELM327" in text or ">" in text
            if not found:
                # DEBUG-only — a full subnet sweep hits hundreds of dead
                # hosts, but this is exactly the evidence needed to tell
                # "wrong port" (TCP connected, garbage/no reply) apart from
                # "nothing there" (connection refused/unreachable) after
                # the fact, for whichever host actually answered at all.
                log.debug("%s:%s: no ELM327 handshake (raw=%r)", host, port, buf)
            return found
    except OSError as e:
        log.debug("%s:%s: probe failed: %s: %s", host, port, type(e).__name__, e)
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


def _own_subnets() -> list["ipaddress.IPv4Network"]:
    """Every IPv4 subnet the Pi currently holds a DHCP lease on. The adapter
    doesn't have to be the gateway or a factory-default IP — this is what
    lets discovery find it anywhere on whatever network the Pi joined,
    instead of only the handful of shapes KNOWN_HOSTS anticipates."""
    nets: list[ipaddress.IPv4Network] = []
    try:
        out = subprocess.check_output(
            ["ip", "-4", "-o", "addr", "show", "scope", "global"],
            timeout=3, text=True, stderr=subprocess.DEVNULL,
        )
    except Exception:
        log.debug("could not enumerate local interfaces/subnets", exc_info=True)
        return nets

    for line in out.splitlines():
        # e.g. "3: wlan0    inet 192.168.4.23/24 brd 192.168.4.255 scope global ..."
        parts = line.split()
        if "inet" not in parts:
            continue
        cidr = parts[parts.index("inet") + 1]
        try:
            net = ipaddress.ip_interface(cidr).network
        except ValueError:
            continue
        if net.is_loopback or net.is_link_local:
            continue
        nets.append(net)
    return nets


def _scan_subnet(network: "ipaddress.IPv4Network", ports: list[int],
                  timeout: float = SCAN_TIMEOUT) -> tuple[str, int] | None:
    """Sweep every host on `network` across every candidate port, in
    parallel, and return the first one that answers like a real ELM327
    adapter. This is the broad, slow fallback — only reached once the
    fast-path gateway/known-host checks have already come up empty."""
    hosts = [str(h) for h in network.hosts()]
    if len(hosts) > MAX_SCAN_HOSTS:
        log.warning("subnet %s has %d hosts, skipping full scan (cap is %d)",
                    network, len(hosts), MAX_SCAN_HOSTS)
        return None

    log.info("scanning subnet %s (%d hosts x %d ports) for OBD adapter",
              network, len(hosts), len(ports))
    pool = ThreadPoolExecutor(max_workers=SCAN_WORKERS)
    try:
        futures = {pool.submit(_looks_like_elm327, h, p, timeout): (h, p)
                   for h in hosts for p in ports}
        for future in as_completed(futures):
            if future.result():
                return futures[future]
    finally:
        # Don't block returning a hit on the hundreds of other in-flight
        # probes — let them finish in the background and get GC'd.
        pool.shutdown(wait=False, cancel_futures=True)
    return None


def discover(candidate_ports: list[int] = None, timeout: float = PROBE_TIMEOUT) -> tuple[str, int] | None:
    """Probe likely host:port combinations for a live ELM327 adapter.

    Fast path: the current default gateway (most likely to be the adapter
    itself) and a handful of known factory-default IPs. If neither hits,
    falls back to scanning every host on the subnet(s) the Pi actually has
    a DHCP lease on. Returns (host, port) of the first responder, or None
    if nothing answered like a real ELM327 adapter.
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

    for net in _own_subnets():
        found = _scan_subnet(net, ports)
        if found is not None:
            host, port = found
            log.info("discovered OBD adapter at %s:%s via subnet scan of %s", host, port, net)
            return host, port

    log.info("OBD auto-discovery found nothing")
    return None
