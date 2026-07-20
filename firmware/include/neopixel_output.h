// Drives two physically separate NeoPixel outputs:
//   - a 60-pixel RGBW shift-light bar (fills toward shiftRpm, flashes at it)
//   - a single-pixel status LED: blinking blue while obd_task is searching/
//     retrying, a one-shot 3x red blink the instant a connect attempt
//     fails, solid green once connected.
//
// Both live entirely in loop() on core 1 — no dedicated FreeRTOS task. The
// ESP32 clocks NeoPixel data out via the RMT hardware peripheral, so show()
// only costs the CPU time to load its buffer; it isn't bit-banged, so it
// doesn't fight the interrupt-watchdog constraints the rest of this project
// cares about. Call update() once per frame from loop(), after the same
// GaugeData/connected snapshot already taken for update_gauge_screen() —
// no new locking needed.
#pragma once
#include <Adafruit_NeoPixel.h>
#include <algorithm>
#include <cmath>

class NeopixelOutput {
public:
    static constexpr uint8_t  BAR_DATA_PIN    = 4;
    static constexpr uint8_t  STATUS_DATA_PIN = 16;
    static constexpr uint16_t BAR_LEN         = 60;

    void begin(float barMinRpm, float shiftRpm, uint8_t brightness) {
        applySettings(barMinRpm, shiftRpm, brightness);
        _bar.begin();
        _bar.clear();
        _bar.show();

        _status.begin();
        _status.clear();
        _status.show();
    }

    // Called from the Settings UI when the user changes shift-light tuning
    // — no reflash needed to dial in the B58's actual redline/shift point.
    // The Settings UI's stepper buttons already keep shiftRpm > barMinRpm,
    // but _updateBar() divides by (shiftRpm - barMinRpm) every frame, so
    // this guards against a bad value reaching here some other way (a
    // hand-edited NVS blob, a future caller that skips the UI) rather than
    // trusting the caller never to pass shiftRpm <= barMinRpm.
    void applySettings(float barMinRpm, float shiftRpm, uint8_t brightness) {
        _barMinRpm = barMinRpm;
        _shiftRpm  = std::max(shiftRpm, barMinRpm + 1.0f);
        _bar.setBrightness(brightness);
        _status.setBrightness(brightness);
    }

    // rpm may be NAN (no data yet / OBD disconnected) -- bar goes dark.
    // obdFailSeq is gShared.obdFailSeq -- a monotonically increasing count
    // of failed connect attempts. Comparing it against the last value seen
    // here (not a bool) means a fresh failure is never missed even if it
    // lands before the previous fail-blink sequence finished.
    void update(float rpm, bool connected, uint32_t obdFailSeq) {
        _updateBar(rpm);
        _updateStatus(connected, obdFailSeq);
    }

private:
    // BAR_LEN RGBW pixels at full white/full brightness would be ~4.8A --
    // proven safe on the bench up to that figure. Brightness is kept well
    // under 255 by default (see gauge_settings.h's ledBrightness()) for
    // continuous in-car operation rather than a one-off bench test.
    Adafruit_NeoPixel _bar{BAR_LEN, BAR_DATA_PIN, NEO_GRBW + NEO_KHZ800};
    Adafruit_NeoPixel _status{1, STATUS_DATA_PIN, NEO_GRBW + NEO_KHZ800};
    float _barMinRpm = 2000.0f;
    float _shiftRpm  = 6500.0f;

    // Status LED fail-blink state -- see _updateStatus().
    uint32_t _lastFailSeq = 0;
    bool _failBlinkActive = false;
    uint32_t _failBlinkStartMs = 0;
    static constexpr uint32_t FAIL_BLINK_HALF_MS = 150;  // 3 full on/off blinks
    static constexpr uint32_t FAIL_BLINK_HALVES  = 6;
    static constexpr uint32_t SEARCH_BLINK_HALF_MS = 300;

    static uint32_t _rpmColor(float t) {
        // t in [0,1]: green -> yellow -> red as RPM climbs toward _shiftRpm.
        uint8_t r = (uint8_t)(255 * std::min(1.0f, t * 2.0f));
        uint8_t g = (uint8_t)(255 * std::min(1.0f, (1.0f - t) * 2.0f));
        return Adafruit_NeoPixel::Color(r, g, 0, 0);
    }

    void _updateBar(float rpm) {
        if (std::isnan(rpm) || rpm < _barMinRpm) {
            _bar.clear();
            _bar.show();
            return;
        }

        if (rpm >= _shiftRpm) {
            // Flash red/white at ~5Hz using millis() parity -- no extra
            // timer, just a phase check against the free-running clock.
            uint32_t color = (millis() / 100) % 2 == 0
                ? _bar.Color(255, 255, 255, 0)
                : _bar.Color(255, 0, 0, 0);
            for (uint16_t i = 0; i < BAR_LEN; i++) _bar.setPixelColor(i, color);
            _bar.show();
            return;
        }

        float t = (rpm - _barMinRpm) / (_shiftRpm - _barMinRpm);
        uint16_t lit = (uint16_t)(t * BAR_LEN);
        for (uint16_t i = 0; i < BAR_LEN; i++) {
            _bar.setPixelColor(i, i < lit ? _rpmColor((float)i / BAR_LEN) : 0);
        }
        _bar.show();
    }

    void _updateStatus(bool connected, uint32_t obdFailSeq) {
        if (obdFailSeq != _lastFailSeq) {
            _lastFailSeq = obdFailSeq;
            _failBlinkActive = true;
            _failBlinkStartMs = millis();
        }

        if (_failBlinkActive) {
            uint32_t elapsed = millis() - _failBlinkStartMs;
            if (elapsed >= FAIL_BLINK_HALF_MS * FAIL_BLINK_HALVES) {
                _failBlinkActive = false;
            } else {
                bool on = (elapsed / FAIL_BLINK_HALF_MS) % 2 == 0;
                _status.setPixelColor(0, on ? _status.Color(255, 0, 0, 0) : 0);
                _status.show();
                return;
            }
        }

        if (connected) {
            _status.setPixelColor(0, _status.Color(0, 255, 0, 0));
        } else {
            bool on = (millis() / SEARCH_BLINK_HALF_MS) % 2 == 0;
            _status.setPixelColor(0, on ? _status.Color(0, 0, 255, 0) : 0);
        }
        _status.show();
    }
};
