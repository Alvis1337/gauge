// XPT2046 resistive touch driver — shares the display's VSPI bus via its
// own CS line. Ported directly from touch.py's proven calibration/noise
// handling rather than a generic library, since this exact panel needed
// specific quirks worked out by hand:
//   - raw X (CMD_X) tracks the panel's VERTICAL axis, raw Y (CMD_Y) tracks
//     the HORIZONTAL axis — swapped from what you'd assume.
//   - a single raw ADC sample is noisy enough to land taps on the wrong
//     button; every read is median-filtered over several samples, and the
//     first sample after switching the ADC mux is discarded (hasn't
//     settled yet).
//   - press detection needs several consecutive above-threshold pressure
//     reads, not just one, to reject noise as a false touch.
// No T_IRQ pin is wired — pressure is polled every frame instead, exactly
// like the Pi build, which avoids needing an interrupt-capable GPIO.
#pragma once
#include <Arduino.h>
#include <SPI.h>
#include <algorithm>

class XPT2046Driver {
public:
    static constexpr uint8_t CMD_X  = 0xD0;
    static constexpr uint8_t CMD_Y  = 0x90;
    static constexpr uint8_t CMD_Z1 = 0xB0;
    static constexpr uint8_t CMD_Z2 = 0xC0;

    static constexpr int PRESSURE_THRESHOLD  = 500;
    static constexpr int PRESSURE_SAMPLES    = 4;
    static constexpr int PRESSURE_SAMPLE_GAP_MS = 3;
    static constexpr int POSITION_SAMPLES     = 5;

    XPT2046Driver(int pin_cs, SPIClass &spi) : _cs(pin_cs), _spi(spi) {}

    void begin() {
        pinMode(_cs, OUTPUT);
        digitalWrite(_cs, HIGH);
    }

    void setCalibration(int x_min, int x_max, int y_min, int y_max) {
        _x_min = x_min; _x_max = x_max;
        _y_min = y_min; _y_max = y_max;
    }

    // Returns true and fills (px, py) in panel pixel coordinates if a real
    // press is detected right now; false otherwise (no touch / bounced noise).
    bool read(int display_w, int display_h, int *px, int *py) {
        for (int i = 0; i < PRESSURE_SAMPLES; i++) {
            if (_pressure() < PRESSURE_THRESHOLD) return false;
            if (i < PRESSURE_SAMPLES - 1) delay(PRESSURE_SAMPLE_GAP_MS);
        }

        int raw_x = _readAdcMedian(CMD_X, POSITION_SAMPLES);
        int raw_y = _readAdcMedian(CMD_Y, POSITION_SAMPLES);

        int x_range = _x_max - _x_min;
        int y_range = _y_max - _y_min;
        if (x_range == 0 || y_range == 0) return false;

        // raw_Y -> screen X (horizontal), raw_X -> screen Y (vertical)
        int x = (int)((raw_y - _y_min) / (float)y_range * display_w);
        int y = (int)((raw_x - _x_min) / (float)x_range * display_h);

        // Hand-rolled clamp — std::clamp needs C++17, which this
        // toolchain's default standard doesn't enable.
        *px = x < 0 ? 0 : (x > display_w - 1 ? display_w - 1 : x);
        *py = y < 0 ? 0 : (y > display_h - 1 ? display_h - 1 : y);
        return true;
    }

    // Raw (unscaled) ADC pair, used only by the touch calibration screen.
    void readRaw(int *raw_x, int *raw_y) {
        *raw_x = _readAdcMedian(CMD_X, POSITION_SAMPLES);
        *raw_y = _readAdcMedian(CMD_Y, POSITION_SAMPLES);
    }

private:
    int _cs;
    SPIClass &_spi;
    int _x_min = 350, _x_max = 3800, _y_min = 350, _y_max = 3900;

    int _readAdc(uint8_t cmd) {
        _spi.beginTransaction(SPISettings(2000000, MSBFIRST, SPI_MODE0));
        digitalWrite(_cs, LOW);
        _spi.transfer(cmd);
        uint8_t hi = _spi.transfer(0);
        uint8_t lo = _spi.transfer(0);
        digitalWrite(_cs, HIGH);
        _spi.endTransaction();
        return ((hi << 8) | lo) >> 3;
    }

    int _readAdcMedian(uint8_t cmd, int samples) {
        _readAdc(cmd);  // throwaway: mux hasn't settled right after switching channel
        int vals[8];
        for (int i = 0; i < samples; i++) vals[i] = _readAdc(cmd);
        std::sort(vals, vals + samples);
        return vals[samples / 2];
    }

    int _pressure() {
        int z1 = _readAdc(CMD_Z1);
        int z2 = _readAdc(CMD_Z2);
        return z1 > 0 ? (z1 - z2 + 4095) : 0;
    }
};
