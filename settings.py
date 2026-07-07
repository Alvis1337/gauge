"""Persistent settings stored in ~/gauge/settings.json."""
import json
import os

_PATH = os.path.join(os.path.dirname(__file__), "settings.json")

_DEFAULTS = {
    "obd_bt_address": "",
    "obd_bt_name": "",
    # touch calibration
    "touch_x_min": 200,
    "touch_x_max": 3900,
    "touch_y_min": 200,
    "touch_y_max": 3900,
}

_data: dict = {}


def load():
    global _data
    try:
        with open(_PATH) as f:
            _data = {**_DEFAULTS, **json.load(f)}
    except Exception:
        _data = dict(_DEFAULTS)


def save():
    with open(_PATH, "w") as f:
        json.dump(_data, f, indent=2)


def get(key):
    return _data.get(key, _DEFAULTS.get(key))


def set(key, value):
    _data[key] = value
    save()
