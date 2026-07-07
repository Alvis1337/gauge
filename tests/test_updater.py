"""Unit tests for updater.py against real throwaway git repos (fetch/pull
plumbing is exactly the thing being tested, so faking subprocess would just
re-assert the mock instead of proving the git invocations are correct)."""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import updater


def _git(cwd, *args):
    subprocess.run(["git", *args], cwd=cwd, check=True, capture_output=True, text=True)


def _make_origin_and_clone(tmp_path):
    origin = tmp_path / "origin"
    origin.mkdir()
    _git(origin, "init", "-b", "main")
    (origin / "file.txt").write_text("v1\n")
    _git(origin, "add", "file.txt")
    _git(origin, "commit", "-m", "initial")

    clone = tmp_path / "clone"
    _git(tmp_path, "clone", str(origin), str(clone))
    return origin, clone


def test_up_to_date_when_no_new_commits(tmp_path, monkeypatch):
    _, clone = _make_origin_and_clone(tmp_path)
    monkeypatch.setattr(updater, "_REPO_DIR", str(clone))

    status = [None]
    applied = updater.check_and_apply(status)

    assert applied is False
    assert status[0] == "up_to_date"


def test_pulls_and_reports_restarting_when_origin_has_new_commit(tmp_path, monkeypatch):
    origin, clone = _make_origin_and_clone(tmp_path)
    (origin / "file.txt").write_text("v2\n")
    _git(origin, "add", "file.txt")
    _git(origin, "commit", "-m", "second")

    monkeypatch.setattr(updater, "_REPO_DIR", str(clone))

    status = [None]
    applied = updater.check_and_apply(status)

    assert applied is True
    assert status[0] == "restarting"
    assert (clone / "file.txt").read_text() == "v2\n"


def test_error_status_when_fetch_fails(tmp_path, monkeypatch):
    _, clone = _make_origin_and_clone(tmp_path)
    _git(clone, "remote", "set-url", "origin", "/nonexistent/path/to/nowhere")
    monkeypatch.setattr(updater, "_REPO_DIR", str(clone))

    status = [None]
    applied = updater.check_and_apply(status)

    assert applied is False
    assert status[0] == "error"
