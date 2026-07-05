"""
Unit tests for the log-parsing and pagination logic in screens.LogsScreen.
No hardware or real display needed — ui.py only calls pygame.font.init(),
not pygame.display, so this runs headless.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import screens


def test_parse_log_lines_extracts_time_level_and_message():
    lines = ["2026-07-02 22:45:52.486 INFO    main     hello world\n"]
    level, text = screens._parse_log_lines(lines)[0]
    assert level == "INFO"
    assert text == "22:45:52 I hello world"


def test_parse_log_lines_groups_traceback_with_parent_error_level():
    lines = [
        "2026-07-02 22:46:04.201 ERROR   main     OBD error (host=x port=y)\n",
        "Traceback (most recent call last):\n",
        '  File "/home/alvis/gauge/main.py", line 67, in _obd_thread\n',
        "TimeoutError: timed out\n",
    ]
    parsed = screens._parse_log_lines(lines)
    levels = [lvl for lvl, _ in parsed]
    assert levels == ["ERROR", "ERROR", "ERROR", "ERROR"]
    # continuation lines are indented and truncated, not left verbatim
    assert parsed[1][1].startswith("    Traceback")


def test_parse_log_lines_continuation_after_debug_stays_debug():
    lines = [
        "2026-07-02 22:46:04.201 DEBUG   obd      poll 50ms boost=1 rpm=2\n",
        "    some continuation that still belongs to the debug line\n",
    ]
    parsed = screens._parse_log_lines(lines)
    assert parsed[0][0] == "DEBUG"
    assert parsed[1][0] == "DEBUG"


def test_parse_log_lines_truncates_long_messages_to_60_chars():
    long_msg = "x" * 200
    lines = [f"2026-07-02 22:45:52.486 INFO    main     {long_msg}\n"]
    _, text = screens._parse_log_lines(lines)[0]
    assert len(text) == 60


def _make_logs_screen(monkeypatch, lines: list[str]):
    monkeypatch.setattr(screens.gauge_logging, "log_path", lambda: "/dev/null")
    monkeypatch.setattr("builtins.open", lambda *a, **k: _FakeFile(lines))
    return screens.LogsScreen()


class _FakeFile:
    def __init__(self, lines):
        self._lines = lines

    def readlines(self):
        return self._lines

    def __enter__(self):
        return self

    def __exit__(self, *a):
        pass


def test_logs_screen_default_filters_out_debug(monkeypatch):
    lines = [
        "2026-07-02 22:45:52.486 INFO    main     one\n",
        "2026-07-02 22:45:52.486 DEBUG   obd      two\n",
        "2026-07-02 22:45:52.486 WARNING obd      three\n",
    ]
    screen = _make_logs_screen(monkeypatch, lines)
    assert [lvl for lvl, _ in screen._lines] == ["INFO", "WARNING"]


def test_logs_screen_toggle_shows_debug(monkeypatch):
    lines = [
        "2026-07-02 22:45:52.486 INFO    main     one\n",
        "2026-07-02 22:45:52.486 DEBUG   obd      two\n",
    ]
    screen = _make_logs_screen(monkeypatch, lines)
    screen._show_debug = True
    screen._load()
    assert [lvl for lvl, _ in screen._lines] == ["INFO", "DEBUG"]


def test_logs_screen_pagination_shows_most_recent_lines_first(monkeypatch):
    lines = [f"2026-07-02 22:45:52.486 INFO    main     line{i}\n" for i in range(30)]
    screen = _make_logs_screen(monkeypatch, lines)
    assert screen._total_pages == 3  # 30 lines / 14 per page, rounded up

    # Page 0 (most recent / freshest) is always a full page; the partial
    # remainder naturally falls on the oldest page instead.
    page0 = [text for _, text in screen._page_lines()]
    assert len(page0) == 14
    assert page0[-1].endswith("line29")
    assert page0[0].endswith("line16")

    screen._page = 1
    page1 = [text for _, text in screen._page_lines()]
    assert len(page1) == 14
    assert page1[-1].endswith("line15")

    screen._page = 2
    page2 = [text for _, text in screen._page_lines()]
    assert len(page2) == 2   # 30 % 14 == 2, oldest/incomplete page
    assert page2[0].endswith("line0")
    assert page2[-1].endswith("line1")


def test_logs_screen_handles_missing_log_file_gracefully(monkeypatch):
    monkeypatch.setattr(screens.gauge_logging, "log_path", lambda: "/dev/null")

    def raise_not_found(*a, **k):
        raise FileNotFoundError()
    monkeypatch.setattr("builtins.open", raise_not_found)

    screen = screens.LogsScreen()
    assert screen._lines == []
    assert screen._total_pages == 1
