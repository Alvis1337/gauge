"""Rotating file + console logging, set up once at startup.

Writes to gauge/logs/gauge.log so a crash or bad OBD session in the car
can be reviewed later even though nobody was watching a terminal.
"""
import logging
import os
from logging.handlers import RotatingFileHandler

_LOG_DIR = os.path.join(os.path.dirname(__file__), "logs")
_LOG_PATH = os.path.join(_LOG_DIR, "gauge.log")


def setup() -> str:
    os.makedirs(_LOG_DIR, exist_ok=True)

    fmt = logging.Formatter(
        "%(asctime)s.%(msecs)03d %(levelname)-7s %(name)-8s %(message)s",
        datefmt="%Y-%m-%d %H:%M:%S",
    )

    root = logging.getLogger()
    root.setLevel(logging.DEBUG)

    fh = RotatingFileHandler(_LOG_PATH, maxBytes=2_000_000, backupCount=5)
    fh.setLevel(logging.DEBUG)
    fh.setFormatter(fmt)
    root.addHandler(fh)

    ch = logging.StreamHandler()
    ch.setLevel(logging.INFO)
    ch.setFormatter(fmt)
    root.addHandler(ch)

    return _LOG_PATH
