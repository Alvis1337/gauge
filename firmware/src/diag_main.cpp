// Hardware bring-up diagnostic — isolates the display/wiring/touch from
// LVGL and the app's own logic entirely, so a wiring mistake or a color-
// format bug can be told apart from an LVGL/app-level bug. Build/flash
// with: pio run -e diag -t upload && pio device monitor
#include <Arduino.h>
#include <SPI.h>
#include "st7796_driver.h"
#include "xpt2046_driver.h"

static constexpr int PIN_SCLK    = 18;
static constexpr int PIN_MOSI    = 23;
static constexpr int PIN_MISO    = 19;
static constexpr int PIN_LCD_CS  = 5;
static constexpr int PIN_LCD_DC  = 26;
static constexpr int PIN_LCD_RST = 25;
static constexpr int PIN_TOUCH_CS = 27;

SPIClass gSpi(VSPI);
ST7796Driver gDisplay(PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST, gSpi);
XPT2046Driver gTouch(PIN_TOUCH_CS, gSpi);

// Fills the whole panel with one solid RGB565 color, already in the
// big-endian wire order the panel expects (no LVGL/byte-swap involved —
// this isolates the ST7796 driver + wiring from everything else).
static void fillScreen(uint16_t color_be) {
    static uint16_t line[480];
    for (int i = 0; i < 480; i++) line[i] = color_be;
    for (int y = 0; y < 320; y++) {
        gDisplay.blit(0, y, 479, y, line, 480);
    }
}

// RGB565, MSB-first (big-endian) as the panel expects on the wire.
static uint16_t rgb565be(uint8_t r, uint8_t g, uint8_t b) {
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (v >> 8) | (v << 8);
}

void setup() {
    Serial.begin(115200);
    delay(500);
    Serial.println("=== AutoGauge hardware diagnostic ===");

    gSpi.begin(PIN_SCLK, PIN_MISO, PIN_MOSI, -1);
    gDisplay.begin();
    gTouch.begin();
    Serial.println("display + touch init done");
}

void loop() {
    static const struct { const char *name; uint16_t color; } kColors[] = {
        {"RED",   rgb565be(255, 0, 0)},
        {"GREEN", rgb565be(0, 255, 0)},
        {"BLUE",  rgb565be(0, 0, 255)},
        {"WHITE", rgb565be(255, 255, 255)},
        {"BLACK", rgb565be(0, 0, 0)},
    };
    for (auto &c : kColors) {
        Serial.printf("filling screen: %s (0x%04X) -- what do you actually see?\n", c.name, c.color);
        fillScreen(c.color);

        // Print raw touch ADC for 3 seconds after each fill so tapping
        // during that window shows whether touch wiring reaches the MCU
        // at all, independent of calibration/pressure-threshold logic.
        uint32_t until = millis() + 3000;
        while (millis() < until) {
            int raw_x, raw_y;
            gTouch.readRaw(&raw_x, &raw_y);
            Serial.printf("  raw touch: x=%d y=%d\n", raw_x, raw_y);
            delay(300);
        }
    }
}
