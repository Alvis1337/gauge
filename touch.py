"""
XPT2046 resistive touch via /dev/spidev1.0 (SPI1, dedicated bus).

Raw axis mapping for this PCB:
  raw X (CMD_X=0xD0) tracks the VERTICAL axis (top→bottom)
  raw Y (CMD_Y=0x90) tracks the HORIZONTAL axis (left→right)
So we map: pygame_x = f(raw_Y), pygame_y = f(raw_X)

A single raw ADC sample from this controller is noisy enough on its own
to make taps land on the wrong on-screen button — every serious XPT2046
driver oversamples position (discarding the first read after switching
the ADC input mux, since it hasn't settled yet) and takes the median of
several more. Press detection likewise requires multiple consecutive
above-threshold pressure samples, spaced a few ms apart, before accepting
a touch as real — the same approach touch_debug.py already proved
necessary (5 consecutive samples) when it was used to hand-calibrate this
panel, but which this driver never adopted.
"""
import time
import spidev
import settings
import config

CMD_X  = 0xD0
CMD_Y  = 0x90
CMD_Z1 = 0xB0
CMD_Z2 = 0xC0

PRESSURE_THRESHOLD  = 500
PRESSURE_SAMPLES    = 4       # consecutive above-threshold reads required to accept a press
PRESSURE_SAMPLE_GAP = 0.003   # seconds between pressure samples
POSITION_SAMPLES    = 5       # medianed reads per axis (plus one discarded mux-settle read)

DISPLAY_W = config.DISPLAY_WIDTH
DISPLAY_H = config.DISPLAY_HEIGHT

# Calibration defaults measured from physical corners
_DEFAULT_X_MIN = 350   # raw_X at physical top
_DEFAULT_X_MAX = 3800  # raw_X at physical bottom
_DEFAULT_Y_MIN = 350   # raw_Y at physical left
_DEFAULT_Y_MAX = 3900  # raw_Y at physical right


class TouchController:
    def __init__(self):
        self._spi = spidev.SpiDev()
        self._spi.open(1, 0)
        self._spi.max_speed_hz = 50_000
        self._spi.mode = 0

    def _read_adc(self, cmd: int) -> int:
        rx = self._spi.xfer2([cmd, 0, 0])
        return ((rx[1] << 8) | rx[2]) >> 3

    def _read_adc_median(self, cmd: int, samples: int) -> int:
        self._read_adc(cmd)  # throwaway: mux hasn't settled right after switching channel
        vals = sorted(self._read_adc(cmd) for _ in range(samples))
        return vals[len(vals) // 2]

    def _pressure(self) -> int:
        z1 = self._read_adc(CMD_Z1)
        z2 = self._read_adc(CMD_Z2)
        return z1 - z2 + 4095 if z1 > 0 else 0

    def read_raw(self) -> tuple[int, int]:
        return (self._read_adc_median(CMD_X, POSITION_SAMPLES),
                self._read_adc_median(CMD_Y, POSITION_SAMPLES))

    def read(self) -> tuple[int, int] | None:
        for i in range(PRESSURE_SAMPLES):
            if self._pressure() < PRESSURE_THRESHOLD:
                return None
            if i < PRESSURE_SAMPLES - 1:
                time.sleep(PRESSURE_SAMPLE_GAP)

        raw_x, raw_y = self.read_raw()   # median-filtered vertical/horizontal axes

        x_min = settings.get("touch_x_min") or _DEFAULT_X_MIN
        x_max = settings.get("touch_x_max") or _DEFAULT_X_MAX
        y_min = settings.get("touch_y_min") or _DEFAULT_Y_MIN
        y_max = settings.get("touch_y_max") or _DEFAULT_Y_MAX

        x_range = x_max - x_min
        y_range = y_max - y_min
        if x_range == 0 or y_range == 0:
            return None

        # raw_Y → pygame X (horizontal),  raw_X → pygame Y (vertical)
        px = int((raw_y - y_min) / y_range * DISPLAY_W)
        py = int((raw_x - x_min) / x_range * DISPLAY_H)

        px = max(0, min(DISPLAY_W - 1, px))
        py = max(0, min(DISPLAY_H - 1, py))
        return px, py

    def close(self):
        self._spi.close()
