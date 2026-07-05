"""
Unit tests for gauge_logging.py's session-file naming and retention.
Uses a scratch directory instead of the real gauge/logs/ so it doesn't
touch or prune actual session logs.
"""
import glob
import logging
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import gauge_logging


def _reset_root_handlers():
    root = logging.getLogger()
    for h in list(root.handlers):
        root.removeHandler(h)
        h.close()


def test_setup_creates_a_session_named_log_file(tmp_path, monkeypatch):
    monkeypatch.setattr(gauge_logging, "_LOG_DIR", str(tmp_path))
    monkeypatch.setattr(time, "strftime", lambda fmt: "20260705_120000")
    try:
        path = gauge_logging.setup()
        assert path == os.path.join(str(tmp_path), "session_20260705_120000.log")
        assert gauge_logging.log_path() == path
        assert os.path.exists(path)
    finally:
        _reset_root_handlers()


def test_each_setup_call_gets_a_distinct_session_file(tmp_path, monkeypatch):
    monkeypatch.setattr(gauge_logging, "_LOG_DIR", str(tmp_path))
    timestamps = iter(["20260705_120000", "20260705_120500"])
    monkeypatch.setattr(time, "strftime", lambda fmt: next(timestamps))
    try:
        first = gauge_logging.setup()
        _reset_root_handlers()
        second = gauge_logging.setup()
        assert first != second
        # Both sessions' files still exist — a restart doesn't clobber the
        # previous run's log.
        assert os.path.exists(first)
        assert os.path.exists(second)
    finally:
        _reset_root_handlers()


def test_prune_keeps_only_the_most_recent_max_sessions(tmp_path, monkeypatch):
    monkeypatch.setattr(gauge_logging, "_LOG_DIR", str(tmp_path))
    monkeypatch.setattr(gauge_logging, "_MAX_SESSIONS", 3)

    for i in range(5):
        (tmp_path / f"session_2026070{i}_120000.log").write_text("x")

    gauge_logging._prune_old_sessions()
    remaining = sorted(os.path.basename(p) for p in glob.glob(str(tmp_path / "session_*.log")))
    assert remaining == [
        "session_20260702_120000.log",
        "session_20260703_120000.log",
        "session_20260704_120000.log",
    ]


def test_prune_also_removes_a_pruned_sessions_rotation_backups(tmp_path, monkeypatch):
    monkeypatch.setattr(gauge_logging, "_LOG_DIR", str(tmp_path))
    monkeypatch.setattr(gauge_logging, "_MAX_SESSIONS", 1)

    (tmp_path / "session_20260701_120000.log").write_text("x")
    (tmp_path / "session_20260701_120000.log.1").write_text("x")
    (tmp_path / "session_20260702_120000.log").write_text("x")

    gauge_logging._prune_old_sessions()
    remaining = sorted(os.path.basename(p) for p in glob.glob(str(tmp_path / "session_*")))
    assert remaining == ["session_20260702_120000.log"]


def test_list_sessions_returns_oldest_first(tmp_path, monkeypatch):
    monkeypatch.setattr(gauge_logging, "_LOG_DIR", str(tmp_path))
    (tmp_path / "session_20260703_120000.log").write_text("x")
    (tmp_path / "session_20260701_120000.log").write_text("x")
    (tmp_path / "session_20260702_120000.log").write_text("x")

    names = [os.path.basename(p) for p in gauge_logging.list_sessions()]
    assert names == [
        "session_20260701_120000.log",
        "session_20260702_120000.log",
        "session_20260703_120000.log",
    ]
