"""
Unit tests for netdiag.py — all subprocess calls are mocked so this runs
fast and deterministically without touching the real network.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import netdiag


def test_current_wifi_returns_the_active_line(monkeypatch):
    def fake_run(cmd, **kwargs):
        class R:
            stdout = "no:HomeNet:60\nyes:MHD ENET C084:75\n"
        return R()
    monkeypatch.setattr(subprocess, "run", fake_run)
    assert netdiag.current_wifi() == "yes:MHD ENET C084:75"


def test_current_wifi_reports_when_not_associated(monkeypatch):
    def fake_run(cmd, **kwargs):
        class R:
            stdout = "no:HomeNet:60\n"
        return R()
    monkeypatch.setattr(subprocess, "run", fake_run)
    assert netdiag.current_wifi() == "<not associated to any WiFi>"


def test_current_wifi_handles_subprocess_failure(monkeypatch):
    def fake_run(cmd, **kwargs):
        raise subprocess.TimeoutExpired(cmd, 5)
    monkeypatch.setattr(subprocess, "run", fake_run)
    assert "failed" in netdiag.current_wifi()


def test_ping_reports_reachable_on_zero_percent_loss(monkeypatch):
    def fake_run(cmd, **kwargs):
        class R:
            stdout = (
                "PING 192.168.4.1: 1 data bytes\n"
                "1 packets transmitted, 1 received, 0% packet loss, time 0ms\n"
            )
        return R()
    monkeypatch.setattr(subprocess, "run", fake_run)
    ok, out = netdiag.ping("192.168.4.1")
    assert ok is True
    assert "0% packet loss" in out


def test_ping_reports_unreachable_on_full_loss(monkeypatch):
    def fake_run(cmd, **kwargs):
        class R:
            stdout = "1 packets transmitted, 0 received, 100% packet loss\n"
        return R()
    monkeypatch.setattr(subprocess, "run", fake_run)
    ok, out = netdiag.ping("192.168.4.1")
    assert ok is False


def test_snapshot_combines_wifi_and_ping_into_one_line(monkeypatch):
    monkeypatch.setattr(netdiag, "current_wifi", lambda: "yes:MHD ENET C084:75")
    monkeypatch.setattr(netdiag, "ping", lambda host, count=1, timeout=2.0: (False, "100% packet loss"))
    line = netdiag.snapshot("192.168.4.1")
    assert "MHD ENET C084" in line
    assert "NO REPLY" in line
    assert "192.168.4.1" in line
