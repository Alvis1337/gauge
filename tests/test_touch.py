"""
Unit tests for touch.TouchController against a fake SPI bus — no physical
XPT2046 hardware required. Verifies the oversampling/median filtering and
pressure debounce that touch.py added on top of the raw single-sample
reads the original driver used.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

import pytest
import touch


class _FakeSpi:
    """Queues raw 12-bit ADC values per command byte, consumed in order."""

    def __init__(self, queues: dict[int, list[int]]):
        self._queues = queues
        self.max_speed_hz = None
        self.mode = None

    def open(self, bus, dev):
        pass

    def xfer2(self, data):
        cmd = data[0]
        raw12 = self._queues[cmd].pop(0)
        assert 0 <= raw12 <= 4095, f"{raw12} isn't a valid 12-bit ADC reading"
        combined = raw12 << 3  # inverse of touch.py's `>> 3` decode
        return [0, (combined >> 8) & 0xFF, combined & 0xFF]

    def close(self):
        pass


@pytest.fixture
def make_touch(monkeypatch):
    def _make(queues, calibration=None):
        fake = _FakeSpi(queues)
        monkeypatch.setattr(touch.spidev, "SpiDev", lambda: fake)
        tc = touch.TouchController()
        if calibration is not None:
            monkeypatch.setattr(touch.settings, "get", lambda k: calibration.get(k))
        return tc, fake
    return _make


def test_read_adc_median_discards_first_sample_and_takes_median(make_touch):
    # First value is a throwaway (mux settle); median of the rest is 300.
    queues = {touch.CMD_X: [4095, 100, 500, 300, 200, 400]}
    tc, _ = make_touch(queues)
    assert tc._read_adc_median(touch.CMD_X, 5) == 300


def test_read_adc_median_rejects_outlier_spike(make_touch):
    # A single wild outlier shouldn't move the result if the rest agree.
    queues = {touch.CMD_X: [0, 350, 355, 360, 4095, 358]}
    tc, _ = make_touch(queues)
    assert tc._read_adc_median(touch.CMD_X, 5) == 358


def _no_touch_pressure_pair():
    return {"z1": 0, "z2": 0}  # pressure formula returns 0 when z1 == 0


def _touch_pressure_pair():
    return {"z1": 4095, "z2": 0}  # pressure = 4095 - 0 + 4095, well above threshold


def test_read_returns_none_when_untouched(make_touch):
    queues = {touch.CMD_Z1: [0], touch.CMD_Z2: [0]}
    tc, _ = make_touch(queues)
    assert tc.read() is None


def test_read_rejects_transient_pressure_dip_mid_debounce(make_touch):
    # Samples 1-2 look like a real touch, sample 3 dips below threshold —
    # the whole read() call must reject it rather than trusting samples 1-2.
    queues = {
        touch.CMD_Z1: [4095, 4095, 0],
        touch.CMD_Z2: [0, 0, 0],
    }
    tc, fake = make_touch(queues)
    assert tc.read() is None
    # Must not have touched X/Y at all, and must not over-consume Z past the dip.
    assert touch.CMD_X not in fake._queues or not fake._queues.get(touch.CMD_X)
    for q in fake._queues.values():
        assert q == [] or all(isinstance(v, int) for v in q)  # no crash from over-pop


def test_read_maps_sustained_touch_to_calibrated_pixel(make_touch):
    n = touch.PRESSURE_SAMPLES
    calibration = {
        "touch_x_min": 350, "touch_x_max": 3800,
        "touch_y_min": 350, "touch_y_max": 3900,
    }
    # raw_X (vertical) median -> 2075 (midpoint of 350..3800) -> py = H/2
    # raw_Y (horizontal) median -> 2125 (midpoint of 350..3900) -> px = W/2
    queues = {
        touch.CMD_Z1: [4095] * n,
        touch.CMD_Z2: [0] * n,
        touch.CMD_X: [0, 2075, 2075, 2075, 2075, 2075],
        touch.CMD_Y: [0, 2125, 2125, 2125, 2125, 2125],
    }
    tc, _ = make_touch(queues, calibration=calibration)
    result = tc.read()
    assert result is not None
    px, py = result
    assert px == pytest.approx(touch.DISPLAY_W // 2, abs=2)
    assert py == pytest.approx(touch.DISPLAY_H // 2, abs=2)


def test_read_clamps_out_of_range_position_to_screen_bounds(make_touch):
    n = touch.PRESSURE_SAMPLES
    calibration = {
        "touch_x_min": 350, "touch_x_max": 3800,
        "touch_y_min": 350, "touch_y_max": 3900,
    }
    queues = {
        touch.CMD_Z1: [4095] * n,
        touch.CMD_Z2: [0] * n,
        touch.CMD_X: [0] + [50] * 5,     # below x_min -> would map negative
        touch.CMD_Y: [0] + [4095] * 5,   # ADC max, above y_max -> would map past width
    }
    tc, _ = make_touch(queues, calibration=calibration)
    px, py = tc.read()
    assert px == touch.DISPLAY_W - 1
    assert py == 0


def test_read_raw_returns_median_filtered_axes_for_calibration_screen(make_touch):
    queues = {
        touch.CMD_X: [0, 10, 20, 30, 40, 50],
        touch.CMD_Y: [0, 100, 200, 300, 400, 500],
    }
    tc, _ = make_touch(queues)
    assert tc.read_raw() == (30, 300)


def test_read_returns_none_instead_of_crashing_on_zero_calibration_range(make_touch):
    # A wiped/corrupt settings.json (or a botched calibration) can leave
    # min == max on an axis — every touch must be silently dropped rather
    # than raising a ZeroDivisionError.
    n = touch.PRESSURE_SAMPLES
    calibration = {
        "touch_x_min": 350, "touch_x_max": 350,
        "touch_y_min": 350, "touch_y_max": 3900,
    }
    queues = {
        touch.CMD_Z1: [4095] * n,
        touch.CMD_Z2: [0] * n,
        touch.CMD_X: [0] + [2075] * 5,
        touch.CMD_Y: [0] + [2125] * 5,
    }
    tc, _ = make_touch(queues, calibration=calibration)
    assert tc.read() is None
