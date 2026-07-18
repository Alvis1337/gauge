// NeoPixel bring-up — prove the data path and full-strip power draw. No
// LVGL, no OBD. Flash with: pio run -e neopixel -t upload
//
// This strip's connector breaks out 4 wires: red/black already carry power
// from the buck converter (untouched here). The data tap uses the other two:
//   white = DATA -> GPIO4, ideally through a 74AHCT125 level shifter and a
//                   330-470ohm series resistor at the first pixel.
//   black = GND  -> ESP32 GND. Must be common with the ESP32 regardless of
//                   whether it's tied to the buck's ground — no shared
//                   reference, no valid data signal.
//
// TEST_BRIGHTNESS is kept well under 255: all 60 RGBW pixels at full white
// and full brightness is ~4.8A. At brightness 60/255 that worst case drops
// to roughly 1.1A, so a modest buck converter isn't stressed during the
// all-on steps below.

#include <Arduino.h>
#include <Adafruit_NeoPixel.h>

static constexpr uint8_t  DATA_PIN        = 4;
static constexpr uint16_t NUM_LEDS        = 60;
static constexpr uint8_t  TEST_BRIGHTNESS = 60;    // 0-255, deliberately capped

// NEO_GRBW + 800KHz: RGBW SK6812 strips are GRBW order. If colors look wrong
// (e.g. red shows as green) this constant is the first thing to change.
static Adafruit_NeoPixel strip(NUM_LEDS, DATA_PIN, NEO_GRBW + NEO_KHZ800);

// Classic Adafruit color wheel, extended with W=0 (RGBW pixels only use the
// W channel when explicitly requested — the rainbow stays on RGB).
static uint32_t wheel(uint8_t pos) {
  pos = 255 - pos;
  if (pos < 85)  return strip.Color(255 - pos * 3, 0, pos * 3, 0);
  if (pos < 170) { pos -= 85; return strip.Color(0, pos * 3, 255 - pos * 3, 0); }
  pos -= 170;
  return strip.Color(pos * 3, 255 - pos * 3, 0, 0);
}

static void colorWipe(uint32_t color, uint16_t waitMs) {
  for (uint16_t i = 0; i < NUM_LEDS; i++) {
    strip.setPixelColor(i, color);
    strip.show();
    delay(waitMs);
  }
}

static void allOn(uint32_t color, uint16_t holdMs) {
  for (uint16_t i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, color);
  strip.show();
  delay(holdMs);
}

static void rainbowCycle(uint16_t frames, uint16_t waitMs) {
  for (uint16_t j = 0; j < frames; j++) {
    for (uint16_t i = 0; i < NUM_LEDS; i++) {
      strip.setPixelColor(i, wheel(((i * 256 / NUM_LEDS) + j) & 255));
    }
    strip.show();
    delay(waitMs);
  }
}

// Ramps all 60 pixels to full white in steps, dwelling at each so a weak
// buck converter or thin wiring shows trouble (flicker, dimming, color
// corruption, or the ESP32 itself brown-out-resetting -- watch for the boot
// banner reprinting on Serial, which means it reset) before going further.
// Runs once at boot, not repeated, since this is meant to be watched closely.
static void whiteRampStressTest() {
  const uint8_t steps[] = {60, 100, 140, 180, 220, 255};
  for (uint8_t s : steps) {
    strip.setBrightness(s);
    for (uint16_t i = 0; i < NUM_LEDS; i++) strip.setPixelColor(i, strip.Color(0, 0, 0, 255));
    strip.show();

    // ~80mA per RGBW pixel at full white, scaled by brightness -- estimate
    // only, not a measurement. Confirm with a multimeter on the buck's
    // output if you have one.
    uint32_t estMilliamps = (uint32_t)NUM_LEDS * 80 * s / 255;
    Serial.printf("white ramp: brightness %3u/255, est. %lu mA -- watching for 3s\n",
                  s, (unsigned long)estMilliamps);
    delay(3000);
  }
  strip.setBrightness(TEST_BRIGHTNESS);
  strip.clear();
  strip.show();
  delay(500);
}

void setup() {
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("NeoPixel light show: GPIO4, 60 pixels, capped brightness.");
  Serial.println("Step 1: white brightness ramp (power stress test).");
  Serial.println("Step 2 (repeats): wipe R/G/B/W -> all-on R/G/B/W -> rainbow cycle.");

  strip.begin();
  strip.setBrightness(TEST_BRIGHTNESS);
  strip.clear();
  strip.show();

  whiteRampStressTest();
}

void loop() {
  const uint32_t red   = strip.Color(255, 0, 0, 0);
  const uint32_t green = strip.Color(0, 255, 0, 0);
  const uint32_t blue  = strip.Color(0, 0, 255, 0);
  const uint32_t white = strip.Color(0, 0, 0, 255);

  // Per-pixel wipe: proves every single pixel along the strip lights and
  // passes data through to the next one.
  colorWipe(red, 15);
  colorWipe(green, 15);
  colorWipe(blue, 15);
  colorWipe(white, 15);
  delay(300);

  // All 60 simultaneously: proves the power feed can drive a full-strip
  // draw without browning out or corrupting data.
  allOn(red, 800);
  allOn(green, 800);
  allOn(blue, 800);
  allOn(white, 800);
  strip.clear();
  strip.show();
  delay(300);

  // Smooth spectrum sweep across the whole strip.
  rainbowCycle(3 * 256, 4);

  strip.clear();
  strip.show();
  delay(500);
}
