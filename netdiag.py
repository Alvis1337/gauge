"""
On-demand network diagnostics, captured automatically whenever the OBD
adapter fails to connect.

The point is to have enough evidence sitting in the session log after a
failed test drive to tell apart "Pi never reached the adapter at the
network layer" (wrong network, adapter not up yet, wrong IP) from "Pi
reached it fine but the ELM327 handshake itself didn't negotiate" (a
protocol-level problem) — without needing to have been watching live.
"""
import logging
import subprocess

log = logging.getLogger("netdiag")


def _run(cmd: list, timeout: float) -> str:
    try:
        return subprocess.run(
            cmd, timeout=timeout, capture_output=True, text=True, check=False,
        ).stdout.strip()
    except Exception as e:
        return f"<{cmd[0]} failed: {e!r}>"


def current_wifi() -> str:
    """Active SSID + signal strength, as nmcli sees it right now."""
    out = _run(["nmcli", "-t", "-f", "active,ssid,signal", "dev", "wifi"], timeout=5)
    if out.startswith("<"):  # _run's own failure sentinel
        return out
    for line in out.splitlines():
        if line.startswith("yes:"):
            return line
    return "<not associated to any WiFi>"


def ping(host: str, count: int = 1, timeout: float = 2.0) -> tuple:
    """A reachability check independent of the OBD TCP port itself — tells
    us whether the Pi can reach the host at the IP layer at all, which
    rules network-layer problems in or out before blaming the adapter's
    ELM327 protocol handling."""
    out = _run(["ping", "-c", str(count), "-W", str(max(1, int(timeout))), host],
               timeout=timeout + 2)
    # A leading space matters: "100% packet loss" contains "0% packet
    # loss" as a substring, which would otherwise misreport total loss as
    # success.
    reachable = " 0% packet loss" in out
    return reachable, out


def snapshot(host: str) -> str:
    """One line summarizing WiFi association + host reachability, logged
    at WARNING so it survives a session log at default verbosity."""
    wifi = current_wifi()
    reachable, ping_out = ping(host)
    last_line = ping_out.splitlines()[-1] if ping_out else ""
    return (f"wifi={wifi!r} ping({host})={'reachable' if reachable else 'NO REPLY'} "
            f"({last_line!r})")
