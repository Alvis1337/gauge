"""
Unit tests for obd_discovery.py — all socket/subprocess calls are mocked
so this runs fast and deterministically without any real network access.
"""
import os
import socket
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import pytest
import obd_discovery


def test_default_gateway_parses_ip_route_output(monkeypatch):
    def fake_check_output(cmd, **kwargs):
        return "default via 192.168.4.1 dev wlan0 proto dhcp src 192.168.4.23 metric 600\n"
    monkeypatch.setattr(subprocess, "check_output", fake_check_output)
    assert obd_discovery._default_gateway() == "192.168.4.1"


def test_default_gateway_returns_none_when_no_default_route(monkeypatch):
    def fake_check_output(cmd, **kwargs):
        raise subprocess.CalledProcessError(1, cmd)
    monkeypatch.setattr(subprocess, "check_output", fake_check_output)
    assert obd_discovery._default_gateway() is None


def test_default_gateway_returns_none_on_timeout(monkeypatch):
    def fake_check_output(cmd, **kwargs):
        raise subprocess.TimeoutExpired(cmd, 3)
    monkeypatch.setattr(subprocess, "check_output", fake_check_output)
    assert obd_discovery._default_gateway() is None


class _FakeSocket:
    def __init__(self, response: bytes):
        self._response = response
        self._sent = None

    def settimeout(self, t):
        pass

    def sendall(self, data):
        self._sent = data

    def recv(self, n):
        chunk, self._response = self._response, b""
        return chunk

    def __enter__(self):
        return self

    def __exit__(self, *a):
        pass


def test_looks_like_elm327_true_on_real_handshake(monkeypatch):
    monkeypatch.setattr(
        socket, "create_connection",
        lambda addr, timeout: _FakeSocket(b"ELM327 v1.5\r\r>"),
    )
    assert obd_discovery._looks_like_elm327("1.2.3.4", 35000) is True


def test_looks_like_elm327_false_on_connection_refused(monkeypatch):
    def raise_refused(addr, timeout):
        raise ConnectionRefusedError()
    monkeypatch.setattr(socket, "create_connection", raise_refused)
    assert obd_discovery._looks_like_elm327("1.2.3.4", 35000) is False


def test_looks_like_elm327_false_on_garbage_response(monkeypatch):
    # Something else entirely is listening on this port/host.
    monkeypatch.setattr(
        socket, "create_connection",
        lambda addr, timeout: _FakeSocket(b"HTTP/1.1 400 Bad Request\r\n"),
    )
    assert obd_discovery._looks_like_elm327("1.2.3.4", 80) is False


def test_probe_host_returns_first_matching_port(monkeypatch):
    def fake_looks_like(host, port, timeout=1.0):
        return port == 6801
    monkeypatch.setattr(obd_discovery, "_looks_like_elm327", fake_looks_like)
    assert obd_discovery._probe_host("1.2.3.4", [35000, 23, 6801], 1.0) == 6801


def test_probe_host_returns_none_when_nothing_matches(monkeypatch):
    monkeypatch.setattr(obd_discovery, "_looks_like_elm327", lambda h, p, timeout=1.0: False)
    assert obd_discovery._probe_host("1.2.3.4", [35000, 23], 1.0) is None


def test_discover_prefers_gateway_over_known_hosts(monkeypatch):
    monkeypatch.setattr(obd_discovery, "_default_gateway", lambda: "10.0.0.1")

    def fake_probe_host(host, ports, timeout):
        return 6801 if host == "10.0.0.1" else None
    monkeypatch.setattr(obd_discovery, "_probe_host", fake_probe_host)

    assert obd_discovery.discover() == ("10.0.0.1", 6801)


def test_discover_falls_back_to_known_hosts_when_gateway_misses(monkeypatch):
    monkeypatch.setattr(obd_discovery, "_default_gateway", lambda: "10.0.0.1")

    def fake_probe_host(host, ports, timeout):
        return 35000 if host == "192.168.4.1" else None
    monkeypatch.setattr(obd_discovery, "_probe_host", fake_probe_host)

    assert obd_discovery.discover() == ("192.168.4.1", 35000)


def test_discover_returns_none_when_nothing_found(monkeypatch):
    monkeypatch.setattr(obd_discovery, "_default_gateway", lambda: None)
    monkeypatch.setattr(obd_discovery, "_probe_host", lambda host, ports, timeout: None)
    assert obd_discovery.discover() is None


def test_discover_does_not_probe_gateway_twice_if_also_a_known_host(monkeypatch):
    # If the gateway happens to equal one of KNOWN_HOSTS, it shouldn't be probed twice.
    monkeypatch.setattr(obd_discovery, "_default_gateway", lambda: obd_discovery.KNOWN_HOSTS[0])
    calls = []

    def fake_probe_host(host, ports, timeout):
        calls.append(host)
        return None
    monkeypatch.setattr(obd_discovery, "_probe_host", fake_probe_host)

    obd_discovery.discover()
    assert calls.count(obd_discovery.KNOWN_HOSTS[0]) == 1
