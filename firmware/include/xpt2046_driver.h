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
//   - press detection uses a single pressure sample per LVGL callback
//     invocation; LVGL's indev state machine handles continuity across
//     frames (blocking inside the callback stalls lv_timer_handler and
//     causes short taps to be dropped entirely).
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

    static constexpr int PRESSURE_THRESHOLD  = 200;
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
    // Single pressure sample only — this runs inside LVGL's indev callback,
    // which must return immediately. LVGL's own state machine handles
    // continuity (it re-calls us each frame); blocking here for multiple
    // samples would stall lv_timer_handler and drop short taps entirely.
    bool read(int display_w, int display_h, int *px, int *py) {
        if (_pressure() < PRESSURE_THRESHOLD) return false;

        int raw_x = _readAdcMedian(CMD_X, POSITION_SAMPLES);
        int raw_y = _readAdcMedian(CMD_Y, POSITION_SAMPLES);

        int x_range = _x_max - _x_min;
        int y_range = _y_max - _y_min;
        if (x_range == 0 || y_range == 0) return false;

        // raw_Y -> screen X (horizontal), raw_X -> screen Y (vertical);
        // both axes are physically inverted on this panel so we mirror them.
        int x = display_w  - 1 - (int)((raw_y - _y_min) / (float)y_range * display_w);
        int y = display_h - 1 - (int)((raw_x - _x_min) / (float)x_range * display_h);

        // Hand-rolled clamp — std::clamp needs C++17, which this
        // toolchain's default standard doesn't enable.
        *px = x < 0 ? 0 : (x > display_w - 1 ? display_w - 1 : x);
        *py = y < 0 ? 0 : (y > display_h - 1 ? display_h - 1 : y);

        _lastRawX = raw_x;
        _lastRawY = raw_y;
        return true;
    }

    // Raw (unscaled) ADC pair, used only by the touch calibration screen.
    void readRaw(int *raw_x, int *raw_y) {
        *raw_x = _readAdcMedian(CMD_X, POSITION_SAMPLES);
        *raw_y = _readAdcMedian(CMD_Y, POSITION_SAMPLES);
    }

    // Last raw position recorded during a confirmed press — safer for
    // calibration taps than a fresh read at finger-up (LV_EVENT_CLICKED),
    // which often catches the ADC mid-release and returns garbage.
    void lastRaw(int *raw_x, int *raw_y) const {
        *raw_x = _lastRawX;
        *raw_y = _lastRawY;
    }

    // All four ADC channels for serial diagnostics.
    void readDiag(int *raw_x, int *raw_y, int *z1, int *z2) {
        *raw_x = _readAdcMedian(CMD_X, POSITION_SAMPLES);
        *raw_y = _readAdcMedian(CMD_Y, POSITION_SAMPLES);
        *z1    = _readAdc(CMD_Z1);
        *z2    = _readAdc(CMD_Z2);
    }

private:
    int _cs;
    SPIClass &_spi;
    int _x_min = 350, _x_max = 3800, _y_min = 350, _y_max = 3900;
    int _lastRawX = 0, _lastRawY = 0;

    int _readAdc(uint8_t cmd) {
        _spi.beginTransaction(SPISettings(1000000, MSBFIRST, SPI_MODE3));
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
