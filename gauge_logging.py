"""Rotating file + console logging, set up once at startup.

Each run gets its own timestamped session file under gauge/logs/ instead
of every run's output getting appended into (or size-rotated through) one
eternal file — the point is to be able to go test-drive, come back, and
hand over exactly that run's log for diagnosis without having to pick it
out of a pile of unrelated earlier sessions.
"""
import glob
import logging
import os
import time
from logging.handlers import RotatingFileHandler

_LOG_DIR = os.path.join(os.path.dirname(__file__), "logs")
# A single very long session (an all-day drive) still needs a cap so it
# can't fill the disk — rotates within its own session_* name instead of
# spilling into the next run's file.
_SESSION_MAX_BYTES = 3_000_000
_SESSION_BACKUPS = 3
# How many past sessions to keep on disk before pruning the oldest.
_MAX_SESSIONS = 20

_session_path = None


def log_path() -> str:
    """Path to the current run's session log file."""
    return _session_path


def list_sessions() -> list:
    """All session log files on disk, oldest first."""
    return sorted(glob.glob(os.path.join(_LOG_DIR, "session_*.log")))


def _prune_old_sessions():
    for old in list_sessions()[:-_MAX_SESSIONS] if _MAX_SESSIONS > 0 else []:
        # Also remove that session's own .1/.2/.3 rotation backups, not
        # just its primary file.
        for path in glob.glob(old + "*"):
            try:
                os.remove(path)
            except OSError:
                pass


def setup() -> str:
    global _session_path
    os.makedirs(_LOG_DIR, exist_ok=True)
    _prune_old_sessions()

    session_id = time.strftime("%Y%m%d_%H%M%S")
    _session_path = os.path.join(_LOG_DIR, f"session_{session_id}.log")

    fmt = logging.Formatter(
        "%(asctime)s.%(msecs)03d %(levelname)-7s %(name)-8s %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    root = logging.getLogger()
    root.setLevel(logging.DEBUG)

    fh = RotatingFileHandler(_session_path, maxBytes=_SESSION_MAX_BYTES, backupCount=_SESSION_BACKUPS)
    fh.setLevel(logging.DEBUG)
    fh.setFormatter(fmt)
    root.addHandler(fh)

    ch = logging.StreamHandler()
    ch.setLevel(logging.INFO)
    ch.setFormatter(fmt)
    root.addHandler(ch)

    return _session_path
