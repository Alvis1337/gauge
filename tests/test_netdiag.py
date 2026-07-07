"""
Unit tests for netdiag.py — all subprocess/BLE scan calls are mocked so
this runs fast and deterministically without touching real Bluetooth
hardware.
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import netdiag


def test_adapter_powered_returns_the_powered_line(monkeypatch):
    def fake_run(cmd, **kwargs):
        class R:
            stdout = "Controller AA:BB:CC:11:22:33 (public)\n\tPowered: yes\n\tDiscoverable: no\n"
        return R()
    monkeypatch.setattr(subprocess, "run", fake_run)
    assert netdiag.adapter_powered() == "Powered: yes"


def test_adapter_powered_handles_subprocess_failure(monkeypatch):
    def fake_run(cmd, **kwargs):
        raise subprocess.TimeoutExpired(cmd, 5)
    monkeypatch.setattr(subprocess, "run", fake_run)
    assert "failed" in netdiag.adapter_powered()


def test_adapter_powered_handles_unrecognized_output(monkeypatch):
    def fake_run(cmd, **kwargs):
        class R:
            stdout = "No default controller available\n"
        return R()
    monkeypatch.setattr(subprocess, "run", fake_run)
    assert "unknown" in netdiag.adapter_powered()


def test_device_visible_true_when_scan_finds_it(monkeypatch):
    def fake_run_coro(coro, timeout):
        coro.close()
        return object()
    monkeypatch.setattr(netdiag, "run_coro", fake_run_coro)
    assert netdiag.device_visible("AA:BB:CC:11:22:33") is True


def test_device_visible_false_when_scan_finds_nothing(monkeypatch):
    def fake_run_coro(coro, timeout):
        coro.close()
        return None
    monkeypatch.setattr(netdiag, "run_coro", fake_run_coro)
    assert netdiag.device_visible("AA:BB:CC:11:22:33") is False


def test_device_visible_false_on_scan_error(monkeypatch):
    def raise_error(coro, timeout):
        coro.close()
        raise RuntimeError("adapter not powered")
    monkeypatch.setattr(netdiag, "run_coro", raise_error)
    assert netdiag.device_visible("AA:BB:CC:11:22:33") is False


def test_snapshot_combines_power_state_and_visibility(monkeypatch):
    monkeypatch.setattr(netdiag, "adapter_powered", lambda: "Powered: yes")
    monkeypatch.setattr(netdiag, "device_visible", lambda address, timeout=5.0: False)
    line = netdiag.snapshot("AA:BB:CC:11:22:33")
    assert "Powered: yes" in line
    assert "NOT SEEN" in line
    assert "AA:BB:CC:11:22:33" in line
